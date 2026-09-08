// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Segment-scoped candidate generation for ordinary scans. The explicit fragment
//! list is the read domain, including on fallback; a segment is only an accelerator.

use std::collections::HashSet;
use std::sync::Arc;
use std::time::Instant;

use datafusion::physical_plan::metrics::ExecutionPlanMetricsSet;
use lance::Dataset;
use lance::dataset::scanner::{
    ExecutionStatsCallback, ExecutionSummaryCounts, RowAddrMask, Scanner,
};
use lance::index::{DatasetIndexExt, DatasetIndexInternalExt};
use lance::io::exec::utils::IndexMetrics;
use lance_core::{Error, Result};
use lance_datafusion::planner::Planner;
use lance_datafusion::utils::MetricsExt;
use lance_index::IndexType;
use lance_index::scalar::SearchResult;
use lance_index::scalar::expression::{PlannerIndexExt, ScalarIndexExpr, ScalarIndexSearch};
use uuid::Uuid;

pub(crate) struct PreparedScalarSegment {
    pub dataset: Arc<Dataset>,
    pub segment_uuid: Uuid,
    pub fragment_ids: Vec<u64>,
    pub callback: Option<ExecutionStatsCallback>,
}

fn invalid(message: impl Into<String>) -> Error {
    Error::invalid_input_source(message.into().into())
}

// Only descend through AND: a leaf below OR or NOT need not contain all matches
// of the full expression. The original expression is always reapplied by reader.
fn driver<'a>(expr: &'a ScalarIndexExpr, index_name: &str) -> Option<&'a ScalarIndexSearch> {
    match expr {
        ScalarIndexExpr::Query(search) if search.index_name == index_name => Some(search),
        ScalarIndexExpr::And(lhs, rhs) => {
            driver(lhs, index_name).or_else(|| driver(rhs, index_name))
        }
        _ => None,
    }
}

impl PreparedScalarSegment {
    pub async fn configure(self, mut reader: Scanner) -> Result<Scanner> {
        // Never let either candidate reads or fallback re-enter a global index search.
        reader.use_scalar_index(false);
        let mut stats = ExecutionSummaryCounts::default();
        stats
            .all_counts
            .insert("scalar_segments_requested".into(), 1);
        let plan_metrics = ExecutionPlanMetricsSet::new();
        let metrics = IndexMetrics::new(&plan_metrics, 0);
        let started = Instant::now();
        let reason = self
            .configure_candidates(&mut reader, &metrics, &mut stats)
            .await?;
        metrics.flush_io();
        stats.all_times.insert(
            "scalar_segment_prepare_time".into(),
            started.elapsed().as_nanos().min(usize::MAX as u128) as usize,
        );
        if let Some(reason) = reason {
            stats
                .all_counts
                .insert("scalar_segment_fallbacks".into(), 1);
            stats
                .all_counts
                .insert(format!("scalar_segment_fallback_{reason}"), 1);
        }
        for (name, count) in plan_metrics.clone_inner().iter_counts() {
            let name = name.as_ref();
            match name {
                "iops" => stats.iops += count.value(),
                "requests" => stats.requests += count.value(),
                "bytes_read" => stats.bytes_read += count.value(),
                "indices_loaded" => stats.indices_loaded += count.value(),
                "parts_loaded" => stats.parts_loaded += count.value(),
                "index_comparisons" => stats.index_comparisons += count.value(),
                _ => *stats.all_counts.entry(name.to_string()).or_default() += count.value(),
            }
        }
        if let Some(callback) = self.callback {
            // Preserve the callback's once-per-successfully-exhausted-stream contract.
            // Candidate work is not part of the underlying reader's plan metrics.
            reader.scan_stats_callback(Arc::new(move |read| {
                let mut combined = read.clone();
                combined.iops += stats.iops;
                combined.requests += stats.requests;
                combined.bytes_read += stats.bytes_read;
                combined.indices_loaded += stats.indices_loaded;
                combined.parts_loaded += stats.parts_loaded;
                combined.index_comparisons += stats.index_comparisons;
                for (name, value) in &stats.all_counts {
                    *combined.all_counts.entry(name.clone()).or_default() += value;
                }
                for (name, value) in &stats.all_times {
                    *combined.all_times.entry(name.clone()).or_default() += value;
                }
                callback(&combined);
            }));
        }
        Ok(reader)
    }

    async fn configure_candidates(
        &self,
        reader: &mut Scanner,
        metrics: &IndexMetrics,
        stats: &mut ExecutionSummaryCounts,
    ) -> Result<Option<&'static str>> {
        let fragments = self.dataset.get_fragments();
        let visible: HashSet<u64> = fragments.iter().map(|f| f.id() as u64).collect();
        if self.fragment_ids.iter().any(|id| !visible.contains(id)) {
            return Err(invalid(
                "scalar segment fragment_ids contains a fragment absent from the dataset snapshot",
            ));
        }
        let indices = self.dataset.load_indices().await?;
        let index_meta = indices
            .iter()
            .find(|i| i.uuid == self.segment_uuid)
            .ok_or_else(|| {
                invalid(format!(
                    "scalar index segment {} is absent from the dataset snapshot",
                    self.segment_uuid
                ))
            })?;
        let field_id = index_meta
            .keyed_field()
            .ok_or_else(|| invalid("scalar segment must index a single key field"))?;
        let field =
            self.dataset.schema().field_by_id(field_id).ok_or_else(|| {
                invalid("scalar segment key field is absent from the dataset schema")
            })?;
        // Match Lance's plain-scan external-mask restriction. Keep the scoped,
        // full-filtered reader intact and avoid index work on legacy storage.
        if self.dataset.manifest().data_storage_format.lance_file_format()
            == lance_file::version::ConcreteFileVersion::V1
        {
            return Ok(Some("legacy_storage"));
        }
        // Keep V1 to flat scalar fields. A dotted name is not sufficient to prove
        // the field path of an evolved or nested schema.
        if !self
            .dataset
            .schema()
            .fields
            .iter()
            .any(|f| f.id == field.id)
        {
            return Ok(Some("nested_field"));
        }
        let scope: HashSet<u64> = self.fragment_ids.iter().copied().collect();
        let Some(coverage) = index_meta.fragment_bitmap.as_ref() else {
            return Ok(Some("unknown_coverage"));
        };
        if self
            .fragment_ids
            .iter()
            .any(|id| u32::try_from(*id).map_or(true, |id| !coverage.contains(id)))
        {
            // Scan the ENTIRE explicit read domain, not just the covered part.
            return Ok(Some("partial_coverage"));
        }
        if fragments
            .iter()
            .filter(|f| scope.contains(&(f.id() as u64)))
            .any(|f| !f.metadata().overlays.is_empty() || f.metadata().physical_rows.is_none())
        {
            return Ok(Some("fragment_state"));
        }
        // Fragment reuse can change the domain of an old segment. Until its
        // coverage mapping is handled here, preserve correctness with a scoped scan.
        if self.dataset.frag_reuse_index_uuid().await.is_some() {
            return Ok(Some("fragment_reuse"));
        }
        let Some(filter) = reader.get_expr_filter()? else {
            return Ok(Some("no_filter"));
        };
        let planner = Planner::new(Arc::new(self.dataset.schema().into()));
        let index_info = self.dataset.scalar_index_info().await?;
        let filter_plan = planner.create_filter_plan(filter, &index_info, true)?;
        let Some(search) = filter_plan
            .index_query
            .as_ref()
            .and_then(|expr| driver(expr, &index_meta.name))
        else {
            return Ok(Some("no_driver"));
        };
        if search.column != field.name {
            return Ok(Some("field_path"));
        }
        let index = self
            .dataset
            .open_scalar_index(&search.column, &self.segment_uuid, metrics)
            .await?;
        if !matches!(index.index_type(), IndexType::BTree | IndexType::Bitmap) {
            return Ok(Some("index_type"));
        }
        // External masks use _rowid, not necessarily physical row addresses.
        if index.results_are_row_addresses() && self.dataset.manifest.uses_stable_row_ids() {
            return Ok(Some("row_id_domain"));
        }
        let started = Instant::now();
        let result = index.search(search.query.as_ref(), metrics).await?;
        stats.all_times.insert(
            "scalar_segment_search_time".into(),
            started.elapsed().as_nanos().min(usize::MAX as u128) as usize,
        );
        stats
            .all_counts
            .insert("scalar_segments_searched".into(), 1);
        let SearchResult::Exact(rows) = result else {
            return Ok(Some("inexact_result"));
        };
        stats.all_counts.insert(
            "scalar_segment_candidate_rows".into(),
            rows.len().unwrap_or(0) as usize,
        );
        // Do not truncate candidates at LIMIT. The reader evaluates the complete
        // filter before applying its existing limit/offset operators.
        reader.with_row_addr_prefilter(RowAddrMask::from_allowed(rows.selected_rows().clone()));
        Ok(None)
    }
}
