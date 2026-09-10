// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Blob v2 C API: take per-row blob handles and read their bytes.
//!
//! A [`LanceBlobFile`] wraps an upstream `BlobFile`, so it stays usable after
//! the dataset handle is closed. [`lance_blob_file_read`] and
//! [`lance_blob_file_read_up_to`] advance the cursor,
//! [`lance_blob_file_read_range`] does not.

use std::ffi::c_char;
use std::ptr;

use lance::dataset::BlobFile;
use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
use crate::helpers;
use crate::runtime::block_on;

/// Opaque handle to one blob value; independent of the dataset handle.
pub struct LanceBlobFile {
    inner: BlobFile,
}

/// Row addressing used by a take entry point.
#[derive(Clone, Copy)]
enum TakeBy {
    /// `_rowid` values.
    RowIds,
    /// Zero-based row offsets.
    Indices,
}

impl TakeBy {
    /// C names of the identifier array and its count, for error messages.
    fn param_names(self) -> (&'static str, &'static str) {
        match self {
            Self::RowIds => ("row_ids", "num_row_ids"),
            Self::Indices => ("indices", "num_indices"),
        }
    }
}

// ---------------------------------------------------------------------------
// Taking blob handles
// ---------------------------------------------------------------------------

/// Take blob handles by dataset row ID. See `lance.h` for the full contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_take_blobs(
    dataset: *const LanceDataset,
    row_ids: *const u64,
    num_row_ids: usize,
    column: *const c_char,
    out: *mut *mut LanceBlobFile,
) -> i32 {
    ffi_try!(
        unsafe {
            dataset_take_blobs_inner(dataset, row_ids, num_row_ids, column, out, TakeBy::RowIds)
        },
        neg
    )
}

/// Take blob handles by row index (offset). See `lance.h` for the full contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_take_blobs_by_indices(
    dataset: *const LanceDataset,
    indices: *const u64,
    num_indices: usize,
    column: *const c_char,
    out: *mut *mut LanceBlobFile,
) -> i32 {
    ffi_try!(
        unsafe {
            dataset_take_blobs_inner(dataset, indices, num_indices, column, out, TakeBy::Indices)
        },
        neg
    )
}

unsafe fn dataset_take_blobs_inner(
    dataset: *const LanceDataset,
    ids: *const u64,
    num_ids: usize,
    column: *const c_char,
    out: *mut *mut LanceBlobFile,
    take_by: TakeBy,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input("dataset must not be NULL"));
    }
    if out.is_null() {
        return Err(lance_core::Error::invalid_input("out must not be NULL"));
    }
    if column.is_null() {
        return Err(lance_core::Error::invalid_input("column must not be NULL"));
    }
    if num_ids > 0 && ids.is_null() {
        let (ids_param, count_param) = take_by.param_names();
        return Err(lance_core::Error::invalid_input(format!(
            "{ids_param} must not be NULL when {count_param} = {num_ids}"
        )));
    }
    let column =
        unsafe { helpers::parse_c_string(column)? }.expect("column was checked for NULL above");

    // Nothing to take; `out` stays untouched.
    if num_ids == 0 {
        return Ok(0);
    }

    let ds = unsafe { &*dataset };
    let id_slice = unsafe { std::slice::from_raw_parts(ids, num_ids) };

    let snap = ds.snapshot();
    // Upstream reports an unknown column as FieldNotFound, which reaches C as
    // LANCE_ERR_INTERNAL; make it an invalid argument like the other take
    // entry points do. A non-blob column is already invalid input upstream.
    if snap.schema().field(column).is_none() {
        return Err(lance_core::Error::invalid_input(format!(
            "column '{column}' does not exist in the dataset schema"
        )));
    }
    let blobs = match take_by {
        TakeBy::RowIds => block_on(snap.take_blobs(id_slice, column))?,
        TakeBy::Indices => block_on(snap.take_blobs_by_indices(id_slice, column))?,
    };

    // Never report success with part of `out` unwritten.
    if blobs.len() != num_ids {
        return Err(lance_core::Error::internal(format!(
            "expected {num_ids} blob handles, got {}",
            blobs.len()
        )));
    }

    // Every failure above returns before `out` is touched.
    for (i, blob) in blobs.into_iter().enumerate() {
        let handle = match blob {
            Some(inner) => Box::into_raw(Box::new(LanceBlobFile { inner })),
            None => ptr::null_mut(),
        };
        unsafe { ptr::write_unaligned(out.add(i), handle) };
    }
    Ok(0)
}

// ---------------------------------------------------------------------------
// Blob handle accessors
// ---------------------------------------------------------------------------

/// Borrow a handle, rejecting NULL with a message naming the parameter.
unsafe fn blob_ref<'a>(blob: *const LanceBlobFile) -> Result<&'a LanceBlobFile> {
    if blob.is_null() {
        return Err(lance_core::Error::invalid_input("blob must not be NULL"));
    }
    Ok(unsafe { &*blob })
}

/// Return the blob size in bytes. See `lance.h` for the full contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_blob_file_size(blob: *const LanceBlobFile) -> u64 {
    ffi_try!(unsafe { blob_file_size_inner(blob) }, 0)
}

unsafe fn blob_file_size_inner(blob: *const LanceBlobFile) -> Result<u64> {
    Ok(unsafe { blob_ref(blob)? }.inner.size())
}

/// Read from the cursor to the end of the blob. See `lance.h` for the full
/// contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_blob_file_read(
    blob: *mut LanceBlobFile,
    dst: *mut u8,
    dst_len: usize,
) -> i32 {
    ffi_try!(unsafe { blob_file_read_inner(blob, dst, dst_len) }, neg)
}

unsafe fn blob_file_read_inner(
    blob: *mut LanceBlobFile,
    dst: *mut u8,
    dst_len: usize,
) -> Result<i32> {
    let handle = unsafe { blob_ref(blob)? };
    let size = handle.inner.size();
    let cursor = block_on(handle.inner.tell())?;
    let remaining = size.saturating_sub(cursor);

    if (dst_len as u64) < remaining {
        return Err(lance_core::Error::invalid_input(format!(
            "dst_len {dst_len} is smaller than the {remaining} bytes remaining from cursor {cursor} (blob size {size})"
        )));
    }
    if dst.is_null() && remaining > 0 {
        return Err(lance_core::Error::invalid_input(format!(
            "dst must not be NULL when {remaining} bytes remain from cursor {cursor} (blob size {size})"
        )));
    }

    let bytes = block_on(handle.inner.read())?;
    if !bytes.is_empty() {
        let dst = unsafe { std::slice::from_raw_parts_mut(dst, dst_len) };
        dst[..bytes.len()].copy_from_slice(&bytes);
    }
    Ok(0)
}

/// Read at most `len` bytes from the cursor. See `lance.h` for the full
/// contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_blob_file_read_up_to(
    blob: *mut LanceBlobFile,
    dst: *mut u8,
    len: usize,
    bytes_read: *mut usize,
) -> i32 {
    ffi_try!(
        unsafe { blob_file_read_up_to_inner(blob, dst, len, bytes_read) },
        neg
    )
}

unsafe fn blob_file_read_up_to_inner(
    blob: *mut LanceBlobFile,
    dst: *mut u8,
    len: usize,
    bytes_read: *mut usize,
) -> Result<i32> {
    let handle = unsafe { blob_ref(blob)? };
    if bytes_read.is_null() {
        return Err(lance_core::Error::invalid_input(
            "bytes_read must not be NULL",
        ));
    }
    if dst.is_null() && len > 0 {
        return Err(lance_core::Error::invalid_input(format!(
            "dst must not be NULL when len = {len}"
        )));
    }

    let bytes = block_on(handle.inner.read_up_to(len))?;
    if !bytes.is_empty() {
        let dst = unsafe { std::slice::from_raw_parts_mut(dst, len) };
        dst[..bytes.len()].copy_from_slice(&bytes);
    }
    unsafe { ptr::write_unaligned(bytes_read, bytes.len()) };
    Ok(0)
}

/// Read `len` bytes at `offset`, leaving the cursor alone. See `lance.h` for
/// the full contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_blob_file_read_range(
    blob: *const LanceBlobFile,
    offset: u64,
    dst: *mut u8,
    len: usize,
) -> i32 {
    ffi_try!(
        unsafe { blob_file_read_range_inner(blob, offset, dst, len) },
        neg
    )
}

unsafe fn blob_file_read_range_inner(
    blob: *const LanceBlobFile,
    offset: u64,
    dst: *mut u8,
    len: usize,
) -> Result<i32> {
    let handle = unsafe { blob_ref(blob)? };
    let end = offset.checked_add(len as u64).ok_or_else(|| {
        lance_core::Error::invalid_input(format!(
            "offset {offset} plus len {len} overflows a 64-bit byte range"
        ))
    })?;

    if len == 0 {
        return Ok(0);
    }
    if dst.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "dst must not be NULL when len = {len}"
        )));
    }

    // Bounds are checked upstream against the blob size.
    let bytes = block_on(handle.inner.read_range(offset..end))?;
    unsafe { std::slice::from_raw_parts_mut(dst, len) }.copy_from_slice(&bytes);
    Ok(0)
}

/// Move the cursor. See `lance.h` for the full contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_blob_file_seek(blob: *mut LanceBlobFile, pos: u64) -> i32 {
    ffi_try!(unsafe { blob_file_seek_inner(blob, pos) }, neg)
}

unsafe fn blob_file_seek_inner(blob: *mut LanceBlobFile, pos: u64) -> Result<i32> {
    let handle = unsafe { blob_ref(blob)? };
    // Seeking past the end is allowed, as upstream; reads then return nothing.
    block_on(handle.inner.seek(pos))?;
    Ok(0)
}

/// Report the cursor. See `lance.h` for the full contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_blob_file_tell(blob: *const LanceBlobFile, pos: *mut u64) -> i32 {
    ffi_try!(unsafe { blob_file_tell_inner(blob, pos) }, neg)
}

unsafe fn blob_file_tell_inner(blob: *const LanceBlobFile, pos: *mut u64) -> Result<i32> {
    let handle = unsafe { blob_ref(blob)? };
    if pos.is_null() {
        return Err(lance_core::Error::invalid_input("pos must not be NULL"));
    }
    let cursor = block_on(handle.inner.tell())?;
    unsafe { ptr::write_unaligned(pos, cursor) };
    Ok(0)
}

/// Close a blob handle. See `lance.h` for the full contract.
///
/// `swallow_unwind` rather than `ffi_try!`, so a pending error survives the
/// close. Dropping the `BlobFile` releases its resources; upstream `close()`
/// only sets a flag.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_blob_file_close(blob: *mut LanceBlobFile) {
    if blob.is_null() {
        return;
    }
    swallow_unwind("lance_blob_file_close", || unsafe {
        drop(Box::from_raw(blob));
    });
}
