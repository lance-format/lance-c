/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file test_c_api.c
 * @brief C compilation and functional test for lance.h
 *
 * This file is compiled by the Rust integration test to verify that
 * lance.h is valid C and the API works end-to-end.
 *
 * Usage: test_c_api <dataset_uri> <write_uri>
 */

#include "lance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void test_scan(const char *uri) {
    printf("  test_scan... ");

    LanceDataset *ds = lance_dataset_open(uri, NULL, 0);
    ASSERT(ds != NULL, "dataset open failed");

    uint64_t expected_rows = lance_dataset_count_rows(ds);
    CHECK_OK();

    /* Full scan via ArrowArrayStream */
    LanceScanner *scanner = lance_scanner_new(ds, NULL, NULL);
    ASSERT(scanner != NULL, "scanner creation failed");

    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    int32_t rc = lance_scanner_to_arrow_stream(scanner, &stream);
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

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <dataset_uri> <write_uri>\n", argv[0]);
        return 1;
    }

    const char *uri = argv[1];
    const char *write_uri = argv[2];
    printf("Running C API tests with dataset: %s\n", uri);

    test_open_and_metadata(uri);
    test_scan(uri);
    test_scan_with_limit(uri);
    test_versions(uri);
    test_restore_to_current(uri);
    test_error_handling();
    test_dataset_write_roundtrip(uri, write_uri);

    printf("All C tests passed!\n");
    return 0;
}
