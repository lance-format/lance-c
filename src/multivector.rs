// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Correct multi-vector scoring before the pinned Lance plan's candidate limits.

use std::collections::HashMap;
use std::sync::Arc;

use arrow_array::types::{Float16Type, Float32Type, Float64Type};
use arrow_array::{
    Array, ArrayRef, ArrowPrimitiveType, BooleanArray, FixedSizeListArray, Float32Array, ListArray,
    RecordBatch, UInt64Array,
};
use arrow_schema::{DataType, SchemaRef};
use datafusion::common::tree_node::TreeNodeRecursion;
use datafusion::error::{DataFusionError, Result};
use datafusion::execution::context::TaskContext;
use datafusion::physical_plan::{
    ChildrenPropertiesMode, DisplayAs, DisplayFormatType, ExecutionPlan,
    InputDistributionRequirements, PhysicalExpr, PlanProperties, ReplaceChildrenOptions,
    SendableRecordBatchStream, replace_children_if_necessary, stream::RecordBatchStreamAdapter,
};
use futures::{StreamExt, TryStreamExt, stream};
use lance::io::exec::KNNVectorDistanceExec;
use lance_linalg::distance::{Cosine, DistanceType, Dot, L2};

// Lance creates one ANN branch per query vector,
// each overfetching 10 * k candidates before scoring; wire bytes alone cannot bound this work.
pub(crate) const MAX_QUERY_VECTORS: usize = 128;
pub(crate) const MAX_QUERY_VECTOR_CANDIDATES: usize = 100_000;

fn invalid(message: impl Into<String>) -> DataFusionError {
    DataFusionError::Execution(message.into())
}

/// Rewrite inside TopK/refinement, before any score can discard a candidate.
pub(crate) fn rewrite(plan: Arc<dyn ExecutionPlan>) -> Result<Arc<dyn ExecutionPlan>> {
    let children = plan
        .children()
        .into_iter()
        .map(|child| rewrite(child.clone()))
        .collect::<Result<Vec<_>>>()?;
    let plan = if children.is_empty() {
        plan
    } else {
        replace_children_if_necessary(plan, children)?
    };
    let mode = if let Some(exact) = plan.downcast_ref::<KNNVectorDistanceExec>() {
        if exact.is_batch {
            return Err(invalid(
                "expected one logical multi-vector query, not batch queries",
            ));
        }
        Some(Scoring::Exact {
            query: exact.query.clone(),
            column: exact.column.clone(),
            metric: exact.distance_type,
        })
    // This pinned Lance node is not publicly re-exported, so match its stable plan name.
    } else if plan.name() == "MultivectorScoringExec" {
        Some(Scoring::Indexed)
    } else {
        None
    };
    Ok(match mode {
        Some(mode) => Arc::new(MultiVectorScoreExec {
            original: plan,
            mode,
        }),
        None => plan,
    })
}

/// Apply the final distance-ordered window without invalidating output batching.
/// `distance_column` carries the native distance even when the caller's
/// projection renames or transforms the visible `_distance` column.
pub(crate) fn apply_result_window(
    plan: Arc<dyn ExecutionPlan>,
    offset: usize,
    limit: Option<usize>,
    distance_column: &str,
) -> Result<Arc<dyn ExecutionPlan>> {
    use datafusion::physical_expr::{PhysicalSortExpr, expressions};
    use datafusion::physical_plan::{
        coalesce_partitions::CoalescePartitionsExec, limit::GlobalLimitExec, sorts::sort::SortExec,
    };
    if plan
        .downcast_ref::<lance_datafusion::exec::StrictBatchSizeExec>()
        .is_some()
    {
        // Offset can split a previously strict batch. Keep Lance's final rechunker
        // outside the window, preserving its resolved batch size, including defaults.
        let input =
            apply_result_window(plan.children()[0].clone(), offset, limit, distance_column)?;
        return replace_children_if_necessary(plan, vec![input]);
    }
    let sort = PhysicalSortExpr {
        expr: expressions::col(distance_column, plan.schema().as_ref())?,
        options: arrow::compute::SortOptions {
            descending: false,
            nulls_first: false,
        },
    };
    // Fragment-scoped payload takes can reorder batches. Restore distance order
    // before the window; the nearest plan already bounds candidate rows by k.
    let sorted = Arc::new(SortExec::new(
        [sort].into(),
        Arc::new(CoalescePartitionsExec::new(plan)),
    ));
    Ok(Arc::new(GlobalLimitExec::new(sorted, offset, limit)))
}

#[derive(Clone, Debug)]
enum Scoring {
    Exact {
        query: ArrayRef,
        column: String,
        metric: DistanceType,
    },
    Indexed,
}

#[derive(Debug)]
struct MultiVectorScoreExec {
    original: Arc<dyn ExecutionPlan>,
    mode: Scoring,
}

impl DisplayAs for MultiVectorScoreExec {
    fn fmt_as(&self, _: DisplayFormatType, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        write!(f, "MultiVectorScore: {}", self.original.name())
    }
}

impl ExecutionPlan for MultiVectorScoreExec {
    fn name(&self) -> &str {
        "MultiVectorScoreExec"
    }
    fn properties(&self) -> &Arc<PlanProperties> {
        self.original.properties()
    }
    fn children(&self) -> Vec<&Arc<dyn ExecutionPlan>> {
        self.original.children()
    }
    fn apply_expressions(
        &self,
        f: &mut dyn FnMut(&Arc<dyn PhysicalExpr>) -> Result<TreeNodeRecursion>,
    ) -> Result<TreeNodeRecursion> {
        self.original.apply_expressions(f)
    }
    fn input_distribution_requirements(&self) -> InputDistributionRequirements {
        self.original.input_distribution_requirements()
    }
    fn replace_children(
        self: Arc<Self>,
        children: Vec<Arc<dyn ExecutionPlan>>,
        options: ReplaceChildrenOptions,
    ) -> Result<Arc<dyn ExecutionPlan>> {
        Ok(Arc::new(Self {
            original: self.original.clone().replace_children(children, options)?,
            mode: self.mode.clone(),
        }))
    }
    #[allow(deprecated)]
    fn with_new_children(
        self: Arc<Self>,
        children: Vec<Arc<dyn ExecutionPlan>>,
    ) -> Result<Arc<dyn ExecutionPlan>> {
        self.replace_children(
            children,
            ReplaceChildrenOptions::new(ChildrenPropertiesMode::Recompute),
        )
    }
    fn execute(
        &self,
        partition: usize,
        context: Arc<TaskContext>,
    ) -> Result<SendableRecordBatchStream> {
        let schema = self.schema();
        match &self.mode {
            Scoring::Exact {
                query,
                column,
                metric,
            } => {
                let input = self.children()[0].execute(partition, context)?;
                let query = query.clone();
                let column = column.clone();
                let metric = *metric;
                let output_schema = schema.clone();
                let output = input
                    .map(move |batch| {
                        let query = query.clone();
                        let column = column.clone();
                        let schema = output_schema.clone();
                        async move {
                            let batch = batch?;
                            tokio::task::spawn_blocking(move || {
                                exact_batch(batch, query, &column, metric, schema)
                            })
                            .await
                            .map_err(|e| DataFusionError::External(Box::new(e)))?
                        }
                    })
                    .buffered(lance_core::utils::tokio::get_num_compute_intensive_cpus());
                Ok(Box::pin(RecordBatchStreamAdapter::new(schema, output)))
            }
            Scoring::Indexed => {
                let inputs = self
                    .children()
                    .into_iter()
                    .map(|child| child.execute(partition, context.clone()))
                    .collect::<Result<Vec<_>>>()?;
                let output_schema = schema.clone();
                let output =
                    stream::once(async move { indexed_batch(inputs, output_schema).await });
                Ok(Box::pin(RecordBatchStreamAdapter::new(schema, output)))
            }
        }
    }
}

fn row_distance<T: ArrowPrimitiveType>(
    query: &dyn Array,
    vectors: &FixedSizeListArray,
    metric: DistanceType,
) -> Result<Option<f32>>
where
    T::Native: L2 + Cosine + Dot + Into<f64>,
{
    let q = query
        .as_any()
        .downcast_ref::<arrow_array::PrimitiveArray<T>>()
        .ok_or_else(|| invalid("multi-vector query element type mismatch"))?;
    let values = vectors
        .values()
        .as_any()
        .downcast_ref::<arrow_array::PrimitiveArray<T>>()
        .ok_or_else(|| invalid("multi-vector stored element type mismatch"))?;
    if vectors.null_count() != 0
        || values.null_count() != 0
        || values
            .values()
            .iter()
            .any(|v| !Into::<f64>::into(*v).is_finite())
    {
        return Err(invalid(
            "multi-vector stored subvectors must contain only finite, non-null elements",
        ));
    }
    let dimension = vectors.value_length() as usize;
    let distance = metric
        .try_func::<<T as ArrowPrimitiveType>::Native>()
        .map_err(|error| invalid(format!("unsupported distance metric: {error}")))?;
    // Subtracting each small distance from 1 rounds it away before TopK. Sum minima
    // directly, using f64 only for the accumulator; the base kernels and output remain f32.
    let mut score = 0.0f64;
    for query_vector in q.values().chunks_exact(dimension) {
        let best = values
            .values()
            .chunks_exact(dimension)
            .map(|vector| distance(query_vector, vector))
            // Finite zero-norm vectors have undefined cosine distance. Ignore those
            // pairs; a query with no defined match masks this row, not the whole scan.
            .filter(|distance| !distance.is_nan())
            .min_by(f32::total_cmp);
        let Some(best) = best else {
            return Ok(None);
        };
        score += best as f64;
    }
    let score = score as f32;
    if !score.is_finite() {
        return Err(invalid("multi-vector distance is not finite"));
    }
    Ok(Some(score))
}

fn vector_column(batch: &RecordBatch, column: &str) -> Result<ArrayRef> {
    if let Some(array) = batch.column_by_name(column) {
        return Ok(array.clone());
    }
    // The planner resolves field paths, including quoted dotted names. Its private
    // KNN resolver is not exported, so use the same parser and struct traversal here.
    let parts = lance_core::datatypes::parse_field_path(column)
        .map_err(|e| invalid(format!("invalid vector column path '{column}': {e}")))?;
    let root = parts
        .first()
        .ok_or_else(|| invalid("empty vector column path"))?;
    let mut array = batch
        .column_by_name(root)
        .cloned()
        .ok_or_else(|| invalid(format!("missing vector column '{column}'")))?;
    for part in &parts[1..] {
        array = array
            .as_any()
            .downcast_ref::<arrow_array::StructArray>()
            .and_then(|parent| parent.column_by_name(part))
            .cloned()
            .ok_or_else(|| {
                invalid(format!(
                    "missing struct field '{part}' in vector column '{column}'"
                ))
            })?;
    }
    Ok(array)
}

fn exact_batch(
    batch: RecordBatch,
    query: ArrayRef,
    column: &str,
    metric: DistanceType,
    schema: SchemaRef,
) -> Result<RecordBatch> {
    if batch.num_rows() == 0 {
        return Ok(RecordBatch::new_empty(schema));
    }
    let array = vector_column(&batch, column)?;
    let vectors = array
        .as_any()
        .downcast_ref::<ListArray>()
        .ok_or_else(|| invalid("multi-vector scoring requires a List column"))?;
    let row_ids = batch.column_by_name("_rowid");
    let mut scores = Vec::with_capacity(batch.num_rows());
    for (i, vector) in vectors.iter().enumerate() {
        if row_ids.is_some_and(|ids| ids.is_null(i)) {
            scores.push(None);
            continue;
        }
        let Some(vector) = vector else {
            scores.push(None);
            continue;
        };
        let vector = vector
            .as_any()
            .downcast_ref::<FixedSizeListArray>()
            .ok_or_else(|| invalid("multi-vector row must be a FixedSizeList"))?;
        if vector.is_empty() {
            scores.push(None);
            continue;
        }
        let score = match query.data_type() {
            DataType::Float16 => row_distance::<Float16Type>(query.as_ref(), vector, metric),
            DataType::Float32 => row_distance::<Float32Type>(query.as_ref(), vector, metric),
            DataType::Float64 => row_distance::<Float64Type>(query.as_ref(), vector, metric),
            _ => Err(invalid("unsupported multi-vector element type")),
        }?;
        scores.push(score);
    }
    let mask = BooleanArray::from_iter(scores.iter().map(|score| Some(score.is_some())));
    let distances: ArrayRef = Arc::new(Float32Array::from(scores));
    let columns = schema
        .fields()
        .iter()
        .map(|field| {
            if field.name() == "_distance" {
                Ok(distances.clone())
            } else {
                batch
                    .column_by_name(field.name())
                    .cloned()
                    .ok_or_else(|| invalid(format!("missing score output column {}", field.name())))
            }
        })
        .collect::<Result<Vec<_>>>()?;
    Ok(arrow::compute::filter_record_batch(
        &RecordBatch::try_new(schema, columns)?,
        &mask,
    )?)
}

async fn indexed_batch(
    inputs: Vec<SendableRecordBatchStream>,
    schema: SchemaRef,
) -> Result<RecordBatch> {
    // A query child may emit several batches. Reduce its entire stream exactly once;
    // treating each batch as a query adds spurious missing-query contributions.
    let queries = futures::future::try_join_all(inputs.into_iter().map(|mut input| async move {
        let mut rows = HashMap::<u64, f32>::new();
        let mut maximum: Option<f32> = None;
        while let Some(batch) = input.try_next().await? {
            let ids = batch
                .column_by_name("_rowid")
                .and_then(|a| a.as_any().downcast_ref::<UInt64Array>())
                .ok_or_else(|| invalid("indexed multi-vector scorer requires row IDs"))?;
            let distances = batch
                .column_by_name("_distance")
                .and_then(|a| a.as_any().downcast_ref::<Float32Array>())
                .ok_or_else(|| invalid("indexed multi-vector scorer requires distances"))?;
            for i in 0..batch.num_rows() {
                let distance = distances.value(i);
                if ids.is_null(i) || distances.is_null(i) || !distance.is_finite() {
                    return Err(invalid(
                        "indexed multi-vector candidate has invalid row ID or distance",
                    ));
                }
                maximum = Some(maximum.map_or(distance, |old| old.max(distance)));
                rows.entry(ids.value(i))
                    .and_modify(|old| *old = old.min(distance))
                    .or_insert(distance);
            }
        }
        Ok::<_, DataFusionError>((rows, maximum.unwrap_or(1.0)))
    }))
    .await?;
    let mut results = HashMap::<u64, f64>::new();
    let mut missed = 0.0f64;
    for (rows, maximum) in queries {
        for (id, score) in &mut results {
            *score += *rows.get(id).unwrap_or(&maximum) as f64;
        }
        for (id, score) in rows {
            results.entry(id).or_insert(score as f64 + missed);
        }
        missed += maximum as f64;
    }
    let (ids, scores): (Vec<_>, Vec<_>) = results.into_iter().unzip();
    Ok(RecordBatch::try_new(
        schema,
        vec![
            Arc::new(Float32Array::from_iter_values(
                scores.into_iter().map(|score| score as f32),
            )),
            Arc::new(UInt64Array::from(ids)),
        ],
    )?)
}

pub(crate) fn validate_query(values: &dyn Array) -> Result<()> {
    fn finite<T: ArrowPrimitiveType>(values: &dyn Array) -> bool
    where
        T::Native: Into<f64>,
    {
        values
            .as_any()
            .downcast_ref::<arrow_array::PrimitiveArray<T>>()
            .is_some_and(|array| {
                array.null_count() == 0
                    && array
                        .values()
                        .iter()
                        .all(|v| Into::<f64>::into(*v).is_finite())
            })
    }
    let valid = match values.data_type() {
        DataType::Float16 => finite::<Float16Type>(values),
        DataType::Float32 => finite::<Float32Type>(values),
        DataType::Float64 => finite::<Float64Type>(values),
        _ => false,
    };
    if valid {
        Ok(())
    } else {
        Err(invalid(
            "multi-vector query must contain only finite, non-null elements",
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use arrow_schema::{Field, Schema};
    use lance_datafusion::exec::{
        LanceExecutionOptions, OneShotExec, StrictBatchSizeExec, execute_plan,
    };

    #[test]
    fn result_window_preserves_strict_batching_and_distance_order() {
        crate::runtime::block_on(async {
            for size in [2, 3] {
                for (offset, limit) in [
                    (1, Some(4)),
                    (1, Some(3)),
                    (5, Some(4)),
                    (6, Some(4)),
                    (1, None),
                ] {
                    let schema = Arc::new(Schema::new(vec![Field::new(
                        "_distance",
                        DataType::Float32,
                        false,
                    )]));
                    let batches = [[5., 0.], [4., 1.], [3., 2.]]
                        .into_iter()
                        .map(|values| {
                            RecordBatch::try_new(
                                schema.clone(),
                                vec![Arc::new(Float32Array::from(values.to_vec()))],
                            )
                            .map_err(DataFusionError::from)
                        })
                        .collect::<Vec<_>>();
                    let input = Arc::new(OneShotExec::new(Box::pin(
                        RecordBatchStreamAdapter::new(schema, stream::iter(batches)),
                    )));
                    let plan = Arc::new(StrictBatchSizeExec::new(input, size));
                    let plan = apply_result_window(plan, offset, limit, "_distance").unwrap();
                    let batches: Vec<_> = execute_plan(
                        plan,
                        LanceExecutionOptions {
                            batch_size: Some(2),
                            ..Default::default()
                        },
                    )
                    .unwrap()
                    .try_collect()
                    .await
                    .unwrap();
                    let actual: Vec<_> = batches
                        .iter()
                        .flat_map(|b| {
                            b.column(0)
                                .as_any()
                                .downcast_ref::<Float32Array>()
                                .unwrap()
                                .values()
                                .to_vec()
                        })
                        .collect();
                    let expected: Vec<_> = (0..6)
                        .skip(offset)
                        .take(limit.unwrap_or(6))
                        .map(|i| i as f32)
                        .collect();
                    assert_eq!(actual, expected);
                    assert_eq!(
                        batches
                            .iter()
                            .map(RecordBatch::num_rows)
                            .collect::<Vec<_>>(),
                        expected.chunks(size).map(<[f32]>::len).collect::<Vec<_>>()
                    );
                }
            }
        });
    }
}
