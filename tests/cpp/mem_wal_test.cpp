// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include "lance/lance.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            std::fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__,         \
                         __LINE__, #condition);                                  \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string uri = argv[1];

    auto dataset = lance::Dataset::open(uri);

    // Initialization and index details through the C++ owners.
    lance::MemWalInitOptions options;
    options.mode = LANCE_MEM_WAL_SHARDING_UNSHARDED;
    options.writer_config = lance::ShardWriterConfig{};
    dataset.initialize_mem_wal(options);

    auto details = dataset.mem_wal_index_details();
    CHECK(details.has_value());
    CHECK(details->num_shards() == 1);
    CHECK(details->maintain_all_indexes());
    CHECK(details->sharding_specs().size() == 1);
    CHECK(details->sharding_specs()[0].id() == 1);
    CHECK(details->sharding_specs()[0].field_count() == 1);
    CHECK(details->sharding_specs()[0].field_at(0).id() == "bucket");
    CHECK(details->sharding_specs()[0].field_at(0).transform() ==
          std::optional<std::string>("unsharded"));
    CHECK(!details->writer_config_defaults().empty());

    // Writer mutation, live scanner visibility, stats, and idempotent close.
    lance::ShardWriterConfig config;
    config.durable_write = true;
    config.max_wal_flush_interval_ms = 1;
    auto writer = dataset.mem_wal_writer(
        "00000000-0000-0000-0000-000000000000", config);
    CHECK(writer.shard_id() == "00000000-0000-0000-0000-000000000000");

    auto source = dataset.scan();
    ArrowArrayStream stream{};
    source.to_arrow_stream(&stream);
    auto write = writer.put(&stream);
    CHECK(write.batch_position_end > write.batch_position_begin);

    auto stats = writer.stats();
    CHECK(stats.put_count == 1);
    auto memory = writer.memtable_stats();
    CHECK(memory.is_live);
    CHECK(memory.row_count >= 3);

    auto scanner = writer.lsm_scanner({});
    CHECK(scanner.count_rows() >= 3);
    scanner.project({"id", "name"});
    scanner.filter("id >= 0");
    scanner.limit(10);
    auto table = scanner.to_table();
    CHECK(!table.batches.empty());
    CHECK(static_cast<bool>(table.schema));

    auto snapshot = writer.snapshot();
    if (snapshot.has_value()) {
        CHECK(snapshot->shard_id() ==
              "00000000-0000-0000-0000-000000000000");
    }
    writer.close();
    writer.close();
    CHECK(writer.stats().put_count == 1);

    // Persist a second generation and read the resulting native snapshot.
    auto next_writer = dataset.mem_wal_writer(
        "00000000-0000-0000-0000-000000000000", config);
    auto next_source = dataset.scan();
    ArrowArrayStream next_stream{};
    next_source.to_arrow_stream(&next_stream);
    next_writer.put(&next_stream);
    next_writer.close();
    auto snapshot_writer = dataset.mem_wal_writer(
        "00000000-0000-0000-0000-000000000000", config);
    auto flushed_snapshot = snapshot_writer.snapshot();
    CHECK(flushed_snapshot.has_value());
    const auto flushed_tables = flushed_snapshot->sstables();
    CHECK(!flushed_tables.empty());
    uint64_t compacted_generation = 0;
    for (const auto& sstable : flushed_tables) {
        CHECK(!sstable.path.empty());
        if (sstable.generation > compacted_generation) {
            compacted_generation = sstable.generation;
        }
    }
    CHECK(compacted_generation > 0);
    const auto compacted_shard = flushed_snapshot->shard_id();
    snapshot_writer.close();

    // Planner execution keeps native dataset/snapshot owners alive.
    lance::LsmPointLookupPlanner point_planner(dataset, {}, std::nullopt);
    auto key_source = lance::LsmScanner::from_snapshots(dataset, {});
    key_source.project({"id"}).filter("id = 1").limit(1);
    auto key = key_source.to_batch();
    auto lookup = point_planner.plan_lookup(key, std::vector<std::string>{"name"});
    CHECK(!lookup.explain().empty());
    CHECK(static_cast<bool>(lookup.schema()));
    CHECK(static_cast<bool>(lookup.dataset_schema()));
    CHECK(lookup.to_table().batches.size() == 1);

    lance::LsmVectorSearchPlanner vector_planner(
        dataset, {}, "vector", std::nullopt, lance::MemWalDistanceType::L2,
        std::nullopt);
    auto search = vector_planner.plan_search({0.1f, 0.2f, 0.3f, 0.4f}, 3, 20);
    CHECK(!search.explain().empty());
    auto reader = search.to_reader();
    (void)reader;

    // Atomic merge-insert with both halves of the compacted SSTable value.
    LanceMergeInsertParams merge_params{};
    merge_params.when_matched = LANCE_MERGE_WHEN_MATCHED_UPDATE_ALL;
    std::vector<lance::ShardSnapshot> merge_snapshots;
    merge_snapshots.push_back(std::move(*flushed_snapshot));
    auto merge_source = lance::LsmScanner::from_snapshots(
        dataset, std::move(merge_snapshots));
    auto merge_stream = merge_source.to_reader();
    std::vector<lance::CompactedSsTable> compacted_values;
    compacted_values.push_back(lance::CompactedSsTable::from_uuid(
        compacted_shard, compacted_generation));
    const auto before_compaction = dataset.version();
    auto merge = dataset.merge_insert_with_compaction(
        {"id"}, merge_stream.get(), &merge_params, compacted_values);
    CHECK(merge.num_updated_rows == 3);
    CHECK(merge.num_inserted_rows == 0);
    CHECK(dataset.version() == before_compaction + 1);
    auto compacted_dataset = lance::Dataset::open(uri);
    CHECK(compacted_dataset.version() == before_compaction + 1);
    CHECK(compacted_dataset.count_rows() == 3);

    // Snapshot values carry native generations and SSTable values.
    auto value = lance::ShardSnapshot::create(
        "00000000-0000-0000-0000-000000000000");
    value.set_spec_id(1);
    value.set_current_generation(4);
    value.add_sstable(3, "shard/3.lance");
    CHECK(value.spec_id() == 1);
    CHECK(value.current_generation() == 4);
    CHECK(value.sstables().size() == 1);
    CHECK(value.sstables()[0].generation == 3);

    auto compacted = lance::CompactedSsTable::from_uuid(
        "00000000-0000-0000-0000-000000000000", 3);
    CHECK(compacted.generation == 3);

    return 0;
}
