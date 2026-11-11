/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file test_fts_query.c
 * @brief Direct C coverage for typed full-text query construction, attachment,
 * execution, and early handle release.
 *
 * The Rust harness creates a two-fragment dataset with two inverted-indexed
 * text columns and passes its URI as argv[1]. It also creates a list-typed
 * fixture and passes its URI as argv[2] plus one row id from that fixture as
 * argv[3].
 */

#include "lance/lance.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

struct hit {
    int32_t id;
    float score;
};

static int compare_hits(const void* left, const void* right) {
    const struct hit* a = (const struct hit*)left;
    const struct hit* b = (const struct hit*)right;
    if (a->id != b->id) return a->id < b->id ? -1 : 1;
    if (a->score < b->score) return -1;
    if (a->score > b->score) return 1;
    return 0;
}

/* Collect every output row, preserving duplicate ids for list-element hits. */
static int collect_hits(LanceScanner* scanner, struct hit* hits, int capacity) {
    int count = 0;
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

        int id_index = -1;
        int score_index = -1;
        for (int64_t index = 0; index < schema.n_children; ++index) {
            const char* name = schema.children[index]->name;
            if (name && strcmp(name, "id") == 0) id_index = (int)index;
            if (name && strcmp(name, "_score") == 0) score_index = (int)index;
        }
        assert(id_index >= 0 && score_index >= 0);

        const int32_t* ids =
            (const int32_t*)array.children[id_index]->buffers[1];
        const float* scores =
            (const float*)array.children[score_index]->buffers[1];
        const int64_t id_offset = array.children[id_index]->offset;
        const int64_t score_offset = array.children[score_index]->offset;
        for (int64_t row = 0; row < array.length; ++row) {
            assert(count < capacity);
            hits[count].id = ids[id_offset + row];
            hits[count].score = scores[score_offset + row];
            ++count;
        }

        if (array.release) array.release(&array);
        if (schema.release) schema.release(&schema);
        lance_batch_free(batch);
    }
    qsort(hits, (size_t)count, sizeof(*hits), compare_hits);
    return count;
}

static LanceScanner* query_scanner(const LanceDataset* dataset,
                                   LanceFtsQuery* query) {
    assert(query != NULL);
    LanceScanner* scanner = lance_scanner_new(dataset, NULL, NULL);
    assert(scanner != NULL);
    if (lance_scanner_set_fts_query(scanner, query) != 0)
        fail_with_last_error("lance_scanner_set_fts_query");
    lance_fts_query_close(query);
    return scanner;
}

static void expect_ids(const struct hit* hits, int count,
                       const int32_t* expected, int expected_count) {
    assert(count == expected_count);
    for (int index = 0; index < count; ++index)
        assert(hits[index].id == expected[index]);
}

static int scores_for_id(const struct hit* hits, int count, int32_t id,
                         float* scores, int capacity) {
    int found = 0;
    for (int index = 0; index < count; ++index) {
        if (hits[index].id == id) {
            assert(found < capacity);
            scores[found++] = hits[index].score;
        }
    }
    return found;
}

static void expect_scores_multiplied(const struct hit* baseline,
                                     int baseline_count,
                                     const struct hit* scaled,
                                     int scaled_count, float factor) {
    assert(baseline_count == scaled_count);
    for (int index = 0; index < baseline_count; ++index) {
        assert(baseline[index].id == scaled[index].id);
        const float expected = baseline[index].score * factor;
        float tolerance = expected < 0.0f ? -expected : expected;
        if (tolerance < 1.0f) tolerance = 1.0f;
        tolerance *= 1e-5f;
        float difference = scaled[index].score - expected;
        if (difference < 0.0f) difference = -difference;
        assert(difference <= tolerance);
    }
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
    assert(argc == 4);
    printf("Running typed FTS C tests with dataset: %s\n", argv[1]);

    LanceDataset* dataset = lance_dataset_open(argv[1], NULL, 0);
    assert(dataset != NULL);
    LanceDataset* list_dataset = lance_dataset_open(argv[2], NULL, 0);
    assert(list_dataset != NULL);
    const uint64_t allowlist_row_id = strtoull(argv[3], NULL, 10);

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
               strstr(message, "FTS query must contain at least one leaf query") !=
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

    TEST(list_element_match);
    {
        struct hit hits[64];
        LanceFtsMatchOptions options;
        lance_fts_query_match_options_init(&options);
        options.document_granularity =
            LANCE_FTS_DOCUMENT_GRANULARITY_LIST_ELEMENT;
        LanceScanner* scanner = query_scanner(
            list_dataset, lance_fts_query_match_new("alpha", "tags", &options));
        const int count = collect_hits(scanner, hits, 64);
        lance_scanner_close(scanner);

        const int32_t expected[] = {1, 1, 2, 2, 8, 8, 9, 10};
        expect_ids(hits, count, expected, 8);
        for (int index = 0; index < count; ++index) {
            assert(hits[index].score > 0.0f);
            assert(hits[index].score == hits[index].score);
        }
    }
    PASS();

    TEST(list_element_boost);
    {
        struct hit baseline[64];
        struct hit boosted[64];
        LanceFtsMatchOptions options;
        lance_fts_query_match_options_init(&options);
        options.document_granularity =
            LANCE_FTS_DOCUMENT_GRANULARITY_LIST_ELEMENT;
        LanceScanner* scanner = query_scanner(
            list_dataset, lance_fts_query_match_new("alpha", "tags", &options));
        const int baseline_count = collect_hits(scanner, baseline, 64);
        lance_scanner_close(scanner);

        options.boost = 2.0f;
        scanner = query_scanner(
            list_dataset, lance_fts_query_match_new("alpha", "tags", &options));
        const int boosted_count = collect_hits(scanner, boosted, 64);
        lance_scanner_close(scanner);

        const int32_t expected[] = {1, 1, 2, 2, 8, 8, 9, 10};
        expect_ids(boosted, boosted_count, expected, 8);
        expect_scores_multiplied(baseline, baseline_count, boosted,
                                 boosted_count, 2.0f);
    }
    PASS();

    TEST(list_element_phrase_boundaries);
    {
        struct phrase_case {
            const char* terms;
            int32_t ids[3];
            int count;
        } cases[] = {
            {"alpha beta", {1, 9, 10}, 3},
            {"beta alpha", {2, 0, 0}, 1},
            {"beta gamma", {9, 0, 0}, 1},
        };
        for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]);
             ++index) {
            struct hit hits[64];
            LanceFtsPhraseOptions options;
            lance_fts_query_phrase_options_init(&options);
            options.document_granularity =
                LANCE_FTS_DOCUMENT_GRANULARITY_LIST_ELEMENT;
            LanceScanner* scanner = query_scanner(
                list_dataset, lance_fts_query_phrase_new(cases[index].terms,
                                                         "tags", &options));
            const int count = collect_hits(scanner, hits, 64);
            lance_scanner_close(scanner);
            expect_ids(hits, count, cases[index].ids, cases[index].count);
        }
    }
    PASS();

    TEST(list_element_row_id_prefilter);
    {
        struct hit baseline[64];
        struct hit filtered[64];
        LanceFtsMatchOptions options;
        lance_fts_query_match_options_init(&options);
        options.document_granularity =
            LANCE_FTS_DOCUMENT_GRANULARITY_LIST_ELEMENT;
        LanceScanner* scanner = query_scanner(
            list_dataset, lance_fts_query_match_new("alpha", "tags", &options));
        const int baseline_count = collect_hits(scanner, baseline, 64);
        lance_scanner_close(scanner);

        scanner = query_scanner(
            list_dataset, lance_fts_query_match_new("alpha", "tags", &options));
        assert(lance_scanner_set_row_addr_allowlist(scanner, &allowlist_row_id,
                                                     1) == 0);
        const int filtered_count = collect_hits(scanner, filtered, 64);
        lance_scanner_close(scanner);

        const int32_t expected[] = {1, 1};
        expect_ids(filtered, filtered_count, expected, 2);

        float baseline_scores[64];
        float filtered_scores[64];
        const int baseline_hits =
            scores_for_id(baseline, baseline_count, 1, baseline_scores, 64);
        const int filtered_hits =
            scores_for_id(filtered, filtered_count, 1, filtered_scores, 64);
        assert(baseline_hits == 2 && filtered_hits == baseline_hits);
        for (int index = 0; index < baseline_hits; ++index)
            assert(filtered_scores[index] == baseline_scores[index]);
    }
    PASS();

    TEST(list_element_rejections);
    {
        LanceFtsMatchOptions row_options;
        lance_fts_query_match_options_init(&row_options);
        row_options.document_granularity = LANCE_FTS_DOCUMENT_GRANULARITY_ROW;
        LanceScanner* scanner = query_scanner(
            list_dataset,
            lance_fts_query_match_new("alpha", "tags", &row_options));
        LanceBatch* batch = NULL;
        assert(lance_scanner_next(scanner, &batch) == -1);
        assert(batch == NULL);
        assert(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT);
        const char* message = lance_last_error_message();
        assert(message != NULL && strstr(message, "requested Row") != NULL &&
               strstr(message, "ListElement") != NULL);
        lance_free_string(message);
        lance_scanner_close(scanner);

        const char* columns[] = {"tags"};
        scanner = query_scanner(
            list_dataset,
            lance_fts_query_multi_match_new("alpha", columns, 1, NULL));
        batch = NULL;
        assert(lance_scanner_next(scanner, &batch) == -1);
        assert(batch == NULL);
        assert(lance_last_error_code() == LANCE_ERR_NOT_SUPPORTED);
        message = lance_last_error_message();
        assert(message != NULL &&
               strstr(message, "MultiMatch does not support ListElement "
                               "document granularity") != NULL);
        lance_free_string(message);
        lance_scanner_close(scanner);

        scanner = query_scanner(
            list_dataset,
            lance_fts_query_combined_fields_new("alpha", columns, 1, NULL));
        batch = NULL;
        assert(lance_scanner_next(scanner, &batch) == -1);
        assert(batch == NULL);
        assert(lance_last_error_code() == LANCE_ERR_NOT_SUPPORTED);
        message = lance_last_error_message();
        assert(message != NULL &&
               strstr(message, "combined_fields (BM25F) scores whole rows") !=
                   NULL);
        lance_free_string(message);
        lance_scanner_close(scanner);
    }
    PASS();

    lance_dataset_close(list_dataset);
    lance_dataset_close(dataset);
    printf("All typed FTS C tests passed!\n");
    return 0;
}
