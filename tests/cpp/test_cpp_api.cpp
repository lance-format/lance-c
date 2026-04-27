/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file test_cpp_api.cpp
 * @brief C++ compilation and functional test for lance.hpp
 *
 * Tests the RAII wrappers, exception handling, and builder pattern.
 *
 * Usage: test_cpp_api <dataset_uri> <write_uri>
 */

#include "lance.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#define TEST(name) printf("  %s... ", #name)
#define PASS()     printf("OK\n")

static void test_dataset_open(const std::string& uri) {
    TEST(test_dataset_open);

    auto ds = lance::Dataset::open(uri);
    assert(ds.version() >= 1);
    assert(ds.count_rows() > 0);

    printf("version=%llu, rows=%llu... ",
           (unsigned long long)ds.version(),
           (unsigned long long)ds.count_rows());

    PASS();
}

static void test_dataset_schema(const std::string& uri) {
    TEST(test_dataset_schema);

    auto ds = lance::Dataset::open(uri);

    ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ds.schema(&schema);

    assert(schema.n_children > 0);
    printf("fields=%lld... ", (long long)schema.n_children);

    // Print field names
    for (int64_t i = 0; i < schema.n_children; i++) {
        if (i > 0) printf(", ");
        printf("%s", schema.children[i]->name);
    }
    printf("... ");

    if (schema.release) schema.release(&schema);

    PASS();
}

static void test_scanner_fluent(const std::string& uri) {
    TEST(test_scanner_fluent);

    auto ds = lance::Dataset::open(uri);

    // Fluent builder pattern.
    auto scanner = ds.scan();
    scanner.limit(5).offset(0).batch_size(2);

    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);

    // Count rows from stream.
    uint64_t total = 0;
    while (true) {
        ArrowArray arr;
        memset(&arr, 0, sizeof(arr));
        int rc = stream.get_next(&stream, &arr);
        assert(rc == 0);
        if (!arr.release) break;
        total += (uint64_t)arr.length;
        arr.release(&arr);
    }

    assert(total == 5);
    printf("rows=%llu... ", (unsigned long long)total);

    if (stream.release) stream.release(&stream);
    PASS();
}

static void test_dataset_take(const std::string& uri) {
    TEST(test_dataset_take);

    auto ds = lance::Dataset::open(uri);

    uint64_t indices[] = {0, 1, 2};
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ds.take(indices, 3, &stream);

    uint64_t total = 0;
    while (true) {
        ArrowArray arr;
        memset(&arr, 0, sizeof(arr));
        int rc = stream.get_next(&stream, &arr);
        assert(rc == 0);
        if (!arr.release) break;
        total += (uint64_t)arr.length;
        arr.release(&arr);
    }

    assert(total == 3);
    printf("rows=%llu... ", (unsigned long long)total);

    if (stream.release) stream.release(&stream);
    PASS();
}

static void test_raii_cleanup(const std::string& uri) {
    TEST(test_raii_cleanup);

    // Dataset and Scanner should clean up automatically.
    {
        auto ds = lance::Dataset::open(uri);
        auto scanner = ds.scan();
        scanner.limit(1);
        // Goes out of scope — RAII cleanup.
    }

    // Move semantics.
    {
        auto ds1 = lance::Dataset::open(uri);
        auto ds2 = std::move(ds1);
        assert(ds2.count_rows() > 0);
    }

    PASS();
}

static void test_versions(const std::string& uri) {
    TEST(test_versions);

    auto ds = lance::Dataset::open(uri);
    auto versions = ds.versions();

    assert(!versions.empty());
    for (const auto& v : versions) {
        assert(v.id >= 1);
        assert(v.timestamp_ms > 0);
    }
    printf("count=%zu... ", versions.size());

    PASS();
}

// Restore to the dataset's own current version — always commits a new
// manifest (no skip-if-equal optimization) to defeat TOCTOU races against
// concurrent writers.
static void test_restore_to_current(const std::string& uri) {
    TEST(test_restore_to_current);

    auto ds = lance::Dataset::open(uri);
    uint64_t current = ds.version();

    auto after = ds.restore(current);
    assert(after.version() == current + 1);

    PASS();
}

static void test_error_exception(const std::string& /*uri*/) {
    TEST(test_error_exception);

    bool caught = false;
    try {
        lance::Dataset::open("file:///nonexistent/path/xyz");
    } catch (const lance::Error& e) {
        caught = true;
        assert(e.code != LANCE_OK);
        assert(strlen(e.what()) > 0);
        printf("caught: %s... ", e.what());
    }
    assert(caught);

    PASS();
}

static void test_index_lifecycle(const std::string& uri) {
    TEST(test_index_lifecycle);

    auto ds = lance::Dataset::open(uri);
    ds.create_scalar_index("id", LANCE_SCALAR_BTREE, "id_idx");
    assert(ds.index_count() == 1);

    auto json = ds.list_indices_json();
    assert(json.find("id_idx") != std::string::npos);
    printf("listed: %s... ", json.c_str());

    ds.drop_index("id_idx");
    assert(ds.index_count() == 0);

    PASS();
}

static void test_nearest_smoke(const std::string& uri) {
    TEST(test_nearest_smoke);

    auto ds = lance::Dataset::open(uri);
    auto scanner = ds.scan();
    float q[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};

    // The test dataset doesn't have a vector column; calling nearest will
    // either succeed (if "name" or "id" happens to work — won't) or throw.
    // We just exercise the wrapper code paths, expecting either outcome
    // gracefully. Compile/link is the main goal here.
    bool caught = false;
    try {
        scanner.nearest("embedding", q, 8, 5)
               .nprobes(2)
               .refine_factor(1)
               .ef(50)
               .metric(LANCE_METRIC_L2)
               .use_index(true)
               .prefilter(false);
        // Try to materialize — will throw because "embedding" column doesn't exist
        // in the basic test fixture.
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);
        if (stream.release) stream.release(&stream);
    } catch (const lance::Error&) {
        caught = true;
    }
    // Either path is fine — we proved compile + linkage + the fluent chain.
    (void)caught;

    PASS();
}

static void test_fts_smoke(const std::string& uri) {
    TEST(test_fts_smoke);

    auto ds = lance::Dataset::open(uri);

    // Build the inverted index needed for FTS. (Inverted requires non-NULL
    // params JSON for the tokenizer config.)
    bool index_built = false;
    try {
        ds.create_scalar_index(
            "name", LANCE_SCALAR_INVERTED, "name_fts",
            R"({"base_tokenizer":"simple","language":"English"})");
        index_built = true;
    } catch (const lance::Error&) {
        // If the test fixture doesn't permit indexing for some reason,
        // we still want to prove the wrappers compile + link.
    }

    auto scanner = ds.scan();
    bool caught = false;
    try {
        scanner.full_text_search("alice", {"name"}, 0);
        ArrowArrayStream stream;
        memset(&stream, 0, sizeof(stream));
        scanner.to_arrow_stream(&stream);
        if (stream.release) stream.release(&stream);
    } catch (const lance::Error&) {
        caught = true;
    }
    // Either path is acceptable — the goal is compile + linkage.
    (void)index_built;
    (void)caught;

    PASS();
}

// Round-trip: scan src dataset to an ArrowArrayStream, write it to a new
// dataset via lance::Dataset::write, and verify row counts match.
// dst_uri must not pre-exist.
static void test_dataset_write_roundtrip(const std::string& src_uri,
                                         const std::string& dst_uri) {
    TEST(test_dataset_write_roundtrip);

    auto src = lance::Dataset::open(src_uri);
    uint64_t src_rows = src.count_rows();

    auto scanner = src.scan();
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);

    auto dst = lance::Dataset::write(
        dst_uri, &stream, lance::WriteMode::Create);

    uint64_t dst_rows = dst.count_rows();
    assert(dst_rows == src_rows);
    printf("src=%llu, dst=%llu... ",
           (unsigned long long)src_rows, (unsigned long long)dst_rows);

    PASS();
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <dataset_uri> <write_uri>\n", argv[0]);
        return 1;
    }

    std::string uri(argv[1]);
    std::string write_uri(argv[2]);
    printf("Running C++ API tests with dataset: %s\n", uri.c_str());

    test_dataset_open(uri);
    test_dataset_schema(uri);
    test_scanner_fluent(uri);
    test_dataset_take(uri);
    test_raii_cleanup(uri);
    test_versions(uri);
    test_restore_to_current(uri);
    test_error_exception(uri);
    test_index_lifecycle(uri);
    test_nearest_smoke(uri);
    test_fts_smoke(uri);
    test_dataset_write_roundtrip(uri, write_uri);

    printf("All C++ tests passed!\n");
    return 0;
}
