// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Native Lance namespace C API.
//!
//! This module is a foreign-language bridge to `lance_namespace::LanceNamespace`.
//! Namespace routing, managed versioning, retries, credential refresh, REST
//! serialization and operation semantics stay in the native namespace crates.
//! The C ABI transports native request/response models as JSON and keeps ARM
//! IPC payloads as separate byte buffers.
//!
//! Native namespace errors are reported twice: the returned `LanceErrorCode`
//! is the generic C error class, while `LanceNamespaceResult` preserves the
//! native `NamespaceError` code and message. In particular native
//! `Unsupported` is code 0 and must be treated as a failed call, never as
//! `LANCE_OK`.

use std::collections::HashMap;
use std::ffi::{CString, c_char, c_void};
use std::fmt;
use std::ptr;
use std::sync::Arc;

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use arrow::record_batch::RecordBatchReader;
use arrow_schema::Schema as ArrowSchema;
use async_trait::async_trait;
use bytes::Bytes;
use lance::Dataset;
use lance::dataset::WriteParams;
use lance::dataset::builder::DatasetBuilder;
use lance_core::{Error, Result};
use lance_io::object_store::{LanceNamespaceStorageOptionsProvider, StorageOptionsProvider};
use lance_namespace::compat::merge_insert_request_from_json;
use lance_namespace::models::*;
use lance_namespace::{LanceNamespace as NativeLanceNamespace, NamespaceError};
use lance_namespace_impls::rest_adapter::{RestAdapter, RestAdapterConfig, RestAdapterHandle};
use lance_namespace_impls::{
    ConnectBuilder, DirectoryNamespace, DirectoryNamespaceBuilder, DynamicContextProvider,
    OperationInfo, RestNamespace, RestNamespaceBuilder,
};
use lance_table::io::commit::CommitHandler;
use lance_table::io::commit::external_manifest::ExternalManifestCommitHandler;
use serde::Serialize;
use serde::de::DeserializeOwned;
use serde_json::Value;

use crate::callback::{
    LanceCallbackError, LanceOwnedString, release_message_only, take_owned_string,
};
use crate::commit::{commit_builder_from_ffi_with_handler, dataset_handle};
use crate::dataset::LanceDataset;
use crate::error::{LanceErrorCode, ffi_guard_with, swallow_unwind};
use crate::helpers::parse_c_string;
use crate::runtime::block_on;
use crate::session::LanceSession;
use crate::storage_options::LanceStorageOptionsProvider;
use crate::writer::{LanceWriteMode, LanceWriteOptions, write_params_from_ffi};
use lance::io::commit::namespace_manifest::LanceNamespaceExternalManifestStore;

/// Opaque namespace handle. Concrete directory/REST variants retain the native
/// implementation so metrics retrieval and reset use its native counters.
pub struct LanceNamespace {
    inner: NamespaceKind,
}

enum NamespaceKind {
    Directory(Arc<DirectoryNamespace>),
    Rest(Arc<RestNamespace>),
    Connected(Arc<dyn NativeLanceNamespace>),
    Custom(Arc<CustomNamespace>),
}

impl LanceNamespace {
    fn trait_object(&self) -> Arc<dyn NativeLanceNamespace> {
        match &self.inner {
            NamespaceKind::Directory(ns) => ns.clone(),
            NamespaceKind::Rest(ns) => ns.clone(),
            NamespaceKind::Connected(ns) => ns.clone(),
            NamespaceKind::Custom(ns) => ns.clone(),
        }
    }

    fn id(&self) -> String {
        match &self.inner {
            NamespaceKind::Directory(ns) => ns.namespace_id(),
            NamespaceKind::Rest(ns) => ns.namespace_id(),
            NamespaceKind::Connected(ns) => ns.namespace_id(),
            NamespaceKind::Custom(ns) => ns.id.clone(),
        }
    }

    fn metrics(&self) -> Result<HashMap<String, u64>> {
        match &self.inner {
            NamespaceKind::Directory(ns) => Ok(ns.retrieve_ops_metrics()),
            NamespaceKind::Rest(ns) => Ok(ns.retrieve_ops_metrics()),
            NamespaceKind::Connected(_) | NamespaceKind::Custom(_) => {
                Err(Error::not_supported_source(
                    "this namespace has no native operation metrics".into(),
                ))
            }
        }
    }

    fn reset_metrics(&self) -> Result<()> {
        match &self.inner {
            NamespaceKind::Directory(ns) => {
                ns.reset_ops_metrics();
                Ok(())
            }
            NamespaceKind::Rest(ns) => {
                ns.reset_ops_metrics();
                Ok(())
            }
            NamespaceKind::Connected(_) | NamespaceKind::Custom(_) => {
                Err(Error::not_supported_source(
                    "this namespace has no native operation metrics".into(),
                ))
            }
        }
    }
}

/// One native namespace operation result.
///
/// Zero-initialize before the first call. `result_json` is a JSON encoding of
/// the native response model, except for unit operations (NULL), count
/// (JSON number), explain/analyze (JSON string), and `query_table` (NULL JSON
/// plus `result_bytes`, which hold the native Arrow IPC stream).
///
/// `namespace_error_code` and `namespace_error_message` are set only when
/// `has_namespace_error` is nonzero. They preserve the native
/// `NamespaceError` even when the generic C error code only exposes a broader
/// class such as internal or invalid argument. Native `Unsupported` has code 0;
/// the call still fails and the generic code is never `LANCE_OK`.
///
/// Release the result exactly once with `lance_namespace_result_close`.
#[repr(C)]
#[derive(Default)]
pub struct LanceNamespaceResult {
    pub result_json: *mut c_char,
    pub result_bytes: *mut u8,
    pub result_bytes_len: usize,
    pub has_namespace_error: i32,
    pub namespace_error_code: u32,
    pub namespace_error_message: *mut c_char,
}

/// Release every owned field and zero the structure.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_namespace_result_close(result: *mut LanceNamespaceResult) {
    if result.is_null() {
        return;
    }
    swallow_unwind("lance_namespace_result_close", || unsafe {
        let result = &mut *result;
        if !result.result_json.is_null() {
            drop(CString::from_raw(result.result_json));
            result.result_json = ptr::null_mut();
        }
        if !result.result_bytes.is_null() {
            let slice = ptr::slice_from_raw_parts_mut(result.result_bytes, result.result_bytes_len);
            drop(Box::from_raw(slice));
            result.result_bytes = ptr::null_mut();
            result.result_bytes_len = 0;
        }
        if !result.namespace_error_message.is_null() {
            drop(CString::from_raw(result.namespace_error_message));
            result.namespace_error_message = ptr::null_mut();
        }
        result.has_namespace_error = 0;
        result.namespace_error_code = 0;
    });
}

/// Read a copied namespace identifier. Free with `lance_free_string`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_namespace_id(namespace: *const LanceNamespace) -> *mut c_char {
    ffi_guard_with(
        || {
            let namespace = unsafe { namespace.as_ref() }
                .ok_or_else(|| Error::invalid_input_source("namespace must not be NULL".into()))?;
            Ok(alloc_c_string(&namespace.id())?)
        },
        |_| ptr::null_mut(),
    )
}

/// Retrieve the namespace's native operation counters as a JSON object.
///
/// Directory and REST handles expose their native counters. Connected and
/// custom handles have no native metrics and return `LANCE_ERR_NOT_SUPPORTED`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_namespace_retrieve_ops_metrics(
    namespace: *const LanceNamespace,
) -> *mut c_char {
    ffi_guard_with(
        || {
            let namespace = unsafe { namespace.as_ref() }
                .ok_or_else(|| Error::invalid_input_source("namespace must not be NULL".into()))?;
            Ok(alloc_c_string(&serde_json::to_string(
                &namespace.metrics()?,
            )?)?)
        },
        |_| ptr::null_mut(),
    )
}

/// Reset the namespace's native operation counters.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_namespace_reset_ops_metrics(
    namespace: *const LanceNamespace,
) -> i32 {
    ffi_guard_with(
        || {
            let namespace = unsafe { namespace.as_ref() }
                .ok_or_else(|| Error::invalid_input_source("namespace must not be NULL".into()))?;
            namespace.reset_metrics()?;
            Ok(0)
        },
        |_| -1,
    )
}

/// Read the provider identity used by native provider equality and refresh.
///
/// The returned string is copied and must be freed with `lance_free_string`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_storage_options_provider_id(
    provider: *const LanceStorageOptionsProvider,
) -> *mut c_char {
    ffi_guard_with(
        || {
            let provider = unsafe { provider.as_ref() }
                .ok_or_else(|| Error::invalid_input_source("provider must not be NULL".into()))?;
            CString::new(provider.inner.provider_id())
                .map(CString::into_raw)
                .map_err(|_| Error::invalid_input("provider id contains an interior NUL byte"))
        },
        |_| ptr::null_mut(),
    )
}

unsafe fn properties_from_json(
    json: *const c_char,
    label: &str,
) -> Result<HashMap<String, String>> {
    let text = unsafe { parse_c_string(json)? }.unwrap_or_default();
    if text.trim().is_empty() {
        return Ok(HashMap::new());
    }
    serde_json::from_str(text).map_err(|error| {
        Error::invalid_input_source(
            format!("{label} must be a JSON object of strings: {error}").into(),
        )
    })
}

unsafe fn table_id_from_json(json: *const c_char) -> Result<Vec<String>> {
    let text = unsafe { parse_c_string(json)? }
        .ok_or_else(|| Error::invalid_input_source("table_id_json must not be NULL".into()))?;
    let table_id: Vec<String> = serde_json::from_str(text).map_err(|error| {
        Error::invalid_input_source(
            format!("table_id_json must be a JSON array of strings: {error}").into(),
        )
    })?;
    if table_id.is_empty() {
        return Err(Error::invalid_input_source(
            "table_id_json must contain at least one identifier part".into(),
        ));
    }
    Ok(table_id)
}

fn alloc_c_string(text: &str) -> Result<*mut c_char> {
    CString::new(text)
        .map(CString::into_raw)
        .map_err(|_| Error::invalid_input("result contains an interior NUL byte"))
}

/// Create a directory namespace from native `DirectoryNamespaceBuilder`.
///
/// `properties_json` is a JSON object of string properties; NULL or an empty
/// string means the native default property set. `session` may be NULL. The
/// optional context-provider spec is consumed on every call: the library owns
/// its context and releases it exactly once, including on construction failure.
/// Native property validation and defaults are left to the builder.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_namespace_directory_new(
    properties_json: *const c_char,
    session: *const LanceSession,
    context_provider: *const LanceNamespaceContextProviderSpec,
) -> *mut LanceNamespace {
    ffi_guard_with(
        || unsafe { directory_new_inner(properties_json, session, context_provider) },
        |_| ptr::null_mut(),
    )
}

unsafe fn directory_new_inner(
    properties_json: *const c_char,
    session: *const LanceSession,
    context_provider: *const LanceNamespaceContextProviderSpec,
) -> Result<*mut LanceNamespace> {
    let provider = unsafe { context_provider_from_spec(context_provider)? };
    let properties = unsafe { properties_from_json(properties_json, "properties_json")? };
    let session = unsafe { session.as_ref() }.map(|session| session.inner.clone());
    let mut builder = DirectoryNamespaceBuilder::from_properties(properties, session)?;
    if let Some(provider) = provider {
        builder = builder.context_provider(provider);
    }
    let namespace = block_on(builder.build())?;
    Ok(Box::into_raw(Box::new(LanceNamespace {
        inner: NamespaceKind::Directory(Arc::new(namespace)),
    })))
}

/// Create a REST namespace from native `RestNamespaceBuilder`.
///
/// Properties use the native `uri`, `delimiter`, `header.*`, `tls.*` and
/// `ops_metrics_enabled` keys. TLS and validation behavior are native. The
/// optional context-provider spec has the same ownership contract as the
/// directory constructor.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_namespace_rest_new(
    properties_json: *const c_char,
    context_provider: *const LanceNamespaceContextProviderSpec,
) -> *mut LanceNamespace {
    ffi_guard_with(
        || unsafe { rest_new_inner(properties_json, context_provider) },
        |_| ptr::null_mut(),
    )
}

unsafe fn rest_new_inner(
    properties_json: *const c_char,
    context_provider: *const LanceNamespaceContextProviderSpec,
) -> Result<*mut LanceNamespace> {
    let provider = unsafe { context_provider_from_spec(context_provider)? };
    let properties = unsafe { properties_from_json(properties_json, "properties_json")? };
    let mut builder = RestNamespaceBuilder::from_properties(properties)?;
    if let Some(provider) = provider {
        builder = builder.context_provider(provider);
    }
    Ok(Box::into_raw(Box::new(LanceNamespace {
        inner: NamespaceKind::Rest(Arc::new(builder.build())),
    })))
}

/// Connect through native `ConnectBuilder` (for example `dir` or `rest`).
///
/// Connected handles deliberately keep the native trait object and therefore
/// do not expose concrete-implementation metrics.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_namespace_connect_new(
    impl_name: *const c_char,
    properties_json: *const c_char,
    session: *const LanceSession,
    context_provider: *const LanceNamespaceContextProviderSpec,
) -> *mut LanceNamespace {
    ffi_guard_with(
        || unsafe { connect_new_inner(impl_name, properties_json, session, context_provider) },
        |_| ptr::null_mut(),
    )
}

unsafe fn connect_new_inner(
    impl_name: *const c_char,
    properties_json: *const c_char,
    session: *const LanceSession,
    context_provider: *const LanceNamespaceContextProviderSpec,
) -> Result<*mut LanceNamespace> {
    let provider = unsafe { context_provider_from_spec(context_provider)? };
    let impl_name = unsafe { parse_c_string(impl_name)? }
        .filter(|name| !name.is_empty())
        .ok_or_else(|| Error::invalid_input("impl_name must not be empty"))?;
    let properties = unsafe { properties_from_json(properties_json, "properties_json")? };
    let mut builder = ConnectBuilder::new(impl_name).properties(properties);
    if let Some(session) = unsafe { session.as_ref() } {
        builder = builder.session(session.inner.clone());
    }
    if let Some(provider) = provider {
        builder = builder.context_provider(provider);
    }
    let namespace = block_on(builder.connect())?;
    Ok(Box::into_raw(Box::new(LanceNamespace {
        inner: NamespaceKind::Connected(namespace),
    })))
}

/// Close a namespace handle. Native datasets, providers and REST adapters hold
/// their own Arc references; the final owner releases foreign callback
/// contexts exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_namespace_close(namespace: *mut LanceNamespace) {
    if !namespace.is_null() {
        swallow_unwind("lance_namespace_close", || unsafe {
            drop(Box::from_raw(namespace));
        });
    }
}

/// Invoke one of the 54 native namespace operations.
///
/// `operation` is the exact native snake_case trait method name. Requests are
/// JSON encodings of the native models; NULL or empty means `{}`. For the four
/// payload operations the request body is `request_bytes`.
///
/// Result shapes:
/// - model responses: JSON in `result_json`
/// - unit responses: no JSON, no bytes
/// - `count_table_rows`: JSON number
/// - `explain_table_query_plan` / `analyze_table_query_plan`: JSON string
/// - `query_table`: native Arrow IPC bytes in `result_bytes`
///
/// Requests for merge-insert go through the native
/// `compat::merge_insert_request_from_json` normalization before the native
/// model is built. No namespace routing or business logic is implemented here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_namespace_call(
    namespace: *const LanceNamespace,
    operation: *const c_char,
    request_json: *const c_char,
    request_bytes: *const u8,
    request_bytes_len: usize,
    out_result: *mut LanceNamespaceResult,
) -> i32 {
    if out_result.is_null() {
        return ffi_guard_with(
            || {
                Err(Error::invalid_input_source(
                    "out_result must not be NULL".into(),
                ))
            },
            |_| -1,
        );
    }
    ffi_guard_with(
        || {
            block_on(async {
                unsafe {
                    namespace_call_inner(
                        namespace,
                        operation,
                        request_json,
                        request_bytes,
                        request_bytes_len,
                        out_result,
                    )
                    .await
                }
            })
        },
        |_| -1,
    )
}

async unsafe fn namespace_call_inner(
    namespace: *const LanceNamespace,
    operation: *const c_char,
    request_json: *const c_char,
    request_bytes: *const u8,
    request_bytes_len: usize,
    out_result: *mut LanceNamespaceResult,
) -> Result<i32> {
    let namespace = unsafe { namespace.as_ref() }
        .ok_or_else(|| Error::invalid_input_source("namespace must not be NULL".into()))?;
    let operation = unsafe { parse_c_string(operation)? }
        .filter(|operation| !operation.is_empty())
        .ok_or_else(|| Error::invalid_input("operation must not be empty"))?;
    let request_text = unsafe { parse_c_string(request_json)? }
        .filter(|text| !text.trim().is_empty())
        .unwrap_or("{}");
    let payload = unsafe { copy_payload(request_bytes, request_bytes_len)? };

    let outcome = dispatch_operation(namespace, operation, request_text, payload).await;
    unsafe { write_call_outcome(out_result, outcome) }
}

enum CallOutcome {
    Json(Option<String>),
    Bytes(Bytes),
}

async fn dispatch_operation(
    namespace: &LanceNamespace,
    operation: &str,
    request_text: &str,
    payload: Bytes,
) -> Result<CallOutcome> {
    let client = namespace.trait_object();
    macro_rules! json_request {
        ($method:ident, $request:ty) => {{
            let request: $request = parse_request(request_text, stringify!($method))?;
            let response = client.$method(request).await?;
            CallOutcome::Json(Some(serde_json::to_string(&response)?))
        }};
    }
    macro_rules! unit_request {
        ($method:ident, $request:ty) => {{
            let request: $request = parse_request(request_text, stringify!($method))?;
            client.$method(request).await?;
            CallOutcome::Json(None)
        }};
    }
    macro_rules! payload_request {
        ($method:ident, $request:ty) => {{
            let request: $request = parse_request(request_text, stringify!($method))?;
            let response = client.$method(request, payload.clone()).await?;
            CallOutcome::Json(Some(serde_json::to_string(&response)?))
        }};
    }
    macro_rules! scalar_request {
        ($method:ident, $request:ty, $response:ty) => {{
            let request: $request = parse_request(request_text, stringify!($method))?;
            let response: $response = client.$method(request).await?;
            CallOutcome::Json(Some(serde_json::to_string(&response)?))
        }};
    }

    Ok(match operation {
        "list_namespaces" => json_request!(list_namespaces, ListNamespacesRequest),
        "describe_namespace" => json_request!(describe_namespace, DescribeNamespaceRequest),
        "create_namespace" => json_request!(create_namespace, CreateNamespaceRequest),
        "drop_namespace" => json_request!(drop_namespace, DropNamespaceRequest),
        "namespace_exists" => unit_request!(namespace_exists, NamespaceExistsRequest),
        "list_tables" => json_request!(list_tables, ListTablesRequest),
        "describe_table" => json_request!(describe_table, DescribeTableRequest),
        "register_table" => json_request!(register_table, RegisterTableRequest),
        "table_exists" => unit_request!(table_exists, TableExistsRequest),
        "drop_table" => json_request!(drop_table, DropTableRequest),
        "deregister_table" => json_request!(deregister_table, DeregisterTableRequest),
        "count_table_rows" => scalar_request!(count_table_rows, CountTableRowsRequest, i64),
        "create_table" => payload_request!(create_table, CreateTableRequest),
        "declare_table" => json_request!(declare_table, DeclareTableRequest),
        "insert_into_table" => payload_request!(insert_into_table, InsertIntoTableRequest),
        "merge_insert_into_table" => {
            let value: Value = serde_json::from_str(request_text).map_err(|error| {
                Error::invalid_input_source(
                    format!("invalid merge_insert_into_table request JSON: {error}").into(),
                )
            })?;
            let request = merge_insert_request_from_json(value)?;
            let response = client.merge_insert_into_table(request, payload).await?;
            CallOutcome::Json(Some(serde_json::to_string(&response)?))
        }
        "update_table" => json_request!(update_table, UpdateTableRequest),
        "delete_from_table" => json_request!(delete_from_table, DeleteFromTableRequest),
        "query_table" => {
            let request: QueryTableRequest = parse_request(request_text, "query_table")?;
            CallOutcome::Bytes(client.query_table(request).await?)
        }
        "create_table_index" => json_request!(create_table_index, CreateTableIndexRequest),
        "list_table_indices" => json_request!(list_table_indices, ListTableIndicesRequest),
        "describe_table_index_stats" => {
            json_request!(describe_table_index_stats, DescribeTableIndexStatsRequest)
        }
        "describe_transaction" => json_request!(describe_transaction, DescribeTransactionRequest),
        "alter_transaction" => json_request!(alter_transaction, AlterTransactionRequest),
        "create_table_scalar_index" => {
            json_request!(create_table_scalar_index, CreateTableIndexRequest)
        }
        "drop_table_index" => json_request!(drop_table_index, DropTableIndexRequest),
        "list_all_tables" => json_request!(list_all_tables, ListTablesRequest),
        "restore_table" => json_request!(restore_table, RestoreTableRequest),
        "rename_table" => json_request!(rename_table, RenameTableRequest),
        "list_table_versions" => json_request!(list_table_versions, ListTableVersionsRequest),
        "create_table_version" => json_request!(create_table_version, CreateTableVersionRequest),
        "describe_table_version" => {
            json_request!(describe_table_version, DescribeTableVersionRequest)
        }
        "batch_delete_table_versions" => {
            json_request!(batch_delete_table_versions, BatchDeleteTableVersionsRequest)
        }
        "batch_create_table_versions" => {
            json_request!(batch_create_table_versions, BatchCreateTableVersionsRequest)
        }
        "batch_commit_tables" => json_request!(batch_commit_tables, BatchCommitTablesRequest),
        "update_table_schema_metadata" => {
            json_request!(
                update_table_schema_metadata,
                UpdateTableSchemaMetadataRequest
            )
        }
        "update_field_metadata" => {
            json_request!(update_field_metadata, UpdateFieldMetadataRequest)
        }
        "get_table_stats" => json_request!(get_table_stats, GetTableStatsRequest),
        "explain_table_query_plan" => scalar_request!(
            explain_table_query_plan,
            ExplainTableQueryPlanRequest,
            String
        ),
        "analyze_table_query_plan" => scalar_request!(
            analyze_table_query_plan,
            AnalyzeTableQueryPlanRequest,
            String
        ),
        "alter_table_add_columns" => {
            json_request!(alter_table_add_columns, AlterTableAddColumnsRequest)
        }
        "alter_table_alter_columns" => {
            json_request!(alter_table_alter_columns, AlterTableAlterColumnsRequest)
        }
        "alter_table_drop_columns" => {
            json_request!(alter_table_drop_columns, AlterTableDropColumnsRequest)
        }
        "alter_table_backfill_columns" => {
            json_request!(
                alter_table_backfill_columns,
                AlterTableBackfillColumnsRequest
            )
        }
        "refresh_materialized_view" => {
            json_request!(refresh_materialized_view, RefreshMaterializedViewRequest)
        }
        "create_materialized_view" => {
            json_request!(create_materialized_view, CreateMaterializedViewRequest)
        }
        "list_table_tags" => json_request!(list_table_tags, ListTableTagsRequest),
        "get_table_tag_version" => {
            json_request!(get_table_tag_version, GetTableTagVersionRequest)
        }
        "create_table_tag" => json_request!(create_table_tag, CreateTableTagRequest),
        "delete_table_tag" => json_request!(delete_table_tag, DeleteTableTagRequest),
        "update_table_tag" => json_request!(update_table_tag, UpdateTableTagRequest),
        "create_table_branch" => json_request!(create_table_branch, CreateTableBranchRequest),
        "list_table_branches" => json_request!(list_table_branches, ListTableBranchesRequest),
        "delete_table_branch" => json_request!(delete_table_branch, DeleteTableBranchRequest),
        other => {
            return Err(Error::invalid_input_source(
                format!("unknown namespace operation: {other}").into(),
            ));
        }
    })
}

fn parse_request<T: DeserializeOwned>(request_text: &str, operation: &str) -> Result<T> {
    serde_json::from_str(request_text).map_err(|error| {
        Error::invalid_input_source(format!("invalid {operation} request JSON: {error}").into())
    })
}

unsafe fn copy_payload(bytes: *const u8, len: usize) -> Result<Bytes> {
    if len == 0 {
        if !bytes.is_null() {
            // A non-NULL pointer with length zero is still accepted as an empty
            // payload; no bytes are read.
        }
        return Ok(Bytes::new());
    }
    if bytes.is_null() {
        return Err(Error::invalid_input_source(
            format!("request_bytes must not be NULL when request_bytes_len = {len}").into(),
        ));
    }
    let slice = unsafe { std::slice::from_raw_parts(bytes, len) };
    Ok(Bytes::copy_from_slice(slice))
}

unsafe fn write_call_outcome(
    out_result: *mut LanceNamespaceResult,
    outcome: Result<CallOutcome>,
) -> Result<i32> {
    unsafe { ptr::write(out_result, LanceNamespaceResult::default()) };
    let outcome = match outcome {
        Ok(outcome) => outcome,
        Err(error) => {
            let mut result = LanceNamespaceResult::default();
            if let Error::Namespace { source, .. } = &error
                && let Some(namespace_error) = source.downcast_ref::<NamespaceError>()
            {
                result.has_namespace_error = 1;
                result.namespace_error_code = namespace_error.code().as_u32();
                result.namespace_error_message =
                    alloc_c_string(namespace_error.message()).unwrap_or(ptr::null_mut());
            }
            unsafe { ptr::write(out_result, result) };
            return Err(error);
        }
    };
    let mut result = LanceNamespaceResult::default();
    match outcome {
        CallOutcome::Json(None) => {}
        CallOutcome::Json(Some(json)) => {
            result.result_json = alloc_c_string(&json)?;
        }
        CallOutcome::Bytes(bytes) => {
            if !bytes.is_empty() {
                let boxed = bytes.to_vec().into_boxed_slice();
                result.result_bytes_len = boxed.len();
                result.result_bytes = Box::into_raw(boxed) as *mut u8;
            }
        }
    }
    unsafe { ptr::write(out_result, result) };
    Ok(0)
}

/// Create a native namespace-backed storage-options provider.
///
/// The provider calls native `describe_table` with `vend_credentials = true`
/// and keeps the namespace Arc alive. Table identity and refresh behavior are
/// exactly those of `LanceNamespaceStorageOptionsProvider`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_namespace_storage_options_provider_new(
    namespace: *const LanceNamespace,
    table_id_json: *const c_char,
) -> *mut LanceStorageOptionsProvider {
    ffi_guard_with(
        || {
            let namespace = unsafe { namespace.as_ref() }
                .ok_or_else(|| Error::invalid_input_source("namespace must not be NULL".into()))?;
            let table_id = unsafe { table_id_from_json(table_id_json)? };
            let provider: Arc<dyn StorageOptionsProvider> = Arc::new(
                LanceNamespaceStorageOptionsProvider::new(namespace.trait_object(), table_id),
            );
            Ok(Box::into_raw(Box::new(LanceStorageOptionsProvider {
                inner: provider,
            })))
        },
        |_| ptr::null_mut(),
    )
}

/// Open a dataset through native `DatasetBuilder::from_namespace`.
///
/// Native describe-table location/storage-option precedence is preserved.
/// `storage_options_json` entries are merged over namespace-provided initial
/// options while the native namespace provider remains attached. `version == 0`
/// selects the native latest version. `session` may be NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_open_namespace(
    namespace: *const LanceNamespace,
    table_id_json: *const c_char,
    storage_options_json: *const c_char,
    version: u64,
    session: *const LanceSession,
) -> *mut LanceDataset {
    ffi_guard_with(
        || unsafe {
            dataset_open_namespace_inner(
                namespace,
                table_id_json,
                storage_options_json,
                version,
                session,
            )
        },
        |_| ptr::null_mut(),
    )
}

unsafe fn dataset_open_namespace_inner(
    namespace: *const LanceNamespace,
    table_id_json: *const c_char,
    storage_options_json: *const c_char,
    version: u64,
    session: *const LanceSession,
) -> Result<*mut LanceDataset> {
    let namespace = unsafe { namespace.as_ref() }
        .ok_or_else(|| Error::invalid_input_source("namespace must not be NULL".into()))?;
    let table_id = unsafe { table_id_from_json(table_id_json)? };
    let storage_options =
        unsafe { properties_from_json(storage_options_json, "storage_options_json")? };
    let mut builder = block_on(DatasetBuilder::from_namespace(
        namespace.trait_object(),
        table_id,
    ))?;
    if !storage_options.is_empty() {
        builder = builder.with_storage_options(storage_options);
    }
    if version != 0 {
        builder = builder.with_version(version);
    }
    if let Some(session) = unsafe { session.as_ref() } {
        builder = builder.with_session(session.inner.clone());
    }
    let dataset = block_on(builder.load())?;
    Ok(Box::into_raw(Box::new(dataset_handle(dataset))))
}

/// Write an Arrow stream into a native namespace-managed table.
///
/// CREATE calls native `declare_table`; APPEND/OVERWRITE open the existing
/// table through native namespace describe + commit-handler behavior. A
/// non-NULL `branch` selects native `write_into_namespace_on_branch`; NULL keeps
/// main. `params`/`options` reuse the existing `LanceWriteParams` conversion,
/// with legacy auto-cleanup disabled so native write defaults apply.
/// `storage_options_json` entries are passed as initial options and merged by
/// the native namespace write path. The Arrow stream is consumed on every
/// return after a successful `from_raw`, including validation failures.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn lance_dataset_write_namespace(
    namespace: *const LanceNamespace,
    table_id_json: *const c_char,
    schema: *const FFI_ArrowSchema,
    stream: *mut FFI_ArrowArrayStream,
    mode: i32,
    params: *const crate::writer::LanceWriteParams,
    options: *const LanceWriteOptions,
    storage_options_json: *const c_char,
    session: *const LanceSession,
    branch: *const c_char,
    out_dataset: *mut *mut LanceDataset,
) -> i32 {
    ffi_guard_with(
        || unsafe {
            dataset_write_namespace_inner(
                namespace,
                table_id_json,
                schema,
                stream,
                mode,
                params,
                options,
                storage_options_json,
                session,
                branch,
                out_dataset,
            )
        },
        |_| -1,
    )
}

#[allow(clippy::too_many_arguments)]
unsafe fn dataset_write_namespace_inner(
    namespace: *const LanceNamespace,
    table_id_json: *const c_char,
    schema: *const FFI_ArrowSchema,
    stream: *mut FFI_ArrowArrayStream,
    mode: i32,
    params: *const crate::writer::LanceWriteParams,
    options: *const LanceWriteOptions,
    storage_options_json: *const c_char,
    session: *const LanceSession,
    branch: *const c_char,
    out_dataset: *mut *mut LanceDataset,
) -> Result<i32> {
    let namespace = unsafe { namespace.as_ref() }
        .ok_or_else(|| Error::invalid_input_source("namespace must not be NULL".into()))?;
    if out_dataset.is_null() {
        return Err(Error::invalid_input_source(
            "out_dataset must not be NULL".into(),
        ));
    }
    if stream.is_null() {
        return Err(Error::invalid_input_source(
            "stream must not be NULL".into(),
        ));
    }
    let reader = unsafe { ArrowArrayStreamReader::from_raw(stream) }
        .map_err(|error| Error::invalid_input_source(error.to_string().into()))?;
    if schema.is_null() {
        return Err(Error::invalid_input_source(
            "schema must not be NULL".into(),
        ));
    }
    let mode = LanceWriteMode::from_raw(mode)?;
    let table_id = unsafe { table_id_from_json(table_id_json)? };
    let expected_schema = ArrowSchema::try_from(unsafe { &*schema })
        .map_err(|error| Error::invalid_input_source(format!("invalid schema: {error}").into()))?;
    let stream_schema = reader.schema();
    if stream_schema.fields() != expected_schema.fields() {
        return Err(Error::invalid_input_source(
            format!(
                "stream schema does not match the provided schema.\n  expected: {expected_schema}\n  got:      {stream_schema}"
            )
            .into(),
        ));
    }

    let storage_options =
        unsafe { properties_from_json(storage_options_json, "storage_options_json")? };
    let mut write_params = unsafe {
        write_params_from_ffi(
            mode,
            params,
            options,
            false,
            ptr::null(),
            unsafe { session.as_ref() }.map(|session| session.inner.clone()),
        )?
    };
    if !storage_options.is_empty() {
        let mut store_params = write_params.store_params.take().unwrap_or_default();
        let existing = store_params.storage_options_accessor.take();
        let mut merged = existing
            .as_ref()
            .and_then(|accessor| accessor.initial_storage_options().cloned())
            .unwrap_or_default();
        merged.extend(storage_options);
        let accessor = match existing
            .as_ref()
            .and_then(|accessor| accessor.provider().cloned())
        {
            Some(provider) => {
                lance_io::object_store::StorageOptionsAccessor::with_initial_and_provider(
                    merged, provider,
                )
            }
            None => lance_io::object_store::StorageOptionsAccessor::with_static_options(merged),
        };
        store_params.storage_options_accessor = Some(Arc::new(accessor));
        write_params.store_params = Some(store_params);
    }

    let branch = unsafe { parse_c_string(branch)? };
    let dataset = if let Some(branch) = branch {
        block_on(Dataset::write_into_namespace_on_branch(
            reader,
            namespace.trait_object(),
            table_id,
            branch,
            Some(write_params),
        ))?
    } else {
        block_on(Dataset::write_into_namespace(
            reader,
            namespace.trait_object(),
            table_id,
            Some(write_params),
        ))?
    };
    unsafe {
        ptr::write(
            out_dataset,
            Box::into_raw(Box::new(dataset_handle(dataset))),
        );
    }
    Ok(0)
}

/// Commit a native transaction to a namespace-managed table URI.
///
/// This is for callers that already hold a publication URI from the namespace.
/// The commit handler is `ExternalManifestCommitHandler` over
/// `LanceNamespaceExternalManifestStore::for_table_uri`, so managed version
/// metadata is published through the namespace. All commit option conversion,
/// retries, storage options and session behavior reuse the existing
/// `CommitBuilder` path. Prefer `lance_dataset_write_namespace` for normal
/// writes; namespace-opened destinations keep their native handler.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn lance_transaction_commit_namespace_uri(
    namespace: *const LanceNamespace,
    table_id_json: *const c_char,
    uri: *const c_char,
    transaction: *const crate::transaction::LanceTransaction,
    options: *const crate::commit::LanceCommitOptions,
    storage_opts: *const *const c_char,
    session: *const LanceSession,
    out_dataset: *mut *mut LanceDataset,
) -> i32 {
    ffi_guard_with(
        || unsafe {
            transaction_commit_namespace_uri_inner(
                namespace,
                table_id_json,
                uri,
                transaction,
                options,
                storage_opts,
                session,
                out_dataset,
            )
        },
        |_| -1,
    )
}

#[allow(clippy::too_many_arguments)]
unsafe fn transaction_commit_namespace_uri_inner(
    namespace: *const LanceNamespace,
    table_id_json: *const c_char,
    uri: *const c_char,
    transaction: *const crate::transaction::LanceTransaction,
    options: *const crate::commit::LanceCommitOptions,
    storage_opts: *const *const c_char,
    session: *const LanceSession,
    out_dataset: *mut *mut LanceDataset,
) -> Result<i32> {
    let namespace = unsafe { namespace.as_ref() }
        .ok_or_else(|| Error::invalid_input_source("namespace must not be NULL".into()))?;
    if transaction.is_null() {
        return Err(Error::invalid_input_source(
            "transaction must not be NULL".into(),
        ));
    }
    if out_dataset.is_null() {
        return Err(Error::invalid_input_source(
            "out_dataset must not be NULL".into(),
        ));
    }
    let table_id = unsafe { table_id_from_json(table_id_json)? };
    let uri = unsafe { parse_c_string(uri)? }
        .filter(|uri| !uri.is_empty())
        .ok_or_else(|| Error::invalid_input("uri must not be empty"))?;

    let external_store = LanceNamespaceExternalManifestStore::for_table_uri(
        namespace.trait_object(),
        table_id,
        uri,
    )?;
    let handler: Arc<dyn CommitHandler> = Arc::new(ExternalManifestCommitHandler {
        external_manifest_store: Arc::new(external_store),
    });
    let transaction = unsafe { &*transaction }.inner.clone();
    let builder = unsafe {
        commit_builder_from_ffi_with_handler(uri, Some(handler), options, storage_opts, session)?
    };
    let dataset = block_on(builder.execute(transaction))?;
    unsafe {
        ptr::write(
            out_dataset,
            Box::into_raw(Box::new(dataset_handle(dataset))),
        );
    }
    Ok(0)
}

/// Start a native REST adapter over an existing namespace backend.
///
/// `host` NULL/empty uses the native default. `port == 0` binds an ephemeral
/// loopback port; read it with `lance_rest_adapter_port` after start. The
/// adapter holds a backend Arc and the server task until shutdown/close.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rest_adapter_start(
    namespace: *const LanceNamespace,
    host: *const c_char,
    port: u16,
    out_adapter: *mut *mut LanceRestAdapter,
) -> i32 {
    ffi_guard_with(
        || unsafe { rest_adapter_start_inner(namespace, host, port, out_adapter) },
        |_| -1,
    )
}

/// Opaque running REST adapter handle.
pub struct LanceRestAdapter {
    handle: Option<RestAdapterHandle>,
    port: u16,
}

unsafe fn rest_adapter_start_inner(
    namespace: *const LanceNamespace,
    host: *const c_char,
    port: u16,
    out_adapter: *mut *mut LanceRestAdapter,
) -> Result<i32> {
    let namespace = unsafe { namespace.as_ref() }
        .ok_or_else(|| Error::invalid_input_source("namespace must not be NULL".into()))?;
    if out_adapter.is_null() {
        return Err(Error::invalid_input_source(
            "out_adapter must not be NULL".into(),
        ));
    }
    let host = unsafe { parse_c_string(host)? }
        .filter(|host| !host.is_empty())
        .unwrap_or("127.0.0.1")
        .to_owned();
    let adapter = RestAdapter::new(namespace.trait_object(), RestAdapterConfig { host, port });
    let handle = block_on(adapter.start())?;
    let actual_port = handle.port();
    let adapter = LanceRestAdapter {
        handle: Some(handle),
        port: actual_port,
    };
    unsafe { ptr::write(out_adapter, Box::into_raw(Box::new(adapter))) };
    Ok(0)
}

/// Return the bound port, or 0 when the adapter is not running.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rest_adapter_port(adapter: *const LanceRestAdapter) -> u16 {
    unsafe { adapter.as_ref() }.map_or(0, |adapter| adapter.port)
}

/// Gracefully stop the adapter. Idempotent; the native handle retains the
/// backend and in-flight server ownership until this call completes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rest_adapter_shutdown(adapter: *mut LanceRestAdapter) {
    if adapter.is_null() {
        return;
    }
    swallow_unwind("lance_rest_adapter_shutdown", || unsafe {
        let adapter = &mut *adapter;
        if let Some(handle) = adapter.handle.take() {
            handle.shutdown();
        }
        adapter.port = 0;
    });
}

/// Close and stop an adapter. Native shutdown is part of close.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rest_adapter_close(adapter: *mut LanceRestAdapter) {
    if adapter.is_null() {
        return;
    }
    swallow_unwind("lance_rest_adapter_close", || unsafe {
        let mut adapter = Box::from_raw(adapter);
        if let Some(handle) = adapter.handle.take() {
            handle.shutdown();
        }
    });
}

// ─── Foreign callback client ───────────────────────────────────────────────

type NamespaceOperationCallback = unsafe extern "C" fn(
    client_ctx: *mut c_void,
    request_json: *const c_char,
    out_response_json: *mut LanceOwnedString,
    out_error: *mut LanceCallbackError,
) -> i32;

/// Foreign client for the four managed table/version callbacks that Python's
/// custom namespace client implements.
///
/// All other operations keep the native trait defaults and therefore fail with
/// native `Unsupported`. Callbacks may synchronously call independent C
/// handles; they run inside the established reentrant blocking region.
///
/// Ownership of `client_ctx` and `release` transfers to the library when this
/// function is called, even if construction fails. The context is released
/// exactly once after the final native namespace Arc is dropped.
#[repr(C)]
pub struct LanceNamespaceCustomClientSpec {
    pub describe_table: Option<NamespaceOperationCallback>,
    pub describe_table_version: Option<NamespaceOperationCallback>,
    pub create_table_version: Option<NamespaceOperationCallback>,
    pub list_table_versions: Option<NamespaceOperationCallback>,
    pub release: Option<unsafe extern "C" fn(*mut c_void)>,
    pub client_ctx: *mut c_void,
    /// Optional copied identifier. NULL or empty means `custom`.
    pub namespace_id: *const c_char,
}

/// Create a foreign-client namespace. See [`LanceNamespaceCustomClientSpec`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_namespace_custom_new(
    spec: *const LanceNamespaceCustomClientSpec,
) -> *mut LanceNamespace {
    ffi_guard_with(|| unsafe { custom_new_inner(spec) }, |_| ptr::null_mut())
}

unsafe fn custom_new_inner(
    spec: *const LanceNamespaceCustomClientSpec,
) -> Result<*mut LanceNamespace> {
    let spec = unsafe { spec.as_ref() }
        .ok_or_else(|| Error::invalid_input_source("spec must not be NULL".into()))?;
    let context = OwnedCallbackContext::new(spec.client_ctx, spec.release);
    let describe_table = spec.describe_table.ok_or_else(|| {
        Error::invalid_input_source("spec.describe_table must not be NULL".into())
    })?;
    let describe_table_version = spec.describe_table_version.ok_or_else(|| {
        Error::invalid_input_source("spec.describe_table_version must not be NULL".into())
    })?;
    let create_table_version = spec.create_table_version.ok_or_else(|| {
        Error::invalid_input_source("spec.create_table_version must not be NULL".into())
    })?;
    let list_table_versions = spec.list_table_versions.ok_or_else(|| {
        Error::invalid_input_source("spec.list_table_versions must not be NULL".into())
    })?;
    let id = unsafe { parse_c_string(spec.namespace_id)? }
        .filter(|id| !id.is_empty())
        .unwrap_or("custom")
        .to_owned();
    let namespace = CustomNamespace {
        id,
        context,
        describe_table,
        describe_table_version,
        create_table_version,
        list_table_versions,
    };
    Ok(Box::into_raw(Box::new(LanceNamespace {
        inner: NamespaceKind::Custom(Arc::new(namespace)),
    })))
}

struct OwnedCallbackContext {
    callback_ctx: *mut c_void,
    release: Option<unsafe extern "C" fn(*mut c_void)>,
}

// SAFETY: the foreign callback contract requires the context and callback
// table to stay usable from native worker threads until the final Arc drops.
unsafe impl Send for OwnedCallbackContext {}
unsafe impl Sync for OwnedCallbackContext {}

impl fmt::Debug for OwnedCallbackContext {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("OwnedCallbackContext")
            .field("has_context", &(!self.callback_ctx.is_null()))
            .field("has_release", &self.release.is_some())
            .finish()
    }
}

impl OwnedCallbackContext {
    fn new(callback_ctx: *mut c_void, release: Option<unsafe extern "C" fn(*mut c_void)>) -> Self {
        Self {
            callback_ctx,
            release,
        }
    }
}

impl Drop for OwnedCallbackContext {
    fn drop(&mut self) {
        let Some(release) = self.release else {
            return;
        };
        let context = self.callback_ctx;
        swallow_unwind("namespace callback context release", || unsafe {
            release(context);
        });
    }
}

struct CustomNamespace {
    id: String,
    context: OwnedCallbackContext,
    describe_table: NamespaceOperationCallback,
    describe_table_version: NamespaceOperationCallback,
    create_table_version: NamespaceOperationCallback,
    list_table_versions: NamespaceOperationCallback,
}

impl fmt::Debug for CustomNamespace {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("CustomNamespace")
            .field("id", &self.id)
            .finish()
    }
}

impl CustomNamespace {
    fn invoke<Request: Serialize, Response: DeserializeOwned>(
        &self,
        callback: NamespaceOperationCallback,
        request: &Request,
        operation: &str,
    ) -> Result<Response> {
        let request_json = serde_json::to_string(request)?;
        let request_json = CString::new(request_json)
            .map_err(|_| Error::invalid_input("request contains an interior NUL byte"))?;
        let mut response = LanceOwnedString::empty();
        let mut error = LanceCallbackError::empty();
        // Foreign callbacks may synchronously read through independent handles.
        let status = tokio::task::block_in_place(|| unsafe {
            callback(
                self.context.callback_ctx,
                request_json.as_ptr(),
                &mut response,
                &mut error,
            )
        });
        let error_code = error.code;
        let error_message = unsafe { take_owned_string(&mut error.message) }.into_display_string(
            &format!("{operation} callback returned error status {status} without a message"),
        );
        unsafe { release_message_only(&mut error) };
        if status != 0 {
            // Release a partial response even on failure so callback-owned
            // memory is not leaked.
            let _ = unsafe { take_owned_string(&mut response) };
            return Err(namespace_error_from_callback(
                error_code,
                error_message,
                operation,
            ));
        }
        let response_json = unsafe { take_owned_string(&mut response) }.into_string(&format!(
            "{operation} callback returned non-UTF-8 response JSON"
        ))?;
        serde_json::from_str(&response_json).map_err(|error| {
            Error::invalid_input_source(
                format!("{operation} callback returned invalid JSON response: {error}").into(),
            )
        })
    }
}

fn namespace_error_from_callback(code: i32, message: String, operation: &str) -> Error {
    let namespace_error = match code {
        1 => NamespaceError::InvalidInput { message },
        2 => NamespaceError::Internal {
            message: format!("{operation} callback I/O error: {message}"),
        },
        3 => NamespaceError::TableNotFound { message },
        4 => NamespaceError::TableAlreadyExists { message },
        6 => NamespaceError::Internal { message },
        7 => NamespaceError::Unsupported { message },
        8 => NamespaceError::ConcurrentModification { message },
        other => NamespaceError::Internal {
            message: format!(
                "{operation} callback returned unsupported error code {other}: {message}"
            ),
        },
    };
    Error::namespace_source(Box::new(namespace_error))
}

#[async_trait]
impl NativeLanceNamespace for CustomNamespace {
    fn namespace_id(&self) -> String {
        self.id.clone()
    }

    async fn describe_table(&self, request: DescribeTableRequest) -> Result<DescribeTableResponse> {
        self.invoke(self.describe_table, &request, "describe_table")
    }

    async fn describe_table_version(
        &self,
        request: DescribeTableVersionRequest,
    ) -> Result<DescribeTableVersionResponse> {
        self.invoke(
            self.describe_table_version,
            &request,
            "describe_table_version",
        )
    }

    async fn create_table_version(
        &self,
        request: CreateTableVersionRequest,
    ) -> Result<CreateTableVersionResponse> {
        self.invoke(self.create_table_version, &request, "create_table_version")
    }

    async fn list_table_versions(
        &self,
        request: ListTableVersionsRequest,
    ) -> Result<ListTableVersionsResponse> {
        self.invoke(self.list_table_versions, &request, "list_table_versions")
    }
}

// ─── Dynamic context provider ──────────────────────────────────────────────

type ContextProviderCallback = unsafe extern "C" fn(
    provider_ctx: *mut c_void,
    operation: *const c_char,
    object_id: *const c_char,
    out_result_json: *mut LanceOwnedString,
    out_error: *mut LanceCallbackError,
) -> i32;

/// Foreign synchronous, infallible dynamic context provider.
///
/// `provide_context` returns a JSON object of string values. Native REST
/// clients strip the `headers.` prefix and apply base headers first, then
/// these overrides. A callback failure or malformed JSON is logged and treated
/// as an empty map; the request continues. The callback receives the native
/// operation name and object id unchanged.
///
/// Ownership of `provider_ctx` and `release` transfers to the library when this
/// spec is passed to a namespace constructor, including on construction
/// failure. It is released exactly once after the final native owner drops.
#[repr(C)]
pub struct LanceNamespaceContextProviderSpec {
    pub provide_context: Option<ContextProviderCallback>,
    pub release: Option<unsafe extern "C" fn(*mut c_void)>,
    pub provider_ctx: *mut c_void,
}

#[derive(Debug)]
struct FfiContextProvider {
    context: OwnedCallbackContext,
    callback: ContextProviderCallback,
}

impl DynamicContextProvider for FfiContextProvider {
    fn provide_context(&self, info: &OperationInfo) -> HashMap<String, String> {
        let operation = CString::new(info.operation.as_str()).unwrap_or_default();
        let object_id = CString::new(info.object_id.as_str()).unwrap_or_default();
        let mut response = LanceOwnedString::empty();
        let mut error = LanceCallbackError::empty();
        let status = tokio::task::block_in_place(|| unsafe {
            (self.callback)(
                self.context.callback_ctx,
                operation.as_ptr(),
                object_id.as_ptr(),
                &mut response,
                &mut error,
            )
        });
        let message = unsafe { take_owned_string(&mut error.message) }
            .into_display_string("context provider callback returned no message");
        unsafe { release_message_only(&mut error) };
        if status != 0 {
            let _ = unsafe { take_owned_string(&mut response) };
            log::warn!(
                "namespace context provider failed for {} {} (status {}): {}; continuing with empty context",
                info.operation,
                info.object_id,
                status,
                message
            );
            return HashMap::new();
        }
        let text = match unsafe { take_owned_string(&mut response) }
            .into_string("context provider returned non-UTF-8 JSON")
        {
            Ok(text) => text,
            Err(error) => {
                log::warn!(
                    "namespace context provider failed for {} {}: {}; continuing with empty context",
                    info.operation,
                    info.object_id,
                    error
                );
                return HashMap::new();
            }
        };
        match serde_json::from_str::<HashMap<String, String>>(&text) {
            Ok(context) => context,
            Err(error) => {
                log::warn!(
                    "namespace context provider returned invalid JSON for {} {}: {}; continuing with empty context",
                    info.operation,
                    info.object_id,
                    error
                );
                HashMap::new()
            }
        }
    }
}

unsafe fn context_provider_from_spec(
    spec: *const LanceNamespaceContextProviderSpec,
) -> Result<Option<Arc<dyn DynamicContextProvider>>> {
    let Some(spec) = (unsafe { spec.as_ref() }) else {
        return Ok(None);
    };
    let context = OwnedCallbackContext::new(spec.provider_ctx, spec.release);
    let callback = spec.provide_context.ok_or_else(|| {
        Error::invalid_input_source("context_provider.provide_context must not be NULL".into())
    })?;
    Ok(Some(Arc::new(FfiContextProvider { context, callback })))
}

// Keep these imports used in every feature combination and document the
// native write model types referenced by the C header.
#[allow(dead_code)]
fn _assert_public_types(_params: WriteParams, _dataset: &Dataset, _error_code: LanceErrorCode) {}
