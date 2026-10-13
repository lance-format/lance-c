// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Row address allowlist / blocklist prefilters (`lance_scanner_set_row_addr_*`).

use std::collections::BTreeMap;
use std::ffi::{CStr, CString};
use std::ptr;
use std::sync::Arc;

use arrow::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use arrow::record_batch::RecordBatchIterator;
use arrow_array::{Array, FixedSizeListArray, Float32Array, Int32Array, RecordBatch, StringArray};
use arrow_schema::{DataType, Field, Schema};
use lance::Dataset;
use lance::dataset::{WriteMode, WriteParams};
use lance_c::*;

const TEXTS: [&str; 10] = [
    "apple banana",
    "apple",
    "banana cherry",
    "cherry",
    "apple cherry",
    "apple banana cherry",
    "banana",
    "apple apple",
    "durian",
    "banana apple",
];

fn c(s: &str) -> CString {
    CString::new(s).unwrap()
}

fn last_error() -> String {
    unsafe {
        let msg = lance_last_error_message();
        let message = CStr::from_ptr(msg).to_string_lossy().into_owned();
        lance_free_string(msg);
        message
    }
}

fn ok(code: i32) {
    assert_eq!(code, 0, "{}", last_error());
}

/// Two fragments of five rows with stable row ids, so `_rowid` (== `id`, 0..9)
/// differs from the row address of every row in the second fragment. Each row
/// has a text column with an inverted index and a 2-d vector `[id, 1]`.
fn fixture() -> (tempfile::TempDir, *mut LanceDataset) {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("row_addr_prefilter.lance");
    lance_c::runtime::block_on(async {
        for fragment in 0..2 {
            let ids: Vec<i32> = (fragment * 5..(fragment + 1) * 5).collect();
            let texts: Vec<&str> = ids.iter().map(|id| TEXTS[*id as usize]).collect();
            let vectors = FixedSizeListArray::try_new(
                Arc::new(Field::new("item", DataType::Float32, false)),
                2,
                Arc::new(Float32Array::from(
                    ids.iter()
                        .flat_map(|id| [*id as f32, 1.])
                        .collect::<Vec<_>>(),
                )),
                None,
            )
            .unwrap();
            let schema = Arc::new(Schema::new(vec![
                Field::new("id", DataType::Int32, false),
                Field::new("text", DataType::Utf8, false),
                Field::new("embedding", vectors.data_type().clone(), false),
            ]));
            let batch = RecordBatch::try_new(
                schema.clone(),
                vec![
                    Arc::new(Int32Array::from(ids)),
                    Arc::new(StringArray::from(texts)),
                    Arc::new(vectors),
                ],
            )
            .unwrap();
            Dataset::write(
                RecordBatchIterator::new(vec![Ok(batch)], schema),
                path.to_str().unwrap(),
                Some(WriteParams {
                    mode: if fragment == 0 {
                        WriteMode::Create
                    } else {
                        WriteMode::Append
                    },
                    enable_stable_row_ids: true,
                    ..Default::default()
                }),
            )
            .await
            .unwrap();
        }
    });

    let uri = c(path.to_str().unwrap());
    let dataset = unsafe { lance_dataset_open(uri.as_ptr(), ptr::null(), 0) };
    assert!(!dataset.is_null(), "{}", last_error());
    let column = c("text");
    let params = c(r#"{"base_tokenizer":"simple","language":"English"}"#);
    ok(unsafe {
        lance_dataset_create_scalar_index(
            dataset,
            column.as_ptr(),
            ptr::null(),
            LanceScalarIndexType::Inverted as i32,
            params.as_ptr(),
            false,
        )
    });
    (dir, dataset)
}

struct Scan(*mut LanceScanner);

impl Scan {
    fn new(dataset: *const LanceDataset, sql_filter: Option<&str>) -> Self {
        let filter = sql_filter.map(c);
        let scanner = unsafe {
            lance_scanner_new(
                dataset,
                ptr::null(),
                filter.as_ref().map_or(ptr::null(), |f| f.as_ptr()),
            )
        };
        assert!(!scanner.is_null(), "{}", last_error());
        Self(scanner)
    }

    fn allow(self, row_ids: &[u64]) -> Self {
        ok(unsafe {
            lance_scanner_set_row_addr_allowlist(self.0, row_ids.as_ptr(), row_ids.len())
        });
        self
    }

    fn block(self, row_ids: &[u64]) -> Self {
        ok(unsafe {
            lance_scanner_set_row_addr_blocklist(self.0, row_ids.as_ptr(), row_ids.len())
        });
        self
    }

    /// `id` → the named score column (`_score` / `_distance`), or 0 when absent.
    fn collect(self, score_column: Option<&str>) -> BTreeMap<i32, f32> {
        let mut stream = FFI_ArrowArrayStream::empty();
        ok(unsafe { lance_scanner_to_arrow_stream(self.0, &mut stream) });
        let reader = unsafe { ArrowArrayStreamReader::from_raw(&mut stream).unwrap() };
        let mut out = BTreeMap::new();
        for batch in reader {
            let batch = batch.unwrap();
            let ids = batch
                .column_by_name("id")
                .unwrap()
                .as_any()
                .downcast_ref::<Int32Array>()
                .unwrap();
            let scores = score_column.map(|name| {
                batch
                    .column_by_name(name)
                    .unwrap()
                    .as_any()
                    .downcast_ref::<Float32Array>()
                    .unwrap()
                    .clone()
            });
            for row in 0..batch.num_rows() {
                let score = scores.as_ref().map_or(0.0, |s| s.value(row));
                assert!(out.insert(ids.value(row), score).is_none());
            }
        }
        out
    }
}

impl Drop for Scan {
    fn drop(&mut self) {
        unsafe { lance_scanner_close(self.0) }
    }
}

fn ids(result: &BTreeMap<i32, f32>) -> Vec<i32> {
    result.keys().copied().collect()
}

/// `full` restricted to `allowed`: what a masked search must return, scores unchanged.
fn restrict(full: &BTreeMap<i32, f32>, allowed: &[u64]) -> BTreeMap<i32, f32> {
    full.iter()
        .filter(|(id, _)| allowed.contains(&(**id as u64)))
        .map(|(id, score)| (*id, *score))
        .collect()
}

#[test]
fn test_row_addr_lists_restrict_plain_scan() {
    let (_dir, dataset) = fixture();

    // Ids 5 and 8 live in the second fragment, so this also checks the lists are
    // keyed by stable row id rather than row address.
    let all = |scan: Scan| ids(&scan.collect(None));
    assert_eq!(
        all(Scan::new(dataset, None).allow(&[8, 1, 5])),
        vec![1, 5, 8]
    );
    assert_eq!(
        all(Scan::new(dataset, None).block(&[0, 1, 2, 3, 4])),
        vec![5, 6, 7, 8, 9]
    );
    assert_eq!(
        all(Scan::new(dataset, None).allow(&[1, 5, 8]).block(&[5])),
        vec![1, 8]
    );
    assert!(all(Scan::new(dataset, None).allow(&[])).is_empty());
    assert_eq!(all(Scan::new(dataset, None).block(&[])).len(), 10);
    // A later call replaces the earlier list.
    assert_eq!(
        all(Scan::new(dataset, None).allow(&[1]).allow(&[2])),
        vec![2]
    );
    // ANDed with the filter expression.
    assert_eq!(
        all(Scan::new(dataset, Some("id > 2")).allow(&[1, 5, 8])),
        vec![5, 8]
    );

    unsafe { lance_dataset_close(dataset) };
}

#[test]
fn test_row_addr_lists_reject_invalid_inputs() {
    let (_dir, dataset) = fixture();

    let scan = Scan::new(dataset, None);
    assert_eq!(
        unsafe { lance_scanner_set_row_addr_allowlist(scan.0, ptr::null(), 1) },
        -1
    );
    assert!(last_error().contains("row_ids is NULL but len > 0"));
    assert_eq!(
        unsafe { lance_scanner_set_row_addr_blocklist(scan.0, ptr::null(), 1) },
        -1
    );
    assert!(last_error().contains("row_ids is NULL but len > 0"));
    assert_eq!(
        unsafe { lance_scanner_set_row_addr_allowlist(ptr::null_mut(), ptr::null(), 0) },
        -1
    );
    // NULL is valid for an empty list.
    ok(unsafe { lance_scanner_set_row_addr_allowlist(scan.0, ptr::null(), 0) });

    let started = Scan::new(dataset, None);
    let mut stream = FFI_ArrowArrayStream::empty();
    ok(unsafe { lance_scanner_to_arrow_stream(started.0, &mut stream) });
    drop(unsafe { ArrowArrayStreamReader::from_raw(&mut stream).unwrap() });
    assert_eq!(
        unsafe { lance_scanner_set_row_addr_allowlist(started.0, [1u64].as_ptr(), 1) },
        -1
    );
    assert!(last_error().contains("row_addr_allowlist must be set before the scan starts"));
    assert_eq!(
        unsafe { lance_scanner_set_row_addr_blocklist(started.0, [1u64].as_ptr(), 1) },
        -1
    );
    assert!(last_error().contains("row_addr_blocklist must be set before the scan starts"));

    let segment = Scan::new(dataset, Some("id > 2")).allow(&[1]);
    ok(unsafe { lance_scanner_set_scalar_index_segment(segment.0, [7u8; 16].as_ptr()) });
    let mut stream = FFI_ArrowArrayStream::empty();
    assert_eq!(
        unsafe { lance_scanner_to_arrow_stream(segment.0, &mut stream) },
        -1
    );
    assert!(last_error().contains("cannot be combined with scalar_index_segment"));

    unsafe { lance_dataset_close(dataset) };
}

#[test]
fn test_row_addr_allowlist_full_text_search_scores_allowed_rows_only() {
    let (_dir, dataset) = fixture();
    let query = c("apple banana");
    let search = |scan: Scan| {
        ok(unsafe { lance_scanner_full_text_search(scan.0, query.as_ptr(), ptr::null(), 0) });
        scan.collect(Some("_score"))
    };

    let full = search(Scan::new(dataset, None));
    assert_eq!(ids(&full), vec![0, 1, 2, 4, 5, 6, 7, 9]);
    let allowed = [0, 3, 6, 9];
    assert_eq!(
        search(Scan::new(dataset, None).allow(&allowed)),
        restrict(&full, &allowed)
    );
    assert_eq!(
        search(Scan::new(dataset, None).block(&[0, 1, 2, 3, 4])),
        restrict(&full, &[5, 6, 7, 8, 9])
    );

    unsafe { lance_dataset_close(dataset) };
}

#[test]
fn test_row_addr_allowlist_prepared_fts_scores_allowed_rows_only() {
    let (_dir, dataset) = fixture();
    let column = c("text");
    let query = c("apple banana");

    for (operator, expected_ids) in [
        (LanceFtsMatchOperator::Or, vec![0, 1, 2, 4, 5, 6, 7, 9]),
        (LanceFtsMatchOperator::And, vec![0, 5, 9]),
    ] {
        let context = unsafe {
            lance_dataset_prepare_fts_match_query(
                dataset,
                column.as_ptr(),
                query.as_ptr(),
                operator as i32,
                0,
                LanceFtsCoverageMode::Strict as i32,
            )
        };
        assert!(!context.is_null(), "{}", last_error());
        let search = |scan: Scan| {
            ok(unsafe { lance_scanner_set_fts_query_context(scan.0, context) });
            scan.collect(Some("_score"))
        };

        // The prepared plan is rebuilt around the shared global scorer; the mask must
        // survive that rewrite and leave the scores of the allowed rows unchanged.
        let full = search(Scan::new(dataset, None));
        assert_eq!(ids(&full), expected_ids);
        for allowed in [&[0u64, 5, 6][..], &[9], &[3, 8], &[]] {
            assert_eq!(
                search(Scan::new(dataset, None).allow(allowed)),
                restrict(&full, allowed)
            );
        }
        assert_eq!(
            search(Scan::new(dataset, Some("id >= 5")).allow(&[0, 5, 6])),
            restrict(&full, &[5, 6])
        );
        unsafe { lance_fts_query_context_close(context) };
    }

    unsafe { lance_dataset_close(dataset) };
}

#[test]
fn test_row_addr_allowlist_nearest_ranks_allowed_rows_only() {
    let (_dir, dataset) = fixture();
    let column = c("embedding");
    let query = [9.0f32, 1.0];
    let nearest = |scan: Scan| {
        ok(unsafe {
            lance_scanner_nearest(
                scan.0,
                column.as_ptr(),
                query.as_ptr().cast(),
                query.len(),
                LanceDataType::Float32 as i32,
                2,
            )
        });
        ids(&scan.collect(Some("_distance")))
    };

    assert_eq!(nearest(Scan::new(dataset, None)), vec![8, 9]);
    // Top-k is computed over the allowed rows, not filtered after ranking.
    assert_eq!(
        nearest(Scan::new(dataset, None).allow(&[1, 3, 6])),
        vec![3, 6]
    );

    unsafe { lance_dataset_close(dataset) };
}
