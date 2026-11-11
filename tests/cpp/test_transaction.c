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

typedef struct StreamInput {
    struct ArrowArrayStream stream;
    struct ArrowSchema schema;
} StreamInput;

static StreamInput make_source_stream(const char *src_uri) {
    StreamInput input;
    memset(&input, 0, sizeof(input));

    LanceDataset *source = lance_dataset_open(src_uri, NULL, 0);
    ASSERT(source != NULL, "open source dataset");
    CHECK_OK();
    LanceScanner *scanner = lance_scanner_new(source, NULL, NULL);
    ASSERT(scanner != NULL, "create source scanner");
    CHECK_OK();
    ASSERT(lance_scanner_to_arrow_stream(scanner, &input.stream) == 0,
           "export source stream");
    CHECK_OK();
    ASSERT(input.stream.get_schema(&input.stream, &input.schema) == 0,
           "read source schema");
    CHECK_OK();
    lance_scanner_close(scanner);
    lance_dataset_close(source);
    return input;
}

static LanceTransaction *write_uncommitted_from_source(
    const char *src_uri,
    const char *dst_uri,
    int32_t mode,
    const LanceUncommittedWriteOptions *options) {
    StreamInput input = make_source_stream(src_uri);
    LanceTransaction *transaction = NULL;
    int32_t rc = lance_dataset_write_uncommitted_stream(
        dst_uri, &input.schema, &input.stream, mode, options, NULL, NULL,
        &transaction);
    if (input.schema.release) input.schema.release(&input.schema);
    ASSERT(rc == 0, "write uncommitted stream");
    CHECK_OK();
    ASSERT(transaction != NULL, "uncommitted transaction output");
    return transaction;
}

static uint64_t transaction_read_version(const LanceTransaction *transaction) {
    uint64_t version = UINT64_MAX;
    ASSERT(lance_transaction_read_version(transaction, &version) == 0,
           "read transaction version");
    CHECK_OK();
    return version;
}

static char *transaction_uuid(const LanceTransaction *transaction) {
    const char *uuid = lance_transaction_uuid(transaction);
    ASSERT(uuid != NULL, "read transaction UUID");
    CHECK_OK();
    size_t len = strlen(uuid) + 1;
    char *copy = malloc(len);
    ASSERT(copy != NULL, "copy transaction UUID");
    memcpy(copy, uuid, len);
    lance_free_string(uuid);
    return copy;
}

static LanceOperationKind transaction_operation_kind(
    const LanceTransaction *transaction) {
    LanceOperation *operation = NULL;
    ASSERT(lance_transaction_operation(transaction, &operation) == 0,
           "clone transaction operation");
    CHECK_OK();
    LanceOperationKind kind = LANCE_OPERATION_UNKNOWN;
    ASSERT(lance_operation_kind(operation, &kind) == 0,
           "read operation kind");
    CHECK_OK();
    lance_operation_close(operation);
    return kind;
}

static uint64_t transaction_fragment_count(
    const LanceTransaction *transaction) {
    LanceOperation *operation = NULL;
    ASSERT(lance_transaction_operation(transaction, &operation) == 0,
           "clone transaction operation for fragments");
    CHECK_OK();
    uint64_t count = UINT64_MAX;
    ASSERT(lance_operation_fragment_count(operation, &count) == 0,
           "read operation fragment count");
    CHECK_OK();
    lance_operation_close(operation);
    return count;
}

static void check_operation_schema(const LanceTransaction *transaction) {
    LanceOperation *operation = NULL;
    ASSERT(lance_transaction_operation(transaction, &operation) == 0,
           "clone transaction operation for schema");
    CHECK_OK();
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(lance_operation_schema(operation, &schema) == 0,
           "export overwrite operation schema");
    CHECK_OK();
    ASSERT(schema.release != NULL, "operation schema must carry a release callback");
    schema.release(&schema);
    lance_operation_close(operation);
}

static void check_properties(const LanceTransaction *transaction,
                             uint64_t expected_count) {
    bool present = false;
    ASSERT(lance_transaction_has_properties(transaction, &present) == 0,
           "read transaction property presence");
    CHECK_OK();
    ASSERT(present, "transaction properties must be explicitly present");
    uint64_t count = 0;
    ASSERT(lance_transaction_property_count(transaction, &count) == 0,
           "read transaction property count");
    CHECK_OK();
    ASSERT(count == expected_count, "transaction property count");
    for (uint64_t i = 0; i < count; ++i) {
        const char *key = lance_transaction_property_key_at(transaction, i);
        const char *value = lance_transaction_property_value_at(transaction, i);
        ASSERT(key != NULL && value != NULL, "transaction property entry");
        CHECK_OK();
        lance_free_string(key);
        lance_free_string(value);
    }
}

static void check_property_value(const LanceTransaction *transaction,
                                 const char *expected_key,
                                 const char *expected_value) {
    uint64_t count = 0;
    ASSERT(lance_transaction_property_count(transaction, &count) == 0,
           "read transaction property count");
    CHECK_OK();
    for (uint64_t i = 0; i < count; ++i) {
        const char *key = lance_transaction_property_key_at(transaction, i);
        const char *value = lance_transaction_property_value_at(transaction, i);
        ASSERT(key != NULL && value != NULL, "transaction property entry");
        int matches = strcmp(key, expected_key) == 0 &&
                      strcmp(value, expected_value) == 0;
        lance_free_string(key);
        lance_free_string(value);
        if (matches) return;
    }
    fprintf(stderr, "missing transaction property: %s=%s\n", expected_key,
            expected_value);
    exit(1);
}

static uint64_t scanner_row_count(LanceScanner *scanner) {
    struct ArrowArrayStream stream = {0};
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "export scanner stream");
    CHECK_OK();
    uint64_t rows = 0;
    while (1) {
        struct ArrowArray array = {0};
        ASSERT(stream.get_next(&stream, &array) == 0,
               "read scanner stream");
        if (!array.release) break;
        rows += (uint64_t)array.length;
        array.release(&array);
    }
    stream.release(&stream);
    return rows;
}

static LanceDataset *commit_transaction(
    const char *uri,
    const LanceTransaction *transaction,
    const LanceCommitOptions *options) {
    LanceDataset *dataset = NULL;
    ASSERT(lance_transaction_commit(uri, transaction, options, NULL, NULL,
                                    &dataset) == 0,
           "commit transaction");
    CHECK_OK();
    ASSERT(dataset != NULL, "commit dataset output");
    return dataset;
}

static void check_dataset(const char *uri,
                          uint64_t version,
                          uint64_t rows,
                          uint64_t fragments) {
    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "reopen committed dataset");
    CHECK_OK();
    ASSERT(lance_dataset_version(dataset) == version, "dataset version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(dataset) == rows, "dataset row count");
    CHECK_OK();
    if (fragments != UINT64_MAX) {
        ASSERT(lance_dataset_fragment_count(dataset) == fragments,
               "dataset fragment count");
        CHECK_OK();
    }
    lance_dataset_close(dataset);
}

static void write_c_replacement_file(const char *uri,
                                     const LanceFragment *fragment,
                                     const char *filename) {
    LanceScanner *scanner = lance_fragment_scan(fragment);
    ASSERT(scanner != NULL, "replacement fragment scanner");
    CHECK_OK();

    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "export replacement stream");
    CHECK_OK();
    lance_scanner_close(scanner);

    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(stream.get_schema(&stream, &schema) == 0,
           "read replacement schema");
    CHECK_OK();

    char data_uri[4096];
    snprintf(data_uri, sizeof(data_uri), "file://%s/data", uri);
    LanceFileSession *session =
        lance_file_session_new(data_uri, NULL, NULL, NULL);
    ASSERT(session != NULL, "replacement file session");
    CHECK_OK();
    LanceFileWriterOptions options;
    memset(&options, 0, sizeof(options));
    options.version = "2.1";
    LanceFileWriter *writer = lance_file_session_open_writer(
        session, filename, &schema, &options);
    ASSERT(writer != NULL, "replacement file writer");
    CHECK_OK();

    uint64_t rows = 0;
    while (1) {
        struct ArrowArray batch;
        memset(&batch, 0, sizeof(batch));
        ASSERT(stream.get_next(&stream, &batch) == 0,
               "read replacement batch");
        if (!batch.release) break;
        ASSERT(batch.n_children > 0, "replacement batch id column");
        int32_t *ids = (int32_t *)batch.children[0]->buffers[1];
        for (int64_t row = 0; row < batch.length; row++) {
            ids[row] += 999;
        }
        rows += (uint64_t)batch.length;
        LanceBatch *imported = NULL;
        ASSERT(lance_batch_from_arrow(&batch, &schema, &imported) == 0,
               "import replacement batch");
        CHECK_OK();
        ASSERT(lance_file_writer_write_batch(writer, imported) == 0,
               "write replacement batch");
        CHECK_OK();
        lance_batch_free(imported);
    }
    if (schema.release) schema.release(&schema);
    if (stream.release) stream.release(&stream);

    LanceFileWriteSummary summary;
    memset(&summary, 0, sizeof(summary));
    ASSERT(lance_file_writer_finish(writer, &summary) == 0,
           "finish replacement file");
    CHECK_OK();
    ASSERT(rows == 4 && summary.num_rows == 4,
           "replacement file must preserve four rows");
    ASSERT(summary.size_bytes > 0, "replacement file must be non-empty");
    lance_file_writer_close(writer);
    lance_file_session_close(session);
}

static int compare_i32(const void *left, const void *right) {
    int32_t a = *(const int32_t *)left;
    int32_t b = *(const int32_t *)right;
    return (a > b) - (a < b);
}

static uint64_t dataset_max_fragment_id(LanceDataset *dataset) {
    uint64_t count = lance_dataset_fragment_count(dataset);
    CHECK_OK();
    uint64_t *ids = (uint64_t *)calloc(count, sizeof(uint64_t));
    ASSERT(ids != NULL, "allocate fragment ids");
    ASSERT(lance_dataset_fragment_ids(dataset, ids) == 0,
           "read fragment ids for high-water mark");
    CHECK_OK();
    uint64_t max_id = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (ids[i] > max_id) max_id = ids[i];
    }
    free(ids);
    return max_id;
}

static size_t collect_c_ids(const char *uri, int32_t *out, size_t capacity) {
    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open replacement result");
    CHECK_OK();
    LanceScanner *scanner = lance_scanner_new(dataset, NULL, NULL);
    ASSERT(scanner != NULL, "replacement result scanner");
    CHECK_OK();
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "export replacement result");
    CHECK_OK();
    lance_scanner_close(scanner);
    lance_dataset_close(dataset);

    size_t count = 0;
    while (1) {
        struct ArrowArray batch;
        memset(&batch, 0, sizeof(batch));
        ASSERT(stream.get_next(&stream, &batch) == 0,
               "read replacement result batch");
        if (!batch.release) break;
        ASSERT(batch.n_children > 0, "replacement result id column");
        const int32_t *ids = (const int32_t *)batch.children[0]->buffers[1];
        ASSERT(count + (size_t)batch.length <= capacity,
               "replacement result capacity");
        for (int64_t row = 0; row < batch.length; row++) {
            out[count++] = ids[row];
        }
        batch.release(&batch);
    }
    if (stream.release) stream.release(&stream);
    qsort(out, count, sizeof(int32_t), compare_i32);
    return count;
}

static void test_create_append_overwrite(const char *src_uri,
                                         const char *base) {
    char create_uri[4096];
    char append_uri[4096];
    snprintf(create_uri, sizeof(create_uri), "%s/c_txn_create", base);
    snprintf(append_uri, sizeof(append_uri), "%s/c_txn_append", base);

    LanceDataset *source = lance_dataset_open(src_uri, NULL, 0);
    ASSERT(source != NULL, "open source dataset");
    CHECK_OK();
    uint64_t source_rows = lance_dataset_count_rows(source);
    CHECK_OK();
    lance_dataset_close(source);

    const char *properties[] = {"engine", "c11", "purpose", "transaction-test",
                                NULL};
    LanceUncommittedWriteOptions options;
    memset(&options, 0, sizeof(options));
    options.write.params.max_rows_per_file = 1;
    options.transaction_properties = properties;

    LanceTransaction *transaction = write_uncommitted_from_source(
        src_uri, create_uri, LANCE_WRITE_CREATE, &options);
    ASSERT(transaction_read_version(transaction) == 0,
           "create transaction read version");
    char *uuid = transaction_uuid(transaction);
    ASSERT(uuid[0] != '\0', "transaction UUID must be non-empty");
    const char *tag = (const char *)0x1;
    ASSERT(lance_transaction_tag(transaction, &tag) == 0,
           "create transaction tag is absent");
    CHECK_OK();
    ASSERT(tag == NULL, "absent tag output must be NULL");
    check_properties(transaction, 2);
    ASSERT(transaction_operation_kind(transaction) == LANCE_OPERATION_OVERWRITE,
           "create operation kind");
    uint64_t create_fragments = transaction_fragment_count(transaction);
    ASSERT(create_fragments > 1, "create must span multiple fragments");
    check_operation_schema(transaction);

    uint8_t *bytes = NULL;
    size_t len = 0;
    ASSERT(lance_transaction_to_proto(transaction, &bytes, &len) == 0,
           "serialize transaction");
    CHECK_OK();
    ASSERT(bytes != NULL && len > 0, "serialized transaction bytes");
    lance_transaction_close(transaction);
    transaction = lance_transaction_from_proto(bytes, len);
    lance_free_bytes(bytes);
    ASSERT(transaction != NULL, "parse transaction protobuf");
    CHECK_OK();
    char *parsed_uuid = transaction_uuid(transaction);
    ASSERT(strcmp(uuid, parsed_uuid) == 0,
           "protobuf handoff preserves UUID");
    free(parsed_uuid);

    LanceDataset *dataset = commit_transaction(create_uri, transaction, NULL);
    lance_transaction_close(transaction);
    ASSERT(lance_dataset_version(dataset) == 1, "create commits version 1");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(dataset) == source_rows,
           "create rows");
    CHECK_OK();
    ASSERT(lance_dataset_fragment_count(dataset) == create_fragments,
           "committed fragment count matches transaction");
    CHECK_OK();

    LanceTransaction *read_transaction = NULL;
    ASSERT(lance_dataset_read_transaction(dataset, &read_transaction) == 0,
           "read committed transaction");
    CHECK_OK();
    ASSERT(read_transaction != NULL, "committed transaction exists");
    char *read_uuid = transaction_uuid(read_transaction);
    ASSERT(strcmp(uuid, read_uuid) == 0, "committed transaction UUID");
    free(read_uuid);
    lance_transaction_close(read_transaction);
    lance_dataset_close(dataset);

    LanceTransaction *append = write_uncommitted_from_source(
        src_uri, create_uri, LANCE_WRITE_APPEND, NULL);
    ASSERT(transaction_read_version(append) == 1,
           "append transaction read version");
    ASSERT(transaction_operation_kind(append) == LANCE_OPERATION_APPEND,
           "append operation kind");
    check_dataset(create_uri, 1, source_rows, UINT64_MAX);

    LanceCommitOptions commit_options;
    memset(&commit_options, 0, sizeof(commit_options));
    commit_options.has_max_retries = true;
    commit_options.max_retries = 0;
    commit_options.has_retry_timeout = true;
    commit_options.timeout_mode = LANCE_COMMIT_TIMEOUT_DISABLED;
    dataset = commit_transaction(create_uri, append, &commit_options);
    lance_transaction_close(append);
    ASSERT(lance_dataset_version(dataset) == 2, "append commits version 2");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(dataset) == source_rows * 2,
           "append rows");
    CHECK_OK();
    lance_dataset_close(dataset);
    check_dataset(create_uri, 2, source_rows * 2, UINT64_MAX);

    LanceTransaction *overwrite = write_uncommitted_from_source(
        src_uri, create_uri, LANCE_WRITE_OVERWRITE, NULL);
    ASSERT(transaction_read_version(overwrite) == 2,
           "overwrite transaction read version");
    ASSERT(transaction_operation_kind(overwrite) == LANCE_OPERATION_OVERWRITE,
           "overwrite operation kind");
    dataset = commit_transaction(create_uri, overwrite, NULL);
    lance_transaction_close(overwrite);
    ASSERT(lance_dataset_version(dataset) == 3,
           "overwrite commits version 3");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(dataset) == source_rows,
           "overwrite replaces rows");
    CHECK_OK();
    lance_dataset_close(dataset);
    check_dataset(create_uri, 3, source_rows, UINT64_MAX);

    LanceTransaction *first = write_uncommitted_from_source(
        src_uri, append_uri, LANCE_WRITE_CREATE, NULL);
    LanceDataset *seed = commit_transaction(append_uri, first, NULL);
    lance_transaction_close(first);
    ASSERT(lance_dataset_version(seed) == 1, "batch seed version");
    CHECK_OK();
    lance_dataset_close(seed);

    LanceTransaction *left = write_uncommitted_from_source(
        src_uri, append_uri, LANCE_WRITE_APPEND, NULL);
    LanceTransaction *right = write_uncommitted_from_source(
        src_uri, append_uri, LANCE_WRITE_APPEND, NULL);
    const LanceTransaction *batch_inputs[] = {left, right};
    LanceDataset *batch_dataset = NULL;
    LanceTransaction *merged = NULL;
    ASSERT(lance_transaction_commit_batch(
               append_uri, batch_inputs, 2, NULL, NULL, NULL, &batch_dataset,
               &merged) == 0,
           "batch append commit");
    CHECK_OK();
    ASSERT(batch_dataset != NULL && merged != NULL,
           "batch outputs must be populated");
    ASSERT(transaction_read_version(merged) == 1,
           "merged batch read version");
    ASSERT(transaction_operation_kind(merged) == LANCE_OPERATION_APPEND,
           "merged batch operation kind");
    ASSERT(lance_dataset_version(batch_dataset) == 2,
           "one batch creates one version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(batch_dataset) == source_rows * 3,
           "batch append rows");
    CHECK_OK();
    lance_dataset_close(batch_dataset);
    lance_transaction_close(merged);

    LanceTransaction *overwrite_batch = write_uncommitted_from_source(
        src_uri, append_uri, LANCE_WRITE_OVERWRITE, NULL);
    const LanceTransaction *invalid_batch[] = {overwrite_batch};
    LanceDataset *sentinel_dataset = (LanceDataset *)(uintptr_t)0x1;
    LanceTransaction *sentinel_transaction =
        (LanceTransaction *)(uintptr_t)0x2;
    batch_dataset = sentinel_dataset;
    merged = sentinel_transaction;
    ASSERT(lance_transaction_commit_batch(
               append_uri, invalid_batch, 1, NULL, NULL, NULL, &batch_dataset,
               &merged) == -1,
           "non-append batch must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_NOT_SUPPORTED,
           "non-append batch error code");
    ASSERT(batch_dataset == sentinel_dataset && merged == sentinel_transaction,
           "failed batch must leave outputs untouched");
    lance_transaction_close(overwrite_batch);
    lance_transaction_close(left);
    lance_transaction_close(right);

    free(uuid);
    printf("create/append/overwrite/batch OK\n");
}

static void test_validation(const char *base) {
    char uri[4096];
    snprintf(uri, sizeof(uri), "%s/c_txn_validation", base);

    LanceTransaction *empty = lance_transaction_from_proto(NULL, 0);
    ASSERT(empty != NULL, "empty protobuf decodes to a native default transaction");
    ASSERT(transaction_operation_kind(empty) == LANCE_OPERATION_UNKNOWN,
           "default transaction has an Unknown operation");
    lance_transaction_close(empty);
    const uint8_t malformed[] = {0xFF};
    ASSERT(lance_transaction_from_proto(malformed, sizeof(malformed)) == NULL,
           "malformed protobuf must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "malformed protobuf error code");

    LanceCommitOptions options;
    memset(&options, 0, sizeof(options));
    options.timeout_mode = 99;
    LanceDataset *sentinel = (LanceDataset *)(uintptr_t)0x1;
    LanceDataset *out = sentinel;
    ASSERT(lance_transaction_commit(uri, NULL, &options, NULL, NULL, &out) ==
               -1,
           "NULL transaction commit must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL transaction error code");
    ASSERT(out == sentinel, "failed commit must leave output untouched");
    printf("validation OK\n");
}

static void test_fragment_delete(const char *src_uri, const char *base) {
    char uri[4096];
    snprintf(uri, sizeof(uri), "%s/c_fragment_delete", base);

    LanceUncommittedWriteOptions options;
    memset(&options, 0, sizeof(options));
    options.write.params.max_rows_per_file = 4;
    options.write.params.enable_stable_row_ids = true;
    LanceTransaction *create = write_uncommitted_from_source(
        src_uri, uri, LANCE_WRITE_CREATE, &options);
    LanceDataset *dataset = commit_transaction(uri, create, NULL);
    lance_transaction_close(create);
    ASSERT(lance_dataset_version(dataset) == 1, "delete fixture version 1");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(dataset) == 20,
           "delete fixture rows");
    CHECK_OK();
    ASSERT(lance_dataset_fragment_count(dataset) == 5,
           "delete fixture fragments");
    CHECK_OK();
    uint64_t num_deleted = 0;
    ASSERT(lance_dataset_delete(dataset, "id IN (2, 6)", &num_deleted) == 0,
           "seed existing deletion files");
    CHECK_OK();
    ASSERT(num_deleted == 2, "seed deletion count");

    uint64_t version = lance_dataset_version(dataset);
    CHECK_OK();
    uint64_t ids[5] = {0};
    ASSERT(lance_dataset_fragment_ids(dataset, ids) == 0,
           "delete fixture fragment IDs");
    CHECK_OK();
    LanceFragment *first = lance_dataset_open_fragment(dataset, ids[0]);
    LanceFragment *second = lance_dataset_open_fragment(dataset, ids[1]);
    ASSERT(first != NULL && second != NULL, "open delete fixture fragments");
    CHECK_OK();
    lance_dataset_close(dataset);

    LanceFragment *no_delete = (LanceFragment *)(uintptr_t)0x1;
    ASSERT(lance_fragment_delete(first, "false", &no_delete) == 0,
           "no-op fragment delete");
    CHECK_OK();
    ASSERT(no_delete != NULL, "no-op delete returns fragment metadata");
    ASSERT(lance_fragment_id(no_delete) == ids[0],
           "no-op delete preserves fragment ID");
    lance_fragment_close(no_delete);

    LanceFragment *all_deleted = (LanceFragment *)(uintptr_t)0x1;
    ASSERT(lance_fragment_delete(second, "true", &all_deleted) == 0,
           "full fragment delete");
    CHECK_OK();
    ASSERT(all_deleted == NULL, "full fragment delete writes NULL");

    LanceFragment *sentinel = (LanceFragment *)(uintptr_t)0x2;
    LanceFragment *staged = sentinel;
    ASSERT(lance_fragment_delete(second, NULL, &staged) == -1,
           "NULL delete predicate must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL predicate error code");
    ASSERT(staged == sentinel, "failed delete must leave output unchanged");
    const char *message = lance_last_error_message();
    if (message) lance_free_string(message);

    ASSERT(lance_fragment_delete(first, "id = 3", &staged) == 0,
           "partial fragment delete");
    CHECK_OK();
    ASSERT(staged != NULL, "partial delete returns updated metadata");
    uint64_t logical_rows = 0;
    ASSERT(lance_fragment_logical_rows(staged, &logical_rows) == 0,
           "staged logical row count");
    CHECK_OK();
    ASSERT(logical_rows == 2, "staged logical row count mismatch");
    LanceScanner *scanner = lance_fragment_scan(staged);
    ASSERT(scanner != NULL, "staged fragment scanner");
    CHECK_OK();
    ASSERT(scanner_row_count(scanner) == 2,
           "staged scanner must read the new deletion file");
    lance_scanner_close(scanner);

    const LanceFragment *updated[] = {staged};
    const uint64_t deleted[] = {ids[1]};
    LanceOperation *operation =
        lance_operation_delete_new(updated, 1, deleted, 1,
                                   "id = 3 OR true");
    ASSERT(operation != NULL, "construct Delete operation");
    CHECK_OK();
    LanceOperationKind kind = LANCE_OPERATION_UNKNOWN;
    ASSERT(lance_operation_kind(operation, &kind) == 0 &&
               kind == LANCE_OPERATION_DELETE,
           "Delete operation kind");
    CHECK_OK();
    uint64_t updated_count = 0;
    ASSERT(lance_operation_delete_updated_fragment_count(
               operation, &updated_count) == 0 &&
               updated_count == 1,
           "updated Delete fragment count");
    CHECK_OK();
    const char *updated_json =
        lance_operation_delete_updated_fragment_metadata_json_at(operation, 0);
    ASSERT(updated_json != NULL, "updated Delete metadata JSON");
    CHECK_OK();
    ASSERT(strstr(updated_json, "\"deletion_file\":{") != NULL,
           "updated metadata carries staged deletion file");
    lance_free_string(updated_json);
    uint64_t deleted_id = UINT64_MAX;
    ASSERT(lance_operation_delete_deleted_fragment_id_at(
               operation, 0, &deleted_id) == 0 &&
               deleted_id == ids[1],
           "deleted fragment ID");
    CHECK_OK();
    const char *operation_predicate =
        lance_operation_delete_predicate(operation);
    ASSERT(operation_predicate != NULL &&
               strcmp(operation_predicate, "id = 3 OR true") == 0,
           "Delete predicate inspection");
    lance_free_string(operation_predicate);

    const char *property_keys[] = {"engine=c11", "empty"};
    const char *property_values[] = {"用户", ""};
    LanceTransactionOptions transaction_options;
    memset(&transaction_options, 0, sizeof(transaction_options));
    transaction_options.tag = "c11-fragment-delete";
    transaction_options.has_transaction_properties = true;
    transaction_options.property_keys = property_keys;
    transaction_options.property_values = property_values;
    transaction_options.property_count = 2;
    LanceTransaction *transaction =
        lance_transaction_new(version, operation, &transaction_options);
    ASSERT(transaction != NULL, "construct Delete transaction");
    CHECK_OK();
    ASSERT(transaction_read_version(transaction) == version,
           "Delete transaction read version");
    const char *tag = NULL;
    ASSERT(lance_transaction_tag(transaction, &tag) == 1,
           "Delete transaction tag");
    CHECK_OK();
    ASSERT(tag != NULL && strcmp(tag, "c11-fragment-delete") == 0,
           "Delete transaction tag value");
    lance_free_string(tag);
    check_property_value(transaction, "engine=c11", "用户");
    check_property_value(transaction, "empty", "");

    uint8_t *bytes = NULL;
    size_t len = 0;
    ASSERT(lance_transaction_to_proto(transaction, &bytes, &len) == 0,
           "serialize Delete transaction");
    CHECK_OK();
    LanceTransaction *parsed = lance_transaction_from_proto(bytes, len);
    lance_free_bytes(bytes);
    ASSERT(parsed != NULL, "parse Delete transaction protobuf");
    CHECK_OK();
    ASSERT(transaction_operation_kind(parsed) == LANCE_OPERATION_DELETE,
           "parsed Delete operation kind");

    lance_fragment_close(staged);
    lance_fragment_close(first);
    lance_fragment_close(second);
    lance_operation_close(operation);

    LanceCommitOptions commit_options;
    memset(&commit_options, 0, sizeof(commit_options));
    commit_options.has_max_retries = true;
    commit_options.max_retries = 0;
    commit_options.timeout_mode = LANCE_COMMIT_TIMEOUT_DISABLED;
    dataset = commit_transaction(uri, parsed, &commit_options);
    lance_transaction_close(parsed);
    lance_transaction_close(transaction);
    ASSERT(lance_dataset_version(dataset) == version + 1,
           "Delete publishes one version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(dataset) == 14,
           "Delete publication row count");
    CHECK_OK();
    ASSERT(lance_dataset_fragment_count(dataset) == 4,
           "Delete publication fragment count");
    CHECK_OK();
    lance_dataset_close(dataset);

    dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "reopen Delete publication");
    CHECK_OK();
    ASSERT(lance_dataset_version(dataset) == version + 1 &&
               lance_dataset_count_rows(dataset) == 14,
           "Delete publication persisted");
    CHECK_OK();

    uint64_t current_ids[4] = {0};
    ASSERT(lance_dataset_fragment_ids(dataset, current_ids) == 0,
           "reopened fragment IDs");
    CHECK_OK();
    LanceFragment *left = lance_dataset_open_fragment(dataset, current_ids[0]);
    LanceFragment *right = lance_dataset_open_fragment(dataset, current_ids[0]);
    ASSERT(left != NULL && right != NULL, "open conflict fragments");
    CHECK_OK();
    uint64_t current_version = lance_dataset_version(dataset);
    CHECK_OK();
    lance_dataset_close(dataset);

    LanceFragment *left_update = NULL;
    LanceFragment *right_update = NULL;
    ASSERT(lance_fragment_delete(left, "id = 1", &left_update) == 0,
           "stage left conflict deletion");
    CHECK_OK();
    ASSERT(lance_fragment_delete(right, "id = 4", &right_update) == 0,
           "stage right conflict deletion");
    CHECK_OK();
    const LanceFragment *left_updates[] = {left_update};
    const LanceFragment *right_updates[] = {right_update};
    LanceOperation *left_operation = lance_operation_delete_new(
        left_updates, 1, NULL, 0, "id = 1");
    LanceOperation *right_operation = lance_operation_delete_new(
        right_updates, 1, NULL, 0, "id = 4");
    ASSERT(left_operation != NULL && right_operation != NULL,
           "construct conflict operations");
    CHECK_OK();
    LanceTransaction *left_transaction =
        lance_transaction_new(current_version, left_operation, NULL);
    LanceTransaction *right_transaction =
        lance_transaction_new(current_version, right_operation, NULL);
    ASSERT(left_transaction != NULL && right_transaction != NULL,
           "construct conflict transactions");
    CHECK_OK();
    dataset = commit_transaction(uri, left_transaction, &commit_options);
    ASSERT(lance_dataset_version(dataset) == current_version + 1,
           "conflict winner version");
    CHECK_OK();
    lance_dataset_close(dataset);

    LanceDataset *sentinel_dataset = (LanceDataset *)(uintptr_t)0x1;
    LanceDataset *failed_output = sentinel_dataset;
    ASSERT(lance_transaction_commit(uri, right_transaction, &commit_options,
                                    NULL, NULL, &failed_output) == -1,
           "same-fragment delete conflict");
    ASSERT(lance_last_error_code() == LANCE_ERR_COMMIT_CONFLICT,
           "same-fragment delete conflict code");
    ASSERT(failed_output == sentinel_dataset,
           "failed conflict must leave output unchanged");
    check_dataset(uri, current_version + 1, 13, 4);

    lance_fragment_close(left_update);
    lance_fragment_close(right_update);
    lance_fragment_close(left);
    lance_fragment_close(right);
    lance_operation_close(left_operation);
    lance_operation_close(right_operation);
    lance_transaction_close(left_transaction);
    lance_transaction_close(right_transaction);
    printf("fragment delete OK\n");
}

static void test_data_replacement(const char *src_uri, const char *base) {
    char uri[4096];
    snprintf(uri, sizeof(uri), "%s/c_data_replacement", base);

    LanceUncommittedWriteOptions options;
    memset(&options, 0, sizeof(options));
    options.write.params.max_rows_per_file = 4;
    options.write.params.data_storage_version = "2.1";
    LanceTransaction *create = write_uncommitted_from_source(
        src_uri, uri, LANCE_WRITE_CREATE, &options);
    LanceDataset *dataset = commit_transaction(uri, create, NULL);
    lance_transaction_close(create);
    ASSERT(lance_dataset_version(dataset) == 1,
           "data replacement fixture version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(dataset) == 20,
           "data replacement fixture rows");
    CHECK_OK();
    ASSERT(lance_dataset_fragment_count(dataset) == 5,
           "data replacement fixture fragments");
    CHECK_OK();

    uint64_t ids[5] = {0};
    ASSERT(lance_dataset_fragment_ids(dataset, ids) == 0,
           "data replacement fragment IDs");
    CHECK_OK();
    LanceFragment *fragment = lance_dataset_open_fragment(dataset, ids[0]);
    ASSERT(fragment != NULL, "open data replacement fragment");
    CHECK_OK();
    const char *filename = "c-data-replacement.lance";
    write_c_replacement_file(uri, fragment, filename);
    lance_fragment_close(fragment);

    LanceDataFile *file = NULL;
    ASSERT(lance_dataset_create_data_file(
               dataset, filename, NULL, &file) == 0,
           "create replacement data file");
    CHECK_OK();
    ASSERT(file != NULL, "replacement data file output");
    const char *path = lance_data_file_path(file);
    ASSERT(path != NULL && strcmp(path, filename) == 0,
           "replacement data file path");
    CHECK_OK();
    size_t field_count = lance_data_file_field_count(file);
    ASSERT(lance_last_error_code() == LANCE_OK && field_count > 0,
           "replacement data file fields");
    int32_t field_id = INT32_MIN;
    ASSERT(lance_data_file_field_id_at(file, 0, &field_id) == 0,
           "replacement data file field ID");
    CHECK_OK();
    size_t column_count = lance_data_file_column_index_count(file);
    ASSERT(lance_last_error_code() == LANCE_OK && column_count > 0,
           "replacement data file column indices");
    int32_t column_index = INT32_MIN;
    ASSERT(lance_data_file_column_index_at(file, 0, &column_index) == 0,
           "replacement data file column index");
    CHECK_OK();
    uint32_t major = UINT32_MAX;
    uint32_t minor = UINT32_MAX;
    ASSERT(lance_data_file_file_version(file, &major, &minor) == 0 &&
               major == 2 && minor == 1,
           "replacement data file version");
    CHECK_OK();
    bool present = true;
    uint64_t size = 0;
    ASSERT(lance_data_file_file_size(file, &present, &size) == 0 &&
               present && size > 0,
           "replacement data file size");
    CHECK_OK();
    present = true;
    uint32_t base_id = UINT32_MAX;
    ASSERT(lance_data_file_base_id(file, &present, &base_id) == 0 &&
               !present,
           "replacement data file base ID is absent");
    CHECK_OK();

    LanceDataFile *file_clone = lance_data_file_clone(file);
    ASSERT(file_clone != NULL, "clone replacement data file");
    CHECK_OK();
    const char *clone_path = lance_data_file_path(file_clone);
    ASSERT(clone_path != NULL && strcmp(clone_path, filename) == 0,
           "cloned replacement data file path");
    CHECK_OK();
    lance_data_file_close(file_clone);

    LanceDataReplacementGroup *group =
        lance_data_replacement_group_new(ids[0], file);
    ASSERT(group != NULL, "construct replacement group");
    CHECK_OK();
    lance_data_file_close(file);
    ASSERT(lance_data_replacement_group_fragment_id(group) == ids[0],
           "replacement group fragment ID");
    CHECK_OK();

    LanceDataFile *group_file = NULL;
    ASSERT(lance_data_replacement_group_file(group, &group_file) == 0,
           "replacement group file");
    CHECK_OK();
    ASSERT(group_file != NULL, "replacement group file output");
    const char *group_path = lance_data_file_path(group_file);
    ASSERT(group_path != NULL && strcmp(group_path, filename) == 0,
           "replacement group file path");
    CHECK_OK();
    lance_data_file_close(group_file);

    const LanceDataReplacementGroup *groups[] = {group};
    LanceOperation *operation =
        lance_operation_data_replacement_new(groups, 1);
    ASSERT(operation != NULL, "construct DataReplacement operation");
    CHECK_OK();
    LanceOperationKind kind = LANCE_OPERATION_UNKNOWN;
    ASSERT(lance_operation_kind(operation, &kind) == 0 &&
               kind == LANCE_OPERATION_DATA_REPLACEMENT,
           "DataReplacement operation kind");
    CHECK_OK();
    uint64_t replacement_count = UINT64_MAX;
    ASSERT(lance_operation_data_replacement_count(
               operation, &replacement_count) == 0 &&
               replacement_count == 1,
           "DataReplacement operation count");
    CHECK_OK();
    LanceDataReplacementGroup *inspected = NULL;
    ASSERT(lance_operation_data_replacement_at(
               operation, 0, &inspected) == 0,
           "inspect DataReplacement group");
    CHECK_OK();
    ASSERT(inspected != NULL &&
               lance_data_replacement_group_fragment_id(inspected) == ids[0],
           "inspected DataReplacement fragment ID");
    CHECK_OK();
    lance_data_replacement_group_close(inspected);
    lance_data_replacement_group_close(group);

    LanceOperation *empty =
        lance_operation_data_replacement_new(NULL, 0);
    ASSERT(empty != NULL, "empty DataReplacement operation");
    CHECK_OK();
    ASSERT(lance_operation_data_replacement_count(
               empty, &replacement_count) == 0 &&
               replacement_count == 0,
           "empty DataReplacement operation count");
    CHECK_OK();
    lance_operation_close(empty);

    LanceOperation *invalid =
        lance_operation_data_replacement_new(NULL, 1);
    ASSERT(invalid == NULL, "NULL replacement array must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL replacement array error code");
    LanceDataReplacementGroup *null_group = NULL;
    const LanceDataReplacementGroup *invalid_groups[] = {null_group};
    invalid = lance_operation_data_replacement_new(invalid_groups, 1);
    ASSERT(invalid == NULL, "NULL replacement group must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL replacement group error code");
    LanceDataReplacementGroup *sentinel_group =
        (LanceDataReplacementGroup *)(uintptr_t)0x1;
    ASSERT(lance_operation_data_replacement_at(
               operation, 1, &sentinel_group) == -1,
           "out-of-range DataReplacement inspection");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "out-of-range DataReplacement error code");
    ASSERT(sentinel_group == (LanceDataReplacementGroup *)(uintptr_t)0x1,
           "failed DataReplacement inspection preserves output");

    LanceOperation *delete_operation =
        lance_operation_delete_new(NULL, 0, NULL, 0, "");
    ASSERT(delete_operation != NULL, "Delete validation operation");
    CHECK_OK();
    ASSERT(lance_operation_data_replacement_count(
               delete_operation, &replacement_count) == -1,
           "wrong variant DataReplacement inspection");
    ASSERT(lance_last_error_code() == LANCE_ERR_NOT_SUPPORTED,
           "wrong variant DataReplacement error code");
    lance_operation_close(delete_operation);

    uint64_t deleted = 0;
    ASSERT(lance_dataset_delete(dataset, "id = 2", &deleted) == 0 &&
               deleted == 1,
           "delete replacement fixture row");
    CHECK_OK();
    ASSERT(lance_dataset_version(dataset) == 2,
           "replacement fixture deletion version");
    CHECK_OK();
    LanceTransaction *transaction =
        lance_transaction_new(2, operation, NULL);
    ASSERT(transaction != NULL, "construct DataReplacement transaction");
    CHECK_OK();
    lance_operation_close(operation);

    LanceCommitOptions commit_options;
    memset(&commit_options, 0, sizeof(commit_options));
    commit_options.has_max_retries = true;
    commit_options.max_retries = 0;
    commit_options.timeout_mode = LANCE_COMMIT_TIMEOUT_DISABLED;
    LanceDataset *committed =
        commit_transaction(uri, transaction, &commit_options);
    lance_transaction_close(transaction);
    ASSERT(lance_dataset_version(committed) == 3,
           "DataReplacement publishes one version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(committed) == 19,
           "DataReplacement row count");
    CHECK_OK();
    ASSERT(lance_dataset_fragment_count(committed) == 5,
           "DataReplacement fragment count");
    CHECK_OK();

    LanceTransaction *history = NULL;
    ASSERT(lance_dataset_read_transaction(committed, &history) == 0,
           "read DataReplacement history");
    CHECK_OK();
    ASSERT(history != NULL, "DataReplacement history exists");
    LanceOperation *history_operation = NULL;
    ASSERT(lance_transaction_operation(history, &history_operation) == 0,
           "clone DataReplacement history operation");
    CHECK_OK();
    ASSERT(lance_operation_kind(history_operation, &kind) == 0 &&
               kind == LANCE_OPERATION_DATA_REPLACEMENT,
           "history DataReplacement kind");
    CHECK_OK();
    ASSERT(lance_operation_data_replacement_count(
               history_operation, &replacement_count) == 0 &&
               replacement_count == 1,
           "history DataReplacement count");
    CHECK_OK();
    lance_operation_close(history_operation);
    lance_transaction_close(history);
    lance_dataset_close(committed);
    lance_dataset_close(dataset);

    int32_t actual[19] = {0};
    ASSERT(collect_c_ids(uri, actual, 19) == 19,
           "reopened DataReplacement row count");
    const int32_t expected[19] = {5,    6,    7,    8,    9,    10,   11,
                                  12,   13,   14,   15,   16,   17,   18,
                                  19,   20,   1000, 1002, 1003};
    for (size_t i = 0; i < 19; i++) {
        ASSERT(actual[i] == expected[i],
               "reopened DataReplacement fixed row value");
    }

    printf("data replacement OK\n");
}


/* ─── Native schema and Project ─────────────────────────────────────────── */

typedef struct RowPair {
    int64_t id;
    uint64_t rowid;
} RowPair;

static size_t read_id_rowid_pairs(LanceDataset *dataset,
                                  RowPair *pairs,
                                  size_t capacity) {
    const char *columns[] = {"id", NULL};
    LanceScanner *scanner = lance_scanner_new(dataset, columns, NULL);
    ASSERT(scanner != NULL, "create schema fixture scanner");
    CHECK_OK();
    ASSERT(lance_scanner_with_row_id(scanner, true) == 0,
           "enable row IDs on schema fixture scanner");
    CHECK_OK();
    struct ArrowArrayStream stream = {0};
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "export schema fixture scanner stream");
    CHECK_OK();
    size_t count = 0;
    for (;;) {
        struct ArrowArray batch = {0};
        ASSERT(stream.get_next(&stream, &batch) == 0,
               "read schema fixture batch");
        CHECK_OK();
        if (!batch.release) break;
        ASSERT(batch.n_children == 2,
               "schema fixture scan must return id and _rowid");
        const struct ArrowArray *ids = batch.children[0];
        const struct ArrowArray *rowids = batch.children[1];
        ASSERT(ids->buffers[1] != NULL && rowids->buffers[1] != NULL,
               "schema fixture id/_rowid buffers");
        const int64_t *id_values = (const int64_t *)ids->buffers[1];
        const uint64_t *rowid_values = (const uint64_t *)rowids->buffers[1];
        for (int64_t i = 0; i < batch.length; i++) {
            ASSERT(count < capacity, "schema fixture row capacity");
            pairs[count].id = id_values[ids->offset + i];
            pairs[count].rowid = rowid_values[rowids->offset + i];
            count++;
        }
        batch.release(&batch);
    }
    stream.release(&stream);
    lance_scanner_close(scanner);
    return count;
}

static LanceSchema *clone_dataset_schema(LanceDataset *dataset) {
    LanceSchema *schema = NULL;
    ASSERT(lance_dataset_schema_clone(dataset, &schema) == 0,
           "clone dataset native schema");
    CHECK_OK();
    ASSERT(schema != NULL, "native schema output");
    return schema;
}

static void check_field_id(const LanceSchema *schema,
                           const char *column,
                           int32_t expected) {
    int32_t id = -1;
    ASSERT(lance_schema_field_id(schema, column, &id) == 0,
           "read native schema field ID");
    CHECK_OK();
    ASSERT(id == expected, "native schema field ID mismatch");
}

static void test_native_schema(const char *uri) {
    LanceDataset *old_dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(old_dataset != NULL, "open native schema fixture");
    CHECK_OK();
    const uint64_t old_version = lance_dataset_version(old_dataset);
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(old_dataset) == 8,
           "native schema fixture row count");
    CHECK_OK();

    LanceSchema *schema = clone_dataset_schema(old_dataset);
    check_field_id(schema, "id", 0);
    check_field_id(schema, "name", 2);
    check_field_id(schema, "profile", 3);
    check_field_id(schema, "profile.score", 4);
    check_field_id(schema, "profile.tags", 5);

    struct ArrowSchema arrow = {0};
    ASSERT(lance_schema_to_arrow(schema, &arrow) == 0,
           "export native schema to Arrow");
    CHECK_OK();
    ASSERT(arrow.n_children == 3, "native schema Arrow field count");
    ASSERT(strcmp(arrow.children[0]->name, "id") == 0,
           "native schema Arrow first field");
    ASSERT(strcmp(arrow.children[2]->name, "profile") == 0,
           "native schema Arrow third field");
    arrow.release(&arrow);

    uint64_t fragment_ids[16] = {0};
    ASSERT(lance_dataset_fragment_count(old_dataset) == 3,
           "native schema fixture fragment count");
    CHECK_OK();
    ASSERT(lance_dataset_fragment_ids(old_dataset, fragment_ids) == 0,
           "native schema fixture fragment IDs");
    CHECK_OK();
    LanceFragment *fragment =
        lance_dataset_open_fragment(old_dataset, fragment_ids[0]);
    ASSERT(fragment != NULL, "open native schema fragment");
    CHECK_OK();
    LanceSchema *fragment_schema = NULL;
    ASSERT(lance_fragment_schema_clone(fragment, &fragment_schema) == 0,
           "clone fragment native schema");
    CHECK_OK();
    lance_fragment_close(fragment);
    /* The clone must outlive its fragment owner. */
    check_field_id(fragment_schema, "profile.tags", 5);
    lance_schema_close(fragment_schema);

    RowPair before[16] = {0};
    size_t before_count = read_id_rowid_pairs(old_dataset, before, 16);
    ASSERT(before_count == 8, "native schema fixture row ID count");

    const char *project_columns[] = {"id", "name"};
    LanceSchema *projected = NULL;
    ASSERT(lance_schema_project(schema, project_columns, 2, &projected) == 0,
           "project native schema");
    CHECK_OK();
    check_field_id(projected, "id", 0);
    check_field_id(projected, "name", 2);
    int32_t missing_id = -7;
    const char *missing_column = "profile.tags";
    ASSERT(lance_schema_field_id(projected, missing_column, &missing_id) == -1,
           "projected schema has no dropped field");
    ASSERT(lance_last_error_code() == LANCE_ERR_INTERNAL,
           "dropped field error code");
    ASSERT(missing_id == -7, "failed field ID lookup leaves output");

    LanceSchema *clone = lance_schema_clone(schema);
    ASSERT(clone != NULL, "clone native schema value");
    CHECK_OK();
    lance_schema_close(schema);
    check_field_id(clone, "profile.score", 4);
    struct ArrowSchema clone_arrow = {0};
    ASSERT(lance_schema_to_arrow(clone, &clone_arrow) == 0,
           "arrow export from independent native schema clone");
    CHECK_OK();
    ASSERT(clone_arrow.n_children == 3, "clone Arrow field count");
    clone_arrow.release(&clone_arrow);
    lance_schema_close(clone);

    LanceOperation *project_operation = NULL;
    ASSERT(lance_operation_project_new(projected, false,
                                       &project_operation) == 0,
           "construct Project operation");
    CHECK_OK();
    LanceOperationKind kind = LANCE_OPERATION_UNKNOWN;
    ASSERT(lance_operation_kind(project_operation, &kind) == 0,
           "Project operation kind");
    CHECK_OK();
    ASSERT(kind == LANCE_OPERATION_PROJECT, "Project operation enum value");
    bool preserves = true;
    ASSERT(lance_operation_preserves_nullability(project_operation,
                                                 &preserves) == 0,
           "read Project preserves_nullability");
    CHECK_OK();
    ASSERT(!preserves, "Project preserves_nullability false");

    LanceSchema *operation_schema = NULL;
    ASSERT(lance_operation_schema_clone(project_operation,
                                        &operation_schema) == 0,
           "clone Project operation schema");
    CHECK_OK();
    check_field_id(operation_schema, "name", 2);
    lance_schema_close(operation_schema);
    lance_operation_close(project_operation);

    LanceOperation *preserving_operation = NULL;
    ASSERT(lance_operation_project_new(projected, true,
                                       &preserving_operation) == 0,
           "construct preserving Project operation");
    CHECK_OK();
    preserves = false;
    ASSERT(lance_operation_preserves_nullability(preserving_operation,
                                                 &preserves) == 0,
           "read preserving Project flag");
    CHECK_OK();
    ASSERT(preserves, "Project preserves_nullability true");
    lance_operation_close(preserving_operation);

    /* Rebuild the operation used for publication, then close its schema
       source before committing: publication must own its schema. */
    project_operation = NULL;
    ASSERT(lance_operation_project_new(projected, false,
                                       &project_operation) == 0,
           "rebuild Project operation");
    CHECK_OK();
    lance_schema_close(projected);

    LanceTransaction *transaction =
        lance_transaction_new(old_version, project_operation, NULL);
    ASSERT(transaction != NULL, "construct Project transaction");
    CHECK_OK();
    ASSERT(transaction_read_version(transaction) == old_version,
           "Project transaction read version");
    ASSERT(transaction_operation_kind(transaction) == LANCE_OPERATION_PROJECT,
           "Project transaction kind");

    LanceDataset *committed = commit_transaction(uri, transaction, NULL);
    ASSERT(lance_dataset_version(committed) == old_version + 1,
           "Project publication version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(committed) == 8,
           "Project publication row count");
    CHECK_OK();
    ASSERT(lance_dataset_fragment_count(committed) == 3,
           "Project publication fragment count");
    CHECK_OK();
    lance_transaction_close(transaction);
    lance_operation_close(project_operation);
    lance_dataset_close(committed);

    LanceDataset *reopened = lance_dataset_open(uri, NULL, 0);
    ASSERT(reopened != NULL, "reopen projected dataset");
    CHECK_OK();
    ASSERT(lance_dataset_version(reopened) == old_version + 1,
           "projected dataset persisted version");
    ASSERT(lance_dataset_count_rows(reopened) == 8,
           "projected dataset persisted rows");
    CHECK_OK();
    schema = clone_dataset_schema(reopened);
    check_field_id(schema, "id", 0);
    check_field_id(schema, "name", 2);
    int32_t dropped_id = -9;
    ASSERT(lance_schema_field_id(schema, "profile.tags", &dropped_id) == -1,
           "projected dataset dropped profile");
    ASSERT(dropped_id == -9, "projected dataset dropped output unchanged");
    struct ArrowSchema projected_arrow = {0};
    ASSERT(lance_schema_to_arrow(schema, &projected_arrow) == 0,
           "projected dataset Arrow export");
    CHECK_OK();
    ASSERT(projected_arrow.n_children == 2,
           "projected dataset Arrow field count");
    projected_arrow.release(&projected_arrow);
    lance_schema_close(schema);

    ASSERT(lance_dataset_count_rows_filtered(reopened, "id = 2") == 0,
           "projected dataset keeps deletion");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows_filtered(reopened, "name = 'alpha'") == 3,
           "projected dictionary value alpha");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows_filtered(reopened, "name = 'beta'") == 2,
           "projected dictionary value beta");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows_filtered(reopened, "name = 'gamma'") == 3,
           "projected dictionary value gamma");
    CHECK_OK();

    RowPair after[16] = {0};
    size_t after_count = read_id_rowid_pairs(reopened, after, 16);
    ASSERT(after_count == before_count,
           "projected dataset stable row ID count");
    for (size_t i = 0; i < after_count; i++) {
        ASSERT(after[i].id == before[i].id,
               "projected dataset row order and values");
        ASSERT(after[i].rowid == before[i].rowid,
               "projected dataset row IDs unchanged");
    }

    /* Two Project operations staged at the same version: the stale one must
       report a native commit conflict, and its output slot must stay untouched. */
    LanceSchema *winner_schema = clone_dataset_schema(reopened);
    const char *winner_columns[] = {"name"};
    LanceSchema *winner_projected = NULL;
    ASSERT(lance_schema_project(winner_schema, winner_columns, 1,
                                &winner_projected) == 0,
           "project winner schema");
    CHECK_OK();
    lance_schema_close(winner_schema);
    LanceOperation *winner_operation = NULL;
    ASSERT(lance_operation_project_new(winner_projected, false,
                                       &winner_operation) == 0,
           "construct winner Project");
    CHECK_OK();
    lance_schema_close(winner_projected);
    LanceTransaction *winner_transaction =
        lance_transaction_new(old_version + 1, winner_operation, NULL);
    ASSERT(winner_transaction != NULL, "construct winner transaction");
    CHECK_OK();

    LanceSchema *stale_schema = clone_dataset_schema(reopened);
    const char *stale_columns[] = {"id"};
    LanceSchema *stale_projected = NULL;
    ASSERT(lance_schema_project(stale_schema, stale_columns, 1,
                                &stale_projected) == 0,
           "project stale schema");
    CHECK_OK();
    lance_schema_close(stale_schema);
    LanceOperation *stale_operation = NULL;
    ASSERT(lance_operation_project_new(stale_projected, false,
                                       &stale_operation) == 0,
           "construct stale Project");
    CHECK_OK();
    lance_schema_close(stale_projected);
    LanceTransaction *stale_transaction =
        lance_transaction_new(old_version + 1, stale_operation, NULL);
    ASSERT(stale_transaction != NULL, "construct stale transaction");
    CHECK_OK();

    LanceCommitOptions zero_retries;
    memset(&zero_retries, 0, sizeof(zero_retries));
    zero_retries.has_max_retries = true;
    zero_retries.max_retries = 0;
    zero_retries.timeout_mode = LANCE_COMMIT_TIMEOUT_DISABLED;
    LanceDataset *winner_dataset =
        commit_transaction(uri, winner_transaction, &zero_retries);
    ASSERT(lance_dataset_version(winner_dataset) == old_version + 2,
           "winner Project version");
    CHECK_OK();
    lance_dataset_close(winner_dataset);

    LanceDataset *conflict_sentinel = (LanceDataset *)(uintptr_t)0x4;
    LanceDataset *conflict_output = conflict_sentinel;
    ASSERT(lance_transaction_commit(uri, stale_transaction, &zero_retries,
                                    NULL, NULL, &conflict_output) == -1,
           "stale Project commit must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_COMMIT_CONFLICT,
           "stale Project commit conflict code");
    ASSERT(conflict_output == conflict_sentinel,
           "stale Project commit leaves output unchanged");
    lance_operation_close(winner_operation);
    lance_transaction_close(winner_transaction);
    lance_operation_close(stale_operation);
    lance_transaction_close(stale_transaction);

    LanceDataset *winner_reopened = lance_dataset_open(uri, NULL, 0);
    ASSERT(winner_reopened != NULL, "reopen winner Project dataset");
    CHECK_OK();
    ASSERT(lance_dataset_version(winner_reopened) == old_version + 2,
           "winner Project persisted version");
    CHECK_OK();
    LanceSchema *winner_schema_after = clone_dataset_schema(winner_reopened);
    check_field_id(winner_schema_after, "name", 2);
    int32_t id_after_conflict = -1;
    ASSERT(lance_schema_field_id(winner_schema_after, "id",
                                 &id_after_conflict) == -1,
           "winner Project dropped id");
    ASSERT(id_after_conflict == -1, "winner Project leaves field id untouched");
    lance_schema_close(winner_schema_after);
    lance_dataset_close(winner_reopened);

    /* The pre-commit snapshot remains readable with its original schema and
       deletion state. */
    ASSERT(lance_dataset_count_rows(old_dataset) == 8,
           "old snapshot rows unchanged");
    CHECK_OK();
    LanceSchema *old_schema = clone_dataset_schema(old_dataset);
    check_field_id(old_schema, "profile.tags", 5);
    lance_schema_close(old_schema);
    lance_dataset_close(old_dataset);
    lance_dataset_close(reopened);

    /* Null, kind, and output-slot guards. */
    LanceSchema *null_clone = lance_schema_clone(NULL);
    ASSERT(null_clone == NULL, "NULL schema clone returns NULL");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL schema clone error code");

    LanceSchema *sentinel_schema = (LanceSchema *)(uintptr_t)0x1;
    LanceSchema *schema_out = sentinel_schema;
    ASSERT(lance_dataset_schema_clone(NULL, &schema_out) == -1,
           "NULL dataset schema clone");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL dataset schema clone code");
    ASSERT(schema_out == sentinel_schema,
           "NULL dataset schema clone leaves output");

    schema_out = sentinel_schema;
    ASSERT(lance_fragment_schema_clone(NULL, &schema_out) == -1,
           "NULL fragment schema clone");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL fragment schema clone code");
    ASSERT(schema_out == sentinel_schema,
           "NULL fragment schema clone leaves output");

    LanceSchema *empty = NULL;
    ASSERT(lance_schema_project(NULL, NULL, 0, &empty) == -1,
           "NULL schema project");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL schema project code");
    ASSERT(empty == NULL, "NULL schema project leaves output");

    LanceDataset *guard_dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(guard_dataset != NULL, "open dataset for schema guards");
    CHECK_OK();
    schema = clone_dataset_schema(guard_dataset);
    lance_dataset_close(guard_dataset);
    LanceSchema *projected_sentinel = (LanceSchema *)(uintptr_t)0x2;
    LanceSchema *projected_out = projected_sentinel;
    const char *unknown_columns[] = {"does_not_exist"};
    ASSERT(lance_schema_project(schema, unknown_columns, 1,
                                &projected_out) == -1,
           "unknown project column");
    ASSERT(lance_last_error_code() == LANCE_ERR_INTERNAL,
           "unknown project column code");
    ASSERT(projected_out == projected_sentinel,
           "unknown project column leaves output");

    projected_out = projected_sentinel;
    ASSERT(lance_schema_project(schema, NULL, 1, &projected_out) == -1,
           "NULL project column array");
    ASSERT(projected_out == projected_sentinel,
           "NULL project column array leaves output");
    ASSERT(lance_schema_project(schema, NULL, SIZE_MAX, &projected_out) == -1,
           "oversized project column count");
    ASSERT(projected_out == projected_sentinel,
           "oversized project column count leaves output");

    LanceOperation *unsupported = lance_operation_delete_new(NULL, 0, NULL, 0,
                                                             "id = 1");
    ASSERT(unsupported != NULL, "construct Delete for schema guard");
    CHECK_OK();
    schema_out = sentinel_schema;
    ASSERT(lance_operation_schema_clone(unsupported, &schema_out) == -1,
           "Delete operation schema unsupported");
    ASSERT(lance_last_error_code() == LANCE_ERR_NOT_SUPPORTED,
           "Delete operation schema error code");
    ASSERT(schema_out == sentinel_schema,
           "Delete operation schema leaves output");
    bool preserves_sentinel = true;
    ASSERT(lance_operation_preserves_nullability(unsupported,
                                                 &preserves_sentinel) == -1,
           "Delete preserves_nullability unsupported");
    ASSERT(preserves_sentinel, "Delete preserves_nullability leaves output");
    lance_operation_close(unsupported);

    LanceOperation *project_sentinel =
        (LanceOperation *)(uintptr_t)0x3;
    LanceOperation *project_out = project_sentinel;
    ASSERT(lance_operation_project_new(NULL, false, &project_out) == -1,
           "NULL schema Project constructor");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL schema Project constructor code");
    ASSERT(project_out == project_sentinel,
           "NULL schema Project constructor leaves output");

    int32_t field_sentinel = -11;
    ASSERT(lance_schema_field_id(schema, "nope", &field_sentinel) == -1,
           "unknown field ID");
    ASSERT(lance_last_error_code() == LANCE_ERR_INTERNAL,
           "unknown field ID code");
    ASSERT(field_sentinel == -11, "unknown field ID leaves output");

    /* Pinned native core accepts an empty Project schema; assert the observed
       behavior so a silent policy change is visible. */
    LanceSchema *empty_schema = NULL;
    ASSERT(lance_schema_project(schema, NULL, 0, &empty_schema) == 0,
           "empty native schema projection");
    CHECK_OK();
    lance_schema_close(schema);
    LanceOperation *empty_operation = NULL;
    ASSERT(lance_operation_project_new(empty_schema, false,
                                       &empty_operation) == 0,
           "empty schema Project operation");
    CHECK_OK();
    lance_schema_close(empty_schema);
    LanceDataset *empty_reopened = lance_dataset_open(uri, NULL, 0);
    ASSERT(empty_reopened != NULL, "open dataset before empty Project");
    CHECK_OK();
    uint64_t empty_version = lance_dataset_version(empty_reopened);
    lance_dataset_close(empty_reopened);
    LanceTransaction *empty_transaction =
        lance_transaction_new(empty_version, empty_operation, NULL);
    ASSERT(empty_transaction != NULL, "empty Project transaction");
    CHECK_OK();
    LanceDataset *empty_committed = NULL;
    int32_t empty_rc = lance_transaction_commit(
        uri, empty_transaction, NULL, NULL, NULL, &empty_committed);
    printf("empty-schema Project commit rc=%d code=%d\n", empty_rc,
           (int)lance_last_error_code());
    fflush(stdout);
    ASSERT(empty_rc == 0, "empty-schema Project commit");
    CHECK_OK();
    ASSERT(empty_committed != NULL, "empty-schema Project commit output");
    lance_dataset_close(empty_committed);
    lance_transaction_close(empty_transaction);
    lance_operation_close(empty_operation);
    printf("native schema and Project OK\n");
}

typedef struct PublicationRow {
    uint32_t id;
    uint64_t row_id;
} PublicationRow;

typedef struct PublicationBlob {
    const char *data;
    size_t len;
    int is_null;
} PublicationBlob;

static void release_publication_schema_leaf(struct ArrowSchema *schema) {
    if (schema) schema->release = NULL;
}

static void release_publication_array_leaf(struct ArrowArray *array) {
    if (array) array->release = NULL;
}

static void release_publication_ids(struct ArrowArray *array) {
    if (!array || !array->release) return;
    free((void *)array->buffers[1]);
    free((void *)array->buffers);
    array->buffers = NULL;
    array->release = NULL;
}

static void release_publication_schema(struct ArrowSchema *schema) {
    if (!schema || !schema->release) return;
    for (int64_t i = 0; i < schema->n_children; i++) {
        struct ArrowSchema *child = schema->children[i];
        if (child && child->release) child->release(child);
    }
    free(schema->children);
    schema->children = NULL;
    schema->n_children = 0;
    schema->release = NULL;
}

static void release_publication_array(struct ArrowArray *array) {
    if (!array || !array->release) return;
    for (int64_t i = 0; i < array->n_children; i++) {
        struct ArrowArray *child = array->children[i];
        if (child && child->release) child->release(child);
        free(child);
    }
    free(array->children);
    free((void *)array->buffers);
    array->children = NULL;
    array->buffers = NULL;
    array->n_children = 0;
    array->release = NULL;
}

static void init_publication_root_schema(
    struct ArrowSchema *root,
    struct ArrowSchema *id_schema,
    struct ArrowSchema *blob_schema) {
    struct ArrowSchema **children =
        (struct ArrowSchema **)calloc(2, sizeof(*children));
    ASSERT(children != NULL, "allocate replacement schema children");
    children[0] = id_schema;
    children[1] = blob_schema;
    memset(root, 0, sizeof(*root));
    root->format = "+s";
    root->name = "";
    root->n_children = 2;
    root->children = children;
    root->release = release_publication_schema;
}

static void write_publication_batch(
    LanceFileWriter *writer,
    const uint32_t *ids,
    int64_t row_count,
    struct ArrowArray *blob_array,
    const struct ArrowSchema *schema) {
    ASSERT(blob_array->length == row_count,
           "descriptor row count must match replacement rows");
    struct ArrowArray *id_array = calloc(1, sizeof(*id_array));
    const void **id_buffers = calloc(2, sizeof(*id_buffers));
    uint32_t *id_values = malloc((size_t)row_count * sizeof(*id_values));
    struct ArrowArray *owned_blob_array = malloc(sizeof(*owned_blob_array));
    ASSERT(id_array && id_buffers && id_values && owned_blob_array,
           "allocate owned replacement inputs");
    memcpy(id_values, ids, (size_t)row_count * sizeof(*id_values));
    id_buffers[1] = id_values;
    id_array->length = row_count;
    id_array->n_buffers = 2;
    id_array->buffers = id_buffers;
    id_array->release = release_publication_ids;
    *owned_blob_array = *blob_array;
    blob_array->release = NULL;

    struct ArrowArray **children =
        (struct ArrowArray **)calloc(2, sizeof(*children));
    ASSERT(children != NULL, "allocate replacement array children");
    children[0] = id_array;
    children[1] = owned_blob_array;
    const void **root_buffers = calloc(1, sizeof(*root_buffers));
    ASSERT(root_buffers != NULL, "allocate replacement root buffers");
    struct ArrowArray root;
    memset(&root, 0, sizeof(root));
    root.length = row_count;
    root.n_buffers = 1;
    root.buffers = root_buffers;
    root.n_children = 2;
    root.children = children;
    root.release = release_publication_array;

    LanceBatch *batch = NULL;
    ASSERT(lance_batch_from_arrow(&root, schema, &batch) == 0,
           "import replacement batch");
    CHECK_OK();
    ASSERT(lance_file_writer_write_batch(writer, batch) == 0,
           "write replacement batch");
    CHECK_OK();
    lance_batch_free(batch);
}

static LanceFileWriter *open_publication_writer(
    const LanceFileSession *session,
    const char *filename,
    const struct ArrowSchema *schema) {
    LanceFileWriterOptions options;
    memset(&options, 0, sizeof(options));
    options.version = "2.2";
    options.has_keep_original_array = 1;
    options.keep_original_array = 1;
    LanceFileWriter *writer =
        lance_file_session_open_writer(session, filename, schema, &options);
    ASSERT(writer != NULL, "open publication raw writer");
    CHECK_OK();
    return writer;
}

static void check_publication_blob(
    const LanceDataset *dataset,
    uint64_t index,
    const PublicationBlob *expected) {
    LanceBlobFile *blob = NULL;
    ASSERT(lance_dataset_take_blobs_by_indices(
               dataset, &index, 1, "blob", &blob) == 0,
           "take publication blob by index");
    CHECK_OK();
    if (expected->is_null) {
        ASSERT(blob == NULL, "expected a null blob handle");
        return;
    }
    ASSERT(blob != NULL, "expected a non-null blob handle");
    ASSERT(lance_blob_file_size(blob) == (uint64_t)expected->len,
           "publication blob size mismatch");
    if (expected->len > 0) {
        uint8_t buffer[128];
        ASSERT(expected->len <= sizeof(buffer), "publication payload too large");
        ASSERT(lance_blob_file_read(blob, buffer, expected->len) == 0,
               "read publication blob");
        CHECK_OK();
        ASSERT(memcmp(buffer, expected->data, expected->len) == 0,
               "publication blob payload mismatch");
    }
    lance_blob_file_close(blob);
}

static void check_publication_payloads(
    const char *uri,
    uint64_t version,
    const PublicationBlob *expected,
    size_t count) {
    LanceDataset *dataset = lance_dataset_open(uri, NULL, version);
    ASSERT(dataset != NULL, "open publication snapshot");
    CHECK_OK();
    for (size_t i = 0; i < count; i++) {
        check_publication_blob(dataset, (uint64_t)i, &expected[i]);
    }
    lance_dataset_close(dataset);
}

static int publication_child_index(
    const struct ArrowSchema *schema,
    const char *name) {
    for (int64_t i = 0; i < schema->n_children; i++) {
        if (schema->children[i]->name != NULL &&
            strcmp(schema->children[i]->name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static uint64_t collect_publication_rows(
    const char *uri,
    uint64_t version,
    PublicationRow *rows,
    uint64_t capacity) {
    LanceDataset *dataset = lance_dataset_open(uri, NULL, version);
    ASSERT(dataset != NULL, "open dataset for row IDs");
    CHECK_OK();
    LanceScanner *scanner = lance_scanner_new(dataset, NULL, NULL);
    ASSERT(scanner != NULL, "create row ID scanner");
    CHECK_OK();
    ASSERT(lance_scanner_with_row_id(scanner, true) == 0,
           "enable scan row IDs");
    CHECK_OK();
    struct ArrowArrayStream stream = {0};
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "export row ID stream");
    CHECK_OK();
    struct ArrowSchema schema = {0};
    ASSERT(stream.get_schema(&stream, &schema) == 0,
           "read row ID schema");
    CHECK_OK();
    int id_index = publication_child_index(&schema, "id");
    int row_id_index = publication_child_index(&schema, "_rowid");
    ASSERT(id_index >= 0 && row_id_index >= 0,
           "row ID schema must contain id and _rowid");
    uint64_t count = 0;
    while (1) {
        struct ArrowArray batch = {0};
        ASSERT(stream.get_next(&stream, &batch) == 0,
               "read row ID batch");
        if (!batch.release) break;
        ASSERT(batch.n_children > id_index &&
                   batch.n_children > row_id_index,
               "row ID batch child count");
        const struct ArrowArray *id_array = batch.children[id_index];
        const struct ArrowArray *row_id_array =
            batch.children[row_id_index];
        ASSERT(id_array->buffers[1] != NULL &&
                   row_id_array->buffers[1] != NULL,
               "row ID arrays must carry values");
        const uint32_t *ids =
            (const uint32_t *)id_array->buffers[1];
        const uint64_t *row_ids =
            (const uint64_t *)row_id_array->buffers[1];
        for (int64_t i = 0; i < batch.length; i++) {
            ASSERT(count < capacity, "row ID capacity");
            rows[count].id = ids[id_array->offset + i];
            rows[count].row_id = row_ids[row_id_array->offset + i];
            count++;
        }
        batch.release(&batch);
    }
    schema.release(&schema);
    if (stream.release) stream.release(&stream);
    lance_scanner_close(scanner);
    lance_dataset_close(dataset);
    for (uint64_t i = 1; i < count; i++) {
        PublicationRow key = rows[i];
        uint64_t j = i;
        while (j > 0 && rows[j - 1].id > key.id) {
            rows[j] = rows[j - 1];
            j--;
        }
        rows[j] = key;
    }
    return count;
}

static void check_row_ids_preserved(
    const PublicationRow *before,
    uint64_t before_count,
    const PublicationRow *after,
    uint64_t after_count) {
    for (uint64_t i = 0; i < after_count; i++) {
        int found = 0;
        for (uint64_t j = 0; j < before_count; j++) {
            if (before[j].id == after[i].id) {
                ASSERT(before[j].row_id == after[i].row_id,
                       "stable row ID changed across replacement");
                found = 1;
                break;
            }
        }
        ASSERT(found, "replacement row must have existed before");
    }
}

static void check_fragment_shape(
    const LanceDataset *dataset,
    uint64_t fragment_id,
    uint64_t physical_rows,
    uint64_t deleted_rows) {
    LanceFragment *fragment =
        lance_dataset_open_fragment(dataset, fragment_id);
    ASSERT(fragment != NULL, "open publication fragment");
    CHECK_OK();
    uint64_t actual_physical = UINT64_MAX;
    ASSERT(lance_fragment_physical_rows(fragment, &actual_physical) == 0 &&
               actual_physical == physical_rows,
           "publication fragment physical rows");
    CHECK_OK();
    uint64_t actual_deleted = UINT64_MAX;
    ASSERT(lance_fragment_deleted_rows(fragment, &actual_deleted) == 0 &&
               actual_deleted == deleted_rows,
           "publication fragment deleted rows");
    CHECK_OK();
    lance_fragment_close(fragment);
}

static void test_blob_writer_publication(const char *uri) {
    printf("  test_blob_writer_publication... ");
    const PublicationBlob pristine[] = {
        {"old-packed-0", 12, 0},
        {NULL, 0, 1},
        {"", 0, 0},
        {"old-dedicated-3", 15, 0},
        {"old-dedicated-4", 15, 0},
        {"keep-c5", 7, 0},
        {"keep-c6", 7, 0},
    };
    const PublicationBlob after_delete[] = {
        {"old-packed-0", 12, 0},
        {"", 0, 0},
        {"old-dedicated-3", 15, 0},
        {"old-dedicated-4", 15, 0},
        {"keep-c5", 7, 0},
        {"keep-c6", 7, 0},
    };
    const PublicationBlob replaced[] = {
        {"new-packed-0", 12, 0},
        {NULL, 0, 1},
        {"new-dedicated-3", 15, 0},
        {"", 0, 0},
        {"keep-c5", 7, 0},
        {"keep-c6", 7, 0},
    };

    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open publication dataset");
    CHECK_OK();
    ASSERT(lance_dataset_version(dataset) == 3,
           "publication fixture version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(dataset) == 7,
           "publication fixture rows");
    CHECK_OK();
    ASSERT(lance_dataset_fragment_count(dataset) == 3,
           "publication fixture fragments");
    CHECK_OK();

    uint64_t fragments[3] = {0};
    ASSERT(lance_dataset_fragment_ids(dataset, fragments) == 0,
           "publication fragment IDs");
    CHECK_OK();

    PublicationRow before[7];
    ASSERT(collect_publication_rows(uri, 3, before, 7) == 7,
           "publication row IDs before deletion");
    check_publication_payloads(uri, 3, pristine, 7);

    uint64_t deleted = 0;
    ASSERT(lance_dataset_delete(dataset, "id = 1", &deleted) == 0 &&
               deleted == 1,
           "delete publication row");
    CHECK_OK();
    ASSERT(lance_dataset_version(dataset) == 4,
           "publication deletion version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(dataset) == 6,
           "publication deletion rows");
    CHECK_OK();
    check_publication_payloads(uri, 4, after_delete, 6);

    char data_uri[4096];
    snprintf(data_uri, sizeof(data_uri), "file://%s/data", uri);
    LanceFileSession *session =
        lance_file_session_new(data_uri, NULL, NULL, NULL);
    ASSERT(session != NULL, "open publication session");
    CHECK_OK();

    const char *packed_name = "c11-packed-publication.lance";
    const char *dedicated_name = "c11-dedicated-publication.lance";
    LancePackedBlobWriter *packed =
        lance_file_session_open_packed_blob_writer(session, packed_name, 8101);
    ASSERT(packed != NULL, "open packed publication writer");
    CHECK_OK();
    ASSERT(lance_packed_blob_writer_blob_id(packed) == 8101,
           "packed publication blob ID");
    CHECK_OK();
    static const uint8_t packed_validity[] = {0x03};
    static const int32_t packed_offsets[] = {0, 12, 12, 12};
    static const char packed_payloads[] = "new-packed-0";
    static const void *packed_buffers[3];
    packed_buffers[0] = packed_validity;
    packed_buffers[1] = packed_offsets;
    packed_buffers[2] = packed_payloads;
    struct ArrowArray packed_payload_array;
    memset(&packed_payload_array, 0, sizeof(packed_payload_array));
    packed_payload_array.length = 3;
    packed_payload_array.null_count = 1;
    packed_payload_array.n_buffers = 3;
    packed_payload_array.buffers = packed_buffers;
    packed_payload_array.release = release_publication_array_leaf;
    struct ArrowSchema packed_payload_schema;
    memset(&packed_payload_schema, 0, sizeof(packed_payload_schema));
    packed_payload_schema.format = "z";
    packed_payload_schema.name = "payload";
    packed_payload_schema.flags = 2;
    packed_payload_schema.release = release_publication_schema_leaf;
    ASSERT(lance_packed_blob_writer_write_blobs(
               packed, &packed_payload_array, &packed_payload_schema) == 0,
           "write packed publication blobs");
    CHECK_OK();
    ASSERT(packed_payload_array.release == NULL,
           "packed publication payload array consumed");
    packed_payload_schema.release(&packed_payload_schema);
    struct ArrowArray packed_array = {0};
    struct ArrowSchema packed_schema = {0};
    ASSERT(lance_packed_blob_writer_finish_array(
               packed, "blob", &packed_array, &packed_schema) == 0,
           "finish packed publication writer");
    CHECK_OK();
    ASSERT(packed_array.length == 3,
           "packed publication descriptor rows");
    ASSERT(packed_schema.release != NULL,
           "packed publication schema ownership");
    lance_packed_blob_writer_close(packed);

    LanceDedicatedBlobWriter *dedicated_a =
        lance_file_session_open_dedicated_blob_writer(
            session, dedicated_name, 8102);
    ASSERT(dedicated_a != NULL, "open first dedicated publication writer");
    CHECK_OK();
    ASSERT(lance_dedicated_blob_writer_write(
               dedicated_a, (const uint8_t *)"new-dedicated-3", 15) == 0,
           "write first dedicated publication blob");
    CHECK_OK();
    struct ArrowArray dedicated_a_array = {0};
    struct ArrowSchema dedicated_a_schema = {0};
    ASSERT(lance_dedicated_blob_writer_finish_array(
               dedicated_a, "blob", &dedicated_a_array,
               &dedicated_a_schema) == 0,
           "finish first dedicated publication writer");
    CHECK_OK();
    ASSERT(dedicated_a_array.length == 1,
           "first dedicated publication descriptor rows");
    lance_dedicated_blob_writer_close(dedicated_a);

    LanceDedicatedBlobWriter *dedicated_b =
        lance_file_session_open_dedicated_blob_writer(
            session, dedicated_name, 8103);
    ASSERT(dedicated_b != NULL, "open second dedicated publication writer");
    CHECK_OK();
    ASSERT(lance_dedicated_blob_writer_write(
               dedicated_b, (const uint8_t *)"", 0) == 0,
           "write second dedicated publication blob");
    CHECK_OK();
    struct ArrowArray dedicated_b_array = {0};
    struct ArrowSchema dedicated_b_schema = {0};
    ASSERT(lance_dedicated_blob_writer_finish_array(
               dedicated_b, "blob", &dedicated_b_array,
               &dedicated_b_schema) == 0,
           "finish second dedicated publication writer");
    CHECK_OK();
    ASSERT(dedicated_b_array.length == 1,
           "second dedicated publication descriptor rows");
    lance_dedicated_blob_writer_close(dedicated_b);

    struct ArrowSchema id_schema;
    memset(&id_schema, 0, sizeof(id_schema));
    id_schema.format = "I";
    id_schema.name = "id";
    id_schema.release = release_publication_schema_leaf;
    struct ArrowSchema root_schema;
    init_publication_root_schema(&root_schema, &id_schema, &packed_schema);

    LanceFileWriter *packed_writer =
        open_publication_writer(session, packed_name, &root_schema);
    const uint32_t packed_ids[3] = {0, 1, 2};
    write_publication_batch(
        packed_writer, packed_ids, 3, &packed_array, &root_schema);
    LanceFileWriteSummary packed_summary = {0};
    ASSERT(lance_file_writer_finish(packed_writer, &packed_summary) == 0,
           "finish packed publication data file");
    CHECK_OK();
    ASSERT(packed_summary.num_rows == 3,
           "packed publication data file rows");
    ASSERT(packed_summary.size_bytes > 0,
           "packed publication data file size");
    lance_file_writer_close(packed_writer);

    LanceFileWriter *dedicated_writer =
        open_publication_writer(session, dedicated_name, &root_schema);
    const uint32_t dedicated_id_a = 3;
    write_publication_batch(
        dedicated_writer, &dedicated_id_a, 1, &dedicated_a_array,
        &root_schema);
    const uint32_t dedicated_id_b = 4;
    write_publication_batch(
        dedicated_writer, &dedicated_id_b, 1, &dedicated_b_array,
        &root_schema);
    LanceFileWriteSummary dedicated_summary = {0};
    ASSERT(lance_file_writer_finish(dedicated_writer, &dedicated_summary) == 0,
           "finish dedicated publication data file");
    CHECK_OK();
    ASSERT(dedicated_summary.num_rows == 2,
           "dedicated publication data file rows");
    ASSERT(dedicated_summary.size_bytes > 0,
           "dedicated publication data file size");
    lance_file_writer_close(dedicated_writer);
    lance_file_session_close(session);
    root_schema.release(&root_schema);
    if (dedicated_a_schema.release) dedicated_a_schema.release(&dedicated_a_schema);
    if (dedicated_b_schema.release) dedicated_b_schema.release(&dedicated_b_schema);

    LanceDataFile *packed_file = NULL;
    ASSERT(lance_dataset_create_data_file(
               dataset, packed_name, NULL, &packed_file) == 0,
           "create packed publication data file");
    CHECK_OK();
    ASSERT(packed_file != NULL, "packed publication data file output");
    ASSERT(strcmp(lance_data_file_path(packed_file), packed_name) == 0,
           "packed publication data file path");
    CHECK_OK();
    uint32_t major = UINT32_MAX;
    uint32_t minor = UINT32_MAX;
    ASSERT(lance_data_file_file_version(packed_file, &major, &minor) == 0 &&
               major == 2 && minor == 2,
           "packed publication data file version");
    CHECK_OK();
    bool present = true;
    uint64_t file_size = 0;
    ASSERT(lance_data_file_file_size(packed_file, &present, &file_size) == 0 &&
               present && file_size > 0,
           "packed publication data file size metadata");
    CHECK_OK();
    present = true;
    uint32_t base_id = UINT32_MAX;
    ASSERT(lance_data_file_base_id(packed_file, &present, &base_id) == 0 &&
               !present,
           "packed publication data file base ID");
    CHECK_OK();

    LanceDataFile *dedicated_file = NULL;
    ASSERT(lance_dataset_create_data_file(
               dataset, dedicated_name, NULL, &dedicated_file) == 0,
           "create dedicated publication data file");
    CHECK_OK();
    ASSERT(dedicated_file != NULL, "dedicated publication data file output");
    ASSERT(strcmp(lance_data_file_path(dedicated_file), dedicated_name) == 0,
           "dedicated publication data file path");
    CHECK_OK();
    ASSERT(lance_data_file_file_version(dedicated_file, &major, &minor) == 0 &&
               major == 2 && minor == 2,
           "dedicated publication data file version");
    CHECK_OK();

    LanceDataReplacementGroup *packed_group =
        lance_data_replacement_group_new(fragments[0], packed_file);
    ASSERT(packed_group != NULL, "construct packed replacement group");
    CHECK_OK();
    LanceDataReplacementGroup *dedicated_group =
        lance_data_replacement_group_new(fragments[1], dedicated_file);
    ASSERT(dedicated_group != NULL, "construct dedicated replacement group");
    CHECK_OK();
    lance_data_file_close(packed_file);
    lance_data_file_close(dedicated_file);

    const LanceDataReplacementGroup *groups[] = {
        packed_group, dedicated_group};
    LanceOperation *operation =
        lance_operation_data_replacement_new(groups, 2);
    ASSERT(operation != NULL, "construct publication operation");
    CHECK_OK();
    uint64_t replacement_count = UINT64_MAX;
    ASSERT(lance_operation_data_replacement_count(
               operation, &replacement_count) == 0 &&
               replacement_count == 2,
           "publication operation replacement count");
    CHECK_OK();
    LanceTransaction *transaction =
        lance_transaction_new(lance_dataset_version(dataset), operation, NULL);
    ASSERT(transaction != NULL, "construct publication transaction");
    CHECK_OK();
    lance_operation_close(operation);
    lance_data_replacement_group_close(packed_group);
    lance_data_replacement_group_close(dedicated_group);

    LanceCommitOptions commit_options;
    memset(&commit_options, 0, sizeof(commit_options));
    commit_options.has_max_retries = true;
    commit_options.max_retries = 0;
    commit_options.timeout_mode = LANCE_COMMIT_TIMEOUT_DISABLED;
    LanceDataset *committed =
        commit_transaction(uri, transaction, &commit_options);
    lance_transaction_close(transaction);
    ASSERT(lance_dataset_version(committed) == 5,
           "publication commit version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(committed) == 6,
           "publication commit rows");
    CHECK_OK();
    ASSERT(lance_dataset_fragment_count(committed) == 3,
           "publication commit fragments");
    CHECK_OK();

    LanceTransaction *history = NULL;
    ASSERT(lance_dataset_read_transaction(committed, &history) == 0,
           "read publication history");
    CHECK_OK();
    LanceOperation *history_operation = NULL;
    ASSERT(lance_transaction_operation(history, &history_operation) == 0,
           "clone publication history operation");
    CHECK_OK();
    ASSERT(lance_operation_data_replacement_count(
               history_operation, &replacement_count) == 0 &&
               replacement_count == 2,
           "publication history replacement count");
    CHECK_OK();
    lance_operation_close(history_operation);
    lance_transaction_close(history);

    uint64_t committed_fragments[3] = {0};
    ASSERT(lance_dataset_fragment_ids(committed, committed_fragments) == 0,
           "publication committed fragment IDs");
    CHECK_OK();
    for (size_t i = 0; i < 3; i++) {
        ASSERT(committed_fragments[i] == fragments[i],
               "replacement must preserve fragment IDs");
    }
    check_fragment_shape(committed, fragments[0], 3, 1);
    check_fragment_shape(committed, fragments[1], 2, 0);
    check_fragment_shape(committed, fragments[2], 2, 0);

    PublicationRow after[6];
    ASSERT(collect_publication_rows(uri, 5, after, 6) == 6,
           "publication row IDs after commit");
    check_row_ids_preserved(before, 7, after, 6);
    check_publication_payloads(uri, 5, replaced, 6);
    check_publication_payloads(uri, 4, after_delete, 6);
    check_publication_payloads(uri, 3, pristine, 7);

    lance_dataset_close(committed);
    lance_dataset_close(dataset);
    printf("OK\n");
    printf("C blob writer publication OK\n");
}

static void test_native_operation_construction(const char *src_uri,
                                               const char *base) {
    char uri[4096];
    snprintf(uri, sizeof(uri), "%s/c_native_operations", base);

    LanceDataset *source = lance_dataset_open(src_uri, NULL, 0);
    ASSERT(source != NULL, "open native operation source");
    CHECK_OK();
    uint64_t source_rows = lance_dataset_count_rows(source);
    CHECK_OK();
    lance_dataset_close(source);

    LanceUncommittedWriteOptions options;
    memset(&options, 0, sizeof(options));
    options.write.params.max_rows_per_file = 1;

    LanceTransaction *create = write_uncommitted_from_source(
        src_uri, uri, LANCE_WRITE_CREATE, &options);
    LanceDataset *dataset = commit_transaction(uri, create, NULL);
    uint64_t version = lance_dataset_version(dataset);
    CHECK_OK();
    ASSERT(version == 1, "native operation seed version");
    uint64_t seed_fragments = lance_dataset_fragment_count(dataset);
    CHECK_OK();
    ASSERT(seed_fragments > 1, "seed must span fragments");
    lance_transaction_close(create);
    lance_dataset_close(dataset);

    LanceTransaction *staged = write_uncommitted_from_source(
        src_uri, uri, LANCE_WRITE_APPEND, &options);
    uint64_t read_version = transaction_read_version(staged);
    ASSERT(read_version == 1, "staged append read version");
    ASSERT(transaction_operation_kind(staged) == LANCE_OPERATION_APPEND,
           "staged append operation kind");
    uint64_t staged_count = transaction_fragment_count(staged);

    LanceOperation *staged_operation = NULL;
    ASSERT(lance_transaction_operation(staged, &staged_operation) == 0,
           "clone staged append operation");
    CHECK_OK();
    uint64_t append_count = UINT64_MAX;
    ASSERT(lance_operation_append_fragment_count(staged_operation,
                                                 &append_count) == 0,
           "append fragment count");
    CHECK_OK();
    ASSERT(append_count == staged_count && append_count > 0,
           "generic and typed append counts agree");

    LanceFragment **handles =
        (LanceFragment **)calloc(append_count, sizeof(LanceFragment *));
    ASSERT(handles != NULL, "allocate detached fragment handles");
    for (uint64_t i = 0; i < append_count; i++) {
        LanceFragment *fragment = NULL;
        ASSERT(lance_operation_append_fragment_at(staged_operation, i,
                                                  &fragment) == 0,
               "typed append fragment");
        CHECK_OK();
        ASSERT(fragment != NULL, "typed append fragment output");
        bool is_bound = true;
        ASSERT(lance_fragment_is_bound(fragment, &is_bound) == 0,
               "append fragment bound state");
        CHECK_OK();
        ASSERT(!is_bound, "append fragments must be detached");
        ASSERT(lance_fragment_data_file_count(fragment) > 0,
               "detached fragment data files");
        CHECK_OK();
        LanceDataFile *file = NULL;
        ASSERT(lance_fragment_data_file_at(fragment, 0, &file) == 0,
               "detached fragment data file");
        CHECK_OK();
        ASSERT(file != NULL, "detached fragment data file output");
        const char *path = lance_data_file_path(file);
        ASSERT(path != NULL, "detached data file path");
        CHECK_OK();
        lance_data_file_close(file);
        LanceDataFile *field_file = NULL;
        ASSERT(lance_fragment_data_file_for_field(fragment, 0, &field_file) == 0,
               "detached field data file");
        CHECK_OK();
        if (field_file != NULL) lance_data_file_close(field_file);
        const char *metadata = lance_fragment_metadata_json(fragment);
        ASSERT(metadata != NULL && metadata[0] != '\0',
               "detached fragment metadata JSON");
        CHECK_OK();
        lance_free_string(metadata);

        uint64_t rows = 0xBEEF;
        ASSERT(lance_fragment_physical_rows(fragment, &rows) == -1,
               "detached physical rows rejected");
        ASSERT(lance_last_error_code() == LANCE_ERR_NOT_SUPPORTED,
               "detached physical rows error code");
        ASSERT(rows == 0xBEEF, "detached physical rows output untouched");
        ASSERT(lance_fragment_scan(fragment) == NULL,
               "detached scan rejected");
        ASSERT(lance_last_error_code() == LANCE_ERR_NOT_SUPPORTED,
               "detached scan error code");
        handles[i] = fragment;
    }

    lance_operation_close(staged_operation);
    lance_transaction_close(staged);

    LanceOperation *typed_append = lance_operation_append_new(
        (const LanceFragment *const *)handles, append_count);
    ASSERT(typed_append != NULL, "construct typed append operation");
    CHECK_OK();
    for (uint64_t i = 0; i < append_count; i++) {
        const char *metadata = lance_fragment_metadata_json(handles[i]);
        ASSERT(metadata != NULL,
               "detached metadata outlives staged transaction");
        CHECK_OK();
        lance_free_string(metadata);
        lance_fragment_close(handles[i]);
    }
    free(handles);

    LanceTransaction *append_transaction =
        lance_transaction_new(read_version, typed_append, NULL);
    ASSERT(append_transaction != NULL, "typed append transaction");
    CHECK_OK();
    dataset = commit_transaction(uri, append_transaction, NULL);
    ASSERT(lance_dataset_version(dataset) == 2, "typed append version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(dataset) == source_rows * 2,
           "typed append rows");
    CHECK_OK();
    uint64_t max_after_append = dataset_max_fragment_id(dataset);
    lance_transaction_close(append_transaction);
    lance_operation_close(typed_append);
    lance_dataset_close(dataset);

    LanceOperation *restore = lance_operation_restore_new(1);
    ASSERT(restore != NULL, "construct restore operation");
    CHECK_OK();
    uint64_t restore_version = UINT64_MAX;
    ASSERT(lance_operation_restore_version(restore, &restore_version) == 0,
           "restore version getter");
    CHECK_OK();
    ASSERT(restore_version == 1, "restore version value");
    LanceTransaction *restore_transaction =
        lance_transaction_new(2, restore, NULL);
    ASSERT(restore_transaction != NULL, "restore transaction");
    dataset = commit_transaction(uri, restore_transaction, NULL);
    ASSERT(lance_dataset_version(dataset) == 3, "restore version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(dataset) == source_rows,
           "restore restores historical rows");
    CHECK_OK();
    lance_transaction_close(restore_transaction);
    lance_operation_close(restore);
    lance_dataset_close(dataset);

    LanceOperation *reserve_zero = lance_operation_reserve_fragments_new(0);
    ASSERT(reserve_zero != NULL, "construct zero reserve");
    CHECK_OK();
    uint32_t reserve_count = UINT32_MAX;
    ASSERT(lance_operation_reserve_fragments_count(reserve_zero,
                                                   &reserve_count) == 0,
           "zero reserve getter");
    CHECK_OK();
    ASSERT(reserve_count == 0, "zero reserve value");
    LanceTransaction *zero_transaction =
        lance_transaction_new(3, reserve_zero, NULL);
    ASSERT(zero_transaction != NULL, "zero reserve transaction");
    dataset = commit_transaction(uri, zero_transaction, NULL);
    ASSERT(lance_dataset_version(dataset) == 4, "zero reserve version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(dataset) == source_rows,
           "zero reserve preserves rows");
    CHECK_OK();
    ASSERT(lance_dataset_fragment_count(dataset) == seed_fragments,
           "zero reserve preserves fragments");
    CHECK_OK();
    lance_transaction_close(zero_transaction);
    lance_operation_close(reserve_zero);
    lance_dataset_close(dataset);

    LanceOperation *reserve_two = lance_operation_reserve_fragments_new(2);
    ASSERT(reserve_two != NULL, "construct reserve two");
    CHECK_OK();
    LanceTransaction *reserve_transaction =
        lance_transaction_new(4, reserve_two, NULL);
    ASSERT(reserve_transaction != NULL, "reserve two transaction");
    dataset = commit_transaction(uri, reserve_transaction, NULL);
    ASSERT(lance_dataset_version(dataset) == 5, "reserve two version");
    CHECK_OK();
    lance_transaction_close(reserve_transaction);
    lance_operation_close(reserve_two);
    lance_dataset_close(dataset);

    LanceTransaction *reserved_append = write_uncommitted_from_source(
        src_uri, uri, LANCE_WRITE_APPEND, &options);
    uint64_t reserved_read_version = transaction_read_version(reserved_append);
    ASSERT(reserved_read_version == 5, "reserved append read version");
    LanceOperation *reserved_operation = NULL;
    ASSERT(lance_transaction_operation(reserved_append, &reserved_operation) == 0,
           "reserved append operation");
    CHECK_OK();
    uint64_t reserved_fragments = UINT64_MAX;
    ASSERT(lance_operation_append_fragment_count(reserved_operation,
                                                 &reserved_fragments) == 0,
           "reserved append fragment count");
    CHECK_OK();
    LanceFragment **reserved_handles = (LanceFragment **)calloc(
        reserved_fragments, sizeof(LanceFragment *));
    ASSERT(reserved_handles != NULL, "allocate reserved handles");
    for (uint64_t i = 0; i < reserved_fragments; i++) {
        ASSERT(lance_operation_append_fragment_at(reserved_operation, i,
                                                  &reserved_handles[i]) == 0,
               "reserved append fragment");
        CHECK_OK();
    }
    LanceOperation *reserved_typed = lance_operation_append_new(
        (const LanceFragment *const *)reserved_handles, reserved_fragments);
    ASSERT(reserved_typed != NULL, "reserved typed append");
    CHECK_OK();
    for (uint64_t i = 0; i < reserved_fragments; i++) {
        lance_fragment_close(reserved_handles[i]);
    }
    free(reserved_handles);
    lance_operation_close(reserved_operation);
    lance_transaction_close(reserved_append);
    LanceTransaction *reserved_transaction =
        lance_transaction_new(reserved_read_version, reserved_typed, NULL);
    ASSERT(reserved_transaction != NULL, "reserved append transaction");
    dataset = commit_transaction(uri, reserved_transaction, NULL);
    ASSERT(lance_dataset_version(dataset) == 6, "reserved append version");
    CHECK_OK();
    lance_transaction_close(reserved_transaction);
    lance_operation_close(reserved_typed);
    ASSERT(lance_dataset_fragment_count(dataset) ==
               seed_fragments + reserved_fragments,
           "reserved append fragment count");
    CHECK_OK();
    uint64_t reserved_ids[64] = {0};
    ASSERT(lance_dataset_fragment_ids(dataset, reserved_ids) == 0,
           "reserved append fragment ids");
    CHECK_OK();
    uint64_t max_after = 0;
    for (uint64_t i = 0; i < seed_fragments + reserved_fragments; i++) {
        if (reserved_ids[i] > max_after) max_after = reserved_ids[i];
    }
    ASSERT(max_after == max_after_append + 2 + reserved_fragments,
           "reserve advances native fragment id allocation");
    CHECK_OK();
    lance_dataset_close(dataset);

    char base_a[4096];
    char base_b[4096];
    snprintf(base_a, sizeof(base_a), "%s_c_base_a", uri);
    snprintf(base_b, sizeof(base_b), "%s_c_base_b", uri);
    LanceBasePath bases[2];
    memset(bases, 0, sizeof(bases));
    bases[0].id = 7;
    bases[0].path = base_a;
    bases[0].name = NULL;
    bases[1].id = 0;
    bases[1].path = base_b;
    bases[1].name = "";
    bases[1].is_dataset_root = true;
    LanceOperation *update_bases =
        lance_operation_update_bases_new(bases, 2);
    ASSERT(update_bases != NULL, "construct update bases");
    CHECK_OK();
    uint64_t base_count = UINT64_MAX;
    ASSERT(lance_operation_update_bases_count(update_bases, &base_count) == 0,
           "update bases count");
    CHECK_OK();
    ASSERT(base_count == 2, "update bases count value");
    LanceBasePathView view;
    memset(&view, 0, sizeof(view));
    ASSERT(lance_operation_update_bases_at(update_bases, 0, &view) == 0,
           "update bases view");
    CHECK_OK();
    ASSERT(view.id == 7, "update bases view id");
    ASSERT(view.name == NULL, "update bases NULL name stays NULL");
    ASSERT(strcmp(view.path, base_a) == 0, "update bases view path");
    memset(&view, 0, sizeof(view));
    ASSERT(lance_operation_update_bases_at(update_bases, 1, &view) == 0,
           "update bases empty name view");
    CHECK_OK();
    ASSERT(view.name != NULL && strcmp(view.name, "") == 0,
           "update bases empty name stays empty");
    ASSERT(view.is_dataset_root, "update bases root flag");

    LanceTransaction *bases_transaction =
        lance_transaction_new(6, update_bases, NULL);
    ASSERT(bases_transaction != NULL, "update bases transaction");
    dataset = commit_transaction(uri, bases_transaction, NULL);
    ASSERT(lance_dataset_version(dataset) == 7, "update bases version");
    CHECK_OK();
    const char *bases_json = lance_dataset_base_paths_json(dataset);
    ASSERT(bases_json != NULL, "base paths JSON");
    CHECK_OK();
    ASSERT(strstr(bases_json, base_a) != NULL &&
               strstr(bases_json, base_b) != NULL,
           "update bases publishes both paths");
    lance_free_string(bases_json);
    lance_transaction_close(bases_transaction);
    lance_operation_close(update_bases);
    lance_dataset_close(dataset);

    LanceBasePath conflict;
    memset(&conflict, 0, sizeof(conflict));
    conflict.path = base_a;
    LanceOperation *conflict_operation =
        lance_operation_update_bases_new(&conflict, 1);
    ASSERT(conflict_operation != NULL, "construct conflicting update bases");
    LanceTransaction *conflict_transaction =
        lance_transaction_new(7, conflict_operation, NULL);
    ASSERT(conflict_transaction != NULL, "conflicting update bases transaction");
    LanceDataset *failed = NULL;
    ASSERT(lance_transaction_commit(uri, conflict_transaction, NULL, NULL, NULL,
                                    &failed) == -1,
           "conflicting base path commit rejected");
    ASSERT(failed == NULL, "conflicting commit has no dataset");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "conflicting base path error code");
    const char *conflict_message = lance_last_error_message();
    ASSERT(conflict_message != NULL &&
               strstr(conflict_message, "Conflict detected") != NULL,
           "conflicting base path native message");
    lance_free_string(conflict_message);
    lance_transaction_close(conflict_transaction);
    lance_operation_close(conflict_operation);

    LanceOperation *empty_bases =
        lance_operation_update_bases_new(NULL, 0);
    ASSERT(empty_bases != NULL, "construct empty update bases");
    CHECK_OK();
    lance_operation_close(empty_bases);

    LanceOperation *null_append = lance_operation_append_new(NULL, 1);
    ASSERT(null_append == NULL, "NULL append fragment array rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL append fragment array error code");
    LanceOperation *empty_append = lance_operation_append_new(NULL, 0);
    ASSERT(empty_append != NULL, "empty append operation");
    CHECK_OK();
    lance_operation_close(empty_append);
    LanceOperation *null_bases = lance_operation_update_bases_new(NULL, 1);
    ASSERT(null_bases == NULL, "NULL update bases array rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL update bases array error code");

    printf("native operation construction OK\n");
}


static bool arrow_metadata_matches(const char *metadata, const char *key,
                                  const char *expected) {
    if (!metadata) return expected == NULL;
    size_t key_len = strlen(key);
    const char *cursor = metadata;
    int32_t pair_count = 0;
    memcpy(&pair_count, cursor, sizeof(pair_count));
    cursor += sizeof(pair_count);
    for (int32_t i = 0; i < pair_count; ++i) {
        int32_t entry_key_len = 0;
        memcpy(&entry_key_len, cursor, sizeof(entry_key_len));
        cursor += sizeof(entry_key_len);
        const char *entry_key = cursor;
        cursor += entry_key_len;
        int32_t value_len = 0;
        memcpy(&value_len, cursor, sizeof(value_len));
        cursor += sizeof(value_len);
        const char *entry_value = cursor;
        cursor += value_len;
        if (entry_key_len == (int32_t)key_len &&
            memcmp(entry_key, key, key_len) == 0) {
            if (expected == NULL) return false;
            size_t expected_len = strlen(expected);
            return value_len == (int32_t)expected_len &&
                   memcmp(entry_value, expected, expected_len) == 0;
        }
    }
    return expected == NULL;
}

static void check_dataset_schema_metadata(LanceDataset *dataset,
                                          const char *key,
                                          const char *expected) {
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(lance_dataset_schema(dataset, &schema) == 0, "export dataset schema");
    CHECK_OK();
    if (expected == NULL) {
        ASSERT(arrow_metadata_matches(schema.metadata, key, NULL),
               "schema metadata key must be absent");
    } else {
        ASSERT(arrow_metadata_matches(schema.metadata, key, expected),
               "schema metadata value");
    }
    if (schema.release) schema.release(&schema);
}

static void check_field_schema_metadata(LanceDataset *dataset,
                                        uint64_t field_index,
                                        const char *key,
                                        const char *expected) {
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(lance_dataset_schema(dataset, &schema) == 0, "export dataset schema");
    CHECK_OK();
    ASSERT(field_index < (uint64_t)schema.n_children, "schema field index");
    if (expected == NULL) {
        ASSERT(arrow_metadata_matches(schema.children[field_index]->metadata,
                                      key, NULL),
               "field metadata key must be absent");
    } else {
        ASSERT(arrow_metadata_matches(schema.children[field_index]->metadata,
                                      key, expected),
               "field metadata value");
    }
    if (schema.release) schema.release(&schema);
}

static const char *config_value_for_key(LanceDatasetConfig *config,
                                        const char *key) {
    uint64_t count = lance_dataset_config_count(config);
    for (uint64_t i = 0; i < count; i++) {
        const char *candidate = lance_dataset_config_key_at(config, i);
        if (candidate != NULL && strcmp(candidate, key) == 0) {
            return lance_dataset_config_value_at(config, i);
        }
    }
    return NULL;
}

static void test_native_typed_operations(const char *src_uri,
                                         const char *base) {
    char seed_uri[4096];
    char overwrite_uri[4096];
    char clone_uri[4096];
    char deep_uri[4096];
    char rejection_uri[4096];
    snprintf(seed_uri, sizeof(seed_uri), "%s/c_typed_seed", base);
    snprintf(overwrite_uri, sizeof(overwrite_uri), "%s/c_typed_overwrite", base);
    snprintf(clone_uri, sizeof(clone_uri), "%s/c_typed_clone", base);
    snprintf(deep_uri, sizeof(deep_uri), "%s/c_typed_clone_deep", base);
    snprintf(rejection_uri, sizeof(rejection_uri), "%s/c_typed_rejection", base);

    LanceDataset *source = lance_dataset_open(src_uri, NULL, 0);
    ASSERT(source != NULL, "open typed operation source");
    CHECK_OK();
    uint64_t source_rows = lance_dataset_count_rows(source);
    CHECK_OK();
    uint64_t source_version = lance_dataset_version(source);
    CHECK_OK();
    lance_dataset_close(source);

    LanceUncommittedWriteOptions write_options;
    memset(&write_options, 0, sizeof(write_options));
    write_options.write.params.max_rows_per_file = 2;
    LanceTransaction *seed_transaction = write_uncommitted_from_source(
        src_uri, seed_uri, LANCE_WRITE_CREATE, &write_options);
    LanceDataset *seed = commit_transaction(seed_uri, seed_transaction, NULL);
    ASSERT(lance_dataset_version(seed) == 1, "typed seed version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(seed) == source_rows, "typed seed rows");
    CHECK_OK();
    uint64_t seed_fragments = lance_dataset_fragment_count(seed);
    CHECK_OK();
    ASSERT(seed_fragments >= 2, "typed seed must span multiple fragments");
    uint64_t *seed_ids =
        (uint64_t *)calloc(seed_fragments, sizeof(uint64_t));
    ASSERT(seed_ids != NULL, "allocate typed seed fragment ids");
    ASSERT(lance_dataset_fragment_ids(seed, seed_ids) == 0,
           "typed seed fragment ids");
    CHECK_OK();
    LanceFragment **seed_handles =
        (LanceFragment **)calloc(seed_fragments, sizeof(LanceFragment *));
    ASSERT(seed_handles != NULL, "allocate typed seed fragment handles");
    for (uint64_t i = 0; i < seed_fragments; i++) {
        seed_handles[i] = lance_dataset_open_fragment(seed, seed_ids[i]);
        ASSERT(seed_handles[i] != NULL, "open typed seed fragment");
        CHECK_OK();
    }
    LanceSchema *schema = NULL;
    ASSERT(lance_dataset_schema_clone(seed, &schema) == 0,
           "clone typed seed schema");
    CHECK_OK();
    ASSERT(schema != NULL, "typed seed schema output");
    int32_t seed_id_field = -1;
    int32_t seed_name_field = -1;
    ASSERT(lance_schema_field_id(schema, "id", &seed_id_field) == 0,
           "typed seed id field id");
    CHECK_OK();
    ASSERT(lance_schema_field_id(schema, "name", &seed_name_field) == 0,
           "typed seed name field id");
    CHECK_OK();
    ASSERT(seed_id_field >= 0 && seed_name_field >= 0,
           "typed seed field ids");

    const char *iterator_key_storage[] = {"owner", "empty"};
    const char *iterator_value_storage[] = {"alice", ""};
    LanceStringMap config = {iterator_key_storage, iterator_value_storage, 2};
    char base_path[4096];
    snprintf(base_path, sizeof(base_path), "%s/c_typed_base", base);
    LanceBasePath base_entries[] = {
        {9001, base_path, "primary", false},
    };
    LanceBasePathList base_list = {base_entries, 1};
    LanceOperation *overwrite = lance_operation_overwrite_new(
        schema, (const LanceFragment *const *)seed_handles, seed_fragments,
        &config, &base_list);
    ASSERT(overwrite != NULL, "construct native Overwrite");
    CHECK_OK();
    LanceOperationKind overwrite_kind = LANCE_OPERATION_UNKNOWN;
    ASSERT(lance_operation_kind(overwrite, &overwrite_kind) == 0 &&
               overwrite_kind == LANCE_OPERATION_OVERWRITE,
           "Overwrite operation kind");
    CHECK_OK();
    uint64_t overwrite_fragments = UINT64_MAX;
    ASSERT(lance_operation_overwrite_fragment_count(overwrite,
                                                    &overwrite_fragments) == 0,
           "Overwrite typed fragment count");
    CHECK_OK();
    ASSERT(overwrite_fragments == seed_fragments,
           "Overwrite typed fragment count value");
    LanceFragment *detached = NULL;
    ASSERT(lance_operation_overwrite_fragment_at(overwrite, 1, &detached) == 0,
           "Overwrite typed fragment clone");
    CHECK_OK();
    ASSERT(detached != NULL, "Overwrite typed fragment output");
    const char *detached_json = lance_fragment_metadata_json(detached);
    ASSERT(detached_json != NULL && detached_json[0] != '\0',
           "Overwrite detached fragment metadata");
    lance_free_string(detached_json);
    lance_fragment_close(detached);
    bool has_config = false;
    ASSERT(lance_operation_overwrite_has_config_upsert_values(overwrite,
                                                              &has_config) == 0,
           "Overwrite config presence");
    CHECK_OK();
    ASSERT(has_config, "Overwrite config must be Some");
    uint64_t config_count = UINT64_MAX;
    ASSERT(lance_operation_overwrite_config_upsert_count(overwrite,
                                                         &config_count) == 0,
           "Overwrite config count");
    CHECK_OK();
    ASSERT(config_count == 2, "Overwrite config count value");
    const char *config_owner = NULL;
    for (uint64_t i = 0; i < config_count; i++) {
        const char *key = lance_operation_overwrite_config_upsert_key_at(
            overwrite, i);
        ASSERT(key != NULL, "Overwrite config key");
        CHECK_OK();
        if (strcmp(key, "owner") == 0) {
            config_owner = lance_operation_overwrite_config_upsert_value_at(
                overwrite, i);
        }
    }
    ASSERT(config_owner != NULL && strcmp(config_owner, "alice") == 0,
           "Overwrite config owner value");
    bool has_bases = false;
    ASSERT(lance_operation_overwrite_has_initial_bases(overwrite,
                                                       &has_bases) == 0,
           "Overwrite bases presence");
    CHECK_OK();
    ASSERT(has_bases, "Overwrite bases must be Some");
    uint64_t base_count = UINT64_MAX;
    ASSERT(lance_operation_overwrite_initial_base_count(overwrite,
                                                        &base_count) == 0,
           "Overwrite base count");
    CHECK_OK();
    ASSERT(base_count == 1, "Overwrite base count value");
    LanceBasePathView base_view;
    memset(&base_view, 0, sizeof(base_view));
    ASSERT(lance_operation_overwrite_initial_base_at(overwrite, 0,
                                                     &base_view) == 0,
           "Overwrite base view");
    CHECK_OK();
    ASSERT(base_view.id == 9001, "Overwrite base id");
    ASSERT(base_view.name != NULL && strcmp(base_view.name, "primary") == 0,
           "Overwrite base name");
    ASSERT(!base_view.is_dataset_root, "Overwrite base root flag");

    uint64_t wrong_kind_count = UINT64_MAX;
    ASSERT(lance_operation_merge_fragment_count(overwrite,
                                                &wrong_kind_count) == -1,
           "wrong-kind typed getter fails");
    ASSERT(lance_last_error_code() == LANCE_ERR_NOT_SUPPORTED,
           "wrong-kind typed getter code");
    ASSERT(wrong_kind_count == UINT64_MAX,
           "wrong-kind typed getter leaves output unchanged");
    ASSERT(lance_operation_overwrite_fragment_count(overwrite, NULL) == -1,
           "NULL typed out parameter fails");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL typed out parameter code");

    LanceTransaction *overwrite_transaction =
        lance_transaction_new(0, overwrite, NULL);
    ASSERT(overwrite_transaction != NULL, "typed Overwrite transaction");
    CHECK_OK();
    LanceDataset *published =
        commit_transaction(overwrite_uri, overwrite_transaction, NULL);
    ASSERT(lance_dataset_version(published) == 1,
           "typed Overwrite published version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(published) == source_rows,
           "typed Overwrite published rows");
    CHECK_OK();
    ASSERT(lance_dataset_fragment_count(published) == seed_fragments,
           "typed Overwrite published fragments");
    CHECK_OK();
    uint64_t *published_ids =
        (uint64_t *)calloc(seed_fragments, sizeof(uint64_t));
    ASSERT(published_ids != NULL, "allocate published fragment ids");
    ASSERT(lance_dataset_fragment_ids(published, published_ids) == 0,
           "typed Overwrite published ids");
    CHECK_OK();
    for (uint64_t i = 0; i < seed_fragments; i++) {
        ASSERT(published_ids[i] == i,
               "typed Overwrite assigns fresh fragment ids");
    }
    free(published_ids);
    LanceDatasetConfig *published_config = lance_dataset_config(published);
    ASSERT(published_config != NULL, "typed Overwrite config snapshot");
    CHECK_OK();
    const char *config_owner_value = config_value_for_key(published_config,
                                                          "owner");
    ASSERT(config_owner_value != NULL &&
               strcmp(config_owner_value, "alice") == 0,
           "typed Overwrite committed config");
    const char *config_empty_value = config_value_for_key(published_config,
                                                          "empty");
    ASSERT(config_empty_value != NULL && strcmp(config_empty_value, "") == 0,
           "typed Overwrite committed empty config value");
    lance_dataset_config_close(published_config);
    const char *bases_json = lance_dataset_base_paths_json(published);
    ASSERT(bases_json != NULL, "typed Overwrite base paths json");
    CHECK_OK();
    ASSERT(strstr(bases_json, "\"9001\"") != NULL,
           "typed Overwrite base path id");
    ASSERT(strstr(bases_json, "primary") != NULL,
           "typed Overwrite base path name");
    lance_free_string(bases_json);
    LanceSchema *published_schema = NULL;
    ASSERT(lance_dataset_schema_clone(published, &published_schema) == 0,
           "typed Overwrite schema clone");
    CHECK_OK();
    int32_t published_id_field = -1;
    ASSERT(lance_schema_field_id(published_schema, "id", &published_id_field) ==
               0,
           "typed Overwrite schema field id");
    CHECK_OK();
    ASSERT(published_id_field == seed_id_field,
           "typed Overwrite keeps native field ids");
    lance_schema_close(published_schema);

    LanceOperation *merge = lance_operation_merge_new(
        schema, (const LanceFragment *const *)seed_handles, seed_fragments,
        false);
    ASSERT(merge != NULL, "construct native Merge");
    CHECK_OK();
    LanceOperationKind merge_kind = LANCE_OPERATION_UNKNOWN;
    ASSERT(lance_operation_kind(merge, &merge_kind) == 0 &&
               merge_kind == LANCE_OPERATION_MERGE,
           "Merge operation kind");
    CHECK_OK();
    uint64_t merge_fragments = UINT64_MAX;
    ASSERT(lance_operation_merge_fragment_count(merge, &merge_fragments) == 0,
           "Merge typed fragment count");
    CHECK_OK();
    ASSERT(merge_fragments == seed_fragments,
           "Merge typed fragment count value");
    bool preserves = true;
    ASSERT(lance_operation_merge_preserves_nullability(merge, &preserves) == 0,
           "Merge preserves nullability");
    CHECK_OK();
    ASSERT(!preserves, "Merge nullability assertion");
    LanceTransaction *merge_transaction = lance_transaction_new(1, merge, NULL);
    ASSERT(merge_transaction != NULL, "typed Merge transaction");
    CHECK_OK();
    LanceDataset *merged =
        commit_transaction(seed_uri, merge_transaction, NULL);
    ASSERT(lance_dataset_version(merged) == 2, "typed Merge version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(merged) == source_rows,
           "typed Merge rows");
    CHECK_OK();
    ASSERT(lance_dataset_fragment_count(merged) == seed_fragments,
           "typed Merge fragments");
    lance_dataset_close(merged);

    const char *ref_path = src_uri;
    LanceOperation *shallow =
        lance_operation_clone_new(true, NULL, source_version, ref_path, NULL);
    ASSERT(shallow != NULL, "construct native shallow Clone");
    CHECK_OK();
    LanceOperationKind shallow_kind = LANCE_OPERATION_UNKNOWN;
    ASSERT(lance_operation_kind(shallow, &shallow_kind) == 0 &&
               shallow_kind == LANCE_OPERATION_CLONE,
           "Clone operation kind");
    CHECK_OK();
    bool is_shallow = false;
    ASSERT(lance_operation_clone_is_shallow(shallow, &is_shallow) == 0,
           "Clone shallow getter");
    CHECK_OK();
    ASSERT(is_shallow, "Clone shallow flag");
    bool ref_present = true;
    const char *ref_value = NULL;
    ASSERT(lance_operation_clone_ref_name(shallow, &ref_present, &ref_value) ==
               0,
           "Clone ref name getter");
    CHECK_OK();
    ASSERT(!ref_present && ref_value == NULL, "Clone absent ref name");
    uint64_t ref_version = 0;
    ASSERT(lance_operation_clone_ref_version(shallow, &ref_version) == 0,
           "Clone ref version getter");
    CHECK_OK();
    ASSERT(ref_version == source_version, "Clone ref version value");
    const char *clone_path = lance_operation_clone_ref_path(shallow);
    ASSERT(clone_path != NULL && strcmp(clone_path, ref_path) == 0,
           "Clone ref path getter");
    LanceTransaction *clone_transaction =
        lance_transaction_new(source_version, shallow, NULL);
    ASSERT(clone_transaction != NULL, "typed Clone transaction");
    CHECK_OK();
    LanceDataset *cloned =
        commit_transaction(clone_uri, clone_transaction, NULL);
    ASSERT(lance_dataset_version(cloned) == source_version,
           "typed Clone keeps the cloned source version");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(cloned) == source_rows,
           "typed Clone rows");
    CHECK_OK();
    lance_dataset_close(cloned);

    LanceOperation *deep =
        lance_operation_clone_new(false, NULL, source_version, ref_path, NULL);
    ASSERT(deep != NULL, "construct native deep Clone");
    CHECK_OK();
    LanceTransaction *deep_transaction =
        lance_transaction_new(source_version, deep, NULL);
    ASSERT(deep_transaction != NULL, "typed deep Clone transaction");
    CHECK_OK();
    LanceDataset *deep_dataset = NULL;
    ASSERT(lance_transaction_commit(deep_uri, deep_transaction, NULL, NULL, NULL,
                                    &deep_dataset) == -1,
           "deep Clone must be rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "deep Clone error code");
    const char *deep_message = lance_last_error_message();
    ASSERT(deep_message != NULL &&
               strstr(deep_message, "deep Clone cannot be committed directly") !=
                   NULL,
           "deep Clone native rejection message");
    lance_free_string(deep_message);
    ASSERT(deep_dataset == NULL, "deep Clone has no dataset output");

    const char *replace_keys[] = {"owner", "empty", "added"};
    const char *replace_values[] = {NULL, NULL, "yes"};
    LanceUpdateMap *config_updates =
        lance_update_map_new(replace_keys, replace_values, 3, true);
    ASSERT(config_updates != NULL, "construct UpdateConfig config map");
    CHECK_OK();
    const char *table_keys[] = {"table-key"};
    const char *table_values[] = {"table-value"};
    LanceUpdateMap *table_updates =
        lance_update_map_new(table_keys, table_values, 1, false);
    ASSERT(table_updates != NULL, "construct UpdateConfig table map");
    CHECK_OK();
    const char *schema_keys[] = {"schema-key"};
    const char *schema_values[] = {""};
    LanceUpdateMap *schema_updates =
        lance_update_map_new(schema_keys, schema_values, 1, false);
    ASSERT(schema_updates != NULL, "construct UpdateConfig schema map");
    CHECK_OK();
    const char *field_keys[] = {"field-key", "field-old"};
    const char *field_values[] = {"field-value", NULL};
    LanceUpdateMap *field_updates =
        lance_update_map_new(field_keys, field_values, 2, true);
    ASSERT(field_updates != NULL, "construct UpdateConfig field map");
    CHECK_OK();
    LanceFieldMetadataUpdateMap field_entries[] = {
        {seed_id_field, field_updates},
    };
    LanceUpdateConfigOperationOptions update_options;
    memset(&update_options, 0, sizeof(update_options));
    update_options.config_updates = config_updates;
    update_options.table_metadata_updates = table_updates;
    update_options.schema_metadata_updates = schema_updates;
    update_options.field_metadata_updates = field_entries;
    update_options.field_metadata_update_count = 1;
    LanceOperation *update_config =
        lance_operation_update_config_new(&update_options);
    ASSERT(update_config != NULL, "construct native UpdateConfig");
    CHECK_OK();
    LanceOperationKind update_config_kind = LANCE_OPERATION_UNKNOWN;
    ASSERT(lance_operation_kind(update_config, &update_config_kind) == 0 &&
               update_config_kind == LANCE_OPERATION_UPDATE_CONFIG,
           "UpdateConfig operation kind");
    CHECK_OK();
    LanceUpdateMap *typed_config_updates = NULL;
    ASSERT(lance_operation_update_config_config_updates(update_config,
                                                        &typed_config_updates) == 0,
           "UpdateConfig typed config map");
    CHECK_OK();
    ASSERT(typed_config_updates != NULL, "UpdateConfig typed config output");
    ASSERT(lance_update_map_count(typed_config_updates) == 3,
           "UpdateConfig typed config entry count");
    CHECK_OK();
    ASSERT(lance_update_map_replace(typed_config_updates),
           "UpdateConfig typed replace flag");
    CHECK_OK();
    bool has_value = false;
    const char *map_value = NULL;
    ASSERT(lance_update_map_value_at(typed_config_updates, 0, &has_value,
                                     &map_value) == 0,
           "UpdateConfig typed first entry value");
    CHECK_OK();
    ASSERT(!has_value && map_value == NULL,
           "UpdateConfig typed None entry");
    uint64_t field_update_count = UINT64_MAX;
    ASSERT(lance_operation_update_config_field_metadata_count(
               update_config, &field_update_count) == 0,
           "UpdateConfig typed field count");
    CHECK_OK();
    ASSERT(field_update_count == 1, "UpdateConfig typed field count value");
    int32_t update_field_id = -1;
    LanceUpdateMap *typed_field_updates = NULL;
    ASSERT(lance_operation_update_config_field_metadata_at(
               update_config, 0, &update_field_id, &typed_field_updates) == 0,
           "UpdateConfig typed field map");
    CHECK_OK();
    ASSERT(update_field_id == seed_id_field, "UpdateConfig field id");
    ASSERT(lance_update_map_count(typed_field_updates) == 2,
           "UpdateConfig field map entries");
    lance_update_map_close(typed_field_updates);
    lance_update_map_close(typed_config_updates);

    // UpdateConfig must apply to a dataset whose data files are reachable;
    // the overwrite publication reuses source fragments for manifest-level
    // assertions only, so commit the metadata change against the seed.
    LanceTransaction *update_transaction =
        lance_transaction_new(1, update_config, NULL);
    ASSERT(update_transaction != NULL, "typed UpdateConfig transaction");
    CHECK_OK();
    LanceDataset *updated =
        commit_transaction(seed_uri, update_transaction, NULL);
    ASSERT(lance_dataset_version(updated) == 2,
           "typed UpdateConfig version");
    CHECK_OK();
    LanceDatasetConfig *updated_config = lance_dataset_config(updated);
    ASSERT(updated_config != NULL, "typed UpdateConfig config snapshot");
    CHECK_OK();
    ASSERT(config_value_for_key(updated_config, "owner") == NULL,
           "UpdateConfig replace clears existing key");
    ASSERT(config_value_for_key(updated_config, "empty") == NULL,
           "UpdateConfig replace clears empty value key");
    const char *added_value = config_value_for_key(updated_config, "added");
    ASSERT(added_value != NULL && strcmp(added_value, "yes") == 0,
           "UpdateConfig replace inserts Some entries");
    lance_dataset_config_close(updated_config);
    const char *table_json = lance_dataset_metadata_json(updated);
    ASSERT(table_json != NULL, "UpdateConfig table metadata json");
    CHECK_OK();
    ASSERT(strstr(table_json, "table-key") != NULL &&
               strstr(table_json, "table-value") != NULL,
           "UpdateConfig table metadata effect");
    lance_free_string(table_json);
    check_dataset_schema_metadata(updated, "schema-key", "");
    check_field_schema_metadata(updated, 0, "field-key", "field-value");
    check_field_schema_metadata(updated, 0, "field-old", NULL);

    // The overwrite publication reuses source fragments, so deletion must run
    // against the source dataset, whose data files are reachable.
    LanceDataset *deletable = lance_dataset_open(src_uri, NULL, 0);
    ASSERT(deletable != NULL, "open typed deletion dataset");
    CHECK_OK();
    uint64_t deleted = 0;
    ASSERT(lance_dataset_delete(deletable, "id = 1", &deleted) == 0,
           "typed deletion seed");
    CHECK_OK();
    ASSERT(deleted == 1, "typed deletion count");
    uint64_t updated_fragment_count = lance_dataset_fragment_count(deletable);
    CHECK_OK();
    uint64_t *updated_ids =
        (uint64_t *)calloc(updated_fragment_count, sizeof(uint64_t));
    ASSERT(updated_ids != NULL, "allocate typed deletion fragment ids");
    ASSERT(lance_dataset_fragment_ids(deletable, updated_ids) == 0,
           "typed deletion fragment ids");
    CHECK_OK();
    LanceFragment *deleted_fragment = NULL;
    for (uint64_t i = 0; i < updated_fragment_count; i++) {
        LanceFragment *candidate =
            lance_dataset_open_fragment(deletable, updated_ids[i]);
        ASSERT(candidate != NULL, "open typed deletion fragment");
        CHECK_OK();
        const char *candidate_json = lance_fragment_metadata_json(candidate);
        ASSERT(candidate_json != NULL, "typed deletion fragment json");
        bool carries_deletion =
            strstr(candidate_json, "\"deletion_file\"") != NULL;
        lance_free_string(candidate_json);
        if (carries_deletion) {
            deleted_fragment = candidate;
            break;
        }
        lance_fragment_close(candidate);
    }
    ASSERT(deleted_fragment != NULL, "typed deletion fragment found");
    LanceSchema *updated_schema = NULL;
    ASSERT(lance_dataset_schema_clone(deletable, &updated_schema) == 0,
           "typed deletion schema clone");
    CHECK_OK();
    const LanceFragment *deletion_fragments[] = {deleted_fragment};
    LanceOperation *deletion_overwrite = lance_operation_overwrite_new(
        updated_schema, deletion_fragments, 1, NULL, NULL);
    ASSERT(deletion_overwrite != NULL, "construct deletion Overwrite");
    CHECK_OK();
    LanceTransaction *deletion_transaction =
        lance_transaction_new(0, deletion_overwrite, NULL);
    ASSERT(deletion_transaction != NULL, "deletion Overwrite transaction");
    CHECK_OK();
    LanceDataset *rejection_dataset = NULL;
    ASSERT(lance_transaction_commit(rejection_uri, deletion_transaction, NULL,
                                    NULL, NULL, &rejection_dataset) == -1,
           "deletion Overwrite must be rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "deletion Overwrite error code");
    const char *deletion_message = lance_last_error_message();
    ASSERT(deletion_message != NULL &&
               strstr(deletion_message, "carries deletion file") != NULL,
           "deletion Overwrite native rejection message");
    lance_free_string(deletion_message);
    ASSERT(rejection_dataset == NULL, "deletion Overwrite has no dataset");
    lance_fragment_close(deleted_fragment);
    free(updated_ids);
    lance_schema_close(updated_schema);
    lance_operation_close(deletion_overwrite);
    lance_transaction_close(deletion_transaction);

    // Direct update-map representation keeps ordered duplicates and None.
    const char *direct_keys[] = {"dup", "deleted", "dup", "empty"};
    const char *direct_values[] = {"first", NULL, "second", ""};
    LanceUpdateMap *direct =
        lance_update_map_new(direct_keys, direct_values, 4, true);
    ASSERT(direct != NULL, "construct ordered UpdateMap");
    CHECK_OK();
    ASSERT(lance_update_map_count(direct) == 4, "ordered UpdateMap count");
    CHECK_OK();
    ASSERT(lance_update_map_replace(direct), "ordered UpdateMap replace");
    CHECK_OK();
    ASSERT(strcmp(lance_update_map_key_at(direct, 0), "dup") == 0,
           "ordered UpdateMap first key");
    ASSERT(strcmp(lance_update_map_key_at(direct, 2), "dup") == 0,
           "ordered UpdateMap duplicate key");
    ASSERT(lance_update_map_key_at(direct, 4) == NULL,
           "ordered UpdateMap out of range key");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "ordered UpdateMap out of range code");
    bool direct_has_value = true;
    const char *direct_value = NULL;
    ASSERT(lance_update_map_value_at(direct, 1, &direct_has_value,
                                     &direct_value) == 0,
           "ordered UpdateMap None value");
    CHECK_OK();
    ASSERT(!direct_has_value && direct_value == NULL,
           "ordered UpdateMap keeps None entry");
    LanceUpdateMap *direct_clone = lance_update_map_clone(direct);
    ASSERT(direct_clone != NULL, "clone ordered UpdateMap");
    CHECK_OK();
    lance_update_map_close(direct);
    ASSERT(lance_update_map_count(direct_clone) == 4,
           "cloned ordered UpdateMap keeps entries");
    CHECK_OK();
    ASSERT(strcmp(lance_update_map_key_at(direct_clone, 3), "empty") == 0,
           "cloned ordered UpdateMap keeps order");
    lance_update_map_close(direct_clone);
    const char invalid_key[] = {(char)0xFF, '\0'};
    const char *invalid_keys[] = {invalid_key};
    ASSERT(lance_update_map_new(invalid_keys, direct_values, 1, false) == NULL,
           "invalid UTF-8 update map key is rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "invalid UTF-8 update map code");

    lance_update_map_close(config_updates);
    lance_update_map_close(table_updates);
    lance_update_map_close(schema_updates);
    lance_update_map_close(field_updates);
    lance_dataset_close(published);
    lance_dataset_close(updated);
    lance_dataset_close(deletable);
    lance_operation_close(overwrite);
    lance_transaction_close(overwrite_transaction);
    lance_operation_close(merge);
    lance_transaction_close(merge_transaction);
    lance_operation_close(shallow);
    lance_transaction_close(clone_transaction);
    lance_operation_close(deep);
    lance_transaction_close(deep_transaction);
    lance_operation_close(update_config);
    lance_transaction_close(update_transaction);
    lance_schema_close(schema);
    for (uint64_t i = 0; i < seed_fragments; i++) {
        lance_fragment_close(seed_handles[i]);
    }
    free(seed_handles);
    free(seed_ids);
    lance_dataset_close(seed);
    lance_transaction_close(seed_transaction);
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr,
                "usage: %s <source-dataset-uri> <base-path> "
                "<native-schema-uri> <blob-publication-uri>\n",
                argv[0]);
        return 2;
    }
    test_create_append_overwrite(argv[1], argv[2]);
    test_validation(argv[2]);
    test_fragment_delete(argv[1], argv[2]);
    test_data_replacement(argv[1], argv[2]);

    test_native_schema(argv[3]);
    test_blob_writer_publication(argv[4]);
    test_native_operation_construction(argv[1], argv[2]);
    test_native_typed_operations(argv[1], argv[2]);
    printf("All C transaction tests passed!\n");
    return 0;
}
