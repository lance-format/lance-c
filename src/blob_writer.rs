// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Native packed and dedicated blob writers backed by a raw-file session.

use std::ffi::{CString, c_char};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::slice;

use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema, from_ffi_and_data_type, to_ffi};
use arrow_array::{Array, ArrayData, ArrayRef, make_array};
use arrow_schema::{DataType, Field};
use lance::{BlobDescriptor, BlobDescriptorArrayBuilder, DedicatedBlobWriter, PackedBlobWriter};
use lance_core::{Error, Result};

use crate::batch::claim_arrow_array;
use crate::error::{ffi_try, panic_payload_message, swallow_unwind};
use crate::file_session::{LanceFileSession, resolve_session_path};
use crate::helpers;
use crate::runtime::block_on;

/// Opaque owner of one native packed blob writer.
///
/// Native `finish` consumes the writer. The C allocation remains valid until
/// close, with `inner == None` representing the finished state.
pub struct LancePackedBlobWriter {
    inner: Option<PackedBlobWriter>,
}

/// Opaque owner of one native dedicated blob writer.
///
/// Native `finish` consumes the writer. The C allocation remains valid until
/// close, with `inner == None` representing the finished state.
pub struct LanceDedicatedBlobWriter {
    inner: Option<DedicatedBlobWriter>,
}

unsafe fn packed_writer_mut<'a>(
    writer: *mut LancePackedBlobWriter,
) -> Result<&'a mut LancePackedBlobWriter> {
    if writer.is_null() {
        return Err(Error::invalid_input_source(
            "writer must not be NULL".into(),
        ));
    }
    let writer = unsafe { &mut *writer };
    if writer.inner.is_none() {
        return Err(Error::invalid_input_source(
            "packed blob writer is already finished".into(),
        ));
    }
    Ok(writer)
}

unsafe fn packed_writer_ref<'a>(
    writer: *const LancePackedBlobWriter,
) -> Result<&'a LancePackedBlobWriter> {
    if writer.is_null() {
        return Err(Error::invalid_input_source(
            "writer must not be NULL".into(),
        ));
    }
    let writer = unsafe { &*writer };
    if writer.inner.is_none() {
        return Err(Error::invalid_input_source(
            "packed blob writer is already finished".into(),
        ));
    }
    Ok(writer)
}

unsafe fn dedicated_writer_mut<'a>(
    writer: *mut LanceDedicatedBlobWriter,
) -> Result<&'a mut LanceDedicatedBlobWriter> {
    if writer.is_null() {
        return Err(Error::invalid_input_source(
            "writer must not be NULL".into(),
        ));
    }
    let writer = unsafe { &mut *writer };
    if writer.inner.is_none() {
        return Err(Error::invalid_input_source(
            "dedicated blob writer is already finished".into(),
        ));
    }
    Ok(writer)
}

unsafe fn dedicated_writer_ref<'a>(
    writer: *const LanceDedicatedBlobWriter,
) -> Result<&'a LanceDedicatedBlobWriter> {
    if writer.is_null() {
        return Err(Error::invalid_input_source(
            "writer must not be NULL".into(),
        ));
    }
    let writer = unsafe { &*writer };
    if writer.inner.is_none() {
        return Err(Error::invalid_input_source(
            "dedicated blob writer is already finished".into(),
        ));
    }
    Ok(writer)
}

unsafe fn required_field_name(field_name: *const c_char) -> Result<String> {
    unsafe { helpers::parse_c_string(field_name)? }
        .map(str::to_owned)
        .ok_or_else(|| Error::invalid_input_source("field_name must not be NULL".into()))
}

unsafe fn borrowed_bytes<'a>(data: *const u8, len: usize, name: &str) -> Result<&'a [u8]> {
    if len == 0 {
        return Ok(&[]);
    }
    if data.is_null() {
        return Err(Error::invalid_input_source(format!(
            "{name} must not be NULL when len is {len}"
        )));
    }
    if len > isize::MAX as usize {
        return Err(Error::invalid_input_source(format!(
            "{name} len={len} exceeds isize::MAX on this platform"
        )));
    }
    Ok(unsafe { slice::from_raw_parts(data, len) })
}

fn binary_data_type(data_type: &DataType) -> bool {
    matches!(
        data_type,
        DataType::Binary
            | DataType::LargeBinary
            | DataType::BinaryView
            | DataType::FixedSizeBinary(_)
    )
}

unsafe fn import_binary_payloads(
    payloads: *mut FFI_ArrowArray,
    schema: *const FFI_ArrowSchema,
) -> Result<ArrayRef> {
    if payloads.is_null() {
        return Err(Error::invalid_input_source(
            "payloads must not be NULL".into(),
        ));
    }

    // Ownership transfers before any writer, schema, type, or data validation.
    let array = unsafe { claim_arrow_array(payloads) };
    let field = unsafe { helpers::import_arrow_field(schema, "schema")? };
    let data_type = field.data_type().clone();
    if !binary_data_type(&data_type) {
        return Err(Error::invalid_input(format!(
            "schema field {:?} has type {data_type}; expected Binary, LargeBinary, BinaryView, or FixedSizeBinary",
            field.name()
        )));
    }
    if array.length < 0 {
        return Err(Error::invalid_input(format!(
            "Arrow payloads length must be non-negative, got {}",
            array.length
        )));
    }

    // A zero-length array can carry an arbitrary unused slice offset. Normalize
    // it before Arrow observes any offsets or value buffers.
    let data = if array.length == 0 {
        drop(array);
        ArrayData::new_empty(&data_type)
    } else {
        let imported = catch_unwind(AssertUnwindSafe(|| {
            let data =
                unsafe { from_ffi_and_data_type(array, data_type.clone()) }.map_err(|error| {
                    Error::invalid_input(format!(
                        "payloads is not a valid Arrow {data_type} array: {error}"
                    ))
                })?;
            data.validate_full().map_err(|error| {
                Error::invalid_input(format!(
                    "payloads Arrow {data_type} data failed validation: {error}"
                ))
            })?;
            Ok::<_, Error>(data)
        }));
        match imported {
            Ok(data) => data?,
            Err(payload) => {
                return Err(Error::invalid_input(format!(
                    "payloads triggered an Arrow validation panic: {}",
                    panic_payload_message(&*payload)
                )));
            }
        }
    };
    Ok(make_array(data))
}

fn descriptor_array(
    field_name: &str,
    descriptors: impl IntoIterator<Item = BlobDescriptor>,
) -> Result<(FFI_ArrowArray, FFI_ArrowSchema)> {
    let mut builder = BlobDescriptorArrayBuilder::new(field_name);
    builder.extend(descriptors)?;
    let (field, array) = builder.finish()?.into_parts();
    let (ffi_array, generated_schema) = to_ffi(&array.to_data())?;
    drop(generated_schema);
    let ffi_schema = FFI_ArrowSchema::try_from(&field)?;
    Ok((ffi_array, ffi_schema))
}

// ---------------------------------------------------------------------------
// File-session constructors
// ---------------------------------------------------------------------------

/// Open a native packed blob writer for a data-file path relative to a session.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_open_packed_blob_writer(
    session: *const LanceFileSession,
    data_file_path: *const c_char,
    blob_id: u32,
) -> *mut LancePackedBlobWriter {
    ffi_try!(
        unsafe { file_session_open_packed_blob_writer_inner(session, data_file_path, blob_id) },
        null
    )
}

unsafe fn file_session_open_packed_blob_writer_inner(
    session: *const LanceFileSession,
    data_file_path: *const c_char,
    blob_id: u32,
) -> Result<*mut LancePackedBlobWriter> {
    if session.is_null() {
        return Err(Error::invalid_input_source(
            "session must not be NULL".into(),
        ));
    }
    let session = unsafe { &*session };
    let path = resolve_session_path(&session.inner.base_path, data_file_path)?;
    let writer = block_on(PackedBlobWriter::try_new(
        session.inner.store.as_ref().clone(),
        path,
        blob_id,
    ))?;
    Ok(Box::into_raw(Box::new(LancePackedBlobWriter {
        inner: Some(writer),
    })))
}

/// Open a native dedicated blob writer for a data-file path relative to a session.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_open_dedicated_blob_writer(
    session: *const LanceFileSession,
    data_file_path: *const c_char,
    blob_id: u32,
) -> *mut LanceDedicatedBlobWriter {
    ffi_try!(
        unsafe { file_session_open_dedicated_blob_writer_inner(session, data_file_path, blob_id) },
        null
    )
}

unsafe fn file_session_open_dedicated_blob_writer_inner(
    session: *const LanceFileSession,
    data_file_path: *const c_char,
    blob_id: u32,
) -> Result<*mut LanceDedicatedBlobWriter> {
    if session.is_null() {
        return Err(Error::invalid_input_source(
            "session must not be NULL".into(),
        ));
    }
    let session = unsafe { &*session };
    let path = resolve_session_path(&session.inner.base_path, data_file_path)?;
    let writer = block_on(DedicatedBlobWriter::try_new(
        session.inner.store.as_ref().clone(),
        path,
        blob_id,
    ))?;
    Ok(Box::into_raw(Box::new(LanceDedicatedBlobWriter {
        inner: Some(writer),
    })))
}

// ---------------------------------------------------------------------------
// Packed writer
// ---------------------------------------------------------------------------

/// Return the native packed writer's blob ID, or 0 after an error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_packed_blob_writer_blob_id(
    writer: *const LancePackedBlobWriter,
) -> u32 {
    ffi_try!(
        unsafe {
            packed_writer_ref(writer)
                .map(|state| state.inner.as_ref().expect("active writer").blob_id())
        },
        0u32
    )
}

/// Return a library-allocated copy of the native packed sidecar path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_packed_blob_writer_path(
    writer: *const LancePackedBlobWriter,
) -> *const c_char {
    ffi_try!(
        unsafe {
            packed_writer_ref(writer).and_then(|state| {
                let path = state.inner.as_ref().expect("active writer").path();
                CString::new(path.as_ref())
                    .map(CString::into_raw)
                    .map(|value| value as *const c_char)
                    .map_err(|error| {
                        Error::internal(format!("blob sidecar path contains a NUL byte: {error}"))
                    })
            })
        },
        null
    )
}

/// Append one payload to the packed sidecar.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_packed_blob_writer_write_blob(
    writer: *mut LancePackedBlobWriter,
    data: *const u8,
    len: usize,
) -> i32 {
    ffi_try!(
        unsafe { packed_blob_writer_write_blob_inner(writer, data, len) },
        neg
    )
}

unsafe fn packed_blob_writer_write_blob_inner(
    writer: *mut LancePackedBlobWriter,
    data: *const u8,
    len: usize,
) -> Result<i32> {
    let writer = unsafe { packed_writer_mut(writer)? };
    let bytes = unsafe { borrowed_bytes(data, len, "data")? };
    block_on(
        writer
            .inner
            .as_mut()
            .expect("active writer")
            .write_blob(bytes),
    )?;
    Ok(0)
}

/// Append one row per Arrow binary payload array.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_packed_blob_writer_write_blobs(
    writer: *mut LancePackedBlobWriter,
    payloads: *mut FFI_ArrowArray,
    schema: *const FFI_ArrowSchema,
) -> i32 {
    ffi_try!(
        unsafe { packed_blob_writer_write_blobs_inner(writer, payloads, schema) },
        neg
    )
}

unsafe fn packed_blob_writer_write_blobs_inner(
    writer: *mut LancePackedBlobWriter,
    payloads: *mut FFI_ArrowArray,
    schema: *const FFI_ArrowSchema,
) -> Result<i32> {
    let payloads = unsafe { import_binary_payloads(payloads, schema)? };
    let writer = unsafe { packed_writer_mut(writer)? };
    let rows = lance_arrow::iter_binary_array(payloads.as_ref())
        .map_err(|error| Error::invalid_input(format!("invalid binary payloads: {error}")))?;
    block_on(
        writer
            .inner
            .as_mut()
            .expect("active writer")
            .write_packed_blobs(rows),
    )?;
    Ok(0)
}

/// Consume the packed writer and export its prepared descriptor field and array.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_packed_blob_writer_finish_array(
    writer: *mut LancePackedBlobWriter,
    field_name: *const c_char,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> i32 {
    ffi_try!(
        unsafe { packed_blob_writer_finish_array_inner(writer, field_name, out_array, out_schema) },
        neg
    )
}

unsafe fn packed_blob_writer_finish_array_inner(
    writer: *mut LancePackedBlobWriter,
    field_name: *const c_char,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> Result<i32> {
    let field_name = unsafe { required_field_name(field_name)? };
    if out_array.is_null() || out_schema.is_null() {
        return Err(Error::invalid_input_source(
            "out_array and out_schema must not be NULL".into(),
        ));
    }
    let native = unsafe { packed_writer_mut(writer)? }
        .inner
        .take()
        .expect("active writer");
    let descriptors = block_on(native.finish())?;
    let (ffi_array, ffi_schema) = descriptor_array(&field_name, descriptors)?;
    unsafe {
        ptr::write_unaligned(out_array, ffi_array);
        ptr::write_unaligned(out_schema, ffi_schema);
    }
    Ok(0)
}

/// Free a packed writer allocation. NULL is a no-op.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_packed_blob_writer_close(writer: *mut LancePackedBlobWriter) {
    if writer.is_null() {
        return;
    }
    swallow_unwind("lance_packed_blob_writer_close", || unsafe {
        drop(Box::from_raw(writer));
    });
}

// ---------------------------------------------------------------------------
// Dedicated writer
// ---------------------------------------------------------------------------

/// Return the native dedicated writer's blob ID, or 0 after an error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dedicated_blob_writer_blob_id(
    writer: *const LanceDedicatedBlobWriter,
) -> u32 {
    ffi_try!(
        unsafe {
            dedicated_writer_ref(writer)
                .map(|state| state.inner.as_ref().expect("active writer").blob_id())
        },
        0u32
    )
}

/// Return a library-allocated copy of the native dedicated sidecar path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dedicated_blob_writer_path(
    writer: *const LanceDedicatedBlobWriter,
) -> *const c_char {
    ffi_try!(
        unsafe {
            dedicated_writer_ref(writer).and_then(|state| {
                let path = state.inner.as_ref().expect("active writer").path();
                CString::new(path.as_ref())
                    .map(CString::into_raw)
                    .map(|value| value as *const c_char)
                    .map_err(|error| {
                        Error::internal(format!("blob sidecar path contains a NUL byte: {error}"))
                    })
            })
        },
        null
    )
}

/// Append bytes to the dedicated sidecar.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dedicated_blob_writer_write(
    writer: *mut LanceDedicatedBlobWriter,
    data: *const u8,
    len: usize,
) -> i32 {
    ffi_try!(
        unsafe { dedicated_blob_writer_write_inner(writer, data, len) },
        neg
    )
}

unsafe fn dedicated_blob_writer_write_inner(
    writer: *mut LanceDedicatedBlobWriter,
    data: *const u8,
    len: usize,
) -> Result<i32> {
    let writer = unsafe { dedicated_writer_mut(writer)? };
    let bytes = unsafe { borrowed_bytes(data, len, "data")? };
    block_on(writer.inner.as_mut().expect("active writer").write(bytes))?;
    Ok(0)
}

/// Consume the dedicated writer and export its prepared descriptor field and array.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dedicated_blob_writer_finish_array(
    writer: *mut LanceDedicatedBlobWriter,
    field_name: *const c_char,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> i32 {
    ffi_try!(
        unsafe {
            dedicated_blob_writer_finish_array_inner(writer, field_name, out_array, out_schema)
        },
        neg
    )
}

unsafe fn dedicated_blob_writer_finish_array_inner(
    writer: *mut LanceDedicatedBlobWriter,
    field_name: *const c_char,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> Result<i32> {
    let field_name = unsafe { required_field_name(field_name)? };
    if out_array.is_null() || out_schema.is_null() {
        return Err(Error::invalid_input_source(
            "out_array and out_schema must not be NULL".into(),
        ));
    }
    let native = unsafe { dedicated_writer_mut(writer)? }
        .inner
        .take()
        .expect("active writer");
    let descriptor = block_on(native.finish())?;
    let (ffi_array, ffi_schema) = descriptor_array(&field_name, [descriptor])?;
    unsafe {
        ptr::write_unaligned(out_array, ffi_array);
        ptr::write_unaligned(out_schema, ffi_schema);
    }
    Ok(0)
}

/// Free a dedicated writer allocation. NULL is a no-op.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dedicated_blob_writer_close(writer: *mut LanceDedicatedBlobWriter) {
    if writer.is_null() {
        return;
    }
    swallow_unwind("lance_dedicated_blob_writer_close", || unsafe {
        drop(Box::from_raw(writer));
    });
}
