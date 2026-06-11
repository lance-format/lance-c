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

    const char *cols[] = {"name"};
    rc = lance_dataset_drop_columns(ds, cols, 1);
    ASSERT(rc == 0, "drop_columns failed");
    uint64_t after_rows = lance_dataset_count_rows(ds);
    CHECK_OK();
    ASSERT(after_rows == before_rows,
           "metadata-only drop must not change row count");
    ASSERT(lance_dataset_version(ds) > v_before,
           "drop_columns must bump the version");

    /* Schema field count must have decreased by exactly 1. The C-test
     * fixture has 2 columns (`id`, `name`) — assert `fields_after == 1`
     * so this self-documents the post-drop expectation and trips if the
     * fixture ever grows additional columns. Release the exported
     * schema before any assertion so we never leak it on failure. */
    struct ArrowSchema schema_after;
    memset(&schema_after, 0, sizeof(schema_after));
    rc = lance_dataset_schema(ds, &schema_after);
    ASSERT(rc == 0, "schema export failed after drop");
    int64_t fields_after = schema_after.n_children;
    if (schema_after.release) schema_after.release(&schema_after);
    ASSERT(fields_after == fields_before - 1,
           "schema field count must decrease by 1 after drop");
    ASSERT(fields_after == 1,
           "C-test fixture should be left with `id` only after dropping `name`");

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
