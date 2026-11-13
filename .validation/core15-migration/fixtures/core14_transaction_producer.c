/*
 * Produce historical Lance 14.0.2 transaction files for the core-15 migration
 * readback gate.
 *
 * This program is deliberately built against a Lance 14.0.2 lance-c library
 * and headers. It writes one attached transaction and one detached
 * transaction with fixed UUIDs, tags and properties, then prints the metadata
 * that the candidate verifier must observe.
 *
 * It is evidence tooling, not part of the candidate library or its CI.
 *
 * Build (server path names are examples; point them at a v14 prefix):
 *   cc -std=c11 -O2 core14_transaction_producer.c \
 *      -I <v14-prefix>/include -L <v14-prefix>/lib -llance_c \
 *      -Wl,-rpath,<v14-prefix>/lib -o core14_transaction_producer
 */

#include <lance/lance.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATTACHED_UUID "11111111-1111-1111-1111-111111111111"
#define ATTACHED_TAG "core14-attached"
#define DETACHED_UUID "22222222-2222-2222-2222-222222222222"
#define DETACHED_TAG "core14-detached"

static void noop_schema_release(struct ArrowSchema *schema) {
    if (schema != NULL) schema->release = NULL;
}

static void noop_array_release(struct ArrowArray *array) {
    if (array != NULL) array->release = NULL;
}

typedef struct {
    struct ArrowSchema schema;
    struct ArrowSchema field;
    struct ArrowSchema *schema_children[1];
    struct ArrowArray array;
    struct ArrowArray field_array;
    struct ArrowArray *array_children[1];
    const void *top_buffers[1];
    const void *field_buffers[2];
    int32_t values[2];
    int emitted;
} FixtureStream;

static int stream_get_schema(struct ArrowArrayStream *stream, struct ArrowSchema *out) {
    FixtureStream *fixture = (FixtureStream *)stream->private_data;
    *out = fixture->schema;
    out->release = noop_schema_release;
    return 0;
}

static int stream_get_next(struct ArrowArrayStream *stream, struct ArrowArray *out) {
    FixtureStream *fixture = (FixtureStream *)stream->private_data;
    if (fixture->emitted) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    fixture->emitted = 1;
    *out = fixture->array;
    out->release = noop_array_release;
    return 0;
}

static const char *stream_last_error(struct ArrowArrayStream *stream) {
    (void)stream;
    return NULL;
}

static void stream_release(struct ArrowArrayStream *stream) {
    if (stream != NULL) stream->release = NULL;
}

static void init_stream(FixtureStream *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->values[0] = 1;
    fixture->values[1] = 2;

    fixture->schema_children[0] = &fixture->field;
    fixture->schema.format = "+s";
    fixture->schema.name = NULL;
    fixture->schema.metadata = NULL;
    fixture->schema.flags = 0;
    fixture->schema.n_children = 1;
    fixture->schema.children = fixture->schema_children;
    fixture->schema.dictionary = NULL;
    fixture->schema.release = noop_schema_release;

    fixture->field.format = "i";
    fixture->field.name = "id";
    fixture->field.metadata = NULL;
    fixture->field.flags = 2; /* ARROW_FLAG_NULLABLE */
    fixture->field.n_children = 0;
    fixture->field.children = NULL;
    fixture->field.dictionary = NULL;
    fixture->field.release = noop_schema_release;

    fixture->top_buffers[0] = NULL;
    fixture->field_buffers[0] = NULL; /* all values are valid */
    fixture->field_buffers[1] = fixture->values;
    fixture->field_array.length = 2;
    fixture->field_array.null_count = 0;
    fixture->field_array.offset = 0;
    fixture->field_array.n_buffers = 2;
    fixture->field_array.buffers = fixture->field_buffers;
    fixture->field_array.n_children = 0;
    fixture->field_array.children = NULL;
    fixture->field_array.dictionary = NULL;
    fixture->field_array.release = noop_array_release;

    fixture->array_children[0] = &fixture->field_array;
    fixture->array.length = 2;
    fixture->array.null_count = 0;
    fixture->array.offset = 0;
    fixture->array.n_buffers = 1;
    fixture->array.buffers = fixture->top_buffers;
    fixture->array.n_children = 1;
    fixture->array.children = fixture->array_children;
    fixture->array.dictionary = NULL;
    fixture->array.release = noop_array_release;
}

static void fail(const char *what) {
    fprintf(stderr, "%s: %s\n", what, lance_last_error_message());
    exit(1);
}

static LanceTransaction *write_uncommitted(const char *uri, LanceWriteMode mode) {
    FixtureStream fixture;
    struct ArrowArrayStream stream;
    LanceUncommittedWriteOptions options;
    LanceTransaction *transaction = NULL;

    init_stream(&fixture);
    memset(&stream, 0, sizeof(stream));
    stream.get_schema = stream_get_schema;
    stream.get_next = stream_get_next;
    stream.get_last_error = stream_last_error;
    stream.release = stream_release;
    stream.private_data = &fixture;

    memset(&options, 0, sizeof(options));
    int32_t rc = lance_dataset_write_uncommitted_stream(
        uri, &fixture.schema, &stream, mode, &options, NULL, NULL, &transaction);
    if (rc != 0) fail("write_uncommitted");
    return transaction;
}

static LanceTransaction *rebuild(
    uint64_t read_version,
    const LanceTransaction *source,
    const char *uuid,
    const char *tag,
    const char *const *properties) {
    LanceOperation *operation = NULL;
    LanceTransactionOptions options;
    LanceTransaction *transaction;

    if (lance_transaction_operation(source, &operation) != 0) fail("transaction_operation");

    memset(&options, 0, sizeof(options));
    options.uuid = uuid;
    options.tag = tag;
    options.has_transaction_properties = true;
    options.property_keys = properties;
    options.property_values = properties + 2;
    options.property_count = 2;

    transaction = lance_transaction_new(read_version, operation, &options);
    if (transaction == NULL) fail("transaction_new");
    lance_operation_close(operation);
    return transaction;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <dataset-uri>\n", argv[0]);
        return 2;
    }
    const char *uri = argv[1];

    static const char *attached_properties[] = {
        "stage", "writer", "attached", "core14",
    };
    static const char *detached_properties[] = {
        "stage", "writer", "detached", "core14",
    };
    LanceCommitOptions commit_options;

    LanceTransaction *uncommitted = write_uncommitted(uri, LANCE_WRITE_CREATE);
    LanceTransaction *attached =
        rebuild(0, uncommitted, ATTACHED_UUID, ATTACHED_TAG, attached_properties);
    lance_transaction_close(uncommitted);

    LanceDataset *dataset = NULL;
    memset(&commit_options, 0, sizeof(commit_options));
    int32_t rc = lance_transaction_commit(
        uri, attached, &commit_options, NULL, NULL, &dataset);
    if (rc != 0) fail("commit attached");
    uint64_t attached_version = lance_dataset_version(dataset);
    lance_transaction_close(attached);
    lance_dataset_close(dataset);

    uncommitted = write_uncommitted(uri, LANCE_WRITE_APPEND);
    LanceTransaction *detached =
        rebuild(1, uncommitted, DETACHED_UUID, DETACHED_TAG, detached_properties);
    lance_transaction_close(uncommitted);

    commit_options.detached = true;
    dataset = NULL;
    rc = lance_transaction_commit(uri, detached, &commit_options, NULL, NULL, &dataset);
    if (rc != 0) fail("commit detached");
    uint64_t detached_version = lance_dataset_version(dataset);
    lance_transaction_close(detached);
    lance_dataset_close(dataset);

    printf("{\n");
    printf("  \"uri\": \"%s\",\n", uri);
    printf("  \"native\": \"Lance 14.0.2 (40f15ed1b71b66950961b175829fdf17a763f108)\",\n");
    printf("  \"attached\": {\"version\": %" PRIu64 ", \"read_version\": 0, "
           "\"uuid\": \"%s\", \"tag\": \"%s\", \"stage\": \"attached\", \"writer\": \"core14\"},\n",
           attached_version, ATTACHED_UUID, ATTACHED_TAG);
    printf("  \"detached\": {\"version\": %" PRIu64 ", \"read_version\": 1, "
           "\"uuid\": \"%s\", \"tag\": \"%s\", \"stage\": \"detached\", \"writer\": \"core14\"}\n",
           detached_version, DETACHED_UUID, DETACHED_TAG);
    printf("}\n");
    return 0;
}
