// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Dataset config C API: read the manifest config map and apply native
//! merge/replace operations.

use std::collections::HashMap;
use std::ffi::{CString, c_char};
use std::future::IntoFuture;

use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
use crate::metadata::{LanceMetadataUpdateMode, parse_metadata_entries, serialize_metadata_map};
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
    let entries = unsafe { parse_metadata_entries(keys, values, num_entries) }?;
    apply_config_update(dataset, entries, LanceMetadataUpdateMode::Merge)?;
    Ok(0)
}

/// Merge or replace dataset config and return the complete updated map as a
/// JSON object.
///
/// Input and ownership rules match `lance_dataset_update_metadata_json`.
/// `LANCE_METADATA_UPDATE_REPLACE` with `count == 0` clears the config map.
/// The native builder commits one transaction and returns its complete map.
///
/// Caller must free the returned JSON string with `lance_free_string()`.
/// Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_update_config_json(
    dataset: *mut LanceDataset,
    keys: *const *const c_char,
    values: *const *const c_char,
    count: usize,
    mode: i32,
) -> *const c_char {
    ffi_try!(
        unsafe { update_config_json_inner(dataset, keys, values, count, mode) },
        null
    )
}

unsafe fn update_config_json_inner(
    dataset: *mut LanceDataset,
    keys: *const *const c_char,
    values: *const *const c_char,
    count: usize,
    mode: i32,
) -> Result<*const c_char> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    let mode = LanceMetadataUpdateMode::from_raw(mode)?;
    let entries = unsafe { parse_metadata_entries(keys, values, count) }?;
    let updated = apply_config_update(dataset, entries, mode)?;
    serialize_metadata_map(&updated)
}

fn apply_config_update(
    dataset: *mut LanceDataset,
    entries: Vec<(String, Option<String>)>,
    mode: LanceMetadataUpdateMode,
) -> Result<HashMap<String, String>> {
    let dataset = unsafe { &*dataset };
    dataset.with_mut(|dataset: &mut lance::Dataset| {
        let builder = dataset.update_config(entries);
        let builder = match mode {
            LanceMetadataUpdateMode::Merge => builder,
            LanceMetadataUpdateMode::Replace => builder.replace(),
        };
        block_on(builder.into_future())
    })
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
