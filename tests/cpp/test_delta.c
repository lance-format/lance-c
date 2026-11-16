/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/*
 * Native dataset delta C API consumer.
 *
 * argv[1] = multi-fragment dataset without stable row ids (two versions)
 * argv[2] = history dataset with stable row ids and five versions
 * argv[3] = blob dataset (kept for sibling binaries; not used here)
 * argv[4..7] = begin/end timestamps (seconds, nanos) of two history versions
 * argv[8..9] = the versions that window resolves to natively
 */

#include "lance/lance.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(condition, message)                                             \
    do {                                                                       \
        if (!(condition)) {                                                    \
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
            fprintf(stderr, "CHECK_OK failed: %s:%d: %s\n", __FILE__,          \
                    __LINE__, lance_message ? lance_message : "unknown");       \
            if (lance_message) lance_free_string(lance_message);               \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

static int compare_i32(const void *left, const void *right) {
    int32_t a = *(const int32_t *)left;
    int32_t b = *(const int32_t *)right;
    return (a > b) - (a < b);
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static int64_t stream_field_count(struct ArrowArrayStream *stream) {
    struct ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ASSERT(stream->get_schema(stream, &schema) == 0, "read stream schema");
    int64_t count = schema.n_children;
    schema.release(&schema);
    return count;
}

static size_t collect_first_i32(struct ArrowArrayStream *stream, int32_t *out,
                                size_t capacity) {
    size_t count = 0;
    while (1) {
        struct ArrowArray batch;
        memset(&batch, 0, sizeof(batch));
        ASSERT(stream->get_next(stream, &batch) == 0, "read id batch");
        if (!batch.release) break;
        ASSERT(batch.n_children > 0, "id column present");
        const int32_t *ids = (const int32_t *)batch.children[0]->buffers[1];
        ASSERT(count + (size_t)batch.length <= capacity, "id capacity");
        for (int64_t row = 0; row < batch.length; row++) {
            out[count++] = ids[row];
        }
        batch.release(&batch);
    }
    if (count > 1) qsort(out, count, sizeof(int32_t), compare_i32);
    return count;
}

static size_t collect_first_u64(struct ArrowArrayStream *stream, uint64_t *out,
                                size_t capacity) {
    size_t count = 0;
    while (1) {
        struct ArrowArray batch;
        memset(&batch, 0, sizeof(batch));
        ASSERT(stream->get_next(stream, &batch) == 0, "read u64 batch");
        if (!batch.release) break;
        ASSERT(batch.n_children > 0, "u64 column present");
        const uint64_t *values = (const uint64_t *)batch.children[0]->buffers[1];
        ASSERT(count + (size_t)batch.length <= capacity, "u64 capacity");
        for (int64_t row = 0; row < batch.length; row++) {
            out[count++] = values[row];
        }
        batch.release(&batch);
    }
    if (count > 1) qsort(out, count, sizeof(uint64_t), compare_u64);
    return count;
}

static void assert_i32_set(const char *label, struct ArrowArrayStream *stream,
                           const int32_t *expected, size_t expected_count) {
    int32_t actual[64];
    size_t count = collect_first_i32(stream, actual, 64);
    if (count != expected_count) {
        fprintf(stderr, "%s: expected %zu rows, got %zu\n", label,
                expected_count, count);
        exit(1);
    }
    for (size_t i = 0; i < count; i++) {
        if (actual[i] != expected[i]) {
            fprintf(stderr, "%s: row %zu expected %d, got %d\n", label, i,
                    expected[i], actual[i]);
            exit(1);
        }
    }
    if (stream->release) stream->release(stream);
}

static void assert_u64_set(const char *label, struct ArrowArrayStream *stream,
                           const uint64_t *expected, size_t expected_count) {
    uint64_t actual[64];
    size_t count = collect_first_u64(stream, actual, 64);
    if (count != expected_count) {
        fprintf(stderr, "%s: expected %zu rows, got %zu\n", label,
                expected_count, count);
        exit(1);
    }
    for (size_t i = 0; i < count; i++) {
        if (actual[i] != expected[i]) {
            fprintf(stderr, "%s: row %zu expected %llu, got %llu\n", label, i,
                    (unsigned long long)expected[i],
                    (unsigned long long)actual[i]);
            exit(1);
        }
    }
    if (stream->release) stream->release(stream);
}

static LanceDatasetDelta *build_delta_versions(const LanceDataset *dataset,
                                               uint64_t begin, uint64_t end) {
    LanceDatasetDeltaBuilder *builder = lance_dataset_delta_builder_new(dataset);
    ASSERT(builder != NULL, "create delta builder");
    CHECK_OK();
    ASSERT(lance_dataset_delta_builder_set_begin_version(builder, begin) == 0,
           "set begin version");
    CHECK_OK();
    ASSERT(lance_dataset_delta_builder_set_end_version(builder, end) == 0,
           "set end version");
    CHECK_OK();
    LanceDatasetDelta *delta = NULL;
    ASSERT(lance_dataset_delta_builder_build(builder, &delta) == 0,
           "build version delta");
    CHECK_OK();
    ASSERT(delta != NULL, "delta handle published");
    return delta;
}

static LanceDatasetDelta *build_delta_dates(const LanceDataset *dataset,
                                            int64_t begin_seconds,
                                            uint32_t begin_nanos,
                                            int64_t end_seconds,
                                            uint32_t end_nanos) {
    LanceDatasetDeltaBuilder *builder = lance_dataset_delta_builder_new(dataset);
    ASSERT(builder != NULL, "create date delta builder");
    CHECK_OK();
    LanceDeltaTimestamp begin = {begin_seconds, begin_nanos};
    LanceDeltaTimestamp end = {end_seconds, end_nanos};
    ASSERT(lance_dataset_delta_builder_set_begin_timestamp(builder, &begin) == 0,
           "set begin timestamp");
    CHECK_OK();
    ASSERT(lance_dataset_delta_builder_set_end_timestamp(builder, &end) == 0,
           "set end timestamp");
    CHECK_OK();
    LanceDatasetDelta *delta = NULL;
    ASSERT(lance_dataset_delta_builder_build(builder, &delta) == 0,
           "build date delta");
    CHECK_OK();
    return delta;
}

static struct ArrowArrayStream inserted_rows(const LanceDatasetDelta *delta) {
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_dataset_delta_get_inserted_rows(delta, &stream) == 0,
           "stream inserted rows");
    CHECK_OK();
    return stream;
}

static struct ArrowArrayStream updated_rows(const LanceDatasetDelta *delta) {
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_dataset_delta_get_updated_rows(delta, &stream) == 0,
           "stream updated rows");
    CHECK_OK();
    return stream;
}

static struct ArrowArrayStream upserted_rows(const LanceDatasetDelta *delta) {
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_dataset_delta_get_upserted_rows(delta, &stream) == 0,
           "stream upserted rows");
    CHECK_OK();
    return stream;
}

static struct ArrowArrayStream deleted_row_ids(const LanceDatasetDelta *delta) {
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_dataset_delta_get_deleted_row_ids(delta, &stream) == 0,
           "stream deleted row ids");
    CHECK_OK();
    return stream;
}

static void test_full_and_version_ranges(const char *uri) {
    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open history dataset");
    CHECK_OK();

    /* Full range from version 0: every row still live at the end version, no
     * updates, and version 0 behaves as the empty snapshot for deleted ids.
     * Row 3 was deleted at version 4, so the end snapshot no longer holds it. */
    LanceDatasetDelta *delta = build_delta_versions(dataset, 0, 5);
    struct ArrowArrayStream stream = inserted_rows(delta);
    ASSERT(stream_field_count(&stream) == 6,
           "row streams carry the row-version metadata columns");
    int32_t all_ids[] = {1, 2, 4, 5, 6, 7};
    assert_i32_set("full inserted", &stream, all_ids, 6);

    stream = updated_rows(delta);
    assert_i32_set("full updated", &stream, NULL, 0);

    stream = upserted_rows(delta);
    assert_i32_set("full upserted", &stream, all_ids, 6);

    stream = deleted_row_ids(delta);
    ASSERT(stream_field_count(&stream) == 1, "deleted ids have one column");
    assert_u64_set("full deleted", &stream, NULL, 0);
    lance_dataset_delta_close(delta);

    /* compared_against_version(2) describes the (2, 5] change set. */
    LanceDatasetDeltaBuilder *builder = lance_dataset_delta_builder_new(dataset);
    ASSERT(builder != NULL, "create compared builder");
    CHECK_OK();
    ASSERT(lance_dataset_delta_builder_set_compared_against_version(builder, 2) == 0,
           "compare against version 2");
    CHECK_OK();
    delta = NULL;
    ASSERT(lance_dataset_delta_builder_build(builder, &delta) == 0,
           "build compared delta");
    CHECK_OK();

    stream = inserted_rows(delta);
    /* Row 6 was created at version 2, so the exclusive begin excludes it. */
    int32_t inserted_after_2[] = {7};
    assert_i32_set("inserted after 2", &stream, inserted_after_2, 1);

    stream = updated_rows(delta);
    int32_t updated_after_2[] = {2, 6};
    assert_i32_set("updated after 2", &stream, updated_after_2, 2);

    stream = upserted_rows(delta);
    int32_t upserted_after_2[] = {2, 6, 7};
    assert_i32_set("upserted after 2", &stream, upserted_after_2, 3);

    stream = deleted_row_ids(delta);
    uint64_t deleted_after_2[] = {2};
    assert_u64_set("deleted after 2", &stream, deleted_after_2, 1);

    /* Row-change streams carry the native metadata columns. */
    stream = inserted_rows(delta);
    ASSERT(stream_field_count(&stream) == 6, "inserted stream fields");
    if (stream.release) stream.release(&stream);

    lance_dataset_delta_close(delta);
    lance_dataset_close(dataset);
}

static void test_transaction_list(const char *uri) {
    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open history dataset");
    CHECK_OK();

    LanceDatasetDelta *delta = build_delta_versions(dataset, 0, 5);
    LanceTransaction **transactions = NULL;
    size_t count = 0;
    ASSERT(lance_dataset_delta_list_transactions(delta, &transactions, &count) == 0,
           "list transactions");
    CHECK_OK();
    ASSERT(count == 5, "one transaction per committed version");
    ASSERT(transactions != NULL, "transaction array published");
    for (size_t i = 0; i < count; i++) {
        ASSERT(transactions[i] != NULL, "transaction entry present");
        uint64_t read_version = 0;
        ASSERT(lance_transaction_read_version(transactions[i], &read_version) == 0,
               "read transaction version");
        CHECK_OK();
        /* A transaction records the version it read, which is the parent of
         * the version it committed. */
        ASSERT(read_version == (uint64_t)i,
               "transactions are listed in commit order");
        const char *uuid = lance_transaction_uuid(transactions[i]);
        ASSERT(uuid != NULL, "transaction uuid present");
        CHECK_OK();
        ASSERT(uuid[0] != '\0', "committed transactions carry a uuid");
        lance_free_string(uuid);
    }
    lance_transaction_array_free(transactions, count);
    /* NULL and zero-length arrays are no-ops. */
    lance_transaction_array_free(NULL, 0);

    /* The delta remains usable after listing. */
    struct ArrowArrayStream stream = inserted_rows(delta);
    int32_t all_ids[] = {1, 2, 4, 5, 6, 7};
    assert_i32_set("post-list inserted", &stream, all_ids, 6);
    lance_dataset_delta_close(delta);

    /* An empty range lists nothing and every stream is empty. */
    delta = build_delta_versions(dataset, 3, 3);
    transactions = (LanceTransaction **)(uintptr_t)1;
    count = 42;
    ASSERT(lance_dataset_delta_list_transactions(delta, &transactions, &count) == 0,
           "list empty range");
    CHECK_OK();
    ASSERT(count == 0, "empty range has no transactions");
    ASSERT(transactions == NULL, "empty range publishes no array");
    stream = inserted_rows(delta);
    assert_i32_set("empty inserted", &stream, NULL, 0);
    stream = updated_rows(delta);
    assert_i32_set("empty updated", &stream, NULL, 0);
    stream = upserted_rows(delta);
    assert_i32_set("empty upserted", &stream, NULL, 0);
    stream = deleted_row_ids(delta);
    assert_u64_set("empty deleted", &stream, NULL, 0);
    lance_dataset_delta_close(delta);

    lance_dataset_close(dataset);
}

static void test_date_mode(const char *uri, int64_t begin_seconds,
                           uint32_t begin_nanos, int64_t end_seconds,
                           uint32_t end_nanos, uint64_t resolved_begin,
                           uint64_t resolved_end) {
    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open history dataset");
    CHECK_OK();

    LanceDatasetDelta *by_date =
        build_delta_dates(dataset, begin_seconds, begin_nanos, end_seconds, end_nanos);
    LanceDatasetDelta *by_version =
        build_delta_versions(dataset, resolved_begin, resolved_end);

    int32_t dates[64];
    int32_t versions[64];
    struct ArrowArrayStream stream = inserted_rows(by_date);
    size_t dates_count = collect_first_i32(&stream, dates, 64);
    if (stream.release) stream.release(&stream);
    stream = inserted_rows(by_version);
    size_t versions_count = collect_first_i32(&stream, versions, 64);
    if (stream.release) stream.release(&stream);
    ASSERT(dates_count == versions_count,
           "date mode matches the resolved version range (inserted)");
    for (size_t i = 0; i < dates_count; i++) {
        ASSERT(dates[i] == versions[i], "date mode inserted row parity");
    }

    uint64_t date_deleted[64];
    uint64_t version_deleted[64];
    stream = deleted_row_ids(by_date);
    size_t date_deleted_count = collect_first_u64(&stream, date_deleted, 64);
    if (stream.release) stream.release(&stream);
    stream = deleted_row_ids(by_version);
    size_t version_deleted_count = collect_first_u64(&stream, version_deleted, 64);
    if (stream.release) stream.release(&stream);
    ASSERT(date_deleted_count == version_deleted_count,
           "date mode matches the resolved version range (deleted)");
    for (size_t i = 0; i < date_deleted_count; i++) {
        ASSERT(date_deleted[i] == version_deleted[i],
               "date mode deleted row-id parity");
    }

    lance_dataset_delta_close(by_date);
    lance_dataset_delta_close(by_version);
    lance_dataset_close(dataset);
}

static void test_streams_after_parent_close(const char *uri) {
    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open history dataset");
    CHECK_OK();

    LanceDatasetDeltaBuilder *builder = lance_dataset_delta_builder_new(dataset);
    ASSERT(builder != NULL, "create outliving builder");
    CHECK_OK();
    ASSERT(lance_dataset_delta_builder_set_compared_against_version(builder, 1) == 0,
           "compare against version 1");
    CHECK_OK();
    LanceDatasetDelta *delta = NULL;
    ASSERT(lance_dataset_delta_builder_build(builder, &delta) == 0,
           "build outliving delta");
    CHECK_OK();
    struct ArrowArrayStream stream = inserted_rows(delta);
    /* Both parent handles are gone before the first batch is read. */
    lance_dataset_delta_close(delta);
    lance_dataset_close(dataset);

    int32_t ids[] = {5, 6, 7};
    assert_i32_set("outliving inserted", &stream, ids, 3);
}

static void test_reversed_and_unsupported(const char *history_uri,
                                          const char *plain_uri) {
    LanceDataset *dataset = lance_dataset_open(history_uri, NULL, 0);
    ASSERT(dataset != NULL, "open history dataset");
    CHECK_OK();

    /* A reversed explicit range builds natively; row changes are empty while
     * deleted ids reject the range at the stream call. */
    LanceDatasetDelta *delta = build_delta_versions(dataset, 5, 2);
    struct ArrowArrayStream stream = inserted_rows(delta);
    assert_i32_set("reversed inserted", &stream, NULL, 0);
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_dataset_delta_get_deleted_row_ids(delta, &stream) != 0,
           "reversed range rejects deleted ids");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "reversed range is invalid input");
    lance_free_string(lance_last_error_message());
    /* The delta stays reusable after the failed stream call. */
    stream = updated_rows(delta);
    assert_i32_set("reversed updated", &stream, NULL, 0);
    lance_dataset_delta_close(delta);
    lance_dataset_close(dataset);

    /* Deleted ids require stable row ids on both endpoints; datasets without
     * them build fine and reject the stream natively. */
    dataset = lance_dataset_open(plain_uri, NULL, 0);
    ASSERT(dataset != NULL, "open plain dataset");
    CHECK_OK();
    delta = build_delta_versions(dataset, 1, 2);
    memset(&stream, 0, sizeof(stream));
    ASSERT(lance_dataset_delta_get_deleted_row_ids(delta, &stream) != 0,
           "non-stable dataset rejects deleted ids");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "missing stable row ids is invalid input");
    const char *message = lance_last_error_message();
    ASSERT(message != NULL && strstr(message, "stable row ids") != NULL,
           "native stable-row-id error surfaces");
    lance_free_string(message);
    /* v15 requires stable row ids for every row-change stream, not only for
     * deleted ids, so inserted/updated/upserted all reject this dataset. */
    const char *row_change_errors[] = {
        "plain inserted rejects missing stable row ids",
        "plain updated rejects missing stable row ids",
        "plain upserted rejects missing stable row ids",
    };
    for (int kind = 0; kind < 3; kind++) {
        memset(&stream, 0, sizeof(stream));
        int32_t rc = kind == 0   ? lance_dataset_delta_get_inserted_rows(delta, &stream)
                     : kind == 1 ? lance_dataset_delta_get_updated_rows(delta, &stream)
                                 : lance_dataset_delta_get_upserted_rows(delta, &stream);
        ASSERT(rc != 0, row_change_errors[kind]);
        ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
               "missing stable row ids is invalid input");
        const char *row_change_message = lance_last_error_message();
        ASSERT(row_change_message != NULL &&
                   strstr(row_change_message, "stable row ids") != NULL,
               "native stable-row-id error surfaces for row changes");
        lance_free_string(row_change_message);
    }
    lance_dataset_delta_close(delta);
    lance_dataset_close(dataset);
}

static void test_build_errors_and_guards(const char *uri) {
    LanceDataset *dataset = lance_dataset_open(uri, NULL, 0);
    ASSERT(dataset != NULL, "open history dataset");
    CHECK_OK();

    /* Native range-combination validation happens synchronously at build. */
    LanceDatasetDeltaBuilder *builder = lance_dataset_delta_builder_new(dataset);
    ASSERT(builder != NULL, "create conflict builder");
    CHECK_OK();
    LanceDatasetDelta *delta = NULL;
    ASSERT(lance_dataset_delta_builder_build(builder, &delta) != 0,
           "missing range fails at build");
    ASSERT(delta == NULL, "failed build publishes no delta");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "missing range is invalid input");
    lance_free_string(lance_last_error_message());

    builder = lance_dataset_delta_builder_new(dataset);
    ASSERT(builder != NULL, "create partial builder");
    CHECK_OK();
    ASSERT(lance_dataset_delta_builder_set_begin_version(builder, 1) == 0,
           "set partial begin");
    CHECK_OK();
    ASSERT(lance_dataset_delta_builder_build(builder, &delta) != 0,
           "partial version range fails at build");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "partial range is invalid input");
    lance_free_string(lance_last_error_message());

    builder = lance_dataset_delta_builder_new(dataset);
    ASSERT(builder != NULL, "create conflict builder");
    CHECK_OK();
    ASSERT(lance_dataset_delta_builder_set_compared_against_version(builder, 2) == 0,
           "set compared version");
    CHECK_OK();
    ASSERT(lance_dataset_delta_builder_set_begin_version(builder, 1) == 0,
           "set conflicting begin version");
    CHECK_OK();
    ASSERT(lance_dataset_delta_builder_build(builder, &delta) != 0,
           "compared version conflicts with an explicit range");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "conflicting range is invalid input");
    lance_free_string(lance_last_error_message());

    /* Foreign-pointer guards preserve the builder's prior state. */
    builder = lance_dataset_delta_builder_new(dataset);
    ASSERT(builder != NULL, "create NULL-timestamp builder");
    CHECK_OK();
    ASSERT(lance_dataset_delta_builder_set_compared_against_version(builder, 1) == 0,
           "set compared version before NULL guard");
    CHECK_OK();
    ASSERT(lance_dataset_delta_builder_set_begin_timestamp(builder, NULL) != 0,
           "NULL begin timestamp rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL timestamp is invalid input");
    lance_free_string(lance_last_error_message());
    /* A timestamp native chrono cannot represent is rejected the same way. */
    LanceDeltaTimestamp unrepresentable = {INT64_MAX, 0};
    ASSERT(lance_dataset_delta_builder_set_end_timestamp(builder, &unrepresentable) != 0,
           "unrepresentable timestamp rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "unrepresentable timestamp is invalid input");
    lance_free_string(lance_last_error_message());
    ASSERT(lance_dataset_delta_builder_build(builder, &delta) == 0,
           "compared version survives the rejected timestamps");
    CHECK_OK();
    struct ArrowArrayStream stream = inserted_rows(delta);
    int32_t ids[] = {5, 6, 7};
    assert_i32_set("post-guard inserted", &stream, ids, 3);

    /* NULL out slots are rejected without consuming the delta. */
    ASSERT(lance_dataset_delta_list_transactions(delta, NULL, NULL) != 0,
           "NULL transaction slots rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL slots are invalid input");
    lance_free_string(lance_last_error_message());
    ASSERT(lance_dataset_delta_get_inserted_rows(delta, NULL) != 0,
           "NULL stream slot rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL stream slot is invalid input");
    lance_free_string(lance_last_error_message());
    stream = inserted_rows(delta);
    assert_i32_set("post-NULL inserted", &stream, ids, 3);

    lance_dataset_delta_close(delta);
    ASSERT(lance_dataset_delta_builder_new(NULL) == NULL, "NULL dataset rejected");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "NULL dataset is invalid input");
    lance_free_string(lance_last_error_message());

    /* Handles still owned by the caller close NULL-safely. */
    lance_dataset_delta_close(NULL);
    lance_dataset_delta_builder_close(NULL);
    lance_dataset_close(dataset);
}

int main(int argc, char **argv) {
    if (argc < 10) {
        fprintf(stderr,
                "usage: %s <plain-dataset> <history-dataset> <blob-dataset> "
                "<begin-sec> <begin-nanos> <end-sec> <end-nanos> "
                "<resolved-begin> <resolved-end>\n",
                argv[0]);
        return 2;
    }
    const char *plain_uri = argv[1];
    const char *history_uri = argv[2];
    int64_t begin_seconds = strtoll(argv[4], NULL, 10);
    uint32_t begin_nanos = (uint32_t)strtoul(argv[5], NULL, 10);
    int64_t end_seconds = strtoll(argv[6], NULL, 10);
    uint32_t end_nanos = (uint32_t)strtoul(argv[7], NULL, 10);
    uint64_t resolved_begin = strtoull(argv[8], NULL, 10);
    uint64_t resolved_end = strtoull(argv[9], NULL, 10);

    test_full_and_version_ranges(history_uri);
    test_transaction_list(history_uri);
    test_date_mode(history_uri, begin_seconds, begin_nanos, end_seconds,
                   end_nanos, resolved_begin, resolved_end);
    test_streams_after_parent_close(history_uri);
    test_reversed_and_unsupported(history_uri, plain_uri);
    test_build_errors_and_guards(history_uri);
    printf("test_delta.c: all checks passed\n");
    return 0;
}
