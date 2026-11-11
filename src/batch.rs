// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! LanceBatch C API: Arrow C Data Interface import and export.

use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;

use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema, from_ffi_and_data_type};
use arrow_array::{RecordBatch, RecordBatchOptions, StructArray};
use arrow_schema::{DataType, Schema};
use lance_core::{Error, Result};

use crate::error::{ffi_try, panic_payload_message, swallow_unwind};

/// Opaque handle wrapping an Arrow RecordBatch.
pub struct LanceBatch {
    pub(crate) inner: RecordBatch,
}

/// Export a `LanceBatch` as Arrow C Data Interface values.
///
/// The returned array is the top-level struct array and the returned schema is
/// the complete `RecordBatch` schema, including schema metadata, field
/// metadata, and extension metadata. Both outputs are caller-owned and must be
/// released exactly once. Neither output is written on failure.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_batch_to_arrow(
    batch: *const LanceBatch,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> i32 {
    ffi_try!(
        unsafe { batch_to_arrow_inner(batch, out_array, out_schema) },
        neg
    )
}

unsafe fn batch_to_arrow_inner(
    batch: *const LanceBatch,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> Result<i32> {
    if batch.is_null() || out_array.is_null() || out_schema.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "batch, out_array, and out_schema must not be NULL".into(),
        ));
    }
    let b = unsafe { &*batch };
    let struct_array: arrow_array::StructArray = b.inner.clone().into();
    let ffi_array = FFI_ArrowArray::new(&struct_array.into_data());
    let ffi_schema = FFI_ArrowSchema::try_from(b.inner.schema().as_ref())?;
    unsafe {
        ptr::write_unaligned(out_array, ffi_array);
        ptr::write_unaligned(out_schema, ffi_schema);
    }
    Ok(0)
}

/// Import one Arrow record batch from the C Data Interface.
///
/// Ownership of a non-NULL `array` transfers to the library on every return,
/// including validation and conversion failures. The source array is left in
/// its canonical released state. `schema` is borrowed for this call only and is
/// never released or modified. `out` is written only on success and is
/// unchanged on failure.
///
/// The complete Arrow schema is preserved, including schema metadata, field
/// metadata, and extension metadata. A zero-column batch retains its row count.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_batch_from_arrow(
    array: *mut FFI_ArrowArray,
    schema: *const FFI_ArrowSchema,
    out: *mut *mut LanceBatch,
) -> i32 {
    ffi_try!(unsafe { batch_from_arrow_inner(array, schema, out) }, neg)
}

unsafe fn batch_from_arrow_inner(
    array: *mut FFI_ArrowArray,
    schema: *const FFI_ArrowSchema,
    out: *mut *mut LanceBatch,
) -> Result<i32> {
    let array = if array.is_null() {
        None
    } else {
        // SAFETY: ownership of a non-NULL source slot transfers to this call.
        Some(unsafe { claim_arrow_array(array) })
    };

    if schema.is_null() {
        return Err(Error::invalid_input_source(
            "schema must not be NULL".into(),
        ));
    }
    if out.is_null() {
        return Err(Error::invalid_input_source("out must not be NULL".into()));
    }
    let array =
        array.ok_or_else(|| Error::invalid_input_source("array must not be NULL".into()))?;

    // SAFETY: `schema` is non-NULL and borrowed for this call.
    let schema = unsafe { &*schema };
    if schema.release.is_none() || schema.format.is_null() {
        return Err(Error::invalid_input_source(
            "schema is uninitialised or already released".into(),
        ));
    }

    let expected_schema = match catch_unwind(AssertUnwindSafe(|| Schema::try_from(schema))) {
        Ok(Ok(schema)) => schema,
        Ok(Err(error)) => {
            return Err(Error::invalid_input_source(
                format!("schema is not a valid Arrow schema: {error}").into(),
            ));
        }
        Err(payload) => {
            return Err(Error::invalid_input_source(
                format!(
                    "schema triggered an Arrow validation panic: {}",
                    panic_payload_message(&*payload)
                )
                .into(),
            ));
        }
    };

    let imported = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: `array` owns a live ArrowArray and `expected_schema` describes
        // the top-level struct children.
        let data = unsafe {
            from_ffi_and_data_type(array, DataType::Struct(expected_schema.fields().clone()))
        }
        .map_err(|error| {
            Error::invalid_input_source(format!("invalid Arrow array: {error}").into())
        })?;
        let row_count = data.len();
        let struct_array = StructArray::from(data);
        let (_, columns, nulls) = struct_array.into_parts();
        if nulls.as_ref().is_some_and(|nulls| nulls.null_count() != 0) {
            return Err(Error::invalid_input_source(
                "top-level Arrow struct array must not contain null rows".into(),
            ));
        }
        RecordBatch::try_new_with_options(
            std::sync::Arc::new(expected_schema),
            columns,
            &RecordBatchOptions::new().with_row_count(Some(row_count)),
        )
        .map_err(|error| {
            Error::invalid_input_source(format!("invalid Arrow record batch: {error}").into())
        })
    }));

    let batch = match imported {
        Ok(Ok(batch)) => batch,
        Ok(Err(error)) => return Err(error),
        Err(payload) => {
            return Err(Error::invalid_input_source(
                format!(
                    "Arrow batch import panicked: {}",
                    panic_payload_message(&*payload)
                )
                .into(),
            ));
        }
    };

    unsafe {
        ptr::write(out, Box::into_raw(Box::new(LanceBatch { inner: batch })));
    }
    Ok(0)
}

unsafe fn claim_arrow_array(array: *mut FFI_ArrowArray) -> FFI_ArrowArray {
    // SAFETY: the caller transfers ownership of the initialized source slot.
    let owned = unsafe { FFI_ArrowArray::from_raw(array) };
    // Leave a canonical released value in the source slot so a caller that
    // accidentally inspects it cannot release the same payload twice.
    unsafe { ptr::write(array, FFI_ArrowArray::empty()) };
    owned
}

/// Free a `LanceBatch` handle.
///
/// Best-effort (issue #61): a panic raised while dropping the batch is
/// caught and logged rather than unwinding into the caller, and the
/// remainder of the value may leak.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_batch_free(batch: *mut LanceBatch) {
    if !batch.is_null() {
        swallow_unwind("lance_batch_free", || unsafe {
            let _ = Box::from_raw(batch);
        });
    }
}
