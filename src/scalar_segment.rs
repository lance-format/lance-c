// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Segment-scoped candidate generation for ordinary scans. The explicit fragment
//! list is the read domain, including on fallback; a segment is only an accelerator.

use std::collections::HashSet;
use std::sync::Arc;
use std::time::Instant;

use datafusion::common::tree_node::TreeNode;
use datafusion::logical_expr::Expr;
use datafusion::physical_plan::metrics::ExecutionPlanMetricsSet;
use lance::Dataset;
use lance::dataset::scanner::{ExecutionStatsCallback, ExecutionSummaryCounts, Scanner};
use lance::index::{DatasetIndexExt, DatasetIndexInternalExt};
use lance::io::exec::utils::IndexMetrics;
use lance_core::{Error, Result};
use lance_datafusion::planner::Planner;
use lance_datafusion::utils::MetricsExt;
use lance_index::IndexType;
use lance_index::scalar::expression::{PlannerIndexExt, ScalarIndexExpr, ScalarIndexLoader};
use lance_index::scalar::{MetricsCollector, ScalarIndex};
use uuid::Uuid;

pub(crate) struct PreparedScalarSegment {
    pub dataset: Arc<Dataset>,
    pub segment_uuid: Uuid,
    pub auto_selection: bool,
    pub fragment_ids: Vec<u64>,
    pub use_scalar_index: bool,
    pub callback: Option<ExecutionStatsCallback>,
}

fn invalid(message: impl Into<String>) -> Error {
    Error::invalid_input_source(message.into().into())
}

// Bound both recursive planning and the concurrent searches in Lance's evaluator.
const MAX_EXPRESSION_NODES: usize = 128;
const MAX_EXPRESSION_DEPTH: usize = 32;

pub(crate) fn scoped_expression(
    expr: &ScalarIndexExpr,
    index_name: &str,
    column: &str,
    require_complete: bool,
    depth: usize,
    remaining: &mut usize,
) -> std::result::Result<Option<ScalarIndexExpr>, &'static str> {
    if depth > MAX_EXPRESSION_DEPTH || *remaining == 0 {
        return Err("expression_budget");
    }
    *remaining -= 1;
    let recurse = |child, complete, remaining: &mut usize| {
        scoped_expression(child, index_name, column, complete, depth + 1, remaining)
    };
    match expr {
        ScalarIndexExpr::Query(search) => {
            if search.index_name != index_name {
                Ok(None)
            } else if search.column != column {
                Err("field_path")
            } else {
                Ok(Some(expr.clone()))
            }
        }
        ScalarIndexExpr::And(lhs, rhs) => {
            let lhs = recurse(lhs, require_complete, remaining)?;
            let rhs = recurse(rhs, require_complete, remaining)?;
            Ok(match (lhs, rhs) {
                (Some(lhs), Some(rhs)) => Some(ScalarIndexExpr::And(Box::new(lhs), Box::new(rhs))),
                (lhs, rhs) if !require_complete => lhs.or(rhs),
                _ => None,
            })
        }
        ScalarIndexExpr::Or(lhs, rhs) => {
            let lhs = recurse(lhs, require_complete, remaining)?;
            let rhs = recurse(rhs, require_complete, remaining)?;
            // Both branches must contribute a superset of their matches. Dropping
            // an unavailable OR branch would silently exclude valid rows.
            Ok(lhs
                .zip(rhs)
                .map(|(lhs, rhs)| ScalarIndexExpr::Or(Box::new(lhs), Box::new(rhs))))
        }
        ScalarIndexExpr::Not(inner) => {
            // Negating a pruned AND would turn a safe superset into an unsafe
            // subset. Preserve the complete subtree and let Lance track NULLs.
            Ok(recurse(inner, true, remaining)?.map(|inner| ScalarIndexExpr::Not(Box::new(inner))))
        }
    }
}

struct SegmentIndexLoader<'a> {
    index: Arc<dyn ScalarIndex>,
    index_name: &'a str,
    column: &'a str,
}

#[async_trait::async_trait]
impl ScalarIndexLoader for SegmentIndexLoader<'_> {
    async fn load_index(
        &self,
        column: &str,
        index_name: &str,
        _metrics: &dyn MetricsCollector,
    ) -> Result<Arc<dyn ScalarIndex>> {
        if column != self.column || index_name != self.index_name {
            return Err(invalid(
                "scalar expression references an index outside the selected segment",
            ));
        }
        // Reuse the one physical segment; a dataset loader would search every
        // segment of the logical index and violate the caller's restriction.
        Ok(self.index.clone())
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
        // Explicitly disabling scalar indices also disables this accelerator.
        // Keep snapshot validation above, but do not plan, open or search an index.
        if !self.use_scalar_index {
            return Ok(Some("disabled"));
        }
        // Match Lance's plain-scan external-mask restriction. Keep the scoped,
        // full-filtered reader intact and avoid index work on legacy storage.
        if self
            .dataset
            .manifest()
            .data_storage_format
            .lance_file_format()
            == lance_file::version::ConcreteFileVersion::V1
        {
            return Ok(Some("legacy_storage"));
        }
        // Keep this implementation to flat scalar fields. A dotted name cannot
        // prove the field path of an evolved or nested schema.
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
        // The pinned Lance planner lowers IS TRUE/FALSE to nullable equality.
        // Under negation this loses matching NULL rows before the full recheck.
        // Inspect the original expression before planning erases that distinction;
        // remove this fallback only after adopting lance-format/lance#9568.
        if filter.exists(|expr| {
            Ok(matches!(
                expr,
                Expr::IsTrue(_) | Expr::IsFalse(_) | Expr::IsNotTrue(_) | Expr::IsNotFalse(_)
            ))
        })? {
            return Ok(Some("boolean_truth_test"));
        }
        let stored_schema: arrow_schema::Schema = self.dataset.schema().into();
        // get_expr_filter validates against the scanner's full filterable
        // schema, including metadata columns absent from the stored schema.
        // Keep the scoped reader's complete filter instead of replanning such
        // expressions against a narrower schema or dropping their residuals.
        if filter
            .column_refs()
            .iter()
            .any(|column| stored_schema.field_with_name(&column.name).is_err())
        {
            return Ok(Some("filter_schema"));
        }
        let planner = Planner::new(Arc::new(stored_schema));
        let index_info = self.dataset.scalar_index_info().await?;
        let filter_plan = planner.create_filter_plan(filter, &index_info, true)?;
        let Some(expr) = filter_plan.index_query.as_ref() else {
            return Ok(Some("no_driver"));
        };
        let mut remaining = MAX_EXPRESSION_NODES;
        let expr = match scoped_expression(
            expr,
            &index_meta.name,
            &field.name,
            false,
            0,
            &mut remaining,
        ) {
            Ok(Some(expr)) => expr,
            Ok(None) => return Ok(Some("no_driver")),
            Err(reason) => return Ok(Some(reason)),
        };
        let (segment_uuid, expr) = if self.auto_selection {
            let physical_rows = fragments
                .iter()
                .filter(|f| scope.contains(&(f.id() as u64)))
                .fold(0_u64, |sum, f| {
                    sum.saturating_add(f.metadata().physical_rows.unwrap_or(0) as u64)
                });
            let Some(selected) = crate::scalar_index_selection::choose(
                &self.dataset,
                &indices,
                index_meta,
                expr,
                filter_plan.index_query.as_ref().unwrap(),
                crate::scalar_index_selection::ReadDomain {
                    fragment_ids: &self.fragment_ids,
                    physical_rows,
                },
                stats,
            )
            .await
            else {
                return Ok(Some("runtime_cost"));
            };
            selected
        } else {
            (self.segment_uuid, expr)
        };
        let index_meta = indices
            .iter()
            .find(|i| i.uuid == segment_uuid)
            .ok_or_else(|| invalid("runtime scalar index is absent from the dataset snapshot"))?;
        let field = self
            .dataset
            .schema()
            .field_by_id(index_meta.keyed_field().unwrap())
            .ok_or_else(|| {
                invalid("runtime scalar index field is absent from the dataset schema")
            })?;
        let index = self
            .dataset
            .open_scalar_index(&field.name, &segment_uuid, metrics)
            .await?;
        // These implementations can return exact candidates. Keep the runtime
        // Exact check below: a type alone is not a guarantee for every query.
        if !matches!(
            index.index_type(),
            IndexType::BTree | IndexType::Bitmap | IndexType::LabelList
        ) {
            return Ok(Some("index_type"));
        }
        // External masks use _rowid, not necessarily physical row addresses.
        if index.results_are_row_addresses() && self.dataset.manifest.uses_stable_row_ids() {
            return Ok(Some("row_id_domain"));
        }
        let started = Instant::now();
        let loader = SegmentIndexLoader {
            index,
            index_name: &index_meta.name,
            column: &field.name,
        };
        let result = expr.evaluate(&loader, metrics).await?;
        stats.all_times.insert(
            "scalar_segment_search_time".into(),
            started.elapsed().as_nanos().min(usize::MAX as u128) as usize,
        );
        stats
            .all_counts
            .insert("scalar_segments_searched".into(), 1);
        if !result.is_exact() {
            return Ok(Some("inexact_result"));
        }
        if let Some(rows) = result.upper.max_len() {
            stats
                .all_counts
                .insert("scalar_segment_candidate_rows".into(), rows as usize);
        } else {
            // A complement mask has no finite cardinality without a row universe.
            // Do not report zero candidates for a successful NOT search.
            stats
                .all_counts
                .insert("scalar_segment_candidate_rows_unknown".into(), 1);
        }
        // Do not truncate candidates at LIMIT. The reader evaluates the complete
        // filter before applying its existing limit/offset operators. Its explicit
        // fragment domain also bounds complement masks produced by NOT.
        reader.with_row_addr_prefilter(result.upper);
        Ok(None)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use lance_index::scalar::SargableQuery;
    use lance_index::scalar::expression::ScalarIndexSearch;

    fn leaf(index_name: &str) -> ScalarIndexExpr {
        ScalarIndexExpr::Query(ScalarIndexSearch {
            column: "key".into(),
            index_name: index_name.into(),
            index_type: "BTree".into(),
            query: Arc::new(SargableQuery::IsNull()),
            needs_recheck: false,
            fragment_bitmap: None,
        })
    }

    fn select(
        expr: &ScalarIndexExpr,
    ) -> std::result::Result<Option<ScalarIndexExpr>, &'static str> {
        let mut remaining = MAX_EXPRESSION_NODES;
        scoped_expression(expr, "selected", "key", false, 0, &mut remaining)
    }

    #[test]
    fn never_negate_a_partial_candidate_expression() {
        let partial = ScalarIndexExpr::And(Box::new(leaf("selected")), Box::new(leaf("other")));
        assert!(select(&partial).unwrap().is_some());
        let negated = ScalarIndexExpr::Not(Box::new(partial));
        assert!(select(&negated).unwrap().is_none());
        let alternative = ScalarIndexExpr::Or(Box::new(leaf("selected")), Box::new(negated));
        assert!(select(&alternative).unwrap().is_none());
    }

    #[test]
    fn expression_budget_bounds_depth_and_concurrent_searches() {
        let mut deep = leaf("selected");
        for _ in 0..=MAX_EXPRESSION_DEPTH {
            deep = ScalarIndexExpr::Not(Box::new(deep));
        }
        assert_eq!(select(&deep).unwrap_err(), "expression_budget");
        let mut wide = leaf("selected");
        for _ in 0..6 {
            wide = ScalarIndexExpr::Or(Box::new(wide.clone()), Box::new(wide));
        }
        assert!(select(&wide).unwrap().is_some());
        wide = ScalarIndexExpr::Or(Box::new(wide.clone()), Box::new(wide));
        assert_eq!(select(&wide).unwrap_err(), "expression_budget");
    }
}
