// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

use std::ffi::c_char;
use std::ptr;

use arrow::ffi::FFI_ArrowSchema;
use lance_core::{Error, Result};
use lance_file::version::LanceFileVersion;
use lance_file::versions;
use lance_file::writer::{FileWriter, FileWriterOptions};
use object_store::path::Path;

use crate::batch::LanceBatch;
use crate::error::{ffi_try, swallow_unwind};
use crate::file_session::{
    FileSessionInner, LanceSchedulerConfig, open_file_session_inner, shared_session_from_ffi,
    storage_options_from_ffi,
};
use crate::helpers::{import_lance_schema, parse_c_string};
use crate::runtime::block_on;
use crate::session::LanceSession;
use crate::storage_options::LanceStorageOptionsAccessor;

/// Writer construction options.
///
/// `version == NULL` uses the native default file version. `has_*` fields
/// preserve native `None` versus explicit zero and false values.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LanceFileWriterOptions {
    pub has_data_cache_bytes: i32,
    pub data_cache_bytes: u64,
    pub has_max_page_bytes: i32,
    pub max_page_bytes: u64,
    pub has_keep_original_array: i32,
    pub keep_original_array: i32,
    pub version: *const c_char,
}

/// Native file write summary.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LanceFileWriteSummary {
    pub num_rows: u64,
    pub size_bytes: u64,
}

pub struct LanceFileWriter {
    inner: FileWriter,
}

struct ParsedWriterOptions {
    native: FileWriterOptions,
    version: LanceFileVersion,
}

fn parse_optional_bool(has_value: i32, value: i32, field: &str) -> Result<Option<bool>> {
    if has_value == 0 {
        return Ok(None);
    }
    match value {
        0 => Ok(Some(false)),
        1 => Ok(Some(true)),
        other => Err(Error::invalid_input(format!(
            "{field} must be 0 or 1 when set, got {other}"
        ))),
    }
}

unsafe fn parse_writer_options(
    options: *const LanceFileWriterOptions,
) -> Result<ParsedWriterOptions> {
    let mut native = FileWriterOptions::default();
    let mut version = LanceFileVersion::default();
    if options.is_null() {
        return Ok(ParsedWriterOptions { native, version });
    }

    let options = unsafe { &*options };
    if options.has_data_cache_bytes != 0 {
        native.data_cache_bytes = Some(options.data_cache_bytes);
    }
    if options.has_max_page_bytes != 0 {
        native.max_page_bytes = Some(options.max_page_bytes);
    }
    native.keep_original_array = parse_optional_bool(
        options.has_keep_original_array,
        options.keep_original_array,
        "keep_original_array",
    )?;
    if let Some(value) = unsafe { parse_c_string(options.version)? } {
        version = value.parse::<LanceFileVersion>()?;
    }
    Ok(ParsedWriterOptions { native, version })
}

/// Parse the shared raw-file writer option struct for a dataset write.
///
/// Dataset writes select the file grammar through
/// `WriteParams::data_storage_version`, so the raw-file `version` field is
/// rejected rather than silently ignored.
pub(crate) unsafe fn dataset_file_writer_options_from_ffi(
    options: *const LanceFileWriterOptions,
) -> Result<Option<FileWriterOptions>> {
    if options.is_null() {
        return Ok(None);
    }
    let raw = unsafe { &*options };
    if !raw.version.is_null() {
        return Err(Error::invalid_input(
            "file_writer_options.version must be NULL for dataset writes; use \
             data_storage_version instead",
        ));
    }
    let parsed = unsafe { parse_writer_options(options)? };
    Ok(Some(parsed.native))
}

pub(crate) unsafe fn open_writer_at(
    session: &FileSessionInner,
    path: Path,
    schema: *const FFI_ArrowSchema,
    options: *const LanceFileWriterOptions,
) -> Result<*mut LanceFileWriter> {
    let options = unsafe { parse_writer_options(options)? };
    let schema = if schema.is_null() {
        None
    } else {
        Some(unsafe { import_lance_schema(schema, "schema")? })
    };
    let object_writer = block_on(session.store.create(&path))?;
    let version = options.version.resolve();
    let writer = if let Some(schema) = schema {
        versions::create_writer(
            version,
            object_writer,
            schema.as_ref().clone(),
            options.native,
        )?
    } else {
        versions::create_lazy_writer(version, object_writer, options.native)?
    };
    Ok(Box::into_raw(Box::new(LanceFileWriter { inner: writer })))
}

/// Open a raw Lance file writer directly.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_writer_open(
    uri_or_path: *const c_char,
    schema: *const FFI_ArrowSchema,
    options: *const LanceFileWriterOptions,
    storage_options: *const LanceStorageOptionsAccessor,
    session: *const LanceSession,
) -> *mut LanceFileWriter {
    ffi_try!(
        unsafe { file_writer_open_inner(uri_or_path, schema, options, storage_options, session) },
        null
    )
}

unsafe fn file_writer_open_inner(
    uri_or_path: *const c_char,
    schema: *const FFI_ArrowSchema,
    options: *const LanceFileWriterOptions,
    storage_options: *const LanceStorageOptionsAccessor,
    session: *const LanceSession,
) -> Result<*mut LanceFileWriter> {
    let uri_or_path = unsafe { parse_c_string(uri_or_path)? }
        .ok_or_else(|| Error::invalid_input_source("uri_or_path must not be NULL".into()))?;
    let session = open_file_session_inner(
        uri_or_path,
        storage_options_from_ffi(storage_options),
        shared_session_from_ffi(session),
        ptr::null::<LanceSchedulerConfig>(),
    )?;
    let path = session.base_path.clone();
    unsafe { open_writer_at(&session, path, schema, options) }
}

/// Write one borrowed batch.
///
/// The writer may retain an Arrow array clone. It never takes ownership of the
/// `LanceBatch` handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_writer_write_batch(
    writer: *mut LanceFileWriter,
    batch: *const LanceBatch,
) -> i32 {
    ffi_try!(unsafe { file_writer_write_batch_inner(writer, batch) }, neg)
}

unsafe fn file_writer_write_batch_inner(
    writer: *mut LanceFileWriter,
    batch: *const LanceBatch,
) -> Result<i32> {
    if writer.is_null() || batch.is_null() {
        return Err(Error::invalid_input("writer and batch must not be NULL"));
    }
    let writer = unsafe { &mut *writer };
    let batch = unsafe { &*batch };
    block_on(writer.inner.write_batch(&batch.inner))?;
    Ok(0)
}

/// Add one schema metadata entry. Native last-write-wins order is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_writer_add_schema_metadata(
    writer: *mut LanceFileWriter,
    key: *const c_char,
    value: *const c_char,
) -> i32 {
    ffi_try!(
        unsafe { file_writer_add_schema_metadata_inner(writer, key, value) },
        neg
    )
}

unsafe fn file_writer_add_schema_metadata_inner(
    writer: *mut LanceFileWriter,
    key: *const c_char,
    value: *const c_char,
) -> Result<i32> {
    if writer.is_null() {
        return Err(Error::invalid_input("writer must not be NULL"));
    }
    let key = unsafe { parse_c_string(key)? }
        .ok_or_else(|| Error::invalid_input("key must not be NULL"))?;
    let value = unsafe { parse_c_string(value)? }
        .ok_or_else(|| Error::invalid_input("value must not be NULL"))?;
    unsafe { &mut *writer }
        .inner
        .add_schema_metadata(key.to_owned(), value.to_owned());
    Ok(0)
}

/// Add one global buffer and return its one-based native index.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_writer_add_global_buffer(
    writer: *mut LanceFileWriter,
    data: *const u8,
    len: usize,
    out_index: *mut u32,
) -> i32 {
    ffi_try!(
        unsafe { file_writer_add_global_buffer_inner(writer, data, len, out_index) },
        neg
    )
}

unsafe fn file_writer_add_global_buffer_inner(
    writer: *mut LanceFileWriter,
    data: *const u8,
    len: usize,
    out_index: *mut u32,
) -> Result<i32> {
    if writer.is_null() || out_index.is_null() {
        return Err(Error::invalid_input(
            "writer and out_index must not be NULL",
        ));
    }
    if len != 0 && data.is_null() {
        return Err(Error::invalid_input(format!(
            "data must not be NULL when len={len}"
        )));
    }
    let bytes = if len == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(data, len) }.to_vec()
    };
    let writer = unsafe { &mut *writer };
    let index = block_on(writer.inner.add_global_buffer(bytes.into()))?;
    unsafe { ptr::write(out_index, index) };
    Ok(0)
}

/// Finish and close the native writer.
///
/// Native behavior is forwarded directly. Repeated calls, calls after abort,
/// and failures preserve the selected core's behavior and error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_writer_finish(
    writer: *mut LanceFileWriter,
    out_summary: *mut LanceFileWriteSummary,
) -> i32 {
    ffi_try!(
        unsafe { file_writer_finish_inner(writer, out_summary) },
        neg
    )
}

unsafe fn file_writer_finish_inner(
    writer: *mut LanceFileWriter,
    out_summary: *mut LanceFileWriteSummary,
) -> Result<i32> {
    if writer.is_null() || out_summary.is_null() {
        return Err(Error::invalid_input(
            "writer and out_summary must not be NULL",
        ));
    }
    let writer = unsafe { &mut *writer };
    let summary = block_on(writer.inner.finish())?;
    unsafe {
        ptr::write(
            out_summary,
            LanceFileWriteSummary {
                num_rows: summary.num_rows,
                size_bytes: summary.size_bytes,
            },
        );
    }
    Ok(0)
}

/// Call native abort.
///
/// Native v2.x abort bodies are currently empty; allocation close drops the
/// writer and owns any remaining object-writer cleanup. The native call is
/// still forwarded on every invocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_writer_abort(writer: *mut LanceFileWriter) -> i32 {
    ffi_try!(unsafe { file_writer_abort_inner(writer) }, neg)
}

unsafe fn file_writer_abort_inner(writer: *mut LanceFileWriter) -> Result<i32> {
    if writer.is_null() {
        return Err(Error::invalid_input("writer must not be NULL"));
    }
    let writer = unsafe { &mut *writer };
    block_on(writer.inner.abort());
    Ok(0)
}

/// Destroy the native writer allocation.
///
/// Dropping the native writer performs native object-writer cleanup. This does
/// not substitute for an explicit fallible `lance_file_writer_finish`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_writer_close(writer: *mut LanceFileWriter) {
    if !writer.is_null() {
        swallow_unwind("lance_file_writer_close", || unsafe {
            let _ = Box::from_raw(writer);
        });
    }
}
