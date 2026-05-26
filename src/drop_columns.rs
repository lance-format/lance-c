// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Drop columns C API: remove columns from the dataset's schema, committing a
//! new manifest. Metadata-only — data files are not rewritten until a later
//! `lance_dataset_compact_files` call materializes the projection.
//!
//! Mutates the dataset in place under an exclusive write lock; existing
//! scanners that already cloned the inner Arc keep their pre-drop schema view.

use std::ffi::c_char;

use lance_core::Result;
use snafu::location;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::block_on;

/// Drop one or more columns from the dataset's schema and commit a new
/// manifest. This is a metadata-only operation: data files remain on storage
/// until they are rewritten by `lance_dataset_compact_files` (and then
/// cleaned up by version cleanup).
///
/// - `dataset`: Open dataset (mutated; same handle remains valid afterward).
///   Must not be NULL.
/// - `columns`: Pointer to an array of NUL-terminated C strings naming the
///   columns to drop. Must not be NULL. Entries must be non-NULL and
///   non-empty UTF-8.
/// - `num_columns`: Length of the `columns` array. Must be non-zero.
///
/// Returns 0 on success, -1 on error. Error codes:
/// `LANCE_ERR_INVALID_ARGUMENT` for NULL/empty args, NULL or empty entries,
/// non-UTF-8 names, unknown columns, or an attempt to drop every column
/// (upstream rejects that since a Lance dataset must retain at least one
/// field). `LANCE_ERR_COMMIT_CONFLICT` for a concurrent writer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_drop_columns(
    dataset: *mut LanceDataset,
    columns: *const *const c_char,
    num_columns: usize,
) -> i32 {
    ffi_try!(
        unsafe { drop_columns_inner(dataset, columns, num_columns) },
        neg
    )
}

unsafe fn drop_columns_inner(
    dataset: *mut LanceDataset,
    columns: *const *const c_char,
    num_columns: usize,
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

    // Materialize the column names up front so any per-index validation
    // error fires before the dataset's write lock is taken — matches the
    // pre-lock validation pattern used by `update.rs`.
    let mut names: Vec<String> = Vec::with_capacity(num_columns);
    for i in 0..num_columns {
        // SAFETY: `columns` is non-NULL (checked above) and the caller
        // guarantees the array has at least `num_columns` entries.
        let entry = unsafe { *columns.add(i) };
        // SAFETY: each entry is either NULL (rejected below) or a
        // NUL-terminated C string the caller keeps alive for this call.
        let name = unsafe { helpers::parse_c_string(entry)? }
            .filter(|s| !s.is_empty())
            .ok_or_else(|| lance_core::Error::InvalidInput {
                source: format!("columns[{i}] must not be NULL or empty").into(),
                location: location!(),
            })?;
        names.push(name.to_string());
    }

    // SAFETY: `dataset` is non-NULL (checked above) and the caller guarantees
    // it points to a live `LanceDataset`. `with_mut` takes an exclusive
    // write lock on the inner `Arc<Dataset>` before yielding `&mut Dataset`,
    // so a shared `&*dataset` borrow here is sound — interior mutability
    // is the synchronization point.
    let ds = unsafe { &*dataset };
    let names_refs: Vec<&str> = names.iter().map(String::as_str).collect();
    ds.with_mut(|d| block_on(d.drop_columns(&names_refs)))?;
    Ok(0)
}
