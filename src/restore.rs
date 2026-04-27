// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Restore C API: move a dataset's latest back to an older version by
//! committing a new manifest whose fragments match the chosen version.
//!
//! Returns a fresh `LanceDataset*` positioned at the target version; the
//! caller's original handle is untouched and remains usable.

use std::sync::Arc;

use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::runtime::block_on;

/// Restore the dataset to an older version by committing a new manifest that
/// carries the fragments of `version`.
///
/// - `dataset`: Open dataset (not consumed). Must not be NULL.
/// - `version`: Target version id. Must be `>= 1`; `0` is reserved as the
///   "latest" sentinel by `lance_dataset_open` and is rejected here with
///   `LANCE_ERR_INVALID_ARGUMENT`.
///
/// A new manifest is always written, even when `version` already matches the
/// current latest — this guarantees the caller's stated intent ("make `version`
/// the new latest") holds under concurrent writers without a TOCTOU race.
///
/// Returns a fresh `LanceDataset*` positioned at the target version on success
/// (caller closes with `lance_dataset_close`), or NULL on error. Errors include
/// `LANCE_ERR_NOT_FOUND` for an unknown `version` and `LANCE_ERR_COMMIT_CONFLICT`
/// for a concurrent commit race.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_restore(
    dataset: *const LanceDataset,
    version: u64,
) -> *mut LanceDataset {
    ffi_try!(unsafe { restore_inner(dataset, version) }, null)
}

unsafe fn restore_inner(dataset: *const LanceDataset, version: u64) -> Result<*mut LanceDataset> {
    if dataset.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "dataset must not be NULL".into(),
            location: snafu::location!(),
        });
    }
    if version == 0 {
        return Err(lance_core::Error::InvalidInput {
            source: "version must be >= 1; 0 is reserved as the \"latest\" sentinel".into(),
            location: snafu::location!(),
        });
    }

    let ds = unsafe { &*dataset };

    // Check out the target version, then always commit a new manifest that
    // aliases its fragments as the new latest. Skipping the commit when
    // `version == latest` would open a TOCTOU window: a concurrent writer
    // could land a newer manifest between the read and the comparison, and
    // we'd silently leave their version as latest instead of the caller's.
    let restored = block_on(async {
        let mut checked_out = ds.inner.checkout_version(version).await?;
        checked_out.restore().await?;
        Ok::<_, lance_core::Error>(checked_out)
    })?;

    let handle = LanceDataset {
        inner: Arc::new(restored),
    };
    Ok(Box::into_raw(Box::new(handle)))
}
