// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Process-local, immutable FTS query context shared by segment-scoped scans.

use std::collections::HashSet;
use std::ffi::c_char;
use std::ptr;
use std::sync::Arc;

use futures::future::try_join_all;
use lance::index::{DatasetIndexExt, DatasetIndexInternalExt};
use lance_core::{Error, Result};
use lance_index::IndexCriteria;
use lance_index::metrics::NoOpMetricsCollector;
use lance_index::scalar::FullTextSearchQuery;
use lance_index::scalar::inverted::query::{FtsQuery, collect_query_tokens};
use lance_index::scalar::inverted::{InvertedIndex, MemBM25Scorer, build_global_bm25_scorer};
use lance_table::format::IndexMetadata;
use uuid::Uuid;

use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
use crate::helpers;
use crate::runtime::block_on;

/// Required relationship between the pinned dataset snapshot and its FTS index.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceFtsCoverageMode {
    /// Every current fragment must be covered by a committed FTS segment.
    Strict = 0,
    /// Search and score only documents covered by committed FTS segments.
    IndexOnly = 1,
}

impl TryFrom<i32> for LanceFtsCoverageMode {
    type Error = Error;

    fn try_from(value: i32) -> Result<Self> {
        match value {
            0 => Ok(Self::Strict),
            1 => Ok(Self::IndexOnly),
            _ => Err(Error::invalid_input(format!(
                "invalid coverage_mode {value}; expected 0 (STRICT) or 1 (INDEX_ONLY)"
            ))),
        }
    }
}

/// Rust-owned immutable state behind [`LanceFtsQueryContext`].
pub(crate) struct FtsQueryContextInner {
    pub(crate) dataset: Arc<lance::Dataset>,
    pub(crate) query: FullTextSearchQuery,
    pub(crate) segments: Vec<IndexMetadata>,
    pub(crate) scorer: Arc<MemBM25Scorer>,
}

impl FtsQueryContextInner {
    pub(crate) fn validate_dataset_identity(&self, dataset: &Arc<lance::Dataset>) -> Result<()> {
        if !Arc::ptr_eq(&self.dataset, dataset) {
            return Err(invalid_input(format!(
                "FTS query context and scanner must originate from the same process-local dataset snapshot; context has uri '{}' version {}, scanner has uri '{}' version {}",
                self.dataset.uri(),
                self.dataset.version_id(),
                dataset.uri(),
                dataset.version_id()
            )));
        }
        Ok(())
    }
}

/// Opaque process-local FTS query context.
///
/// The handle owns an `Arc`, and scanners clone that `Arc` when the context is
/// attached. It is therefore safe to close the public handle after all scanner
/// attachments have completed.
pub struct LanceFtsQueryContext {
    pub(crate) inner: Arc<FtsQueryContextInner>,
}

fn invalid_input(message: impl Into<String>) -> Error {
    Error::invalid_input(message.into())
}

async fn prepare_fts_query_context(
    dataset: Arc<lance::Dataset>,
    column: String,
    query_text: String,
    coverage_mode: LanceFtsCoverageMode,
) -> Result<FtsQueryContextInner> {
    let logical_index = dataset
        .load_scalar_index(IndexCriteria::default().for_column(&column).supports_fts())
        .await?
        .ok_or_else(|| {
            invalid_input(format!(
                "no committed FTS index exists for column '{column}' in dataset version {}",
                dataset.version_id()
            ))
        })?;
    let segments = dataset.load_indices_by_name(&logical_index.name).await?;
    if segments.is_empty() {
        return Err(invalid_input(format!(
            "FTS index for column '{column}' has no committed segments in dataset version {}",
            dataset.version_id()
        )));
    }

    let expected_fields = &segments[0].fields;
    if let Some(segment) = segments
        .iter()
        .find(|segment| &segment.fields != expected_fields)
    {
        return Err(invalid_input(format!(
            "FTS index '{}' has inconsistent fields across segments; segment {} has fields {:?}, expected {:?}",
            logical_index.name, segment.uuid, segment.fields, expected_fields
        )));
    }

    let current_fragment_ids: HashSet<u32> = dataset
        .get_fragments()
        .into_iter()
        .map(|fragment| {
            u32::try_from(fragment.id()).map_err(|_| {
                invalid_input(format!(
                    "fragment id {} exceeds the u32 index metadata range",
                    fragment.id()
                ))
            })
        })
        .collect::<Result<_>>()?;

    let mut indexed_fragment_ids = HashSet::new();
    for segment in &segments {
        let fragment_bitmap = segment.fragment_bitmap.as_ref().ok_or_else(|| {
            invalid_input(format!(
                "FTS segment {} for column '{column}' has unknown fragment coverage",
                segment.uuid
            ))
        })?;
        indexed_fragment_ids.extend(
            fragment_bitmap
                .iter()
                .filter(|fragment_id| current_fragment_ids.contains(fragment_id)),
        );
    }
    let mut unindexed_fragment_ids: Vec<u32> = current_fragment_ids
        .difference(&indexed_fragment_ids)
        .copied()
        .collect();
    unindexed_fragment_ids.sort_unstable();

    if coverage_mode == LanceFtsCoverageMode::Strict && !unindexed_fragment_ids.is_empty() {
        return Err(invalid_input(format!(
            "coverage_mode=STRICT requires every fragment in dataset version {} to be indexed; column '{column}' has {} unindexed fragments: {:?}",
            dataset.version_id(),
            unindexed_fragment_ids.len(),
            unindexed_fragment_ids
        )));
    }

    let indices: Vec<Arc<InvertedIndex>> = try_join_all(segments.iter().map(|segment| {
        let dataset = Arc::clone(&dataset);
        let column = column.clone();
        async move {
            let index = dataset
                .open_scalar_index(&column, &segment.uuid, &NoOpMetricsCollector)
                .await?;
            let inverted = index
                .as_any()
                .downcast_ref::<InvertedIndex>()
                .ok_or_else(|| {
                    invalid_input(format!(
                        "index segment {} for column '{column}' is not an inverted index",
                        segment.uuid
                    ))
                })?;
            Ok::<_, Error>(Arc::new(inverted.clone()))
        }
    }))
    .await?;

    let expected_params = indices[0].params();
    if let Some((position, _)) = indices
        .iter()
        .enumerate()
        .find(|(_, index)| index.params() != expected_params)
    {
        return Err(invalid_input(format!(
            "FTS index '{}' has inconsistent inverted index parameters; segment {} differs from segment {}",
            logical_index.name, segments[position].uuid, segments[0].uuid
        )));
    }

    let query = FullTextSearchQuery::new(query_text).with_column(column.clone())?;
    let match_query = match &query.query {
        FtsQuery::Match(query) => query,
        _ => {
            return Err(Error::internal(
                "prepared FTS query unexpectedly produced a non-Match query".to_string(),
            ));
        }
    };
    let mut tokenizer = indices[0].tokenizer();
    let query_tokens = collect_query_tokens(&match_query.terms, &mut tokenizer);
    let params = query
        .params()
        .with_fuzziness(match_query.fuzziness)
        .with_max_expansions(match_query.max_expansions)
        .with_prefix_length(match_query.prefix_length);
    let scorer = Arc::new(build_global_bm25_scorer(&indices, &query_tokens, &params).await?);

    Ok(FtsQueryContextInner {
        dataset,
        query,
        segments,
        scorer,
    })
}

/// Prepare a process-local global BM25 scorer and the committed segment list
/// for one single-column Match query against the dataset's pinned snapshot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_prepare_fts_query(
    dataset: *const LanceDataset,
    column: *const c_char,
    query: *const c_char,
    max_fuzzy_distance: u32,
    coverage_mode: i32,
) -> *mut LanceFtsQueryContext {
    ffi_try!(
        unsafe {
            prepare_fts_query_inner(dataset, column, query, max_fuzzy_distance, coverage_mode)
        },
        null
    )
}

unsafe fn prepare_fts_query_inner(
    dataset: *const LanceDataset,
    column: *const c_char,
    query: *const c_char,
    max_fuzzy_distance: u32,
    coverage_mode: i32,
) -> Result<*mut LanceFtsQueryContext> {
    if dataset.is_null() || column.is_null() || query.is_null() {
        return Err(invalid_input("dataset, column, and query must not be NULL"));
    }
    let column = unsafe { helpers::parse_c_string(column)? }
        .filter(|value| !value.is_empty())
        .ok_or_else(|| invalid_input("column must not be empty"))?
        .to_string();
    let query = unsafe { helpers::parse_c_string(query)? }
        .filter(|value| !value.is_empty())
        .ok_or_else(|| invalid_input("query must not be empty"))?
        .to_string();
    let coverage_mode = LanceFtsCoverageMode::try_from(coverage_mode)?;
    if max_fuzzy_distance != 0 {
        return Err(invalid_input(format!(
            "max_fuzzy_distance must be 0 for prepared FTS query contexts, got {max_fuzzy_distance}; fuzzy queries require a canonical prepared BM25 vocabulary"
        )));
    }
    let snapshot = unsafe { &*dataset }.snapshot();
    let inner = block_on(prepare_fts_query_context(
        snapshot,
        column,
        query,
        coverage_mode,
    ))?;
    Ok(Box::into_raw(Box::new(LanceFtsQueryContext {
        inner: Arc::new(inner),
    })))
}

/// Close a context handle. NULL-safe. Scanners that already attached the
/// context retain their own shared reference.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_query_context_close(context: *mut LanceFtsQueryContext) {
    if !context.is_null() {
        swallow_unwind("lance_fts_query_context_close", || unsafe {
            drop(Box::from_raw(context));
        });
    }
}

pub(crate) unsafe fn clone_context(
    context: *const LanceFtsQueryContext,
) -> Result<Arc<FtsQueryContextInner>> {
    if context.is_null() {
        return Err(invalid_input("context must not be NULL"));
    }
    Ok(Arc::clone(&unsafe { &*context }.inner))
}

pub(crate) fn parse_segment_uuids(segment_uuids: *const u8, len: usize) -> Result<Vec<Uuid>> {
    if segment_uuids.is_null() && len > 0 {
        return Err(invalid_input(
            "segment_uuids is NULL but len is greater than 0",
        ));
    }
    if len > isize::MAX as usize / 16 {
        return Err(invalid_input(format!(
            "segment UUID count {len} exceeds the maximum addressable byte slice length"
        )));
    }
    let mut uuids = Vec::with_capacity(len);
    for position in 0..len {
        let mut bytes = [0_u8; 16];
        unsafe {
            ptr::copy_nonoverlapping(segment_uuids.add(position * 16), bytes.as_mut_ptr(), 16);
        }
        uuids.push(Uuid::from_bytes(bytes));
    }
    let unique: HashSet<Uuid> = uuids.iter().copied().collect();
    if unique.len() != uuids.len() {
        return Err(invalid_input(format!(
            "segment_uuids contains duplicate UUIDs; len={}, unique={}",
            uuids.len(),
            unique.len()
        )));
    }
    Ok(uuids)
}
