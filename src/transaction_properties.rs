// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Shared transaction-property input for commit-capable C entry points.

use std::collections::HashMap;
use std::ffi::c_char;

use lance_core::Result;

use crate::helpers;

/// Optional transaction properties for one native commit.
///
/// A NULL pointer passed to an entry point maps to native `None`. A non-NULL
/// pointer with `count == 0` maps to native `Some(empty map)`. `keys` and
/// `values` are parallel arrays of length `count`; every key and value must be
/// non-NULL. Duplicate keys follow native `HashMap` last-write-wins behavior.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceTransactionProperties {
    pub keys: *const *const c_char,
    pub values: *const *const c_char,
    pub count: usize,
}

pub(crate) unsafe fn parse_transaction_properties(
    properties: &LanceTransactionProperties,
) -> Result<HashMap<String, String>> {
    if properties.count > 0 && properties.keys.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "transaction_properties.keys must not be NULL when count = {}",
            properties.count
        )));
    }
    if properties.count > 0 && properties.values.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "transaction_properties.values must not be NULL when count = {}",
            properties.count
        )));
    }

    let mut parsed = HashMap::with_capacity(properties.count);
    for index in 0..properties.count {
        let key = unsafe { *properties.keys.add(index) };
        let key = unsafe { helpers::parse_c_string(key) }
            .map_err(|error| {
                lance_core::Error::invalid_input(format!(
                    "transaction_properties.keys[{index}]: {error}"
                ))
            })?
            .ok_or_else(|| {
                lance_core::Error::invalid_input(format!(
                    "transaction_properties.keys[{index}] must not be NULL"
                ))
            })?;
        let value = unsafe { *properties.values.add(index) };
        let value = unsafe { helpers::parse_c_string(value) }
            .map_err(|error| {
                lance_core::Error::invalid_input(format!(
                    "transaction_properties.values[{index}]: {error}"
                ))
            })?
            .ok_or_else(|| {
                lance_core::Error::invalid_input(format!(
                    "transaction_properties.values[{index}] must not be NULL"
                ))
            })?;
        parsed.insert(key.to_owned(), value.to_owned());
    }
    Ok(parsed)
}
