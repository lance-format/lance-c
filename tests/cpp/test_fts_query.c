/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file test_fts_query.c
 * @brief Direct C coverage for typed full-text query construction, attachment,
 * execution, and early handle release.
 *
 * The Rust harness creates a two-fragment dataset with two inverted-indexed
 * text columns and passes its URI as argv[1].
 */

#include "lance/lance.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST(name) printf("  %s... ", #name)
#define PASS()     printf("OK\n")

static void fail_with_last_error(const char* context) {
    const char* message = lance_last_error_message();
    fprintf(stderr, "%s: %s\n", context, message ? message : "unknown error");
    if (message) lance_free_string(message);
    assert(0 && context);
}

static int drain_scanner(LanceScanner* scanner) {
    int total = 0;
    for (;;) {
        LanceBatch* batch = NULL;
        int rc = lance_scanner_next(scanner, &batch);
        if (rc == 1) break;
        if (rc == -1) fail_with_last_error("lance_scanner_next");
        assert(batch != NULL);

        struct ArrowArray array;
        struct ArrowSchema schema;
        memset(&array, 0, sizeof(array));
        memset(&schema, 0, sizeof(schema));
        if (lance_batch_to_arrow(batch, &array, &schema) != 0) {
            lance_batch_free(batch);
            fail_with_last_error("lance_batch_to_arrow");
        }
        total += (int)array.length;
        if (array.release) array.release(&array);
        if (schema.release) schema.release(&schema);
        lance_batch_free(batch);
    }
    return total;
}

static int count_match_rows(const LanceDataset* dataset) {
    LanceFtsMatchOptions options;
    lance_fts_query_match_options_init(&options);
    LanceFtsQuery* query =
        lance_fts_query_match_new("rust", "title", &options);
    assert(query != NULL);

    LanceScanner* scanner = lance_scanner_new(dataset, NULL, NULL);
    assert(scanner != NULL);
    if (lance_scanner_set_fts_query(scanner, query) != 0)
        fail_with_last_error("lance_scanner_set_fts_query");
    /* The scanner owns a snapshot; the query handle may be released now. */
    lance_fts_query_close(query);

    int rows = drain_scanner(scanner);
    lance_scanner_close(scanner);
    return rows;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    printf("Running typed FTS C tests with dataset: %s\n", argv[1]);

    LanceDataset* dataset = lance_dataset_open(argv[1], NULL, 0);
    assert(dataset != NULL);

    TEST(simple_match);
    assert(count_match_rows(dataset) == 2);
    PASS();

    TEST(boolean_tree);
    {
        LanceFtsQuery* must =
            lance_fts_query_match_new("alpha", "title", NULL);
        LanceFtsQuery* must_not =
            lance_fts_query_match_new("delta", "body", NULL);
        assert(must != NULL && must_not != NULL);
        int occurs[2] = {LANCE_FTS_OCCUR_MUST, LANCE_FTS_OCCUR_MUST_NOT};
        const LanceFtsQuery* children[2] = {must, must_not};
        LanceFtsQuery* query =
            lance_fts_query_boolean_new(occurs, children, 2);
        assert(query != NULL);
        lance_fts_query_close(must);
        lance_fts_query_close(must_not);

        LanceScanner* scanner = lance_scanner_new(dataset, NULL, NULL);
        assert(scanner != NULL);
        assert(lance_scanner_set_fts_query(scanner, query) == 0);
        lance_fts_query_close(query);
        assert(drain_scanner(scanner) == 2);
        lance_scanner_close(scanner);
    }
    PASS();

    TEST(phrase);
    {
        LanceFtsQuery* query =
            lance_fts_query_phrase_new("alpha beta", "title", NULL);
        assert(query != NULL);
        LanceScanner* scanner = lance_scanner_new(dataset, NULL, NULL);
        assert(scanner != NULL);
        assert(lance_scanner_set_fts_query(scanner, query) == 0);
        lance_fts_query_close(query);
        assert(drain_scanner(scanner) == 2);
        lance_scanner_close(scanner);
    }
    PASS();

    TEST(invalid_install_keeps_previous_query);
    {
        LanceFtsQuery* query =
            lance_fts_query_match_new("rust", "title", NULL);
        assert(query != NULL);
        LanceScanner* scanner = lance_scanner_new(dataset, NULL, NULL);
        assert(scanner != NULL);
        assert(lance_scanner_set_fts_query(scanner, query) == 0);
        lance_fts_query_close(query);

        assert(lance_scanner_set_fts_query(scanner, NULL) == -1);
        assert(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT);
        const char* message = lance_last_error_message();
        assert(message != NULL &&
               strstr(message, "query must not be NULL") != NULL);
        lance_free_string(message);

        assert(drain_scanner(scanner) == 2);
        lance_scanner_close(scanner);
    }
    PASS();

    TEST(empty_boolean_rejected_at_scan);
    {
        LanceFtsQuery* query = lance_fts_query_boolean_new(NULL, NULL, 0);
        assert(query != NULL);
        LanceScanner* scanner = lance_scanner_new(dataset, NULL, NULL);
        assert(scanner != NULL);
        assert(lance_scanner_set_fts_query(scanner, query) == 0);
        lance_fts_query_close(query);

        LanceBatch* batch = NULL;
        assert(lance_scanner_next(scanner, &batch) == -1);
        assert(batch == NULL);
        assert(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT);
        const char* message = lance_last_error_message();
        assert(message != NULL &&
               strstr(message,
                      "boolean query must have at least one should/must query") !=
                   NULL);
        lance_free_string(message);
        lance_scanner_close(scanner);
    }
    PASS();

    TEST(combined_fields_validation);
    {
        const char* columns[2] = {"title", "title"};
        LanceFtsQuery* query =
            lance_fts_query_combined_fields_new("rust", columns, 2, NULL);
        assert(query == NULL);
        assert(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT);
        const char* message = lance_last_error_message();
        assert(message != NULL && strstr(message, "unique") != NULL);
        lance_free_string(message);
    }
    PASS();

    lance_dataset_close(dataset);
    printf("All typed FTS C tests passed!\n");
    return 0;
}
