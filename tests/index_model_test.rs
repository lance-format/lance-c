// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

use std::ffi::CString;
use std::ptr;
use std::sync::Arc;

use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema, from_ffi};
use arrow_array::{Array, FixedSizeListArray, Float32Array, RecordBatch, make_array};
use arrow_schema::{DataType, Field, Schema};
use lance::Dataset;
use lance_c::{
    LanceErrorCode, LanceMetricType, lance_dataset_close, lance_dataset_open,
    lance_index_train_ivf_model, lance_index_train_pq_model, lance_last_error_code,
};

const DIMENSION: i32 = 4;

fn vector_batch(start: usize, rows: usize, schema: Arc<Schema>) -> RecordBatch {
    let values = Float32Array::from_iter_values((start..start + rows).flat_map(|row| {
        let row = row as f32;
        [row, row * 0.5 + 1.0, (row % 11.0) - 5.0, row * 0.25]
    }));
    let item = Arc::new(Field::new("item", DataType::Float32, false));
    let vectors = FixedSizeListArray::try_new(item, DIMENSION, Arc::new(values), None).unwrap();
    RecordBatch::try_new(schema, vec![Arc::new(vectors)]).unwrap()
}

fn create_multi_fragment_vector_dataset() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp
        .path()
        .join("index-model.lance")
        .to_string_lossy()
        .into_owned();
    let item = Arc::new(Field::new("item", DataType::Float32, false));
    let schema = Arc::new(Schema::new(vec![Field::new(
        "vector",
        DataType::FixedSizeList(item, DIMENSION),
        false,
    )]));

    lance_c::runtime::block_on(async {
        let first = vector_batch(0, 64, schema.clone());
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(first)], schema.clone()),
            &uri,
            None,
        )
        .await
        .unwrap();

        let second = vector_batch(64, 64, schema.clone());
        let mut dataset = Dataset::open(&uri).await.unwrap();
        dataset
            .append(
                arrow::record_batch::RecordBatchIterator::new(vec![Ok(second)], schema.clone()),
                None,
            )
            .await
            .unwrap();
        assert_eq!(dataset.count_fragments(), 2);
    });

    (tmp, uri)
}

#[test]
fn train_ivf_model_exports_requested_shape_and_release_callbacks() {
    let (_tmp, uri) = create_multi_fragment_vector_dataset();
    let uri = CString::new(uri).unwrap();
    let column = CString::new("vector").unwrap();
    let dataset = unsafe { lance_dataset_open(uri.as_ptr(), ptr::null(), 0) };
    assert!(!dataset.is_null());

    let fragment_ids = [0_u32];
    let mut out_array = FFI_ArrowArray::empty();
    let mut out_schema = FFI_ArrowSchema::empty();
    let result = unsafe {
        lance_index_train_ivf_model(
            dataset,
            column.as_ptr(),
            2,
            LanceMetricType::L2 as i32,
            fragment_ids.as_ptr(),
            fragment_ids.len(),
            &mut out_array,
            &mut out_schema,
        )
    };
    assert_eq!(result, 0);
    assert!(!out_array.is_released());
    assert!(out_schema.release.is_some());

    let array_data =
        unsafe { from_ffi(FFI_ArrowArray::from_raw(&mut out_array), &out_schema) }.unwrap();
    assert!(out_array.is_released());
    let array = make_array(array_data);
    let centroids = array.as_any().downcast_ref::<FixedSizeListArray>().unwrap();
    assert_eq!(centroids.len(), 2);
    assert_eq!(centroids.value_length(), DIMENSION);
    assert_eq!(centroids.value_type(), DataType::Float32);

    unsafe { lance_dataset_close(dataset) };
}

#[test]
fn train_pq_model_exports_documented_subvector_shape() {
    let (_tmp, uri) = create_multi_fragment_vector_dataset();
    let uri = CString::new(uri).unwrap();
    let column = CString::new("vector").unwrap();
    let dataset = unsafe { lance_dataset_open(uri.as_ptr(), ptr::null(), 0) };
    assert!(!dataset.is_null());

    let fragment_ids = [0_u32, 1_u32];
    let mut centroids = FFI_ArrowArray::empty();
    let mut centroids_schema = FFI_ArrowSchema::empty();
    assert_eq!(
        unsafe {
            lance_index_train_ivf_model(
                dataset,
                column.as_ptr(),
                2,
                LanceMetricType::L2 as i32,
                fragment_ids.as_ptr(),
                fragment_ids.len(),
                &mut centroids,
                &mut centroids_schema,
            )
        },
        0
    );
    let mut out_array = FFI_ArrowArray::empty();
    let mut out_schema = FFI_ArrowSchema::empty();
    let result = unsafe {
        lance_index_train_pq_model(
            dataset,
            column.as_ptr(),
            2,
            4,
            LanceMetricType::L2 as i32,
            fragment_ids.as_ptr(),
            fragment_ids.len(),
            &mut centroids,
            &centroids_schema,
            &mut out_array,
            &mut out_schema,
        )
    };
    assert_eq!(result, 0);
    assert!(!centroids.is_released());

    let array_data =
        unsafe { from_ffi(FFI_ArrowArray::from_raw(&mut out_array), &out_schema) }.unwrap();
    let array = make_array(array_data);
    let codebook = array.as_any().downcast_ref::<FixedSizeListArray>().unwrap();
    assert_eq!(codebook.len(), 2 * (1 << 4));
    assert_eq!(codebook.value_length(), DIMENSION / 2);
    assert_eq!(codebook.value_type(), DataType::Float32);

    unsafe {
        if let Some(release) = centroids.release {
            release(&mut centroids);
        }
        lance_dataset_close(dataset);
    }
}

#[test]
fn trainer_rejects_invalid_metric_without_touching_outputs() {
    let (_tmp, uri) = create_multi_fragment_vector_dataset();
    let uri = CString::new(uri).unwrap();
    let column = CString::new("vector").unwrap();
    let dataset = unsafe { lance_dataset_open(uri.as_ptr(), ptr::null(), 0) };
    let mut out_array = FFI_ArrowArray::empty();
    let mut out_schema = FFI_ArrowSchema::empty();

    assert_eq!(
        unsafe {
            lance_index_train_ivf_model(
                dataset,
                column.as_ptr(),
                2,
                99,
                ptr::null(),
                0,
                &mut out_array,
                &mut out_schema,
            )
        },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert!(out_array.is_released());
    assert!(out_schema.release.is_none());

    unsafe { lance_dataset_close(dataset) };
}
