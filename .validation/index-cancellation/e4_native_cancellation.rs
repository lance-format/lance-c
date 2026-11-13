// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Acceptance tests for the cooperative native index cancellation candidate.
//!
//! Every scenario drives the real public `DatasetIndexExt::create_index_builder`
//! path. Cancellation is requested by the wrapped observer at a deterministic
//! callback; there are no sleeps or timing races.
//!
//! The tests cover:
//! - a pre-cancelled request rejected before any index work,
//! - cancellation during a long BTREE page-building stage,
//! - cancellation in the last observer callback, before publication,
//! - `execute_uncommitted` returning no metadata after cancellation,
//! - typed cancellation staying distinct from an unrelated observer error,
//! - successful BTREE and vector controls through the cancellable adapter,
//! - both trained and empty/untrained multi-segment FM-Index publication routes.

use std::fmt;
use std::sync::{Arc, Mutex};

use arrow_array::builder::{FixedSizeListBuilder, Float32Builder, StringBuilder};
use arrow_array::{Int32Array, RecordBatch};
use arrow_schema::{DataType, Field, Schema};
use async_trait::async_trait;
use lance::dataset::Dataset;
use lance::dataset::{WriteMode, WriteParams};
use lance::index::DatasetIndexExt;
use lance::index::vector::VectorIndexParams;
use lance_core::Error;
use lance_index::IndexType;
use lance_index::progress::{
    CancellableIndexBuildProgress, IndexBuildCancelled, IndexBuildProgress,
    is_index_build_cancelled,
};
use lance_index::scalar::{BuiltinIndexType, ScalarIndexParams};
use lance_linalg::distance::MetricType;
use tempfile::TempDir;
use tokio_util::sync::CancellationToken;

const DIM: i32 = 8;
const NUM_FRAGMENTS: i32 = 3;
const ROWS_PER_FRAGMENT: i32 = 32;
const TOTAL_ROWS: u64 = (NUM_FRAGMENTS * ROWS_PER_FRAGMENT) as u64;
const NUM_PARTITIONS: usize = 2;

#[derive(Debug, Clone, PartialEq, Eq)]
enum Event {
    Start {
        stage: String,
        total: Option<u64>,
        unit: String,
    },
    Progress {
        stage: String,
        completed: u64,
    },
    Complete {
        stage: String,
    },
}

impl Event {
    fn boundary(&self) -> String {
        match self {
            Self::Start { stage, .. } => format!("start:{stage}"),
            Self::Progress { stage, .. } => format!("progress:{stage}"),
            Self::Complete { stage } => format!("complete:{stage}"),
        }
    }

    fn is_complete(&self, expected: &str) -> bool {
        matches!(self, Self::Complete { stage } if stage == expected)
    }
}

#[derive(Debug)]
struct ObserverFailure {
    marker: String,
}

impl fmt::Display for ObserverFailure {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.marker)
    }
}

impl std::error::Error for ObserverFailure {}

fn observer_failure(kind: &str, stage: &str) -> Error {
    Error::external(Box::new(ObserverFailure {
        marker: format!("e4-cancel-test-observer:{kind}:{stage}"),
    }))
}

#[derive(Debug, Clone)]
enum Trigger {
    None,
    FailStart(String),
    CancelAtProgress { stage: String, min_completed: u64 },
    CancelAtComplete { stage: String, occurrence: usize },
}

/// Wrapped observer. It intentionally does not implement `check_cancelled`, so
/// the test also proves existing observers remain source compatible.
#[derive(Debug)]
struct TestObserver {
    trigger: Trigger,
    token: CancellationToken,
    events: Mutex<Vec<Event>>,
    complete_counts: Mutex<Vec<(String, usize)>>,
}

impl TestObserver {
    fn new(trigger: Trigger, token: CancellationToken) -> Self {
        Self {
            trigger,
            token,
            events: Mutex::new(Vec::new()),
            complete_counts: Mutex::new(Vec::new()),
        }
    }

    fn record(&self, event: Event) {
        self.events.lock().unwrap().push(event);
    }

    fn events(&self) -> Vec<Event> {
        self.events.lock().unwrap().clone()
    }

    fn boundaries(&self) -> Vec<String> {
        self.events().iter().map(Event::boundary).collect()
    }

    fn progress_values(&self, stage: &str) -> Vec<u64> {
        self.events()
            .into_iter()
            .filter_map(|event| match event {
                Event::Progress {
                    stage: event_stage,
                    completed,
                } if event_stage == stage => Some(completed),
                _ => None,
            })
            .collect()
    }
}

#[async_trait]
impl IndexBuildProgress for TestObserver {
    async fn stage_start(
        &self,
        stage: &str,
        total: Option<u64>,
        unit: &str,
    ) -> lance_core::Result<()> {
        self.record(Event::Start {
            stage: stage.to_string(),
            total,
            unit: unit.to_string(),
        });
        if let Trigger::FailStart(fail_stage) = &self.trigger
            && fail_stage == stage
        {
            return Err(observer_failure("stage_start", stage));
        }
        Ok(())
    }

    async fn stage_progress(&self, stage: &str, completed: u64) -> lance_core::Result<()> {
        self.record(Event::Progress {
            stage: stage.to_string(),
            completed,
        });
        match &self.trigger {
            Trigger::CancelAtProgress {
                stage: cancel_stage,
                min_completed,
            } if cancel_stage == stage && completed >= *min_completed => {
                self.token.cancel();
            }
            _ => {}
        }
        Ok(())
    }

    async fn stage_complete(&self, stage: &str) -> lance_core::Result<()> {
        self.record(Event::Complete {
            stage: stage.to_string(),
        });
        let occurrence = {
            let mut counts = self.complete_counts.lock().unwrap();
            match counts.iter_mut().find(|(name, _)| name == stage) {
                Some((_, count)) => {
                    *count += 1;
                    *count
                }
                None => {
                    counts.push((stage.to_string(), 1));
                    1
                }
            }
        };
        if let Trigger::CancelAtComplete {
            stage: cancel_stage,
            occurrence: cancel_occurrence,
        } = &self.trigger
            && cancel_stage == stage
            && occurrence == *cancel_occurrence
        {
            self.token.cancel();
        }
        Ok(())
    }
}

fn cancellable(
    trigger: Trigger,
) -> (
    CancellationToken,
    Arc<TestObserver>,
    Arc<dyn IndexBuildProgress>,
) {
    let token = CancellationToken::new();
    let observer = Arc::new(TestObserver::new(trigger, token.clone()));
    let progress: Arc<dyn IndexBuildProgress> = Arc::new(CancellableIndexBuildProgress::new(
        observer.clone(),
        token.clone(),
    ));
    (token, observer, progress)
}

fn assert_typed_cancellation(err: &Error) {
    assert!(
        is_index_build_cancelled(err),
        "expected typed native cancellation, got {err:?}"
    );
    match err {
        Error::External { source } => assert!(
            source.downcast_ref::<IndexBuildCancelled>().is_some(),
            "cancellation source lost its type: {source:?}"
        ),
        _ => panic!("typed cancellation must use Error::External, got {err:?}"),
    }
}

fn assert_observer_failure(err: &Error) {
    assert!(
        !is_index_build_cancelled(err),
        "observer failure was reclassified as cancellation: {err:?}"
    );
    match err {
        Error::External { source } => assert!(
            source.downcast_ref::<ObserverFailure>().is_some(),
            "observer error identity was not preserved: {source:?}"
        ),
        _ => panic!("observer failure must stay Error::External, got {err:?}"),
    }
}

async fn dataset_state(uri: &str) -> (u64, Vec<String>) {
    let dataset = Dataset::open(uri).await.unwrap();
    let version = dataset.version().version;
    let mut names = dataset
        .load_indices()
        .await
        .unwrap()
        .iter()
        .map(|index| index.name.clone())
        .collect::<Vec<_>>();
    names.sort();
    (version, names)
}

async fn write_fragments(uri: &str, schema: Arc<Schema>, batches: Vec<RecordBatch>) {
    for (fragment, batch) in batches.into_iter().enumerate() {
        let params = WriteParams {
            mode: if fragment == 0 {
                WriteMode::Create
            } else {
                WriteMode::Append
            },
            ..Default::default()
        };
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema.clone()),
            uri,
            Some(params),
        )
        .await
        .unwrap();
    }
}

async fn scalar_dataset() -> (TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp
        .path()
        .join("e4-native-cancellation-scalar")
        .to_str()
        .unwrap()
        .to_string();
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new(
            "embedding",
            DataType::FixedSizeList(Arc::new(Field::new("item", DataType::Float32, true)), DIM),
            false,
        ),
    ]));

    let mut batches = Vec::new();
    for fragment in 0..NUM_FRAGMENTS {
        let mut vectors = FixedSizeListBuilder::new(Float32Builder::new(), DIM);
        for row in 0..ROWS_PER_FRAGMENT {
            for component in 0..DIM {
                vectors.values().append_value(
                    (fragment * ROWS_PER_FRAGMENT + row) as f32 + component as f32 / DIM as f32,
                );
            }
            vectors.append(true);
        }
        let ids = (0..ROWS_PER_FRAGMENT)
            .map(|row| fragment * ROWS_PER_FRAGMENT + row)
            .collect::<Vec<_>>();
        batches.push(
            RecordBatch::try_new(
                schema.clone(),
                vec![Arc::new(Int32Array::from(ids)), Arc::new(vectors.finish())],
            )
            .unwrap(),
        );
    }
    write_fragments(&uri, schema, batches).await;

    let dataset = Dataset::open(&uri).await.unwrap();
    assert_eq!(dataset.count_fragments(), NUM_FRAGMENTS as usize);
    assert_eq!(dataset.version().version, NUM_FRAGMENTS as u64);
    assert_eq!(dataset.count_rows(None).await.unwrap(), TOTAL_ROWS as usize);
    (tmp, uri)
}

async fn text_dataset() -> (TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp
        .path()
        .join("e4-native-cancellation-fm")
        .to_str()
        .unwrap()
        .to_string();
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("text", DataType::Utf8, false),
    ]));

    let mut batches = Vec::new();
    for fragment in 0..NUM_FRAGMENTS {
        let ids = (0..ROWS_PER_FRAGMENT)
            .map(|row| fragment * ROWS_PER_FRAGMENT + row)
            .collect::<Vec<_>>();
        let mut text = StringBuilder::new();
        for row in 0..ROWS_PER_FRAGMENT {
            text.append_value(format!(
                "fragment{fragment} row{row} alpha beta gamma delta"
            ));
        }
        batches.push(
            RecordBatch::try_new(
                schema.clone(),
                vec![Arc::new(Int32Array::from(ids)), Arc::new(text.finish())],
            )
            .unwrap(),
        );
    }
    write_fragments(&uri, schema, batches).await;

    let dataset = Dataset::open(&uri).await.unwrap();
    assert_eq!(dataset.count_fragments(), NUM_FRAGMENTS as usize);
    assert_eq!(dataset.version().version, NUM_FRAGMENTS as u64);
    (tmp, uri)
}

fn btree_params() -> ScalarIndexParams {
    ScalarIndexParams::for_builtin(BuiltinIndexType::BTree)
}

fn btree_page_params() -> ScalarIndexParams {
    ScalarIndexParams::for_builtin(BuiltinIndexType::BTree)
        .with_params(&serde_json::json!({ "zone_size": 16 }))
}

fn fm_params() -> ScalarIndexParams {
    ScalarIndexParams::new("fm".to_string()).with_params(&serde_json::json!({ "num_segments": 2 }))
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn pre_cancelled_startup_returns_typed_cancellation_without_work() {
    let (_tmp, uri) = scalar_dataset().await;
    let before = dataset_state(&uri).await;
    let mut dataset = Dataset::open(&uri).await.unwrap();

    let (token, observer, progress) = cancellable(Trigger::None);
    token.cancel();

    let err = dataset
        .create_index_builder(&["id"], IndexType::BTree, &btree_params())
        .name("id_btree".to_string())
        .index_uuid(uuid::Uuid::from_u128(0xe4_ca_0000_0000_0000_0000_0000_0001))
        .progress(progress)
        .await
        .unwrap_err();

    assert_typed_cancellation(&err);
    assert!(
        observer.events().is_empty(),
        "startup cancellation ran index work: {:?}",
        observer.boundaries()
    );
    assert_eq!(dataset_state(&uri).await, before);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn btree_page_build_cancellation_stops_long_stage() {
    let (_tmp, uri) = scalar_dataset().await;
    let before = dataset_state(&uri).await;
    let mut dataset = Dataset::open(&uri).await.unwrap();

    let (_token, observer, progress) = cancellable(Trigger::CancelAtProgress {
        stage: "build_pages".to_string(),
        min_completed: 2,
    });

    let err = dataset
        .create_index_builder(&["id"], IndexType::BTree, &btree_page_params())
        .name("id_btree".to_string())
        .index_uuid(uuid::Uuid::from_u128(0xe4_ca_0000_0000_0000_0000_0000_0002))
        .progress(progress)
        .await
        .unwrap_err();

    assert_typed_cancellation(&err);
    assert_eq!(
        observer.progress_values("build_pages"),
        vec![1, 2],
        "cancellation must stop page building at the requested checkpoint"
    );
    assert!(
        observer
            .boundaries()
            .iter()
            .any(|boundary| boundary == "start:build_pages"),
        "BTREE must report the long page-building stage: {:?}",
        observer.boundaries()
    );
    assert!(
        !observer
            .events()
            .iter()
            .any(|event| event.is_complete("build_pages")),
        "cancelled page building must not report completion"
    );
    assert_eq!(dataset_state(&uri).await, before);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn uncommitted_last_completion_window_returns_no_metadata() {
    let (_tmp, uri) = scalar_dataset().await;
    let before = dataset_state(&uri).await;
    let mut dataset = Dataset::open(&uri).await.unwrap();

    let (_token, observer, progress) = cancellable(Trigger::CancelAtComplete {
        stage: "merge_partitions".to_string(),
        occurrence: 1,
    });

    let err = dataset
        .create_index_builder(
            &["embedding"],
            IndexType::Vector,
            &VectorIndexParams::ivf_flat(NUM_PARTITIONS, MetricType::L2),
        )
        .name("embedding_ivf".to_string())
        .index_uuid(uuid::Uuid::from_u128(0xe4_ca_0000_0000_0000_0000_0000_0003))
        .progress(progress)
        .execute_uncommitted()
        .await
        .unwrap_err();

    assert_typed_cancellation(&err);
    assert!(
        observer
            .boundaries()
            .iter()
            .any(|boundary| boundary == "complete:merge_partitions"),
        "test must cancel inside the last observer callback: {:?}",
        observer.boundaries()
    );
    assert_eq!(dataset_state(&uri).await, before);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn committed_last_completion_window_does_not_publish() {
    let (_tmp, uri) = scalar_dataset().await;
    let before = dataset_state(&uri).await;
    let mut dataset = Dataset::open(&uri).await.unwrap();

    let (_token, observer, progress) = cancellable(Trigger::CancelAtComplete {
        stage: "merge_partitions".to_string(),
        occurrence: 1,
    });

    let err = dataset
        .create_index_builder(
            &["embedding"],
            IndexType::Vector,
            &VectorIndexParams::ivf_flat(NUM_PARTITIONS, MetricType::L2),
        )
        .name("embedding_ivf".to_string())
        .index_uuid(uuid::Uuid::from_u128(0xe4_ca_0000_0000_0000_0000_0000_0004))
        .progress(progress)
        .await
        .unwrap_err();

    assert_typed_cancellation(&err);
    assert!(
        observer
            .events()
            .iter()
            .any(|event| event.is_complete("merge_partitions")),
        "test must cancel after the final stage callback: {:?}",
        observer.boundaries()
    );
    assert_eq!(dataset_state(&uri).await, before);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn observer_error_is_not_reclassified_as_cancellation() {
    let (_tmp, uri) = scalar_dataset().await;
    let before = dataset_state(&uri).await;
    let mut dataset = Dataset::open(&uri).await.unwrap();

    let (token, observer, progress) = cancellable(Trigger::FailStart("load_data".to_string()));

    let err = dataset
        .create_index_builder(&["id"], IndexType::BTree, &btree_params())
        .name("id_btree".to_string())
        .index_uuid(uuid::Uuid::from_u128(0xe4_ca_0000_0000_0000_0000_0000_0005))
        .progress(progress)
        .await
        .unwrap_err();

    assert_observer_failure(&err);
    assert!(!token.is_cancelled());
    assert_eq!(
        observer.boundaries(),
        vec!["start:load_data"],
        "observer failure must propagate from the same callback"
    );
    assert_eq!(dataset_state(&uri).await, before);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn btree_success_control_reports_pages_and_publishes() {
    let (_tmp, uri) = scalar_dataset().await;
    let before = dataset_state(&uri).await;
    let mut dataset = Dataset::open(&uri).await.unwrap();

    let (token, observer, progress) = cancellable(Trigger::None);

    let created = dataset
        .create_index_builder(&["id"], IndexType::BTree, &btree_page_params())
        .name("id_btree".to_string())
        .index_uuid(uuid::Uuid::from_u128(0xe4_ca_0000_0000_0000_0000_0000_0006))
        .progress(progress)
        .await
        .unwrap();

    assert!(!token.is_cancelled());
    assert_eq!(created.name, "id_btree");
    assert_eq!(
        observer.progress_values("build_pages"),
        vec![1, 2, 3, 4, 5, 6],
        "each 16-row BTREE page must report progress"
    );
    assert!(
        observer
            .events()
            .iter()
            .any(|event| event.is_complete("build_pages")),
        "successful page building must report completion: {:?}",
        observer.boundaries()
    );
    let (version, names) = dataset_state(&uri).await;
    assert_eq!(version, before.0 + 1);
    assert_eq!(names, vec!["id_btree".to_string()]);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn vector_success_control_publishes_after_all_checkpoints() {
    let (_tmp, uri) = scalar_dataset().await;
    let before = dataset_state(&uri).await;
    let mut dataset = Dataset::open(&uri).await.unwrap();

    let (token, observer, progress) = cancellable(Trigger::None);

    let created = dataset
        .create_index_builder(
            &["embedding"],
            IndexType::Vector,
            &VectorIndexParams::ivf_flat(NUM_PARTITIONS, MetricType::L2),
        )
        .name("embedding_ivf".to_string())
        .index_uuid(uuid::Uuid::from_u128(0xe4_ca_0000_0000_0000_0000_0000_0007))
        .progress(progress)
        .await
        .unwrap();

    assert!(!token.is_cancelled());
    assert_eq!(created.name, "embedding_ivf");
    assert_eq!(
        observer.progress_values("merge_partitions"),
        vec![1, 2],
        "both merged partitions must report progress"
    );
    assert!(
        observer
            .events()
            .iter()
            .any(|event| event.is_complete("merge_partitions")),
        "successful merge must report completion: {:?}",
        observer.boundaries()
    );
    let (version, names) = dataset_state(&uri).await;
    assert_eq!(version, before.0 + 1);
    assert_eq!(names, vec!["embedding_ivf".to_string()]);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn fm_trained_multi_segment_cancellation_before_publication() {
    let (_tmp, uri) = text_dataset().await;
    let before = dataset_state(&uri).await;
    let mut dataset = Dataset::open(&uri).await.unwrap();

    let (_token, observer, progress) = cancellable(Trigger::CancelAtComplete {
        stage: "load_data".to_string(),
        occurrence: 2,
    });

    let err = dataset
        .create_index_builder(&["text"], IndexType::Fm, &fm_params())
        .name("text_fm".to_string())
        .index_uuid(uuid::Uuid::from_u128(0xe4_ca_0000_0000_0000_0000_0000_0008))
        .progress(progress)
        .await
        .unwrap_err();

    assert_typed_cancellation(&err);
    assert_eq!(
        observer
            .events()
            .iter()
            .filter(|event| event.is_complete("load_data"))
            .count(),
        2,
        "trained two-segment FM build must reach the second segment completion: {:?}",
        observer.boundaries()
    );
    assert_eq!(dataset_state(&uri).await, before);

    // Proves the same fixture publishes when the request is not cancelled.
    let (_token, _observer, progress) = cancellable(Trigger::None);
    let created = dataset
        .create_index_builder(&["text"], IndexType::Fm, &fm_params())
        .name("text_fm".to_string())
        .index_uuid(uuid::Uuid::from_u128(0xe4_ca_0000_0000_0000_0000_0000_0009))
        .progress(progress)
        .await
        .unwrap();
    assert_eq!(created.name, "text_fm");
    let (version, names) = dataset_state(&uri).await;
    assert_eq!(version, before.0 + 1);
    // Multi-segment FM publishes two segment metadata entries under one name.
    assert_eq!(
        names,
        vec!["text_fm".to_string(), "text_fm".to_string()],
        "both trained FM segments must be committed"
    );
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn fm_untrained_multi_segment_cancellation_before_publication() {
    let (_tmp, uri) = text_dataset().await;
    let before = dataset_state(&uri).await;
    let mut dataset = Dataset::open(&uri).await.unwrap();

    let (_token, observer, progress) = cancellable(Trigger::CancelAtComplete {
        stage: "load_data".to_string(),
        occurrence: 1,
    });

    let err = dataset
        .create_index_builder(&["text"], IndexType::Fm, &fm_params())
        .name("text_fm_empty".to_string())
        .train(false)
        .index_uuid(uuid::Uuid::from_u128(0xe4_ca_0000_0000_0000_0000_0000_000a))
        .progress(progress)
        .await
        .unwrap_err();

    assert_typed_cancellation(&err);
    assert!(
        observer
            .events()
            .iter()
            .any(|event| event.is_complete("load_data")),
        "untrained FM build must reach its completion callback: {:?}",
        observer.boundaries()
    );
    assert_eq!(dataset_state(&uri).await, before);

    // Proves the empty/untrained publication route works without cancellation.
    let (_token, _observer, progress) = cancellable(Trigger::None);
    let created = dataset
        .create_index_builder(&["text"], IndexType::Fm, &fm_params())
        .name("text_fm_empty".to_string())
        .train(false)
        .index_uuid(uuid::Uuid::from_u128(0xe4_ca_0000_0000_0000_0000_0000_000b))
        .progress(progress)
        .await
        .unwrap();
    assert_eq!(created.name, "text_fm_empty");
    let (version, names) = dataset_state(&uri).await;
    assert_eq!(version, before.0 + 1);
    assert_eq!(names, vec!["text_fm_empty".to_string()]);
}
