// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Async callback dispatcher for non-blocking scan operations.
//!
//! Inspired by the Java JNI dispatcher (PR #6102). A dedicated background thread
//! receives completion messages from Tokio tasks and invokes C callbacks
//! sequentially, avoiding reentrancy and Tokio thread blocking.

use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{LazyLock, mpsc};

use crate::error::{LanceErrorCode, clear_last_error, panic_payload_message, set_last_error};

/// C callback function pointer type for async operations.
/// - `ctx`: opaque pointer passed back to the caller
/// - `status`: 0 = success, -1 = error (check `lance_last_error_*`)
/// - `result`: operation-specific result pointer (e.g., `*mut ArrowArrayStream`)
pub type LanceCallback = unsafe extern "C" fn(ctx: *mut c_void, status: i32, result: *mut c_void);

// Safety: LanceCallback is a C function pointer (Send by definition for FFI).
// The ctx pointer is transferred to the dispatcher thread which calls the callback.
unsafe impl Send for DispatcherMessage {}

pub(crate) struct DispatcherMessage {
    pub callback: LanceCallback,
    pub callback_ctx: *mut c_void,
    pub status: i32,
    pub result: *mut c_void,
    /// Error to install on the dispatcher thread's TLS just before invoking
    /// the callback: `Some((code, message))` on failure, `None` on success.
    /// The error must travel inside the message itself (issue #61): an async
    /// operation fails on a Tokio worker thread whose thread-local error the
    /// callback can never observe, because the callback runs here, on the
    /// dispatcher thread.
    pub error: Option<(LanceErrorCode, String)>,
}

struct Dispatcher {
    tx: mpsc::Sender<DispatcherMessage>,
}

impl Dispatcher {
    fn new() -> std::io::Result<Self> {
        let (tx, rx) = mpsc::channel::<DispatcherMessage>();

        std::thread::Builder::new()
            .name("lance-c-dispatcher".to_string())
            .spawn(move || {
                log::debug!("Lance C dispatcher thread started");
                while let Ok(msg) = rx.recv() {
                    deliver_message(msg);
                }
                log::debug!("Lance C dispatcher thread shutting down");
            })?;

        Ok(Self { tx })
    }

    fn send(&self, msg: DispatcherMessage) -> Result<(), DispatcherMessage> {
        self.tx.send(msg).map_err(|err| err.0)
    }
}

/// Install one completion's TLS state and invoke its callback on the current
/// thread. Normally that thread is the dispatcher; this is also the fallback
/// when dispatcher creation or channel delivery fails, preserving the
/// exactly-once completion contract instead of silently dropping the message.
fn deliver_message(msg: DispatcherMessage) {
    match &msg.error {
        Some((code, message)) => set_last_error(*code, message),
        None => clear_last_error(),
    }

    // Best-effort only (issue #61). A real `extern "C"` callback cannot
    // unwind; a panic aborts at its own boundary before this catch runs. The
    // catch only helps Rust hosts that deliberately supply a C-unwind shim.
    let outcome = catch_unwind(AssertUnwindSafe(|| unsafe {
        (msg.callback)(msg.callback_ctx, msg.status, msg.result);
    }));
    if let Err(payload) = outcome {
        log::error!(
            "lance-c dispatcher: unwinding host callback panicked; contained best-effort: {}",
            panic_payload_message(&*payload)
        );
    }
}

fn dispatch_message(dispatcher: Option<&Dispatcher>, msg: DispatcherMessage) {
    let undelivered = match dispatcher {
        Some(dispatcher) => match dispatcher.send(msg) {
            Ok(()) => return,
            Err(msg) => msg,
        },
        None => msg,
    };
    log::error!("lance-c dispatcher unavailable; invoking async completion on the current thread");
    deliver_message(undelivered);
}

static DISPATCHER: LazyLock<Option<Dispatcher>> = LazyLock::new(|| match Dispatcher::new() {
    Ok(dispatcher) => Some(dispatcher),
    Err(err) => {
        log::error!("failed to start lance-c dispatcher thread: {err}");
        None
    }
});

/// Send a completion message to the dispatcher thread. Before invoking the
/// callback, the dispatcher installs `error` on its own thread-local error
/// slot — `Some((code, message))` for failures, `None` (clearing any stale
/// error) for successes — so `lance_last_error_*` called from inside the
/// callback observes the outcome of THIS completion.
pub(crate) fn dispatch_callback(
    callback: LanceCallback,
    callback_ctx: *mut c_void,
    status: i32,
    result: *mut c_void,
    error: Option<(LanceErrorCode, String)>,
) {
    dispatch_message(
        DISPATCHER.as_ref(),
        DispatcherMessage {
            callback,
            callback_ctx,
            status,
            result,
            error,
        },
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::error::{lance_free_string, lance_last_error_code, lance_last_error_message};
    use std::ffi::CStr;
    use std::ptr;
    use std::time::Duration;

    /// What a test callback observed on the dispatcher thread when it fired.
    #[derive(Debug, PartialEq, Eq)]
    struct Observation {
        status: i32,
        result_was_null: bool,
        code: LanceErrorCode,
        message: Option<String>,
    }

    struct CallbackProbe {
        tx: mpsc::Sender<Observation>,
    }

    /// Records the callback arguments plus this thread's TLS error state, and
    /// reports them back over the probe channel. Mirrors exactly what a C
    /// consumer does on `status == -1`: read the code, then take the message.
    unsafe extern "C" fn observe(ctx: *mut c_void, status: i32, result: *mut c_void) {
        let probe = unsafe { &*(ctx as *const CallbackProbe) };
        let code = lance_last_error_code();
        let msg_ptr = lance_last_error_message();
        let message = if msg_ptr.is_null() {
            None
        } else {
            let msg = unsafe { CStr::from_ptr(msg_ptr) }
                .to_string_lossy()
                .into_owned();
            unsafe { lance_free_string(msg_ptr) };
            Some(msg)
        };
        let _ = probe.tx.send(Observation {
            status,
            result_was_null: result.is_null(),
            code,
            message,
        });
    }

    fn probe() -> (mpsc::Receiver<Observation>, *mut c_void) {
        let (tx, rx) = mpsc::channel();
        let ctx = Box::into_raw(Box::new(CallbackProbe { tx })) as *mut c_void;
        (rx, ctx)
    }

    unsafe fn reclaim(ctx: *mut c_void) {
        unsafe {
            drop(Box::from_raw(ctx as *mut CallbackProbe));
        }
    }

    fn recv(rx: &mpsc::Receiver<Observation>) -> Observation {
        rx.recv_timeout(Duration::from_secs(5))
            .expect("dispatcher thread must deliver the callback within 5s")
    }

    #[test]
    fn error_payload_is_installed_on_dispatcher_thread_tls() {
        let (rx, ctx) = probe();
        dispatch_callback(
            observe,
            ctx,
            -1,
            ptr::null_mut(),
            Some((
                LanceErrorCode::InvalidArgument,
                "boom on the worker".to_string(),
            )),
        );

        let obs = recv(&rx);
        assert_eq!(obs.status, -1);
        assert!(obs.result_was_null);
        assert_eq!(
            obs.code,
            LanceErrorCode::InvalidArgument,
            "callback must observe the carried error code on its own thread"
        );
        assert_eq!(obs.message.as_deref(), Some("boom on the worker"));

        unsafe { reclaim(ctx) };
    }

    #[test]
    fn success_clears_stale_error_from_earlier_failed_callback() {
        let (rx, ctx) = probe();
        // A failure followed by a success to the SAME callback: the second
        // invocation must not see the first one's error, even though the
        // dispatcher thread's TLS persists across callbacks.
        dispatch_callback(
            observe,
            ctx,
            -1,
            ptr::null_mut(),
            Some((LanceErrorCode::Internal, "first failure".to_string())),
        );
        dispatch_callback(observe, ctx, 0, ptr::dangling_mut::<c_void>(), None);

        let first = recv(&rx);
        assert_eq!(first.code, LanceErrorCode::Internal);
        assert_eq!(first.message.as_deref(), Some("first failure"));

        let second = recv(&rx);
        assert_eq!(second.status, 0);
        assert!(!second.result_was_null);
        assert_eq!(
            second.code,
            LanceErrorCode::Ok,
            "stale error must be cleared before a successful callback"
        );
        assert_eq!(second.message, None);

        unsafe { reclaim(ctx) };
    }

    #[test]
    fn unavailable_dispatcher_falls_back_without_dropping_completion() {
        let (rx, ctx) = probe();
        dispatch_message(
            None,
            DispatcherMessage {
                callback: observe,
                callback_ctx: ctx,
                status: -1,
                result: ptr::null_mut(),
                error: Some((
                    LanceErrorCode::Internal,
                    "dispatcher unavailable".to_string(),
                )),
            },
        );

        let obs = recv(&rx);
        assert_eq!(obs.status, -1);
        assert_eq!(obs.code, LanceErrorCode::Internal);
        assert_eq!(obs.message.as_deref(), Some("dispatcher unavailable"));
        unsafe { reclaim(ctx) };
    }

    #[test]
    fn closed_dispatch_channel_falls_back_without_dropping_completion() {
        let (tx, dead_rx) = mpsc::channel();
        drop(dead_rx);
        let dispatcher = Dispatcher { tx };
        let (rx, ctx) = probe();

        dispatch_message(
            Some(&dispatcher),
            DispatcherMessage {
                callback: observe,
                callback_ctx: ctx,
                status: 0,
                result: ptr::dangling_mut::<c_void>(),
                error: None,
            },
        );

        let obs = recv(&rx);
        assert_eq!(obs.status, 0);
        assert!(!obs.result_was_null);
        assert_eq!(obs.code, LanceErrorCode::Ok);
        unsafe { reclaim(ctx) };
    }
}
