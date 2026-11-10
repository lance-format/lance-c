// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Shared Lance session C API.

use std::collections::HashMap;
use std::collections::hash_map::Entry;
use std::ffi::c_char;
use std::sync::Arc;

use lance::session::{CacheSpec, Session};
use lance_core::Result;
use lance_core::cache::{BackendConfig, build_from_config, build_from_uri};

use crate::error::{ffi_try, swallow_unwind};
use crate::helpers::parse_c_string;
use crate::runtime::block_on;

/// Opaque handle for sharing Lance metadata and index caches across datasets.
pub struct LanceSession {
    pub(crate) inner: Arc<Session>,
}

/// Cache selection for one session cache tier.
///
/// Discriminants are pinned for ABI stability. The corresponding field in
/// [`LanceSessionCacheSpec`] is an `i32` so callers built with
/// `-fshort-enums` cannot mismatch the Rust ABI.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceSessionCacheSpecKind {
    /// Use the native Lance default capacity for this tier.
    Default = 0,
    /// Use the native default in-memory backend with this explicit capacity.
    Size = 1,
    /// Build a backend from a backend URI.
    Uri = 2,
    /// Build a backend from a kind plus string options.
    Config = 3,
}

impl LanceSessionCacheSpecKind {
    fn from_raw(raw: i32) -> Result<Self> {
        match raw {
            0 => Ok(Self::Default),
            1 => Ok(Self::Size),
            2 => Ok(Self::Uri),
            3 => Ok(Self::Config),
            other => Err(lance_core::Error::invalid_input_source(
                format!(
                    "invalid session cache spec kind {other}; expected 0 (default), \
                     1 (size), 2 (uri), or 3 (config)"
                )
                .into(),
            )),
        }
    }
}

/// Description of one session cache tier.
///
/// Only the field selected by `kind` is read. All strings and option arrays
/// are borrowed for the duration of [`lance_session_new_with_cache_specs`];
/// the native backend constructors copy everything they need before it
/// returns. `option_count` is an exact count, not a NULL-terminated sentinel.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceSessionCacheSpec {
    /// A [`LanceSessionCacheSpecKind`] discriminant.
    pub kind: i32,
    /// Explicit capacity for `Size`, in bytes.
    pub size_bytes: u64,
    /// Backend URI for `Uri`, e.g. `moka://?capacity=1048576`.
    pub backend_uri: *const c_char,
    /// Registry kind for `Config`, e.g. `moka`.
    pub backend_kind: *const c_char,
    /// First entry in the backend option-key array for `Config`.
    pub option_keys: *const *const c_char,
    /// Parallel backend option-value array for `Config`.
    pub option_values: *const *const c_char,
    /// Number of entries in `option_keys` and `option_values`.
    pub option_count: usize,
}

/// Snapshot of a session's metadata and index cache statistics.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LanceSessionCacheStats {
    pub index_cache_hits: u64,
    pub index_cache_misses: u64,
    pub index_cache_entries: u64,
    pub index_cache_size_bytes: u64,
    pub metadata_cache_hits: u64,
    pub metadata_cache_misses: u64,
    pub metadata_cache_entries: u64,
    pub metadata_cache_size_bytes: u64,
}

/// Create a shared Lance session with byte-based cache limits.
///
/// A zero limit requests zero capacity for the corresponding cache.
#[unsafe(no_mangle)]
pub extern "C" fn lance_session_new(
    index_cache_size_bytes: u64,
    metadata_cache_size_bytes: u64,
) -> *mut LanceSession {
    ffi_try!(
        session_new_inner(index_cache_size_bytes, metadata_cache_size_bytes),
        null
    )
}

fn session_new_inner(
    index_cache_size_bytes: u64,
    metadata_cache_size_bytes: u64,
) -> Result<*mut LanceSession> {
    let index_cache_size_bytes = u64_to_usize(index_cache_size_bytes, "index_cache_size_bytes")?;
    let metadata_cache_size_bytes =
        u64_to_usize(metadata_cache_size_bytes, "metadata_cache_size_bytes")?;
    let session = Session::new(
        index_cache_size_bytes,
        metadata_cache_size_bytes,
        Default::default(),
    );
    Ok(Box::into_raw(Box::new(LanceSession {
        inner: Arc::new(session),
    })))
}

/// Create a shared Lance session with independent cache configuration per tier.
///
/// Each spec maps directly to the selected Lance core `CacheSpec`:
/// `Default` uses the native tier default, `Size` uses the native default
/// in-memory backend with an explicit capacity (zero means zero capacity), and
/// `Uri`/`Config` build a backend through the native cache registry.
///
/// Both spec pointers must be non-NULL. Backend URI parsing, kind
/// normalization, option validation, and unknown-kind errors come from Lance
/// core. For `Config`, duplicate option keys are rejected because the native
/// map representation cannot preserve a duplicate.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_session_new_with_cache_specs(
    index_cache: *const LanceSessionCacheSpec,
    metadata_cache: *const LanceSessionCacheSpec,
) -> *mut LanceSession {
    ffi_try!(
        unsafe { session_new_with_cache_specs_inner(index_cache, metadata_cache) },
        null
    )
}

unsafe fn session_new_with_cache_specs_inner(
    index_cache: *const LanceSessionCacheSpec,
    metadata_cache: *const LanceSessionCacheSpec,
) -> Result<*mut LanceSession> {
    if index_cache.is_null() || metadata_cache.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "index_cache and metadata_cache must not be NULL".into(),
        ));
    }
    let index_cache = unsafe { &*index_cache };
    let metadata_cache = unsafe { &*metadata_cache };
    let index_cache = unsafe { resolve_cache_spec(index_cache, "index_cache") }?;
    let metadata_cache = unsafe { resolve_cache_spec(metadata_cache, "metadata_cache") }?;
    let session = Session::with_cache_backends(index_cache, metadata_cache, Default::default());
    Ok(Box::into_raw(Box::new(LanceSession {
        inner: Arc::new(session),
    })))
}

unsafe fn resolve_cache_spec(
    spec: &LanceSessionCacheSpec,
    field: &'static str,
) -> Result<CacheSpec> {
    match LanceSessionCacheSpecKind::from_raw(spec.kind)? {
        LanceSessionCacheSpecKind::Default => Ok(CacheSpec::Default),
        LanceSessionCacheSpecKind::Size => {
            let field = if field == "index_cache" {
                "index_cache.size_bytes"
            } else {
                "metadata_cache.size_bytes"
            };
            Ok(CacheSpec::Size(u64_to_usize(spec.size_bytes, field)?))
        }
        LanceSessionCacheSpecKind::Uri => {
            let field = if field == "index_cache" {
                "index_cache.backend_uri"
            } else {
                "metadata_cache.backend_uri"
            };
            let uri = unsafe { required_c_string(spec.backend_uri, field) }?;
            build_from_uri(uri).map(CacheSpec::Backend)
        }
        LanceSessionCacheSpecKind::Config => {
            let (kind_field, options_field) = if field == "index_cache" {
                ("index_cache.backend_kind", "index_cache.backend_options")
            } else {
                (
                    "metadata_cache.backend_kind",
                    "metadata_cache.backend_options",
                )
            };
            let kind = unsafe { required_c_string(spec.backend_kind, kind_field) }?;
            let mut config = BackendConfig::new(kind)?;
            config.options = unsafe { parse_backend_options(spec, options_field) }?;
            build_from_config(&config).map(CacheSpec::Backend)
        }
    }
}

unsafe fn required_c_string<'a>(value: *const c_char, field: &str) -> Result<&'a str> {
    let Some(value) = (unsafe { parse_c_string(value) })? else {
        return Err(lance_core::Error::invalid_input_source(
            format!("{field} must not be NULL").into(),
        ));
    };
    Ok(value)
}

unsafe fn parse_backend_options(
    spec: &LanceSessionCacheSpec,
    field: &str,
) -> Result<HashMap<String, String>> {
    if spec.option_count == 0 {
        return Ok(HashMap::new());
    }
    if spec.option_keys.is_null() || spec.option_values.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            format!(
                "{field}: option_keys and option_values must not be NULL when option_count={}",
                spec.option_count
            )
            .into(),
        ));
    }

    let mut options = HashMap::new();
    for index in 0..spec.option_count {
        let key_ptr = unsafe { *spec.option_keys.add(index) };
        let value_ptr = unsafe { *spec.option_values.add(index) };
        let key = unsafe { required_c_string(key_ptr, &format!("{field}.option_keys[{index}]")) }?;
        let value =
            unsafe { required_c_string(value_ptr, &format!("{field}.option_values[{index}]")) }?;
        match options.entry(key.to_owned()) {
            Entry::Occupied(entry) => {
                return Err(lance_core::Error::invalid_input_source(
                    format!("{field}: duplicate option key {:?}", entry.key()).into(),
                ));
            }
            Entry::Vacant(entry) => {
                entry.insert(value.to_owned());
            }
        }
    }
    Ok(options)
}

/// Close a session handle.
///
/// Datasets opened with this session retain shared ownership of its runtime
/// state and remain valid after this handle is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_session_close(session: *mut LanceSession) {
    if !session.is_null() {
        swallow_unwind("lance_session_close", || unsafe {
            let _ = Box::from_raw(session);
        });
    }
}

/// Copy the current cache statistics into `out_stats`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_session_get_cache_stats(
    session: *const LanceSession,
    out_stats: *mut LanceSessionCacheStats,
) -> i32 {
    ffi_try!(
        unsafe { session_get_cache_stats_inner(session, out_stats) },
        neg
    )
}

unsafe fn session_get_cache_stats_inner(
    session: *const LanceSession,
    out_stats: *mut LanceSessionCacheStats,
) -> Result<i32> {
    if session.is_null() || out_stats.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "session and out_stats must not be NULL".into(),
        ));
    }
    let session = unsafe { &*session };
    let (index, metadata) = block_on(async {
        let index = session.inner.index_cache_stats().await;
        let metadata = session.inner.metadata_cache_stats().await;
        (index, metadata)
    });
    unsafe {
        std::ptr::write_unaligned(
            out_stats,
            LanceSessionCacheStats {
                index_cache_hits: index.hits,
                index_cache_misses: index.misses,
                index_cache_entries: index.num_entries as u64,
                index_cache_size_bytes: index.size_bytes as u64,
                metadata_cache_hits: metadata.hits,
                metadata_cache_misses: metadata.misses,
                metadata_cache_entries: metadata.num_entries as u64,
                metadata_cache_size_bytes: metadata.size_bytes as u64,
            },
        );
    }
    Ok(0)
}

/// Return the current deep size of the session in bytes.
///
/// This walks the session's live cache contents and includes retained session
/// state. It is not an RSS measurement or a configured capacity limit.
/// `out_size_bytes` is written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_session_get_size_bytes(
    session: *const LanceSession,
    out_size_bytes: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { session_get_size_bytes_inner(session, out_size_bytes) },
        neg
    )
}

unsafe fn session_get_size_bytes_inner(
    session: *const LanceSession,
    out_size_bytes: *mut u64,
) -> Result<i32> {
    if session.is_null() || out_size_bytes.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "session and out_size_bytes must not be NULL".into(),
        ));
    }
    let session = unsafe { &*session };
    unsafe {
        std::ptr::write_unaligned(out_size_bytes, session.inner.size_bytes());
    }
    Ok(0)
}

/// Return the approximate number of items in the session.
///
/// This is the native explicitly approximate inspection value. It can differ
/// from the exact cache entry counts and may change as cache maintenance runs.
/// `out_num_items` is written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_session_get_approx_num_items(
    session: *const LanceSession,
    out_num_items: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { session_get_approx_num_items_inner(session, out_num_items) },
        neg
    )
}

unsafe fn session_get_approx_num_items_inner(
    session: *const LanceSession,
    out_num_items: *mut u64,
) -> Result<i32> {
    if session.is_null() || out_num_items.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "session and out_num_items must not be NULL".into(),
        ));
    }
    let session = unsafe { &*session };
    let num_items = u64::try_from(session.inner.approx_num_items()).map_err(|_| {
        lance_core::Error::invalid_input_source(
            "session approx_num_items exceeds u64::MAX on this target".into(),
        )
    })?;
    unsafe {
        std::ptr::write_unaligned(out_num_items, num_items);
    }
    Ok(0)
}

fn u64_to_usize(value: u64, field: &'static str) -> Result<usize> {
    usize::try_from(value).map_err(|_| {
        lance_core::Error::invalid_input_source(
            format!("{field}={value} exceeds usize::MAX on this target").into(),
        )
    })
}
