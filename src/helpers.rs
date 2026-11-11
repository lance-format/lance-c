// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! C string parsing utilities.

use std::collections::HashMap;
use std::ffi::{CStr, c_char};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::Arc;

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use arrow_schema::{Field, Schema as ArrowSchema};
use chrono::TimeDelta;
use lance_core::Result;
use lance_core::datatypes::Schema as LanceSchema;

use crate::error::panic_payload_message;

/// Parse a nullable C string pointer into an `Option<&str>`.
pub unsafe fn parse_c_string<'a>(ptr: *const c_char) -> Result<Option<&'a str>> {
    if ptr.is_null() {
        return Ok(None);
    }
    let cstr = unsafe { CStr::from_ptr(ptr) };
    let s = cstr
        .to_str()
        .map_err(|e| lance_core::Error::invalid_input_source(Box::new(e)))?;
    Ok(Some(s))
}

/// Parse a NULL-terminated array of C strings into `Option<Vec<String>>`.
/// If `ptr` is NULL, returns `None` (meaning "all columns" / "no filter").
/// The array must end with a NULL pointer sentinel.
pub unsafe fn parse_c_string_array(ptr: *const *const c_char) -> Result<Option<Vec<String>>> {
    if ptr.is_null() {
        return Ok(None);
    }
    let mut result = Vec::new();
    let mut i = 0;
    loop {
        let entry = unsafe { *ptr.add(i) };
        if entry.is_null() {
            break;
        }
        let s = unsafe { parse_c_string(entry)? };
        match s {
            Some(s) => result.push(s.to_string()),
            None => break,
        }
        i += 1;
    }
    Ok(Some(result))
}

/// Take ownership of a caller-provided Arrow C stream.
///
/// A NULL pointer is rejected without consuming anything. Once a non-NULL
/// pointer is accepted, the stream is released exactly once on every outcome,
/// including malformed stream callbacks and `get_schema` failures.
pub(crate) unsafe fn take_arrow_array_stream(
    stream: *mut FFI_ArrowArrayStream,
) -> Result<ArrowArrayStreamReader> {
    if stream.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "stream must not be NULL".into(),
        ));
    }

    // `ArrowArrayStreamReader` only guards against a NULL `release`. Reject a
    // stream missing `get_schema` or `get_next` before arrow-rs can reach an
    // `unwrap()` in a non-unwinding callback.
    let (release, has_schema_cb, has_next_cb) = unsafe {
        let raw = &*stream;
        (
            raw.release,
            raw.get_schema.is_some(),
            raw.get_next.is_some(),
        )
    };
    if release.is_none() || !has_schema_cb || !has_next_cb {
        if let Some(release_fn) = release {
            // Claim ownership before invoking foreign cleanup. Clearing the
            // release slot also leaves the caller's struct in the canonical
            // released state.
            unsafe {
                (*stream).release = None;
                release_fn(stream);
            }
        }
        return Err(lance_core::Error::invalid_input_source(
            "stream is uninitialised, already released, or missing a \
             required get_schema/get_next/release callback"
                .into(),
        ));
    }

    // `from_raw` moves the complete stream into Rust. A later `get_schema`
    // failure is still owned and released by the returned error path.
    unsafe { ArrowArrayStreamReader::from_raw(stream) }
        .map_err(|error| lance_core::Error::invalid_input_source(error.to_string().into()))
}

/// Parse NULL-terminated key-value pairs into a `HashMap`.
/// Format: `["key1", "val1", "key2", "val2", NULL]`
/// If `ptr` is NULL, returns an empty map.
pub unsafe fn parse_c_string_map(
    ptr: *const *const c_char,
    label: &str,
) -> Result<HashMap<String, String>> {
    let mut map = HashMap::new();
    if ptr.is_null() {
        return Ok(map);
    }
    let mut i = 0;
    loop {
        let key_ptr = unsafe { *ptr.add(i) };
        if key_ptr.is_null() {
            break;
        }
        let val_ptr = unsafe { *ptr.add(i + 1) };
        if val_ptr.is_null() {
            return Err(lance_core::Error::invalid_input_source(
                format!("{label} must be key-value pairs; odd number of entries").into(),
            ));
        }
        let key = unsafe { parse_c_string(key_ptr)? }.unwrap().to_string();
        let val = unsafe { parse_c_string(val_ptr)? }.unwrap().to_string();
        map.insert(key, val);
        i += 2;
    }
    Ok(map)
}

/// Parse a nullable NULL-terminated key-value array.
///
/// A NULL pointer means the native `Option` is `None`. A non-NULL pointer,
/// including one whose first entry is NULL, means `Some`, with the latter
/// representing an explicitly empty map.
pub unsafe fn parse_optional_c_string_map(
    ptr: *const *const c_char,
    label: &str,
) -> Result<Option<HashMap<String, String>>> {
    if ptr.is_null() {
        return Ok(None);
    }
    unsafe { parse_c_string_map(ptr, label).map(Some) }
}

/// Parse NULL-terminated storage option pairs. NULL means no options.
pub unsafe fn parse_storage_options(ptr: *const *const c_char) -> Result<HashMap<String, String>> {
    unsafe { parse_c_string_map(ptr, "storage options") }
}

/// Import a borrowed Arrow schema without taking its release ownership.
///
/// Complete schema and field metadata are preserved by the native conversion.
/// Arrow can assert on malformed child pointers, so deterministic validation
/// panics are contained and reported as invalid input.
pub(crate) unsafe fn import_arrow_schema(
    schema: *const FFI_ArrowSchema,
    name: &str,
) -> Result<Arc<ArrowSchema>> {
    if schema.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            format!("{name} must not be NULL").into(),
        ));
    }
    // SAFETY: caller supplies a live ArrowSchema for this call.
    let schema = unsafe { &*schema };
    if schema.release.is_none() || schema.format.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            format!("{name} is uninitialised or already released").into(),
        ));
    }
    // SAFETY: `format` is non-NULL and follows the C Data Interface invariant.
    if unsafe { CStr::from_ptr(schema.format) }.to_str().is_err() {
        return Err(lance_core::Error::invalid_input_source(
            format!("{name} format is not valid UTF-8").into(),
        ));
    }

    match catch_unwind(AssertUnwindSafe(|| ArrowSchema::try_from(schema))) {
        Ok(Ok(schema)) => Ok(Arc::new(schema)),
        Ok(Err(error)) => Err(lance_core::Error::invalid_input_source(
            format!("{name} is not a valid Arrow schema: {error}").into(),
        )),
        Err(payload) => Err(lance_core::Error::invalid_input_source(
            format!(
                "{name} triggered an Arrow validation panic: {}",
                panic_payload_message(&*payload)
            )
            .into(),
        )),
    }
}

/// Import one borrowed Arrow field without taking release ownership.
///
/// A C Data Interface field uses the same ArrowSchema layout as a one-field
/// schema. Complete field metadata is preserved. Arrow can assert on malformed
/// child pointers, so deterministic validation panics are contained and
/// reported as invalid input.
pub(crate) unsafe fn import_arrow_field(
    schema: *const FFI_ArrowSchema,
    name: &str,
) -> Result<Field> {
    if schema.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            format!("{name} must not be NULL").into(),
        ));
    }
    // SAFETY: caller supplies a live ArrowSchema for this call.
    let schema = unsafe { &*schema };
    if schema.release.is_none() || schema.format.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            format!("{name} is uninitialised or already released").into(),
        ));
    }
    // SAFETY: `format` is non-NULL and follows the C Data Interface invariant.
    if unsafe { CStr::from_ptr(schema.format) }.to_str().is_err() {
        return Err(lance_core::Error::invalid_input_source(
            format!("{name} format is not valid UTF-8").into(),
        ));
    }

    match catch_unwind(AssertUnwindSafe(|| Field::try_from(schema))) {
        Ok(Ok(field)) => Ok(field),
        Ok(Err(error)) => Err(lance_core::Error::invalid_input_source(
            format!("{name} is not a valid Arrow field: {error}").into(),
        )),
        Err(payload) => Err(lance_core::Error::invalid_input_source(
            format!(
                "{name} triggered an Arrow validation panic: {}",
                panic_payload_message(&*payload)
            )
            .into(),
        )),
    }
}

/// Import a borrowed Arrow schema and convert it to the native Lance schema.
pub(crate) unsafe fn import_lance_schema(
    schema: *const FFI_ArrowSchema,
    name: &str,
) -> Result<Arc<LanceSchema>> {
    let schema = unsafe { import_arrow_schema(schema, name)? };
    match catch_unwind(AssertUnwindSafe(|| LanceSchema::try_from(schema.as_ref()))) {
        Ok(Ok(schema)) => Ok(Arc::new(schema)),
        Ok(Err(error)) => Err(lance_core::Error::invalid_input_source(
            format!("{name} is not a valid Lance schema: {error}").into(),
        )),
        Err(payload) => Err(lance_core::Error::invalid_input_source(
            format!(
                "{name} triggered a Lance schema validation panic: {}",
                panic_payload_message(&*payload)
            )
            .into(),
        )),
    }
}

/// Convert seconds plus a normalized signed nanosecond component into the
/// native chrono range without narrowing through signed 64-bit nanoseconds.
pub(crate) fn time_delta_from_ffi(seconds: i64, nanos: i32, context: &str) -> Result<TimeDelta> {
    const MAX_NANOS: i32 = 999_999_999;
    if !(-MAX_NANOS..=MAX_NANOS).contains(&nanos) {
        return Err(lance_core::Error::invalid_input_source(
            format!("{context}.older_than_nanos={nanos} must be in [-{MAX_NANOS}, {MAX_NANOS}]")
                .into(),
        ));
    }
    let seconds_delta = TimeDelta::try_seconds(seconds).ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!("{context}.older_than_seconds={seconds} is outside chrono::TimeDelta range")
                .into(),
        )
    })?;
    let nanos_delta = TimeDelta::nanoseconds(i64::from(nanos));
    seconds_delta.checked_add(&nanos_delta).ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!(
                "{context} older_than ({seconds} seconds + {nanos} nanoseconds) is outside chrono::TimeDelta range"
            )
            .into(),
        )
    })
}
