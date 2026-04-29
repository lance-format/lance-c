// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Dataset write C API: create, append, or overwrite a Lance dataset from an
//! Arrow C Data Interface stream, committing a manifest.
//!
//! Mirrors the structure of `src/fragment_writer.rs` but produces a full
//! dataset with a committed manifest rather than uncommitted fragment files.

use std::ffi::c_char;
use std::str::FromStr;
use std::sync::{Arc, RwLock};

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use arrow::record_batch::RecordBatchReader;
use arrow_schema::Schema as ArrowSchema;
use lance::Dataset;
use lance::dataset::{WriteMode as LanceWriteModeUpstream, WriteParams};
use lance_core::Result;
use lance_file::version::LanceFileVersion;
use lance_io::object_store::{ObjectStoreParams, StorageOptionsAccessor};

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::block_on;

/// Write mode for `lance_dataset_write`.
///
/// Discriminants are pinned for ABI stability. The FFI accepts this as
/// `int32_t` and rejects out-of-range values with `LANCE_ERR_INVALID_ARGUMENT`
/// — storing an out-of-range tag as a `repr(C)` enum would be UB.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceWriteMode {
    /// Create a new dataset. Fails with `LANCE_ERR_DATASET_ALREADY_EXISTS` if
    /// the path already exists.
    Create = 0,
    /// Append to an existing dataset. Schema-incompatibility against the
    /// existing dataset is reported via the upstream Lance error code, which
    /// currently surfaces as `LANCE_ERR_INTERNAL`; the declared-vs-stream
    /// schema mismatch handled in this layer surfaces as
    /// `LANCE_ERR_INVALID_ARGUMENT`.
    Append = 1,
    /// Overwrite an existing dataset (or create one if the path does not exist).
    Overwrite = 2,
}

impl LanceWriteMode {
    /// Validate a raw FFI integer into a `LanceWriteMode`. Out-of-range
    /// values become `InvalidInput`.
    fn from_raw(raw: i32) -> Result<Self> {
        match raw {
            0 => Ok(Self::Create),
            1 => Ok(Self::Append),
            2 => Ok(Self::Overwrite),
            other => Err(lance_core::Error::InvalidInput {
                source: format!(
                    "invalid write mode {other}; expected 0 (create), 1 (append), or 2 (overwrite)"
                )
                .into(),
                location: snafu::location!(),
            }),
        }
    }
}

impl From<LanceWriteMode> for LanceWriteModeUpstream {
    fn from(mode: LanceWriteMode) -> Self {
        match mode {
            LanceWriteMode::Create => LanceWriteModeUpstream::Create,
            LanceWriteMode::Append => LanceWriteModeUpstream::Append,
            LanceWriteMode::Overwrite => LanceWriteModeUpstream::Overwrite,
        }
    }
}

/// Tunable parameters for `lance_dataset_write_with_params`.
///
/// Numeric fields use `0` as a "keep upstream default" sentinel, and
/// `data_storage_version` uses NULL the same way. The struct is `#[repr(C)]`
/// and ABI-stable within a minor version.
///
/// `enable_stable_row_ids` is a `bool` and therefore has **no default
/// sentinel** — whatever the caller writes is forwarded verbatim to upstream.
/// Callers that zero-initialize this struct (the documented way to inherit
/// other defaults) end up explicitly setting `enable_stable_row_ids = false`.
/// This matches upstream's current default, so the behavior is identical
/// today; if upstream ever changes that default, callers must set this field
/// explicitly to follow.
#[repr(C)]
pub struct LanceWriteParams {
    /// Soft cap on rows per data file. `0` uses upstream's default.
    pub max_rows_per_file: u64,
    /// Soft cap on rows per row group. `0` uses upstream's default.
    pub max_rows_per_group: u64,
    /// Soft cap on bytes per data file (~90 GB by default). `0` uses upstream's default.
    pub max_bytes_per_file: u64,
    /// Lance file format version string, e.g. `"2.0"`, `"2.1"`, `"stable"`,
    /// `"legacy"`. NULL uses upstream's default. Invalid strings are rejected
    /// with `LANCE_ERR_INVALID_ARGUMENT`.
    pub data_storage_version: *const c_char,
    /// Strictly an override (no default sentinel — see struct-level docs).
    /// Whatever value the caller writes is forwarded to upstream.
    pub enable_stable_row_ids: bool,
}

/// Write an Arrow record batch stream to a Lance dataset at `uri`, committing a manifest.
///
/// - `uri`: Dataset URI (`file://`, `s3://`, `memory://`, ...). Must not be NULL or empty.
/// - `schema`: Caller-provided Arrow schema. The stream's schema must match;
///   mismatch returns `LANCE_ERR_INVALID_ARGUMENT`.
/// - `stream`: Arrow C Data Interface stream. Consumed by this call — the
///   caller must not use it again on any return path.
/// - `mode`: `LANCE_WRITE_CREATE` (0), `LANCE_WRITE_APPEND` (1), or
///   `LANCE_WRITE_OVERWRITE` (2). Any other value → `LANCE_ERR_INVALID_ARGUMENT`.
/// - `storage_opts`: NULL-terminated key-value pairs `["k","v",NULL]`, or NULL.
/// - `out_dataset`: If non-NULL, receives an open `LanceDataset*` at the new
///   version on success (caller closes). Pass NULL to discard. On error
///   `*out_dataset` is untouched — do not read or free it.
///
/// Equivalent to `lance_dataset_write_with_params(..., params = NULL, ...)`.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_write(
    uri: *const c_char,
    schema: *const FFI_ArrowSchema,
    stream: *mut FFI_ArrowArrayStream,
    mode: i32,
    storage_opts: *const *const c_char,
    out_dataset: *mut *mut LanceDataset,
) -> i32 {
    unsafe {
        lance_dataset_write_with_params(
            uri,
            schema,
            stream,
            mode,
            std::ptr::null(),
            storage_opts,
            out_dataset,
        )
    }
}

/// Same as `lance_dataset_write` but takes a `LanceWriteParams` for tuning the
/// output shape. Pass `params = NULL` to use upstream defaults.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_write_with_params(
    uri: *const c_char,
    schema: *const FFI_ArrowSchema,
    stream: *mut FFI_ArrowArrayStream,
    mode: i32,
    params: *const LanceWriteParams,
    storage_opts: *const *const c_char,
    out_dataset: *mut *mut LanceDataset,
) -> i32 {
    ffi_try!(
        unsafe {
            write_dataset_inner(uri, schema, stream, mode, params, storage_opts, out_dataset)
        },
        neg
    )
}

unsafe fn write_dataset_inner(
    uri: *const c_char,
    schema: *const FFI_ArrowSchema,
    stream: *mut FFI_ArrowArrayStream,
    mode: i32,
    params: *const LanceWriteParams,
    storage_opts: *const *const c_char,
    out_dataset: *mut *mut LanceDataset,
) -> Result<i32> {
    // The stream NULL check is the only validation that runs *before* the
    // stream is consumed; once `from_raw` succeeds, every other return path
    // drops `reader`, which fires the FFI release callback. Reordering the
    // uri/schema NULL checks ahead of `from_raw` would leak the stream on
    // those paths and break the documented "consumed on every return" contract.
    if stream.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "stream must not be NULL".into(),
            location: snafu::location!(),
        });
    }

    // SAFETY: `stream` is non-NULL (checked above) and the caller guarantees
    // it points to an initialized, properly-aligned `FFI_ArrowArrayStream`
    // owned by them. `from_raw` performs a `ptr::replace` that transfers
    // ownership into the returned reader, zeroing the caller's release
    // callback so it cannot be released twice.
    let reader = unsafe { ArrowArrayStreamReader::from_raw(stream) }.map_err(|e| {
        lance_core::Error::InvalidInput {
            source: e.to_string().into(),
            location: snafu::location!(),
        }
    })?;

    if uri.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "uri must not be NULL".into(),
            location: snafu::location!(),
        });
    }
    if schema.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "schema must not be NULL".into(),
            location: snafu::location!(),
        });
    }

    // Validate the mode at the boundary — storing an out-of-range tag as a
    // `LanceWriteMode` would be UB.
    let mode = LanceWriteMode::from_raw(mode)?;

    // SAFETY: `uri` is non-NULL (checked above) and the caller guarantees it
    // points to a NUL-terminated C string that lives for the duration of this
    // call. `parse_c_string`'s lifetime parameter is unconstrained, so we rely
    // on the borrow being used only within this synchronous function body —
    // which `block_on` enforces by completing before this function returns.
    let uri_str = unsafe { helpers::parse_c_string(uri)? }
        .filter(|s| !s.is_empty())
        .ok_or_else(|| lance_core::Error::InvalidInput {
            // NULL is rejected above; only the empty case reaches here.
            source: "uri must not be empty".into(),
            location: snafu::location!(),
        })?;

    // SAFETY: `schema` is non-NULL (checked above) and the caller guarantees
    // it points to a properly-initialized `FFI_ArrowSchema` valid for the
    // duration of this call. `try_from(&FFI_ArrowSchema)` reads by shared
    // reference and does not move out of or release the schema.
    let expected_schema = ArrowSchema::try_from(unsafe { &*schema }).map_err(|e| {
        lance_core::Error::InvalidInput {
            source: format!("invalid schema: {e}").into(),
            location: snafu::location!(),
        }
    })?;

    // SAFETY: `storage_opts` is either NULL or a NULL-terminated array of
    // C-string pointers per the FFI contract; `parse_storage_options` returns
    // an empty map for NULL.
    let opts = unsafe { helpers::parse_storage_options(storage_opts)? };

    // Fail fast: compare the stream schema against the caller-provided schema.
    let stream_schema = reader.schema();
    if stream_schema.fields() != expected_schema.fields() {
        return Err(lance_core::Error::InvalidInput {
            source: format!(
                "stream schema does not match the provided schema.\n  expected: {expected_schema}\n  got:      {stream_schema}"
            )
            .into(),
            location: snafu::location!(),
        });
    }

    let store_params = (!opts.is_empty()).then(|| ObjectStoreParams {
        storage_options_accessor: Some(Arc::new(StorageOptionsAccessor::with_static_options(opts))),
        ..ObjectStoreParams::default()
    });
    let mut write_params = WriteParams {
        mode: mode.into(),
        store_params,
        ..WriteParams::default()
    };
    if !params.is_null() {
        // SAFETY: `params` is non-NULL (checked above) and the caller
        // guarantees it points to a properly-initialized `LanceWriteParams`
        // valid for the duration of this call. We borrow by shared reference
        // and only read; the borrow is consumed before any await point.
        unsafe { apply_write_params(&mut write_params, &*params)? };
    }

    let dataset = block_on(Dataset::write(reader, uri_str, Some(write_params)))?;

    if !out_dataset.is_null() {
        let handle = LanceDataset {
            inner: RwLock::new(Arc::new(dataset)),
        };
        // SAFETY: `out_dataset` is non-NULL (checked above) and the caller
        // guarantees it points to caller-owned, writable storage of size
        // `sizeof(LanceDataset*)`. We only write on success; on any error
        // path the early returns above leave `*out_dataset` untouched.
        unsafe {
            *out_dataset = Box::into_raw(Box::new(handle));
        }
    }

    Ok(0)
}

/// Apply caller-provided overrides onto an `lance::WriteParams`. Zero / NULL
/// fields are no-ops so upstream defaults flow through.
unsafe fn apply_write_params(target: &mut WriteParams, params: &LanceWriteParams) -> Result<()> {
    if params.max_rows_per_file > 0 {
        target.max_rows_per_file = u64_to_usize(params.max_rows_per_file, "max_rows_per_file")?;
    }
    if params.max_rows_per_group > 0 {
        target.max_rows_per_group = u64_to_usize(params.max_rows_per_group, "max_rows_per_group")?;
    }
    if params.max_bytes_per_file > 0 {
        target.max_bytes_per_file = u64_to_usize(params.max_bytes_per_file, "max_bytes_per_file")?;
    }
    if !params.data_storage_version.is_null() {
        // SAFETY: `data_storage_version` is non-NULL (checked above) and the
        // `apply_write_params` caller (`unsafe fn` contract) guarantees it
        // points to a NUL-terminated C string valid for the duration of the
        // outer FFI call. `parse_c_string` reads by shared reference and the
        // returned borrow is consumed before this function returns.
        //
        // `parse_c_string` returns `None` only for NULL input, which the
        // outer check already ruled out. `.filter` lets an empty C string
        // also fail presence, producing the clearer message below instead
        // of relying on `FromStr`'s generic "unknown version" path.
        let s = unsafe { helpers::parse_c_string(params.data_storage_version)? }
            .filter(|s| !s.is_empty())
            .ok_or_else(|| lance_core::Error::InvalidInput {
                source: "data_storage_version must not be an empty string".into(),
                location: snafu::location!(),
            })?;
        let version =
            LanceFileVersion::from_str(s).map_err(|e| lance_core::Error::InvalidInput {
                source: format!("invalid data_storage_version {s:?}: {e}").into(),
                location: snafu::location!(),
            })?;
        target.data_storage_version = Some(version);
    }
    target.enable_stable_row_ids = params.enable_stable_row_ids;
    Ok(())
}

/// Narrow `u64 -> usize` with an explicit error on overflow (32-bit targets).
/// Realistic write tunings fit in `usize` on every supported target, but a
/// silent `as` cast would wrap on a 32-bit host.
fn u64_to_usize(v: u64, field: &'static str) -> Result<usize> {
    usize::try_from(v).map_err(|_| lance_core::Error::InvalidInput {
        source: format!("{field}={v} exceeds usize::MAX on this target").into(),
        location: snafu::location!(),
    })
}
