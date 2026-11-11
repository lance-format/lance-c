/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#include "lance/lance.hpp"

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

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <dataset_uri>\n", argv[0]);
        return 1;
    }
    const std::string uri(argv[1]);

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
