// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Shared native fixtures for C and C++ entry-point tests.

use std::sync::Arc;

use arrow_array::{Int32Array, RecordBatch, StringArray};
use arrow_schema::{DataType, Field, Schema};
use geo_types::{LineString, coord};
use geoarrow_array::{GeoArrowArray, builder::LineStringBuilder};
use geoarrow_schema::{Dimension, LineStringType};
use lance::Dataset;
use lance::dataset::write::{WriteMode, WriteParams};
use lance::index::DatasetIndexExt;
use lance_index::IndexType;
use lance_index::scalar::ScalarIndexParams;
use lance_table::format::BasePath;

/// A shallow clone with source data in an alternate base plus local index,
/// deletion, transaction and config state.
pub struct ConfigBaseFilesFixture {
    pub _tmp: tempfile::TempDir,
    pub uri: String,
    #[allow(dead_code)]
    pub source_uri: String,
    pub alternate_base_uri: String,
}

/// Three-fragment GeoArrow LineString fixture for native RTree tests.
///
/// Stable IDs 100..=114 are split into fragments of five rows. Rows 103 and
/// 108 are NULL; rows 104 and 112 are empty LineStrings. Queries used by both
/// Rust and C/C++ consumers distinguish bounding-box candidates from exact
/// geometry matches:
/// - `POINT (0 10)` intersects ids 101 and 114. Id 100's diagonal bounding box
///   also contains the point, but the line does not.
/// - `LINESTRING (25 25, 26 26)` intersects only id 106. Id 105 is a closed
///   rectangle boundary whose bounding box contains the query line, but the
///   boundary itself does not intersect it.
/// - `geometry IS NULL` returns ids 103 and 108.
pub struct RTreeGeometryFixture {
    pub _tmp: tempfile::TempDir,
    pub uri: String,
}

pub fn create_rtree_geometry_fixture() -> RTreeGeometryFixture {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp
        .path()
        .join("rtree_geometry_ds")
        .to_str()
        .unwrap()
        .to_string();

    let line = |coords: &[(f64, f64)]| {
        LineString::new(coords.iter().map(|&(x, y)| coord! { x: x, y: y }).collect())
    };
    let empty = || LineString::new(Vec::new());
    let geometries = vec![
        Some(line(&[(0.0, 0.0), (10.0, 10.0)])),
        Some(line(&[(0.0, 10.0), (10.0, 10.0)])),
        Some(line(&[(2.0, 0.0), (2.0, 10.0)])),
        None,
        Some(empty()),
        Some(line(&[
            (20.0, 20.0),
            (30.0, 20.0),
            (30.0, 30.0),
            (20.0, 30.0),
            (20.0, 20.0),
        ])),
        Some(line(&[(20.0, 20.0), (30.0, 30.0)])),
        Some(line(&[(40.0, 40.0), (50.0, 50.0)])),
        None,
        Some(line(&[(60.0, 60.0), (61.0, 61.0)])),
        Some(line(&[(0.0, 5.0), (5.0, 0.0)])),
        Some(line(&[(100.0, 100.0), (101.0, 101.0)])),
        Some(empty()),
        Some(line(&[(2.0, 2.0), (3.0, 2.0)])),
        Some(line(&[(0.0, 10.0), (5.0, 10.0)])),
    ];

    let geometry_type = LineStringType::new(Dimension::XY, Default::default());
    let mut builder = LineStringBuilder::new(geometry_type.clone());
    for geometry in &geometries {
        builder.push_line_string(geometry.as_ref()).unwrap();
    }
    let geometry_array = builder.finish();
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        geometry_type.to_field("geometry", true),
    ]));
    let batch = RecordBatch::try_new(
        schema.clone(),
        vec![
            Arc::new(Int32Array::from_iter_values(100..115)),
            geometry_array.to_array_ref(),
        ],
    )
    .unwrap();

    lance_c::runtime::block_on(async {
        let dataset = Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch)], schema),
            &uri,
            Some(WriteParams {
                max_rows_per_file: 5,
                enable_stable_row_ids: true,
                ..Default::default()
            }),
        )
        .await
        .unwrap();
        assert_eq!(dataset.get_fragments().len(), 3);
    });

    RTreeGeometryFixture { _tmp: tmp, uri }
}

/// Create a multi-version, multi-fragment dataset.
///
/// Facts intentionally fixed by the fixture:
/// - base ID 0 is the source dataset root, introduced by the shallow clone;
/// - base ID 7 is the alternate base named `alt-root`, not a dataset root;
/// - one fragment was written to base 7 and one to the source primary root;
/// - one row was deleted, creating a deletion file;
/// - a local scalar index was created after cloning;
/// - config keys `fixture.key=fixture-value`, `fixture.empty=""` and
///   `fixture.remove=present` exist;
/// - `untracked.txt` exists under the primary root and is not tracked.
pub fn create_config_base_files_fixture() -> ConfigBaseFilesFixture {
    let tmp = tempfile::tempdir().unwrap();
    let source_uri = tmp
        .path()
        .join("config_base_source")
        .to_str()
        .unwrap()
        .to_string();
    let uri = tmp
        .path()
        .join("config_base_dataset")
        .to_str()
        .unwrap()
        .to_string();
    let alternate_base_uri = tmp
        .path()
        .join("config_base_alternate")
        .to_str()
        .unwrap()
        .to_string();

    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("label", DataType::Utf8, true),
    ]));
    let batch = |ids: Vec<i32>, labels: Vec<Option<&str>>| {
        RecordBatch::try_new(
            schema.clone(),
            vec![
                Arc::new(Int32Array::from(ids)),
                Arc::new(StringArray::from(labels)),
            ],
        )
        .unwrap()
    };

    lance_c::runtime::block_on(async {
        let first = batch(
            vec![0, 1, 2, 3],
            vec![Some("alt-a"), Some("alt-b"), Some("alt-c"), None],
        );
        let mut source = Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(first)], schema.clone()),
            &source_uri,
            Some(WriteParams {
                mode: WriteMode::Create,
                initial_bases: Some(vec![BasePath::new(
                    7,
                    alternate_base_uri.clone(),
                    Some("alt-root".to_string()),
                    false,
                )]),
                target_bases: Some(vec![7]),
                ..Default::default()
            }),
        )
        .await
        .unwrap();

        let second = batch(
            vec![10, 11, 12, 13],
            vec![
                Some("primary-a"),
                None,
                Some("primary-c"),
                Some("primary-d"),
            ],
        );
        source
            .append(
                arrow::record_batch::RecordBatchIterator::new(vec![Ok(second)], schema.clone()),
                Some(WriteParams {
                    mode: WriteMode::Append,
                    target_bases: Some(vec![0]),
                    ..Default::default()
                }),
            )
            .await
            .unwrap();
        source.delete("id = 2").await.unwrap();

        let source_version = source.version().version;
        let mut cloned = source
            .shallow_clone(&uri, source_version, None)
            .await
            .unwrap();
        cloned
            .create_index(
                &["id"],
                IndexType::Scalar,
                Some("id_idx".to_string()),
                &ScalarIndexParams::default(),
                false,
            )
            .await
            .unwrap();
        cloned
            .update_config([
                ("fixture.key", "fixture-value"),
                ("fixture.empty", ""),
                ("fixture.remove", "present"),
            ])
            .await
            .unwrap();
    });

    std::fs::write(
        std::path::Path::new(&uri).join("untracked.txt"),
        b"untracked fixture file\n",
    )
    .unwrap();

    ConfigBaseFilesFixture {
        _tmp: tmp,
        uri,
        source_uri,
        alternate_base_uri,
    }
}
