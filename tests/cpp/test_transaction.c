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

    dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "reopen before reserve");
    CHECK_OK();
    uint64_t ids_before[64] = {0};
    ASSERT(lance_dataset_fragment_ids(dataset, ids_before) == 0,
           "fragment ids before reserve");
    CHECK_OK();
    uint64_t max_before = 0;
    for (uint64_t i = 0; i < seed_fragments; i++) {
        if (ids_before[i] > max_before) max_before = ids_before[i];
    }
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
    ASSERT(max_after == max_before + 2 + reserved_fragments,
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

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <source-dataset-uri> <base-path>\n",
                argv[0]);
        return 2;
    }
    test_create_append_overwrite(argv[1], argv[2]);
    test_validation(argv[2]);
    test_fragment_delete(argv[1], argv[2]);
    test_data_replacement(argv[1], argv[2]);
    test_native_operation_construction(argv[1], argv[2]);
    printf("All C transaction tests passed!\n");
    return 0;
}
