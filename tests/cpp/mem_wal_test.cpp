// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include "lance/lance.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
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
    options.column = std::string("id");
    dataset.initialize_mem_wal(options);

    auto details = dataset.mem_wal_index_details();
    CHECK(details.has_value());
    CHECK(details->num_shards == 1);
    CHECK(details->maintain_all_indexes);
    CHECK(details->sharding_specs.size() == 1);
    CHECK(details->sharding_specs[0].id() == 1);
    CHECK(details->sharding_specs[0].field_count() == 1);
    CHECK(details->sharding_specs[0].field_at(0).id() == "bucket");
    CHECK(details->sharding_specs[0].field_at(0).transform() ==
          std::optional<std::string>("unsharded"));
    CHECK(!details->writer_config_defaults.empty());

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

    auto snapshot = writer.snapshot();
    if (snapshot.has_value()) {
        CHECK(snapshot->shard_id() ==
              "00000000-0000-0000-0000-000000000000");
    }
    writer.close();
    writer.close();
    CHECK(writer.stats().put_count == 1);

    // Planner constructors compile and borrow the dataset/snapshots.
    lance::LsmPointLookupPlanner point_planner(dataset, {}, std::nullopt);
    lance::LsmVectorSearchPlanner vector_planner(
        dataset, {}, "vector", std::nullopt, lance::MemWalDistanceType::L2,
        std::nullopt);

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
