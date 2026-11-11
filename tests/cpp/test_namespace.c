/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#include <lance/lance.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CHECK(cond, message)                                                   \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL: %s (line %d): %s\n", message, __LINE__,     \
                    lance_last_error_message() ? lance_last_error_message() : ""); \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

typedef struct {
    struct ArrowSchema schema;
    struct ArrowArray array;
    struct ArrowSchema* schema_children[1];
    struct ArrowArray* array_children[1];
    const void* array_buffers[2];
    int32_t ids[3];
    int emitted;
    struct ArrowArrayStream stream;
} IntStream;

static void noop_schema_release(struct ArrowSchema* schema) {
    schema->release = NULL;
}

static void noop_array_release(struct ArrowArray* array) {
    array->release = NULL;
}

static int stream_get_schema(struct ArrowArrayStream* stream, struct ArrowSchema* out) {
    IntStream* fixture = (IntStream*)stream->private_data;
    *out = fixture->schema;
    out->release = noop_schema_release;
    return 0;
}

static int stream_get_next(struct ArrowArrayStream* stream, struct ArrowArray* out) {
    IntStream* fixture = (IntStream*)stream->private_data;
    if (fixture->emitted) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    fixture->emitted = 1;
    *out = fixture->array;
    out->release = noop_array_release;
    return 0;
}

static const char* stream_last_error(struct ArrowArrayStream* stream) {
    (void)stream;
    return NULL;
}

static void stream_release(struct ArrowArrayStream* stream) {
    stream->release = NULL;
}

static void init_stream(IntStream* fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->ids[0] = 1;
    fixture->ids[1] = 2;
    fixture->ids[2] = 3;
    fixture->schema.format = "i";
    fixture->schema.name = "id";
    fixture->array.length = 3;
    fixture->array.n_buffers = 2;
    fixture->array_buffers[0] = NULL;
    fixture->array_buffers[1] = fixture->ids;
    fixture->array.buffers = fixture->array_buffers;
    fixture->stream.get_schema = stream_get_schema;
    fixture->stream.get_next = stream_get_next;
    fixture->stream.get_last_error = stream_last_error;
    fixture->stream.release = stream_release;
    fixture->stream.private_data = fixture;
}

static int contains(const char* haystack, const char* needle) {
    return haystack != NULL && strstr(haystack, needle) != NULL;
}

static void check_call(
    LanceNamespace* ns, const char* operation, const char* request, const char* expected) {
    LanceNamespaceResult result;
    memset(&result, 0, sizeof(result));
    CHECK(lance_namespace_call(ns, operation, request, NULL, 0, &result) == 0, operation);
    CHECK(contains(result.result_json, expected), operation);
    lance_namespace_result_close(&result);
}

static void write_table(
    LanceNamespace* ns, const char* table_id, int32_t mode, IntStream* fixture) {
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    init_stream(fixture);
    CHECK(fixture->stream.get_schema(&fixture->stream, &schema) == 0, "stream schema");
    LanceDataset* dataset = NULL;
    int32_t rc = lance_dataset_write_namespace(
        ns, table_id, &schema, &fixture->stream, mode, NULL, NULL, NULL, NULL, NULL,
        &dataset);
    CHECK(rc == 0, "namespace write");
    CHECK(dataset != NULL, "write returned dataset");
    lance_dataset_close(dataset);
}

int main(void) {
    char root_template[] = "/tmp/lance-ns-c-XXXXXX";
    CHECK(mkdtemp(root_template) != NULL, "mkdtemp");
    char root[512];
    snprintf(root, sizeof(root), "%s/namespace", root_template);
    char properties[1024];
    snprintf(properties, sizeof(properties),
             "{\"root\":\"%s\",\"manifest_enabled\":\"true\","
             "\"table_version_tracking_enabled\":\"true\",\"ops_metrics_enabled\":\"true\"}",
             root);

    LanceNamespace* ns = lance_namespace_directory_new(properties, NULL, NULL);
    CHECK(ns != NULL, "directory namespace");
    char* id = lance_namespace_id(ns);
    CHECK(id != NULL && id[0] != '\0', "namespace id");
    lance_free_string(id);

    check_call(ns, "create_namespace", "{\"id\":[\"workspace\"]}", NULL);
    LanceNamespaceResult exists;
    memset(&exists, 0, sizeof(exists));
    CHECK(lance_namespace_call(ns, "namespace_exists", "{\"id\":[\"workspace\"]}",
                               NULL, 0, &exists) == 0, "namespace_exists");
    lance_namespace_result_close(&exists);

    IntStream fixture;
    const char* table_id = "[\"workspace\",\"numbers\"]";
    write_table(ns, table_id, LANCE_WRITE_CREATE, &fixture);

    LanceNamespaceResult count;
    memset(&count, 0, sizeof(count));
    CHECK(lance_namespace_call(ns, "count_table_rows", "{\"id\":[\"workspace\",\"numbers\"]}",
                               NULL, 0, &count) == 0, "count create");
    CHECK(strcmp(count.result_json, "3") == 0, "create rows");
    lance_namespace_result_close(&count);

    write_table(ns, table_id, LANCE_WRITE_APPEND, &fixture);
    memset(&count, 0, sizeof(count));
    CHECK(lance_namespace_call(ns, "count_table_rows", "{\"id\":[\"workspace\",\"numbers\"]}",
                               NULL, 0, &count) == 0, "count append");
    CHECK(strcmp(count.result_json, "6") == 0, "append rows");
    lance_namespace_result_close(&count);

    write_table(ns, table_id, LANCE_WRITE_OVERWRITE, &fixture);
    memset(&count, 0, sizeof(count));
    CHECK(lance_namespace_call(ns, "count_table_rows", "{\"id\":[\"workspace\",\"numbers\"]}",
                               NULL, 0, &count) == 0, "count overwrite");
    CHECK(strcmp(count.result_json, "3") == 0, "overwrite rows");
    lance_namespace_result_close(&count);

    LanceDataset* opened = lance_dataset_open_namespace(
        ns, table_id, NULL, 0, NULL);
    CHECK(opened != NULL, "namespace open");
    CHECK(lance_dataset_count_rows(opened) == 3, "reopen rows");
    lance_dataset_close(opened);

    char* metrics = lance_namespace_retrieve_ops_metrics(ns);
    CHECK(metrics != NULL && strstr(metrics, ":") != NULL, "metrics");
    lance_free_string(metrics);
    CHECK(lance_namespace_reset_ops_metrics(ns) == 0, "metrics reset");
    metrics = lance_namespace_retrieve_ops_metrics(ns);
    CHECK(metrics != NULL, "metrics after reset");
    lance_free_string(metrics);

    /* Directory does not implement materialized views: preserve native Unsupported. */
    LanceNamespaceResult unsupported;
    memset(&unsupported, 0, sizeof(unsupported));
    int32_t rc = lance_namespace_call(
        ns, "create_materialized_view", "{}", NULL, 0, &unsupported);
    CHECK(rc != 0, "unsupported must fail");
    CHECK(lance_last_error_code() == LANCE_ERR_NOT_SUPPORTED, "generic unsupported");
    CHECK(unsupported.has_namespace_error != 0, "native error present");
    CHECK(unsupported.namespace_error_code == 0, "native Unsupported code");
    CHECK(unsupported.namespace_error_message != NULL &&
              unsupported.namespace_error_message[0] != '\0',
          "native unsupported message");
    lance_namespace_result_close(&unsupported);

    lance_namespace_close(ns);
    puts("native namespace C test passed");
    return 0;
}
