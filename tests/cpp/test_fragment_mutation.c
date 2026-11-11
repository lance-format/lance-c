// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include "lance/lance.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ArrowSchema ArrowSchema;
typedef struct ArrowArray ArrowArray;
typedef struct ArrowArrayStream ArrowArrayStream;

#define ARROW_FLAG_NULLABLE 2

#define ASSERT(condition, message)                                             \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "ASSERT failed %s:%d: %s\n", __FILE__, __LINE__,  \
                    (message));                                                \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static void check_ok(const char *context) {
    LanceErrorCode code = lance_last_error_code();
    if (code == LANCE_OK) return;
    const char *message = lance_last_error_message();
    fprintf(stderr, "%s failed (%d): %s\n", context, (int)code,
            message ? message : "unknown error");
    if (message) lance_free_string(message);
    exit(1);
}

static int64_t bits_value(const ArrowArray *array, int64_t row) {
    if (array->null_count == 0 || array->buffers == NULL ||
        array->buffers[0] == NULL) {
        return 1;
    }
    const uint8_t *bits = (const uint8_t *)array->buffers[0];
    int64_t index = array->offset + row;
    return (bits[index / 8] >> (index % 8)) & 1;
}

static int64_t integer_value(const ArrowArray *array, int64_t row,
                             const char *format) {
    int64_t index = array->offset + row;
    if (strcmp(format, "i") == 0) {
        return ((const int32_t *)array->buffers[1])[index];
    }
    if (strcmp(format, "l") == 0) {
        return ((const int64_t *)array->buffers[1])[index];
    }
    if (strcmp(format, "L") == 0) {
        return (int64_t)((const uint64_t *)array->buffers[1])[index];
    }
    fprintf(stderr, "unsupported integer format %s\n", format);
    exit(1);
}

static int child_index(const ArrowSchema *schema, const char *name) {
    for (int64_t i = 0; i < schema->n_children; ++i) {
        if (strcmp(schema->children[i]->name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int find_value_in_stream(ArrowArrayStream *stream, const char *column,
                                int64_t wanted_id, int *found, int *is_null,
                                int64_t *value) {
    ArrowSchema schema = {0};
    if (stream->get_schema(stream, &schema) != 0) return -1;
    int id_index = child_index(&schema, "id");
    int value_index = child_index(&schema, column);
    if (id_index < 0 || value_index < 0) {
        if (schema.release) schema.release(&schema);
        return -1;
    }
    const char *id_format = schema.children[id_index]->format;
    const char *value_format = schema.children[value_index]->format;

    *found = 0;
    *is_null = 0;
    *value = 0;
    while (1) {
        ArrowArray batch = {0};
        if (stream->get_next(stream, &batch) != 0) {
            if (schema.release) schema.release(&schema);
            return -1;
        }
        if (batch.release == NULL) break;
        for (int64_t row = 0; row < batch.length; ++row) {
            int64_t id = integer_value(batch.children[id_index], row, id_format);
            if (id != wanted_id) continue;
            *found = 1;
            const ArrowArray *value_array = batch.children[value_index];
            if (!bits_value(value_array, row)) {
                *is_null = 1;
            } else {
                *value = integer_value(value_array, row, value_format);
            }
            break;
        }
        if (batch.release) batch.release(&batch);
        if (*found) break;
    }
    if (schema.release) schema.release(&schema);
    return 0;
}

static void scan_dataset_value(const char *uri, const char *column,
                               int64_t wanted_id, int *found, int *is_null,
                               int64_t *value) {
    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open dataset for scan");
    check_ok("lance_dataset_open");
    LanceScanner *scanner = lance_scanner_new(dataset, NULL, NULL);
    ASSERT(scanner != NULL, "create scanner");
    check_ok("lance_scanner_new");
    if (strcmp(column, "_rowid") == 0) {
        ASSERT(lance_scanner_with_row_id(scanner, true) == 0, "project row IDs");
        check_ok("project row IDs");
    }
    ArrowArrayStream stream = {0};
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "export scanner stream");
    check_ok("lance_scanner_to_arrow_stream");
    ASSERT(find_value_in_stream(&stream, column, wanted_id, found, is_null,
                                value) == 0,
           "scan projected value");
    if (stream.release) stream.release(&stream);
    lance_scanner_close(scanner);
    lance_dataset_close(dataset);
}

static void scan_fragment_value(const LanceFragment *fragment,
                                const char *column, int64_t wanted_id,
                                int *found, int *is_null, int64_t *value) {
    LanceScanner *scanner = lance_fragment_scan(fragment);
    ASSERT(scanner != NULL, "create fragment scanner");
    check_ok("lance_fragment_scan");
    ArrowArrayStream stream = {0};
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "export fragment stream");
    check_ok("fragment scanner export");
    ASSERT(find_value_in_stream(&stream, column, wanted_id, found, is_null,
                                value) == 0,
           "scan fragment value");
    if (stream.release) stream.release(&stream);
    lance_scanner_close(scanner);
}

static void release_schema(ArrowSchema *schema) { schema->release = NULL; }
static void release_array(ArrowArray *array) { array->release = NULL; }

/* Stream descriptors live with the caller. Exported schemas and batches share
 * backing storage so each can outlive the stream and the other outputs. */
typedef struct {
    atomic_size_t references;
} StreamStorage;

static void retain_storage(StreamStorage *storage) {
    atomic_fetch_add(&storage->references, 1);
}

static void release_storage(StreamStorage *storage) {
    if (atomic_fetch_sub(&storage->references, 1) == 1) free(storage);
}

static void release_owned_schema(ArrowSchema *schema) {
    StreamStorage *storage = (StreamStorage *)schema->private_data;
    schema->release = NULL;
    schema->private_data = NULL;
    release_storage(storage);
}

static void release_owned_array(ArrowArray *array) {
    StreamStorage *storage = (StreamStorage *)array->private_data;
    array->release = NULL;
    array->private_data = NULL;
    release_storage(storage);
}

typedef struct {
    StreamStorage storage; /* First member so the last owner frees the allocation. */
    ArrowSchema schema;
    ArrowSchema child_schema[2];
    ArrowSchema *schema_children[2];
    ArrowArray batch;
    ArrowArray child_array[2];
    ArrowArray *batch_children[2];
    const void *root_buffers[1];
    const void *child0_buffers[2];
    const void *child1_buffers[2];
    int64_t ids[8];
    int32_t values[8];
    size_t len;
    int emitted;
} TwoColumnStream;

typedef struct {
    StreamStorage storage;
    ArrowSchema schema;
    ArrowSchema child_schema;
    ArrowSchema *schema_children[1];
    ArrowArray batch;
    ArrowArray child_array;
    ArrowArray *batch_children[1];
    const void *root_buffers[1];
    const void *child_buffers[2];
    int32_t values[8];
    size_t len;
    int emitted;
} OneColumnStream;

static int two_stream_get_schema(ArrowArrayStream *stream, ArrowSchema *out);
static int two_stream_get_next(ArrowArrayStream *stream, ArrowArray *out);
static int one_stream_get_schema(ArrowArrayStream *stream, ArrowSchema *out);
static int one_stream_get_next(ArrowArrayStream *stream, ArrowArray *out);
static const char *stream_last_error(ArrowArrayStream *stream);
static void two_stream_release(ArrowArrayStream *stream);
static void one_stream_release(ArrowArrayStream *stream);

static int two_stream_get_schema(ArrowArrayStream *stream, ArrowSchema *out) {
    if (stream->private_data == NULL) return -1;
    TwoColumnStream *data = (TwoColumnStream *)stream->private_data;
    retain_storage(&data->storage);
    *out = data->schema;
    return 0;
}

static int two_stream_get_next(ArrowArrayStream *stream, ArrowArray *out) {
    if (stream->private_data == NULL) return -1;
    TwoColumnStream *data = (TwoColumnStream *)stream->private_data;
    memset(out, 0, sizeof(*out));
    if (data->emitted) return 0;
    retain_storage(&data->storage);
    *out = data->batch;
    data->emitted = 1;
    return 0;
}

static int one_stream_get_schema(ArrowArrayStream *stream, ArrowSchema *out) {
    if (stream->private_data == NULL) return -1;
    OneColumnStream *data = (OneColumnStream *)stream->private_data;
    retain_storage(&data->storage);
    *out = data->schema;
    return 0;
}

static int one_stream_get_next(ArrowArrayStream *stream, ArrowArray *out) {
    if (stream->private_data == NULL) return -1;
    OneColumnStream *data = (OneColumnStream *)stream->private_data;
    memset(out, 0, sizeof(*out));
    if (data->emitted) return 0;
    retain_storage(&data->storage);
    *out = data->batch;
    data->emitted = 1;
    return 0;
}

static const char *stream_last_error(ArrowArrayStream *stream) {
    (void)stream;
    return NULL;
}

static void two_stream_release(ArrowArrayStream *stream) {
    if (stream == NULL) return;
    TwoColumnStream *data = (TwoColumnStream *)stream->private_data;
    stream->release = NULL;
    stream->private_data = NULL;
    release_storage(&data->storage);
}

static void one_stream_release(ArrowArrayStream *stream) {
    if (stream == NULL) return;
    OneColumnStream *data = (OneColumnStream *)stream->private_data;
    stream->release = NULL;
    stream->private_data = NULL;
    release_storage(&data->storage);
}

static ArrowArrayStream two_column_stream(const char *left_name,
                                          const char *left_format,
                                          const char *right_name,
                                          const char *right_format,
                                          const int64_t *ids,
                                          const int32_t *values, size_t len) {
    ASSERT(len <= 8, "two-column stream length");
    TwoColumnStream *data = (TwoColumnStream *)calloc(1, sizeof(*data));
    ASSERT(data != NULL, "allocate two-column stream");
    atomic_init(&data->storage.references, 1);
    data->len = len;
    memcpy(data->ids, ids, len * sizeof(int64_t));
    memcpy(data->values, values, len * sizeof(int32_t));

    data->schema.format = "+s";
    data->schema.name = "";
    data->schema.n_children = 2;
    data->schema.release = release_owned_schema;
    data->schema.private_data = &data->storage;
    data->schema_children[0] = &data->child_schema[0];
    data->schema_children[1] = &data->child_schema[1];
    data->schema.children = data->schema_children;

    data->child_schema[0].format = left_format;
    data->child_schema[0].name = left_name;
    data->child_schema[0].flags = 0;
    data->child_schema[0].release = release_schema;
    data->child_schema[1].format = right_format;
    data->child_schema[1].name = right_name;
    data->child_schema[1].flags = ARROW_FLAG_NULLABLE;
    data->child_schema[1].release = release_schema;

    data->root_buffers[0] = NULL;
    data->child0_buffers[0] = NULL;
    data->child0_buffers[1] = data->ids;
    data->child1_buffers[0] = NULL;
    data->child1_buffers[1] = data->values;

    data->child_array[0].length = (int64_t)len;
    data->child_array[0].n_buffers = 2;
    data->child_array[0].buffers = data->child0_buffers;
    data->child_array[0].release = release_array;
    data->child_array[1].length = (int64_t)len;
    data->child_array[1].n_buffers = 2;
    data->child_array[1].buffers = data->child1_buffers;
    data->child_array[1].release = release_array;
    data->batch_children[0] = &data->child_array[0];
    data->batch_children[1] = &data->child_array[1];

    data->batch.length = (int64_t)len;
    data->batch.n_buffers = 1;
    data->batch.buffers = data->root_buffers;
    data->batch.n_children = 2;
    data->batch.children = data->batch_children;
    data->batch.release = release_owned_array;
    data->batch.private_data = &data->storage;
    return (ArrowArrayStream){
        .get_schema = two_stream_get_schema,
        .get_next = two_stream_get_next,
        .get_last_error = stream_last_error,
        .release = two_stream_release,
        .private_data = data,
    };
}

static ArrowArrayStream one_column_stream(const char *name, const char *format,
                                          const int32_t *values, size_t len) {
    ASSERT(len <= 8, "one-column stream length");
    ASSERT(strcmp(format, "i") == 0, "one-column stream int32");
    OneColumnStream *data = (OneColumnStream *)calloc(1, sizeof(*data));
    ASSERT(data != NULL, "allocate one-column stream");
    atomic_init(&data->storage.references, 1);
    data->len = len;
    memcpy(data->values, values, len * sizeof(int32_t));

    data->schema.format = "+s";
    data->schema.name = "";
    data->schema.n_children = 1;
    data->schema.release = release_owned_schema;
    data->schema.private_data = &data->storage;
    data->schema_children[0] = &data->child_schema;
    data->schema.children = data->schema_children;
    data->child_schema.format = format;
    data->child_schema.name = name;
    data->child_schema.flags = ARROW_FLAG_NULLABLE;
    data->child_schema.release = release_schema;

    data->root_buffers[0] = NULL;
    data->child_buffers[0] = NULL;
    data->child_buffers[1] = data->values;
    data->child_array.length = (int64_t)len;
    data->child_array.n_buffers = 2;
    data->child_array.buffers = data->child_buffers;
    data->child_array.release = release_array;
    data->batch_children[0] = &data->child_array;

    data->batch.length = (int64_t)len;
    data->batch.n_buffers = 1;
    data->batch.buffers = data->root_buffers;
    data->batch.n_children = 1;
    data->batch.children = data->batch_children;
    data->batch.release = release_owned_array;
    data->batch.private_data = &data->storage;
    return (ArrowArrayStream){
        .get_schema = one_stream_get_schema,
        .get_next = one_stream_get_next,
        .get_last_error = stream_last_error,
        .release = one_stream_release,
        .private_data = data,
    };
}

static void test_stream_output_lifetimes(void) {
    const int64_t ids[] = {7, 9};
    const int32_t values[] = {70, 90};
    ArrowArrayStream two =
        two_column_stream("id", "l", "value", "i", ids, values, 2);
    ArrowSchema schema = {0};
    ArrowArray batch = {0};
    ASSERT(two.get_schema(&two, &schema) == 0, "export schema");
    ASSERT(two.get_next(&two, &batch) == 0, "export batch");
    two.release(&two);
    ASSERT(integer_value(batch.children[0], 1, "l") == 9,
           "batch remains readable after stream release");
    batch.release(&batch);
    ASSERT(strcmp(schema.children[1]->name, "value") == 0,
           "schema remains readable after stream and batch release");
    schema.release(&schema);
    ASSERT(two.release == NULL && two.private_data == NULL,
           "released stream descriptor remains readable after storage release");

    ArrowArrayStream one = one_column_stream("value", "i", values, 2);
    ASSERT(one.get_schema(&one, &schema) == 0, "export schema");
    ASSERT(one.get_next(&one, &batch) == 0, "export batch");
    schema.release(&schema);
    one.release(&one);
    ASSERT(integer_value(batch.children[0], 1, "i") == 90,
           "batch remains readable after schema and stream release");
    batch.release(&batch);
    ASSERT(one.release == NULL && one.private_data == NULL,
           "released stream descriptor remains readable after final batch release");
}

static void commit_operation(const char *uri, uint64_t read_version,
                             LanceOperation *operation) {
    LanceTransaction *transaction =
        lance_transaction_new(read_version, operation, NULL);
    ASSERT(transaction != NULL, "construct transaction");
    check_ok("lance_transaction_new");
    LanceDataset *committed = NULL;
    ASSERT(lance_transaction_commit(uri, transaction, NULL, NULL, NULL,
                                    &committed) == 0,
           "commit fragment mutation");
    check_ok("lance_transaction_commit");
    ASSERT(committed != NULL, "commit returned dataset");
    lance_dataset_close(committed);
    lance_transaction_close(transaction);
    lance_operation_close(operation);
}

static void open_fragments(const char *uri, uint64_t *ids,
                           LanceFragment **fragments) {
    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open fixture");
    check_ok("open fixture");
    ASSERT(lance_dataset_fragment_count(dataset) == 3, "three fixture fragments");
    check_ok("fragment count");
    ASSERT(lance_dataset_fragment_ids(dataset, ids) == 0, "fragment IDs");
    check_ok("fragment ids");
    for (int i = 0; i < 3; ++i) {
        fragments[i] = lance_dataset_open_fragment(dataset, ids[i]);
        ASSERT(fragments[i] != NULL, "open fragment");
        check_ok("open fragment");
    }
    lance_dataset_close(dataset);
}

static void test_add_columns_sql(const char *uri) {
    uint64_t ids[3] = {0};
    LanceFragment *fragments[3] = {0};
    open_fragments(uri, ids, fragments);
    uint64_t read_version = 0;
    ASSERT(lance_fragment_read_version(fragments[0], &read_version) == 0,
           "fragment read version");
    check_ok("fragment read version");
    ASSERT(read_version == 5, "fixture read version");

    LanceSqlColumn sql = {"sql_value", "value * 2 + 1"};
    LanceFragment *updated = NULL;
    LanceSchema *schema = NULL;
    ASSERT(lance_fragment_add_columns_sql(fragments[0], &sql, 1, NULL,
                                          &updated, &schema) == 0,
           "fragment SQL add_columns");
    check_ok("fragment SQL add_columns");
    ASSERT(updated != NULL && schema != NULL, "SQL outputs");
    bool is_bound = true;
    ASSERT(lance_fragment_is_bound(updated, &is_bound) == 0 && !is_bound,
           "add_columns output is detached");
    check_ok("add_columns bound state");
    int32_t sql_field_id = -1;
    ASSERT(lance_schema_field_id(schema, "sql_value", &sql_field_id) == 0 &&
               sql_field_id > 5,
           "SQL field identity");
    check_ok("SQL field identity");
    int32_t nested_field_id = -1;
    ASSERT(lance_schema_field_id(schema, "profile.score", &nested_field_id) ==
               0 &&
               nested_field_id == 4,
           "nested field identity");
    check_ok("nested field identity");

    const LanceFragment *final_fragments[3] = {updated, fragments[1],
                                               fragments[2]};
    LanceOperation *operation =
        lance_operation_merge_new(schema, final_fragments, 3, false);
    ASSERT(operation != NULL, "construct Merge");
    check_ok("construct Merge");
    commit_operation(uri, read_version, operation);

    int found = 0;
    int is_null = 0;
    int64_t value = 0;
    scan_dataset_value(uri, "sql_value", 1, &found, &is_null, &value);
    ASSERT(found && !is_null && value == 21, "id 1 SQL value");
    scan_dataset_value(uri, "sql_value", 3, &found, &is_null, &value);
    ASSERT(found && !is_null && value == 61, "id 3 SQL value");
    scan_dataset_value(uri, "sql_value", 4, &found, &is_null, &value);
    ASSERT(found && !is_null && value == 81, "id 4 SQL value");
    scan_dataset_value(uri, "sql_value", 5, &found, &is_null, &value);
    ASSERT(found && is_null, "other fragment reads null SQL value");

    uint64_t logical_rows = 0;
    ASSERT(lance_fragment_logical_rows(fragments[0], &logical_rows) == 0 &&
               logical_rows == 3,
           "source fragment unchanged");
    check_ok("source fragment logical rows");
    lance_schema_close(schema);
    lance_fragment_close(updated);
    for (int i = 0; i < 3; ++i) lance_fragment_close(fragments[i]);
    printf("fragment SQL add -> typed Merge -> reopen OK\n");
}

static void test_merge_columns(const char *uri) {
    uint64_t ids[3] = {0};
    LanceFragment *fragments[3] = {0};
    open_fragments(uri, ids, fragments);
    uint64_t read_version = 0;
    ASSERT(lance_fragment_read_version(fragments[0], &read_version) == 0,
           "merge read version");
    check_ok("merge read version");

    int64_t join_ids[4] = {1, 2, 3, 4};
    int32_t join_values[4] = {11, 22, 33, 44};
    ArrowArrayStream stream =
        two_column_stream("id", "l", "joined", "i", join_ids, join_values, 4);
    LanceFragment *updated = NULL;
    LanceSchema *schema = NULL;
    ASSERT(lance_fragment_merge_columns(fragments[0], &stream, "id",
                                        "id", &updated, &schema) == 0,
           "fragment hash merge");
    check_ok("fragment hash merge");
    ASSERT(stream.release == NULL, "merge consumed stream");

    const LanceFragment *final_fragments[3] = {updated, fragments[1],
                                               fragments[2]};
    LanceOperation *operation =
        lance_operation_merge_new(schema, final_fragments, 3, false);
    ASSERT(operation != NULL, "construct merge operation");
    check_ok("merge operation");
    commit_operation(uri, read_version, operation);

    int found = 0;
    int is_null = 0;
    int64_t value = 0;
    scan_dataset_value(uri, "joined", 1, &found, &is_null, &value);
    ASSERT(found && !is_null && value == 11, "joined id 1");
    scan_dataset_value(uri, "joined", 3, &found, &is_null, &value);
    ASSERT(found && !is_null && value == 33, "joined id 3");
    scan_dataset_value(uri, "joined", 5, &found, &is_null, &value);
    ASSERT(found && is_null, "unmerged fragment reads null");

    lance_schema_close(schema);
    lance_fragment_close(updated);
    for (int i = 0; i < 3; ++i) lance_fragment_close(fragments[i]);
    printf("fragment hash merge -> typed Merge -> reopen OK\n");
}

static void test_update_columns(const char *uri) {
    uint64_t ids[3] = {0};
    LanceFragment *fragments[3] = {0};
    open_fragments(uri, ids, fragments);
    uint64_t read_version = 0;
    ASSERT(lance_fragment_read_version(fragments[0], &read_version) == 0,
           "update read version");
    check_ok("update read version");

    int64_t update_ids[2] = {1, 3};
    int32_t update_values[2] = {901, 903};
    ArrowArrayStream stream =
        two_column_stream("id", "l", "value", "i", update_ids, update_values,
                          2);
    LanceFragmentUpdateColumnsResult *result = NULL;
    ASSERT(lance_fragment_update_columns(fragments[0], &stream, "id",
                                         "id", &result) == 0,
           "fragment update_columns");
    check_ok("fragment update_columns");
    ASSERT(stream.release == NULL, "update consumed stream");

    LanceFragment *updated = NULL;
    ASSERT(lance_fragment_update_columns_result_fragment(result, &updated) == 0,
           "clone updated fragment");
    check_ok("updated fragment clone");
    uint64_t field_count = 0;
    ASSERT(lance_fragment_update_columns_result_fields_modified_count(
               result, &field_count) == 0 &&
               field_count == 1,
           "updated field count");
    check_ok("updated field count");
    uint32_t modified_field = UINT32_MAX;
    ASSERT(lance_fragment_update_columns_result_fields_modified_at(
               result, 0, &modified_field) == 0 &&
               modified_field == 1,
           "updated field ID");
    check_ok("updated field ID");
    uint64_t offset_count = 0;
    ASSERT(lance_fragment_update_columns_result_matched_offset_count(
               result, &offset_count) == 0 &&
               offset_count == 2,
           "matched offset count");
    check_ok("matched offset count");
    uint32_t offsets[2] = {UINT32_MAX, UINT32_MAX};
    for (size_t i = 0; i < 2; ++i) {
        ASSERT(lance_fragment_update_columns_result_matched_offset_at(
                   result, i, &offsets[i]) == 0,
               "matched offset");
        check_ok("matched offset");
    }
    ASSERT(offsets[0] == 0 && offsets[1] == 2, "matched physical offsets");
    uint32_t copied_offsets[2] = {UINT32_MAX, UINT32_MAX};
    ASSERT(lance_fragment_update_columns_result_matched_offsets(
               result, copied_offsets, 2) == 0,
           "copy matched offsets");
    check_ok("copy matched offsets");
    ASSERT(memcmp(offsets, copied_offsets, sizeof(offsets)) == 0,
           "bulk offsets match indexed offsets");

    LanceFragmentOffsetsView entry = {0, copied_offsets, 2};
    LanceUpdatedFragmentOffsets *offset_owner =
        lance_updated_fragment_offsets_new(&entry, 1);
    ASSERT(offset_owner != NULL, "construct UpdatedFragmentOffsets");
    check_ok("UpdatedFragmentOffsets");
    const LanceFragment *updated_fragments[1] = {updated};
    const uint32_t fields_modified[1] = {modified_field};
    LanceUpdateOperationOptions options;
    memset(&options, 0, sizeof(options));
    options.updated_fragments = updated_fragments;
    options.updated_fragment_count = 1;
    options.fields_modified = fields_modified;
    options.fields_modified_count = 1;
    options.has_update_mode = true;
    options.update_mode = LANCE_UPDATE_MODE_REWRITE_COLUMNS;
    options.updated_fragment_offsets = offset_owner;
    LanceOperation *operation = lance_operation_update_new(&options);
    ASSERT(operation != NULL, "construct Update");
    check_ok("Update operation");
    commit_operation(uri, read_version, operation);

    int found = 0;
    int is_null = 0;
    int64_t value = 0;
    scan_dataset_value(uri, "value", 1, &found, &is_null, &value);
    ASSERT(found && !is_null && value == 901, "updated id 1");
    scan_dataset_value(uri, "value", 3, &found, &is_null, &value);
    ASSERT(found && !is_null && value == 903, "updated id 3");
    scan_dataset_value(uri, "value", 4, &found, &is_null, &value);
    ASSERT(found && !is_null && value == 40, "untouched id 4");
    scan_dataset_value(uri, "value", 2, &found, &is_null, &value);
    ASSERT(!found, "deleted id 2 stays deleted");

    LanceDataset *reopened = lance_dataset_open(uri, NULL, 0);
    ASSERT(reopened != NULL, "reopen update dataset");
    check_ok("reopen update dataset");
    bool stable = false;
    ASSERT(lance_dataset_has_stable_row_ids(reopened, &stable) == 0 && stable,
           "stable row IDs remain");
    check_ok("stable row IDs");
    lance_dataset_close(reopened);

    lance_updated_fragment_offsets_close(offset_owner);
    lance_fragment_update_columns_result_close(result);
    lance_fragment_close(updated);
    for (int i = 0; i < 3; ++i) lance_fragment_close(fragments[i]);
    printf("fragment update offsets -> typed Update -> reopen OK\n");
}

static void test_write_columns(const char *uri) {
    uint64_t ids[3] = {0};
    LanceFragment *fragments[3] = {0};
    open_fragments(uri, ids, fragments);
    uint64_t read_version = 0;
    ASSERT(lance_fragment_read_version(fragments[0], &read_version) == 0,
           "write read version");
    check_ok("write read version");

    LanceSchema *full_schema = NULL;
    ASSERT(lance_fragment_schema_clone(fragments[0], &full_schema) == 0,
           "clone fragment schema");
    check_ok("clone fragment schema");
    const char *paths[1] = {"value"};
    LanceSchema *value_schema = NULL;
    ASSERT(lance_schema_project(full_schema, paths, 1, &value_schema) == 0,
           "project value schema");
    check_ok("project value schema");

    int32_t replacement[4] = {111, 222, 333, 444};
    ArrowArrayStream stream = one_column_stream("value", "i", replacement, 4);
    LanceDataReplacementGroup *group = NULL;
    ASSERT(lance_fragment_write_columns(fragments[0], value_schema,
                                        &stream, &group) == 0,
           "fragment write_columns");
    check_ok("fragment write_columns");
    ASSERT(stream.release == NULL, "write consumed stream");

    const LanceDataReplacementGroup *groups[1] = {group};
    LanceOperation *operation =
        lance_operation_data_replacement_new(groups, 1);
    ASSERT(operation != NULL, "construct DataReplacement");
    check_ok("DataReplacement operation");
    commit_operation(uri, read_version, operation);

    int found = 0;
    int is_null = 0;
    int64_t value = 0;
    scan_dataset_value(uri, "value", 1, &found, &is_null, &value);
    ASSERT(found && !is_null && value == 111, "replacement id 1");
    scan_dataset_value(uri, "value", 3, &found, &is_null, &value);
    ASSERT(found && !is_null && value == 333, "replacement id 3");
    scan_dataset_value(uri, "value", 4, &found, &is_null, &value);
    ASSERT(found && !is_null && value == 444, "replacement id 4");
    scan_dataset_value(uri, "value", 5, &found, &is_null, &value);
    ASSERT(found && !is_null && value == 50, "replacement leaves other fragment");

    scan_fragment_value(fragments[0], "value", 1, &found, &is_null, &value);
    ASSERT(found && !is_null && value == 10, "old fragment snapshot preserved");

    lance_data_replacement_group_close(group);
    lance_schema_close(value_schema);
    lance_schema_close(full_schema);
    for (int i = 0; i < 3; ++i) lance_fragment_close(fragments[i]);
    printf("fragment write_columns -> DataReplacement -> reopen OK\n");
}

static void test_delete_rows(const char *uri) {
    uint64_t ids[3] = {0};
    LanceFragment *fragments[3] = {0};
    open_fragments(uri, ids, fragments);
    uint64_t read_version = 0;
    ASSERT(lance_fragment_read_version(fragments[1], &read_version) == 0,
           "delete read version");
    check_ok("delete read version");

    uint32_t duplicated[2] = {0, 0};
    LanceFragment *partial = NULL;
    ASSERT(lance_fragment_delete_rows(fragments[1], duplicated, 2, &partial) ==
               0 &&
               partial != NULL,
           "partial offset deletion");
    check_ok("partial offset deletion");
    uint64_t logical_rows = 0;
    ASSERT(lance_fragment_logical_rows(partial, &logical_rows) == 0 &&
               logical_rows == 3,
           "duplicate offsets collapse natively");
    check_ok("partial logical rows");

    uint32_t all[4] = {0, 1, 2, 3};
    LanceFragment *full = (LanceFragment *)(uintptr_t)0x1;
    ASSERT(lance_fragment_delete_rows(fragments[2], all, 4, &full) == 0 &&
               full == NULL,
           "full offset deletion returns NULL");
    check_ok("full offset deletion");

    const LanceFragment *updated[1] = {partial};
    const uint64_t deleted[1] = {ids[2]};
    LanceOperation *operation =
        lance_operation_delete_new(updated, 1, deleted, 1, "delete_rows");
    ASSERT(operation != NULL, "construct offset Delete");
    check_ok("offset Delete operation");
    commit_operation(uri, read_version, operation);

    LanceDataset *reopened = lance_dataset_open(uri, NULL, 0);
    ASSERT(reopened != NULL, "reopen delete dataset");
    check_ok("reopen delete dataset");
    ASSERT(lance_dataset_count_rows(reopened) == 6, "offset deletion live rows");
    check_ok("offset deletion live rows");
    lance_dataset_close(reopened);

    int found = 0;
    int is_null = 0;
    int64_t value = 0;
    scan_dataset_value(uri, "value", 5, &found, &is_null, &value);
    ASSERT(!found, "offset-deleted id 5 absent");
    scan_dataset_value(uri, "value", 6, &found, &is_null, &value);
    ASSERT(found && !is_null && value == 60, "id 6 remains");

    uint64_t source_rows = 0;
    ASSERT(lance_fragment_logical_rows(fragments[1], &source_rows) == 0 &&
               source_rows == 4,
           "delete source handle unchanged");
    check_ok("delete source handle");
    lance_fragment_close(partial);
    for (int i = 0; i < 3; ++i) lance_fragment_close(fragments[i]);
    printf("fragment offset Delete -> reopen OK\n");
}

int main(int argc, char **argv) {
    test_stream_output_lifetimes();
    if (argc != 6) {
        fprintf(stderr,
                "usage: %s ADD_URI MERGE_URI UPDATE_URI REPLACE_URI DELETE_URI\n",
                argv[0]);
        return 2;
    }
    test_add_columns_sql(argv[1]);
    test_merge_columns(argv[2]);
    test_update_columns(argv[3]);
    test_write_columns(argv[4]);
    test_delete_rows(argv[5]);
    for (int i = 1; i < argc; ++i) {
        int found = 0, is_null = 0;
        int64_t row_id = -1;
        scan_dataset_value(argv[i], "_rowid", 3, &found, &is_null, &row_id);
        ASSERT(found && !is_null && row_id == 2,
               "published mutation preserves the original stable row ID");
    }
    printf("all C11 fragment mutation publication checks passed\n");
    return 0;
}
