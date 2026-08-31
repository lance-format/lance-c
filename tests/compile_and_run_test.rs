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
use std::{collections::BTreeSet, mem};

use arrow_array::{FixedSizeListArray, Float32Array, Int32Array, RecordBatch, StringArray};
use arrow_schema::{DataType, Field, Schema};
use lance::Dataset;

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
        Field::new(
            "embedding",
            DataType::FixedSizeList(Arc::new(Field::new("item", DataType::Float32, false)), 8),
            false,
        ),
    ]));

    let make_batch = |start: i32, names: Vec<&str>| {
        let ids: Vec<i32> = (start..start + names.len() as i32).collect();
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

/// Compile a standalone C source file that only inspects the public header.
fn compile_c_header_test(source: &Path, output: &Path, include_dir: &Path) -> bool {
    let status = Command::new("clang")
        .args([
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-o",
            output.to_str().unwrap(),
            source.to_str().unwrap(),
            &format!("-I{}", include_dir.display()),
        ])
        .status();

    status
        .expect("C compiler is required for this ignored test")
        .success()
}

/// Run a compiled test binary with the source dataset URI and a destination URI
/// for the write test. The destination path must not pre-exist.
fn run_test_binary(binary: &Path, dataset_uri: &str, write_uri: &str) {
    let output = Command::new(binary)
        .arg(dataset_uri)
        .arg(write_uri)
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

    run_test_binary(&binary, &dataset_uri, &write_uri);
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

    run_test_binary(&binary, &dataset_uri, &write_uri);
}

#[test]
#[ignore = "requires C compiler (clang); run with: cargo test --test compile_and_run_test -- --ignored"]
fn test_c_and_rust_abi_layouts_match() {
    macro_rules! record_type {
        ($records:ident, $type:ty) => {
            $records.insert(format!(
                "T|{}|{}|{}",
                stringify!($type).rsplit("::").next().unwrap(),
                mem::size_of::<$type>(),
                mem::align_of::<$type>()
            ));
        };
    }

    macro_rules! record_field {
        ($records:ident, $type:ty, $field:ident) => {
            $records.insert(format!(
                "F|{}.{}|{}",
                stringify!($type).rsplit("::").next().unwrap(),
                stringify!($field),
                mem::offset_of!($type, $field)
            ));
        };
    }

    macro_rules! record_struct {
        ($records:ident, $type:ty, [$($field:ident),+ $(,)?]) => {
            record_type!($records, $type);
            $(record_field!($records, $type, $field);)+
        };
    }

    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let include_dir = manifest_dir.join("include");
    let source = manifest_dir.join("tests").join("cpp").join("abi_layout.c");
    let build_dir = tempfile::tempdir().unwrap();
    let binary = build_dir.path().join("abi_layout");
    assert!(
        compile_c_header_test(&source, &binary, &include_dir),
        "C ABI layout test compilation failed"
    );

    let output = Command::new(&binary)
        .output()
        .expect("failed to run C ABI layout test");
    assert!(
        output.status.success(),
        "C ABI layout test failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let c_records = String::from_utf8(output.stdout)
        .expect("C ABI layout output must be UTF-8")
        .lines()
        .map(str::to_owned)
        .collect::<BTreeSet<_>>();

    let mut rust_records = BTreeSet::new();
    record_type!(rust_records, lance_c::LanceErrorCode);
    record_type!(rust_records, lance_c::LanceVectorIndexType);
    record_type!(rust_records, lance_c::LanceScalarIndexType);
    record_type!(rust_records, lance_c::LanceMetricType);
    record_type!(rust_records, lance_c::LanceDataType);
    record_type!(rust_records, lance_c::LanceMergeWhenMatched);
    record_type!(rust_records, lance_c::LanceMergeWhenNotMatched);
    record_type!(rust_records, lance_c::LanceMergeWhenNotMatchedBySource);
    record_type!(rust_records, lance_c::LanceColumnNullableMode);
    record_type!(rust_records, lance_c::LanceScanMetricKind);
    record_type!(rust_records, lance_c::LancePollStatus);
    record_type!(rust_records, lance_c::LanceIndexSegmentBuildMode);
    record_type!(rust_records, lance_c::LanceFtsCoverageMode);
    record_type!(rust_records, lance_c::LanceWriteMode);

    record_struct!(
        rust_records,
        lance_c::LanceVectorIndexParams,
        [
            index_type,
            metric,
            num_partitions,
            num_sub_vectors,
            num_bits,
            max_iterations,
            hnsw_m,
            hnsw_ef_construction,
            sample_rate,
        ]
    );
    record_struct!(
        rust_records,
        lance_c::LanceMergeInsertParams,
        [
            when_matched,
            when_matched_expr,
            when_not_matched,
            when_not_matched_by_source,
            when_not_matched_by_source_expr,
        ]
    );
    record_struct!(
        rust_records,
        lance_c::LanceMergeInsertResult,
        [num_inserted_rows, num_updated_rows, num_deleted_rows]
    );
    record_struct!(
        rust_records,
        lance_c::LanceCompactionOptions,
        [
            target_rows_per_fragment,
            max_rows_per_group,
            max_bytes_per_file,
            num_threads,
            batch_size,
        ]
    );
    record_struct!(
        rust_records,
        lance_c::LanceCompactionMetrics,
        [
            fragments_removed,
            fragments_added,
            files_removed,
            files_added,
        ]
    );
    record_struct!(
        rust_records,
        lance_c::LanceColumnAlteration,
        [path, rename, nullable_mode, data_type]
    );
    record_struct!(rust_records, lance_c::LanceSqlColumn, [name, expression]);
    record_struct!(
        rust_records,
        lance_c::LanceScanMetric,
        [name, name_len, kind, value]
    );
    record_struct!(
        rust_records,
        lance_c::LanceScanStatistics,
        [
            iops,
            requests,
            bytes_read,
            indices_loaded,
            index_partitions_loaded,
            index_comparisons,
            metrics,
            metrics_len,
        ]
    );
    record_struct!(
        rust_records,
        lance_c::LanceIndexSegmentBuildOptions,
        [
            fragment_ids,
            fragment_count,
            index_uuid,
            ivf_centroids,
            ivf_centroids_schema,
            pq_codebook,
            pq_codebook_schema,
            mode,
        ]
    );
    record_struct!(
        rust_records,
        lance_c::LanceVectorIndexSegmentParams,
        [
            index_type,
            metric,
            num_partitions,
            num_sub_vectors,
            num_bits,
            max_iterations,
            hnsw_m,
            hnsw_ef_construction,
            sample_rate,
        ]
    );
    record_struct!(
        rust_records,
        lance_c::LanceWriteParams,
        [
            max_rows_per_file,
            max_rows_per_group,
            max_bytes_per_file,
            data_storage_version,
            enable_stable_row_ids,
        ]
    );

    assert_eq!(
        c_records, rust_records,
        "public C and Rust ABI layouts diverged"
    );
}
