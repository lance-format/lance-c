/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file test_c_api.c
 * @brief C compilation and functional test for lance.h
 *
 * This file is compiled by the Rust integration test to verify that
 * lance.h is valid C and the API works end-to-end.
 *
 * Usage: test_c_api <dataset_uri> <write_uri> <blob_uri> <ordering_uri>
 *                    <fragment_uri>
 */

#include "lance/lance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    ASSERT(schema.n_children == 2, "fragment schema field count mismatch");
    const struct ArrowSchema *profile = find_schema_child(&schema, "profile");
    ASSERT(profile != NULL, "nested profile field missing");
    ASSERT(profile->format != NULL && strcmp(profile->format, "+s") == 0,
           "profile must remain a struct");
    ASSERT(profile->n_children == 2, "struct field children missing");
    ASSERT(find_schema_child(profile, "name") != NULL,
           "nested name field missing");
    ASSERT(find_schema_child(profile, "score") != NULL,
           "nested score field missing");
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

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr,
                "Usage: %s <dataset_uri> <write_uri> <blob_uri> "
                "<ordering_uri> <fragment_uri>\n",
                argv[0]);
        return 1;
    }

    const char *uri = argv[1];
    const char *write_uri = argv[2];
    const char *blob_uri = argv[3];
    const char *ordering_uri = argv[4];
    const char *fragment_uri = argv[5];
    printf("Running C API tests with dataset: %s\n", uri);

    test_batch_nearest(uri);
    test_materialization_and_fast_search(uri);
    test_open_and_metadata(uri);
    test_shared_session(uri);
    test_scan(uri);
    test_distance_range(uri);
    test_scan_with_limit(uri);
    test_scan_with_row_addr_lists(uri);
    test_scanner_order_by(ordering_uri);
    test_fragment_snapshot(fragment_uri);
    test_scanner_blob_handling(blob_uri);
    test_take_blobs(blob_uri);
    test_versions(uri);
    test_native_refs_and_tags(uri);
    test_restore_to_current(uri);
    test_error_handling();
    test_index_segment_builder(uri);
    test_index_segment_builder_progress(uri);
    test_vector_models_and_reusable_segments(uri);
    test_commit_index_segments(uri);
    test_native_scalar_indexes(uri);
    test_dataset_write_roundtrip(uri, write_uri);
    test_data_statistics(write_uri);
    test_dataset_metadata(write_uri);
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
