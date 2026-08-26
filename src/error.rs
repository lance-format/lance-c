// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Thread-local error handling for FFI.
//!
//! After any C function returns an error indicator (NULL pointer or negative int),
//! the caller retrieves the error code and message from thread-local storage.
//!
//! Panic handling (issue #61): every entry point runs its body under
//! `catch_unwind` (via `ffi_try!` or a hand-rolled guard), so a panic in
//! Lance/Arrow code is reported as `LanceErrorCode::Panic` instead of
//! unwinding across the FFI boundary. Scanner handles are poisoned after a
//! panic and reject later calls; dataset handles stay usable because commits
//! are atomic manifest swaps and failed mutations roll back in memory. Void
//! close/free entry points have no error channel, so `swallow_unwind` catches
//! a panicking `Drop`, logs it, and leaks the remainder by design. The limits
//! are honest: double panics, panics in `Drop` during unwind, stack overflow,
//! and allocation failure still abort the process, and post-panic state is
//! best-effort — hosts should fail the query rather than retry a poisoned
//! handle.

use std::cell::RefCell;
use std::ffi::{CString, c_char};
use std::ptr;

/// Error codes returned by `lance_last_error_code()`.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceErrorCode {
    Ok = 0,
    InvalidArgument = 1,
    IoError = 2,
    NotFound = 3,
    DatasetAlreadyExists = 4,
    IndexError = 5,
    Internal = 6,
    NotSupported = 7,
    CommitConflict = 8,
    /// An unexpected panic was caught at the FFI boundary.
    Panic = 9,
}

struct LastError {
    code: LanceErrorCode,
    message: CString,
}

thread_local! {
    static LAST_ERROR: RefCell<Option<LastError>> = const { RefCell::new(None) };
}

pub fn clear_last_error() {
    LAST_ERROR.with(|e| {
        *e.borrow_mut() = None;
    });
}

pub fn set_last_error(code: LanceErrorCode, message: impl AsRef<str>) {
    let message = match CString::new(message.as_ref()) {
        Ok(v) => v,
        Err(_) => CString::new(message.as_ref().replace('\0', "\\0"))
            .unwrap_or_else(|_| CString::new("invalid error message").unwrap()),
    };
    LAST_ERROR.with(|e| {
        *e.borrow_mut() = Some(LastError { code, message });
    });
}

/// Map a `lance_core::Error` to an `LanceErrorCode`.
pub fn error_code_from_lance(err: &lance_core::Error) -> LanceErrorCode {
    use lance_core::Error;
    match err {
        Error::InvalidInput { .. } => LanceErrorCode::InvalidArgument,
        Error::DatasetAlreadyExists { .. } => LanceErrorCode::DatasetAlreadyExists,
        Error::CommitConflict { .. } => LanceErrorCode::CommitConflict,
        Error::DatasetNotFound { .. } | Error::NotFound { .. } | Error::IndexNotFound { .. } => {
            LanceErrorCode::NotFound
        }
        Error::IO { .. } => LanceErrorCode::IoError,
        Error::Index { .. } => LanceErrorCode::IndexError,
        Error::NotSupported { .. } => LanceErrorCode::NotSupported,
        _ => LanceErrorCode::Internal,
    }
}

/// Set the thread-local error from a `lance_core::Error`.
pub fn set_lance_error(err: &lance_core::Error) {
    set_last_error(error_code_from_lance(err), err.to_string());
}

/// Why an [`ffi_guard_with`] invocation failed.
pub(crate) enum FfiFailure {
    /// The guarded body returned a regular `lance_core::Error`.
    Lance,
    /// Something panicked while executing the body or mapping its result.
    Panic,
}

/// Finish a caught FFI panic without leaving panic reporting unguarded.
///
/// A failure while recording the panic or constructing the caller's error
/// value is itself caught. There is no type-safe value we can manufacture if
/// that recovery also panics, so the second payload is resumed; this is the
/// documented double-panic limit of the FFI firewall.
fn recover_from_ffi_panic<T>(
    payload: Box<dyn std::any::Any + Send>,
    recover: impl FnOnce() -> T,
) -> T {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(move || {
        set_last_error(
            LanceErrorCode::Panic,
            format!("panic in FFI call: {}", panic_payload_message(&*payload)),
        );
        recover()
    })) {
        Ok(value) => value,
        Err(payload) => std::panic::resume_unwind(payload),
    }
}

/// Run a complete fallible FFI operation under the panic firewall and map any
/// failure to the ABI-specific return value.
///
/// The guard deliberately includes result mapping, not just `body()`: a
/// wrapped external error may itself panic from `Display` while
/// [`set_lance_error`] formats it. Keeping formatting, TLS mutation, and the
/// error sentinel inside the unwind boundary prevents those secondary panics
/// from escaping an `extern "C"` entry point. If the sentinel itself panics,
/// the recovery path records `LanceErrorCode::Panic` and asks for it once more.
pub(crate) fn ffi_guard_with<T>(
    body: impl FnOnce() -> lance_core::Result<T>,
    mut on_failure: impl FnMut(FfiFailure) -> T,
) -> T {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| match body() {
        Ok(value) => {
            clear_last_error();
            value
        }
        Err(err) => {
            set_lance_error(&err);
            on_failure(FfiFailure::Lance)
        }
    })) {
        Ok(value) => value,
        Err(payload) => recover_from_ffi_panic(payload, || on_failure(FfiFailure::Panic)),
    }
}

/// Extract a human-readable message from a `catch_unwind` panic payload.
///
/// `panic!` only ever produces `&str` or `String` payloads; anything else
/// (e.g. from `std::panic::panic_any`) falls back to a fixed placeholder.
/// NUL bytes are sanitized here once for all consumers: `set_last_error`
/// sanitizes again internally, but other consumers — e.g. Arrow stream error
/// strings built via `CString::new` — must never see a NUL.
///
/// Callers holding the `Box<dyn Any + Send>` from `catch_unwind` must pass
/// `&*payload`, not `&payload`: the latter unsizes the *box itself* into the
/// trait object, and the downcasts then never match.
pub(crate) fn panic_payload_message(payload: &(dyn std::any::Any + Send)) -> String {
    payload
        .downcast_ref::<&str>()
        .map(|s| (*s).to_owned())
        .or_else(|| payload.downcast_ref::<String>().cloned())
        .unwrap_or_else(|| "unknown panic payload".to_owned())
        .replace('\0', "\\0")
}

/// Run `f` under `catch_unwind`, swallowing any panic after logging it.
///
/// For void close/free paths where the caller has no error signal: a panic
/// must not unwind out of an `extern "C"` function (that aborts the host
/// process under Rust 1.81+ semantics), so it is logged and dropped instead.
/// Thread-local error state is deliberately left untouched — a void return
/// gives the caller no channel to observe an error through anyway.
pub(crate) fn swallow_unwind(context: &str, f: impl FnOnce()) {
    if let Err(payload) = std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        log::error!(
            "{context}: swallowed panic during unwinding: {}",
            panic_payload_message(&*payload)
        );
    }
}

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

/// Return the error code from the last failed operation on this thread.
/// Returns `LanceErrorCode::Ok` if no error is pending.
#[unsafe(no_mangle)]
pub extern "C" fn lance_last_error_code() -> LanceErrorCode {
    LAST_ERROR.with(|e| {
        e.borrow()
            .as_ref()
            .map(|v| v.code)
            .unwrap_or(LanceErrorCode::Ok)
    })
}

/// Return the error message from the last failed operation on this thread.
/// The caller must free the returned string with `lance_free_string()`.
/// Returns NULL if no error is pending.
#[unsafe(no_mangle)]
pub extern "C" fn lance_last_error_message() -> *const c_char {
    LAST_ERROR.with(|e| match e.borrow_mut().take() {
        Some(err) => err.message.into_raw() as *const c_char,
        None => ptr::null(),
    })
}

/// Free a string returned by `lance_last_error_message()`.
///
/// Best-effort (issue #61): a panic raised while dropping the string is
/// caught and logged rather than unwinding into the caller, and the
/// allocation may leak.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_free_string(s: *const c_char) {
    if !s.is_null() {
        swallow_unwind("lance_free_string", || unsafe {
            let _ = CString::from_raw(s as *mut c_char);
        });
    }
}

// ---------------------------------------------------------------------------
// Helper macro for FFI functions
// ---------------------------------------------------------------------------

/// Wrap an FFI function body: run it under `catch_unwind`, then on success
/// clear the thread-local error and return the value; on `lance_core::Error`
/// set the thread-local error and return the shape's error value; on panic
/// set a `LanceErrorCode::Panic` error carrying the panic message and return
/// the shape's error value. Shapes: `null` (returns `*mut T`), `neg`
/// (returns a signed integer, `-1` on error), `void` (returns `()`, error
/// observable only via thread-local storage), or a generic `$errval`
/// expression returned verbatim on either failure path (for 0-sentinel
/// integer returns and any other shape the literal arms do not cover).
/// The literal-token arms are listed first so `null`/`neg`/`void` are never
/// captured by the `$errval:expr` catch-all.
macro_rules! ffi_try {
    ($body:expr, null) => {
        $crate::error::ffi_guard_with(|| $body, |_| std::ptr::null_mut())
    };
    ($body:expr, neg) => {
        $crate::error::ffi_guard_with(|| $body, |_| -1)
    };
    ($body:expr, void) => {
        $crate::error::ffi_guard_with(|| $body, |_| ())
    };
    ($body:expr, $errval:expr) => {
        $crate::error::ffi_guard_with(|| $body, |_| $errval)
    };
}

pub(crate) use ffi_try;

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;

    #[derive(Debug)]
    struct PanickingDisplay;

    impl std::fmt::Display for PanickingDisplay {
        fn fmt(&self, _f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            panic!("simulated panic while formatting an FFI error")
        }
    }

    impl std::error::Error for PanickingDisplay {}

    /// Yields a `lance_core::Result<T>` by panicking — the panic is what the
    /// `ffi_try!` shapes under test must catch. (The panic hook prints to
    /// stderr during these tests; that is expected noise.)
    fn panicking<T>() -> lance_core::Result<T> {
        panic!("simulated bug in FFI body")
    }

    /// Take the pending thread-local error message, if any.
    fn take_last_error_message() -> Option<String> {
        let ptr = lance_last_error_message();
        if ptr.is_null() {
            None
        } else {
            let msg = unsafe { CStr::from_ptr(ptr) }
                .to_string_lossy()
                .into_owned();
            unsafe { lance_free_string(ptr) };
            Some(msg)
        }
    }

    #[test]
    fn panic_payload_message_str_payload() {
        let payload: Box<dyn std::any::Any + Send> = Box::new("boom");
        assert_eq!(panic_payload_message(&*payload), "boom");
    }

    #[test]
    fn panic_payload_message_string_payload() {
        let payload: Box<dyn std::any::Any + Send> = Box::new(String::from("boom"));
        assert_eq!(panic_payload_message(&*payload), "boom");
    }

    #[test]
    fn panic_payload_message_non_string_payload_falls_back() {
        let payload: Box<dyn std::any::Any + Send> = Box::new(42i32);
        assert_eq!(panic_payload_message(&*payload), "unknown panic payload");
    }

    #[test]
    fn panic_payload_message_sanitizes_nul() {
        let payload: Box<dyn std::any::Any + Send> = Box::new("bo\0om");
        assert_eq!(panic_payload_message(&*payload), "bo\\0om");
        let payload: Box<dyn std::any::Any + Send> = Box::new(String::from("\0"));
        assert_eq!(panic_payload_message(&*payload), "\\0");
    }

    #[test]
    fn ffi_try_null_maps_panic_to_null_and_panic_code() {
        let ptr: *mut u8 = ffi_try!(panicking(), null);
        assert!(ptr.is_null());
        assert_eq!(lance_last_error_code(), LanceErrorCode::Panic);
        let msg = take_last_error_message().expect("panic must set a message");
        assert!(
            msg.contains("simulated bug in FFI body"),
            "panic payload text must reach the error message, got: {msg}"
        );

        // A succeeding call clears the error.
        let mut value = 7u8;
        let ptr: *mut u8 = ffi_try!(Ok(&mut value as *mut u8), null);
        assert_eq!(ptr, &mut value as *mut u8);
        assert_eq!(lance_last_error_code(), LanceErrorCode::Ok);
        assert!(take_last_error_message().is_none());
    }

    #[test]
    fn ffi_try_null_still_maps_lance_error() {
        // Pre-existing behavior: Err(lance_core::Error) maps via
        // error_code_from_lance, not via the Panic code.
        let ptr: *mut u8 = ffi_try!(
            Err(lance_core::Error::invalid_input_source("bad arg".into())),
            null
        );
        assert!(ptr.is_null());
        assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
        let msg = take_last_error_message().expect("error must set a message");
        assert!(msg.contains("bad arg"), "got: {msg}");
    }

    #[test]
    fn ffi_try_neg_maps_panic_to_minus_one_and_panic_code() {
        let rc: i32 = ffi_try!(panicking(), neg);
        assert_eq!(rc, -1);
        assert_eq!(lance_last_error_code(), LanceErrorCode::Panic);
        let msg = take_last_error_message().expect("panic must set a message");
        assert!(msg.contains("simulated bug in FFI body"), "got: {msg}");

        // A succeeding call clears the error and returns the value.
        let rc: i32 = ffi_try!(Ok(3i32), neg);
        assert_eq!(rc, 3);
        assert_eq!(lance_last_error_code(), LanceErrorCode::Ok);
    }

    #[test]
    fn ffi_try_void_maps_panic_to_panic_code() {
        ffi_try!(panicking::<()>(), void);
        assert_eq!(lance_last_error_code(), LanceErrorCode::Panic);
        let msg = take_last_error_message().expect("panic must set a message");
        assert!(msg.contains("simulated bug in FFI body"), "got: {msg}");

        // A succeeding call clears the error.
        ffi_try!(lance_core::Result::<()>::Ok(()), void);
        assert_eq!(lance_last_error_code(), LanceErrorCode::Ok);
    }

    #[test]
    fn ffi_try_errval_ok_clears_and_returns_value() {
        set_last_error(LanceErrorCode::Internal, "stale error");
        let v: u64 = ffi_try!(Ok(42u64), 0);
        assert_eq!(v, 42);
        assert_eq!(lance_last_error_code(), LanceErrorCode::Ok);
        assert!(take_last_error_message().is_none());
    }

    #[test]
    fn ffi_try_errval_maps_lance_error_to_errval_and_code() {
        let v: u64 = ffi_try!(
            Err(lance_core::Error::invalid_input_source("bad arg".into())),
            0
        );
        assert_eq!(v, 0);
        assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
        let msg = take_last_error_message().expect("error must set a message");
        assert!(msg.contains("bad arg"), "got: {msg}");
    }

    #[test]
    fn ffi_try_catches_panic_while_formatting_lance_error() {
        let v: u64 = ffi_try!(
            Err(lance_core::Error::invalid_input_source(Box::new(
                PanickingDisplay,
            ))),
            0
        );
        assert_eq!(v, 0);
        assert_eq!(lance_last_error_code(), LanceErrorCode::Panic);
        let msg = take_last_error_message().expect("panic must set a message");
        assert!(
            msg.contains("simulated panic while formatting an FFI error"),
            "got: {msg}"
        );
    }

    #[test]
    fn ffi_try_catches_panic_while_building_error_sentinel() {
        let attempts = std::cell::Cell::new(0);
        let v: i64 = ffi_try!(
            Err(lance_core::Error::invalid_input_source("bad arg".into())),
            {
                let attempt = attempts.get();
                attempts.set(attempt + 1);
                if attempt == 0 {
                    panic!("simulated panic while building an FFI error sentinel");
                }
                7
            }
        );

        assert_eq!(v, 7);
        assert_eq!(attempts.get(), 2);
        assert_eq!(lance_last_error_code(), LanceErrorCode::Panic);
        let msg = take_last_error_message().expect("panic must set a message");
        assert!(
            msg.contains("simulated panic while building an FFI error sentinel"),
            "got: {msg}"
        );
    }

    #[test]
    fn ffi_try_errval_maps_panic_to_errval_and_panic_code() {
        // A non-zero sentinel proves the arm returns `$errval` verbatim.
        let v: i64 = ffi_try!(panicking(), 7);
        assert_eq!(v, 7);
        assert_eq!(lance_last_error_code(), LanceErrorCode::Panic);
        let msg = take_last_error_message().expect("panic must set a message");
        assert!(msg.contains("simulated bug in FFI body"), "got: {msg}");

        // The zero sentinel used by the 0-sentinel integer entry points.
        let v: u32 = ffi_try!(panicking(), 0);
        assert_eq!(v, 0);
        assert_eq!(lance_last_error_code(), LanceErrorCode::Panic);
        take_last_error_message();
    }

    #[test]
    fn swallow_unwind_swallows_panic_and_preserves_tls_error() {
        set_last_error(LanceErrorCode::Internal, "marker error");
        swallow_unwind("test context", || panic!("swallowed panic"));
        // Returns normally, and the pre-existing error state is untouched.
        assert_eq!(lance_last_error_code(), LanceErrorCode::Internal);
        let msg = take_last_error_message().expect("marker error must survive");
        assert!(msg.contains("marker error"), "got: {msg}");
        clear_last_error();
    }

    #[test]
    fn swallow_unwind_runs_closure() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        static CALLS: AtomicUsize = AtomicUsize::new(0);
        swallow_unwind("test context", || {
            CALLS.fetch_add(1, Ordering::SeqCst);
        });
        assert_eq!(CALLS.load(Ordering::SeqCst), 1);
    }
}
