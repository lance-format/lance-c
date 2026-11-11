// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! End-to-end tests for the native namespace C ABI.
//!
//! These tests deliberately go through the exported C entry points. They use
//! the native directory backend, the native REST adapter/client pair, and the
//! four-callback custom client. Payload operations are exercised through the
//! same dispatch table used by C and C++ callers.

use std::ffi::{CStr, CString, c_char, c_void};
use std::ptr;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi_stream::FFI_ArrowArrayStream;
use arrow_array::builder::{FixedSizeListBuilder, Float32Builder};
use arrow_array::{Int32Array, RecordBatch};
use arrow_schema::{DataType, Field, Schema};
use lance_c::*;

fn c_string(value: &str) -> CString {
    CString::new(value).unwrap()
}

fn owned_string(value: &str) -> *mut c_char {
    c_string(value).into_raw()
}

fn take_and_free(value: *const c_char) -> String {
    assert!(!value.is_null(), "expected a copied C string");
    let text = unsafe { CStr::from_ptr(value) }
        .to_str()
        .unwrap()
        .to_owned();
    unsafe { lance_free_string(value.cast_mut()) };
    text
}

fn last_error() -> String {
    let ptr = lance_last_error_message();
    if ptr.is_null() {
        return String::new();
    }
    take_and_free(ptr)
}

#[derive(Default)]
struct NamespaceResult(LanceNamespaceResult);

impl Drop for NamespaceResult {
    fn drop(&mut self) {
        unsafe { lance_namespace_result_close(&mut self.0) };
    }
}

impl NamespaceResult {
    fn json(&self) -> Option<String> {
        if self.0.result_json.is_null() {
            None
        } else {
            Some(
                unsafe { CStr::from_ptr(self.0.result_json) }
                    .to_str()
                    .unwrap()
                    .to_owned(),
            )
        }
    }

    fn bytes(&self) -> &[u8] {
        if self.0.result_bytes.is_null() {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(self.0.result_bytes, self.0.result_bytes_len) }
        }
    }

    fn native_error(&self) -> Option<(u32, String)> {
        if self.0.has_namespace_error == 0 {
            return None;
        }
        let message = if self.0.namespace_error_message.is_null() {
            String::new()
        } else {
            unsafe { CStr::from_ptr(self.0.namespace_error_message) }
                .to_str()
                .unwrap()
                .to_owned()
        };
        Some((self.0.namespace_error_code, message))
    }
}

fn call(
    namespace: *const LanceNamespace,
    operation: &str,
    request: &str,
    payload: Option<&[u8]>,
) -> (i32, NamespaceResult) {
    let operation = c_string(operation);
    let request = c_string(request);
    let mut result = NamespaceResult::default();
    let (bytes, len) = payload
        .map(|bytes| (bytes.as_ptr(), bytes.len()))
        .unwrap_or((ptr::null(), 0));
    let rc = unsafe {
        lance_namespace_call(
            namespace,
            operation.as_ptr(),
            request.as_ptr(),
            bytes,
            len,
            &mut result.0,
        )
    };
    (rc, result)
}

fn assert_call_ok(
    namespace: *const LanceNamespace,
    operation: &str,
    request: &str,
) -> NamespaceResult {
    let (rc, result) = call(namespace, operation, request, None);
    assert_eq!(
        rc,
        0,
        "{operation} failed: code={:?} error={} native={:?}",
        lance_last_error_code(),
        last_error(),
        result.native_error()
    );
    result
}

fn assert_native_unsupported(namespace: *const LanceNamespace, operation: &str, request: &str) {
    let (rc, result) = call(namespace, operation, request, None);
    assert_ne!(rc, 0, "{operation} must not report success");
    assert_eq!(
        lance_last_error_code(),
        LanceErrorCode::NotSupported,
        "{operation} generic code: {}",
        last_error()
    );
    if let Some((code, message)) = result.native_error() {
        assert_eq!(code, 0, "{operation} native Unsupported code");
        assert!(!message.is_empty(), "{operation} native error message");
    }
}

fn unsupported_request(operation: &str) -> &'static str {
    match operation {
        "list_namespaces" => r#"{"id":[]}"#,
        "query_table" => r#"{"id":["workspace"],"k":1,"vector":{"single_vector":[0.0]}}"#,
        "create_table_index" | "create_table_scalar_index" => {
            r#"{"id":["workspace"],"column":"id","index_type":"BTREE"}"#
        }
        "alter_table_add_columns" => r#"{"id":["workspace"],"new_columns":[{"name":"x"}]}"#,
        "alter_table_alter_columns" => r#"{"id":["workspace"],"alterations":[{"path":"x"}]}"#,
        "alter_table_drop_columns" => r#"{"id":["workspace"],"columns":["x"]}"#,
        "alter_table_backfill_columns" => r#"{"id":["workspace"],"column":"x"}"#,
        "update_table" => r#"{"id":["workspace"],"updates":[]}"#,
        "delete_from_table" => r#"{"id":["workspace"],"predicate":"true"}"#,
        "register_table" => r#"{"id":["workspace"],"location":"x"}"#,
        "restore_table" => r#"{"id":["workspace"],"version":1}"#,
        "rename_table" => r#"{"id":["workspace"],"new_table_name":"x"}"#,
        "alter_transaction" => r#"{"id":["workspace"],"actions":[]}"#,
        "batch_commit_tables" => r#"{"operations":[]}"#,
        "create_table_version" => r#"{"id":["workspace"],"version":1,"manifest_path":"x"}"#,
        "batch_delete_table_versions" => {
            r#"{"id":["workspace"],"ranges":[{"start_version":0,"end_version":1}]}"#
        }
        "batch_create_table_versions" => {
            r#"{"entries":[{"id":["workspace"],"version":1,"manifest_path":"x"}]}"#
        }
        "update_field_metadata" => r#"{"id":["workspace"],"updates":[{"path":"x","metadata":{}}]}"#,
        "explain_table_query_plan" | "analyze_table_query_plan" => {
            r#"{"id":["workspace"],"k":1,"vector":{"single_vector":[0.0]},"query":{"id":["workspace"],"k":1,"vector":{"single_vector":[0.0]}}}"#
        }
        "create_materialized_view" => {
            r#"{"id":["workspace"],"kind":"query","source_query":"select 1","output_schema":"{}"}"#
        }
        "create_table_tag" | "update_table_tag" => r#"{"id":["workspace"],"tag":"v1","version":1}"#,
        "get_table_tag_version" | "delete_table_tag" => r#"{"id":["workspace"],"tag":"v1"}"#,
        "create_table_branch" | "delete_table_branch" => r#"{"id":["workspace"],"name":"main"}"#,
        _ => r#"{"id":["workspace"]}"#,
    }
}

fn make_vector_batch() -> (Schema, RecordBatch) {
    const DIM: i32 = 2;
    let schema = Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new(
            "vector",
            DataType::FixedSizeList(Arc::new(Field::new("item", DataType::Float32, true)), DIM),
            false,
        ),
    ]);
    let ids = Int32Array::from(vec![0, 1, 2]);
    let mut vectors = FixedSizeListBuilder::new(Float32Builder::new(), DIM);
    for id in 0..3 {
        vectors.values().append_slice(&[id as f32, 0.0]);
        vectors.append(true);
    }
    let batch = RecordBatch::try_new(
        Arc::new(schema.clone()),
        vec![Arc::new(ids), Arc::new(vectors.finish())],
    )
    .unwrap();
    (schema, batch)
}

fn make_batch() -> (Arc<Schema>, RecordBatch) {
    let schema = Arc::new(Schema::new(vec![Field::new("id", DataType::Int32, false)]));
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![Arc::new(Int32Array::from(vec![1_i32, 2, 3]))],
    )
    .unwrap();
    (schema, batch)
}

fn write_namespace_result(
    namespace: *const LanceNamespace,
    table_id: &str,
    mode: i32,
    schema: &Schema,
    batch: RecordBatch,
) -> (i32, *mut LanceDataset) {
    let table_id = c_string(table_id);
    let ffi_schema = FFI_ArrowSchema::try_from(schema).unwrap();
    let reader =
        arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], Arc::new(schema.clone()));
    let mut stream = FFI_ArrowArrayStream::new(Box::new(reader));
    let mut dataset = ptr::null_mut();
    let rc = unsafe {
        lance_dataset_write_namespace(
            namespace,
            table_id.as_ptr(),
            &ffi_schema,
            &mut stream,
            mode,
            ptr::null(),
            ptr::null(),
            ptr::null(),
            ptr::null(),
            ptr::null(),
            &mut dataset,
        )
    };
    (rc, dataset)
}

fn write_namespace(
    namespace: *const LanceNamespace,
    table_id: &str,
    mode: i32,
    schema: &Schema,
    batch: RecordBatch,
) -> *mut LanceDataset {
    let (rc, dataset) = write_namespace_result(namespace, table_id, mode, schema, batch);
    assert_eq!(
        rc,
        0,
        "namespace write mode {mode} failed: code={:?} error={}",
        lance_last_error_code(),
        last_error()
    );
    assert!(!dataset.is_null());
    dataset
}

fn close_namespace(namespace: *mut LanceNamespace) {
    unsafe { lance_namespace_close(namespace) };
}

fn directory_namespace(root: &str) -> *mut LanceNamespace {
    directory_namespace_with_session(root, ptr::null_mut())
}

fn directory_namespace_with_session(root: &str, session: *mut LanceSession) -> *mut LanceNamespace {
    let properties = format!(
        r#"{{"root":"{}","manifest_enabled":"true","table_version_tracking_enabled":"true","ops_metrics_enabled":"true"}}"#,
        root.replace('\\', "\\\\").replace('"', "\\\"")
    );
    let properties = c_string(&properties);
    let namespace =
        unsafe { lance_namespace_directory_new(properties.as_ptr(), session, ptr::null()) };
    assert!(
        !namespace.is_null(),
        "directory namespace failed: {:?} {}",
        lance_last_error_code(),
        last_error()
    );
    namespace
}

#[test]
fn directory_lifecycle_dataset_versions_metrics_and_dispatch() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path().join("namespace");
    std::fs::create_dir_all(&root).unwrap();
    let session = lance_session_new(0, 0);
    assert!(!session.is_null(), "session: {}", last_error());
    let namespace = directory_namespace_with_session(root.to_str().unwrap(), session);
    // The native directory retains its own session Arc. Closing the C handle
    // here must not invalidate later namespace operations.
    unsafe { lance_session_close(session) };

    let id = unsafe { lance_namespace_id(namespace) };
    assert!(!id.is_null());
    assert!(!take_and_free(id).is_empty());

    assert_call_ok(namespace, "create_namespace", r#"{"id":["workspace"]}"#);
    assert_call_ok(namespace, "namespace_exists", r#"{"id":["workspace"]}"#);
    let listed = assert_call_ok(namespace, "list_namespaces", r#"{"id":[]}"#);
    assert!(listed.json().unwrap().contains("workspace"));

    let (schema, batch) = make_batch();
    let table_id = r#"["workspace","numbers"]"#;
    let dataset = write_namespace(namespace, table_id, 0, &schema, batch.clone());
    unsafe { lance_dataset_close(dataset) };

    let count = assert_call_ok(
        namespace,
        "count_table_rows",
        r#"{"id":["workspace","numbers"]}"#,
    );
    assert_eq!(count.json().as_deref(), Some("3"));
    let tables = assert_call_ok(namespace, "list_tables", r#"{"id":["workspace"]}"#);
    assert!(tables.json().unwrap().contains("numbers"));
    let described = assert_call_ok(
        namespace,
        "describe_table",
        r#"{"id":["workspace","numbers"],"load_detailed_metadata":true}"#,
    );
    let described_json: serde_json::Value =
        serde_json::from_str(&described.json().unwrap()).unwrap();
    assert_eq!(described_json["managed_versioning"], true);
    assert!(described_json["location"].is_string());

    let dataset = write_namespace(namespace, table_id, 1, &schema, batch.clone());
    unsafe { lance_dataset_close(dataset) };
    let count = assert_call_ok(
        namespace,
        "count_table_rows",
        r#"{"id":["workspace","numbers"]}"#,
    );
    assert_eq!(count.json().as_deref(), Some("6"));

    let dataset = write_namespace(namespace, table_id, 2, &schema, batch);
    unsafe { lance_dataset_close(dataset) };
    let count = assert_call_ok(
        namespace,
        "count_table_rows",
        r#"{"id":["workspace","numbers"]}"#,
    );
    assert_eq!(count.json().as_deref(), Some("3"));

    let table_id_c = c_string(table_id);
    let opened = unsafe {
        lance_dataset_open_namespace(namespace, table_id_c.as_ptr(), ptr::null(), 0, ptr::null())
    };
    assert!(!opened.is_null(), "namespace open failed: {}", last_error());
    assert_eq!(unsafe { lance_dataset_count_rows(opened) }, 3);
    let read_version = unsafe { lance_dataset_version(opened) };
    unsafe { lance_dataset_close(opened) };

    let predicate = c_string("id < 0");
    let operation =
        unsafe { lance_operation_delete_new(ptr::null(), 0, ptr::null(), 0, predicate.as_ptr()) };
    assert!(
        !operation.is_null(),
        "delete operation failed: {}",
        last_error()
    );
    let transaction = unsafe { lance_transaction_new(read_version, operation, ptr::null()) };
    assert!(
        !transaction.is_null(),
        "transaction failed: {}",
        last_error()
    );
    let location = c_string(described_json["location"].as_str().unwrap());
    let mut committed = ptr::null_mut();
    let rc = unsafe {
        lance_transaction_commit_namespace_uri(
            namespace,
            table_id_c.as_ptr(),
            location.as_ptr(),
            transaction,
            ptr::null(),
            ptr::null(),
            ptr::null(),
            &mut committed,
        )
    };
    assert_eq!(
        rc,
        0,
        "namespace URI commit failed: code={:?} error={}",
        lance_last_error_code(),
        last_error()
    );
    assert!(!committed.is_null());
    unsafe {
        lance_dataset_close(committed);
        lance_transaction_close(transaction);
        lance_operation_close(operation);
    }
    let count = assert_call_ok(
        namespace,
        "count_table_rows",
        r#"{"id":["workspace","numbers"]}"#,
    );
    assert_eq!(count.json().as_deref(), Some("3"));

    let versions = assert_call_ok(
        namespace,
        "list_table_versions",
        r#"{"id":["workspace","numbers"]}"#,
    );
    assert!(versions.json().unwrap().contains("version"));

    // Representative native table metadata, index, tag and branch families.
    let tag = assert_call_ok(
        namespace,
        "create_table_tag",
        r#"{"id":["workspace","numbers"],"tag":"v1","version":3}"#,
    );
    assert!(tag.json().is_some());
    let tag = assert_call_ok(
        namespace,
        "get_table_tag_version",
        r#"{"id":["workspace","numbers"],"tag":"v1"}"#,
    );
    assert!(tag.json().unwrap().contains("\"version\":3"));
    let tags = assert_call_ok(
        namespace,
        "list_table_tags",
        r#"{"id":["workspace","numbers"]}"#,
    );
    assert!(tags.json().unwrap().contains("v1"));
    assert_call_ok(
        namespace,
        "update_table_tag",
        r#"{"id":["workspace","numbers"],"tag":"v1","version":2}"#,
    );
    let tag = assert_call_ok(
        namespace,
        "get_table_tag_version",
        r#"{"id":["workspace","numbers"],"tag":"v1"}"#,
    );
    assert!(tag.json().unwrap().contains("\"version\":2"));
    assert_call_ok(
        namespace,
        "delete_table_tag",
        r#"{"id":["workspace","numbers"],"tag":"v1"}"#,
    );

    assert_call_ok(
        namespace,
        "create_table_branch",
        r#"{"id":["workspace","numbers"],"name":"dev","from_version":3}"#,
    );
    let branches = assert_call_ok(
        namespace,
        "list_table_branches",
        r#"{"id":["workspace","numbers"]}"#,
    );
    assert!(branches.json().unwrap().contains("dev"));
    assert_call_ok(
        namespace,
        "delete_table_branch",
        r#"{"id":["workspace","numbers"],"name":"dev"}"#,
    );

    assert_call_ok(
        namespace,
        "create_table_index",
        r#"{"id":["workspace","numbers"],"column":"id","index_type":"BTREE","name":"numbers_id_idx"}"#,
    );
    let indices = assert_call_ok(
        namespace,
        "list_table_indices",
        r#"{"id":["workspace","numbers"]}"#,
    );
    assert!(indices.json().unwrap().contains("numbers_id_idx"));
    assert_call_ok(
        namespace,
        "describe_table_index_stats",
        r#"{"id":["workspace","numbers"],"index_name":"numbers_id_idx"}"#,
    );
    assert_call_ok(
        namespace,
        "drop_table_index",
        r#"{"id":["workspace","numbers"],"index_name":"numbers_id_idx"}"#,
    );

    let metadata = assert_call_ok(
        namespace,
        "update_table_schema_metadata",
        r#"{"id":["workspace","numbers"],"metadata":{"owner":"namespace-test"}}"#,
    );
    assert!(metadata.json().unwrap().contains("namespace-test"));

    let metrics = unsafe { lance_namespace_retrieve_ops_metrics(namespace) };
    assert!(!metrics.is_null(), "metrics failed: {}", last_error());
    let metrics: serde_json::Value = serde_json::from_str(&take_and_free(metrics)).unwrap();
    let total: u64 = metrics
        .as_object()
        .unwrap()
        .values()
        .map(|value| value.as_u64().unwrap())
        .sum();
    assert!(total > 0, "native operation metrics must record calls");

    assert_eq!(unsafe { lance_namespace_reset_ops_metrics(namespace) }, 0);
    let metrics = unsafe { lance_namespace_retrieve_ops_metrics(namespace) };
    let metrics: serde_json::Value = serde_json::from_str(&take_and_free(metrics)).unwrap();
    let total: u64 = metrics
        .as_object()
        .unwrap()
        .values()
        .map(|value| value.as_u64().unwrap_or(0))
        .sum();
    assert_eq!(total, 0, "reset must clear native counters");

    close_namespace(namespace);
}

unsafe extern "C" fn unsupported_callback(
    _context: *mut c_void,
    _request: *const c_char,
    _response: *mut LanceOwnedString,
    error: *mut LanceCallbackError,
) -> i32 {
    unsafe {
        (*error).code = LanceErrorCode::NotSupported as i32;
        (*error).message.data = owned_string("custom callback does not implement this operation");
        (*error).message.release = Some(free_owned_string);
    }
    7
}

unsafe extern "C" fn free_owned_string(value: *mut LanceOwnedString) {
    unsafe {
        if value.is_null() || (*value).data.is_null() {
            return;
        }
        drop(CString::from_raw((*value).data));
        (*value).data = ptr::null_mut();
    }
}

unsafe extern "C" fn count_release(context: *mut c_void) {
    let counter = unsafe { &*(context.cast::<Arc<AtomicUsize>>()) };
    counter.fetch_add(1, Ordering::SeqCst);
}

#[test]
fn custom_namespace_preserves_native_unsupported_for_all_54_operations() {
    let released = Arc::new(AtomicUsize::new(0));
    let context = Box::into_raw(Box::new(released.clone())) as *mut c_void;
    let namespace_id = c_string("custom-test");
    let spec = LanceNamespaceCustomClientSpec {
        describe_table: Some(unsupported_callback),
        describe_table_version: Some(unsupported_callback),
        create_table_version: Some(unsupported_callback),
        list_table_versions: Some(unsupported_callback),
        release: Some(count_release),
        client_ctx: context,
        namespace_id: namespace_id.as_ptr(),
    };
    let namespace = unsafe { lance_namespace_custom_new(&spec) };
    assert!(
        !namespace.is_null(),
        "custom namespace failed: {}",
        last_error()
    );
    let id = unsafe { lance_namespace_id(namespace) };
    assert_eq!(take_and_free(id), "custom-test");

    let operations = [
        "list_namespaces",
        "describe_namespace",
        "create_namespace",
        "drop_namespace",
        "namespace_exists",
        "list_tables",
        "describe_table",
        "register_table",
        "table_exists",
        "drop_table",
        "deregister_table",
        "count_table_rows",
        "create_table",
        "declare_table",
        "insert_into_table",
        "merge_insert_into_table",
        "update_table",
        "delete_from_table",
        "query_table",
        "create_table_index",
        "list_table_indices",
        "describe_table_index_stats",
        "describe_transaction",
        "alter_transaction",
        "create_table_scalar_index",
        "drop_table_index",
        "list_all_tables",
        "restore_table",
        "rename_table",
        "list_table_versions",
        "create_table_version",
        "describe_table_version",
        "batch_delete_table_versions",
        "batch_create_table_versions",
        "batch_commit_tables",
        "update_table_schema_metadata",
        "update_field_metadata",
        "get_table_stats",
        "explain_table_query_plan",
        "analyze_table_query_plan",
        "alter_table_add_columns",
        "alter_table_alter_columns",
        "alter_table_drop_columns",
        "alter_table_backfill_columns",
        "refresh_materialized_view",
        "create_materialized_view",
        "list_table_tags",
        "get_table_tag_version",
        "create_table_tag",
        "delete_table_tag",
        "update_table_tag",
        "create_table_branch",
        "list_table_branches",
        "delete_table_branch",
    ];
    assert_eq!(operations.len(), 54);
    for operation in operations {
        assert_native_unsupported(namespace, operation, unsupported_request(operation));
    }
    let (rc, result) = call(
        namespace,
        "describe_table",
        r#"{"id":["workspace","table"]}"#,
        None,
    );
    assert_ne!(rc, 0);
    assert_eq!(lance_last_error_code(), LanceErrorCode::NotSupported);
    let (native_code, native_message) = result
        .native_error()
        .expect("callback Unsupported must be preserved");
    assert_eq!(native_code, 0);
    assert!(native_message.contains("custom callback"));

    close_namespace(namespace);
    assert_eq!(released.load(Ordering::SeqCst), 1);
}

#[derive(Default)]
struct DelegateContext {
    directory: *const LanceNamespace,
    describe_calls: AtomicUsize,
    fail_next_version: AtomicUsize,
    read_dataset: *mut LanceDataset,
    reentrant_count: AtomicUsize,
}

unsafe fn delegate_call(
    context: *mut c_void,
    operation: &str,
    request: *const c_char,
    response: *mut LanceOwnedString,
    error: *mut LanceCallbackError,
) -> i32 {
    let context = unsafe { &*(context.cast::<DelegateContext>()) };
    if operation == "describe_table" {
        context.describe_calls.fetch_add(1, Ordering::SeqCst);
    }
    if operation == "create_table_version" {
        if !context.read_dataset.is_null() {
            let count = unsafe { lance_dataset_count_rows(context.read_dataset) };
            context
                .reentrant_count
                .store(count as usize, Ordering::SeqCst);
        }
        if context.fail_next_version.swap(0, Ordering::SeqCst) > 0 {
            unsafe {
                (*error).code = LanceErrorCode::CommitConflict as i32;
                (*error).message.data = owned_string("injected managed version conflict");
                (*error).message.release = Some(free_owned_string);
            }
            return 1;
        }
    }

    // The native callback already runs inside a blocking region. Run the
    // nested namespace call on an independent thread so it can enter the
    // global runtime without nesting block_on on the callback thread.
    let directory = context.directory as usize;
    let request = request as usize;
    let operation = operation.to_owned();
    let (rc, json, code, message) = std::thread::scope(|scope| {
        scope
            .spawn(move || {
                let operation = c_string(&operation);
                let mut result = NamespaceResult::default();
                let rc = unsafe {
                    lance_namespace_call(
                        directory as *const LanceNamespace,
                        operation.as_ptr(),
                        request as *const c_char,
                        ptr::null(),
                        0,
                        &mut result.0,
                    )
                };
                let json = result.json().unwrap_or_default();
                let code = lance_last_error_code() as i32;
                let message = last_error();
                (rc, json, code, message)
            })
            .join()
            .unwrap()
    });

    if rc == 0 {
        unsafe {
            (*response).data = owned_string(&json);
            (*response).release = Some(free_owned_string);
        }
        return 0;
    }
    unsafe {
        (*error).code = code;
        (*error).message.data = owned_string(&message);
        (*error).message.release = Some(free_owned_string);
    }
    1
}

unsafe extern "C" fn delegate_describe_table(
    context: *mut c_void,
    request: *const c_char,
    response: *mut LanceOwnedString,
    error: *mut LanceCallbackError,
) -> i32 {
    unsafe { delegate_with_name(context, "describe_table", request, response, error) }
}

unsafe extern "C" fn delegate_describe_table_version(
    context: *mut c_void,
    request: *const c_char,
    response: *mut LanceOwnedString,
    error: *mut LanceCallbackError,
) -> i32 {
    unsafe { delegate_with_name(context, "describe_table_version", request, response, error) }
}

unsafe extern "C" fn delegate_create_table_version(
    context: *mut c_void,
    request: *const c_char,
    response: *mut LanceOwnedString,
    error: *mut LanceCallbackError,
) -> i32 {
    unsafe { delegate_with_name(context, "create_table_version", request, response, error) }
}

unsafe extern "C" fn delegate_list_table_versions(
    context: *mut c_void,
    request: *const c_char,
    response: *mut LanceOwnedString,
    error: *mut LanceCallbackError,
) -> i32 {
    unsafe { delegate_with_name(context, "list_table_versions", request, response, error) }
}

unsafe fn delegate_with_name(
    context: *mut c_void,
    operation: &str,
    request: *const c_char,
    response: *mut LanceOwnedString,
    error: *mut LanceCallbackError,
) -> i32 {
    unsafe { delegate_call(context, operation, request, response, error) }
}

unsafe extern "C" fn release_delegate(context: *mut c_void) {
    let _ = unsafe { Box::from_raw(context.cast::<DelegateContext>()) };
}

#[test]
fn custom_namespace_managed_versions_and_reentrant_reads() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path().join("namespace");
    std::fs::create_dir_all(&root).unwrap();
    let directory = directory_namespace(root.to_str().unwrap());
    assert_call_ok(directory, "create_namespace", r#"{"id":["workspace"]}"#);
    let (schema, batch) = make_batch();
    let table_id = r#"["workspace","managed"]"#;
    let dataset = write_namespace(directory, table_id, 0, &schema, batch.clone());
    unsafe { lance_dataset_close(dataset) };
    let read_table = c_string(table_id);
    let read_dataset = unsafe {
        lance_dataset_open_namespace(directory, read_table.as_ptr(), ptr::null(), 0, ptr::null())
    };
    assert!(
        !read_dataset.is_null(),
        "read dataset open failed: {}",
        last_error()
    );

    let context = Box::into_raw(Box::new(DelegateContext {
        directory,
        describe_calls: AtomicUsize::new(0),
        fail_next_version: AtomicUsize::new(0),
        read_dataset,
        reentrant_count: AtomicUsize::new(0),
    })) as *mut c_void;
    let namespace_id = c_string("custom-managed");
    let spec = LanceNamespaceCustomClientSpec {
        describe_table: Some(delegate_describe_table),
        describe_table_version: Some(delegate_describe_table_version),
        create_table_version: Some(delegate_create_table_version),
        list_table_versions: Some(delegate_list_table_versions),
        release: Some(release_delegate),
        client_ctx: context,
        namespace_id: namespace_id.as_ptr(),
    };
    let custom = unsafe { lance_namespace_custom_new(&spec) };
    assert!(
        !custom.is_null(),
        "custom namespace failed: {}",
        last_error()
    );

    let dataset = write_namespace(custom, table_id, 1, &schema, batch.clone());
    unsafe { lance_dataset_close(dataset) };
    let table_id_c = c_string(table_id);
    let opened = unsafe {
        lance_dataset_open_namespace(custom, table_id_c.as_ptr(), ptr::null(), 0, ptr::null())
    };
    assert!(
        !opened.is_null(),
        "custom managed open failed: {}",
        last_error()
    );
    assert_eq!(unsafe { lance_dataset_count_rows(opened) }, 6);
    unsafe { lance_dataset_close(opened) };
    let delegate_context = unsafe { &*(context.cast::<DelegateContext>()) };
    assert!(delegate_context.describe_calls.load(Ordering::SeqCst) > 0);
    assert_eq!(
        delegate_context.reentrant_count.load(Ordering::SeqCst),
        3,
        "callback must synchronously read an independent dataset handle"
    );

    delegate_context
        .fail_next_version
        .store(1, Ordering::SeqCst);
    let (rc, failed) = write_namespace_result(custom, table_id, 1, &schema, batch);
    assert_ne!(rc, 0, "conflicting managed version must fail");
    assert!(failed.is_null(), "failed write must not publish a dataset");
    delegate_context
        .fail_next_version
        .store(1, Ordering::SeqCst);
    let (rc, conflict_result) = call(
        custom,
        "create_table_version",
        r#"{"id":["workspace","managed"],"version":1,"manifest_path":"x"}"#,
        None,
    );
    assert_ne!(rc, 0);
    assert_eq!(lance_last_error_code(), LanceErrorCode::CommitConflict);
    assert_eq!(
        conflict_result.native_error(),
        Some((14, "injected managed version conflict".to_owned()))
    );

    close_namespace(custom);
    close_namespace(directory);
    unsafe { lance_dataset_close(read_dataset) };
}

static CAPTURED_REQUESTS: Mutex<Vec<String>> = Mutex::new(Vec::new());

unsafe extern "C" fn capture_describe(
    _context: *mut c_void,
    request: *const c_char,
    response: *mut LanceOwnedString,
    error: *mut LanceCallbackError,
) -> i32 {
    let request = unsafe { CStr::from_ptr(request) }.to_str().unwrap();
    CAPTURED_REQUESTS.lock().unwrap().push(request.to_owned());
    unsafe {
        (*response).data = owned_string("{}");
        (*response).release = Some(free_owned_string);
        let _ = error;
    }
    0
}

unsafe extern "C" fn authorization_context(
    context: *mut c_void,
    _operation: *const c_char,
    _object_id: *const c_char,
    response: *mut LanceOwnedString,
    error: *mut LanceCallbackError,
) -> i32 {
    let counter = unsafe { &*(context.cast::<Arc<AtomicUsize>>()) };
    counter.fetch_add(1, Ordering::SeqCst);
    unsafe {
        (*response).data = owned_string(r#"{"headers.Authorization":"Bearer header-token"}"#);
        (*response).release = Some(free_owned_string);
        let _ = error;
    }
    0
}

unsafe extern "C" fn dynamic_context(
    context: *mut c_void,
    operation: *const c_char,
    object_id: *const c_char,
    response: *mut LanceOwnedString,
    error: *mut LanceCallbackError,
) -> i32 {
    let counter = unsafe { &*(context.cast::<Arc<AtomicUsize>>()) };
    counter.fetch_add(1, Ordering::SeqCst);
    let operation = unsafe { CStr::from_ptr(operation) }.to_str().unwrap();
    let object_id = unsafe { CStr::from_ptr(object_id) }.to_str().unwrap();
    assert!(!operation.is_empty());
    let _ = object_id;
    unsafe {
        (*response).data = owned_string(r#"{"headers.X-Test-Context":"dynamic"}"#);
        (*response).release = Some(free_owned_string);
        let _ = error;
    }
    0
}

unsafe extern "C" fn failing_dynamic_context(
    _context: *mut c_void,
    _operation: *const c_char,
    _object_id: *const c_char,
    _response: *mut LanceOwnedString,
    error: *mut LanceCallbackError,
) -> i32 {
    unsafe {
        (*error).code = LanceErrorCode::Internal as i32;
        (*error).message.data = owned_string("context provider failure");
        (*error).message.release = Some(free_owned_string);
    }
    6
}

#[test]
fn rest_adapter_client_context_headers_metrics_and_native_errors() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path().join("backend");
    std::fs::create_dir_all(&root).unwrap();
    let backend = directory_namespace(root.to_str().unwrap());
    assert_call_ok(backend, "create_namespace", r#"{"id":["workspace"]}"#);

    let mut adapter = ptr::null_mut();
    let host = c_string("127.0.0.1");
    let rc = unsafe { lance_rest_adapter_start(backend, host.as_ptr(), 0, &mut adapter) };
    assert_eq!(rc, 0, "adapter start failed: {}", last_error());
    assert!(!adapter.is_null());
    let port = unsafe { lance_rest_adapter_port(adapter) };
    assert_ne!(port, 0, "ephemeral adapter port");
    // The adapter owns a native backend Arc; closing the caller handle must not
    // invalidate in-flight or subsequent requests.
    close_namespace(backend);

    let calls = Arc::new(AtomicUsize::new(0));
    let context = Box::into_raw(Box::new(calls.clone())) as *mut c_void;
    let provider_spec = LanceNamespaceContextProviderSpec {
        provide_context: Some(dynamic_context),
        release: Some(count_release),
        provider_ctx: context,
    };
    let properties = c_string(&format!(
        r#"{{"uri":"http://127.0.0.1:{port}","ops_metrics_enabled":"true"}}"#
    ));
    let client = unsafe { lance_namespace_rest_new(properties.as_ptr(), &provider_spec) };
    assert!(!client.is_null(), "REST namespace failed: {}", last_error());

    assert_call_ok(client, "namespace_exists", r#"{"id":["workspace"]}"#);
    let (schema, batch) = make_batch();
    let table_id = r#"["workspace","rest-table"]"#;
    let dataset = write_namespace(client, table_id, 0, &schema, batch);
    unsafe { lance_dataset_close(dataset) };
    let (vector_schema, vector_batch) = make_vector_batch();
    let vector_table = r#"["workspace","rest-vectors"]"#;
    let dataset = write_namespace(client, vector_table, 0, &vector_schema, vector_batch);
    unsafe { lance_dataset_close(dataset) };
    let (rc, query) = call(
        client,
        "query_table",
        r#"{"id":["workspace","rest-vectors"],"k":2,"vector":{"single_vector":[0.0,0.0]},"vector_column":"vector"}"#,
        None,
    );
    assert_eq!(rc, 0, "REST query failed: {}", last_error());
    assert!(
        query.bytes().windows(6).any(|window| window == b"ARROW1"),
        "query_table must return a native Arrow IPC stream"
    );
    let count = assert_call_ok(
        client,
        "count_table_rows",
        r#"{"id":["workspace","rest-table"]}"#,
    );
    assert_eq!(count.json().as_deref(), Some("3"));
    assert!(
        calls.load(Ordering::SeqCst) > 0,
        "context provider must run"
    );

    // DynamicContextProvider is infallible: a foreign context failure is
    // logged natively and the request continues with an empty context map.
    let failing_spec = LanceNamespaceContextProviderSpec {
        provide_context: Some(failing_dynamic_context),
        release: Some(count_release),
        provider_ctx: Box::into_raw(Box::new(Arc::new(AtomicUsize::new(0)))).cast(),
    };
    let failing_client = unsafe { lance_namespace_rest_new(properties.as_ptr(), &failing_spec) };
    assert!(
        !failing_client.is_null(),
        "REST namespace with failing context failed: {}",
        last_error()
    );
    let count = assert_call_ok(
        failing_client,
        "count_table_rows",
        r#"{"id":["workspace","rest-table"]}"#,
    );
    assert_eq!(count.json().as_deref(), Some("3"));
    close_namespace(failing_client);

    let table_id_c = c_string(table_id);
    let opened = unsafe {
        lance_dataset_open_namespace(client, table_id_c.as_ptr(), ptr::null(), 0, ptr::null())
    };
    assert!(
        !opened.is_null(),
        "REST namespace open failed: {}",
        last_error()
    );
    assert_eq!(unsafe { lance_dataset_count_rows(opened) }, 3);
    unsafe { lance_dataset_close(opened) };

    let (rc, result) = call(
        client,
        "describe_table",
        r#"{"id":["workspace","missing"]}"#,
        None,
    );
    assert_ne!(rc, 0);
    assert_eq!(lance_last_error_code(), LanceErrorCode::NotFound);
    assert_eq!(result.native_error().unwrap().0, 4);

    // A dynamic context header reaches the native adapter as caller identity.
    CAPTURED_REQUESTS.lock().unwrap().clear();
    let capture_release = Arc::new(AtomicUsize::new(0));
    let header_namespace_id = c_string("header-capture");
    let capture_spec = LanceNamespaceCustomClientSpec {
        describe_table: Some(capture_describe),
        describe_table_version: Some(unsupported_callback),
        create_table_version: Some(unsupported_callback),
        list_table_versions: Some(unsupported_callback),
        release: Some(count_release),
        client_ctx: Box::into_raw(Box::new(capture_release.clone())).cast(),
        namespace_id: header_namespace_id.as_ptr(),
    };
    let capture_namespace = unsafe { lance_namespace_custom_new(&capture_spec) };
    assert!(
        !capture_namespace.is_null(),
        "capture namespace: {}",
        last_error()
    );
    let mut capture_adapter = ptr::null_mut();
    let rc = unsafe {
        lance_rest_adapter_start(capture_namespace, host.as_ptr(), 0, &mut capture_adapter)
    };
    assert_eq!(rc, 0, "capture adapter: {}", last_error());
    let capture_port = unsafe { lance_rest_adapter_port(capture_adapter) };
    // The custom namespace context is released only after the adapter's final
    // native Arc owner drops, not when the caller handle closes.
    close_namespace(capture_namespace);
    let call_count = Arc::new(AtomicUsize::new(0));
    let auth_spec = LanceNamespaceContextProviderSpec {
        provide_context: Some(authorization_context),
        release: Some(count_release),
        provider_ctx: Box::into_raw(Box::new(call_count.clone())).cast(),
    };
    let capture_properties = c_string(&format!(r#"{{"uri":"http://127.0.0.1:{capture_port}"}}"#));
    let capture_client =
        unsafe { lance_namespace_rest_new(capture_properties.as_ptr(), &auth_spec) };
    assert!(
        !capture_client.is_null(),
        "capture client: {}",
        last_error()
    );
    assert_call_ok(
        capture_client,
        "describe_table",
        r#"{"id":["workspace","header-table"]}"#,
    );
    let captured = CAPTURED_REQUESTS.lock().unwrap().join("\n");
    assert!(
        captured.contains("\"auth_token\":\"header-token\""),
        "context header did not reach the adapter: {captured}"
    );
    assert!(call_count.load(Ordering::SeqCst) > 0);
    unsafe { lance_rest_adapter_shutdown(capture_adapter) };
    unsafe { lance_rest_adapter_close(capture_adapter) };
    close_namespace(capture_client);

    let metrics = unsafe { lance_namespace_retrieve_ops_metrics(client) };
    assert!(!metrics.is_null());
    let metrics: serde_json::Value = serde_json::from_str(&take_and_free(metrics)).unwrap();
    assert!(
        metrics
            .as_object()
            .is_some_and(|metrics| !metrics.is_empty()),
        "REST client metrics missing: {metrics}"
    );
    assert_eq!(unsafe { lance_namespace_reset_ops_metrics(client) }, 0);
    let metrics = unsafe { lance_namespace_retrieve_ops_metrics(client) };
    let metrics: serde_json::Value = serde_json::from_str(&take_and_free(metrics)).unwrap();
    assert_eq!(metrics, serde_json::json!({}), "REST metrics reset");
    unsafe { lance_rest_adapter_shutdown(adapter) };
    unsafe { lance_rest_adapter_close(adapter) };
    close_namespace(client);
}

#[test]
fn namespace_provider_identity_and_forced_refresh() {
    let fetch_count = Arc::new(AtomicUsize::new(0));
    // A custom namespace's describe callback returns changing storage options.
    let context = Box::into_raw(Box::new(fetch_count.clone())) as *mut c_void;
    let namespace_id = c_string("provider-backend");
    let spec = LanceNamespaceCustomClientSpec {
        describe_table: Some(changing_storage_describe),
        describe_table_version: Some(unsupported_callback),
        create_table_version: Some(unsupported_callback),
        list_table_versions: Some(unsupported_callback),
        release: Some(count_release),
        client_ctx: context,
        namespace_id: namespace_id.as_ptr(),
    };
    let namespace = unsafe { lance_namespace_custom_new(&spec) };
    assert!(
        !namespace.is_null(),
        "custom namespace failed: {}",
        last_error()
    );
    let table_id = c_string(r#"["workspace","provider-table"]"#);
    let provider =
        unsafe { lance_namespace_storage_options_provider_new(namespace, table_id.as_ptr()) };
    assert!(
        !provider.is_null(),
        "provider creation failed: {}",
        last_error()
    );
    let id = unsafe { lance_storage_options_provider_id(provider) };
    let id = take_and_free(id);
    assert!(id.contains("provider-backend"), "provider id: {id}");
    assert!(id.contains("provider-table"), "provider id: {id}");

    let accessor = unsafe { lance_storage_options_accessor_new(ptr::null(), provider) };
    assert!(
        !accessor.is_null(),
        "accessor creation failed: {}",
        last_error()
    );
    let first = unsafe { lance_storage_options_accessor_snapshot(accessor) };
    assert!(!first.is_null(), "first snapshot failed: {}", last_error());
    let first_value = snapshot_value(first, "token");
    unsafe { lance_storage_options_snapshot_close(first) };

    let second = unsafe { lance_storage_options_accessor_snapshot(accessor) };
    assert!(
        !second.is_null(),
        "second snapshot failed: {}",
        last_error()
    );
    let second_value = snapshot_value(second, "token");
    unsafe { lance_storage_options_snapshot_close(second) };
    assert_ne!(
        first_value, second_value,
        "forced refresh must call native fetch again"
    );
    assert!(fetch_count.load(Ordering::SeqCst) >= 2);

    unsafe { lance_storage_options_accessor_close(accessor) };
    unsafe { lance_storage_options_provider_close(provider) };
    close_namespace(namespace);
}

unsafe extern "C" fn changing_storage_describe(
    context: *mut c_void,
    _request: *const c_char,
    response: *mut LanceOwnedString,
    error: *mut LanceCallbackError,
) -> i32 {
    let counter = unsafe { &*(context.cast::<Arc<AtomicUsize>>()) };
    let value = counter.fetch_add(1, Ordering::SeqCst);
    let json = format!(r#"{{"storage_options":{{"token":"v{value}","expires_at_millis":"0"}}}}"#);
    unsafe {
        (*response).data = owned_string(&json);
        (*response).release = Some(free_owned_string);
        let _ = error;
    }
    0
}

fn snapshot_value(snapshot: *const LanceStorageOptionsSnapshot, key: &str) -> String {
    let count = unsafe { lance_storage_options_snapshot_count(snapshot) };
    for index in 0..count {
        let index = usize::try_from(index).unwrap();
        let candidate = unsafe { lance_storage_options_snapshot_key_at(snapshot, index) };
        assert!(!candidate.is_null());
        if unsafe { CStr::from_ptr(candidate) }.to_str().unwrap() == key {
            let value = unsafe { lance_storage_options_snapshot_value_at(snapshot, index) };
            return unsafe { CStr::from_ptr(value) }
                .to_str()
                .unwrap()
                .to_owned();
        }
    }
    panic!("missing storage option {key}");
}

#[test]
fn constructor_failures_release_foreign_contexts_exactly_once() {
    let released = Arc::new(AtomicUsize::new(0));
    let context = Box::into_raw(Box::new(released.clone())) as *mut c_void;
    let namespace_id = c_string("incomplete");
    let spec = LanceNamespaceCustomClientSpec {
        describe_table: Some(unsupported_callback),
        describe_table_version: Some(unsupported_callback),
        create_table_version: Some(unsupported_callback),
        list_table_versions: None,
        release: Some(count_release),
        client_ctx: context,
        namespace_id: namespace_id.as_ptr(),
    };
    let namespace = unsafe { lance_namespace_custom_new(&spec) };
    assert!(namespace.is_null(), "incomplete custom client must fail");
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(released.load(Ordering::SeqCst), 1);

    let provider_context = Box::into_raw(Box::new(released.clone())) as *mut c_void;
    let provider_spec = LanceNamespaceContextProviderSpec {
        provide_context: Some(dynamic_context),
        release: Some(count_release),
        provider_ctx: provider_context,
    };
    let invalid_properties = c_string("{not json");
    let namespace =
        unsafe { lance_namespace_rest_new(invalid_properties.as_ptr(), &provider_spec) };
    assert!(namespace.is_null(), "invalid REST properties must fail");
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(released.load(Ordering::SeqCst), 2);
}

static PARTIAL_RESPONSE_RELEASES: AtomicUsize = AtomicUsize::new(0);

unsafe extern "C" fn free_counted_owned_string(value: *mut LanceOwnedString) {
    unsafe {
        if value.is_null() || (*value).data.is_null() {
            return;
        }
        PARTIAL_RESPONSE_RELEASES.fetch_add(1, Ordering::SeqCst);
        free_owned_string(value);
    }
}

unsafe extern "C" fn partial_response_then_error(
    _context: *mut c_void,
    _request: *const c_char,
    response: *mut LanceOwnedString,
    error: *mut LanceCallbackError,
) -> i32 {
    unsafe {
        (*response).data = owned_string(r#"{"location":"partial"}"#);
        (*response).release = Some(free_counted_owned_string);
        (*error).code = LanceErrorCode::Internal as i32;
        (*error).message.data = owned_string("callback failed after writing a response");
        (*error).message.release = Some(free_owned_string);
    }
    1
}

#[test]
fn failed_custom_callback_releases_partial_response_exactly_once() {
    PARTIAL_RESPONSE_RELEASES.store(0, Ordering::SeqCst);
    let released = Arc::new(AtomicUsize::new(0));
    let context = Box::into_raw(Box::new(released.clone())) as *mut c_void;
    let namespace_id = c_string("partial");
    let spec = LanceNamespaceCustomClientSpec {
        describe_table: Some(partial_response_then_error),
        describe_table_version: Some(unsupported_callback),
        create_table_version: Some(unsupported_callback),
        list_table_versions: Some(unsupported_callback),
        release: Some(count_release),
        client_ctx: context,
        namespace_id: namespace_id.as_ptr(),
    };
    let namespace = unsafe { lance_namespace_custom_new(&spec) };
    assert!(
        !namespace.is_null(),
        "custom namespace failed: {}",
        last_error()
    );

    let (rc, result) = call(
        namespace,
        "describe_table",
        r#"{"id":["workspace","table"]}"#,
        None,
    );
    assert_ne!(rc, 0);
    assert_eq!(
        result.native_error(),
        Some((18, "callback failed after writing a response".to_owned()))
    );
    assert_eq!(PARTIAL_RESPONSE_RELEASES.load(Ordering::SeqCst), 1);
    close_namespace(namespace);
    assert_eq!(released.load(Ordering::SeqCst), 1);
}
