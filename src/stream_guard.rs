// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Panic guard for exported Arrow streams (issue #61).
//!
//! An exported `FFI_ArrowArrayStream` outlives the call that created it:
//! lance-io's `RecordBatchIteratorAdaptor::next()` runs
//! `handle.block_on(stream.next())` inside the *consumer's* later `get_next`
//! call, on the consumer's thread, and neither arrow-rs nor lance-io guards
//! that call. A mid-iteration panic therefore unwinds out of arrow-rs's
//! `extern "C" fn get_next` and aborts the host process even under
//! `panic = "unwind"` (Rust 1.81+ semantics). Catching the panic inside the
//! stream's own `poll_next` is the only placement that covers every
//! consumer-thread poll, which is what [`GuardedStream`] does.

use std::panic::{AssertUnwindSafe, catch_unwind};
use std::pin::Pin;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::task::{Context, Poll};

use arrow_array::RecordBatch;
use futures::Stream;

use crate::error::panic_payload_message;

/// A stream wrapper that catches a panic from the inner stream's `poll_next`
/// and converts it into the Arrow C stream error contract: exactly one
/// terminal `Some(Err(..))` item — which arrow-rs's exported `get_next` maps
/// to a nonzero return plus `get_last_error` — followed by end-of-stream
/// (`None`) on every later poll. C consumers therefore observe an ordinary
/// stream error instead of a process abort, with no consumer-side changes.
///
/// On a caught panic the wrapper also flips `scanner_poison`, the flag shared
/// with the owning `LanceScanner` handle (see `LanceScanner::poison_flag()`),
/// so every later `lance_scanner_*` call on that handle fails with
/// `LANCE_ERR_PANIC` instead of touching possibly-wedged stream state.
///
/// The panic message is sanitized by [`panic_payload_message`] (NUL bytes
/// replaced) before it is baked into the error string: arrow-rs's `get_next`
/// runs `CString::new` on this string, and an embedded NUL would panic right
/// through the guard.
///
/// Upstream note: this wrapper is a candidate for promotion into
/// `lance_io::ffi::to_ffi_arrow_array_stream` itself, which would extend the
/// same protection to every consumer of that export path (e.g. lance-java).
pub struct GuardedStream<S> {
    inner: S,
    /// Fused state: set when a panic is caught, after which every poll
    /// yields `None` (end-of-stream) without touching the inner stream again.
    poisoned: bool,
    /// Shared with the owning scanner handle; flipped when a panic is caught
    /// so the handle rejects later calls with `LANCE_ERR_PANIC`.
    scanner_poison: Arc<AtomicBool>,
}

impl<S> GuardedStream<S> {
    /// Wrap `inner`, wiring the shared `scanner_poison` flag that a caught
    /// panic sets (from `LanceScanner::poison_flag()` at the export sites).
    pub fn new(inner: S, scanner_poison: Arc<AtomicBool>) -> Self {
        Self {
            inner,
            poisoned: false,
            scanner_poison,
        }
    }
}

impl<S> Stream for GuardedStream<S>
where
    S: Stream<Item = lance_core::Result<RecordBatch>> + Unpin,
{
    type Item = lance_core::Result<RecordBatch>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        if self.poisoned {
            return Poll::Ready(None);
        }
        let this = &mut *self;
        let polled = catch_unwind(AssertUnwindSafe(|| Pin::new(&mut this.inner).poll_next(cx)));
        match polled {
            Ok(item) => item,
            Err(payload) => {
                this.poisoned = true;
                this.scanner_poison.store(true, Ordering::SeqCst);
                Poll::Ready(Some(Err(lance_core::Error::internal(format!(
                    "panic in stream: {}",
                    panic_payload_message(&*payload)
                )))))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use arrow_array::Int32Array;
    use arrow_schema::{DataType, Field, Schema};
    use futures::StreamExt;

    use crate::runtime::block_on;

    fn test_batch() -> RecordBatch {
        let schema = Arc::new(Schema::new(vec![Field::new("x", DataType::Int32, false)]));
        RecordBatch::try_new(schema, vec![Arc::new(Int32Array::from(vec![1, 2, 3]))]).unwrap()
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

    #[test]
    fn panic_yields_one_error_then_fuses_and_flips_flag() {
        let scanner_poison = Arc::new(AtomicBool::new(false));
        let mut stream = GuardedStream::new(
            PanicOnSecondPoll {
                yielded: false,
                message: "simulated scan bug",
            },
            Arc::clone(&scanner_poison),
        );

        // The pre-panic item passes through untouched.
        let item = block_on(stream.next()).expect("stream ended before first batch");
        let batch = item.expect("first batch must pass through as Ok");
        assert_eq!(batch.num_rows(), 3);

        // The panic becomes exactly one terminal Err item carrying the
        // sanitized payload, and the shared flag flips.
        let item = block_on(stream.next()).expect("panic item missing");
        let err = item.expect_err("panic must surface as an Err item");
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

        // Then the stream is fused: None forever, no repeat panic, no second
        // error item.
        assert!(
            block_on(stream.next()).is_none(),
            "fused stream must yield None"
        );
        assert!(
            block_on(stream.next()).is_none(),
            "fused stream must stay fused"
        );
    }

    #[test]
    fn panic_payload_with_nul_is_sanitized() {
        let scanner_poison = Arc::new(AtomicBool::new(false));
        let mut stream = GuardedStream::new(
            PanicOnSecondPoll {
                yielded: false,
                message: "bo\0om",
            },
            scanner_poison,
        );

        let _ = block_on(stream.next()); // consume the good batch
        let err = block_on(stream.next())
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
        let mut stream = GuardedStream::new(inner, Arc::clone(&scanner_poison));

        for i in 0..2 {
            let item = block_on(stream.next()).expect("stream ended early");
            let batch = item.expect("batch must pass through untouched");
            assert_eq!(batch.num_rows(), 3, "batch {i} contents changed");
        }
        assert!(
            block_on(stream.next()).is_none(),
            "inner end-of-stream must pass through"
        );
        assert!(
            !scanner_poison.load(Ordering::SeqCst),
            "flag must stay false without a panic"
        );
    }
}
