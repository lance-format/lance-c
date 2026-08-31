// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Dataset C API: open, close, metadata, schema, take.

use std::ffi::c_char;
use std::panic::{AssertUnwindSafe, catch_unwind, resume_unwind};
use std::sync::{Arc, RwLock};

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi_stream::FFI_ArrowArrayStream;
use arrow_schema::Schema as ArrowSchema;
use lance::Dataset;
use lance::dataset::builder::DatasetBuilder;
use lance_core::Result;

use crate::error::{ffi_try, swallow_unwind};
use crate::helpers;
use crate::runtime::block_on;
use crate::session::LanceSession;
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
        unsafe { open_dataset_inner(uri, storage_options, version, None) },
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
    unsafe { open_dataset_inner(uri, storage_options, version, Some(session)) }
}

unsafe fn open_dataset_inner(
    uri: *const c_char,
    storage_options: *const *const c_char,
    version: u64,
    session: Option<&LanceSession>,
) -> Result<*mut LanceDataset> {
    let uri_str = unsafe { helpers::parse_c_string(uri)? }
        .ok_or_else(|| lance_core::Error::invalid_input_source("uri must not be NULL".into()))?;

    let opts = unsafe { helpers::parse_storage_options(storage_options)? };

    let mut builder = DatasetBuilder::from_uri(uri_str);
    if !opts.is_empty() {
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

    use arrow_array::{Int32Array, RecordBatch, StringArray};
    use arrow_schema::{DataType, Field, Schema};

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
