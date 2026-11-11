// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Dataset delta C API: native `dataset::delta` version-diff scans.
//!
//! Mirrors `lance::dataset::delta::{DatasetDeltaBuilder, DatasetDelta}`:
//! synchronous builder validation, native version/date range resolution,
//! native transaction listing and the four guarded row-change streams.
//! No binding-side diff or row-change algorithm is introduced.

use std::ptr;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;

use arrow::ffi_stream::FFI_ArrowArrayStream;
use chrono::{DateTime, Utc};
use lance::dataset::delta::{DatasetDelta, DatasetDeltaBuilder};
use lance::io::RecordBatchStream;
use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::runtime::{RT, block_on};
use crate::stream_guard::GuardedReader;
use crate::transaction::LanceTransaction;

/// Timestamp for delta date-mode ranges.
///
/// `unix_seconds` is a signed Unix timestamp in whole seconds and `nanos`
/// the nanosecond component. Native `chrono::DateTime::from_timestamp` defines
/// representability, including leap seconds. An unrepresentable value is
/// rejected by the timestamp setter.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct LanceDeltaTimestamp {
    /// Signed Unix seconds.
    pub unix_seconds: i64,
    /// Nanosecond component, validated by the native timestamp constructor.
    pub nanos: u32,
}

/// C builder wrapping a native `DatasetDeltaBuilder`, which owns a clone of
/// the caller's dataset. Building validates range combinations synchronously.
pub struct LanceDatasetDeltaBuilder {
    /// `None` once `build` has consumed the native builder; the handle
    /// allocation itself is freed on `build`'s consuming return.
    inner: Option<DatasetDeltaBuilder>,
}

/// C handle wrapping a native `DatasetDelta`. Reusable for `list_transactions`
/// and every `get_*` stream; each returned stream owns its state after the
/// delta (and dataset) handle closes.
pub struct LanceDatasetDelta {
    inner: DatasetDelta,
}

/// Convert a C timestamp to a native `DateTime<Utc>`. The native chrono
/// constructor owns representability and range; a value it rejects is an
/// invalid argument here.
fn delta_timestamp_from_ffi(ts: &LanceDeltaTimestamp) -> Result<DateTime<Utc>> {
    DateTime::from_timestamp(ts.unix_seconds, ts.nanos).ok_or_else(|| {
        lance_core::Error::invalid_input(format!(
            "timestamp (unix_seconds = {}, nanos = {}) is not representable as a native DateTime<Utc>",
            ts.unix_seconds, ts.nanos
        ))
    })
}

/// Construct a delta builder from a dataset snapshot. The builder owns a
/// native `Dataset` clone, so a closed `LanceDataset` handle does not
/// invalidate it.
///
/// Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_delta_builder_new(
    dataset: *const LanceDataset,
) -> *mut LanceDatasetDeltaBuilder {
    ffi_try!(unsafe { dataset_delta_builder_new_inner(dataset) }, null)
}

unsafe fn dataset_delta_builder_new_inner(
    dataset: *const LanceDataset,
) -> Result<*mut LanceDatasetDeltaBuilder> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    // SAFETY: non-NULL (checked above); the caller guarantees a live handle.
    let snapshot = unsafe { &*dataset }.snapshot();
    let builder = DatasetDeltaBuilder::new((*snapshot).clone());
    Ok(Box::into_raw(Box::new(LanceDatasetDeltaBuilder {
        inner: Some(builder),
    })))
}

/// Close a delta builder handle. NULL-safe. Close exactly once.
/// `build` consumes the allocation, so a consumed builder must not be closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_delta_builder_close(builder: *mut LanceDatasetDeltaBuilder) {
    if !builder.is_null() {
        crate::error::swallow_unwind("lance_dataset_delta_builder_close", || unsafe {
            let _ = Box::from_raw(builder);
        });
    }
}

/// Close a delta handle. NULL-safe. Close exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_delta_close(delta: *mut LanceDatasetDelta) {
    if !delta.is_null() {
        crate::error::swallow_unwind("lance_dataset_delta_close", || unsafe {
            let _ = Box::from_raw(delta);
        });
    }
}

/// Set the compared-against version. Native `build` rejects combining this
/// with any explicit begin/end version or date. Not set (native `None`) until
/// called.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_delta_builder_set_compared_against_version(
    builder: *mut LanceDatasetDeltaBuilder,
    version: u64,
) -> i32 {
    ffi_try!(
        unsafe { apply_delta_builder(builder, |inner| inner.compared_against_version(version)) },
        neg
    )
}

/// Set the explicit begin version. Native `build` requires both begin and end
/// versions or a compared-against version. Not set until called.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_delta_builder_set_begin_version(
    builder: *mut LanceDatasetDeltaBuilder,
    version: u64,
) -> i32 {
    ffi_try!(
        unsafe { apply_delta_builder(builder, |inner| inner.with_begin_version(version)) },
        neg
    )
}

/// Set the explicit end version.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_delta_builder_set_end_version(
    builder: *mut LanceDatasetDeltaBuilder,
    version: u64,
) -> i32 {
    ffi_try!(
        unsafe { apply_delta_builder(builder, |inner| inner.with_end_version(version)) },
        neg
    )
}

/// Set the begin date for date-mode ranges. Native `build` requires a begin
/// date when an end date is provided. The timestamp is copied; NULL is
/// rejected and the builder keeps its prior value.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_delta_builder_set_begin_timestamp(
    builder: *mut LanceDatasetDeltaBuilder,
    timestamp: *const LanceDeltaTimestamp,
) -> i32 {
    ffi_try!(
        unsafe { set_delta_timestamp(builder, timestamp, false) },
        neg
    )
}

/// Set the end date for date-mode ranges (requires a begin date in native
/// `build`).
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_delta_builder_set_end_timestamp(
    builder: *mut LanceDatasetDeltaBuilder,
    timestamp: *const LanceDeltaTimestamp,
) -> i32 {
    ffi_try!(
        unsafe { set_delta_timestamp(builder, timestamp, true) },
        neg
    )
}

unsafe fn set_delta_timestamp(
    builder: *mut LanceDatasetDeltaBuilder,
    timestamp: *const LanceDeltaTimestamp,
    is_end: bool,
) -> Result<i32> {
    if timestamp.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "timestamp must not be NULL".into(),
        ));
    }
    // SAFETY: non-NULL (checked above); the caller guarantees a readable
    // `LanceDeltaTimestamp` for the duration of this call.
    let ts = unsafe { &*timestamp };
    let native = delta_timestamp_from_ffi(ts)?;
    // SAFETY: `apply_delta_builder` dereferences `builder`, which the caller
    // guarantees is a live handle for this call.
    unsafe {
        if is_end {
            apply_delta_builder(builder, |inner| inner.with_end_date(native))
        } else {
            apply_delta_builder(builder, |inner| inner.with_begin_date(native))
        }
    }
}

/// Build the delta synchronously, consuming `builder` on every return path —
/// including foreign-pointer errors and native validation failures. Native
/// `build` validates range combinations (compared-against version conflicts,
/// partial version and date ranges, missing range) and normalizes the
/// compared-against mode's versions natively. On success `*out_delta`
/// receives an independently owned delta handle; on error the slot is left
/// unchanged.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_delta_builder_build(
    builder: *mut LanceDatasetDeltaBuilder,
    out_delta: *mut *mut LanceDatasetDelta,
) -> i32 {
    ffi_try!(
        unsafe { dataset_delta_builder_build_inner(builder, out_delta) },
        neg
    )
}

unsafe fn dataset_delta_builder_build_inner(
    builder: *mut LanceDatasetDeltaBuilder,
    out_delta: *mut *mut LanceDatasetDelta,
) -> Result<i32> {
    // Consume the caller's allocation before any operation that can fail.
    let builder = unsafe { consume_delta_builder(builder)? };
    let inner = builder.inner.ok_or_else(|| {
        lance_core::Error::invalid_input_source("builder was already consumed".into())
    })?;
    if out_delta.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_delta must not be NULL".into(),
        ));
    }
    // Native DatasetDeltaBuilder::build is synchronous.
    let delta = inner.build()?;
    // SAFETY: `out_delta` is non-NULL (checked above) and the caller
    // guarantees it points to writable storage.
    unsafe {
        ptr::write(
            out_delta,
            Box::into_raw(Box::new(LanceDatasetDelta { inner: delta })),
        );
    }
    Ok(0)
}

/// Apply a native builder method to the stored builder. The stored builder
/// value is taken before the call and restored afterwards, so a panic inside
/// the native method cannot leave a half-mutated builder behind.
unsafe fn apply_delta_builder(
    builder: *mut LanceDatasetDeltaBuilder,
    f: impl FnOnce(DatasetDeltaBuilder) -> DatasetDeltaBuilder,
) -> Result<i32> {
    if builder.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "builder must not be NULL".into(),
        ));
    }
    // SAFETY: non-NULL (checked above); the caller guarantees a live handle.
    let builder = unsafe { &mut *builder };
    let inner = builder.inner.take().ok_or_else(|| {
        lance_core::Error::invalid_input_source("builder was already consumed".into())
    })?;
    builder.inner = Some(f(inner));
    Ok(0)
}

/// Take ownership of a delta builder allocation, freeing it on every return.
unsafe fn consume_delta_builder(
    builder: *mut LanceDatasetDeltaBuilder,
) -> Result<LanceDatasetDeltaBuilder> {
    if builder.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "builder must not be NULL".into(),
        ));
    }
    // SAFETY: the caller transfers ownership of a live allocation to this
    // call exactly once; the box frees it on return.
    Ok(unsafe { *Box::from_raw(builder) })
}

/// List the transactions committed between the delta's begin and end
/// versions. Each returned transaction is independently owned; the caller
/// releases them with `lance_transaction_array_free`. Versions without a
/// transaction file are skipped natively. `*out_transactions` receives the
/// array (NULL when the count is 0) and `*out_count` its length; both slots
/// are left unchanged on error.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_delta_list_transactions(
    delta: *const LanceDatasetDelta,
    out_transactions: *mut *mut *mut LanceTransaction,
    out_count: *mut usize,
) -> i32 {
    ffi_try!(
        unsafe { dataset_delta_list_transactions_inner(delta, out_transactions, out_count) },
        neg
    )
}

unsafe fn dataset_delta_list_transactions_inner(
    delta: *const LanceDatasetDelta,
    out_transactions: *mut *mut *mut LanceTransaction,
    out_count: *mut usize,
) -> Result<i32> {
    if delta.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "delta must not be NULL".into(),
        ));
    }
    if out_transactions.is_null() || out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_transactions and out_count must not be NULL".into(),
        ));
    }
    // SAFETY: non-NULL (checked above); the caller guarantees a live delta
    // and writable output slots for the duration of this call.
    let delta = unsafe { &*delta };
    let transactions = block_on(delta.inner.list_transactions())?;
    let items: Vec<*mut LanceTransaction> = transactions
        .into_iter()
        .map(|inner| Box::into_raw(Box::new(LanceTransaction { inner })))
        .collect();
    let count = items.len();
    let array: *mut *mut LanceTransaction = if count == 0 {
        drop(items);
        ptr::null_mut()
    } else {
        // A boxed slice's raw pointer is fat; the thin data pointer plus the
        // returned count is what `lance_transaction_array_free` reconstructs.
        Box::into_raw(items.into_boxed_slice()) as *mut *mut LanceTransaction
    };
    // SAFETY: both slots are non-NULL (checked above) and writable.
    unsafe {
        ptr::write(out_count, count);
        ptr::write(out_transactions, array);
    }
    Ok(0)
}

/// Free a transaction array produced by `lance_dataset_delta_list_transactions`,
/// closing each non-NULL transaction handle and the array allocation. NULL and
/// zero-length arrays are no-ops. Call exactly once with the exact pointer and
/// count returned by `list_transactions`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_transaction_array_free(
    transactions: *mut *mut LanceTransaction,
    count: usize,
) {
    if transactions.is_null() || count == 0 {
        return;
    }
    crate::error::swallow_unwind("lance_transaction_array_free", || unsafe {
        let slice = std::slice::from_raw_parts_mut(transactions, count);
        for slot in slice.iter_mut() {
            let item = *slot;
            *slot = ptr::null_mut();
            if !item.is_null() {
                drop(Box::from_raw(item));
            }
        }
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
            transactions,
            count,
        )));
    });
}

/// Stream the rows inserted across the delta's range, scanning natively.
/// `*out_stream` receives a guarded Arrow C stream (release it exactly once);
/// it stays valid after the delta and dataset handles close.
///
/// Returns 0 on success, -1 on error. The stream slot is left unchanged on
/// error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_delta_get_inserted_rows(
    delta: *const LanceDatasetDelta,
    out_stream: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe { delta_rows_inner(delta, out_stream, DeltaRowKind::Inserted) },
        neg
    )
}

/// Stream the rows updated across the delta's range. See
/// `lance_dataset_delta_get_inserted_rows`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_delta_get_updated_rows(
    delta: *const LanceDatasetDelta,
    out_stream: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe { delta_rows_inner(delta, out_stream, DeltaRowKind::Updated) },
        neg
    )
}

/// Stream the rows upserted across the delta's range. See
/// `lance_dataset_delta_get_inserted_rows`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_delta_get_upserted_rows(
    delta: *const LanceDatasetDelta,
    out_stream: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe { delta_rows_inner(delta, out_stream, DeltaRowKind::Upserted) },
        neg
    )
}

/// Stream the deleted row ids across the delta's range. The stream's single
/// column is the native stable row-id column; native resolution and id
/// normalization are authoritative, and native errors (unsupported versions,
/// reversed ranges, missing stable row ids) surface as returned errors.
/// See `lance_dataset_delta_get_inserted_rows`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_delta_get_deleted_row_ids(
    delta: *const LanceDatasetDelta,
    out_stream: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe { delta_rows_inner(delta, out_stream, DeltaRowKind::DeletedRowIds) },
        neg
    )
}

enum DeltaRowKind {
    Inserted,
    Updated,
    Upserted,
    DeletedRowIds,
}

unsafe fn delta_rows_inner(
    delta: *const LanceDatasetDelta,
    out_stream: *mut FFI_ArrowArrayStream,
    kind: DeltaRowKind,
) -> Result<i32> {
    if delta.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "delta must not be NULL".into(),
        ));
    }
    if out_stream.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_stream must not be NULL".into(),
        ));
    }
    // SAFETY: both pointers are non-NULL (checked above); the caller
    // guarantees a live delta (the call holds no reference across it) and
    // writable stream storage.
    let delta = unsafe { &*delta };
    let stream = match kind {
        DeltaRowKind::Inserted => block_on(delta.inner.get_inserted_rows())?,
        DeltaRowKind::Updated => block_on(delta.inner.get_updated_rows())?,
        DeltaRowKind::Upserted => block_on(delta.inner.get_upserted_rows())?,
        DeltaRowKind::DeletedRowIds => block_on(delta.inner.get_deleted_row_ids())?,
    };
    let schema = stream.schema();
    let guarded = GuardedReader::new(
        stream,
        schema,
        RT.handle().clone(),
        Arc::new(AtomicBool::new(false)),
    );
    let ffi_stream = FFI_ArrowArrayStream::new(Box::new(guarded));
    // SAFETY: non-NULL (checked above); the caller guarantees writable
    // storage.
    unsafe {
        ptr::write_unaligned(out_stream, ffi_stream);
    }
    Ok(0)
}
