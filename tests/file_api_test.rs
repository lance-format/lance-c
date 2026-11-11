// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

use std::collections::HashMap;
use std::ffi::{CStr, CString, c_char, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use arrow::buffer::NullBuffer;
use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema};
use arrow::ffi_stream::ArrowArrayStreamReader;
use arrow::record_batch::RecordBatchReader;
use arrow_array::builder::{Int32Builder, ListBuilder};
use arrow_array::{
    Array, ArrayRef, BinaryArray, BooleanArray, DictionaryArray, Int8Array, Int32Array,
    RecordBatch, RecordBatchOptions, StringArray, StructArray,
};
use arrow_schema::{DataType, Field, Fields, Schema};
use futures::StreamExt;
use lance_c::*;
use lance_core::cache::LanceCache;
use lance_encoding::decoder::{DecoderPlugins, FilterExpression};
use lance_file::reader::{
    CachedFileMetadata, FileReader as NativeFileReader, FileReaderOptions as NativeReaderOptions,
    describe_encoding,
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

fn panic_on_error() {
    let (code, message) = take_error();
    panic!("unexpected C API error {code:?}: {message}");
}

fn fixture_schema() -> Arc<Schema> {
    let id = Field::new("id", DataType::Int32, false).with_metadata(HashMap::from([(
        "fixture.field".to_owned(),
        "fixed".to_owned(),
    )]));
    let label = Field::new("label", DataType::Utf8, true).with_metadata(HashMap::from([
        ("ARROW:extension:name".to_owned(), "lance-c.test".to_owned()),
        ("ARROW:extension:metadata".to_owned(), "v1".to_owned()),
    ]));
    let category = Field::new(
        "category",
        DataType::Dictionary(Box::new(DataType::Int8), Box::new(DataType::Utf8)),
        true,
    );
    let nested = Field::new(
        "nested",
        DataType::Struct(Fields::from(vec![
            Field::new("flag", DataType::Boolean, true),
            Field::new(
                "items",
                DataType::List(Arc::new(Field::new("item", DataType::Int32, true))),
                true,
            ),
        ])),
        true,
    );
    let payload = Field::new("payload", DataType::Binary, true);
    Arc::new(Schema::new_with_metadata(
        vec![id, label, category, nested, payload],
        HashMap::from([("fixture.schema".to_owned(), "lance-c".to_owned())]),
    ))
}

fn fixture_batch() -> RecordBatch {
    fixture_batch_with_nested_nulls(true)
}

fn fixture_batch_with_nested_nulls(include_nested_nulls: bool) -> RecordBatch {
    let schema = fixture_schema();
    let ids = Arc::new(Int32Array::from_iter_values(1..=7)) as ArrayRef;
    let labels = Arc::new(StringArray::from(vec![
        Some("alpha"),
        None,
        Some("gamma"),
        Some(""),
        Some("epsilon"),
        None,
        Some("eta"),
    ])) as ArrayRef;
    let categories = Arc::new(
        DictionaryArray::try_new(
            Int8Array::from(vec![
                Some(0),
                Some(1),
                None,
                Some(0),
                Some(2),
                Some(1),
                Some(0),
            ]),
            Arc::new(StringArray::from(vec!["a", "b", "c"])),
        )
        .unwrap(),
    ) as ArrayRef;

    let flags = Arc::new(BooleanArray::from(vec![
        Some(true),
        Some(false),
        None,
        Some(true),
        Some(false),
        None,
        Some(true),
    ])) as ArrayRef;
    let mut items = ListBuilder::new(Int32Builder::new());
    for values in [
        Some(vec![1, 2]),
        Some(Vec::new()),
        None,
        Some(vec![3]),
        Some(vec![4, 5]),
        None,
        Some(vec![6]),
    ] {
        if let Some(values) = values {
            for value in values {
                items.values().append_value(value);
            }
            items.append(true);
        } else {
            items.append(false);
        }
    }
    let nested_nulls = include_nested_nulls
        .then(|| NullBuffer::from(vec![true, true, false, true, true, false, true]));
    let nested = Arc::new(
        StructArray::try_new(
            Fields::from(vec![
                Field::new("flag", DataType::Boolean, true),
                Field::new(
                    "items",
                    DataType::List(Arc::new(Field::new("item", DataType::Int32, true))),
                    true,
                ),
            ]),
            vec![flags, Arc::new(items.finish())],
            nested_nulls,
        )
        .unwrap(),
    ) as ArrayRef;
    let payload = Arc::new(BinaryArray::from_iter([
        Some(&b"a\0b"[..]),
        None,
        Some(&b""[..]),
        Some(&b"payload"[..]),
        None,
        Some(&b"\xff"[..]),
        Some(&b"last"[..]),
    ])) as ArrayRef;

    RecordBatch::try_new(schema, vec![ids, labels, categories, nested, payload]).unwrap()
}

fn zero_column_batch() -> RecordBatch {
    RecordBatch::try_new_with_options(
        Arc::new(Schema::empty()),
        Vec::new(),
        &RecordBatchOptions::new().with_row_count(Some(3)),
    )
    .unwrap()
}

fn ffi_pair(batch: &RecordBatch) -> (FFI_ArrowArray, FFI_ArrowSchema) {
    let struct_array: StructArray = batch.clone().into();
    let array = FFI_ArrowArray::new(&struct_array.into_data());
    let schema = FFI_ArrowSchema::try_from(batch.schema().as_ref()).unwrap();
    (array, schema)
}

fn import_batch(batch: &RecordBatch) -> *mut LanceBatch {
    let (mut array, schema) = ffi_pair(batch);
    let mut imported = ptr::null_mut();
    let status = unsafe { lance_batch_from_arrow(&mut array, &schema, &mut imported) };
    assert_eq!(status, 0, "batch import failed");
    assert!(array.release.is_none(), "input array must be consumed");
    assert!(schema.release.is_some(), "schema remains caller-owned");
    imported
}

#[allow(clippy::too_many_arguments)]
fn write_fixture(
    uri: &str,
    version: Option<&str>,
    lazy: bool,
    keep_original_array: Option<bool>,
    data_cache_bytes: Option<u64>,
    max_page_bytes: Option<u64>,
    global_buffer: Option<&[u8]>,
    schema_metadata: bool,
) -> (LanceFileWriteSummary, Option<u32>) {
    let uri_text = uri.to_owned();
    let batch = if version == Some("2.0") {
        fixture_batch_with_nested_nulls(false)
    } else {
        fixture_batch()
    };
    let ffi_schema = FFI_ArrowSchema::try_from(batch.schema().as_ref()).unwrap();
    let version = version.map(c_string);
    let options = LanceFileWriterOptions {
        has_data_cache_bytes: data_cache_bytes.is_some() as i32,
        data_cache_bytes: data_cache_bytes.unwrap_or(0),
        has_max_page_bytes: max_page_bytes.is_some() as i32,
        max_page_bytes: max_page_bytes.unwrap_or(0),
        has_keep_original_array: keep_original_array.is_some() as i32,
        keep_original_array: keep_original_array.unwrap_or(false) as i32,
        version: version
            .as_ref()
            .map_or(ptr::null(), |version| version.as_ptr()),
    };
    let uri = c_string(uri);
    let writer = unsafe {
        lance_file_writer_open(
            uri.as_ptr(),
            if lazy { ptr::null() } else { &ffi_schema },
            &options,
            ptr::null(),
            ptr::null(),
        )
    };
    if writer.is_null() {
        panic_on_error();
    }

    let raw_batch = import_batch(&batch);
    let write_status = unsafe { lance_file_writer_write_batch(writer, raw_batch) };
    if write_status != 0 {
        let (code, message) = take_error();
        unsafe {
            lance_batch_free(raw_batch);
            lance_file_writer_close(writer);
        }
        panic!("write batch failed for {uri_text}: {code:?}: {message}");
    }
    unsafe { lance_batch_free(raw_batch) };

    if schema_metadata {
        assert_eq!(
            unsafe {
                lance_file_writer_add_schema_metadata(
                    writer,
                    c"owner".as_ptr(),
                    c"lance-c".as_ptr(),
                )
            },
            0
        );
    }
    let buffer_index = global_buffer.map(|bytes| {
        let mut index = 0;
        assert_eq!(
            unsafe {
                lance_file_writer_add_global_buffer(writer, bytes.as_ptr(), bytes.len(), &mut index)
            },
            0
        );
        index
    });

    let mut summary = LanceFileWriteSummary::default();
    assert_eq!(
        unsafe { lance_file_writer_finish(writer, &mut summary) },
        0,
        "finish failed"
    );
    unsafe { lance_file_writer_close(writer) };
    (summary, buffer_index)
}

fn open_c_reader(uri: &str, options: Option<&LanceFileReaderOptions>) -> *mut LanceFileReader {
    let uri = c_string(uri);
    let reader = unsafe {
        lance_file_reader_open(
            uri.as_ptr(),
            ptr::null(),
            options.map_or(ptr::null(), |options| options),
            ptr::null(),
            ptr::null(),
        )
    };
    if reader.is_null() {
        panic_on_error();
    }
    reader
}

fn collect_c_stream(
    mut stream: arrow::ffi_stream::FFI_ArrowArrayStream,
) -> (Arc<Schema>, Vec<RecordBatch>) {
    assert!(stream.release.is_some(), "stream was not initialized");
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream) }.unwrap();
    let schema = reader.schema();
    let batches = reader.map(|batch| batch.unwrap()).collect();
    (schema, batches)
}

fn read_all_c(
    uri: &str,
    options: Option<&LanceFileReaderOptions>,
) -> (Arc<Schema>, Vec<RecordBatch>) {
    let reader = open_c_reader(uri, options);
    let mut stream = arrow::ffi_stream::FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_file_reader_read_all(reader, 3, 2, &mut stream) },
        0
    );
    let result = collect_c_stream(stream);
    unsafe { lance_file_reader_close(reader) };
    result
}

fn native_reader(uri: &str) -> NativeFileReader {
    lance_c::runtime::block_on(async {
        let (store, path) = ObjectStore::from_uri(uri).await.unwrap();
        let scheduler = ScanScheduler::new(store, SchedulerConfig::default_for_testing());
        let file = scheduler
            .open_file(&path, &CachedFileSize::unknown())
            .await
            .unwrap();
        let metadata = NativeFileReader::read_all_metadata(&file).await.unwrap();
        let io = Arc::new(lance_file::LanceEncodingsIo::new(file));
        NativeFileReader::try_open_with_file_metadata(
            io,
            path,
            None,
            Arc::<DecoderPlugins>::default(),
            Arc::new(metadata),
            &LanceCache::no_cache(),
            NativeReaderOptions::default(),
        )
        .await
        .unwrap()
    })
}

fn native_batches(reader: &NativeFileReader) -> Vec<RecordBatch> {
    lance_c::runtime::block_on(async {
        let stream = reader
            .read_stream(
                ReadBatchParams::RangeFull,
                3,
                2,
                FilterExpression::no_filter(),
            )
            .await
            .unwrap();
        stream.map(|batch| batch.unwrap()).collect().await
    })
}

fn assert_batches_match_ignoring_root_metadata(left: &[RecordBatch], right: &[RecordBatch]) {
    assert_eq!(left.len(), right.len());
    for (left, right) in left.iter().zip(right.iter()) {
        assert_eq!(left.num_rows(), right.num_rows());
        assert_eq!(left.schema().fields(), right.schema().fields());
        assert_eq!(left.columns(), right.columns());
    }
}

fn native_metadata(uri: &str) -> Arc<CachedFileMetadata> {
    native_reader(uri).metadata().clone()
}

fn assert_fixture_batches(batches: &[RecordBatch]) {
    assert_fixture_batches_expected(batches, &fixture_batch());
}

fn assert_fixture_batches_expected(batches: &[RecordBatch], expected: &RecordBatch) {
    let rows: usize = batches.iter().map(RecordBatch::num_rows).sum();
    assert_eq!(rows, 7);
    let actual = arrow::compute::concat_batches(&expected.schema(), batches).unwrap();
    assert_eq!(actual.schema(), expected.schema());
    assert_eq!(actual.num_rows(), expected.num_rows());
    for (index, (actual, expected)) in actual
        .columns()
        .iter()
        .zip(expected.columns().iter())
        .enumerate()
    {
        assert_eq!(actual.as_ref(), expected.as_ref(), "column {index}");
    }
}

fn format_version(version: LanceFileVersion) -> i32 {
    match version.resolve() {
        lance_file::version::ConcreteFileVersion::V1 => LanceFileFormatVersion::V1 as i32,
        lance_file::version::ConcreteFileVersion::V2_0 => LanceFileFormatVersion::V2_0 as i32,
        lance_file::version::ConcreteFileVersion::V2_1 => LanceFileFormatVersion::V2_1 as i32,
        lance_file::version::ConcreteFileVersion::V2_2 => LanceFileFormatVersion::V2_2 as i32,
        lance_file::version::ConcreteFileVersion::V2_3 => LanceFileFormatVersion::V2_3 as i32,
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

fn native_success<T, E>(operation: impl FnOnce() -> Result<T, E>) -> bool {
    catch_unwind(AssertUnwindSafe(operation))
        .map(|result| result.is_ok())
        .unwrap_or(false)
}

#[test]
fn versions_declared_lazy_and_defaults_round_trip_against_native() {
    let root = tempfile::tempdir().unwrap();
    for version in ["2.0", "2.1", "2.2", "2.3"] {
        let uri = root.path().join(format!("v{version}.lance"));
        let uri = uri.to_str().unwrap();
        let (summary, _) = write_fixture(uri, Some(version), false, None, None, None, None, false);
        assert_eq!(summary.num_rows, 7);
        assert!(summary.size_bytes > 0);

        let reader = open_c_reader(uri, None);
        let info = metadata_info(reader);
        let expected_version = version.parse::<LanceFileVersion>().unwrap();
        assert_eq!(info.format_version, format_version(expected_version));
        assert_eq!(info.num_rows, 7);
        assert_eq!(info.num_columns, 5);
        assert!(info.num_physical_columns >= 5);
        unsafe { lance_file_reader_close(reader) };

        let (schema, batches) = read_all_c(uri, None);
        let expected = if version == "2.0" {
            fixture_batch_with_nested_nulls(false)
        } else {
            fixture_batch()
        };
        assert_fixture_batches_expected(&batches, &expected);
        assert_eq!(schema.metadata(), fixture_schema().metadata());

        let oracle = native_reader(uri);
        assert_batches_match_ignoring_root_metadata(&batches, &native_batches(&oracle));
    }

    let lazy = root.path().join("lazy.lance");
    let lazy = lazy.to_str().unwrap();
    write_fixture(lazy, None, true, None, None, None, None, false);
    assert_fixture_batches(&read_all_c(lazy, None).1);

    let default_uri = root.path().join("default.lance");
    let default_uri = default_uri.to_str().unwrap();
    write_fixture(default_uri, None, false, None, None, None, None, false);
    let reader = open_c_reader(default_uri, None);
    assert_eq!(
        metadata_info(reader).format_version,
        format_version(LanceFileVersion::default())
    );
    unsafe { lance_file_reader_close(reader) };

    for (name, version, expected) in [
        ("stable", "stable", LanceFileVersion::Stable),
        ("next", "next", LanceFileVersion::Next),
    ] {
        let uri = root.path().join(format!("{name}.lance"));
        let uri = uri.to_str().unwrap();
        write_fixture(uri, Some(version), false, None, None, None, None, false);
        let reader = open_c_reader(uri, None);
        assert_eq!(
            metadata_info(reader).format_version,
            format_version(expected)
        );
        unsafe { lance_file_reader_close(reader) };
    }

    let unsupported = root.path().join("v2.0-struct-null.lance");
    let unsupported = unsupported.to_str().unwrap();
    let uri_c = c_string(unsupported);
    let batch = fixture_batch();
    let schema = FFI_ArrowSchema::try_from(batch.schema().as_ref()).unwrap();
    let version = c"2.0";
    let options = LanceFileWriterOptions {
        version: version.as_ptr(),
        ..Default::default()
    };
    let writer = unsafe {
        lance_file_writer_open(uri_c.as_ptr(), &schema, &options, ptr::null(), ptr::null())
    };
    assert!(!writer.is_null());
    let raw_batch = import_batch(&batch);
    assert_eq!(
        unsafe { lance_file_writer_write_batch(writer, raw_batch) },
        -1
    );
    let (code, message) = take_error();
    assert_eq!(code, LanceErrorCode::InvalidArgument);
    assert!(message.contains("2.0"));
    assert!(message.contains("struct"));
    unsafe {
        lance_batch_free(raw_batch);
        lance_file_writer_close(writer);
    }
}

#[test]
fn metadata_statistics_pages_and_global_buffers_match_native() {
    let root = tempfile::tempdir().unwrap();
    let uri = root.path().join("metadata.lance");
    let uri = uri.to_str().unwrap();
    let global_buffer = b"\0global\xffbuffer";
    let (_, buffer_index) = write_fixture(
        uri,
        Some("2.2"),
        false,
        Some(false),
        Some(0),
        Some(1 << 20),
        Some(global_buffer),
        true,
    );
    assert_eq!(buffer_index, Some(1));

    let reader = open_c_reader(uri, None);
    let native = native_metadata(uri);
    let info = metadata_info(reader);
    assert_eq!(info.major_version, native.major_version);
    assert_eq!(info.minor_version, native.minor_version);
    assert_eq!(info.num_rows, native.num_rows);
    assert_eq!(info.num_data_bytes, native.num_data_bytes);
    assert_eq!(
        info.num_column_metadata_bytes,
        native.num_column_metadata_bytes
    );
    assert_eq!(info.num_global_buffer_bytes, native.num_global_buffer_bytes);
    assert_eq!(info.num_footer_bytes, native.num_footer_bytes);
    assert_eq!(info.file_size_bytes, native.file_size_bytes);
    assert_eq!(info.num_columns, native.file_schema.fields.len());
    assert_eq!(info.num_physical_columns, native.column_metadatas.len());
    assert_eq!(info.num_global_buffers, native.file_buffers.len());

    let mut metadata = ptr::null_mut();
    assert_eq!(
        unsafe { lance_file_reader_get_metadata(reader, &mut metadata) },
        0
    );
    let mut schema = FFI_ArrowSchema::empty();
    assert_eq!(
        unsafe { lance_file_metadata_get_schema(metadata, &mut schema) },
        0
    );
    let schema = Schema::try_from(&schema).unwrap();
    let native_schema: Schema = native.file_schema.as_ref().into();
    assert_eq!(schema, native_schema);
    assert_eq!(
        schema.metadata().get("owner").map(String::as_str),
        Some("lance-c")
    );
    assert_eq!(
        schema
            .field_with_name("label")
            .unwrap()
            .metadata()
            .get("ARROW:extension:name")
            .map(String::as_str),
        Some("lance-c.test")
    );

    let mut global_buffers = ptr::null();
    let mut global_count = 0;
    assert_eq!(
        unsafe {
            lance_file_metadata_get_global_buffers(metadata, &mut global_buffers, &mut global_count)
        },
        0
    );
    assert_eq!(global_count, native.file_buffers.len());
    for (index, native_buffer) in native.file_buffers.iter().enumerate() {
        let buffer = unsafe { &*global_buffers.add(index) };
        assert_eq!(buffer.position, native_buffer.position);
        assert_eq!(buffer.size, native_buffer.size);
    }

    let mut columns = ptr::null();
    let mut column_count = 0;
    assert_eq!(
        unsafe { lance_file_metadata_get_columns(metadata, &mut columns, &mut column_count) },
        0
    );
    assert_eq!(column_count, native.column_metadatas.len());
    for (column_index, native_column) in native.column_metadatas.iter().enumerate() {
        let column = unsafe { &*columns.add(column_index) };
        assert_eq!(
            column.column_buffer_count,
            native_column.buffer_offsets.len()
        );
        assert_eq!(column.page_count, native_column.pages.len());
        for page_index in 0..column.page_count {
            let page = unsafe { &*column.pages.add(page_index) };
            let native_page = &native_column.pages[page_index];
            assert_eq!(page.num_rows, native_page.length);
            assert_eq!(page.priority, native_page.priority);
            assert_eq!(
                unsafe { CStr::from_ptr(page.encoding_description) }
                    .to_str()
                    .unwrap(),
                describe_encoding(native_page)
            );
        }
    }

    let mut stats = ptr::null_mut();
    assert_eq!(
        unsafe { lance_file_reader_get_statistics(reader, &mut stats) },
        0
    );
    let native_stats = native_reader(uri).file_statistics();
    let mut stat_columns = ptr::null();
    let mut stat_count = 0;
    assert_eq!(
        unsafe { lance_file_statistics_get_columns(stats, &mut stat_columns, &mut stat_count) },
        0
    );
    assert_eq!(stat_count, native_stats.columns.len());
    for (index, native_column) in native_stats.columns.iter().enumerate() {
        let column = unsafe { &*stat_columns.add(index) };
        assert_eq!(column.num_pages, native_column.num_pages);
        assert_eq!(column.size_bytes, native_column.size_bytes);
    }

    let mut user_buffer = ptr::null_mut();
    assert_eq!(
        unsafe {
            lance_file_reader_read_global_buffer(reader, buffer_index.unwrap(), &mut user_buffer)
        },
        0
    );
    let user_bytes = unsafe {
        std::slice::from_raw_parts(
            lance_byte_buffer_data(user_buffer),
            lance_byte_buffer_len(user_buffer),
        )
    };
    assert_eq!(user_bytes, global_buffer);
    unsafe {
        lance_byte_buffer_close(user_buffer);
        lance_file_statistics_close(stats);
        lance_file_metadata_close(metadata);
        lance_file_reader_close(reader);
    }
}

#[test]
fn reader_projection_range_indices_errors_and_io_stats() {
    let root = tempfile::tempdir().unwrap();
    let uri = root.path().join("reader.lance");
    let uri = uri.to_str().unwrap();
    write_fixture(uri, Some("2.2"), false, None, None, None, None, false);

    let columns = [c"id".as_ptr(), c"label".as_ptr()];
    let options = LanceFileReaderOptions {
        has_columns: 1,
        columns: columns.as_ptr(),
        column_count: columns.len(),
        cache_mode: LanceFileCacheMode::None as i32,
        ..Default::default()
    };
    let (schema, batches) = read_all_c(uri, Some(&options));
    assert_eq!(schema.fields().len(), 2);
    assert_eq!(batches.iter().map(RecordBatch::num_rows).sum::<usize>(), 7);

    let reader = open_c_reader(uri, None);
    let mut range = arrow::ffi_stream::FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_file_reader_read_range(reader, 2, 3, 2, 2, &mut range) },
        0
    );
    let (_, range_batches) = collect_c_stream(range);
    let range = arrow::compute::concat_batches(&fixture_schema(), &range_batches).unwrap();
    let ids = range
        .column_by_name("id")
        .unwrap()
        .as_any()
        .downcast_ref::<Int32Array>()
        .unwrap();
    assert_eq!(ids.values(), &[3, 4, 5]);

    let indices = [0u32, 2, 6];
    let mut stream = arrow::ffi_stream::FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe {
            lance_file_reader_read_indices(
                reader,
                indices.as_ptr(),
                indices.len(),
                8,
                1,
                &mut stream,
            )
        },
        0
    );
    let (_, index_batches) = collect_c_stream(stream);
    let selected = arrow::compute::concat_batches(&fixture_schema(), &index_batches).unwrap();
    assert_eq!(
        selected
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap()
            .values(),
        &[1, 3, 7]
    );

    let mut stats = LanceScanStats::default();
    assert_eq!(
        unsafe { lance_file_reader_get_io_stats(reader, &mut stats) },
        0
    );
    assert!(stats.bytes_read > 0);

    let descending = [3u32, 2];
    let mut failed_stream = arrow::ffi_stream::FFI_ArrowArrayStream::empty();
    let descending_status = unsafe {
        lance_file_reader_read_indices(
            reader,
            descending.as_ptr(),
            descending.len(),
            2,
            1,
            &mut failed_stream,
        )
    };
    if descending_status == 0 {
        let error = Arc::new(Mutex::new(None));
        let error_clone = error.clone();
        let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut failed_stream) }.unwrap();
        for batch in reader {
            if let Err(error_value) = batch {
                *error_clone.lock().unwrap() = Some(error_value.to_string());
                break;
            }
        }
        assert!(error.lock().unwrap().is_some());
    } else {
        let (_, message) = take_error();
        assert!(message.to_lowercase().contains("increasing"));
    }

    let mut overflow_stream = arrow::ffi_stream::FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_file_reader_read_range(reader, u64::MAX, 1, 2, 1, &mut overflow_stream,) },
        -1
    );
    let (code, message) = take_error();
    assert_eq!(code, LanceErrorCode::InvalidArgument);
    assert!(message.contains("overflow"));

    let mut buffer = ptr::null_mut();
    assert_eq!(
        unsafe { lance_file_reader_read_global_buffer(reader, 999, &mut buffer) },
        -1
    );
    assert!(buffer.is_null());

    unsafe { lance_file_reader_close(reader) };

    let empty_options = LanceFileReaderOptions {
        has_columns: 1,
        columns: ptr::null(),
        column_count: 0,
        ..Default::default()
    };
    let uri_c = c_string(uri);
    let empty_reader = unsafe {
        lance_file_reader_open(
            uri_c.as_ptr(),
            ptr::null(),
            &empty_options,
            ptr::null(),
            ptr::null(),
        )
    };
    assert!(empty_reader.is_null());
    let (code, _) = take_error();
    assert_eq!(code, LanceErrorCode::InvalidArgument);

    let unknown = c"missing";
    let unknown_columns = [unknown.as_ptr()];
    let unknown_options = LanceFileReaderOptions {
        has_columns: 1,
        columns: unknown_columns.as_ptr(),
        column_count: 1,
        ..Default::default()
    };
    let unknown_reader = unsafe {
        lance_file_reader_open(
            uri_c.as_ptr(),
            ptr::null(),
            &unknown_options,
            ptr::null(),
            ptr::null(),
        )
    };
    assert!(unknown_reader.is_null());
}

#[test]
fn batch_import_export_preserves_schema_and_zero_column_rows() {
    let batch = fixture_batch();
    let (mut array, schema) = ffi_pair(&batch);
    let mut imported = ptr::null_mut();
    assert_eq!(
        unsafe { lance_batch_from_arrow(&mut array, &schema, &mut imported) },
        0
    );
    assert!(array.release.is_none());
    assert!(schema.release.is_some());

    let mut exported_array = FFI_ArrowArray::empty();
    let mut exported_schema = FFI_ArrowSchema::empty();
    assert_eq!(
        unsafe { lance_batch_to_arrow(imported, &mut exported_array, &mut exported_schema) },
        0
    );
    let exported_schema_value = Schema::try_from(&exported_schema).unwrap();
    assert_eq!(exported_schema_value, batch.schema().as_ref().clone());
    let exported_array = unsafe {
        arrow::ffi::from_ffi_and_data_type(
            exported_array,
            DataType::Struct(batch.schema().fields().clone()),
        )
    }
    .map(StructArray::from)
    .unwrap();
    assert_eq!(exported_array.len(), 7);
    unsafe { lance_batch_free(imported) };

    let zero = zero_column_batch();
    let (mut zero_array, zero_schema) = ffi_pair(&zero);
    let mut zero_imported = ptr::null_mut();
    assert_eq!(
        unsafe { lance_batch_from_arrow(&mut zero_array, &zero_schema, &mut zero_imported) },
        0
    );
    let mut zero_exported_array = FFI_ArrowArray::empty();
    let mut zero_exported_schema = FFI_ArrowSchema::empty();
    assert_eq!(
        unsafe {
            lance_batch_to_arrow(
                zero_imported,
                &mut zero_exported_array,
                &mut zero_exported_schema,
            )
        },
        0
    );
    assert_eq!(zero_exported_array.length, 3);
    assert_eq!(zero_exported_array.n_children, 0);
    assert_eq!(
        Schema::try_from(&zero_exported_schema).unwrap(),
        Schema::empty()
    );
    unsafe {
        drop(FFI_ArrowArray::from_raw(&mut zero_exported_array));
        lance_batch_free(zero_imported);
    }

    let mut consumed = ffi_pair(&batch).0;
    let mut sentinel = ptr::dangling_mut::<LanceBatch>();
    let original_sentinel = sentinel;
    assert_eq!(
        unsafe { lance_batch_from_arrow(&mut consumed, ptr::null(), &mut sentinel) },
        -1
    );
    assert!(
        consumed.release.is_none(),
        "array must be consumed on error"
    );
    assert_eq!(
        sentinel, original_sentinel,
        "out must be unchanged on error"
    );
}

#[test]
fn writer_zero_column_and_failure_boundaries_are_native() {
    let root = tempfile::tempdir().unwrap();
    let zero_uri = root.path().join("zero.lance");
    let zero_uri_c = c_string(zero_uri.to_str().unwrap());
    let zero_batch = zero_column_batch();
    let zero_schema = FFI_ArrowSchema::try_from(zero_batch.schema().as_ref()).unwrap();
    let writer = unsafe {
        lance_file_writer_open(
            zero_uri_c.as_ptr(),
            &zero_schema,
            ptr::null(),
            ptr::null(),
            ptr::null(),
        )
    };
    assert!(!writer.is_null());
    let raw_zero = import_batch(&zero_batch);
    let write_status = unsafe { lance_file_writer_write_batch(writer, raw_zero) };
    let mut summary = LanceFileWriteSummary::default();
    let finish_status = if write_status == 0 {
        unsafe { lance_file_writer_finish(writer, &mut summary) }
    } else {
        -1
    };
    if write_status != 0 || finish_status != 0 {
        let (code, message) = take_error();
        assert_eq!(code, LanceErrorCode::InvalidArgument);
        assert!(!message.is_empty());
    } else {
        assert_eq!(summary.num_rows, 3);
        let reader = open_c_reader(zero_uri.to_str().unwrap(), None);
        assert_eq!(metadata_info(reader).num_rows, 3);
        let empty_columns = LanceFileReaderOptions {
            has_columns: 1,
            columns: ptr::null(),
            column_count: 0,
            ..Default::default()
        };
        let uri = c_string(zero_uri.to_str().unwrap());
        let empty_reader = unsafe {
            lance_file_reader_open(
                uri.as_ptr(),
                ptr::null(),
                &empty_columns,
                ptr::null(),
                ptr::null(),
            )
        };
        assert!(empty_reader.is_null());
        let (code, _) = take_error();
        assert_eq!(code, LanceErrorCode::InvalidArgument);
        unsafe { lance_file_reader_close(reader) };
    }
    unsafe {
        lance_batch_free(raw_zero);
        lance_file_writer_close(writer);
    }

    let cache_uri = root.path().join("zero-cache.lance");
    let cache_uri_c = c_string(cache_uri.to_str().unwrap());
    let cache_options = LanceFileWriterOptions {
        has_data_cache_bytes: 1,
        data_cache_bytes: 1024,
        ..Default::default()
    };
    let cache_writer = unsafe {
        lance_file_writer_open(
            cache_uri_c.as_ptr(),
            &zero_schema,
            &cache_options,
            ptr::null(),
            ptr::null(),
        )
    };
    if !cache_writer.is_null() {
        let raw_zero = import_batch(&zero_batch);
        let status = unsafe { lance_file_writer_write_batch(cache_writer, raw_zero) };
        if status == 0 {
            let mut summary = LanceFileWriteSummary::default();
            let status = unsafe { lance_file_writer_finish(cache_writer, &mut summary) };
            if status != 0 {
                let (code, _) = take_error();
                assert_eq!(code, LanceErrorCode::Panic);
            }
        } else {
            let (code, _) = take_error();
            assert_eq!(code, LanceErrorCode::Panic);
        }
        unsafe {
            lance_batch_free(raw_zero);
            lance_file_writer_close(cache_writer);
        }
    } else {
        let (code, _) = take_error();
        assert_eq!(code, LanceErrorCode::Panic);
    }

    let empty_uri = root.path().join("empty.lance");
    let empty_uri_c = c_string(empty_uri.to_str().unwrap());
    let batch = fixture_batch();
    let schema = FFI_ArrowSchema::try_from(batch.schema().as_ref()).unwrap();
    let writer = unsafe {
        lance_file_writer_open(
            empty_uri_c.as_ptr(),
            &schema,
            ptr::null(),
            ptr::null(),
            ptr::null(),
        )
    };
    assert!(!writer.is_null());
    let mut summary = LanceFileWriteSummary::default();
    assert_eq!(unsafe { lance_file_writer_finish(writer, &mut summary) }, 0);
    assert_eq!(summary.num_rows, 0);
    unsafe { lance_file_writer_close(writer) };
    assert_eq!(read_all_c(empty_uri.to_str().unwrap(), None).1.len(), 0);

    let lazy_uri = root.path().join("lazy-empty.lance");
    let lazy_uri_c = c_string(lazy_uri.to_str().unwrap());
    let writer = unsafe {
        lance_file_writer_open(
            lazy_uri_c.as_ptr(),
            ptr::null(),
            ptr::null(),
            ptr::null(),
            ptr::null(),
        )
    };
    assert!(!writer.is_null());
    let mut summary = LanceFileWriteSummary::default();
    assert_eq!(
        unsafe { lance_file_writer_finish(writer, &mut summary) },
        -1
    );
    let (code, message) = take_error();
    assert_eq!(code, LanceErrorCode::InvalidArgument);
    assert!(!message.is_empty());
    unsafe { lance_file_writer_close(writer) };
}

#[test]
fn writer_repeated_abort_and_failure_follow_native_state() {
    let root = tempfile::tempdir().unwrap();
    let batch = fixture_batch();

    let native_uri = root.path().join("native-repeat.lance");
    let native_uri = native_uri.to_str().unwrap();
    let c_uri = root.path().join("c-repeat.lance");
    let c_uri = c_uri.to_str().unwrap();

    let mut native = lance_c::runtime::block_on(async {
        let (store, path) = ObjectStore::from_uri(native_uri).await.unwrap();
        let object_writer = store.create(&path).await.unwrap();
        let schema = lance_core::datatypes::Schema::try_from(batch.schema().as_ref()).unwrap();
        versions::create_writer(
            LanceFileVersion::V2_2.resolve(),
            object_writer,
            schema,
            NativeWriterOptions::default(),
        )
        .unwrap()
    });
    assert!(native_success(|| lance_c::runtime::block_on(
        native.write_batch(&batch)
    )));
    let native_first = native_success(|| lance_c::runtime::block_on(native.finish()));
    let native_second = native_success(|| lance_c::runtime::block_on(native.finish()));

    let ffi_schema = FFI_ArrowSchema::try_from(batch.schema().as_ref()).unwrap();
    let raw_batch = import_batch(&batch);
    let uri_c = c_string(c_uri);
    let writer = unsafe {
        lance_file_writer_open(
            uri_c.as_ptr(),
            &ffi_schema,
            ptr::null(),
            ptr::null(),
            ptr::null(),
        )
    };
    assert!(!writer.is_null());
    assert_eq!(
        unsafe { lance_file_writer_write_batch(writer, raw_batch) },
        0
    );
    let mut summary = LanceFileWriteSummary::default();
    let c_first = unsafe { lance_file_writer_finish(writer, &mut summary) } == 0;
    let c_second = unsafe { lance_file_writer_finish(writer, &mut summary) } == 0;
    assert_eq!(c_first, native_first);
    assert_eq!(c_second, native_second);
    unsafe {
        lance_batch_free(raw_batch);
        lance_file_writer_close(writer);
    }

    let native_abort_uri = root.path().join("native-abort.lance");
    let mut native_abort = lance_c::runtime::block_on(async {
        let (store, path) = ObjectStore::from_uri(native_abort_uri.to_str().unwrap())
            .await
            .unwrap();
        let object_writer = store.create(&path).await.unwrap();
        let schema = lance_core::datatypes::Schema::try_from(batch.schema().as_ref()).unwrap();
        versions::create_writer(
            LanceFileVersion::V2_2.resolve(),
            object_writer,
            schema,
            NativeWriterOptions::default(),
        )
        .unwrap()
    });
    lance_c::runtime::block_on(native_abort.abort());
    let native_abort_write =
        native_success(|| lance_c::runtime::block_on(native_abort.write_batch(&batch)));
    let native_abort_finish = native_success(|| lance_c::runtime::block_on(native_abort.finish()));

    let abort_uri = root.path().join("c-abort.lance");
    let abort_uri = abort_uri.to_str().unwrap();
    let ffi_schema = FFI_ArrowSchema::try_from(batch.schema().as_ref()).unwrap();
    let uri_c = c_string(abort_uri);
    let abort_writer = unsafe {
        lance_file_writer_open(
            uri_c.as_ptr(),
            &ffi_schema,
            ptr::null(),
            ptr::null(),
            ptr::null(),
        )
    };
    assert!(!abort_writer.is_null());
    let raw_batch = import_batch(&batch);
    assert_eq!(unsafe { lance_file_writer_abort(abort_writer) }, 0);
    let c_abort_write = unsafe { lance_file_writer_write_batch(abort_writer, raw_batch) } == 0;
    let mut summary = LanceFileWriteSummary::default();
    let c_abort_finish = unsafe { lance_file_writer_finish(abort_writer, &mut summary) } == 0;
    assert_eq!(c_abort_write, native_abort_write);
    assert_eq!(c_abort_finish, native_abort_finish);
    unsafe {
        lance_batch_free(raw_batch);
        lance_file_writer_close(abort_writer);
    }

    let native_lazy_uri = root.path().join("native-lazy-fail.lance");
    let mut native_lazy = lance_c::runtime::block_on(async {
        let (store, path) = ObjectStore::from_uri(native_lazy_uri.to_str().unwrap())
            .await
            .unwrap();
        let object_writer = store.create(&path).await.unwrap();
        versions::create_lazy_writer(
            LanceFileVersion::V2_2.resolve(),
            object_writer,
            NativeWriterOptions::default(),
        )
        .unwrap()
    });
    let native_failed_finish = !native_success(|| lance_c::runtime::block_on(native_lazy.finish()));
    let native_after_failure =
        native_success(|| lance_c::runtime::block_on(native_lazy.write_batch(&batch)));

    let lazy_uri = root.path().join("c-lazy-fail.lance");
    let lazy_uri = lazy_uri.to_str().unwrap();
    let lazy_uri_c = c_string(lazy_uri);
    let lazy_writer = unsafe {
        lance_file_writer_open(
            lazy_uri_c.as_ptr(),
            ptr::null(),
            ptr::null(),
            ptr::null(),
            ptr::null(),
        )
    };
    assert!(!lazy_writer.is_null());
    let mut summary = LanceFileWriteSummary::default();
    let c_failed_finish = unsafe { lance_file_writer_finish(lazy_writer, &mut summary) } != 0;
    if c_failed_finish {
        let _ = take_error();
    }
    let raw_batch = import_batch(&batch);
    let c_after_failure = unsafe { lance_file_writer_write_batch(lazy_writer, raw_batch) } == 0;
    assert_eq!(c_failed_finish, native_failed_finish);
    assert_eq!(c_after_failure, native_after_failure);
    unsafe {
        lance_batch_free(raw_batch);
        lance_file_writer_close(lazy_writer);
    }
}

#[test]
fn malformed_files_and_v1_are_rejected_without_panic() {
    let root = tempfile::tempdir().unwrap();
    let valid = root.path().join("valid.lance");
    let valid = valid.to_str().unwrap();
    write_fixture(valid, Some("2.2"), false, None, None, None, None, false);
    let bytes = std::fs::read(valid).unwrap();

    let cases = [
        ("truncated", bytes[..bytes.len() - 12].to_vec()),
        ("magic", {
            let mut bytes = bytes.clone();
            let len = bytes.len();
            bytes[len - 4..].copy_from_slice(b"FAIL");
            bytes
        }),
        ("cmo", {
            let mut bytes = bytes.clone();
            let len = bytes.len();
            let footer = &bytes[len - 40..];
            let table_start = u64::from_le_bytes(footer[8..16].try_into().unwrap()) as usize;
            bytes[table_start..table_start + 8].copy_from_slice(&0u64.to_le_bytes());
            bytes
        }),
        ("v1", {
            let mut bytes = bytes.clone();
            let len = bytes.len();
            bytes[len - 8..len - 6].copy_from_slice(&0u16.to_le_bytes());
            bytes[len - 6..len - 4].copy_from_slice(&1u16.to_le_bytes());
            bytes
        }),
    ];

    for (name, malformed) in cases {
        let uri = root.path().join(format!("{name}.lance"));
        std::fs::write(&uri, malformed).unwrap();
        let uri_string = uri.to_str().unwrap();
        let uri_c = c_string(uri_string);
        let reader = unsafe {
            lance_file_reader_open(
                uri_c.as_ptr(),
                ptr::null(),
                ptr::null(),
                ptr::null(),
                ptr::null(),
            )
        };
        assert!(reader.is_null(), "{name} unexpectedly opened");
        let (code, message) = take_error();
        assert_ne!(code, LanceErrorCode::Ok);
        assert!(!message.is_empty(), "{name} returned no error message");
    }
}

#[derive(Clone, Default)]
struct ProviderCounters {
    fetches: Arc<AtomicUsize>,
    force_fetches: Arc<AtomicUsize>,
    snapshot_releases: Arc<AtomicUsize>,
    provider_releases: Arc<AtomicUsize>,
}

struct ProviderTestContext {
    counters: ProviderCounters,
    fetch_options: Option<StorageOptions>,
    force_options: Option<StorageOptions>,
    error: Option<(i32, String)>,
    expires_at_millis: Option<u64>,
    refresh_offset_millis: Option<u64>,
}

type StorageOptions = HashMap<String, String>;

struct ProviderSnapshot {
    keys: Vec<CString>,
    values: Vec<CString>,
    key_ptrs: Vec<*const c_char>,
    value_ptrs: Vec<*const c_char>,
    releases: Arc<AtomicUsize>,
}

unsafe extern "C" fn release_provider_snapshot(raw: *mut c_void) {
    let snapshot = unsafe { Box::from_raw(raw as *mut ProviderSnapshot) };
    snapshot.releases.fetch_add(1, Ordering::SeqCst);
}

unsafe extern "C" fn release_provider_owned_string(value: *mut LanceOwnedString) {
    if value.is_null() {
        return;
    }
    let value = unsafe { &mut *value };
    if !value.data.is_null() {
        let _ = unsafe { CString::from_raw(value.data) };
        value.data = ptr::null_mut();
    }
    value.release = None;
}

unsafe fn set_provider_error(error: *mut LanceCallbackError, code: i32, message: &str) {
    if error.is_null() {
        return;
    }
    let message = CString::new(message).unwrap();
    unsafe {
        (*error).code = code;
        (*error).message.data = message.into_raw();
        (*error).message.release = Some(release_provider_owned_string);
    }
}

unsafe fn write_provider_snapshot(
    out: *mut LanceStorageOptionsFetchResult,
    options: Option<StorageOptions>,
    releases: Arc<AtomicUsize>,
) {
    if out.is_null() {
        return;
    }
    let Some(options) = options else {
        unsafe { (*out).has_options = 0 };
        return;
    };
    let entries = options.into_iter().collect::<Vec<_>>();
    let mut snapshot = Box::new(ProviderSnapshot {
        keys: entries
            .iter()
            .map(|(key, _)| CString::new(key.as_str()).unwrap())
            .collect(),
        values: entries
            .iter()
            .map(|(_, value)| CString::new(value.as_str()).unwrap())
            .collect(),
        key_ptrs: Vec::new(),
        value_ptrs: Vec::new(),
        releases,
    });
    snapshot.key_ptrs = snapshot.keys.iter().map(|key| key.as_ptr()).collect();
    snapshot.value_ptrs = snapshot.values.iter().map(|value| value.as_ptr()).collect();
    unsafe {
        (*out).has_options = 1;
        (*out).keys = snapshot.key_ptrs.as_ptr();
        (*out).values = snapshot.value_ptrs.as_ptr();
        (*out).count = snapshot.key_ptrs.len();
        (*out).snapshot_ctx = Box::into_raw(snapshot) as *mut c_void;
        (*out).release_snapshot = Some(release_provider_snapshot);
    }
}

fn current_millis() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_millis() as u64
}

unsafe extern "C" fn provider_fetch(
    callback_ctx: *mut c_void,
    out: *mut LanceStorageOptionsFetchResult,
    error: *mut LanceCallbackError,
) -> i32 {
    let context = unsafe { &*(callback_ctx as *const ProviderTestContext) };
    context.counters.fetches.fetch_add(1, Ordering::SeqCst);
    if let Some((code, message)) = &context.error {
        unsafe { set_provider_error(error, *code, message) };
        return -1;
    }
    let mut options = context.fetch_options.clone();
    if let Some(options) = &mut options {
        if let Some(expires_at) = context.expires_at_millis {
            options.insert("expires_at_millis".to_owned(), expires_at.to_string());
        }
        if let Some(refresh_offset) = context.refresh_offset_millis {
            options.insert(
                "refresh_offset_millis".to_owned(),
                refresh_offset.to_string(),
            );
        }
    }
    unsafe { write_provider_snapshot(out, options, context.counters.snapshot_releases.clone()) };
    0
}

unsafe extern "C" fn provider_force_fetch(
    callback_ctx: *mut c_void,
    out: *mut LanceStorageOptionsFetchResult,
    error: *mut LanceCallbackError,
) -> i32 {
    let context = unsafe { &*(callback_ctx as *const ProviderTestContext) };
    context
        .counters
        .force_fetches
        .fetch_add(1, Ordering::SeqCst);
    if let Some((code, message)) = &context.error {
        unsafe { set_provider_error(error, *code, message) };
        return -1;
    }
    unsafe {
        write_provider_snapshot(
            out,
            context.force_options.clone(),
            context.counters.snapshot_releases.clone(),
        )
    };
    0
}

unsafe extern "C" fn provider_release(callback_ctx: *mut c_void) {
    let context = unsafe { Box::from_raw(callback_ctx as *mut ProviderTestContext) };
    context
        .counters
        .provider_releases
        .fetch_add(1, Ordering::SeqCst);
}

struct MalformedContext {
    releases: Arc<AtomicUsize>,
}

unsafe extern "C" fn release_malformed_snapshot(raw: *mut c_void) {
    let state = unsafe { Box::from_raw(raw as *mut Arc<AtomicUsize>) };
    state.fetch_add(1, Ordering::SeqCst);
}

unsafe extern "C" fn malformed_fetch(
    callback_ctx: *mut c_void,
    out: *mut LanceStorageOptionsFetchResult,
    _error: *mut LanceCallbackError,
) -> i32 {
    let context = unsafe { &*(callback_ctx as *const MalformedContext) };
    let state = Box::new(context.releases.clone());
    unsafe {
        (*out).has_options = 1;
        (*out).keys = ptr::null();
        (*out).values = ptr::null();
        (*out).count = 1;
        (*out).snapshot_ctx = Box::into_raw(state) as *mut c_void;
        (*out).release_snapshot = Some(release_malformed_snapshot);
    }
    0
}

unsafe extern "C" fn release_malformed_provider(callback_ctx: *mut c_void) {
    unsafe { drop(Box::from_raw(callback_ctx as *mut MalformedContext)) };
}

fn create_test_provider(
    context: ProviderTestContext,
    with_force: bool,
) -> (*mut LanceStorageOptionsProvider, ProviderCounters, CString) {
    let counters = context.counters.clone();
    let provider_id = c_string("test-provider");
    let context = Box::into_raw(Box::new(context));
    let spec = LanceStorageOptionsProviderSpec {
        fetch: Some(provider_fetch),
        force_fetch: with_force.then_some(provider_force_fetch),
        release: Some(provider_release),
        provider_ctx: context as *mut c_void,
        provider_id: provider_id.as_ptr(),
    };
    let provider = unsafe { lance_storage_options_provider_new(&spec) };
    if provider.is_null() {
        unsafe { drop(Box::from_raw(context)) };
        panic_on_error();
    }
    (provider, counters, provider_id)
}

fn snapshot_options(
    accessor: *const LanceStorageOptionsAccessor,
) -> Result<Option<StorageOptions>, (LanceErrorCode, String)> {
    let snapshot = unsafe { lance_storage_options_accessor_snapshot(accessor) };
    if snapshot.is_null() {
        return Err(take_error());
    }
    let has_options = unsafe { lance_storage_options_snapshot_has_options(snapshot) };
    assert!(has_options >= 0);
    if has_options == 0 {
        unsafe { lance_storage_options_snapshot_close(snapshot) };
        return Ok(None);
    }
    let count = unsafe { lance_storage_options_snapshot_count(snapshot) };
    let mut options = HashMap::new();
    for index in 0..count {
        let key = unsafe { lance_storage_options_snapshot_key_at(snapshot, index as usize) };
        let value = unsafe { lance_storage_options_snapshot_value_at(snapshot, index as usize) };
        assert!(!key.is_null());
        assert!(!value.is_null());
        options.insert(
            unsafe { CStr::from_ptr(key) }.to_str().unwrap().to_owned(),
            unsafe { CStr::from_ptr(value) }
                .to_str()
                .unwrap()
                .to_owned(),
        );
    }
    unsafe { lance_storage_options_snapshot_close(snapshot) };
    Ok(Some(options))
}

fn accessor_has_options(accessor: *const LanceStorageOptionsAccessor) -> Option<bool> {
    let snapshot = unsafe { lance_storage_options_accessor_snapshot(accessor) };
    if snapshot.is_null() {
        return None;
    }
    let has_options = unsafe { lance_storage_options_snapshot_has_options(snapshot) };
    unsafe { lance_storage_options_snapshot_close(snapshot) };
    Some(has_options != 0)
}

#[test]
fn storage_options_static_provider_optional_maps_and_release() {
    let accessor = unsafe { lance_storage_options_accessor_new(ptr::null(), ptr::null()) };
    assert!(!accessor.is_null());
    assert_eq!(accessor_has_options(accessor), Some(false));
    unsafe { lance_storage_options_accessor_close(accessor) };

    let empty_input = LanceStorageOptionsInput::default();
    let accessor = unsafe { lance_storage_options_accessor_new(&empty_input, ptr::null()) };
    assert!(!accessor.is_null());
    let snapshot = unsafe { lance_storage_options_accessor_snapshot(accessor) };
    assert_eq!(
        unsafe { lance_storage_options_snapshot_has_options(snapshot) },
        1
    );
    assert_eq!(unsafe { lance_storage_options_snapshot_count(snapshot) }, 0);
    unsafe {
        lance_storage_options_snapshot_close(snapshot);
        lance_storage_options_accessor_close(accessor);
    }

    let keys = [c"block_size".as_ptr(), c"block_size".as_ptr()];
    let values = [c"1024".as_ptr(), c"4096".as_ptr()];
    let duplicate = LanceStorageOptionsInput {
        keys: keys.as_ptr(),
        values: values.as_ptr(),
        count: 2,
    };
    let accessor = unsafe { lance_storage_options_accessor_new(&duplicate, ptr::null()) };
    let options = snapshot_options(accessor).unwrap().unwrap();
    assert_eq!(options.get("block_size").map(String::as_str), Some("4096"));
    unsafe { lance_storage_options_accessor_close(accessor) };

    let counters = ProviderCounters::default();
    let provider_context = ProviderTestContext {
        counters: counters.clone(),
        fetch_options: Some(HashMap::from([("dynamic".to_owned(), "value".to_owned())])),
        force_options: None,
        error: None,
        expires_at_millis: None,
        refresh_offset_millis: None,
    };
    let (provider, provider_counters, provider_id) = create_test_provider(provider_context, false);
    let initial = HashMap::from([("initial".to_owned(), "kept".to_owned())]);
    let initial_keys = [c"initial".as_ptr()];
    let initial_values = [c"kept".as_ptr()];
    let initial_input = LanceStorageOptionsInput {
        keys: initial_keys.as_ptr(),
        values: initial_values.as_ptr(),
        count: 1,
    };
    assert_eq!(initial.get("initial").map(String::as_str), Some("kept"));
    let accessor = unsafe { lance_storage_options_accessor_new(&initial_input, provider) };
    let options = snapshot_options(accessor).unwrap().unwrap();
    assert_eq!(options.get("initial").map(String::as_str), Some("kept"));
    assert_eq!(provider_counters.fetches.load(Ordering::SeqCst), 0);
    assert_eq!(
        provider_counters.snapshot_releases.load(Ordering::SeqCst),
        0
    );
    unsafe {
        lance_storage_options_accessor_close(accessor);
    }
    assert_eq!(
        provider_counters.provider_releases.load(Ordering::SeqCst),
        0
    );
    unsafe {
        lance_storage_options_provider_close(provider);
    }
    assert_eq!(
        provider_counters.provider_releases.load(Ordering::SeqCst),
        1,
        "final provider handle drop must release the context"
    );
    drop(provider_id);
}

#[test]
fn storage_provider_none_some_empty_error_expiry_and_force() {
    let counters = ProviderCounters::default();
    let context = ProviderTestContext {
        counters: counters.clone(),
        fetch_options: None,
        force_options: None,
        error: None,
        expires_at_millis: None,
        refresh_offset_millis: None,
    };
    let (provider, provider_counters, provider_id) = create_test_provider(context, false);
    let initial = HashMap::from([
        ("initial".to_owned(), "fallback".to_owned()),
        ("expires_at_millis".to_owned(), "0".to_owned()),
        ("refresh_offset_millis".to_owned(), "0".to_owned()),
    ]);
    let initial_keys = [
        c"initial".as_ptr(),
        c"expires_at_millis".as_ptr(),
        c"refresh_offset_millis".as_ptr(),
    ];
    let initial_values = [c"fallback".as_ptr(), c"0".as_ptr(), c"0".as_ptr()];
    let initial_input = LanceStorageOptionsInput {
        keys: initial_keys.as_ptr(),
        values: initial_values.as_ptr(),
        count: 3,
    };
    let accessor = unsafe { lance_storage_options_accessor_new(&initial_input, provider) };
    let options = snapshot_options(accessor).unwrap().unwrap();
    assert_eq!(options.get("initial").map(String::as_str), Some("fallback"));
    assert_eq!(provider_counters.fetches.load(Ordering::SeqCst), 1);
    assert_eq!(
        provider_counters.snapshot_releases.load(Ordering::SeqCst),
        0
    );
    unsafe {
        lance_storage_options_accessor_close(accessor);
        lance_storage_options_provider_close(provider);
    }
    assert_eq!(
        provider_counters.provider_releases.load(Ordering::SeqCst),
        1
    );
    drop(initial);
    drop(provider_id);

    let counters = ProviderCounters::default();
    let context = ProviderTestContext {
        counters: counters.clone(),
        fetch_options: Some(HashMap::new()),
        force_options: None,
        error: None,
        expires_at_millis: None,
        refresh_offset_millis: None,
    };
    let (provider, provider_counters, provider_id) = create_test_provider(context, false);
    let initial = HashMap::from([
        ("initial".to_owned(), "must-not-win".to_owned()),
        ("expires_at_millis".to_owned(), "0".to_owned()),
        ("refresh_offset_millis".to_owned(), "0".to_owned()),
    ]);
    let initial_keys = [
        c"initial".as_ptr(),
        c"expires_at_millis".as_ptr(),
        c"refresh_offset_millis".as_ptr(),
    ];
    let initial_values = [c"must-not-win".as_ptr(), c"0".as_ptr(), c"0".as_ptr()];
    let initial_input = LanceStorageOptionsInput {
        keys: initial_keys.as_ptr(),
        values: initial_values.as_ptr(),
        count: 3,
    };
    let accessor = unsafe { lance_storage_options_accessor_new(&initial_input, provider) };
    let options = snapshot_options(accessor).unwrap().unwrap();
    assert!(options.is_empty());
    assert_eq!(
        provider_counters.snapshot_releases.load(Ordering::SeqCst),
        1
    );
    unsafe {
        lance_storage_options_accessor_close(accessor);
        lance_storage_options_provider_close(provider);
    }
    drop(initial);
    drop(provider_id);

    let context = ProviderTestContext {
        counters: ProviderCounters::default(),
        fetch_options: None,
        force_options: None,
        error: Some((1, "provider failed".to_owned())),
        expires_at_millis: None,
        refresh_offset_millis: None,
    };
    let counters = context.counters.clone();
    let (provider, _, provider_id) = create_test_provider(context, false);
    let accessor = unsafe { lance_storage_options_accessor_new(ptr::null(), provider) };
    let (code, message) = snapshot_options(accessor).unwrap_err();
    assert_eq!(code, LanceErrorCode::IoError);
    assert!(message.contains("provider failed"));
    unsafe {
        lance_storage_options_accessor_close(accessor);
        lance_storage_options_provider_close(provider);
    }
    assert_eq!(counters.provider_releases.load(Ordering::SeqCst), 1);
    drop(provider_id);

    let counters = ProviderCounters::default();
    let context = ProviderTestContext {
        counters: counters.clone(),
        fetch_options: Some(HashMap::from([("expires".to_owned(), "cached".to_owned())])),
        force_options: None,
        error: None,
        expires_at_millis: Some(current_millis() + 150),
        refresh_offset_millis: Some(0),
    };
    let (provider, counters, provider_id) = create_test_provider(context, false);
    let initial_keys = [
        c"expires_at_millis".as_ptr(),
        c"refresh_offset_millis".as_ptr(),
    ];
    let initial_values = [c"0".as_ptr(), c"0".as_ptr()];
    let initial_input = LanceStorageOptionsInput {
        keys: initial_keys.as_ptr(),
        values: initial_values.as_ptr(),
        count: 2,
    };
    let accessor = unsafe { lance_storage_options_accessor_new(&initial_input, provider) };
    let first = snapshot_options(accessor).unwrap().unwrap();
    assert_eq!(first.get("expires").map(String::as_str), Some("cached"));
    assert_eq!(counters.fetches.load(Ordering::SeqCst), 1);
    let second = snapshot_options(accessor).unwrap().unwrap();
    assert_eq!(second.get("expires").map(String::as_str), Some("cached"));
    assert_eq!(counters.fetches.load(Ordering::SeqCst), 1);
    std::thread::sleep(Duration::from_millis(220));
    let third = snapshot_options(accessor).unwrap().unwrap();
    assert_eq!(third.get("expires").map(String::as_str), Some("cached"));
    assert_eq!(counters.fetches.load(Ordering::SeqCst), 2);
    assert_eq!(counters.snapshot_releases.load(Ordering::SeqCst), 2);
    unsafe {
        lance_storage_options_accessor_close(accessor);
        lance_storage_options_provider_close(provider);
    }
    assert_eq!(counters.provider_releases.load(Ordering::SeqCst), 1);
    drop(provider_id);

    let counters = ProviderCounters::default();
    let context = ProviderTestContext {
        counters: counters.clone(),
        fetch_options: Some(HashMap::from([
            ("aws_access_key_id".to_owned(), "fetch".to_owned()),
            (
                "aws_secret_access_key".to_owned(),
                "fetch-secret".to_owned(),
            ),
            ("region".to_owned(), "us-east-1".to_owned()),
        ])),
        force_options: Some(HashMap::from([
            ("aws_access_key_id".to_owned(), "force".to_owned()),
            (
                "aws_secret_access_key".to_owned(),
                "force-secret".to_owned(),
            ),
            ("region".to_owned(), "us-east-1".to_owned()),
        ])),
        error: None,
        expires_at_millis: None,
        refresh_offset_millis: None,
    };
    let (provider, counters, provider_id) = create_test_provider(context, true);
    let initial_keys = [c"region".as_ptr()];
    let initial_values = [c"us-east-1".as_ptr()];
    let initial_input = LanceStorageOptionsInput {
        keys: initial_keys.as_ptr(),
        values: initial_values.as_ptr(),
        count: 1,
    };
    let accessor = unsafe { lance_storage_options_accessor_new(&initial_input, provider) };
    let base = c_string("s3://test-bucket");
    let file_session =
        unsafe { lance_file_session_new(base.as_ptr(), accessor, ptr::null(), ptr::null()) };
    assert!(!file_session.is_null(), "S3 store construction failed");
    assert!(
        counters.force_fetches.load(Ordering::SeqCst) > 0,
        "native S3 credential setup did not use force-fetch"
    );
    unsafe {
        lance_file_session_close(file_session);
        lance_storage_options_accessor_close(accessor);
        lance_storage_options_provider_close(provider);
    }
    let deadline = std::time::Instant::now() + Duration::from_secs(2);
    while counters.provider_releases.load(Ordering::SeqCst) == 0
        && std::time::Instant::now() < deadline
    {
        std::thread::sleep(Duration::from_millis(10));
    }
    assert_eq!(counters.provider_releases.load(Ordering::SeqCst), 1);
    drop(provider_id);

    let counters = ProviderCounters::default();
    let context = ProviderTestContext {
        counters: counters.clone(),
        fetch_options: Some(HashMap::from([
            ("aws_access_key_id".to_owned(), "fetch".to_owned()),
            (
                "aws_secret_access_key".to_owned(),
                "fetch-secret".to_owned(),
            ),
            ("region".to_owned(), "us-east-1".to_owned()),
        ])),
        force_options: None,
        error: None,
        expires_at_millis: None,
        refresh_offset_millis: None,
    };
    let (provider, counters, provider_id) = create_test_provider(context, false);
    let initial_keys = [c"region".as_ptr()];
    let initial_values = [c"us-east-1".as_ptr()];
    let initial_input = LanceStorageOptionsInput {
        keys: initial_keys.as_ptr(),
        values: initial_values.as_ptr(),
        count: 1,
    };
    let accessor = unsafe { lance_storage_options_accessor_new(&initial_input, provider) };
    let base = c_string("s3://test-bucket");
    let file_session =
        unsafe { lance_file_session_new(base.as_ptr(), accessor, ptr::null(), ptr::null()) };
    assert!(!file_session.is_null());
    assert_eq!(
        counters.force_fetches.load(Ordering::SeqCst),
        0,
        "omitted force callback must use the native fetch default"
    );
    assert!(counters.fetches.load(Ordering::SeqCst) > 0);
    unsafe {
        lance_file_session_close(file_session);
        lance_storage_options_accessor_close(accessor);
        lance_storage_options_provider_close(provider);
    }
    assert_eq!(counters.provider_releases.load(Ordering::SeqCst), 1);
    drop(provider_id);

    let releases = Arc::new(AtomicUsize::new(0));
    let malformed_context = Box::into_raw(Box::new(MalformedContext {
        releases: releases.clone(),
    }));
    let malformed_provider_id = c_string("malformed-provider");
    let spec = LanceStorageOptionsProviderSpec {
        fetch: Some(malformed_fetch),
        force_fetch: None,
        release: Some(release_malformed_provider),
        provider_ctx: malformed_context as *mut c_void,
        provider_id: malformed_provider_id.as_ptr(),
    };
    let provider = unsafe { lance_storage_options_provider_new(&spec) };
    assert!(!provider.is_null());
    let accessor = unsafe { lance_storage_options_accessor_new(ptr::null(), provider) };
    let (code, _) = snapshot_options(accessor).unwrap_err();
    assert_eq!(code, LanceErrorCode::InvalidArgument);
    assert_eq!(releases.load(Ordering::SeqCst), 1);
    unsafe {
        lance_storage_options_accessor_close(accessor);
        lance_storage_options_provider_close(provider);
    }
    drop(malformed_provider_id);
}

fn write_fixture_with_session(
    session: *const LanceFileSession,
    relative_path: &str,
) -> LanceFileWriteSummary {
    let batch = fixture_batch();
    let schema = FFI_ArrowSchema::try_from(batch.schema().as_ref()).unwrap();
    let relative_path = c_string(relative_path);
    let writer = unsafe {
        lance_file_session_open_writer(session, relative_path.as_ptr(), &schema, ptr::null())
    };
    if writer.is_null() {
        panic_on_error();
    }
    let raw_batch = import_batch(&batch);
    assert_eq!(
        unsafe { lance_file_writer_write_batch(writer, raw_batch) },
        0
    );
    let mut summary = LanceFileWriteSummary::default();
    assert_eq!(unsafe { lance_file_writer_finish(writer, &mut summary) }, 0);
    unsafe {
        lance_batch_free(raw_batch);
        lance_file_writer_close(writer);
    }
    summary
}

fn read_fixture_with_session(
    session: *const LanceFileSession,
    relative_path: &str,
) -> Vec<RecordBatch> {
    let relative_path = c_string(relative_path);
    let reader =
        unsafe { lance_file_session_open_reader(session, relative_path.as_ptr(), ptr::null()) };
    if reader.is_null() {
        panic_on_error();
    }
    let mut stream = arrow::ffi_stream::FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_file_reader_read_all(reader, 2, 2, &mut stream) },
        0
    );
    let (_, batches) = collect_c_stream(stream);
    unsafe {
        lance_file_reader_close(reader);
    }
    batches
}

#[test]
fn file_session_operations_diagnostics_and_static_options() {
    let root = tempfile::tempdir().unwrap();
    let base_uri = format!("file://{}", root.path().display());
    let base_uri_c = c_string(&base_uri);
    let block_keys = [c"block_size".as_ptr()];
    let block_values = [c"4096".as_ptr()];
    let input = LanceStorageOptionsInput {
        keys: block_keys.as_ptr(),
        values: block_values.as_ptr(),
        count: 1,
    };
    let accessor = unsafe { lance_storage_options_accessor_new(&input, ptr::null()) };
    let session =
        unsafe { lance_file_session_new(base_uri_c.as_ptr(), accessor, ptr::null(), ptr::null()) };
    if session.is_null() {
        panic_on_error();
    }
    let mut info = LanceObjectStoreInfo::default();
    assert_eq!(
        unsafe { lance_file_session_get_store_info(session, &mut info) },
        0
    );
    assert_eq!(info.block_size, 4096);
    assert_eq!(info.is_local, 1);
    assert_eq!(info.is_cloud, 0);

    let summary = write_fixture_with_session(session, "data.lance");
    assert_eq!(summary.num_rows, 7);
    let batches = read_fixture_with_session(session, "data.lance");
    assert_fixture_batches(&batches);

    let mut scheduler_stats = LanceScanStats::default();
    assert_eq!(
        unsafe { lance_file_session_get_scheduler_stats(session, &mut scheduler_stats) },
        0
    );
    assert!(scheduler_stats.bytes_read > 0);
    let mut store_stats = LanceObjectStoreIoStats::default();
    assert_eq!(
        unsafe { lance_file_session_get_store_io_stats(session, &mut store_stats) },
        0
    );
    assert!(store_stats.read_bytes > 0);
    let mut registry_stats = LanceObjectStoreRegistryStats::default();
    assert_eq!(
        unsafe { lance_file_session_get_registry_stats(session, &mut registry_stats) },
        0
    );
    assert!(registry_stats.active_stores >= 1);

    let mut exists = -1;
    assert_eq!(
        unsafe { lance_file_session_contains(session, c"data.lance".as_ptr(), &mut exists,) },
        0
    );
    assert_eq!(exists, 1);

    let local = root.path().join("local-upload.bin");
    std::fs::write(&local, b"0123456789").unwrap();
    let local_c = c_string(local.to_str().unwrap());
    assert_eq!(
        unsafe {
            lance_file_session_upload_file(session, local_c.as_ptr(), c"uploads/blob.bin".as_ptr())
        },
        0
    );
    assert_eq!(
        unsafe { lance_file_session_contains(session, c"uploads/blob.bin".as_ptr(), &mut exists,) },
        0
    );
    assert_eq!(exists, 1);

    let list = unsafe { lance_file_session_list(session, ptr::null()) };
    assert!(!list.is_null());
    let list_count = unsafe { lance_path_list_count(list) };
    assert!(list_count >= 2);
    let mut listed = Vec::new();
    for index in 0..list_count {
        let path = unsafe { lance_path_list_at(list, index as usize) };
        assert!(!path.is_null());
        listed.push(unsafe { CStr::from_ptr(path) }.to_str().unwrap().to_owned());
    }
    unsafe { lance_path_list_close(list) };
    assert!(listed.iter().any(|path| path == "data.lance"));
    assert!(listed.iter().any(|path| path == "uploads/blob.bin"));

    let delimited = unsafe { lance_file_session_list_with_delimiter(session, ptr::null()) };
    assert!(!delimited.is_null());
    let mut prefixes = Vec::new();
    let prefix_count = unsafe { lance_delimited_list_common_prefix_count(delimited) };
    for index in 0..prefix_count {
        let path = unsafe { lance_delimited_list_common_prefix_at(delimited, index as usize) };
        prefixes.push(unsafe { CStr::from_ptr(path) }.to_str().unwrap().to_owned());
    }
    unsafe { lance_delimited_list_close(delimited) };
    assert!(prefixes.iter().any(|path| path == "uploads"));

    let range =
        unsafe { lance_file_session_read_range(session, c"uploads/blob.bin".as_ptr(), 2, 4) };
    assert!(!range.is_null());
    let range_bytes = unsafe {
        std::slice::from_raw_parts(lance_byte_buffer_data(range), lance_byte_buffer_len(range))
    };
    assert_eq!(range_bytes, b"2345");
    unsafe { lance_byte_buffer_close(range) };

    let download = root.path().join("downloaded.bin");
    let download_c = c_string(download.to_str().unwrap());
    assert_eq!(
        unsafe {
            lance_file_session_download_file(
                session,
                c"uploads/blob.bin".as_ptr(),
                download_c.as_ptr(),
            )
        },
        0
    );
    assert_eq!(std::fs::read(download).unwrap(), b"0123456789");

    assert_eq!(
        unsafe { lance_file_session_delete_file(session, c"uploads/blob.bin".as_ptr()) },
        0
    );
    unsafe { lance_file_session_contains(session, c"uploads/blob.bin".as_ptr(), &mut exists) };
    assert_eq!(exists, 0);
    assert_eq!(
        unsafe { lance_file_session_delete_file(session, c"uploads/blob.bin".as_ptr()) },
        -1
    );
    let (code, _) = take_error();
    assert_eq!(code, LanceErrorCode::NotFound);

    unsafe {
        lance_file_session_close(session);
        lance_storage_options_accessor_close(accessor);
    }

    let invalid_keys = [c"block_size".as_ptr()];
    let invalid_values = [c"not-a-size".as_ptr()];
    let invalid = LanceStorageOptionsInput {
        keys: invalid_keys.as_ptr(),
        values: invalid_values.as_ptr(),
        count: 1,
    };
    let accessor = unsafe { lance_storage_options_accessor_new(&invalid, ptr::null()) };
    let session =
        unsafe { lance_file_session_new(base_uri_c.as_ptr(), accessor, ptr::null(), ptr::null()) };
    assert!(session.is_null());
    let (code, _) = take_error();
    assert_eq!(code, LanceErrorCode::InvalidArgument);
    unsafe { lance_storage_options_accessor_close(accessor) };
}

fn session_cache_stats(session: *const LanceSession) -> LanceSessionCacheStats {
    let mut stats = LanceSessionCacheStats::default();
    assert_eq!(
        unsafe { lance_session_get_cache_stats(session, &mut stats) },
        0
    );
    stats
}

#[test]
fn shared_session_cache_is_explicit_and_streams_outlive_parents() {
    let root = tempfile::tempdir().unwrap();
    let base_uri = format!("file://{}", root.path().display());
    let base_uri_c = c_string(&base_uri);
    let cache_session = lance_session_new(8 * 1024 * 1024, 8 * 1024 * 1024);
    assert!(!cache_session.is_null());
    let file_session = unsafe {
        lance_file_session_new(base_uri_c.as_ptr(), ptr::null(), cache_session, ptr::null())
    };
    assert!(!file_session.is_null());
    write_fixture_with_session(file_session, "cached.lance");

    let none_options = LanceFileReaderOptions::default();
    let before_none = session_cache_stats(cache_session);
    let none_reader = unsafe {
        lance_file_session_open_reader(file_session, c"cached.lance".as_ptr(), &none_options)
    };
    let mut none_stream = arrow::ffi_stream::FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_file_reader_read_all(none_reader, 2, 2, &mut none_stream) },
        0
    );
    let _ = collect_c_stream(none_stream);
    unsafe { lance_file_reader_close(none_reader) };
    let after_none = session_cache_stats(cache_session);
    assert_eq!(
        before_none.metadata_cache_hits,
        after_none.metadata_cache_hits
    );
    assert_eq!(
        before_none.metadata_cache_entries,
        after_none.metadata_cache_entries
    );
    assert_eq!(
        before_none.metadata_cache_size_bytes,
        after_none.metadata_cache_size_bytes
    );

    let session_options = LanceFileReaderOptions {
        cache_mode: LanceFileCacheMode::Session as i32,
        ..Default::default()
    };
    let reader = unsafe {
        lance_file_session_open_reader(file_session, c"cached.lance".as_ptr(), &session_options)
    };
    assert!(!reader.is_null());
    let mut stream = arrow::ffi_stream::FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_file_reader_read_all(reader, 2, 2, &mut stream) },
        0
    );
    let batches = collect_c_stream(stream).1;
    assert_fixture_batches(&batches);
    let after_first = session_cache_stats(cache_session);
    assert!(
        after_first.metadata_cache_entries > after_none.metadata_cache_entries
            || after_first.metadata_cache_hits > after_none.metadata_cache_hits
            || after_first.metadata_cache_misses > after_none.metadata_cache_misses
    );

    let second_reader = unsafe {
        lance_file_session_open_reader(file_session, c"cached.lance".as_ptr(), &session_options)
    };
    assert!(!second_reader.is_null());
    let mut second_stream = arrow::ffi_stream::FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_file_reader_read_all(second_reader, 2, 2, &mut second_stream) },
        0
    );
    let _ = collect_c_stream(second_stream);
    let after_second = session_cache_stats(cache_session);
    assert!(after_second.metadata_cache_hits >= after_first.metadata_cache_hits);
    unsafe { lance_file_reader_close(second_reader) };

    let lifetime_reader = unsafe {
        lance_file_session_open_reader(file_session, c"cached.lance".as_ptr(), &session_options)
    };
    assert!(!lifetime_reader.is_null());
    let mut lifetime_stream = arrow::ffi_stream::FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_file_reader_read_all(lifetime_reader, 2, 2, &mut lifetime_stream) },
        0
    );
    unsafe {
        lance_file_reader_close(lifetime_reader);
        lance_file_session_close(file_session);
        lance_session_close(cache_session);
    }
    let (_, lifetime_batches) = collect_c_stream(lifetime_stream);
    assert_fixture_batches(&lifetime_batches);

    let missing_session_reader = unsafe {
        lance_file_reader_open(
            c"no-such-file.lance".as_ptr(),
            ptr::null(),
            &session_options,
            ptr::null(),
            ptr::null(),
        )
    };
    assert!(missing_session_reader.is_null());
    let (code, message) = take_error();
    assert_eq!(code, LanceErrorCode::InvalidArgument);
    assert!(message.contains("SESSION"));
}

#[test]
fn early_stream_release_and_reader_close_are_safe() {
    let root = tempfile::tempdir().unwrap();
    let uri = root.path().join("early.lance");
    let uri = uri.to_str().unwrap();
    write_fixture(uri, Some("2.2"), false, None, None, None, None, false);
    let reader = open_c_reader(uri, None);
    let mut stream = arrow::ffi_stream::FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_file_reader_read_all(reader, 2, 2, &mut stream) },
        0
    );
    unsafe {
        lance_file_reader_close(reader);
    }
    let release = stream.release.take().unwrap();
    unsafe { release(&mut stream) };
}
