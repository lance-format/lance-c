/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#include "lance/lance.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(condition, message)                                             \
    do {                                                                       \
        if (!(condition)) {                                                     \
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
        if (lance_last_error_code() != LANCE_OK) {                              \
            const char *lance_message = lance_last_error_message();            \
            fprintf(stderr, "CHECK_OK failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    lance_message ? lance_message : "unknown error");           \
            if (lance_message) lance_free_string(lance_message);               \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

#define ROW_ADDRESS(fragment_id, offset)                                       \
    (((uint64_t)(fragment_id) << 32) | (uint64_t)(offset))

typedef struct OverlayBatch {
    struct ArrowSchema schema;
    struct ArrowSchema **schema_children;
    struct ArrowSchema *schema_storage;
    struct ArrowArray array;
    struct ArrowArray **array_children;
    struct ArrowArray *array_storage;
    uint64_t *rowaddrs;
    int32_t *a_values;
    uint8_t *a_validity;
    int32_t *b_values;
    uint8_t *b_validity;
    size_t len;
    int with_a;
    int with_b;
} OverlayBatch;

typedef struct Row {
    int found;
    int32_t id;
    int a_null;
    int32_t a;
    int b_null;
    int32_t b;
    uint64_t rowid;
} Row;

static void release_child_schema(struct ArrowSchema *schema) {
    schema->release = NULL;
}

static void release_root_schema(struct ArrowSchema *schema) {
    free(schema->children);
    free(schema->private_data);
    schema->children = NULL;
    schema->private_data = NULL;
    schema->release = NULL;
}

static void release_child_array(struct ArrowArray *array) {
    if (array->buffers) {
        free((void *)array->buffers[0]);
        free((void *)array->buffers[1]);
        free((void *)array->buffers);
    }
    array->buffers = NULL;
    array->release = NULL;
}

static void release_root_array(struct ArrowArray *array) {
    if (array->children) {
        for (int64_t i = 0; i < array->n_children; ++i) {
            if (array->children[i]->release) {
                array->children[i]->release(array->children[i]);
            }
        }
        free(array->children);
        free(array->private_data);
    }
    free((void *)array->buffers);
    array->children = NULL;
    array->buffers = NULL;
    array->release = NULL;
}

static void init_child_schema(struct ArrowSchema *schema, const char *format,
                              const char *name, int32_t flags) {
    memset(schema, 0, sizeof(*schema));
    schema->format = format;
    schema->name = name;
    schema->flags = flags;
    schema->release = release_child_schema;
}

static void init_child_array(struct ArrowArray *array, size_t len,
                             const void *validity, const void *values) {
    const void **buffers = calloc(2, sizeof(*buffers));
    ASSERT(buffers != NULL, "allocate child buffers");
    buffers[0] = validity;
    buffers[1] = values;
    memset(array, 0, sizeof(*array));
    array->length = (int64_t)len;
    array->n_buffers = 2;
    array->buffers = buffers;
    array->release = release_child_array;
}

static void init_overlay_batch(OverlayBatch *batch, size_t len, int with_a,
                               int with_b) {
    memset(batch, 0, sizeof(*batch));
    batch->len = len;
    batch->with_a = with_a;
    batch->with_b = with_b;
    size_t children = 1 + (size_t)with_a + (size_t)with_b;
    batch->schema_children = calloc(children, sizeof(*batch->schema_children));
    batch->schema_storage = calloc(children, sizeof(*batch->schema_storage));
    batch->array_children = calloc(children, sizeof(*batch->array_children));
    batch->array_storage = calloc(children, sizeof(*batch->array_storage));
    batch->rowaddrs = calloc(len, sizeof(*batch->rowaddrs));
    ASSERT(batch->schema_children != NULL && batch->schema_storage != NULL &&
               batch->array_children != NULL && batch->array_storage != NULL &&
               batch->rowaddrs != NULL,
           "allocate overlay batch");
    for (size_t i = 0; i < children; ++i) {
        batch->schema_children[i] = &batch->schema_storage[i];
        batch->array_children[i] = &batch->array_storage[i];
    }

    batch->schema.format = "+s";
    batch->schema.name = "";
    batch->schema.n_children = (int64_t)children;
    batch->schema.children = batch->schema_children;
    batch->schema.private_data = batch->schema_storage;
    batch->schema.release = release_root_schema;
    init_child_schema(batch->schema_children[0], "L", "_rowaddr", 0);

    batch->array.length = (int64_t)len;
    batch->array.n_buffers = 1;
    batch->array.buffers = calloc(1, sizeof(*batch->array.buffers));
    ASSERT(batch->array.buffers != NULL, "allocate struct buffers");
    batch->array.n_children = (int64_t)children;
    batch->array.children = batch->array_children;
    batch->array.private_data = batch->array_storage;
    batch->array.release = release_root_array;
    init_child_array(batch->array_children[0], len, NULL, batch->rowaddrs);

    size_t child = 1;
    if (with_a) {
        batch->a_values = calloc(len, sizeof(*batch->a_values));
        batch->a_validity = malloc((len + 7) / 8);
        ASSERT(batch->a_values != NULL && batch->a_validity != NULL,
               "allocate a buffers");
        memset(batch->a_validity, 0xff, (len + 7) / 8);
        init_child_schema(batch->schema_children[child], "i", "a", 2);
        init_child_array(batch->array_children[child], len,
                         batch->a_validity, batch->a_values);
        ++child;
    }
    if (with_b) {
        batch->b_values = calloc(len, sizeof(*batch->b_values));
        batch->b_validity = malloc((len + 7) / 8);
        ASSERT(batch->b_values != NULL && batch->b_validity != NULL,
               "allocate b buffers");
        memset(batch->b_validity, 0xff, (len + 7) / 8);
        init_child_schema(batch->schema_children[child], "i", "b", 2);
        init_child_array(batch->array_children[child], len,
                         batch->b_validity, batch->b_values);
    }
}

static void set_rowaddr(OverlayBatch *batch, size_t index, uint64_t value) {
    batch->rowaddrs[index] = value;
}

static void set_a(OverlayBatch *batch, size_t index, int32_t value) {
    ASSERT(batch->with_a, "batch has an a column");
    batch->a_values[index] = value;
}

static void set_a_null(OverlayBatch *batch, size_t index) {
    ASSERT(batch->with_a, "batch has an a column");
    size_t child = 1;
    batch->a_validity[index / 8] &= (uint8_t)~(1u << (index % 8));
    batch->array_children[child]->null_count += 1;
}

static void set_b(OverlayBatch *batch, size_t index, int32_t value) {
    ASSERT(batch->with_b, "batch has a b column");
    batch->b_values[index] = value;
}

static LanceBatch *import_overlay_batch(OverlayBatch *batch) {
    LanceBatch *imported = NULL;
    int32_t rc = lance_batch_from_arrow(&batch->array, &batch->schema, &imported);
    ASSERT(rc == 0, "import overlay batch");
    CHECK_OK();
    if (batch->schema.release) batch->schema.release(&batch->schema);
    return imported;
}

static int array_bit(const struct ArrowArray *array, size_t index) {
    if (!array->buffers || !array->buffers[0]) return 1;
    const uint8_t *bitmap = array->buffers[0];
    return (bitmap[index / 8] >> (index % 8)) & 1;
}

static Row find_row(const LanceDataset *dataset, int32_t wanted) {
    const char *columns[] = {"id", "a", "b", NULL};
    Row row;
    memset(&row, 0, sizeof(row));
    LanceScanner *scanner = lance_scanner_new(dataset, columns, NULL);
    ASSERT(scanner != NULL, "create row scanner");
    CHECK_OK();
    ASSERT(lance_scanner_with_row_id(scanner, true) == 0, "enable row id");
    CHECK_OK();

    for (;;) {
        LanceBatch *batch = NULL;
        int32_t status = lance_scanner_next(scanner, &batch);
        ASSERT(status >= 0, "read scanner batch");
        if (status == 1) break;
        struct ArrowArray array;
        struct ArrowSchema schema;
        memset(&array, 0, sizeof(array));
        memset(&schema, 0, sizeof(schema));
        ASSERT(lance_batch_to_arrow(batch, &array, &schema) == 0,
               "export scanner batch");
        CHECK_OK();

        int id_child = -1;
        int a_child = -1;
        int b_child = -1;
        int rowid_child = -1;
        for (int64_t c = 0; c < array.n_children; ++c) {
            const char *name = schema.children[c]->name;
            if (strcmp(name, "id") == 0) id_child = (int)c;
            if (strcmp(name, "a") == 0) a_child = (int)c;
            if (strcmp(name, "b") == 0) b_child = (int)c;
            if (strcmp(name, "_rowid") == 0) rowid_child = (int)c;
        }
        ASSERT(id_child >= 0 && a_child >= 0 && b_child >= 0 &&
                   rowid_child >= 0,
               "scanner columns present");
        for (int64_t i = 0; i < array.length; ++i) {
            const int32_t *ids = array.children[id_child]->buffers[1];
            if (!array_bit(array.children[id_child], (size_t)i)) continue;
            if (ids[i] != wanted) continue;
            row.found = 1;
            row.id = ids[i];
            row.a_null = !array_bit(array.children[a_child], (size_t)i);
            row.a = row.a_null
                        ? 0
                        : ((const int32_t *)array.children[a_child]->buffers[1])[i];
            row.b_null = !array_bit(array.children[b_child], (size_t)i);
            row.b = row.b_null
                        ? 0
                        : ((const int32_t *)array.children[b_child]->buffers[1])[i];
            row.rowid =
                ((const uint64_t *)array.children[rowid_child]->buffers[1])[i];
            break;
        }
        if (array.release) array.release(&array);
        if (schema.release) schema.release(&schema);
        lance_batch_free(batch);
        if (row.found) break;
    }
    lance_scanner_close(scanner);
    return row;
}

static void assert_row(const LanceDataset *dataset, int32_t id, int a_null,
                       int32_t a, int32_t b, uint64_t rowid) {
    Row row = find_row(dataset, id);
    ASSERT(row.found, "row must exist");
    ASSERT(row.a_null == a_null, "row a NULL state");
    if (!a_null) ASSERT(row.a == a, "row a value");
    ASSERT(!row.b_null, "row b must not be NULL");
    ASSERT(row.b == b, "row b value");
    ASSERT(row.rowid == rowid, "row id unchanged");
}

static LanceFragment *open_fragment(const LanceDataset *dataset,
                                    uint64_t fragment_id) {
    LanceFragment *fragment = lance_dataset_open_fragment(dataset, fragment_id);
    ASSERT(fragment != NULL, "open fragment");
    CHECK_OK();
    return fragment;
}

static LanceSchema *project_schema(const LanceFragment *fragment,
                                   const char *const *columns,
                                   size_t num_columns) {
    LanceSchema *schema = NULL;
    ASSERT(lance_fragment_schema_clone(fragment, &schema) == 0,
           "clone fragment schema");
    CHECK_OK();
    LanceSchema *projected = NULL;
    ASSERT(lance_schema_project(schema, columns, num_columns, &projected) == 0,
           "project fragment schema");
    CHECK_OK();
    lance_schema_close(schema);
    return projected;
}

static LanceFragmentOverlayWriter *new_writer(const LanceFragment *fragment,
                                              const LanceSchema *schema) {
    LanceFragmentOverlayWriter *writer = NULL;
    ASSERT(lance_fragment_overlay_writer_new(fragment, schema, &writer) == 0,
           "open overlay writer");
    CHECK_OK();
    ASSERT(writer != NULL, "overlay writer non-null");
    return writer;
}

static LanceDataOverlayGroup *finish_writer(LanceFragmentOverlayWriter *writer) {
    LanceDataOverlayGroup *group = NULL;
    ASSERT(lance_fragment_overlay_writer_finish(writer, &group) == 0,
           "finish overlay writer");
    CHECK_OK();
    return group;
}

static void check_overlay_metadata(const LanceDataOverlayFile *overlay,
                                   uint64_t committed_version) {
    int32_t kind = -1;
    uint64_t count = 0;
    uint64_t version = UINT64_MAX;
    ASSERT(lance_data_overlay_file_coverage_kind(overlay, &kind) == 0,
           "coverage kind");
    ASSERT(lance_data_overlay_file_bitmap_count(overlay, &count) == 0,
           "bitmap count");
    ASSERT(lance_data_overlay_file_committed_version(overlay, &version) == 0,
           "committed version");
    ASSERT(version == committed_version, "committed version value");
    ASSERT(count > 0, "bitmap count non-zero");
    ASSERT(kind == LANCE_DATA_OVERLAY_COVERAGE_SHARED ||
               kind == LANCE_DATA_OVERLAY_COVERAGE_PER_FIELD,
           "coverage kind value");

    LanceDataFile *data_file = lance_data_overlay_file_data_file(overlay);
    ASSERT(data_file != NULL, "overlay data file");
    CHECK_OK();
    const char *path = lance_data_file_path(data_file);
    ASSERT(path != NULL && path[0] != '\0', "overlay data file path");
    ASSERT(lance_data_file_field_count(data_file) > 0,
           "overlay data file fields");
    lance_data_file_close(data_file);
}

static LanceDataOverlayGroup *
reconstruct_group(const LanceDataOverlayGroup *source,
                  int32_t expected_kind) {
    uint64_t fragment_id = UINT64_MAX;
    uint64_t overlay_count = 0;
    ASSERT(lance_data_overlay_group_fragment_id(source, &fragment_id) == 0,
           "group fragment id");
    ASSERT(lance_data_overlay_group_overlay_count(source, &overlay_count) == 0,
           "group overlay count");
    ASSERT(overlay_count == 1, "one overlay per group");

    LanceDataOverlayFile *overlay = NULL;
    ASSERT(lance_data_overlay_group_overlay_at(source, 0, &overlay) == 0,
           "group overlay at");
    CHECK_OK();
    check_overlay_metadata(overlay, 0);

    int32_t kind = -1;
    uint64_t bitmap_count = 0;
    ASSERT(lance_data_overlay_file_coverage_kind(overlay, &kind) == 0,
           "overlay kind");
    ASSERT(kind == expected_kind, "expected coverage kind");
    ASSERT(lance_data_overlay_file_bitmap_count(overlay, &bitmap_count) == 0,
           "overlay bitmap count");
    ASSERT(bitmap_count > 0 && bitmap_count <= 8, "bitmap count bounds");

    uint8_t *bitmaps[8] = {0};
    size_t bitmap_lens[8] = {0};
    const uint8_t *bitmap_ptrs[8] = {0};
    for (uint64_t i = 0; i < bitmap_count; ++i) {
        ASSERT(lance_data_overlay_file_bitmap_at(overlay, (size_t)i, &bitmaps[i],
                                                 &bitmap_lens[i]) == 0,
               "read overlay bitmap");
        CHECK_OK();
        ASSERT(bitmaps[i] != NULL && bitmap_lens[i] > 0, "bitmap bytes");
        bitmap_ptrs[i] = bitmaps[i];
    }

    LanceDataFile *data_file = lance_data_overlay_file_data_file(overlay);
    ASSERT(data_file != NULL, "reconstruct data file");
    CHECK_OK();
    LanceDataOverlayFile *rebuilt = lance_data_overlay_file_new(
        data_file, kind, bitmap_ptrs, bitmap_lens, (size_t)bitmap_count, 0);
    ASSERT(rebuilt != NULL, "rebuild overlay file");
    CHECK_OK();
    lance_data_file_close(data_file);

    LanceDataOverlayFile *cloned = lance_data_overlay_file_clone(rebuilt);
    ASSERT(cloned != NULL, "clone rebuilt overlay");
    CHECK_OK();
    lance_data_overlay_file_close(rebuilt);

    const LanceDataOverlayFile *overlay_handles[1] = {cloned};
    LanceDataOverlayGroup *group =
        lance_data_overlay_group_new(fragment_id, overlay_handles, 1);
    ASSERT(group != NULL, "rebuild overlay group");
    CHECK_OK();
    lance_data_overlay_file_close(cloned);

    for (uint64_t i = 0; i < bitmap_count; ++i) lance_free_bytes(bitmaps[i]);
    lance_data_overlay_file_close(overlay);
    return group;
}

static LanceDataset *commit_overlay(const char *uri, uint64_t read_version,
                                    const LanceDataOverlayGroup *const *groups,
                                    size_t group_count) {
    LanceOperation *operation =
        lance_operation_data_overlay_new(groups, group_count);
    ASSERT(operation != NULL, "create data overlay operation");
    CHECK_OK();
    LanceOperationKind operation_kind = LANCE_OPERATION_UNKNOWN;
    ASSERT(lance_operation_kind(operation, &operation_kind) == 0,
           "operation kind");
    ASSERT(operation_kind == LANCE_OPERATION_DATA_OVERLAY, "operation kind");
    uint64_t operation_groups = 0;
    ASSERT(lance_operation_data_overlay_group_count(operation,
                                                    &operation_groups) == 0,
           "operation group count");
    ASSERT(operation_groups == group_count, "operation group count value");

    LanceTransaction *transaction =
        lance_transaction_new(read_version, operation, NULL);
    ASSERT(transaction != NULL, "create transaction");
    CHECK_OK();
    uint64_t transaction_version = 0;
    ASSERT(lance_transaction_read_version(transaction, &transaction_version) == 0,
           "transaction read version");
    ASSERT(transaction_version == read_version, "transaction version value");

    LanceDataset *committed = NULL;
    int32_t rc = lance_transaction_commit(uri, transaction, NULL, NULL, NULL,
                                          &committed);
    lance_transaction_close(transaction);
    lance_operation_close(operation);
    ASSERT(rc == 0, "commit data overlay");
    CHECK_OK();
    ASSERT(committed != NULL, "committed dataset");
    return committed;
}

static void check_history(const LanceDataset *dataset, uint64_t version,
                          uint64_t expected_group_count) {
    LanceTransaction *transaction = NULL;
    ASSERT(lance_dataset_read_transaction_by_version(dataset, version,
                                                     &transaction) == 0,
           "read transaction history");
    CHECK_OK();
    ASSERT(transaction != NULL, "history transaction");

    LanceOperation *operation = NULL;
    ASSERT(lance_transaction_operation(transaction, &operation) == 0,
           "history operation");
    CHECK_OK();
    ASSERT(operation != NULL, "history operation non-null");
    LanceOperationKind operation_kind = LANCE_OPERATION_UNKNOWN;
    ASSERT(lance_operation_kind(operation, &operation_kind) == 0,
           "history operation kind");
    ASSERT(operation_kind == LANCE_OPERATION_DATA_OVERLAY,
           "history operation kind");
    uint64_t group_count = 0;
    ASSERT(lance_operation_data_overlay_group_count(operation, &group_count) == 0,
           "history group count");
    ASSERT(group_count == expected_group_count, "history group count");
    for (uint64_t i = 0; i < group_count; ++i) {
        LanceDataOverlayGroup *group = NULL;
        ASSERT(lance_operation_data_overlay_group_at(operation, (size_t)i,
                                                     &group) == 0,
               "history group at");
        CHECK_OK();
        uint64_t overlay_count = 0;
        ASSERT(lance_data_overlay_group_overlay_count(group, &overlay_count) == 0,
               "history overlay count");
        ASSERT(overlay_count == 1, "history one overlay");
        LanceDataOverlayFile *overlay = NULL;
        ASSERT(lance_data_overlay_group_overlay_at(group, 0, &overlay) == 0,
               "history overlay at");
        CHECK_OK();
        uint64_t committed_version = 0;
        ASSERT(lance_data_overlay_file_committed_version(overlay,
                                                         &committed_version) == 0,
               "history committed version");
        ASSERT(committed_version == 0,
               "history preserves submitted placeholder version");
        lance_data_overlay_file_close(overlay);
        lance_data_overlay_group_close(group);
    }
    lance_operation_close(operation);
    lance_transaction_close(transaction);
}

static void check_group_metadata(const LanceDataOverlayGroup *group,
                                uint64_t expected_fragment_id,
                                int32_t expected_kind,
                                uint64_t expected_bitmap_count) {
    uint64_t fragment_id = UINT64_MAX;
    uint64_t overlay_count = 0;
    ASSERT(lance_data_overlay_group_fragment_id(group, &fragment_id) == 0,
           "group fragment id");
    ASSERT(fragment_id == expected_fragment_id, "group fragment id value");
    ASSERT(lance_data_overlay_group_overlay_count(group, &overlay_count) == 0,
           "group overlay count");
    ASSERT(overlay_count == 1, "group overlay count value");
    LanceDataOverlayFile *overlay = NULL;
    ASSERT(lance_data_overlay_group_overlay_at(group, 0, &overlay) == 0,
           "group overlay at");
    CHECK_OK();
    check_overlay_metadata(overlay, 0);
    int32_t kind = -1;
    uint64_t bitmap_count = 0;
    ASSERT(lance_data_overlay_file_coverage_kind(overlay, &kind) == 0,
           "group overlay kind");
    ASSERT(kind == expected_kind, "group overlay kind value");
    ASSERT(lance_data_overlay_file_bitmap_count(overlay, &bitmap_count) == 0,
           "group overlay bitmap count");
    ASSERT(bitmap_count == expected_bitmap_count,
           "group overlay bitmap count value");
    lance_data_overlay_file_close(overlay);
}

int main(int argc, char **argv) {
    ASSERT(argc >= 2, "dataset URI argument");
    const char *uri = argv[1];

    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open dataset");
    CHECK_OK();
    uint64_t base_version = lance_dataset_version(dataset);
    ASSERT(base_version == 2, "fixture dataset version");
    ASSERT(lance_dataset_fragment_count(dataset) == 2, "fixture fragment count");

    uint64_t fragment_ids[2] = {0, 0};
    ASSERT(lance_dataset_fragment_ids(dataset, fragment_ids) == 0,
           "fragment ids");
    CHECK_OK();

    Row before[21];
    memset(before, 0, sizeof(before));
    for (int32_t id = 1; id <= 20; ++id) {
        before[id] = find_row(dataset, id);
    }
    for (int32_t id = 1; id <= 20; ++id) {
        if (id == 10) {
            ASSERT(!before[id].found, "deleted row 10 absent");
        } else {
            ASSERT(before[id].found, "fixture row present");
        }
    }
    ASSERT(!before[10].found, "deleted row absent");
    ASSERT(before[1].rowid != before[2].rowid, "row ids populated");

    LanceFragment *fragment0 = open_fragment(dataset, fragment_ids[0]);
    LanceFragment *fragment1 = open_fragment(dataset, fragment_ids[1]);
    const char *ab_columns[] = {"a", "b"};
    LanceSchema *ab_schema0 = project_schema(fragment0, ab_columns, 2);
    LanceSchema *ab_schema1 = project_schema(fragment1, ab_columns, 2);

    LanceFragmentOverlayWriter *writer0 = new_writer(fragment0, ab_schema0);
    OverlayBatch batch0;
    init_overlay_batch(&batch0, 5, 1, 1);
    const uint32_t offsets0[5] = {0, 2, 4, 7, 9};
    for (size_t i = 0; i < 5; ++i) {
        set_rowaddr(&batch0, i, ROW_ADDRESS(fragment_ids[0], offsets0[i]));
    }
    set_a(&batch0, 0, 111);
    set_a(&batch0, 1, 0);
    set_a_null(&batch0, 1);
    set_a(&batch0, 2, 113);
    set_a(&batch0, 3, 114);
    set_a(&batch0, 4, 115);
    set_b(&batch0, 0, 211);
    set_b(&batch0, 1, 212);
    set_b(&batch0, 2, 213);
    set_b(&batch0, 3, 214);
    set_b(&batch0, 4, 215);
    LanceBatch *imported0 = import_overlay_batch(&batch0);
    ASSERT(lance_fragment_overlay_writer_write_batch(writer0, imported0) == 0,
           "write shared overlay batch");
    CHECK_OK();
    lance_batch_free(imported0);
    LanceDataOverlayGroup *finished0 = finish_writer(writer0);
    ASSERT(finished0 != NULL, "shared finish returns group");
    check_group_metadata(finished0, fragment_ids[0],
                         LANCE_DATA_OVERLAY_COVERAGE_SHARED, 1);
    LanceDataOverlayGroup *group0 = reconstruct_group(finished0,
                                                      LANCE_DATA_OVERLAY_COVERAGE_SHARED);
    lance_data_overlay_group_close(finished0);

    LanceFragmentOverlayWriter *writer1 = new_writer(fragment1, ab_schema1);
    OverlayBatch batch1;
    init_overlay_batch(&batch1, 3, 1, 0);
    const uint32_t offsets1[3] = {1, 3, 5};
    for (size_t i = 0; i < 3; ++i) {
        set_rowaddr(&batch1, i, ROW_ADDRESS(fragment_ids[1], offsets1[i]));
    }
    set_a(&batch1, 0, 1000);
    set_a(&batch1, 1, 1001);
    set_a(&batch1, 2, 1002);
    LanceBatch *imported1 = import_overlay_batch(&batch1);
    ASSERT(lance_fragment_overlay_writer_write_batch(writer1, imported1) == 0,
           "write per-field a batch");
    CHECK_OK();
    lance_batch_free(imported1);

    OverlayBatch batch2;
    init_overlay_batch(&batch2, 2, 0, 1);
    const uint32_t offsets2[2] = {6, 8};
    for (size_t i = 0; i < 2; ++i) {
        set_rowaddr(&batch2, i, ROW_ADDRESS(fragment_ids[1], offsets2[i]));
    }
    set_b(&batch2, 0, 2000);
    set_b(&batch2, 1, 2001);
    LanceBatch *imported2 = import_overlay_batch(&batch2);
    ASSERT(lance_fragment_overlay_writer_write_batch(writer1, imported2) == 0,
           "write per-field b batch");
    CHECK_OK();
    lance_batch_free(imported2);

    LanceDataOverlayGroup *finished1 = finish_writer(writer1);
    ASSERT(finished1 != NULL, "per-field finish returns group");
    check_group_metadata(finished1, fragment_ids[1],
                         LANCE_DATA_OVERLAY_COVERAGE_PER_FIELD, 2);
    LanceDataOverlayGroup *group1 = reconstruct_group(finished1,
                                                      LANCE_DATA_OVERLAY_COVERAGE_PER_FIELD);
    lance_data_overlay_group_close(finished1);

    const char *a_columns[] = {"a"};
    LanceSchema *a_schema = project_schema(fragment1, a_columns, 1);
    LanceFragmentOverlayWriter *empty_writer = new_writer(fragment1, a_schema);
    LanceDataOverlayGroup *empty_group = finish_writer(empty_writer);
    ASSERT(empty_group == NULL, "empty overlay finishes as NULL");
    lance_schema_close(a_schema);

    lance_schema_close(ab_schema0);
    lance_schema_close(ab_schema1);
    lance_fragment_close(fragment0);
    lance_fragment_close(fragment1);
    lance_dataset_close(dataset);

    const LanceDataOverlayGroup *groups[2] = {group0, group1};
    LanceDataset *committed = commit_overlay(uri, base_version, groups, 2);
    ASSERT(lance_dataset_version(committed) == base_version + 1,
           "overlay commit advances version");
    ASSERT(lance_dataset_fragment_count(committed) == 2,
           "overlay commit preserves fragments");
    lance_data_overlay_group_close(group0);
    lance_data_overlay_group_close(group1);

    assert_row(committed, 1, 0, 111, 211, before[1].rowid);
    assert_row(committed, 2, 0, 20, 200, before[2].rowid);
    assert_row(committed, 3, 1, 0, 212, before[3].rowid);
    assert_row(committed, 4, 0, 40, 400, before[4].rowid);
    assert_row(committed, 5, 0, 113, 213, before[5].rowid);
    assert_row(committed, 6, 0, 60, 600, before[6].rowid);
    assert_row(committed, 7, 1, 0, 700, before[7].rowid);
    assert_row(committed, 8, 0, 114, 214, before[8].rowid);
    assert_row(committed, 9, 0, 90, 900, before[9].rowid);
    ASSERT(!find_row(committed, 10).found, "deleted row stays deleted");
    assert_row(committed, 11, 0, 110, 1100, before[11].rowid);
    assert_row(committed, 12, 0, 1000, 1200, before[12].rowid);
    assert_row(committed, 13, 0, 130, 1300, before[13].rowid);
    assert_row(committed, 14, 0, 1001, 1400, before[14].rowid);
    assert_row(committed, 15, 0, 150, 1500, before[15].rowid);
    assert_row(committed, 16, 0, 1002, 1600, before[16].rowid);
    assert_row(committed, 17, 0, 170, 2000, before[17].rowid);
    assert_row(committed, 18, 0, 180, 1800, before[18].rowid);
    assert_row(committed, 19, 0, 190, 2001, before[19].rowid);
    assert_row(committed, 20, 0, 200, 2000, before[20].rowid);

    check_history(committed, base_version + 1, 2);

    uint64_t committed_fragment_ids[2] = {0, 0};
    ASSERT(lance_dataset_fragment_ids(committed, committed_fragment_ids) == 0,
           "committed fragment ids");
    LanceFragment *committed_fragment0 =
        open_fragment(committed, committed_fragment_ids[0]);
    LanceSchema *a_schema0 = project_schema(committed_fragment0, a_columns, 1);
    LanceFragmentOverlayWriter *writer2 = new_writer(committed_fragment0, a_schema0);
    OverlayBatch batch3;
    init_overlay_batch(&batch3, 1, 1, 0);
    set_rowaddr(&batch3, 0, ROW_ADDRESS(committed_fragment_ids[0], 0));
    set_a(&batch3, 0, 9999);
    LanceBatch *imported3 = import_overlay_batch(&batch3);
    ASSERT(lance_fragment_overlay_writer_write_batch(writer2, imported3) == 0,
           "write second overlay batch");
    CHECK_OK();
    lance_batch_free(imported3);
    LanceDataOverlayGroup *finished2 = finish_writer(writer2);
    ASSERT(finished2 != NULL, "second overlay finish returns group");
    check_group_metadata(finished2, committed_fragment_ids[0],
                         LANCE_DATA_OVERLAY_COVERAGE_SHARED, 1);
    const LanceDataOverlayGroup *second_groups[1] = {finished2};
    LanceDataset *committed2 =
        commit_overlay(uri, base_version + 1, second_groups, 1);
    ASSERT(lance_dataset_version(committed2) == base_version + 2,
           "second overlay advances version");
    lance_data_overlay_group_close(finished2);
    assert_row(committed2, 1, 0, 9999, 211, before[1].rowid);
    assert_row(committed2, 2, 0, 20, 200, before[2].rowid);
    assert_row(committed2, 3, 1, 0, 212, before[3].rowid);
    check_history(committed2, base_version + 2, 1);
    check_history(committed2, base_version + 1, 2);
    lance_schema_close(a_schema0);
    lance_fragment_close(committed_fragment0);

    LanceDataset *old = lance_dataset_open(uri, NULL, 1);
    ASSERT(old != NULL, "open version 1 snapshot");
    CHECK_OK();
    ASSERT(lance_dataset_version(old) == 1, "old snapshot version");
    uint64_t old_fragment_ids[2] = {0, 0};
    ASSERT(lance_dataset_fragment_ids(old, old_fragment_ids) == 0,
           "old fragment ids");
    LanceFragment *old_fragment = open_fragment(old, old_fragment_ids[0]);
    LanceSchema *old_a_schema = project_schema(old_fragment, a_columns, 1);
    LanceFragmentOverlayWriter *stale_writer =
        new_writer(old_fragment, old_a_schema);
    OverlayBatch stale_batch;
    init_overlay_batch(&stale_batch, 1, 1, 0);
    set_rowaddr(&stale_batch, 0, ROW_ADDRESS(old_fragment_ids[0], 9));
    set_a(&stale_batch, 0, 777);
    LanceBatch *stale_imported = import_overlay_batch(&stale_batch);
    ASSERT(lance_fragment_overlay_writer_write_batch(stale_writer,
                                                     stale_imported) == 0,
           "write stale overlay batch");
    CHECK_OK();
    lance_batch_free(stale_imported);
    LanceDataOverlayGroup *stale_group = finish_writer(stale_writer);
    ASSERT(stale_group != NULL, "stale finish returns group");

    const LanceDataOverlayGroup *stale_groups[1] = {stale_group};
    LanceOperation *stale_operation =
        lance_operation_data_overlay_new(stale_groups, 1);
    ASSERT(stale_operation != NULL, "stale operation");
    CHECK_OK();
    LanceTransaction *stale_transaction =
        lance_transaction_new(1, stale_operation, NULL);
    ASSERT(stale_transaction != NULL, "stale transaction");
    CHECK_OK();
    LanceDataset *stale_committed = NULL;
    int32_t stale_rc = lance_transaction_commit(uri, stale_transaction, NULL,
                                                NULL, NULL, &stale_committed);
    ASSERT(stale_rc != 0, "stale overlay commit must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_COMMIT_CONFLICT,
           "stale overlay reports commit conflict");
    ASSERT(stale_committed == NULL, "failed commit leaves dataset NULL");
    lance_transaction_close(stale_transaction);
    lance_operation_close(stale_operation);
    lance_data_overlay_group_close(stale_group);
    lance_schema_close(old_a_schema);
    lance_fragment_close(old_fragment);
    lance_dataset_close(old);

    LanceDataset *reopened = lance_dataset_open(uri, NULL, 0);
    ASSERT(reopened != NULL, "reopen after stale conflict");
    CHECK_OK();
    ASSERT(lance_dataset_version(reopened) == base_version + 3,
           "stale conflict leaves version unchanged");
    assert_row(reopened, 1, 0, 9999, 211, before[1].rowid);
    assert_row(reopened, 2, 0, 20, 200, before[2].rowid);
    assert_row(reopened, 3, 1, 0, 212, before[3].rowid);

    lance_dataset_close(reopened);
    lance_dataset_close(committed2);
    lance_dataset_close(committed);

    fprintf(stdout, "data overlay C workflow passed\n");
    return 0;
}
