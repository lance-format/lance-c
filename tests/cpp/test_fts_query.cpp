/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file test_fts_query.cpp
 * @brief Functional C++ coverage for typed full-text query trees.
 *
 * The Rust harness creates a two-fragment dataset with two inverted-indexed
 * text columns and passes its URI as argv[1].
 */

#include "lance/lance.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
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
        auto should = lance::FtsQuery::match("beta", std::string("title"));
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
            text.find("boolean query must have at least one should/must query") !=
                std::string::npos;
    }
    assert(empty_rejected);

    PASS();
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string uri(argv[1]);
    printf("Running typed FTS C++ tests with dataset: %s\n", uri.c_str());

    auto dataset = lance::Dataset::open(uri);
    test_match_boost_and_granularity(dataset);
    test_boolean_phrase_and_boost(dataset);
    test_multi_match_and_combined_fields(dataset);
    test_validation_and_replacement(dataset);

    printf("All typed FTS C++ tests passed!\n");
    return 0;
}
