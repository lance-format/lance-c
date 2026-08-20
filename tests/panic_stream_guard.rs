// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Regression tests for issue #61 (panic handling across the FFI boundary).
//!
//! Both tests drive the *exact* mechanism a C consumer hits when reading a
//! `lance_scanner_to_arrow_stream` export (lance-io's
//! `RecordBatchIteratorAdaptor::next()` runs `handle.block_on(stream.next())`
//! inside the consumer's `get_next` call):
//!
//! 1. `unguarded_stream_panic_aborts_process` — an unguarded stream that
//!    panics mid-iteration kills the host process even under
//!    `panic = "unwind"`: the panic unwinds out of arrow-rs's
//!    `extern "C" fn get_next`, which aborts (Rust 1.81 semantics).
//!    Runs in a child process so the test suite survives. This deliberately
//!    exports a raw panicking stream — the mechanism must stay so the test
//!    keeps proving the gap is real.
//!
//! 2. `guarded_stream_maps_panic_to_c_stream_error` — the shipped guard
//!    (`lance_c::stream_guard::GuardedStream`, wired into both scanner export
//!    sites in `src/scanner.rs`) catches the same panic in the stream's
//!    `poll_next` and converts it into one `Some(Err(..))` item, which
//!    arrow-rs's exported `get_next` maps to a nonzero return +
//!    `get_last_error`, then end-of-stream. This is exactly the Arrow C
//!    stream error contract, so C consumers need no changes.

use std::ffi::CStr;
use std::pin::Pin;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::task::{Context, Poll};

use arrow::array::{Int32Array, RecordBatch};
use arrow::datatypes::{DataType, Field, Schema, SchemaRef};
use arrow::ffi::FFI_ArrowArray;
use arrow::ffi_stream::FFI_ArrowArrayStream;
use futures::Stream;
use lance_c::stream_guard::GuardedStream;
use lance_io::ffi::to_ffi_arrow_array_stream;
use lance_io::stream::RecordBatchStreamAdapter;

fn test_schema() -> SchemaRef {
    Arc::new(Schema::new(vec![Field::new("x", DataType::Int32, false)]))
}

fn test_batch() -> RecordBatch {
    RecordBatch::try_new(
        test_schema(),
        vec![Arc::new(Int32Array::from(vec![1, 2, 3]))],
    )
    .unwrap()
}

/// A stream that yields one batch, then panics on the second poll —
/// simulating an unwrap/index bug deep in Lance or Arrow scan code.
struct PanicOnSecondPoll {
    yielded: bool,
}

impl Stream for PanicOnSecondPoll {
    type Item = lance_core::Result<RecordBatch>;

    fn poll_next(mut self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        if !self.yielded {
            self.yielded = true;
            Poll::Ready(Some(Ok(test_batch())))
        } else {
            panic!("simulated Lance bug: index out of bounds in scan path");
        }
    }
}

/// Drive the exported stream the way a C consumer does: through the raw
/// `get_next` function pointer, not through any Rust wrapper.
unsafe fn c_get_next(stream: *mut FFI_ArrowArrayStream, array: *mut FFI_ArrowArray) -> i32 {
    let get_next = unsafe { (*stream).get_next }.expect("get_next callback is NULL");
    unsafe { get_next(stream, array) }
}

unsafe fn c_get_last_error(stream: *mut FFI_ArrowArrayStream) -> Option<String> {
    let get_last_error =
        unsafe { (*stream).get_last_error }.expect("get_last_error callback is NULL");
    let ptr = unsafe { get_last_error(stream) };
    if ptr.is_null() {
        None
    } else {
        Some(
            unsafe { CStr::from_ptr(ptr) }
                .to_string_lossy()
                .into_owned(),
        )
    }
}

fn export(
    stream: impl lance_io::stream::RecordBatchStream + Unpin + 'static,
) -> (tokio::runtime::Runtime, FFI_ArrowArrayStream) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let ffi = to_ffi_arrow_array_stream(stream, rt.handle().clone()).unwrap();
    (rt, ffi)
}

/// Child-process entry point: export an *unguarded* panicking stream and read
/// it to the panic. The process is expected to die by SIGABRT.
#[test]
fn unguarded_stream_panic_aborts_process() {
    if std::env::var("POC_CHILD").is_err() {
        // Parent mode: re-run this test in a child process and observe how it dies.
        let exe = std::env::current_exe().unwrap();
        let output = std::process::Command::new(exe)
            .args([
                "unguarded_stream_panic_aborts_process",
                "--exact",
                "--nocapture",
                "--test-threads=1",
            ])
            .env("POC_CHILD", "1")
            .output()
            .unwrap();

        use std::os::unix::process::ExitStatusExt;
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert_eq!(
            output.status.signal(),
            Some(libc::SIGABRT),
            "expected SIGABRT, got status {:?}\nstderr:\n{stderr}",
            output.status
        );
        assert!(
            stderr.contains("panic in a function that cannot unwind"),
            "expected the Rust 1.81 cannot-unwind abort message\nstderr:\n{stderr}"
        );
        return;
    }

    // Child mode: this process is expected to abort on the second get_next.
    let stream = RecordBatchStreamAdapter::new(test_schema(), PanicOnSecondPoll { yielded: false });
    let (_rt, mut ffi) = export(stream);
    let mut array = FFI_ArrowArray::empty();

    let rc = unsafe { c_get_next(&mut ffi, &mut array) };
    assert_eq!(rc, 0, "first batch should be delivered fine");

    // The panic unwinds out of arrow-rs's `extern "C" fn get_next` here.
    unsafe { c_get_next(&mut ffi, &mut array) };
    eprintln!("UNREACHABLE: process should have aborted before this line");
    std::process::exit(1);
}

/// Same panicking stream, but behind the shipped `GuardedStream` guard.
/// Verifies the full Arrow C stream error contract end to end, plus the
/// shared poison flag the guard flips for the owning scanner handle.
#[test]
fn guarded_stream_maps_panic_to_c_stream_error() {
    let scanner_poison = Arc::new(AtomicBool::new(false));
    let stream = RecordBatchStreamAdapter::new(
        test_schema(),
        GuardedStream::new(
            PanicOnSecondPoll { yielded: false },
            Arc::clone(&scanner_poison),
        ),
    );
    let (_rt, mut ffi) = export(stream);

    // 1. First batch arrives normally.
    let mut array = FFI_ArrowArray::empty();
    let rc = unsafe { c_get_next(&mut ffi, &mut array) };
    assert_eq!(rc, 0, "first batch should be delivered fine, rc={rc}");

    // 2. The panic surfaces as a nonzero return, and get_last_error carries
    //    the panic message — the Arrow C stream error contract.
    let rc = unsafe { c_get_next(&mut ffi, &mut array) };
    assert_ne!(rc, 0, "panic item must map to a nonzero return code");
    let msg = unsafe { c_get_last_error(&mut ffi) }.expect("get_last_error returned NULL");
    assert!(
        msg.contains("simulated Lance bug"),
        "panic message should propagate to get_last_error, got: {msg}"
    );

    // 3. The shared poison flag flipped, so the owning scanner handle would
    //    now reject later calls with LANCE_ERR_PANIC.
    assert!(
        scanner_poison.load(Ordering::SeqCst),
        "guard must flip the shared scanner poison flag on panic"
    );

    // 4. After the error the stream is fused: get_next reports end-of-stream
    //    (rc=0 with a released/empty array) instead of panicking again.
    let mut array2 = FFI_ArrowArray::empty();
    let rc = unsafe { c_get_next(&mut ffi, &mut array2) };
    assert_eq!(rc, 0, "fused stream must report end-of-stream, rc={rc}");
    assert!(
        array2.release.is_none(),
        "end-of-stream must yield a released array"
    );
}
