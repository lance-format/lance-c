/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#include "lance/lance.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ArrowSchema ArrowSchema;
typedef struct ArrowArray ArrowArray;
typedef struct ArrowArrayStream ArrowArrayStream;

#define ARROW_FLAG_NULLABLE 2

static int failures = 0;

#define CHECK(condition, message)                                               \
    do {                                                                        \
        if (!(condition)) {                                                     \
            const char *error = lance_last_error_message();                     \
            fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__,        \
                    (message), error ? error : "no error");                    \
            if (error) lance_free_string(error);                               \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static void release_schema(ArrowSchema *schema) {
    if (schema) schema->release = NULL;
}

static void release_array(ArrowArray *array) {
    if (array) array->release = NULL;
}

static void init_schema(
    ArrowSchema *schema,
    const char *format,
    const char *name,
    int64_t flags,
    ArrowSchema **children,
    int64_t n_children) {
    memset(schema, 0, sizeof(*schema));
    schema->format = format;
    schema->name = name;
    schema->flags = flags;
    schema->children = children;
    schema->n_children = n_children;
    schema->release = release_schema;
}

static void init_struct_schema(
    ArrowSchema *schema,
    ArrowSchema *id,
    ArrowSchema *label) {
    ArrowSchema *children[2] = {id, label};
    ArrowSchema **owned = (ArrowSchema **)malloc(sizeof(children));
    memcpy(owned, children, sizeof(children));
    init_schema(schema, "+s", "", 0, owned, 2);
}

static void init_batch_arrays(
    ArrowArray *array,
    ArrowArray *id,
    ArrowArray *label,
    const int32_t *ids,
    const int32_t *offsets,
    const uint8_t *label_validity,
    const char *labels) {
    static const void *id_buffers[2];
    static const void *label_buffers[3];
    static const void *struct_buffers[1];
    static ArrowArray *children[2];

    id_buffers[0] = NULL;
    id_buffers[1] = ids;
    memset(id, 0, sizeof(*id));
    id->length = 3;
    id->null_count = 0;
    id->n_buffers = 2;
    id->buffers = id_buffers;
    id->release = release_array;

    label_buffers[0] = label_validity;
    label_buffers[1] = offsets;
    label_buffers[2] = labels;
    memset(label, 0, sizeof(*label));
    label->length = 3;
    label->null_count = 1;
    label->n_buffers = 3;
    label->buffers = label_buffers;
    label->release = release_array;

    children[0] = id;
    children[1] = label;
    struct_buffers[0] = NULL;
    memset(array, 0, sizeof(*array));
    array->length = 3;
    array->null_count = 0;
    array->n_buffers = 1;
    array->buffers = struct_buffers;
    array->n_children = 2;
    array->children = children;
    array->release = release_array;
}

static const char *label_at(const ArrowArray *array, int64_t index) {
    static char output[16];
    const uint8_t *validity = (const uint8_t *)array->buffers[0];
    if (validity && ((validity[index / 8] >> (index % 8)) & 1) == 0) {
        return NULL;
    }
    const int32_t *offsets = (const int32_t *)array->buffers[1];
    const char *values = (const char *)array->buffers[2];
    int32_t length = offsets[index + 1] - offsets[index];
    memcpy(output, values + offsets[index], (size_t)length);
    output[length] = '\0';
    return output;
}

static int verify_stream(
    ArrowArrayStream *stream,
    int expected_rows,
    int first_id) {
    ArrowSchema schema = {0};
    if (stream->get_schema(stream, &schema) != 0) {
        const char *error = stream->get_last_error(stream);
        fprintf(stderr, "VERIFY get_schema failed: %s\n",
                error ? error : "none");
        return 0;
    }
    int valid_schema = strcmp(schema.format, "+s") == 0 && schema.n_children == 2;
    if (!valid_schema) {
        fprintf(stderr, "VERIFY schema format=%s children=%lld\n",
                schema.format, (long long)schema.n_children);
    }
    if (schema.release) schema.release(&schema);
    if (!valid_schema) return 0;

    int rows = 0;
    ArrowArray batch = {0};
    while (stream->get_next(stream, &batch) == 0) {
        if (!batch.release) break;
        if (batch.n_children != 2) {
            fprintf(stderr, "VERIFY batch children=%lld rows=%d\n",
                    (long long)batch.n_children, rows);
            return 0;
        }
        const ArrowArray *id = batch.children[0];
        const ArrowArray *label = batch.children[1];
        const int32_t *ids = (const int32_t *)id->buffers[1];
        for (int64_t row = 0; row < batch.length; row++) {
            if (ids[row] != first_id + rows + (int)row) {
                fprintf(stderr,
                        "VERIFY id mismatch row=%lld value=%d expected=%d "
                        "batch_len=%lld rows=%d\n",
                        (long long)row, ids[row],
                        first_id + rows + (int)row,
                        (long long)batch.length, rows);
                return 0;
            }
        }
        if (rows == 0 && batch.length > 2) {
            if (strcmp(label_at(label, 1), "two") != 0) return 0;
            if (label_at(label, 2) != NULL) return 0;
        }
        rows += (int)batch.length;
        batch.release(&batch);
    }
    if (rows != expected_rows) {
        const char *error = stream->get_last_error(stream);
        fprintf(stderr, "VERIFY rows=%d expected=%d error=%s\n", rows,
                expected_rows, error ? error : "none");
    }
    return rows == expected_rows;
}

static int provider_fetch(
    void *provider_ctx,
    LanceStorageOptionsFetchResult *out,
    LanceCallbackError *error) {
    (void)provider_ctx;
    (void)error;
    static const char *keys[] = {"block_size"};
    static const char *values[] = {"8192"};
    out->has_options = 1;
    out->keys = keys;
    out->values = values;
    out->count = 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char base_uri[4096];
    snprintf(base_uri, sizeof(base_uri), "file://%s", argv[1]);

    const char *option_keys[] = {"block_size"};
    const char *option_values[] = {"4096"};
    LanceStorageOptionsInput options = {option_keys, option_values, 1};
    LanceStorageOptionsAccessor *storage =
        lance_storage_options_accessor_new(&options, NULL);
    CHECK(storage != NULL, "storage accessor");
    LanceFileSession *session =
        lance_file_session_new(base_uri, storage, NULL, NULL);
    CHECK(session != NULL, "file session");
    if (!session) return 1;

    LanceObjectStoreInfo info = {0};
    CHECK(lance_file_session_get_store_info(session, &info) == 0,
          "store info");
    CHECK(info.block_size == 4096, "configured block size");
    CHECK(info.is_local != 0, "local store");

    ArrowSchema id_schema;
    ArrowSchema label_schema;
    ArrowSchema schema;
    init_schema(&id_schema, "i", "id", 0, NULL, 0);
    init_schema(&label_schema, "u", "label", ARROW_FLAG_NULLABLE, NULL, 0);
    init_struct_schema(&schema, &id_schema, &label_schema);

    const int32_t ids[] = {1, 2, 3};
    const int32_t offsets[] = {0, 3, 6, 6};
    const uint8_t label_validity[] = {0x03};
    const char labels[] = "onetwo";
    ArrowArray id_array;
    ArrowArray label_array;
    ArrowArray array;
    init_batch_arrays(
        &array, &id_array, &label_array, ids, offsets, label_validity, labels);
    LanceBatch *batch = NULL;
    CHECK(lance_batch_from_arrow(&array, &schema, &batch) == 0,
          "batch import");
    CHECK(array.release == NULL, "array consumed");
    CHECK(schema.release != NULL, "schema remains borrowed");

    LanceFileWriter *writer = lance_file_session_open_writer(
        session, "c/data.lance", &schema, NULL);
    CHECK(writer != NULL, "writer open");
    if (!writer) return 1;
    CHECK(lance_file_writer_write_batch(writer, batch) == 0, "write batch");
    CHECK(lance_file_writer_add_schema_metadata(writer, "owner", "lance-c") == 0,
          "schema metadata");
    const uint8_t global_bytes[] = {0x00, 0x67, 0xff};
    uint32_t global_index = 0;
    CHECK(lance_file_writer_add_global_buffer(
              writer, global_bytes, sizeof(global_bytes), &global_index) == 0,
          "global buffer");
    CHECK(global_index == 1, "global buffer index");
    LanceFileWriteSummary summary = {0};
    CHECK(lance_file_writer_finish(writer, &summary) == 0, "writer finish");
    CHECK(summary.num_rows == 3, "summary rows");
    CHECK(summary.size_bytes > 0, "summary size");
    lance_file_writer_close(writer);
    lance_batch_free(batch);

    LanceFileReader *reader =
        lance_file_session_open_reader(session, "c/data.lance", NULL);
    CHECK(reader != NULL, "reader open");
    if (!reader) return 1;
    uint64_t rows = 0;
    CHECK(lance_file_reader_num_rows(reader, &rows) == 0, "reader rows");
    CHECK(rows == 3, "reader row count");

    LanceFileMetadata *metadata = NULL;
    CHECK(lance_file_reader_get_metadata(reader, &metadata) == 0,
          "metadata snapshot");
    LanceFileMetadataInfo metadata_info = {0};
    CHECK(lance_file_metadata_get_info(metadata, &metadata_info) == 0,
          "metadata info");
    CHECK(metadata_info.num_rows == 3, "metadata rows");
    ArrowSchema exported_schema = {0};
    CHECK(lance_file_metadata_get_schema(metadata, &exported_schema) == 0,
          "metadata schema");
    CHECK(exported_schema.release != NULL, "schema ownership");
    if (exported_schema.release) exported_schema.release(&exported_schema);

    LanceByteBuffer *global = NULL;
    CHECK(lance_file_reader_read_global_buffer(reader, global_index, &global) == 0,
          "read global buffer");
    CHECK(global != NULL, "global buffer handle");
    CHECK(lance_byte_buffer_len(global) == sizeof(global_bytes),
          "global buffer length");
    CHECK(memcmp(lance_byte_buffer_data(global), global_bytes,
                 sizeof(global_bytes)) == 0,
          "global buffer bytes");
    lance_byte_buffer_close(global);

    ArrowArrayStream stream = {0};
    CHECK(lance_file_reader_read_all(reader, 2, 2, &stream) == 0,
          "read all");
    CHECK(verify_stream(&stream, 3, 1), "read all rows");
    if (stream.release) stream.release(&stream);

    ArrowArrayStream range_stream = {0};
    CHECK(lance_file_reader_read_range(reader, 1, 1, 1, 1, &range_stream) == 0,
          "read range");
    CHECK(verify_stream(&range_stream, 1, 2), "range rows");
    if (range_stream.release) range_stream.release(&range_stream);

    lance_file_metadata_close(metadata);
    lance_file_reader_close(reader);

    char local_path[4096];
    snprintf(local_path, sizeof(local_path), "%s/source.bin", argv[1]);
    FILE *local = fopen(local_path, "wb");
    CHECK(local != NULL, "local file");
    if (local) {
        fwrite("0123456789", 1, 10, local);
        fclose(local);
    }
    CHECK(lance_file_session_upload_file(session, local_path, "c/ops.bin") == 0,
          "upload");
    int32_t exists = 0;
    CHECK(lance_file_session_contains(session, "c/ops.bin", &exists) == 0,
          "contains");
    CHECK(exists == 1, "uploaded object exists");
    LanceByteBuffer *range =
        lance_file_session_read_range(session, "c/ops.bin", 2, 4);
    CHECK(range != NULL, "session range");
    if (range) {
        CHECK(lance_byte_buffer_len(range) == 4, "session range length");
        CHECK(memcmp(lance_byte_buffer_data(range), "2345", 4) == 0,
              "session range bytes");
        lance_byte_buffer_close(range);
    }

    LancePathList *list = lance_file_session_list(session, NULL);
    CHECK(list != NULL, "recursive list");
    if (list) {
        uint64_t count = lance_path_list_count(list);
        int found_data = 0;
        int found_ops = 0;
        for (uint64_t index = 0; index < count; index++) {
            const char *path = lance_path_list_at(list, (size_t)index);
            if (path && strcmp(path, "c/data.lance") == 0) found_data = 1;
            if (path && strcmp(path, "c/ops.bin") == 0) found_ops = 1;
        }
        CHECK(found_data && found_ops, "list contents");
        lance_path_list_close(list);
    }

    char download_path[4096];
    snprintf(download_path, sizeof(download_path), "%s/download.bin", argv[1]);
    CHECK(lance_file_session_download_file(
              session, "c/ops.bin", download_path) == 0,
          "download");
    local = fopen(download_path, "rb");
    CHECK(local != NULL, "downloaded file");
    if (local) {
        char downloaded[11] = {0};
        CHECK(fread(downloaded, 1, 10, local) == 10, "download length");
        CHECK(strcmp(downloaded, "0123456789") == 0, "download bytes");
        fclose(local);
    }
    CHECK(lance_file_session_delete_file(session, "c/ops.bin") == 0, "delete");

    LanceStorageOptionsProviderSpec provider_spec = {0};
    provider_spec.fetch = provider_fetch;
    provider_spec.provider_id = "c-file-provider";
    LanceStorageOptionsProvider *provider =
        lance_storage_options_provider_new(&provider_spec);
    CHECK(provider != NULL, "provider create");
    LanceStorageOptionsAccessor *dynamic =
        lance_storage_options_accessor_new(NULL, provider);
    CHECK(dynamic != NULL, "dynamic accessor");
    LanceStorageOptionsSnapshot *snapshot =
        lance_storage_options_accessor_snapshot(dynamic);
    CHECK(snapshot != NULL, "dynamic snapshot");
    if (snapshot) {
        CHECK(lance_storage_options_snapshot_has_options(snapshot) == 1,
              "snapshot has options");
        CHECK(lance_storage_options_snapshot_count(snapshot) == 1,
              "snapshot count");
        const char *key = lance_storage_options_snapshot_key_at(snapshot, 0);
        const char *value = lance_storage_options_snapshot_value_at(snapshot, 0);
        CHECK(key && strcmp(key, "block_size") == 0, "snapshot key");
        CHECK(value && strcmp(value, "8192") == 0, "snapshot value");
        lance_storage_options_snapshot_close(snapshot);
    }
    lance_storage_options_accessor_close(dynamic);
    lance_storage_options_provider_close(provider);

    lance_file_session_close(session);
    lance_storage_options_accessor_close(storage);
    free(schema.children);
    return failures == 0 ? 0 : 1;
}
