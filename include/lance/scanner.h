/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file scanner.h
 * @brief Scanner construction, configuration, planning and iteration entry points.
 */

#ifndef LANCE_SCANNER_H
#define LANCE_SCANNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lance/attributes.h"
#include "lance/arrow_abi.h"
#include "lance/fwd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Scanner builder ─── */

/*
 * A scanner is a stateful builder and iterator; no internal lock makes its
 * configuration or iteration calls concurrent. All lance_scanner_* calls on
 * one handle must be serialized by the caller, including planning,
 * introspection, iteration, poll, and async entry points.
 * Each exported ArrowArrayStream is independent and may outlive the scanner,
 * but callbacks on one stream must be serialized. lance_scanner_close is
 * exclusive and waits for in-flight poll wakers.
 */

/**
 * Create a scanner for the dataset.
 * @param dataset  Open dataset (not consumed)
 * @param columns  NULL-terminated column names, or NULL for all
 * @param filter   SQL filter expression, or NULL
 * @return Scanner handle, or NULL on error
 */
LANCE_NODISCARD LANCE_API LanceScanner* lance_scanner_new(
    const LanceDataset* dataset,
    const char* const* columns,
    const char* filter
);

LANCE_API int32_t lance_scanner_set_limit(LanceScanner* scanner, int64_t limit);
LANCE_API int32_t lance_scanner_set_offset(LanceScanner* scanner, int64_t offset);
LANCE_API int32_t lance_scanner_set_batch_size(LanceScanner* scanner, int64_t batch_size);

/**
 * Set the target output batch size in bytes.
 *
 * When set, this takes precedence over the row-based batch size. The value
 * must be greater than zero and must be set before scanning starts. The call
 * is rejected without changing scanner state if strict batch sizing is enabled.
 */
LANCE_API int32_t lance_scanner_set_batch_size_bytes(
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
LANCE_API int32_t lance_scanner_set_io_buffer_size(
    LanceScanner* scanner,
    uint64_t io_buffer_size_bytes
);

/**
 * Set the maximum number of batches decoded concurrently.
 *
 * @param scanner          Scanner handle. Must not be NULL.
 * @param batch_readahead  Number of in-flight batch decode tasks. Must be greater than zero.
 * @return 0 on success, -1 on error
 */
LANCE_API int32_t lance_scanner_set_batch_readahead(
    LanceScanner* scanner,
    size_t batch_readahead
);

/**
 * Set fragment readahead for unordered scans.
 *
 * This setting is only used when scan-in-order is disabled. The value must be
 * greater than zero.
 */
LANCE_API int32_t lance_scanner_set_fragment_readahead(
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
LANCE_API int32_t lance_scanner_set_target_parallelism(
    LanceScanner* scanner,
    size_t target_parallelism
);

/**
 * Configure whether batches are returned in storage order (default: true).
 *
 * Disabling ordering can improve throughput by returning batches as soon as
 * they are ready.
 */
LANCE_API int32_t lance_scanner_set_scan_in_order(LanceScanner* scanner, bool scan_in_order);

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
LANCE_API int32_t lance_scanner_set_use_scalar_index(
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
LANCE_API int32_t lance_scanner_set_strict_batch_size(
    LanceScanner* scanner,
    bool strict_batch_size
);

/**
 * Configure whether file statistics may optimize the scan (default: true).
 * Intended primarily for debugging and benchmarking. Must be set before
 * scanning starts.
 */
LANCE_API int32_t lance_scanner_set_use_stats(LanceScanner* scanner, bool use_stats);

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
LANCE_API int32_t lance_scanner_set_projection(
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
LANCE_API int32_t lance_scanner_disable_scoring_autoprojection(LanceScanner* scanner);

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
LANCE_API int32_t lance_scanner_set_order_by(
    LanceScanner* scanner,
    const LanceColumnOrdering* orderings,
    size_t len
);

LANCE_API int32_t lance_scanner_with_row_id(LanceScanner* scanner, bool enable);

/** Include or omit the `_rowaddr` metadata column. Must be set before scanning. */
LANCE_API int32_t lance_scanner_with_row_address(LanceScanner* scanner, bool enable);

/**
 * Configure whether deleted rows still present in storage are returned.
 * Requires with_row_id=true; deleted rows have a NULL `_rowid`.
 * For filtered scans, also set use_scalar_index=false: indices built after a
 * deletion may omit tombstoned rows. Incompatible with scalar_index_segment,
 * even when scalar indices are disabled.
 * Fragments removed from the current snapshot are not scanned.
 * Must be set before scanning starts.
 */
LANCE_API int32_t lance_scanner_set_include_deleted_rows(
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
LANCE_API int32_t lance_scanner_set_blob_handling(LanceScanner* scanner, LanceBlobHandling handling);

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
LANCE_API int32_t lance_scanner_set_materialization_style(
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
 * @param scanner Scanner handle. Must not be NULL.
 * @param ids     Array of fragment IDs
 * @param len     Number of fragment IDs
 * @return 0 on success, -1 on error
 */
LANCE_API int32_t lance_scanner_set_fragment_ids(
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
 * @param scanner  Scanner handle. Must not be NULL.
 * @param row_ids  Array of `_rowid` values, in any order (may be NULL when len is 0)
 * @param len      Number of ids; 0 allows no rows
 * @return 0 on success, -1 on error
 */
LANCE_API int32_t lance_scanner_set_row_addr_allowlist(
    LanceScanner* scanner,
    const uint64_t* row_ids,
    size_t len
);

/**
 * Exclude rows whose `_rowid` is in `row_ids` from the scan or search.
 * Same `_rowid` space, ordering, and restrictions as
 * lance_scanner_set_row_addr_allowlist().
 * @param scanner  Scanner handle. Must not be NULL.
 * @param row_ids  Array of `_rowid` values, in any order (may be NULL when len is 0)
 * @param len      Number of ids; 0 blocks no rows
 * @return 0 on success, -1 on error
 */
LANCE_API int32_t lance_scanner_set_row_addr_blocklist(
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
 * @param scanner Scanner handle. Must not be NULL.
 * @param bytes   Serialized Substrait `ExtendedExpression` bytes (must not be NULL)
 * @param len     Length of the byte buffer (must be > 0)
 * @return 0 on success, -1 on error
 */
LANCE_API int32_t lance_scanner_set_substrait_filter(
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
 * @param scanner Scanner handle. Must not be NULL.
 * @param bytes   Serialized Substrait `Plan` bytes (NULL only when len is zero)
 * @param len     Length of the byte buffer
 * @return 0 on success, -1 on error
 */
LANCE_API int32_t lance_scanner_set_substrait_aggregate(
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
 * @param scanner Scanner handle. Must not be NULL.
 * @param filter  Non-NULL, non-empty SQL filter expression
 * @return 0 on success, -1 on error
 */
LANCE_API int32_t lance_scanner_additional_sql_filter(
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
LANCE_API int32_t lance_scanner_set_statistics_callback(
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
LANCE_API void lance_scanner_close(LanceScanner* scanner);

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
LANCE_API int32_t lance_scanner_schema(
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
LANCE_NODISCARD LANCE_API const char* lance_scanner_explain_plan(
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
LANCE_NODISCARD LANCE_API const char* lance_scanner_analyze_plan(const LanceScanner* scanner);

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
LANCE_API uint64_t lance_scanner_count_rows(const LanceScanner* scanner);

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
LANCE_NODISCARD LANCE_API const char* lance_scanner_analyze_count_plan(const LanceScanner* scanner);

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
LANCE_API int32_t lance_scanner_to_arrow_stream(
    LanceScanner* scanner,
    struct ArrowArrayStream* out
);

/* ─── Sync scan: batch iteration ─── */

/**
 * Read the next batch (blocking).
 * @param scanner  Scanner handle. Must not be NULL.
 * @param out      Set to a LanceBatch* on success, NULL on end/error
 * @return 0 = batch available, 1 = end of stream, -1 = error
 */
LANCE_API int32_t lance_scanner_next(
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
LANCE_API void lance_scanner_scan_async(
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
LANCE_API void lance_scanner_async_stream_free(struct ArrowArrayStream* stream);

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
LANCE_API LancePollStatus lance_scanner_poll_next(
    LanceScanner* scanner,
    LanceWaker waker,
    void* waker_ctx,
    LanceBatch** out
);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LANCE_SCANNER_H */
