// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Native `Dataset::merge` acceptance through the C entry point.

use std::collections::BTreeMap;
use std::ffi::{CStr, CString};
use std::ptr;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema};
use arrow::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use arrow::record_batch::RecordBatchReader;
use arrow_array::{Array, Int32Array, Int64Array, RecordBatch, StringArray, UInt64Array};
use arrow_schema::{DataType, Field, Schema};
use lance::Dataset;
use lance::dataset::WriteParams;
use lance_c::*;

fn c_string(value: &str) -> CString {
    CString::new(value).unwrap()
}

fn schema_to_ffi(schema: &Schema) -> FFI_ArrowSchema {
    FFI_ArrowSchema::try_from(schema).unwrap()
}

fn stream_from(batches: Vec<RecordBatch>, schema: Arc<Schema>) -> FFI_ArrowArrayStream {
    FFI_ArrowArrayStream::new(Box::new(arrow::record_batch::RecordBatchIterator::new(
        batches.into_iter().map(Ok),
        schema,
    )))
}

struct CountingReader {
    inner: Box<dyn RecordBatchReader + Send>,
    drops: Arc<AtomicUsize>,
}

impl Iterator for CountingReader {
    type Item = arrow::error::Result<RecordBatch>;

    fn next(&mut self) -> Option<Self::Item> {
        self.inner.next()
    }
}

impl RecordBatchReader for CountingReader {
    fn schema(&self) -> Arc<Schema> {
        self.inner.schema()
    }
}

impl Drop for CountingReader {
    fn drop(&mut self) {
        self.drops.fetch_add(1, Ordering::SeqCst);
    }
}

fn counted_stream(
    batches: Vec<RecordBatch>,
    schema: Arc<Schema>,
) -> (FFI_ArrowArrayStream, Arc<AtomicUsize>) {
    let drops = Arc::new(AtomicUsize::new(0));
    let inner = arrow::record_batch::RecordBatchIterator::new(batches.into_iter().map(Ok), schema);
    let reader = CountingReader {
        inner: Box::new(inner),
        drops: drops.clone(),
    };
    (FFI_ArrowArrayStream::new(Box::new(reader)), drops)
}

fn assert_stream_released(drops: &Arc<AtomicUsize>) {
    assert_eq!(
        drops.load(Ordering::SeqCst),
        1,
        "source stream release must run exactly once"
    );
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

fn create_merge_dataset(uri: &str, stable: bool) {
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("label", DataType::Utf8, true),
        Field::new("rank", DataType::Int32, true),
    ]));
    let first = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from(vec![20, 10, 50, 30])),
            Arc::new(StringArray::from(vec![
                Some("twenty"),
                Some("ten"),
                None,
                Some("thirty"),
            ])),
            Arc::new(Int32Array::from(vec![Some(2), Some(1), None, Some(3)])),
        ],
    )
    .unwrap();
    let second = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from(vec![40, 60, 15, 25])),
            Arc::new(StringArray::from(vec![
                Some("forty"),
                None,
                Some("fifteen"),
                Some("twenty-five"),
            ])),
            Arc::new(Int32Array::from(vec![Some(4), None, Some(15), Some(25)])),
        ],
    )
    .unwrap();

    lance_c::runtime::block_on(async {
        let mut dataset = Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(first)], schema.clone()),
            uri,
            Some(WriteParams {
                max_rows_per_file: 3,
                enable_stable_row_ids: stable,
                ..Default::default()
            }),
        )
        .await
        .unwrap();
        dataset
            .append(
                arrow::record_batch::RecordBatchIterator::new(vec![Ok(second)], schema),
                None,
            )
            .await
            .unwrap();
        dataset.delete("id = 30").await.unwrap();
    });
}

fn rhs_batch() -> (Arc<Schema>, RecordBatch) {
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, true),
        Field::new("score", DataType::Int64, true),
        Field::new("tag", DataType::Utf8, true),
    ]));
    let keys: Vec<Option<i32>> = vec![
        Some(10),
        Some(99),
        Some(20),
        None,
        Some(20),
        Some(40),
        Some(15),
        Some(70),
    ];
    let scores: Vec<Option<i64>> = vec![
        Some(100),
        Some(199),
        Some(200),
        Some(999),
        Some(201),
        Some(400),
        Some(150),
        Some(700),
    ];
    let tags = vec![
        Some("a"),
        Some("unused-99"),
        Some("b-first"),
        Some("null-key"),
        Some("b-last"),
        Some("d"),
        Some("o"),
        Some("unused-70"),
    ];
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from(keys)),
            Arc::new(Int64Array::from(scores)),
            Arc::new(StringArray::from(tags)),
        ],
    )
    .unwrap();
    (schema, batch)
}

fn rows_by_id(dataset: *const LanceDataset) -> BTreeMap<i32, (Option<i64>, Option<String>)> {
    let mut rows = BTreeMap::new();
    for batch in scan_batches(dataset) {
        let ids = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        let scores = batch
            .column_by_name("score")
            .unwrap()
            .as_any()
            .downcast_ref::<Int64Array>()
            .unwrap();
        let tags = batch
            .column_by_name("tag")
            .unwrap()
            .as_any()
            .downcast_ref::<StringArray>()
            .unwrap();
        for row in 0..batch.num_rows() {
            rows.insert(
                ids.value(row),
                (
                    if scores.is_null(row) {
                        None
                    } else {
                        Some(scores.value(row))
                    },
                    if tags.is_null(row) {
                        None
                    } else {
                        Some(tags.value(row).to_owned())
                    },
                ),
            );
        }
    }
    rows
}

#[test]
fn test_merge_matches_native_oracle_and_preserves_snapshots() {
    let tmp = tempfile::tempdir().unwrap();
    let native_uri = tmp.path().join("native").to_str().unwrap().to_owned();
    let c_uri = tmp.path().join("c").to_str().unwrap().to_owned();
    create_merge_dataset(&native_uri, false);
    create_merge_dataset(&c_uri, false);

    let (rhs_schema, rhs) = rhs_batch();
    let mut native = lance_c::runtime::block_on(Dataset::open(&native_uri)).unwrap();
    lance_c::runtime::block_on(native.merge(
        arrow::record_batch::RecordBatchIterator::new(vec![Ok(rhs.clone())], rhs_schema.clone()),
        "id",
        "id",
    ))
    .unwrap();

    let c_dataset = open(&c_uri);
    let old_handle = open(&c_uri);
    let old_version = unsafe { lance_dataset_version(c_dataset) };
    let old_scanner = unsafe { lance_scanner_new(c_dataset, ptr::null(), ptr::null()) };
    assert!(!old_scanner.is_null());
    let mut ids = vec![0u64; unsafe { lance_dataset_fragment_count(c_dataset) } as usize];
    assert_eq!(
        unsafe { lance_dataset_fragment_ids(c_dataset, ids.as_mut_ptr()) },
        0
    );
    let old_fragment = unsafe { lance_dataset_open_fragment(c_dataset, ids[0]) };
    assert!(!old_fragment.is_null());

    let mut stream = stream_from(vec![rhs], rhs_schema);
    let left = c_string("id");
    let right = c_string("id");
    assert_eq!(
        unsafe { lance_dataset_merge(c_dataset, left.as_ptr(), right.as_ptr(), &mut stream) },
        0
    );
    assert!(unsafe { lance_dataset_version(c_dataset) } > old_version);

    let oracle = open(&native_uri);
    let expected = rows_by_id(oracle);
    let actual = rows_by_id(c_dataset);
    assert_eq!(actual, expected);
    assert_eq!(actual.get(&10), Some(&(Some(100), Some("a".to_owned()))));
    assert_eq!(
        actual.get(&20),
        Some(&(Some(201), Some("b-last".to_owned())))
    );
    assert_eq!(actual.get(&40), Some(&(Some(400), Some("d".to_owned()))));
    assert_eq!(
        actual.get(&50),
        Some(&(None, None)),
        "unmatched LHS rows receive NULL new columns"
    );
    assert!(!actual.contains_key(&30), "deleted rows stay deleted");
    assert!(!actual.contains_key(&99), "unmatched RHS rows are ignored");
    assert_eq!(
        exported_schema(c_dataset),
        exported_schema(oracle),
        "C and native oracle schemas must match exactly"
    );

    let reopened = open(&c_uri);
    assert_eq!(unsafe { lance_dataset_version(reopened) }, unsafe {
        lance_dataset_version(c_dataset)
    });
    assert_eq!(exported_schema(reopened), exported_schema(c_dataset));

    assert_eq!(unsafe { lance_dataset_version(old_handle) }, old_version);
    let old_schema = exported_schema(old_handle);
    assert!(old_schema.field_with_name("score").is_err());
    let mut fragment_schema = FFI_ArrowSchema::empty();
    assert_eq!(
        unsafe { lance_fragment_schema(old_fragment, &mut fragment_schema) },
        0
    );
    let fragment_schema = Schema::try_from(&fragment_schema).unwrap();
    assert!(fragment_schema.field_with_name("score").is_err());

    let mut old_stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(old_scanner, &mut old_stream) },
        0
    );
    let old_reader = unsafe { ArrowArrayStreamReader::from_raw(&mut old_stream) }.unwrap();
    assert!(old_reader.schema().field_with_name("score").is_err());
    assert_eq!(
        old_reader
            .map(Result::unwrap)
            .map(|b| b.num_rows())
            .sum::<usize>(),
        7
    );

    unsafe {
        lance_scanner_close(old_scanner);
        lance_fragment_close(old_fragment);
        lance_dataset_close(old_handle);
        lance_dataset_close(reopened);
        lance_dataset_close(oracle);
        lance_dataset_close(c_dataset);
    }
}

fn collect_row_keys(uri: &str, with_row_id: bool) -> (Vec<(i32, u64)>, u64, u64) {
    let dataset = open(uri);
    let mut scanner = unsafe { lance_scanner_new(dataset, ptr::null(), ptr::null()) };
    assert!(!scanner.is_null());
    if with_row_id {
        assert_eq!(unsafe { lance_scanner_with_row_id(scanner, true) }, 0);
    } else {
        assert_eq!(unsafe { lance_scanner_with_row_address(scanner, true) }, 0);
    }
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream) },
        0
    );
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream) }.unwrap();
    let name = if with_row_id { "_rowid" } else { "_rowaddr" };
    let mut rows = Vec::new();
    let mut min_key = u64::MAX;
    let mut max_key = 0;
    for batch in reader {
        let batch = batch.unwrap();
        let ids = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        let keys = batch
            .column_by_name(name)
            .unwrap()
            .as_any()
            .downcast_ref::<UInt64Array>()
            .unwrap();
        for row in 0..batch.num_rows() {
            assert!(!keys.is_null(row));
            let key = keys.value(row);
            min_key = min_key.min(key);
            max_key = max_key.max(key);
            rows.push((ids.value(row), key));
        }
    }
    unsafe {
        lance_scanner_close(scanner);
        lance_dataset_close(dataset);
    }
    (rows, min_key, max_key)
}

fn write_key_rhs(
    key_field: &str,
    output_field: &str,
    rows: &[(i32, u64)],
) -> (Arc<Schema>, RecordBatch) {
    let schema = Arc::new(Schema::new(vec![
        Field::new(key_field, DataType::UInt64, false),
        Field::new(output_field, DataType::Int64, false),
    ]));
    let keys = rows.iter().map(|(_, key)| *key).collect::<Vec<_>>();
    let values = rows
        .iter()
        .map(|(id, _)| i64::from(*id) + 1000)
        .collect::<Vec<_>>();
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(UInt64Array::from(keys)),
            Arc::new(Int64Array::from(values)),
        ],
    )
    .unwrap();
    (schema, batch)
}

#[test]
fn test_merge_row_id_and_row_addr_keys() {
    for (key, output_field) in [("_rowid", "by_rowid"), ("_rowaddr", "by_rowaddr")] {
        let tmp = tempfile::tempdir().unwrap();
        let native_uri = tmp.path().join("native").to_str().unwrap().to_owned();
        let c_uri = tmp.path().join("c").to_str().unwrap().to_owned();
        create_merge_dataset(&native_uri, true);
        create_merge_dataset(&c_uri, true);

        let (rows, _, _) = collect_row_keys(&c_uri, key == "_rowid");
        let (rhs_schema, rhs) = write_key_rhs("join_key", output_field, &rows);
        let (native_rows, _, _) = collect_row_keys(&native_uri, key == "_rowid");
        let (native_schema, native_rhs) = write_key_rhs("join_key", output_field, &native_rows);

        let mut native = lance_c::runtime::block_on(Dataset::open(&native_uri)).unwrap();
        lance_c::runtime::block_on(native.merge(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(native_rhs)], native_schema),
            key,
            "join_key",
        ))
        .unwrap();

        let dataset = open(&c_uri);
        let mut stream = stream_from(vec![rhs], rhs_schema);
        let left_key = c_string(key);
        let right_key = c_string("join_key");
        assert_eq!(
            unsafe {
                lance_dataset_merge(dataset, left_key.as_ptr(), right_key.as_ptr(), &mut stream)
            },
            0,
            "native merge with {key} failed: {:?}",
            take_error()
        );

        let mut expected = BTreeMap::new();
        let oracle = open(&native_uri);
        for batch in scan_batches(oracle) {
            let ids = batch
                .column_by_name("id")
                .unwrap()
                .as_any()
                .downcast_ref::<Int32Array>()
                .unwrap();
            let output = batch
                .column_by_name(output_field)
                .unwrap()
                .as_any()
                .downcast_ref::<Int64Array>()
                .unwrap();
            for row in 0..batch.num_rows() {
                expected.insert(ids.value(row), output.value(row));
            }
        }
        unsafe { lance_dataset_close(oracle) };
        let mut actual = BTreeMap::new();
        for batch in scan_batches(dataset) {
            let ids = batch
                .column_by_name("id")
                .unwrap()
                .as_any()
                .downcast_ref::<Int32Array>()
                .unwrap();
            let output = batch
                .column_by_name(output_field)
                .unwrap()
                .as_any()
                .downcast_ref::<Int64Array>()
                .unwrap();
            for row in 0..batch.num_rows() {
                actual.insert(ids.value(row), output.value(row));
            }
        }
        assert_eq!(actual, expected, "{key} native oracle mismatch");
        for (id, value) in actual {
            assert_eq!(value, i64::from(id) + 1000);
        }
        unsafe { lance_dataset_close(dataset) };
    }
}

#[test]
fn test_merge_errors_consume_stream_and_preserve_version() {
    let (_tmp, uri) = {
        let tmp = tempfile::tempdir().unwrap();
        let uri = tmp.path().join("ds").to_str().unwrap().to_owned();
        create_merge_dataset(&uri, false);
        (tmp, uri)
    };
    let dataset = open(&uri);
    let baseline = unsafe { lance_dataset_version(dataset) };
    let (rhs_schema, rhs) = rhs_batch();
    let left = c_string("id");
    let right = c_string("id");

    let (mut stream, drops) = counted_stream(vec![rhs.clone()], rhs_schema.clone());
    assert_eq!(
        unsafe { lance_dataset_merge(ptr::null_mut(), left.as_ptr(), right.as_ptr(), &mut stream) },
        -1
    );
    assert_eq!(take_error().0, LanceErrorCode::InvalidArgument);
    assert_stream_released(&drops);

    let (mut stream, drops) = counted_stream(vec![rhs.clone()], rhs_schema.clone());
    assert_eq!(
        unsafe { lance_dataset_merge(dataset, ptr::null(), right.as_ptr(), &mut stream) },
        -1
    );
    assert_eq!(take_error().0, LanceErrorCode::InvalidArgument);
    assert_stream_released(&drops);

    let (mut stream, drops) = counted_stream(vec![rhs.clone()], rhs_schema.clone());
    assert_eq!(
        unsafe { lance_dataset_merge(dataset, left.as_ptr(), ptr::null(), &mut stream) },
        -1
    );
    assert_eq!(take_error().0, LanceErrorCode::InvalidArgument);
    assert_stream_released(&drops);

    for (left_key, right_key) in [
        (String::new(), "id".to_string()),
        ("id".to_string(), String::new()),
        ("missing".to_string(), "id".to_string()),
        ("id".to_string(), "missing".to_string()),
    ] {
        let (mut stream, drops) = counted_stream(vec![rhs.clone()], rhs_schema.clone());
        assert_eq!(
            unsafe {
                lance_dataset_merge(
                    dataset,
                    c_string(&left_key).as_ptr(),
                    c_string(&right_key).as_ptr(),
                    &mut stream,
                )
            },
            -1,
            "merge should reject left={left_key:?} right={right_key:?}"
        );
        let (code, _) = take_error();
        assert!(matches!(
            code,
            LanceErrorCode::InvalidArgument | LanceErrorCode::Internal
        ));
        assert_stream_released(&drops);
    }

    let conflict_schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("label", DataType::Utf8, true),
    ]));
    let conflict = RecordBatch::try_new(
        conflict_schema.clone(),
        vec![
            Arc::new(Int32Array::from(vec![10])),
            Arc::new(StringArray::from(vec![Some("collision")])),
        ],
    )
    .unwrap();
    let (mut stream, drops) = counted_stream(vec![conflict], conflict_schema);
    assert_eq!(
        unsafe { lance_dataset_merge(dataset, left.as_ptr(), right.as_ptr(), &mut stream) },
        -1
    );
    assert_eq!(take_error().0, LanceErrorCode::InvalidArgument);
    assert_stream_released(&drops);

    let mismatch_schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Utf8, false),
        Field::new("other", DataType::Int64, true),
    ]));
    let mismatch = RecordBatch::try_new(
        mismatch_schema.clone(),
        vec![
            Arc::new(StringArray::from(vec!["10"])),
            Arc::new(Int64Array::from(vec![Some(1)])),
        ],
    )
    .unwrap();
    let (mut stream, drops) = counted_stream(vec![mismatch], mismatch_schema);
    assert_eq!(
        unsafe { lance_dataset_merge(dataset, left.as_ptr(), right.as_ptr(), &mut stream) },
        -1
    );
    take_error();
    assert_stream_released(&drops);

    let (mut stream, drops) = counted_stream(Vec::new(), rhs_schema.clone());
    assert_eq!(
        unsafe { lance_dataset_merge(dataset, left.as_ptr(), right.as_ptr(), &mut stream) },
        -1
    );
    take_error();
    assert_stream_released(&drops);

    let (mut stream, drops) = counted_stream(vec![rhs.clone()], rhs_schema.clone());
    stream.get_next = None;
    assert_eq!(
        unsafe { lance_dataset_merge(dataset, left.as_ptr(), right.as_ptr(), &mut stream) },
        -1
    );
    assert_eq!(take_error().0, LanceErrorCode::InvalidArgument);
    assert_stream_released(&drops);

    assert_eq!(unsafe { lance_dataset_version(dataset) }, baseline);
    let (mut stream, drops) = counted_stream(vec![rhs], rhs_schema);
    assert_eq!(
        unsafe { lance_dataset_merge(dataset, left.as_ptr(), right.as_ptr(), &mut stream) },
        0
    );
    assert_stream_released(&drops);
    assert!(unsafe { lance_dataset_version(dataset) } > baseline);
    unsafe { lance_dataset_close(dataset) };
}

#[test]
fn test_merge_independent_handle_commit_conflict() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ds").to_str().unwrap().to_owned();
    create_merge_dataset(&uri, false);
    let stale = open(&uri);
    let writer = open(&uri);
    let baseline = unsafe { lance_dataset_version(stale) };
    assert_eq!(baseline, unsafe { lance_dataset_version(writer) });

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
    assert!(unsafe { lance_dataset_version(writer) } > baseline);

    let (rhs_schema, rhs) = rhs_batch();
    let (mut stream, drops) = counted_stream(vec![rhs], rhs_schema);
    let left = c_string("id");
    let right = c_string("id");
    assert_eq!(
        unsafe { lance_dataset_merge(stale, left.as_ptr(), right.as_ptr(), &mut stream) },
        -1
    );
    assert_eq!(take_error().0, LanceErrorCode::CommitConflict);
    assert_stream_released(&drops);
    assert_eq!(unsafe { lance_dataset_version(stale) }, baseline);

    let latest = open(&uri);
    assert!(unsafe { lance_dataset_version(latest) } > baseline);
    let schema = exported_schema(latest);
    assert!(schema.field_with_name("writer_column").is_ok());
    assert!(schema.field_with_name("score").is_err());
    unsafe {
        lance_dataset_close(latest);
        lance_dataset_close(writer);
        lance_dataset_close(stale);
    }
}
