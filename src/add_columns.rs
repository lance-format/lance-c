// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Add columns C API: append new columns to a dataset, committing a new
//! manifest. Three variants mirror the upstream `NewColumnTransform` cases
//! that translate cleanly across the C ABI:
//!
//! * `..._sql`    — derive columns from SQL expressions over existing columns.
//! * `..._nulls`  — add all-null columns described by an Arrow schema; this is
//!   a metadata-only operation on non-legacy datasets.
//! * `..._stream` — splice in precomputed column data from an Arrow C stream,
//!   aligned to the dataset's existing rows in order.
//!
//! The upstream `BatchUDF` variant is intentionally omitted: it carries a Rust
//! closure that cannot cross the C ABI. The `_stream` variant covers the same
//! "bring your own computed data" use case.
//!
//! Each call mutates the dataset in place under an exclusive write lock;
//! existing scanners that already cloned the inner Arc keep their pre-add view.

use std::ffi::c_char;
use std::sync::Arc;

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use arrow_schema::Schema as ArrowSchema;
use lance::dataset::NewColumnTransform;
use lance_core::Result;
use snafu::location;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::block_on;

/// A single new column defined by a SQL expression over the dataset's existing
/// columns, e.g. `name = "doubled"`, `expression = "x * 2"`. Both fields are
/// required, non-empty UTF-8; the strings are read by shared reference for the
/// duration of the call.
#[repr(C)]
pub struct LanceSqlColumn {
    /// Name of the new column. Required, non-empty UTF-8.
    pub name: *const c_char,
    /// SQL expression evaluated against existing columns. Required, non-empty.
    pub expression: *const c_char,
}

/// Add one or more columns computed from SQL expressions over the dataset's
/// existing columns, committing a new manifest. Each fragment is scanned, the
/// expressions are evaluated over each Arrow batch, and the results are written
/// as new column files.
///
/// - `dataset`: Open dataset (mutated; same handle remains valid afterward).
///   Must not be NULL.
/// - `columns`: Pointer to an array of `LanceSqlColumn`. Must not be NULL.
/// - `num_columns`: Length of the `columns` array. Must be non-zero.
/// - `batch_size`: Rows per scan batch while evaluating expressions. `0` uses
///   the upstream default.
///
/// Returns 0 on success, -1 on error. Error codes:
/// `LANCE_ERR_INVALID_ARGUMENT` for NULL/empty args, NULL or empty `name` /
/// `expression`, non-UTF-8 strings, malformed SQL *syntax*, a new column whose
/// name collides with an existing column, or a `batch_size` that exceeds
/// `u32::MAX`. An expression that references a *non-existent column*
/// is resolved by the upstream planner and surfaces as `LANCE_ERR_INTERNAL`
/// (an upstream schema error, the same path as `lance_dataset_delete`); we do
/// not re-classify it at the FFI boundary. `LANCE_ERR_COMMIT_CONFLICT` for a
/// concurrent writer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_add_columns_sql(
    dataset: *mut LanceDataset,
    columns: *const LanceSqlColumn,
    num_columns: usize,
    batch_size: u64,
) -> i32 {
    ffi_try!(
        unsafe { add_columns_sql_inner(dataset, columns, num_columns, batch_size) },
        neg
    )
}

unsafe fn add_columns_sql_inner(
    dataset: *mut LanceDataset,
    columns: *const LanceSqlColumn,
    num_columns: usize,
    batch_size: u64,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "dataset must not be NULL".into(),
            location: location!(),
        });
    }
    if columns.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "columns must not be NULL".into(),
            location: location!(),
        });
    }
    if num_columns == 0 {
        return Err(lance_core::Error::InvalidInput {
            source: "num_columns must be > 0".into(),
            location: location!(),
        });
    }

    let batch_size = resolve_batch_size(batch_size)?;

    // Materialize the (name, expression) pairs up front so a precise per-index
    // error fires before the dataset's write lock is taken — matches the
    // pre-lock validation pattern used by the sibling schema-evolution APIs.
    let mut pairs: Vec<(String, String)> = Vec::with_capacity(num_columns);
    for i in 0..num_columns {
        // SAFETY: `columns` is non-NULL (checked above) and the caller
        // guarantees the array has at least `num_columns` initialised entries
        // valid for this call. Each entry's `name` / `expression` pointers are
        // dereferenced by `parse_required_field` under the same guarantee.
        let entry = unsafe { &*columns.add(i) };
        let name = unsafe { parse_required_field(entry.name, i, "name")? };
        let expression = unsafe { parse_required_field(entry.expression, i, "expression")? };
        pairs.push((name, expression));
    }

    let transform = NewColumnTransform::SqlExpressions(pairs);

    // SAFETY: `dataset` is non-NULL (checked above) and the caller guarantees
    // it points to a live `LanceDataset`. `with_mut` takes an exclusive write
    // lock on the inner `Arc<Dataset>` before yielding `&mut Dataset`, so a
    // shared `&*dataset` borrow here is sound.
    let ds = unsafe { &*dataset };
    ds.with_mut(|d| block_on(d.add_columns(transform, None, batch_size)))?;
    Ok(0)
}

/// Add one or more all-null columns described by an Arrow C Data Interface
/// schema, committing a new manifest. On non-legacy datasets this is a
/// metadata-only operation — no data files are rewritten. Every field in the
/// schema must be nullable (an all-null column cannot be non-nullable).
///
/// - `dataset`: Open dataset (mutated; same handle remains valid afterward).
///   Must not be NULL.
/// - `schema`: Arrow C `ArrowSchema` describing the new columns. Read by shared
///   reference; its `release` callback is never invoked. Must not be NULL. Only
///   the top-level schema is validated before it is handed to arrow-rs; the
///   caller is responsible for providing fully-initialised child fields.
///
/// Returns 0 on success, -1 on error. Error codes:
/// `LANCE_ERR_INVALID_ARGUMENT` for a NULL `dataset` / `schema`, an
/// uninitialised or already-released schema, a schema that is not a valid
/// Arrow schema, a non-nullable field, or a name that collides with an existing
/// column. `LANCE_ERR_NOT_SUPPORTED` for a legacy-format dataset (which cannot
/// take all-null columns as a metadata-only change). `LANCE_ERR_COMMIT_CONFLICT`
/// for a concurrent writer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_add_columns_nulls(
    dataset: *mut LanceDataset,
    schema: *const FFI_ArrowSchema,
) -> i32 {
    ffi_try!(unsafe { add_columns_nulls_inner(dataset, schema) }, neg)
}

unsafe fn add_columns_nulls_inner(
    dataset: *mut LanceDataset,
    schema: *const FFI_ArrowSchema,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "dataset must not be NULL".into(),
            location: location!(),
        });
    }
    if schema.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "schema must not be NULL".into(),
            location: location!(),
        });
    }

    // SAFETY: `schema` is non-NULL (checked above) and the caller guarantees it
    // points to a valid `FFI_ArrowSchema` for the duration of this call. We
    // read by shared reference and never invoke its release callback.
    let ffi_schema = unsafe { &*schema };
    // Reject an already-released or never-initialised schema before handing it
    // to arrow-rs, which would otherwise `assert!` on the NULL `format` field
    // and abort the host process under our `panic = "abort"` profile. Both
    // checks are intentional — `release == NULL` is the canonical Arrow C Data
    // Interface "released" sentinel, while `format == NULL` catches a
    // zero-initialised or half-built struct that would slip past the release
    // check.
    if ffi_schema.release.is_none() || ffi_schema.format.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "schema is uninitialised or already released".into(),
            location: location!(),
        });
    }
    // arrow-rs's `FFI_ArrowSchema::format()` does `to_str().expect(..)` on the
    // format pointer; a non-NULL but non-UTF-8 top-level format would abort the
    // process under `panic = "abort"`. Validate it here so a malformed format
    // surfaces as INVALID_ARGUMENT instead. (Child fields are still the caller's
    // responsibility — see the doc comment — as walking them would duplicate
    // arrow-rs's recursive descent.)
    //
    // SAFETY: `format` is non-NULL (checked above) and, per the caller's CADI
    // contract, points to a NUL-terminated C string valid for this call.
    if unsafe { std::ffi::CStr::from_ptr(ffi_schema.format) }
        .to_str()
        .is_err()
    {
        return Err(lance_core::Error::InvalidInput {
            source: "schema format string is not valid UTF-8".into(),
            location: location!(),
        });
    }
    let arrow_schema =
        ArrowSchema::try_from(ffi_schema).map_err(|e| lance_core::Error::InvalidInput {
            source: format!("schema is not a valid Arrow schema: {e}").into(),
            location: location!(),
        })?;

    let transform = NewColumnTransform::AllNulls(Arc::new(arrow_schema));

    // SAFETY: `dataset` is non-NULL (checked above); see `add_columns_sql_inner`
    // for the `with_mut` locking justification.
    let ds = unsafe { &*dataset };
    ds.with_mut(|d| block_on(d.add_columns(transform, None, None)))?;
    Ok(0)
}

/// Add columns by splicing precomputed data from an Arrow C Data Interface
/// stream into the dataset, committing a new manifest. The stream's batches are
/// consumed in order and aligned positionally to the dataset's existing rows:
/// the total row count must match the dataset exactly, or the call fails.
///
/// - `dataset`: Open dataset (mutated; same handle remains valid afterward).
///   Must not be NULL.
/// - `stream`: Arrow C stream of the new column data. When non-NULL it is
///   consumed (released) on every return path, including error returns — the
///   caller must not use it again. (A NULL `stream` is rejected before anything
///   is consumed.) Its schema defines the new columns and must not collide with
///   existing column names.
/// - `batch_size`: Rows per write batch while aligning the stream to fragments.
///   `0` uses the upstream default.
///
/// Returns 0 on success, -1 on error. Error codes:
/// `LANCE_ERR_INVALID_ARGUMENT` for a NULL `dataset` / `stream`, a stream
/// missing a mandatory `get_schema` / `get_next` / `release` callback, a stream
/// whose total row count does not match the dataset, a new column whose name
/// collides with an existing column, or a `batch_size` that exceeds `u32::MAX`.
/// `LANCE_ERR_COMMIT_CONFLICT` for a concurrent writer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_add_columns_stream(
    dataset: *mut LanceDataset,
    stream: *mut FFI_ArrowArrayStream,
    batch_size: u64,
) -> i32 {
    ffi_try!(
        unsafe { add_columns_stream_inner(dataset, stream, batch_size) },
        neg
    )
}

unsafe fn add_columns_stream_inner(
    dataset: *mut LanceDataset,
    stream: *mut FFI_ArrowArrayStream,
    batch_size: u64,
) -> Result<i32> {
    // The stream NULL check is the only validation that runs *before* the
    // stream is consumed. Once `from_raw` succeeds, every later return path
    // (dataset / batch_size) drops `reader`, which fires the FFI release
    // callback — so those checks are deliberately deferred to after `from_raw`.
    // Moving them ahead of `from_raw` would early-return without releasing the
    // caller's stream, breaking the documented "consumed on every return"
    // contract. (This NULL-before-`from_raw` ordering matches `merge_insert.rs`;
    // the callback pre-flight guard below is specific to this function.)
    if stream.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "stream must not be NULL".into(),
            location: location!(),
        });
    }

    // Reject a stream missing a mandatory C Data Interface callback *before*
    // handing it to arrow-rs. `ArrowArrayStreamReader` only guards against a
    // NULL `release`; a NULL `get_schema` or `get_next` would otherwise reach an
    // `unwrap()` deep inside arrow-rs and abort the host process under our
    // `panic = "abort"` profile. We do not require `get_last_error` (the spec
    // marks it optional): requiring it would not close the abort anyway, since a
    // present callback that *returns* NULL at error time hits the same
    // `last_error.unwrap()` on arrow-rs's `get_next` error path — a residual
    // upstream limitation reachable only by a stream that signals an error
    // without a message, which we cannot guard at intake.
    //
    // SAFETY: `stream` is non-NULL (checked above) and the caller guarantees it
    // points to an initialised, properly-aligned `FFI_ArrowArrayStream`. The
    // callback slots are `Copy` function pointers; this shared borrow ends
    // before any ownership transfer below.
    let (release, has_schema_cb, has_next_cb) = unsafe {
        let raw = &*stream;
        (
            raw.release,
            raw.get_schema.is_some(),
            raw.get_next.is_some(),
        )
    };
    if release.is_none() || !has_schema_cb || !has_next_cb {
        // Preserve the "consumed on every return path" contract: release the
        // stream ourselves rather than routing the broken struct through
        // arrow-rs's aborting `from_raw`.
        if let Some(release_fn) = release {
            // SAFETY: `release_fn` is the producer's release callback for this
            // stream. We null the caller's `release` field *first* — the Arrow C
            // Data Interface "released" sentinel — then invoke the callback once.
            // Nulling first is the move-semantics convention (the consumer claims
            // ownership before cleanup) and is robust even against a producer
            // that frees the struct inside its own callback: we never touch the
            // struct after `release_fn` returns. `release_fn` reads `private_data`
            // (untouched), so its cleanup still runs.
            unsafe {
                (*stream).release = None;
                release_fn(stream);
            }
        }
        return Err(lance_core::Error::InvalidInput {
            source: "stream is uninitialised, already released, or missing a \
                     required get_schema/get_next/release callback"
                .into(),
            location: location!(),
        });
    }

    // SAFETY: `stream` is non-NULL (checked above) and the caller guarantees it
    // points to an initialised, properly-aligned `FFI_ArrowArrayStream` they
    // own. `from_raw` moves the entire caller struct into Rust (via `ptr::replace`
    // with an empty, released stream), so the caller's memory cannot be released
    // twice — on success or on the error path. The pre-flight guard rules out the
    // `release == NULL` error, but `from_raw` still fails (a live `map_err` path)
    // if the stream's `get_schema` callback returns an error code or yields a
    // schema arrow-rs cannot convert; that surfaces as `LANCE_ERR_INVALID_ARGUMENT`.
    let reader = unsafe { ArrowArrayStreamReader::from_raw(stream) }.map_err(|e| {
        lance_core::Error::InvalidInput {
            source: e.to_string().into(),
            location: location!(),
        }
    })?;

    if dataset.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "dataset must not be NULL".into(),
            location: location!(),
        });
    }

    let batch_size = resolve_batch_size(batch_size)?;

    let transform = NewColumnTransform::Reader(Box::new(reader));

    // SAFETY: `dataset` is non-NULL (checked above); see `add_columns_sql_inner`
    // for the `with_mut` locking justification.
    let ds = unsafe { &*dataset };
    ds.with_mut(|d| block_on(d.add_columns(transform, None, batch_size)))?;
    Ok(0)
}

/// Parse a required, non-empty C string field of a `LanceSqlColumn`, attaching
/// the column index and field name to any error so the caller can pinpoint the
/// offending entry.
unsafe fn parse_required_field(ptr: *const c_char, index: usize, field: &str) -> Result<String> {
    // SAFETY: `ptr` is either NULL (rejected below) or a NUL-terminated C
    // string the caller keeps alive for this call.
    let value = unsafe { helpers::parse_c_string(ptr)? }
        .filter(|s| !s.is_empty())
        .ok_or_else(|| lance_core::Error::InvalidInput {
            source: format!("columns[{index}].{field} must not be NULL or empty").into(),
            location: location!(),
        })?;
    Ok(value.to_string())
}

/// Translate the `0 = upstream default` batch-size sentinel into `Option<u32>`,
/// rejecting values that do not fit `u32` rather than silently wrapping with an
/// `as` cast.
fn resolve_batch_size(batch_size: u64) -> Result<Option<u32>> {
    if batch_size == 0 {
        return Ok(None);
    }
    let narrowed = u32::try_from(batch_size).map_err(|_| lance_core::Error::InvalidInput {
        source: format!("batch_size={batch_size} exceeds u32::MAX").into(),
        location: location!(),
    })?;
    Ok(Some(narrowed))
}
