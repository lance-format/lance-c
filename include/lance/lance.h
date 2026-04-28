/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file lance.h
 * @brief C API for the Lance columnar data format.
 *
 * All data crosses this boundary via the Arrow C Data Interface
 * (ArrowSchema, ArrowArray, ArrowArrayStream).
 *
 * Error handling uses thread-local storage: after any function returns
 * NULL (pointer) or -1 (int), call lance_last_error_code() and
 * lance_last_error_message() to get details.
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
} LanceErrorCode;

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
    uint32_t num_partitions;        /* IVF; 0 = default (lance internal) */
    uint32_t num_sub_vectors;       /* PQ;  0 = default */
    uint32_t num_bits;              /* PQ/RQ; 0 = 8 */
    uint32_t max_iterations;        /* IVF kmeans; 0 = 50 */
    uint32_t hnsw_m;                /* HNSW; 0 = default */
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

/** Return the version number of this dataset snapshot. */
uint64_t lance_dataset_version(const LanceDataset* dataset);

/** Return the number of rows. Returns 0 on error. */
uint64_t lance_dataset_count_rows(const LanceDataset* dataset);

/** Return the latest version ID (I/O). Returns 0 on error. */
uint64_t lance_dataset_latest_version(const LanceDataset* dataset);

/* ─── Version history ─── */

/**
 * Snapshot the dataset's version history. Caller frees the returned handle
 * with lance_versions_close().
 * @return handle on success, or NULL on error
 */
LanceVersions* lance_dataset_versions(const LanceDataset* dataset);

/** Number of versions in the snapshot. Returns 0 on error. */
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
 * Export the dataset schema via Arrow C Data Interface.
 * @param out  Pointer to caller-allocated ArrowSchema struct
 * @return 0 on success, -1 on error
 */
int32_t lance_dataset_schema(
    const LanceDataset* dataset,
    struct ArrowSchema* out
);

/* ─── Fragment enumeration ─── */

/** Return the number of fragments in the dataset. Returns 0 on error. */
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

/** Close and free a scanner handle. */
void lance_scanner_close(LanceScanner* scanner);

/* ─── Sync scan: ArrowArrayStream ─── */

/**
 * Materialize the scan as an ArrowArrayStream (blocking).
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
 * @param ctx     Opaque pointer passed back from the caller
 * @param status  0 = success, -1 = error
 * @param result  Operation-specific result (e.g., ArrowArrayStream*)
 */
typedef void (*LanceCallback)(void* ctx, int32_t status, void* result);

/**
 * Start an async scan. The callback fires on a dedicated dispatcher thread
 * when the ArrowArrayStream is ready.
 */
void lance_scanner_scan_async(
    const LanceScanner* scanner,
    LanceCallback callback,
    void* callback_ctx
);

/* ─── Poll-based scan (for cooperative async runtimes) ─── */

typedef enum {
    LANCE_POLL_READY    =  0,
    LANCE_POLL_PENDING  =  1,
    LANCE_POLL_FINISHED =  2,
    LANCE_POLL_ERROR    = -1,
} LancePollStatus;

/** Waker callback: called from a Tokio thread when data is ready. */
typedef void (*LanceWaker)(void* ctx);

/**
 * Poll for the next batch without blocking.
 * See RFC for usage pattern.
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

/** Drop an index by name. Returns -1 (NOT_FOUND) if no such index. */
int32_t lance_dataset_drop_index(LanceDataset* dataset, const char* name);

/** Number of user indexes (excludes system indexes). Returns 0 on error. */
uint64_t lance_dataset_index_count(const LanceDataset* dataset);

/**
 * JSON array describing all user indexes.
 * Caller must free the returned string with lance_free_string().
 * Returns NULL on error.
 */
const char* lance_dataset_index_list_json(const LanceDataset* dataset);

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
