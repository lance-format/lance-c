// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Read-only fragment snapshots.
//!
//! A [`LanceFragment`] owns Lance's `FileFragment`, which in turn owns an
//! `Arc<Dataset>` for the exact dataset snapshot it was resolved from. The
//! handle therefore remains valid after the source dataset handle is closed or
//! mutated.

use std::ffi::{CString, c_char};
use std::ptr;

use arrow::ffi::FFI_ArrowSchema;
use arrow_schema::Schema as ArrowSchema;
use lance::dataset::fragment::FileFragment;
use lance_core::{Error, Result};

use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
use crate::runtime::block_on;

/// Opaque handle to one read-only fragment from a dataset snapshot.
pub struct LanceFragment {
    inner: FileFragment,
}

unsafe fn fragment_ref<'a>(fragment: *const LanceFragment) -> Result<&'a LanceFragment> {
    if fragment.is_null() {
        return Err(Error::invalid_input_source(
            "fragment must not be NULL".into(),
        ));
    }
    Ok(unsafe { &*fragment })
}

/// Resolve a fragment by ID from the dataset's current snapshot.
///
/// The returned handle retains the native fragment's dataset snapshot
/// ownership, so it remains valid after `dataset` is closed or mutated.
///
/// Returns NULL on error. A missing fragment ID sets `LANCE_ERR_NOT_FOUND`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_open_fragment(
    dataset: *const LanceDataset,
    fragment_id: u64,
) -> *mut LanceFragment {
    ffi_try!(unsafe { open_fragment_inner(dataset, fragment_id) }, null)
}

unsafe fn open_fragment_inner(
    dataset: *const LanceDataset,
    fragment_id: u64,
) -> Result<*mut LanceFragment> {
    if dataset.is_null() {
        return Err(Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    let dataset = unsafe { &*dataset }.snapshot();
    let fragment_id = usize::try_from(fragment_id).map_err(|_| {
        Error::invalid_input(format!(
            "fragment_id {fragment_id} exceeds the platform usize range"
        ))
    })?;
    let inner = dataset.get_fragment(fragment_id).ok_or_else(|| {
        Error::not_found(format!(
            "fragment_id {fragment_id} does not exist in dataset version {}",
            dataset.manifest().version
        ))
    })?;
    Ok(Box::into_raw(Box::new(LanceFragment { inner })))
}

/// Close and free a fragment handle. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_close(fragment: *mut LanceFragment) {
    if !fragment.is_null() {
        swallow_unwind("lance_fragment_close", || unsafe {
            let _ = Box::from_raw(fragment);
        });
    }
}

/// Return the fragment ID. Returns 0 and sets an error for a NULL handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_id(fragment: *const LanceFragment) -> u64 {
    ffi_try!(
        unsafe { fragment_ref(fragment).map(|fragment| fragment.inner.id() as u64) },
        0
    )
}

/// Return the number of native base data files in the fragment.
///
/// This does not include overlay files or the deletion file. Returns 0 and
/// sets an error for a NULL handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_data_file_count(fragment: *const LanceFragment) -> u64 {
    ffi_try!(
        unsafe { fragment_ref(fragment).map(|fragment| fragment.inner.num_data_files() as u64) },
        0
    )
}

/// Serialize the native fragment metadata as JSON.
///
/// The JSON is the serialization of Lance's `Fragment` value. It includes
/// every native field, including base data files, overlays, deletion-file
/// metadata, optional row-id/version metadata, and `physical_rows`. Optional
/// unknown values remain null or omitted as defined by Lance's serializer;
/// they are not replaced with zero.
///
/// The caller must free the returned string with `lance_free_string()`.
/// Returns NULL and sets an error for a NULL handle or serialization failure.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_metadata_json(
    fragment: *const LanceFragment,
) -> *const c_char {
    ffi_try!(unsafe { metadata_json_inner(fragment) }, null)
}

unsafe fn metadata_json_inner(fragment: *const LanceFragment) -> Result<*const c_char> {
    let fragment = unsafe { fragment_ref(fragment) }?;
    let json = serde_json::to_string(fragment.inner.metadata()).map_err(|error| {
        Error::internal(format!(
            "failed to serialize metadata for fragment {}: {error}",
            fragment.inner.id()
        ))
    })?;
    CString::new(json)
        .map(|value| value.into_raw() as *const c_char)
        .map_err(|error| {
            Error::internal(format!(
                "fragment {} metadata JSON contains an embedded NUL: {error}",
                fragment.inner.id()
            ))
        })
}

/// Export the fragment's logical dataset schema via Arrow C Data Interface.
///
/// This is Lance's logical dataset schema, not a physical file schema. `out`
/// is caller-allocated; its non-NULL release callback must be invoked exactly
/// once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_schema(
    fragment: *const LanceFragment,
    out: *mut FFI_ArrowSchema,
) -> i32 {
    ffi_try!(unsafe { fragment_schema_inner(fragment, out) }, neg)
}

unsafe fn fragment_schema_inner(
    fragment: *const LanceFragment,
    out: *mut FFI_ArrowSchema,
) -> Result<i32> {
    let fragment = unsafe { fragment_ref(fragment) }?;
    if out.is_null() {
        return Err(Error::invalid_input_source("out must not be NULL".into()));
    }
    let arrow_schema: ArrowSchema = fragment.inner.schema().into();
    let ffi_schema = FFI_ArrowSchema::try_from(&arrow_schema)?;
    unsafe {
        ptr::write_unaligned(out, ffi_schema);
    }
    Ok(0)
}

unsafe fn require_row_output(fragment: *const LanceFragment, out: *mut u64) -> Result<()> {
    unsafe { fragment_ref(fragment) }?;
    if out.is_null() {
        return Err(Error::invalid_input_source(
            "out_rows must not be NULL".into(),
        ));
    }
    Ok(())
}

/// Read the fragment's physical row count, including deleted rows.
///
/// This calls Lance's `FileFragment::physical_rows`, which may read a data
/// file when the manifest has no trustworthy cached count. Native errors are
/// propagated. `out_rows` is written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_physical_rows(
    fragment: *const LanceFragment,
    out_rows: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { fragment_physical_rows_inner(fragment, out_rows) },
        neg
    )
}

unsafe fn fragment_physical_rows_inner(
    fragment: *const LanceFragment,
    out_rows: *mut u64,
) -> Result<i32> {
    unsafe { require_row_output(fragment, out_rows) }?;
    let fragment = unsafe { &*fragment };
    let rows = block_on(fragment.inner.physical_rows())?;
    unsafe {
        ptr::write_unaligned(out_rows, rows as u64);
    }
    Ok(0)
}

/// Read the fragment's deleted-row count.
///
/// This calls Lance's `FileFragment::count_deletions`, which may load the
/// deletion vector when its count is not cached in the manifest. Native errors
/// are propagated. `out_rows` is written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_deleted_rows(
    fragment: *const LanceFragment,
    out_rows: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { fragment_deleted_rows_inner(fragment, out_rows) },
        neg
    )
}

unsafe fn fragment_deleted_rows_inner(
    fragment: *const LanceFragment,
    out_rows: *mut u64,
) -> Result<i32> {
    unsafe { require_row_output(fragment, out_rows) }?;
    let fragment = unsafe { &*fragment };
    let rows = block_on(fragment.inner.count_deletions())?;
    unsafe {
        ptr::write_unaligned(out_rows, rows as u64);
    }
    Ok(0)
}

/// Read the fragment's logical row count.
///
/// This calls Lance's `FileFragment::count_rows(None)`. The binding does not
/// subtract the deletion count itself, so Native's legacy handling and errors
/// remain authoritative. Native errors are propagated, and `out_rows` is
/// written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_logical_rows(
    fragment: *const LanceFragment,
    out_rows: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { fragment_logical_rows_inner(fragment, out_rows) },
        neg
    )
}

unsafe fn fragment_logical_rows_inner(
    fragment: *const LanceFragment,
    out_rows: *mut u64,
) -> Result<i32> {
    unsafe { require_row_output(fragment, out_rows) }?;
    let fragment = unsafe { &*fragment };
    let rows = block_on(fragment.inner.count_rows(None))?;
    unsafe {
        ptr::write_unaligned(out_rows, rows as u64);
    }
    Ok(0)
}
