// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Tests that compile and run actual C and C++ programs against the lance-c library.
//!
//! These tests:
//! 1. Create a test dataset on disk
//! 2. Build the lance-c shared library
//! 3. Compile C/C++ test programs linking against it
//! 4. Run the compiled binaries with the dataset path
//!
//! This validates that lance.h and lance.hpp are valid C/C++ and that
//! the API works end-to-end from a real C/C++ caller.

use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Arc;

use arrow_array::{
    BinaryArray, FixedSizeListArray, Float32Array, Int32Array, RecordBatch, StringArray,
    StructArray, UInt32Array,
};
use arrow_schema::{DataType, Field, Fields, Schema};
use lance::Dataset;
use lance::dataset::{WriteMode, WriteParams};
use lance_file::version::LanceFileVersion;

use std::ffi::CString;

/// Build the lance-c cdylib and return the path to the shared library and include dir.
fn build_lance_c() -> (PathBuf, PathBuf) {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));

    // Build the cdylib in debug mode.
    let status = Command::new("cargo")
        .args(["build", "--lib"])
        .current_dir(&manifest_dir)
        .status()
        .expect("Failed to run cargo build");
    assert!(status.success(), "cargo build failed");

    let target_dir = manifest_dir.join("target").join("debug");

    // Find the shared library.
    let lib_path = if cfg!(target_os = "macos") {
        target_dir.join("liblance_c.dylib")
    } else if cfg!(target_os = "linux") {
        target_dir.join("liblance_c.so")
    } else {
        panic!("Unsupported OS for C/C++ link test");
    };

    assert!(
        lib_path.exists(),
        "Shared library not found at {}",
        lib_path.display()
    );

    let include_dir = manifest_dir.join("include");
    (lib_path, include_dir)
}

/// Create a test dataset on disk and return (TempDir, path_string).
fn create_test_dataset_on_disk() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("c_test_ds").to_str().unwrap().to_string();

    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("name", DataType::Utf8, true),
        Field::new("score", DataType::Int32, true),
        Field::new(
            "embedding",
            DataType::FixedSizeList(Arc::new(Field::new("item", DataType::Float32, false)), 8),
            false,
        ),
    ]));

    let make_batch = |start: i32, names: Vec<&str>| {
        let ids: Vec<i32> = (start..start + names.len() as i32).collect();
        let scores = ids
            .iter()
            .map(|id| {
                if *id == 7 || *id == 16 {
                    None
                } else {
                    Some(id * 10)
                }
            })
            .collect::<Vec<_>>();
        let values = Float32Array::from_iter_values(
            ids.iter()
                .flat_map(|id| (0..8).map(move |component| *id as f32 * 0.1 + component as f32)),
        );
        let vectors = FixedSizeListArray::try_new(
            Arc::new(Field::new("item", DataType::Float32, false)),
            8,
            Arc::new(values),
            None,
        )
        .unwrap();
        RecordBatch::try_new(
            schema.clone(),
            vec![
                Arc::new(Int32Array::from(ids)),
                Arc::new(StringArray::from(names)),
                Arc::new(Int32Array::from(scores)),
                Arc::new(vectors),
            ],
        )
        .unwrap()
    };
    let first = make_batch(
        1,
        vec![
            "alice", "bob", "carol", "dave", "eve", "frank", "grace", "heidi", "ivan", "judy",
        ],
    );
    let second = make_batch(
        11,
        vec![
            "kate", "leo", "maya", "nick", "olga", "paul", "quinn", "ruth", "sam", "tina",
        ],
    );

    lance_c::runtime::block_on(async {
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(first)], schema.clone()),
            &uri,
            None,
        )
        .await
        .unwrap()
        .append(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(second)], schema),
            None,
        )
        .await
        .unwrap();
    });

    (tmp, uri)
}

/// Create a five-version, four-fragment dataset with one deleted row for
/// maintenance API tests.
fn create_cleanup_dataset_on_disk() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp
        .path()
        .join("cleanup_c_test_ds")
        .to_str()
        .unwrap()
        .to_string();
    let schema = Arc::new(Schema::new(vec![Field::new("id", DataType::Int32, false)]));
    let batch = |start: i32| {
        RecordBatch::try_new(
            schema.clone(),
            vec![Arc::new(Int32Array::from_iter_values(start..start + 4))],
        )
        .unwrap()
    };

    lance_c::runtime::block_on(async {
        let mut dataset = Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(batch(0))], schema.clone()),
            &uri,
            None,
        )
        .await
        .unwrap();
        for start in [4, 8, 12] {
            dataset
                .append(
                    arrow::record_batch::RecordBatchIterator::new(
                        vec![Ok(batch(start))],
                        schema.clone(),
                    ),
                    None,
                )
                .await
                .unwrap();
        }
        dataset.delete("id = 1").await.unwrap();
    });
    (tmp, uri)
}

/// Create a two-fragment dataset with a B-tree index for session cache tests.
fn create_session_cache_dataset_on_disk() -> (tempfile::TempDir, String) {
    let fixture = create_test_dataset_on_disk();
    let uri = CString::new(fixture.1.as_str()).unwrap();
    unsafe {
        let dataset = lance_c::lance_dataset_open(uri.as_ptr(), std::ptr::null(), 0);
        assert!(!dataset.is_null(), "failed to open session cache fixture");
        assert_eq!(lance_c::lance_dataset_fragment_count(dataset), 2);
        let column = c"id";
        let index_name = c"id_idx";
        let rc = lance_c::lance_dataset_create_scalar_index(
            dataset,
            column.as_ptr(),
            index_name.as_ptr(),
            lance_c::LanceScalarIndexType::BTree as i32,
            std::ptr::null(),
            false,
        );
        if rc != 0 {
            let message = lance_c::lance_last_error_message();
            let message = if message.is_null() {
                "unknown error".to_string()
            } else {
                std::ffi::CStr::from_ptr(message)
                    .to_string_lossy()
                    .into_owned()
            };
            panic!("failed to create session cache B-tree index: {message}");
        }
        assert_eq!(lance_c::lance_dataset_index_count(dataset), 1);
        lance_c::lance_dataset_close(dataset);
    }
    fixture
}

/// Create a two-fragment dataset with two independently indexed FTS columns.
fn create_typed_fts_dataset_on_disk() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp
        .path()
        .join("typed_fts_c_test_ds")
        .to_str()
        .unwrap()
        .to_string();
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("title", DataType::Utf8, false),
        Field::new("body", DataType::Utf8, false),
        Field::new("category", DataType::Utf8, false),
    ]));

    let make_batch =
        |ids: Vec<i32>, titles: Vec<&str>, bodies: Vec<&str>, categories: Vec<&str>| {
            RecordBatch::try_new(
                schema.clone(),
                vec![
                    Arc::new(Int32Array::from(ids)),
                    Arc::new(StringArray::from(titles)),
                    Arc::new(StringArray::from(bodies)),
                    Arc::new(StringArray::from(categories)),
                ],
            )
            .unwrap()
        };
    let first = make_batch(
        vec![1, 2, 3, 4],
        vec!["alpha beta", "alpha", "alpha beta", "beta gamma"],
        vec!["gamma rust", "delta rust", "gamma", "alpha"],
        vec!["a", "b", "a", "b"],
    );
    let second = make_batch(
        vec![5, 6, 7, 8, 9, 10],
        vec!["rust rust", "rust", "lance", "delta", "rusty", "bust"],
        vec![
            "lance",
            "rust rust rust",
            "database",
            "alpha",
            "suffix",
            "prefix",
        ],
        vec!["a", "b", "a", "b", "a", "b"],
    );

    lance_c::runtime::block_on(async {
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(first)], schema.clone()),
            &uri,
            None,
        )
        .await
        .unwrap()
        .append(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(second)], schema),
            None,
        )
        .await
        .unwrap();
    });

    let uri_c = CString::new(uri.clone()).unwrap();
    let dataset = unsafe { lance_c::lance_dataset_open(uri_c.as_ptr(), std::ptr::null(), 0) };
    assert!(!dataset.is_null(), "failed to open typed FTS dataset");
    let params =
        CString::new(r#"{"base_tokenizer":"simple","language":"English","with_position":true}"#)
            .unwrap();
    for column in ["title", "body"] {
        let column = CString::new(column).unwrap();
        let rc = unsafe {
            lance_c::lance_dataset_create_scalar_index(
                dataset,
                column.as_ptr(),
                std::ptr::null(),
                lance_c::LanceScalarIndexType::Inverted as i32,
                params.as_ptr(),
                false,
            )
        };
        if rc != 0 {
            let message = lance_c::lance_last_error_message();
            let message = if message.is_null() {
                "unknown error".to_string()
            } else {
                unsafe { std::ffi::CStr::from_ptr(message) }
                    .to_string_lossy()
                    .into_owned()
            };
            panic!("failed to create inverted index: {message}");
        }
    }
    unsafe { lance_c::lance_dataset_close(dataset) };

    (tmp, uri)
}

/// Create a two-fragment dataset for scanner ordering tests.
fn create_ordering_dataset_on_disk() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("ordering_ds").to_str().unwrap().to_string();
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("category", DataType::Utf8, true),
        Field::new("rank", DataType::Int32, true),
    ]));
    let make_batch = |ids: Vec<i32>, groups: &[Option<&str>], ranks: &[Option<i32>]| {
        RecordBatch::try_new(
            schema.clone(),
            vec![
                Arc::new(Int32Array::from(ids)),
                Arc::new(StringArray::from(groups.to_vec())),
                Arc::new(Int32Array::from(ranks.to_vec())),
            ],
        )
        .unwrap()
    };

    lance_c::runtime::block_on(async {
        let first = make_batch(
            vec![0, 1, 2, 3, 4, 5],
            &[
                Some("b"),
                Some("b"),
                Some("a"),
                Some("a"),
                Some("b"),
                Some("a"),
            ],
            &[Some(2), None, Some(1), Some(1), Some(2), None],
        );
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(first)], schema.clone()),
            &uri,
            None,
        )
        .await
        .unwrap();
        let second = make_batch(
            vec![6, 7, 8, 9, 10, 11],
            &[Some("b"), Some("a"), None, Some("b"), Some("a"), None],
            &[Some(2), Some(1), Some(3), None, Some(1), Some(2)],
        );
        let mut dataset = Dataset::open(&uri).await.unwrap();
        dataset
            .append(
                arrow::record_batch::RecordBatchIterator::new(vec![Ok(second)], schema.clone()),
                None,
            )
            .await
            .unwrap();
    });

    (tmp, uri)
}

/// Create a three-version, two-fragment dataset with nested schema and one
/// deleted row in each fragment. Version 2 is the multi-fragment state before
/// deletion; version 3 adds the deletion files.
fn create_fragment_dataset_on_disk() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("fragment_ds").to_str().unwrap().to_string();

    let profile_fields = Fields::from(vec![
        Field::new("name", DataType::Utf8, true),
        Field::new("score", DataType::Float32, true),
    ]);
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("profile", DataType::Struct(profile_fields.clone()), true),
    ]));

    let make_batch = |ids: Vec<i32>, names: Vec<Option<&str>>, scores: Vec<Option<f32>>| {
        let profiles = StructArray::new(
            profile_fields.clone(),
            vec![
                Arc::new(StringArray::from(names)),
                Arc::new(Float32Array::from(scores)),
            ],
            None,
        );
        RecordBatch::try_new(
            schema.clone(),
            vec![Arc::new(Int32Array::from(ids)), Arc::new(profiles)],
        )
        .unwrap()
    };

    lance_c::runtime::block_on(async {
        let first = make_batch(
            vec![0, 1, 2, 3],
            vec![Some("a"), Some("b"), None, Some("d")],
            vec![Some(1.0), Some(2.0), Some(3.5), None],
        );
        Dataset::write(
            arrow::record_batch::RecordBatchIterator::new(vec![Ok(first)], schema.clone()),
            &uri,
            None,
        )
        .await
        .unwrap();

        let second = make_batch(
            vec![10, 11, 12, 13],
            vec![Some("j"), None, Some("l"), Some("m")],
            vec![Some(10.0), Some(11.0), None, Some(13.0)],
        );
        let mut dataset = Dataset::open(&uri).await.unwrap();
        dataset
            .append(
                arrow::record_batch::RecordBatchIterator::new(vec![Ok(second)], schema.clone()),
                None,
            )
            .await
            .unwrap();
        dataset.delete("id IN (1, 12)").await.unwrap();
    });

    (tmp, uri)
}

/// Create a two-fragment Blob v2 dataset on disk and return (TempDir, path_string).
///
/// Each fragment has five rows: blobs of 8, 128 and 1024 bytes (inline, packed
/// and dedicated under the 16 / 256 thresholds), an empty blob and a null,
/// next to `id` and a plain `raw` binary column. Payload byte `i` is
/// `(i * 7 + 3) as u8`.
fn create_blob_dataset_on_disk() -> (tempfile::TempDir, String) {
    let tmp = tempfile::tempdir().unwrap();
    let uri = tmp.path().join("blob_ds").to_str().unwrap().to_string();

    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::UInt32, false),
        lance::blob_field_with_options(
            "blob",
            true,
            lance::BlobFieldOptions {
                inline_size_threshold: Some(16),
                dedicated_size_threshold: std::num::NonZeroUsize::new(256),
            },
        ),
        Field::new("raw", DataType::Binary, true),
    ]));

    let make_batch = |first_id: u32| {
        let mut blobs = lance::BlobArrayBuilder::new(5);
        for len in [8usize, 128, 1024] {
            let payload: Vec<u8> = (0..len).map(|i| (i * 7 + 3) as u8).collect();
            blobs.push_bytes(payload).unwrap();
        }
        blobs.push_empty().unwrap();
        blobs.push_null().unwrap();

        let ids: Vec<u32> = (first_id..first_id + 5).collect();
        // The plain binary column is null in the same row as the blob column.
        let raw = BinaryArray::from_iter((0..5).map(|row| (row < 4).then_some(&b"raw"[..])));
        RecordBatch::try_new(
            schema.clone(),
            vec![
                Arc::new(UInt32Array::from(ids)),
                blobs.finish().unwrap(),
                Arc::new(raw),
            ],
        )
        .unwrap()
    };

    lance_c::runtime::block_on(async {
        for (first_id, mode) in [(0u32, WriteMode::Create), (100u32, WriteMode::Append)] {
            let params = WriteParams {
                mode,
                // Blob v2 is a 2.2 storage feature.
                data_storage_version: Some(LanceFileVersion::V2_2),
                ..Default::default()
            };
            Dataset::write(
                arrow::record_batch::RecordBatchIterator::new(
                    vec![Ok(make_batch(first_id))],
                    schema.clone(),
                ),
                &uri,
                Some(params),
            )
            .await
            .unwrap();
        }
    });

    (tmp, uri)
}

/// Compile a C source file, linking against lance-c.
fn compile_c_test(source: &Path, output: &Path, include_dir: &Path, lib_path: &Path) -> bool {
    let lib_dir = lib_path.parent().unwrap();
    let lib_name = "lance_c";

    let status = Command::new("clang")
        .args([
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-o",
            output.to_str().unwrap(),
            source.to_str().unwrap(),
            &format!("-I{}", include_dir.display()),
            &format!("-L{}", lib_dir.display()),
            &format!("-l{lib_name}"),
            // On macOS, set rpath so the dylib is found at runtime.
            &format!("-Wl,-rpath,{}", lib_dir.display()),
        ])
        .status();

    status
        .expect("C compiler is required for this ignored test")
        .success()
}

/// Compile a C++ source file, linking against lance-c.
fn compile_cpp_test(source: &Path, output: &Path, include_dir: &Path, lib_path: &Path) -> bool {
    let lib_dir = lib_path.parent().unwrap();
    let lib_name = "lance_c";

    let status = Command::new("c++")
        .args([
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-o",
            output.to_str().unwrap(),
            source.to_str().unwrap(),
            &format!("-I{}", include_dir.display()),
            &format!("-L{}", lib_dir.display()),
            &format!("-l{lib_name}"),
            &format!("-Wl,-rpath,{}", lib_dir.display()),
        ])
        .status();

    status
        .expect("C++ compiler is required for this ignored test")
        .success()
}

/// Run a compiled test binary with the source dataset URI, a destination URI
/// for the write test, the Blob v2 and ordering datasets, the indexed session
/// cache dataset, the nested-schema fragment-deletion dataset and the cleanup
/// fixture. The destination path must not pre-exist.
#[allow(clippy::too_many_arguments)]
fn run_test_binary(
    binary: &Path,
    dataset_uri: &str,
    write_uri: &str,
    blob_uri: &str,
    ordering_uri: &str,
    session_uri: &str,
    fragment_uri: &str,
    cleanup_uri: &str,
) {
    let output = Command::new(binary)
        .arg(dataset_uri)
        .arg(write_uri)
        .arg(blob_uri)
        .arg(ordering_uri)
        .arg(session_uri)
        .arg(fragment_uri)
        .arg(cleanup_uri)
        .output()
        .unwrap_or_else(|e| panic!("Failed to run {}: {e}", binary.display()));

    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);

    println!("--- stdout ---\n{stdout}");
    if !stderr.is_empty() {
        eprintln!("--- stderr ---\n{stderr}");
    }

    assert!(
        output.status.success(),
        "Test binary {} failed with exit code {:?}\nstdout: {}\nstderr: {}",
        binary.display(),
        output.status.code(),
        stdout,
        stderr
    );
}

fn run_test_binary_with_dataset(binary: &Path, dataset_uri: &str) {
    let output = Command::new(binary)
        .arg(dataset_uri)
        .output()
        .unwrap_or_else(|e| panic!("Failed to run {}: {e}", binary.display()));

    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);

    println!("--- stdout ---\n{stdout}");
    if !stderr.is_empty() {
        eprintln!("--- stderr ---\n{stderr}");
    }

    assert!(
        output.status.success(),
        "Test binary {} failed with exit code {:?}\nstdout: {}\nstderr: {}",
        binary.display(),
        output.status.code(),
        stdout,
        stderr
    );
}

#[test]
#[ignore = "requires C compiler (cc); run with: cargo test -p lance-c -- --ignored test_c_compilation"]
fn test_c_compilation_and_execution() {
    let (lib_path, include_dir) = build_lance_c();
    let (tmp, dataset_uri) = create_test_dataset_on_disk();
    let write_uri = tmp.path().join("c_write_ds").to_str().unwrap().to_string();
    let (_blob_tmp, blob_uri) = create_blob_dataset_on_disk();
    let (_ordering_tmp, ordering_uri) = create_ordering_dataset_on_disk();
    let (_session_tmp, session_uri) = create_session_cache_dataset_on_disk();
    let (_fragment_tmp, fragment_uri) = create_fragment_dataset_on_disk();
    let (_cleanup_tmp, cleanup_uri) = create_cleanup_dataset_on_disk();
    let build_dir = tempfile::tempdir().unwrap();

    let source = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("cpp")
        .join("test_c_api.c");
    let binary = build_dir.path().join("test_c_api");

    assert!(
        compile_c_test(&source, &binary, &include_dir, &lib_path),
        "C test compilation failed"
    );

    run_test_binary(
        &binary,
        &dataset_uri,
        &write_uri,
        &blob_uri,
        &ordering_uri,
        &session_uri,
        &fragment_uri,
        &cleanup_uri,
    );
}

#[test]
#[ignore = "requires C++ compiler (c++); run with: cargo test -p lance-c -- --ignored test_cpp_compilation"]
fn test_cpp_compilation_and_execution() {
    let (lib_path, include_dir) = build_lance_c();
    let (tmp, dataset_uri) = create_test_dataset_on_disk();
    let write_uri = tmp
        .path()
        .join("cpp_write_ds")
        .to_str()
        .unwrap()
        .to_string();
    let (_blob_tmp, blob_uri) = create_blob_dataset_on_disk();
    let (_ordering_tmp, ordering_uri) = create_ordering_dataset_on_disk();
    let (_session_tmp, session_uri) = create_session_cache_dataset_on_disk();
    let (_fragment_tmp, fragment_uri) = create_fragment_dataset_on_disk();
    let (_cleanup_tmp, cleanup_uri) = create_cleanup_dataset_on_disk();
    let build_dir = tempfile::tempdir().unwrap();

    let source = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("cpp")
        .join("test_cpp_api.cpp");
    let binary = build_dir.path().join("test_cpp_api");

    assert!(
        compile_cpp_test(&source, &binary, &include_dir, &lib_path),
        "C++ test compilation failed"
    );

    run_test_binary(
        &binary,
        &dataset_uri,
        &write_uri,
        &blob_uri,
        &ordering_uri,
        &session_uri,
        &fragment_uri,
        &cleanup_uri,
    );
}

#[test]
#[ignore = "requires C and C++ compilers; run with: cargo test -p lance-c -- --ignored test_typed_fts_native_compilation_and_execution"]
fn test_typed_fts_native_compilation_and_execution() {
    let (lib_path, include_dir) = build_lance_c();
    let (_tmp, dataset_uri) = create_typed_fts_dataset_on_disk();
    let build_dir = tempfile::tempdir().unwrap();
    let tests_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("cpp");

    let c_source = tests_dir.join("test_fts_query.c");
    let c_binary = build_dir.path().join("test_fts_query_c");
    assert!(
        compile_c_test(&c_source, &c_binary, &include_dir, &lib_path),
        "C FTS test compilation failed"
    );
    run_test_binary_with_dataset(&c_binary, &dataset_uri);

    let cpp_source = tests_dir.join("test_fts_query.cpp");
    let cpp_binary = build_dir.path().join("test_fts_query_cpp");
    assert!(
        compile_cpp_test(&cpp_source, &cpp_binary, &include_dir, &lib_path),
        "C++ FTS test compilation failed"
    );
    run_test_binary_with_dataset(&cpp_binary, &dataset_uri);
}

/// A fresh C executable must initialize OpenDAL even when archive constructors are omitted.
#[cfg(target_os = "linux")]
#[test]
#[ignore = "requires a C compiler, Python 3, and building the static library"]
fn test_static_oss_transport() {
    let (shared_library, _) = build_lance_c();
    let static_library = shared_library.with_file_name("liblance_c.a");
    assert!(static_library.exists(), "static library was not built");
    let status = Command::new("python3")
        .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/static_oss_transport_test.py"))
        .arg(static_library)
        .status()
        .expect("failed to run the static OSS transport test");
    assert!(status.success(), "static OSS HTTP transport test failed");
}
