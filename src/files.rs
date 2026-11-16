// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Dataset file-enumeration C API.
//!
//! Both entry points export the native Lance streams unchanged. Tracked-file
//! progress is bridged to a C callback whose context is owned until the native
//! emitter releases it, which can happen after the exported stream is
//! released.

use std::ffi::c_void;
use std::ptr;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;

use arrow::ffi_stream::FFI_ArrowArrayStream;
use lance::dataset::files::{TrackedFilesOptions, TrackedFilesProgress};
use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
use crate::runtime::{RT, block_on};
use crate::stream_guard::GuardedReader;

/// Progress update delivered to `LanceTrackedFilesProgressCallback`.
///
/// `manifests_processed` counts completed manifests. `manifests_total` is
/// non-NULL only when native progress reports a known total; the pointed-to
/// value is borrowed for the duration of the callback and must not be retained.
pub type LanceTrackedFilesProgressCallback = Option<
    unsafe extern "C" fn(
        callback_ctx: *mut c_void,
        manifests_processed: u64,
        manifests_total: *const u64,
    ),
>;

/// Release callback for a tracked-files progress context.
///
/// The library calls this exactly once after the native emitter releases the
/// progress closure. It may run on a Tokio worker after the exported stream was
/// released or after this entry point returned an error.
pub type LanceTrackedFilesProgressRelease = Option<unsafe extern "C" fn(callback_ctx: *mut c_void)>;

/// Foreign progress callback and its owned context.
///
/// When this descriptor is passed to `lance_dataset_tracked_files_stream`,
/// both function pointers must be non-NULL and ownership of `callback_ctx`
/// transfers to the library. The library invokes `release(callback_ctx)`
/// exactly once, including on factory errors and early stream release.
/// Callbacks must return normally and must not unwind.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceTrackedFilesProgress {
    pub callback: LanceTrackedFilesProgressCallback,
    pub release: LanceTrackedFilesProgressRelease,
    pub callback_ctx: *mut c_void,
}

/// Options for `lance_dataset_tracked_files_stream`.
///
/// A NULL options pointer uses native defaults. `has_min_version != 0`
/// forwards `min_version`, including zero. `progress == NULL` disables
/// progress reporting.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceTrackedFilesOptions {
    pub has_min_version: i32,
    pub min_version: u64,
    pub progress: *const LanceTrackedFilesProgress,
}

struct OwnedProgressContext {
    callback: unsafe extern "C" fn(*mut c_void, u64, *const u64),
    release: unsafe extern "C" fn(*mut c_void),
    callback_ctx: *mut c_void,
}

// SAFETY: the C ownership contract requires the callback and context to remain
// usable from the native emitter task until `release` is called. Native
// scheduling may invoke progress from a background Tokio worker.
unsafe impl Send for OwnedProgressContext {}
unsafe impl Sync for OwnedProgressContext {}

impl OwnedProgressContext {
    fn notify(&self, update: TrackedFilesProgress) {
        let total = update.manifests_total.map(|value| value as u64);
        unsafe {
            (self.callback)(
                self.callback_ctx,
                update.manifests_processed as u64,
                total
                    .as_ref()
                    .map_or(ptr::null(), |value| value as *const u64),
            )
        };
    }
}

impl Drop for OwnedProgressContext {
    fn drop(&mut self) {
        let release = self.release;
        let callback_ctx = self.callback_ctx;
        swallow_unwind(
            "tracked-files progress release (native emitter teardown)",
            || unsafe {
                release(callback_ctx);
            },
        );
    }
}

/// Export the native tracked-files stream as an Arrow C Data Interface stream.
///
/// This follows `Dataset::tracked_files_with_options` exactly: one row per
/// `(manifest version, referenced file)`, with the native Arrow schema,
/// unspecified row order and optional `min_version` filtering. The stream owns
/// the resources it needs and remains valid after the source dataset is
/// closed.
///
/// On entry with a valid non-NULL `options->progress`, ownership of
/// `callback_ctx` transfers to the library before any other validation.
/// `release(callback_ctx)` is invoked exactly once when the native emitter
/// releases the progress closure, or before this function returns an error.
/// Until that release runs, the callback and context must remain valid. The
/// stream's `release` callback may return before a blocked or in-flight native
/// progress callback finishes.
///
/// `out` is caller-owned and is written only on success. Its non-NULL
/// `release` callback must be invoked exactly once.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_tracked_files_stream(
    dataset: *const LanceDataset,
    options: *const LanceTrackedFilesOptions,
    out: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe { tracked_files_stream_inner(dataset, options, out) },
        neg
    )
}

unsafe fn tracked_files_stream_inner(
    dataset: *const LanceDataset,
    options: *const LanceTrackedFilesOptions,
    out: *mut FFI_ArrowArrayStream,
) -> Result<i32> {
    let mut progress = None;
    let mut min_version = None;
    if !options.is_null() {
        let options = unsafe { &*options };
        if options.has_min_version != 0 {
            min_version = Some(options.min_version);
        }
        if !options.progress.is_null() {
            let descriptor = unsafe { &*options.progress };
            let callback = descriptor.callback.ok_or_else(|| {
                lance_core::Error::invalid_input(
                    "options.progress.callback must not be NULL when progress is provided",
                )
            })?;
            let release = descriptor.release.ok_or_else(|| {
                lance_core::Error::invalid_input(
                    "options.progress.release must not be NULL; it makes callback context ownership observable",
                )
            })?;
            progress = Some(OwnedProgressContext {
                callback,
                release,
                callback_ctx: descriptor.callback_ctx,
            });
        }
    }

    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input("dataset must not be NULL"));
    }
    if out.is_null() {
        return Err(lance_core::Error::invalid_input("out must not be NULL"));
    }

    let progress = progress.map(|context| {
        Box::new(move |update: TrackedFilesProgress| context.notify(update))
            as Box<dyn Fn(TrackedFilesProgress) + Send + Sync>
    });

    let snapshot = unsafe { &*dataset }.snapshot();
    let stream = block_on(snapshot.tracked_files_with_options(TrackedFilesOptions {
        min_version,
        progress,
    }));
    export_stream(stream, out)
}

/// Export the native physical-file listing as an Arrow C Data Interface
/// stream.
///
/// This follows `Dataset::all_files`: it lists the primary dataset root only,
/// with the native Arrow schema and unspecified row order. It does not include
/// files under registered additional base paths and does not filter out
/// untracked files. The stream retains its resources after the source dataset
/// is closed.
///
/// `out` is caller-owned and is written only on success. Its non-NULL
/// `release` callback must be invoked exactly once.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_all_files_stream(
    dataset: *const LanceDataset,
    out: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(unsafe { all_files_stream_inner(dataset, out) }, neg)
}

unsafe fn all_files_stream_inner(
    dataset: *const LanceDataset,
    out: *mut FFI_ArrowArrayStream,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input("dataset must not be NULL"));
    }
    if out.is_null() {
        return Err(lance_core::Error::invalid_input("out must not be NULL"));
    }
    let snapshot = unsafe { &*dataset }.snapshot();
    let stream = block_on(snapshot.all_files());
    export_stream(stream, out)
}

fn export_stream(
    stream: datafusion::execution::SendableRecordBatchStream,
    out: *mut FFI_ArrowArrayStream,
) -> Result<i32> {
    let schema = stream.schema();
    let reader = GuardedReader::new(
        stream,
        schema,
        RT.handle().clone(),
        Arc::new(AtomicBool::new(false)),
    );
    let ffi_stream = FFI_ArrowArrayStream::new(Box::new(reader));
    unsafe {
        ptr::write_unaligned(out, ffi_stream);
    }
    Ok(0)
}
