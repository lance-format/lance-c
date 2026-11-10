// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

use std::collections::HashMap;
use std::ffi::{CStr, CString, c_char};
use std::ptr;
use std::sync::Arc;

use arrow::ffi::{FFI_ArrowSchema, from_ffi};
use arrow::record_batch::RecordBatch;
use arrow_array::{Array, FixedSizeListArray, Float32Array, Int32Array, StructArray, UInt64Array};
use arrow_schema::{DataType, Field, Schema};
use futures::StreamExt;
use lance::Dataset;
use lance::dataset::WriteParams;
use lance::dataset::scanner::{
    ColumnOrdering as NativeColumnOrdering, RowAddrMask, RowAddrTreeMap,
};
use lance_c::*;

fn c_str(value: &str) -> CString {
    CString::new(value).unwrap()
}

fn take_error_message() -> String {
    let raw = lance_last_error_message();
    assert!(!raw.is_null(), "error code must carry a message");
    let message = unsafe { CStr::from_ptr(raw) }
        .to_string_lossy()
        .into_owned();
    unsafe { lance_free_string(raw) };
    message
}

fn take_owned_string(raw: *const c_char) -> String {
    assert!(!raw.is_null(), "{}", take_error_message());
    let value = unsafe { CStr::from_ptr(raw) }
        .to_string_lossy()
        .into_owned();
    unsafe { lance_free_string(raw) };
    value
}

fn take_schema(scanner: *const LanceScanner) -> Schema {
    let mut ffi = FFI_ArrowSchema::empty();
    assert_eq!(
        unsafe { lance_scanner_schema(scanner, &mut ffi) },
        0,
        "{}",
        take_error_message()
    );
    let schema = Schema::try_from(&ffi).unwrap();
    if let Some(release) = ffi.release {
        unsafe { release(&mut ffi) };
    }
    schema
}

fn schema_signature(schema: &Schema) -> Vec<(String, DataType, bool)> {
    schema
        .fields()
        .iter()
        .map(|field| {
            (
                field.name().clone(),
                field.data_type().clone(),
                field.is_nullable(),
            )
        })
        .collect()
}

fn fixture() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp
        .path()
        .join("scanner-introspection")
        .to_str()
        .unwrap()
        .to_string();
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("category", DataType::Utf8, true),
        Field::new("rank", DataType::Int32, true),
        Field::new(
            "embedding",
            DataType::FixedSizeList(Arc::new(Field::new("item", DataType::Float32, false)), 2),
            false,
        ),
        Field::new("text", DataType::Utf8, false),
    ]));
    let batch = |start: i32| {
        let ids = (start..start + 4).collect::<Vec<_>>();
        let categories = ids
            .iter()
            .map(|id| if id % 2 == 0 { "a" } else { "b" })
            .collect::<Vec<_>>();
        let ranks = ids.iter().map(|id| Some(id * 2)).collect::<Vec<_>>();
        let values = ids
            .iter()
            .flat_map(|id| [*id as f32, *id as f32 + 1.0])
            .collect::<Vec<_>>();
        let embeddings = FixedSizeListArray::try_new(
            Arc::new(Field::new("item", DataType::Float32, false)),
            2,
            Arc::new(Float32Array::from(values)),
            None,
        )
        .unwrap();
        let text = ids
            .iter()
            .map(|id| format!("row {id} quick brown"))
            .collect::<Vec<_>>();
        let text_refs = text.iter().map(String::as_str).collect::<Vec<_>>();
        RecordBatch::try_new(
            schema.clone(),
            vec![
                Arc::new(Int32Array::from(ids)),
                Arc::new(arrow_array::StringArray::from(categories)),
                Arc::new(Int32Array::from(ranks)),
                Arc::new(embeddings),
                Arc::new(arrow_array::StringArray::from(text_refs)),
            ],
        )
        .unwrap()
    };

    lance_c::runtime::block_on(async {
        let mut dataset = Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch(0))], schema.clone()),
            &uri,
            Some(WriteParams {
                enable_stable_row_ids: true,
                ..Default::default()
            }),
        )
        .await
        .unwrap();
        for start in [4, 8] {
            dataset
                .append(
                    arrow::record_batch::RecordBatchIterator::new(
                        vec![Ok(batch(start))],
                        schema.clone(),
                    ),
                    None,
                )
                .await
                .unwrap();
        }
    });
    (tmp, uri)
}

fn native_row_ids(uri: &str) -> HashMap<i32, u64> {
    lance_c::runtime::block_on(async {
        let dataset = Dataset::open(uri).await.unwrap();
        let mut scanner = dataset.scan();
        scanner.project(&["id"]).unwrap();
        scanner.with_row_id();
        let mut stream = scanner.try_into_stream().await.unwrap();
        let mut rows = HashMap::new();
        while let Some(batch) = stream.next().await {
            let batch = batch.unwrap();
            let ids = batch
                .column_by_name("id")
                .unwrap()
                .as_any()
                .downcast_ref::<Int32Array>()
                .unwrap();
            let row_ids = batch
                .column_by_name("_rowid")
                .unwrap()
                .as_any()
                .downcast_ref::<UInt64Array>()
                .unwrap();
            for row in 0..batch.num_rows() {
                rows.insert(ids.value(row), row_ids.value(row));
            }
        }
        rows
    })
}

fn next_batch_rows(scanner: *mut LanceScanner) -> Option<Vec<(i32, Option<i32>)>> {
    let mut batch_ptr = ptr::null_mut();
    let rc = unsafe { lance_scanner_next(scanner, &mut batch_ptr) };
    match rc {
        1 => None,
        0 => {
            assert!(!batch_ptr.is_null());
            let mut ffi_array = arrow::ffi::FFI_ArrowArray::empty();
            let mut ffi_schema = FFI_ArrowSchema::empty();
            assert_eq!(
                unsafe { lance_batch_to_arrow(batch_ptr, &mut ffi_array, &mut ffi_schema) },
                0
            );
            let data = unsafe { from_ffi(ffi_array, &ffi_schema) }.unwrap();
            if let Some(release) = ffi_schema.release {
                unsafe { release(&mut ffi_schema) };
            }
            let batch = RecordBatch::from(StructArray::from(data));
            let ids = batch
                .column_by_name("id")
                .unwrap()
                .as_any()
                .downcast_ref::<Int32Array>()
                .unwrap();
            let ranks = batch
                .column_by_name("rank")
                .unwrap()
                .as_any()
                .downcast_ref::<Int32Array>()
                .unwrap();
            let rows = (0..batch.num_rows())
                .map(|row| {
                    (
                        ids.value(row),
                        (!ranks.is_null(row)).then(|| ranks.value(row)),
                    )
                })
                .collect();
            unsafe { lance_batch_free(batch_ptr) };
            Some(rows)
        }
        -1 => panic!("{}", take_error_message()),
        other => panic!("unexpected lance_scanner_next status {other}"),
    }
}

#[test]
fn dataset_filtered_count_matches_native_and_preserves_null_vs_empty() {
    let (_tmp, uri) = fixture();
    let uri_c = c_str(&uri);
    let dataset = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    assert!(!dataset.is_null(), "{}", take_error_message());

    let native = lance_c::runtime::block_on(async {
        let snapshot = Dataset::open(&uri).await.unwrap();
        (
            snapshot.count_rows(None).await.unwrap() as u64,
            snapshot
                .count_rows(Some("rank >= 8 AND category = 'b'".to_string()))
                .await
                .unwrap() as u64,
        )
    });
    assert_eq!(unsafe { lance_dataset_count_rows(dataset) }, native.0);
    assert_eq!(
        unsafe { lance_dataset_count_rows_filtered(dataset, ptr::null()) },
        native.0
    );
    let filter = c_str("rank >= 8 AND category = 'b'");
    assert_eq!(
        unsafe { lance_dataset_count_rows_filtered(dataset, filter.as_ptr()) },
        native.1
    );
    assert_eq!(native.1, 4);
    assert_eq!(lance_last_error_code(), LanceErrorCode::Ok);

    let empty = c_str("");
    assert_eq!(
        unsafe { lance_dataset_count_rows_filtered(dataset, empty.as_ptr()) },
        0
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    let c_error = take_error_message();
    let native_error = lance_c::runtime::block_on(async {
        Dataset::open(&uri)
            .await
            .unwrap()
            .count_rows(Some(String::new()))
            .await
            .unwrap_err()
            .to_string()
    });
    assert_eq!(c_error, native_error);

    assert_eq!(
        unsafe { lance_dataset_count_rows_filtered(ptr::null(), ptr::null()) },
        0
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert!(take_error_message().contains("dataset must not be NULL"));

    unsafe { lance_dataset_close(dataset) };
}

#[test]
fn scanner_schema_count_and_plan_strings_match_native_oracles() {
    let (_tmp, uri) = fixture();
    let uri_c = c_str(&uri);
    let dataset = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    assert!(!dataset.is_null(), "{}", take_error_message());
    let row_ids = native_row_ids(&uri);
    let allow_ids = [0, 1, 2, 4, 6, 8, 10]
        .into_iter()
        .map(|id| row_ids[&id])
        .collect::<Vec<_>>();
    let block_ids = [row_ids[&3]];
    let fragments = [0_u64, 2];
    let projection = [("id_alias", "id"), ("rank_alias", "rank + 1")];
    let filter = "rank >= 4";

    let native = lance_c::runtime::block_on(async {
        let snapshot = Dataset::open(&uri).await.unwrap();
        let mut scanner = snapshot.scan();
        scanner.project_with_transform(&projection).unwrap();
        scanner.filter(filter).unwrap();
        let selected = snapshot
            .get_fragments()
            .into_iter()
            .filter(|fragment| fragments.contains(&(fragment.id() as u64)))
            .map(|fragment| fragment.metadata().clone())
            .collect::<Vec<_>>();
        scanner.with_fragments(selected);
        let allowed: RowAddrTreeMap = allow_ids.iter().copied().collect();
        let blocked: RowAddrTreeMap = block_ids.iter().copied().collect();
        scanner.with_row_id_prefilter(RowAddrMask::from_allowed(allowed).also_block(blocked));
        let schema = scanner.schema().await.unwrap();
        let count = scanner.count_rows().await.unwrap();
        let explain = scanner.explain_plan(false).await.unwrap();
        (schema, count, explain)
    });

    let filter_c = c_str(filter);
    let scanner = unsafe { lance_scanner_new(dataset, ptr::null(), filter_c.as_ptr()) };
    assert!(!scanner.is_null(), "{}", take_error_message());
    let names = projection.map(|(name, _)| c_str(name));
    let expressions = projection.map(|(_, expression)| c_str(expression));
    let columns = names
        .iter()
        .zip(&expressions)
        .map(|(name, expression)| LanceProjectionColumn {
            output_name: name.as_ptr(),
            expression: expression.as_ptr(),
        })
        .collect::<Vec<_>>();
    assert_eq!(
        unsafe { lance_scanner_set_projection(scanner, columns.as_ptr(), columns.len()) },
        0,
        "{}",
        take_error_message()
    );
    assert_eq!(
        unsafe { lance_scanner_set_fragment_ids(scanner, fragments.as_ptr(), fragments.len()) },
        0,
        "{}",
        take_error_message()
    );
    assert_eq!(
        unsafe {
            lance_scanner_set_row_addr_allowlist(scanner, allow_ids.as_ptr(), allow_ids.len())
        },
        0,
        "{}",
        take_error_message()
    );
    assert_eq!(
        unsafe {
            lance_scanner_set_row_addr_blocklist(scanner, block_ids.as_ptr(), block_ids.len())
        },
        0,
        "{}",
        take_error_message()
    );

    let schema = take_schema(scanner);
    assert_eq!(schema_signature(&schema), schema_signature(&native.0));
    assert_eq!(
        schema
            .fields()
            .iter()
            .map(|field| field.name().as_str())
            .collect::<Vec<_>>(),
        vec!["id_alias", "rank_alias"]
    );
    assert_eq!(
        schema_signature(&take_schema(scanner)),
        schema_signature(&schema)
    );

    let count = unsafe { lance_scanner_count_rows(scanner) };
    assert_eq!(lance_last_error_code(), LanceErrorCode::Ok);
    assert_eq!(count, native.1);
    assert_eq!(
        count, 3,
        "fragment, filter, allow and block scopes must all apply"
    );
    assert_eq!(unsafe { lance_scanner_count_rows(scanner) }, count);

    let c_explain = take_owned_string(unsafe { lance_scanner_explain_plan(scanner, false) });
    assert_eq!(c_explain, native.2);
    assert_eq!(
        take_owned_string(unsafe { lance_scanner_explain_plan(scanner, false) }),
        c_explain
    );
    let verbose = take_owned_string(unsafe { lance_scanner_explain_plan(scanner, true) });
    assert!(verbose.len() >= c_explain.len());

    let analyze = take_owned_string(unsafe { lance_scanner_analyze_plan(scanner) });
    assert!(!analyze.is_empty());
    assert!(!analyze.contains("AggregateExec"), "{analyze}");
    let analyze_count = take_owned_string(unsafe { lance_scanner_analyze_count_plan(scanner) });
    assert!(analyze_count.contains("AggregateExec"), "{analyze_count}");
    assert_ne!(analyze, analyze_count);

    unsafe {
        lance_scanner_close(scanner);
        lance_dataset_close(dataset);
    }
}

#[test]
fn scanner_count_preserves_native_aggregate_planning_errors() {
    let (_tmp, uri) = fixture();
    let uri_c = c_str(&uri);
    let dataset = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    assert!(!dataset.is_null());
    let native = lance_c::runtime::block_on(Dataset::open(&uri)).unwrap();

    for setting in ["limit_zero", "offset_zero", "order_nonempty"] {
        let scanner = unsafe { lance_scanner_new(dataset, ptr::null(), ptr::null()) };
        assert!(!scanner.is_null());
        let native_error = lance_c::runtime::block_on(async {
            let mut native_scanner = native.scan();
            match setting {
                "limit_zero" => native_scanner.limit(Some(0), None).unwrap(),
                "offset_zero" => native_scanner.limit(None, Some(0)).unwrap(),
                "order_nonempty" => native_scanner
                    .order_by(Some(vec![NativeColumnOrdering::asc_nulls_first(
                        "id".to_string(),
                    )]))
                    .unwrap(),
                _ => unreachable!(),
            };
            native_scanner.count_rows().await.unwrap_err().to_string()
        });
        let rc = match setting {
            "limit_zero" => unsafe { lance_scanner_set_limit(scanner, 0) },
            "offset_zero" => unsafe { lance_scanner_set_offset(scanner, 0) },
            "order_nonempty" => {
                let name = c_str("id");
                let ordering = LanceColumnOrdering {
                    column_name: name.as_ptr(),
                    direction: LanceOrderDirection::Ascending as i32,
                    null_order: LanceNullOrder::NullsFirst as i32,
                };
                unsafe { lance_scanner_set_order_by(scanner, &ordering, 1) }
            }
            _ => unreachable!(),
        };
        assert_eq!(rc, 0);
        assert_eq!(unsafe { lance_scanner_count_rows(scanner) }, 0);
        assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
        assert_eq!(take_error_message(), native_error, "{setting}");
        unsafe { lance_scanner_close(scanner) };
    }

    // The pinned public Scanner::order_by normalizes an explicit empty list to
    // None before aggregate validation. The binding preserves that native
    // behavior rather than inventing a C-only rejection.
    let scanner = unsafe { lance_scanner_new(dataset, ptr::null(), ptr::null()) };
    let empty = LanceColumnOrdering {
        column_name: ptr::null(),
        direction: LanceOrderDirection::Ascending as i32,
        null_order: LanceNullOrder::NullsLast as i32,
    };
    assert_eq!(unsafe { lance_scanner_set_order_by(scanner, &empty, 0) }, 0);
    let native_count = lance_c::runtime::block_on(async {
        let mut native_scanner = native.scan();
        native_scanner.order_by(Some(Vec::new())).unwrap();
        native_scanner.count_rows().await.unwrap()
    });
    assert_eq!(unsafe { lance_scanner_count_rows(scanner) }, native_count);
    assert_eq!(lance_last_error_code(), LanceErrorCode::Ok);
    unsafe { lance_scanner_close(scanner) };

    unsafe { lance_dataset_close(dataset) };
}

#[test]
fn inspection_does_not_advance_streams_and_setters_freeze_at_iteration() {
    let (_tmp, uri) = fixture();
    let uri_c = c_str(&uri);
    let dataset = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    assert!(!dataset.is_null());

    let build = || {
        let filter = c_str("rank >= 2");
        let scanner = unsafe { lance_scanner_new(dataset, ptr::null(), filter.as_ptr()) };
        assert!(!scanner.is_null());
        assert_eq!(unsafe { lance_scanner_set_batch_size(scanner, 2) }, 0);
        assert_eq!(unsafe { lance_scanner_set_scan_in_order(scanner, true) }, 0);
        scanner
    };
    let baseline = build();
    let inspected = build();

    let baseline_first = next_batch_rows(baseline).unwrap();
    let inspected_first = next_batch_rows(inspected).unwrap();
    assert_eq!(inspected_first, baseline_first);

    let _ = take_schema(inspected);
    let _ = take_owned_string(unsafe { lance_scanner_explain_plan(inspected, true) });
    let _ = take_owned_string(unsafe { lance_scanner_analyze_plan(inspected) });
    let count = unsafe { lance_scanner_count_rows(inspected) };
    assert_eq!(lance_last_error_code(), LanceErrorCode::Ok);
    let _ = take_owned_string(unsafe { lance_scanner_analyze_count_plan(inspected) });

    let mut baseline_rows = baseline_first;
    while let Some(rows) = next_batch_rows(baseline) {
        baseline_rows.extend(rows);
    }
    let mut inspected_rows = inspected_first;
    while let Some(rows) = next_batch_rows(inspected) {
        inspected_rows.extend(rows);
    }
    assert_eq!(inspected_rows, baseline_rows);
    assert_eq!(count, baseline_rows.len() as u64);

    assert_eq!(
        unsafe { lance_scanner_set_projection(inspected, ptr::null(), 0) },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert!(take_error_message().contains("must be set before the scan starts"));

    // A read-only inspection does not freeze configuration. Setters still apply
    // to the eventual stream.
    let scanner = build();
    let _ = take_schema(scanner);
    let _ = take_owned_string(unsafe { lance_scanner_explain_plan(scanner, false) });
    assert_eq!(unsafe { lance_scanner_set_limit(scanner, 2) }, 0);
    let mut rows = next_batch_rows(scanner).unwrap();
    while let Some(more) = next_batch_rows(scanner) {
        rows.extend(more);
    }
    assert_eq!(rows.len(), 2);

    unsafe {
        lance_scanner_close(scanner);
        lance_scanner_close(inspected);
        lance_scanner_close(baseline);
        lance_dataset_close(dataset);
    }
}

#[test]
fn scanner_introspection_rejects_null_and_nonempty_output_slots() {
    let (_tmp, uri) = fixture();
    let uri_c = c_str(&uri);
    let dataset = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    let scanner = unsafe { lance_scanner_new(dataset, ptr::null(), ptr::null()) };
    assert!(!scanner.is_null());

    let mut schema = FFI_ArrowSchema::empty();
    assert_eq!(
        unsafe { lance_scanner_schema(ptr::null(), &mut schema) },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert!(take_error_message().contains("scanner and out must not be NULL"));
    assert_eq!(
        unsafe { lance_scanner_schema(scanner, ptr::null_mut()) },
        -1
    );
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert!(take_error_message().contains("scanner and out must not be NULL"));

    let mut nonempty = FFI_ArrowSchema::try_from(&Field::new("x", DataType::Int32, true)).unwrap();
    assert_eq!(unsafe { lance_scanner_schema(scanner, &mut nonempty) }, -1);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert!(take_error_message().contains("empty ArrowSchema"));
    if let Some(release) = nonempty.release {
        unsafe { release(&mut nonempty) };
    }

    assert!(unsafe { lance_scanner_explain_plan(ptr::null(), false) }.is_null());
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert!(take_error_message().contains("scanner is NULL"));
    assert!(unsafe { lance_scanner_analyze_plan(ptr::null()) }.is_null());
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert!(take_error_message().contains("scanner is NULL"));
    assert!(unsafe { lance_scanner_analyze_count_plan(ptr::null()) }.is_null());
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert!(take_error_message().contains("scanner is NULL"));
    assert_eq!(unsafe { lance_scanner_count_rows(ptr::null()) }, 0);
    assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
    assert!(take_error_message().contains("scanner is NULL"));

    unsafe {
        lance_scanner_close(scanner);
        lance_dataset_close(dataset);
    }
}

#[test]
fn vector_and_batch_schema_count_match_native() {
    let (_tmp, uri) = fixture();
    let uri_c = c_str(&uri);
    let dataset = unsafe { lance_dataset_open(uri_c.as_ptr(), ptr::null(), 0) };
    assert!(!dataset.is_null());
    let query = [0.0_f32, 1.0];

    let native = lance_c::runtime::block_on(async {
        let snapshot = Dataset::open(&uri).await.unwrap();
        let mut scanner = snapshot.scan();
        scanner
            .nearest("embedding", &Float32Array::from(query.to_vec()), 3)
            .unwrap();
        scanner.use_index(false);
        scanner.prefilter(true);
        let schema = scanner.schema().await.unwrap();
        let count = scanner.count_rows().await.unwrap();
        (schema, count)
    });
    let column = c_str("embedding");
    let scanner = unsafe { lance_scanner_new(dataset, ptr::null(), ptr::null()) };
    assert_eq!(
        unsafe {
            lance_scanner_nearest(
                scanner,
                column.as_ptr(),
                query.as_ptr().cast(),
                query.len(),
                LanceDataType::Float32 as i32,
                3,
            )
        },
        0,
        "{}",
        take_error_message()
    );
    assert_eq!(unsafe { lance_scanner_set_use_index(scanner, false) }, 0);
    assert_eq!(unsafe { lance_scanner_set_prefilter(scanner, true) }, 0);
    assert_eq!(
        schema_signature(&take_schema(scanner)),
        schema_signature(&native.0)
    );
    assert!(take_schema(scanner).field_with_name("_distance").is_ok());
    assert_eq!(unsafe { lance_scanner_count_rows(scanner) }, native.1);
    assert_eq!(native.1, 3);
    unsafe { lance_scanner_close(scanner) };

    let queries = FixedSizeListArray::try_new(
        Arc::new(Field::new("item", DataType::Float32, false)),
        2,
        Arc::new(Float32Array::from(vec![0.0, 1.0, 8.0, 9.0])),
        None,
    )
    .unwrap();
    let native = lance_c::runtime::block_on(async {
        let snapshot = Dataset::open(&uri).await.unwrap();
        let mut scanner = snapshot.scan();
        scanner.nearest("embedding", &queries, 2).unwrap();
        scanner.use_index(false);
        scanner.prefilter(true);
        let schema = scanner.schema().await.unwrap();
        let count = scanner.count_rows().await.unwrap();
        (schema, count)
    });
    let scanner = unsafe { lance_scanner_new(dataset, ptr::null(), ptr::null()) };
    assert_eq!(
        unsafe {
            lance_scanner_nearest_batch(
                scanner,
                column.as_ptr(),
                query_values(&queries).as_ptr().cast(),
                2,
                2,
                LanceDataType::Float32 as i32,
                2,
            )
        },
        0,
        "{}",
        take_error_message()
    );
    assert_eq!(unsafe { lance_scanner_set_use_index(scanner, false) }, 0);
    assert_eq!(unsafe { lance_scanner_set_prefilter(scanner, true) }, 0);
    let schema = take_schema(scanner);
    assert_eq!(schema_signature(&schema), schema_signature(&native.0));
    assert_eq!(schema.field(0).name(), "query_index");
    assert!(schema.field_with_name("_distance").is_ok());
    assert_eq!(unsafe { lance_scanner_count_rows(scanner) }, native.1);
    assert_eq!(native.1, 4);
    unsafe {
        lance_scanner_close(scanner);
        lance_dataset_close(dataset);
    }
}

fn query_values(queries: &FixedSizeListArray) -> Vec<f32> {
    queries
        .values()
        .as_any()
        .downcast_ref::<Float32Array>()
        .unwrap()
        .values()
        .to_vec()
}
