// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Blob v2 C API: take per-row blob handles and read their bytes.
//!
//! A [`LanceBlobFile`] wraps an upstream `BlobFile`, so it stays usable after
//! the dataset handle is closed. [`lance_blob_file_read`] and
//! [`lance_blob_file_read_up_to`] advance the cursor,
//! [`lance_blob_file_read_range`] and [`lance_blob_file_read_ranges`] do not.

use std::ffi::c_char;
use std::ops::Range;
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

/// One blob-relative read range for [`lance_blob_file_read_ranges`].
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LanceBlobRange {
    /// Byte offset from the start of the blob.
    pub offset: u64,
    /// Number of bytes to read.
    pub length: u64,
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

/// Take blob handles by row address. See `lance.h` for the full contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_take_blobs_by_addresses(
    dataset: *const LanceDataset,
    row_addresses: *const u64,
    num_row_addresses: usize,
    column: *const c_char,
    out: *mut *mut LanceBlobFile,
) -> i32 {
    ffi_try!(
        unsafe {
            dataset_take_blobs_by_addresses_inner(
                dataset,
                row_addresses,
                num_row_addresses,
                column,
                out,
            )
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

    unsafe { store_blob_handles(blobs, num_ids, out)? };
    Ok(0)
}

unsafe fn dataset_take_blobs_by_addresses_inner(
    dataset: *const LanceDataset,
    row_addresses: *const u64,
    num_row_addresses: usize,
    column: *const c_char,
    out: *mut *mut LanceBlobFile,
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
    if num_row_addresses > 0 && row_addresses.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "row_addresses must not be NULL when num_row_addresses = {num_row_addresses}"
        )));
    }
    let column =
        unsafe { helpers::parse_c_string(column)? }.expect("column was checked for NULL above");
    let row_addresses = if num_row_addresses == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(row_addresses, num_row_addresses) }
    };

    // Unlike the older take entry points, empty selections still reach native
    // column validation and keep native error classification.
    let snapshot = unsafe { &*dataset }.snapshot();
    let blobs = block_on(snapshot.take_blobs_by_addresses(row_addresses, column))?;

    unsafe { store_blob_handles(blobs, num_row_addresses, out)? };
    Ok(0)
}

/// Move native results into caller-owned slots only after the complete call
/// succeeds. On error no slot is written.
unsafe fn store_blob_handles(
    blobs: Vec<Option<BlobFile>>,
    expected: usize,
    out: *mut *mut LanceBlobFile,
) -> Result<()> {
    if blobs.len() != expected {
        return Err(lance_core::Error::internal(format!(
            "expected {expected} blob handles, got {}",
            blobs.len()
        )));
    }

    for (i, blob) in blobs.into_iter().enumerate() {
        let handle = match blob {
            Some(inner) => Box::into_raw(Box::new(LanceBlobFile { inner })),
            None => ptr::null_mut(),
        };
        unsafe { ptr::write_unaligned(out.add(i), handle) };
    }
    Ok(())
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
        // The read stops at the blob end, so `bytes` fits in the `dst_len`
        // checked above; size the slice to what is actually written.
        let dst = unsafe { std::slice::from_raw_parts_mut(dst, bytes.len()) };
        dst.copy_from_slice(&bytes);
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
        // Upstream caps the read at `len`; size the slice to what is actually
        // written.
        let dst = unsafe { std::slice::from_raw_parts_mut(dst, bytes.len()) };
        dst.copy_from_slice(&bytes);
    }
    unsafe { ptr::write_unaligned(bytes_read, bytes.len()) };
    Ok(0)
}

/// Read multiple ranges without moving the cursor. See `lance.h` for the full
/// contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_blob_file_read_ranges(
    blob: *const LanceBlobFile,
    ranges: *const LanceBlobRange,
    num_ranges: usize,
    destinations: *const *mut u8,
    destination_lens: *const usize,
) -> i32 {
    ffi_try!(
        unsafe {
            blob_file_read_ranges_inner(blob, ranges, num_ranges, destinations, destination_lens)
        },
        neg
    )
}

unsafe fn blob_file_read_ranges_inner(
    blob: *const LanceBlobFile,
    ranges: *const LanceBlobRange,
    num_ranges: usize,
    destinations: *const *mut u8,
    destination_lens: *const usize,
) -> Result<i32> {
    let handle = unsafe { blob_ref(blob)? };
    if num_ranges > 0 {
        if ranges.is_null() {
            return Err(lance_core::Error::invalid_input(format!(
                "ranges must not be NULL when num_ranges = {num_ranges}"
            )));
        }
        if destinations.is_null() {
            return Err(lance_core::Error::invalid_input(format!(
                "destinations must not be NULL when num_ranges = {num_ranges}"
            )));
        }
        if destination_lens.is_null() {
            return Err(lance_core::Error::invalid_input(format!(
                "destination_lens must not be NULL when num_ranges = {num_ranges}"
            )));
        }
    }

    let range_specs = if num_ranges == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(ranges, num_ranges) }
    };
    let destinations = if num_ranges == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(destinations, num_ranges) }
    };
    let destination_lens = if num_ranges == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(destination_lens, num_ranges) }
    };

    let mut native_ranges: Vec<Range<u64>> = Vec::with_capacity(num_ranges);
    let mut requested_lens = Vec::with_capacity(num_ranges);
    for (index, range) in range_specs.iter().enumerate() {
        let end = range.offset.checked_add(range.length).ok_or_else(|| {
            lance_core::Error::invalid_input(format!(
                "ranges[{index}].offset {} plus length {} overflows a 64-bit byte range",
                range.offset, range.length
            ))
        })?;
        let requested_len = usize::try_from(range.length).map_err(|_| {
            lance_core::Error::invalid_input(format!(
                "ranges[{index}].length {} exceeds usize::MAX on this platform",
                range.length
            ))
        })?;
        if requested_len > 0 && destinations[index].is_null() {
            return Err(lance_core::Error::invalid_input(format!(
                "destinations[{index}] must not be NULL when ranges[{index}].length = {requested_len}"
            )));
        }
        if destination_lens[index] < requested_len {
            return Err(lance_core::Error::invalid_input(format!(
                "destination_lens[{index}] = {} is smaller than ranges[{index}].length = {requested_len}",
                destination_lens[index]
            )));
        }

        native_ranges.push(range.offset..end);
        requested_lens.push(requested_len);
    }

    // Native validates the closed state, blob bounds, and the complete range
    // list before any caller buffer is touched.
    let buffers = block_on(handle.inner.read_ranges(&native_ranges))?;
    if buffers.len() != num_ranges {
        return Err(lance_core::Error::internal(format!(
            "expected {num_ranges} range buffers, got {}",
            buffers.len()
        )));
    }
    for (index, buffer) in buffers.iter().enumerate() {
        if buffer.len() != requested_lens[index] {
            return Err(lance_core::Error::internal(format!(
                "range {index} requested {} bytes but native returned {}",
                requested_lens[index],
                buffer.len()
            )));
        }
    }

    for (index, buffer) in buffers.iter().enumerate() {
        if buffer.is_empty() {
            continue;
        }
        let destination =
            unsafe { std::slice::from_raw_parts_mut(destinations[index], buffer.len()) };
        destination.copy_from_slice(buffer);
    }
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

/// Set the sequential read-ahead size. See `lance.h` for the full contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_blob_file_set_buffer_size(
    blob: *mut LanceBlobFile,
    buffer_size: usize,
) -> i32 {
    ffi_try!(
        unsafe { blob_file_set_buffer_size_inner(blob, buffer_size) },
        neg
    )
}

unsafe fn blob_file_set_buffer_size_inner(
    blob: *const LanceBlobFile,
    buffer_size: usize,
) -> Result<i32> {
    let handle = unsafe { blob_ref(blob)? };
    block_on(handle.inner.set_buffer_size(buffer_size))?;
    Ok(0)
}

/// Mark the native blob file closed. See `lance.h` for the full contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_blob_file_mark_closed(blob: *mut LanceBlobFile) -> i32 {
    ffi_try!(unsafe { blob_file_mark_closed_inner(blob) }, neg)
}

unsafe fn blob_file_mark_closed_inner(blob: *const LanceBlobFile) -> Result<i32> {
    let handle = unsafe { blob_ref(blob)? };
    block_on(handle.inner.close())?;
    Ok(0)
}

/// Report the native blob file's closed state. See `lance.h` for the full
/// contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_blob_file_is_closed(
    blob: *const LanceBlobFile,
    out_closed: *mut bool,
) -> i32 {
    ffi_try!(unsafe { blob_file_is_closed_inner(blob, out_closed) }, neg)
}

unsafe fn blob_file_is_closed_inner(
    blob: *const LanceBlobFile,
    out_closed: *mut bool,
) -> Result<i32> {
    let handle = unsafe { blob_ref(blob)? };
    if out_closed.is_null() {
        return Err(lance_core::Error::invalid_input(
            "out_closed must not be NULL",
        ));
    }
    let closed = block_on(handle.inner.is_closed());
    unsafe { ptr::write_unaligned(out_closed, closed) };
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

#[cfg(test)]
mod tests {
    use super::*;

    use std::ffi::CStr;
    use std::num::NonZeroUsize;
    use std::sync::Arc;

    use arrow_array::{RecordBatch, RecordBatchIterator, UInt32Array};
    use arrow_schema::{DataType, Field, Schema};
    use lance::dataset::WriteParams;
    use lance::{BlobArrayBuilder, BlobFieldOptions, Dataset, blob_field_with_options};
    use lance_file::version::LanceFileVersion;

    fn take_last_error_message() -> String {
        let message = crate::lance_last_error_message();
        if message.is_null() {
            return String::new();
        }
        let owned = unsafe { CStr::from_ptr(message) }
            .to_string_lossy()
            .into_owned();
        unsafe { crate::lance_free_string(message) };
        owned
    }

    fn write_prefetch_fixture(uri: &str) {
        let schema = Arc::new(Schema::new(vec![
            Field::new("id", DataType::UInt32, false),
            blob_field_with_options(
                "blob",
                true,
                BlobFieldOptions {
                    inline_size_threshold: Some(16),
                    dedicated_size_threshold: NonZeroUsize::new(256),
                },
            ),
        ]));
        let mut blobs = BlobArrayBuilder::new(1);
        blobs
            .push_bytes((0..128).map(|index| (index * 7 + 3) as u8).collect())
            .unwrap();
        let batch = RecordBatch::try_new(
            schema.clone(),
            vec![
                Arc::new(UInt32Array::from(vec![0])),
                blobs.finish().unwrap(),
            ],
        )
        .unwrap();

        block_on(async {
            Dataset::write(
                RecordBatchIterator::new(vec![Ok(batch)], schema),
                uri,
                Some(WriteParams {
                    data_storage_version: Some(LanceFileVersion::V2_2),
                    ..Default::default()
                }),
            )
            .await
            .unwrap();
        });
    }

    #[test]
    fn test_blob_file_set_buffer_size_controls_prefetch() {
        let tmp = tempfile::tempdir().unwrap();
        let uri = tmp
            .path()
            .join("prefetch_blob")
            .to_str()
            .unwrap()
            .to_string();
        write_prefetch_fixture(&uri);

        let uri = std::ffi::CString::new(uri).unwrap();
        let dataset = unsafe { crate::lance_dataset_open(uri.as_ptr(), ptr::null(), 0) };
        assert!(!dataset.is_null(), "{}", take_last_error_message());

        let indices = [0u64];
        let mut handle = ptr::null_mut();
        assert_eq!(
            unsafe {
                lance_dataset_take_blobs_by_indices(
                    dataset,
                    indices.as_ptr(),
                    indices.len(),
                    c"blob".as_ptr(),
                    &mut handle,
                )
            },
            0,
            "{}",
            take_last_error_message()
        );
        assert!(!handle.is_null());

        let blob = unsafe { &*handle };
        assert_eq!(blob.inner.range_submission_count(), 0);

        assert_eq!(unsafe { lance_blob_file_set_buffer_size(handle, 64) }, 0);
        let mut first = [0u8; 32];
        let mut bytes_read = 0usize;
        assert_eq!(
            unsafe {
                lance_blob_file_read_up_to(handle, first.as_mut_ptr(), first.len(), &mut bytes_read)
            },
            0,
            "{}",
            take_last_error_message()
        );
        assert_eq!(bytes_read, first.len());
        assert_eq!(
            blob.inner.range_submission_count(),
            1,
            "the first buffered read should submit one source range request"
        );

        let mut second = [0u8; 32];
        assert_eq!(
            unsafe {
                lance_blob_file_read_up_to(
                    handle,
                    second.as_mut_ptr(),
                    second.len(),
                    &mut bytes_read,
                )
            },
            0
        );
        assert_eq!(bytes_read, second.len());
        assert_eq!(
            blob.inner.range_submission_count(),
            1,
            "a read fully served by prefetch must not submit another range request"
        );

        let mut one = [0u8; 1];
        assert_eq!(
            unsafe {
                lance_blob_file_read_up_to(handle, one.as_mut_ptr(), one.len(), &mut bytes_read)
            },
            0
        );
        assert_eq!(
            blob.inner.range_submission_count(),
            2,
            "reading past the prefetched span must submit a new range request"
        );

        assert_eq!(unsafe { lance_blob_file_set_buffer_size(handle, 0) }, 0);
        let mut disabled = [0u8; 32];
        for expected_submissions in 3..=4 {
            assert_eq!(
                unsafe {
                    lance_blob_file_read_up_to(
                        handle,
                        disabled.as_mut_ptr(),
                        disabled.len(),
                        &mut bytes_read,
                    )
                },
                0
            );
            assert_eq!(
                blob.inner.range_submission_count(),
                expected_submissions,
                "with buffer_size 0 every sequential read submits one range request"
            );
        }

        assert_eq!(unsafe { lance_blob_file_set_buffer_size(handle, 64) }, 0);
        assert_eq!(unsafe { lance_blob_file_seek(handle, 0) }, 0);
        assert_eq!(
            unsafe {
                lance_blob_file_read_up_to(
                    handle,
                    disabled.as_mut_ptr(),
                    disabled.len(),
                    &mut bytes_read,
                )
            },
            0
        );
        assert_eq!(blob.inner.range_submission_count(), 5);

        assert_eq!(unsafe { lance_blob_file_set_buffer_size(handle, 256) }, 0);
        assert_eq!(
            unsafe {
                lance_blob_file_read_up_to(
                    handle,
                    disabled.as_mut_ptr(),
                    disabled.len(),
                    &mut bytes_read,
                )
            },
            0
        );
        assert_eq!(
            blob.inner.range_submission_count(),
            6,
            "changing buffer_size drops unused prefetch, as native does"
        );

        unsafe { lance_blob_file_close(handle) };
        unsafe { crate::lance_dataset_close(dataset) };
    }
}
