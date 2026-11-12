// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

use std::cell::Cell;
use std::collections::HashMap;

use futures::{FutureExt, channel::oneshot, poll};

use super::*;

fn segment(id: u128, field_id: Option<i32>) -> IndexMetadata {
    IndexMetadata {
        uuid: Uuid::from_u128(id),
        fields: field_id.into_iter().collect(),
        covering_fields: Vec::new(),
        name: "id".into(),
        dataset_version: 1,
        fragment_bitmap: None,
        index_details: None,
        index_version: 0,
        created_at: None,
        base_id: None,
        files: None,
    }
}

#[test]
fn index_segments_json_distinguishes_unknown_empty_and_historical_coverage() {
    let mut unknown = segment(1, Some(0));
    unknown.name = "unknown".to_string();
    unknown.dataset_version = 7;
    unknown.index_version = 3;

    let mut empty = segment(2, Some(0));
    empty.name = "empty".to_string();
    empty.fragment_bitmap = Some(Default::default());

    let mut historical = segment(3, Some(1));
    historical.name = "quote\"backslash\\newline\n".to_string();
    historical.fragment_bitmap = Some([2_u32, 9].into_iter().collect());
    historical.index_version = 5;
    historical.fields.push(0);
    historical.covering_fields.push(0);

    let mut system = segment(4, None);
    system.name = lance_table::system_index::frag_reuse::FRAG_REUSE_INDEX_NAME.to_string();

    let columns = HashMap::from([(0, "id".to_string()), (1, "quote\"column\\".to_string())]);
    let json = serialize_index_segments_json(&[unknown, empty, historical, system], |field_id| {
        columns.get(&field_id).cloned()
    })
    .unwrap();
    let entries: serde_json::Value = serde_json::from_str(&json).unwrap();
    let entries = entries.as_array().unwrap();
    assert_eq!(entries.len(), 3, "system segments must be excluded");

    let unknown = entries
        .iter()
        .find(|entry| entry["name"] == "unknown")
        .unwrap();
    assert!(unknown["fragment_ids"].is_null());
    assert_eq!(unknown["index_version"], 3);
    assert_eq!(unknown["dataset_version"], 7);

    let empty = entries
        .iter()
        .find(|entry| entry["name"] == "empty")
        .unwrap();
    assert_eq!(empty["fragment_ids"], serde_json::json!([]));

    let historical = entries
        .iter()
        .find(|entry| entry["name"] == "quote\"backslash\\newline\n")
        .unwrap();
    assert_eq!(historical["fragment_ids"], serde_json::json!([2, 9]));
    assert_eq!(historical["index_version"], 5);
    assert_eq!(
        historical["columns"],
        serde_json::json!(["quote\"column\\"])
    );
    let expected_uuid = Uuid::from_u128(3).to_string();
    assert_eq!(historical["uuid"], expected_uuid);
}

#[test]
fn index_segments_json_preserves_vector_subtypes() {
    use lance_index::pb::VectorIndexDetails;
    use lance_index::pb::vector_index_details::Compression;

    let cases = [
        (false, Compression::Flat(Default::default()), "IVF_FLAT"),
        (true, Compression::Flat(Default::default()), "IVF_HNSW_FLAT"),
        (false, Compression::Pq(Default::default()), "IVF_PQ"),
        (true, Compression::Pq(Default::default()), "IVF_HNSW_PQ"),
        (false, Compression::Sq(Default::default()), "IVF_SQ"),
        (true, Compression::Sq(Default::default()), "IVF_HNSW_SQ"),
        (false, Compression::Rq(Default::default()), "IVF_RQ"),
    ];
    for (hnsw, compression, expected) in cases {
        let protobuf = VectorIndexDetails {
            compression: Some(compression),
            hnsw_index_config: hnsw.then(Default::default),
            ..Default::default()
        };
        let mut index = segment(1, Some(0));
        index.index_details = Some(Default::default());
        let details = Arc::make_mut(index.index_details.as_mut().unwrap());
        details.type_url = "/lance.index.pb.VectorIndexDetails".to_string();
        details.value = protobuf.encode_to_vec();
        assert_eq!(serialized_index_type(&index), expected);
        Arc::make_mut(index.index_details.as_mut().unwrap())
            .value
            .clear();
        assert_eq!(serialized_index_type(&index), "Vector");
    }
}

#[test]
fn prewarm_colliding_segments_opens_and_warms_concurrently() {
    futures::executor::block_on(async {
        const SEGMENTS: usize = 8;
        let opening = Cell::new(0);
        let warming = Cell::new(0);
        let (release_open, open_gate) = oneshot::channel::<()>();
        let (release_warm, warm_gate) = oneshot::channel::<()>();
        let open_gate = open_gate.shared();
        let warm_gate = warm_gate.shared();
        let indices = (0..SEGMENTS)
            .map(|id| segment(id as u128, Some(1)))
            .collect();
        let mut prewarm = Box::pin(prewarm_colliding_segments(indices, 0, |field_id, _| {
            assert_eq!(field_id, 1);
            let open_gate = open_gate.clone();
            let warm_gate = warm_gate.clone();
            let opening = &opening;
            let warming = &warming;
            async move {
                opening.set(opening.get() + 1);
                open_gate.await.unwrap();
                warming.set(warming.get() + 1);
                warm_gate.await.unwrap();
                Ok(true)
            }
        }));

        // Gates make this independent of storage speed and scheduler timing:
        // every open must start before any open is allowed to finish.
        assert!(poll!(&mut prewarm).is_pending());
        assert_eq!(opening.get(), SEGMENTS);
        assert_eq!(warming.get(), 0);
        release_open.send(()).unwrap();
        assert!(poll!(&mut prewarm).is_pending());
        assert_eq!(warming.get(), SEGMENTS);
        release_warm.send(()).unwrap();
        assert!(prewarm.await.unwrap().is_empty());
    });
}

struct Dropped<'a>(&'a Cell<usize>);

impl Drop for Dropped<'_> {
    fn drop(&mut self) {
        self.0.set(self.0.get() + 1);
    }
}

#[test]
fn prewarm_colliding_segments_cancels_pending_work_on_error() {
    for fail_during_open in [true, false] {
        futures::executor::block_on(async {
            const SEGMENTS: usize = 8;
            let started = Cell::new(0);
            let dropped = Cell::new(0);
            let (release, gate) = oneshot::channel::<()>();
            let gate = gate.shared();
            let indices = (0..SEGMENTS)
                .map(|id| segment(id as u128, Some(1)))
                .collect();
            let mut prewarm = Box::pin(prewarm_colliding_segments(indices, 0, |_, uuid| {
                let gate = gate.clone();
                let started = &started;
                let dropped = &dropped;
                async move {
                    let _guard = Dropped(dropped);
                    started.set(started.get() + 1);
                    gate.await.unwrap();
                    if !fail_during_open {
                        // Allow all opens to finish before testing a prewarm failure.
                        futures::pending!();
                    }
                    if uuid == Uuid::from_u128(3) {
                        return Err(lance_core::Error::invalid_input_source(
                            "segment I/O failed".into(),
                        ));
                    }
                    futures::future::pending::<Result<bool>>().await
                }
            }));

            assert!(poll!(&mut prewarm).is_pending());
            assert_eq!(started.get(), SEGMENTS);
            release.send(()).unwrap();
            if !fail_during_open {
                assert!(poll!(&mut prewarm).is_pending());
            }
            let result = poll!(&mut prewarm);
            assert!(
                result.is_ready(),
                "a failed segment must not wait for peers"
            );
            let err = match result {
                std::task::Poll::Ready(Err(err)) => err,
                other => panic!("expected the segment error, got {other:?}"),
            };
            assert!(err.to_string().contains("segment I/O failed"));
            assert_eq!(dropped.get(), SEGMENTS, "pending work must be cancelled");
        });
    }
}

#[test]
fn prewarm_colliding_segments_preserves_sdk_fallback() {
    futures::executor::block_on(async {
        let opened = Cell::new(0);
        let indices = vec![
            segment(0, None),
            segment(1, Some(0)),
            segment(2, Some(1)),
            segment(3, Some(1)),
        ];
        let sdk_segments = prewarm_colliding_segments(indices, 0, |field_id, uuid| {
            assert_eq!(field_id, 1);
            opened.set(opened.get() + 1);
            // Segment 2 is scalar; segment 3 is vector.
            futures::future::ready(Ok(uuid == Uuid::from_u128(3)))
        })
        .await
        .unwrap();
        assert_eq!(opened.get(), 2);
        assert_eq!(
            sdk_segments,
            [0, 1, 2].map(Uuid::from_u128),
            "scalar and unresolved segments must retain SDK validation"
        );
    });
}
