/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/*
 * Native dataset SQL C API consumer.
 *
 * argv[1] = multi-fragment dataset (id/name/score, three fragments)
 * argv[2] = history dataset (kept for sibling binaries; not used here)
 * argv[3] = blob-v2 dataset (id, blob)
 */

#include "lance/lance.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(condition, message)                                             \
    do {                                                                       \
        if (!(condition)) {                                                    \
            const char *lance_message = lance_last_error_message();            \
            fprintf(stderr, "ASSERT failed: %s: %s:%d", (message), __FILE__,   \
                    __LINE__);                                                  \
            if (lance_message) {                                                \
                fprintf(stderr, ": %s", lance_message);                         \
                lance_free_string(lance_message);                               \
            }                                                                   \
            fprintf(stderr, "\n");                                              \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

#define CHECK_OK()                                                             \
    do {                                                                       \
        if (lance_last_error_code() != LANCE_OK) {                             \
            const char *lance_message = lance_last_error_message();            \
            fprintf(stderr, "CHECK_OK failed: %s:%d: %s\n", __FILE__,          \
                    __LINE__, lance_message ? lance_message : "unknown");       \
            if (lance_message) lance_free_string(lance_message);               \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

static int compare_i32(const void *left, const void *right) {
    int32_t a = *(const int32_t *)left;
    int32_t b = *(const int32_t *)right;
    return (a > b) - (a < b);
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

/* The number of top-level fields advertised by the stream schema. */
static int64_t stream_field_count(struct ArrowArrayStream *stream) {
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(stream->get_schema(stream, &schema) == 0, "read stream schema");
    int64_t count = schema.n_children;
    schema.release(&schema);
    return count;
}

/* Collect the Int32 values of the first column, sorted ascending. */
static size_t collect_first_i32(struct ArrowArrayStream *stream, int32_t *out,
                                size_t capacity) {
    size_t count = 0;
    while (1) {
        struct ArrowArray batch;
        memset(&batch, 0, sizeof(batch));
        ASSERT(stream->get_next(stream, &batch) == 0, "read id batch");
        if (!batch.release) {
            break;
        }
        ASSERT(batch.n_children > 0, "id column present");
        ASSERT(batch.children[0]->n_buffers > 1, "id column has a values buffer");
        const int32_t *ids = (const int32_t *)batch.children[0]->buffers[1];
        ASSERT(count + (size_t)batch.length <= capacity, "id capacity");
        for (int64_t row = 0; row < batch.length; row++) {
            out[count++] = ids[row];
        }
        batch.release(&batch);
    }
    qsort(out, count, sizeof(int32_t), compare_i32);
    return count;
}

/* Collect the UInt64 values of one column in stream order. */
static size_t collect_u64_column(struct ArrowArrayStream *stream, int64_t column,
                                 uint64_t *out, size_t capacity) {
    size_t count = 0;
    while (1) {
        struct ArrowArray batch;
        memset(&batch, 0, sizeof(batch));
        ASSERT(stream->get_next(stream, &batch) == 0, "read u64 batch");
        if (!batch.release) {
            break;
        }
        ASSERT(batch.n_children > column, "u64 column present");
        const uint64_t *values =
            (const uint64_t *)batch.children[column]->buffers[1];
        ASSERT(count + (size_t)batch.length <= capacity, "u64 capacity");
        for (int64_t row = 0; row < batch.length; row++) {
            out[count++] = values[row];
        }
        batch.release(&batch);
    }
    return count;
}

static void assert_ids(const char *label, int32_t *actual, size_t actual_count,
                       const int32_t *expected, size_t expected_count) {
    if (actual_count != expected_count) {
        fprintf(stderr, "%s: expected %zu rows, got %zu\n", label,
                expected_count, actual_count);
        exit(1);
    }
    for (size_t i = 0; i < actual_count; i++) {
        if (actual[i] != expected[i]) {
            fprintf(stderr, "%s: row %zu expected %d, got %d\n", label, i,
                    expected[i], actual[i]);
            exit(1);
        }
    }
}

static void test_projection_filter_order(const char *uri) {
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));

    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open sql dataset");
    CHECK_OK();

    LanceSqlQueryBuilder *builder = lance_dataset_sql_builder_new(
        dataset, "SELECT id, name FROM dataset WHERE id % 3 = 0 ORDER BY id DESC");
    ASSERT(builder != NULL, "create SQL builder");
    CHECK_OK();
    ASSERT(lance_sql_query_builder_set_batch_size(builder, 4) == 0,
           "set batch size");
    CHECK_OK();

    LanceSqlQuery *query = NULL;
    ASSERT(lance_sql_query_builder_build(builder, &query) == 0, "plan query");
    CHECK_OK();
    ASSERT(query != NULL, "query handle published");

    ASSERT(lance_sql_query_into_stream(query, &stream) == 0,
           "export query stream");
    CHECK_OK();

    /* The builder and query allocations were consumed and the dataset handle
     * is irrelevant to the stream: close it before reading any batch. */
    lance_dataset_close(dataset);

    int32_t ids[30];
    size_t count = collect_first_i32(&stream, ids, 30);
    int32_t expected[] = {3, 6, 9, 12, 15, 18, 21, 24, 27, 30};
    assert_ids("projection/filter/order", ids, count, expected, 10);
    if (stream.release) stream.release(&stream);
}

static void test_renamed_table(const char *uri) {
    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open sql dataset");
    CHECK_OK();

    /* The default table name must not resolve once renamed. */
    LanceSqlQueryBuilder *builder = lance_dataset_sql_builder_new(
        dataset, "SELECT id FROM dataset ORDER BY id");
    ASSERT(builder != NULL, "create renamed builder");
    CHECK_OK();
    ASSERT(lance_sql_query_builder_set_table_name(builder, "orders") == 0,
           "rename table");
    CHECK_OK();
    LanceSqlQuery *query = NULL;
    ASSERT(lance_sql_query_builder_build(builder, &query) != 0,
           "renamed builder must not resolve the default name");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "rename conflict is invalid input");
    ASSERT(query == NULL, "failed build must not publish a query");
    lance_free_string(lance_last_error_message());

    builder = lance_dataset_sql_builder_new(
        dataset, "SELECT id FROM orders WHERE id <= 5 ORDER BY id");
    ASSERT(builder != NULL, "create renamed query builder");
    CHECK_OK();
    ASSERT(lance_sql_query_builder_set_table_name(builder, "orders") == 0,
           "rename table");
    CHECK_OK();
    ASSERT(lance_sql_query_builder_build(builder, &query) == 0,
           "plan renamed query");
    CHECK_OK();
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_sql_query_into_stream(query, &stream) == 0,
           "export renamed stream");
    CHECK_OK();
    int32_t ids[30];
    size_t count = collect_first_i32(&stream, ids, 30);
    int32_t expected[] = {1, 2, 3, 4, 5};
    assert_ids("renamed table", ids, count, expected, 5);
    if (stream.release) stream.release(&stream);

    /* A rejected setter keeps the builder's prior state. */
    builder = lance_dataset_sql_builder_new(dataset, "SELECT id FROM dataset ORDER BY id");
    ASSERT(builder != NULL, "create NULL-name builder");
    CHECK_OK();
    ASSERT(lance_sql_query_builder_set_table_name(builder, NULL) != 0,
           "NULL table name rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL table name is invalid input");
    lance_free_string(lance_last_error_message());
    ASSERT(lance_sql_query_builder_build(builder, &query) == 0,
           "default name survives the rejected setter");
    CHECK_OK();
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_sql_query_into_stream(query, &stream) == 0,
           "export default-name stream");
    CHECK_OK();
    count = collect_first_i32(&stream, ids, 30);
    ASSERT(count == 30, "default table name queries all rows");
    if (stream.release) stream.release(&stream);

    lance_dataset_close(dataset);
}

static void test_system_columns(const char *uri) {
    /* The history dataset enables stable row ids, so ids follow insertion
     * order deterministically: id 1 has row id 0, id 2 -> 1, id 4 -> 3 (id 3
     * was deleted at v4, its row id 2 is no longer live). */
    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open history dataset");
    CHECK_OK();

    /* Row ids are appended to a row-preserving projection. */
    LanceSqlQueryBuilder *builder = lance_dataset_sql_builder_new(
        dataset, "SELECT id FROM dataset WHERE id <= 4 ORDER BY id");
    ASSERT(builder != NULL, "create row-id builder");
    CHECK_OK();
    ASSERT(lance_sql_query_builder_set_with_row_id(builder, true) == 0,
           "enable row ids");
    CHECK_OK();
    LanceSqlQuery *query = NULL;
    ASSERT(lance_sql_query_builder_build(builder, &query) == 0,
           "plan row-id query");
    CHECK_OK();
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_sql_query_into_stream(query, &stream) == 0,
           "export row-id stream");
    CHECK_OK();
    ASSERT(stream_field_count(&stream) == 2, "id plus _rowid");
    /* Native appends the system column after the projected fields. */
    uint64_t row_ids[4];
    size_t count = collect_u64_column(&stream, 1, row_ids, 4);
    ASSERT(count == 3, "three live rows below id 5");
    ASSERT(row_ids[0] == 0 && row_ids[1] == 1 && row_ids[2] == 3,
           "_rowid follows stable insertion order");
    if (stream.release) stream.release(&stream);

    /* Aggregates collapse rows, so native must not inject system columns. */
    builder = lance_dataset_sql_builder_new(
        dataset, "SELECT COUNT(*) AS n FROM dataset");
    ASSERT(builder != NULL, "create aggregate builder");
    CHECK_OK();
    ASSERT(lance_sql_query_builder_set_with_row_id(builder, true) == 0,
           "enable row ids for aggregate");
    CHECK_OK();
    ASSERT(lance_sql_query_builder_set_with_row_addr(builder, true) == 0,
           "enable row addresses for aggregate");
    CHECK_OK();
    ASSERT(lance_sql_query_builder_build(builder, &query) == 0,
           "plan aggregate query");
    CHECK_OK();
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_sql_query_into_stream(query, &stream) == 0,
           "export aggregate stream");
    CHECK_OK();
    ASSERT(stream_field_count(&stream) == 1,
           "aggregates must not receive injected system columns");
    struct ArrowArray batch;
    memset(&batch, 0, sizeof(batch));
    ASSERT(stream.get_next(&stream, &batch) == 0, "read aggregate batch");
    ASSERT(batch.release != NULL, "aggregate batch present");
    ASSERT(batch.length == 1, "single aggregate row");
    const int64_t *counts = (const int64_t *)batch.children[0]->buffers[1];
    ASSERT(counts[0] == 6, "COUNT(*) skips the deleted row");
    batch.release(&batch);
    memset(&batch, 0, sizeof(batch));
    ASSERT(stream.get_next(&stream, &batch) == 0, "aggregate stream ends");
    ASSERT(batch.release == NULL, "aggregate stream has one batch");
    if (stream.release) stream.release(&stream);

    lance_dataset_close(dataset);
}

static void test_blob_modes(const char *uri) {
    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open blob dataset");
    CHECK_OK();

    /* ALL_BINARY materializes payload bytes; the stream must stay valid after
     * the dataset handle closes. */
    LanceSqlQueryBuilder *builder = lance_dataset_sql_builder_new(
        dataset, "SELECT * FROM dataset ORDER BY id");
    ASSERT(builder != NULL, "create blob builder");
    CHECK_OK();
    ASSERT(lance_sql_query_builder_set_blob_handling(
               builder, LANCE_BLOB_HANDLING_ALL_BINARY) == 0,
           "select ALL_BINARY");
    CHECK_OK();
    LanceSqlQuery *query = NULL;
    ASSERT(lance_sql_query_builder_build(builder, &query) == 0,
           "plan blob query");
    CHECK_OK();
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_sql_query_into_stream(query, &stream) == 0,
           "export blob stream");
    CHECK_OK();
    ASSERT(stream_field_count(&stream) == 2, "id and blob columns");
    lance_dataset_close(dataset);
    struct ArrowArray batch;
    memset(&batch, 0, sizeof(batch));
    ASSERT(stream.get_next(&stream, &batch) == 0, "read blob batch");
    ASSERT(batch.release != NULL, "blob batch present");
    ASSERT(batch.length == 3, "three blob rows");
    ASSERT(batch.children[1]->n_buffers == 3,
           "materialized blobs are LargeBinary offsets/values");
    const int64_t *offsets = (const int64_t *)batch.children[1]->buffers[1];
    const char *values = (const char *)batch.children[1]->buffers[2];
    ASSERT(offsets[1] - offsets[0] == 11, "first blob payload length");
    ASSERT(memcmp(values + offsets[0], "inline-blob", 11) == 0,
           "first blob payload");
    batch.release(&batch);
    if (stream.release) stream.release(&stream);

    /* Descriptions keep the native struct layout for the blob column. */
    dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "reopen blob dataset");
    CHECK_OK();
    builder = lance_dataset_sql_builder_new(
        dataset, "SELECT * FROM dataset ORDER BY id");
    ASSERT(builder != NULL, "create descriptions builder");
    CHECK_OK();
    ASSERT(lance_sql_query_builder_set_blob_handling(
               builder, LANCE_BLOB_HANDLING_ALL_DESCRIPTIONS) == 0,
           "select ALL_DESCRIPTIONS");
    CHECK_OK();
    ASSERT(lance_sql_query_builder_build(builder, &query) == 0,
           "plan descriptions query");
    CHECK_OK();
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_sql_query_into_stream(query, &stream) == 0,
           "export descriptions stream");
    CHECK_OK();
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(stream.get_schema(&stream, &schema) == 0, "read descriptions schema");
    ASSERT(schema.n_children == 2, "id and blob columns");
    ASSERT(schema.children[1]->format[0] == '+',
           "descriptions use a struct layout");
    schema.release(&schema);
    if (stream.release) stream.release(&stream);

    /* Foreign enum values and NULL pointers are rejected as invalid input. */
    builder = lance_dataset_sql_builder_new(dataset, "SELECT id FROM dataset");
    ASSERT(builder != NULL, "create enum guard builder");
    CHECK_OK();
    ASSERT(lance_sql_query_builder_set_blob_handling(builder, (LanceBlobHandling)7) != 0,
           "invalid blob handling rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "invalid blob handling is invalid input");
    lance_free_string(lance_last_error_message());
    ASSERT(lance_sql_query_builder_set_batch_size(builder, 2) == 0,
           "builder usable after rejected enum");
    CHECK_OK();
    ASSERT(lance_sql_query_builder_build(builder, &query) == 0,
           "default blob handling survives the rejected enum");
    CHECK_OK();
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_sql_query_into_stream(query, &stream) == 0,
           "export post-guard stream");
    CHECK_OK();
    if (stream.release) stream.release(&stream);

    ASSERT(lance_dataset_sql_builder_new(NULL, "SELECT 1") == NULL,
           "NULL dataset rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL dataset is invalid input");
    lance_free_string(lance_last_error_message());
    ASSERT(lance_dataset_sql_builder_new(dataset, NULL) == NULL,
           "NULL SQL rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL SQL is invalid input");
    lance_free_string(lance_last_error_message());

    lance_dataset_close(dataset);
}

static void test_planning_and_runtime_errors(const char *uri) {
    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open sql dataset");
    CHECK_OK();

    const char *bad_queries[] = {
        "SELECT * FROM missing_table",
        "SELECT missing_column FROM dataset",
        "SELEC broken syntax",
    };
    for (size_t i = 0; i < sizeof(bad_queries) / sizeof(bad_queries[0]); i++) {
        LanceSqlQueryBuilder *builder =
            lance_dataset_sql_builder_new(dataset, bad_queries[i]);
        ASSERT(builder != NULL, "create malformed builder");
        CHECK_OK();
        LanceSqlQuery *query = NULL;
        ASSERT(lance_sql_query_builder_build(builder, &query) != 0,
               "malformed query fails at build");
        ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
               "malformed query is invalid input");
        lance_free_string(lance_last_error_message());
        ASSERT(query == NULL, "failed planning publishes no query");
    }

    /* Execution errors surface while reading the stream, not at export. */
    LanceSqlQueryBuilder *builder = lance_dataset_sql_builder_new(
        dataset, "SELECT id / (id - id) AS boom FROM dataset");
    ASSERT(builder != NULL, "create runtime-error builder");
    CHECK_OK();
    LanceSqlQuery *query = NULL;
    ASSERT(lance_sql_query_builder_build(builder, &query) == 0,
           "runtime error plans successfully");
    CHECK_OK();
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_sql_query_into_stream(query, &stream) == 0,
           "runtime error exports a stream");
    CHECK_OK();
    struct ArrowArray batch;
    memset(&batch, 0, sizeof(batch));
    ASSERT(stream.get_next(&stream, &batch) != 0,
           "division by zero fails while streaming");
    ASSERT(batch.release == NULL, "failed batch is not published");
    if (stream.release) stream.release(&stream);

    /* The query allocation is consumed even for a NULL stream slot. */
    builder = lance_dataset_sql_builder_new(dataset, "SELECT id FROM dataset");
    ASSERT(builder != NULL, "create NULL-slot builder");
    CHECK_OK();
    ASSERT(lance_sql_query_builder_build(builder, &query) == 0,
           "plan NULL-slot query");
    CHECK_OK();
    ASSERT(lance_sql_query_into_stream(query, NULL) != 0,
           "NULL stream slot rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL stream slot is invalid input");
    lance_free_string(lance_last_error_message());

    /* Handles still owned by the caller close NULL-safely. */
    lance_sql_query_close(NULL);
    lance_sql_query_builder_close(NULL);
    lance_dataset_close(dataset);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <sql-dataset> <history-dataset> <blob-dataset>\n",
                argv[0]);
        return 2;
    }
    test_projection_filter_order(argv[1]);
    test_renamed_table(argv[1]);
    test_system_columns(argv[2]);
    test_blob_modes(argv[3]);
    test_planning_and_runtime_errors(argv[1]);
    printf("test_sql.c: all checks passed\n");
    return 0;
}
