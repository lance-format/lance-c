// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Host-provided random-access reads for query-engine integration.
//!
//! A read provider is deliberately independent from [`crate::LanceSession`].
//! Sessions may be shared across queries to cache portable metadata and index
//! state, while each dataset binds the provider that owns its current storage
//! credentials, cancellation state, data-cache policy, and I/O statistics.

use std::collections::HashMap;
use std::ffi::{CStr, CString, c_char, c_void};
use std::fmt::{Debug, Display, Formatter};
use std::ops::Range;
use std::sync::{Arc, Mutex, Weak};

use async_trait::async_trait;
use bytes::Bytes;
use futures::StreamExt;
use futures::stream::{self, BoxStream};
use lance_core::Result;
use lance_io::object_store::WrappingObjectStore;
use object_store::path::Path;
use object_store::{
    CopyOptions, GetOptions, GetResult, GetResultPayload, ListResult, MultipartUpload, ObjectMeta,
    ObjectStore, PutMultipartOptions, PutOptions, PutPayload, PutResult,
    Result as ObjectStoreResult,
};

use crate::error::{ffi_try, swallow_unwind};

/// The host callback completed successfully.
pub const LANCE_READ_OK: i32 = 0;
/// The provider does not handle this object; use Lance's native object store.
pub const LANCE_READ_NOT_SUPPORTED: i32 = 1;
/// The requested object does not exist.
pub const LANCE_READ_NOT_FOUND: i32 = 2;
/// The request was cancelled by the host.
pub const LANCE_READ_CANCELLED: i32 = 3;
/// The provider encountered an I/O error.
pub const LANCE_READ_IO_ERROR: i32 = 4;

/// Stable identity of an object passed to the host's `open` callback.
///
/// All string pointers are borrowed and remain valid only for the duration of
/// the callback. `path` is relative to `store_prefix`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceFileIdentity {
    pub store_prefix: *const c_char,
    pub path: *const c_char,
    pub size: u64,
    pub last_modified_millis: i64,
    pub e_tag: *const c_char,
    pub version: *const c_char,
}

/// C callbacks used by a host-provided random-access reader.
///
/// `open` and `read_at` may be called concurrently from blocking worker
/// threads. `close_reader` is called exactly once for every reader returned by
/// a successful `open`. When supplied, `destroy_context` is called exactly once
/// when the last provider reference is released. Callbacks must not unwind
/// across the C ABI.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceReadProviderOps {
    pub open: Option<
        unsafe extern "C" fn(
            context: *mut c_void,
            identity: *const LanceFileIdentity,
            out_reader: *mut *mut c_void,
        ) -> i32,
    >,
    pub read_at: Option<
        unsafe extern "C" fn(
            reader: *mut c_void,
            offset: u64,
            buffer: *mut u8,
            length: u64,
            bytes_read: *mut u64,
        ) -> i32,
    >,
    pub close_reader: Option<unsafe extern "C" fn(reader: *mut c_void)>,
    pub destroy_context: Option<unsafe extern "C" fn(context: *mut c_void)>,
    /// Return the current thread's last host error. The returned string is
    /// borrowed; lance-c copies it immediately.
    pub last_error_message: Option<unsafe extern "C" fn(context: *mut c_void) -> *const c_char>,
}

/// Opaque, reference-counted host read provider.
pub struct LanceReadProvider {
    pub(crate) inner: Arc<ForeignReadProvider>,
}

/// Create a host read provider.
///
/// Lance's I/O scheduler controls the number of concurrent reads. The provider
/// owns `context` after this function succeeds.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_read_provider_new(
    ops: *const LanceReadProviderOps,
    context: *mut c_void,
) -> *mut LanceReadProvider {
    ffi_try!(unsafe { read_provider_new_inner(ops, context) }, null)
}

unsafe fn read_provider_new_inner(
    ops: *const LanceReadProviderOps,
    context: *mut c_void,
) -> Result<*mut LanceReadProvider> {
    if ops.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "ops must not be NULL".into(),
        ));
    }
    let ops = unsafe { *ops };
    if ops.open.is_none() || ops.read_at.is_none() || ops.close_reader.is_none() {
        return Err(lance_core::Error::invalid_input_source(
            "ops.open, ops.read_at, and ops.close_reader must not be NULL".into(),
        ));
    }

    Ok(Box::into_raw(Box::new(LanceReadProvider {
        inner: Arc::new(ForeignReadProvider {
            ops,
            context: context as usize,
        }),
    })))
}

/// Close a provider handle. Datasets already opened with the provider retain
/// shared ownership and remain valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_read_provider_close(provider: *mut LanceReadProvider) {
    if !provider.is_null() {
        swallow_unwind("lance_read_provider_close", || unsafe {
            let _ = Box::from_raw(provider);
        });
    }
}

pub(crate) struct ForeignReadProvider {
    ops: LanceReadProviderOps,
    context: usize,
}

impl Debug for ForeignReadProvider {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> std::fmt::Result {
        formatter.debug_struct("ForeignReadProvider").finish()
    }
}

impl Drop for ForeignReadProvider {
    fn drop(&mut self) {
        if let Some(destroy_context) = self.ops.destroy_context {
            unsafe { destroy_context(self.context as *mut c_void) };
        }
    }
}

impl ForeignReadProvider {
    fn error_message(&self, operation: &str, status: i32) -> String {
        let host_message = self.ops.last_error_message.and_then(|last_error_message| {
            let message = unsafe { last_error_message(self.context as *mut c_void) };
            if message.is_null() {
                None
            } else {
                Some(
                    unsafe { CStr::from_ptr(message) }
                        .to_string_lossy()
                        .into_owned(),
                )
            }
        });
        host_message.unwrap_or_else(|| format!("host {operation} failed with status {status}"))
    }

    fn error(&self, operation: &str, path: &Path, status: i32) -> object_store::Error {
        let message = self.error_message(operation, status);
        let source = || Box::new(std::io::Error::other(message.clone()));
        match status {
            LANCE_READ_NOT_FOUND => object_store::Error::NotFound {
                path: path.to_string(),
                source: source(),
            },
            LANCE_READ_NOT_SUPPORTED => object_store::Error::NotSupported { source: source() },
            _ => object_store::Error::Generic {
                store: "host read provider",
                source: source(),
            },
        }
    }

    async fn open_file(
        self: &Arc<Self>,
        store_prefix: Arc<str>,
        path: Path,
        meta: ObjectMeta,
        attributes: object_store::Attributes,
    ) -> ObjectStoreResult<Option<Arc<HostFile>>> {
        let provider = Arc::clone(self);
        let callback = self.ops.open.expect("validated at provider construction");
        let callback_path = path.clone();
        let callback_meta = meta.clone();

        tokio::task::spawn_blocking(move || {
            let store_prefix = CString::new(store_prefix.as_ref()).map_err(|error| {
                object_store::Error::Generic {
                    store: "host read provider",
                    source: Box::new(error),
                }
            })?;
            let path_string = CString::new(callback_path.as_ref()).map_err(|error| {
                object_store::Error::Generic {
                    store: "host read provider",
                    source: Box::new(error),
                }
            })?;
            let e_tag = callback_meta
                .e_tag
                .as_deref()
                .map(CString::new)
                .transpose()
                .map_err(|error| object_store::Error::Generic {
                    store: "host read provider",
                    source: Box::new(error),
                })?;
            let version = callback_meta
                .version
                .as_deref()
                .map(CString::new)
                .transpose()
                .map_err(|error| object_store::Error::Generic {
                    store: "host read provider",
                    source: Box::new(error),
                })?;
            let identity = LanceFileIdentity {
                store_prefix: store_prefix.as_ptr(),
                path: path_string.as_ptr(),
                size: callback_meta.size,
                last_modified_millis: callback_meta.last_modified.timestamp_millis(),
                e_tag: e_tag
                    .as_ref()
                    .map_or(std::ptr::null(), |value| value.as_ptr()),
                version: version
                    .as_ref()
                    .map_or(std::ptr::null(), |value| value.as_ptr()),
            };
            let mut reader = std::ptr::null_mut();
            let status =
                unsafe { callback(provider.context as *mut c_void, &identity, &mut reader) };
            if status == LANCE_READ_NOT_SUPPORTED {
                return Ok(None);
            }
            if status != LANCE_READ_OK {
                return Err(provider.error("open", &callback_path, status));
            }
            if reader.is_null() {
                return Err(object_store::Error::Generic {
                    store: "host read provider",
                    source: Box::new(std::io::Error::other(
                        "host open returned success with a NULL reader",
                    )),
                });
            }

            Ok(Some(Arc::new(HostFile {
                provider,
                reader: reader as usize,
                meta,
                attributes,
            })))
        })
        .await
        .map_err(|error| object_store::Error::Generic {
            store: "host read provider",
            source: Box::new(error),
        })?
    }
}

#[derive(Debug)]
pub(crate) struct ProviderWrapper {
    // ObjectStoreRegistry stores ObjectStoreParams as map keys. Keeping only a
    // Weak reference here prevents a long-lived Session registry entry from
    // retaining a Dataset-scoped provider after the wrapped store is gone.
    provider: Weak<ForeignReadProvider>,
}

impl ProviderWrapper {
    pub(crate) fn new(provider: &Arc<ForeignReadProvider>) -> Self {
        Self {
            provider: Arc::downgrade(provider),
        }
    }
}

impl WrappingObjectStore for ProviderWrapper {
    fn wrap(&self, store_prefix: &str, original: Arc<dyn ObjectStore>) -> Arc<dyn ObjectStore> {
        let Some(provider) = self.provider.upgrade() else {
            return original;
        };
        Arc::new(HostReadObjectStore {
            provider,
            original,
            store_prefix: Arc::from(store_prefix),
            files: Arc::new(Mutex::new(HashMap::new())),
        })
    }
}

#[derive(Debug, Hash, PartialEq, Eq)]
struct FileKey {
    path: Path,
    requested_version: Option<String>,
}

#[derive(Debug)]
struct HostReadObjectStore {
    provider: Arc<ForeignReadProvider>,
    original: Arc<dyn ObjectStore>,
    store_prefix: Arc<str>,
    files: Arc<Mutex<HashMap<FileKey, Arc<HostFile>>>>,
}

impl Display for HostReadObjectStore {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "HostReadObjectStore({})", self.store_prefix)
    }
}

impl HostReadObjectStore {
    fn cached_file(&self, key: &FileKey) -> Option<Arc<HostFile>> {
        self.files
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .get(key)
            .cloned()
    }

    fn cache_file(&self, key: FileKey, file: &Arc<HostFile>) {
        self.files
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .insert(key, Arc::clone(file));
    }

    fn invalidate(&self, location: &Path) {
        self.files
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .retain(|key, _| key.path != *location);
    }

    async fn file(
        &self,
        location: &Path,
        options: &GetOptions,
    ) -> ObjectStoreResult<Option<Arc<HostFile>>> {
        let key = FileKey {
            path: location.clone(),
            requested_version: options.version.clone(),
        };
        if let Some(file) = self.cached_file(&key) {
            options.check_preconditions(&file.meta)?;
            return Ok(Some(file));
        }

        let mut head_options = options.clone();
        head_options.head = true;
        head_options.range = None;
        let head = self.original.get_opts(location, head_options).await?;
        options.check_preconditions(&head.meta)?;
        let file = self
            .provider
            .open_file(
                Arc::clone(&self.store_prefix),
                location.clone(),
                head.meta,
                head.attributes,
            )
            .await?;
        if let Some(file) = &file {
            self.cache_file(key, file);
        }
        Ok(file)
    }
}

#[derive(Debug)]
struct HostFile {
    provider: Arc<ForeignReadProvider>,
    reader: usize,
    meta: ObjectMeta,
    attributes: object_store::Attributes,
}

impl HostFile {
    async fn read(self: &Arc<Self>, range: Range<u64>) -> ObjectStoreResult<Bytes> {
        let length = range.end - range.start;
        if length == 0 {
            return Ok(Bytes::new());
        }
        let length_usize =
            usize::try_from(length).map_err(|error| object_store::Error::Generic {
                store: "host read provider",
                source: Box::new(error),
            })?;
        let file = Arc::clone(self);
        let callback = self
            .provider
            .ops
            .read_at
            .expect("validated at provider construction");
        tokio::task::spawn_blocking(move || {
            let mut buffer = vec![0_u8; length_usize];
            let mut bytes_read = 0_u64;
            let status = unsafe {
                callback(
                    file.reader as *mut c_void,
                    range.start,
                    buffer.as_mut_ptr(),
                    length,
                    &mut bytes_read,
                )
            };
            if status != LANCE_READ_OK {
                return Err(file.provider.error("read_at", &file.meta.location, status));
            }
            if bytes_read != length {
                return Err(object_store::Error::Generic {
                    store: "host read provider",
                    source: Box::new(std::io::Error::new(
                        std::io::ErrorKind::UnexpectedEof,
                        format!(
                            "host read_at returned {bytes_read} bytes for requested range {}..{}",
                            range.start, range.end
                        ),
                    )),
                });
            }
            Ok(Bytes::from(buffer))
        })
        .await
        .map_err(|error| object_store::Error::Generic {
            store: "host read provider",
            source: Box::new(error),
        })?
    }
}

impl Drop for HostFile {
    fn drop(&mut self) {
        let close_reader = self
            .provider
            .ops
            .close_reader
            .expect("validated at provider construction");
        unsafe { close_reader(self.reader as *mut c_void) };
    }
}

#[async_trait]
impl ObjectStore for HostReadObjectStore {
    async fn put_opts(
        &self,
        location: &Path,
        payload: PutPayload,
        options: PutOptions,
    ) -> ObjectStoreResult<PutResult> {
        self.invalidate(location);
        self.original.put_opts(location, payload, options).await
    }

    async fn put_multipart_opts(
        &self,
        location: &Path,
        options: PutMultipartOptions,
    ) -> ObjectStoreResult<Box<dyn MultipartUpload>> {
        self.invalidate(location);
        self.original.put_multipart_opts(location, options).await
    }

    async fn get_opts(&self, location: &Path, options: GetOptions) -> ObjectStoreResult<GetResult> {
        if options.head {
            return self.original.get_opts(location, options).await;
        }
        let Some(file) = self.file(location, &options).await? else {
            return self.original.get_opts(location, options).await;
        };
        let range = options
            .range
            .as_ref()
            .map(|range| range.as_range(file.meta.size))
            .transpose()
            .map_err(|error| object_store::Error::Generic {
                store: "host read provider",
                source: Box::new(error),
            })?
            .unwrap_or(0..file.meta.size);
        let bytes = file.read(range.clone()).await?;
        let payload = stream::once(async move { Ok(bytes) }).boxed();
        Ok(GetResult {
            payload: GetResultPayload::Stream(payload),
            meta: file.meta.clone(),
            range,
            attributes: file.attributes.clone(),
        })
    }

    fn delete_stream(
        &self,
        locations: BoxStream<'static, ObjectStoreResult<Path>>,
    ) -> BoxStream<'static, ObjectStoreResult<Path>> {
        let files = Arc::clone(&self.files);
        self.original
            .delete_stream(locations)
            .map(move |result| {
                if let Ok(location) = &result {
                    files
                        .lock()
                        .unwrap_or_else(|error| error.into_inner())
                        .retain(|key, _| key.path != *location);
                }
                result
            })
            .boxed()
    }

    fn list(&self, prefix: Option<&Path>) -> BoxStream<'static, ObjectStoreResult<ObjectMeta>> {
        self.original.list(prefix)
    }

    fn list_with_offset(
        &self,
        prefix: Option<&Path>,
        offset: &Path,
    ) -> BoxStream<'static, ObjectStoreResult<ObjectMeta>> {
        self.original.list_with_offset(prefix, offset)
    }

    async fn list_with_delimiter(&self, prefix: Option<&Path>) -> ObjectStoreResult<ListResult> {
        self.original.list_with_delimiter(prefix).await
    }

    async fn copy_opts(
        &self,
        from: &Path,
        to: &Path,
        options: CopyOptions,
    ) -> ObjectStoreResult<()> {
        self.invalidate(to);
        self.original.copy_opts(from, to, options).await
    }
}
