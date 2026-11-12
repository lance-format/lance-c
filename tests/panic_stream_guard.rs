// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Regression tests for issue #61 (panic handling across the FFI boundary).
//!
//! All tests drive the *exact* mechanism a C consumer hits when reading an
//! exported `ArrowArrayStream`: through the raw `get_next` / `release`
//! function pointers, not through any Rust wrapper. The shipped guard is
//! `lance_c::stream_guard::GuardedReader`, wired into both scanner export
//! sites in `src/scanner.rs`; it drives the stream with
//! `handle.block_on(stream.next())` inside the consumer's `get_next` call,
//! exactly like lance-io's unguarded `RecordBatchIteratorAdaptor` does.
//!
//! 1. `unguarded_stream_panic_aborts_process` — an unguarded stream that
//!    panics mid-iteration kills the host process even under
//!    `panic = "unwind"`: the panic unwinds out of arrow-rs's
//!    `extern "C" fn get_next`, which aborts (Rust 1.81 semantics).
//!    Runs in a child process so the test suite survives. This deliberately
//!    exports a raw panicking stream — the mechanism must stay so the test
//!    keeps proving the gap is real.
//!
//! 2. `guarded_stream_maps_panic_to_c_stream_error` — behind the shipped
//!    guard, the same panic becomes one nonzero `get_next` return +
//!    `get_last_error`, then end-of-stream: the Arrow C stream error
//!    contract, so C consumers need no changes.
//!
//! 3. `guarded_stream_get_next_inside_tokio_runtime_survives` — regression
//!    for the review finding that the previous stream-level guard caught too
//!    late: when the consumer's thread is currently driving a Tokio runtime
//!    (inside `Runtime::block_on` or a spawned task), `Handle::block_on`
//!    panics *before any poll*. The unguarded path died
//!    by SIGABRT here; the reader-level guard must deliver the same error
//!    contract instead. Runs in a child process: the parent asserts the
//!    child exits cleanly AND that tokio's panic really fired (caught).
//!
//! 4. `guarded_stream_release_drop_panic_survives` — regression for the
//!    review finding that the release path was unguarded: arrow-rs's
//!    `release_stream` drops the reader (and the inner stream) inside its
//!    `extern "C"` callback, so a panicking destructor used to abort the
//!    host. The guard's `Drop` detaches the inner stream and contains
//!    cleanup. Runs in a child process, asserting a clean exit AND that
//!    the destructor panic really fired (caught).
//!
//! 5. Regular errors containing NUL are sanitized before arrow-rs formats
//!    them inside `get_next`.
//! 6. A panic from an external error's `Display` is caught, reported as one
//!    terminal stream error, and poisons the scanner.
//! 7. An unexportable schema is rejected while still inside the Rust guard,
//!    before arrow-rs's non-unwinding `get_schema` callback is exposed.

use std::ffi::CStr;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::pin::Pin;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::task::{Context, Poll};

use arrow::array::{Int32Array, RecordBatch};
use arrow::datatypes::{DataType, Field, Schema, SchemaRef};
use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema};
use arrow::ffi_stream::FFI_ArrowArrayStream;
use futures::Stream;
use lance_c::stream_guard::GuardedReader;
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

/// A stream whose cleanup panics — simulating a wedged Lance/Arrow
/// destructor reached from the Arrow C `release` callback.
struct PanicOnDrop;

#[derive(Debug)]
struct PanickingDisplay;

impl std::fmt::Display for PanickingDisplay {
    fn fmt(&self, _f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        panic!("simulated panic while formatting a stream error")
    }
}

impl std::error::Error for PanickingDisplay {}

impl Stream for PanicOnDrop {
    type Item = lance_core::Result<RecordBatch>;

    fn poll_next(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        Poll::Ready(None)
    }
}

impl Drop for PanicOnDrop {
    fn drop(&mut self) {
        panic!("simulated Lance bug: panic in stream cleanup");
    }
}

/// Drive the exported stream the way a C consumer does: through the raw
/// `get_next` function pointer, not through any Rust wrapper.
unsafe fn c_get_next(stream: *mut FFI_ArrowArrayStream, array: *mut FFI_ArrowArray) -> i32 {
    let get_next = unsafe { (*stream).get_next }.expect("get_next callback is NULL");
    unsafe { get_next(stream, array) }
}

unsafe fn c_get_schema(stream: *mut FFI_ArrowArrayStream, schema: *mut FFI_ArrowSchema) -> i32 {
    let get_schema = unsafe { (*stream).get_schema }.expect("get_schema callback is NULL");
    unsafe { get_schema(stream, schema) }
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

/// The unguarded export path (lance-io's adaptor): kept for the child-process
/// test that proves the abort mechanism is real.
fn export(
    stream: impl lance_io::stream::RecordBatchStream + Unpin + 'static,
) -> (tokio::runtime::Runtime, FFI_ArrowArrayStream) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let ffi = to_ffi_arrow_array_stream(stream, rt.handle().clone()).unwrap();
    (rt, ffi)
}

/// The shipped export path, mirroring `scanner_to_arrow_stream_inner` and the
/// async scan task in `src/scanner.rs`: the stream is owned by a
/// [`GuardedReader`] and exported directly via `FFI_ArrowArrayStream::new`.
fn guarded_export(
    stream: impl Stream<Item = lance_core::Result<RecordBatch>> + Unpin + Send + 'static,
    scanner_poison: Arc<AtomicBool>,
) -> (tokio::runtime::Runtime, FFI_ArrowArrayStream) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let reader = GuardedReader::new(stream, test_schema(), rt.handle().clone(), scanner_poison);
    (rt, FFI_ArrowArrayStream::new(Box::new(reader)))
}

/// Re-run the named test in a child process with `env_var` set, returning
/// its output for the parent to assert on.
fn run_child(test_name: &str, env_var: &str) -> std::process::Output {
    let exe = std::env::current_exe().unwrap();
    std::process::Command::new(exe)
        .args([test_name, "--exact", "--nocapture", "--test-threads=1"])
        .env(env_var, "1")
        .output()
        .unwrap()
}

/// Child-process entry point: export an *unguarded* panicking stream and read
/// it to the panic. The process is expected to die by SIGABRT.
#[test]
fn unguarded_stream_panic_aborts_process() {
    if std::env::var("POC_CHILD").is_err() {
        // Parent mode: re-run this test in a child process and observe how it dies.
        let output = run_child("unguarded_stream_panic_aborts_process", "POC_CHILD");

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

/// Same panicking stream, but behind the shipped [`GuardedReader`] guard.
/// Verifies the full Arrow C stream error contract end to end, plus the
/// shared poison flag the guard flips for the owning scanner handle.
#[test]
fn guarded_stream_maps_panic_to_c_stream_error() {
    let scanner_poison = Arc::new(AtomicBool::new(false));
    let (_rt, mut ffi) = guarded_export(
        PanicOnSecondPoll { yielded: false },
        Arc::clone(&scanner_poison),
    );

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

#[test]
fn guarded_stream_sanitizes_nul_in_regular_error() {
    if std::env::var("POC_CHILD_NUL_ERROR").is_err() {
        let output = run_child(
            "guarded_stream_sanitizes_nul_in_regular_error",
            "POC_CHILD_NUL_ERROR",
        );
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(
            output.status.success(),
            "guarded child must exit cleanly, got status {:?}\nstderr:\n{stderr}",
            output.status
        );
        return;
    }

    let scanner_poison = Arc::new(AtomicBool::new(false));
    let stream = futures::stream::iter(vec![Err(lance_core::Error::invalid_input_source(
        "ordinary error with a NUL: bo\0om".into(),
    ))]);
    let (_rt, mut ffi) = guarded_export(stream, Arc::clone(&scanner_poison));
    let mut array = FFI_ArrowArray::empty();

    let rc = unsafe { c_get_next(&mut ffi, &mut array) };
    assert_ne!(rc, 0, "the ordinary stream error must reach Arrow C");
    let msg = unsafe { c_get_last_error(&mut ffi) }.expect("get_last_error returned NULL");
    assert!(msg.contains("bo\\0om"), "NUL must be escaped, got: {msg:?}");
    assert!(
        !scanner_poison.load(Ordering::SeqCst),
        "an ordinary stream error must not poison the scanner"
    );
}

#[test]
fn guarded_stream_catches_panicking_error_display() {
    if std::env::var("POC_CHILD_DISPLAY_ERROR").is_err() {
        let output = run_child(
            "guarded_stream_catches_panicking_error_display",
            "POC_CHILD_DISPLAY_ERROR",
        );
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(
            output.status.success(),
            "guarded child must exit cleanly, got status {:?}\nstderr:\n{stderr}",
            output.status
        );
        assert!(
            stderr.contains("simulated panic while formatting a stream error"),
            "the formatting panic must have fired and been caught\nstderr:\n{stderr}"
        );
        return;
    }

    let scanner_poison = Arc::new(AtomicBool::new(false));
    let stream = futures::stream::iter(vec![Err(lance_core::Error::invalid_input_source(
        Box::new(PanickingDisplay),
    ))]);
    let (_rt, mut ffi) = guarded_export(stream, Arc::clone(&scanner_poison));
    let mut array = FFI_ArrowArray::empty();

    let rc = unsafe { c_get_next(&mut ffi, &mut array) };
    assert_ne!(rc, 0, "the caught panic must reach Arrow C as an error");
    let msg = unsafe { c_get_last_error(&mut ffi) }.expect("get_last_error returned NULL");
    assert!(
        msg.contains("simulated panic while formatting a stream error"),
        "panic message should propagate to get_last_error, got: {msg}"
    );
    assert!(
        scanner_poison.load(Ordering::SeqCst),
        "a formatting panic must poison the owning scanner"
    );
}

#[test]
fn guarded_stream_rejects_nul_schema_before_arrow_callback() {
    if std::env::var("POC_CHILD_NUL_SCHEMA").is_err() {
        let output = run_child(
            "guarded_stream_rejects_nul_schema_before_arrow_callback",
            "POC_CHILD_NUL_SCHEMA",
        );
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(
            output.status.success(),
            "schema validation must fail before Arrow's callback can abort, got status {:?}\nstderr:\n{stderr}",
            output.status
        );
        return;
    }

    let schema = Arc::new(Schema::new(vec![Field::new(
        "field\0name",
        DataType::Int32,
        false,
    )]));
    let rt = tokio::runtime::Runtime::new().unwrap();
    let scanner_poison = Arc::new(AtomicBool::new(false));
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        let reader = GuardedReader::new(
            futures::stream::empty::<lance_core::Result<RecordBatch>>(),
            schema,
            rt.handle().clone(),
            scanner_poison,
        );
        let mut ffi = FFI_ArrowArrayStream::new(Box::new(reader));
        let mut ffi_schema = FFI_ArrowSchema::empty();
        let rc = unsafe { c_get_schema(&mut ffi, &mut ffi_schema) };
        panic!("invalid schema reached Arrow callback and returned rc={rc}");
    }));
    assert!(
        outcome.is_err(),
        "invalid schema must be rejected while the Rust FFI guard can still catch it"
    );
}

/// A `get_next` call made from a thread that is currently driving a Tokio
/// runtime (inside `Runtime::block_on` or a spawned task — a merely
/// `enter()`ed context does not trip tokio's check) makes `Handle::block_on`
/// panic *before any poll* — the previous stream-level guard could not catch
/// that, and the host died by SIGABRT. The reader-level guard must deliver
/// the ordinary stream error contract instead. Child mode exits cleanly only
/// if every assertion holds.
#[test]
fn guarded_stream_get_next_inside_tokio_runtime_survives() {
    if std::env::var("POC_CHILD_RUNTIME").is_err() {
        // Parent mode: the child must exit cleanly (no SIGABRT), and tokio's
        // block_on panic must have really fired — caught by the guard, per
        // the panic hook's stderr output.
        let output = run_child(
            "guarded_stream_get_next_inside_tokio_runtime_survives",
            "POC_CHILD_RUNTIME",
        );
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(
            output.status.success(),
            "guarded child must exit cleanly, got status {:?}\nstderr:\n{stderr}",
            output.status
        );
        assert!(
            stderr.contains("Cannot start a runtime from within a runtime"),
            "tokio's runtime-driving panic must have fired (and been caught)\nstderr:\n{stderr}"
        );
        return;
    }

    // Child mode: call the raw get_next pointer from inside a future running
    // ON the runtime — what a Rust host embedding lance-c would hit.
    let scanner_poison = Arc::new(AtomicBool::new(false));
    let (rt, mut ffi) = guarded_export(
        PanicOnSecondPoll { yielded: false },
        Arc::clone(&scanner_poison),
    );

    // The very first get_next panics in `Handle::block_on`, before the stream
    // is ever polled: the guard maps it to a nonzero return + get_last_error.
    let mut array = FFI_ArrowArray::empty();
    let rc = rt.block_on(async { unsafe { c_get_next(&mut ffi, &mut array) } });
    assert_ne!(
        rc, 0,
        "block_on panic must map to a nonzero return code, rc={rc}"
    );
    let msg = unsafe { c_get_last_error(&mut ffi) }.expect("get_last_error returned NULL");
    assert!(
        msg.contains("runtime"),
        "expected tokio's panic message via get_last_error, got: {msg}"
    );
    assert!(
        scanner_poison.load(Ordering::SeqCst),
        "guard must flip the shared scanner poison flag on a block_on panic"
    );

    // Then the stream is fused: end-of-stream, no second panic.
    let mut array2 = FFI_ArrowArray::empty();
    let rc = unsafe { c_get_next(&mut ffi, &mut array2) };
    assert_eq!(rc, 0, "fused stream must report end-of-stream, rc={rc}");
    assert!(
        array2.release.is_none(),
        "end-of-stream must yield a released array"
    );
}

/// A panicking destructor on the `release` path used to unwind out of
/// arrow-rs's `extern "C" fn release_stream` and abort the host. The guard's
/// `Drop` detaches the inner stream and contains the cleanup panic
/// (best-effort: logged, remainder leaked). Child mode exits cleanly only if
/// `release` returns normally.
#[test]
fn guarded_stream_release_drop_panic_survives() {
    if std::env::var("POC_CHILD_RELEASE").is_err() {
        // Parent mode: the child must exit cleanly (no SIGABRT), and the
        // destructor panic must have really fired — caught by the guard, per
        // the panic hook's stderr output.
        let output = run_child(
            "guarded_stream_release_drop_panic_survives",
            "POC_CHILD_RELEASE",
        );
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(
            output.status.success(),
            "guarded child must exit cleanly, got status {:?}\nstderr:\n{stderr}",
            output.status
        );
        assert!(
            stderr.contains("simulated Lance bug: panic in stream cleanup"),
            "the destructor panic must have fired (and been caught)\nstderr:\n{stderr}"
        );
        return;
    }

    // Child mode: invoke the raw release callback the way a C consumer does.
    let scanner_poison = Arc::new(AtomicBool::new(false));
    let (_rt, mut ffi) = guarded_export(PanicOnDrop, Arc::clone(&scanner_poison));
    let release = ffi.release.expect("release callback is NULL");
    unsafe { release(&mut ffi) };

    assert!(
        ffi.release.is_none(),
        "release must be one-shot per the Arrow C stream contract"
    );
    assert!(
        !scanner_poison.load(Ordering::SeqCst),
        "cleanup panic is best-effort and must not poison the handle"
    );
}
