// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Foreign commit-lock bridge.
//!
//! The native blanket `CommitHandler` implementation owns conflict detection,
//! retries, manifest publication, and lease lifetime. This module only maps a
//! synchronous foreign lock/lease pair onto `CommitLock` / `CommitLease`, so
//! callbacks may run on native worker threads and may re-enter the runtime
//! through independent snapshot handles.
//!
//! Calling conventions:
//! - Callbacks must be thread-safe and nonthrowing. `destroy_lease` and
//!   `release_context` must not throw or unwind.
//! - `release(lease, success)` may be called more than once, including during
//!   native cancellation. It does not consume or free the lease token.
//! - `destroy_lease(callback_ctx, lease)` runs exactly once for each
//!   successfully acquired lease, including a NULL stateless token. It is the
//!   only callback that may free token storage.
//! - Native owners (lock handles, datasets, handlers, jobs and leases) retain
//!   the callback context. `release_context` runs exactly once when the final
//!   retained owner drops, not necessarily when `lance_commit_lock_close`
//!   returns.

use std::ffi::c_void;
use std::fmt;
use std::ptr;
use std::sync::Arc;

use async_trait::async_trait;
use lance_core::Error;
use lance_io::object_store::StorageOptionsAccessor;
use lance_table::io::commit::{CommitError, CommitHandler, CommitLease, CommitLock};

use crate::callback::{LanceCallbackError, callback_error, release_message_only};
use crate::error::{ffi_try, swallow_unwind};
use crate::storage_options::LanceStorageOptionsAccessor;

/// Acquire a native version lease.
///
/// The callback starts with `*out_lease == NULL`. On success it may leave the
/// token NULL for a stateless lock. On failure it may store a partially built
/// token; the library destroys that token exactly once without calling
/// `release`.
pub type LanceCommitLockAcquire = Option<
    unsafe extern "C" fn(
        callback_ctx: *mut c_void,
        version: u64,
        out_lease: *mut *mut c_void,
        error: *mut LanceCallbackError,
    ) -> i32,
>;

/// Release a lease for one commit attempt.
///
/// `success` mirrors native `CommitLease::release`. The token stays owned by
/// the library until `destroy_lease`, so this callback must not free it.
pub type LanceCommitLockRelease = Option<
    unsafe extern "C" fn(
        callback_ctx: *mut c_void,
        lease: *mut c_void,
        success: bool,
        error: *mut LanceCallbackError,
    ) -> i32,
>;

/// Destroy one lease token. Called exactly once per acquired lease.
pub type LanceCommitLeaseDestroy =
    Option<unsafe extern "C" fn(callback_ctx: *mut c_void, lease: *mut c_void)>;

/// Release the callback context when the library drops its final owner.
pub type LanceCommitLockContextRelease = Option<unsafe extern "C" fn(callback_ctx: *mut c_void)>;

/// Immutable foreign lock declaration.
///
/// `lock` and `release` are required. `destroy_lease` is optional but is the
/// only way a non-NULL token can be reclaimed; without it the library never
/// frees tokens. `release_context` is likewise optional; when present the
/// library calls it once after the final native owner drops.
///
/// The library copies this table. Ownership of `callback_ctx` transfers only
/// when `lance_commit_lock_new` succeeds.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceCommitLockCallbacks {
    pub lock: LanceCommitLockAcquire,
    pub release: LanceCommitLockRelease,
    pub destroy_lease: LanceCommitLeaseDestroy,
    pub callback_ctx: *mut c_void,
    pub release_context: LanceCommitLockContextRelease,
}

/// Opaque owning handle for a foreign commit lock.
pub struct LanceCommitLock {
    pub(crate) inner: Arc<ForeignCommitLock>,
}

/// Native `CommitLock` adapter. Cloning the inner context into each lease keeps
/// the foreign callback context alive independently of this handler allocation.
pub(crate) struct ForeignCommitLock {
    context: Arc<ForeignCommitLockContext>,
}

struct ForeignCommitLockContext {
    acquire: unsafe extern "C" fn(
        callback_ctx: *mut c_void,
        version: u64,
        out_lease: *mut *mut c_void,
        error: *mut LanceCallbackError,
    ) -> i32,
    release: unsafe extern "C" fn(
        callback_ctx: *mut c_void,
        lease: *mut c_void,
        success: bool,
        error: *mut LanceCallbackError,
    ) -> i32,
    destroy_lease: Option<unsafe extern "C" fn(callback_ctx: *mut c_void, lease: *mut c_void)>,
    callback_ctx: *mut c_void,
    release_context: Option<unsafe extern "C" fn(callback_ctx: *mut c_void)>,
}

// SAFETY: the foreign declaration promises thread-safe callbacks, including
// calls from native runtime workers during conflict handling and cancellation.
unsafe impl Send for ForeignCommitLockContext {}
unsafe impl Sync for ForeignCommitLockContext {}

impl fmt::Debug for ForeignCommitLock {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ForeignCommitLock")
            .field("has_destroy_lease", &self.context.destroy_lease.is_some())
            .field(
                "has_release_context",
                &self.context.release_context.is_some(),
            )
            .finish_non_exhaustive()
    }
}

impl Drop for ForeignCommitLockContext {
    fn drop(&mut self) {
        let Some(release_context) = self.release_context else {
            return;
        };
        let callback_ctx = self.callback_ctx;
        swallow_unwind("commit lock context release", || {
            // SAFETY: the foreign contract requires the callback to remain
            // valid until this final native owner drops.
            unsafe { release_context(callback_ctx) };
        });
    }
}

/// Owned lease token. The token is opaque to the library.
pub(crate) struct ForeignCommitLease {
    context: Arc<ForeignCommitLockContext>,
    /// Opaque foreign token. Stored as an integer so the lease is `Send+Sync`
    /// without a raw-pointer wrapper; the library never dereferences it.
    token: usize,
}

impl Drop for ForeignCommitLease {
    fn drop(&mut self) {
        let Some(destroy_lease) = self.context.destroy_lease else {
            return;
        };
        let callback_ctx = self.context.callback_ctx;
        let token = self.token as *mut c_void;
        swallow_unwind("commit lease destroy", || {
            // SAFETY: `destroy_lease` receives the same token once, after any
            // final `release` callbacks have completed.
            unsafe { destroy_lease(callback_ctx, token) };
        });
    }
}

impl ForeignCommitLockContext {
    fn acquire(self: &Arc<Self>, version: u64) -> Result<ForeignCommitLease, CommitError> {
        let mut out_lease: *mut c_void = ptr::null_mut();
        let mut error = LanceCallbackError::empty();
        // SAFETY: the callback runs synchronously. `block_in_place` leaves the
        // async scheduler so it may perform blocking reads through independent
        // handles and re-enter the runtime on this worker thread.
        let status = tokio::task::block_in_place(|| unsafe {
            (self.acquire)(self.callback_ctx, version, &mut out_lease, &mut error)
        });

        if status != 0 {
            let failure = map_callback_failure(&mut error, status, "commit lock acquire");
            if !out_lease.is_null() {
                if let Some(destroy_lease) = self.destroy_lease {
                    let callback_ctx = self.callback_ctx;
                    swallow_unwind("commit lease partial destroy", || {
                        // SAFETY: the foreign contract promises `out_lease` is
                        // either NULL or a valid token owned by the library.
                        unsafe { destroy_lease(callback_ctx, out_lease) };
                    });
                }
            }
            return Err(failure);
        }

        // A successful acquire must not leak an unexpected error message.
        unsafe { release_message_only(&mut error) };
        Ok(ForeignCommitLease {
            context: Arc::clone(self),
            token: out_lease as usize,
        })
    }

    fn release(&self, token: *mut c_void, success: bool) -> Result<(), CommitError> {
        let mut error = LanceCallbackError::empty();
        // SAFETY: synchronous foreign callback under the same runtime contract
        // as acquire. The token remains valid until `destroy_lease`.
        let status = tokio::task::block_in_place(|| unsafe {
            (self.release)(self.callback_ctx, token, success, &mut error)
        });
        if status != 0 {
            return Err(map_callback_failure(
                &mut error,
                status,
                "commit lock release",
            ));
        }
        unsafe { release_message_only(&mut error) };
        Ok(())
    }
}

fn map_callback_failure(error: &mut LanceCallbackError, status: i32, context: &str) -> CommitError {
    let code = error.code;
    // SAFETY: the callback has returned; the message is library-owned until
    // `callback_error` copies and releases it.
    let native = unsafe { callback_error(error, status, context) };
    if code == 8 {
        CommitError::CommitConflict
    } else {
        CommitError::OtherError(native)
    }
}

#[async_trait]
impl CommitLock for ForeignCommitLock {
    type Lease = ForeignCommitLease;

    async fn lock(&self, version: u64) -> Result<Self::Lease, CommitError> {
        self.context.acquire(version)
    }
}

#[async_trait]
impl CommitLease for ForeignCommitLease {
    async fn release(&self, success: bool) -> Result<(), CommitError> {
        self.context.release(self.token as *mut c_void, success)
    }
}

/// Copy one nullable accessor handle's native owner.
///
/// A NULL handle means native `None`. A non-NULL accessor whose native inner
/// owner is absent also means `None`, matching accessor snapshot semantics.
pub(crate) unsafe fn storage_accessor_from_ffi(
    accessor: *const LanceStorageOptionsAccessor,
) -> Option<Arc<StorageOptionsAccessor>> {
    if accessor.is_null() {
        return None;
    }
    // SAFETY: non-NULL handles are caller-owned and valid for this call.
    unsafe { &*accessor }.inner.clone()
}

/// Clone the native handler behind a nullable lock handle.
pub(crate) fn commit_handler_from_ffi(
    lock: *const LanceCommitLock,
) -> Option<Arc<dyn CommitHandler>> {
    if lock.is_null() {
        return None;
    }
    // SAFETY: non-NULL lock handles are caller-owned and valid for this call.
    Some(unsafe { &*lock }.inner.clone() as Arc<dyn CommitHandler>)
}

/// Create a foreign commit lock.
///
/// The callback table is copied. `callback_ctx` ownership transfers only when
/// this function succeeds; on failure the caller retains it and must not
/// expect `release_context` to run.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_commit_lock_new(
    callbacks: *const LanceCommitLockCallbacks,
) -> *mut LanceCommitLock {
    ffi_try!(unsafe { commit_lock_new_inner(callbacks) }, null)
}

unsafe fn commit_lock_new_inner(
    callbacks: *const LanceCommitLockCallbacks,
) -> Result<*mut LanceCommitLock, Error> {
    if callbacks.is_null() {
        return Err(Error::invalid_input("callbacks must not be NULL"));
    }
    // SAFETY: the caller guarantees a readable table for this call.
    let callbacks = unsafe { &*callbacks };
    let acquire = callbacks
        .lock
        .ok_or_else(|| Error::invalid_input("callbacks.lock must not be NULL"))?;
    let release = callbacks
        .release
        .ok_or_else(|| Error::invalid_input("callbacks.release must not be NULL"))?;

    let context = Arc::new(ForeignCommitLockContext {
        acquire,
        release,
        destroy_lease: callbacks.destroy_lease,
        callback_ctx: callbacks.callback_ctx,
        release_context: callbacks.release_context,
    });
    Ok(Box::into_raw(Box::new(LanceCommitLock {
        inner: Arc::new(ForeignCommitLock { context }),
    })))
}

/// Close a commit-lock handle. Safe to call with NULL.
///
/// Datasets, scanners, handlers, in-flight jobs and leases retain their own
/// native context owners, so they continue to work after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_commit_lock_close(lock: *mut LanceCommitLock) {
    if lock.is_null() {
        return;
    }
    swallow_unwind("lance_commit_lock_close", || {
        // SAFETY: the handle came from `lance_commit_lock_new` and this call
        // consumes it once.
        drop(unsafe { Box::from_raw(lock) });
    });
}
