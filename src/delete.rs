// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Delete C API: drop rows matching an SQL predicate, committing a new manifest.
//!
//! Mutates the dataset in place under an exclusive write lock; existing
//! scanners that already cloned the inner Arc keep their snapshot view.

use std::ffi::c_char;

use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::block_on;

/// Delete rows matching the SQL `predicate`, committing a new manifest.
///
/// - `dataset`: Open dataset (mutated; same handle remains valid afterward).
///   Must not be NULL.
/// - `predicate`: SQL filter expression, e.g. `"id > 100"` or `"name = 'a'"`.
///   Must not be NULL or empty.
/// - `out_num_deleted`: Optional. If non-NULL, receives the number of rows
///   that were deleted (0 if the predicate matched nothing). On error the
///   slot is untouched.
///
/// Returns 0 on success, -1 on error. Error codes:
/// `LANCE_ERR_INVALID_ARGUMENT` for NULL/empty args (validated at this
/// boundary), `LANCE_ERR_INTERNAL` for malformed SQL or unknown columns
/// (surfaced from the upstream parser), and `LANCE_ERR_COMMIT_CONFLICT`
/// for a concurrent writer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_delete(
    dataset: *mut LanceDataset,
    predicate: *const c_char,
    out_num_deleted: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { delete_inner(dataset, predicate, out_num_deleted) },
        neg
    )
}

unsafe fn delete_inner(
    dataset: *mut LanceDataset,
    predicate: *const c_char,
    out_num_deleted: *mut u64,
) -> Result<i32> {
    if dataset.is_null() || predicate.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "dataset and predicate must not be NULL".into(),
            location: snafu::location!(),
        });
    }

    // SAFETY: `predicate` is non-NULL (checked above) and the caller
    // guarantees it points to a NUL-terminated C string valid for the
    // duration of this call. `parse_c_string` reads by shared reference.
    let predicate_str = unsafe { helpers::parse_c_string(predicate)? }
        .filter(|s| !s.is_empty())
        .ok_or_else(|| lance_core::Error::InvalidInput {
            // NULL is rejected above; only the empty case reaches here.
            source: "predicate must not be empty".into(),
            location: snafu::location!(),
        })?;

    // SAFETY: `dataset` is non-NULL (checked above) and the caller guarantees
    // it points to a live `LanceDataset` not aliased mutably elsewhere.
    let ds = unsafe { &*dataset };
    let result = ds.with_mut(|d| block_on(d.delete(predicate_str)))?;

    if !out_num_deleted.is_null() {
        // SAFETY: caller guarantees `out_num_deleted` (when non-NULL) points
        // to caller-owned, writable storage of size `sizeof(uint64_t)`. We
        // only write on success; on the error paths above the slot stays
        // untouched per the documented contract.
        unsafe { *out_num_deleted = result.num_deleted_rows };
    }
    Ok(0)
}
