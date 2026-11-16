// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Raw Lance-file session and object-store operations.

use std::ffi::{CString, c_char};
use std::ptr;
use std::sync::Arc;

use futures::StreamExt;
use lance::session::Session;
use lance_core::utils::path::LancePathExt;
use lance_core::{Error, Result};
use lance_io::object_store::{
    ObjectStore, ObjectStoreParams, ObjectStoreRegistry, StorageOptionsAccessor,
};
use lance_io::scheduler::{ScanScheduler, ScanStats, SchedulerConfig};
use lance_io::traits::Writer;
use object_store::path::Path;
use tokio::io::AsyncWriteExt;

use crate::error::{ffi_try, swallow_unwind};
use crate::helpers::parse_c_string;
use crate::runtime::block_on;
use crate::storage_options::LanceStorageOptionsAccessor;

/// Native scheduler selection.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceSchedulerLiteMode {
    Auto = 0,
    Standard = 1,
    Lite = 2,
}

impl LanceSchedulerLiteMode {
    fn from_raw(raw: i32) -> Result<Self> {
        match raw {
            0 => Ok(Self::Auto),
            1 => Ok(Self::Standard),
            2 => Ok(Self::Lite),
            other => Err(Error::invalid_input(format!(
                "invalid scheduler lite mode {other}; expected 0 (auto), 1 (standard), or 2 (lite)"
            ))),
        }
    }
}

/// Native scheduler configuration.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceSchedulerConfig {
    pub io_buffer_size_bytes: u64,
    pub use_lite_scheduler: i32,
}

/// Native reader or scheduler I/O counters.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LanceScanStats {
    pub iops: u64,
    pub requests: u64,
    pub bytes_read: u64,
}

/// Native object-store registry cache counters.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LanceObjectStoreRegistryStats {
    pub hits: u64,
    pub misses: u64,
    pub active_stores: u64,
}

/// Native object-store I/O counters.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LanceObjectStoreIoStats {
    pub read_iops: u64,
    pub read_bytes: u64,
    pub write_iops: u64,
    pub written_bytes: u64,
}

/// Native object-store capabilities and sizing.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LanceObjectStoreInfo {
    pub block_size: u64,
    pub max_iop_size: u64,
    pub io_parallelism: u64,
    pub is_local: i32,
    pub is_cloud: i32,
    pub prefers_lite_scheduler: i32,
}

pub struct LanceFileSession {
    pub(crate) inner: Arc<FileSessionInner>,
}

pub(crate) struct FileSessionInner {
    pub(crate) session: Option<Arc<Session>>,
    pub(crate) registry: Arc<ObjectStoreRegistry>,
    pub(crate) store: Arc<ObjectStore>,
    pub(crate) base_path: Path,
    pub(crate) scheduler: Arc<ScanScheduler>,
}

/// Owned relative-path list.
pub struct LancePathList {
    paths: Vec<CString>,
}

/// Owned non-recursive listing.
pub struct LanceDelimitedList {
    common_prefixes: Vec<CString>,
    objects: Vec<CString>,
}

/// Owned byte buffer.
pub struct LanceByteBuffer {
    bytes: Vec<u8>,
}

pub(crate) fn storage_options_from_ffi(
    accessor: *const LanceStorageOptionsAccessor,
) -> Option<Arc<StorageOptionsAccessor>> {
    if accessor.is_null() {
        None
    } else {
        unsafe { &*accessor }.inner.clone()
    }
}

pub(crate) fn shared_session_from_ffi(
    session: *const crate::session::LanceSession,
) -> Option<Arc<Session>> {
    if session.is_null() {
        None
    } else {
        Some(unsafe { &*session }.inner.clone())
    }
}

pub(crate) fn open_file_session_inner(
    base_uri: &str,
    storage_options: Option<Arc<StorageOptionsAccessor>>,
    session: Option<Arc<Session>>,
    scheduler_config: *const LanceSchedulerConfig,
) -> Result<FileSessionInner> {
    let registry = session
        .as_ref()
        .map(|session| session.store_registry())
        .unwrap_or_else(|| Arc::new(ObjectStoreRegistry::default()));
    let params = ObjectStoreParams {
        storage_options_accessor: storage_options,
        ..ObjectStoreParams::default()
    };
    let (store, base_path, scheduler) = block_on(async {
        let (store, base_path) =
            ObjectStore::from_uri_and_params(registry.clone(), base_uri, &params).await?;
        let scheduler_config = scheduler_config_from_ffi(scheduler_config, &store)?;
        let scheduler = ScanScheduler::new(store.clone(), scheduler_config);
        Ok::<_, Error>((store, base_path, scheduler))
    })?;
    Ok(FileSessionInner {
        session,
        registry,
        store,
        base_path,
        scheduler,
    })
}

fn scheduler_config_from_ffi(
    config: *const LanceSchedulerConfig,
    store: &ObjectStore,
) -> Result<SchedulerConfig> {
    if config.is_null() {
        return Ok(SchedulerConfig::max_bandwidth(store));
    }
    let config = unsafe { *config };
    let mode = LanceSchedulerLiteMode::from_raw(config.use_lite_scheduler)?;
    let use_lite_scheduler = match mode {
        LanceSchedulerLiteMode::Auto => {
            SchedulerConfig::new(config.io_buffer_size_bytes).use_lite_scheduler
        }
        LanceSchedulerLiteMode::Standard => Some(false),
        LanceSchedulerLiteMode::Lite => Some(true),
    };
    Ok(SchedulerConfig {
        io_buffer_size_bytes: config.io_buffer_size_bytes,
        use_lite_scheduler,
    })
}

pub(crate) fn resolve_session_path(base: &Path, relative: *const c_char) -> Result<Path> {
    let relative = unsafe { parse_c_string(relative)? }
        .ok_or_else(|| Error::invalid_input_source("relative_path must not be NULL".into()))?;
    Ok(base.child_path(&Path::from(relative)))
}

fn required_c_string<'a>(value: *const c_char, field: &str) -> Result<&'a str> {
    unsafe { parse_c_string(value)? }
        .ok_or_else(|| Error::invalid_input_source(format!("{field} must not be NULL").into()))
}

fn relativize(base: &Path, path: &Path) -> Result<String> {
    let parts = path.prefix_match(base).ok_or_else(|| {
        Error::internal(format!(
            "path {:?} does not start with session base path {:?}",
            path.as_ref(),
            base.as_ref()
        ))
    })?;
    Ok(Path::from_iter(parts).as_ref().to_string())
}

fn c_string_vec(paths: impl IntoIterator<Item = String>, context: &str) -> Result<Vec<CString>> {
    paths
        .into_iter()
        .map(|path| {
            CString::new(path).map_err(|error| {
                Error::internal(format!("{context} contains an embedded NUL byte: {error}"))
            })
        })
        .collect()
}

/// Create a raw-file session over a base URI.
///
/// NULL storage options mean no accessor. A non-NULL accessor preserves the
/// native `None`/`Some(empty)` distinction. A NULL shared session uses a fresh
/// default object-store registry. A NULL scheduler configuration uses native
/// `SchedulerConfig::max_bandwidth`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_new(
    base_uri: *const c_char,
    storage_options: *const LanceStorageOptionsAccessor,
    session: *const crate::session::LanceSession,
    scheduler_config: *const LanceSchedulerConfig,
) -> *mut LanceFileSession {
    ffi_try!(
        unsafe { file_session_new_inner(base_uri, storage_options, session, scheduler_config) },
        null
    )
}

unsafe fn file_session_new_inner(
    base_uri: *const c_char,
    storage_options: *const LanceStorageOptionsAccessor,
    session: *const crate::session::LanceSession,
    scheduler_config: *const LanceSchedulerConfig,
) -> Result<*mut LanceFileSession> {
    let base_uri = required_c_string(base_uri, "base_uri")?;
    let inner = open_file_session_inner(
        base_uri,
        storage_options_from_ffi(storage_options),
        shared_session_from_ffi(session),
        scheduler_config,
    )?;
    Ok(Box::into_raw(Box::new(LanceFileSession {
        inner: Arc::new(inner),
    })))
}

/// Close a raw-file session handle.
///
/// Readers, writers, and streams retain their shared native resources.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_close(session: *mut LanceFileSession) {
    if !session.is_null() {
        swallow_unwind("lance_file_session_close", || unsafe {
            let _ = Box::from_raw(session);
        });
    }
}

/// Return the resolved native base path.
///
/// The caller must release the returned string with `lance_free_string()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_base_path(
    session: *const LanceFileSession,
) -> *const c_char {
    ffi_try!(unsafe { file_session_base_path_inner(session) }, null)
}

unsafe fn file_session_base_path_inner(session: *const LanceFileSession) -> Result<*const c_char> {
    if session.is_null() {
        return Err(Error::invalid_input_source(
            "session must not be NULL".into(),
        ));
    }
    CString::new(unsafe { &*session }.inner.base_path.as_ref())
        .map(CString::into_raw)
        .map(|value| value as *const c_char)
        .map_err(|error| Error::internal(format!("base path contains a NUL byte: {error}")))
}

/// Open a reader at a path relative to the session base.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_open_reader(
    session: *const LanceFileSession,
    relative_path: *const c_char,
    options: *const crate::file_reader::LanceFileReaderOptions,
) -> *mut crate::file_reader::LanceFileReader {
    ffi_try!(
        unsafe { file_session_open_reader_inner(session, relative_path, options) },
        null
    )
}

unsafe fn file_session_open_reader_inner(
    session: *const LanceFileSession,
    relative_path: *const c_char,
    options: *const crate::file_reader::LanceFileReaderOptions,
) -> Result<*mut crate::file_reader::LanceFileReader> {
    if session.is_null() {
        return Err(Error::invalid_input_source(
            "session must not be NULL".into(),
        ));
    }
    let session = unsafe { &*session };
    let path = resolve_session_path(&session.inner.base_path, relative_path)?;
    unsafe { crate::file_reader::open_reader_at(&session.inner, path, options) }
}

/// Open a writer at a path relative to the session base.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_open_writer(
    session: *const LanceFileSession,
    relative_path: *const c_char,
    schema: *const arrow::ffi::FFI_ArrowSchema,
    options: *const crate::file_writer::LanceFileWriterOptions,
) -> *mut crate::file_writer::LanceFileWriter {
    ffi_try!(
        unsafe { file_session_open_writer_inner(session, relative_path, schema, options) },
        null
    )
}

unsafe fn file_session_open_writer_inner(
    session: *const LanceFileSession,
    relative_path: *const c_char,
    schema: *const arrow::ffi::FFI_ArrowSchema,
    options: *const crate::file_writer::LanceFileWriterOptions,
) -> Result<*mut crate::file_writer::LanceFileWriter> {
    if session.is_null() {
        return Err(Error::invalid_input_source(
            "session must not be NULL".into(),
        ));
    }
    let session = unsafe { &*session };
    let path = resolve_session_path(&session.inner.base_path, relative_path)?;
    unsafe { crate::file_writer::open_writer_at(&session.inner, path, schema, options) }
}

/// Check whether a path relative to the session base exists.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_contains(
    session: *const LanceFileSession,
    relative_path: *const c_char,
    out_exists: *mut i32,
) -> i32 {
    ffi_try!(
        unsafe { file_session_contains_inner(session, relative_path, out_exists) },
        neg
    )
}

unsafe fn file_session_contains_inner(
    session: *const LanceFileSession,
    relative_path: *const c_char,
    out_exists: *mut i32,
) -> Result<i32> {
    if session.is_null() || out_exists.is_null() {
        return Err(Error::invalid_input_source(
            "session and out_exists must not be NULL".into(),
        ));
    }
    let session = unsafe { &*session };
    let path = resolve_session_path(&session.inner.base_path, relative_path)?;
    let exists = block_on(session.inner.store.exists(&path))?;
    unsafe { ptr::write(out_exists, i32::from(exists)) };
    Ok(0)
}

/// Recursively list objects under a session-relative path.
///
/// A NULL path lists from the session base. Native order is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_list(
    session: *const LanceFileSession,
    relative_path: *const c_char,
) -> *mut LancePathList {
    ffi_try!(
        unsafe { file_session_list_inner(session, relative_path) },
        null
    )
}

unsafe fn file_session_list_inner(
    session: *const LanceFileSession,
    relative_path: *const c_char,
) -> Result<*mut LancePathList> {
    if session.is_null() {
        return Err(Error::invalid_input_source(
            "session must not be NULL".into(),
        ));
    }
    let session = unsafe { &*session };
    let path = if relative_path.is_null() {
        session.inner.base_path.clone()
    } else {
        resolve_session_path(&session.inner.base_path, relative_path)?
    };
    let stream = session.inner.store.list(Some(path));
    let results = block_on(stream.collect::<Vec<_>>());
    let mut paths = Vec::with_capacity(results.len());
    for result in results {
        let metadata = result?;
        paths.push(relativize(&session.inner.base_path, &metadata.location)?);
    }
    Ok(Box::into_raw(Box::new(LancePathList {
        paths: c_string_vec(paths, "listed paths")?,
    })))
}

/// Non-recursively list one session-relative directory level.
///
/// A NULL path lists from the session base. Native order is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_list_with_delimiter(
    session: *const LanceFileSession,
    relative_path: *const c_char,
) -> *mut LanceDelimitedList {
    ffi_try!(
        unsafe { file_session_list_with_delimiter_inner(session, relative_path) },
        null
    )
}

unsafe fn file_session_list_with_delimiter_inner(
    session: *const LanceFileSession,
    relative_path: *const c_char,
) -> Result<*mut LanceDelimitedList> {
    if session.is_null() {
        return Err(Error::invalid_input_source(
            "session must not be NULL".into(),
        ));
    }
    let session = unsafe { &*session };
    let path = if relative_path.is_null() {
        session.inner.base_path.clone()
    } else {
        resolve_session_path(&session.inner.base_path, relative_path)?
    };
    let result = block_on(session.inner.store.list_with_delimiter(Some(&path)))?;
    let common_prefixes = result
        .common_prefixes
        .iter()
        .map(|path| relativize(&session.inner.base_path, path))
        .collect::<Result<Vec<_>>>()?;
    let objects = result
        .objects
        .iter()
        .map(|metadata| relativize(&session.inner.base_path, &metadata.location))
        .collect::<Result<Vec<_>>>()?;
    Ok(Box::into_raw(Box::new(LanceDelimitedList {
        common_prefixes: c_string_vec(common_prefixes, "common prefixes")?,
        objects: c_string_vec(objects, "listed objects")?,
    })))
}

/// Read one byte range from a session-relative object.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_read_range(
    session: *const LanceFileSession,
    relative_path: *const c_char,
    offset: u64,
    length: u64,
) -> *mut LanceByteBuffer {
    ffi_try!(
        unsafe { file_session_read_range_inner(session, relative_path, offset, length) },
        null
    )
}

unsafe fn file_session_read_range_inner(
    session: *const LanceFileSession,
    relative_path: *const c_char,
    offset: u64,
    length: u64,
) -> Result<*mut LanceByteBuffer> {
    if session.is_null() {
        return Err(Error::invalid_input_source(
            "session must not be NULL".into(),
        ));
    }
    let offset = usize::try_from(offset).map_err(|_| {
        Error::invalid_input(format!(
            "offset {offset} does not fit in usize on this platform"
        ))
    })?;
    let length = usize::try_from(length).map_err(|_| {
        Error::invalid_input(format!(
            "length {length} does not fit in usize on this platform"
        ))
    })?;
    let end = offset.checked_add(length).ok_or_else(|| {
        Error::invalid_input(format!(
            "byte range offset={offset} and length={length} overflow usize"
        ))
    })?;
    let session = unsafe { &*session };
    let path = resolve_session_path(&session.inner.base_path, relative_path)?;
    let bytes = block_on(session.inner.store.read_one_range(&path, offset..end))?;
    Ok(Box::into_raw(Box::new(LanceByteBuffer {
        bytes: bytes.to_vec(),
    })))
}

/// Delete a session-relative object. Missing objects preserve native NotFound.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_delete_file(
    session: *const LanceFileSession,
    relative_path: *const c_char,
) -> i32 {
    ffi_try!(
        unsafe { file_session_delete_file_inner(session, relative_path) },
        neg
    )
}

unsafe fn file_session_delete_file_inner(
    session: *const LanceFileSession,
    relative_path: *const c_char,
) -> Result<i32> {
    if session.is_null() {
        return Err(Error::invalid_input_source(
            "session must not be NULL".into(),
        ));
    }
    let session = unsafe { &*session };
    let path = resolve_session_path(&session.inner.base_path, relative_path)?;
    block_on(session.inner.store.delete(&path))?;
    Ok(0)
}

/// Upload a local file to a session-relative object.
///
/// The remote object is created with native object-store semantics. Failure
/// may leave a partially written or uncommitted object; no rollback is claimed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_upload_file(
    session: *const LanceFileSession,
    local_path: *const c_char,
    relative_remote_path: *const c_char,
) -> i32 {
    ffi_try!(
        unsafe { file_session_upload_file_inner(session, local_path, relative_remote_path) },
        neg
    )
}

unsafe fn file_session_upload_file_inner(
    session: *const LanceFileSession,
    local_path: *const c_char,
    relative_remote_path: *const c_char,
) -> Result<i32> {
    if session.is_null() {
        return Err(Error::invalid_input_source(
            "session must not be NULL".into(),
        ));
    }
    let local_path = required_c_string(local_path, "local_path")?;
    let session = unsafe { &*session };
    let remote_path = resolve_session_path(&session.inner.base_path, relative_remote_path)?;
    block_on(async {
        let local_file = tokio::fs::File::open(local_path).await.map_err(|error| {
            Error::io(format!("failed to open local file {local_path:?}: {error}"))
        })?;
        let mut reader = tokio::io::BufReader::new(local_file);
        let mut writer = session.inner.store.create(&remote_path).await?;
        tokio::io::copy(&mut reader, &mut writer)
            .await
            .map_err(|error| {
                Error::io(format!(
                    "failed to upload {local_path:?} to {:?}: {error}",
                    remote_path.as_ref()
                ))
            })?;
        // The pinned LocalWriter can ignore buffered errors in shutdown.
        // A checked flush must succeed before it publishes the destination.
        writer.flush().await.map_err(|error| {
            Error::io(format!(
                "failed to flush upload {local_path:?} to {:?}: {error}",
                remote_path.as_ref()
            ))
        })?;
        Writer::shutdown(writer.as_mut()).await?;
        Ok::<(), Error>(())
    })?;
    Ok(0)
}

/// Download a session-relative object to a local path.
///
/// The native local writer stages bytes and publishes them at shutdown, after
/// the source stream completes. Source/destination aliases are safe to copy.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_download_file(
    session: *const LanceFileSession,
    relative_remote_path: *const c_char,
    local_path: *const c_char,
) -> i32 {
    ffi_try!(
        unsafe { file_session_download_file_inner(session, relative_remote_path, local_path) },
        neg
    )
}

unsafe fn file_session_download_file_inner(
    session: *const LanceFileSession,
    relative_remote_path: *const c_char,
    local_path: *const c_char,
) -> Result<i32> {
    if session.is_null() {
        return Err(Error::invalid_input_source(
            "session must not be NULL".into(),
        ));
    }
    let local_path = required_c_string(local_path, "local_path")?;
    let session = unsafe { &*session };
    let remote_path = resolve_session_path(&session.inner.base_path, relative_remote_path)?;
    block_on(async {
        let reader = session.inner.store.open(&remote_path).await?;
        let mut stream = reader.get_stream().await?;
        let destination = Path::from_absolute_path(std::path::absolute(local_path)?)?;
        let mut file = ObjectStore::local()
            .create(&destination)
            .await
            .map_err(|error| {
                Error::io(format!(
                    "failed to create local file {local_path:?}: {error}"
                ))
            })?;
        while let Some(chunk) = stream.next().await {
            let chunk = chunk?;
            file.write_all(&chunk).await.map_err(|error| {
                Error::io(format!(
                    "failed to write local file {local_path:?}: {error}"
                ))
            })?;
        }
        file.flush().await.map_err(|error| {
            Error::io(format!(
                "failed to flush local file {local_path:?}: {error}"
            ))
        })?;
        Writer::shutdown(file.as_mut()).await.map_err(|error| {
            Error::io(format!(
                "failed to publish local file {local_path:?}: {error}"
            ))
        })?;
        Ok::<(), Error>(())
    })?;
    Ok(0)
}

/// Return native scheduler-wide I/O counters.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_get_scheduler_stats(
    session: *const LanceFileSession,
    out_stats: *mut LanceScanStats,
) -> i32 {
    ffi_try!(
        unsafe { file_session_get_scheduler_stats_inner(session, out_stats) },
        neg
    )
}

unsafe fn file_session_get_scheduler_stats_inner(
    session: *const LanceFileSession,
    out_stats: *mut LanceScanStats,
) -> Result<i32> {
    if session.is_null() || out_stats.is_null() {
        return Err(Error::invalid_input_source(
            "session and out_stats must not be NULL".into(),
        ));
    }
    let stats = unsafe { &*session }.inner.scheduler.stats();
    unsafe { ptr::write(out_stats, scan_stats(stats)) };
    Ok(0)
}

/// Return object-store registry cache counters.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_get_registry_stats(
    session: *const LanceFileSession,
    out_stats: *mut LanceObjectStoreRegistryStats,
) -> i32 {
    ffi_try!(
        unsafe { file_session_get_registry_stats_inner(session, out_stats) },
        neg
    )
}

unsafe fn file_session_get_registry_stats_inner(
    session: *const LanceFileSession,
    out_stats: *mut LanceObjectStoreRegistryStats,
) -> Result<i32> {
    if session.is_null() || out_stats.is_null() {
        return Err(Error::invalid_input_source(
            "session and out_stats must not be NULL".into(),
        ));
    }
    let stats = unsafe { &*session }.inner.registry.stats();
    unsafe {
        ptr::write(
            out_stats,
            LanceObjectStoreRegistryStats {
                hits: stats.hits,
                misses: stats.misses,
                active_stores: stats.active_stores as u64,
            },
        );
    }
    Ok(0)
}

/// Return object-store capabilities.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_get_store_info(
    session: *const LanceFileSession,
    out_info: *mut LanceObjectStoreInfo,
) -> i32 {
    ffi_try!(
        unsafe { file_session_get_store_info_inner(session, out_info) },
        neg
    )
}

unsafe fn file_session_get_store_info_inner(
    session: *const LanceFileSession,
    out_info: *mut LanceObjectStoreInfo,
) -> Result<i32> {
    if session.is_null() || out_info.is_null() {
        return Err(Error::invalid_input_source(
            "session and out_info must not be NULL".into(),
        ));
    }
    let store = &unsafe { &*session }.inner.store;
    unsafe {
        ptr::write(
            out_info,
            LanceObjectStoreInfo {
                block_size: store.block_size() as u64,
                max_iop_size: store.max_iop_size(),
                io_parallelism: store.io_parallelism() as u64,
                is_local: store.is_local() as i32,
                is_cloud: store.is_cloud() as i32,
                prefers_lite_scheduler: store.prefers_lite_scheduler() as i32,
            },
        );
    }
    Ok(0)
}

/// Return native object-store I/O counters.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_session_get_store_io_stats(
    session: *const LanceFileSession,
    out_stats: *mut LanceObjectStoreIoStats,
) -> i32 {
    ffi_try!(
        unsafe { file_session_get_store_io_stats_inner(session, out_stats) },
        neg
    )
}

unsafe fn file_session_get_store_io_stats_inner(
    session: *const LanceFileSession,
    out_stats: *mut LanceObjectStoreIoStats,
) -> Result<i32> {
    if session.is_null() || out_stats.is_null() {
        return Err(Error::invalid_input_source(
            "session and out_stats must not be NULL".into(),
        ));
    }
    let stats = unsafe { &*session }.inner.store.io_stats_snapshot();
    unsafe {
        ptr::write(
            out_stats,
            LanceObjectStoreIoStats {
                read_iops: stats.read_iops,
                read_bytes: stats.read_bytes,
                write_iops: stats.write_iops,
                written_bytes: stats.written_bytes,
            },
        );
    }
    Ok(0)
}

pub(crate) fn scan_stats(stats: ScanStats) -> LanceScanStats {
    LanceScanStats {
        iops: stats.iops,
        requests: stats.requests,
        bytes_read: stats.bytes_read,
    }
}

/// Number of paths in an owned path list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_path_list_count(list: *const LancePathList) -> u64 {
    ffi_try!(unsafe { path_list_count_inner(list) }, 0)
}

unsafe fn path_list_count_inner(list: *const LancePathList) -> Result<u64> {
    if list.is_null() {
        return Err(Error::invalid_input_source("list must not be NULL".into()));
    }
    Ok(unsafe { &*list }.paths.len() as u64)
}

/// Borrowed path at `index`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_path_list_at(
    list: *const LancePathList,
    index: usize,
) -> *const c_char {
    ffi_try!(unsafe { path_list_at_inner(list, index) }, null)
}

unsafe fn path_list_at_inner(list: *const LancePathList, index: usize) -> Result<*const c_char> {
    if list.is_null() {
        return Err(Error::invalid_input_source("list must not be NULL".into()));
    }
    let list = unsafe { &*list };
    let path = list.paths.get(index).ok_or_else(|| {
        Error::invalid_input(format!(
            "path index {index} is out of bounds (count={})",
            list.paths.len()
        ))
    })?;
    Ok(path.as_ptr())
}

/// Close a path list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_path_list_close(list: *mut LancePathList) {
    if !list.is_null() {
        swallow_unwind("lance_path_list_close", || unsafe {
            let _ = Box::from_raw(list);
        });
    }
}

/// Number of common prefixes in a delimited listing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_delimited_list_common_prefix_count(
    list: *const LanceDelimitedList,
) -> u64 {
    ffi_try!(unsafe { delimited_list_common_prefix_count_inner(list) }, 0)
}

unsafe fn delimited_list_common_prefix_count_inner(list: *const LanceDelimitedList) -> Result<u64> {
    if list.is_null() {
        return Err(Error::invalid_input_source("list must not be NULL".into()));
    }
    Ok(unsafe { &*list }.common_prefixes.len() as u64)
}

/// Borrowed common prefix at `index`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_delimited_list_common_prefix_at(
    list: *const LanceDelimitedList,
    index: usize,
) -> *const c_char {
    ffi_try!(
        unsafe { delimited_list_common_prefix_at_inner(list, index) },
        null
    )
}

unsafe fn delimited_list_common_prefix_at_inner(
    list: *const LanceDelimitedList,
    index: usize,
) -> Result<*const c_char> {
    if list.is_null() {
        return Err(Error::invalid_input_source("list must not be NULL".into()));
    }
    let list = unsafe { &*list };
    let prefix = list.common_prefixes.get(index).ok_or_else(|| {
        Error::invalid_input(format!(
            "common prefix index {index} is out of bounds (count={})",
            list.common_prefixes.len()
        ))
    })?;
    Ok(prefix.as_ptr())
}

/// Number of files in a delimited listing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_delimited_list_object_count(list: *const LanceDelimitedList) -> u64 {
    ffi_try!(unsafe { delimited_list_object_count_inner(list) }, 0)
}

unsafe fn delimited_list_object_count_inner(list: *const LanceDelimitedList) -> Result<u64> {
    if list.is_null() {
        return Err(Error::invalid_input_source("list must not be NULL".into()));
    }
    Ok(unsafe { &*list }.objects.len() as u64)
}

/// Borrowed object path at `index`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_delimited_list_object_at(
    list: *const LanceDelimitedList,
    index: usize,
) -> *const c_char {
    ffi_try!(unsafe { delimited_list_object_at_inner(list, index) }, null)
}

unsafe fn delimited_list_object_at_inner(
    list: *const LanceDelimitedList,
    index: usize,
) -> Result<*const c_char> {
    if list.is_null() {
        return Err(Error::invalid_input_source("list must not be NULL".into()));
    }
    let list = unsafe { &*list };
    let object = list.objects.get(index).ok_or_else(|| {
        Error::invalid_input(format!(
            "object index {index} is out of bounds (count={})",
            list.objects.len()
        ))
    })?;
    Ok(object.as_ptr())
}

/// Close a delimited listing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_delimited_list_close(list: *mut LanceDelimitedList) {
    if !list.is_null() {
        swallow_unwind("lance_delimited_list_close", || unsafe {
            let _ = Box::from_raw(list);
        });
    }
}

/// Borrowed byte pointer. Returns NULL for an empty buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_byte_buffer_data(buffer: *const LanceByteBuffer) -> *const u8 {
    ffi_try!(unsafe { byte_buffer_data_inner(buffer) }, null)
}

unsafe fn byte_buffer_data_inner(buffer: *const LanceByteBuffer) -> Result<*const u8> {
    if buffer.is_null() {
        return Err(Error::invalid_input_source(
            "buffer must not be NULL".into(),
        ));
    }
    let bytes = &unsafe { &*buffer }.bytes;
    Ok(if bytes.is_empty() {
        ptr::null()
    } else {
        bytes.as_ptr()
    })
}

/// Number of bytes in the buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_byte_buffer_len(buffer: *const LanceByteBuffer) -> usize {
    ffi_try!(unsafe { byte_buffer_len_inner(buffer) }, 0)
}

unsafe fn byte_buffer_len_inner(buffer: *const LanceByteBuffer) -> Result<usize> {
    if buffer.is_null() {
        return Err(Error::invalid_input_source(
            "buffer must not be NULL".into(),
        ));
    }
    Ok(unsafe { &*buffer }.bytes.len())
}

/// Close a byte buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_byte_buffer_close(buffer: *mut LanceByteBuffer) {
    if !buffer.is_null() {
        swallow_unwind("lance_byte_buffer_close", || unsafe {
            let _ = Box::from_raw(buffer);
        });
    }
}

pub(crate) fn byte_buffer_from_bytes(bytes: impl Into<Vec<u8>>) -> *mut LanceByteBuffer {
    Box::into_raw(Box::new(LanceByteBuffer {
        bytes: bytes.into(),
    }))
}
