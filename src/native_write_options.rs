// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Owned dataset-write options backed by native `WriteParams`.
//!
//! The C configuration is copied at construction time. Strings, base lists,
//! per-base storage accessors, the shared session, the commit lock and the
//! progress context all become native owners retained by the opaque handle.
//! No C-side option cache or second conversion layer is kept.

use std::collections::HashMap;
use std::ffi::{c_char, c_void};
use std::fmt;
use std::ptr;
use std::sync::Arc;
use std::time::Duration;

use async_trait::async_trait;
use lance::dataset::progress::{WriteFragmentProgress, WriteProgressFn, WriteStats};
use lance::dataset::{ExternalBlobMode, WriteParams};
use lance_core::Result;
use lance_io::object_store::ObjectStoreParams;
use lance_table::format::Fragment;

use crate::base_paths::{LanceBasePathList, parse_base_path_list};
use crate::callback::{LanceCallbackError, callback_error, release_message_only};
use crate::commit_lock::{LanceCommitLock, commit_handler_from_ffi};
use crate::error::{ffi_try, swallow_unwind};
use crate::file_session::shared_session_from_ffi;
use crate::file_writer::{LanceFileWriterOptions, dataset_file_writer_options_from_ffi};
use crate::session::LanceSession;
use crate::storage_options::{LanceStorageOptionsAccessor, storage_options_accessor_from_ffi};
use crate::transaction_properties::{LanceTransactionProperties, parse_transaction_properties};
use crate::writer::{LanceWriteOptions, apply_write_options};

/// Borrowed list of native `u32` values.
///
/// A NULL pointer maps to native `None`. A non-NULL pointer with `count == 0`
/// maps to native `Some(empty)`. Entries are copied before
/// [`lance_native_write_options_new`] returns.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceUInt32List {
    pub items: *const u32,
    pub count: usize,
}

/// Borrowed list of NUL-terminated UTF-8 strings.
///
/// A NULL pointer maps to native `None`. A non-NULL pointer with `count == 0`
/// maps to native `Some(empty)`. Entries are copied before
/// [`lance_native_write_options_new`] returns.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceStringList {
    pub items: *const *const c_char,
    pub count: usize,
}

/// Per-base native `ObjectStoreParams` values that have a C representation.
///
/// `base_path` is the exact key used by native `WriteParams::base_store_params`.
/// The storage accessor is cloned into the native params. Scalar fields use
/// `has_*` flags so omitted values keep the native `ObjectStoreParams` default.
/// Native object-store and wrapper trait hooks are not representable here.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceBaseStoreParams {
    pub base_path: *const c_char,
    pub storage_options_accessor: *const LanceStorageOptionsAccessor,
    pub has_block_size: i32,
    pub block_size: u64,
    pub has_s3_credentials_refresh_offset: i32,
    pub s3_credentials_refresh_seconds: u64,
    pub s3_credentials_refresh_nanos: u32,
    pub has_use_constant_size_upload_parts: i32,
    pub use_constant_size_upload_parts: bool,
    pub has_list_is_lexically_ordered: i32,
    pub list_is_lexically_ordered: bool,
}

/// Borrowed per-base storage parameter list.
///
/// A NULL pointer maps to native `None`. A non-NULL pointer with `count == 0`
/// maps to native `Some(empty)`. Duplicate `base_path` keys use native
/// `HashMap` last-write-wins semantics.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceBaseStoreParamsList {
    pub items: *const LanceBaseStoreParams,
    pub count: usize,
}

/// Native cumulative write statistics passed to
/// [`LanceNativeWriteProgress::write_stats`].
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LanceWriteStats {
    pub bytes_written: u64,
    pub rows_written: u64,
    pub files_written: u32,
}

/// Native fragment progress payload.
///
/// This value is borrowed for the duration of one callback only.
/// `physical_rows` is present for `complete_fragment` and normally absent for
/// `begin_fragment`, matching the native `Fragment` state at each call.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceWriteFragmentProgressInfo {
    pub fragment_id: u64,
    pub has_physical_rows: i32,
    pub physical_rows: u64,
    pub file_count: u64,
}

/// Report native fragment progress.
///
/// Returning a nonzero status reports failure through `error`; the native
/// write then follows its normal cleanup and no-commit path. The callback may
/// run concurrently from native worker threads and must be thread-safe.
pub type LanceWriteFragmentProgressCallback = Option<
    unsafe extern "C" fn(
        callback_ctx: *mut c_void,
        info: *const LanceWriteFragmentProgressInfo,
        error: *mut LanceCallbackError,
    ) -> i32,
>;

/// Receive cumulative native `WriteStats` after a batch or completed file.
///
/// Native `write_progress` has no error channel. The callback must be cheap,
/// non-blocking, thread-safe, and must not unwind.
pub type LanceWriteStatsCallback =
    Option<unsafe extern "C" fn(callback_ctx: *mut c_void, stats: *const LanceWriteStats)>;

/// Release the progress callback context exactly once.
///
/// The final native progress owner invokes this callback, which may happen
/// after the write entry point returns or while the options handle is being
/// closed. It may run on a native worker thread and must not unwind.
pub type LanceNativeWriteProgressRelease = Option<unsafe extern "C" fn(*mut c_void)>;

/// Foreign dataset-write progress callbacks and their owned context.
///
/// Ownership of `callback_ctx` transfers only when
/// [`lance_native_write_options_new`] succeeds. `release` is required whenever
/// this descriptor is supplied. At least one callback may be omitted; omitted
/// callbacks leave the corresponding native progress hook unchanged.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceNativeWriteProgress {
    pub begin_fragment: LanceWriteFragmentProgressCallback,
    pub complete_fragment: LanceWriteFragmentProgressCallback,
    pub write_stats: LanceWriteStatsCallback,
    pub release: LanceNativeWriteProgressRelease,
    pub callback_ctx: *mut c_void,
}

/// Borrowed configuration for one owned native write-options handle.
///
/// Every pointer is read only for the duration of
/// [`lance_native_write_options_new`]; all values, strings and native owners
/// are copied or cloned before it returns. Presence flags preserve native
/// `None` versus `Some(empty)` and explicit `false` values.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceNativeWriteOptionsSpec {
    /// Existing shape/auto-cleanup settings. NULL selects native defaults.
    pub write_options: *const LanceWriteOptions,
    /// NULL selects native `None`; non-NULL count zero selects `Some(empty)`.
    pub transaction_properties: *const LanceTransactionProperties,
    pub has_enable_v2_manifest_paths: i32,
    pub enable_v2_manifest_paths: bool,
    /// NULL selects native `None`; non-NULL count zero selects `Some(empty)`.
    pub initial_bases: *const LanceBasePathList,
    /// NULL selects native `None`; non-NULL count zero selects `Some(empty)`.
    pub target_bases: *const LanceUInt32List,
    /// NULL selects native `None`; non-NULL count zero selects `Some(empty)`.
    pub target_base_names_or_paths: *const LanceStringList,
    pub has_target_all_bases: i32,
    pub target_all_bases: bool,
    /// NULL selects native `None`; non-NULL count zero selects `Some(empty)`.
    pub base_store_params: *const LanceBaseStoreParamsList,
    pub allow_external_blob_outside_bases: bool,
    /// `0` selects `Reference`; `1` selects `Ingest`.
    pub external_blob_mode: i32,
    pub has_blob_pack_file_size_threshold: i32,
    pub blob_pack_file_size_threshold: u64,
    /// Existing raw-file writer options. `version` must stay NULL; dataset
    /// file-version selection belongs in `write_options.params`.
    pub file_writer_options: *const LanceFileWriterOptions,
    pub session: *const LanceSession,
    pub storage_options_accessor: *const LanceStorageOptionsAccessor,
    pub commit_lock: *const LanceCommitLock,
    pub progress: *const LanceNativeWriteProgress,
}

/// Opaque owner of one native `WriteParams` plus callback and owner Arcs.
pub struct LanceNativeWriteOptions {
    params: WriteParams,
    _progress: Option<Arc<OwnedNativeWriteProgress>>,
}

impl LanceNativeWriteOptions {
    /// Clone the native params for one write call. The handle stays immutable.
    pub(crate) fn params_clone(&self) -> WriteParams {
        self.params.clone()
    }
}

struct OwnedNativeWriteProgress {
    begin_fragment: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const LanceWriteFragmentProgressInfo,
            *mut LanceCallbackError,
        ) -> i32,
    >,
    complete_fragment: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const LanceWriteFragmentProgressInfo,
            *mut LanceCallbackError,
        ) -> i32,
    >,
    write_stats: Option<unsafe extern "C" fn(*mut c_void, *const LanceWriteStats)>,
    release: unsafe extern "C" fn(*mut c_void),
    callback_ctx: *mut c_void,
}

// SAFETY: the foreign contract requires callbacks and the context to remain
// usable from native worker threads until `release` runs exactly once.
unsafe impl Send for OwnedNativeWriteProgress {}
unsafe impl Sync for OwnedNativeWriteProgress {}

impl fmt::Debug for OwnedNativeWriteProgress {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("OwnedNativeWriteProgress")
            .field("has_begin_fragment", &self.begin_fragment.is_some())
            .field("has_complete_fragment", &self.complete_fragment.is_some())
            .field("has_write_stats", &self.write_stats.is_some())
            .finish_non_exhaustive()
    }
}

impl Drop for OwnedNativeWriteProgress {
    fn drop(&mut self) {
        let release = self.release;
        let callback_ctx = self.callback_ctx;
        swallow_unwind("native write progress release", || unsafe {
            release(callback_ctx);
        });
    }
}

impl OwnedNativeWriteProgress {
    fn fragment_info(fragment: &Fragment) -> LanceWriteFragmentProgressInfo {
        LanceWriteFragmentProgressInfo {
            fragment_id: fragment.id,
            has_physical_rows: i32::from(fragment.physical_rows.is_some()),
            physical_rows: fragment.physical_rows.unwrap_or_default() as u64,
            file_count: fragment.files.len() as u64,
        }
    }

    fn call_fragment(
        &self,
        callback: unsafe extern "C" fn(
            *mut c_void,
            *const LanceWriteFragmentProgressInfo,
            *mut LanceCallbackError,
        ) -> i32,
        fragment: &Fragment,
        context: &str,
    ) -> Result<()> {
        let info = Self::fragment_info(fragment);
        let mut error = LanceCallbackError::empty();
        // SAFETY: the callback and context live until the final Arc drops.
        let status = tokio::task::block_in_place(|| unsafe {
            callback(self.callback_ctx, &info, &mut error)
        });
        if status != 0 {
            return Err(callback_error(&mut error, status, context));
        }
        unsafe { release_message_only(&mut error) };
        Ok(())
    }

    fn write_progress_fn(self: &Arc<Self>) -> Option<WriteProgressFn> {
        let callback = self.write_stats?;
        let progress = Arc::clone(self);
        Some(WriteProgressFn::new(move |stats: WriteStats| {
            let info = LanceWriteStats {
                bytes_written: stats.bytes_written,
                rows_written: stats.rows_written,
                files_written: stats.files_written,
            };
            // SAFETY: the callback and context live until the final Arc drops.
            tokio::task::block_in_place(|| unsafe {
                callback(progress.callback_ctx, &info);
            });
        }))
    }
}

#[async_trait]
impl WriteFragmentProgress for OwnedNativeWriteProgress {
    async fn begin(&self, fragment: &Fragment) -> Result<()> {
        let Some(callback) = self.begin_fragment else {
            return Ok(());
        };
        self.call_fragment(callback, fragment, "write fragment begin")
    }

    async fn complete(&self, fragment: &Fragment) -> Result<()> {
        let Some(callback) = self.complete_fragment else {
            return Ok(());
        };
        self.call_fragment(callback, fragment, "write fragment complete")
    }
}

unsafe fn parse_u32_list(list: *const LanceUInt32List) -> Result<Option<Vec<u32>>> {
    if list.is_null() {
        return Ok(None);
    }
    let list = unsafe { &*list };
    if list.count > 0 && list.items.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "target_bases.items must not be NULL when count = {}",
            list.count
        )));
    }
    if list.count > isize::MAX as usize / std::mem::size_of::<u32>() {
        return Err(lance_core::Error::invalid_input(format!(
            "target_bases.count {} exceeds the maximum addressable u32 slice length",
            list.count
        )));
    }
    let values = if list.count == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(list.items, list.count) }.to_vec()
    };
    Ok(Some(values))
}

unsafe fn parse_string_list(list: *const LanceStringList) -> Result<Option<Vec<String>>> {
    if list.is_null() {
        return Ok(None);
    }
    let list = unsafe { &*list };
    if list.count > 0 && list.items.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "target_base_names_or_paths.items must not be NULL when count = {}",
            list.count
        )));
    }
    if list.count > isize::MAX as usize / std::mem::size_of::<*const c_char>() {
        return Err(lance_core::Error::invalid_input(format!(
            "target_base_names_or_paths.count {} exceeds the maximum addressable pointer slice length",
            list.count
        )));
    }

    let mut values = Vec::with_capacity(list.count);
    for index in 0..list.count {
        let value = unsafe { *list.items.add(index) };
        let value = unsafe { crate::helpers::parse_c_string(value) }.map_err(|error| {
            lance_core::Error::invalid_input(format!(
                "target_base_names_or_paths.items[{index}]: {error}"
            ))
        })?;
        let value = value.ok_or_else(|| {
            lance_core::Error::invalid_input(format!(
                "target_base_names_or_paths.items[{index}] must not be NULL"
            ))
        })?;
        values.push(value.to_owned());
    }
    Ok(Some(values))
}

fn narrow_u64(value: u64, field: &'static str) -> Result<usize> {
    usize::try_from(value).map_err(|_| {
        lance_core::Error::invalid_input(format!(
            "{field}={value} exceeds usize::MAX on this target"
        ))
    })
}

unsafe fn parse_base_store_params(
    list: *const LanceBaseStoreParamsList,
) -> Result<Option<HashMap<String, ObjectStoreParams>>> {
    if list.is_null() {
        return Ok(None);
    }
    let list = unsafe { &*list };
    if list.count > 0 && list.items.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "base_store_params.items must not be NULL when count = {}",
            list.count
        )));
    }
    if list.count > isize::MAX as usize / std::mem::size_of::<LanceBaseStoreParams>() {
        return Err(lance_core::Error::invalid_input(format!(
            "base_store_params.count {} exceeds the maximum addressable slice length",
            list.count
        )));
    }

    let mut params = HashMap::with_capacity(list.count);
    for index in 0..list.count {
        let entry = unsafe { &*list.items.add(index) };
        let base_path = unsafe { crate::helpers::parse_c_string(entry.base_path) }
            .map_err(|error| {
                lance_core::Error::invalid_input(format!(
                    "base_store_params.items[{index}].base_path: {error}"
                ))
            })?
            .ok_or_else(|| {
                lance_core::Error::invalid_input(format!(
                    "base_store_params.items[{index}].base_path must not be NULL"
                ))
            })?;

        let mut native = ObjectStoreParams::default();
        if let Some(accessor) =
            unsafe { storage_options_accessor_from_ffi(entry.storage_options_accessor) }?
        {
            native.storage_options_accessor = Some(accessor);
        }
        if entry.has_block_size != 0 {
            native.block_size = Some(narrow_u64(
                entry.block_size,
                "base_store_params.block_size",
            )?);
        }
        if entry.has_s3_credentials_refresh_offset != 0 {
            if entry.s3_credentials_refresh_nanos >= 1_000_000_000 {
                return Err(lance_core::Error::invalid_input(format!(
                    "base_store_params.items[{index}].s3_credentials_refresh_nanos={} must be less than 1_000_000_000",
                    entry.s3_credentials_refresh_nanos
                )));
            }
            native.s3_credentials_refresh_offset = Duration::new(
                entry.s3_credentials_refresh_seconds,
                entry.s3_credentials_refresh_nanos,
            );
        }
        if entry.has_use_constant_size_upload_parts != 0 {
            native.use_constant_size_upload_parts = entry.use_constant_size_upload_parts;
        }
        if entry.has_list_is_lexically_ordered != 0 {
            native.list_is_lexically_ordered = Some(entry.list_is_lexically_ordered);
        }
        params.insert(base_path.to_owned(), native);
    }
    Ok(Some(params))
}

fn external_blob_mode_from_ffi(mode: i32) -> Result<ExternalBlobMode> {
    match mode {
        0 => Ok(ExternalBlobMode::Reference),
        1 => Ok(ExternalBlobMode::Ingest),
        other => Err(lance_core::Error::invalid_input(format!(
            "external_blob_mode={other} is invalid; expected 0 (reference) or 1 (ingest)"
        ))),
    }
}

unsafe fn parse_progress(
    progress: *const LanceNativeWriteProgress,
) -> Result<Option<Arc<OwnedNativeWriteProgress>>> {
    if progress.is_null() {
        return Ok(None);
    }
    let progress = unsafe { &*progress };
    let release = progress.release.ok_or_else(|| {
        lance_core::Error::invalid_input(
            "progress.release must not be NULL when progress is provided",
        )
    })?;
    Ok(Some(Arc::new(OwnedNativeWriteProgress {
        begin_fragment: progress.begin_fragment,
        complete_fragment: progress.complete_fragment,
        write_stats: progress.write_stats,
        release,
        callback_ctx: progress.callback_ctx,
    })))
}

unsafe fn native_write_options_new_inner(
    spec: *const LanceNativeWriteOptionsSpec,
) -> Result<*mut LanceNativeWriteOptions> {
    let mut params = WriteParams::default();
    if spec.is_null() {
        return Ok(Box::into_raw(Box::new(LanceNativeWriteOptions {
            params,
            _progress: None,
        })));
    }
    let spec = unsafe { &*spec };

    if !spec.write_options.is_null() {
        unsafe { apply_write_options(&mut params, spec.write_options)? };
    }
    if spec.has_enable_v2_manifest_paths != 0 {
        params.enable_v2_manifest_paths = spec.enable_v2_manifest_paths;
    }
    if !spec.initial_bases.is_null() {
        params.initial_bases = unsafe { parse_base_path_list(spec.initial_bases)? };
    }
    if !spec.target_bases.is_null() {
        params.target_bases = unsafe { parse_u32_list(spec.target_bases)? };
    }
    if !spec.target_base_names_or_paths.is_null() {
        params.target_base_names_or_paths =
            unsafe { parse_string_list(spec.target_base_names_or_paths)? };
    }
    if spec.has_target_all_bases != 0 {
        params.target_all_bases = Some(spec.target_all_bases);
    }
    if !spec.base_store_params.is_null() {
        params.base_store_params = unsafe { parse_base_store_params(spec.base_store_params)? };
    }
    params.allow_external_blob_outside_bases = spec.allow_external_blob_outside_bases;
    params.external_blob_mode = external_blob_mode_from_ffi(spec.external_blob_mode)?;
    if spec.has_blob_pack_file_size_threshold != 0 {
        params.blob_pack_file_size_threshold = Some(narrow_u64(
            spec.blob_pack_file_size_threshold,
            "blob_pack_file_size_threshold",
        )?);
    }
    params.file_writer_options =
        unsafe { dataset_file_writer_options_from_ffi(spec.file_writer_options)? };
    if !spec.transaction_properties.is_null() {
        params.transaction_properties = Some(Arc::new(unsafe {
            parse_transaction_properties(&*spec.transaction_properties)?
        }));
    }
    params.session = shared_session_from_ffi(spec.session);
    if let Some(accessor) =
        unsafe { storage_options_accessor_from_ffi(spec.storage_options_accessor) }?
    {
        params.store_params = Some(ObjectStoreParams {
            storage_options_accessor: Some(accessor),
            ..ObjectStoreParams::default()
        });
    }
    params.commit_handler = commit_handler_from_ffi(spec.commit_lock);

    // Progress is parsed last so an invalid descriptor cannot transfer context
    // ownership before the rest of the specification has been accepted.
    let progress = unsafe { parse_progress(spec.progress)? };
    if let Some(progress) = &progress {
        if progress.begin_fragment.is_some() || progress.complete_fragment.is_some() {
            params.progress = progress.clone();
        }
        if let Some(write_progress) = progress.write_progress_fn() {
            params.write_progress = Some(write_progress);
        }
    }

    Ok(Box::into_raw(Box::new(LanceNativeWriteOptions {
        params,
        _progress: progress,
    })))
}

/// Create an owned native-backed dataset write-options handle.
///
/// A NULL `spec` selects `WriteParams::default()`. Otherwise every referenced
/// value is copied or cloned before this call returns. `mode` is supplied by
/// the later write entry point, not by this handle.
///
/// Ownership of `spec->progress->callback_ctx` transfers only on success. On
/// failure the caller retains it and `release` is not called. On success the
/// context is released exactly once when the final native owner drops; that
/// can happen inside `lance_native_write_options_close` or later on a native
/// worker thread. Close the handle only after all calls using it have
/// returned.
///
/// Native validation of option combinations remains in Lance and is applied
/// by the write entry point. This constructor only rejects values that cannot
/// be represented or parsed at the C boundary.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_native_write_options_new(
    spec: *const LanceNativeWriteOptionsSpec,
) -> *mut LanceNativeWriteOptions {
    ffi_try!(unsafe { native_write_options_new_inner(spec) }, null)
}

/// Close an owned native write-options handle.
///
/// Native writes clone the values they need. Close the handle only after all
/// calls that borrowed it have returned. Closing drops the retained session,
/// storage accessors, commit lock and progress context; the progress release
/// callback may run from this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_native_write_options_close(options: *mut LanceNativeWriteOptions) {
    if options.is_null() {
        return;
    }
    swallow_unwind("lance_native_write_options_close", || unsafe {
        let _ = Box::from_raw(options);
    });
}
