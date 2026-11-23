/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file dataset.h
 * @brief Dataset handles: open, close, snapshots, metadata and version history.
 */

#ifndef LANCE_DATASET_H
#define LANCE_DATASET_H

#include <stddef.h>
#include <stdint.h>
#include "lance/arrow_abi.h"
#include "lance/fwd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Dataset lifecycle ─── */

/*
 * A dataset handle may be shared across threads. Snapshot-based reads may
 * overlap each other and may overlap in-place mutations. In-place mutations
 * take the handle's internal write lock for the complete native call, including
 * synchronous callbacks, so they serialize; a reader that already holds a
 * snapshot keeps its old view. Snapshot-style operations such as restore and
 * checkout_* work from a snapshot and return a new handle; they do not mutate
 * the source handle and are not serialized by its lock. Restore still follows
 * native commit-conflict rules. lance_dataset_close is exclusive.
 */

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

/**
 * Additive options for lance_dataset_open_with_options.
 *
 * Zero-initialize to open the latest version with native storage defaults.
 * `storage_options_accessor` may be NULL for native defaults or point to a
 * native dynamic accessor. The native dataset retains the accessor (and its
 * provider/cache) after this call, so closing the C accessor does not
 * invalidate later dataset I/O. Static options belong in the accessor's
 * initial options. `version == 0` opens the latest version.
 */
typedef struct LanceDatasetOpenOptions {
    uint64_t version;
    const LanceStorageOptionsAccessor* storage_options_accessor;
} LanceDatasetOpenOptions;

/**
 * Open a dataset with additive options and an optional shared session.
 *
 * `options` may be NULL, which is equivalent to a zero-initialized struct.
 * `session` may be NULL for the default session. A non-NULL session is
 * retained through shared ownership, exactly like
 * lance_dataset_open_with_session.
 *
 * The returned dataset retains the native accessor and its provider. Later
 * dataset I/O uses the native provider refresh, error and cache policy; the
 * binding adds no second credential cache. Passing NULL for the accessor
 * preserves the existing native default behavior.
 *
 * @return Dataset handle, or NULL on error.
 */
LanceDataset* lance_dataset_open_with_options(
    const char* uri,
    const LanceDatasetOpenOptions* options,
    const LanceSession* session
);


/**
 * Open a dataset with additive options, an optional shared session, and
 * per-base store parameters for its shared bases.
 *
 * `options` and `session` have the same meaning and ownership behavior as
 * lance_dataset_open_with_options: NULL options means latest version and native
 * storage defaults, and NULL session means the default session. A non-NULL
 * accessor is retained natively and closing the C accessor does not invalidate
 * the dataset.
 *
 * `base_store_params` binds native `ObjectStoreParams` to exact
 * `BasePath.path` keys from the dataset manifest:
 *
 * - NULL selects native `None` (no per-base bindings).
 * - A non-NULL list with `count == 0` selects native `Some(empty)`, which adds
 *   no bindings.
 * - Entries match the exact stored base path, not a base id or name and with no
 *   URI normalization. Duplicate keys use the last entry.
 *
 * An exact binding takes precedence over the dataset-level storage options.
 * Without one, native base store resolution falls back to the dataset-level
 * accessor/options scoped to that base. These bindings are runtime-only:
 * Lance does not persist accessors, credentials, or other store parameters in
 * the dataset, so reopening the dataset later must supply them again when the
 * base needs them. This call adds no second parser, credential cache, or store
 * manager.
 *
 * Malformed lists are rejected before the native dataset is opened. A NULL
 * `items` with a non-zero `count`, a NULL or non-UTF-8 base path, an
 * out-of-range block size, or invalid S3 refresh nanoseconds returns NULL with
 * LANCE_ERR_INVALID_ARGUMENT.
 *
 * @param uri               Dataset path (file://, s3://, memory://, etc.)
 * @param options           Additive open options, or NULL
 * @param session           Shared session, or NULL for the default session
 * @param base_store_params Per-base bindings, or NULL for native None
 * @return Dataset handle, or NULL on error
 */
LanceDataset* lance_dataset_open_with_base_store_params(
    const char* uri,
    const LanceDatasetOpenOptions* options,
    const LanceSession* session,
    const LanceBaseStoreParamsList* base_store_params
);

/**
 * Open a Lance dataset through a foreign commit lock and an optional dynamic
 * storage-options accessor.
 *
 * A non-NULL `storage_accessor` forwards its native accessor owner; NULL, or
 * an accessor constructed without an inner owner, selects native omission.
 * `commit_lock` may be NULL to keep the URI-derived native handler. `session`
 * may be NULL. `version == 0` opens the latest version.
 *
 * The returned dataset retains the native handler and accessor owners, so
 * closing either input handle afterwards does not invalidate this dataset,
 * its scanners, or later commits performed through it.
 *
 * @param uri               Dataset path (file://, s3://, memory://, etc.)
 * @param storage_accessor  Dynamic storage-options accessor, or NULL
 * @param version           Version to open; 0 opens the latest version
 * @param session           Shared session, or NULL for the default session
 * @param commit_lock       Foreign commit lock, or NULL for the URI-derived
 *                          native handler
 * @param out_dataset       Receives the dataset handle on success; must not
 *                          be NULL
 * @return 0 on success, -1 on error
 */
int32_t lance_dataset_open_with_commit_lock(
    const char* uri,
    const LanceStorageOptionsAccessor* storage_accessor,
    uint64_t version,
    const LanceSession* session,
    const LanceCommitLock* commit_lock,
    LanceDataset** out_dataset
);

/** Close and free a dataset handle. Safe to call with NULL. */
void lance_dataset_close(LanceDataset* dataset);

/**
 * Clone a dataset handle as an independent in-memory snapshot.
 *
 * The returned handle shares the source's native Dataset snapshot and its
 * Arc-backed state (session, object-store registry, caches, commit handler),
 * but has its own lock. Cloning performs no storage I/O. Mutating either
 * handle does not change the other handle's version, schema, or rows, and
 * does not hold the other handle's lock. Close the returned handle with
 * `lance_dataset_close`; source and clone may be closed in either order.
 *
 * This duplicates an in-memory handle; it is not Lance's on-disk shallow or
 * deep clone operation.
 *
 * @param dataset Source handle; must not be NULL
 * @return New independent handle, or NULL on error
 */
LanceDataset* lance_dataset_clone_handle(const LanceDataset* dataset);


/* ─── Dataset metadata (sync, in-memory) ─── */

/**
 * Return the version number of this dataset snapshot.
 * @return version on success, or 0 on error (check lance_last_error_code())
 */
uint64_t lance_dataset_version(const LanceDataset* dataset);

/**
 * Return the fully qualified URI of this dataset snapshot.
 *
 * This copies native `Dataset::uri()`, the URI resolved when the dataset was
 * opened. A namespace-opened dataset reports the `describe_table` location,
 * not its table ID. The returned string stays valid after the dataset handle
 * is closed.
 *
 * Caller must free the returned string with `lance_free_string()`.
 * Returns NULL on error, including a NULL dataset handle.
 */
const char* lance_dataset_uri(const LanceDataset* dataset);

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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LANCE_DATASET_H */
