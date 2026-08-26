// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Panic guard for exported Arrow streams (issue #61).
//!
//! An exported `FFI_ArrowArrayStream` outlives the call that created it: the
//! consumer's later `get_next` / `release` calls re-enter our code on the
//! *consumer's* thread, through arrow-rs's `extern "C"` callbacks, and
//! neither arrow-rs nor lance-io guards those calls. The guard therefore
//! sits at the outermost Rust edge the C consumer can reach — the
//! [`RecordBatchReader`] that arrow-rs's callbacks invoke — not inside the
//! stream:
//!
//! - **`get_next` → `Iterator::next`**: the catch wraps the *complete*
//!   `handle.block_on(stream.next())` operation. Catching inside the
//!   stream's `poll_next` would be too late: `Handle::block_on` itself
//!   panics before any poll when the consumer's thread is currently driving
//!   a Tokio runtime ("Cannot start a runtime from within a runtime"), and
//!   that panic would unwind out of arrow-rs's `extern "C" fn get_next` and
//!   abort the host. A caught panic becomes exactly one terminal
//!   `Some(Err(..))` item — which arrow-rs's exported `get_next` maps to a
//!   nonzero return plus `get_last_error` — followed by end-of-stream, and
//!   flips the shared `scanner_poison` flag so the owning scanner handle
//!   rejects later calls with `LANCE_ERR_PANIC`.
//!
//! - **`release` → `Drop`**: arrow-rs's `release_stream` drops this reader
//!   (and with it the inner Lance stream) inside its `extern "C"` callback,
//!   so [`GuardedReader::drop`] detaches the inner stream and drops it under
//!   `catch_unwind`, accepting a leak of the remainder per the documented
//!   best-effort close/free policy in `lance.h`. A cleanup panic is logged,
//!   not poisoned — the handle's own state was never touched.
//!
//! The panic message is sanitized by `panic_payload_message` (NUL bytes
//! replaced) before it is baked into the error string: arrow-rs's `get_next`
//! runs `CString::new` on this string, and an embedded NUL would panic right
//! through the guard.
//!
//! Upstream note: this reader is a candidate for promotion into
//! `lance_io::ffi::to_ffi_arrow_array_stream` itself, which would extend the
//! same protection to every consumer of that export path (e.g. lance-java).

use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi_stream::FFI_ArrowArrayStream;
use arrow::record_batch::RecordBatchReader;
use arrow_array::RecordBatch;
use arrow_schema::{ArrowError, SchemaRef};
use futures::{Stream, StreamExt};

use crate::error::{panic_payload_message, swallow_unwind};

/// An owned, NUL-free error whose `Display` implementation cannot call back
/// into an arbitrary external error source. Arrow formats this value from
/// inside its non-unwinding `get_next` callback.
#[derive(Debug)]
struct FfiSafeStreamError(String);

impl std::fmt::Display for FfiSafeStreamError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for FfiSafeStreamError {}

fn ffi_safe_stream_error(message: String) -> ArrowError {
    ArrowError::ExternalError(Box::new(FfiSafeStreamError(message.replace('\0', "\\0"))))
}

/// Exercise arrow-rs's exact schema conversion before its non-unwinding
/// `get_schema` callback is exposed to C.
fn preflight_schema(schema: &SchemaRef) -> std::result::Result<(), ArrowError> {
    match catch_unwind(AssertUnwindSafe(|| {
        let ffi_schema = FFI_ArrowSchema::try_from(schema.as_ref()).map_err(|err| {
            // Detach the error under the guard for the same reason `next`
            // does: this value may ultimately be formatted by an FFI caller.
            ffi_safe_stream_error(err.to_string())
        })?;
        drop(ffi_schema);
        Ok(())
    })) {
        Ok(result) => result,
        Err(payload) => Err(ffi_safe_stream_error(format!(
            "panic exporting Arrow schema: {}",
            panic_payload_message(&*payload)
        ))),
    }
}

/// A [`RecordBatchReader`] that owns the exported Lance stream, drives it
/// with a Tokio runtime handle, and contains panics at both C-reachable
/// edges (`next` and `drop`) — see the module docs for why the guard lives
/// at this level. Construct via [`GuardedReader::new`] and export with
/// `FFI_ArrowArrayStream::new(Box::new(reader))`.
pub struct GuardedReader<S> {
    /// An `Option` solely so [`GuardedReader::drop`] can detach ownership
    /// before running cleanup under `catch_unwind`; always `Some` outside
    /// the destructor.
    inner: Option<S>,
    schema: SchemaRef,
    handle: tokio::runtime::Handle,
    /// Fused state: set when a panic is caught, after which every `next`
    /// yields `None` (end-of-stream) without touching the inner stream again.
    poisoned: bool,
    /// Shared with the owning scanner handle; flipped when a panic is caught
    /// so the handle rejects later calls with `LANCE_ERR_PANIC`.
    scanner_poison: Arc<AtomicBool>,
}

impl<S> GuardedReader<S> {
    /// Wrap `inner`, driving it with `handle` and wiring the shared
    /// `scanner_poison` flag that a caught panic sets (from
    /// `LanceScanner::poison_flag()` at the export sites).
    ///
    /// # Panics
    ///
    /// Panics if `schema` cannot be converted to the Arrow C Data Interface.
    /// Production callers construct this reader inside their outer FFI panic
    /// guard, before arrow-rs's non-unwinding `get_schema` callback is exposed.
    pub fn new(
        inner: S,
        schema: SchemaRef,
        handle: tokio::runtime::Handle,
        scanner_poison: Arc<AtomicBool>,
    ) -> Self {
        // arrow-rs converts this schema later from inside its non-unwinding
        // `get_schema` callback. Perform the exact conversion once while the
        // scanner export's outer catch_unwind is still active, so a malformed
        // schema (for example, a field name containing NUL) cannot first
        // panic after control has crossed into that callback.
        preflight_schema(&schema)
            .unwrap_or_else(|err| panic!("Arrow schema cannot be exported: {err}"));

        Self {
            inner: Some(inner),
            schema,
            handle,
            poisoned: false,
            scanner_poison,
        }
    }
}

impl<S> Iterator for GuardedReader<S>
where
    S: Stream<Item = lance_core::Result<RecordBatch>> + Unpin,
{
    type Item = std::result::Result<RecordBatch, ArrowError>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.poisoned {
            return None;
        }
        let Self {
            inner,
            handle,
            poisoned,
            scanner_poison,
            ..
        } = self;
        // `None` only while `Drop` is running, when `next` can no longer be
        // called; arrow-rs never invokes `get_next` after `release`.
        let inner = inner.as_mut()?;
        // The catch covers the WHOLE block_on + poll operation: a panic in
        // `Handle::block_on` (runtime-driving consumer thread) or in the
        // stream's `poll_next` lands here, one frame below arrow-rs's
        // `extern "C"` callback, so neither can unwind across the FFI
        // boundary.
        let polled = catch_unwind(AssertUnwindSafe(|| {
            match handle.block_on(inner.next()) {
                Some(Ok(batch)) => Some(Ok(batch)),
                Some(Err(err)) => {
                    // Format and detach the arbitrary Lance error while still
                    // inside the guard. arrow-rs later calls Display and
                    // CString::new from a non-unwinding callback, so neither a
                    // panicking source nor an embedded NUL may reach it.
                    Some(Err(ffi_safe_stream_error(err.to_string())))
                }
                None => None,
            }
        }));
        match polled {
            Ok(item) => item,
            Err(payload) => {
                *poisoned = true;
                scanner_poison.store(true, Ordering::SeqCst);
                Some(Err(ffi_safe_stream_error(format!(
                    "panic in stream: {}",
                    panic_payload_message(&*payload)
                ))))
            }
        }
    }
}

impl<S> RecordBatchReader for GuardedReader<S>
where
    S: Stream<Item = lance_core::Result<RecordBatch>> + Unpin + Send,
{
    fn schema(&self) -> SchemaRef {
        self.schema.clone()
    }
}

impl<S> Drop for GuardedReader<S> {
    fn drop(&mut self) {
        let Some(inner) = self.inner.take() else {
            return;
        };
        // arrow-rs's `release_stream` runs this drop inside its `extern "C"`
        // callback: a panic unwinding out of here would abort the host
        // process. Ownership was detached above, so a contained cleanup panic
        // can only leak the remainder — the documented best-effort policy.
        swallow_unwind("GuardedReader::drop (ArrowArrayStream release)", || {
            drop(inner)
        });
    }
}

/// A panic-safe owner for an already-materialized [`RecordBatchReader`].
///
/// Dataset `take` operations use readers whose batches are already in memory,
/// so no Tokio handle is needed. Arrow still invokes `schema`, `next`, and
/// `drop` later from non-unwinding C callbacks, however, which requires the
/// same error-detachment and cleanup containment as [`GuardedReader`].
struct GuardedRecordBatchReader<R> {
    inner: Option<R>,
    schema: SchemaRef,
    poisoned: bool,
}

impl<R> Iterator for GuardedRecordBatchReader<R>
where
    R: RecordBatchReader,
{
    type Item = std::result::Result<RecordBatch, ArrowError>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.poisoned {
            return None;
        }

        let inner = self.inner.as_mut()?;
        let next = catch_unwind(AssertUnwindSafe(|| match inner.next() {
            Some(Ok(batch)) => Some(Ok(batch)),
            Some(Err(err)) => Some(Err(ffi_safe_stream_error(err.to_string()))),
            None => None,
        }));

        match next {
            Ok(item) => item,
            Err(payload) => {
                self.poisoned = true;
                Some(Err(ffi_safe_stream_error(format!(
                    "panic in record batch reader: {}",
                    panic_payload_message(&*payload)
                ))))
            }
        }
    }
}

impl<R> RecordBatchReader for GuardedRecordBatchReader<R>
where
    R: RecordBatchReader + Send,
{
    fn schema(&self) -> SchemaRef {
        Arc::clone(&self.schema)
    }
}

impl<R> Drop for GuardedRecordBatchReader<R> {
    fn drop(&mut self) {
        let Some(inner) = self.inner.take() else {
            return;
        };
        swallow_unwind(
            "GuardedRecordBatchReader::drop (ArrowArrayStream release)",
            || drop(inner),
        );
    }
}

/// Export an already-materialized reader through panic-safe Arrow C stream
/// callbacks.
///
/// The schema is converted once before the callback table is returned. This
/// turns deterministic schema conversion failures into an ordinary export
/// failure (or lets the caller's outer FFI guard catch an arrow-rs conversion
/// panic) instead of deferring them to `get_schema`.
pub(crate) fn guarded_ffi_stream_from_reader<R>(
    reader: R,
) -> std::result::Result<FFI_ArrowArrayStream, ArrowError>
where
    R: RecordBatchReader + Send + 'static,
{
    let schema = reader.schema();
    preflight_schema(&schema)?;
    let reader = GuardedRecordBatchReader {
        inner: Some(reader),
        schema,
        poisoned: false,
    };
    Ok(FFI_ArrowArrayStream::new(Box::new(reader)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use arrow::ffi::FFI_ArrowArray;
    use arrow::ffi_stream::FFI_ArrowArrayStream;
    use arrow_array::Int32Array;
    use arrow_schema::{DataType, Field, Schema};
    use std::ffi::CStr;
    use std::pin::Pin;
    use std::task::{Context, Poll};

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

    /// A stream that yields one batch, then panics with `message` —
    /// simulating an unwrap/index bug deep in Lance or Arrow scan code. (The
    /// panic hook prints to stderr during these tests; that is expected
    /// noise.)
    struct PanicOnSecondPoll {
        yielded: bool,
        message: &'static str,
    }

    #[derive(Debug)]
    struct PanickingDisplay;

    impl std::fmt::Display for PanickingDisplay {
        fn fmt(&self, _f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            panic!("simulated panic while formatting a reader error")
        }
    }

    impl std::error::Error for PanickingDisplay {}

    impl Stream for PanicOnSecondPoll {
        type Item = lance_core::Result<RecordBatch>;

        fn poll_next(mut self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
            if !self.yielded {
                self.yielded = true;
                Poll::Ready(Some(Ok(test_batch())))
            } else {
                panic!("{}", self.message)
            }
        }
    }

    fn reader_for<S>(
        stream: S,
        scanner_poison: Arc<AtomicBool>,
    ) -> (tokio::runtime::Runtime, GuardedReader<S>)
    where
        S: Stream<Item = lance_core::Result<RecordBatch>> + Unpin,
    {
        let rt = tokio::runtime::Runtime::new().unwrap();
        let reader = GuardedReader::new(stream, test_schema(), rt.handle().clone(), scanner_poison);
        (rt, reader)
    }

    fn guarded_export<S>(
        stream: S,
        schema: SchemaRef,
        scanner_poison: Arc<AtomicBool>,
    ) -> (tokio::runtime::Runtime, FFI_ArrowArrayStream)
    where
        S: Stream<Item = lance_core::Result<RecordBatch>> + Unpin + Send + 'static,
    {
        let rt = tokio::runtime::Runtime::new().unwrap();
        let reader = GuardedReader::new(stream, schema, rt.handle().clone(), scanner_poison);
        (rt, FFI_ArrowArrayStream::new(Box::new(reader)))
    }

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
        let message = unsafe { get_last_error(stream) };
        if message.is_null() {
            None
        } else {
            Some(
                unsafe { CStr::from_ptr(message) }
                    .to_string_lossy()
                    .into_owned(),
            )
        }
    }

    fn run_child(test_name: &str, environment_variable: &str) -> std::process::Output {
        let exact_name = format!("stream_guard::tests::{test_name}");
        std::process::Command::new(std::env::current_exe().unwrap())
            .args([&exact_name, "--exact", "--nocapture", "--test-threads=1"])
            .env(environment_variable, "1")
            .output()
            .unwrap()
    }

    fn assert_child_succeeds(test_name: &str, environment_variable: &str) -> String {
        let output = run_child(test_name, environment_variable);
        let stderr = String::from_utf8_lossy(&output.stderr).into_owned();
        assert!(
            output.status.success(),
            "guarded child must exit cleanly, got status {:?}\nstderr:\n{stderr}",
            output.status
        );
        stderr
    }

    fn raw_error_then_eos(stream: &mut FFI_ArrowArrayStream) -> String {
        let mut array = FFI_ArrowArray::empty();
        let status = unsafe { c_get_next(stream, &mut array) };
        assert_ne!(status, 0, "expected an Arrow C stream error");
        let message = unsafe { c_get_last_error(stream) }.expect("get_last_error returned NULL");

        let mut eos = FFI_ArrowArray::empty();
        assert_eq!(unsafe { c_get_next(stream, &mut eos) }, 0);
        assert!(eos.release.is_none(), "error must be followed by EOS");
        message
    }

    #[test]
    fn panic_yields_one_error_then_fuses_and_flips_flag() {
        let scanner_poison = Arc::new(AtomicBool::new(false));
        let (_rt, mut reader) = reader_for(
            PanicOnSecondPoll {
                yielded: false,
                message: "simulated scan bug",
            },
            Arc::clone(&scanner_poison),
        );

        // The pre-panic item passes through untouched.
        let batch = reader
            .next()
            .expect("stream ended before first batch")
            .expect("first batch must pass through as Ok");
        assert_eq!(batch.num_rows(), 3);

        // The panic becomes exactly one terminal Err item carrying the
        // sanitized payload, and the shared flag flips.
        let err = reader
            .next()
            .expect("panic item missing")
            .expect_err("panic must surface as an Err item");
        let msg = err.to_string();
        assert!(
            msg.contains("simulated scan bug"),
            "panic payload must reach the error message, got: {msg}"
        );
        assert!(
            !msg.contains('\0'),
            "error string must be NUL-free, got: {msg:?}"
        );
        assert!(
            scanner_poison.load(Ordering::SeqCst),
            "scanner poison flag must flip on panic"
        );

        // Then the reader is fused: None forever, no repeat panic, no second
        // error item.
        assert!(reader.next().is_none(), "fused reader must yield None");
        assert!(reader.next().is_none(), "fused reader must stay fused");
    }

    #[test]
    fn panic_payload_with_nul_is_sanitized() {
        let scanner_poison = Arc::new(AtomicBool::new(false));
        let (_rt, mut reader) = reader_for(
            PanicOnSecondPoll {
                yielded: false,
                message: "bo\0om",
            },
            scanner_poison,
        );

        let _ = reader.next(); // consume the good batch
        let err = reader
            .next()
            .expect("panic item missing")
            .expect_err("panic must surface as an Err item");
        let msg = err.to_string();
        assert!(
            !msg.contains('\0'),
            "embedded NUL must be sanitized, got: {msg:?}"
        );
        assert!(
            msg.contains("bo\\0om"),
            "NUL must render as the 2-char escape, got: {msg:?}"
        );
    }

    #[test]
    fn no_panic_passes_items_through_and_flag_stays_clear() {
        let scanner_poison = Arc::new(AtomicBool::new(false));
        let inner = futures::stream::iter(vec![Ok(test_batch()), Ok(test_batch())]);
        let (_rt, mut reader) = reader_for(inner, Arc::clone(&scanner_poison));

        for i in 0..2 {
            let batch = reader
                .next()
                .expect("stream ended early")
                .expect("batch must pass through untouched");
            assert_eq!(batch.num_rows(), 3, "batch {i} contents changed");
        }
        assert!(
            reader.next().is_none(),
            "inner end-of-stream must pass through"
        );
        assert!(
            !scanner_poison.load(Ordering::SeqCst),
            "flag must stay false without a panic"
        );
    }

    /// Regression for the review finding that a stream-level guard catches
    /// too late: `Handle::block_on` panics *before any poll* when the calling
    /// thread is currently driving a Tokio runtime (inside `Runtime::block_on`
    /// or a spawned task — a merely `enter()`ed context does not trip tokio's
    /// check). The reader-level catch must turn that into the same
    /// terminal-error + fuse + poison contract instead of letting it unwind
    /// out of arrow-rs's `extern "C" fn get_next`.
    #[test]
    fn block_on_panic_on_runtime_driving_thread_is_contained() {
        let scanner_poison = Arc::new(AtomicBool::new(false));
        let (rt, mut reader) = reader_for(
            PanicOnSecondPoll {
                yielded: false,
                message: "unreachable: block_on panics before any poll",
            },
            Arc::clone(&scanner_poison),
        );

        // Drive `next` from inside a future running ON the runtime: this is
        // the consumer context in which tokio's `Handle::block_on` panics.
        let reader_ref = &mut reader;
        let err = rt
            .block_on(async move { reader_ref.next() })
            .expect("panic item missing")
            .expect_err("block_on panic must surface as an Err item");
        let msg = err.to_string();
        assert!(
            msg.contains("runtime"),
            "expected tokio's runtime-driving panic message, got: {msg}"
        );
        assert!(
            scanner_poison.load(Ordering::SeqCst),
            "scanner poison flag must flip on a block_on panic"
        );
        // Back outside the runtime context the guard stays fused without
        // touching the inner stream (or block_on) again.
        assert!(reader.next().is_none(), "guard must fuse after the panic");
    }

    /// A stream whose cleanup panics — simulating a wedged Lance/Arrow
    /// destructor on the `release` path.
    struct PanicOnDrop;

    impl Stream for PanicOnDrop {
        type Item = lance_core::Result<RecordBatch>;

        fn poll_next(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
            Poll::Ready(None)
        }
    }

    impl Drop for PanicOnDrop {
        fn drop(&mut self) {
            panic!("simulated drop bug in stream cleanup");
        }
    }

    struct PanicOnReaderNext {
        schema: SchemaRef,
    }

    impl Iterator for PanicOnReaderNext {
        type Item = std::result::Result<RecordBatch, ArrowError>;

        fn next(&mut self) -> Option<Self::Item> {
            panic!("simulated panic in materialized reader next");
        }
    }

    impl RecordBatchReader for PanicOnReaderNext {
        fn schema(&self) -> SchemaRef {
            Arc::clone(&self.schema)
        }
    }

    struct PanicOnReaderDrop {
        schema: SchemaRef,
    }

    impl Iterator for PanicOnReaderDrop {
        type Item = std::result::Result<RecordBatch, ArrowError>;

        fn next(&mut self) -> Option<Self::Item> {
            None
        }
    }

    impl RecordBatchReader for PanicOnReaderDrop {
        fn schema(&self) -> SchemaRef {
            Arc::clone(&self.schema)
        }
    }

    impl Drop for PanicOnReaderDrop {
        fn drop(&mut self) {
            panic!("simulated panic in materialized reader drop");
        }
    }

    /// Regression for the review finding that the release path was unguarded:
    /// arrow-rs's `release_stream` drops this reader inside its `extern "C"`
    /// callback, so a cleanup panic must be contained here (best-effort:
    /// logged, remainder leaked) rather than unwinding out and aborting the
    /// host. Cleanup failure does not poison the scanner handle.
    #[test]
    fn drop_panic_is_contained_without_poisoning() {
        let scanner_poison = Arc::new(AtomicBool::new(false));
        let (_rt, reader) = reader_for(PanicOnDrop, Arc::clone(&scanner_poison));

        // Must not unwind out: if it did, this test process would abort.
        drop(reader);

        assert!(
            !scanner_poison.load(Ordering::SeqCst),
            "cleanup panic is best-effort and must not poison the handle"
        );
    }

    #[test]
    fn raw_stream_get_next_contains_poll_panic() {
        const CHILD: &str = "LANCE_C_CHILD_STREAM_POLL_PANIC";
        if std::env::var(CHILD).is_err() {
            let stderr = assert_child_succeeds("raw_stream_get_next_contains_poll_panic", CHILD);
            assert!(stderr.contains("simulated raw poll panic"));
            return;
        }

        let scanner_poison = Arc::new(AtomicBool::new(false));
        let (_runtime, mut stream) = guarded_export(
            PanicOnSecondPoll {
                yielded: false,
                message: "simulated raw poll panic",
            },
            test_schema(),
            Arc::clone(&scanner_poison),
        );

        let mut first = FFI_ArrowArray::empty();
        assert_eq!(unsafe { c_get_next(&mut stream, &mut first) }, 0);
        assert!(first.release.is_some());
        unsafe { first.release.unwrap()(&mut first) };

        let message = raw_error_then_eos(&mut stream);
        assert!(
            message.contains("simulated raw poll panic"),
            "got: {message}"
        );
        assert!(scanner_poison.load(Ordering::SeqCst));
    }

    #[test]
    fn raw_stream_get_next_sanitizes_regular_error() {
        const CHILD: &str = "LANCE_C_CHILD_STREAM_NUL_ERROR";
        if std::env::var(CHILD).is_err() {
            assert_child_succeeds("raw_stream_get_next_sanitizes_regular_error", CHILD);
            return;
        }

        let scanner_poison = Arc::new(AtomicBool::new(false));
        let stream = futures::stream::iter(vec![Err(lance_core::Error::invalid_input_source(
            "ordinary error with a NUL: bo\0om".into(),
        ))]);
        let (_runtime, mut stream) =
            guarded_export(stream, test_schema(), Arc::clone(&scanner_poison));

        let message = raw_error_then_eos(&mut stream);
        assert!(message.contains("bo\\0om"), "got: {message:?}");
        assert!(!message.contains('\0'));
        assert!(!scanner_poison.load(Ordering::SeqCst));
    }

    #[test]
    fn raw_stream_get_next_contains_error_display_panic() {
        const CHILD: &str = "LANCE_C_CHILD_STREAM_DISPLAY_PANIC";
        if std::env::var(CHILD).is_err() {
            let stderr =
                assert_child_succeeds("raw_stream_get_next_contains_error_display_panic", CHILD);
            assert!(stderr.contains("simulated panic while formatting a reader error"));
            return;
        }

        let scanner_poison = Arc::new(AtomicBool::new(false));
        let stream = futures::stream::iter(vec![Err(lance_core::Error::invalid_input_source(
            Box::new(PanickingDisplay),
        ))]);
        let (_runtime, mut stream) =
            guarded_export(stream, test_schema(), Arc::clone(&scanner_poison));

        let message = raw_error_then_eos(&mut stream);
        assert!(
            message.contains("simulated panic while formatting a reader error"),
            "got: {message}"
        );
        assert!(scanner_poison.load(Ordering::SeqCst));
    }

    #[test]
    fn stream_schema_is_rejected_before_raw_get_schema_is_exposed() {
        const CHILD: &str = "LANCE_C_CHILD_STREAM_NUL_SCHEMA";
        if std::env::var(CHILD).is_err() {
            assert_child_succeeds(
                "stream_schema_is_rejected_before_raw_get_schema_is_exposed",
                CHILD,
            );
            return;
        }

        let schema = Arc::new(Schema::new(vec![Field::new(
            "field\0name",
            DataType::Int32,
            false,
        )]));
        let runtime = tokio::runtime::Runtime::new().unwrap();
        let callback_was_exposed = std::cell::Cell::new(false);
        let outcome = catch_unwind(AssertUnwindSafe(|| {
            let reader = GuardedReader::new(
                futures::stream::empty::<lance_core::Result<RecordBatch>>(),
                schema,
                runtime.handle().clone(),
                Arc::new(AtomicBool::new(false)),
            );
            callback_was_exposed.set(true);
            let mut stream = FFI_ArrowArrayStream::new(Box::new(reader));
            let mut ffi_schema = FFI_ArrowSchema::empty();
            unsafe { c_get_schema(&mut stream, &mut ffi_schema) }
        }));

        assert!(outcome.is_err(), "invalid schema must fail during export");
        assert!(
            !callback_was_exposed.get(),
            "invalid schema reached raw get_schema"
        );
    }

    #[test]
    fn raw_stream_get_next_inside_tokio_runtime_is_contained() {
        const CHILD: &str = "LANCE_C_CHILD_STREAM_NESTED_RUNTIME";
        if std::env::var(CHILD).is_err() {
            let stderr = assert_child_succeeds(
                "raw_stream_get_next_inside_tokio_runtime_is_contained",
                CHILD,
            );
            assert!(stderr.contains("Cannot start a runtime from within a runtime"));
            return;
        }

        let scanner_poison = Arc::new(AtomicBool::new(false));
        let (runtime, mut stream) = guarded_export(
            futures::stream::iter(vec![Ok(test_batch())]),
            test_schema(),
            Arc::clone(&scanner_poison),
        );
        let mut array = FFI_ArrowArray::empty();
        let status = runtime.block_on(async { unsafe { c_get_next(&mut stream, &mut array) } });
        assert_ne!(status, 0);
        let message = unsafe { c_get_last_error(&mut stream) }.unwrap();
        assert!(message.contains("runtime"), "got: {message}");
        assert!(scanner_poison.load(Ordering::SeqCst));

        let mut eos = FFI_ArrowArray::empty();
        assert_eq!(unsafe { c_get_next(&mut stream, &mut eos) }, 0);
        assert!(eos.release.is_none());
    }

    #[test]
    fn raw_stream_release_contains_drop_panic() {
        const CHILD: &str = "LANCE_C_CHILD_STREAM_DROP_PANIC";
        if std::env::var(CHILD).is_err() {
            let stderr = assert_child_succeeds("raw_stream_release_contains_drop_panic", CHILD);
            assert!(stderr.contains("simulated drop bug in stream cleanup"));
            return;
        }

        let scanner_poison = Arc::new(AtomicBool::new(false));
        let (_runtime, mut stream) =
            guarded_export(PanicOnDrop, test_schema(), Arc::clone(&scanner_poison));
        let release = stream.release.expect("release callback is NULL");
        unsafe { release(&mut stream) };
        assert!(stream.release.is_none());
        assert!(!scanner_poison.load(Ordering::SeqCst));
    }

    #[test]
    fn guarded_in_memory_export_supports_raw_arrow_callbacks() {
        let reader =
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(test_batch())], test_schema());
        let mut stream = guarded_ffi_stream_from_reader(reader).unwrap();

        let mut schema = FFI_ArrowSchema::empty();
        let get_schema = stream.get_schema.expect("get_schema callback is NULL");
        assert_eq!(unsafe { get_schema(&mut stream, &mut schema) }, 0);
        assert!(schema.release.is_some());
        unsafe { schema.release.unwrap()(&mut schema) };

        let get_next = stream.get_next.expect("get_next callback is NULL");
        let mut array = FFI_ArrowArray::empty();
        assert_eq!(unsafe { get_next(&mut stream, &mut array) }, 0);
        assert!(array.release.is_some());
        unsafe { array.release.unwrap()(&mut array) };

        let mut eos = FFI_ArrowArray::empty();
        assert_eq!(unsafe { get_next(&mut stream, &mut eos) }, 0);
        assert!(eos.release.is_none());

        let release = stream.release.expect("release callback is NULL");
        unsafe { release(&mut stream) };
        assert!(stream.release.is_none());
    }

    #[test]
    fn guarded_in_memory_get_next_sanitizes_nul_error() {
        const CHILD: &str = "LANCE_C_CHILD_READER_NUL_ERROR";
        if std::env::var(CHILD).is_err() {
            assert_child_succeeds("guarded_in_memory_get_next_sanitizes_nul_error", CHILD);
            return;
        }

        let reader = arrow::record_batch::RecordBatchIterator::new(
            vec![Err(ArrowError::ComputeError(
                "ordinary reader error: bo\0om".into(),
            ))],
            test_schema(),
        );
        let mut stream = guarded_ffi_stream_from_reader(reader).unwrap();
        let message = raw_error_then_eos(&mut stream);
        assert!(message.contains("bo\\0om"), "got: {message:?}");
        assert!(!message.contains('\0'));
    }

    #[test]
    fn guarded_in_memory_get_next_contains_error_display_panic() {
        const CHILD: &str = "LANCE_C_CHILD_READER_DISPLAY_PANIC";
        if std::env::var(CHILD).is_err() {
            let stderr = assert_child_succeeds(
                "guarded_in_memory_get_next_contains_error_display_panic",
                CHILD,
            );
            assert!(stderr.contains("simulated panic while formatting a reader error"));
            return;
        }

        let reader = arrow::record_batch::RecordBatchIterator::new(
            vec![Err(ArrowError::ExternalError(Box::new(PanickingDisplay)))],
            test_schema(),
        );
        let mut stream = guarded_ffi_stream_from_reader(reader).unwrap();
        let message = raw_error_then_eos(&mut stream);
        assert!(
            message.contains("simulated panic while formatting a reader error"),
            "got: {message}"
        );
    }

    #[test]
    fn guarded_in_memory_get_next_contains_reader_panic() {
        const CHILD: &str = "LANCE_C_CHILD_READER_NEXT_PANIC";
        if std::env::var(CHILD).is_err() {
            let stderr =
                assert_child_succeeds("guarded_in_memory_get_next_contains_reader_panic", CHILD);
            assert!(stderr.contains("simulated panic in materialized reader next"));
            return;
        }

        let reader = PanicOnReaderNext {
            schema: test_schema(),
        };
        let mut stream = guarded_ffi_stream_from_reader(reader).unwrap();
        let message = raw_error_then_eos(&mut stream);
        assert!(
            message.contains("simulated panic in materialized reader next"),
            "got: {message}"
        );
    }

    #[test]
    fn guarded_in_memory_rejects_nul_schema_before_callback_exposure() {
        const CHILD: &str = "LANCE_C_CHILD_READER_NUL_SCHEMA";
        if std::env::var(CHILD).is_err() {
            let stderr = assert_child_succeeds(
                "guarded_in_memory_rejects_nul_schema_before_callback_exposure",
                CHILD,
            );
            assert!(stderr.contains("NulError"));
            return;
        }

        let schema = Arc::new(Schema::new(vec![Field::new(
            "field\0name",
            DataType::Int32,
            false,
        )]));
        let reader = arrow::record_batch::RecordBatchIterator::new(
            Vec::<std::result::Result<RecordBatch, ArrowError>>::new(),
            schema,
        );

        let result = guarded_ffi_stream_from_reader(reader);
        let error = result.expect_err("invalid schema must fail before export");
        assert!(
            error.to_string().contains("panic exporting Arrow schema"),
            "got: {error}"
        );
    }

    #[test]
    fn guarded_in_memory_release_contains_reader_drop_panic() {
        const CHILD: &str = "LANCE_C_CHILD_READER_DROP_PANIC";
        if std::env::var(CHILD).is_err() {
            let stderr = assert_child_succeeds(
                "guarded_in_memory_release_contains_reader_drop_panic",
                CHILD,
            );
            assert!(stderr.contains("simulated panic in materialized reader drop"));
            return;
        }

        let reader = PanicOnReaderDrop {
            schema: test_schema(),
        };
        let mut stream = guarded_ffi_stream_from_reader(reader).unwrap();
        let release = stream.release.expect("release callback is NULL");
        unsafe { release(&mut stream) };
        assert!(stream.release.is_none());
    }
}
