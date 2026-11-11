// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Validation-only probe for native cross-store clone behavior at Lance
//! v14.0.2 (`40f15ed1b71b66950961b175829fdf17a763f108`).
//!
//! The probe uses two independent `object_store::memory::InMemory` instances,
//! pinned explicitly through the native `ObjectStoreParams`. Store A holds the
//! source dataset; store B is the target store for both clones.
//!
//! The deep-clone control establishes that native `Dataset::deep_clone` writes
//! the target manifest and copies data into B. The shallow-clone case then
//! exercises `Dataset::shallow_clone` with B parameters and asserts the
//! contract that the new target manifest is persisted in B. The assertion is
//! expected to fail while the native shallow path routes its commit through the
//! source store.
//!
//! Run explicitly with:
//! `cargo test --test native_cross_store_clone_probe -- --ignored --nocapture`

use std::sync::Arc;

use arrow::record_batch::RecordBatchIterator;
use arrow_array::{Int32Array, RecordBatch};
use arrow_schema::{DataType, Field, Schema};
use futures::TryStreamExt;
use lance::Dataset;
use lance::dataset::builder::DatasetBuilder;
use lance::dataset::{ReadParams, WriteParams};
use lance_io::object_store::{ObjectStoreParams, uri_to_url};
use lance_table::io::commit::ConditionalPutCommitHandler;
use object_store::memory::InMemory;
use object_store::path::Path as ObjectPath;
use object_store::{ObjectMeta, ObjectStore};

const SOURCE_URI: &str = "memory://source-a/source";
const SOURCE_BASE_URL: &str = "memory:///source-a/source";
const DEEP_URI: &str = "memory://target-b/deep-target";
const DEEP_BASE_URL: &str = "memory:///target-b/deep-target";
const SHALLOW_URI: &str = "memory://target-b/shallow-target";
const SHALLOW_BASE_URL: &str = "memory:///target-b/shallow-target";

const SOURCE_PREFIX: &str = "source-a/source";
const DEEP_PREFIX: &str = "target-b/deep-target";
const SHALLOW_PREFIX: &str = "target-b/shallow-target";

fn store_params(store: &Arc<InMemory>, base_url: &str) -> ObjectStoreParams {
    let inner: Arc<dyn ObjectStore> = store.clone();
    #[allow(deprecated)]
    let params = ObjectStoreParams {
        object_store: Some((
            inner,
            uri_to_url(base_url).expect("probe base URL must parse"),
        )),
        ..Default::default()
    };
    params
}

fn read_params(store: &Arc<InMemory>, base_url: &str) -> ReadParams {
    ReadParams {
        store_options: Some(store_params(store, base_url)),
        commit_handler: Some(Arc::new(ConditionalPutCommitHandler)),
        ..Default::default()
    }
}

fn batch(start: i32, rows: i32, schema: Arc<Schema>) -> RecordBatch {
    let ids = Int32Array::from_iter_values(start..start + rows);
    RecordBatch::try_new(schema, vec![Arc::new(ids)]).expect("probe batch must match schema")
}

async fn objects(store: &Arc<InMemory>, prefix: &str) -> Vec<ObjectMeta> {
    let prefix = ObjectPath::from(prefix);
    store
        .list(Some(&prefix))
        .try_collect::<Vec<_>>()
        .await
        .expect("probe listing on in-memory store must succeed")
}

fn describe(objects: &[ObjectMeta]) -> Vec<String> {
    objects
        .iter()
        .map(|meta| {
            format!(
                "{} ({} bytes, etag={:?})",
                meta.location, meta.size, meta.e_tag
            )
        })
        .collect()
}

fn has_manifest(objects: &[ObjectMeta]) -> bool {
    objects.iter().any(|meta| {
        let key = meta.location.as_ref();
        key.contains("/_manifests/") || key.contains("/_versions/")
    })
}

fn has_data_file(objects: &[ObjectMeta]) -> bool {
    objects
        .iter()
        .any(|meta| meta.location.as_ref().contains("/data/"))
}

async fn read_ids(dataset: &Dataset) -> Vec<i32> {
    let batch = dataset
        .scan()
        .try_into_batch()
        .await
        .expect("probe scan must succeed");
    let ids = batch
        .column(0)
        .as_any()
        .downcast_ref::<Int32Array>()
        .expect("probe dataset must expose the id column");
    ids.values().to_vec()
}

#[tokio::test(flavor = "multi_thread", worker_threads = 1)]
#[ignore = "native cross-store clone discriminator; run explicitly"]
async fn shallow_clone_with_target_store_params_must_persist_manifest_in_target() {
    let schema = Arc::new(Schema::new(vec![Field::new("id", DataType::Int32, false)]));
    let store_a = Arc::new(InMemory::new());
    let store_b = Arc::new(InMemory::new());

    let source_params = store_params(&store_a, SOURCE_BASE_URL);
    let mut source = Dataset::write(
        RecordBatchIterator::new(vec![Ok(batch(0, 8, schema.clone()))], schema.clone()),
        SOURCE_URI,
        Some(WriteParams {
            max_rows_per_file: 4,
            max_rows_per_group: 4,
            store_params: Some(source_params.clone()),
            commit_handler: Some(Arc::new(ConditionalPutCommitHandler)),
            ..Default::default()
        }),
    )
    .await
    .expect("setup: source dataset must be written into store A");

    assert!(
        source.count_fragments() >= 2,
        "setup: source dataset must have multiple fragments, got {}",
        source.count_fragments()
    );
    assert_eq!(
        source
            .count_rows(None)
            .await
            .expect("setup: source row count"),
        8,
        "setup: source dataset must contain 8 rows"
    );
    let source_objects_before = objects(&store_a, SOURCE_PREFIX).await;
    eprintln!("PROBE source_keys={:?}", describe(&source_objects_before));
    assert!(
        has_manifest(&source_objects_before),
        "setup: source manifest must exist in store A, keys={:?}",
        describe(&source_objects_before)
    );
    assert!(
        has_data_file(&source_objects_before),
        "setup: source data files must exist in store A, keys={:?}",
        describe(&source_objects_before)
    );

    // Control: deep_clone must resolve the supplied target params into store B,
    // copy data there, and leave the source dataset intact.
    let deep_params = store_params(&store_b, DEEP_BASE_URL);
    let deep = source
        .deep_clone(DEEP_URI, source.version().version, Some(deep_params))
        .await
        .expect("control: native deep_clone into distinct target store B must succeed");
    let deep_uri = deep.uri().to_string();
    let deep_version = deep.version().version;
    let deep_rows = deep
        .count_rows(None)
        .await
        .expect("control: returned deep clone must be readable");
    let deep_ids = read_ids(&deep).await;

    let a_deep_objects = objects(&store_a, DEEP_PREFIX).await;
    let b_deep_objects = objects(&store_b, DEEP_PREFIX).await;
    eprintln!("PROBE A_deep_keys={:?}", describe(&a_deep_objects));
    eprintln!("PROBE B_deep_keys={:?}", describe(&b_deep_objects));
    assert!(
        has_manifest(&b_deep_objects),
        "control: deep clone target manifest must exist in store B, B keys={:?}",
        describe(&b_deep_objects)
    );
    assert!(
        has_data_file(&b_deep_objects),
        "control: deep clone data must be copied into store B, B keys={:?}",
        describe(&b_deep_objects)
    );
    assert!(
        a_deep_objects.is_empty(),
        "control: deep clone must not write under the target path in source store A, A keys={:?}",
        describe(&a_deep_objects)
    );
    assert!(
        !has_manifest(&a_deep_objects) && !has_data_file(&a_deep_objects),
        "control: source store A must remain free of deep clone target objects"
    );
    assert_eq!(deep_rows, 8, "control: deep clone must read 8 rows");
    assert_eq!(
        deep_ids,
        (0..8).collect::<Vec<_>>(),
        "control: deep clone values must match the source"
    );

    let reopened_deep = DatasetBuilder::from_uri(DEEP_URI)
        .with_read_params(read_params(&store_b, DEEP_BASE_URL))
        .load()
        .await
        .expect("control: deep clone target must reopen from store B");
    assert_eq!(
        reopened_deep.version().version,
        deep_version,
        "control: reopened deep clone version must match the returned dataset"
    );
    assert_eq!(
        read_ids(&reopened_deep).await,
        (0..8).collect::<Vec<_>>(),
        "control: reopened deep clone must read the expected values from B"
    );

    let source_objects_after_deep = objects(&store_a, SOURCE_PREFIX).await;
    assert_eq!(
        describe(&source_objects_after_deep),
        describe(&source_objects_before),
        "control: deep clone must not remove source objects from store A"
    );
    assert_eq!(
        read_ids(&source).await,
        (0..8).collect::<Vec<_>>(),
        "control: source dataset must remain readable after deep clone"
    );

    // Discriminator: shallow_clone receives target-store parameters for B at an
    // absent target path. The caller contract is that the new target manifest
    // and transaction live in B.
    let shallow_params = store_params(&store_b, SHALLOW_BASE_URL);
    let shallow_result = source
        .shallow_clone(SHALLOW_URI, source.version().version, Some(shallow_params))
        .await;
    let (shallow_status, shallow_uri, shallow_rows_via_returned) = match shallow_result {
        Ok(dataset) => {
            let rows = dataset.count_rows(None).await;
            (
                format!("Ok(version={})", dataset.version().version),
                Some(dataset.uri().to_string()),
                Some(format!("{rows:?}")),
            )
        }
        Err(error) => (format!("Err({error})"), None, None),
    };

    let a_shallow_objects = objects(&store_a, SHALLOW_PREFIX).await;
    let b_shallow_objects = objects(&store_b, SHALLOW_PREFIX).await;

    let reopen_from_b = DatasetBuilder::from_uri(SHALLOW_URI)
        .with_read_params(read_params(&store_b, SHALLOW_BASE_URL))
        .load()
        .await
        .map(|dataset| dataset.version().version);
    let reopen_from_a = DatasetBuilder::from_uri(SHALLOW_URI)
        .with_read_params(read_params(&store_a, SHALLOW_BASE_URL))
        .load()
        .await
        .map(|dataset| dataset.version().version);

    let source_objects_after_shallow = objects(&store_a, SOURCE_PREFIX).await;
    let source_rows_after_shallow = source.count_rows(None).await;

    eprintln!("PROBE shallow_result={shallow_status}");
    eprintln!("PROBE shallow_uri={shallow_uri:?}");
    eprintln!("PROBE shallow_rows_via_returned={shallow_rows_via_returned:?}");
    eprintln!("PROBE A_shallow_keys={:?}", describe(&a_shallow_objects));
    eprintln!("PROBE B_shallow_keys={:?}", describe(&b_shallow_objects));
    eprintln!("PROBE reopen_from_B={reopen_from_b:?}");
    eprintln!("PROBE reopen_from_A={reopen_from_a:?}");
    eprintln!(
        "PROBE source_objects_unchanged_after_shallow={}",
        describe(&source_objects_after_shallow) == describe(&source_objects_before)
    );
    eprintln!("PROBE source_rows_after_shallow={source_rows_after_shallow:?}");
    eprintln!(
        "PROBE A_has_shallow_manifest={} B_has_shallow_manifest={}",
        has_manifest(&a_shallow_objects),
        has_manifest(&b_shallow_objects)
    );
    eprintln!("PROBE deep_uri={deep_uri} deep_version={deep_version} deep_rows={deep_rows}");

    assert!(
        has_manifest(&b_shallow_objects),
        "cross-store shallow_clone did not persist a target manifest in store B: \
         shallow_result={shallow_status}, shallow_uri={shallow_uri:?}, \
         A_shallow_keys={:?}, B_shallow_keys={:?}, reopen_from_B={reopen_from_b:?}, \
         reopen_from_A={reopen_from_a:?}, deep_control_uri={deep_uri}, deep_control_version={deep_version}",
        describe(&a_shallow_objects),
        describe(&b_shallow_objects),
    );
    assert!(
        !has_manifest(&a_shallow_objects),
        "cross-store shallow_clone wrote its target manifest into source store A: \
         A_shallow_keys={:?}, B_shallow_keys={:?}",
        describe(&a_shallow_objects),
        describe(&b_shallow_objects),
    );
}
