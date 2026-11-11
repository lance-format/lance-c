// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Uncommitted Arrow writes and native transaction publication.
//!
//! This module is deliberately a foreign-language bridge only: writer and
//! commit eligibility, conflict resolution, retries, detached versions,
//! cleanup and normalization stay in the selected Lance core.

use std::ffi::c_char;
use std::sync::{Arc, RwLock};
use std::time::Duration;

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use arrow::record_batch::RecordBatchReader;
use arrow_schema::Schema as ArrowSchema;
use lance::Dataset;
use lance::dataset::{CommitBuilder, InsertBuilder, WriteParams};
use lance::session::Session;
use lance_core::Result;
use lance_io::object_store::{ObjectStoreParams, StorageOptionsAccessor};
use lance_table::io::commit::CommitHandler;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::block_on;
use crate::session::LanceSession;
use crate::transaction::LanceTransaction;
use crate::writer::{
    LanceWriteMode, LanceWriteOptions, parse_storage_version, write_params_from_ffi,
};

/// Native write options for an uncommitted Arrow stream.
///
/// `write` reuses `LanceWriteOptions`, including its native-defaults mapping.
/// `transaction_properties` is the native `WriteParams::transaction_properties`
/// option: NULL means `None`; a non-NULL pointer, including an array whose
/// first entry is NULL, means `Some` and therefore distinguishes an explicitly
/// empty map from an omitted one.
#[repr(C)]
pub struct LanceUncommittedWriteOptions {
    pub write: LanceWriteOptions,
    pub transaction_properties: *const *const c_char,
}

/// Tri-state mapping for `CommitBuilder::with_timeout`.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceCommitTimeoutMode {
    /// Keep the native default (30 minutes).
    Default = 0,
    /// Pass `None`, disabling the overall commit timeout.
    Disabled = 1,
    /// Pass the explicit seconds/nanoseconds value, including zero.
    Explicit = 2,
}

/// Native `CommitBuilder` settings for this delivery.
///
/// Zero-initialize this struct to keep native defaults. Presence flags keep
/// omitted values distinct from explicit `false`, zero retries, zero retry
/// timeout and zero overall timeout.
#[repr(C)]
pub struct LanceCommitOptions {
    /// Apply `CommitBuilder::use_stable_row_ids` when true.
    pub has_use_stable_row_ids: bool,
    pub use_stable_row_ids: bool,
    /// Apply `CommitBuilder::enable_v2_manifest_paths` when true.
    pub has_enable_v2_manifest_paths: bool,
    pub enable_v2_manifest_paths: bool,
    /// Apply `CommitBuilder::with_max_retries` when true. Zero is a real
    /// native value and selects strict overwrite behavior when applicable.
    pub has_max_retries: bool,
    pub max_retries: u32,
    /// Apply `CommitBuilder::with_retry_timeout` when true. Zero is valid.
    pub has_retry_timeout: bool,
    pub retry_timeout_seconds: u64,
    pub retry_timeout_nanos: u32,
    /// Fixed-width `LanceCommitTimeoutMode` discriminant.
    pub timeout_mode: i32,
    pub timeout_seconds: u64,
    pub timeout_nanos: u32,
    /// Apply `CommitBuilder::with_detached`.
    pub detached: bool,
    /// Apply `CommitBuilder::with_skip_auto_cleanup`.
    pub skip_auto_cleanup: bool,
    /// Optional `CommitBuilder::with_storage_format` value. NULL keeps the
    /// native `None` default. Parsed with the same version-string conversion
    /// as `LanceWriteParams::data_storage_version`.
    pub data_storage_version: *const c_char,
}

fn duration_from_parts(seconds: u64, nanos: u32, label: &str) -> Result<Duration> {
    if nanos >= 1_000_000_000 {
        return Err(lance_core::Error::invalid_input_source(
            format!("{label}_nanos={nanos} must be less than 1_000_000_000").into(),
        ));
    }
    Ok(Duration::new(seconds, nanos))
}

fn storage_options_from_ffi(
    storage_opts: *const *const c_char,
) -> Result<Option<ObjectStoreParams>> {
    let opts = unsafe { helpers::parse_storage_options(storage_opts)? };
    Ok((!opts.is_empty()).then(|| ObjectStoreParams {
        storage_options_accessor: Some(Arc::new(StorageOptionsAccessor::with_static_options(opts))),
        ..ObjectStoreParams::default()
    }))
}

fn session_from_ffi(session: *const LanceSession) -> Option<Arc<Session>> {
    if session.is_null() {
        None
    } else {
        Some(unsafe { &*session }.inner.clone())
    }
}

unsafe fn commit_builder_from_ffi<'a>(
    uri: &'a str,
    options: *const LanceCommitOptions,
    storage_opts: *const *const c_char,
    session: *const LanceSession,
) -> Result<CommitBuilder<'a>> {
    unsafe { commit_builder_from_ffi_with_handler(uri, None, options, storage_opts, session) }
}

/// Build a `CommitBuilder` with an optional native commit handler.
///
/// Namespace URI commits supply `ExternalManifestCommitHandler`; all other
/// conversions stay exactly the same as the plain URI entry points.
pub(crate) unsafe fn commit_builder_from_ffi_with_handler<'a>(
    uri: &'a str,
    commit_handler: Option<Arc<dyn CommitHandler>>,
    options: *const LanceCommitOptions,
    storage_opts: *const *const c_char,
    session: *const LanceSession,
) -> Result<CommitBuilder<'a>> {
    let mut builder = CommitBuilder::new(uri);
    if let Some(commit_handler) = commit_handler {
        builder = builder.with_commit_handler(commit_handler);
    }
    if let Some(store_params) = storage_options_from_ffi(storage_opts)? {
        builder = builder.with_store_params(store_params);
    }
    if let Some(session) = session_from_ffi(session) {
        builder = builder.with_session(session);
    }
    if options.is_null() {
        return Ok(builder);
    }

    // SAFETY: a non-NULL `options` points to a valid `LanceCommitOptions` for
    // the duration of this synchronous builder construction.
    let options = unsafe { &*options };
    if options.has_use_stable_row_ids {
        builder = builder.use_stable_row_ids(options.use_stable_row_ids);
    }
    if options.has_enable_v2_manifest_paths {
        builder = builder.enable_v2_manifest_paths(options.enable_v2_manifest_paths);
    }
    if options.has_max_retries {
        builder = builder.with_max_retries(options.max_retries);
    }
    if options.has_retry_timeout {
        builder = builder.with_retry_timeout(duration_from_parts(
            options.retry_timeout_seconds,
            options.retry_timeout_nanos,
            "retry_timeout",
        )?);
    }
    match options.timeout_mode {
        0 => {}
        1 => builder = builder.with_timeout(None),
        2 => {
            builder = builder.with_timeout(Some(duration_from_parts(
                options.timeout_seconds,
                options.timeout_nanos,
                "timeout",
            )?));
        }
        other => {
            return Err(lance_core::Error::invalid_input_source(
                format!(
                    "timeout_mode={other} is invalid; expected 0 (default), 1 (disabled), or 2 (explicit)"
                )
                .into(),
            ));
        }
    }
    if options.detached {
        builder = builder.with_detached(true);
    }
    if options.skip_auto_cleanup {
        builder = builder.with_skip_auto_cleanup(true);
    }
    if !options.data_storage_version.is_null() {
        let version =
            unsafe { parse_storage_version(options.data_storage_version, "data_storage_version")? }
                .expect("non-NULL data_storage_version must parse as Some");
        builder = builder.with_storage_format(version);
    }
    Ok(builder)
}

fn writer_params_from_ffi(
    mode: LanceWriteMode,
    options: *const LanceUncommittedWriteOptions,
    storage_opts: *const *const c_char,
    session: *const LanceSession,
) -> Result<WriteParams> {
    let (write_options, transaction_properties) = if options.is_null() {
        (std::ptr::null(), None)
    } else {
        // SAFETY: the caller guarantees a non-NULL `options` is valid for this
        // call. All referenced values are copied before this function returns.
        let options = unsafe { &*options };
        let properties = unsafe {
            helpers::parse_optional_c_string_map(
                options.transaction_properties,
                "transaction properties",
            )?
        };
        (&options.write as *const LanceWriteOptions, properties)
    };

    let mut params = unsafe {
        write_params_from_ffi(
            mode,
            std::ptr::null(),
            write_options,
            false,
            storage_opts,
            session_from_ffi(session),
        )?
    };
    params.transaction_properties = transaction_properties.map(Arc::new);
    Ok(params)
}

/// Write an Arrow stream into native uncommitted data files and return the
/// owned transaction that would publish them.
///
/// This maps to `InsertBuilder::execute_uncommitted_stream`. The stream is
/// consumed on every return path except a NULL stream pointer. `schema` is
/// borrowed and must be released by the caller; the stream schema must match.
/// `options` may be NULL for native defaults. `storage_opts` and `session`
/// have the same meanings as the committing writer entry points.
///
/// On success `*out_transaction` receives an independent handle. On error it
/// is unchanged, and any data files already written remain uncommitted; Lance
/// exposes no binding-side rollback or cleanup policy for them.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn lance_dataset_write_uncommitted_stream(
    uri: *const c_char,
    schema: *const FFI_ArrowSchema,
    stream: *mut FFI_ArrowArrayStream,
    mode: i32,
    options: *const LanceUncommittedWriteOptions,
    storage_opts: *const *const c_char,
    session: *const LanceSession,
    out_transaction: *mut *mut LanceTransaction,
) -> i32 {
    ffi_try!(
        unsafe {
            write_uncommitted_stream_inner(
                uri,
                schema,
                stream,
                mode,
                options,
                storage_opts,
                session,
                out_transaction,
            )
        },
        neg
    )
}

#[allow(clippy::too_many_arguments)]
unsafe fn write_uncommitted_stream_inner(
    uri: *const c_char,
    schema: *const FFI_ArrowSchema,
    stream: *mut FFI_ArrowArrayStream,
    mode: i32,
    options: *const LanceUncommittedWriteOptions,
    storage_opts: *const *const c_char,
    session: *const LanceSession,
    out_transaction: *mut *mut LanceTransaction,
) -> Result<i32> {
    if stream.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "stream must not be NULL".into(),
        ));
    }
    // SAFETY: the stream is non-NULL and the caller guarantees a valid,
    // initialized Arrow stream. `from_raw` transfers release ownership.
    let reader = unsafe { ArrowArrayStreamReader::from_raw(stream) }
        .map_err(|error| lance_core::Error::invalid_input_source(error.to_string().into()))?;

    if uri.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "uri must not be NULL".into(),
        ));
    }
    if schema.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "schema must not be NULL".into(),
        ));
    }
    if out_transaction.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_transaction must not be NULL".into(),
        ));
    }

    let mode = LanceWriteMode::from_raw(mode)?;
    let uri = unsafe { helpers::parse_c_string(uri)? }
        .filter(|uri| !uri.is_empty())
        .ok_or_else(|| lance_core::Error::invalid_input("uri must not be empty"))?;
    let expected_schema = ArrowSchema::try_from(unsafe { &*schema }).map_err(|error| {
        lance_core::Error::invalid_input_source(format!("invalid schema: {error}").into())
    })?;
    let params = writer_params_from_ffi(mode, options, storage_opts, session)?;

    let stream_schema = reader.schema();
    if stream_schema.fields() != expected_schema.fields() {
        return Err(lance_core::Error::invalid_input_source(format!(
            "stream schema does not match the provided schema.\n  expected: {expected_schema}\n  got:      {stream_schema}"
        )
        .into()));
    }

    let transaction = block_on(
        InsertBuilder::new(uri)
            .with_params(&params)
            .execute_uncommitted_stream(reader),
    )?;
    let transaction = Box::new(LanceTransaction { inner: transaction });
    unsafe {
        std::ptr::write(out_transaction, Box::into_raw(transaction));
    }
    Ok(0)
}

pub(crate) fn dataset_handle(dataset: Dataset) -> LanceDataset {
    LanceDataset {
        inner: RwLock::new(Arc::new(dataset)),
    }
}

/// Publish one native transaction through `CommitBuilder::execute`.
///
/// `transaction` is borrowed and cloned; its handle remains valid. The
/// destination is the URI, with the supplied storage options and session, so
/// create, append and overwrite transactions follow the same native resolution
/// path. `options` may be NULL for native commit defaults.
///
/// On success `*out_dataset` receives an independently owned dataset at the
/// committed version. On error it is unchanged. Commit errors are not rolled
/// back by this binding: uncommitted files may remain, and an I/O error can
/// leave the published version uncertain. Reopen the URI to establish the
/// actual committed state before retrying.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_transaction_commit(
    uri: *const c_char,
    transaction: *const LanceTransaction,
    options: *const LanceCommitOptions,
    storage_opts: *const *const c_char,
    session: *const LanceSession,
    out_dataset: *mut *mut LanceDataset,
) -> i32 {
    ffi_try!(
        unsafe {
            transaction_commit_inner(
                uri,
                transaction,
                options,
                storage_opts,
                session,
                out_dataset,
            )
        },
        neg
    )
}

#[allow(clippy::too_many_arguments)]
unsafe fn transaction_commit_inner(
    uri: *const c_char,
    transaction: *const LanceTransaction,
    options: *const LanceCommitOptions,
    storage_opts: *const *const c_char,
    session: *const LanceSession,
    out_dataset: *mut *mut LanceDataset,
) -> Result<i32> {
    if transaction.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "transaction must not be NULL".into(),
        ));
    }
    if out_dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_dataset must not be NULL".into(),
        ));
    }
    let uri = unsafe { helpers::parse_c_string(uri)? }
        .filter(|uri| !uri.is_empty())
        .ok_or_else(|| lance_core::Error::invalid_input("uri must not be empty"))?;

    let transaction = unsafe { &*transaction }.inner.clone();
    let builder = unsafe { commit_builder_from_ffi(uri, options, storage_opts, session)? };
    let dataset = block_on(builder.execute(transaction))?;
    let handle = Box::new(dataset_handle(dataset));
    unsafe {
        std::ptr::write(out_dataset, Box::into_raw(handle));
    }
    Ok(0)
}

/// Commit a batch of native append transactions through
/// `CommitBuilder::execute_batch`.
///
/// Batch eligibility, merge order, conflict handling and the merged read
/// version are native. Non-append batches return `LANCE_ERR_NOT_SUPPORTED`;
/// an empty batch returns the native invalid-input error. The output dataset
/// and the actual merged transaction are independently owned and are written
/// only on success. Both output pointers are required.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn lance_transaction_commit_batch(
    uri: *const c_char,
    transactions: *const *const LanceTransaction,
    transaction_count: usize,
    options: *const LanceCommitOptions,
    storage_opts: *const *const c_char,
    session: *const LanceSession,
    out_dataset: *mut *mut LanceDataset,
    out_merged_transaction: *mut *mut LanceTransaction,
) -> i32 {
    ffi_try!(
        unsafe {
            transaction_commit_batch_inner(
                uri,
                transactions,
                transaction_count,
                options,
                storage_opts,
                session,
                out_dataset,
                out_merged_transaction,
            )
        },
        neg
    )
}

#[allow(clippy::too_many_arguments)]
unsafe fn transaction_commit_batch_inner(
    uri: *const c_char,
    transactions: *const *const LanceTransaction,
    transaction_count: usize,
    options: *const LanceCommitOptions,
    storage_opts: *const *const c_char,
    session: *const LanceSession,
    out_dataset: *mut *mut LanceDataset,
    out_merged_transaction: *mut *mut LanceTransaction,
) -> Result<i32> {
    if out_dataset.is_null() || out_merged_transaction.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_dataset and out_merged_transaction must not be NULL".into(),
        ));
    }
    if transaction_count > 0 && transactions.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            format!("transactions must not be NULL when transaction_count = {transaction_count}")
                .into(),
        ));
    }
    let uri = unsafe { helpers::parse_c_string(uri)? }
        .filter(|uri| !uri.is_empty())
        .ok_or_else(|| lance_core::Error::invalid_input("uri must not be empty"))?;

    let transaction_slice = if transaction_count == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(transactions, transaction_count) }
    };
    let mut native_transactions = Vec::with_capacity(transaction_count);
    for (index, transaction) in transaction_slice.iter().enumerate() {
        if transaction.is_null() {
            return Err(lance_core::Error::invalid_input_source(
                format!("transactions[{index}] must not be NULL").into(),
            ));
        }
        native_transactions.push(unsafe { &**transaction }.inner.clone());
    }

    let builder = unsafe { commit_builder_from_ffi(uri, options, storage_opts, session)? };
    let result = block_on(builder.execute_batch(native_transactions))?;
    let dataset = Box::new(dataset_handle(result.dataset));
    let merged = Box::new(LanceTransaction {
        inner: result.merged,
    });
    let dataset = Box::into_raw(dataset);
    let merged = Box::into_raw(merged);
    unsafe {
        std::ptr::write(out_dataset, dataset);
        std::ptr::write(out_merged_transaction, merged);
    }
    Ok(0)
}
