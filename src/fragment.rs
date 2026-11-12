// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Fragment snapshots.
//!
//! A [`LanceFragment`] is either bound to Lance's `FileFragment`, which owns an
//! `Arc<Dataset>` for the exact snapshot it was resolved from, or detached with
//! only cloned native `Fragment` metadata. Bound handles remain valid after the
//! source dataset handle is closed or mutated and support dataset-dependent
//! reads. Detached handles support metadata inspection and operation
//! construction but reject reads that need a dataset.

use std::ffi::{CString, c_char};
use std::ptr;

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi_stream::FFI_ArrowArrayStream;
use arrow_schema::Schema as ArrowSchema;
use lance::dataset::fragment::FileFragment;
use lance_core::datatypes::Schema as LanceSchema;
use lance_core::{Error, Result};
use lance_table::format::Fragment;

use crate::data_file::LanceDataFile;
use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
use crate::helpers;
use crate::runtime::block_on;
use crate::scanner::LanceScanner;
use crate::stream_guard::guarded_ffi_stream_from_reader;

/// Internal fragment ownership state.
///
/// `Bound` retains the native dataset snapshot and supports dataset-dependent
/// reads. `Detached` owns only the cloned native metadata that operations carry,
/// so it can outlive every transaction and dataset handle that produced it.
pub(crate) enum FragmentState {
    Bound(FileFragment),
    Detached(Fragment),
}

/// Opaque handle to one fragment snapshot.
pub struct LanceFragment {
    inner: FragmentState,
}

impl LanceFragment {
    pub(crate) fn from_metadata(fragment: Fragment) -> Self {
        Self {
            inner: FragmentState::Detached(fragment),
        }
    }

    pub(crate) fn from_native(fragment: FileFragment) -> Self {
        Self {
            inner: FragmentState::Bound(fragment),
        }
    }

    pub(crate) fn metadata(&self) -> &Fragment {
        match &self.inner {
            FragmentState::Bound(fragment) => fragment.metadata(),
            FragmentState::Detached(fragment) => fragment,
        }
    }

    /// Return the dataset-bound fragment or reject a dataset-dependent read.
    pub(crate) fn bound(&self) -> Result<&FileFragment> {
        match &self.inner {
            FragmentState::Bound(fragment) => Ok(fragment),
            FragmentState::Detached(_) => Err(Error::not_supported_source(
                "fragment is detached; this operation requires a dataset-bound fragment".into(),
            )),
        }
    }

    pub(crate) fn schema(&self) -> Result<&LanceSchema> {
        Ok(self.bound()?.schema())
    }
}

pub(crate) unsafe fn fragment_ref<'a>(fragment: *const LanceFragment) -> Result<&'a LanceFragment> {
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
    Ok(Box::into_raw(Box::new(LanceFragment {
        inner: FragmentState::Bound(inner),
    })))
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

/// Construct detached native fragment metadata.
///
/// This starts from native `Fragment::new(fragment_id)`, clones the borrowed
/// data files in input order, and optionally sets `physical_rows`. Native
/// defaults are retained for deletion files, overlays, row IDs and row
/// version metadata. This constructor does not infer schema coverage or the
/// physical row count and adds no binding-specific consistency checks.
///
/// `data_files` may be NULL only when `data_file_count` is zero. Every element
/// of a non-NULL array must be non-NULL. `physical_rows` is optional; a
/// non-NULL value must fit the platform `usize`. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_new(
    fragment_id: u64,
    data_files: *const *const LanceDataFile,
    data_file_count: usize,
    physical_rows: *const u64,
) -> *mut LanceFragment {
    ffi_try!(
        unsafe { fragment_new_inner(fragment_id, data_files, data_file_count, physical_rows) },
        null
    )
}

unsafe fn fragment_new_inner(
    fragment_id: u64,
    data_files: *const *const LanceDataFile,
    data_file_count: usize,
    physical_rows: *const u64,
) -> Result<*mut LanceFragment> {
    let files = if data_files.is_null() {
        if data_file_count != 0 {
            return Err(Error::invalid_input_source(
                format!("data_files must not be NULL when data_file_count = {data_file_count}")
                    .into(),
            ));
        }
        Vec::new()
    } else {
        if data_file_count > isize::MAX as usize / std::mem::size_of::<*const LanceDataFile>() {
            return Err(Error::invalid_input_source(
                format!(
                    "data_file_count {data_file_count} exceeds the maximum addressable pointer slice length"
                )
                .into(),
            ));
        }
        if data_file_count
            > isize::MAX as usize / std::mem::size_of::<lance_table::format::DataFile>()
        {
            return Err(Error::invalid_input_source(
                format!(
                    "data_file_count {data_file_count} exceeds the maximum cloneable data-file count"
                )
                .into(),
            ));
        }
        let data_files = unsafe { std::slice::from_raw_parts(data_files, data_file_count) };
        let mut files = Vec::with_capacity(data_file_count);
        for (index, data_file) in data_files.iter().enumerate() {
            if data_file.is_null() {
                return Err(Error::invalid_input_source(
                    format!(
                        "data_files[{index}] must not be NULL when data_file_count = {data_file_count}"
                    )
                    .into(),
                ));
            }
            files.push(unsafe { &**data_file }.native().clone());
        }
        files
    };

    let physical_rows = if physical_rows.is_null() {
        None
    } else {
        let value = unsafe { ptr::read_unaligned(physical_rows) };
        Some(usize::try_from(value).map_err(|_| {
            Error::invalid_input(format!(
                "physical_rows {value} exceeds the platform usize range"
            ))
        })?)
    };

    let mut fragment = Fragment::new(fragment_id);
    fragment.files = files;
    fragment.physical_rows = physical_rows;
    Ok(Box::into_raw(Box::new(LanceFragment::from_metadata(
        fragment,
    ))))
}

/// Return the fragment ID. Returns 0 and sets an error for a NULL handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_id(fragment: *const LanceFragment) -> u64 {
    ffi_try!(
        unsafe { fragment_ref(fragment).map(|fragment| fragment.metadata().id) },
        0
    )
}

/// Return whether this handle retains a dataset-bound native fragment.
///
/// Detached handles own only cloned metadata, load no data files, and reject
/// dataset-dependent reads. `out_is_bound` is written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_is_bound(
    fragment: *const LanceFragment,
    out_is_bound: *mut bool,
) -> i32 {
    ffi_try!(
        unsafe { fragment_is_bound_inner(fragment, out_is_bound) },
        neg
    )
}

unsafe fn fragment_is_bound_inner(
    fragment: *const LanceFragment,
    out_is_bound: *mut bool,
) -> Result<i32> {
    let fragment = unsafe { fragment_ref(fragment) }?;
    if out_is_bound.is_null() {
        return Err(Error::invalid_input_source(
            "out_is_bound must not be NULL".into(),
        ));
    }
    let is_bound = matches!(fragment.inner, FragmentState::Bound(_));
    unsafe {
        ptr::write_unaligned(out_is_bound, is_bound);
    }
    Ok(0)
}

/// Return the dataset read version captured by a bound fragment.
///
/// This is the native dataset snapshot version carried by the fragment's
/// `FileFragment` and is the value callers pass to generic transaction commit
/// after staging fragment mutations. A detached metadata-only handle has no
/// dataset snapshot and returns `LANCE_ERR_NOT_SUPPORTED`. The output slot is
/// written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_read_version(
    fragment: *const LanceFragment,
    out_version: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { fragment_read_version_inner(fragment, out_version) },
        neg
    )
}

unsafe fn fragment_read_version_inner(
    fragment: *const LanceFragment,
    out_version: *mut u64,
) -> Result<i32> {
    let fragment = unsafe { fragment_ref(fragment) }?;
    if out_version.is_null() {
        return Err(Error::invalid_input_source(
            "out_version must not be NULL".into(),
        ));
    }
    let version = fragment.bound()?.dataset().version_id();
    unsafe {
        ptr::write_unaligned(out_version, version);
    }
    Ok(0)
}

/// Return the number of native base data files in the fragment.
///
/// This does not include overlay files or the deletion file. The count comes
/// from owned native metadata and is available for bound and detached handles.
/// Returns 0 and sets an error for a NULL handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_data_file_count(fragment: *const LanceFragment) -> u64 {
    ffi_try!(
        unsafe { fragment_ref(fragment).map(|fragment| fragment.metadata().files.len() as u64) },
        0
    )
}

/// Clone one native base data file from the fragment metadata.
///
/// `index` addresses the native `Fragment::files` array. The returned handle
/// owns an independent copy and must be closed with `lance_data_file_close()`.
/// A missing index sets `LANCE_ERR_INVALID_ARGUMENT` and leaves `out_file`
/// unchanged.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_data_file_at(
    fragment: *const LanceFragment,
    index: usize,
    out_file: *mut *mut LanceDataFile,
) -> i32 {
    ffi_try!(
        unsafe { fragment_data_file_at_inner(fragment, index, out_file) },
        neg
    )
}

unsafe fn fragment_data_file_at_inner(
    fragment: *const LanceFragment,
    index: usize,
    out_file: *mut *mut LanceDataFile,
) -> Result<i32> {
    let fragment = unsafe { fragment_ref(fragment) }?;
    if out_file.is_null() {
        return Err(Error::invalid_input_source(
            "out_file must not be NULL".into(),
        ));
    }
    let data_file = fragment.metadata().files.get(index).ok_or_else(|| {
        Error::invalid_input(format!(
            "data file index {index} is out of range for {} files",
            fragment.metadata().files.len()
        ))
    })?;
    let data_file = Box::new(LanceDataFile::from_native(data_file.clone())?);
    unsafe {
        ptr::write(out_file, Box::into_raw(data_file));
    }
    Ok(0)
}

/// Find the native base data file that stores a field ID.
///
/// The lookup follows native `FileFragment::data_file_for_field` for bound
/// handles and the same `fields` membership rule for detached metadata. A
/// missing field succeeds with `*out_file = NULL`; a negative `field_id` or a
/// NULL output slot is an invalid argument. The returned handle is independent
/// and must be closed with `lance_data_file_close()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_data_file_for_field(
    fragment: *const LanceFragment,
    field_id: i32,
    out_file: *mut *mut LanceDataFile,
) -> i32 {
    ffi_try!(
        unsafe { fragment_data_file_for_field_inner(fragment, field_id, out_file) },
        neg
    )
}

unsafe fn fragment_data_file_for_field_inner(
    fragment: *const LanceFragment,
    field_id: i32,
    out_file: *mut *mut LanceDataFile,
) -> Result<i32> {
    let fragment = unsafe { fragment_ref(fragment) }?;
    if out_file.is_null() {
        return Err(Error::invalid_input_source(
            "out_file must not be NULL".into(),
        ));
    }
    let field_id = u32::try_from(field_id)
        .map_err(|_| Error::invalid_input(format!("field_id {field_id} must be non-negative")))?;
    let data_file = match &fragment.inner {
        FragmentState::Bound(bound) => bound.data_file_for_field(field_id).cloned(),
        FragmentState::Detached(detached) => detached
            .files
            .iter()
            .find(|file| file.fields.contains(&(field_id as i32)))
            .cloned(),
    };
    let data_file = match data_file {
        Some(data_file) => Box::into_raw(Box::new(LanceDataFile::from_native(data_file)?)),
        None => ptr::null_mut(),
    };
    unsafe {
        ptr::write(out_file, data_file);
    }
    Ok(0)
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
    fragment_metadata_json(fragment.metadata())
}

pub(crate) fn fragment_metadata_json(fragment: &Fragment) -> Result<*const c_char> {
    let json = serde_json::to_string(fragment).map_err(|error| {
        Error::internal(format!(
            "failed to serialize metadata for fragment {}: {error}",
            fragment.id
        ))
    })?;
    CString::new(json)
        .map(|value| value.into_raw() as *const c_char)
        .map_err(|error| {
            Error::internal(format!(
                "fragment {} metadata JSON contains an embedded NUL: {error}",
                fragment.id
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
    let arrow_schema: ArrowSchema = fragment.bound()?.schema().into();
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
    let rows = block_on(fragment.bound()?.physical_rows())?;
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
    let rows = block_on(fragment.bound()?.count_deletions())?;
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
    let rows = block_on(fragment.bound()?.count_rows(None))?;
    unsafe {
        ptr::write_unaligned(out_rows, rows as u64);
    }
    Ok(0)
}

/// Start a scanner over this fragment's owned dataset snapshot.
///
/// This is the binding for `FileFragment::scan`. The returned scanner follows
/// the ordinary scanner lifecycle and configuration rules, with the fragment
/// selected as its initial scan scope.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_scan(fragment: *const LanceFragment) -> *mut LanceScanner {
    ffi_try!(unsafe { fragment_scan_inner(fragment) }, null)
}

unsafe fn fragment_scan_inner(fragment: *const LanceFragment) -> Result<*mut LanceScanner> {
    let fragment = unsafe { fragment_ref(fragment) }?;
    Ok(Box::into_raw(Box::new(LanceScanner::from_fragment(
        fragment.bound()?,
    ))))
}

/// Delete rows from this fragment without publishing a transaction.
///
/// The input fragment is borrowed and cloned before Lance's consuming
/// `FileFragment::delete` runs, so the input handle remains usable. A partial
/// deletion returns an independently owned fragment whose metadata carries the
/// staged deletion file. Deleting every row succeeds with NULL output. On any
/// error, `out_fragment` is unchanged.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_delete(
    fragment: *const LanceFragment,
    predicate: *const c_char,
    out_fragment: *mut *mut LanceFragment,
) -> i32 {
    ffi_try!(
        unsafe { fragment_delete_inner(fragment, predicate, out_fragment) },
        neg
    )
}

unsafe fn fragment_delete_inner(
    fragment: *const LanceFragment,
    predicate: *const c_char,
    out_fragment: *mut *mut LanceFragment,
) -> Result<i32> {
    let fragment = unsafe { fragment_ref(fragment) }?;
    if out_fragment.is_null() {
        return Err(Error::invalid_input_source(
            "out_fragment must not be NULL".into(),
        ));
    }
    let predicate = unsafe { helpers::parse_c_string(predicate)? }
        .ok_or_else(|| Error::invalid_input_source("predicate must not be NULL".into()))?;

    let updated = block_on(fragment.bound()?.clone().delete(predicate))?;
    let updated = match updated {
        Some(updated) => Box::into_raw(Box::new(LanceFragment {
            inner: FragmentState::Bound(updated),
        })),
        None => ptr::null_mut(),
    };
    unsafe {
        ptr::write(out_fragment, updated);
    }
    Ok(0)
}

fn fragment_take_projection(
    fragment: &LanceFragment,
    columns: Option<&[String]>,
) -> Result<LanceSchema> {
    let schema = fragment.bound()?.dataset().schema();
    match columns {
        Some(columns) => schema.project(columns).map_err(|error| {
            Error::invalid_input(format!(
                "invalid columns {columns:?} for fragment {}: {error}",
                fragment.metadata().id
            ))
        }),
        None => Ok(schema.clone()),
    }
}

/// Take fragment-local logical rows by index.
///
/// `indices` address logical rows in this fragment after deletion vectors are
/// resolved. They are not dataset `_rowid` values or physical row addresses.
/// Indices are forwarded unchanged to native `FileFragment::take`. Deletion
/// resolution, supported index ordering, duplicate handling, range validation,
/// projection, and JSON conversion follow the selected core. Lance 14.0.2's
/// v2 decoder can reject unsorted or duplicate indices.
///
/// `columns` is a NULL-terminated array. NULL selects the fragment's full
/// logical schema. A non-NULL array whose first entry is NULL is passed to
/// native take as an explicit empty projection, so its native failure behavior
/// is preserved rather than treated as a full projection. `indices` may be
/// NULL only when `num_indices` is zero.
/// On success `out` receives a caller-owned Arrow stream that remains valid
/// after the fragment and source dataset handles are closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_take(
    fragment: *const LanceFragment,
    indices: *const u32,
    num_indices: usize,
    columns: *const *const c_char,
    out: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe { fragment_take_inner(fragment, indices, num_indices, columns, out) },
        neg
    )
}

unsafe fn fragment_take_inner(
    fragment: *const LanceFragment,
    indices: *const u32,
    num_indices: usize,
    columns: *const *const c_char,
    out: *mut FFI_ArrowArrayStream,
) -> Result<i32> {
    let fragment = unsafe { fragment_ref(fragment) }?;
    if out.is_null() {
        return Err(Error::invalid_input_source("out must not be NULL".into()));
    }
    if num_indices > 0 && indices.is_null() {
        return Err(Error::invalid_input_source(
            format!("indices must not be NULL when num_indices = {num_indices}").into(),
        ));
    }

    let indices = if num_indices == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(indices, num_indices) }
    };
    let columns = unsafe { helpers::parse_c_string_array(columns)? };
    let projection = fragment_take_projection(fragment, columns.as_deref())?;

    let batch = block_on(fragment.bound()?.take(indices, &projection))?;
    let schema = batch.schema();
    let reader = arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema);
    let ffi_stream = guarded_ffi_stream_from_reader(reader)?;
    unsafe {
        ptr::write_unaligned(out, ffi_stream);
    }
    Ok(0)
}

/// Count this fragment's rows, optionally applying a native SQL filter.
///
/// NULL calls `FileFragment::count_rows(None)`, which uses the native physical
/// row and deletion counts. A non-NULL string, including an empty string, is
/// passed as `Some(filter)` without binding-side pruning. Native errors are
/// propagated and `out_rows` is written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_count_rows(
    fragment: *const LanceFragment,
    filter: *const c_char,
    out_rows: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { fragment_count_rows_inner(fragment, filter, out_rows) },
        neg
    )
}

unsafe fn fragment_count_rows_inner(
    fragment: *const LanceFragment,
    filter: *const c_char,
    out_rows: *mut u64,
) -> Result<i32> {
    unsafe { require_row_output(fragment, out_rows) }?;
    let fragment = unsafe { &*fragment };
    let filter = unsafe { helpers::parse_c_string(filter)? }.map(str::to_string);
    let rows = block_on(fragment.bound()?.count_rows(filter))?;
    unsafe {
        ptr::write_unaligned(out_rows, rows as u64);
    }
    Ok(0)
}
