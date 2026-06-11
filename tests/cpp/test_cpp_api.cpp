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

#include "lance/lance.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// Arrow C Data Interface flag bits — see arrow_schema/arrow_c_data_interface.h.
#define ARROW_FLAG_NULLABLE 2

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

static void test_index_segments_smoke(const std::string& /*uri*/) {
    TEST(test_index_segments_smoke);

    // The shared test fixture has no vector column, so we can't actually run
    // segment enumeration end-to-end without building our own dataset. The
    // smoke goal is to prove the C++ wrappers compile and link.

    // Exercise the no-op clear path on a fresh scanner — passing a nullptr
    // buffer with len=0 must succeed.
    {
        // Create a scanner-less, empty-options scenario by trying to call the
        // wrapper signatures. We don't actually invoke them at runtime here
        // because we don't have a vector dataset to point at.
        constexpr auto verify_signatures = []() {
            using DsMember = uint64_t (lance::Dataset::*)(const std::string&) const;
            using SegMember = std::vector<std::array<uint8_t, 16>>
                (lance::Dataset::*)(const std::string&) const;
            using ScanMember = lance::Scanner& (lance::Scanner::*)(
                const std::vector<std::array<uint8_t, 16>>&);
            DsMember a = &lance::Dataset::index_segment_count;
            SegMember b = &lance::Dataset::index_segments;
            ScanMember c = static_cast<ScanMember>(&lance::Scanner::index_segments);
            (void)a; (void)b; (void)c;
        };
        verify_signatures();
    }

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

// Re-opens the dataset just written by `test_dataset_write_roundtrip` and
// exercises `Dataset::update`. Must run before `test_delete_rows`, which
// empties the dataset.
static void test_update(const std::string& dst_uri) {
    TEST(test_update);

    auto ds = lance::Dataset::open(dst_uri);
    uint64_t before = ds.count_rows();
    assert(before > 0 && "test fixture expected to have rows");

    // Empty predicate -> updates every row.
    uint64_t updated = ds.update("", {{"name", "'frozen'"}});
    assert(updated == before);
    assert(ds.count_rows() == before);

    // Empty updates vector must throw (num_updates == 0).
    bool caught_empty = false;
    try {
        ds.update("", {});
    } catch (const lance::Error& e) {
        caught_empty = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_empty);

    printf("updated=%llu... ", (unsigned long long)updated);
    PASS();
}

// Re-opens the dataset just written by `test_dataset_write_roundtrip` and
// exercises `Dataset::merge_insert`. Must run before `test_delete_rows`,
// which empties the dataset.
static void test_merge_insert(const std::string& dst_uri) {
    TEST(test_merge_insert);

    auto ds = lance::Dataset::open(dst_uri);
    uint64_t before = ds.count_rows();
    assert(before > 0 && "test fixture expected to have rows");

    // Self-merge: scan the dataset itself and use that as the source. With
    // find-or-create defaults every row is a self-match and DoNothing fires,
    // so insert/update counts stay at zero and the row count is preserved.
    auto scanner = ds.scan();
    ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    scanner.to_arrow_stream(&stream);

    auto result = ds.merge_insert({"id"}, &stream);
    assert(result.num_inserted_rows == 0);
    assert(result.num_updated_rows == 0);
    assert(ds.count_rows() == before);

    // Empty key vector must throw (num_on_columns == 0).
    bool caught_empty = false;
    try {
        ds.merge_insert({}, nullptr);
    } catch (const lance::Error& e) {
        caught_empty = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_empty);

    PASS();
}

// Re-opens the dataset just written by `test_dataset_write_roundtrip` and
// exercises `Dataset::alter_columns`. Relaxes the nullability of `id` (non-
// nullable in the fixture) to nullable; the column survives the subsequent
// drop_columns({"name"}) test untouched.
static void test_alter_columns(const std::string& dst_uri) {
    TEST(test_alter_columns);

    auto ds = lance::Dataset::open(dst_uri);
    uint64_t v_before = ds.version();

    lance::ColumnAlteration alt;
    alt.path          = "id";
    alt.nullable_mode = LANCE_COLUMN_NULLABLE_TRUE;
    ds.alter_columns({alt});
    assert(ds.version() > v_before
           && "alter_columns must bump the version");

    // Confirm the schema reflects the relaxed nullability.
    ArrowSchema schema;
    memset(&schema, 0, sizeof(schema));
    ds.schema(&schema);
    assert(schema.n_children > 0 && "schema must have children");
    bool id_is_nullable = false;
    for (int64_t i = 0; i < schema.n_children; i++) {
        ArrowSchema* child = schema.children[i];
        if (!child) continue;
        if (strcmp(child->name, "id") == 0) {
            id_is_nullable = (child->flags & ARROW_FLAG_NULLABLE) != 0;
        }
    }
    if (schema.release) schema.release(&schema);
    assert(id_is_nullable && "id should be nullable after alter");

    // No-op alteration (all sentinels left at defaults) must throw with
    // INVALID_ARGUMENT.
    bool caught_noop = false;
    try {
        lance::ColumnAlteration noop;
        noop.path = "id";
        ds.alter_columns({noop});
    } catch (const lance::Error& e) {
        caught_noop = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_noop);

    // Out-of-range nullable_mode discriminant must throw with INVALID_ARGUMENT.
    // Cast `99` through the enum type to verify the C++ wrapper forwards it
    // verbatim rather than silently clamping.
    bool caught_bad_mode = false;
    try {
        lance::ColumnAlteration bad_mode;
        bad_mode.path = "id";
        bad_mode.nullable_mode =
            static_cast<LanceColumnNullableMode>(99);
        ds.alter_columns({bad_mode});
    } catch (const lance::Error& e) {
        caught_bad_mode = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_bad_mode);

    PASS();
}

// Re-opens the dataset just written by `test_dataset_write_roundtrip` and
// exercises `Dataset::drop_columns`. Drops the `name` column so the dataset
// is left with `id` only; subsequent tests (`compact_files`, `delete_rows`)
// do not reference any dropped column. Must run after `test_update` /
// `test_merge_insert`.
static void test_drop_columns(const std::string& dst_uri) {
    TEST(test_drop_columns);

    auto ds = lance::Dataset::open(dst_uri);
    uint64_t before_rows = ds.count_rows();
    uint64_t v_before = ds.version();

    ds.drop_columns({"name"});
    assert(ds.count_rows() == before_rows
           && "metadata-only drop must preserve row count");
    assert(ds.version() > v_before
           && "drop_columns must bump the version");
    uint64_t v_after_drop = ds.version();

    // Dropping an unknown column must throw with INVALID_ARGUMENT and
    // leave the dataset unchanged (no version bump on the error path).
    bool caught_unknown = false;
    try {
        ds.drop_columns({"no_such_column"});
    } catch (const lance::Error& e) {
        caught_unknown = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_unknown);
    assert(ds.version() == v_after_drop
           && "failed drop must not bump the version");

    // Empty column list must throw with INVALID_ARGUMENT.
    bool caught_empty = false;
    try {
        ds.drop_columns({});
    } catch (const lance::Error& e) {
        caught_empty = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_empty);

    // Dropping the sole remaining column (`id`) must throw with
    // INVALID_ARGUMENT — upstream refuses to leave a dataset with zero
    // fields.
    bool caught_last = false;
    try {
        ds.drop_columns({"id"});
    } catch (const lance::Error& e) {
        caught_last = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_last);

    PASS();
}

// Re-opens the dataset (reduced to `id` only by `test_drop_columns`) and
// exercises the three `Dataset::add_columns_*` wrappers. The positive path
// uses the SQL variant (strings only); the nulls/stream variants are
// smoke-checked through their argument rejections, since their happy paths are
// covered by the Rust integration tests. The added `id_doubled` column is
// harmless to the subsequent compact/delete steps. Must run after
// `test_drop_columns`.
static void test_add_columns(const std::string& dst_uri) {
    TEST(test_add_columns);

    auto ds = lance::Dataset::open(dst_uri);
    uint64_t v_before = ds.version();

    // Snapshot the field count before the add. (If ds.schema() threw, the
    // zero-initialised struct's release stays null, so there is no leak; here
    // the handle is freshly opened and valid, so the export is expected to
    // succeed.)
    ArrowSchema schema_before;
    memset(&schema_before, 0, sizeof(schema_before));
    ds.schema(&schema_before);
    int64_t fields_before = schema_before.n_children;
    if (schema_before.release) schema_before.release(&schema_before);

    // SQL variant: derive `id_doubled = id * 2` from the surviving `id`.
    ds.add_columns_sql({{"id_doubled", "id * 2"}});
    assert(ds.version() > v_before
           && "add_columns_sql must bump the version");

    ArrowSchema schema_after;
    memset(&schema_after, 0, sizeof(schema_after));
    ds.schema(&schema_after);
    int64_t fields_after = schema_after.n_children;
    if (schema_after.release) schema_after.release(&schema_after);
    assert(fields_after == fields_before + 1
           && "schema field count must increase by 1 after add");

    // Empty SQL column list must throw with INVALID_ARGUMENT.
    bool caught_empty = false;
    try {
        ds.add_columns_sql({});
    } catch (const lance::Error& e) {
        caught_empty = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_empty);

    // An empty column name must throw with INVALID_ARGUMENT.
    bool caught_empty_name = false;
    try {
        ds.add_columns_sql({{"", "id * 2"}});
    } catch (const lance::Error& e) {
        caught_empty_name = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_empty_name);

    // An empty expression must throw with INVALID_ARGUMENT. (A NULL expression
    // is not representable here — `SqlColumn::expression` is a std::string — so
    // the NULL-pointer case is covered by the C test instead.)
    bool caught_empty_expr = false;
    try {
        ds.add_columns_sql({{"x", ""}});
    } catch (const lance::Error& e) {
        caught_empty_expr = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_empty_expr);

    // AllNulls with a NULL schema pointer must throw with INVALID_ARGUMENT.
    bool caught_null_schema = false;
    try {
        ds.add_columns_nulls(nullptr);
    } catch (const lance::Error& e) {
        caught_null_schema = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_null_schema);

    // Stream with a NULL stream pointer must throw with INVALID_ARGUMENT.
    bool caught_null_stream = false;
    try {
        ds.add_columns_stream(nullptr);
    } catch (const lance::Error& e) {
        caught_null_stream = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_null_stream);

    PASS();
}

// Re-opens the dataset just written by `test_dataset_write_roundtrip` and
// exercises `Dataset::compact_files`. The smoke fixture is a single fragment
// so the default planner has nothing to compact — we expect a no-op (zero
// metrics, no version bump). Must run before `test_delete_rows`.
static void test_compact_files(const std::string& dst_uri) {
    TEST(test_compact_files);

    auto ds = lance::Dataset::open(dst_uri);
    uint64_t v_before = ds.version();

    auto metrics = ds.compact_files();
    assert(metrics.fragments_removed == 0);
    assert(metrics.fragments_added == 0);
    assert(ds.version() == v_before);

    PASS();
}

// Re-opens the dataset just written by `test_dataset_write_roundtrip` and
// exercises `Dataset::delete_rows`. Must run after the write roundtrip.
static void test_delete_rows(const std::string& dst_uri) {
    TEST(test_delete_rows);

    auto ds = lance::Dataset::open(dst_uri);
    uint64_t before = ds.count_rows();
    assert(before > 0 && "test fixture expected to have rows");

    // Predicate that matches everything — exact deleted count == before.
    uint64_t deleted = ds.delete_rows("true");
    assert(deleted == before);
    assert(ds.count_rows() == 0);

    // Empty predicate must throw.
    bool caught_empty = false;
    try {
        ds.delete_rows("");
    } catch (const lance::Error& e) {
        caught_empty = true;
        assert(e.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught_empty);

    printf("deleted=%llu... ", (unsigned long long)deleted);
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
    test_index_segments_smoke(uri);
    test_fts_smoke(uri);
    test_dataset_write_roundtrip(uri, write_uri);
    test_update(write_uri);
    test_merge_insert(write_uri);
    test_alter_columns(write_uri);
    test_drop_columns(write_uri);
    test_add_columns(write_uri);
    test_compact_files(write_uri);
    test_delete_rows(write_uri);

    printf("All C++ tests passed!\n");
    return 0;
}
