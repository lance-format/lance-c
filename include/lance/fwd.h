/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file fwd.h
 * @brief Opaque native handle declarations shared by the domain headers.
 */

#ifndef LANCE_FWD_H
#define LANCE_FWD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque native dynamic storage-options handles. The accessor is declared
   early because dataset open and write options attach it. */
typedef struct LanceStorageOptionsAccessor LanceStorageOptionsAccessor;

/* Defined with the native write-options declarations; forward-declared
 * here so dataset open entry points can take it by pointer. */
typedef struct LanceBaseStoreParamsList LanceBaseStoreParamsList;

/* ─── Opaque handles ─── */

typedef struct LanceDataset  LanceDataset;
typedef struct LanceScanner  LanceScanner;
typedef struct LanceBatch    LanceBatch;
typedef struct LanceSession  LanceSession;
typedef struct LanceVersions LanceVersions;
typedef struct LanceVersion  LanceVersion;
typedef struct LanceVersionRefs LanceVersionRefs;
typedef struct LanceRef      LanceRef;
typedef struct LanceTag      LanceTag;
typedef struct LanceTags     LanceTags;
typedef struct LanceDataStatistics LanceDataStatistics;
typedef struct LanceCleanupExplanation LanceCleanupExplanation;
typedef struct LanceDatasetConfig LanceDatasetConfig;
typedef struct LanceDatasetDeltaBuilder LanceDatasetDeltaBuilder;
typedef struct LanceDatasetDelta LanceDatasetDelta;
typedef struct LanceSqlQueryBuilder LanceSqlQueryBuilder;
typedef struct LanceSqlQuery LanceSqlQuery;
typedef struct LanceCompactionOptionsV2 LanceCompactionOptionsV2;
typedef struct LanceCompactionPlan LanceCompactionPlan;
typedef struct LanceCompactionTask LanceCompactionTask;
typedef struct LanceRewriteResult LanceRewriteResult;
typedef struct LanceOptimizeIndicesOptions LanceOptimizeIndicesOptions;
typedef struct LanceOptimizeIndicesPlan LanceOptimizeIndicesPlan;
typedef struct LanceOptimizeIndexTask LanceOptimizeIndexTask;
typedef struct LanceOptimizeIndexMerge LanceOptimizeIndexMerge;
typedef struct LanceIndexSegmentResult LanceIndexSegmentResult;
typedef struct LanceIndexSegmentBuilder LanceIndexSegmentBuilder;
typedef struct LanceIndexSegmentMetadata LanceIndexSegmentMetadata;
typedef struct LanceFtsQueryContext LanceFtsQueryContext;
typedef struct LanceFtsQuery LanceFtsQuery;
typedef struct LanceFtsTokenList LanceFtsTokenList;
typedef struct LanceBlobFile LanceBlobFile;
typedef struct LanceBlobDescriptorArrayBuilder LanceBlobDescriptorArrayBuilder;
typedef struct LanceDataFile LanceDataFile;
/** Owned native `DataReplacementGroup` snapshot. */
typedef struct LanceDataReplacementGroup LanceDataReplacementGroup;
typedef struct LanceFragment LanceFragment;
typedef struct LanceTransaction LanceTransaction;
typedef struct LanceOperation LanceOperation;
typedef struct LanceDataOverlayFile LanceDataOverlayFile;
typedef struct LanceDataOverlayGroup LanceDataOverlayGroup;
typedef struct LanceFragmentOverlayWriter LanceFragmentOverlayWriter;
typedef struct LanceFragmentUpdateColumnsResult LanceFragmentUpdateColumnsResult;
typedef struct LanceUpdatedFragmentOffsets LanceUpdatedFragmentOffsets;
typedef struct LanceKeyExistenceFilter LanceKeyExistenceFilter;
typedef struct LanceRewriteGroup LanceRewriteGroup;
typedef struct LanceRewrittenIndex LanceRewrittenIndex;
typedef struct LanceCompactedSsTable LanceCompactedSsTable;
typedef struct LanceIvfModel LanceIvfModel;
typedef struct LanceSchema LanceSchema;
typedef struct LanceNamespace LanceNamespace;
typedef struct LanceRestAdapter LanceRestAdapter;
typedef struct LanceCommitLock LanceCommitLock;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LANCE_FWD_H */
