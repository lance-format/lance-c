#![recursion_limit = "512"]

// Temporary validation-only native trainer zero-control probe.

use std::any::Any;
use std::ffi::{CStr, CString};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::sync::Arc;

use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema};
use arrow_array::{Array, FixedSizeListArray, Float32Array, Int32Array, RecordBatch, StringArray};
use arrow_schema::{DataType, Field, Schema};
use lance::Dataset;
use lance::index::vector::ivf::build_ivf_model;
use lance::index::vector::pq::build_pq_model_in_fragments;
use lance_c::{
    LanceDataset, LanceIndexTrainerOptions, LanceMetricType, LanceOptionalU64, lance_dataset_close,
    lance_dataset_open, lance_free_string, lance_index_train_ivf_model,
    lance_index_train_ivf_model_with_options, lance_index_train_pq_model_with_options,
    lance_last_error_code, lance_last_error_message,
};
use lance_index::progress::noop_progress;
use lance_index::vector::ivf::IvfBuildParams;
use lance_index::vector::ivf::storage::IvfModel;
use lance_index::vector::pq::PQBuildParams;
use lance_linalg::distance::DistanceType;

fn panic_message(payload: Box<dyn Any + Send>) -> String {
    if let Some(message) = payload.downcast_ref::<&str>() {
        (*message).to_string()
    } else if let Some(message) = payload.downcast_ref::<String>() {
        message.clone()
    } else {
        "<non-string panic>".to_string()
    }
}

fn report_native<T>(
    label: &str,
    operation: impl FnOnce() -> lance_core::Result<T>,
    shape: impl FnOnce(&T) -> String,
) {
    match catch_unwind(AssertUnwindSafe(operation)) {
        Ok(Ok(value)) => println!("{label}: Ok {}", shape(&value)),
        Ok(Err(error)) => println!("{label}: Err {error}"),
        Err(payload) => println!("{label}: PANIC {}", panic_message(payload)),
    }
}

fn dataset() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("zero_probe").to_str().unwrap().to_string();
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("text", DataType::Utf8, false),
        Field::new(
            "vector",
            DataType::FixedSizeList(Arc::new(Field::new("item", DataType::Float32, false)), 8),
            false,
        ),
    ]));
    lance_c::runtime::block_on(async {
        for fragment in 0..3 {
            let ids = (0..64).map(|row| fragment * 64 + row).collect::<Vec<_>>();
            let texts = ids.iter().map(|id| format!("doc{id}")).collect::<Vec<_>>();
            let text_refs = texts.iter().map(String::as_str).collect::<Vec<_>>();
            let values = ids
                .iter()
                .flat_map(|id| (0..8).map(move |component| *id as f32 + component as f32 / 10.0))
                .collect::<Vec<_>>();
            let vectors = FixedSizeListArray::try_new(
                Arc::new(Field::new("item", DataType::Float32, false)),
                8,
                Arc::new(Float32Array::from(values)),
                None,
            )
            .unwrap();
            let batch = RecordBatch::try_new(
                schema.clone(),
                vec![
                    Arc::new(Int32Array::from(ids)),
                    Arc::new(StringArray::from(text_refs)),
                    Arc::new(vectors),
                ],
            )
            .unwrap();
            let params = lance::dataset::WriteParams {
                mode: if fragment == 0 {
                    lance::dataset::WriteMode::Create
                } else {
                    lance::dataset::WriteMode::Append
                },
                ..Default::default()
            };
            Dataset::write(
                arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema.clone()),
                &uri,
                Some(params),
            )
            .await
            .unwrap();
        }
    });
    (tmp, uri)
}

fn fixed_centroids() -> FixedSizeListArray {
    FixedSizeListArray::try_new(
        Arc::new(Field::new("item", DataType::Float32, false)),
        8,
        Arc::new(Float32Array::from_iter_values(
            (0..16).map(|value| value as f32 * 10.0),
        )),
        None,
    )
    .unwrap()
}

fn optional(value: u64) -> LanceOptionalU64 {
    LanceOptionalU64 {
        value,
        is_set: true,
    }
}

fn take_error() -> String {
    let code = lance_last_error_code();
    let message = lance_last_error_message();
    let message = if message.is_null() {
        "<null>".to_string()
    } else {
        let value = unsafe { CStr::from_ptr(message) }
            .to_string_lossy()
            .into_owned();
        unsafe { lance_free_string(message) };
        value
    };
    format!("{code:?}: {message}")
}

fn release(array: &mut FFI_ArrowArray, schema: &mut FFI_ArrowSchema) {
    unsafe {
        if let Some(release) = array.release {
            release(array);
        }
        if let Some(release) = schema.release {
            release(schema);
        }
    }
}

#[test]
#[ignore]
fn probe_zero_trainer_controls() {
    let (_tmp, uri) = dataset();
    let native = lance_c::runtime::block_on(Dataset::open(&uri)).unwrap();
    let centroids = fixed_centroids();
    let ivf_model = IvfModel::new(centroids.clone(), None);

    for (sample_rate, max_iters) in [(0, 1), (1, 0)] {
        let mut params = IvfBuildParams::new(2);
        params.sample_rate = sample_rate;
        params.max_iters = max_iters;
        report_native(
            &format!("NATIVE_IVF sample_rate={sample_rate} max_iters={max_iters}"),
            || {
                lance_c::runtime::block_on(build_ivf_model(
                    &native,
                    "vector",
                    8,
                    DistanceType::L2,
                    &params,
                    None,
                    noop_progress(),
                ))
            },
            |model| {
                format!(
                    "partitions={} dimension={} rows={}",
                    model.num_partitions(),
                    model.dimension(),
                    model.num_rows()
                )
            },
        );
    }

    for (sample_rate, max_iters) in [(0, 1), (1, 0)] {
        let params = PQBuildParams {
            num_sub_vectors: 2,
            num_bits: 4,
            sample_rate,
            max_iters,
            ..Default::default()
        };
        report_native(
            &format!("NATIVE_PQ sample_rate={sample_rate} max_iters={max_iters}"),
            || {
                lance_c::runtime::block_on(build_pq_model_in_fragments(
                    &native,
                    "vector",
                    8,
                    DistanceType::L2,
                    &params,
                    Some(&ivf_model),
                    None,
                ))
            },
            |model| {
                format!(
                    "sub_vectors={} bits={} dimension={} codebook_len={}",
                    model.num_sub_vectors,
                    model.num_bits,
                    model.dimension,
                    model.codebook.len()
                )
            },
        );
    }

    let uri_c = CString::new(uri.as_str()).unwrap();
    let dataset: *mut LanceDataset = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    assert!(!dataset.is_null());
    let column = CString::new("vector").unwrap();

    for (sample_rate, max_iters) in [(0, 1), (1, 0)] {
        let options = LanceIndexTrainerOptions {
            sample_rate: optional(sample_rate),
            max_iters: optional(max_iters),
        };
        let mut array = FFI_ArrowArray::empty();
        let mut schema = FFI_ArrowSchema::empty();
        let rc = unsafe {
            lance_index_train_ivf_model_with_options(
                dataset,
                column.as_ptr(),
                2,
                LanceMetricType::L2 as i32,
                ptr::null(),
                0,
                &options,
                &mut array,
                &mut schema,
            )
        };
        if rc == 0 {
            println!(
                "C_IVF sample_rate={sample_rate} max_iters={max_iters}: Ok release={}",
                array.release.is_some()
            );
            release(&mut array, &mut schema);
        } else {
            println!(
                "C_IVF sample_rate={sample_rate} max_iters={max_iters}: {}",
                take_error()
            );
        }
    }

    let mut centroids_array = FFI_ArrowArray::empty();
    let mut centroids_schema = FFI_ArrowSchema::empty();
    let valid = LanceIndexTrainerOptions {
        sample_rate: optional(1),
        max_iters: optional(1),
    };
    assert_eq!(
        unsafe {
            lance_index_train_ivf_model_with_options(
                dataset,
                column.as_ptr(),
                2,
                LanceMetricType::L2 as i32,
                ptr::null(),
                0,
                &valid,
                &mut centroids_array,
                &mut centroids_schema,
            )
        },
        0,
        "{}",
        take_error()
    );

    for (sample_rate, max_iters) in [(0, 1), (1, 0)] {
        let options = LanceIndexTrainerOptions {
            sample_rate: optional(sample_rate),
            max_iters: optional(max_iters),
        };
        let mut codebook = FFI_ArrowArray::empty();
        let mut codebook_schema = FFI_ArrowSchema::empty();
        let rc = unsafe {
            lance_index_train_pq_model_with_options(
                dataset,
                column.as_ptr(),
                2,
                4,
                LanceMetricType::L2 as i32,
                ptr::null(),
                0,
                &mut centroids_array,
                &centroids_schema,
                &options,
                &mut codebook,
                &mut codebook_schema,
            )
        };
        if rc == 0 {
            println!(
                "C_PQ sample_rate={sample_rate} max_iters={max_iters}: Ok release={}",
                codebook.release.is_some()
            );
            release(&mut codebook, &mut codebook_schema);
        } else {
            println!(
                "C_PQ sample_rate={sample_rate} max_iters={max_iters}: {}",
                take_error()
            );
        }
    }

    release(&mut centroids_array, &mut centroids_schema);
    unsafe { lance_dataset_close(dataset) };

    let mut legacy_array = FFI_ArrowArray::empty();
    let mut legacy_schema = FFI_ArrowSchema::empty();
    let legacy = unsafe {
        lance_index_train_ivf_model(
            ptr::null(),
            ptr::null(),
            0,
            0,
            ptr::null(),
            0,
            &mut legacy_array,
            &mut legacy_schema,
        )
    };
    println!("LEGACY_NULL_RC {legacy}");
}
