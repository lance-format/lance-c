// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Dataset config C API: read the manifest config map and apply the native
//! merge-style upsert/removal operation.

use std::ffi::{CString, c_char};

use lance::dataset::transaction::UpdateMapEntry;
use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
use crate::helpers;
use crate::runtime::block_on;

/// Opaque snapshot of a dataset's manifest config map.
pub struct LanceDatasetConfig {
    entries: Vec<ConfigEntry>,
}

struct ConfigEntry {
    key: CString,
    value: CString,
}

/// Return a snapshot of the dataset's manifest config map.
///
/// The snapshot owns copies of the keys and values, so it remains valid after
/// the dataset handle is closed. Free it with `lance_dataset_config_close`.
/// Entry order is unspecified. Keys and values are exposed as NUL-terminated C
/// strings; an entry containing an embedded NUL byte fails the snapshot with
/// `LANCE_ERR_INTERNAL`. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_config(
    dataset: *const LanceDataset,
) -> *mut LanceDatasetConfig {
    ffi_try!(unsafe { config_inner(dataset) }, null)
}

unsafe fn config_inner(dataset: *const LanceDataset) -> Result<*mut LanceDatasetConfig> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let config = ds.snapshot().config().clone();
    let mut entries = Vec::with_capacity(config.len());
    for (key, value) in config {
        let key = CString::new(key.clone()).map_err(|_| {
            lance_core::Error::internal(format!(
                "config key {key:?} contains a NUL byte and cannot be exposed as a C string"
            ))
        })?;
        let value = CString::new(value).map_err(|_| {
            lance_core::Error::internal(format!(
                "config value for key {key:?} contains a NUL byte and cannot be exposed as a C string"
            ))
        })?;
        entries.push(ConfigEntry { key, value });
    }
    Ok(Box::into_raw(Box::new(LanceDatasetConfig { entries })))
}

/// Return the number of config entries in the snapshot.
///
/// Clears the thread-local error on success. Returns 0 and sets
/// `LANCE_ERR_INVALID_ARGUMENT` on a NULL handle. An empty config also yields
/// 0 with no error set, so check `lance_last_error_code()` to distinguish the
/// two.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_config_count(config: *const LanceDatasetConfig) -> u64 {
    ffi_try!(unsafe { config_count_inner(config) }, 0)
}

unsafe fn config_count_inner(config: *const LanceDatasetConfig) -> Result<u64> {
    if config.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "config must not be NULL".into(),
        ));
    }
    let config = unsafe { &*config };
    Ok(config.entries.len() as u64)
}

/// Return the config key at `index` in the snapshot.
///
/// The pointer is borrowed from the snapshot and remains valid until
/// `lance_dataset_config_close`. Returns NULL and sets `LANCE_ERR_INVALID_ARGUMENT`
/// on a NULL handle or out-of-range index.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_config_key_at(
    config: *const LanceDatasetConfig,
    index: usize,
) -> *const c_char {
    ffi_try!(unsafe { config_key_at_inner(config, index) }, null)
}

unsafe fn config_key_at_inner(
    config: *const LanceDatasetConfig,
    index: usize,
) -> Result<*const c_char> {
    Ok(unsafe { config_entry_at(config, index) }?.0)
}

/// Return the config value at `index` in the snapshot.
///
/// The pointer is borrowed from the snapshot and remains valid until
/// `lance_dataset_config_close`. Returns NULL and sets `LANCE_ERR_INVALID_ARGUMENT`
/// on a NULL handle or out-of-range index.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_config_value_at(
    config: *const LanceDatasetConfig,
    index: usize,
) -> *const c_char {
    ffi_try!(unsafe { config_value_at_inner(config, index) }, null)
}

unsafe fn config_value_at_inner(
    config: *const LanceDatasetConfig,
    index: usize,
) -> Result<*const c_char> {
    Ok(unsafe { config_entry_at(config, index) }?.1)
}

/// Close and free a dataset config snapshot. Safe to call with NULL.
///
/// Best-effort: a panic raised while dropping the snapshot is caught and
/// logged rather than unwinding into the caller.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_config_close(config: *mut LanceDatasetConfig) {
    if !config.is_null() {
        swallow_unwind("lance_dataset_config_close", || unsafe {
            let _ = Box::from_raw(config);
        });
    }
}

/// Apply merge-style dataset config updates and commit a new manifest.
///
/// `keys` and `values` are parallel arrays of length `num_entries`. A non-NULL
/// `values[i]` upserts `keys[i]`; a NULL `values[i]` removes `keys[i]`. This
/// maps directly to `Dataset::update_config` with merge semantics. A
/// `num_entries == 0` call is passed through as an empty native update.
///
/// When `num_entries > 0`, both arrays must be non-NULL and every `keys[i]`
/// must be a non-NULL C string. Empty keys and empty string values are passed
/// through unchanged, matching the native map semantics.
///
/// Returns 0 on success, -1 on error. The dataset handle continues to point at
/// the newly committed version on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_update_config(
    dataset: *mut LanceDataset,
    keys: *const *const c_char,
    values: *const *const c_char,
    num_entries: usize,
) -> i32 {
    ffi_try!(
        unsafe { update_config_inner(dataset, keys, values, num_entries) },
        neg
    )
}

unsafe fn update_config_inner(
    dataset: *mut LanceDataset,
    keys: *const *const c_char,
    values: *const *const c_char,
    num_entries: usize,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    if num_entries > 0 && (keys.is_null() || values.is_null()) {
        return Err(lance_core::Error::invalid_input_source(
            "keys and values must not be NULL when num_entries > 0".into(),
        ));
    }

    let mut entries = Vec::with_capacity(num_entries);
    for i in 0..num_entries {
        // SAFETY: the caller guarantees both arrays have at least
        // `num_entries` entries when `num_entries > 0`.
        let key_ptr = unsafe { *keys.add(i) };
        let key = unsafe { helpers::parse_c_string(key_ptr)? }.ok_or_else(|| {
            lance_core::Error::invalid_input_source(format!("keys[{i}] must not be NULL").into())
        })?;
        // SAFETY: see the guarantee on `keys` above; each value entry may be
        // NULL to request removal.
        let value_ptr = unsafe { *values.add(i) };
        let value = unsafe { helpers::parse_c_string(value_ptr)? };
        entries.push(UpdateMapEntry {
            key: key.to_string(),
            value: value.map(str::to_owned),
        });
    }

    let ds = unsafe { &*dataset };
    ds.with_mut(|d| {
        block_on(async move {
            d.update_config(entries).await?;
            Ok::<(), lance_core::Error>(())
        })
    })?;
    Ok(0)
}

/// Borrow the entry at `index` from the snapshot.
unsafe fn config_entry_at(
    config: *const LanceDatasetConfig,
    index: usize,
) -> Result<(*const c_char, *const c_char)> {
    if config.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "config must not be NULL".into(),
        ));
    }
    // SAFETY: `config` is non-NULL and points at a snapshot created by
    // `lance_dataset_config`; accessors take only shared borrows, so no
    // mutable alias exists. The returned reference is tied to the borrowed
    // snapshot by the caller's FFI contract. The returned pointers borrow from
    // the snapshot and stay valid until `lance_dataset_config_close`.
    let config = unsafe { &*config };
    config
        .entries
        .get(index)
        .map(|entry| (entry.key.as_ptr(), entry.value.as_ptr()))
        .ok_or_else(|| {
            lance_core::Error::invalid_input_source(
                format!(
                    "config index {} out of range; count = {}",
                    index,
                    config.entries.len()
                )
                .into(),
            )
        })
}
