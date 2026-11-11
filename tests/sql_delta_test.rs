// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Native Dataset SQL and Delta C API tests.
//!
//! Every C-API result is compared with the same query/delta computed in this
//! process through the public native `dataset::sql` / `dataset::delta` APIs,
//! and every fixture is built with real dataset writes (create, append,
//! update, delete, merge-insert). No expected result is assembled by hand in
//! place of a native oracle; the small fixed id sets below are independent
//! expectations derived from the fixture's known row history.

use std::ffi::{CStr, CString};
use std::ptr;
use std::sync::Arc;

use arrow::ffi_stream::ArrowArrayStreamReader;
use arrow::ffi_stream::FFI_ArrowArrayStream;
use arrow_array::{
    Array, Int32Array, LargeBinaryArray, RecordBatch, RecordBatchReader, StringArray,
};
use arrow_schema::{DataType, Field, Schema};
use futures::TryStreamExt;
use lance::dataset::delta::DatasetDeltaBuilder as NativeDeltaBuilder;
use lance::dataset::sql::SqlQueryBuilder as NativeSqlBuilder;
use lance::dataset::write::merge_insert::{MergeInsertBuilder, WhenMatched, WhenNotMatched};
use lance::dataset::{UpdateBuilder, WriteMode, WriteParams};
use lance::{Dataset, Result as LanceResult};
use lance_c::*;
use lance_core::datatypes::BlobHandling;

// Mirror of the C enum `LanceBlobHandling`; the FFI parameter is an int32.
const BLOB_HANDLING_BLOBS_DESCRIPTIONS: i32 = 0;
const BLOB_HANDLING_ALL_BINARY: i32 = 1;
const BLOB_HANDLING_ALL_DESCRIPTIONS: i32 = 2;

// ─── Helpers ─────────────────────────────────────────────────────────────────

fn c_str(value: &str) -> CString {
    CString::new(value).unwrap()
}

fn take_last_error_message() -> String {
    unsafe {
        let message = lance_last_error_message();
        if message.is_null() {
            return String::new();
        }
        let out = CStr::from_ptr(message).to_string_lossy().into_owned();
        lance_free_string(message);
        out
    }
}

fn open_dataset(uri: &str, version: u64) -> *mut LanceDataset {
    let c_uri = c_str(uri);
    let ds = unsafe { lance_dataset_open(c_uri.as_ptr(), ptr::null(), version) };
    assert!(
        !ds.is_null(),
        "opening {uri} at version {version} failed: {}",
        take_last_error_message()
    );
    ds
}

/// Take ownership of a caller-owned Arrow C stream and collect every batch.
fn collect_stream(mut stream: FFI_ArrowArrayStream) -> (Schema, Vec<RecordBatch>) {
    let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream) }
        .expect("arrow stream import failed");
    let schema = reader.schema().as_ref().clone();
    let batches: Vec<RecordBatch> = reader
        .map(|batch| batch.expect("stream batch failed"))
        .collect();
    (schema, batches)
}

/// Collect every batch of a native lance stream.
fn collect_native(
    stream: impl futures::Stream<Item = LanceResult<RecordBatch>>,
) -> Vec<RecordBatch> {
    lance_c::runtime::block_on(async { stream.try_collect::<Vec<_>>().await.unwrap() })
}

fn collect_native_df(
    stream: datafusion::execution::SendableRecordBatchStream,
) -> (Schema, Vec<RecordBatch>) {
    let schema = stream.schema().as_ref().clone();
    let batches = lance_c::runtime::block_on(async {
        stream
            .try_collect::<Vec<_>>()
            .await
            .unwrap_or_else(|err| panic!("native SQL stream failed: {err}"))
    });
    (schema, batches)
}

fn ids_of(batches: &[RecordBatch]) -> Vec<i32> {
    let mut ids = Vec::new();
    for batch in batches {
        let column = batch.column_by_name("id").expect("id column");
        let ids_column = column
            .as_any()
            .downcast_ref::<Int32Array>()
            .expect("id is Int32");
        for row in 0..ids_column.len() {
            ids.push(ids_column.value(row));
        }
    }
    ids.sort_unstable();
    ids
}

fn row_ids_of(batches: &[RecordBatch]) -> Vec<u64> {
    use arrow_array::UInt64Array;
    let mut row_ids = Vec::new();
    for batch in batches {
        let column = batch.column_by_name("_rowid").expect("_rowid column");
        let column = column
            .as_any()
            .downcast_ref::<UInt64Array>()
            .expect("_rowid is UInt64");
        for row in 0..column.len() {
            row_ids.push(column.value(row));
        }
    }
    row_ids.sort_unstable();
    row_ids
}

// ─── Fixtures ────────────────────────────────────────────────────────────────

fn history_schema() -> Arc<Schema> {
    Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("name", DataType::Utf8, true),
        Field::new("score", DataType::Int32, true),
    ]))
}

fn history_batch(rows: &[(i32, &str)]) -> RecordBatch {
    let schema = history_schema();
    RecordBatch::try_new(
        schema,
        vec![
            Arc::new(Int32Array::from_iter_values(rows.iter().map(|r| r.0))),
            Arc::new(StringArray::from_iter_values(rows.iter().map(|r| r.1))),
            Arc::new(Int32Array::from_iter_values(rows.iter().map(|r| r.0 * 10))),
        ],
    )
    .unwrap()
}

fn batch_iterator(
    batch: RecordBatch,
    schema: Arc<Schema>,
) -> arrow::record_batch::RecordBatchIterator<Vec<arrow::error::Result<RecordBatch>>> {
    arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema)
}

/// Build a dataset with real append/update/delete/upsert history and stable
/// row ids:
///
/// - v1 create ids 1..=4 (`a`..`d`)
/// - v2 append ids 5, 6 (`e`, `f`)
/// - v3 update id 2 -> `updated-2`
/// - v4 delete id 3
/// - v5 upsert ids 6 (matched, update) and 7 (not matched, insert)
fn create_history_dataset() -> (tempfile::TempDir, String, Vec<(i64, u32)>) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp
        .path()
        .join("sql_delta_history")
        .to_str()
        .unwrap()
        .to_string();
    let schema = history_schema();

    let timestamps = lance_c::runtime::block_on(async {
        let params = WriteParams {
            mode: WriteMode::Create,
            enable_stable_row_ids: true,
            ..Default::default()
        };
        let dataset = Dataset::write(
            batch_iterator(
                history_batch(&[(1, "a"), (2, "b"), (3, "c"), (4, "d")]),
                schema.clone(),
            ),
            &uri,
            Some(params),
        )
        .await
        .unwrap();
        let mut dataset = dataset;
        dataset
            .append(
                batch_iterator(history_batch(&[(5, "e"), (6, "f")]), schema.clone()),
                None,
            )
            .await
            .unwrap();
        UpdateBuilder::new(Arc::new(dataset.clone()))
            .update_where("id = 2")
            .unwrap()
            .set("name", "'updated-2'")
            .unwrap()
            .build()
            .unwrap()
            .execute()
            .await
            .unwrap();
        dataset.delete("id = 3").await.unwrap();

        let mut builder =
            MergeInsertBuilder::try_new(Arc::new(dataset.clone()), vec!["id".to_string()]).unwrap();
        builder
            .when_matched(WhenMatched::UpdateAll)
            .when_not_matched(WhenNotMatched::InsertAll);
        builder
            .try_build()
            .unwrap()
            .execute_batches(vec![history_batch(&[(6, "upserted-6"), (7, "g")])])
            .await
            .unwrap();

        let versions = Dataset::open(&uri).await.unwrap().versions().await.unwrap();
        versions
            .iter()
            .map(|version| {
                (
                    version.timestamp.timestamp(),
                    version.timestamp.timestamp_subsec_nanos(),
                )
            })
            .collect::<Vec<_>>()
    });
    assert_eq!(timestamps.len(), 5, "history has one version per commit");
    (tmp, uri, timestamps)
}

/// A blob-v2 dataset exercising the SQL blob-handling modes.
fn create_blob_dataset() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("sql_blob_ds").to_str().unwrap().to_string();
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        lance::blob_field_with_options(
            "blob",
            true,
            lance::BlobFieldOptions {
                inline_size_threshold: Some(8),
                dedicated_size_threshold: std::num::NonZeroUsize::new(64),
            },
        ),
    ]));
    let mut blobs = lance::BlobArrayBuilder::new(3);
    blobs.push_bytes(b"inline-blob").unwrap();
    blobs.push_null().unwrap();
    blobs
        .push_bytes(b"another-blob-value-longer-than-inline")
        .unwrap();
    let blob_array = blobs.finish().unwrap();
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![Arc::new(Int32Array::from_iter_values(1..=3)), blob_array],
    )
    .unwrap();
    lance_c::runtime::block_on(async {
        Dataset::write(
            batch_iterator(batch, schema),
            &uri,
            Some(WriteParams {
                data_storage_version: Some(lance_file::version::LanceFileVersion::V2_2),
                ..Default::default()
            }),
        )
        .await
        .unwrap();
    });
    (tmp, uri)
}

/// A three-fragment dataset for SQL projection/filter/order/aggregate.
fn create_sql_dataset() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp
        .path()
        .join("sql_multi_fragment")
        .to_str()
        .unwrap()
        .to_string();
    let schema = history_schema();
    lance_c::runtime::block_on(async {
        for (index, start) in [1_i32, 11, 21].into_iter().enumerate() {
            let rows: Vec<(i32, String)> = (0..10)
                .map(|offset| {
                    let id = start + offset;
                    (id, format!("name-{id}"))
                })
                .collect();
            let refs: Vec<(i32, &str)> =
                rows.iter().map(|(id, name)| (*id, name.as_str())).collect();
            let params = WriteParams {
                mode: if index == 0 {
                    WriteMode::Create
                } else {
                    WriteMode::Append
                },
                ..Default::default()
            };
            Dataset::write(
                batch_iterator(history_batch(&refs), schema.clone()),
                &uri,
                Some(params),
            )
            .await
            .unwrap();
        }
    });
    (tmp, uri)
}

// ─── SQL oracles and tests ───────────────────────────────────────────────────

/// Pass-through for oracle calls that keep the native builder unconfigured.
fn unchanged<T>(value: T) -> T {
    value
}

fn native_sql(
    uri: &str,
    sql: &str,
    configure: impl FnOnce(NativeSqlBuilder) -> NativeSqlBuilder,
) -> (Schema, Vec<RecordBatch>) {
    let dataset = lance_c::runtime::block_on(Dataset::open(uri)).unwrap();
    let builder = NativeSqlBuilder::new(dataset, sql);
    let query = lance_c::runtime::block_on(configure(builder).build())
        .unwrap_or_else(|err| panic!("native SQL build failed for {sql:?}: {err}"));
    collect_native_df(lance_c::runtime::block_on(query.into_stream()).unwrap())
}

fn c_sql(
    ds: *const LanceDataset,
    sql: &str,
    configure: impl FnOnce(*mut LanceSqlQueryBuilder),
) -> (Schema, Vec<RecordBatch>) {
    let c_sql = c_str(sql);
    let builder = unsafe { lance_dataset_sql_builder_new(ds, c_sql.as_ptr()) };
    assert!(
        !builder.is_null(),
        "sql builder: {}",
        take_last_error_message()
    );
    configure(builder);
    let mut query: *mut LanceSqlQuery = ptr::null_mut();
    assert_eq!(
        unsafe { lance_sql_query_builder_build(builder, &mut query) },
        0,
        "build {sql:?}: {}",
        take_last_error_message()
    );
    assert!(!query.is_null());
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_sql_query_into_stream(query, &mut stream) },
        0,
        "into_stream {sql:?}: {}",
        take_last_error_message()
    );
    collect_stream(stream)
}

fn assert_matches_oracle(
    c_result: &(Schema, Vec<RecordBatch>),
    native_result: &(Schema, Vec<RecordBatch>),
    context: &str,
) {
    let (c_schema, c_batches) = c_result;
    let (native_schema, native_batches) = native_result;
    assert_eq!(c_schema, native_schema, "{context}: schema mismatch");
    assert_eq!(
        c_batches,
        native_batches,
        "{context}: batch mismatch ({} vs {} batches)",
        c_batches.len(),
        native_batches.len()
    );
}

#[test]
fn test_sql_multi_fragment_projection_filter_order_aggregate() {
    let (_tmp, uri) = create_sql_dataset();
    let ds = open_dataset(&uri, 0);

    let cases: &[&str] = &[
        "SELECT id, name FROM dataset WHERE id % 3 = 0 ORDER BY id DESC",
        "SELECT name, id FROM dataset WHERE score > 50 AND id < 25 ORDER BY name",
        "SELECT COUNT(*) AS rows FROM dataset",
        "SELECT SUM(score) AS total, MIN(score) AS lo, MAX(score) AS hi FROM dataset",
        "SELECT id / 10 AS bucket, COUNT(*) AS n, SUM(score) AS total FROM dataset GROUP BY id / 10 ORDER BY bucket",
        "SELECT DISTINCT name FROM dataset WHERE id <= 3 ORDER BY name",
    ];
    for sql in cases {
        let native = native_sql(&uri, sql, unchanged);
        let c_result = c_sql(ds, sql, |_| {});
        assert_matches_oracle(&c_result, &native, sql);
    }

    // A version-pinned snapshot only sees its own rows.
    let historical = open_dataset(&uri, 1);
    let sql = "SELECT id FROM dataset ORDER BY id";
    let native = native_sql(&uri, sql, unchanged);
    let c_all = c_sql(ds, sql, |_| {});
    let c_historical = c_sql(historical, sql, |_| {});
    assert_eq!(ids_of(&c_all.1), ids_of(&native.1));
    assert_eq!(c_historical.1.len(), 1);
    assert_eq!(ids_of(&c_historical.1), (1..=10).collect::<Vec<_>>());
    unsafe {
        lance_dataset_close(historical);
        lance_dataset_close(ds);
    }
}

#[test]
fn test_sql_renamed_table_and_system_columns() {
    let (_tmp, uri) = create_sql_dataset();
    let ds = open_dataset(&uri, 0);

    // Renamed table: the native default name must not resolve once renamed.
    let renamed = "SELECT id FROM orders WHERE id <= 3 ORDER BY id";
    let native = native_sql(&uri, renamed, |builder| builder.table_name("orders"));
    let c_result = c_sql(ds, renamed, |builder| {
        assert_eq!(
            unsafe { lance_sql_query_builder_set_table_name(builder, c_str("orders").as_ptr()) },
            0
        );
    });
    assert_matches_oracle(&c_result, &native, renamed);

    let c_bad = c_str("SELECT id FROM dataset");
    let builder = unsafe { lance_dataset_sql_builder_new(ds, c_bad.as_ptr()) };
    assert!(!builder.is_null());
    assert_eq!(
        unsafe { lance_sql_query_builder_set_table_name(builder, c_str("orders").as_ptr()) },
        0
    );
    let mut query: *mut LanceSqlQuery = ptr::null_mut();
    assert_eq!(
        unsafe { lance_sql_query_builder_build(builder, &mut query) },
        -1,
        "the default table name must not resolve after renaming"
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let _ = take_last_error_message();
    assert!(query.is_null(), "failed build must not publish a query");

    // Safe native system-column injection: row ids are appended for a plain
    // projection, and native refuses to append them for aggregates.
    let plain = "SELECT id FROM dataset WHERE id <= 4 ORDER BY id";
    let native_plain = native_sql(&uri, plain, |builder| builder.with_row_id(true));
    let c_plain = c_sql(ds, plain, |builder| {
        assert_eq!(
            unsafe { lance_sql_query_builder_set_with_row_id(builder, true) },
            0
        );
    });
    assert_matches_oracle(&c_plain, &native_plain, plain);
    let plain_row_ids = row_ids_of(&c_plain.1);
    assert_eq!(plain_row_ids.len(), 4);
    let mut distinct = plain_row_ids.clone();
    distinct.dedup();
    assert_eq!(distinct.len(), 4, "one row id per projected row");

    let addr = "SELECT id FROM dataset WHERE id <= 2 ORDER BY id";
    let native_addr = native_sql(&uri, addr, |builder| builder.with_row_addr(true));
    let c_addr = c_sql(ds, addr, |builder| {
        assert_eq!(
            unsafe { lance_sql_query_builder_set_with_row_addr(builder, true) },
            0
        );
    });
    assert_matches_oracle(&c_addr, &native_addr, addr);
    assert!(c_addr.0.field_with_name("_rowaddr").is_ok());

    // Aggregates collapse rows, so the planner must not inject clients.
    let aggregate = "SELECT COUNT(*) AS n FROM dataset";
    let native_aggregate = native_sql(&uri, aggregate, |builder| {
        builder.with_row_id(true).with_row_addr(true)
    });
    let c_aggregate = c_sql(ds, aggregate, |builder| {
        assert_eq!(
            unsafe { lance_sql_query_builder_set_with_row_id(builder, true) },
            0
        );
        assert_eq!(
            unsafe { lance_sql_query_builder_set_with_row_addr(builder, true) },
            0
        );
    });
    assert_matches_oracle(&c_aggregate, &native_aggregate, aggregate);
    assert!(c_aggregate.0.field_with_name("_rowid").is_err());
    assert_eq!(
        c_aggregate.1[0]
            .column_by_name("n")
            .unwrap()
            .as_any()
            .downcast_ref::<arrow_array::Int64Array>()
            .unwrap()
            .value(0),
        30
    );

    // Stable row ids are assigned in insertion order, so the fixed ids are
    // deterministic for the history fixture: id 3 was deleted between v2 and
    // v5 and its row id 2 is not live.
    let (_history_tmp, history_uri, _timestamps) = create_history_dataset();
    let history = open_dataset(&history_uri, 0);
    let history_sql = "SELECT id FROM dataset WHERE id <= 4 ORDER BY id";
    let native_history = native_sql(&history_uri, history_sql, |builder| {
        builder.with_row_id(true)
    });
    let c_history = c_sql(history, history_sql, |builder| {
        assert_eq!(
            unsafe { lance_sql_query_builder_set_with_row_id(builder, true) },
            0
        );
    });
    assert_matches_oracle(&c_history, &native_history, history_sql);
    assert_eq!(ids_of(&c_history.1), vec![1, 2, 4]);
    assert_eq!(row_ids_of(&c_history.1), vec![0, 1, 3]);
    unsafe { lance_dataset_close(history) };

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_sql_batching_blob_modes_and_limits() {
    let (_tmp, uri) = create_sql_dataset();
    let ds = open_dataset(&uri, 0);

    // batch_size is a native usize target: every batch stays within it and no
    // row is lost or duplicated.
    let sql = "SELECT id FROM dataset ORDER BY id";
    let native_small = native_sql(&uri, sql, |builder| builder.batch_size(3));
    let c_small = c_sql(ds, sql, |builder| {
        assert_eq!(
            unsafe { lance_sql_query_builder_set_batch_size(builder, 3) },
            0
        );
    });
    assert_matches_oracle(&c_small, &native_small, "batch_size = 3");
    assert!(
        c_small.1.len() > 1,
        "batch_size = 3 must split 30 rows into several batches, got {}",
        c_small.1.len()
    );
    assert!(c_small.1.iter().all(|batch| batch.num_rows() <= 3));
    assert_eq!(ids_of(&c_small.1), (1..=30).collect::<Vec<i32>>());

    let native_bytes = native_sql(&uri, sql, |builder| builder.batch_size_bytes(512));
    let c_bytes = c_sql(ds, sql, |builder| {
        assert_eq!(
            unsafe { lance_sql_query_builder_set_batch_size_bytes(builder, 512) },
            0
        );
    });
    assert_matches_oracle(&c_bytes, &native_bytes, "batch_size_bytes = 512");

    // Native validation owns the accepted range; the C layer only forwards.
    let too_large = u32::MAX as u64 + 1;
    let c_sql_text = c_str(sql);
    let builder = unsafe { lance_dataset_sql_builder_new(ds, c_sql_text.as_ptr()) };
    assert!(!builder.is_null());
    assert_eq!(
        unsafe { lance_sql_query_builder_set_batch_size(builder, too_large) },
        0,
        "forwarding a large batch size is not a representation error"
    );
    let mut query: *mut LanceSqlQuery = ptr::null_mut();
    assert_eq!(
        unsafe { lance_sql_query_builder_build(builder, &mut query) },
        -1,
        "native validation must reject batch sizes above u32::MAX"
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let message = take_last_error_message();
    assert!(
        message.contains("batch") || message.contains("Batch"),
        "native batch-size error should be surfaced, got: {message}"
    );

    // Blob modes: the native planner rewrites the blob column per mode.
    let (_blob_tmp, blob_uri) = create_blob_dataset();
    let blob_ds = open_dataset(&blob_uri, 0);
    let blob_sql = "SELECT * FROM dataset ORDER BY id";

    let native_binary = native_sql(&blob_uri, blob_sql, |builder| {
        builder.blob_handling(BlobHandling::AllBinary)
    });
    let c_binary = c_sql(blob_ds, blob_sql, |builder| {
        assert_eq!(
            unsafe { lance_sql_query_builder_set_blob_handling(builder, BLOB_HANDLING_ALL_BINARY) },
            0
        );
    });
    assert_matches_oracle(&c_binary, &native_binary, "blob ALL_BINARY");
    assert_eq!(
        *c_binary.0.field_with_name("blob").unwrap().data_type(),
        DataType::LargeBinary
    );
    let blobs = c_binary.1[0]
        .column_by_name("blob")
        .unwrap()
        .as_any()
        .downcast_ref::<LargeBinaryArray>()
        .unwrap();
    assert_eq!(blobs.value(0), b"inline-blob");
    assert!(blobs.is_null(1));
    assert_eq!(blobs.value(2), b"another-blob-value-longer-than-inline");

    let native_descriptions = native_sql(&blob_uri, blob_sql, |builder| {
        builder.blob_handling(BlobHandling::BlobsDescriptions)
    });
    let c_descriptions = c_sql(blob_ds, blob_sql, |builder| {
        assert_eq!(
            unsafe {
                lance_sql_query_builder_set_blob_handling(builder, BLOB_HANDLING_ALL_DESCRIPTIONS)
            },
            0
        );
    });
    assert_matches_oracle(
        &c_descriptions,
        &native_descriptions,
        "blob ALL_DESCRIPTIONS",
    );
    assert!(
        matches!(
            c_descriptions
                .0
                .field_with_name("blob")
                .unwrap()
                .data_type(),
            DataType::Struct(_)
        ),
        "ALL_DESCRIPTIONS materializes the description struct"
    );

    // A foreign enum discriminant is rejected before reaching native, and the
    // prior configuration survives the rejected call.
    let builder = unsafe { lance_dataset_sql_builder_new(blob_ds, c_sql_text.as_ptr()) };
    assert!(!builder.is_null());
    assert_eq!(
        unsafe { lance_sql_query_builder_set_blob_handling(builder, 7) },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let _ = take_last_error_message();
    assert_eq!(
        unsafe { lance_sql_query_builder_set_table_name(builder, c_str("dataset").as_ptr()) },
        0,
        "a rejected setter must leave the builder usable"
    );
    let mut query: *mut LanceSqlQuery = ptr::null_mut();
    assert_eq!(
        unsafe { lance_sql_query_builder_build(builder, &mut query) },
        0,
        "builder still plans after the rejected blob mode"
    );
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_sql_query_into_stream(query, &mut stream) },
        0
    );
    let (_, batches) = collect_stream(stream);
    assert_eq!(ids_of(&batches), vec![1, 2, 3]);

    // NULL strings and pointers are representation errors that preserve state.
    let builder = unsafe { lance_dataset_sql_builder_new(blob_ds, c_sql_text.as_ptr()) };
    assert!(!builder.is_null());
    assert_eq!(
        unsafe { lance_sql_query_builder_set_table_name(builder, ptr::null()) },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let _ = take_last_error_message();
    let mut query: *mut LanceSqlQuery = ptr::null_mut();
    assert_eq!(
        unsafe { lance_sql_query_builder_build(builder, &mut query) },
        0,
        "the default table name survives the rejected setter"
    );
    assert!(!query.is_null());
    assert_eq!(
        unsafe { lance_sql_query_into_stream(query, ptr::null_mut()) },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let _ = take_last_error_message();

    assert!(unsafe { lance_dataset_sql_builder_new(ptr::null(), c_sql_text.as_ptr()) }.is_null());
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let _ = take_last_error_message();
    assert!(unsafe { lance_dataset_sql_builder_new(blob_ds, ptr::null()) }.is_null());
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let _ = take_last_error_message();
    unsafe { lance_sql_query_builder_close(ptr::null_mut()) };

    unsafe {
        lance_dataset_close(blob_ds);
        lance_dataset_close(ds);
    }
}

#[test]
fn test_sql_planning_and_runtime_errors() {
    let (_tmp, uri) = create_sql_dataset();
    let ds = open_dataset(&uri, 0);

    // Planning errors surface on build, are classified as invalid input, and
    // leave no query behind.
    for sql in [
        "SELECT * FROM missing_table",
        "SELECT missing_column FROM dataset",
        "SELEC broken syntax",
    ] {
        let c_sql_text = c_str(sql);
        let builder = unsafe { lance_dataset_sql_builder_new(ds, c_sql_text.as_ptr()) };
        assert!(!builder.is_null(), "builder for {sql:?}");
        let mut query: *mut LanceSqlQuery = ptr::null_mut();
        assert_eq!(
            unsafe { lance_sql_query_builder_build(builder, &mut query) },
            -1,
            "{sql:?} must fail at build"
        );
        assert_eq!(
            lance_last_error_code(),
            LanceErrorCode::InvalidArgument,
            "{sql:?}: {}",
            take_last_error_message()
        );
        assert!(query.is_null());
    }

    // Runtime errors surface while the stream is read, not from into_stream.
    let sql = "SELECT id / (id - id) AS boom FROM dataset";
    let c_sql_text = c_str(sql);
    let builder = unsafe { lance_dataset_sql_builder_new(ds, c_sql_text.as_ptr()) };
    assert!(!builder.is_null());
    let mut query: *mut LanceSqlQuery = ptr::null_mut();
    assert_eq!(
        unsafe { lance_sql_query_builder_build(builder, &mut query) },
        0
    );
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_sql_query_into_stream(query, &mut stream) },
        0
    );
    let mut reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream) }.unwrap();
    let mut saw_error = false;
    for batch in reader.by_ref() {
        if batch.is_err() {
            saw_error = true;
            break;
        }
    }
    assert!(saw_error, "division by zero must fail while streaming");

    // The query allocation is consumed even when the stream slot is NULL.
    let builder = unsafe { lance_dataset_sql_builder_new(ds, c_sql_text.as_ptr()) };
    assert!(!builder.is_null());
    let mut query: *mut LanceSqlQuery = ptr::null_mut();
    assert_eq!(
        unsafe { lance_sql_query_builder_build(builder, &mut query) },
        0
    );
    assert_eq!(
        unsafe { lance_sql_query_into_stream(query, ptr::null_mut()) },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let _ = take_last_error_message();

    // Close is NULL-safe.
    unsafe { lance_sql_query_close(ptr::null_mut()) };

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_sql_stream_outlives_dataset_and_builder() {
    let (_tmp, uri) = create_sql_dataset();
    let ds = open_dataset(&uri, 0);
    let sql = "SELECT id, name FROM dataset WHERE id <= 5 ORDER BY id";
    let c_sql_text = c_str(sql);
    let builder = unsafe { lance_dataset_sql_builder_new(ds, c_sql_text.as_ptr()) };
    assert!(!builder.is_null());
    let native = native_sql(&uri, sql, |builder| builder.batch_size(2));
    assert_eq!(
        unsafe { lance_sql_query_builder_set_batch_size(builder, 2) },
        0
    );
    let mut query: *mut LanceSqlQuery = ptr::null_mut();
    assert_eq!(
        unsafe { lance_sql_query_builder_build(builder, &mut query) },
        0
    );
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_sql_query_into_stream(query, &mut stream) },
        0
    );
    // Both the C dataset handle and the builder/query allocations are gone;
    // the stream still owns the native snapshot and runtime it needs.
    unsafe { lance_dataset_close(ds) };
    let (schema, batches) = collect_stream(stream);
    assert_eq!(schema, native.0);
    assert_eq!(batches, native.1);
    assert_eq!(ids_of(&batches), vec![1, 2, 3, 4, 5]);
}

// ─── Delta oracles and tests ─────────────────────────────────────────────────

fn native_delta(
    uri: &str,
    configure: impl FnOnce(NativeDeltaBuilder) -> NativeDeltaBuilder,
    kind: DeltaStream,
) -> Vec<RecordBatch> {
    let dataset = lance_c::runtime::block_on(Dataset::open(uri)).unwrap();
    let delta = configure(NativeDeltaBuilder::new(dataset)).build().unwrap();
    let stream = lance_c::runtime::block_on(async {
        match kind {
            DeltaStream::Inserted => delta.get_inserted_rows().await,
            DeltaStream::Updated => delta.get_updated_rows().await,
            DeltaStream::Upserted => delta.get_upserted_rows().await,
            DeltaStream::Deleted => delta.get_deleted_row_ids().await,
        }
    })
    .unwrap_or_else(|err| panic!("native delta {kind:?} stream failed: {err}"));
    collect_native(stream)
}

fn c_delta_stream(delta: *mut LanceDatasetDelta, kind: DeltaStream) -> Vec<RecordBatch> {
    let mut stream = FFI_ArrowArrayStream::empty();
    let rc = unsafe {
        match kind {
            DeltaStream::Inserted => lance_dataset_delta_get_inserted_rows(delta, &mut stream),
            DeltaStream::Updated => lance_dataset_delta_get_updated_rows(delta, &mut stream),
            DeltaStream::Upserted => lance_dataset_delta_get_upserted_rows(delta, &mut stream),
            DeltaStream::Deleted => lance_dataset_delta_get_deleted_row_ids(delta, &mut stream),
        }
    };
    assert_eq!(rc, 0, "delta {kind:?}: {}", take_last_error_message());
    let (_, batches) = collect_stream(stream);
    batches
}

#[derive(Clone, Copy, Debug)]
enum DeltaStream {
    Inserted,
    Updated,
    Upserted,
    Deleted,
}

fn build_c_delta(
    ds: *const LanceDataset,
    configure: impl FnOnce(*mut LanceDatasetDeltaBuilder),
) -> *mut LanceDatasetDelta {
    let builder = unsafe { lance_dataset_delta_builder_new(ds) };
    assert!(
        !builder.is_null(),
        "delta builder: {}",
        take_last_error_message()
    );
    configure(builder);
    let mut delta: *mut LanceDatasetDelta = ptr::null_mut();
    assert_eq!(
        unsafe { lance_dataset_delta_builder_build(builder, &mut delta) },
        0,
        "delta build: {}",
        take_last_error_message()
    );
    assert!(!delta.is_null());
    delta
}

#[test]
fn test_delta_full_and_version_ranges_match_native() {
    let (_tmp, uri, _timestamps) = create_history_dataset();
    let ds = open_dataset(&uri, 0);

    // Full range from version 0: every inserted row, no updates (nothing can
    // predate version 0), and no deleted ids because version 0 is the empty
    // snapshot by native rule.
    let delta = build_c_delta(ds, |builder| {
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_begin_version(builder, 0) },
            0
        );
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_end_version(builder, 5) },
            0
        );
    });
    let inserted = c_delta_stream(delta, DeltaStream::Inserted);
    let native_inserted = native_delta(
        &uri,
        |builder| builder.with_begin_version(0).with_end_version(5),
        DeltaStream::Inserted,
    );
    assert_eq!(inserted, native_inserted);
    assert_eq!(ids_of(&inserted), vec![1, 2, 3, 4, 5, 6, 7]);

    let updated = c_delta_stream(delta, DeltaStream::Updated);
    assert!(updated.is_empty(), "no row was updated after version 0");
    let upserted = c_delta_stream(delta, DeltaStream::Upserted);
    assert_eq!(ids_of(&upserted), vec![1, 2, 3, 4, 5, 6, 7]);
    let deleted = c_delta_stream(delta, DeltaStream::Deleted);
    assert!(deleted.is_empty(), "version 0 deleted-row ids are empty");
    unsafe { lance_dataset_delta_close(delta) };

    // compared_against_version normalizes to (2, 5].
    let delta = build_c_delta(ds, |builder| {
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_compared_against_version(builder, 2) },
            0
        );
    });
    let inserted = c_delta_stream(delta, DeltaStream::Inserted);
    let native_inserted = native_delta(
        &uri,
        |builder| builder.compared_against_version(2),
        DeltaStream::Inserted,
    );
    assert_eq!(inserted, native_inserted);
    // Row 6 was appended at version 2, so its created-at version is 2 and
    // the exclusive begin keeps it out; row 7 is the only new row.
    assert_eq!(ids_of(&inserted), vec![7]);

    let updated = c_delta_stream(delta, DeltaStream::Updated);
    let native_updated = native_delta(
        &uri,
        |builder| builder.compared_against_version(2),
        DeltaStream::Updated,
    );
    assert_eq!(updated, native_updated);
    assert_eq!(ids_of(&updated), vec![2, 6]);

    let upserted = c_delta_stream(delta, DeltaStream::Upserted);
    assert_eq!(ids_of(&upserted), vec![2, 6, 7]);

    let deleted = c_delta_stream(delta, DeltaStream::Deleted);
    let native_deleted = native_delta(
        &uri,
        |builder| builder.compared_against_version(2),
        DeltaStream::Deleted,
    );
    assert_eq!(deleted, native_deleted);
    assert_eq!(
        row_ids_of(&deleted),
        vec![2],
        "id 3 was the third inserted row and is the only row that disappeared"
    );
    assert_eq!(deleted[0].schema().fields().len(), 1);
    unsafe { lance_dataset_delta_close(delta) };

    // Reversed explicit ranges: native rows are empty, native deleted ids
    // reject the range at the stream call (build itself succeeds).
    let reversed = build_c_delta(ds, |builder| {
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_begin_version(builder, 5) },
            0
        );
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_end_version(builder, 2) },
            0
        );
    });
    assert!(c_delta_stream(reversed, DeltaStream::Inserted).is_empty());
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_dataset_delta_get_deleted_row_ids(reversed, &mut stream) },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let message = take_last_error_message();
    assert!(
        message.contains("newer than end version"),
        "native reversed-range error should surface, got: {message}"
    );
    // The delta is reusable after the failed stream call.
    assert!(c_delta_stream(reversed, DeltaStream::Updated).is_empty());
    unsafe { lance_dataset_delta_close(reversed) };

    unsafe { lance_dataset_close(ds) };
}

#[test]
fn test_delta_date_mode_matches_resolved_version_range() {
    let (_tmp, uri, timestamps) = create_history_dataset();
    let ds = open_dataset(&uri, 0);
    let versions = lance_c::runtime::block_on(async {
        Dataset::open(&uri).await.unwrap().versions().await.unwrap()
    });

    // Begin is exclusive and end inclusive; resolve the same window natively
    // in-process so the expectation does not depend on clock granularity.
    let begin = timestamps[1]; // version 2
    let end = timestamps[4]; // version 5
    let mut resolved_begin = 0_u64;
    let mut resolved_end = 0_u64;
    for version in &versions {
        let stamp = (
            version.timestamp.timestamp(),
            version.timestamp.timestamp_subsec_nanos(),
        );
        if stamp < begin && version.version > resolved_begin {
            resolved_begin = version.version;
        }
        if stamp <= end && version.version > resolved_end {
            resolved_end = version.version;
        }
    }

    let delta = build_c_delta(ds, |builder| {
        let begin_ts = LanceDeltaTimestamp {
            unix_seconds: begin.0,
            nanos: begin.1,
        };
        let end_ts = LanceDeltaTimestamp {
            unix_seconds: end.0,
            nanos: end.1,
        };
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_begin_timestamp(builder, &begin_ts) },
            0
        );
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_end_timestamp(builder, &end_ts) },
            0
        );
    });

    let native = native_delta(
        &uri,
        |builder| {
            builder
                .with_begin_date(chrono::DateTime::from_timestamp(begin.0, begin.1).unwrap())
                .with_end_date(chrono::DateTime::from_timestamp(end.0, end.1).unwrap())
        },
        DeltaStream::Inserted,
    );
    assert_eq!(c_delta_stream(delta, DeltaStream::Inserted), native);
    unsafe { lance_dataset_delta_close(delta) };

    // Date mode and the resolved version range describe the same change set.
    let by_version = build_c_delta(ds, |builder| {
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_begin_version(builder, resolved_begin) },
            0
        );
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_end_version(builder, resolved_end) },
            0
        );
    });
    let delta = build_c_delta(ds, |builder| {
        let begin_ts = LanceDeltaTimestamp {
            unix_seconds: begin.0,
            nanos: begin.1,
        };
        let end_ts = LanceDeltaTimestamp {
            unix_seconds: end.0,
            nanos: end.1,
        };
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_begin_timestamp(builder, &begin_ts) },
            0
        );
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_end_timestamp(builder, &end_ts) },
            0
        );
    });
    for kind in [
        DeltaStream::Inserted,
        DeltaStream::Updated,
        DeltaStream::Upserted,
        DeltaStream::Deleted,
    ] {
        assert_eq!(
            c_delta_stream(delta, kind),
            c_delta_stream(by_version, kind),
            "date mode should equal the resolved version range for {kind:?}"
        );
    }
    unsafe {
        lance_dataset_delta_close(delta);
        lance_dataset_delta_close(by_version);
        lance_dataset_close(ds);
    }
}

#[test]
fn test_delta_transactions_ownership_and_reuse() {
    let (_tmp, uri, _timestamps) = create_history_dataset();
    let ds = open_dataset(&uri, 0);

    let delta = build_c_delta(ds, |builder| {
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_begin_version(builder, 0) },
            0
        );
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_end_version(builder, 5) },
            0
        );
    });

    let mut transactions: *mut *mut LanceTransaction = ptr::null_mut();
    let mut count = 0_usize;
    assert_eq!(
        unsafe { lance_dataset_delta_list_transactions(delta, &mut transactions, &mut count) },
        0,
        "list_transactions: {}",
        take_last_error_message()
    );
    assert_eq!(count, 5);
    assert!(!transactions.is_null());

    // Native oracle: version v committed the transaction whose read version is
    // v, in commit order.
    let native = lance_c::runtime::block_on(async {
        Dataset::open(&uri).await.unwrap().versions().await.unwrap()
    });
    assert_eq!(native.len(), 5);
    let mut read_versions = Vec::new();
    for index in 0..count {
        let transaction = unsafe { *transactions.add(index) };
        assert!(!transaction.is_null());
        let mut read_version = 0_u64;
        assert_eq!(
            unsafe { lance_transaction_read_version(transaction, &mut read_version) },
            0
        );
        read_versions.push(read_version);
        let raw_uuid = unsafe { lance_transaction_uuid(transaction) };
        assert!(!raw_uuid.is_null(), "committed transactions carry a uuid");
        let uuid = unsafe { CStr::from_ptr(raw_uuid) }
            .to_string_lossy()
            .into_owned();
        unsafe { lance_free_string(raw_uuid) };
        assert!(!uuid.is_empty(), "committed transactions carry a uuid");
    }
    assert_eq!(read_versions, vec![1, 2, 3, 4, 5]);

    // Releasing the array closes the remaining elements and the allocation;
    // the delta stays usable afterwards (nothing was consumed here).
    unsafe { lance_transaction_array_free(transactions, count) };
    unsafe { lance_transaction_array_free(ptr::null_mut(), 0) };

    // Empty range: no transactions, no array.
    let empty = build_c_delta(ds, |builder| {
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_begin_version(builder, 3) },
            0
        );
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_end_version(builder, 3) },
            0
        );
    });
    let mut transactions: *mut *mut LanceTransaction = ptr::null_mut();
    let mut count = 42_usize;
    assert_eq!(
        unsafe { lance_dataset_delta_list_transactions(empty, &mut transactions, &mut count) },
        0
    );
    assert_eq!(count, 0);
    assert!(transactions.is_null());
    // Every stream of an empty range is empty, never an error.
    for kind in [
        DeltaStream::Inserted,
        DeltaStream::Updated,
        DeltaStream::Upserted,
        DeltaStream::Deleted,
    ] {
        assert!(c_delta_stream(empty, kind).is_empty(), "{kind:?}");
    }
    unsafe {
        lance_dataset_delta_close(empty);
        lance_dataset_delta_close(delta);
        lance_dataset_close(ds);
    }
}

#[test]
fn test_delta_streams_outlive_parents_and_build_errors() {
    let (_tmp, uri, _timestamps) = create_history_dataset();
    let ds = open_dataset(&uri, 0);
    let delta = build_c_delta(ds, |builder| {
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_compared_against_version(builder, 1) },
            0
        );
    });

    // Open all four streams, then close the delta and dataset handles. Each
    // stream owns its native state.
    let mut streams = Vec::new();
    for kind in [
        DeltaStream::Inserted,
        DeltaStream::Updated,
        DeltaStream::Upserted,
        DeltaStream::Deleted,
    ] {
        let mut stream = FFI_ArrowArrayStream::empty();
        let rc = unsafe {
            match kind {
                DeltaStream::Inserted => lance_dataset_delta_get_inserted_rows(delta, &mut stream),
                DeltaStream::Updated => lance_dataset_delta_get_updated_rows(delta, &mut stream),
                DeltaStream::Upserted => lance_dataset_delta_get_upserted_rows(delta, &mut stream),
                DeltaStream::Deleted => lance_dataset_delta_get_deleted_row_ids(delta, &mut stream),
            }
        };
        assert_eq!(rc, 0, "{kind:?}: {}", take_last_error_message());
        streams.push(stream);
    }
    unsafe {
        lance_dataset_delta_close(delta);
        lance_dataset_close(ds);
    }
    let deleted = collect_stream(streams.pop().unwrap()).1;
    assert_eq!(
        row_ids_of(&deleted),
        vec![2],
        "the deleted row id is readable after both parents closed"
    );
    // Compared against version 1, rows 5..7 were created afterwards and row
    // 2 was rewritten at version 3.
    let upserted = collect_stream(streams.pop().unwrap()).1;
    assert_eq!(ids_of(&upserted), vec![2, 5, 6, 7]);
    let updated = collect_stream(streams.pop().unwrap()).1;
    assert_eq!(ids_of(&updated), vec![2]);
    let (schema, batches) = collect_stream(streams.pop().unwrap());
    assert_eq!(schema.field_with_name("id").unwrap().name(), "id");
    assert_eq!(ids_of(&batches), vec![5, 6, 7]);

    // Builder validation is synchronous and classifies each native conflict
    // as invalid input; the dataset handle stays open.
    let ds = open_dataset(&uri, 0);
    let cases: Vec<(&str, Box<dyn Fn(*mut LanceDatasetDeltaBuilder)>)> = vec![
        (
            "compared_against_version with a begin version",
            Box::new(|builder| {
                assert_eq!(
                    unsafe { lance_dataset_delta_builder_set_compared_against_version(builder, 2) },
                    0
                );
                assert_eq!(
                    unsafe { lance_dataset_delta_builder_set_begin_version(builder, 1) },
                    0
                );
            }),
        ),
        (
            "only a begin version",
            Box::new(|builder| {
                assert_eq!(
                    unsafe { lance_dataset_delta_builder_set_begin_version(builder, 1) },
                    0
                );
            }),
        ),
        (
            "only an end date",
            Box::new(|builder| {
                let timestamp = LanceDeltaTimestamp {
                    unix_seconds: 0,
                    nanos: 0,
                };
                assert_eq!(
                    unsafe { lance_dataset_delta_builder_set_end_timestamp(builder, &timestamp) },
                    0
                );
            }),
        ),
        ("no range at all", Box::new(|_builder| {})),
    ];
    for (label, configure) in cases {
        let builder = unsafe { lance_dataset_delta_builder_new(ds) };
        assert!(!builder.is_null());
        configure(builder);
        let mut delta: *mut LanceDatasetDelta = ptr::null_mut();
        assert_eq!(
            unsafe { lance_dataset_delta_builder_build(builder, &mut delta) },
            -1,
            "{label} must fail at build"
        );
        assert_eq!(
            lance_last_error_code(),
            LanceErrorCode::InvalidArgument,
            "{label}"
        );
        let _ = take_last_error_message();
        assert!(delta.is_null());
    }

    // Foreign representation guards preserve the builder's prior state.
    let builder = unsafe { lance_dataset_delta_builder_new(ds) };
    assert!(!builder.is_null());
    assert_eq!(
        unsafe { lance_dataset_delta_builder_set_compared_against_version(builder, 1) },
        0
    );
    assert_eq!(
        unsafe { lance_dataset_delta_builder_set_begin_timestamp(builder, ptr::null()) },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let _ = take_last_error_message();
    let mut delta: *mut LanceDatasetDelta = ptr::null_mut();
    assert_eq!(
        unsafe { lance_dataset_delta_builder_build(builder, &mut delta) },
        0,
        "the rejected timestamp setter must not disturb the compared version"
    );
    assert!(!delta.is_null());
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_dataset_delta_get_inserted_rows(delta, &mut stream) },
        0
    );
    assert_eq!(ids_of(&collect_stream(stream).1), vec![5, 6, 7]);

    // NULL out slots are rejected without consuming the delta.
    assert_eq!(
        unsafe { lance_dataset_delta_list_transactions(delta, ptr::null_mut(), ptr::null_mut()) },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let _ = take_last_error_message();
    assert_eq!(
        unsafe { lance_dataset_delta_get_inserted_rows(delta, ptr::null_mut()) },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let _ = take_last_error_message();
    assert!(unsafe { lance_dataset_delta_builder_new(ptr::null()) }.is_null());
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let _ = take_last_error_message();
    unsafe {
        lance_dataset_delta_close(ptr::null_mut());
        lance_dataset_delta_builder_close(ptr::null_mut());
        lance_dataset_delta_close(delta);
        lance_dataset_close(ds);
    }
}

#[test]
fn test_delta_deleted_ids_require_stable_row_ids() {
    let (_tmp, uri) = create_sql_dataset();
    let ds = open_dataset(&uri, 0);

    // The fixture has two versions but no stable row ids: the native reader
    // rejects the request at the stream call, after a successful build.
    let delta = build_c_delta(ds, |builder| {
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_begin_version(builder, 1) },
            0
        );
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_end_version(builder, 2) },
            0
        );
    });
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_dataset_delta_get_deleted_row_ids(delta, &mut stream) },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let message = take_last_error_message();
    assert!(
        message.contains("stable row ids"),
        "native unsupported-case error should surface, got: {message}"
    );

    // Row-change streams still work on the same delta.
    let inserted = c_delta_stream(delta, DeltaStream::Inserted);
    assert_eq!(
        ids_of(&inserted),
        vec![11, 12, 13, 14, 15, 16, 17, 18, 19, 20]
    );

    unsafe {
        lance_dataset_delta_close(delta);
        lance_dataset_close(ds);
    }
}

#[test]
fn test_delta_timestamp_representability() {
    let (_tmp, uri, _timestamps) = create_history_dataset();
    let ds = open_dataset(&uri, 0);

    // The native chrono construction owns representability: a timestamp it
    // cannot represent is rejected at the setter, and the builder keeps its
    // prior state.
    let builder = unsafe { lance_dataset_delta_builder_new(ds) };
    assert!(!builder.is_null());
    assert_eq!(
        unsafe { lance_dataset_delta_builder_set_compared_against_version(builder, 1) },
        0
    );
    let unrepresentable = LanceDeltaTimestamp {
        unix_seconds: i64::MAX,
        nanos: 0,
    };
    assert_eq!(
        unsafe { lance_dataset_delta_builder_set_begin_timestamp(builder, &unrepresentable) },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let message = take_last_error_message();
    assert!(
        message.contains("not representable"),
        "native chrono representability error should surface, got: {message}"
    );
    // A non-normalized nanosecond field is equally rejected by chrono.
    let over_nanos = LanceDeltaTimestamp {
        unix_seconds: 0,
        nanos: 1_000_000_000,
    };
    assert_eq!(
        unsafe { lance_dataset_delta_builder_set_end_timestamp(builder, &over_nanos) },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let _ = take_last_error_message();
    let mut delta: *mut LanceDatasetDelta = ptr::null_mut();
    assert_eq!(
        unsafe { lance_dataset_delta_builder_build(builder, &mut delta) },
        0,
        "rejected timestamps must leave the compared version in place"
    );
    assert!(!delta.is_null());
    assert_eq!(
        ids_of(&c_delta_stream(delta, DeltaStream::Inserted)),
        vec![5, 6, 7]
    );

    // A representable window before the first commit resolves to an empty
    // change set rather than an error.
    let delta = build_c_delta(ds, |builder| {
        let begin = LanceDeltaTimestamp {
            unix_seconds: 0,
            nanos: 0,
        };
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_begin_timestamp(builder, &begin) },
            0
        );
        assert_eq!(
            unsafe { lance_dataset_delta_builder_set_end_timestamp(builder, &begin) },
            0
        );
    });
    assert!(c_delta_stream(delta, DeltaStream::Inserted).is_empty());

    unsafe {
        lance_dataset_delta_close(delta);
        lance_dataset_close(ds);
    }
}
