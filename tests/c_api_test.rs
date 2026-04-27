// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Integration tests for the Lance C API.
//!
//! These tests call the `extern "C"` functions directly from Rust,
//! validating the C API contract without needing a C compiler.

use std::ffi::CString;
use std::ptr;
use std::sync::Arc;

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi::from_ffi;
use arrow::ffi_stream::ArrowArrayStreamReader;
use arrow::ffi_stream::FFI_ArrowArrayStream;
use arrow::record_batch::RecordBatchReader;
use arrow_array::{Float32Array, Int32Array, RecordBatch, StringArray};
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
