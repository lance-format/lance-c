// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Planned whole-blob and blob-range reads exported as Arrow streams.
//!
//! The native builders plan against one dataset snapshot and produce one
//! [`lance::dataset::ReadBlob`] / [`lance::dataset::ReadBlobRange`] per
//! selected row or request. This module adapts each native result to one
//! one-row Arrow [`RecordBatch`] and exports the resulting incremental stream
//! through [`GuardedReader`].

use std::ffi::c_char;
use std::ptr;
use std::sync::atomic::AtomicBool;
use std::sync::{Arc, LazyLock};

use arrow::array::{ArrayRef, LargeBinaryArray, RecordBatch, UInt64Array};
use arrow::buffer::{Buffer, NullBuffer, OffsetBuffer, ScalarBuffer};
use arrow::ffi_stream::FFI_ArrowArrayStream;
use arrow_schema::{DataType, Field, Schema, SchemaRef};
use bytes::Bytes;
use futures::{Stream, StreamExt};
use lance::dataset::{
    BlobRangeRequest, ReadBlob, ReadBlobRange, ReadBlobRangesStream, ReadBlobsStream,
};
use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::{RT, block_on};
use crate::stream_guard::GuardedReader;

/// Selector kind used by both planned read factories.
///
/// The C ABI passes this as an `int32_t`; storing an out-of-range integer in a
/// Rust enum would be undefined behavior.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum BlobRowSelector {
    /// Leave the native builder without a selection.
    None,
    /// Stable `_rowid` values.
    Ids,
    /// Physical row addresses.
    Addresses,
    /// Zero-based logical row indices.
    Indices,
}

impl BlobRowSelector {
    fn from_raw(raw: i32) -> Result<Self> {
        match raw {
            0 => Ok(Self::None),
            1 => Ok(Self::Ids),
            2 => Ok(Self::Addresses),
            3 => Ok(Self::Indices),
            other => Err(lance_core::Error::invalid_input(format!(
                "invalid blob row selector {other}; expected 0 (none), 1 (ids), 2 (addresses), or 3 (indices)"
            ))),
        }
    }
}

/// Result ordering option for planned reads.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum BlobReadOrder {
    Preserve,
    Unordered,
}

impl BlobReadOrder {
    fn from_raw(raw: i32) -> Result<Self> {
        match raw {
            0 => Ok(Self::Preserve),
            1 => Ok(Self::Unordered),
            other => Err(lance_core::Error::invalid_input(format!(
                "invalid blob read order {other}; expected 0 (preserve) or 1 (unordered)"
            ))),
        }
    }

    fn preserve_order(self) -> bool {
        matches!(self, Self::Preserve)
    }
}

/// Optional scheduler I/O buffer and ordering controls for planned reads.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceBlobReadOptions {
    /// Whether `io_buffer_size` is present. An explicit zero is forwarded to
    /// native and rejected there.
    pub has_io_buffer_size: bool,
    /// Native scheduler I/O buffer size in bytes.
    pub io_buffer_size: u64,
    /// `0` preserves requested order; `1` permits unordered completion.
    pub order: i32,
}

impl Default for LanceBlobReadOptions {
    fn default() -> Self {
        Self {
            has_io_buffer_size: false,
            io_buffer_size: 0,
            order: 0,
        }
    }
}

/// One row-specific blob-range request.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceBlobRangeRequest {
    /// Row value interpreted by the selected row selector.
    pub row: u64,
    /// Byte offset from the beginning of the selected blob.
    pub offset: u64,
    /// Number of bytes to read.
    pub length: u64,
}

static WHOLE_BLOB_SCHEMA: LazyLock<SchemaRef> = LazyLock::new(|| {
    Arc::new(Schema::new(vec![
        Field::new("row_address", DataType::UInt64, false),
        Field::new("data", DataType::LargeBinary, true),
    ]))
});

static BLOB_RANGE_SCHEMA: LazyLock<SchemaRef> = LazyLock::new(|| {
    Arc::new(Schema::new(vec![
        Field::new("request_index", DataType::UInt64, false),
        Field::new("row_address", DataType::UInt64, false),
        Field::new("offset", DataType::UInt64, false),
        Field::new("length", DataType::UInt64, false),
        Field::new("data", DataType::LargeBinary, true),
    ]))
});

fn whole_blob_schema() -> SchemaRef {
    Arc::clone(&WHOLE_BLOB_SCHEMA)
}

fn blob_range_schema() -> SchemaRef {
    Arc::clone(&BLOB_RANGE_SCHEMA)
}

fn large_binary_row(data: Option<Bytes>) -> Result<LargeBinaryArray> {
    let (offsets, values, nulls) = match data {
        Some(bytes) => {
            let len = i64::try_from(bytes.len()).map_err(|_| {
                lance_core::Error::internal(format!(
                    "native blob payload length {} exceeds LargeBinary's i64 offset range",
                    bytes.len()
                ))
            })?;
            (
                OffsetBuffer::new(ScalarBuffer::from(vec![0_i64, len])),
                // arrow-rs retains bytes::Bytes directly. The imported buffer
                // remains valid after the dataset, stream, and native result
                // are released.
                Buffer::from(bytes),
                None,
            )
        }
        None => (
            OffsetBuffer::new(ScalarBuffer::from(vec![0_i64, 0])),
            Buffer::from(Bytes::new()),
            Some(NullBuffer::from(vec![false])),
        ),
    };
    LargeBinaryArray::try_new(offsets, values, nulls).map_err(|error| {
        lance_core::Error::internal(format!(
            "failed to build one-row LargeBinary blob payload: {error}"
        ))
    })
}

fn record_batch(schema: SchemaRef, columns: Vec<ArrayRef>) -> Result<RecordBatch> {
    RecordBatch::try_new(schema, columns).map_err(|error| {
        lance_core::Error::internal(format!(
            "failed to build one-row planned blob result: {error}"
        ))
    })
}

fn whole_blob_batch(blob: ReadBlob, schema: SchemaRef) -> Result<RecordBatch> {
    record_batch(
        schema,
        vec![
            Arc::new(UInt64Array::from(vec![blob.row_address])),
            Arc::new(large_binary_row(blob.data)?),
        ],
    )
}

fn blob_range_batch(range: ReadBlobRange, schema: SchemaRef) -> Result<RecordBatch> {
    let request_index = u64::try_from(range.request_index).map_err(|_| {
        lance_core::Error::internal(format!(
            "native request index {} exceeds u64",
            range.request_index
        ))
    })?;
    record_batch(
        schema,
        vec![
            Arc::new(UInt64Array::from(vec![request_index])),
            Arc::new(UInt64Array::from(vec![range.row_address])),
            Arc::new(UInt64Array::from(vec![range.range.offset])),
            Arc::new(UInt64Array::from(vec![range.range.length])),
            Arc::new(large_binary_row(range.data)?),
        ],
    )
}

fn whole_batch_stream(
    stream: ReadBlobsStream,
    schema: SchemaRef,
) -> impl Stream<Item = Result<RecordBatch>> {
    stream
        .map(move |result| result.and_then(|blob| whole_blob_batch(blob, Arc::clone(&schema))))
        .boxed()
}

fn range_batch_stream(
    stream: ReadBlobRangesStream,
    schema: SchemaRef,
) -> impl Stream<Item = Result<RecordBatch>> {
    stream
        .map(move |result| result.and_then(|range| blob_range_batch(range, Arc::clone(&schema))))
        .boxed()
}

fn export_stream<S, E>(stream: S, schema: SchemaRef, out: *mut FFI_ArrowArrayStream) -> Result<i32>
where
    S: Stream<Item = std::result::Result<RecordBatch, E>> + Unpin + Send + 'static,
    E: std::fmt::Display,
{
    let reader = GuardedReader::new(
        stream,
        schema,
        RT.handle().clone(),
        Arc::new(AtomicBool::new(false)),
    );
    let ffi_stream = FFI_ArrowArrayStream::new(Box::new(reader));
    unsafe { ptr::write_unaligned(out, ffi_stream) };
    Ok(0)
}

fn read_options_from_ffi(options: *const LanceBlobReadOptions) -> Result<(Option<u64>, bool)> {
    if options.is_null() {
        return Ok((None, true));
    }
    let options = unsafe { &*options };
    let order = BlobReadOrder::from_raw(options.order)?;
    Ok((
        options.has_io_buffer_size.then_some(options.io_buffer_size),
        order.preserve_order(),
    ))
}

unsafe fn selected_rows(
    selector: BlobRowSelector,
    rows: *const u64,
    num_rows: usize,
) -> Result<Vec<u64>> {
    if selector == BlobRowSelector::None {
        if num_rows != 0 {
            return Err(lance_core::Error::invalid_input(format!(
                "rows must be omitted for selector NONE, got num_rows = {num_rows}"
            )));
        }
        return Ok(Vec::new());
    }
    if num_rows > 0 && rows.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "rows must not be NULL when num_rows = {num_rows}"
        )));
    }
    let rows = if num_rows == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(rows, num_rows) }
    };
    Ok(rows.to_vec())
}

fn apply_whole_selection(
    builder: lance::dataset::ReadBlobsBuilder,
    selector: BlobRowSelector,
    rows: Vec<u64>,
) -> lance::dataset::ReadBlobsBuilder {
    match selector {
        BlobRowSelector::None => builder,
        BlobRowSelector::Ids => builder.with_row_ids(rows),
        BlobRowSelector::Addresses => builder.with_row_addresses(rows),
        BlobRowSelector::Indices => builder.with_row_indices(rows),
    }
}

fn apply_range_selection(
    builder: lance::dataset::ReadBlobRangesBuilder,
    selector: BlobRowSelector,
    requests: Vec<BlobRangeRequest>,
) -> lance::dataset::ReadBlobRangesBuilder {
    match selector {
        BlobRowSelector::None => builder,
        BlobRowSelector::Ids => builder.with_row_ids(requests),
        BlobRowSelector::Addresses => builder.with_row_addresses(requests),
        BlobRowSelector::Indices => builder.with_row_indices(requests),
    }
}

unsafe fn read_blobs_inner(
    dataset: *const LanceDataset,
    column: *const c_char,
    selector: i32,
    rows: *const u64,
    num_rows: usize,
    options: *const LanceBlobReadOptions,
    out: *mut FFI_ArrowArrayStream,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input("dataset must not be NULL"));
    }
    if column.is_null() {
        return Err(lance_core::Error::invalid_input("column must not be NULL"));
    }
    if out.is_null() {
        return Err(lance_core::Error::invalid_input("out must not be NULL"));
    }
    let selector = BlobRowSelector::from_raw(selector)?;
    let rows = unsafe { selected_rows(selector, rows, num_rows)? };
    let (io_buffer_size, preserve_order) = read_options_from_ffi(options)?;
    let column = unsafe { helpers::parse_c_string(column)? }
        .expect("column was checked for NULL above")
        .to_string();

    let snapshot = unsafe { &*dataset }.snapshot();
    let builder = snapshot.read_blobs(&column)?;
    let builder = apply_whole_selection(builder, selector, rows);
    let mut builder = match io_buffer_size {
        Some(size) => builder.with_io_buffer_size_bytes(size),
        None => builder,
    };
    builder = builder.preserve_order(preserve_order);

    let stream = block_on(builder.try_into_stream())?;
    let schema = whole_blob_schema();
    export_stream(whole_batch_stream(stream, Arc::clone(&schema)), schema, out)
}

/// Plan and export whole-blob reads as an Arrow stream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_read_blobs(
    dataset: *const LanceDataset,
    column: *const c_char,
    selector: i32,
    rows: *const u64,
    num_rows: usize,
    options: *const LanceBlobReadOptions,
    out: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe { read_blobs_inner(dataset, column, selector, rows, num_rows, options, out) },
        neg
    )
}

unsafe fn read_blob_ranges_inner(
    dataset: *const LanceDataset,
    column: *const c_char,
    selector: i32,
    requests: *const LanceBlobRangeRequest,
    num_requests: usize,
    options: *const LanceBlobReadOptions,
    out: *mut FFI_ArrowArrayStream,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input("dataset must not be NULL"));
    }
    if column.is_null() {
        return Err(lance_core::Error::invalid_input("column must not be NULL"));
    }
    if out.is_null() {
        return Err(lance_core::Error::invalid_input("out must not be NULL"));
    }
    let selector = BlobRowSelector::from_raw(selector)?;
    if selector == BlobRowSelector::None && num_requests != 0 {
        return Err(lance_core::Error::invalid_input(format!(
            "requests must be omitted for selector NONE, got num_requests = {num_requests}"
        )));
    }
    if num_requests > 0 && requests.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "requests must not be NULL when num_requests = {num_requests}"
        )));
    }
    let (io_buffer_size, preserve_order) = read_options_from_ffi(options)?;
    let column = unsafe { helpers::parse_c_string(column)? }
        .expect("column was checked for NULL above")
        .to_string();

    // Copy every foreign field before native state retains the request list.
    let requests = if num_requests == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(requests, num_requests) }
            .iter()
            .map(|request| BlobRangeRequest::new(request.row, request.offset, request.length))
            .collect()
    };

    let snapshot = unsafe { &*dataset }.snapshot();
    let builder = snapshot.read_blob_ranges(&column)?;
    let builder = apply_range_selection(builder, selector, requests);
    let mut builder = match io_buffer_size {
        Some(size) => builder.with_io_buffer_size_bytes(size),
        None => builder,
    };
    builder = builder.preserve_order(preserve_order);

    let stream = block_on(builder.try_into_stream())?;
    let schema = blob_range_schema();
    export_stream(range_batch_stream(stream, Arc::clone(&schema)), schema, out)
}

/// Plan and export blob-range reads as an Arrow stream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_read_blob_ranges(
    dataset: *const LanceDataset,
    column: *const c_char,
    selector: i32,
    requests: *const LanceBlobRangeRequest,
    num_requests: usize,
    options: *const LanceBlobReadOptions,
    out: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe {
            read_blob_ranges_inner(
                dataset,
                column,
                selector,
                requests,
                num_requests,
                options,
                out,
            )
        },
        neg
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    use arrow::array::Array;

    #[test]
    fn large_binary_row_retains_native_bytes_without_copying() {
        let bytes = Bytes::from_static(b"native payload");
        let expected_ptr = bytes.as_ptr();
        let array = large_binary_row(Some(bytes)).unwrap();
        assert_eq!(array.value(0), b"native payload");
        assert_eq!(array.value_data().as_ptr(), expected_ptr);
    }

    #[test]
    fn large_binary_row_distinguishes_null_from_valid_empty() {
        let null = large_binary_row(None).unwrap();
        assert!(null.is_null(0));

        let empty = large_binary_row(Some(Bytes::new())).unwrap();
        assert!(!empty.is_null(0));
        assert!(empty.value(0).is_empty());
    }
}
