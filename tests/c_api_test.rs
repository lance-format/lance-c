// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Integration tests for the Lance C API.
//!
//! These tests call the `extern "C"` functions directly from Rust,
//! validating the C API contract without needing a C compiler.

use std::ffi::{CString, c_char};
use std::ptr;
use std::sync::Arc;

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi::from_ffi;
use arrow::ffi_stream::ArrowArrayStreamReader;
use arrow::ffi_stream::FFI_ArrowArrayStream;
use arrow::record_batch::RecordBatchReader;
use arrow_array::{Array, Float32Array, Int32Array, RecordBatch, StringArray};
use arrow_schema::{DataType, Field, Schema};
use lance::Dataset;
use lance_c::*;

/// Helper: create a test dataset in a temp directory and return its path.
fn create_test_dataset() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("test_ds").to_str().unwrap().to_string();

    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("name", DataType::Utf8, true),
    ]));

    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from(vec![1, 2, 3, 4, 5])),
            Arc::new(StringArray::from(vec![
                "alice", "bob", "carol", "dave", "eve",
            ])),
        ],
    )
    .unwrap();

    // Use lance-c's internal runtime to write the dataset.
    lance_c::runtime::block_on(async {
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
            &uri,
            None,
        )
        .await
        .unwrap();
    });

    (tmp, uri)
}

/// Helper: create a larger dataset with multiple columns and many rows.
fn create_large_dataset(num_rows: i32) -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("large_ds").to_str().unwrap().to_string();

    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("value", DataType::Float32, true),
        Field::new("label", DataType::Utf8, true),
    ]));

    let ids: Vec<i32> = (0..num_rows).collect();
    let values: Vec<f32> = (0..num_rows).map(|i| i as f32 * 0.5).collect();
    let labels: Vec<String> = (0..num_rows).map(|i| format!("row_{i}")).collect();
    let label_refs: Vec<&str> = labels.iter().map(|s| s.as_str()).collect();

    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from(ids)),
            Arc::new(Float32Array::from(values)),
            Arc::new(StringArray::from(label_refs)),
        ],
    )
    .unwrap();

    lance_c::runtime::block_on(async {
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
            &uri,
            None,
        )
        .await
        .unwrap();
    });

    (tmp, uri)
}

fn c_str(s: &str) -> CString {
    CString::new(s).unwrap()
}

/// Helper: build a tiny dataset whose `value` column is nullable AND contains
/// at least one NULL. Used by tests that need to exercise upstream's
/// nullability-tightening pre-scan failure path.
fn create_dataset_with_nulls() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("with_nulls").to_str().unwrap().to_string();
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("value", DataType::Float32, true),
    ]));
    let ids = Int32Array::from(vec![1, 2, 3]);
    let values = Float32Array::from(vec![Some(1.0), None, Some(3.0)]);
    let batch =
        RecordBatch::try_new(schema.clone(), vec![Arc::new(ids), Arc::new(values)]).unwrap();
    lance_c::runtime::block_on(async {
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
            &uri,
            None,
        )
        .await
        .unwrap();
    });
    (tmp, uri)
}

/// Helper: scan to ArrowArrayStream and collect all rows.
fn scan_all_rows(ds: *const LanceDataset) -> Vec<RecordBatch> {
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    assert!(!scanner.is_null());
    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe { lance_scanner_to_arrow_stream(scanner, &mut ffi_stream) };
    assert_eq!(rc, 0);
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    let batches: Vec<RecordBatch> = reader.map(|r| r.unwrap()).collect();
    unsafe { lance_scanner_close(scanner) };
    batches
}

// ---------------------------------------------------------------------------
// Dataset tests
// ---------------------------------------------------------------------------

#[test]
fn test_open_close() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);

    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null(), "dataset open should succeed");
    assert_eq!(lance_last_error_code(), LanceErrorCode::Ok);

    unsafe { lance_dataset_close(ds) };

    // Closing NULL is safe.
    unsafe { lance_dataset_close(ptr::null_mut()) };
}

#[test]
fn test_open_nonexistent() {
    let c_uri = c_str("memory://nonexistent_dataset_xyz");
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(
        ds.is_null(),
        "opening nonexistent dataset should return NULL"
    );
    assert_ne!(lance_last_error_code(), LanceErrorCode::Ok);

    let msg = lance_last_error_message();
    assert!(!msg.is_null());
    unsafe { lance_free_string(msg) };
}

#[test]
fn test_version() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let version = unsafe { lance_dataset_version(ds) };
    assert!(version >= 1, "version should be >= 1, got {version}");

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_count_rows() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let count = unsafe { lance_dataset_count_rows(ds) };
    assert_eq!(count, 5);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_schema_export() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let mut ffi_schema = FFI_ArrowSchema::empty();
    let rc = unsafe { lance_dataset_schema(ds, &mut ffi_schema) };
    assert_eq!(rc, 0);

    // Import the schema back and verify fields.
    let schema = Schema::try_from(&ffi_schema).unwrap();
    assert_eq!(schema.fields().len(), 2);
    assert_eq!(schema.field(0).name(), "id");
    assert_eq!(schema.field(1).name(), "name");

    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Scanner tests
// ---------------------------------------------------------------------------

#[test]
fn test_scanner_full_scan() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // Create scanner (all columns, no filter).
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    assert!(!scanner.is_null());

    // Iterate via lance_scanner_next.
    let mut total_rows = 0u64;
    loop {
        let mut batch: *mut LanceBatch = ptr::null_mut();
        let rc = unsafe { lance_scanner_next(scanner, &mut batch) };
        match rc {
            0 => {
                assert!(!batch.is_null());
                // Export to Arrow and count rows.
                let mut ffi_array = arrow::ffi::FFI_ArrowArray::empty();
                let mut ffi_schema = FFI_ArrowSchema::empty();
                let rc2 = unsafe { lance_batch_to_arrow(batch, &mut ffi_array, &mut ffi_schema) };
                assert_eq!(rc2, 0);
                let data = unsafe { from_ffi(ffi_array, &ffi_schema) }.unwrap();
                total_rows += data.len() as u64;
                unsafe { lance_batch_free(batch) };
            }
            1 => break, // end of stream
            _ => panic!("scanner_next returned error: {rc}"),
        }
    }
    assert_eq!(total_rows, 5);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_to_arrow_stream() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    assert!(!scanner.is_null());

    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe { lance_scanner_to_arrow_stream(scanner, &mut ffi_stream) };
    assert_eq!(rc, 0);

    // Read via Arrow's standard stream reader.
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    let batches: Vec<RecordBatch> = reader.map(|r| r.unwrap()).collect();
    let total_rows: usize = batches.iter().map(|b| b.num_rows()).sum();
    assert_eq!(total_rows, 5);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_with_filter() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let filter = c_str("id > 3");
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), filter.as_ptr()) };
    assert!(!scanner.is_null());

    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe { lance_scanner_to_arrow_stream(scanner, &mut ffi_stream) };
    assert_eq!(rc, 0);

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    let total_rows: usize = reader.map(|r| r.unwrap().num_rows()).sum();
    assert_eq!(total_rows, 2); // id=4 and id=5

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_with_projection() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // Project only "name" column.
    let col = c_str("name");
    let columns: [*const i8; 2] = [col.as_ptr(), ptr::null()];
    let scanner = unsafe { lance_scanner_new(ds, columns.as_ptr(), ptr::null()) };
    assert!(!scanner.is_null());

    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe { lance_scanner_to_arrow_stream(scanner, &mut ffi_stream) };
    assert_eq!(rc, 0);

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    let schema = reader.schema();
    assert_eq!(schema.fields().len(), 1);
    assert_eq!(schema.field(0).name(), "name");

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_with_limit_offset() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    assert!(!scanner.is_null());
    unsafe {
        lance_scanner_set_limit(scanner, 2);
        lance_scanner_set_offset(scanner, 1);
    };

    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe { lance_scanner_to_arrow_stream(scanner, &mut ffi_stream) };
    assert_eq!(rc, 0);

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    let total_rows: usize = reader.map(|r| r.unwrap().num_rows()).sum();
    assert_eq!(total_rows, 2);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Take test
// ---------------------------------------------------------------------------

#[test]
fn test_dataset_take() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let indices: [u64; 3] = [0, 2, 4];
    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe { lance_dataset_take(ds, indices.as_ptr(), 3, ptr::null(), &mut ffi_stream) };
    assert_eq!(rc, 0);

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    let batches: Vec<RecordBatch> = reader.map(|r| r.unwrap()).collect();
    let total_rows: usize = batches.iter().map(|b| b.num_rows()).sum();
    assert_eq!(total_rows, 3);

    // Verify the taken IDs.
    let id_col = batches[0]
        .column_by_name("id")
        .unwrap()
        .as_any()
        .downcast_ref::<Int32Array>()
        .unwrap();
    assert_eq!(id_col.values(), &[1, 3, 5]);

    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Error handling tests
// ---------------------------------------------------------------------------

#[test]
fn test_null_inputs() {
    // NULL dataset in version query.
    let v = unsafe { lance_dataset_version(ptr::null()) };
    assert_eq!(v, 0);
    assert_ne!(lance_last_error_code(), LanceErrorCode::Ok);

    // NULL dataset in scanner creation.
    let scanner = unsafe { lance_scanner_new(ptr::null(), ptr::null(), ptr::null()) };
    assert!(scanner.is_null());
    assert_ne!(lance_last_error_code(), LanceErrorCode::Ok);
}

// ---------------------------------------------------------------------------
// Async scan test
// ---------------------------------------------------------------------------

#[test]
fn test_scanner_scan_async() {
    use std::sync::{Condvar, Mutex};

    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    assert!(!scanner.is_null());

    // Synchronization primitive for the async callback.
    struct CallbackResult {
        status: i32,
        stream_ptr: *mut std::ffi::c_void,
    }
    unsafe impl Send for CallbackResult {}

    let pair = Arc::new((Mutex::new(None::<CallbackResult>), Condvar::new()));
    let pair_clone = pair.clone();

    unsafe extern "C" fn on_complete(
        ctx: *mut std::ffi::c_void,
        status: i32,
        result: *mut std::ffi::c_void,
    ) {
        let pair = unsafe { &*(ctx as *const (Mutex<Option<CallbackResult>>, Condvar)) };
        let mut guard = pair.0.lock().unwrap();
        *guard = Some(CallbackResult {
            status,
            stream_ptr: result,
        });
        pair.1.notify_one();
    }

    unsafe {
        lance_scanner_scan_async(
            scanner,
            on_complete,
            Arc::as_ptr(&pair_clone) as *mut std::ffi::c_void,
        );
    }

    // Wait for callback.
    let (lock, cvar) = &*pair;
    let guard = cvar
        .wait_while(lock.lock().unwrap(), |r| r.is_none())
        .unwrap();
    let result = guard.as_ref().unwrap();
    assert_eq!(result.status, 0, "async scan should succeed");
    assert!(!result.stream_ptr.is_null());

    // Read the stream.
    let ffi_stream = unsafe { &mut *(result.stream_ptr as *mut FFI_ArrowArrayStream) };
    let reader = unsafe { ArrowArrayStreamReader::from_raw(ffi_stream) }.unwrap();
    let total_rows: usize = reader.map(|r| r.unwrap().num_rows()).sum();
    assert_eq!(total_rows, 5);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

// ===========================================================================
// Additional tests
// ===========================================================================

// ---------------------------------------------------------------------------
// Schema field types validation
// ---------------------------------------------------------------------------

#[test]
fn test_schema_field_types() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let mut ffi_schema = FFI_ArrowSchema::empty();
    let rc = unsafe { lance_dataset_schema(ds, &mut ffi_schema) };
    assert_eq!(rc, 0);

    let schema = Schema::try_from(&ffi_schema).unwrap();
    assert_eq!(*schema.field(0).data_type(), DataType::Int32);
    assert_eq!(*schema.field(1).data_type(), DataType::Utf8);
    assert!(!schema.field(0).is_nullable());
    assert!(schema.field(1).is_nullable());

    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Latest version
// ---------------------------------------------------------------------------

#[test]
fn test_latest_version() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let latest = unsafe { lance_dataset_latest_version(ds) };
    let current = unsafe { lance_dataset_version(ds) };
    assert!(
        latest >= current,
        "latest({latest}) should be >= current({current})"
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::Ok);

    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Batch size control
// ---------------------------------------------------------------------------

#[test]
fn test_scanner_batch_size() {
    let (_tmp, uri) = create_large_dataset(100);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    assert!(!scanner.is_null());
    let rc = unsafe { lance_scanner_set_batch_size(scanner, 10) };
    assert_eq!(rc, 0);

    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe { lance_scanner_to_arrow_stream(scanner, &mut ffi_stream) };
    assert_eq!(rc, 0);

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    let batches: Vec<RecordBatch> = reader.map(|r| r.unwrap()).collect();

    assert!(
        batches.len() > 1,
        "expected multiple batches, got {}",
        batches.len()
    );
    let total_rows: usize = batches.iter().map(|b| b.num_rows()).sum();
    assert_eq!(total_rows, 100);

    for (i, b) in batches.iter().enumerate() {
        assert!(
            b.num_rows() <= 10,
            "batch {i} has {} rows, expected <= 10",
            b.num_rows()
        );
    }

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Combined filter + projection + limit
// ---------------------------------------------------------------------------

#[test]
fn test_scanner_combined_options() {
    let (_tmp, uri) = create_large_dataset(50);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let filter = c_str("id >= 10 AND id < 30");
    let col_id = c_str("id");
    let col_label = c_str("label");
    let columns: [*const i8; 3] = [col_id.as_ptr(), col_label.as_ptr(), ptr::null()];

    let scanner = unsafe { lance_scanner_new(ds, columns.as_ptr(), filter.as_ptr()) };
    assert!(!scanner.is_null());
    unsafe { lance_scanner_set_limit(scanner, 5) };

    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe { lance_scanner_to_arrow_stream(scanner, &mut ffi_stream) };
    assert_eq!(rc, 0);

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    let schema = reader.schema();
    assert_eq!(schema.fields().len(), 2);
    assert_eq!(schema.field(0).name(), "id");
    assert_eq!(schema.field(1).name(), "label");

    let batches: Vec<RecordBatch> = reader.map(|r| r.unwrap()).collect();
    let total_rows: usize = batches.iter().map(|b| b.num_rows()).sum();
    assert_eq!(total_rows, 5);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Take with column projection
// ---------------------------------------------------------------------------

#[test]
fn test_take_with_projection() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let indices: [u64; 2] = [1, 3];
    let col_name = c_str("name");
    let columns: [*const i8; 2] = [col_name.as_ptr(), ptr::null()];

    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    let rc =
        unsafe { lance_dataset_take(ds, indices.as_ptr(), 2, columns.as_ptr(), &mut ffi_stream) };
    assert_eq!(rc, 0);

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    let schema = reader.schema();
    assert_eq!(schema.fields().len(), 1);
    assert_eq!(schema.field(0).name(), "name");

    let batches: Vec<RecordBatch> = reader.map(|r| r.unwrap()).collect();
    assert_eq!(batches[0].num_rows(), 2);

    let names = batches[0]
        .column(0)
        .as_any()
        .downcast_ref::<StringArray>()
        .unwrap();
    assert_eq!(names.value(0), "bob");
    assert_eq!(names.value(1), "dave");

    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Multiple scanners on same dataset
// ---------------------------------------------------------------------------

#[test]
fn test_multiple_scanners_same_dataset() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let filter1 = c_str("id <= 2");
    let filter2 = c_str("id > 3");
    let scanner1 = unsafe { lance_scanner_new(ds, ptr::null(), filter1.as_ptr()) };
    let scanner2 = unsafe { lance_scanner_new(ds, ptr::null(), filter2.as_ptr()) };
    assert!(!scanner1.is_null());
    assert!(!scanner2.is_null());

    let mut stream1 = FFI_ArrowArrayStream::empty();
    let mut stream2 = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(scanner1, &mut stream1) },
        0
    );
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(scanner2, &mut stream2) },
        0
    );

    let reader1 = unsafe { ArrowArrayStreamReader::from_raw(&mut stream1) }.unwrap();
    let reader2 = unsafe { ArrowArrayStreamReader::from_raw(&mut stream2) }.unwrap();
    let rows1: usize = reader1.map(|r| r.unwrap().num_rows()).sum();
    let rows2: usize = reader2.map(|r| r.unwrap().num_rows()).sum();
    assert_eq!(rows1, 2); // id=1,2
    assert_eq!(rows2, 2); // id=4,5

    unsafe { lance_scanner_close(scanner1) };
    unsafe { lance_scanner_close(scanner2) };
    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Open with specific version
// ---------------------------------------------------------------------------

#[test]
fn test_open_specific_version() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);

    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 1) };
    assert!(!ds.is_null());
    assert_eq!(unsafe { lance_dataset_version(ds) }, 1);
    unsafe { lance_dataset_close(ds) };

    // Non-existent version should fail.
    let ds2 = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 9999) };
    assert!(ds2.is_null());
    assert_ne!(lance_last_error_code(), LanceErrorCode::Ok);
}

// ---------------------------------------------------------------------------
// Error: invalid filter / column
// ---------------------------------------------------------------------------

#[test]
fn test_scanner_invalid_filter() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let bad_filter = c_str("NOT A VALID >>> FILTER ???");
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), bad_filter.as_ptr()) };
    if !scanner.is_null() {
        let mut ffi_stream = FFI_ArrowArrayStream::empty();
        let rc = unsafe { lance_scanner_to_arrow_stream(scanner, &mut ffi_stream) };
        assert_eq!(rc, -1);
        assert_ne!(lance_last_error_code(), LanceErrorCode::Ok);
        let msg = lance_last_error_message();
        assert!(!msg.is_null());
        unsafe { lance_free_string(msg) };
        unsafe { lance_scanner_close(scanner) };
    }

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_invalid_column() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let col = c_str("nonexistent_column");
    let columns: [*const i8; 2] = [col.as_ptr(), ptr::null()];
    let scanner = unsafe { lance_scanner_new(ds, columns.as_ptr(), ptr::null()) };
    if !scanner.is_null() {
        let mut ffi_stream = FFI_ArrowArrayStream::empty();
        let rc = unsafe { lance_scanner_to_arrow_stream(scanner, &mut ffi_stream) };
        assert_eq!(rc, -1);
        assert_ne!(lance_last_error_code(), LanceErrorCode::Ok);
        unsafe { lance_scanner_close(scanner) };
    } else {
        assert_ne!(lance_last_error_code(), LanceErrorCode::Ok);
    }

    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Comprehensive NULL safety
// ---------------------------------------------------------------------------

#[test]
fn test_null_safety_comprehensive() {
    // Free functions with NULL should not crash.
    unsafe { lance_free_string(ptr::null()) };
    unsafe { lance_batch_free(ptr::null_mut()) };
    unsafe { lance_scanner_close(ptr::null_mut()) };
    unsafe { lance_dataset_close(ptr::null_mut()) };

    // Dataset functions with NULL.
    assert_eq!(unsafe { lance_dataset_count_rows(ptr::null()) }, 0);
    assert_ne!(lance_last_error_code(), LanceErrorCode::Ok);
    assert_eq!(unsafe { lance_dataset_latest_version(ptr::null()) }, 0);
    assert_ne!(lance_last_error_code(), LanceErrorCode::Ok);

    let mut ffi_schema = FFI_ArrowSchema::empty();
    assert_eq!(
        unsafe { lance_dataset_schema(ptr::null(), &mut ffi_schema) },
        -1
    );

    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    let indices: [u64; 1] = [0];
    assert_eq!(
        unsafe {
            lance_dataset_take(
                ptr::null(),
                indices.as_ptr(),
                1,
                ptr::null(),
                &mut ffi_stream,
            )
        },
        -1
    );

    // Scanner builder functions with NULL.
    assert_eq!(unsafe { lance_scanner_set_limit(ptr::null_mut(), 10) }, -1);
    assert_eq!(unsafe { lance_scanner_set_offset(ptr::null_mut(), 10) }, -1);
    assert_eq!(
        unsafe { lance_scanner_set_batch_size(ptr::null_mut(), 10) },
        -1
    );
    assert_eq!(
        unsafe { lance_scanner_with_row_id(ptr::null_mut(), true) },
        -1
    );

    // Scanner iteration with NULL.
    let mut ffi_stream2 = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(ptr::null_mut(), &mut ffi_stream2) },
        -1
    );
    let mut batch_ptr: *mut LanceBatch = ptr::null_mut();
    assert_eq!(
        unsafe { lance_scanner_next(ptr::null_mut(), &mut batch_ptr) },
        -1
    );

    // Batch functions with NULL.
    let mut ffi_array = arrow::ffi::FFI_ArrowArray::empty();
    let mut ffi_schema2 = FFI_ArrowSchema::empty();
    assert_eq!(
        unsafe { lance_batch_to_arrow(ptr::null(), &mut ffi_array, &mut ffi_schema2) },
        -1
    );
}

// ---------------------------------------------------------------------------
// Error message lifecycle
// ---------------------------------------------------------------------------

#[test]
fn test_error_message_lifecycle() {
    let c_uri = c_str("memory://does_not_exist_12345");
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(ds.is_null());
    assert_ne!(lance_last_error_code(), LanceErrorCode::Ok);

    let msg = lance_last_error_message();
    assert!(!msg.is_null());
    let msg_str = unsafe { std::ffi::CStr::from_ptr(msg) }.to_str().unwrap();
    assert!(!msg_str.is_empty());
    unsafe { lance_free_string(msg) };

    // Message consumed — next call returns NULL.
    let msg2 = lance_last_error_message();
    assert!(msg2.is_null());
}

// ---------------------------------------------------------------------------
// Large dataset scan
// ---------------------------------------------------------------------------

#[test]
fn test_large_dataset_scan() {
    let (_tmp, uri) = create_large_dataset(10_000);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 10_000);
    let batches = scan_all_rows(ds);
    let total: usize = batches.iter().map(|b| b.num_rows()).sum();
    assert_eq!(total, 10_000);

    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Equality filter with value verification
// ---------------------------------------------------------------------------

#[test]
fn test_scanner_equality_filter() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let filter = c_str("name = 'carol'");
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), filter.as_ptr()) };
    assert!(!scanner.is_null());

    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(scanner, &mut ffi_stream) },
        0
    );

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    let batches: Vec<RecordBatch> = reader.map(|r| r.unwrap()).collect();
    assert_eq!(batches.iter().map(|b| b.num_rows()).sum::<usize>(), 1);

    let id_col = batches[0]
        .column_by_name("id")
        .unwrap()
        .as_any()
        .downcast_ref::<Int32Array>()
        .unwrap();
    assert_eq!(id_col.value(0), 3);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Limit only / Offset only
// ---------------------------------------------------------------------------

#[test]
fn test_scanner_limit_only() {
    let (_tmp, uri) = create_large_dataset(50);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    unsafe { lance_scanner_set_limit(scanner, 7) };

    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(scanner, &mut ffi_stream) },
        0
    );
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    assert_eq!(reader.map(|r| r.unwrap().num_rows()).sum::<usize>(), 7);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_offset_only() {
    let (_tmp, uri) = create_large_dataset(20);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    unsafe { lance_scanner_set_offset(scanner, 15) };

    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(scanner, &mut ffi_stream) },
        0
    );
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    assert_eq!(reader.map(|r| r.unwrap().num_rows()).sum::<usize>(), 5);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Take edge cases
// ---------------------------------------------------------------------------

#[test]
fn test_take_empty_indices() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let indices: [u64; 0] = [];
    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe { lance_dataset_take(ds, indices.as_ptr(), 0, ptr::null(), &mut ffi_stream) };
    assert_eq!(rc, 0);

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    assert_eq!(reader.map(|r| r.unwrap().num_rows()).sum::<usize>(), 0);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_take_large_dataset_values() {
    let (_tmp, uri) = create_large_dataset(100);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let indices: [u64; 3] = [0, 50, 99];
    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_dataset_take(ds, indices.as_ptr(), 3, ptr::null(), &mut ffi_stream) },
        0
    );

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    let batches: Vec<RecordBatch> = reader.map(|r| r.unwrap()).collect();
    assert_eq!(batches[0].num_rows(), 3);

    let ids = batches[0]
        .column_by_name("id")
        .unwrap()
        .as_any()
        .downcast_ref::<Int32Array>()
        .unwrap();
    assert_eq!(ids.values(), &[0, 50, 99]);

    let labels = batches[0]
        .column_by_name("label")
        .unwrap()
        .as_any()
        .downcast_ref::<StringArray>()
        .unwrap();
    assert_eq!(labels.value(0), "row_0");
    assert_eq!(labels.value(1), "row_50");
    assert_eq!(labels.value(2), "row_99");

    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Async scan with filter
// ---------------------------------------------------------------------------

#[test]
fn test_async_scan_with_filter() {
    use std::sync::{Condvar, Mutex};

    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let filter = c_str("id <= 2");
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), filter.as_ptr()) };

    struct CallbackResult {
        status: i32,
        stream_ptr: *mut std::ffi::c_void,
    }
    unsafe impl Send for CallbackResult {}

    let pair = Arc::new((Mutex::new(None::<CallbackResult>), Condvar::new()));
    let pair_clone = pair.clone();

    unsafe extern "C" fn on_complete(
        ctx: *mut std::ffi::c_void,
        status: i32,
        result: *mut std::ffi::c_void,
    ) {
        let pair = unsafe { &*(ctx as *const (Mutex<Option<CallbackResult>>, Condvar)) };
        pair.0.lock().unwrap().replace(CallbackResult {
            status,
            stream_ptr: result,
        });
        pair.1.notify_one();
    }

    unsafe {
        lance_scanner_scan_async(
            scanner,
            on_complete,
            Arc::as_ptr(&pair_clone) as *mut std::ffi::c_void,
        );
    }

    let (lock, cvar) = &*pair;
    let guard = cvar
        .wait_while(lock.lock().unwrap(), |r| r.is_none())
        .unwrap();
    let result = guard.as_ref().unwrap();
    assert_eq!(result.status, 0);

    let ffi_stream = unsafe { &mut *(result.stream_ptr as *mut FFI_ArrowArrayStream) };
    let reader = unsafe { ArrowArrayStreamReader::from_raw(ffi_stream) }.unwrap();
    assert_eq!(reader.map(|r| r.unwrap().num_rows()).sum::<usize>(), 2);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Poll-based iteration
// ---------------------------------------------------------------------------

#[test]
fn test_poll_next_basic() {
    let (_tmp, uri) = create_test_dataset();
    let _c_uri = c_str(&uri);

    // poll_next calls materialize_stream() which uses block_on().
    // This must run on a non-tokio thread to avoid nested runtime panics.
    let uri_clone = uri.clone();
    let handle = std::thread::spawn(move || {
        let c_uri = c_str(&uri_clone);
        let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
        let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };

        use std::sync::atomic::{AtomicBool, Ordering};
        static WOKE: AtomicBool = AtomicBool::new(false);
        unsafe extern "C" fn test_waker(_ctx: *mut std::ffi::c_void) {
            WOKE.store(true, Ordering::SeqCst);
        }

        let mut total_rows = 0usize;
        let mut iterations = 0;
        loop {
            let mut batch: *mut LanceBatch = ptr::null_mut();
            let status = unsafe {
                lance_scanner_poll_next(scanner, test_waker, ptr::null_mut(), &mut batch)
            };
            match status {
                LancePollStatus::Ready => {
                    assert!(!batch.is_null());
                    let mut ffi_array = arrow::ffi::FFI_ArrowArray::empty();
                    let mut ffi_schema = FFI_ArrowSchema::empty();
                    unsafe { lance_batch_to_arrow(batch, &mut ffi_array, &mut ffi_schema) };
                    let data = unsafe { from_ffi(ffi_array, &ffi_schema) }.unwrap();
                    total_rows += data.len();
                    unsafe { lance_batch_free(batch) };
                }
                LancePollStatus::Pending => {
                    std::thread::sleep(std::time::Duration::from_millis(1));
                }
                LancePollStatus::Finished => break,
                LancePollStatus::Error => panic!("poll_next returned error"),
            }
            iterations += 1;
            assert!(iterations < 1000, "poll loop should not spin forever");
        }
        assert_eq!(total_rows, 5);

        unsafe { lance_scanner_close(scanner) };
        unsafe { lance_dataset_close(ds) };
    });
    handle.join().unwrap();
}

// ---------------------------------------------------------------------------
// Scan data value verification
// ---------------------------------------------------------------------------

#[test]
fn test_scan_data_values() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let batches = scan_all_rows(ds);
    let mut all_ids = Vec::new();
    let mut all_names = Vec::new();
    for batch in &batches {
        let ids = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        let names = batch
            .column_by_name("name")
            .unwrap()
            .as_any()
            .downcast_ref::<StringArray>()
            .unwrap();
        for i in 0..batch.num_rows() {
            all_ids.push(ids.value(i));
            all_names.push(names.value(i).to_string());
        }
    }
    assert_eq!(all_ids, vec![1, 2, 3, 4, 5]);
    assert_eq!(all_names, vec!["alice", "bob", "carol", "dave", "eve"]);

    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Reopen dataset / large dataset schema
// ---------------------------------------------------------------------------

#[test]
fn test_reopen_dataset() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);

    let ds1 = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert_eq!(unsafe { lance_dataset_count_rows(ds1) }, 5);
    unsafe { lance_dataset_close(ds1) };

    let ds2 = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert_eq!(unsafe { lance_dataset_count_rows(ds2) }, 5);
    assert_eq!(
        scan_all_rows(ds2)
            .iter()
            .map(|b| b.num_rows())
            .sum::<usize>(),
        5
    );

    unsafe { lance_dataset_close(ds2) };
}

#[test]
fn test_large_dataset_schema() {
    let (_tmp, uri) = create_large_dataset(10);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let mut ffi_schema = FFI_ArrowSchema::empty();
    assert_eq!(unsafe { lance_dataset_schema(ds, &mut ffi_schema) }, 0);

    let schema = Schema::try_from(&ffi_schema).unwrap();
    assert_eq!(schema.fields().len(), 3);
    assert_eq!(schema.field(0).name(), "id");
    assert_eq!(schema.field(1).name(), "value");
    assert_eq!(schema.field(2).name(), "label");
    assert_eq!(*schema.field(1).data_type(), DataType::Float32);

    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Fragment enumeration and fragment-scoped scanning
// ---------------------------------------------------------------------------

/// Helper: create a dataset with multiple fragments by writing multiple batches.
fn create_multi_fragment_dataset() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp
        .path()
        .join("multi_frag_ds")
        .to_str()
        .unwrap()
        .to_string();

    let schema = Arc::new(Schema::new(vec![Field::new("id", DataType::Int32, false)]));

    lance_c::runtime::block_on(async {
        // Write first fragment (rows 0..5)
        let batch1 = RecordBatch::try_new(
            schema.clone(),
            vec![Arc::new(Int32Array::from(vec![0, 1, 2, 3, 4]))],
        )
        .unwrap();
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch1)], schema.clone()),
            &uri,
            None,
        )
        .await
        .unwrap();

        // Append second fragment (rows 5..10)
        let batch2 = RecordBatch::try_new(
            schema.clone(),
            vec![Arc::new(Int32Array::from(vec![5, 6, 7, 8, 9]))],
        )
        .unwrap();
        let mut ds = Dataset::open(&uri).await.unwrap();
        ds.append(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch2)], schema.clone()),
            None,
        )
        .await
        .unwrap();
    });

    (tmp, uri)
}

#[test]
fn test_fragment_count() {
    let (_tmp, uri) = create_multi_fragment_dataset();
    let c_uri = c_str(&uri);

    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let count = unsafe { lance_dataset_fragment_count(ds) };
    assert_eq!(count, 2, "should have 2 fragments");

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_fragment_ids() {
    let (_tmp, uri) = create_multi_fragment_dataset();
    let c_uri = c_str(&uri);

    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let count = unsafe { lance_dataset_fragment_count(ds) };
    assert_eq!(count, 2);

    let mut ids = vec![0u64; count as usize];
    let rc = unsafe { lance_dataset_fragment_ids(ds, ids.as_mut_ptr()) };
    assert_eq!(rc, 0);
    assert_eq!(ids.len(), 2);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_with_fragment_ids() {
    let (_tmp, uri) = create_multi_fragment_dataset();
    let c_uri = c_str(&uri);

    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    // Get fragment IDs
    let count = unsafe { lance_dataset_fragment_count(ds) };
    let mut ids = vec![0u64; count as usize];
    unsafe { lance_dataset_fragment_ids(ds, ids.as_mut_ptr()) };

    // Scan only the first fragment
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    assert!(!scanner.is_null());
    let rc = unsafe { lance_scanner_set_fragment_ids(scanner, ids[..1].as_ptr(), 1) };
    assert_eq!(rc, 0);

    // Should get only 5 rows (first fragment)
    let batches = scan_all_rows_from_scanner(scanner);
    let total: usize = batches.iter().map(|b| b.num_rows()).sum();
    assert_eq!(total, 5, "scanning one fragment should yield 5 rows");

    unsafe { lance_scanner_close(scanner) };

    // Scan only the second fragment
    let scanner2 = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    unsafe { lance_scanner_set_fragment_ids(scanner2, ids[1..].as_ptr(), 1) };

    let batches2 = scan_all_rows_from_scanner(scanner2);
    let total2: usize = batches2.iter().map(|b| b.num_rows()).sum();
    assert_eq!(total2, 5, "scanning second fragment should yield 5 rows");

    unsafe { lance_scanner_close(scanner2) };
    unsafe { lance_dataset_close(ds) };
}

/// Helper: scan all rows from a scanner using batch iteration, returning RecordBatches.
fn scan_all_rows_from_scanner(scanner: *mut LanceScanner) -> Vec<RecordBatch> {
    let mut batches = Vec::new();
    loop {
        let mut batch_ptr: *mut LanceBatch = ptr::null_mut();
        let rc = unsafe { lance_scanner_next(scanner, &mut batch_ptr) };
        if rc == 1 {
            break; // end of stream
        }
        assert_eq!(rc, 0, "scanner_next should succeed");
        assert!(!batch_ptr.is_null());
        let mut ffi_array = arrow::ffi::FFI_ArrowArray::empty();
        let mut ffi_schema = FFI_ArrowSchema::empty();
        unsafe { lance_batch_to_arrow(batch_ptr, &mut ffi_array, &mut ffi_schema) };
        let data = unsafe { from_ffi(ffi_array, &ffi_schema) }.unwrap();
        let struct_array = arrow_array::StructArray::from(data);
        batches.push(RecordBatch::from(struct_array));
        unsafe { lance_batch_free(batch_ptr) };
    }
    batches
}

// ---------------------------------------------------------------------------
// Tests with checked-in historical test datasets
// ---------------------------------------------------------------------------

/// Helper: resolve path to a checked-in test dataset.
fn test_data_path(relative: &str) -> String {
    let path = if let Ok(test_data_dir) = std::env::var("LANCE_TEST_DATA") {
        std::path::PathBuf::from(test_data_dir).join(relative)
    } else {
        let mut path = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        path.push("test_data");
        path.push(relative);
        path
    };
    assert!(path.exists(), "Test data not found at {}", path.display());
    path.to_str().unwrap().to_string()
}

#[test]
fn test_historical_dataset_v0_27_1() {
    let uri = test_data_path("v0.27.1/pq_in_schema");
    let c_uri = c_str(&uri);

    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null(), "should open historical dataset");

    let version = unsafe { lance_dataset_version(ds) };
    assert!(version >= 1);

    let count = unsafe { lance_dataset_count_rows(ds) };
    assert!(count > 0, "historical dataset should have rows");

    let mut ffi_schema = FFI_ArrowSchema::empty();
    let rc = unsafe { lance_dataset_schema(ds, &mut ffi_schema) };
    assert_eq!(rc, 0);
    let schema = Schema::try_from(&ffi_schema).unwrap();
    assert!(!schema.fields().is_empty(), "schema should have fields");

    let batches = scan_all_rows(ds);
    let total: usize = batches.iter().map(|b| b.num_rows()).sum();
    assert_eq!(total, count as usize);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_historical_dataset_open_specific_version() {
    let uri = test_data_path("v0.27.1/pq_in_schema");
    let c_uri = c_str(&uri);

    // This dataset has 2 versions.
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 1) };
    assert!(!ds.is_null());
    assert_eq!(unsafe { lance_dataset_version(ds) }, 1);
    let count_v1 = unsafe { lance_dataset_count_rows(ds) };
    assert!(count_v1 > 0);
    unsafe { lance_dataset_close(ds) };

    let ds2 = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 2) };
    assert!(!ds2.is_null());
    assert_eq!(unsafe { lance_dataset_version(ds2) }, 2);
    unsafe { lance_dataset_close(ds2) };
}

// ---------------------------------------------------------------------------
// Fragment writer
// ---------------------------------------------------------------------------

/// Helper: build an FFI_ArrowArrayStream from a single RecordBatch.
fn batch_to_ffi_stream(batch: RecordBatch) -> FFI_ArrowArrayStream {
    let schema = batch.schema();
    let reader = arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema);
    FFI_ArrowArrayStream::new(Box::new(reader))
}

/// Helper: export an Arrow Schema to FFI_ArrowSchema.
fn schema_to_ffi(schema: &Schema) -> FFI_ArrowSchema {
    FFI_ArrowSchema::try_from(schema).expect("schema export must succeed")
}

#[test]
fn test_write_fragments_creates_data_files() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = format!("file://{}", tmp.path().to_str().unwrap());
    let c_uri = CString::new(uri.clone()).unwrap();

    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("val", DataType::Float32, true),
    ]));
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from(vec![1, 2, 3])),
            Arc::new(Float32Array::from(vec![1.0, 2.0, 3.0])),
        ],
    )
    .unwrap();

    let ffi_schema = schema_to_ffi(&schema);
    let mut stream = batch_to_ffi_stream(batch);
    let rc =
        unsafe { lance_write_fragments(c_uri.as_ptr(), &ffi_schema, &mut stream, ptr::null()) };
    assert_eq!(rc, 0, "lance_write_fragments failed");

    // Data files should exist under data/.
    let data_dir = tmp.path().join("data");
    assert!(data_dir.exists(), "data/ dir must exist");

    let lance_files: Vec<_> = std::fs::read_dir(&data_dir)
        .unwrap()
        .filter_map(|e| e.ok())
        .filter(|e| e.path().extension().is_some_and(|ext| ext == "lance"))
        .collect();
    assert!(
        !lance_files.is_empty(),
        "expected at least one .lance data file"
    );
}

#[test]
fn test_write_fragments_null_args_returns_error() {
    let schema = Arc::new(Schema::new(vec![Field::new("x", DataType::Int32, false)]));
    let batch =
        RecordBatch::try_new(schema.clone(), vec![Arc::new(Int32Array::from(vec![1]))]).unwrap();
    let mut stream = batch_to_ffi_stream(batch);

    // NULL uri
    let ffi_schema = schema_to_ffi(&schema);
    let result =
        unsafe { lance_write_fragments(ptr::null(), &ffi_schema, &mut stream, ptr::null()) };
    assert_eq!(result, -1);
    assert_ne!(lance_last_error_code(), LanceErrorCode::Ok);
}

#[test]
fn test_write_fragments_schema_mismatch() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = format!("file://{}", tmp.path().to_str().unwrap());
    let c_uri = CString::new(uri).unwrap();

    // Stream has columns (id: Int32, val: Float32)
    let stream_schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("val", DataType::Float32, true),
    ]));
    let batch = RecordBatch::try_new(
        stream_schema.clone(),
        vec![
            Arc::new(Int32Array::from(vec![1])),
            Arc::new(Float32Array::from(vec![1.0])),
        ],
    )
    .unwrap();
    let mut stream = batch_to_ffi_stream(batch);

    // But the declared schema only has (id: Int32) — mismatch.
    let declared_schema = Schema::new(vec![Field::new("id", DataType::Int32, false)]);
    let ffi_schema = schema_to_ffi(&declared_schema);

    let rc =
        unsafe { lance_write_fragments(c_uri.as_ptr(), &ffi_schema, &mut stream, ptr::null()) };
    assert_eq!(rc, -1, "should fail on schema mismatch");
    assert_ne!(lance_last_error_code(), LanceErrorCode::Ok);
}

// ---------------------------------------------------------------------------
// End-to-end robotics scenario: C++ writes fragments, Rust finalizer commits
// ---------------------------------------------------------------------------

/// Simulate the full robotics ingestion pipeline:
///   1. C++ edge device writes sensor data via lance_write_fragments
///   2. Separate Rust finalizer scans .lance files, reconstructs Fragment
///      metadata from file footers, and commits into a dataset
///   3. The committed dataset is readable and contains the original data
#[test]
fn test_robotics_e2e_write_then_finalize() {
    use lance::dataset::transaction::{Operation, Transaction};
    use lance::dataset::{CommitBuilder, WriteDestination};
    use lance_file::reader::{CachedFileMetadata, FileReader as LanceFileReader};
    use lance_io::scheduler::{ScanScheduler, SchedulerConfig};
    use lance_io::utils::CachedFileSize;
    use lance_table::format::{DataFile, Fragment};

    // ── Step 1: "C++ edge device" writes fragment data files ──

    let staging_dir = tempfile::tempdir().unwrap();
    let staging_uri = format!("file://{}", staging_dir.path().to_str().unwrap());
    let c_uri = CString::new(staging_uri.clone()).unwrap();

    let schema = Arc::new(Schema::new(vec![
        Field::new("sensor_id", DataType::Int32, false),
        Field::new("temperature", DataType::Float32, true),
        Field::new("label", DataType::Utf8, true),
    ]));

    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from(vec![1, 2, 3, 4, 5])),
            Arc::new(Float32Array::from(vec![20.1, 21.5, 19.8, 22.0, 20.5])),
            Arc::new(StringArray::from(vec![
                "front", "rear", "left", "right", "top",
            ])),
        ],
    )
    .unwrap();

    let ffi_schema = schema_to_ffi(&schema);
    let mut stream = batch_to_ffi_stream(batch);
    let rc =
        unsafe { lance_write_fragments(c_uri.as_ptr(), &ffi_schema, &mut stream, ptr::null()) };
    assert_eq!(rc, 0, "lance_write_fragments failed");

    // ── Step 2: "Rust finalizer" scans files and reconstructs fragments ──

    let dataset_dir = tempfile::tempdir().unwrap();
    let dataset_uri = dataset_dir
        .path()
        .join("robot.lance")
        .to_str()
        .unwrap()
        .to_string();

    let fragments = lance_c::runtime::block_on(async {
        let (object_store, _base_path) =
            lance_io::object_store::ObjectStore::from_uri(&staging_uri)
                .await
                .unwrap();
        let scan_scheduler = ScanScheduler::new(
            object_store.clone(),
            SchedulerConfig::max_bandwidth(&object_store),
        );

        // Discover .lance files in data/ directory
        let data_dir = staging_dir.path().join("data");
        let lance_files: Vec<_> = std::fs::read_dir(&data_dir)
            .unwrap()
            .filter_map(|e| e.ok())
            .filter(|e| e.path().extension().is_some_and(|ext| ext == "lance"))
            .collect();
        assert!(!lance_files.is_empty());

        let mut fragments = Vec::new();
        for (frag_idx, entry) in lance_files.iter().enumerate() {
            let filename = entry.file_name().to_string_lossy().to_string();
            let file_path = lance_io::object_store::ObjectStore::extract_path_from_uri(
                Arc::new(Default::default()),
                &format!("{}/data/{}", staging_uri, filename),
            )
            .unwrap();

            let file_size: CachedFileSize = Default::default();
            let file_scheduler = scan_scheduler
                .open_file(&file_path, &file_size)
                .await
                .unwrap();
            let meta: CachedFileMetadata = LanceFileReader::read_all_metadata(&file_scheduler)
                .await
                .unwrap();

            // Reconstruct DataFile from footer metadata
            let field_ids: Vec<i32> = meta.file_schema.field_ids();
            let column_indices: Vec<i32> = (0..field_ids.len() as i32).collect();

            let data_file = DataFile::new(
                format!("data/{}", filename),
                field_ids,
                column_indices,
                meta.major_version as u32,
                meta.minor_version as u32,
                None, // file_size_bytes
                None, // base_id
            );

            let mut fragment = Fragment::new(frag_idx as u64);
            fragment.files.push(data_file);
            fragment.physical_rows = Some(meta.num_rows as usize);
            fragments.push(fragment);
        }
        fragments
    });

    assert!(!fragments.is_empty());
    let total_rows: usize = fragments.iter().filter_map(|f| f.physical_rows).sum();
    assert_eq!(total_rows, 5);

    // ── Step 3: Commit fragments into a new dataset ──

    // Copy data files to the dataset directory first
    let src_data = staging_dir.path().join("data");
    let dst_data = dataset_dir.path().join("robot.lance").join("data");
    std::fs::create_dir_all(&dst_data).unwrap();
    for entry in std::fs::read_dir(&src_data).unwrap() {
        let entry = entry.unwrap();
        std::fs::copy(entry.path(), dst_data.join(entry.file_name())).unwrap();
    }

    // Build a lance schema from the arrow schema for the Overwrite operation
    let lance_schema = lance_core::datatypes::Schema::try_from(schema.as_ref()).unwrap();

    let transaction = Transaction::new(
        0,
        Operation::Overwrite {
            fragments,
            schema: lance_schema,
            config_upsert_values: None,
            initial_bases: None,
        },
        None,
    );

    lance_c::runtime::block_on(async {
        CommitBuilder::new(WriteDestination::Uri(&dataset_uri))
            .execute(transaction)
            .await
            .unwrap();
    });

    // ── Step 4: Verify the committed dataset is readable ──

    let c_ds_uri = CString::new(dataset_uri.clone()).unwrap();
    let ds = unsafe { lance_dataset_open(c_ds_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null(), "failed to open committed dataset");

    let count = unsafe { lance_dataset_count_rows(ds) };
    assert_eq!(count, 5, "committed dataset should have 5 rows");

    let frag_count = unsafe { lance_dataset_fragment_count(ds) };
    assert_eq!(frag_count, 1, "committed dataset should have 1 fragment");

    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Version history (lance_dataset_versions)
// ---------------------------------------------------------------------------

/// Helper: open an existing dataset and append a batch, creating a new version.
fn append_batch(uri: &str, schema: Arc<Schema>, batch: RecordBatch) {
    lance_c::runtime::block_on(async {
        let mut ds = Dataset::open(uri).await.unwrap();
        ds.append(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
            None,
        )
        .await
        .unwrap();
    });
}

#[test]
fn test_dataset_versions_single_version() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let vs = unsafe { lance_dataset_versions(ds) };
    assert!(!vs.is_null());
    assert_eq!(unsafe { lance_versions_count(vs) }, 1);
    assert_eq!(unsafe { lance_versions_id_at(vs, 0) }, 1);
    assert!(unsafe { lance_versions_timestamp_ms_at(vs, 0) } > 0);

    unsafe { lance_versions_close(vs) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_dataset_versions_multiple_versions() {
    let (_tmp, uri) = create_test_dataset();
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("name", DataType::Utf8, true),
    ]));
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from(vec![6, 7])),
            Arc::new(StringArray::from(vec!["frank", "grace"])),
        ],
    )
    .unwrap();
    append_batch(&uri, schema, batch);

    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let vs = unsafe { lance_dataset_versions(ds) };

    let count = unsafe { lance_versions_count(vs) };
    assert_eq!(count, 2);

    let id0 = unsafe { lance_versions_id_at(vs, 0) };
    let id1 = unsafe { lance_versions_id_at(vs, 1) };
    assert_eq!(id0, 1);
    assert_eq!(id1, 2);

    let ts0 = unsafe { lance_versions_timestamp_ms_at(vs, 0) };
    let ts1 = unsafe { lance_versions_timestamp_ms_at(vs, 1) };
    assert!(ts0 > 0, "timestamps should be populated");
    assert!(
        ts1 >= ts0,
        "timestamps should be monotonic by version order"
    );

    unsafe { lance_versions_close(vs) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_dataset_versions_null_dataset() {
    let vs = unsafe { lance_dataset_versions(ptr::null()) };
    assert!(vs.is_null());
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

#[test]
fn test_versions_count_null_handle() {
    let n = unsafe { lance_versions_count(ptr::null()) };
    assert_eq!(n, 0);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

#[test]
fn test_versions_index_out_of_range() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let vs = unsafe { lance_dataset_versions(ds) };

    // Count is 1 for a freshly-created dataset. Exercise both the exact
    // boundary (index == count) and a clearly-out-of-range index.
    let count = unsafe { lance_versions_count(vs) };
    for index in [count as usize, 5] {
        let id = unsafe { lance_versions_id_at(vs, index) };
        assert_eq!(id, 0);
        assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

        let ts = unsafe { lance_versions_timestamp_ms_at(vs, index) };
        assert_eq!(ts, 0);
        assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    }

    unsafe { lance_versions_close(vs) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_versions_accessors_null_handle() {
    let id = unsafe { lance_versions_id_at(ptr::null(), 0) };
    assert_eq!(id, 0);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    let ts = unsafe { lance_versions_timestamp_ms_at(ptr::null(), 0) };
    assert_eq!(ts, 0);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

#[test]
fn test_versions_close_null_is_safe() {
    unsafe { lance_versions_close(ptr::null_mut()) };
}

// ---------------------------------------------------------------------------
// Restore (lance_dataset_restore)
// ---------------------------------------------------------------------------

/// Helper: set up a dataset with two versions — initial create (rows 1..=5)
/// plus an append (rows 6..=7), returning `(tempdir, uri)`.
fn create_two_version_dataset() -> (tempfile::TempDir, String) {
    let (tmp, uri) = create_test_dataset();
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("name", DataType::Utf8, true),
    ]));
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from(vec![6, 7])),
            Arc::new(StringArray::from(vec!["frank", "grace"])),
        ],
    )
    .unwrap();
    append_batch(&uri, schema, batch);
    (tmp, uri)
}

#[test]
fn test_dataset_restore_to_prior_version() {
    let (_tmp, uri) = create_two_version_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert_eq!(unsafe { lance_dataset_version(ds) }, 2);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 7);

    // Restore to V1 — expect a fresh handle at a new version (3) with V1's
    // row count (5).
    let restored = unsafe { lance_dataset_restore(ds, 1) };
    assert!(!restored.is_null());
    assert_eq!(unsafe { lance_dataset_version(restored) }, 3);
    assert_eq!(unsafe { lance_dataset_count_rows(restored) }, 5);

    // Original handle is untouched.
    assert_eq!(unsafe { lance_dataset_version(ds) }, 2);

    unsafe { lance_dataset_close(restored) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_dataset_restore_to_current_latest_writes_new_manifest() {
    // Restoring to the current latest still writes a new manifest. The
    // optimization that previously skipped the commit was racy: a concurrent
    // writer could land a newer manifest between the staleness check and the
    // skip, silently leaving their version as latest. We always commit so the
    // caller's "make `version` the new latest" intent holds unconditionally.
    let (_tmp, uri) = create_two_version_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let latest = unsafe { lance_dataset_version(ds) };
    assert_eq!(latest, 2);

    let restored = unsafe { lance_dataset_restore(ds, latest) };
    assert!(!restored.is_null());
    assert_eq!(
        unsafe { lance_dataset_version(restored) },
        latest + 1,
        "restore to latest must commit a new manifest to defeat TOCTOU races"
    );
    assert_eq!(unsafe { lance_dataset_count_rows(restored) }, 7);

    // Reopening the dataset reports the bumped latest.
    unsafe { lance_dataset_close(restored) };
    let ds2 = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert_eq!(unsafe { lance_dataset_version(ds2) }, latest + 1);

    unsafe { lance_dataset_close(ds2) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_dataset_restore_nonexistent_version() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let restored = unsafe { lance_dataset_restore(ds, 999) };
    assert!(restored.is_null());
    assert_eq!(lance_last_error_code(), LanceErrorCode::NotFound);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_dataset_restore_version_zero_rejected() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let restored = unsafe { lance_dataset_restore(ds, 0) };
    assert!(restored.is_null());
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_dataset_restore_null_dataset_rejected() {
    let restored = unsafe { lance_dataset_restore(ptr::null(), 1) };
    assert!(restored.is_null());
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Index lifecycle tests (Phase 2)
// ---------------------------------------------------------------------------

#[test]
fn test_create_scalar_index_btree() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let column = c_str("id");
    let rc = unsafe {
        lance_dataset_create_scalar_index(
            ds,
            column.as_ptr(),
            ptr::null(), /* default name */
            LanceScalarIndexType::BTree as i32,
            ptr::null(), /* no params */
            false,
        )
    };
    assert_eq!(
        rc,
        0,
        "create_scalar_index returned {} ({:?})",
        rc,
        unsafe { std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy() }
    );

    let count = unsafe { lance_dataset_index_count(ds) };
    assert_eq!(count, 1);

    unsafe { lance_dataset_close(ds) };
}

/// Helper: create a dataset with a List<Utf8> column for LabelList index testing.
fn create_label_list_dataset() -> (tempfile::TempDir, String) {
    use arrow_array::ListArray;
    use arrow_array::builder::{ListBuilder, StringBuilder};

    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ll_ds").to_str().unwrap().to_string();

    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new(
            "tags",
            DataType::List(Arc::new(Field::new("item", DataType::Utf8, true))),
            true,
        ),
    ]));

    let mut tag_builder = ListBuilder::new(StringBuilder::new());
    tag_builder.values().append_value("rust");
    tag_builder.values().append_value("ffi");
    tag_builder.append(true);
    tag_builder.values().append_value("cpp");
    tag_builder.append(true);
    let tags: ListArray = tag_builder.finish();

    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![Arc::new(Int32Array::from(vec![1, 2])), Arc::new(tags)],
    )
    .unwrap();

    lance_c::runtime::block_on(async {
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
            &uri,
            None,
        )
        .await
        .unwrap();
    });

    (tmp, uri)
}

#[test]
fn test_create_scalar_index_bitmap() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("name");
    let rc = unsafe {
        lance_dataset_create_scalar_index(
            ds,
            column.as_ptr(),
            ptr::null(),
            LanceScalarIndexType::Bitmap as i32,
            ptr::null(),
            false,
        )
    };
    assert_eq!(rc, 0);
    assert_eq!(unsafe { lance_dataset_index_count(ds) }, 1);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_create_scalar_index_inverted() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("name");
    // Inverted index requires JSON params with at least `base_tokenizer` and
    // `language`. Pass the documented defaults.
    let params = c_str(r#"{"base_tokenizer":"simple","language":"English"}"#);
    let rc = unsafe {
        lance_dataset_create_scalar_index(
            ds,
            column.as_ptr(),
            ptr::null(),
            LanceScalarIndexType::Inverted as i32,
            params.as_ptr(),
            false,
        )
    };
    assert_eq!(rc, 0, "{}", unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });
    assert_eq!(unsafe { lance_dataset_index_count(ds) }, 1);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_create_scalar_index_label_list() {
    let (_tmp, uri) = create_label_list_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("tags");
    let rc = unsafe {
        lance_dataset_create_scalar_index(
            ds,
            column.as_ptr(),
            ptr::null(),
            LanceScalarIndexType::LabelList as i32,
            ptr::null(),
            false,
        )
    };
    assert_eq!(rc, 0, "{}", unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });
    assert_eq!(unsafe { lance_dataset_index_count(ds) }, 1);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_drop_index() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("id");
    let name = c_str("my_idx");

    unsafe {
        lance_dataset_create_scalar_index(
            ds,
            column.as_ptr(),
            name.as_ptr(),
            LanceScalarIndexType::BTree as i32,
            ptr::null(),
            false,
        );
    }
    assert_eq!(unsafe { lance_dataset_index_count(ds) }, 1);

    let rc = unsafe { lance_dataset_drop_index(ds, name.as_ptr()) };
    assert_eq!(rc, 0);
    assert_eq!(unsafe { lance_dataset_index_count(ds) }, 0);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_drop_missing_index() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let name = c_str("does_not_exist");
    let rc = unsafe { lance_dataset_drop_index(ds, name.as_ptr()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::NotFound);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_list_indices_json() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("id");
    let name = c_str("id_btree");
    unsafe {
        lance_dataset_create_scalar_index(
            ds,
            column.as_ptr(),
            name.as_ptr(),
            LanceScalarIndexType::BTree as i32,
            ptr::null(),
            false,
        );
    }

    let json_ptr = unsafe { lance_dataset_index_list_json(ds) };
    assert!(!json_ptr.is_null());
    let json = unsafe {
        std::ffi::CStr::from_ptr(json_ptr)
            .to_str()
            .unwrap()
            .to_string()
    };
    unsafe { lance_free_string(json_ptr) };

    assert!(json.contains("\"name\":\"id_btree\""), "json was: {}", json);
    assert!(json.contains("\"columns\":[\"id\"]"), "json was: {}", json);
    assert!(json.contains("\"type\""), "json was: {}", json);

    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Vector index lifecycle tests (Phase 2)
// ---------------------------------------------------------------------------

/// Helper: create a dataset with a FixedSizeList<Float32> column for vector index testing.
fn create_vector_dataset(num_rows: i32, dim: i32) -> (tempfile::TempDir, String) {
    use arrow_array::FixedSizeListArray;
    use arrow_array::builder::{FixedSizeListBuilder, Float32Builder};

    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("vec_ds").to_str().unwrap().to_string();

    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new(
            "embedding",
            DataType::FixedSizeList(Arc::new(Field::new("item", DataType::Float32, true)), dim),
            false,
        ),
        Field::new("text", DataType::Utf8, true),
    ]));

    let mut emb_builder = FixedSizeListBuilder::new(Float32Builder::new(), dim);
    let texts: Vec<String> = (0..num_rows).map(|i| format!("doc {i}")).collect();
    let mut rng_seed: u32 = 1;
    for _ in 0..num_rows {
        for _ in 0..dim {
            // simple deterministic pseudo-random in [0,1)
            rng_seed = rng_seed.wrapping_mul(1664525).wrapping_add(1013904223);
            emb_builder
                .values()
                .append_value((rng_seed as f32) / (u32::MAX as f32));
        }
        emb_builder.append(true);
    }
    let embeddings: FixedSizeListArray = emb_builder.finish();
    let text_refs: Vec<&str> = texts.iter().map(|s| s.as_str()).collect();

    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from((0..num_rows).collect::<Vec<_>>())),
            Arc::new(embeddings) as Arc<dyn arrow_array::Array>,
            Arc::new(StringArray::from(text_refs)),
        ],
    )
    .unwrap();

    lance_c::runtime::block_on(async {
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
            &uri,
            None,
        )
        .await
        .unwrap();
    });

    (tmp, uri)
}

#[test]
fn test_create_vector_index_ivf_flat() {
    let (_tmp, uri) = create_vector_dataset(256, 16);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("embedding");
    let params = LanceVectorIndexParams {
        index_type: LanceVectorIndexType::IvfFlat,
        metric: LanceMetricType::L2,
        num_partitions: 8,
        num_sub_vectors: 0,
        num_bits: 0,
        max_iterations: 0,
        hnsw_m: 0,
        hnsw_ef_construction: 0,
        sample_rate: 0,
    };
    let rc = unsafe {
        lance_dataset_create_vector_index(ds, column.as_ptr(), ptr::null(), &params, false)
    };
    assert_eq!(rc, 0, "{}", unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });
    assert_eq!(unsafe { lance_dataset_index_count(ds) }, 1);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_create_vector_index_ivf_pq() {
    let (_tmp, uri) = create_vector_dataset(256, 16);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("embedding");
    let params = LanceVectorIndexParams {
        index_type: LanceVectorIndexType::IvfPq,
        metric: LanceMetricType::L2,
        num_partitions: 8,
        num_sub_vectors: 4,
        num_bits: 8,
        max_iterations: 0,
        hnsw_m: 0,
        hnsw_ef_construction: 0,
        sample_rate: 0,
    };
    let rc = unsafe {
        lance_dataset_create_vector_index(ds, column.as_ptr(), ptr::null(), &params, false)
    };
    assert_eq!(rc, 0, "{}", unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_create_vector_index_ivf_hnsw_sq() {
    let (_tmp, uri) = create_vector_dataset(256, 16);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("embedding");
    let params = LanceVectorIndexParams {
        index_type: LanceVectorIndexType::IvfHnswSq,
        metric: LanceMetricType::L2,
        num_partitions: 8,
        num_sub_vectors: 0,
        num_bits: 0,
        max_iterations: 0,
        hnsw_m: 16,
        hnsw_ef_construction: 100,
        sample_rate: 0,
    };
    let rc = unsafe {
        lance_dataset_create_vector_index(ds, column.as_ptr(), ptr::null(), &params, false)
    };
    assert_eq!(rc, 0, "{}", unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_vector_index_missing_required_param() {
    let (_tmp, uri) = create_vector_dataset(256, 16);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("embedding");
    let params = LanceVectorIndexParams {
        index_type: LanceVectorIndexType::IvfPq,
        metric: LanceMetricType::L2,
        num_partitions: 8,
        num_sub_vectors: 0, // missing!
        num_bits: 0,
        max_iterations: 0,
        hnsw_m: 0,
        hnsw_ef_construction: 0,
        sample_rate: 0,
    };
    let rc = unsafe {
        lance_dataset_create_vector_index(ds, column.as_ptr(), ptr::null(), &params, false)
    };
    assert_eq!(rc, -1);
    let msg = unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message())
            .to_string_lossy()
            .into_owned()
    };
    assert!(msg.contains("num_sub_vectors"), "msg was: {}", msg);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_create_index_replace_true() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("id");
    let name = c_str("dup");
    unsafe {
        lance_dataset_create_scalar_index(
            ds,
            column.as_ptr(),
            name.as_ptr(),
            LanceScalarIndexType::BTree as i32,
            ptr::null(),
            false,
        );
    }
    let rc = unsafe {
        lance_dataset_create_scalar_index(
            ds,
            column.as_ptr(),
            name.as_ptr(),
            LanceScalarIndexType::BTree as i32,
            ptr::null(),
            true,
        )
    };
    assert_eq!(rc, 0, "replace=true should succeed");
    assert_eq!(unsafe { lance_dataset_index_count(ds) }, 1);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_create_index_replace_false_conflicts() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("id");
    let name = c_str("dup2");
    unsafe {
        lance_dataset_create_scalar_index(
            ds,
            column.as_ptr(),
            name.as_ptr(),
            LanceScalarIndexType::BTree as i32,
            ptr::null(),
            false,
        );
    }
    let rc = unsafe {
        lance_dataset_create_scalar_index(
            ds,
            column.as_ptr(),
            name.as_ptr(),
            LanceScalarIndexType::BTree as i32,
            ptr::null(),
            false,
        )
    };
    assert_eq!(rc, -1);
    let code = lance_last_error_code();
    assert!(
        code == LanceErrorCode::IndexError || code == LanceErrorCode::InvalidArgument,
        "expected IndexError or InvalidArgument, got {:?}",
        code
    );
    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Vector search (k-NN) tests (Phase 2)
// ---------------------------------------------------------------------------

#[test]
fn test_scanner_nearest_brute_force() {
    let (_tmp, uri) = create_vector_dataset(64, 8);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    assert!(!scanner.is_null());

    let query: Vec<f32> = (0..8).map(|i| i as f32 * 0.1).collect();
    let column = c_str("embedding");
    let rc = unsafe {
        lance_scanner_nearest(
            scanner,
            column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void,
            query.len(),
            LanceDataType::Float32 as i32,
            5,
        )
    };
    assert_eq!(rc, 0, "{}", unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });

    let mut stream = FFI_ArrowArrayStream::empty();
    let rc2 = unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) };
    assert_eq!(rc2, 0);

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream as *mut _).unwrap() };
    let schema = reader.schema();
    let saw_distance = schema.field_with_name("_distance").is_ok();

    let mut total = 0;
    for batch in reader {
        let b = batch.unwrap();
        total += b.num_rows();
    }
    assert!(saw_distance, "_distance column missing from schema");
    assert_eq!(total, 5, "expected k=5 results");

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_nearest_with_ivf_pq_index() {
    let (_tmp, uri) = create_vector_dataset(512, 16);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("embedding");
    let params = LanceVectorIndexParams {
        index_type: LanceVectorIndexType::IvfPq,
        metric: LanceMetricType::L2,
        num_partitions: 8,
        num_sub_vectors: 4,
        num_bits: 8,
        max_iterations: 0,
        hnsw_m: 0,
        hnsw_ef_construction: 0,
        sample_rate: 0,
    };
    unsafe {
        lance_dataset_create_vector_index(ds, column.as_ptr(), ptr::null(), &params, false);
    }

    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    let query: Vec<f32> = vec![0.5; 16];
    unsafe {
        lance_scanner_nearest(
            scanner,
            column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void,
            16,
            LanceDataType::Float32 as i32,
            10,
        );
        lance_scanner_set_nprobes(scanner, 4);
    }

    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) },
        0
    );
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream as *mut _).unwrap() };
    let mut total = 0;
    for batch in reader {
        total += batch.unwrap().num_rows();
    }
    assert_eq!(total, 10);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_nearest_dim_mismatch() {
    let (_tmp, uri) = create_vector_dataset(64, 8);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    let query: Vec<f32> = vec![0.0; 4]; // wrong dim — column is 8
    let column = c_str("embedding");

    // The dim mismatch is caught either by lance_scanner_nearest itself or by
    // build_scanner when materializing the stream. Either is acceptable.
    let nearest_rc = unsafe {
        lance_scanner_nearest(
            scanner,
            column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void,
            4,
            LanceDataType::Float32 as i32,
            5,
        )
    };

    let final_failed = if nearest_rc != 0 {
        true
    } else {
        let mut stream = FFI_ArrowArrayStream::empty();
        let rc = unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) };
        rc != 0
    };
    assert!(
        final_failed,
        "expected dim mismatch error somewhere in the pipeline"
    );
    let msg = unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message())
            .to_string_lossy()
            .into_owned()
    };
    assert!(msg.to_lowercase().contains("dim"), "msg was: {}", msg);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_nearest_filter_postfilter() {
    let (_tmp, uri) = create_vector_dataset(64, 8);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let filter = c_str("id < 10");
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), filter.as_ptr()) };
    let query: Vec<f32> = vec![0.5; 8];
    let column = c_str("embedding");
    unsafe {
        lance_scanner_nearest(
            scanner,
            column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void,
            8,
            LanceDataType::Float32 as i32,
            20,
        );
    }
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) },
        0
    );
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream as *mut _).unwrap() };
    let mut total = 0;
    for b in reader {
        total += b.unwrap().num_rows();
    }
    // Post-filter on top-20 nearest: count is 0..20 depending on data.
    // We just assert the call succeeds and returns at most 20 rows.
    assert!(total <= 20);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_nearest_multi_fragment() {
    use arrow_array::builder::{FixedSizeListBuilder, Float32Builder};

    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("multifrag").to_str().unwrap().to_string();
    let dim: i32 = 8;
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new(
            "embedding",
            DataType::FixedSizeList(Arc::new(Field::new("item", DataType::Float32, true)), dim),
            false,
        ),
    ]));

    let mut batches = Vec::new();
    for frag in 0..2i32 {
        let mut emb = FixedSizeListBuilder::new(Float32Builder::new(), dim);
        let ids: Vec<i32> = (0..32i32).map(|i| frag * 32 + i).collect();
        for _ in 0..32 {
            for _ in 0..dim {
                emb.values().append_value(0.5);
            }
            emb.append(true);
        }
        batches.push(
            RecordBatch::try_new(
                schema.clone(),
                vec![Arc::new(Int32Array::from(ids)), Arc::new(emb.finish())],
            )
            .unwrap(),
        );
    }

    lance_c::runtime::block_on(async {
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(
                vec![Ok(batches[0].clone())],
                schema.clone(),
            ),
            &uri,
            None,
        )
        .await
        .unwrap();
        let params = lance::dataset::WriteParams {
            mode: lance::dataset::WriteMode::Append,
            ..Default::default()
        };
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batches[1].clone())], schema),
            &uri,
            Some(params),
        )
        .await
        .unwrap();
    });

    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    assert_eq!(unsafe { lance_dataset_fragment_count(ds) }, 2);

    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    let column = c_str("embedding");
    let query: Vec<f32> = vec![0.5; 8];
    unsafe {
        lance_scanner_nearest(
            scanner,
            column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void,
            8,
            LanceDataType::Float32 as i32,
            20,
        );
    }
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) },
        0
    );
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream as *mut _).unwrap() };
    let mut total = 0;
    for b in reader {
        total += b.unwrap().num_rows();
    }
    assert_eq!(total, 20);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_nearest_null_safety() {
    let column = c_str("embedding");
    let query: Vec<f32> = vec![0.0; 8];
    // NULL scanner
    let rc = unsafe {
        lance_scanner_nearest(
            ptr::null_mut(),
            column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void,
            8,
            LanceDataType::Float32 as i32,
            5,
        )
    };
    assert_eq!(rc, -1);

    // Build a valid scanner.
    let (_tmp, uri) = create_vector_dataset(8, 8);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };

    // NULL column.
    let rc2 = unsafe {
        lance_scanner_nearest(
            scanner,
            ptr::null(),
            query.as_ptr() as *const std::ffi::c_void,
            8,
            LanceDataType::Float32 as i32,
            5,
        )
    };
    assert_eq!(rc2, -1);

    // NULL query_data.
    let rc3 = unsafe {
        lance_scanner_nearest(
            scanner,
            column.as_ptr(),
            ptr::null(),
            8,
            LanceDataType::Float32 as i32,
            5,
        )
    };
    assert_eq!(rc3, -1);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_full_text_search() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("name");
    // Build inverted index on `name` first.
    let inverted_params = c_str(r#"{"base_tokenizer":"simple","language":"English"}"#);
    unsafe {
        lance_dataset_create_scalar_index(
            ds,
            column.as_ptr(),
            ptr::null(),
            LanceScalarIndexType::Inverted as i32,
            inverted_params.as_ptr(),
            false,
        );
    }
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    let q = c_str("alice");
    let cols = [column.as_ptr(), ptr::null()];
    let rc = unsafe { lance_scanner_full_text_search(scanner, q.as_ptr(), cols.as_ptr(), 0) };
    assert_eq!(rc, 0, "{}", unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });

    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) },
        0
    );
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream as *mut _).unwrap() };
    let schema = reader.schema();
    assert!(
        schema.field_with_name("_score").is_ok(),
        "_score column missing from schema"
    );
    let mut total = 0;
    for b in reader {
        total += b.unwrap().num_rows();
    }
    assert!(total >= 1, "expected at least 1 hit for 'alice'");
    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_fts_fuzzy() {
    let (_tmp, uri) = create_test_dataset();
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("name");
    let inverted_params = c_str(r#"{"base_tokenizer":"simple","language":"English"}"#);
    unsafe {
        lance_dataset_create_scalar_index(
            ds,
            column.as_ptr(),
            ptr::null(),
            LanceScalarIndexType::Inverted as i32,
            inverted_params.as_ptr(),
            false,
        );
    }
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    // "alise" within edit distance 2 of "alice" (in the test fixture).
    let q = c_str("alise");
    let cols = [column.as_ptr(), ptr::null()];
    let rc = unsafe { lance_scanner_full_text_search(scanner, q.as_ptr(), cols.as_ptr(), 2) };
    assert_eq!(rc, 0, "{}", unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message()).to_string_lossy()
    });

    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) },
        0
    );
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream as *mut _).unwrap() };
    let mut total = 0;
    for b in reader {
        total += b.unwrap().num_rows();
    }
    assert!(total >= 1, "expected fuzzy match for 'alise' → 'alice'");

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_nearest_after_fts_is_rejected() {
    let (_tmp, uri) = create_vector_dataset(64, 8);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };

    // Set FTS first (no inverted index needed for this test — error happens
    // at the second call, before any stream materialization).
    let q = c_str("foo");
    unsafe {
        lance_scanner_full_text_search(scanner, q.as_ptr(), ptr::null(), 0);
    }

    let column = c_str("embedding");
    let query: Vec<f32> = vec![0.5; 8];
    let rc = unsafe {
        lance_scanner_nearest(
            scanner,
            column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void,
            8,
            LanceDataType::Float32 as i32,
            5,
        )
    };
    assert_eq!(rc, -1);
    let msg = unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message())
            .to_string_lossy()
            .into_owned()
    };
    let lower = msg.to_lowercase();
    assert!(
        lower.contains("full_text")
            || lower.contains("fts")
            || lower.contains("mutually exclusive"),
        "msg was: {}",
        msg
    );

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// Dataset writer (lance_dataset_write)
// ---------------------------------------------------------------------------

fn write_schema() -> Arc<Schema> {
    Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("val", DataType::Float32, true),
    ]))
}

fn write_batch(ids: Vec<i32>, vals: Vec<f32>) -> RecordBatch {
    assert_eq!(ids.len(), vals.len());
    RecordBatch::try_new(
        write_schema(),
        vec![
            Arc::new(Int32Array::from(ids)),
            Arc::new(Float32Array::from(vals)),
        ],
    )
    .unwrap()
}

#[test]
fn test_dataset_write_create() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("new_ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);

    let ffi_schema = schema_to_ffi(&write_schema());
    let mut stream = batch_to_ffi_stream(write_batch(vec![1, 2, 3], vec![1.0, 2.0, 3.0]));

    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0, "lance_dataset_write create failed");
    assert_eq!(lance_last_error_code(), LanceErrorCode::Ok);

    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_dataset_write_populates_out_dataset() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);

    let ffi_schema = schema_to_ffi(&write_schema());
    let mut stream = batch_to_ffi_stream(write_batch(vec![1, 2, 3], vec![1.0, 2.0, 3.0]));

    let mut out_ds: *mut LanceDataset = ptr::null_mut();
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            ptr::null(),
            &mut out_ds,
        )
    };
    assert_eq!(rc, 0);
    assert!(!out_ds.is_null(), "out_dataset must be populated");
    assert_eq!(unsafe { lance_dataset_count_rows(out_ds) }, 3);
    unsafe { lance_dataset_close(out_ds) };
}

#[test]
fn test_dataset_write_append_accumulates_rows() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);

    let ffi_schema1 = schema_to_ffi(&write_schema());
    let mut stream1 = batch_to_ffi_stream(write_batch(vec![1, 2, 3], vec![1.0, 2.0, 3.0]));
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema1,
            &mut stream1,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);

    let ffi_schema2 = schema_to_ffi(&write_schema());
    let mut stream2 = batch_to_ffi_stream(write_batch(vec![4, 5], vec![4.0, 5.0]));
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema2,
            &mut stream2,
            LanceWriteMode::Append as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);

    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 5);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_dataset_write_overwrite_replaces_rows() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);

    let ffi_schema1 = schema_to_ffi(&write_schema());
    let mut stream1 = batch_to_ffi_stream(write_batch(vec![1, 2, 3], vec![1.0, 2.0, 3.0]));
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema1,
            &mut stream1,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);

    let ffi_schema2 = schema_to_ffi(&write_schema());
    let mut stream2 = batch_to_ffi_stream(write_batch(vec![100, 200], vec![100.0, 200.0]));
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema2,
            &mut stream2,
            LanceWriteMode::Overwrite as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);

    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());
    assert_eq!(
        unsafe { lance_dataset_count_rows(ds) },
        2,
        "overwrite must replace, not append"
    );
    let batches = scan_all_rows(ds);
    assert!(!batches.is_empty(), "scan must return at least one batch");
    let mut ids: Vec<i32> = Vec::new();
    for batch in &batches {
        let id_col = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        ids.extend((0..id_col.len()).map(|i| id_col.value(i)));
    }
    ids.sort();
    assert_eq!(ids, vec![100, 200]);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_dataset_write_overwrite_on_missing_path_creates_dataset() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);

    let ffi_schema = schema_to_ffi(&write_schema());
    let mut stream = batch_to_ffi_stream(write_batch(vec![7, 8], vec![7.0, 8.0]));

    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Overwrite as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0, "OVERWRITE on missing path must succeed as create");

    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 2);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_dataset_write_invalid_mode_rejected() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);

    let ffi_schema = schema_to_ffi(&write_schema());
    let mut stream = batch_to_ffi_stream(write_batch(vec![1], vec![1.0]));

    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            99, // out of range — must be rejected, not cause UB
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

#[test]
fn test_dataset_write_create_on_existing_fails() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);

    let ffi_schema1 = schema_to_ffi(&write_schema());
    let mut stream1 = batch_to_ffi_stream(write_batch(vec![1], vec![1.0]));
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema1,
            &mut stream1,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);

    let ffi_schema2 = schema_to_ffi(&write_schema());
    let mut stream2 = batch_to_ffi_stream(write_batch(vec![2], vec![2.0]));
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema2,
            &mut stream2,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(
        lance_last_error_code(),
        LanceErrorCode::DatasetAlreadyExists
    );
}

#[test]
fn test_dataset_write_append_schema_mismatch_fails() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);

    // Create with the original schema.
    let ffi_schema1 = schema_to_ffi(&write_schema());
    let mut stream1 = batch_to_ffi_stream(write_batch(vec![1, 2], vec![1.0, 2.0]));
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema1,
            &mut stream1,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);

    // Append with an extra column → must fail.
    let mismatched_schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("val", DataType::Float32, true),
        Field::new("extra", DataType::Utf8, true),
    ]));
    let batch2 = RecordBatch::try_new(
        mismatched_schema.clone(),
        vec![
            Arc::new(Int32Array::from(vec![10])),
            Arc::new(Float32Array::from(vec![10.0])),
            Arc::new(StringArray::from(vec!["x"])),
        ],
    )
    .unwrap();
    let ffi_schema2 = schema_to_ffi(&mismatched_schema);
    let mut stream2 = batch_to_ffi_stream(batch2);
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema2,
            &mut stream2,
            LanceWriteMode::Append as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    // Upstream Lance currently surfaces append-with-mismatched-schema as
    // `Internal` rather than `InvalidArgument`. Lock the assertion to the
    // observed code so we notice (and can revisit the mapping) if it changes.
    assert_eq!(lance_last_error_code(), LanceErrorCode::Internal);
}

#[test]
fn test_dataset_write_declared_schema_mismatch_fails() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);

    // Stream has 2 columns but declared schema has only 1 — fail fast.
    let mut stream = batch_to_ffi_stream(write_batch(vec![1], vec![1.0]));
    let declared_schema = Schema::new(vec![Field::new("id", DataType::Int32, false)]);
    let ffi_schema = schema_to_ffi(&declared_schema);

    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

#[test]
fn test_dataset_write_empty_stream_creates_empty_dataset() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("empty_ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);

    let schema = write_schema();
    let ffi_schema = schema_to_ffi(&schema);

    let empty: Vec<arrow::error::Result<RecordBatch>> = vec![];
    let reader = arrow::record_batch::RecordBatchIterator::new(empty, schema.clone());
    let mut stream = FFI_ArrowArrayStream::new(Box::new(reader));

    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);

    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 0);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_fts_after_nearest_is_rejected() {
    let (_tmp, uri) = create_vector_dataset(64, 8);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    let column = c_str("embedding");
    let query: Vec<f32> = vec![0.5; 8];
    unsafe {
        lance_scanner_nearest(
            scanner,
            column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void,
            8,
            LanceDataType::Float32 as i32,
            5,
        );
    }
    let q = c_str("foo");
    let rc = unsafe { lance_scanner_full_text_search(scanner, q.as_ptr(), ptr::null(), 0) };
    assert_eq!(rc, -1);
    let msg = unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message())
            .to_string_lossy()
            .into_owned()
    };
    let lower = msg.to_lowercase();
    assert!(
        lower.contains("nearest")
            || lower.contains("vector")
            || lower.contains("mutually exclusive"),
        "msg was: {}",
        msg
    );

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_dataset_write_null_args_return_error() {
    let schema = write_schema();
    let c_uri = c_str("memory://x");

    // NULL uri.
    let ffi_schema_a = schema_to_ffi(&schema);
    let mut stream_a = batch_to_ffi_stream(write_batch(vec![1], vec![1.0]));
    let rc = unsafe {
        lance_dataset_write(
            ptr::null(),
            &ffi_schema_a,
            &mut stream_a,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    // NULL schema.
    let mut stream_b = batch_to_ffi_stream(write_batch(vec![1], vec![1.0]));
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            ptr::null(),
            &mut stream_b,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    // NULL stream.
    let ffi_schema_c = schema_to_ffi(&schema);
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema_c,
            ptr::null_mut(),
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

/// A `RecordBatchReader` that bumps a shared counter when it is dropped.
/// Wrapping this in an `FFI_ArrowArrayStream` lets a test observe whether the
/// stream's `release` callback was invoked: dropping the boxed reader (via
/// `release` on the FFI side) fires `Drop` and increments the counter.
struct CountingReader<R: RecordBatchReader> {
    inner: R,
    drop_count: Arc<std::sync::atomic::AtomicUsize>,
}

impl<R: RecordBatchReader> Drop for CountingReader<R> {
    fn drop(&mut self) {
        self.drop_count
            .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    }
}

impl<R: RecordBatchReader> Iterator for CountingReader<R> {
    type Item = arrow::error::Result<RecordBatch>;
    fn next(&mut self) -> Option<Self::Item> {
        self.inner.next()
    }
}

impl<R: RecordBatchReader> RecordBatchReader for CountingReader<R> {
    fn schema(&self) -> Arc<Schema> {
        self.inner.schema()
    }
}

/// Like `new_column_stream`, but the reader's `Drop` increments a counter so a
/// test can prove the stream is consumed (released) on a given path. The single
/// `name` column avoids colliding with the fixtures' existing columns.
fn make_counted_column_stream(
    name: &str,
    values: Vec<i32>,
) -> (FFI_ArrowArrayStream, Arc<std::sync::atomic::AtomicUsize>) {
    let drop_count = Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let schema = Arc::new(Schema::new(vec![Field::new(name, DataType::Int32, true)]));
    let batch =
        RecordBatch::try_new(schema.clone(), vec![Arc::new(Int32Array::from(values))]).unwrap();
    let reader = CountingReader {
        inner: arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
        drop_count: drop_count.clone(),
    };
    (FFI_ArrowArrayStream::new(Box::new(reader)), drop_count)
}

/// Build a `(stream, drop_counter)` pair where the stream wraps a single-batch
/// reader whose `Drop` increments the counter. After a call that consumes the
/// stream, the counter goes from 0 → 1.
fn make_counted_stream(
    schema: &Arc<Schema>,
) -> (FFI_ArrowArrayStream, Arc<std::sync::atomic::AtomicUsize>) {
    let drop_count = Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let reader = CountingReader {
        inner: arrow::record_batch::RecordBatchIterator::new(
            vec![Ok(write_batch(vec![1], vec![1.0]))].into_iter(),
            schema.clone(),
        ),
        drop_count: drop_count.clone(),
    };
    (FFI_ArrowArrayStream::new(Box::new(reader)), drop_count)
}

fn assert_stream_consumed(
    _stream: &FFI_ArrowArrayStream,
    drop_count: &Arc<std::sync::atomic::AtomicUsize>,
) {
    // The drop count is the real behavioral check — it can only reach 1 if
    // the FFI release callback fired, which is what frees the boxed reader.
    // (We do not also assert `stream.release.is_none()` because `from_raw`
    // unconditionally clears that field via `ptr::replace` before any other
    // work; the assertion would be vacuously true on every path.)
    assert_eq!(
        drop_count.load(std::sync::atomic::Ordering::SeqCst),
        1,
        "stream's release callback must fire exactly once during the call"
    );
}

/// FFI contract: every error path that received a non-NULL stream must also
/// release it, so the C caller never has to. We assert this by wrapping the
/// reader in a `Drop`-counter and checking the counter immediately after each
/// `lance_dataset_write` call. The cases below exercise every validation
/// branch in `write_dataset_inner` that runs *after* the stream has been
/// consumed via `from_raw` — including NULL uri/schema, which were previously
/// gated *before* consumption (the bug R1 fixed).
#[test]
fn test_dataset_write_releases_stream_on_every_error_path() {
    let schema = write_schema();
    let c_uri = c_str("memory://x");

    // Each case that passes a non-NULL schema constructs its own
    // `FFI_ArrowSchema` via `schema_to_ffi` so the cases stay independent: a
    // hypothetical regression where Rust accidentally consumes the schema
    // would surface as an immediate failure here instead of silently
    // corrupting later cases. Case 2 deliberately passes `ptr::null()` and
    // therefore needs no schema construction.

    // Case 1: NULL uri.
    let (mut stream, drop_count) = make_counted_stream(&schema);
    let ffi_schema = schema_to_ffi(&schema);
    let rc = unsafe {
        lance_dataset_write(
            ptr::null(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_stream_consumed(&stream, &drop_count);

    // Case 2: NULL schema.
    let (mut stream, drop_count) = make_counted_stream(&schema);
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            ptr::null(),
            &mut stream,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_stream_consumed(&stream, &drop_count);

    // Case 3: invalid mode.
    let (mut stream, drop_count) = make_counted_stream(&schema);
    let ffi_schema = schema_to_ffi(&schema);
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            99,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_stream_consumed(&stream, &drop_count);

    // Case 4: empty URI.
    let (mut stream, drop_count) = make_counted_stream(&schema);
    let ffi_schema = schema_to_ffi(&schema);
    let empty_uri = c_str("");
    let rc = unsafe {
        lance_dataset_write(
            empty_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_stream_consumed(&stream, &drop_count);

    // Case 5: declared-schema mismatch.
    let (mut stream, drop_count) = make_counted_stream(&schema);
    let one_col_schema = Schema::new(vec![Field::new("id", DataType::Int32, false)]);
    let ffi_schema = schema_to_ffi(&one_col_schema);
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_stream_consumed(&stream, &drop_count);

    // Case 6: Lance-level rejection (CREATE on an existing dataset). This is
    // the only error path that fails inside `block_on(Dataset::write)` after
    // the stream has been moved into the upstream writer. Verifies the stream
    // is still released even when the failure originates upstream.
    let tmp = tempfile::tempdir().unwrap();
    let existing_uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_existing = c_str(&existing_uri);
    // Seed the path with an initial dataset.
    let mut seed_stream = batch_to_ffi_stream(write_batch(vec![1], vec![1.0]));
    let seed_schema = schema_to_ffi(&schema);
    let rc = unsafe {
        lance_dataset_write(
            c_existing.as_ptr(),
            &seed_schema,
            &mut seed_stream,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);
    // Now CREATE again — expected to fail with DatasetAlreadyExists, and the
    // stream must still be released by the failure path.
    let ffi_schema = schema_to_ffi(&schema);
    let (mut stream, drop_count) = make_counted_stream(&schema);
    let rc = unsafe {
        lance_dataset_write(
            c_existing.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(
        lance_last_error_code(),
        LanceErrorCode::DatasetAlreadyExists
    );
    assert_stream_consumed(&stream, &drop_count);
}

/// On error, `*out_dataset` must be left untouched. A caller that passes
/// `&mut some_existing_handle` (perhaps re-using the slot) must be able to
/// trust that a failed call does not silently overwrite or close their handle.
/// Covers both pre-`block_on` validation errors (NULL uri) and Lance-level
/// errors (CREATE on existing) — the contract holds across the success-prep
/// boundary.
#[test]
fn test_dataset_write_leaves_out_dataset_untouched_on_error() {
    let schema = write_schema();

    // Sentinel that is non-NULL but otherwise invalid. `without_provenance_mut`
    // (stable since 1.84) creates the pointer without exposing provenance —
    // strict-provenance-clean. We never dereference it; the test only checks
    // value equality after the call to confirm `*out_dataset` was not written.
    let sentinel: *mut LanceDataset = std::ptr::without_provenance_mut(0xDEAD_BEEF);

    // Case 1: pre-`block_on` validation error (NULL uri).
    let mut stream = batch_to_ffi_stream(write_batch(vec![1], vec![1.0]));
    let ffi_schema = schema_to_ffi(&schema);
    let mut out_ds = sentinel;
    let rc = unsafe {
        lance_dataset_write(
            ptr::null(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            ptr::null(),
            &mut out_ds,
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(
        out_ds, sentinel,
        "*out_dataset must be untouched on pre-block_on error"
    );

    // Case 2: Lance-level error (CREATE on an existing dataset). Verifies the
    // contract still holds when failure originates inside `block_on(write)`.
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);
    let mut seed_stream = batch_to_ffi_stream(write_batch(vec![1], vec![1.0]));
    let seed_schema = schema_to_ffi(&schema);
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &seed_schema,
            &mut seed_stream,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);

    let mut stream = batch_to_ffi_stream(write_batch(vec![2], vec![2.0]));
    let ffi_schema = schema_to_ffi(&schema);
    let mut out_ds = sentinel;
    let rc = unsafe {
        lance_dataset_write(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            ptr::null(),
            &mut out_ds,
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(
        lance_last_error_code(),
        LanceErrorCode::DatasetAlreadyExists
    );
    assert_eq!(
        out_ds, sentinel,
        "*out_dataset must be untouched on Lance-level error"
    );
}

// ---------------------------------------------------------------------------
// Substrait filter tests
// ---------------------------------------------------------------------------

/// Build a serialized Substrait `ExtendedExpression` for `id > 3`
/// against the test dataset's schema (id: Int32, name: Utf8).
fn substrait_id_gt_3() -> Vec<u8> {
    use datafusion::logical_expr::{col, lit};
    use datafusion::prelude::SessionContext;
    use lance_datafusion::substrait::encode_substrait;

    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("name", DataType::Utf8, true),
    ]));
    let expr = col("id").gt(lit(3i32));
    let state = SessionContext::new().state();
    encode_substrait(expr, schema, &state).unwrap()
}

#[test]
fn test_scanner_with_substrait_filter() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let bytes = substrait_id_gt_3();
    assert!(!bytes.is_empty(), "encoded substrait must be non-empty");

    // Create scanner with no SQL filter, then attach Substrait filter.
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    assert!(!scanner.is_null());

    let rc = unsafe { lance_scanner_set_substrait_filter(scanner, bytes.as_ptr(), bytes.len()) };
    assert_eq!(
        rc,
        0,
        "set_substrait_filter should succeed; err: {:?}",
        lance_last_error_code()
    );

    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe { lance_scanner_to_arrow_stream(scanner, &mut ffi_stream) };
    assert_eq!(rc, 0);

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    let total_rows: usize = reader.map(|r| r.unwrap().num_rows()).sum();
    assert_eq!(total_rows, 2, "id > 3 should match 2 rows (id=4, id=5)");

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_substrait_filter_overrides_sql_filter() {
    // If both SQL and Substrait filters are set, Substrait wins (last write).
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // Start with SQL filter "id < 0" (matches 0 rows).
    let sql = c_str("id < 0");
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), sql.as_ptr()) };
    assert!(!scanner.is_null());

    // Override with Substrait filter "id > 3" (matches 2 rows).
    let bytes = substrait_id_gt_3();
    let rc = unsafe { lance_scanner_set_substrait_filter(scanner, bytes.as_ptr(), bytes.len()) };
    assert_eq!(rc, 0);

    let mut ffi_stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe { lance_scanner_to_arrow_stream(scanner, &mut ffi_stream) };
    assert_eq!(rc, 0);

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut ffi_stream) }.unwrap();
    let total_rows: usize = reader.map(|r| r.unwrap().num_rows()).sum();
    assert_eq!(total_rows, 2, "Substrait filter should override SQL filter");

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_set_substrait_filter_invalid_inputs() {
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    assert!(!scanner.is_null());

    let bytes = [0u8; 4];

    // NULL scanner.
    let rc =
        unsafe { lance_scanner_set_substrait_filter(ptr::null_mut(), bytes.as_ptr(), bytes.len()) };
    assert_eq!(rc, -1);

    // NULL bytes pointer with non-zero len.
    let rc = unsafe { lance_scanner_set_substrait_filter(scanner, ptr::null(), 4) };
    assert_eq!(rc, -1);

    // Zero len (empty filter) is rejected.
    let rc = unsafe { lance_scanner_set_substrait_filter(scanner, bytes.as_ptr(), 0) };
    assert_eq!(rc, -1);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

// ===========================================================================
// lance_dataset_write_with_params (Issue #15)
// ===========================================================================

fn default_write_params() -> LanceWriteParams {
    LanceWriteParams {
        max_rows_per_file: 0,
        max_rows_per_group: 0,
        max_bytes_per_file: 0,
        data_storage_version: ptr::null(),
        enable_stable_row_ids: false,
    }
}

/// Build a larger batch than the minimal test batch so `max_rows_per_file`
/// has enough rows to exercise multi-file output.
fn large_write_batch(n: i32) -> RecordBatch {
    let ids: Vec<i32> = (0..n).collect();
    let vals: Vec<f32> = (0..n).map(|i| i as f32).collect();
    write_batch(ids, vals)
}

#[test]
fn test_write_with_params_null_is_like_plain_write() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);

    let ffi_schema = schema_to_ffi(&write_schema());
    let mut stream = batch_to_ffi_stream(write_batch(vec![1, 2, 3], vec![1.0, 2.0, 3.0]));

    let rc = unsafe {
        lance_dataset_write_with_params(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            ptr::null(),
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);

    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_write_with_params_max_rows_per_file_splits_fragments() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);

    let ffi_schema = schema_to_ffi(&write_schema());
    let mut stream = batch_to_ffi_stream(large_write_batch(100));

    let mut params = default_write_params();
    params.max_rows_per_file = 20;

    let mut out_ds: *mut LanceDataset = ptr::null_mut();
    let rc = unsafe {
        lance_dataset_write_with_params(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            &params,
            ptr::null(),
            &mut out_ds,
        )
    };
    assert_eq!(rc, 0);
    assert!(!out_ds.is_null());

    // 100 rows / 20 per file → at least 5 fragments.
    let frag_count = unsafe { lance_dataset_fragment_count(out_ds) };
    assert!(
        frag_count >= 5,
        "expected at least 5 fragments, got {frag_count}"
    );
    assert_eq!(unsafe { lance_dataset_count_rows(out_ds) }, 100);

    unsafe { lance_dataset_close(out_ds) };
}

#[test]
fn test_write_with_params_accepts_known_storage_version() {
    for version_str in ["2.0", "2.1", "stable"] {
        let tmp = tempfile::tempdir().unwrap();
        let uri = tmp.path().join("ds").to_str().unwrap().to_string();
        let c_uri = c_str(&uri);
        let version_cstr = c_str(version_str);

        let ffi_schema = schema_to_ffi(&write_schema());
        let mut stream = batch_to_ffi_stream(write_batch(vec![1], vec![1.0]));

        let mut params = default_write_params();
        params.data_storage_version = version_cstr.as_ptr();

        let rc = unsafe {
            lance_dataset_write_with_params(
                c_uri.as_ptr(),
                &ffi_schema,
                &mut stream,
                LanceWriteMode::Create as i32,
                &params,
                ptr::null(),
                ptr::null_mut(),
            )
        };
        assert_eq!(rc, 0, "version {version_str} should be accepted");
    }
}

#[test]
fn test_write_with_params_max_rows_per_group_accepted() {
    // Row-group layout isn't easily observable from FFI; confirm the field
    // is plumbed by writing successfully with a non-zero value.
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);

    let ffi_schema = schema_to_ffi(&write_schema());
    let mut stream = batch_to_ffi_stream(large_write_batch(50));

    let mut params = default_write_params();
    params.max_rows_per_group = 10;

    let rc = unsafe {
        lance_dataset_write_with_params(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            &params,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);
}

#[test]
fn test_write_with_params_max_bytes_per_file_accepted() {
    // Small-byte-cap behaviour depends on input size crossing the cap; this
    // test just confirms the field is plumbed (non-zero value accepted).
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);

    let ffi_schema = schema_to_ffi(&write_schema());
    let mut stream = batch_to_ffi_stream(write_batch(vec![1, 2, 3], vec![1.0, 2.0, 3.0]));

    let mut params = default_write_params();
    params.max_bytes_per_file = 1024 * 1024; // 1 MiB

    let rc = unsafe {
        lance_dataset_write_with_params(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            &params,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);
}

#[test]
fn test_write_with_params_rejects_empty_storage_version() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);
    let empty = c_str("");

    let ffi_schema = schema_to_ffi(&write_schema());
    let mut stream = batch_to_ffi_stream(write_batch(vec![1], vec![1.0]));

    let mut params = default_write_params();
    params.data_storage_version = empty.as_ptr();

    let rc = unsafe {
        lance_dataset_write_with_params(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            &params,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

#[test]
fn test_write_with_params_rejects_invalid_storage_version() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);
    let bad_version = c_str("banana");

    let ffi_schema = schema_to_ffi(&write_schema());
    let mut stream = batch_to_ffi_stream(write_batch(vec![1], vec![1.0]));

    let mut params = default_write_params();
    params.data_storage_version = bad_version.as_ptr();

    let rc = unsafe {
        lance_dataset_write_with_params(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            &params,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

#[test]
fn test_write_with_params_stable_row_ids_accepted() {
    // Toggle is accepted end-to-end; verifying the flag landed in the
    // manifest would require upstream inspection we don't want to reach
    // into from the FFI crate, so confirm only that the write succeeds.
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_string();
    let c_uri = c_str(&uri);

    let ffi_schema = schema_to_ffi(&write_schema());
    let mut stream = batch_to_ffi_stream(write_batch(vec![1, 2, 3], vec![1.0, 2.0, 3.0]));

    let mut params = default_write_params();
    params.enable_stable_row_ids = true;

    let rc = unsafe {
        lance_dataset_write_with_params(
            c_uri.as_ptr(),
            &ffi_schema,
            &mut stream,
            LanceWriteMode::Create as i32,
            &params,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);
}

// ===========================================================================
// lance_dataset_delete
// ===========================================================================

#[test]
fn test_delete_basic_predicate() {
    let (_tmp, uri) = create_large_dataset(100);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let pred = c_str("id >= 50");
    let mut num_deleted: u64 = 0;
    let rc = unsafe { lance_dataset_delete(ds, pred.as_ptr(), &mut num_deleted) };
    assert_eq!(rc, 0);
    assert_eq!(num_deleted, 50);

    // Existing handle now sees the post-delete dataset.
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 50);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_delete_all_rows() {
    let (_tmp, uri) = create_large_dataset(20);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let pred = c_str("true");
    let mut num_deleted: u64 = 0;
    let rc = unsafe { lance_dataset_delete(ds, pred.as_ptr(), &mut num_deleted) };
    assert_eq!(rc, 0);
    assert_eq!(num_deleted, 20);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 0);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_delete_no_match_returns_zero() {
    let (_tmp, uri) = create_large_dataset(10);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let pred = c_str("id > 9999");
    let mut num_deleted: u64 = 0;
    let rc = unsafe { lance_dataset_delete(ds, pred.as_ptr(), &mut num_deleted) };
    assert_eq!(rc, 0);
    assert_eq!(num_deleted, 0);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 10);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_delete_out_param_optional() {
    let (_tmp, uri) = create_large_dataset(10);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let pred = c_str("id < 3");
    // Pass NULL out_num_deleted — must succeed without writing anything.
    let rc = unsafe { lance_dataset_delete(ds, pred.as_ptr(), ptr::null_mut()) };
    assert_eq!(rc, 0);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 7);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_delete_bumps_version() {
    let (_tmp, uri) = create_large_dataset(5);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let v_before = unsafe { lance_dataset_version(ds) };
    let pred = c_str("id = 0");
    let rc = unsafe { lance_dataset_delete(ds, pred.as_ptr(), ptr::null_mut()) };
    assert_eq!(rc, 0);
    let v_after = unsafe { lance_dataset_version(ds) };
    assert!(
        v_after > v_before,
        "version should increase: before={v_before}, after={v_after}"
    );

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_delete_null_dataset_rejected() {
    let pred = c_str("id > 0");
    let rc = unsafe { lance_dataset_delete(ptr::null_mut(), pred.as_ptr(), ptr::null_mut()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

// Locks in the documented contract: when the call fails, `out_num_deleted`
// must be left unchanged. A future refactor that pre-zeroes the slot before
// validating inputs would silently break this guarantee.
#[test]
fn test_delete_out_param_untouched_on_error() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let mut sentinel: u64 = 0xDEAD_BEEF;
    // Empty predicate → INVALID_ARGUMENT before any work happens.
    let pred = c_str("");
    let rc = unsafe { lance_dataset_delete(ds, pred.as_ptr(), &mut sentinel) };
    assert_eq!(rc, -1);
    assert_eq!(sentinel, 0xDEAD_BEEF, "out slot must be untouched on error");

    // Same property must hold for upstream-surfaced errors (malformed SQL).
    let pred = c_str("not a real predicate ((((");
    let rc = unsafe { lance_dataset_delete(ds, pred.as_ptr(), &mut sentinel) };
    assert_eq!(rc, -1);
    assert_eq!(sentinel, 0xDEAD_BEEF, "out slot must be untouched on error");

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_delete_null_predicate_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let rc = unsafe { lance_dataset_delete(ds, ptr::null(), ptr::null_mut()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    // Dataset is unchanged.
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_delete_empty_predicate_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let pred = c_str("");
    let rc = unsafe { lance_dataset_delete(ds, pred.as_ptr(), ptr::null_mut()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_delete_invalid_predicate_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    // Garbage SQL — Lance / DataFusion should reject this at parse time.
    let pred = c_str("not a real predicate ((((");
    let rc = unsafe { lance_dataset_delete(ds, pred.as_ptr(), ptr::null_mut()) };
    assert_eq!(rc, -1);
    // Parser errors come back as Internal (this is what upstream surfaces;
    // we don't try to re-classify them at the FFI boundary). If upstream
    // ever tightens this to InvalidArgument, tighten this assertion too.
    assert_eq!(lance_last_error_code(), LanceErrorCode::Internal);
    // The dataset is left untouched on the error path.
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_delete_unknown_column_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let pred = c_str("no_such_column = 1");
    let rc = unsafe { lance_dataset_delete(ds, pred.as_ptr(), ptr::null_mut()) };
    assert_eq!(rc, -1);
    // Same upstream classification as malformed SQL — see note above.
    assert_eq!(lance_last_error_code(), LanceErrorCode::Internal);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);

    unsafe { lance_dataset_close(ds) };
}

// ===========================================================================
// lance_dataset_update
// ===========================================================================

/// Build a `[*const c_char; N]` ptr array from a slice of `&CString`.
fn cstr_ptrs(items: &[CString]) -> Vec<*const c_char> {
    items.iter().map(|s| s.as_ptr()).collect()
}

#[test]
fn test_update_basic_predicate() {
    let (_tmp, uri) = create_large_dataset(100);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let pred = c_str("id < 50");
    let cols = [c_str("value")];
    let vals = [c_str("99.0")];
    let col_ptrs = cstr_ptrs(&cols);
    let val_ptrs = cstr_ptrs(&vals);

    let mut num_updated: u64 = 0;
    let rc = unsafe {
        lance_dataset_update(
            ds,
            pred.as_ptr(),
            col_ptrs.as_ptr(),
            val_ptrs.as_ptr(),
            1,
            &mut num_updated,
        )
    };
    assert_eq!(rc, 0);
    assert_eq!(num_updated, 50);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 100);

    // Verify the matched rows now read back as 99.0 and the rest are unchanged.
    let batches = scan_all_rows(ds);
    let mut updated_count = 0;
    let mut unchanged_count = 0;
    for batch in &batches {
        let ids = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        let values = batch
            .column_by_name("value")
            .unwrap()
            .as_any()
            .downcast_ref::<Float32Array>()
            .unwrap();
        for i in 0..batch.num_rows() {
            let id = ids.value(i);
            let v = values.value(i);
            if id < 50 {
                assert_eq!(v, 99.0, "id={id} should have been updated to 99.0");
                updated_count += 1;
            } else {
                assert_eq!(v, id as f32 * 0.5, "id={id} should be unchanged");
                unchanged_count += 1;
            }
        }
    }
    assert_eq!(updated_count, 50);
    assert_eq!(unchanged_count, 50);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_update_null_predicate_updates_all() {
    let (_tmp, uri) = create_large_dataset(20);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let cols = [c_str("label")];
    let vals = [c_str("'frozen'")];
    let col_ptrs = cstr_ptrs(&cols);
    let val_ptrs = cstr_ptrs(&vals);

    // NULL predicate → update every row.
    let mut num_updated: u64 = 0;
    let rc = unsafe {
        lance_dataset_update(
            ds,
            ptr::null(),
            col_ptrs.as_ptr(),
            val_ptrs.as_ptr(),
            1,
            &mut num_updated,
        )
    };
    assert_eq!(rc, 0);
    assert_eq!(num_updated, 20);

    let batches = scan_all_rows(ds);
    for batch in &batches {
        let labels = batch
            .column_by_name("label")
            .unwrap()
            .as_any()
            .downcast_ref::<StringArray>()
            .unwrap();
        for i in 0..batch.num_rows() {
            assert_eq!(labels.value(i), "frozen");
        }
    }

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_update_multiple_columns() {
    let (_tmp, uri) = create_large_dataset(10);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let pred = c_str("id = 7");
    let cols = [c_str("value"), c_str("label")];
    let vals = [c_str("value * 2"), c_str("'updated'")];
    let col_ptrs = cstr_ptrs(&cols);
    let val_ptrs = cstr_ptrs(&vals);

    let mut num_updated: u64 = 0;
    let rc = unsafe {
        lance_dataset_update(
            ds,
            pred.as_ptr(),
            col_ptrs.as_ptr(),
            val_ptrs.as_ptr(),
            2,
            &mut num_updated,
        )
    };
    assert_eq!(rc, 0);
    assert_eq!(num_updated, 1);

    // Row 7 originally had value = 3.5, label = "row_7".
    // After update: value = 7.0, label = "updated". Other rows unchanged.
    let batches = scan_all_rows(ds);
    for batch in &batches {
        let ids = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        let values = batch
            .column_by_name("value")
            .unwrap()
            .as_any()
            .downcast_ref::<Float32Array>()
            .unwrap();
        let labels = batch
            .column_by_name("label")
            .unwrap()
            .as_any()
            .downcast_ref::<StringArray>()
            .unwrap();
        for i in 0..batch.num_rows() {
            let id = ids.value(i);
            if id == 7 {
                assert_eq!(values.value(i), 7.0);
                assert_eq!(labels.value(i), "updated");
            } else {
                assert_eq!(values.value(i), id as f32 * 0.5);
                assert_eq!(labels.value(i), format!("row_{id}"));
            }
        }
    }

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_update_no_match_returns_zero() {
    let (_tmp, uri) = create_large_dataset(10);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let pred = c_str("id > 9999");
    let cols = [c_str("value")];
    let vals = [c_str("0.0")];
    let col_ptrs = cstr_ptrs(&cols);
    let val_ptrs = cstr_ptrs(&vals);

    let mut num_updated: u64 = 12345;
    let rc = unsafe {
        lance_dataset_update(
            ds,
            pred.as_ptr(),
            col_ptrs.as_ptr(),
            val_ptrs.as_ptr(),
            1,
            &mut num_updated,
        )
    };
    assert_eq!(rc, 0);
    assert_eq!(num_updated, 0);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 10);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_update_out_param_optional() {
    let (_tmp, uri) = create_large_dataset(5);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let pred = c_str("id < 3");
    let cols = [c_str("value")];
    let vals = [c_str("42.0")];
    let col_ptrs = cstr_ptrs(&cols);
    let val_ptrs = cstr_ptrs(&vals);

    // Pass NULL out_num_updated — must succeed without writing anything.
    let rc = unsafe {
        lance_dataset_update(
            ds,
            pred.as_ptr(),
            col_ptrs.as_ptr(),
            val_ptrs.as_ptr(),
            1,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_update_bumps_version() {
    let (_tmp, uri) = create_large_dataset(5);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let v_before = unsafe { lance_dataset_version(ds) };
    let pred = c_str("id = 0");
    let cols = [c_str("value")];
    let vals = [c_str("123.0")];
    let col_ptrs = cstr_ptrs(&cols);
    let val_ptrs = cstr_ptrs(&vals);
    let rc = unsafe {
        lance_dataset_update(
            ds,
            pred.as_ptr(),
            col_ptrs.as_ptr(),
            val_ptrs.as_ptr(),
            1,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);
    let v_after = unsafe { lance_dataset_version(ds) };
    assert!(
        v_after > v_before,
        "version should increase: before={v_before}, after={v_after}"
    );

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_update_null_dataset_rejected() {
    let cols = [c_str("value")];
    let vals = [c_str("0.0")];
    let col_ptrs = cstr_ptrs(&cols);
    let val_ptrs = cstr_ptrs(&vals);
    let rc = unsafe {
        lance_dataset_update(
            ptr::null_mut(),
            ptr::null(),
            col_ptrs.as_ptr(),
            val_ptrs.as_ptr(),
            1,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

#[test]
fn test_update_zero_num_updates_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let rc = unsafe {
        lance_dataset_update(
            ds,
            ptr::null(),
            ptr::null(),
            ptr::null(),
            0,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_update_null_columns_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let vals = [c_str("0.0")];
    let val_ptrs = cstr_ptrs(&vals);
    let rc = unsafe {
        lance_dataset_update(
            ds,
            ptr::null(),
            ptr::null(),
            val_ptrs.as_ptr(),
            1,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_update_null_values_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let cols = [c_str("value")];
    let col_ptrs = cstr_ptrs(&cols);
    let rc = unsafe {
        lance_dataset_update(
            ds,
            ptr::null(),
            col_ptrs.as_ptr(),
            ptr::null(),
            1,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_update_empty_predicate_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let pred = c_str("");
    let cols = [c_str("value")];
    let vals = [c_str("0.0")];
    let col_ptrs = cstr_ptrs(&cols);
    let val_ptrs = cstr_ptrs(&vals);
    let rc = unsafe {
        lance_dataset_update(
            ds,
            pred.as_ptr(),
            col_ptrs.as_ptr(),
            val_ptrs.as_ptr(),
            1,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_update_empty_column_entry_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let cols = [c_str("")];
    let vals = [c_str("0.0")];
    let col_ptrs = cstr_ptrs(&cols);
    let val_ptrs = cstr_ptrs(&vals);
    let rc = unsafe {
        lance_dataset_update(
            ds,
            ptr::null(),
            col_ptrs.as_ptr(),
            val_ptrs.as_ptr(),
            1,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_update_null_entry_in_columns_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    // Build an array where the first column pointer is NULL.
    let val_a = c_str("0.0");
    let col_ptrs: [*const c_char; 1] = [ptr::null()];
    let val_ptrs: [*const c_char; 1] = [val_a.as_ptr()];
    let rc = unsafe {
        lance_dataset_update(
            ds,
            ptr::null(),
            col_ptrs.as_ptr(),
            val_ptrs.as_ptr(),
            1,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_update_invalid_predicate_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    // Garbage SQL — UpdateBuilder::update_where wraps parser errors as
    // InvalidInput, so this surfaces as InvalidArgument (different from
    // lance_dataset_delete, which routes through a different upstream path
    // and surfaces these as Internal).
    let pred = c_str("not a real predicate ((((");
    let cols = [c_str("value")];
    let vals = [c_str("0.0")];
    let col_ptrs = cstr_ptrs(&cols);
    let val_ptrs = cstr_ptrs(&vals);
    let rc = unsafe {
        lance_dataset_update(
            ds,
            pred.as_ptr(),
            col_ptrs.as_ptr(),
            val_ptrs.as_ptr(),
            1,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_update_unknown_column_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let cols = [c_str("no_such_column")];
    let vals = [c_str("0.0")];
    let col_ptrs = cstr_ptrs(&cols);
    let val_ptrs = cstr_ptrs(&vals);
    let rc = unsafe {
        lance_dataset_update(
            ds,
            ptr::null(),
            col_ptrs.as_ptr(),
            val_ptrs.as_ptr(),
            1,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    // UpdateBuilder::set returns InvalidInput for unknown columns.
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);

    unsafe { lance_dataset_close(ds) };
}

// Predicate-side unknown column goes through `UpdateBuilder::update_where`
// (a different upstream path from `set`), so pin it separately.
#[test]
fn test_update_unknown_predicate_column_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let pred = c_str("no_such_column = 1");
    let cols = [c_str("value")];
    let vals = [c_str("0.0")];
    let col_ptrs = cstr_ptrs(&cols);
    let val_ptrs = cstr_ptrs(&vals);
    let rc = unsafe {
        lance_dataset_update(
            ds,
            pred.as_ptr(),
            col_ptrs.as_ptr(),
            val_ptrs.as_ptr(),
            1,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);

    unsafe { lance_dataset_close(ds) };
}

// Locks in the documented contract: when the call fails, `out_num_updated`
// must be left unchanged. A future refactor that pre-zeroes the slot before
// validating inputs would silently break this guarantee.
#[test]
fn test_update_out_param_untouched_on_error() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let mut sentinel: u64 = 0xDEAD_BEEF;

    // Empty predicate → INVALID_ARGUMENT before any work happens (boundary).
    let pred = c_str("");
    let cols = [c_str("value")];
    let vals = [c_str("0.0")];
    let col_ptrs = cstr_ptrs(&cols);
    let val_ptrs = cstr_ptrs(&vals);
    let rc = unsafe {
        lance_dataset_update(
            ds,
            pred.as_ptr(),
            col_ptrs.as_ptr(),
            val_ptrs.as_ptr(),
            1,
            &mut sentinel,
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(sentinel, 0xDEAD_BEEF, "out slot must be untouched on error");

    // Same property must hold for upstream-surfaced errors (unknown column).
    let bad_cols = [c_str("no_such_column")];
    let bad_col_ptrs = cstr_ptrs(&bad_cols);
    let rc = unsafe {
        lance_dataset_update(
            ds,
            ptr::null(),
            bad_col_ptrs.as_ptr(),
            val_ptrs.as_ptr(),
            1,
            &mut sentinel,
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(sentinel, 0xDEAD_BEEF, "out slot must be untouched on error");

    unsafe { lance_dataset_close(ds) };
}

// ===========================================================================
// lance_dataset_merge_insert
// ===========================================================================

/// Build a {id, value, label} batch matching `create_large_dataset`'s schema.
fn make_merge_source(rows: &[(i32, f32, &str)]) -> RecordBatch {
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("value", DataType::Float32, true),
        Field::new("label", DataType::Utf8, true),
    ]));
    let ids: Vec<i32> = rows.iter().map(|r| r.0).collect();
    let values: Vec<f32> = rows.iter().map(|r| r.1).collect();
    let labels: Vec<&str> = rows.iter().map(|r| r.2).collect();
    RecordBatch::try_new(
        schema,
        vec![
            Arc::new(Int32Array::from(ids)),
            Arc::new(Float32Array::from(values)),
            Arc::new(StringArray::from(labels)),
        ],
    )
    .unwrap()
}

/// Build a `LanceMergeInsertParams` zero-initialized except for the supplied
/// fields. Helps keep tests readable when only a couple of knobs differ from
/// the find-or-create defaults.
fn merge_params(
    when_matched: LanceMergeWhenMatched,
    when_not_matched: LanceMergeWhenNotMatched,
    when_not_matched_by_source: LanceMergeWhenNotMatchedBySource,
) -> LanceMergeInsertParams {
    LanceMergeInsertParams {
        when_matched: when_matched as i32,
        when_matched_expr: ptr::null(),
        when_not_matched: when_not_matched as i32,
        when_not_matched_by_source: when_not_matched_by_source as i32,
        when_not_matched_by_source_expr: ptr::null(),
    }
}

#[test]
fn test_merge_insert_default_is_find_or_create() {
    // Default params (`params=NULL`) should match upstream's find-or-create:
    // existing keys are kept untouched; missing keys are inserted.
    let (_tmp, uri) = create_large_dataset(10);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let source = make_merge_source(&[(5, 999.0, "rewritten"), (200, 12.5, "new_row")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);

    let mut result = LanceMergeInsertResult::default();
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            ptr::null(),
            &mut result,
        )
    };
    assert_eq!(rc, 0);
    assert_eq!(result.num_inserted_rows, 1);
    assert_eq!(result.num_updated_rows, 0);
    assert_eq!(result.num_deleted_rows, 0);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 11);

    // id=5 must remain unchanged (DoNothing on match).
    let batches = scan_all_rows(ds);
    let mut row5_value = None;
    for batch in &batches {
        let ids = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        let values = batch
            .column_by_name("value")
            .unwrap()
            .as_any()
            .downcast_ref::<Float32Array>()
            .unwrap();
        for i in 0..batch.num_rows() {
            if ids.value(i) == 5 {
                row5_value = Some(values.value(i));
            }
        }
    }
    assert_eq!(
        row5_value,
        Some(2.5),
        "id=5 should be unchanged on DoNothing"
    );

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_upsert_updates_and_inserts() {
    let (_tmp, uri) = create_large_dataset(10);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let source = make_merge_source(&[(5, 999.0, "rewritten"), (200, 12.5, "new_row")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);

    let params = merge_params(
        LanceMergeWhenMatched::UpdateAll,
        LanceMergeWhenNotMatched::InsertAll,
        LanceMergeWhenNotMatchedBySource::Keep,
    );
    let mut result = LanceMergeInsertResult::default();
    let rc = unsafe {
        lance_dataset_merge_insert(ds, on_ptrs.as_ptr(), 1, &mut stream, &params, &mut result)
    };
    assert_eq!(rc, 0);
    assert_eq!(result.num_inserted_rows, 1);
    assert_eq!(result.num_updated_rows, 1);
    assert_eq!(result.num_deleted_rows, 0);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 11);

    // id=5 should now read 999.0 / "rewritten"; id=200 should appear with
    // the source values; everything else stays as the original generator
    // produced (`row_<id>`, value = id * 0.5).
    let batches = scan_all_rows(ds);
    let mut seen_5 = false;
    let mut seen_200 = false;
    for batch in &batches {
        let ids = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        let values = batch
            .column_by_name("value")
            .unwrap()
            .as_any()
            .downcast_ref::<Float32Array>()
            .unwrap();
        let labels = batch
            .column_by_name("label")
            .unwrap()
            .as_any()
            .downcast_ref::<StringArray>()
            .unwrap();
        for i in 0..batch.num_rows() {
            match ids.value(i) {
                5 => {
                    assert_eq!(values.value(i), 999.0);
                    assert_eq!(labels.value(i), "rewritten");
                    seen_5 = true;
                }
                200 => {
                    assert_eq!(values.value(i), 12.5);
                    assert_eq!(labels.value(i), "new_row");
                    seen_200 = true;
                }
                id => {
                    assert_eq!(values.value(i), id as f32 * 0.5);
                    assert_eq!(labels.value(i), format!("row_{id}"));
                }
            }
        }
    }
    assert!(seen_5 && seen_200);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_when_matched_fail_errors_on_match() {
    let (_tmp, uri) = create_large_dataset(10);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let source = make_merge_source(&[(5, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);

    let params = merge_params(
        LanceMergeWhenMatched::Fail,
        LanceMergeWhenNotMatched::InsertAll,
        LanceMergeWhenNotMatchedBySource::Keep,
    );
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    // Dataset is left unchanged on the error path.
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 10);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_when_matched_delete_drops_match() {
    let (_tmp, uri) = create_large_dataset(10);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    // Source has matching id=5 and non-matching id=200. With Delete+DoNothing
    // the matching row is removed, the non-matching row is dropped.
    let source = make_merge_source(&[(5, 0.0, "x"), (200, 0.0, "y")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);

    let params = merge_params(
        LanceMergeWhenMatched::Delete,
        LanceMergeWhenNotMatched::DoNothing,
        LanceMergeWhenNotMatchedBySource::Keep,
    );
    let mut result = LanceMergeInsertResult::default();
    let rc = unsafe {
        lance_dataset_merge_insert(ds, on_ptrs.as_ptr(), 1, &mut stream, &params, &mut result)
    };
    assert_eq!(rc, 0);
    assert_eq!(result.num_inserted_rows, 0);
    assert_eq!(result.num_deleted_rows, 1);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 9);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_update_if_filters_matches() {
    // UpdateIf only updates matched rows where the filter holds. The source
    // matches both id=2 and id=8; the filter `target.value > 3` selects only
    // id=8 (target value 4.0) — id=2's target value 1.0 stays put.
    let (_tmp, uri) = create_large_dataset(10);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let source = make_merge_source(&[(2, 100.0, "x"), (8, 100.0, "y")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);

    let expr = c_str("target.value > 3");
    let params = LanceMergeInsertParams {
        when_matched: LanceMergeWhenMatched::UpdateIf as i32,
        when_matched_expr: expr.as_ptr(),
        when_not_matched: LanceMergeWhenNotMatched::DoNothing as i32,
        when_not_matched_by_source: LanceMergeWhenNotMatchedBySource::Keep as i32,
        when_not_matched_by_source_expr: ptr::null(),
    };
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);

    let batches = scan_all_rows(ds);
    let mut row2_value = None;
    let mut row8_value = None;
    for batch in &batches {
        let ids = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        let values = batch
            .column_by_name("value")
            .unwrap()
            .as_any()
            .downcast_ref::<Float32Array>()
            .unwrap();
        for i in 0..batch.num_rows() {
            match ids.value(i) {
                2 => row2_value = Some(values.value(i)),
                8 => row8_value = Some(values.value(i)),
                _ => {}
            }
        }
    }
    assert_eq!(
        row2_value,
        Some(1.0),
        "id=2 should be unchanged (filter false)"
    );
    assert_eq!(
        row8_value,
        Some(100.0),
        "id=8 should be updated (filter true)"
    );

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_when_not_matched_do_nothing_skips_inserts() {
    let (_tmp, uri) = create_large_dataset(5);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let source = make_merge_source(&[(100, 0.0, "x"), (200, 0.0, "y")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);

    let params = merge_params(
        LanceMergeWhenMatched::UpdateAll,
        LanceMergeWhenNotMatched::DoNothing,
        LanceMergeWhenNotMatchedBySource::Keep,
    );
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);
    // Source rows did not match anything; with DoNothing they are discarded.
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 5);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_when_not_matched_by_source_delete() {
    // Replace-everything-not-in-source semantics: target rows whose key does
    // not appear in the source are dropped.
    let (_tmp, uri) = create_large_dataset(5);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let source = make_merge_source(&[(2, 0.0, "x"), (3, 0.0, "y")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);

    let params = merge_params(
        LanceMergeWhenMatched::DoNothing,
        LanceMergeWhenNotMatched::DoNothing,
        LanceMergeWhenNotMatchedBySource::Delete,
    );
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);
    // 5 -> 2 rows remain (ids 2 and 3).
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 2);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_when_not_matched_by_source_delete_if() {
    // DeleteIf("id < 3"): drop unmatched target rows that satisfy the filter
    // (ids 0, 1) and keep the rest (ids 3, 4). id=2 is matched by source so
    // it is preserved regardless of the filter.
    let (_tmp, uri) = create_large_dataset(5);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let source = make_merge_source(&[(2, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);

    let expr = c_str("id < 3");
    let params = LanceMergeInsertParams {
        when_matched: LanceMergeWhenMatched::DoNothing as i32,
        when_matched_expr: ptr::null(),
        when_not_matched: LanceMergeWhenNotMatched::DoNothing as i32,
        when_not_matched_by_source: LanceMergeWhenNotMatchedBySource::DeleteIf as i32,
        when_not_matched_by_source_expr: expr.as_ptr(),
    };
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);
    // id=0 and id=1 deleted; id=2,3,4 kept.
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_multi_column_keys() {
    // Match on (id, label). The source row matches id=3 but with a different
    // label, so no target row is matched and the source row is inserted as a
    // brand-new row under upsert semantics.
    let (_tmp, uri) = create_large_dataset(5);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let source = make_merge_source(&[(3, 99.0, "different")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id"), c_str("label")];
    let on_ptrs = cstr_ptrs(&on);

    let params = merge_params(
        LanceMergeWhenMatched::UpdateAll,
        LanceMergeWhenNotMatched::InsertAll,
        LanceMergeWhenNotMatchedBySource::Keep,
    );
    let mut result = LanceMergeInsertResult::default();
    let rc = unsafe {
        lance_dataset_merge_insert(ds, on_ptrs.as_ptr(), 2, &mut stream, &params, &mut result)
    };
    assert_eq!(rc, 0);
    assert_eq!(result.num_inserted_rows, 1);
    assert_eq!(result.num_updated_rows, 0);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 6);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_bumps_version() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let v_before = unsafe { lance_dataset_version(ds) };
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);
    let v_after = unsafe { lance_dataset_version(ds) };
    assert!(v_after > v_before);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_out_result_optional() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);
    // Pass NULL out_result — must succeed without writing anything.
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);

    unsafe { lance_dataset_close(ds) };
}

// Locks in the documented contract: when the call fails, `out_result` must be
// left unchanged. A future refactor that pre-zeroes the slot before validating
// inputs would silently break this guarantee.
#[test]
fn test_merge_insert_out_result_untouched_on_error() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let sentinel = LanceMergeInsertResult {
        num_inserted_rows: 0xDEAD,
        num_updated_rows: 0xBEEF,
        num_deleted_rows: 0xCAFE,
    };
    let mut out = sentinel;

    // num_on_columns = 0 → INVALID_ARGUMENT before any work happens. The
    // stream is still consumed (NULL stream is the only check ahead of the
    // `from_raw` consume), but the result slot must be untouched.
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let rc = unsafe {
        lance_dataset_merge_insert(ds, ptr::null(), 0, &mut stream, ptr::null(), &mut out)
    };
    assert_eq!(rc, -1);
    assert_eq!(out, sentinel, "out slot must be untouched on error");

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_null_dataset_rejected() {
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);
    let rc = unsafe {
        lance_dataset_merge_insert(
            ptr::null_mut(),
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

#[test]
fn test_merge_insert_null_source_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            ptr::null_mut(),
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_zero_num_on_columns_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            ptr::null(),
            0,
            &mut stream,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_null_on_columns_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            ptr::null(),
            1,
            &mut stream,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_empty_key_entry_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("")];
    let on_ptrs = cstr_ptrs(&on);
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_null_entry_in_on_columns_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on_ptrs: [*const c_char; 1] = [ptr::null()];
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_unknown_key_column_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("no_such_column")];
    let on_ptrs = cstr_ptrs(&on);
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            ptr::null(),
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    // MergeInsertBuilder::try_new returns InvalidInput for an unknown key.
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_invalid_when_matched_discriminant_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);
    let params = LanceMergeInsertParams {
        when_matched: 99,
        when_matched_expr: ptr::null(),
        when_not_matched: 0,
        when_not_matched_by_source: 0,
        when_not_matched_by_source_expr: ptr::null(),
    };
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_invalid_when_not_matched_discriminant_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);
    let params = LanceMergeInsertParams {
        when_matched: 0,
        when_matched_expr: ptr::null(),
        when_not_matched: 99,
        when_not_matched_by_source: 0,
        when_not_matched_by_source_expr: ptr::null(),
    };
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_invalid_when_not_matched_by_source_discriminant_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);
    let params = LanceMergeInsertParams {
        when_matched: 0,
        when_matched_expr: ptr::null(),
        when_not_matched: 0,
        when_not_matched_by_source: 99,
        when_not_matched_by_source_expr: ptr::null(),
    };
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_empty_expr_rejected() {
    // Empty expression string is rejected at the FFI boundary so callers hit
    // a precise error rather than an opaque parser failure later on.
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);
    let empty = c_str("");
    let params = LanceMergeInsertParams {
        when_matched: LanceMergeWhenMatched::UpdateIf as i32,
        when_matched_expr: empty.as_ptr(),
        when_not_matched: 0,
        when_not_matched_by_source: 0,
        when_not_matched_by_source_expr: ptr::null(),
    };
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_update_if_missing_expr_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);
    let params = LanceMergeInsertParams {
        when_matched: LanceMergeWhenMatched::UpdateIf as i32,
        when_matched_expr: ptr::null(),
        when_not_matched: 0,
        when_not_matched_by_source: 0,
        when_not_matched_by_source_expr: ptr::null(),
    };
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_unused_expr_for_update_all_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);
    let expr = c_str("id > 0");
    let params = LanceMergeInsertParams {
        when_matched: LanceMergeWhenMatched::UpdateAll as i32,
        when_matched_expr: expr.as_ptr(),
        when_not_matched: 0,
        when_not_matched_by_source: 0,
        when_not_matched_by_source_expr: ptr::null(),
    };
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_unused_expr_for_keep_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);
    let expr = c_str("id > 0");
    let params = LanceMergeInsertParams {
        when_matched: 0,
        when_matched_expr: ptr::null(),
        when_not_matched: 0,
        when_not_matched_by_source: LanceMergeWhenNotMatchedBySource::Keep as i32,
        when_not_matched_by_source_expr: expr.as_ptr(),
    };
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_delete_if_missing_expr_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);
    let params = LanceMergeInsertParams {
        when_matched: 0,
        when_matched_expr: ptr::null(),
        when_not_matched: 0,
        when_not_matched_by_source: LanceMergeWhenNotMatchedBySource::DeleteIf as i32,
        when_not_matched_by_source_expr: ptr::null(),
    };
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_no_op_config_rejected() {
    // DoNothing + DoNothing + Keep is a configuration that mutates nothing;
    // upstream's `try_build` rejects it as InvalidInput.
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let source = make_merge_source(&[(100, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);
    let params = merge_params(
        LanceMergeWhenMatched::DoNothing,
        LanceMergeWhenNotMatched::DoNothing,
        LanceMergeWhenNotMatchedBySource::Keep,
    );
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_schema_mismatch_rejected() {
    // Source `value` column is Float64 instead of Float32, so upstream's
    // schema-compatibility check rejects the merge before any commit lands.
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let bad_schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("value", DataType::Float64, true),
    ]));
    let bad_batch = RecordBatch::try_new(
        bad_schema.clone(),
        vec![
            Arc::new(Int32Array::from(vec![100])),
            Arc::new(arrow_array::Float64Array::from(vec![1.0])),
        ],
    )
    .unwrap();
    let reader = arrow::record_batch::RecordBatchIterator::new(vec![Ok(bad_batch)], bad_schema);
    let mut stream = FFI_ArrowArrayStream::new(Box::new(reader));

    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);
    let params = merge_params(
        LanceMergeWhenMatched::UpdateAll,
        LanceMergeWhenNotMatched::InsertAll,
        LanceMergeWhenNotMatchedBySource::Keep,
    );
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    // The dataset should not be corrupted by the rejected merge.
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_merge_insert_unknown_predicate_column_in_delete_if_rejected() {
    // DeleteIf parses against the dataset schema at FFI time; an unknown
    // column surfaces as InvalidArgument.
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    let source = make_merge_source(&[(2, 0.0, "x")]);
    let mut stream = batch_to_ffi_stream(source);
    let on = [c_str("id")];
    let on_ptrs = cstr_ptrs(&on);

    let expr = c_str("no_such_column = 1");
    let params = LanceMergeInsertParams {
        when_matched: 0,
        when_matched_expr: ptr::null(),
        when_not_matched: 0,
        when_not_matched_by_source: LanceMergeWhenNotMatchedBySource::DeleteIf as i32,
        when_not_matched_by_source_expr: expr.as_ptr(),
    };
    let rc = unsafe {
        lance_dataset_merge_insert(
            ds,
            on_ptrs.as_ptr(),
            1,
            &mut stream,
            &params,
            ptr::null_mut(),
        )
    };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 3);
    unsafe { lance_dataset_close(ds) };
}

// ===========================================================================
// lance_dataset_compact_files
// ===========================================================================

/// Build a dataset of `num_fragments` small fragments (3 unique ids each)
/// so the default planner sees plenty of small neighbors to merge. Unique
/// ids keep fragments alive across partial-row deletes — upstream's `delete`
/// drops a fragment entirely once all of its rows are gone.
fn create_many_small_fragments(num_fragments: i32) -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("small_frags").to_str().unwrap().to_string();
    let schema = Arc::new(Schema::new(vec![Field::new("id", DataType::Int32, false)]));

    lance_c::runtime::block_on(async {
        for i in 0..num_fragments {
            let base = i * 3;
            let batch = RecordBatch::try_new(
                schema.clone(),
                vec![Arc::new(Int32Array::from(vec![base, base + 1, base + 2]))],
            )
            .unwrap();
            if i == 0 {
                Dataset::write(
                    arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema.clone()),
                    &uri,
                    None,
                )
                .await
                .unwrap();
            } else {
                let mut ds = Dataset::open(&uri).await.unwrap();
                ds.append(
                    arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema.clone()),
                    None,
                )
                .await
                .unwrap();
            }
        }
    });

    (tmp, uri)
}

#[test]
fn test_compact_basic_merges_small_fragments() {
    let (_tmp, uri) = create_many_small_fragments(4);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    assert_eq!(unsafe { lance_dataset_fragment_count(ds) }, 4);
    let v_before = unsafe { lance_dataset_version(ds) };

    let mut metrics = LanceCompactionMetrics::default();
    let rc = unsafe { lance_dataset_compact_files(ds, ptr::null(), &mut metrics) };
    assert_eq!(rc, 0);

    // All four small neighbors are below the default 1Mi target, so they
    // collapse into a single output fragment. Row count is preserved.
    assert_eq!(metrics.fragments_removed, 4);
    assert_eq!(metrics.fragments_added, 1);
    assert_eq!(unsafe { lance_dataset_fragment_count(ds) }, 1);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 12);
    assert!(unsafe { lance_dataset_version(ds) } > v_before);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_compact_preserves_data() {
    let (_tmp, uri) = create_many_small_fragments(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let rc = unsafe { lance_dataset_compact_files(ds, ptr::null(), ptr::null_mut()) };
    assert_eq!(rc, 0);

    // Three fragments × three unique ids each = 0..=8, every value once.
    let mut seen = [false; 9];
    for batch in &scan_all_rows(ds) {
        let ids = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        for i in 0..batch.num_rows() {
            seen[ids.value(i) as usize] = true;
        }
    }
    assert!(seen.iter().all(|&b| b));

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_compact_no_op_on_clean_single_fragment() {
    // A single fragment with no neighbors and no deletions has nothing to
    // compact: upstream returns success without committing, so the version
    // and counts are unchanged.
    let (_tmp, uri) = create_test_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let v_before = unsafe { lance_dataset_version(ds) };
    let mut metrics = LanceCompactionMetrics {
        fragments_removed: 0xDEAD,
        fragments_added: 0xBEEF,
        files_removed: 0xCAFE,
        files_added: 0xF00D,
    };
    let rc = unsafe { lance_dataset_compact_files(ds, ptr::null(), &mut metrics) };
    assert_eq!(rc, 0);
    assert_eq!(metrics, LanceCompactionMetrics::default());
    assert_eq!(unsafe { lance_dataset_version(ds) }, v_before);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_compact_after_deletes_materializes_deletion_files() {
    let (_tmp, uri) = create_many_small_fragments(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    // Soft-delete one row from each fragment (id=1 from frag 0, id=4 from
    // frag 1) so each fragment ends up with a deletion file to materialize.
    let pred = c_str("id = 1 OR id = 4");
    let rc = unsafe { lance_dataset_delete(ds, pred.as_ptr(), ptr::null_mut()) };
    assert_eq!(rc, 0);
    assert_eq!(unsafe { lance_dataset_fragment_count(ds) }, 2);

    let mut metrics = LanceCompactionMetrics::default();
    let rc = unsafe { lance_dataset_compact_files(ds, ptr::null(), &mut metrics) };
    assert_eq!(rc, 0);
    // Both small fragments are rewritten into a single one with the deleted
    // rows physically removed and the deletion files gone.
    assert_eq!(metrics.fragments_removed, 2);
    assert_eq!(metrics.fragments_added, 1);
    assert!(
        metrics.files_removed >= 4,
        "expected ≥ 2 data files + 2 deletion files removed, got {}",
        metrics.files_removed
    );
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 4);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_compact_target_rows_per_fragment_override_accepted() {
    let (_tmp, uri) = create_many_small_fragments(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let opts = LanceCompactionOptions {
        target_rows_per_fragment: 100,
        max_rows_per_group: 0,
        max_bytes_per_file: 0,
        num_threads: 0,
        batch_size: 0,
    };
    let rc = unsafe { lance_dataset_compact_files(ds, &opts, ptr::null_mut()) };
    assert_eq!(rc, 0);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 9);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_compact_num_threads_override_accepted() {
    let (_tmp, uri) = create_many_small_fragments(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let opts = LanceCompactionOptions {
        target_rows_per_fragment: 0,
        max_rows_per_group: 0,
        max_bytes_per_file: 0,
        num_threads: 1,
        batch_size: 0,
    };
    let rc = unsafe { lance_dataset_compact_files(ds, &opts, ptr::null_mut()) };
    assert_eq!(rc, 0);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 9);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_compact_max_bytes_per_file_override_accepted() {
    // 1 MiB cap is far above what 6 rows of i32 produce, so the override
    // just smoke-checks that the field flows through without erroring.
    let (_tmp, uri) = create_many_small_fragments(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let opts = LanceCompactionOptions {
        target_rows_per_fragment: 0,
        max_rows_per_group: 0,
        max_bytes_per_file: 1 << 20,
        num_threads: 0,
        batch_size: 0,
    };
    let rc = unsafe { lance_dataset_compact_files(ds, &opts, ptr::null_mut()) };
    assert_eq!(rc, 0);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 6);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_compact_batch_size_and_max_rows_per_group_overrides_accepted() {
    // Smoke-checks that the two least-observable overrides flow through
    // without erroring; the resulting layout isn't introspected here.
    let (_tmp, uri) = create_many_small_fragments(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let opts = LanceCompactionOptions {
        target_rows_per_fragment: 0,
        max_rows_per_group: 4,
        max_bytes_per_file: 0,
        num_threads: 0,
        batch_size: 32,
    };
    let rc = unsafe { lance_dataset_compact_files(ds, &opts, ptr::null_mut()) };
    assert_eq!(rc, 0);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 6);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_compact_zero_options_equivalent_to_null() {
    // A zero-initialized options struct must behave identically to NULL.
    let (_tmp, uri) = create_many_small_fragments(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };

    let opts = LanceCompactionOptions {
        target_rows_per_fragment: 0,
        max_rows_per_group: 0,
        max_bytes_per_file: 0,
        num_threads: 0,
        batch_size: 0,
    };
    let mut metrics = LanceCompactionMetrics::default();
    let rc = unsafe { lance_dataset_compact_files(ds, &opts, &mut metrics) };
    assert_eq!(rc, 0);
    assert_eq!(metrics.fragments_removed, 2);
    assert_eq!(metrics.fragments_added, 1);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 6);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_compact_out_metrics_optional() {
    let (_tmp, uri) = create_many_small_fragments(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    // Pass NULL out_metrics — must succeed without writing anything.
    let rc = unsafe { lance_dataset_compact_files(ds, ptr::null(), ptr::null_mut()) };
    assert_eq!(rc, 0);
    assert_eq!(unsafe { lance_dataset_fragment_count(ds) }, 1);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_compact_null_dataset_rejected() {
    let rc = unsafe { lance_dataset_compact_files(ptr::null_mut(), ptr::null(), ptr::null_mut()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

// Locks in the documented contract: when the call fails, `out_metrics` must
// be left unchanged. A future refactor that pre-zeroes the slot before
// validating inputs would silently break this guarantee.
#[test]
fn test_compact_out_metrics_untouched_on_error() {
    let sentinel = LanceCompactionMetrics {
        fragments_removed: 0xDEAD,
        fragments_added: 0xBEEF,
        files_removed: 0xCAFE,
        files_added: 0xF00D,
    };
    let mut out = sentinel;
    let rc = unsafe { lance_dataset_compact_files(ptr::null_mut(), ptr::null(), &mut out) };
    assert_eq!(rc, -1);
    assert_eq!(out, sentinel, "out slot must be untouched on error");
}

// ---------------------------------------------------------------------------
// Distributed vector search via index segments
// ---------------------------------------------------------------------------

#[test]
fn test_index_segment_count_and_list() {
    let (_tmp, uri) = create_vector_dataset(256, 16);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("embedding");
    let name = c_str("emb_idx");
    let params = LanceVectorIndexParams {
        index_type: LanceVectorIndexType::IvfFlat,
        metric: LanceMetricType::L2,
        num_partitions: 4,
        num_sub_vectors: 0,
        num_bits: 0,
        max_iterations: 0,
        hnsw_m: 0,
        hnsw_ef_construction: 0,
        sample_rate: 0,
    };
    unsafe {
        lance_dataset_create_vector_index(ds, column.as_ptr(), name.as_ptr(), &params, false)
    };

    // A single create_index call produces one logical index = one segment.
    let count = unsafe { lance_dataset_index_segment_count(ds, name.as_ptr()) };
    assert_eq!(count, 1);

    let mut bytes = vec![0u8; (count as usize) * 16];
    let mut written: u64 = 0;
    let rc = unsafe {
        lance_dataset_index_segments(
            ds,
            name.as_ptr(),
            bytes.as_mut_ptr(),
            count as usize,
            &mut written,
        )
    };
    assert_eq!(rc, 0);
    assert_eq!(written, count);
    // Sanity: not all zeros (a real UUID was written).
    assert!(
        bytes.iter().any(|b| *b != 0),
        "expected non-zero UUID bytes"
    );

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_index_segment_count_unknown_index() {
    let (_tmp, uri) = create_vector_dataset(8, 8);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let name = c_str("does_not_exist");
    let count = unsafe { lance_dataset_index_segment_count(ds, name.as_ptr()) };
    assert_eq!(count, 0);
    assert_eq!(lance_last_error_code(), LanceErrorCode::NotFound);
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_set_index_segments_with_listed_uuids() {
    // End-to-end: build IVF index, list its segment UUIDs, then run nearest
    // restricted to that single segment. Should return k results identical
    // to an unrestricted search since there's only one segment.
    let (_tmp, uri) = create_vector_dataset(256, 16);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("embedding");
    let name = c_str("idx");
    let params = LanceVectorIndexParams {
        index_type: LanceVectorIndexType::IvfFlat,
        metric: LanceMetricType::L2,
        num_partitions: 4,
        num_sub_vectors: 0,
        num_bits: 0,
        max_iterations: 0,
        hnsw_m: 0,
        hnsw_ef_construction: 0,
        sample_rate: 0,
    };
    unsafe {
        lance_dataset_create_vector_index(ds, column.as_ptr(), name.as_ptr(), &params, false)
    };

    let count = unsafe { lance_dataset_index_segment_count(ds, name.as_ptr()) };
    assert_eq!(count, 1);
    let mut uuids = vec![0u8; (count as usize) * 16];
    let rc = unsafe {
        lance_dataset_index_segments(
            ds,
            name.as_ptr(),
            uuids.as_mut_ptr(),
            count as usize,
            std::ptr::null_mut(),
        )
    };
    assert_eq!(rc, 0);

    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    let query: Vec<f32> = vec![0.5; 16];
    unsafe {
        lance_scanner_nearest(
            scanner,
            column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void,
            16,
            LanceDataType::Float32 as i32,
            5,
        );
        let rc = lance_scanner_set_index_segments(scanner, uuids.as_ptr(), count as usize);
        assert_eq!(rc, 0);
    }
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) },
        0
    );
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream as *mut _).unwrap() };
    let mut total = 0;
    for b in reader {
        total += b.unwrap().num_rows();
    }
    assert_eq!(total, 5, "expected k=5 results from the (single) segment");

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_set_index_segments_unknown_uuid() {
    let (_tmp, uri) = create_vector_dataset(64, 8);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let column = c_str("embedding");
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    let query: Vec<f32> = vec![0.5; 8];
    unsafe {
        lance_scanner_nearest(
            scanner,
            column.as_ptr(),
            query.as_ptr() as *const std::ffi::c_void,
            8,
            LanceDataType::Float32 as i32,
            5,
        )
    };
    // 16 bytes of all-zeros = a Uuid that does not match any real segment.
    let bogus = [0u8; 16];
    let rc = unsafe { lance_scanner_set_index_segments(scanner, bogus.as_ptr(), 1) };
    assert_eq!(
        rc, 0,
        "setter accepts any byte sequence; lookup happens at scan time"
    );

    let mut stream = FFI_ArrowArrayStream::empty();
    let scan_rc = unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream as *mut _) };
    assert_eq!(
        scan_rc, -1,
        "unknown segment UUID should error at materialize time"
    );
    let msg = unsafe {
        std::ffi::CStr::from_ptr(lance_last_error_message())
            .to_string_lossy()
            .into_owned()
    };
    assert!(
        msg.to_lowercase().contains("segment"),
        "msg should mention segment: {}",
        msg
    );

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_scanner_set_index_segments_null_safety() {
    // NULL scanner.
    let bytes = [0u8; 16];
    let rc = unsafe { lance_scanner_set_index_segments(ptr::null_mut(), bytes.as_ptr(), 1) };
    assert_eq!(rc, -1);

    // NULL pointer with non-zero len.
    let (_tmp, uri) = create_vector_dataset(8, 8);
    let uri_c = c_str(&uri);
    let ds = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let scanner = unsafe { lance_scanner_new(ds, ptr::null(), ptr::null()) };
    let rc2 = unsafe { lance_scanner_set_index_segments(scanner, ptr::null(), 1) };
    assert_eq!(rc2, -1);

    // NULL with len=0 clears the restriction → success.
    let rc3 = unsafe { lance_scanner_set_index_segments(scanner, ptr::null(), 0) };
    assert_eq!(rc3, 0);

    unsafe { lance_scanner_close(scanner) };
    unsafe { lance_dataset_close(ds) };
}

// ===========================================================================
// lance_dataset_drop_columns
// ===========================================================================

/// Return the dataset's column names by exporting its schema through the
/// Arrow C Data Interface.
fn schema_field_names(ds: *const LanceDataset) -> Vec<String> {
    debug_assert!(!ds.is_null(), "schema_field_names called with NULL dataset");
    let mut ffi_schema = FFI_ArrowSchema::empty();
    let rc = unsafe { lance_dataset_schema(ds, &mut ffi_schema) };
    assert_eq!(rc, 0, "lance_dataset_schema failed");
    let arrow_schema =
        arrow_schema::Schema::try_from(&ffi_schema).expect("FFI_ArrowSchema -> Schema");
    arrow_schema
        .fields()
        .iter()
        .map(|f| f.name().to_string())
        .collect()
}

#[test]
fn test_drop_columns_single() {
    let (_tmp, uri) = create_large_dataset(10);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let col = c_str("value");
    let cols = [col.as_ptr()];
    let rc = unsafe { lance_dataset_drop_columns(ds, cols.as_ptr(), cols.len()) };
    assert_eq!(rc, 0);

    // Row count is unchanged — drop is metadata-only.
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 10);
    // Schema reflects the dropped column.
    let names = schema_field_names(ds);
    assert_eq!(names, vec!["id".to_string(), "label".to_string()]);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_drop_columns_multiple() {
    let (_tmp, uri) = create_large_dataset(5);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let c_value = c_str("value");
    let c_label = c_str("label");
    let cols = [c_value.as_ptr(), c_label.as_ptr()];
    let rc = unsafe { lance_dataset_drop_columns(ds, cols.as_ptr(), cols.len()) };
    assert_eq!(rc, 0);

    let names = schema_field_names(ds);
    assert_eq!(names, vec!["id".to_string()]);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_drop_columns_bumps_version() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let v_before = unsafe { lance_dataset_version(ds) };
    let col = c_str("label");
    let cols = [col.as_ptr()];
    let rc = unsafe { lance_dataset_drop_columns(ds, cols.as_ptr(), cols.len()) };
    assert_eq!(rc, 0);
    let v_after = unsafe { lance_dataset_version(ds) };
    assert!(
        v_after > v_before,
        "version should increase: before={v_before}, after={v_after}"
    );

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_drop_columns_preserves_remaining_data() {
    let (_tmp, uri) = create_large_dataset(4);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let col = c_str("value");
    let cols = [col.as_ptr()];
    let rc = unsafe { lance_dataset_drop_columns(ds, cols.as_ptr(), cols.len()) };
    assert_eq!(rc, 0);

    // Materialize the remaining columns to confirm row data is intact.
    let indices: [u64; 4] = [0, 1, 2, 3];
    let mut stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe {
        lance_dataset_take(
            ds,
            indices.as_ptr(),
            indices.len(),
            ptr::null(),
            &mut stream,
        )
    };
    assert_eq!(rc, 0);

    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream) }.unwrap();
    let schema = reader.schema();
    let names: Vec<&str> = schema.fields().iter().map(|f| f.name().as_str()).collect();
    assert_eq!(names, vec!["id", "label"]);

    // Concatenate the resulting batches and assert the surviving column
    // values are unchanged — a regression that zeroed surviving data
    // would slip past a shape-only check.
    let batches: Vec<_> = reader.into_iter().collect::<Result<Vec<_>, _>>().unwrap();
    let total: usize = batches.iter().map(|b| b.num_rows()).sum();
    assert_eq!(total, 4);

    let mut ids: Vec<i32> = Vec::with_capacity(4);
    let mut labels: Vec<String> = Vec::with_capacity(4);
    for batch in &batches {
        let id_col = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        let label_col = batch
            .column_by_name("label")
            .unwrap()
            .as_any()
            .downcast_ref::<StringArray>()
            .unwrap();
        for i in 0..batch.num_rows() {
            ids.push(id_col.value(i));
            labels.push(label_col.value(i).to_string());
        }
    }
    assert_eq!(ids, vec![0, 1, 2, 3]);
    assert_eq!(labels, vec!["row_0", "row_1", "row_2", "row_3"]);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_drop_columns_unknown_column_rejected() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let col = c_str("no_such_column");
    let cols = [col.as_ptr()];
    let rc = unsafe { lance_dataset_drop_columns(ds, cols.as_ptr(), cols.len()) };
    assert_eq!(rc, -1);
    // Upstream surfaces this as InvalidInput → InvalidArgument at the FFI
    // boundary. The dataset is left untouched on the error path.
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let names = schema_field_names(ds);
    assert_eq!(names, vec!["id", "value", "label"]);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_drop_columns_cannot_drop_all() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let c_id = c_str("id");
    let c_value = c_str("value");
    let c_label = c_str("label");
    let cols = [c_id.as_ptr(), c_value.as_ptr(), c_label.as_ptr()];
    let rc = unsafe { lance_dataset_drop_columns(ds, cols.as_ptr(), cols.len()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    // Schema is unchanged.
    let names = schema_field_names(ds);
    assert_eq!(names, vec!["id", "value", "label"]);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_drop_columns_null_dataset_rejected() {
    let col = c_str("value");
    let cols = [col.as_ptr()];
    let rc = unsafe { lance_dataset_drop_columns(ptr::null_mut(), cols.as_ptr(), cols.len()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

#[test]
fn test_drop_columns_null_columns_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let rc = unsafe { lance_dataset_drop_columns(ds, ptr::null(), 1) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 2);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_drop_columns_zero_count_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let v_before = unsafe { lance_dataset_version(ds) };
    let dummy = c_str("value");
    let cols = [dummy.as_ptr()];
    let rc = unsafe { lance_dataset_drop_columns(ds, cols.as_ptr(), 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    // No version bump on the error path — compare against the pre-call value.
    assert_eq!(unsafe { lance_dataset_version(ds) }, v_before);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_drop_columns_null_entry_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let c_value = c_str("value");
    let cols: [*const c_char; 2] = [c_value.as_ptr(), ptr::null()];
    let rc = unsafe { lance_dataset_drop_columns(ds, cols.as_ptr(), cols.len()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    // Dataset is unchanged.
    let names = schema_field_names(ds);
    assert_eq!(names, vec!["id", "value", "label"]);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_drop_columns_empty_entry_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // Empty string is rejected by the `.filter(!empty)` guard at the FFI
    // boundary, distinct from the NULL-entry path.
    let empty = c_str("");
    let cols = [empty.as_ptr()];
    let rc = unsafe { lance_dataset_drop_columns(ds, cols.as_ptr(), cols.len()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let names = schema_field_names(ds);
    assert_eq!(names, vec!["id", "value", "label"]);

    unsafe { lance_dataset_close(ds) };
}

// ===========================================================================
// lance_dataset_alter_columns
// ===========================================================================

/// Build an FFI ArrowSchema describing a single field of the given Arrow
/// `DataType`. The returned struct owns its memory; the caller must keep it
/// alive for the duration of any `lance_dataset_alter_columns` call that
/// references it via `LanceColumnAlteration::data_type`. The placeholder
/// field name is never inspected by the alter path — only the `format`
/// string (i.e. the data type) is read out via `DataType::try_from`.
fn ffi_schema_for(data_type: DataType) -> FFI_ArrowSchema {
    let field = arrow_schema::Field::new("_", data_type, true);
    FFI_ArrowSchema::try_from(&field).expect("Field -> FFI_ArrowSchema")
}

/// Convenience: build a `LanceColumnAlteration` with all-default sentinels.
fn default_alteration(path: *const c_char) -> LanceColumnAlteration {
    LanceColumnAlteration {
        path,
        rename: ptr::null(),
        nullable_mode: LanceColumnNullableMode::Unchanged as i32,
        data_type: ptr::null(),
    }
}

#[test]
fn test_alter_columns_rename_only() {
    let (_tmp, uri) = create_large_dataset(4);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let path = c_str("label");
    let rename = c_str("tag");
    let alt = LanceColumnAlteration {
        path: path.as_ptr(),
        rename: rename.as_ptr(),
        nullable_mode: LanceColumnNullableMode::Unchanged as i32,
        data_type: ptr::null(),
    };
    let alts = [alt];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, 0);

    let names = schema_field_names(ds);
    assert_eq!(names, vec!["id", "value", "tag"]);
    // Metadata-only rename preserves row count.
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 4);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_relax_nullability() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // `id` is non-nullable in the fixture; relax it to nullable.
    let path = c_str("id");
    let alt = LanceColumnAlteration {
        path: path.as_ptr(),
        rename: ptr::null(),
        nullable_mode: LanceColumnNullableMode::True as i32,
        data_type: ptr::null(),
    };
    let alts = [alt];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, 0);

    let mut ffi_schema = FFI_ArrowSchema::empty();
    let rc = unsafe { lance_dataset_schema(ds, &mut ffi_schema) };
    assert_eq!(rc, 0);
    let arrow_schema = arrow_schema::Schema::try_from(&ffi_schema).unwrap();
    let id_field = arrow_schema.field_with_name("id").unwrap();
    assert!(id_field.is_nullable(), "id should be nullable after alter");

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_tighten_nullability_no_nulls() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // `label` is nullable in the fixture but no rows hold a NULL, so
    // tightening to non-nullable should succeed.
    let path = c_str("label");
    let alt = LanceColumnAlteration {
        path: path.as_ptr(),
        rename: ptr::null(),
        nullable_mode: LanceColumnNullableMode::False as i32,
        data_type: ptr::null(),
    };
    let alts = [alt];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, 0);

    let mut ffi_schema = FFI_ArrowSchema::empty();
    let rc = unsafe { lance_dataset_schema(ds, &mut ffi_schema) };
    assert_eq!(rc, 0);
    let arrow_schema = arrow_schema::Schema::try_from(&ffi_schema).unwrap();
    let label_field = arrow_schema.field_with_name("label").unwrap();
    assert!(
        !label_field.is_nullable(),
        "label should be non-nullable after tightening"
    );

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_type_change_int32_to_int64() {
    let (_tmp, uri) = create_large_dataset(4);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // `id` is Int32 in the fixture; upcast to Int64.
    let new_type = ffi_schema_for(DataType::Int64);
    let path = c_str("id");
    let alt = LanceColumnAlteration {
        path: path.as_ptr(),
        rename: ptr::null(),
        nullable_mode: LanceColumnNullableMode::Unchanged as i32,
        data_type: &new_type as *const _,
    };
    let alts = [alt];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, 0);

    let mut ffi_schema = FFI_ArrowSchema::empty();
    let rc = unsafe { lance_dataset_schema(ds, &mut ffi_schema) };
    assert_eq!(rc, 0);
    let arrow_schema = arrow_schema::Schema::try_from(&ffi_schema).unwrap();
    let id_field = arrow_schema.field_with_name("id").unwrap();
    assert_eq!(id_field.data_type(), &DataType::Int64);

    // Data is preserved (and now Int64-typed).
    let indices: [u64; 4] = [0, 1, 2, 3];
    let mut stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe {
        lance_dataset_take(
            ds,
            indices.as_ptr(),
            indices.len(),
            ptr::null(),
            &mut stream,
        )
    };
    assert_eq!(rc, 0);
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream) }.unwrap();
    let batches: Vec<_> = reader.into_iter().collect::<Result<Vec<_>, _>>().unwrap();
    let mut ids: Vec<i64> = Vec::new();
    for b in &batches {
        let col = b
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<arrow_array::Int64Array>()
            .unwrap();
        for i in 0..b.num_rows() {
            ids.push(col.value(i));
        }
    }
    assert_eq!(ids, vec![0i64, 1, 2, 3]);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_rename_and_relax_nullable_combined() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let path = c_str("id");
    let rename = c_str("row_id");
    let alt = LanceColumnAlteration {
        path: path.as_ptr(),
        rename: rename.as_ptr(),
        nullable_mode: LanceColumnNullableMode::True as i32,
        data_type: ptr::null(),
    };
    let alts = [alt];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, 0);

    let mut ffi_schema = FFI_ArrowSchema::empty();
    let rc = unsafe { lance_dataset_schema(ds, &mut ffi_schema) };
    assert_eq!(rc, 0);
    let arrow_schema = arrow_schema::Schema::try_from(&ffi_schema).unwrap();
    let renamed = arrow_schema.field_with_name("row_id").unwrap();
    assert!(renamed.is_nullable());
    assert!(arrow_schema.field_with_name("id").is_err());

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_multiple_per_call() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let p1 = c_str("value");
    let r1 = c_str("val");
    let p2 = c_str("label");
    let r2 = c_str("tag");
    let alts = [
        LanceColumnAlteration {
            path: p1.as_ptr(),
            rename: r1.as_ptr(),
            nullable_mode: LanceColumnNullableMode::Unchanged as i32,
            data_type: ptr::null(),
        },
        LanceColumnAlteration {
            path: p2.as_ptr(),
            rename: r2.as_ptr(),
            nullable_mode: LanceColumnNullableMode::Unchanged as i32,
            data_type: ptr::null(),
        },
    ];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, 0);

    let names = schema_field_names(ds);
    assert_eq!(names, vec!["id", "val", "tag"]);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_bumps_version() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let v_before = unsafe { lance_dataset_version(ds) };
    let path = c_str("label");
    let rename = c_str("tag");
    let alt = LanceColumnAlteration {
        path: path.as_ptr(),
        rename: rename.as_ptr(),
        nullable_mode: LanceColumnNullableMode::Unchanged as i32,
        data_type: ptr::null(),
    };
    let alts = [alt];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, 0);
    let v_after = unsafe { lance_dataset_version(ds) };
    assert!(
        v_after > v_before,
        "version should increase: before={v_before}, after={v_after}"
    );

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_unknown_column_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let path = c_str("no_such_column");
    let rename = c_str("whatever");
    let alt = LanceColumnAlteration {
        path: path.as_ptr(),
        rename: rename.as_ptr(),
        nullable_mode: LanceColumnNullableMode::Unchanged as i32,
        data_type: ptr::null(),
    };
    let alts = [alt];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    // Dataset is untouched on the error path.
    let names = schema_field_names(ds);
    assert_eq!(names, vec!["id", "value", "label"]);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_type_change_incompatible_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // Int32 -> Utf8 is not a valid Arrow upcast/downcast — upstream rejects.
    let new_type = ffi_schema_for(DataType::Utf8);
    let path = c_str("id");
    let alt = LanceColumnAlteration {
        path: path.as_ptr(),
        rename: ptr::null(),
        nullable_mode: LanceColumnNullableMode::Unchanged as i32,
        data_type: &new_type as *const _,
    };
    let alts = [alt];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_noop_alteration_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let path = c_str("id");
    let v_before = unsafe { lance_dataset_version(ds) };
    let alts = [default_alteration(path.as_ptr())];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    // No version bump and no schema change on the error path.
    assert_eq!(unsafe { lance_dataset_version(ds) }, v_before);
    assert_eq!(schema_field_names(ds), vec!["id", "value", "label"]);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_null_dataset_rejected() {
    let path = c_str("id");
    let rename = c_str("row_id");
    let alts = [LanceColumnAlteration {
        path: path.as_ptr(),
        rename: rename.as_ptr(),
        nullable_mode: LanceColumnNullableMode::Unchanged as i32,
        data_type: ptr::null(),
    }];
    let rc = unsafe { lance_dataset_alter_columns(ptr::null_mut(), alts.as_ptr(), alts.len()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

#[test]
fn test_alter_columns_null_alterations_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let rc = unsafe { lance_dataset_alter_columns(ds, ptr::null(), 1) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_zero_count_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let path = c_str("id");
    let rename = c_str("row_id");
    let alts = [LanceColumnAlteration {
        path: path.as_ptr(),
        rename: rename.as_ptr(),
        nullable_mode: LanceColumnNullableMode::Unchanged as i32,
        data_type: ptr::null(),
    }];
    let v_before = unsafe { lance_dataset_version(ds) };
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(unsafe { lance_dataset_version(ds) }, v_before);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_null_path_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let rename = c_str("whatever");
    let alts = [LanceColumnAlteration {
        path: ptr::null(),
        rename: rename.as_ptr(),
        nullable_mode: LanceColumnNullableMode::Unchanged as i32,
        data_type: ptr::null(),
    }];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_empty_path_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let empty = c_str("");
    let rename = c_str("whatever");
    let alts = [LanceColumnAlteration {
        path: empty.as_ptr(),
        rename: rename.as_ptr(),
        nullable_mode: LanceColumnNullableMode::Unchanged as i32,
        data_type: ptr::null(),
    }];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_empty_rename_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // Distinct from `rename = NULL` (which means "keep current name") — an
    // explicit empty string is a malformed request.
    let path = c_str("id");
    let empty = c_str("");
    let alts = [LanceColumnAlteration {
        path: path.as_ptr(),
        rename: empty.as_ptr(),
        nullable_mode: LanceColumnNullableMode::Unchanged as i32,
        data_type: ptr::null(),
    }];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_invalid_nullable_mode_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // An out-of-range discriminant (e.g. 99) must be rejected at the FFI
    // boundary rather than transmuted into the repr(C) enum. This locks in
    // the discriminant-validation contract.
    let path = c_str("id");
    let alts = [LanceColumnAlteration {
        path: path.as_ptr(),
        rename: ptr::null(),
        nullable_mode: 99,
        data_type: ptr::null(),
    }];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_released_schema_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // An uninitialised / already-released `FFI_ArrowSchema` has both its
    // `release` callback and its `format` field set to NULL. Passing it as
    // `data_type` must surface as INVALID_ARGUMENT rather than aborting the
    // host process via the arrow-rs `assert!(!format.is_null())`.
    let empty_schema = FFI_ArrowSchema::empty();
    let path = c_str("id");
    let alts = [LanceColumnAlteration {
        path: path.as_ptr(),
        rename: ptr::null(),
        nullable_mode: LanceColumnNullableMode::Unchanged as i32,
        data_type: &empty_schema as *const _,
    }];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_alter_columns_tighten_nullability_with_nulls_rejected() {
    // The `value` column actually carries a NULL, so upstream's pre-write
    // scan must reject the attempt to make it non-nullable.
    let (_tmp, uri) = create_dataset_with_nulls();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let path = c_str("value");
    let alts = [LanceColumnAlteration {
        path: path.as_ptr(),
        rename: ptr::null(),
        nullable_mode: LanceColumnNullableMode::False as i32,
        data_type: ptr::null(),
    }];
    let rc = unsafe { lance_dataset_alter_columns(ds, alts.as_ptr(), alts.len()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

// ---------------------------------------------------------------------------
// lance_dataset_add_columns_{sql,nulls,stream} tests
// ---------------------------------------------------------------------------

/// Build a `LanceSqlColumn` from two live `CString`s. The caller must keep the
/// `CString`s alive for as long as the returned struct is used.
fn sql_column(name: &CString, expression: &CString) -> LanceSqlColumn {
    LanceSqlColumn {
        name: name.as_ptr(),
        expression: expression.as_ptr(),
    }
}

/// Scan the dataset and build a `key -> value` map, casting both columns to
/// i64 so comparisons are independent of the exact arithmetic result type and
/// robust to any fragment/row scan ordering. A NULL value maps to `None`.
fn collect_i64_pairs(
    ds: *const LanceDataset,
    key: &str,
    value: &str,
) -> std::collections::HashMap<i64, Option<i64>> {
    let batches = scan_all_rows(ds);
    let mut map = std::collections::HashMap::new();
    for batch in &batches {
        let key_col =
            arrow::compute::cast(batch.column_by_name(key).unwrap(), &DataType::Int64).unwrap();
        let key_arr = key_col
            .as_any()
            .downcast_ref::<arrow_array::Int64Array>()
            .unwrap();
        let val_col =
            arrow::compute::cast(batch.column_by_name(value).unwrap(), &DataType::Int64).unwrap();
        let val_arr = val_col
            .as_any()
            .downcast_ref::<arrow_array::Int64Array>()
            .unwrap();
        for i in 0..batch.num_rows() {
            let v = if val_arr.is_null(i) {
                None
            } else {
                Some(val_arr.value(i))
            };
            map.insert(key_arr.value(i), v);
        }
    }
    map
}

// ── SQL variant ────────────────────────────────────────────────────────────

#[test]
fn test_add_columns_sql_single() {
    let (_tmp, uri) = create_large_dataset(5);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let name = c_str("id_x2");
    let expr = c_str("id * 2");
    let cols = [sql_column(&name, &expr)];
    let rc = unsafe { lance_dataset_add_columns_sql(ds, cols.as_ptr(), cols.len(), 0) };
    assert_eq!(rc, 0);

    // New column appears; row count is unchanged.
    let names = schema_field_names(ds);
    assert_eq!(names, vec!["id", "value", "label", "id_x2"]);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 5);

    // Values are computed from the existing `id` column.
    let pairs = collect_i64_pairs(ds, "id", "id_x2");
    for k in 0..5i64 {
        assert_eq!(pairs.get(&k), Some(&Some(k * 2)));
    }

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_sql_multiple_per_call() {
    let (_tmp, uri) = create_large_dataset(4);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let n1 = c_str("id_plus");
    let e1 = c_str("id + 10");
    let n2 = c_str("id_const");
    let e2 = c_str("100");
    let cols = [sql_column(&n1, &e1), sql_column(&n2, &e2)];
    let rc = unsafe { lance_dataset_add_columns_sql(ds, cols.as_ptr(), cols.len(), 0) };
    assert_eq!(rc, 0);

    let names = schema_field_names(ds);
    assert!(names.contains(&"id_plus".to_string()));
    assert!(names.contains(&"id_const".to_string()));

    let plus = collect_i64_pairs(ds, "id", "id_plus");
    let konst = collect_i64_pairs(ds, "id", "id_const");
    for k in 0..4i64 {
        assert_eq!(plus.get(&k), Some(&Some(k + 10)));
        assert_eq!(konst.get(&k), Some(&Some(100)));
    }

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_sql_bumps_version() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let v_before = unsafe { lance_dataset_version(ds) };
    let name = c_str("c");
    let expr = c_str("id + 1");
    let cols = [sql_column(&name, &expr)];
    let rc = unsafe { lance_dataset_add_columns_sql(ds, cols.as_ptr(), cols.len(), 0) };
    assert_eq!(rc, 0);
    let v_after = unsafe { lance_dataset_version(ds) };
    assert!(v_after > v_before, "version should increase");

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_sql_honors_batch_size() {
    // A small explicit batch size must still produce correct results across
    // the whole dataset (the scan is chunked, the output is not).
    let (_tmp, uri) = create_large_dataset(7);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let name = c_str("id_x2");
    let expr = c_str("id * 2");
    let cols = [sql_column(&name, &expr)];
    let rc = unsafe { lance_dataset_add_columns_sql(ds, cols.as_ptr(), cols.len(), 2) };
    assert_eq!(rc, 0);

    let pairs = collect_i64_pairs(ds, "id", "id_x2");
    for k in 0..7i64 {
        assert_eq!(pairs.get(&k), Some(&Some(k * 2)));
    }

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_sql_null_dataset_rejected() {
    let name = c_str("c");
    let expr = c_str("id + 1");
    let cols = [sql_column(&name, &expr)];
    let rc =
        unsafe { lance_dataset_add_columns_sql(ptr::null_mut(), cols.as_ptr(), cols.len(), 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

#[test]
fn test_add_columns_sql_null_columns_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let rc = unsafe { lance_dataset_add_columns_sql(ds, ptr::null(), 1, 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_sql_zero_count_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let name = c_str("c");
    let expr = c_str("id + 1");
    let cols = [sql_column(&name, &expr)];
    let rc = unsafe { lance_dataset_add_columns_sql(ds, cols.as_ptr(), 0, 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_sql_null_name_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let expr = c_str("id + 1");
    let cols = [LanceSqlColumn {
        name: ptr::null(),
        expression: expr.as_ptr(),
    }];
    let rc = unsafe { lance_dataset_add_columns_sql(ds, cols.as_ptr(), cols.len(), 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_sql_empty_name_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let name = c_str("");
    let expr = c_str("id + 1");
    let cols = [sql_column(&name, &expr)];
    let rc = unsafe { lance_dataset_add_columns_sql(ds, cols.as_ptr(), cols.len(), 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_sql_null_expression_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let name = c_str("c");
    let cols = [LanceSqlColumn {
        name: name.as_ptr(),
        expression: ptr::null(),
    }];
    let rc = unsafe { lance_dataset_add_columns_sql(ds, cols.as_ptr(), cols.len(), 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_sql_empty_expression_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let name = c_str("c");
    let expr = c_str("");
    let cols = [sql_column(&name, &expr)];
    let rc = unsafe { lance_dataset_add_columns_sql(ds, cols.as_ptr(), cols.len(), 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_sql_non_utf8_name_rejected() {
    // A non-UTF-8 `name` must surface as INVALID_ARGUMENT (parse_c_string maps
    // the Utf8Error to InvalidInput), not panic. `CString` holds arbitrary
    // non-NUL bytes, so it carries the invalid UTF-8 across the FFI boundary.
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let bad_name = CString::new([0xFFu8, 0xFE]).unwrap();
    let expr = c_str("id * 2");
    let cols = [LanceSqlColumn {
        name: bad_name.as_ptr(),
        expression: expr.as_ptr(),
    }];
    let rc = unsafe { lance_dataset_add_columns_sql(ds, cols.as_ptr(), cols.len(), 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_sql_non_utf8_expression_rejected() {
    // Symmetric with the name case: a non-UTF-8 `expression` goes through the
    // same `parse_required_field` path and must surface as INVALID_ARGUMENT.
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let name = c_str("c");
    let bad_expr = CString::new([0xFFu8, 0xFE]).unwrap();
    let cols = [LanceSqlColumn {
        name: name.as_ptr(),
        expression: bad_expr.as_ptr(),
    }];
    let rc = unsafe { lance_dataset_add_columns_sql(ds, cols.as_ptr(), cols.len(), 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_sql_malformed_expr_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let name = c_str("c");
    let expr = c_str("id +* 2");
    let cols = [sql_column(&name, &expr)];
    let rc = unsafe { lance_dataset_add_columns_sql(ds, cols.as_ptr(), cols.len(), 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_sql_unknown_column_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let name = c_str("c");
    let expr = c_str("does_not_exist + 1");
    let cols = [sql_column(&name, &expr)];
    let rc = unsafe { lance_dataset_add_columns_sql(ds, cols.as_ptr(), cols.len(), 0) };
    assert_eq!(rc, -1);
    // A non-existent column is resolved by the lance-datafusion planner, which
    // raises a schema error (`Error::Schema`) — the same upstream path as
    // `lance_dataset_delete`'s unknown-column predicate. We don't re-classify
    // it at the FFI boundary, so it surfaces as `Internal`, distinct from a
    // *syntax* error, which the planner wraps as `InvalidInput`. If upstream
    // ever tightens this to InvalidInput, tighten this assertion too.
    assert_eq!(lance_last_error_code(), LanceErrorCode::Internal);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_sql_name_collision_rejected() {
    // A new column whose name matches an existing column is rejected.
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let name = c_str("id");
    let expr = c_str("id + 1");
    let cols = [sql_column(&name, &expr)];
    let rc = unsafe { lance_dataset_add_columns_sql(ds, cols.as_ptr(), cols.len(), 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_sql_batch_size_overflow_rejected() {
    // A batch_size beyond u32::MAX must be rejected rather than silently
    // wrapped. (Only exercisable where u64 > u32::MAX, i.e. 64-bit hosts.)
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let name = c_str("c");
    let expr = c_str("id + 1");
    let cols = [sql_column(&name, &expr)];
    let too_big = u32::MAX as u64 + 1;
    let rc = unsafe { lance_dataset_add_columns_sql(ds, cols.as_ptr(), cols.len(), too_big) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

// ── AllNulls variant ───────────────────────────────────────────────────────

#[test]
fn test_add_columns_nulls_single() {
    let (_tmp, uri) = create_large_dataset(5);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let new_schema = Schema::new(vec![Field::new("extra", DataType::Int64, true)]);
    let ffi = schema_to_ffi(&new_schema);
    let rc = unsafe { lance_dataset_add_columns_nulls(ds, &ffi as *const _) };
    assert_eq!(rc, 0);

    let names = schema_field_names(ds);
    assert_eq!(names, vec!["id", "value", "label", "extra"]);
    // Row count is unchanged — this is a metadata-only add.
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 5);

    // Every row in the new column is NULL.
    let batches = scan_all_rows(ds);
    let total_nulls: usize = batches
        .iter()
        .map(|b| b.column_by_name("extra").unwrap().null_count())
        .sum();
    assert_eq!(total_nulls, 5);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_nulls_multiple_fields() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let new_schema = Schema::new(vec![
        Field::new("extra_int", DataType::Int64, true),
        Field::new("extra_str", DataType::Utf8, true),
    ]);
    let ffi = schema_to_ffi(&new_schema);
    let rc = unsafe { lance_dataset_add_columns_nulls(ds, &ffi as *const _) };
    assert_eq!(rc, 0);

    let names = schema_field_names(ds);
    assert!(names.contains(&"extra_int".to_string()));
    assert!(names.contains(&"extra_str".to_string()));

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_nulls_bumps_version() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let v_before = unsafe { lance_dataset_version(ds) };
    let new_schema = Schema::new(vec![Field::new("extra", DataType::Int64, true)]);
    let ffi = schema_to_ffi(&new_schema);
    let rc = unsafe { lance_dataset_add_columns_nulls(ds, &ffi as *const _) };
    assert_eq!(rc, 0);
    let v_after = unsafe { lance_dataset_version(ds) };
    assert!(v_after > v_before, "version should increase");

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_nulls_null_dataset_rejected() {
    let new_schema = Schema::new(vec![Field::new("extra", DataType::Int64, true)]);
    let ffi = schema_to_ffi(&new_schema);
    let rc = unsafe { lance_dataset_add_columns_nulls(ptr::null_mut(), &ffi as *const _) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
}

#[test]
fn test_add_columns_nulls_null_schema_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let rc = unsafe { lance_dataset_add_columns_nulls(ds, ptr::null()) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_nulls_released_schema_rejected() {
    // An uninitialised / already-released `FFI_ArrowSchema` has both its
    // `release` callback and `format` field NULL. It must surface as
    // INVALID_ARGUMENT rather than aborting via the arrow-rs assert.
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let empty_schema = FFI_ArrowSchema::empty();
    let rc = unsafe { lance_dataset_add_columns_nulls(ds, &empty_schema as *const _) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_nulls_non_utf8_format_rejected() {
    // A non-NULL but non-UTF-8 top-level `format` must be rejected at the FFI
    // boundary rather than aborting via arrow-rs's `format().to_str().expect()`
    // under `panic = "abort"`.
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // Hand-build a minimal `FFI_ArrowSchema` that owns no arrow-managed memory:
    // an empty struct with a no-op `release` we install, and `format` pointed at
    // non-UTF-8 bytes we own. This avoids overwriting an arrow-allocated
    // `format` pointer (whose producer release would then double-free against
    // our `CString` and corrupt the heap).
    unsafe extern "C" fn noop_release(_: *mut FFI_ArrowSchema) {}
    let bad_format = CString::new([0xFFu8, 0xFE]).unwrap();
    let mut ffi = FFI_ArrowSchema::empty();
    ffi.format = bad_format.as_ptr();
    ffi.release = Some(noop_release);
    let rc = unsafe { lance_dataset_add_columns_nulls(ds, &ffi as *const _) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_nulls_non_nullable_field_rejected() {
    // An all-null column cannot be non-nullable — upstream rejects it.
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let new_schema = Schema::new(vec![Field::new("extra", DataType::Int64, false)]);
    let ffi = schema_to_ffi(&new_schema);
    let rc = unsafe { lance_dataset_add_columns_nulls(ds, &ffi as *const _) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_nulls_name_collision_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // `value` already exists in the fixture.
    let new_schema = Schema::new(vec![Field::new("value", DataType::Int64, true)]);
    let ffi = schema_to_ffi(&new_schema);
    let rc = unsafe { lance_dataset_add_columns_nulls(ds, &ffi as *const _) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_nulls_legacy_dataset_not_supported() {
    // Adding all-null columns is metadata-only on the modern format, but the
    // legacy (0.1) file format can't represent missing columns that way, so
    // upstream returns NotSupported → LANCE_ERR_NOT_SUPPORTED. This is the one
    // documented error code the other tests don't reach, so write a legacy
    // dataset explicitly to exercise it.
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("legacy_ds").to_str().unwrap().to_string();
    let schema = Arc::new(Schema::new(vec![Field::new("id", DataType::Int32, false)]));
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![Arc::new(Int32Array::from(vec![1, 2, 3]))],
    )
    .unwrap();
    lance_c::runtime::block_on(async {
        let params = lance::dataset::WriteParams {
            data_storage_version: Some(lance_file::version::LanceFileVersion::Legacy),
            ..Default::default()
        };
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
            &uri,
            Some(params),
        )
        .await
        .unwrap();
    });

    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let new_schema = Schema::new(vec![Field::new("extra", DataType::Int64, true)]);
    let ffi = schema_to_ffi(&new_schema);
    let rc = unsafe { lance_dataset_add_columns_nulls(ds, &ffi as *const _) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::NotSupported);

    unsafe { lance_dataset_close(ds) };
}

// ── Stream variant ─────────────────────────────────────────────────────────

/// Build an `FFI_ArrowArrayStream` carrying a single Int32 column named `name`
/// with the given values — the precomputed data for a new column.
fn new_column_stream(name: &str, values: Vec<i32>) -> FFI_ArrowArrayStream {
    let schema = Arc::new(Schema::new(vec![Field::new(name, DataType::Int32, true)]));
    let batch = RecordBatch::try_new(schema, vec![Arc::new(Int32Array::from(values))]).unwrap();
    batch_to_ffi_stream(batch)
}

#[test]
fn test_add_columns_stream_single() {
    let (_tmp, uri) = create_large_dataset(5);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // Storage order matches id order (single fragment written 0..5).
    let mut stream = new_column_stream("extra", vec![1000, 1001, 1002, 1003, 1004]);
    let rc = unsafe { lance_dataset_add_columns_stream(ds, &mut stream, 0) };
    assert_eq!(rc, 0);

    let names = schema_field_names(ds);
    assert_eq!(names, vec!["id", "value", "label", "extra"]);
    assert_eq!(unsafe { lance_dataset_count_rows(ds) }, 5);

    let pairs = collect_i64_pairs(ds, "id", "extra");
    for k in 0..5i64 {
        assert_eq!(pairs.get(&k), Some(&Some(1000 + k)));
    }

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_stream_multi_fragment() {
    // The stream is sliced across fragment boundaries (5 + 5 rows).
    let (_tmp, uri) = create_multi_fragment_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let values: Vec<i32> = (0..10).map(|i| 1000 + i).collect();
    let mut stream = new_column_stream("extra", values);
    let rc = unsafe { lance_dataset_add_columns_stream(ds, &mut stream, 0) };
    assert_eq!(rc, 0);

    let pairs = collect_i64_pairs(ds, "id", "extra");
    for k in 0..10i64 {
        assert_eq!(pairs.get(&k), Some(&Some(1000 + k)));
    }

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_stream_honors_batch_size() {
    let (_tmp, uri) = create_multi_fragment_dataset();
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let values: Vec<i32> = (0..10).map(|i| 2000 + i).collect();
    let mut stream = new_column_stream("extra", values);
    let rc = unsafe { lance_dataset_add_columns_stream(ds, &mut stream, 3) };
    assert_eq!(rc, 0);

    let pairs = collect_i64_pairs(ds, "id", "extra");
    for k in 0..10i64 {
        assert_eq!(pairs.get(&k), Some(&Some(2000 + k)));
    }

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_stream_bumps_version() {
    let (_tmp, uri) = create_large_dataset(3);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let v_before = unsafe { lance_dataset_version(ds) };
    let mut stream = new_column_stream("extra", vec![7, 8, 9]);
    let rc = unsafe { lance_dataset_add_columns_stream(ds, &mut stream, 0) };
    assert_eq!(rc, 0);
    let v_after = unsafe { lance_dataset_version(ds) };
    assert!(v_after > v_before, "version should increase");

    unsafe { lance_dataset_close(ds) };
}

// (The NULL-dataset path is covered by `test_add_columns_stream_null_dataset_consumes_stream`
// below, which also proves the stream is consumed on that error path.)

#[test]
fn test_add_columns_stream_null_stream_rejected() {
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let rc = unsafe { lance_dataset_add_columns_stream(ds, ptr::null_mut(), 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_stream_row_count_mismatch_rejected() {
    // The stream supplies fewer rows than the dataset has — upstream rejects
    // the misaligned splice.
    let (_tmp, uri) = create_large_dataset(5);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // 3 stream rows vs 5 dataset rows. The error fires *inside* `add_columns`
    // (a different drop point than the early-return paths), so use the counted
    // stream to also prove the reader is released there.
    let (mut stream, drop_count) = make_counted_column_stream("extra", vec![1, 2, 3]);
    let rc = unsafe { lance_dataset_add_columns_stream(ds, &mut stream, 0) };
    assert_eq!(rc, -1);
    // Upstream `add_columns_from_stream` raises this via `Error::invalid_input`
    // ("Stream ended before producing values for all rows"), so it maps to
    // InvalidArgument — unlike the SQL unknown-column path, which is a schema
    // error (Internal). If upstream re-classifies it, update this assertion.
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_stream_consumed(&stream, &drop_count);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_stream_name_collision_rejected() {
    let (_tmp, uri) = create_large_dataset(5);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    // A stream column named `id` collides with the existing column.
    let mut stream = new_column_stream("id", vec![1, 2, 3, 4, 5]);
    let rc = unsafe { lance_dataset_add_columns_stream(ds, &mut stream, 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_stream_batch_size_overflow_rejected() {
    // Mirrors the SQL overflow test: a batch_size beyond u32::MAX is rejected
    // rather than silently wrapped. `from_raw` runs before the batch_size check,
    // so the stream must still be consumed — proven via the drop counter (a bare
    // `release.is_none()` check would be vacuous, since `from_raw` clears that
    // slot unconditionally).
    let (_tmp, uri) = create_large_dataset(5);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let (mut stream, drop_count) = make_counted_stream(&write_schema());
    let too_big = u32::MAX as u64 + 1;
    let rc = unsafe { lance_dataset_add_columns_stream(ds, &mut stream, too_big) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_stream_consumed(&stream, &drop_count);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_stream_missing_callback_rejected() {
    // A stream missing a mandatory CADI callback must be rejected at the FFI
    // boundary rather than aborting the process via an `unwrap()` deep inside
    // arrow-rs (which only guards against a NULL `release`). Cover both the
    // `get_schema` (construction) and `get_next` (iteration) callbacks, since
    // they abort on different arrow-rs code paths. The drop counter proves our
    // manual `release_fn(stream)` actually frees the reader on this path (which
    // does not go through `from_raw`).
    let (_tmp, uri) = create_large_dataset(5);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    for sabotage in ["get_schema", "get_next"] {
        let (mut stream, drop_count) = make_counted_stream(&write_schema());
        match sabotage {
            "get_schema" => stream.get_schema = None,
            "get_next" => stream.get_next = None,
            other => unreachable!("unknown sabotage target: {other}"),
        }
        let rc = unsafe { lance_dataset_add_columns_stream(ds, &mut stream, 0) };
        assert_eq!(rc, -1, "{sabotage}=None must be rejected");
        assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
        assert_stream_consumed(&stream, &drop_count);
        // We also null the caller's `release` slot so a non-compliant producer
        // cannot trigger a second release.
        assert!(
            stream.release.is_none(),
            "{sabotage}: release slot must be cleared"
        );
    }

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_stream_already_released_rejected() {
    // A stream with `release == None` is the CADI "already released" sentinel
    // (the first conjunct of the callback guard). It must be rejected, and our
    // handler must NOT invoke any release callback. `FFI_ArrowArrayStream::empty()`
    // owns no resources, so this path leaks nothing.
    let (_tmp, uri) = create_large_dataset(2);
    let c_uri = c_str(&uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
    assert!(!ds.is_null());

    let mut stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe { lance_dataset_add_columns_stream(ds, &mut stream, 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_add_columns_stream_null_dataset_consumes_stream() {
    // The dataset-NULL check runs *after* `from_raw`, so the stream is consumed
    // (released) even on that error path — proven via the drop counter.
    let (mut stream, drop_count) = make_counted_stream(&write_schema());
    let rc = unsafe { lance_dataset_add_columns_stream(ptr::null_mut(), &mut stream, 0) };
    assert_eq!(rc, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_stream_consumed(&stream, &drop_count);
}
