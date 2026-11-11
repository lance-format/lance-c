// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Temporary native oracle for publishing new Blob rows through manual
//! Fragment metadata construction and the existing Append path.

use std::num::NonZeroUsize;
use std::sync::Arc;

use arrow_array::{ArrayRef, RecordBatch, UInt32Array};
use arrow_schema::{DataType, Field, Schema};
use lance::dataset::transaction::{Operation, Transaction};
use lance::dataset::write::CommitBuilder;
use lance::dataset::{WriteMode, WriteParams};
use lance::io::ObjectStore;
use lance::{
    BlobArrayBuilder, BlobDescriptorArrayBuilder, BlobFieldOptions, Dataset, DedicatedBlobWriter,
    PackedBlobWriter, blob_field_with_options,
};
use lance_core::datatypes::Schema as LanceSchema;
use lance_file::version::ConcreteFileVersion;
use lance_file::writer::FileWriterOptions;
use lance_table::format::Fragment;

fn native_file_writer(
    store: &ObjectStore,
    data_dir: &object_store::path::Path,
    file_name: &str,
    schema: Arc<Schema>,
) -> lance_file::writer::FileWriter {
    let object_writer =
        lance_c::runtime::block_on(store.create(&data_dir.clone().join(file_name))).unwrap();
    lance_file::versions::create_writer(
        ConcreteFileVersion::V2_2,
        object_writer,
        LanceSchema::try_from(schema.as_ref()).unwrap(),
        FileWriterOptions::default(),
    )
    .unwrap()
}

fn write_data_file(
    store: &ObjectStore,
    data_dir: &object_store::path::Path,
    file_name: &str,
    schema: Arc<Schema>,
    batch: &RecordBatch,
) {
    let mut writer = native_file_writer(store, data_dir, file_name, schema);
    lance_c::runtime::block_on(writer.write_batch(batch)).unwrap();
    let summary = lance_c::runtime::block_on(writer.finish()).unwrap();
    assert_eq!(summary.num_rows, batch.num_rows() as u64);
}

fn descriptor_column(descriptors: Vec<lance::BlobDescriptor>) -> (Field, ArrayRef) {
    let mut builder = BlobDescriptorArrayBuilder::new("blob");
    builder.extend(descriptors).unwrap();
    builder.finish().unwrap().into_parts()
}

#[test]
fn native_manual_fragment_append_public_blob_path() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("blob_append_oracle").display().to_string();

    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::UInt32, false),
        blob_field_with_options(
            "blob",
            true,
            BlobFieldOptions {
                inline_size_threshold: Some(16),
                dedicated_size_threshold: NonZeroUsize::new(256),
            },
        ),
    ]));

    let mut initial_blobs = BlobArrayBuilder::new(2);
    initial_blobs.push_bytes(b"old-packed").unwrap();
    initial_blobs.push_null().unwrap();
    let initial = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(UInt32Array::from(vec![0, 1])),
            initial_blobs.finish().unwrap(),
        ],
    )
    .unwrap();

    let dataset = Arc::new(lance_c::runtime::block_on(async {
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(initial)], schema.clone()),
            &uri,
            Some(WriteParams {
                data_storage_version: Some(lance_file::version::LanceFileVersion::V2_2),
                enable_stable_row_ids: true,
                ..Default::default()
            }),
        )
        .await
        .unwrap()
    }));
    let old_snapshot = dataset.clone();
    let initial_fragments = dataset.fragments().len();
    let initial_rows = lance_c::runtime::block_on(dataset.count_rows(None)).unwrap();
    assert_eq!(initial_rows, 2);

    let (store, _) = lance_c::runtime::block_on(ObjectStore::from_uri(&uri)).unwrap();
    let data_dir = dataset.data_dir();

    let packed_name = "oracle-packed-append.lance";
    let packed_path = data_dir.clone().join(packed_name);
    let mut packed_writer = lance_c::runtime::block_on(PackedBlobWriter::try_new(
        store.as_ref().clone(),
        packed_path,
        9001,
    ))
    .unwrap();
    lance_c::runtime::block_on(packed_writer.write_packed_blobs([
        Some(b"new-packed".as_slice()),
        None,
        Some(b"".as_slice()),
    ]))
    .unwrap();
    let (packed_field, packed_array) =
        descriptor_column(lance_c::runtime::block_on(packed_writer.finish()).unwrap());
    let packed_schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::UInt32, false),
        packed_field,
    ]));
    let packed_batch = RecordBatch::try_new(
        packed_schema.clone(),
        vec![Arc::new(UInt32Array::from(vec![2, 3, 4])), packed_array],
    )
    .unwrap();
    write_data_file(&store, &data_dir, packed_name, packed_schema, &packed_batch);

    let dedicated_name = "oracle-dedicated-append.lance";
    let dedicated_path = data_dir.clone().join(dedicated_name);
    let mut dedicated_writer = lance_c::runtime::block_on(DedicatedBlobWriter::try_new(
        store.as_ref().clone(),
        dedicated_path,
        9002,
    ))
    .unwrap();
    lance_c::runtime::block_on(dedicated_writer.write(b"new-dedicated")).unwrap();
    let dedicated_nonempty = lance_c::runtime::block_on(dedicated_writer.finish()).unwrap();

    let mut empty_writer = lance_c::runtime::block_on(DedicatedBlobWriter::try_new(
        store.as_ref().clone(),
        data_dir.clone().join(dedicated_name),
        9003,
    ))
    .unwrap();
    lance_c::runtime::block_on(empty_writer.write(b"")).unwrap();
    let dedicated_empty = lance_c::runtime::block_on(empty_writer.finish()).unwrap();

    let (dedicated_field, dedicated_array) =
        descriptor_column(vec![dedicated_nonempty, dedicated_empty]);
    let dedicated_schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::UInt32, false),
        dedicated_field,
    ]));
    let dedicated_batch = RecordBatch::try_new(
        dedicated_schema.clone(),
        vec![Arc::new(UInt32Array::from(vec![5, 6])), dedicated_array],
    )
    .unwrap();
    write_data_file(
        &store,
        &data_dir,
        dedicated_name,
        dedicated_schema,
        &dedicated_batch,
    );

    let packed_file =
        lance_c::runtime::block_on(dataset.create_data_file(packed_name, None)).unwrap();
    let dedicated_file =
        lance_c::runtime::block_on(dataset.create_data_file(dedicated_name, None)).unwrap();
    assert_eq!(
        packed_file.file_version().unwrap(),
        ConcreteFileVersion::V2_2
    );
    assert_eq!(
        dedicated_file.file_version().unwrap(),
        ConcreteFileVersion::V2_2
    );

    let mut packed_fragment = Fragment::new(0);
    packed_fragment.files.push(packed_file);
    packed_fragment.physical_rows = Some(3);
    let mut dedicated_fragment = Fragment::new(0);
    dedicated_fragment.files.push(dedicated_file);
    dedicated_fragment.physical_rows = Some(2);

    let transaction = Transaction::new(
        dataset.version().version,
        Operation::Append {
            fragments: vec![packed_fragment, dedicated_fragment],
        },
        None,
    );
    let committed =
        lance_c::runtime::block_on(CommitBuilder::new(dataset).execute(transaction)).unwrap();
    assert_eq!(
        committed.version().version,
        old_snapshot.version().version + 1
    );
    assert_eq!(committed.fragments().len(), initial_fragments + 2);
    assert_eq!(
        lance_c::runtime::block_on(committed.count_rows(None)).unwrap(),
        initial_rows + 5
    );

    let reopened = Arc::new(lance_c::runtime::block_on(Dataset::open(&uri)).unwrap());
    assert_eq!(reopened.version().version, committed.version().version);
    assert_eq!(
        lance_c::runtime::block_on(reopened.count_rows(None)).unwrap(),
        initial_rows + 5
    );
    let blobs =
        lance_c::runtime::block_on(reopened.take_blobs_by_indices(&[0, 1, 2, 3, 4, 5, 6], "blob"))
            .unwrap();
    let expected = [
        Some(b"old-packed".as_slice()),
        None,
        Some(b"new-packed".as_slice()),
        None,
        Some(b"".as_slice()),
        Some(b"new-dedicated".as_slice()),
        Some(b"".as_slice()),
    ];
    for (blob, expected) in blobs.iter().zip(expected) {
        match expected {
            Some(expected) => {
                let blob = blob.as_ref().unwrap();
                assert_eq!(
                    lance_c::runtime::block_on(blob.read()).unwrap().as_ref(),
                    expected
                );
            }
            None => assert!(blob.is_none()),
        }
    }

    assert_eq!(
        lance_c::runtime::block_on(old_snapshot.count_rows(None)).unwrap(),
        initial_rows
    );
    let old_blobs =
        lance_c::runtime::block_on(old_snapshot.take_blobs_by_indices(&[0, 1], "blob")).unwrap();
    assert_eq!(
        lance_c::runtime::block_on(old_blobs[0].as_ref().unwrap().read())
            .unwrap()
            .as_ref(),
        b"old-packed"
    );
    assert!(old_blobs[1].is_none());
}
