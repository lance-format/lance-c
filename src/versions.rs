// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Versions C API: list all versions of a Lance dataset.
//!
//! `lance_dataset_versions` returns an opaque `LanceVersions` snapshot;
//! accessors read entries by index, and `lance_versions_close` frees it.

use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::{LanceErrorCode, clear_last_error, ffi_try, set_last_error};
use crate::runtime::block_on;

/// Opaque snapshot of a dataset's version history.
pub struct LanceVersions {
    entries: Vec<VersionEntry>,
}

#[derive(Clone, Copy)]
struct VersionEntry {
    id: u64,
    timestamp_ms: i64,
}

/// Return a snapshot of the dataset's version list. The caller frees the
/// returned handle with `lance_versions_close`. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_versions(
    dataset: *const LanceDataset,
) -> *mut LanceVersions {
    ffi_try!(unsafe { versions_inner(dataset) }, null)
}

unsafe fn versions_inner(dataset: *const LanceDataset) -> Result<*mut LanceVersions> {
    if dataset.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "dataset must not be NULL".into(),
            location: snafu::location!(),
        });
    }
    let ds = unsafe { &*dataset };
    let versions = block_on(ds.inner.versions())?;
    let entries = versions
        .into_iter()
        .map(|v| VersionEntry {
            id: v.version,
            timestamp_ms: v.timestamp.timestamp_millis(),
        })
        .collect();
    Ok(Box::into_raw(Box::new(LanceVersions { entries })))
}

/// Return the number of versions. Returns 0 on error (NULL handle).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_versions_count(versions: *const LanceVersions) -> u64 {
    if versions.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "versions is NULL");
        return 0;
    }
    let v = unsafe { &*versions };
    clear_last_error();
    v.entries.len() as u64
}

/// Return the monotonic version id at `index` (0 <= index < count).
/// Returns 0 and sets the thread-local error on NULL or out-of-range input.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_versions_id_at(versions: *const LanceVersions, index: usize) -> u64 {
    unsafe { entry_at(versions, index) }
        .map(|e| e.id)
        .unwrap_or(0)
}

/// Return the Unix epoch millisecond timestamp at `index`.
/// Returns 0 and sets the thread-local error on NULL or out-of-range input.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_versions_timestamp_ms_at(
    versions: *const LanceVersions,
    index: usize,
) -> i64 {
    unsafe { entry_at(versions, index) }
        .map(|e| e.timestamp_ms)
        .unwrap_or(0)
}

/// Close and free a versions handle. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_versions_close(versions: *mut LanceVersions) {
    if !versions.is_null() {
        unsafe {
            let _ = Box::from_raw(versions);
        }
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Copy the entry at `index` out of the versions handle. Sets the thread-local
/// error and returns `None` on NULL handle or out-of-range index.
unsafe fn entry_at(versions: *const LanceVersions, index: usize) -> Option<VersionEntry> {
    if versions.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "versions is NULL");
        return None;
    }
    let v = unsafe { &*versions };
    match v.entries.get(index).copied() {
        Some(e) => {
            clear_last_error();
            Some(e)
        }
        None => {
            set_last_error(
                LanceErrorCode::InvalidArgument,
                format!(
                    "version index {} out of range; count = {}",
                    index,
                    v.entries.len()
                ),
            );
            None
        }
    }
}
