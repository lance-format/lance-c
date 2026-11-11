// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Owned native `UpdateMap` values and their C ABI.
//!
//! `UpdateMap` is an ordered list of key edits plus a replacement flag, not a
//! map. Native commit code applies the entries in order: replacement clears the
//! target and then inserts entries with values while ignoring `None` entries,
//! and incremental mode upserts `Some` values and removes `None` keys. The
//! entry list is therefore copied verbatim; duplicate keys and their order are
//! native data and are never normalized here.
//!
//! Borrowed key/value views require NUL-terminated storage, so every handle
//! keeps one conversion result per entry. Native history can contain keys or
//! values with embedded NUL bytes that a C-string getter cannot represent; only
//! that entry's borrowed view fails, while cloning the map and inspecting other
//! entries keeps working.

use std::ffi::{CStr, CString, c_char};
use std::ptr;

use lance_core::Result;
use lance_table::transaction::{UpdateMap, UpdateMapEntry};

use crate::error::{ffi_try, swallow_unwind};

/// Owned native `UpdateMap` snapshot.
pub struct LanceUpdateMap {
    pub(crate) inner: UpdateMap,
    /// Owned C-string storage backing borrowed entry views. Key and value
    /// conversions are retained independently so an embedded NUL in one does
    /// not make the other unavailable.
    entries: Vec<OwnedUpdateMapEntry>,
}

struct OwnedUpdateMapEntry {
    key: Result<CString>,
    value: Option<Result<CString>>,
}

impl OwnedUpdateMapEntry {
    fn from_native(entry: &UpdateMapEntry) -> Self {
        let key = CString::new(entry.key.as_str()).map_err(|_| {
            lance_core::Error::internal(format!(
                "update map key {:?} contains an embedded NUL byte",
                entry.key
            ))
        });
        let value = entry.value.as_ref().map(|value| {
            CString::new(value.as_str()).map_err(|_| {
                lance_core::Error::internal(format!(
                    "update map value {value:?} for key {:?} contains an embedded NUL byte",
                    entry.key
                ))
            })
        });
        Self { key, value }
    }
}

impl LanceUpdateMap {
    /// Wrap a native value, retaining C-string storage for borrowed views.
    pub(crate) fn from_native(inner: UpdateMap) -> Self {
        let entries = inner
            .update_entries
            .iter()
            .map(OwnedUpdateMapEntry::from_native)
            .collect();
        Self { inner, entries }
    }
}

pub(crate) fn update_map_at<'a>(map: *const LanceUpdateMap) -> Result<&'a LanceUpdateMap> {
    if map.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "update map must not be NULL".into(),
        ));
    }
    Ok(unsafe { &*map })
}

fn borrowed_entry(map: &LanceUpdateMap, index: usize) -> Result<&OwnedUpdateMapEntry> {
    let count = map.inner.update_entries.len();
    if map.entries.len() != count {
        return Err(lance_core::Error::internal(
            "update map view storage is inconsistent with native entries".to_string(),
        ));
    }
    let entry = map.entries.get(index).ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!("update map entry index {index} is out of range for {count} entries").into(),
        )
    })?;
    Ok(entry)
}

/// Parse a borrowed `LanceUpdateMap` pointer as an optional owned native value.
///
/// NULL maps to native `None`; a non-NULL handle is cloned, including an
/// explicitly empty map.
pub(crate) unsafe fn parse_update_map(map: *const LanceUpdateMap) -> Result<Option<UpdateMap>> {
    if map.is_null() {
        return Ok(None);
    }
    Ok(Some(unsafe { &*map }.inner.clone()))
}

/// Parse borrowed key/value arrays into one native `UpdateMap`.
///
/// `keys[i]` is required and `values[i] == NULL` is native `None`. Duplicate
/// keys and their order are retained. Native commit code interprets the list.
unsafe fn parse_update_map_entries(
    keys: *const *const c_char,
    values: *const *const c_char,
    count: usize,
    replace: bool,
) -> Result<UpdateMap> {
    if keys.is_null() && count > 0 {
        return Err(lance_core::Error::invalid_input_source(
            format!("keys must not be NULL when count = {count}").into(),
        ));
    }
    if values.is_null() && count > 0 {
        return Err(lance_core::Error::invalid_input_source(
            format!("values must not be NULL when count = {count}").into(),
        ));
    }
    if count > isize::MAX as usize / std::mem::size_of::<*const c_char>() {
        return Err(lance_core::Error::invalid_input_source(
            format!("count {count} exceeds the maximum addressable pointer slice length").into(),
        ));
    }
    if count > isize::MAX as usize / std::mem::size_of::<UpdateMapEntry>() {
        return Err(lance_core::Error::invalid_input_source(
            format!("count {count} exceeds the maximum addressable update entry length").into(),
        ));
    }

    let mut entries = Vec::with_capacity(count);
    for index in 0..count {
        let key = unsafe { *keys.add(index) };
        if key.is_null() {
            return Err(lance_core::Error::invalid_input_source(
                format!("keys[{index}] must not be NULL when count = {count}").into(),
            ));
        }
        let key = unsafe { CStr::from_ptr(key) }.to_str().map_err(|error| {
            lance_core::Error::invalid_input_source(
                format!("keys[{index}] is not valid UTF-8: {error}").into(),
            )
        })?;
        let value = unsafe { *values.add(index) };
        let value = if value.is_null() {
            None
        } else {
            Some(
                unsafe { CStr::from_ptr(value) }
                    .to_str()
                    .map_err(|error| {
                        lance_core::Error::invalid_input_source(
                            format!("values[{index}] is not valid UTF-8: {error}").into(),
                        )
                    })?
                    .to_owned(),
            )
        };
        entries.push(UpdateMapEntry {
            key: key.to_owned(),
            value,
        });
    }
    Ok(UpdateMap {
        update_entries: entries,
        replace,
    })
}

/// Construct an owned native `UpdateMap` from borrowed UTF-8 arrays.
///
/// `count == 0` accepts a NULL array for either argument; every entry requires
/// a non-NULL key, and a NULL value means "remove this key". Duplicate keys and
/// input order are retained. `replace` is the caller's native mode selection.
/// Native commit code owns replacement/incremental semantics. Returns NULL on
/// error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_update_map_new(
    keys: *const *const c_char,
    values: *const *const c_char,
    count: usize,
    replace: bool,
) -> *mut LanceUpdateMap {
    ffi_try!(
        unsafe { update_map_new_inner(keys, values, count, replace) },
        null
    )
}

unsafe fn update_map_new_inner(
    keys: *const *const c_char,
    values: *const *const c_char,
    count: usize,
    replace: bool,
) -> Result<*mut LanceUpdateMap> {
    let inner = unsafe { parse_update_map_entries(keys, values, count, replace) }?;
    Ok(Box::into_raw(Box::new(LanceUpdateMap::from_native(inner))))
}

/// Clone an update map into an independent snapshot.
///
/// The clone stays valid after the source handle is closed. Returns NULL on
/// error. The caller closes the returned handle exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_update_map_clone(map: *const LanceUpdateMap) -> *mut LanceUpdateMap {
    ffi_try!(unsafe { update_map_clone_inner(map) }, null)
}

unsafe fn update_map_clone_inner(map: *const LanceUpdateMap) -> Result<*mut LanceUpdateMap> {
    let map = update_map_at(map)?;
    Ok(Box::into_raw(Box::new(LanceUpdateMap::from_native(
        map.inner.clone(),
    ))))
}

/// Close an update map handle. NULL-safe. A non-NULL handle must be closed
/// exactly once and must not be used afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_update_map_close(map: *mut LanceUpdateMap) {
    if !map.is_null() {
        swallow_unwind("lance_update_map_close", || unsafe {
            let _ = Box::from_raw(map);
        });
    }
}

/// Read the number of native update entries.
///
/// Returns 0 on error; check `lance_last_error_code()`. Zero is also a valid
/// native entry count.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_update_map_count(map: *const LanceUpdateMap) -> usize {
    ffi_try!(Ok(update_map_at(map)?.inner.update_entries.len()), 0)
}

/// Read one borrowed entry key. The pointer remains valid until the update map
/// handle is closed. Returns NULL on error, including a key that cannot be
/// represented as a C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_update_map_key_at(
    map: *const LanceUpdateMap,
    index: usize,
) -> *const c_char {
    ffi_try!(unsafe { update_map_key_at_inner(map, index) }, ptr::null())
}

unsafe fn update_map_key_at_inner(
    map: *const LanceUpdateMap,
    index: usize,
) -> Result<*const c_char> {
    let map = update_map_at(map)?;
    let entry = borrowed_entry(map, index)?;
    let key = entry
        .key
        .as_ref()
        .map_err(|error| lance_core::Error::internal(error.to_string()))?;
    Ok(key.as_ptr())
}

/// Read one borrowed entry value.
///
/// Both output pointers are required. On success `*out_has_value` reports
/// native `Some`/`None`; `*out_value` receives a borrowed pointer when present
/// and NULL otherwise. The pointer remains valid until the update map handle is
/// closed. Returns 0 on success, -1 on error, and leaves both outputs unchanged
/// on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_update_map_value_at(
    map: *const LanceUpdateMap,
    index: usize,
    out_has_value: *mut bool,
    out_value: *mut *const c_char,
) -> i32 {
    ffi_try!(
        unsafe { update_map_value_at_inner(map, index, out_has_value, out_value) },
        neg
    )
}

unsafe fn update_map_value_at_inner(
    map: *const LanceUpdateMap,
    index: usize,
    out_has_value: *mut bool,
    out_value: *mut *const c_char,
) -> Result<i32> {
    if out_has_value.is_null() || out_value.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_has_value and out_value must not be NULL".into(),
        ));
    }
    let map = update_map_at(map)?;
    let entry = borrowed_entry(map, index)?;
    let value = match &entry.value {
        Some(Ok(value)) => Some(value.as_ptr()),
        Some(Err(error)) => return Err(lance_core::Error::internal(error.to_string())),
        None => None,
    };
    unsafe {
        ptr::write(out_has_value, entry.value.is_some());
        ptr::write(out_value, value.unwrap_or(ptr::null()));
    }
    Ok(0)
}

/// Read the native replacement flag. `true` replaces the target map; `false`
/// applies the entries incrementally. Returns false on error; check
/// `lance_last_error_code()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_update_map_replace(map: *const LanceUpdateMap) -> bool {
    ffi_try!(Ok(update_map_at(map)?.inner.replace), false)
}
