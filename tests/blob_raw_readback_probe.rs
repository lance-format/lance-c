// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Diagnostic probe: raw v2.2 file containing an exported Blob descriptor
//! column, read through the public C reader and through the native reader.
//!
//! This is a temporary investigation artifact, not a regression test.

use std::ffi::{CStr, CString};
use std::ptr;
use std::sync::Arc;

use arrow::ffi::from_ffi_and_data_type;
use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema};
use arrow::ffi_stream::ArrowArrayStreamReader;
use arrow::record_batch::RecordBatchReader;
use arrow_array::{Array, ArrayRef, BinaryArray, Int32Array, RecordBatch, StructArray, make_array};
use arrow_schema::{DataType, Field, Schema};
use futures::{StreamExt, TryStreamExt};
use lance_c::*;
use lance_core::cache::LanceCache;
use lance_encoding::decoder::{DecoderPlugins, FilterExpression};
use lance_file::reader::{
    CachedFileMetadata, FileReader as NativeFileReader, FileReaderOptions as NativeReaderOptions,
    ReaderProjection,
};
use lance_file::version::LanceFileVersion;
use lance_file::versions;
use lance_file::writer::FileWriterOptions as NativeWriterOptions;
use lance_io::ReadBatchParams;
use lance_io::object_store::ObjectStore;
use lance_io::scheduler::{ScanScheduler, SchedulerConfig};
use lance_io::utils::CachedFileSize;

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

fn panic_on_error(what: &str) {
    let (code, message) = take_error();
    panic!("{what}: {code:?}: {message}");
}

fn fixture_blob_batch(
    root: &std::path::Path,
    sidecar_name: &str,
    field_name: &str,
    include_id: bool,
) -> RecordBatch {
    let session = unsafe {
        lance_file_session_new(
            c_string(root.to_str().unwrap()).as_ptr(),
            ptr::null(),
            ptr::null(),
            ptr::null(),
        )
    };
    if session.is_null() {
        panic_on_error("session open");
    }
    let data_file = c_string(sidecar_name);
    let writer =
        unsafe { lance_file_session_open_packed_blob_writer(session, data_file.as_ptr(), 7_u32) };
    if writer.is_null() {
        panic_on_error("packed blob writer open");
    }
    let payloads = BinaryArray::from(vec![Some(b"first".as_slice()), None, Some(b"".as_slice())]);
    let mut payload_array = FFI_ArrowArray::empty();
    let payload_schema =
        FFI_ArrowSchema::try_from(&Field::new("payloads", DataType::Binary, true)).unwrap();
    unsafe {
        ptr::write(&mut payload_array, FFI_ArrowArray::new(&payloads.to_data()));
    }
    assert_eq!(
        unsafe {
            lance_packed_blob_writer_write_blobs(writer, &mut payload_array, &payload_schema)
        },
        0
    );

    let mut descriptor_array = FFI_ArrowArray::empty();
    let mut descriptor_schema = FFI_ArrowSchema::empty();
    assert_eq!(
        unsafe {
            lance_packed_blob_writer_finish_array(
                writer,
                c_string(field_name).as_ptr(),
                &mut descriptor_array,
                &mut descriptor_schema,
            )
        },
        0
    );
    unsafe { lance_packed_blob_writer_close(writer) };
    unsafe { lance_file_session_close(session) };

    let descriptor_field = Field::try_from(&descriptor_schema).unwrap();
    println!(
        "descriptor field name={} type={:?} nullable={} metadata={:?}",
        descriptor_field.name(),
        descriptor_field.data_type(),
        descriptor_field.is_nullable(),
        descriptor_field.metadata()
    );
    let data =
        unsafe { from_ffi_and_data_type(descriptor_array, descriptor_field.data_type().clone()) }
            .unwrap();
    data.validate_full().unwrap();
    let descriptor_array: ArrayRef = make_array(data);

    let mut fields = Vec::new();
    let mut arrays = Vec::new();
    if include_id {
        fields.push(Field::new("id", DataType::Int32, false));
        arrays.push(Arc::new(Int32Array::from(vec![10, 11, 12])) as ArrayRef);
    }
    fields.push(descriptor_field);
    arrays.push(descriptor_array);
    RecordBatch::try_new(Arc::new(Schema::new(fields)), arrays).unwrap()
}

fn write_with_c(uri: &str, batch: &RecordBatch) {
    let schema = FFI_ArrowSchema::try_from(batch.schema().as_ref()).unwrap();
    let version = c"2.2";
    let options = LanceFileWriterOptions {
        has_data_cache_bytes: 0,
        data_cache_bytes: 0,
        has_max_page_bytes: 0,
        max_page_bytes: 0,
        has_keep_original_array: 0,
        keep_original_array: 0,
        version: version.as_ptr(),
    };
    let uri = c_string(uri);
    let writer = unsafe {
        lance_file_writer_open(uri.as_ptr(), &schema, &options, ptr::null(), ptr::null())
    };
    if writer.is_null() {
        panic_on_error("C file writer open");
    }
    let mut array = FFI_ArrowArray::empty();
    let batch_schema = FFI_ArrowSchema::try_from(batch.schema().as_ref()).unwrap();
    let struct_array = StructArray::from(batch.clone());
    unsafe { ptr::write(&mut array, FFI_ArrowArray::new(&struct_array.into_data())) };
    let mut imported = ptr::null_mut();
    assert_eq!(
        unsafe { lance_batch_from_arrow(&mut array, &batch_schema, &mut imported) },
        0
    );
    assert_eq!(
        unsafe { lance_file_writer_write_batch(writer, imported) },
        0
    );
    unsafe { lance_batch_free(imported) };
    let mut summary = LanceFileWriteSummary::default();
    assert_eq!(unsafe { lance_file_writer_finish(writer, &mut summary) }, 0);
    unsafe { lance_file_writer_close(writer) };
    println!("C write {uri:?}: {summary:?}");
}

fn write_with_native(uri: &str, batch: &RecordBatch) {
    lance_c::runtime::block_on(async {
        let (store, path) = ObjectStore::from_uri(uri).await.unwrap();
        let object_writer = store.create(&path).await.unwrap();
        let schema = lance_core::datatypes::Schema::try_from(batch.schema().as_ref()).unwrap();
        let mut writer = versions::create_writer(
            LanceFileVersion::V2_2.resolve(),
            object_writer,
            schema,
            NativeWriterOptions::default(),
        )
        .unwrap();
        writer.write_batch(batch).await.unwrap();
        let summary = writer.finish().await.unwrap();
        println!("native write {uri:?}: {summary:?}");
    });
}

fn open_c_reader_result(
    uri: &str,
    options: Option<&LanceFileReaderOptions>,
) -> Result<*mut LanceFileReader, (LanceErrorCode, String)> {
    let uri = c_string(uri);
    let options_ptr = options.map_or(ptr::null(), |options| options as *const _);
    let reader = unsafe {
        lance_file_reader_open(
            uri.as_ptr(),
            options_ptr,
            ptr::null(),
            ptr::null(),
            ptr::null(),
        )
    };
    if reader.is_null() {
        Err(take_error())
    } else {
        Ok(reader)
    }
}

fn metadata_info(reader: *const LanceFileReader) -> LanceFileMetadataInfo {
    let mut metadata = ptr::null_mut();
    assert_eq!(
        unsafe { lance_file_reader_get_metadata(reader, &mut metadata) },
        0
    );
    let mut info = LanceFileMetadataInfo::default();
    assert_eq!(
        unsafe { lance_file_metadata_get_info(metadata, &mut info) },
        0
    );
    unsafe { lance_file_metadata_close(metadata) };
    info
}

fn c_read_all(uri: &str) -> Result<usize, String> {
    c_read_all_with_options(uri, None)
}

fn c_read_all_columns(uri: &str, columns: &[&str]) -> Result<usize, String> {
    let columns: Vec<CString> = columns.iter().map(|column| c_string(column)).collect();
    let column_ptrs: Vec<*const std::ffi::c_char> =
        columns.iter().map(|column| column.as_ptr()).collect();
    let options = LanceFileReaderOptions {
        has_columns: 1,
        columns: column_ptrs.as_ptr(),
        column_count: column_ptrs.len(),
        ..Default::default()
    };
    println!("C read_all columns={columns:?}");
    c_read_all_with_options(uri, Some(&options))
}

fn c_read_all_with_options(
    uri: &str,
    options: Option<&LanceFileReaderOptions>,
) -> Result<usize, String> {
    let reader = open_c_reader_result(uri, options)
        .map_err(|(code, text)| format!("C open failed: {code:?}: {text}"))?;
    let info = metadata_info(reader);
    println!(
        "C metadata: version={} rows={} columns={} physical={}",
        info.format_version, info.num_rows, info.num_columns, info.num_physical_columns
    );
    let mut stream = arrow::ffi_stream::FFI_ArrowArrayStream::empty();
    let status = unsafe { lance_file_reader_read_all(reader, 3, 2, &mut stream) };
    if status != 0 {
        let (code, text) = take_error();
        unsafe { lance_file_reader_close(reader) };
        return Err(format!(
            "C read_all failed: status={status} {code:?}: {text}"
        ));
    }
    let mut stream_reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream) }.unwrap();
    let schema = stream_reader.schema();
    println!("C read schema: {schema:#?}");
    let mut rows = 0;
    while let Some(batch) = stream_reader.next() {
        match batch {
            Ok(batch) => {
                rows += batch.num_rows();
                println!("C batch: {batch:?}");
            }
            Err(error) => {
                unsafe { lance_file_reader_close(reader) };
                return Err(format!("C stream item failed: {error}"));
            }
        }
    }
    unsafe { lance_file_reader_close(reader) };
    Ok(rows)
}

fn native_metadata(uri: &str) -> Result<Arc<CachedFileMetadata>, String> {
    lance_c::runtime::block_on(async {
        let (store, path) = ObjectStore::from_uri(uri)
            .await
            .map_err(|error| format!("native store open: {error}"))?;
        let scheduler = ScanScheduler::new(store, SchedulerConfig::default_for_testing());
        let file = scheduler
            .open_file(&path, &CachedFileSize::unknown())
            .await
            .map_err(|error| format!("native file open: {error}"))?;
        NativeFileReader::read_all_metadata(&file)
            .await
            .map(Arc::new)
            .map_err(|error| format!("native read_all_metadata: {error}"))
    })
}

fn describe_schema(metadata: &CachedFileMetadata) {
    println!(
        "native metadata: version={:?} rows={} column_metadatas={} column_infos={}",
        metadata.version,
        metadata.num_rows,
        metadata.column_metadatas.len(),
        metadata.column_infos.len()
    );
    for (index, info) in metadata.column_infos.iter().enumerate() {
        println!(
            "  column_info[{index}]: pages={} encoding={:?}",
            info.page_infos.len(),
            info.encoding
        );
    }
    for field in metadata.file_schema.fields.iter() {
        println!(
            "  field name={:?} id={} type={:?} blob={} packed={} metadata={:?}",
            field.name,
            field.id,
            field.logical_type,
            field.is_blob(),
            field.is_packed_struct(),
            field.metadata
        );
        for child in &field.children {
            println!(
                "    child name={:?} id={} type={:?} metadata={:?}",
                child.name, child.id, child.logical_type, child.metadata
            );
        }
    }
    let projection =
        versions::reader_projection_from_whole_schema(&metadata.file_schema, metadata.version);
    println!(
        "whole-schema projection: indices={:?} fields={}",
        projection.column_indices,
        projection.schema.fields.len()
    );
}

fn native_read_all(uri: &str) -> Result<usize, String> {
    native_read_all_with_projection(uri, None)
}

fn native_read_all_columns(uri: &str, columns: &[&str]) -> Result<usize, String> {
    let metadata = native_metadata(uri)?;
    describe_schema(&metadata);
    let projection = versions::reader_projection_from_column_names(
        metadata.version,
        &metadata.file_schema,
        columns,
    )
    .map_err(|error| format!("native projection from columns: {error}"))?;
    println!(
        "explicit native projection columns={columns:?}: indices={:?}",
        projection.column_indices
    );
    native_read_all_with_projection(uri, Some(projection))
}

fn native_read_all_with_projection(
    uri: &str,
    projection: Option<ReaderProjection>,
) -> Result<usize, String> {
    let metadata = native_metadata(uri)?;
    if projection.is_none() {
        describe_schema(&metadata);
    }
    lance_c::runtime::block_on(async {
        let (store, path) = ObjectStore::from_uri(uri)
            .await
            .map_err(|error| format!("native store open: {error}"))?;
        let scheduler = ScanScheduler::new(store, SchedulerConfig::default_for_testing());
        let file = scheduler
            .open_file(&path, &CachedFileSize::unknown())
            .await
            .map_err(|error| format!("native file open: {error}"))?;
        let io = Arc::new(
            lance_file::LanceEncodingsIo::new(file)
                .with_read_chunk_size(NativeReaderOptions::default().read_chunk_size),
        );
        let reader = NativeFileReader::try_open_with_file_metadata(
            io,
            path,
            projection,
            Arc::<DecoderPlugins>::default(),
            metadata,
            &LanceCache::no_cache(),
            NativeReaderOptions::default(),
        )
        .await
        .map_err(|error| format!("native try_open: {error}"))?;
        println!("native reader schema: {:#?}", reader.schema());
        let stream = reader
            .read_stream(
                ReadBatchParams::RangeFull,
                3,
                2,
                FilterExpression::no_filter(),
            )
            .await
            .map_err(|error| format!("native read_stream: {error}"))?;
        let batches = stream
            .map(|batch| batch.map_err(|error| error.to_string()))
            .try_collect::<Vec<_>>()
            .await
            .map_err(|error| format!("native stream item: {error}"))?;
        for batch in &batches {
            println!("native batch: {batch:?}");
        }
        Ok(batches.iter().map(RecordBatch::num_rows).sum())
    })
}

fn probe_case(root: &std::path::Path, name: &str, include_id: bool, use_c_writer: bool) {
    let uri = root.join(format!("{name}.lance"));
    let uri = uri.to_str().unwrap();
    let batch = fixture_blob_batch(root, &format!("{name}-payloads.lance"), "blob", include_id);
    println!(
        "\n=== {name}: writer={} include_id={include_id} rows={} ===",
        if use_c_writer { "C" } else { "native" },
        batch.num_rows()
    );
    println!("input schema: {:#?}", batch.schema());
    if use_c_writer {
        write_with_c(uri, &batch);
    } else {
        write_with_native(uri, &batch);
    }
    match c_read_all(uri) {
        Ok(rows) => println!("C read_all OK rows={rows}"),
        Err(error) => println!("C read_all ERROR: {error}"),
    }
    match native_read_all(uri) {
        Ok(rows) => println!("native read_all OK rows={rows}"),
        Err(error) => println!("native read_all ERROR: {error}"),
    }
}

fn probe_projection(root: &std::path::Path, name: &str) {
    let uri = root.join(format!("{name}.lance"));
    let uri = uri.to_str().unwrap();
    println!("\n=== {name}: explicit blob-column projection ===");
    match c_read_all_columns(uri, &["blob"]) {
        Ok(rows) => println!("C read_all blob column OK rows={rows}"),
        Err(error) => println!("C read_all blob column ERROR: {error}"),
    }
    match native_read_all_columns(uri, &["blob"]) {
        Ok(rows) => println!("native read_all blob column OK rows={rows}"),
        Err(error) => println!("native read_all blob column ERROR: {error}"),
    }
}

#[test]
#[ignore = "diagnostic probe; run explicitly with --ignored"]
fn raw_blob_readback_probe() {
    let root = tempfile::tempdir().unwrap();
    probe_case(root.path(), "c_blob_only", false, true);
    probe_case(root.path(), "c_id_blob", true, true);
    probe_case(root.path(), "n_blob_only", false, false);
    probe_case(root.path(), "n_id_blob", true, false);
    probe_projection(root.path(), "c_blob_only");
    probe_projection(root.path(), "n_blob_only");
}
