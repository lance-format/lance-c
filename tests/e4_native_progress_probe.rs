// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Validation-only probe for issue #55 E4: can the public native
//! `IndexBuildProgress` error path carry a reusable cooperative cancellation
//! seam?
//!
//! This file is experiment evidence, not a permanent regression suite and not
//! a cancellation API. It must not change production code, the C callback
//! contract, or the native pin.
//!
//! Each scenario drives a real Lance build through the public
//! `DatasetIndexExt::create_index_builder` surface with a probe progress
//! implementation that fails at a deterministic stage event (no sleeps).
//! The probe asserts:
//!
//! 1. the failing callback's error reaches the builder caller as the exact
//!    `lance_core::Error::External` we constructed (variant, source type, and
//!    marker message are preserved),
//! 2. a committed build that fails during a stage publishes no manifest
//!    version and no index metadata,
//! 3. an uncommitted build that fails after partition work leaves only
//!    unreferenced sidecars, never published metadata,
//! 4. the stage/checkpoint coverage the seam can rely on (including the
//!    missing final pre-publication check).

use std::fmt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

use arrow_array::builder::{FixedSizeListBuilder, Float32Builder};
use arrow_array::{Int32Array, RecordBatch};
use arrow_schema::{DataType, Field, Schema};
use async_trait::async_trait;
use lance::dataset::{Dataset, WriteMode, WriteParams};
use lance::index::DatasetIndexExt;
use lance::index::vector::VectorIndexParams;
use lance_core::Error;
use lance_index::progress::IndexBuildProgress;
use lance_index::scalar::{BuiltinIndexType, ScalarIndexParams};
use lance_index::{INDEX_FILE_NAME, IndexType};
use lance_linalg::distance::MetricType;
use uuid::Uuid;

const DIM: i32 = 8;
const NUM_FRAGMENTS: i32 = 3;
const ROWS_PER_FRAGMENT: i32 = 32;
const TOTAL_ROWS: u64 = (NUM_FRAGMENTS * ROWS_PER_FRAGMENT) as u64;
const NUM_PARTITIONS: usize = 2;

fn probe_uuid() -> Uuid {
    Uuid::from_u128(0xe4_0000_0000_0000_0000_0000_0000_0001)
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum ProbeEvent {
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

impl ProbeEvent {
    fn boundary(&self) -> String {
        match self {
            Self::Start { stage, .. } => format!("start:{stage}"),
            Self::Progress { stage, .. } => format!("progress:{stage}"),
            Self::Complete { stage } => format!("complete:{stage}"),
        }
    }
}

/// A uniquely identifiable error source. A cancellation seam needs this
/// identity to survive to the caller so it can be classified separately from
/// genuine build failures.
#[derive(Debug)]
struct ProbeCancellation {
    marker: String,
}

impl fmt::Display for ProbeCancellation {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.marker)
    }
}

impl std::error::Error for ProbeCancellation {}

fn probe_cancel(kind: &str, stage: &str) -> Error {
    Error::external(Box::new(ProbeCancellation {
        marker: format!("e4-probe-cancel:{kind}:{stage}"),
    }))
}

#[derive(Debug, Clone)]
enum FailPoint {
    Never,
    Start(String),
    Progress { stage: String, min_completed: u64 },
}

/// Deterministic progress implementation: records every callback and fails at
/// one configured event. `late_cancel_at_complete` simulates cancellation
/// arriving after the last checkpoint callback but before publication; that
/// callback records the request and still returns `Ok`, which is what the
/// current callback contract allows.
#[derive(Debug)]
struct ProbeProgress {
    fail: FailPoint,
    late_cancel_at_complete: Option<String>,
    events: Mutex<Vec<ProbeEvent>>,
    cancel_requested: AtomicBool,
    callback_count: AtomicUsize,
}

impl ProbeProgress {
    fn never() -> Self {
        Self::new(FailPoint::Never, None)
    }

    fn fail_start(stage: &str) -> Self {
        Self::new(FailPoint::Start(stage.to_string()), None)
    }

    fn fail_progress(stage: &str, min_completed: u64) -> Self {
        Self::new(
            FailPoint::Progress {
                stage: stage.to_string(),
                min_completed,
            },
            None,
        )
    }

    fn late_cancel_at_complete(stage: &str) -> Self {
        Self::new(FailPoint::Never, Some(stage.to_string()))
    }

    fn new(fail: FailPoint, late_cancel_at_complete: Option<String>) -> Self {
        Self {
            fail,
            late_cancel_at_complete,
            events: Mutex::new(Vec::new()),
            cancel_requested: AtomicBool::new(false),
            callback_count: AtomicUsize::new(0),
        }
    }

    fn record(&self, event: ProbeEvent) {
        self.callback_count.fetch_add(1, Ordering::SeqCst);
        self.events.lock().unwrap().push(event);
    }

    fn events(&self) -> Vec<ProbeEvent> {
        self.events.lock().unwrap().clone()
    }

    fn cancel_requested(&self) -> bool {
        self.cancel_requested.load(Ordering::SeqCst)
    }
}

#[async_trait]
impl IndexBuildProgress for ProbeProgress {
    async fn stage_start(
        &self,
        stage: &str,
        total: Option<u64>,
        unit: &str,
    ) -> lance_core::Result<()> {
        self.record(ProbeEvent::Start {
            stage: stage.to_string(),
            total,
            unit: unit.to_string(),
        });
        if let FailPoint::Start(fail_stage) = &self.fail
            && fail_stage == stage
        {
            return Err(probe_cancel("stage_start", stage));
        }
        Ok(())
    }

    async fn stage_progress(&self, stage: &str, completed: u64) -> lance_core::Result<()> {
        self.record(ProbeEvent::Progress {
            stage: stage.to_string(),
            completed,
        });
        if let FailPoint::Progress {
            stage: fail_stage,
            min_completed,
        } = &self.fail
            && fail_stage == stage
            && completed >= *min_completed
        {
            return Err(probe_cancel("stage_progress", stage));
        }
        Ok(())
    }

    async fn stage_complete(&self, stage: &str) -> lance_core::Result<()> {
        self.record(ProbeEvent::Complete {
            stage: stage.to_string(),
        });
        if self.late_cancel_at_complete.as_deref() == Some(stage) {
            self.cancel_requested.store(true, Ordering::SeqCst);
        }
        Ok(())
    }
}

fn assert_external_probe_cancel(err: &Error, expected_marker: &str) {
    match err {
        Error::External { source } => {
            let probe = source
                .downcast_ref::<ProbeCancellation>()
                .unwrap_or_else(|| panic!("External source is not ProbeCancellation: {source:?}"));
            assert_eq!(probe.marker, expected_marker);
            assert_eq!(probe.to_string(), expected_marker);
        }
        other => panic!("expected lance_core::Error::External, got {other:?} ({other})"),
    }
}

fn boundaries(events: &[ProbeEvent]) -> Vec<String> {
    events.iter().map(ProbeEvent::boundary).collect()
}

async fn multi_fragment_dataset() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp
        .path()
        .join("e4-progress-probe")
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
        let batch = RecordBatch::try_new(
            schema.clone(),
            vec![Arc::new(Int32Array::from(ids)), Arc::new(vectors.finish())],
        )
        .unwrap();
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
            &uri,
            Some(params),
        )
        .await
        .unwrap();
    }

    let dataset = Dataset::open(&uri).await.unwrap();
    assert_eq!(dataset.count_fragments(), NUM_FRAGMENTS as usize);
    assert_eq!(dataset.version().version, NUM_FRAGMENTS as u64);
    assert_eq!(dataset.count_rows(None).await.unwrap(), TOTAL_ROWS as usize);
    (tmp, uri)
}

async fn dataset_state(uri: &str) -> (u64, usize) {
    let dataset = Dataset::open(uri).await.unwrap();
    let version = dataset.version().version;
    let indices = dataset.load_indices().await.unwrap().len();
    (version, indices)
}

/// All regular files under `<dataset>/_indices`, as paths relative to the
/// dataset root with their sizes. Sorted for exact assertions.
fn index_tree(uri: &str) -> Vec<(String, u64)> {
    fn walk(root: &Path, dir: &Path, out: &mut Vec<(String, u64)>) {
        let Ok(entries) = std::fs::read_dir(dir) else {
            return;
        };
        for entry in entries {
            let entry = entry.unwrap();
            let path = entry.path();
            let metadata = entry.metadata().unwrap();
            if metadata.is_dir() {
                walk(root, &path, out);
            } else if metadata.is_file() {
                let relative = path
                    .strip_prefix(root)
                    .unwrap()
                    .to_string_lossy()
                    .to_string();
                out.push((format!("_indices/{relative}"), metadata.len()));
            }
        }
    }

    let root = PathBuf::from(uri);
    let mut files = Vec::new();
    walk(&root, &root.join("_indices"), &mut files);
    files.sort();
    files
}

fn vector_params() -> VectorIndexParams {
    VectorIndexParams::ivf_flat(NUM_PARTITIONS, MetricType::L2)
}

fn scalar_params() -> ScalarIndexParams {
    ScalarIndexParams::for_builtin(BuiltinIndexType::BTree)
}

// Scenario 1: the scalar entry point fails at the first legal checkpoint
// (`load_data` stage start), before any scalar build work starts. A committed
// build must not publish a manifest version or index metadata.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn scalar_stage_start_error_prevents_commit() {
    let (_tmp, uri) = multi_fragment_dataset().await;
    let params = scalar_params();
    let progress = Arc::new(ProbeProgress::fail_start("load_data"));

    let mut dataset = Dataset::open(&uri).await.unwrap();
    let before = dataset.version().version;
    let err = dataset
        .create_index_builder(&["id"], IndexType::BTree, &params)
        .name("id_btree".to_string())
        .index_uuid(probe_uuid())
        .progress(progress.clone())
        .await
        .expect_err("progress failure must abort the committed scalar build");

    eprintln!("scalar_stage_start_error_prevents_commit: error={err:?}");
    assert_external_probe_cancel(&err, "e4-probe-cancel:stage_start:load_data");

    let events = progress.events();
    eprintln!(
        "scalar_stage_start_error_prevents_commit: events={:?}",
        boundaries(&events)
    );
    assert_eq!(
        events,
        vec![ProbeEvent::Start {
            stage: "load_data".to_string(),
            total: None,
            unit: "rows".to_string(),
        }]
    );

    let (after, indices) = dataset_state(&uri).await;
    eprintln!(
        "scalar_stage_start_error_prevents_commit: version {before} -> {after}, indices={indices}, tree={:?}",
        index_tree(&uri)
    );
    assert_eq!(after, before, "cancelled scalar build must not commit");
    assert_eq!(indices, 0, "cancelled scalar build must publish no index");
    assert_eq!(
        index_tree(&uri),
        Vec::<(String, u64)>::new(),
        "stage-start failure must write no sidecars"
    );
}

// Scenario 2: successful scalar control, proving the fixture and showing the
// exact checkpoint coverage a progress-driven seam can rely on for BTREE.
// The scalar trainer receives the progress object but emits nothing after
// `load_data`, so the whole training/merge work has no checkpoint.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn scalar_success_control_has_only_load_data_checkpoints() {
    let (_tmp, uri) = multi_fragment_dataset().await;
    let params = scalar_params();
    let progress = Arc::new(ProbeProgress::never());
    let uuid = probe_uuid();

    let mut dataset = Dataset::open(&uri).await.unwrap();
    let before = dataset.version().version;
    let metadata = dataset
        .create_index_builder(&["id"], IndexType::BTree, &params)
        .name("id_btree".to_string())
        .index_uuid(uuid)
        .progress(progress.clone())
        .await
        .expect("scalar control build must succeed");

    let events = progress.events();
    eprintln!("scalar_success_control: events={:?}", boundaries(&events));
    assert_eq!(
        boundaries(&events),
        vec!["start:load_data", "complete:load_data"],
        "BTREE exposes no checkpoints after load_data"
    );
    assert_eq!(metadata.uuid, uuid);
    assert_eq!(metadata.name, "id_btree");
    let files = metadata
        .files
        .as_ref()
        .expect("committed scalar metadata must list its index files");
    assert!(!files.is_empty());
    assert!(files.iter().all(|file| file.size_bytes > 0));

    let (after, indices) = dataset_state(&uri).await;
    let tree = index_tree(&uri);
    eprintln!(
        "scalar_success_control: version {before} -> {after}, indices={indices}, tree={tree:?}"
    );
    assert_eq!(after, before + 1, "committed scalar control publishes");
    assert_eq!(indices, 1);
    let expected_prefix = format!("_indices/{uuid}/");
    assert!(!tree.is_empty());
    assert!(
        tree.iter()
            .all(|(path, size)| path.starts_with(&expected_prefix) && *size > 0)
    );
}

// Scenario 3: a committed vector build fails at the first shuffle progress
// callback, i.e. after real shuffle work has run. The error must abort before
// manifest publication.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn vector_shuffle_error_after_work_prevents_commit() {
    let (_tmp, uri) = multi_fragment_dataset().await;
    let params = vector_params();
    let progress = Arc::new(ProbeProgress::fail_progress("shuffle", 1));

    let mut dataset = Dataset::open(&uri).await.unwrap();
    let before = dataset.version().version;
    let err = dataset
        .create_index_builder(&["embedding"], IndexType::Vector, &params)
        .name("embedding_idx".to_string())
        .index_uuid(probe_uuid())
        .progress(progress.clone())
        .await
        .expect_err("shuffle progress failure must abort the committed vector build");

    eprintln!("vector_shuffle_error_after_work_prevents_commit: error={err:?}");
    assert_external_probe_cancel(&err, "e4-probe-cancel:stage_progress:shuffle");

    let events = progress.events();
    let observed = boundaries(&events);
    eprintln!("vector_shuffle_error_after_work_prevents_commit: events={observed:?}");
    assert_eq!(
        observed,
        vec![
            "start:train_ivf",
            "progress:train_ivf",
            "complete:train_ivf",
            "start:train_quantizer",
            "complete:train_quantizer",
            "start:shuffle",
            "progress:shuffle",
        ],
        "failure must land on the first shuffle progress callback"
    );
    match events.last().unwrap() {
        ProbeEvent::Progress { stage, completed } => {
            assert_eq!(stage, "shuffle");
            assert!(
                *completed >= 1 && *completed <= TOTAL_ROWS,
                "shuffle progress must report real completed rows, got {completed}"
            );
        }
        other => panic!("last event must be shuffle progress, got {other:?}"),
    }
    match &events[5] {
        ProbeEvent::Start { stage, total, .. } => {
            assert_eq!(stage, "shuffle");
            assert_eq!(*total, Some(TOTAL_ROWS));
        }
        other => panic!("expected shuffle stage start, got {other:?}"),
    }

    let (after, indices) = dataset_state(&uri).await;
    let tree = index_tree(&uri);
    eprintln!(
        "vector_shuffle_error_after_work_prevents_commit: version {before} -> {after}, indices={indices}, tree={tree:?}"
    );
    assert_eq!(after, before, "cancelled vector build must not commit");
    assert_eq!(indices, 0, "cancelled vector build must publish no index");
    assert_eq!(
        tree,
        Vec::<(String, u64)>::new(),
        "shuffle failure writes no index sidecars"
    );
}

// Scenario 4: an uncommitted vector build fails at the first
// `merge_partitions` progress callback, after partition build work has
// produced a result. No manifest publication may happen, but the index writer
// has already created sidecar files: they stay behind unreferenced.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn vector_merge_error_after_partition_work_leaves_only_unreferenced_sidecar() {
    let (_tmp, uri) = multi_fragment_dataset().await;
    let params = vector_params();
    let progress = Arc::new(ProbeProgress::fail_progress("merge_partitions", 1));
    let uuid = probe_uuid();

    let mut dataset = Dataset::open(&uri).await.unwrap();
    let before = dataset.version().version;
    let err = dataset
        .create_index_builder(&["embedding"], IndexType::Vector, &params)
        .name("embedding_idx".to_string())
        .index_uuid(uuid)
        .progress(progress.clone())
        .execute_uncommitted()
        .await
        .expect_err("merge progress failure must abort the uncommitted build");

    eprintln!("vector_merge_error_after_partition_work: error={err:?}");
    assert_external_probe_cancel(&err, "e4-probe-cancel:stage_progress:merge_partitions");

    let events = progress.events();
    let observed = boundaries(&events);
    eprintln!("vector_merge_error_after_partition_work: events={observed:?}");
    assert_eq!(
        observed,
        vec![
            "start:train_ivf",
            "progress:train_ivf",
            "complete:train_ivf",
            "start:train_quantizer",
            "complete:train_quantizer",
            "start:shuffle",
            "progress:shuffle",
            "complete:shuffle",
            "start:merge_partitions",
            "progress:merge_partitions",
        ],
        "failure must land on the first merge progress callback after shuffle"
    );
    match events.last().unwrap() {
        ProbeEvent::Progress { stage, completed } => {
            assert_eq!(stage, "merge_partitions");
            assert_eq!(*completed, 1);
        }
        other => panic!("last event must be merge progress, got {other:?}"),
    }

    let (after, indices) = dataset_state(&uri).await;
    let tree = index_tree(&uri);
    eprintln!(
        "vector_merge_error_after_partition_work: version {before} -> {after}, indices={indices}, tree={tree:?}"
    );
    assert_eq!(after, before, "uncommitted failure must not publish");
    assert_eq!(indices, 0, "no index metadata may reference the sidecar");

    let sidecar = format!("_indices/{uuid}/{INDEX_FILE_NAME}");
    assert_eq!(
        tree,
        vec![(sidecar.clone(), 0)],
        "merge failure leaves exactly the empty index writer sidecar"
    );
    assert!(
        Path::new(&uri).join(&sidecar).is_file(),
        "sidecar evidence must exist on disk"
    );
}

// Scenario 5: successful vector control, proving the fixture and the full
// stage boundary sequence, including merge progress for every partition.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn vector_success_control_publishes_after_all_checkpoints() {
    let (_tmp, uri) = multi_fragment_dataset().await;
    let params = vector_params();
    let progress = Arc::new(ProbeProgress::never());
    let uuid = probe_uuid();

    let mut dataset = Dataset::open(&uri).await.unwrap();
    let before = dataset.version().version;
    let metadata = dataset
        .create_index_builder(&["embedding"], IndexType::Vector, &params)
        .name("embedding_idx".to_string())
        .index_uuid(uuid)
        .progress(progress.clone())
        .await
        .expect("vector control build must succeed");

    let events = progress.events();
    let observed = boundaries(&events);
    eprintln!("vector_success_control: events={observed:?}");
    assert_eq!(
        observed,
        vec![
            "start:train_ivf",
            "progress:train_ivf",
            "complete:train_ivf",
            "start:train_quantizer",
            "complete:train_quantizer",
            "start:shuffle",
            "progress:shuffle",
            "complete:shuffle",
            "start:merge_partitions",
            "progress:merge_partitions",
            "progress:merge_partitions",
            "complete:merge_partitions",
        ]
    );
    let merge_completed = events
        .iter()
        .filter_map(|event| match event {
            ProbeEvent::Progress { stage, completed } if stage == "merge_partitions" => {
                Some(*completed)
            }
            _ => None,
        })
        .collect::<Vec<_>>();
    assert_eq!(merge_completed, vec![1, 2]);
    match events.last().unwrap() {
        ProbeEvent::Complete { stage } => assert_eq!(stage, "merge_partitions"),
        other => panic!("build must end with merge_partitions complete, got {other:?}"),
    }
    assert_eq!(metadata.uuid, uuid);
    let files = metadata
        .files
        .as_ref()
        .expect("committed vector metadata must list its index files");
    assert!(!files.is_empty());
    assert!(files.iter().all(|file| file.size_bytes > 0));

    let (after, indices) = dataset_state(&uri).await;
    let tree = index_tree(&uri);
    eprintln!(
        "vector_success_control: version {before} -> {after}, indices={indices}, tree={tree:?}"
    );
    assert_eq!(after, before + 1, "committed vector control publishes");
    assert_eq!(indices, 1);
    assert!(!tree.is_empty());
    assert!(tree.iter().all(|(_, size)| *size > 0));
}

// Scenario 6: the seam gap. A cancellation request that arrives after the
// final checkpoint (`merge_partitions` complete) but before commit is never
// consulted: the callback returns Ok, and the build publishes. This shows the
// native entry point needs an explicit final pre-publication check; a
// progress-callback-only seam cannot close this window.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn late_cancel_after_final_checkpoint_is_not_observed_before_commit() {
    let (_tmp, uri) = multi_fragment_dataset().await;
    let params = vector_params();
    let progress = Arc::new(ProbeProgress::late_cancel_at_complete("merge_partitions"));

    let mut dataset = Dataset::open(&uri).await.unwrap();
    let before = dataset.version().version;
    let metadata = dataset
        .create_index_builder(&["embedding"], IndexType::Vector, &params)
        .name("embedding_idx".to_string())
        .index_uuid(probe_uuid())
        .progress(progress.clone())
        .await
        .expect("build with a late cancel request still publishes");

    let events = progress.events();
    eprintln!(
        "late_cancel: cancel_requested={}, last={:?}, events={:?}",
        progress.cancel_requested(),
        events.last(),
        boundaries(&events)
    );
    assert!(
        progress.cancel_requested(),
        "probe must have requested cancellation at the final checkpoint"
    );
    assert!(matches!(
        events.last(),
        Some(ProbeEvent::Complete { stage }) if stage == "merge_partitions"
    ));
    assert_eq!(metadata.uuid, probe_uuid());

    let (after, indices) = dataset_state(&uri).await;
    eprintln!("late_cancel: version {before} -> {after}, indices={indices}");
    assert_eq!(
        after,
        before + 1,
        "the entry point commits despite the unobserved late cancellation request"
    );
    assert_eq!(indices, 1);
}
