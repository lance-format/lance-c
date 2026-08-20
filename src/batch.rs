// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! LanceBatch C API: Arrow C Data Interface export.

use std::ptr;

use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema};
use arrow_array::RecordBatch;
use lance_core::Result;

use crate::error::{ffi_try, swallow_unwind};

/// Opaque handle wrapping an Arrow RecordBatch.
pub struct LanceBatch {
    pub(crate) inner: RecordBatch,
}

/// Export a `LanceBatch` as Arrow C Data Interface structs.
///
/// Writes the array data to `out_array` and the schema to `out_schema`.
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
    let (ffi_array, ffi_schema) = arrow::ffi::to_ffi(&struct_array.into())?;
    unsafe {
        ptr::write_unaligned(out_array, ffi_array);
        ptr::write_unaligned(out_schema, ffi_schema);
    }
    Ok(0)
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
