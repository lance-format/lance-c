/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file lance.h
 * @brief C API for the Lance columnar data format.
 *
 * All data crosses this boundary via the Arrow C Data Interface
 * (ArrowSchema, ArrowArray, ArrowArrayStream).
 * For Arrow structures written to caller-provided output storage, the caller
 * retains ownership of the outer structure and must invoke its non-NULL
 * `release` callback exactly once to release the contents. APIs that allocate
 * the outer structure as well document a separate matching free function.
 *
 * Error handling uses thread-local storage: after any function returns its
 * documented error sentinel (for example NULL, -1, or 0 for selected scalar
 * accessors), call lance_last_error_code() and lance_last_error_message() to
 * get details.
 */

#ifndef LANCE_H
#define LANCE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Arrow C Data Interface forward declarations ─── */
/* These match the canonical Arrow spec structs. If you already include
   arrow/c/abi.h, guard with ARROW_C_DATA_INTERFACE. */

#ifndef ARROW_C_DATA_INTERFACE
#define ARROW_C_DATA_INTERFACE

struct ArrowSchema {
    const char* format;
    const char* name;
    const char* metadata;
    int64_t flags;
    int64_t n_children;
    struct ArrowSchema** children;
    struct ArrowSchema* dictionary;
    void (*release)(struct ArrowSchema*);
    void* private_data;
};

struct ArrowArray {
    int64_t length;
    int64_t null_count;
    int64_t offset;
    int64_t n_buffers;
    int64_t n_children;
    const void** buffers;
    struct ArrowArray** children;
    struct ArrowArray* dictionary;
    void (*release)(struct ArrowArray*);
    void* private_data;
};

struct ArrowArrayStream {
    int (*get_schema)(struct ArrowArrayStream*, struct ArrowSchema* out);
    int (*get_next)(struct ArrowArrayStream*, struct ArrowArray* out);
    const char* (*get_last_error)(struct ArrowArrayStream*);
    void (*release)(struct ArrowArrayStream*);
    void* private_data;
};

#endif /* ARROW_C_DATA_INTERFACE */

/* ─── Error handling ─── */

typedef enum {
    LANCE_OK = 0,
    LANCE_ERR_INVALID_ARGUMENT = 1,
    LANCE_ERR_IO = 2,
    LANCE_ERR_NOT_FOUND = 3,
    LANCE_ERR_DATASET_ALREADY_EXISTS = 4,
    LANCE_ERR_INDEX = 5,
    LANCE_ERR_INTERNAL = 6,
    LANCE_ERR_NOT_SUPPORTED = 7,
    LANCE_ERR_COMMIT_CONFLICT = 8,
    /* An unexpected panic was caught at the FFI boundary. */
    LANCE_ERR_PANIC = 9,
} LanceErrorCode;

/**
 * Panic handling (issue lance-format/lance-c#61).
 *
 * A panic raised inside Lance/Arrow code is caught at the FFI boundary and
 * reported as LANCE_ERR_PANIC through the usual thread-local error channel
 * (lance_last_error_code / lance_last_error_message) instead of unwinding
 * into the host. After a panic:
 *
 *  - A LanceScanner handle is poisoned: every later call on it fails with
 *    LANCE_ERR_PANIC ("scanner is poisoned by an earlier panic"). Close it
 *    and build a fresh scanner; do not retry the poisoned handle.
 *  - A LanceDataset handle remains usable: commits are atomic manifest
 *    swaps, and a mutation that panics before commit rolls back in memory,
 *    leaving the last committed snapshot intact.
 *
 * Honest limits: a double panic, a panic in a destructor while unwinding, a
 * stack overflow, or an allocation failure still aborts the process. A
 * panic caught inside a close/free call (lance_*_close, lance_batch_free,
 * lance_free_string, lance_scanner_async_stream_free, or the release callback
 * of an exported ArrowArrayStream) is logged and the remainder of the value
 * may leak — close is best-effort by design. Post-panic process state is
 * best-effort: hosts should fail the in-flight query rather than retry a
 * poisoned handle.
 *
 * Callbacks passed INTO the library (LanceCallback, LanceWaker, and
 * LanceScanStatisticsCallback, LanceTrackedFilesProgressCallback and
 * LanceTrackedFilesProgressRelease) are the reverse direction and are NOT
 * covered by this contract: their ABI is non-unwinding, so a callback that
 * throws or unwinds can abort the host process before the library can contain
 * it. Callbacks must return normally.
 *
 * This contract requires Rust's `panic = "unwind"` strategy. The crate
 * rejects `panic = "abort"` builds at compile time because catch_unwind
 * cannot provide this API contract in such a build.
 */

/* ─── Index types (Phase 2) ─── */

typedef enum {
    LANCE_INDEX_IVF_FLAT      = 101,
    LANCE_INDEX_IVF_SQ        = 102,
    LANCE_INDEX_IVF_PQ        = 103,
    LANCE_INDEX_IVF_HNSW_SQ   = 104,
    LANCE_INDEX_IVF_HNSW_PQ   = 105,
    LANCE_INDEX_IVF_HNSW_FLAT = 106,
    LANCE_INDEX_IVF_RQ        = 107,
} LanceVectorIndexType;

typedef enum {
    LANCE_SCALAR_BTREE        = 1,
    LANCE_SCALAR_BITMAP       = 2,
    LANCE_SCALAR_LABEL_LIST   = 3,
    LANCE_SCALAR_INVERTED     = 4,
    LANCE_SCALAR_NGRAM        = 5,
    LANCE_SCALAR_ZONE_MAP     = 6,
    LANCE_SCALAR_BLOOM_FILTER = 7,
    LANCE_SCALAR_MINHASH_LSH  = 8,
} LanceScalarIndexType;

typedef enum {
    LANCE_METRIC_L2      = 0,
    LANCE_METRIC_COSINE  = 1,
    LANCE_METRIC_DOT     = 2,
    LANCE_METRIC_HAMMING = 3,
} LanceMetricType;

/** Speed / accuracy tradeoff for approximate vector search. */
typedef enum {
    LANCE_APPROX_MODE_FAST     = 0,
    LANCE_APPROX_MODE_NORMAL   = 1,
    LANCE_APPROX_MODE_ACCURATE = 2,
} LanceApproxMode;

typedef enum {
    LANCE_DTYPE_FLOAT32 = 0,
    LANCE_DTYPE_FLOAT16 = 1,
    LANCE_DTYPE_FLOAT64 = 2,
    LANCE_DTYPE_UINT8   = 3,
    LANCE_DTYPE_INT8    = 4,
} LanceDataType;

/**
 * Legacy vector parameters. Values 101-106 retain their existing defaults and
 * required-field rules. IVF_RQ (107) uses num_partitions and num_bits where
 * num_bits 0 selects the native RQ default of 5; RQ rotation JSON and richer
 * absent-versus-zero controls require the additive options entry points.
 */
typedef struct {
    LanceVectorIndexType index_type;
    LanceMetricType      metric;
    uint32_t num_partitions;        /* IVF; required, must be > 0 */
    uint32_t num_sub_vectors;       /* PQ; required, must be > 0 */
    uint32_t num_bits;              /* PQ: 0 (default 8), 4, or 8; SQ: 0 or 8 */
    uint32_t max_iterations;        /* IVF kmeans; 0 = 50 */
    uint32_t hnsw_m;                /* HNSW; required, must be > 0 */
    uint32_t hnsw_ef_construction;  /* HNSW; 0 = default */
    uint32_t sample_rate;           /* IVF; 0 = 256 */
} LanceVectorIndexParams;

typedef enum {
    LANCE_INDEX_FILE_VERSION_V3 = 0,
    LANCE_INDEX_FILE_VERSION_LEGACY = 1,
} LanceIndexFileVersion;

/**
 * Presence-aware unsigned option. When `is_set` is false the native option is
 * absent and `value` is ignored. When true, `value` is forwarded verbatim,
 * including zero. This keeps native automatic sizing and explicit-zero
 * validation distinguishable.
 */
typedef struct {
    uint64_t value;
    bool is_set;
} LanceOptionalU64;

/**
 * Presence-aware timestamp split into whole Unix seconds and nanoseconds.
 * When `is_set` is false the native option is absent and the other fields are
 * zero.
 */
typedef struct {
    bool is_set;
    int64_t unix_seconds;
    uint32_t nanos; /* 0..999999999 */
} LanceOptionalTimestamp;

/**
 * Borrowed UTF-8 key/value arrays with `count` entries. Keys and values may
 * not be NULL when count is non-zero. Duplicate keys use Rust HashMap
 * last-write-wins semantics; the last value for a key is retained.
 */
typedef struct {
    const char* const* keys;
    const char* const* values;
    size_t count;
} LanceStringMap;

/**
 * Precomputed shuffle input. `path` is required. A non-NULL struct with
 * `buffer_count == 0` represents an explicitly empty buffer list.
 */
typedef struct {
    const char* path;
    const char* const* buffers;
    size_t buffer_count;
} LancePrecomputedShuffleBuffers;

/**
 * Additive native vector-build options. Zero-initialize this structure.
 *
 * Optional numeric fields distinguish absent (`is_set == false`) from an
 * explicit value, including zero. Native Lance code owns defaults, validation
 * timing and execution semantics. `num_bits` applies to IVF_PQ and IVF_RQ;
 * absent PQ bits use the native default and an absent RQ value uses the native
 * default of 5. `num_partitions` and `target_partition_size` may be absent for
 * native automatic sizing.
 *
 * `ivf_centroids` and `pq_codebook` are native Arrow C Data Interface
 * FixedSizeList values. The whole options object is borrowed only for this
 * call. A non-NULL ArrowArray may be replaced while a live caller-owned
 * equivalent is left in place; the schema remains caller-owned. Both members
 * of each pair must be NULL or non-NULL. Native model shape and dtype
 * acceptance is not narrowed here.
 *
 * `rabitq_model_json` is the native RabitQuantizationMetadata JSON accepted by
 * IVF_RQ. Use lance_index_build_rq_model() to mint a portable fast rotation.
 * `runtime_hints` may contain arbitrary UTF-8 keys and values and does not
 * enable any accelerator execution in this binding.
 *
 * `has_train == false` retains the native default train=true.
 */
typedef struct LanceVectorIndexOptions {
    int32_t index_type;   /* LanceVectorIndexType */
    int32_t metric;       /* LanceMetricType */

    LanceOptionalU64 num_partitions;
    LanceOptionalU64 target_partition_size;
    LanceOptionalU64 max_iters;
    LanceOptionalU64 sample_rate;
    LanceOptionalU64 streaming_sample_rate;
    LanceOptionalU64 streaming_coreset_rate;
    uint64_t streaming_refine_passes;

    const char* precomputed_partitions_file;
    const LancePrecomputedShuffleBuffers* precomputed_shuffle_buffers;
    LanceOptionalU64 shuffle_partition_batches;
    LanceOptionalU64 shuffle_partition_concurrency;
    const LanceStringMap* storage_options;

    LanceOptionalU64 num_sub_vectors;
    LanceOptionalU64 num_bits;

    struct ArrowArray* ivf_centroids;
    const struct ArrowSchema* ivf_centroids_schema;
    struct ArrowArray* pq_codebook;
    const struct ArrowSchema* pq_codebook_schema;
    const char* rabitq_model_json;

    LanceOptionalU64 hnsw_max_level;
    LanceOptionalU64 hnsw_m;
    LanceOptionalU64 hnsw_ef_construction;

    int32_t index_file_version; /* LanceIndexFileVersion */
    bool skip_transpose;
    const LanceStringMap* runtime_hints;
    bool has_train;
    bool train;
} LanceVectorIndexOptions;

/**
 * Additive standalone IVF/PQ trainer controls. Zero-initialize this structure.
 *
 * An absent field preserves the native trainer default. A present value is
 * forwarded verbatim, including zero, and native validation and failure
 * behavior apply.
 */
typedef struct LanceIndexTrainerOptions {
    LanceOptionalU64 sample_rate;
    LanceOptionalU64 max_iters;
} LanceIndexTrainerOptions;

/**
 * Additive scalar-index build controls.
 *
 * `has_train == false` preserves the native default (`train=true`). When true,
 * `train` is forwarded exactly to the native `CreateIndexBuilder`.
 */
typedef struct LanceScalarIndexOptions {
    bool has_train;
    bool train;
} LanceScalarIndexOptions;

/** Return the error code from the last failed operation on this thread. */
LanceErrorCode lance_last_error_code(void);

/** Return the error message. Caller must free with lance_free_string(). */
const char* lance_last_error_message(void);

/** Free a string returned by lance_last_error_message(). */
void lance_free_string(const char* s);

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
typedef struct LanceBlobFile LanceBlobFile;
typedef struct LanceBlobDescriptorArrayBuilder LanceBlobDescriptorArrayBuilder;
typedef struct LanceFragment LanceFragment;
typedef struct LanceTransaction LanceTransaction;
typedef struct LanceOperation LanceOperation;
typedef struct LanceIvfModel LanceIvfModel;

/* ─── Shared session ─── */

/**
 * Snapshot of a shared session's cache statistics.
 *
 * Cache sizes are the bytes currently retained, not their configured limits.
 */
typedef struct LanceSessionCacheStats {
    uint64_t index_cache_hits;
    uint64_t index_cache_misses;
    uint64_t index_cache_entries;
    uint64_t index_cache_size_bytes;
    uint64_t metadata_cache_hits;
    uint64_t metadata_cache_misses;
    uint64_t metadata_cache_entries;
    uint64_t metadata_cache_size_bytes;
} LanceSessionCacheStats;

/**
 * Cache selection for one session cache tier.
 *
 * Discriminants are ABI-stable. The `kind` field in
 * LanceSessionCacheSpec is a fixed-width int32_t, not this enum type, so
 * callers built with non-default enum sizing cannot mismatch the Rust ABI.
 */
typedef enum {
    /* Use the native Lance default capacity for this tier. */
    LANCE_SESSION_CACHE_SPEC_DEFAULT = 0,
    /* Use the native default in-memory backend with an explicit capacity. */
    LANCE_SESSION_CACHE_SPEC_SIZE = 1,
    /* Build a backend from a backend URI. */
    LANCE_SESSION_CACHE_SPEC_URI = 2,
    /* Build a backend from a kind plus string options. */
    LANCE_SESSION_CACHE_SPEC_CONFIG = 3,
} LanceSessionCacheSpecKind;

/**
 * Description of one session cache tier.
 *
 * Only the field selected by `kind` is read:
 *
 *  - DEFAULT reads no additional fields.
 *  - SIZE reads `size_bytes`. Zero is an explicit zero capacity, not DEFAULT.
 *  - URI reads `backend_uri`.
 *  - CONFIG reads `backend_kind`, then `option_count` entries from the
 *    parallel `option_keys` and `option_values` arrays.
 *
 * All strings must be NUL-terminated UTF-8. `option_count` is an exact count,
 * not a NULL-terminated sentinel: when it is non-zero, both option arrays must
 * be non-NULL and each selected key/value must be non-NULL. Duplicate option
 * keys are rejected. Unused fields are ignored.
 *
 * Every buffer is borrowed only for the duration of
 * lance_session_new_with_cache_specs(). Backend construction happens before
 * it returns, and the caller retains ownership of all buffers.
 */
typedef struct LanceSessionCacheSpec {
    /* LanceSessionCacheSpecKind discriminant. */
    int32_t     kind;
    /* Explicit capacity for SIZE, in bytes. */
    uint64_t    size_bytes;
    /* Backend URI for URI, e.g. "moka://?capacity=1048576". */
    const char* backend_uri;
    /* Registry kind for CONFIG, e.g. "moka". */
    const char* backend_kind;
    /* Parallel option-key array for CONFIG. */
    const char* const* option_keys;
    /* Parallel option-value array for CONFIG. */
    const char* const* option_values;
    /* Number of entries in option_keys and option_values. */
    size_t      option_count;
} LanceSessionCacheSpec;

/**
 * Create a session that can share metadata and index caches across datasets.
 *
 * Cache limits are specified in bytes. Pass 0 to request zero capacity.
 * @return Session handle, or NULL on error
 */
LanceSession* lance_session_new(
    uint64_t index_cache_size_bytes,
    uint64_t metadata_cache_size_bytes
);

/**
 * Create a session with independent cache configuration for each tier.
 *
 * Each spec maps to the native Lance CacheSpec: DEFAULT uses the tier default,
 * SIZE uses the native default in-memory backend with an explicit capacity,
 * and URI/CONFIG build a backend through the native cache registry. Backend
 * URI parsing, kind normalization, option validation, and unknown-kind errors
 * come from Lance core.
 *
 * Both spec pointers must be non-NULL. The specs and all buffers they
 * reference are borrowed only for this call.
 *
 * @return Session handle, or NULL on error
 */
LanceSession* lance_session_new_with_cache_specs(
    const LanceSessionCacheSpec* index_cache,
    const LanceSessionCacheSpec* metadata_cache
);

/**
 * Close a session handle. Safe to call with NULL. Datasets previously opened
 * with the session remain valid and retain the shared cache state.
 */
void lance_session_close(LanceSession* session);

/**
 * Copy current cache statistics to `out_stats`.
 *
 * @return 0 on success, -1 on error
 */
int32_t lance_session_get_cache_stats(
    const LanceSession* session,
    LanceSessionCacheStats* out_stats
);

/**
 * Copy the current deep size of the session to `out_size_bytes`.
 *
 * This walks live cache contents and includes retained session state. It is
 * not an RSS measurement or a configured capacity limit. The output is
 * written only on success.
 *
 * @return 0 on success, -1 on error
 */
int32_t lance_session_get_size_bytes(
    const LanceSession* session,
    uint64_t* out_size_bytes
);

/**
 * Copy the approximate number of items in the session to `out_num_items`.
 *
 * This is the native explicitly approximate inspection value. It can differ
 * from the exact cache entry counts and may change as cache maintenance runs.
 * The output is written only on success.
 *
 * @return 0 on success, -1 on error
 */
int32_t lance_session_get_approx_num_items(
    const LanceSession* session,
    uint64_t* out_num_items
);

/* ─── Dataset lifecycle ─── */

/**
 * Open a Lance dataset.
 *
 * Pass `version` = 0 to open the latest, or a specific version id (e.g. one
 * returned by `lance_dataset_versions`) to check out that version:
 *
 *     LanceDataset* ds = lance_dataset_open("data.lance", NULL, 42);
 *
 * @param uri           Dataset path (file://, s3://, memory://, etc.)
 * @param storage_opts  NULL-terminated key-value pairs ["k1","v1",NULL], or NULL
 * @param version       Version to open (0 = latest)
 * @return Dataset handle, or NULL on error
 */
LanceDataset* lance_dataset_open(
    const char* uri,
    const char* const* storage_opts,
    uint64_t version
);

/**
 * Open a Lance dataset using a shared session.
 *
 * The dataset retains the shared session state and remains valid if the caller
 * subsequently closes `session`.
 *
 * @param uri           Dataset path (file://, s3://, memory://, etc.)
 * @param storage_opts  NULL-terminated key-value pairs ["k1","v1",NULL], or NULL
 * @param version       Version to open (0 = latest)
 * @param session       Shared session; must not be NULL
 * @return Dataset handle, or NULL on error
 */
LanceDataset* lance_dataset_open_with_session(
    const char* uri,
    const char* const* storage_opts,
    uint64_t version,
    const LanceSession* session
);

/** Close and free a dataset handle. Safe to call with NULL. */
void lance_dataset_close(LanceDataset* dataset);

/* ─── Dataset metadata (sync, in-memory) ─── */

/**
 * Return the version number of this dataset snapshot.
 * @return version on success, or 0 on error (check lance_last_error_code())
 */
uint64_t lance_dataset_version(const LanceDataset* dataset);

/**
 * Return the number of rows. Returns 0 on error; an empty dataset also returns
 * 0, so check lance_last_error_code().
 */
uint64_t lance_dataset_count_rows(const LanceDataset* dataset);

/**
 * Return the number of rows matching an optional SQL filter.
 *
 * `filter == NULL` maps to native `Dataset::count_rows(None)` and uses the
 * metadata fast path. A non-NULL `filter`, including `""`, maps to
 * `Dataset::count_rows(Some(filter))` and therefore follows native SQL filter
 * parsing and error semantics.
 *
 * Returns 0 on error; an empty match also returns 0, so check
 * lance_last_error_code().
 */
uint64_t lance_dataset_count_rows_filtered(
    const LanceDataset* dataset,
    const char* filter
);

/**
 * Return the latest version ID (I/O), or 0 on error (check
 * lance_last_error_code()).
 */
uint64_t lance_dataset_latest_version(const LanceDataset* dataset);

/* ─── Dataset maintenance ─── */

/**
 * Copied native `RemovalStats`.
 *
 * `failed_deletes` counts files that cleanup attempted but could not remove.
 * Those files are not counted as removed and are found again by a later
 * cleanup. Cleanup does not roll back deletes that already succeeded when a
 * later file fails.
 */
typedef struct LanceRemovalStats {
    uint64_t bytes_removed;
    uint64_t old_versions;
    uint64_t data_files_removed;
    uint64_t transaction_files_removed;
    uint64_t index_files_removed;
    uint64_t deletion_files_removed;
    uint64_t failed_deletes;
} LanceRemovalStats;

/**
 * Native `CleanupFileKind` discriminants. Accessors return a fixed-width
 * int32_t carrying one of these values.
 */
typedef enum {
    LANCE_CLEANUP_FILE_MANIFEST           = 0,
    LANCE_CLEANUP_FILE_DATA               = 1,
    LANCE_CLEANUP_FILE_TRANSACTION        = 2,
    LANCE_CLEANUP_FILE_INDEX              = 3,
    LANCE_CLEANUP_FILE_DELETION           = 4,
    LANCE_CLEANUP_FILE_TEMPORARY_MANIFEST = 5,
} LanceCleanupFileKind;

/**
 * Native manual cleanup policy with explicit presence for every optional
 * selector. Zero-initialize this struct to leave every optional field unset
 * and inherit native defaults. In particular, the native default
 * `error_if_tagged_old_versions` is true; it is not the legacy writer's
 * automatic 20-version / 14-day policy.
 *
 * `before_timestamp`, `before_version` and `versions` are independent
 * selectors. When more than one is set, a version is eligible only if it
 * satisfies all of them (AND).
 *
 * - `versions == NULL` means no exact-version selector. A non-NULL pointer
 *   with `num_versions == 0` is an explicit empty selection and Lance rejects
 *   it.
 * - Set `has_retain_versions` to use the native asynchronous
 *   `CleanupPolicyBuilder::retain_n_versions`; zero is rejected by Lance.
 *   Do not set both `has_before_version` and `has_retain_versions`, because
 *   both set the same native field.
 * - A present boolean may be true or false. The `has_*` field controls
 *   presence independently, so zero-initialized optional values preserve
 *   native defaults.
 * - Timestamps use seconds plus a nanosecond component. They are not narrowed
 *   to the signed 64-bit nanosecond range.
 *
 * The policy and `versions` array are copied before asynchronous work.
 */
typedef struct LanceCleanupPolicy {
    int32_t     has_before_timestamp;
    int64_t     before_timestamp_seconds;
    uint32_t    before_timestamp_nanos;
    int32_t     has_before_version;
    uint64_t    before_version;
    const uint64_t* versions;
    size_t      num_versions;
    int32_t     has_retain_versions;
    uint64_t    retain_versions;
    int32_t     has_delete_unverified;
    bool        delete_unverified;
    int32_t     has_error_if_tagged_old_versions;
    bool        error_if_tagged_old_versions;
    int32_t     has_clean_referenced_branches;
    bool        clean_referenced_branches;
    int32_t     has_delete_rate_limit;
    uint64_t    delete_rate_limit;
} LanceCleanupPolicy;

/**
 * Optional native `CleanupOperation` overrides. Zero-initialize to use the
 * native operation defaults.
 *
 * An operation-level delete rate limit overrides the policy-level value.
 * `max_candidate_files` affects explanations only; zero is valid and means
 * include no candidate files while still returning aggregate stats and a
 * truncation flag.
 */
typedef struct LanceCleanupOperationOptions {
    int32_t  has_delete_concurrency;
    uint64_t delete_concurrency;
    int32_t  has_delete_rate_limit;
    uint64_t delete_rate_limit;
    int32_t  has_max_candidate_files;
    uint64_t max_candidate_files;
} LanceCleanupOperationOptions;

/**
 * Relative-duration cleanup options for
 * `lance_dataset_cleanup_old_versions`.
 *
 * `older_than_seconds + older_than_nanos / 1000000000` is converted to the
 * native `chrono::TimeDelta` range without narrowing through signed 64-bit
 * nanoseconds. Both components carry the sign; `older_than_nanos` must be in
 * [-999999999, 999999999]. This entry point calls
 * `Dataset::cleanup_old_versions`, so Lance owns the time cutoff. The optional
 * booleans use independent presence flags; leaving them unset preserves native
 * defaults.
 */
typedef struct LanceCleanupOldVersionsOptions {
    int64_t  older_than_seconds;
    int32_t  older_than_nanos;
    int32_t  has_delete_unverified;
    bool     delete_unverified;
    int32_t  has_error_if_tagged_old_versions;
    bool     error_if_tagged_old_versions;
} LanceCleanupOldVersionsOptions;

/**
 * Destructively clean versions older than a relative duration using native
 * `Dataset::cleanup_old_versions`.
 *
 * The current version, tagged versions, referenced branch state and
 * unverified-file protection follow Lance core. Deletes already performed are
 * not rolled back if a later delete fails; inspect `failed_deletes`. The
 * function has no lease or ownership relationship with historical snapshot
 * handles, and removed files are no longer available to them.
 *
 * @param dataset   Open dataset. Must not be NULL.
 * @param options   Relative window and optional native flags. Must not be NULL.
 * @param out_stats Receives copied native stats on success only.
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_cleanup_old_versions(
    const LanceDataset* dataset,
    const LanceCleanupOldVersionsOptions* options,
    LanceRemovalStats* out_stats
);

/**
 * Execute manual cleanup with an explicit native policy.
 *
 * Cleanup re-evaluates current dataset references and storage state. It
 * protects the current version, tags and referenced branches according to
 * Lance core. Deletes already performed are not rolled back when a later
 * delete fails; `failed_deletes` reports failures.
 *
 * @param dataset           Open dataset. Must not be NULL.
 * @param policy            Native cleanup policy. Must not be NULL.
 * @param operation_options Optional operation overrides; NULL uses defaults.
 * @param out_stats         Receives copied native stats on success only.
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_cleanup(
    const LanceDataset* dataset,
    const LanceCleanupPolicy* policy,
    const LanceCleanupOperationOptions* operation_options,
    LanceRemovalStats* out_stats
);

/**
 * Explain manual cleanup without deleting files.
 *
 * Execution state is re-evaluated by a later
 * `lance_dataset_cleanup`/`lance_dataset_cleanup_old_versions` call, so an
 * explanation is not a deletion plan or a reservation.
 *
 * @param dataset           Open dataset. Must not be NULL.
 * @param policy            Native cleanup policy. Must not be NULL.
 * @param operation_options Optional operation overrides; NULL uses defaults.
 * @return Owned explanation snapshot, or NULL on error. Close it with
 *         `lance_cleanup_explanation_close`. All strings borrowed from it
 *         remain valid until close.
 */
LanceCleanupExplanation* lance_dataset_cleanup_explain(
    const LanceDataset* dataset,
    const LanceCleanupPolicy* policy,
    const LanceCleanupOperationOptions* operation_options
);

/** Dataset version observed when the explanation was produced. */
uint64_t lance_cleanup_explanation_read_version(
    const LanceCleanupExplanation* explanation
);

/**
 * Copy aggregate removal stats from an explanation. The output is written
 * only on success.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_cleanup_explanation_stats(
    const LanceCleanupExplanation* explanation,
    LanceRemovalStats* out_stats
);

/** Number of candidate files included in the explanation. */
uint64_t lance_cleanup_explanation_candidate_count(
    const LanceCleanupExplanation* explanation
);

/**
 * Whether more candidate files were found than included.
 * @return 1 or 0 on success, -1 on error.
 */
int32_t lance_cleanup_explanation_candidate_files_truncated(
    const LanceCleanupExplanation* explanation
);

/** Native maximum candidate-file count used by the explanation. */
uint64_t lance_cleanup_explanation_candidate_file_limit(
    const LanceCleanupExplanation* explanation
);

/** Candidate path at `index`; owned by `explanation`. NULL on error. */
const char* lance_cleanup_explanation_candidate_path_at(
    const LanceCleanupExplanation* explanation,
    size_t index
);

/**
 * Candidate `LanceCleanupFileKind` at `index`.
 * @return the fixed-width discriminant, or -1 on error.
 */
int32_t lance_cleanup_explanation_candidate_kind_at(
    const LanceCleanupExplanation* explanation,
    size_t index
);

/**
 * Candidate unverified flag at `index`.
 * @return 1 or 0 on success, -1 on error.
 */
int32_t lance_cleanup_explanation_candidate_unverified_at(
    const LanceCleanupExplanation* explanation,
    size_t index
);

/** Candidate file size in bytes at `index`; 0 on error (check error code). */
uint64_t lance_cleanup_explanation_candidate_size_bytes_at(
    const LanceCleanupExplanation* explanation,
    size_t index
);

/** Number of referenced child branches in the explanation. */
uint64_t lance_cleanup_explanation_referenced_branch_count(
    const LanceCleanupExplanation* explanation
);

/** Referenced branch name at `index`; owned by `explanation`. NULL on error. */
const char* lance_cleanup_explanation_referenced_branch_name_at(
    const LanceCleanupExplanation* explanation,
    size_t index
);

/** Referenced version at `index`; 0 on error (check error code). */
uint64_t lance_cleanup_explanation_referenced_branch_version_at(
    const LanceCleanupExplanation* explanation,
    size_t index
);

/**
 * Whether the referenced branch at `index` is a cascade cleanup candidate.
 * @return 1 or 0 on success, -1 on error.
 */
int32_t lance_cleanup_explanation_referenced_branch_cleanup_candidate_at(
    const LanceCleanupExplanation* explanation,
    size_t index
);

/** Number of non-fatal warnings in the explanation. */
uint64_t lance_cleanup_explanation_warning_count(
    const LanceCleanupExplanation* explanation
);

/** Warning at `index`; owned by `explanation`. NULL on error. */
const char* lance_cleanup_explanation_warning_at(
    const LanceCleanupExplanation* explanation,
    size_t index
);

/** Close an explanation snapshot. Safe to call with NULL. */
void lance_cleanup_explanation_close(LanceCleanupExplanation* explanation);

/**
 * Validate the dataset's fragments, stable row IDs and indices.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_validate(const LanceDataset* dataset);

/**
 * Count rows hidden by deletion files across all fragments.
 *
 * @param dataset  Open dataset. Must not be NULL.
 * @param out_rows Receives the native count on success only.
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_count_deleted_rows(
    const LanceDataset* dataset,
    uint64_t* out_rows
);

/**
 * Count fragments with fewer physical rows than `max_rows_per_group`.
 *
 * This follows the infallible native contract: in particular,
 * `max_rows_per_group == 0` returns 0. The output is written on success only.
 *
 * @return 0 on success, -1 on binding input error.
 */
int32_t lance_dataset_num_small_files(
    const LanceDataset* dataset,
    size_t max_rows_per_group,
    uint64_t* out_count
);

/* ─── Dataset table and schema metadata ─── */

/** How a metadata update applies to the existing map. */
typedef enum {
    /* Merge entries into the existing map. */
    LANCE_METADATA_UPDATE_MERGE   = 0,
    /* Replace the existing map with the entries that carry values. */
    LANCE_METADATA_UPDATE_REPLACE = 1,
} LanceMetadataUpdateMode;

/**
 * Return this snapshot's table metadata as a JSON object mapping keys to
 * string values. Keys and values are JSON-escaped. An empty map is `{}`.
 *
 * Schema metadata is part of the Arrow schema returned by
 * `lance_dataset_schema()` and is not duplicated here.
 *
 * Caller must free the returned string with `lance_free_string()`.
 * Returns NULL on error.
 */
const char* lance_dataset_metadata_json(const LanceDataset* dataset);

/**
 * Merge or replace table metadata and return the complete updated map as a
 * JSON object.
 *
 * `keys` and `values` are parallel arrays of length `count`.
 * - `keys[i]` must be a non-NULL, NUL-terminated UTF-8 string.
 * - `values[i] == NULL` removes `keys[i]`, matching the native `None` value.
 *   It does not store an empty value.
 * - An empty string value is a real value and is stored as `""`.
 * - `keys` and `values` may be NULL only when `count == 0`.
 *
 * `mode` is explicit:
 * - `LANCE_METADATA_UPDATE_MERGE` applies the entries to the existing map.
 * - `LANCE_METADATA_UPDATE_REPLACE` replaces the entire map. With
 *   `count == 0`, this clears the map; value-less entries are omitted.
 *
 * The native builder performs the commit. This mutates `dataset` in place;
 * the same handle remains valid and sees the new version, while existing
 * dataset snapshots and scanners keep their prior view. Key validation,
 * version transitions, concurrent transaction behavior and error semantics
 * follow the pinned Lance core.
 *
 * Caller must free the returned JSON string with `lance_free_string()`.
 * Returns NULL on error. Commit and error semantics follow the native core.
 */
const char* lance_dataset_update_metadata_json(
    LanceDataset* dataset,
    const char* const* keys,
    const char* const* values,
    size_t count,
    LanceMetadataUpdateMode mode
);

/**
 * Merge or replace schema metadata and return the complete updated schema
 * metadata map as a JSON object. Input and ownership rules match
 * `lance_dataset_update_metadata_json`.
 *
 * This changes only schema metadata: dataset fields, fragments, table
 * metadata and configuration are unchanged. Read the committed schema
 * metadata through the Arrow schema returned by `lance_dataset_schema()`.
 *
 * Caller must free the returned JSON string with `lance_free_string()`.
 * Returns NULL on error. Commit and error semantics follow the native core.
 */
const char* lance_dataset_update_schema_metadata_json(
    LanceDataset* dataset,
    const char* const* keys,
    const char* const* values,
    size_t count,
    LanceMetadataUpdateMode mode
);

/** Selects a field by numeric ID or by Lance field path. */
typedef enum {
    LANCE_FIELD_REFERENCE_ID   = 0,
    LANCE_FIELD_REFERENCE_PATH = 1,
} LanceFieldReferenceKind;

/**
 * A field reference for `lance_dataset_update_field_metadata` and
 * `lance_dataset_resolve_field_id`.
 *
 * For `LANCE_FIELD_REFERENCE_ID`, `field_id` is used and `path` is ignored.
 * For `LANCE_FIELD_REFERENCE_PATH`, `path` must be a non-NULL, NUL-terminated
 * UTF-8 Lance field path (for example `profile.name`) and `field_id` is
 * ignored.
 */
typedef struct LanceFieldReference {
    LanceFieldReferenceKind kind;
    int32_t field_id;
    const char* path;
} LanceFieldReference;

/**
 * One field's metadata update in a batch.
 *
 * `keys` and `values` are parallel arrays of length `count`. A NULL value
 * removes its key; an empty string remains a real empty value. `mode` is
 * applied independently to this field. `keys` and `values` may be NULL only
 * when `count == 0`.
 */
typedef struct LanceFieldMetadataUpdate {
    LanceFieldReference field;
    const char* const* keys;
    const char* const* values;
    size_t count;
    LanceMetadataUpdateMode mode;
} LanceFieldMetadataUpdate;

/**
 * Apply a batch of field metadata updates and commit one native operation.
 *
 * Each `updates[i]` is resolved against the dataset's current schema when the
 * call starts. A batch may mix field references, merge/replace modes and
 * fields. If the same field is referenced more than once, the native builder's
 * last update wins.
 *
 * The input array, keys, values and paths are borrowed only for this call and
 * are copied before the native asynchronous operation starts. The native
 * builder performs the commit and validation. The same dataset handle sees the
 * new version; existing snapshots, scanners and independent handles keep their
 * prior view. Read the resulting field metadata through the Arrow schema
 * returned by `lance_dataset_schema()`.
 *
 * Returns 0 on success, -1 on error. Input, field lookup, validation and commit
 * errors follow the native Lance core.
 */
int32_t lance_dataset_update_field_metadata(
    LanceDataset* dataset,
    const LanceFieldMetadataUpdate* updates,
    size_t update_count
);

/**
 * Return the current snapshot's canonical Lance data-storage version string
 * (for example `"2.2"`). The value is the native manifest file-format version.
 *
 * Caller must free the returned string with `lance_free_string()`.
 * Returns NULL on error.
 */
const char* lance_dataset_data_storage_version(const LanceDataset* dataset);

/**
 * Report whether the current snapshot uses stable row IDs.
 * On success writes `*out_has_stable_row_ids` and returns 0; on error returns
 * -1 and leaves the output untouched.
 */
int32_t lance_dataset_has_stable_row_ids(
    const LanceDataset* dataset,
    bool* out_has_stable_row_ids
);

/**
 * Return the number of field IDs in the current snapshot's schema. IDs are
 * available in native pre-order traversal through `lance_dataset_field_id_at`.
 * On success writes `*out_count` and returns 0; on error returns -1 and leaves
 * the output untouched.
 */
int32_t lance_dataset_field_id_count(
    const LanceDataset* dataset,
    size_t* out_count
);

/**
 * Return the field ID at `index` in native pre-order traversal. Index 0 is the
 * first top-level field; nested children follow their parent.
 * On success writes `*out_field_id` and returns 0; on error returns -1 and
 * leaves the output untouched.
 */
int32_t lance_dataset_field_id_at(
    const LanceDataset* dataset,
    size_t index,
    int32_t* out_field_id
);

/**
 * Resolve a field reference by ID or path against the current snapshot.
 * Native `FieldRef` validation supplies the error for an unknown ID or path.
 * On success writes `*out_field_id` and returns 0; on error returns -1 and
 * leaves the output untouched.
 */
int32_t lance_dataset_resolve_field_id(
    const LanceDataset* dataset,
    const LanceFieldReference* field,
    int32_t* out_field_id
);

/**
 * Return the native minimal Lance field path for `field_id` in the current
 * schema. The path round-trips through Lance field-path parsing and includes
 * nested fields. Caller must free the returned string with
 * `lance_free_string()`. Returns NULL on error, including an unknown ID.
 */
const char* lance_dataset_field_path(
    const LanceDataset* dataset,
    int32_t field_id
);

/* ─── Version history ─── */

/**
 * Snapshot the current dataset version, including its native metadata summary.
 * Caller frees the returned handle with lance_version_close(). The snapshot is
 * copied and remains valid after the dataset is closed.
 * @return handle on success, or NULL on error
 */
LanceVersion* lance_dataset_current_version(const LanceDataset* dataset);

/**
 * Version id from a current-version or history snapshot.
 * Returns 0 on error (check lance_last_error_code()).
 */
uint64_t lance_version_id(const LanceVersion* version);

/**
 * Commit timestamp as Unix epoch nanoseconds. Returns 0 on error (check
 * lance_last_error_code()).
 *
 * The i64 nanosecond representation covers 1677-09-21 through 2262-04-11 UTC.
 * A timestamp outside that range makes this accessor return 0 with
 * LANCE_ERR_INVALID_ARGUMENT. Snapshot creation and millisecond reads retain
 * the native timestamp range.
 */
int64_t lance_version_timestamp_ns(const LanceVersion* version);

/**
 * Number of version metadata entries. Native version metadata is the manifest
 * summary map; it is empty only when the native map is empty. Returns 0 on
 * error, so check lance_last_error_code().
 */
uint64_t lance_version_metadata_count(const LanceVersion* version);

/**
 * Metadata key at `index`, borrowed until the version handle is closed.
 * Returns NULL on error (NULL handle or out-of-range index).
 */
const char* lance_version_metadata_key_at(
    const LanceVersion* version,
    size_t index
);

/**
 * Metadata value at `index`, borrowed until the version handle is closed.
 * Returns NULL on error (NULL handle or out-of-range index).
 */
const char* lance_version_metadata_value_at(
    const LanceVersion* version,
    size_t index
);

/** Close and free a current-version snapshot. Safe to call with NULL. */
void lance_version_close(LanceVersion* version);

/**
 * Snapshot the dataset's version history. Caller frees the returned handle
 * with lance_versions_close(). The copied snapshot remains valid after the
 * dataset is closed. Entries are ordered by version id.
 * @return handle on success, or NULL on error
 */
LanceVersions* lance_dataset_versions(const LanceDataset* dataset);

/**
 * Number of versions in the snapshot, or 0 on error (check
 * lance_last_error_code()).
 */
uint64_t lance_versions_count(const LanceVersions* versions);

/**
 * Borrowed version details at `index` (0 <= index < count), or NULL on error.
 * The pointer remains valid until lance_versions_close().
 */
const LanceVersion* lance_versions_at(const LanceVersions* versions, size_t index);

/**
 * Monotonic version id at `index` (0 <= index < count).
 * Returns 0 on error (NULL handle or out-of-range index) — check
 * lance_last_error_code().
 */
uint64_t lance_versions_id_at(const LanceVersions* versions, size_t index);

/**
 * Version timestamp at `index`, as Unix epoch milliseconds.
 * Returns 0 on error (NULL handle or out-of-range index) — check
 * lance_last_error_code().
 */
int64_t lance_versions_timestamp_ms_at(const LanceVersions* versions, size_t index);

/** Close and free a versions handle. Safe to call with NULL. */
void lance_versions_close(LanceVersions* versions);

/**
 * Snapshot the lightweight attached-version references. Unlike full version
 * details, this only enumerates the attached manifest locations. Caller frees
 * with lance_version_refs_close(). The snapshot remains valid after the dataset
 * is closed. Entries are ordered by version id.
 * @return handle on success, or NULL on error
 */
LanceVersionRefs* lance_dataset_version_refs(const LanceDataset* dataset);

/** Number of lightweight version refs, or 0 on error. */
uint64_t lance_version_refs_count(const LanceVersionRefs* refs);

/**
 * Version id at `index`, or 0 on error (NULL handle or out-of-range index).
 * Check lance_last_error_code() to distinguish an error.
 */
uint64_t lance_version_refs_id_at(
    const LanceVersionRefs* refs,
    size_t index
);

/** Close and free a lightweight version-ref snapshot. Safe to call with NULL. */
void lance_version_refs_close(LanceVersionRefs* refs);

/* ─── Native references and checkout ─── */

/** Construct Ref::VersionNumber(version). Caller frees with lance_ref_close(). */
LanceRef* lance_ref_version_number(uint64_t version);

/**
 * Construct the native Ref::Version variant.
 *
 * `branch == NULL` maps to None. `has_version == false` maps the version to
 * None; otherwise `version` is used. The native tuple conversion normalizes
 * "main" to the default branch. No C-side validation policy is applied here;
 * the native operation that consumes the ref validates it.
 */
LanceRef* lance_ref_version(
    const char* branch,
    bool has_version,
    uint64_t version
);

/**
 * Construct Ref::Tag(tag_name). A NULL name is rejected; an empty name is
 * passed through to the native operation for its normal validation.
 */
LanceRef* lance_ref_tag(const char* tag_name);

/** Close and free a native reference. Safe to call with NULL. */
void lance_ref_close(LanceRef* reference);

/**
 * Check out a native Ref as a new dataset handle.
 *
 * The source dataset and reference are unchanged. The returned handle owns an
 * independent snapshot and is usable after the source dataset is closed.
 * Caller closes it with lance_dataset_close().
 * @return new dataset handle, or NULL on error
 */
LanceDataset* lance_dataset_checkout_ref(
    const LanceDataset* dataset,
    const LanceRef* reference
);

/* ─── Branches ─── */

/**
 * Create a branch from a native ref.
 *
 * `branch_name` must be UTF-8. `reference` must be an existing LanceRef.
 * Branch-name validation, reference resolution and duplicate detection are
 * performed by Lance's `Dataset::create_branch`. Reusing an existing branch
 * name may fail in the native shallow-clone phase instead of reaching the
 * BranchContents conflict check; that native error is passed through rather
 * than replaced with a synthesized conflict.
 *
 * `storage_opts` is NULL-terminated key-value pairs, or NULL. NULL passes
 * `None` to native, so the source dataset's store params are used. A non-NULL
 * array, including an immediately NULL-terminated empty array, passes explicit
 * object-store params.
 *
 * Native creation is a non-atomic shallow clone followed by a BranchContents
 * write. If creation stops after the clone, a branch dataset without
 * BranchContents may remain. Retrying the same name may then fail; use
 * lance_dataset_branch_force_delete() to remove that zombie dataset.
 *
 * The returned dataset owns an independent snapshot and remains usable after
 * the source dataset or reference is closed.
 * @return new dataset handle, or NULL on error
 */
LanceDataset* lance_dataset_branch_create(
    const LanceDataset* dataset,
    const char* branch_name,
    const LanceRef* reference,
    const char* const* storage_opts
);

/**
 * Check out the latest version of a branch as an independent dataset.
 *
 * "main" selects the default branch. The returned handle remains usable after
 * the source dataset is closed.
 * @return new dataset handle, or NULL on error
 */
LanceDataset* lance_dataset_checkout_branch(
    const LanceDataset* dataset,
    const char* branch_name
);

/**
 * Serialize all branches as a JSON object keyed by branch name.
 *
 * The value is Lance's native `HashMap<String, BranchContents>` serde output.
 * Each value has the selected core's BranchContents shape, including
 * `parentBranch`, `identifier.version_mapping`, `parentVersion`, `createAt`
 * (Unix seconds), `manifestSize` and `metadata`. Map enumeration order is not
 * promised. The caller owns the string and frees it with
 * lance_free_string().
 * @return JSON string, or NULL on error
 */
const char* lance_dataset_branches_json(const LanceDataset* dataset);

/**
 * Serialize one native BranchContents as JSON.
 *
 * The object follows the selected core's BranchContents serde shape. The
 * caller owns the string and frees it with lance_free_string().
 * @return JSON string, or NULL on error (including LANCE_ERR_NOT_FOUND)
 */
const char* lance_dataset_branch_get_json(
    const LanceDataset* dataset,
    const char* branch_name
);

/**
 * Serialize the current dataset's native BranchIdentifier as JSON.
 *
 * The object follows the selected core's BranchIdentifier serde shape:
 * `{"version_mapping":[[version,uuid],...]}`. For the main branch this is
 * empty. The caller owns the string and frees it with lance_free_string().
 * @return JSON string, or NULL on error
 */
const char* lance_dataset_branch_identifier_json(const LanceDataset* dataset);

/**
 * Delete a branch.
 *
 * Native checks reject a branch referenced by a child branch or by any tag
 * with LANCE_ERR_COMMIT_CONFLICT. A missing BranchContents produces
 * LANCE_ERR_NOT_FOUND. The branch dataset directories are cleaned up by the
 * native implementation.
 * @return 0 on success, -1 on error
 */
int32_t lance_dataset_branch_delete(
    const LanceDataset* dataset,
    const char* branch_name
);

/**
 * Delete a branch, bypassing only a missing BranchContents.
 *
 * This is the native recovery path for a branch dataset left without
 * BranchContents by the non-atomic creation sequence. Child-branch and tag
 * reference checks still apply and surface as LANCE_ERR_COMMIT_CONFLICT.
 * @return 0 on success, -1 on error
 */
int32_t lance_dataset_branch_force_delete(
    const LanceDataset* dataset,
    const char* branch_name
);

/**
 * Replace all metadata entries on an existing branch.
 *
 * `metadata` is NULL-terminated key-value pairs ["key","value",NULL]. NULL or
 * an empty array means an empty native map. This is a full replacement of
 * BranchContents.metadata, not a merge or a special NULL-delete operation.
 * Other branch fields are preserved. An unknown branch surfaces as
 * LANCE_ERR_NOT_FOUND.
 * @return 0 on success, -1 on error
 */
int32_t lance_dataset_branch_replace_metadata(
    const LanceDataset* dataset,
    const char* branch_name,
    const char* const* metadata
);

/* ─── Tags ─── */

/**
 * Snapshot all tags. Caller frees with lance_tags_close(). The copied snapshot
 * remains valid after the dataset is closed. Entry order is unspecified.
 * @return handle on success, or NULL on error
 */
LanceTags* lance_dataset_tags(const LanceDataset* dataset);

/** Number of tags, or 0 on error (check lance_last_error_code()). */
uint64_t lance_tags_count(const LanceTags* tags);

/**
 * Borrowed tag at `index` (0 <= index < count), or NULL on error. The pointer
 * remains valid until lance_tags_close().
 */
const LanceTag* lance_tags_at(const LanceTags* tags, size_t index);

/** Close and free a tag-list snapshot. Safe to call with NULL. */
void lance_tags_close(LanceTags* tags);

/**
 * Fetch one tag by name. Caller frees with lance_tag_close().
 * @return tag handle, or NULL on error
 */
LanceTag* lance_dataset_tag_get(
    const LanceDataset* dataset,
    const char* tag_name
);

/**
 * Create a tag from a native ref.
 *
 * Target resolution, name validation, version lookup and duplicate detection
 * are performed by the native Tags::create implementation. A duplicate tag
 * surfaces as LANCE_ERR_COMMIT_CONFLICT; an unknown version surfaces as
 * LANCE_ERR_NOT_FOUND.
 * @return 0 on success, -1 on error
 */
int32_t lance_dataset_tag_create(
    const LanceDataset* dataset,
    const char* tag_name,
    const LanceRef* reference
);

/**
 * Update an existing tag to a native ref. The native implementation preserves
 * metadata while updating the target and timestamps.
 * @return 0 on success, -1 on error (including LANCE_ERR_NOT_FOUND)
 */
int32_t lance_dataset_tag_update(
    const LanceDataset* dataset,
    const char* tag_name,
    const LanceRef* reference
);

/**
 * Delete a tag. An unknown tag surfaces as LANCE_ERR_NOT_FOUND.
 * @return 0 on success, -1 on error
 */
int32_t lance_dataset_tag_delete(
    const LanceDataset* dataset,
    const char* tag_name
);

/**
 * Replace all metadata entries on an existing tag.
 *
 * `metadata` is NULL-terminated key-value pairs ["key","value",NULL]. NULL
 * means an empty map. Native storage serializes missing metadata as an empty
 * map on read, so absent and empty metadata are observationally equivalent.
 * @return 0 on success, -1 on error (including LANCE_ERR_NOT_FOUND)
 */
int32_t lance_dataset_tag_replace_metadata(
    const LanceDataset* dataset,
    const char* tag_name,
    const char* const* metadata
);

/** Tag name, borrowed until the tag handle is closed. NULL on error. */
const char* lance_tag_name(const LanceTag* tag);

/** Tagged version id, or 0 on error. */
uint64_t lance_tag_version(const LanceTag* tag);

/**
 * Report the tag's optional branch.
 *
 * Returns 1 and writes a borrowed branch when present, 0 and writes NULL when
 * absent, or -1 on error. The branch remains valid until the tag is closed.
 */
int32_t lance_tag_branch(
    const LanceTag* tag,
    const char** out_branch
);

/**
 * Report the optional creation timestamp as Unix epoch nanoseconds.
 * Returns 1 when present, 0 when absent, or -1 on error.
 * A present timestamp outside the i64 nanosecond range returns -1 with
 * LANCE_ERR_INVALID_ARGUMENT. On absence or error, the output is unchanged.
 */
int32_t lance_tag_created_at_ns(
    const LanceTag* tag,
    int64_t* out_timestamp_ns
);

/**
 * Report the optional update timestamp as Unix epoch nanoseconds.
 * Returns 1 when present, 0 when absent, or -1 on error.
 * A present timestamp outside the i64 nanosecond range returns -1 with
 * LANCE_ERR_INVALID_ARGUMENT. On absence or error, the output is unchanged.
 */
int32_t lance_tag_updated_at_ns(
    const LanceTag* tag,
    int64_t* out_timestamp_ns
);

/** Manifest size recorded in the tag, or 0 on error. */
uint64_t lance_tag_manifest_size(const LanceTag* tag);

/**
 * Number of tag metadata entries, or 0 on error. Missing metadata and an empty
 * map both produce zero entries, matching native TagContents deserialization.
 */
uint64_t lance_tag_metadata_count(const LanceTag* tag);

/** Metadata key at `index`, borrowed until the tag is closed. NULL on error. */
const char* lance_tag_metadata_key_at(const LanceTag* tag, size_t index);

/** Metadata value at `index`, borrowed until the tag is closed. NULL on error. */
const char* lance_tag_metadata_value_at(const LanceTag* tag, size_t index);

/** Close and free one tag handle. Safe to call with NULL. */
void lance_tag_close(LanceTag* tag);

/* ─── Data statistics ─── */

/**
 * Compute per-field data statistics (compressed on-disk byte size) for query
 * planning. Walks every fragment, so this performs I/O. Caller frees the
 * returned handle with lance_data_statistics_close().
 *
 * Entries are ordered by schema field id, one per field (including nested
 * struct/list children).
 * @return handle on success, or NULL on error
 */
LanceDataStatistics* lance_dataset_calculate_data_stats(const LanceDataset* dataset);

/**
 * Number of fields in the statistics snapshot. Clears the thread-local error
 * on success. Returns 0 and sets LANCE_ERR_INVALID_ARGUMENT on a NULL handle;
 * a dataset with an empty schema also yields 0 with no error set, so check
 * lance_last_error_code() to distinguish the error case from an empty result.
 */
uint64_t lance_data_statistics_count(const LanceDataStatistics* stats);

/**
 * Schema field id at `index` (0 <= index < count).
 * Returns 0 on error (NULL handle or out-of-range index), setting
 * LANCE_ERR_INVALID_ARGUMENT. Because 0 is itself a valid field id, check
 * lance_last_error_code() when passing an untrusted index; iterating
 * `0..count` never errors.
 */
uint32_t lance_data_statistics_field_id_at(const LanceDataStatistics* stats, size_t index);

/**
 * Compressed on-disk byte size of the field at `index`.
 * Returns 0 on error (NULL handle or out-of-range index), setting
 * LANCE_ERR_INVALID_ARGUMENT. A field written with the legacy (v1) storage
 * format also reports 0 but sets no error, so check lance_last_error_code() to
 * distinguish a genuine 0 from the error sentinel.
 */
uint64_t lance_data_statistics_bytes_on_disk_at(const LanceDataStatistics* stats, size_t index);

/** Close and free a data statistics handle. Safe to call with NULL. */
void lance_data_statistics_close(LanceDataStatistics* stats);

/* ─── Dataset config ─── */

/**
 * Return a snapshot of the dataset's manifest config map.
 *
 * The snapshot owns copies of the keys and values, so it remains valid after
 * the dataset handle is closed. Accessors borrow from the snapshot and remain
 * valid until lance_dataset_config_close().
 *
 * Returns NULL on error (for example a NULL dataset handle).
 *
 * Keys and values are exposed as NUL-terminated C strings. A config entry
 * containing an embedded NUL byte cannot be represented and fails the
 * snapshot with LANCE_ERR_INTERNAL. Entry order is unspecified.
 */
LanceDatasetConfig* lance_dataset_config(const LanceDataset* dataset);

/**
 * Return the number of entries in the config snapshot. Returns 0 and sets
 * LANCE_ERR_INVALID_ARGUMENT on a NULL handle. An empty config also yields 0
 * with no error set, so check lance_last_error_code() to distinguish the two.
 */
uint64_t lance_dataset_config_count(const LanceDatasetConfig* config);

/**
 * Return the key at `index` (0 <= index < count). The pointer is borrowed from
 * the snapshot. Returns NULL and sets LANCE_ERR_INVALID_ARGUMENT on a NULL
 * handle or out-of-range index.
 */
const char* lance_dataset_config_key_at(
    const LanceDatasetConfig* config,
    size_t index
);

/**
 * Return the value at `index` (0 <= index < count). The pointer is borrowed
 * from the snapshot. Returns NULL and sets LANCE_ERR_INVALID_ARGUMENT on a
 * NULL handle or out-of-range index.
 */
const char* lance_dataset_config_value_at(
    const LanceDatasetConfig* config,
    size_t index
);

/** Close and free a dataset config snapshot. Safe to call with NULL. */
void lance_dataset_config_close(LanceDatasetConfig* config);

/**
 * Apply merge-style dataset config updates and commit a new manifest.
 *
 * `keys` and `values` are parallel arrays of length `num_entries`.
 * A non-NULL `values[i]` upserts `keys[i]`; a NULL `values[i]` removes it.
 * This maps directly to the native `Dataset::update_config` merge operation.
 * An entry count of 0 is passed through as an empty native update.
 *
 * When `num_entries > 0`, both arrays must be non-NULL and every `keys[i]`
 * must be a non-NULL C string. Empty keys and empty string values are passed
 * through unchanged, matching native map semantics.
 *
 * On success the dataset handle points at the newly committed version.
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_update_config(
    LanceDataset* dataset,
    const char* const* keys,
    const char* const* values,
    size_t num_entries
);

/**
 * Merge or replace dataset config and return the complete updated map as a
 * JSON object.
 *
 * `keys` and `values` are parallel arrays of length `count`.
 * - `keys[i]` must be a non-NULL, NUL-terminated UTF-8 string.
 * - `values[i] == NULL` removes `keys[i]`.
 * - An empty string value is a real value and is stored as `""`.
 * - `keys` and `values` may be NULL only when `count == 0`.
 *
 * `mode` is explicit:
 * - `LANCE_METADATA_UPDATE_MERGE` applies the entries to the existing map.
 * - `LANCE_METADATA_UPDATE_REPLACE` replaces the entire map. With
 *   `count == 0`, this clears the map; value-less entries are omitted.
 *
 * Duplicate keys in one call are forwarded in input order and follow the
 * native builder's last-write-wins behavior. The native builder commits one
 * transaction and its complete resulting map is serialized as JSON. This
 * mutates `dataset` in place; other dataset handles and existing streams keep
 * their prior snapshot view.
 *
 * Caller must free the returned JSON string with `lance_free_string()`.
 * Returns NULL on error.
 */
const char* lance_dataset_update_config_json(
    LanceDataset* dataset,
    const char* const* keys,
    const char* const* values,
    size_t count,
    LanceMetadataUpdateMode mode
);

/* ─── Dataset base paths ─── */

/**
 * Return the current snapshot's native base-path registry as a JSON object
 * keyed by decimal base ID.
 *
 * Each value contains exactly the native fields:
 * - `id`: unsigned base ID;
 * - `name`: string or JSON null;
 * - `path`: full URI string;
 * - `is_dataset_root`: boolean.
 *
 * The snapshot owns copied strings and remains independent of later dataset
 * versions. Caller must free the returned string with `lance_free_string()`.
 * Returns NULL on error.
 */
const char* lance_dataset_base_paths_json(const LanceDataset* dataset);

/**
 * One native `BasePath` field set for registration.
 *
 * `name == NULL` maps to native `None`; a non-NULL pointer, including an empty
 * string, maps to `Some(name)`. `id == 0` requests native ID assignment.
 */
typedef struct LanceBasePath {
    uint32_t id;
    const char* path;
    const char* name;
    bool is_dataset_root;
} LanceBasePath;

/**
 * Borrowed native `BasePath` fields owned by an operation handle.
 *
 * `path` and `name` point into the operation's storage and remain valid until
 * `lance_operation_close()` is called on that handle. `name == NULL` represents
 * native `None`; a non-NULL pointer, including an empty string, represents
 * `Some(name)`.
 */
typedef struct LanceBasePathView {
    uint32_t id;
    const char* path;
    const char* name;
    bool is_dataset_root;
} LanceBasePathView;

/**
 * Optional transaction properties for one native `add_bases` commit.
 *
 * `keys` and `values` are parallel arrays of length `count`; every key and
 * value must be a non-NULL C string. Empty strings are values. Duplicate keys
 * follow native `HashMap` last-write-wins behavior.
 */
typedef struct LanceTransactionProperties {
    const char* const* keys;
    const char* const* values;
    size_t count;
} LanceTransactionProperties;

/**
 * Register new base paths and commit one native `Dataset::add_bases`
 * transaction.
 *
 * `bases` may be NULL when `base_count == 0`. An empty list is passed to
 * the native transaction builder and follows its commit behavior.
 *
 * `transaction_properties == NULL` maps to native `None`. A non-NULL pointer
 * with `count == 0` maps to native `Some(empty map)`, so the caller's explicit
 * empty selection is preserved at this boundary. Native handling determines
 * how that empty map is persisted.
 *
 * Inputs are copied before the asynchronous operation starts. ID allocation,
 * duplicate/conflict checks, persistence and path resolution follow Lance
 * core. On success the same dataset handle sees the new version; existing
 * snapshots and streams keep their prior view.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_add_bases(
    LanceDataset* dataset,
    const LanceBasePath* bases,
    size_t base_count,
    const LanceTransactionProperties* transaction_properties
);

/* ─── Dataset file enumeration ─── */

/**
 * Tracked-files progress callback.
 *
 * `manifests_total` is NULL when the native total is not yet known. Otherwise
 * it points to a value borrowed for the duration of this call only. Progress
 * callbacks run on a native background task and must not block or unwind.
 */
typedef void (*LanceTrackedFilesProgressCallback)(
    void* callback_ctx,
    uint64_t manifests_processed,
    const uint64_t* manifests_total
);

/**
 * Release a tracked-files progress context.
 *
 * The library invokes this exactly once after the native emitter releases the
 * progress closure. It may run on a native worker after the exported stream
 * was released or after the stream factory returned an error. Must not unwind.
 */
typedef void (*LanceTrackedFilesProgressRelease)(void* callback_ctx);

/**
 * Foreign progress callback and owned context for tracked-file enumeration.
 *
 * When passed to `lance_dataset_tracked_files_stream`, both function pointers
 * must be non-NULL. Ownership of `callback_ctx` transfers to the library
 * before any other validation. `release(callback_ctx)` is called exactly once,
 * including on factory errors and early stream release. Until release runs,
 * the callback and context must remain valid. The stream's own `release`
 * callback may return before an in-flight native progress callback finishes.
 */
typedef struct LanceTrackedFilesProgress {
    LanceTrackedFilesProgressCallback callback;
    LanceTrackedFilesProgressRelease release;
    void* callback_ctx;
} LanceTrackedFilesProgress;

/**
 * Options for `lance_dataset_tracked_files_stream`.
 *
 * A NULL options pointer uses native defaults. A non-zero `has_min_version`
 * forwards `min_version`, including zero. `progress == NULL` disables
 * progress reporting.
 */
typedef struct LanceTrackedFilesOptions {
    int32_t has_min_version;
    uint64_t min_version;
    const LanceTrackedFilesProgress* progress;
} LanceTrackedFilesOptions;

/**
 * Export `Dataset::tracked_files_with_options` as an Arrow C stream.
 *
 * The native schema, dictionary/timestamp types, values, optional
 * `min_version` filtering and unspecified row order are preserved. Tracked
 * files follow native manifest walking; this is not a physical file listing.
 * The stream owns its resources and remains valid after the source dataset is
 * closed.
 *
 * `out` is caller-owned, must be zero-initialized by convention, and is
 * written only on success. Invoke its non-NULL `release` callback exactly once.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_tracked_files_stream(
    const LanceDataset* dataset,
    const LanceTrackedFilesOptions* options,
    struct ArrowArrayStream* out
);

/**
 * Export `Dataset::all_files` as an Arrow C stream.
 *
 * This lists only the primary dataset root. Registered additional base paths
 * are not scanned, and untracked files physically present under the primary
 * root are included. The native schema, values and unspecified row order are
 * preserved. The stream remains valid after the source dataset is closed.
 *
 * `out` is caller-owned, must be zero-initialized by convention, and is
 * written only on success. Invoke its non-NULL `release` callback exactly once.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_all_files_stream(
    const LanceDataset* dataset,
    struct ArrowArrayStream* out
);

/**
 * Restore the dataset to an older version by committing a new manifest that
 * carries the fragments of `version`. If `version` is already the latest,
 * succeeds as a no-op without writing a new manifest.
 *
 * @param dataset  Open dataset (not consumed). Must not be NULL.
 * @param version  Target version id (>= 1). `0` is rejected since it is the
 *                 "latest" sentinel used by lance_dataset_open.
 * @return Fresh LanceDataset* positioned at the target version (caller closes
 *         with lance_dataset_close), or NULL on error. Possible error codes
 *         include LANCE_ERR_INVALID_ARGUMENT (NULL handle or version == 0),
 *         LANCE_ERR_NOT_FOUND (unknown version),
 *         LANCE_ERR_COMMIT_CONFLICT (concurrent writer).
 */
LanceDataset* lance_dataset_restore(const LanceDataset* dataset, uint64_t version);

/**
 * Delete rows matching the SQL `predicate`, committing a new manifest.
 *
 * Mutates `dataset` in place — the same handle remains valid afterward and
 * sees the new version. Scanners already in flight against this dataset
 * keep their pre-delete snapshot view.
 *
 * @param dataset          Open dataset (not consumed). Must not be NULL.
 * @param predicate        SQL filter, e.g. "id > 100" or "name = 'alice'".
 *                         Must not be NULL or empty.
 * @param out_num_deleted  Optional. If non-NULL, on success receives the
 *                         number of rows that were deleted (0 if the
 *                         predicate matched nothing). On error the slot is
 *                         left unchanged — do not read it.
 * @return 0 on success, -1 on error. Error codes:
 *         LANCE_ERR_INVALID_ARGUMENT for NULL/empty args (validated at this
 *         boundary) and for malformed SQL or unknown columns (surfaced from
 *         the upstream parser since Lance 9.1; previously LANCE_ERR_INTERNAL),
 *         and LANCE_ERR_COMMIT_CONFLICT for a concurrent writer.
 */
int32_t lance_dataset_delete(
    LanceDataset* dataset,
    const char* predicate,
    uint64_t* out_num_deleted
);

/**
 * Update rows matching the SQL `predicate` by applying per-column SQL
 * expressions, committing a new manifest.
 *
 * Mutates `dataset` in place — the same handle remains valid afterward and
 * sees the new version. Scanners already in flight against this dataset
 * keep their pre-update snapshot view.
 *
 * @param dataset          Open dataset (not consumed). Must not be NULL.
 * @param predicate        SQL filter, e.g. "id > 100". Pass NULL to update
 *                         every row. An explicit empty string is rejected.
 * @param columns          Column names to update. Length = `num_updates`.
 *                         Must not be NULL when `num_updates > 0`; each
 *                         entry must be a non-NULL, non-empty C string.
 * @param values           SQL scalar expressions, evaluated per row, one
 *                         per `columns[i]` (e.g. `"100"`, `"price * 2"`,
 *                         `"CASE WHEN ... END"`). Same NULL/length rules.
 * @param num_updates      Length of `columns` and `values`. Must be >= 1.
 * @param out_num_updated  Optional. If non-NULL, on success receives the
 *                         number of rows that were updated (0 if the
 *                         predicate matched nothing). On error the slot is
 *                         left unchanged — do not read it.
 * @return 0 on success, -1 on error. Error codes:
 *         LANCE_ERR_INVALID_ARGUMENT for NULL/empty args, `num_updates == 0`,
 *         malformed SQL, and unknown columns; LANCE_ERR_COMMIT_CONFLICT for
 *         a concurrent writer.
 */
int32_t lance_dataset_update(
    LanceDataset* dataset,
    const char* predicate,
    const char* const* columns,
    const char* const* values,
    size_t num_updates,
    uint64_t* out_num_updated
);

/* ─── lance_dataset_merge_insert ──────────────────────────────────────────── */

/**
 * Behavior when a target row matches a source row on the join keys.
 * Defaults are zero-valued so a zero-initialized LanceMergeInsertParams is a
 * valid find-or-create configuration.
 */
typedef enum {
    /* Keep the target row unchanged (find-or-create). Default. */
    LANCE_MERGE_WHEN_MATCHED_DO_NOTHING  = 0,
    /* Replace the target row with the source row (upsert). */
    LANCE_MERGE_WHEN_MATCHED_UPDATE_ALL  = 1,
    /* Replace only when an SQL filter evaluates true; requires
       when_matched_expr. */
    LANCE_MERGE_WHEN_MATCHED_UPDATE_IF   = 2,
    /* Fail the operation on any match. */
    LANCE_MERGE_WHEN_MATCHED_FAIL        = 3,
    /* Drop the matching target row without inserting anything. */
    LANCE_MERGE_WHEN_MATCHED_DELETE      = 4,
} LanceMergeWhenMatched;

/** Behavior when a source row has no matching target row. */
typedef enum {
    /* Insert the source row. Default. */
    LANCE_MERGE_WHEN_NOT_MATCHED_INSERT_ALL = 0,
    /* Discard the source row. */
    LANCE_MERGE_WHEN_NOT_MATCHED_DO_NOTHING = 1,
} LanceMergeWhenNotMatched;

/** Behavior when a target row has no matching source row. */
typedef enum {
    /* Keep the target row. Default. */
    LANCE_MERGE_WHEN_NOT_MATCHED_BY_SOURCE_KEEP      = 0,
    /* Delete every unmatched target row. */
    LANCE_MERGE_WHEN_NOT_MATCHED_BY_SOURCE_DELETE    = 1,
    /* Delete unmatched target rows that satisfy an SQL filter; requires
       when_not_matched_by_source_expr. */
    LANCE_MERGE_WHEN_NOT_MATCHED_BY_SOURCE_DELETE_IF = 2,
} LanceMergeWhenNotMatchedBySource;

/**
 * Tunable parameters for lance_dataset_merge_insert. Pass NULL to use the
 * find-or-create defaults (DO_NOTHING / INSERT_ALL / KEEP).
 *
 * Expression strings are read only when the corresponding mode requires
 * them; spurious non-NULL pointers on other modes are rejected so the
 * contract is unambiguous.
 */
typedef struct LanceMergeInsertParams {
    /* LanceMergeWhenMatched discriminant. */
    int32_t     when_matched;
    /* SQL filter for UPDATE_IF; NULL otherwise. Empty string is rejected. */
    const char* when_matched_expr;
    /* LanceMergeWhenNotMatched discriminant. */
    int32_t     when_not_matched;
    /* LanceMergeWhenNotMatchedBySource discriminant. */
    int32_t     when_not_matched_by_source;
    /* SQL filter for DELETE_IF; NULL otherwise. Empty string is rejected. */
    const char* when_not_matched_by_source_expr;
} LanceMergeInsertParams;

/** Per-call merge statistics returned via the optional out parameter. */
typedef struct LanceMergeInsertResult {
    uint64_t num_inserted_rows;
    uint64_t num_updated_rows;
    uint64_t num_deleted_rows;
} LanceMergeInsertResult;

/**
 * Merge `source` into `dataset` keyed on `on_columns`, committing a new
 * manifest. Mirrors SQL MERGE; the default parameters yield a find-or-create
 * (insert rows that do not match an existing key).
 *
 * Mutates `dataset` in place — the same handle remains valid afterward and
 * sees the new version. Scanners already in flight against this dataset
 * keep their pre-merge snapshot view.
 *
 * @param dataset         Open dataset (not consumed). Must not be NULL.
 * @param on_columns      Join keys. Length = `num_on_columns`. Must be
 *                        non-NULL when `num_on_columns > 0`; each entry
 *                        must be a non-NULL, non-empty C string. Column
 *                        names are matched case-insensitively (upstream).
 * @param num_on_columns  Length of `on_columns`. Must be >= 1.
 * @param source          Arrow C Data Interface stream of source rows.
 *                        Consumed by this call. Its schema must be
 *                        compatible with the dataset schema (full match or
 *                        a subschema).
 * @param params          Tunable parameters. Pass NULL for find-or-create
 *                        defaults.
 * @param out_result      Optional. If non-NULL, on success receives the
 *                        per-call insert/update/delete counts. On error the
 *                        slot is left unchanged — do not read it.
 * @return 0 on success, -1 on error. Error codes:
 *         LANCE_ERR_INVALID_ARGUMENT for NULL/empty args, out-of-range mode
 *         discriminants, missing or extraneous expression strings, malformed
 *         SQL, unknown columns, schema incompatibility, and no-op
 *         configurations; LANCE_ERR_COMMIT_CONFLICT for a concurrent writer.
 */
int32_t lance_dataset_merge_insert(
    LanceDataset* dataset,
    const char* const* on_columns,
    size_t num_on_columns,
    struct ArrowArrayStream* source,
    const LanceMergeInsertParams* params,
    LanceMergeInsertResult* out_result
);

/* ─── lance_dataset_merge ─────────────────────────────────────────────────── */

/**
 * Left-join precomputed columns from `source` into `dataset` and commit one
 * native `Dataset::merge` operation.
 *
 * `left_on` may name an existing dataset column, `_rowid`, or `_rowaddr`.
 * `right_on` names a field in the source schema. Field lookup is exact and
 * case-sensitive. Empty key strings are forwarded to native validation.
 *
 * The source field named by `right_on` is the join key and is not appended.
 * Every other top-level RHS field must be new to the dataset. Native owns key
 * matching, duplicate and NULL semantics, schema compatibility, field IDs,
 * physical writes, and commit conflicts.
 *
 * The non-NULL `source` stream is consumed on every return path, including
 * invalid dataset/key arguments and native failures. After this call the
 * caller must not read, move, or release the stream again. A NULL `source` is
 * rejected without consuming anything.
 *
 * On success the same dataset handle points at the newly committed version.
 * Existing scanners, fragment snapshots, and independently opened handles
 * retain their previous snapshot. On error the handle keeps its prior
 * in-memory snapshot unless the native commit outcome is uncertain; reopen
 * the URI to establish the actual committed state in that case.
 *
 * Native may leave uncommitted data files after a failed merge. The binding
 * does not delete or roll them back; dataset cleanup/GC owns their
 * disposition.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_merge(
    LanceDataset* dataset,
    const char* left_on,
    const char* right_on,
    struct ArrowArrayStream* source
);

/* ─── BatchUDF callbacks ─────────────────────────────────────────────────── */

/**
 * Owned, optional NUL-terminated UTF-8 string returned by a foreign callback.
 *
 * When `data` is non-NULL, `release` must be non-NULL. The library copies the
 * string before invoking `release`, then calls `release` exactly once. The
 * release callback must free the storage behind `data` and return normally.
 */
typedef struct LanceOwnedString {
    char* data;
    void (*release)(struct LanceOwnedString* self);
} LanceOwnedString;

/**
 * Error returned by a BatchUDF mapper or checkpoint callback.
 *
 * The library zero-initializes this structure before every invocation. On
 * success the callback leaves it zeroed. On failure `code` is nonzero:
 * INVALID_ARGUMENT, IO, NOT_FOUND, NOT_SUPPORTED, and INTERNAL map to their
 * native variants; other values are treated as INTERNAL.
 *
 * `message` is copied into a Rust-owned error before `message.release` is
 * called exactly once. A callback must not report failure through
 * lance_last_error_*: callbacks may run on a different thread.
 */
typedef struct LanceCallbackError {
    int32_t code;
    LanceOwnedString message;
} LanceCallbackError;

/**
 * Native BatchInfo passed to checkpoint callbacks. The pointer is borrowed for
 * the callback only. `batch_index` is size_t to match native usize.
 */
typedef struct LanceUDFBatchInfo {
    uint32_t fragment_id;
    size_t batch_index;
} LanceUDFBatchInfo;

/**
 * Map one input RecordBatch to an owned output ArrowArray.
 *
 * Input values are borrowed for the callback only. The input ArrowArray is the
 * top-level struct array and `input_schema` is its complete schema. On success
 * initialize `output` with an owned ArrowArray whose struct children match the
 * declared `LanceBatchUDF.output_schema`, then return 0. Native owns the output
 * and its release callback after a successful return. Zero-column batches
 * retain their row count through the top-level array length.
 *
 * On failure return nonzero, fill `error`, and leave any initialized output
 * for the library to release. The callback may run on a native runtime thread
 * and must be thread-safe and non-unwinding.
 */
typedef int32_t (*LanceBatchUDFMapper)(
    void* callback_ctx,
    const struct ArrowArray* input,
    const struct ArrowSchema* input_schema,
    struct ArrowArray* output,
    LanceCallbackError* error
);

/**
 * Retrieve a checkpointed output batch.
 *
 * Return 1 with `output` initialized to an owned ArrowArray, 0 with `output`
 * left zero-initialized, or -1 with `error` initialized.
 */
typedef int32_t (*LanceUDFCheckpointGetBatch)(
    void* callback_ctx,
    const LanceUDFBatchInfo* info,
    struct ArrowArray* output,
    LanceCallbackError* error
);

/**
 * Retain a checkpoint batch.
 *
 * `batch` and `batch_schema` are owned, movable Arrow C Data Interface values.
 * The callback may move both structs into its storage and clear the source
 * `release` fields. The library releases any ownership left in these argument
 * slots after the callback returns, so a retained value can outlive the call
 * and dataset close. Return 0 on success or -1 with `error`.
 */
typedef int32_t (*LanceUDFCheckpointInsertBatch)(
    void* callback_ctx,
    const LanceUDFBatchInfo* info,
    struct ArrowArray* batch,
    struct ArrowSchema* batch_schema,
    LanceCallbackError* error
);

/**
 * Retrieve native Fragment JSON for a fragment checkpoint hit.
 *
 * Return 1 with `out_json` initialized, 0 for a miss, or -1 with `error`.
 * The library copies the JSON before calling `out_json->release` exactly once.
 */
typedef int32_t (*LanceUDFCheckpointGetFragment)(
    void* callback_ctx,
    uint32_t fragment_id,
    LanceOwnedString* out_json,
    LanceCallbackError* error
);

/**
 * Persist native Fragment JSON and its native u64 Fragment ID.
 *
 * `fragment_json` is borrowed for the callback only. Return 0 on success or
 * -1 with `error`.
 */
typedef int32_t (*LanceUDFCheckpointInsertFragment)(
    void* callback_ctx,
    uint64_t fragment_id,
    const char* fragment_json,
    LanceCallbackError* error
);

/**
 * Foreign UDFCheckpointStore adapter.
 *
 * All four callbacks are required. `callback_ctx` is a call-scoped borrow and
 * may be NULL for stateless callbacks. The library neither retains nor
 * releases it.
 */
typedef struct LanceUDFCheckpointStore {
    LanceUDFCheckpointGetBatch get_batch;
    LanceUDFCheckpointInsertBatch insert_batch;
    LanceUDFCheckpointGetFragment get_fragment;
    LanceUDFCheckpointInsertFragment insert_fragment;
    void* callback_ctx;
} LanceUDFCheckpointStore;

/**
 * Foreign BatchUDF declaration.
 *
 * `output_schema` is the complete schema of every mapper and get-batch output
 * batch and is borrowed for this call. `checkpoint_store` may be NULL.
 * Mapper and checkpoint contexts are call-scoped and must remain valid until
 * the enclosing function returns.
 */
typedef struct LanceBatchUDF {
    LanceBatchUDFMapper mapper;
    void* mapper_ctx;
    const struct ArrowSchema* output_schema;
    const LanceUDFCheckpointStore* checkpoint_store;
} LanceBatchUDF;

/**
 * Add columns through native `NewColumnTransform::BatchUDF`.
 *
 * `read_columns` NULL maps to native None. A non-NULL NULL-terminated array
 * maps to native Some; an immediately terminated array maps to Some(empty).
 * `batch_size` NULL maps to None. A non-NULL pointer maps to Some(*batch_size),
 * including explicit zero; native owns validation of zero.
 *
 * Native owns output schema compatibility, row alignment, checkpoint ordering,
 * file cleanup, publication, and commit conflicts. Mapper and store callbacks
 * execute synchronously under the dataset handle's exclusive mutation lock.
 * They must not re-enter the same handle; use an independent handle or an
 * existing fragment/scanner snapshot for reads.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_add_columns_batch_udf(
    LanceDataset* dataset,
    const LanceBatchUDF* udf,
    const char* const* read_columns,
    const uint32_t* batch_size
);

/* ─── lance_dataset_compact_files ─────────────────────────────────────────── */

/**
 * Tunable parameters for lance_dataset_compact_files. Pass NULL to use the
 * upstream defaults. Each numeric field uses 0 as a "keep upstream default"
 * sentinel; non-zero values are forwarded after a usize range check so the
 * API does not silently truncate on 32-bit hosts.
 *
 * This five-field layout is frozen. New callers should use
 * LanceCompactionOptionsV2, which preserves native None/Some(0)/Some(empty)
 * choices and exposes every native compaction option.
 */
typedef struct LanceCompactionOptions {
    /* Target row count per output fragment. Fragments below this size are
       candidates for being merged with neighbors. 0 = default (~1Mi rows). */
    uint64_t target_rows_per_fragment;
    /* Soft cap on rows per row group within an output fragment. 0 = default. */
    uint64_t max_rows_per_group;
    /* Soft cap on bytes per output fragment file. 0 = default (writer cap). */
    uint64_t max_bytes_per_file;
    /* Compute parallelism for compaction tasks. 0 = default
       (number of compute-intensive CPUs). */
    uint64_t num_threads;
    /* Scanner batch size for reading input fragments. 0 = default. */
    uint64_t batch_size;
} LanceCompactionOptions;

/** Per-call compaction metrics returned via the optional out parameter. */
typedef struct LanceCompactionMetrics {
    /* Number of input fragments that were rewritten and dropped. */
    uint64_t fragments_removed;
    /* Number of new fragments produced by the rewrite. */
    uint64_t fragments_added;
    /* Total files removed across the operation, including deletion files. */
    uint64_t files_removed;
    /* Total files added across the operation; one per new fragment. */
    uint64_t files_added;
} LanceCompactionMetrics;

/**
 * Compact the dataset's fragments, committing a new manifest if anything
 * changed. Each compaction task merges adjacent small fragments and
 * materializes any deletion files in the process. A clean dataset (no
 * fragment under the target size, no deletions worth materializing) is a
 * no-op: the function returns success with all-zero metrics and the
 * dataset's version is unchanged.
 *
 * Mutates `dataset` in place — the same handle remains valid afterward and
 * sees the new version. Scanners already in flight against this dataset
 * keep their pre-compaction snapshot view.
 *
 * @param dataset      Open dataset (not consumed). Must not be NULL.
 * @param options      Tunable parameters. Pass NULL for upstream defaults.
 * @param out_metrics  Optional. If non-NULL, on success receives the per-call
 *                     compaction metrics. On error the slot is left unchanged
 *                     — do not read it.
 * @return 0 on success, -1 on error. Error codes:
 *         LANCE_ERR_INVALID_ARGUMENT for NULL `dataset` or for numeric
 *         overrides that exceed usize::MAX on the running target;
 *         LANCE_ERR_COMMIT_CONFLICT for a concurrent writer.
 */
int32_t lance_dataset_compact_files(
    LanceDataset* dataset,
    const LanceCompactionOptions* options,
    LanceCompactionMetrics* out_metrics
);

/* ─── Native maintenance lifecycle ────────────────────────────────────────── */

/** Migrate this dataset's manifest paths to v2 naming. Native rename behavior. */
int32_t lance_dataset_migrate_manifest_paths_v2(LanceDataset* dataset);

/** Delete all rows through native Dataset::truncate_table. */
int32_t lance_dataset_truncate_table(LanceDataset* dataset);

/* ─── Native compaction options V2 ────────────────────────────────────────── */

/** Controls how data is rewritten during compaction. */
typedef enum LanceCompactionMode {
    LANCE_COMPACTION_REENCODE          = 0,
    LANCE_COMPACTION_TRY_BINARY_COPY   = 1,
    LANCE_COMPACTION_FORCE_BINARY_COPY = 2
} LanceCompactionMode;

/** Controls how the old-to-new row-address mapping is represented. */
typedef enum LanceIndexRemapMode {
    LANCE_INDEX_REMAP_COMPACT = 0,
    LANCE_INDEX_REMAP_DIRECT  = 1
} LanceIndexRemapMode;

/** Create options from native CompactionOptions::default(). */
LanceCompactionOptionsV2* lance_compaction_options_v2_new(void);

/** Create options from native CompactionOptions::from_dataset_config(). */
LanceCompactionOptionsV2* lance_compaction_options_v2_from_dataset(
    const LanceDataset* dataset
);

/** Clone an owned native options value. */
LanceCompactionOptionsV2* lance_compaction_options_v2_clone(
    const LanceCompactionOptionsV2* options
);

/** Free options. NULL-safe. */
void lance_compaction_options_v2_free(LanceCompactionOptionsV2* options);

int32_t lance_compaction_options_v2_set_target_rows_per_fragment(
    LanceCompactionOptionsV2* options,
    uint64_t value
);
int32_t lance_compaction_options_v2_set_max_rows_per_group(
    LanceCompactionOptionsV2* options,
    uint64_t value
);
/** `has_value == false` selects native None; true preserves zero. */
int32_t lance_compaction_options_v2_set_max_bytes_per_file(
    LanceCompactionOptionsV2* options,
    bool has_value,
    uint64_t value
);
int32_t lance_compaction_options_v2_set_materialize_deletions(
    LanceCompactionOptionsV2* options,
    bool value
);
int32_t lance_compaction_options_v2_set_materialize_deletions_threshold(
    LanceCompactionOptionsV2* options,
    float value
);
int32_t lance_compaction_options_v2_set_num_threads(
    LanceCompactionOptionsV2* options,
    bool has_value,
    uint64_t value
);
int32_t lance_compaction_options_v2_set_batch_size(
    LanceCompactionOptionsV2* options,
    bool has_value,
    uint64_t value
);
int32_t lance_compaction_options_v2_set_io_buffer_size(
    LanceCompactionOptionsV2* options,
    bool has_value,
    uint64_t value
);
int32_t lance_compaction_options_v2_set_defer_index_remap(
    LanceCompactionOptionsV2* options,
    bool value
);
int32_t lance_compaction_options_v2_set_index_remap_mode(
    LanceCompactionOptionsV2* options,
    int32_t mode
);
/** `has_value == false` selects native None; true selects the given mode. */
int32_t lance_compaction_options_v2_set_compaction_mode(
    LanceCompactionOptionsV2* options,
    bool has_value,
    int32_t mode
);
int32_t lance_compaction_options_v2_set_binary_copy_read_batch_bytes(
    LanceCompactionOptionsV2* options,
    bool has_value,
    uint64_t value
);
int32_t lance_compaction_options_v2_set_max_source_fragments(
    LanceCompactionOptionsV2* options,
    bool has_value,
    uint64_t value
);
int32_t lance_compaction_options_v2_set_max_source_rows(
    LanceCompactionOptionsV2* options,
    bool has_value,
    uint64_t value
);
int32_t lance_compaction_options_v2_set_max_source_bytes(
    LanceCompactionOptionsV2* options,
    bool has_value,
    uint64_t value
);
/** Replace excluded IDs. NULL with count=0 is the empty list. */
int32_t lance_compaction_options_v2_set_excluded_fragment_ids(
    LanceCompactionOptionsV2* options,
    const uint32_t* fragment_ids,
    size_t count
);
int32_t lance_compaction_options_v2_set_max_overlays_per_fragment(
    LanceCompactionOptionsV2* options,
    bool has_value,
    uint64_t value
);
/** Parse the selector with native LanceFileVersion::from_str(). NULL = None. */
int32_t lance_compaction_options_v2_set_data_storage_version(
    LanceCompactionOptionsV2* options,
    const char* selector
);
/** NULL = native None; non-NULL with count=0 = Some(empty map). */
int32_t lance_compaction_options_v2_set_transaction_properties(
    LanceCompactionOptionsV2* options,
    const LanceTransactionProperties* properties
);

/**
 * Compact using native CompactionOptions.
 * NULL options select from_dataset_config. Metrics are written only on
 * success.
 */
int32_t lance_dataset_compact_files_v2(
    LanceDataset* dataset,
    const LanceCompactionOptionsV2* options,
    LanceCompactionMetrics* out_metrics
);

/** Plan compaction. NULL options select from_dataset_config. */
LanceCompactionPlan* lance_dataset_plan_compaction(
    const LanceDataset* dataset,
    const LanceCompactionOptionsV2* options
);

uint64_t lance_compaction_plan_read_version(const LanceCompactionPlan* plan);
size_t lance_compaction_plan_task_count(const LanceCompactionPlan* plan);

/** Owned task snapshot; free with lance_compaction_task_close(). */
LanceCompactionTask* lance_compaction_plan_task_at(
    const LanceCompactionPlan* plan,
    size_t index
);

/** Clone effective plan options, including the resolved exact storage version. */
LanceCompactionOptionsV2* lance_compaction_plan_options_clone(
    const LanceCompactionPlan* plan
);

/** Native serde transport. Free strings with lance_free_string(). */
const char* lance_compaction_plan_json(const LanceCompactionPlan* plan);
LanceCompactionPlan* lance_compaction_plan_from_json(const char* json);
void lance_compaction_plan_close(LanceCompactionPlan* plan);

uint64_t lance_compaction_task_read_version(const LanceCompactionTask* task);
size_t lance_compaction_task_fragment_count(const LanceCompactionTask* task);
const char* lance_compaction_task_fragment_metadata_json_at(
    const LanceCompactionTask* task,
    size_t index
);
LanceCompactionOptionsV2* lance_compaction_task_options_clone(
    const LanceCompactionTask* task
);

/** Execute against the dataset at the task's planned read version. */
LanceRewriteResult* lance_compaction_task_execute(
    const LanceDataset* dataset,
    const LanceCompactionTask* task
);

const char* lance_compaction_task_json(const LanceCompactionTask* task);
LanceCompactionTask* lance_compaction_task_from_json(const char* json);
void lance_compaction_task_close(LanceCompactionTask* task);

uint64_t lance_rewrite_result_read_version(const LanceRewriteResult* result);
int32_t lance_rewrite_result_metrics(
    const LanceRewriteResult* result,
    LanceCompactionMetrics* out_metrics
);
size_t lance_rewrite_result_original_fragment_count(
    const LanceRewriteResult* result
);
size_t lance_rewrite_result_new_fragment_count(
    const LanceRewriteResult* result
);
const char* lance_rewrite_result_original_fragment_metadata_json_at(
    const LanceRewriteResult* result,
    size_t index
);
const char* lance_rewrite_result_new_fragment_metadata_json_at(
    const LanceRewriteResult* result,
    size_t index
);
const char* lance_rewrite_result_json(const LanceRewriteResult* result);
LanceRewriteResult* lance_rewrite_result_from_json(const char* json);
void lance_rewrite_result_close(LanceRewriteResult* result);

/**
 * Commit rewrite results through native commit_compaction.
 * NULL options select CompactionOptions::default(). Metrics are written only
 * on success.
 */
int32_t lance_dataset_commit_compaction(
    LanceDataset* dataset,
    const LanceRewriteResult* const* results,
    size_t result_count,
    const LanceCompactionOptionsV2* options,
    LanceCompactionMetrics* out_metrics
);

/* ─── Native index optimization ───────────────────────────────────────────── */

/** Create options from native OptimizeOptions::default(). */
LanceOptimizeIndicesOptions* lance_optimize_indices_options_new(void);
LanceOptimizeIndicesOptions* lance_optimize_indices_options_clone(
    const LanceOptimizeIndicesOptions* options
);
void lance_optimize_indices_options_free(LanceOptimizeIndicesOptions* options);

int32_t lance_optimize_indices_options_set_num_indices_to_merge(
    LanceOptimizeIndicesOptions* options,
    bool has_value,
    uint64_t value
);
int32_t lance_optimize_indices_options_set_enable_partition_rebalancing(
    LanceOptimizeIndicesOptions* options,
    bool value
);
/** false = native None; true + count=0 = native Some(empty). */
int32_t lance_optimize_indices_options_set_index_names(
    LanceOptimizeIndicesOptions* options,
    bool has_value,
    const char* const* names,
    size_t count
);
int32_t lance_optimize_indices_options_set_retrain(
    LanceOptimizeIndicesOptions* options,
    bool value
);
int32_t lance_optimize_indices_options_set_transaction_properties(
    LanceOptimizeIndicesOptions* options,
    const LanceTransactionProperties* properties
);
int32_t lance_optimize_indices_options_set_max_rows_per_task(
    LanceOptimizeIndicesOptions* options,
    bool has_value,
    uint64_t value
);
int32_t lance_optimize_indices_options_set_max_rows_per_segment(
    LanceOptimizeIndicesOptions* options,
    bool has_value,
    uint64_t value
);

/** Single-process native optimize_indices. NULL options use defaults. */
int32_t lance_dataset_optimize_indices(
    LanceDataset* dataset,
    const LanceOptimizeIndicesOptions* options
);

/** Plan the native distributed optimize workflow. */
LanceOptimizeIndicesPlan* lance_dataset_plan_optimize_indices(
    const LanceDataset* dataset,
    const LanceOptimizeIndicesOptions* options
);

/** Copy a native UUID as 16 raw RFC 4122 bytes. */
int32_t lance_optimize_indices_plan_id(
    const LanceOptimizeIndicesPlan* plan,
    uint8_t out_uuid[16]
);
uint64_t lance_optimize_indices_plan_read_version(
    const LanceOptimizeIndicesPlan* plan
);
size_t lance_optimize_indices_plan_task_count(
    const LanceOptimizeIndicesPlan* plan
);
size_t lance_optimize_indices_plan_merge_count(
    const LanceOptimizeIndicesPlan* plan
);
LanceOptimizeIndexTask* lance_optimize_indices_plan_task_at(
    const LanceOptimizeIndicesPlan* plan,
    size_t index
);
LanceOptimizeIndexMerge* lance_optimize_indices_plan_merge_at(
    const LanceOptimizeIndicesPlan* plan,
    size_t index
);
const char* lance_optimize_indices_plan_json(
    const LanceOptimizeIndicesPlan* plan
);
LanceOptimizeIndicesPlan* lance_optimize_indices_plan_from_json(
    const char* json
);
void lance_optimize_indices_plan_close(LanceOptimizeIndicesPlan* plan);

int32_t lance_optimize_index_task_plan_id(
    const LanceOptimizeIndexTask* task,
    uint8_t out_uuid[16]
);
uint64_t lance_optimize_index_task_read_version(
    const LanceOptimizeIndexTask* task
);
uint32_t lance_optimize_index_task_group(
    const LanceOptimizeIndexTask* task
);
size_t lance_optimize_index_task_fragment_count(
    const LanceOptimizeIndexTask* task
);
int32_t lance_optimize_index_task_fragment_id_at(
    const LanceOptimizeIndexTask* task,
    size_t index,
    uint32_t* out_id
);
/** Owned LanceIndexSegmentMetadata; free with its existing free function. */
LanceIndexSegmentMetadata* lance_optimize_index_task_reference(
    const LanceOptimizeIndexTask* task
);
size_t lance_optimize_index_task_old_segment_count(
    const LanceOptimizeIndexTask* task
);
LanceIndexSegmentMetadata* lance_optimize_index_task_old_segment_at(
    const LanceOptimizeIndexTask* task,
    size_t index
);
int32_t lance_optimize_index_task_num_indices_to_merge(
    const LanceOptimizeIndexTask* task,
    bool* out_has_value,
    uint64_t* out_value
);
int32_t lance_optimize_index_task_enable_partition_rebalancing(
    const LanceOptimizeIndexTask* task,
    bool* out_value
);
int32_t lance_optimize_index_task_retrain(
    const LanceOptimizeIndexTask* task,
    bool* out_value
);

/** `*out_result` receives NULL when native returns None; untouched on error. */
int32_t lance_optimize_index_task_execute(
    const LanceDataset* dataset,
    const LanceOptimizeIndexTask* task,
    LanceIndexSegmentResult** out_result
);

const char* lance_optimize_index_task_json(
    const LanceOptimizeIndexTask* task
);
LanceOptimizeIndexTask* lance_optimize_index_task_from_json(const char* json);
void lance_optimize_index_task_close(LanceOptimizeIndexTask* task);

int32_t lance_optimize_index_merge_plan_id(
    const LanceOptimizeIndexMerge* merge,
    uint8_t out_uuid[16]
);
uint64_t lance_optimize_index_merge_read_version(
    const LanceOptimizeIndexMerge* merge
);
uint32_t lance_optimize_index_merge_group(
    const LanceOptimizeIndexMerge* merge
);
size_t lance_optimize_index_merge_old_segment_count(
    const LanceOptimizeIndexMerge* merge
);
LanceIndexSegmentMetadata* lance_optimize_index_merge_old_segment_at(
    const LanceOptimizeIndexMerge* merge,
    size_t index
);
size_t lance_optimize_index_merge_replace_count(
    const LanceOptimizeIndexMerge* merge
);
int32_t lance_optimize_index_merge_replace_uuid_at(
    const LanceOptimizeIndexMerge* merge,
    size_t index,
    uint8_t out_uuid[16]
);
/** Native plan/group/version checks remain authoritative. */
int32_t lance_optimize_index_merge_execute(
    const LanceDataset* dataset,
    const LanceOptimizeIndexMerge* merge,
    const LanceIndexSegmentResult* const* task_results,
    size_t task_result_count,
    LanceIndexSegmentResult** out_result
);
const char* lance_optimize_index_merge_json(
    const LanceOptimizeIndexMerge* merge
);
LanceOptimizeIndexMerge* lance_optimize_index_merge_from_json(const char* json);
void lance_optimize_index_merge_close(LanceOptimizeIndexMerge* merge);

int32_t lance_index_segment_result_plan_id(
    const LanceIndexSegmentResult* result,
    uint8_t out_uuid[16]
);
uint64_t lance_index_segment_result_read_version(
    const LanceIndexSegmentResult* result
);
uint32_t lance_index_segment_result_group(
    const LanceIndexSegmentResult* result
);
LanceIndexSegmentMetadata* lance_index_segment_result_segment(
    const LanceIndexSegmentResult* result
);
size_t lance_index_segment_result_replace_count(
    const LanceIndexSegmentResult* result
);
int32_t lance_index_segment_result_replace_uuid_at(
    const LanceIndexSegmentResult* result,
    size_t index,
    uint8_t out_uuid[16]
);
const char* lance_index_segment_result_json(
    const LanceIndexSegmentResult* result
);
LanceIndexSegmentResult* lance_index_segment_result_from_json(const char* json);
void lance_index_segment_result_close(LanceIndexSegmentResult* result);

/** Commit merged results. Empty results retain native no-op/MemWAL behavior. */
int32_t lance_dataset_commit_optimize_indices(
    LanceDataset* dataset,
    const LanceIndexSegmentResult* const* results,
    size_t result_count,
    const LanceOptimizeIndicesOptions* options
);

/* ─── lance_dataset_drop_columns ──────────────────────────────────────────── */

/**
 * Drop one or more columns from the dataset's schema, committing a new
 * manifest. This is a metadata-only operation: the data files on storage
 * are not rewritten until a later `lance_dataset_compact_files` call
 * materializes the projection (after which the previous version's files
 * can be removed by a future cleanup operation).
 *
 * Mutates `dataset` in place — the same handle remains valid afterward
 * and sees the new version. Scanners already in flight against this
 * dataset keep their pre-drop schema view.
 *
 * @param dataset      Open dataset (not consumed). Mutated in place to
 *                     see the new version. Must not be NULL.
 * @param columns      Array of NUL-terminated UTF-8 column names to drop.
 *                     Must not be NULL; entries must be non-NULL and
 *                     non-empty.
 * @param num_columns  Length of `columns`. Must be > 0.
 * @return 0 on success, -1 on error. Error codes:
 *         LANCE_ERR_INVALID_ARGUMENT for NULL/empty inputs, NULL or empty
 *         entries, non-UTF-8 column names, unknown columns, or an attempt
 *         to drop every column;
 *         LANCE_ERR_COMMIT_CONFLICT for a concurrent writer.
 */
int32_t lance_dataset_drop_columns(
    LanceDataset* dataset,
    const char* const* columns,
    size_t num_columns
);

/* ─── lance_dataset_alter_columns ─────────────────────────────────────────── */

/**
 * Tri-state nullability override for `LanceColumnAlteration`. The
 * `UNCHANGED` discriminant is zero so a zero-initialised
 * `LanceColumnAlteration` leaves nullability alone by default.
 *
 * Discriminants are pinned for ABI stability. Out-of-range values are
 * rejected with `LANCE_ERR_INVALID_ARGUMENT` — that's why the field on
 * `LanceColumnAlteration` is `int32_t`, not this enum directly.
 */
typedef enum {
    /* Do not touch the column's existing nullability. */
    LANCE_COLUMN_NULLABLE_UNCHANGED = 0,
    /* Set the column to nullable. */
    LANCE_COLUMN_NULLABLE_TRUE      = 1,
    /* Set the column to non-nullable. Upstream verifies via a scan that no
       row holds a NULL — the call fails if any do. */
    LANCE_COLUMN_NULLABLE_FALSE     = 2,
} LanceColumnNullableMode;

/**
 * A single alteration applied to one column. Every non-`path` field is
 * optional via a sentinel:
 *
 *   - `rename = NULL` keeps the current name.
 *   - `nullable_mode = LANCE_COLUMN_NULLABLE_UNCHANGED` keeps current nullability.
 *   - `data_type = NULL` keeps the current data type.
 *
 * At least one of `rename`, `nullable_mode`, or `data_type` must request a
 * change; an alteration that touches nothing is rejected at the FFI boundary.
 *
 * `data_type`, when non-NULL, borrows an Arrow C Data Interface `ArrowSchema`
 * describing the target type. The struct is read by shared reference for the
 * duration of the call; its `release` callback is never invoked.
 */
typedef struct LanceColumnAlteration {
    /* Path to the existing column. Required, non-empty UTF-8. */
    const char* path;
    /* New column name, or NULL to keep the current name. */
    const char* rename;
    /* LanceColumnNullableMode discriminant. */
    int32_t     nullable_mode;
    /* New data type, or NULL to keep the current type. */
    const struct ArrowSchema* data_type;
} LanceColumnAlteration;

/**
 * Apply one or more column alterations and commit a new manifest. Rename and
 * nullability-only changes are zero-copy and preserve any indices on the
 * affected columns. A type change rewrites the column's data files and drops
 * any indices that referenced it, mirroring upstream behaviour.
 *
 * Mutates `dataset` in place — the same handle remains valid afterward and
 * sees the new version. Scanners already in flight against this dataset keep
 * their pre-alteration view.
 *
 * @param dataset          Open dataset (not consumed). Mutated in place to
 *                         see the new version. Must not be NULL.
 * @param alterations      Array of `LanceColumnAlteration`. Must not be NULL.
 * @param num_alterations  Length of `alterations`. Must be > 0.
 * @return 0 on success, -1 on error. Error codes:
 *         LANCE_ERR_INVALID_ARGUMENT for NULL/empty inputs, NULL or empty
 *         `path`, non-UTF-8 strings, no-op alterations (all three optional
 *         fields left at their sentinels), invalid `nullable_mode`
 *         discriminant, unknown columns, type changes that aren't a valid
 *         cast, or tightening nullability when existing rows hold NULLs;
 *         LANCE_ERR_COMMIT_CONFLICT for a concurrent writer.
 */
int32_t lance_dataset_alter_columns(
    LanceDataset* dataset,
    const LanceColumnAlteration* alterations,
    size_t num_alterations
);

/* ─── lance_dataset_add_columns ───────────────────────────────────────────── */

/**
 * A single new column defined by a SQL expression over the dataset's existing
 * columns, e.g. { .name = "doubled", .expression = "x * 2" }. Both fields are
 * required, non-empty UTF-8, and are read by shared reference for the duration
 * of the call.
 */
typedef struct LanceSqlColumn {
    /* Name of the new column. Required, non-empty UTF-8. */
    const char* name;
    /* SQL expression evaluated against existing columns. Required, non-empty. */
    const char* expression;
} LanceSqlColumn;

/**
 * Add one or more columns computed from SQL expressions over the dataset's
 * existing columns, committing a new manifest. Each fragment is scanned, the
 * expressions are evaluated, and the results are written as new column files.
 *
 * Mutates `dataset` in place — the same handle remains valid afterward and
 * sees the new version. Scanners already in flight keep their pre-add view.
 *
 * @param dataset      Open dataset (not consumed). Mutated in place. Must not
 *                     be NULL.
 * @param columns      Array of `LanceSqlColumn`. Must not be NULL; each entry's
 *                     `name` and `expression` must be non-NULL and non-empty.
 * @param num_columns  Length of `columns`. Must be > 0.
 * @param batch_size   Rows per scan batch while evaluating expressions.
 *                     0 = upstream default.
 * @return 0 on success, -1 on error. Error codes:
 *         LANCE_ERR_INVALID_ARGUMENT for NULL/empty inputs, NULL or empty
 *         `name` / `expression`, non-UTF-8 strings, malformed SQL *syntax*, a
 *         new column name that collides with an existing column, an
 *         expression that references a non-existent column (an upstream
 *         schema error reclassified in Lance 9.1; previously
 *         LANCE_ERR_INTERNAL), or a `batch_size` beyond UINT32_MAX.
 *         LANCE_ERR_COMMIT_CONFLICT for a concurrent writer.
 */
int32_t lance_dataset_add_columns_sql(
    LanceDataset* dataset,
    const LanceSqlColumn* columns,
    size_t num_columns,
    uint64_t batch_size
);

/**
 * Add one or more all-null columns described by an Arrow C Data Interface
 * schema, committing a new manifest. On non-legacy datasets this is a
 * metadata-only operation — no data files are rewritten. Every field in the
 * schema must be nullable.
 *
 * Mutates `dataset` in place — the same handle remains valid afterward and
 * sees the new version. Scanners already in flight keep their pre-add view.
 *
 * @param dataset  Open dataset (not consumed). Mutated in place. Must not be
 *                 NULL.
 * @param schema   Arrow C `ArrowSchema` describing the new columns. Read by
 *                 shared reference; its `release` callback is never invoked.
 *                 Must not be NULL. Only the top-level schema is validated
 *                 before it is handed to arrow-rs; the caller is responsible for
 *                 providing fully-initialised child fields.
 * @return 0 on success, -1 on error. Error codes:
 *         LANCE_ERR_INVALID_ARGUMENT for a NULL dataset/schema, an
 *         uninitialised or already-released schema, an invalid Arrow schema, a
 *         non-nullable field, or a name that collides with an existing column.
 *         LANCE_ERR_NOT_SUPPORTED for a legacy-format dataset (which cannot take
 *         all-null columns as a metadata-only change).
 *         LANCE_ERR_COMMIT_CONFLICT for a concurrent writer.
 */
int32_t lance_dataset_add_columns_nulls(
    LanceDataset* dataset,
    const struct ArrowSchema* schema
);

/**
 * Add columns by splicing precomputed data from an Arrow C Data Interface
 * stream into the dataset, committing a new manifest. The stream's batches are
 * consumed in order and aligned positionally to the dataset's existing rows;
 * the total row count must match the dataset exactly.
 *
 * Mutates `dataset` in place — the same handle remains valid afterward and
 * sees the new version. Scanners already in flight keep their pre-add view.
 *
 * @param dataset     Open dataset (not consumed). Mutated in place. Must not
 *                    be NULL.
 * @param stream      Arrow C stream of new column data. When non-NULL it is
 *                    consumed (released) on every return path, including error
 *                    returns — the caller must not use it again. (A NULL stream
 *                    is rejected before anything is consumed.) Its schema
 *                    defines the new columns and must not collide with existing
 *                    column names.
 * @param batch_size  Rows per write batch while aligning the stream to
 *                    fragments. 0 = upstream default.
 * @return 0 on success, -1 on error. Error codes:
 *         LANCE_ERR_INVALID_ARGUMENT for a NULL dataset/stream, a stream missing
 *         a mandatory get_schema/get_next/release callback, a stream whose total
 *         row count does not match the dataset, a new column name that collides
 *         with an existing column, or a `batch_size` beyond UINT32_MAX.
 *         LANCE_ERR_COMMIT_CONFLICT for a concurrent writer.
 */
int32_t lance_dataset_add_columns_stream(
    LanceDataset* dataset,
    struct ArrowArrayStream* stream,
    uint64_t batch_size
);

/**
 * Export the dataset schema via Arrow C Data Interface.
 * @param out  Pointer to caller-allocated ArrowSchema struct
 * @return 0 on success, -1 on error
 */
int32_t lance_dataset_schema(
    const LanceDataset* dataset,
    struct ArrowSchema* out
);

/* ─── Fragment enumeration ─── */

/**
 * Return the number of fragments in the dataset. Returns 0 on error; a
 * dataset with no fragments also returns 0, so check lance_last_error_code().
 */
uint64_t lance_dataset_fragment_count(const LanceDataset* dataset);

/**
 * Fill out_ids with the fragment IDs of the dataset.
 * Caller must allocate out_ids with at least lance_dataset_fragment_count() elements.
 * @return 0 on success, -1 on error
 */
int32_t lance_dataset_fragment_ids(const LanceDataset* dataset, uint64_t* out_ids);

/* ─── Read-only fragment snapshots ─── */

/**
 * Resolve a fragment by ID from this dataset snapshot.
 *
 * The returned handle retains Native `FileFragment` snapshot ownership: it
 * remains valid, and keeps reading the same dataset version, after `dataset`
 * is closed or mutated.
 *
 * @param dataset      Open dataset snapshot.
 * @param fragment_id  Fragment ID from lance_dataset_fragment_ids().
 * @return Fragment handle, or NULL on error. A missing ID sets
 *         LANCE_ERR_NOT_FOUND; a NULL dataset sets
 *         LANCE_ERR_INVALID_ARGUMENT.
 */
LanceFragment* lance_dataset_open_fragment(
    const LanceDataset* dataset,
    uint64_t fragment_id
);

/** Close and free a fragment handle. Safe to call with NULL. */
void lance_fragment_close(LanceFragment* fragment);

/**
 * Return the fragment ID. Returns 0 and sets LANCE_ERR_INVALID_ARGUMENT on a
 * NULL handle.
 */
uint64_t lance_fragment_id(const LanceFragment* fragment);

/**
 * Return the number of Native base data files in the fragment. Overlay files
 * and the deletion file are not included. Returns 0 and sets an error on a
 * NULL handle.
 */
uint64_t lance_fragment_data_file_count(const LanceFragment* fragment);

/**
 * Return whether this handle retains a dataset-bound native fragment.
 *
 * Detached handles own only cloned native metadata, load no data files, and
 * reject dataset-dependent reads with LANCE_ERR_NOT_SUPPORTED. The output slot
 * is written only on success.
 *
 * @return 0 on success, -1 on error (including NULL fragment/out_is_bound).
 */
int32_t lance_fragment_is_bound(
    const LanceFragment* fragment,
    bool* out_is_bound
);

/**
 * Clone one native base data file from the fragment metadata.
 *
 * `index` addresses the native `Fragment::files` array. The returned handle
 * owns an independent copy and must be closed with `lance_data_file_close()`.
 *
 * @return 0 on success, -1 on error. A missing index sets
 *         LANCE_ERR_INVALID_ARGUMENT; `*out_file` is unchanged on error.
 */
int32_t lance_fragment_data_file_at(
    const LanceFragment* fragment,
    size_t index,
    LanceDataFile** out_file
);

/**
 * Find the native base data file that stores a field ID.
 *
 * Bound handles use Native `FileFragment::data_file_for_field`; detached
 * handles use the same `fields` membership rule on owned metadata. A missing
 * field succeeds with `*out_file == NULL`; a negative field ID or NULL output
 * slot is an invalid argument. The returned handle is independent and must be
 * closed with `lance_data_file_close()`.
 *
 * @return 0 on success, -1 on error; the output slot is unchanged on error.
 */
int32_t lance_fragment_data_file_for_field(
    const LanceFragment* fragment,
    int32_t field_id,
    LanceDataFile** out_file
);

/**
 * Serialize the Native fragment metadata as JSON.
 *
 * The object is Lance's `Fragment` serialization. It includes base data files
 * (path, field IDs, column indices, file version, size, base ID), overlays,
 * deletion-file metadata, row-id/version metadata, and physical_rows. Optional
 * unknown values remain null or omitted as Native serializes them; they are
 * never replaced with zero.
 *
 * The caller must free the returned string with lance_free_string(). Returns
 * NULL and sets an error for a NULL handle or serialization failure.
 */
const char* lance_fragment_metadata_json(const LanceFragment* fragment);

/**
 * Export the fragment's logical dataset schema via Arrow C Data Interface.
 *
 * This is FileFragment::schema(), the logical dataset schema, not a physical
 * file schema. `out` is caller-allocated and remains caller-owned; invoke its
 * non-NULL release callback exactly once.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_fragment_schema(
    const LanceFragment* fragment,
    struct ArrowSchema* out
);

/**
 * Read the physical row count, including deleted rows.
 *
 * Calls FileFragment::physical_rows(), which may perform data-file I/O when
 * the manifest has no trustworthy cached count. Native errors propagate.
 *
 * @param fragment  Fragment snapshot.
 * @param out_rows  Receives the count on success; unchanged on failure.
 * @return 0 on success, -1 on error (including NULL fragment/out_rows).
 */
int32_t lance_fragment_physical_rows(
    const LanceFragment* fragment,
    uint64_t* out_rows
);

/**
 * Read the deleted-row count.
 *
 * Calls FileFragment::count_deletions(), which may load the deletion vector
 * when its count is not cached in the manifest. Native errors propagate.
 *
 * @param fragment  Fragment snapshot.
 * @param out_rows  Receives the count on success; unchanged on failure.
 * @return 0 on success, -1 on error (including NULL fragment/out_rows).
 */
int32_t lance_fragment_deleted_rows(
    const LanceFragment* fragment,
    uint64_t* out_rows
);

/**
 * Read the logical row count.
 *
 * Calls FileFragment::count_rows(None) directly; the binding does not subtract
 * the deletion count itself. Native errors propagate.
 *
 * @param fragment  Fragment snapshot.
 * @param out_rows  Receives the count on success; unchanged on failure.
 * @return 0 on success, -1 on error (including NULL fragment/out_rows).
 */
int32_t lance_fragment_logical_rows(
    const LanceFragment* fragment,
    uint64_t* out_rows
);

/**
 * Start a scanner over the fragment's owned dataset snapshot.
 *
 * This calls Native `FileFragment::scan`, which uses the ordinary scanner path
 * with this fragment selected as the initial scan scope. The returned scanner
 * follows the normal scanner lifecycle and configuration rules. It remains
 * valid after the fragment and source dataset handles are closed.
 *
 * @param fragment  Fragment snapshot. Must not be NULL.
 * @return Scanner handle, or NULL on error.
 */
LanceScanner* lance_fragment_scan(const LanceFragment* fragment);

/**
 * Take fragment-local logical rows by index.
 *
 * Indices address logical rows in this fragment after deletion vectors are
 * resolved. They are not dataset `_rowid` values or physical row addresses.
 * Indices are forwarded unchanged to native `FileFragment::take`. Deletion
 * resolution, supported index ordering, duplicate handling, range validation,
 * projection and JSON conversion follow the selected core. Lance 14.0.2's
 * v2 decoder can reject unsorted or duplicate indices.
 *
 * `columns` is a NULL-terminated array of UTF-8 column paths:
 * - NULL selects the fragment's full logical schema.
 * - A non-NULL array whose first entry is NULL is an explicit empty
 *   projection and retains Native take's failure behavior instead of being
 *   treated as full. Missing top-level columns and non-UTF-8 names are errors;
 *   nested paths follow Native `Schema::project` semantics.
 * `indices` may be NULL only when `num_indices` is zero.
 *
 * On success `out` is initialized in caller-owned storage and must have its
 * non-NULL release callback invoked exactly once. The stream owns its result
 * and remains valid after the fragment and dataset handles are closed. Schema
 * validation and native take errors are reported before the callbacks are
 * exposed; deferred iteration and cleanup panics follow the existing guarded
 * Arrow stream contract. `out` is unchanged on failure.
 *
 * @param fragment     Fragment snapshot. Must not be NULL.
 * @param indices      Fragment-local logical row indices.
 * @param num_indices  Length of `indices`; zero is a valid empty request.
 * @param columns      NULL-terminated projected column paths, or NULL for all.
 * @param out          Pointer to caller-allocated ArrowArrayStream.
 * @return 0 on success, -1 on error.
 */
int32_t lance_fragment_take(
    const LanceFragment* fragment,
    const uint32_t* indices,
    size_t num_indices,
    const char* const* columns,
    struct ArrowArrayStream* out
);

/**
 * Count this fragment's rows, optionally applying a native SQL filter.
 *
 * NULL calls Native `FileFragment::count_rows(None)`. A non-NULL string,
 * including an empty string, is passed as `Some(filter)` without binding-side
 * pruning; Native validation and errors are preserved.
 *
 * @param fragment  Fragment snapshot. Must not be NULL.
 * @param filter    SQL filter expression, or NULL for no filter.
 * @param out_rows  Receives the count on success; unchanged on failure.
 * @return 0 on success, -1 on error.
 */
int32_t lance_fragment_count_rows(
    const LanceFragment* fragment,
    const char* filter,
    uint64_t* out_rows
);

/**
 * Delete rows from this fragment snapshot without publishing a transaction.
 *
 * This calls native `FileFragment::delete`. The input fragment is borrowed and
 * internally cloned, so it remains usable on every return path. The deletion
 * file, when needed, is written during this call. The dataset manifest changes
 * when the caller publishes the returned metadata in a Delete transaction.
 *
 * On success, `out_fragment` receives:
 * - a new independently owned fragment with the updated deletion-file
 *   metadata when some rows remain;
 * - NULL when every row was deleted.
 *
 * `out_fragment` is written only on success; it is unchanged on error. The
 * returned fragment owns its dataset snapshot and can be scanned, taken,
 * counted, closed, or used to build a Delete operation independently of the
 * source dataset. A staged scanner reads the new deletion file before
 * publication. A no-op predicate returns the original fragment metadata.
 *
 * `predicate` must be non-NULL UTF-8 SQL. An empty string and all predicate
 * semantics are passed to native Lance without binding-side pruning.
 *
 * @param fragment      Fragment snapshot. Must not be NULL.
 * @param predicate     SQL predicate. Must not be NULL.
 * @param out_fragment  Receives the fragment or NULL on success. Must not be NULL.
 * @return 0 on success, -1 on error.
 */
int32_t lance_fragment_delete(
    const LanceFragment* fragment,
    const char* predicate,
    LanceFragment** out_fragment
);

/* ─── Random access ─── */

/**
 * Take rows by indices.
 *
 * On success, `out` is initialized in caller-owned storage; the caller must
 * eventually invoke its non-NULL `release` callback exactly once. The schema
 * is validated before the stream callbacks are exposed. A deferred iteration
 * failure, including a caught panic in `get_next`, is reported through the
 * Arrow C stream contract (nonzero `get_next` plus `get_last_error`). A panic
 * during `release` cleanup is contained and logged; cleanup remains
 * best-effort.
 *
 * @param indices      Array of 0-based row offsets
 * @param num_indices  Length of indices array
 * @param columns      NULL-terminated column names, or NULL for all
 * @param out          Pointer to caller-allocated ArrowArrayStream
 * @return 0 on success, -1 on error
 */
int32_t lance_dataset_take(
    const LanceDataset* dataset,
    const uint64_t* indices,
    size_t num_indices,
    const char* const* columns,
    struct ArrowArrayStream* out
);

/**
 * Take rows by dataset row IDs.
 *
 * Row IDs are values from the `_rowid` scanner column, not zero-based row
 * offsets. They must belong to the same dataset snapshot used for this read.
 * Missing or deleted row IDs may be omitted from the result. For found rows,
 * input order and duplicates are preserved.
 *
 * On success, `out` is initialized in caller-owned storage; the caller must
 * eventually invoke its non-NULL `release` callback exactly once. The schema
 * is validated before the stream callbacks are exposed. A deferred iteration
 * failure, including a caught panic in `get_next`, is reported through the
 * Arrow C stream contract (nonzero `get_next` plus `get_last_error`). A panic
 * during `release` cleanup is contained and logged; cleanup remains
 * best-effort.
 *
 * @param dataset      Open dataset snapshot.
 * @param row_ids      Array of dataset row IDs. May be NULL only when
 *                     `num_row_ids` is zero.
 * @param num_row_ids  Length of `row_ids`.
 * @param columns      NULL-terminated column names, or NULL for all. The
 *                     system column `_rowid` may be requested explicitly.
 * @param out          Pointer to caller-allocated ArrowArrayStream.
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_take_rows(
    const LanceDataset* dataset,
    const uint64_t* row_ids,
    size_t num_row_ids,
    const char* const* columns,
    struct ArrowArrayStream* out
);

/* ─── Blob v2 random access ─── */

/*
 * A LanceBlobFile is a file-like handle over one value of a Blob v2 column,
 * returned by lance_dataset_take_blobs(), lance_dataset_take_blobs_by_indices()
 * or lance_dataset_take_blobs_by_addresses(), and released with
 * lance_blob_file_close(). It owns what it needs to read, so it stays valid
 * after the dataset is closed. Not thread-safe per handle; distinct handles
 * are independent.
 *
 * Reads are cursor-based: the cursor starts at 0, lance_blob_file_read() and
 * lance_blob_file_read_up_to() advance it, lance_blob_file_read_range() does
 * not, lance_blob_file_read_ranges() does not, lance_blob_file_seek() sets it.
 * lance_blob_file_mark_closed() changes the native file state without freeing
 * the opaque allocation; lance_blob_file_close() releases the allocation.
 */

/**
 * One blob-relative byte range for lance_blob_file_read_ranges().
 *
 * `offset` and `length` are interpreted against the start of the blob. Empty
 * ranges are valid.
 */
typedef struct LanceBlobRange {
    uint64_t offset;
    uint64_t length;
} LanceBlobRange;

/**
 * Take blob handles by dataset row ID.
 *
 * Row IDs are values from the `_rowid` scanner column, not zero-based row
 * offsets. They must belong to the same dataset snapshot used for this read.
 *
 * On success `out[i]` holds the handle for `row_ids[i]`, or NULL when that
 * blob value is null (an empty blob is a handle of size 0). The caller closes
 * every non-NULL handle exactly once. On failure `out` is left untouched; a
 * row ID that does not resolve fails the whole call.
 *
 * @param dataset      Open dataset snapshot.
 * @param row_ids      Array of dataset row IDs. May be NULL only when
 *                     `num_row_ids` is zero.
 * @param num_row_ids  Length of `row_ids`. Zero is a no-op that succeeds
 *                     without writing to `out`.
 * @param column       Name of a Blob v2 column. Must not be NULL. A missing
 *                     column, or a column that is not a blob column, is an
 *                     error.
 * @param out          Caller-allocated array of at least `num_row_ids`
 *                     handle pointers. Must not be NULL.
 * @return 0 on success, -1 on error
 */
int32_t lance_dataset_take_blobs(
    const LanceDataset* dataset,
    const uint64_t* row_ids,
    size_t num_row_ids,
    const char* column,
    LanceBlobFile** out
);

/**
 * Take blob handles by row index.
 *
 * Row indices are 0-based offsets in the dataset, as used by
 * lance_dataset_take(). Ownership, ordering, NULL slots, and failure
 * behavior are identical to lance_dataset_take_blobs().
 *
 * @param dataset      Open dataset snapshot.
 * @param indices      Array of 0-based row offsets. May be NULL only when
 *                     `num_indices` is zero.
 * @param num_indices  Length of `indices`. Zero is a no-op that succeeds
 *                     without writing to `out`.
 * @param column       Name of a Blob v2 column. Must not be NULL.
 * @param out          Caller-allocated array of at least `num_indices`
 *                     handle pointers. Must not be NULL.
 * @return 0 on success, -1 on error
 */
int32_t lance_dataset_take_blobs_by_indices(
    const LanceDataset* dataset,
    const uint64_t* indices,
    size_t num_indices,
    const char* column,
    LanceBlobFile** out
);

/**
 * Take blob handles by physical row address.
 *
 * A row address encodes `(fragment_id << 32) | row_offset`. These are
 * physical row addresses, such as those produced by a scan with row addresses
 * enabled; they are not stable row IDs from `_rowid`.
 *
 * On success `out[i]` holds the handle for `row_addresses[i]`, or NULL when
 * that blob value is null. A valid empty blob yields a non-NULL handle of size
 * zero. Handles preserve input order and duplicates, have independent cursors,
 * and remain usable after the dataset closes. On failure `out` is left
 * untouched.
 *
 * Unlike the older take entry points, an empty selection still validates
 * `column` through the native API. This includes rejecting a missing or
 * non-blob column.
 *
 * @param dataset            Open dataset snapshot.
 * @param row_addresses      Array of physical row addresses. May be NULL only
 *                           when `num_row_addresses` is zero.
 * @param num_row_addresses  Length of `row_addresses`.
 * @param column             Blob column to read. Must not be NULL.
 * @param out                Caller-allocated array of at least
 *                           `num_row_addresses` handle pointers. Must not be
 *                           NULL.
 * @return 0 on success, -1 on error
 */
int32_t lance_dataset_take_blobs_by_addresses(
    const LanceDataset* dataset,
    const uint64_t* row_addresses,
    size_t num_row_addresses,
    const char* column,
    LanceBlobFile** out
);

/**
 * Return the size of the blob in bytes.
 *
 * Metadata carried by the handle: no storage access, independent of the
 * cursor, still available after lance_dataset_close().
 *
 * @param blob  Blob handle. NULL is an error.
 * @return The blob size, or 0 on error. A return of 0 may be an empty blob
 *         or an error; check lance_last_error_code() to tell them apart.
 */
uint64_t lance_blob_file_size(const LanceBlobFile* blob);

/**
 * Read from the current cursor to the end of the blob.
 *
 * With the cursor at 0 that is the whole blob. The cursor ends up at the end.
 * `dst` must hold every remaining byte; a smaller buffer is an error and
 * reads nothing. At or past the end this writes nothing and succeeds.
 *
 * @param blob     Blob handle. NULL is an error.
 * @param dst      Destination buffer. May be NULL only when no bytes remain
 *                 from the current cursor.
 * @param dst_len  Capacity of `dst` in bytes. Must be at least the number of
 *                 bytes remaining from the cursor, or 0 if the cursor is at
 *                 or past the end.
 * @return 0 on success, -1 on error
 */
int32_t lance_blob_file_read(LanceBlobFile* blob, uint8_t* dst, size_t dst_len);

/**
 * Read at most `len` bytes from the current cursor.
 *
 * Reads `min(len, size - cursor)` bytes and advances the cursor past them,
 * so repeated calls walk the blob. At or past the end this writes no bytes,
 * stores 0 in `*bytes_read`, and succeeds.
 *
 * @param blob        Blob handle. NULL is an error.
 * @param dst         Destination buffer. May be NULL only when `len` is zero.
 * @param len         Maximum number of bytes to read.
 * @param bytes_read  Receives the number of bytes actually written to `dst`,
 *                    never more than `len`. Must not be NULL. Written only on
 *                    success.
 * @return 0 on success, -1 on error
 */
int32_t lance_blob_file_read_up_to(
    LanceBlobFile* blob,
    uint8_t* dst,
    size_t len,
    size_t* bytes_read
);

/**
 * Read exactly `len` bytes starting at `offset`, without moving the cursor.
 *
 * `offset` is blob-relative. A non-empty range that ends past the blob size,
 * or an `offset` plus `len` that overflows 64 bits, is an error; `len` 0
 * succeeds without checking `offset`. Nothing is written to `dst` on error.
 *
 * @param blob    Blob handle. NULL is an error.
 * @param offset  Byte offset from the start of the blob.
 * @param dst     Destination buffer of at least `len` bytes. May be NULL only
 *                when `len` is zero.
 * @param len     Number of bytes to read. Zero is a no-op that succeeds.
 * @return 0 on success, -1 on error
 */
int32_t lance_blob_file_read_range(
    const LanceBlobFile* blob,
    uint64_t offset,
    uint8_t* dst,
    size_t len
);

/**
 * Read multiple blob-relative ranges without moving the cursor.
 *
 * The result for `ranges[i]` is copied to `destinations[i]`. Input order,
 * duplicate ranges, overlaps and empty ranges are preserved. Each
 * `destination_lens[i]` is a capacity, not an exact length: it must be at
 * least `ranges[i].length`, and any excess capacity is left untouched.
 *
 * Every offset-plus-length is checked for 64-bit overflow. Blob bounds and the
 * closed state are validated by the native implementation for the complete
 * request before any destination is written. A request with `num_ranges == 0`
 * still reaches native closed-state validation, so it succeeds on an open
 * handle and fails on one marked closed.
 *
 * When `num_ranges` is zero, `ranges`, `destinations` and `destination_lens`
 * may be NULL. For a zero-length range, its destination may be NULL. For a
 * non-empty range, its destination must not be NULL. On any error no
 * destination is modified.
 *
 * @param blob              Blob handle. NULL is an error.
 * @param ranges            Array of ranges. May be NULL only when
 *                          `num_ranges` is zero.
 * @param num_ranges        Number of entries in each range/buffer array.
 * @param destinations      Parallel array of destination pointers.
 * @param destination_lens  Parallel array of destination capacities in bytes.
 * @return 0 on success, -1 on error
 */
int32_t lance_blob_file_read_ranges(
    const LanceBlobFile* blob,
    const LanceBlobRange* ranges,
    size_t num_ranges,
    uint8_t* const* destinations,
    const size_t* destination_lens
);

/**
 * Move the cursor to `pos`.
 *
 * Seeking past the end of the blob is allowed, mirroring the underlying Lance
 * API; a subsequent read then returns zero bytes.
 *
 * @param blob  Blob handle. NULL is an error.
 * @param pos   New cursor position, in bytes from the start of the blob.
 * @return 0 on success, -1 on error
 */
int32_t lance_blob_file_seek(LanceBlobFile* blob, uint64_t pos);

/**
 * Report the current cursor position.
 *
 * @param blob  Blob handle. NULL is an error.
 * @param pos   Receives the cursor position in bytes from the start of the
 *              blob. Must not be NULL. Written only on success.
 * @return 0 on success, -1 on error
 */
int32_t lance_blob_file_tell(const LanceBlobFile* blob, uint64_t* pos);

/**
 * Set the sequential read-ahead size used by future reads.
 *
 * Zero disables read-ahead. A changed value drops any unused prefetched bytes,
 * matching the native BlobFile behavior. The size is a hint and does not
 * change the visible blob bytes or cursor.
 *
 * @param blob         Blob handle. NULL is an error.
 * @param buffer_size  Read-ahead size in bytes; zero disables it.
 * @return 0 on success, -1 on error
 */
int32_t lance_blob_file_set_buffer_size(
    LanceBlobFile* blob,
    size_t buffer_size
);

/**
 * Mark the native blob file closed.
 *
 * This maps to the native BlobFile close operation. It is idempotent and does
 * not free the opaque allocation. After this succeeds, size() remains
 * available but cursor and range reads, tell() and set_buffer_size() follow
 * native closed-handle errors. Releasing the allocation still requires
 * lance_blob_file_close().
 *
 * The existing single-range zero-length compatibility is unchanged: a
 * zero-length lance_blob_file_read_range() request succeeds without reaching
 * the closed-state check.
 *
 * @param blob  Blob handle. NULL is an error.
 * @return 0 on success, -1 on error
 */
int32_t lance_blob_file_mark_closed(LanceBlobFile* blob);

/**
 * Report whether the native blob file is closed.
 *
 * @param blob        Blob handle. NULL is an error.
 * @param out_closed  Receives the native closed state. Must not be NULL.
 *                    Written only on success.
 * @return 0 on success, -1 on error
 */
int32_t lance_blob_file_is_closed(
    const LanceBlobFile* blob,
    bool* out_closed
);

/**
 * Free a blob handle's opaque allocation.
 *
 * This does not perform the native close operation; use
 * lance_blob_file_mark_closed() for that. Call exactly once per non-NULL
 * handle; the handle is invalid afterwards. Dropping the allocation also drops
 * the underlying native BlobFile. NULL is a no-op. Never fails and leaves the
 * pending error untouched.
 *
 * @param blob  Blob handle, or NULL.
 */
void lance_blob_file_close(LanceBlobFile* blob);

/* ─── Blob v2 fields and prepared descriptor arrays ─── */

/**
 * Native storage thresholds for a blob v2 field.
 *
 * Each optional threshold is selected by its `has_*` flag. An enabled inline
 * threshold of zero is valid. An enabled dedicated threshold must be non-zero
 * because native `NonZeroUsize` cannot represent zero. Values wider than the
 * platform `size_t` are rejected rather than truncated. A zero-initialized
 * struct selects native defaults (both thresholds absent).
 */
typedef struct LanceBlobFieldOptions {
    bool has_inline_size_threshold;
    uint64_t inline_size_threshold;
    bool has_dedicated_size_threshold;
    uint64_t dedicated_size_threshold;
} LanceBlobFieldOptions;

/**
 * Export the native blob v2 field with default storage thresholds.
 *
 * The exported field is the complete native `lance::blob_field` result:
 * `Struct<data: LargeBinary?, uri: Utf8?>`, extension name
 * `lance.blob.v2`, and its native field metadata. The caller owns `out` and
 * must invoke its non-NULL release callback exactly once. `out` is written
 * only on success.
 *
 * @param name      Field name. May be empty but must not be NULL.
 * @param nullable  Native field nullability.
 * @param out       Caller-owned ArrowSchema. Must not be NULL.
 * @return 0 on success, -1 on error.
 */
int32_t lance_blob_field(
    const char* name,
    bool nullable,
    struct ArrowSchema* out
);

/**
 * Export the native blob v2 field with explicit storage thresholds.
 *
 * `options == NULL` selects native defaults. Threshold conversion follows
 * LanceBlobFieldOptions. The result is otherwise identical to
 * lance_blob_field().
 *
 * @param name      Field name. May be empty but must not be NULL.
 * @param nullable  Native field nullability.
 * @param options   Optional threshold options, or NULL for defaults.
 * @param out       Caller-owned ArrowSchema. Must not be NULL.
 * @return 0 on success, -1 on error.
 */
int32_t lance_blob_field_with_options(
    const char* name,
    bool nullable,
    const LanceBlobFieldOptions* options,
    struct ArrowSchema* out
);

/**
 * Create a native writer-prepared blob descriptor array builder.
 *
 * This wraps `lance::BlobDescriptorArrayBuilder`. It builds the prepared
 * struct representation consumed by dataset writes; it does not allocate blob
 * IDs, write payload objects, choose sidecar paths, or commit data.
 *
 * @param column  Blob column name. Must not be NULL.
 * @return Builder handle, or NULL on error.
 */
LanceBlobDescriptorArrayBuilder* lance_blob_descriptor_array_builder_new(
    const char* column
);

/**
 * Export the complete native prepared field for this column.
 *
 * The field remains exportable after a successful or failed finish. `out` is
 * caller-owned and is written only on success.
 *
 * @param builder  Builder handle. Must not be NULL.
 * @param out      Caller-owned ArrowSchema. Must not be NULL.
 * @return 0 on success, -1 on error.
 */
int32_t lance_blob_descriptor_array_builder_field(
    const LanceBlobDescriptorArrayBuilder* builder,
    struct ArrowSchema* out
);

/**
 * Append an inline payload.
 *
 * `data == NULL && len == 0` is a valid empty inline blob. A NULL data pointer
 * with non-zero len is an error. Foreign bytes are copied before the call
 * returns.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_blob_descriptor_array_builder_append_inline(
    LanceBlobDescriptorArrayBuilder* builder,
    const uint8_t* data,
    size_t len
);

/**
 * Append a null blob descriptor.
 * @return 0 on success, -1 on error.
 */
int32_t lance_blob_descriptor_array_builder_append_null(
    LanceBlobDescriptorArrayBuilder* builder
);

/**
 * Append one packed descriptor for `offset..offset + length`.
 *
 * `length` maps to native `BlobRange.size`. Native validates the blob ID and
 * the 64-bit range before appending.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_blob_descriptor_array_builder_append_packed(
    LanceBlobDescriptorArrayBuilder* builder,
    uint32_t blob_id,
    uint64_t offset,
    uint64_t length
);

/**
 * Append one packed descriptor per range in input order.
 *
 * Native pushes one range at a time. If a later range is invalid, all ranges
 * before it remain appended and the error is returned. `ranges` may be NULL
 * only when `num_ranges == 0`.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_blob_descriptor_array_builder_extend_packed(
    LanceBlobDescriptorArrayBuilder* builder,
    uint32_t blob_id,
    const LanceBlobRange* ranges,
    size_t num_ranges
);

/**
 * Append one dedicated descriptor. Native validates the blob ID.
 * @return 0 on success, -1 on error.
 */
int32_t lance_blob_descriptor_array_builder_append_dedicated(
    LanceBlobDescriptorArrayBuilder* builder,
    uint32_t blob_id,
    uint64_t length
);

/**
 * Append one Managed descriptor.
 *
 * `has_base_id == false` maps to native `base_id: None`; true maps to
 * `Some(base_id)`, including explicit base ID 0. Native validates that `uri`
 * is a canonical relative object path and that `offset + length` does not
 * overflow. Whether the base is registered and whether the dataset schema opts
 * in through `lance.blob.managed=true` are validated by the native dataset
 * write path, not by this builder.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_blob_descriptor_array_builder_append_managed(
    LanceBlobDescriptorArrayBuilder* builder,
    const char* uri,
    uint64_t offset,
    uint64_t length,
    bool has_base_id,
    uint32_t base_id
);

/**
 * Append one External descriptor with an explicit native base ID.
 *
 * Native rejects an empty URI and an overflowing `offset + length`.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_blob_descriptor_array_builder_append_external(
    LanceBlobDescriptorArrayBuilder* builder,
    uint32_t base_id,
    const char* uri,
    uint64_t offset,
    uint64_t length
);

/**
 * Append an External descriptor through native `push_external`.
 *
 * This convenience sets native `base_id = 0`. When `has_range` is false,
 * native receives `None` and stores offset 0, length 0. When true, the range is
 * passed through, including an explicit 0/0 range. `offset` and `length` are
 * ignored when `has_range` is false.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_blob_descriptor_array_builder_push_external(
    LanceBlobDescriptorArrayBuilder* builder,
    const char* uri,
    bool has_range,
    uint64_t offset,
    uint64_t length
);

/**
 * Finish the native prepared descriptor column and export both parts.
 *
 * This consumes the native builder. On every return path, later append calls
 * report that the builder is finished; `field` remains available. On success
 * `out_array` receives an owned ArrowArray and `out_schema` receives the
 * complete native prepared field (including extension and threshold
 * metadata). The caller must release each non-NULL output exactly once.
 *
 * Both outputs are written only after the complete native finish and both FFI
 * conversions succeed. On error neither output is modified.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_blob_descriptor_array_builder_finish(
    LanceBlobDescriptorArrayBuilder* builder,
    struct ArrowArray* out_array,
    struct ArrowSchema* out_schema
);

/**
 * Free a descriptor builder handle. NULL is a no-op. An unfinished native
 * builder is dropped without sidecar rollback or publication.
 */
void lance_blob_descriptor_array_builder_close(
    LanceBlobDescriptorArrayBuilder* builder
);

/* ─── Planned whole-blob and blob-range reads ─── */

/**
 * Row selector for planned blob reads. The C ABI passes this as an int32_t,
 * so callers built with non-default enum sizing cannot mismatch the Rust ABI.
 *
 * NONE leaves the native builder without a selection and therefore preserves
 * the native "requires a row selection" error. IDS, ADDRESSES and INDICES with
 * a zero count are explicit empty selections and produce an empty stream.
 */
typedef enum {
    LANCE_BLOB_ROWS_NONE      = 0,
    LANCE_BLOB_ROWS_IDS       = 1,
    LANCE_BLOB_ROWS_ADDRESSES = 2,
    LANCE_BLOB_ROWS_INDICES   = 3,
} LanceBlobRowSelector;

/**
 * Result ordering for planned blob reads.
 */
typedef enum {
    LANCE_BLOB_READ_PRESERVE_ORDER = 0,
    LANCE_BLOB_READ_UNORDERED      = 1,
} LanceBlobReadOrder;

/**
 * Optional controls for planned blob reads.
 *
 * A NULL options pointer means `io_buffer_size = None` and native
 * `preserve_order = true`. `has_io_buffer_size = true` forwards the value
 * verbatim; zero is an explicit native value and is rejected by the native
 * builder rather than replaced with a default. `order` is a
 * LanceBlobReadOrder value.
 */
typedef struct LanceBlobReadOptions {
    bool has_io_buffer_size;
    uint64_t io_buffer_size;
    int32_t order;
} LanceBlobReadOptions;

/**
 * One row-specific blob-range request.
 *
 * `row` has the meaning selected by LanceBlobRowSelector. `offset` and
 * `length` are blob-local bytes and map directly to native
 * `BlobReadRange.offset/length`.
 */
typedef struct LanceBlobRangeRequest {
    uint64_t row;
    uint64_t offset;
    uint64_t length;
} LanceBlobRangeRequest;

/**
 * Plan and export whole-blob reads.
 *
 * The returned Arrow stream schema is, in order:
 * 1. `row_address: UInt64`, non-null
 * 2. `data: LargeBinary`, nullable
 *
 * Exactly one Arrow record follows each native `ReadBlob` result. NULL blob
 * values have a null `data` entry; valid empty blobs have a non-null empty
 * buffer. Native ordering, duplicate selection and payload bytes are
 * preserved. Selected input rows are copied before native state retains them.
 * Native payload `Bytes` are retained directly by the Arrow buffers; payload
 * bytes are not copied when an Arrow record is assembled.
 *
 * Column lookup and read planning run in this factory and their errors are
 * returned before any output stream exists. Payload I/O errors happen while
 * consuming the stream and use the ArrowArrayStream error contract
 * (`get_next` nonzero plus `get_last_error`). Closing the dataset handle does
 * not invalidate a successfully created stream. Exported batches own their
 * buffers and remain valid after the stream is released.
 *
 * `out` is written only after successful native planning and Arrow stream
 * export, and must then be released exactly once. On error it is untouched.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_read_blobs(
    const LanceDataset* dataset,
    const char* column,
    int32_t selector,
    const uint64_t* rows,
    size_t num_rows,
    const LanceBlobReadOptions* options,
    struct ArrowArrayStream* out
);

/**
 * Plan and export one blob-local range read per request.
 *
 * The returned Arrow stream schema is, in order:
 * 1. `request_index: UInt64`, non-null
 * 2. `row_address: UInt64`, non-null
 * 3. `offset: UInt64`, non-null
 * 4. `length: UInt64`, non-null
 * 5. `data: LargeBinary`, nullable
 *
 * Exactly one Arrow record follows each native `ReadBlobRange`. `request_index`
 * is the zero-based input position and remains present for duplicate and
 * unordered requests. NULL blobs have null `data`; a non-null blob with an
 * empty range has a non-null empty buffer.
 * Native payload `Bytes` are retained directly by the Arrow buffers; payload
 * bytes are not copied when an Arrow record is assembled.
 *
 * Native validation stages are retained. Request offset-plus-length overflow,
 * scheduler buffer validation and physical range planning happen in the
 * factory. Native null-blob requests skip logical bounds checks. Payload I/O
 * errors happen while consuming the stream and use the ArrowArrayStream error
 * contract. Exported batches own their buffers and remain valid after the
 * stream is released.
 *
 * Planning retains O(request_count) metadata. `io_buffer_size` bounds each
 * scheduler-visible physical batch after range coalescing, except that one
 * request larger than the buffer is materialized by itself. The scheduler
 * separately applies byte backpressure to physical I/O; this is not a hard cap
 * on all process memory.
 *
 * `out` is written only after successful native planning and Arrow stream
 * export, and must then be released exactly once. On error it is untouched.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_read_blob_ranges(
    const LanceDataset* dataset,
    const char* column,
    int32_t selector,
    const LanceBlobRangeRequest* requests,
    size_t num_requests,
    const LanceBlobReadOptions* options,
    struct ArrowArrayStream* out
);

/* ─── Scanner builder ─── */

/**
 * Create a scanner for the dataset.
 * @param dataset  Open dataset (not consumed)
 * @param columns  NULL-terminated column names, or NULL for all
 * @param filter   SQL filter expression, or NULL
 * @return Scanner handle, or NULL on error
 */
LanceScanner* lance_scanner_new(
    const LanceDataset* dataset,
    const char* const* columns,
    const char* filter
);

int32_t lance_scanner_set_limit(LanceScanner* scanner, int64_t limit);
int32_t lance_scanner_set_offset(LanceScanner* scanner, int64_t offset);
int32_t lance_scanner_set_batch_size(LanceScanner* scanner, int64_t batch_size);

/**
 * Set the target output batch size in bytes.
 *
 * When set, this takes precedence over the row-based batch size. The value
 * must be greater than zero and must be set before scanning starts. The call
 * is rejected without changing scanner state if strict batch sizing is enabled.
 */
int32_t lance_scanner_set_batch_size_bytes(
    LanceScanner* scanner,
    uint64_t batch_size_bytes
);

/**
 * Set the scanner I/O buffer size in bytes.
 *
 * The value must be between 1 and INT64_MAX, inclusive, and must be set before
 * scanning starts. This bounds buffered I/O received from storage, but is not
 * a hard limit on all memory used by the scanner.
 *
 * @param scanner               Scanner handle. Must not be NULL.
 * @param io_buffer_size_bytes  I/O buffer size in bytes, in the range [1, INT64_MAX].
 * @return 0 on success, -1 on error.
 */
int32_t lance_scanner_set_io_buffer_size(
    LanceScanner* scanner,
    uint64_t io_buffer_size_bytes
);

/**
 * Set the maximum number of batches decoded concurrently.
 *
 * @param batch_readahead  Number of in-flight batch decode tasks. Must be greater than zero.
 */
int32_t lance_scanner_set_batch_readahead(
    LanceScanner* scanner,
    size_t batch_readahead
);

/**
 * Set fragment readahead for unordered scans.
 *
 * This setting is only used when scan-in-order is disabled. The value must be
 * greater than zero.
 */
int32_t lance_scanner_set_fragment_readahead(
    LanceScanner* scanner,
    size_t fragment_readahead
);

/**
 * Set the target number of physical execution partitions.
 *
 * This controls the partition count used by the physical optimizer and can be
 * used to bound scan CPU parallelism. The value must be greater than zero and
 * must be set before scanning starts.
 */
int32_t lance_scanner_set_target_parallelism(
    LanceScanner* scanner,
    size_t target_parallelism
);

/**
 * Configure whether batches are returned in storage order (default: true).
 *
 * Disabling ordering can improve throughput by returning batches as soon as
 * they are ready.
 */
int32_t lance_scanner_set_scan_in_order(LanceScanner* scanner, bool scan_in_order);

/**
 * Configure whether scalar indices may be used to optimize filters.
 *
 * Scalar indices are enabled by default. Disable this to force filter
 * evaluation without scalar indices, including an explicitly selected scalar
 * segment (which falls back to a scan of its explicit fragment_ids).
 * This setting is independent of
 * `lance_scanner_set_use_index`, which controls vector ANN index usage.
 * Must be set before scanning starts.
 */
int32_t lance_scanner_set_use_scalar_index(
    LanceScanner* scanner,
    bool use_scalar_index
);

/**
 * Configure whether row-based output batches are strict.
 *
 * When enabled, every batch except the last has exactly the configured row
 * batch size. This may require copying and cannot be combined with a byte-based
 * batch-size limit. The call is rejected without changing scanner state if a
 * byte limit is already set. Must be set before scanning starts.
 */
int32_t lance_scanner_set_strict_batch_size(
    LanceScanner* scanner,
    bool strict_batch_size
);

/**
 * Configure whether file statistics may optimize the scan (default: true).
 * Intended primarily for debugging and benchmarking. Must be set before
 * scanning starts.
 */
int32_t lance_scanner_set_use_stats(LanceScanner* scanner, bool use_stats);

/** Sort direction for one scanner ordering key. */
typedef enum {
    LANCE_ORDER_ASCENDING = 0,
    LANCE_ORDER_DESCENDING = 1,
} LanceOrderDirection;

/** Placement of NULL values for one scanner ordering key. */
typedef enum {
    LANCE_ORDER_NULLS_LAST = 0,
    LANCE_ORDER_NULLS_FIRST = 1,
} LanceNullOrder;

/**
 * One native scanner ordering key.
 *
 * `direction` is a LanceOrderDirection value and `null_order` is a
 * LanceNullOrder value. Both are int32_t at the ABI boundary so out-of-range
 * values can be rejected safely. `column_name` is copied during the call.
 */
typedef struct LanceColumnOrdering {
    const char* column_name;
    int32_t direction;
    int32_t null_order;
} LanceColumnOrdering;

/**
 * One ordered scanner projection entry.
 *
 * `output_name` is the output field name and may be an alias. `expression` is
 * a Lance SQL expression evaluated against the dataset schema. Both strings
 * are copied during `lance_scanner_set_projection()`.
 */
typedef struct LanceProjectionColumn {
    const char* output_name;
    const char* expression;
} LanceProjectionColumn;

/**
 * Replace the scanner projection with ordered output-name / SQL-expression
 * pairs, mirroring Lance `Scanner::project_with_transform()`.
 *
 * Each output name may be an alias. Each expression may reference stored
 * columns, metadata columns, nested field paths, and query-generated scoring
 * columns according to the selected Lance version. Output order follows the
 * input array. Lance parses and validates expressions, duplicate output names,
 * and schema-dependent expression resolution during this call. Planning-only
 * errors, such as an unsupported projection with no physical output column, are
 * reported when the scan starts.
 *
 * Passing `columns == NULL` with `len == 0` clears the projection and restores
 * the native unset projection, which selects all dataset columns. Passing a
 * non-NULL pointer with `len == 0` sets an explicit empty projection. A NULL
 * pointer with `len > 0` is invalid.
 *
 * The array and every string are copied before this call returns. A NULL
 * output name or expression, non-UTF-8 input, or a projection rejected by
 * Lance leaves the previous projection unchanged. This does not alter the
 * column-list projection accepted by `lance_scanner_new()`; that path keeps
 * Lance's escaped field-path behavior.
 *
 * Must be called before the scan starts.
 *
 * @param scanner  Scanner handle. Must not be NULL.
 * @param columns  Projection entries, or NULL to clear.
 * @param len      Number of projection entries.
 * @return 0 on success, -1 on error.
 */
int32_t lance_scanner_set_projection(
    LanceScanner* scanner,
    const LanceProjectionColumn* columns,
    size_t len
);

/**
 * Disable automatic `_score` / `_distance` projection for this scanner.
 *
 * With the published default, an explicit projection still receives `_score`
 * for full-text search or `_distance` for vector and MinHash search. After this
 * call succeeds, those columns are included only when explicitly requested by
 * the projection, matching Lance `Scanner::disable_scoring_autoprojection()`.
 *
 * The change is one-way for the lifetime of the scanner. Must be called before
 * the scan starts.
 *
 * @param scanner  Scanner handle. Must not be NULL.
 * @return 0 on success, -1 on error.
 */
int32_t lance_scanner_disable_scoring_autoprojection(LanceScanner* scanner);

/**
 * Replace the scanner's native result ordering.
 *
 * Entries are applied in array order, matching Lance's ColumnOrdering list.
 * With limit/offset, Lance sorts all rows before applying the window. For
 * nearest or full-text search, Lance first selects its query result set and
 * then sorts that set.
 *
 * Passing `orderings == NULL` with `len == 0` clears the ordering (the native
 * `None` option). Passing a non-NULL pointer with `len == 0` is an explicit
 * empty ordering list (the native `Some(empty)` option); Lance treats it as
 * clearing the ordering. Both forms are accepted. `orderings == NULL` with
 * `len > 0` is invalid.
 *
 * The array, every column name, and all scalar fields are copied before this
 * call returns. A NULL column name, an invalid direction, or an invalid
 * null_order is rejected without changing the previous ordering. Column names
 * are validated by Lance when the scan is materialized, so a missing column is
 * reported by starting the scan. Must be called before the scan starts.
 *
 * The ordering cannot be combined with `lance_scanner_nearest_multivector`:
 * that API's result window is always restored to distance order, so accepting
 * an ordering would silently ignore it. Materializing such a scan fails with
 * LANCE_ERR_INVALID_ARGUMENT.
 *
 * @param scanner    Scanner handle. Must not be NULL.
 * @param orderings  Ordering keys, or NULL to clear.
 * @param len        Number of ordering keys.
 * @return 0 on success, -1 on error.
 */
int32_t lance_scanner_set_order_by(
    LanceScanner* scanner,
    const LanceColumnOrdering* orderings,
    size_t len
);

int32_t lance_scanner_with_row_id(LanceScanner* scanner, bool enable);

/** Include or omit the `_rowaddr` metadata column. Must be set before scanning. */
int32_t lance_scanner_with_row_address(LanceScanner* scanner, bool enable);

/**
 * Configure whether deleted rows still present in storage are returned.
 * Requires with_row_id=true; deleted rows have a NULL `_rowid`.
 * For filtered scans, also set use_scalar_index=false: indices built after a
 * deletion may omit tombstoned rows. Incompatible with scalar_index_segment,
 * even when scalar indices are disabled.
 * Fragments removed from the current snapshot are not scanned.
 * Must be set before scanning starts.
 */
int32_t lance_scanner_set_include_deleted_rows(
    LanceScanner* scanner,
    bool include_deleted_rows
);

/** How blob columns are materialized by a scan. Validated as an integer. */
typedef enum {
    /**
     * Default: blob columns are returned as descriptor structs and every
     * other binary column is returned as bytes. The descriptor layout
     * depends on the storage format of the column: Blob v2 columns yield
     * (kind, position, size, blob_id, blob_uri), while legacy blob columns
     * (large_binary tagged `lance-encoding: blob`) yield (position, size).
     */
    LANCE_BLOB_HANDLING_BLOBS_DESCRIPTIONS = 0,
    /** Every blob column is materialized as bytes (LargeBinary). */
    LANCE_BLOB_HANDLING_ALL_BINARY = 1,
    /**
     * Requests descriptors for every binary column. On lance v11.0.0 only
     * columns carrying blob metadata are affected; other binary columns keep
     * their bytes, so this behaves like
     * LANCE_BLOB_HANDLING_BLOBS_DESCRIPTIONS.
     */
    LANCE_BLOB_HANDLING_ALL_DESCRIPTIONS = 2,
} LanceBlobHandling;

/**
 * Choose how blob columns are materialized by this scan. Default:
 * LANCE_BLOB_HANDLING_BLOBS_DESCRIPTIONS. ALL_BINARY pulls the full payload
 * into the batches, so keep descriptors for large values. Columns without
 * blob metadata keep their bytes under every mode.
 *
 * Must be set before scanning starts; values outside the enum are rejected.
 * @return 0 on success, -1 on error
 */
int32_t lance_scanner_set_blob_handling(LanceScanner* scanner, LanceBlobHandling handling);

/**
 * Materialization style for ordinary scanner payload columns.
 *
 * This follows Lance's native materialization policy. It affects columns that
 * are not required for filtering. Vector search and full-text search always use
 * their native late-materialization paths.
 */
typedef enum {
    /** Native heuristic based on storage type and column width (default). */
    LANCE_MATERIALIZATION_STYLE_HEURISTIC = 0,
    /** Fetch projected payload columns for matching rows only where possible. */
    LANCE_MATERIALIZATION_STYLE_ALL_LATE = 1,
    /** Fetch every projected payload column eagerly where possible. */
    LANCE_MATERIALIZATION_STYLE_ALL_EARLY = 2,
    /**
     * Fetch every projected payload column eagerly except the named columns.
     * Names use Lance's schema projection paths, including nested paths.
     */
    LANCE_MATERIALIZATION_STYLE_ALL_EARLY_EXCEPT = 3,
} LanceMaterializationStyle;

/**
 * Configure native materialization for ordinary scanner payload columns.
 *
 * For ALL_EARLY_EXCEPT, `early_except_columns` contains
 * `early_except_column_count` column paths to exclude from early materialization.
 * Passing a NULL array with count 0 is an explicit empty exception list, so all
 * columns are early. Lance resolves the names against the dataset schema at call
 * time, including nested paths. Resolution and validation follow Lance's native
 * schema projection. The strings and array are copied; the caller may release
 * them when this function returns.
 *
 * For HEURISTIC, ALL_LATE, and ALL_EARLY, the column count must be 0. Other style
 * values are rejected. A rejected call leaves the previous style unchanged.
 * The style must be configured before the scan starts.
 *
 * @param scanner                   Scanner handle. Must not be NULL.
 * @param style                     One of `LanceMaterializationStyle`.
 * @param early_except_columns      Column paths for ALL_EARLY_EXCEPT; may be NULL
 *                                  when `early_except_column_count` is 0.
 * @param early_except_column_count Number of column path pointers.
 * @return 0 on success, -1 on error (check `lance_last_error_*`).
 */
int32_t lance_scanner_set_materialization_style(
    LanceScanner* scanner,
    LanceMaterializationStyle style,
    const char* const* early_except_columns,
    size_t early_except_column_count
);

/**
 * Restrict the scan/search domain to the given fragment IDs. For nearest-neighbor
 * queries, this limits the candidates searched before Top-K regardless of the
 * prefilter setting; it is not a predicate applied to ranked results.
 *
 * Calling this replaces any initial metadata selection from
 * `lance_fragment_scan()`, including staged deletion metadata. A zero-length
 * selection selects zero fragments. To select every fragment in the scanner's
 * dataset snapshot, supply all of its native fragment IDs explicitly.
 * Must be called before iteration.
 * @param ids  Array of fragment IDs
 * @param len  Number of fragment IDs
 * @return 0 on success, -1 on error
 */
int32_t lance_scanner_set_fragment_ids(
    LanceScanner* scanner,
    const uint64_t* ids,
    size_t len
);

/**
 * Restrict the scan or search to rows whose `_rowid` is in `row_ids`.
 *
 * The ids are in the dataset's `_rowid` space: the stable row id when stable
 * row ids are enabled, otherwise the row address. The allowlist is applied
 * before ranking: a nearest or full-text search (including one using a
 * prepared FTS query context) computes its top-k over the allowed rows only,
 * and a plain scan reads only those rows. It is combined with any filter
 * expression (AND) and with a blocklist (allowed AND NOT blocked). It cannot be
 * combined with lance_scanner_set_scalar_index_segment().
 * Calling it again replaces the previous allowlist. Must be called before
 * iteration; the ids are copied.
 * @param row_ids  Array of `_rowid` values, in any order (may be NULL when len is 0)
 * @param len      Number of ids; 0 allows no rows
 * @return 0 on success, -1 on error
 */
int32_t lance_scanner_set_row_addr_allowlist(
    LanceScanner* scanner,
    const uint64_t* row_ids,
    size_t len
);

/**
 * Exclude rows whose `_rowid` is in `row_ids` from the scan or search.
 * Same `_rowid` space, ordering, and restrictions as
 * lance_scanner_set_row_addr_allowlist().
 * @param row_ids  Array of `_rowid` values, in any order (may be NULL when len is 0)
 * @param len      Number of ids; 0 blocks no rows
 * @return 0 on success, -1 on error
 */
int32_t lance_scanner_set_row_addr_blocklist(
    LanceScanner* scanner,
    const uint64_t* row_ids,
    size_t len
);

/**
 * Set a Substrait filter on the scanner.
 *
 * `bytes` must point to a serialized Substrait `ExtendedExpression` message
 * containing exactly one expression of boolean type. This is the preferred
 * filter API for query engines that already speak Substrait — it avoids the
 * round-trip through SQL string formatting and parsing.
 *
 * If both this and the SQL filter passed to `lance_scanner_new` are set, the
 * Substrait filter wins. Calling this with the same scanner more than once
 * replaces the previously-set Substrait filter. The bytes are copied; the
 * caller may free them after this call returns.
 *
 * @param bytes  Serialized Substrait `ExtendedExpression` bytes (must not be NULL)
 * @param len    Length of the byte buffer (must be > 0)
 * @return 0 on success, -1 on error
 */
int32_t lance_scanner_set_substrait_filter(
    LanceScanner* scanner,
    const uint8_t* bytes,
    size_t len
);

/**
 * Set a Substrait aggregate on the scanner.
 *
 * `bytes` must point to a serialized Substrait `Plan` whose first relation is
 * an `AggregateRel`, either directly or through a `RelRoot`. This is a Plan,
 * not the `ExtendedExpression` accepted by
 * lance_scanner_set_substrait_filter(). Field references use positions in the
 * dataset schema, not projection positions. Group columns and measures follow
 * the native planner; `RelRoot.names` supplies output aliases in group-then-
 * measure order.
 *
 * The plan is validated immediately against the dataset schema using a
 * temporary native scanner. A failed replacement retains the previous
 * aggregate. Malformed protobuf bytes report the native I/O error; valid
 * non-aggregate plans and invalid field references report the native invalid-
 * argument errors. Empty bytes are passed to native plan validation.
 *
 * The bytes are copied. NULL is accepted only when `len` is zero. The setter
 * must be called before the scan starts. Planning observes the configured
 * projection, filter, fragments, masks, FTS, vector search, batch size, and
 * native limit/offset/ordering. Native aggregate validation rejects explicit
 * limit, offset, or non-empty ordering. Count and analyze-count replace this
 * aggregate with native `COUNT(*)`; schema, explain, analyze, and ordinary scan
 * streams use it.
 *
 * @param bytes  Serialized Substrait `Plan` bytes (NULL only when len is zero)
 * @param len    Length of the byte buffer
 * @return 0 on success, -1 on error
 */
int32_t lance_scanner_set_substrait_aggregate(
    LanceScanner* scanner,
    const uint8_t* bytes,
    size_t len
);

/**
 * Add an SQL filter that is combined with the selected primary filter using
 * AND. The primary filter is the Substrait filter when set, otherwise it is
 * the SQL filter passed to `lance_scanner_new`. Multiple additional SQL
 * filters are also combined using AND.
 *
 * Must be called before the scan starts. The filter string is copied.
 *
 * @param filter  Non-NULL, non-empty SQL filter expression
 * @return 0 on success, -1 on error
 */
int32_t lance_scanner_additional_sql_filter(
    LanceScanner* scanner,
    const char* filter
);

/** Type of a dynamically named scan metric. */
typedef enum {
    LANCE_SCAN_METRIC_COUNT = 0,
    LANCE_SCAN_METRIC_TIME_NANOSECONDS = 1,
} LanceScanMetricKind;

/**
 * Borrowed view of one dynamically named scan metric.
 *
 * `name` is not NUL-terminated. `name` and this structure are valid only for
 * the duration of the LanceScanStatisticsCallback invocation. Metric order is
 * unspecified.
 */
typedef struct {
    const char* name;
    size_t name_len;
    LanceScanMetricKind kind;
    uint64_t value;
} LanceScanMetric;

/**
 * Borrowed view of the execution statistics for one fully consumed scan.
 *
 * The fixed fields are stable summary metrics. `metrics` contains additional
 * implementation-specific counters and timings. Those names are not a stable
 * API and are intended for diagnostics and profiles. Dynamic metrics are
 * best-effort and may be omitted if they cannot be materialized. `metrics` is
 * NULL when `metrics_len` is zero.
 */
typedef struct {
    uint64_t iops;
    uint64_t requests;
    uint64_t bytes_read;
    uint64_t indices_loaded;
    uint64_t index_partitions_loaded;
    uint64_t index_comparisons;
    const LanceScanMetric* metrics;
    size_t metrics_len;
} LanceScanStatistics;

/**
 * Receives scan statistics after a stream is fully consumed to EOF.
 *
 * `statistics` is non-NULL. It and all nested pointers are borrowed and valid
 * only for the duration of this call. The callback may run on the thread that
 * observes EOF and must therefore be thread-safe. It must return normally
 * without throwing an exception or unwinding, and must not call any
 * `lance_scanner_*` function with the originating scanner.
 *
 * From callback entry until the enclosing operation that observes EOF has
 * returned to its caller, the callback must not directly or indirectly cause
 * `get_schema`, `get_next`, `get_last_error`, or `release` to be called on any
 * ArrowArrayStream derived from the originating scanner, nor cause such a
 * stream to be moved, destroyed, or otherwise accessed. This includes signaling
 * or scheduling another thread to act based only on callback completion: the
 * callback returns before the enclosing stream operation does. Such interaction
 * is reentrant and has undefined behavior. Normal access may resume only after
 * the enclosing ArrowArrayStream `get_next`, `lance_scanner_next`, or
 * `lance_scanner_poll_next` call returns to its caller.
 *
 * Scan statistics are diagnostic and best-effort. The callback must handle its
 * own errors and must not use them to abort or throw across this FFI boundary.
 */
typedef void (*LanceScanStatisticsCallback)(
    void* callback_ctx,
    const LanceScanStatistics* statistics
);

/**
 * Register the execution-statistics callback for this scanner.
 *
 * Must be called before starting the scan; registering after the scan starts
 * returns an error. `callback` must not be NULL. `callback_ctx` may be NULL. A
 * non-NULL `callback_ctx` must remain valid, and `callback` must remain valid,
 * until all of the following are true: the scanner is closed, every in-flight
 * `lance_scanner_scan_async` call has delivered its completion callback, and
 * every ArrowArrayStream derived from the scanner has been released. The
 * registration remains installed after a callback returns and applies to
 * streams created later from the same scanner. For `lance_scanner_next` and
 * `lance_scanner_poll_next`, the scanner owns the stream. Exported and
 * asynchronous ArrowArrayStreams own their registrations independently of the
 * scanner and may invoke the callback after the scanner is closed. Concurrent
 * streams may invoke the callback concurrently.
 *
 * The callback is invoked exactly once for each derived stream that is fully
 * consumed to EOF. It is not guaranteed to run for a stream if execution fails,
 * the scan is cancelled, or the scanner / ArrowArrayStream is released before
 * EOF. Before scanning starts, a new registration replaces the previous one;
 * after a successful replacement, the previous callback and context are no
 * longer retained and may be retired.
 *
 * From callback entry until the enclosing EOF-observing operation returns, the
 * callback must not directly or indirectly cause interaction with any
 * ArrowArrayStream derived from this scanner; see LanceScanStatisticsCallback
 * for the complete reentrancy restriction.
 *
 * @return 0 on success, -1 on error
 */
int32_t lance_scanner_set_statistics_callback(
    LanceScanner* scanner,
    LanceScanStatisticsCallback callback,
    void* callback_ctx
);

/**
 * Close and free a scanner handle. Safe to call with NULL; a non-NULL handle
 * must be closed exactly once.
 *
 * This is the retirement boundary for poll wakers registered by
 * lance_scanner_poll_next(): it cancels callbacks that have not entered and
 * waits for any callback already in progress to return before freeing the
 * scanner. Do not call this function from one of the scanner's own waker
 * callbacks, because close must wait for that callback to return.
 */
void lance_scanner_close(LanceScanner* scanner);

/* ─── Scanner planning, introspection and count ─── */

/**
 * Export the configured scanner output schema.
 *
 * Schemas come from the native plan for the actual configured execution path,
 * including projections, scoring columns, multi-vector and prepared FTS
 * transformations, and scalar-segment setup. A binding-private multi-vector
 * distance column is removed before export.
 *
 * This is planning-only: it does not start iteration or advance any stream
 * already owned by or exported from this scanner. Configuration setters remain
 * usable afterward. Read-only index metadata and scalar-segment candidate work
 * required to construct the configured plan may still perform I/O.
 *
 * `out` must be non-NULL and point to an empty ArrowSchema whose `release`
 * callback is NULL. On success the caller must invoke `out->release(out)`
 * exactly once.
 *
 * @return 0 on success, -1 on error
 */
int32_t lance_scanner_schema(
    const LanceScanner* scanner,
    struct ArrowSchema* out
);

/**
 * Explain the configured scanner plan without executing it.
 *
 * `verbose` selects native DataFusion tree formatting. The plan includes the
 * same transformations used by a stream from this configuration.
 *
 * This does not start iteration or advance any stream already owned by or
 * exported from this scanner, and the scanner remains configurable afterward.
 * Read-only index metadata and scalar-segment candidate work required to
 * construct the configured plan may still perform I/O.
 *
 * The returned string is owned by the caller and must be freed with
 * lance_free_string(). Returns NULL on error.
 */
const char* lance_scanner_explain_plan(
    const LanceScanner* scanner,
    bool verbose
);

/**
 * Execute the configured scanner plan and return its native runtime analysis.
 *
 * This executes the same plan a stream from this configuration would use,
 * including scalar-segment, multi-vector, and prepared FTS transformations.
 * It does not consume or advance any existing stream. The scanner remains
 * configurable afterward.
 *
 * The returned string is owned by the caller and must be freed with
 * lance_free_string(). Returns NULL on error.
 */
const char* lance_scanner_analyze_plan(const LanceScanner* scanner);

/**
 * Count rows matched by the configured scanner.
 *
 * This applies the native `COUNT(*)` aggregate to the configured scanner plan
 * and executes that count plan. It does not count batches from a stream.
 * Native aggregate planning rejects any configured limit, offset, or ordering.
 * MinHash search requires a limit, so it cannot be combined with this count
 * operation; use a limited MinHash scan and count the returned rows instead.
 * Count execution uses the native count operation's execution options; the
 * registered scan-statistics callback is not invoked for this separate count
 * operation.
 *
 * This does not start normal scanner iteration or advance an existing stream,
 * and the scanner remains configurable afterward.
 *
 * Returns 0 on error; an empty match also returns 0, so check
 * lance_last_error_code().
 */
uint64_t lance_scanner_count_rows(const LanceScanner* scanner);

/**
 * Execute and format the configured native `COUNT(*)` plan.
 *
 * This analyzes the plan used by `lance_scanner_count_rows`, not the ordinary
 * scan plan. It obeys the same aggregate planning constraints and does not
 * invoke the registered scan-statistics callback. The scanner remains
 * configurable afterward.
 *
 * The returned string is owned by the caller and must be freed with
 * lance_free_string(). Returns NULL on error.
 */
const char* lance_scanner_analyze_count_plan(const LanceScanner* scanner);

/* ─── Sync scan: ArrowArrayStream ─── */

/**
 * Materialize the scan as an ArrowArrayStream (blocking).
 * The scanner remains valid, and each call creates an independent stream.
 * `out` points to caller-owned storage. On success, the caller must eventually
 * invoke `out->release(out)` exactly once when `release` is non-NULL; that
 * releases the stream contents but not the caller-owned outer structure. Do
 * not pass this caller-allocated stream to lance_scanner_async_stream_free().
 *
 * Reading the exported stream may surface a mid-iteration panic as one
 * error through the Arrow C stream contract (nonzero get_next plus
 * get_last_error), followed by end-of-stream; the scanner handle is
 * poisoned afterwards.
 *
 * @return 0 on success, -1 on error
 */
int32_t lance_scanner_to_arrow_stream(
    LanceScanner* scanner,
    struct ArrowArrayStream* out
);

/* ─── Sync scan: batch iteration ─── */

/**
 * Read the next batch (blocking).
 * @param out  Set to a LanceBatch* on success, NULL on end/error
 * @return 0 = batch available, 1 = end of stream, -1 = error
 */
int32_t lance_scanner_next(
    LanceScanner* scanner,
    LanceBatch** out
);

/* ─── Async scan: callback-based ─── */

/**
 * Callback type for async operations.
 *
 * The callback normally runs on the dedicated dispatcher thread. During a
 * rare dispatcher startup or delivery failure, completion falls back to the
 * thread that detects the failure (for example the calling or producing
 * thread), so the callback must be thread-safe. On failure the error code and
 * message are installed on the actual callback thread immediately before the
 * callback runs, so lance_last_error_* called from inside the callback
 * observes this completion's failure.
 *
 * Callbacks must return normally: the callback ABI is non-unwinding, so a
 * callback that throws or unwinds can abort the host process before the
 * dispatcher can contain it.
 * A callback passed to lance_scanner_scan_async() must not be NULL.
 *
 * @param ctx     Opaque pointer passed back from the caller
 * @param status  0 = success, -1 = error
 * @param result  Operation-specific result (e.g., ArrowArrayStream*)
 */
typedef void (*LanceCallback)(void* ctx, int32_t status, void* result);

/**
 * Start an async scan. The callback normally fires on a dedicated dispatcher
 * thread when the ArrowArrayStream is ready. During a rare dispatcher
 * infrastructure failure it may instead run on the calling or producing
 * thread, so it must be thread-safe.
 *
 * For a non-NULL callback, exactly one completion is delivered, including for
 * validation, setup, task, and dispatcher failures. The fallback path may
 * invoke it before lance_scanner_scan_async() returns. `callback` and a
 * non-NULL `callback_ctx` must remain valid until that invocation returns.
 *
 * `callback` must not be NULL; `callback_ctx` may be NULL. On success, result
 * is a library-allocated ArrowArrayStream owned by the caller. The caller must
 * eventually pass it exactly once to lance_scanner_async_stream_free(), even
 * if it has already invoked the stream's release callback directly. Do not
 * free the returned outer structure with free(), delete, or a platform
 * allocator.
 *
 * On failure the callback receives status -1 with result NULL, and the
 * error code/message are installed in the actual callback thread's
 * thread-local storage immediately before the callback runs (per completion).
 * A panic in the scan task also yields status -1 with LANCE_ERR_PANIC and
 * poisons the scanner handle.
 */
void lance_scanner_scan_async(
    const LanceScanner* scanner,
    LanceCallback callback,
    void* callback_ctx
);

/**
 * Release and free an ArrowArrayStream returned by a successful
 * lance_scanner_scan_async() callback.
 *
 * If `stream->release` is non-NULL, this function invokes it before freeing
 * the library-allocated outer structure. It is therefore valid both before
 * and after a consumer has directly released the stream contents. `stream`
 * may be NULL. A non-NULL pointer must be passed exactly once and must be the
 * pointer delivered by lance_scanner_scan_async(); using this function for a
 * caller-allocated ArrowArrayStream is invalid.
 */
void lance_scanner_async_stream_free(struct ArrowArrayStream* stream);

/* ─── Poll-based scan (for cooperative async runtimes) ─── */

typedef enum {
    LANCE_POLL_READY    =  0,
    LANCE_POLL_PENDING  =  1,
    LANCE_POLL_FINISHED =  2,
    LANCE_POLL_ERROR    = -1,
} LancePollStatus;

/**
 * Waker callback: called from a Tokio thread when data is ready. A waker
 * passed to lance_scanner_poll_next() must not be NULL. For one poll call
 * that returns LANCE_POLL_PENDING, all internal RawWaker clones share a
 * one-shot gate, so the callback fires at most once.
 *
 * The callback and `ctx` must be thread-safe and must remain valid until the
 * callback returns or lance_scanner_close() returns. Close cancels a pending
 * callback and waits for an active callback before returning, so the caller
 * may destroy `ctx` afterwards. The callback must return normally and must
 * not call lance_scanner_close() or otherwise re-enter its originating
 * scanner.
 */
typedef void (*LanceWaker)(void* ctx);

/**
 * Poll for the next batch without blocking.
 * `waker` must not be NULL; `waker_ctx` may be NULL. `out` is set to a
 * LanceBatch only for LANCE_POLL_READY and is set to NULL for
 * LANCE_POLL_PENDING, LANCE_POLL_FINISHED, and LANCE_POLL_ERROR.
 */
LancePollStatus lance_scanner_poll_next(
    LanceScanner* scanner,
    LanceWaker waker,
    void* waker_ctx,
    LanceBatch** out
);

/* ─── Batch (Arrow C Data Interface) ─── */

/**
 * Export a batch as Arrow C Data Interface structs.
 * @return 0 on success, -1 on error
 */
int32_t lance_batch_to_arrow(
    const LanceBatch* batch,
    struct ArrowArray* out_array,
    struct ArrowSchema* out_schema
);

/**
 * Import one Arrow record batch.
 *
 * A non-NULL `array` is consumed on every return, including validation and
 * conversion failures. The source slot is left in its canonical released
 * state. `schema` is borrowed and is never released or modified. `out` is
 * written only on success and is unchanged on failure.
 *
 * The complete schema is retained, including schema metadata, field metadata,
 * and extension metadata. A zero-column batch retains its row count.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_batch_from_arrow(
    struct ArrowArray* array,
    const struct ArrowSchema* schema,
    LanceBatch** out
);

/** Free a batch handle. */
void lance_batch_free(LanceBatch* batch);

/* ─── Fragment writer ─── */

/**
 * Write an Arrow record batch stream to fragment files at `uri`.
 *
 * Designed for embedded / robotics C++ pipelines: write Lance fragment files
 * locally with minimal overhead. A separate Rust finalizer process later
 * reconstructs Fragment metadata from the file footers and commits them
 * into a dataset on a remote data lake via CommitBuilder.
 *
 * The data is written but NOT committed — no dataset manifest is created or
 * updated. The written .lance files under <uri>/data/ contain full metadata
 * in their footers (schema with field IDs, row counts, format version).
 *
 * @param uri          Directory URI for fragment files (file://, s3://, etc.)
 * @param schema       Required Arrow schema. The stream schema must match
 *                     or the call fails with LANCE_ERR_INVALID_ARGUMENT.
 * @param stream       Arrow C Data Interface stream; consumed by this call —
 *                     do not use the stream after returning.
 * @param storage_opts NULL-terminated key-value pairs ["k","v",NULL], or NULL.
 * @return 0 on success, -1 on error
 */
int32_t lance_write_fragments(
    const char* uri,
    const struct ArrowSchema* schema,
    struct ArrowArrayStream* stream,
    const char* const* storage_opts
);

/* ─── Index lifecycle (Phase 2) ─── */

/**
 * Create a vector index on a column.
 * @param dataset    Open dataset (mutated; same handle remains valid).
 * @param column     Column name (must be FixedSizeList<float32|float16|uint8|int8>).
 * @param index_name Optional index name; NULL → "<column>_idx".
 * @param params     Vector index params; index_type field selects the variant.
 * @param replace    If true, replace any existing index of the same name.
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_create_vector_index(
    LanceDataset* dataset,
    const char* column,
    const char* index_name,
    const LanceVectorIndexParams* params,
    bool replace
);

/**
 * Create a vector index from additive native options.
 *
 * The native builder commits the index and then reloads committed metadata.
 * An error can therefore be returned after the dataset version has already
 * been published. Reopen the dataset to determine its current version; the
 * binding does not claim storage rollback.
 *
 * `options` is borrowed for the call and must not be NULL.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_create_vector_index_with_options(
    LanceDataset* dataset,
    const char* column,
    const char* index_name,
    const LanceVectorIndexOptions* options,
    bool replace
);

/**
 * Create a scalar index on a column.
 * @param params_json Optional JSON params forwarded unchanged to the native
 *                    Lance scalar-index plugin, or NULL for plugin defaults.
 *                    Plugin field names and validation apply; for example
 *                    ZoneMap uses `rows_per_zone`/`use_seeds` and BloomFilter
 *                    uses `number_of_items`/`probability`. MinHash LSH uses
 *                    `num_hashes`, `num_bands`, `shingle_size`, and
 *                    `tokenizer`.
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_create_scalar_index(
    LanceDataset* dataset,
    const char* column,
    const char* index_name,
    LanceScalarIndexType index_type,
    const char* params_json,
    bool replace
);

/**
 * Create a scalar index with the additive native train control.
 *
 * `options == NULL` or `options->has_train == false` preserves the native
 * default. `params_json` remains the native plugin parameter object.
 */
int32_t lance_dataset_create_scalar_index_with_options(
    LanceDataset* dataset,
    const char* column,
    const char* index_name,
    LanceScalarIndexType index_type,
    const char* params_json,
    const LanceScalarIndexOptions* options,
    bool replace
);

/* ─── Uncommitted index segment build ─── */

/**
 * Options for an uncommitted index segment build.
 *
 * `fragment_ids == NULL && fragment_count == 0` preserves native None and
 * selects the whole dataset. A non-NULL pointer with `fragment_count == 0`
 * preserves native Some(empty). Any non-empty list is copied when the builder
 * is created. Duplicate and missing-ID handling follows the native
 * CreateIndexBuilder at build time.
 * `index_uuid`, when non-NULL, points to exactly 16 RFC 4122 bytes and is also
 * copied during builder creation. Lance does not support an assigned UUID for
 * fragment-scoped BTree builds; leave it NULL for that combination.
 *
 * The Arrow array/schema pairs provide vector model injection. Both pointers
 * in a pair must be NULL or both must be non-NULL. Vector builder creation
 * borrows model pairs synchronously. It may replace each ArrowArray struct but
 * leaves a live caller-owned equivalent in place, so one trained model can be
 * reused by multiple segment builders. Schemas remain caller-owned. Scalar
 * builders reject non-NULL model pairs. Inputs must be valid Arrow C Data
 * Interface trees; malformed arrays may be moved/released while reporting an
 * error. Models produced by the trainer functions carry provenance metadata;
 * vector builders reject mismatched metric/dimension or PQ/IVF model identity.
 *
 * `mode` is zero-defaulted: AUTO uses a supplied model set, or trains locally
 * when no model set is supplied. For IVF-PQ and IVF-HNSW-PQ, IVF centroids and
 * the PQ codebook are one model set: callers must supply both or neither;
 * partial model training is not supported. LOCAL_TRAIN rejects supplied
 * models; PRECOMPUTED requires the complete model set needed by the selected
 * vector index. Scalar builders accept AUTO and LOCAL_TRAIN. Passing NULL for
 * the entire options pointer uses AUTO.
 *
 * Temporary limitation: with the DOT metric, IVF-PQ and IVF-HNSW-PQ reject a
 * supplied model set when the fragment selection is an effective strict
 * subset of the dataset, in both AUTO and PRECOMPUTED modes. Cover every
 * fragment in one segment (or pass NULL fragment_ids) until the upstream
 * Lance distributed builder reconstructs supplied codebooks with an L2
 * ProductQuantizer. L2 and Cosine model sets are unaffected.
 */
typedef enum LanceIndexSegmentBuildMode {
    LANCE_INDEX_SEGMENT_BUILD_AUTO = 0,
    LANCE_INDEX_SEGMENT_BUILD_LOCAL_TRAIN = 1,
    LANCE_INDEX_SEGMENT_BUILD_PRECOMPUTED = 2
} LanceIndexSegmentBuildMode;

typedef struct LanceIndexSegmentBuildOptions {
    const uint32_t*          fragment_ids;
    size_t                   fragment_count;
    const uint8_t*           index_uuid;
    struct ArrowArray*       ivf_centroids;
    const struct ArrowSchema* ivf_centroids_schema;
    struct ArrowArray*       pq_codebook;
    const struct ArrowSchema* pq_codebook_schema;
    int32_t                  mode;
} LanceIndexSegmentBuildOptions;

/**
 * Vector parameters with fixed-width, boundary-validated discriminants.
 * num_partitions is required for every variant; num_sub_vectors is required
 * for PQ variants; hnsw_m is required for HNSW variants. num_bits is 0 for
 * the Lance default (8). PQ accepts 4 or 8; SQ accepts only 8.
 */
typedef struct LanceVectorIndexSegmentParams {
    int32_t  index_type;
    int32_t  metric;
    uint32_t num_partitions;
    uint32_t num_sub_vectors;
    uint32_t num_bits;
    uint32_t max_iterations;
    uint32_t hnsw_m;
    uint32_t hnsw_ef_construction;
    uint32_t sample_rate;
} LanceVectorIndexSegmentParams;

/**
 * Fragment selection and optional segment UUID for the additive vector builder.
 *
 * A NULL scope preserves both native selections as None. Within a non-NULL
 * scope, a NULL `fragment_ids` with count 0 is None, while a non-NULL pointer
 * with count 0 is Some(empty). IDs and a non-NULL UUID are copied at builder
 * creation; native Lance code decides duplicate/missing validation and timing.
 * `index_uuid`, when non-NULL, points to exactly 16 RFC 4122 bytes.
 */
typedef struct LanceVectorIndexSegmentScope {
    const uint32_t* fragment_ids;
    size_t fragment_count;
    const uint8_t* index_uuid;
} LanceVectorIndexSegmentScope;

/**
 * Create a single-use scalar index segment builder bound to the dataset's
 * current snapshot. The dataset handle is not consumed and need not outlive
 * the returned builder.
 *
 * @param index_name Optional index name; NULL selects the Lance default.
 * @param index_type LanceScalarIndexType discriminant, passed as int32_t so
 *                   out-of-range values can be rejected safely.
 * @param params_json Optional scalar-index JSON parameters, or NULL.
 * @param options Optional build options, or NULL for defaults.
 * @return Builder handle on success, or NULL on error.
 */
LanceIndexSegmentBuilder* lance_index_segment_builder_new_scalar(
    const LanceDataset* dataset,
    const char* column,
    const char* index_name,
    int32_t index_type,
    const char* params_json,
    const LanceIndexSegmentBuildOptions* options
);

/**
 * Create a scalar segment builder with the additive native train control.
 * `scalar_options == NULL` or an unset `has_train` preserves native defaults.
 */
LanceIndexSegmentBuilder* lance_index_segment_builder_new_scalar_with_options(
    const LanceDataset* dataset,
    const char* column,
    const char* index_name,
    int32_t index_type,
    const char* params_json,
    const LanceScalarIndexOptions* scalar_options,
    const LanceIndexSegmentBuildOptions* options
);

/**
 * Create a single-use vector index segment builder. Model arrays and schemas
 * are borrowed synchronously; arrays may be replaced but remain live and
 * caller-owned. When no model set is supplied, AUTO trains locally. PQ
 * variants require both the IVF centroids and PQ codebook, or neither.
 */
LanceIndexSegmentBuilder* lance_index_segment_builder_new_vector(
    const LanceDataset* dataset,
    const char* column,
    const char* index_name,
    const LanceVectorIndexSegmentParams* params,
    const LanceIndexSegmentBuildOptions* options
);

/**
 * Create a single-use vector segment builder from additive native options.
 * The builder owns a dataset snapshot. Strings, maps, paths, fragment IDs and
 * UUID are copied; Arrow models are borrowed synchronously as described by
 * LanceVectorIndexOptions. The dataset handle need not outlive the builder.
 *
 * `scope` may be NULL for whole-dataset/no-UUID semantics. `train` follows
 * the native builder default unless `options->has_train` is true.
 * Training with supplied PQ codebooks and DOT requires full dataset coverage
 * on the selected core. A strict subset returns LANCE_ERR_INVALID_ARGUMENT,
 * preserving the legacy protection against incompatible PQ assignment.
 * Definition-only builds with `train` set to false accept fragment subsets.
 */
LanceIndexSegmentBuilder* lance_index_segment_builder_new_vector_with_options(
    const LanceDataset* dataset,
    const char* column,
    const char* index_name,
    const LanceVectorIndexOptions* options,
    const LanceVectorIndexSegmentScope* scope
);

/**
 * Train IVF centroids, exporting FixedSizeList<Float32>[vector_dimension].
 * The output structs must be zero-initialized; the caller owns and releases
 * both Arrow C Data Interface outputs after success.
 */
int32_t lance_index_train_ivf_model(
    const LanceDataset* dataset,
    const char* column,
    uint32_t num_partitions,
    int32_t metric,
    const uint32_t* fragment_ids,
    size_t fragment_count,
    struct ArrowArray* out_array,
    struct ArrowSchema* out_schema
);

/**
 * Train IVF centroids with native sample-rate and iteration controls.
 * `options == NULL` is equivalent to both fields absent. A present zero is
 * forwarded to Lance; this binding does not add a zero-value policy.
 */
int32_t lance_index_train_ivf_model_with_options(
    const LanceDataset* dataset,
    const char* column,
    uint32_t num_partitions,
    int32_t metric,
    const uint32_t* fragment_ids,
    size_t fragment_count,
    const LanceIndexTrainerOptions* options,
    struct ArrowArray* out_array,
    struct ArrowSchema* out_schema
);

/**
 * Train a PQ codebook, exporting
 * FixedSizeList<Float32>[dimension / num_sub_vectors] with
 * num_sub_vectors * 2^num_bits rows. `num_bits` must be 4 or 8. The IVF
 * centroids must be the shared centroids that will be injected with this PQ
 * codebook. L2 and cosine training use them to compute residuals; DOT training
 * keeps them for model identity but trains on raw vectors. The trainer borrows
 * them synchronously: it may replace the ArrowArray struct, but leaves a live
 * caller-owned equivalent in place.
 */
int32_t lance_index_train_pq_model(
    const LanceDataset* dataset,
    const char* column,
    uint32_t num_sub_vectors,
    uint32_t num_bits,
    int32_t metric,
    const uint32_t* fragment_ids,
    size_t fragment_count,
    struct ArrowArray* ivf_centroids,
    const struct ArrowSchema* ivf_centroids_schema,
    struct ArrowArray* out_array,
    struct ArrowSchema* out_schema
);

/**
 * Train a PQ codebook with native sample-rate and iteration controls.
 * The IVF centroids and Arrow output otherwise follow
 * lance_index_train_pq_model.
 */
int32_t lance_index_train_pq_model_with_options(
    const LanceDataset* dataset,
    const char* column,
    uint32_t num_sub_vectors,
    uint32_t num_bits,
    int32_t metric,
    const uint32_t* fragment_ids,
    size_t fragment_count,
    struct ArrowArray* ivf_centroids,
    const struct ArrowSchema* ivf_centroids_schema,
    const LanceIndexTrainerOptions* options,
    struct ArrowArray* out_array,
    struct ArrowSchema* out_schema
);

/**
 * Open the first physical vector segment named `index_name` and return an
 * owned clone of its IVF model. The handle does not borrow the dataset.
 *
 * Returns NULL on error. `index_name` must be non-NULL and non-empty.
 */
LanceIvfModel* lance_dataset_get_ivf_model(
    const LanceDataset* dataset,
    const char* index_name
);

/** Free an IVF model. NULL-safe. */
void lance_ivf_model_free(LanceIvfModel* model);

/** Whether the model contains centroids. NULL sets INVALID_ARGUMENT. */
bool lance_ivf_model_has_centroids(const LanceIvfModel* model);

/**
 * Export an owned Arrow copy of the centroids. Both output structs must be
 * zero-initialized and are unchanged on error. A model without centroids
 * returns LANCE_ERR_NOT_FOUND.
 */
int32_t lance_ivf_model_export_centroids(
    const LanceIvfModel* model,
    struct ArrowArray* out_array,
    struct ArrowSchema* out_schema
);

/**
 * Native partition count, dimension, and per-partition row count.
 *
 * These return 0 on an error sentinel; check lance_last_error_code(). An
 * absent partition also returns 0 from lance_ivf_model_partition_size(),
 * preserving IvfModel::partition_size for every representable usize.
 */
uint64_t lance_ivf_model_num_partitions(const LanceIvfModel* model);
uint64_t lance_ivf_model_dimension(const LanceIvfModel* model);
uint64_t lance_ivf_model_partition_size(
    const LanceIvfModel* model,
    uint64_t partition_id
);

/**
 * Read one logical IVF partition across every physical segment.
 *
 * The returned ArrowArrayStream owns its native snapshot and remains valid
 * after the source dataset is closed. The caller releases it once with its
 * Arrow release callback.
 */
int32_t lance_dataset_read_index_partition(
    const LanceDataset* dataset,
    const char* index_name,
    uint64_t partition_id,
    bool with_vector,
    struct ArrowArrayStream* out_stream
);

/**
 * Build a portable IVF_RQ fast-rotation model as native
 * RabitQuantizationMetadata JSON.
 *
 * `dimension` must be positive and divisible by 8. `num_bits == 0` selects the
 * native RQ default of 5. The returned JSON can be shared across fragment
 * builds through LanceVectorIndexOptions.rabitq_model_json. Matrix rotations
 * are not emitted because their dense matrix is skipped by the native JSON
 * representation.
 *
 * Returns a library-owned NUL-terminated string on success, or NULL on error.
 * Free a successful result with lance_free_string().
 */
const char* lance_index_build_rq_model(uint32_t dimension, uint32_t num_bits);

/**
 * Build segment artifacts without committing them to the dataset manifest.
 * The builder is single-use, including when execution fails.
 *
 * On success, `*out_bytes` receives protobuf-encoded IndexMetadata and
 * `*out_len` receives its byte length. Free the buffer with
 * lance_free_bytes(). On error, the output slots are left unchanged.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_index_segment_builder_execute_uncommitted(
    LanceIndexSegmentBuilder* builder,
    uint8_t** out_bytes,
    size_t* out_len
);

/**
 * Event codes for LanceIndexBuildProgressCallback, passed as the `event`
 * argument. Exactly one stage is active at a time: a stage's
 * LANCE_INDEX_BUILD_PROGRESS_STAGE_COMPLETE is always delivered before the
 * next stage's LANCE_INDEX_BUILD_PROGRESS_STAGE_START.
 */
typedef enum {
    LANCE_INDEX_BUILD_PROGRESS_STAGE_START = 0,
    LANCE_INDEX_BUILD_PROGRESS_STAGE_PROGRESS = 1,
    LANCE_INDEX_BUILD_PROGRESS_STAGE_COMPLETE = 2,
} LanceIndexBuildProgressEvent;

/**
 * Receives index build progress events while
 * lance_index_segment_builder_execute_uncommitted runs.
 *
 * `stage` is non-NULL, NUL-terminated, and borrowed: it is valid only for the
 * duration of this call. `unit` is non-NULL and NUL-terminated, but is the
 * empty string ("") for LANCE_INDEX_BUILD_PROGRESS_STAGE_PROGRESS and
 * LANCE_INDEX_BUILD_PROGRESS_STAGE_COMPLETE. The parameter mapping is:
 *
 *  - LANCE_INDEX_BUILD_PROGRESS_STAGE_START: `stage` is the stage name,
 *    `total` is the number of work units (0 = unknown), `unit` describes what
 *    is being counted (e.g. "partitions", "batches", "rows"; "" = unknown),
 *    and `completed` is 0.
 *  - LANCE_INDEX_BUILD_PROGRESS_STAGE_PROGRESS: `total` is 0, `unit` is "",
 *    and `completed` is the number of units completed so far.
 *  - LANCE_INDEX_BUILD_PROGRESS_STAGE_COMPLETE: `total` is 0, `unit` is "",
 *    and `completed` is 0.
 *
 * Stage names are index-type-specific (e.g. "train_ivf", "shuffle",
 * "merge_partitions" for vector indices; "load_data" for scalar indices) and
 * are diagnostic-only: they are not a stable cross-version contract, so
 * consumers must treat them as opaque strings.
 *
 * The callback is invoked from lance-c's internal tokio runtime worker
 * threads. Certain stages report progress concurrently from parallel worker
 * tasks, so the callback MUST be thread-safe and reentrant. It must be
 * non-blocking and must not call back into any `lance_*` function (no
 * reentrancy).
 *
 * The callback is invoked without a panic guard: it must return normally,
 * because unwinding or throwing across this boundary can abort the host
 * process. The callback cannot abort the build; progress reporting is
 * advisory and diagnostic and cannot affect the build outcome.
 */
typedef void (*LanceIndexBuildProgressCallback)(
    void* callback_ctx,
    int32_t event,
    const char* stage,
    uint64_t total,
    const char* unit,
    uint64_t completed
);

/**
 * Register the index-build progress callback for a segment builder.
 *
 * Must be called before the builder is executed; the builder is single-use,
 * so calling it after lance_index_segment_builder_execute_uncommitted has
 * been called (even if that call failed) returns -1. `callback` must not be
 * NULL. `callback_ctx` may be NULL and is passed through to the callback
 * opaquely. Setting a callback replaces any previously set callback.
 *
 * Invocations occur only while lance_index_segment_builder_execute_uncommitted
 * is executing, and this is enforced rather than contractual: lance-c
 * disables the callback and drains in-flight invocations through a retirement
 * gate before that call returns, including on error, so a worker task
 * detached by lance core on an error path can never invoke the callback
 * afterwards. `callback` and `callback_ctx` must therefore remain valid and
 * safe to invoke until lance_index_segment_builder_execute_uncommitted
 * returns. See LanceIndexBuildProgressCallback for the full threading and
 * reentrancy contract.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_index_segment_builder_set_progress_callback(
    LanceIndexSegmentBuilder* builder,
    LanceIndexBuildProgressCallback callback,
    void* callback_ctx
);

/**
 * Merge distributed Inverted index files for `index_uuid` into one root
 * artifact. This call writes/merges files only: it does not commit a dataset
 * version or return metadata. On error, partial files may remain.
 *
 * `index_uuid` points to 16 RFC 4122 bytes. `index_type` is parsed by native
 * Lance; at this pin only "Inverted" is supported by this merge path. Other
 * native index types return their native errors. `has_batch_readhead` absent
 * maps to None; true forwards Some(batch_readhead), including zero. The
 * selected core currently ignores this parameter.
 *
 * A non-NULL callback follows LanceIndexBuildProgressCallback's threading and
 * lifetime contract. Callback retirement is enforced before this blocking
 * call returns.
 *
 * Complete publication requires a separate CreateIndex operation.
 */
int32_t lance_dataset_merge_index_metadata(
    const LanceDataset* dataset,
    const uint8_t index_uuid[16],
    const char* index_type,
    bool has_batch_readhead,
    size_t batch_readhead,
    LanceIndexBuildProgressCallback callback,
    void* callback_ctx
);

/** Free metadata bytes returned by an uncommitted segment build. NULL-safe. */
void lance_free_bytes(uint8_t* bytes);

/** Free an index segment builder. NULL-safe. */
void lance_index_segment_builder_free(LanceIndexSegmentBuilder* builder);

/**
 * Parse protobuf-encoded IndexMetadata into an opaque metadata handle.
 * `bytes` is borrowed only for this call. On error, `*out_metadata` is left
 * unchanged.
 */
int32_t lance_index_segment_metadata_parse(
    const uint8_t* bytes,
    size_t len,
    LanceIndexSegmentMetadata** out_metadata
);

/** Copy the segment UUID into a caller-provided 16-byte buffer. */
int32_t lance_index_segment_metadata_uuid(
    const LanceIndexSegmentMetadata* metadata,
    uint8_t* out_uuid
);

/**
 * Return the segment name. The string is borrowed from `metadata` and remains
 * valid until lance_index_segment_metadata_free(); do not free it separately.
 */
const char* lance_index_segment_metadata_name(
    const LanceIndexSegmentMetadata* metadata
);

/**
 * Return the dataset version against which the segment was built, or 0 on
 * error (check lance_last_error_code()).
 */
uint64_t lance_index_segment_metadata_dataset_version(
    const LanceIndexSegmentMetadata* metadata
);

/** Return the physical index version, or -1 on error. */
int32_t lance_index_segment_metadata_index_version(
    const LanceIndexSegmentMetadata* metadata
);

/**
 * Return the LanceScalarIndexType/LanceVectorIndexType discriminant, or -1 on
 * error. Both enum domains use the stable, non-overlapping values above.
 */
int32_t lance_index_segment_metadata_index_type(
    const LanceIndexSegmentMetadata* metadata
);

/**
 * Return the protobuf Any type URL, borrowed until metadata is freed.
 * Returns NULL if metadata has no index_details.
 */
const char* lance_index_segment_metadata_index_details_type_url(
    const LanceIndexSegmentMetadata* metadata
);

/**
 * Return the number of indexed field IDs. Returns 0 on error; zero may also be
 * a valid count, so check lance_last_error_code().
 */
size_t lance_index_segment_metadata_field_count(
    const LanceIndexSegmentMetadata* metadata
);

/** Copy indexed field IDs in metadata order. */
int32_t lance_index_segment_metadata_field_ids(
    const LanceIndexSegmentMetadata* metadata,
    int32_t* out_field_ids,
    size_t capacity,
    size_t* out_count
);

/**
 * Return the number of fragment IDs covered by the segment. Returns 0 on
 * error; zero may also be a valid count, so check lance_last_error_code().
 */
size_t lance_index_segment_metadata_fragment_count(
    const LanceIndexSegmentMetadata* metadata
);

/**
 * Copy covered fragment IDs in ascending order.
 *
 * `capacity` is measured in uint32_t elements. `out_count` is required and
 * receives the number written. `out_fragment_ids` may be NULL only when the
 * metadata covers zero fragments.
 */
int32_t lance_index_segment_metadata_fragment_ids(
    const LanceIndexSegmentMetadata* metadata,
    uint32_t* out_fragment_ids,
    size_t capacity,
    size_t* out_count
);

/**
 * Clone parsed index metadata into an independent handle.
 *
 * The clone copies the complete in-memory native value, including optional
 * index details, fragment bitmap, created_at, base_id and files. It does not
 * re-encode through protobuf, so an in-memory Some(empty) files vector remains
 * distinct from None even though the protobuf decoder normalizes it.
 * Returns NULL on error.
 */
LanceIndexSegmentMetadata* lance_index_segment_metadata_clone(
    const LanceIndexSegmentMetadata* metadata
);

/**
 * Clone parsed index metadata with caller-supplied fragment coverage.
 *
 * When `has_fragment_ids` is false, the native `fragment_bitmap` option is
 * copied unchanged and `fragment_ids` / `fragment_count` are ignored. When
 * true, the clone's coverage becomes some of the caller's IDs: `fragment_count
 * == 0` maps to present-empty coverage, and a positive count requires
 * `fragment_ids` to point at that many `uint32_t` values. IDs are copied into
 * the native bitmap, which sorts them and treats them as a set.
 *
 * Every other native field is copied exactly, including name, field and
 * covering declarations, dataset/index versions, index details, created_at,
 * base_id, and files, together with their in-memory option presence. This call
 * performs no coverage, overlap, staleness, index-family, or file validation;
 * those rules remain with the native commit path. The source handle is
 * unchanged and remains usable.
 *
 * Returns an independently owned handle, or NULL on error.
 */
LanceIndexSegmentMetadata* lance_index_segment_metadata_clone_with_fragment_ids(
    const LanceIndexSegmentMetadata* metadata,
    bool has_fragment_ids,
    const uint32_t* fragment_ids,
    size_t fragment_count
);

/**
 * Encode parsed index metadata as protobuf-encoded Lance `IndexMetadata`.
 *
 * This explicitly serializes the complete in-memory native value. On success,
 * `*out_bytes` receives a library-owned buffer of `*out_len` bytes that the
 * caller frees with lance_free_bytes(). Both output slots are success-only and
 * remain unchanged on error. Protobuf cannot distinguish some in-memory
 * optionals, so encoding may normalize them (for example present-empty files
 * decode as absent); cloning does not perform that normalization.
 *
 * Returns 0 on success, -1 on error.
 */
int32_t lance_index_segment_metadata_encode(
    const LanceIndexSegmentMetadata* metadata,
    uint8_t** out_bytes,
    size_t* out_len
);

/**
 * Return the number of covering field IDs. Returns 0 on error; zero is also a
 * valid count, so check lance_last_error_code().
 */
size_t lance_index_segment_metadata_covering_field_count(
    const LanceIndexSegmentMetadata* metadata
);

/**
 * Copy one covering field ID in metadata order.
 * Returns 0 on success, -1 on error; `*out_field_id` is unchanged on error.
 */
int32_t lance_index_segment_metadata_covering_field_id_at(
    const LanceIndexSegmentMetadata* metadata,
    size_t index,
    int32_t* out_field_id
);

/**
 * Return the borrowed native `index_details.value` bytes.
 *
 * The pointer remains valid until the metadata handle is freed. A present but
 * empty value returns a non-NULL pointer and writes `*out_len = 0`. Absent
 * index details return NULL with LANCE_ERR_NOT_FOUND. `*out_len` is unchanged
 * on error.
 */
const uint8_t* lance_index_segment_metadata_index_details_value(
    const LanceIndexSegmentMetadata* metadata,
    size_t* out_len
);

/**
 * Return the number of files recorded in the metadata. Returns 0 on error or
 * when the native `files` option is absent; check lance_last_error_code() to
 * distinguish.
 */
size_t lance_index_segment_metadata_file_count(
    const LanceIndexSegmentMetadata* metadata
);

/**
 * Return one recorded file path, borrowed from the metadata handle. Returns
 * NULL on error, including an out-of-range index.
 */
const char* lance_index_segment_metadata_file_path_at(
    const LanceIndexSegmentMetadata* metadata,
    size_t index
);

/**
 * Copy one recorded file size. Returns 0 on success, -1 on error; `*out_size`
 * is unchanged on error.
 */
int32_t lance_index_segment_metadata_file_size_at(
    const LanceIndexSegmentMetadata* metadata,
    size_t index,
    uint64_t* out_size
);

/**
 * Read the optional native `created_at` timestamp.
 *
 * On success, absent is reported with `is_set == false` and zeroed fields;
 * present is reported with `is_set == true`. The output is unchanged on error.
 * Returns 0 on success, -1 on error.
 */
int32_t lance_index_segment_metadata_created_at(
    const LanceIndexSegmentMetadata* metadata,
    LanceOptionalTimestamp* out_timestamp
);

/**
 * Read the optional native `base_id`.
 *
 * On success, absent is reported with `is_set == false` and `base_id == 0`;
 * present is reported with `is_set == true`. Both outputs are unchanged on
 * error. Returns 0 on success, -1 on error.
 */
int32_t lance_index_segment_metadata_base_id(
    const LanceIndexSegmentMetadata* metadata,
    bool* out_is_set,
    uint32_t* out_base_id
);

/** Free parsed segment metadata. NULL-safe. */
void lance_index_segment_metadata_free(LanceIndexSegmentMetadata* metadata);

/**
 * Commit previously built uncommitted index segments as one logical index.
 *
 * `segment_metadata_bytes[i]` must point to
 * `segment_metadata_lens[i]` bytes of protobuf-encoded IndexMetadata produced
 * by lance_index_segment_builder_execute_uncommitted() (typically built on
 * distributed workers). All segments are registered under `index_name` on
 * `column` in a single commit, so the dataset version increases by exactly
 * one on success.
 *
 * The segment set is validated by the Lance core and rejected with
 * LANCE_ERR_INVALID_ARGUMENT when it is empty, contains duplicate segment
 * UUIDs, or has overlapping fragment coverage. All segments must share one
 * index type, and the commit fails if `column` does not exist. Every segment
 * must declare `column` as its keyed field — that is, have been built for
 * `column` — or the commit fails with LANCE_ERR_INVALID_ARGUMENT.
 * Vector segments that will coexist (incoming segments and retained existing
 * segments) must have compatible distance metrics, dimensions, sub-index
 * types, and quantizer kinds. Independently trained IVF centroids and PQ
 * codebooks may differ. Incompatible segments are rejected with
 * LANCE_ERR_INVALID_ARGUMENT without changing the dataset version or index.
 *
 * Replacement is automatic and coverage-driven — there is no replace flag:
 * existing same-name segments of the same index type whose fragment coverage
 * is fully covered by the incoming set are replaced, while existing segments
 * covering disjoint fragments are retained as additional deltas of the
 * logical index. A commit that would orphan fragments from an existing
 * segment (partial overlap) is rejected. A commit whose index type differs
 * from the existing same-name index replaces that index entirely, and
 * therefore requires the incoming segments to cover every current fragment;
 * a partial-coverage type change is rejected with LANCE_ERR_INVALID_ARGUMENT.
 * Vector compatibility is checked after selecting replacements, so a complete
 * replacement may change the metric without conflicting with removed segments.
 *
 * @param dataset    Open dataset (mutated; same handle remains valid).
 * @param index_name Logical index name; must not be NULL or empty.
 * @param column     Indexed column; must not be NULL or empty.
 * @param segment_metadata_bytes Array of pointers to encoded IndexMetadata.
 * @param segment_metadata_lens  Array of byte lengths, parallel to
 *                               segment_metadata_bytes.
 * @param segment_count          Number of segments; must be > 0.
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_commit_index_segments(
    LanceDataset* dataset,
    const char* index_name,
    const char* column,
    const uint8_t* const* segment_metadata_bytes,
    const size_t* segment_metadata_lens,
    size_t segment_count
);

/**
 * Merge existing uncommitted physical segments into one uncommitted segment.
 *
 * Every input is a protobuf-encoded native IndexMetadata. Native Lance owns
 * all compatibility, replay, stale-fragment, coverage and type-family rules.
 * An empty set is passed to native validation. On success, `*out_bytes` and
 * `*out_len` receive a library-owned metadata buffer to free with
 * lance_free_bytes(); publication still requires
 * lance_dataset_commit_index_segments(). This call does not commit or mutate
 * the in-memory dataset snapshot.
 */
int32_t lance_dataset_merge_index_segments(
    const LanceDataset* dataset,
    const uint8_t* const* segment_metadata_bytes,
    const size_t* segment_metadata_lens,
    size_t segment_count,
    uint8_t** out_bytes,
    size_t* out_len
);

/**
 * Native prewarm controls.
 *
 * When `has_segment_selection` is false, the UUID pointer and count are
 * ignored and map to native None. When true, count 0 maps to Some(empty) and
 * performs no segment work; a positive count requires `segment_uuids` to point
 * at `segment_count * 16` bytes of concatenated RFC 4122 UUIDs. Unknown
 * segment UUIDs return LANCE_ERR_NOT_FOUND.
 */
typedef struct LanceIndexPrewarmOptions {
    bool with_position;
    bool has_segment_selection;
    const uint8_t* segment_uuids;
    size_t segment_count;
} LanceIndexPrewarmOptions;

/**
 * Synchronously prewarm selected physical segments with native FTS position
 * controls. `options == NULL` preserves the legacy all-segment behavior.
 * Existing index-name/schema-column collision handling applies. Cache entries
 * remain evictable; no residency or latency guarantee is made.
 */
int32_t lance_dataset_prewarm_index_with_options(
    const LanceDataset* dataset,
    const char* index_name,
    const LanceIndexPrewarmOptions* options
);

/**
 * Synchronously prewarm a logical index in the dataset's session index cache.
 *
 * Delegates to the Rust SDK's prewarm_index for all segments with index_name
 * in the handle's current snapshot. Does not change the dataset version.
 * Index types/formats follow the SDK's prewarm support and error behavior.
 *
 * Reuse the same LanceSession when opening subsequent query datasets to reuse
 * warmed entries across handles. Cache entries remain evictable; success does
 * not guarantee full or permanent residency. Ordinary data columns are not
 * prewarmed. An error may leave partially warmed entries; retrying is safe.
 *
 * dataset must be valid and index_name must be a non-empty, NUL-terminated
 * UTF-8 string. Both must remain valid until this blocking call returns.
 * Returns 0 on success, -1 on error (see lance_last_error_code/message).
 * NULL/empty/invalid UTF-8 arguments yield LANCE_ERR_INVALID_ARGUMENT;
 * an index absent from this snapshot yields LANCE_ERR_NOT_FOUND.
 */
int32_t lance_dataset_prewarm_index(const LanceDataset* dataset, const char* index_name);

/** Drop an index by name. Returns -1 (NOT_FOUND) if no such index. */
int32_t lance_dataset_drop_index(LanceDataset* dataset, const char* name);

/**
 * Number of user indexes (excludes system indexes). Returns 0 on error; a
 * dataset with no user indexes also returns 0, so check
 * lance_last_error_code().
 */
uint64_t lance_dataset_index_count(const LanceDataset* dataset);

/**
 * JSON array describing all user indexes.
 * Caller must free the returned string with lance_free_string().
 * Returns NULL on error.
 */
const char* lance_dataset_index_list_json(const LanceDataset* dataset);

/**
 * JSON array of every native IndexDescription, including system indexes.
 *
 * Each entry carries the native name/type URL/index type, keyed field IDs,
 * resolved field paths, approximate indexed-row count, native details JSON,
 * total size (null when any segment lacks file sizes), and segment metadata.
 * Native details conversion and field-path errors are returned as errors; the
 * binding does not substitute `{}` or fabricated field paths. For each
 * segment, fragment_ids is null for unknown coverage and [] for known-empty
 * coverage. An empty dataset returns [].
 *
 * Caller must free the returned string with lance_free_string().
 */
const char* lance_dataset_describe_indices_json(const LanceDataset* dataset);

/**
 * Return the native index-family-specific statistics JSON unchanged.
 *
 * An absent index or empty name returns LANCE_ERR_NOT_FOUND. On old corrupt
 * fragment coverage the native path may publish a migration version before
 * returning; this handle is not automatically refreshed. Caller must free the
 * returned string with lance_free_string().
 */
const char* lance_dataset_index_statistics_json(
    LanceDataset* dataset,
    const char* index_name
);

/**
 * JSON array describing non-system physical index segments visible to the
 * Rust core for this dataset snapshot. Core compatibility filtering applies.
 *
 * Each entry contains `name`, `uuid`, `columns`, `type`, `dataset_version`,
 * `index_version`, and `fragment_ids`.
 *
 * `index_version` is the physical index format version recorded in the
 * segment's Lance IndexMetadata. `fragment_ids` is the recorded coverage in
 * ascending order, including fragment IDs that no longer exist in the current
 * dataset; the listing does not intersect it with the dataset's live
 * fragments. A JSON null means the recorded coverage is unknown, while an
 * empty array means the recorded coverage is known to be empty. Per-segment
 * row counts are not provided.
 *
 * Index names, column names, and other string values are JSON-escaped.
 * Caller must free the returned string with lance_free_string().
 * Returns NULL on error (see lance_last_error_code/message).
 */
const char* lance_dataset_index_segments_json(const LanceDataset* dataset);

/* ─── Distributed vector search: index segment enumeration ─── */

/**
 * Count the segments that make up a logical vector index.
 *
 * A logical index is a set of physical segments (one per distributed-build
 * worker, or one per fragment range). Each segment has a stable UUID. Returns
 * 0 if the index does not exist (also sets `LANCE_ERR_NOT_FOUND`) or on error.
 */
uint64_t lance_dataset_index_segment_count(
    const LanceDataset* dataset,
    const char* index_name
);

/**
 * Fill `out_uuids` with the UUIDs of the segments that make up a logical index.
 * Each UUID is written as 16 raw bytes (RFC 4122 layout).
 *
 * @param out_uuids  Caller-allocated buffer for the UUIDs (byte length >= capacity * 16).
 * @param capacity   Number of UUIDs the buffer can hold.
 * @param out_count  Optional (may be NULL). On success, receives the number of
 *                   UUIDs actually written.
 *
 * Returns 0 on success, -1 on error.  If the index has more segments than
 * `capacity`, returns LANCE_ERR_INVALID_ARGUMENT without writing anything;
 * the caller can retry with a larger buffer.
 */
int32_t lance_dataset_index_segments(
    const LanceDataset* dataset,
    const char* index_name,
    uint8_t* out_uuids,
    size_t capacity,
    uint64_t* out_count
);

/* ─── Vector search (Phase 2) ─── */

/**
 * Set the k-NN query on the scanner.
 * @param column        Vector column (FixedSizeList<element_type>).
 * @param query_data    Pointer to a single query vector of length `query_len`.
 * @param query_len     Number of elements in the query (= column dim).
 * @param element_type  Element type of the query (must match column).
 * @param k             Number of nearest neighbors to return.
 * @return 0 on success, -1 on error.
 *
 * Defined in a follow-up commit; declaration only here.
 */
int32_t lance_scanner_nearest(
    LanceScanner* scanner,
    const char* column,
    const void* query_data,
    size_t query_len,
    LanceDataType element_type,
    uint32_t k
);

/**
 * Restrict a single-vector nearest query to lower_bound <= _distance < upper_bound.
 *
 * Call after lance_scanner_nearest and before starting the scan. Both bounds
 * are copied before returning; NULL means unbounded on that side. Passing NULL
 * for both clears the range. A successful replacement nearest query also clears
 * the range. Batch/multi-vector queries and scans without nearest are rejected.
 *
 * Bounds must be finite; negative distances are allowed. When both are present,
 * lower_bound must be strictly smaller than upper_bound. Invalid calls leave the
 * previous range unchanged. Distances use the configured metric's units (L2 is
 * squared Euclidean distance). Results still have the nearest query's k cap;
 * index-based search remains approximate, not an exhaustive range enumeration.
 *
 * @param scanner      Scanner with a single-vector nearest query.
 * @param lower_bound  Inclusive lower bound, or NULL.
 * @param upper_bound  Exclusive upper bound, or NULL.
 * @return 0 on success, -1 on error (check lance_last_error_*).
 */
int32_t lance_scanner_set_distance_range(
    LanceScanner* scanner,
    const float* lower_bound,
    const float* upper_bound
);

/**
 * Set independent k-NN queries on a FixedSizeList<element_type, dimension> column.
 * query_data contains dimension * num_queries aligned, row-major elements, copied
 * before returning. Supported element types: Float16/32/64, Int8, and UInt8 (Hamming).
 * Floating-point values must be finite and the type/dimension must match the column.
 *
 * Every query returns up to k rows with a zero-based int32 query_index column and
 * _distance, including batches of one query. Duplicate queries retain separate
 * indices. A dataset column named query_index is rejected. No output row order
 * is guaranteed. Index, metric, filter and search settings are shared by all queries.
 *
 * dimension and k must be positive; num_queries must be in 1..=128. Copied values
 * are bounded to 64 MiB; num_queries * k * refine_factor must be <= 100000, using
 * 1 when refinement is unset. Explicit refinement must be positive. These are
 * request bounds, not a total index/payload-memory limit.
 *
 * Must be configured before scanning. Replaces a previous nearest query atomically;
 * an invalid request preserves it. Mutually exclusive with FTS and global scanner
 * limit/offset, in either configuration order. Apply per-query windows in the caller.
 * Streams retain the existing ownership, error and early-release/cancellation contract.
 * Returns 0 on success, -1 on error; use lance_last_error_code/message for details.
 */
int32_t lance_scanner_nearest_batch(
    LanceScanner* scanner, const char* column, const void* query_data,
    size_t dimension, size_t num_queries, LanceDataType element_type, uint32_t k
);

/**
 * Set one multi-vector query on a List<FixedSizeList<float16|float32|float64>> column.
 * Inner vectors must be non-nullable and contain no null elements; the outer list may be nullable.
 * query_data contains dimension * num_vectors aligned elements in row-major order.
 * Both sizes and k must be positive. At most 128 query subvectors are accepted;
 * num_vectors * k and refine_factor * k must each be at most 100000.
 * Values are copied before returning. The default metric is L2 on every fragment.
 * Scores sum each query vector's minimum distance; refinement defaults to 1.
 * Returns 0 on success, -1 on error. Stored invalid elements fail during execution.
 */
int32_t lance_scanner_nearest_multivector(
    LanceScanner* scanner, const char* column, const void* query_data,
    size_t dimension, size_t num_vectors, LanceDataType element_type, uint32_t k
);

/**
 * Set both the minimum and maximum vector-index partition-search bounds.
 *
 * This replaces both bounds configured by earlier calls to any nprobes
 * setter. The value must be greater than zero. Must be set before scanning.
 */
int32_t lance_scanner_set_nprobes(LanceScanner* scanner, uint32_t nprobes);

/**
 * Set the minimum number of vector-index partitions to search.
 * This replaces only the minimum bound; the current maximum is preserved.
 * Must be greater than zero and no greater than an explicitly configured
 * `maximum_nprobes`. A conflict with only a fixed `nprobes` value is reported
 * when the nearest scan is prepared. An invalid explicit range is rejected
 * without changing either bound.
 * Must be set before scanning starts.
 */
int32_t lance_scanner_set_minimum_nprobes(
    LanceScanner* scanner,
    uint32_t minimum_nprobes
);

/**
 * Set the maximum number of vector-index partitions to search.
 * This replaces only the maximum bound; the current minimum is preserved.
 * Must be greater than zero and no less than an explicitly configured
 * `minimum_nprobes`. A conflict with only a fixed `nprobes` value is reported
 * when the nearest scan is prepared.
 * This only affects prefiltered searches that need more candidates.
 * An invalid explicit range is rejected without changing either bound.
 * Must be set before scanning starts.
 */
int32_t lance_scanner_set_maximum_nprobes(
    LanceScanner* scanner,
    uint32_t maximum_nprobes
);

/**
 * Set the native continuous IVF search effort.
 *
 * The value must be finite and in [0, 1]. Any explicit value, including 0.5,
 * ignores a fixed nprobes count while explicit minimum and maximum bounds
 * remain effective, regardless of setter order. Invalid values are rejected
 * without changing the previous configuration. Must be set before scanning.
 * A scanner without nearest() fails when materialized.
 */
int32_t lance_scanner_set_search_effort(
    LanceScanner* scanner,
    double effort
);

/**
 * Configure the speed / accuracy tradeoff for approximate vector search.
 * Must be set before scanning starts.
 */
int32_t lance_scanner_set_approx_mode(
    LanceScanner* scanner,
    LanceApproxMode approx_mode
);

/**
 * Set vector index partition-search concurrency for each query.
 *
 * A value of -1 uses the CPU pool size, 0 selects Lance's automatic policy,
 * 1 uses the sequential path, and values greater than 1 request parallel
 * partition search. The effective value is capped by available parallelism.
 * Values below -1 are rejected. Must be set before scanning starts.
 */
int32_t lance_scanner_set_query_parallelism(
    LanceScanner* scanner,
    int32_t query_parallelism
);
int32_t lance_scanner_set_refine_factor(LanceScanner* scanner, uint32_t f);
int32_t lance_scanner_set_ef(LanceScanner* scanner, uint32_t e);
int32_t lance_scanner_set_metric(LanceScanner* scanner, LanceMetricType metric);
int32_t lance_scanner_set_use_index(LanceScanner* scanner, bool enable);

/**
 * Configure native indexed-only fast search.
 *
 * This weak-consistency mode applies to vector queries, scalar-indexed filters,
 * full-text search and MinHash search. Index-backed paths can omit unindexed
 * data; for MinHash search it omits appended/unindexed fragments. Ordinary
 * scans and expressions without an indexable path follow the native planner's
 * behavior.
 *
 * Lance's native `fast_search()` is applied before the nearest query and
 * explicit `use_index` setting. For vector queries, unindexed fragments are
 * omitted and the result is empty when no usable index is selected, including
 * when `use_index=false`. Disabling restores the default native behavior.
 * Disabled by default. Must be set before the scan starts.
 *
 * Passing NULL scanner or setting this after scanning starts returns -1 and
 * leaves the prior state unchanged.
 *
 * @param scanner Scanner handle. Must not be NULL.
 * @param enable  True for indexed-only search, false for native default.
 * @return 0 on success, -1 on error (check `lance_last_error_*`).
 */
int32_t lance_scanner_set_fast_search(LanceScanner* scanner, bool enable);

/**
 * Choose when scanner filter expressions are applied during vector or MinHash
 * search. When enabled, filters constrain candidates before ranking; when
 * disabled, they refine the ranked candidates afterward and may produce fewer
 * than the configured limit. This does not change the fragment search domain set
 * by lance_scanner_set_fragment_ids().
 */
int32_t lance_scanner_set_prefilter(LanceScanner* scanner, bool enable);

/**
 * Restrict the next k-NN query to a specific subset of vector index segments.
 *
 * Used by distributed query engines (e.g. Velox) to fan a single k-NN query
 * out across workers, each handling a slice of segments. The coordinator gets
 * the segment list via `lance_dataset_index_segments()`.
 *
 * @param segment_uuids Pointer to `len` 16-byte UUIDs concatenated end-to-end
 *                      (total byte length = `len * 16`). Each UUID identifies
 *                      one physical segment of a logical index.
 * @param len           Number of UUIDs. Pass 0 (and segment_uuids may be NULL)
 *                      to clear any previously-set segment restriction.
 * @return 0 on success, -1 on error.
 */
int32_t lance_scanner_set_index_segments(
    LanceScanner* scanner,
    const uint8_t* segment_uuids,
    size_t len
);

/**
 * Accelerate an ordinary scalar-filtered scan with one physical index segment.
 * segment_uuid points to 16 UUID bytes in RFC 4122 order; NULL clears the setting.
 * Must be configured before scanning. Requires explicit nonempty fragment_ids,
 * which define BOTH the read and fallback domain, independently of the segment.
 * Missing snapshot UUIDs / fragment IDs are errors. Extra segment coverage is
 * excluded by fragment_ids; incomplete coverage falls back to a full filtered
 * scan of those fragment_ids. Callers distributing work must assign disjoint
 * fragment domains and separately include any unindexed data they wish to read.
 * The segment metadata must identify one key field present in the schema.
 *
 * BTree/Bitmap/LabelList searches evaluate a candidate expression on the selected
 * logical index, including AND, OR, IN and NULL-aware NOT, and require an Exact
 * result. LabelList supports Lance array_has/array_contains, array_has_all and
 * array_has_any through SQL or Substrait filters with matching list element types.
 * These functions retain Lance semantics, including for NULL search values.
 * An AND may retain only its supported necessary conditions. OR needs
 * candidates for both branches; NOT requires its complete indexed subtree.
 * Expressions exceeding 128 nodes or depth 32 use the scoped fallback.
 * Filters containing IS [NOT] TRUE/FALSE also use that fallback until the Lance
 * planner dependency preserves their NULL semantics under negation.
 * use_scalar_index=false skips segment search and uses the scoped fallback;
 * snapshot UUID and fragment validation still applies.
 * AtMost/AtLeast results fall back to a full filtered scan of fragment_ids.
 * All predicates are reapplied during candidate reads; other scalar indices
 * are disabled. Legacy storage, expressions without safe scoped candidates,
 * overlays, fragment reuse, unsupported index types / result domains
 * and missing coverage use the same domain without an index. No filter also
 * falls back. LIMIT/OFFSET apply after the complete scanner filter, never to the
 * unfiltered candidate set. Vector/MinHash/FTS queries and
 * include_deleted_rows=true are rejected even when use_scalar_index=false;
 * segment mode is live-row-only.
 *
 * UUID bytes are copied. Metadata and final option compatibility are validated
 * when creating the stream. Index corruption or I/O failures remain errors.
 */
int32_t lance_scanner_set_scalar_index_segment(
    LanceScanner* scanner, const uint8_t* segment_uuid);

/* Opt in to bounded runtime BTree page-cost selection for a scalar segment scan.
 * Applies only to scalar_index_segment scans and defaults to false.
 * The explicit fragment domain and complete filter are preserved.
 * Unsupported/unknown estimates retain the selected segment. Returns 0 or -1. */
int32_t lance_scanner_set_scalar_index_auto_selection(LanceScanner* scanner, bool enabled);

/* ─── Full-text search (Phase 2) ─── */

/**
 * Required relationship between a pinned dataset snapshot and its committed
 * FTS index segments. Values are ABI-stable; API parameters use int32_t.
 */
typedef enum {
    /** Fail prepare if any current fragment is not covered by the FTS index. */
    LANCE_FTS_COVERAGE_STRICT = 0,
    /** Score and search only rows covered by committed FTS index segments. */
    LANCE_FTS_COVERAGE_INDEX_ONLY = 1,
} LanceFtsCoverageMode;

/** How the analyzed terms of one Match query are combined. */
typedef enum {
    /** At least one analyzed term must match. */
    LANCE_FTS_MATCH_OPERATOR_OR = 0,
    /** Every analyzed term must match. */
    LANCE_FTS_MATCH_OPERATOR_AND = 1,
} LanceFtsMatchOperator;

/** Document unit selected by a Match or Phrase query. */
typedef enum {
    /** Infer the granularity stored by the target FTS index. */
    LANCE_FTS_DOCUMENT_GRANULARITY_UNSPECIFIED = 0,
    /** All text selected from one dataset row is one document. */
    LANCE_FTS_DOCUMENT_GRANULARITY_ROW = 1,
    /** Each element of the deepest list on the field path is one document. */
    LANCE_FTS_DOCUMENT_GRANULARITY_LIST_ELEMENT = 2,
} LanceFtsDocumentGranularity;

/** Membership requirement for one Boolean query clause. */
typedef enum {
    /** The clause may match and contributes its score when it does. */
    LANCE_FTS_OCCUR_SHOULD = 0,
    /** The clause must match and contributes its score. */
    LANCE_FTS_OCCUR_MUST = 1,
    /** The clause must not match and never contributes to the score. */
    LANCE_FTS_OCCUR_MUST_NOT = 2,
} LanceFtsOccur;

/**
 * Options for lance_fts_query_match_new(). Call
 * lance_fts_query_match_options_init() before changing individual fields.
 */
typedef struct LanceFtsMatchOptions {
    /** Score multiplier; native default 1.0. Scan planning requires finite,
     *  non-negative values. */
    float boost;
    /** -1 represents the core AUTO setting. The Lance 14.0.2 dataset planner
     *  resolves AUTO to exact matching. Other values are explicit edit
     *  distances in [0, UINT32_MAX], with 0 selecting exact matching.
     *  Native default 0. */
    int64_t fuzziness;
    /** Final fuzzy vocabulary budget for this Match leaf. Native default 50. */
    uint64_t max_expansions;
    /** Fixed-width LanceFtsMatchOperator discriminant. Native default OR. */
    int32_t match_operator;
    /** Beginning characters left unchanged during fuzzy matching. Native
     *  default 0. */
    uint32_t prefix_length;
    /** Fixed-width LanceFtsDocumentGranularity discriminant. Native default
     *  UNSPECIFIED. */
    int32_t document_granularity;
} LanceFtsMatchOptions;

/**
 * Options for lance_fts_query_phrase_new(). Call
 * lance_fts_query_phrase_options_init() before changing individual fields.
 */
typedef struct LanceFtsPhraseOptions {
    /** Maximum non-negative number of intervening token positions. Native
     *  default 0. */
    uint32_t slop;
    /** Fixed-width LanceFtsDocumentGranularity discriminant. Native default
     *  UNSPECIFIED. */
    int32_t document_granularity;
} LanceFtsPhraseOptions;

/**
 * Options for lance_fts_query_multi_match_new(). Call
 * lance_fts_query_multi_match_options_init() before changing individual
 * fields.
 */
typedef struct LanceFtsMultiMatchOptions {
    /** Optional per-column boosts. NULL means the native default of 1.0 for
     *  every column. A non-NULL pointer makes boost_count significant. */
    const float* boosts;
    /** Number of entries readable through boosts. */
    size_t boost_count;
    /** Fixed-width LanceFtsMatchOperator discriminant. Native default OR. */
    int32_t match_operator;
} LanceFtsMultiMatchOptions;

/**
 * Options for lance_fts_query_combined_fields_new(). Call
 * lance_fts_query_combined_fields_options_init() before changing individual
 * fields.
 */
typedef struct LanceFtsCombinedFieldsOptions {
    /** Optional per-column BM25F weights. NULL means the native default of 1.0
     *  for every column. A non-NULL pointer makes boost_count significant. */
    const float* boosts;
    /** Number of entries readable through boosts. */
    size_t boost_count;
    /** Fixed-width LanceFtsMatchOperator discriminant. Native default OR. */
    int32_t match_operator;
} LanceFtsCombinedFieldsOptions;

/**
 * Options for lance_fts_query_boost_new(). Call
 * lance_fts_query_boost_options_init() before changing individual fields.
 */
typedef struct LanceFtsBoostOptions {
    /** Multiplier subtracted from the positive score. Native default 0.5.
     *  Scan planning requires finite, non-negative values. */
    float negative_boost;
} LanceFtsBoostOptions;

/** Initialize Match options with the native defaults. NULL-safe. */
void lance_fts_query_match_options_init(LanceFtsMatchOptions* options);

/** Initialize Phrase options with the native defaults. NULL-safe. */
void lance_fts_query_phrase_options_init(LanceFtsPhraseOptions* options);

/** Initialize MultiMatch options with the native defaults. NULL-safe. */
void lance_fts_query_multi_match_options_init(LanceFtsMultiMatchOptions* options);

/** Initialize CombinedFields options with the native defaults. NULL-safe. */
void lance_fts_query_combined_fields_options_init(
    LanceFtsCombinedFieldsOptions* options
);

/** Initialize Boost options with the native defaults. NULL-safe. */
void lance_fts_query_boost_options_init(LanceFtsBoostOptions* options);

/**
 * Create an immutable Match query.
 *
 * @param terms    Query text. May be empty. Must not be NULL.
 * @param column   Target column, or NULL to let scan planning resolve the
 *                 indexed columns.
 * @param options  Match options, or NULL for the native defaults.
 * @return Query handle on success, or NULL on error.
 */
LanceFtsQuery* lance_fts_query_match_new(
    const char* terms,
    const char* column,
    const LanceFtsMatchOptions* options
);

/**
 * Create an immutable Phrase query.
 *
 * The selected FTS index must store token positions; scan planning reports an
 * error otherwise.
 *
 * @param terms    Query text. May be empty. Must not be NULL.
 * @param column   Target column, or NULL to let scan planning resolve the
 *                 indexed columns.
 * @param options  Phrase options, or NULL for the native defaults.
 * @return Query handle on success, or NULL on error.
 */
LanceFtsQuery* lance_fts_query_phrase_new(
    const char* terms,
    const char* column,
    const LanceFtsPhraseOptions* options
);

/**
 * Create an immutable MultiMatch query.
 *
 * Each target field is scored independently and the maximum field score is
 * retained, matching the native best-fields behavior. Native construction
 * rejects an empty column list. A provided boost list must contain exactly one
 * entry per column; its values are validated by scan planning.
 *
 * @param terms         Query text. May be empty. Must not be NULL.
 * @param columns       Array of column names. Must not be NULL when
 *                      column_count is non-zero.
 * @param column_count  Number of entries in columns.
 * @param options       MultiMatch options, or NULL for the native defaults.
 * @return Query handle on success, or NULL on error.
 */
LanceFtsQuery* lance_fts_query_multi_match_new(
    const char* terms,
    const char* const* columns,
    size_t column_count,
    const LanceFtsMultiMatchOptions* options
);

/**
 * Create an immutable CombinedFields (BM25F) query.
 *
 * The target columns form one virtual field whose term statistics are blended.
 * Native construction rejects an empty or duplicate column list and weights
 * outside [1, 2^20].
 *
 * @param terms         Query text. May be empty. Must not be NULL.
 * @param columns       Array of unique column names. Must not be NULL when
 *                      column_count is non-zero.
 * @param column_count  Number of entries in columns.
 * @param options       CombinedFields options, or NULL for the native defaults.
 * @return Query handle on success, or NULL on error.
 */
LanceFtsQuery* lance_fts_query_combined_fields_new(
    const char* terms,
    const char* const* columns,
    size_t column_count,
    const LanceFtsCombinedFieldsOptions* options
);

/**
 * Create an immutable Boolean query.
 *
 * An empty clause list is accepted here and rejected by scan planning when the
 * query has no positive clause. Both arrays must contain query_count entries
 * unless the count is zero. Child query values are copied during construction.
 *
 * @param occurs       Fixed-width LanceFtsOccur discriminants.
 * @param queries      Child query handles.
 * @param query_count  Number of entries in both arrays.
 * @return Query handle on success, or NULL on error.
 */
LanceFtsQuery* lance_fts_query_boolean_new(
    const int32_t* occurs,
    const LanceFtsQuery* const* queries,
    size_t query_count
);

/**
 * Create an immutable Boost query from positive and negative child queries.
 * Child query values are copied during construction. negative_boost is
 * validated by scan planning.
 *
 * @param positive  Positive child query. Must not be NULL.
 * @param negative  Negative child query. Must not be NULL.
 * @param options   Boost options, or NULL for the native default.
 * @return Query handle on success, or NULL on error.
 */
LanceFtsQuery* lance_fts_query_boost_new(
    const LanceFtsQuery* positive,
    const LanceFtsQuery* negative,
    const LanceFtsBoostOptions* options
);

/**
 * Close a typed query handle. NULL-safe. Composite children and scanners that
 * already attached this query retain their own cloned values.
 */
void lance_fts_query_close(LanceFtsQuery* query);

/**
 * Prepare an OR Match query context for one column.
 *
 * @deprecated Use lance_dataset_prepare_fts_match_query() to select the Match
 *             operator explicitly. This compatibility API is equivalent to
 *             LANCE_FTS_MATCH_OPERATOR_OR.
 */
LanceFtsQueryContext* lance_dataset_prepare_fts_query(
    const LanceDataset* dataset,
    const char* column,
    const char* query,
    uint32_t max_fuzzy_distance,
    int32_t coverage_mode
);

/**
 * Prepare an immutable, process-local Match query context for one column.
 *
 * Preparation pins the dataset handle's current snapshot, enumerates all
 * committed FTS segments for `column`, checks fragment coverage, opens those
 * segments, and prepares one global BM25 scorer across their indexed
 * documents. `match_operator` supports both AND and OR.
 *
 * The context can be shared by scanners created from the exact same
 * process-local dataset snapshot. It has no serialization or cross-process
 * transport format. Reopening the same URI and manifest version creates a
 * different identity and cannot reuse the context because storage options and
 * object-store endpoints may differ.
 *
 * In LANCE_FTS_COVERAGE_INDEX_ONLY mode, unindexed fragments are allowed and
 * excluded from both the scorer corpus and query results. In STRICT mode any
 * unindexed fragment makes this call fail.
 *
 * @param match_operator Fixed-width LanceFtsMatchOperator discriminant.
 * @param max_fuzzy_distance Reserved for prepared fuzzy matching and currently
 *                           must be 0. The parameter is retained so enabling
 *                           canonical cross-segment fuzzy vocabulary injection
 *                           later does not require another C ABI change.
 * @param coverage_mode Fixed-width LanceFtsCoverageMode discriminant.
 * @return Context handle on success, or NULL on error.
 */
LanceFtsQueryContext* lance_dataset_prepare_fts_match_query(
    const LanceDataset* dataset,
    const char* column,
    const char* query,
    int32_t match_operator,
    uint32_t max_fuzzy_distance,
    int32_t coverage_mode
);

/**
 * Prepare an immutable, process-local Phrase query context for one column.
 *
 * The selected FTS index must store token positions. `slop == 0` requires an
 * exact phrase; a positive value permits that many intervening positions.
 * Dataset identity, coverage, sharing, and segment-scoped execution follow the
 * same contract as lance_dataset_prepare_fts_match_query().
 *
 * @param slop Maximum non-negative number of intervening token positions
 *             permitted between adjacent phrase terms.
 * @param coverage_mode Fixed-width LanceFtsCoverageMode discriminant.
 * @return Context handle on success, or NULL on error.
 */
LanceFtsQueryContext* lance_dataset_prepare_fts_phrase_query(
    const LanceDataset* dataset,
    const char* column,
    const char* query,
    int32_t slop,
    int32_t coverage_mode
);

/**
 * Close a context handle. NULL-safe. Scanners that already attached this
 * context retain shared ownership and remain valid.
 */
void lance_fts_query_context_close(LanceFtsQueryContext* context);

/**
 * Set a BM25 full-text search query on the scanner.
 *
 * Mutually exclusive with lance_scanner_nearest: calling either after the
 * other returns LANCE_ERR_INVALID_ARGUMENT.
 *
 * @param query              Query string (terms).
 * @param columns            NULL-terminated array of columns, or NULL for all
 *                           FTS-indexed columns.
 * @param max_fuzzy_distance 0 = exact match; >0 = MatchQuery::with_fuzziness.
 * @return 0 on success, -1 on error.
 */
int32_t lance_scanner_full_text_search(
    LanceScanner* scanner,
    const char* query,
    const char* const* columns,
    uint32_t max_fuzzy_distance
);

/**
 * Attach an immutable typed full-text query tree to the scanner.
 *
 * The scanner snapshots the query value, so the query handle and every source
 * child handle may be closed after this function returns successfully.
 *
 * This is mutually exclusive with nearest search and with
 * lance_scanner_set_fts_query_context(). Calling it again replaces the
 * currently attached query. Invalid input returns an error without changing
 * the previously attached configuration. Query-tree validation that requires
 * planning (for example a Boolean tree with no positive clause) is reported
 * when the scan starts.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_scanner_set_fts_query(
    LanceScanner* scanner,
    const LanceFtsQuery* query
);

/**
 * Attach a prepared process-local FTS query context. The scanner must have
 * been created from the exact LanceDataset snapshot used to prepare the
 * context; URI and manifest version equality is not sufficient. The scanner
 * retains shared ownership, so the caller may close `context` after success.
 * This is mutually exclusive with nearest and lance_scanner_full_text_search
 * because the context already owns the FTS query.
 */
int32_t lance_scanner_set_fts_query_context(
    LanceScanner* scanner,
    const LanceFtsQueryContext* context
);

/**
 * Set a native MinHash LSH text-similarity query on the scanner.
 *
 * The column and text are copied and may be released after this call. A valid
 * earlier query remains attached when this call fails. An empty column is
 * rejected by the native query builder; an empty text is accepted by native
 * Lance and produces no matching signatures.
 *
 * The scanner limit is required when planning this query and controls the
 * number of returned rows. Native planning searches rows with the column's
 * MinHash LSH index, covers unindexed fragments by default, and adds
 * `_distance`, equal to one minus the estimated Jaccard similarity. Filters,
 * prefiltering, fast search, fragment scope, row masks, projection, offset, and
 * query-combination errors follow native Lance behavior.
 *
 * @param scanner Scanner handle. Must not be NULL.
 * @param column  Text column with a MinHash LSH index. Must not be NULL or
 *                empty after native validation.
 * @param text    Query text. Must not be NULL; empty text is allowed.
 * @return 0 on success, -1 on error (check `lance_last_error_*`).
 */
int32_t lance_scanner_set_minhash_query(
    LanceScanner* scanner,
    const char* column,
    const char* text
);

/**
 * Restrict a context-backed FTS scan to `len` context segment UUIDs supplied
 * by the caller's planner. Pass `len == 0` to clear the restriction and search
 * all context segments. Duplicate or unknown UUIDs are rejected.
 */
int32_t lance_scanner_set_fts_index_segments(
    LanceScanner* scanner,
    const uint8_t* segment_uuids,
    size_t len
);

/* ─── Dataset writer ─── */

/**
 * Write mode for lance_dataset_write. Values are ABI-stable.
 *
 * The `mode` parameter on the FFI call is a fixed-width int32_t — not this
 * enum type — so callers built with `-fshort-enums` or non-default enum
 * sizing cannot mismatch the Rust ABI. The Rust implementation validates the
 * received integer and rejects any out-of-range value with
 * LANCE_ERR_INVALID_ARGUMENT.
 */
typedef enum {
    LANCE_WRITE_CREATE    = 0,  /* Create new dataset; fail if path exists. */
    LANCE_WRITE_APPEND    = 1,  /* Append; fail if the new schema is incompatible. */
    LANCE_WRITE_OVERWRITE = 2,  /* Overwrite existing, or create if missing. */
} LanceWriteMode;

/**
 * Write an Arrow record batch stream to a Lance dataset at `uri`, committing
 * a manifest.
 *
 * This legacy entry point records the documented lance-c auto-cleanup policy
 * in a newly created manifest: every 20 committed versions, versions older
 * than 14 days are reclaimed automatically. Callers relying on
 * lance_dataset_versions / lance_dataset_restore for time travel should be
 * aware that versions past that horizon may no longer exist. Use
 * lance_dataset_write_with_options to select native WriteParams defaults
 * instead.
 *
 * @param uri          Dataset URI (file://, s3://, memory://, etc.). Must not
 *                     be NULL or an empty string.
 * @param schema       Required Arrow schema. The stream schema must match or
 *                     the call fails with LANCE_ERR_INVALID_ARGUMENT. This
 *                     function does NOT call schema->release; the caller
 *                     retains ownership and must release the schema after the
 *                     call returns (success or failure).
 * @param stream       Arrow C Data Interface stream consumed by this call.
 *                     Do not use the stream after returning, regardless of
 *                     the return code.
 * @param mode         CREATE / APPEND / OVERWRITE (see LanceWriteMode).
 * @param storage_opts NULL-terminated key-value pairs ["k","v",NULL], or NULL.
 * @param out_dataset  If non-NULL, on success receives an open LanceDataset*
 *                     at the newly-committed version (caller must
 *                     lance_dataset_close it). Pass NULL to discard. On error
 *                     *out_dataset is left unchanged — do not read or free it.
 *                     On entry `*out_dataset` should be NULL or a pointer
 *                     whose previous value is no longer needed; this function
 *                     overwrites the slot on success without releasing any
 *                     prior handle.
 * @return 0 on success, -1 on error. Possible error codes include
 *         LANCE_ERR_DATASET_ALREADY_EXISTS (CREATE on an existing path),
 *         LANCE_ERR_INVALID_ARGUMENT (NULL/empty args, invalid mode,
 *         schema mismatch),
 *         LANCE_ERR_COMMIT_CONFLICT (concurrent writer).
 */
int32_t lance_dataset_write(
    const char* uri,
    const struct ArrowSchema* schema,
    struct ArrowArrayStream* stream,
    int32_t mode,
    const char* const* storage_opts,
    LanceDataset** out_dataset
);

/**
 * Tunable parameters for lance_dataset_write_with_params. Numeric fields
 * default-out via 0; `data_storage_version` defaults out via NULL.
 *
 * Note: `enable_stable_row_ids` is a `bool` and therefore has no default
 * sentinel — callers that zero-initialize this struct end up explicitly
 * setting it to false (which matches upstream's current default).
 */
typedef struct LanceWriteParams {
    /* Soft cap on rows per data file. 0 = default. */
    uint64_t    max_rows_per_file;
    /* Soft cap on rows per row group. 0 = default. */
    uint64_t    max_rows_per_group;
    /* Soft cap on bytes per data file (~90 GB upstream default). 0 = default. */
    uint64_t    max_bytes_per_file;
    /* Lance file format version, e.g. "2.0", "2.1", "stable", "legacy".
     * NULL = default. Invalid strings → LANCE_ERR_INVALID_ARGUMENT. */
    const char* data_storage_version;
    /* Opt into stable row ids (better for compaction at a small write cost).
     * Strictly an override — see struct-level note above. */
    bool        enable_stable_row_ids;
} LanceWriteParams;

/**
 * Native auto-cleanup parameters for lance_dataset_write_with_options.
 *
 * `interval` is the number of committed versions between cleanup runs; 0 is a
 * valid native value that considers cleanup on every commit. It is narrowed to
 * the native usize type; values that do not fit return
 * LANCE_ERR_INVALID_ARGUMENT.
 *
 * `older_than_seconds` and `older_than_nanos` express the native
 * chrono::TimeDelta retention window as
 * `older_than_seconds + older_than_nanos / 1000000000`. Both fields carry the
 * sign, and `older_than_nanos` preserves subsecond precision. It must be in
 * [-999999999, 999999999], and the combined value must fit in the native
 * TimeDelta range (about +/-292 million years). Negative durations are
 * forwarded to Lance, which rejects them when it builds the write transaction
 * because the persisted config uses std::time::Duration.
 */
typedef struct LanceAutoCleanupParams {
    /* Committed versions between cleanup runs. 0 is valid. */
    uint64_t interval;
    /* Whole-second component of the retention window. */
    int64_t  older_than_seconds;
    /* Signed subsecond component, in nanoseconds. */
    int32_t  older_than_nanos;
} LanceAutoCleanupParams;

/**
 * Native write options for lance_dataset_write_with_options.
 *
 * Zero-initialize this struct to select native WriteParams::default():
 * auto_cleanup = NULL (native None) and skip_auto_cleanup = false. The embedded
 * `params` retains the zero-means-upstream-default mapping of LanceWriteParams.
 *
 * A non-NULL `auto_cleanup` selects native Some, including interval 0 or a
 * zero-length retention window. `skip_auto_cleanup` affects only the current
 * commit and never changes persisted configuration.
 */
typedef struct LanceWriteOptions {
    /* Output-shape overrides. Zero-initialize to inherit upstream defaults. */
    LanceWriteParams params;
    /* Native WriteParams::auto_cleanup; NULL means None. */
    const LanceAutoCleanupParams* auto_cleanup;
    /* Native WriteParams::skip_auto_cleanup. */
    bool skip_auto_cleanup;
} LanceWriteOptions;

/**
 * Same as lance_dataset_write but takes a LanceWriteParams for tuning the
 * output shape. Pass `params` = NULL to use defaults (equivalent to calling
 * lance_dataset_write directly).
 *
 * This legacy entry point keeps the documented 20-version / 14-day
 * auto-cleanup policy. Use lance_dataset_write_with_options for native
 * WriteParams defaults.
 *
 * @return 0 on success, -1 on error. See lance_dataset_write for the error
 *         code list; invalid `data_storage_version` also returns
 *         LANCE_ERR_INVALID_ARGUMENT.
 */
int32_t lance_dataset_write_with_params(
    const char* uri,
    const struct ArrowSchema* schema,
    struct ArrowArrayStream* stream,
    int32_t mode,
    const LanceWriteParams* params,
    const char* const* storage_opts,
    LanceDataset** out_dataset
);

/**
 * Same as lance_dataset_write but selects native WriteParams defaults instead
 * of the legacy 20-version / 14-day policy.
 *
 * Pass `options` = NULL to use a zero-initialized LanceWriteOptions, which is
 * equivalent to WriteParams::default(): auto_cleanup = NULL (None) and
 * skip_auto_cleanup = false.
 *
 * On an existing dataset, auto_cleanup is written only by CREATE; APPEND and
 * OVERWRITE leave the persisted policy unchanged. An explicit NULL
 * auto_cleanup on an existing manifest therefore does not delete stored
 * cleanup configuration.
 *
 * The stream is consumed on every return path except a NULL `stream`.
 * `out_dataset` follows the same contract as lance_dataset_write.
 *
 * @return 0 on success, -1 on error. Error codes match
 *         lance_dataset_write. Invalid auto-cleanup values also return
 *         LANCE_ERR_INVALID_ARGUMENT.
 */
int32_t lance_dataset_write_with_options(
    const char* uri,
    const struct ArrowSchema* schema,
    struct ArrowArrayStream* stream,
    int32_t mode,
    const LanceWriteOptions* options,
    const char* const* storage_opts,
    LanceDataset** out_dataset
);

/* ─── Native data files ─── */

/** Owned native `DataFile` metadata snapshot. */
typedef struct LanceDataFile LanceDataFile;

/**
 * Create native data-file metadata from an existing Lance file.
 *
 * Maps directly to `Dataset::create_data_file`. `path` is relative to the
 * selected native data directory and must be non-NULL UTF-8. `base_id == NULL`
 * maps to native `None`; a non-NULL pointer maps to native `Some`, including
 * zero. Empty paths, missing files, malformed Lance files, schema mismatches
 * and unregistered base IDs retain native validation and error timing.
 *
 * On success `*out_file` receives an independently owned snapshot that remains
 * valid after the dataset is closed. The output is written only on success.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_create_data_file(
    const LanceDataset* dataset,
    const char* path,
    const uint32_t* base_id,
    LanceDataFile** out_file
);

/** Close a data-file handle. NULL-safe. Close exactly once. */
void lance_data_file_close(LanceDataFile* file);

/**
 * Clone a data-file handle into an independent snapshot.
 * Returns NULL on error; close the result exactly once.
 */
LanceDataFile* lance_data_file_clone(const LanceDataFile* file);

/**
 * Return the native data-file path. The pointer is borrowed from `file` and
 * remains valid until the handle is closed. Returns NULL on error.
 */
const char* lance_data_file_path(const LanceDataFile* file);

/**
 * Read the number of native field IDs. Returns 0 on error; check
 * `lance_last_error_code()` to distinguish failure from an empty list.
 */
size_t lance_data_file_field_count(const LanceDataFile* file);

/**
 * Read one native field ID. Returns 0 on success, -1 on error.
 * `out_field_id` is written only on success.
 */
int32_t lance_data_file_field_id_at(
    const LanceDataFile* file,
    size_t index,
    int32_t* out_field_id
);

/**
 * Read the number of native column indices. Returns 0 on error; check
 * `lance_last_error_code()` to distinguish failure from an empty list.
 */
size_t lance_data_file_column_index_count(const LanceDataFile* file);

/**
 * Read one native column index, including the native `-1` marker.
 * Returns 0 on success, -1 on error. The output is written only on success.
 */
int32_t lance_data_file_column_index_at(
    const LanceDataFile* file,
    size_t index,
    int32_t* out_column_index
);

/**
 * Read the native data-file format version. Both outputs are required and
 * written only on success. Returns 0 on success, -1 on error.
 */
int32_t lance_data_file_file_version(
    const LanceDataFile* file,
    uint32_t* out_major,
    uint32_t* out_minor
);

/**
 * Read the optional native data-file size. On success `*out_is_set` reports
 * native `Some`/`None`; `out_size` is written only when present.
 * Returns 0 on success, -1 on error.
 */
int32_t lance_data_file_file_size(
    const LanceDataFile* file,
    bool* out_is_set,
    uint64_t* out_size
);

/**
 * Read the optional native base ID. On success `*out_is_set` reports native
 * `Some`/`None`; `out_base_id` is written only when present.
 * Returns 0 on success, -1 on error.
 */
int32_t lance_data_file_base_id(
    const LanceDataFile* file,
    bool* out_is_set,
    uint32_t* out_base_id
);

/* ─── Native transactions ─── */

/**
 * Native operation variant. Discriminants are ABI-stable.
 *
 * Append and Overwrite are constructed by the native writer. Delete,
 * CreateIndex and DataReplacement have typed constructors and inspection. The
 * other variants can be read from history and inspected by kind; their
 * constructors and variant-specific mutation helpers remain later issue #112
 * scope.
 */
typedef enum {
    LANCE_OPERATION_UNKNOWN             = 0,
    LANCE_OPERATION_APPEND              = 1,
    LANCE_OPERATION_DELETE              = 2,
    LANCE_OPERATION_OVERWRITE            = 3,
    LANCE_OPERATION_CREATE_INDEX        = 4,
    LANCE_OPERATION_REWRITE             = 5,
    LANCE_OPERATION_DATA_REPLACEMENT    = 6,
    LANCE_OPERATION_DATA_OVERLAY        = 7,
    LANCE_OPERATION_MERGE               = 8,
    LANCE_OPERATION_RESTORE             = 9,
    LANCE_OPERATION_RESERVE_FRAGMENTS   = 10,
    LANCE_OPERATION_UPDATE              = 11,
    LANCE_OPERATION_PROJECT             = 12,
    LANCE_OPERATION_UPDATE_CONFIG       = 13,
    LANCE_OPERATION_UPDATE_MEM_WAL_STATE = 14,
    LANCE_OPERATION_CLONE               = 15,
    LANCE_OPERATION_UPDATE_BASES        = 16,
} LanceOperationKind;

/**
 * Native write options for an uncommitted Arrow stream.
 *
 * `write` reuses the `LanceWriteOptions` mapping from the committing writer.
 * Pass an all-zero struct for native `WriteParams::default()`.
 *
 * `write.skip_auto_cleanup` is parsed but cannot affect this call because no
 * commit occurs. Pass the same setting through LanceCommitOptions when
 * publishing the returned transaction. `write.auto_cleanup` still participates
 * in a CREATE transaction's native config upsert.
 *
 * `transaction_properties` is a NULL-terminated key-value array. NULL means
 * native None. A non-NULL pointer, including one whose first entry is NULL,
 * means native Some and distinguishes an explicitly empty map. The selected
 * core's CommitBuilder has a separate transaction-properties setter, but its
 * execute/execute_batch path does not apply that field; properties for this
 * delivery are therefore carried by the transaction produced here.
 */
typedef struct LanceUncommittedWriteOptions {
    LanceWriteOptions write;
    const char* const* transaction_properties;
} LanceUncommittedWriteOptions;

/** Tri-state mapping for the native overall commit timeout. */
typedef enum {
    /** Keep the native default (30 minutes). */
    LANCE_COMMIT_TIMEOUT_DEFAULT  = 0,
    /** Pass None and disable the overall timeout. */
    LANCE_COMMIT_TIMEOUT_DISABLED = 1,
    /** Use the explicit seconds/nanoseconds value, including zero. */
    LANCE_COMMIT_TIMEOUT_EXPLICIT = 2,
} LanceCommitTimeoutMode;

/**
 * Native CommitBuilder settings for transaction publication.
 *
 * Zero-initialize this struct to preserve native defaults. Presence flags keep
 * omitted settings distinct from explicit false, zero retries, zero retry
 * budget and zero overall timeout.
 *
 * The native default retry count is 20. An explicit `max_retries = 0` is
 * forwarded and selects native strict-overwrite behavior for an Overwrite
 * transaction. The native retry backoff budget defaults to 30 seconds; an
 * explicit zero is valid. The overall timeout defaults to 30 minutes; an
 * explicit zero duration is rejected by the native execute path.
 *
 * `data_storage_version` uses the same parser as LanceWriteParams and maps to
 * CommitBuilder::with_storage_format. NULL keeps the native omitted value.
 */
typedef struct LanceCommitOptions {
    bool has_use_stable_row_ids;
    bool use_stable_row_ids;
    bool has_enable_v2_manifest_paths;
    bool enable_v2_manifest_paths;
    bool has_max_retries;
    uint32_t max_retries;
    bool has_retry_timeout;
    uint64_t retry_timeout_seconds;
    uint32_t retry_timeout_nanos;
    int32_t timeout_mode;
    uint64_t timeout_seconds;
    uint32_t timeout_nanos;
    bool detached;
    bool skip_auto_cleanup;
    const char* data_storage_version;
} LanceCommitOptions;

/**
 * Options for one-shot native transaction construction.
 *
 * `uuid == NULL` lets Lance generate a v4 UUID. A non-NULL value, including an
 * empty string, is passed through unchanged. `tag == NULL` maps to native
 * `None`; a non-NULL empty string maps to `Some("")`.
 *
 * `has_transaction_properties == false` maps to native `None`.
 * `has_transaction_properties == true` maps to `Some(map)`, including
 * `Some(empty)` when `property_count == 0`. If `property_count > 0`,
 * `property_keys` and `property_values` must contain that many non-NULL UTF-8
 * strings. Keys and values are separate parallel arrays, so keys may contain
 * any character, including `=`. Duplicate keys follow native last-write-wins
 * map behavior.
 */
typedef struct LanceTransactionOptions {
    const char* uuid;
    const char* tag;
    bool has_transaction_properties;
    const char* const* property_keys;
    const char* const* property_values;
    size_t property_count;
} LanceTransactionOptions;

/**
 * Write an Arrow stream as uncommitted Lance data files and return the owned
 * native transaction that would publish it.
 *
 * Maps to InsertBuilder::execute_uncommitted_stream. The stream is consumed on
 * every return path except a NULL stream pointer. `schema` is borrowed; the
 * caller releases it after this call. The stream schema must match.
 *
 * `options` may be NULL for native writer defaults. `storage_opts` uses the
 * normal NULL-terminated key/value format. `session` may be NULL; a non-NULL
 * session is retained by native derived values. Write mode, dataset existence,
 * schema compatibility and create/append/overwrite normalization follow the
 * selected core.
 *
 * On success `*out_transaction` receives an independently owned handle. On
 * error it remains unchanged. Data files already written by a failed call
 * remain uncommitted; this API performs no rollback or cleanup policy.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_dataset_write_uncommitted_stream(
    const char* uri,
    const struct ArrowSchema* schema,
    struct ArrowArrayStream* stream,
    int32_t mode,
    const LanceUncommittedWriteOptions* options,
    const char* const* storage_opts,
    const LanceSession* session,
    LanceTransaction** out_transaction
);

/** Close a transaction handle. NULL-safe. Close exactly once. */
void lance_transaction_close(LanceTransaction* transaction);

/** Close an operation handle. NULL-safe. Close exactly once. */
void lance_operation_close(LanceOperation* operation);

/** Owned native `DataReplacementGroup` snapshot. */
typedef struct LanceDataReplacementGroup LanceDataReplacementGroup;

/** Close a data replacement group. NULL-safe. Close exactly once. */
void lance_data_replacement_group_close(LanceDataReplacementGroup* group);

/**
 * Construct a native data replacement group.
 *
 * `new_file` is borrowed and its complete native metadata is cloned. The
 * fragment ID is copied unchanged. No binding-side ordering, uniqueness or
 * coverage policy is added. Returns NULL on error.
 */
LanceDataReplacementGroup* lance_data_replacement_group_new(
    uint64_t fragment_id,
    const LanceDataFile* new_file
);

/**
 * Clone a data replacement group into an independent snapshot.
 * Returns NULL on error; close the result exactly once.
 */
LanceDataReplacementGroup* lance_data_replacement_group_clone(
    const LanceDataReplacementGroup* group
);

/**
 * Read the native fragment ID. Returns 0 on error; check
 * `lance_last_error_code()`. Fragment ID 0 is a valid native value.
 */
uint64_t lance_data_replacement_group_fragment_id(
    const LanceDataReplacementGroup* group
);

/**
 * Clone the data file owned by a data replacement group. The output is written
 * only on success and must be closed independently.
 * Returns 0 on success, -1 on error.
 */
int32_t lance_data_replacement_group_file(
    const LanceDataReplacementGroup* group,
    LanceDataFile** out_file
);

/**
 * Construct an owned native DataReplacement operation.
 *
 * Group handles are borrowed and copied in input order. A NULL array is
 * accepted only with a zero count. Empty replacement lists are retained
 * because the native operation permits them. No sorting, deduplication,
 * non-empty, field-ID or coverage policy is added. Returns NULL on error.
 */
LanceOperation* lance_operation_data_replacement_new(
    const LanceDataReplacementGroup* const* replacements,
    size_t replacement_count
);

/**
 * Read the native transaction read version.
 * Returns 0 on success, -1 on error. Zero is valid for an uncommitted create.
 */
int32_t lance_transaction_read_version(
    const LanceTransaction* transaction,
    uint64_t* out_read_version
);

/**
 * Construct an owned native Delete operation.
 *
 * `updated_fragments` is an array of borrowed fragment handles whose complete
 * native metadata is copied. Each pointer must be non-NULL when the count is
 * non-zero. `deleted_fragment_ids` is copied. `predicate` must be non-NULL
 * UTF-8 and is copied; an empty predicate is accepted.
 *
 * Native constructors and commit own fragment existence, uniqueness, overlap
 * and predicate validation. This function adds no binding-side policy.
 * Returns an owned operation, or NULL on error.
 */
LanceOperation* lance_operation_delete_new(
    const LanceFragment* const* updated_fragments,
    size_t updated_fragment_count,
    const uint64_t* deleted_fragment_ids,
    size_t deleted_fragment_count,
    const char* predicate
);

/**
 * Construct an owned native Append operation.
 *
 * `fragments` is an array of borrowed fragment handles; each handle is required
 * when the count is non-zero and its complete native metadata, including
 * pre-assigned IDs, row ID metadata and data files, is copied. A NULL array is
 * valid only with a zero count.
 *
 * Native commit validation owns schema coverage, fragment ID assignment and
 * row-ID assignment. Explicit fields and non-zero reserved IDs pass through
 * unchanged; this constructor adds no binding-side policy. Returns an owned
 * operation, or NULL on error.
 */
LanceOperation* lance_operation_append_new(
    const LanceFragment* const* fragments,
    size_t fragment_count
);

/**
 * Construct an owned native Restore operation.
 *
 * `version` passes through unchanged. Native commit validation owns whether the
 * version exists and is restorable. Returns an owned operation, or NULL on
 * error.
 */
LanceOperation* lance_operation_restore_new(uint64_t version);

/**
 * Construct an owned native ReserveFragments operation.
 *
 * `num_fragments == 0` is a native no-op version; larger values advance the
 * manifest maximum fragment ID for later native allocation. Returns an owned
 * operation, or NULL on error.
 */
LanceOperation* lance_operation_reserve_fragments_new(uint32_t num_fragments);

/**
 * Construct an owned native UpdateBases operation.
 *
 * Each entry is parsed into native `BasePath` storage owned by the returned
 * operation. A NULL array is valid only with a zero count; every non-NULL entry
 * requires a non-NULL UTF-8 path, while `name == NULL` means native `None`.
 *
 * Native commit validation owns path/name conflict detection and zero-ID
 * allocation. This constructor adds no binding-side policy. Returns an owned
 * operation, or NULL on error.
 */
LanceOperation* lance_operation_update_bases_new(
    const LanceBasePath* new_bases,
    size_t new_base_count
);

/**
 * Construct an owned native CreateIndex operation.
 *
 * `new_indices` and `removed_indices` are arrays of borrowed metadata handles
 * whose complete native `IndexMetadata` values are copied. A NULL array is
 * valid only when its count is zero; a non-NULL array with a zero count is
 * ignored. Each non-NULL entry is required when the count is non-zero.
 *
 * Native `Operation::CreateIndex` owns index replacement, name handling and
 * coverage validation. This constructor does not add binding-side non-empty,
 * sorting, de-duplication, name or coverage checks. Returns an owned operation,
 * or NULL on error.
 */
LanceOperation* lance_operation_create_index_new(
    const LanceIndexSegmentMetadata* const* new_indices,
    size_t new_index_count,
    const LanceIndexSegmentMetadata* const* removed_indices,
    size_t removed_index_count
);

/**
 * Construct an owned native transaction from an operation.
 *
 * The operation is cloned and remains usable. `options == NULL` selects a
 * generated UUID, no tag and no transaction properties. A non-NULL options
 * value applies its documented UUID/tag/property mapping. Native
 * `TransactionBuilder` construction and later commit validation remain
 * authoritative. Returns an owned transaction, or NULL on error.
 */
LanceTransaction* lance_transaction_new(
    uint64_t read_version,
    const LanceOperation* operation,
    const LanceTransactionOptions* options
);

/**
 * Return a newly allocated copy of the native transaction UUID.
 * Caller frees with lance_free_string(). Returns NULL on error.
 */
const char* lance_transaction_uuid(const LanceTransaction* transaction);

/**
 * Copy the optional native transaction tag.
 *
 * Returns 1 and a newly allocated string through `out_tag` when present, 0 and
 * NULL when absent, or -1 on error. The caller frees a present string with
 * lance_free_string(). Absent and empty are distinct native Option values,
 * though native protobuf decoding normalizes an encoded empty tag to absent.
 */
int32_t lance_transaction_tag(
    const LanceTransaction* transaction,
    const char** out_tag
);

/**
 * Report whether native transaction properties are Some, including Some(empty).
 * An absent map is distinct from an empty map.
 * Returns 0 on success, -1 on error.
 */
int32_t lance_transaction_has_properties(
    const LanceTransaction* transaction,
    bool* out_has_properties
);

/**
 * Read the transaction property count. The count is 0 for both None and
 * Some(empty); use lance_transaction_has_properties() to distinguish.
 * Returns 0 on success, -1 on error.
 */
int32_t lance_transaction_property_count(
    const LanceTransaction* transaction,
    uint64_t* out_count
);

/**
 * Return a newly allocated property key at `index`.
 * Map order is unspecified but stable for the handle. Caller frees with
 * lance_free_string(). Returns NULL on error, including an out-of-range index.
 */
const char* lance_transaction_property_key_at(
    const LanceTransaction* transaction,
    size_t index
);

/**
 * Return a newly allocated property value at `index`.
 * Map order is unspecified but stable for the handle. Caller frees with
 * lance_free_string(). Returns NULL on error, including an out-of-range index.
 */
const char* lance_transaction_property_value_at(
    const LanceTransaction* transaction,
    size_t index
);

/**
 * Clone the transaction operation into an independent owned handle.
 * Returns 0 on success, -1 on error; the output slot is success-only.
 */
int32_t lance_transaction_operation(
    const LanceTransaction* transaction,
    LanceOperation** out_operation
);

/**
 * Read the operation variant.
 * Returns 0 on success, -1 on error; the output slot is success-only.
 */
int32_t lance_operation_kind(
    const LanceOperation* operation,
    LanceOperationKind* out_kind
);

/**
 * Read the fragment count for an Append or Overwrite operation.
 * Returns 0 on success, -1 on error. Other variants return
 * LANCE_ERR_NOT_SUPPORTED; use the typed operation getters for other variants.
 */
int32_t lance_operation_fragment_count(
    const LanceOperation* operation,
    uint64_t* out_count
);

/**
 * Read the number of fragments in an Append operation.
 * Returns 0 on success, -1 on error; other variants return
 * LANCE_ERR_NOT_SUPPORTED.
 */
int32_t lance_operation_append_fragment_count(
    const LanceOperation* operation,
    uint64_t* out_count
);

/**
 * Clone one Append fragment as an independently owned detached handle.
 *
 * The clone owns the complete native `Fragment` metadata and remains valid
 * after the operation and every source dataset are closed. It supports metadata
 * inspection and operation construction but rejects dataset-dependent reads.
 * Returns 0 on success, -1 on error; the output slot is unchanged on error.
 */
int32_t lance_operation_append_fragment_at(
    const LanceOperation* operation,
    size_t index,
    LanceFragment** out_fragment
);

/**
 * Read the target version of a Restore operation.
 * Returns 0 on success, -1 on error; other variants return
 * LANCE_ERR_NOT_SUPPORTED.
 */
int32_t lance_operation_restore_version(
    const LanceOperation* operation,
    uint64_t* out_version
);

/**
 * Read the reserved fragment count of a ReserveFragments operation.
 * Returns 0 on success, -1 on error; other variants return
 * LANCE_ERR_NOT_SUPPORTED.
 */
int32_t lance_operation_reserve_fragments_count(
    const LanceOperation* operation,
    uint32_t* out_count
);

/**
 * Read the number of new base paths in an UpdateBases operation.
 * Returns 0 on success, -1 on error; other variants return
 * LANCE_ERR_NOT_SUPPORTED.
 */
int32_t lance_operation_update_bases_count(
    const LanceOperation* operation,
    uint64_t* out_count
);

/**
 * Read one borrowed new base path from an UpdateBases operation.
 *
 * `path` and `name` point into the operation handle and remain valid until it
 * is closed. `name == NULL` means native `None`; an empty string is an
 * explicit empty name. Returns 0 on success, -1 on error; the output slot is
 * unchanged on error.
 */
int32_t lance_operation_update_bases_at(
    const LanceOperation* operation,
    size_t index,
    LanceBasePathView* out_base
);

/**
 * Read the number of new metadata entries in a CreateIndex operation.
 * Returns 0 on success, -1 on error (including a different operation kind).
 */
int32_t lance_operation_create_index_new_count(
    const LanceOperation* operation,
    uint64_t* out_count
);

/**
 * Clone one new metadata entry from a CreateIndex operation.
 *
 * On success `*out_metadata` receives an independently owned handle that
 * remains valid after the operation is closed. Returns 0 on success, -1 on
 * error; the output slot is unchanged on error.
 */
int32_t lance_operation_create_index_new_at(
    const LanceOperation* operation,
    size_t index,
    LanceIndexSegmentMetadata** out_metadata
);

/**
 * Read the number of removed metadata entries in a CreateIndex operation.
 * Returns 0 on success, -1 on error (including a different operation kind).
 */
int32_t lance_operation_create_index_removed_count(
    const LanceOperation* operation,
    uint64_t* out_count
);

/**
 * Clone one removed metadata entry from a CreateIndex operation.
 *
 * On success `*out_metadata` receives an independently owned handle that
 * remains valid after the operation is closed. Returns 0 on success, -1 on
 * error; the output slot is unchanged on error.
 */
int32_t lance_operation_create_index_removed_at(
    const LanceOperation* operation,
    size_t index,
    LanceIndexSegmentMetadata** out_metadata
);

/**
 * Read the number of updated fragments in a Delete operation.
 * Returns 0 on success, -1 on error (including wrong operation kind).
 */
int32_t lance_operation_delete_updated_fragment_count(
    const LanceOperation* operation,
    uint64_t* out_count
);

/**
 * Serialize one updated Delete fragment's native metadata as JSON.
 *
 * The JSON is the same native `Fragment` serialization returned by
 * `lance_fragment_metadata_json()`. Caller frees with `lance_free_string()`.
 * Returns NULL on error, including a wrong operation kind or bad index.
 */
const char* lance_operation_delete_updated_fragment_metadata_json_at(
    const LanceOperation* operation,
    size_t index
);

/**
 * Read the number of fully deleted fragment IDs in a Delete operation.
 * Returns 0 on success, -1 on error (including wrong operation kind).
 */
int32_t lance_operation_delete_deleted_fragment_count(
    const LanceOperation* operation,
    uint64_t* out_count
);

/**
 * Read one fully deleted fragment ID from a Delete operation.
 * Returns 0 on success, -1 on error. `out_id` is unchanged on error.
 */
int32_t lance_operation_delete_deleted_fragment_id_at(
    const LanceOperation* operation,
    size_t index,
    uint64_t* out_id
);

/**
 * Return a newly allocated copy of a Delete operation's predicate.
 * Caller frees with `lance_free_string()`. Returns NULL on error.
 */
const char* lance_operation_delete_predicate(
    const LanceOperation* operation
);

/**
 * Read the number of data replacement groups. A non-DataReplacement operation
 * returns LANCE_ERR_NOT_SUPPORTED. Returns 0 on success, -1 on error.
 */
int32_t lance_operation_data_replacement_count(
    const LanceOperation* operation,
    uint64_t* out_count
);

/**
 * Clone one data replacement group. A wrong operation variant returns
 * LANCE_ERR_NOT_SUPPORTED; an out-of-range index returns
 * LANCE_ERR_INVALID_ARGUMENT. The output is written only on success and must
 * be closed independently. Returns 0 on success, -1 on error.
 */
int32_t lance_operation_data_replacement_at(
    const LanceOperation* operation,
    size_t index,
    LanceDataReplacementGroup** out_group
);

/**
 * Export an Overwrite operation's schema as Arrow C Data Interface.
 * The caller owns and releases the output ArrowSchema exactly once.
 * Append has no schema; other variants return LANCE_ERR_NOT_SUPPORTED.
 * Returns 0 on success, -1 on error.
 */
int32_t lance_operation_schema(
    const LanceOperation* operation,
    struct ArrowSchema* out
);

/**
 * Serialize a transaction through the selected core's
 * TryFrom<&Transaction> for pb.Transaction conversion.
 *
 * On success, `*out_bytes` receives a malloc-compatible allocation and
 * `*out_len` its length; free with lance_free_bytes(). Both output slots are
 * unchanged on error.
 *
 * Conversion boundaries are the native contract: Operation::Unknown cannot be
 * encoded and returns LANCE_ERR_NOT_SUPPORTED. Empty tags/property maps
 * normalize to absent on decode. The selected core does not carry Overwrite
 * schema metadata through its separate protobuf field. Consumers must treat
 * this as native transport, not a lossless object serialization.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_transaction_to_proto(
    const LanceTransaction* transaction,
    uint8_t** out_bytes,
    size_t* out_len
);

/**
 * Parse one complete protobuf Transaction through the selected core's native
 * conversion.
 *
 * Empty input decodes to the native default transaction with an Unknown
 * operation. NULL bytes with a non-zero length is invalid. The length must
 * fit a Rust slice (at most PTRDIFF_MAX). Unknown protobuf oneof variants become
 * Operation::Unknown: they can be inspected but cannot be serialized again or
 * committed. The returned handle must be closed exactly once. Returns NULL on
 * error.
 */
LanceTransaction* lance_transaction_from_proto(
    const uint8_t* bytes,
    size_t len
);

/**
 * Read the current snapshot's transaction.
 *
 * Returns 0 on success. On success the output is either an independently owned
 * transaction handle or NULL when the manifest has no transaction file. The
 * output slot is unchanged on error. The transaction remains valid after the
 * source dataset handle is closed.
 */
int32_t lance_dataset_read_transaction(
    const LanceDataset* dataset,
    LanceTransaction** out_transaction
);

/**
 * Read a specific version's transaction.
 *
 * `version` is resolved through the native dataset branch and storage rules.
 * Missing versions and invalid IDs follow native errors. Returns 0 on success;
 * success writes a transaction handle or NULL when that version has no
 * transaction file. The output slot is unchanged on error.
 */
int32_t lance_dataset_read_transaction_by_version(
    const LanceDataset* dataset,
    uint64_t version,
    LanceTransaction** out_transaction
);

/**
 * Publish one native transaction through CommitBuilder::execute.
 *
 * `transaction` is borrowed and cloned by the native builder; closing it after
 * a successful or failed call is the caller's choice and never invalidates a
 * returned dataset. `options` may be NULL for native commit defaults.
 * `storage_opts` and `session` configure URI resolution and caches.
 *
 * On success `*out_dataset` receives an independently owned dataset at the
 * committed version and is written only on success. Commit eligibility,
 * conflict resolution, retries, detached versions, cleanup and normalization
 * are native.
 *
 * A failed publish performs no binding-side rollback. Uncommitted data files
 * may remain, and an I/O failure can leave the resulting version uncertain.
 * Reopen the URI to establish the actual committed state before retrying.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_transaction_commit(
    const char* uri,
    const LanceTransaction* transaction,
    const LanceCommitOptions* options,
    const char* const* storage_opts,
    const LanceSession* session,
    LanceDataset** out_dataset
);

/**
 * Publish append transactions through CommitBuilder::execute_batch.
 *
 * Each input transaction is borrowed and cloned. Native batch rules determine
 * eligibility, merge order, conflicts, retries and the merged read version.
 * Non-append batches return LANCE_ERR_NOT_SUPPORTED; an empty batch returns the
 * native invalid-input error. Both output pointers are required.
 *
 * On success the actual merged transaction and an independently owned dataset
 * are written to the output slots. Both slots are unchanged on error.
 *
 * @param transactions Array of `transaction_count` non-NULL handles.
 * @return 0 on success, -1 on error.
 */
int32_t lance_transaction_commit_batch(
    const char* uri,
    const LanceTransaction* const* transactions,
    size_t transaction_count,
    const LanceCommitOptions* options,
    const char* const* storage_opts,
    const LanceSession* session,
    LanceDataset** out_dataset,
    LanceTransaction** out_merged_transaction
);

/* ─── Storage options ─────────────────────────────────────────────────────── */

typedef struct LanceStorageOptionsAccessor LanceStorageOptionsAccessor;
typedef struct LanceStorageOptionsProvider LanceStorageOptionsProvider;
typedef struct LanceStorageOptionsSnapshot LanceStorageOptionsSnapshot;

/**
 * Borrowed static storage options.
 *
 * A NULL `LanceStorageOptionsInput*` passed to accessor construction means
 * native None. A non-NULL input, including one with `count == 0`, means native
 * Some, so an empty map is distinct from no options. Duplicate keys follow the
 * native map's last-write-wins behavior.
 */
typedef struct LanceStorageOptionsInput {
    const char* const* keys;
    const char* const* values;
    size_t count;
} LanceStorageOptionsInput;

/**
 * Result of one provider fetch.
 *
 * `has_options == 0` means native None. Any nonzero value means native Some;
 * `count == 0` then means Some(empty). Key and value arrays must contain
 * exactly `count` entries.
 *
 * The library copies the complete map and the callback error message before
 * calling `release_snapshot`. `release_snapshot`, when non-NULL, is called
 * exactly once on every path after the result struct was initialized,
 * including malformed partial results and callback errors. Without a release
 * callback the map storage is borrowed for the callback only.
 */
typedef struct LanceStorageOptionsFetchResult {
    int32_t has_options;
    const char* const* keys;
    const char* const* values;
    size_t count;
    void* snapshot_ctx;
    void (*release_snapshot)(void* snapshot_ctx);
} LanceStorageOptionsFetchResult;

/**
 * Fetch storage options.
 *
 * Return 0 on success. Return nonzero on failure and initialize `error` with a
 * typed LanceCallbackError. A callback must not report failure through
 * lance_last_error_* because it may run on a native worker thread.
 */
typedef int32_t (*LanceStorageOptionsFetchFn)(
    void* provider_ctx,
    LanceStorageOptionsFetchResult* out,
    LanceCallbackError* error
);

/** Release the provider context exactly once. */
typedef void (*LanceStorageOptionsProviderReleaseFn)(void* provider_ctx);

/**
 * Foreign storage-options provider declaration.
 *
 * `fetch` is required. `force_fetch` is optional; omission follows the native
 * trait default and calls `fetch`. `provider_id` is copied and must be
 * non-empty. It preserves native equality and object-store registry cache
 * identity, so equal IDs may share a native store.
 *
 * Ownership of `provider_ctx` and `release` transfers only when
 * lance_storage_options_provider_new succeeds. The library retains the context
 * through every native Arc owner, including accessor and registry cache keys,
 * and calls `release(provider_ctx)` exactly once when the final owner drops.
 */
typedef struct LanceStorageOptionsProviderSpec {
    LanceStorageOptionsFetchFn fetch;
    LanceStorageOptionsFetchFn force_fetch;
    LanceStorageOptionsProviderReleaseFn release;
    void* provider_ctx;
    const char* provider_id;
} LanceStorageOptionsProviderSpec;

LanceStorageOptionsProvider* lance_storage_options_provider_new(
    const LanceStorageOptionsProviderSpec* spec
);
void lance_storage_options_provider_close(LanceStorageOptionsProvider* provider);

LanceStorageOptionsAccessor* lance_storage_options_accessor_new(
    const LanceStorageOptionsInput* initial,
    const LanceStorageOptionsProvider* provider
);
void lance_storage_options_accessor_close(LanceStorageOptionsAccessor* accessor);

/**
 * Resolve and copy the accessor's current options.
 *
 * Native StorageOptionsAccessor owns caching and expiration policy. The
 * binding keeps no second refresh cache. A NULL accessor or provider error
 * returns NULL and sets the thread-local error.
 */
LanceStorageOptionsSnapshot* lance_storage_options_accessor_snapshot(
    const LanceStorageOptionsAccessor* accessor
);

/** 1 = Some, 0 = None, -1 = invalid handle with thread-local error. */
int32_t lance_storage_options_snapshot_has_options(
    const LanceStorageOptionsSnapshot* snapshot
);
uint64_t lance_storage_options_snapshot_count(
    const LanceStorageOptionsSnapshot* snapshot
);
const char* lance_storage_options_snapshot_key_at(
    const LanceStorageOptionsSnapshot* snapshot,
    size_t index
);
const char* lance_storage_options_snapshot_value_at(
    const LanceStorageOptionsSnapshot* snapshot,
    size_t index
);
void lance_storage_options_snapshot_close(
    LanceStorageOptionsSnapshot* snapshot
);

/* ─── Raw Lance-file session ──────────────────────────────────────────────── */

typedef struct LanceFileSession LanceFileSession;
typedef struct LanceFileReader LanceFileReader;
typedef struct LanceFileWriter LanceFileWriter;
typedef struct LancePackedBlobWriter LancePackedBlobWriter;
typedef struct LanceDedicatedBlobWriter LanceDedicatedBlobWriter;
typedef struct LanceFileMetadata LanceFileMetadata;
typedef struct LanceFileStatistics LanceFileStatistics;
typedef struct LancePathList LancePathList;
typedef struct LanceDelimitedList LanceDelimitedList;
typedef struct LanceByteBuffer LanceByteBuffer;

typedef enum {
    LANCE_FILE_CACHE_NONE = 0,
    LANCE_FILE_CACHE_SESSION = 1
} LanceFileCacheMode;

/**
 * Reader options.
 *
 * `has_columns` nonzero forwards exactly `column_count` names; count zero is
 * an explicit empty projection and is rejected by native validation. A NULL
 * options pointer uses native defaults and no projection.
 *
 * SESSION cache requires a shared LanceSession and shares its capacity through
 * a reader-local namespace. Entries can be reused within that reader. Native
 * file cache keys lack backing-store identity and content versions. Files must
 * remain immutable while their reader or streams are active. NONE uses native
 * LanceCache::no_cache and is the default.
 *
 * `has_*` fields distinguish None from explicit zero/false. Boolean payloads
 * must be 0 or 1 when set.
 */
typedef struct LanceFileReaderOptions {
    int32_t has_columns;
    const char* const* columns;
    size_t column_count;
    int32_t cache_mode;
    int32_t has_read_chunk_size;
    uint64_t read_chunk_size;
    int32_t has_batch_size_bytes;
    uint64_t batch_size_bytes;
    int32_t has_cache_repetition_index;
    int32_t cache_repetition_index;
    int32_t has_validate_on_decode;
    int32_t validate_on_decode;
    int32_t has_inline_scheduling;
    int32_t inline_scheduling;
} LanceFileReaderOptions;

/**
 * Writer options. `has_*` fields distinguish None from explicit zero/false.
 * `version == NULL` uses the native default; otherwise native parsing and
 * unsupported-version errors are preserved.
 */
typedef struct LanceFileWriterOptions {
    int32_t has_data_cache_bytes;
    uint64_t data_cache_bytes;
    int32_t has_max_page_bytes;
    uint64_t max_page_bytes;
    int32_t has_keep_original_array;
    int32_t keep_original_array;
    const char* version;
} LanceFileWriterOptions;

typedef struct LanceFileWriteSummary {
    uint64_t num_rows;
    uint64_t size_bytes;
} LanceFileWriteSummary;

typedef enum {
    LANCE_SCHEDULER_LITE_AUTO = 0,
    LANCE_SCHEDULER_LITE_STANDARD = 1,
    LANCE_SCHEDULER_LITE_LITE = 2
} LanceSchedulerLiteMode;

/**
 * Native scheduler configuration.
 *
 * `use_lite_scheduler` is a LanceSchedulerLiteMode. AUTO delegates to
 * SchedulerConfig::new and its environment policy, STANDARD maps to
 * Some(false), and LITE maps to Some(true).
 */
typedef struct LanceSchedulerConfig {
    uint64_t io_buffer_size_bytes;
    int32_t use_lite_scheduler;
} LanceSchedulerConfig;

typedef struct LanceScanStats {
    uint64_t iops;
    uint64_t requests;
    uint64_t bytes_read;
} LanceScanStats;

typedef struct LanceObjectStoreRegistryStats {
    uint64_t hits;
    uint64_t misses;
    uint64_t active_stores;
} LanceObjectStoreRegistryStats;

typedef struct LanceObjectStoreIoStats {
    uint64_t read_iops;
    uint64_t read_bytes;
    uint64_t write_iops;
    uint64_t written_bytes;
} LanceObjectStoreIoStats;

typedef struct LanceObjectStoreInfo {
    uint64_t block_size;
    uint64_t max_iop_size;
    uint64_t io_parallelism;
    int32_t is_local;
    int32_t is_cloud;
    int32_t prefers_lite_scheduler;
} LanceObjectStoreInfo;

/**
 * Create a raw-file session over `base_uri`.
 *
 * `storage_options` may be NULL. `session` may be NULL; otherwise the session's
 * existing object-store registry is shared. `scheduler_config` may be NULL,
 * which uses native SchedulerConfig::max_bandwidth for the resolved store.
 */
LanceFileSession* lance_file_session_new(
    const char* base_uri,
    const LanceStorageOptionsAccessor* storage_options,
    const LanceSession* session,
    const LanceSchedulerConfig* scheduler_config
);
void lance_file_session_close(LanceFileSession* session);

/** Release the returned string with lance_free_string(). */
const char* lance_file_session_base_path(const LanceFileSession* session);

LanceFileReader* lance_file_session_open_reader(
    const LanceFileSession* session,
    const char* relative_path,
    const LanceFileReaderOptions* options
);
LanceFileWriter* lance_file_session_open_writer(
    const LanceFileSession* session,
    const char* relative_path,
    const struct ArrowSchema* schema,
    const LanceFileWriterOptions* options
);

/**
 * Open a native packed blob writer through an existing raw-file session.
 *
 * `data_file_path` is relative to the session base. Native validates the
 * `.lance` suffix, derives the sidecar path, and rejects blob ID zero. The
 * writer retains the session's native object store; closing the session,
 * accessor, or provider does not invalidate the writer.
 *
 * Callers choose unique data-file names and blob IDs. No collision check or
 * ID allocation is added by the binding. Returns NULL on error.
 */
LancePackedBlobWriter* lance_file_session_open_packed_blob_writer(
    const LanceFileSession* session,
    const char* data_file_path,
    uint32_t blob_id
);

/**
 * Open a native dedicated blob writer through an existing raw-file session.
 *
 * Path derivation and ownership match the packed constructor. Returns NULL on
 * error.
 */
LanceDedicatedBlobWriter* lance_file_session_open_dedicated_blob_writer(
    const LanceFileSession* session,
    const char* data_file_path,
    uint32_t blob_id
);

/**
 * Return the native packed writer blob ID.
 *
 * Returns 0 and sets the thread-local error for a NULL, finished, or otherwise
 * invalid handle. Blob ID zero is reserved, so 0 is never a success value.
 */
uint32_t lance_packed_blob_writer_blob_id(
    const LancePackedBlobWriter* writer
);

/**
 * Return a library-allocated copy of the native packed sidecar path.
 *
 * Release the returned string with lance_free_string(). Returns NULL on error.
 */
const char* lance_packed_blob_writer_path(
    const LancePackedBlobWriter* writer
);

/**
 * Append one payload to the packed sidecar.
 *
 * `data == NULL && len == 0` is a valid empty payload. A NULL data pointer
 * with non-zero len is rejected before native invocation. Foreign bytes are
 * copied by the native writer before this call returns.
 *
 * Native packed write failures can leave the writer without an active upload.
 * The binding forwards that state without adding a second poison flag.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_packed_blob_writer_write_blob(
    LancePackedBlobWriter* writer,
    const uint8_t* data,
    size_t len
);

/**
 * Append one packed row per payload array element.
 *
 * Ownership of a non-NULL `payloads` transfers on every return path, including
 * writer, schema, type, data, and native-write failures. The source slot is
 * left in canonical released state. `schema` is borrowed and describes one
 * Binary, LargeBinary, BinaryView, or FixedSizeBinary field.
 *
 * Arrow null rows become native Null descriptors. Empty non-null rows become
 * zero-size Packed descriptors. Slices, row order, and repeated calls are
 * preserved. A C ArrowArray represents one chunk; pass chunk arrays in order.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_packed_blob_writer_write_blobs(
    LancePackedBlobWriter* writer,
    struct ArrowArray* payloads,
    const struct ArrowSchema* schema
);

/**
 * Consume the native packed writer and export its prepared descriptor parts.
 *
 * Preflight requires an active writer, a non-NULL UTF-8 `field_name` (empty is
 * allowed), and non-NULL output pointers. Preflight failure leaves the writer
 * active. Once native finish starts, the writer is consumed on every return.
 *
 * On error both outputs are unchanged. On success `out_array` is an owned
 * ArrowArray and `out_schema` is the complete native prepared field. Release
 * each non-NULL output exactly once. The C writer allocation remains valid
 * until close; later calls report its finished state.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_packed_blob_writer_finish_array(
    LancePackedBlobWriter* writer,
    const char* field_name,
    struct ArrowArray* out_array,
    struct ArrowSchema* out_schema
);

/**
 * Free a packed writer allocation. NULL is a no-op. Close never calls native
 * finish; an unfinished writer is dropped with native cleanup semantics.
 */
void lance_packed_blob_writer_close(
    LancePackedBlobWriter* writer
);

/**
 * Return the native dedicated writer blob ID.
 *
 * Returns 0 and sets the thread-local error for a NULL, finished, or otherwise
 * invalid handle.
 */
uint32_t lance_dedicated_blob_writer_blob_id(
    const LanceDedicatedBlobWriter* writer
);

/**
 * Return a library-allocated copy of the native dedicated sidecar path.
 *
 * Release the returned string with lance_free_string(). Returns NULL on error.
 */
const char* lance_dedicated_blob_writer_path(
    const LanceDedicatedBlobWriter* writer
);

/**
 * Append bytes to the dedicated sidecar.
 *
 * Repeated writes concatenate. `data == NULL && len == 0` is valid; a NULL
 * data pointer with non-zero len is rejected before native invocation.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_dedicated_blob_writer_write(
    LanceDedicatedBlobWriter* writer,
    const uint8_t* data,
    size_t len
);

/**
 * Consume the native dedicated writer and export its prepared descriptor parts.
 *
 * The success result has exactly one Dedicated row covering the concatenated
 * payload, including size zero. Preflight, consumption, output ownership, and
 * error handling match the packed finish.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_dedicated_blob_writer_finish_array(
    LanceDedicatedBlobWriter* writer,
    const char* field_name,
    struct ArrowArray* out_array,
    struct ArrowSchema* out_schema
);

/**
 * Free a dedicated writer allocation. NULL is a no-op. Close never calls
 * native finish.
 */
void lance_dedicated_blob_writer_close(
    LanceDedicatedBlobWriter* writer
);

int32_t lance_file_session_contains(
    const LanceFileSession* session,
    const char* relative_path,
    int32_t* out_exists
);
LancePathList* lance_file_session_list(
    const LanceFileSession* session,
    const char* relative_path
);
LanceDelimitedList* lance_file_session_list_with_delimiter(
    const LanceFileSession* session,
    const char* relative_path
);
LanceByteBuffer* lance_file_session_read_range(
    const LanceFileSession* session,
    const char* relative_path,
    uint64_t offset,
    uint64_t length
);
int32_t lance_file_session_delete_file(
    const LanceFileSession* session,
    const char* relative_path
);
int32_t lance_file_session_upload_file(
    const LanceFileSession* session,
    const char* local_path,
    const char* relative_remote_path
);
/**
 * Download through the native local writer. It publishes the destination only
 * after the source stream completes, including when both paths alias one file.
 */
int32_t lance_file_session_download_file(
    const LanceFileSession* session,
    const char* relative_remote_path,
    const char* local_path
);
int32_t lance_file_session_get_scheduler_stats(
    const LanceFileSession* session,
    LanceScanStats* out_stats
);
int32_t lance_file_session_get_registry_stats(
    const LanceFileSession* session,
    LanceObjectStoreRegistryStats* out_stats
);
int32_t lance_file_session_get_store_info(
    const LanceFileSession* session,
    LanceObjectStoreInfo* out_info
);
int32_t lance_file_session_get_store_io_stats(
    const LanceFileSession* session,
    LanceObjectStoreIoStats* out_stats
);

/* ─── Raw Lance-file reader ───────────────────────────────────────────────── */

LanceFileReader* lance_file_reader_open(
    const char* uri_or_path,
    const LanceStorageOptionsAccessor* storage_options,
    const LanceFileReaderOptions* options,
    const LanceSession* session,
    const LanceSchedulerConfig* scheduler_config
);

int32_t lance_file_reader_num_rows(
    const LanceFileReader* reader,
    uint64_t* out_rows
);
int32_t lance_file_reader_get_metadata(
    const LanceFileReader* reader,
    LanceFileMetadata** out_metadata
);
int32_t lance_file_reader_get_statistics(
    const LanceFileReader* reader,
    LanceFileStatistics** out_statistics
);
int32_t lance_file_reader_read_global_buffer(
    const LanceFileReader* reader,
    uint32_t index,
    LanceByteBuffer** out_bytes
);
int32_t lance_file_reader_read_all(
    const LanceFileReader* reader,
    uint32_t batch_size,
    uint32_t batch_readahead,
    struct ArrowArrayStream* out
);
int32_t lance_file_reader_read_range(
    const LanceFileReader* reader,
    uint64_t offset,
    uint64_t num_rows,
    uint32_t batch_size,
    uint32_t batch_readahead,
    struct ArrowArrayStream* out
);
int32_t lance_file_reader_read_indices(
    const LanceFileReader* reader,
    const uint32_t* indices,
    size_t count,
    uint32_t batch_size,
    uint32_t batch_readahead,
    struct ArrowArrayStream* out
);
int32_t lance_file_reader_get_io_stats(
    const LanceFileReader* reader,
    LanceScanStats* out_stats
);
void lance_file_reader_close(LanceFileReader* reader);

/* ─── Raw Lance-file metadata snapshots ──────────────────────────────────── */

typedef enum {
    LANCE_FILE_FORMAT_V1 = 0,
    LANCE_FILE_FORMAT_V2_0 = 1,
    LANCE_FILE_FORMAT_V2_1 = 2,
    LANCE_FILE_FORMAT_V2_2 = 3,
    LANCE_FILE_FORMAT_V2_3 = 4
} LanceFileFormatVersion;

typedef struct LanceFileMetadataInfo {
    int32_t format_version;
    uint16_t major_version;
    uint16_t minor_version;
    uint64_t num_rows;
    uint64_t num_data_bytes;
    uint64_t num_column_metadata_bytes;
    uint64_t num_global_buffer_bytes;
    uint64_t num_footer_bytes;
    uint64_t file_size_bytes;
    size_t num_columns;
    size_t num_physical_columns;
    size_t num_global_buffers;
} LanceFileMetadataInfo;

typedef struct LanceBufferDescriptor {
    uint64_t position;
    uint64_t size;
} LanceBufferDescriptor;

typedef enum {
    LANCE_FILE_ENCODING_ABSENT = 0,
    LANCE_FILE_ENCODING_NONE = 1,
    LANCE_FILE_ENCODING_DIRECT = 2,
    LANCE_FILE_ENCODING_INDIRECT = 3
} LanceFileEncodingKind;

typedef struct LanceEncodingDescriptor {
    int32_t kind;
    uint64_t buffer_location;
    uint64_t buffer_length;
    const uint8_t* direct_bytes;
    size_t direct_bytes_len;
} LanceEncodingDescriptor;

typedef struct LancePageMetadata {
    const LanceBufferDescriptor* buffers;
    size_t buffer_count;
    uint64_t num_rows;
    uint64_t priority;
    LanceEncodingDescriptor encoding;
    const char* encoding_description;
} LancePageMetadata;

typedef struct LanceColumnMetadata {
    const LanceBufferDescriptor* column_buffers;
    size_t column_buffer_count;
    const LancePageMetadata* pages;
    size_t page_count;
    LanceEncodingDescriptor encoding;
} LanceColumnMetadata;

int32_t lance_file_metadata_get_info(
    const LanceFileMetadata* metadata,
    LanceFileMetadataInfo* out_info
);
int32_t lance_file_metadata_get_schema(
    const LanceFileMetadata* metadata,
    struct ArrowSchema* out_schema
);
int32_t lance_file_metadata_get_global_buffers(
    const LanceFileMetadata* metadata,
    const LanceBufferDescriptor** out_buffers,
    size_t* out_count
);
int32_t lance_file_metadata_get_columns(
    const LanceFileMetadata* metadata,
    const LanceColumnMetadata** out_columns,
    size_t* out_count
);
void lance_file_metadata_close(LanceFileMetadata* metadata);

typedef struct LanceColumnStatistics {
    size_t num_pages;
    uint64_t size_bytes;
} LanceColumnStatistics;

int32_t lance_file_statistics_get_columns(
    const LanceFileStatistics* statistics,
    const LanceColumnStatistics** out_columns,
    size_t* out_count
);
void lance_file_statistics_close(LanceFileStatistics* statistics);

/* ─── Raw Lance-file writer ───────────────────────────────────────────────── */

LanceFileWriter* lance_file_writer_open(
    const char* uri_or_path,
    const struct ArrowSchema* schema,
    const LanceFileWriterOptions* options,
    const LanceStorageOptionsAccessor* storage_options,
    const LanceSession* session
);

/** Write one borrowed LanceBatch. The writer may clone its Arrow arrays. */
int32_t lance_file_writer_write_batch(
    LanceFileWriter* writer,
    const LanceBatch* batch
);
int32_t lance_file_writer_add_schema_metadata(
    LanceFileWriter* writer,
    const char* key,
    const char* value
);
int32_t lance_file_writer_add_global_buffer(
    LanceFileWriter* writer,
    const uint8_t* data,
    size_t len,
    uint32_t* out_index
);

/**
 * Finish the file.
 *
 * Native mutable state and errors are forwarded directly. Repeated finish,
 * writes after finish or abort, and failures retain the selected core's
 * behavior; this binding does not add terminal-state rejection.
 */
int32_t lance_file_writer_finish(
    LanceFileWriter* writer,
    LanceFileWriteSummary* out_summary
);

/**
 * Call native FileWriter::abort.
 *
 * Native v2.x abort bodies are currently empty. Allocation close drops the
 * native writer, which owns remaining object-writer cleanup. This call does not
 * implicitly destroy the allocation, and repeated calls are forwarded.
 */
int32_t lance_file_writer_abort(LanceFileWriter* writer);

/**
 * Destroy the writer allocation.
 *
 * Native drop performs object-writer cleanup. This is not a successful
 * finalization and does not replace an explicit fallible finish.
 */
void lance_file_writer_close(LanceFileWriter* writer);

/* ─── Owned raw-file path and byte snapshots ──────────────────────────────── */

uint64_t lance_path_list_count(const LancePathList* list);
const char* lance_path_list_at(const LancePathList* list, size_t index);
void lance_path_list_close(LancePathList* list);

uint64_t lance_delimited_list_common_prefix_count(
    const LanceDelimitedList* list
);
const char* lance_delimited_list_common_prefix_at(
    const LanceDelimitedList* list,
    size_t index
);
uint64_t lance_delimited_list_object_count(
    const LanceDelimitedList* list
);
const char* lance_delimited_list_object_at(
    const LanceDelimitedList* list,
    size_t index
);
void lance_delimited_list_close(LanceDelimitedList* list);

const uint8_t* lance_byte_buffer_data(const LanceByteBuffer* buffer);
size_t lance_byte_buffer_len(const LanceByteBuffer* buffer);
void lance_byte_buffer_close(LanceByteBuffer* buffer);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LANCE_H */
