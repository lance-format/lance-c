// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Dataset metadata, field metadata and schema inspection C API.
//!
//! Metadata reads and update results cross the C boundary as JSON objects.
//! Updates use the native `UpdateMetadataBuilder`, preserving Lance's merge,
//! replace, removal and commit semantics. Field updates use the native
//! `UpdateFieldMetadataBuilder`, and schema inspection delegates to the current
//! snapshot's `Schema` methods.

use std::collections::{BTreeMap, HashMap};
use std::ffi::{CString, c_char};
use std::future::IntoFuture;
use std::ptr;

use lance_core::datatypes::FieldRef;
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

/// Selects a field by numeric ID or path.
const LANCE_FIELD_REFERENCE_ID: i32 = 0;
const LANCE_FIELD_REFERENCE_PATH: i32 = 1;

/// A field reference for a field metadata update or field lookup.
///
/// `kind` is one of `LANCE_FIELD_REFERENCE_ID` or
/// `LANCE_FIELD_REFERENCE_PATH`. The unused selector fields are ignored.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceFieldReference {
    pub kind: i32,
    pub field_id: i32,
    pub path: *const c_char,
}

/// One field's metadata update in a batch.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceFieldMetadataUpdate {
    pub field: LanceFieldReference,
    pub keys: *const *const c_char,
    pub values: *const *const c_char,
    pub count: usize,
    pub mode: i32,
}

enum OwnedFieldReference {
    ById(i32),
    ByPath(String),
}

impl OwnedFieldReference {
    fn as_field_ref(&self) -> FieldRef<'_> {
        match self {
            Self::ById(field_id) => FieldRef::ById(*field_id),
            Self::ByPath(path) => FieldRef::ByPath(path.as_str()),
        }
    }
}

struct OwnedFieldUpdate {
    field: OwnedFieldReference,
    entries: Vec<(String, Option<String>)>,
    mode: LanceMetadataUpdateMode,
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

    let updated = dataset.with_mut(|dataset: &mut lance::Dataset| {
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

/// Apply a batch of per-field metadata updates and commit one native operation.
///
/// Each entry selects a field by ID or path and applies its own merge or
/// replace mode. Duplicate references to the same resolved field follow the
/// native builder's last-write-wins behavior. The input arrays and strings are
/// copied before the asynchronous native operation starts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_update_field_metadata(
    dataset: *mut LanceDataset,
    updates: *const LanceFieldMetadataUpdate,
    update_count: usize,
) -> i32 {
    ffi_try!(
        unsafe { update_field_metadata_inner(dataset, updates, update_count) },
        neg
    )
}

unsafe fn update_field_metadata_inner(
    dataset: *mut LanceDataset,
    updates: *const LanceFieldMetadataUpdate,
    update_count: usize,
) -> Result<i32> {
    let dataset = unsafe { dataset_mut_ref(dataset)? };
    let updates = unsafe { parse_field_updates(updates, update_count)? };

    dataset.with_mut(|dataset: &mut lance::Dataset| -> Result<()> {
        let mut builder = dataset.update_field_metadata();
        for update in updates {
            let OwnedFieldUpdate {
                field,
                entries,
                mode,
            } = update;
            let field_ref = field.as_field_ref();
            builder = match mode {
                LanceMetadataUpdateMode::Merge => builder.update(field_ref, entries)?,
                LanceMetadataUpdateMode::Replace => builder.replace(field_ref, entries)?,
            };
        }
        block_on(builder.into_future())?;
        Ok(())
    })?;

    Ok(0)
}

/// Return the current snapshot's canonical Lance data-storage version.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_data_storage_version(
    dataset: *const LanceDataset,
) -> *const c_char {
    ffi_try!(unsafe { data_storage_version_inner(dataset) }, null)
}

unsafe fn data_storage_version_inner(dataset: *const LanceDataset) -> Result<*const c_char> {
    let snapshot = unsafe { dataset_ref(dataset)? }.snapshot();
    owned_c_string(
        snapshot
            .manifest()
            .data_storage_format
            .version
            .to_manifest_string()
            .to_owned(),
        "data storage version",
    )
}

/// Report whether the current snapshot uses stable row IDs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_has_stable_row_ids(
    dataset: *const LanceDataset,
    out_has_stable_row_ids: *mut bool,
) -> i32 {
    ffi_try!(
        unsafe { has_stable_row_ids_inner(dataset, out_has_stable_row_ids) },
        neg
    )
}

unsafe fn has_stable_row_ids_inner(
    dataset: *const LanceDataset,
    out_has_stable_row_ids: *mut bool,
) -> Result<i32> {
    if out_has_stable_row_ids.is_null() {
        return Err(lance_core::Error::invalid_input(
            "out_has_stable_row_ids must not be NULL",
        ));
    }
    let snapshot = unsafe { dataset_ref(dataset)? }.snapshot();
    unsafe {
        ptr::write_unaligned(
            out_has_stable_row_ids,
            snapshot.manifest().uses_stable_row_ids(),
        )
    };
    Ok(0)
}

/// Return the number of field IDs in the current snapshot's schema.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_field_id_count(
    dataset: *const LanceDataset,
    out_count: *mut usize,
) -> i32 {
    ffi_try!(unsafe { field_id_count_inner(dataset, out_count) }, neg)
}

unsafe fn field_id_count_inner(
    dataset: *const LanceDataset,
    out_count: *mut usize,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input(
            "out_count must not be NULL",
        ));
    }
    let snapshot = unsafe { dataset_ref(dataset)? }.snapshot();
    let count = snapshot.schema().field_ids().len();
    unsafe { ptr::write_unaligned(out_count, count) };
    Ok(0)
}

/// Return the field ID at `index` in native pre-order traversal.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_field_id_at(
    dataset: *const LanceDataset,
    index: usize,
    out_field_id: *mut i32,
) -> i32 {
    ffi_try!(
        unsafe { field_id_at_inner(dataset, index, out_field_id) },
        neg
    )
}

unsafe fn field_id_at_inner(
    dataset: *const LanceDataset,
    index: usize,
    out_field_id: *mut i32,
) -> Result<i32> {
    if out_field_id.is_null() {
        return Err(lance_core::Error::invalid_input(
            "out_field_id must not be NULL",
        ));
    }
    let snapshot = unsafe { dataset_ref(dataset)? }.snapshot();
    let field_ids = snapshot.schema().field_ids();
    let field_id = field_ids.get(index).copied().ok_or_else(|| {
        lance_core::Error::invalid_input(format!(
            "field ID index {index} out of range; count = {}",
            field_ids.len()
        ))
    })?;
    unsafe { ptr::write_unaligned(out_field_id, field_id) };
    Ok(0)
}

/// Resolve a field reference against the current snapshot's schema.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_resolve_field_id(
    dataset: *const LanceDataset,
    field: *const LanceFieldReference,
    out_field_id: *mut i32,
) -> i32 {
    ffi_try!(
        unsafe { resolve_field_id_inner(dataset, field, out_field_id) },
        neg
    )
}

unsafe fn resolve_field_id_inner(
    dataset: *const LanceDataset,
    field: *const LanceFieldReference,
    out_field_id: *mut i32,
) -> Result<i32> {
    if field.is_null() {
        return Err(lance_core::Error::invalid_input(
            "field must not be NULL",
        ));
    }
    if out_field_id.is_null() {
        return Err(lance_core::Error::invalid_input(
            "out_field_id must not be NULL",
        ));
    }
    let snapshot = unsafe { dataset_ref(dataset)? }.snapshot();
    let reference = unsafe { parse_field_reference(&*field, "field")? };
    let field_id = reference.as_field_ref().into_id(snapshot.schema())?;
    unsafe { ptr::write_unaligned(out_field_id, field_id) };
    Ok(0)
}

/// Return the native minimal path for `field_id` in the current schema.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_field_path(
    dataset: *const LanceDataset,
    field_id: i32,
) -> *const c_char {
    ffi_try!(unsafe { field_path_inner(dataset, field_id) }, null)
}

unsafe fn field_path_inner(dataset: *const LanceDataset, field_id: i32) -> Result<*const c_char> {
    let snapshot = unsafe { dataset_ref(dataset)? }.snapshot();
    owned_c_string(
        snapshot.schema().field_path_minimal(field_id)?,
        "field path",
    )
}

unsafe fn dataset_ref<'a>(dataset: *const LanceDataset) -> Result<&'a LanceDataset> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input("dataset must not be NULL"));
    }
    Ok(unsafe { &*dataset })
}

unsafe fn dataset_mut_ref<'a>(dataset: *mut LanceDataset) -> Result<&'a LanceDataset> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input("dataset must not be NULL"));
    }
    Ok(unsafe { &*dataset })
}

unsafe fn parse_field_updates(
    updates: *const LanceFieldMetadataUpdate,
    update_count: usize,
) -> Result<Vec<OwnedFieldUpdate>> {
    if update_count > 0 && updates.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "updates must not be NULL when update_count = {update_count}"
        )));
    }

    let mut parsed = Vec::with_capacity(update_count);
    for index in 0..update_count {
        let update = unsafe { &*updates.add(index) };
        let field = unsafe {
            parse_field_reference(&update.field, &format!("updates[{index}].field"))?
        };
        let entries = unsafe { parse_metadata_entries(update.keys, update.values, update.count) }
            .map_err(|error| {
                lance_core::Error::invalid_input(format!("updates[{index}]: {error}"))
            })?;
        let mode = LanceMetadataUpdateMode::from_raw(update.mode).map_err(|error| {
            lance_core::Error::invalid_input(format!("updates[{index}]: {error}"))
        })?;
        parsed.push(OwnedFieldUpdate {
            field,
            entries,
            mode,
        });
    }
    Ok(parsed)
}

unsafe fn parse_field_reference(
    field: &LanceFieldReference,
    label: &str,
) -> Result<OwnedFieldReference> {
    match field.kind {
        LANCE_FIELD_REFERENCE_ID => Ok(OwnedFieldReference::ById(field.field_id)),
        LANCE_FIELD_REFERENCE_PATH => {
            let path = unsafe { helpers::parse_c_string(field.path) }
                .map_err(|error| {
                    lance_core::Error::invalid_input(format!("{label}.path: {error}"))
                })?
                .ok_or_else(|| {
                    lance_core::Error::invalid_input(format!(
                        "{label}.path must not be NULL when kind = \
                         LANCE_FIELD_REFERENCE_PATH"
                    ))
                })?;
            Ok(OwnedFieldReference::ByPath(path.to_owned()))
        }
        other => Err(lance_core::Error::invalid_input(format!(
            "{label}.kind is {other}; expected LANCE_FIELD_REFERENCE_ID (0) or \
             LANCE_FIELD_REFERENCE_PATH (1)"
        ))),
    }
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

fn owned_c_string(value: String, context: &str) -> Result<*const c_char> {
    CString::new(value)
        .map(|value| value.into_raw() as *const c_char)
        .map_err(|_| {
            lance_core::Error::internal(format!("{context} contains an embedded NUL byte"))
        })
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
