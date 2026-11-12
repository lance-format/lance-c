/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file test_fts_query.cpp
 * @brief Functional C++ coverage for typed full-text query trees.
 *
 * The Rust harness creates a two-fragment dataset with two inverted-indexed
 * text columns and passes its URI as argv[1]. It also creates a list-typed
 * fixture and passes its URI as argv[2] plus one row id from that fixture as
 * argv[3].
 */

#include "lance/lance.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define TEST(name) printf("  %s... ", #name)
#define PASS()     printf("OK\n")

struct QueryResult {
    std::vector<int32_t> ids;
    std::vector<float> scores;
};

static std::string take_error() {
    const char* message = lance_last_error_message();
    std::string text = message ? message : "";
    if (message) lance_free_string(message);
    return text;
}

static QueryResult collect(lance::Scanner& scanner) {
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);

    ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    if (stream.get_schema(&stream, &schema) != 0) {
        const char* message =
            stream.get_last_error ? stream.get_last_error(&stream) : nullptr;
        std::string text = message ? message : "get_schema failed";
        if (stream.release) stream.release(&stream);
        throw lance::Error(LANCE_ERR_INTERNAL, text);
    }

    int64_t id_index = -1;
    int64_t score_index = -1;
    for (int64_t index = 0; index < schema.n_children; ++index) {
        const char* name = schema.children[index]->name;
        if (name && strcmp(name, "id") == 0) id_index = index;
        if (name && strcmp(name, "_score") == 0) score_index = index;
    }
    assert(id_index >= 0 && "id column missing from FTS output");
    assert(score_index >= 0 && "_score column missing from FTS output");
    if (schema.release) schema.release(&schema);

    QueryResult result;
    for (;;) {
        ArrowArray array;
        memset(&array, 0, sizeof(array));
        int rc = stream.get_next(&stream, &array);
        if (rc != 0) {
            const char* message =
                stream.get_last_error ? stream.get_last_error(&stream) : nullptr;
            std::string text = message ? message : "get_next failed";
            if (stream.release) stream.release(&stream);
            throw lance::Error(LANCE_ERR_INTERNAL, text);
        }
        if (!array.release) break;

        const auto* ids = static_cast<const int32_t*>(
            array.children[id_index]->buffers[1]);
        const auto* scores = static_cast<const float*>(
            array.children[score_index]->buffers[1]);
        const int64_t id_offset = array.children[id_index]->offset;
        const int64_t score_offset = array.children[score_index]->offset;
        for (int64_t row = 0; row < array.length; ++row) {
            result.ids.push_back(ids[id_offset + row]);
            result.scores.push_back(scores[score_offset + row]);
        }
        array.release(&array);
    }
    if (stream.release) stream.release(&stream);
    return result;
}

static void expect_ids(QueryResult result, std::vector<int32_t> expected) {
    std::sort(result.ids.begin(), result.ids.end());
    std::sort(expected.begin(), expected.end());
    assert(result.ids == expected);
}

static float score_of(const QueryResult& result, int32_t id) {
    for (size_t index = 0; index < result.ids.size(); ++index) {
        if (result.ids[index] == id) return result.scores[index];
    }
    assert(false && "row id missing from FTS result");
    return 0.0f;
}

static void expect_score(QueryResult result, int32_t id, float expected) {
    const float actual = score_of(result, id);
    const float tolerance = 1e-5f * std::max(1.0f, std::fabs(expected));
    assert(std::fabs(actual - expected) <= tolerance);
}

static std::vector<float> scores_of(const QueryResult& result, int32_t id) {
    std::vector<float> scores;
    for (size_t index = 0; index < result.ids.size(); ++index) {
        if (result.ids[index] == id) scores.push_back(result.scores[index]);
    }
    std::sort(scores.begin(), scores.end());
    return scores;
}

static void expect_scores_multiplied(const QueryResult& baseline,
                                     const QueryResult& scaled, int32_t id,
                                     float factor) {
    const auto expected = scores_of(baseline, id);
    const auto actual = scores_of(scaled, id);
    assert(expected.size() == actual.size());
    for (size_t index = 0; index < expected.size(); ++index) {
        const float target = expected[index] * factor;
        const float tolerance =
            1e-5f * std::max(1.0f, std::fabs(target));
        assert(std::fabs(actual[index] - target) <= tolerance);
    }
}

static void expect_scores_equal(const QueryResult& expected,
                                const QueryResult& actual, int32_t id) {
    assert(scores_of(expected, id) == scores_of(actual, id));
}

static void test_match_boost_and_granularity(const lance::Dataset& dataset) {
    TEST(test_match_boost_and_granularity);

    auto scanner = dataset.scan();
    {
        auto query = lance::FtsQuery::match("rust", std::string("title"));
        scanner.full_text_search(query);
        // `query` is destroyed here; the scanner owns a snapshot.
    }
    auto baseline = collect(scanner);
    expect_ids(baseline, {5, 6});

    lance::FtsMatchOptions boosted_options;
    boosted_options.boost = 2.0f;
    auto boosted_scanner = dataset.scan();
    {
        auto query =
            lance::FtsQuery::match("rust", std::string("title"), boosted_options);
        boosted_scanner.full_text_search(query);
    }
    auto boosted = collect(boosted_scanner);
    expect_ids(boosted, {5, 6});
    expect_score(boosted, 5, 2.0f * score_of(baseline, 5));
    expect_score(boosted, 6, 2.0f * score_of(baseline, 6));

    lance::FtsMatchOptions row_options;
    row_options.document_granularity = lance::FtsDocumentGranularity::Row;
    auto row_scanner = dataset.scan();
    {
        auto query =
            lance::FtsQuery::match("rust", std::string("title"), row_options);
        row_scanner.full_text_search(query);
    }
    auto row = collect(row_scanner);
    expect_ids(row, {5, 6});
    expect_score(row, 5, score_of(baseline, 5));
    expect_score(row, 6, score_of(baseline, 6));

    PASS();
}

static void test_boolean_phrase_and_boost(const lance::Dataset& dataset) {
    TEST(test_boolean_phrase_and_boost);

    auto boolean_scanner = dataset.scan();
    {
        auto must = lance::FtsQuery::match("alpha", std::string("title"));
        auto should = lance::FtsQuery::match("rust", std::string("body"));
        auto must_not = lance::FtsQuery::match("delta", std::string("body"));
        auto query = lance::FtsQuery::boolean({
            {lance::FtsOccur::Must, &must},
            {lance::FtsOccur::Should, &should},
            {lance::FtsOccur::MustNot, &must_not},
        });
        boolean_scanner.full_text_search(query);
        // The composite and all children are destroyed before execution.
    }
    auto boolean = collect(boolean_scanner);
    expect_ids(boolean, {1, 3});
    assert(score_of(boolean, 1) > score_of(boolean, 3));

    auto phrase_scanner = dataset.scan();
    {
        auto query = lance::FtsQuery::phrase("alpha beta", std::string("title"));
        phrase_scanner.full_text_search(query);
    }
    auto phrase = collect(phrase_scanner);
    expect_ids(phrase, {1, 3});

    auto positive_scanner = dataset.scan();
    {
        auto query = lance::FtsQuery::match("alpha", std::string("title"));
        positive_scanner.full_text_search(query);
    }
    auto positive = collect(positive_scanner);
    expect_ids(positive, {1, 2, 3});

    lance::FtsBoostOptions boost_options;
    boost_options.negative_boost = 0.5f;
    auto boost_scanner = dataset.scan();
    {
        auto positive_query =
            lance::FtsQuery::match("alpha", std::string("title"));
        auto negative_query =
            lance::FtsQuery::match("beta", std::string("title"));
        auto query =
            lance::FtsQuery::boost(positive_query, negative_query, boost_options);
        boost_scanner.full_text_search(query);
    }
    auto boosted = collect(boost_scanner);
    expect_ids(boosted, {1, 2, 3});
    assert(score_of(boosted, 1) < score_of(positive, 1));
    expect_score(boosted, 2, score_of(positive, 2));

    PASS();
}

static void test_multi_match_and_combined_fields(const lance::Dataset& dataset) {
    TEST(test_multi_match_and_combined_fields);

    auto multi_scanner = dataset.scan();
    {
        auto query =
            lance::FtsQuery::multi_match("rust", {"title", "body"}, {});
        multi_scanner.full_text_search(query);
    }
    auto multi = collect(multi_scanner);
    expect_ids(multi, {1, 2, 5, 6});

    auto combined_scanner = dataset.scan();
    {
        auto query =
            lance::FtsQuery::combined_fields("rust", {"title", "body"}, {});
        combined_scanner.full_text_search(query);
    }
    auto combined = collect(combined_scanner);
    expect_ids(combined, {1, 2, 5, 6});

    bool differs = false;
    for (int32_t id : multi.ids) {
        if (std::fabs(score_of(multi, id) - score_of(combined, id)) > 1e-6f) {
            differs = true;
        }
    }
    assert(differs && "BM25F and best-field scores must be distinguishable");

    PASS();
}

static void test_validation_and_replacement(const lance::Dataset& dataset) {
    TEST(test_validation_and_replacement);

    bool rejected = false;
    try {
        auto query =
            lance::FtsQuery::combined_fields("rust", {"title", "title"}, {});
    } catch (const lance::Error& error) {
        rejected = error.code == LANCE_ERR_INVALID_ARGUMENT;
    }
    assert(rejected && "duplicate combined_fields columns must be rejected");

    auto previous = lance::FtsQuery::match("alpha", std::string("title"));
    auto scanner = dataset.scan();
    scanner.full_text_search(previous);
    assert(lance_scanner_set_fts_query(scanner.c_handle(), nullptr) == -1);
    assert(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT);
    const std::string message = take_error();
    assert(message.find("query must not be NULL") != std::string::npos);
    expect_ids(collect(scanner), {1, 2, 3});

    auto empty_scanner = dataset.scan();
    auto empty = lance::FtsQuery::boolean({});
    empty_scanner.full_text_search(empty);
    bool empty_rejected = false;
    try {
        (void)collect(empty_scanner);
    } catch (const lance::Error& error) {
        const std::string text = error.what();
        empty_rejected =
            error.code == LANCE_ERR_INVALID_ARGUMENT &&
            text.find("FTS query must contain at least one leaf query") !=
                std::string::npos;
    }
    assert(empty_rejected);

    PASS();
}

static void test_list_element_execution(const lance::Dataset& dataset,
                                        uint64_t row_id) {
    TEST(test_list_element_execution);

    const std::vector<int32_t> alpha_ids = {1, 1, 2, 2, 8, 8, 9, 10};
    lance::FtsMatchOptions match_options;
    match_options.document_granularity =
        lance::FtsDocumentGranularity::ListElement;

    auto scanner = dataset.scan();
    {
        auto query = lance::FtsQuery::match("alpha", std::string("tags"),
                                            match_options);
        scanner.full_text_search(query);
    }
    auto alpha = collect(scanner);
    expect_ids(alpha, alpha_ids);
    for (float score : alpha.scores) assert(score > 0.0f);

    lance::FtsMatchOptions unspecified_options;
    auto unspecified_scanner = dataset.scan();
    {
        auto query = lance::FtsQuery::match("alpha", std::string("tags"),
                                            unspecified_options);
        unspecified_scanner.full_text_search(query);
    }
    auto unspecified = collect(unspecified_scanner);
    expect_ids(unspecified, alpha_ids);
    for (int32_t id : {1, 2, 8, 9, 10})
        expect_scores_equal(alpha, unspecified, id);

    lance::FtsMatchOptions boost_options = match_options;
    boost_options.boost = 2.0f;
    auto boost_scanner = dataset.scan();
    {
        auto query =
            lance::FtsQuery::match("alpha", std::string("tags"), boost_options);
        boost_scanner.full_text_search(query);
    }
    auto boosted = collect(boost_scanner);
    expect_ids(boosted, alpha_ids);
    for (int32_t id : {1, 2, 8, 9, 10})
        expect_scores_multiplied(alpha, boosted, id, 2.0f);

    lance::FtsPhraseOptions phrase_options;
    phrase_options.document_granularity =
        lance::FtsDocumentGranularity::ListElement;
    auto phrase = [&dataset, &phrase_options](const std::string& terms) {
        auto phrase_scanner = dataset.scan();
        {
            auto query = lance::FtsQuery::phrase(terms, std::string("tags"),
                                                 phrase_options);
            phrase_scanner.full_text_search(query);
        }
        return collect(phrase_scanner);
    };
    expect_ids(phrase("alpha beta"), {1, 9, 10});
    expect_ids(phrase("beta alpha"), {2});
    expect_ids(phrase("beta gamma"), {9});

    auto filtered_scanner = dataset.scan();
    {
        auto query = lance::FtsQuery::match("alpha", std::string("tags"),
                                            match_options);
        filtered_scanner.full_text_search(query);
        filtered_scanner.row_addr_allowlist(&row_id, 1);
    }
    auto filtered = collect(filtered_scanner);
    expect_ids(filtered, {1, 1});
    expect_scores_equal(alpha, filtered, 1);

    auto expect_rejection = [&dataset](lance::FtsQuery query,
                                       LanceErrorCode code,
                                       const std::string& first,
                                       const std::string& second) {
        auto rejection_scanner = dataset.scan();
        rejection_scanner.full_text_search(query);
        bool rejected = false;
        try {
            (void)collect(rejection_scanner);
        } catch (const lance::Error& error) {
            const std::string text = error.what();
            rejected = error.code == code &&
                       text.find(first) != std::string::npos &&
                       (second.empty() ||
                        text.find(second) != std::string::npos);
        }
        assert(rejected);
    };

    lance::FtsMatchOptions row_options;
    row_options.document_granularity = lance::FtsDocumentGranularity::Row;
    expect_rejection(
        lance::FtsQuery::match("alpha", std::string("tags"), row_options),
        LANCE_ERR_INVALID_ARGUMENT, "requested Row", "ListElement");
    expect_rejection(lance::FtsQuery::multi_match("alpha", {"tags"}, {}),
                     LANCE_ERR_NOT_SUPPORTED,
                     "MultiMatch does not support ListElement document "
                     "granularity",
                     "");
    expect_rejection(
        lance::FtsQuery::combined_fields("alpha", {"tags"}, {}),
        LANCE_ERR_NOT_SUPPORTED,
        "combined_fields (BM25F) scores whole rows", "");

    PASS();
}

int main(int argc, char** argv) {
    assert(argc == 4);
    const std::string uri(argv[1]);
    const std::string list_uri(argv[2]);
    const uint64_t list_row_id = std::strtoull(argv[3], nullptr, 10);
    printf("Running typed FTS C++ tests with dataset: %s\n", uri.c_str());

    auto dataset = lance::Dataset::open(uri);
    auto list_dataset = lance::Dataset::open(list_uri);
    test_match_boost_and_granularity(dataset);
    test_boolean_phrase_and_boost(dataset);
    test_multi_match_and_combined_fields(dataset);
    test_validation_and_replacement(dataset);
    test_list_element_execution(list_dataset, list_row_id);

    printf("All typed FTS C++ tests passed!\n");
    return 0;
}
