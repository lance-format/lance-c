// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Merge-insert C API: SQL-MERGE-style upsert from an Arrow record-batch
//! stream into an existing dataset, committing a new manifest.
//!
//! Mutates the dataset in place under an exclusive write lock; existing
//! scanners that already cloned the inner Arc keep their snapshot view.

use std::ffi::c_char;
use std::sync::Arc;

use arrow::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use lance::dataset::{MergeInsertBuilder, WhenMatched, WhenNotMatched, WhenNotMatchedBySource};
use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::block_on;

/// Behavior when a target row matches a source row on the join keys.
///
/// Discriminants are pinned for ABI stability. Out-of-range values stored on
/// the FFI side are rejected with `LANCE_ERR_INVALID_ARGUMENT` rather than
/// being transmuted into a `repr(C)` enum (which would be UB).
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceMergeWhenMatched {
    /// Keep the target row unchanged (find-or-create). This is the default.
    DoNothing = 0,
    /// Replace the target row with the source row (upsert).
    UpdateAll = 1,
    /// Replace the target row only when an SQL filter evaluates true.
    /// Requires `when_matched_expr` on `LanceMergeInsertParams`.
    UpdateIf = 2,
    /// Fail the operation on any match.
    Fail = 3,
    /// Drop the matching target row without inserting anything in its place.
    Delete = 4,
}

impl LanceMergeWhenMatched {
    fn from_raw(raw: i32) -> Result<Self> {
        match raw {
            0 => Ok(Self::DoNothing),
            1 => Ok(Self::UpdateAll),
            2 => Ok(Self::UpdateIf),
            3 => Ok(Self::Fail),
            4 => Ok(Self::Delete),
            other => Err(lance_core::Error::InvalidInput {
                source: format!(
                    "invalid when_matched {other}; expected 0..=4 (see LanceMergeWhenMatched)"
                )
                .into(),
                location: snafu::location!(),
            }),
        }
    }
}

/// Behavior when a source row has no matching target row.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceMergeWhenNotMatched {
    /// Insert the source row (the default).
    InsertAll = 0,
    /// Discard the source row.
    DoNothing = 1,
}

impl LanceMergeWhenNotMatched {
    fn from_raw(raw: i32) -> Result<Self> {
        match raw {
            0 => Ok(Self::InsertAll),
            1 => Ok(Self::DoNothing),
            other => Err(lance_core::Error::InvalidInput {
                source: format!(
                    "invalid when_not_matched {other}; expected 0 or 1 (see LanceMergeWhenNotMatched)"
                )
                .into(),
                location: snafu::location!(),
            }),
        }
    }
}

/// Behavior when a target row has no matching source row.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceMergeWhenNotMatchedBySource {
    /// Keep the target row (the default).
    Keep = 0,
    /// Delete every unmatched target row.
    Delete = 1,
    /// Delete unmatched target rows that satisfy an SQL filter. Requires
    /// `when_not_matched_by_source_expr` on `LanceMergeInsertParams`.
    DeleteIf = 2,
}

impl LanceMergeWhenNotMatchedBySource {
    fn from_raw(raw: i32) -> Result<Self> {
        match raw {
            0 => Ok(Self::Keep),
            1 => Ok(Self::Delete),
            2 => Ok(Self::DeleteIf),
            other => Err(lance_core::Error::InvalidInput {
                source: format!(
                    "invalid when_not_matched_by_source {other}; expected 0..=2 (see LanceMergeWhenNotMatchedBySource)"
                )
                .into(),
                location: snafu::location!(),
            }),
        }
    }
}

/// Tunable parameters for `lance_dataset_merge_insert`. Pass NULL to use the
/// upstream find-or-create defaults (`DoNothing` / `InsertAll` / `Keep`).
///
/// The struct is `#[repr(C)]` and ABI-stable within a minor version.
/// Expression strings are read only when the corresponding mode requires
/// them; spurious non-NULL pointers on other modes are rejected to keep the
/// contract unambiguous.
#[repr(C)]
pub struct LanceMergeInsertParams {
    /// `LanceMergeWhenMatched` discriminant. Default: `DoNothing` (0).
    pub when_matched: i32,
    /// SQL filter for `UpdateIf`. Required iff `when_matched == UpdateIf`,
    /// forbidden otherwise. Must not be empty when set.
    pub when_matched_expr: *const c_char,
    /// `LanceMergeWhenNotMatched` discriminant. Default: `InsertAll` (0).
    pub when_not_matched: i32,
    /// `LanceMergeWhenNotMatchedBySource` discriminant. Default: `Keep` (0).
    pub when_not_matched_by_source: i32,
    /// SQL filter for `DeleteIf`. Required iff
    /// `when_not_matched_by_source == DeleteIf`, forbidden otherwise. Must
    /// not be empty when set.
    pub when_not_matched_by_source_expr: *const c_char,
}

/// Per-call merge statistics returned via the optional out parameter.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LanceMergeInsertResult {
    /// Rows that did not match any target row and were inserted.
    pub num_inserted_rows: u64,
    /// Target rows that matched a source row and were updated in place.
    pub num_updated_rows: u64,
    /// Target rows deleted as a result of the merge (e.g. `WhenMatched::Delete`
    /// or `WhenNotMatchedBySource::Delete[If]`).
    pub num_deleted_rows: u64,
}

/// Resolved merge-insert parameters with caller strings copied into owned
/// `String`s so they outlive the FFI argument lifetime.
struct ResolvedParams {
    when_matched: WhenMatched,
    when_not_matched: WhenNotMatched,
    when_not_matched_by_source: ResolvedWhenNotMatchedBySource,
}

enum ResolvedWhenNotMatchedBySource {
    Keep,
    Delete,
    DeleteIf(String),
}

impl ResolvedParams {
    fn defaults() -> Self {
        Self {
            when_matched: WhenMatched::DoNothing,
            when_not_matched: WhenNotMatched::InsertAll,
            when_not_matched_by_source: ResolvedWhenNotMatchedBySource::Keep,
        }
    }
}

/// Merge `source` into `dataset` keyed on `on_columns`, committing a new
/// manifest.
///
/// - `dataset`: Open dataset (mutated; same handle remains valid afterward).
///   Must not be NULL.
/// - `on_columns` / `num_on_columns`: Join keys. Must be non-NULL with
///   `num_on_columns >= 1`; each entry must be a non-NULL, non-empty C
///   string. Column names are matched case-insensitively (upstream
///   behavior).
/// - `source`: Arrow C Data Interface stream of source rows. Consumed by
///   this call — the caller must not use it again on any return path. Its
///   schema must be compatible with the dataset schema (full match or a
///   subschema; upstream rejects mismatches with `INVALID_ARGUMENT`).
/// - `params`: Optional. NULL uses the find-or-create defaults
///   (`DoNothing` / `InsertAll` / `Keep`).
/// - `out_result`: Optional. If non-NULL, on success receives the
///   `LanceMergeInsertResult` for this call. On error the slot is untouched.
///
/// Returns 0 on success, -1 on error. Error codes:
/// `LANCE_ERR_INVALID_ARGUMENT` for NULL/empty args, out-of-range mode
/// discriminants, missing/extraneous expression strings, malformed SQL,
/// unknown columns, schema incompatibility, or no-op configurations;
/// `LANCE_ERR_COMMIT_CONFLICT` for a concurrent writer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_merge_insert(
    dataset: *mut LanceDataset,
    on_columns: *const *const c_char,
    num_on_columns: usize,
    source: *mut FFI_ArrowArrayStream,
    params: *const LanceMergeInsertParams,
    out_result: *mut LanceMergeInsertResult,
) -> i32 {
    ffi_try!(
        unsafe {
            merge_insert_inner(
                dataset,
                on_columns,
                num_on_columns,
                source,
                params,
                out_result,
            )
        },
        neg
    )
}

unsafe fn merge_insert_inner(
    dataset: *mut LanceDataset,
    on_columns: *const *const c_char,
    num_on_columns: usize,
    source: *mut FFI_ArrowArrayStream,
    params: *const LanceMergeInsertParams,
    out_result: *mut LanceMergeInsertResult,
) -> Result<i32> {
    // The stream NULL check is the only validation that runs *before* the
    // stream is consumed; once `from_raw` succeeds, every other return path
    // drops `reader`, which fires the FFI release callback. Reordering the
    // dataset/on-columns checks ahead of `from_raw` would leak the stream on
    // those paths and break the documented "consumed on every return"
    // contract (mirrors `lance_dataset_write`).
    if source.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "source stream must not be NULL".into(),
            location: snafu::location!(),
        });
    }

    // SAFETY: `source` is non-NULL (checked above) and the caller guarantees
    // it points to an initialized, properly-aligned `FFI_ArrowArrayStream`
    // owned by them. `from_raw` performs a `ptr::replace` that transfers
    // ownership into the returned reader, zeroing the caller's release
    // callback so it cannot be released twice.
    let reader = unsafe { ArrowArrayStreamReader::from_raw(source) }.map_err(|e| {
        lance_core::Error::InvalidInput {
            source: e.to_string().into(),
            location: snafu::location!(),
        }
    })?;

    if dataset.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "dataset must not be NULL".into(),
            location: snafu::location!(),
        });
    }
    if num_on_columns == 0 {
        return Err(lance_core::Error::InvalidInput {
            source: "num_on_columns must be >= 1".into(),
            location: snafu::location!(),
        });
    }
    if on_columns.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "on_columns must not be NULL when num_on_columns > 0".into(),
            location: snafu::location!(),
        });
    }

    // Materialize key columns up front so a precise per-index error fires
    // before the write lock is taken.
    let mut keys: Vec<String> = Vec::with_capacity(num_on_columns);
    for i in 0..num_on_columns {
        // SAFETY: `on_columns` is non-NULL (checked above) and the caller
        // guarantees the array has at least `num_on_columns` entries.
        let entry_ptr = unsafe { *on_columns.add(i) };
        // SAFETY: each entry pointer is either NULL (rejected below) or a
        // NUL-terminated C string the caller keeps alive for this call.
        let key = unsafe { helpers::parse_c_string(entry_ptr)? }
            .filter(|s| !s.is_empty())
            .ok_or_else(|| lance_core::Error::InvalidInput {
                source: format!("on_columns[{i}] must not be NULL or empty").into(),
                location: snafu::location!(),
            })?;
        keys.push(key.to_string());
    }

    // SAFETY: `params` is either NULL (use defaults) or points to a valid
    // `LanceMergeInsertParams` for the duration of this call.
    let resolved = unsafe { resolve_params(params)? };

    // SAFETY: `dataset` is non-NULL (checked above) and the caller guarantees
    // it points to a live `LanceDataset` not aliased mutably elsewhere.
    let ds = unsafe { &*dataset };
    let stats = ds.with_mut(|d| {
        block_on(async {
            // MergeInsertBuilder takes `Arc<Dataset>` (snapshot-based), so
            // mirror what update.rs does: clone for the builder, then publish
            // the new dataset back into `*d` after the commit lands.
            let snapshot = Arc::new(d.clone());

            let when_not_matched_by_source = match resolved.when_not_matched_by_source {
                ResolvedWhenNotMatchedBySource::Keep => WhenNotMatchedBySource::Keep,
                ResolvedWhenNotMatchedBySource::Delete => WhenNotMatchedBySource::Delete,
                ResolvedWhenNotMatchedBySource::DeleteIf(expr) => {
                    // `delete_if` parses the SQL against the dataset's schema
                    // and surfaces parse / unknown-column errors as
                    // InvalidInput → INVALID_ARGUMENT at the FFI boundary.
                    WhenNotMatchedBySource::delete_if(&snapshot, &expr)?
                }
            };

            let mut builder = MergeInsertBuilder::try_new(snapshot, keys)?;
            builder
                .when_matched(resolved.when_matched)
                .when_not_matched(resolved.when_not_matched)
                .when_not_matched_by_source(when_not_matched_by_source);
            let job = builder.try_build()?;
            let (new_dataset, stats) = job.execute_reader(reader).await?;
            *d = Arc::try_unwrap(new_dataset.clone()).unwrap_or_else(|arc| (*arc).clone());
            Ok::<_, lance_core::Error>(stats)
        })
    })?;

    if !out_result.is_null() {
        // SAFETY: caller guarantees `out_result` (when non-NULL) points to
        // caller-owned, writable storage of size `sizeof(LanceMergeInsertResult)`.
        // We only write on success; on the error paths above the slot stays
        // untouched per the documented contract.
        unsafe {
            *out_result = LanceMergeInsertResult {
                num_inserted_rows: stats.num_inserted_rows,
                num_updated_rows: stats.num_updated_rows,
                num_deleted_rows: stats.num_deleted_rows,
            };
        }
    }
    Ok(0)
}

/// Translate caller-supplied `LanceMergeInsertParams` (or NULL) into the
/// upstream behavior enums, reading every C string by shared reference.
unsafe fn resolve_params(params: *const LanceMergeInsertParams) -> Result<ResolvedParams> {
    if params.is_null() {
        return Ok(ResolvedParams::defaults());
    }

    // SAFETY: `params` is non-NULL (checked above) and the caller guarantees
    // it points to a properly-initialized `LanceMergeInsertParams` valid for
    // the duration of this call. We read by shared reference.
    let params = unsafe { &*params };

    let when_matched_kind = LanceMergeWhenMatched::from_raw(params.when_matched)?;
    let when_not_matched = LanceMergeWhenNotMatched::from_raw(params.when_not_matched)?;
    let when_not_matched_by_source_kind =
        LanceMergeWhenNotMatchedBySource::from_raw(params.when_not_matched_by_source)?;

    // SAFETY: pointer is either NULL (no string) or a NUL-terminated C string
    // valid for this call; `parse_c_string` reads by shared reference.
    let when_matched_expr =
        unsafe { read_optional_expr(params.when_matched_expr, "when_matched_expr")? };
    let when_not_matched_by_source_expr = unsafe {
        read_optional_expr(
            params.when_not_matched_by_source_expr,
            "when_not_matched_by_source_expr",
        )?
    };

    let when_matched = match when_matched_kind {
        LanceMergeWhenMatched::DoNothing => {
            reject_unused_expr("when_matched", "DoNothing", &when_matched_expr)?;
            WhenMatched::DoNothing
        }
        LanceMergeWhenMatched::UpdateAll => {
            reject_unused_expr("when_matched", "UpdateAll", &when_matched_expr)?;
            WhenMatched::UpdateAll
        }
        LanceMergeWhenMatched::UpdateIf => {
            let expr = when_matched_expr.ok_or_else(|| lance_core::Error::InvalidInput {
                source: "when_matched=UpdateIf requires when_matched_expr".into(),
                location: snafu::location!(),
            })?;
            // Upstream `WhenMatched::update_if` defers parsing until execute
            // time; we only forward the string here.
            WhenMatched::UpdateIf(expr)
        }
        LanceMergeWhenMatched::Fail => {
            reject_unused_expr("when_matched", "Fail", &when_matched_expr)?;
            WhenMatched::Fail
        }
        LanceMergeWhenMatched::Delete => {
            reject_unused_expr("when_matched", "Delete", &when_matched_expr)?;
            WhenMatched::Delete
        }
    };

    let when_not_matched = match when_not_matched {
        LanceMergeWhenNotMatched::InsertAll => WhenNotMatched::InsertAll,
        LanceMergeWhenNotMatched::DoNothing => WhenNotMatched::DoNothing,
    };

    let when_not_matched_by_source = match when_not_matched_by_source_kind {
        LanceMergeWhenNotMatchedBySource::Keep => {
            reject_unused_expr(
                "when_not_matched_by_source",
                "Keep",
                &when_not_matched_by_source_expr,
            )?;
            ResolvedWhenNotMatchedBySource::Keep
        }
        LanceMergeWhenNotMatchedBySource::Delete => {
            reject_unused_expr(
                "when_not_matched_by_source",
                "Delete",
                &when_not_matched_by_source_expr,
            )?;
            ResolvedWhenNotMatchedBySource::Delete
        }
        LanceMergeWhenNotMatchedBySource::DeleteIf => {
            let expr = when_not_matched_by_source_expr.ok_or_else(|| {
                lance_core::Error::InvalidInput {
                    source:
                        "when_not_matched_by_source=DeleteIf requires when_not_matched_by_source_expr"
                            .into(),
                    location: snafu::location!(),
                }
            })?;
            ResolvedWhenNotMatchedBySource::DeleteIf(expr)
        }
    };

    Ok(ResolvedParams {
        when_matched,
        when_not_matched,
        when_not_matched_by_source,
    })
}

unsafe fn read_optional_expr(ptr: *const c_char, field: &str) -> Result<Option<String>> {
    // SAFETY: the caller's contract on `LanceMergeInsertParams` requires
    // every non-NULL expression pointer to be a NUL-terminated C string
    // valid for the call.
    let parsed = unsafe { helpers::parse_c_string(ptr)? };
    let Some(s) = parsed else {
        return Ok(None);
    };
    if s.is_empty() {
        return Err(lance_core::Error::InvalidInput {
            source: format!("{field} must not be empty").into(),
            location: snafu::location!(),
        });
    }
    Ok(Some(s.to_string()))
}

fn reject_unused_expr(field: &str, mode: &str, expr: &Option<String>) -> Result<()> {
    if expr.is_some() {
        return Err(lance_core::Error::InvalidInput {
            source: format!("{field}_expr must be NULL when {field}={mode}").into(),
            location: snafu::location!(),
        });
    }
    Ok(())
}
