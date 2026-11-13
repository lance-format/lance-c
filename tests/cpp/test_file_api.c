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

static int read_object_bytes(
    const char *object_path,
    char *output,
    size_t capacity,
    long *output_len) {
    char local_path[4096];
    if (object_path[0] == '/') {
        snprintf(local_path, sizeof(local_path), "%s", object_path);
    } else {
        snprintf(local_path, sizeof(local_path), "/%s", object_path);
    }
    FILE *file = fopen(local_path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    long length = ftell(file);
    if (length < 0 || (size_t)length >= capacity ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    size_t read = fread(output, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) return 0;
    output[length] = '\0';
    *output_len = length;
    return 1;
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

static ArrowArrayStream invalid_stream_sentinel(void) {
    ArrowArrayStream stream;
    memset(&stream, 0xA5, sizeof(stream));
    return stream;
}

static int expect_stream_option_error(
    int status,
    const char *option,
    const char *context) {
    if (status != -1) {
        fprintf(stderr, "FAIL %s: status=%d, expected -1\n", context, status);
        return 0;
    }
    LanceErrorCode code = lance_last_error_code();
    const char *message = lance_last_error_message();
    int matches = code == LANCE_ERR_INVALID_ARGUMENT && message != NULL &&
                  strstr(message, option) != NULL &&
                  strstr(message, "greater than 0") != NULL;
    if (!matches) {
        fprintf(stderr, "FAIL %s: code=%d message=%s\n", context, (int)code,
                message ? message : "no error");
    }
    if (message) lance_free_string(message);
    return matches;
}

static int stream_output_unchanged(
    const ArrowArrayStream *before,
    const ArrowArrayStream *after) {
    return memcmp(before, after, sizeof(*before)) == 0;
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

static const char *const blob_v2_descriptor_children[] = {
    "kind", "position", "size", "blob_id", "blob_uri"};

static int verify_blob_v2_descriptor_schema(
    const ArrowSchema *field,
    const char *label) {
    if (!field || !field->format || strcmp(field->format, "+s") != 0) {
        fprintf(stderr, "BLOB %s is not a struct\n", label);
        return 0;
    }
    if (field->n_children != 5) {
        fprintf(stderr, "BLOB %s children=%lld\n", label,
                (long long)field->n_children);
        return 0;
    }
    for (int64_t index = 0; index < field->n_children; index++) {
        const ArrowSchema *child = field->children[index];
        if (!child->name ||
            strcmp(child->name, blob_v2_descriptor_children[index]) != 0) {
            fprintf(stderr, "BLOB %s child[%lld]=%s expected=%s\n", label,
                    (long long)index, child->name ? child->name : "null",
                    blob_v2_descriptor_children[index]);
            return 0;
        }
    }
    return 1;
}

static int struct_is_valid(const ArrowArray *array, int64_t row) {
    const uint8_t *validity = (const uint8_t *)array->buffers[0];
    return validity == NULL || ((validity[row / 8] >> (row % 8)) & 1) != 0;
}

static int64_t descriptor_size_at(const ArrowArray *descriptor, int64_t row) {
    const ArrowArray *size = descriptor->children[2];
    const int64_t *values = (const int64_t *)size->buffers[1];
    return values[row];
}

/// Read the released Lance 14.0.2 Blob v2 raw file through the public C file
/// reader. The 14.0.2 writer stored Blob v2 descriptor children as footer root
/// fields, so a reader that does not reattach them exposes childless Blob
/// structs (or fails to project the file at all).
static int verify_released_blob_v2_raw_file(const char *base_dir) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/blob_v2_raw_reader/blob_v2_raw.lance",
             base_dir);
    LanceFileReader *reader = lance_file_reader_open(path, NULL, NULL, NULL, NULL);
    if (reader == NULL) {
        fprintf(stderr, "BLOB open failed\n");
        return 0;
    }

    int ok = 1;
    uint64_t num_rows = 0;
    ok = lance_file_reader_num_rows(reader, &num_rows) == 0 && num_rows == 3;
    LanceFileMetadata *metadata = NULL;
    LanceFileMetadataInfo info = {0};
    if (ok) ok = lance_file_reader_get_metadata(reader, &metadata) == 0;
    if (ok) ok = lance_file_metadata_get_info(metadata, &info) == 0;
    if (ok) {
        ok = info.num_rows == 3 && info.num_columns == 4 &&
             info.format_version == LANCE_FILE_FORMAT_V2_2;
    }
    ArrowSchema schema = {0};
    if (ok) ok = lance_file_metadata_get_schema(metadata, &schema) == 0;
    if (ok) {
        ok = schema.n_children == 4 &&
             strcmp(schema.children[0]->name, "id") == 0 &&
             strcmp(schema.children[2]->name, "blob_b") == 0 &&
             strcmp(schema.children[3]->name, "info") == 0;
    }
    if (ok) {
        ok = verify_blob_v2_descriptor_schema(schema.children[1], "blob_a") &&
             verify_blob_v2_descriptor_schema(schema.children[2], "blob_b");
    }
    if (ok) {
        const ArrowSchema *info_field = schema.children[3];
        ok = info_field->n_children == 1 && info_field->children[0]->name &&
             strcmp(info_field->children[0]->name, "payload") == 0 &&
             verify_blob_v2_descriptor_schema(info_field->children[0],
                                              "info.payload");
    }

    ArrowArrayStream stream = {0};
    ArrowArray batch = {0};
    int32_t ids[3] = {0};
    int blob_a_valid[3] = {0, 0, 0};
    int blob_b_valid[3] = {0, 0, 0};
    int payload_valid[3] = {0, 0, 0};
    int64_t blob_a_size[3] = {-1, -1, -1};
    int rows = 0;
    if (ok) ok = lance_file_reader_read_all(reader, 2, 2, &stream) == 0;
    if (ok) {
        ArrowSchema stream_schema = {0};
        ok = stream.get_schema(&stream, &stream_schema) == 0 &&
             strcmp(stream_schema.format, "+s") == 0 &&
             stream_schema.n_children == 4;
        if (stream_schema.release) stream_schema.release(&stream_schema);
    }
    while (ok && stream.get_next(&stream, &batch) == 0) {
        if (!batch.release) break;
        if (batch.n_children != 4) {
            ok = 0;
            break;
        }
        const ArrowArray *id = batch.children[0];
        const int32_t *values = (const int32_t *)id->buffers[1];
        const ArrowArray *blob_a = batch.children[1];
        const ArrowArray *blob_b = batch.children[2];
        const ArrowArray *payload = batch.children[3]->children[0];
        for (int64_t row = 0; row < batch.length; row++) {
            int index = rows + (int)row;
            if (index >= 3) {
                ok = 0;
                break;
            }
            ids[index] = values[row];
            blob_a_valid[index] = struct_is_valid(blob_a, row);
            blob_b_valid[index] = struct_is_valid(blob_b, row);
            payload_valid[index] = struct_is_valid(payload, row);
            if (blob_a_valid[index]) {
                blob_a_size[index] = descriptor_size_at(blob_a, row);
            }
        }
        rows += (int)batch.length;
        batch.release(&batch);
        memset(&batch, 0, sizeof(batch));
    }
    if (batch.release) {
        batch.release(&batch);
        memset(&batch, 0, sizeof(batch));
    }
    ok = ok && rows == 3 && ids[0] == 10 && ids[1] == 20 && ids[2] == 30;
    if (ok) {
        ok = blob_a_valid[0] && !blob_a_valid[1] && blob_a_valid[2] &&
             blob_b_valid[0] && blob_b_valid[1] && !blob_b_valid[2] &&
             payload_valid[0] && !payload_valid[1] && payload_valid[2] &&
             blob_a_size[0] == 5 && blob_a_size[2] == 0;
        if (!ok) {
            fprintf(stderr,
                    "BLOB null and empty rows mismatch: "
                    "a_valid=%d%d%d b_valid=%d%d%d p_valid=%d%d%d sizes=%lld,%lld\n",
                    blob_a_valid[0], blob_a_valid[1], blob_a_valid[2],
                    blob_b_valid[0], blob_b_valid[1], blob_b_valid[2],
                    payload_valid[0], payload_valid[1], payload_valid[2],
                    (long long)blob_a_size[0], (long long)blob_a_size[2]);
        }
    }
    ok = ok && rows == 3 && ids[0] == 10 && ids[1] == 20 && ids[2] == 30;
    if (!ok) fprintf(stderr, "BLOB default readback failed\n");
    if (stream.release) stream.release(&stream);

    if (ok) {
        const char *columns[] = {"blob_b", "info.payload"};
        LanceFileReaderOptions read_options = {0};
        read_options.has_columns = 1;
        read_options.columns = columns;
        read_options.column_count = 2;
        LanceFileReader *projected = NULL;
        projected = lance_file_reader_open(path, NULL, &read_options, NULL, NULL);
        ok = projected != NULL;
        if (ok) {
            ArrowArrayStream projected_stream = {0};
            ok = lance_file_reader_read_all(projected, 3, 2,
                                            &projected_stream) == 0;
            if (ok) {
                ArrowSchema projected_schema = {0};
                ok = projected_stream.get_schema(&projected_stream,
                                                 &projected_schema) == 0 &&
                     strcmp(projected_schema.format, "+s") == 0 &&
                     projected_schema.n_children == 2 &&
                     strcmp(projected_schema.children[0]->name, "blob_b") == 0 &&
                     strcmp(projected_schema.children[1]->name, "info") == 0 &&
                     projected_schema.children[1]->n_children == 1 &&
                     verify_blob_v2_descriptor_schema(
                         projected_schema.children[1]->children[0],
                         "projected info.payload");
                if (projected_schema.release) {
                    projected_schema.release(&projected_schema);
                }
            }
            if (ok) {
                int projected_rows = 0;
                int blob_b_valid[3] = {0, 0, 0};
                int payload_valid[3] = {0, 0, 0};
                int64_t blob_b_size[3] = {-1, -1, -1};
                ArrowArray projected_batch = {0};
                while (projected_stream.get_next(&projected_stream,
                                                 &projected_batch) == 0) {
                    if (!projected_batch.release) break;
                    if (projected_batch.n_children != 2) {
                        ok = 0;
                        break;
                    }
                    const ArrowArray *blob_b = projected_batch.children[0];
                    const ArrowArray *payload =
                        projected_batch.children[1]->children[0];
                    for (int64_t row = 0; row < projected_batch.length; row++) {
                        int index = projected_rows + (int)row;
                        if (index >= 3) {
                            ok = 0;
                            break;
                        }
                        blob_b_valid[index] = struct_is_valid(blob_b, row);
                        payload_valid[index] = struct_is_valid(payload, row);
                        if (blob_b_valid[index]) {
                            blob_b_size[index] = descriptor_size_at(blob_b, row);
                        }
                    }
                    projected_rows += (int)projected_batch.length;
                    projected_batch.release(&projected_batch);
                    memset(&projected_batch, 0, sizeof(projected_batch));
                }
                if (projected_batch.release) {
                    projected_batch.release(&projected_batch);
                    memset(&projected_batch, 0, sizeof(projected_batch));
                }
                ok = ok && projected_rows == 3 && blob_b_valid[0] &&
                     blob_b_valid[1] && !blob_b_valid[2] &&
                     blob_b_size[0] == 3 && blob_b_size[1] == 5 &&
                     payload_valid[0] && !payload_valid[1] && payload_valid[2];
            }
            if (!ok) fprintf(stderr, "BLOB projection readback failed\n");
            if (projected_stream.release) {
                projected_stream.release(&projected_stream);
            }
            lance_file_reader_close(projected);
        }
    }

    lance_file_reader_close(reader);
    // The metadata snapshot is owned and must outlive the reader.
    if (ok) {
        LanceFileMetadataInfo after = {0};
        ok = lance_file_metadata_get_info(metadata, &after) == 0 &&
             after.num_rows == 3;
        if (!ok) fprintf(stderr, "BLOB metadata did not outlive reader\n");
    }
    if (schema.release) schema.release(&schema);
    if (metadata) lance_file_metadata_close(metadata);
    return ok;
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

    const uint32_t selected_indices[] = {1u, 2u};
    const int32_t option_cases[][2] = {{0, 2}, {2, 0}, {0, 0}};
    const char *option_names[] = {"batch_size", "batch_readahead", "batch_size"};
    for (size_t i = 0; i < sizeof(option_cases) / sizeof(option_cases[0]); i++) {
        ArrowArrayStream out = invalid_stream_sentinel();
        ArrowArrayStream before = out;
        int status = lance_file_reader_read_all(
            reader, (uint32_t)option_cases[i][0], (uint32_t)option_cases[i][1],
            &out);
        CHECK(status == -1, "read_all rejects zero stream option");
        CHECK(expect_stream_option_error(status, option_names[i], "read_all"),
              "read_all option error");
        CHECK(stream_output_unchanged(&before, &out),
              "read_all modified out on error");

        out = invalid_stream_sentinel();
        before = out;
        status = lance_file_reader_read_range(
            reader, 1, 1, (uint32_t)option_cases[i][0],
            (uint32_t)option_cases[i][1], &out);
        CHECK(status == -1, "read_range rejects zero stream option");
        CHECK(expect_stream_option_error(status, option_names[i], "read_range"),
              "read_range option error");
        CHECK(stream_output_unchanged(&before, &out),
              "read_range modified out on error");

        out = invalid_stream_sentinel();
        before = out;
        status = lance_file_reader_read_indices(
            reader, selected_indices,
            sizeof(selected_indices) / sizeof(selected_indices[0]),
            (uint32_t)option_cases[i][0], (uint32_t)option_cases[i][1], &out);
        CHECK(status == -1, "read_indices rejects zero stream option");
        CHECK(expect_stream_option_error(status, option_names[i], "read_indices"),
              "read_indices option error");
        CHECK(stream_output_unchanged(&before, &out),
              "read_indices modified out on error");
    }

    ArrowArrayStream indices_stream = {0};
    CHECK(lance_file_reader_read_indices(
              reader, selected_indices,
              sizeof(selected_indices) / sizeof(selected_indices[0]), 2, 1,
              &indices_stream) == 0,
          "read indices");
    CHECK(verify_stream(&indices_stream, 2, 2), "index rows");
    if (indices_stream.release) indices_stream.release(&indices_stream);

    ArrowArrayStream empty_range = {0};
    CHECK(lance_file_reader_read_range(reader, 0, 0, 2, 1, &empty_range) == 0,
          "empty range read");
    CHECK(verify_stream(&empty_range, 0, 1), "empty range stream");
    if (empty_range.release) empty_range.release(&empty_range);

    ArrowArrayStream empty_indices = {0};
    CHECK(lance_file_reader_read_indices(reader, NULL, 0, 2, 1,
                                         &empty_indices) == 0,
          "empty indices read");
    CHECK(verify_stream(&empty_indices, 0, 1), "empty indices stream");
    if (empty_indices.release) empty_indices.release(&empty_indices);

    ArrowArrayStream empty_range_zero = invalid_stream_sentinel();
    ArrowArrayStream empty_range_zero_before = empty_range_zero;
    int empty_status = lance_file_reader_read_range(
        reader, 0, 0, 1, 0, &empty_range_zero);
    CHECK(empty_status == -1, "empty read_range rejects zero readahead");
    CHECK(expect_stream_option_error(empty_status, "batch_readahead",
                                     "empty read_range"),
          "empty read_range option error");
    CHECK(stream_output_unchanged(&empty_range_zero_before, &empty_range_zero),
          "empty read_range modified out on error");

    ArrowArrayStream empty_indices_zero = invalid_stream_sentinel();
    ArrowArrayStream empty_indices_zero_before = empty_indices_zero;
    empty_status = lance_file_reader_read_indices(
        reader, NULL, 0, 0, 1, &empty_indices_zero);
    CHECK(empty_status == -1, "empty read_indices rejects zero batch_size");
    CHECK(expect_stream_option_error(empty_status, "batch_size",
                                     "empty read_indices"),
          "empty read_indices option error");
    CHECK(stream_output_unchanged(&empty_indices_zero_before,
                                  &empty_indices_zero),
          "empty read_indices modified out on error");

    lance_file_metadata_close(metadata);
    lance_file_reader_close(reader);

    ArrowSchema payload_schema;
    init_schema(&payload_schema, "z", "payload", ARROW_FLAG_NULLABLE, NULL, 0);
    const char payload_bytes[] = "skipbeta";
    const int32_t payload_offsets[] = {0, 4, 4, 8};
    const uint8_t payload_validity[] = {0x05};
    const void *payload_buffers[3] = {
        payload_validity, payload_offsets, payload_bytes};
    ArrowArray payload_array = {0};
    payload_array.length = 2;
    payload_array.offset = 1;
    payload_array.n_buffers = 3;
    payload_array.buffers = payload_buffers;
    payload_array.release = release_array;

    LancePackedBlobWriter *packed = lance_file_session_open_packed_blob_writer(
        session, "c/packed-blob.lance", 31);
    CHECK(packed != NULL, "packed blob writer");
    if (packed) {
        CHECK(lance_packed_blob_writer_blob_id(packed) == 31,
              "packed blob id");
        const char *packed_path = lance_packed_blob_writer_path(packed);
        CHECK(packed_path != NULL, "packed blob path");
        char packed_path_copy[4096] = {0};
        if (packed_path) {
            snprintf(packed_path_copy, sizeof(packed_path_copy), "%s",
                     packed_path);
            lance_free_string(packed_path);
        }
        CHECK(lance_packed_blob_writer_write_blobs(
                  packed, &payload_array, &payload_schema) == 0,
              "packed bulk write");
        CHECK(payload_array.release == NULL, "packed input array consumed");
        CHECK(payload_schema.release != NULL, "packed input schema borrowed");
        CHECK(lance_packed_blob_writer_write_blob(packed, NULL, 0) == 0,
              "packed NULL empty payload");
        ArrowArray prepared_array = {0};
        ArrowSchema prepared_schema = {0};
        CHECK(lance_packed_blob_writer_finish_array(
                  packed, "blob", &prepared_array, &prepared_schema) == 0,
              "packed finish");
        CHECK(prepared_array.release != NULL, "packed prepared array");
        CHECK(prepared_schema.release != NULL, "packed prepared field");
        CHECK(prepared_array.length == 3, "packed prepared rows");
        CHECK(prepared_schema.name &&
                  strcmp(prepared_schema.name, "blob") == 0,
              "packed prepared field name");
        char sidecar[64] = {0};
        long sidecar_len = 0;
        CHECK(read_object_bytes(
                  packed_path_copy, sidecar, sizeof(sidecar), &sidecar_len),
              "packed sidecar read");
        CHECK(sidecar_len == 4 && strcmp(sidecar, "beta") == 0,
              "packed sidecar bytes");
        if (prepared_array.release) prepared_array.release(&prepared_array);
        if (prepared_schema.release) prepared_schema.release(&prepared_schema);
        lance_packed_blob_writer_close(packed);
    }

    LanceDedicatedBlobWriter *dedicated =
        lance_file_session_open_dedicated_blob_writer(
            session, "c/dedicated-blob.lance", 32);
    CHECK(dedicated != NULL, "dedicated blob writer");
    if (dedicated) {
        CHECK(lance_dedicated_blob_writer_blob_id(dedicated) == 32,
              "dedicated blob id");
        const char *dedicated_path = lance_dedicated_blob_writer_path(dedicated);
        CHECK(dedicated_path != NULL, "dedicated blob path");
        char dedicated_path_copy[4096] = {0};
        if (dedicated_path) {
            snprintf(dedicated_path_copy, sizeof(dedicated_path_copy), "%s",
                     dedicated_path);
            lance_free_string(dedicated_path);
        }
        CHECK(lance_dedicated_blob_writer_write(
                  dedicated, (const uint8_t *)"alpha", 5) == 0,
              "dedicated write alpha");
        CHECK(lance_dedicated_blob_writer_write(dedicated, NULL, 0) == 0,
              "dedicated NULL empty write");
        CHECK(lance_dedicated_blob_writer_write(
                  dedicated, (const uint8_t *)"omega", 5) == 0,
              "dedicated write omega");
        ArrowArray prepared_array = {0};
        ArrowSchema prepared_schema = {0};
        CHECK(lance_dedicated_blob_writer_finish_array(
                  dedicated, "blob", &prepared_array, &prepared_schema) == 0,
              "dedicated finish");
        CHECK(prepared_array.release != NULL, "dedicated prepared array");
        CHECK(prepared_schema.release != NULL, "dedicated prepared field");
        CHECK(prepared_array.length == 1, "dedicated prepared rows");
        char sidecar[64] = {0};
        long sidecar_len = 0;
        CHECK(read_object_bytes(
                  dedicated_path_copy, sidecar, sizeof(sidecar), &sidecar_len),
              "dedicated sidecar read");
        CHECK(sidecar_len == 10 && strcmp(sidecar, "alphaomega") == 0,
              "dedicated sidecar bytes");
        if (prepared_array.release) prepared_array.release(&prepared_array);
        if (prepared_schema.release) prepared_schema.release(&prepared_schema);
        lance_dedicated_blob_writer_close(dedicated);
    }

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

    LanceFileSession *dynamic_session =
        lance_file_session_new(base_uri, dynamic, NULL, NULL);
    CHECK(dynamic_session != NULL, "dynamic file session");
    LancePackedBlobWriter *dynamic_writer = NULL;
    if (dynamic_session) {
        dynamic_writer = lance_file_session_open_packed_blob_writer(
            dynamic_session, "c/dynamic-writer.lance", 33);
        CHECK(dynamic_writer != NULL, "dynamic packed writer");
    }
    if (dynamic_session) lance_file_session_close(dynamic_session);
    lance_storage_options_accessor_close(dynamic);
    lance_storage_options_provider_close(provider);
    if (dynamic_writer) {
        CHECK(lance_packed_blob_writer_write_blob(
                  dynamic_writer, (const uint8_t *)"alpha", 5) == 0,
              "dynamic writer after accessor close");
        CHECK(lance_packed_blob_writer_write_blob(
                  dynamic_writer, (const uint8_t *)"beta", 4) == 0,
              "dynamic writer second write");
        ArrowArray prepared_array = {0};
        ArrowSchema prepared_schema = {0};
        CHECK(lance_packed_blob_writer_finish_array(
                  dynamic_writer, "blob", &prepared_array,
                  &prepared_schema) == 0,
              "dynamic writer finish after owner close");
        CHECK(prepared_array.length == 2,
              "dynamic writer descriptor rows");
        if (prepared_array.release) prepared_array.release(&prepared_array);
        if (prepared_schema.release) prepared_schema.release(&prepared_schema);
        lance_packed_blob_writer_close(dynamic_writer);
    }

    CHECK(verify_released_blob_v2_raw_file(argv[1]),
          "released 14.0.2 Blob v2 raw file");

    lance_file_session_close(session);
    lance_storage_options_accessor_close(storage);
    free(schema.children);
    return failures == 0 ? 0 : 1;
}
