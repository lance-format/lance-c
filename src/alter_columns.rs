// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Alter columns C API: rename a column, change its nullability, or change its
//! data type, committing a new manifest. Rename and nullability-only changes
//! are zero-copy and preserve indices; a type change rewrites the column's
//! data files and drops any associated indices, mirroring upstream behaviour.
//!
//! Mutates the dataset in place under an exclusive write lock; existing
//! scanners that already cloned the inner Arc keep their pre-alteration view.

use std::ffi::c_char;

use arrow::ffi::FFI_ArrowSchema;
use arrow_schema::DataType;
use lance::dataset::ColumnAlteration;
use lance_core::Result;
use snafu::location;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::block_on;

/// Tri-state nullability override for `LanceColumnAlteration`. The `Unchanged`
/// discriminant is zero so a zero-initialised struct leaves nullability alone.
///
/// Discriminants are pinned for ABI stability. Out-of-range values stored on
/// the FFI side are rejected with `LANCE_ERR_INVALID_ARGUMENT` rather than
/// being transmuted into a `repr(C)` enum (which would be UB) — that's why
/// the corresponding field on `LanceColumnAlteration` is `i32`, not this
/// enum directly.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceColumnNullableMode {
    /// Do not touch the column's existing nullability.
    Unchanged = 0,
    /// Set the column to nullable.
    True = 1,
    /// Set the column to non-nullable. Upstream verifies via a scan that no
    /// existing rows hold a NULL — the call fails if any do.
    False = 2,
}

impl LanceColumnNullableMode {
    fn from_raw(raw: i32) -> Result<Self> {
        match raw {
            0 => Ok(Self::Unchanged),
            1 => Ok(Self::True),
            2 => Ok(Self::False),
            other => Err(lance_core::Error::InvalidInput {
                source: format!(
                    "invalid nullable_mode {other}; expected 0..=2 (see LanceColumnNullableMode)"
                )
                .into(),
                location: location!(),
            }),
        }
    }
}

/// A single alteration applied to one column. Each non-`path` field is
/// optional via a sentinel:
///
/// * `rename = NULL` keeps the current name.
/// * `nullable_mode = LANCE_COLUMN_NULLABLE_UNCHANGED` keeps the current
///   nullability.
/// * `data_type = NULL` keeps the current data type.
///
/// At least one of `rename`, `nullable_mode`, or `data_type` must request a
/// change; an alteration that touches nothing is rejected at the FFI
/// boundary.
///
/// `data_type` borrows an Arrow C Data Interface `ArrowSchema` describing the
/// target type for the duration of the call. The struct is read by shared
/// reference; we never call its `release` callback.
#[repr(C)]
pub struct LanceColumnAlteration {
    /// Path to the existing column to alter. Required, non-empty UTF-8.
    pub path: *const c_char,
    /// New column name, or NULL to keep the current name.
    pub rename: *const c_char,
    /// `LanceColumnNullableMode` discriminant; carried as `i32` so an
    /// invalid value coming in from C is rejected at the FFI boundary
    /// instead of being transmuted into an enum (which would be UB).
    pub nullable_mode: i32,
    /// New data type, or NULL to keep the current type.
    pub data_type: *const FFI_ArrowSchema,
}

/// Apply one or more `LanceColumnAlteration`s and commit a new manifest.
/// Rename and nullability-only changes are zero-copy and preserve any indices
/// on the affected columns. A type change rewrites the column's data files
/// and drops any indices that referenced it.
///
/// - `dataset`: Open dataset (mutated; same handle remains valid afterward).
///   Must not be NULL.
/// - `alterations`: Pointer to an array of `LanceColumnAlteration`. Must not
///   be NULL.
/// - `num_alterations`: Length of the `alterations` array. Must be non-zero.
///
/// Returns 0 on success, -1 on error. Error codes:
/// `LANCE_ERR_INVALID_ARGUMENT` for NULL/empty args, NULL or empty `path`,
/// non-UTF-8 strings, no-op alterations (all three optional fields left at
/// their sentinels), invalid `nullable_mode` discriminant, unknown columns,
/// type changes that aren't a valid cast, or tightening nullability when
/// existing rows hold NULLs. `LANCE_ERR_COMMIT_CONFLICT` for a concurrent
/// writer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_alter_columns(
    dataset: *mut LanceDataset,
    alterations: *const LanceColumnAlteration,
    num_alterations: usize,
) -> i32 {
    ffi_try!(
        unsafe { alter_columns_inner(dataset, alterations, num_alterations) },
        neg
    )
}

unsafe fn alter_columns_inner(
    dataset: *mut LanceDataset,
    alterations: *const LanceColumnAlteration,
    num_alterations: usize,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "dataset must not be NULL".into(),
            location: location!(),
        });
    }
    if alterations.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "alterations must not be NULL".into(),
            location: location!(),
        });
    }
    if num_alterations == 0 {
        return Err(lance_core::Error::InvalidInput {
            source: "num_alterations must be > 0".into(),
            location: location!(),
        });
    }

    // Materialize alterations up front so any per-index validation error
    // fires before the dataset's write lock is taken — matches the pre-lock
    // validation pattern used by `update.rs` and `drop_columns.rs`.
    let mut owned: Vec<ColumnAlteration> = Vec::with_capacity(num_alterations);
    for i in 0..num_alterations {
        // SAFETY: `alterations` is non-NULL (checked above) and the caller
        // guarantees the array has at least `num_alterations` initialised
        // entries valid for the duration of this call. `parse_alteration`
        // is `unsafe` solely because it dereferences each entry's `path` /
        // `rename` / `data_type` pointers — same caller-side guarantee.
        let entry = unsafe { &*alterations.add(i) };
        owned.push(unsafe { parse_alteration(i, entry)? });
    }

    // SAFETY: `dataset` is non-NULL (checked above) and the caller guarantees
    // it points to a live `LanceDataset`. `with_mut` takes an exclusive
    // write lock on the inner `Arc<Dataset>` before yielding `&mut Dataset`,
    // so a shared `&*dataset` borrow here is sound — interior mutability is
    // the synchronization point.
    let ds = unsafe { &*dataset };
    ds.with_mut(|d| block_on(d.alter_columns(&owned)))?;
    Ok(0)
}

/// Translate one C-side `LanceColumnAlteration` into the upstream owned form,
/// validating every field at the FFI boundary so the caller sees precise
/// per-index errors rather than upstream-internal ones.
unsafe fn parse_alteration(
    index: usize,
    entry: &LanceColumnAlteration,
) -> Result<ColumnAlteration> {
    // SAFETY: `entry.path` is either NULL (rejected below) or a NUL-terminated
    // C string the caller keeps alive for this call.
    let path = unsafe { helpers::parse_c_string(entry.path)? }
        .filter(|s| !s.is_empty())
        .ok_or_else(|| lance_core::Error::InvalidInput {
            source: format!("alterations[{index}].path must not be NULL or empty").into(),
            location: location!(),
        })?
        .to_string();

    // SAFETY: same shape as `path`; NULL is a documented sentinel for "keep
    // the current name", not an error.
    let rename = unsafe { helpers::parse_c_string(entry.rename)? }
        .map(|s| {
            if s.is_empty() {
                Err(lance_core::Error::InvalidInput {
                    source: format!("alterations[{index}].rename must not be empty").into(),
                    location: location!(),
                })
            } else {
                Ok(s.to_string())
            }
        })
        .transpose()?;

    let nullable = match LanceColumnNullableMode::from_raw(entry.nullable_mode)? {
        LanceColumnNullableMode::Unchanged => None,
        LanceColumnNullableMode::True => Some(true),
        LanceColumnNullableMode::False => Some(false),
    };

    let data_type = if entry.data_type.is_null() {
        None
    } else {
        // SAFETY: caller guarantees `data_type` (when non-NULL) points to a
        // valid `FFI_ArrowSchema` for the duration of this call. We read by
        // shared reference and never invoke the struct's release callback.
        let ffi_schema = unsafe { &*entry.data_type };
        // Reject an already-released or never-initialised schema before
        // handing it to arrow-rs, which would otherwise `assert!` on the
        // NULL `format` field and abort the host process under our
        // `panic = "abort"` profile. Both checks are intentional:
        //   - `release == NULL`: the canonical Arrow CADI "released" sentinel.
        //   - `format == NULL`: catches a zero-initialised or otherwise
        //     half-built struct that would slip past the release check.
        if ffi_schema.release.is_none() || ffi_schema.format.is_null() {
            return Err(lance_core::Error::InvalidInput {
                source: format!(
                    "alterations[{index}].data_type is uninitialised or already released"
                )
                .into(),
                location: location!(),
            });
        }
        let dt = DataType::try_from(ffi_schema).map_err(|e| lance_core::Error::InvalidInput {
            source: format!("alterations[{index}].data_type is not a valid Arrow type: {e}").into(),
            location: location!(),
        })?;
        Some(dt)
    };

    if rename.is_none() && nullable.is_none() && data_type.is_none() {
        return Err(lance_core::Error::InvalidInput {
            source: format!(
                "alterations[{index}] is a no-op: \
                 set rename, nullable_mode, or data_type to request a change"
            )
            .into(),
            location: location!(),
        });
    }

    Ok(ColumnAlteration {
        path,
        rename,
        nullable,
        data_type,
    })
}
