// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Scanner C API: builder, sync iteration, async scan, poll-based iteration.

use std::ffi::{c_char, c_void};
use std::pin::Pin;
use std::ptr;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};

use arrow::ffi_stream::FFI_ArrowArrayStream;
use arrow_schema::SchemaRef;
use futures::{FutureExt, Stream, StreamExt};
use lance::Dataset;
use lance::dataset::scanner::DatasetRecordBatchStream;
use lance_core::Result;
use lance_index::scalar::FullTextSearchQuery;
use lance_io::stream::RecordBatchStream;
use uuid::Uuid;

use crate::async_dispatcher::{self, LanceCallback};
use crate::batch::LanceBatch;
use crate::dataset::LanceDataset;
use crate::error::{
    LanceErrorCode, clear_last_error, error_code_from_lance, ffi_try, panic_payload_message,
    set_lance_error, set_last_error, swallow_unwind,
};
use crate::helpers;
use crate::runtime::{RT, block_on};
use crate::stream_guard::GuardedReader;

/// Data type tag for query vectors, mirroring the C enum `LanceDataType`.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceDataType {
    Float32 = 0,
    Float16 = 1,
    Float64 = 2,
    UInt8 = 3,
    Int8 = 4,
}

/// Opaque scanner handle. Stores configuration until stream materialization.
pub struct LanceScanner {
    dataset: Arc<Dataset>,
    columns: Option<Vec<String>>,
    filter: Option<String>,
    substrait_filter: Option<Vec<u8>>,
    limit: Option<i64>,
    offset: Option<i64>,
    batch_size: Option<usize>,
    with_row_id: bool,
    fragment_ids: Option<Vec<u64>>,
    index_segments: Option<Vec<Uuid>>,
    nearest: Option<NearestQuery>,
    nprobes: Option<u32>,
    refine_factor: Option<u32>,
    ef: Option<u32>,
    metric_override: Option<crate::index::LanceMetricType>,
    use_index: Option<bool>,
    prefilter: bool,
    fts_query: Option<FullTextSearchQuery>,
    // Set when a panic is caught in a stateful stream operation (issue #61):
    // once poisoned, every later `lance_scanner_*` call on this handle (except
    // `lance_scanner_close`, which must always free memory) fails with
    // `LANCE_ERR_PANIC`. Behind an `Arc` so the exported-stream wrapper and
    // the spawned async task can poison the handle from outside this call
    // frame via `poison_flag()`.
    poisoned: Arc<AtomicBool>,
    // Materialized on first iteration call
    stream: Option<Pin<Box<DatasetRecordBatchStream>>>,
    #[allow(dead_code)]
    schema: Option<SchemaRef>,
}

struct NearestQuery {
    column: String,
    query: arrow_array::ArrayRef,
    k: u32,
}

/// Poll status for `lance_scanner_poll_next`.
#[repr(C)]
#[derive(Debug, PartialEq, Eq)]
pub enum LancePollStatus {
    /// Batch available in `*out`.
    Ready = 0,
    /// I/O in progress; waker will fire when ready.
    Pending = 1,
    /// End of stream.
    Finished = 2,
    /// Error occurred (check `lance_last_error_*`).
    Error = -1,
}

/// Waker callback type for poll-based iteration.
/// Called from a Tokio I/O thread when data becomes available.
/// Must be thread-safe and must NOT call back into `lance_scanner_*`.
pub type LanceWaker = unsafe extern "C" fn(ctx: *mut c_void);

impl LanceScanner {
    fn new(dataset: Arc<Dataset>) -> Self {
        Self {
            dataset,
            columns: None,
            filter: None,
            substrait_filter: None,
            limit: None,
            offset: None,
            batch_size: None,
            with_row_id: false,
            fragment_ids: None,
            index_segments: None,
            nearest: None,
            nprobes: None,
            refine_factor: None,
            ef: None,
            metric_override: None,
            use_index: None,
            prefilter: false,
            fts_query: None,
            poisoned: Arc::new(AtomicBool::new(false)),
            stream: None,
            schema: None,
        }
    }

    /// Whether a panic was caught in an earlier stateful operation on this
    /// scanner. Checked by every `lance_scanner_*` entry point except close.
    fn is_poisoned(&self) -> bool {
        self.poisoned.load(Ordering::SeqCst)
    }

    /// Clone of the shared poison flag, for wiring into the exported Arrow
    /// stream wrapper and the async scan task (later steps of issue #61) so a
    /// panic caught there marks this handle unusable for later calls.
    pub(crate) fn poison_flag(&self) -> Arc<AtomicBool> {
        Arc::clone(&self.poisoned)
    }

    /// Apply fragment selection to a scanner builder if fragment_ids is set.
    fn apply_fragment_filter(&self, scanner: &mut lance::dataset::scanner::Scanner) -> Result<()> {
        if let Some(ids) = &self.fragment_ids {
            let all_fragments = self.dataset.get_fragments();
            let id_set: std::collections::HashSet<u64> = ids.iter().copied().collect();
            let selected: Vec<_> = all_fragments
                .into_iter()
                .filter(|f| id_set.contains(&(f.id() as u64)))
                .map(|f| f.metadata().clone())
                .collect();
            scanner.with_fragments(selected);
        }
        Ok(())
    }

    /// Build the underlying Scanner and open a stream.
    fn materialize_stream(&mut self) -> Result<()> {
        let mut scanner = self.dataset.scan();
        if let Some(cols) = &self.columns {
            scanner.project(cols)?;
        }
        // Substrait filter takes precedence over SQL filter when both are set.
        if let Some(bytes) = &self.substrait_filter {
            scanner.filter_substrait(bytes)?;
        } else if let Some(filter) = &self.filter {
            scanner.filter(filter)?;
        }
        if self.limit.is_some() || self.offset.is_some() {
            scanner.limit(self.limit, self.offset)?;
        }
        if let Some(bs) = self.batch_size {
            scanner.batch_size(bs);
        }
        if self.with_row_id {
            scanner.with_row_id();
        }
        self.apply_fragment_filter(&mut scanner)?;
        if self.index_segments.is_some() && self.nearest.is_none() {
            return Err(lance_core::Error::invalid_input_source(
                "index_segments requires nearest() to be configured".into(),
            ));
        }
        // Lance validates fragment-scoped nearest searches when nearest() is
        // configured. Such searches are supported when the fragment scan is
        // the input to a prefilter, so this flag must be set first.
        if self.prefilter {
            scanner.prefilter(true);
        }
        if let Some(n) = &self.nearest {
            scanner.nearest(&n.column, n.query.as_ref(), n.k as usize)?;
            if let Some(np) = self.nprobes {
                scanner.nprobes(np as usize);
            }
            if let Some(rf) = self.refine_factor {
                scanner.refine(rf);
            }
            if let Some(ef) = self.ef {
                scanner.ef(ef as usize);
            }
            if let Some(m) = self.metric_override {
                scanner.distance_metric(m.to_distance());
            }
            if let Some(ui) = self.use_index {
                scanner.use_index(ui);
            }
            if let Some(segments) = &self.index_segments {
                scanner.with_index_segments(segments.clone())?;
            }
        }
        if let Some(fts) = &self.fts_query {
            scanner.full_text_search(fts.clone())?;
        }
        let stream = block_on(scanner.try_into_stream())?;
        self.schema = Some(stream.schema());
        self.stream = Some(Box::pin(stream));
        Ok(())
    }

    /// Build a Scanner (without materializing) and return it.
    fn build_scanner(&self) -> Result<lance::dataset::scanner::Scanner> {
        let mut scanner = self.dataset.scan();
        if let Some(cols) = &self.columns {
            scanner.project(cols)?;
        }
        // Substrait filter takes precedence over SQL filter when both are set.
        if let Some(bytes) = &self.substrait_filter {
            scanner.filter_substrait(bytes)?;
        } else if let Some(filter) = &self.filter {
            scanner.filter(filter)?;
        }
        if self.limit.is_some() || self.offset.is_some() {
            scanner.limit(self.limit, self.offset)?;
        }
        if let Some(bs) = self.batch_size {
            scanner.batch_size(bs);
        }
        if self.with_row_id {
            scanner.with_row_id();
        }
        self.apply_fragment_filter(&mut scanner)?;
        if self.index_segments.is_some() && self.nearest.is_none() {
            return Err(lance_core::Error::invalid_input_source(
                "index_segments requires nearest() to be configured".into(),
            ));
        }
        // nearest() checks the current prefilter setting before accepting a
        // fragment-scoped search. Enable it before installing the query.
        if self.prefilter {
            scanner.prefilter(true);
        }
        if let Some(n) = &self.nearest {
            scanner.nearest(&n.column, n.query.as_ref(), n.k as usize)?;
            if let Some(np) = self.nprobes {
                scanner.nprobes(np as usize);
            }
            if let Some(rf) = self.refine_factor {
                scanner.refine(rf);
            }
            if let Some(ef) = self.ef {
                scanner.ef(ef as usize);
            }
            if let Some(m) = self.metric_override {
                scanner.distance_metric(m.to_distance());
            }
            if let Some(ui) = self.use_index {
                scanner.use_index(ui);
            }
            if let Some(segments) = &self.index_segments {
                scanner.with_index_segments(segments.clone())?;
            }
        }
        if let Some(fts) = &self.fts_query {
            scanner.full_text_search(fts.clone())?;
        }
        Ok(scanner)
    }
}

// ---------------------------------------------------------------------------
// Poison check shared by all `lance_scanner_*` entry points
// ---------------------------------------------------------------------------

/// Reject calls on a scanner handle that was poisoned by an earlier panic
/// (issue #61). Applies to every `lance_scanner_*` entry point that
/// dereferences the handle — setters included, since they are still "later
/// calls on a poisoned handle" — EXCEPT the void close/free path, which must
/// always be able to free memory.
///
/// The check runs only when the pointer is non-NULL, so a NULL handle keeps
/// flowing into the normal NULL-argument error path (observably, the poison
/// check sits right after the handle NULL check). A poisoned handle is
/// unusable, so the check precedes validation of any other arguments.
///
/// The poison error code must be `LanceErrorCode::Panic`, which a plain
/// `lance_core::Error` cannot express, so this sets the thread-local error
/// and returns `$errval` directly instead of going through `ffi_try!`.
macro_rules! scanner_poison_check {
    ($scanner:expr, $errval:expr) => {
        if !$scanner.is_null() && unsafe { &*$scanner }.is_poisoned() {
            $crate::error::set_last_error(
                $crate::error::LanceErrorCode::Panic,
                "scanner is poisoned by an earlier panic",
            );
            return $errval;
        }
    };
}

// ---------------------------------------------------------------------------
// Scanner lifecycle + builder
// ---------------------------------------------------------------------------

/// Create a new scanner for the given dataset.
///
/// - `dataset`: An open `LanceDataset*` (not consumed; remains valid).
/// - `columns`: NULL-terminated column name array, or NULL for all columns.
/// - `filter`: SQL filter expression, or NULL for no filter.
///
/// Returns a `LanceScanner*` on success, or NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_new(
    dataset: *const LanceDataset,
    columns: *const *const c_char,
    filter: *const c_char,
) -> *mut LanceScanner {
    ffi_try!(unsafe { scanner_new_inner(dataset, columns, filter) }, null)
}

unsafe fn scanner_new_inner(
    dataset: *const LanceDataset,
    columns: *const *const c_char,
    filter: *const c_char,
) -> Result<*mut LanceScanner> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let col_names = unsafe { helpers::parse_c_string_array(columns)? };
    let filter_str = unsafe { helpers::parse_c_string(filter)? }.map(|s| s.to_string());

    let mut scanner = LanceScanner::new(ds.snapshot());
    scanner.columns = col_names;
    scanner.filter = filter_str;
    Ok(Box::into_raw(Box::new(scanner)))
}

/// Set the row limit on the scanner. Returns 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_set_limit(scanner: *mut LanceScanner, limit: i64) -> i32 {
    scanner_poison_check!(scanner, -1);
    ffi_try!(unsafe { scanner_set_limit_inner(scanner, limit) }, neg)
}

unsafe fn scanner_set_limit_inner(scanner: *mut LanceScanner, limit: i64) -> Result<i32> {
    if scanner.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "scanner is NULL".into(),
        ));
    }
    let s = unsafe { &mut *scanner };
    s.limit = Some(limit);
    Ok(0)
}

/// Set the row offset on the scanner. Returns 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_set_offset(scanner: *mut LanceScanner, offset: i64) -> i32 {
    scanner_poison_check!(scanner, -1);
    ffi_try!(unsafe { scanner_set_offset_inner(scanner, offset) }, neg)
}

unsafe fn scanner_set_offset_inner(scanner: *mut LanceScanner, offset: i64) -> Result<i32> {
    if scanner.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "scanner is NULL".into(),
        ));
    }
    let s = unsafe { &mut *scanner };
    s.offset = Some(offset);
    Ok(0)
}

/// Set the batch size on the scanner. Returns 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_set_batch_size(
    scanner: *mut LanceScanner,
    batch_size: i64,
) -> i32 {
    scanner_poison_check!(scanner, -1);
    ffi_try!(
        unsafe { scanner_set_batch_size_inner(scanner, batch_size) },
        neg
    )
}

unsafe fn scanner_set_batch_size_inner(scanner: *mut LanceScanner, batch_size: i64) -> Result<i32> {
    if scanner.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "scanner is NULL".into(),
        ));
    }
    let s = unsafe { &mut *scanner };
    s.batch_size = Some(batch_size as usize);
    Ok(0)
}

/// Enable or disable row ID in scan output. Returns 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_with_row_id(
    scanner: *mut LanceScanner,
    enable: bool,
) -> i32 {
    scanner_poison_check!(scanner, -1);
    ffi_try!(unsafe { scanner_with_row_id_inner(scanner, enable) }, neg)
}

unsafe fn scanner_with_row_id_inner(scanner: *mut LanceScanner, enable: bool) -> Result<i32> {
    if scanner.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "scanner is NULL".into(),
        ));
    }
    let s = unsafe { &mut *scanner };
    s.with_row_id = enable;
    Ok(0)
}

/// Restrict the scan to the given fragment IDs.
/// Must be called before any iteration method.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_set_fragment_ids(
    scanner: *mut LanceScanner,
    ids: *const u64,
    len: usize,
) -> i32 {
    scanner_poison_check!(scanner, -1);
    ffi_try!(
        unsafe { scanner_set_fragment_ids_inner(scanner, ids, len) },
        neg
    )
}

unsafe fn scanner_set_fragment_ids_inner(
    scanner: *mut LanceScanner,
    ids: *const u64,
    len: usize,
) -> Result<i32> {
    if scanner.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "scanner is NULL".into(),
        ));
    }
    if ids.is_null() && len > 0 {
        return Err(lance_core::Error::invalid_input_source(
            "ids is NULL but len > 0".into(),
        ));
    }
    let s = unsafe { &mut *scanner };
    let id_slice = if len > 0 {
        unsafe { std::slice::from_raw_parts(ids, len) }
    } else {
        &[]
    };
    s.fragment_ids = Some(id_slice.to_vec());
    Ok(0)
}

/// Set a Substrait filter on the scanner.
///
/// `bytes` must point to a serialized Substrait
/// [`ExtendedExpression`](https://substrait.io/expressions/extended_expression/)
/// message containing exactly one expression of boolean type. This is the
/// preferred filter API for query engines that already speak Substrait — it
/// avoids the round-trip through SQL string formatting and parsing.
///
/// If both this and the SQL filter passed to `lance_scanner_new` are set, the
/// Substrait filter wins. Calling this with the same scanner more than once
/// replaces the previously-set Substrait filter.
///
/// - `scanner`: An open `LanceScanner*`.
/// - `bytes`: Pointer to the serialized Substrait `ExtendedExpression` bytes.
///   Must not be NULL and `len` must be > 0. The bytes are copied into the
///   scanner; the caller may free them after this call returns.
/// - `len`: Length of the byte buffer.
///
/// Returns 0 on success, -1 on error (check `lance_last_error_*`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_set_substrait_filter(
    scanner: *mut LanceScanner,
    bytes: *const u8,
    len: usize,
) -> i32 {
    scanner_poison_check!(scanner, -1);
    ffi_try!(
        unsafe { scanner_set_substrait_filter_inner(scanner, bytes, len) },
        neg
    )
}

unsafe fn scanner_set_substrait_filter_inner(
    scanner: *mut LanceScanner,
    bytes: *const u8,
    len: usize,
) -> Result<i32> {
    if scanner.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "scanner is NULL".into(),
        ));
    }
    if bytes.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "bytes is NULL".into(),
        ));
    }
    if len == 0 {
        return Err(lance_core::Error::invalid_input_source(
            "Substrait filter bytes must be non-empty".into(),
        ));
    }
    let slice = unsafe { std::slice::from_raw_parts(bytes, len) };
    let s = unsafe { &mut *scanner };
    s.substrait_filter = Some(slice.to_vec());
    Ok(0)
}

/// Close and free a scanner handle.
///
/// Best-effort (issue #61): this drops a possibly-live
/// `DatasetRecordBatchStream`, the highest-risk `Drop` in this crate. A
/// panic raised while dropping the handle is caught and logged rather than
/// unwinding into the caller, and the remainder of the value may leak.
/// Deliberately no poison check — a poisoned scanner must still be freeable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_close(scanner: *mut LanceScanner) {
    if !scanner.is_null() {
        swallow_unwind("lance_scanner_close", || unsafe {
            let _ = Box::from_raw(scanner);
        });
    }
}

// ---------------------------------------------------------------------------
// Sync stream: ArrowArrayStream export
// ---------------------------------------------------------------------------

/// Materialize the scan as an Arrow C Data Interface `ArrowArrayStream`.
///
/// This is the preferred API for simple integrations — blocks the calling thread.
/// The scanner is consumed by this call and should not be used afterward (close it).
///
/// The exported stream is panic-guarded (issue #61): a panic during export
/// poisons the scanner — this call returns -1 with `LANCE_ERR_PANIC`, and
/// every later call on the handle fails the same way — while a panic during
/// later consumer-driven `get_next` calls surfaces as a stream error
/// (nonzero `get_next` + `get_last_error`, then end-of-stream) instead of
/// unwinding out of the Arrow C callbacks and aborting the host process.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_to_arrow_stream(
    scanner: *mut LanceScanner,
    out: *mut FFI_ArrowArrayStream,
) -> i32 {
    if scanner.is_null() || out.is_null() {
        set_last_error(
            LanceErrorCode::InvalidArgument,
            "scanner and out must not be NULL",
        );
        return -1;
    }
    scanner_poison_check!(scanner, -1);
    let s = unsafe { &*scanner };
    let poisoned = s.poison_flag();
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        scanner_to_arrow_stream_inner(s, out)
    })) {
        Ok(Ok(rc)) => {
            clear_last_error();
            rc
        }
        Ok(Err(err)) => {
            set_lance_error(&err);
            -1
        }
        Err(payload) => {
            poisoned.store(true, Ordering::SeqCst);
            set_last_error(
                LanceErrorCode::Panic,
                format!("panic in FFI call: {}", panic_payload_message(&*payload)),
            );
            -1
        }
    }
}

/// Export logic, split out so `lance_scanner_to_arrow_stream` can wrap the
/// whole thing in `catch_unwind` and poison the handle on panic. The stream
/// is exported through a [`GuardedReader`] so panics during the consumer's
/// later `get_next` calls — including a `Handle::block_on` panic on a
/// consumer thread that is driving a Tokio runtime — convert to a terminal
/// stream
/// error (and poison the handle) instead of unwinding across the Arrow C
/// callbacks, and cleanup panics on the `release` path are contained.
///
/// # Safety
/// `out` must be a valid, writable pointer (checked by the caller).
unsafe fn scanner_to_arrow_stream_inner(
    s: &LanceScanner,
    out: *mut FFI_ArrowArrayStream,
) -> Result<i32> {
    let built_scanner = s.build_scanner()?;
    let stream = block_on(built_scanner.try_into_stream())?;
    let schema = stream.schema();
    let reader = GuardedReader::new(stream, schema, RT.handle().clone(), s.poison_flag());
    let ffi_stream = FFI_ArrowArrayStream::new(Box::new(reader));
    unsafe {
        ptr::write_unaligned(out, ffi_stream);
    }
    Ok(0)
}

// ---------------------------------------------------------------------------
// Sync iteration: blocking batch-at-a-time
// ---------------------------------------------------------------------------

/// Read the next batch from the scanner (blocking).
///
/// Returns:
/// -  `0` — batch available, `*out` is set.
/// -  `1` — end of stream, `*out` is NULL.
/// - `-1` — error (check `lance_last_error_*`), `*out` is NULL.
///
/// The caller must free each returned batch with `lance_batch_free()`.
///
/// A panic in the stream logic poisons the scanner: this call returns -1 with
/// `LANCE_ERR_PANIC`, and every later call on the handle fails the same way.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_next(
    scanner: *mut LanceScanner,
    out: *mut *mut LanceBatch,
) -> i32 {
    if scanner.is_null() || out.is_null() {
        set_last_error(
            LanceErrorCode::InvalidArgument,
            "scanner and out must not be NULL",
        );
        return -1;
    }
    scanner_poison_check!(scanner, -1);
    let s = unsafe { &mut *scanner };
    let poisoned = s.poison_flag();
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        scanner_next_inner(s, out)
    })) {
        Ok(rc) => rc,
        Err(payload) => {
            poisoned.store(true, Ordering::SeqCst);
            set_last_error(
                LanceErrorCode::Panic,
                format!("panic in FFI call: {}", panic_payload_message(&*payload)),
            );
            unsafe { *out = ptr::null_mut() };
            -1
        }
    }
}

/// Blocking next-batch logic, split out so `lance_scanner_next` can wrap the
/// whole thing in `catch_unwind` and poison the handle on panic. Error and
/// end-of-stream semantics (0/1/-1 plus thread-local error) are unchanged.
///
/// # Safety
/// `out` must be a valid, writable pointer (checked by the caller).
unsafe fn scanner_next_inner(s: &mut LanceScanner, out: *mut *mut LanceBatch) -> i32 {
    // Lazily materialize the stream on first call.
    if s.stream.is_none()
        && let Err(err) = s.materialize_stream()
    {
        set_lance_error(&err);
        unsafe { *out = ptr::null_mut() };
        return -1;
    }

    let stream = s.stream.as_mut().unwrap();
    match block_on(stream.next()) {
        Some(Ok(batch)) => {
            clear_last_error();
            let lance_batch = LanceBatch { inner: batch };
            unsafe { *out = Box::into_raw(Box::new(lance_batch)) };
            0
        }
        Some(Err(err)) => {
            set_lance_error(&err);
            unsafe { *out = ptr::null_mut() };
            -1
        }
        None => {
            // End of stream
            clear_last_error();
            unsafe { *out = ptr::null_mut() };
            1
        }
    }
}

// ---------------------------------------------------------------------------
// Async scan: callback-based
// ---------------------------------------------------------------------------

/// Start an async scan. The callback is invoked on a dedicated dispatcher thread
/// when the ArrowArrayStream is ready.
///
/// - `callback`: Called with `(ctx, 0, *mut ArrowArrayStream)` on success,
///   or `(ctx, -1, NULL)` on error. On error, the dispatcher installs the
///   error on the callback thread's TLS first, so `lance_last_error_*`
///   called from inside the callback observes the failure.
/// - `callback_ctx`: Opaque pointer passed back to the callback.
///
/// The scanner configuration is captured at call time. The scanner handle
/// can be closed immediately after this call.
///
/// The promised contract is exactly one callback completion, even on panic.
/// A panic anywhere in call-time setup (validation, scanner building,
/// runtime access, task spawn) is caught by the entry guard below and still
/// reported through the callback: `(ctx, -1, NULL)` with `LANCE_ERR_PANIC`,
/// and the scanner is poisoned. A panic inside the spawned task is caught by
/// the task's own `catch_unwind().await` and reported the same way — the
/// caller never hangs waiting for a completion that would otherwise die as
/// an unobserved task `JoinError` or an unwinding abort.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_scan_async(
    scanner: *const LanceScanner,
    callback: LanceCallback,
    callback_ctx: *mut c_void,
) {
    unsafe {
        scan_async_guarded(scanner, callback, callback_ctx, |s, cb, ctx| {
            scan_async_setup(s, cb, ctx)
        });
    }
}

/// Entry guard for `lance_scanner_scan_async` (issue #61): catches a panic
/// anywhere in call-time setup so it cannot unwind out of the non-unwinding
/// public entry point, then poisons the handle (when one was passed) and
/// dispatches exactly one `LANCE_ERR_PANIC` completion. The setup closure is
/// injectable so tests can exercise this guard without a production fault
/// hook.
///
/// # Safety
/// `scanner` must be NULL or a valid scanner handle; `callback` and
/// `callback_ctx` follow the same contract as `lance_scanner_scan_async`.
unsafe fn scan_async_guarded(
    scanner: *const LanceScanner,
    callback: LanceCallback,
    callback_ctx: *mut c_void,
    setup: impl FnOnce(*const LanceScanner, LanceCallback, *mut c_void),
) {
    // Capture the poison flag BEFORE setup runs: a panic during setup must
    // still be able to poison the handle it never finished configuring.
    let poison_flag = if scanner.is_null() {
        None
    } else {
        Some(unsafe { &*scanner }.poison_flag())
    };
    let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        setup(scanner, callback, callback_ctx)
    }));
    if let Err(payload) = outcome {
        if let Some(flag) = poison_flag {
            flag.store(true, Ordering::SeqCst);
        }
        // Best-effort: if even reporting panics (e.g. the dispatcher thread
        // never started), swallow — a second panic unwinding out of this
        // entry point would abort the host, which is what this guard exists
        // to prevent.
        swallow_unwind("lance_scanner_scan_async panic report", || {
            async_dispatcher::dispatch_callback(
                callback,
                callback_ctx,
                -1,
                ptr::null_mut(),
                Some((
                    LanceErrorCode::Panic,
                    format!("panic in FFI call: {}", panic_payload_message(&*payload)),
                )),
            );
        });
    }
}

/// Call-time setup plus task spawn for `lance_scanner_scan_async`, split out
/// so the entry guard can wrap the whole thing in `catch_unwind`. Every
/// handled failure delivers the `-1` callback itself; an escaped panic is
/// the entry guard's job.
///
/// # Safety
/// `scanner` must be NULL or a valid scanner handle (checked first).
unsafe fn scan_async_setup(
    scanner: *const LanceScanner,
    callback: LanceCallback,
    callback_ctx: *mut c_void,
) {
    // Validation-time failures happen before scan_async returns, so they keep
    // setting the caller thread's TLS AND carry the error inside the dispatch
    // message for the callback thread to observe (issue #61).
    if scanner.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "scanner is NULL");
        async_dispatcher::dispatch_callback(
            callback,
            callback_ctx,
            -1,
            ptr::null_mut(),
            Some((LanceErrorCode::InvalidArgument, "scanner is NULL".into())),
        );
        return;
    }

    let s = unsafe { &*scanner };
    if s.is_poisoned() {
        // Hand-rolled poison check (the shared macro cannot dispatch the
        // error callback this void entry point reports through).
        set_last_error(
            LanceErrorCode::Panic,
            "scanner is poisoned by an earlier panic",
        );
        async_dispatcher::dispatch_callback(
            callback,
            callback_ctx,
            -1,
            ptr::null_mut(),
            Some((
                LanceErrorCode::Panic,
                "scanner is poisoned by an earlier panic".into(),
            )),
        );
        return;
    }

    let built_scanner = match s.build_scanner() {
        Ok(sc) => sc,
        Err(err) => {
            set_lance_error(&err);
            async_dispatcher::dispatch_callback(
                callback,
                callback_ctx,
                -1,
                ptr::null_mut(),
                Some((error_code_from_lance(&err), err.to_string())),
            );
            return;
        }
    };

    let handle = RT.handle().clone();

    // Wrap non-Send raw pointers for the async task.
    // Safety: The C caller guarantees callback_ctx remains valid until callback fires.
    #[derive(Clone, Copy)]
    struct SendCallback {
        callback: LanceCallback,
        ctx: *mut c_void,
    }
    unsafe impl Send for SendCallback {}

    impl SendCallback {
        fn dispatch(
            &self,
            status: i32,
            result: *mut c_void,
            error: Option<(LanceErrorCode, String)>,
        ) {
            async_dispatcher::dispatch_callback(self.callback, self.ctx, status, result, error);
        }
    }

    let send_cb = SendCallback {
        callback,
        ctx: callback_ctx,
    };

    // Shared poison flag moved into the task: the GuardedReader below flips
    // it if a panic is caught during the consumer's later `get_next` calls.
    let poisoned = s.poison_flag();

    RT.spawn(async move {
        // Copies kept outside the inner future (which consumes the originals)
        // so the panic arm below can still poison the handle and report.
        let poisoned_on_panic = Arc::clone(&poisoned);
        let send_cb_on_panic = send_cb;
        // The whole task body runs under catch_unwind (issue #61): a panic
        // here would otherwise die as an unobserved JoinError — the callback
        // would never fire and the C caller would hang forever waiting for a
        // completion that never arrives.
        let outcome = std::panic::AssertUnwindSafe(async move {
            let result = built_scanner.try_into_stream().await;
            match result {
                Ok(stream) => {
                    // Guard the exported stream at the reader level (issue
                    // #61): a mid-iteration panic — including a
                    // `Handle::block_on` panic on a consumer thread driving a
                    // Tokio runtime — becomes one terminal error item per
                    // the Arrow C stream error contract instead of unwinding
                    // out of arrow-rs's `get_next`, and cleanup panics on the
                    // `release` path are contained.
                    let schema = stream.schema();
                    let reader = GuardedReader::new(stream, schema, handle, poisoned);
                    let ffi_stream = FFI_ArrowArrayStream::new(Box::new(reader));
                    let ptr = Box::into_raw(Box::new(ffi_stream));
                    send_cb.dispatch(0, ptr as *mut c_void, None);
                }
                Err(err) => {
                    // Runs on a Tokio worker AFTER scan_async returned:
                    // setting this thread's TLS would be invisible to
                    // everyone, so the error rides inside the dispatch
                    // message and the dispatcher installs it on the
                    // callback thread instead (issue #61).
                    send_cb.dispatch(
                        -1,
                        std::ptr::null_mut(),
                        Some((error_code_from_lance(&err), err.to_string())),
                    );
                }
            }
        })
        .catch_unwind()
        .await;
        if let Err(payload) = outcome {
            poisoned_on_panic.store(true, Ordering::SeqCst);
            send_cb_on_panic.dispatch(
                -1,
                std::ptr::null_mut(),
                Some((
                    LanceErrorCode::Panic,
                    format!("panic in FFI call: {}", panic_payload_message(&*payload)),
                )),
            );
        }
    });
}

// ---------------------------------------------------------------------------
// Poll-based iteration (for cooperative async runtimes)
// ---------------------------------------------------------------------------

/// Poll for the next batch without blocking.
///
/// - If data is already buffered, returns `LANCE_POLL_READY` immediately.
/// - If I/O is needed, returns `LANCE_POLL_PENDING` and schedules the waker callback.
///   The caller should yield the thread and re-poll after the waker fires.
/// - The waker is single-use: it fires at most once per poll call that returns PENDING.
///
/// The stream is lazily materialized on the first poll call (which will typically
/// return PENDING while the stream opens).
///
/// A panic in the poll logic poisons the scanner: this call returns
/// `LANCE_POLL_ERROR` with `LANCE_ERR_PANIC`, and every later call on the
/// handle fails the same way.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_poll_next(
    scanner: *mut LanceScanner,
    waker: LanceWaker,
    waker_ctx: *mut c_void,
    out: *mut *mut LanceBatch,
) -> LancePollStatus {
    if scanner.is_null() || out.is_null() {
        set_last_error(
            LanceErrorCode::InvalidArgument,
            "scanner and out must not be NULL",
        );
        return LancePollStatus::Error;
    }
    scanner_poison_check!(scanner, LancePollStatus::Error);
    let s = unsafe { &mut *scanner };
    let poisoned = s.poison_flag();
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        scanner_poll_next_inner(s, waker, waker_ctx, out)
    })) {
        Ok(status) => status,
        Err(payload) => {
            poisoned.store(true, Ordering::SeqCst);
            set_last_error(
                LanceErrorCode::Panic,
                format!("panic in FFI call: {}", panic_payload_message(&*payload)),
            );
            unsafe { *out = ptr::null_mut() };
            LancePollStatus::Error
        }
    }
}

/// Poll logic, split out so `lance_scanner_poll_next` can wrap the whole
/// thing in `catch_unwind` and poison the handle on panic. Ready/Pending/
/// Finished/Error mapping and thread-local error semantics are unchanged.
///
/// # Safety
/// `out` must be a valid, writable pointer (checked by the caller);
/// `waker_ctx` must satisfy the `LanceWaker` contract.
unsafe fn scanner_poll_next_inner(
    s: &mut LanceScanner,
    waker: LanceWaker,
    waker_ctx: *mut c_void,
    out: *mut *mut LanceBatch,
) -> LancePollStatus {
    // Lazily materialize the stream.
    if s.stream.is_none()
        && let Err(err) = s.materialize_stream()
    {
        set_lance_error(&err);
        unsafe { *out = ptr::null_mut() };
        return LancePollStatus::Error;
    }

    let stream = s.stream.as_mut().unwrap();

    // Construct a std::task::Waker from the C function pointer.
    let raw_waker = make_raw_waker(waker, waker_ctx);
    let waker_obj = unsafe { Waker::from_raw(raw_waker) };
    let mut cx = Context::from_waker(&waker_obj);

    // Enter the Tokio runtime context so internal I/O futures can access
    // the reactor. Without this, polling from a non-Tokio thread panics.
    let _guard = RT.enter();

    match stream.as_mut().poll_next(&mut cx) {
        Poll::Ready(Some(Ok(batch))) => {
            clear_last_error();
            let lance_batch = LanceBatch { inner: batch };
            unsafe { *out = Box::into_raw(Box::new(lance_batch)) };
            LancePollStatus::Ready
        }
        Poll::Ready(Some(Err(err))) => {
            set_lance_error(&err);
            unsafe { *out = ptr::null_mut() };
            LancePollStatus::Error
        }
        Poll::Ready(None) => {
            clear_last_error();
            unsafe { *out = ptr::null_mut() };
            LancePollStatus::Finished
        }
        Poll::Pending => {
            clear_last_error();
            unsafe { *out = ptr::null_mut() };
            LancePollStatus::Pending
        }
    }
}

// ---------------------------------------------------------------------------
// Waker construction from C function pointer
// ---------------------------------------------------------------------------

/// Context for a C waker callback.
struct CWakerContext {
    waker_fn: LanceWaker,
    ctx: *mut c_void,
}

// C function pointers + void* are Send by convention for FFI.
unsafe impl Send for CWakerContext {}
unsafe impl Sync for CWakerContext {}

fn make_raw_waker(waker_fn: LanceWaker, ctx: *mut c_void) -> RawWaker {
    let data = Box::into_raw(Box::new(CWakerContext { waker_fn, ctx })) as *const ();

    const VTABLE: RawWakerVTable = RawWakerVTable::new(
        // clone
        |data| {
            let orig = unsafe { &*(data as *const CWakerContext) };
            let cloned = Box::new(CWakerContext {
                waker_fn: orig.waker_fn,
                ctx: orig.ctx,
            });
            RawWaker::new(Box::into_raw(cloned) as *const (), &VTABLE)
        },
        // wake (consumes)
        |data| {
            let ctx = unsafe { Box::from_raw(data as *mut CWakerContext) };
            unsafe { (ctx.waker_fn)(ctx.ctx) };
        },
        // wake_by_ref
        |data| {
            let ctx = unsafe { &*(data as *const CWakerContext) };
            unsafe { (ctx.waker_fn)(ctx.ctx) };
        },
        // drop
        |data| {
            unsafe {
                let _ = Box::from_raw(data as *mut CWakerContext);
            };
        },
    );

    RawWaker::new(data, &VTABLE)
}

// ---------------------------------------------------------------------------
// Vector search (Phase 2): setter knobs
// ---------------------------------------------------------------------------

macro_rules! scanner_set_u32 {
    ($name:ident, $field:ident) => {
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $name(scanner: *mut LanceScanner, value: u32) -> i32 {
            scanner_poison_check!(scanner, -1);
            ffi_try!(
                (|| -> Result<i32> {
                    if scanner.is_null() {
                        return Err(lance_core::Error::invalid_input_source(
                            "scanner is NULL".into(),
                        ));
                    }
                    unsafe {
                        (*scanner).$field = Some(value);
                    }
                    Ok(0)
                })(),
                neg
            )
        }
    };
}

scanner_set_u32!(lance_scanner_set_nprobes, nprobes);
scanner_set_u32!(lance_scanner_set_refine_factor, refine_factor);
scanner_set_u32!(lance_scanner_set_ef, ef);

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_set_metric(scanner: *mut LanceScanner, metric: i32) -> i32 {
    scanner_poison_check!(scanner, -1);
    ffi_try!(unsafe { scanner_set_metric_inner(scanner, metric) }, neg)
}

unsafe fn scanner_set_metric_inner(scanner: *mut LanceScanner, metric: i32) -> Result<i32> {
    if scanner.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "scanner is NULL".into(),
        ));
    }
    let m = match metric {
        0 => crate::index::LanceMetricType::L2,
        1 => crate::index::LanceMetricType::Cosine,
        2 => crate::index::LanceMetricType::Dot,
        3 => crate::index::LanceMetricType::Hamming,
        _ => {
            return Err(lance_core::Error::invalid_input_source(
                format!("invalid metric: {}", metric).into(),
            ));
        }
    };
    unsafe {
        (*scanner).metric_override = Some(m);
    }
    Ok(0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_set_use_index(
    scanner: *mut LanceScanner,
    enable: bool,
) -> i32 {
    scanner_poison_check!(scanner, -1);
    ffi_try!(unsafe { scanner_set_use_index_inner(scanner, enable) }, neg)
}

unsafe fn scanner_set_use_index_inner(scanner: *mut LanceScanner, enable: bool) -> Result<i32> {
    if scanner.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "scanner is NULL".into(),
        ));
    }
    unsafe {
        (*scanner).use_index = Some(enable);
    }
    Ok(0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_set_prefilter(
    scanner: *mut LanceScanner,
    enable: bool,
) -> i32 {
    scanner_poison_check!(scanner, -1);
    ffi_try!(unsafe { scanner_set_prefilter_inner(scanner, enable) }, neg)
}

unsafe fn scanner_set_prefilter_inner(scanner: *mut LanceScanner, enable: bool) -> Result<i32> {
    if scanner.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "scanner is NULL".into(),
        ));
    }
    unsafe {
        (*scanner).prefilter = enable;
    }
    Ok(0)
}

/// Restrict the next `nearest()` query to a specific subset of vector index segments.
///
/// Each segment is a 16-byte UUID (RFC 4122 layout). Pass an array of `len`
/// 16-byte buffers concatenated end-to-end (so the total byte length is `len * 16`).
/// Used by distributed query engines (e.g. Velox) to fan k-NN out across workers,
/// each handling a slice of segments. The coordinator gets the segment list via
/// `lance_dataset_index_segments()`.
///
/// Calling with `len == 0` clears the segment restriction.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_set_index_segments(
    scanner: *mut LanceScanner,
    segment_uuids: *const u8,
    len: usize,
) -> i32 {
    scanner_poison_check!(scanner, -1);
    ffi_try!(
        unsafe { scanner_set_index_segments_inner(scanner, segment_uuids, len) },
        neg
    )
}

unsafe fn scanner_set_index_segments_inner(
    scanner: *mut LanceScanner,
    segment_uuids: *const u8,
    len: usize,
) -> Result<i32> {
    if scanner.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "scanner is NULL".into(),
        ));
    }
    if segment_uuids.is_null() && len > 0 {
        return Err(lance_core::Error::invalid_input_source(
            "segment_uuids is NULL but len > 0".into(),
        ));
    }
    let s = unsafe { &mut *scanner };
    if len == 0 {
        s.index_segments = None;
    } else {
        let mut uuids = Vec::with_capacity(len);
        for i in 0..len {
            let mut bytes = [0u8; 16];
            unsafe {
                std::ptr::copy_nonoverlapping(segment_uuids.add(i * 16), bytes.as_mut_ptr(), 16);
            }
            uuids.push(Uuid::from_bytes(bytes));
        }
        s.index_segments = Some(uuids);
    }
    Ok(0)
}

// ---------------------------------------------------------------------------
// Vector search (Phase 2): k-NN query setter
// ---------------------------------------------------------------------------

/// Set the k-NN query on the scanner.
///
/// - `column`: Vector column to search.
/// - `query_data`: Pointer to the query vector elements.
/// - `query_len`: Number of elements (vector dimension).
/// - `element_type`: `LanceDataType` discriminant for the element type.
/// - `k`: Number of nearest neighbors to return (must be > 0).
///
/// Returns 0 on success, -1 on error (check `lance_last_error_*`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_nearest(
    scanner: *mut LanceScanner,
    column: *const c_char,
    query_data: *const c_void,
    query_len: usize,
    element_type: i32,
    k: u32,
) -> i32 {
    scanner_poison_check!(scanner, -1);
    ffi_try!(
        unsafe { scanner_nearest_inner(scanner, column, query_data, query_len, element_type, k) },
        neg
    )
}

unsafe fn scanner_nearest_inner(
    scanner: *mut LanceScanner,
    column: *const c_char,
    query_data: *const c_void,
    query_len: usize,
    element_type: i32,
    k: u32,
) -> Result<i32> {
    if scanner.is_null() || column.is_null() || query_data.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "scanner, column, and query_data must not be NULL".into(),
        ));
    }
    if k == 0 {
        return Err(lance_core::Error::invalid_input_source(
            "k must be > 0".into(),
        ));
    }
    let s = unsafe { &mut *scanner };
    if s.fts_query.is_some() {
        return Err(lance_core::Error::invalid_input_source(
            "cannot call nearest after full_text_search; they are mutually exclusive".into(),
        ));
    }
    let column_str = unsafe { helpers::parse_c_string(column)? }.unwrap();

    let dtype = match element_type {
        0 => LanceDataType::Float32,
        1 => LanceDataType::Float16,
        2 => LanceDataType::Float64,
        3 => LanceDataType::UInt8,
        4 => LanceDataType::Int8,
        _ => {
            return Err(lance_core::Error::invalid_input_source(
                format!("invalid element_type: {}", element_type).into(),
            ));
        }
    };

    let query: arrow_array::ArrayRef = match dtype {
        LanceDataType::Float32 => {
            let slice = unsafe { std::slice::from_raw_parts(query_data as *const f32, query_len) };
            std::sync::Arc::new(arrow_array::Float32Array::from(slice.to_vec()))
        }
        LanceDataType::Float64 => {
            let slice = unsafe { std::slice::from_raw_parts(query_data as *const f64, query_len) };
            std::sync::Arc::new(arrow_array::Float64Array::from(slice.to_vec()))
        }
        LanceDataType::UInt8 => {
            let slice = unsafe { std::slice::from_raw_parts(query_data as *const u8, query_len) };
            std::sync::Arc::new(arrow_array::UInt8Array::from(slice.to_vec()))
        }
        LanceDataType::Int8 => {
            let slice = unsafe { std::slice::from_raw_parts(query_data as *const i8, query_len) };
            std::sync::Arc::new(arrow_array::Int8Array::from(slice.to_vec()))
        }
        LanceDataType::Float16 => {
            let raw = unsafe { std::slice::from_raw_parts(query_data as *const u16, query_len) };
            let values: Vec<half::f16> =
                raw.iter().map(|bits| half::f16::from_bits(*bits)).collect();
            std::sync::Arc::new(arrow_array::Float16Array::from(values))
        }
    };

    s.nearest = Some(NearestQuery {
        column: column_str.to_string(),
        query,
        k,
    });
    Ok(0)
}

// ---------------------------------------------------------------------------
// Full-text search (Phase 2)
// ---------------------------------------------------------------------------

/// Set a BM25 full-text search query on the scanner.
///
/// - `query`: Query string (terms).
/// - `columns`: NULL-terminated array of column names, or NULL to search all
///   FTS-indexed columns.
/// - `max_fuzzy_distance`: 0 = exact match; >0 = `MatchQuery::with_fuzziness`.
///
/// Returns 0 on success, -1 on error (check `lance_last_error_*`).
///
/// Mutually exclusive with `lance_scanner_nearest`: calling either after the
/// other returns InvalidArgument.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_scanner_full_text_search(
    scanner: *mut LanceScanner,
    query: *const c_char,
    columns: *const *const c_char,
    max_fuzzy_distance: u32,
) -> i32 {
    scanner_poison_check!(scanner, -1);
    ffi_try!(
        unsafe { fts_inner(scanner, query, columns, max_fuzzy_distance) },
        neg
    )
}

unsafe fn fts_inner(
    scanner: *mut LanceScanner,
    query: *const c_char,
    columns: *const *const c_char,
    max_fuzzy_distance: u32,
) -> Result<i32> {
    if scanner.is_null() || query.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "scanner and query must not be NULL".into(),
        ));
    }
    let s = unsafe { &mut *scanner };

    // Mutual exclusion with vector search.
    if s.nearest.is_some() {
        return Err(lance_core::Error::invalid_input_source(
            "cannot call full_text_search after nearest; they are mutually exclusive".into(),
        ));
    }

    let query_str = unsafe { helpers::parse_c_string(query)? }
        .unwrap()
        .to_string();
    let cols = unsafe { helpers::parse_c_string_array(columns)? };

    let mut fts = if max_fuzzy_distance > 0 {
        FullTextSearchQuery::new_fuzzy(query_str, Some(max_fuzzy_distance))
    } else {
        FullTextSearchQuery::new(query_str)
    };

    if let Some(cols) = cols
        && !cols.is_empty()
    {
        fts = fts.with_columns(&cols)?;
    }

    s.fts_query = Some(fts);
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dataset::{lance_dataset_close, lance_dataset_open};
    use crate::error::{lance_last_error_code, lance_last_error_message};
    use std::ffi::{CStr, CString};
    use std::sync::atomic::AtomicI32;

    use arrow_array::{Int32Array, RecordBatch, StringArray};
    use arrow_schema::{DataType, Field, Schema};

    /// Write a 3-row dataset to a tempdir, returning (tempdir, uri).
    fn create_test_dataset() -> (tempfile::TempDir, String) {
        let tmp = tempfile::tempdir().unwrap();
        let uri = tmp.path().join("scanner_ds").to_str().unwrap().to_string();

        let schema = Arc::new(Schema::new(vec![
            Field::new("id", DataType::Int32, false),
            Field::new("name", DataType::Utf8, true),
        ]));
        let batch = RecordBatch::try_new(
            schema.clone(),
            vec![
                Arc::new(Int32Array::from(vec![1, 2, 3])),
                Arc::new(StringArray::from(vec!["a", "b", "c"])),
            ],
        )
        .unwrap();

        block_on(Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
            &uri,
            None,
        ))
        .unwrap();
        (tmp, uri)
    }

    /// Open a dataset + unconfigured scanner through the public entry points.
    fn open_dataset_and_scanner(uri: &str) -> (*mut LanceDataset, *mut LanceScanner) {
        let c_uri = CString::new(uri).unwrap();
        let dataset = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
        assert!(!dataset.is_null(), "lance_dataset_open failed");
        let scanner = unsafe { lance_scanner_new(dataset, ptr::null(), ptr::null()) };
        assert!(!scanner.is_null(), "lance_scanner_new failed");
        (dataset, scanner)
    }

    fn poison(scanner: *mut LanceScanner) {
        unsafe { &*scanner }
            .poison_flag()
            .store(true, Ordering::SeqCst);
    }

    /// Assert the pending thread-local error is `Panic` carrying the poison
    /// message; consumes it so the next assertion starts from a clean slate.
    fn assert_poison_error_pending() {
        assert_eq!(lance_last_error_code(), LanceErrorCode::Panic);
        let msg_ptr = lance_last_error_message();
        assert!(!msg_ptr.is_null(), "poison error must carry a message");
        let msg = unsafe { CStr::from_ptr(msg_ptr) }
            .to_string_lossy()
            .into_owned();
        unsafe { crate::error::lance_free_string(msg_ptr) };
        assert!(
            msg.contains("poisoned by an earlier panic"),
            "unexpected message: {msg}"
        );
        assert_eq!(lance_last_error_code(), LanceErrorCode::Ok);
    }

    unsafe extern "C" fn noop_waker(_ctx: *mut c_void) {}

    #[test]
    fn poisoned_scanner_rejects_setters_with_panic_code() {
        let (_tmp, uri) = create_test_dataset();
        let (dataset, scanner) = open_dataset_and_scanner(&uri);
        poison(scanner);

        // Representative setters across the hand-written and macro-generated
        // families: each must return -1 with LANCE_ERR_PANIC.
        let rc = unsafe { lance_scanner_set_limit(scanner, 10) };
        assert_eq!(rc, -1);
        assert_poison_error_pending();

        let rc = unsafe { lance_scanner_set_nprobes(scanner, 4) };
        assert_eq!(rc, -1);
        assert_poison_error_pending();

        let rc = unsafe { lance_scanner_set_prefilter(scanner, true) };
        assert_eq!(rc, -1);
        assert_poison_error_pending();

        // to_arrow_stream also dereferences the handle: same rejection.
        let mut ffi_stream = FFI_ArrowArrayStream::empty();
        let rc = unsafe { lance_scanner_to_arrow_stream(scanner, &mut ffi_stream) };
        assert_eq!(rc, -1);
        assert_poison_error_pending();

        // Close must still free a poisoned handle (no poison check there).
        unsafe {
            lance_scanner_close(scanner);
            lance_dataset_close(dataset);
        }
    }

    #[test]
    fn poisoned_scanner_next_returns_panic_code() {
        let (_tmp, uri) = create_test_dataset();
        let (dataset, scanner) = open_dataset_and_scanner(&uri);
        poison(scanner);

        let mut batch: *mut LanceBatch = ptr::null_mut();
        let rc = unsafe { lance_scanner_next(scanner, &mut batch) };
        assert_eq!(rc, -1);
        assert!(batch.is_null(), "error path must leave *out NULL");
        assert_poison_error_pending();

        unsafe {
            lance_scanner_close(scanner);
            lance_dataset_close(dataset);
        }
    }

    #[test]
    fn poisoned_scanner_poll_next_returns_error_status() {
        let (_tmp, uri) = create_test_dataset();
        let (dataset, scanner) = open_dataset_and_scanner(&uri);
        poison(scanner);

        let mut batch: *mut LanceBatch = ptr::null_mut();
        let status =
            unsafe { lance_scanner_poll_next(scanner, noop_waker, ptr::null_mut(), &mut batch) };
        assert_eq!(status, LancePollStatus::Error);
        assert!(batch.is_null(), "error path must leave *out NULL");
        assert_poison_error_pending();

        unsafe {
            lance_scanner_close(scanner);
            lance_dataset_close(dataset);
        }
    }

    static CALLBACK_STATUS: AtomicI32 = AtomicI32::new(i32::MIN);
    static CALLBACK_RESULT_WAS_NULL: AtomicBool = AtomicBool::new(false);
    static CALLBACK_ERROR_CODE: AtomicI32 = AtomicI32::new(i32::MIN);
    static CALLBACK_ERROR_MESSAGE: std::sync::Mutex<Option<String>> = std::sync::Mutex::new(None);

    unsafe extern "C" fn record_status(_ctx: *mut c_void, status: i32, result: *mut c_void) {
        CALLBACK_RESULT_WAS_NULL.store(result.is_null(), Ordering::SeqCst);
        // The dispatcher installs the error on this (callback) thread's TLS
        // before invoking us; record it exactly as a C consumer reads it:
        // code first, then the (consuming) message read.
        CALLBACK_ERROR_CODE.store(lance_last_error_code() as i32, Ordering::SeqCst);
        let msg_ptr = lance_last_error_message();
        let message = if msg_ptr.is_null() {
            None
        } else {
            let msg = unsafe { CStr::from_ptr(msg_ptr) }
                .to_string_lossy()
                .into_owned();
            unsafe { crate::error::lance_free_string(msg_ptr) };
            Some(msg)
        };
        *CALLBACK_ERROR_MESSAGE.lock().unwrap() = message;
        // Published last: a reader that observes `status` also observes the
        // error fields written above (SeqCst total order).
        CALLBACK_STATUS.store(status, Ordering::SeqCst);
    }

    #[test]
    fn poisoned_scanner_scan_async_dispatches_error_callback() {
        let (_tmp, uri) = create_test_dataset();
        let (dataset, scanner) = open_dataset_and_scanner(&uri);
        poison(scanner);

        unsafe { lance_scanner_scan_async(scanner, record_status, ptr::null_mut()) };
        // The poison error is also visible on the calling thread.
        assert_poison_error_pending();

        // The void entry point reports through the callback on the dispatcher
        // thread; wait for delivery.
        let mut waited_ms = 0;
        while CALLBACK_STATUS.load(Ordering::SeqCst) == i32::MIN && waited_ms < 5000 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            waited_ms += 10;
        }
        assert_eq!(CALLBACK_STATUS.load(Ordering::SeqCst), -1);
        assert!(CALLBACK_RESULT_WAS_NULL.load(Ordering::SeqCst));
        // The callback thread's TLS must carry the poison error too (issue
        // #61): validation-time failures set the caller thread's TLS AND ride
        // inside the dispatch message, which the dispatcher installs before
        // invoking the callback.
        assert_eq!(
            CALLBACK_ERROR_CODE.load(Ordering::SeqCst),
            LanceErrorCode::Panic as i32
        );
        let callback_msg = CALLBACK_ERROR_MESSAGE
            .lock()
            .unwrap()
            .clone()
            .expect("callback-thread TLS must carry the poison message");
        assert!(
            callback_msg.contains("poisoned by an earlier panic"),
            "unexpected callback-thread message: {callback_msg}"
        );

        unsafe {
            lance_scanner_close(scanner);
            lance_dataset_close(dataset);
        }
    }

    #[test]
    fn fresh_scanner_is_not_poisoned() {
        let (_tmp, uri) = create_test_dataset();
        let (dataset, scanner) = open_dataset_and_scanner(&uri);

        let rc = unsafe { lance_scanner_set_limit(scanner, 10) };
        assert_eq!(rc, 0);
        assert_eq!(lance_last_error_code(), LanceErrorCode::Ok);

        // Smoke-test the newly guarded iteration path end to end.
        let mut batches = 0;
        let mut batch: *mut LanceBatch = ptr::null_mut();
        loop {
            let rc = unsafe { lance_scanner_next(scanner, &mut batch) };
            assert!(rc >= 0, "lance_scanner_next failed with {rc}");
            if rc == 1 {
                break;
            }
            assert!(!batch.is_null());
            batches += 1;
            unsafe { crate::lance_batch_free(batch) };
            batch = ptr::null_mut();
        }
        assert!(batches > 0, "dataset must yield at least one batch");

        unsafe {
            lance_scanner_close(scanner);
            lance_dataset_close(dataset);
        }
    }

    /// What the probe callback observed on the dispatcher thread: the
    /// completion status plus the error the dispatcher installed on TLS.
    #[derive(Debug)]
    struct SetupPanicObservation {
        status: i32,
        code: LanceErrorCode,
        message: Option<String>,
    }

    unsafe extern "C" fn record_setup_panic(ctx: *mut c_void, status: i32, _result: *mut c_void) {
        let tx = unsafe { &*(ctx as *const std::sync::mpsc::Sender<SetupPanicObservation>) };
        let code = lance_last_error_code();
        let msg_ptr = lance_last_error_message();
        let message = if msg_ptr.is_null() {
            None
        } else {
            let msg = unsafe { CStr::from_ptr(msg_ptr) }
                .to_string_lossy()
                .into_owned();
            unsafe { crate::error::lance_free_string(msg_ptr) };
            Some(msg)
        };
        let _ = tx.send(SetupPanicObservation {
            status,
            code,
            message,
        });
    }

    /// Regression for the review finding that the spawned task's
    /// `catch_unwind().await` started too late: a panic in *call-time setup*
    /// (validation, `build_scanner`, runtime access, `RT.spawn`) used to
    /// unwind out of the non-unwinding entry point and abort the host
    /// without delivering the promised callback. The entry guard must poison
    /// the handle and dispatch exactly one `LANCE_ERR_PANIC` completion
    /// instead. The setup closure is injected so no production fault hook is
    /// needed. (The panic hook prints to stderr; expected noise.)
    #[test]
    fn scan_async_setup_panic_poisons_and_dispatches_exactly_one_completion() {
        let (_tmp, uri) = create_test_dataset();
        let (dataset, scanner) = open_dataset_and_scanner(&uri);

        let (tx, rx) = std::sync::mpsc::channel::<SetupPanicObservation>();
        let ctx = Box::into_raw(Box::new(tx)) as *mut c_void;

        unsafe {
            scan_async_guarded(scanner, record_setup_panic, ctx, |_, _, _| {
                panic!("injected setup panic")
            });
        }

        assert!(
            unsafe { &*scanner }.is_poisoned(),
            "a setup panic must poison the scanner handle"
        );

        let obs = rx
            .recv_timeout(std::time::Duration::from_secs(5))
            .expect("the entry guard must still deliver the callback");
        assert_eq!(obs.status, -1);
        assert_eq!(obs.code, LanceErrorCode::Panic);
        assert!(
            obs.message
                .as_deref()
                .expect("panic completion must carry a message")
                .contains("injected setup panic"),
            "panic payload must reach the callback, got: {:?}",
            obs.message
        );

        // The contract is exactly ONE completion: no duplicate may follow.
        assert!(
            rx.recv_timeout(std::time::Duration::from_millis(300))
                .is_err(),
            "a setup panic must dispatch exactly one callback completion"
        );

        unsafe {
            drop(Box::from_raw(
                ctx as *mut std::sync::mpsc::Sender<SetupPanicObservation>,
            ));
            lance_scanner_close(scanner);
            lance_dataset_close(dataset);
        }
    }
}
