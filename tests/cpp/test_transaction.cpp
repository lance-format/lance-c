// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include "lance/lance.hpp"

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

    auto first = dataset.open_fragment(ids[0]);
    auto second = dataset.open_fragment(ids[1]);
    auto no_delete = first.delete_if("false");
    CHECK(no_delete.has_value());
    CHECK(no_delete->id() == ids[0]);
    no_delete.reset();

    auto all_deleted = second.delete_if("true");
    CHECK(!all_deleted.has_value());
    CHECK(second.logical_rows() == 3);

    auto staged = first.delete_if("id = 3");
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
    } catch (const lance::Error& error) {
        std::fprintf(stderr, "lance::Error(%d): %s\n",
                     static_cast<int>(error.code), error.what());
        return 1;
    }
    std::printf("All C++ transaction tests passed!\n");
    return 0;
}
