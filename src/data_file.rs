// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Owned native data-file metadata.

use std::ffi::{CString, c_char};
use std::ptr;

use lance_core::Result;
use lance_table::format::DataFile;

use crate::error::{ffi_try, swallow_unwind};

/// Owned native `DataFile` snapshot.
///
/// The handle owns native paths, field IDs, column indices, file versions,
/// optional size and optional base ID. It remains valid after the dataset or
/// operation that produced it is closed.
pub struct LanceDataFile {
    inner: DataFile,
    path: CString,
}

impl LanceDataFile {
    pub(crate) fn from_native(inner: DataFile) -> Result<Self> {
        let path = CString::new(inner.path.as_str()).map_err(|_| {
            lance_core::Error::invalid_input(format!(
                "data file path contains an embedded NUL byte: {:?}",
                inner.path
            ))
        })?;
        Ok(Self { inner, path })
    }

    pub(crate) fn native(&self) -> &DataFile {
        &self.inner
    }
}

fn data_file_at<'a>(file: *const LanceDataFile) -> Result<&'a LanceDataFile> {
    if file.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "data file must not be NULL".into(),
        ));
    }
    Ok(unsafe { &*file })
}

fn required_output<T>(out: *mut T, label: &str) -> Result<()> {
    if out.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            format!("{label} must not be NULL").into(),
        ));
    }
    Ok(())
}

/// Close a data-file handle. NULL-safe. A non-NULL handle must be closed
/// exactly once and must not be used afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_file_close(file: *mut LanceDataFile) {
    if !file.is_null() {
        swallow_unwind("lance_data_file_close", || unsafe {
            let _ = Box::from_raw(file);
        });
    }
}

/// Clone a data-file handle into an independent snapshot.
///
/// Returns NULL on error. The caller closes the returned handle exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_file_clone(file: *const LanceDataFile) -> *mut LanceDataFile {
    ffi_try!(unsafe { data_file_clone_inner(file) }, null)
}

unsafe fn data_file_clone_inner(file: *const LanceDataFile) -> Result<*mut LanceDataFile> {
    let file = data_file_at(file)?;
    let clone = LanceDataFile::from_native(file.inner.clone())?;
    Ok(Box::into_raw(Box::new(clone)))
}

/// Return the native data-file path.
///
/// The pointer is borrowed from `file` and remains valid until the handle is
/// closed. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_file_path(file: *const LanceDataFile) -> *const c_char {
    ffi_try!(unsafe { data_file_path_inner(file) }, null)
}

unsafe fn data_file_path_inner(file: *const LanceDataFile) -> Result<*const c_char> {
    Ok(data_file_at(file)?.path.as_ptr())
}

/// Read the number of native field IDs in this data file.
///
/// Returns 0 on error; distinguish an empty list from failure with
/// `lance_last_error_code()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_file_field_count(file: *const LanceDataFile) -> usize {
    ffi_try!(Ok(data_file_at(file)?.inner.fields.len()), 0)
}

/// Read one native field ID.
///
/// Returns 0 on success, -1 on error. `out_field_id` is written only on
/// success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_file_field_id_at(
    file: *const LanceDataFile,
    index: usize,
    out_field_id: *mut i32,
) -> i32 {
    ffi_try!(
        unsafe { data_file_field_id_at_inner(file, index, out_field_id) },
        neg
    )
}

unsafe fn data_file_field_id_at_inner(
    file: *const LanceDataFile,
    index: usize,
    out_field_id: *mut i32,
) -> Result<i32> {
    required_output(out_field_id, "out_field_id")?;
    let file = data_file_at(file)?;
    let field_id = file.inner.fields.get(index).ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!(
                "field index {index} is out of range for {} fields",
                file.inner.fields.len()
            )
            .into(),
        )
    })?;
    unsafe {
        ptr::write(out_field_id, *field_id);
    }
    Ok(0)
}

/// Read the number of native column indices in this data file.
///
/// Returns 0 on error; distinguish an empty list from failure with
/// `lance_last_error_code()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_file_column_index_count(file: *const LanceDataFile) -> usize {
    ffi_try!(Ok(data_file_at(file)?.inner.column_indices.len()), 0)
}

/// Read one native column index, including the native `-1` marker.
///
/// Returns 0 on success, -1 on error. `out_column_index` is written only on
/// success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_file_column_index_at(
    file: *const LanceDataFile,
    index: usize,
    out_column_index: *mut i32,
) -> i32 {
    ffi_try!(
        unsafe { data_file_column_index_at_inner(file, index, out_column_index) },
        neg
    )
}

unsafe fn data_file_column_index_at_inner(
    file: *const LanceDataFile,
    index: usize,
    out_column_index: *mut i32,
) -> Result<i32> {
    required_output(out_column_index, "out_column_index")?;
    let file = data_file_at(file)?;
    let column_index = file.inner.column_indices.get(index).ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!(
                "column index {index} is out of range for {} column indices",
                file.inner.column_indices.len()
            )
            .into(),
        )
    })?;
    unsafe {
        ptr::write(out_column_index, *column_index);
    }
    Ok(0)
}

/// Read the native data-file format version.
///
/// Returns 0 on success, -1 on error. Both output pointers are required and
/// written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_file_file_version(
    file: *const LanceDataFile,
    out_major: *mut u32,
    out_minor: *mut u32,
) -> i32 {
    ffi_try!(
        unsafe { data_file_file_version_inner(file, out_major, out_minor) },
        neg
    )
}

unsafe fn data_file_file_version_inner(
    file: *const LanceDataFile,
    out_major: *mut u32,
    out_minor: *mut u32,
) -> Result<i32> {
    required_output(out_major, "out_major")?;
    required_output(out_minor, "out_minor")?;
    let file = data_file_at(file)?;
    unsafe {
        ptr::write(out_major, file.inner.file_major_version);
        ptr::write(out_minor, file.inner.file_minor_version);
    }
    Ok(0)
}

/// Read the optional native data-file size.
///
/// Returns 0 on success, -1 on error. On success `*out_is_set` reports native
/// `Some`/`None`; `out_size` is written only when a size is present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_file_file_size(
    file: *const LanceDataFile,
    out_is_set: *mut bool,
    out_size: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { data_file_file_size_inner(file, out_is_set, out_size) },
        neg
    )
}

unsafe fn data_file_file_size_inner(
    file: *const LanceDataFile,
    out_is_set: *mut bool,
    out_size: *mut u64,
) -> Result<i32> {
    required_output(out_is_set, "out_is_set")?;
    required_output(out_size, "out_size")?;
    let file = data_file_at(file)?;
    match file.inner.file_size_bytes.get() {
        Some(size) => unsafe {
            ptr::write(out_is_set, true);
            ptr::write(out_size, size.get());
        },
        None => unsafe {
            ptr::write(out_is_set, false);
        },
    }
    Ok(0)
}

/// Read the optional native base ID.
///
/// Returns 0 on success, -1 on error. On success `*out_is_set` reports native
/// `Some`/`None`; `out_base_id` is written only when a base ID is present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_file_base_id(
    file: *const LanceDataFile,
    out_is_set: *mut bool,
    out_base_id: *mut u32,
) -> i32 {
    ffi_try!(
        unsafe { data_file_base_id_inner(file, out_is_set, out_base_id) },
        neg
    )
}

unsafe fn data_file_base_id_inner(
    file: *const LanceDataFile,
    out_is_set: *mut bool,
    out_base_id: *mut u32,
) -> Result<i32> {
    required_output(out_is_set, "out_is_set")?;
    required_output(out_base_id, "out_base_id")?;
    let file = data_file_at(file)?;
    match file.inner.base_id {
        Some(base_id) => unsafe {
            ptr::write(out_is_set, true);
            ptr::write(out_base_id, base_id);
        },
        None => unsafe {
            ptr::write(out_is_set, false);
        },
    }
    Ok(0)
}
