// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

use std::ffi::{CStr, CString};
use std::ptr;
use std::sync::Arc;

use arrow::array::{Array, Int32Array, RecordBatch, StructArray, UInt64Array};
use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema};
use arrow_schema::{DataType, Field, Schema};
use lance::Dataset;
use lance::dataset::transaction::Operation;
use lance::dataset::{WriteMode, WriteParams};
use lance_c::*;

fn c_string(value: &str) -> CString {
    CString::new(value).unwrap()
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

fn error_is(code: LanceErrorCode, expected: LanceErrorCode) -> bool {
    code == expected
}

async fn create_fixture(uri: &str) -> Dataset {
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("a", DataType::Int32, true),
        Field::new("b", DataType::Int32, true),
    ]));
    let ids: Vec<i32> = (1..=20).collect();
    let a: Vec<Option<i32>> = ids
        .iter()
        .map(|id| {
            if *id == 7 || *id == 16 {
                None
            } else {
                Some(id * 10)
            }
        })
        .collect();
    let b: Vec<Option<i32>> = ids.iter().map(|id| Some(id * 100)).collect();
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from(ids)),
            Arc::new(Int32Array::from(a)),
            Arc::new(Int32Array::from(b)),
        ],
    )
    .unwrap();
    let mut dataset = Dataset::write(
        arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
        uri,
        Some(WriteParams {
            max_rows_per_file: 10,
            mode: WriteMode::Create,
            ..Default::default()
        }),
    )
    .await
    .unwrap();
    dataset.delete("id = 10").await.unwrap();
    dataset
}

struct Fixture {
    uri: String,
    dataset: *mut LanceDataset,
    _dir: tempfile::TempDir,
}

impl Fixture {
    fn new() -> Self {
        let dir = tempfile::tempdir().unwrap();
        let uri = format!("{}/dataset", dir.path().display());
        let native = lance_c::runtime::block_on(create_fixture(&uri));
        drop(native);
        let c_uri = c_string(&uri);
        let dataset = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
        assert!(!dataset.is_null(), "{:?}", take_error());
        Self {
            uri,
            dataset,
            _dir: dir,
        }
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        unsafe { lance_dataset_close(self.dataset) };
    }
}

fn ffi_pair(batch: &RecordBatch) -> (FFI_ArrowArray, FFI_ArrowSchema) {
    let struct_array: StructArray = batch.clone().into();
    (
        FFI_ArrowArray::new(&struct_array.into_data()),
        FFI_ArrowSchema::try_from(batch.schema().as_ref()).unwrap(),
    )
}

fn import_batch(batch: &RecordBatch) -> *mut LanceBatch {
    let (mut array, schema) = ffi_pair(batch);
    let mut imported = ptr::null_mut();
    assert_eq!(
        unsafe { lance_batch_from_arrow(&mut array, &schema, &mut imported) },
        0,
        "{:?}",
        take_error()
    );
    assert!(array.release.is_none());
    imported
}

fn row_address(fragment_id: u64, offset: u64) -> u64 {
    (fragment_id << 32) | offset
}

fn overlay_schema(fields: &[&str]) -> Arc<Schema> {
    let mut columns = vec![Field::new("_rowaddr", DataType::UInt64, false)];
    for name in fields {
        columns.push(Field::new(*name, DataType::Int32, true));
    }
    Arc::new(Schema::new(columns))
}

fn overlay_batch(
    rowaddrs: Vec<u64>,
    a: Option<Vec<Option<i32>>>,
    b: Option<Vec<Option<i32>>>,
) -> RecordBatch {
    let mut names = vec!["_rowaddr"];
    let mut arrays: Vec<Arc<dyn Array>> = vec![Arc::new(UInt64Array::from(rowaddrs))];
    if let Some(values) = a {
        names.push("a");
        arrays.push(Arc::new(Int32Array::from(values)));
    }
    if let Some(values) = b {
        names.push("b");
        arrays.push(Arc::new(Int32Array::from(values)));
    }
    let schema = overlay_schema(&names[1..]);
    RecordBatch::try_new(schema, arrays).unwrap()
}

fn open_fragment(dataset: *mut LanceDataset, fragment_id: u64) -> *mut LanceFragment {
    let fragment = unsafe { lance_dataset_open_fragment(dataset, fragment_id) };
    assert!(!fragment.is_null(), "{:?}", take_error());
    fragment
}

fn projected_schema(fragment: *mut LanceFragment, columns: &[&str]) -> *mut LanceSchema {
    let mut schema = ptr::null_mut();
    assert_eq!(
        unsafe { lance_fragment_schema_clone(fragment, &mut schema) },
        0,
        "{:?}",
        take_error()
    );
    let names: Vec<CString> = columns.iter().map(|name| c_string(name)).collect();
    let pointers: Vec<*const i8> = names.iter().map(|name| name.as_ptr()).collect();
    let mut projected = ptr::null_mut();
    assert_eq!(
        unsafe { lance_schema_project(schema, pointers.as_ptr(), pointers.len(), &mut projected) },
        0,
        "{:?}",
        take_error()
    );
    unsafe { lance_schema_close(schema) };
    projected
}

fn new_writer(
    fragment: *mut LanceFragment,
    schema: *mut LanceSchema,
) -> *mut LanceFragmentOverlayWriter {
    let mut writer = ptr::null_mut();
    assert_eq!(
        unsafe { lance_fragment_overlay_writer_new(fragment, schema, &mut writer) },
        0,
        "{:?}",
        take_error()
    );
    assert!(!writer.is_null());
    writer
}

fn write_overlay(writer: *mut LanceFragmentOverlayWriter, batch: &RecordBatch) {
    let imported = import_batch(batch);
    assert_eq!(
        unsafe { lance_fragment_overlay_writer_write_batch(writer, imported) },
        0,
        "{:?}",
        take_error()
    );
    unsafe { lance_batch_free(imported) };
}

fn finish(writer: *mut LanceFragmentOverlayWriter) -> *mut LanceDataOverlayGroup {
    let mut group = ptr::null_mut();
    assert_eq!(
        unsafe { lance_fragment_overlay_writer_finish(writer, &mut group) },
        0,
        "{:?}",
        take_error()
    );
    group
}

fn file_bitmaps(file: *mut LanceDataOverlayFile) -> Vec<Vec<u8>> {
    let mut count = 0;
    assert_eq!(
        unsafe { lance_data_overlay_file_bitmap_count(file, &mut count) },
        0,
        "{:?}",
        take_error()
    );
    let mut bitmaps = Vec::new();
    for index in 0..count {
        let mut bytes: *mut u8 = ptr::null_mut();
        let mut len = 0usize;
        assert_eq!(
            unsafe {
                lance_data_overlay_file_bitmap_at(file, index as usize, &mut bytes, &mut len)
            },
            0,
            "{:?}",
            take_error()
        );
        assert!(!bytes.is_null());
        let bitmap = unsafe { std::slice::from_raw_parts(bytes, len).to_vec() };
        unsafe { lance_free_bytes(bytes) };
        bitmaps.push(bitmap);
    }
    bitmaps
}

fn operation_groups(operation: *mut LanceOperation) -> Vec<*mut LanceDataOverlayGroup> {
    let mut count = 0;
    assert_eq!(
        unsafe { lance_operation_data_overlay_group_count(operation, &mut count) },
        0,
        "{:?}",
        take_error()
    );
    let mut groups = Vec::new();
    for index in 0..count {
        let mut group = ptr::null_mut();
        assert_eq!(
            unsafe { lance_operation_data_overlay_group_at(operation, index as usize, &mut group) },
            0,
            "{:?}",
            take_error()
        );
        assert!(!group.is_null());
        groups.push(group);
    }
    groups
}

fn commit(
    uri: &str,
    read_version: u64,
    groups: &[*mut LanceDataOverlayGroup],
) -> (*mut LanceDataset, LanceErrorCode) {
    let operation = unsafe {
        lance_operation_data_overlay_new(
            groups.as_ptr() as *const *const LanceDataOverlayGroup,
            groups.len(),
        )
    };
    assert!(!operation.is_null(), "{:?}", take_error());
    let transaction = unsafe { lance_transaction_new(read_version, operation, ptr::null()) };
    assert!(!transaction.is_null(), "{:?}", take_error());
    let mut dataset = ptr::null_mut();
    let uri = c_string(uri);
    let _status = unsafe {
        lance_transaction_commit(
            uri.as_ptr(),
            transaction,
            ptr::null(),
            ptr::null(),
            ptr::null(),
            &mut dataset,
        )
    };
    let code = lance_last_error_code();
    unsafe {
        lance_transaction_close(transaction);
        lance_operation_close(operation);
    }
    (dataset, code)
}

fn read_native_batch(dataset: &Dataset) -> RecordBatch {
    lance_c::runtime::block_on(async {
        dataset.scan().with_row_id().try_into_batch().await.unwrap()
    })
}

fn row_values(batch: &RecordBatch, wanted: i32) -> Option<(Option<i32>, Option<i32>, u64)> {
    let ids = batch
        .column_by_name("id")
        .unwrap()
        .as_any()
        .downcast_ref::<Int32Array>()
        .unwrap();
    let a = batch
        .column_by_name("a")
        .unwrap()
        .as_any()
        .downcast_ref::<Int32Array>()
        .unwrap();
    let b = batch
        .column_by_name("b")
        .unwrap()
        .as_any()
        .downcast_ref::<Int32Array>()
        .unwrap();
    let rowid = batch
        .column_by_name("_rowid")
        .unwrap()
        .as_any()
        .downcast_ref::<UInt64Array>()
        .unwrap();
    for index in 0..batch.num_rows() {
        if ids.value(index) == wanted {
            return Some((
                (!a.is_null(index)).then(|| a.value(index)),
                (!b.is_null(index)).then(|| b.value(index)),
                rowid.value(index),
            ));
        }
    }
    None
}

fn assert_values(dataset: &Dataset, expected: &[(i32, Option<i32>, Option<i32>)], rowids: &[u64]) {
    let batch = read_native_batch(dataset);
    for (id, a, b) in expected {
        let values = row_values(&batch, *id).unwrap_or_else(|| panic!("missing row {id}"));
        assert_eq!(values.0, *a, "row {id} a");
        assert_eq!(values.1, *b, "row {id} b");
    }
    let ids = batch
        .column_by_name("id")
        .unwrap()
        .as_any()
        .downcast_ref::<Int32Array>()
        .unwrap();
    let rowid = batch
        .column_by_name("_rowid")
        .unwrap()
        .as_any()
        .downcast_ref::<UInt64Array>()
        .unwrap();
    for index in 0..batch.num_rows() {
        let id = ids.value(index);
        if let Some(position) = expected.iter().position(|(wanted, _, _)| *wanted == id) {
            assert_eq!(rowid.value(index), rowids[position], "row {id} row id");
        }
    }
}

#[test]
fn data_overlay_value_and_operation_lifetimes() {
    let fixture = Fixture::new();
    let fragment_count = unsafe { lance_dataset_fragment_count(fixture.dataset) };
    assert_eq!(fragment_count, 2);
    let mut ids = vec![0u64; fragment_count as usize];
    assert_eq!(
        unsafe { lance_dataset_fragment_ids(fixture.dataset, ids.as_mut_ptr()) },
        0,
        "{:?}",
        take_error()
    );

    let fragment = open_fragment(fixture.dataset, ids[0]);
    let schema = projected_schema(fragment, &["a", "b"]);
    let writer = new_writer(fragment, schema);
    write_overlay(
        writer,
        &overlay_batch(
            vec![row_address(ids[0], 0), row_address(ids[0], 2)],
            Some(vec![Some(111), None]),
            Some(vec![Some(211), Some(213)]),
        ),
    );
    write_overlay(
        writer,
        &overlay_batch(
            vec![row_address(ids[0], 4)],
            Some(vec![Some(113)]),
            Some(vec![Some(215)]),
        ),
    );
    let group = finish(writer);
    assert!(!group.is_null());

    let mut overlay = ptr::null_mut();
    assert_eq!(
        unsafe { lance_data_overlay_group_overlay_at(group, 0, &mut overlay) },
        0,
        "{:?}",
        take_error()
    );
    let mut kind = -1;
    assert_eq!(
        unsafe { lance_data_overlay_file_coverage_kind(overlay, &mut kind) },
        0
    );
    assert_eq!(kind, 0);
    let mut committed_version = 1u64;
    assert_eq!(
        unsafe { lance_data_overlay_file_committed_version(overlay, &mut committed_version) },
        0
    );
    assert_eq!(committed_version, 0);
    let data_file = unsafe { lance_data_overlay_file_data_file(overlay) };
    assert!(!data_file.is_null(), "{:?}", take_error());
    assert!(unsafe { lance_data_file_field_count(data_file) } > 0);
    let path = unsafe { lance_data_file_path(data_file) };
    assert!(!path.is_null());
    assert!(!unsafe { CStr::from_ptr(path) }.to_bytes().is_empty());
    unsafe { lance_data_file_close(data_file) };

    let bitmaps = file_bitmaps(overlay);
    assert_eq!(bitmaps.len(), 1);
    let pointers: Vec<*const u8> = bitmaps.iter().map(|bitmap| bitmap.as_ptr()).collect();
    let lengths: Vec<usize> = bitmaps.iter().map(|bitmap| bitmap.len()).collect();
    let rebuilt = unsafe {
        lance_data_overlay_file_new(
            lance_data_overlay_file_data_file(overlay),
            0,
            pointers.as_ptr(),
            lengths.as_ptr(),
            pointers.len(),
            42,
        )
    };
    assert!(!rebuilt.is_null(), "{:?}", take_error());
    let mut version = 0u64;
    assert_eq!(
        unsafe { lance_data_overlay_file_committed_version(rebuilt, &mut version) },
        0
    );
    assert_eq!(version, 42);
    let cloned = unsafe { lance_data_overlay_file_clone(rebuilt) };
    assert!(!cloned.is_null());
    assert_eq!(
        unsafe { lance_data_overlay_file_committed_version(cloned, &mut version) },
        0
    );
    assert_eq!(version, 42);
    unsafe {
        lance_data_overlay_file_close(overlay);
        lance_data_overlay_file_close(rebuilt);
        lance_data_overlay_file_close(cloned);
        lance_data_overlay_group_close(group);
    }
    unsafe {
        lance_schema_close(schema);
        lance_fragment_close(fragment);
    }
}

#[test]
fn data_overlay_shared_and_per_field_commit_and_stale_conflict() {
    let fixture = Fixture::new();
    let native = lance_c::runtime::block_on(Dataset::open(&fixture.uri)).unwrap();
    assert_eq!(native.version().version, 2);
    let mut ids = vec![0u64; 2];
    assert_eq!(
        unsafe { lance_dataset_fragment_ids(fixture.dataset, ids.as_mut_ptr()) },
        0
    );
    let base = read_native_batch(&native);
    let base_rowids: Vec<u64> = (1..=20)
        .map(|id| row_values(&base, id).map(|values| values.2).unwrap_or(0))
        .collect();

    let fragment0 = open_fragment(fixture.dataset, ids[0]);
    let schema0 = projected_schema(fragment0, &["a", "b"]);
    let writer0 = new_writer(fragment0, schema0);
    let a_values = vec![Some(111), None, Some(113)];
    write_overlay(
        writer0,
        &overlay_batch(
            vec![
                row_address(ids[0], 0),
                row_address(ids[0], 2),
                row_address(ids[0], 4),
            ],
            Some(a_values),
            Some(vec![Some(211), Some(213), Some(215)]),
        ),
    );
    let group0 = finish(writer0);
    assert!(!group0.is_null());
    let mut kind = -1;
    let mut overlay0 = ptr::null_mut();
    assert_eq!(
        unsafe { lance_data_overlay_group_overlay_at(group0, 0, &mut overlay0) },
        0
    );
    assert_eq!(
        unsafe { lance_data_overlay_file_coverage_kind(overlay0, &mut kind) },
        0
    );
    assert_eq!(kind, 0);
    unsafe { lance_data_overlay_file_close(overlay0) };

    let fragment1 = open_fragment(fixture.dataset, ids[1]);
    let schema1 = projected_schema(fragment1, &["a", "b"]);
    let writer1 = new_writer(fragment1, schema1);
    write_overlay(
        writer1,
        &overlay_batch(
            vec![row_address(ids[1], 1), row_address(ids[1], 3)],
            Some(vec![Some(1000), Some(1001)]),
            None,
        ),
    );
    write_overlay(
        writer1,
        &overlay_batch(
            vec![row_address(ids[1], 6), row_address(ids[1], 8)],
            None,
            Some(vec![Some(2000), Some(2001)]),
        ),
    );
    let group1 = finish(writer1);
    assert!(!group1.is_null());
    let mut overlay1 = ptr::null_mut();
    assert_eq!(
        unsafe { lance_data_overlay_group_overlay_at(group1, 0, &mut overlay1) },
        0
    );
    assert_eq!(
        unsafe { lance_data_overlay_file_coverage_kind(overlay1, &mut kind) },
        0
    );
    assert_eq!(kind, 1);
    let mut bitmap_count = 0u64;
    assert_eq!(
        unsafe { lance_data_overlay_file_bitmap_count(overlay1, &mut bitmap_count) },
        0
    );
    assert_eq!(bitmap_count, 2);
    unsafe { lance_data_overlay_file_close(overlay1) };

    let operation = unsafe {
        lance_operation_data_overlay_new(
            [group0, group1].as_ptr() as *const *const LanceDataOverlayGroup,
            2,
        )
    };
    assert!(!operation.is_null(), "{:?}", take_error());
    let mut op_kind = LanceOperationKind::Unknown;
    assert_eq!(unsafe { lance_operation_kind(operation, &mut op_kind) }, 0);
    assert_eq!(op_kind, LanceOperationKind::DataOverlay);
    let groups = operation_groups(operation);
    assert_eq!(groups.len(), 2);
    let mut cloned_fragment = 0u64;
    assert_eq!(
        unsafe { lance_data_overlay_group_fragment_id(groups[1], &mut cloned_fragment) },
        0
    );
    assert_eq!(cloned_fragment, ids[1]);
    unsafe {
        lance_operation_close(operation);
        for group in groups {
            lance_data_overlay_group_close(group);
        }
    }
    unsafe {
        lance_data_overlay_group_close(group0);
        lance_data_overlay_group_close(group1);
    }

    let (committed, _) = commit(&fixture.uri, 2, &[group0, group1]);
    if committed.is_null() {
        panic!("commit failed: {:?}", take_error());
    }
    let committed_native = lance_c::runtime::block_on(Dataset::open(&fixture.uri)).unwrap();
    assert_eq!(committed_native.version().version, 3);
    let expected = vec![
        (1, Some(111), Some(211)),
        (2, Some(20), Some(200)),
        (3, None, Some(213)),
        (4, Some(40), Some(400)),
        (5, Some(113), Some(215)),
        (6, Some(60), Some(600)),
        (7, Some(70), Some(700)),
        (8, Some(80), Some(800)),
        (9, Some(90), Some(900)),
    ];
    let rowids: Vec<u64> = expected
        .iter()
        .map(|(id, _, _)| base_rowids[(*id - 1) as usize])
        .collect();
    assert_values(&committed_native, &expected, &rowids);
    let fragment1_expected = vec![
        (11, Some(110), Some(1100)),
        (12, Some(1000), Some(1200)),
        (13, Some(130), Some(1300)),
        (14, Some(1001), Some(1400)),
        (15, Some(150), Some(1500)),
        (16, Some(160), Some(1600)),
        (17, Some(170), Some(2000)),
        (18, Some(180), Some(1800)),
        (19, Some(190), Some(2001)),
        (20, Some(200), Some(2000)),
    ];
    let rowids1: Vec<u64> = fragment1_expected
        .iter()
        .map(|(id, _, _)| base_rowids[(*id - 1) as usize])
        .collect();
    assert_values(&committed_native, &fragment1_expected, &rowids1);
    let all = read_native_batch(&committed_native);
    let ids_column = all
        .column_by_name("id")
        .unwrap()
        .as_any()
        .downcast_ref::<Int32Array>()
        .unwrap();
    assert!(!(0..all.num_rows()).any(|index| ids_column.value(index) == 10));

    let transaction = lance_c::runtime::block_on(committed_native.read_transaction_by_version(3))
        .unwrap()
        .unwrap();
    match transaction.operation {
        Operation::DataOverlay { groups } => {
            assert_eq!(groups.len(), 2);
            assert_eq!(groups[0].overlays[0].committed_version, 3);
            assert_eq!(groups[1].overlays[0].committed_version, 3);
            assert_eq!(groups[0].fragment_id, ids[0]);
            assert_eq!(groups[1].fragment_id, ids[1]);
        }
        other => panic!("unexpected history operation: {other:?}"),
    }
    unsafe { lance_dataset_close(committed) };

    let mut empty_writer = ptr::null_mut();
    let a_only = projected_schema(fragment1, &["a"]);
    assert_eq!(
        unsafe { lance_fragment_overlay_writer_new(fragment1, a_only, &mut empty_writer) },
        0
    );
    let empty = finish(empty_writer);
    assert!(empty.is_null());
    unsafe {
        lance_schema_close(a_only);
        lance_fragment_close(fragment1);
        lance_fragment_close(fragment0);
        lance_schema_close(schema0);
        lance_schema_close(schema1);
    }

    let (second, _) = {
        let mut second_writer_schema = ptr::null_mut();
        let c_uri = c_string(&fixture.uri);
        let reopened = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 0) };
        assert!(!reopened.is_null());
        let second_fragment = unsafe { lance_dataset_open_fragment(reopened, ids[0]) };
        assert!(!second_fragment.is_null());
        assert_eq!(
            unsafe { lance_fragment_schema_clone(second_fragment, &mut second_writer_schema) },
            0
        );
        let mut projected = ptr::null_mut();
        let column = c_string("a");
        let pointer = column.as_ptr();
        assert_eq!(
            unsafe { lance_schema_project(second_writer_schema, &pointer, 1, &mut projected) },
            0
        );
        let writer = new_writer(second_fragment, projected);
        write_overlay(
            writer,
            &overlay_batch(vec![row_address(ids[0], 0)], Some(vec![Some(9999)]), None),
        );
        let group = finish(writer);
        let (committed, code) = commit(&fixture.uri, 3, &[group]);
        assert!(
            code == LanceErrorCode::Ok,
            "second commit failed: {:?}",
            take_error()
        );
        unsafe {
            lance_data_overlay_group_close(group);
            lance_schema_close(projected);
            lance_schema_close(second_writer_schema);
            lance_fragment_close(second_fragment);
            lance_dataset_close(reopened);
        }
        (committed, code)
    };
    assert!(!second.is_null());
    let after_second = lance_c::runtime::block_on(Dataset::open(&fixture.uri)).unwrap();
    assert_eq!(after_second.version().version, 4);
    let values = row_values(&read_native_batch(&after_second), 1).unwrap();
    assert_eq!(values.0, Some(9999));
    assert_eq!(values.1, Some(211));

    let old = lance_c::runtime::block_on(Dataset::open(&fixture.uri)).unwrap();
    let c_uri = c_string(&fixture.uri);
    let old_handle = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), 1) };
    assert!(!old_handle.is_null());
    let old_fragment = unsafe { lance_dataset_open_fragment(old_handle, ids[0]) };
    assert!(!old_fragment.is_null());
    let mut old_schema = ptr::null_mut();
    assert_eq!(
        unsafe { lance_fragment_schema_clone(old_fragment, &mut old_schema) },
        0
    );
    let mut old_projected = ptr::null_mut();
    let column = c_string("a");
    let pointer = column.as_ptr();
    assert_eq!(
        unsafe { lance_schema_project(old_schema, &pointer, 1, &mut old_projected) },
        0
    );
    let stale_writer = new_writer(old_fragment, old_projected);
    write_overlay(
        stale_writer,
        &overlay_batch(vec![row_address(ids[0], 1)], Some(vec![Some(777)]), None),
    );
    let stale_group = finish(stale_writer);
    let (stale_committed, stale_code) = commit(&fixture.uri, 1, &[stale_group]);
    assert!(stale_committed.is_null());
    assert_eq!(
        stale_code,
        LanceErrorCode::CommitConflict,
        "{:?}",
        take_error()
    );
    unsafe {
        lance_data_overlay_group_close(stale_group);
        lance_schema_close(old_projected);
        lance_schema_close(old_schema);
        lance_fragment_close(old_fragment);
        lance_dataset_close(old_handle);
        lance_dataset_close(second);
    }
    let final_native = lance_c::runtime::block_on(Dataset::open(&fixture.uri)).unwrap();
    assert_eq!(final_native.version().version, 4);
    let final_values = row_values(&read_native_batch(&final_native), 1).unwrap();
    assert_eq!(final_values.0, Some(9999));
    assert_eq!(final_values.1, Some(211));
    assert_eq!(old.version().version, 3);
}

#[test]
fn data_overlay_validation_errors() {
    assert!(
        unsafe { lance_data_overlay_file_new(ptr::null(), 0, ptr::null(), ptr::null(), 0, 0) }
            .is_null()
    );
    assert!(error_is(
        lance_last_error_code(),
        LanceErrorCode::InvalidArgument
    ));

    let fixture = Fixture::new();
    let fragment = open_fragment(fixture.dataset, 0);
    let schema = projected_schema(fragment, &["a"]);
    let writer = new_writer(fragment, schema);
    write_overlay(writer, &overlay_batch(vec![0], Some(vec![Some(1)]), None));
    let group = finish(writer);
    assert!(!group.is_null());
    let mut overlay = ptr::null_mut();
    assert_eq!(
        unsafe { lance_data_overlay_group_overlay_at(group, 0, &mut overlay) },
        0
    );

    let data_file = unsafe { lance_data_overlay_file_data_file(overlay) };
    assert!(!data_file.is_null());
    let bitmap = [0xffu8; 5];
    let pointer = bitmap.as_ptr();
    let len = bitmap.len();
    assert!(unsafe { lance_data_overlay_file_new(data_file, 2, &pointer, &len, 1, 0) }.is_null());
    assert!(error_is(
        lance_last_error_code(),
        LanceErrorCode::InvalidArgument
    ));
    assert!(take_error().1.contains("coverage_kind"));

    assert!(
        unsafe { lance_data_overlay_file_new(data_file, 1, ptr::null(), ptr::null(), 0, 0) }
            .is_null()
    );
    assert!(take_error().1.contains("per-field"));

    assert!(unsafe { lance_data_overlay_file_new(data_file, 0, &pointer, &len, 2, 0) }.is_null());
    assert!(take_error().1.contains("shared"));

    let invalid = [0xffu8; 1];
    let invalid_pointer = invalid.as_ptr();
    let invalid_len = invalid.len();
    assert!(
        unsafe { lance_data_overlay_file_new(data_file, 0, &invalid_pointer, &invalid_len, 1, 0) }
            .is_null()
    );
    assert!(take_error().1.contains("Roaring"));

    let mut count = 0u64;
    assert_eq!(
        unsafe { lance_data_overlay_file_bitmap_count(overlay, &mut count) },
        0
    );
    assert!(count > 0);
    let mut bytes = ptr::null_mut();
    let mut len = 0usize;
    assert_eq!(
        unsafe { lance_data_overlay_file_bitmap_at(overlay, count as usize, &mut bytes, &mut len) },
        -1
    );
    assert!(error_is(
        lance_last_error_code(),
        LanceErrorCode::InvalidArgument
    ));

    let mut group_out = ptr::null_mut();
    assert!(unsafe { lance_data_overlay_group_new(1, ptr::null(), 1) }.is_null());
    assert!(error_is(
        lance_last_error_code(),
        LanceErrorCode::InvalidArgument
    ));

    let null_group: *mut LanceDataOverlayGroup = ptr::null_mut();
    assert!(
        unsafe {
            lance_operation_data_overlay_new(
                (&null_group as *const *mut LanceDataOverlayGroup)
                    as *const *const LanceDataOverlayGroup,
                1,
            )
        }
        .is_null()
    );
    assert!(error_is(
        lance_last_error_code(),
        LanceErrorCode::InvalidArgument
    ));

    let mut writer_out = ptr::null_mut();
    assert_eq!(
        unsafe { lance_fragment_overlay_writer_new(ptr::null(), schema, &mut writer_out) },
        -1
    );
    assert!(error_is(
        lance_last_error_code(),
        LanceErrorCode::InvalidArgument
    ));

    let mut null_writer = ptr::null_mut();
    assert_eq!(
        unsafe { lance_fragment_overlay_writer_new(fragment, ptr::null(), &mut null_writer) },
        -1
    );
    assert!(error_is(
        lance_last_error_code(),
        LanceErrorCode::InvalidArgument
    ));
    assert!(null_writer.is_null());

    assert_eq!(
        unsafe { lance_fragment_overlay_writer_new(fragment, schema, ptr::null_mut()) },
        -1
    );
    assert!(error_is(
        lance_last_error_code(),
        LanceErrorCode::InvalidArgument
    ));

    let batch = overlay_batch(vec![row_address(0, 0)], Some(vec![Some(1)]), None);
    let imported = import_batch(&batch);
    let active_writer = new_writer(fragment, schema);
    assert_eq!(
        unsafe { lance_fragment_overlay_writer_write_batch(active_writer, ptr::null()) },
        -1
    );
    assert!(error_is(
        lance_last_error_code(),
        LanceErrorCode::InvalidArgument
    ));
    assert_eq!(
        unsafe { lance_fragment_overlay_writer_write_batch(active_writer, imported) },
        0
    );
    unsafe { lance_batch_free(imported) };
    assert_eq!(
        unsafe { lance_fragment_overlay_writer_finish(active_writer, ptr::null_mut()) },
        0
    );
    assert_eq!(
        unsafe { lance_fragment_overlay_writer_finish(ptr::null_mut(), &mut group_out) },
        -1
    );
    assert!(error_is(
        lance_last_error_code(),
        LanceErrorCode::InvalidArgument
    ));
    unsafe {
        lance_fragment_overlay_writer_close(ptr::null_mut());
        lance_fragment_overlay_writer_abort(ptr::null_mut());
        lance_fragment_overlay_writer_close(active_writer);
        lance_data_file_close(data_file);
        lance_data_overlay_file_close(overlay);
        lance_data_overlay_group_close(group);
        lance_schema_close(schema);
        lance_fragment_close(fragment);
    }
}
