// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Update C API: rewrite columns of rows matching an SQL predicate by
//! evaluating per-column SQL expressions, committing a new manifest.
//!
//! Mutates the dataset in place under an exclusive write lock; existing
//! scanners that already cloned the inner Arc keep their snapshot view.

use std::ffi::c_char;
use std::sync::Arc;

use lance::dataset::UpdateBuilder;
use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::block_on;

/// Update rows matching the SQL `predicate` by applying per-column SQL
/// expressions, committing a new manifest.
///
/// - `dataset`: Open dataset (mutated; same handle remains valid afterward).
///   Must not be NULL.
/// - `predicate`: SQL filter expression, or NULL to update every row. When
///   non-NULL it must not be empty.
/// - `columns` / `values`: Parallel arrays of length `num_updates`. Each
///   `values[i]` is an SQL scalar expression evaluated per row (literals,
///   column references, arithmetic, `CASE`, ...). Both arrays must be
///   non-NULL when `num_updates > 0`, and every entry must itself be a
///   non-NULL, non-empty C string.
/// - `num_updates`: Length of `columns` and `values`. Must be `>= 1`.
/// - `out_num_updated`: Optional. If non-NULL, receives the number of rows
///   that were updated (0 if `predicate` matched nothing). On error the
///   slot is untouched.
///
/// Returns 0 on success, -1 on error. Error codes:
/// `LANCE_ERR_INVALID_ARGUMENT` for NULL/empty args, `num_updates == 0`,
/// malformed SQL, and unknown columns; `LANCE_ERR_COMMIT_CONFLICT` for a
/// concurrent writer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_update(
    dataset: *mut LanceDataset,
    predicate: *const c_char,
    columns: *const *const c_char,
    values: *const *const c_char,
    num_updates: usize,
    out_num_updated: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe {
            update_inner(
                dataset,
                predicate,
                columns,
                values,
                num_updates,
                out_num_updated,
            )
        },
        neg
    )
}

unsafe fn update_inner(
    dataset: *mut LanceDataset,
    predicate: *const c_char,
    columns: *const *const c_char,
    values: *const *const c_char,
    num_updates: usize,
    out_num_updated: *mut u64,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "dataset must not be NULL".into(),
            location: snafu::location!(),
        });
    }
    if num_updates == 0 {
        return Err(lance_core::Error::InvalidInput {
            source: "num_updates must be >= 1".into(),
            location: snafu::location!(),
        });
    }
    if columns.is_null() || values.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "columns and values must not be NULL when num_updates > 0".into(),
            location: snafu::location!(),
        });
    }

    // Optional predicate. NULL means "update every row"; explicit empty
    // string is rejected so callers who mean "all rows" go through NULL
    // rather than a parse error.
    // SAFETY: when non-NULL the caller guarantees `predicate` points to a
    // NUL-terminated C string valid for this call. `parse_c_string` reads
    // by shared reference.
    let predicate_str = unsafe { helpers::parse_c_string(predicate)? };
    if let Some(p) = predicate_str.as_ref()
        && p.is_empty()
    {
        return Err(lance_core::Error::InvalidInput {
            source: "predicate must not be empty (pass NULL to update all rows)".into(),
            location: snafu::location!(),
        });
    }

    // Collect (column, value) pairs into owned strings up front so we can
    // surface a precise per-index error on bad input before touching the
    // write lock.
    let mut update_pairs: Vec<(String, String)> = Vec::with_capacity(num_updates);
    for i in 0..num_updates {
        // SAFETY: `columns` and `values` are non-NULL (checked above) and the
        // caller guarantees both arrays have at least `num_updates` entries.
        let col_ptr = unsafe { *columns.add(i) };
        let val_ptr = unsafe { *values.add(i) };
        // SAFETY: see the guarantee on `predicate` above; the same applies
        // to each entry pointer within the arrays.
        let col = unsafe { helpers::parse_c_string(col_ptr)? }
            .filter(|s| !s.is_empty())
            .ok_or_else(|| lance_core::Error::InvalidInput {
                source: format!("columns[{i}] must not be NULL or empty").into(),
                location: snafu::location!(),
            })?;
        let val = unsafe { helpers::parse_c_string(val_ptr)? }
            .filter(|s| !s.is_empty())
            .ok_or_else(|| lance_core::Error::InvalidInput {
                source: format!("values[{i}] must not be NULL or empty").into(),
                location: snafu::location!(),
            })?;
        update_pairs.push((col.to_string(), val.to_string()));
    }

    // SAFETY: `dataset` is non-NULL (checked above) and the caller guarantees
    // it points to a live `LanceDataset` not aliased mutably elsewhere.
    let ds = unsafe { &*dataset };
    let rows_updated = ds.with_mut(|d| {
        block_on(async {
            // UpdateBuilder takes `Arc<Dataset>` (snapshot-based), so mirror
            // what `Dataset::delete` does internally: clone for the builder,
            // then publish `result.new_dataset` back into `*d`.
            let snapshot = Arc::new(d.clone());
            let mut builder = UpdateBuilder::new(snapshot);
            if let Some(p) = predicate_str {
                builder = builder.update_where(p)?;
            }
            for (col, val) in &update_pairs {
                builder = builder.set(col, val)?;
            }
            let result = builder.build()?.execute().await?;
            *d = Arc::try_unwrap(result.new_dataset.clone()).unwrap_or_else(|arc| (*arc).clone());
            Ok::<u64, lance_core::Error>(result.rows_updated)
        })
    })?;

    if !out_num_updated.is_null() {
        // SAFETY: caller guarantees `out_num_updated` (when non-NULL) points
        // to caller-owned, writable storage of size `sizeof(uint64_t)`. We
        // only write on success; on the error paths above the slot stays
        // untouched per the documented contract.
        unsafe { *out_num_updated = rows_updated };
    }
    Ok(0)
}
