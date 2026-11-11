// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Probe for issue #114 / #167: does native `FileFragment::create_from_file`
//! accept a raw v2.2 data file that carries a Blob v2 column?
//!
//! The same data file is handed to the public native API and to the C entry
//! point `lance_fragment_create_from_file`, so a failure can be attributed to
//! core rather than the binding. A plain v2.2 dataset without a Blob column
//! acts as the control.

use std::ffi::{CStr, CString};
use std::ptr;
use std::sync::Arc;

use arrow_array::{RecordBatch, UInt32Array};
use arrow_schema::{DataType, Field, Schema};
use lance::BlobArrayBuilder;
use lance::dataset::fragment::FileFragment;
use lance::dataset::{Dataset, WriteMode, WriteParams};
use lance_file::version::LanceFileVersion;

fn blob_schema() -> Arc<Schema> {
    Arc::new(Schema::new(vec![
        Field::new("id", DataType::UInt32, false),
        lance::blob_field_with_options(
            "blob",
            true,
            lance::BlobFieldOptions {
                inline_size_threshold: Some(16),
                dedicated_size_threshold: std::num::NonZeroUsize::new(256),
            },
        ),
    ]))
}

fn blob_batch(schema: &Arc<Schema>, first_id: u32) -> RecordBatch {
    let mut blobs = BlobArrayBuilder::new(3);
    blobs.push_bytes(b"packed-payload".to_vec()).unwrap();
    blobs.push_bytes(vec![7_u8; 1024]).unwrap();
    blobs.push_null().unwrap();
    RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(UInt32Array::from(vec![
                first_id,
                first_id + 1,
                first_id + 2,
            ])),
            blobs.finish().unwrap(),
        ],
    )
    .unwrap()
}

fn plain_schema() -> Arc<Schema> {
    Arc::new(Schema::new(vec![
        Field::new("id", DataType::UInt32, false),
        Field::new("payload", DataType::Binary, true),
    ]))
}

fn plain_batch(schema: &Arc<Schema>, first_id: u32) -> RecordBatch {
    use arrow_array::BinaryArray;
    RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(UInt32Array::from(vec![first_id, first_id + 1])),
            Arc::new(BinaryArray::from_vec(vec![
                b"a".as_slice(),
                b"bb".as_slice(),
            ])),
        ],
    )
    .unwrap()
}

async fn data_files(uri: &str) -> Vec<String> {
    let dataset = Dataset::open(uri).await.unwrap();
    let mut paths = Vec::new();
    for fragment in dataset.get_fragments() {
        let metadata = fragment.metadata();
        for file in &metadata.files {
            let name = std::path::Path::new(&file.path)
                .file_name()
                .expect("data file path has a file name")
                .to_string_lossy()
                .into_owned();
            paths.push(name);
        }
    }
    paths
}

fn report_native(uri: &str, file: &str) {
    lance_c::runtime::block_on(async {
        let dataset = Dataset::open(uri).await.unwrap();
        match FileFragment::create_from_file(file, &dataset, 9_001, None).await {
            Ok(fragment) => {
                let metadata = fragment.metadata();
                println!(
                    "native create_from_file OK file={file} id={} physical_rows={:?} files={:?}",
                    metadata.id,
                    metadata.physical_rows,
                    metadata
                        .files
                        .iter()
                        .map(|f| f.path.clone())
                        .collect::<Vec<_>>()
                );
            }
            Err(error) => println!("native create_from_file ERR file={file}: {error}"),
        }
    });
}

unsafe fn last_error() -> String {
    let message = lance_c::lance_last_error_message();
    if message.is_null() {
        return String::new();
    }
    let text = CStr::from_ptr(message).to_string_lossy().into_owned();
    lance_c::lance_free_string(message);
    text
}

fn report_c(uri: &str, file: &str) {
    let uri_c = CString::new(uri).unwrap();
    let file_c = CString::new(file).unwrap();
    unsafe {
        let dataset = lance_c::lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0);
        if dataset.is_null() {
            println!("C dataset open FAILED: {}", last_error());
            return;
        }
        let mut out: *mut lance_c::LanceFragment = ptr::null_mut();
        let code = lance_c::lance_fragment_create_from_file(
            dataset,
            file_c.as_ptr(),
            9_002,
            ptr::null(),
            &mut out,
        );
        if code == 0 && !out.is_null() {
            let json = lance_c::lance_fragment_metadata_json(out);
            println!(
                "C create_from_file OK file={file} code={code} metadata={}",
                CStr::from_ptr(json).to_string_lossy()
            );
            lance_c::lance_free_string(json);
            lance_c::lance_fragment_close(out);
        } else {
            println!(
                "C create_from_file ERR file={file} code={code} message={}",
                last_error()
            );
        }
        lance_c::lance_dataset_close(dataset);
    }
}

fn main() {
    let root = std::env::temp_dir().join("lance-c-fragment-blob-probe");
    let _ = std::fs::remove_dir_all(&root);
    let blob_uri = root.join("blob_ds").to_str().unwrap().to_string();
    let plain_uri = root.join("plain_ds").to_str().unwrap().to_string();

    let blob_schema = blob_schema();
    let plain_schema = plain_schema();
    lance_c::runtime::block_on(async {
        let params = |mode| WriteParams {
            mode,
            data_storage_version: Some(LanceFileVersion::V2_2),
            ..Default::default()
        };
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(
                vec![Ok(blob_batch(&blob_schema, 0))],
                blob_schema.clone(),
            ),
            &blob_uri,
            Some(params(WriteMode::Create)),
        )
        .await
        .unwrap();
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(
                vec![Ok(blob_batch(&blob_schema, 10))],
                blob_schema.clone(),
            ),
            &blob_uri,
            Some(params(WriteMode::Append)),
        )
        .await
        .unwrap();
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(
                vec![Ok(plain_batch(&plain_schema, 0))],
                plain_schema.clone(),
            ),
            &plain_uri,
            Some(params(WriteMode::Create)),
        )
        .await
        .unwrap();
    });

    let plain_files = lance_c::runtime::block_on(data_files(&plain_uri));
    let blob_files = lance_c::runtime::block_on(data_files(&blob_uri));
    println!("plain data files: {plain_files:?}");
    println!("blob data files: {blob_files:?}");

    for file in &plain_files {
        report_native(&plain_uri, file);
        report_c(&plain_uri, file);
    }
    for file in &blob_files {
        report_native(&blob_uri, file);
        report_c(&blob_uri, file);
    }
}
