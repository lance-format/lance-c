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
 * LanceScanStatisticsCallback) are the reverse direction and are NOT covered
 * by this contract: their ABI is non-unwinding, so a callback that throws or
 * unwinds can abort the host process before the library can contain it.
 * Callbacks must return normally.
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
} LanceVectorIndexType;

typedef enum {
    LANCE_SCALAR_BTREE      = 1,
    LANCE_SCALAR_BITMAP     = 2,
    LANCE_SCALAR_LABEL_LIST = 3,
    LANCE_SCALAR_INVERTED   = 4,
} LanceScalarIndexType;

typedef enum {
    LANCE_METRIC_L2      = 0,
    LANCE_METRIC_COSINE  = 1,
    LANCE_METRIC_DOT     = 2,
    LANCE_METRIC_HAMMING = 3,
} LanceMetricType;

typedef enum {
    LANCE_DTYPE_FLOAT32 = 0,
    LANCE_DTYPE_FLOAT16 = 1,
    LANCE_DTYPE_FLOAT64 = 2,
    LANCE_DTYPE_UINT8   = 3,
    LANCE_DTYPE_INT8    = 4,
} LanceDataType;

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
typedef struct LanceVersions LanceVersions;
typedef struct LanceDataStatistics LanceDataStatistics;
typedef struct LanceIndexSegmentBuilder LanceIndexSegmentBuilder;
typedef struct LanceIndexSegmentMetadata LanceIndexSegmentMetadata;

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
 * Return the latest version ID (I/O), or 0 on error (check
 * lance_last_error_code()).
 */
uint64_t lance_dataset_latest_version(const LanceDataset* dataset);

/* ─── Version history ─── */

/**
 * Snapshot the dataset's version history. Caller frees the returned handle
 * with lance_versions_close().
 * @return handle on success, or NULL on error
 */
LanceVersions* lance_dataset_versions(const LanceDataset* dataset);

/**
 * Number of versions in the snapshot, or 0 on error (check
 * lance_last_error_code()).
 */
uint64_t lance_versions_count(const LanceVersions* versions);

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

/* ─── lance_dataset_compact_files ─────────────────────────────────────────── */

/**
 * Tunable parameters for lance_dataset_compact_files. Pass NULL to use the
 * upstream defaults. Each numeric field uses 0 as a "keep upstream default"
 * sentinel; non-zero values are forwarded after a usize range check so the
 * API does not silently truncate on 32-bit hosts.
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
int32_t lance_scanner_with_row_id(LanceScanner* scanner, bool enable);

/**
 * Restrict scan to the given fragment IDs. Must be called before iteration.
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
 * Create a scalar index on a column.
 * @param params_json Optional JSON params string (e.g. inverted tokenizer config), or NULL.
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

/* ─── Uncommitted index segment build ─── */

/**
 * Options for an uncommitted index segment build.
 *
 * `fragment_ids == NULL && fragment_count == 0` selects the whole dataset.
 * Any non-empty fragment list is copied when the builder is created.
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

/** Free parsed segment metadata. NULL-safe. */
void lance_index_segment_metadata_free(LanceIndexSegmentMetadata* metadata);

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

int32_t lance_scanner_set_nprobes(LanceScanner* scanner, uint32_t n);
int32_t lance_scanner_set_refine_factor(LanceScanner* scanner, uint32_t f);
int32_t lance_scanner_set_ef(LanceScanner* scanner, uint32_t e);
int32_t lance_scanner_set_metric(LanceScanner* scanner, LanceMetricType metric);
int32_t lance_scanner_set_use_index(LanceScanner* scanner, bool enable);
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

/* ─── Full-text search (Phase 2) ─── */

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
 * A dataset created through this call records an auto-cleanup policy in its
 * manifest: every 20 committed versions, versions older than 14 days are
 * reclaimed automatically. Callers relying on lance_dataset_versions /
 * lance_dataset_restore for time travel should be aware that versions past
 * that horizon may no longer exist.
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
 * Same as lance_dataset_write but takes a LanceWriteParams for tuning the
 * output shape. Pass `params` = NULL to use defaults (equivalent to calling
 * lance_dataset_write directly).
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LANCE_H */
