// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Static and dynamic storage-options C API.
//!
//! The provider adapter delegates caching, expiration, and refresh policy to
//! the native `StorageOptionsAccessor`. It only translates the optional map
//! and owned callback errors across the C ABI.

use std::collections::HashMap;
use std::ffi::{CStr, CString, c_char, c_void};
use std::fmt;
use std::ptr;
use std::sync::Arc;

use async_trait::async_trait;
use lance_core::{Error, Result};
use lance_io::object_store::{StorageOptionsAccessor, StorageOptionsProvider};

use crate::callback::{LanceCallbackError, callback_error, release_message_only};
use crate::error::{ffi_try, swallow_unwind};
use crate::runtime::block_on;

/// Borrowed static storage options.
///
/// A NULL input pointer maps to native `None`. A non-NULL pointer with
/// `count == 0` maps to native `Some(empty)`.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LanceStorageOptionsInput {
    pub keys: *const *const c_char,
    pub values: *const *const c_char,
    pub count: usize,
}

/// Result slot for one storage-options provider invocation.
///
/// `has_options == 0` maps to native `None`. A nonzero value maps to native
/// `Some`, including `count == 0` for an explicitly empty map. When
/// `release_snapshot` is non-NULL it is called exactly once after the map has
/// been copied, on success and failure alike. Without a release callback the
/// keys and values are borrowed only until the callback returns.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LanceStorageOptionsFetchResult {
    pub has_options: i32,
    pub keys: *const *const c_char,
    pub values: *const *const c_char,
    pub count: usize,
    pub snapshot_ctx: *mut c_void,
    pub release_snapshot: Option<unsafe extern "C" fn(*mut c_void)>,
}

/// Provider fetch callback. Return 0 on success or nonzero with `error` filled.
pub type LanceStorageOptionsFetchFn = Option<
    unsafe extern "C" fn(
        provider_ctx: *mut c_void,
        out: *mut LanceStorageOptionsFetchResult,
        error: *mut LanceCallbackError,
    ) -> i32,
>;

/// Release the caller-owned provider context exactly once.
pub type LanceStorageOptionsProviderReleaseFn = Option<unsafe extern "C" fn(*mut c_void)>;

/// Foreign storage-options provider declaration.
///
/// `force_fetch` is optional; when omitted it uses `fetch`, matching the
/// native trait default. `provider_id` is copied and defines native equality
/// and object-store registry cache identity. Ownership of `provider_ctx` and
/// `release` transfers only when provider construction succeeds.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceStorageOptionsProviderSpec {
    pub fetch: LanceStorageOptionsFetchFn,
    pub force_fetch: LanceStorageOptionsFetchFn,
    pub release: LanceStorageOptionsProviderReleaseFn,
    pub provider_ctx: *mut c_void,
    pub provider_id: *const c_char,
}

pub struct LanceStorageOptionsProvider {
    pub(crate) inner: Arc<FfiStorageOptionsProvider>,
}

pub struct LanceStorageOptionsAccessor {
    pub(crate) inner: Option<Arc<StorageOptionsAccessor>>,
}

pub struct LanceStorageOptionsSnapshot {
    has_options: bool,
    entries: Vec<(CString, CString)>,
}

struct OwnedProviderContext {
    callback_ctx: *mut c_void,
    release: Option<unsafe extern "C" fn(*mut c_void)>,
}

// SAFETY: the C contract requires the provider context and callbacks to remain
// usable from native worker threads until the final Arc releases the context.
unsafe impl Send for OwnedProviderContext {}
unsafe impl Sync for OwnedProviderContext {}

impl Drop for OwnedProviderContext {
    fn drop(&mut self) {
        let Some(release) = self.release else {
            return;
        };
        let callback_ctx = self.callback_ctx;
        swallow_unwind("storage options provider release", || unsafe {
            release(callback_ctx);
        });
    }
}

struct FfiStorageOptionsProvider {
    fetch: unsafe extern "C" fn(
        *mut c_void,
        *mut LanceStorageOptionsFetchResult,
        *mut LanceCallbackError,
    ) -> i32,
    force_fetch: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *mut LanceStorageOptionsFetchResult,
            *mut LanceCallbackError,
        ) -> i32,
    >,
    provider_id: String,
    context: OwnedProviderContext,
}

impl fmt::Debug for FfiStorageOptionsProvider {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("FfiStorageOptionsProvider")
            .field("provider_id", &self.provider_id)
            .finish()
    }
}

struct FetchSnapshot {
    callback_ctx: *mut c_void,
    release: Option<unsafe extern "C" fn(*mut c_void)>,
}

impl FetchSnapshot {
    fn new(result: &LanceStorageOptionsFetchResult) -> Self {
        Self {
            callback_ctx: result.snapshot_ctx,
            release: result.release_snapshot,
        }
    }
}

impl Drop for FetchSnapshot {
    fn drop(&mut self) {
        let Some(release) = self.release else {
            return;
        };
        let callback_ctx = self.callback_ctx;
        swallow_unwind("storage options result snapshot release", || unsafe {
            release(callback_ctx);
        });
    }
}

impl FfiStorageOptionsProvider {
    fn call(
        &self,
        callback: unsafe extern "C" fn(
            *mut c_void,
            *mut LanceStorageOptionsFetchResult,
            *mut LanceCallbackError,
        ) -> i32,
        context: &str,
    ) -> Result<Option<HashMap<String, String>>> {
        let mut result = empty_fetch_result();
        let mut error = LanceCallbackError::empty();
        // SAFETY: the context and callback table remain valid for the complete
        // synchronous invocation. The output slots are initialized and writable.
        let status = unsafe { callback(self.context.callback_ctx, &mut result, &mut error) };
        let _snapshot = FetchSnapshot::new(&result);

        if status != 0 {
            return Err(unsafe { callback_error(&mut error, status, context) });
        }

        let options = unsafe { copy_fetch_result(&result, context) };
        unsafe { release_message_only(&mut error) };
        options
    }
}

#[async_trait]
impl StorageOptionsProvider for FfiStorageOptionsProvider {
    async fn fetch_storage_options(&self) -> Result<Option<HashMap<String, String>>> {
        self.call(self.fetch, "storage options provider fetch")
    }

    async fn force_fetch_storage_options(&self) -> Result<Option<HashMap<String, String>>> {
        self.call(
            self.force_fetch.unwrap_or(self.fetch),
            "storage options provider force fetch",
        )
    }

    fn provider_id(&self) -> String {
        self.provider_id.clone()
    }
}

/// Create a foreign storage-options provider.
///
/// On success, ownership of `provider_ctx` and its optional `release` callback
/// transfers to the returned provider and ultimately to the native accessor or
/// registry entry that retains the provider. `release(provider_ctx)` is called
/// exactly once when the final native owner is dropped. On failure, ownership
/// remains with the caller. `provider_id` is copied and must be non-empty.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_storage_options_provider_new(
    spec: *const LanceStorageOptionsProviderSpec,
) -> *mut LanceStorageOptionsProvider {
    ffi_try!(unsafe { storage_options_provider_new_inner(spec) }, null)
}

unsafe fn storage_options_provider_new_inner(
    spec: *const LanceStorageOptionsProviderSpec,
) -> Result<*mut LanceStorageOptionsProvider> {
    if spec.is_null() {
        return Err(Error::invalid_input_source("spec must not be NULL".into()));
    }
    let spec = unsafe { &*spec };
    let fetch = spec
        .fetch
        .ok_or_else(|| Error::invalid_input_source("spec.fetch must not be NULL".into()))?;
    let provider_id = unsafe { required_c_string(spec.provider_id, "spec.provider_id")? };
    if provider_id.is_empty() {
        return Err(Error::invalid_input_source(
            "spec.provider_id must not be empty".into(),
        ));
    }

    let provider = FfiStorageOptionsProvider {
        fetch,
        force_fetch: spec.force_fetch,
        provider_id: provider_id.to_owned(),
        context: OwnedProviderContext {
            callback_ctx: spec.provider_ctx,
            release: spec.release,
        },
    };
    Ok(Box::into_raw(Box::new(LanceStorageOptionsProvider {
        inner: Arc::new(provider),
    })))
}

/// Close a storage-options provider handle.
///
/// Accessors and object-store registry entries retain their own native Arc
/// references. The caller's context is released only when the final owner is
/// dropped, not necessarily by this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_storage_options_provider_close(
    provider: *mut LanceStorageOptionsProvider,
) {
    if !provider.is_null() {
        swallow_unwind("lance_storage_options_provider_close", || unsafe {
            let _ = Box::from_raw(provider);
        });
    }
}

/// Create a static and/or provider-backed storage-options accessor.
///
/// `initial == NULL` maps to native `None`; a non-NULL input, including one
/// with count zero, maps to native `Some`. With neither initial options nor a
/// provider the handle represents native `None`. Duplicate keys use the native
/// map's last-write-wins semantics.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_storage_options_accessor_new(
    initial: *const LanceStorageOptionsInput,
    provider: *const LanceStorageOptionsProvider,
) -> *mut LanceStorageOptionsAccessor {
    ffi_try!(
        unsafe { storage_options_accessor_new_inner(initial, provider) },
        null
    )
}

unsafe fn storage_options_accessor_new_inner(
    initial: *const LanceStorageOptionsInput,
    provider: *const LanceStorageOptionsProvider,
) -> Result<*mut LanceStorageOptionsAccessor> {
    let initial = if initial.is_null() {
        None
    } else {
        Some(unsafe { copy_storage_options_input(&*initial)? })
    };
    let provider = if provider.is_null() {
        None
    } else {
        Some(unsafe { &*provider }.inner.clone() as Arc<dyn StorageOptionsProvider>)
    };

    let inner = match (initial, provider) {
        (None, None) => None,
        (Some(options), None) => Some(Arc::new(StorageOptionsAccessor::with_static_options(
            options,
        ))),
        (None, Some(provider)) => Some(Arc::new(StorageOptionsAccessor::with_provider(provider))),
        (Some(options), Some(provider)) => Some(Arc::new(
            StorageOptionsAccessor::with_initial_and_provider(options, provider),
        )),
    };
    Ok(Box::into_raw(Box::new(LanceStorageOptionsAccessor {
        inner,
    })))
}

/// Close an accessor handle. Providers and options retained by native object
/// stores keep their own shared ownership.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_storage_options_accessor_close(
    accessor: *mut LanceStorageOptionsAccessor,
) {
    if !accessor.is_null() {
        swallow_unwind("lance_storage_options_accessor_close", || unsafe {
            let _ = Box::from_raw(accessor);
        });
    }
}

/// Snapshot the accessor's current resolved options.
///
/// This follows native `StorageOptionsAccessor::get_storage_options`, including
/// cache and expiration behavior. Provider refresh happens in the native
/// accessor; this binding keeps no second cache. A NULL handle or provider
/// error returns NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_storage_options_accessor_snapshot(
    accessor: *const LanceStorageOptionsAccessor,
) -> *mut LanceStorageOptionsSnapshot {
    ffi_try!(
        unsafe { storage_options_accessor_snapshot_inner(accessor) },
        null
    )
}

unsafe fn storage_options_accessor_snapshot_inner(
    accessor: *const LanceStorageOptionsAccessor,
) -> Result<*mut LanceStorageOptionsSnapshot> {
    if accessor.is_null() {
        return Err(Error::invalid_input_source(
            "accessor must not be NULL".into(),
        ));
    }
    let accessor = unsafe { &*accessor };
    let Some(accessor) = &accessor.inner else {
        return Ok(Box::into_raw(Box::new(LanceStorageOptionsSnapshot {
            has_options: false,
            entries: Vec::new(),
        })));
    };
    let options = block_on(accessor.get_storage_options())?.0;
    let mut entries = options
        .into_iter()
        .map(|(key, value)| {
            let key = CString::new(key).map_err(|error| {
                Error::invalid_input(format!(
                    "storage option key contains an embedded NUL: {error}"
                ))
            })?;
            let value = CString::new(value).map_err(|error| {
                Error::invalid_input(format!(
                    "storage option value contains an embedded NUL: {error}"
                ))
            })?;
            Ok((key, value))
        })
        .collect::<Result<Vec<_>>>()?;
    entries.sort_by(|left, right| left.0.as_bytes().cmp(right.0.as_bytes()));
    Ok(Box::into_raw(Box::new(LanceStorageOptionsSnapshot {
        has_options: true,
        entries,
    })))
}

/// Return 1 for native `Some`, 0 for native `None`, or -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_storage_options_snapshot_has_options(
    snapshot: *const LanceStorageOptionsSnapshot,
) -> i32 {
    ffi_try!(
        unsafe { storage_options_snapshot_has_options_inner(snapshot) },
        neg
    )
}

unsafe fn storage_options_snapshot_has_options_inner(
    snapshot: *const LanceStorageOptionsSnapshot,
) -> Result<i32> {
    if snapshot.is_null() {
        return Err(Error::invalid_input_source(
            "snapshot must not be NULL".into(),
        ));
    }
    Ok(i32::from(unsafe { &*snapshot }.has_options))
}

/// Number of options in the snapshot. A NULL snapshot returns 0 with a
/// thread-local error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_storage_options_snapshot_count(
    snapshot: *const LanceStorageOptionsSnapshot,
) -> u64 {
    ffi_try!(unsafe { storage_options_snapshot_count_inner(snapshot) }, 0)
}

unsafe fn storage_options_snapshot_count_inner(
    snapshot: *const LanceStorageOptionsSnapshot,
) -> Result<u64> {
    if snapshot.is_null() {
        return Err(Error::invalid_input_source(
            "snapshot must not be NULL".into(),
        ));
    }
    Ok(unsafe { &*snapshot }.entries.len() as u64)
}

/// Borrowed key at `index`; valid until the snapshot is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_storage_options_snapshot_key_at(
    snapshot: *const LanceStorageOptionsSnapshot,
    index: usize,
) -> *const c_char {
    ffi_try!(
        unsafe { storage_options_snapshot_key_at_inner(snapshot, index) },
        null
    )
}

unsafe fn storage_options_snapshot_key_at_inner(
    snapshot: *const LanceStorageOptionsSnapshot,
    index: usize,
) -> Result<*const c_char> {
    if snapshot.is_null() {
        return Err(Error::invalid_input_source(
            "snapshot must not be NULL".into(),
        ));
    }
    let snapshot = unsafe { &*snapshot };
    let entry = snapshot.entries.get(index).ok_or_else(|| {
        Error::invalid_input(format!(
            "storage option index {index} is out of bounds (count={})",
            snapshot.entries.len()
        ))
    })?;
    Ok(entry.0.as_ptr())
}

/// Borrowed value at `index`; valid until the snapshot is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_storage_options_snapshot_value_at(
    snapshot: *const LanceStorageOptionsSnapshot,
    index: usize,
) -> *const c_char {
    ffi_try!(
        unsafe { storage_options_snapshot_value_at_inner(snapshot, index) },
        null
    )
}

unsafe fn storage_options_snapshot_value_at_inner(
    snapshot: *const LanceStorageOptionsSnapshot,
    index: usize,
) -> Result<*const c_char> {
    if snapshot.is_null() {
        return Err(Error::invalid_input_source(
            "snapshot must not be NULL".into(),
        ));
    }
    let snapshot = unsafe { &*snapshot };
    let entry = snapshot.entries.get(index).ok_or_else(|| {
        Error::invalid_input(format!(
            "storage option index {index} is out of bounds (count={})",
            snapshot.entries.len()
        ))
    })?;
    Ok(entry.1.as_ptr())
}

/// Close a storage-options snapshot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_storage_options_snapshot_close(
    snapshot: *mut LanceStorageOptionsSnapshot,
) {
    if !snapshot.is_null() {
        swallow_unwind("lance_storage_options_snapshot_close", || unsafe {
            let _ = Box::from_raw(snapshot);
        });
    }
}

unsafe fn copy_storage_options_input(
    input: &LanceStorageOptionsInput,
) -> Result<HashMap<String, String>> {
    unsafe { copy_string_map(input.keys, input.values, input.count, "storage options") }
}

unsafe fn copy_fetch_result(
    result: &LanceStorageOptionsFetchResult,
    context: &str,
) -> Result<Option<HashMap<String, String>>> {
    if result.has_options == 0 {
        if result.count != 0 {
            return Err(Error::invalid_input(format!(
                "{context} returned has_options=0 with count={}",
                result.count
            )));
        }
        return Ok(None);
    }
    unsafe { copy_string_map(result.keys, result.values, result.count, context).map(Some) }
}

unsafe fn copy_string_map(
    keys: *const *const c_char,
    values: *const *const c_char,
    count: usize,
    context: &str,
) -> Result<HashMap<String, String>> {
    if count == 0 {
        return Ok(HashMap::new());
    }
    if keys.is_null() || values.is_null() {
        return Err(Error::invalid_input(format!(
            "{context}: keys and values must not be NULL when count={count}"
        )));
    }
    let mut output = HashMap::with_capacity(count);
    for index in 0..count {
        let key_ptr = unsafe { *keys.add(index) };
        let value_ptr = unsafe { *values.add(index) };
        let key = unsafe { required_c_string(key_ptr, &format!("{context}.keys[{index}]")) }?;
        let value = unsafe { required_c_string(value_ptr, &format!("{context}.values[{index}]")) }?;
        output.insert(key.to_owned(), value.to_owned());
    }
    Ok(output)
}

unsafe fn required_c_string<'a>(value: *const c_char, field: &str) -> Result<&'a str> {
    if value.is_null() {
        return Err(Error::invalid_input(format!("{field} must not be NULL")));
    }
    // SAFETY: the caller supplies a NUL-terminated string for this call.
    let value = unsafe { CStr::from_ptr(value) };
    value
        .to_str()
        .map_err(|error| Error::invalid_input(format!("{field} is not valid UTF-8: {error}")))
}

fn empty_fetch_result() -> LanceStorageOptionsFetchResult {
    LanceStorageOptionsFetchResult {
        has_options: 0,
        keys: ptr::null(),
        values: ptr::null(),
        count: 0,
        snapshot_ctx: ptr::null_mut(),
        release_snapshot: None,
    }
}
