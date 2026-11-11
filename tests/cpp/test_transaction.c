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
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
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
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
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
    ASSERT(field_sentinel == -11, "unknown field ID leaves output");

    /* Experimental: native Project with an empty selection commits or rejects
       according to core validation; record the actual behavior. */
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
    /* Final assertion is tightened after the experimental observation. */
    (void)empty_committed;
    lance_transaction_close(empty_transaction);
    lance_operation_close(empty_operation);
    printf("native schema and Project OK\n");
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr,
                "usage: %s <source-dataset-uri> <base-path> "
                "<native-schema-uri>\n",
                argv[0]);
        return 2;
    }
    test_create_append_overwrite(argv[1], argv[2]);
    test_validation(argv[2]);
    test_fragment_delete(argv[1], argv[2]);
    test_native_schema(argv[3]);
    printf("All C transaction tests passed!\n");
    return 0;
}
