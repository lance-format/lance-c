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
use chrono::TimeDelta;
use lance::Dataset;
use lance::dataset::{AutoCleanupParams, WriteMode as LanceWriteModeUpstream, WriteParams};
use lance::session::Session;
use lance_core::Result;
use lance_file::version::LanceFileVersion;
use lance_io::object_store::{ObjectStoreParams, StorageOptionsAccessor};

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::block_on;
use crate::storage_options::{LanceStorageOptionsAccessor, storage_options_accessor_from_ffi};

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
    pub(crate) fn from_raw(raw: i32) -> Result<Self> {
        match raw {
            0 => Ok(Self::Create),
            1 => Ok(Self::Append),
            2 => Ok(Self::Overwrite),
            other => Err(lance_core::Error::invalid_input_source(
                format!(
                    "invalid write mode {other}; expected 0 (create), 1 (append), or 2 (overwrite)"
                )
                .into(),
            )),
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

/// Native automatic-cleanup parameters for `lance_dataset_write_with_options`.
///
/// `interval` is the number of committed versions between cleanup runs; `0`
/// is a valid native value that considers cleanup on every commit.
///
/// `older_than_seconds` and `older_than_nanos` together express the native
/// `chrono::TimeDelta` retention window as
/// `older_than_seconds + older_than_nanos / 1_000_000_000`. Both fields carry
/// the sign, and the nanosecond component preserves the native precision.
/// `older_than_nanos` must be in `[-999_999_999, 999_999_999]`, and the
/// combined value must fit in `chrono::TimeDelta` (about +/-292 million
/// years). Negative durations are forwarded to Lance, which rejects them when
/// it builds the write transaction because the persisted config uses
/// `std::time::Duration`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceAutoCleanupParams {
    /// Number of committed versions between cleanup runs. `0` is valid.
    pub interval: u64,
    /// Whole-second component of the retention window.
    pub older_than_seconds: i64,
    /// Signed subsecond component of the retention window, in nanoseconds.
    pub older_than_nanos: i32,
}

/// Native write options for `lance_dataset_write_with_options`.
///
/// Zero-initialize this struct to select `WriteParams::default()`:
/// `auto_cleanup = None` and `skip_auto_cleanup = false`. The embedded
/// `params` keeps the zero-means-upstream-default mapping documented on
/// `LanceWriteParams`.
///
/// A NULL `auto_cleanup` selects native `None`; a non-NULL pointer selects
/// `Some`, including an explicit `interval = 0` or a zero-length retention
/// window. `skip_auto_cleanup` affects only the current commit and never
/// changes persisted configuration.
#[repr(C)]
pub struct LanceWriteOptions {
    /// Output-shape overrides. Zero-initialize to inherit upstream defaults.
    pub params: LanceWriteParams,
    /// Native `WriteParams::auto_cleanup`; NULL means `None`.
    pub auto_cleanup: *const LanceAutoCleanupParams,
    /// Native `WriteParams::skip_auto_cleanup`.
    pub skip_auto_cleanup: bool,
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
/// This legacy entry point records the documented 20-version / 14-day
/// auto-cleanup policy. Use `lance_dataset_write_with_options` for native
/// `WriteParams` defaults.
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
    // Wrap the shared inner fn directly rather than forwarding to the
    // `lance_dataset_write_with_params` extern: both extern "C" frames must
    // carry their own panic guard, and a guard around the forwarding call
    // would clear the thread-local error the inner guard just set (its
    // `-1` return looks like `Ok(-1)` to an outer guard).
    ffi_try!(
        unsafe {
            write_dataset_inner(
                uri,
                schema,
                stream,
                mode,
                WriteSettings {
                    params: std::ptr::null(),
                    options: std::ptr::null(),
                    legacy_auto_cleanup: true,
                    storage_options_accessor: std::ptr::null(),
                },
                storage_opts,
                out_dataset,
            )
        },
        neg
    )
}

/// Same as `lance_dataset_write` but takes a `LanceWriteParams` for tuning the
/// output shape. Pass `params = NULL` to use upstream defaults.
///
/// This legacy entry point records the documented 20-version / 14-day
/// auto-cleanup policy. Use `lance_dataset_write_with_options` for native
/// `WriteParams` defaults.
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
            write_dataset_inner(
                uri,
                schema,
                stream,
                mode,
                WriteSettings {
                    params,
                    options: std::ptr::null(),
                    legacy_auto_cleanup: true,
                    storage_options_accessor: std::ptr::null(),
                },
                storage_opts,
                out_dataset,
            )
        },
        neg
    )
}

/// Same as `lance_dataset_write` but selects native `WriteParams` defaults
/// and optional auto-cleanup settings. Pass `options = NULL` to use a
/// zero-initialized `LanceWriteOptions`, which is equivalent to
/// `WriteParams::default()`: `auto_cleanup = None`, `skip_auto_cleanup =
/// false`.
///
/// On an existing dataset, `auto_cleanup` is written only by CREATE;
/// APPEND and OVERWRITE leave the persisted policy unchanged. An explicit
/// `auto_cleanup = None` on an existing manifest therefore does not delete
/// stored cleanup configuration.
///
/// The stream is consumed on every return path except a NULL `stream`.
/// `out_dataset` follows the same contract as `lance_dataset_write`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_write_with_options(
    uri: *const c_char,
    schema: *const FFI_ArrowSchema,
    stream: *mut FFI_ArrowArrayStream,
    mode: i32,
    options: *const LanceWriteOptions,
    storage_opts: *const *const c_char,
    out_dataset: *mut *mut LanceDataset,
) -> i32 {
    ffi_try!(
        unsafe {
            write_dataset_inner(
                uri,
                schema,
                stream,
                mode,
                WriteSettings {
                    params: std::ptr::null(),
                    options,
                    legacy_auto_cleanup: false,
                    storage_options_accessor: std::ptr::null(),
                },
                storage_opts,
                out_dataset,
            )
        },
        neg
    )
}

/// Write with a native storage-options accessor and the legacy 20/14
/// auto-cleanup policy.
///
/// `storage_options_accessor` may be NULL for native defaults. Static storage
/// options belong in the accessor's initial options, so no `storage_opts`
/// parameter is accepted here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_write_with_accessor(
    uri: *const c_char,
    schema: *const FFI_ArrowSchema,
    stream: *mut FFI_ArrowArrayStream,
    mode: i32,
    storage_options_accessor: *const LanceStorageOptionsAccessor,
    out_dataset: *mut *mut LanceDataset,
) -> i32 {
    ffi_try!(
        unsafe {
            write_dataset_inner(
                uri,
                schema,
                stream,
                mode,
                WriteSettings {
                    params: std::ptr::null(),
                    options: std::ptr::null(),
                    legacy_auto_cleanup: true,
                    storage_options_accessor,
                },
                std::ptr::null(),
                out_dataset,
            )
        },
        neg
    )
}

/// Write with native `LanceWriteParams` and a storage-options accessor.
///
/// `params` may be NULL for native defaults. `storage_options_accessor` may be
/// NULL for native defaults. This entry point keeps the legacy 20/14
/// auto-cleanup policy, matching `lance_dataset_write_with_params`.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn lance_dataset_write_with_params_and_accessor(
    uri: *const c_char,
    schema: *const FFI_ArrowSchema,
    stream: *mut FFI_ArrowArrayStream,
    mode: i32,
    params: *const LanceWriteParams,
    storage_options_accessor: *const LanceStorageOptionsAccessor,
    out_dataset: *mut *mut LanceDataset,
) -> i32 {
    ffi_try!(
        unsafe {
            write_dataset_inner(
                uri,
                schema,
                stream,
                mode,
                WriteSettings {
                    params,
                    options: std::ptr::null(),
                    legacy_auto_cleanup: true,
                    storage_options_accessor,
                },
                std::ptr::null(),
                out_dataset,
            )
        },
        neg
    )
}

/// Write with native `LanceWriteOptions` and a storage-options accessor.
///
/// `options` may be NULL for a zero-initialized `LanceWriteOptions`, which
/// selects native `WriteParams::default()`. `storage_options_accessor` may be
/// NULL for native defaults.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn lance_dataset_write_with_options_and_accessor(
    uri: *const c_char,
    schema: *const FFI_ArrowSchema,
    stream: *mut FFI_ArrowArrayStream,
    mode: i32,
    options: *const LanceWriteOptions,
    storage_options_accessor: *const LanceStorageOptionsAccessor,
    out_dataset: *mut *mut LanceDataset,
) -> i32 {
    ffi_try!(
        unsafe {
            write_dataset_inner(
                uri,
                schema,
                stream,
                mode,
                WriteSettings {
                    params: std::ptr::null(),
                    options,
                    legacy_auto_cleanup: false,
                    storage_options_accessor,
                },
                std::ptr::null(),
                out_dataset,
            )
        },
        neg
    )
}

/// Internal selection between the legacy write entry points and the native
/// options entry point. Raw pointers stay valid for the synchronous FFI call.
#[derive(Clone, Copy)]
struct WriteSettings {
    params: *const LanceWriteParams,
    options: *const LanceWriteOptions,
    legacy_auto_cleanup: bool,
    storage_options_accessor: *const LanceStorageOptionsAccessor,
}

/// Build native `WriteParams` from the writer option representations.
///
/// This is shared by the committing writer entry points and the uncommitted
/// transaction entry point so their option/default conversion cannot drift.
/// The C pointers are borrowed only for this synchronous call.
pub(crate) unsafe fn write_params_from_ffi(
    mode: LanceWriteMode,
    params: *const LanceWriteParams,
    options: *const LanceWriteOptions,
    legacy_auto_cleanup: bool,
    storage_opts: *const *const c_char,
    storage_options_accessor: *const LanceStorageOptionsAccessor,
    session: Option<Arc<Session>>,
) -> Result<WriteParams> {
    // SAFETY: `storage_opts` is either NULL or a NULL-terminated array of
    // C-string pointers per the FFI contract.
    let opts = unsafe { helpers::parse_storage_options(storage_opts)? };
    let accessor = unsafe { storage_options_accessor_from_ffi(storage_options_accessor)? };
    if accessor.is_some() && !opts.is_empty() {
        return Err(lance_core::Error::invalid_input_source(
            "storage_opts must be NULL when a storage options accessor is provided".into(),
        ));
    }
    let store_params = if let Some(accessor) = accessor {
        Some(ObjectStoreParams {
            storage_options_accessor: Some(accessor),
            ..ObjectStoreParams::default()
        })
    } else {
        (!opts.is_empty()).then(|| ObjectStoreParams {
            storage_options_accessor: Some(Arc::new(StorageOptionsAccessor::with_static_options(
                opts,
            ))),
            ..ObjectStoreParams::default()
        })
    };
    let mut write_params = WriteParams {
        mode: mode.into(),
        store_params,
        session,
        ..WriteParams::default()
    };

    if !options.is_null() {
        // SAFETY: a non-NULL `options` points to a valid `LanceWriteOptions`
        // for the duration of this call. The borrow ends before any await.
        let options = unsafe { &*options };
        unsafe { apply_write_params(&mut write_params, &options.params)? };
        if !options.auto_cleanup.is_null() {
            // SAFETY: the caller guarantees a non-NULL `auto_cleanup` points
            // to a valid `LanceAutoCleanupParams` for this call.
            write_params.auto_cleanup =
                Some(unsafe { auto_cleanup_from_ffi(&*options.auto_cleanup)? });
        }
        write_params.skip_auto_cleanup = options.skip_auto_cleanup;
    } else if !params.is_null() {
        // SAFETY: a non-NULL `params` points to a valid `LanceWriteParams` for
        // the duration of this call.
        unsafe { apply_write_params(&mut write_params, &*params)? };
    }

    if legacy_auto_cleanup {
        // Lance 9.1 changed the upstream default to `auto_cleanup: None`.
        // The long-standing C entry points still expose the pre-9.1 policy
        // (every 20 versions, older than 14 days) so datasets created through
        // them keep their reclamation path. Pin the values explicitly rather
        // than via `AutoCleanupParams::default()`, so a future upstream
        // default change cannot silently shift C-visible behavior.
        write_params.auto_cleanup = Some(legacy_auto_cleanup_params());
    }

    Ok(write_params)
}

unsafe fn write_dataset_inner(
    uri: *const c_char,
    schema: *const FFI_ArrowSchema,
    stream: *mut FFI_ArrowArrayStream,
    mode: i32,
    settings: WriteSettings,
    storage_opts: *const *const c_char,
    out_dataset: *mut *mut LanceDataset,
) -> Result<i32> {
    // The stream NULL check is the only validation that runs *before* the
    // stream is consumed; once `from_raw` succeeds, every other return path
    // drops `reader`, which fires the FFI release callback. Reordering the
    // uri/schema NULL checks ahead of `from_raw` would leak the stream on
    // those paths and break the documented "consumed on every return" contract.
    if stream.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "stream must not be NULL".into(),
        ));
    }

    // SAFETY: `stream` is non-NULL (checked above) and the caller guarantees
    // it points to an initialized, properly-aligned `FFI_ArrowArrayStream`
    // owned by them. `from_raw` performs a `ptr::replace` that transfers
    // ownership into the returned reader, zeroing the caller's release
    // callback so it cannot be released twice.
    let reader = unsafe { ArrowArrayStreamReader::from_raw(stream) }
        .map_err(|e| lance_core::Error::invalid_input_source(e.to_string().into()))?;

    if uri.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "uri must not be NULL".into(),
        ));
    }
    if schema.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "schema must not be NULL".into(),
        ));
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
        // NULL is rejected above; only the empty case reaches here.
        .ok_or_else(|| lance_core::Error::invalid_input("uri must not be empty"))?;

    // SAFETY: `schema` is non-NULL (checked above) and the caller guarantees
    // it points to a properly-initialized `FFI_ArrowSchema` valid for the
    // duration of this call. `try_from(&FFI_ArrowSchema)` reads by shared
    // reference and does not move out of or release the schema.
    let expected_schema = ArrowSchema::try_from(unsafe { &*schema }).map_err(|e| {
        lance_core::Error::invalid_input_source(format!("invalid schema: {e}").into())
    })?;

    let write_params = unsafe {
        write_params_from_ffi(
            mode,
            settings.params,
            settings.options,
            settings.legacy_auto_cleanup,
            storage_opts,
            settings.storage_options_accessor,
            None,
        )?
    };

    // Fail fast: compare the stream schema against the caller-provided schema.
    let stream_schema = reader.schema();
    if stream_schema.fields() != expected_schema.fields() {
        return Err(lance_core::Error::invalid_input_source(format!(
                "stream schema does not match the provided schema.\n  expected: {expected_schema}\n  got:      {stream_schema}"
            )
            .into()));
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

/// The documented legacy lance-c cleanup policy.
fn legacy_auto_cleanup_params() -> AutoCleanupParams {
    AutoCleanupParams {
        interval: 20,
        older_than: TimeDelta::days(14),
    }
}

/// Convert the C representation into native `AutoCleanupParams`.
///
/// The nanosecond component must be already normalized to the native
/// `TimeDelta` subsecond range. Out-of-range values and durations outside
/// `chrono::TimeDelta` are rejected with context instead of being clamped.
fn auto_cleanup_from_ffi(raw: &LanceAutoCleanupParams) -> Result<AutoCleanupParams> {
    let interval = u64_to_usize(raw.interval, "auto_cleanup.interval")?;
    let older_than =
        helpers::time_delta_from_ffi(raw.older_than_seconds, raw.older_than_nanos, "auto_cleanup")?;

    Ok(AutoCleanupParams {
        interval,
        older_than,
    })
}

/// Parse a nullable native Lance file-version string.
pub(crate) unsafe fn parse_storage_version(
    ptr: *const c_char,
    field: &str,
) -> Result<Option<LanceFileVersion>> {
    if ptr.is_null() {
        return Ok(None);
    }
    // SAFETY: the caller guarantees a non-NULL `ptr` is a NUL-terminated
    // string valid for the duration of the outer FFI call.
    let value = unsafe { helpers::parse_c_string(ptr)? }
        .filter(|value| !value.is_empty())
        .ok_or_else(|| {
            lance_core::Error::invalid_input_source(
                format!("{field} must not be an empty string").into(),
            )
        })?;
    let version = LanceFileVersion::from_str(value).map_err(|error| {
        lance_core::Error::invalid_input_source(
            format!("invalid {field} {value:?}: {error}").into(),
        )
    })?;
    Ok(Some(version))
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
        target.data_storage_version =
            unsafe { parse_storage_version(params.data_storage_version, "data_storage_version")? };
    }
    target.enable_stable_row_ids = params.enable_stable_row_ids;
    Ok(())
}

/// Narrow `u64 -> usize` with an explicit error on overflow (32-bit targets).
/// Realistic write tunings fit in `usize` on every supported target, but a
/// silent `as` cast would wrap on a 32-bit host.
fn u64_to_usize(v: u64, field: &'static str) -> Result<usize> {
    usize::try_from(v).map_err(|_| {
        lance_core::Error::invalid_input_source(
            format!("{field}={v} exceeds usize::MAX on this target").into(),
        )
    })
}
