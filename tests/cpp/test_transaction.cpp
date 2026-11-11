// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include "lance/lance.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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

static ArrowArrayStream source_stream(const std::string& uri) {
    auto source = lance::Dataset::open(uri);
    auto scanner = source.scan();
    ArrowArrayStream stream;
    std::memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);
    return stream;
}

static lance::Transaction write_uncommitted(
    const std::string& source_uri,
    const std::string& destination_uri,
    lance::WriteMode mode,
    const lance::UncommittedWriteOptions& options = {},
    const lance::Session* session = nullptr) {
    ArrowArrayStream stream = source_stream(source_uri);
    return lance::Dataset::write_uncommitted(
        destination_uri, &stream, mode, options, {}, session);
}

static void check_dataset(const std::string& uri,
                          uint64_t version,
                          uint64_t rows) {
    auto dataset = lance::Dataset::open(uri);
    CHECK(dataset.version() == version);
    CHECK(dataset.count_rows() == rows);
}

static uint64_t write_cpp_replacement_file(
    const std::string& uri,
    const lance::Fragment& fragment,
    const std::string& filename) {
    auto scanner = fragment.scan();
    ArrowArrayStream raw_stream{};
    scanner.to_arrow_stream(&raw_stream);
    lance::ArrowStream stream(raw_stream);

    ArrowSchema schema{};
    if (stream->get_schema(stream.get(), &schema) != 0) {
        throw lance::Error(LANCE_ERR_INVALID_ARGUMENT,
                           "failed to read replacement stream schema");
    }

    lance::FileWriterOptions options;
    options.version = std::string("2.1");
    lance::FileSession files("file://" + uri + "/data");
    auto writer = files.open_writer(filename, &schema, options);

    while (true) {
        ArrowArray batch{};
        if (stream->get_next(stream.get(), &batch) != 0) {
            if (schema.release) schema.release(&schema);
            throw lance::Error(LANCE_ERR_INVALID_ARGUMENT,
                               "failed to read replacement batch");
        }
        if (!batch.release) break;
        CHECK(batch.n_children > 0);
        auto* ids = static_cast<int32_t*>(
            const_cast<void*>(batch.children[0]->buffers[1]));
        for (int64_t row = 0; row < batch.length; ++row) {
            ids[row] += 999;
        }
        writer.write_batch(lance::Batch::from_arrow(&batch, &schema));
    }
    if (schema.release) schema.release(&schema);

    const auto summary = writer.finish();
    CHECK(summary.num_rows == 4);
    CHECK(summary.size_bytes > 0);
    return summary.num_rows;
}

static std::vector<int32_t> collect_cpp_ids(const std::string& uri) {
    auto dataset = lance::Dataset::open(uri);
    auto scanner = dataset.scan();
    ArrowArrayStream raw_stream{};
    scanner.to_arrow_stream(&raw_stream);
    lance::ArrowStream stream(raw_stream);

    std::vector<int32_t> ids;
    while (true) {
        ArrowArray batch{};
        if (stream->get_next(stream.get(), &batch) != 0) {
            throw lance::Error(LANCE_ERR_INVALID_ARGUMENT,
                               "failed to read replacement result");
        }
        if (!batch.release) break;
        CHECK(batch.n_children > 0);
        const auto* values =
            static_cast<const int32_t*>(batch.children[0]->buffers[1]);
        ids.insert(ids.end(), values, values + batch.length);
        if (batch.release) batch.release(&batch);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}


struct RowPair {
    int64_t id;
    uint64_t rowid;
};

static std::vector<RowPair> read_id_rowid_pairs(lance::Dataset& dataset) {
    auto scanner = dataset.scan();
    scanner.project(std::vector<lance::ProjectionColumn>{{"id", "id"}});
    scanner.with_row_id(true);
    ArrowArrayStream stream;
    std::memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);
    std::vector<RowPair> rows;
    for (;;) {
        ArrowArray batch;
        std::memset(&batch, 0, sizeof(batch));
        CHECK(stream.get_next(&stream, &batch) == 0);
        if (!batch.release) break;
        CHECK(batch.n_children == 2);
        const ArrowArray* ids = batch.children[0];
        const ArrowArray* rowids = batch.children[1];
        CHECK(ids->buffers[1] != nullptr);
        CHECK(rowids->buffers[1] != nullptr);
        const auto* id_values =
            static_cast<const int64_t*>(ids->buffers[1]);
        const auto* rowid_values =
            static_cast<const uint64_t*>(rowids->buffers[1]);
        for (int64_t i = 0; i < batch.length; ++i) {
            rows.push_back(RowPair{id_values[ids->offset + i],
                                   rowid_values[rowids->offset + i]});
        }
        batch.release(&batch);
    }
    stream.release(&stream);
    return rows;
}

static void test_native_schema(const std::string& uri) {
    auto old_dataset = lance::Dataset::open(uri);
    const uint64_t old_version = old_dataset.version();
    CHECK(old_dataset.count_rows() == 8);
    CHECK(old_dataset.fragment_count() == 3);
    CHECK(old_dataset.has_stable_row_ids());

    auto schema = old_dataset.native_schema();
    CHECK(schema.field_id("id") == 0);
    CHECK(schema.field_id("name") == 2);
    CHECK(schema.field_id("profile") == 3);
    CHECK(schema.field_id("profile.score") == 4);
    CHECK(schema.field_id("profile.tags") == 5);

    ArrowSchema arrow;
    std::memset(&arrow, 0, sizeof(arrow));
    schema.to_arrow(&arrow);
    CHECK(arrow.n_children == 3);
    CHECK(std::string(arrow.children[0]->name) == "id");
    CHECK(std::string(arrow.children[2]->name) == "profile");
    arrow.release(&arrow);

    auto clone = schema.clone();
    bool threw = false;
    try {
        (void)clone.field_id("missing.column");
    } catch (const lance::Error& error) {
        threw = true;
        CHECK(error.code == LANCE_ERR_INTERNAL);
    }
    CHECK(threw);

    const auto before = read_id_rowid_pairs(old_dataset);
    CHECK(before.size() == 8);

    auto projected =
        lance::LanceSchema::project(schema, {"id", "name"});
    CHECK(projected.field_id("id") == 0);
    CHECK(projected.field_id("name") == 2);
    threw = false;
    try {
        (void)projected.field_id("profile.tags");
    } catch (const lance::Error& error) {
        threw = true;
        CHECK(error.code == LANCE_ERR_INTERNAL);
    }
    CHECK(threw);

    auto non_preserving =
        lance::Operation::make_project(projected, false);
    CHECK(non_preserving.kind() == lance::OperationKind::Project);
    CHECK(!non_preserving.preserves_nullability());

    auto operation = lance::Operation::make_project(projected, true);
    CHECK(operation.kind() == lance::OperationKind::Project);
    CHECK(operation.preserves_nullability());
    auto operation_schema = lance::LanceSchema::from_operation(operation);
    CHECK(operation_schema.field_id("name") == 2);
    auto operation_schema_clone = operation_schema.clone();
    CHECK(operation_schema_clone.field_id("id") == 0);

    lance::CommitOptions commit_options;
    commit_options.max_retries = 0;
    commit_options.timeout_mode = lance::CommitTimeoutMode::Disabled;
    projected = lance::LanceSchema::project(schema, {"id", "name"});
    operation = lance::Operation::make_project(projected, true);
    auto transaction =
        lance::Transaction::from_operation(old_version, operation);
    auto committed = lance::Dataset::commit(uri, transaction, commit_options);
    CHECK(committed.version() == old_version + 1);
    CHECK(committed.count_rows() == 8);
    CHECK(committed.fragment_count() == 3);
    CHECK(committed.native_schema().field_id("name") == 2);

    // The pre-commit snapshot keeps its original schema and rows.
    CHECK(old_dataset.count_rows() == 8);
    CHECK(old_dataset.native_schema().field_id("profile.tags") == 5);

    auto reopened = lance::Dataset::open(uri);
    CHECK(reopened.version() == old_version + 1);
    CHECK(reopened.count_rows() == 8);
    CHECK(reopened.fragment_count() == 3);
    auto reopened_schema = reopened.native_schema();
    CHECK(reopened_schema.field_id("id") == 0);
    CHECK(reopened_schema.field_id("name") == 2);
    ArrowSchema reopened_arrow;
    std::memset(&reopened_arrow, 0, sizeof(reopened_arrow));
    reopened_schema.to_arrow(&reopened_arrow);
    CHECK(reopened_arrow.n_children == 2);
    reopened_arrow.release(&reopened_arrow);
    CHECK(reopened.count_rows("id = 2") == 0);
    CHECK(reopened.count_rows("name = 'alpha'") == 3);
    CHECK(reopened.count_rows("name = 'beta'") == 2);
    CHECK(reopened.count_rows("name = 'gamma'") == 3);

    const auto after = read_id_rowid_pairs(reopened);
    CHECK(after.size() == before.size());
    for (size_t i = 0; i < after.size(); ++i) {
        CHECK(after[i].id == before[i].id);
        CHECK(after[i].rowid == before[i].rowid);
    }
    std::printf("C++ native schema and Project OK\n");
}

static void test_create_append_overwrite_and_batch(
    const std::string& source_uri,
    const std::string& base) {
    const std::string create_uri = base + "/cpp_txn_create";
    const std::string batch_uri = base + "/cpp_txn_batch";

    auto source = lance::Dataset::open(source_uri);
    const uint64_t source_rows = source.count_rows();

    lance::Session session(0, 0);
    lance::UncommittedWriteOptions write_options;
    write_options.write.params.max_rows_per_file = 1;
    write_options.transaction_properties =
        std::vector<std::pair<std::string, std::string>>{
            {"engine", "cpp17"},
            {"purpose", "transaction-raii"},
        };

    std::string create_uuid;
    std::vector<uint8_t> create_proto;
    std::optional<lance::Operation> held_operation;
    uint64_t create_fragments = 0;
    {
        auto transaction = write_uncommitted(
            source_uri, create_uri, lance::WriteMode::Create, write_options,
            &session);
        CHECK(transaction.read_version() == 0);
        create_uuid = transaction.uuid();
        CHECK(!create_uuid.empty());
        CHECK(!transaction.tag().has_value());
        CHECK(transaction.has_properties());
        auto properties = transaction.properties();
        CHECK(properties.size() == 2);
        CHECK(properties.at("engine") == "cpp17");
        CHECK(properties.at("purpose") == "transaction-raii");

        auto operation = transaction.operation();
        CHECK(operation.kind() == lance::OperationKind::Overwrite);
        create_fragments = operation.fragment_count();
        CHECK(create_fragments > 1);
        held_operation.emplace(std::move(operation));
        create_proto = transaction.to_proto();
    }

    CHECK(held_operation.has_value());
    CHECK(held_operation->kind() == lance::OperationKind::Overwrite);
    held_operation.reset();

    auto parsed = lance::Transaction::from_proto(create_proto);
    CHECK(parsed.uuid() == create_uuid);
    auto dataset =
        lance::Dataset::commit(create_uri, parsed, {}, {}, &session);
    CHECK(dataset.version() == 1);
    CHECK(dataset.count_rows() == source_rows);
    CHECK(dataset.fragment_count() == create_fragments);

    auto read_transaction = dataset.read_transaction();
    CHECK(read_transaction.has_value());
    CHECK(read_transaction->uuid() == create_uuid);
    auto history = dataset.read_transaction_by_version(1);
    CHECK(history.has_value());
    CHECK(history->uuid() == create_uuid);

    std::optional<lance::Transaction> held_transaction;
    {
        auto reopened = lance::Dataset::open(create_uri);
        held_transaction = reopened.read_transaction();
    }
    CHECK(held_transaction.has_value());
    CHECK(held_transaction->uuid() == create_uuid);
    CHECK(held_transaction->operation().kind() ==
          lance::OperationKind::Overwrite);
    held_transaction.reset();

    auto append =
        write_uncommitted(source_uri, create_uri, lance::WriteMode::Append);
    CHECK(append.read_version() == 1);
    CHECK(append.operation().kind() == lance::OperationKind::Append);
    check_dataset(create_uri, 1, source_rows);

    lance::CommitOptions append_options;
    append_options.max_retries = 0;
    append_options.retry_timeout = lance::CommitDuration{0, 0};
    append_options.timeout_mode = lance::CommitTimeoutMode::Disabled;
    auto appended =
        lance::Dataset::commit(create_uri, append, append_options);
    CHECK(appended.version() == 2);
    CHECK(appended.count_rows() == source_rows * 2);
    check_dataset(create_uri, 2, source_rows * 2);

    auto overwrite =
        write_uncommitted(source_uri, create_uri, lance::WriteMode::Overwrite);
    CHECK(overwrite.read_version() == 2);
    auto overwritten = lance::Dataset::commit(create_uri, overwrite);
    CHECK(overwritten.version() == 3);
    CHECK(overwritten.count_rows() == source_rows);
    check_dataset(create_uri, 3, source_rows);

    auto seed = write_uncommitted(source_uri, batch_uri, lance::WriteMode::Create);
    auto seed_dataset = lance::Dataset::commit(batch_uri, seed);
    CHECK(seed_dataset.version() == 1);

    auto left =
        write_uncommitted(source_uri, batch_uri, lance::WriteMode::Append);
    auto right =
        write_uncommitted(source_uri, batch_uri, lance::WriteMode::Append);
    std::vector<std::reference_wrapper<const lance::Transaction>> batch{
        std::cref(left), std::cref(right)};
    auto result = lance::Dataset::commit_batch(batch_uri, batch);
    CHECK(result.dataset.version() == 2);
    CHECK(result.dataset.count_rows() == source_rows * 3);
    CHECK(result.transaction.read_version() == 1);
    CHECK(result.transaction.operation().kind() ==
          lance::OperationKind::Append);
    check_dataset(batch_uri, 2, source_rows * 3);

    auto invalid_overwrite =
        write_uncommitted(source_uri, batch_uri, lance::WriteMode::Overwrite);
    std::vector<std::reference_wrapper<const lance::Transaction>> invalid_batch{
        std::cref(invalid_overwrite)};
    bool rejected = false;
    try {
        auto invalid = lance::Dataset::commit_batch(batch_uri, invalid_batch);
        (void)invalid;
    } catch (const lance::Error& error) {
        rejected = true;
        CHECK(error.code == LANCE_ERR_NOT_SUPPORTED);
    }
    CHECK(rejected);

    std::printf("C++ create/append/overwrite/batch OK\n");
}

static void test_detached_conflict_and_errors(const std::string& source_uri,
                                              const std::string& base) {
    const std::string uri = base + "/cpp_txn_detached";
    auto source = lance::Dataset::open(source_uri);
    const uint64_t source_rows = source.count_rows();

    auto seed = write_uncommitted(source_uri, uri, lance::WriteMode::Create);
    auto dataset = lance::Dataset::commit(uri, seed);
    CHECK(dataset.version() == 1);

    auto detached_transaction =
        write_uncommitted(source_uri, uri, lance::WriteMode::Append);
    const std::string detached_uuid = detached_transaction.uuid();
    lance::CommitOptions detached_options;
    detached_options.detached = true;
    detached_options.timeout_mode = lance::CommitTimeoutMode::Disabled;
    auto detached = lance::Dataset::commit(uri, detached_transaction,
                                           detached_options);
    const uint64_t detached_version = detached.version();
    CHECK((detached_version & (uint64_t{1} << 63)) != 0);
    CHECK(detached.count_rows() == source_rows * 2);

    auto detached_read = detached.read_transaction();
    CHECK(detached_read.has_value());
    CHECK(detached_read->uuid() == detached_uuid);

    auto latest = lance::Dataset::open(uri);
    CHECK(latest.version() == 1);
    CHECK(latest.count_rows() == source_rows);
    auto detached_history =
        latest.read_transaction_by_version(detached_version);
    CHECK(detached_history.has_value());
    CHECK(detached_history->uuid() == detached_uuid);

    auto first =
        write_uncommitted(source_uri, uri, lance::WriteMode::Overwrite);
    auto second =
        write_uncommitted(source_uri, uri, lance::WriteMode::Overwrite);
    auto winner = lance::Dataset::commit(uri, first);
    CHECK(winner.version() == 2);

    lance::CommitOptions zero_retries;
    zero_retries.max_retries = 0;
    bool conflicted = false;
    try {
        auto loser = lance::Dataset::commit(uri, second, zero_retries);
        (void)loser;
    } catch (const lance::Error& error) {
        conflicted = true;
        CHECK(error.code == LANCE_ERR_COMMIT_CONFLICT);
    }
    CHECK(conflicted);

    auto empty = lance::Transaction::from_proto(std::vector<uint8_t>{});
    CHECK(empty.read_version() == 0);
    CHECK(empty.uuid().empty());
    CHECK(empty.operation().kind() == lance::OperationKind::Unknown);

    std::printf("C++ detached/conflict/errors OK\n");
}

static void test_fragment_delete(const std::string& source_uri,
                                 const std::string& base) {
    const std::string uri = base + "/cpp_fragment_delete";

    lance::UncommittedWriteOptions write_options;
    write_options.write.params.max_rows_per_file = 4;
    write_options.write.params.enable_stable_row_ids = true;
    auto create =
        write_uncommitted(source_uri, uri, lance::WriteMode::Create, write_options);
    auto dataset = lance::Dataset::commit(uri, create);
    CHECK(dataset.version() == 1);
    CHECK(dataset.count_rows() == 20);
    CHECK(dataset.fragment_count() == 5);
    CHECK(dataset.delete_rows("id IN (2, 6)") == 2);
    const uint64_t version = dataset.version();
    const auto ids = dataset.fragment_ids();
    CHECK(ids.size() == 5);

    std::optional<lance::Fragment> first =
        dataset.open_fragment(ids[0]);
    std::optional<lance::Fragment> second =
        dataset.open_fragment(ids[1]);
    auto no_delete = first->delete_if("false");
    CHECK(no_delete.has_value());
    CHECK(no_delete->id() == ids[0]);
    no_delete.reset();

    auto all_deleted = second->delete_if("true");
    CHECK(!all_deleted.has_value());
    CHECK(second->logical_rows() == 3);

    auto staged = first->delete_if("id = 3");
    CHECK(staged.has_value());
    CHECK(staged->deleted_rows() == 2);
    CHECK(staged->logical_rows() == 2);
    CHECK(staged->metadata_json().find("\"deletion_file\":{") !=
          std::string::npos);
    {
        auto scanner = staged->scan();
        CHECK(scanner.count_rows() == 2);
    }

    auto operation = lance::Operation::make_delete(
        {&*staged}, {ids[1]}, "id = 3 OR true");
    CHECK(operation.kind() == lance::OperationKind::Delete);
    CHECK(operation.delete_updated_fragment_count() == 1);
    CHECK(operation.delete_updated_fragment_metadata_json(0) ==
          staged->metadata_json());
    CHECK(operation.delete_deleted_fragment_count() == 1);
    CHECK(operation.delete_deleted_fragment_id(0) == ids[1]);
    CHECK(operation.delete_predicate() == "id = 3 OR true");

    lance::TransactionOptions options;
    options.tag = "cpp-fragment-delete";
    options.transaction_properties =
        std::vector<std::pair<std::string, std::string>>{
            {"owner=cpp", "用户"},
            {"empty", ""},
        };
    auto transaction =
        lance::Transaction::from_operation(version, operation, options);
    CHECK(transaction.read_version() == version);
    CHECK(transaction.uuid().size() == 36);
    CHECK(transaction.tag() == std::optional<std::string>{"cpp-fragment-delete"});
    CHECK(transaction.has_properties());
    CHECK((transaction.properties() ==
           std::map<std::string, std::string>{
               {"owner=cpp", "用户"},
               {"empty", ""},
           }));

    auto bytes = transaction.to_proto();
    auto parsed = lance::Transaction::from_proto(bytes);
    CHECK(parsed.operation().kind() == lance::OperationKind::Delete);
    CHECK(parsed.operation().delete_predicate() == "id = 3 OR true");

    auto omitted = lance::Transaction::from_operation(version, operation);
    CHECK(omitted.uuid().size() == 36);
    CHECK(!omitted.tag().has_value());
    CHECK(!omitted.has_properties());

    staged.reset();
    first.reset();
    second.reset();

    lance::CommitOptions commit_options;
    commit_options.max_retries = 0;
    commit_options.timeout_mode = lance::CommitTimeoutMode::Disabled;
    auto committed = lance::Dataset::commit(uri, parsed, commit_options);
    CHECK(committed.version() == version + 1);
    CHECK(committed.count_rows() == 14);
    CHECK(committed.fragment_count() == 4);
    auto read_shaped = committed.read_transaction();
    CHECK(read_shaped.has_value());
    CHECK(read_shaped->read_version() == version);
    CHECK(read_shaped->tag() ==
          std::optional<std::string>{"cpp-fragment-delete"});
    CHECK(read_shaped->properties().at("owner=cpp") == "用户");
    CHECK(read_shaped->properties().at("empty").empty());
    CHECK(read_shaped->operation().kind() == lance::OperationKind::Delete);

    auto reopened = lance::Dataset::open(uri);
    CHECK(reopened.version() == version + 1);
    CHECK(reopened.count_rows() == 14);
    CHECK(reopened.fragment_count() == 4);

    std::printf("C++ fragment delete OK\n");
}

static void test_create_index_operation(const std::string& source_uri,
                                        const std::string& base) {
    const std::string uri = base + "/cpp_create_index";
    lance::UncommittedWriteOptions write_options;
    write_options.write.params.max_rows_per_file = 5;
    write_options.write.params.enable_stable_row_ids = true;
    auto create =
        write_uncommitted(source_uri, uri, lance::WriteMode::Create, write_options);
    auto dataset = lance::Dataset::commit(uri, create);
    CHECK(dataset.version() == 1);
    CHECK(dataset.fragment_count() == 4);

    auto fragment_ids = dataset.fragment_ids();
    CHECK(fragment_ids.size() >= 2);
    const uint64_t version = dataset.version();
    const char* params = "{\"rows_per_zone\":2,\"use_seeds\":false}";
    const std::array<uint8_t, 16> first_uuid = {
        0x51, 0x62, 0x73, 0x84, 0x95, 0xa6, 0x47, 0xb8,
        0xc9, 0xda, 0xeb, 0xfc, 0x0d, 0x1e, 0x2f, 0x30,
    };
    const std::array<uint8_t, 16> second_uuid = {
        0x41, 0x52, 0x63, 0x74, 0x85, 0x96, 0xa7, 0xb8,
        0xc9, 0xda, 0xeb, 0xfc, 0x0d, 0x1e, 0x2f, 0x30,
    };

    std::optional<lance::Operation> held_operation;
    std::array<uint8_t, 16> first_committed_uuid{};
    std::array<uint8_t, 16> second_committed_uuid{};
    std::vector<lance::IndexSegmentFile> first_files;
    std::vector<uint8_t> first_details;
    std::optional<lance::IndexTimestamp> first_created_at;
    {
        uint32_t first_fragment = static_cast<uint32_t>(fragment_ids[0]);
        uint32_t second_fragment = static_cast<uint32_t>(fragment_ids[1]);
        LanceIndexSegmentBuildOptions first_options = {};
        first_options.fragment_ids = &first_fragment;
        first_options.fragment_count = 1;
        first_options.index_uuid = first_uuid.data();
        first_options.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;
        auto first_builder = dataset.new_scalar_index_segment_builder(
            "score", LANCE_SCALAR_ZONE_MAP, "cpp_create_index_zonemap",
            params, &first_options);
        auto first_bytes = first_builder.execute_uncommitted();
        auto first = lance::IndexSegmentMetadata::parse(first_bytes);

        LanceIndexSegmentBuildOptions second_options = {};
        second_options.fragment_ids = &second_fragment;
        second_options.fragment_count = 1;
        second_options.index_uuid = second_uuid.data();
        second_options.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;
        auto second_builder = dataset.new_scalar_index_segment_builder(
            "score", LANCE_SCALAR_ZONE_MAP, "cpp_create_index_zonemap",
            params, &second_options);
        auto second_bytes = second_builder.execute_uncommitted();
        auto second = lance::IndexSegmentMetadata::parse(second_bytes);

        auto cloned = first.clone();
        CHECK(cloned.uuid() == first_uuid);
        CHECK(cloned.covering_field_ids().empty());
        CHECK(!cloned.index_details_value().empty());
        CHECK(cloned.created_at().has_value());
        CHECK(!cloned.base_id().has_value());
        CHECK(cloned.files().size() == first.files().size());

        first_committed_uuid = first.uuid();
        second_committed_uuid = second.uuid();
        first_files = first.files();
        first_details = first.index_details_value();
        first_created_at = first.created_at();
        CHECK(!first_files.empty());
        CHECK(std::all_of(first_files.begin(), first_files.end(),
                          [](const lance::IndexSegmentFile& file) {
                              return !file.path.empty() && file.size_bytes > 0;
                          }));
        CHECK(first_created_at->unix_seconds > 0);

        held_operation.emplace(
            lance::Operation::make_create_index({&first, &second}, {}));
    }

    CHECK(held_operation->kind() == lance::OperationKind::CreateIndex);
    auto new_metadata = held_operation->create_index_new();
    CHECK(new_metadata.size() == 2);
    CHECK(new_metadata[0].uuid() == first_committed_uuid);
    CHECK(new_metadata[1].uuid() == second_committed_uuid);
    CHECK(new_metadata[0].name() == "cpp_create_index_zonemap");
    CHECK(new_metadata[0].index_details_value() == first_details);
    CHECK(new_metadata[0].files().size() == first_files.size());
    CHECK(held_operation->create_index_removed().empty());

    auto transaction =
        lance::Transaction::from_operation(version, *held_operation);
    auto proto = transaction.to_proto();
    auto parsed = lance::Transaction::from_proto(proto);
    auto parsed_metadata = parsed.operation().create_index_new();
    CHECK(parsed_metadata.size() == 2);
    CHECK(parsed_metadata[0].uuid() == first_committed_uuid);
    CHECK(parsed_metadata[1].uuid() == second_committed_uuid);

    lance::CommitOptions commit_options;
    commit_options.max_retries = 0;
    commit_options.timeout_mode = lance::CommitTimeoutMode::Disabled;
    auto committed =
        lance::Dataset::commit(uri, transaction, commit_options);
    CHECK(committed.version() == version + 1);
    CHECK(committed.index_segment_count("cpp_create_index_zonemap") == 2);
    CHECK(committed.count_rows("score = 80") == 1);

    auto history = committed.read_transaction();
    CHECK(history.has_value());
    auto history_operation = history->operation();
    CHECK(history_operation.kind() == lance::OperationKind::CreateIndex);
    auto history_metadata = history_operation.create_index_new();
    CHECK(history_metadata.size() == 2);
    CHECK(history_metadata[0].uuid() == first_committed_uuid);
    CHECK(history_metadata[1].uuid() == second_committed_uuid);
    CHECK(history_metadata[0].index_details_value() == first_details);
    CHECK(history_metadata[0].files() == first_files);
    CHECK(history_metadata[0].created_at().has_value());
    CHECK(history_metadata[0].created_at()->unix_seconds ==
          first_created_at->unix_seconds);
    CHECK(history_metadata[0].created_at()->nanos == first_created_at->nanos);
    CHECK(history_operation.create_index_removed().empty());

    held_operation.reset();
    new_metadata.clear();
    history.reset();
    committed = lance::Dataset::open(uri);
    CHECK(committed.version() == version + 1);
    CHECK(committed.index_segment_count("cpp_create_index_zonemap") == 2);
    std::printf("C++ CreateIndex transaction OK\n");
}

static void test_data_replacement(const std::string& source_uri,
                                  const std::string& base) {
    const std::string uri = base + "/cpp_data_replacement";
    lance::UncommittedWriteOptions options;
    options.write.params.max_rows_per_file = 4;
    options.write.params.data_storage_version = std::string("2.1");
    auto create = write_uncommitted(
        source_uri, uri, lance::WriteMode::Create, options);
    auto dataset = lance::Dataset::commit(uri, create);
    CHECK(dataset.version() == 1);
    CHECK(dataset.count_rows() == 20);
    CHECK(dataset.fragment_count() == 5);
    const auto ids = dataset.fragment_ids();

    const std::string filename = "cpp-data-replacement.lance";
    auto fragment = dataset.open_fragment(ids[0]);
    CHECK(write_cpp_replacement_file(uri, fragment, filename) == 4);

    auto file = dataset.create_data_file(filename);
    CHECK(file.path() == filename);
    CHECK(!file.fields().empty());
    CHECK(!file.column_indices().empty());
    CHECK(file.file_version() == std::make_pair(uint32_t{2}, uint32_t{1}));
    CHECK(file.file_size().has_value() && *file.file_size() > 0);
    CHECK(!file.base_id().has_value());

    auto file_clone = file.clone();
    CHECK(file_clone.path() == file.path());
    CHECK(file_clone.fields() == file.fields());
    auto moved_file = std::move(file_clone);
    CHECK(moved_file.path() == file.path());

    auto group = lance::DataReplacementGroup(ids[0], file);
    CHECK(group.fragment_id() == ids[0]);
    CHECK(group.file().path() == filename);
    auto group_clone = group.clone();
    CHECK(group_clone.fragment_id() == ids[0]);

    auto operation = lance::Operation::make_data_replacement({&group});
    CHECK(operation.kind() == lance::OperationKind::DataReplacement);
    CHECK(operation.data_replacement_count() == 1);
    auto inspected = operation.data_replacement_at(0);
    CHECK(inspected.fragment_id() == ids[0]);
    CHECK(inspected.file().fields() == file.fields());
    bool out_of_range = false;
    try {
        (void)operation.data_replacement_at(1);
    } catch (const lance::Error& error) {
        out_of_range = true;
        CHECK(error.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    CHECK(out_of_range);

    auto empty = lance::Operation::make_data_replacement({});
    CHECK(empty.kind() == lance::OperationKind::DataReplacement);
    CHECK(empty.data_replacement_count() == 0);

    CHECK(dataset.delete_rows("id = 2") == 1);
    CHECK(dataset.version() == 2);
    auto transaction =
        lance::Transaction::from_operation(dataset.version(), operation);
    lance::CommitOptions commit_options;
    commit_options.max_retries = 0;
    commit_options.timeout_mode = lance::CommitTimeoutMode::Disabled;
    auto committed =
        lance::Dataset::commit(uri, transaction, commit_options);
    CHECK(committed.version() == 3);
    CHECK(committed.count_rows() == 19);
    CHECK(committed.fragment_count() == 5);

    auto history = committed.read_transaction();
    CHECK(history.has_value());
    CHECK(history->operation().kind() ==
          lance::OperationKind::DataReplacement);
    CHECK(history->operation().data_replacement_count() == 1);
    CHECK(history->operation().data_replacement_at(0).fragment_id() == ids[0]);

    auto actual = collect_cpp_ids(uri);
    std::vector<int32_t> expected{1000, 1002, 1003, 5, 6, 7, 8, 9, 10,
                                  11,   12,   13,   14, 15, 16, 17, 18,
                                  19,   20};
    std::sort(expected.begin(), expected.end());
    CHECK(actual == expected);

    std::printf("C++ data replacement OK\n");
}

namespace {

void publication_release_schema(ArrowSchema* schema) {
    if (schema) schema->release = nullptr;
}

void publication_release_array(ArrowArray* array) {
    if (array) array->release = nullptr;
}

struct PublicationBatchData {
    std::vector<uint32_t> ids;
    std::array<const void*, 2> id_buffers{};
    ArrowArray id_array{};
    ArrowArray blob_array{};
    std::array<ArrowArray*, 2> children{};
    std::array<const void*, 1> root_buffers{};
};

/// The native writer may retain the input after write_batch returns.
/// Its Arrow owner releases these headers and buffers together.
void publication_release_root_array(ArrowArray* array) {
    if (!array || !array->release) return;
    array->release = nullptr;
    for (int64_t i = 0; i < array->n_children; ++i) {
        ArrowArray* child = array->children[i];
        if (child && child->release) child->release(child);
    }
    delete static_cast<PublicationBatchData*>(array->private_data);
    array->private_data = nullptr;
}

struct PublicationRow {
    uint32_t id = 0;
    bool blob_is_null = false;
    std::vector<uint8_t> blob;
    uint64_t row_id = 0;
};

int64_t publication_column(const ArrowSchema& schema, const char* name) {
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (std::strcmp(schema.children[i]->name, name) == 0) return i;
    }
    std::fprintf(stderr, "missing column %s\n", name);
    std::exit(1);
}

std::vector<PublicationRow> read_publication_rows(
    const lance::Dataset& dataset) {
    auto scanner = dataset.scan();
    scanner.with_row_id(true);
    scanner.blob_handling(LANCE_BLOB_HANDLING_ALL_BINARY);
    ArrowArrayStream raw{};
    scanner.to_arrow_stream(&raw);
    lance::ArrowStream stream(raw);

    ArrowSchema schema{};
    CHECK(stream->get_schema(stream.get(), &schema) == 0);
    const int64_t id_column = publication_column(schema, "id");
    const int64_t blob_column = publication_column(schema, "blob");
    const int64_t row_id_column = publication_column(schema, "_rowid");
    CHECK(std::strcmp(schema.children[blob_column]->format, "Z") == 0);

    std::vector<PublicationRow> rows;
    while (true) {
        ArrowArray batch{};
        CHECK(stream->get_next(stream.get(), &batch) == 0);
        if (!batch.release) break;
        CHECK(batch.n_children == schema.n_children);
        const ArrowArray& ids = *batch.children[id_column];
        const ArrowArray& blobs = *batch.children[blob_column];
        const ArrowArray& row_ids = *batch.children[row_id_column];
        CHECK(ids.offset == 0 && blobs.offset == 0 && row_ids.offset == 0);
        const auto* id_values =
            static_cast<const uint32_t*>(ids.buffers[1]);
        const auto* row_id_values =
            static_cast<const uint64_t*>(row_ids.buffers[1]);
        const auto* offsets =
            static_cast<const int64_t*>(blobs.buffers[1]);
        const auto* data = static_cast<const uint8_t*>(blobs.buffers[2]);
        const auto* validity =
            static_cast<const uint8_t*>(blobs.buffers[0]);
        for (int64_t i = 0; i < batch.length; ++i) {
            const int64_t index = batch.offset + i;
            PublicationRow row;
            row.id = id_values[index];
            row.row_id = row_id_values[index];
            const bool valid =
                !validity || (validity[index / 8] & (1u << (index % 8)));
            if (valid) {
                row.blob.assign(data + offsets[index],
                                data + offsets[index + 1]);
            } else {
                row.blob_is_null = true;
            }
            rows.push_back(std::move(row));
        }
        if (batch.release) batch.release(&batch);
    }
    if (schema.release) schema.release(&schema);
    std::sort(rows.begin(), rows.end(),
              [](const PublicationRow& a, const PublicationRow& b) {
                  return a.id < b.id;
              });
    return rows;
}

const PublicationRow& publication_row(const std::vector<PublicationRow>& rows,
                                      uint32_t id) {
    for (const auto& row : rows) {
        if (row.id == id) return row;
    }
    std::fprintf(stderr, "missing published row id=%u\n", id);
    std::exit(1);
}

void check_publication_blob(const std::vector<PublicationRow>& rows,
                            uint32_t id,
                            const std::string& expected) {
    const auto& row = publication_row(rows, id);
    CHECK(!row.blob_is_null);
    CHECK(row.blob == std::vector<uint8_t>(expected.begin(), expected.end()));
}

void check_publication_null(const std::vector<PublicationRow>& rows,
                            uint32_t id) {
    const auto& row = publication_row(rows, id);
    CHECK(row.blob_is_null);
    CHECK(row.blob.empty());
}

struct ReplacementBatch {
    std::vector<uint32_t> ids;
    ArrowArray* blob_array;
};

/// Write one or more pre-finished blob-descriptor batches to a raw Lance file.
///
/// The first batch may hold many packed descriptors; a dedicated file is
/// assembled by appending one-row batches through the same writer. The blob
/// field comes from `finish_array`, so its native metadata is preserved.
void write_blob_replacement_file(
    const std::string& uri,
    const std::string& filename,
    const ArrowSchema* blob_field,
    const std::vector<ReplacementBatch>& batches) {
    ArrowSchema id_field{};
    id_field.format = "I";
    id_field.name = "id";
    id_field.release = &publication_release_schema;

    ArrowSchema root_schema{};
    ArrowSchema* schema_children[2] = {
        &id_field, const_cast<ArrowSchema*>(blob_field)};
    root_schema.format = "+s";
    root_schema.name = "";
    root_schema.n_children = 2;
    root_schema.children = schema_children;
    root_schema.release = &publication_release_schema;

    lance::FileSession files("file://" + uri + "/data");
    lance::FileWriterOptions options;
    options.version = std::string("2.2");
    options.keep_original_array = true;
    auto writer = files.open_writer(filename, &root_schema, options);

    uint64_t written = 0;
    for (const auto& replacement : batches) {
        auto data = std::make_unique<PublicationBatchData>();
        data->ids = replacement.ids;
        data->id_buffers = {nullptr, data->ids.data()};
        data->id_array.length = static_cast<int64_t>(data->ids.size());
        data->id_array.n_buffers = 2;
        data->id_array.buffers = data->id_buffers.data();
        data->id_array.release = &publication_release_array;
        data->blob_array = *replacement.blob_array;
        replacement.blob_array->release = nullptr;
        data->children = {&data->id_array, &data->blob_array};
        ArrowArray root_array{};
        root_array.length = static_cast<int64_t>(data->ids.size());
        root_array.n_buffers = 1;
        root_array.buffers = data->root_buffers.data();
        root_array.n_children = 2;
        root_array.children = data->children.data();
        root_array.release = &publication_release_root_array;
        root_array.private_data = data.release();

        auto batch = lance::Batch::from_arrow(&root_array, &root_schema);
        writer.write_batch(batch);
        written += replacement.ids.size();
    }

    auto summary = writer.finish();
    CHECK(summary.num_rows == written);
}

}  // namespace

static void test_blob_writer_data_replacement(const std::string& blob_uri) {
    auto dataset = lance::Dataset::open(blob_uri);
    CHECK(dataset.version() == 3);
    CHECK(dataset.data_storage_version() == "2.2");
    CHECK(dataset.has_stable_row_ids());
    CHECK(dataset.count_rows() == 7);
    CHECK(dataset.fragment_count() == 3);
    const auto fragment_ids = dataset.fragment_ids();

    const auto initial = read_publication_rows(dataset);
    CHECK(initial.size() == 7);
    check_publication_blob(initial, 0, "old-packed-0");
    check_publication_null(initial, 1);
    check_publication_blob(initial, 2, "");
    check_publication_blob(initial, 3, "old-dedicated-3");
    check_publication_blob(initial, 4, "old-dedicated-4");
    check_publication_blob(initial, 5, "keep-c5");
    check_publication_blob(initial, 6, "keep-c6");

    CHECK(dataset.delete_rows("id = 1") == 1);
    CHECK(dataset.version() == 4);
    CHECK(dataset.count_rows() == 6);
    const auto before = read_publication_rows(dataset);
    CHECK(before.size() == 6);
    check_publication_blob(before, 0, "old-packed-0");
    check_publication_blob(before, 2, "");
    check_publication_blob(before, 3, "old-dedicated-3");
    check_publication_blob(before, 4, "old-dedicated-4");

    const std::string packed_name = "cpp-packed-publication.lance";
    const std::string dedicated_name = "cpp-dedicated-publication.lance";
    lance::FileSession files("file://" + blob_uri + "/data");

    std::vector<uint8_t> packed_validity(1, 0x03);
    std::vector<int32_t> packed_offsets{0};
    std::vector<uint8_t> packed_data;
    auto append_packed = [&](const char* value) {
        packed_data.insert(packed_data.end(), value, value + std::strlen(value));
        packed_offsets.push_back(static_cast<int32_t>(packed_data.size()));
    };
    append_packed("new-packed-0");
    packed_offsets.push_back(static_cast<int32_t>(packed_data.size()));
    packed_offsets.push_back(static_cast<int32_t>(packed_data.size()));
    const void* packed_buffers[3] = {
        packed_validity.data(), packed_offsets.data(), packed_data.data()};
    ArrowArray packed_payloads{};
    packed_payloads.length = 3;
    packed_payloads.null_count = 1;
    packed_payloads.n_buffers = 3;
    packed_payloads.buffers = packed_buffers;
    packed_payloads.release = &publication_release_array;
    ArrowSchema packed_payload_schema{};
    packed_payload_schema.format = "z";
    packed_payload_schema.name = "blob";
    packed_payload_schema.flags = 2;
    packed_payload_schema.release = &publication_release_schema;

    auto packed_writer = files.open_packed_blob_writer(packed_name, 7001);
    CHECK(packed_writer.blob_id() == 7001);
    const std::string packed_sidecar = packed_writer.path();
    CHECK(packed_sidecar != packed_name);
    packed_writer.write_blobs(&packed_payloads, &packed_payload_schema);
    CHECK(packed_payloads.release == nullptr);
    CHECK(packed_payload_schema.release != nullptr);
    ArrowArray packed_array{};
    ArrowSchema packed_field{};
    packed_writer.finish_array("blob", &packed_array, &packed_field);
    CHECK(packed_array.length == 3);
    CHECK(packed_field.release != nullptr);

    ArrowArray dedicated_a{};
    ArrowSchema dedicated_a_field{};
    ArrowArray dedicated_b{};
    ArrowSchema dedicated_b_field{};
    std::string dedicated_sidecar_a;
    std::string dedicated_sidecar_b;
    {
        const std::string payload = "new-dedicated-3";
        auto writer =
            files.open_dedicated_blob_writer(dedicated_name, 7002);
        CHECK(writer.blob_id() == 7002);
        dedicated_sidecar_a = writer.path();
        writer.write(reinterpret_cast<const uint8_t*>(payload.data()),
                     payload.size());
        writer.finish_array("blob", &dedicated_a, &dedicated_a_field);
        CHECK(dedicated_a.length == 1);
    }
    {
        const std::string payload;
        auto writer =
            files.open_dedicated_blob_writer(dedicated_name, 7003);
        CHECK(writer.blob_id() == 7003);
        dedicated_sidecar_b = writer.path();
        writer.write(reinterpret_cast<const uint8_t*>(payload.data()),
                     payload.size());
        writer.finish_array("blob", &dedicated_b, &dedicated_b_field);
        CHECK(dedicated_b.length == 1);
    }
    CHECK(dedicated_sidecar_a != dedicated_sidecar_b);
    CHECK(dedicated_sidecar_a != packed_sidecar);

    write_blob_replacement_file(
        blob_uri, packed_name, &packed_field,
        {ReplacementBatch{{0, 1, 2}, &packed_array}});
    CHECK(packed_array.release == nullptr);
    write_blob_replacement_file(
        blob_uri, dedicated_name, &dedicated_a_field,
        {ReplacementBatch{{3}, &dedicated_a},
         ReplacementBatch{{4}, &dedicated_b}});
    CHECK(dedicated_a.release == nullptr);
    CHECK(dedicated_b.release == nullptr);
    if (packed_field.release) packed_field.release(&packed_field);
    if (dedicated_a_field.release) {
        dedicated_a_field.release(&dedicated_a_field);
    }
    if (dedicated_b_field.release) {
        dedicated_b_field.release(&dedicated_b_field);
    }

    auto packed_file = dataset.create_data_file(packed_name);
    auto dedicated_file = dataset.create_data_file(dedicated_name);
    CHECK(packed_file.path() == packed_name);
    CHECK(dedicated_file.path() == dedicated_name);
    CHECK(packed_file.file_version() ==
          std::make_pair(uint32_t{2}, uint32_t{2}));
    CHECK(dedicated_file.file_version() ==
          std::make_pair(uint32_t{2}, uint32_t{2}));
    CHECK(!packed_file.fields().empty());
    CHECK(packed_file.fields() == dedicated_file.fields());
    CHECK(!packed_file.column_indices().empty());
    CHECK(packed_file.column_indices() == dedicated_file.column_indices());
    CHECK(packed_file.file_size().has_value() && *packed_file.file_size() > 0);
    CHECK(dedicated_file.file_size().has_value() &&
          *dedicated_file.file_size() > 0);
    CHECK(!packed_file.base_id().has_value());
    CHECK(!dedicated_file.base_id().has_value());

    auto packed_group =
        lance::DataReplacementGroup(fragment_ids[0], packed_file);
    auto dedicated_group =
        lance::DataReplacementGroup(fragment_ids[1], dedicated_file);
    auto operation = lance::Operation::make_data_replacement(
        {&packed_group, &dedicated_group});
    CHECK(operation.data_replacement_count() == 2);
    CHECK(operation.data_replacement_at(0).fragment_id() == fragment_ids[0]);
    CHECK(operation.data_replacement_at(1).fragment_id() == fragment_ids[1]);

    lance::CommitOptions commit_options;
    commit_options.max_retries = 0;
    commit_options.timeout_mode = lance::CommitTimeoutMode::Disabled;
    auto committed = lance::Dataset::commit(
        blob_uri,
        lance::Transaction::from_operation(dataset.version(), operation),
        commit_options);
    CHECK(committed.version() == dataset.version() + 1);
    CHECK(committed.count_rows() == 6);
    CHECK(committed.fragment_count() == 3);
    CHECK(committed.fragment_ids() == fragment_ids);

    auto history = committed.read_transaction();
    CHECK(history.has_value());
    CHECK(history->operation().data_replacement_count() == 2);

    auto first = committed.open_fragment(fragment_ids[0]);
    CHECK(first.physical_rows() == 3);
    CHECK(first.deleted_rows() == 1);
    CHECK(first.logical_rows() == 2);
    auto second = committed.open_fragment(fragment_ids[1]);
    CHECK(second.physical_rows() == 2);
    CHECK(second.deleted_rows() == 0);
    CHECK(second.logical_rows() == 2);
    auto third = committed.open_fragment(fragment_ids[2]);
    CHECK(third.physical_rows() == 2);
    CHECK(third.logical_rows() == 2);

    auto reopened = lance::Dataset::open(blob_uri);
    CHECK(reopened.version() == committed.version());
    const auto after = read_publication_rows(reopened);
    CHECK(after.size() == 6);
    check_publication_blob(after, 0, "new-packed-0");
    check_publication_null(after, 2);
    check_publication_blob(after, 3, "new-dedicated-3");
    check_publication_blob(after, 4, "");
    check_publication_blob(after, 5, "keep-c5");
    check_publication_blob(after, 6, "keep-c6");
    for (const auto& row : before) {
        CHECK(publication_row(after, row.id).row_id == row.row_id);
    }

    auto old_after_delete = lance::Dataset::open(blob_uri, {}, 4);
    CHECK(old_after_delete.version() == 4);
    const auto old_rows = read_publication_rows(old_after_delete);
    check_publication_blob(old_rows, 0, "old-packed-0");
    check_publication_blob(old_rows, 3, "old-dedicated-3");
    auto pristine = lance::Dataset::open(blob_uri, {}, 3);
    CHECK(pristine.count_rows() == 7);
    const auto pristine_rows = read_publication_rows(pristine);
    check_publication_blob(pristine_rows, 0, "old-packed-0");
    check_publication_null(pristine_rows, 1);

    const std::vector<uint64_t> live_indices{0, 1, 2, 3, 4, 5};
    auto blobs = reopened.take_blobs_by_indices(
        live_indices.data(), live_indices.size(), "blob");
    CHECK(blobs.size() == 6);
    const std::vector<std::optional<std::string>> expected{
        "new-packed-0", std::nullopt, "new-dedicated-3", "",
        "keep-c5", "keep-c6"};
    for (size_t i = 0; i < expected.size(); ++i) {
        CHECK(blobs[i].has_value() == expected[i].has_value());
        if (!expected[i]) continue;
        CHECK(blobs[i]->size() == expected[i]->size());
        const auto bytes = blobs[i]->read();
        CHECK(bytes == std::vector<uint8_t>(expected[i]->begin(),
                                            expected[i]->end()));
    }
    const std::vector<uint8_t> expected_range{'p', 'a', 'c', 'k', 'e', 'd', '-'};
    CHECK(blobs[0]->read_range(4, 7) == expected_range);

    std::printf("C++ blob writer publication OK\n");
}


static void test_native_operations(const std::string& source_uri,
                                   const std::string& base) {
    const std::string uri = base + "/cpp_native_operations";
    const auto source_rows =
        lance::Dataset::open(source_uri).count_rows();

    lance::UncommittedWriteOptions options;
    options.write.params.max_rows_per_file = 1;
    auto seed = lance::Dataset::commit(
        uri, write_uncommitted(source_uri, uri, lance::WriteMode::Create,
                               options));
    CHECK(seed.version() == 1);
    CHECK(seed.fragment_count() > 1);
    const auto seed_fragments = seed.fragment_count();

    uint64_t read_version = 0;
    std::vector<lance::Fragment> detached;
    std::vector<std::string> detached_json;
    {
        auto staged = write_uncommitted(
            source_uri, uri, lance::WriteMode::Append, options);
        read_version = staged.read_version();
        CHECK(read_version == 1);
        auto operation = staged.operation();
        CHECK(operation.kind() == lance::OperationKind::Append);
        CHECK(operation.append_fragment_count() > 0);
        CHECK(operation.append_fragment_count() == operation.fragment_count());
        for (size_t i = 0; i < operation.append_fragment_count(); ++i) {
            detached.push_back(operation.append_fragment_at(i));
            CHECK(!detached.back().is_bound());
            CHECK(detached.back().data_file_count() > 0);
            auto file = detached.back().data_file_at(0);
            CHECK(!file.path().empty());
            auto field_file = detached.back().data_file_for_field(0);
            CHECK(field_file.has_value());
            CHECK(!field_file->path().empty());
            detached_json.push_back(detached.back().metadata_json());
            CHECK(!detached_json.back().empty());
        }
    }
    for (size_t i = 0; i < detached.size(); ++i) {
        CHECK(detached[i].metadata_json() == detached_json[i]);
    }

    bool detached_rejected = false;
    try {
        (void)detached.front().physical_rows();
    } catch (const lance::Error& error) {
        detached_rejected = error.code == LANCE_ERR_NOT_SUPPORTED;
    }
    CHECK(detached_rejected);

    std::vector<const lance::Fragment*> pointers;
    pointers.reserve(detached.size());
    for (const auto& fragment : detached) pointers.push_back(&fragment);
    auto append = lance::Operation::make_append(pointers);
    CHECK(append.append_fragment_count() == detached.size());
    auto append_transaction =
        lance::Transaction::from_operation(read_version, append);
    auto appended = lance::Dataset::commit(uri, append_transaction);
    CHECK(appended.version() == 2);
    CHECK(appended.count_rows() == source_rows * 2);
    CHECK(appended.fragment_count() == seed_fragments * 2);
    const auto appended_ids = appended.fragment_ids();
    const auto max_after_append =
        *std::max_element(appended_ids.begin(), appended_ids.end());

    auto restore = lance::Operation::make_restore(1);
    CHECK(restore.restore_version() == 1);
    auto restored = lance::Dataset::commit(
        uri, lance::Transaction::from_operation(2, restore));
    CHECK(restored.version() == 3);
    CHECK(restored.count_rows() == source_rows);
    CHECK(restored.fragment_count() == seed_fragments);

    auto reserve_zero = lance::Operation::make_reserve_fragments(0);
    CHECK(reserve_zero.reserve_fragments_count() == 0);
    auto reserved_zero = lance::Dataset::commit(
        uri, lance::Transaction::from_operation(3, reserve_zero));
    CHECK(reserved_zero.version() == 4);
    CHECK(reserved_zero.count_rows() == source_rows);

    auto reserve_two = lance::Operation::make_reserve_fragments(2);
    CHECK(reserve_two.reserve_fragments_count() == 2);
    auto reserved = lance::Dataset::commit(
        uri, lance::Transaction::from_operation(4, reserve_two));
    CHECK(reserved.version() == 5);
    CHECK(reserved.fragment_count() == seed_fragments);

    std::vector<lance::Fragment> reserved_fragments;
    uint64_t reserved_read_version = 0;
    {
        auto staged = write_uncommitted(
            source_uri, uri, lance::WriteMode::Append, options);
        reserved_read_version = staged.read_version();
        CHECK(reserved_read_version == 5);
        auto operation = staged.operation();
        for (size_t i = 0; i < operation.append_fragment_count(); ++i) {
            reserved_fragments.push_back(operation.append_fragment_at(i));
            CHECK(!reserved_fragments.back().metadata_json().empty());
        }
    }
    std::vector<const lance::Fragment*> reserved_pointers;
    reserved_pointers.reserve(reserved_fragments.size());
    for (const auto& fragment : reserved_fragments) {
        reserved_pointers.push_back(&fragment);
    }
    auto reserved_append = lance::Operation::make_append(reserved_pointers);
    auto reserved_commit = lance::Dataset::commit(
        uri, lance::Transaction::from_operation(reserved_read_version,
                                                reserved_append));
    CHECK(reserved_commit.version() == 6);
    CHECK(reserved_commit.fragment_count() ==
          seed_fragments + reserved_fragments.size());
    const auto reserved_ids = reserved_commit.fragment_ids();
    CHECK(*std::max_element(reserved_ids.begin(), reserved_ids.end()) ==
          max_after_append + 2 + reserved_fragments.size());

    const std::string base_a = uri + "_cpp_base_a";
    const std::string base_b = uri + "_cpp_base_b";
    std::vector<lance::BasePath> bases;
    lance::BasePath first_base;
    first_base.id = 7;
    first_base.path = base_a;
    first_base.name = std::nullopt;
    first_base.is_dataset_root = false;
    bases.push_back(first_base);
    lance::BasePath second_base;
    second_base.id = 0;
    second_base.path = base_b;
    second_base.name = std::string();
    second_base.is_dataset_root = true;
    bases.push_back(second_base);

    auto update_bases = lance::Operation::make_update_bases(bases);
    CHECK(update_bases.update_bases_count() == 2);
    auto first_view = update_bases.update_bases_at(0);
    CHECK(first_view.id == 7);
    CHECK(first_view.name == nullptr);
    CHECK(std::string(first_view.path) == base_a);
    auto second_view = update_bases.update_bases_at(1);
    CHECK(second_view.name != nullptr);
    CHECK(std::string(second_view.name).empty());
    CHECK(second_view.is_dataset_root);
    auto typed_bases = update_bases.update_bases();
    CHECK(typed_bases.size() == 2);
    CHECK(!typed_bases[0].name.has_value());
    CHECK(typed_bases[1].name.has_value());
    CHECK(typed_bases[1].name->empty());

    auto bases_committed = lance::Dataset::commit(
        uri, lance::Transaction::from_operation(6, update_bases));
    CHECK(bases_committed.version() == 7);
    const auto bases_json = bases_committed.base_paths_json();
    CHECK(bases_json.find(base_a) != std::string::npos);
    CHECK(bases_json.find(base_b) != std::string::npos);

    lance::BasePath conflict_base;
    conflict_base.path = base_a;
    auto conflict = lance::Operation::make_update_bases({conflict_base});
    bool conflict_rejected = false;
    try {
        auto failed = lance::Dataset::commit(
            uri, lance::Transaction::from_operation(7, conflict));
        (void)failed;
    } catch (const lance::Error& error) {
        conflict_rejected =
            error.code == LANCE_ERR_INVALID_ARGUMENT &&
            std::string(error.what()).find("Conflict detected") !=
                std::string::npos;
    }
    CHECK(conflict_rejected);

    auto empty_bases = lance::Operation::make_update_bases({});
    CHECK(empty_bases.update_bases_count() == 0);
    auto empty_committed = lance::Dataset::commit(
        uri, lance::Transaction::from_operation(7, empty_bases));
    CHECK(empty_committed.version() == 8);
    CHECK(empty_committed.base_paths_json() == bases_json);

    bool null_fragment_rejected = false;
    try {
        std::vector<const lance::Fragment*> nulls{nullptr};
        (void)lance::Operation::make_append(nulls);
    } catch (const lance::Error& error) {
        null_fragment_rejected = error.code == LANCE_ERR_INVALID_ARGUMENT;
    }
    CHECK(null_fragment_rejected);

    std::printf("C++ native operation construction OK\n");
}


static std::map<std::string, std::string> typed_arrow_metadata(
    const char* metadata) {
    std::map<std::string, std::string> out;
    if (metadata == nullptr) return out;
    const char* cursor = metadata;
    while (*cursor != '\0') {
        std::string key(cursor);
        cursor += key.size() + 1;
        std::string value(cursor);
        cursor += value.size() + 1;
        out.emplace(std::move(key), std::move(value));
    }
    return out;
}

static void check_typed_schema_metadata(const lance::Dataset& dataset,
                                        const std::string& key,
                                        const std::optional<std::string>& value) {
    ArrowSchema schema;
    std::memset(&schema, 0, sizeof(schema));
    dataset.schema(&schema);
    auto metadata = typed_arrow_metadata(schema.metadata);
    if (value) {
        CHECK(metadata.find(key) != metadata.end() && metadata.at(key) == *value);
    } else {
        CHECK(metadata.find(key) == metadata.end());
    }
    if (schema.release) schema.release(&schema);
}

static void check_typed_field_metadata(const lance::Dataset& dataset,
                                       size_t field_index,
                                       const std::string& key,
                                       const std::optional<std::string>& value) {
    ArrowSchema schema;
    std::memset(&schema, 0, sizeof(schema));
    dataset.schema(&schema);
    CHECK(field_index < static_cast<size_t>(schema.n_children));
    auto metadata = typed_arrow_metadata(schema.children[field_index]->metadata);
    if (value) {
        CHECK(metadata.find(key) != metadata.end() && metadata.at(key) == *value);
    } else {
        CHECK(metadata.find(key) == metadata.end());
    }
    if (schema.release) schema.release(&schema);
}

static void test_typed_native_operations(const std::string& source_uri,
                                         const std::string& base) {
    const std::string seed_uri = base + "/cpp_typed_seed";
    const std::string overwrite_uri = base + "/cpp_typed_overwrite";
    const std::string clone_uri = base + "/cpp_typed_clone";
    const std::string deep_uri = base + "/cpp_typed_clone_deep";
    const std::string rejection_uri = base + "/cpp_typed_rejection";

    auto source = lance::Dataset::open(source_uri);
    const uint64_t source_rows = source.count_rows();
    const uint64_t source_version = source.version();

    lance::UncommittedWriteOptions write_options;
    write_options.write.params.max_rows_per_file = 2;
    {
        auto seed = lance::Dataset::commit(
            seed_uri, write_uncommitted(source_uri, seed_uri,
                                        lance::WriteMode::Create, write_options));
        CHECK(seed.version() == 1);
        CHECK(seed.count_rows() == source_rows);
        CHECK(seed.fragment_count() >= 2);
    }

    auto seed = lance::Dataset::open(seed_uri);
    CHECK(seed.fragment_count() >= 2);
    auto seed_ids = seed.fragment_ids();
    std::vector<lance::Fragment> seed_fragments;
    std::vector<const lance::Fragment*> seed_fragment_ptrs;
    seed_fragments.reserve(seed_ids.size());
    for (uint64_t fragment_id : seed_ids) {
        seed_fragments.push_back(seed.open_fragment(fragment_id));
    }
    for (const auto& fragment : seed_fragments) {
        seed_fragment_ptrs.push_back(&fragment);
    }
    auto schema = seed.native_schema();
    const int32_t seed_id_field = schema.field_id("id");
    const int32_t seed_name_field = schema.field_id("name");
    CHECK(seed_id_field >= 0);
    CHECK(seed_name_field >= 0);

    lance::UpdateMap config(
        std::vector<std::string>{"owner", "empty"},
        std::vector<std::optional<std::string>>{std::string("alice"),
                                               std::string("")},
        true);
    std::vector<std::pair<std::string, std::string>> config_upserts = {
        {"owner", "alice"},
        {"empty", ""},
    };
    std::vector<lance::BasePath> bases;
    bases.push_back(lance::BasePath{9001, base + "/cpp_typed_base",
                                    std::optional<std::string>("primary"),
                                    false});
    auto overwrite = lance::Operation::make_overwrite(
        schema, seed_fragment_ptrs,
        std::optional<std::vector<std::pair<std::string, std::string>>>(
            config_upserts),
        std::optional<std::vector<lance::BasePath>>(bases));
    CHECK(overwrite.kind() == lance::OperationKind::Overwrite);
    CHECK(overwrite.overwrite_fragment_count() == seed_fragment_ptrs.size());
    CHECK(overwrite.has_config_upsert_values());
    CHECK(overwrite.config_upsert_count() == 2);
    std::map<std::string, std::string> overwrite_config;
    for (size_t index = 0; index < overwrite.config_upsert_count(); ++index) {
        overwrite_config.emplace(overwrite.config_upsert_key_at(index),
                                 overwrite.config_upsert_value_at(index));
    }
    CHECK(overwrite_config.at("owner") == "alice");
    CHECK(overwrite_config.at("empty").empty());
    CHECK(overwrite.has_initial_bases());
    CHECK(overwrite.initial_base_count() == 1);
    auto initial_bases = overwrite.initial_bases();
    CHECK(initial_bases.size() == 1);
    CHECK(initial_bases[0].id == 9001);
    CHECK(initial_bases[0].path == base + "/cpp_typed_base");
    CHECK(initial_bases[0].name == std::optional<std::string>("primary"));
    auto overwrite_schema = overwrite.overwrite_schema();
    CHECK(overwrite_schema.field_id("id") == seed_id_field);
    auto detached = overwrite.overwrite_fragment_at(1);
    CHECK(!detached.metadata_json().empty());

    bool wrong_kind_rejected = false;
    try {
        (void)overwrite.merge_fragment_count();
    } catch (const lance::Error& error) {
        wrong_kind_rejected = error.code == LANCE_ERR_NOT_SUPPORTED;
    }
    CHECK(wrong_kind_rejected);

    auto published = lance::Dataset::commit(
        overwrite_uri, lance::Transaction::from_operation(0, overwrite));
    CHECK(published.version() == 1);
    CHECK(published.count_rows() == source_rows);
    CHECK(published.fragment_count() == seed_fragment_ptrs.size());
    auto published_ids = published.fragment_ids();
    CHECK(published_ids.size() == seed_fragment_ptrs.size());
    for (size_t index = 0; index < published_ids.size(); ++index) {
        CHECK(published_ids[index] == index);
    }
    auto published_config = published.config();
    CHECK(published_config.at("owner") == "alice");
    CHECK(published_config.at("empty").empty());
    CHECK(published.base_paths_json().find("\"9001\"") != std::string::npos);
    CHECK(published.base_paths_json().find("primary") != std::string::npos);
    CHECK(published.native_schema().field_id("id") == seed_id_field);
    CHECK(published.native_schema().field_id("name") == seed_name_field);

    auto merge =
        lance::Operation::make_merge(schema, seed_fragment_ptrs, false);
    CHECK(merge.kind() == lance::OperationKind::Merge);
    CHECK(merge.merge_fragment_count() == seed_fragment_ptrs.size());
    CHECK(!merge.merge_preserves_nullability());
    CHECK(merge.merge_schema().field_id("id") == seed_id_field);
    auto merged = lance::Dataset::commit(
        seed_uri, lance::Transaction::from_operation(seed.version(), merge));
    CHECK(merged.version() == 2);
    CHECK(merged.count_rows() == source_rows);
    CHECK(merged.fragment_count() == seed_fragment_ptrs.size());

    auto shallow = lance::Operation::make_clone(
        true, std::nullopt, source_version, source_uri, std::nullopt);
    CHECK(shallow.kind() == lance::OperationKind::Clone);
    CHECK(shallow.clone_is_shallow());
    CHECK(!shallow.clone_ref_name().has_value());
    CHECK(shallow.clone_ref_version() == source_version);
    CHECK(shallow.clone_ref_path() == source_uri);
    CHECK(!shallow.clone_branch_name().has_value());
    auto cloned = lance::Dataset::commit(
        clone_uri,
        lance::Transaction::from_operation(source_version, shallow));
    // A shallow clone copies the source manifest, including its version.
    CHECK(cloned.version() == source_version);
    CHECK(cloned.count_rows() == source_rows);

    auto deep = lance::Operation::make_clone(
        false, std::nullopt, source_version, source_uri,
        std::optional<std::string>(""));
    CHECK(!deep.clone_is_shallow());
    CHECK(deep.clone_branch_name() == std::optional<std::string>(""));
    bool deep_rejected = false;
    try {
        (void)lance::Dataset::commit(
            deep_uri,
            lance::Transaction::from_operation(source_version, deep));
    } catch (const lance::Error& error) {
        deep_rejected =
            error.code == LANCE_ERR_INVALID_ARGUMENT &&
            std::string(error.what())
                    .find("deep Clone cannot be committed directly") !=
                std::string::npos;
    }
    CHECK(deep_rejected);

    lance::UpdateConfigOptions update_config_options;
    update_config_options.config_updates.emplace(
        std::vector<std::string>{"owner", "empty", "added"},
        std::vector<std::optional<std::string>>{std::nullopt, std::nullopt,
                                               std::string("yes")},
        true);
    update_config_options.table_metadata_updates.emplace(
        std::vector<std::string>{"table-key"},
        std::vector<std::optional<std::string>>{std::string("table-value")},
        false);
    update_config_options.schema_metadata_updates.emplace(
        std::vector<std::string>{"schema-key"},
        std::vector<std::optional<std::string>>{std::string("")}, false);
    update_config_options.field_metadata_updates.emplace(
        seed_id_field,
        lance::UpdateMap(
            std::vector<std::string>{"field-key", "field-old"},
            std::vector<std::optional<std::string>>{std::string("field-value"),
                                                   std::nullopt},
            true));
    auto update_config =
        lance::Operation::make_update_config(update_config_options);
    CHECK(update_config.kind() == lance::OperationKind::UpdateConfig);
    CHECK(update_config.config_updates().has_value());
    CHECK(update_config.config_updates()->count() == 3);
    CHECK(update_config.config_updates()->key_at(0) == "owner");
    CHECK(!update_config.config_updates()->value_at(0).has_value());
    CHECK(update_config.table_metadata_updates().has_value());
    CHECK(update_config.schema_metadata_updates().has_value());
    auto field_maps = update_config.field_metadata_updates();
    CHECK(field_maps.size() == 1);
    CHECK(field_maps.at(seed_id_field).count() == 2);

    auto updated = lance::Dataset::commit(
        overwrite_uri,
        lance::Transaction::from_operation(published.version(), update_config));
    CHECK(updated.version() == 2);
    auto updated_config = updated.config();
    CHECK(updated_config.find("owner") == updated_config.end());
    CHECK(updated_config.find("empty") == updated_config.end());
    CHECK(updated_config.at("added") == "yes");
    auto table_metadata = updated.metadata_json();
    CHECK(table_metadata.find("table-key") != std::string::npos);
    CHECK(table_metadata.find("table-value") != std::string::npos);
    check_typed_schema_metadata(updated, "schema-key", std::string(""));
    check_typed_field_metadata(updated, 0, "field-key",
                               std::string("field-value"));
    check_typed_field_metadata(updated, 0, "field-old", std::nullopt);

    CHECK(updated.delete_rows("id = 1") == 1);
    CHECK(updated.count_rows() == source_rows - 1);
    std::optional<lance::Fragment> deleted_fragment;
    auto updated_ids = updated.fragment_ids();
    for (uint64_t fragment_id : updated_ids) {
        auto candidate = updated.open_fragment(fragment_id);
        if (candidate.metadata_json().find("\"deletion_file\"") !=
            std::string::npos) {
            deleted_fragment.emplace(std::move(candidate));
            break;
        }
    }
    CHECK(deleted_fragment.has_value());
    auto updated_schema = updated.native_schema();
    auto deletion_overwrite = lance::Operation::make_overwrite(
        updated_schema, {&*deleted_fragment}, std::nullopt, std::nullopt);
    bool deletion_rejected = false;
    try {
        (void)lance::Dataset::commit(
            rejection_uri,
            lance::Transaction::from_operation(0, deletion_overwrite));
    } catch (const lance::Error& error) {
        deletion_rejected =
            error.code == LANCE_ERR_INVALID_ARGUMENT &&
            std::string(error.what()).find("carries deletion file") !=
                std::string::npos;
    }
    CHECK(deletion_rejected);

    lance::UpdateMap ordered(
        std::vector<std::string>{"dup", "deleted", "dup", "empty"},
        std::vector<std::optional<std::string>>{
            std::string("first"), std::nullopt, std::string("second"),
            std::string("")},
        true);
    CHECK(ordered.count() == 4);
    CHECK(ordered.replace());
    CHECK(ordered.key_at(0) == "dup");
    CHECK(ordered.key_at(2) == "dup");
    CHECK(!ordered.value_at(1).has_value());
    CHECK(ordered.value_at(3) == std::optional<std::string>(""));
    auto ordered_clone = ordered.clone();
    CHECK(ordered_clone.count() == 4);
    CHECK(ordered_clone.key_at(3) == "empty");
    bool missing_key_rejected = false;
    try {
        (void)lance::UpdateMap(
            std::vector<std::string>{"only-key"},
            std::vector<std::optional<std::string>>{}, false);
    } catch (const lance::Error& error) {
        missing_key_rejected = error.code == LANCE_ERR_INVALID_ARGUMENT;
    }
    CHECK(missing_key_rejected);
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: %s <source-dataset-uri> <base-path> "
                     "<native-schema-uri> <blob-publication-uri>\n",
                     argv[0]);
        return 2;
    }
    try {
        test_create_append_overwrite_and_batch(argv[1], argv[2]);
        test_detached_conflict_and_errors(argv[1], argv[2]);
        test_fragment_delete(argv[1], argv[2]);
        test_create_index_operation(argv[1], argv[2]);
        test_data_replacement(argv[1], argv[2]);

        test_native_schema(argv[3]);
        test_blob_writer_data_replacement(argv[4]);
        test_native_operations(argv[1], argv[2]);
        test_typed_native_operations(argv[1], argv[2]);
    } catch (const lance::Error& error) {
        std::fprintf(stderr, "lance::Error(%d): %s\n",
                     static_cast<int>(error.code), error.what());
        return 1;
    }
    std::printf("All C++ transaction tests passed!\n");
    return 0;
}
