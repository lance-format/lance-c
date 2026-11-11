// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Native fragment mutation and staging acceptance.

use std::collections::HashMap;
use std::ffi::{CStr, CString, c_void};
use std::ptr;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema};
use arrow::ffi_stream::FFI_ArrowArrayStream;
use arrow::record_batch::RecordBatchIterator;
use arrow_array::{Array, Int32Array, Int64Array, RecordBatch, StringArray, StructArray};
use arrow_schema::{DataType, Field, Fields, Schema};
use lance::Dataset;
use lance::dataset::fragment::FileFragment;
use lance::dataset::{NewColumnTransform, WriteMode, WriteParams};
use lance_c::*;
use roaring::RoaringBitmap;

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

fn mutation_schema() -> Arc<Schema> {
    let mut root_metadata = HashMap::new();
    root_metadata.insert("fixture".to_owned(), "fragment-mutation".to_owned());

    let mut id_metadata = HashMap::new();
    id_metadata.insert("role".to_owned(), "id".to_owned());
    let mut value_metadata = HashMap::new();
    value_metadata.insert("role".to_owned(), "value".to_owned());
    let mut profile_metadata = HashMap::new();
    profile_metadata.insert("role".to_owned(), "nested".to_owned());

    let profile_fields = Fields::from(vec![
        Field::new("score", DataType::Int32, true)
            .with_metadata(HashMap::from([("nested".to_owned(), "score".to_owned())])),
        Field::new("tag", DataType::Utf8, true),
    ]);
    Arc::new(Schema::new_with_metadata(
        vec![
            Field::new("id", DataType::Int64, false).with_metadata(id_metadata),
            Field::new("value", DataType::Int32, true).with_metadata(value_metadata),
            Field::new("stale", DataType::Utf8, true),
            Field::new("profile", DataType::Struct(profile_fields), true)
                .with_metadata(profile_metadata),
        ],
        root_metadata,
    ))
}

fn fixture_batch(ids: &[i64]) -> RecordBatch {
    let schema = mutation_schema();
    let values = ids
        .iter()
        .map(|id| Some(*id as i32 * 10))
        .collect::<Vec<_>>();
    let stale = StringArray::from(vec!["stale"; ids.len()]);
    let scores = Arc::new(Int32Array::from(
        ids.iter()
            .map(|id| Some(*id as i32 * 100))
            .collect::<Vec<_>>(),
    ));
    let tags = Arc::new(StringArray::from(
        ids.iter().map(|id| format!("tag-{id}")).collect::<Vec<_>>(),
    ));
    let profile_fields = match schema.field_with_name("profile").unwrap().data_type() {
        DataType::Struct(fields) => fields.clone(),
        other => panic!("profile has unexpected type {other:?}"),
    };
    let profile = Arc::new(StructArray::new(profile_fields, vec![scores, tags], None));
    RecordBatch::try_new(
        schema,
        vec![
            Arc::new(Int64Array::from(ids.to_vec())),
            Arc::new(Int32Array::from(values)),
            Arc::new(stale),
            profile,
        ],
    )
    .unwrap()
}

fn create_mutation_fixture(name: &str) -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join(name).to_str().unwrap().to_owned();
    let schema = mutation_schema();
    lance_c::runtime::block_on(async {
        let mut dataset = Dataset::write(
            RecordBatchIterator::new(vec![Ok(fixture_batch(&[1, 2, 3, 4]))], schema.clone()),
            &uri,
            Some(WriteParams {
                mode: WriteMode::Create,
                max_rows_per_file: 4,
                enable_stable_row_ids: true,
                ..Default::default()
            }),
        )
        .await
        .unwrap();
        dataset
            .append(
                RecordBatchIterator::new(vec![Ok(fixture_batch(&[5, 6, 7, 8]))], schema.clone()),
                None,
            )
            .await
            .unwrap();
        dataset
            .append(
                RecordBatchIterator::new(vec![Ok(fixture_batch(&[9, 10, 11, 12]))], schema),
                None,
            )
            .await
            .unwrap();
        dataset.drop_columns(&["stale"]).await.unwrap();
        assert_eq!(
            dataset
                .delete("id IN (2, 10)")
                .await
                .unwrap()
                .num_deleted_rows,
            2
        );
        assert_eq!(dataset.version().version, 5);
        assert_eq!(dataset.get_fragments().len(), 3);
    });
    (tmp, uri)
}

fn open_dataset(uri: &str) -> *mut LanceDataset {
    let uri = c_string(uri);
    let dataset = unsafe { lance_dataset_open(uri.as_ptr(), ptr::null(), 0) };
    assert!(
        !dataset.is_null(),
        "dataset open failed: {:?}",
        take_error()
    );
    dataset
}

fn open_bound_fragment(uri: &str, fragment_id: u64) -> *mut LanceFragment {
    let dataset = open_dataset(uri);
    let fragment = unsafe { lance_dataset_open_fragment(dataset, fragment_id) };
    assert!(
        !fragment.is_null(),
        "fragment open failed: {:?}",
        take_error()
    );
    unsafe { lance_dataset_close(dataset) };
    fragment
}

async fn native_fragment(uri: &str, fragment_id: u64) -> FileFragment {
    let dataset = Dataset::open(uri).await.unwrap();
    dataset
        .get_fragment(usize::try_from(fragment_id).unwrap())
        .unwrap()
}

fn schema_field_id(schema: *const LanceSchema, name: &str) -> i32 {
    let name = c_string(name);
    let mut field_id = -1;
    assert_eq!(
        unsafe { lance_schema_field_id(schema, name.as_ptr(), &mut field_id) },
        0,
        "schema field lookup failed: {:?}",
        take_error()
    );
    field_id
}

fn schema_arrow(schema: *const LanceSchema) -> Schema {
    let mut arrow = FFI_ArrowSchema::empty();
    assert_eq!(unsafe { lance_schema_to_arrow(schema, &mut arrow) }, 0);
    let owned = Schema::try_from(&arrow).unwrap();
    drop(arrow);
    owned
}

fn fragment_metadata(fragment: *const LanceFragment) -> serde_json::Value {
    let raw = unsafe { lance_fragment_metadata_json(fragment) };
    assert!(!raw.is_null(), "metadata JSON failed: {:?}", take_error());
    let text = unsafe { CStr::from_ptr(raw) }
        .to_string_lossy()
        .into_owned();
    unsafe { lance_free_string(raw) };
    serde_json::from_str(&text).unwrap()
}

fn fragment_read_version(fragment: *const LanceFragment) -> u64 {
    let mut version = 0;
    assert_eq!(
        unsafe { lance_fragment_read_version(fragment, &mut version) },
        0,
        "read_version failed: {:?}",
        take_error()
    );
    version
}

fn fragment_physical_rows(fragment: *const LanceFragment) -> u64 {
    let mut rows = 0;
    assert_eq!(
        unsafe { lance_fragment_physical_rows(fragment, &mut rows) },
        0,
        "physical_rows failed: {:?}",
        take_error()
    );
    rows
}

fn fragment_logical_rows(fragment: *const LanceFragment) -> u64 {
    let mut rows = 0;
    assert_eq!(
        unsafe { lance_fragment_logical_rows(fragment, &mut rows) },
        0,
        "logical_rows failed: {:?}",
        take_error()
    );
    rows
}

fn arrow_stream(schema: Arc<Schema>, batches: Vec<RecordBatch>) -> FFI_ArrowArrayStream {
    FFI_ArrowArrayStream::new(Box::new(RecordBatchIterator::new(
        batches.into_iter().map(Ok),
        schema,
    )))
}

fn one_column_stream(name: &str, values: Vec<Option<i32>>) -> FFI_ArrowArrayStream {
    let field = Field::new(name, DataType::Int32, true);
    let schema = Arc::new(Schema::new(vec![field]));
    let batch =
        RecordBatch::try_new(schema.clone(), vec![Arc::new(Int32Array::from(values))]).unwrap();
    arrow_stream(schema, vec![batch])
}

fn join_stream(ids: Vec<i64>, column: &str, values: Vec<Option<i32>>) -> FFI_ArrowArrayStream {
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int64, false),
        Field::new(column, DataType::Int32, true),
    ]));
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int64Array::from(ids)),
            Arc::new(Int32Array::from(values)),
        ],
    )
    .unwrap();
    arrow_stream(schema, vec![batch])
}

fn take_stream_consumed(stream: &FFI_ArrowArrayStream) {
    assert!(
        stream.release.is_none(),
        "non-NULL Arrow stream was not consumed"
    );
}

#[test]
fn test_fragment_add_columns_variants_preserve_native_schema() {
    let (_tmp, uri) = create_mutation_fixture("add_columns_variants");
    let (_oracle_tmp, oracle_uri) = create_mutation_fixture("add_columns_variants_oracle");
    let fragment = open_bound_fragment(&uri, 0);
    let before_metadata = fragment_metadata(fragment);
    assert_eq!(fragment_physical_rows(fragment), 4);
    assert_eq!(fragment_logical_rows(fragment), 3);

    let sql_name = c_string("sql_value");
    let sql_expression = c_string("value * 2 + 1");
    let sql_columns = [LanceSqlColumn {
        name: sql_name.as_ptr(),
        expression: sql_expression.as_ptr(),
    }];
    let batch_size = 2u32;
    let mut sql_fragment = ptr::null_mut();
    let mut sql_schema = ptr::null_mut();
    assert_eq!(
        unsafe {
            lance_fragment_add_columns_sql(
                fragment,
                sql_columns.as_ptr(),
                sql_columns.len(),
                &batch_size,
                &mut sql_fragment,
                &mut sql_schema,
            )
        },
        0,
        "SQL add_columns failed: {:?}",
        take_error()
    );
    assert!(!sql_fragment.is_null() && !sql_schema.is_null());
    let mut sql_bound = true;
    assert_eq!(
        unsafe { lance_fragment_is_bound(sql_fragment, &mut sql_bound) },
        0
    );
    assert!(!sql_bound, "add_columns output must be detached metadata");
    let sql_field_id = schema_field_id(sql_schema, "sql_value");
    assert!(sql_field_id >= 6);
    assert_eq!(schema_field_id(sql_schema, "profile.score"), 4);
    assert_eq!(schema_field_id(sql_schema, "profile.tag"), 5);
    let sql_arrow = schema_arrow(sql_schema);
    assert_eq!(
        sql_arrow.metadata().get("fixture").map(String::as_str),
        Some("fragment-mutation")
    );
    let native = lance_c::runtime::block_on(async {
        let dataset = Dataset::open(&oracle_uri).await.unwrap();
        let native = dataset.get_fragment(0).unwrap();
        native
            .add_columns(
                NewColumnTransform::SqlExpressions(vec![(
                    "sql_value".to_owned(),
                    "value * 2 + 1".to_owned(),
                )]),
                None,
                Some(2),
            )
            .await
            .unwrap()
    });
    assert_eq!(native.1.field_id("sql_value").unwrap() as i32, sql_field_id);
    assert_eq!(native.1.field_id("profile.tag").unwrap() as i32, 5);

    let mut reader_fragment = ptr::null_mut();
    let mut reader_schema = ptr::null_mut();
    let mut reader_stream =
        one_column_stream("reader_value", vec![Some(100), Some(300), Some(400)]);
    assert_eq!(
        unsafe {
            lance_fragment_add_columns_stream(
                fragment,
                &mut reader_stream,
                ptr::null(),
                &mut reader_fragment,
                &mut reader_schema,
            )
        },
        0,
        "Reader add_columns failed: {:?}",
        take_error()
    );
    take_stream_consumed(&reader_stream);
    assert!(schema_field_id(reader_schema, "reader_value") > 5);

    let null_field = Field::new("null_value", DataType::Int32, true);
    let null_arrow_schema = FFI_ArrowSchema::try_from(&Schema::new(vec![null_field])).unwrap();
    let mut null_fragment = ptr::null_mut();
    let mut null_schema = ptr::null_mut();
    assert_eq!(
        unsafe {
            lance_fragment_add_columns_nulls(
                fragment,
                &null_arrow_schema,
                &mut null_fragment,
                &mut null_schema,
            )
        },
        0,
        "AllNulls add_columns failed: {:?}",
        take_error()
    );
    assert!(schema_field_id(null_schema, "null_value") > 5);

    struct CallbackState {
        output_schema: Arc<Schema>,
        dataset: *const LanceDataset,
        calls: AtomicUsize,
        independent_count: AtomicUsize,
    }

    unsafe extern "C" fn mapper(
        callback_ctx: *mut c_void,
        input: *const FFI_ArrowArray,
        _input_schema: *const FFI_ArrowSchema,
        output: *mut FFI_ArrowArray,
        _error: *mut LanceCallbackError,
    ) -> i32 {
        let state = unsafe { &*callback_ctx.cast::<CallbackState>() };
        state.calls.fetch_add(1, Ordering::SeqCst);
        let count = unsafe { lance_dataset_count_rows(state.dataset) };
        if count != 10 {
            return -1;
        }
        state.independent_count.fetch_add(1, Ordering::SeqCst);
        let rows = unsafe { (*input).length as usize };
        let batch = RecordBatch::try_new(
            state.output_schema.clone(),
            vec![Arc::new(Int64Array::from(vec![77i64; rows]))],
        )
        .unwrap();
        let struct_array: StructArray = batch.into();
        unsafe { ptr::write(output, FFI_ArrowArray::new(&struct_array.into_data())) };
        0
    }

    let udf_schema = Arc::new(Schema::new(vec![Field::new(
        "udf_value",
        DataType::Int64,
        true,
    )]));
    let udf_ffi_schema = FFI_ArrowSchema::try_from(udf_schema.as_ref()).unwrap();
    let independent = open_dataset(&uri);
    let state = CallbackState {
        output_schema: udf_schema,
        dataset: independent,
        calls: AtomicUsize::new(0),
        independent_count: AtomicUsize::new(0),
    };
    let read_column = c_string("id");
    let read_columns = [read_column.as_ptr(), ptr::null()];
    let udf_batch_size = 2u32;
    let udf = LanceBatchUDF {
        mapper: Some(mapper),
        mapper_ctx: (&state as *const CallbackState).cast_mut().cast(),
        output_schema: &udf_ffi_schema,
        checkpoint_store: ptr::null(),
    };
    let mut udf_fragment = ptr::null_mut();
    let mut udf_schema_out = ptr::null_mut();
    let udf_status = unsafe {
        lance_fragment_add_columns_batch_udf(
            fragment,
            &udf,
            read_columns.as_ptr(),
            &udf_batch_size,
            &mut udf_fragment,
            &mut udf_schema_out,
        )
    };
    if udf_status != 0 {
        eprintln!("BatchUDF add_columns failed: {:?}", take_error());
    }
    assert_eq!(udf_status, 0);
    assert!(schema_field_id(udf_schema_out, "udf_value") > 5);
    assert!(state.calls.load(Ordering::SeqCst) > 0);
    assert_eq!(
        state.calls.load(Ordering::SeqCst),
        state.independent_count.load(Ordering::SeqCst)
    );

    assert_eq!(fragment_metadata(fragment), before_metadata);
    assert_eq!(fragment_read_version(fragment), 5);
    unsafe {
        lance_fragment_close(udf_fragment);
        lance_schema_close(udf_schema_out);
        lance_fragment_close(null_fragment);
        lance_schema_close(null_schema);
        lance_fragment_close(reader_fragment);
        lance_schema_close(reader_schema);
        lance_fragment_close(sql_fragment);
        lance_schema_close(sql_schema);
        lance_fragment_close(fragment);
        lance_dataset_close(independent);
    }
}

#[test]
fn test_fragment_merge_and_update_match_native_oracle() {
    let (_tmp, uri) = create_mutation_fixture("merge_update");
    let (_oracle_tmp, oracle_uri) = create_mutation_fixture("merge_update_oracle");
    let fragment = open_bound_fragment(&uri, 0);
    let source_metadata = fragment_metadata(fragment);
    let source_version = fragment_read_version(fragment);

    let native_merge = lance_c::runtime::block_on(async {
        let mut native = native_fragment(&oracle_uri, 0).await;
        let schema = Arc::new(Schema::new(vec![
            Field::new("id", DataType::Int64, false),
            Field::new("merged", DataType::Int32, true),
        ]));
        let batch = RecordBatch::try_new(
            schema.clone(),
            vec![
                Arc::new(Int64Array::from(vec![1, 2, 3, 4])),
                Arc::new(Int32Array::from(vec![
                    Some(11),
                    Some(22),
                    Some(33),
                    Some(44),
                ])),
            ],
        )
        .unwrap();
        native
            .merge_columns(
                RecordBatchIterator::new(vec![Ok(batch)], schema),
                "id",
                "id",
            )
            .await
            .unwrap()
    });

    let mut merge_stream = join_stream(
        vec![1, 2, 3, 4],
        "merged",
        vec![Some(11), Some(22), Some(33), Some(44)],
    );
    let mut merged_fragment = ptr::null_mut();
    let mut merged_schema = ptr::null_mut();
    let left_on = c_string("id");
    let right_on = c_string("id");
    assert_eq!(
        unsafe {
            lance_fragment_merge_columns(
                fragment,
                &mut merge_stream,
                left_on.as_ptr(),
                right_on.as_ptr(),
                &mut merged_fragment,
                &mut merged_schema,
            )
        },
        0,
        "merge_columns failed: {:?}",
        take_error()
    );
    take_stream_consumed(&merge_stream);
    assert_eq!(
        schema_field_id(merged_schema, "merged"),
        native_merge.1.field_id("merged").unwrap() as i32
    );
    assert_eq!(schema_field_id(merged_schema, "profile.score"), 4);
    assert_eq!(
        fragment_metadata(merged_fragment)["physical_rows"].as_u64(),
        native_merge.0.physical_rows.map(|rows| rows as u64)
    );
    assert_eq!(
        fragment_metadata(merged_fragment)["files"]
            .as_array()
            .unwrap()
            .len(),
        native_merge.0.files.len()
    );

    let native_update = lance_c::runtime::block_on(async {
        let mut native = native_fragment(&oracle_uri, 0).await;
        let schema = Arc::new(Schema::new(vec![
            Field::new("id", DataType::Int64, false),
            Field::new("value", DataType::Int32, true),
        ]));
        let batch = RecordBatch::try_new(
            schema.clone(),
            vec![
                Arc::new(Int64Array::from(vec![1, 3])),
                Arc::new(Int32Array::from(vec![Some(901), Some(903)])),
            ],
        )
        .unwrap();
        native
            .update_columns_with_offsets(
                RecordBatchIterator::new(vec![Ok(batch)], schema),
                "id",
                "id",
            )
            .await
            .unwrap()
    });

    let mut update_stream = join_stream(vec![1, 3], "value", vec![Some(901), Some(903)]);
    let mut update_result = ptr::null_mut();
    assert_eq!(
        unsafe {
            lance_fragment_update_columns(
                fragment,
                &mut update_stream,
                left_on.as_ptr(),
                right_on.as_ptr(),
                &mut update_result,
            )
        },
        0,
        "update_columns failed: {:?}",
        take_error()
    );
    take_stream_consumed(&update_stream);
    assert!(!update_result.is_null());

    let mut field_count = 0_u64;
    assert_eq!(
        unsafe {
            lance_fragment_update_columns_result_fields_modified_count(
                update_result,
                &mut field_count,
            )
        },
        0
    );
    let mut fields = Vec::new();
    for index in 0..field_count as usize {
        let mut field_id = u32::MAX;
        assert_eq!(
            unsafe {
                lance_fragment_update_columns_result_fields_modified_at(
                    update_result,
                    index,
                    &mut field_id,
                )
            },
            0
        );
        fields.push(field_id);
    }
    let mut expected_fields = native_update.fields_modified.clone();
    fields.sort_unstable();
    expected_fields.sort_unstable();
    assert_eq!(fields, expected_fields);

    let mut offset_count = 0_u64;
    assert_eq!(
        unsafe {
            lance_fragment_update_columns_result_matched_offset_count(
                update_result,
                &mut offset_count,
            )
        },
        0
    );
    let mut offsets = Vec::new();
    for index in 0..offset_count as usize {
        let mut offset = u32::MAX;
        assert_eq!(
            unsafe {
                lance_fragment_update_columns_result_matched_offset_at(
                    update_result,
                    index,
                    &mut offset,
                )
            },
            0
        );
        offsets.push(offset);
    }
    let expected_offsets = native_update.matched_offsets.iter().collect::<Vec<_>>();
    assert_eq!(offsets, expected_offsets);

    let mut serialized = ptr::null_mut();
    let mut serialized_len = 0_usize;
    assert_eq!(
        unsafe {
            lance_fragment_update_columns_result_matched_offsets_serialize(
                update_result,
                &mut serialized,
                &mut serialized_len,
            )
        },
        0
    );
    let bytes = unsafe { std::slice::from_raw_parts(serialized, serialized_len) };
    let bitmap = RoaringBitmap::deserialize_from(bytes).unwrap();
    unsafe { lance_free_bytes(serialized) };
    assert_eq!(
        bitmap.iter().collect::<Vec<_>>(),
        native_update.matched_offsets.iter().collect::<Vec<_>>()
    );
    let mut updated_fragment = ptr::null_mut();
    assert_eq!(
        unsafe {
            lance_fragment_update_columns_result_fragment(update_result, &mut updated_fragment)
        },
        0
    );
    unsafe { lance_fragment_update_columns_result_close(update_result) };
    assert_eq!(
        fragment_metadata(updated_fragment)["physical_rows"].as_u64(),
        native_update.fragment.physical_rows.map(|rows| rows as u64)
    );
    assert_eq!(
        fragment_metadata(updated_fragment)["files"]
            .as_array()
            .unwrap()
            .len(),
        native_update.fragment.files.len()
    );

    assert_eq!(fragment_metadata(fragment), source_metadata);
    assert_eq!(fragment_read_version(fragment), source_version);
    unsafe {
        lance_fragment_close(updated_fragment);
        lance_fragment_close(merged_fragment);
        lance_schema_close(merged_schema);
        lance_fragment_close(fragment);
    }
}

#[test]
fn test_fragment_write_columns_and_offset_deletion_match_native_oracle() {
    let (_tmp, uri) = create_mutation_fixture("write_delete");
    let (_oracle_tmp, oracle_uri) = create_mutation_fixture("write_delete_oracle");
    let fragment = open_bound_fragment(&uri, 0);
    let source_metadata = fragment_metadata(fragment);

    let mut full_schema = ptr::null_mut();
    assert_eq!(
        unsafe { lance_fragment_schema_clone(fragment, &mut full_schema) },
        0
    );
    let value_path = c_string("value");
    let paths = [value_path.as_ptr()];
    let mut value_schema = ptr::null_mut();
    assert_eq!(
        unsafe { lance_schema_project(full_schema, paths.as_ptr(), 1, &mut value_schema) },
        0,
        "project value schema failed: {:?}",
        take_error()
    );

    let mut replacement_stream =
        one_column_stream("value", vec![Some(111), Some(222), Some(333), Some(444)]);
    let mut group = ptr::null_mut();
    assert_eq!(
        unsafe {
            lance_fragment_write_columns(
                fragment,
                value_schema,
                &mut replacement_stream,
                &mut group,
            )
        },
        0,
        "write_columns failed: {:?}",
        take_error()
    );
    take_stream_consumed(&replacement_stream);
    assert!(!group.is_null());
    assert_eq!(
        unsafe { lance_data_replacement_group_fragment_id(group) },
        0
    );
    let mut group_file = ptr::null_mut();
    assert_eq!(
        unsafe { lance_data_replacement_group_file(group, &mut group_file) },
        0
    );
    let mut major = 0_u32;
    let mut minor = 0_u32;
    assert_eq!(
        unsafe { lance_data_file_file_version(group_file, &mut major, &mut minor) },
        0
    );
    assert_eq!((major, minor), (2, 2));

    let native_group = lance_c::runtime::block_on(async {
        let native = native_fragment(&oracle_uri, 0).await;
        let schema = native.dataset().schema().project(&["value"]).unwrap();
        let batch = RecordBatch::try_new(
            Arc::new(Schema::new(vec![Field::new(
                "value",
                DataType::Int32,
                true,
            )])),
            vec![Arc::new(Int32Array::from(vec![
                Some(111),
                Some(222),
                Some(333),
                Some(444),
            ]))],
        )
        .unwrap();
        native
            .write_columns(futures::stream::iter([Ok(batch)]), &schema)
            .await
            .unwrap()
    });
    assert_eq!(native_group.0, 0);
    assert_eq!(native_group.1.file_major_version, major);
    assert_eq!(native_group.1.file_minor_version, minor);
    assert_eq!(
        native_group.1.fields.as_ref(),
        &[schema_field_id(value_schema, "value")]
    );

    let mut explicit_stream =
        one_column_stream("value", vec![Some(511), Some(522), Some(533), Some(544)]);
    let mut explicit_group = ptr::null_mut();
    assert_eq!(
        unsafe {
            lance_fragment_write_column_with_version(
                fragment,
                value_schema,
                &mut explicit_stream,
                LanceFileFormatVersion::V2_2 as i32,
                &mut explicit_group,
            )
        },
        0,
        "explicit write_columns failed: {:?}",
        take_error()
    );
    take_stream_consumed(&explicit_stream);
    let mut explicit_file = ptr::null_mut();
    assert_eq!(
        unsafe { lance_data_replacement_group_file(explicit_group, &mut explicit_file) },
        0
    );
    assert_eq!(
        unsafe { lance_data_file_file_version(explicit_file, &mut major, &mut minor) },
        0
    );
    assert_eq!((major, minor), (2, 2));

    let native_v1_error = lance_c::runtime::block_on(async {
        let native = native_fragment(&oracle_uri, 0).await;
        let schema = native.dataset().schema().project(&["value"]).unwrap();
        let batch = RecordBatch::try_new(
            Arc::new(Schema::new(vec![Field::new(
                "value",
                DataType::Int32,
                true,
            )])),
            vec![Arc::new(Int32Array::from(vec![
                Some(1),
                Some(2),
                Some(3),
                Some(4),
            ]))],
        )
        .unwrap();
        native
            .write_column_with_version(
                futures::stream::iter([Ok(batch)]),
                &schema,
                lance_file::version::ConcreteFileVersion::V1,
            )
            .await
            .unwrap_err()
    });

    let mut v1_stream = one_column_stream("value", vec![Some(1), Some(2), Some(3), Some(4)]);
    let mut sentinel_group = 1usize as *mut LanceDataReplacementGroup;
    let unchanged_group = sentinel_group;
    assert_eq!(
        unsafe {
            lance_fragment_write_column_with_version(
                fragment,
                value_schema,
                &mut v1_stream,
                LanceFileFormatVersion::V1 as i32,
                &mut sentinel_group,
            )
        },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(sentinel_group, unchanged_group);
    take_stream_consumed(&v1_stream);
    let (_, message) = take_error();
    assert_eq!(message, native_v1_error.to_string());

    let second = open_bound_fragment(&uri, 1);
    let native_delete = lance_c::runtime::block_on(async {
        native_fragment(&oracle_uri, 1)
            .await
            .extend_deletions([0, 0])
            .await
    });
    let offsets = [0u32, 0u32];
    let mut staged_second = ptr::null_mut();
    assert_eq!(
        unsafe {
            lance_fragment_delete_rows(second, offsets.as_ptr(), offsets.len(), &mut staged_second)
        },
        0,
        "delete_rows failed: {:?}",
        take_error()
    );
    let native_delete = native_delete.unwrap().unwrap();
    assert_eq!(
        fragment_logical_rows(staged_second),
        lance_c::runtime::block_on(native_delete.count_rows(None)).unwrap() as u64
    );
    assert_eq!(fragment_logical_rows(staged_second), 3);

    let third = open_bound_fragment(&uri, 2);
    let native_full = lance_c::runtime::block_on(async {
        native_fragment(&oracle_uri, 2)
            .await
            .extend_deletions([0, 1, 2, 3])
            .await
    });
    assert!(native_full.unwrap().is_none());
    let all_offsets = [0u32, 1, 2, 3];
    let mut full_output = 1usize as *mut LanceFragment;
    assert_eq!(
        unsafe {
            lance_fragment_delete_rows(
                third,
                all_offsets.as_ptr(),
                all_offsets.len(),
                &mut full_output,
            )
        },
        0
    );
    assert!(full_output.is_null());

    let native_out_of_range = lance_c::runtime::block_on(async {
        native_fragment(&oracle_uri, 1)
            .await
            .extend_deletions([100])
            .await
    });
    let invalid_offsets = [100u32];
    let mut invalid_output = 1usize as *mut LanceFragment;
    let rc = unsafe {
        lance_fragment_delete_rows(
            second,
            invalid_offsets.as_ptr(),
            invalid_offsets.len(),
            &mut invalid_output,
        )
    };
    assert_eq!(rc == 0, native_out_of_range.is_ok());
    if rc != 0 {
        assert_eq!(invalid_output, 1usize as *mut LanceFragment);
        let (_, message) = take_error();
        assert_eq!(
            message,
            native_out_of_range.unwrap_err().to_string(),
            "binding must expose the direct native error"
        );
    }

    assert_eq!(fragment_metadata(fragment), source_metadata);
    unsafe {
        if !invalid_output.is_null() && invalid_output != 1usize as *mut LanceFragment {
            lance_fragment_close(invalid_output);
        }
        lance_fragment_close(staged_second);
        lance_fragment_close(second);
        lance_fragment_close(third);
        lance_data_file_close(explicit_file);
        lance_data_file_close(group_file);
        lance_data_replacement_group_close(explicit_group);
        lance_data_replacement_group_close(group);
        lance_schema_close(value_schema);
        lance_schema_close(full_schema);
        lance_fragment_close(fragment);
    }
}

#[test]
fn test_fragment_mutation_errors_preserve_outputs_and_consume_streams_once() {
    let (_tmp, uri) = create_mutation_fixture("mutation_errors");
    let fragment = open_bound_fragment(&uri, 0);

    let operation_fragments = [fragment as *const LanceFragment];
    let append = unsafe { lance_operation_append_new(operation_fragments.as_ptr(), 1) };
    assert!(!append.is_null());
    let mut detached = ptr::null_mut();
    assert_eq!(
        unsafe { lance_operation_append_fragment_at(append, 0, &mut detached) },
        0
    );
    assert!(!detached.is_null());
    unsafe { lance_operation_close(append) };

    let sql_name = c_string("blocked");
    let sql_expression = c_string("value + 1");
    let sql_columns = [LanceSqlColumn {
        name: sql_name.as_ptr(),
        expression: sql_expression.as_ptr(),
    }];
    let mut sql_fragment = 1usize as *mut LanceFragment;
    let mut sql_schema = 1usize as *mut LanceSchema;
    let sql_fragment_sentinel = sql_fragment;
    let sql_schema_sentinel = sql_schema;
    assert_eq!(
        unsafe {
            lance_fragment_add_columns_sql(
                detached,
                sql_columns.as_ptr(),
                1,
                ptr::null(),
                &mut sql_fragment,
                &mut sql_schema,
            )
        },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::NotSupported);
    assert_eq!(sql_fragment, sql_fragment_sentinel);
    assert_eq!(sql_schema, sql_schema_sentinel);
    let _ = take_error();

    let mut version = 0_u64;
    assert_eq!(
        unsafe { lance_fragment_read_version(detached, &mut version) },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::NotSupported);
    assert_eq!(version, 0);
    let _ = take_error();

    let zero = 0u32;
    let mut zero_fragment = 1usize as *mut LanceFragment;
    let mut zero_schema = 1usize as *mut LanceSchema;
    let zero_fragment_sentinel = zero_fragment;
    let zero_schema_sentinel = zero_schema;
    assert_eq!(
        unsafe {
            lance_fragment_add_columns_sql(
                fragment,
                sql_columns.as_ptr(),
                1,
                &zero,
                &mut zero_fragment,
                &mut zero_schema,
            )
        },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(zero_fragment, zero_fragment_sentinel);
    assert_eq!(zero_schema, zero_schema_sentinel);
    let (_, message) = take_error();
    assert!(message.contains("batch_size"));

    let mut zero_reader_stream =
        one_column_stream("zero_reader", vec![Some(1), Some(2), Some(3), Some(4)]);
    let mut zero_reader_fragment = 1usize as *mut LanceFragment;
    let mut zero_reader_schema = 1usize as *mut LanceSchema;
    let zero_reader_fragment_sentinel = zero_reader_fragment;
    let zero_reader_schema_sentinel = zero_reader_schema;
    assert_eq!(
        unsafe {
            lance_fragment_add_columns_stream(
                fragment,
                &mut zero_reader_stream,
                &zero,
                &mut zero_reader_fragment,
                &mut zero_reader_schema,
            )
        },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(zero_reader_fragment, zero_reader_fragment_sentinel);
    assert_eq!(zero_reader_schema, zero_reader_schema_sentinel);
    take_stream_consumed(&zero_reader_stream);
    let _ = take_error();

    let mut invalid_output_stream =
        one_column_stream("invalid_output", vec![Some(1), Some(2), Some(3), Some(4)]);
    assert_eq!(
        unsafe {
            lance_fragment_add_columns_stream(
                fragment,
                &mut invalid_output_stream,
                ptr::null(),
                ptr::null_mut(),
                ptr::null_mut(),
            )
        },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    take_stream_consumed(&invalid_output_stream);
    let _ = take_error();

    let mut missing_stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe {
            lance_fragment_merge_columns(
                fragment,
                &mut missing_stream,
                ptr::null(),
                ptr::null(),
                ptr::null_mut(),
                ptr::null_mut(),
            )
        },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let _ = take_error();

    let result_sentinel = 1usize as *mut LanceFragmentUpdateColumnsResult;
    let mut result = result_sentinel;
    let mut bad_stream = join_stream(vec![1], "value", vec![Some(1)]);
    let left_on = c_string("missing_left");
    let right_on = c_string("id");
    assert_eq!(
        unsafe {
            lance_fragment_update_columns(
                fragment,
                &mut bad_stream,
                left_on.as_ptr(),
                right_on.as_ptr(),
                &mut result,
            )
        },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert_eq!(result, result_sentinel);
    take_stream_consumed(&bad_stream);
    let _ = take_error();

    let mut null_count = u64::MAX;
    assert_eq!(
        unsafe {
            lance_fragment_update_columns_result_fields_modified_count(ptr::null(), &mut null_count)
        },
        -1
    );
    assert_eq!(null_count, u64::MAX);
    let _ = take_error();

    unsafe {
        lance_fragment_close(detached);
        lance_fragment_close(fragment);
    }
}
