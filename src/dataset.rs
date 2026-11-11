// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Dataset C API: open, close, metadata, schema, take.

use std::ffi::c_char;
use std::panic::{AssertUnwindSafe, catch_unwind, resume_unwind};
use std::ptr;
use std::sync::{Arc, RwLock};

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi_stream::FFI_ArrowArrayStream;
use arrow_schema::Schema as ArrowSchema;
use lance::Dataset;
use lance::dataset::builder::DatasetBuilder;
use lance_core::Result;

use crate::data_file::LanceDataFile;
use crate::error::{ffi_try, swallow_unwind};
use crate::file_session::LanceObjectStoreIoStats;
use crate::helpers;
use crate::runtime::block_on;
use crate::session::LanceSession;
use crate::storage_options::{LanceStorageOptionsAccessor, storage_options_accessor_from_ffi};
use crate::stream_guard::guarded_ffi_stream_from_reader;

/// Opaque handle representing an opened Lance dataset.
pub struct LanceDataset {
    pub(crate) inner: RwLock<Arc<Dataset>>,
}

impl LanceDataset {
    /// Take a consistent snapshot of the inner dataset.
    /// Returns a cloned Arc so the caller can hold it without keeping the lock.
    ///
    /// Lock access is deliberately poison-tolerant. The lock is only ever
    /// poisoned by a panic unwinding out of `with_mut`, and `with_mut` catches
    /// that panic before swapping (see below), so the guarded value is always
    /// a single consistent `Arc<Dataset>` pointer: it is never observed
    /// half-mutated, and the swap itself is one atomic pointer store that
    /// cannot tear. A poisoned lock therefore still protects consistent data,
    /// and dataset handles stay usable after a caught panic.
    pub(crate) fn snapshot(&self) -> Arc<Dataset> {
        self.inner.read().unwrap_or_else(|e| e.into_inner()).clone()
    }

    /// Mutate the inner dataset under an exclusive write lock, using
    /// clone-execute-swap so a panicking mutation cannot corrupt the handle:
    ///
    /// 1. `Dataset::clone` (a cheap shallow clone — every heap field of
    ///    `Dataset` is an `Arc` or small value) copies the current dataset
    ///    out of the lock.
    /// 2. `f` runs on that clone, still under the write lock, so concurrent
    ///    mutations stay serialized exactly as before. The call is wrapped in
    ///    `catch_unwind`.
    /// 3. On success the mutated clone replaces the stored `Arc` in one
    ///    pointer swap and `f`'s return value is handed back.
    /// 4. If `f` panics, the guard still holds the pristine old `Arc`, so the
    ///    in-memory state is rolled back simply by never swapping. The panic
    ///    is re-thrown via `resume_unwind` with its original payload so the
    ///    entry-point `ffi_try!` guard maps it to `LANCE_ERR_PANIC` with the
    ///    real message.
    ///
    /// Note this is deliberately *not* `Arc::make_mut` + restore-on-panic:
    /// when the handle is the unique owner, `make_mut` mutates the shared
    /// allocation in place, and "restoring" the old `Arc` would then restore
    /// the same already-half-mutated dataset. Cloning first is the only form
    /// that makes rollback real.
    ///
    /// The resumed unwind poisons the `RwLock`; the poison-tolerant accessors
    /// are why the handle keeps working afterwards (per the issue consensus:
    /// on-disk state cannot tear either, because Lance commits are atomic
    /// manifest swaps).
    pub(crate) fn with_mut<F, R>(&self, f: F) -> R
    where
        F: FnOnce(&mut Dataset) -> R,
    {
        let mut guard = self.inner.write().unwrap_or_else(|e| e.into_inner());
        let mut working = Dataset::clone(&**guard);
        match catch_unwind(AssertUnwindSafe(|| f(&mut working))) {
            Ok(ret) => {
                *guard = Arc::new(working);
                ret
            }
            Err(payload) => resume_unwind(payload),
        }
    }
}

fn projection_from_columns(
    dataset: &Dataset,
    columns: Option<&[String]>,
) -> Result<lance::dataset::ProjectionRequest> {
    match columns {
        Some(columns) => {
            let schema = dataset
                .schema()
                .project_preserve_system_columns(columns)
                .map_err(|err| {
                    lance_core::Error::invalid_input(format!("invalid columns {columns:?}: {err}"))
                })?;
            Ok(lance::dataset::ProjectionRequest::from_schema(schema))
        }
        None => Ok(lance::dataset::ProjectionRequest::from_schema(
            dataset.schema().clone(),
        )),
    }
}

// ---------------------------------------------------------------------------
// Dataset lifecycle
// ---------------------------------------------------------------------------

/// Optional settings for `lance_dataset_open_with_options`.
///
/// Zero-initialization opens the latest version with native storage defaults.
/// A NULL `storage_options_accessor` uses native defaults; a non-NULL accessor
/// is retained by the returned dataset and by later native owners. Static
/// storage options belong in the accessor's initial options.
#[repr(C)]
pub struct LanceDatasetOpenOptions {
    /// Dataset version to open. Pass 0 for latest.
    pub version: u64,
    /// Native dynamic storage-options accessor, or NULL for native defaults.
    pub storage_options_accessor: *const LanceStorageOptionsAccessor,
}

/// Open a Lance dataset at the given URI using additive options.
///
/// `options` may be NULL for latest version and native storage defaults.
/// `session` may be NULL for the default session; a non-NULL session is
/// retained through shared ownership exactly like
/// `lance_dataset_open_with_session`. A non-NULL accessor is cloned and
/// retained by the native dataset; closing the C accessor does not invalidate
/// the dataset.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_open_with_options(
    uri: *const c_char,
    options: *const LanceDatasetOpenOptions,
    session: *const LanceSession,
) -> *mut LanceDataset {
    ffi_try!(
        unsafe { open_dataset_with_options_inner(uri, options, session) },
        null
    )
}

/// Open a Lance dataset at the given URI.
///
/// - `uri`: Dataset path (file://, s3://, az://, gs://, memory://)
/// - `storage_options`: NULL-terminated key-value pairs `["k1","v1","k2","v2",NULL]`, or NULL.
/// - `version`: Dataset version to open. Pass 0 for latest.
///
/// Returns an opaque `LanceDataset*` on success, or NULL on error.
/// On error, call `lance_last_error_code()` / `lance_last_error_message()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_open(
    uri: *const c_char,
    storage_options: *const *const c_char,
    version: u64,
) -> *mut LanceDataset {
    ffi_try!(
        unsafe { open_dataset_inner(uri, storage_options, version, None, ptr::null()) },
        null
    )
}

/// Open a Lance dataset using a shared session.
///
/// The dataset retains shared ownership of the session state, so the caller
/// may close the session handle after this function returns successfully.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_open_with_session(
    uri: *const c_char,
    storage_options: *const *const c_char,
    version: u64,
    session: *const LanceSession,
) -> *mut LanceDataset {
    ffi_try!(
        unsafe { open_dataset_with_session_inner(uri, storage_options, version, session) },
        null
    )
}

unsafe fn open_dataset_with_session_inner(
    uri: *const c_char,
    storage_options: *const *const c_char,
    version: u64,
    session: *const LanceSession,
) -> Result<*mut LanceDataset> {
    if session.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "session must not be NULL".into(),
        ));
    }
    let session = unsafe { &*session };
    unsafe { open_dataset_inner(uri, storage_options, version, Some(session), ptr::null()) }
}

unsafe fn open_dataset_with_options_inner(
    uri: *const c_char,
    options: *const LanceDatasetOpenOptions,
    session: *const LanceSession,
) -> Result<*mut LanceDataset> {
    let (version, accessor) = if options.is_null() {
        (0, ptr::null())
    } else {
        // SAFETY: the caller guarantees a non-NULL options pointer is valid
        // for the duration of this call.
        let options = unsafe { &*options };
        (options.version, options.storage_options_accessor)
    };
    let session = if session.is_null() {
        None
    } else {
        // SAFETY: non-NULL session points to a live handle for this call.
        Some(unsafe { &*session })
    };
    unsafe { open_dataset_inner(uri, ptr::null(), version, session, accessor) }
}

unsafe fn open_dataset_inner(
    uri: *const c_char,
    storage_options: *const *const c_char,
    version: u64,
    session: Option<&LanceSession>,
    storage_options_accessor: *const LanceStorageOptionsAccessor,
) -> Result<*mut LanceDataset> {
    let uri_str = unsafe { helpers::parse_c_string(uri)? }
        .ok_or_else(|| lance_core::Error::invalid_input_source("uri must not be NULL".into()))?;

    let opts = unsafe { helpers::parse_storage_options(storage_options)? };
    let accessor = unsafe { storage_options_accessor_from_ffi(storage_options_accessor)? };
    if accessor.is_some() && !opts.is_empty() {
        return Err(lance_core::Error::invalid_input_source(
            "storage_options must be NULL when a storage options accessor is provided".into(),
        ));
    }

    let mut builder = DatasetBuilder::from_uri(uri_str);
    if let Some(accessor) = accessor {
        builder = builder.with_storage_options_accessor(accessor);
    } else if !opts.is_empty() {
        builder = builder.with_storage_options(opts);
    }
    if version != 0 {
        builder = builder.with_version(version);
    }
    if let Some(session) = session {
        builder = builder.with_session(session.inner.clone());
    }

    let dataset = block_on(builder.load())?;
    let handle = LanceDataset {
        inner: RwLock::new(Arc::new(dataset)),
    };
    Ok(Box::into_raw(Box::new(handle)))
}

/// Clone a dataset handle as an independent in-memory snapshot.
///
/// The returned handle is backed by the same `Arc<Dataset>` snapshot as
/// `dataset`, but has its own lock. Mutating either handle therefore does not
/// change the other's version, schema, or rows, and does not hold the other
/// handle's lock. Cloning performs no storage I/O and does not create a new
/// session, object-store registry, or cache.
///
/// This is in-memory handle duplication, not Lance's on-disk shallow or deep
/// clone. Close the returned handle with `lance_dataset_close`; the source and
/// clone may be closed in either order.
///
/// Returns NULL with `LANCE_ERR_INVALID_ARGUMENT` when `dataset` is NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_clone_handle(
    dataset: *const LanceDataset,
) -> *mut LanceDataset {
    ffi_try!(unsafe { dataset_clone_handle_inner(dataset) }, null)
}

unsafe fn dataset_clone_handle_inner(dataset: *const LanceDataset) -> Result<*mut LanceDataset> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset is NULL".into(),
        ));
    }
    let dataset = unsafe { &*dataset };
    let snapshot = dataset.snapshot();
    let handle = LanceDataset {
        inner: RwLock::new(snapshot),
    };
    Ok(Box::into_raw(Box::new(handle)))
}

/// Close and free a dataset handle.
/// Safe to call with NULL. A non-NULL handle must be closed exactly once and
/// must not be used again afterwards.
///
/// Best-effort (issue #61): a panic raised while dropping the handle is
/// caught and logged rather than unwinding into the caller, and the
/// remainder of the value may leak. Deliberately no poison check — a
/// poisoned handle must still be freeable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_close(dataset: *mut LanceDataset) {
    if !dataset.is_null() {
        swallow_unwind("lance_dataset_close", || unsafe {
            let _ = Box::from_raw(dataset);
        });
    }
}

// ---------------------------------------------------------------------------
// Metadata (in-memory, sync only)
// ---------------------------------------------------------------------------

/// Return the version number of this dataset snapshot.
/// Returns 0 on error — check `lance_last_error_code()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_version(dataset: *const LanceDataset) -> u64 {
    ffi_try!(unsafe { dataset_version_inner(dataset) }, 0)
}

unsafe fn dataset_version_inner(dataset: *const LanceDataset) -> Result<u64> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset is NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    Ok(ds.snapshot().version().version)
}

/// Return the number of rows in the dataset.
/// Returns 0 on error — check `lance_last_error_code()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_count_rows(dataset: *const LanceDataset) -> u64 {
    ffi_try!(unsafe { dataset_count_rows_inner(dataset) }, 0)
}

unsafe fn dataset_count_rows_inner(dataset: *const LanceDataset) -> Result<u64> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset is NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    Ok(block_on(ds.snapshot().count_rows(None))? as u64)
}

/// Validate the dataset's fragments, stable row IDs and indices.
///
/// This calls the native `Dataset::validate` over the handle's current
/// snapshot. Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_validate(dataset: *const LanceDataset) -> i32 {
    ffi_try!(unsafe { dataset_validate_inner(dataset) }, neg)
}

unsafe fn dataset_validate_inner(dataset: *const LanceDataset) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset is NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    block_on(ds.snapshot().validate())?;
    Ok(0)
}

/// Count rows hidden by deletion files across all fragments.
///
/// The output is written only on success. Uses the native
/// `Dataset::count_deleted_rows` result.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_count_deleted_rows(
    dataset: *const LanceDataset,
    out_rows: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { dataset_count_deleted_rows_inner(dataset, out_rows) },
        neg
    )
}

unsafe fn dataset_count_deleted_rows_inner(
    dataset: *const LanceDataset,
    out_rows: *mut u64,
) -> Result<i32> {
    if dataset.is_null() || out_rows.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset and out_rows must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let rows = block_on(ds.snapshot().count_deleted_rows())? as u64;
    // SAFETY: both pointers are non-NULL and caller-owned. The output is
    // written only after the native count succeeds.
    unsafe {
        std::ptr::write_unaligned(out_rows, rows);
    }
    Ok(0)
}

/// Count fragments with fewer physical rows than `max_rows_per_group`.
///
/// This is the infallible native `Dataset::num_small_files` contract:
/// `max_rows_per_group == 0` therefore returns 0. The output is written only
/// when `dataset` and `out_count` are non-NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_num_small_files(
    dataset: *const LanceDataset,
    max_rows_per_group: usize,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { dataset_num_small_files_inner(dataset, max_rows_per_group, out_count) },
        neg
    )
}

unsafe fn dataset_num_small_files_inner(
    dataset: *const LanceDataset,
    max_rows_per_group: usize,
    out_count: *mut u64,
) -> Result<i32> {
    if dataset.is_null() || out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset and out_count must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let count = block_on(ds.snapshot().num_small_files(max_rows_per_group)) as u64;
    // SAFETY: both pointers are non-NULL and caller-owned.
    unsafe {
        std::ptr::write_unaligned(out_count, count);
    }
    Ok(0)
}

/// Migrate this dataset's manifest paths to the v2 naming scheme.
///
/// The rename is performed by native `Dataset::migrate_manifest_paths_v2`;
/// the dataset handle is replaced with the reopened native snapshot on
/// success. Storage renames can persist if a later native step fails, so the
/// binding makes no rollback or added-atomicity claim.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_migrate_manifest_paths_v2(
    dataset: *mut LanceDataset,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if dataset.is_null() {
                return Err(lance_core::Error::invalid_input("dataset must not be NULL"));
            }
            unsafe { &*dataset }
                .with_mut(|dataset| block_on(dataset.migrate_manifest_paths_v2()))?;
            Ok(0)
        })(),
        neg
    )
}

/// Delete every row through native `Dataset::truncate_table`.
///
/// A new dataset version is committed on success; schema and configuration
/// stay unchanged. Native commit and storage failure behavior is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_truncate_table(dataset: *mut LanceDataset) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if dataset.is_null() {
                return Err(lance_core::Error::invalid_input("dataset must not be NULL"));
            }
            unsafe { &*dataset }.with_mut(|dataset| block_on(dataset.truncate_table()))?;
            Ok(0)
        })(),
        neg
    )
}

/// Count rows matching an optional SQL filter.
///
/// `filter == NULL` maps to native `Dataset::count_rows(None)` and uses the
/// metadata fast path. A non-NULL pointer, including an empty string, maps to
/// `Some(filter)`. Returns 0 on error; an empty match also returns 0, so check
/// `lance_last_error_code()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_count_rows_filtered(
    dataset: *const LanceDataset,
    filter: *const c_char,
) -> u64 {
    ffi_try!(
        unsafe { dataset_count_rows_filtered_inner(dataset, filter) },
        0
    )
}

unsafe fn dataset_count_rows_filtered_inner(
    dataset: *const LanceDataset,
    filter: *const c_char,
) -> Result<u64> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let filter = unsafe { helpers::parse_c_string(filter) }?.map(str::to_owned);
    Ok(block_on(ds.snapshot().count_rows(filter))? as u64)
}

/// Return the latest version ID of the dataset.
/// Returns 0 on error — check `lance_last_error_code()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_latest_version(dataset: *const LanceDataset) -> u64 {
    ffi_try!(unsafe { dataset_latest_version_inner(dataset) }, 0)
}

unsafe fn dataset_latest_version_inner(dataset: *const LanceDataset) -> Result<u64> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset is NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    block_on(ds.snapshot().latest_version_id())
}

/// Snapshot the dataset's native object-store I/O counters.
///
/// The counters are the current native accumulator and are not reset by this
/// call. They come from the same native store tracker used by
/// `Dataset::object_store(None)`, so the semantics match the native Python
/// `io_stats_snapshot` binding. A preceding incremental call resets the
/// accumulator. Returns 0 on
/// success or -1 on a NULL handle/out pointer or native failure. `out_stats` is
/// written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_io_stats_snapshot(
    dataset: *const LanceDataset,
    out_stats: *mut LanceObjectStoreIoStats,
) -> i32 {
    ffi_try!(
        unsafe { dataset_io_stats_inner(dataset, out_stats, false) },
        neg
    )
}

/// Return the dataset's native object-store I/O counters accumulated since the
/// previous incremental call, then reset that accumulator to zero.
///
/// The reset is owned by the native store tracker. This binding keeps no
/// baseline or cached counters. Returns 0 on success or -1 on a NULL
/// handle/out pointer or native failure. `out_stats` is written only on
/// success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_io_stats_incremental(
    dataset: *const LanceDataset,
    out_stats: *mut LanceObjectStoreIoStats,
) -> i32 {
    ffi_try!(
        unsafe { dataset_io_stats_inner(dataset, out_stats, true) },
        neg
    )
}

unsafe fn dataset_io_stats_inner(
    dataset: *const LanceDataset,
    out_stats: *mut LanceObjectStoreIoStats,
    incremental: bool,
) -> Result<i32> {
    if dataset.is_null() || out_stats.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset and out_stats must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let snapshot = ds.snapshot();
    let store = block_on(snapshot.object_store(None))?;
    let stats = if incremental {
        store.io_stats_incremental()
    } else {
        store.io_stats_snapshot()
    };
    unsafe {
        ptr::write(
            out_stats,
            LanceObjectStoreIoStats {
                read_iops: stats.read_iops,
                read_bytes: stats.read_bytes,
                write_iops: stats.write_iops,
                written_bytes: stats.written_bytes,
            },
        );
    }
    Ok(0)
}

// ---------------------------------------------------------------------------
// Schema (Arrow C Data Interface)
// ---------------------------------------------------------------------------

/// Export the dataset schema as an Arrow C Data Interface `ArrowSchema`.
///
/// The caller must provide a pointer to a stack-allocated `ArrowSchema` struct.
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_schema(
    dataset: *const LanceDataset,
    out: *mut FFI_ArrowSchema,
) -> i32 {
    ffi_try!(unsafe { dataset_schema_inner(dataset, out) }, neg)
}

unsafe fn dataset_schema_inner(
    dataset: *const LanceDataset,
    out: *mut FFI_ArrowSchema,
) -> Result<i32> {
    if dataset.is_null() || out.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset and out must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let snap = ds.snapshot();
    let lance_schema = snap.schema();
    let arrow_schema: ArrowSchema = lance_schema.into();
    let ffi_schema = FFI_ArrowSchema::try_from(&arrow_schema)?;
    unsafe {
        std::ptr::write_unaligned(out, ffi_schema);
    }
    Ok(0)
}

/// Create native data-file metadata from an existing Lance file.
///
/// Maps directly to `Dataset::create_data_file(path, base_id)`. `path` is
/// relative to the selected data directory and must be non-NULL UTF-8.
/// `base_id == NULL` selects native `None`; a non-NULL pointer selects native
/// `Some`, including `Some(0)`. Empty paths, missing files, malformed Lance
/// files, schema mismatches and unregistered base IDs retain native validation
/// and error timing.
///
/// On success `*out_file` receives an independently owned snapshot that
/// remains valid after the dataset is closed. The output is written only on
/// success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_create_data_file(
    dataset: *const LanceDataset,
    path: *const c_char,
    base_id: *const u32,
    out_file: *mut *mut LanceDataFile,
) -> i32 {
    ffi_try!(
        unsafe { dataset_create_data_file_inner(dataset, path, base_id, out_file) },
        neg
    )
}

unsafe fn dataset_create_data_file_inner(
    dataset: *const LanceDataset,
    path: *const c_char,
    base_id: *const u32,
    out_file: *mut *mut LanceDataFile,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    if out_file.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_file must not be NULL".into(),
        ));
    }
    let path = unsafe { helpers::parse_c_string(path)? }
        .ok_or_else(|| lance_core::Error::invalid_input_source("path must not be NULL".into()))?;
    let base_id = if base_id.is_null() {
        None
    } else {
        Some(unsafe { ptr::read(base_id) })
    };

    let snapshot = unsafe { &*dataset }.snapshot();
    let file = block_on(snapshot.create_data_file(path, base_id))?;
    let file = Box::new(LanceDataFile::from_native(file)?);
    unsafe {
        ptr::write(out_file, Box::into_raw(file));
    }
    Ok(0)
}

// ---------------------------------------------------------------------------
// Random access (take)
// ---------------------------------------------------------------------------

/// Take rows by indices, returning results as an ArrowArrayStream.
///
/// - `indices`: array of row indices (0-based offsets)
/// - `num_indices`: length of the indices array
/// - `columns`: NULL-terminated column name array, or NULL for all columns
/// - `out`: pointer to a stack-allocated `ArrowArrayStream`
///
/// The already-materialized batch is exported through a guarded reader:
/// schema conversion is validated before callbacks are exposed, and later
/// `get_next` / `release` panics are contained at the Arrow C boundary.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_take(
    dataset: *const LanceDataset,
    indices: *const u64,
    num_indices: usize,
    columns: *const *const c_char,
    out: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe { dataset_take_inner(dataset, indices, num_indices, columns, out) },
        neg
    )
}

unsafe fn dataset_take_inner(
    dataset: *const LanceDataset,
    indices: *const u64,
    num_indices: usize,
    columns: *const *const c_char,
    out: *mut FFI_ArrowArrayStream,
) -> Result<i32> {
    if dataset.is_null() || indices.is_null() || out.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset, indices, and out must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let idx_slice = unsafe { std::slice::from_raw_parts(indices, num_indices) };
    let col_names = unsafe { helpers::parse_c_string_array(columns)? };

    let snap = ds.snapshot();
    let projection = projection_from_columns(&snap, col_names.as_deref())?;

    let batch = block_on(snap.take(idx_slice, projection))?;

    // Wrap the single RecordBatch as a RecordBatchReader, then export as FFI stream.
    let schema = batch.schema();
    let reader = arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema);
    let ffi_stream = guarded_ffi_stream_from_reader(reader)?;
    unsafe {
        std::ptr::write_unaligned(out, ffi_stream);
    }
    Ok(0)
}

/// Take rows by dataset row IDs, returning results as an ArrowArrayStream.
///
/// - `row_ids`: array of dataset row IDs, such as values returned in the
///   `_rowid` scanner column
/// - `num_row_ids`: length of the row ID array
/// - `columns`: NULL-terminated column name array, or NULL for all columns
/// - `out`: pointer to a stack-allocated `ArrowArrayStream`
///
/// `row_ids` may be NULL only when `num_row_ids` is zero. Row IDs must belong
/// to the same dataset snapshot used for this read. Missing or deleted row IDs
/// may be omitted from the result by the upstream Lance implementation.
///
/// The already-materialized batch is exported through the same guarded reader
/// as [`lance_dataset_take`], including schema preflight and deferred callback
/// panic containment.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_take_rows(
    dataset: *const LanceDataset,
    row_ids: *const u64,
    num_row_ids: usize,
    columns: *const *const c_char,
    out: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe { dataset_take_rows_inner(dataset, row_ids, num_row_ids, columns, out) },
        neg
    )
}

unsafe fn dataset_take_rows_inner(
    dataset: *const LanceDataset,
    row_ids: *const u64,
    num_row_ids: usize,
    columns: *const *const c_char,
    out: *mut FFI_ArrowArrayStream,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input("dataset must not be NULL"));
    }
    if out.is_null() {
        return Err(lance_core::Error::invalid_input("out must not be NULL"));
    }
    if num_row_ids > 0 && row_ids.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "row_ids must not be NULL when num_row_ids = {num_row_ids}"
        )));
    }

    let ds = unsafe { &*dataset };
    let row_id_slice = if num_row_ids == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(row_ids, num_row_ids) }
    };
    let col_names = unsafe { helpers::parse_c_string_array(columns)? };

    let snap = ds.snapshot();
    let projection = projection_from_columns(&snap, col_names.as_deref())?;

    let batch = block_on(snap.take_rows(row_id_slice, projection))?;

    // Match lance_dataset_take: export the single RecordBatch as an Arrow stream.
    let schema = batch.schema();
    let reader = arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema);
    let ffi_stream = guarded_ffi_stream_from_reader(reader)?;
    unsafe {
        std::ptr::write_unaligned(out, ffi_stream);
    }
    Ok(0)
}

// ---------------------------------------------------------------------------
// Fragment enumeration
// ---------------------------------------------------------------------------

/// Return the number of fragments in the dataset.
/// Returns 0 on error — check `lance_last_error_code()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_fragment_count(dataset: *const LanceDataset) -> u64 {
    ffi_try!(unsafe { dataset_fragment_count_inner(dataset) }, 0)
}

unsafe fn dataset_fragment_count_inner(dataset: *const LanceDataset) -> Result<u64> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset is NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    Ok(ds.snapshot().count_fragments() as u64)
}

/// Fill `out_ids` with the fragment IDs of the dataset.
///
/// The caller must allocate `out_ids` with at least
/// `lance_dataset_fragment_count()` elements.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_fragment_ids(
    dataset: *const LanceDataset,
    out_ids: *mut u64,
) -> i32 {
    ffi_try!(unsafe { dataset_fragment_ids_inner(dataset, out_ids) }, neg)
}

unsafe fn dataset_fragment_ids_inner(
    dataset: *const LanceDataset,
    out_ids: *mut u64,
) -> Result<i32> {
    if dataset.is_null() || out_ids.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset and out_ids must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let fragments = ds.snapshot().get_fragments();
    for (i, frag) in fragments.iter().enumerate() {
        unsafe {
            *out_ids.add(i) = frag.id() as u64;
        }
    }
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    use std::sync::mpsc;
    use std::thread;

    use arrow_array::{Int32Array, RecordBatch, StringArray};
    use arrow_schema::{DataType, Field, Schema};

    /// Unblock a scoped writer even if an assertion panics before the test
    /// reaches its explicit release point.
    struct ReleaseOnDrop(mpsc::Sender<()>);

    impl Drop for ReleaseOnDrop {
        fn drop(&mut self) {
            let _ = self.0.send(());
        }
    }

    /// Build a real 3-row dataset in a tempdir and wrap it in a handle.
    /// (The default panic hook prints to stderr during the panic test; that
    /// is expected noise.)
    fn create_test_handle() -> (tempfile::TempDir, LanceDataset) {
        let tmp = tempfile::tempdir().unwrap();
        let uri = tmp.path().join("with_mut_ds").to_str().unwrap().to_string();

        let schema = Arc::new(Schema::new(vec![
            Field::new("id", DataType::Int32, false),
            Field::new("name", DataType::Utf8, true),
        ]));
        let batch = RecordBatch::try_new(
            schema.clone(),
            vec![
                Arc::new(Int32Array::from(vec![1, 2, 3])),
                Arc::new(StringArray::from(vec!["a", "b", "c"])),
            ],
        )
        .unwrap();

        let dataset = block_on(Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
            &uri,
            None,
        ))
        .unwrap();
        let handle = LanceDataset {
            inner: RwLock::new(Arc::new(dataset)),
        };
        (tmp, handle)
    }

    #[test]
    fn with_mut_mutation_is_visible_via_snapshot() {
        let (_tmp, handle) = create_test_handle();
        assert_eq!(handle.snapshot().version().version, 1);

        let result = handle.with_mut(|ds| block_on(ds.delete("id = 1")).unwrap());
        assert_eq!(result.num_deleted_rows, 1);

        let snap = handle.snapshot();
        assert_eq!(snap.version().version, 2);
        assert_eq!(block_on(snap.count_rows(None)).unwrap(), 2);
    }

    #[test]
    fn clone_handle_shares_snapshot_without_sharing_lock() {
        let (_tmp, handle) = create_test_handle();
        let source_snapshot = handle.snapshot();

        let clone_ptr = unsafe { lance_dataset_clone_handle(&handle) };
        assert!(!clone_ptr.is_null());
        assert!(!std::ptr::eq(
            clone_ptr as *const LanceDataset,
            &handle as *const LanceDataset
        ));
        let cloned = unsafe { &*clone_ptr };

        // The clone must reuse the exact native snapshot instead of reopening
        // the dataset (which would create a new Arc and perform storage I/O).
        let cloned_snapshot = cloned.snapshot();
        assert!(Arc::ptr_eq(&source_snapshot, &cloned_snapshot));
        assert_eq!(
            cloned_snapshot.version().version,
            source_snapshot.version().version
        );

        // Hold the clone's exclusive mutation lock at a controlled point, then
        // prove the source handle can still be read. try_read is deterministic:
        // a shared lock fails immediately instead of hanging the test.
        let (entered_tx, entered_rx) = mpsc::channel();
        let (release_tx, release_rx) = mpsc::channel();
        thread::scope(|scope| {
            let writer = scope.spawn(move || {
                cloned.with_mut(|ds| {
                    entered_tx.send(()).unwrap();
                    release_rx.recv().unwrap();
                    block_on(ds.delete("id = 1")).unwrap()
                })
            });

            let release = ReleaseOnDrop(release_tx);
            entered_rx.recv().unwrap();
            let source_guard = handle
                .inner
                .try_read()
                .expect("source read must not wait for a cloned handle mutation");
            assert_eq!(source_guard.version().version, 1);
            assert_eq!(block_on(source_guard.count_rows(None)).unwrap(), 3);
            drop(source_guard);

            drop(release);
            assert_eq!(writer.join().unwrap().num_deleted_rows, 1);
        });

        // The clone advanced independently; the source snapshot did not.
        assert_eq!(cloned.snapshot().version().version, 2);
        assert_eq!(handle.snapshot().version().version, 1);
        assert_eq!(block_on(handle.snapshot().count_rows(None)).unwrap(), 3);

        unsafe { lance_dataset_close(clone_ptr) };
    }

    #[test]
    fn with_mut_panic_rolls_back_and_handle_stays_usable() {
        let (_tmp, handle) = create_test_handle();
        let (_replacement_tmp, replacement_handle) = create_test_handle();
        let replacement = Dataset::clone(&*replacement_handle.snapshot());
        let uri_before = handle.snapshot().uri().to_string();
        assert_ne!(replacement.uri(), uri_before);

        let result = catch_unwind(AssertUnwindSafe(|| {
            handle.with_mut(|ds| {
                // Make a visible in-memory mutation before panicking. This
                // distinguishes clone-execute-swap from mutating the handle's
                // stored Dataset in place and merely skipping the final swap.
                *ds = replacement;
                panic!("simulated bug in mutation")
            })
        }));
        let payload = result.expect_err("panic must escape with_mut unchanged");
        let msg = crate::error::panic_payload_message(&*payload);
        assert!(
            msg.contains("simulated bug in mutation"),
            "original payload must be preserved, got: {msg}"
        );

        // The unwind poisoned the RwLock; poison-tolerant reads must still
        // return the pre-panic dataset (the swap never happened, so the
        // in-memory state was rolled back).
        let snap = handle.snapshot();
        assert_eq!(snap.version().version, 1);
        assert_eq!(snap.uri(), uri_before);
        assert_eq!(block_on(snap.count_rows(None)).unwrap(), 3);

        // A later mutation on the same handle still works (poison-tolerant
        // write path) and is again visible via snapshot.
        let result = handle.with_mut(|ds| block_on(ds.delete("id = 1")).unwrap());
        assert_eq!(result.num_deleted_rows, 1);
        let snap = handle.snapshot();
        assert_eq!(snap.version().version, 2);
        assert_eq!(block_on(snap.count_rows(None)).unwrap(), 2);
    }
}
