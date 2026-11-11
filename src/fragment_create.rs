// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Native fragment creation.
//!
//! These entry points map directly to Lance's public
//! `FragmentCreateBuilder::{write, write_fragments}` and
//! `FileFragment::create_from_file`. They write uncommitted data files and
//! return detached metadata; publication happens only through the existing
//! typed `Append`/`Overwrite` operation and commit path.
//!
//! Native code owns schema inference and compatibility, input splitting,
//! physical row counts, file-format selection, fragment ID assignment for
//! multi-fragment writes and every validation rule. The binding converts the C
//! representation, transfers stream ownership, and passes the explicit single
//! `fragment_id` that native takes as `Option<u64>`.

use std::ffi::c_char;
use std::ptr;

use arrow::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use lance::dataset::WriteParams;
use lance::dataset::fragment::FileFragment;
use lance::dataset::fragment::write::FragmentCreateBuilder;
use lance_core::datatypes::Schema as NativeSchema;
use lance_core::{Error, Result};

use crate::commit::session_from_ffi;
use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
use crate::fragment::LanceFragment;
use crate::helpers;
use crate::runtime::block_on;
use crate::schema::LanceSchema;
use crate::session::LanceSession;
use crate::writer::{LanceWriteMode, LanceWriteOptions, write_params_from_ffi};

/// Options shared by [`lance_fragment_create`] and
/// [`lance_fragment_create_many`].
///
/// `schema == NULL` leaves native schema inference in place; in APPEND mode
/// native inference may reuse the existing dataset's schema, including its
/// field IDs. A non-NULL schema is borrowed for the call and cloned into
/// Rust-owned storage before the asynchronous write begins.
#[repr(C)]
pub struct LanceFragmentCreateOptions {
    /// Native schema to write with. NULL uses native inference.
    pub schema: *const LanceSchema,
    /// `LanceWriteMode`: create, append or overwrite.
    pub mode: i32,
    /// Native write options. NULL uses native `WriteParams::default()`.
    pub write: *const LanceWriteOptions,
    /// NULL-terminated `[key, value, ..., NULL]` storage options.
    pub storage_opts: *const *const c_char,
    /// Shared session for caches and store configuration. NULL means default.
    pub session: *const LanceSession,
}

/// Owned native inputs for one creation call.
struct CreateInputs {
    schema: Option<NativeSchema>,
    params: WriteParams,
}

/// Convert the optional C options into a cloned native schema and native
/// `WriteParams`. All representation validation is delegated to the shared
/// writer conversions.
unsafe fn create_inputs_from_ffi(
    options: *const LanceFragmentCreateOptions,
) -> Result<CreateInputs> {
    // SAFETY: a non-NULL `options` points to a caller-owned, initialized
    // `LanceFragmentCreateOptions` that stays valid for this call.
    let Some(options) = (unsafe { options.as_ref() }) else {
        return Ok(CreateInputs {
            schema: None,
            params: WriteParams::default(),
        });
    };

    let mode = LanceWriteMode::from_raw(options.mode)?;
    let params = unsafe {
        write_params_from_ffi(
            mode,
            ptr::null(),
            options.write,
            false,
            options.storage_opts,
            session_from_ffi(options.session),
        )?
    };
    // Cloning a native schema preserves field IDs, logical types, encodings,
    // dictionary values and nested metadata; an Arrow round trip would not.
    let schema = if options.schema.is_null() {
        None
    } else {
        // SAFETY: a non-NULL `schema` points to a live native schema handle for
        // the duration of this call.
        Some(unsafe { &*options.schema }.inner.clone())
    };
    Ok(CreateInputs { schema, params })
}

/// Claim a caller-provided stream.
///
/// A NULL stream is rejected without consuming anything. Once accepted, the
/// returned reader owns the stream so every later error path releases it.
unsafe fn claim_stream(stream: *mut FFI_ArrowArrayStream) -> Result<ArrowArrayStreamReader> {
    unsafe { helpers::take_arrow_array_stream(stream) }
}

/// Parse the dataset URI, reusing the creating entry points' representation
/// rules: NULL and empty are invalid arguments.
unsafe fn require_uri<'a>(dataset_uri: *const c_char) -> Result<&'a str> {
    unsafe { helpers::parse_c_string(dataset_uri)? }
        .filter(|uri| !uri.is_empty())
        .ok_or_else(|| Error::invalid_input("dataset_uri must not be empty"))
}

/// Build the native builder for one call.
fn builder<'a>(uri: &'a str, inputs: &'a CreateInputs) -> FragmentCreateBuilder<'a> {
    let builder = FragmentCreateBuilder::new(uri).write_params(&inputs.params);
    match &inputs.schema {
        Some(schema) => builder.schema(schema),
        None => builder,
    }
}

/// Create one fragment without publishing a version.
///
/// Maps to `FragmentCreateBuilder::write(source, Some(fragment_id))`. Native
/// accepts any representable ID, including one already present in existing
/// metadata, and rejects an empty input.
///
/// The stream is consumed on every return path except a NULL `stream`.
/// `out_fragment` is written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_create(
    dataset_uri: *const c_char,
    fragment_id: u64,
    options: *const LanceFragmentCreateOptions,
    stream: *mut FFI_ArrowArrayStream,
    out_fragment: *mut *mut LanceFragment,
) -> i32 {
    ffi_try!(
        unsafe { fragment_create_inner(dataset_uri, fragment_id, options, stream, out_fragment) },
        neg
    )
}

unsafe fn fragment_create_inner(
    dataset_uri: *const c_char,
    fragment_id: u64,
    options: *const LanceFragmentCreateOptions,
    stream: *mut FFI_ArrowArrayStream,
    out_fragment: *mut *mut LanceFragment,
) -> Result<i32> {
    // This NULL check is the only validation that runs before ownership
    // transfer. After `claim_stream` succeeds, every other return path drops
    // the reader and fires the caller's release callback exactly once.
    if stream.is_null() {
        return Err(Error::invalid_input_source(
            "stream must not be NULL".into(),
        ));
    }
    let reader = unsafe { claim_stream(stream) }?;

    if out_fragment.is_null() {
        return Err(Error::invalid_input_source(
            "out_fragment must not be NULL".into(),
        ));
    }
    let uri = unsafe { require_uri(dataset_uri) }?;
    let inputs = unsafe { create_inputs_from_ffi(options) }?;

    let fragment = block_on(builder(uri, &inputs).write(reader, Some(fragment_id)))?;

    // SAFETY: `out_fragment` is non-NULL and caller-owned; success-only write.
    unsafe {
        ptr::write(
            out_fragment,
            Box::into_raw(Box::new(LanceFragment::from_metadata(fragment))),
        );
    }
    Ok(0)
}

/// Create zero or more fragments without publishing a version.
///
/// Maps to `FragmentCreateBuilder::write_fragments`, so native
/// `max_rows_per_file` splitting and fragment ID assignment apply.
///
/// Both output slots are written only on success, and the stream follows the
/// same consumption contract as [`lance_fragment_create`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_create_many(
    dataset_uri: *const c_char,
    options: *const LanceFragmentCreateOptions,
    stream: *mut FFI_ArrowArrayStream,
    out_fragments: *mut *mut *mut LanceFragment,
    out_count: *mut usize,
) -> i32 {
    ffi_try!(
        unsafe {
            fragment_create_many_inner(dataset_uri, options, stream, out_fragments, out_count)
        },
        neg
    )
}

unsafe fn fragment_create_many_inner(
    dataset_uri: *const c_char,
    options: *const LanceFragmentCreateOptions,
    stream: *mut FFI_ArrowArrayStream,
    out_fragments: *mut *mut *mut LanceFragment,
    out_count: *mut usize,
) -> Result<i32> {
    if stream.is_null() {
        return Err(Error::invalid_input_source(
            "stream must not be NULL".into(),
        ));
    }
    let reader = unsafe { claim_stream(stream) }?;

    if out_fragments.is_null() {
        return Err(Error::invalid_input_source(
            "out_fragments must not be NULL".into(),
        ));
    }
    if out_count.is_null() {
        return Err(Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let uri = unsafe { require_uri(dataset_uri) }?;
    let inputs = unsafe { create_inputs_from_ffi(options) }?;

    let fragments = block_on(builder(uri, &inputs).write_fragments(reader))?;

    if fragments.is_empty() {
        // SAFETY: both output pointers are non-NULL and caller-owned.
        unsafe {
            ptr::write(out_fragments, ptr::null_mut());
            ptr::write(out_count, 0);
        }
        return Ok(0);
    }

    let count = fragments.len();
    if count > isize::MAX as usize / std::mem::size_of::<*mut LanceFragment>() {
        return Err(Error::invalid_input(format!(
            "fragment count {count} overflows the fragment pointer array"
        )));
    }

    let handles: Vec<*mut LanceFragment> = fragments
        .into_iter()
        .map(|fragment| Box::into_raw(Box::new(LanceFragment::from_metadata(fragment))))
        .collect();
    let array = Box::into_raw(handles.into_boxed_slice()) as *mut *mut LanceFragment;

    // SAFETY: both output pointers are non-NULL and caller-owned; the array is
    // fully built before either slot is touched, and the writes are success-only.
    unsafe {
        ptr::write(out_fragments, array);
        ptr::write(out_count, count);
    }
    Ok(0)
}

/// Close every remaining non-NULL fragment in an array from
/// [`lance_fragment_create_many`], then release the array allocation.
///
/// NULL-safe. `count` must be the count `create_many` reported; slots a C++
/// wrapper already adopted are NULL and are skipped.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_array_free(
    fragments: *mut *mut LanceFragment,
    count: usize,
) {
    if fragments.is_null() || count == 0 {
        return;
    }
    swallow_unwind("lance_fragment_array_free", || unsafe {
        let slots = std::slice::from_raw_parts_mut(fragments, count);
        for slot in slots.iter_mut() {
            if !slot.is_null() {
                drop(Box::from_raw(*slot));
                *slot = ptr::null_mut();
            }
        }
        // Rebuild the exact `Box<[*mut LanceFragment]>` allocation that
        // `create_many` created: the slice length is the same, so the layout
        // matches.
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
            fragments, count,
        )));
    });
}

/// Create one fragment from an existing Lance data file.
///
/// Maps to `FileFragment::create_from_file(filename, dataset_snapshot,
/// fragment_id, physical_rows)`. Native owns file-version, schema
/// compatibility and physical-row validation.
///
/// The dataset handle supplies the target snapshot and is not modified.
/// `out_fragment` is written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_create_from_file(
    dataset: *const LanceDataset,
    filename: *const c_char,
    fragment_id: u64,
    physical_rows: *const u64,
    out_fragment: *mut *mut LanceFragment,
) -> i32 {
    ffi_try!(
        unsafe {
            fragment_create_from_file_inner(
                dataset,
                filename,
                fragment_id,
                physical_rows,
                out_fragment,
            )
        },
        neg
    )
}

unsafe fn fragment_create_from_file_inner(
    dataset: *const LanceDataset,
    filename: *const c_char,
    fragment_id: u64,
    physical_rows: *const u64,
    out_fragment: *mut *mut LanceFragment,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    if out_fragment.is_null() {
        return Err(Error::invalid_input_source(
            "out_fragment must not be NULL".into(),
        ));
    }
    let filename = unsafe { helpers::parse_c_string(filename)? }
        .filter(|filename| !filename.is_empty())
        .ok_or_else(|| Error::invalid_input("filename must not be empty"))?;

    // Only integer representability is checked here. Zero is a real native
    // `Some(0)` even though the current file format ignores the hint.
    let fragment_id = usize::try_from(fragment_id).map_err(|_| {
        Error::invalid_input(format!(
            "fragment_id {fragment_id} does not fit the native usize type"
        ))
    })?;
    let physical_rows = if physical_rows.is_null() {
        None
    } else {
        // SAFETY: the caller guarantees a non-NULL pointer to a readable u64.
        let raw = unsafe { ptr::read(physical_rows) };
        Some(usize::try_from(raw).map_err(|_| {
            Error::invalid_input(format!(
                "physical_rows {raw} does not fit the native usize type"
            ))
        })?)
    };

    let snapshot = unsafe { &*dataset }.snapshot();
    let fragment = block_on(FileFragment::create_from_file(
        filename,
        snapshot.as_ref(),
        fragment_id,
        physical_rows,
    ))?;

    // SAFETY: `out_fragment` is non-NULL and caller-owned; success-only write.
    unsafe {
        ptr::write(
            out_fragment,
            Box::into_raw(Box::new(LanceFragment::from_metadata(fragment))),
        );
    }
    Ok(0)
}
