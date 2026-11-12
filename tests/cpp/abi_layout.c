// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include <stddef.h>
#include <stdio.h>

#include "lance/lance.h"

#define PRINT_TYPE(type) \
    printf("T|%s|%zu|%zu\n", #type, sizeof(type), _Alignof(type))

#define PRINT_FIELD(type, field) \
    printf("F|%s.%s|%zu\n", #type, #field, offsetof(type, field))

int main(void) {
    PRINT_TYPE(LanceErrorCode);
    PRINT_TYPE(LanceVectorIndexType);
    PRINT_TYPE(LanceScalarIndexType);
    PRINT_TYPE(LanceMetricType);
    PRINT_TYPE(LanceDataType);
    PRINT_TYPE(LanceMergeWhenMatched);
    PRINT_TYPE(LanceMergeWhenNotMatched);
    PRINT_TYPE(LanceMergeWhenNotMatchedBySource);
    PRINT_TYPE(LanceColumnNullableMode);
    PRINT_TYPE(LanceScanMetricKind);
    PRINT_TYPE(LancePollStatus);
    PRINT_TYPE(LanceIndexSegmentBuildMode);
    PRINT_TYPE(LanceFtsCoverageMode);
    PRINT_TYPE(LanceWriteMode);

    PRINT_TYPE(LanceVectorIndexParams);
    PRINT_FIELD(LanceVectorIndexParams, index_type);
    PRINT_FIELD(LanceVectorIndexParams, metric);
    PRINT_FIELD(LanceVectorIndexParams, num_partitions);
    PRINT_FIELD(LanceVectorIndexParams, num_sub_vectors);
    PRINT_FIELD(LanceVectorIndexParams, num_bits);
    PRINT_FIELD(LanceVectorIndexParams, max_iterations);
    PRINT_FIELD(LanceVectorIndexParams, hnsw_m);
    PRINT_FIELD(LanceVectorIndexParams, hnsw_ef_construction);
    PRINT_FIELD(LanceVectorIndexParams, sample_rate);

    PRINT_TYPE(LanceMergeInsertParams);
    PRINT_FIELD(LanceMergeInsertParams, when_matched);
    PRINT_FIELD(LanceMergeInsertParams, when_matched_expr);
    PRINT_FIELD(LanceMergeInsertParams, when_not_matched);
    PRINT_FIELD(LanceMergeInsertParams, when_not_matched_by_source);
    PRINT_FIELD(LanceMergeInsertParams, when_not_matched_by_source_expr);

    PRINT_TYPE(LanceMergeInsertResult);
    PRINT_FIELD(LanceMergeInsertResult, num_inserted_rows);
    PRINT_FIELD(LanceMergeInsertResult, num_updated_rows);
    PRINT_FIELD(LanceMergeInsertResult, num_deleted_rows);

    PRINT_TYPE(LanceCompactionOptions);
    PRINT_FIELD(LanceCompactionOptions, target_rows_per_fragment);
    PRINT_FIELD(LanceCompactionOptions, max_rows_per_group);
    PRINT_FIELD(LanceCompactionOptions, max_bytes_per_file);
    PRINT_FIELD(LanceCompactionOptions, num_threads);
    PRINT_FIELD(LanceCompactionOptions, batch_size);

    PRINT_TYPE(LanceCompactionMetrics);
    PRINT_FIELD(LanceCompactionMetrics, fragments_removed);
    PRINT_FIELD(LanceCompactionMetrics, fragments_added);
    PRINT_FIELD(LanceCompactionMetrics, files_removed);
    PRINT_FIELD(LanceCompactionMetrics, files_added);

    PRINT_TYPE(LanceColumnAlteration);
    PRINT_FIELD(LanceColumnAlteration, path);
    PRINT_FIELD(LanceColumnAlteration, rename);
    PRINT_FIELD(LanceColumnAlteration, nullable_mode);
    PRINT_FIELD(LanceColumnAlteration, data_type);

    PRINT_TYPE(LanceSqlColumn);
    PRINT_FIELD(LanceSqlColumn, name);
    PRINT_FIELD(LanceSqlColumn, expression);

    PRINT_TYPE(LanceScanMetric);
    PRINT_FIELD(LanceScanMetric, name);
    PRINT_FIELD(LanceScanMetric, name_len);
    PRINT_FIELD(LanceScanMetric, kind);
    PRINT_FIELD(LanceScanMetric, value);

    PRINT_TYPE(LanceScanStatistics);
    PRINT_FIELD(LanceScanStatistics, iops);
    PRINT_FIELD(LanceScanStatistics, requests);
    PRINT_FIELD(LanceScanStatistics, bytes_read);
    PRINT_FIELD(LanceScanStatistics, indices_loaded);
    PRINT_FIELD(LanceScanStatistics, index_partitions_loaded);
    PRINT_FIELD(LanceScanStatistics, index_comparisons);
    PRINT_FIELD(LanceScanStatistics, metrics);
    PRINT_FIELD(LanceScanStatistics, metrics_len);

    PRINT_TYPE(LanceIndexSegmentBuildOptions);
    PRINT_FIELD(LanceIndexSegmentBuildOptions, fragment_ids);
    PRINT_FIELD(LanceIndexSegmentBuildOptions, fragment_count);
    PRINT_FIELD(LanceIndexSegmentBuildOptions, index_uuid);
    PRINT_FIELD(LanceIndexSegmentBuildOptions, ivf_centroids);
    PRINT_FIELD(LanceIndexSegmentBuildOptions, ivf_centroids_schema);
    PRINT_FIELD(LanceIndexSegmentBuildOptions, pq_codebook);
    PRINT_FIELD(LanceIndexSegmentBuildOptions, pq_codebook_schema);
    PRINT_FIELD(LanceIndexSegmentBuildOptions, mode);

    PRINT_TYPE(LanceVectorIndexSegmentParams);
    PRINT_FIELD(LanceVectorIndexSegmentParams, index_type);
    PRINT_FIELD(LanceVectorIndexSegmentParams, metric);
    PRINT_FIELD(LanceVectorIndexSegmentParams, num_partitions);
    PRINT_FIELD(LanceVectorIndexSegmentParams, num_sub_vectors);
    PRINT_FIELD(LanceVectorIndexSegmentParams, num_bits);
    PRINT_FIELD(LanceVectorIndexSegmentParams, max_iterations);
    PRINT_FIELD(LanceVectorIndexSegmentParams, hnsw_m);
    PRINT_FIELD(LanceVectorIndexSegmentParams, hnsw_ef_construction);
    PRINT_FIELD(LanceVectorIndexSegmentParams, sample_rate);

    PRINT_TYPE(LanceWriteParams);
    PRINT_FIELD(LanceWriteParams, max_rows_per_file);
    PRINT_FIELD(LanceWriteParams, max_rows_per_group);
    PRINT_FIELD(LanceWriteParams, max_bytes_per_file);
    PRINT_FIELD(LanceWriteParams, data_storage_version);
    PRINT_FIELD(LanceWriteParams, enable_stable_row_ids);

    return 0;
}
