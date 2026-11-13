// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Native `NewColumnTransform::BatchUDF` and checkpoint acceptance.

use std::alloc::{Layout, alloc, dealloc};
use std::collections::{BTreeMap, HashMap};
use std::ffi::{CStr, CString, c_char, c_void};
use std::mem;
use std::ptr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema, from_ffi};
use arrow::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use arrow::record_batch::RecordBatchReader;
use arrow_array::{
    Array, Int32Array, Int64Array, RecordBatch, RecordBatchOptions, StringArray, StructArray,
};
use arrow_schema::{DataType, Field, Schema};
use lance::Dataset;
use lance::dataset::{BatchUDF as NativeBatchUDF, NewColumnTransform};
use lance_c::*;
use lance_table::format::Fragment;

fn c_string(value: &str) -> CString {
    CString::new(value).unwrap()
}

fn schema_to_ffi(schema: &Schema) -> FFI_ArrowSchema {
    FFI_ArrowSchema::try_from(schema).unwrap()
}

fn take_error() -> (LanceErrorCode, String) {
    let code = lance_last_error_code();
    let message = lance_last_error_message();
    let text = if message.is_null() {
        String::new()
    } else {
        let text = unsafe { CStr::from_ptr(message) }
            .to_string_lossy()
            .into_owned();
        unsafe { lance_free_string(message) };
        text
    };
    (code, text)
}

fn open(uri: &str) -> *mut LanceDataset {
    let uri = c_string(uri);
    let dataset = unsafe { lance_dataset_open(uri.as_ptr(), ptr::null(), 0) };
    assert!(!dataset.is_null(), "dataset open failed");
    dataset
}

fn open_version(uri: &str, version: u64) -> *mut LanceDataset {
    let uri = c_string(uri);
    let dataset = unsafe { lance_dataset_open(uri.as_ptr(), ptr::null(), version) };
    assert!(
        !dataset.is_null(),
        "dataset open at version {version} failed: {:?}",
        take_error()
    );
    dataset
}

/// Clear the observations of one checkpoint-store run without dropping the
/// values the store already owns.
fn reset_store(store: &StoreState) {
    store.events.lock().unwrap().clear();
    store.calls.lock().unwrap().clear();
    store.inserted_batches.lock().unwrap().clear();
    store.inserted_fragments.lock().unwrap().clear();
}

fn scan_batches(dataset: *const LanceDataset) -> Vec<RecordBatch> {
    let scanner = unsafe { lance_scanner_new(dataset, ptr::null(), ptr::null()) };
    assert!(!scanner.is_null(), "scanner creation failed");
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream) },
        0
    );
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream) }.unwrap();
    let batches = reader.map(Result::unwrap).collect();
    unsafe { lance_scanner_close(scanner) };
    batches
}

fn exported_schema(dataset: *const LanceDataset) -> Schema {
    let mut schema = FFI_ArrowSchema::empty();
    assert_eq!(unsafe { lance_dataset_schema(dataset, &mut schema) }, 0);
    let result = Schema::try_from(&schema).unwrap();
    drop(schema);
    result
}

fn fixture_uri(root: &tempfile::TempDir, name: &str) -> String {
    root.path().join(name).to_str().unwrap().to_owned()
}

fn create_batch_udf_dataset(uri: &str, stable: bool) {
    let metadata = HashMap::from([("dataset-key".to_owned(), "dataset-value".to_owned())]);
    let schema = Arc::new(Schema::new_with_metadata(
        vec![
            Field::new("id", DataType::Int32, false).with_metadata(HashMap::from([(
                "field-key".to_owned(),
                "field-value".to_owned(),
            )])),
            Field::new("label", DataType::Utf8, true),
        ],
        metadata,
    ));
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from_iter_values(0..8)),
            Arc::new(StringArray::from(
                (0..8)
                    .map(|value| format!("row-{value}"))
                    .collect::<Vec<_>>(),
            )),
        ],
    )
    .unwrap();
    lance_c::runtime::block_on(async {
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
            uri,
            Some(lance::dataset::WriteParams {
                max_rows_per_file: 4,
                enable_stable_row_ids: stable,
                ..Default::default()
            }),
        )
        .await
        .unwrap();
    });
}

fn output_schema() -> Arc<Schema> {
    Arc::new(Schema::new_with_metadata(
        vec![
            Field::new("derived", DataType::Int64, true)
                .with_metadata(HashMap::from([("unit".to_owned(), "native".to_owned())])),
        ],
        HashMap::from([("producer".to_owned(), "callback-test".to_owned())]),
    ))
}

fn empty_output_schema() -> Arc<Schema> {
    Arc::new(Schema::empty())
}

#[derive(Clone)]
enum MapperMode {
    Constant(i64),
    ZeroColumns,
    RowMismatch(i64),
    NoOutput,
    Error {
        code: i32,
        message: String,
        partial_output: bool,
    },
}

#[derive(Clone, Debug)]
struct MapperEvent {
    input_fields: Vec<String>,
    rows: usize,
}

struct MapperState {
    output_schema: Arc<Schema>,
    mode: MapperMode,
    calls: AtomicUsize,
    events: Mutex<Vec<MapperEvent>>,
    string_releases: Arc<AtomicUsize>,
    partial_releases: Arc<AtomicUsize>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
enum StoreMethod {
    GetFragment,
    GetBatch,
    InsertBatch,
    InsertFragment,
}

struct Failure {
    method: StoreMethod,
    call_index: usize,
    code: i32,
    message: String,
}

struct StoredArrow {
    array: FFI_ArrowArray,
    schema: FFI_ArrowSchema,
}

struct StoreState {
    events: Arc<Mutex<Vec<String>>>,
    calls: Mutex<HashMap<StoreMethod, usize>>,
    failure: Mutex<Option<Failure>>,
    fragment_hits: Mutex<HashMap<u32, String>>,
    batch_hits: Mutex<HashMap<(u32, usize), RecordBatch>>,
    inserted_batches: Mutex<Vec<StoredArrow>>,
    inserted_fragments: Mutex<Vec<(u64, String)>>,
    string_releases: Arc<AtomicUsize>,
}

impl StoreState {
    fn new(events: Arc<Mutex<Vec<String>>>, string_releases: Arc<AtomicUsize>) -> Self {
        Self {
            events,
            calls: Mutex::new(HashMap::new()),
            failure: Mutex::new(None),
            fragment_hits: Mutex::new(HashMap::new()),
            batch_hits: Mutex::new(HashMap::new()),
            inserted_batches: Mutex::new(Vec::new()),
            inserted_fragments: Mutex::new(Vec::new()),
            string_releases,
        }
    }

    fn record(&self, event: impl Into<String>) {
        self.events.lock().unwrap().push(event.into());
    }

    fn should_fail(&self, method: StoreMethod) -> Option<(i32, String)> {
        let call_index = {
            let mut calls = self.calls.lock().unwrap();
            let entry = calls.entry(method).or_insert(0);
            let current = *entry;
            *entry += 1;
            current
        };
        self.failure
            .lock()
            .unwrap()
            .as_ref()
            .filter(|failure| failure.method == method && failure.call_index == call_index)
            .map(|failure| (failure.code, failure.message.clone()))
    }
}

struct CallbackContext {
    mapper: Arc<MapperState>,
    store: Arc<StoreState>,
    independent_reads: Option<CallbackReads>,
}

struct CallbackRead {
    callback: &'static str,
    count: u64,
    ids: Result<Vec<i32>, String>,
}

struct CallbackReads {
    dataset: *const LanceDataset,
    scanners: Mutex<Vec<*mut LanceScanner>>,
    results: Mutex<Vec<CallbackRead>>,
}

impl CallbackReads {
    fn record(&self, index: usize, callback: &'static str) {
        let scanner = {
            let mut scanners = self.scanners.lock().unwrap();
            mem::replace(&mut scanners[index], ptr::null_mut())
        };
        if scanner.is_null() {
            return;
        }
        let count = unsafe { lance_dataset_count_rows(self.dataset) };
        let ids = (|| {
            let mut stream = FFI_ArrowArrayStream::empty();
            if unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream) } != 0 {
                return Err(format!("scanner export failed: {:?}", take_error()));
            }
            let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream) }
                .map_err(|error| error.to_string())?;
            if reader.schema().field_with_name("derived").is_ok() {
                return Err("pre-existing scanner lost its snapshot".to_owned());
            }
            let mut ids = Vec::new();
            for batch in reader {
                let batch = batch.map_err(|error| error.to_string())?;
                let column = batch
                    .column_by_name("id")
                    .and_then(|column| column.as_any().downcast_ref::<Int32Array>())
                    .ok_or_else(|| "scanner did not return Int32 ids".to_owned())?;
                ids.extend_from_slice(column.values());
            }
            ids.sort_unstable();
            Ok(ids)
        })();
        unsafe { lance_scanner_close(scanner) };
        self.results.lock().unwrap().push(CallbackRead {
            callback,
            count,
            ids,
        });
    }
}

impl Drop for CallbackReads {
    fn drop(&mut self) {
        for scanner in self.scanners.get_mut().unwrap().drain(..) {
            unsafe { lance_scanner_close(scanner) };
        }
    }
}

#[repr(C)]
struct MessageAllocation {
    releases: *const AtomicUsize,
    total_size: usize,
    alignment: usize,
}

unsafe fn owned_message(value: &str, releases: &Arc<AtomicUsize>) -> LanceOwnedString {
    let total_size = mem::size_of::<MessageAllocation>() + value.len() + 1;
    let alignment = mem::align_of::<MessageAllocation>();
    let layout = Layout::from_size_align(total_size, alignment).unwrap();
    let base = unsafe { alloc(layout) };
    assert!(!base.is_null(), "test message allocation failed");
    let header = MessageAllocation {
        releases: Arc::as_ptr(releases),
        total_size,
        alignment,
    };
    unsafe {
        ptr::write(base.cast::<MessageAllocation>(), header);
        let data = base.add(mem::size_of::<MessageAllocation>());
        ptr::copy_nonoverlapping(value.as_ptr(), data, value.len());
        *data.add(value.len()) = 0;
    }
    LanceOwnedString {
        data: unsafe {
            base.add(mem::size_of::<MessageAllocation>())
                .cast::<c_char>()
        },
        release: Some(release_message),
    }
}

unsafe extern "C" fn release_message(value: *mut LanceOwnedString) {
    if value.is_null() {
        return;
    }
    let value = unsafe { &mut *value };
    if !value.data.is_null() {
        let base = unsafe {
            value
                .data
                .cast::<u8>()
                .sub(mem::size_of::<MessageAllocation>())
        };
        let header = unsafe { &*base.cast::<MessageAllocation>() };
        if let Some(releases) = unsafe { header.releases.as_ref() } {
            releases.fetch_add(1, Ordering::SeqCst);
        }
        let layout = Layout::from_size_align(header.total_size, header.alignment).unwrap();
        unsafe { dealloc(base, layout) };
    }
    value.data = ptr::null_mut();
    value.release = None;
}

unsafe extern "C" fn release_partial_array(value: *mut FFI_ArrowArray) {
    if value.is_null() {
        return;
    }
    let private_data = unsafe { (*value).private_data };
    if !private_data.is_null() {
        let releases = unsafe { Box::from_raw(private_data.cast::<Arc<AtomicUsize>>()) };
        releases.fetch_add(1, Ordering::SeqCst);
    }
    unsafe {
        (*value).release = None;
        (*value).private_data = ptr::null_mut();
    }
}

unsafe fn set_callback_error(
    error: *mut LanceCallbackError,
    code: i32,
    message: &str,
    releases: &Arc<AtomicUsize>,
) {
    if error.is_null() {
        return;
    }
    unsafe {
        (*error).code = code;
        (*error).message = owned_message(message, releases);
    }
}

unsafe fn set_owned_json(out: *mut LanceOwnedString, value: &str, releases: &Arc<AtomicUsize>) {
    unsafe { ptr::write(out, owned_message(value, releases)) };
}

fn emit_batch(out: *mut FFI_ArrowArray, batch: RecordBatch) {
    let struct_array: StructArray = batch.into();
    let array = FFI_ArrowArray::new(&struct_array.into_data());
    unsafe { ptr::write(out, array) };
}

unsafe extern "C" fn mapper_callback(
    callback_ctx: *mut c_void,
    input: *const FFI_ArrowArray,
    input_schema: *const FFI_ArrowSchema,
    output: *mut FFI_ArrowArray,
    error: *mut LanceCallbackError,
) -> i32 {
    let context = unsafe { &*(callback_ctx.cast::<CallbackContext>()) };
    if let Some(reads) = &context.independent_reads {
        reads.record(0, "mapper");
    }
    let input_schema = Schema::try_from(unsafe { &*input_schema }).unwrap();
    let rows = unsafe { (*input).length as usize };
    context.mapper.calls.fetch_add(1, Ordering::SeqCst);
    context
        .store
        .events
        .lock()
        .unwrap()
        .push("mapper".to_owned());
    context.mapper.events.lock().unwrap().push(MapperEvent {
        input_fields: input_schema
            .fields()
            .iter()
            .map(|field| field.name().clone())
            .collect(),
        rows,
    });

    match &context.mapper.mode {
        MapperMode::Constant(value) => {
            let batch = RecordBatch::try_new(
                context.mapper.output_schema.clone(),
                vec![Arc::new(Int64Array::from(vec![*value; rows]))],
            )
            .unwrap();
            emit_batch(output, batch);
            0
        }
        MapperMode::ZeroColumns => {
            let batch = RecordBatch::try_new_with_options(
                context.mapper.output_schema.clone(),
                Vec::new(),
                &RecordBatchOptions::new().with_row_count(Some(rows)),
            )
            .unwrap();
            emit_batch(output, batch);
            0
        }
        MapperMode::RowMismatch(value) => {
            let batch = RecordBatch::try_new(
                context.mapper.output_schema.clone(),
                vec![Arc::new(Int64Array::from(vec![*value; rows + 1]))],
            )
            .unwrap();
            emit_batch(output, batch);
            0
        }
        MapperMode::NoOutput => 0,
        MapperMode::Error {
            code,
            message,
            partial_output,
        } => {
            if *partial_output {
                let releases = Box::into_raw(Box::new(context.mapper.partial_releases.clone()));
                unsafe {
                    (*output).length = rows as i64;
                    (*output).release = Some(release_partial_array);
                    (*output).private_data = releases.cast();
                }
            }
            unsafe { set_callback_error(error, *code, message, &context.mapper.string_releases) };
            -1
        }
    }
}

unsafe extern "C" fn store_get_batch(
    callback_ctx: *mut c_void,
    info: *const LanceUDFBatchInfo,
    output: *mut FFI_ArrowArray,
    error: *mut LanceCallbackError,
) -> i32 {
    let context = unsafe { &*(callback_ctx.cast::<CallbackContext>()) };
    if let Some(reads) = &context.independent_reads {
        reads.record(1, "get_batch");
    }
    let info = unsafe { &*info };
    context.store.record(format!(
        "get_batch:{}:{}",
        info.fragment_id, info.batch_index
    ));
    if let Some((code, message)) = context.store.should_fail(StoreMethod::GetBatch) {
        unsafe { set_callback_error(error, code, &message, &context.store.string_releases) };
        return -1;
    }
    let batch = context
        .store
        .batch_hits
        .lock()
        .unwrap()
        .get(&(info.fragment_id, info.batch_index))
        .cloned();
    if let Some(batch) = batch {
        emit_batch(output, batch);
        1
    } else {
        0
    }
}

unsafe extern "C" fn store_insert_batch(
    callback_ctx: *mut c_void,
    info: *const LanceUDFBatchInfo,
    batch: *mut FFI_ArrowArray,
    batch_schema: *mut FFI_ArrowSchema,
    error: *mut LanceCallbackError,
) -> i32 {
    let context = unsafe { &*(callback_ctx.cast::<CallbackContext>()) };
    if let Some(reads) = &context.independent_reads {
        reads.record(2, "insert_batch");
    }
    let info = unsafe { &*info };
    context.store.record(format!(
        "insert_batch:{}:{}",
        info.fragment_id, info.batch_index
    ));
    if let Some((code, message)) = context.store.should_fail(StoreMethod::InsertBatch) {
        unsafe { set_callback_error(error, code, &message, &context.store.string_releases) };
        return -1;
    }

    let array = unsafe { ptr::read(batch) };
    let schema = unsafe { ptr::read(batch_schema) };
    unsafe {
        ptr::write(batch, FFI_ArrowArray::empty());
        ptr::write(batch_schema, FFI_ArrowSchema::empty());
    }
    context
        .store
        .inserted_batches
        .lock()
        .unwrap()
        .push(StoredArrow { array, schema });
    0
}

unsafe extern "C" fn store_get_fragment(
    callback_ctx: *mut c_void,
    fragment_id: u32,
    out_json: *mut LanceOwnedString,
    error: *mut LanceCallbackError,
) -> i32 {
    let context = unsafe { &*(callback_ctx.cast::<CallbackContext>()) };
    if let Some(reads) = &context.independent_reads {
        reads.record(3, "get_fragment");
    }
    context.store.record(format!("get_fragment:{fragment_id}"));
    if let Some((code, message)) = context.store.should_fail(StoreMethod::GetFragment) {
        unsafe { set_callback_error(error, code, &message, &context.store.string_releases) };
        return -1;
    }
    let json = context
        .store
        .fragment_hits
        .lock()
        .unwrap()
        .get(&fragment_id)
        .cloned();
    if let Some(json) = json {
        unsafe { set_owned_json(out_json, &json, &context.store.string_releases) };
        1
    } else {
        0
    }
}

unsafe extern "C" fn store_insert_fragment(
    callback_ctx: *mut c_void,
    fragment_id: u64,
    fragment_json: *const c_char,
    error: *mut LanceCallbackError,
) -> i32 {
    let context = unsafe { &*(callback_ctx.cast::<CallbackContext>()) };
    if let Some(reads) = &context.independent_reads {
        reads.record(4, "insert_fragment");
    }
    context
        .store
        .record(format!("insert_fragment:{fragment_id}"));
    if let Some((code, message)) = context.store.should_fail(StoreMethod::InsertFragment) {
        unsafe { set_callback_error(error, code, &message, &context.store.string_releases) };
        return -1;
    }
    let json = unsafe { CStr::from_ptr(fragment_json) }
        .to_str()
        .unwrap()
        .to_owned();
    context
        .store
        .inserted_fragments
        .lock()
        .unwrap()
        .push((fragment_id, json));
    0
}

fn checkpoint_store(context: &CallbackContext) -> LanceUDFCheckpointStore {
    LanceUDFCheckpointStore {
        get_batch: Some(store_get_batch),
        insert_batch: Some(store_insert_batch),
        get_fragment: Some(store_get_fragment),
        insert_fragment: Some(store_insert_fragment),
        callback_ctx: (context as *const CallbackContext).cast_mut().cast(),
    }
}

fn run_batch_udf(
    dataset: *mut LanceDataset,
    declared_schema: &FFI_ArrowSchema,
    context: &CallbackContext,
    read_columns: Option<&[&str]>,
    batch_size: Option<u32>,
    with_checkpoint: bool,
) -> i32 {
    let read_strings = read_columns.map(|columns| {
        columns
            .iter()
            .map(|column| c_string(column))
            .collect::<Vec<_>>()
    });
    let mut read_ptrs = read_strings.as_ref().map(|columns| {
        columns
            .iter()
            .map(|column| column.as_ptr())
            .collect::<Vec<_>>()
    });
    if let Some(ptrs) = read_ptrs.as_mut() {
        ptrs.push(ptr::null());
    }
    let batch_size_value = batch_size.unwrap_or(0);
    let batch_size_ptr = if batch_size.is_some() {
        &batch_size_value as *const u32
    } else {
        ptr::null()
    };

    let raw_store = checkpoint_store(context);
    let raw = LanceBatchUDF {
        mapper: Some(mapper_callback),
        mapper_ctx: (context as *const CallbackContext).cast_mut().cast(),
        output_schema: declared_schema,
        checkpoint_store: if with_checkpoint {
            &raw_store
        } else {
            ptr::null()
        },
    };
    unsafe {
        lance_dataset_add_columns_batch_udf(
            dataset,
            &raw,
            read_ptrs
                .as_ref()
                .map_or(ptr::null(), |columns| columns.as_ptr()),
            batch_size_ptr,
        )
    }
}

fn make_context(
    output_schema: Arc<Schema>,
    mode: MapperMode,
) -> (Arc<MapperState>, Arc<StoreState>, CallbackContext) {
    let events = Arc::new(Mutex::new(Vec::new()));
    let string_releases = Arc::new(AtomicUsize::new(0));
    let partial_releases = Arc::new(AtomicUsize::new(0));
    let mapper = Arc::new(MapperState {
        output_schema,
        mode,
        calls: AtomicUsize::new(0),
        events: Mutex::new(Vec::new()),
        string_releases: string_releases.clone(),
        partial_releases,
    });
    let store = Arc::new(StoreState::new(events, string_releases));
    (
        mapper.clone(),
        store.clone(),
        CallbackContext {
            mapper,
            store,
            independent_reads: None,
        },
    )
}

fn derive_map(dataset: *const LanceDataset, value_name: &str) -> BTreeMap<i32, Option<i64>> {
    let mut result = BTreeMap::new();
    for batch in scan_batches(dataset) {
        let ids = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        let values = batch
            .column_by_name(value_name)
            .unwrap()
            .as_any()
            .downcast_ref::<Int64Array>()
            .unwrap();
        for row in 0..batch.num_rows() {
            result.insert(
                ids.value(row),
                if values.is_null(row) {
                    None
                } else {
                    Some(values.value(row))
                },
            );
        }
    }
    result
}

fn import_stored(stored: StoredArrow) -> RecordBatch {
    let schema = Schema::try_from(&stored.schema).unwrap();
    let data = unsafe { from_ffi(stored.array, &stored.schema) }.unwrap();
    let row_count = data.len();
    let struct_array = StructArray::from(data);
    let (_, columns, nulls) = struct_array.into_parts();
    assert!(nulls.as_ref().is_none_or(|nulls| nulls.null_count() == 0));
    RecordBatch::try_new_with_options(
        Arc::new(schema),
        columns,
        &RecordBatchOptions::new().with_row_count(Some(row_count)),
    )
    .unwrap()
}

fn count_data_files(uri: &str) -> usize {
    std::fs::read_dir(std::path::Path::new(uri).join("data"))
        .unwrap()
        .filter_map(Result::ok)
        .filter(|entry| entry.path().extension().is_some_and(|ext| ext == "lance"))
        .count()
}

fn checkpoint_file_exists(uri: &str, path: &str) -> bool {
    let relative = path.trim_start_matches("./");
    let root = std::path::Path::new(uri);
    root.join(relative).exists()
        || root.join("data").join(relative).exists()
        || root
            .join("data")
            .join(std::path::Path::new(relative).file_name().unwrap())
            .exists()
}

#[test]
fn test_batch_udf_all_callbacks_read_independent_handles_and_existing_scanners() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = fixture_uri(&tmp, "reentrant_reads");
    create_batch_udf_dataset(&uri, true);
    let dataset = open(&uri);
    let independent = open(&uri);
    let baseline = unsafe { lance_dataset_version(independent) };
    let scanners = (0..5)
        .map(|_| unsafe { lance_scanner_new(independent, ptr::null(), ptr::null()) })
        .collect::<Vec<_>>();
    assert!(scanners.iter().all(|scanner| !scanner.is_null()));
    assert_eq!(unsafe { lance_dataset_count_rows(independent) }, 8);

    let declared = output_schema();
    let ffi_schema = schema_to_ffi(&declared);
    let (_mapper, _store, mut context) = make_context(declared, MapperMode::Constant(77));
    context.independent_reads = Some(CallbackReads {
        dataset: independent,
        scanners: Mutex::new(scanners),
        results: Mutex::new(Vec::new()),
    });
    assert_eq!(
        run_batch_udf(dataset, &ffi_schema, &context, None, Some(2), true),
        0,
        "reentrant callback UDF failed: {:?}",
        take_error()
    );
    let results = context
        .independent_reads
        .as_ref()
        .unwrap()
        .results
        .lock()
        .unwrap();
    assert_eq!(results.len(), 5, "all callback kinds must execute");
    for read in results.iter() {
        eprintln!(
            "callback {}: count={}, ids={:?}",
            read.callback, read.count, read.ids
        );
        assert_eq!(read.count, 8, "{} independent-handle read", read.callback);
        assert_eq!(
            read.ids.as_ref().unwrap(),
            &(0..8).collect::<Vec<_>>(),
            "{} pre-existing scanner read",
            read.callback
        );
    }
    assert_eq!(unsafe { lance_dataset_version(independent) }, baseline);
    assert!(
        exported_schema(independent)
            .field_with_name("derived")
            .is_err()
    );
    assert!(
        derive_map(dataset, "derived")
            .values()
            .all(|value| *value == Some(77))
    );
    unsafe {
        lance_dataset_close(independent);
        lance_dataset_close(dataset);
    }
}

#[test]
fn test_batch_udf_native_oracle_metadata_and_retained_payloads() {
    let tmp = tempfile::tempdir().unwrap();
    let native_uri = fixture_uri(&tmp, "native");
    let c_uri = fixture_uri(&tmp, "c");
    create_batch_udf_dataset(&native_uri, false);
    create_batch_udf_dataset(&c_uri, false);

    let declared = output_schema();
    let mut native = lance_c::runtime::block_on(Dataset::open(&native_uri)).unwrap();
    let native_schema = declared.clone();
    lance_c::runtime::block_on(native.add_columns(
        NewColumnTransform::BatchUDF(NativeBatchUDF {
            mapper: Box::new(move |batch| {
                Ok(RecordBatch::try_new(
                    native_schema.clone(),
                    vec![Arc::new(Int64Array::from(vec![42i64; batch.num_rows()]))],
                )?)
            }),
            output_schema: declared.clone(),
            result_checkpoint: None,
        }),
        None,
        None,
    ))
    .unwrap();

    let dataset = open(&c_uri);
    let baseline = unsafe { lance_dataset_version(dataset) };
    let fragment_ids = lance_c::runtime::block_on(Dataset::open(&c_uri))
        .unwrap()
        .get_fragments()
        .iter()
        .map(|fragment| fragment.id() as u32)
        .collect::<Vec<_>>();
    let ffi_schema = schema_to_ffi(&declared);
    let (mapper, store, context) = make_context(declared.clone(), MapperMode::Constant(42));
    assert_eq!(
        run_batch_udf(dataset, &ffi_schema, &context, None, None, true),
        0,
        "BatchUDF failed: {:?}",
        take_error()
    );
    assert_eq!(unsafe { lance_dataset_version(dataset) }, baseline + 1);

    let oracle = open(&native_uri);
    assert_eq!(exported_schema(dataset), exported_schema(oracle));
    assert_eq!(
        derive_map(dataset, "derived"),
        derive_map(oracle, "derived")
    );
    assert!(
        derive_map(dataset, "derived")
            .values()
            .all(|value| *value == Some(42))
    );
    assert_eq!(mapper.calls.load(Ordering::SeqCst), 2);
    for event in mapper.events.lock().unwrap().iter() {
        assert_eq!(
            event.input_fields,
            vec!["id".to_owned(), "label".to_owned()]
        );
        assert_eq!(event.rows, 4);
    }
    let events = store.events.lock().unwrap().clone();
    for fragment_id in fragment_ids {
        let get_fragment = events
            .iter()
            .position(|event| event == &format!("get_fragment:{fragment_id}"))
            .unwrap();
        let get_batch = events
            .iter()
            .position(|event| event == &format!("get_batch:{fragment_id}:0"))
            .unwrap();
        let mapper_index = events
            .iter()
            .enumerate()
            .skip(get_batch + 1)
            .find(|(_, event)| *event == "mapper")
            .map(|(index, _)| index)
            .unwrap();
        let insert_batch = events
            .iter()
            .position(|event| event == &format!("insert_batch:{fragment_id}:0"))
            .unwrap();
        let insert_fragment = events
            .iter()
            .position(|event| event == &format!("insert_fragment:{fragment_id}"))
            .unwrap();
        assert!(get_fragment < get_batch);
        assert!(get_batch < mapper_index);
        assert!(mapper_index < insert_batch);
        assert!(insert_batch < insert_fragment);
    }

    let inserted_fragments = store.inserted_fragments.lock().unwrap().clone();
    assert_eq!(inserted_fragments.len(), 2);
    for (id, json) in inserted_fragments {
        let fragment: Fragment = serde_json::from_str(&json).unwrap();
        assert_eq!(fragment.id, id);
        assert_eq!(fragment.physical_rows, Some(4));
    }

    let inserted_batches = std::mem::take(&mut *store.inserted_batches.lock().unwrap());
    assert_eq!(inserted_batches.len(), 2);
    unsafe { lance_dataset_close(dataset) };
    let mut retained_rows = 0;
    for stored in inserted_batches {
        let batch = import_stored(stored);
        assert_eq!(
            batch
                .schema()
                .metadata()
                .get("producer")
                .map(String::as_str),
            Some("callback-test")
        );
        assert_eq!(
            batch
                .schema()
                .field_with_name("derived")
                .unwrap()
                .metadata()
                .get("unit")
                .map(String::as_str),
            Some("native")
        );
        assert_eq!(batch.num_columns(), 1);
        assert_eq!(batch.num_rows(), 4);
        retained_rows += batch.num_rows();
    }
    assert_eq!(retained_rows, 8);
    unsafe { lance_dataset_close(oracle) };
}

#[test]
fn test_batch_udf_zero_columns_and_null_context() {
    unsafe extern "C" fn null_context_mapper(
        _callback_ctx: *mut c_void,
        input: *const FFI_ArrowArray,
        _input_schema: *const FFI_ArrowSchema,
        output: *mut FFI_ArrowArray,
        _error: *mut LanceCallbackError,
    ) -> i32 {
        let rows = unsafe { (*input).length as usize };
        let batch = RecordBatch::try_new_with_options(
            Arc::new(Schema::empty()),
            Vec::new(),
            &RecordBatchOptions::new().with_row_count(Some(rows)),
        )
        .unwrap();
        emit_batch(output, batch);
        0
    }

    let tmp = tempfile::tempdir().unwrap();
    let uri = fixture_uri(&tmp, "zero");
    create_batch_udf_dataset(&uri, false);
    let dataset = open(&uri);
    let baseline = unsafe { lance_dataset_version(dataset) };
    let rows_before = unsafe { lance_dataset_count_rows(dataset) };
    let ffi_schema = schema_to_ffi(&empty_output_schema());
    let raw = LanceBatchUDF {
        mapper: Some(null_context_mapper),
        mapper_ctx: ptr::null_mut(),
        output_schema: &ffi_schema,
        checkpoint_store: ptr::null(),
    };
    assert_eq!(
        unsafe { lance_dataset_add_columns_batch_udf(dataset, &raw, ptr::null(), ptr::null(),) },
        -1,
    );
    let (code, message) = take_error();
    assert_eq!(code, LanceErrorCode::InvalidArgument);
    assert!(
        message.contains("does not contain any fields"),
        "native must reject the fieldless data file, got: {message}"
    );
    assert_eq!(unsafe { lance_dataset_count_rows(dataset) }, rows_before);
    assert_eq!(unsafe { lance_dataset_version(dataset) }, baseline);
    unsafe { lance_dataset_close(dataset) };

    let tmp = tempfile::tempdir().unwrap();
    let uri = fixture_uri(&tmp, "zero-checkpoint");
    create_batch_udf_dataset(&uri, false);
    let declared = empty_output_schema();
    let ffi_schema = schema_to_ffi(&declared);
    let dataset = open(&uri);
    let baseline = unsafe { lance_dataset_version(dataset) };
    let (mapper, store, context) = make_context(declared, MapperMode::ZeroColumns);
    assert_eq!(
        run_batch_udf(dataset, &ffi_schema, &context, None, None, true),
        -1,
    );
    assert_eq!(take_error().0, LanceErrorCode::InvalidArgument);
    assert_eq!(mapper.calls.load(Ordering::SeqCst), 2);
    assert_eq!(unsafe { lance_dataset_version(dataset) }, baseline);
    let stored = std::mem::take(&mut *store.inserted_batches.lock().unwrap());
    assert_eq!(stored.len(), 2);
    unsafe { lance_dataset_close(dataset) };
    for stored in stored {
        let batch = import_stored(stored);
        assert_eq!(batch.num_columns(), 0);
        assert_eq!(batch.num_rows(), 4);
    }
}

#[test]
fn test_batch_udf_read_columns_and_batch_options() {
    struct ReadColumnsCase<'a> {
        read_columns: Option<Vec<&'a str>>,
        expected_inputs: Vec<String>,
        batch_size: Option<u32>,
        expected_calls: usize,
    }

    let cases = vec![
        ReadColumnsCase {
            read_columns: None,
            expected_inputs: vec!["id".into(), "label".into()],
            batch_size: None,
            expected_calls: 2,
        },
        ReadColumnsCase {
            read_columns: Some(Vec::new()),
            expected_inputs: vec!["_rowaddr".into()],
            batch_size: None,
            expected_calls: 2,
        },
        ReadColumnsCase {
            read_columns: Some(vec!["id"]),
            expected_inputs: vec!["id".into()],
            batch_size: None,
            expected_calls: 2,
        },
        ReadColumnsCase {
            read_columns: Some(vec!["label", "id"]),
            expected_inputs: vec!["label".into(), "id".into()],
            batch_size: None,
            expected_calls: 2,
        },
        ReadColumnsCase {
            read_columns: Some(vec!["id"]),
            expected_inputs: vec!["id".into()],
            batch_size: Some(1),
            expected_calls: 8,
        },
    ];

    for case in cases {
        let ReadColumnsCase {
            read_columns,
            expected_inputs,
            batch_size,
            expected_calls,
        } = case;
        let tmp = tempfile::tempdir().unwrap();
        let uri = fixture_uri(&tmp, "read");
        create_batch_udf_dataset(&uri, false);
        let declared = output_schema();
        let ffi_schema = schema_to_ffi(&declared);
        let dataset = open(&uri);
        let (mapper, _store, context) = make_context(declared, MapperMode::Constant(7));
        let reads = read_columns.as_deref();
        assert_eq!(
            run_batch_udf(dataset, &ffi_schema, &context, reads, batch_size, false),
            0,
            "read_columns={read_columns:?} batch_size={batch_size:?}: {:?}",
            take_error()
        );
        let events = mapper.events.lock().unwrap().clone();
        assert_eq!(events.len(), expected_calls);
        for event in &events {
            assert_eq!(event.input_fields, expected_inputs);
            if batch_size == Some(1) {
                assert_eq!(event.rows, 1);
            }
        }
        assert_eq!(derive_map(dataset, "derived").len(), 8);
        unsafe { lance_dataset_close(dataset) };
    }

    for read_columns in [vec!["missing"], vec!["_rowid"]] {
        let tmp = tempfile::tempdir().unwrap();
        let uri = fixture_uri(&tmp, "unknown");
        create_batch_udf_dataset(&uri, true);
        let declared = output_schema();
        let ffi_schema = schema_to_ffi(&declared);
        let dataset = open(&uri);
        let (mapper, _store, context) = make_context(declared, MapperMode::Constant(1));
        let rc = run_batch_udf(
            dataset,
            &ffi_schema,
            &context,
            Some(&read_columns),
            None,
            false,
        );
        if read_columns == vec!["missing"] {
            assert_eq!(rc, -1);
            assert_eq!(take_error().0, LanceErrorCode::Internal);
            assert_eq!(mapper.calls.load(Ordering::SeqCst), 0);
        } else {
            assert_eq!(rc, 0, "_rowid should be accepted: {:?}", take_error());
            assert_eq!(
                mapper.events.lock().unwrap()[0].input_fields,
                vec!["_rowid"]
            );
        }
        unsafe { lance_dataset_close(dataset) };
    }

    let tmp = tempfile::tempdir().unwrap();
    let uri = fixture_uri(&tmp, "zero-batch");
    create_batch_udf_dataset(&uri, false);
    let declared = output_schema();
    let ffi_schema = schema_to_ffi(&declared);
    let dataset = open(&uri);
    let baseline = unsafe { lance_dataset_version(dataset) };
    let (mapper, _store, context) = make_context(declared, MapperMode::Constant(1));
    assert_eq!(
        run_batch_udf(dataset, &ffi_schema, &context, None, Some(0), false),
        -1
    );
    assert_eq!(take_error().0, LanceErrorCode::InvalidArgument);
    assert_eq!(mapper.calls.load(Ordering::SeqCst), 0);
    assert_eq!(unsafe { lance_dataset_version(dataset) }, baseline);
    unsafe { lance_dataset_close(dataset) };
}

#[test]
fn test_batch_udf_validation_and_mapper_error_ownership() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = fixture_uri(&tmp, "validation");
    create_batch_udf_dataset(&uri, false);
    let dataset = open(&uri);
    let baseline = unsafe { lance_dataset_version(dataset) };
    let declared = output_schema();
    let ffi_schema = schema_to_ffi(&declared);
    let (_mapper, _store, context) = make_context(declared, MapperMode::Constant(1));
    let raw_store = checkpoint_store(&context);
    let raw = LanceBatchUDF {
        mapper: Some(mapper_callback),
        mapper_ctx: (&context as *const CallbackContext).cast_mut().cast(),
        output_schema: &ffi_schema,
        checkpoint_store: ptr::null(),
    };
    assert_eq!(
        unsafe {
            lance_dataset_add_columns_batch_udf(ptr::null_mut(), &raw, ptr::null(), ptr::null())
        },
        -1
    );
    assert_eq!(take_error().0, LanceErrorCode::InvalidArgument);
    assert_eq!(
        unsafe {
            lance_dataset_add_columns_batch_udf(dataset, ptr::null(), ptr::null(), ptr::null())
        },
        -1
    );
    assert_eq!(take_error().0, LanceErrorCode::InvalidArgument);

    let mut missing_mapper = LanceBatchUDF {
        mapper: None,
        mapper_ctx: ptr::null_mut(),
        output_schema: &ffi_schema,
        checkpoint_store: ptr::null(),
    };
    assert_eq!(
        unsafe {
            lance_dataset_add_columns_batch_udf(dataset, &missing_mapper, ptr::null(), ptr::null())
        },
        -1
    );
    assert_eq!(take_error().0, LanceErrorCode::InvalidArgument);

    missing_mapper.mapper = Some(mapper_callback);
    missing_mapper.output_schema = ptr::null();
    assert_eq!(
        unsafe {
            lance_dataset_add_columns_batch_udf(dataset, &missing_mapper, ptr::null(), ptr::null())
        },
        -1
    );
    assert_eq!(take_error().0, LanceErrorCode::InvalidArgument);

    let empty_schema = FFI_ArrowSchema::empty();
    missing_mapper.output_schema = &empty_schema;
    assert_eq!(
        unsafe {
            lance_dataset_add_columns_batch_udf(dataset, &missing_mapper, ptr::null(), ptr::null())
        },
        -1
    );
    assert_eq!(take_error().0, LanceErrorCode::InvalidArgument);

    let mut incomplete_store = raw_store;
    incomplete_store.insert_fragment = None;
    let missing_store = LanceBatchUDF {
        mapper: Some(mapper_callback),
        mapper_ctx: (&context as *const CallbackContext).cast_mut().cast(),
        output_schema: &ffi_schema,
        checkpoint_store: &incomplete_store,
    };
    assert_eq!(
        unsafe {
            lance_dataset_add_columns_batch_udf(dataset, &missing_store, ptr::null(), ptr::null())
        },
        -1
    );
    assert_eq!(take_error().0, LanceErrorCode::InvalidArgument);

    let (error_mapper, _store, error_context) = make_context(
        output_schema(),
        MapperMode::Error {
            code: LanceErrorCode::InvalidArgument as i32,
            message: "mapper rejected input".to_owned(),
            partial_output: true,
        },
    );
    let release_before = error_mapper.string_releases.load(Ordering::SeqCst);
    let partial_before = error_mapper.partial_releases.load(Ordering::SeqCst);
    let error_schema = schema_to_ffi(&output_schema());
    assert_eq!(
        run_batch_udf(dataset, &error_schema, &error_context, None, None, false,),
        -1
    );
    let (code, message) = take_error();
    assert_eq!(code, LanceErrorCode::InvalidArgument);
    assert!(message.contains("mapper rejected input"));
    assert_eq!(error_mapper.calls.load(Ordering::SeqCst), 1);
    assert_eq!(
        error_mapper.string_releases.load(Ordering::SeqCst),
        release_before + 1,
        "callback-owned message must be released exactly once"
    );
    assert_eq!(
        error_mapper.partial_releases.load(Ordering::SeqCst),
        partial_before + 1,
        "initialized output on failure must be released exactly once"
    );
    assert_eq!(unsafe { lance_dataset_version(dataset) }, baseline);

    for mode in [MapperMode::NoOutput, MapperMode::RowMismatch(9)] {
        let (mapper, _store, context) = make_context(output_schema(), mode);
        let schema = schema_to_ffi(&output_schema());
        assert_eq!(
            run_batch_udf(dataset, &schema, &context, None, None, false),
            -1
        );
        assert_eq!(take_error().0, LanceErrorCode::InvalidArgument);
        assert_eq!(mapper.calls.load(Ordering::SeqCst), 1);
        assert_eq!(unsafe { lance_dataset_version(dataset) }, baseline);
    }
    unsafe { lance_dataset_close(dataset) };
}

#[test]
fn test_batch_udf_checkpoint_hits_skip_mapper_and_batches() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = fixture_uri(&tmp, "hits");
    create_batch_udf_dataset(&uri, false);

    let declared = output_schema();
    let ffi_schema = schema_to_ffi(&declared);
    let dataset = open(&uri);
    let original_version = unsafe { lance_dataset_version(dataset) };
    let (mapper, store, context) = make_context(declared.clone(), MapperMode::Constant(5));

    // A failure after the first fragment checkpoint leaves the dataset at its
    // original version and keeps the fragment JSON native wrote, which is the
    // resume state the checkpoint protocol is built for.
    *store.failure.lock().unwrap() = Some(Failure {
        method: StoreMethod::InsertFragment,
        call_index: 1,
        code: LanceErrorCode::IoError as i32,
        message: "interrupted after the first fragment checkpoint".into(),
    });
    assert_eq!(
        run_batch_udf(dataset, &ffi_schema, &context, None, None, true),
        -1
    );
    let (code, message) = take_error();
    assert_eq!(code, LanceErrorCode::IoError);
    assert!(
        message.contains("interrupted after the first fragment checkpoint"),
        "unexpected seeding failure: {message}"
    );
    assert_eq!(unsafe { lance_dataset_version(dataset) }, original_version);
    // Both fragments reach the mapper before the second fragment checkpoint is
    // rejected, so the seeding run only persists the first fragment's result.
    assert_eq!(mapper.calls.load(Ordering::SeqCst), 2);

    // A durable store returns the native fragment JSON verbatim, marker included.
    let checkpointed: Vec<(u64, String)> = store.inserted_fragments.lock().unwrap().clone();
    assert_eq!(checkpointed.len(), 1);
    for (fragment_id, json) in checkpointed {
        store
            .fragment_hits
            .lock()
            .unwrap()
            .insert(fragment_id as u32, json);
    }
    unsafe { lance_dataset_close(dataset) };

    // Resume the same add_columns request against the original snapshot.
    let replay = open_version(&uri, original_version);
    reset_store(&store);
    *store.failure.lock().unwrap() = None;
    let replay_mapper = Arc::new(MapperState {
        output_schema: declared.clone(),
        mode: MapperMode::Constant(7),
        calls: AtomicUsize::new(0),
        events: Mutex::new(Vec::new()),
        string_releases: Arc::new(AtomicUsize::new(0)),
        partial_releases: Arc::new(AtomicUsize::new(0)),
    });
    let replay_context = CallbackContext {
        mapper: replay_mapper.clone(),
        store: store.clone(),
        independent_reads: None,
    };
    assert_eq!(
        run_batch_udf(replay, &ffi_schema, &replay_context, None, None, true),
        0,
        "checkpoint hit call failed: {:?}",
        take_error()
    );
    assert_eq!(
        replay_mapper.calls.load(Ordering::SeqCst),
        1,
        "the fragment checkpoint hit must skip the mapper for its fragment"
    );
    assert_eq!(
        store.inserted_fragments.lock().unwrap().len(),
        1,
        "a fragment hit skips insert_fragment; only the replayed fragment is stored"
    );

    let events = store.events.lock().unwrap().clone();
    assert!(
        events
            .iter()
            .any(|event| event.starts_with("get_fragment:"))
    );
    assert_eq!(events.iter().filter(|event| *event == "mapper").count(), 1);

    let values = derive_map(replay, "derived");
    for (id, value) in values {
        if id < 4 {
            assert_eq!(
                value,
                Some(5),
                "the fragment hit must reuse the checkpointed column value"
            );
        } else {
            assert_eq!(value, Some(7), "the replayed fragment must run the mapper");
        }
    }
    unsafe { lance_dataset_close(replay) };
}

/// Native Beta20 binds every checkpoint entry to a checkpoint-context marker:
/// fragment JSON and cached batch schemas must carry the marker native wrote.
/// The public C checkpoint callbacks return an owned ArrowArray for a batch
/// hit, so the adapter rebuilds that batch from the declared output schema and
/// cannot carry the marker back. A marker-less entry is therefore rejected by
/// native validation instead of being treated as a checkpoint hit; fragment
/// checkpoints still round-trip because their JSON carries the marker.
#[test]
fn test_batch_udf_checkpoint_without_native_context_is_rejected() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = fixture_uri(&tmp, "context-rejection");
    create_batch_udf_dataset(&uri, false);
    let native = lance_c::runtime::block_on(Dataset::open(&uri)).unwrap();
    let fragments = native.get_fragments();
    assert_eq!(fragments.len(), 2);
    let fragment_a = fragments[0].metadata().clone();
    let fragment_b_id = fragments[1].id() as u32;

    let declared = output_schema();
    let ffi_schema = schema_to_ffi(&declared);
    let dataset = open(&uri);
    let version = unsafe { lance_dataset_version(dataset) };
    let (mapper, store, context) = make_context(declared.clone(), MapperMode::Constant(5));
    store.fragment_hits.lock().unwrap().insert(
        fragment_a.id as u32,
        serde_json::to_string(&fragment_a).unwrap(),
    );
    assert_eq!(
        run_batch_udf(dataset, &ffi_schema, &context, None, None, true),
        -1
    );
    let (code, message) = take_error();
    assert_eq!(code, LanceErrorCode::InvalidArgument);
    assert!(
        message.contains("incompatible with this add_columns invocation"),
        "unexpected marker rejection message: {message}"
    );
    assert_eq!(mapper.calls.load(Ordering::SeqCst), 0);

    store.fragment_hits.lock().unwrap().clear();
    // A batch entry restored through the C callbacks carries no schema
    // metadata, so native rejects it instead of using it as a hit even when
    // every fragment has an entry and the mapper would never run.
    for fragment_id in [fragment_a.id as u32, fragment_b_id] {
        store.batch_hits.lock().unwrap().insert(
            (fragment_id, 0),
            RecordBatch::try_new(
                declared.clone(),
                vec![Arc::new(Int64Array::from(vec![9; 4]))],
            )
            .unwrap(),
        );
    }
    assert_eq!(
        run_batch_udf(dataset, &ffi_schema, &context, None, None, true),
        -1
    );
    let (code, message) = take_error();
    assert_eq!(code, LanceErrorCode::InvalidArgument);
    assert!(
        message.contains("incompatible with this add_columns invocation"),
        "unexpected batch marker rejection message: {message}"
    );
    assert_eq!(mapper.calls.load(Ordering::SeqCst), 0);
    assert_eq!(
        unsafe { lance_dataset_version(dataset) },
        version,
        "a rejected checkpoint must not commit"
    );
    unsafe { lance_dataset_close(dataset) };
}

#[test]
fn test_batch_udf_checkpoint_failures_cleanup_and_preserve() {
    // get_fragment failure happens before any mapper or batch work.
    {
        let tmp = tempfile::tempdir().unwrap();
        let uri = fixture_uri(&tmp, "get-fragment");
        create_batch_udf_dataset(&uri, false);
        let declared = output_schema();
        let ffi_schema = schema_to_ffi(&declared);
        let dataset = open(&uri);
        let baseline = unsafe { lance_dataset_version(dataset) };
        let (mapper, store, context) = make_context(declared, MapperMode::Constant(1));
        *store.failure.lock().unwrap() = Some(Failure {
            method: StoreMethod::GetFragment,
            call_index: 0,
            code: LanceErrorCode::IoError as i32,
            message: "fragment cache read failed".into(),
        });
        assert_eq!(
            run_batch_udf(dataset, &ffi_schema, &context, None, None, true),
            -1
        );
        let (code, message) = take_error();
        assert_eq!(code, LanceErrorCode::IoError);
        assert!(message.contains("fragment cache read failed"));
        assert_eq!(mapper.calls.load(Ordering::SeqCst), 0);
        assert_eq!(unsafe { lance_dataset_version(dataset) }, baseline);
        unsafe { lance_dataset_close(dataset) };
    }

    // get_batch failure happens before the mapper for the corresponding batch.
    {
        let tmp = tempfile::tempdir().unwrap();
        let uri = fixture_uri(&tmp, "get-batch");
        create_batch_udf_dataset(&uri, false);
        let declared = output_schema();
        let ffi_schema = schema_to_ffi(&declared);
        let dataset = open(&uri);
        let baseline = unsafe { lance_dataset_version(dataset) };
        let (mapper, store, context) = make_context(declared, MapperMode::Constant(1));
        *store.failure.lock().unwrap() = Some(Failure {
            method: StoreMethod::GetBatch,
            call_index: 0,
            code: LanceErrorCode::NotFound as i32,
            message: "batch cache read failed".into(),
        });
        assert_eq!(
            run_batch_udf(dataset, &ffi_schema, &context, None, None, true),
            -1
        );
        assert_eq!(take_error().0, LanceErrorCode::NotFound);
        assert_eq!(mapper.calls.load(Ordering::SeqCst), 0);
        assert_eq!(unsafe { lance_dataset_version(dataset) }, baseline);
        unsafe { lance_dataset_close(dataset) };
    }

    // insert_batch failure occurs after mapping and leaves no new data files.
    {
        let tmp = tempfile::tempdir().unwrap();
        let uri = fixture_uri(&tmp, "insert-batch");
        create_batch_udf_dataset(&uri, false);
        let files_before = count_data_files(&uri);
        let declared = output_schema();
        let ffi_schema = schema_to_ffi(&declared);
        let dataset = open(&uri);
        let baseline = unsafe { lance_dataset_version(dataset) };
        let (mapper, store, context) = make_context(declared, MapperMode::Constant(1));
        *store.failure.lock().unwrap() = Some(Failure {
            method: StoreMethod::InsertBatch,
            call_index: 0,
            code: LanceErrorCode::NotSupported as i32,
            message: "batch cache write failed".into(),
        });
        assert_eq!(
            run_batch_udf(dataset, &ffi_schema, &context, None, None, true),
            -1
        );
        assert_eq!(take_error().0, LanceErrorCode::NotSupported);
        assert_eq!(mapper.calls.load(Ordering::SeqCst), 1);
        assert_eq!(unsafe { lance_dataset_version(dataset) }, baseline);
        assert_eq!(count_data_files(&uri), files_before);
        unsafe { lance_dataset_close(dataset) };
    }

    // insert_fragment failure on the second fragment preserves the first
    // checkpoint-owned fragment files and cleans unfinished fragment B files.
    {
        let tmp = tempfile::tempdir().unwrap();
        let uri = fixture_uri(&tmp, "insert-fragment");
        create_batch_udf_dataset(&uri, false);
        let files_before = count_data_files(&uri);
        let declared = output_schema();
        let ffi_schema = schema_to_ffi(&declared);
        let dataset = open(&uri);
        let baseline = unsafe { lance_dataset_version(dataset) };
        let (mapper, store, context) = make_context(declared, MapperMode::Constant(3));
        *store.failure.lock().unwrap() = Some(Failure {
            method: StoreMethod::InsertFragment,
            call_index: 1,
            code: LanceErrorCode::Internal as i32,
            message: "fragment cache write failed".into(),
        });
        assert_eq!(
            run_batch_udf(dataset, &ffi_schema, &context, None, None, true),
            -1
        );
        assert_eq!(take_error().0, LanceErrorCode::Internal);
        assert_eq!(mapper.calls.load(Ordering::SeqCst), 2);
        assert_eq!(unsafe { lance_dataset_version(dataset) }, baseline);
        assert!(
            count_data_files(&uri) > files_before,
            "the first checkpoint-owned fragment's new files must survive cleanup"
        );
        let inserted = store.inserted_fragments.lock().unwrap().clone();
        assert_eq!(inserted.len(), 1);
        let fragment: Fragment = serde_json::from_str(&inserted[0].1).unwrap();
        for file in fragment.files {
            assert!(
                checkpoint_file_exists(&uri, &file.path),
                "checkpoint-owned file {} must be preserved",
                file.path
            );
        }
        unsafe { lance_dataset_close(dataset) };
    }

    // Invalid cached Fragment JSON is rejected without advancing the dataset.
    {
        let tmp = tempfile::tempdir().unwrap();
        let uri = fixture_uri(&tmp, "invalid-json");
        create_batch_udf_dataset(&uri, false);
        let native = lance_c::runtime::block_on(Dataset::open(&uri)).unwrap();
        let fragment_id = native.get_fragments()[0].id() as u32;
        let declared = output_schema();
        let ffi_schema = schema_to_ffi(&declared);
        let dataset = open(&uri);
        let baseline = unsafe { lance_dataset_version(dataset) };
        let (mapper, store, context) = make_context(declared, MapperMode::Constant(1));
        store
            .fragment_hits
            .lock()
            .unwrap()
            .insert(fragment_id, "{not-json".to_owned());
        assert_eq!(
            run_batch_udf(dataset, &ffi_schema, &context, None, None, true),
            -1
        );
        assert_eq!(take_error().0, LanceErrorCode::InvalidArgument);
        assert_eq!(mapper.calls.load(Ordering::SeqCst), 0);
        assert_eq!(unsafe { lance_dataset_version(dataset) }, baseline);
        unsafe { lance_dataset_close(dataset) };
    }
}

#[test]
fn test_batch_udf_snapshots_reopen_and_commit_conflict() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = fixture_uri(&tmp, "snapshots");
    create_batch_udf_dataset(&uri, false);
    let dataset = open(&uri);
    let old_handle = open(&uri);
    let baseline = unsafe { lance_dataset_version(dataset) };
    let old_scanner = unsafe { lance_scanner_new(dataset, ptr::null(), ptr::null()) };
    assert!(!old_scanner.is_null());
    let mut fragment_ids = vec![0u64; unsafe { lance_dataset_fragment_count(dataset) } as usize];
    assert_eq!(
        unsafe { lance_dataset_fragment_ids(dataset, fragment_ids.as_mut_ptr()) },
        0
    );
    let fragment_id = fragment_ids[0];
    let old_fragment = unsafe { lance_dataset_open_fragment(dataset, fragment_id) };
    assert!(!old_fragment.is_null());

    let declared = output_schema();
    let ffi_schema = schema_to_ffi(&declared);
    let (_mapper, _store, context) = make_context(declared, MapperMode::Constant(77));
    assert_eq!(
        run_batch_udf(dataset, &ffi_schema, &context, None, None, false),
        0,
        "snapshot mutation failed: {:?}",
        take_error()
    );
    assert!(unsafe { lance_dataset_version(dataset) } > baseline);
    assert_eq!(unsafe { lance_dataset_version(old_handle) }, baseline);
    assert!(
        exported_schema(old_handle)
            .field_with_name("derived")
            .is_err()
    );

    let mut old_stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(old_scanner, &mut old_stream) },
        0
    );
    let old_reader = unsafe { ArrowArrayStreamReader::from_raw(&mut old_stream) }.unwrap();
    assert!(old_reader.schema().field_with_name("derived").is_err());
    assert_eq!(
        old_reader
            .map(Result::unwrap)
            .map(|batch| batch.num_rows())
            .sum::<usize>(),
        8
    );
    let mut fragment_schema = FFI_ArrowSchema::empty();
    assert_eq!(
        unsafe { lance_fragment_schema(old_fragment, &mut fragment_schema) },
        0
    );
    assert!(
        Schema::try_from(&fragment_schema)
            .unwrap()
            .field_with_name("derived")
            .is_err()
    );
    let reopened = open(&uri);
    assert!(exported_schema(reopened).field_with_name("derived").is_ok());
    assert_eq!(derive_map(reopened, "derived").len(), 8);

    let stale = open(&uri);
    let writer = open(&uri);
    let conflict_baseline = unsafe { lance_dataset_version(stale) };
    let name = c_string("writer_column");
    let expression = c_string("id * 10");
    let column = LanceSqlColumn {
        name: name.as_ptr(),
        expression: expression.as_ptr(),
    };
    assert_eq!(
        unsafe { lance_dataset_add_columns_sql(writer, &column, 1, 0) },
        0
    );
    assert!(unsafe { lance_dataset_version(writer) } > conflict_baseline);
    let conflict_schema = Arc::new(Schema::new(vec![Field::new(
        "conflict_column",
        DataType::Int64,
        true,
    )]));
    let conflict_ffi = schema_to_ffi(&conflict_schema);
    let (_mapper, _store, context) = make_context(conflict_schema, MapperMode::Constant(1));
    assert_eq!(
        run_batch_udf(stale, &conflict_ffi, &context, None, None, false),
        -1
    );
    assert_eq!(take_error().0, LanceErrorCode::CommitConflict);
    assert_eq!(unsafe { lance_dataset_version(stale) }, conflict_baseline);
    let latest = open(&uri);
    let latest_schema = exported_schema(latest);
    assert!(latest_schema.field_with_name("writer_column").is_ok());
    assert!(latest_schema.field_with_name("conflict_column").is_err());

    unsafe {
        lance_dataset_close(latest);
        lance_dataset_close(writer);
        lance_dataset_close(stale);
        lance_dataset_close(reopened);
        lance_fragment_close(old_fragment);
        lance_scanner_close(old_scanner);
        lance_dataset_close(old_handle);
        lance_dataset_close(dataset);
    }
}
