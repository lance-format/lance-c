/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#define _POSIX_C_SOURCE 200809L

/**
 * @file test_c_api.c
 * @brief C compilation and functional test for lance.h
 *
 * This file is compiled by the Rust integration test to verify that
 * lance.h is valid C and the API works end-to-end.
 *
 * Usage: test_c_api <dataset_uri> <write_uri> <blob_uri> <ordering_uri>
 *                    <session_uri> <fragment_uri> <fragment_read_uri>
 *                    <cleanup_uri> <aggregate_plan_path>
 *                    <config_base_uri> <alternate_base_uri>
 */

#include "lance/lance.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Arrow C Data Interface flag bits — mirror arrow_schema/arrow_c_data_interface.h
 * so we don't depend on the full Arrow header just to read schema flags. */
#define ARROW_FLAG_NULLABLE 2

#define ASSERT(cond, msg)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);            \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define CHECK_OK()                                                             \
    do {                                                                       \
        if (lance_last_error_code() != LANCE_OK) {                             \
            const char *msg = lance_last_error_message();                      \
            fprintf(stderr, "FAIL: lance error: %s (line %d)\n",              \
                    msg ? msg : "unknown", __LINE__);                          \
            if (msg) lance_free_string(msg);                                   \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static void discard_last_error(void) {
    const char *message = lance_last_error_message();
    if (message) lance_free_string(message);
}

static void expect_error(LanceErrorCode expected, const char *operation);
static void expect_error_contains(
    LanceErrorCode expected,
    const char *needle,
    const char *operation);

static const char *arrow_field_metadata_value(
    const struct ArrowSchema *field,
    const char *key,
    size_t *value_len) {
    if (field == NULL || field->metadata == NULL) return NULL;
    const char *cursor = field->metadata;
    int32_t pair_count = 0;
    memcpy(&pair_count, cursor, sizeof(pair_count));
    cursor += sizeof(pair_count);
    for (int32_t i = 0; i < pair_count; ++i) {
        int32_t key_len = 0;
        memcpy(&key_len, cursor, sizeof(key_len));
        cursor += sizeof(key_len);
        const char *key_data = cursor;
        cursor += key_len;
        int32_t stored_value_len = 0;
        memcpy(&stored_value_len, cursor, sizeof(stored_value_len));
        cursor += sizeof(stored_value_len);
        const char *value_data = cursor;
        cursor += stored_value_len;
        if (key_len == (int32_t)strlen(key) &&
            memcmp(key_data, key, (size_t)key_len) == 0) {
            if (value_len != NULL) *value_len = (size_t)stored_value_len;
            return value_data;
        }
    }
    return NULL;
}

static const struct ArrowSchema *arrow_field_at_path(
    const struct ArrowSchema *schema,
    const char *path) {
    const struct ArrowSchema *field = schema;
    const char *segment = path;
    while (*segment != '\0') {
        const char *dot = strchr(segment, '.');
        size_t segment_len = dot == NULL ? strlen(segment) : (size_t)(dot - segment);
        const struct ArrowSchema *child = NULL;
        for (int64_t i = 0; i < field->n_children; ++i) {
            const struct ArrowSchema *candidate = field->children[i];
            if (candidate != NULL && candidate->name != NULL &&
                strlen(candidate->name) == segment_len &&
                memcmp(candidate->name, segment, segment_len) == 0) {
                child = candidate;
                break;
            }
        }
        if (child == NULL) return NULL;
        field = child;
        if (dot == NULL) break;
        segment = dot + 1;
    }
    return field;
}

static void assert_field_metadata(
    const struct ArrowSchema *field,
    const char *key,
    const char *expected) {
    size_t value_len = 0;
    const char *value = arrow_field_metadata_value(field, key, &value_len);
    ASSERT(value != NULL, "expected field metadata key");
    ASSERT(value_len == strlen(expected), "field metadata value length mismatch");
    ASSERT(memcmp(value, expected, value_len) == 0, "field metadata value mismatch");
}

static void assert_field_metadata_absent(
    const struct ArrowSchema *field,
    const char *key) {
    if (arrow_field_metadata_value(field, key, NULL) != NULL) {
        fprintf(stderr,
                "unexpected field metadata: field=%s key=%s\n",
                field != NULL && field->name != NULL ? field->name : "<null>",
                key);
    }
    ASSERT(arrow_field_metadata_value(field, key, NULL) == NULL,
           "field metadata key should be absent");
}

typedef struct {
    uint64_t calls;
    uint64_t bytes_read;
    uint64_t indices_loaded;
    int invalid;
} ScanStatisticsCapture;

static void capture_scan_statistics(
    void *callback_ctx,
    const LanceScanStatistics *statistics
) {
    if (callback_ctx == NULL) return;
    ScanStatisticsCapture *captured = (ScanStatisticsCapture *)callback_ctx;
    if (statistics == NULL ||
        (statistics->metrics_len > 0 && statistics->metrics == NULL)) {
        captured->invalid = 1;
        return;
    }
    for (size_t i = 0; i < statistics->metrics_len; ++i) {
        const LanceScanMetric *metric = &statistics->metrics[i];
        if ((metric->name_len > 0 && metric->name == NULL) ||
            (metric->kind != LANCE_SCAN_METRIC_COUNT &&
             metric->kind != LANCE_SCAN_METRIC_TIME_NANOSECONDS)) {
            captured->invalid = 1;
            return;
        }
    }
    captured->calls += 1;
    captured->bytes_read = statistics->bytes_read;
    captured->indices_loaded = statistics->indices_loaded;
}

typedef struct {
    uint64_t events;
    uint64_t starts;
    uint64_t completes;
    int saw_shuffle_start;
    int saw_shuffle_complete;
    int invalid;
    void *expected_ctx;
    int ctx_mismatch;
} BuildProgressCapture;

static void capture_build_progress(
    void *callback_ctx,
    int32_t event,
    const char *stage,
    uint64_t total,
    const char *unit,
    uint64_t completed) {
    (void)total;
    (void)completed;
    if (callback_ctx == NULL) return;
    BuildProgressCapture *captured = (BuildProgressCapture *)callback_ctx;
    if (callback_ctx != captured->expected_ctx) {
        captured->ctx_mismatch = 1;
    }
    if (stage == NULL || unit == NULL) {
        captured->invalid = 1;
        return;
    }
    /* Exercise strcmp on the borrowed stage string. */
    if (strcmp(stage, "shuffle") == 0) {
        if (event == LANCE_INDEX_BUILD_PROGRESS_STAGE_START)
            captured->saw_shuffle_start = 1;
        if (event == LANCE_INDEX_BUILD_PROGRESS_STAGE_COMPLETE)
            captured->saw_shuffle_complete = 1;
    }
    if (event == LANCE_INDEX_BUILD_PROGRESS_STAGE_START)
        captured->starts += 1;
    else if (event == LANCE_INDEX_BUILD_PROGRESS_STAGE_COMPLETE)
        captured->completes += 1;
    else if (event != LANCE_INDEX_BUILD_PROGRESS_STAGE_PROGRESS)
        captured->invalid = 1;
    captured->events += 1;
}

static void test_open_and_metadata(const char *uri) {
    printf("  test_open_and_metadata... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "dataset open failed");
    CHECK_OK();

    uint64_t version = lance_dataset_version(ds);
    ASSERT(version >= 1, "version should be >= 1");

    uint64_t count = lance_dataset_count_rows(ds);
    CHECK_OK();
    ASSERT(count > 0, "dataset should have rows");
    printf("version=%llu, rows=%llu... ", (unsigned long long)version,
           (unsigned long long)count);

    /* Schema export */
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    int32_t rc = lance_dataset_schema(ds, &schema);
    ASSERT(rc == 0, "schema export failed");
    ASSERT(schema.n_children > 0, "schema should have fields");
    printf("fields=%lld... ", (long long)schema.n_children);

    /* Release the schema */
    if (schema.release) {
        schema.release(&schema);
    }

    lance_dataset_close(ds);
    printf("OK\n");
}

static void test_shared_session(const char *uri) {
    printf("  test_shared_session... ");

    LanceSession *session = lance_session_new(0, 16 * 1024 * 1024);
    ASSERT(session != NULL, "session creation failed");

    LanceDataset *ds = lance_dataset_open_with_session(uri, NULL, 0, session);
    ASSERT(ds != NULL, "shared-session dataset open failed");

    LanceSessionCacheStats stats;
    memset(&stats, 0, sizeof(stats));
    int32_t rc = lance_session_get_cache_stats(session, &stats);
    ASSERT(rc == 0, "session cache stats failed");

    /* The dataset retains the shared state after the caller drops its handle. */
    lance_session_close(session);
    ASSERT(lance_dataset_count_rows(ds) > 0,
           "dataset should remain valid after session close");

    lance_dataset_close(ds);
    printf("metadata_entries=%llu... OK\n",
           (unsigned long long)stats.metadata_cache_entries);
}

static uint64_t scan_filtered_with_index(
    const LanceDataset *dataset,
    const char *filter) {
    LanceScanner *scanner = lance_scanner_new(dataset, NULL, filter);
    ASSERT(scanner != NULL, "filtered scanner creation failed");
    ASSERT(lance_scanner_set_use_scalar_index(scanner, true) == 0,
           "enable scalar index failed");

    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "filtered scanner stream creation failed");

    uint64_t total = 0;
    while (1) {
        struct ArrowArray array;
        memset(&array, 0, sizeof(array));
        ASSERT(stream.get_next(&stream, &array) == 0,
               "filtered stream get_next failed");
        if (array.release == NULL) break;
        total += (uint64_t)array.length;
        array.release(&array);
    }
    if (stream.release) stream.release(&stream);
    lance_scanner_close(scanner);
    return total;
}

static void test_session_cache_config(const char *uri) {
    printf("  test_session_cache_config... ");

    LanceSessionCacheSpec zero = {0};
    zero.kind = LANCE_SESSION_CACHE_SPEC_SIZE;
    zero.size_bytes = 0;

    LanceSession *session = lance_session_new_with_cache_specs(&zero, &zero);
    ASSERT(session != NULL, "zero-capacity session creation failed");

    uint64_t initial_size = 0;
    uint64_t approx_items = 1;
    ASSERT(lance_session_get_size_bytes(session, &initial_size) == 0,
           "initial session size failed");
    ASSERT(lance_session_get_approx_num_items(session, &approx_items) == 0,
           "initial approximate item count failed");
    ASSERT(approx_items == 0, "new zero-capacity session should be empty");
    ASSERT(initial_size > 0, "deep size should include session state");

    LanceDataset *ds = lance_dataset_open_with_session(uri, NULL, 0, session);
    ASSERT(ds != NULL, "zero-capacity shared-session open failed");
    ASSERT(lance_dataset_fragment_count(ds) >= 2,
           "session fixture must have multiple fragments");
    ASSERT(lance_dataset_prewarm_index(ds, "id_idx") == 0,
           "zero-capacity index prewarm failed");
    ASSERT(scan_filtered_with_index(ds, "id >= 5 AND id < 8") == 3,
           "zero-capacity indexed read returned wrong rows");

    LanceSessionCacheStats stats;
    memset(&stats, 0, sizeof(stats));
    ASSERT(lance_session_get_cache_stats(session, &stats) == 0,
           "zero-capacity cache stats failed");
    ASSERT(stats.index_cache_entries == 0 &&
           stats.index_cache_size_bytes == 0 &&
           stats.metadata_cache_entries == 0 &&
           stats.metadata_cache_size_bytes == 0,
           "zero capacity must retain no cache entries");
    ASSERT(lance_session_get_approx_num_items(session, &approx_items) == 0,
           "zero-capacity approximate item count failed");
    ASSERT(approx_items == 0, "zero capacity should report no items");
    uint64_t final_size = 0;
    ASSERT(lance_session_get_size_bytes(session, &final_size) == 0,
           "final session size failed");
    ASSERT(final_size == initial_size,
           "zero-capacity reads must not grow session cache state");

    lance_session_close(session);
    ASSERT(scan_filtered_with_index(ds, "id >= 5 AND id < 8") == 3,
           "dataset must keep working after session handle close");
    lance_dataset_close(ds);

    char index_uri[] = "moka://?capacity=67108864";
    char backend_kind[] = "MOKA";
    char option_key[] = "capacity";
    char option_value[] = "33554432";
    const char *option_keys[] = {option_key};
    const char *option_values[] = {option_value};

    LanceSessionCacheSpec index_spec = {0};
    index_spec.kind = LANCE_SESSION_CACHE_SPEC_URI;
    index_spec.backend_uri = index_uri;
    LanceSessionCacheSpec metadata_spec = {0};
    metadata_spec.kind = LANCE_SESSION_CACHE_SPEC_CONFIG;
    metadata_spec.backend_kind = backend_kind;
    metadata_spec.option_keys = option_keys;
    metadata_spec.option_values = option_values;
    metadata_spec.option_count = 1;

    session = lance_session_new_with_cache_specs(&index_spec, &metadata_spec);
    ASSERT(session != NULL, "Moka URI/config session creation failed");
    ASSERT(lance_session_get_size_bytes(session, &initial_size) == 0,
           "configured initial session size failed");

    memset(index_uri, 'x', sizeof(index_uri) - 1);
    memset(backend_kind, 'x', sizeof(backend_kind) - 1);
    memset(option_key, 'x', sizeof(option_key) - 1);
    memset(option_value, 'x', sizeof(option_value) - 1);

    ds = lance_dataset_open_with_session(uri, NULL, 0, session);
    ASSERT(ds != NULL, "Moka-configured shared-session open failed");
    ASSERT(lance_dataset_prewarm_index(ds, "id_idx") == 0,
           "Moka-configured index prewarm failed");
    ASSERT(scan_filtered_with_index(ds, "id >= 5 AND id < 8") == 3,
           "Moka-configured indexed read returned wrong rows");

    memset(&stats, 0, sizeof(stats));
    ASSERT(lance_session_get_cache_stats(session, &stats) == 0,
           "Moka-configured cache stats failed");
    ASSERT(stats.index_cache_entries > 0,
           "Moka URI index cache should retain entries");
    ASSERT(stats.metadata_cache_entries > 0,
           "Moka config metadata cache should retain entries");
    ASSERT(lance_session_get_approx_num_items(session, &approx_items) == 0,
           "Moka-configured approximate item count failed");
    ASSERT(approx_items > 0, "Moka-configured session should report items");
    ASSERT(lance_session_get_size_bytes(session, &final_size) == 0,
           "Moka-configured final session size failed");
    ASSERT(final_size > initial_size,
           "Moka-configured reads should be reflected in deep size");

    lance_session_close(session);
    ASSERT(scan_filtered_with_index(ds, "id >= 9") == 12,
           "dataset must keep configured cache state after session close");
    lance_dataset_close(ds);

    LanceSessionCacheSpec invalid = {0};
    invalid.kind = LANCE_SESSION_CACHE_SPEC_CONFIG;
    invalid.backend_kind = "not a kind";
    session = lance_session_new_with_cache_specs(&invalid, &zero);
    ASSERT(session == NULL, "invalid backend kind must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "invalid backend kind error code");
    const char *message = lance_last_error_message();
    ASSERT(message != NULL && strstr(message, "invalid character") != NULL,
           "invalid backend kind should preserve the native error");
    if (message) lance_free_string(message);

    printf("index_entries=%llu, metadata_entries=%llu... OK\n",
           (unsigned long long)stats.index_cache_entries,
           (unsigned long long)stats.metadata_cache_entries);
}

static void test_scan(const char *uri) {
    printf("  test_scan... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "dataset open failed");

    uint64_t expected_rows = lance_dataset_count_rows(ds);
    CHECK_OK();

    /* Full scan via ArrowArrayStream */
    LanceScanner *scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "scanner creation failed");
    ScanStatisticsCapture captured = {0};
    int32_t rc = lance_scanner_set_statistics_callback(
        scanner, capture_scan_statistics, &captured);
    ASSERT(rc == 0, "statistics callback registration failed");

    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    rc = lance_scanner_to_arrow_stream(scanner, &stream);
    ASSERT(rc == 0, "to_arrow_stream failed");

    /* Read schema from stream */
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    rc = stream.get_schema(&stream, &schema);
    ASSERT(rc == 0, "get_schema from stream failed");
    ASSERT(schema.n_children > 0, "stream schema should have fields");
    if (schema.release) schema.release(&schema);

    /* Read all batches */
    uint64_t total_rows = 0;
    while (1) {
        struct ArrowArray array;
        memset(&array, 0, sizeof(array));
        rc = stream.get_next(&stream, &array);
        ASSERT(rc == 0, "get_next failed");
        if (array.release == NULL) {
            break; /* end of stream */
        }
        total_rows += (uint64_t)array.length;
        array.release(&array);
    }

    ASSERT(total_rows == expected_rows, "row count mismatch");
    ASSERT(captured.calls == 1, "statistics callback count mismatch");
    ASSERT(captured.bytes_read > 0, "statistics should report bytes read");
    ASSERT(captured.invalid == 0, "statistics callback received invalid data");
    printf("rows=%llu... ", (unsigned long long)total_rows);

    if (stream.release) stream.release(&stream);
    lance_scanner_close(scanner);
    lance_dataset_close(ds);
    printf("OK\n");
}

static void test_scan_with_limit(const char *uri) {
    printf("  test_scan_with_limit... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "dataset open failed");

    LanceScanner *scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "scanner creation failed");

    lance_scanner_set_limit(scanner, 3);

    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    int32_t rc = lance_scanner_to_arrow_stream(scanner, &stream);
    ASSERT(rc == 0, "to_arrow_stream failed");

    uint64_t total_rows = 0;
    while (1) {
        struct ArrowArray array;
        memset(&array, 0, sizeof(array));
        rc = stream.get_next(&stream, &array);
        ASSERT(rc == 0, "get_next failed");
        if (array.release == NULL) break;
        total_rows += (uint64_t)array.length;
        array.release(&array);
    }

    ASSERT(total_rows == 3, "limit should return exactly 3 rows");
    printf("rows=%llu... ", (unsigned long long)total_rows);

    if (stream.release) stream.release(&stream);
    lance_scanner_close(scanner);
    lance_dataset_close(ds);
    printf("OK\n");
}

static uint64_t count_stream_rows(struct ArrowArrayStream *stream);

static void test_scanner_introspection(const char *uri) {
    printf("  test_scanner_introspection... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "dataset open failed");
    CHECK_OK();
    uint64_t all_rows = lance_dataset_count_rows(ds);
    CHECK_OK();
    ASSERT(all_rows == 20, "fixture row count");
    ASSERT(lance_dataset_count_rows_filtered(ds, NULL) == all_rows,
           "NULL filter must use the native None fast path");
    CHECK_OK();

    const char *filter = "id >= 11";
    ASSERT(lance_dataset_count_rows_filtered(ds, filter) == 10,
           "filtered dataset count");
    CHECK_OK();
    const char *empty_filter = "";
    ASSERT(lance_dataset_count_rows_filtered(ds, empty_filter) == 0,
           "empty filter must be an explicit Some input");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "empty dataset filter must fail");
    const char *error = lance_last_error_message();
    ASSERT(error != NULL && error[0] != '\0', "empty filter error message");
    if (error) lance_free_string(error);

    LanceScanner *scanner = lance_scanner_new(ds, NULL, filter);
    ASSERT(scanner != NULL, "scanner creation failed");
    CHECK_OK();
    LanceProjectionColumn projection[] = {
        {"id_alias", "id"},
        {"next_id", "id + 1"},
    };
    ASSERT(lance_scanner_set_projection(scanner, projection, 2) == 0,
           "scanner expression projection");
    CHECK_OK();

    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(lance_scanner_schema(scanner, &schema) == 0,
           "scanner schema planning");
    CHECK_OK();
    ASSERT(schema.n_children == 2, "scanner schema field count");
    int saw_id = 0;
    int saw_next = 0;
    for (int64_t i = 0; i < schema.n_children; ++i) {
        const char *name = schema.children[i]->name;
        if (name && strcmp(name, "id_alias") == 0) saw_id = 1;
        if (name && strcmp(name, "next_id") == 0) saw_next = 1;
    }
    ASSERT(saw_id && saw_next, "scanner schema projected fields");
    if (schema.release) schema.release(&schema);

    uint64_t count = lance_scanner_count_rows(scanner);
    CHECK_OK();
    ASSERT(count == 10, "scanner native count");
    ASSERT(lance_scanner_count_rows(scanner) == count,
           "scanner count must be repeatable");
    CHECK_OK();

    const char *explain = lance_scanner_explain_plan(scanner, false);
    ASSERT(explain != NULL && explain[0] != '\0', "scanner explain");
    lance_free_string(explain);
    CHECK_OK();
    const char *analyze = lance_scanner_analyze_plan(scanner);
    ASSERT(analyze != NULL && strstr(analyze, "AggregateExec") == NULL,
           "normal analyze must not use the count plan");
    lance_free_string(analyze);
    CHECK_OK();
    const char *analyze_count = lance_scanner_analyze_count_plan(scanner);
    ASSERT(analyze_count != NULL &&
               strstr(analyze_count, "AggregateExec") != NULL,
           "analyze count plan must contain the native aggregate");
    lance_free_string(analyze_count);
    CHECK_OK();

    ASSERT(lance_scanner_set_batch_size(scanner, 2) == 0,
           "planning must leave scanner configuration editable");
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "scanner stream after inspection");
    CHECK_OK();
    ASSERT(count_stream_rows(&stream) == count,
           "scanner count must match the configured stream");
    lance_scanner_close(scanner);

    LanceScanner *limited = lance_scanner_new(ds, NULL, NULL);
    ASSERT(limited != NULL, "limited scanner");
    ASSERT(lance_scanner_set_limit(limited, 0) == 0, "zero limit setter");
    ASSERT(lance_scanner_count_rows(limited) == 0,
           "native aggregate must reject a zero limit");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "aggregate limit error code");
    error = lance_last_error_message();
    ASSERT(error != NULL &&
               strstr(error, "Cannot use limit/offset with aggregate") != NULL,
           "aggregate limit error message");
    if (error) lance_free_string(error);
    lance_scanner_close(limited);

    memset(&schema, 0, sizeof(schema));
    ASSERT(lance_scanner_schema(NULL, &schema) == -1,
           "NULL scanner schema must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL scanner schema error code");
    error = lance_last_error_message();
    if (error) lance_free_string(error);
    ASSERT(lance_scanner_count_rows(NULL) == 0,
           "NULL scanner count must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL scanner count error code");
    error = lance_last_error_message();
    if (error) lance_free_string(error);

    lance_dataset_close(ds);
    printf("OK\n");
}

static uint64_t count_stream_rows(struct ArrowArrayStream *stream) {
    uint64_t total_rows = 0;
    while (1) {
        struct ArrowArray array;
        memset(&array, 0, sizeof(array));
        ASSERT(stream->get_next(stream, &array) == 0, "get_next failed");
        if (array.release == NULL) break;
        total_rows += (uint64_t)array.length;
        array.release(&array);
    }
    if (stream->release) stream->release(stream);
    return total_rows;
}

static int json_contains(const char *json, const char *needle) {
    return json != NULL && strstr(json, needle) != NULL;
}

static int config_has_value(
    const LanceDataset *dataset,
    const char *key,
    const char *value) {
    LanceDatasetConfig *config = lance_dataset_config(dataset);
    ASSERT(config != NULL, "config snapshot failed");
    uint64_t count = lance_dataset_config_count(config);
    CHECK_OK();
    int found = 0;
    for (uint64_t i = 0; i < count; ++i) {
        const char *candidate_key =
            lance_dataset_config_key_at(config, (size_t)i);
        const char *candidate_value =
            lance_dataset_config_value_at(config, (size_t)i);
        CHECK_OK();
        if (strcmp(candidate_key, key) == 0) {
            found = strcmp(candidate_value, value) == 0;
            break;
        }
    }
    lance_dataset_config_close(config);
    return found;
}

static int config_has_key(const LanceDataset *dataset, const char *key) {
    LanceDatasetConfig *config = lance_dataset_config(dataset);
    ASSERT(config != NULL, "config snapshot failed");
    uint64_t count = lance_dataset_config_count(config);
    CHECK_OK();
    int found = 0;
    for (uint64_t i = 0; i < count; ++i) {
        const char *candidate_key =
            lance_dataset_config_key_at(config, (size_t)i);
        CHECK_OK();
        if (strcmp(candidate_key, key) == 0) {
            found = 1;
            break;
        }
    }
    lance_dataset_config_close(config);
    return found;
}

static int string_array_equals(
    const struct ArrowArray *array,
    int64_t row,
    const char *expected) {
    const int32_t *offsets = (const int32_t *)array->buffers[1];
    int32_t begin = offsets[row];
    int32_t end = offsets[row + 1];
    size_t length = (size_t)(end - begin);
    return length == strlen(expected) &&
           memcmp((const char *)array->buffers[2] + begin, expected, length) == 0;
}

static int dictionary_string_equals(
    const struct ArrowArray *array,
    int64_t row,
    int key_width,
    const char *expected) {
    ASSERT(array->dictionary != NULL, "dictionary values are required");
    int64_t value_index = 0;
    if (key_width == 4) {
        int32_t key = 0;
        memcpy(&key, (const char *)array->buffers[1] + row * 4, 4);
        value_index = key;
    } else {
        int8_t key = 0;
        memcpy(&key, (const char *)array->buffers[1] + row, 1);
        value_index = key;
    }
    ASSERT(value_index >= 0, "dictionary key must be non-negative");
    return string_array_equals(array->dictionary, value_index, expected);
}

static void wait_released(atomic_int *released) {
    for (int attempt = 0; attempt < 10000; ++attempt) {
        if (atomic_load_explicit(released, memory_order_seq_cst) > 0) {
            return;
        }
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    ASSERT(0, "progress context release timed out");
}

typedef struct {
    atomic_int calls;
    atomic_int released;
    atomic_ullong last_processed;
    atomic_ullong last_total;
    atomic_int has_total;
} TrackedProgressCapture;

typedef struct {
    TrackedProgressCapture *capture;
} TrackedProgressContext;

static void capture_tracked_progress(
    void *callback_ctx,
    uint64_t manifests_processed,
    const uint64_t *manifests_total) {
    ASSERT(callback_ctx != NULL, "progress context must be non-NULL");
    TrackedProgressContext *context =
        (TrackedProgressContext *)callback_ctx;
    atomic_fetch_add_explicit(
        &context->capture->calls, 1, memory_order_seq_cst);
    atomic_store_explicit(
        &context->capture->last_processed,
        manifests_processed,
        memory_order_seq_cst);
    if (manifests_total != NULL) {
        atomic_store_explicit(
            &context->capture->has_total, 1, memory_order_seq_cst);
        atomic_store_explicit(
            &context->capture->last_total,
            *manifests_total,
            memory_order_seq_cst);
    }
}

static void release_tracked_progress(void *callback_ctx) {
    ASSERT(callback_ctx != NULL, "progress release context must be non-NULL");
    TrackedProgressContext *context =
        (TrackedProgressContext *)callback_ctx;
    atomic_fetch_add_explicit(
        &context->capture->released, 1, memory_order_seq_cst);
    free(context);
}

static TrackedProgressContext *new_tracked_progress(
    TrackedProgressCapture *capture) {
    memset(capture, 0, sizeof(*capture));
    TrackedProgressContext *context =
        (TrackedProgressContext *)malloc(sizeof(*context));
    ASSERT(context != NULL, "allocate progress context");
    context->capture = capture;
    return context;
}

static void test_scanner_projection(const char *uri) {
    printf("  test_scanner_projection... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "dataset open failed");
    LanceScanner *scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "scanner creation failed");

    LanceProjectionColumn projection[] = {
        {"id_alias", "id"},
        {"next_id", "id + 1"},
    };
    ASSERT(lance_scanner_set_projection(scanner, projection, 2) == 0,
           "set expression projection");
    ASSERT(lance_scanner_disable_scoring_autoprojection(scanner) == 0,
           "disable scoring autoprojection");

    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "projection stream failed");
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(stream.get_schema(&stream, &schema) == 0, "projection schema failed");
    ASSERT(schema.n_children == 2, "projection should have two fields");
    ASSERT(strcmp(schema.children[0]->name, "id_alias") == 0,
           "first projection field");
    ASSERT(strcmp(schema.children[1]->name, "next_id") == 0,
           "second projection field");
    if (schema.release) schema.release(&schema);
    ASSERT(count_stream_rows(&stream) > 0, "projection should return rows");
    ASSERT(lance_scanner_set_projection(scanner, NULL, 0) == -1,
           "projection after scan start must fail");
    ASSERT(lance_scanner_disable_scoring_autoprojection(scanner) == -1,
           "scoring setting after scan start must fail");
    lance_scanner_close(scanner);

    scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "scanner creation failed");
    LanceProjectionColumn explicit_empty = {NULL, NULL};
    ASSERT(lance_scanner_set_projection(scanner, &explicit_empty, 0) == 0,
           "explicit empty projection");
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "empty projection stream failed");
    memset(&schema, 0, sizeof(schema));
    ASSERT(stream.get_schema(&stream, &schema) == 0, "empty projection schema failed");
    ASSERT(schema.n_children == 0, "explicit empty projection should have no fields");
    if (schema.release) schema.release(&schema);
    if (stream.release) stream.release(&stream);
    lance_scanner_close(scanner);

    lance_dataset_close(ds);
    printf("OK\n");
}

static void test_scan_with_row_addr_lists(const char *uri) {
    printf("  test_scan_with_row_addr_lists... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "dataset open failed");
    uint64_t expected_rows = lance_dataset_count_rows(ds);
    CHECK_OK();

    /* Rows 0, 2 and 4 of the first fragment, minus row 2. */
    const uint64_t allowed[] = {4, 0, 2};
    const uint64_t blocked[] = {2};
    LanceScanner *scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "scanner creation failed");
    ASSERT(lance_scanner_set_row_addr_allowlist(scanner, allowed, 3) == 0, "set allowlist");
    ASSERT(lance_scanner_set_row_addr_blocklist(scanner, blocked, 1) == 0, "set blocklist");
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0, "to_arrow_stream failed");
    ASSERT(count_stream_rows(&stream) == 2, "allowlist minus blocklist should keep 2 rows");
    ASSERT(lance_scanner_set_row_addr_allowlist(scanner, allowed, 3) == -1,
           "allowlist after the scan started must fail");
    lance_scanner_close(scanner);

    /* An empty blocklist keeps every row; NULL is valid when len is 0. */
    scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "scanner creation failed");
    ASSERT(lance_scanner_set_row_addr_blocklist(scanner, NULL, 0) == 0, "set empty blocklist");
    ASSERT(lance_scanner_set_row_addr_allowlist(scanner, NULL, 1) == -1,
           "NULL row_ids with len > 0 must fail");
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0, "to_arrow_stream failed");
    ASSERT(count_stream_rows(&stream) == expected_rows, "empty blocklist should keep every row");
    lance_scanner_close(scanner);

    lance_dataset_close(ds);
    printf("OK\n");
}

/* Copy the Arrow C Data Interface format of the `blob` column of a stream's
 * schema into `out`; `out` is empty when the column is missing. */
static void blob_column_format(struct ArrowArrayStream *stream, char *out, size_t out_len) {
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    int rc = stream->get_schema(stream, &schema);
    ASSERT(rc == 0, "get_schema from stream failed");
    out[0] = '\0';
    for (int64_t i = 0; i < schema.n_children; i++) {
        if (strcmp(schema.children[i]->name, "blob") == 0) {
            snprintf(out, out_len, "%s", schema.children[i]->format);
        }
    }
    if (schema.release) schema.release(&schema);
}

static void test_scanner_blob_handling(const char *blob_uri) {
    printf("  test_scanner_blob_handling... ");

    LanceDataset *ds = lance_dataset_open(blob_uri, NULL, 0);
    ASSERT(ds != NULL, "blob dataset open failed");
    uint64_t expected_rows = lance_dataset_count_rows(ds);
    CHECK_OK();

    char format[16];
    struct ArrowArrayStream stream;

    /* By default a blob column arrives as its description struct. */
    LanceScanner *scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "scanner creation failed");
    memset(&stream, 0, sizeof(stream));
    int32_t rc = lance_scanner_to_arrow_stream(scanner, &stream);
    ASSERT(rc == 0, "to_arrow_stream failed");
    blob_column_format(&stream, format, sizeof(format));
    ASSERT(strcmp(format, "+s") == 0, "default blob column should be a struct");
    if (stream.release) stream.release(&stream);
    lance_scanner_close(scanner);

    /* ALL_BINARY materializes the bytes as LargeBinary and keeps every row. */
    scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "scanner creation failed");
    rc = lance_scanner_set_blob_handling(scanner, LANCE_BLOB_HANDLING_ALL_BINARY);
    ASSERT(rc == 0, "set_blob_handling failed");
    memset(&stream, 0, sizeof(stream));
    rc = lance_scanner_to_arrow_stream(scanner, &stream);
    ASSERT(rc == 0, "to_arrow_stream failed");
    blob_column_format(&stream, format, sizeof(format));
    ASSERT(strcmp(format, "Z") == 0, "ALL_BINARY blob column should be LargeBinary");

    uint64_t total_rows = 0;
    while (1) {
        struct ArrowArray array;
        memset(&array, 0, sizeof(array));
        rc = stream.get_next(&stream, &array);
        ASSERT(rc == 0, "get_next failed");
        if (array.release == NULL) {
            break;
        }
        total_rows += (uint64_t)array.length;
        array.release(&array);
    }
    ASSERT(total_rows == expected_rows, "row count mismatch");
    if (stream.release) stream.release(&stream);

    /* Once the scan has started the setting is rejected. */
    rc = lance_scanner_set_blob_handling(scanner, LANCE_BLOB_HANDLING_BLOBS_DESCRIPTIONS);
    ASSERT(rc == -1, "set_blob_handling after the scan started should fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT, "wrong error code");
    const char *msg = lance_last_error_message();
    if (msg) lance_free_string(msg);

    printf("rows=%llu... ", (unsigned long long)total_rows);
    lance_scanner_close(scanner);
    lance_dataset_close(ds);
    printf("OK\n");
}

/* Byte `i` of every blob payload in the smoke fixture. */
static uint8_t blob_byte(size_t i) { return (uint8_t)(i * 7 + 3); }

/* Check that `bytes` are the payload bytes starting at `offset`. */
static void assert_blob_payload(const uint8_t *bytes, size_t len, size_t offset) {
    for (size_t i = 0; i < len; i++) {
        ASSERT(bytes[i] == blob_byte(offset + i), "blob payload mismatch");
    }
}

static void test_take_blobs(const char *blob_uri) {
    printf("  test_take_blobs... ");

    LanceDataset *ds = lance_dataset_open(blob_uri, NULL, 0);
    ASSERT(ds != NULL, "blob dataset open failed");

    /* The first fragment holds an inline, a packed, a dedicated, an empty and
     * a null blob, in that order. */
    const uint64_t indices[] = {0, 1, 2, 3, 4};
    LanceBlobFile *blobs[5] = {0};
    int32_t rc = lance_dataset_take_blobs_by_indices(ds, indices, 5, "blob", blobs);
    ASSERT(rc == 0, "take_blobs_by_indices failed");

    const uint64_t sizes[] = {8, 128, 1024, 0};
    uint8_t buffer[1024];
    for (size_t i = 0; i < 4; i++) {
        ASSERT(blobs[i] != NULL, "a non-null blob should yield a handle");
        uint64_t size = lance_blob_file_size(blobs[i]);
        CHECK_OK();
        ASSERT(size == sizes[i], "blob size mismatch");
        rc = lance_blob_file_read(blobs[i], buffer, (size_t)size);
        ASSERT(rc == 0, "blob read failed");
        assert_blob_payload(buffer, (size_t)size, 0);
    }
    ASSERT(blobs[4] == NULL, "a null blob should yield a NULL slot");

    /* Cursor and positional reads on the packed blob. */
    LanceBlobFile *packed = blobs[1];
    rc = lance_blob_file_seek(packed, 100);
    ASSERT(rc == 0, "seek failed");
    size_t bytes_read = 0;
    rc = lance_blob_file_read_up_to(packed, buffer, 64, &bytes_read);
    ASSERT(rc == 0, "read_up_to failed");
    ASSERT(bytes_read == 28, "read_up_to should stop at the end of the blob");
    assert_blob_payload(buffer, bytes_read, 100);
    uint64_t pos = 0;
    rc = lance_blob_file_tell(packed, &pos);
    ASSERT(rc == 0, "tell failed");
    ASSERT(pos == 128, "cursor should be at the end");
    rc = lance_blob_file_read_range(packed, 40, buffer, 16);
    ASSERT(rc == 0, "read_range failed");
    assert_blob_payload(buffer, 16, 40);
    rc = lance_blob_file_tell(packed, &pos);
    ASSERT(rc == 0 && pos == 128, "read_range must not move the cursor");

    /* A buffer smaller than the remaining bytes is rejected, not truncated. */
    rc = lance_blob_file_seek(packed, 0);
    ASSERT(rc == 0, "seek failed");
    rc = lance_blob_file_read(packed, buffer, 64);
    ASSERT(rc == -1, "a short buffer should be rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT, "wrong error code");
    const char *msg = lance_last_error_message();
    ASSERT(msg != NULL, "an error message is expected");
    lance_free_string(msg);

    /* A column that is not a blob column is rejected and leaves `out` alone. */
    LanceBlobFile *untouched[5] = {0};
    rc = lance_dataset_take_blobs_by_indices(ds, indices, 5, "raw", untouched);
    ASSERT(rc == -1, "a non-blob column should be rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT, "wrong error code");
    msg = lance_last_error_message();
    if (msg) lance_free_string(msg);
    for (size_t i = 0; i < 5; i++) {
        ASSERT(untouched[i] == NULL, "out must stay untouched on error");
    }

    /* Handles stay readable after the dataset is closed. */
    lance_dataset_close(ds);
    rc = lance_blob_file_read_range(blobs[2], 0, buffer, 16);
    ASSERT(rc == 0, "read after the dataset was closed failed");
    assert_blob_payload(buffer, 16, 0);

    for (size_t i = 0; i < 5; i++) {
        lance_blob_file_close(blobs[i]); /* NULL-safe for the null slot */
    }
    printf("OK\n");
}

static void test_blob_native_read_slice(const char *blob_uri) {
    printf("  test_blob_native_read_slice... ");

    LanceDataset *ds = lance_dataset_open(blob_uri, NULL, 0);
    ASSERT(ds != NULL, "blob dataset open failed");

    const uint64_t addresses[] = {
        0,
        (((uint64_t)1) << 32) | 2,
        (((uint64_t)1) << 32) | 1,
        3,
        4,
        0,
    };
    LanceBlobFile *addressed[6] = {0};
    int32_t rc = lance_dataset_take_blobs_by_addresses(
        ds, addresses, 6, "blob", addressed);
    ASSERT(rc == 0, "take_blobs_by_addresses failed");

    /* Empty selections reach native column validation, unlike the old
     * entry points. Failures leave `out` untouched. */
    LanceBlobFile *untouched[2] = {addressed[0], addressed[0]};
    rc = lance_dataset_take_blobs_by_addresses(
        ds, NULL, 0, "does_not_exist", untouched);
    ASSERT(rc == -1, "empty unknown column should fail");
    expect_error_contains(
        LANCE_ERR_INTERNAL, "does_not_exist", "empty unknown column");
    ASSERT(untouched[0] == addressed[0] && untouched[1] == addressed[0],
           "failed by-address take must leave out untouched");

    rc = lance_dataset_take_blobs(
        ds, NULL, 0, "does_not_exist", untouched);
    ASSERT(rc == 0, "legacy take must keep its early-empty behavior");
    ASSERT(untouched[0] == addressed[0] && untouched[1] == addressed[0],
           "legacy empty take must leave out untouched");

    rc = lance_dataset_take_blobs_by_addresses(
        ds, NULL, 0, "raw", untouched);
    ASSERT(rc == -1, "empty non-blob column should fail");
    expect_error_contains(
        LANCE_ERR_INVALID_ARGUMENT, "not a blob column", "non-blob column");

    const uint64_t invalid_addresses[] = {0, UINT64_MAX - 1};
    rc = lance_dataset_take_blobs_by_addresses(
        ds, invalid_addresses, 2, "blob", untouched);
    ASSERT(rc == -1, "partially invalid addresses should fail");
    expect_error(LANCE_ERR_INVALID_ARGUMENT, "invalid row address");
    ASSERT(untouched[0] == addressed[0] && untouched[1] == addressed[0],
           "a partially valid request must not fill out");

    rc = lance_dataset_take_blobs_by_addresses(
        ds, NULL, 1, "blob", untouched);
    ASSERT(rc == -1, "NULL row_addresses with count 1 should fail");
    expect_error_contains(
        LANCE_ERR_INVALID_ARGUMENT,
        "row_addresses must not be NULL",
        "NULL row_addresses");

    /* Handles own their readers; the dataset can be released first. */
    lance_dataset_close(ds);

    const uint64_t sizes[] = {8, 1024, 128, 0, 0, 8};
    uint8_t buffer[1024];
    for (size_t i = 0; i < 6; i++) {
        if (i == 4) {
            ASSERT(addressed[i] == NULL, "the null blob must yield a NULL slot");
            continue;
        }
        ASSERT(addressed[i] != NULL, "a non-null blob must yield a handle");
        ASSERT(lance_blob_file_size(addressed[i]) == sizes[i],
               "by-address size mismatch");
        if (i == 5) {
            uint64_t duplicate_cursor = UINT64_MAX;
            rc = lance_blob_file_tell(addressed[i], &duplicate_cursor);
            ASSERT(rc == 0 && duplicate_cursor == 0,
                   "duplicate addresses must have independent cursors");
        }
        rc = lance_blob_file_read(
            addressed[i], buffer, (size_t)sizes[i]);
        ASSERT(rc == 0, "by-address read after dataset close failed");
        assert_blob_payload(buffer, (size_t)sizes[i], 0);
    }

    /* Ordered, overlapping, duplicate and empty ranges. Capacities are larger
     * than requested lengths and their tails must stay untouched. */
    LanceBlobFile *packed = addressed[2];
    rc = lance_blob_file_seek(packed, 5);
    ASSERT(rc == 0, "seek failed");

    const LanceBlobRange ranges[] = {
        {100, 20},
        {128, 0},
        {40, 16},
        {100, 8},
        {127, 1},
        {0, 128},
    };
    uint8_t range0[32];
    uint8_t range2[24];
    uint8_t range3[8];
    uint8_t range4[2];
    uint8_t range5[130];
    memset(range0, 0xEE, sizeof(range0));
    memset(range2, 0xEE, sizeof(range2));
    memset(range3, 0xEE, sizeof(range3));
    memset(range4, 0xEE, sizeof(range4));
    memset(range5, 0xEE, sizeof(range5));
    uint8_t *destinations[] = {
        range0, NULL, range2, range3, range4, range5,
    };
    const size_t destination_lens[] = {
        sizeof(range0), 0, sizeof(range2), sizeof(range3),
        sizeof(range4), sizeof(range5),
    };
    rc = lance_blob_file_read_ranges(
        packed, ranges, 6, destinations, destination_lens);
    ASSERT(rc == 0, "read_ranges failed");
    assert_blob_payload(range0, 20, 100);
    assert_blob_payload(range2, 16, 40);
    assert_blob_payload(range3, 8, 100);
    assert_blob_payload(range4, 1, 127);
    assert_blob_payload(range5, 128, 0);
    for (size_t i = 20; i < sizeof(range0); i++) {
        ASSERT(range0[i] == 0xEE, "range0 tail was modified");
    }
    for (size_t i = 16; i < sizeof(range2); i++) {
        ASSERT(range2[i] == 0xEE, "range2 tail was modified");
    }
    for (size_t i = 1; i < sizeof(range4); i++) {
        ASSERT(range4[i] == 0xEE, "range4 tail was modified");
    }
    for (size_t i = 128; i < sizeof(range5); i++) {
        ASSERT(range5[i] == 0xEE, "range5 tail was modified");
    }
    uint64_t cursor = UINT64_MAX;
    rc = lance_blob_file_tell(packed, &cursor);
    ASSERT(rc == 0 && cursor == 5, "read_ranges moved the cursor");

    /* Representative invalid requests must not write any destination. */
    const LanceBlobRange small_range[] = {{0, 4}};
    uint8_t small_destination[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    uint8_t *small_destinations[] = {small_destination};
    const size_t short_lens[] = {3};
    rc = lance_blob_file_read_ranges(
        packed, small_range, 1, small_destinations, short_lens);
    ASSERT(rc == -1, "short destination should fail");
    expect_error_contains(
        LANCE_ERR_INVALID_ARGUMENT, "smaller than", "short destination");
    for (size_t i = 0; i < sizeof(small_destination); i++) {
        ASSERT(small_destination[i] == 0xAA,
               "short destination was modified");
    }

    const LanceBlobRange overflow_range[] = {{UINT64_MAX, 2}};
    uint8_t overflow_destination[2] = {0xAA, 0xAA};
    uint8_t *overflow_destinations[] = {overflow_destination};
    const size_t overflow_lens[] = {sizeof(overflow_destination)};
    rc = lance_blob_file_read_ranges(
        packed, overflow_range, 1, overflow_destinations, overflow_lens);
    ASSERT(rc == -1, "overflowing range should fail");
    expect_error_contains(
        LANCE_ERR_INVALID_ARGUMENT, "overflows", "overflowing range");
    ASSERT(overflow_destination[0] == 0xAA &&
           overflow_destination[1] == 0xAA,
           "overflow destination was modified");

    const LanceBlobRange mixed_ranges[] = {{0, 4}, {127, 2}};
    uint8_t mixed0[8];
    uint8_t mixed1[8];
    memset(mixed0, 0xBB, sizeof(mixed0));
    memset(mixed1, 0xCC, sizeof(mixed1));
    uint8_t *mixed_destinations[] = {mixed0, mixed1};
    const size_t mixed_lens[] = {sizeof(mixed0), sizeof(mixed1)};
    rc = lance_blob_file_read_ranges(
        packed, mixed_ranges, 2, mixed_destinations, mixed_lens);
    ASSERT(rc == -1, "out-of-bounds range should fail");
    expect_error_contains(
        LANCE_ERR_INVALID_ARGUMENT, "exceeds blob size", "out-of-bounds range");
    for (size_t i = 0; i < sizeof(mixed0); i++) {
        ASSERT(mixed0[i] == 0xBB, "mixed valid destination was modified");
    }
    for (size_t i = 0; i < sizeof(mixed1); i++) {
        ASSERT(mixed1[i] == 0xCC, "mixed invalid destination was modified");
    }

    rc = lance_blob_file_read_ranges(
        packed, NULL, 1, destinations, destination_lens);
    ASSERT(rc == -1, "NULL ranges with count 1 should fail");
    expect_error_contains(
        LANCE_ERR_INVALID_ARGUMENT, "ranges must not be NULL", "NULL ranges");
    rc = lance_blob_file_read_ranges(
        packed, ranges, 1, NULL, destination_lens);
    ASSERT(rc == -1, "NULL destinations with count 1 should fail");
    expect_error_contains(
        LANCE_ERR_INVALID_ARGUMENT,
        "destinations must not be NULL",
        "NULL destinations");
    rc = lance_blob_file_read_ranges(
        packed, ranges, 1, destinations, NULL);
    ASSERT(rc == -1, "NULL destination lengths with count 1 should fail");
    expect_error_contains(
        LANCE_ERR_INVALID_ARGUMENT,
        "destination_lens must not be NULL",
        "NULL destination_lens");

    rc = lance_blob_file_read_ranges(
        packed, NULL, 0, NULL, NULL);
    ASSERT(rc == 0, "empty read_ranges on an open handle should succeed");

    rc = lance_blob_file_set_buffer_size(packed, 0);
    ASSERT(rc == 0, "set_buffer_size(0) failed");
    rc = lance_blob_file_set_buffer_size(packed, 64);
    ASSERT(rc == 0, "set_buffer_size(64) failed");
    rc = lance_blob_file_set_buffer_size(packed, 1024 * 1024);
    ASSERT(rc == 0, "set_buffer_size(large) failed");

    bool closed = true;
    rc = lance_blob_file_is_closed(packed, &closed);
    ASSERT(rc == 0 && !closed, "a fresh handle should be open");
    rc = lance_blob_file_mark_closed(packed);
    ASSERT(rc == 0, "mark_closed failed");
    rc = lance_blob_file_is_closed(packed, &closed);
    ASSERT(rc == 0 && closed, "mark_closed did not update native state");
    ASSERT(lance_blob_file_size(packed) == 128,
           "size should remain available after native close");
    rc = lance_blob_file_mark_closed(packed);
    ASSERT(rc == 0, "mark_closed should be idempotent");

    rc = lance_blob_file_read_ranges(
        packed, NULL, 0, NULL, NULL);
    ASSERT(rc == -1, "empty read_ranges on a closed handle should fail");
    expect_error_contains(
        LANCE_ERR_INVALID_ARGUMENT, "already closed", "closed read_ranges");

    rc = lance_blob_file_set_buffer_size(packed, 32);
    ASSERT(rc == -1, "set_buffer_size on a closed handle should fail");
    expect_error_contains(
        LANCE_ERR_INVALID_ARGUMENT, "already closed", "closed set_buffer_size");

    /* Preserve the old zero-length single-range behavior. */
    memset(range0, 0xEE, sizeof(range0));
    rc = lance_blob_file_read_range(packed, 0, NULL, 0);
    ASSERT(rc == 0, "zero-length read_range must keep succeeding");
    rc = lance_blob_file_read_range(packed, 0, range0, 1);
    ASSERT(rc == -1, "non-empty read_range on a closed handle should fail");
    expect_error_contains(
        LANCE_ERR_INVALID_ARGUMENT, "already closed", "closed read_range");
    ASSERT(range0[0] == 0xEE, "closed read_range modified its destination");

    for (size_t i = 0; i < 6; i++) {
        lance_blob_file_close(addressed[i]);
    }
    printf("OK\n");
}

static int array_value_is_null(const struct ArrowArray *array, int64_t index) {
    if (array->null_count == 0) return 0;
    ASSERT(array->buffers[0] != NULL, "nullable array must carry a validity buffer");
    int64_t bit = array->offset + index;
    const uint8_t *validity = (const uint8_t *)array->buffers[0];
    return (validity[bit / 8] & (uint8_t)(1u << (bit % 8))) == 0;
}

static uint64_t u64_array_value(const struct ArrowArray *array, int64_t index) {
    ASSERT(array->buffers[1] != NULL, "UInt64 array must carry values");
    const uint64_t *values = (const uint64_t *)array->buffers[1];
    return values[array->offset + index];
}

static uint32_t u32_array_value(const struct ArrowArray *array, int64_t index) {
    ASSERT(array->buffers[1] != NULL, "UInt32 array must carry values");
    const uint32_t *values = (const uint32_t *)array->buffers[1];
    return values[array->offset + index];
}

static void test_blob_fields_and_descriptors(void) {
    printf("  test_blob_fields_and_descriptors... ");

    LanceBlobFieldOptions options = {
        true, 0,
        true, 256,
    };
    struct ArrowSchema field_schema = {0};
    ASSERT(lance_blob_field_with_options(
               "blob", true, &options, &field_schema) == 0,
           "blob_field_with_options failed");
    ASSERT(strcmp(field_schema.name, "blob") == 0, "field name");
    ASSERT(strcmp(field_schema.format, "+s") == 0, "blob field must be a struct");
    ASSERT((field_schema.flags & ARROW_FLAG_NULLABLE) != 0, "nullable flag");
    assert_field_metadata(
        &field_schema, "ARROW:extension:name", "lance.blob.v2");
    assert_field_metadata(
        &field_schema,
        "lance-encoding:blob-inline-size-threshold",
        "0");
    assert_field_metadata(
        &field_schema,
        "lance-encoding:blob-dedicated-size-threshold",
        "256");
    ASSERT(field_schema.n_children == 2, "minimal blob field child count");
    ASSERT(strcmp(field_schema.children[0]->name, "data") == 0,
           "blob field child 0");
    ASSERT(strcmp(field_schema.children[1]->name, "uri") == 0,
           "blob field child 1");
    field_schema.release(&field_schema);

    LanceBlobFieldOptions invalid = {0};
    invalid.has_dedicated_size_threshold = true;
    invalid.dedicated_size_threshold = 0;
    struct ArrowSchema untouched = {0};
    ASSERT(lance_blob_field_with_options(
               "invalid", true, &invalid, &untouched) == -1,
           "zero dedicated threshold must fail");
    expect_error_contains(
        LANCE_ERR_INVALID_ARGUMENT,
        "greater than 0",
        "zero dedicated threshold");
    ASSERT(untouched.release == NULL && untouched.format == NULL,
           "failed field export must leave out untouched");

    LanceBlobDescriptorArrayBuilder *builder =
        lance_blob_descriptor_array_builder_new("blob");
    ASSERT(builder != NULL, "descriptor builder creation failed");
    ASSERT(lance_blob_descriptor_array_builder_append_inline(
               builder, (const uint8_t *)"inline payload", 14) == 0,
           "inline append");
    ASSERT(lance_blob_descriptor_array_builder_append_null(builder) == 0,
           "null append");
    ASSERT(lance_blob_descriptor_array_builder_append_packed(
               builder, 7, 100, 8) == 0,
           "packed append");
    ASSERT(lance_blob_descriptor_array_builder_append_dedicated(
               builder, 9, 1024) == 0,
           "dedicated append");
    ASSERT(lance_blob_descriptor_array_builder_append_managed(
               builder, "data/a.blob", 2, 3, false, 0) == 0,
           "managed base None append");
    ASSERT(lance_blob_descriptor_array_builder_append_managed(
               builder, "data/b.blob", 4, 5, true, 0) == 0,
           "managed base Some(0) append");
    const LanceBlobRange packed_ranges[] = {{1, 2}, {3, 4}};
    ASSERT(lance_blob_descriptor_array_builder_extend_packed(
               builder, 11, packed_ranges, 2) == 0,
           "extend_packed");
    ASSERT(lance_blob_descriptor_array_builder_push_external(
               builder, "file:///tmp/external.bin", true, 7, 9) == 0,
           "push_external");
    ASSERT(lance_blob_descriptor_array_builder_append_external(
               builder, 3, "s3://bucket/object", 11, 13) == 0,
           "append_external");
    ASSERT(lance_blob_descriptor_array_builder_append_inline(
               builder, NULL, 0) == 0,
           "valid empty inline append");

    struct ArrowSchema builder_field = {0};
    ASSERT(lance_blob_descriptor_array_builder_field(
               builder, &builder_field) == 0,
           "builder field export");
    assert_field_metadata(
        &builder_field, "ARROW:extension:name", "lance.blob.v2");
    builder_field.release(&builder_field);

    struct ArrowArray array = {0};
    struct ArrowSchema schema = {0};
    ASSERT(lance_blob_descriptor_array_builder_finish(
               builder, &array, &schema) == 0,
           "descriptor finish");
    ASSERT(array.release != NULL && schema.release != NULL,
           "finish must export both Arrow parts");
    ASSERT(array.length == 11, "descriptor array length");
    ASSERT(array.n_children == 6, "prepared descriptor child count");
    ASSERT(schema.n_children == 6, "prepared schema child count");
    const char *prepared_names[] = {
        "kind", "data", "uri", "blob_id", "blob_size", "position",
    };
    for (size_t i = 0; i < 6; ++i) {
        ASSERT(strcmp(schema.children[i]->name, prepared_names[i]) == 0,
               "prepared schema child name/order");
    }

    const struct ArrowArray *kinds = array.children[0];
    const struct ArrowArray *data = array.children[1];
    const struct ArrowArray *blob_ids = array.children[3];
    const uint8_t *kind_values = (const uint8_t *)kinds->buffers[1];
    ASSERT(!array_value_is_null(kinds, 0) && kind_values[0] == 0,
           "inline kind");
    ASSERT(array_value_is_null(kinds, 1), "null row kind");
    ASSERT(kind_values[2] == 1, "packed kind");
    ASSERT(kind_values[3] == 2, "dedicated kind");
    ASSERT(kind_values[4] == 4, "Managed without base kind");
    ASSERT(kind_values[5] == 5, "ManagedWithBase kind");
    ASSERT(kind_values[8] == 3 && kind_values[9] == 3,
           "external descriptor kinds");
    ASSERT(array_value_is_null(blob_ids, 4),
           "Managed None must have a null blob_id");
    ASSERT(!array_value_is_null(blob_ids, 5) &&
               u32_array_value(blob_ids, 5) == 0,
           "Managed Some(0) must retain an explicit zero blob_id");
    ASSERT(!array_value_is_null(data, 0), "inline data must be non-null");
    const int64_t *data_offsets = (const int64_t *)data->buffers[1];
    ASSERT(data_offsets[1] - data_offsets[0] == 14,
           "inline payload length");
    ASSERT(memcmp((const uint8_t *)data->buffers[2] + data_offsets[0],
                  "inline payload", 14) == 0,
           "inline payload bytes");

    struct ArrowSchema post_finish_field = {0};
    ASSERT(lance_blob_descriptor_array_builder_field(
               builder, &post_finish_field) == 0,
           "field must remain available after finish");
    post_finish_field.release(&post_finish_field);
    ASSERT(lance_blob_descriptor_array_builder_append_null(builder) == -1,
           "append after finish must fail");
    expect_error_contains(
        LANCE_ERR_INVALID_ARGUMENT, "already finished", "append after finish");

    array.release(&array);
    schema.release(&schema);
    lance_blob_descriptor_array_builder_close(builder);
    printf("OK\n");
}

static void assert_blob_payload_with_seed(
    const uint8_t *bytes,
    size_t len,
    size_t offset,
    size_t seed) {
    for (size_t i = 0; i < len; ++i) {
        ASSERT(bytes[i] == (uint8_t)((offset + i) * 7 + 3 + seed),
               "planned blob payload mismatch");
    }
}

static void test_blob_planned_reads(const char *blob_uri) {
    printf("  test_blob_planned_reads... ");

    LanceDataset *ds = lance_dataset_open(blob_uri, NULL, 0);
    ASSERT(ds != NULL, "blob dataset open failed");
    const uint64_t indices[] = {7, 2, 3, 4, 9, 0};
    const uint64_t expected_addresses[] = {
        (((uint64_t)1) << 32) | 2,
        2,
        3,
        4,
        (((uint64_t)1) << 32) | 4,
        0,
    };
    const size_t expected_sizes[] = {1024, 1024, 0, 0, 0, 8};
    const size_t expected_seeds[] = {0, 0, 0, 0, 0, 0};
    const int expected_null[] = {0, 0, 0, 1, 1, 0};

    LanceBlobReadOptions options = {0};
    options.order = LANCE_BLOB_READ_PRESERVE_ORDER;
    struct ArrowArrayStream stream = {0};
    ASSERT(lance_dataset_read_blobs(
               ds, "blob", LANCE_BLOB_ROWS_INDICES, indices, 6,
               &options, &stream) == 0,
           "planned whole-blob read failed");
    struct ArrowSchema schema = {0};
    ASSERT(stream.get_schema(&stream, &schema) == 0, "whole schema");
    ASSERT(schema.n_children == 2, "whole schema child count");
    ASSERT(strcmp(schema.children[0]->name, "row_address") == 0 &&
               strcmp(schema.children[0]->format, "L") == 0,
           "whole row_address schema");
    ASSERT(strcmp(schema.children[1]->name, "data") == 0 &&
               strcmp(schema.children[1]->format, "Z") == 0,
           "whole data schema");
    schema.release(&schema);

    for (size_t row = 0; row < 6; ++row) {
        struct ArrowArray array = {0};
        ASSERT(stream.get_next(&stream, &array) == 0,
               "whole get_next failed");
        ASSERT(array.release != NULL, "expected one whole record");
        ASSERT(array.length == 1 && array.n_children == 2,
               "one whole result per Arrow record");
        ASSERT(u64_array_value(array.children[0], 0) == expected_addresses[row],
               "whole row address");
        const struct ArrowArray *data = array.children[1];
        if (expected_null[row]) {
            ASSERT(array_value_is_null(data, 0), "NULL blob must stay NULL");
        } else {
            ASSERT(!array_value_is_null(data, 0),
                   "non-NULL blob must not be NULL");
            const int64_t *offsets = (const int64_t *)data->buffers[1];
            int64_t first = data->offset;
            size_t len =
                (size_t)(offsets[first + 1] - offsets[first]);
            ASSERT(len == expected_sizes[row], "whole payload length");
            assert_blob_payload_with_seed(
                (const uint8_t *)data->buffers[2] + offsets[first],
                len, 0, expected_seeds[row]);
        }
        array.release(&array);
    }
    struct ArrowArray eos = {0};
    ASSERT(stream.get_next(&stream, &eos) == 0 && eos.release == NULL,
           "whole stream must end after native results");
    stream.release(&stream);

    const LanceBlobRangeRequest requests[] = {
        {7, 100, 20},
        {2, 128, 0},
        {9, 40, 16},
        {2, 100, 8},
        {3, 0, 0},
        {4, 1000, 100},
    };
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_dataset_read_blob_ranges(
               ds, "blob", LANCE_BLOB_ROWS_INDICES, requests, 6,
               &options, &stream) == 0,
           "planned range read failed");
    memset(&schema, 0, sizeof(schema));
    ASSERT(stream.get_schema(&stream, &schema) == 0, "range schema");
    const char *range_names[] = {
        "request_index", "row_address", "offset", "length", "data",
    };
    ASSERT(schema.n_children == 5, "range schema child count");
    for (size_t i = 0; i < 5; ++i) {
        ASSERT(strcmp(schema.children[i]->name, range_names[i]) == 0,
               "range schema child name/order");
    }
    schema.release(&schema);

    const uint8_t expected_null_range[] = {0, 0, 0, 0, 0, 1};
    const size_t expected_range_sizes[] = {20, 0, 16, 8, 0, 0};
    for (size_t request = 0; request < 6; ++request) {
        struct ArrowArray array = {0};
        ASSERT(stream.get_next(&stream, &array) == 0,
               "range get_next failed");
        ASSERT(array.release != NULL, "expected one range record");
        ASSERT(array.length == 1 && array.n_children == 5,
               "one range result per Arrow record");
        ASSERT(u64_array_value(array.children[0], 0) == request,
               "request_index identity");
        ASSERT(u64_array_value(array.children[1], 0) ==
                   (request == 0 ? ((((uint64_t)1) << 32) | 2) :
                    request == 2 ? ((((uint64_t)1) << 32) | 4) :
                    request == 3 ? 2 : request == 4 ? 3 :
                    request == 5 ? 4 : 2),
               "range row address");
        const struct ArrowArray *data = array.children[4];
        if (expected_null_range[request]) {
            ASSERT(array_value_is_null(data, 0),
                   "NULL range must stay NULL");
        } else {
            ASSERT(!array_value_is_null(data, 0),
                   "non-NULL range must not be NULL");
            const int64_t *offsets = (const int64_t *)data->buffers[1];
            int64_t first = data->offset;
            ASSERT((size_t)(offsets[first + 1] - offsets[first]) ==
                       expected_range_sizes[request],
                   "range payload length");
        }
        array.release(&array);
    }
    memset(&eos, 0, sizeof(eos));
    ASSERT(stream.get_next(&stream, &eos) == 0 && eos.release == NULL,
           "range stream must end");
    stream.release(&stream);

    memset(&stream, 0, sizeof(stream));
    const uint64_t empty_rows = 0;
    (void)empty_rows;
    ASSERT(lance_dataset_read_blobs(
               ds, "blob", LANCE_BLOB_ROWS_INDICES, NULL, 0,
               &options, &stream) == 0,
           "explicit empty planned read failed");
    memset(&schema, 0, sizeof(schema));
    ASSERT(stream.get_schema(&stream, &schema) == 0, "empty read schema");
    ASSERT(schema.n_children == 2, "empty read schema shape");
    schema.release(&schema);
    memset(&eos, 0, sizeof(eos));
    ASSERT(stream.get_next(&stream, &eos) == 0 && eos.release == NULL,
           "explicit empty selection must yield no records");
    stream.release(&stream);

    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_dataset_read_blobs(
               ds, "blob", LANCE_BLOB_ROWS_NONE, NULL, 0,
               &options, &stream) == -1,
           "native no-selection must fail");
    expect_error_contains(
        LANCE_ERR_INVALID_ARGUMENT, "requires a row selection",
        "planned read no-selection");
    ASSERT(stream.release == NULL, "failed factory must not write stream");

    lance_dataset_close(ds);
    printf("OK\n");
}

typedef struct {
    struct ArrowSchema *child;
} WrappedSchemaState;

static void release_wrapped_schema(struct ArrowSchema *schema) {
    WrappedSchemaState *state = (WrappedSchemaState *)schema->private_data;
    if (state != NULL) {
        if (state->child != NULL) {
            if (state->child->release != NULL) {
                state->child->release(state->child);
            }
            free(state->child);
        }
        free(state);
    }
    free((void *)schema->format);
    free((void *)schema->name);
    free((void *)schema->metadata);
    free(schema->children);
    schema->release = NULL;
}

static char *managed_schema_metadata(void) {
    const char *key = "lance.blob.managed";
    const char *value = "true";
    int32_t key_len = (int32_t)strlen(key);
    int32_t value_len = (int32_t)strlen(value);
    int32_t pair_count = 1;
    size_t size = sizeof(pair_count) + sizeof(key_len) + (size_t)key_len +
                  sizeof(value_len) + (size_t)value_len;
    char *metadata = (char *)malloc(size);
    ASSERT(metadata != NULL, "allocate schema metadata");
    char *cursor = metadata;
    memcpy(cursor, &pair_count, sizeof(pair_count));
    cursor += sizeof(pair_count);
    memcpy(cursor, &key_len, sizeof(key_len));
    cursor += sizeof(key_len);
    memcpy(cursor, key, (size_t)key_len);
    cursor += key_len;
    memcpy(cursor, &value_len, sizeof(value_len));
    cursor += sizeof(value_len);
    memcpy(cursor, value, (size_t)value_len);
    return metadata;
}

static struct ArrowSchema *wrap_record_schema(
    struct ArrowSchema *child,
    int managed_opt_in) {
    struct ArrowSchema *schema =
        (struct ArrowSchema *)calloc(1, sizeof(*schema));
    WrappedSchemaState *state =
        (WrappedSchemaState *)calloc(1, sizeof(*state));
    ASSERT(schema != NULL && state != NULL, "allocate wrapped schema");
    schema->format = strdup("+s");
    schema->metadata = managed_opt_in ? managed_schema_metadata() : NULL;
    schema->flags = 0;
    schema->n_children = 1;
    schema->children =
        (struct ArrowSchema **)calloc(1, sizeof(struct ArrowSchema *));
    ASSERT(schema->format != NULL && schema->children != NULL,
           "allocate wrapped schema children");
    state->child = child;
    schema->children[0] = child;
    schema->dictionary = NULL;
    schema->release = release_wrapped_schema;
    schema->private_data = state;
    return schema;
}

static void release_wrapped_array(struct ArrowArray *array) {
    for (int64_t i = 0; i < array->n_children; ++i) {
        if (array->children != NULL && array->children[i] != NULL) {
            if (array->children[i]->release != NULL) {
                array->children[i]->release(array->children[i]);
            }
            free(array->children[i]);
        }
    }
    free(array->children);
    free((void *)array->buffers);
    array->release = NULL;
}

static struct ArrowArray *wrap_record_array(struct ArrowArray *child) {
    struct ArrowArray *array =
        (struct ArrowArray *)calloc(1, sizeof(*array));
    const void **buffers =
        (const void **)calloc(1, sizeof(const void *));
    struct ArrowArray **children =
        (struct ArrowArray **)calloc(1, sizeof(struct ArrowArray *));
    ASSERT(array != NULL && buffers != NULL && children != NULL,
           "allocate wrapped array");
    array->length = child->length;
    array->null_count = 0;
    array->offset = 0;
    array->n_buffers = 1;
    array->n_children = 1;
    array->buffers = buffers;
    array->children = children;
    array->dictionary = NULL;
    array->release = release_wrapped_array;
    array->private_data = NULL;
    array->children[0] = child;
    return array;
}

typedef struct {
    struct ArrowSchema schema;
    int schema_taken;
    struct ArrowArray array;
    int array_taken;
} PreparedStreamState;

static int prepared_stream_get_schema(
    struct ArrowArrayStream *stream,
    struct ArrowSchema *out) {
    PreparedStreamState *state =
        (PreparedStreamState *)stream->private_data;
    if (state->schema_taken) {
        return -1;
    }
    *out = state->schema;
    memset(&state->schema, 0, sizeof(state->schema));
    state->schema_taken = 1;
    return 0;
}

static int prepared_stream_get_next(
    struct ArrowArrayStream *stream,
    struct ArrowArray *out) {
    PreparedStreamState *state =
        (PreparedStreamState *)stream->private_data;
    if (state->array_taken) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    *out = state->array;
    memset(&state->array, 0, sizeof(state->array));
    state->array_taken = 1;
    return 0;
}

static const char *prepared_stream_last_error(
    struct ArrowArrayStream *stream) {
    (void)stream;
    return NULL;
}

static void prepared_stream_release(struct ArrowArrayStream *stream) {
    PreparedStreamState *state =
        (PreparedStreamState *)stream->private_data;
    if (state == NULL) return;
    if (state->schema.release != NULL) state->schema.release(&state->schema);
    if (state->array.release != NULL) state->array.release(&state->array);
    free(state);
    memset(stream, 0, sizeof(*stream));
}

static int write_managed_descriptor_stream(
    const char *uri,
    int32_t mode,
    uint64_t offset,
    uint64_t length,
    int managed_opt_in) {
    LanceBlobDescriptorArrayBuilder *builder =
        lance_blob_descriptor_array_builder_new("blob");
    ASSERT(builder != NULL, "managed descriptor builder creation failed");
    ASSERT(lance_blob_descriptor_array_builder_append_managed(
               builder, "_blobs/managed.bin", offset, length, false, 0) == 0,
           "managed descriptor append failed");

    struct ArrowSchema *argument_child =
        (struct ArrowSchema *)calloc(1, sizeof(*argument_child));
    struct ArrowSchema *stream_child =
        (struct ArrowSchema *)calloc(1, sizeof(*stream_child));
    ASSERT(argument_child != NULL && stream_child != NULL,
           "allocate prepared helper schemas");
    ASSERT(lance_blob_descriptor_array_builder_field(
               builder, argument_child) == 0,
           "argument field export failed");
    ASSERT(lance_blob_descriptor_array_builder_field(
               builder, stream_child) == 0,
           "stream field export failed");

    struct ArrowArray prepared_array = {0};
    struct ArrowSchema finished_field = {0};
    ASSERT(lance_blob_descriptor_array_builder_finish(
               builder, &prepared_array, &finished_field) == 0,
           "managed descriptor finish failed");
    finished_field.release(&finished_field);
    lance_blob_descriptor_array_builder_close(builder);

    struct ArrowArray *array =
        (struct ArrowArray *)calloc(1, sizeof(*array));
    ASSERT(array != NULL, "allocate prepared array");
    *array = prepared_array;
    memset(&prepared_array, 0, sizeof(prepared_array));

    struct ArrowSchema *argument_schema =
        wrap_record_schema(argument_child, managed_opt_in);
    PreparedStreamState *state =
        (PreparedStreamState *)calloc(1, sizeof(*state));
    ASSERT(state != NULL, "allocate prepared stream state");
    struct ArrowSchema *wrapped_schema =
        wrap_record_schema(stream_child, managed_opt_in);
    state->schema = *wrapped_schema;
    free(wrapped_schema);
    struct ArrowArray *wrapped_array = wrap_record_array(array);
    state->array = *wrapped_array;
    free(wrapped_array);

    struct ArrowArrayStream stream = {0};
    stream.get_schema = prepared_stream_get_schema;
    stream.get_next = prepared_stream_get_next;
    stream.get_last_error = prepared_stream_last_error;
    stream.release = prepared_stream_release;
    stream.private_data = state;

    LanceWriteParams params = {0};
    params.data_storage_version = "2.2";
    LanceDataset *dataset = NULL;
    int32_t rc = lance_dataset_write_with_params(
        uri, argument_schema, &stream, mode, &params, NULL, &dataset);
    argument_schema->release(argument_schema);
    free(argument_schema);
    if (rc == 0) {
        ASSERT(dataset != NULL, "managed write returned null dataset");
        lance_dataset_close(dataset);
    }
    return rc;
}

static void test_managed_blob_descriptor_roundtrip(const char *write_uri) {
    printf("  test_managed_blob_descriptor_roundtrip... ");

    char managed_uri[4096];
    char no_opt_in_uri[4096];
    int written = snprintf(
        managed_uri, sizeof(managed_uri), "%s.managed-blob", write_uri);
    ASSERT(written > 0 && (size_t)written < sizeof(managed_uri),
           "managed dataset URI too long");
    written = snprintf(
        no_opt_in_uri, sizeof(no_opt_in_uri), "%s.no-managed-opt-in",
        write_uri);
    ASSERT(written > 0 && (size_t)written < sizeof(no_opt_in_uri),
           "no-opt-in dataset URI too long");

    ASSERT(write_managed_descriptor_stream(
               managed_uri, LANCE_WRITE_CREATE, 1, 5, 1) == 0,
           "managed create failed");

    char object_dir[4096];
    char object_path[4096];
    written = snprintf(object_dir, sizeof(object_dir), "%s/_blobs", managed_uri);
    ASSERT(written > 0 && (size_t)written < sizeof(object_dir),
           "managed object directory URI too long");
    written = snprintf(
        object_path, sizeof(object_path), "%s/managed.bin", object_dir);
    ASSERT(written > 0 && (size_t)written < sizeof(object_path),
           "managed object path too long");
    (void)mkdir(object_dir, 0755);
    FILE *object = fopen(object_path, "wb");
    ASSERT(object != NULL, "create managed object");
    ASSERT(fwrite("0123456789", 1, 10, object) == 10,
           "write managed object");
    ASSERT(fclose(object) == 0, "close managed object");

    LanceDataset *ds = lance_dataset_open(managed_uri, NULL, 0);
    ASSERT(ds != NULL, "managed dataset open failed");
    const uint64_t first_index = 0;
    LanceBlobFile *blob = NULL;
    ASSERT(lance_dataset_take_blobs_by_indices(
               ds, &first_index, 1, "blob", &blob) == 0 && blob != NULL,
           "managed first read failed");
    uint8_t bytes[16] = {0};
    ASSERT(lance_blob_file_read(blob, bytes, 5) == 0, "managed blob read");
    ASSERT(memcmp(bytes, "12345", 5) == 0, "managed first range bytes");
    lance_blob_file_close(blob);
    lance_dataset_close(ds);

    ASSERT(write_managed_descriptor_stream(
               managed_uri, LANCE_WRITE_APPEND, 5, 3, 1) == 0,
           "managed append failed");
    ds = lance_dataset_open(managed_uri, NULL, 0);
    ASSERT(ds != NULL, "managed reopen failed");
    const uint64_t both_indices[] = {0, 1};
    LanceBlobFile *blobs[2] = {0};
    ASSERT(lance_dataset_take_blobs_by_indices(
               ds, both_indices, 2, "blob", blobs) == 0,
           "managed reuse read failed");
    ASSERT(blobs[0] != NULL && blobs[1] != NULL,
           "managed reuse handles");
    ASSERT(lance_blob_file_read(blobs[0], bytes, 5) == 0 &&
               memcmp(bytes, "12345", 5) == 0,
           "managed reuse first bytes");
    ASSERT(lance_blob_file_read(blobs[1], bytes, 3) == 0 &&
               memcmp(bytes, "567", 3) == 0,
           "managed reuse second bytes");
    lance_blob_file_close(blobs[0]);
    lance_blob_file_close(blobs[1]);
    lance_dataset_close(ds);

    ASSERT(write_managed_descriptor_stream(
               no_opt_in_uri, LANCE_WRITE_CREATE, 0, 1, 0) == -1,
           "managed write must require schema opt-in");
    expect_error_contains(
        LANCE_ERR_INVALID_ARGUMENT,
        "lance.blob.managed=true",
        "managed schema opt-in");

    printf("OK\n");
}

static void test_versions(const char *uri) {
    printf("  test_versions... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");

    LanceVersions *vs = lance_dataset_versions(ds);
    ASSERT(vs != NULL, "versions snapshot failed");

    uint64_t n = lance_versions_count(vs);
    ASSERT(n >= 1, "at least one version expected");
    printf("count=%llu... ", (unsigned long long)n);

    for (uint64_t i = 0; i < n; i++) {
        uint64_t id = lance_versions_id_at(vs, (size_t)i);
        int64_t ts = lance_versions_timestamp_ms_at(vs, (size_t)i);
        CHECK_OK();
        ASSERT(id >= 1, "version id should be >= 1");
        ASSERT(ts > 0, "timestamp should be populated");
    }

    lance_versions_close(vs);
    lance_dataset_close(ds);
    printf("OK\n");
}

static const char *version_metadata_get(const LanceVersion *version, const char *key) {
    uint64_t count = lance_version_metadata_count(version);
    CHECK_OK();
    for (uint64_t i = 0; i < count; ++i) {
        const char *candidate = lance_version_metadata_key_at(version, (size_t)i);
        ASSERT(candidate != NULL, "version metadata key");
        if (strcmp(candidate, key) == 0) {
            const char *value = lance_version_metadata_value_at(version, (size_t)i);
            ASSERT(value != NULL, "version metadata value");
            return value;
        }
    }
    return NULL;
}

static const char *tag_metadata_get(const LanceTag *tag, const char *key) {
    uint64_t count = lance_tag_metadata_count(tag);
    CHECK_OK();
    for (uint64_t i = 0; i < count; ++i) {
        const char *candidate = lance_tag_metadata_key_at(tag, (size_t)i);
        ASSERT(candidate != NULL, "tag metadata key");
        if (strcmp(candidate, key) == 0) {
            const char *value = lance_tag_metadata_value_at(tag, (size_t)i);
            ASSERT(value != NULL, "tag metadata value");
            return value;
        }
    }
    return NULL;
}

static void expect_error(LanceErrorCode expected, const char *operation) {
    if (lance_last_error_code() != expected) {
        const char *msg = lance_last_error_message();
        fprintf(stderr, "FAIL: %s returned error code %d, expected %d: %s\n",
                operation, (int)lance_last_error_code(), (int)expected,
                msg ? msg : "unknown");
        if (msg) lance_free_string(msg);
        exit(1);
    }
    const char *msg = lance_last_error_message();
    if (msg) lance_free_string(msg);
}

static void expect_error_contains(
    LanceErrorCode expected,
    const char *needle,
    const char *operation) {
    if (lance_last_error_code() != expected) {
        const char *msg = lance_last_error_message();
        fprintf(stderr, "FAIL: %s returned error code %d, expected %d: %s\n",
                operation, (int)lance_last_error_code(), (int)expected,
                msg ? msg : "unknown");
        if (msg) lance_free_string(msg);
        exit(1);
    }
    const char *msg = lance_last_error_message();
    ASSERT(msg != NULL, "an error message is expected");
    ASSERT(strstr(msg, needle) != NULL, "error message did not contain expected text");
    lance_free_string(msg);
}

static void test_native_refs_and_tags(const char *uri) {
    printf("  test_native_refs_and_tags... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");
    ASSERT(lance_dataset_version(ds) == 2, "fixture must have two versions");
    ASSERT(lance_dataset_count_rows(ds) == 20, "fixture must have twenty rows");

    LanceVersion *current = lance_dataset_current_version(ds);
    ASSERT(current != NULL, "current version snapshot failed");
    ASSERT(lance_version_id(current) == 2, "wrong current version id");
    ASSERT(lance_version_timestamp_ns(current) > 0, "missing native timestamp");
    ASSERT(strcmp(version_metadata_get(current, "total_rows"), "20") == 0,
           "current version row metadata");
    ASSERT(strcmp(version_metadata_get(current, "total_fragments"), "2") == 0,
           "current version fragment metadata");

    LanceVersions *versions = lance_dataset_versions(ds);
    ASSERT(versions != NULL, "version snapshot failed");
    ASSERT(lance_versions_count(versions) == 2, "wrong version count");
    const LanceVersion *first = lance_versions_at(versions, 0);
    ASSERT(first != NULL, "first version details");
    ASSERT(lance_version_id(first) == 1, "wrong first version id");
    ASSERT(strcmp(version_metadata_get(first, "total_rows"), "10") == 0,
           "first version row metadata");

    LanceVersionRefs *refs = lance_dataset_version_refs(ds);
    ASSERT(refs != NULL, "version refs snapshot failed");
    ASSERT(lance_version_refs_count(refs) == 2, "wrong version ref count");
    ASSERT(lance_version_refs_id_at(refs, 0) == 1, "first version ref");
    ASSERT(lance_version_refs_id_at(refs, 1) == 2, "second version ref");

    LanceRef *version1 = lance_ref_version_number(1);
    LanceRef *latest = lance_ref_version(NULL, false, 0);
    LanceRef *main_v1 = lance_ref_version("main", true, 1);
    ASSERT(version1 != NULL && latest != NULL && main_v1 != NULL, "ref creation");

    LanceDataset *checked_v1 = lance_dataset_checkout_ref(ds, version1);
    LanceDataset *checked_latest = lance_dataset_checkout_ref(ds, latest);
    LanceDataset *checked_main = lance_dataset_checkout_ref(ds, main_v1);
    ASSERT(checked_v1 != NULL && checked_latest != NULL && checked_main != NULL,
           "ref checkout");
    ASSERT(lance_dataset_version(checked_v1) == 1 &&
           lance_dataset_count_rows(checked_v1) == 10,
           "version-number checkout");
    ASSERT(lance_dataset_version(checked_latest) == 2 &&
           lance_dataset_count_rows(checked_latest) == 20,
           "latest checkout");
    ASSERT(lance_dataset_version(checked_main) == 1,
           "main branch alias checkout");

    LanceRef *tag_source = lance_ref_version_number(1);
    ASSERT(lance_dataset_tag_create(ds, "c-release", tag_source) == 0,
           "tag create");
    lance_ref_close(tag_source);
    LanceTag *tag = lance_dataset_tag_get(ds, "c-release");
    ASSERT(tag != NULL, "tag get");
    ASSERT(strcmp(lance_tag_name(tag), "c-release") == 0, "tag name");
    ASSERT(lance_tag_version(tag) == 1, "tag version");
    ASSERT(lance_tag_manifest_size(tag) > 0, "tag manifest size");
    const char *branch = "unset";
    ASSERT(lance_tag_branch(tag, &branch) == 0 && branch == NULL,
           "main tag has no explicit branch");
    int64_t created_at = 0;
    int64_t updated_at = 0;
    ASSERT(lance_tag_created_at_ns(tag, &created_at) == 1 && created_at > 0,
           "tag created timestamp");
    ASSERT(lance_tag_updated_at_ns(tag, &updated_at) == 1 && updated_at >= created_at,
           "tag updated timestamp");

    LanceRef *duplicate = lance_ref_version_number(2);
    ASSERT(lance_dataset_tag_create(ds, "c-release", duplicate) == -1,
           "duplicate tag must conflict");
    expect_error(LANCE_ERR_COMMIT_CONFLICT, "duplicate tag");
    lance_ref_close(duplicate);

    LanceRef *tag_ref = lance_ref_tag("c-release");
    LanceDataset *tag_checkout = lance_dataset_checkout_ref(ds, tag_ref);
    ASSERT(tag_checkout != NULL && lance_dataset_version(tag_checkout) == 1,
           "tag checkout before update");

    const char *metadata[] = {"owner", "c-api", "empty", "", NULL};
    ASSERT(lance_dataset_tag_replace_metadata(ds, "c-release", metadata) == 0,
           "tag metadata replace");

    LanceTags *tags = lance_dataset_tags(ds);
    ASSERT(tags != NULL && lance_tags_count(tags) == 1, "tag list");
    const LanceTag *listed = lance_tags_at(tags, 0);
    ASSERT(listed != NULL && strcmp(lance_tag_name(listed), "c-release") == 0,
           "listed tag");

    lance_dataset_close(ds);
    ASSERT(lance_version_id(current) == 2, "current snapshot after dataset close");
    ASSERT(lance_versions_count(versions) == 2, "versions snapshot after dataset close");
    ASSERT(lance_version_refs_count(refs) == 2, "refs snapshot after dataset close");
    ASSERT(lance_tags_count(tags) == 1, "tags snapshot after dataset close");
    ASSERT(lance_dataset_version(tag_checkout) == 1,
           "old checkout after dataset close");

    ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "reopen failed");
    tag = lance_dataset_tag_get(ds, "c-release");
    ASSERT(tag != NULL, "persisted tag");
    ASSERT(strcmp(tag_metadata_get(tag, "owner"), "c-api") == 0,
           "persisted metadata value");
    ASSERT(strcmp(tag_metadata_get(tag, "empty"), "") == 0,
           "empty metadata value");
    lance_tag_close(tag);

    LanceRef *version2 = lance_ref_version_number(2);
    ASSERT(lance_dataset_tag_update(ds, "c-release", version2) == 0,
           "tag update");
    lance_ref_close(version2);
    tag = lance_dataset_tag_get(ds, "c-release");
    ASSERT(tag != NULL && lance_tag_version(tag) == 2, "updated tag target");
    ASSERT(strcmp(tag_metadata_get(tag, "owner"), "c-api") == 0,
           "update preserves metadata");
    int64_t updated_created_at = 0;
    ASSERT(lance_tag_created_at_ns(tag, &updated_created_at) == 1 &&
           updated_created_at == created_at,
           "update preserves created_at");
    lance_tag_close(tag);

    ASSERT(lance_dataset_version(tag_checkout) == 1,
           "pre-update checkout remains old");
    LanceDataset *updated_checkout = lance_dataset_checkout_ref(ds, tag_ref);
    ASSERT(updated_checkout != NULL && lance_dataset_version(updated_checkout) == 2,
           "updated tag checkout");

    ASSERT(lance_dataset_tag_delete(ds, "c-release") == 0, "tag delete");
    ASSERT(lance_dataset_tag_delete(ds, "c-release") == -1, "unknown tag delete");
    expect_error(LANCE_ERR_NOT_FOUND, "unknown tag delete");

    lance_ref_close(tag_ref);
    lance_dataset_close(updated_checkout);
    lance_dataset_close(tag_checkout);
    lance_dataset_close(checked_main);
    lance_dataset_close(checked_latest);
    lance_dataset_close(checked_v1);
    lance_ref_close(main_v1);
    lance_ref_close(latest);
    lance_ref_close(version1);
    lance_tags_close(tags);
    lance_version_refs_close(refs);
    lance_versions_close(versions);
    lance_version_close(current);
    lance_dataset_close(ds);
    printf("OK\n");
}

static void test_branch_lifecycle(const char *uri) {
    printf("  test_branch_lifecycle... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");
    ASSERT(lance_dataset_version(ds) == 2, "fixture must have two versions");
    ASSERT(lance_dataset_count_rows(ds) == 20, "fixture must have twenty rows");

    const char *json = lance_dataset_branch_identifier_json(ds);
    ASSERT(json != NULL, "main branch identifier JSON");
    ASSERT(strcmp(json, "{\"version_mapping\":[]}") == 0,
           "main branch identifier must have an empty mapping");
    lance_free_string(json);

    json = lance_dataset_branches_json(ds);
    ASSERT(json != NULL, "initial branch list JSON");
    ASSERT(strcmp(json, "{}") == 0, "fixture must start without branches");
    lance_free_string(json);

    LanceRef *version1 = lance_ref_version_number(1);
    ASSERT(version1 != NULL, "version ref");
    LanceDataset *parent =
        lance_dataset_branch_create(ds, "c/parent", version1, NULL);
    ASSERT(parent != NULL, "branch create");
    lance_ref_close(version1);
    ASSERT(lance_dataset_version(parent) == 1, "branch version");
    ASSERT(lance_dataset_count_rows(parent) == 10, "branch row count");

    LanceRef *child_ref = lance_ref_version_number(1);
    LanceDataset *child = lance_dataset_branch_create(
        parent, "c/parent/child", child_ref, NULL);
    ASSERT(child != NULL, "child branch create");
    lance_ref_close(child_ref);
    ASSERT(lance_dataset_count_rows(child) == 10, "child branch row count");

    const char *parent_json =
        lance_dataset_branch_get_json(ds, "c/parent");
    ASSERT(parent_json != NULL, "branch get JSON");
    ASSERT(strstr(parent_json, "\"parentBranch\":null") != NULL,
           "main-based branch has no parent branch");
    ASSERT(strstr(parent_json, "\"parentVersion\":1") != NULL,
           "branch parent version");
    ASSERT(strstr(parent_json, "\"createAt\":") != NULL,
           "branch createAt");
    ASSERT(strstr(parent_json, "\"manifestSize\":") != NULL,
           "branch manifestSize");
    ASSERT(strstr(parent_json, "\"identifier\":{\"version_mapping\":") != NULL,
           "branch identifier mapping");
    ASSERT(strstr(parent_json, "\"metadata\":{}") != NULL,
           "branch starts with empty metadata");
    lance_free_string(parent_json);

    const char *child_json =
        lance_dataset_branch_get_json(ds, "c/parent/child");
    ASSERT(child_json != NULL, "child branch get JSON");
    ASSERT(strstr(child_json, "\"parentBranch\":\"c/parent\"") != NULL,
           "child branch parent");
    ASSERT(strstr(child_json, "\"parentVersion\":1") != NULL,
           "child branch parent version");
    lance_free_string(child_json);

    const char *branches_json = lance_dataset_branches_json(ds);
    ASSERT(branches_json != NULL, "branch list JSON");
    ASSERT(strstr(branches_json, "\"c/parent\"") != NULL,
           "branch list includes parent");
    ASSERT(strstr(branches_json, "\"c/parent/child\"") != NULL,
           "branch list includes child");

    const char *metadata[] = {"owner", "c-api", "empty", "", NULL};
    ASSERT(lance_dataset_branch_replace_metadata(
               ds, "c/parent", metadata) == 0,
           "branch metadata replace");
    parent_json = lance_dataset_branch_get_json(ds, "c/parent");
    ASSERT(parent_json != NULL, "branch metadata JSON");
    ASSERT(strstr(parent_json, "\"owner\":\"c-api\"") != NULL,
           "branch metadata owner");
    ASSERT(strstr(parent_json, "\"empty\":\"\"") != NULL,
           "branch metadata empty value");
    lance_free_string(parent_json);

    LanceDataset *parent_checkout =
        lance_dataset_checkout_branch(ds, "c/parent");
    LanceDataset *main_checkout =
        lance_dataset_checkout_branch(ds, "main");
    ASSERT(parent_checkout != NULL && main_checkout != NULL,
           "branch checkout");
    ASSERT(lance_dataset_version(parent_checkout) == 1 &&
               lance_dataset_count_rows(parent_checkout) == 10,
           "parent branch checkout snapshot");
    ASSERT(lance_dataset_version(main_checkout) == 2 &&
               lance_dataset_count_rows(main_checkout) == 20,
           "main branch checkout");

    LanceRef *reserved = lance_ref_version("main", true, 1);
    ASSERT(lance_dataset_branch_create(ds, "main", reserved, NULL) == NULL,
           "main is reserved for branch creation");
    expect_error(LANCE_ERR_INVALID_ARGUMENT, "create reserved main branch");
    lance_ref_close(reserved);

    LanceRef *duplicate = lance_ref_version_number(2);
    ASSERT(lance_dataset_branch_create(ds, "c/parent", duplicate, NULL) == NULL,
           "duplicate branch must fail");
    ASSERT(lance_last_error_code() != LANCE_OK, "duplicate branch error");
    const char *duplicate_message = lance_last_error_message();
    if (duplicate_message) lance_free_string(duplicate_message);
    lance_ref_close(duplicate);

    ASSERT(lance_dataset_branch_delete(ds, "c/parent") == -1,
           "parent with child must not delete");
    expect_error(LANCE_ERR_COMMIT_CONFLICT, "delete parent branch");

    /* JSON and checkout handles own their data. */
    const char *owned_list = lance_dataset_branches_json(ds);
    const char *owned_parent = lance_dataset_branch_get_json(ds, "c/parent");
    ASSERT(owned_list != NULL && owned_parent != NULL, "owned branch JSON");
    lance_dataset_close(ds);
    ASSERT(strstr(owned_list, "\"c/parent\"") != NULL,
           "branch list after source close");
    ASSERT(strstr(owned_parent, "\"owner\":\"c-api\"") != NULL,
           "branch JSON after source close");
    ASSERT(lance_dataset_count_rows(parent_checkout) == 10,
           "checkout after source close");
    lance_free_string(owned_parent);
    lance_free_string(owned_list);

    ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "reopen failed");
    parent_json = lance_dataset_branch_get_json(ds, "c/parent");
    ASSERT(parent_json != NULL, "persisted branch metadata");
    ASSERT(strstr(parent_json, "\"owner\":\"c-api\"") != NULL,
           "persisted branch metadata value");
    lance_free_string(parent_json);

    /* NULL or an empty array fully replaces metadata with an empty map. */
    ASSERT(lance_dataset_branch_replace_metadata(ds, "c/parent", NULL) == 0,
           "clear branch metadata");
    parent_json = lance_dataset_branch_get_json(ds, "c/parent");
    ASSERT(parent_json != NULL && strstr(parent_json, "\"owner\"") == NULL,
           "cleared branch metadata");
    lance_free_string(parent_json);

    LanceRef *empty_options_ref = lance_ref_version_number(1);
    const char *empty_options[] = {NULL};
    LanceDataset *options_branch = lance_dataset_branch_create(
        ds, "c/empty-options", empty_options_ref, empty_options);
    ASSERT(options_branch != NULL, "explicit empty storage options");
    lance_ref_close(empty_options_ref);
    lance_dataset_close(options_branch);
    ASSERT(lance_dataset_branch_delete(ds, "c/empty-options") == 0,
           "delete explicit-options branch");

    ASSERT(lance_dataset_branch_delete(ds, "c/parent/child") == 0,
           "delete child branch");
    ASSERT(lance_dataset_branch_delete(ds, "c/parent") == 0,
           "delete parent branch after child");

    LanceRef *tagged_ref = lance_ref_version_number(1);
    LanceDataset *tagged =
        lance_dataset_branch_create(ds, "c/tagged", tagged_ref, NULL);
    ASSERT(tagged != NULL, "tagged branch create");
    lance_ref_close(tagged_ref);
    LanceRef *tag_target = lance_ref_version("c/tagged", true, 1);
    ASSERT(lance_dataset_tag_create(ds, "c-branch-tag", tag_target) == 0,
           "tag branch");
    ASSERT(lance_dataset_branch_delete(ds, "c/tagged") == -1,
           "tagged branch must not delete");
    expect_error(LANCE_ERR_COMMIT_CONFLICT, "delete tagged branch");
    ASSERT(lance_dataset_tag_delete(ds, "c-branch-tag") == 0,
           "delete branch tag");
    ASSERT(lance_dataset_branch_delete(ds, "c/tagged") == 0,
           "delete tagged branch after tag");

    json = lance_dataset_branches_json(ds);
    ASSERT(json != NULL, "final branch list");
    ASSERT(strcmp(json, "{}") == 0, "branch list must be empty");
    lance_free_string(json);

    ASSERT(lance_dataset_branch_get_json(ds, "missing") == NULL,
           "missing branch get");
    expect_error(LANCE_ERR_NOT_FOUND, "missing branch get");
    ASSERT(lance_dataset_checkout_branch(ds, "missing") == NULL,
           "missing branch checkout");
    expect_error(LANCE_ERR_NOT_FOUND, "missing branch checkout");
    ASSERT(lance_dataset_branch_delete(ds, "missing") == -1,
           "missing branch delete");
    expect_error(LANCE_ERR_NOT_FOUND, "missing branch delete");

    ASSERT(lance_dataset_branches_json(NULL) == NULL,
           "NULL branch list");
    expect_error(LANCE_ERR_INVALID_ARGUMENT, "NULL branch list");
    ASSERT(lance_dataset_branch_identifier_json(NULL) == NULL,
           "NULL branch identifier");
    expect_error(LANCE_ERR_INVALID_ARGUMENT, "NULL branch identifier");
    ASSERT(branches_json != NULL, "keep branch JSON alive");
    lance_free_string(branches_json);

    lance_ref_close(tag_target);
    lance_dataset_close(tagged);
    lance_dataset_close(ds);
    lance_dataset_close(main_checkout);
    lance_dataset_close(parent_checkout);
    lance_dataset_close(child);
    lance_dataset_close(parent);
    printf("OK\n");
}

/* Restore the dataset to its own current version — always commits a new
 * manifest (no skip-if-equal optimization) so the caller's "make `version`
 * the new latest" intent holds even under concurrent writers. */
static void test_restore_to_current(const char *uri) {
    printf("  test_restore_to_current... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");
    uint64_t current = lance_dataset_version(ds);

    LanceDataset *after = lance_dataset_restore(ds, current);
    ASSERT(after != NULL, "restore failed");
    ASSERT(lance_dataset_version(after) == current + 1,
           "restore must bump the version to commit a fresh manifest");

    lance_dataset_close(after);
    lance_dataset_close(ds);
    printf("OK\n");
}

static void test_error_handling(void) {
    printf("  test_error_handling... ");

    /* Open non-existent dataset */
    LanceDataset *ds = lance_dataset_open("file:///nonexistent/path/xyz", NULL, 0);
    ASSERT(ds == NULL, "should fail to open nonexistent dataset");
    ASSERT(lance_last_error_code() != LANCE_OK, "error code should be set");

    const char *msg = lance_last_error_message();
    ASSERT(msg != NULL, "error message should be set");
    ASSERT(strlen(msg) > 0, "error message should be non-empty");
    lance_free_string(msg);

    /* NULL safety */
    lance_dataset_close(NULL);
    lance_scanner_close(NULL);
    lance_batch_free(NULL);
    lance_free_string(NULL);

    printf("OK\n");
}

/* Round-trip: scan src dataset to an ArrowArrayStream, write it into a new
 * dataset at dst_uri, and verify row counts match. dst_uri must not pre-exist. */
static void test_dataset_write_roundtrip(const char *src_uri, const char *dst_uri) {
    printf("  test_dataset_write_roundtrip... ");

    LanceDataset *src = lance_dataset_open(src_uri, NULL, 0);
    ASSERT(src != NULL, "open source failed");
    uint64_t src_rows = lance_dataset_count_rows(src);
    CHECK_OK();

    LanceScanner *scanner = lance_scanner_new(src, NULL, NULL);
    ASSERT(scanner != NULL, "scanner creation failed");

    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    int32_t rc = lance_scanner_to_arrow_stream(scanner, &stream);
    ASSERT(rc == 0, "to_arrow_stream failed");

    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    rc = stream.get_schema(&stream, &schema);
    ASSERT(rc == 0, "get_schema from stream failed");

    LanceDataset *dst = NULL;
    rc = lance_dataset_write(
        dst_uri, &schema, &stream, LANCE_WRITE_CREATE, NULL, &dst);

    /* The Rust side reads `schema` by shared reference and never releases it,
     * so we must release it ourselves on every return path — including
     * failure. Release before the ASSERTs so a failed write doesn't leak. */
    if (schema.release) schema.release(&schema);

    ASSERT(rc == 0, "lance_dataset_write failed");
    ASSERT(dst != NULL, "out_dataset should be populated");

    uint64_t dst_rows = lance_dataset_count_rows(dst);
    CHECK_OK();
    ASSERT(dst_rows == src_rows, "row count mismatch after write");
    printf("src=%llu, dst=%llu... ",
           (unsigned long long)src_rows, (unsigned long long)dst_rows);

    lance_dataset_close(dst);
    lance_scanner_close(scanner);
    lance_dataset_close(src);
    printf("OK\n");
}

/* Exercises the native write-options path and generic config read/update from
 * a real C caller. The destination was created by the legacy writer, so its
 * 20-version / 14-day policy must survive the append and be visible here. */
static void test_write_options_and_config(const char *src_uri,
                                          const char *write_uri) {
    printf("  test_write_options_and_config... ");

    LanceDataset *src = lance_dataset_open(src_uri, NULL, 0);
    ASSERT(src != NULL, "open source failed");
    LanceScanner *scanner = lance_scanner_new(src, NULL, NULL);
    ASSERT(scanner != NULL, "scanner creation failed");

    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    int32_t rc = lance_scanner_to_arrow_stream(scanner, &stream);
    ASSERT(rc == 0, "to_arrow_stream failed");

    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    rc = stream.get_schema(&stream, &schema);
    ASSERT(rc == 0, "get_schema from stream failed");

    LanceAutoCleanupParams cleanup = {1, 0, 0};
    LanceWriteOptions options;
    memset(&options, 0, sizeof(options));
    options.auto_cleanup = &cleanup;
    options.skip_auto_cleanup = true;

    LanceDataset *ds = NULL;
    rc = lance_dataset_write_with_options(
        write_uri, &schema, &stream, LANCE_WRITE_APPEND, &options, NULL, &ds);
    if (schema.release) schema.release(&schema);
    ASSERT(rc == 0, "lance_dataset_write_with_options append failed");
    CHECK_OK();
    ASSERT(ds != NULL, "out_dataset should be populated");
    ASSERT(lance_dataset_count_rows(ds) > 0, "append should add rows");
    CHECK_OK();

    LanceDatasetConfig *config = lance_dataset_config(ds);
    ASSERT(config != NULL, "config snapshot failed");
    uint64_t config_count = lance_dataset_config_count(config);
    CHECK_OK();
    int saw_interval = 0;
    int saw_older_than = 0;
    for (uint64_t i = 0; i < config_count; i++) {
        const char *key = lance_dataset_config_key_at(config, (size_t)i);
        const char *value = lance_dataset_config_value_at(config, (size_t)i);
        CHECK_OK();
        ASSERT(key != NULL && value != NULL, "config entry must be non-NULL");
        if (strcmp(key, "lance.auto_cleanup.interval") == 0) {
            saw_interval = 1;
            ASSERT(strcmp(value, "20") == 0,
                   "legacy interval must survive native append");
        } else if (strcmp(key, "lance.auto_cleanup.older_than") == 0) {
            saw_older_than = 1;
            ASSERT(strcmp(value, "14days") == 0,
                   "legacy older_than must survive native append");
        }
    }
    ASSERT(saw_interval && saw_older_than,
           "legacy cleanup policy must remain persisted");
    lance_dataset_config_close(config);

    const char *keys[] = {"c_test.key", "lance.auto_cleanup.older_than"};
    const char *values[] = {"value", NULL};
    rc = lance_dataset_update_config(ds, keys, values, 2);
    ASSERT(rc == 0, "config update failed");
    CHECK_OK();
    LanceDatasetConfig *after_update = lance_dataset_config(ds);
    ASSERT(after_update != NULL, "post-update config snapshot failed");
    uint64_t after_update_count = lance_dataset_config_count(after_update);
    CHECK_OK();
    lance_dataset_config_close(after_update);

    /* A snapshot taken before close must remain usable after the dataset
     * handle is gone. */
    LanceDatasetConfig *snapshot = lance_dataset_config(ds);
    ASSERT(snapshot != NULL, "post-update config snapshot failed");
    lance_dataset_close(ds);
    uint64_t snapshot_count = lance_dataset_config_count(snapshot);
    CHECK_OK();
    ASSERT(snapshot_count == after_update_count, "snapshot count mismatch");
    const char *snapshot_key = lance_dataset_config_key_at(snapshot, 0);
    const char *snapshot_value = lance_dataset_config_value_at(snapshot, 0);
    CHECK_OK();
    ASSERT(snapshot_key != NULL && snapshot_value != NULL,
           "snapshot accessors must stay valid after dataset close");
    lance_dataset_config_close(snapshot);

    LanceDataset *reopened = lance_dataset_open(write_uri, NULL, 0);
    ASSERT(reopened != NULL, "reopen failed");
    config = lance_dataset_config(reopened);
    ASSERT(config != NULL, "reopened config snapshot failed");
    config_count = lance_dataset_config_count(config);
    CHECK_OK();
    int saw_custom = 0;
    int saw_removed_older_than = 0;
    for (uint64_t i = 0; i < config_count; i++) {
        const char *key = lance_dataset_config_key_at(config, (size_t)i);
        const char *value = lance_dataset_config_value_at(config, (size_t)i);
        CHECK_OK();
        ASSERT(key != NULL && value != NULL, "config entry must be non-NULL");
        if (strcmp(key, "c_test.key") == 0) {
            saw_custom = 1;
            ASSERT(strcmp(value, "value") == 0, "custom config mismatch");
        } else if (strcmp(key, "lance.auto_cleanup.older_than") == 0) {
            saw_removed_older_than = 1;
        }
    }
    ASSERT(saw_custom, "custom config must persist across reopen");
    ASSERT(!saw_removed_older_than, "removed config must stay removed");
    lance_dataset_config_close(config);

    /* Error paths: NULL dataset and NULL arrays are rejected. */
    rc = lance_dataset_update_config(NULL, keys, values, 2);
    ASSERT(rc == -1, "NULL dataset must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");
    const char *message = lance_last_error_message();
    if (message) lance_free_string(message);
    rc = lance_dataset_update_config(reopened, NULL, values, 2);
    ASSERT(rc == -1, "NULL keys must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");
    message = lance_last_error_message();
    if (message) lance_free_string(message);

    lance_dataset_close(reopened);
    lance_scanner_close(scanner);
    lance_dataset_close(src);
    printf("OK\n");
}

/* Exercises `lance_dataset_calculate_data_stats` on the freshly-written
 * (modern, v2+) dataset, where per-field on-disk sizes are populated. Runs
 * before the mutation tests reshape or empty the dataset. */
static void test_data_statistics(const char *write_uri) {
    printf("  test_data_statistics... ");

    LanceDataset *ds = lance_dataset_open(write_uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");

    LanceDataStatistics *stats = lance_dataset_calculate_data_stats(ds);
    ASSERT(stats != NULL, "data stats failed");

    uint64_t n = lance_data_statistics_count(stats);
    CHECK_OK();
    ASSERT(n >= 1, "at least one field expected");

    uint64_t total = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t id = lance_data_statistics_field_id_at(stats, (size_t)i);
        uint64_t bytes = lance_data_statistics_bytes_on_disk_at(stats, (size_t)i);
        CHECK_OK();
        (void)id;
        total += bytes;
    }
    ASSERT(total > 0, "modern storage should report non-zero on-disk size");

    /* Out-of-range index is rejected with INVALID_ARGUMENT on both accessors. */
    (void)lance_data_statistics_field_id_at(stats, (size_t)n);
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "out-of-range field_id must fail");
    (void)lance_data_statistics_bytes_on_disk_at(stats, (size_t)n);
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "out-of-range bytes_on_disk must fail");

    lance_data_statistics_close(stats);
    lance_dataset_close(ds);

    /* NULL dataset is rejected before anything is allocated. */
    ASSERT(lance_dataset_calculate_data_stats(NULL) == NULL,
           "NULL dataset must yield NULL handle");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    /* NULL handle is rejected by every accessor. */
    ASSERT(lance_data_statistics_count(NULL) == 0, "NULL handle count must be 0");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL handle count must set INVALID_ARGUMENT");
    (void)lance_data_statistics_field_id_at(NULL, 0);
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL handle field_id must fail");
    (void)lance_data_statistics_bytes_on_disk_at(NULL, 0);
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL handle bytes_on_disk must fail");
    lance_data_statistics_close(NULL); /* must be a safe no-op */

    printf("fields=%llu... OK\n", (unsigned long long)n);
}

static void test_config_replacement(const char *uri) {
    printf("  test_config_replacement... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "config replace fixture open failed");
    LanceDataset *old = lance_dataset_open(uri, NULL, 0);
    ASSERT(old != NULL, "old config fixture open failed");
    uint64_t before_version = lance_dataset_version(ds);
    CHECK_OK();
    uint64_t old_version = lance_dataset_version(old);
    CHECK_OK();
    ASSERT(config_has_value(ds, "fixture.key", "fixture-value"),
           "fixture config key missing");
    ASSERT(config_has_value(ds, "fixture.remove", "present"),
           "fixture removable config missing");

    const char *merge_keys[] = {
        "fixture.key", "fixture.remove", "fixture.empty",
        "dup", "dup", "", "added",
    };
    const char *merge_values[] = {
        "updated", NULL, "", "first", "second", "", "one",
    };
    const char *merged = lance_dataset_update_config_json(
        ds,
        merge_keys,
        merge_values,
        sizeof(merge_keys) / sizeof(merge_keys[0]),
        LANCE_METADATA_UPDATE_MERGE);
    ASSERT(merged != NULL, "config merge JSON failed");
    CHECK_OK();
    ASSERT(json_contains(merged, "\"fixture.key\":\"updated\""),
           "merge must update existing key");
    ASSERT(!json_contains(merged, "\"fixture.remove\""),
           "NULL merge value must remove key");
    ASSERT(json_contains(merged, "\"fixture.empty\":\"\""),
           "empty string must remain a value");
    ASSERT(json_contains(merged, "\"dup\":\"second\""),
           "duplicate key must follow native last-write-wins");
    ASSERT(json_contains(merged, "\"\":\"\""),
           "empty key and value must round trip");
    lance_free_string(merged);

    ASSERT(lance_dataset_version(ds) == before_version + 1,
           "merge must commit one version");
    ASSERT(lance_dataset_version(old) == old_version,
           "old handle must retain its snapshot");
    ASSERT(config_has_value(old, "fixture.key", "fixture-value"),
           "old handle config must remain unchanged");

    LanceDataset *historical = lance_dataset_open(uri, NULL, before_version);
    ASSERT(historical != NULL, "historical config version open failed");
    ASSERT(config_has_value(historical, "fixture.key", "fixture-value"),
           "historical version config must remain unchanged");
    lance_dataset_close(historical);

    const char *replace_keys[] = {"a", "b", "gone"};
    const char *replace_values[] = {"1", "", NULL};
    const char *replaced = lance_dataset_update_config_json(
        ds,
        replace_keys,
        replace_values,
        3,
        LANCE_METADATA_UPDATE_REPLACE);
    ASSERT(replaced != NULL, "config replace JSON failed");
    CHECK_OK();
    ASSERT(strcmp(replaced, "{\"a\":\"1\",\"b\":\"\"}") == 0,
           "replace must return the complete map");
    lance_free_string(replaced);

    const char *cleared = lance_dataset_update_config_json(
        ds, NULL, NULL, 0, LANCE_METADATA_UPDATE_REPLACE);
    ASSERT(cleared != NULL, "config clear JSON failed");
    CHECK_OK();
    ASSERT(strcmp(cleared, "{}") == 0, "empty replace must clear config");
    lance_free_string(cleared);
    ASSERT(!config_has_key(ds, "fixture.key"), "clear must remove prior keys");

    const char *bad = lance_dataset_update_config_json(
        ds, replace_keys, replace_values, 3, 7);
    ASSERT(bad == NULL, "invalid config update mode must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "invalid mode error code");
    const char *message = lance_last_error_message();
    if (message) lance_free_string(message);

    lance_dataset_close(old);
    lance_dataset_close(ds);

    LanceDataset *reopened = lance_dataset_open(uri, NULL, 0);
    ASSERT(reopened != NULL, "reopened config fixture failed");
    ASSERT(!config_has_key(reopened, "a"), "cleared config must persist");
    lance_dataset_close(reopened);
    printf("OK\n");
}

static void test_base_paths_and_add(const char *uri, const char *alternate_uri) {
    printf("  test_base_paths_and_add... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "base path fixture open failed");
    LanceDataset *old = lance_dataset_open(uri, NULL, 0);
    ASSERT(old != NULL, "old base path fixture open failed");
    uint64_t old_version = lance_dataset_version(old);
    CHECK_OK();
    uint64_t before_version = lance_dataset_version(ds);
    CHECK_OK();

    const char *json = lance_dataset_base_paths_json(ds);
    ASSERT(json != NULL, "base path snapshot failed");
    CHECK_OK();
    ASSERT(json_contains(json, "\"0\""), "base ID 0 must be listed");
    ASSERT(json_contains(json, "\"7\""), "alternate base ID must be listed");
    ASSERT(json_contains(json, alternate_uri),
           "alternate base path must be listed");
    ASSERT(json_contains(json, "\"name\":\"alt-root\""),
           "alternate base name must be listed");
    ASSERT(json_contains(json, "\"is_dataset_root\":false"),
           "alternate root flag must be listed");
    lance_free_string(json);

    char auto_path[4096];
    char named_path[4096];
    char none_path[4096];
    char empty_path[4096];
    ASSERT(snprintf(auto_path, sizeof(auto_path), "%s_added_auto", uri) <
               (int)sizeof(auto_path),
           "auto base path fits");
    ASSERT(snprintf(named_path, sizeof(named_path), "%s_added_named", uri) <
               (int)sizeof(named_path),
           "named base path fits");
    ASSERT(snprintf(none_path, sizeof(none_path), "%s_none_props", uri) <
               (int)sizeof(none_path),
           "none base path fits");
    ASSERT(snprintf(empty_path, sizeof(empty_path), "%s_empty_props", uri) <
               (int)sizeof(empty_path),
           "empty base path fits");

    LanceBasePath bases[] = {
        {0, auto_path, "auto", false},
        {42, named_path, "", true},
    };
    const char *property_keys[] = {"fixture.tx"};
    const char *property_values[] = {"add-bases"};
    LanceTransactionProperties properties = {
        property_keys, property_values, 1,
    };
    int32_t rc = lance_dataset_add_bases(ds, bases, 2, &properties);
    ASSERT(rc == 0, "base path add failed");
    CHECK_OK();
    ASSERT(lance_dataset_version(ds) == before_version + 1,
           "base add must commit one version");
    ASSERT(lance_dataset_version(old) == old_version,
           "old handle must retain base paths");

    json = lance_dataset_base_paths_json(ds);
    ASSERT(json != NULL, "post-add base snapshot failed");
    CHECK_OK();
    ASSERT(json_contains(json, "\"id\":1"),
           "zero ID must receive native allocation");
    ASSERT(json_contains(json, "\"id\":42"),
           "explicit base ID must persist");
    ASSERT(json_contains(json, "\"name\":\"auto\""),
           "optional base name must persist");
    ASSERT(json_contains(json, "\"name\":\"\""),
           "explicit empty base name must persist");
    ASSERT(json_contains(json, auto_path), "allocated base path must persist");
    ASSERT(json_contains(json, named_path), "named base path must persist");
    lance_free_string(json);

    LanceBasePath none_bases[] = {{0, none_path, "none-props", false}};
    ASSERT(lance_dataset_add_bases(ds, none_bases, 1, NULL) == 0,
           "None transaction properties must be accepted");
    CHECK_OK();

    LanceBasePath empty_bases[] = {{0, empty_path, "empty-props", false}};
    LanceTransactionProperties empty_properties = {NULL, NULL, 0};
    ASSERT(lance_dataset_add_bases(
               ds, empty_bases, 1, &empty_properties) == 0,
           "explicit empty transaction properties must be accepted");
    CHECK_OK();

    uint64_t before_empty = lance_dataset_version(ds);
    CHECK_OK();
    const char *before_empty_json = lance_dataset_base_paths_json(ds);
    ASSERT(before_empty_json != NULL, "base paths before empty registration");
    ASSERT(lance_dataset_add_bases(ds, NULL, 0, NULL) == 0,
           "empty base registration must use native commit behavior");
    CHECK_OK();
    ASSERT(lance_dataset_add_bases(ds, empty_bases, 0, &empty_properties) == 0,
           "non-NULL empty base array must use native commit behavior");
    CHECK_OK();
    uint64_t stable_version = lance_dataset_version(ds);
    ASSERT(stable_version == before_empty + 2,
           "each empty registration commits one native version");
    const char *after_empty_json = lance_dataset_base_paths_json(ds);
    ASSERT(after_empty_json != NULL, "base paths after empty registration");
    ASSERT(strcmp(before_empty_json, after_empty_json) == 0,
           "empty registration preserves the base registry");
    lance_free_string(before_empty_json);
    lance_free_string(after_empty_json);
    ASSERT(lance_dataset_add_bases(ds, NULL, 1, NULL) == -1,
           "NULL base array must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL base array error code");
    const char *message = lance_last_error_message();
    if (message) lance_free_string(message);

    LanceBasePath missing_path = {3, NULL, NULL, false};
    ASSERT(lance_dataset_add_bases(ds, &missing_path, 1, NULL) == -1,
           "NULL base path must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL base path error code");
    message = lance_last_error_message();
    if (message) lance_free_string(message);

    LanceBasePath duplicate_path = {0, alternate_uri, "duplicate-path", false};
    ASSERT(lance_dataset_add_bases(ds, &duplicate_path, 1, NULL) == -1,
           "duplicate base path must fail natively");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "duplicate base path error code");
    message = lance_last_error_message();
    ASSERT(message != NULL && strstr(message, "Conflict detected") != NULL,
           "duplicate base path must preserve native error");
    if (message) lance_free_string(message);

    char duplicate_name_path[4096];
    ASSERT(snprintf(
               duplicate_name_path,
               sizeof(duplicate_name_path),
               "%s_duplicate_name",
               uri) < (int)sizeof(duplicate_name_path),
           "duplicate name path fits");
    LanceBasePath duplicate_name = {
        0, duplicate_name_path, "alt-root", false,
    };
    ASSERT(lance_dataset_add_bases(ds, &duplicate_name, 1, NULL) == -1,
           "duplicate base name must fail natively");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "duplicate base name error code");
    message = lance_last_error_message();
    ASSERT(message != NULL && strstr(message, "Conflict detected") != NULL,
           "duplicate base name must preserve native error");
    if (message) lance_free_string(message);
    ASSERT(lance_dataset_version(ds) == stable_version,
           "failed registrations must not commit");

    lance_dataset_close(old);
    lance_dataset_close(ds);
    printf("OK\n");
}

static void test_file_enumeration(const char *uri, const char *alternate_uri) {
    printf("  test_file_enumeration... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "file enumeration fixture open failed");
    uint64_t latest = lance_dataset_version(ds);
    CHECK_OK();

    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_dataset_tracked_files_stream(ds, NULL, &stream) == 0,
           "tracked-files stream failed");
    CHECK_OK();

    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(stream.get_schema(&stream, &schema) == 0,
           "tracked-files get_schema failed");
    ASSERT(schema.n_children == 4, "tracked-files field count");
    ASSERT(strcmp(schema.children[0]->name, "version") == 0,
           "tracked version field");
    ASSERT(strcmp(schema.children[1]->name, "base_uri") == 0,
           "tracked base_uri field");
    ASSERT(strcmp(schema.children[2]->name, "path") == 0,
           "tracked path field");
    ASSERT(strcmp(schema.children[3]->name, "type") == 0,
           "tracked type field");
    ASSERT(strcmp(schema.children[0]->format, "l") == 0,
           "tracked version is Int64");
    ASSERT(strcmp(schema.children[1]->format, "i") == 0 &&
               schema.children[1]->dictionary != NULL,
           "tracked base_uri is Int32 dictionary");
    ASSERT(strcmp(schema.children[2]->format, "u") == 0,
           "tracked path is Utf8");
    ASSERT(strcmp(schema.children[3]->format, "c") == 0 &&
               schema.children[3]->dictionary != NULL,
           "tracked type is Int8 dictionary");
    schema.release(&schema);

    int saw_manifest = 0;
    int saw_data = 0;
    int saw_deletion = 0;
    int saw_transaction = 0;
    int saw_index = 0;
    int saw_alt_data = 0;
    int saw_untracked = 0;
    int saw_old_version = 0;
    uint64_t manifest_rows = 0;
    while (1) {
        struct ArrowArray batch;
        memset(&batch, 0, sizeof(batch));
        ASSERT(stream.get_next(&stream, &batch) == 0,
               "tracked-files get_next failed");
        if (batch.release == NULL) break;
        for (int64_t row = 0; row < batch.length; ++row) {
            int64_t version = 0;
            memcpy(
                &version,
                (const char *)batch.children[0]->buffers[1] + row * 8,
                8);
            if (version < (int64_t)latest) saw_old_version = 1;
            if (dictionary_string_equals(batch.children[1], row, 4, alternate_uri)) {
                if (dictionary_string_equals(
                        batch.children[3], row, 1, "data file")) {
                    saw_alt_data = 1;
                }
            }
            if (string_array_equals(batch.children[2], row, "untracked.txt")) {
                saw_untracked = 1;
            }
            if (dictionary_string_equals(
                    batch.children[3], row, 1, "manifest")) {
                saw_manifest = 1;
                manifest_rows += 1;
            } else if (dictionary_string_equals(
                           batch.children[3], row, 1, "data file")) {
                saw_data = 1;
            } else if (dictionary_string_equals(
                           batch.children[3], row, 1, "deletion file")) {
                saw_deletion = 1;
            } else if (dictionary_string_equals(
                           batch.children[3], row, 1, "transaction file")) {
                saw_transaction = 1;
            } else if (dictionary_string_equals(
                           batch.children[3], row, 1, "index file")) {
                saw_index = 1;
            }
        }
        batch.release(&batch);
    }
    if (stream.release) stream.release(&stream);
    ASSERT(saw_manifest && saw_data && saw_deletion && saw_transaction &&
               saw_index,
           "tracked-files must include every native file type");
    ASSERT(saw_alt_data, "tracked-files must resolve alternate base data");
    ASSERT(!saw_untracked, "untracked file must not be tracked");
    ASSERT(saw_old_version, "unfiltered tracked-files must include history");

    LanceTrackedFilesOptions options = {0};
    options.has_min_version = 1;
    options.min_version = latest;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_dataset_tracked_files_stream(ds, &options, &stream) == 0,
           "tracked-files min-version stream failed");
    CHECK_OK();
    uint64_t min_rows = count_stream_rows(&stream);
    ASSERT(min_rows > 0, "min-version stream must include latest manifest");

    TrackedProgressCapture capture;
    TrackedProgressContext *context = new_tracked_progress(&capture);
    LanceTrackedFilesProgress progress = {
        capture_tracked_progress,
        release_tracked_progress,
        context,
    };
    memset(&options, 0, sizeof(options));
    options.progress = &progress;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_dataset_tracked_files_stream(ds, &options, &stream) == 0,
           "tracked-files progress stream failed");
    CHECK_OK();
    lance_dataset_close(ds);
    uint64_t all_tracked_rows = count_stream_rows(&stream);
    ASSERT(all_tracked_rows > 0, "stream must outlive dataset close");
    wait_released(&capture.released);
    ASSERT(atomic_load(&capture.released) == 1,
           "progress context must be released once");
    ASSERT(manifest_rows > 0, "tracked output must include manifests");
    ASSERT(atomic_load(&capture.calls) == (int)manifest_rows,
           "each manifest must report progress");
    ASSERT(atomic_load(&capture.last_processed) == manifest_rows,
           "last progress must process every manifest");
    ASSERT(atomic_load(&capture.has_total) == 1 &&
               atomic_load(&capture.last_total) == manifest_rows,
           "final progress must report the native total");

    ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "early-close fixture reopen failed");
    TrackedProgressCapture early_capture;
    TrackedProgressContext *early_context = new_tracked_progress(&early_capture);
    LanceTrackedFilesProgress early_progress = {
        capture_tracked_progress,
        release_tracked_progress,
        early_context,
    };
    memset(&options, 0, sizeof(options));
    options.progress = &early_progress;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_dataset_tracked_files_stream(ds, &options, &stream) == 0,
           "early-close progress stream failed");
    CHECK_OK();
    if (stream.release) stream.release(&stream);
    wait_released(&early_capture.released);
    ASSERT(atomic_load(&early_capture.released) == 1,
           "early close must release progress context once");

    TrackedProgressCapture factory_capture;
    TrackedProgressContext *factory_context =
        new_tracked_progress(&factory_capture);
    LanceTrackedFilesProgress factory_progress = {
        capture_tracked_progress,
        release_tracked_progress,
        factory_context,
    };
    memset(&options, 0, sizeof(options));
    options.progress = &factory_progress;
    ASSERT(lance_dataset_tracked_files_stream(ds, &options, NULL) == -1,
           "NULL output must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL output error code");
    const char *message = lance_last_error_message();
    if (message) lance_free_string(message);
    wait_released(&factory_capture.released);
    ASSERT(atomic_load(&factory_capture.released) == 1,
           "factory error must release progress context once");
    lance_dataset_close(ds);

    ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "all-files fixture reopen failed");
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_dataset_all_files_stream(ds, &stream) == 0,
           "all-files stream failed");
    CHECK_OK();
    memset(&schema, 0, sizeof(schema));
    ASSERT(stream.get_schema(&stream, &schema) == 0,
           "all-files get_schema failed");
    ASSERT(schema.n_children == 4, "all-files field count");
    ASSERT(strcmp(schema.children[0]->name, "base_uri") == 0 &&
               schema.children[0]->dictionary != NULL,
           "all-files base_uri schema");
    ASSERT(strcmp(schema.children[1]->name, "path") == 0,
           "all-files path schema");
    ASSERT(strcmp(schema.children[2]->name, "size_bytes") == 0 &&
               strcmp(schema.children[2]->format, "l") == 0,
           "all-files size schema");
    ASSERT(strcmp(schema.children[3]->name, "last_modified") == 0 &&
               strncmp(schema.children[3]->format, "tsu:", 4) == 0,
           "all-files timestamp schema");
    schema.release(&schema);
    lance_dataset_close(ds);

    int saw_untracked_physical = 0;
    uint64_t physical_rows = 0;
    while (1) {
        struct ArrowArray batch;
        memset(&batch, 0, sizeof(batch));
        ASSERT(stream.get_next(&stream, &batch) == 0,
               "all-files get_next failed");
        if (batch.release == NULL) break;
        for (int64_t row = 0; row < batch.length; ++row) {
            physical_rows += 1;
            ASSERT(dictionary_string_equals(
                       batch.children[0], row, 4, uri),
                   "all-files must use the primary base URI");
            if (string_array_equals(batch.children[1], row, "untracked.txt")) {
                int64_t size = 0;
                int64_t modified = 0;
                memcpy(
                    &size,
                    (const char *)batch.children[2]->buffers[1] + row * 8,
                    8);
                memcpy(
                    &modified,
                    (const char *)batch.children[3]->buffers[1] + row * 8,
                    8);
                ASSERT(size == 23, "untracked file size");
                ASSERT(modified > 0, "physical timestamp must be populated");
                saw_untracked_physical = 1;
            }
        }
        batch.release(&batch);
    }
    if (stream.release) stream.release(&stream);
    ASSERT(physical_rows > 0, "all-files must list physical files");
    ASSERT(saw_untracked_physical,
           "all-files must include controlled untracked file");
    printf("tracked=%llu, physical=%llu... OK\n",
           (unsigned long long)all_tracked_rows,
           (unsigned long long)physical_rows);
}

/* Exercises native manual cleanup, explanation snapshots, validation and
 * lifecycle statistics on the dedicated five-version fixture. */
static void test_cleanup_and_lifecycle(const char *uri) {
    printf("  test_cleanup_and_lifecycle... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "cleanup fixture open failed");
    ASSERT(lance_dataset_version(ds) == 5, "cleanup fixture version mismatch");
    ASSERT(lance_dataset_validate(ds) == 0, "valid fixture failed validation");
    CHECK_OK();

    uint64_t deleted = UINT64_MAX;
    ASSERT(lance_dataset_count_deleted_rows(ds, &deleted) == 0,
           "deleted-row count failed");
    CHECK_OK();
    ASSERT(deleted == 1, "cleanup fixture deleted-row count mismatch");

    uint64_t small = UINT64_MAX;
    ASSERT(lance_dataset_num_small_files(ds, 0, &small) == 0,
           "zero small-file threshold failed");
    ASSERT(small == 0, "native zero threshold must return zero");
    ASSERT(lance_dataset_num_small_files(ds, 5, &small) == 0,
           "small-file count failed");
    ASSERT(small == 4, "small-file count mismatch");
    ASSERT(lance_dataset_num_small_files(ds, 4, &small) == 0,
           "strict small-file boundary failed");
    ASSERT(small == 0, "physical rows equal to threshold are not small");

    const uint64_t selected[] = {2, 3};
    LanceCleanupPolicy policy = {0};
    policy.has_before_version = 1;
    policy.before_version = 5;
    policy.versions = selected;
    policy.num_versions = 2;
    LanceCleanupOperationOptions operation = {0};
    operation.has_max_candidate_files = 1;
    operation.max_candidate_files = 1;

    LanceCleanupExplanation *explanation =
        lance_dataset_cleanup_explain(ds, &policy, &operation);
    ASSERT(explanation != NULL, "cleanup explanation failed");
    CHECK_OK();
    ASSERT(lance_cleanup_explanation_read_version(explanation) == 5,
           "explanation read_version mismatch");
    CHECK_OK();
    LanceRemovalStats stats = {0};
    ASSERT(lance_cleanup_explanation_stats(explanation, &stats) == 0,
           "explanation stats failed");
    CHECK_OK();
    ASSERT(stats.old_versions == 2, "explanation old_versions mismatch");
    ASSERT(lance_cleanup_explanation_candidate_count(explanation) == 1,
           "candidate limit should include one file");
    CHECK_OK();
    ASSERT(lance_cleanup_explanation_candidate_files_truncated(explanation) == 1,
           "candidate list should report truncation");
    ASSERT(lance_cleanup_explanation_candidate_file_limit(explanation) == 1,
           "candidate limit mismatch");
    const char *candidate_path =
        lance_cleanup_explanation_candidate_path_at(explanation, 0);
    ASSERT(candidate_path != NULL && candidate_path[0] != '\0',
           "candidate path must be copied");
    int32_t candidate_kind =
        lance_cleanup_explanation_candidate_kind_at(explanation, 0);
    ASSERT(candidate_kind >= LANCE_CLEANUP_FILE_MANIFEST &&
               candidate_kind <= LANCE_CLEANUP_FILE_TEMPORARY_MANIFEST,
           "candidate kind out of range");
    int32_t candidate_unverified =
        lance_cleanup_explanation_candidate_unverified_at(explanation, 0);
    ASSERT(candidate_unverified >= 0, "candidate unverified flag failed");
    ASSERT(lance_cleanup_explanation_candidate_size_bytes_at(explanation, 0) > 0,
           "candidate size must be copied");
    CHECK_OK();
    ASSERT(lance_cleanup_explanation_warning_count(explanation) == 1,
           "truncation warning missing");
    CHECK_OK();
    const char *warning = lance_cleanup_explanation_warning_at(explanation, 0);
    ASSERT(warning != NULL && strstr(warning, "truncated to 1 entries") != NULL,
           "unexpected truncation warning");

    /* Explanation is read-only and owns its data after the dataset closes. */
    LanceDataset *old_version = lance_dataset_open(uri, NULL, 1);
    ASSERT(old_version != NULL, "explain must not delete old versions");
    lance_dataset_close(old_version);
    lance_dataset_close(ds);
    ASSERT(lance_cleanup_explanation_read_version(explanation) == 5,
           "owned explanation changed after dataset close");
    CHECK_OK();
    lance_cleanup_explanation_close(explanation);

    ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "cleanup fixture reopen failed");

    /* Explicit zero includes no candidate files but keeps full aggregate stats. */
    memset(&policy, 0, sizeof(policy));
    policy.has_retain_versions = 1;
    policy.retain_versions = 3;
    operation.has_max_candidate_files = 1;
    operation.max_candidate_files = 0;
    explanation = lance_dataset_cleanup_explain(ds, &policy, &operation);
    ASSERT(explanation != NULL, "zero-limit explanation failed");
    CHECK_OK();
    ASSERT(lance_cleanup_explanation_candidate_count(explanation) == 0,
           "zero candidate limit must include no files");
    CHECK_OK();
    ASSERT(lance_cleanup_explanation_candidate_files_truncated(explanation) == 1,
           "zero candidate limit must report truncation");
    ASSERT(lance_cleanup_explanation_candidate_file_limit(explanation) == 0,
           "zero candidate limit mismatch");
    memset(&stats, 0, sizeof(stats));
    ASSERT(lance_cleanup_explanation_stats(explanation, &stats) == 0,
           "zero-limit explanation stats failed");
    ASSERT(stats.old_versions == 2,
           "zero candidate limit must retain aggregate stats");
    lance_cleanup_explanation_close(explanation);

    /* Explicit empty version selection reaches the native builder error. */
    uint64_t empty_version = 0;
    memset(&policy, 0, sizeof(policy));
    policy.versions = &empty_version;
    policy.num_versions = 0;
    ASSERT(lance_dataset_cleanup_explain(ds, &policy, NULL) == NULL,
           "empty version selection must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "empty version selection must be INVALID_ARGUMENT");
    const char *message = lance_last_error_message();
    ASSERT(message != NULL &&
               strstr(message, "versions must not be empty when specified") != NULL,
           "empty version error missing native context");
    if (message) lance_free_string(message);

    LanceCleanupOldVersionsOptions invalid_relative = {0};
    invalid_relative.older_than_nanos = 1000000000;
    LanceRemovalStats sentinel = {
        UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX,
        UINT64_MAX, UINT64_MAX, UINT64_MAX};
    stats = sentinel;
    ASSERT(lance_dataset_cleanup_old_versions(
               ds, &invalid_relative, &stats) == -1,
           "invalid nanosecond duration must fail");
    ASSERT(memcmp(&stats, &sentinel, sizeof(stats)) == 0,
           "failed relative cleanup must not write stats");
    message = lance_last_error_message();
    if (message) lance_free_string(message);

    /* Native operation overrides are forwarded. */
    uint64_t version_two[] = {2};
    memset(&policy, 0, sizeof(policy));
    policy.versions = version_two;
    policy.num_versions = 1;
    memset(&operation, 0, sizeof(operation));
    operation.has_delete_concurrency = 1;
    operation.delete_concurrency = 2;
    operation.has_delete_rate_limit = 1;
    operation.delete_rate_limit = 1000000;
    memset(&stats, 0, sizeof(stats));
    ASSERT(lance_dataset_cleanup(ds, &policy, &operation, &stats) == 0,
           "manual cleanup failed");
    CHECK_OK();
    ASSERT(stats.old_versions == 1, "manual cleanup old_versions mismatch");
    ASSERT(stats.failed_deletes == 0, "unexpected failed deletes");
    lance_dataset_close(ds);

    old_version = lance_dataset_open(uri, NULL, 2);
    ASSERT(old_version == NULL, "removed version must no longer open");
    ASSERT(lance_last_error_code() == LANCE_ERR_NOT_FOUND,
           "removed version should be NOT_FOUND");
    message = lance_last_error_message();
    if (message) lance_free_string(message);

    ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "cleanup fixture reopen after delete failed");
    LanceCleanupOldVersionsOptions relative = {0};
    memset(&stats, 0, sizeof(stats));
    ASSERT(lance_dataset_cleanup_old_versions(ds, &relative, &stats) == 0,
           "relative cleanup failed");
    CHECK_OK();
    ASSERT(stats.old_versions == 3,
           "relative cleanup should remove the three remaining old versions");

    /* Binding validation and success-only outputs. */
    uint64_t untouched = 123;
    ASSERT(lance_dataset_count_deleted_rows(NULL, &untouched) == -1,
           "NULL count_deleted_rows dataset must fail");
    ASSERT(untouched == 123, "failed deleted-row count wrote output");
    ASSERT(lance_dataset_num_small_files(NULL, 5, &untouched) == -1,
           "NULL num_small_files dataset must fail");
    ASSERT(untouched == 123, "failed small-file count wrote output");
    ASSERT(lance_dataset_validate(NULL) == -1,
           "NULL validate dataset must fail");
    ASSERT(lance_dataset_cleanup_explain(NULL, &policy, NULL) == NULL,
           "NULL explanation dataset must fail");
    ASSERT(lance_dataset_cleanup_old_versions(ds, NULL, &stats) == -1,
           "NULL relative cleanup options must fail");
    discard_last_error();
    ASSERT(lance_cleanup_explanation_read_version(NULL) == 0,
           "NULL explanation accessor must fail");
    ASSERT(lance_cleanup_explanation_candidate_count(NULL) == 0,
           "NULL explanation count must fail");
    ASSERT(lance_cleanup_explanation_candidate_files_truncated(NULL) == -1,
           "NULL explanation truncation must fail");
    discard_last_error();
    lance_dataset_close(ds);
    lance_cleanup_explanation_close(NULL);

    printf("OK\n");
}

/* Exercises native table/schema metadata reads and updates through the C API,
 * including removal, empty values, replace/clear and reopen persistence. */
static void test_dataset_metadata(const char *write_uri) {
    printf("  test_dataset_metadata... ");

    LanceDataset *ds = lance_dataset_open(write_uri, NULL, 0);
    ASSERT(ds != NULL, "metadata test dataset open failed");

    uint64_t version_before = lance_dataset_version(ds);
    CHECK_OK();

    const char *initial = lance_dataset_metadata_json(ds);
    ASSERT(initial != NULL, "initial metadata JSON must not be NULL");
    ASSERT(strcmp(initial, "{}") == 0, "newly written dataset should have empty table metadata");
    lance_free_string(initial);

    const char *keys[] = {"owner", "empty", "remove"};
    const char *values[] = {"c-consumer", "", "temporary"};
    const char *updated = lance_dataset_update_metadata_json(
        ds, keys, values, 3, LANCE_METADATA_UPDATE_MERGE);
    ASSERT(updated != NULL, "table metadata merge failed");
    ASSERT(strstr(updated, "\"owner\":\"c-consumer\"") != NULL,
           "merged owner missing");
    ASSERT(strstr(updated, "\"empty\":\"\"") != NULL,
           "empty string must remain a real value");
    lance_free_string(updated);

    const char *delete_keys[] = {"remove"};
    const char *delete_values[] = {NULL};
    updated = lance_dataset_update_metadata_json(
        ds, delete_keys, delete_values, 1, LANCE_METADATA_UPDATE_MERGE);
    ASSERT(updated != NULL, "table metadata removal failed");
    ASSERT(strstr(updated, "\"remove\"") == NULL,
           "NULL value must remove the key");
    lance_free_string(updated);

    const char *schema_keys[] = {"schema-owner", "shared"};
    const char *schema_values[] = {"c-consumer", "schema"};
    const char *schema_updated = lance_dataset_update_schema_metadata_json(
        ds, schema_keys, schema_values, 2, LANCE_METADATA_UPDATE_MERGE);
    ASSERT(schema_updated != NULL, "schema metadata merge failed");
    ASSERT(strstr(schema_updated, "\"schema-owner\":\"c-consumer\"") != NULL,
           "updated schema metadata missing");
    lance_free_string(schema_updated);

    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(lance_dataset_schema(ds, &schema) == 0, "schema export failed");
    ASSERT(schema.metadata != NULL, "Arrow schema must expose schema metadata");
    if (schema.release) schema.release(&schema);

    const char *table = lance_dataset_metadata_json(ds);
    ASSERT(table != NULL, "table metadata read failed");
    ASSERT(strstr(table, "\"schema-owner\"") == NULL,
           "schema metadata update must not change table metadata");
    lance_free_string(table);

    const char *cleared = lance_dataset_update_metadata_json(
        ds, NULL, NULL, 0, LANCE_METADATA_UPDATE_REPLACE);
    ASSERT(cleared != NULL, "table metadata clear failed");
    ASSERT(strcmp(cleared, "{}") == 0,
           "empty replacement must clear the table metadata map");
    lance_free_string(cleared);

    const char *persist_keys[] = {"persist"};
    const char *persist_values[] = {"yes"};
    updated = lance_dataset_update_metadata_json(
        ds, persist_keys, persist_values, 1, LANCE_METADATA_UPDATE_MERGE);
    ASSERT(updated != NULL, "persistent table metadata update failed");
    lance_free_string(updated);

    schema_updated = lance_dataset_update_schema_metadata_json(
        ds, NULL, NULL, 0, LANCE_METADATA_UPDATE_REPLACE);
    ASSERT(schema_updated != NULL, "schema metadata clear failed");
    ASSERT(strcmp(schema_updated, "{}") == 0,
           "empty replacement must clear schema metadata");
    lance_free_string(schema_updated);
    ASSERT(lance_dataset_version(ds) > version_before,
           "metadata updates must advance the dataset version");

    /* Invalid inputs fail without changing the committed metadata. */
    ASSERT(lance_dataset_metadata_json(NULL) == NULL,
           "NULL dataset metadata read must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL dataset metadata read must return INVALID_ARGUMENT");
    ASSERT(lance_dataset_update_metadata_json(
               ds, NULL, persist_values, 1, LANCE_METADATA_UPDATE_MERGE) == NULL,
           "NULL keys must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL keys must return INVALID_ARGUMENT");
    ASSERT(lance_dataset_update_metadata_json(
               ds, persist_keys, NULL, 1, LANCE_METADATA_UPDATE_MERGE) == NULL,
           "NULL values array must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL values array must return INVALID_ARGUMENT");
    ASSERT(lance_dataset_update_metadata_json(
               ds, persist_keys, persist_values, 1,
               (LanceMetadataUpdateMode)99) == NULL,
           "invalid update mode must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "invalid update mode must return INVALID_ARGUMENT");

    lance_dataset_close(ds);

    ds = lance_dataset_open(write_uri, NULL, 0);
    ASSERT(ds != NULL, "metadata test reopen failed");
    const char *reopened = lance_dataset_metadata_json(ds);
    ASSERT(reopened != NULL, "reopened metadata read failed");
    ASSERT(strstr(reopened, "\"persist\":\"yes\"") != NULL,
           "table metadata must persist across reopen");
    lance_free_string(reopened);
    lance_dataset_close(ds);

    printf("OK\n");
}

/* Exercises batched native field metadata updates plus schema/snapshot
 * inspection through the C ABI. */
static void test_dataset_field_metadata(const char *fragment_uri) {
    printf("  test_dataset_field_metadata... ");

    LanceDataset *ds = lance_dataset_open(fragment_uri, NULL, 0);
    LanceDataset *old = lance_dataset_open(fragment_uri, NULL, 0);
    ASSERT(ds != NULL && old != NULL, "field metadata dataset open failed");

    uint64_t initial_version = lance_dataset_version(ds);
    CHECK_OK();
    ASSERT(lance_dataset_fragment_count(ds) == 2,
           "nested fixture must have two fragments");
    ASSERT(lance_dataset_count_rows(ds) == 6,
           "nested fixture must retain its logical rows");

    const char *storage_version = lance_dataset_data_storage_version(ds);
    ASSERT(storage_version != NULL, "data storage version read failed");
    ASSERT(strcmp(storage_version, "2.2") == 0,
           "fragment fixture should use native 2.2 storage");
    lance_free_string(storage_version);

    bool stable = true;
    ASSERT(lance_dataset_has_stable_row_ids(ds, &stable) == 0,
           "stable row ID read failed");
    ASSERT(!stable, "fragment fixture must not use stable row IDs");

    size_t field_count = 0;
    ASSERT(lance_dataset_field_id_count(ds, &field_count) == 0,
           "field ID count failed");
    ASSERT(field_count >= 4, "nested fixture should expose four fields");

    LanceFieldReference id_path = {LANCE_FIELD_REFERENCE_PATH, 0, "id"};
    LanceFieldReference name_path = {LANCE_FIELD_REFERENCE_PATH, 0, "profile.name"};
    LanceFieldReference score_path = {LANCE_FIELD_REFERENCE_PATH, 0, "profile.score"};
    int32_t id_id = 0;
    int32_t name_id = 0;
    int32_t score_id = 0;
    ASSERT(lance_dataset_resolve_field_id(ds, &id_path, &id_id) == 0,
           "resolve id path failed");
    ASSERT(lance_dataset_resolve_field_id(ds, &name_path, &name_id) == 0,
           "resolve nested name path failed");
    ASSERT(lance_dataset_resolve_field_id(ds, &score_path, &score_id) == 0,
           "resolve nested score path failed");

    const char *nested_path = lance_dataset_field_path(ds, name_id);
    ASSERT(nested_path != NULL, "nested field path read failed");
    ASSERT(strcmp(nested_path, "profile.name") == 0,
           "nested field path mismatch");
    lance_free_string(nested_path);

    int32_t listed_nested_id = 0;
    ASSERT(lance_dataset_field_id_at(ds, field_count - 1, &listed_nested_id) == 0,
           "field ID traversal failed");
    const char *traversed_path = lance_dataset_field_path(ds, listed_nested_id);
    ASSERT(traversed_path != NULL, "traversed field path failed");
    lance_free_string(traversed_path);

    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(lance_dataset_schema(ds, &schema) == 0, "initial schema export failed");
    const struct ArrowSchema *profile_name =
        arrow_field_at_path(&schema, "profile.name");
    ASSERT(profile_name != NULL, "nested Arrow field missing");
    ASSERT(arrow_field_metadata_value(profile_name, "nested", NULL) == NULL,
           "initial field metadata should be empty");
    if (schema.release) schema.release(&schema);

    /* Invalid inputs and native field lookup errors must not advance the
     * version or change committed metadata. */
    const char *bad_key[] = {"key"};
    const char *bad_value[] = {"value"};
    LanceFieldMetadataUpdate bad_updates[] = {
        {{LANCE_FIELD_REFERENCE_PATH, 0, "id"}, bad_key, bad_value, 1,
         LANCE_METADATA_UPDATE_MERGE},
    };
    ASSERT(lance_dataset_update_field_metadata(NULL, bad_updates, 1) == -1,
           "NULL dataset update must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL dataset update must return INVALID_ARGUMENT");
    ASSERT(lance_dataset_update_field_metadata(ds, NULL, 1) == -1,
           "NULL updates must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL updates must return INVALID_ARGUMENT");
    bad_updates[0].keys = NULL;
    ASSERT(lance_dataset_update_field_metadata(ds, bad_updates, 1) == -1,
           "NULL keys must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL keys must return INVALID_ARGUMENT");
    bad_updates[0].keys = bad_key;
    bad_updates[0].mode = (LanceMetadataUpdateMode)99;
    ASSERT(lance_dataset_update_field_metadata(ds, bad_updates, 1) == -1,
           "invalid field update mode must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "invalid field update mode must return INVALID_ARGUMENT");
    bad_updates[0].mode = LANCE_METADATA_UPDATE_MERGE;
    bad_updates[0].field.field_id = 99999;
    bad_updates[0].field.kind = LANCE_FIELD_REFERENCE_ID;
    ASSERT(lance_dataset_update_field_metadata(ds, bad_updates, 1) == -1,
           "unknown field ID must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "unknown field ID must return INVALID_ARGUMENT");
    bad_updates[0].field.kind = LANCE_FIELD_REFERENCE_PATH;
    bad_updates[0].field.path = "profile.missing";
    ASSERT(lance_dataset_update_field_metadata(ds, bad_updates, 1) == -1,
           "unknown field path must fail");
    ASSERT(lance_last_error_code() != LANCE_OK,
           "unknown field path must set an error");
    const char *path_error = lance_last_error_message();
    ASSERT(path_error != NULL && strstr(path_error, "profile.missing") != NULL,
           "unknown field path error must identify the path");
    if (path_error) lance_free_string(path_error);
    ASSERT(lance_dataset_version(ds) == initial_version,
           "rejected field updates must not commit");

    const char *id_keys[] = {"owner", "empty", "remove"};
    const char *id_values[] = {"c-consumer", "", "temporary"};
    const char *name_keys[] = {"nested", "empty"};
    const char *name_values[] = {"value", ""};
    const char *score_keys[] = {"score", "remove"};
    const char *score_values[] = {"1.5", NULL};
    LanceFieldMetadataUpdate updates[] = {
        {{LANCE_FIELD_REFERENCE_ID, id_id, NULL}, id_keys, id_values, 3,
         LANCE_METADATA_UPDATE_MERGE},
        {{LANCE_FIELD_REFERENCE_PATH, 0, "profile.name"}, name_keys, name_values, 2,
         LANCE_METADATA_UPDATE_REPLACE},
        {{LANCE_FIELD_REFERENCE_ID, score_id, NULL}, score_keys, score_values, 2,
         LANCE_METADATA_UPDATE_MERGE},
    };
    uint64_t version_before = lance_dataset_version(ds);
    ASSERT(lance_dataset_update_field_metadata(ds, updates, 3) == 0,
           "batched field update failed");
    CHECK_OK();
    ASSERT(lance_dataset_version(ds) == version_before + 1,
           "batched field updates must share one commit");

    memset(&schema, 0, sizeof(schema));
    ASSERT(lance_dataset_schema(ds, &schema) == 0, "updated schema export failed");
    const struct ArrowSchema *id_field = arrow_field_at_path(&schema, "id");
    profile_name = arrow_field_at_path(&schema, "profile.name");
    const struct ArrowSchema *score_field =
        arrow_field_at_path(&schema, "profile.score");
    ASSERT(id_field != NULL && profile_name != NULL && score_field != NULL,
           "updated Arrow fields missing");
    assert_field_metadata(id_field, "owner", "c-consumer");
    assert_field_metadata(id_field, "empty", "");
    assert_field_metadata(id_field, "remove", "temporary");
    assert_field_metadata(profile_name, "nested", "value");
    assert_field_metadata(profile_name, "empty", "");
    assert_field_metadata(score_field, "score", "1.5");
    assert_field_metadata_absent(score_field, "remove");
    if (schema.release) schema.release(&schema);

    /* The native builder stores updates by resolved field ID; the last update
     * for `id` wins even though it is reached through another reference form. */
    const char *root_keys[] = {"root"};
    const char *first_values[] = {"first"};
    const char *last_values[] = {"last"};
    LanceFieldMetadataUpdate duplicate_updates[] = {
        {{LANCE_FIELD_REFERENCE_ID, id_id, NULL}, root_keys, first_values, 1,
         LANCE_METADATA_UPDATE_MERGE},
        {{LANCE_FIELD_REFERENCE_PATH, 0, "id"}, root_keys, last_values, 1,
         LANCE_METADATA_UPDATE_REPLACE},
    };
    version_before = lance_dataset_version(ds);
    ASSERT(lance_dataset_update_field_metadata(ds, duplicate_updates, 2) == 0,
           "duplicate field update failed");
    ASSERT(lance_dataset_version(ds) == version_before + 1,
           "duplicate field updates must share one commit");
    memset(&schema, 0, sizeof(schema));
    ASSERT(lance_dataset_schema(ds, &schema) == 0,
           "duplicate update schema export failed");
    id_field = arrow_field_at_path(&schema, "id");
    ASSERT(id_field != NULL, "duplicate update id field missing");
    assert_field_metadata(id_field, "root", "last");
    assert_field_metadata_absent(id_field, "owner");
    if (schema.release) schema.release(&schema);

    const char *temp_keys[] = {"remove", "empty"};
    const char *temp_values[] = {"temporary", ""};
    const char *remove_keys[] = {"remove"};
    const char *remove_values[] = {NULL};
    LanceFieldMetadataUpdate temp_update[] = {
        {{LANCE_FIELD_REFERENCE_PATH, 0, "id"}, temp_keys, temp_values, 2,
         LANCE_METADATA_UPDATE_MERGE},
    };
    LanceFieldMetadataUpdate remove_update[] = {
        {{LANCE_FIELD_REFERENCE_ID, id_id, NULL}, remove_keys, remove_values, 1,
         LANCE_METADATA_UPDATE_MERGE},
    };
    ASSERT(lance_dataset_update_field_metadata(ds, temp_update, 1) == 0,
           "temporary field metadata update failed");
    ASSERT(lance_dataset_update_field_metadata(ds, remove_update, 1) == 0,
           "field metadata removal failed");
    memset(&schema, 0, sizeof(schema));
    ASSERT(lance_dataset_schema(ds, &schema) == 0,
           "removal schema export failed");
    id_field = arrow_field_at_path(&schema, "id");
    ASSERT(id_field != NULL, "removal id field missing");
    assert_field_metadata(id_field, "root", "last");
    assert_field_metadata(id_field, "empty", "");
    assert_field_metadata_absent(id_field, "remove");
    if (schema.release) schema.release(&schema);

    LanceFieldMetadataUpdate clear_update[] = {
        {{LANCE_FIELD_REFERENCE_PATH, 0, "id"}, NULL, NULL, 0,
         LANCE_METADATA_UPDATE_REPLACE},
    };
    ASSERT(lance_dataset_update_field_metadata(ds, clear_update, 1) == 0,
           "empty field replacement failed");
    memset(&schema, 0, sizeof(schema));
    ASSERT(lance_dataset_schema(ds, &schema) == 0, "clear schema export failed");
    id_field = arrow_field_at_path(&schema, "id");
    ASSERT(id_field != NULL, "clear id field missing");
    ASSERT(arrow_field_metadata_value(id_field, "root", NULL) == NULL,
           "empty replacement must clear field metadata");
    if (schema.release) schema.release(&schema);

    ASSERT(lance_dataset_update_field_metadata(ds, NULL, 0) == 0,
           "empty field update batch must succeed");
    CHECK_OK();

    const char *persist_keys[] = {"persist"};
    const char *persist_values[] = {"yes"};
    LanceFieldMetadataUpdate persist_updates[] = {
        {{LANCE_FIELD_REFERENCE_ID, id_id, NULL}, persist_keys, persist_values, 1,
         LANCE_METADATA_UPDATE_MERGE},
        {{LANCE_FIELD_REFERENCE_PATH, 0, "profile.name"}, persist_keys,
         persist_values, 1, LANCE_METADATA_UPDATE_MERGE},
    };
    version_before = lance_dataset_version(ds);
    ASSERT(lance_dataset_update_field_metadata(ds, persist_updates, 2) == 0,
           "persistent field metadata update failed");
    ASSERT(lance_dataset_version(ds) == version_before + 1,
           "persistent field updates must share one commit");

    memset(&schema, 0, sizeof(schema));
    ASSERT(lance_dataset_schema(old, &schema) == 0,
           "old snapshot schema export failed");
    id_field = arrow_field_at_path(&schema, "id");
    ASSERT(id_field != NULL, "old snapshot id field missing");
    ASSERT(arrow_field_metadata_value(id_field, "persist", NULL) == NULL,
           "independent handle must keep its prior snapshot");
    if (schema.release) schema.release(&schema);
    ASSERT(lance_dataset_version(old) == initial_version,
           "independent handle version must not advance");

    lance_dataset_close(old);
    lance_dataset_close(ds);

    ds = lance_dataset_open(fragment_uri, NULL, 0);
    ASSERT(ds != NULL, "field metadata reopen failed");
    memset(&schema, 0, sizeof(schema));
    ASSERT(lance_dataset_schema(ds, &schema) == 0,
           "reopened schema export failed");
    id_field = arrow_field_at_path(&schema, "id");
    profile_name = arrow_field_at_path(&schema, "profile.name");
    ASSERT(id_field != NULL && profile_name != NULL,
           "reopened fields missing");
    assert_field_metadata(id_field, "persist", "yes");
    assert_field_metadata(profile_name, "persist", "yes");
    if (schema.release) schema.release(&schema);
    lance_dataset_close(ds);

    printf("OK\n");
}

/* Re-opens the dataset just written by `test_dataset_write_roundtrip` and
 * exercises `lance_dataset_update`. Must run before `test_delete`, which
 * empties the dataset. */
static void test_update(const char *write_uri) {
    printf("  test_update... ");

    LanceDataset *ds = lance_dataset_open(write_uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");

    uint64_t before = lance_dataset_count_rows(ds);
    CHECK_OK();
    ASSERT(before > 0, "fixture expected to have rows");

    /* Set every row's `name` column to a literal (NULL predicate -> all rows). */
    const char *cols[] = {"name"};
    const char *vals[] = {"'frozen'"};
    uint64_t updated = 0;
    int32_t rc = lance_dataset_update(ds, NULL, cols, vals, 1, &updated);
    ASSERT(rc == 0, "update failed");
    ASSERT(updated == before, "updated count mismatch");
    ASSERT(lance_dataset_count_rows(ds) == before, "row count must be unchanged");

    /* num_updates == 0 must be rejected. */
    rc = lance_dataset_update(ds, NULL, NULL, NULL, 0, NULL);
    ASSERT(rc == -1, "num_updates=0 must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    lance_dataset_close(ds);
    printf("updated=%llu... OK\n", (unsigned long long)updated);
}

/* Re-opens the dataset just written by `test_dataset_write_roundtrip` and
 * exercises `lance_dataset_merge_insert`. Must run before `test_delete`,
 * which empties the dataset. The source comes from scanning the dataset
 * itself, so under find-or-create defaults every row is a self-match
 * (DoNothing) and nothing changes — this validates the FFI plumbing without
 * needing to hand-build an Arrow batch in pure C. */
static void test_merge_insert(const char *write_uri) {
    printf("  test_merge_insert... ");

    LanceDataset *ds = lance_dataset_open(write_uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");
    uint64_t before = lance_dataset_count_rows(ds);
    CHECK_OK();
    ASSERT(before > 0, "fixture expected to have rows");

    /* Build a self-source via the scanner. */
    LanceScanner *scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "scanner creation failed");

    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    int32_t rc = lance_scanner_to_arrow_stream(scanner, &stream);
    ASSERT(rc == 0, "to_arrow_stream failed");

    const char *on_cols[] = {"id"};
    LanceMergeInsertResult result;
    memset(&result, 0, sizeof(result));
    rc = lance_dataset_merge_insert(ds, on_cols, 1, &stream, NULL, &result);
    ASSERT(rc == 0, "merge_insert failed");
    /* Self-match under DoNothing: nothing inserted, nothing updated. */
    ASSERT(result.num_inserted_rows == 0, "expected 0 inserts");
    ASSERT(result.num_updated_rows == 0, "expected 0 updates");
    ASSERT(lance_dataset_count_rows(ds) == before, "row count must be unchanged");

    /* num_on_columns == 0 must be rejected. */
    rc = lance_dataset_merge_insert(ds, NULL, 0, NULL, NULL, NULL);
    ASSERT(rc == -1, "num_on_columns=0 must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    lance_scanner_close(scanner);
    lance_dataset_close(ds);
    printf("OK\n");
}

/* Re-opens the dataset just written by `test_dataset_write_roundtrip` and
 * exercises `lance_dataset_alter_columns` by relaxing the nullability of the
 * `id` column (non-nullable in the fixture) to nullable. Must run before
 * `test_drop_columns` removes `name`, but the alteration itself only touches
 * `id`, so the column survives the subsequent drop. */
static void test_alter_columns(const char *write_uri) {
    printf("  test_alter_columns... ");

    LanceDataset *ds = lance_dataset_open(write_uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");
    uint64_t v_before = lance_dataset_version(ds);

    LanceColumnAlteration alt = {0};
    alt.path          = "id";
    alt.nullable_mode = LANCE_COLUMN_NULLABLE_TRUE;
    int32_t rc = lance_dataset_alter_columns(ds, &alt, 1);
    ASSERT(rc == 0, "alter_columns failed");
    ASSERT(lance_dataset_version(ds) > v_before,
           "alter_columns must bump the version");

    /* Schema export to confirm `id` is now nullable. */
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    rc = lance_dataset_schema(ds, &schema);
    ASSERT(rc == 0, "schema export failed");
    ASSERT(schema.n_children > 0, "schema must have children");
    int found_nullable_id = 0;
    for (int64_t i = 0; i < schema.n_children; i++) {
        struct ArrowSchema *child = schema.children[i];
        if (!child) continue;
        if (strcmp(child->name, "id") == 0) {
            if ((child->flags & ARROW_FLAG_NULLABLE) != 0) found_nullable_id = 1;
        }
    }
    if (schema.release) schema.release(&schema);
    ASSERT(found_nullable_id, "id should be nullable after alter");

    /* NULL alterations and num_alterations == 0 must be rejected. */
    rc = lance_dataset_alter_columns(ds, NULL, 1);
    ASSERT(rc == -1, "NULL alterations must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    rc = lance_dataset_alter_columns(ds, &alt, 0);
    ASSERT(rc == -1, "num_alterations=0 must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    /* No-op alteration (all sentinels left at defaults) must be rejected. */
    LanceColumnAlteration noop = {0};
    noop.path = "id";
    rc = lance_dataset_alter_columns(ds, &noop, 1);
    ASSERT(rc == -1, "no-op alteration must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    /* Out-of-range nullable_mode discriminant must be rejected at the FFI
     * boundary rather than transmuted into the repr(C) enum. */
    LanceColumnAlteration bad_mode = {0};
    bad_mode.path = "id";
    bad_mode.nullable_mode = 99;
    rc = lance_dataset_alter_columns(ds, &bad_mode, 1);
    ASSERT(rc == -1, "invalid nullable_mode must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    lance_dataset_close(ds);
    printf("OK\n");
}

/* Re-opens the dataset just written by `test_dataset_write_roundtrip` and
 * exercises `lance_dataset_drop_columns`. Drops the `name` column so the
 * dataset is left with `id` only; subsequent tests (`compact_files`,
 * `delete`) do not reference any dropped column. Must run after
 * `test_update` / `test_merge_insert`, which both still need `name` / `id`. */
static void test_drop_columns(const char *write_uri) {
    printf("  test_drop_columns... ");

    LanceDataset *ds = lance_dataset_open(write_uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");
    uint64_t before_rows = lance_dataset_count_rows(ds);
    CHECK_OK();
    ASSERT(before_rows > 0, "fixture expected to have rows");
    uint64_t v_before = lance_dataset_version(ds);

    /* Snapshot the schema so we can confirm the field count decreased
     * (otherwise a bug that bumped the version without modifying the
     * schema would pass silently). The ArrowSchema struct owns its
     * children — release it before any potentially-aborting assert so
     * we don't leak under sanitizer runs in CI. */
    struct ArrowSchema schema_before;
    memset(&schema_before, 0, sizeof(schema_before));
    int32_t rc = lance_dataset_schema(ds, &schema_before);
    ASSERT(rc == 0, "schema export failed");
    int64_t fields_before = schema_before.n_children;
    if (schema_before.release) schema_before.release(&schema_before);

    const char *cols[] = {"name", "embedding", "score"};
    rc = lance_dataset_drop_columns(ds, cols, 3);
    ASSERT(rc == 0, "drop_columns failed");
    uint64_t after_rows = lance_dataset_count_rows(ds);
    CHECK_OK();
    ASSERT(after_rows == before_rows,
           "metadata-only drop must not change row count");
    ASSERT(lance_dataset_version(ds) > v_before,
           "drop_columns must bump the version");

    /* Schema field count must have decreased by exactly 3. The C-test
     * fixture has 4 columns (`id`, `name`, `score`, `embedding`) — assert `fields_after == 1`
     * so this self-documents the post-drop expectation and trips if the
     * fixture ever grows additional columns. Release the exported
     * schema before any assertion so we never leak it on failure. */
    struct ArrowSchema schema_after;
    memset(&schema_after, 0, sizeof(schema_after));
    rc = lance_dataset_schema(ds, &schema_after);
    ASSERT(rc == 0, "schema export failed after drop");
    int64_t fields_after = schema_after.n_children;
    if (schema_after.release) schema_after.release(&schema_after);
    ASSERT(fields_after == fields_before - 3,
           "schema field count must decrease by 3 after drop");
    ASSERT(fields_after == 1,
           "C-test fixture should be left with `id` only after dropping columns");

    /* NULL `columns` and num_columns == 0 must both be rejected. */
    rc = lance_dataset_drop_columns(ds, NULL, 1);
    ASSERT(rc == -1, "NULL columns must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    rc = lance_dataset_drop_columns(ds, cols, 0);
    ASSERT(rc == -1, "num_columns=0 must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    /* Dropping the sole remaining column (`id`) must fail with
     * INVALID_ARGUMENT — upstream refuses to leave a dataset with zero
     * fields. */
    const char *last_col[] = {"id"};
    rc = lance_dataset_drop_columns(ds, last_col, 1);
    ASSERT(rc == -1, "dropping last column must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    lance_dataset_close(ds);
    printf("OK\n");
}

/* Re-opens the dataset (reduced to `id` only by `test_drop_columns`) and
 * exercises the three `lance_dataset_add_columns_*` entry points. The positive
 * path uses the SQL variant — strings only, no hand-built Arrow C structures;
 * the nulls/stream variants are smoke-checked through their NULL-argument
 * rejections, since their happy paths are covered by the Rust integration
 * tests. The added `id_doubled` column is harmless to the subsequent
 * compact/delete steps. Must run after `test_drop_columns`. */
static void test_add_columns(const char *write_uri) {
    printf("  test_add_columns... ");

    LanceDataset *ds = lance_dataset_open(write_uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");
    uint64_t v_before = lance_dataset_version(ds);

    /* Snapshot field count before the add so we can confirm it grew by one. */
    struct ArrowSchema schema_before;
    memset(&schema_before, 0, sizeof(schema_before));
    int32_t rc = lance_dataset_schema(ds, &schema_before);
    ASSERT(rc == 0, "schema export failed");
    int64_t fields_before = schema_before.n_children;
    if (schema_before.release) schema_before.release(&schema_before);

    /* SQL variant: derive `id_doubled = id * 2` from the surviving `id`. */
    LanceSqlColumn col = {0};
    col.name = "id_doubled";
    col.expression = "id * 2";
    rc = lance_dataset_add_columns_sql(ds, &col, 1, 0);
    ASSERT(rc == 0, "add_columns_sql failed");
    ASSERT(lance_dataset_version(ds) > v_before,
           "add_columns_sql must bump the version");

    struct ArrowSchema schema_after;
    memset(&schema_after, 0, sizeof(schema_after));
    rc = lance_dataset_schema(ds, &schema_after);
    ASSERT(rc == 0, "schema export failed after add");
    int64_t fields_after = schema_after.n_children;
    if (schema_after.release) schema_after.release(&schema_after);
    ASSERT(fields_after == fields_before + 1,
           "schema field count must increase by 1 after add");

    /* SQL rejections: NULL dataset, NULL columns, zero count, NULL name. */
    rc = lance_dataset_add_columns_sql(NULL, &col, 1, 0);
    ASSERT(rc == -1, "NULL dataset must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    rc = lance_dataset_add_columns_sql(ds, NULL, 1, 0);
    ASSERT(rc == -1, "NULL columns must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    rc = lance_dataset_add_columns_sql(ds, &col, 0, 0);
    ASSERT(rc == -1, "num_columns=0 must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    LanceSqlColumn bad_name = {0};
    bad_name.name = NULL;
    bad_name.expression = "id * 2";
    rc = lance_dataset_add_columns_sql(ds, &bad_name, 1, 0);
    ASSERT(rc == -1, "NULL name must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    LanceSqlColumn empty_name = {0};
    empty_name.name = "";
    empty_name.expression = "id * 2";
    rc = lance_dataset_add_columns_sql(ds, &empty_name, 1, 0);
    ASSERT(rc == -1, "empty name must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    LanceSqlColumn null_expr = {0};
    null_expr.name = "x";
    null_expr.expression = NULL;
    rc = lance_dataset_add_columns_sql(ds, &null_expr, 1, 0);
    ASSERT(rc == -1, "NULL expression must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    LanceSqlColumn empty_expr = {0};
    empty_expr.name = "x";
    empty_expr.expression = "";
    rc = lance_dataset_add_columns_sql(ds, &empty_expr, 1, 0);
    ASSERT(rc == -1, "empty expression must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    /* AllNulls variant rejections: NULL dataset, NULL schema. */
    rc = lance_dataset_add_columns_nulls(NULL, NULL);
    ASSERT(rc == -1, "NULL dataset must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    rc = lance_dataset_add_columns_nulls(ds, NULL);
    ASSERT(rc == -1, "NULL schema must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    /* Stream variant rejections. The stream-NULL check fires first, so passing
     * NULL for both arguments also surfaces INVALID_ARGUMENT. (The
     * valid-stream + NULL-dataset path runs after the stream is consumed and
     * cannot be smoke-tested in pure C without a live stream struct; it is
     * covered by the Rust integration tests.) */
    rc = lance_dataset_add_columns_stream(NULL, NULL, 0);
    ASSERT(rc == -1, "NULL dataset and NULL stream must fail (stream check first)");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    rc = lance_dataset_add_columns_stream(ds, NULL, 0);
    ASSERT(rc == -1, "NULL stream must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    lance_dataset_close(ds);
    printf("OK\n");
}

/* Builds one uncommitted scalar index segment and exercises the byte-buffer
 * and parsed-metadata ownership APIs from a real C caller. */
static void test_index_segment_builder(const char *uri) {
    printf("  test_index_segment_builder... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");

    uint64_t fragment_count = lance_dataset_fragment_count(ds);
    CHECK_OK();
    ASSERT(fragment_count > 0, "fixture must contain at least one fragment");
    uint64_t *all_fragment_ids =
        (uint64_t *)calloc((size_t)fragment_count, sizeof(uint64_t));
    ASSERT(all_fragment_ids != NULL, "fragment-id allocation failed");
    ASSERT(lance_dataset_fragment_ids(ds, all_fragment_ids) == 0,
           "fragment enumeration failed");
    uint32_t selected_fragment_id = (uint32_t)all_fragment_ids[0];
    free(all_fragment_ids);

    const uint8_t expected_uuid[16] = {
        0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0x4d, 0xef,
        0x80, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde,
    };
    LanceIndexSegmentBuildOptions options;
    memset(&options, 0, sizeof(options));
    options.fragment_ids = &selected_fragment_id;
    options.fragment_count = 1;
    options.index_uuid = expected_uuid;
    options.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;

    uint64_t version_before = lance_dataset_version(ds);
    LanceIndexSegmentBuilder *builder =
        lance_index_segment_builder_new_scalar(
            ds, "id", "c_id_segment", LANCE_SCALAR_BITMAP, NULL, &options);
    ASSERT(builder != NULL, "scalar segment builder creation failed");

    uint8_t *metadata_bytes = NULL;
    size_t metadata_len = 0;
    int32_t rc = lance_index_segment_builder_execute_uncommitted(
        builder, &metadata_bytes, &metadata_len);
    ASSERT(rc == 0, "uncommitted scalar segment build failed");
    ASSERT(metadata_bytes != NULL && metadata_len > 0,
           "segment build returned empty metadata");
    ASSERT(lance_dataset_version(ds) == version_before,
           "uncommitted build must not change the dataset version");

    LanceIndexSegmentMetadata *metadata = NULL;
    rc = lance_index_segment_metadata_parse(
        metadata_bytes, metadata_len, &metadata);
    ASSERT(rc == 0 && metadata != NULL, "metadata parse failed");

    uint8_t actual_uuid[16] = {0};
    ASSERT(lance_index_segment_metadata_uuid(metadata, actual_uuid) == 0,
           "metadata UUID read failed");
    ASSERT(memcmp(actual_uuid, expected_uuid, sizeof(actual_uuid)) == 0,
           "metadata UUID mismatch");
    ASSERT(strcmp(lance_index_segment_metadata_name(metadata),
                  "c_id_segment") == 0,
           "metadata name mismatch");
    ASSERT(lance_index_segment_metadata_dataset_version(metadata) ==
               version_before,
           "metadata dataset version mismatch");
    ASSERT(lance_index_segment_metadata_index_version(metadata) == 0,
           "metadata index version mismatch");
    ASSERT(lance_index_segment_metadata_index_type(metadata) ==
               LANCE_SCALAR_BITMAP,
           "metadata index type mismatch");
    const char *type_url =
        lance_index_segment_metadata_index_details_type_url(metadata);
    ASSERT(type_url != NULL && strstr(type_url, "BitmapIndexDetails") != NULL,
           "metadata type URL mismatch");
    ASSERT(lance_index_segment_metadata_field_count(metadata) == 1,
           "metadata field count mismatch");
    int32_t field_id = -1;
    size_t field_count = 0;
    ASSERT(lance_index_segment_metadata_field_ids(
               metadata, &field_id, 1, &field_count) == 0,
           "metadata field IDs read failed");
    ASSERT(field_count == 1 && field_id == 0,
           "metadata field ID mismatch");
    ASSERT(lance_index_segment_metadata_fragment_count(metadata) == 1,
           "metadata fragment count mismatch");

    uint32_t actual_fragment_id = 0;
    size_t written = 0;
    ASSERT(lance_index_segment_metadata_fragment_ids(
               metadata, &actual_fragment_id, 1, &written) == 0,
           "metadata fragment IDs read failed");
    ASSERT(written == 1 && actual_fragment_id == selected_fragment_id,
           "metadata fragment ID mismatch");

    lance_index_segment_metadata_free(metadata);
    lance_free_bytes(metadata_bytes);
    lance_index_segment_builder_free(builder);
    lance_dataset_close(ds);
    printf("OK\n");
}

/* Runs a small vector segment build with a progress callback and verifies
 * events are delivered with a readable stage name and a round-tripped
 * callback context. */
static void test_index_segment_builder_progress(const char *uri) {
    printf("  test_index_segment_builder_progress... ");
    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");
    uint64_t fragment_count = lance_dataset_fragment_count(ds);
    ASSERT(fragment_count >= 2, "vector fixture must have two fragments");
    uint64_t all_ids[2] = {0, 0};
    ASSERT(lance_dataset_fragment_ids(ds, all_ids) == 0,
           "fragment enumeration failed");
    uint32_t fragment_ids[2] = {(uint32_t)all_ids[0], (uint32_t)all_ids[1]};

    LanceVectorIndexSegmentParams params = {
        LANCE_INDEX_IVF_FLAT, LANCE_METRIC_L2, 2, 0, 0, 2, 0, 0, 16,
    };
    LanceIndexSegmentBuildOptions options = {0};
    options.fragment_ids = fragment_ids;
    options.fragment_count = 2;
    options.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;
    LanceIndexSegmentBuilder *builder =
        lance_index_segment_builder_new_vector(
            ds, "embedding", "c_progress_idx", &params, &options);
    ASSERT(builder != NULL, "vector segment builder failed");

    BuildProgressCapture captured = {0};
    captured.expected_ctx = &captured;
    ASSERT(lance_index_segment_builder_set_progress_callback(
               builder, capture_build_progress, &captured) == 0,
           "progress callback registration failed");

    uint8_t *bytes = NULL;
    size_t len = 0;
    ASSERT(lance_index_segment_builder_execute_uncommitted(
               builder, &bytes, &len) == 0,
           "vector segment execution failed");
    ASSERT(bytes != NULL && len > 0, "empty segment metadata");

    ASSERT(captured.events > 0, "expected progress events");
    ASSERT(captured.invalid == 0, "malformed progress event");
    ASSERT(captured.ctx_mismatch == 0, "callback_ctx must round-trip");
    ASSERT(captured.starts > 0 && captured.completes > 0,
           "expected at least one START and one COMPLETE stage");
    ASSERT(captured.saw_shuffle_start && captured.saw_shuffle_complete,
           "expected shuffle START and COMPLETE events");

    lance_free_bytes(bytes);
    lance_index_segment_builder_free(builder);
    lance_dataset_close(ds);
    printf("events=%llu... OK\n", (unsigned long long)captured.events);
}

static void test_vector_models_and_reusable_segments(const char *uri) {
    printf("  test_vector_models_and_reusable_segments... ");
    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");
    uint64_t fragment_count = lance_dataset_fragment_count(ds);
    ASSERT(fragment_count >= 2, "vector fixture must have two fragments");
    uint64_t all_ids[2] = {0, 0};
    ASSERT(lance_dataset_fragment_ids(ds, all_ids) == 0,
           "fragment enumeration failed");
    ASSERT(all_ids[0] <= UINT32_MAX && all_ids[1] <= UINT32_MAX,
           "fragment id does not fit segment ABI");
    uint32_t fragment_ids[2] = {(uint32_t)all_ids[0], (uint32_t)all_ids[1]};

    struct ArrowArray centroids = {0};
    struct ArrowSchema centroids_schema = {0};
    ASSERT(lance_index_train_ivf_model(
               ds, "embedding", 2, LANCE_METRIC_L2, NULL, 0,
               &centroids, &centroids_schema) == 0,
           "IVF training failed");
    struct ArrowArray codebook = {0};
    struct ArrowSchema codebook_schema = {0};
    ASSERT(lance_index_train_pq_model(
               ds, "embedding", 2, 4, LANCE_METRIC_L2, NULL, 0,
               &centroids, &centroids_schema, &codebook, &codebook_schema) == 0,
           "residual PQ training failed");

    LanceVectorIndexSegmentParams params = {
        LANCE_INDEX_IVF_PQ, LANCE_METRIC_L2, 2, 2, 4, 2, 0, 0, 16,
    };
    for (size_t i = 0; i < 2; i++) {
        LanceIndexSegmentBuildOptions options = {0};
        options.fragment_ids = &fragment_ids[i];
        options.fragment_count = 1;
        options.ivf_centroids = &centroids;
        options.ivf_centroids_schema = &centroids_schema;
        options.pq_codebook = &codebook;
        options.pq_codebook_schema = &codebook_schema;
        options.mode = LANCE_INDEX_SEGMENT_BUILD_PRECOMPUTED;
        LanceIndexSegmentBuilder *builder =
            lance_index_segment_builder_new_vector(
                ds, "embedding", NULL, &params, &options);
        ASSERT(builder != NULL, "vector segment builder failed");
        ASSERT(centroids.release != NULL && codebook.release != NULL,
               "models must remain reusable");
        uint8_t *bytes = NULL;
        size_t len = 0;
        ASSERT(lance_index_segment_builder_execute_uncommitted(
                   builder, &bytes, &len) == 0,
               "vector segment execution failed");
        LanceIndexSegmentMetadata *metadata = NULL;
        ASSERT(lance_index_segment_metadata_parse(bytes, len, &metadata) == 0,
               "vector metadata parse failed");
        uint32_t actual = UINT32_MAX;
        size_t written = 0;
        ASSERT(lance_index_segment_metadata_fragment_ids(
                   metadata, &actual, 1, &written) == 0,
               "vector metadata fragment read failed");
        ASSERT(written == 1 && actual == fragment_ids[i],
               "vector metadata fragment mismatch");
        lance_index_segment_metadata_free(metadata);
        lance_free_bytes(bytes);
        lance_index_segment_builder_free(builder);
    }
    if (centroids.release) centroids.release(&centroids);
    if (centroids_schema.release) centroids_schema.release(&centroids_schema);
    if (codebook.release) codebook.release(&codebook);
    if (codebook_schema.release) codebook_schema.release(&codebook_schema);
    lance_dataset_close(ds);
    printf("OK\n");
}

/* Builds one uncommitted vector segment per fragment and commits them as a
 * single logical multi-segment index from a real C caller. */
static void test_commit_index_segments(const char *uri) {
    printf("  test_commit_index_segments... ");
    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");
    uint64_t all_ids[2] = {0, 0};
    ASSERT(lance_dataset_fragment_ids(ds, all_ids) == 0,
           "fragment enumeration failed");
    uint32_t fragment_ids[2] = {(uint32_t)all_ids[0], (uint32_t)all_ids[1]};

    LanceVectorIndexSegmentParams params = {
        LANCE_INDEX_IVF_FLAT, LANCE_METRIC_L2, 2, 0, 0, 2, 0, 0, 16,
    };
    uint8_t *segment_bytes[2] = {NULL, NULL};
    size_t segment_lens[2] = {0, 0};
    uint8_t expected_uuids[2][16];
    memset(expected_uuids, 0, sizeof(expected_uuids));
    for (size_t i = 0; i < 2; i++) {
        LanceIndexSegmentBuildOptions options = {0};
        options.fragment_ids = &fragment_ids[i];
        options.fragment_count = 1;
        options.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;
        LanceIndexSegmentBuilder *builder =
            lance_index_segment_builder_new_vector(
                ds, "embedding", "c_distributed_idx", &params, &options);
        ASSERT(builder != NULL, "vector segment builder failed");
        ASSERT(lance_index_segment_builder_execute_uncommitted(
                   builder, &segment_bytes[i], &segment_lens[i]) == 0,
               "vector segment execution failed");
        LanceIndexSegmentMetadata *metadata = NULL;
        ASSERT(lance_index_segment_metadata_parse(
                   segment_bytes[i], segment_lens[i], &metadata) == 0,
               "metadata parse failed");
        ASSERT(lance_index_segment_metadata_uuid(metadata,
                                                 expected_uuids[i]) == 0,
               "metadata UUID read failed");
        lance_index_segment_metadata_free(metadata);
        lance_index_segment_builder_free(builder);
    }

    /* One commit registers both segments as a single logical index. */
    uint64_t version_before = lance_dataset_version(ds);
    const uint8_t *const_bytes[2] = {segment_bytes[0], segment_bytes[1]};
    int32_t rc = lance_dataset_commit_index_segments(
        ds, "c_distributed_idx", "embedding", const_bytes, segment_lens, 2);
    ASSERT(rc == 0, "commit_index_segments failed");
    ASSERT(lance_dataset_version(ds) == version_before + 1,
           "commit must bump the dataset version exactly once");
    ASSERT(lance_dataset_index_segment_count(ds, "c_distributed_idx") == 2,
           "committed index must have two segments");
    ASSERT(lance_dataset_prewarm_index(ds, "c_distributed_idx") == 0,
           "prewarm both index segments");
    ASSERT(lance_dataset_prewarm_index(ds, "c_distributed_idx") == 0,
           "repeat prewarm");

    uint8_t committed_uuids[32] = {0};
    uint64_t committed_count = 0;
    ASSERT(lance_dataset_index_segments(ds, "c_distributed_idx",
                                        committed_uuids, 2,
                                        &committed_count) == 0,
           "segment enumeration failed");
    ASSERT(committed_count == 2, "committed segment count mismatch");
    ASSERT(memcmp(committed_uuids, expected_uuids[0], 16) == 0 &&
               memcmp(committed_uuids + 16, expected_uuids[1], 16) == 0,
           "committed segment UUIDs mismatch");

    const char *segments_json = lance_dataset_index_segments_json(ds);
    ASSERT(segments_json != NULL, "index segment listing failed");
    ASSERT(strstr(segments_json, "\"name\":\"c_distributed_idx\"") != NULL,
           "segment listing must include the committed index name");
    ASSERT(strstr(segments_json, "\"index_version\":1") != NULL,
           "segment listing must include the recorded physical index version");
    char first_coverage[64];
    char second_coverage[64];
    snprintf(first_coverage, sizeof(first_coverage),
             "\"fragment_ids\":[%u]", fragment_ids[0]);
    snprintf(second_coverage, sizeof(second_coverage),
             "\"fragment_ids\":[%u]", fragment_ids[1]);
    ASSERT(strstr(segments_json, first_coverage) != NULL,
           "segment listing must record the first segment's coverage");
    ASSERT(strstr(segments_json, second_coverage) != NULL,
           "segment listing must record the second segment's coverage");
    lance_free_string(segments_json);

    /* Duplicate segment UUIDs in the commit set are rejected. */
    const uint8_t *dup_bytes[2] = {segment_bytes[0], segment_bytes[0]};
    size_t dup_lens[2] = {segment_lens[0], segment_lens[0]};
    rc = lance_dataset_commit_index_segments(ds, "c_dup_idx", "embedding",
                                             dup_bytes, dup_lens, 2);
    ASSERT(rc == -1, "duplicate segment UUIDs must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    /* An empty commit set is rejected. */
    rc = lance_dataset_commit_index_segments(ds, "c_empty_idx", "embedding",
                                             const_bytes, segment_lens, 0);
    ASSERT(rc == -1, "empty commit set must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");
    ASSERT(lance_dataset_version(ds) == version_before + 1,
           "rejected commits must not bump the version");

    lance_free_bytes(segment_bytes[0]);
    lance_free_bytes(segment_bytes[1]);
    lance_dataset_close(ds);
    printf("OK\n");
}

/* Re-opens the dataset just written by `test_dataset_write_roundtrip` and
 * exercises `lance_dataset_compact_files`. The smoke fixture is a single
 * fragment, so the default planner has nothing to compact — we expect
 * all-zero metrics and no version bump. Must run before `test_delete`. */
static void test_compact_files(const char *write_uri) {
    printf("  test_compact_files... ");

    LanceDataset *ds = lance_dataset_open(write_uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");
    uint64_t v_before = lance_dataset_version(ds);

    LanceCompactionMetrics metrics;
    memset(&metrics, 0, sizeof(metrics));
    int32_t rc = lance_dataset_compact_files(ds, NULL, &metrics);
    ASSERT(rc == 0, "compact_files failed");
    ASSERT(metrics.fragments_removed == 0 && metrics.fragments_added == 0,
           "expected no-op metrics on a clean single-fragment dataset");
    ASSERT(lance_dataset_version(ds) == v_before,
           "no-op compaction must not bump the version");

    /* NULL dataset must be rejected with INVALID_ARGUMENT. */
    rc = lance_dataset_compact_files(NULL, NULL, NULL);
    ASSERT(rc == -1, "NULL dataset must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    lance_dataset_close(ds);
    printf("OK\n");
}

/* Re-opens the dataset just written by `test_dataset_write_roundtrip` and
 * exercises `lance_dataset_delete`. Must run after the write roundtrip. */
static void test_delete(const char *write_uri) {
    printf("  test_delete... ");

    LanceDataset *ds = lance_dataset_open(write_uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");

    uint64_t before = lance_dataset_count_rows(ds);
    CHECK_OK();
    ASSERT(before > 0, "fixture expected to have rows");

    /* Match-everything predicate; deleted count must equal `before`. */
    uint64_t deleted = 0;
    int32_t rc = lance_dataset_delete(ds, "true", &deleted);
    ASSERT(rc == 0, "delete failed");
    ASSERT(deleted == before, "deleted count mismatch");
    ASSERT(lance_dataset_count_rows(ds) == 0, "expected zero rows after delete");

    /* NULL predicate must be rejected with INVALID_ARGUMENT. */
    rc = lance_dataset_delete(ds, NULL, NULL);
    ASSERT(rc == -1, "NULL predicate must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "expected INVALID_ARGUMENT");

    lance_dataset_close(ds);
    printf("deleted=%llu... OK\n", (unsigned long long)deleted);
}

static void check_distance_range_results(struct ArrowArrayStream *stream,
                                         const float *lower, const float *upper,
                                         uint32_t expected_ids, int64_t expected_count) {
    struct ArrowSchema schema = {0};
    ASSERT(stream->get_schema(stream, &schema) == 0, "get_schema failed");
    int id_column = -1, distance_column = -1;
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (strcmp(schema.children[i]->name, "id") == 0) id_column = (int)i;
        if (strcmp(schema.children[i]->name, "_distance") == 0) distance_column = (int)i;
    }
    ASSERT(id_column >= 0 && distance_column >= 0, "missing range result columns");
    ASSERT(strcmp(schema.children[id_column]->format, "i") == 0 &&
           strcmp(schema.children[distance_column]->format, "f") == 0, "unexpected range result types");
    schema.release(&schema);
    int64_t rows = 0;
    uint32_t seen = 0;
    while (1) {
        struct ArrowArray array = {0};
        ASSERT(stream->get_next(stream, &array) == 0, "get_next failed");
        if (!array.release) break;
        const struct ArrowArray *ids = array.children[id_column];
        const struct ArrowArray *distances = array.children[distance_column];
        const int32_t *id_values = (const int32_t *)ids->buffers[1];
        const float *distance_values = (const float *)distances->buffers[1];
        for (int64_t i = 0; i < array.length; ++i) {
            int32_t id = id_values[ids->offset + i];
            float distance = distance_values[distances->offset + i];
            ASSERT(id >= 1 && id <= 20, "unexpected range result id");
            uint32_t bit = 1u << (id - 1);
            ASSERT((seen & bit) == 0, "duplicate range result id");
            seen |= bit;
            ASSERT(distance >= 0.0f && (!lower || distance >= *lower) && (!upper || distance < *upper), "distance outside half-open range");
        }
        rows += array.length;
        array.release(&array);
    }
    ASSERT(rows == expected_count && seen == expected_ids, "range result ids/count mismatch");
    stream->release(stream);
}

static void test_distance_range(const char *uri) {
    printf("  test_distance_range... ");
    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "open failed");
    float query[8];
    for (int i = 0; i < 8; ++i) query[i] = 0.1f + (float)i;
    const float positive_lower = 0.02f, positive_upper = 0.125f, zero = 0.0f;
    const int64_t expected[] = {2, 1, 20, 20, 0};
    const uint32_t expected_ids[] = {3u, 2u, (1u << 20) - 1, (1u << 20) - 1, 0u};
    for (int mode = 0; mode < 5; ++mode) {
        LanceScanner *scanner = lance_scanner_new(ds, NULL, NULL);
        ASSERT(scanner != NULL, "create scanner failed");
        ASSERT(lance_scanner_nearest(scanner, "embedding", query, 8,
                                    LANCE_DTYPE_FLOAT32, 20) == 0, "nearest failed");
        ASSERT(lance_scanner_set_use_index(scanner, false) == 0, "use_index failed");
        const float *lower = mode == 1 ? &positive_lower : NULL;
        const float *upper = &positive_upper;
        /* Row 1 lies on zero: distinguish inclusive lower and exclusive upper bounds. */
        if (mode == 3) { lower = &zero; upper = NULL; }
        if (mode == 4) upper = &zero;
        ASSERT(lance_scanner_set_distance_range(scanner, lower, upper) == 0,
               "distance_range failed");
        if (mode == 2) {
            ASSERT(lance_scanner_set_distance_range(scanner, NULL, NULL) == 0,
                   "clearing distance_range failed");
            lower = NULL;
            upper = NULL;
        }
        ASSERT(lance_scanner_set_distance_range(scanner, &positive_upper, &positive_lower) == -1,
               "reversed distance range must fail");
        ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
               "expected INVALID_ARGUMENT");
        struct ArrowArrayStream stream = {0};
        ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0, "stream failed");
        check_distance_range_results(&stream, lower, upper, expected_ids[mode], expected[mode]);
        lance_scanner_close(scanner);
    }
    lance_dataset_close(ds);
    printf("OK\n");
}

static void check_batch_nearest_results(struct ArrowArrayStream *stream) {
    struct ArrowSchema schema = {0};
    ASSERT(stream->get_schema(stream, &schema) == 0, "batch nearest result");
    int query_column = -1, id_column = -1, distance_column = -1;
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (strcmp(schema.children[i]->name, "query_index") == 0) {
            query_column = (int)i;
        }
        if (strcmp(schema.children[i]->name, "id") == 0) id_column = (int)i;
        if (strcmp(schema.children[i]->name, "_distance") == 0) distance_column = (int)i;
    }
    ASSERT(query_column >= 0, "batch nearest result");
    ASSERT(strcmp(schema.children[query_column]->format, "i") == 0, "batch nearest result");
    ASSERT(id_column >= 0 && distance_column >= 0, "missing result columns");
    schema.release(&schema);
    int counts[2] = {0, 0};
    int seen[2] = {0, 0};
    while (1) {
        struct ArrowArray batch = {0};
        ASSERT(stream->get_next(stream, &batch) == 0, "batch nearest result");
        if (!batch.release) break;
        struct ArrowArray *queries = batch.children[query_column];
        const int32_t *values = (const int32_t *)queries->buffers[1];
        struct ArrowArray *ids = batch.children[id_column];
        struct ArrowArray *distances = batch.children[distance_column];
        const int32_t *id_values = (const int32_t *)ids->buffers[1];
        const float *distance_values = (const float *)distances->buffers[1];
        for (int64_t i = 0; i < batch.length; ++i) {
            int32_t index = values[queries->offset + i];
            ASSERT(index >= 0 && index < 2, "batch nearest result");
            int32_t id = id_values[ids->offset + i];
            int32_t first_id = index == 0 ? 1 : 19;
            ASSERT(id >= first_id && id <= first_id + 1, "wrong per-query neighbor");
            seen[index] |= 1 << (id - first_id);
            float expected = (id == 1 || id == 20) ? 0.0f : 0.08f;
            float error = distance_values[distances->offset + i] - expected;
            ASSERT(error > -0.0001f && error < 0.0001f, "wrong per-query distance");
            ++counts[index];
        }
        batch.release(&batch);
    }
    ASSERT(counts[0] == 2 && counts[1] == 2, "batch nearest result");
    ASSERT(seen[0] == 3 && seen[1] == 3, "missing per-query neighbor");
    stream->release(stream);
}

static void test_batch_nearest(const char *uri) {
    float queries[16];
    for (int i = 0; i < 8; ++i) {
        queries[i] = 0.1f + i;
        queries[8 + i] = 2.0f + i;
    }
    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open batch dataset");
    LanceScanner *scanner = lance_scanner_new(dataset, NULL, NULL);
    ASSERT(scanner != NULL, "open batch scanner");
    ASSERT(lance_scanner_nearest_batch(scanner, "embedding", queries, 8, 2, LANCE_DTYPE_FLOAT32, 2) == 0, "batch nearest");
    ASSERT(lance_scanner_set_use_index(scanner, false) == 0, "flat batch search");
    ASSERT(lance_scanner_set_batch_size(scanner, 1) == 0, "small output batches");
    struct ArrowArrayStream stream = {0};
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0, "export batch stream");
    lance_scanner_close(scanner);
    lance_dataset_close(dataset);
    check_batch_nearest_results(&stream);
}

static void test_materialization_and_fast_search(const char *uri) {
    printf("  test_materialization_and_fast_search... ");

    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open materialization dataset");
    const char *projection[] = {"id", "name", NULL};
    const char *except_name[] = {"name"};
    const char *missing_name[] = {"missing"};
    struct ArrowArrayStream stream;

    for (int style = LANCE_MATERIALIZATION_STYLE_HEURISTIC;
         style <= LANCE_MATERIALIZATION_STYLE_ALL_EARLY_EXCEPT; ++style) {
        LanceScanner *scanner =
            lance_scanner_new(dataset, projection, "id >= 18");
        ASSERT(scanner != NULL, "create materialization scanner");
        const char *const *columns =
            style == LANCE_MATERIALIZATION_STYLE_ALL_EARLY_EXCEPT
                ? except_name
                : NULL;
        size_t column_count =
            style == LANCE_MATERIALIZATION_STYLE_ALL_EARLY_EXCEPT ? 1 : 0;
        ASSERT(lance_scanner_set_materialization_style(
                   scanner, (LanceMaterializationStyle)style,
                   columns, column_count) == 0,
               "set materialization style");
        memset(&stream, 0, sizeof(stream));
        ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
               "materialization stream");
        ASSERT(count_stream_rows(&stream) == 3,
               "materialization result count");
        lance_scanner_close(scanner);
    }

    LanceScanner *scanner = lance_scanner_new(dataset, projection, "id >= 18");
    ASSERT(scanner != NULL, "create validation scanner");
    ASSERT(lance_scanner_set_materialization_style(
               scanner, LANCE_MATERIALIZATION_STYLE_ALL_LATE, NULL, 0) == 0,
           "set all-late");
    ASSERT(lance_scanner_set_materialization_style(
               scanner, LANCE_MATERIALIZATION_STYLE_ALL_EARLY_EXCEPT,
               missing_name, 1) == -1,
           "missing exception column must fail");
    ASSERT(lance_last_error_code() != LANCE_OK,
           "missing exception column must set an error");
    ASSERT(lance_scanner_set_materialization_style(
               scanner, (LanceMaterializationStyle)99, NULL, 0) == -1,
           "unknown materialization style must fail");
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "scan after rejected style");
    ASSERT(count_stream_rows(&stream) == 3,
           "rejected style must preserve prior configuration");
    ASSERT(lance_scanner_set_materialization_style(
               scanner, LANCE_MATERIALIZATION_STYLE_ALL_EARLY, NULL, 0) == -1,
           "style after scan start must fail");
    ASSERT(lance_scanner_set_fast_search(scanner, true) == -1,
           "fast search after scan start must fail");
    lance_scanner_close(scanner);

    float query[8];
    for (int i = 0; i < 8; ++i) query[i] = 0.1f + (float)i;

    scanner = lance_scanner_new(dataset, NULL, NULL);
    ASSERT(scanner != NULL, "create ordinary nearest scanner");
    ASSERT(lance_scanner_nearest(scanner, "embedding", query, 8,
                                LANCE_DTYPE_FLOAT32, 5) == 0,
           "ordinary nearest");
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "ordinary nearest stream");
    ASSERT(count_stream_rows(&stream) == 5,
           "ordinary nearest must use native fallback without an index");
    lance_scanner_close(scanner);

    scanner = lance_scanner_new(dataset, NULL, NULL);
    ASSERT(scanner != NULL, "create fast scanner");
    ASSERT(lance_scanner_nearest(scanner, "embedding", query, 8,
                                LANCE_DTYPE_FLOAT32, 5) == 0,
           "fast nearest");
    ASSERT(lance_scanner_set_fast_search(scanner, true) == 0, "fast search");
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "fast nearest stream");
    ASSERT(count_stream_rows(&stream) == 0,
           "fast search without an index must preserve native empty result");
    lance_scanner_close(scanner);

    scanner = lance_scanner_new(dataset, NULL, NULL);
    ASSERT(scanner != NULL, "create fast-without-index scanner");
    ASSERT(lance_scanner_nearest(scanner, "embedding", query, 8,
                                LANCE_DTYPE_FLOAT32, 5) == 0,
           "fast-without-index nearest");
    ASSERT(lance_scanner_set_fast_search(scanner, true) == 0,
           "enable fast search");
    ASSERT(lance_scanner_set_use_index(scanner, false) == 0,
           "disable index under fast search");
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "fast-without-index stream");
    ASSERT(count_stream_rows(&stream) == 0,
           "fast search must not restore flat fallback");
    lance_scanner_close(scanner);

    scanner = lance_scanner_new(dataset, NULL, NULL);
    ASSERT(scanner != NULL, "create fast-reset scanner");
    ASSERT(lance_scanner_nearest(scanner, "embedding", query, 8,
                                LANCE_DTYPE_FLOAT32, 5) == 0,
           "fast-reset nearest");
    ASSERT(lance_scanner_set_fast_search(scanner, true) == 0,
           "enable fast search before reset");
    ASSERT(lance_scanner_set_fast_search(scanner, false) == 0,
           "reset fast search");
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "fast-reset stream");
    ASSERT(count_stream_rows(&stream) == 5,
           "disabling fast search must restore native fallback");
    lance_scanner_close(scanner);

    lance_dataset_close(dataset);
    printf("OK\n");
}

/* Collect the `id` column from an ordering stream. Returns the row count and
 * stores up to `capacity` ids in `out`. */
static size_t collect_ordering_ids(
    struct ArrowArrayStream *stream,
    int32_t *out,
    size_t capacity) {
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(stream->get_schema(stream, &schema) == 0, "ordering schema");
    int id_column = -1;
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (strcmp(schema.children[i]->name, "id") == 0) {
            id_column = (int)i;
        }
    }
    ASSERT(id_column >= 0, "ordering output must contain id");
    ASSERT(strcmp(schema.children[id_column]->format, "i") == 0,
           "ordering id must be int32");
    if (schema.release) schema.release(&schema);

    size_t count = 0;
    while (1) {
        struct ArrowArray array;
        memset(&array, 0, sizeof(array));
        ASSERT(stream->get_next(stream, &array) == 0, "ordering get_next");
        if (array.release == NULL) break;
        const struct ArrowArray *ids = array.children[id_column];
        const int32_t *values = (const int32_t *)ids->buffers[1];
        for (int64_t i = 0; i < array.length; ++i) {
            ASSERT(count < capacity, "ordering output exceeds capacity");
            out[count++] = values[ids->offset + i];
        }
        array.release(&array);
    }
    if (stream->release) stream->release(stream);
    return count;
}

static int compare_int32(const void *lhs, const void *rhs) {
    int32_t left = *(const int32_t *)lhs;
    int32_t right = *(const int32_t *)rhs;
    return (left > right) - (left < right);
}

static void assert_ids_match(
    const int32_t *actual,
    size_t actual_count,
    const int32_t *expected,
    size_t expected_count) {
    ASSERT(actual_count == expected_count, "indexed query row count mismatch");
    int32_t sorted_actual[64];
    int32_t sorted_expected[64];
    ASSERT(actual_count <= 64, "indexed query result exceeds test capacity");
    memcpy(sorted_actual, actual, actual_count * sizeof(int32_t));
    memcpy(sorted_expected, expected, expected_count * sizeof(int32_t));
    qsort(sorted_actual, actual_count, sizeof(int32_t), compare_int32);
    qsort(sorted_expected, expected_count, sizeof(int32_t), compare_int32);
    ASSERT(memcmp(sorted_actual, sorted_expected,
                  actual_count * sizeof(int32_t)) == 0,
           "indexed query ids mismatch");
}

/* Scan `id` through the C entry point and retain the execution statistics. */
static size_t scan_ids_with_statistics(
    const char *uri,
    const char *filter,
    int use_scalar_index,
    int32_t *out,
    size_t capacity,
    ScanStatisticsCapture *captured) {
    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "native scalar dataset open failed");
    const char *columns[] = {"id", "name", "score", NULL};
    LanceScanner *scanner = lance_scanner_new(ds, columns, filter);
    ASSERT(scanner != NULL, "native scalar scanner creation failed");
    ASSERT(lance_scanner_set_use_scalar_index(
               scanner, use_scalar_index != 0) == 0,
           "native scalar use_scalar_index setter failed");
    memset(captured, 0, sizeof(*captured));
    ASSERT(lance_scanner_set_statistics_callback(
               scanner, capture_scan_statistics, captured) == 0,
           "native scalar statistics callback registration failed");
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "native scalar to_arrow_stream failed");
    size_t count = collect_ordering_ids(&stream, out, capacity);
    lance_scanner_close(scanner);
    lance_dataset_close(ds);
    return count;
}

static void check_native_scalar_query(
    const char *uri,
    const char *filter,
    const int32_t *expected,
    size_t expected_count) {
    int32_t actual[64];
    ScanStatisticsCapture stats;
    size_t count =
        scan_ids_with_statistics(uri, filter, 1, actual, 64, &stats);
    assert_ids_match(actual, count, expected, expected_count);
    ASSERT(stats.calls == 1, "native scalar statistics callback count mismatch");
    ASSERT(stats.indices_loaded > 0,
           "native scalar query must load an index");

    count = scan_ids_with_statistics(uri, filter, 0, actual, 64, &stats);
    assert_ids_match(actual, count, expected, expected_count);
    ASSERT(stats.indices_loaded == 0,
           "disabled scalar index must not load an index");
}

static void test_native_scalar_indexes(const char *uri) {
    printf("  test_native_scalar_indexes... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "native scalar dataset open failed");
    ASSERT(lance_dataset_create_scalar_index(
               ds, "name", "c_name_ngram", LANCE_SCALAR_NGRAM, NULL, 0) == 0,
           "C NGram index creation failed");
    ASSERT(lance_dataset_create_scalar_index(
               ds, "score", "c_score_zonemap", LANCE_SCALAR_ZONE_MAP,
               "{\"rows_per_zone\":3,\"use_seeds\":false}", 0) == 0,
           "C ZoneMap index creation failed");
    ASSERT(lance_dataset_create_scalar_index(
               ds, "id", "c_id_bloom", LANCE_SCALAR_BLOOM_FILTER,
               "{\"number_of_items\":8,\"probability\":0.01}", 0) == 0,
           "C BloomFilter index creation failed");

    char *json = (char *)lance_dataset_index_list_json(ds);
    ASSERT(json != NULL, "C index list failed");
    ASSERT(strstr(json, "\"name\":\"c_name_ngram\"") != NULL &&
               strstr(json, "\"type\":\"NGram\"") != NULL,
           "C NGram index listing mismatch");
    ASSERT(strstr(json, "\"name\":\"c_score_zonemap\"") != NULL &&
               strstr(json, "\"type\":\"ZoneMap\"") != NULL,
           "C ZoneMap index listing mismatch");
    ASSERT(strstr(json, "\"name\":\"c_id_bloom\"") != NULL &&
               strstr(json, "\"type\":\"BloomFilter\"") != NULL,
           "C BloomFilter index listing mismatch");
    lance_free_string(json);

    uint64_t index_count = lance_dataset_index_count(ds);
    CHECK_OK();

    const int32_t alice[] = {1};
    const int32_t none[] = {0};
    const int32_t high_scores[] = {18, 19, 20};
    const int32_t null_scores[] = {7, 16};
    const int32_t id_thirteen[] = {13};
    check_native_scalar_query(uri, "contains(name, 'ali')", alice, 1);
    check_native_scalar_query(uri, "contains(name, 'zzz')", none, 0);
    check_native_scalar_query(uri, "score >= 180", high_scores, 3);
    check_native_scalar_query(uri, "score IS NULL", null_scores, 2);
    check_native_scalar_query(uri, "id = 13", id_thirteen, 1);
    check_native_scalar_query(uri, "id = 999", none, 0);

    ASSERT(lance_dataset_create_scalar_index(
               ds, "score", "c_score_zonemap", LANCE_SCALAR_ZONE_MAP,
               "{\"rows_per_zone\":4,\"use_seeds\":false}", 1) == 0,
           "C ZoneMap replacement failed");
    ASSERT(lance_dataset_index_count(ds) == index_count,
           "C ZoneMap replacement changed the index count");

    const int32_t expected_after_replace[] = {13};
    check_native_scalar_query(uri, "id = 13", expected_after_replace, 1);

    ASSERT(lance_dataset_prewarm_index(ds, "c_name_ngram") == 0,
           "C NGram prewarm failed");
    ASSERT(lance_dataset_prewarm_index(ds, "c_score_zonemap") == 0,
           "C ZoneMap prewarm failed");
    ASSERT(lance_dataset_prewarm_index(ds, "c_id_bloom") == 0,
           "C BloomFilter prewarm failed");

    lance_dataset_close(ds);
    ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "C native scalar dataset reopen failed");
    json = (char *)lance_dataset_index_list_json(ds);
    ASSERT(json != NULL, "C reopened index list failed");
    ASSERT(strstr(json, "\"name\":\"c_name_ngram\"") != NULL &&
               strstr(json, "\"name\":\"c_score_zonemap\"") != NULL &&
               strstr(json, "\"name\":\"c_id_bloom\"") != NULL,
           "C native scalar indexes did not survive reopen");
    lance_free_string(json);

    ASSERT(lance_dataset_drop_index(ds, "c_name_ngram") == 0,
           "C NGram drop failed");
    ASSERT(lance_dataset_drop_index(ds, "c_score_zonemap") == 0,
           "C ZoneMap drop failed");
    ASSERT(lance_dataset_drop_index(ds, "c_id_bloom") == 0,
           "C BloomFilter drop failed");
    json = (char *)lance_dataset_index_list_json(ds);
    ASSERT(json != NULL, "C final index list failed");
    ASSERT(strstr(json, "c_name_ngram") == NULL &&
               strstr(json, "c_score_zonemap") == NULL &&
               strstr(json, "c_id_bloom") == NULL,
           "C dropped native scalar indexes remain listed");
    lance_free_string(json);
    lance_dataset_close(ds);
    printf("OK\n");
}

static int collect_minhash_alice(struct ArrowArrayStream *stream) {
    struct ArrowSchema schema = {0};
    ASSERT(stream->get_schema(stream, &schema) == 0,
           "MinHash stream schema failed");
    int id_column = -1;
    int distance_column = -1;
    for (int64_t i = 0; i < schema.n_children; ++i) {
        const struct ArrowSchema *child = schema.children[i];
        if (child != NULL && child->name != NULL) {
            if (strcmp(child->name, "id") == 0) id_column = (int)i;
            if (strcmp(child->name, "_distance") == 0) distance_column = (int)i;
        }
    }
    ASSERT(id_column >= 0, "MinHash schema must contain id");
    ASSERT(distance_column >= 0, "MinHash schema must contain _distance");
    schema.release(&schema);

    int found_alice = 0;
    int64_t total_rows = 0;
    while (1) {
        struct ArrowArray array = {0};
        ASSERT(stream->get_next(stream, &array) == 0,
               "MinHash stream get_next failed");
        if (!array.release) break;
        const struct ArrowArray *ids = array.children[id_column];
        const struct ArrowArray *distances = array.children[distance_column];
        ASSERT(ids != NULL && ids->buffers[1] != NULL,
               "MinHash id values missing");
        ASSERT(distances != NULL && distances->buffers[1] != NULL,
               "MinHash distance values missing");
        const int32_t *id_values = (const int32_t *)ids->buffers[1];
        const float *distance_values = (const float *)distances->buffers[1];
        for (int64_t row = 0; row < array.length; ++row) {
            const int32_t id = id_values[ids->offset + row];
            const float distance = distance_values[distances->offset + row];
            ASSERT(distance >= 0.0f, "MinHash distance must be non-negative");
            total_rows += 1;
            if (id == 1) {
                ASSERT(distance == 0.0f,
                       "identical MinHash text must have distance zero");
                found_alice += 1;
            }
        }
        array.release(&array);
    }
    stream->release(stream);
    ASSERT(total_rows > 0, "MinHash search must return rows");
    return found_alice;
}

static void test_minhash_lsh(const char *uri) {
    printf("  test_minhash_lsh... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "MinHash dataset open failed");
    ASSERT(lance_dataset_create_scalar_index(
               ds, "name", "c_minhash", LANCE_SCALAR_MINHASH_LSH,
               "{\"num_hashes\":64,\"num_bands\":16,\"shingle_size\":1}", 0) == 0,
           "C MinHash index creation failed");

    char *json = (char *)lance_dataset_index_list_json(ds);
    ASSERT(json != NULL, "C MinHash index list failed");
    ASSERT(strstr(json, "\"name\":\"c_minhash\"") != NULL &&
               strstr(json, "\"type\":\"MinHashLsh\"") != NULL,
           "C MinHash index listing mismatch");
    lance_free_string(json);
    json = (char *)lance_dataset_index_segments_json(ds);
    ASSERT(json != NULL, "C MinHash segment list failed");
    ASSERT(strstr(json, "\"name\":\"c_minhash\"") != NULL &&
               strstr(json, "\"type\":\"MinHashLsh\"") != NULL,
           "C MinHash segment listing mismatch");
    lance_free_string(json);
    ASSERT(lance_dataset_prewarm_index(ds, "c_minhash") == 0,
           "C MinHash prewarm failed");

    const char *columns[] = {"id", "name", NULL};
    LanceScanner *scanner = lance_scanner_new(ds, columns, NULL);
    ASSERT(scanner != NULL, "C MinHash scanner creation failed");
    ASSERT(lance_scanner_set_minhash_query(scanner, "name", "alice") == 0,
           "C MinHash query setter failed");
    ASSERT(lance_scanner_set_limit(scanner, 10) == 0,
           "C MinHash limit failed");
    struct ArrowArrayStream stream = {0};
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "C MinHash stream failed");
    CHECK_OK();
    ASSERT(collect_minhash_alice(&stream) == 1,
           "C MinHash query must return exact alice row");
    lance_scanner_close(scanner);

    scanner = lance_scanner_new(ds, columns, NULL);
    ASSERT(scanner != NULL, "C MinHash retention scanner creation failed");
    ASSERT(lance_scanner_set_minhash_query(scanner, "name", "alice") == 0,
           "C MinHash retention query failed");
    ASSERT(lance_scanner_set_limit(scanner, 3) == 0,
           "C MinHash retention limit failed");
    ASSERT(lance_scanner_set_minhash_query(scanner, "", "alice") == -1,
           "C MinHash empty column must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "C MinHash empty column error code");
    const char *message = lance_last_error_message();
    ASSERT(message != NULL, "C MinHash empty column must set an error");
    lance_free_string(message);
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "C MinHash failed setter must retain the old query");
    ASSERT(collect_minhash_alice(&stream) == 1,
           "C MinHash retained query must find alice");
    lance_scanner_close(scanner);

    scanner = lance_scanner_new(ds, columns, NULL);
    ASSERT(scanner != NULL, "C MinHash missing-limit scanner creation failed");
    ASSERT(lance_scanner_set_minhash_query(scanner, "name", "alice") == 0,
           "C MinHash missing-limit query failed");
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == -1,
           "C MinHash search without a limit must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "C MinHash missing-limit error code");
    message = lance_last_error_message();
    ASSERT(message != NULL && strstr(message, "requires a limit") != NULL,
           "C MinHash missing-limit native error");
    lance_free_string(message);
    lance_scanner_close(scanner);

    scanner = lance_scanner_new(ds, columns, NULL);
    ASSERT(scanner != NULL, "C MinHash missing-index scanner creation failed");
    ASSERT(lance_scanner_set_minhash_query(scanner, "score", "alice") == 0,
           "C MinHash missing-index query failed");
    ASSERT(lance_scanner_set_limit(scanner, 5) == 0,
           "C MinHash missing-index limit failed");
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == -1,
           "C MinHash search without an index must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "C MinHash missing-index error code");
    message = lance_last_error_message();
    ASSERT(message != NULL &&
               strstr(message, "No MinHash LSH index found for column score") != NULL,
           "C MinHash missing-index native error");
    lance_free_string(message);
    lance_scanner_close(scanner);

    ASSERT(lance_dataset_drop_index(ds, "c_minhash") == 0,
           "C MinHash drop failed");
    lance_dataset_close(ds);
    printf("OK\n");
}

static void check_ordering(
    LanceScanner *scanner,
    const int32_t *expected,
    size_t expected_count) {
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "ordering stream failed");
    int32_t actual[32];
    size_t count = collect_ordering_ids(&stream, actual, 32);
    ASSERT(count == expected_count, "ordering row count mismatch");
    ASSERT(memcmp(actual, expected, expected_count * sizeof(int32_t)) == 0,
           "ordering ids mismatch");
}

static void test_scanner_order_by(const char *uri) {
    printf("  test_scanner_order_by... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "ordering dataset open failed");
    const int32_t expected[] = {1, 5, 9, 8, 0, 4, 6, 11, 2, 3, 7, 10};
    LanceColumnOrdering ordering[] = {
        {"rank", LANCE_ORDER_DESCENDING, LANCE_ORDER_NULLS_FIRST},
        {"id", LANCE_ORDER_ASCENDING, LANCE_ORDER_NULLS_LAST},
    };

    LanceScanner *scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "ordering scanner");
    ASSERT(lance_scanner_set_order_by(scanner, ordering, 2) == 0,
           "set order by");
    check_ordering(scanner, expected, 12);
    lance_scanner_close(scanner);

    /* Invalid raw enum values and NULL names retain the previous ordering. */
    scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "ordering scanner");
    ASSERT(lance_scanner_set_order_by(scanner, ordering, 2) == 0,
           "set order by");
    LanceColumnOrdering invalid = {
        "rank", 7, LANCE_ORDER_NULLS_LAST,
    };
    ASSERT(lance_scanner_set_order_by(scanner, &invalid, 1) == -1,
           "invalid direction must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "invalid direction code");
    invalid.direction = LANCE_ORDER_ASCENDING;
    invalid.null_order = 7;
    ASSERT(lance_scanner_set_order_by(scanner, &invalid, 1) == -1,
           "invalid null order must fail");
    invalid.null_order = LANCE_ORDER_NULLS_LAST;
    invalid.column_name = NULL;
    ASSERT(lance_scanner_set_order_by(scanner, &invalid, 1) == -1,
           "NULL column name must fail");
    check_ordering(scanner, expected, 12);
    lance_scanner_close(scanner);

    /* The setter copies the caller's column string. */
    char column_name[8];
    strcpy(column_name, "rank");
    LanceColumnOrdering copied[] = {
        {column_name, LANCE_ORDER_DESCENDING, LANCE_ORDER_NULLS_FIRST},
        {"id", LANCE_ORDER_ASCENDING, LANCE_ORDER_NULLS_LAST},
    };
    scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "ordering scanner");
    ASSERT(lance_scanner_set_order_by(scanner, copied, 2) == 0,
           "set copied order by");
    strcpy(column_name, "junk");
    check_ordering(scanner, expected, 12);
    lance_scanner_close(scanner);

    /* NULL resets to the same rows as a fresh, unordered scanner. */
    int32_t baseline[32];
    scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "ordering scanner");
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "baseline stream");
    size_t baseline_count =
        collect_ordering_ids(&stream, baseline, 32);
    lance_scanner_close(scanner);

    scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "ordering scanner");
    ASSERT(lance_scanner_set_order_by(scanner, ordering, 2) == 0,
           "set order by");
    ASSERT(lance_scanner_set_order_by(scanner, NULL, 0) == 0,
           "clear order by");
    check_ordering(scanner, baseline, baseline_count);
    lance_scanner_close(scanner);

    scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "ordering scanner");
    ASSERT(lance_scanner_set_order_by(scanner, ordering, 2) == 0,
           "set order by");
    ASSERT(lance_scanner_set_order_by(scanner, &invalid, 0) == 0,
           "explicit empty order by");
    check_ordering(scanner, baseline, baseline_count);
    lance_scanner_close(scanner);

    /* Missing columns are a native materialization error. */
    scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "ordering scanner");
    LanceColumnOrdering missing = {
        "missing", LANCE_ORDER_ASCENDING, LANCE_ORDER_NULLS_LAST,
    };
    ASSERT(lance_scanner_set_order_by(scanner, &missing, 1) == 0,
           "set missing order column");
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == -1,
           "missing order column must fail at materialization");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "missing order column code");
    const char *message = lance_last_error_message();
    ASSERT(message != NULL && strstr(message, "missing") != NULL,
           "missing order column message");
    if (message) lance_free_string(message);
    lance_scanner_close(scanner);

    lance_dataset_close(ds);
    printf("OK\n");
}

static uint8_t *read_file_bytes(const char *path, size_t *len) {
    FILE *file = fopen(path, "rb");
    ASSERT(file != NULL, "aggregate plan file open");
    ASSERT(fseek(file, 0, SEEK_END) == 0, "aggregate plan file seek");
    long size = ftell(file);
    ASSERT(size >= 0, "aggregate plan file size");
    ASSERT(fseek(file, 0, SEEK_SET) == 0, "aggregate plan file rewind");
    uint8_t *bytes = malloc((size_t)size);
    ASSERT(bytes != NULL || size == 0, "aggregate plan allocation");
    if (size > 0) {
        ASSERT(fread(bytes, 1, (size_t)size, file) == (size_t)size,
               "aggregate plan read");
    }
    ASSERT(fclose(file) == 0, "aggregate plan file close");
    *len = (size_t)size;
    return bytes;
}

static void test_scanner_substrait_aggregate(const char *uri, const char *plan_path) {
    printf("  test_scanner_substrait_aggregate... ");

    size_t len = 0;
    uint8_t *bytes = read_file_bytes(plan_path, &len);
    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "aggregate dataset open failed");
    LanceScanner *scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "aggregate scanner");
    ASSERT(lance_scanner_set_substrait_aggregate(scanner, bytes, len) == 0,
           "set substrait aggregate");
    CHECK_OK();
    if (len > 0) memset(bytes, 0, len);
    free(bytes);

    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "aggregate stream");
    CHECK_OK();
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(stream.get_schema(&stream, &schema) == 0, "aggregate schema");
    ASSERT(schema.n_children == 5, "aggregate field count");
    const char *expected_names[] = {
        "rows", "id_sum", "rank_sum", "min_id", "max_id",
    };
    const char *expected_formats[] = {"l", "l", "l", "i", "i"};
    for (int64_t i = 0; i < schema.n_children; ++i) {
        ASSERT(strcmp(schema.children[i]->name, expected_names[i]) == 0,
               "aggregate field name");
        ASSERT(strcmp(schema.children[i]->format, expected_formats[i]) == 0,
               "aggregate field format");
    }
    if (schema.release) schema.release(&schema);

    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    ASSERT(stream.get_next(&stream, &array) == 0, "aggregate get_next");
    ASSERT(array.release != NULL, "aggregate result batch");
    ASSERT(array.length == 1, "aggregate result count");
    ASSERT(array.n_children == 5, "aggregate result width");
    const int64_t *rows =
        (const int64_t *)array.children[0]->buffers[1] + array.children[0]->offset;
    const int64_t *id_sum =
        (const int64_t *)array.children[1]->buffers[1] + array.children[1]->offset;
    const int64_t *rank_sum =
        (const int64_t *)array.children[2]->buffers[1] + array.children[2]->offset;
    const int32_t *min_id =
        (const int32_t *)array.children[3]->buffers[1] + array.children[3]->offset;
    const int32_t *max_id =
        (const int32_t *)array.children[4]->buffers[1] + array.children[4]->offset;
    ASSERT(rows[0] == 12, "aggregate count");
    ASSERT(id_sum[0] == 66, "aggregate id sum");
    ASSERT(rank_sum[0] == 15, "aggregate rank sum");
    ASSERT(min_id[0] == 0, "aggregate min id");
    ASSERT(max_id[0] == 11, "aggregate max id");
    array.release(&array);

    memset(&array, 0, sizeof(array));
    ASSERT(stream.get_next(&stream, &array) == 0, "aggregate end");
    ASSERT(array.release == NULL, "aggregate stream ends after one batch");
    if (stream.release) stream.release(&stream);

    ASSERT(lance_scanner_set_substrait_aggregate(scanner, NULL, 0) == -1,
           "aggregate setter after scan start must fail");
    const char *error = lance_last_error_message();
    ASSERT(error != NULL &&
               strstr(error, "substrait_aggregate must be set before") != NULL,
           "aggregate lifecycle error");
    if (error) lance_free_string(error);
    lance_scanner_close(scanner);
    lance_dataset_close(ds);
    printf("OK\n");
}

static const struct ArrowSchema *find_schema_child(
    const struct ArrowSchema *schema,
    const char *name
) {
    for (int64_t i = 0; i < schema->n_children; ++i) {
        const struct ArrowSchema *child = schema->children[i];
        if (child != NULL && child->name != NULL &&
            strcmp(child->name, name) == 0) {
            return child;
        }
    }
    return NULL;
}

static void test_fragment_snapshot(const char *uri) {
    printf("  test_fragment_snapshot... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "fragment fixture open failed");
    CHECK_OK();

    ASSERT(lance_dataset_fragment_count(ds) == 2,
           "fragment fixture must contain two fragments");
    CHECK_OK();
    uint64_t ids[2] = {0, 0};
    ASSERT(lance_dataset_fragment_ids(ds, ids) == 0,
           "fragment enumeration failed");
    CHECK_OK();

    LanceFragment *fragment = lance_dataset_open_fragment(ds, ids[0]);
    ASSERT(fragment != NULL, "fragment lookup failed");
    CHECK_OK();

    LanceFragment *missing =
        lance_dataset_open_fragment(ds, UINT64_MAX);
    ASSERT(missing == NULL, "out-of-range fragment ID must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_NOT_FOUND,
           "out-of-range fragment ID must be NOT_FOUND");
    const char *message = lance_last_error_message();
    ASSERT(message != NULL && strstr(message, "fragment_id") != NULL,
           "missing fragment error must name fragment_id");
    if (message) lance_free_string(message);

    ASSERT(lance_dataset_open_fragment(NULL, ids[0]) == NULL,
           "NULL dataset must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL dataset must be INVALID_ARGUMENT");
    message = lance_last_error_message();
    if (message) lance_free_string(message);

    uint64_t rows = 123;
    ASSERT(lance_fragment_physical_rows(fragment, NULL) == -1,
           "NULL physical-row output must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL physical-row output must be INVALID_ARGUMENT");
    ASSERT(rows == 123, "failed count must not write the output");
    message = lance_last_error_message();
    if (message) lance_free_string(message);

    ASSERT(lance_fragment_deleted_rows(NULL, &rows) == -1,
           "NULL fragment deleted-row count must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL fragment must be INVALID_ARGUMENT");
    message = lance_last_error_message();
    if (message) lance_free_string(message);

    ASSERT(lance_fragment_id(NULL) == 0, "NULL fragment ID must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL fragment ID must be INVALID_ARGUMENT");
    message = lance_last_error_message();
    if (message) lance_free_string(message);

    ASSERT(lance_fragment_metadata_json(NULL) == NULL,
           "NULL fragment metadata must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL fragment metadata must be INVALID_ARGUMENT");
    message = lance_last_error_message();
    if (message) lance_free_string(message);

    /* The fragment owns its snapshot; every read remains valid after close. */
    lance_dataset_close(ds);
    ASSERT(lance_fragment_id(fragment) == ids[0],
           "fragment ID changed after dataset close");
    CHECK_OK();
    ASSERT(lance_fragment_data_file_count(fragment) == 1,
           "fragment should have one base data file");
    CHECK_OK();

    const char *metadata = lance_fragment_metadata_json(fragment);
    ASSERT(metadata != NULL, "fragment metadata JSON failed");
    CHECK_OK();
    ASSERT(strstr(metadata, "\"files\":[") != NULL,
           "metadata must include data files");
    ASSERT(strstr(metadata, "\"path\":\"") != NULL,
           "metadata must include data file paths");
    ASSERT(strstr(metadata, "\"physical_rows\":4") != NULL,
           "metadata must preserve the native physical row count");
    ASSERT(strstr(metadata, "\"deletion_file\":{") != NULL,
           "metadata must include deletion-file information");
    ASSERT(strstr(metadata, "\"num_deleted_rows\":1") != NULL,
           "metadata must preserve the native deletion count");
    lance_free_string(metadata);

    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(lance_fragment_schema(fragment, &schema) == 0,
           "fragment schema export failed");
    CHECK_OK();
    ASSERT(schema.n_children == 3, "fragment schema field count mismatch");
    const struct ArrowSchema *profile = find_schema_child(&schema, "profile");
    ASSERT(profile != NULL, "nested profile field missing");
    ASSERT(profile->format != NULL && strcmp(profile->format, "+s") == 0,
           "profile must remain a struct");
    ASSERT(profile->n_children == 2, "struct field children missing");
    ASSERT(find_schema_child(profile, "name") != NULL,
           "nested name field missing");
    ASSERT(find_schema_child(profile, "score") != NULL,
           "nested score field missing");
    const struct ArrowSchema *meta = find_schema_child(&schema, "meta");
    ASSERT(meta != NULL, "JSON meta field missing");
    ASSERT(meta->format != NULL && strcmp(meta->format, "Z") == 0,
           "fragment schema must retain native JSONB storage");
    if (schema.release) schema.release(&schema);

    ASSERT(lance_fragment_physical_rows(fragment, &rows) == 0,
           "physical row count failed");
    ASSERT(rows == 4, "physical row count mismatch");
    rows = 0;
    ASSERT(lance_fragment_deleted_rows(fragment, &rows) == 0,
           "deleted row count failed");
    ASSERT(rows == 1, "deleted row count mismatch");
    rows = 0;
    ASSERT(lance_fragment_logical_rows(fragment, &rows) == 0,
           "logical row count failed");
    ASSERT(rows == 3, "logical row count mismatch");

    lance_fragment_close(fragment);
    printf("OK\n");
}

static void check_fragment_take_ids(
    struct ArrowArrayStream *stream,
    const char *label,
    const int32_t *expected,
    size_t expected_len,
    int profile_name_only
) {
    struct ArrowSchema schema = {0};
    ASSERT(stream->get_schema(stream, &schema) == 0, "fragment take get_schema");
    ASSERT(schema.n_children == 3, "fragment take schema field count");
    int id_column = -1;
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (schema.children[i] != NULL && schema.children[i]->name != NULL &&
            strcmp(schema.children[i]->name, "id") == 0) {
            id_column = (int)i;
        }
    }
    ASSERT(id_column >= 0, "fragment take id column missing");
    const struct ArrowSchema *profile = find_schema_child(&schema, "profile");
    ASSERT(profile != NULL && profile->format != NULL &&
           strcmp(profile->format, "+s") == 0, "fragment take profile schema");
    ASSERT(profile->n_children == (profile_name_only ? 1 : 2),
           "fragment take profile projection");
    ASSERT(find_schema_child(&schema, "meta") != NULL,
           "fragment take JSON field missing");
    schema.release(&schema);

    int32_t actual[16];
    size_t actual_len = 0;
    while (1) {
        struct ArrowArray array = {0};
        ASSERT(stream->get_next(stream, &array) == 0, "fragment take get_next");
        if (!array.release) break;
        ASSERT(array.n_children == 3, "fragment take batch field count");
        const struct ArrowArray *ids = array.children[id_column];
        ASSERT(ids != NULL && ids->buffers[1] != NULL, "fragment take id values");
        const int32_t *values = (const int32_t *)ids->buffers[1];
        for (int64_t row = 0; row < array.length; ++row) {
            ASSERT(actual_len < sizeof(actual) / sizeof(actual[0]),
                   "fragment take output too large for fixture assertion");
            actual[actual_len++] = values[ids->offset + row];
        }
        array.release(&array);
    }
    stream->release(stream);
    ASSERT(actual_len == expected_len, label);
    ASSERT(memcmp(actual, expected, expected_len * sizeof(expected[0])) == 0,
           label);
}

static void test_fragment_read_operations(const char *uri) {
    printf("  test_fragment_read_operations... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "fragment read fixture open failed");
    CHECK_OK();
    bool stable_row_ids = false;
    ASSERT(lance_dataset_has_stable_row_ids(ds, &stable_row_ids) == 0,
           "fragment read stable-row-ID inspection failed");
    ASSERT(stable_row_ids, "fragment read fixture must use stable row IDs");
    uint64_t ids[2] = {0, 0};
    ASSERT(lance_dataset_fragment_ids(ds, ids) == 0,
           "fragment read enumeration failed");
    CHECK_OK();
    LanceFragment *fragment = lance_dataset_open_fragment(ds, ids[0]);
    ASSERT(fragment != NULL, "fragment read lookup failed");
    CHECK_OK();

    uint64_t rows = UINT64_MAX;
    ASSERT(lance_fragment_count_rows(fragment, NULL, &rows) == 0,
           "fragment count_rows(NULL) failed");
    CHECK_OK();
    ASSERT(rows == 3, "fragment count_rows(NULL) mismatch");
    ASSERT(lance_fragment_count_rows(fragment, "id >= 2", &rows) == 0,
           "fragment filtered count failed");
    CHECK_OK();
    ASSERT(rows == 2, "fragment filtered count mismatch");
    rows = 77;
    ASSERT(lance_fragment_count_rows(fragment, "", &rows) == -1,
           "empty fragment filter must fail");
    ASSERT(rows == 77, "failed fragment count must not write out_rows");
    const char *message = lance_last_error_message();
    ASSERT(message != NULL, "empty fragment filter must set an error");
    lance_free_string(message);

    const uint32_t indices[] = {0, 2};
    const int32_t expected[] = {0, 3};
    struct ArrowArrayStream stream = {0};
    ASSERT(lance_fragment_take(
               fragment, indices, 2, NULL, &stream) == 0,
           "fragment take full projection failed");
    CHECK_OK();
    check_fragment_take_ids(
        &stream, "fragment take logical offsets",
        expected, 2, 0);

    const char *nested_columns[] = {"id", "profile.name", "meta", NULL};
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_fragment_take(
               fragment, indices, 2, nested_columns, &stream) == 0,
           "fragment take nested projection failed");
    CHECK_OK();
    check_fragment_take_ids(
        &stream, "fragment take nested projection rows",
        expected, 2, 1);

    const char *empty_columns[] = {NULL};
    const uint32_t one_index[] = {0};
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_fragment_take(
               fragment, one_index, 1, empty_columns, &stream) == -1,
           "explicit empty projection must preserve native take failure");
    ASSERT(lance_last_error_code() == LANCE_ERR_NOT_FOUND,
           "explicit empty projection native error code");
    ASSERT(stream.release == NULL,
           "failed empty projection must leave out uninitialized");
    message = lance_last_error_message();
    ASSERT(message != NULL && strstr(message, "No data files") != NULL,
           "explicit empty projection native error message");
    lance_free_string(message);

    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_fragment_take(
               fragment, NULL, 0, NULL, &stream) == 0,
           "fragment take NULL/zero indices failed");
    CHECK_OK();
    ASSERT(count_stream_rows(&stream) == 0,
           "NULL/zero indices must produce an empty stream");

    const uint32_t out_of_range[] = {3};
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_fragment_take(
               fragment, out_of_range, 1, NULL, &stream) == -1,
           "fragment take out-of-range index must fail");
    ASSERT(stream.release == NULL, "failed take must leave out uninitialized");
    message = lance_last_error_message();
    ASSERT(message != NULL, "out-of-range take must set an error");
    lance_free_string(message);
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_fragment_take(
               fragment, NULL, 1, NULL, &stream) == -1,
           "fragment take NULL indices with nonzero count must fail");
    message = lance_last_error_message();
    ASSERT(message != NULL && strstr(message, "indices") != NULL,
           "NULL indices error must name indices");
    lance_free_string(message);

    LanceScanner *scanner = lance_fragment_scan(fragment);
    ASSERT(scanner != NULL, "fragment scan failed");
    CHECK_OK();
    LanceColumnOrdering descending = {
        "id", LANCE_ORDER_DESCENDING, LANCE_ORDER_NULLS_LAST,
    };
    ASSERT(lance_scanner_set_order_by(scanner, &descending, 1) == 0,
           "fragment scanner order_by");
    ASSERT(lance_scanner_set_limit(scanner, 1) == 0,
           "fragment scanner limit");
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "fragment scanner stream");
    CHECK_OK();
    const int32_t last_id[] = {3};
    check_fragment_take_ids(
        &stream, "fragment scanner must honor normal controls",
        last_id, 1, 0);
    lance_scanner_close(scanner);

    scanner = lance_fragment_scan(fragment);
    ASSERT(scanner != NULL, "fragment validation scanner");
    LanceColumnOrdering missing = {
        "missing", LANCE_ORDER_ASCENDING, LANCE_ORDER_NULLS_LAST,
    };
    ASSERT(lance_scanner_set_order_by(scanner, &missing, 1) == 0,
           "set missing fragment order column");
    struct LanceBatch *batch = NULL;
    ASSERT(lance_scanner_next(scanner, &batch) == -1,
           "missing fragment order column must fail during iteration");
    ASSERT(batch == NULL, "failed fragment iteration must not return a batch");
    message = lance_last_error_message();
    ASSERT(message != NULL && strstr(message, "missing") != NULL,
           "fragment iteration error must name the missing column");
    lance_free_string(message);
    lance_scanner_close(scanner);

    /* Streams and scanners own their results after parent handles close. */
    memset(&stream, 0, sizeof(stream));
    const int32_t first_id[] = {0};
    ASSERT(lance_fragment_take(
               fragment, one_index, 1, NULL, &stream) == 0,
           "fragment take before close");
    scanner = lance_fragment_scan(fragment);
    ASSERT(scanner != NULL, "fragment scan before close");
    struct ArrowArrayStream scan_stream = {0};
    ASSERT(lance_scanner_to_arrow_stream(scanner, &scan_stream) == 0,
           "fragment scan stream before close");
    lance_scanner_close(scanner);
    lance_fragment_close(fragment);
    lance_dataset_close(ds);

    check_fragment_take_ids(
        &stream, "take stream must outlive fragment and dataset",
        first_id, 1, 0);
    const int32_t fragment_ids[] = {0, 2, 3};
    check_fragment_take_ids(
        &scan_stream, "scan stream must retain initial fragment scope",
        fragment_ids, 3, 0);

    printf("OK\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc < 12) {
        fprintf(stderr,
                "Usage: %s <dataset_uri> <write_uri> <blob_uri> <ordering_uri> "
                "<session_uri> <fragment_uri> <fragment_read_uri> <cleanup_uri> "
                "<aggregate_plan_path> <config_base_uri> <alternate_base_uri>\n",
                argv[0]);
        return 1;
    }

    const char *uri = argv[1];
    const char *write_uri = argv[2];
    const char *blob_uri = argv[3];
    const char *ordering_uri = argv[4];
    const char *session_uri = argv[5];
    const char *fragment_uri = argv[6];
    const char *fragment_read_uri = argv[7];
    const char *cleanup_uri = argv[8];
    const char *aggregate_plan_path = argv[9];
    const char *config_base_uri = argv[10];
    const char *alternate_base_uri = argv[11];
    printf("Running C API tests with dataset: %s\n", uri);

    test_batch_nearest(uri);
    test_materialization_and_fast_search(uri);
    test_open_and_metadata(uri);
    test_shared_session(uri);
    test_session_cache_config(session_uri);
    test_scan(uri);
    test_scanner_projection(uri);
    test_distance_range(uri);
    test_scan_with_limit(uri);
    test_scanner_introspection(uri);
    test_scan_with_row_addr_lists(uri);
    test_scanner_order_by(ordering_uri);
    test_scanner_substrait_aggregate(ordering_uri, aggregate_plan_path);
    test_fragment_snapshot(fragment_uri);
    test_fragment_read_operations(fragment_read_uri);
    test_scanner_blob_handling(blob_uri);
    test_take_blobs(blob_uri);
    test_blob_native_read_slice(blob_uri);
    test_blob_fields_and_descriptors();
    test_blob_planned_reads(blob_uri);
    test_managed_blob_descriptor_roundtrip(write_uri);
    test_versions(uri);
    test_native_refs_and_tags(uri);
    test_branch_lifecycle(uri);
    test_restore_to_current(uri);
    test_error_handling();
    test_index_segment_builder(uri);
    test_index_segment_builder_progress(uri);
    test_vector_models_and_reusable_segments(uri);
    test_commit_index_segments(uri);
    test_native_scalar_indexes(uri);
    test_minhash_lsh(uri);
    test_dataset_write_roundtrip(uri, write_uri);
    test_write_options_and_config(uri, write_uri);
    test_data_statistics(write_uri);
    test_config_replacement(config_base_uri);
    test_base_paths_and_add(config_base_uri, alternate_base_uri);
    test_file_enumeration(config_base_uri, alternate_base_uri);
    test_cleanup_and_lifecycle(cleanup_uri);
    test_dataset_metadata(write_uri);
    test_dataset_field_metadata(fragment_uri);
    test_update(write_uri);
    test_merge_insert(write_uri);
    test_alter_columns(write_uri);
    test_drop_columns(write_uri);
    test_add_columns(write_uri);
    test_compact_files(write_uri);
    test_delete(write_uri);

    printf("All C tests passed!\n");
    return 0;
}
