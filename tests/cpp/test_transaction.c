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

    ASSERT(lance_transaction_from_proto(NULL, 0) == NULL,
           "empty protobuf must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "empty protobuf error code");
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

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <source-dataset-uri> <base-path>\n",
                argv[0]);
        return 2;
    }
    test_create_append_overwrite(argv[1], argv[2]);
    test_validation(argv[2]);
    printf("All C transaction tests passed!\n");
    return 0;
}
