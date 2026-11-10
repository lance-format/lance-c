// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

use std::ffi::{CStr, CString, c_void};
use std::ptr;
use std::sync::{Arc, Mutex};

use arrow::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use arrow_array::{
    Array, FixedSizeListArray, Float32Array, Int32Array, RecordBatch, RecordBatchReader,
    StringArray, StructArray,
};
use arrow_schema::{DataType, Field, Fields, Schema};
use lance::Dataset;
use lance::dataset::scanner::{
    ExecutionSummaryCounts, MaterializationStyle, RowAddrMask, RowAddrTreeMap,
};
use lance::dataset::{WriteMode, WriteParams};
use lance_c::*;

fn c(value: &str) -> CString {
    CString::new(value).unwrap()
}

fn ok(code: i32) {
    if code == 0 {
        return;
    }
    let message = unsafe {
        let pointer = lance_last_error_message();
        let message = CStr::from_ptr(pointer).to_string_lossy().into_owned();
        lance_free_string(pointer);
        message
    };
    panic!("{message}");
}

struct Data(*mut LanceDataset);

impl Drop for Data {
    fn drop(&mut self) {
        unsafe { lance_dataset_close(self.0) };
    }
}

impl Data {
    fn open(uri: &CString) -> Self {
        let dataset = unsafe { lance_dataset_open(uri.as_ptr(), ptr::null(), 0) };
        assert!(!dataset.is_null());
        Self(dataset)
    }
}

struct Scan(*mut LanceScanner);

impl Drop for Scan {
    fn drop(&mut self) {
        unsafe { lance_scanner_close(self.0) };
    }
}

#[derive(Debug, Default, PartialEq, Eq)]
struct ScanStatistics {
    calls: u64,
    bytes_read: u64,
    requests: u64,
    indices_loaded: u64,
    index_partitions_loaded: u64,
}

unsafe extern "C" fn capture_scan_statistics(
    callback_ctx: *mut c_void,
    statistics: *const LanceScanStatistics,
) {
    assert!(!callback_ctx.is_null());
    assert!(!statistics.is_null());
    let captured = unsafe { &mut *callback_ctx.cast::<ScanStatistics>() };
    let statistics = unsafe { &*statistics };
    captured.calls += 1;
    captured.bytes_read = statistics.bytes_read;
    captured.requests = statistics.requests;
    captured.indices_loaded = statistics.indices_loaded;
    captured.index_partitions_loaded = statistics.index_partitions_loaded;
}

fn scan_statistics_from_native(statistics: &ExecutionSummaryCounts) -> ScanStatistics {
    ScanStatistics {
        calls: 1,
        bytes_read: statistics.bytes_read as u64,
        requests: statistics.requests as u64,
        indices_loaded: statistics.indices_loaded as u64,
        index_partitions_loaded: statistics.parts_loaded as u64,
    }
}

fn collect_c_scanner(scanner: *mut LanceScanner) -> Vec<RecordBatch> {
    let mut stream = FFI_ArrowArrayStream::empty();
    ok(unsafe { lance_scanner_to_arrow_stream(scanner, &mut stream) });
    unsafe { ArrowArrayStreamReader::from_raw(&mut stream) }
        .unwrap()
        .map(|batch| batch.unwrap())
        .collect()
}

fn pseudo_text(seed: u32, len: usize) -> String {
    let mut state = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
    let mut text = String::with_capacity(len);
    for _ in 0..len {
        state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        text.push(char::from(b'!' + (state % 90) as u8));
    }
    text
}

#[derive(Debug, PartialEq, Eq)]
struct MaterializationRow {
    id: i32,
    wide: Option<String>,
    payload_narrow: Option<String>,
    payload_wide: Option<String>,
}

fn materialization_rows(batches: &[RecordBatch]) -> Vec<MaterializationRow> {
    let mut rows = Vec::new();
    for batch in batches {
        let ids = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        let wide = batch
            .column_by_name("wide")
            .unwrap()
            .as_any()
            .downcast_ref::<StringArray>()
            .unwrap();
        let payload = batch
            .column_by_name("payload")
            .unwrap()
            .as_any()
            .downcast_ref::<StructArray>()
            .unwrap();
        let narrow = payload
            .column_by_name("narrow")
            .unwrap()
            .as_any()
            .downcast_ref::<StringArray>()
            .unwrap();
        let payload_wide = payload
            .column_by_name("wide")
            .unwrap()
            .as_any()
            .downcast_ref::<StringArray>()
            .unwrap();
        for row in 0..batch.num_rows() {
            rows.push(MaterializationRow {
                id: ids.value(row),
                wide: (!wide.is_null(row)).then(|| wide.value(row).to_string()),
                payload_narrow: (!narrow.is_null(row)).then(|| narrow.value(row).to_string()),
                payload_wide: (!payload_wide.is_null(row))
                    .then(|| payload_wide.value(row).to_string()),
            });
        }
    }
    rows.sort_by_key(|row| row.id);
    rows
}

fn materialization_fixture() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("materialization.lance");
    let payload_fields = Fields::from(vec![
        Field::new("narrow", DataType::Utf8, true),
        Field::new("wide", DataType::Utf8, true),
    ]);
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("category", DataType::Utf8, true),
        Field::new("wide", DataType::Utf8, true),
        Field::new("payload", DataType::Struct(payload_fields.clone()), true),
    ]));

    let make_batch = |start: i32, count: i32| {
        let ids = (start..start + count).collect::<Vec<_>>();
        let categories = ids
            .iter()
            .map(|id| {
                if id % 61 == 0 {
                    Some("hit")
                } else if id % 17 == 0 {
                    None
                } else {
                    Some("miss")
                }
            })
            .collect::<Vec<_>>();
        let wide = ids
            .iter()
            .map(|id| {
                if *id == 0 {
                    None
                } else {
                    Some(pseudo_text(*id as u32 * 17 + 9, 4096))
                }
            })
            .collect::<Vec<_>>();
        let narrow = ids
            .iter()
            .map(|id| Some(format!("payload-{id}")))
            .collect::<Vec<_>>();
        let payload_wide = ids
            .iter()
            .map(|id| Some(pseudo_text(*id as u32 * 31 + 7, 4096)))
            .collect::<Vec<_>>();
        let payload = StructArray::new(
            payload_fields.clone(),
            vec![
                Arc::new(StringArray::from_iter(narrow)) as Arc<dyn Array>,
                Arc::new(StringArray::from_iter(payload_wide)) as Arc<dyn Array>,
            ],
            None,
        );
        RecordBatch::try_new(
            schema.clone(),
            vec![
                Arc::new(Int32Array::from(ids)) as Arc<dyn Array>,
                Arc::new(StringArray::from_iter(categories)) as Arc<dyn Array>,
                Arc::new(StringArray::from_iter(wide)) as Arc<dyn Array>,
                Arc::new(payload) as Arc<dyn Array>,
            ],
        )
        .unwrap()
    };

    lance_c::runtime::block_on(async {
        for (index, batch) in [make_batch(0, 128), make_batch(128, 128)]
            .into_iter()
            .enumerate()
        {
            Dataset::write(
                arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema.clone()),
                uri.to_str().unwrap(),
                Some(WriteParams {
                    mode: if index == 0 {
                        WriteMode::Create
                    } else {
                        WriteMode::Append
                    },
                    ..Default::default()
                }),
            )
            .await
            .unwrap();
        }
        let dataset = Dataset::open(uri.to_str().unwrap()).await.unwrap();
        assert_eq!(dataset.count_fragments(), 2);
    });

    (tmp, uri.to_str().unwrap().to_owned())
}

fn c_materialization_scan(
    uri: &CString,
    style: LanceMaterializationStyle,
    except_columns: &[&str],
) -> (Vec<MaterializationRow>, ScanStatistics) {
    let data = Data::open(uri);
    let id = c("id");
    let wide = c("wide");
    let payload = c("payload");
    let projection = [id.as_ptr(), wide.as_ptr(), payload.as_ptr(), ptr::null()];
    let filter = c("category = 'hit'");
    let scan = Scan(unsafe { lance_scanner_new(data.0, projection.as_ptr(), filter.as_ptr()) });
    assert!(!scan.0.is_null());

    {
        let columns = except_columns
            .iter()
            .map(|name| c(name))
            .collect::<Vec<_>>();
        let pointers = columns
            .iter()
            .map(|column| column.as_ptr())
            .collect::<Vec<_>>();
        ok(unsafe {
            lance_scanner_set_materialization_style(
                scan.0,
                style,
                pointers.as_ptr(),
                pointers.len(),
            )
        });
    }

    let mut statistics = ScanStatistics::default();
    ok(unsafe {
        lance_scanner_set_statistics_callback(
            scan.0,
            Some(capture_scan_statistics),
            (&mut statistics as *mut ScanStatistics).cast(),
        )
    });
    let rows = materialization_rows(&collect_c_scanner(scan.0));
    (rows, statistics)
}

fn direct_materialization_scan(
    uri: &str,
    style: LanceMaterializationStyle,
    except_columns: &[&str],
) -> (Vec<MaterializationRow>, ScanStatistics) {
    lance_c::runtime::block_on(async {
        let dataset = Dataset::open(uri).await.unwrap();
        let style = match style {
            LanceMaterializationStyle::Heuristic => MaterializationStyle::Heuristic,
            LanceMaterializationStyle::AllLate => MaterializationStyle::AllLate,
            LanceMaterializationStyle::AllEarly => MaterializationStyle::AllEarly,
            LanceMaterializationStyle::AllEarlyExcept => {
                MaterializationStyle::all_early_except(except_columns, dataset.schema()).unwrap()
            }
        };
        let statistics = Arc::new(Mutex::new(None));
        let collected = Arc::clone(&statistics);
        let mut scan = dataset.scan();
        scan.project(&["id", "wide", "payload"]).unwrap();
        scan.filter("category = 'hit'").unwrap();
        scan.materialization_style(style);
        scan.scan_stats_callback(Arc::new(move |summary| {
            *collected.lock().unwrap() = Some(summary.clone());
        }));
        let batch = scan.try_into_batch().await.unwrap();
        let statistics = statistics
            .lock()
            .unwrap()
            .as_ref()
            .map(scan_statistics_from_native)
            .unwrap();
        (materialization_rows(&[batch]), statistics)
    })
}

#[test]
fn materialization_styles_change_execution_and_match_direct_native_scans() {
    let (_tmp, uri) = materialization_fixture();
    let uri_c = c(&uri);
    let styles = [
        ("late", LanceMaterializationStyle::AllLate, Vec::new()),
        ("early", LanceMaterializationStyle::AllEarly, Vec::new()),
        (
            "empty-except",
            LanceMaterializationStyle::AllEarlyExcept,
            Vec::new(),
        ),
        (
            "nested-except",
            LanceMaterializationStyle::AllEarlyExcept,
            vec!["payload.wide"],
        ),
        (
            "heuristic",
            LanceMaterializationStyle::Heuristic,
            Vec::new(),
        ),
    ];

    let mut results = Vec::new();
    for (name, style, columns) in &styles {
        let (wrapper_rows, wrapper_statistics) = c_materialization_scan(&uri_c, *style, columns);
        let (native_rows, native_statistics) = direct_materialization_scan(&uri, *style, columns);
        assert_eq!(wrapper_rows, native_rows, "{name}");
        assert_eq!(
            wrapper_statistics, native_statistics,
            "{name} statistics differ"
        );
        assert_eq!(wrapper_statistics.calls, 1, "{name}");
        assert!(wrapper_statistics.bytes_read > 0, "{name}");
        results.push((name.to_string(), wrapper_rows, wrapper_statistics));
    }

    let expected_ids: Vec<i32> = (0..256).filter(|id| id % 61 == 0).collect();
    for (name, rows, _) in &results {
        assert_eq!(
            rows.iter().map(|row| row.id).collect::<Vec<_>>(),
            expected_ids,
            "{name}"
        );
    }
    let late = &results[0].2;
    let early = &results[1].2;
    let empty_except = &results[2].2;
    let nested_except = &results[3].2;
    assert!(
        late.bytes_read < nested_except.bytes_read,
        "late={late:?} nested-except={nested_except:?}"
    );
    assert!(
        nested_except.bytes_read < early.bytes_read,
        "nested-except={nested_except:?} early={early:?}"
    );
    assert_eq!(
        empty_except.bytes_read, early.bytes_read,
        "an empty exception list must be indistinguishable from ALL_EARLY"
    );
    assert!(
        results[0].1[0].wide.is_none(),
        "nullable projected payload must preserve NULL"
    );
}

#[test]
fn materialization_rejections_preserve_state_and_lifecycle() {
    let (_tmp, uri) = materialization_fixture();
    let uri_c = c(&uri);
    let data = Data::open(&uri_c);
    let id = c("id");
    let wide = c("wide");
    let payload = c("payload");
    let projection = [id.as_ptr(), wide.as_ptr(), payload.as_ptr(), ptr::null()];
    let filter = c("category = 'hit'");
    let scan = Scan(unsafe { lance_scanner_new(data.0, projection.as_ptr(), filter.as_ptr()) });
    assert!(!scan.0.is_null());

    ok(unsafe {
        lance_scanner_set_materialization_style(
            scan.0,
            LanceMaterializationStyle::AllLate,
            ptr::null(),
            0,
        )
    });
    let missing = c("payload.missing");
    assert_eq!(
        unsafe {
            lance_scanner_set_materialization_style(
                scan.0,
                LanceMaterializationStyle::AllEarlyExcept,
                [missing.as_ptr()].as_ptr(),
                1,
            )
        },
        -1
    );
    assert_eq!(
        unsafe { lance_last_error_code() },
        LanceErrorCode::InvalidArgument
    );
    assert_eq!(
        unsafe {
            lance_scanner_set_materialization_style(
                scan.0,
                LanceMaterializationStyle::AllEarlyExcept,
                [ptr::null()].as_ptr(),
                1,
            )
        },
        -1
    );
    assert_eq!(
        unsafe {
            lance_scanner_set_materialization_style(
                scan.0,
                LanceMaterializationStyle::AllEarly,
                [payload.as_ptr()].as_ptr(),
                1,
            )
        },
        -1
    );
    assert_eq!(
        unsafe { lance_scanner_set_materialization_style(scan.0, 99, ptr::null(), 0) },
        -1
    );

    let mut statistics = ScanStatistics::default();
    ok(unsafe {
        lance_scanner_set_statistics_callback(
            scan.0,
            Some(capture_scan_statistics),
            (&mut statistics as *mut ScanStatistics).cast(),
        )
    });
    let wrapper_rows = materialization_rows(&collect_c_scanner(scan.0));
    let (baseline_rows, baseline_statistics) =
        c_materialization_scan(&uri_c, LanceMaterializationStyle::AllLate, &[]);
    assert_eq!(wrapper_rows, baseline_rows);
    assert_eq!(statistics, baseline_statistics);

    assert_eq!(unsafe { lance_scanner_set_fast_search(scan.0, true) }, -1);
    assert_eq!(
        unsafe {
            lance_scanner_set_materialization_style(
                scan.0,
                LanceMaterializationStyle::AllEarly,
                ptr::null(),
                0,
            )
        },
        -1
    );
}

fn fast_search_fixture() -> (tempfile::TempDir, String) {
    use lance::index::{DatasetIndexExt, vector::VectorIndexParams};
    use lance_index::IndexType;
    use lance_linalg::distance::MetricType;

    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("fast-search.lance");
    let item = Arc::new(Field::new("item", DataType::Float32, false));
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("category", DataType::Utf8, true),
        Field::new("embedding", DataType::FixedSizeList(item.clone(), 2), false),
    ]));

    let make_batch = |ids: Vec<i32>, vector: impl Fn(i32) -> [f32; 2]| {
        let categories = ids
            .iter()
            .map(|id| if *id == 64 { "hit" } else { "other" })
            .collect::<Vec<_>>();
        let values = Float32Array::from_iter_values(ids.iter().flat_map(|id| vector(*id)));
        let embeddings =
            FixedSizeListArray::try_new(item.clone(), 2, Arc::new(values), None).unwrap();
        RecordBatch::try_new(
            schema.clone(),
            vec![
                Arc::new(Int32Array::from(ids)) as Arc<dyn Array>,
                Arc::new(StringArray::from(categories)) as Arc<dyn Array>,
                Arc::new(embeddings) as Arc<dyn Array>,
            ],
        )
        .unwrap()
    };

    lance_c::runtime::block_on(async {
        let indexed = make_batch((100..164).collect(), |id| [id as f32, 1.0]);
        let mut dataset = Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(indexed)], schema.clone()),
            uri.to_str().unwrap(),
            Some(WriteParams {
                mode: WriteMode::Create,
                ..Default::default()
            }),
        )
        .await
        .unwrap();
        let params = VectorIndexParams::ivf_flat(2, MetricType::L2);
        let segment = dataset
            .create_index_builder(&["embedding"], IndexType::Vector, &params)
            .name("embedding_idx".into())
            .fragments(vec![0])
            .execute_uncommitted()
            .await
            .unwrap();
        dataset
            .commit_existing_index_segments("embedding_idx", "embedding", vec![segment])
            .await
            .unwrap();

        let mut appended_ids = vec![64];
        appended_ids.extend(200..263);
        let appended = make_batch(appended_ids, |id| {
            if id == 64 {
                [0.0, 0.0]
            } else {
                [id as f32, 1.0]
            }
        });
        let mut dataset = Dataset::open(uri.to_str().unwrap()).await.unwrap();
        dataset
            .append(
                arrow::record_batch::RecordBatchIterator::new(vec![Ok(appended)], schema.clone()),
                None,
            )
            .await
            .unwrap();

        let unindexed = dataset
            .unindexed_fragments("embedding_idx")
            .await
            .unwrap()
            .into_iter()
            .map(|fragment| fragment.id())
            .collect::<Vec<_>>();
        assert_eq!(
            unindexed,
            vec![1],
            "index must cover only the first fragment"
        );
        assert_eq!(dataset.count_fragments(), 2);
        assert_eq!(dataset.load_indices().await.unwrap().len(), 1);
    });

    (tmp, uri.to_str().unwrap().to_owned())
}

#[derive(Clone, Copy)]
struct FastSearchCase {
    fast_search: bool,
    use_index: Option<bool>,
    filter: Option<&'static str>,
    prefilter: bool,
    row_ids: Option<&'static [u64]>,
    fragment_ids: Option<&'static [u64]>,
}

const ROW_IDS: &[u64] = &[(1 << 32), 0, 1];
const UNINDEXED_FRAGMENT: &[u64] = &[1];
const INDEXED_FRAGMENT: &[u64] = &[0];

fn c_fast_search(uri: &CString, case: FastSearchCase) -> (Vec<i32>, ScanStatistics) {
    let data = Data::open(uri);
    let id = c("id");
    let projection = [id.as_ptr(), ptr::null()];
    let filter = case.filter.map(c);
    let scan = Scan(unsafe {
        lance_scanner_new(
            data.0,
            projection.as_ptr(),
            filter
                .as_ref()
                .map_or(ptr::null(), |filter| filter.as_ptr()),
        )
    });
    assert!(!scan.0.is_null());
    if case.prefilter {
        ok(unsafe { lance_scanner_set_prefilter(scan.0, true) });
    }
    if let Some(row_ids) = case.row_ids {
        ok(unsafe {
            lance_scanner_set_row_addr_allowlist(scan.0, row_ids.as_ptr(), row_ids.len())
        });
    }
    if let Some(fragment_ids) = case.fragment_ids {
        ok(unsafe {
            lance_scanner_set_fragment_ids(scan.0, fragment_ids.as_ptr(), fragment_ids.len())
        });
    }
    ok(unsafe { lance_scanner_set_fast_search(scan.0, case.fast_search) });
    let query = [0.0f32, 0.0];
    ok(unsafe {
        lance_scanner_nearest(
            scan.0,
            c("embedding").as_ptr(),
            query.as_ptr().cast(),
            2,
            LanceDataType::Float32 as i32,
            3,
        )
    });
    ok(unsafe { lance_scanner_set_nprobes(scan.0, 2) });
    if let Some(use_index) = case.use_index {
        ok(unsafe { lance_scanner_set_use_index(scan.0, use_index) });
    }

    let mut statistics = ScanStatistics::default();
    ok(unsafe {
        lance_scanner_set_statistics_callback(
            scan.0,
            Some(capture_scan_statistics),
            (&mut statistics as *mut ScanStatistics).cast(),
        )
    });
    let batches = collect_c_scanner(scan.0);
    let mut ids = Vec::new();
    for batch in batches {
        let values = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        ids.extend((0..values.len()).map(|row| values.value(row)));
    }
    ids.sort_unstable();
    (ids, statistics)
}

fn direct_fast_search(uri: &str, case: FastSearchCase) -> (Vec<i32>, ScanStatistics) {
    lance_c::runtime::block_on(async {
        let dataset = Dataset::open(uri).await.unwrap();
        let mut scan = dataset.scan();
        scan.project(&["id"]).unwrap();
        if let Some(filter) = case.filter {
            scan.filter(filter).unwrap();
        }
        if case.prefilter {
            scan.prefilter(true);
        }
        if case.fast_search {
            scan.fast_search();
        }
        if let Some(row_ids) = case.row_ids {
            let rows: RowAddrTreeMap = row_ids.iter().copied().collect();
            scan.with_row_addr_prefilter(RowAddrMask::from_allowed(rows));
        }
        let selected_fragments = case.fragment_ids.map(|ids| {
            dataset
                .get_fragments()
                .into_iter()
                .filter(|fragment| ids.contains(&fragment.id()))
                .collect::<Vec<_>>()
        });
        if case.prefilter
            && let Some(fragments) = &selected_fragments
        {
            scan.with_fragments(fragments.clone());
        }
        scan.nearest("embedding", &Float32Array::from(vec![0.0, 0.0]), 3)
            .unwrap();
        scan.nprobes(2);
        if let Some(use_index) = case.use_index {
            scan.use_index(use_index);
        }
        if !case.prefilter
            && let Some(fragments) = &selected_fragments
        {
            scan.with_fragments(fragments.clone());
        }
        let statistics = Arc::new(Mutex::new(None));
        let collected = Arc::clone(&statistics);
        scan.scan_stats_callback(Arc::new(move |summary| {
            *collected.lock().unwrap() = Some(summary.clone());
        }));
        let batch = scan.try_into_batch().await.unwrap();
        let statistics = statistics
            .lock()
            .unwrap()
            .as_ref()
            .map(scan_statistics_from_native)
            .unwrap();
        let values = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        let mut ids = (0..values.len())
            .map(|row| values.value(row))
            .collect::<Vec<_>>();
        ids.sort_unstable();
        (ids, statistics)
    })
}

#[test]
fn fast_search_uses_only_indexed_fragments_and_matches_direct_native_queries() {
    let (_tmp, uri) = fast_search_fixture();
    let uri_c = c(&uri);
    let cases = [
        (
            "ordinary",
            FastSearchCase {
                fast_search: false,
                use_index: None,
                filter: None,
                prefilter: false,
                row_ids: None,
                fragment_ids: None,
            },
            vec![64, 100, 101],
        ),
        (
            "fast",
            FastSearchCase {
                fast_search: true,
                use_index: None,
                filter: None,
                prefilter: false,
                row_ids: None,
                fragment_ids: None,
            },
            vec![100, 101, 102],
        ),
        (
            "flat-explicit",
            FastSearchCase {
                fast_search: false,
                use_index: Some(false),
                filter: None,
                prefilter: false,
                row_ids: None,
                fragment_ids: None,
            },
            vec![64, 100, 101],
        ),
        (
            "fast-without-index",
            FastSearchCase {
                fast_search: true,
                use_index: Some(false),
                filter: None,
                prefilter: false,
                row_ids: None,
                fragment_ids: None,
            },
            Vec::new(),
        ),
        (
            "fast-reset",
            FastSearchCase {
                fast_search: false,
                use_index: None,
                filter: None,
                prefilter: false,
                row_ids: None,
                fragment_ids: None,
            },
            vec![64, 100, 101],
        ),
        (
            "row-mask-ordinary",
            FastSearchCase {
                fast_search: false,
                use_index: None,
                filter: None,
                prefilter: false,
                row_ids: Some(ROW_IDS),
                fragment_ids: None,
            },
            vec![64, 100, 101],
        ),
        (
            "row-mask-fast",
            FastSearchCase {
                fast_search: true,
                use_index: None,
                filter: None,
                prefilter: false,
                row_ids: Some(ROW_IDS),
                fragment_ids: None,
            },
            vec![100, 101],
        ),
        (
            "prefilter-ordinary",
            FastSearchCase {
                fast_search: false,
                use_index: None,
                filter: Some("category = 'hit'"),
                prefilter: true,
                row_ids: None,
                fragment_ids: None,
            },
            vec![64],
        ),
        (
            "prefilter-fast",
            FastSearchCase {
                fast_search: true,
                use_index: None,
                filter: Some("category = 'hit'"),
                prefilter: true,
                row_ids: None,
                fragment_ids: None,
            },
            Vec::new(),
        ),
        (
            "unindexed-fragment-ordinary",
            FastSearchCase {
                fast_search: false,
                use_index: None,
                filter: None,
                prefilter: false,
                row_ids: None,
                fragment_ids: Some(UNINDEXED_FRAGMENT),
            },
            vec![64],
        ),
        (
            "unindexed-fragment-fast",
            FastSearchCase {
                fast_search: true,
                use_index: None,
                filter: None,
                prefilter: false,
                row_ids: None,
                fragment_ids: Some(UNINDEXED_FRAGMENT),
            },
            Vec::new(),
        ),
        (
            "indexed-fragment-fast",
            FastSearchCase {
                fast_search: true,
                use_index: None,
                filter: None,
                prefilter: false,
                row_ids: None,
                fragment_ids: Some(INDEXED_FRAGMENT),
            },
            vec![100, 101, 102],
        ),
    ];

    for (name, case, expected) in cases {
        let (wrapper_ids, wrapper_statistics) = c_fast_search(&uri_c, case);
        let (native_ids, native_statistics) = direct_fast_search(&uri, case);
        assert_eq!(wrapper_ids, expected, "{name}");
        assert_eq!(native_ids, expected, "{name}");
        assert_eq!(
            wrapper_statistics, native_statistics,
            "{name} statistics differ"
        );
        assert_eq!(wrapper_statistics.calls, 1, "{name}");
        if case.fast_search {
            if expected.is_empty() {
                assert_eq!(wrapper_statistics.indices_loaded, 0, "{name}");
            } else {
                assert!(wrapper_statistics.indices_loaded > 0, "{name}");
                assert!(wrapper_statistics.index_partitions_loaded > 0, "{name}");
            }
        }
    }
}

#[test]
fn fast_search_without_any_index_propagates_native_empty_result() {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("no-index.lance");
    let item = Arc::new(Field::new("item", DataType::Float32, false));
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("embedding", DataType::FixedSizeList(item.clone(), 2), false),
    ]));
    let ids = (0..16).collect::<Vec<_>>();
    let values = Float32Array::from_iter_values(ids.iter().flat_map(|id| [*id as f32, 0.0]));
    let embeddings = FixedSizeListArray::try_new(item, 2, Arc::new(values), None).unwrap();
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from(ids)) as Arc<dyn Array>,
            Arc::new(embeddings) as Arc<dyn Array>,
        ],
    )
    .unwrap();
    lance_c::runtime::block_on(Dataset::write(
        arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
        uri.to_str().unwrap(),
        None,
    ))
    .unwrap();
    let uri = uri.to_str().unwrap().to_owned();
    let uri_c = c(&uri);

    for (fast_search, expected) in [(false, vec![0, 1, 2]), (true, Vec::new())] {
        let case = FastSearchCase {
            fast_search,
            use_index: None,
            filter: None,
            prefilter: false,
            row_ids: None,
            fragment_ids: None,
        };
        let (wrapper_ids, wrapper_statistics) = c_fast_search(&uri_c, case);
        let (native_ids, native_statistics) = direct_fast_search(&uri, case);
        assert_eq!(wrapper_ids, expected);
        assert_eq!(native_ids, expected);
        assert_eq!(wrapper_statistics, native_statistics);
        if fast_search {
            assert_eq!(wrapper_statistics.indices_loaded, 0);
        }
    }
}
