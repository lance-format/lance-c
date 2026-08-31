// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Reusable bridge from Lance futures to C callback completions.

use std::ffi::c_void;
use std::future::Future;

use futures::FutureExt;

use crate::async_dispatcher::Completion;
use crate::error::{LanceErrorCode, error_code_from_lance, panic_payload_message, swallow_unwind};
use crate::runtime::RT;

/// Spawn a Lance future and translate its terminal state into exactly one C
/// completion.
///
/// `into_result` converts the successful Rust value into an operation-specific
/// C result pointer. `on_panic` lets stateful callers poison or invalidate
/// state before the panic completion is delivered.
pub(crate) fn spawn_lance_future<F, T, S, P>(
    completion: Completion,
    future: F,
    into_result: S,
    on_panic: P,
) where
    F: Future<Output = lance_core::Result<T>> + Send + 'static,
    T: Send + 'static,
    S: FnOnce(T) -> *mut c_void + Send + 'static,
    P: FnOnce() + Send + 'static,
{
    RT.spawn(async move {
        let completion_on_panic = completion;
        let outcome = std::panic::AssertUnwindSafe(async move {
            match future.await {
                Ok(value) => completion.succeed(into_result(value)),
                Err(err) => completion.fail(error_code_from_lance(&err), err.to_string()),
            }
        })
        .catch_unwind()
        .await;

        if let Err(payload) = outcome {
            swallow_unwind("async FFI panic hook", on_panic);
            completion_on_panic.fail(
                LanceErrorCode::Panic,
                format!("panic in FFI call: {}", panic_payload_message(&*payload)),
            );
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::async_dispatcher::LanceCallback;
    use crate::error::{lance_free_string, lance_last_error_code, lance_last_error_message};
    use std::ffi::CStr;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{Arc, mpsc};
    use std::time::Duration;

    #[derive(Debug)]
    struct Observation {
        status: i32,
        result: *mut c_void,
        code: LanceErrorCode,
        message: Option<String>,
    }

    unsafe impl Send for Observation {}

    unsafe extern "C" fn observe(ctx: *mut c_void, status: i32, result: *mut c_void) {
        let tx = unsafe { &*(ctx as *const mpsc::Sender<Observation>) };
        let code = lance_last_error_code();
        let message_ptr = lance_last_error_message();
        let message = if message_ptr.is_null() {
            None
        } else {
            let message = unsafe { CStr::from_ptr(message_ptr) }
                .to_string_lossy()
                .into_owned();
            unsafe { lance_free_string(message_ptr) };
            Some(message)
        };
        tx.send(Observation {
            status,
            result,
            code,
            message,
        })
        .unwrap();
    }

    fn completion() -> (
        Completion,
        mpsc::Receiver<Observation>,
        *mut mpsc::Sender<Observation>,
    ) {
        let (tx, rx) = mpsc::channel();
        let ctx = Box::into_raw(Box::new(tx));
        let callback: LanceCallback = observe;
        let completion = unsafe { Completion::new(callback, ctx.cast()) };
        (completion, rx, ctx)
    }

    fn receive(rx: &mpsc::Receiver<Observation>) -> Observation {
        rx.recv_timeout(Duration::from_secs(5))
            .expect("async bridge must deliver completion")
    }

    #[test]
    fn success_is_converted_and_delivered() {
        let (completion, rx, ctx) = completion();
        spawn_lance_future(
            completion,
            async { Ok::<_, lance_core::Error>(42_u64) },
            |value| Box::into_raw(Box::new(value)).cast(),
            || panic!("success must not invoke on_panic"),
        );

        let observation = receive(&rx);
        assert_eq!(observation.status, 0);
        assert_eq!(observation.code, LanceErrorCode::Ok);
        assert!(observation.message.is_none());
        assert_eq!(
            unsafe { *Box::from_raw(observation.result.cast::<u64>()) },
            42
        );
        unsafe { drop(Box::from_raw(ctx)) };
    }

    #[test]
    fn lance_error_is_delivered_without_running_converter() {
        let (completion, rx, ctx) = completion();
        spawn_lance_future::<_, (), _, _>(
            completion,
            async {
                Err(lance_core::Error::invalid_input_source(
                    "invalid async input".into(),
                ))
            },
            |_| panic!("error must not run success converter"),
            || panic!("ordinary error must not invoke on_panic"),
        );

        let observation = receive(&rx);
        assert_eq!(observation.status, -1);
        assert!(observation.result.is_null());
        assert_eq!(observation.code, LanceErrorCode::InvalidArgument);
        assert!(
            observation
                .message
                .as_deref()
                .is_some_and(|message| message.contains("invalid async input"))
        );
        unsafe { drop(Box::from_raw(ctx)) };
    }

    #[test]
    fn panic_runs_hook_and_delivers_panic_error() {
        let (completion, rx, ctx) = completion();
        let panicked = Arc::new(AtomicBool::new(false));
        let panicked_in_hook = Arc::clone(&panicked);
        spawn_lance_future::<_, (), _, _>(
            completion,
            async {
                panic!("async bridge panic");
                #[allow(unreachable_code)]
                Ok(())
            },
            |_| std::ptr::null_mut(),
            move || panicked_in_hook.store(true, Ordering::SeqCst),
        );

        let observation = receive(&rx);
        assert!(panicked.load(Ordering::SeqCst));
        assert_eq!(observation.status, -1);
        assert_eq!(observation.code, LanceErrorCode::Panic);
        assert!(
            observation
                .message
                .as_deref()
                .is_some_and(|message| message.contains("async bridge panic"))
        );
        unsafe { drop(Box::from_raw(ctx)) };
    }

    #[test]
    fn panicking_hook_does_not_suppress_completion() {
        let (completion, rx, ctx) = completion();
        spawn_lance_future::<_, (), _, _>(
            completion,
            async {
                panic!("original async panic");
                #[allow(unreachable_code)]
                Ok(())
            },
            |_| std::ptr::null_mut(),
            || panic!("panic hook also panicked"),
        );

        let observation = receive(&rx);
        assert_eq!(observation.status, -1);
        assert_eq!(observation.code, LanceErrorCode::Panic);
        assert!(
            observation
                .message
                .as_deref()
                .is_some_and(|message| message.contains("original async panic"))
        );
        unsafe { drop(Box::from_raw(ctx)) };
    }
}
