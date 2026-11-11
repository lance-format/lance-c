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
    const auto seed_ids = seed.fragment_ids();
    const auto seed_max_id =
        *std::max_element(seed_ids.begin(), seed_ids.end());

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
          seed_max_id + 2 + reserved_fragments.size());

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

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: %s <source-dataset-uri> <base-path>\n", argv[0]);
        return 2;
    }
    try {
        test_create_append_overwrite_and_batch(argv[1], argv[2]);
        test_detached_conflict_and_errors(argv[1], argv[2]);
        test_fragment_delete(argv[1], argv[2]);
        test_create_index_operation(argv[1], argv[2]);
        test_data_replacement(argv[1], argv[2]);
        test_native_operations(argv[1], argv[2]);
    } catch (const lance::Error& error) {
        std::fprintf(stderr, "lance::Error(%d): %s\n",
                     static_cast<int>(error.code), error.what());
        return 1;
    }
    std::printf("All C++ transaction tests passed!\n");
    return 0;
}
