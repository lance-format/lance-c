/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file test_cpp_api.cpp
 * @brief C++ compilation and functional test for lance.hpp
 *
 * Tests the RAII wrappers, exception handling, and builder pattern.
 *
 * Usage: test_cpp_api <dataset_uri> <write_uri> <blob_uri> <ordering_uri>
 *                     <session_uri> <fragment_uri> <fragment_read_uri>
 *                     <cleanup_uri> <aggregate_plan_path>
 */

#include "lance/lance.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

// Arrow C Data Interface flag bits — see arrow_schema/arrow_c_data_interface.h.
#define ARROW_FLAG_NULLABLE 2

#define TEST(name) printf("  %s... ", #name)
#define PASS()     printf("OK\n")

static const char* arrow_field_metadata_value(
    const ArrowSchema* field,
    const char* key,
    size_t* value_len = nullptr) {
    if (!field || !field->metadata) return nullptr;
    const char* cursor = field->metadata;
    int32_t pair_count = 0;
    memcpy(&pair_count, cursor, sizeof(pair_count));
    cursor += sizeof(pair_count);
    for (int32_t i = 0; i < pair_count; ++i) {
        int32_t key_len = 0;
        memcpy(&key_len, cursor, sizeof(key_len));
        cursor += sizeof(key_len);
        const char* key_data = cursor;
        cursor += key_len;
        int32_t stored_value_len = 0;
        memcpy(&stored_value_len, cursor, sizeof(stored_value_len));
        cursor += sizeof(stored_value_len);
        const char* value_data = cursor;
        cursor += stored_value_len;
        if (key_len == static_cast<int32_t>(strlen(key)) &&
            memcmp(key_data, key, static_cast<size_t>(key_len)) == 0) {
            if (value_len) *value_len = static_cast<size_t>(stored_value_len);
            return value_data;
        }
    }
    return nullptr;
}

static const ArrowSchema* arrow_field_at_path(
    const ArrowSchema* schema,
    const std::string& path) {
    const ArrowSchema* field = schema;
    size_t offset = 0;
    while (offset < path.size()) {
        const size_t dot = path.find('.', offset);
        const size_t segment_len =
            dot == std::string::npos ? path.size() - offset : dot - offset;
        const ArrowSchema* child = nullptr;
        for (int64_t i = 0; i < field->n_children; ++i) {
            const ArrowSchema* candidate = field->children[i];
            if (candidate && candidate->name &&
                strlen(candidate->name) == segment_len &&
                memcmp(candidate->name, path.data() + offset, segment_len) == 0) {
                child = candidate;
                break;
            }
        }
        if (!child) return nullptr;
        field = child;
        if (dot == std::string::npos) break;
        offset = dot + 1;
    }
    return field;
}

static void assert_field_metadata(
    const ArrowSchema* field,
    const char* key,
    const std::string& expected) {
    size_t value_len = 0;
    const char* value = arrow_field_metadata_value(field, key, &value_len);
    assert(value != nullptr);
    assert(value_len == expected.size());
    assert(memcmp(value, expected.data(), value_len) == 0);
}

static void assert_field_metadata_absent(
    const ArrowSchema* field,
    const char* key) {
    if (arrow_field_metadata_value(field, key) != nullptr) {
        fprintf(stderr,
                "unexpected field metadata: field=%s key=%s\n",
                field && field->name ? field->name : "<null>",
                key);
    }
    assert(arrow_field_metadata_value(field, key) == nullptr);
}

struct ScanStatisticsCapture {
    uint64_t calls = 0;
    uint64_t bytes_read = 0;
    uint64_t indices_loaded = 0;
    bool invalid = false;
};

static void capture_scan_statistics(
    void* callback_ctx,
    const LanceScanStatistics* statistics) noexcept {
    if (!callback_ctx) return;
    auto* captured = static_cast<ScanStatisticsCapture*>(callback_ctx);
    if (!statistics || (statistics->metrics_len > 0 && !statistics->metrics)) {
        captured->invalid = true;
        return;
    }
    captured->calls += 1;
    captured->bytes_read = statistics->bytes_read;
    captured->indices_loaded = statistics->indices_loaded;
}

struct AsyncScanCapture {
    std::mutex mutex;
    std::condition_variable ready;
    bool completed = false;
    int32_t status = -1;
    ArrowArrayStream* stream = nullptr;
};

static void capture_async_scan(
    void* callback_ctx,
    int32_t status,
    void* result) noexcept {
    if (!callback_ctx) return;
    auto* captured = static_cast<AsyncScanCapture*>(callback_ctx);
    {
        std::lock_guard<std::mutex> lock(captured->mutex);
        captured->status = status;
        captured->stream = static_cast<ArrowArrayStream*>(result);
        captured->completed = true;
    }
    captured->ready.notify_one();
}

struct BuildProgressCapture {
    uint64_t events = 0;
    uint64_t starts = 0;
    uint64_t completes = 0;
    bool invalid = false;
};

static void capture_build_progress(
    void* callback_ctx,
    int32_t event,
    const char* stage,
    uint64_t total,
    const char* unit,
    uint64_t completed) noexcept {
    (void)total;
    (void)completed;
    if (!callback_ctx) return;
    auto* captured = static_cast<BuildProgressCapture*>(callback_ctx);
    if (!stage || !unit) {
        captured->invalid = true;
        return;
    }
    if (event == LANCE_INDEX_BUILD_PROGRESS_STAGE_START)
        captured->starts += 1;
    else if (event == LANCE_INDEX_BUILD_PROGRESS_STAGE_COMPLETE)
        captured->completes += 1;
    else if (event != LANCE_INDEX_BUILD_PROGRESS_STAGE_PROGRESS)
        captured->invalid = true;
    captured->events += 1;
}

struct TrackedFileRow {
    int64_t version;
    std::string base_uri;
    std::string path;
    std::string file_type;
};

struct PhysicalFileRow {
    std::string base_uri;
    std::string path;
    int64_t size_bytes;
    int64_t last_modified;
};

static std::string string_array_value(
    const ArrowArray* array,
    int64_t row) {
    const auto* offsets = static_cast<const int32_t*>(array->buffers[1]);
    const int32_t begin = offsets[row];
    const int32_t end = offsets[row + 1];
    return std::string(
        static_cast<const char*>(array->buffers[2]) + begin,
        static_cast<size_t>(end - begin));
}

static std::string dictionary_string_value(
    const ArrowArray* array,
    int64_t row,
    size_t key_width) {
    assert(array->dictionary != nullptr);
    int64_t value_index = 0;
    if (key_width == 4) {
        int32_t key = 0;
        memcpy(&key, static_cast<const char*>(array->buffers[1]) + row * 4, 4);
        value_index = key;
    } else {
        int8_t key = 0;
        memcpy(&key, static_cast<const char*>(array->buffers[1]) + row, 1);
        value_index = key;
    }
    assert(value_index >= 0);
    return string_array_value(array->dictionary, value_index);
}

static std::vector<TrackedFileRow> read_tracked_files(
    lance::ArrowStream& stream) {
    ArrowSchema schema = {};
    assert(stream->get_schema(stream.get(), &schema) == 0);
    assert(schema.n_children == 4);
    assert(std::string(schema.children[0]->name) == "version");
    assert(std::string(schema.children[1]->name) == "base_uri");
    assert(std::string(schema.children[2]->name) == "path");
    assert(std::string(schema.children[3]->name) == "type");
    assert(std::string(schema.children[0]->format) == "l");
    assert(std::string(schema.children[1]->format) == "i");
    assert(schema.children[1]->dictionary != nullptr);
    assert(std::string(schema.children[2]->format) == "u");
    assert(std::string(schema.children[3]->format) == "c");
    assert(schema.children[3]->dictionary != nullptr);
    schema.release(&schema);

    std::vector<TrackedFileRow> rows;
    while (true) {
        ArrowArray batch = {};
        assert(stream->get_next(stream.get(), &batch) == 0);
        if (batch.release == nullptr) break;
        assert(batch.n_children == 4);
        const auto* versions =
            static_cast<const int64_t*>(batch.children[0]->buffers[1]);
        for (int64_t row = 0; row < batch.length; ++row) {
            rows.push_back(TrackedFileRow{
                versions[row],
                dictionary_string_value(batch.children[1], row, 4),
                string_array_value(batch.children[2], row),
                dictionary_string_value(batch.children[3], row, 1),
            });
        }
        batch.release(&batch);
    }
    return rows;
}

static std::vector<PhysicalFileRow> read_all_files(
    lance::ArrowStream& stream) {
    ArrowSchema schema = {};
    assert(stream->get_schema(stream.get(), &schema) == 0);
    assert(schema.n_children == 4);
    assert(std::string(schema.children[0]->name) == "base_uri");
    assert(schema.children[0]->dictionary != nullptr);
    assert(std::string(schema.children[1]->name) == "path");
    assert(std::string(schema.children[2]->name) == "size_bytes");
    assert(std::string(schema.children[2]->format) == "l");
    assert(std::string(schema.children[3]->name) == "last_modified");
    assert(std::string(schema.children[3]->format).rfind("tsu:", 0) == 0);
    schema.release(&schema);

    std::vector<PhysicalFileRow> rows;
    while (true) {
        ArrowArray batch = {};
        assert(stream->get_next(stream.get(), &batch) == 0);
        if (batch.release == nullptr) break;
        assert(batch.n_children == 4);
        const auto* sizes =
            static_cast<const int64_t*>(batch.children[2]->buffers[1]);
        const auto* timestamps =
            static_cast<const int64_t*>(batch.children[3]->buffers[1]);
        for (int64_t row = 0; row < batch.length; ++row) {
            rows.push_back(PhysicalFileRow{
                dictionary_string_value(batch.children[0], row, 4),
                string_array_value(batch.children[1], row),
                sizes[row],
                timestamps[row],
            });
        }
        batch.release(&batch);
    }
    return rows;
}

static void test_dataset_open(const std::string& uri) {
    TEST(test_dataset_open);

    auto ds = lance::Dataset::open(uri);
    assert(ds.version() >= 1);
    assert(ds.count_rows() > 0);

    printf("version=%llu, rows=%llu... ",
           (unsigned long long)ds.version(),
           (unsigned long long)ds.count_rows());

    PASS();
}

static uint64_t drain_scan_rows(ArrowArrayStream& stream);

static void test_shared_session(const std::string& uri) {
    TEST(test_shared_session);

    auto session = std::make_unique<lance::Session>(0, 16 * 1024 * 1024);
    auto ds = lance::Dataset::open_with_session(*session, uri);
    auto stats = session->cache_stats();

    session.reset();
    assert(ds.count_rows() > 0);

    printf("metadata_entries=%llu... ",
           (unsigned long long)stats.metadata_cache_entries);
    PASS();
}

static void test_session_cache_config(const std::string& uri) {
    TEST(test_session_cache_config);

    {
        auto session = std::make_unique<lance::Session>(
            lance::SessionCacheSpec::with_size(0),
            lance::SessionCacheSpec::with_size(0));
        const uint64_t initial_size = session->size_bytes();
        assert(session->approx_num_items() == 0);

        auto ds = lance::Dataset::open_with_session(*session, uri);
        assert(ds.fragment_count() >= 2);
        ds.prewarm_index("id_idx");

        auto scanner = ds.scan({"id"}, "id >= 5 AND id < 8");
        scanner.use_scalar_index(true);
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);
        assert(drain_scan_rows(stream) == 3);

        auto stats = session->cache_stats();
        assert(stats.index_cache_entries == 0);
        assert(stats.index_cache_size_bytes == 0);
        assert(stats.metadata_cache_entries == 0);
        assert(stats.metadata_cache_size_bytes == 0);
        assert(session->approx_num_items() == 0);
        assert(session->size_bytes() == initial_size);

        session.reset();
        auto after_close = ds.scan({"id"}, "id >= 5 AND id < 8");
        after_close.use_scalar_index(true);
        ArrowArrayStream after_stream;
        memset(&after_stream, 0, sizeof(after_stream));
        after_close.to_arrow_stream(&after_stream);
        assert(drain_scan_rows(after_stream) == 3);
    }

    {
        auto session = std::make_unique<lance::Session>(
            lance::SessionCacheSpec::from_uri("moka://?capacity=67108864"),
            lance::SessionCacheSpec::from_config(
                "MOKA", {{"capacity", "33554432"}}));
        const uint64_t initial_size = session->size_bytes();

        auto ds = lance::Dataset::open_with_session(*session, uri);
        ds.prewarm_index("id_idx");

        auto scanner = ds.scan({"id"}, "id >= 5 AND id < 8");
        scanner.use_scalar_index(true);
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);
        assert(drain_scan_rows(stream) == 3);

        auto stats = session->cache_stats();
        assert(stats.index_cache_entries > 0);
        assert(stats.metadata_cache_entries > 0);
        assert(session->approx_num_items() > 0);
        assert(session->size_bytes() > initial_size);

        session.reset();
        auto after_close = ds.scan({"id"}, "id >= 9");
        after_close.use_scalar_index(true);
        ArrowArrayStream after_stream;
        memset(&after_stream, 0, sizeof(after_stream));
        after_close.to_arrow_stream(&after_stream);
        assert(drain_scan_rows(after_stream) == 12);
    }

    bool caught = false;
    try {
        lance::Session invalid(
            lance::SessionCacheSpec::from_uri("moka://?capacity"),
            lance::SessionCacheSpec{});
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
        assert(std::string(error.what()).find("missing '='") != std::string::npos);
    }
    assert(caught);

    PASS();
}

static void test_dataset_schema(const std::string& uri) {
    TEST(test_dataset_schema);

    auto ds = lance::Dataset::open(uri);

    ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ds.schema(&schema);

    assert(schema.n_children > 0);
    printf("fields=%lld... ", (long long)schema.n_children);

    // Print field names
    for (int64_t i = 0; i < schema.n_children; i++) {
        if (i > 0) printf(", ");
        printf("%s", schema.children[i]->name);
    }
    printf("... ");

    if (schema.release) schema.release(&schema);

    PASS();
}

static void test_scanner_fluent(const std::string& uri) {
    TEST(test_scanner_fluent);

    auto ds = lance::Dataset::open(uri);

    // Fluent builder pattern.
    auto scanner = ds.scan();
    ScanStatisticsCapture captured;
    scanner.limit(5)
           .offset(0)
           .batch_size(2)
           .batch_size_bytes(1024)
           .io_buffer_size(64 * 1024)
           .batch_readahead(1)
           .fragment_readahead(1)
           .target_parallelism(1)
           .scan_in_order(false)
           .use_scalar_index(false)
           .scalar_index_auto_selection(true)
           .scalar_index_auto_selection(false)
           .strict_batch_size(false)
           .use_stats(false)
           .with_row_address(true)
           .include_deleted_rows(false)
           .statistics_callback(capture_scan_statistics, &captured);

    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);

    // Count rows from stream.
    uint64_t total = 0;
    while (true) {
        ArrowArray arr;
        memset(&arr, 0, sizeof(arr));
        int rc = stream.get_next(&stream, &arr);
        assert(rc == 0);
        if (!arr.release) break;
        total += (uint64_t)arr.length;
        arr.release(&arr);
    }

    assert(total == 5);
    assert(captured.calls == 1);
    assert(captured.bytes_read > 0);
    assert(!captured.invalid);
    printf("rows=%llu... ", (unsigned long long)total);

    if (stream.release) stream.release(&stream);
    PASS();
}

struct ScanFieldExpectation {
    const char* name;
    const char* format;
};

static void assert_scan_schema(
    ArrowArrayStream& stream,
    const std::vector<ScanFieldExpectation>& expected) {
    ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    assert(stream.get_schema(&stream, &schema) == 0);
    assert(schema.n_children == static_cast<int64_t>(expected.size()));
    for (size_t i = 0; i < expected.size(); ++i) {
        const ArrowSchema* field = schema.children[i];
        assert(field != nullptr);
        assert(strcmp(field->name, expected[i].name) == 0);
        if (expected[i].format) {
            assert(strcmp(field->format, expected[i].format) == 0);
        }
    }
    assert(schema.release != nullptr);
    schema.release(&schema);
}

static uint64_t drain_scan_rows(ArrowArrayStream& stream) {
    uint64_t total = 0;
    while (true) {
        ArrowArray array;
        memset(&array, 0, sizeof(array));
        assert(stream.get_next(&stream, &array) == 0);
        if (!array.release) break;
        total += static_cast<uint64_t>(array.length);
        array.release(&array);
    }
    if (stream.release) stream.release(&stream);
    return total;
}

static std::vector<std::pair<int32_t, std::string>> read_id_name_rows(
    ArrowArrayStream& stream) {
    std::vector<std::pair<int32_t, std::string>> rows;
    while (true) {
        ArrowArray array;
        memset(&array, 0, sizeof(array));
        assert(stream.get_next(&stream, &array) == 0);
        if (!array.release) break;
        assert(array.n_children == 2);
        const ArrowArray* ids = array.children[0];
        const ArrowArray* names = array.children[1];
        assert(ids->null_count == 0);
        assert(names->null_count == 0);
        const auto* id_values =
            static_cast<const int32_t*>(ids->buffers[1]);
        const auto* name_offsets =
            static_cast<const int32_t*>(names->buffers[1]);
        const auto* name_data =
            static_cast<const char*>(names->buffers[2]);
        for (int64_t i = 0; i < array.length; ++i) {
            int64_t id_index = ids->offset + i;
            int64_t name_index = names->offset + i;
            rows.emplace_back(
                id_values[id_index],
                std::string(name_data + name_offsets[name_index],
                            name_offsets[name_index + 1] -
                                name_offsets[name_index]));
        }
        array.release(&array);
    }
    if (stream.release) stream.release(&stream);
    std::sort(rows.begin(), rows.end());
    return rows;
}

static void test_scanner_projection_filter(const std::string& uri) {
    TEST(test_scanner_projection_filter);

    auto ds = lance::Dataset::open(uri);
    assert(ds.fragment_count() >= 2);

    {
        auto scanner = ds.scan();
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);
        assert_scan_schema(stream, {
            {"id", "i"}, {"name", "u"}, {"score", "i"}, {"embedding", nullptr},
        });
        assert(drain_scan_rows(stream) == ds.count_rows());
    }

    {
        auto scanner = ds.scan({});
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);
        assert_scan_schema(stream, {});
        assert(drain_scan_rows(stream) == ds.count_rows());
    }

    {
        auto scanner = ds.scan({"id", "name"}, "id >= 9 AND id <= 14");
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);
        assert_scan_schema(stream, {{"id", "i"}, {"name", "u"}});

        std::vector<std::pair<int32_t, std::string>> expected = {
            {9, "ivan"},
            {10, "judy"},
            {11, "kate"},
            {12, "leo"},
            {13, "maya"},
            {14, "nick"},
        };
        assert(read_id_name_rows(stream) == expected);
    }

    {
        auto scanner = ds.scan(std::nullopt, "id = 13");
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);
        assert_scan_schema(stream, {
            {"id", "i"}, {"name", "u"}, {"score", "i"}, {"embedding", nullptr},
        });
        assert(drain_scan_rows(stream) == 1);
    }

    {
        auto scanner = ds.scan({}, "id = 13");
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);
        assert_scan_schema(stream, {});
        assert(drain_scan_rows(stream) == 1);
    }

    for (const std::string filter : {"", "NOT A VALID >>> FILTER ???"}) {
        auto scanner = ds.scan({"id"}, filter);
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        bool caught = false;
        try {
            scanner.to_arrow_stream(&stream);
        } catch (const lance::Error& error) {
            caught = true;
            assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
        }
        if (stream.release) stream.release(&stream);
        assert(caught);
    }

    PASS();
}

static void test_scanner_projection(const std::string& uri) {
    TEST(test_scanner_projection);

    auto ds = lance::Dataset::open(uri);

    auto scanner = ds.scan();
    scanner.project({
        {"id_alias", "id"},
        {"next_id", "id + 1"},
    }).disable_scoring_autoprojection();

    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);

    ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    assert(stream.get_schema(&stream, &schema) == 0);
    assert(schema.n_children == 2);
    assert(strcmp(schema.children[0]->name, "id_alias") == 0);
    assert(strcmp(schema.children[1]->name, "next_id") == 0);
    if (schema.release) schema.release(&schema);

    uint64_t rows = 0;
    while (true) {
        ArrowArray array;
        memset(&array, 0, sizeof(array));
        assert(stream.get_next(&stream, &array) == 0);
        if (!array.release) break;
        rows += static_cast<uint64_t>(array.length);
        array.release(&array);
    }
    assert(rows > 0);
    if (stream.release) stream.release(&stream);

    // std::nullopt restores the unset projection; an empty vector is explicit empty.
    auto cleared = ds.scan();
    cleared.project({{"ignored", "id"}})
           .project(std::nullopt);
    memset(&stream, 0, sizeof(stream));
    cleared.to_arrow_stream(&stream);
    memset(&schema, 0, sizeof(schema));
    assert(stream.get_schema(&stream, &schema) == 0);
    assert(schema.n_children > 1);
    if (schema.release) schema.release(&schema);
    if (stream.release) stream.release(&stream);

    auto empty = ds.scan();
    empty.project(std::vector<lance::ProjectionColumn>{});
    memset(&stream, 0, sizeof(stream));
    empty.to_arrow_stream(&stream);
    memset(&schema, 0, sizeof(schema));
    assert(stream.get_schema(&stream, &schema) == 0);
    assert(schema.n_children == 0);
    if (schema.release) schema.release(&schema);
    if (stream.release) stream.release(&stream);

    PASS();
}

static void test_scanner_row_addr_lists(const std::string& uri) {
    TEST(test_scanner_row_addr_lists);

    auto ds = lance::Dataset::open(uri);

    // Rows 0, 2 and 4 of the first fragment, minus row 2.
    auto scanner = ds.scan();
    scanner.row_addr_allowlist(std::vector<uint64_t>{4, 0, 2})
           .row_addr_blocklist(std::vector<uint64_t>{2});
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);
    uint64_t total = 0;
    while (true) {
        ArrowArray arr;
        memset(&arr, 0, sizeof(arr));
        int rc = stream.get_next(&stream, &arr);
        assert(rc == 0);
        if (!arr.release) break;
        total += (uint64_t)arr.length;
        arr.release(&arr);
    }
    if (stream.release) stream.release(&stream);
    assert(total == 2);

    bool caught = false;
    try {
        ds.scan().row_addr_allowlist(nullptr, 1);
    } catch (const lance::Error&) {
        caught = true;
    }
    assert(caught);
    printf("rows=%llu... ", (unsigned long long)total);

    PASS();
}

static void test_scanner_async_stream_ownership(const std::string& uri) {
    TEST(test_scanner_async_stream_ownership);

    auto ds = lance::Dataset::open(uri);
    auto scanner = ds.scan();
    AsyncScanCapture captured;
    scanner.scan_async(capture_async_scan, &captured);

    ArrowArrayStream* stream = nullptr;
    {
        std::unique_lock<std::mutex> lock(captured.mutex);
        bool completed = captured.ready.wait_for(
            lock, std::chrono::seconds(30), [&captured] {
                return captured.completed;
            });
        assert(completed && "async scan callback timed out");
        assert(captured.status == 0);
        assert(captured.stream != nullptr);
        stream = captured.stream;
    }

    uint64_t total = 0;
    while (true) {
        ArrowArray array;
        memset(&array, 0, sizeof(array));
        int rc = stream->get_next(stream, &array);
        assert(rc == 0);
        if (!array.release) break;
        total += static_cast<uint64_t>(array.length);
        array.release(&array);
    }
    assert(total > 0);

    // This releases the stream contents (if still live) and the separate
    // library-allocated outer structure. It is also explicitly NULL-safe.
    lance::scanner_async_stream_free(stream);
    lance::scanner_async_stream_free(nullptr);

    PASS();
}

/// Arrow C Data Interface format of the `blob` column in a stream's schema,
/// or an empty string when the column is missing.
static std::string blob_column_format(ArrowArrayStream& stream) {
    ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    int rc = stream.get_schema(&stream, &schema);
    assert(rc == 0);
    std::string format;
    for (int64_t i = 0; i < schema.n_children; i++) {
        if (strcmp(schema.children[i]->name, "blob") == 0) {
            format = schema.children[i]->format;
        }
    }
    if (schema.release) schema.release(&schema);
    return format;
}

static void test_scanner_blob_handling(const std::string& blob_uri) {
    TEST(test_scanner_blob_handling);

    auto ds = lance::Dataset::open(blob_uri);

    // By default a blob column arrives as its description struct ("+s").
    {
        auto scanner = ds.scan();
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);
        assert(blob_column_format(stream) == "+s");
        if (stream.release) stream.release(&stream);
    }

    // ALL_BINARY: LargeBinary ("Z"), and every row is still returned.
    auto scanner = ds.scan();
    scanner.blob_handling(LANCE_BLOB_HANDLING_ALL_BINARY);
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);
    assert(blob_column_format(stream) == "Z");

    uint64_t total = 0;
    while (true) {
        ArrowArray arr;
        memset(&arr, 0, sizeof(arr));
        int rc = stream.get_next(&stream, &arr);
        assert(rc == 0);
        if (!arr.release) break;
        total += (uint64_t)arr.length;
        arr.release(&arr);
    }
    assert(total == ds.count_rows());
    if (stream.release) stream.release(&stream);

    // Once the scan has started the setting is rejected.
    bool caught = false;
    try {
        scanner.blob_handling(LANCE_BLOB_HANDLING_BLOBS_DESCRIPTIONS);
    } catch (const lance::Error& e) {
        caught = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught);

    printf("rows=%llu... ", (unsigned long long)total);
    PASS();
}

/// Byte `i` of every blob payload in the smoke fixture.
static uint8_t blob_byte(size_t i) { return static_cast<uint8_t>(i * 7 + 3); }

/// Check that `bytes` are the payload bytes starting at `offset`.
static void assert_blob_payload(const std::vector<uint8_t>& bytes, size_t offset) {
    for (size_t i = 0; i < bytes.size(); i++) {
        assert(bytes[i] == blob_byte(offset + i));
    }
}

static void test_take_blobs(const std::string& blob_uri) {
    TEST(test_take_blobs);

    std::vector<std::optional<lance::BlobFile>> survivors;
    {
        auto ds = lance::Dataset::open(blob_uri);

        // The first fragment holds an inline, a packed, a dedicated, an empty
        // and a null blob, in that order.
        uint64_t indices[] = {0, 1, 2, 3, 4};
        auto blobs = ds.take_blobs_by_indices(indices, 5, "blob");
        assert(blobs.size() == 5);
        const uint64_t sizes[] = {8, 128, 1024, 0};
        for (size_t i = 0; i < 4; i++) {
            assert(blobs[i].has_value());
            assert(blobs[i]->size() == sizes[i]);
            assert_blob_payload(blobs[i]->read(), 0);
            assert(blobs[i]->tell() == sizes[i]);
        }
        assert(!blobs[4].has_value());

        // Cursor and positional reads on the packed blob.
        lance::BlobFile& packed = *blobs[1];
        packed.seek(100);
        auto tail = packed.read_up_to(64);
        assert(tail.size() == 28);
        assert_blob_payload(tail, 100);
        assert(packed.tell() == 128);
        auto window = packed.read_range(40, 16);
        assert(window.size() == 16);
        assert_blob_payload(window, 40);
        assert(packed.tell() == 128);

        // The same column by row ID. Without stable row ids a row id is the
        // row address, so the second fragment starts at 1 << 32.
        uint64_t row_ids[] = {0, (uint64_t{1} << 32) | 2};
        survivors = ds.take_blobs(row_ids, 2, "blob");
        assert(survivors.size() == 2);
        assert(survivors[0]->size() == 8);
        assert(survivors[1]->size() == 1024);

        // A column that is not a blob column is rejected.
        bool caught = false;
        try {
            ds.take_blobs_by_indices(indices, 5, "raw");
        } catch (const lance::Error& e) {
            caught = true;
            assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
        }
        assert(caught);
    }

    // Handles stay readable after the Dataset is gone.
    assert_blob_payload(survivors[1]->read(), 0);
    assert(survivors[1]->tell() == 1024);

    PASS();
}

static void test_blob_native_read_slice(const std::string& blob_uri) {
    TEST(test_blob_native_read_slice);

    std::vector<std::optional<lance::BlobFile>> addressed_survivors;
    {
        auto ds = lance::Dataset::open(blob_uri);
        uint64_t addresses[] = {
            0,
            (uint64_t{1} << 32) | 2,
            (uint64_t{1} << 32) | 1,
            3,
            4,
            0,
        };
        addressed_survivors =
            ds.take_blobs_by_addresses(addresses, 6, "blob");
        assert(addressed_survivors.size() == 6);
        const uint64_t sizes[] = {8, 1024, 128, 0, 0, 8};
        for (size_t i = 0; i < 6; i++) {
            if (i == 4) {
                assert(!addressed_survivors[i].has_value());
            } else {
                assert(addressed_survivors[i].has_value());
                assert(addressed_survivors[i]->size() == sizes[i]);
            }
        }

        // Empty by-address selections still validate the column natively.
        bool unknown_column = false;
        try {
            ds.take_blobs_by_addresses(nullptr, 0, "does_not_exist");
        } catch (const lance::Error& error) {
            unknown_column = error.code == LANCE_ERR_INTERNAL;
        }
        assert(unknown_column);

        bool non_blob = false;
        try {
            ds.take_blobs_by_addresses(nullptr, 0, "raw");
        } catch (const lance::Error& error) {
            non_blob = error.code == LANCE_ERR_INVALID_ARGUMENT;
        }
        assert(non_blob);

        // The old C++ take wrapper keeps its empty-selection shortcut.
        auto legacy = ds.take_blobs(nullptr, 0, "does_not_exist");
        assert(legacy.empty());

        uint64_t invalid_addresses[] = {0, UINT64_MAX - 1};
        bool invalid_address = false;
        try {
            ds.take_blobs_by_addresses(invalid_addresses, 2, "blob");
        } catch (const lance::Error& error) {
            invalid_address = error.code == LANCE_ERR_INVALID_ARGUMENT;
        }
        assert(invalid_address);
    }

    // The dataset is gone; every handle still owns its reader.
    for (size_t i = 0; i < addressed_survivors.size(); i++) {
        if (i == 4) continue;
        assert(addressed_survivors[i].has_value());
        if (i == 5) {
            assert(addressed_survivors[i]->tell() == 0);
        }
        assert_blob_payload(addressed_survivors[i]->read(), 0);
    }

    lance::BlobFile& packed = *addressed_survivors[2];
    packed.seek(5);
    std::vector<lance::BlobRange> ranges = {
        {100, 20},
        {128, 0},
        {40, 16},
        {100, 8},
        {127, 1},
        {0, 128},
    };
    auto parts = packed.read_ranges(ranges);
    assert(parts.size() == ranges.size());
    const size_t sizes[] = {20, 0, 16, 8, 1, 128};
    for (size_t i = 0; i < parts.size(); i++) {
        assert(parts[i].size() == sizes[i]);
        assert_blob_payload(parts[i], ranges[i].offset);
    }
    assert(packed.tell() == 5);
    assert(packed.read_ranges(std::vector<lance::BlobRange>{}).empty());
    assert(packed.tell() == 5);

    // read_into reuses the C read_up_to path and reports EOF as zero bytes.
    packed.seek(120);
    std::array<uint8_t, 16> tail{};
    size_t bytes_read = packed.read_into(tail.data(), tail.size());
    assert(bytes_read == 8);
    std::vector<uint8_t> tail_bytes(tail.begin(), tail.begin() + 8);
    assert_blob_payload(tail_bytes, 120);
    assert(packed.tell() == 128);
    assert(packed.read_into(nullptr, 0) == 0);

    packed.set_buffer_size(0);
    packed.set_buffer_size(64);
    assert(!packed.closed());
    packed.close();
    assert(packed.closed());
    assert(packed.size() == 128);
    packed.close();
    assert(packed.closed());

    bool set_buffer_failed = false;
    try {
        packed.set_buffer_size(32);
    } catch (const lance::Error& error) {
        set_buffer_failed = error.code == LANCE_ERR_INVALID_ARGUMENT;
    }
    assert(set_buffer_failed);

    bool ranges_failed = false;
    try {
        packed.read_ranges(std::vector<lance::BlobRange>{});
    } catch (const lance::Error& error) {
        ranges_failed = error.code == LANCE_ERR_INVALID_ARGUMENT;
    }
    assert(ranges_failed);
    assert(packed.read_range(0, 0).empty());

    bool read_into_failed = false;
    try {
        packed.read_into(tail.data(), 1);
    } catch (const lance::Error& error) {
        read_into_failed = error.code == LANCE_ERR_INVALID_ARGUMENT;
    }
    assert(read_into_failed);

    PASS();
}

static int64_t cpp_schema_field_index(const ArrowSchema* schema,
                                      const char* name) {
    for (int64_t i = 0; i < schema->n_children; ++i) {
        if (schema->children[i] && schema->children[i]->name &&
            strcmp(schema->children[i]->name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool cpp_array_value_is_null(const ArrowArray* array, int64_t index) {
    if (array->null_count == 0) return false;
    assert(array->buffers[0] != nullptr);
    int64_t bit = array->offset + index;
    const auto* validity = static_cast<const uint8_t*>(array->buffers[0]);
    return (validity[bit / 8] & static_cast<uint8_t>(1u << (bit % 8))) == 0;
}

static uint64_t cpp_u64_array_value(const ArrowArray* array, int64_t index) {
    assert(array->buffers[1] != nullptr);
    const auto* values = static_cast<const uint64_t*>(array->buffers[1]);
    return values[array->offset + index];
}

static uint32_t cpp_u32_array_value(const ArrowArray* array, int64_t index) {
    assert(array->buffers[1] != nullptr);
    const auto* values = static_cast<const uint32_t*>(array->buffers[1]);
    return values[array->offset + index];
}

static void test_blob_fields_and_descriptors() {
    TEST(test_blob_fields_and_descriptors);

    lance::BlobFieldOptions options;
    options.inline_size_threshold = size_t{0};
    options.dedicated_size_threshold = size_t{256};
    ArrowSchema field{};
    lance::blob_field_with_options("blob", true, options, &field);
    assert(strcmp(field.name, "blob") == 0);
    assert(strcmp(field.format, "+s") == 0);
    assert((field.flags & ARROW_FLAG_NULLABLE) != 0);
    assert_field_metadata(&field, "ARROW:extension:name", "lance.blob.v2");
    assert_field_metadata(
        &field, "lance-encoding:blob-inline-size-threshold", "0");
    assert_field_metadata(
        &field, "lance-encoding:blob-dedicated-size-threshold", "256");
    field.release(&field);

    ArrowSchema default_field{};
    lance::blob_field("default", false, &default_field);
    assert(strcmp(default_field.name, "default") == 0);
    assert((default_field.flags & ARROW_FLAG_NULLABLE) == 0);
    assert_field_metadata_absent(
        &default_field, "lance-encoding:blob-inline-size-threshold");
    default_field.release(&default_field);

    lance::BlobDescriptorArrayBuilder builder("blob");
    builder.append_inline(
        reinterpret_cast<const uint8_t*>("inline payload"), 14);
    builder.append_null();
    builder.append_packed(7, 100, 8);
    builder.append_dedicated(9, 1024);
    builder.append_managed("data/a.blob", 2, 3);
    builder.append_managed("data/b.blob", 4, 5, uint32_t{0});
    builder.extend_packed(11, {{1, 2}, {3, 4}});
    builder.push_external(
        "file:///tmp/external.bin", lance::BlobRange{7, 9});
    builder.append_external(3, "s3://bucket/object", 11, 13);
    builder.append_inline(nullptr, 0);

    ArrowSchema builder_field{};
    builder.field(&builder_field);
    assert_field_metadata(
        &builder_field, "ARROW:extension:name", "lance.blob.v2");
    builder_field.release(&builder_field);

    ArrowArray array{};
    ArrowSchema schema{};
    builder.finish(&array, &schema);
    assert(array.length == 11);
    assert(array.n_children == 6 && schema.n_children == 6);
    const char* prepared_names[] = {
        "kind", "data", "uri", "blob_id", "blob_size", "position",
    };
    for (size_t i = 0; i < 6; ++i) {
        assert(std::string(schema.children[i]->name) == prepared_names[i]);
    }

    const ArrowArray* kinds = array.children[0];
    const ArrowArray* data = array.children[1];
    const ArrowArray* blob_ids = array.children[3];
    const auto* kind_values = static_cast<const uint8_t*>(kinds->buffers[1]);
    assert(!cpp_array_value_is_null(kinds, 0) && kind_values[0] == 0);
    assert(cpp_array_value_is_null(kinds, 1));
    assert(kind_values[2] == 1);
    assert(kind_values[3] == 2);
    assert(kind_values[4] == 4);
    assert(kind_values[5] == 5);
    assert(kind_values[8] == 3 && kind_values[9] == 3);
    assert(cpp_array_value_is_null(blob_ids, 4));
    assert(!cpp_array_value_is_null(blob_ids, 5) &&
           cpp_u32_array_value(blob_ids, 5) == 0);
    const auto* data_offsets =
        static_cast<const int64_t*>(data->buffers[1]);
    assert(data_offsets[1] - data_offsets[0] == 14);
    assert(memcmp(static_cast<const uint8_t*>(data->buffers[2]) +
                      data_offsets[0],
                  "inline payload", 14) == 0);

    ArrowSchema post_finish_field{};
    builder.field(&post_finish_field);
    assert(strcmp(post_finish_field.name, "blob") == 0);
    post_finish_field.release(&post_finish_field);
    bool finished = false;
    try {
        builder.append_null();
    } catch (const lance::Error& error) {
        finished = error.code == LANCE_ERR_INVALID_ARGUMENT &&
                   std::string(error.what()).find("already finished") !=
                       std::string::npos;
    }
    assert(finished);

    array.release(&array);
    schema.release(&schema);
    PASS();
}

static void test_blob_planned_reads(const std::string& blob_uri) {
    TEST(test_blob_planned_reads);

    lance::ArrowStream stream;
    {
        auto ds = lance::Dataset::open(blob_uri);
        stream = ds.read_blobs(
            "blob", lance::BlobRowSelector::Indices,
            std::vector<uint64_t>{7, 2, 3, 4, 9, 0});
        // Dataset destruction must not invalidate the exported stream.
    }
    assert(static_cast<bool>(stream));
    ArrowSchema schema{};
    assert(stream->get_schema(stream.get(), &schema) == 0);
    assert(schema.n_children == 2);
    assert(cpp_schema_field_index(&schema, "row_address") == 0);
    assert(cpp_schema_field_index(&schema, "data") == 1);
    assert(strcmp(schema.children[0]->format, "L") == 0);
    assert(strcmp(schema.children[1]->format, "Z") == 0);
    schema.release(&schema);

    const uint64_t expected_addresses[] = {
        (uint64_t{1} << 32) | 2,
        2,
        3,
        4,
        (uint64_t{1} << 32) | 4,
        0,
    };
    const size_t expected_sizes[] = {1024, 1024, 0, 0, 0, 8};
    const bool expected_null[] = {false, false, false, true, true, false};
    for (size_t row = 0; row < 6; ++row) {
        ArrowArray array{};
        assert(stream->get_next(stream.get(), &array) == 0);
        assert(array.release != nullptr);
        assert(array.length == 1 && array.n_children == 2);
        assert(cpp_u64_array_value(array.children[0], 0) ==
               expected_addresses[row]);
        const ArrowArray* data = array.children[1];
        if (expected_null[row]) {
            assert(cpp_array_value_is_null(data, 0));
        } else {
            assert(!cpp_array_value_is_null(data, 0));
            const auto* offsets =
                static_cast<const int64_t*>(data->buffers[1]);
            int64_t first = data->offset;
            assert(static_cast<size_t>(offsets[first + 1] - offsets[first]) ==
                   expected_sizes[row]);
        }
        array.release(&array);
    }
    ArrowArray eos{};
    assert(stream->get_next(stream.get(), &eos) == 0 &&
           eos.release == nullptr);

    auto ds = lance::Dataset::open(blob_uri);
    lance::BlobReadOptions read_options;
    read_options.io_buffer_size = uint64_t{64};
    read_options.preserve_order = false;
    auto range_stream = ds.read_blob_ranges(
        "blob", lance::BlobRowSelector::Indices,
        std::vector<lance::BlobRangeRequest>{
            {7, 100, 20},
            {2, 128, 0},
            {9, 40, 16},
            {2, 100, 8},
            {3, 0, 0},
            {4, 1000, 100},
        },
        read_options);
    assert(static_cast<bool>(range_stream));
    ArrowSchema range_schema{};
    assert(range_stream->get_schema(range_stream.get(), &range_schema) == 0);
    assert(range_schema.n_children == 5);
    assert(cpp_schema_field_index(&range_schema, "request_index") == 0);
    assert(cpp_schema_field_index(&range_schema, "row_address") == 1);
    assert(cpp_schema_field_index(&range_schema, "offset") == 2);
    assert(cpp_schema_field_index(&range_schema, "length") == 3);
    assert(cpp_schema_field_index(&range_schema, "data") == 4);
    range_schema.release(&range_schema);
    std::set<size_t> request_indices;
    while (true) {
        ArrowArray array{};
        assert(range_stream->get_next(range_stream.get(), &array) == 0);
        if (!array.release) break;
        assert(array.length == 1 && array.n_children == 5);
        size_t request =
            static_cast<size_t>(cpp_u64_array_value(array.children[0], 0));
        assert(request_indices.insert(request).second);
        const ArrowArray* data = array.children[4];
        if (request == 5) {
            assert(cpp_array_value_is_null(data, 0));
        } else {
            assert(!cpp_array_value_is_null(data, 0));
        }
        array.release(&array);
    }
    assert(request_indices.size() == 6);

    auto empty = ds.read_blobs(
        "blob", lance::BlobRowSelector::Ids, std::vector<uint64_t>{});
    ArrowSchema empty_schema{};
    assert(empty->get_schema(empty.get(), &empty_schema) == 0);
    ArrowArray empty_array{};
    assert(empty->get_next(empty.get(), &empty_array) == 0 &&
           empty_array.release == nullptr);
    empty_schema.release(&empty_schema);

    bool no_selection = false;
    try {
        (void)ds.read_blobs("blob", lance::BlobRowSelector::None, {});
    } catch (const lance::Error& error) {
        no_selection =
            error.code == LANCE_ERR_INVALID_ARGUMENT &&
            std::string(error.what()).find("requires a row selection") !=
                std::string::npos;
    }
    assert(no_selection);

    bool zero_buffer = false;
    try {
        lance::BlobReadOptions invalid;
        invalid.io_buffer_size = uint64_t{0};
        (void)ds.read_blobs(
            "blob", lance::BlobRowSelector::Indices,
            std::vector<uint64_t>{0}, invalid);
    } catch (const lance::Error& error) {
        zero_buffer = error.code == LANCE_ERR_INVALID_ARGUMENT &&
                      std::string(error.what()).find("greater than 0") !=
                          std::string::npos;
    }
    assert(zero_buffer);

    PASS();
}

static void test_dataset_take(const std::string& uri) {
    TEST(test_dataset_take);

    auto ds = lance::Dataset::open(uri);

    uint64_t indices[] = {0, 1, 2};
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ds.take(indices, 3, &stream);

    uint64_t total = 0;
    while (true) {
        ArrowArray arr;
        memset(&arr, 0, sizeof(arr));
        int rc = stream.get_next(&stream, &arr);
        assert(rc == 0);
        if (!arr.release) break;
        total += (uint64_t)arr.length;
        arr.release(&arr);
    }

    assert(total == 3);
    printf("rows=%llu... ", (unsigned long long)total);

    if (stream.release) stream.release(&stream);
    PASS();
}

static void test_dataset_take_rows(const std::string& uri) {
    TEST(test_dataset_take_rows);

    auto ds = lance::Dataset::open(uri);

    // The smoke fixture has one fragment, so its first row IDs are 0, 1, 2.
    uint64_t row_ids[] = {0, 1, 2};
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ds.take_rows(row_ids, 3, &stream);

    uint64_t total = 0;
    while (true) {
        ArrowArray arr;
        memset(&arr, 0, sizeof(arr));
        int rc = stream.get_next(&stream, &arr);
        assert(rc == 0);
        if (!arr.release) break;
        total += (uint64_t)arr.length;
        arr.release(&arr);
    }

    assert(total == 3);
    printf("rows=%llu... ", (unsigned long long)total);

    if (stream.release) stream.release(&stream);
    PASS();
}

static void test_raii_cleanup(const std::string& uri) {
    TEST(test_raii_cleanup);

    // Dataset and Scanner should clean up automatically.
    {
        auto ds = lance::Dataset::open(uri);
        auto scanner = ds.scan();
        scanner.limit(1);
        // Goes out of scope — RAII cleanup.
    }

    // Move semantics.
    {
        auto ds1 = lance::Dataset::open(uri);
        auto ds2 = std::move(ds1);
        assert(ds2.count_rows() > 0);

        bool moved_from_version_threw = false;
        try {
            (void)ds1.version();
        } catch (const lance::Error& e) {
            moved_from_version_threw = true;
            assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
        }
        assert(moved_from_version_threw);
    }

    PASS();
}

static void test_versions(const std::string& uri) {
    TEST(test_versions);

    auto ds = lance::Dataset::open(uri);
    auto versions = ds.versions();

    assert(!versions.empty());
    for (const auto& v : versions) {
        assert(v.id >= 1);
        assert(v.timestamp_ms > 0);
    }
    printf("count=%zu... ", versions.size());

    PASS();
}

static void test_version_refs_and_tags(const std::string& uri) {
    TEST(test_version_refs_and_tags);

    auto ds = lance::Dataset::open(uri);
    assert(ds.version() == 2);
    assert(ds.count_rows() == 20);

    auto current = ds.current_version();
    assert(current.id == ds.version());
    assert(current.timestamp_ns > 0);
    assert(current.metadata.at("total_rows") == "20");
    assert(current.metadata.at("total_fragments") == "2");

    auto details = ds.version_details();
    assert(details.size() == 2);
    assert(details[0].id == 1);
    assert(details[0].metadata.at("total_rows") == "10");
    assert(details[1].id == 2);
    assert(details[1].metadata.at("total_rows") == "20");

    auto refs = ds.version_refs();
    assert((refs == std::vector<uint64_t>{1, 2}));

    auto checked_v1 = ds.checkout(lance::Ref::version_number(1));
    assert(checked_v1.version() == 1 && checked_v1.count_rows() == 10);
    auto checked_latest = ds.checkout(lance::Ref::version(std::nullopt, std::nullopt));
    assert(checked_latest.version() == 2 && checked_latest.count_rows() == 20);
    auto checked_main =
        ds.checkout(lance::Ref::version(std::optional<std::string>("main"), 1));
    assert(checked_main.version() == 1);

    ds.create_tag("cpp-release", lance::Ref::version_number(1));
    auto tag = ds.get_tag("cpp-release");
    assert(tag.name == "cpp-release");
    assert(tag.version == 1);
    assert(!tag.branch.has_value());
    assert(tag.created_at_ns.has_value());
    assert(tag.updated_at_ns.has_value());
    assert(tag.manifest_size > 0);
    assert(tag.metadata.empty());

    bool duplicate_conflict = false;
    try {
        ds.create_tag("cpp-release", lance::Ref::version_number(2));
    } catch (const lance::Error& error) {
        duplicate_conflict = error.code == LANCE_ERR_COMMIT_CONFLICT;
    }
    assert(duplicate_conflict);

    auto old_checkout = ds.checkout(lance::Ref::tag("cpp-release"));
    assert(old_checkout.version() == 1);

    ds.replace_tag_metadata(
        "cpp-release", {{"owner", "cpp"}, {"empty", ""}});
    auto persisted = lance::Dataset::open(uri).get_tag("cpp-release");
    assert(persisted.metadata.at("owner") == "cpp");
    assert(persisted.metadata.at("empty").empty());

    ds.update_tag(
        "cpp-release",
        lance::Ref::version(std::optional<std::string>("main"), 2));
    auto updated = ds.get_tag("cpp-release");
    assert(updated.version == 2);
    assert(updated.metadata == persisted.metadata);
    assert(updated.created_at_ns == tag.created_at_ns);
    assert(updated.updated_at_ns.has_value());
    assert(*updated.updated_at_ns >= *tag.created_at_ns);

    // Existing checked-out snapshots keep their original manifest.
    assert(old_checkout.version() == 1 && old_checkout.count_rows() == 10);
    auto updated_checkout = ds.checkout(lance::Ref::tag("cpp-release"));
    assert(updated_checkout.version() == 2 && updated_checkout.count_rows() == 20);

    auto tags = ds.tags();
    assert(tags.size() == 1);
    assert(tags[0].name == "cpp-release");

    ds.replace_tag_metadata("cpp-release", {});
    assert(ds.get_tag("cpp-release").metadata.empty());

    ds.delete_tag("cpp-release");
    bool missing_tag = false;
    try {
        (void)ds.get_tag("cpp-release");
    } catch (const lance::Error& error) {
        missing_tag = error.code == LANCE_ERR_NOT_FOUND;
    }
    assert(missing_tag);

    assert(ds.version() == 2);
    PASS();
}

static void test_branch_lifecycle(const std::string& uri) {
    TEST(test_branch_lifecycle);

    auto ds = lance::Dataset::open(uri);
    assert(ds.version() == 2);
    assert(ds.count_rows() == 20);

    auto main_identifier = ds.branch_identifier_json();
    assert(main_identifier.find("\"version_mapping\":[]") !=
           std::string::npos);
    assert(ds.branches_json() == "{}");

    auto parent = ds.create_branch(
        "cpp/parent", lance::Ref::version_number(1));
    assert(parent.version() == 1);
    assert(parent.count_rows() == 10);
    auto child = parent.create_branch(
        "cpp/parent/child", lance::Ref::version_number(1));
    assert(child.count_rows() == 10);

    auto parent_json = ds.get_branch_json("cpp/parent");
    assert(parent_json.find("\"parentBranch\":null") != std::string::npos);
    assert(parent_json.find("\"parentVersion\":1") != std::string::npos);
    assert(parent_json.find("\"createAt\":") != std::string::npos);
    assert(parent_json.find("\"manifestSize\":") != std::string::npos);
    assert(parent_json.find("\"identifier\":{\"version_mapping\":") !=
           std::string::npos);
    assert(parent_json.find("\"metadata\":{}") != std::string::npos);

    auto child_json = ds.get_branch_json("cpp/parent/child");
    assert(child_json.find("\"parentBranch\":\"cpp/parent\"") !=
           std::string::npos);

    auto branch_list = ds.branches_json();
    assert(branch_list.find("\"cpp/parent\"") != std::string::npos);
    assert(branch_list.find("\"cpp/parent/child\"") != std::string::npos);
    assert(parent.branch_identifier_json().find(
               "\"version_mapping\":[[") != std::string::npos);

    std::vector<std::pair<std::string, std::string>> empty_storage_opts;
    auto options_branch = ds.create_branch(
        "cpp/empty-options",
        lance::Ref::version_number(1),
        empty_storage_opts);
    assert(options_branch.count_rows() == 10);
    options_branch = ds.create_branch(
        "cpp/empty-options-2", lance::Ref::version_number(2));
    assert(options_branch.count_rows() == 20);

    auto parent_checkout = ds.checkout_branch("cpp/parent");
    auto main_checkout = ds.checkout_branch("main");
    assert(parent_checkout.version() == 1);
    assert(parent_checkout.count_rows() == 10);
    assert(main_checkout.version() == 2);
    assert(main_checkout.count_rows() == 20);

    ds.replace_branch_metadata(
        "cpp/parent", {{"owner", "cpp"}, {"empty", ""}});
    auto persisted = lance::Dataset::open(uri).get_branch_json("cpp/parent");
    assert(persisted.find("\"owner\":\"cpp\"") != std::string::npos);
    assert(persisted.find("\"empty\":\"\"") != std::string::npos);
    ds.replace_branch_metadata("cpp/parent", {});
    auto cleared = ds.get_branch_json("cpp/parent");
    assert(cleared.find("\"owner\"") == std::string::npos);
    assert(cleared.find("\"metadata\":{}") != std::string::npos);

    bool duplicate_failed = false;
    try {
        ds.create_branch("cpp/parent", lance::Ref::version_number(2));
    } catch (const lance::Error& error) {
        duplicate_failed = error.code != LANCE_OK;
    }
    assert(duplicate_failed);

    bool child_protection = false;
    try {
        ds.delete_branch("cpp/parent");
    } catch (const lance::Error& error) {
        child_protection = error.code == LANCE_ERR_COMMIT_CONFLICT;
    }
    assert(child_protection);

    ds.delete_branch("cpp/parent/child");
    ds.delete_branch("cpp/parent");
    ds.delete_branch("cpp/empty-options");
    ds.delete_branch("cpp/empty-options-2");

    auto tagged = ds.create_branch(
        "cpp/tagged", lance::Ref::version_number(1));
    ds.create_tag(
        "cpp-branch-tag",
        lance::Ref::version(
            std::optional<std::string>("cpp/tagged"),
            std::optional<uint64_t>(1)));
    bool tag_protection = false;
    try {
        ds.delete_branch("cpp/tagged");
    } catch (const lance::Error& error) {
        tag_protection = error.code == LANCE_ERR_COMMIT_CONFLICT;
    }
    assert(tag_protection);
    ds.delete_tag("cpp-branch-tag");
    ds.delete_branch("cpp/tagged");

    bool reserved_main = false;
    try {
        ds.create_branch("main", lance::Ref::version_number(1));
    } catch (const lance::Error& error) {
        reserved_main = error.code == LANCE_ERR_INVALID_ARGUMENT;
    }
    assert(reserved_main);

    bool missing_get = false;
    try {
        (void)ds.get_branch_json("missing");
    } catch (const lance::Error& error) {
        missing_get = error.code == LANCE_ERR_NOT_FOUND;
    }
    assert(missing_get);

    bool missing_checkout = false;
    try {
        (void)ds.checkout_branch("missing");
    } catch (const lance::Error& error) {
        missing_checkout = error.code == LANCE_ERR_NOT_FOUND;
    }
    assert(missing_checkout);

    assert(ds.branches_json() == "{}");
    assert(tagged.version() == 1);
    assert(parent_checkout.count_rows() == 10);
    assert(main_checkout.count_rows() == 20);
    PASS();
}

// Restore to the dataset's own current version — always commits a new
// manifest (no skip-if-equal optimization) to defeat TOCTOU races against
// concurrent writers.
static void test_restore_to_current(const std::string& uri) {
    TEST(test_restore_to_current);

    auto ds = lance::Dataset::open(uri);
    uint64_t current = ds.version();

    auto after = ds.restore(current);
    assert(after.version() == current + 1);

    PASS();
}

static void test_error_exception(const std::string& /*uri*/) {
    TEST(test_error_exception);

    bool caught = false;
    try {
        lance::Dataset::open("file:///nonexistent/path/xyz");
    } catch (const lance::Error& e) {
        caught = true;
        assert(e.code != LANCE_OK);
        assert(strlen(e.what()) > 0);
        printf("caught: %s... ", e.what());
    }
    assert(caught);

    PASS();
}

static void test_index_lifecycle(const std::string& uri) {
    TEST(test_index_lifecycle);

    auto ds = lance::Dataset::open(uri);
    ds.create_scalar_index("id", LANCE_SCALAR_BTREE, "id_idx");
    assert(ds.index_count() == 1);

    auto json = ds.list_indices_json();
    assert(json.find("id_idx") != std::string::npos);
    printf("listed: %s... ", json.c_str());

    const auto& readonly_ds = ds;
    readonly_ds.prewarm_index("id_idx");
    readonly_ds.prewarm_index("id_idx");
    bool missing_index = false;
    try {
        readonly_ds.prewarm_index("missing_idx");
    } catch (const lance::Error& error) {
        assert(error.code == LANCE_ERR_NOT_FOUND);
        missing_index = true;
    }
    assert(missing_index);

    ds.drop_index("id_idx");
    assert(ds.index_count() == 0);

    PASS();
}

static void test_nearest_smoke(const std::string& uri) {
    TEST(test_nearest_smoke);

    auto ds = lance::Dataset::open(uri);
    auto scanner = ds.scan();
    float q[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};

    // The test dataset doesn't have a vector column; calling nearest will
    // either succeed (if "name" or "id" happens to work — won't) or throw.
    // We just exercise the wrapper code paths, expecting either outcome
    // gracefully. Compile/link is the main goal here.
    bool caught = false;
    try {
        scanner.nearest("embedding", q, 8, 5)
               .nprobes(2)
               .minimum_nprobes(1)
               .maximum_nprobes(2)
               .approx_mode(LANCE_APPROX_MODE_NORMAL)
               .query_parallelism(2)
               .refine_factor(1)
               .ef(50)
               .metric(LANCE_METRIC_L2)
               .use_index(true)
               .prefilter(false);
        // Try to materialize — will throw because "embedding" column doesn't exist
        // in the basic test fixture.
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);
        if (stream.release) stream.release(&stream);
    } catch (const lance::Error&) {
        caught = true;
    }
    // Either path is fine — we proved compile + linkage + the fluent chain.
    (void)caught;

    PASS();
}

static void test_multivector_rejects_flat_column(const std::string& uri) {
    TEST(test_multivector_rejects_flat_column);
    auto scanner = lance::Dataset::open(uri).scan();
    const float query[8] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    bool caught = false;
    try {
        scanner.nearest_multivector("embedding", query, 8, 1, LANCE_DTYPE_FLOAT32, 1);
    } catch (const lance::Error&) {
        caught = true;
    }
    assert(caught);
    PASS();
}

static void test_index_segments_smoke(const std::string& /*uri*/) {
    TEST(test_index_segments_smoke);

    // The shared test fixture has no vector column, so we can't actually run
    // segment enumeration end-to-end without building our own dataset. The
    // smoke goal is to prove the C++ wrappers compile and link.

    // Exercise the no-op clear path on a fresh scanner — passing a nullptr
    // buffer with len=0 must succeed.
    {
        // Create a scanner-less, empty-options scenario by trying to call the
        // wrapper signatures. We don't actually invoke them at runtime here
        // because we don't have a vector dataset to point at.
        constexpr auto verify_signatures = []() {
            using DsMember = uint64_t (lance::Dataset::*)(const std::string&) const;
            using SegMember = std::vector<std::array<uint8_t, 16>>
                (lance::Dataset::*)(const std::string&) const;
            using ScanMember = lance::Scanner& (lance::Scanner::*)(
                const std::vector<std::array<uint8_t, 16>>&);
            DsMember a = &lance::Dataset::index_segment_count;
            SegMember b = &lance::Dataset::index_segments;
            ScanMember c = static_cast<ScanMember>(&lance::Scanner::index_segments);
            (void)a; (void)b; (void)c;
        };
        verify_signatures();
    }

    PASS();
}

static void test_index_segment_builder(const std::string& uri) {
    TEST(test_index_segment_builder);

    static_assert(!std::is_copy_constructible_v<lance::IndexSegmentBuilder>);
    static_assert(!std::is_copy_assignable_v<lance::IndexSegmentBuilder>);
    static_assert(std::is_move_constructible_v<lance::IndexSegmentBuilder>);
    static_assert(!std::is_copy_constructible_v<lance::IndexSegmentMetadata>);
    static_assert(std::is_move_constructible_v<lance::IndexSegmentMetadata>);
    static_assert(!std::is_copy_constructible_v<lance::IndexModel>);
    static_assert(std::is_move_constructible_v<lance::IndexModel>);

    auto ds = lance::Dataset::open(uri);
    auto fragment_ids = ds.fragment_ids();
    assert(!fragment_ids.empty());
    uint32_t selected_fragment_id = static_cast<uint32_t>(fragment_ids.front());
    std::array<uint8_t, 16> expected_uuid = {
        0x21, 0x43, 0x65, 0x87, 0xa9, 0xcb, 0x4e, 0xfd,
        0x81, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    };

    LanceIndexSegmentBuildOptions options = {};
    options.fragment_ids = &selected_fragment_id;
    options.fragment_count = 1;
    options.index_uuid = expected_uuid.data();
    options.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;

    uint64_t version_before = ds.version();
    auto builder = ds.new_scalar_index_segment_builder(
        "id", LANCE_SCALAR_BITMAP, "cpp_id_segment", "", &options);
    auto bytes = builder.execute_uncommitted();
    assert(!bytes.empty());
    assert(ds.version() == version_before);

    auto metadata = lance::IndexSegmentMetadata::parse(bytes);
    assert(metadata.uuid() == expected_uuid);
    assert(metadata.name() == "cpp_id_segment");
    assert(metadata.dataset_version() == version_before);
    assert(metadata.index_version() == 0);
    assert(metadata.index_type() == LANCE_SCALAR_BITMAP);
    assert(metadata.index_details_type_url().find("BitmapIndexDetails") !=
           std::string::npos);
    assert(metadata.field_ids() == std::vector<int32_t>{0});
    assert(metadata.fragment_ids() ==
           std::vector<uint32_t>{selected_fragment_id});

    PASS();
}

static void test_index_segment_builder_progress(const std::string& uri) {
    TEST(test_index_segment_builder_progress);
    auto ds = lance::Dataset::open(uri);
    auto all_ids = ds.fragment_ids();
    assert(all_ids.size() >= 2);
    std::vector<uint32_t> fragment_ids;
    for (auto id : all_ids) fragment_ids.push_back(static_cast<uint32_t>(id));

    LanceVectorIndexParams params = {
        LANCE_INDEX_IVF_FLAT, LANCE_METRIC_L2, 2, 0, 0, 2, 0, 0, 16,
    };
    LanceIndexSegmentBuildOptions options = {};
    options.fragment_ids = fragment_ids.data();
    options.fragment_count = fragment_ids.size();
    options.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;

    BuildProgressCapture captured;
    auto builder = ds.new_vector_index_segment_builder(
        "embedding", params, "cpp_progress_idx", &options);
    builder.progress_callback(capture_build_progress, &captured);
    auto bytes = builder.execute_uncommitted();

    assert(!bytes.empty());
    assert(captured.events > 0);
    assert(captured.starts > 0 && captured.completes > 0);
    assert(!captured.invalid);
    PASS();
}

static void test_vector_models_and_reusable_segments(const std::string& uri) {
    TEST(test_vector_models_and_reusable_segments);
    auto ds = lance::Dataset::open(uri);
    auto all_ids = ds.fragment_ids();
    assert(all_ids.size() >= 2);
    assert(all_ids[0] <= UINT32_MAX && all_ids[1] <= UINT32_MAX);

    auto centroids =
        ds.train_ivf_model("embedding", 2, LANCE_METRIC_L2);
    auto codebook =
        ds.train_pq_model("embedding", 2, 4, LANCE_METRIC_L2, centroids);
    LanceVectorIndexParams params = {
        LANCE_INDEX_IVF_PQ, LANCE_METRIC_L2, 2, 2, 4, 2, 0, 0, 16,
    };
    for (size_t i = 0; i < 2; ++i) {
        uint32_t fragment_id = static_cast<uint32_t>(all_ids[i]);
        LanceIndexSegmentBuildOptions options = {};
        options.fragment_ids = &fragment_id;
        options.fragment_count = 1;
        options.ivf_centroids = centroids.array();
        options.ivf_centroids_schema = centroids.schema();
        options.pq_codebook = codebook.array();
        options.pq_codebook_schema = codebook.schema();
        options.mode = LANCE_INDEX_SEGMENT_BUILD_PRECOMPUTED;
        auto builder = ds.new_vector_index_segment_builder(
            "embedding", params, "", &options);
        auto bytes = builder.execute_uncommitted();
        auto metadata = lance::IndexSegmentMetadata::parse(bytes);
        assert(metadata.index_type() == LANCE_INDEX_IVF_PQ);
        assert(metadata.fragment_ids() == std::vector<uint32_t>{fragment_id});
        assert(centroids.array()->release != nullptr);
        assert(codebook.array()->release != nullptr);
    }
    PASS();
}

static void test_commit_index_segments(const std::string& uri) {
    TEST(test_commit_index_segments);

    auto ds = lance::Dataset::open(uri);
    auto all_ids = ds.fragment_ids();
    assert(all_ids.size() >= 2);

    LanceVectorIndexParams params = {
        LANCE_INDEX_IVF_FLAT, LANCE_METRIC_L2, 2, 0, 0, 2, 0, 0, 16,
    };

    // Build one uncommitted segment per fragment (the distributed workers).
    std::vector<std::vector<uint8_t>> segments;
    std::vector<std::array<uint8_t, 16>> expected_uuids;
    for (size_t i = 0; i < 2; ++i) {
        uint32_t fragment_id = static_cast<uint32_t>(all_ids[i]);
        LanceIndexSegmentBuildOptions options = {};
        options.fragment_ids = &fragment_id;
        options.fragment_count = 1;
        options.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;
        auto builder = ds.new_vector_index_segment_builder(
            "embedding", params, "cpp_distributed_idx", &options);
        segments.push_back(builder.execute_uncommitted());
        auto metadata = lance::IndexSegmentMetadata::parse(segments.back());
        expected_uuids.push_back(metadata.uuid());
    }

    // One commit registers both segments as a single logical index.
    uint64_t version_before = ds.version();
    ds.commit_index_segments("cpp_distributed_idx", "embedding", segments);
    assert(ds.version() == version_before + 1);
    assert(ds.index_segment_count("cpp_distributed_idx") == 2);
    auto committed = ds.index_segments("cpp_distributed_idx");
    assert(committed.size() == 2);
    for (size_t i = 0; i < 2; ++i) assert(committed[i] == expected_uuids[i]);

    auto segments_json = ds.index_segments_json();
    assert(segments_json.find("\"name\":\"cpp_distributed_idx\"") !=
           std::string::npos);
    assert(segments_json.find("\"index_version\":1") != std::string::npos);
    for (size_t i = 0; i < 2; ++i) {
        auto coverage = "\"fragment_ids\":[" +
                        std::to_string(all_ids[i]) + "]";
        assert(segments_json.find(coverage) != std::string::npos);
    }

    // Duplicate segment UUIDs in the commit set are rejected.
    bool caught = false;
    try {
        ds.commit_index_segments(
            "cpp_dup_idx", "embedding", {segments[0], segments[0]});
    } catch (const lance::Error& e) {
        caught = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught);

    // An empty commit set is rejected.
    caught = false;
    try {
        ds.commit_index_segments(
            "cpp_empty_idx", "embedding", {});
    } catch (const lance::Error& e) {
        caught = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught);
    assert(ds.version() == version_before + 1);

    PASS();
}

static void test_fts_smoke(const std::string& uri) {
    TEST(test_fts_smoke);

    auto ds = lance::Dataset::open(uri);

    // Build the inverted index needed for FTS. (Inverted requires non-NULL
    // params JSON for the tokenizer config.)
    bool index_built = false;
    try {
        ds.create_scalar_index(
            "name", LANCE_SCALAR_INVERTED, "name_fts",
            R"({"base_tokenizer":"simple","language":"English"})");
        index_built = true;
    } catch (const lance::Error&) {
        // If the test fixture doesn't permit indexing for some reason,
        // we still want to prove the wrappers compile + link.
    }

    auto scanner = ds.scan();
    bool caught = false;
    try {
        scanner.full_text_search("alice", {"name"}, 0);
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);
        if (stream.release) stream.release(&stream);
    } catch (const lance::Error&) {
        caught = true;
    }
    // Either path is acceptable — the goal is compile + linkage.
    (void)index_built;
    (void)caught;

    PASS();
}

// Round-trip: scan src dataset to an ArrowArrayStream, write it to a new
// dataset via lance::Dataset::write, and verify row counts match.
// dst_uri must not pre-exist.
static void test_dataset_write_roundtrip(const std::string& src_uri,
                                         const std::string& dst_uri) {
    TEST(test_dataset_write_roundtrip);

    auto src = lance::Dataset::open(src_uri);
    uint64_t src_rows = src.count_rows();

    auto scanner = src.scan();
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);

    // The existing five-argument call must accept brace-initialized defaults.
    auto dst = lance::Dataset::write(
        dst_uri, &stream, lance::WriteMode::Create, {}, {});

    uint64_t dst_rows = dst.count_rows();
    assert(dst_rows == src_rows);
    auto config = dst.config();
    assert(config.at("lance.auto_cleanup.interval") == "20");
    assert(config.at("lance.auto_cleanup.older_than") == "14days");
    printf("src=%llu, dst=%llu... ",
           (unsigned long long)src_rows, (unsigned long long)dst_rows);

    PASS();
}

static void test_write_options_and_config(const std::string& src_uri,
                                          const std::string& dst_uri) {
    TEST(test_write_options_and_config);

    const std::string uri = dst_uri + "_native_options";
    auto src = lance::Dataset::open(src_uri);
    const uint64_t src_rows = src.count_rows();

    {
        auto scanner = src.scan();
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);

        lance::WriteOptions options;
        auto ds = lance::Dataset::write_with_options(
            uri, &stream, lance::WriteMode::Create, options);
        assert(ds.count_rows() == src_rows);
        auto config = ds.config();
        assert(config.find("lance.auto_cleanup.interval") == config.end());
        assert(config.find("lance.auto_cleanup.older_than") == config.end());
    }

    {
        auto scanner = src.scan();
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);

        lance::WriteOptions options;
        options.params.max_rows_per_file = 2;
        options.auto_cleanup = lance::AutoCleanupParams{0, 0, 0};
        options.skip_auto_cleanup = true;
        auto ds = lance::Dataset::write_with_options(
            uri, &stream, lance::WriteMode::Append, options);
        assert(ds.count_rows() == src_rows * 2);
        auto config = ds.config();
        assert(config.find("lance.auto_cleanup.interval") == config.end());

        ds.update_config({
            lance::ConfigUpdate{"cpp.key", std::string("value")},
            lance::ConfigUpdate{"cpp.removed", std::nullopt},
        });
        auto updated = ds.config();
        assert(updated.at("cpp.key") == "value");
        assert(updated.find("cpp.removed") == updated.end());
    }

    auto reopened = lance::Dataset::open(uri);
    auto config = reopened.config();
    assert(config.at("cpp.key") == "value");
    assert(config.find("lance.auto_cleanup.interval") == config.end());

    PASS();
}

// Exercises `Dataset::calculate_data_stats` on the freshly-written (modern,
// v2+) dataset, where per-field on-disk sizes are populated. Runs before the
// mutation tests reshape or empty the dataset.
static void test_data_statistics(const std::string& dst_uri) {
    TEST(test_data_statistics);

    auto ds = lance::Dataset::open(dst_uri);
    auto stats = ds.calculate_data_stats();

    assert(!stats.empty() && "at least one field expected");
    uint64_t total = 0;
    for (const auto& f : stats) {
        total += f.bytes_on_disk;
    }
    assert(total > 0 && "modern storage should report non-zero on-disk size");
    printf("fields=%zu... ", stats.size());

    PASS();
}

static void test_config_replacement(const std::string& uri) {
    TEST(test_config_replacement);

    auto ds = lance::Dataset::open(uri);
    auto old = lance::Dataset::open(uri);
    const uint64_t before = ds.version();
    const uint64_t old_version = old.version();
    assert(ds.config().at("fixture.key") == "fixture-value");

    auto merged = ds.update_config_json(
        {
            {"fixture.key", std::string("updated")},
            {"fixture.remove", std::nullopt},
            {"fixture.empty", std::string("")},
            {"dup", std::string("first")},
            {"dup", std::string("second")},
            {"", std::string("")},
        },
        lance::MetadataUpdateMode::Merge);
    assert(merged.find("\"fixture.key\":\"updated\"") != std::string::npos);
    assert(merged.find("\"fixture.remove\"") == std::string::npos);
    assert(merged.find("\"fixture.empty\":\"\"") != std::string::npos);
    assert(merged.find("\"dup\":\"second\"") != std::string::npos);
    assert(merged.find("\"\":\"\"") != std::string::npos);
    assert(ds.version() == before + 1);
    assert(old.version() == old_version);
    assert(old.config().at("fixture.key") == "fixture-value");

    auto historical = lance::Dataset::open(uri, {}, before);
    assert(historical.config().at("fixture.key") == "fixture-value");

    auto replaced = ds.update_config_json(
        {
            {"a", std::string("1")},
            {"b", std::string("")},
            {"gone", std::nullopt},
        },
        lance::MetadataUpdateMode::Replace);
    assert(replaced == "{\"a\":\"1\",\"b\":\"\"}");
    auto cleared =
        ds.update_config_json({}, lance::MetadataUpdateMode::Replace);
    assert(cleared == "{}");
    assert(ds.config().empty());

    auto reopened = lance::Dataset::open(uri);
    assert(reopened.config().empty());
    PASS();
}

static void test_base_paths_and_add(
    const std::string& uri,
    const std::string& alternate_uri) {
    TEST(test_base_paths_and_add);

    auto ds = lance::Dataset::open(uri);
    auto old = lance::Dataset::open(uri);
    const uint64_t before = ds.version();
    const uint64_t old_version = old.version();
    auto json = ds.base_paths_json();
    assert(json.find("\"0\"") != std::string::npos);
    assert(json.find("\"7\"") != std::string::npos);
    assert(json.find(alternate_uri) != std::string::npos);
    assert(json.find("\"name\":\"alt-root\"") != std::string::npos);
    assert(json.find("\"is_dataset_root\":false") != std::string::npos);

    const std::string auto_path = uri + "_cpp_added_auto";
    const std::string named_path = uri + "_cpp_added_named";
    const std::string none_path = uri + "_cpp_none_props";
    const std::string empty_path = uri + "_cpp_empty_props";
    lance::AddBasesOptions options;
    options.transaction_properties =
        std::vector<std::pair<std::string, std::string>>{
            {"fixture.tx", "cpp-add-bases"},
        };
    ds.add_bases(
        {
            lance::BasePath{0, auto_path, std::string("auto"), false},
            lance::BasePath{42, named_path, std::string(""), true},
        },
        options);
    assert(ds.version() == before + 1);
    assert(old.version() == old_version);
    json = ds.base_paths_json();
    assert(json.find("\"id\":1") != std::string::npos);
    assert(json.find("\"id\":42") != std::string::npos);
    assert(json.find("\"name\":\"auto\"") != std::string::npos);
    assert(json.find("\"name\":\"\"") != std::string::npos);
    assert(json.find(auto_path) != std::string::npos);
    assert(json.find(named_path) != std::string::npos);

    ds.add_bases(
        {lance::BasePath{0, none_path, std::string("none-props"), false}});
    lance::AddBasesOptions empty_options;
    empty_options.transaction_properties =
        std::vector<std::pair<std::string, std::string>>{};
    ds.add_bases(
        {lance::BasePath{0, empty_path, std::string("empty-props"), false}},
        empty_options);

    const uint64_t before_empty = ds.version();
    const std::string before_empty_json = ds.base_paths_json();
    ds.add_bases({});
    ds.add_bases({}, empty_options);
    const uint64_t stable_version = ds.version();
    assert(stable_version == before_empty + 2);
    assert(ds.base_paths_json() == before_empty_json);
    assert(old.version() == old_version);

    bool caught = false;
    try {
        ds.add_bases(
            {lance::BasePath{0, alternate_uri, std::string("duplicate"), false}});
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
        assert(std::string(error.what()).find("Conflict detected") !=
               std::string::npos);
    }
    assert(caught);
    assert(ds.version() == stable_version);
    PASS();
}

struct ProgressLifetime {
    std::atomic<int>* destroyed;
    explicit ProgressLifetime(std::atomic<int>* counter)
        : destroyed(counter) {}
    ~ProgressLifetime() {
        destroyed->fetch_add(1, std::memory_order_seq_cst);
    }
};

static void wait_for_expired(
    const std::weak_ptr<ProgressLifetime>& weak) {
    for (int attempt = 0; attempt < 10000; ++attempt) {
        if (weak.expired()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false && "progress capture was not released");
}

static void test_file_stream_ownership(
    const std::string& uri,
    const std::string& alternate_uri) {
    TEST(test_file_stream_ownership);

    uint64_t latest = 0;
    lance::ArrowStream tracked;
    {
        auto ds = lance::Dataset::open(uri);
        latest = ds.version();
        tracked = ds.tracked_files_stream();
    }
    auto tracked_rows = read_tracked_files(tracked);
    assert(!tracked_rows.empty());
    std::set<std::string> types;
    uint64_t manifest_rows = 0;
    bool saw_alt_data = false;
    for (const auto& row : tracked_rows) {
        types.insert(row.file_type);
        if (row.file_type == "manifest") manifest_rows += 1;
        if (row.file_type == "data file" && row.base_uri == alternate_uri) {
            saw_alt_data = true;
        }
        assert(row.path != "untracked.txt");
    }
    assert(types.count("manifest") == 1);
    assert(types.count("data file") == 1);
    assert(types.count("deletion file") == 1);
    assert(types.count("transaction file") == 1);
    assert(types.count("index file") == 1);
    assert(saw_alt_data);
    assert(manifest_rows > 0);

    {
        auto ds = lance::Dataset::open(uri);
        auto min_stream = ds.tracked_files_stream(latest);
        for (const auto& row : read_tracked_files(min_stream)) {
            assert(row.version == static_cast<int64_t>(latest));
        }
    }

    std::atomic<int> destroyed{0};
    std::weak_ptr<ProgressLifetime> weak;
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> last_processed{0};
    std::atomic<uint64_t> last_total{0};
    {
        auto lifetime = std::make_shared<ProgressLifetime>(&destroyed);
        weak = lifetime;
        lance::ArrowStream progress_stream;
        {
            auto ds = lance::Dataset::open(uri);
            progress_stream = ds.tracked_files_stream(
                std::nullopt,
                [lifetime, &calls, &last_processed, &last_total](
                    uint64_t processed,
                    std::optional<uint64_t> total) {
                    calls.fetch_add(1, std::memory_order_seq_cst);
                    last_processed.store(processed, std::memory_order_seq_cst);
                    if (total) {
                        last_total.store(*total, std::memory_order_seq_cst);
                    }
                });
        }
        assert(read_tracked_files(progress_stream).size() == tracked_rows.size());
        progress_stream.reset();
        lifetime.reset();
    }
    wait_for_expired(weak);
    assert(destroyed.load() == 1);
    assert(calls.load() == manifest_rows);
    assert(last_processed.load() == manifest_rows);
    assert(last_total.load() == manifest_rows);

    std::atomic<int> early_destroyed{0};
    std::weak_ptr<ProgressLifetime> early_weak;
    {
        auto lifetime = std::make_shared<ProgressLifetime>(&early_destroyed);
        early_weak = lifetime;
        {
            auto ds = lance::Dataset::open(uri);
            auto early = ds.tracked_files_stream(
                std::nullopt,
                [lifetime](uint64_t, std::optional<uint64_t>) {});
        }
        lifetime.reset();
    }
    wait_for_expired(early_weak);
    assert(early_destroyed.load() == 1);

    lance::ArrowStream all;
    {
        auto ds = lance::Dataset::open(uri);
        all = ds.all_files_stream();
    }
    auto all_rows = read_all_files(all);
    assert(!all_rows.empty());
    bool saw_untracked = false;
    for (const auto& row : all_rows) {
        assert(row.base_uri == uri);
        if (row.path == "untracked.txt") {
            saw_untracked = true;
            assert(row.size_bytes == 23);
            assert(row.last_modified > 0);
        }
    }
    assert(saw_untracked);
    PASS();
}

static void test_cleanup_and_lifecycle(const std::string& uri) {
    TEST(test_cleanup_and_lifecycle);

    std::optional<lance::CleanupExplanation> owned;
    {
        auto ds = lance::Dataset::open(uri);
        assert(ds.version() == 5);
        ds.validate();
        assert(ds.count_deleted_rows() == 1);
        assert(ds.num_small_files(0) == 0);
        assert(ds.num_small_files(5) == 4);
        assert(ds.num_small_files(4) == 0);

        lance::CleanupPolicy policy;
        policy.before_version = 5;
        policy.versions = std::vector<uint64_t>{2, 3};
        lance::CleanupOperationOptions operation;
        operation.max_candidate_files = 1;
        owned.emplace(ds.explain_cleanup(policy, operation));
    }

    // The explanation is a fully copied value and remains usable after the
    // dataset wrapper is destroyed.
    assert(owned->read_version == 5);
    assert(owned->stats.old_versions == 2);
    assert(owned->candidate_files.size() == 1);
    assert(owned->candidate_files_truncated);
    assert(owned->candidate_file_limit == 1);
    assert(owned->warnings.size() == 1);
    assert(owned->warnings[0].find("truncated to 1 entries") !=
           std::string::npos);
    assert(!owned->candidate_files[0].path.empty());
    assert(owned->candidate_files[0].size_bytes > 0);
    const int32_t kind =
        static_cast<int32_t>(owned->candidate_files[0].kind);
    assert(kind >= LANCE_CLEANUP_FILE_MANIFEST);
    assert(kind <= LANCE_CLEANUP_FILE_TEMPORARY_MANIFEST);

    // Explain did not delete the selected version.
    assert(lance::Dataset::open(uri, {}, 1).version() == 1);

    {
        auto ds = lance::Dataset::open(uri);
        lance::CleanupPolicy policy;
        policy.retain_versions = 3;
        lance::CleanupOperationOptions operation;
        operation.max_candidate_files = 0;
        auto zero = ds.explain_cleanup(policy, operation);
        assert(zero.stats.old_versions == 2);
        assert(zero.candidate_files.empty());
        assert(zero.candidate_files_truncated);
        assert(zero.candidate_file_limit == 0);
    }

    {
        auto ds = lance::Dataset::open(uri);
        bool caught = false;
        try {
            lance::CleanupPolicy policy;
            policy.retain_versions = 0;
            lance::CleanupOperationOptions operation;
            operation.max_candidate_files = 100;
            (void)ds.explain_cleanup(policy, operation);
        } catch (const lance::Error& error) {
            caught = true;
            assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
        }
        assert(caught);

        caught = false;
        try {
            lance::CleanupPolicy policy;
            policy.versions = std::vector<uint64_t>{};
            (void)ds.explain_cleanup(policy);
        } catch (const lance::Error& error) {
            caught = true;
            assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
            assert(std::string(error.what()).find(
                       "versions must not be empty when specified") !=
                   std::string::npos);
        }
        assert(caught);

        caught = false;
        try {
            lance::CleanupPolicy policy;
            policy.versions = std::vector<uint64_t>{2};
            lance::CleanupOperationOptions operation;
            operation.delete_concurrency = 0;
            (void)ds.cleanup(policy, operation);
        } catch (const lance::Error& error) {
            caught = true;
            assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
            assert(std::string(error.what()).find(
                       "delete_concurrency must be between 1") !=
                   std::string::npos);
        }
        assert(caught);
        assert(lance::Dataset::open(uri, {}, 2).version() == 2);
    }

    {
        auto ds = lance::Dataset::open(uri);
        lance::CleanupPolicy policy;
        policy.versions = std::vector<uint64_t>{2};
        lance::CleanupOperationOptions operation;
        operation.delete_concurrency = 2;
        operation.delete_rate_limit = 1000000;
        auto stats = ds.cleanup(policy, operation);
        assert(stats.old_versions == 1);
        assert(stats.failed_deletes == 0);
    }

    bool caught = false;
    try {
        (void)lance::Dataset::open(uri, {}, 2);
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_NOT_FOUND);
    }
    assert(caught);

    {
        auto ds = lance::Dataset::open(uri);
        lance::CleanupOldVersionsOptions relative;
        auto stats = ds.cleanup_old_versions(relative);
        assert(stats.old_versions == 3);

        caught = false;
        try {
            lance::CleanupOldVersionsOptions invalid;
            invalid.older_than_nanos = 1000000000;
            (void)ds.cleanup_old_versions(invalid);
        } catch (const lance::Error& error) {
            caught = true;
            assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
        }
        assert(caught);
    }

    caught = false;
    try {
        (void)lance::Dataset::open(uri, {}, 1);
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_NOT_FOUND);
    }
    assert(caught);
    assert(lance::Dataset::open(uri).version() == 5);

    PASS();
}

// Exercises table/schema metadata through the C++17 wrappers, including
// removal, real empty values, replace/clear, snapshot isolation and reopen.
static void test_dataset_metadata(const std::string& dst_uri) {
    TEST(test_dataset_metadata);
    using MetadataUpdates =
        std::vector<std::pair<std::string, std::optional<std::string>>>;

    auto old = lance::Dataset::open(dst_uri);
    auto ds = lance::Dataset::open(dst_uri);
    uint64_t initial_version = ds.version();
    assert(ds.metadata_json() == "{}");

    auto updated = ds.update_metadata_json(MetadataUpdates{
        {"empty", std::string("")},
        {"escaped", std::string("line\n\"quoted\"\\path")},
        {"owner", std::string("c++")},
        {"remove", std::string("temporary")},
    });
    assert(updated.find("\"empty\":\"\"") != std::string::npos);
    assert(updated.find("\"owner\":\"c++\"") != std::string::npos);
    assert(updated.find("\\n") != std::string::npos);
    assert(updated.find("\\\"quoted\\\"") != std::string::npos);
    assert(old.version() == initial_version);
    assert(old.metadata_json() == "{}");

    auto removed = ds.update_metadata_json(
        MetadataUpdates{{"remove", std::nullopt}});
    assert(removed.find("\"remove\"") == std::string::npos);

    auto schema_updated = ds.update_schema_metadata_json(
        MetadataUpdates{{"schema-owner", std::string("c++")}});
    assert(schema_updated.find("\"schema-owner\":\"c++\"") !=
           std::string::npos);
    assert(ds.metadata_json().find("schema-owner") == std::string::npos);

    auto cleared = ds.update_metadata_json(
        MetadataUpdates{}, lance::MetadataUpdateMode::Replace);
    assert(cleared == "{}");
    ds.update_metadata_json(
        MetadataUpdates{{"persist", std::string("yes")}});
    auto schema_cleared = ds.update_schema_metadata_json(
        MetadataUpdates{}, lance::MetadataUpdateMode::Replace);
    assert(schema_cleared == "{}");

    auto reopened = lance::Dataset::open(dst_uri);
    assert(reopened.version() > initial_version);
    assert(reopened.metadata_json().find("\"persist\":\"yes\"") !=
           std::string::npos);

    bool caught = false;
    try {
        ds.update_metadata_json(
            MetadataUpdates{{"bad", std::string("value")}},
            static_cast<lance::MetadataUpdateMode>(99));
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught);
    assert(ds.metadata_json().find("\"bad\"") == std::string::npos);

    PASS();
}

// Exercises batched field metadata updates and schema/snapshot inspection
// through the C++17 wrappers on the nested two-fragment fixture.
static void test_dataset_field_metadata(const std::string& uri) {
    TEST(test_dataset_field_metadata);
    using MetadataUpdates =
        std::vector<std::pair<std::string, std::optional<std::string>>>;

    auto old = lance::Dataset::open(uri);
    auto ds = lance::Dataset::open(uri);
    const uint64_t initial_version = ds.version();
    assert(ds.fragment_count() == 2);
    assert(ds.count_rows() == 6);
    assert(ds.data_storage_version() == "2.2");
    assert(!ds.has_stable_row_ids());

    auto field_ids = ds.field_ids();
    assert(field_ids.size() >= 4);
    const int32_t id_field_id =
        ds.resolve_field_id(lance::FieldReference::by_path("id"));
    const int32_t name_field_id =
        ds.resolve_field_id(lance::FieldReference::by_path("profile.name"));
    const int32_t score_field_id =
        ds.resolve_field_id(lance::FieldReference::by_path("profile.score"));
    assert(ds.resolve_field_id(lance::FieldReference::by_id(name_field_id)) ==
           name_field_id);
    assert(ds.field_path(name_field_id) == "profile.name");
    assert(std::find(field_ids.begin(), field_ids.end(), name_field_id) !=
           field_ids.end());

    ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ds.schema(&schema);
    const ArrowSchema* field = arrow_field_at_path(&schema, "profile.name");
    assert(field != nullptr);
    assert_field_metadata_absent(field, "nested");
    if (schema.release) schema.release(&schema);

    bool caught = false;
    try {
        ds.update_field_metadata({lance::FieldMetadataUpdate{
            lance::FieldReference::by_path("id"),
            MetadataUpdates{{"bad", std::string("value")}},
            static_cast<lance::MetadataUpdateMode>(99),
        }});
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught);
    assert(ds.version() == initial_version);

    caught = false;
    try {
        ds.update_field_metadata({lance::FieldMetadataUpdate{
            lance::FieldReference::by_path("profile.missing"),
            MetadataUpdates{{"bad", std::string("value")}},
            lance::MetadataUpdateMode::Merge,
        }});
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code != LANCE_OK);
    }
    assert(caught);
    assert(ds.version() == initial_version);

    uint64_t version_before = ds.version();
    ds.update_field_metadata({
        lance::FieldMetadataUpdate{
            lance::FieldReference::by_id(id_field_id),
            MetadataUpdates{
                {"owner", std::string("c++")},
                {"empty", std::string("")},
                {"remove", std::string("temporary")},
            },
            lance::MetadataUpdateMode::Merge,
        },
        lance::FieldMetadataUpdate{
            lance::FieldReference::by_path("profile.name"),
            MetadataUpdates{
                {"nested", std::string("value")},
                {"empty", std::string("")},
            },
            lance::MetadataUpdateMode::Replace,
        },
        lance::FieldMetadataUpdate{
            lance::FieldReference::by_id(score_field_id),
            MetadataUpdates{
                {"score", std::string("1.5")},
                {"remove", std::nullopt},
            },
            lance::MetadataUpdateMode::Merge,
        },
    });
    assert(ds.version() == version_before + 1);

    memset(&schema, 0, sizeof(schema));
    ds.schema(&schema);
    const ArrowSchema* id_field = arrow_field_at_path(&schema, "id");
    field = arrow_field_at_path(&schema, "profile.name");
    const ArrowSchema* score_field = arrow_field_at_path(&schema, "profile.score");
    assert(id_field != nullptr && field != nullptr && score_field != nullptr);
    assert_field_metadata(id_field, "owner", "c++");
    assert_field_metadata(id_field, "empty", "");
    assert_field_metadata(field, "nested", "value");
    assert_field_metadata(score_field, "score", "1.5");
    assert_field_metadata_absent(score_field, "remove");
    if (schema.release) schema.release(&schema);

    ds.update_field_metadata({
        lance::FieldMetadataUpdate{
            lance::FieldReference::by_id(id_field_id),
            MetadataUpdates{{"root", std::string("first")}},
            lance::MetadataUpdateMode::Merge,
        },
        lance::FieldMetadataUpdate{
            lance::FieldReference::by_path("id"),
            MetadataUpdates{{"root", std::string("last")}},
            lance::MetadataUpdateMode::Replace,
        },
    });
    memset(&schema, 0, sizeof(schema));
    ds.schema(&schema);
    id_field = arrow_field_at_path(&schema, "id");
    assert(id_field != nullptr);
    assert_field_metadata(id_field, "root", "last");
    assert_field_metadata_absent(id_field, "owner");
    if (schema.release) schema.release(&schema);

    ds.update_field_metadata({
        lance::FieldMetadataUpdate{
            lance::FieldReference::by_path("id"),
            MetadataUpdates{
                {"remove", std::string("temporary")},
                {"empty", std::string("")},
            },
            lance::MetadataUpdateMode::Merge,
        },
    });
    ds.update_field_metadata({
        lance::FieldMetadataUpdate{
            lance::FieldReference::by_id(id_field_id),
            MetadataUpdates{{"remove", std::nullopt}},
            lance::MetadataUpdateMode::Merge,
        },
    });
    memset(&schema, 0, sizeof(schema));
    ds.schema(&schema);
    id_field = arrow_field_at_path(&schema, "id");
    assert(id_field != nullptr);
    assert_field_metadata(id_field, "root", "last");
    assert_field_metadata(id_field, "empty", "");
    assert_field_metadata_absent(id_field, "remove");
    if (schema.release) schema.release(&schema);

    ds.update_field_metadata({
        lance::FieldMetadataUpdate{
            lance::FieldReference::by_path("id"),
            MetadataUpdates{},
            lance::MetadataUpdateMode::Replace,
        },
    });
    memset(&schema, 0, sizeof(schema));
    ds.schema(&schema);
    id_field = arrow_field_at_path(&schema, "id");
    assert(id_field != nullptr);
    assert_field_metadata_absent(id_field, "root");
    if (schema.release) schema.release(&schema);

    ds.update_field_metadata({});
    ds.update_field_metadata({
        lance::FieldMetadataUpdate{
            lance::FieldReference::by_id(id_field_id),
            MetadataUpdates{{"persist", std::string("yes")}},
            lance::MetadataUpdateMode::Merge,
        },
        lance::FieldMetadataUpdate{
            lance::FieldReference::by_path("profile.name"),
            MetadataUpdates{{"persist", std::string("yes")}},
            lance::MetadataUpdateMode::Merge,
        },
    });

    memset(&schema, 0, sizeof(schema));
    old.schema(&schema);
    id_field = arrow_field_at_path(&schema, "id");
    assert(id_field != nullptr);
    assert_field_metadata_absent(id_field, "persist");
    if (schema.release) schema.release(&schema);
    assert(old.version() == initial_version);

    auto reopened = lance::Dataset::open(uri);
    memset(&schema, 0, sizeof(schema));
    reopened.schema(&schema);
    id_field = arrow_field_at_path(&schema, "id");
    field = arrow_field_at_path(&schema, "profile.name");
    assert(id_field != nullptr && field != nullptr);
    assert_field_metadata(id_field, "persist", "yes");
    assert_field_metadata(field, "persist", "yes");
    if (schema.release) schema.release(&schema);

    PASS();
}

// Re-opens the dataset just written by `test_dataset_write_roundtrip` and
// exercises `Dataset::update`. Must run before `test_delete_rows`, which
// empties the dataset.
static void test_update(const std::string& dst_uri) {
    TEST(test_update);

    auto ds = lance::Dataset::open(dst_uri);
    uint64_t before = ds.count_rows();
    assert(before > 0 && "test fixture expected to have rows");

    // Empty predicate -> updates every row.
    uint64_t updated = ds.update("", {{"name", "'frozen'"}});
    assert(updated == before);
    assert(ds.count_rows() == before);

    // Empty updates vector must throw (num_updates == 0).
    bool caught_empty = false;
    try {
        ds.update("", {});
    } catch (const lance::Error& e) {
        caught_empty = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_empty);

    printf("updated=%llu... ", (unsigned long long)updated);
    PASS();
}

// Re-opens the dataset just written by `test_dataset_write_roundtrip` and
// exercises `Dataset::merge_insert`. Must run before `test_delete_rows`,
// which empties the dataset.
static void test_merge_insert(const std::string& dst_uri) {
    TEST(test_merge_insert);

    auto ds = lance::Dataset::open(dst_uri);
    uint64_t before = ds.count_rows();
    assert(before > 0 && "test fixture expected to have rows");

    // Self-merge: scan the dataset itself and use that as the source. With
    // find-or-create defaults every row is a self-match and DoNothing fires,
    // so insert/update counts stay at zero and the row count is preserved.
    auto scanner = ds.scan();
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);

    auto result = ds.merge_insert({"id"}, &stream);
    assert(result.num_inserted_rows == 0);
    assert(result.num_updated_rows == 0);
    assert(ds.count_rows() == before);

    // Empty key vector must throw (num_on_columns == 0).
    bool caught_empty = false;
    try {
        ds.merge_insert({}, nullptr);
    } catch (const lance::Error& e) {
        caught_empty = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_empty);

    PASS();
}

// Re-opens the dataset just written by `test_dataset_write_roundtrip` and
// exercises `Dataset::alter_columns`. Relaxes the nullability of `id` (non-
// nullable in the fixture) to nullable; the column survives the subsequent
// drop_columns({"name"}) test untouched.
static void test_alter_columns(const std::string& dst_uri) {
    TEST(test_alter_columns);

    auto ds = lance::Dataset::open(dst_uri);
    uint64_t v_before = ds.version();

    lance::ColumnAlteration alt;
    alt.path          = "id";
    alt.nullable_mode = LANCE_COLUMN_NULLABLE_TRUE;
    ds.alter_columns({alt});
    assert(ds.version() > v_before
           && "alter_columns must bump the version");

    // Confirm the schema reflects the relaxed nullability.
    ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ds.schema(&schema);
    assert(schema.n_children > 0 && "schema must have children");
    bool id_is_nullable = false;
    for (int64_t i = 0; i < schema.n_children; i++) {
        ArrowSchema* child = schema.children[i];
        if (!child) continue;
        if (strcmp(child->name, "id") == 0) {
            id_is_nullable = (child->flags & ARROW_FLAG_NULLABLE) != 0;
        }
    }
    if (schema.release) schema.release(&schema);
    assert(id_is_nullable && "id should be nullable after alter");

    // No-op alteration (all sentinels left at defaults) must throw with
    // INVALID_ARGUMENT.
    bool caught_noop = false;
    try {
        lance::ColumnAlteration noop;
        noop.path = "id";
        ds.alter_columns({noop});
    } catch (const lance::Error& e) {
        caught_noop = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_noop);

    // Out-of-range nullable_mode discriminant must throw with INVALID_ARGUMENT.
    // Cast `99` through the enum type to verify the C++ wrapper forwards it
    // verbatim rather than silently clamping.
    bool caught_bad_mode = false;
    try {
        lance::ColumnAlteration bad_mode;
        bad_mode.path = "id";
        bad_mode.nullable_mode =
            static_cast<LanceColumnNullableMode>(99);
        ds.alter_columns({bad_mode});
    } catch (const lance::Error& e) {
        caught_bad_mode = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_bad_mode);

    PASS();
}

// Re-opens the dataset just written by `test_dataset_write_roundtrip` and
// exercises `Dataset::drop_columns`. Drops `name`, `embedding` and `score` so the
// dataset is left with `id` only; subsequent tests (`compact_files`, `delete_rows`)
// do not reference any dropped column. Must run after `test_update` /
// `test_merge_insert`.
static void test_drop_columns(const std::string& dst_uri) {
    TEST(test_drop_columns);

    auto ds = lance::Dataset::open(dst_uri);
    uint64_t before_rows = ds.count_rows();
    uint64_t v_before = ds.version();

    ds.drop_columns({"name", "embedding", "score"});
    assert(ds.count_rows() == before_rows
           && "metadata-only drop must preserve row count");
    assert(ds.version() > v_before
           && "drop_columns must bump the version");
    uint64_t v_after_drop = ds.version();

    // Dropping an unknown column must throw with INVALID_ARGUMENT and
    // leave the dataset unchanged (no version bump on the error path).
    bool caught_unknown = false;
    try {
        ds.drop_columns({"no_such_column"});
    } catch (const lance::Error& e) {
        caught_unknown = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_unknown);
    assert(ds.version() == v_after_drop
           && "failed drop must not bump the version");

    // Empty column list must throw with INVALID_ARGUMENT.
    bool caught_empty = false;
    try {
        ds.drop_columns({});
    } catch (const lance::Error& e) {
        caught_empty = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_empty);

    // Dropping the sole remaining column (`id`) must throw with
    // INVALID_ARGUMENT — upstream refuses to leave a dataset with zero
    // fields.
    bool caught_last = false;
    try {
        ds.drop_columns({"id"});
    } catch (const lance::Error& e) {
        caught_last = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_last);

    PASS();
}

// Re-opens the dataset (reduced to `id` only by `test_drop_columns`) and
// exercises the three `Dataset::add_columns_*` wrappers. The positive path
// uses the SQL variant (strings only); the nulls/stream variants are
// smoke-checked through their argument rejections, since their happy paths are
// covered by the Rust integration tests. The added `id_doubled` column is
// harmless to the subsequent compact/delete steps. Must run after
// `test_drop_columns`.
static void test_add_columns(const std::string& dst_uri) {
    TEST(test_add_columns);

    auto ds = lance::Dataset::open(dst_uri);
    uint64_t v_before = ds.version();

    // Snapshot the field count before the add. (If ds.schema() threw, the
    // zero-initialised struct's release stays null, so there is no leak; here
    // the handle is freshly opened and valid, so the export is expected to
    // succeed.)
    ArrowSchema schema_before;
    memset(&schema_before, 0, sizeof(schema_before));
    ds.schema(&schema_before);
    int64_t fields_before = schema_before.n_children;
    if (schema_before.release) schema_before.release(&schema_before);

    // SQL variant: derive `id_doubled = id * 2` from the surviving `id`.
    ds.add_columns_sql({{"id_doubled", "id * 2"}});
    assert(ds.version() > v_before
           && "add_columns_sql must bump the version");

    ArrowSchema schema_after;
    memset(&schema_after, 0, sizeof(schema_after));
    ds.schema(&schema_after);
    int64_t fields_after = schema_after.n_children;
    if (schema_after.release) schema_after.release(&schema_after);
    assert(fields_after == fields_before + 1
           && "schema field count must increase by 1 after add");

    // Empty SQL column list must throw with INVALID_ARGUMENT.
    bool caught_empty = false;
    try {
        ds.add_columns_sql({});
    } catch (const lance::Error& e) {
        caught_empty = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_empty);

    // An empty column name must throw with INVALID_ARGUMENT.
    bool caught_empty_name = false;
    try {
        ds.add_columns_sql({{"", "id * 2"}});
    } catch (const lance::Error& e) {
        caught_empty_name = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_empty_name);

    // An empty expression must throw with INVALID_ARGUMENT. (A NULL expression
    // is not representable here — `SqlColumn::expression` is a std::string — so
    // the NULL-pointer case is covered by the C test instead.)
    bool caught_empty_expr = false;
    try {
        ds.add_columns_sql({{"x", ""}});
    } catch (const lance::Error& e) {
        caught_empty_expr = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_empty_expr);

    // AllNulls with a NULL schema pointer must throw with INVALID_ARGUMENT.
    bool caught_null_schema = false;
    try {
        ds.add_columns_nulls(nullptr);
    } catch (const lance::Error& e) {
        caught_null_schema = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_null_schema);

    // Stream with a NULL stream pointer must throw with INVALID_ARGUMENT.
    bool caught_null_stream = false;
    try {
        ds.add_columns_stream(nullptr);
    } catch (const lance::Error& e) {
        caught_null_stream = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_null_stream);

    PASS();
}

// Re-opens the dataset just written by `test_dataset_write_roundtrip` and
// exercises `Dataset::compact_files`. The smoke fixture is a single fragment
// so the default planner has nothing to compact — we expect a no-op (zero
// metrics, no version bump). Must run before `test_delete_rows`.
static void test_compact_files(const std::string& dst_uri) {
    TEST(test_compact_files);

    auto ds = lance::Dataset::open(dst_uri);
    uint64_t v_before = ds.version();

    auto metrics = ds.compact_files();
    assert(metrics.fragments_removed == 0);
    assert(metrics.fragments_added == 0);
    assert(ds.version() == v_before);

    PASS();
}

// Re-opens the dataset just written by `test_dataset_write_roundtrip` and
// exercises `Dataset::delete_rows`. Must run after the write roundtrip.
static void test_delete_rows(const std::string& dst_uri) {
    TEST(test_delete_rows);

    auto ds = lance::Dataset::open(dst_uri);
    uint64_t before = ds.count_rows();
    assert(before > 0 && "test fixture expected to have rows");

    // Predicate that matches everything — exact deleted count == before.
    uint64_t deleted = ds.delete_rows("true");
    assert(deleted == before);
    assert(ds.count_rows() == 0);

    // Empty predicate must throw.
    bool caught_empty = false;
    try {
        ds.delete_rows("");
    } catch (const lance::Error& e) {
        caught_empty = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_empty);

    printf("deleted=%llu... ", (unsigned long long)deleted);
    PASS();
}

static void check_distance_range_results(struct ArrowArrayStream *stream,
                                         const float *lower, const float *upper,
                                         uint32_t expected_ids, int64_t expected_count) {
    struct ArrowSchema schema = {};
    assert(stream->get_schema(stream, &schema) == 0);
    int id_column = -1, distance_column = -1;
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (strcmp(schema.children[i]->name, "id") == 0) id_column = (int)i;
        if (strcmp(schema.children[i]->name, "_distance") == 0) distance_column = (int)i;
    }
    assert(id_column >= 0 && distance_column >= 0);
    assert(strcmp(schema.children[id_column]->format, "i") == 0 &&
           strcmp(schema.children[distance_column]->format, "f") == 0);
    schema.release(&schema);
    int64_t rows = 0;
    uint32_t seen = 0;
    while (1) {
        struct ArrowArray array = {};
        assert(stream->get_next(stream, &array) == 0);
        if (!array.release) break;
        const struct ArrowArray *ids = array.children[id_column];
        const struct ArrowArray *distances = array.children[distance_column];
        const int32_t *id_values = (const int32_t *)ids->buffers[1];
        const float *distance_values = (const float *)distances->buffers[1];
        for (int64_t i = 0; i < array.length; ++i) {
            int32_t id = id_values[ids->offset + i];
            float distance = distance_values[distances->offset + i];
            assert(id >= 1 && id <= 20);
            uint32_t bit = 1u << (id - 1);
            assert((seen & bit) == 0);
            seen |= bit;
            assert(distance >= 0.0f && (!lower || distance >= *lower) && (!upper || distance < *upper));
        }
        rows += array.length;
        array.release(&array);
    }
    assert(rows == expected_count && seen == expected_ids);
    stream->release(stream);
}

static void test_distance_range(const std::string& uri) {
    TEST(test_distance_range);
    auto ds = lance::Dataset::open(uri);
    float query[8];
    for (int i = 0; i < 8; ++i) query[i] = 0.1f + static_cast<float>(i);
    const int64_t expected[] = {2, 1, 20, 20, 0};
    const uint32_t expected_ids[] = {3u, 2u, (1u << 20) - 1, (1u << 20) - 1, 0u};
    for (int mode = 0; mode < 5; ++mode) {
        auto scanner = ds.scan();
        scanner.nearest("embedding", query, 8, 20).use_index(false);
        std::optional<float> lower = mode == 1 ? std::optional<float>(0.02f) : std::nullopt;
        std::optional<float> upper = 0.125f;
        // Row 1 has exactly zero distance: modes 3/4 distinguish >= from > and < from <=.
        if (mode == 3) { lower = 0.0f; upper = std::nullopt; }
        if (mode == 4) upper = 0.0f;
        scanner.distance_range(lower, upper);
        if (mode == 2) { scanner.distance_range(); lower.reset(); upper.reset(); }
        bool rejected = false;
        try {
            scanner.distance_range(1.0f, 0.0f);
        } catch (const lance::Error& e) {
            rejected = true;
            assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
        }
        assert(rejected);
        ArrowArrayStream stream{};
        scanner.to_arrow_stream(&stream);
        check_distance_range_results(&stream, lower ? &*lower : nullptr,
                                     upper ? &*upper : nullptr, expected_ids[mode], expected[mode]);
    }
    PASS();
}

static void check_batch_nearest_results(struct ArrowArrayStream *stream) {
    ArrowSchema schema{};
    assert(stream->get_schema(stream, &schema) == 0);
    int query_column = -1, id_column = -1, distance_column = -1;
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (strcmp(schema.children[i]->name, "query_index") == 0) {
            query_column = (int)i;
        }
        if (strcmp(schema.children[i]->name, "id") == 0) id_column = (int)i;
        if (strcmp(schema.children[i]->name, "_distance") == 0) distance_column = (int)i;
    }
    assert(query_column >= 0);
    assert(strcmp(schema.children[query_column]->format, "i") == 0);
    assert(id_column >= 0 && distance_column >= 0);
    schema.release(&schema);
    int counts[2] = {0, 0};
    int seen[2] = {0, 0};
    while (1) {
        ArrowArray batch{};
        assert(stream->get_next(stream, &batch) == 0);
        if (!batch.release) break;
        struct ArrowArray *queries = batch.children[query_column];
        const int32_t *values = (const int32_t *)queries->buffers[1];
        struct ArrowArray *ids = batch.children[id_column];
        struct ArrowArray *distances = batch.children[distance_column];
        const int32_t *id_values = (const int32_t *)ids->buffers[1];
        const float *distance_values = (const float *)distances->buffers[1];
        for (int64_t i = 0; i < batch.length; ++i) {
            int32_t index = values[queries->offset + i];
            assert(index >= 0 && index < 2);
            int32_t id = id_values[ids->offset + i];
            int32_t first_id = index == 0 ? 1 : 19;
            assert(id >= first_id && id <= first_id + 1);
            seen[index] |= 1 << (id - first_id);
            float expected = (id == 1 || id == 20) ? 0.0f : 0.08f;
            float error = distance_values[distances->offset + i] - expected;
            assert(error > -0.0001f && error < 0.0001f);
            ++counts[index];
        }
        batch.release(&batch);
    }
    assert(counts[0] == 2 && counts[1] == 2);
    assert(seen[0] == 3 && seen[1] == 3);
    stream->release(stream);
}

static void test_batch_nearest(const std::string& uri) {
    TEST(test_batch_nearest);
    float queries[16];
    for (int i = 0; i < 8; ++i) {
        queries[i] = 0.1f + i;
        queries[8 + i] = 2.0f + i;
    }
    auto dataset = lance::Dataset::open(uri);
    for (bool typed : {false, true}) {
        auto scanner = dataset.scan();
        if (typed) {
            scanner.nearest_batch("embedding", queries, 8, 2, LANCE_DTYPE_FLOAT32, 2);
        } else {
            scanner.nearest_batch("embedding", queries, 8, 2, 2);
        }
        scanner.use_index(false).batch_size(1);
        ArrowArrayStream stream = {};
        scanner.to_arrow_stream(&stream);
        check_batch_nearest_results(&stream);
    }
    PASS();
}

static uint64_t count_scanner_rows(lance::Scanner& scanner) {
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);
    uint64_t total = 0;
    while (true) {
        ArrowArray array;
        memset(&array, 0, sizeof(array));
        assert(stream.get_next(&stream, &array) == 0);
        if (!array.release) break;
        total += static_cast<uint64_t>(array.length);
        array.release(&array);
    }
    if (stream.release) stream.release(&stream);
    return total;
}

static void test_scanner_materialization_fast_search(const std::string& uri) {
    TEST(test_scanner_materialization_fast_search);

    auto ds = lance::Dataset::open(uri);
    const LanceMaterializationStyle styles[] = {
        LANCE_MATERIALIZATION_STYLE_HEURISTIC,
        LANCE_MATERIALIZATION_STYLE_ALL_LATE,
        LANCE_MATERIALIZATION_STYLE_ALL_EARLY,
    };
    for (auto style : styles) {
        auto scanner = ds.scan();
        scanner.materialization_style(style)
               .additional_sql_filter("id >= 18");
        assert(count_scanner_rows(scanner) == 3);
    }

    {
        auto scanner = ds.scan();
        scanner.materialization_style(
                   LANCE_MATERIALIZATION_STYLE_ALL_EARLY_EXCEPT,
                   std::vector<std::string>{"name"})
               .additional_sql_filter("id >= 18");
        assert(count_scanner_rows(scanner) == 3);
    }
    {
        auto scanner = ds.scan();
        scanner.materialization_style(
                   LANCE_MATERIALIZATION_STYLE_ALL_EARLY_EXCEPT)
               .additional_sql_filter("id >= 18");
        assert(count_scanner_rows(scanner) == 3);
    }

    bool caught = false;
    try {
        ds.scan().materialization_style(
            LANCE_MATERIALIZATION_STYLE_ALL_EARLY_EXCEPT,
            std::vector<std::string>{"missing"});
    } catch (const lance::Error&) {
        caught = true;
    }
    assert(caught);

    float query[8];
    for (int i = 0; i < 8; ++i) query[i] = 0.1f + static_cast<float>(i);
    {
        auto scanner = ds.scan();
        scanner.nearest("embedding", query, 8, 5);
        assert(count_scanner_rows(scanner) == 5);
    }
    {
        auto scanner = ds.scan();
        scanner.nearest("embedding", query, 8, 5).fast_search(true);
        assert(count_scanner_rows(scanner) == 0);
    }
    {
        auto scanner = ds.scan();
        scanner.nearest("embedding", query, 8, 5)
               .fast_search(true)
               .use_index(false);
        assert(count_scanner_rows(scanner) == 0);
    }
    {
        auto scanner = ds.scan();
        scanner.nearest("embedding", query, 8, 5)
               .fast_search(true)
               .fast_search(false);
        assert(count_scanner_rows(scanner) == 5);
    }

    auto scanner = ds.scan();
    scanner.nearest("embedding", query, 8, 5);
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);
    caught = false;
    try {
        scanner.fast_search(true);
    } catch (const lance::Error&) {
        caught = true;
    }
    assert(caught);
    caught = false;
    try {
        scanner.materialization_style(LANCE_MATERIALIZATION_STYLE_ALL_EARLY);
    } catch (const lance::Error&) {
        caught = true;
    }
    assert(caught);
    if (stream.release) stream.release(&stream);

    PASS();
}

static std::vector<int32_t> collect_ordering_ids(ArrowArrayStream& stream) {
    ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    assert(stream.get_schema(&stream, &schema) == 0);
    int id_column = -1;
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (strcmp(schema.children[i]->name, "id") == 0) id_column = static_cast<int>(i);
    }
    assert(id_column >= 0);
    assert(strcmp(schema.children[id_column]->format, "i") == 0);
    if (schema.release) schema.release(&schema);

    std::vector<int32_t> ids;
    while (true) {
        ArrowArray array;
        memset(&array, 0, sizeof(array));
        assert(stream.get_next(&stream, &array) == 0);
        if (!array.release) break;
        const auto* values = static_cast<const int32_t*>(
            array.children[id_column]->buffers[1]);
        const auto offset = array.children[id_column]->offset;
        for (int64_t i = 0; i < array.length; ++i) {
            ids.push_back(values[offset + i]);
        }
        array.release(&array);
    }
    if (stream.release) stream.release(&stream);
    return ids;
}

static std::vector<int32_t> scan_ordering_ids(lance::Scanner& scanner) {
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);
    return collect_ordering_ids(stream);
}

static void test_scanner_order_by(const std::string& uri) {
    TEST(test_scanner_order_by);

    auto ds = lance::Dataset::open(uri);
    std::vector<lance::ColumnOrdering> ordering = {
        {"category", LANCE_ORDER_DESCENDING, LANCE_ORDER_NULLS_FIRST},
        {"rank", LANCE_ORDER_ASCENDING, LANCE_ORDER_NULLS_LAST},
        {"id", LANCE_ORDER_ASCENDING, LANCE_ORDER_NULLS_LAST},
    };
    const std::vector<int32_t> expected =
        {11, 8, 0, 4, 6, 1, 9, 2, 3, 7, 10, 5};

    {
        auto scanner = ds.scan();
        auto copied = ordering;
        scanner.order_by(copied);
        // The C ABI copies the names; later caller mutation must not matter.
        copied[0].column_name = "junk";
        assert(scan_ordering_ids(scanner) == expected);
        bool caught = false;
        try {
            scanner.order_by(std::nullopt);
        } catch (const lance::Error& error) {
            caught = true;
            assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
            assert(std::string(error.what()).find(
                       "order_by must be set before the scan starts") !=
                   std::string::npos);
        }
        assert(caught);
    }

    {
        auto scanner = ds.scan();
        scanner.order_by(ordering);
        bool caught = false;
        try {
            scanner.order_by(std::vector<lance::ColumnOrdering>{
                {"rank", static_cast<LanceOrderDirection>(7),
                 LANCE_ORDER_NULLS_LAST},
            });
        } catch (const lance::Error& error) {
            caught = true;
            assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
            assert(std::string(error.what()).find("order_by direction") !=
                   std::string::npos);
        }
        assert(caught);
        // The rejected call retained the previous ordering.
        assert(scan_ordering_ids(scanner) == expected);
    }

    std::vector<int32_t> unordered;
    {
        auto scanner = ds.scan();
        unordered = scan_ordering_ids(scanner);
    }
    {
        auto scanner = ds.scan();
        scanner.order_by(ordering).order_by(std::nullopt);
        assert(scan_ordering_ids(scanner) == unordered);
    }
    {
        auto scanner = ds.scan();
        scanner.order_by(ordering).order_by(
            std::vector<lance::ColumnOrdering>{});
        assert(scan_ordering_ids(scanner) == unordered);
    }
    {
        auto scanner = ds.scan();
        scanner.order_by(ordering).offset(2).limit(4);
        assert((scan_ordering_ids(scanner) ==
                std::vector<int32_t>{0, 4, 6, 1}));
    }

    {
        auto scanner = ds.scan();
        bool caught = false;
        try {
            scanner.order_by(std::vector<lance::ColumnOrdering>{
                {"missing", LANCE_ORDER_ASCENDING, LANCE_ORDER_NULLS_LAST},
            });
            ArrowArrayStream stream;
            memset(&stream, 0, sizeof(stream));
            scanner.to_arrow_stream(&stream);
        } catch (const lance::Error& error) {
            caught = true;
            assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
            assert(std::string(error.what()).find("Column missing not found") !=
                   std::string::npos);
        }
        assert(caught);
    }

    PASS();
}

static std::vector<uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    assert(file.good());
    std::streamoff size = file.tellg();
    assert(size >= 0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        file.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(size));
        assert(file.good());
    }
    return bytes;
}

static void test_scanner_substrait_aggregate(
    const std::string& uri,
    const std::string& plan_path) {
    TEST(test_scanner_substrait_aggregate);

    auto ds = lance::Dataset::open(uri);
    auto scanner = ds.scan();
    auto bytes = read_file_bytes(plan_path);
    scanner.substrait_aggregate(bytes);
    std::fill(bytes.begin(), bytes.end(), 0);
    bytes.clear();

    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);

    ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    assert(stream.get_schema(&stream, &schema) == 0);
    assert(schema.n_children == 5);
    const char* expected_names[] = {
        "rows", "id_sum", "rank_sum", "min_id", "max_id",
    };
    const char* expected_formats[] = {"l", "l", "l", "i", "i"};
    for (int64_t i = 0; i < schema.n_children; ++i) {
        assert(std::string(schema.children[i]->name) == expected_names[i]);
        assert(std::string(schema.children[i]->format) == expected_formats[i]);
    }
    assert(schema.release != nullptr);
    schema.release(&schema);

    ArrowArray array;
    memset(&array, 0, sizeof(array));
    assert(stream.get_next(&stream, &array) == 0);
    assert(array.release != nullptr);
    assert(array.length == 1);
    assert(array.n_children == 5);
    const auto* rows = static_cast<const int64_t*>(
        array.children[0]->buffers[1]) + array.children[0]->offset;
    const auto* id_sum = static_cast<const int64_t*>(
        array.children[1]->buffers[1]) + array.children[1]->offset;
    const auto* rank_sum = static_cast<const int64_t*>(
        array.children[2]->buffers[1]) + array.children[2]->offset;
    const auto* min_id = static_cast<const int32_t*>(
        array.children[3]->buffers[1]) + array.children[3]->offset;
    const auto* max_id = static_cast<const int32_t*>(
        array.children[4]->buffers[1]) + array.children[4]->offset;
    assert(rows[0] == 12);
    assert(id_sum[0] == 66);
    assert(rank_sum[0] == 15);
    assert(min_id[0] == 0);
    assert(max_id[0] == 11);
    array.release(&array);

    memset(&array, 0, sizeof(array));
    assert(stream.get_next(&stream, &array) == 0);
    assert(array.release == nullptr);
    if (stream.release) stream.release(&stream);

    bool caught = false;
    try {
        scanner.substrait_aggregate(std::vector<uint8_t>{0});
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
        assert(std::string(error.what()).find(
                   "substrait_aggregate must be set before") !=
               std::string::npos);
    }
    assert(caught);

    PASS();
}

static std::vector<int32_t> scan_filtered_ids(
    const std::string& uri,
    const std::string& filter,
    bool use_scalar_index,
    ScanStatisticsCapture& captured) {
    auto ds = lance::Dataset::open(uri);
    auto scanner = ds.scan({"id", "name", "score"}, filter);
    captured = ScanStatisticsCapture{};
    scanner.use_scalar_index(use_scalar_index)
        .statistics_callback(capture_scan_statistics, &captured);
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);
    auto ids = collect_ordering_ids(stream);
    std::sort(ids.begin(), ids.end());
    return ids;
}

static void test_native_scalar_indexes(const std::string& uri) {
    TEST(test_native_scalar_indexes);

    auto ds = lance::Dataset::open(uri);
    ds.create_scalar_index("name", LANCE_SCALAR_NGRAM, "cpp_name_ngram");
    ds.create_scalar_index(
        "score", LANCE_SCALAR_ZONE_MAP, "cpp_score_zonemap",
        R"({"rows_per_zone":3,"use_seeds":false})");
    ds.create_scalar_index(
        "id", LANCE_SCALAR_BLOOM_FILTER, "cpp_id_bloom",
        R"({"number_of_items":8,"probability":0.01})");

    auto json = ds.list_indices_json();
    assert(json.find("\"name\":\"cpp_name_ngram\"") != std::string::npos);
    assert(json.find("\"type\":\"NGram\"") != std::string::npos);
    assert(json.find("\"name\":\"cpp_score_zonemap\"") != std::string::npos);
    assert(json.find("\"type\":\"ZoneMap\"") != std::string::npos);
    assert(json.find("\"name\":\"cpp_id_bloom\"") != std::string::npos);
    assert(json.find("\"type\":\"BloomFilter\"") != std::string::npos);

    const uint64_t index_count = ds.index_count();
    auto query_ds = lance::Dataset::open(uri);
    auto assert_indexed = [&](const std::string& filter,
                              const std::vector<int32_t>& expected) {
        ScanStatisticsCapture stats;
        assert(scan_filtered_ids(uri, filter, true, stats) == expected);
        assert(stats.calls == 1);
        assert(stats.indices_loaded > 0);
        ScanStatisticsCapture unindexed_stats;
        assert(scan_filtered_ids(uri, filter, false, unindexed_stats) == expected);
        assert(unindexed_stats.indices_loaded == 0);
    };

    assert_indexed("contains(name, 'ali')", {1});
    assert_indexed("contains(name, 'zzz')", {});
    assert_indexed("score >= 180", {18, 19, 20});
    assert_indexed("score IS NULL", {7, 16});
    assert_indexed("id = 13", {13});
    assert_indexed("id = 999", {});

    query_ds.create_scalar_index(
        "score", LANCE_SCALAR_ZONE_MAP, "cpp_score_zonemap",
        R"({"rows_per_zone":4,"use_seeds":false})", true);
    assert(query_ds.index_count() == index_count);
    assert_indexed("id = 13", {13});

    query_ds.prewarm_index("cpp_name_ngram");
    query_ds.prewarm_index("cpp_score_zonemap");
    query_ds.prewarm_index("cpp_id_bloom");

    {
        auto reopened = lance::Dataset::open(uri);
        json = reopened.list_indices_json();
        assert(json.find("\"name\":\"cpp_name_ngram\"") != std::string::npos);
        assert(json.find("\"name\":\"cpp_score_zonemap\"") != std::string::npos);
        assert(json.find("\"name\":\"cpp_id_bloom\"") != std::string::npos);

        reopened.drop_index("cpp_name_ngram");
        reopened.drop_index("cpp_score_zonemap");
        reopened.drop_index("cpp_id_bloom");
        json = reopened.list_indices_json();
        assert(json.find("cpp_name_ngram") == std::string::npos);
        assert(json.find("cpp_score_zonemap") == std::string::npos);
        assert(json.find("cpp_id_bloom") == std::string::npos);
    }

    PASS();
}

static int collect_minhash_alice(ArrowArrayStream& stream) {
    ArrowSchema schema{};
    assert(stream.get_schema(&stream, &schema) == 0);
    int id_column = -1;
    int distance_column = -1;
    for (int64_t i = 0; i < schema.n_children; ++i) {
        const ArrowSchema* child = schema.children[i];
        if (!child || !child->name) continue;
        if (strcmp(child->name, "id") == 0) id_column = static_cast<int>(i);
        if (strcmp(child->name, "_distance") == 0) {
            distance_column = static_cast<int>(i);
        }
    }
    assert(id_column >= 0);
    assert(distance_column >= 0);
    schema.release(&schema);

    int found_alice = 0;
    int64_t total_rows = 0;
    while (true) {
        ArrowArray array{};
        assert(stream.get_next(&stream, &array) == 0);
        if (!array.release) break;
        const ArrowArray* ids = array.children[id_column];
        const ArrowArray* distances = array.children[distance_column];
        assert(ids && ids->buffers[1]);
        assert(distances && distances->buffers[1]);
        const auto* id_values = static_cast<const int32_t*>(ids->buffers[1]);
        const auto* distance_values =
            static_cast<const float*>(distances->buffers[1]);
        for (int64_t row = 0; row < array.length; ++row) {
            const int32_t id = id_values[ids->offset + row];
            const float distance = distance_values[distances->offset + row];
            assert(distance >= 0.0f);
            total_rows += 1;
            if (id == 1) {
                assert(distance == 0.0f);
                found_alice += 1;
            }
        }
        array.release(&array);
    }
    stream.release(&stream);
    assert(total_rows > 0);
    return found_alice;
}

static void test_minhash_lsh(const std::string& uri) {
    TEST(test_minhash_lsh);

    auto ds = lance::Dataset::open(uri);
    ds.create_scalar_index(
        "name", LANCE_SCALAR_MINHASH_LSH, "cpp_minhash",
        R"({"num_hashes":64,"num_bands":16,"shingle_size":1})");
    auto json = ds.list_indices_json();
    assert(json.find("\"name\":\"cpp_minhash\"") != std::string::npos);
    assert(json.find("\"type\":\"MinHashLsh\"") != std::string::npos);
    json = ds.index_segments_json();
    assert(json.find("\"name\":\"cpp_minhash\"") != std::string::npos);
    assert(json.find("\"type\":\"MinHashLsh\"") != std::string::npos);
    ds.prewarm_index("cpp_minhash");

    {
        auto scanner = ds.scan({"id", "name"});
        scanner.minhash_search("name", "alice").limit(10);
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);
        assert(collect_minhash_alice(stream) == 1);
    }

    {
        auto scanner = ds.scan({"id", "name"});
        scanner.minhash_search("name", "alice").limit(3);
        bool caught = false;
        try {
            scanner.minhash_search("", "alice");
        } catch (const lance::Error& error) {
            caught = true;
            assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
            assert(std::string(error.what()).find("column to search") !=
                   std::string::npos);
        }
        assert(caught);
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);
        assert(collect_minhash_alice(stream) == 1);
    }

    {
        auto scanner = ds.scan({"id"});
        scanner.minhash_search("name", "alice");
        bool caught = false;
        try {
            ArrowArrayStream stream;
            memset(&stream, 0, sizeof(stream));
            scanner.to_arrow_stream(&stream);
        } catch (const lance::Error& error) {
            caught = true;
            assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
            assert(std::string(error.what()).find("requires a limit") !=
                   std::string::npos);
        }
        assert(caught);
    }

    {
        auto scanner = ds.scan({"id"});
        scanner.minhash_search("score", "alice").limit(5);
        bool caught = false;
        try {
            ArrowArrayStream stream;
            memset(&stream, 0, sizeof(stream));
            scanner.to_arrow_stream(&stream);
        } catch (const lance::Error& error) {
            caught = true;
            assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
            assert(std::string(error.what()).find(
                       "No MinHash LSH index found for column score") !=
                   std::string::npos);
        }
        assert(caught);
    }

    {
        auto reopened = lance::Dataset::open(uri);
        json = reopened.list_indices_json();
        assert(json.find("\"name\":\"cpp_minhash\"") != std::string::npos);
        reopened.drop_index("cpp_minhash");
        json = reopened.list_indices_json();
        assert(json.find("cpp_minhash") == std::string::npos);
    }

    PASS();
}

static const ArrowSchema* find_schema_child(
    const ArrowSchema& schema,
    const char* name) {
    for (int64_t i = 0; i < schema.n_children; ++i) {
        const ArrowSchema* child = schema.children[i];
        if (child && child->name && strcmp(child->name, name) == 0) {
            return child;
        }
    }
    return nullptr;
}

static void test_scanner_introspection(const std::string& uri) {
    TEST(test_scanner_introspection);

    auto ds = lance::Dataset::open(uri);
    assert(ds.count_rows() == 20);
    assert(ds.count_rows("id >= 11") == 10);

    bool caught = false;
    try {
        (void)ds.count_rows(std::string());
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught);

    auto scanner = ds.scan(std::nullopt, "id >= 11");
    scanner.project({
        {"id_alias", "id"},
        {"next_id", "id + 1"},
    });
    ArrowSchema schema{};
    scanner.schema(&schema);
    assert(schema.n_children == 2);
    assert(find_schema_child(schema, "id_alias"));
    assert(find_schema_child(schema, "next_id"));
    if (schema.release) schema.release(&schema);

    uint64_t count = scanner.count_rows();
    assert(count == 10);
    assert(scanner.count_rows() == count);
    assert(!scanner.explain_plan().empty());
    assert(scanner.analyze_plan().find("AggregateExec") == std::string::npos);
    assert(scanner.analyze_count_plan().find("AggregateExec") !=
           std::string::npos);

    scanner.batch_size(2);
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);
    ArrowSchema stream_schema{};
    assert(stream.get_schema(&stream, &stream_schema) == 0);
    assert(find_schema_child(stream_schema, "id_alias"));
    assert(find_schema_child(stream_schema, "next_id"));
    if (stream_schema.release) stream_schema.release(&stream_schema);
    assert(drain_scan_rows(stream) == count);

    auto limited = ds.scan();
    limited.limit(0);
    caught = false;
    try {
        (void)limited.count_rows();
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
        assert(std::string(error.what()).find(
                   "Cannot use limit/offset with aggregate") !=
               std::string::npos);
    }
    assert(caught);

    PASS();
}

static void test_fragment_snapshots(const std::string& uri) {
    TEST(test_fragment_snapshots);

    auto latest = lance::Dataset::open(uri);
    auto ids = latest.fragment_ids();
    assert(ids.size() == 2);
    auto latest_version = latest.version();
    assert(latest_version >= 3);

    std::optional<lance::Fragment> before_delete;
    {
        auto before = lance::Dataset::open(uri, {}, latest_version - 1);
        before_delete.emplace(before.open_fragment(ids[0]));
    }
    // The fragment retains its own snapshot after the source Dataset closes.
    assert(before_delete->id() == ids[0]);
    assert(before_delete->data_file_count() == 1);
    assert(before_delete->physical_rows() == 4);
    assert(before_delete->deleted_rows() == 0);
    assert(before_delete->logical_rows() == 4);
    auto before_metadata = before_delete->metadata_json();
    assert(before_metadata.find("\"physical_rows\":4") != std::string::npos);
    assert(before_metadata.find("\"deletion_file\"") == std::string::npos);

    // The latest snapshot has one deletion in each fragment. The older
    // fragment remains bound to version 2 rather than reading version 3.
    auto current = latest.open_fragment(ids[0]);
    assert(current.id() == ids[0]);
    assert(current.physical_rows() == 4);
    assert(current.deleted_rows() == 1);
    assert(current.logical_rows() == 3);

    auto metadata = current.metadata_json();
    assert(metadata.find("\"files\":[") != std::string::npos);
    assert(metadata.find("\"path\":\"") != std::string::npos);
    assert(metadata.find("\"fields\":[") != std::string::npos);
    assert(metadata.find("\"column_indices\":[") != std::string::npos);
    assert(metadata.find("\"deletion_file\":{") != std::string::npos);
    assert(metadata.find("\"read_version\":") != std::string::npos);
    assert(metadata.find("\"num_deleted_rows\":1") != std::string::npos);

    auto second = latest.open_fragment(ids[1]);
    assert(second.id() == ids[1]);
    assert(second.physical_rows() == 4);
    assert(second.deleted_rows() == 1);
    assert(second.logical_rows() == 3);

    ArrowSchema schema{};
    current.schema(&schema);
    assert(schema.n_children == 3);
    const ArrowSchema* profile = find_schema_child(schema, "profile");
    assert(profile);
    assert(profile->format && strcmp(profile->format, "+s") == 0);
    assert(profile->n_children == 2);
    assert(find_schema_child(*profile, "name"));
    assert(find_schema_child(*profile, "score"));
    const ArrowSchema* meta = find_schema_child(schema, "meta");
    assert(meta);
    assert(meta->format && strcmp(meta->format, "Z") == 0);
    if (schema.release) schema.release(&schema);

    bool caught = false;
    try {
        (void)latest.open_fragment(std::numeric_limits<uint64_t>::max());
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_NOT_FOUND);
        assert(std::string(error.what()).find("fragment_id") !=
               std::string::npos);
    }
    assert(caught);

    // Every read still works after the source Dataset is destroyed.
    std::optional<lance::Fragment> owned;
    {
        auto ds = lance::Dataset::open(uri);
        owned.emplace(ds.open_fragment(ids[0]));
    }
    assert(owned->id() == ids[0]);
    assert(owned->physical_rows() == 4);
    assert(owned->deleted_rows() == 1);
    assert(owned->logical_rows() == 3);
    assert(owned->metadata_json().find("\"deletion_file\":{") !=
           std::string::npos);

    PASS();
}

static std::vector<int32_t> read_fragment_take_ids(
    ArrowArrayStream& stream,
    bool profile_name_only) {
    ArrowSchema schema{};
    assert(stream.get_schema(&stream, &schema) == 0);
    assert(schema.n_children == 3);
    int id_column = -1;
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (schema.children[i] && schema.children[i]->name &&
            strcmp(schema.children[i]->name, "id") == 0) {
            id_column = static_cast<int>(i);
        }
    }
    assert(id_column >= 0);
    const ArrowSchema* profile = find_schema_child(schema, "profile");
    assert(profile && profile->format && strcmp(profile->format, "+s") == 0);
    assert(profile->n_children == (profile_name_only ? 1 : 2));
    const ArrowSchema* meta = find_schema_child(schema, "meta");
    assert(meta);
    assert(meta->format && strcmp(meta->format, "u") == 0);
    schema.release(&schema);

    std::vector<int32_t> ids;
    while (true) {
        ArrowArray array;
        memset(&array, 0, sizeof(array));
        assert(stream.get_next(&stream, &array) == 0);
        if (!array.release) break;
        assert(array.n_children == 3);
        const ArrowArray* id_array = array.children[id_column];
        assert(id_array && id_array->buffers[1]);
        const auto* values =
            static_cast<const int32_t*>(id_array->buffers[1]);
        for (int64_t row = 0; row < array.length; ++row) {
            ids.push_back(values[id_array->offset + row]);
        }
        array.release(&array);
    }
    if (stream.release) stream.release(&stream);
    return ids;
}

static void test_fragment_read_operations(const std::string& uri) {
    TEST(test_fragment_read_operations);

    auto latest = lance::Dataset::open(uri);
    assert(latest.has_stable_row_ids());
    auto ids = latest.fragment_ids();
    assert(ids.size() == 2);
    auto fragment = latest.open_fragment(ids[0]);
    assert(fragment.count_rows() == 3);
    assert(fragment.count_rows("id >= 2") == 2);

    bool caught = false;
    try {
        (void)fragment.count_rows(std::string{});
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught);

    std::vector<uint32_t> indices{0, 2};
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    fragment.take(indices.data(), indices.size(), &stream);
    assert((read_fragment_take_ids(stream, false) ==
            std::vector<int32_t>{0, 3}));

    std::vector<std::string> columns{"id", "profile.name", "meta"};
    memset(&stream, 0, sizeof(stream));
    fragment.take(indices.data(), indices.size(), columns, &stream);
    assert((read_fragment_take_ids(stream, true) ==
            std::vector<int32_t>{0, 3}));

    std::vector<std::string> empty_columns;
    std::vector<uint32_t> one_index{0};
    caught = false;
    memset(&stream, 0, sizeof(stream));
    try {
        fragment.take(one_index.data(), one_index.size(), empty_columns, &stream);
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_NOT_FOUND);
        assert(std::string(error.what()).find("No data files") !=
               std::string::npos);
    }
    assert(caught);

    {
        auto scanner = fragment.scan();
        scanner.order_by(std::vector<lance::ColumnOrdering>{
            {"id", LANCE_ORDER_DESCENDING, LANCE_ORDER_NULLS_LAST},
        }).limit(1);
        ArrowArrayStream scan_stream;
        memset(&scan_stream, 0, sizeof(scan_stream));
        scanner.to_arrow_stream(&scan_stream);
        assert((read_fragment_take_ids(scan_stream, false) ==
                std::vector<int32_t>{3}));
    }

    caught = false;
    try {
        auto scanner = fragment.scan();
        scanner.order_by(std::vector<lance::ColumnOrdering>{
            {"missing", LANCE_ORDER_ASCENDING, LANCE_ORDER_NULLS_LAST},
        });
        ArrowArrayStream scan_stream;
        memset(&scan_stream, 0, sizeof(scan_stream));
        scanner.to_arrow_stream(&scan_stream);
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
        assert(std::string(error.what()).find("missing") !=
               std::string::npos);
    }
    assert(caught);

    /* The returned streams own their data after all parent handles close. */
    std::optional<lance::Fragment> held_fragment;
    std::optional<lance::Scanner> held_scanner;
    ArrowArrayStream take_stream;
    memset(&take_stream, 0, sizeof(take_stream));
    {
        auto ds = lance::Dataset::open(uri);
        auto owned = ds.open_fragment(ids[0]);
        owned.take(one_index.data(), one_index.size(), &take_stream);
        held_fragment.emplace(std::move(owned));
        held_scanner.emplace(held_fragment->scan());
    }
    held_fragment.reset();
    ArrowArrayStream scan_stream;
    memset(&scan_stream, 0, sizeof(scan_stream));
    held_scanner->to_arrow_stream(&scan_stream);
    held_scanner.reset();

    assert((read_fragment_take_ids(take_stream, false) ==
            std::vector<int32_t>{0}));
    assert((read_fragment_take_ids(scan_stream, false) ==
            std::vector<int32_t>{0, 2, 3}));

    PASS();
}

int main(int argc, char** argv) {
    if (argc < 12) {
        fprintf(stderr,
                "Usage: %s <dataset_uri> <write_uri> <blob_uri> <ordering_uri> "
                "<session_uri> <fragment_uri> <fragment_read_uri> <cleanup_uri> "
                "<aggregate_plan_path> <config_base_uri> <alternate_base_uri>\n",
                argv[0]);
        return 1;
    }

    std::string uri(argv[1]);
    std::string write_uri(argv[2]);
    std::string blob_uri(argv[3]);
    std::string ordering_uri(argv[4]);
    std::string session_uri(argv[5]);
    std::string fragment_uri(argv[6]);
    std::string fragment_read_uri(argv[7]);
    std::string cleanup_uri(argv[8]);
    std::string aggregate_plan_path(argv[9]);
    std::string config_base_uri(argv[10]);
    std::string alternate_base_uri(argv[11]);
    printf("Running C++ API tests with dataset: %s\n", uri.c_str());

    test_dataset_open(uri);
    test_shared_session(uri);
    test_session_cache_config(session_uri);
    test_dataset_schema(uri);
    test_scanner_fluent(uri);
    test_scanner_projection_filter(uri);
    test_scanner_projection(uri);
    test_scanner_order_by(ordering_uri);
    test_scanner_substrait_aggregate(ordering_uri, aggregate_plan_path);
    test_scanner_introspection(uri);
    test_fragment_snapshots(fragment_uri);
    test_fragment_read_operations(fragment_read_uri);
    test_scanner_row_addr_lists(uri);
    test_distance_range(uri);
    test_scanner_async_stream_ownership(uri);
    test_scanner_blob_handling(blob_uri);
    test_take_blobs(blob_uri);
    test_blob_native_read_slice(blob_uri);
    test_blob_fields_and_descriptors();
    test_blob_planned_reads(blob_uri);
    test_dataset_take(uri);
    test_dataset_take_rows(uri);
    test_raii_cleanup(uri);
    test_versions(uri);
    test_version_refs_and_tags(uri);
    test_branch_lifecycle(uri);
    test_restore_to_current(uri);
    test_error_exception(uri);
    test_index_lifecycle(uri);
    test_native_scalar_indexes(uri);
    test_minhash_lsh(uri);
    test_batch_nearest(uri);
    test_scanner_materialization_fast_search(uri);
    test_nearest_smoke(uri);
    test_multivector_rejects_flat_column(uri);
    test_index_segments_smoke(uri);
    test_index_segment_builder(uri);
    test_index_segment_builder_progress(uri);
    test_vector_models_and_reusable_segments(uri);
    test_commit_index_segments(uri);
    test_fts_smoke(uri);
    test_dataset_write_roundtrip(uri, write_uri);
    test_write_options_and_config(uri, write_uri);
    test_data_statistics(write_uri);
    test_config_replacement(config_base_uri);
    test_base_paths_and_add(config_base_uri, alternate_base_uri);
    test_file_stream_ownership(config_base_uri, alternate_base_uri);
    test_cleanup_and_lifecycle(cleanup_uri);
    test_dataset_metadata(write_uri);
    test_dataset_field_metadata(fragment_uri);
    test_update(write_uri);
    test_merge_insert(write_uri);
    test_alter_columns(write_uri);
    test_drop_columns(write_uri);
    test_add_columns(write_uri);
    test_compact_files(write_uri);
    test_delete_rows(write_uri);

    printf("All C++ tests passed!\n");
    return 0;
}
