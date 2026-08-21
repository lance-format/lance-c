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

use arrow::record_batch::RecordBatchReader;
use arrow_array::RecordBatch;
use arrow_schema::{ArrowError, SchemaRef};
use futures::{Stream, StreamExt};

use crate::error::{panic_payload_message, swallow_unwind};

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
    pub fn new(
        inner: S,
        schema: SchemaRef,
        handle: tokio::runtime::Handle,
        scanner_poison: Arc<AtomicBool>,
    ) -> Self {
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
        let polled = catch_unwind(AssertUnwindSafe(|| handle.block_on(inner.next())));
        match polled {
            Ok(Some(Ok(batch))) => Some(Ok(batch)),
            Ok(Some(Err(err))) => Some(Err(ArrowError::ExternalError(Box::new(err)))),
            Ok(None) => None,
            Err(payload) => {
                *poisoned = true;
                scanner_poison.store(true, Ordering::SeqCst);
                Some(Err(ArrowError::ExternalError(Box::new(
                    lance_core::Error::internal(format!(
                        "panic in stream: {}",
                        panic_payload_message(&*payload)
                    )),
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

#[cfg(test)]
mod tests {
    use super::*;
    use arrow_array::Int32Array;
    use arrow_schema::{DataType, Field, Schema};
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
}
