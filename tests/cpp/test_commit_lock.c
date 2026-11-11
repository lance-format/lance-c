/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/*
 * C11 consumer for the native commit-lock bridge.
 *
 * argv[1] is an existing two-fragment dataset used as the Arrow stream source.
 * argv[2] is a writable scratch directory.
 */

#include "lance/lance.h"

#include <stdbool.h>
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
        if (lance_last_error_code() != LANCE_OK) {                             \
            const char *lance_message = lance_last_error_message();            \
            fprintf(stderr, "CHECK_OK failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    lance_message ? lance_message : "unknown error");          \
            if (lance_message) lance_free_string(lance_message);               \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

#define TOKEN_MAGIC 0x1ea5e123u

typedef struct LeaseToken {
    uint32_t magic;
} LeaseToken;

typedef struct LockState {
    char uri[2048];
    int acquire_calls;
    int acquire_success;
    int acquire_version_seen;
    int release_calls;
    int release_true;
    int release_false;
    int destroy_calls;
    int destroy_null;
    int destroy_nonnull;
    int context_release_calls;
    int callback_reads;
    int read_in_callbacks;
    int fail_conflict_once;
    int fail_io;
    int fail_partial_once;
    int fail_release_once;
    int bump_version_once;
    int bump_append_rows;
} LockState;

typedef struct StreamInput {
    struct ArrowArrayStream stream;
    struct ArrowSchema schema;
} StreamInput;

static const char *g_source_uri = NULL;

static StreamInput make_source_stream(void) {
    StreamInput input;
    memset(&input, 0, sizeof(input));

    LanceDataset *source = lance_dataset_open(g_source_uri, NULL, 0);
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

static uint64_t dataset_rows(LanceDataset *dataset) {
    LanceScanner *scanner = lance_scanner_new(dataset, NULL, NULL);
    ASSERT(scanner != NULL, "create scanner");
    CHECK_OK();
    uint64_t rows = 0;
    for (;;) {
        LanceBatch *batch = NULL;
        int32_t rc = lance_scanner_next(scanner, &batch);
        ASSERT(rc >= 0, "scanner next");
        CHECK_OK();
        if (rc == 1) break;
        ASSERT(batch != NULL, "batch output");
        struct ArrowArray array = {0};
        struct ArrowSchema schema = {0};
        ASSERT(lance_batch_to_arrow(batch, &array, &schema) == 0,
               "batch Arrow export");
        CHECK_OK();
        rows += (uint64_t)array.length;
        if (array.release) array.release(&array);
        if (schema.release) schema.release(&schema);
        lance_batch_free(batch);
    }
    lance_scanner_close(scanner);
    return rows;
}

static int64_t callback_snapshot_rows(const char *uri) {
    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    if (dataset == NULL) return -1;
    uint64_t rows = dataset_rows(dataset);
    lance_dataset_close(dataset);
    if (lance_last_error_code() != LANCE_OK) return -1;
    return (int64_t)rows;
}

static void free_owned_string(LanceOwnedString *string) {
    free(string->data);
    string->data = NULL;
    string->release = NULL;
}

static void fill_error(LanceCallbackError *error, int32_t code,
                       const char *message) {
    error->code = code;
    error->message.data = (char *)malloc(strlen(message) + 1);
    ASSERT(error->message.data != NULL, "allocate callback error message");
    strcpy(error->message.data, message);
    error->message.release = free_owned_string;
}

static void validate_token(void *lease) {
    if (lease == NULL) return;
    LeaseToken *token = (LeaseToken *)lease;
    if (token->magic != TOKEN_MAGIC) {
        fprintf(stderr, "commit lease token corruption\n");
        abort();
    }
}

static void bump_dataset_version(LockState *state) {
    StreamInput input = make_source_stream();
    LanceDataset *dataset = NULL;
    int32_t rc = lance_dataset_write(
        state->uri, &input.schema, &input.stream, LANCE_WRITE_APPEND, NULL,
        &dataset);
    if (input.schema.release) input.schema.release(&input.schema);
    ASSERT(rc == 0, "independent append inside acquire");
    CHECK_OK();
    ASSERT(dataset != NULL, "independent append output");
    state->bump_append_rows += 20;
    lance_dataset_close(dataset);
}

static int32_t lock_acquire(void *callback_ctx, uint64_t version,
                            void **out_lease, LanceCallbackError *error) {
    LockState *state = (LockState *)callback_ctx;
    ASSERT(out_lease != NULL, "acquire out_lease");
    *out_lease = NULL;
    state->acquire_calls++;
    if (version > 0) state->acquire_version_seen++;

    if (state->read_in_callbacks) {
        if (callback_snapshot_rows(state->uri) < 0) {
            fill_error(error, LANCE_ERR_IO, "callback snapshot read failed");
            return -1;
        }
        state->callback_reads++;
    }

    if (state->fail_conflict_once) {
        state->fail_conflict_once = 0;
        fill_error(error, LANCE_ERR_COMMIT_CONFLICT, "simulated lock conflict");
        return -1;
    }
    if (state->fail_io) {
        fill_error(error, LANCE_ERR_IO, "simulated io failure");
        return -1;
    }
    if (state->fail_partial_once) {
        state->fail_partial_once = 0;
        LeaseToken *token = (LeaseToken *)malloc(sizeof(LeaseToken));
        ASSERT(token != NULL, "allocate partial token");
        token->magic = TOKEN_MAGIC;
        *out_lease = token;
        fill_error(error, LANCE_ERR_INTERNAL, "simulated partial lease");
        return -1;
    }
    if (state->bump_version_once) {
        state->bump_version_once = 0;
        bump_dataset_version(state);
    }

    LeaseToken *token = (LeaseToken *)malloc(sizeof(LeaseToken));
    ASSERT(token != NULL, "allocate lease token");
    token->magic = TOKEN_MAGIC;
    *out_lease = token;
    state->acquire_success++;
    return 0;
}

static int32_t lock_release(void *callback_ctx, void *lease, bool success,
                            LanceCallbackError *error) {
    LockState *state = (LockState *)callback_ctx;
    validate_token(lease);
    state->release_calls++;
    if (success) {
        state->release_true++;
    } else {
        state->release_false++;
    }

    if (state->read_in_callbacks) {
        if (callback_snapshot_rows(state->uri) < 0) {
            fill_error(error, LANCE_ERR_IO, "release snapshot read failed");
            return -1;
        }
        state->callback_reads++;
    }

    if (state->fail_release_once) {
        state->fail_release_once = 0;
        fill_error(error, LANCE_ERR_INVALID_ARGUMENT,
                   "simulated release failure");
        return -1;
    }
    return 0;
}

static void lock_destroy(void *callback_ctx, void *lease) {
    LockState *state = (LockState *)callback_ctx;
    state->destroy_calls++;
    if (lease == NULL) {
        state->destroy_null++;
        return;
    }
    LeaseToken *token = (LeaseToken *)lease;
    if (token->magic != TOKEN_MAGIC) {
        fprintf(stderr, "destroyed commit lease token corruption\n");
        abort();
    }
    state->destroy_nonnull++;
    free(token);
}

static void lock_context_release(void *callback_ctx) {
    LockState *state = (LockState *)callback_ctx;
    state->context_release_calls++;
}

static LanceCommitLock *new_lock(LockState *state) {
    LanceCommitLockCallbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.lock = lock_acquire;
    callbacks.release = lock_release;
    callbacks.destroy_lease = lock_destroy;
    callbacks.callback_ctx = state;
    callbacks.release_context = lock_context_release;
    LanceCommitLock *lock = lance_commit_lock_new(&callbacks);
    ASSERT(lock != NULL, "construct commit lock");
    CHECK_OK();
    return lock;
}

static LanceDataset *lock_write(const char *uri, LanceCommitLock *lock,
                                LanceWriteMode mode,
                                const LanceWriteOptions *options) {
    StreamInput input = make_source_stream();
    LanceDataset *dataset = NULL;
    int32_t rc = lance_dataset_write_with_options_with_commit_lock(
        uri, &input.schema, &input.stream, (int32_t)mode, options, NULL, NULL,
        lock, &dataset);
    if (input.schema.release) input.schema.release(&input.schema);
    ASSERT(rc == 0, "lock-enabled write");
    CHECK_OK();
    return dataset;
}

static LanceTransaction *lock_write_uncommitted(const char *uri,
                                                LanceCommitLock *lock) {
    StreamInput input = make_source_stream();
    LanceTransaction *transaction = NULL;
    int32_t rc = lance_dataset_write_uncommitted_stream_with_commit_lock(
        uri, &input.schema, &input.stream, LANCE_WRITE_APPEND, NULL, NULL, NULL,
        lock, &transaction);
    if (input.schema.release) input.schema.release(&input.schema);
    ASSERT(rc == 0, "write uncommitted through lock");
    CHECK_OK();
    ASSERT(transaction != NULL, "uncommitted transaction output");
    return transaction;
}

static char *scratch_path(const char *base, const char *name) {
    size_t size = strlen(base) + strlen(name) + 2;
    char *path = (char *)malloc(size);
    ASSERT(path != NULL, "allocate scratch path");
    snprintf(path, size, "%s/%s", base, name);
    return path;
}

static void expect_release_context(LockState *state, int expected) {
    ASSERT(state->context_release_calls == expected,
           "commit lock context released exactly once");
}

static void test_constructor_contract(const char *base) {
    ASSERT(lance_commit_lock_new(NULL) == NULL,
           "null callback table rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "null callback table reports invalid argument");

    LockState state;
    memset(&state, 0, sizeof(state));
    LanceCommitLockCallbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.release = lock_release;
    callbacks.callback_ctx = &state;
    callbacks.release_context = lock_context_release;
    ASSERT(lance_commit_lock_new(&callbacks) == NULL,
           "missing acquire callback rejected");
    ASSERT(state.context_release_calls == 0,
           "failed construction never releases the context");

    char *uri = scratch_path(base, "stateless_lock");
    memset(&state, 0, sizeof(state));
    snprintf(state.uri, sizeof(state.uri), "%s", uri);
    LanceCommitLock *lock = new_lock(&state);
    LanceWriteOptions options;
    memset(&options, 0, sizeof(options));
    LanceDataset *dataset = lock_write(uri, lock, LANCE_WRITE_CREATE, &options);
    ASSERT(dataset != NULL, "stateless lock create");
    CHECK_OK();
    ASSERT(state.acquire_success == 1, "stateless acquire counted");
    ASSERT(state.release_true == 1, "stateless release success");
    ASSERT(state.destroy_calls == 1 && state.destroy_null == 1,
           "stateless NULL token destroyed exactly once");
    ASSERT(state.context_release_calls == 0,
           "dataset retains the lock context");
    lance_dataset_close(dataset);
    lance_commit_lock_close(lock);
    expect_release_context(&state, 1);
    ASSERT(state.destroy_nonnull == 0, "no non-null token in stateless test");

    LanceDataset *reopened = lance_dataset_open(uri, NULL, 0);
    ASSERT(reopened != NULL, "reopen stateless dataset");
    CHECK_OK();
    ASSERT(dataset_rows(reopened) == 20, "stateless dataset rows");
    lance_dataset_close(reopened);
    free(uri);
}

static void test_multi_fragment_and_deferred_context(const char *base) {
    char *uri = scratch_path(base, "multi_fragment");
    LockState state;
    memset(&state, 0, sizeof(state));
    snprintf(state.uri, sizeof(state.uri), "%s", uri);
    state.read_in_callbacks = 1;
    LanceCommitLock *lock = new_lock(&state);

    LanceWriteOptions options;
    memset(&options, 0, sizeof(options));
    options.params.max_rows_per_file = 3;
    LanceDataset *dataset = lock_write(uri, lock, LANCE_WRITE_CREATE, &options);
    ASSERT(dataset != NULL, "multi-fragment create through lock");
    CHECK_OK();
    uint64_t version = lance_dataset_version(dataset);
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(dataset) == 20,
           "multi-fragment row count");
    ASSERT(lance_dataset_fragment_count(dataset) == 7,
           "multi-fragment fragment count");
    ASSERT(state.acquire_version_seen == state.acquire_calls,
           "every acquire saw a native base version");
    ASSERT(state.callback_reads == 2, "acquire and release each read a snapshot");

    /* Closing the C handle drops only that owner; the dataset stays usable. */
    lance_commit_lock_close(lock);

    LanceScanner *scanner = lance_scanner_new(dataset, NULL, NULL);
    ASSERT(scanner != NULL, "scanner after lock close");
    CHECK_OK();
    uint64_t rows_before = dataset_rows(dataset);
    ASSERT(rows_before == 20, "scan after lock close");

    const char *key = "lock_mutation";
    const char *value = "after_close";
    char *json = (char *)lance_dataset_update_metadata_json(
        dataset, &key, &value, 1, LANCE_METADATA_UPDATE_MERGE);
    ASSERT(json != NULL, "metadata mutation through retained handler");
    CHECK_OK();
    lance_free_string(json);
    ASSERT(lance_dataset_version(dataset) == version + 1,
           "retained handler commits the mutation");
    ASSERT(state.acquire_calls >= 2, "retained handler still acquires");
    ASSERT(state.context_release_calls == 0,
           "context held by dataset and scanner owners");

    lance_scanner_close(scanner);
    lance_dataset_close(dataset);
    expect_release_context(&state, 1);

    LanceDataset *reopened = lance_dataset_open(uri, NULL, 0);
    ASSERT(reopened != NULL, "reopen multi-fragment dataset");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(reopened) == 20, "reopened rows");
    ASSERT(lance_dataset_fragment_count(reopened) == 7,
           "reopened fragment count");
    ASSERT(lance_dataset_version(reopened) == version + 1,
           "reopened version");
    lance_dataset_close(reopened);
    free(uri);
}

static void test_conflict_retry(const char *base) {
    char *uri = scratch_path(base, "conflict_retry");
    LockState state;
    memset(&state, 0, sizeof(state));
    snprintf(state.uri, sizeof(state.uri), "%s", uri);
    LanceCommitLock *lock = new_lock(&state);
    LanceWriteOptions options;
    memset(&options, 0, sizeof(options));

    LanceDataset *created = lock_write(uri, lock, LANCE_WRITE_CREATE, &options);
    ASSERT(created != NULL, "conflict fixture create");
    CHECK_OK();
    lance_dataset_close(created);

    int acquire_before = state.acquire_calls;
    int success_before = state.acquire_success;
    int destroy_before = state.destroy_calls;
    state.fail_conflict_once = 1;
    LanceCommitOptions commit_options;
    memset(&commit_options, 0, sizeof(commit_options));
    commit_options.has_max_retries = 1;
    commit_options.max_retries = 4;

    /* A lock-acquire conflict is retried by the native blanket handler. */
    StreamInput input = make_source_stream();
    LanceTransaction *transaction = NULL;
    int32_t rc = lance_dataset_write_uncommitted_stream_with_commit_lock(
        uri, &input.schema, &input.stream, LANCE_WRITE_APPEND, NULL, NULL, NULL,
        lock, &transaction);
    ASSERT(rc == 0, "stage append for conflict retry");
    CHECK_OK();
    LanceDataset *committed = NULL;
    rc = lance_transaction_commit_with_commit_lock(
        uri, transaction, &commit_options, NULL, NULL, lock, &committed);
    if (input.schema.release) input.schema.release(&input.schema);
    ASSERT(rc == 0, "conflict retry commit");
    CHECK_OK();
    ASSERT(committed != NULL, "conflict retry output");
    ASSERT(state.acquire_calls == acquire_before + 2,
           "conflict acquire retried exactly once");
    ASSERT(state.acquire_success == success_before + 1,
           "one successful lease after retry");
    ASSERT(state.release_true >= 1, "retried lease released successfully");
    ASSERT(state.destroy_calls == destroy_before + 1,
           "retry token destroyed exactly once");
    ASSERT(lance_dataset_count_rows(committed) == 40, "conflict retry rows");
    lance_dataset_close(committed);
    lance_transaction_close(transaction);

    /* Non-conflict callback errors surface without retrying. */
    LockState io_state;
    memset(&io_state, 0, sizeof(io_state));
    snprintf(io_state.uri, sizeof(io_state.uri), "%s", uri);
    LanceCommitLock *io_lock = new_lock(&io_state);
    io_state.fail_io = 1;
    transaction = lock_write_uncommitted(uri, io_lock);
    committed = NULL;
    rc = lance_transaction_commit_with_commit_lock(
        uri, transaction, &commit_options, NULL, NULL, io_lock, &committed);
    ASSERT(rc == -1, "io failure fails the commit");
    ASSERT(lance_last_error_code() == LANCE_ERR_IO, "io failure code");
    const char *message = lance_last_error_message();
    ASSERT(message != NULL && strstr(message, "simulated io failure") != NULL,
           "io failure message preserved");
    if (message) lance_free_string(message);
    ASSERT(committed == NULL, "failed commit leaves output untouched");
    ASSERT(io_state.release_calls == 0,
           "failed acquire has no lease to release");
    ASSERT(io_state.destroy_calls == 0,
           "failed acquire has no token to destroy");
    io_state.fail_io = 0;
    commit_options.max_retries = 0;
    committed = NULL;
    rc = lance_transaction_commit_with_commit_lock(
        uri, transaction, &commit_options, NULL, NULL, io_lock, &committed);
    ASSERT(rc == 0, "commit after clearing io failure");
    CHECK_OK();
    ASSERT(committed != NULL && lance_dataset_count_rows(committed) == 60,
           "io retry rows");
    lance_dataset_close(committed);
    lance_transaction_close(transaction);
    lance_commit_lock_close(io_lock);
    expect_release_context(&io_state, 1);

    lance_commit_lock_close(lock);
    expect_release_context(&state, 1);
    free(uri);
}

static void test_partial_and_release_paths(const char *base) {
    char *uri = scratch_path(base, "release_paths");
    LockState state;
    memset(&state, 0, sizeof(state));
    snprintf(state.uri, sizeof(state.uri), "%s", uri);
    LanceCommitLock *lock = new_lock(&state);
    LanceWriteOptions options;
    memset(&options, 0, sizeof(options));

    LanceDataset *created = lock_write(uri, lock, LANCE_WRITE_CREATE, &options);
    ASSERT(created != NULL, "release-path fixture create");
    CHECK_OK();
    lance_dataset_close(created);

    int destroy_before = state.destroy_calls;
    int nonnull_before = state.destroy_nonnull;
    int release_before = state.release_calls;
    state.fail_partial_once = 1;
    {
        LanceTransaction *transaction = lock_write_uncommitted(uri, lock);
        LanceDataset *committed = NULL;
        int32_t rc = lance_transaction_commit_with_commit_lock(
            uri, transaction, NULL, NULL, NULL, lock, &committed);
        ASSERT(rc == -1, "partial-token acquire fails the commit");
        ASSERT(lance_last_error_code() == LANCE_ERR_INTERNAL,
               "partial-token acquire code");
        ASSERT(committed == NULL, "partial-token output untouched");
        ASSERT(state.destroy_nonnull == nonnull_before + 1,
               "partial token destroyed exactly once");
        ASSERT(state.destroy_calls == destroy_before + 1,
               "partial token adds one destroy");
        ASSERT(state.release_calls == release_before,
               "partial token is never released");
        lance_transaction_close(transaction);
    }

    /* Retry succeeds and destroys the new token once. */
    {
        LanceTransaction *transaction = lock_write_uncommitted(uri, lock);
        LanceDataset *committed = NULL;
        int32_t rc = lance_transaction_commit_with_commit_lock(
            uri, transaction, NULL, NULL, NULL, lock, &committed);
        ASSERT(rc == 0, "commit after partial-token failure");
        CHECK_OK();
        ASSERT(committed != NULL, "post-partial output");
        ASSERT(state.destroy_nonnull == nonnull_before + 2,
               "successful token destroyed once");
        ASSERT(lance_dataset_count_rows(committed) == 40, "post-partial rows");
        lance_dataset_close(committed);
        lance_transaction_close(transaction);
    }

    /* Native conflict after acquire releases the lease with success=false. */
    state.bump_version_once = 1;
    state.bump_append_rows = 0;
    {
        LanceTransaction *transaction = lock_write_uncommitted(uri, lock);
        LanceDataset *committed = NULL;
        int32_t rc = lance_transaction_commit_with_commit_lock(
            uri, transaction, NULL, NULL, NULL, lock, &committed);
        ASSERT(rc == 0, "commit after injected head conflict");
        CHECK_OK();
        ASSERT(committed != NULL, "head-conflict output");
        ASSERT(state.release_false >= 1, "abandoned attempt released false");
        ASSERT(state.bump_append_rows == 20, "independent bump published");
        ASSERT(lance_dataset_count_rows(committed) == 80,
               "head-conflict rows after bump and retry");
        lance_dataset_close(committed);
        lance_transaction_close(transaction);
    }

    /* Release failure happens after publication: commit reports the error but
       the published manifest must be visible on reopen. */
    state.fail_release_once = 1;
    {
        LanceTransaction *transaction = lock_write_uncommitted(uri, lock);
        LanceDataset *committed = NULL;
        int32_t rc = lance_transaction_commit_with_commit_lock(
            uri, transaction, NULL, NULL, NULL, lock, &committed);
        ASSERT(rc == -1, "release failure fails the commit call");
        ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
               "release failure code");
        const char *message = lance_last_error_message();
        ASSERT(message != NULL &&
                   strstr(message, "simulated release failure") != NULL,
               "release failure message preserved");
        if (message) lance_free_string(message);
        ASSERT(committed == NULL, "release failure leaves output untouched");
        lance_transaction_close(transaction);
    }

    LanceDataset *reopened = lance_dataset_open(uri, NULL, 0);
    ASSERT(reopened != NULL, "reopen after release failure");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(reopened) == 100,
           "release failure still published the manifest");
    lance_dataset_close(reopened);

    lance_commit_lock_close(lock);
    expect_release_context(&state, 1);
    free(uri);
}

static void test_staged_commit_paths(const char *base) {
    char *uri = scratch_path(base, "staged_commit");
    LockState state;
    memset(&state, 0, sizeof(state));
    snprintf(state.uri, sizeof(state.uri), "%s", uri);
    LanceCommitLock *lock = new_lock(&state);
    LanceWriteOptions options;
    memset(&options, 0, sizeof(options));

    LanceDataset *created = lock_write(uri, lock, LANCE_WRITE_CREATE, &options);
    ASSERT(created != NULL, "staged fixture create");
    CHECK_OK();
    lance_dataset_close(created);

    LanceTransaction *single = lock_write_uncommitted(uri, lock);
    LanceDataset *committed = NULL;
    ASSERT(lance_transaction_commit_with_commit_lock(
               uri, single, NULL, NULL, NULL, lock, &committed) == 0,
           "single staged commit through lock");
    CHECK_OK();
    ASSERT(committed != NULL && lance_dataset_count_rows(committed) == 40,
           "single staged rows");
    uint64_t version = lance_dataset_version(committed);
    CHECK_OK();
    lance_dataset_close(committed);
    lance_transaction_close(single);

    LanceTransaction *first = lock_write_uncommitted(uri, lock);
    LanceTransaction *second = lock_write_uncommitted(uri, lock);
    const LanceTransaction *batch[2] = {first, second};
    LanceDataset *merged_dataset = NULL;
    LanceTransaction *merged = NULL;
    ASSERT(lance_transaction_commit_batch_with_commit_lock(
               uri, batch, 2, NULL, NULL, NULL, lock, &merged_dataset,
               &merged) == 0,
           "batch staged commit through lock");
    CHECK_OK();
    ASSERT(merged_dataset != NULL && merged != NULL, "batch outputs");
    ASSERT(lance_dataset_count_rows(merged_dataset) == 80, "batch rows");
    ASSERT(lance_dataset_version(merged_dataset) == version + 1,
           "batch commits one version");
    lance_dataset_close(merged_dataset);
    lance_transaction_close(merged);
    lance_transaction_close(first);
    lance_transaction_close(second);

    lance_commit_lock_close(lock);
    expect_release_context(&state, 1);

    LanceDataset *reopened = lance_dataset_open(uri, NULL, 0);
    ASSERT(reopened != NULL, "reopen staged dataset");
    CHECK_OK();
    ASSERT(lance_dataset_count_rows(reopened) == 80, "staged reopen rows");
    ASSERT(lance_dataset_version(reopened) == version + 1,
           "staged reopen version");
    lance_dataset_close(reopened);
    free(uri);
}

int main(int argc, char **argv) {
    ASSERT(argc == 3, "usage: test_commit_lock_c <source-uri> <scratch-dir>");
    g_source_uri = argv[1];
    const char *base = argv[2];

    test_constructor_contract(base);
    test_multi_fragment_and_deferred_context(base);
    test_conflict_retry(base);
    test_partial_and_release_paths(base);
    test_staged_commit_paths(base);

    printf("test_commit_lock_c: all checks passed\n");
    return 0;
}
