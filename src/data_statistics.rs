// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Data statistics C API: per-field storage statistics for query planning.
//!
//! `lance_dataset_calculate_data_stats` walks every fragment to total each
//! field's compressed on-disk byte size, returning the result as an opaque
//! `LanceDataStatistics` snapshot. Accessors read entries by index, and
//! `lance_data_statistics_close` frees it.

use lance::dataset::statistics::DatasetStatisticsExt;
use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::{LanceErrorCode, clear_last_error, ffi_try, set_last_error};
use crate::runtime::block_on;

/// Opaque snapshot of a dataset's per-field data statistics.
pub struct LanceDataStatistics {
    fields: Vec<FieldStat>,
}

#[derive(Clone, Copy)]
struct FieldStat {
    id: u32,
    bytes_on_disk: u64,
}

/// Compute per-field data statistics for the dataset. The caller frees the
/// returned handle with `lance_data_statistics_close`. Returns NULL on error.
///
/// Entries are ordered by the dataset's schema field id, one per field
/// (including nested struct/list children). `bytes_on_disk` is the field's
/// compressed on-disk size; it is 0 for datasets written with the legacy (v1)
/// storage format, which does not track per-field sizes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_calculate_data_stats(
    dataset: *const LanceDataset,
) -> *mut LanceDataStatistics {
    ffi_try!(unsafe { calculate_inner(dataset) }, null)
}

unsafe fn calculate_inner(dataset: *const LanceDataset) -> Result<*mut LanceDataStatistics> {
    if dataset.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "dataset must not be NULL".into(),
            location: snafu::location!(),
        });
    }
    // SAFETY: `dataset` is non-null (checked above) and points at a live
    // `LanceDataset` created by `lance_dataset_open`; we take only a shared
    // borrow, which is sound for the duration of this call.
    let ds = unsafe { &*dataset };
    let snapshot = ds.snapshot();
    let stats = block_on(snapshot.calculate_data_stats())?;
    let fields = stats
        .fields
        .into_iter()
        .map(|f| FieldStat {
            id: f.id,
            bytes_on_disk: f.bytes_on_disk,
        })
        .collect();
    Ok(Box::into_raw(Box::new(LanceDataStatistics { fields })))
}

/// Return the number of fields in the statistics snapshot.
///
/// Clears the thread-local error on success. Returns 0 and sets
/// `InvalidArgument` on a NULL handle. A dataset with an empty schema also
/// yields 0 with no error set, so check `lance_last_error_code()` to
/// distinguish the error case from an empty result.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_statistics_count(stats: *const LanceDataStatistics) -> u64 {
    if stats.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "stats is NULL");
        return 0;
    }
    // SAFETY: `stats` is non-null (checked above) and was produced by
    // `lance_dataset_calculate_data_stats` via `Box::into_raw`; the accessors
    // only ever take shared borrows, so no mutable alias exists.
    let s = unsafe { &*stats };
    let count = s.fields.len() as u64;
    clear_last_error();
    count
}

/// Return the schema field id at `index` (0 <= index < count).
///
/// Returns 0 and sets the thread-local error on NULL or out-of-range input.
/// Because 0 is itself a valid field id, check `lance_last_error_code()` when
/// passing an untrusted index; iterating `0..count` never triggers the error
/// path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_statistics_field_id_at(
    stats: *const LanceDataStatistics,
    index: usize,
) -> u32 {
    unsafe { entry_at(stats, index) }.map(|f| f.id).unwrap_or(0)
}

/// Return the compressed on-disk byte size of the field at `index`.
///
/// Returns 0 and sets the thread-local error on NULL or out-of-range input.
/// A genuine 0 (legacy storage, or an empty field) is indistinguishable from
/// the error sentinel by value alone — check `lance_last_error_code()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_statistics_bytes_on_disk_at(
    stats: *const LanceDataStatistics,
    index: usize,
) -> u64 {
    unsafe { entry_at(stats, index) }
        .map(|f| f.bytes_on_disk)
        .unwrap_or(0)
}

/// Close and free a data statistics handle. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_statistics_close(stats: *mut LanceDataStatistics) {
    if !stats.is_null() {
        unsafe {
            let _ = Box::from_raw(stats);
        }
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Copy the field stat at `index` out of the handle. Sets the thread-local
/// error and returns `None` on NULL handle or out-of-range index.
unsafe fn entry_at(stats: *const LanceDataStatistics, index: usize) -> Option<FieldStat> {
    if stats.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "stats is NULL");
        return None;
    }
    // SAFETY: `stats` is non-null (checked above) and was produced by
    // `lance_dataset_calculate_data_stats` via `Box::into_raw`; we take only a
    // shared borrow.
    let s = unsafe { &*stats };
    match s.fields.get(index).copied() {
        Some(f) => {
            clear_last_error();
            Some(f)
        }
        None => {
            set_last_error(
                LanceErrorCode::InvalidArgument,
                format!(
                    "field statistics index {} out of range; count = {}",
                    index,
                    s.fields.len()
                ),
            );
            None
        }
    }
}
