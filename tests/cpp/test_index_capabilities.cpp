/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#include "lance/lance.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

static void capture_progress(
    void* callback_ctx,
    int32_t,
    const char* stage,
    uint64_t,
    const char* unit,
    uint64_t) {
    auto* events = static_cast<std::atomic<uint64_t>*>(callback_ctx);
    assert(events != nullptr);
    assert(stage != nullptr);
    assert(unit != nullptr);
    events->fetch_add(1, std::memory_order_relaxed);
}

struct OrdinaryProgressCapture {
    std::atomic<uint64_t> events{0};
    std::atomic<int> invalid{0};
    std::atomic<int> late{0};
    std::atomic<int> closed{0};
    void* expected_ctx = nullptr;
};

static void capture_ordinary_index_progress(
    void* callback_ctx,
    int32_t event,
    const char* stage,
    uint64_t,
    const char* unit,
    uint64_t) {
    auto* capture = static_cast<OrdinaryProgressCapture*>(callback_ctx);
    if (!capture || !stage || !unit ||
        event < LANCE_INDEX_BUILD_PROGRESS_STAGE_START ||
        event > LANCE_INDEX_BUILD_PROGRESS_STAGE_COMPLETE) {
        if (capture) capture->invalid.store(1, std::memory_order_relaxed);
        return;
    }
    if (capture->expected_ctx && callback_ctx != capture->expected_ctx) {
        capture->invalid.store(1, std::memory_order_relaxed);
    }
    if (capture->closed.load(std::memory_order_relaxed)) {
        capture->late.store(1, std::memory_order_relaxed);
    }
    capture->events.fetch_add(1, std::memory_order_relaxed);
}

static int64_t count_stream(ArrowArrayStream* stream) {
    int64_t rows = 0;
    for (;;) {
        ArrowArray array = {};
        assert(stream->get_next(stream, &array) == 0);
        if (!array.release) break;
        rows += array.length;
        array.release(&array);
    }
    return rows;
}

static std::vector<int32_t> collect_scanner_ids(lance::Scanner& scanner) {
    ArrowArrayStream stream{};
    scanner.to_arrow_stream(&stream);

    ArrowSchema schema{};
    if (stream.get_schema(&stream, &schema) != 0) {
        const char* message =
            stream.get_last_error ? stream.get_last_error(&stream) : nullptr;
        std::string text = message ? message : "get_schema failed";
        if (stream.release) stream.release(&stream);
        throw lance::Error(LANCE_ERR_INTERNAL, text);
    }
    int64_t id_index = -1;
    for (int64_t child = 0; child < schema.n_children; ++child) {
        const char* name = schema.children[child]->name;
        if (name && std::strcmp(name, "id") == 0) {
            id_index = child;
            break;
        }
    }
    assert(id_index >= 0 && "id column missing from scanner output");
    if (schema.release) schema.release(&schema);

    std::vector<int32_t> ids;
    for (;;) {
        ArrowArray array{};
        if (stream.get_next(&stream, &array) != 0) {
            const char* message =
                stream.get_last_error ? stream.get_last_error(&stream) : nullptr;
            std::string text = message ? message : "get_next failed";
            if (stream.release) stream.release(&stream);
            throw lance::Error(LANCE_ERR_INTERNAL, text);
        }
        if (!array.release) break;
        const auto* values = static_cast<const int32_t*>(
            array.children[id_index]->buffers[1]);
        const int64_t offset = array.children[id_index]->offset;
        for (int64_t row = 0; row < array.length; ++row) {
            ids.push_back(values[offset + row]);
        }
        array.release(&array);
    }
    if (stream.release) stream.release(&stream);
    return ids;
}

static void assert_metadata_equal_except_fragments(
    const lance::IndexSegmentMetadata& expected,
    const lance::IndexSegmentMetadata& actual) {
    assert(expected.uuid() == actual.uuid());
    assert(expected.name() == actual.name());
    assert(expected.field_ids() == actual.field_ids());
    assert(expected.covering_field_ids() == actual.covering_field_ids());
    assert(expected.dataset_version() == actual.dataset_version());
    assert(expected.index_version() == actual.index_version());
    assert(expected.index_type() == actual.index_type());
    assert(expected.index_details_type_url() ==
           actual.index_details_type_url());
    assert(expected.index_details_value() == actual.index_details_value());
    assert(expected.files() == actual.files());
    assert(expected.base_id() == actual.base_id());
    const auto expected_created = expected.created_at();
    const auto actual_created = actual.created_at();
    assert(expected_created.has_value() == actual_created.has_value());
    if (expected_created) {
        assert(expected_created->unix_seconds == actual_created->unix_seconds);
        assert(expected_created->nanos == actual_created->nanos);
    }
}

static lance::VectorIndexOptions vector_options(uint32_t partitions) {
    lance::VectorIndexOptions options;
    options.index_type = LANCE_INDEX_IVF_FLAT;
    options.metric = LANCE_METRIC_L2;
    options.num_partitions = partitions;
    options.sample_rate = 32;
    options.max_iters = 3;
    options.index_file_version = LANCE_INDEX_FILE_VERSION_V3;
    return options;
}

static int64_t query_rows(lance::Dataset& dataset,
                          const float* query,
                          int effort_first) {
    auto scanner = dataset.scan();
    scanner.nearest("embedding", query, 8, LANCE_DTYPE_FLOAT32, 20);
    if (effort_first) {
        scanner.search_effort(1.0).nprobes(1);
    } else {
        scanner.nprobes(1).search_effort(1.0);
    }
    ArrowArrayStream stream = {};
    scanner.to_arrow_stream(&stream);
    lance::ArrowStream owned(stream);
    return count_stream(owned.get());
}

static void test_distributed_metadata_publication(const std::string& uri,
                                                  lance::Dataset& dataset) {
    const std::string fts_params =
        R"({"base_tokenizer":"simple","language":"English","with_position":true})";
    const std::array<uint8_t, 16> index_uuid = {
        0x50, 0x55, 0x42, 0x4c, 0x49, 0x53, 0x48, 0x45,
        0x44, 0x02, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde,
    };
    const auto fragments = dataset.fragment_ids();
    assert(fragments.size() >= 2 && fragments.size() <= 8);
    std::vector<uint32_t> coverage;
    coverage.reserve(fragments.size());
    for (uint64_t fragment : fragments) {
        coverage.push_back(static_cast<uint32_t>(fragment));
    }

    lance::ScalarIndexOptions train;
    train.train = true;
    std::vector<std::vector<uint8_t>> staged;
    staged.reserve(coverage.size());
    for (uint32_t fragment_id : coverage) {
        LanceIndexSegmentBuildOptions build{};
        build.fragment_ids = &fragment_id;
        build.fragment_count = 1;
        build.index_uuid = index_uuid.data();
        build.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;
        auto builder = dataset.new_scalar_index_segment_builder_with_options(
            "dist_text", LANCE_SCALAR_INVERTED, train,
            "c_dist_text_publish_cpp", fts_params, &build);
        staged.push_back(builder.execute_uncommitted());
    }

    const uint64_t version_before = dataset.version();
    std::atomic<uint64_t> merge_events{0};
    dataset.merge_index_metadata(index_uuid, "Inverted", std::nullopt,
                                 capture_progress, &merge_events);
    assert(merge_events.load(std::memory_order_relaxed) > 0);
    assert(dataset.version() == version_before);

    auto partial = lance::IndexSegmentMetadata::parse(staged[0]);
    assert(partial.fragment_ids().size() == 1);
    auto combined = partial.clone_with_fragment_ids(coverage);
    assert(combined.fragment_ids() == coverage);
    assert_metadata_equal_except_fragments(partial, combined);
    assert(partial.fragment_ids().size() == 1);

    auto unchanged = partial.clone_with_fragment_ids(std::nullopt);
    assert(unchanged.fragment_ids() == partial.fragment_ids());
    assert_metadata_equal_except_fragments(partial, unchanged);

    auto encoded = combined.encode();
    assert(!encoded.empty());
    assert(dataset.version() == version_before);

    dataset.commit_index_segments(
        "c_dist_text_published_cpp", "dist_text",
        std::vector<std::vector<uint8_t>>{encoded});
    assert(dataset.version() == version_before + 1);

    const auto description = dataset.describe_indices_json();
    assert(description.find("\"name\":\"c_dist_text_published_cpp\"") !=
           std::string::npos);
    std::string expected_coverage = "\"fragment_ids\":[";
    for (size_t i = 0; i < coverage.size(); ++i) {
        if (i > 0) expected_coverage += ",";
        expected_coverage += std::to_string(coverage[i]);
    }
    expected_coverage += "]";
    assert(description.find(expected_coverage) != std::string::npos);

    auto reopened = lance::Dataset::open(uri);
    auto scanner = reopened.scan();
    {
        auto query =
            lance::FtsQuery::phrase("alpha beta", std::string("dist_text"));
        scanner.full_text_search(query);
    }
    const auto explain = scanner.explain_plan();
    if (explain.find("PhraseQuery:") == std::string::npos) {
        std::fprintf(stderr,
                     "C++ phrase plan did not contain PhraseQuery:\n%s\n",
                     explain.c_str());
    }
    assert(explain.find("PhraseQuery:") != std::string::npos);
    auto ids = collect_scanner_ids(scanner);
    assert(ids.size() == 48);
    std::sort(ids.begin(), ids.end());
    for (size_t i = 0; i < ids.size(); ++i) {
        assert(ids[i] == static_cast<int32_t>(i * 4));
    }
    std::printf("Distributed inverted metadata publication C++ path passed\n");
}

static void test_ordinary_index_progress(const std::string& uri) {
    auto dataset = lance::Dataset::open(uri);

    lance::ScalarIndexOptions train;
    train.train = true;
    uint64_t version = dataset.version();
    OrdinaryProgressCapture scalar_capture;
    scalar_capture.expected_ctx = &scalar_capture;
    dataset.create_scalar_index_with_options_and_progress(
        "id", LANCE_SCALAR_BTREE, train, capture_ordinary_index_progress,
        &scalar_capture, "cpp_btree_progress");
    assert(dataset.version() == version + 1);
    assert(scalar_capture.events.load(std::memory_order_relaxed) > 0);
    assert(scalar_capture.invalid.load(std::memory_order_relaxed) == 0);
    assert(scalar_capture.late.load(std::memory_order_relaxed) == 0);
    scalar_capture.closed.store(1, std::memory_order_relaxed);

    version = dataset.version();
    OrdinaryProgressCapture scalar_replacement;
    scalar_replacement.expected_ctx = &scalar_replacement;
    dataset.create_scalar_index_with_options_and_progress(
        "id", LANCE_SCALAR_BTREE, train, capture_ordinary_index_progress,
        &scalar_replacement, "cpp_btree_progress", "", true);
    assert(dataset.version() == version + 1);
    assert(scalar_replacement.events.load(std::memory_order_relaxed) > 0);
    assert(scalar_replacement.invalid.load(std::memory_order_relaxed) == 0);
    assert(scalar_replacement.late.load(std::memory_order_relaxed) == 0);
    scalar_replacement.closed.store(1, std::memory_order_relaxed);

    version = dataset.version();
    dataset.create_scalar_index_with_progress(
        "id", LANCE_SCALAR_BTREE, nullptr,
        reinterpret_cast<void*>(static_cast<uintptr_t>(1)),
        "cpp_btree_progress", "", true);
    assert(dataset.version() == version + 1);

    lance::VectorIndexOptions vector = vector_options(2);
    version = dataset.version();
    OrdinaryProgressCapture vector_capture;
    vector_capture.expected_ctx = &vector_capture;
    dataset.create_vector_index_with_options_and_progress(
        "embedding", vector, capture_ordinary_index_progress, &vector_capture,
        "cpp_ivf_progress");
    assert(dataset.version() == version + 1);
    assert(vector_capture.events.load(std::memory_order_relaxed) > 0);
    assert(vector_capture.invalid.load(std::memory_order_relaxed) == 0);
    assert(vector_capture.late.load(std::memory_order_relaxed) == 0);
    vector_capture.closed.store(1, std::memory_order_relaxed);

    version = dataset.version();
    OrdinaryProgressCapture vector_replacement;
    vector_replacement.expected_ctx = &vector_replacement;
    dataset.create_vector_index_with_options_and_progress(
        "embedding", vector, capture_ordinary_index_progress,
        &vector_replacement, "cpp_ivf_progress", true);
    assert(dataset.version() == version + 1);
    assert(vector_replacement.events.load(std::memory_order_relaxed) > 0);
    assert(vector_replacement.invalid.load(std::memory_order_relaxed) == 0);
    assert(vector_replacement.late.load(std::memory_order_relaxed) == 0);
    vector_replacement.closed.store(1, std::memory_order_relaxed);

    LanceVectorIndexParams legacy = {
        LANCE_INDEX_IVF_FLAT, LANCE_METRIC_L2, 2, 0, 0, 10, 0, 0, 128,
    };
    version = dataset.version();
    dataset.create_vector_index_with_progress(
        "embedding", legacy, nullptr,
        reinterpret_cast<void*>(static_cast<uintptr_t>(1)),
        "cpp_ivf_progress", true);
    assert(dataset.version() == version + 1);

    version = dataset.version();
    OrdinaryProgressCapture failed_capture;
    failed_capture.expected_ctx = &failed_capture;
    bool caught = false;
    try {
        dataset.create_vector_index_with_options_and_progress(
            "no_such_vector_column", vector, capture_ordinary_index_progress,
            &failed_capture, "cpp_ivf_progress_failure");
    } catch (const lance::Error&) {
        caught = true;
    }
    assert(caught);
    assert(dataset.version() == version);
    failed_capture.closed.store(1, std::memory_order_relaxed);
    assert(failed_capture.late.load(std::memory_order_relaxed) == 0);

    dataset = lance::Dataset::open(uri);
    auto scalar_scanner = dataset.scan(
        std::vector<std::string>{"id"}, std::string("id = 20"));
    assert(collect_scanner_ids(scalar_scanner) == std::vector<int32_t>{20});
    const float query[8] = {20.25f, 20.375f, 20.5f, 20.625f,
                            20.75f, 20.875f, 21.0f, 21.125f};
    auto vector_scanner = dataset.scan();
    vector_scanner
        .nearest("embedding", query, 8, LANCE_DTYPE_FLOAT32, 3)
        .nprobes(2);
    assert(collect_scanner_ids(vector_scanner) ==
           std::vector<int32_t>({20, 21, 19}));
    dataset.drop_index("cpp_btree_progress");
    dataset.drop_index("cpp_ivf_progress");
    std::printf("Ordinary index progress C++ path passed\n");
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <dataset_uri>\n", argv[0]);
        return 1;
    }
    const std::string uri(argv[1]);

    test_ordinary_index_progress(uri);
    auto dataset = lance::Dataset::open(uri);
    assert(dataset.describe_indices_json() == "[]");

    lance::ScalarIndexOptions train;
    train.train = true;
    dataset.create_scalar_index_with_options(
        "id", LANCE_SCALAR_BTREE, train, "c_btree_cpp");
    lance::ScalarIndexOptions definition_only;
    definition_only.train = false;
    dataset.create_scalar_index_with_options(
        "id", LANCE_SCALAR_BTREE, definition_only, "c_btree_defined_cpp");

    const std::string fts_params =
        R"({"base_tokenizer":"simple","language":"English","with_position":true})";
    dataset.create_scalar_index_with_options(
        "text", LANCE_SCALAR_INVERTED, train, "c_fts_cpp", fts_params);
    dataset.create_vector_index_with_options(
        "embedding", vector_options(4), "c_ivf_cpp");

    const auto description = dataset.describe_indices_json();
    assert(description.find("\"name\":\"c_btree_cpp\"") != std::string::npos);
    assert(description.find("\"name\":\"c_fts_cpp\"") != std::string::npos);
    assert(description.find("\"name\":\"c_ivf_cpp\"") != std::string::npos);
    const auto statistics = dataset.index_statistics_json("c_ivf_cpp");
    assert(statistics.find("\"num_partitions\"") != std::string::npos);
    bool caught = false;
    try {
        (void)dataset.index_statistics_json("missing_idx");
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_NOT_FOUND);
    }
    assert(caught);

    lance::IndexTrainerOptions trainer;
    trainer.sample_rate = 32;
    trainer.max_iters = 3;
    auto centroids =
        dataset.train_ivf_model_with_options("embedding", 2, LANCE_METRIC_L2,
                                             trainer);
    auto codebook = dataset.train_pq_model_with_options(
        "embedding", 2, 4, LANCE_METRIC_L2, centroids, trainer);
    assert(codebook.array()->release != nullptr);

    auto model = dataset.get_ivf_model("c_ivf_cpp");
    assert(model.has_centroids());
    assert(model.num_partitions() == 4);
    assert(model.dimension() == 8);
    assert(model.partition_size(99) == 0);
    for (uint64_t partition = 0; partition < 4; ++partition) {
        (void)model.partition_size(partition);
        auto stream = dataset.read_index_partition(
            "c_ivf_cpp", partition, true);
        assert(count_stream(stream.get()) >= 0);
    }
    auto exported_centroids = model.centroids();
    assert(exported_centroids.has_value());
    assert(exported_centroids->array()->release != nullptr);

    lance::IndexPrewarmOptions prewarm;
    prewarm.with_position = true;
    prewarm.index_segments = std::vector<std::array<uint8_t, 16>>{};
    dataset.prewarm_index_with_options("c_fts_cpp", prewarm);
    auto fts_segments = dataset.index_segments("c_fts_cpp");
    assert(!fts_segments.empty());
    prewarm.index_segments = fts_segments;
    dataset.prewarm_index_with_options("c_fts_cpp", prewarm);
    std::array<uint8_t, 16> missing_uuid{};
    missing_uuid.fill(0xa5);
    caught = false;
    try {
        prewarm.index_segments =
            std::vector<std::array<uint8_t, 16>>{missing_uuid};
        dataset.prewarm_index_with_options("c_fts_cpp", prewarm);
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_NOT_FOUND);
    }
    assert(caught);

    const std::array<uint8_t, 16> merge_uuid = {
        0x43, 0x4f, 0x4d, 0x42, 0x49, 0x4e, 0x45, 0x44,
        0x8f, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd,
    };
    auto fragment_ids = dataset.fragment_ids();
    assert(fragment_ids.size() >= 2);
    std::vector<std::vector<uint8_t>> staged;
    for (size_t i = 0; i < 2; ++i) {
        const uint32_t fragment_id = static_cast<uint32_t>(fragment_ids[i]);
        LanceIndexSegmentBuildOptions build{};
        build.fragment_ids = &fragment_id;
        build.fragment_count = 1;
        build.index_uuid = merge_uuid.data();
        build.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;
        auto builder = dataset.new_scalar_index_segment_builder_with_options(
            "text", LANCE_SCALAR_INVERTED, train, "c_dist_fts_cpp",
            fts_params, &build);
        staged.push_back(builder.execute_uncommitted());
    }
    const uint64_t version_before = dataset.version();
    std::atomic<uint64_t> progress_events{0};
    dataset.merge_index_metadata(merge_uuid, "Inverted", std::nullopt,
                                 capture_progress, &progress_events);
    assert(dataset.version() == version_before);
    std::vector<std::vector<uint8_t>> physical;
    for (size_t i = 0; i < 2; ++i) {
        const uint32_t fragment_id = static_cast<uint32_t>(fragment_ids[i]);
        auto physical_uuid = merge_uuid;
        physical_uuid[15] = static_cast<uint8_t>(i + 1);
        LanceIndexSegmentBuildOptions build{};
        build.fragment_ids = &fragment_id;
        build.fragment_count = 1;
        build.index_uuid = physical_uuid.data();
        build.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;
        auto builder = dataset.new_scalar_index_segment_builder_with_options(
            "text", LANCE_SCALAR_INVERTED, train, "c_dist_fts_physical_cpp",
            fts_params, &build);
        physical.push_back(builder.execute_uncommitted());
    }
    auto merged = dataset.merge_index_segments(physical);
    assert(dataset.version() == version_before);
    assert(!merged.empty());
    dataset.commit_index_segments("c_fts_merged_cpp", "text",
                                  std::vector<std::vector<uint8_t>>{merged});
    assert(dataset.version() == version_before + 1);

    test_distributed_metadata_publication(uri, dataset);

    const float query[8] = {20.0f, 20.1f, 20.2f, 20.3f,
                            20.4f, 20.5f, 20.6f, 20.7f};
    const int64_t fixed_rows = query_rows(dataset, query, false);
    const int64_t effort_first = query_rows(dataset, query, true);
    assert(fixed_rows > 0);
    assert(effort_first > 0);

    auto invalid_scanner = dataset.scan();
    invalid_scanner.nearest("embedding", query, 8, LANCE_DTYPE_FLOAT32, 10);
    caught = false;
    try {
        invalid_scanner.search_effort(std::numeric_limits<double>::quiet_NaN());
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught);

    caught = false;
    try {
        auto no_nearest = dataset.scan();
        no_nearest.search_effort(0.5);
        ArrowArrayStream stream = {};
        no_nearest.to_arrow_stream(&stream);
    } catch (const lance::Error& error) {
        caught = true;
        assert(error.code == LANCE_ERR_INVALID_ARGUMENT);
    }
    assert(caught);

    std::optional<lance::IvfModel> held_model;
    std::optional<lance::ArrowStream> held_stream;
    {
        auto lifetime_dataset = lance::Dataset::open(uri);
        held_model.emplace(
            lifetime_dataset.get_ivf_model("c_ivf_cpp"));
        held_stream.emplace(
            lifetime_dataset.read_index_partition("c_ivf_cpp", 0, true));
    }
    assert(held_model->num_partitions() == 4);
    assert(count_stream(held_stream->get()) > 0);
    auto moved_model = std::move(*held_model);
    assert(moved_model.num_partitions() == 4);

    std::printf("Native index capability C++ tests passed\n");
    return 0;
}
