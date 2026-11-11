// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Callback-based `NewColumnTransform::BatchUDF` C API.
//!
//! Native owns schema publication, row-count checks, cache ordering, fragment
//! cleanup, and commit semantics. This module only translates Arrow C Data
//! values, callback errors, and the four synchronous checkpoint methods.

use std::ffi::{CStr, CString, c_char, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::sync::Arc;

use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema, from_ffi_and_data_type};
use arrow_array::{Array, RecordBatch, RecordBatchOptions, StructArray};
use arrow_schema::{DataType, Schema};
use lance::dataset::{BatchInfo, BatchUDF, NewColumnTransform, UDFCheckpointStore};
use lance_core::{Error, Result};
use lance_table::format::Fragment;

use crate::dataset::LanceDataset;
use crate::error::{ffi_try, panic_payload_message};
use crate::helpers;
use crate::runtime::block_on;

/// Owned, optional UTF-8 string produced by a foreign callback.
#[repr(C)]
pub struct LanceOwnedString {
    /// NUL-terminated string data, or NULL when absent.
    pub data: *mut c_char,
    /// Releases `data`. Required whenever `data` is non-NULL.
    pub release: Option<unsafe extern "C" fn(*mut LanceOwnedString)>,
}

impl LanceOwnedString {
    fn empty() -> Self {
        Self {
            data: ptr::null_mut(),
            release: None,
        }
    }
}

/// Structured failure returned by a mapper or checkpoint callback.
#[repr(C)]
pub struct LanceCallbackError {
    /// A nonzero `LanceErrorCode` value.
    pub code: i32,
    /// Callback-owned message copied before `release` is invoked.
    pub message: LanceOwnedString,
}

impl LanceCallbackError {
    fn empty() -> Self {
        Self {
            code: 0,
            message: LanceOwnedString::empty(),
        }
    }
}

/// Native checkpoint batch identity.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LanceUDFBatchInfo {
    /// Native fragment ID.
    pub fragment_id: u32,
    /// Zero-based updater batch index within the fragment.
    pub batch_index: usize,
}

/// Map one borrowed input RecordBatch to an owned output array.
pub type LanceBatchUDFMapper = unsafe extern "C" fn(
    callback_ctx: *mut c_void,
    input: *const FFI_ArrowArray,
    input_schema: *const FFI_ArrowSchema,
    output: *mut FFI_ArrowArray,
    error: *mut LanceCallbackError,
) -> i32;

/// Retrieve a checkpointed output batch. Returns 1 on hit, 0 on miss, -1 on
/// error.
pub type LanceUDFCheckpointGetBatch = unsafe extern "C" fn(
    callback_ctx: *mut c_void,
    info: *const LanceUDFBatchInfo,
    output: *mut FFI_ArrowArray,
    error: *mut LanceCallbackError,
) -> i32;

/// Retain a checkpoint batch. The mutable Arrow arguments are movable owned
/// values; the library releases any ownership left in the slots on return.
pub type LanceUDFCheckpointInsertBatch = unsafe extern "C" fn(
    callback_ctx: *mut c_void,
    info: *const LanceUDFBatchInfo,
    batch: *mut FFI_ArrowArray,
    batch_schema: *mut FFI_ArrowSchema,
    error: *mut LanceCallbackError,
) -> i32;

/// Retrieve native Fragment JSON. Returns 1 on hit, 0 on miss, -1 on error.
pub type LanceUDFCheckpointGetFragment = unsafe extern "C" fn(
    callback_ctx: *mut c_void,
    fragment_id: u32,
    out_json: *mut LanceOwnedString,
    error: *mut LanceCallbackError,
) -> i32;

/// Persist native Fragment JSON. The JSON is borrowed for the callback.
pub type LanceUDFCheckpointInsertFragment = unsafe extern "C" fn(
    callback_ctx: *mut c_void,
    fragment_id: u64,
    fragment_json: *const c_char,
    error: *mut LanceCallbackError,
) -> i32;

/// Foreign implementation of every native `UDFCheckpointStore` method.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceUDFCheckpointStore {
    pub get_batch: Option<LanceUDFCheckpointGetBatch>,
    pub insert_batch: Option<LanceUDFCheckpointInsertBatch>,
    pub get_fragment: Option<LanceUDFCheckpointGetFragment>,
    pub insert_fragment: Option<LanceUDFCheckpointInsertFragment>,
    pub callback_ctx: *mut c_void,
}

/// Foreign BatchUDF declaration.
#[repr(C)]
pub struct LanceBatchUDF {
    /// Required mapper callback.
    pub mapper: Option<LanceBatchUDFMapper>,
    /// Call-scoped mapper context.
    pub mapper_ctx: *mut c_void,
    /// Complete native output RecordBatch schema, borrowed for this call.
    pub output_schema: *const FFI_ArrowSchema,
    /// Optional pointer to all four checkpoint callbacks.
    pub checkpoint_store: *const LanceUDFCheckpointStore,
}

/// Add columns produced by the native BatchUDF transform.
///
/// `read_columns == NULL` maps to native `None`; a non-NULL NULL-terminated
/// array maps to `Some`, including an immediately terminated empty array.
/// `batch_size == NULL` maps to `None`; otherwise the pointed-to value,
/// including zero, maps to `Some(value)`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_add_columns_batch_udf(
    dataset: *mut LanceDataset,
    udf: *const LanceBatchUDF,
    read_columns: *const *const c_char,
    batch_size: *const u32,
) -> i32 {
    ffi_try!(
        unsafe { add_columns_batch_udf_inner(dataset, udf, read_columns, batch_size) },
        neg
    )
}

unsafe fn add_columns_batch_udf_inner(
    dataset: *mut LanceDataset,
    udf: *const LanceBatchUDF,
    read_columns: *const *const c_char,
    batch_size: *const u32,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    if udf.is_null() {
        return Err(Error::invalid_input_source("udf must not be NULL".into()));
    }

    // SAFETY: `udf` is non-NULL and the caller guarantees it points to a live
    // LanceBatchUDF for the duration of this call. The output schema is read
    // only through this borrow and is never released by the binding.
    let udf = unsafe { &*udf };
    let mapper_callback = udf
        .mapper
        .ok_or_else(|| Error::invalid_input_source("udf.mapper must not be NULL".into()))?;
    let output_schema = unsafe { import_output_schema(udf.output_schema, "udf.output_schema")? };
    let read_columns = unsafe { helpers::parse_c_string_array(read_columns)? };
    let batch_size = if batch_size.is_null() {
        None
    } else {
        // SAFETY: non-NULL means the caller supplied one readable u32.
        Some(unsafe { *batch_size })
    };

    let result_checkpoint = if udf.checkpoint_store.is_null() {
        None
    } else {
        // SAFETY: the pointer is either NULL or caller-owned for this call.
        let store = unsafe { *udf.checkpoint_store };
        if store.get_batch.is_none()
            || store.insert_batch.is_none()
            || store.get_fragment.is_none()
            || store.insert_fragment.is_none()
        {
            return Err(Error::invalid_input_source(
                "checkpoint_store must provide all four callbacks".into(),
            ));
        }
        Some(Arc::new(FfiCheckpointStore {
            store,
            output_schema: output_schema.clone(),
        }) as Arc<dyn UDFCheckpointStore>)
    };

    let mapper = FfiBatchUdfMapper {
        callback: mapper_callback,
        callback_ctx: udf.mapper_ctx,
        output_schema: output_schema.clone(),
    };
    let mapper = Box::new(move |batch: &RecordBatch| {
        let mapper = &mapper;
        mapper.map(batch)
    });

    let transform = NewColumnTransform::BatchUDF(BatchUDF {
        mapper,
        output_schema,
        result_checkpoint,
    });

    // SAFETY: `dataset` is non-NULL and points to a live LanceDataset. The
    // write lock is held for the complete native call, including synchronous
    // mapper and checkpoint callbacks. Callbacks must not re-enter this handle;
    // independent handles and pre-existing snapshots remain readable.
    let dataset = unsafe { &*dataset };
    dataset
        .with_mut(|dataset| block_on(dataset.add_columns(transform, read_columns, batch_size)))?;
    Ok(0)
}

unsafe fn import_output_schema(schema: *const FFI_ArrowSchema, name: &str) -> Result<Arc<Schema>> {
    if schema.is_null() {
        return Err(Error::invalid_input_source(
            format!("{name} must not be NULL").into(),
        ));
    }
    // SAFETY: caller supplies a live ArrowSchema for this call.
    let schema = unsafe { &*schema };
    if schema.release.is_none() || schema.format.is_null() {
        return Err(Error::invalid_input_source(
            format!("{name} is uninitialised or already released").into(),
        ));
    }
    // SAFETY: `format` is non-NULL and follows the C Data Interface invariant.
    if unsafe { CStr::from_ptr(schema.format) }.to_str().is_err() {
        return Err(Error::invalid_input_source(
            format!("{name} format is not valid UTF-8").into(),
        ));
    }

    // Complete Arrow Schema import preserves field order, names, nullability,
    // field metadata, and schema metadata. Arrow may assert on malformed child
    // pointers, so contain that deterministic validation failure.
    match catch_unwind(AssertUnwindSafe(|| Schema::try_from(schema))) {
        Ok(Ok(schema)) => Ok(Arc::new(schema)),
        Ok(Err(error)) => Err(Error::invalid_input_source(
            format!("{name} is not a valid Arrow schema: {error}").into(),
        )),
        Err(payload) => Err(Error::invalid_input_source(
            format!(
                "{name} triggered an Arrow validation panic: {}",
                panic_payload_message(&*payload)
            )
            .into(),
        )),
    }
}

struct FfiBatchUdfMapper {
    callback: LanceBatchUDFMapper,
    callback_ctx: *mut c_void,
    output_schema: Arc<Schema>,
}

// SAFETY: the callback ABI requires the context and callback table to be valid
// for the complete native call, which is synchronous and awaited. Native may
// invoke the mapper from runtime worker threads, so the caller must provide a
// thread-safe context.
unsafe impl Send for FfiBatchUdfMapper {}
unsafe impl Sync for FfiBatchUdfMapper {}

impl FfiBatchUdfMapper {
    fn map(&self, input: &RecordBatch) -> Result<RecordBatch> {
        let struct_array: StructArray = input.clone().into();
        let input_array = FFI_ArrowArray::new(&struct_array.into_data());
        let input_schema = FFI_ArrowSchema::try_from(input.schema().as_ref()).map_err(|error| {
            Error::internal(format!("failed to export mapper input schema: {error}"))
        })?;

        let mut output = FFI_ArrowArray::empty();
        let mut error = LanceCallbackError::empty();
        // SAFETY: all pointers are valid for the callback. Input values are
        // borrowed and the output/error slots are initialized and writable.
        let status = unsafe {
            (self.callback)(
                self.callback_ctx,
                &input_array,
                &input_schema,
                &mut output,
                &mut error,
            )
        };
        drop(input_array);
        drop(input_schema);

        if status != 0 {
            let native = unsafe { callback_error(&mut error, status, "mapper") };
            unsafe { release_array_slot(&mut output) };
            return Err(native);
        }
        unsafe { release_message_only(&mut error) };
        unsafe { import_output_batch(&mut output, &self.output_schema, "mapper") }
    }
}

struct FfiCheckpointStore {
    store: LanceUDFCheckpointStore,
    output_schema: Arc<Schema>,
}

// SAFETY: native calls all four methods synchronously during the enclosing
// add-columns call. The caller owns the call-scoped context and must make it
// safe for concurrent invocation and for uses from native worker threads.
unsafe impl Send for FfiCheckpointStore {}
unsafe impl Sync for FfiCheckpointStore {}

impl UDFCheckpointStore for FfiCheckpointStore {
    fn get_batch(&self, info: &BatchInfo) -> Result<Option<RecordBatch>> {
        let info = LanceUDFBatchInfo {
            fragment_id: info.fragment_id,
            batch_index: info.batch_index,
        };
        let mut output = FFI_ArrowArray::empty();
        let mut error = LanceCallbackError::empty();
        let callback = self
            .store
            .get_batch
            .expect("validated checkpoint get_batch callback");
        // SAFETY: the callback table and context are valid for this call.
        let status = unsafe { callback(self.store.callback_ctx, &info, &mut output, &mut error) };
        match status {
            0 => {
                unsafe { release_array_slot(&mut output) };
                unsafe { release_message_only(&mut error) };
                Ok(None)
            }
            1 => {
                let batch = unsafe {
                    import_output_batch(&mut output, &self.output_schema, "checkpoint get_batch")
                };
                unsafe { release_message_only(&mut error) };
                batch.map(Some)
            }
            _ => {
                let native = unsafe { callback_error(&mut error, status, "checkpoint get_batch") };
                unsafe { release_array_slot(&mut output) };
                Err(native)
            }
        }
    }

    fn insert_batch(&self, info: BatchInfo, batch: RecordBatch) -> Result<()> {
        let info = LanceUDFBatchInfo {
            fragment_id: info.fragment_id,
            batch_index: info.batch_index,
        };
        let (mut array, mut schema) = export_batch(&batch)?;
        let mut error = LanceCallbackError::empty();
        let callback = self
            .store
            .insert_batch
            .expect("validated checkpoint insert_batch callback");
        // SAFETY: both Arrow values are initialized owned payloads. The
        // callback may move them and clear the source release callbacks. Any
        // ownership left in the slots belongs to the library and is released
        // after the callback returns on every path.
        let status = unsafe {
            callback(
                self.store.callback_ctx,
                &info,
                &mut array,
                &mut schema,
                &mut error,
            )
        };
        unsafe { release_array_slot(&mut array) };
        unsafe { release_schema_slot(&mut schema) };

        if status == 0 {
            unsafe { release_message_only(&mut error) };
            Ok(())
        } else {
            Err(unsafe { callback_error(&mut error, status, "checkpoint insert_batch") })
        }
    }

    fn get_fragment(&self, fragment_id: u32) -> Result<Option<Fragment>> {
        let mut out_json = LanceOwnedString::empty();
        let mut error = LanceCallbackError::empty();
        let callback = self
            .store
            .get_fragment
            .expect("validated checkpoint get_fragment callback");
        // SAFETY: the callback table and context are valid for this call.
        let status = unsafe {
            callback(
                self.store.callback_ctx,
                fragment_id,
                &mut out_json,
                &mut error,
            )
        };
        match status {
            0 => {
                unsafe { release_owned_string_slot(&mut out_json) };
                unsafe { release_message_only(&mut error) };
                Ok(None)
            }
            1 => {
                let text = unsafe { take_owned_string(&mut out_json) };
                unsafe { release_message_only(&mut error) };
                let json = text.into_string(
                    "checkpoint get_fragment returned a hit without valid UTF-8 JSON",
                )?;
                serde_json::from_str(&json).map(Some).map_err(|error| {
                    Error::invalid_input(format!(
                        "checkpoint get_fragment returned invalid Fragment JSON: {error}"
                    ))
                })
            }
            _ => {
                let native =
                    unsafe { callback_error(&mut error, status, "checkpoint get_fragment") };
                unsafe { release_owned_string_slot(&mut out_json) };
                Err(native)
            }
        }
    }

    fn insert_fragment(&self, fragment: Fragment) -> Result<()> {
        let fragment_id = fragment.id;
        let json = serde_json::to_string(&fragment).map_err(|error| {
            Error::internal(format!(
                "failed to serialize Fragment {fragment_id} for checkpoint insert: {error}"
            ))
        })?;
        let json = CString::new(json).map_err(|error| {
            Error::internal(format!(
                "Fragment {fragment_id} checkpoint JSON contains an embedded NUL: {error}"
            ))
        })?;
        let mut error = LanceCallbackError::empty();
        let callback = self
            .store
            .insert_fragment
            .expect("validated checkpoint insert_fragment callback");
        // SAFETY: `json` remains alive and is borrowed for the callback only.
        let status = unsafe {
            callback(
                self.store.callback_ctx,
                fragment_id,
                json.as_ptr(),
                &mut error,
            )
        };
        if status == 0 {
            unsafe { release_message_only(&mut error) };
            Ok(())
        } else {
            Err(unsafe { callback_error(&mut error, status, "checkpoint insert_fragment") })
        }
    }
}

fn export_batch(batch: &RecordBatch) -> Result<(FFI_ArrowArray, FFI_ArrowSchema)> {
    let struct_array: StructArray = batch.clone().into();
    let array = FFI_ArrowArray::new(&struct_array.into_data());
    let schema = FFI_ArrowSchema::try_from(batch.schema().as_ref()).map_err(|error| {
        Error::internal(format!(
            "failed to export checkpoint insert schema: {error}"
        ))
    })?;
    Ok((array, schema))
}

unsafe fn import_output_batch(
    output: *mut FFI_ArrowArray,
    schema: &Arc<Schema>,
    context: &str,
) -> Result<RecordBatch> {
    if output.is_null() {
        return Err(Error::invalid_input_source(
            format!("{context} output must not be NULL").into(),
        ));
    }

    // SAFETY: the slot is caller-owned and initialized.
    let array = unsafe { &*output };
    if array.is_released() {
        return Err(Error::invalid_input_source(
            format!("{context} did not return an owned ArrowArray").into(),
        ));
    }
    if array.length < 0
        || array.offset < 0
        || array.n_buffers < 0
        || array.n_children < 0
        || array.null_count < -1
        || array.null_count > array.length
    {
        return Err(Error::invalid_input_source(
            format!(
                "{context} returned an invalid ArrowArray geometry: length={}, offset={}, \
                 null_count={}, n_buffers={}, n_children={}",
                array.length, array.offset, array.null_count, array.n_buffers, array.n_children
            )
            .into(),
        ));
    }
    if array.n_children != schema.fields().len() as i64 {
        return Err(Error::invalid_input_source(
            format!(
                "{context} returned {} struct children but the declared schema has {} fields",
                array.n_children,
                schema.fields().len()
            )
            .into(),
        ));
    }
    if array.n_buffers != 1 || array.buffers.is_null() {
        return Err(Error::invalid_input_source(
            format!(
                "{context} returned a struct array with n_buffers={} and {} buffers pointer",
                array.n_buffers,
                if array.buffers.is_null() {
                    "NULL"
                } else {
                    "non-NULL"
                }
            )
            .into(),
        ));
    }
    if array.n_children > 0 && array.children.is_null() {
        return Err(Error::invalid_input_source(
            format!("{context} returned a NULL children pointer").into(),
        ));
    }
    if !array.dictionary.is_null() {
        return Err(Error::invalid_input_source(
            format!("{context} returned a struct array with a dictionary").into(),
        ));
    }
    if array.null_count > 0 && unsafe { *array.buffers }.is_null() {
        return Err(Error::invalid_input_source(
            format!("{context} returned null_count > 0 without a validity buffer").into(),
        ));
    }

    // Claim the slot exactly once even if Arrow later rejects the payload.
    let array = unsafe { FFI_ArrowArray::from_raw(output) };
    let imported = catch_unwind(AssertUnwindSafe(|| {
        let data =
            unsafe { from_ffi_and_data_type(array, DataType::Struct(schema.fields().clone())) }
                .map_err(|error| {
                    Error::invalid_input_source(
                        format!("{context} returned invalid Arrow C data: {error}").into(),
                    )
                })?;
        let row_count = data.len();
        let struct_array = StructArray::from(data);
        let (_, columns, nulls) = struct_array.into_parts();
        if nulls.as_ref().is_some_and(|nulls| nulls.null_count() != 0) {
            return Err(Error::invalid_input_source(
                format!("{context} returned a nullable top-level struct").into(),
            ));
        }
        RecordBatch::try_new_with_options(
            schema.clone(),
            columns,
            &RecordBatchOptions::new().with_row_count(Some(row_count)),
        )
        .map_err(|error| {
            Error::invalid_input_source(
                format!("{context} returned an invalid RecordBatch: {error}").into(),
            )
        })
    }));

    match imported {
        Ok(result) => result,
        Err(payload) => Err(Error::invalid_input_source(
            format!(
                "{context} triggered an Arrow import panic: {}",
                panic_payload_message(&*payload)
            )
            .into(),
        )),
    }
}

unsafe fn callback_error(error: &mut LanceCallbackError, status: i32, context: &str) -> Error {
    let code = error.code;
    let message = unsafe { take_owned_string(&mut error.message) }.into_display_string(&format!(
        "{context} callback returned error status {status} without a message"
    ));

    match code {
        1 => Error::invalid_input(message),
        2 => Error::io(message),
        3 => Error::not_found(message),
        6 => Error::internal(message),
        7 => Error::not_supported(message),
        other => Error::internal(format!(
            "{context} callback returned unsupported error code {other} (status {status}): {message}"
        )),
    }
}

struct CallbackString {
    text: Option<String>,
    invalid_utf8: bool,
}

impl CallbackString {
    fn into_display_string(self, fallback: &str) -> String {
        if self.invalid_utf8 {
            return format!("{fallback}: callback message is not valid UTF-8");
        }
        self.text.unwrap_or_else(|| fallback.to_owned())
    }

    fn into_string(self, fallback: &str) -> Result<String> {
        if self.invalid_utf8 {
            return Err(Error::invalid_input(format!(
                "{fallback}: message is not valid UTF-8"
            )));
        }
        Ok(self.text.unwrap_or_else(|| fallback.to_owned()))
    }
}

unsafe fn take_owned_string(value: &mut LanceOwnedString) -> CallbackString {
    let mut text = None;
    let mut invalid_utf8 = false;
    if !value.data.is_null() {
        // SAFETY: a callback-provided string must be NUL-terminated while its
        // release callback is still live.
        match unsafe { CStr::from_ptr(value.data) }.to_str() {
            Ok(value) => text = Some(value.to_owned()),
            Err(_) => invalid_utf8 = true,
        }
    }
    unsafe { release_owned_string_slot(value) };
    CallbackString { text, invalid_utf8 }
}

unsafe fn release_message_only(error: &mut LanceCallbackError) {
    unsafe { release_owned_string_slot(&mut error.message) };
}

unsafe fn release_owned_string_slot(value: &mut LanceOwnedString) {
    if let Some(release) = value.release.take() {
        unsafe { release(value) };
    }
    value.data = ptr::null_mut();
}

unsafe fn release_array_slot(array: &mut FFI_ArrowArray) {
    if array.release.is_some() {
        drop(unsafe { FFI_ArrowArray::from_raw(array) });
    }
}

unsafe fn release_schema_slot(schema: &mut FFI_ArrowSchema) {
    if schema.release.is_some() {
        drop(unsafe { FFI_ArrowSchema::from_raw(schema) });
    }
}
