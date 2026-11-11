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
        CHECK(error.code == LANCE_ERR_INVALID_ARGUMENT);
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
        CHECK(error.code == LANCE_ERR_INVALID_ARGUMENT);
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

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: %s <source-dataset-uri> <base-path> "
                     "<native-schema-uri>\n",
                     argv[0]);
        return 2;
    }
    try {
        test_create_append_overwrite_and_batch(argv[1], argv[2]);
        test_detached_conflict_and_errors(argv[1], argv[2]);
        test_fragment_delete(argv[1], argv[2]);
        test_create_index_operation(argv[1], argv[2]);
        test_native_schema(argv[3]);
    } catch (const lance::Error& error) {
        std::fprintf(stderr, "lance::Error(%d): %s\n",
                     static_cast<int>(error.code), error.what());
        return 1;
    }
    std::printf("All C++ transaction tests passed!\n");
    return 0;
}
