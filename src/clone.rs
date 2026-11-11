// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Native dataset deep clone.
//!
//! The C entry point delegates the whole operation to Lance's
//! `Dataset::deep_clone`. This module owns the FFI translation: source snapshot
//! capture, NULL-reference defaulting, and passing the target storage-options
//! accessor through as a native `ObjectStoreParams`.

use std::ffi::{c_char, c_int};
use std::sync::{Arc, RwLock};

use lance::Dataset;
use lance::dataset::refs::Ref;
use lance_core::Result;
use lance_io::object_store::ObjectStoreParams;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::file_session::storage_options_from_ffi;
use crate::helpers;
use crate::runtime::block_on;
use crate::storage_options::LanceStorageOptionsAccessor;
use crate::versions::{LanceRef, reference_at};

fn invalid(message: impl Into<String>) -> lance_core::Error {
    lance_core::Error::invalid_input(message.into())
}

/// Deep clone the dataset's current snapshot into `target_path`.
///
/// The source handle is not modified and no source write lock is held while
/// files are copied: the handle's snapshot `Arc` is cloned and the native
/// `Dataset` value is cloned before `Dataset::deep_clone` runs.
///
/// - `reference == NULL` clones the source handle's current snapshot, using
///   `Ref::Version(snapshot.manifest.branch, Some(snapshot.version_id()))`.
///   This pins the snapshot; it does not select the latest head of the branch.
/// - `target_storage_options == NULL` passes native `None`. A non-NULL accessor
///   that holds static options and/or a provider contributes its native `Arc`
///   to `ObjectStoreParams`; an accessor created with neither contributes
///   `None`. Credentials are not resolved or copied here.
///
/// The out handle is written only on success. On failure, native copies may
/// leave partial objects under `target_path`, while the target dataset itself
/// is published only by the final manifest commit. A retry then fails the
/// native target-exists preflight until those objects are removed.
///
/// For datasets with additional base paths, source credentials come from the
/// source dataset's native session and per-base store binding. This function
/// does not construct extra providers, caches, registries, or per-base
/// credential sets.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_deep_clone(
    dataset: *const LanceDataset,
    target_path: *const c_char,
    reference: *const LanceRef,
    target_storage_options: *const LanceStorageOptionsAccessor,
    out: *mut *mut LanceDataset,
) -> c_int {
    ffi_try!(
        unsafe { deep_clone_inner(dataset, target_path, reference, target_storage_options, out,) },
        -1
    )
}

unsafe fn deep_clone_inner(
    dataset: *const LanceDataset,
    target_path: *const c_char,
    reference: *const LanceRef,
    target_storage_options: *const LanceStorageOptionsAccessor,
    out: *mut *mut LanceDataset,
) -> Result<c_int> {
    if out.is_null() {
        return Err(invalid("out must not be NULL"));
    }
    if dataset.is_null() {
        return Err(invalid("dataset must not be NULL"));
    }
    let target_path = unsafe { helpers::parse_c_string(target_path)? }
        .ok_or_else(|| invalid("target_path must not be NULL"))?;

    let snapshot = unsafe { &*dataset }.snapshot();
    let reference = if reference.is_null() {
        Ref::Version(
            snapshot.manifest.branch.clone(),
            Some(snapshot.version_id()),
        )
    } else {
        unsafe { reference_at(reference)? }.clone()
    };
    // Reuse the accessor's Arc. It composes whatever static options or dynamic
    // provider the caller configured; nothing is snapshotted into credentials.
    let store_params =
        storage_options_from_ffi(target_storage_options).map(|accessor| ObjectStoreParams {
            storage_options_accessor: Some(accessor),
            ..ObjectStoreParams::default()
        });

    // The mutable value is a private shallow clone. `deep_clone` may check out
    // a different ref on it, which must not be visible through the source
    // handle.
    let mut source = Dataset::clone(&*snapshot);
    let cloned = block_on(source.deep_clone(target_path, reference, store_params))?;
    let handle = Box::into_raw(Box::new(LanceDataset {
        inner: RwLock::new(Arc::new(cloned)),
    }));
    unsafe { *out = handle };
    Ok(0)
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;
    use std::ffi::CString;
    use std::sync::Arc;

    use arrow_array::{Int32Array, RecordBatch, RecordBatchIterator, StringArray, StructArray};
    use arrow_schema::{DataType, Field, Schema};
    use futures::TryStreamExt;
    use lance::Dataset;
    use lance::dataset::WriteParams;
    use lance::dataset::builder::DatasetBuilder;
    use lance::dataset::refs::Ref;
    use lance::session::Session;
    use lance_io::object_store::providers::shared_memory::SharedMemoryStoreProvider;
    use lance_io::object_store::{ObjectStore, ObjectStoreParams, ObjectStoreRegistry};
    use object_store::path::Path as ObjectPath;
    use uuid::Uuid;

    use super::*;
    use crate::dataset::{
        lance_dataset_close, lance_dataset_count_rows, lance_dataset_open_with_session,
        lance_dataset_version,
    };
    use crate::error::{LanceErrorCode, lance_last_error_code};
    use crate::session::LanceSession;
    use crate::versions::{
        lance_ref_close, lance_ref_tag, lance_ref_version, lance_ref_version_number,
    };

    const SRC_SCHEME: &str = "clone-src";
    const DST_SCHEME: &str = "clone-dst";

    fn c_string(value: &str) -> CString {
        CString::new(value).unwrap()
    }

    fn registry_with(schemes: &[&str]) -> ObjectStoreRegistry {
        let registry = ObjectStoreRegistry::default();
        for scheme in schemes {
            registry.insert(scheme, Arc::new(SharedMemoryStoreProvider::default()));
        }
        registry
    }

    fn uri(scheme: &str, authority: &str, name: &str) -> String {
        format!("{scheme}://{authority}/{name}")
    }

    fn label_field() -> Field {
        Field::new(
            "label",
            DataType::Struct(
                vec![
                    Field::new("code", DataType::Utf8, true),
                    Field::new("rank", DataType::Int32, true),
                ]
                .into(),
            ),
            true,
        )
    }

    fn batch(
        schema: Arc<Schema>,
        ids: Vec<i32>,
        codes: Vec<Option<&str>>,
        ranks: Vec<Option<i32>>,
    ) -> RecordBatch {
        let labels = StructArray::new(
            vec![
                Field::new("code", DataType::Utf8, true),
                Field::new("rank", DataType::Int32, true),
            ]
            .into(),
            vec![
                Arc::new(StringArray::from(codes)),
                Arc::new(Int32Array::from(ranks)),
            ],
            None,
        );
        RecordBatch::try_new(
            schema,
            vec![Arc::new(Int32Array::from(ids)), Arc::new(labels)],
        )
        .unwrap()
    }

    /// Native fixture: three fragments, one deletion, nested nullable fields
    /// and schema metadata.
    fn write_source(session: Arc<Session>, uri: &str) -> Dataset {
        let schema = Arc::new(
            Schema::new(vec![
                Field::new("id", DataType::Int32, false),
                label_field(),
            ])
            .with_metadata(HashMap::from([(
                "fixture".to_string(),
                "deep-clone".to_string(),
            )])),
        );
        let batches = vec![
            batch(
                schema.clone(),
                vec![0, 1],
                vec![Some("a"), None],
                vec![Some(0), None],
            ),
            batch(
                schema.clone(),
                vec![2, 3],
                vec![Some("b"), Some("c")],
                vec![Some(2), None],
            ),
            batch(
                schema.clone(),
                vec![4, 5],
                vec![None, Some("e")],
                vec![Some(4), Some(5)],
            ),
        ];
        let params = WriteParams {
            max_rows_per_file: 2,
            enable_stable_row_ids: true,
            session: Some(session),
            ..WriteParams::default()
        };
        let mut dataset = block_on(Dataset::write(
            RecordBatchIterator::new(batches.into_iter().map(Ok), schema),
            uri,
            Some(params),
        ))
        .unwrap();
        assert_eq!(block_on(dataset.count_rows(None)).unwrap(), 6);
        block_on(dataset.delete("id = 3")).unwrap();
        assert_eq!(block_on(dataset.count_rows(None)).unwrap(), 5);
        dataset
    }

    fn open_with(uri: &str, session: Arc<Session>) -> Dataset {
        block_on(DatasetBuilder::from_uri(uri).with_session(session).load()).unwrap()
    }

    fn rows(dataset: &Dataset) -> Vec<(i32, Option<String>, Option<i32>)> {
        let mut scanner = dataset.scan();
        scanner.project(&["id", "label"]).unwrap();
        let batch = block_on(scanner.try_into_batch()).unwrap();
        let ids = batch
            .column_by_name("id")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        let labels = batch
            .column_by_name("label")
            .unwrap()
            .as_any()
            .downcast_ref::<StructArray>()
            .unwrap();
        let codes = labels
            .column_by_name("code")
            .unwrap()
            .as_any()
            .downcast_ref::<StringArray>()
            .unwrap();
        let ranks = labels
            .column_by_name("rank")
            .unwrap()
            .as_any()
            .downcast_ref::<Int32Array>()
            .unwrap();
        let mut out: Vec<_> = (0..batch.num_rows())
            .map(|row| {
                (
                    ids.value(row),
                    codes.is_valid(row).then(|| codes.value(row).to_string()),
                    ranks.is_valid(row).then(|| ranks.value(row)),
                )
            })
            .collect();
        out.sort();
        out
    }

    fn list_objects(store: &ObjectStore, prefix: &ObjectPath) -> Vec<String> {
        let entries: Vec<_> = block_on(store.list(Some(prefix)).try_collect()).unwrap();
        entries
            .into_iter()
            .map(|meta| meta.location.to_string())
            .collect()
    }

    fn store(registry: Arc<ObjectStoreRegistry>, uri: &str) -> ObjectStore {
        block_on(ObjectStore::from_uri_and_params(
            registry,
            uri,
            &ObjectStoreParams::default(),
        ))
        .unwrap()
        .0
    }

    fn open_c(dataset_uri: &str, session: &Session) -> *mut LanceDataset {
        let uri = c_string(dataset_uri);
        let handle = LanceSession {
            inner: Arc::new(session.clone()),
        };
        unsafe {
            lance_dataset_open_with_session(
                uri.as_ptr(),
                std::ptr::null(),
                0,
                &handle as *const LanceSession,
            )
        }
    }

    #[test]
    fn deep_clone_cross_store_defaults_to_source_snapshot() {
        let suffix = Uuid::new_v4().simple().to_string();
        let src_uri = uri(SRC_SCHEME, &format!("src-{suffix}"), "source.lance");
        let dst_uri = uri(DST_SCHEME, &format!("dst-{suffix}"), "target.lance");

        let registry = Arc::new(registry_with(&[SRC_SCHEME, DST_SCHEME]));
        let session = Arc::new(Session::new(0, 0, registry.clone()));
        let source = write_source(session.clone(), &src_uri);
        let source_version = source.version_id();
        let expected = rows(&source);

        let dataset = open_c(&src_uri, &session);
        assert!(!dataset.is_null(), "source open failed");
        // Advance the source after the C handle was opened. The handle keeps
        // its snapshot; a NULL reference must clone that snapshot, not latest.
        let appended_schema = source.schema().clone();
        let mut advanced = Dataset::clone(&source);
        block_on(advanced.append(
            RecordBatchIterator::new(
                vec![Ok(batch(
                    appended_schema.clone(),
                    vec![10],
                    vec![Some("z")],
                    vec![Some(10)],
                ))],
                appended_schema,
            ),
            None,
        ))
        .unwrap();
        assert_eq!(block_on(advanced.count_rows(None)).unwrap(), 6);
        assert_eq!(unsafe { lance_dataset_version(dataset) }, source_version);

        let dst_c = c_string(&dst_uri);
        let mut cloned: *mut LanceDataset = std::ptr::null_mut();
        let rc = unsafe {
            lance_dataset_deep_clone(
                dataset,
                dst_c.as_ptr(),
                std::ptr::null(),
                std::ptr::null(),
                &mut cloned,
            )
        };
        assert_eq!(rc, 0, "clone failed");
        assert!(!cloned.is_null());
        // A fresh target publishes its own first version.
        assert_eq!(unsafe { lance_dataset_version(cloned) }, 1);
        assert_eq!(unsafe { lance_dataset_count_rows(cloned) }, 5);
        assert_eq!(unsafe { lance_dataset_version(dataset) }, source_version);

        // Close both handles, then reopen the target through a fresh session
        // that cannot resolve the source scheme.
        unsafe {
            lance_dataset_close(cloned);
            lance_dataset_close(dataset);
        }
        let fresh = Arc::new(Session::new(0, 0, Arc::new(registry_with(&[DST_SCHEME]))));
        let reopened = open_with(&dst_uri, fresh.clone());
        assert_eq!(reopened.version_id(), 1);
        assert_eq!(block_on(reopened.count_rows(None)).unwrap(), 5);
        assert_eq!(rows(&reopened), expected);
        assert_eq!(reopened.schema().metadata(), source.schema().metadata());
        assert!(
            block_on(
                DatasetBuilder::from_uri(&src_uri)
                    .with_session(fresh)
                    .load()
            )
            .is_err(),
            "fresh session must not resolve the source scheme"
        );

        // Native control on the same source snapshot.
        let control_uri = uri(DST_SCHEME, &format!("control-{suffix}"), "control.lance");
        let mut control_source = Dataset::clone(&source);
        let control = block_on(control_source.deep_clone(
            &control_uri,
            Ref::Version(source.manifest.branch.clone(), Some(source_version)),
            None,
        ))
        .unwrap();
        assert_eq!(block_on(control.count_rows(None)).unwrap(), 5);
        assert_eq!(rows(&control), expected);

        let store_a = store(registry.clone(), &src_uri);
        let store_b = store(registry, &dst_uri);
        let target_prefix = ObjectPath::from("target.lance");
        let source_prefix = ObjectPath::from("source.lance");
        assert!(
            list_objects(&store_a, &target_prefix).is_empty(),
            "source store has target objects"
        );
        assert!(
            !list_objects(&store_a, &source_prefix).is_empty(),
            "source objects missing"
        );
        let target_objects = list_objects(&store_b, &target_prefix);
        assert!(
            target_objects.len() >= 2,
            "target store lacks manifest+data: {target_objects:?}"
        );
        assert!(
            target_objects.iter().any(|path| path.contains("manifest")),
            "target manifest missing: {target_objects:?}"
        );
    }

    #[test]
    fn deep_clone_refs_and_errors_are_native() {
        let suffix = Uuid::new_v4().simple().to_string();
        let src_uri = uri(SRC_SCHEME, &format!("src-{suffix}"), "source.lance");
        let dst_root = format!("dst-{suffix}");
        let registry = Arc::new(registry_with(&[SRC_SCHEME, DST_SCHEME]));
        let session = Arc::new(Session::new(0, 0, registry));
        let source = write_source(session.clone(), &src_uri);
        assert_eq!(source.version_id(), 2);

        let dataset = open_c(&src_uri, &session);
        assert!(!dataset.is_null());

        // Explicit version ref clones that historical snapshot.
        let version_ref = unsafe { lance_ref_version_number(1) };
        let dst_v1 = uri(DST_SCHEME, &dst_root, "v1.lance");
        let dst_v1_c = c_string(&dst_v1);
        let mut out_v1: *mut LanceDataset = std::ptr::null_mut();
        assert_eq!(
            unsafe {
                lance_dataset_deep_clone(
                    dataset,
                    dst_v1_c.as_ptr(),
                    version_ref,
                    std::ptr::null(),
                    &mut out_v1,
                )
            },
            0
        );
        assert_eq!(unsafe { lance_dataset_count_rows(out_v1) }, 6);
        assert_eq!(unsafe { lance_dataset_version(out_v1) }, 1);

        // Explicit main head ref (Version(None, None)).
        let main_head = unsafe { lance_ref_version(std::ptr::null(), false, 0) };
        let dst_head = uri(DST_SCHEME, &dst_root, "head.lance");
        let dst_head_c = c_string(&dst_head);
        let mut out_head: *mut LanceDataset = std::ptr::null_mut();
        assert_eq!(
            unsafe {
                lance_dataset_deep_clone(
                    dataset,
                    dst_head_c.as_ptr(),
                    main_head,
                    std::ptr::null(),
                    &mut out_head,
                )
            },
            0
        );
        assert_eq!(
            unsafe { lance_dataset_count_rows(out_head) },
            block_on(source.count_rows(None)).unwrap()
        );

        // Missing tag fails through the native ref resolver.
        let missing = c_string("missing");
        let bad_tag = unsafe { lance_ref_tag(missing.as_ptr()) };
        let dst_bad = uri(DST_SCHEME, &dst_root, "bad.lance");
        let dst_bad_c = c_string(&dst_bad);
        let mut sentinel: *mut LanceDataset = std::ptr::null_mut();
        let rc = unsafe {
            lance_dataset_deep_clone(
                dataset,
                dst_bad_c.as_ptr(),
                bad_tag,
                std::ptr::null(),
                &mut sentinel,
            )
        };
        assert_eq!(rc, -1);
        assert!(sentinel.is_null(), "out written on failure");
        assert_eq!(lance_last_error_code(), LanceErrorCode::NotFound);

        // Existing target is rejected by the native preflight.
        let mut existing: *mut LanceDataset = std::ptr::null_mut();
        assert_eq!(
            unsafe {
                lance_dataset_deep_clone(
                    dataset,
                    dst_v1_c.as_ptr(),
                    std::ptr::null(),
                    std::ptr::null(),
                    &mut existing,
                )
            },
            -1
        );
        assert!(existing.is_null());
        assert_eq!(
            lance_last_error_code(),
            LanceErrorCode::DatasetAlreadyExists
        );

        // NULL arguments are rejected before native work starts.
        assert_eq!(
            unsafe {
                lance_dataset_deep_clone(
                    std::ptr::null(),
                    dst_bad_c.as_ptr(),
                    std::ptr::null(),
                    std::ptr::null(),
                    &mut existing,
                )
            },
            -1
        );
        assert_eq!(
            unsafe {
                lance_dataset_deep_clone(
                    dataset,
                    std::ptr::null(),
                    std::ptr::null(),
                    std::ptr::null(),
                    &mut existing,
                )
            },
            -1
        );
        assert_eq!(
            unsafe {
                lance_dataset_deep_clone(
                    dataset,
                    dst_bad_c.as_ptr(),
                    std::ptr::null(),
                    std::ptr::null(),
                    std::ptr::null_mut(),
                )
            },
            -1
        );
        assert!(existing.is_null());

        unsafe {
            lance_ref_close(version_ref);
            lance_ref_close(main_head);
            lance_ref_close(bad_tag);
            lance_dataset_close(out_v1);
            lance_dataset_close(out_head);
            lance_dataset_close(dataset);
        }
    }
}
