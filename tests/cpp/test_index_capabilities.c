/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#include "lance/lance.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(cond, msg)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);            \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define CHECK_OK()                                                             \
    do {                                                                       \
        if (lance_last_error_code() != LANCE_OK) {                             \
            const char *msg = lance_last_error_message();                      \
            fprintf(stderr, "FAIL: lance error: %s (line %d)\n",              \
                    msg ? msg : "unknown", __LINE__);                          \
            if (msg) lance_free_string(msg);                                   \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

typedef struct {
    _Atomic uint64_t events;
    _Atomic int invalid;
} ProgressCapture;

static void capture_progress(
    void *callback_ctx,
    int32_t event,
    const char *stage,
    uint64_t total,
    const char *unit,
    uint64_t completed) {
    (void)total;
    (void)completed;
    ProgressCapture *capture = (ProgressCapture *)callback_ctx;
    if (capture == NULL || stage == NULL || unit == NULL ||
        event < LANCE_INDEX_BUILD_PROGRESS_STAGE_START ||
        event > LANCE_INDEX_BUILD_PROGRESS_STAGE_COMPLETE) {
        if (capture != NULL) atomic_store(&capture->invalid, 1);
        return;
    }
    atomic_fetch_add(&capture->events, 1);
}

static void release_arrow(struct ArrowArray *array, struct ArrowSchema *schema) {
    if (array != NULL && array->release != NULL) array->release(array);
    if (schema != NULL && schema->release != NULL) schema->release(schema);
}

static int64_t count_stream_rows(struct ArrowArrayStream *stream) {
    int64_t rows = 0;
    for (;;) {
        struct ArrowArray array = {0};
        ASSERT(stream->get_next(stream, &array) == 0,
               "ArrowArrayStream get_next failed");
        if (array.release == NULL) break;
        rows += array.length;
        array.release(&array);
    }
    stream->release(stream);
    return rows;
}

static LanceVectorIndexOptions vector_options(uint32_t partitions) {
    LanceVectorIndexOptions options;
    memset(&options, 0, sizeof(options));
    options.index_type = LANCE_INDEX_IVF_FLAT;
    options.metric = LANCE_METRIC_L2;
    options.num_partitions.value = partitions;
    options.num_partitions.is_set = true;
    options.sample_rate.value = 32;
    options.sample_rate.is_set = true;
    options.max_iters.value = 3;
    options.max_iters.is_set = true;
    options.index_file_version = LANCE_INDEX_FILE_VERSION_V3;
    return options;
}

static int64_t query_vector_rows(
    LanceDataset *dataset,
    const float *query,
    uint32_t k,
    int use_fixed_nprobes,
    uint32_t nprobes,
    int use_effort,
    double effort,
    int effort_first) {
    LanceScanner *scanner =
        lance_scanner_new(dataset, NULL, NULL);
    ASSERT(scanner != NULL, "scanner creation failed");
    CHECK_OK();
    ASSERT(lance_scanner_nearest(scanner, "embedding", query, 8,
                                 LANCE_DTYPE_FLOAT32, k) == 0,
           "nearest failed");
    if (effort_first && use_effort) {
        ASSERT(lance_scanner_set_search_effort(scanner, effort) == 0,
               "search_effort failed");
    }
    if (use_fixed_nprobes) {
        ASSERT(lance_scanner_set_nprobes(scanner, nprobes) == 0,
               "nprobes failed");
    }
    if (!effort_first && use_effort) {
        ASSERT(lance_scanner_set_search_effort(scanner, effort) == 0,
               "search_effort failed");
    }
    struct ArrowArrayStream stream = {0};
    ASSERT(lance_scanner_to_arrow_stream(scanner, &stream) == 0,
           "query materialization failed");
    CHECK_OK();
    int64_t rows = count_stream_rows(&stream);
    lance_scanner_close(scanner);
    return rows;
}

static void test_trainers(LanceDataset *dataset) {
    LanceIndexTrainerOptions trainer;
    memset(&trainer, 0, sizeof(trainer));
    trainer.sample_rate.value = 32;
    trainer.sample_rate.is_set = true;
    trainer.max_iters.value = 3;
    trainer.max_iters.is_set = true;

    struct ArrowArray centroids = {0};
    struct ArrowSchema centroids_schema = {0};
    ASSERT(lance_index_train_ivf_model_with_options(
               dataset, "embedding", 2, LANCE_METRIC_L2, NULL, 0, &trainer,
               &centroids, &centroids_schema) == 0,
           "IVF trainer options failed");
    CHECK_OK();

    struct ArrowArray codebook = {0};
    struct ArrowSchema codebook_schema = {0};
    ASSERT(lance_index_train_pq_model_with_options(
               dataset, "embedding", 2, 4, LANCE_METRIC_L2, NULL, 0,
               &centroids, &centroids_schema, &trainer, &codebook,
               &codebook_schema) == 0,
           "PQ trainer options failed");
    CHECK_OK();
    release_arrow(&codebook, &codebook_schema);
    release_arrow(&centroids, &centroids_schema);
}

static void test_prewarm(LanceDataset *dataset) {
    LanceIndexPrewarmOptions options;
    memset(&options, 0, sizeof(options));
    options.with_position = true;
    ASSERT(lance_dataset_prewarm_index_with_options(
               dataset, "c_fts_caps", &options) == 0,
           "position prewarm failed");
    CHECK_OK();

    options.has_segment_selection = true;
    ASSERT(lance_dataset_prewarm_index_with_options(
               dataset, "c_fts_caps", &options) == 0,
           "explicit empty prewarm selection failed");
    CHECK_OK();

    uint64_t segment_count =
        lance_dataset_index_segment_count(dataset, "c_fts_caps");
    CHECK_OK();
    ASSERT(segment_count > 0, "FTS index has no segments");
    uint8_t *uuids = (uint8_t *)malloc((size_t)segment_count * 16);
    ASSERT(uuids != NULL, "UUID allocation failed");
    uint64_t written = 0;
    ASSERT(lance_dataset_index_segments(dataset, "c_fts_caps", uuids,
                                        (size_t)segment_count, &written) == 0,
           "segment UUID listing failed");
    CHECK_OK();
    ASSERT(written == segment_count, "segment UUID count mismatch");
    options.segment_uuids = uuids;
    options.segment_count = (size_t)written;
    ASSERT(lance_dataset_prewarm_index_with_options(
               dataset, "c_fts_caps", &options) == 0,
           "selected position prewarm failed");
    CHECK_OK();

    uint8_t missing[16];
    memset(missing, 0xa5, sizeof(missing));
    options.segment_uuids = missing;
    options.segment_count = 1;
    ASSERT(lance_dataset_prewarm_index_with_options(
               dataset, "c_fts_caps", &options) == -1,
           "unknown prewarm segment must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_NOT_FOUND,
           "unknown prewarm segment error code");
    {
        const char *message = lance_last_error_message();
        if (message) lance_free_string(message);
    }
    free(uuids);

    memset(&options, 0, sizeof(options));
    options.with_position = true;
    ASSERT(lance_dataset_prewarm_index_with_options(
               dataset, "c_btree_caps", &options) == -1,
           "positions on a non-inverted index must fail");
    {
        const char *message = lance_last_error_message();
        if (message) lance_free_string(message);
    }
}

static void test_physical_merge(LanceDataset *dataset) {
    const char *params =
        "{\"base_tokenizer\":\"simple\",\"language\":\"English\","
        "\"with_position\":true}";
    const uint8_t index_uuid[16] = {
        0x43, 0x4f, 0x4d, 0x42, 0x49, 0x4e, 0x45, 0x44,
        0x8f, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd,
    };
    uint64_t fragment_ids[3] = {0, 0, 0};
    ASSERT(lance_dataset_fragment_ids(dataset, fragment_ids) == 0,
           "fragment id listing failed");
    CHECK_OK();

    uint8_t *metadata[2] = {NULL, NULL};
    size_t metadata_lens[2] = {0, 0};
    LanceScalarIndexOptions scalar_options = {true, true};
    for (size_t i = 0; i < 2; ++i) {
        uint32_t fragment_id = (uint32_t)fragment_ids[i];
        LanceIndexSegmentBuildOptions build;
        memset(&build, 0, sizeof(build));
        build.fragment_ids = &fragment_id;
        build.fragment_count = 1;
        build.index_uuid = index_uuid;
        build.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;
        LanceIndexSegmentBuilder *builder =
            lance_index_segment_builder_new_scalar_with_options(
                dataset, "text", "c_dist_fts", LANCE_SCALAR_INVERTED, params,
                &scalar_options, &build);
        ASSERT(builder != NULL, "distributed FTS builder creation failed");
        CHECK_OK();
        ASSERT(lance_index_segment_builder_execute_uncommitted(
                   builder, &metadata[i], &metadata_lens[i]) == 0,
               "distributed FTS segment build failed");
        CHECK_OK();
        lance_index_segment_builder_free(builder);
    }

    uint64_t version_before = lance_dataset_version(dataset);
    CHECK_OK();
    ProgressCapture capture = {0};
    ASSERT(lance_dataset_merge_index_metadata(
               dataset, index_uuid, "Inverted", true, 0, capture_progress,
               &capture) == 0,
           "metadata merge failed");
    CHECK_OK();
    ASSERT(atomic_load(&capture.invalid) == 0,
           "metadata merge callback contract violated");
    ASSERT(lance_dataset_version(dataset) == version_before,
           "metadata merge must not commit");

    uint8_t *physical_metadata[2] = {NULL, NULL};
    size_t physical_metadata_lens[2] = {0, 0};
    for (size_t i = 0; i < 2; ++i) {
        uint8_t physical_uuid[16];
        memcpy(physical_uuid, index_uuid, sizeof(physical_uuid));
        physical_uuid[15] = (uint8_t)(i + 1);
        uint32_t fragment_id = (uint32_t)fragment_ids[i];
        LanceIndexSegmentBuildOptions build;
        memset(&build, 0, sizeof(build));
        build.fragment_ids = &fragment_id;
        build.fragment_count = 1;
        build.index_uuid = physical_uuid;
        build.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;
        LanceIndexSegmentBuilder *builder =
            lance_index_segment_builder_new_scalar_with_options(
                dataset, "text", "c_dist_fts_physical", LANCE_SCALAR_INVERTED,
                params, &scalar_options, &build);
        ASSERT(builder != NULL, "physical merge builder creation failed");
        CHECK_OK();
        ASSERT(lance_index_segment_builder_execute_uncommitted(
                   builder, &physical_metadata[i], &physical_metadata_lens[i]) == 0,
               "physical merge segment build failed");
        CHECK_OK();
        lance_index_segment_builder_free(builder);
    }

    uint8_t *merged = NULL;
    size_t merged_len = 0;
    ASSERT(lance_dataset_merge_index_segments(
               dataset, (const uint8_t *const *)physical_metadata,
               physical_metadata_lens, 2,
               &merged, &merged_len) == 0,
           "physical segment merge failed");
    CHECK_OK();
    ASSERT(merged != NULL && merged_len > 0, "physical merge returned no bytes");
    ASSERT(lance_dataset_version(dataset) == version_before,
           "physical merge must not commit");

    LanceIndexSegmentMetadata *metadata_handle = NULL;
    ASSERT(lance_index_segment_metadata_parse(
               merged, merged_len, &metadata_handle) == 0,
           "merged metadata parse failed");
    CHECK_OK();
    ASSERT(lance_index_segment_metadata_fragment_count(metadata_handle) == 2,
           "merged coverage should contain two fragments");
    ASSERT(lance_dataset_commit_index_segments(
               dataset, "c_fts_merged", "text",
               (const uint8_t *const *)&merged, &merged_len, 1) == 0,
           "merged segment commit failed");
    CHECK_OK();
    ASSERT(lance_dataset_version(dataset) == version_before + 1,
           "commit must publish exactly one version");

    uint8_t *untouched_bytes = NULL;
    size_t untouched_len = 777;
    ASSERT(lance_dataset_merge_index_segments(
               dataset, NULL, NULL, 0, &untouched_bytes, &untouched_len) == -1,
           "empty physical merge must use native validation");
    ASSERT(untouched_bytes == NULL && untouched_len == 777,
           "failed physical merge must leave outputs unchanged");
    {
        const char *message = lance_last_error_message();
        if (message) lance_free_string(message);
    }
    ASSERT(lance_dataset_merge_index_metadata(
               dataset, index_uuid, "BTree", false, 0, NULL, NULL) == -1,
           "BTree metadata merge must fail");
    {
        const char *message = lance_last_error_message();
        if (message) lance_free_string(message);
    }

    lance_index_segment_metadata_free(metadata_handle);
    lance_free_bytes(merged);
    for (size_t i = 0; i < 2; ++i) lance_free_bytes(metadata[i]);
    for (size_t i = 0; i < 2; ++i) lance_free_bytes(physical_metadata[i]);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <dataset_uri>\n", argv[0]);
        return 1;
    }

    LanceDataset *dataset = lance_dataset_open(argv[1], NULL, 0);
    ASSERT(dataset != NULL, "dataset open failed");
    CHECK_OK();

    const char *empty = lance_dataset_describe_indices_json(dataset);
    ASSERT(empty != NULL && strcmp(empty, "[]") == 0,
           "fresh dataset description must be empty");
    lance_free_string(empty);
    CHECK_OK();

    LanceScalarIndexOptions train = {true, true};
    ASSERT(lance_dataset_create_scalar_index_with_options(
               dataset, "id", "c_btree_caps", LANCE_SCALAR_BTREE, NULL,
               &train, false) == 0,
           "BTree with scalar options failed");
    CHECK_OK();
    LanceScalarIndexOptions definition_only = {true, false};
    ASSERT(lance_dataset_create_scalar_index_with_options(
               dataset, "id", "c_btree_defined", LANCE_SCALAR_BTREE, NULL,
               &definition_only, false) == 0,
           "definition-only BTree failed");
    CHECK_OK();

    const char *fts_params =
        "{\"base_tokenizer\":\"simple\",\"language\":\"English\","
        "\"with_position\":true}";
    ASSERT(lance_dataset_create_scalar_index_with_options(
               dataset, "text", "c_fts_caps", LANCE_SCALAR_INVERTED, fts_params,
               &train, false) == 0,
           "positional FTS creation failed");
    CHECK_OK();

    LanceVectorIndexOptions vector = vector_options(4);
    ASSERT(lance_dataset_create_vector_index_with_options(
               dataset, "embedding", "c_ivf_caps", &vector, false) == 0,
           "IVF index creation failed");
    CHECK_OK();

    const char *description = lance_dataset_describe_indices_json(dataset);
    ASSERT(description != NULL, "rich description failed");
    ASSERT(strstr(description, "\"name\":\"c_btree_caps\"") != NULL,
           "description omitted BTree");
    ASSERT(strstr(description, "\"name\":\"c_fts_caps\"") != NULL,
           "description omitted FTS");
    ASSERT(strstr(description, "\"name\":\"c_ivf_caps\"") != NULL,
           "description omitted IVF");
    ASSERT(strstr(description, "\"type_url\":\"\"") == NULL,
           "description fabricated an absent type URL");
    lance_free_string(description);
    CHECK_OK();

    const char *statistics =
        lance_dataset_index_statistics_json(dataset, "c_ivf_caps");
    ASSERT(statistics != NULL && strstr(statistics, "\"indices\"") != NULL,
           "vector statistics missing native indices");
    ASSERT(strstr(statistics, "\"num_partitions\"") != NULL,
           "vector statistics missing partition count");
    lance_free_string(statistics);
    CHECK_OK();
    ASSERT(lance_dataset_index_statistics_json(dataset, "missing_idx") == NULL,
           "missing statistics index must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_NOT_FOUND,
           "missing statistics error code");
    {
        const char *message = lance_last_error_message();
        if (message) lance_free_string(message);
    }
    ASSERT(lance_dataset_index_statistics_json(dataset, "") == NULL,
           "empty statistics index must fail");
    ASSERT(lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
           "empty statistics error code");
    {
        const char *message = lance_last_error_message();
        if (message) lance_free_string(message);
    }

    test_trainers(dataset);

    LanceIvfModel *model = lance_dataset_get_ivf_model(dataset, "c_ivf_caps");
    ASSERT(model != NULL, "IVF model retrieval failed");
    CHECK_OK();
    ASSERT(lance_ivf_model_has_centroids(model), "IVF model has no centroids");
    CHECK_OK();
    struct ArrowArray centroids = {0};
    struct ArrowSchema centroids_schema = {0};
    ASSERT(lance_ivf_model_export_centroids(
               model, &centroids, &centroids_schema) == 0,
           "centroid export failed");
    CHECK_OK();
    release_arrow(&centroids, &centroids_schema);
    ASSERT(lance_ivf_model_num_partitions(model) == 4,
           "unexpected IVF partition count");
    CHECK_OK();
    ASSERT(lance_ivf_model_dimension(model) == 8,
           "unexpected IVF dimension");
    CHECK_OK();
    for (uint64_t partition = 0; partition < 4; ++partition) {
        (void)lance_ivf_model_partition_size(model, partition);
        CHECK_OK();
    }
    ASSERT(lance_ivf_model_partition_size(model, 99) == 0,
           "absent partition size must be zero");
    CHECK_OK();

    uint64_t stream_rows = 0;
    for (uint64_t partition = 0; partition < 4; ++partition) {
        struct ArrowArrayStream stream = {0};
        ASSERT(lance_dataset_read_index_partition(
                   dataset, "c_ivf_caps", partition, false, &stream) == 0,
               "partition read failed");
        CHECK_OK();
        stream_rows += (uint64_t)count_stream_rows(&stream);
    }
    ASSERT(stream_rows == 192, "partition row IDs must cover every vector");

    float query[8] = {20.0f, 20.1f, 20.2f, 20.3f, 20.4f, 20.5f, 20.6f, 20.7f};
    int64_t fixed_rows =
        query_vector_rows(dataset, query, 20, 1, 1, 0, 0.0, 0);
    int64_t effort_first =
        query_vector_rows(dataset, query, 20, 1, 1, 1, 1.0, 1);
    int64_t effort_last =
        query_vector_rows(dataset, query, 20, 1, 1, 1, 1.0, 0);
    ASSERT(fixed_rows > 0 && effort_first == effort_last,
           "search_effort must be independent of fixed-nprobes setter order");

    LanceScanner *invalid =
        lance_scanner_new(dataset, NULL, NULL);
    ASSERT(invalid != NULL, "invalid-effort scanner creation failed");
    CHECK_OK();
    ASSERT(lance_scanner_set_search_effort(invalid, NAN) == -1,
           "NaN search effort must fail");
    ASSERT(lance_scanner_set_search_effort(invalid, 1.5) == -1,
           "out-of-range search effort must fail");
    {
        const char *message = lance_last_error_message();
        if (message) lance_free_string(message);
    }
    lance_scanner_close(invalid);

    LanceScanner *without_nearest =
        lance_scanner_new(dataset, NULL, NULL);
    ASSERT(without_nearest != NULL, "no-nearest scanner creation failed");
    CHECK_OK();
    ASSERT(lance_scanner_set_search_effort(without_nearest, 0.5) == 0,
           "stored search effort must be accepted before nearest");
    struct ArrowArrayStream no_nearest_stream = {0};
    ASSERT(lance_scanner_to_arrow_stream(
               without_nearest, &no_nearest_stream) == -1,
           "search_effort without nearest must fail at materialization");
    ASSERT(no_nearest_stream.release == NULL,
           "failed materialization must not export a stream");
    {
        const char *message = lance_last_error_message();
        if (message) lance_free_string(message);
    }
    lance_scanner_close(without_nearest);

    test_prewarm(dataset);

    uint32_t first_fragment = 0;
    LanceIndexSegmentBuildOptions defined_build;
    memset(&defined_build, 0, sizeof(defined_build));
    defined_build.fragment_ids = &first_fragment;
    defined_build.fragment_count = 1;
    defined_build.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;
    LanceIndexSegmentBuilder *defined_builder =
        lance_index_segment_builder_new_scalar_with_options(
            dataset, "text", "c_fts_defined", LANCE_SCALAR_INVERTED, fts_params,
            &definition_only, &defined_build);
    ASSERT(defined_builder != NULL,
           "definition-only FTS segment builder failed");
    CHECK_OK();
    uint8_t *defined_bytes = NULL;
    size_t defined_len = 0;
    ASSERT(lance_index_segment_builder_execute_uncommitted(
               defined_builder, &defined_bytes, &defined_len) == 0,
           "definition-only FTS segment execution failed");
    CHECK_OK();
    lance_free_bytes(defined_bytes);
    lance_index_segment_builder_free(defined_builder);

    test_physical_merge(dataset);
    lance_ivf_model_free(model);
    lance_dataset_close(dataset);

    dataset = lance_dataset_open(argv[1], NULL, 0);
    ASSERT(dataset != NULL, "dataset reopen failed");
    CHECK_OK();
    description = lance_dataset_describe_indices_json(dataset);
    ASSERT(description != NULL &&
               strstr(description, "\"name\":\"c_fts_merged\"") != NULL,
           "reopened dataset omitted merged FTS index");
    lance_free_string(description);
    CHECK_OK();

    model = lance_dataset_get_ivf_model(dataset, "c_ivf_caps");
    ASSERT(model != NULL, "lifetime IVF model retrieval failed");
    CHECK_OK();
    struct ArrowArrayStream lifetime_stream = {0};
    ASSERT(lance_dataset_read_index_partition(
               dataset, "c_ivf_caps", 0, true, &lifetime_stream) == 0,
           "lifetime partition stream creation failed");
    CHECK_OK();
    lance_dataset_close(dataset);
    ASSERT(lance_ivf_model_num_partitions(model) == 4,
           "model must outlive its dataset");
    CHECK_OK();
    ASSERT(count_stream_rows(&lifetime_stream) > 0,
           "partition stream must outlive its dataset");
    lance_ivf_model_free(model);

    printf("Native index capability C tests passed\n");
    return 0;
}
