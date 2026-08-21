// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Versions C API: list all versions of a Lance dataset.
//!
//! `lance_dataset_versions` returns an opaque `LanceVersions` snapshot;
//! accessors read entries by index, and `lance_versions_close` frees it.

use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
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
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let versions = block_on(ds.snapshot().versions())?;
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
    ffi_try!(unsafe { count_inner(versions) }, 0)
}

unsafe fn count_inner(versions: *const LanceVersions) -> Result<u64> {
    if versions.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "versions is NULL".into(),
        ));
    }
    let v = unsafe { &*versions };
    Ok(v.entries.len() as u64)
}

/// Return the monotonic version id at `index` (0 <= index < count).
/// Returns 0 and sets the thread-local error on NULL or out-of-range input.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_versions_id_at(versions: *const LanceVersions, index: usize) -> u64 {
    ffi_try!(unsafe { id_at_inner(versions, index) }, 0)
}

unsafe fn id_at_inner(versions: *const LanceVersions, index: usize) -> Result<u64> {
    Ok(unsafe { entry_at(versions, index) }?.id)
}

/// Return the Unix epoch millisecond timestamp at `index`.
/// Returns 0 and sets the thread-local error on NULL or out-of-range input.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_versions_timestamp_ms_at(
    versions: *const LanceVersions,
    index: usize,
) -> i64 {
    ffi_try!(unsafe { timestamp_ms_at_inner(versions, index) }, 0)
}

unsafe fn timestamp_ms_at_inner(versions: *const LanceVersions, index: usize) -> Result<i64> {
    Ok(unsafe { entry_at(versions, index) }?.timestamp_ms)
}

/// Close and free a versions handle. Safe to call with NULL.
///
/// Best-effort (issue #61): a panic raised while dropping the handle is
/// caught and logged rather than unwinding into the caller, and the
/// remainder of the value may leak.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_versions_close(versions: *mut LanceVersions) {
    if !versions.is_null() {
        swallow_unwind("lance_versions_close", || unsafe {
            let _ = Box::from_raw(versions);
        });
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Copy the entry at `index` out of the versions handle. Returns an
/// `InvalidArgument` error on NULL handle or out-of-range index.
unsafe fn entry_at(versions: *const LanceVersions, index: usize) -> Result<VersionEntry> {
    if versions.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "versions is NULL".into(),
        ));
    }
    let v = unsafe { &*versions };
    v.entries.get(index).copied().ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!(
                "version index {} out of range; count = {}",
                index,
                v.entries.len()
            )
            .into(),
        )
    })
}
