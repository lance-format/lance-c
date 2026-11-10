// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Dataset table and schema metadata C API.
//!
//! Metadata reads and update results cross the C boundary as JSON objects.
//! Updates use the native `UpdateMetadataBuilder`, preserving Lance's merge,
//! replace, removal and commit semantics.

use std::collections::{BTreeMap, HashMap};
use std::ffi::{CString, c_char};
use std::future::IntoFuture;

use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::block_on;

/// Selects how an update is applied to the existing metadata map.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceMetadataUpdateMode {
    /// Merge entries into the existing map.
    Merge = 0,
    /// Replace the existing map with the entries that carry values.
    Replace = 1,
}

impl LanceMetadataUpdateMode {
    fn from_raw(raw: i32) -> Result<Self> {
        match raw {
            0 => Ok(Self::Merge),
            1 => Ok(Self::Replace),
            other => Err(lance_core::Error::invalid_input(format!(
                "invalid metadata update mode {other}; expected \
                 LANCE_METADATA_UPDATE_MERGE (0) or LANCE_METADATA_UPDATE_REPLACE (1)"
            ))),
        }
    }
}

/// Return this snapshot's table metadata as a JSON object.
///
/// Schema metadata is exposed through `lance_dataset_schema` and is not
/// duplicated here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_metadata_json(
    dataset: *const LanceDataset,
) -> *const c_char {
    ffi_try!(unsafe { metadata_json_inner(dataset) }, null)
}

unsafe fn metadata_json_inner(dataset: *const LanceDataset) -> Result<*const c_char> {
    let dataset = unsafe { dataset_ref(dataset)? };
    let snapshot = dataset.snapshot();
    serialize_metadata_map(snapshot.metadata())
}

/// Apply a merge or replacement to table metadata and return the complete
/// updated map as a JSON object.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_update_metadata_json(
    dataset: *mut LanceDataset,
    keys: *const *const c_char,
    values: *const *const c_char,
    count: usize,
    mode: i32,
) -> *const c_char {
    ffi_try!(
        unsafe { update_metadata_json_inner(dataset, keys, values, count, mode, false) },
        null
    )
}

/// Apply a merge or replacement to schema metadata and return the complete
/// updated map as a JSON object.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_update_schema_metadata_json(
    dataset: *mut LanceDataset,
    keys: *const *const c_char,
    values: *const *const c_char,
    count: usize,
    mode: i32,
) -> *const c_char {
    ffi_try!(
        unsafe { update_metadata_json_inner(dataset, keys, values, count, mode, true) },
        null
    )
}

unsafe fn update_metadata_json_inner(
    dataset: *mut LanceDataset,
    keys: *const *const c_char,
    values: *const *const c_char,
    count: usize,
    mode: i32,
    schema_metadata: bool,
) -> Result<*const c_char> {
    let dataset = unsafe { dataset_mut_ref(dataset)? };
    let mode = LanceMetadataUpdateMode::from_raw(mode)?;
    let entries = unsafe { parse_metadata_entries(keys, values, count)? };

    let updated = dataset.with_mut(|dataset| {
        let builder = if schema_metadata {
            dataset.update_schema_metadata(entries)
        } else {
            dataset.update_metadata(entries)
        };
        let builder = match mode {
            LanceMetadataUpdateMode::Merge => builder,
            LanceMetadataUpdateMode::Replace => builder.replace(),
        };
        block_on(builder.into_future())
    })?;

    serialize_metadata_map(&updated)
}

unsafe fn dataset_ref(dataset: *const LanceDataset) -> Result<&LanceDataset> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input("dataset must not be NULL"));
    }
    Ok(unsafe { &*dataset })
}

unsafe fn dataset_mut_ref(dataset: *mut LanceDataset) -> Result<&LanceDataset> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input("dataset must not be NULL"));
    }
    Ok(unsafe { &*dataset })
}

unsafe fn parse_metadata_entries(
    keys: *const *const c_char,
    values: *const *const c_char,
    count: usize,
) -> Result<Vec<(String, Option<String>)>> {
    if count > 0 && keys.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "keys must not be NULL when count = {count}"
        )));
    }
    if count > 0 && values.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "values must not be NULL when count = {count}; \
             use a NULL value entry to remove a key"
        )));
    }

    let mut entries = Vec::with_capacity(count);
    for index in 0..count {
        let key_ptr = unsafe { *keys.add(index) };
        let key = unsafe { helpers::parse_c_string(key_ptr)? }.ok_or_else(|| {
            lance_core::Error::invalid_input(format!(
                "keys[{index}] must not be NULL; use count = 0 for an empty update"
            ))
        })?;

        let value_ptr = unsafe { *values.add(index) };
        let value = unsafe { helpers::parse_c_string(value_ptr)? }.map(str::to_owned);
        entries.push((key.to_owned(), value));
    }
    Ok(entries)
}

fn serialize_metadata_map(metadata: &HashMap<String, String>) -> Result<*const c_char> {
    let sorted = metadata
        .iter()
        .map(|(key, value)| (key.as_str(), value.as_str()))
        .collect::<BTreeMap<_, _>>();
    let json = serde_json::to_string(&sorted).map_err(|error| {
        lance_core::Error::internal(format!("failed to serialize dataset metadata: {error}"))
    })?;
    CString::new(json)
        .map(|value| value.into_raw() as *const c_char)
        .map_err(|_| {
            lance_core::Error::internal("dataset metadata JSON contains an embedded NUL byte")
        })
}
