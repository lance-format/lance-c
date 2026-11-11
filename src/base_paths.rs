// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Dataset base-path inspection and registration C API.
//!
//! The snapshot is serialized as JSON because native `BasePath` has no stable
//! foreign layout. Registration accepts only the four native fields plus
//! optional transaction properties; ID allocation, conflict validation,
//! persistence and path resolution remain in Lance core.

use std::collections::BTreeMap;
use std::ffi::c_char;
use std::sync::Arc;

use lance_core::Result;
use lance_table::format::BasePath;
use serde_json::json;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::metadata::owned_c_string;
use crate::runtime::block_on;
use crate::transaction_properties::{LanceTransactionProperties, parse_transaction_properties};

/// One native `BasePath` field set for registration.
///
/// `name == NULL` maps to native `None`; a non-NULL pointer, including an
/// empty string, maps to `Some(name)`. `id == 0` requests native ID
/// assignment. IDs, names and paths are validated by Lance core.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceBasePath {
    pub id: u32,
    pub path: *const c_char,
    pub name: *const c_char,
    pub is_dataset_root: bool,
}

/// Return the current snapshot's native base-path registry as a JSON object
/// keyed by decimal base ID.
///
/// Each value contains exactly the native fields `id`, `name`, `path` and
/// `is_dataset_root`. `name` is JSON `null` for native `None`. The snapshot
/// owns copied strings and does not change when the dataset later advances.
///
/// Caller must free the returned string with `lance_free_string()`.
/// Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_base_paths_json(
    dataset: *const LanceDataset,
) -> *const c_char {
    ffi_try!(unsafe { base_paths_json_inner(dataset) }, null)
}

unsafe fn base_paths_json_inner(dataset: *const LanceDataset) -> Result<*const c_char> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input("dataset must not be NULL"));
    }
    let snapshot = unsafe { &*dataset }.snapshot();
    let mut bases = BTreeMap::new();
    for (id, base) in &snapshot.manifest().base_paths {
        bases.insert(
            *id,
            json!({
                "id": base.id,
                "name": base.name,
                "path": base.path,
                "is_dataset_root": base.is_dataset_root,
            }),
        );
    }
    let json = serde_json::to_string(&bases).map_err(|error| {
        lance_core::Error::internal(format!("failed to serialize dataset base paths: {error}"))
    })?;
    owned_c_string(json, "dataset base paths JSON")
}

/// Register new base paths and commit one native `Dataset::add_bases`
/// transaction.
///
/// `bases` may be NULL when `base_count == 0`. An empty list is passed to
/// the native transaction builder.
/// Each `bases[i]` is copied before the asynchronous native operation starts.
/// A zero ID requests native allocation, and conflicts with existing IDs,
/// names or paths are reported by Lance core.
///
/// `transaction_properties == NULL` passes native `None`. A non-NULL pointer
/// with `count == 0` passes native `Some(empty map)`, preserving the caller's
/// explicit-empty choice at the API boundary. The dataset handle sees the new
/// version on success; existing dataset snapshots and streams keep their prior
/// view.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_add_bases(
    dataset: *mut LanceDataset,
    bases: *const LanceBasePath,
    base_count: usize,
    transaction_properties: *const LanceTransactionProperties,
) -> i32 {
    ffi_try!(
        unsafe { add_bases_inner(dataset, bases, base_count, transaction_properties) },
        neg
    )
}

unsafe fn add_bases_inner(
    dataset: *mut LanceDataset,
    bases: *const LanceBasePath,
    base_count: usize,
    transaction_properties: *const LanceTransactionProperties,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input("dataset must not be NULL"));
    }
    if base_count > 0 && bases.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "bases must not be NULL when base_count = {base_count}"
        )));
    }

    let mut parsed_bases = Vec::with_capacity(base_count);
    for index in 0..base_count {
        let base = unsafe { &*bases.add(index) };
        let path = unsafe { helpers::parse_c_string(base.path) }
            .map_err(|error| {
                lance_core::Error::invalid_input(format!("bases[{index}].path: {error}"))
            })?
            .ok_or_else(|| {
                lance_core::Error::invalid_input(format!("bases[{index}].path must not be NULL"))
            })?;
        let name = unsafe { helpers::parse_c_string(base.name) }
            .map_err(|error| {
                lance_core::Error::invalid_input(format!("bases[{index}].name: {error}"))
            })?
            .map(str::to_owned);
        parsed_bases.push(BasePath::new(
            base.id,
            path.to_owned(),
            name,
            base.is_dataset_root,
        ));
    }

    let properties = if transaction_properties.is_null() {
        None
    } else {
        Some(unsafe { parse_transaction_properties(&*transaction_properties)? })
    };

    let dataset = unsafe { &*dataset };
    dataset.with_mut(|dataset: &mut lance::Dataset| -> Result<()> {
        let current = Arc::new(dataset.clone());
        let updated = block_on(current.add_bases(parsed_bases, properties))?;
        *dataset = updated;
        Ok(())
    })?;
    Ok(0)
}
