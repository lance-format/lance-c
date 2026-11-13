// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! One-off oracle for `MatchQuery::last_as_prefix`.
//!
//! This test is deliberately isolated from the regular lance test suite: it
//! exercises the proposed native contract on a patched checkout and is not
//! part of the candidate upstream patch.

use std::sync::Arc;

use arrow_array::builder::{ListBuilder, StringBuilder};
use arrow_array::cast::AsArray;
use arrow_array::types::{Float32Type, Int32Type};
use arrow_array::{ArrayRef, Int32Array, RecordBatch, RecordBatchIterator, StringArray};
use arrow_schema::{DataType, Field as ArrowField, Schema as ArrowSchema};
use lance::Dataset;
use lance::dataset::scanner::ColumnOrdering;
use lance::index::DatasetIndexExt;
use lance_index::IndexType;
use lance_index::scalar::inverted::query::{BooleanQuery, FtsQuery, MatchQuery, Occur, Operator};
use lance_index::scalar::inverted::{DocumentGranularity, Language};
use lance_index::scalar::{FullTextSearchQuery, InvertedIndexParams};
use tempfile::TempDir;

fn row_params() -> InvertedIndexParams {
    InvertedIndexParams::new("simple".to_string(), Language::English)
        .with_position(false)
        .lower_case(true)
        .stem(false)
        .remove_stop_words(false)
        .ascii_folding(false)
        .max_token_length(None)
}

fn list_element_params() -> InvertedIndexParams {
    row_params().document_granularity(DocumentGranularity::ListElement)
}

fn match_node(column: &str, terms: &str, last_as_prefix: bool) -> MatchQuery {
    MatchQuery::new(terms.to_string())
        .with_column(Some(column.to_string()))
        .with_last_as_prefix(last_as_prefix)
}

async fn write_rows(rows: &[&str]) -> (TempDir, Dataset) {
    let id = Int32Array::from_iter_values(0..rows.len() as i32);
    let text = StringArray::from_iter_values(rows.iter().copied());
    let schema = Arc::new(ArrowSchema::new(vec![
        ArrowField::new("id", DataType::Int32, false),
        ArrowField::new("text", DataType::Utf8, true),
    ]));
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![Arc::new(id) as ArrayRef, Arc::new(text) as ArrayRef],
    )
    .unwrap();
    let dir = tempfile::tempdir().unwrap();
    let mut dataset = Dataset::write(
        RecordBatchIterator::new(vec![Ok(batch)], schema),
        dir.path().to_str().unwrap(),
        None,
    )
    .await
    .unwrap();
    dataset
        .create_index(&["text"], IndexType::Inverted, None, &row_params(), true)
        .await
        .unwrap();
    (dir, dataset)
}

async fn write_tags(rows: &[&[&str]]) -> (TempDir, Dataset) {
    let id = Int32Array::from_iter_values(0..rows.len() as i32);
    let mut tags = ListBuilder::new(StringBuilder::new());
    for row in rows {
        for tag in *row {
            tags.values().append_value(tag);
        }
        tags.append(true);
    }
    let tags = tags.finish();
    let schema = Arc::new(ArrowSchema::new(vec![
        ArrowField::new("id", DataType::Int32, false),
        ArrowField::new(
            "tags",
            DataType::List(Arc::new(ArrowField::new("item", DataType::Utf8, true))),
            true,
        ),
    ]));
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![Arc::new(id) as ArrayRef, Arc::new(tags) as ArrayRef],
    )
    .unwrap();
    let dir = tempfile::tempdir().unwrap();
    let mut dataset = Dataset::write(
        RecordBatchIterator::new(vec![Ok(batch)], schema),
        dir.path().to_str().unwrap(),
        None,
    )
    .await
    .unwrap();
    dataset
        .create_index(
            &["tags"],
            IndexType::Inverted,
            None,
            &list_element_params(),
            true,
        )
        .await
        .unwrap();
    (dir, dataset)
}

async fn run(ds: &Dataset, query: MatchQuery) -> RecordBatch {
    let mut scanner = ds.scan();
    scanner
        .full_text_search(FullTextSearchQuery::new_query(FtsQuery::Match(query)))
        .unwrap();
    scanner
        .order_by(Some(vec![ColumnOrdering::asc_nulls_first(
            "id".to_string(),
        )]))
        .unwrap();
    scanner.try_into_batch().await.unwrap()
}

async fn run_boolean(ds: &Dataset, query: BooleanQuery) -> RecordBatch {
    let mut scanner = ds.scan();
    scanner
        .full_text_search(FullTextSearchQuery::new_query(FtsQuery::Boolean(query)))
        .unwrap();
    scanner
        .order_by(Some(vec![ColumnOrdering::asc_nulls_first(
            "id".to_string(),
        )]))
        .unwrap();
    scanner.try_into_batch().await.unwrap()
}

fn ids(batch: &RecordBatch) -> Vec<i32> {
    batch["id"].as_primitive::<Int32Type>().values().to_vec()
}

const ROWS: &[&str] = &[
    "alpha alpenglow",
    "alphabet soup",
    "alpha beta",
    "alphanumeric values",
    "alpine climbing",
    "beta gamma",
    "the alpha",
    "ALPHA ALPENGLOW",
    "alpha alp!",
    "alpha   ",
];

#[tokio::test]
async fn exact_match_remains_unchanged() {
    let (_dir, ds) = write_rows(ROWS).await;

    // `alp` is a prefix, not a term: exact matching must not invent it.
    assert_eq!(
        ids(&run(&ds, match_node("text", "alpha alp", false)).await),
        Vec::<i32>::new()
    );
    assert_eq!(
        ids(&run(
            &ds,
            match_node("text", "alpha beta", false).with_operator(Operator::And)
        )
        .await),
        vec![2]
    );

    // The refactored fuzzy path keeps its previous behavior.
    assert_eq!(
        ids(&run(
            &ds,
            match_node("text", "alpa", false).with_fuzziness(Some(1))
        )
        .await),
        vec![0, 2, 6, 7, 8, 9]
    );
}

#[tokio::test]
async fn prefix_matches_last_token_only() {
    let (_dir, ds) = write_rows(ROWS).await;

    assert_eq!(
        ids(&run(&ds, match_node("text", "alp", true)).await),
        vec![0, 1, 2, 3, 4, 6, 7, 8, 9]
    );
    assert_eq!(
        ids(&run(
            &ds,
            match_node("text", "alpha alp", true).with_operator(Operator::And)
        )
        .await),
        vec![0, 7, 8]
    );
}

#[tokio::test]
async fn boolean_composition_with_prefix() {
    let (_dir, ds) = write_rows(ROWS).await;

    let must = BooleanQuery::new([
        (
            Occur::Must,
            FtsQuery::Match(match_node("text", "alpha", false)),
        ),
        (
            Occur::Must,
            FtsQuery::Match(match_node("text", "alp", true)),
        ),
    ]);
    assert_eq!(ids(&run_boolean(&ds, must).await), vec![0, 7, 8]);

    let should = BooleanQuery::new([
        (
            Occur::Should,
            FtsQuery::Match(match_node("text", "alp", true)),
        ),
        (
            Occur::Should,
            FtsQuery::Match(match_node("text", "beta", false)),
        ),
    ]);
    assert_eq!(
        ids(&run_boolean(&ds, should).await),
        (0..=9).collect::<Vec<_>>()
    );
}

#[tokio::test]
async fn expansion_budget_is_shared_and_deterministic() {
    let (_dir, ds) = write_rows(ROWS).await;

    // `alp` < `alpenglow` < `alpha` < `alphabet` < `alphanumeric` < `alpine`.
    assert_eq!(
        ids(&run(&ds, match_node("text", "alp", true).with_max_expansions(2)).await),
        vec![0, 7, 8]
    );
    // Five terms fit, so the lexicographically last one (`alpine`) is dropped.
    assert_eq!(
        ids(&run(&ds, match_node("text", "alp", true).with_max_expansions(5)).await),
        vec![0, 1, 2, 3, 6, 7, 8, 9]
    );
    // The budget is shared with the exact `alpha` term at the earlier position.
    assert_eq!(
        ids(&run(
            &ds,
            match_node("text", "alpha alp", true)
                .with_operator(Operator::And)
                .with_max_expansions(2)
        )
        .await),
        vec![8]
    );
    assert_eq!(
        ids(&run(
            &ds,
            match_node("text", "alpha alp", true)
                .with_operator(Operator::And)
                .with_max_expansions(3)
        )
        .await),
        vec![0, 7, 8]
    );
    assert_eq!(
        ids(&run(&ds, match_node("text", "alp", true).with_max_expansions(0)).await),
        Vec::<i32>::new()
    );
}

#[tokio::test]
async fn empty_analysis_and_trailing_punctuation() {
    let (_dir, ds) = write_rows(ROWS).await;

    assert_eq!(
        ids(&run(&ds, match_node("text", "!!!", true)).await),
        Vec::<i32>::new()
    );
    assert_eq!(
        ids(&run(&ds, match_node("text", "   ", true)).await),
        Vec::<i32>::new()
    );
    for terms in ["alpha alpe!!", "alpha alpe   "] {
        assert_eq!(
            ids(&run(
                &ds,
                match_node("text", terms, true).with_operator(Operator::And)
            )
            .await),
            vec![0, 7],
            "terms={terms}"
        );
    }
}

#[tokio::test]
async fn serde_default_is_backwards_compatible() {
    let old: MatchQuery = serde_json::from_value(serde_json::json!({
        "column": "text",
        "terms": "alpha alp",
    }))
    .unwrap();
    assert!(!old.last_as_prefix);
    assert!(old.fuzziness.is_none());
    assert_eq!(old.max_expansions, 50);

    let new: MatchQuery = serde_json::from_value(serde_json::json!({
        "column": "text",
        "terms": "alpha alp",
        "last_as_prefix": true,
    }))
    .unwrap();
    assert!(new.last_as_prefix);

    let round_trip: MatchQuery =
        serde_json::from_str(&serde_json::to_string(&new).unwrap()).unwrap();
    assert_eq!(round_trip, new);

    let (_dir, ds) = write_rows(ROWS).await;
    let mut new = new;
    new.operator = Operator::And;
    assert_eq!(ids(&run(&ds, new).await), vec![0, 7]);
}

#[tokio::test]
async fn boost_scales_prefix_leaf_score() {
    let (_dir, ds) = write_rows(ROWS).await;

    let plain = run(&ds, match_node("text", "alp", true)).await;
    let boosted = run(&ds, match_node("text", "alp", true).with_boost(2.0)).await;

    let plain_scores = plain["_score"].as_primitive::<Float32Type>();
    let boosted_scores = boosted["_score"].as_primitive::<Float32Type>();
    let plain_ids = ids(&plain);
    let boosted_ids = ids(&boosted);
    assert_eq!(plain_ids, boosted_ids);
    let id0 = plain_ids.iter().position(|id| *id == 0).unwrap();
    assert!((boosted_scores.value(id0) - 2.0 * plain_scores.value(id0)).abs() < 1e-5);
}

#[tokio::test]
async fn list_element_prefix_does_not_cross_elements() {
    let (_dir, ds) = write_tags(&[
        &["alpha", "beta"],
        &["alphabet"],
        &["al", "pha"],
        &["alp", "ine"],
    ])
    .await;

    let prefix =
        match_node("tags", "alp", true).with_document_granularity(DocumentGranularity::ListElement);
    assert_eq!(ids(&run(&ds, prefix).await), vec![0, 1, 3]);

    let exact = match_node("tags", "alp", false)
        .with_document_granularity(DocumentGranularity::ListElement);
    assert_eq!(ids(&run(&ds, exact).await), vec![3]);
}
