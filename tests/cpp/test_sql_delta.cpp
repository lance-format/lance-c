// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

// Native dataset SQL and delta C++17 consumer exercising the RAII wrappers:
// chained builders, consume-on-build/into-stream handle release, exception
// safety, transaction array adoption and stream ownership across parent
// handle destruction.
//
// argv[1] = multi-fragment SQL dataset
// argv[2] = history dataset with stable row ids (five versions)
// argv[3] = blob dataset
// argv[4..7] = begin/end timestamps (seconds, nanos) of a history window
// argv[8..9] = the versions that window resolves to natively

#include "lance/lance.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__,        \
                         __LINE__, #condition);                                \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

namespace {

int column_index(const ArrowSchema& schema, const std::string& name) {
    for (int64_t i = 0; i < schema.n_children; i++) {
        const char* child = schema.children[i]->name;
        if (child && name == child) return static_cast<int>(i);
    }
    return -1;
}

int64_t field_count(lance::ArrowStream& stream) {
    ArrowSchema schema{};
    CHECK(stream->get_schema(stream.get(), &schema) == 0);
    const int64_t count = schema.n_children;
    schema.release(&schema);
    return count;
}

std::vector<int32_t> read_i32(lance::ArrowStream& stream, int column) {
    std::vector<int32_t> out;
    while (true) {
        ArrowArray batch{};
        CHECK(stream->get_next(stream.get(), &batch) == 0);
        if (!batch.release) break;
        CHECK(batch.n_children > column);
        const auto* values =
            static_cast<const int32_t*>(batch.children[column]->buffers[1]);
        for (int64_t row = 0; row < batch.length; row++) {
            out.push_back(values[row]);
        }
        batch.release(&batch);
    }
    return out;
}

std::vector<uint64_t> read_u64(lance::ArrowStream& stream, int column) {
    std::vector<uint64_t> out;
    while (true) {
        ArrowArray batch{};
        CHECK(stream->get_next(stream.get(), &batch) == 0);
        if (!batch.release) break;
        CHECK(batch.n_children > column);
        const auto* values =
            static_cast<const uint64_t*>(batch.children[column]->buffers[1]);
        for (int64_t row = 0; row < batch.length; row++) {
            out.push_back(values[row]);
        }
        batch.release(&batch);
    }
    return out;
}

std::vector<std::string> read_strings(lance::ArrowStream& stream,
                                      int column) {
    std::vector<std::string> out;
    while (true) {
        ArrowArray batch{};
        CHECK(stream->get_next(stream.get(), &batch) == 0);
        if (!batch.release) break;
        CHECK(batch.n_children > column);
        const ArrowArray* array = batch.children[column];
        const auto* offsets = static_cast<const int32_t*>(array->buffers[1]);
        const auto* data = static_cast<const char*>(array->buffers[2]);
        for (int64_t row = 0; row < batch.length; row++) {
            out.emplace_back(data + offsets[row],
                             static_cast<size_t>(offsets[row + 1] -
                                                 offsets[row]));
        }
        batch.release(&batch);
    }
    return out;
}

std::vector<int32_t> sorted(std::vector<int32_t> values) {
    std::sort(values.begin(), values.end());
    return values;
}

std::vector<uint64_t> sorted(std::vector<uint64_t> values) {
    std::sort(values.begin(), values.end());
    return values;
}

LanceErrorCode error_code_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const lance::Error& error) {
        return error.code;
    }
    return LANCE_OK;
}

std::string error_message_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const lance::Error& error) {
        return error.what();
    }
    return std::string();
}

void test_sql_builder_lifecycle(const std::string& uri) {
    lance::ArrowStream stream;
    {
        auto dataset = lance::Dataset::open(uri);
        auto builder = dataset.sql(
            "SELECT id, name FROM dataset WHERE id % 3 = 0 ORDER BY id DESC");
        builder.table_name("dataset").batch_size(4).batch_size_bytes(4096);
        auto query = builder.build();
        // into_stream consumes the query; build consumed the builder.
        stream = query.into_stream();
    }
    CHECK(static_cast<bool>(stream));
    CHECK(field_count(stream) == 2);
    // The stream owns its plan and outlives the dataset handle.
    auto ids = read_i32(stream, 0);
    CHECK((ids == std::vector<int32_t>{30, 27, 24, 21, 18, 15, 12, 9, 6, 3}));
    stream.reset();
    CHECK(!stream);
}

void test_sql_renamed_table(const std::string& uri) {
    auto dataset = lance::Dataset::open(uri);

    // The default table name does not resolve a renamed query, so planning
    // throws and the builder is already consumed.
    auto renamed = dataset.sql("SELECT id FROM orders ORDER BY id");
    renamed.table_name("orders");
    CHECK(error_code_of([&] { renamed.build(); }) ==
          LANCE_ERR_INVALID_ARGUMENT);

    auto stream = dataset.sql("SELECT id FROM orders ORDER BY id")
                      .table_name("orders")
                      .build()
                      .into_stream();
    auto ids = read_i32(stream, 0);
    CHECK(ids.size() == 30);
    CHECK(ids.front() == 1);
    CHECK(ids.back() == 30);
}

void test_sql_foreign_guards_keep_builder(const std::string& uri) {
    auto dataset = lance::Dataset::open(uri);
    auto builder = dataset.sql("SELECT id FROM dataset ORDER BY id LIMIT 2");

    // A foreign enum value is rejected at the setter, before native build,
    // and the builder keeps its previous state.
    CHECK(error_code_of([&] {
              builder.blob_handling(static_cast<LanceBlobHandling>(7));
          }) == LANCE_ERR_INVALID_ARGUMENT);
    builder.with_row_id(true);
    auto stream = builder.build().into_stream();
    auto ids = read_i32(stream, 0);
    CHECK((ids == std::vector<int32_t>{1, 2}));
}

void test_sql_system_columns(const std::string& uri) {
    // Stable row ids follow insertion order: id 1 -> row id 0, id 2 -> 1,
    // id 4 -> 3; id 3 was deleted at v4 so its row id 2 is no longer live.
    auto dataset = lance::Dataset::open(uri);
    auto query =
        dataset
            .sql("SELECT id, name FROM dataset WHERE id <= 4 ORDER BY id")
            .with_row_id(true)
            .build();
    auto stream = query.into_stream();

    ArrowSchema schema{};
    CHECK(stream->get_schema(stream.get(), &schema) == 0);
    const int name_index = column_index(schema, "name");
    const int row_id_index = column_index(schema, "_rowid");
    schema.release(&schema);
    CHECK(name_index == 1);
    CHECK(row_id_index == 2);

    // The updated name and the row ids prove the live-row history.
    CHECK((read_strings(stream, name_index) ==
           std::vector<std::string>{"a", "updated-2", "d"}));
}

void test_sql_aggregates_skip_injection(const std::string& uri) {
    auto dataset = lance::Dataset::open(uri);
    auto query = dataset.sql("SELECT COUNT(*) AS n FROM dataset")
                     .with_row_id(true)
                     .with_row_addr(true)
                     .build();
    auto stream = query.into_stream();
    ArrowSchema schema{};
    CHECK(stream->get_schema(stream.get(), &schema) == 0);
    CHECK(schema.n_children == 1);
    CHECK(column_index(schema, "_rowid") == -1);
    CHECK(column_index(schema, "_rowaddr") == -1);
    schema.release(&schema);

    ArrowArray batch{};
    CHECK(stream->get_next(stream.get(), &batch) == 0);
    CHECK(batch.release != nullptr);
    CHECK(batch.length == 1);
    const auto* counts = static_cast<const int64_t*>(batch.children[0]->buffers[1]);
    CHECK(counts[0] == 6);
    batch.release(&batch);
}

void test_sql_blob_handling(const std::string& uri) {
    lance::ArrowStream stream;
    {
        auto dataset = lance::Dataset::open(uri);
        stream = dataset.sql("SELECT id, blob FROM dataset ORDER BY id")
                     .blob_handling(LANCE_BLOB_HANDLING_ALL_BINARY)
                     .build()
                     .into_stream();
    }
    ArrowSchema schema{};
    CHECK(stream->get_schema(stream.get(), &schema) == 0);
    CHECK(schema.n_children == 2);
    CHECK(std::string(schema.children[1]->format) == "Z");
    schema.release(&schema);

    ArrowArray batch{};
    CHECK(stream->get_next(stream.get(), &batch) == 0);
    CHECK(batch.release != nullptr);
    CHECK(batch.length == 3);
    CHECK(batch.children[1]->n_buffers == 3);
    const auto* offsets = static_cast<const int64_t*>(batch.children[1]->buffers[1]);
    const auto* values = static_cast<const char*>(batch.children[1]->buffers[2]);
    CHECK(offsets[1] - offsets[0] == 11);
    CHECK(std::memcmp(values + offsets[0], "inline-blob", 11) == 0);
    batch.release(&batch);
}

void test_sql_errors(const std::string& uri) {
    auto dataset = lance::Dataset::open(uri);
    auto builder = dataset.sql("SELECT * FROM does_not_exist");
    CHECK(error_code_of([&] { builder.build(); }) ==
          LANCE_ERR_INVALID_ARGUMENT);

    // A runtime failure only surfaces while reading the stream.
    auto query =
        dataset.sql("SELECT id / (id - 1) AS boom FROM dataset").build();
    auto stream = query.into_stream();
    ArrowArray batch{};
    CHECK(stream->get_next(stream.get(), &batch) != 0);
    CHECK(batch.release == nullptr);
}

lance::DatasetDelta build_versions(const lance::Dataset& dataset,
                                   uint64_t begin, uint64_t end) {
    return dataset.delta().begin_version(begin).end_version(end).build();
}

void test_delta_full_range(const std::string& uri) {
    auto inserted = lance::ArrowStream();
    auto updated = lance::ArrowStream();
    auto upserted = lance::ArrowStream();
    auto deleted = lance::ArrowStream();
    {
        auto dataset = lance::Dataset::open(uri);
        auto delta = build_versions(dataset, 0, 5);
        inserted = delta.get_inserted_rows();
        updated = delta.get_updated_rows();
        upserted = delta.get_upserted_rows();
        deleted = delta.get_deleted_row_ids();
        // Every stream keeps its own native state after both parents close:
        // the delta and the dataset handle go out of scope here.
    }

    CHECK(field_count(inserted) == 6);
    CHECK(field_count(deleted) == 1);
    // Row 3 was deleted at version 4, so it is absent from the end snapshot.
    const std::vector<int32_t> all{1, 2, 4, 5, 6, 7};
    CHECK(sorted(read_i32(inserted, 0)) == all);
    CHECK(read_i32(updated, 0).empty());
    CHECK(sorted(read_i32(upserted, 0)) == all);
    CHECK(read_u64(deleted, 0).empty());
}

void test_delta_compared_version_transactions(const std::string& uri) {
    auto dataset = lance::Dataset::open(uri);
    auto delta = dataset.delta().compared_against_version(2).build();

    {
        auto transactions = delta.list_transactions();
        CHECK(transactions.size() == 3);
        // A transaction records the version it read, so the transactions of
        // versions 3, 4 and 5 read versions 2, 3 and 4.
        CHECK(transactions.front().read_version() == 2);
        CHECK(transactions.back().read_version() == 4);
        CHECK(!transactions.front().uuid().empty());
    }

    // Adoption keeps each element alive after the shared array guard frees
    // the untouched remainder, including through vector moves.
    auto transactions = delta.list_transactions();
    std::vector<lance::Transaction> adopted;
    adopted.reserve(transactions.size());
    for (auto& transaction : transactions) {
        adopted.push_back(std::move(transaction));
    }
    transactions.clear();
    transactions.shrink_to_fit();
    CHECK(adopted.size() == 3);
    for (size_t i = 0; i < adopted.size(); i++) {
        CHECK(adopted[i].read_version() == i + 2);
        CHECK(!adopted[i].uuid().empty());
    }
    adopted.clear();

    auto inserted = delta.get_inserted_rows();
    // Row 6 was created at version 2, so the exclusive begin excludes it.
    CHECK((sorted(read_i32(inserted, 0)) == std::vector<int32_t>{7}));
    auto updated = delta.get_updated_rows();
    CHECK((sorted(read_i32(updated, 0)) == std::vector<int32_t>{2, 6}));
    auto deleted = delta.get_deleted_row_ids();
    CHECK((read_u64(deleted, 0) == std::vector<uint64_t>{2}));
}

void test_delta_date_mode(const std::string& uri, int64_t begin_seconds,
                          uint32_t begin_nanos, int64_t end_seconds,
                          uint32_t end_nanos, uint64_t resolved_begin,
                          uint64_t resolved_end) {
    auto dataset = lance::Dataset::open(uri);
    LanceDeltaTimestamp begin{begin_seconds, begin_nanos};
    LanceDeltaTimestamp end{end_seconds, end_nanos};

    auto by_date =
        dataset.delta().begin_timestamp(begin).end_timestamp(end).build();
    auto by_version = build_versions(dataset, resolved_begin, resolved_end);

    auto date_inserted = by_date.get_inserted_rows();
    auto version_inserted = by_version.get_inserted_rows();
    // Row order is a plan detail, so compare the change sets themselves.
    CHECK(sorted(read_i32(date_inserted, 0)) ==
          sorted(read_i32(version_inserted, 0)));

    auto date_deleted = by_date.get_deleted_row_ids();
    auto version_deleted = by_version.get_deleted_row_ids();
    CHECK(sorted(read_u64(date_deleted, 0)) == sorted(read_u64(version_deleted, 0)));
}

void test_delta_errors(const std::string& history_uri,
                       const std::string& plain_uri) {
    auto dataset = lance::Dataset::open(history_uri);

    // Native range-combination validation runs synchronously on build.
    CHECK(error_code_of([&] { dataset.delta().build(); }) ==
          LANCE_ERR_INVALID_ARGUMENT);
    CHECK(error_code_of([&] { dataset.delta().begin_version(1).build(); }) ==
          LANCE_ERR_INVALID_ARGUMENT);
    CHECK(error_code_of([&] {
              dataset.delta().compared_against_version(2).begin_version(1).build();
          }) == LANCE_ERR_INVALID_ARGUMENT);

    // Foreign timestamp guards preserve the configured comparison.
    auto builder = dataset.delta();
    builder.compared_against_version(1);
    CHECK(error_code_of([&] {
              builder.begin_timestamp(LanceDeltaTimestamp{INT64_MAX, 0});
          }) == LANCE_ERR_INVALID_ARGUMENT);
    auto delta = builder.build();
    auto inserted = delta.get_inserted_rows();
    CHECK((sorted(read_i32(inserted, 0)) == std::vector<int32_t>{5, 6, 7}));

    // A reversed explicit range rejects deleted ids natively while row-change
    // streams stay empty; the delta remains usable afterwards.
    auto reversed = build_versions(dataset, 5, 2);
    auto empty_inserted = reversed.get_inserted_rows();
    CHECK(read_i32(empty_inserted, 0).empty());
    CHECK(error_code_of([&] { reversed.get_deleted_row_ids(); }) ==
          LANCE_ERR_INVALID_ARGUMENT);
    auto empty_updated = reversed.get_updated_rows();
    CHECK(read_i32(empty_updated, 0).empty());

    auto plain = lance::Dataset::open(plain_uri);
    auto plain_delta = build_versions(plain, 1, 2);
    auto message = error_message_of([&] { plain_delta.get_deleted_row_ids(); });
    CHECK(message.find("stable row ids") != std::string::npos);
    auto plain_inserted = plain_delta.get_inserted_rows();
    // Row changes filter on row-version lineage, which a write without stable
    // row ids does not record, so a plain dataset reports no changes.
    CHECK(sorted(read_i32(plain_inserted, 0)).empty());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 10) {
        std::fprintf(stderr,
                     "usage: %s <sql-dataset> <history-dataset> <blob-dataset> "
                     "<begin-sec> <begin-nanos> <end-sec> <end-nanos> "
                     "<resolved-begin> <resolved-end>\n",
                     argv[0]);
        return 2;
    }
    const std::string sql_uri = argv[1];
    const std::string history_uri = argv[2];
    const std::string blob_uri = argv[3];
    const auto begin_seconds = std::strtoll(argv[4], nullptr, 10);
    const auto begin_nanos =
        static_cast<uint32_t>(std::strtoul(argv[5], nullptr, 10));
    const auto end_seconds = std::strtoll(argv[6], nullptr, 10);
    const auto end_nanos =
        static_cast<uint32_t>(std::strtoul(argv[7], nullptr, 10));
    const auto resolved_begin = std::strtoull(argv[8], nullptr, 10);
    const auto resolved_end = std::strtoull(argv[9], nullptr, 10);

    test_sql_builder_lifecycle(sql_uri);
    test_sql_renamed_table(sql_uri);
    test_sql_foreign_guards_keep_builder(sql_uri);
    test_sql_system_columns(history_uri);
    test_sql_aggregates_skip_injection(history_uri);
    test_sql_blob_handling(blob_uri);
    test_sql_errors(sql_uri);
    test_delta_full_range(history_uri);
    test_delta_compared_version_transactions(history_uri);
    test_delta_date_mode(history_uri, begin_seconds, begin_nanos, end_seconds,
                         end_nanos, resolved_begin, resolved_end);
    test_delta_errors(history_uri, sql_uri);
    std::printf("test_sql_delta.cpp: all checks passed\n");
    return 0;
}
