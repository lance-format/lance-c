// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Shared Lance session C API.

use std::sync::Arc;

use lance::session::Session;
use lance_core::Result;

use crate::error::{ffi_try, swallow_unwind};
use crate::runtime::block_on;

/// Opaque handle for sharing Lance metadata and index caches across datasets.
pub struct LanceSession {
    pub(crate) inner: Arc<Session>,
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

fn u64_to_usize(value: u64, field: &'static str) -> Result<usize> {
    usize::try_from(value).map_err(|_| {
        lance_core::Error::invalid_input_source(
            format!("{field}={value} exceeds usize::MAX on this target").into(),
        )
    })
}
