/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#include <lance/lance.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail_check(const char* message, int line) {
    const char* error = lance_last_error_message();
    fprintf(stderr, "FAIL: %s (line %d): %s\n", message, line,
            error != NULL ? error : "");
    exit(1);
}

#define CHECK(cond, message)                                                   \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fail_check(message, __LINE__);                                     \
        }                                                                      \
    } while (0)

typedef struct {
    struct ArrowSchema schema;
    struct ArrowSchema id_schema;
    struct ArrowArray array;
    struct ArrowArray id_array;
    struct ArrowSchema* schema_children[1];
    struct ArrowArray* array_children[1];
    const void* array_buffers[1];
    const void* id_buffers[2];
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
    fixture->schema_children[0] = &fixture->id_schema;
    fixture->schema.format = "+s";
    fixture->schema.n_children = 1;
    fixture->schema.children = fixture->schema_children;
    fixture->schema.release = noop_schema_release;
    fixture->id_schema.format = "i";
    fixture->id_schema.name = "id";
    fixture->id_schema.release = noop_schema_release;

    fixture->id_buffers[0] = NULL;
    fixture->id_buffers[1] = fixture->ids;
    fixture->id_array.length = 3;
    fixture->id_array.n_buffers = 2;
    fixture->id_array.buffers = fixture->id_buffers;
    fixture->id_array.release = noop_array_release;

    fixture->array_children[0] = &fixture->id_array;
    fixture->array.length = 3;
    fixture->array.n_buffers = 1;
    fixture->array_buffers[0] = NULL;
    fixture->array.buffers = fixture->array_buffers;
    fixture->array.n_children = 1;
    fixture->array.children = fixture->array_children;
    fixture->array.release = noop_array_release;
    fixture->stream.get_schema = stream_get_schema;
    fixture->stream.get_next = stream_get_next;
    fixture->stream.get_last_error = stream_last_error;
    fixture->stream.release = stream_release;
    fixture->stream.private_data = fixture;
}

static int contains(const char* haystack, const char* needle) {
    return haystack != NULL && strstr(haystack, needle) != NULL;
}

typedef struct {
    int describe_table;
    int describe_table_version;
    int create_table_version;
    int list_table_versions;
    int releases;
} DispatchCounts;

static void release_owned_callback_string(LanceOwnedString* value) {
    free(value->data);
    value->data = NULL;
    value->release = NULL;
}

static void set_owned_callback_string(LanceOwnedString* out, const char* value) {
    size_t length = strlen(value) + 1;
    char* copy = malloc(length);
    if (copy == NULL) {
        out->data = NULL;
        out->release = NULL;
        return;
    }
    memcpy(copy, value, length);
    out->data = copy;
    out->release = release_owned_callback_string;
}

static int32_t dispatch_callback(
    void* context,
    const char* operation,
    const char* request_json,
    LanceOwnedString* out_response_json,
    LanceCallbackError* out_error) {
    DispatchCounts* counts = (DispatchCounts*)context;
    (void)request_json;
    (void)out_error;
    if (strcmp(operation, "describe_table") == 0) {
        counts->describe_table++;
        set_owned_callback_string(out_response_json, "{}");
    } else if (strcmp(operation, "describe_table_version") == 0) {
        counts->describe_table_version++;
        set_owned_callback_string(
            out_response_json,
            "{\"version\":{\"version\":1,\"manifest_path\":\"manifest\"}}");
    } else if (strcmp(operation, "create_table_version") == 0) {
        counts->create_table_version++;
        set_owned_callback_string(out_response_json, "{}");
    } else if (strcmp(operation, "list_table_versions") == 0) {
        counts->list_table_versions++;
        set_owned_callback_string(out_response_json, "{\"versions\":[]}");
    } else {
        set_owned_callback_string(out_response_json, "{}");
    }
    return 0;
}

static void dispatch_release(void* context) {
    DispatchCounts* counts = (DispatchCounts*)context;
    counts->releases++;
}

static void check_call(
    LanceNamespace* ns, const char* operation, const char* request, const char* expected) {
    LanceNamespaceResult result;
    memset(&result, 0, sizeof(result));
    CHECK(lance_namespace_call(ns, operation, request, NULL, 0, &result) == 0, operation);
    CHECK(expected == NULL || contains(result.result_json, expected), operation);
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

int main(int argc, char** argv) {
    CHECK(argc > 1, "missing test root argument");
    char root[512];
    snprintf(root, sizeof(root), "%s/namespace", argv[1]);
    CHECK(mkdir(root, 0755) == 0 || access(root, F_OK) == 0, "namespace root");
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
        ns, "create_materialized_view",
        "{\"id\":[\"workspace\",\"mv\"],\"kind\":\"query\","
        "\"source_query\":\"select 1\",\"output_schema\":\"{}\"}",
        NULL, 0, &unsupported);
    CHECK(rc != 0, "unsupported must fail");
    CHECK(lance_last_error_code() == LANCE_ERR_NOT_SUPPORTED, "generic unsupported");
    CHECK(unsupported.namespace_error_code == 0 || unsupported.has_namespace_error == 0,
          "native unsupported code must stay zero when present");
    lance_namespace_result_close(&unsupported);

    DispatchCounts dispatch_counts = {0};
    LanceNamespaceCustomClientDispatchSpec dispatch_spec;
    memset(&dispatch_spec, 0, sizeof(dispatch_spec));
    dispatch_spec.dispatch = dispatch_callback;
    dispatch_spec.release = dispatch_release;
    dispatch_spec.client_ctx = &dispatch_counts;
    LanceNamespace* dispatch_ns = lance_namespace_custom_dispatch_new(&dispatch_spec);
    CHECK(dispatch_ns != NULL, "custom dispatch namespace");
    check_call(dispatch_ns, "describe_table",
               "{\"id\":[\"workspace\",\"numbers\"],\"version\":1}", NULL);
    check_call(dispatch_ns, "describe_table_version",
               "{\"id\":[\"workspace\",\"numbers\"],\"version\":1}", NULL);
    check_call(dispatch_ns, "create_table_version",
               "{\"id\":[\"workspace\",\"numbers\"],\"version\":1,"
               "\"manifest_path\":\"manifest\"}", NULL);
    check_call(dispatch_ns, "list_table_versions",
               "{\"id\":[\"workspace\",\"numbers\"],\"version\":1}", NULL);
    CHECK(dispatch_counts.describe_table == 1, "dispatch describe_table");
    CHECK(dispatch_counts.describe_table_version == 1, "dispatch describe_table_version");
    CHECK(dispatch_counts.create_table_version == 1, "dispatch create_table_version");
    CHECK(dispatch_counts.list_table_versions == 1, "dispatch list_table_versions");
    lance_namespace_close(dispatch_ns);
    CHECK(dispatch_counts.releases == 1, "dispatch context release");

    lance_namespace_close(ns);
    puts("native namespace C test passed");
    return 0;
}
