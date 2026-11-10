// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

use std::ffi::{CString, c_void};
use std::ptr;
use std::sync::Arc;

use arrow::buffer::{NullBuffer, OffsetBuffer};
use arrow::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use arrow::record_batch::{RecordBatchIterator, RecordBatchReader};
use arrow_array::{Array, FixedSizeListArray, Float32Array, Int32Array, ListArray, RecordBatch};
use arrow_schema::{DataType, Field, Schema};
use lance::Dataset;
use lance_c::*;

fn fixture() -> (tempfile::TempDir, CString) {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("vectors.lance");
    let element = Arc::new(Field::new("item", DataType::Float32, false));
    let vectors = FixedSizeListArray::try_new(
        element,
        2,
        Arc::new(Float32Array::from(vec![
            1., 0., 0., 1., 2., 0., 3., 0., 0., 3.,
        ])),
        None,
    )
    .unwrap();
    let child = Arc::new(Field::new("item", vectors.data_type().clone(), false));
    let rows = ListArray::try_new(
        child,
        OffsetBuffer::new(vec![0i32, 2, 3, 5, 5, 5].into()),
        Arc::new(vectors),
        Some(NullBuffer::from(vec![true, true, true, true, false])),
    )
    .unwrap();
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("vectors", rows.data_type().clone(), true),
    ]));
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from(vec![1, 2, 3, 4, 5])),
            Arc::new(rows),
        ],
    )
    .unwrap();
    lance_c::runtime::block_on(async {
        Dataset::write(
            RecordBatchIterator::new(vec![Ok(batch)], schema),
            path.to_str().unwrap(),
            None,
        )
        .await
        .unwrap();
    });
    (dir, CString::new(path.to_str().unwrap()).unwrap())
}

unsafe fn collect(scanner: *mut LanceScanner) -> Vec<(i32, f32)> {
    let mut stream = FFI_ArrowArrayStream::empty();
    let status = unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream) };
    if status != 0 {
        panic!(
            "{}",
            unsafe { std::ffi::CStr::from_ptr(lance_last_error_message()) }.to_string_lossy()
        );
    }
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream) }.unwrap();
    reader
        .flat_map(|batch| {
            let batch = batch.unwrap();
            let ids = batch
                .column_by_name("id")
                .unwrap()
                .as_any()
                .downcast_ref::<Int32Array>()
                .unwrap();
            let distances = batch
                .column_by_name("_distance")
                .unwrap()
                .as_any()
                .downcast_ref::<Float32Array>()
                .unwrap();
            (0..batch.num_rows())
                .map(|i| (ids.value(i), distances.value(i)))
                .collect::<Vec<_>>()
        })
        .collect()
}

fn set_projection(scanner: *mut LanceScanner, specs: &[(&str, &str)]) {
    let names = specs
        .iter()
        .map(|(name, _)| CString::new(*name).unwrap())
        .collect::<Vec<_>>();
    let expressions = specs
        .iter()
        .map(|(_, expression)| CString::new(*expression).unwrap())
        .collect::<Vec<_>>();
    let columns = names
        .iter()
        .zip(&expressions)
        .map(|(name, expression)| LanceProjectionColumn {
            output_name: name.as_ptr(),
            expression: expression.as_ptr(),
        })
        .collect::<Vec<_>>();
    let explicit_empty = LanceProjectionColumn {
        output_name: ptr::null(),
        expression: ptr::null(),
    };
    let data = if columns.is_empty() {
        &explicit_empty
    } else {
        columns.as_ptr()
    };
    let status = unsafe { lance_scanner_set_projection(scanner, data, columns.len()) };
    if status != 0 {
        panic!(
            "{}",
            unsafe { std::ffi::CStr::from_ptr(lance_last_error_message()) }.to_string_lossy()
        );
    }
}

unsafe fn collect_projected(scanner: *mut LanceScanner) -> (Vec<String>, Vec<(i32, Option<f32>)>) {
    let mut stream = FFI_ArrowArrayStream::empty();
    let status = unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream) };
    if status != 0 {
        panic!(
            "{}",
            unsafe { std::ffi::CStr::from_ptr(lance_last_error_message()) }.to_string_lossy()
        );
    }
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream) }.unwrap();
    let fields = reader
        .schema()
        .fields()
        .iter()
        .map(|field| field.name().clone())
        .collect::<Vec<_>>();
    let mut rows = Vec::new();
    for batch in reader {
        let batch = batch.unwrap();
        let ids = batch
            .column_by_name("id_alias")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        let distances = batch
            .column_by_name("_distance")
            .map(|array| array.as_any().downcast_ref::<Float32Array>().unwrap());
        for row in 0..batch.num_rows() {
            rows.push((
                ids.value(row),
                distances.and_then(|values| (!values.is_null(row)).then(|| values.value(row))),
            ));
        }
    }
    (fields, rows)
}

#[test]
fn multivector_search_scores_logical_rows_and_excludes_empty_and_null_rows() {
    let (_dir, uri) = fixture();
    let column = CString::new("vectors").unwrap();
    let q = [1.0f32, 0., 0., 1.];
    unsafe {
        let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
        assert!(!ds.is_null());
        for count in [1, 2] {
            let scan = lance_scanner_new(ds, ptr::null(), ptr::null());
            assert_eq!(
                lance_scanner_nearest_multivector(
                    scan,
                    column.as_ptr(),
                    q.as_ptr() as *const c_void,
                    2,
                    count,
                    0,
                    10
                ),
                0
            );
            assert_eq!(lance_scanner_set_use_index(scan, false), 0);
            assert_eq!(lance_scanner_set_prefilter(scan, true), 0);
            let rows = collect(scan);
            let expected = if count == 2 {
                vec![(1, 0.), (2, 6.), (3, 8.)]
            } else {
                vec![(1, 0.), (2, 1.), (3, 4.)]
            };
            assert_eq!(rows, expected);
            lance_scanner_close(scan);
        }
        lance_dataset_close(ds);
    }
}

#[test]
fn multivector_projection_keeps_the_internal_scoring_window() {
    let (_dir, uri) = fixture();
    let column = CString::new("vectors").unwrap();
    let q = [1.0f32, 0., 0., 1.];
    let alias = [("id_alias", "id")];
    let explicit_distance = [("id_alias", "id"), ("_distance", "_distance")];
    let negated_distance = [("id_alias", "id"), ("_distance", "-_distance")];
    let distances = [0.0, 6.0, 8.0];
    let negated_distances = [0.0, -6.0, -8.0];

    for (projection, disable, expected_distances) in [
        (alias.as_slice(), false, Some(distances.as_slice())),
        (alias.as_slice(), true, None),
        (
            explicit_distance.as_slice(),
            true,
            Some(distances.as_slice()),
        ),
        (
            negated_distance.as_slice(),
            true,
            Some(negated_distances.as_slice()),
        ),
    ] {
        unsafe {
            let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
            assert!(!ds.is_null());
            let scan = lance_scanner_new(ds, ptr::null(), ptr::null());
            set_projection(scan, projection);
            if disable {
                assert_eq!(lance_scanner_disable_scoring_autoprojection(scan), 0);
            }
            assert_eq!(
                lance_scanner_nearest_multivector(
                    scan,
                    column.as_ptr(),
                    q.as_ptr() as *const c_void,
                    2,
                    2,
                    0,
                    10
                ),
                0
            );
            assert_eq!(lance_scanner_set_use_index(scan, false), 0);
            assert_eq!(lance_scanner_set_prefilter(scan, true), 0);

            let (fields, rows) = collect_projected(scan);
            assert_eq!(
                fields.contains(&"_distance".to_string()),
                expected_distances.is_some(),
                "projection={projection:?} disable={disable}"
            );
            let ids = rows.iter().map(|row| row.0).collect::<Vec<_>>();
            assert_eq!(ids, vec![1, 2, 3]);
            if let Some(expected_distances) = expected_distances {
                assert_eq!(
                    rows.iter().map(|row| row.1).collect::<Vec<_>>(),
                    expected_distances
                        .iter()
                        .copied()
                        .map(Some)
                        .collect::<Vec<_>>()
                );
            } else {
                assert!(rows.iter().all(|row| row.1.is_none()));
            }

            lance_scanner_close(scan);
            lance_dataset_close(ds);
        }
    }
}

#[test]
fn multivector_rejects_order_by_instead_of_ignoring_it() {
    let (_dir, uri) = fixture();
    let column = CString::new("vectors").unwrap();
    let q = [1.0f32, 0., 0., 1.];
    let ordering = LanceColumnOrdering {
        column_name: c"id".as_ptr(),
        direction: LanceOrderDirection::Ascending as i32,
        null_order: LanceNullOrder::NullsLast as i32,
    };
    unsafe {
        let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
        assert!(!ds.is_null());
        let scan = lance_scanner_new(ds, ptr::null(), ptr::null());
        assert_eq!(lance_scanner_set_order_by(scan, &ordering, 1), 0);
        assert_eq!(
            lance_scanner_nearest_multivector(
                scan,
                column.as_ptr(),
                q.as_ptr() as *const c_void,
                2,
                1,
                0,
                10
            ),
            0
        );
        assert_eq!(lance_scanner_set_use_index(scan, false), 0);
        let mut stream = FFI_ArrowArrayStream::empty();
        assert_eq!(lance_scanner_to_arrow_stream(scan, &mut stream), -1);
        assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
        let message = std::ffi::CStr::from_ptr(lance_last_error_message())
            .to_string_lossy()
            .into_owned();
        assert!(message.contains("order_by is not supported with nearest_multivector"));
        lance_scanner_close(scan);
        lance_dataset_close(ds);
    }
}

#[test]
fn multivector_rejects_invalid_shape_and_preserves_previous_query() {
    let (_dir, uri) = fixture();
    let column = CString::new("vectors").unwrap();
    let q = [1.0f32, 0., 0., 1.];
    unsafe {
        let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
        let scan = lance_scanner_new(ds, ptr::null(), ptr::null());
        assert_eq!(
            lance_scanner_nearest_multivector(
                scan,
                column.as_ptr(),
                q.as_ptr() as *const c_void,
                2,
                2,
                0,
                10
            ),
            0
        );
        for (dim, count, dtype, k) in [
            (0, 2, 0, 10),
            (2, 0, 0, 10),
            (3, 1, 0, 10),
            (2, 2, 3, 10),
            (2, 2, 4, 10),
            (2, 2, 1, 10),
            (2, 2, 0, 0),
            (usize::MAX, 2, 0, 10),
            (2, usize::MAX, 0, 10),
        ] {
            assert_eq!(
                lance_scanner_nearest_multivector(
                    scan,
                    column.as_ptr(),
                    q.as_ptr() as *const c_void,
                    dim,
                    count,
                    dtype,
                    k
                ),
                -1
            );
        }
        assert_eq!(
            lance_scanner_nearest_multivector(scan, column.as_ptr(), ptr::null(), 2, 2, 0, 10),
            -1
        );
        assert_eq!(lance_scanner_set_use_index(scan, false), 0);
        for (limit, offset) in [(-1, 0), (10, -1)] {
            assert_eq!(lance_scanner_set_limit(scan, limit), 0);
            assert_eq!(lance_scanner_set_offset(scan, offset), 0);
            let mut stream = FFI_ArrowArrayStream::empty();
            assert_eq!(lance_scanner_to_arrow_stream(scan, &mut stream), -1);
        }
        assert_eq!(lance_scanner_set_limit(scan, 10), 0);
        assert_eq!(lance_scanner_set_offset(scan, 0), 0);
        assert_eq!(collect(scan), vec![(1, 0.), (2, 6.), (3, 8.)]);
        lance_scanner_close(scan);
        lance_dataset_close(ds);
    }
}

#[test]
fn fragment_scoped_indexed_search_orders_candidates_before_offset() {
    use lance::index::DatasetIndexExt;
    use lance::index::vector::VectorIndexParams;
    use lance_index::IndexType;
    use lance_linalg::distance::MetricType;

    let (_dir, uri) = fixture();
    lance_c::runtime::block_on(async {
        let mut ds = Dataset::open(uri.to_str().unwrap()).await.unwrap();
        let batch = ds.scan().try_into_batch().await.unwrap();
        ds.create_index(
            &["vectors"],
            IndexType::Vector,
            None,
            &VectorIndexParams::ivf_flat(1, MetricType::Cosine),
            false,
        )
        .await
        .unwrap();
        ds.append(
            RecordBatchIterator::new(vec![Ok(batch.clone())], batch.schema()),
            None,
        )
        .await
        .unwrap();
    });
    let column = CString::new("vectors").unwrap();
    let filter = CString::new("id >= 2").unwrap();
    let q = [1.0f32, 0., 0., 1.];
    unsafe {
        let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
        for _ in 0..10 {
            let scan = lance_scanner_new(ds, ptr::null(), filter.as_ptr());
            assert_eq!(
                lance_scanner_set_fragment_ids(scan, [0u64, 1].as_ptr(), 2),
                0
            );
            assert_eq!(lance_scanner_set_prefilter(scan, true), 0);
            assert_eq!(lance_scanner_set_batch_size(scan, 1), 0);
            assert_eq!(
                lance_scanner_nearest_multivector(
                    scan,
                    column.as_ptr(),
                    q.as_ptr() as *const c_void,
                    2,
                    2,
                    0,
                    4
                ),
                0
            );
            assert_eq!(lance_scanner_set_metric(scan, 1), 0);
            assert_eq!(lance_scanner_set_refine_factor(scan, 1), 0);
            assert_eq!(lance_scanner_set_offset(scan, 1), 0);
            assert_eq!(lance_scanner_set_limit(scan, 2), 0);
            assert_eq!(collect(scan), vec![(3, 0.), (2, 1.)]);
            lance_scanner_close(scan);
        }
        lance_dataset_close(ds);
    }
}

fn custom_fixture(
    rows: Vec<Vec<Option<f32>>>,
    dim: i32,
    indexed: bool,
) -> (tempfile::TempDir, CString) {
    custom_fixture_at(rows, dim, indexed, "vectors")
}

fn custom_fixture_at(
    rows: Vec<Vec<Option<f32>>>,
    dim: i32,
    indexed: bool,
    column: &str,
) -> (tempfile::TempDir, CString) {
    use lance::index::DatasetIndexExt;
    use lance::index::vector::VectorIndexParams;
    use lance_index::IndexType;
    use lance_linalg::distance::MetricType;
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("vectors.lance");
    let mut offsets = vec![0i32];
    let mut values = Vec::new();
    for row in &rows {
        values.extend_from_slice(row);
        offsets.push(values.len() as i32 / dim);
    }
    let vectors = FixedSizeListArray::try_new(
        Arc::new(Field::new("item", DataType::Float32, true)),
        dim,
        Arc::new(Float32Array::from(values)),
        None,
    )
    .unwrap();
    let rows_array = ListArray::try_new(
        Arc::new(Field::new("item", vectors.data_type().clone(), false)),
        OffsetBuffer::new(offsets.into()),
        Arc::new(vectors),
        None,
    )
    .unwrap();
    let parts = lance_core::datatypes::parse_field_path(column).unwrap();
    let mut array: arrow_array::ArrayRef = Arc::new(rows_array);
    for name in parts[1..].iter().rev() {
        let field = Arc::new(Field::new(name, array.data_type().clone(), true));
        let labels: arrow_array::ArrayRef = Arc::new(Int32Array::from(vec![7; rows.len()]));
        array = Arc::new(arrow_array::StructArray::from(vec![
            (field, array),
            (
                Arc::new(Field::new("label", DataType::Int32, false)),
                labels,
            ),
        ]));
    }
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new(&parts[0], array.data_type().clone(), true),
    ]));
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from_iter_values(1..=rows.len() as i32)),
            array,
        ],
    )
    .unwrap();
    lance_c::runtime::block_on(async {
        let mut ds = Dataset::write(
            RecordBatchIterator::new(vec![Ok(batch)], schema),
            path.to_str().unwrap(),
            None,
        )
        .await
        .unwrap();
        if indexed {
            ds.create_index(
                &[column],
                IndexType::Vector,
                None,
                &VectorIndexParams::ivf_flat(1, MetricType::Cosine),
                false,
            )
            .await
            .unwrap();
        }
    });
    (dir, CString::new(path.to_str().unwrap()).unwrap())
}

#[test]
fn exact_top_one_preserves_small_distances_before_truncation() {
    let (_dir, uri) = custom_fixture(vec![vec![Some(0.00015)], vec![Some(0.0001)]], 1, false);
    let column = CString::new("vectors").unwrap();
    unsafe {
        let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
        for count in [1, 2] {
            for batch_size in [1, 1024] {
                let scan = lance_scanner_new(ds, ptr::null(), ptr::null());
                assert_eq!(
                    lance_scanner_nearest_multivector(
                        scan,
                        column.as_ptr(),
                        [0.0f32, 0.0].as_ptr().cast(),
                        1,
                        count,
                        0,
                        1
                    ),
                    0
                );
                assert_eq!(lance_scanner_set_use_index(scan, false), 0);
                assert_eq!(lance_scanner_set_batch_size(scan, batch_size), 0);
                let rows = collect(scan);
                assert_eq!(rows.len(), 1);
                assert_eq!(rows[0].0, 2);
                assert!((rows[0].1 - count as f32 * 1e-8).abs() < 1e-14, "{rows:?}");
                lance_scanner_close(scan);
            }
        }
        lance_dataset_close(ds);
    }
}

#[test]
fn indexed_top_one_is_independent_of_child_batch_boundaries() {
    let rows = vec![
        vec![Some(1.), Some(0.)],
        vec![Some(0.), Some(1.)],
        vec![Some(1.), Some(1.)],
        vec![Some(2.), Some(1.)],
        vec![Some(1.), Some(2.)],
    ];
    let (_dir, uri) = custom_fixture(rows, 2, true);
    let column = CString::new("vectors").unwrap();
    unsafe {
        let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
        for batch_size in [1, 2, 1024] {
            let scan = lance_scanner_new(ds, ptr::null(), ptr::null());
            assert_eq!(
                lance_scanner_nearest_multivector(
                    scan,
                    column.as_ptr(),
                    [1.0f32, 0., 0., 1.].as_ptr().cast(),
                    2,
                    2,
                    0,
                    1
                ),
                0
            );
            assert_eq!(lance_scanner_set_metric(scan, 1), 0);
            assert_eq!(lance_scanner_set_refine_factor(scan, 1), 0);
            assert_eq!(lance_scanner_set_batch_size(scan, batch_size), 0);
            let result = collect(scan);
            assert_eq!(result[0].0, 3, "batch_size={batch_size}");
            assert!((result[0].1 - (2.0 - 2.0f32.sqrt())).abs() < 1e-6);
            lance_scanner_close(scan);
        }
        lance_dataset_close(ds);
    }
}

#[test]
fn actual_null_and_nonfinite_stored_elements_fail_search() {
    let column = CString::new("vectors").unwrap();
    for value in [
        None,
        Some(f32::NAN),
        Some(f32::INFINITY),
        Some(f32::NEG_INFINITY),
    ] {
        let (_dir, uri) = custom_fixture(vec![vec![value, Some(0.)]], 2, false);
        unsafe {
            let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
            let scan = lance_scanner_new(ds, ptr::null(), ptr::null());
            assert_eq!(
                lance_scanner_nearest_multivector(
                    scan,
                    column.as_ptr(),
                    [0.0f32, 0.].as_ptr().cast(),
                    2,
                    1,
                    0,
                    1
                ),
                0
            );
            assert_eq!(lance_scanner_set_use_index(scan, false), 0);
            let mut stream = FFI_ArrowArrayStream::empty();
            assert_eq!(lance_scanner_to_arrow_stream(scan, &mut stream), 0);
            let mut reader = ArrowArrayStreamReader::from_raw(&mut stream).unwrap();
            let result = reader.next();
            assert!(
                matches!(result, Some(Err(_))),
                "value={value:?}: {result:?}"
            );
            drop(reader);
            lance_scanner_close(scan);
            lance_dataset_close(ds);
        }
    }
}

#[test]
fn rejects_excessive_multivector_plan_width_and_candidate_work() {
    let (_dir, uri) = fixture();
    let column = CString::new("vectors").unwrap();
    unsafe {
        let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
        let scan = lance_scanner_new(ds, ptr::null(), ptr::null());
        let q = [0.0f32; 258];
        for (count, k) in [(129, 1), (128, 782), (1, 100001)] {
            assert_eq!(
                lance_scanner_nearest_multivector(
                    scan,
                    column.as_ptr(),
                    q.as_ptr().cast(),
                    2,
                    count,
                    0,
                    k
                ),
                -1
            );
        }
        lance_scanner_close(scan);
        lance_dataset_close(ds);
    }
}

#[test]
fn omitted_metric_is_l2_on_indexed_and_unindexed_fragments() {
    let (_dir, uri) = custom_fixture(vec![vec![Some(2.), Some(0.)]], 2, true);
    lance_c::runtime::block_on(async {
        let mut ds = Dataset::open(uri.to_str().unwrap()).await.unwrap();
        let old = ds.scan().try_into_batch().await.unwrap();
        let vectors = FixedSizeListArray::try_new(
            Arc::new(Field::new("item", DataType::Float32, true)),
            2,
            Arc::new(Float32Array::from(vec![1.0, 0.1])),
            None,
        )
        .unwrap();
        let lists = ListArray::try_new(
            Arc::new(Field::new("item", vectors.data_type().clone(), false)),
            OffsetBuffer::new(vec![0i32, 1].into()),
            Arc::new(vectors),
            None,
        )
        .unwrap();
        let batch = RecordBatch::try_new(
            old.schema(),
            vec![Arc::new(Int32Array::from(vec![2])), Arc::new(lists)],
        )
        .unwrap();
        ds.append(
            RecordBatchIterator::new(vec![Ok(batch)], old.schema()),
            None,
        )
        .await
        .unwrap();
    });
    let column = CString::new("vectors").unwrap();
    unsafe {
        let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
        for (fragment, id, score) in [(0u64, 1, 1.0f32), (1u64, 2, 0.01f32)] {
            let scan = lance_scanner_new(ds, ptr::null(), ptr::null());
            assert_eq!(
                lance_scanner_nearest_multivector(
                    scan,
                    column.as_ptr(),
                    [1.0f32, 0.].as_ptr().cast(),
                    2,
                    1,
                    0,
                    1
                ),
                0
            );
            assert_eq!(lance_scanner_set_prefilter(scan, true), 0);
            assert_eq!(lance_scanner_set_fragment_ids(scan, &fragment, 1), 0);
            let rows = collect(scan);
            assert_eq!(rows[0].0, id);
            assert!(
                (rows[0].1 - score).abs() < 1e-6,
                "fragment={fragment}: {rows:?}"
            );
            lance_scanner_close(scan);
        }
        lance_dataset_close(ds);
    }
}

#[test]
fn query_values_and_refinement_are_validated_before_execution() {
    let (_dir, uri) = fixture();
    let column = CString::new("vectors").unwrap();
    unsafe {
        let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
        let scan = lance_scanner_new(ds, ptr::null(), ptr::null());
        for value in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
            assert_eq!(
                lance_scanner_nearest_multivector(
                    scan,
                    column.as_ptr(),
                    [value, 0.0f32].as_ptr().cast(),
                    2,
                    1,
                    0,
                    1
                ),
                -1
            );
        }
        let query = [1.0f32; 256];
        assert_eq!(
            lance_scanner_nearest_multivector(
                scan,
                column.as_ptr(),
                query.as_ptr().cast(),
                2,
                128,
                0,
                781
            ),
            0
        );
        assert_eq!(lance_scanner_set_metric(scan, 3), 0);
        let mut invalid_metric_stream = FFI_ArrowArrayStream::empty();
        assert_eq!(
            lance_scanner_to_arrow_stream(scan, &mut invalid_metric_stream),
            -1
        );
        assert_eq!(lance_scanner_set_metric(scan, 0), 0);
        assert_eq!(lance_scanner_set_refine_factor(scan, u32::MAX), 0);
        let mut stream = FFI_ArrowArrayStream::empty();
        assert_eq!(lance_scanner_to_arrow_stream(scan, &mut stream), -1);
        lance_scanner_close(scan);
        lance_dataset_close(ds);
    }
}

#[test]
fn float16_and_float64_queries_support_all_distance_metrics() {
    let (dir, source) = fixture();
    for (code, dtype) in [(1, DataType::Float16), (2, DataType::Float64)] {
        let path = dir.path().join(format!("typed_{code}.lance"));
        lance_c::runtime::block_on(async {
            let source = Dataset::open(source.to_str().unwrap()).await.unwrap();
            let batch = source.scan().try_into_batch().await.unwrap();
            let vector_type = DataType::List(Arc::new(Field::new(
                "item",
                DataType::FixedSizeList(Arc::new(Field::new("item", dtype, true)), 2),
                false,
            )));
            let vectors =
                arrow::compute::cast(batch.column_by_name("vectors").unwrap(), &vector_type)
                    .unwrap();
            let schema = Arc::new(Schema::new(vec![
                Field::new("id", DataType::Int32, false),
                Field::new("vectors", vector_type, true),
            ]));
            let batch =
                RecordBatch::try_new(schema.clone(), vec![batch.column(0).clone(), vectors])
                    .unwrap();
            Dataset::write(
                RecordBatchIterator::new(vec![Ok(batch)], schema),
                path.to_str().unwrap(),
                None,
            )
            .await
            .unwrap();
        });
        let uri = CString::new(path.to_str().unwrap()).unwrap();
        let column = CString::new("vectors").unwrap();
        let f16_query = [1.0f32, 0., 0., 1.].map(half::f16::from_f32);
        let f64_query = [1.0f64, 0., 0., 1.];
        let query = if code == 1 {
            f16_query.as_ptr().cast()
        } else {
            f64_query.as_ptr().cast()
        };
        unsafe {
            let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
            for (metric, expected) in [(0, [0., 6., 8.]), (1, [0., 1., 0.]), (2, [0., 0., -4.])] {
                let scanner = lance_scanner_new(ds, ptr::null(), ptr::null());
                assert_eq!(
                    lance_scanner_nearest_multivector(
                        scanner,
                        column.as_ptr(),
                        query,
                        2,
                        2,
                        code,
                        10
                    ),
                    0
                );
                assert_eq!(lance_scanner_set_metric(scanner, metric), 0);
                assert_eq!(lance_scanner_set_use_index(scanner, false), 0);
                let mut rows = collect(scanner);
                rows.sort_by_key(|row| row.0);
                assert_eq!(
                    rows,
                    vec![(1, expected[0]), (2, expected[1]), (3, expected[2])]
                );
                lance_scanner_close(scanner);
            }
            lance_dataset_close(ds);
        }
    }
}

#[test]
fn cosine_zero_norm_rows_do_not_abort_exact_or_refined_search() {
    let rows = vec![
        vec![Some(0.), Some(0.)],
        vec![Some(1.), Some(0.)],
        vec![Some(0.), Some(0.), Some(0.), Some(1.)],
    ];
    for indexed in [false, true] {
        let (_dir, uri) = custom_fixture(rows.clone(), 2, indexed);
        unsafe {
            let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
            for query in [[1.0f32, 0., 0., 1.], [0., 0., 0., 1.]] {
                for count in [1, 2] {
                    let scan = lance_scanner_new(ds, ptr::null(), ptr::null());
                    assert_eq!(
                        lance_scanner_nearest_multivector(
                            scan,
                            c"vectors".as_ptr(),
                            query.as_ptr().cast(),
                            2,
                            count,
                            0,
                            3
                        ),
                        0
                    );
                    assert_eq!(lance_scanner_set_use_index(scan, indexed), 0);
                    assert_eq!(lance_scanner_set_metric(scan, 1), 0);
                    assert_eq!(lance_scanner_set_refine_factor(scan, 2), 0);
                    let mut actual = collect(scan);
                    actual.sort_by_key(|row| row.0);
                    let expected = if query[0] == 0. {
                        vec![]
                    } else if count == 1 {
                        vec![(2, 0.), (3, 1.)]
                    } else {
                        vec![(2, 1.), (3, 1.)]
                    };
                    assert_eq!(
                        actual, expected,
                        "indexed={indexed}, query={query:?}, count={count}"
                    );
                    lance_scanner_close(scan);
                }
            }
            lance_dataset_close(ds);
        }
    }
}

#[test]
fn nested_and_quoted_columns_support_exact_and_refined_search() {
    for column in [
        "payload.vectors",
        "payload.`vectors.with.dot`",
        "payload.inner.`vectors.with.dot`",
    ] {
        let rows = vec![
            vec![Some(1.), Some(0.), Some(0.), Some(1.)],
            vec![Some(2.), Some(0.)],
            vec![Some(1.), Some(1.)],
        ];
        for indexed in [false, true] {
            let (_dir, uri) = custom_fixture_at(rows.clone(), 2, indexed, column);
            let column = CString::new(column).unwrap();
            unsafe {
                let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
                let scan = lance_scanner_new(ds, ptr::null(), ptr::null());
                assert_eq!(
                    lance_scanner_nearest_multivector(
                        scan,
                        column.as_ptr(),
                        [1.0f32, 0., 0., 1.].as_ptr().cast(),
                        2,
                        2,
                        0,
                        3
                    ),
                    0
                );
                assert_eq!(lance_scanner_set_use_index(scan, indexed), 0);
                assert_eq!(lance_scanner_set_metric(scan, 1), 0);
                assert_eq!(lance_scanner_set_refine_factor(scan, 2), 0);
                let actual = collect(scan);
                assert_eq!(actual.len(), 3);
                assert_eq!(
                    actual.iter().map(|row| row.0).collect::<Vec<_>>(),
                    vec![1, 3, 2]
                );
                assert_eq!(actual[0].1, 0.);
                assert!((actual[1].1 - (2. - 2.0f32.sqrt())).abs() < 1e-6);
                assert_eq!(actual[2].1, 1.);
                lance_scanner_close(scan);
                lance_dataset_close(ds);
            }
        }
    }
}

#[test]
fn nested_projections_preserve_schema_and_values_in_exact_indexed_and_hybrid_search() {
    for column in ["payload.vectors", "payload.`vectors.with.dot`"] {
        for indexed in [false, true] {
            for appended in [false, true] {
                let (_dir, uri) = custom_fixture_at(
                    vec![vec![Some(1.), Some(0.)], vec![Some(0.), Some(1.)]],
                    2,
                    indexed,
                    column,
                );
                if appended {
                    lance_c::runtime::block_on(async {
                        let mut ds = Dataset::open(uri.to_str().unwrap()).await.unwrap();
                        let batch = ds.scan().try_into_batch().await.unwrap();
                        ds.append(
                            RecordBatchIterator::new(vec![Ok(batch.clone())], batch.schema()),
                            None,
                        )
                        .await
                        .unwrap();
                    });
                }
                for projection in [
                    vec![],
                    vec!["id"],
                    vec!["payload.label"],
                    vec!["id", column],
                ] {
                    let expected = lance_c::runtime::block_on(async {
                        let ds = Dataset::open(uri.to_str().unwrap()).await.unwrap();
                        let mut scanner = ds.scan();
                        scanner.scan_in_order(true);
                        if !projection.is_empty() {
                            scanner.project(&projection).unwrap();
                        }
                        scanner.try_into_batch().await.unwrap()
                    });
                    // The appended fragment repeats the same two values; distance order groups
                    // both exact matches before the orthogonal rows, regardless of tie order.
                    let indices = arrow_array::UInt32Array::from(if appended {
                        vec![0, 2, 1, 3]
                    } else {
                        vec![0, 1]
                    });
                    let expected = RecordBatch::try_new(
                        expected.schema(),
                        expected
                            .columns()
                            .iter()
                            .map(|array| {
                                arrow::compute::take(array.as_ref(), &indices, None).unwrap()
                            })
                            .collect(),
                    )
                    .unwrap();
                    unsafe {
                        let names: Vec<_> = projection
                            .iter()
                            .map(|name| CString::new(*name).unwrap())
                            .collect();
                        let mut columns: Vec<_> = names.iter().map(|name| name.as_ptr()).collect();
                        columns.push(ptr::null());
                        let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
                        let scan = lance_scanner_new(
                            ds,
                            if projection.is_empty() {
                                ptr::null()
                            } else {
                                columns.as_ptr()
                            },
                            ptr::null(),
                        );
                        let column = CString::new(column).unwrap();
                        assert_eq!(
                            lance_scanner_nearest_multivector(
                                scan,
                                column.as_ptr(),
                                [1.0f32, 0.].as_ptr().cast(),
                                2,
                                1,
                                0,
                                10
                            ),
                            0
                        );
                        assert_eq!(lance_scanner_set_use_index(scan, indexed), 0);
                        assert_eq!(lance_scanner_set_metric(scan, 1), 0);
                        assert_eq!(lance_scanner_set_batch_size(scan, 1), 0);
                        let mut stream = FFI_ArrowArrayStream::empty();
                        assert_eq!(lance_scanner_to_arrow_stream(scan, &mut stream), 0);
                        let batches = ArrowArrayStreamReader::from_raw(&mut stream)
                            .unwrap()
                            .collect::<Result<Vec<_>, _>>()
                            .unwrap();
                        lance_scanner_close(scan);
                        lance_dataset_close(ds);
                        let actual =
                            arrow::compute::concat_batches(&batches[0].schema(), &batches).unwrap();
                        let distances = actual
                            .column_by_name("_distance")
                            .unwrap()
                            .as_any()
                            .downcast_ref::<Float32Array>()
                            .unwrap();
                        assert_eq!(
                            distances.values().as_ref(),
                            if appended {
                                &[0., 0., 1., 1.][..]
                            } else {
                                &[0., 1.][..]
                            }
                        );
                        let positions: Vec<_> = actual
                            .schema()
                            .fields()
                            .iter()
                            .enumerate()
                            .filter_map(|(i, field)| (field.name() != "_distance").then_some(i))
                            .collect();
                        assert_eq!(
                            actual.project(&positions).unwrap(),
                            expected,
                            "column={column:?} indexed={indexed} appended={appended} projection={projection:?}"
                        );
                    }
                }
            }
        }
    }
}

#[test]
fn strict_batches_apply_after_multivector_offset_and_limit() {
    let (_dir, uri) = custom_fixture(
        (1..=6).map(|i| vec![Some(i as f32), Some(0.)]).collect(),
        2,
        false,
    );
    unsafe {
        let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
        for batch_size in [Some(2), None] {
            for (offset, limit) in [(1, 4), (1, 3), (5, 4), (6, 4), (0, 6)] {
                let scan = lance_scanner_new(ds, ptr::null(), ptr::null());
                assert_eq!(
                    lance_scanner_nearest_multivector(
                        scan,
                        c"vectors".as_ptr(),
                        [1.0f32, 0.].as_ptr().cast(),
                        2,
                        1,
                        0,
                        6
                    ),
                    0
                );
                assert_eq!(lance_scanner_set_use_index(scan, false), 0);
                if let Some(size) = batch_size {
                    assert_eq!(lance_scanner_set_batch_size(scan, size), 0);
                }
                assert_eq!(lance_scanner_set_strict_batch_size(scan, true), 0);
                assert_eq!(lance_scanner_set_offset(scan, offset), 0);
                assert_eq!(lance_scanner_set_limit(scan, limit), 0);
                let mut stream = FFI_ArrowArrayStream::empty();
                assert_eq!(lance_scanner_to_arrow_stream(scan, &mut stream), 0);
                let batches = ArrowArrayStreamReader::from_raw(&mut stream)
                    .unwrap()
                    .collect::<Result<Vec<_>, _>>()
                    .unwrap();
                lance_scanner_close(scan);
                let sizes: Vec<_> = batches.iter().map(RecordBatch::num_rows).collect();
                let ids: Vec<_> = batches
                    .iter()
                    .flat_map(|b| {
                        b.column_by_name("id")
                            .unwrap()
                            .as_any()
                            .downcast_ref::<Int32Array>()
                            .unwrap()
                            .values()
                            .to_vec()
                    })
                    .collect();
                let expected_ids: Vec<_> =
                    (1..=6).skip(offset as usize).take(limit as usize).collect();
                let expected_sizes: Vec<_> = expected_ids
                    .chunks(batch_size.unwrap_or(8192) as usize)
                    .map(<[i32]>::len)
                    .collect();
                assert_eq!(ids, expected_ids);
                assert_eq!(
                    sizes, expected_sizes,
                    "batch_size={batch_size:?} offset={offset} limit={limit}"
                );
            }
        }
        lance_dataset_close(ds);
    }
}

#[test]
fn multivector_distance_range_is_rejected_instead_of_filtering_subvectors() {
    let (_dir, uri) = fixture();
    unsafe {
        let ds = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
        let scan = lance_scanner_new(ds, ptr::null(), ptr::null());
        let query = [1.0f32, 0.0];
        assert_eq!(
            lance_scanner_nearest_multivector(
                scan,
                CString::new("vectors").unwrap().as_ptr(),
                query.as_ptr().cast(),
                2,
                1,
                LanceDataType::Float32 as i32,
                3
            ),
            0
        );
        assert_eq!(
            lance_scanner_set_distance_range(scan, ptr::null(), &1.0),
            -1
        );
        assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
        lance_scanner_close(scan);
        lance_dataset_close(ds);
    }
}

#[test]
fn batch_nearest_rejects_multivector_column_without_replacing_query() {
    let (_dir, uri) = fixture();
    unsafe {
        let dataset = lance_dataset_open(uri.as_ptr(), ptr::null(), 0);
        assert!(!dataset.is_null());
        let scan = lance_scanner_new(dataset, ptr::null(), ptr::null());
        assert!(!scan.is_null());
        let column = CString::new("vectors").unwrap();
        let query = [1f32, 0.];
        assert_eq!(
            lance_scanner_nearest_multivector(
                scan,
                column.as_ptr(),
                query.as_ptr().cast(),
                2,
                1,
                0,
                1
            ),
            0
        );
        assert_ne!(
            lance_scanner_nearest_batch(scan, column.as_ptr(), query.as_ptr().cast(), 2, 1, 0, 1),
            0
        );
        assert_eq!(lance_scanner_set_use_index(scan, false), 0);
        assert_eq!(collect(scan), vec![(1, 0.)]);
        lance_scanner_close(scan);
        lance_dataset_close(dataset);
    }
}
