// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Tests for the CMake helpers under `cmake/` that aren't directly exercised by
//! Rust code but are critical to the `find_package(LanceC)` consumer story.
//!
//! These tests `cmake -P` standalone CMake scripts that assert on `read_cargo_version`
//! and friends. CMake is pre-installed on every GitHub-hosted runner, so this
//! runs in the standard CI lane (no `--ignored` gate).

use std::path::PathBuf;
use std::process::Command;

#[test]
fn read_cargo_version_handles_stable_and_prerelease_inputs() {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let script = manifest_dir.join("cmake").join("test_cargo_version.cmake");

    let output = Command::new("cmake")
        .arg("-P")
        .arg(&script)
        .output()
        .unwrap_or_else(|e| panic!("cmake not available on PATH: {e}"));

    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);

    assert!(
        output.status.success(),
        "cmake -P {} failed (exit {:?})\nstdout:\n{stdout}\nstderr:\n{stderr}",
        script.display(),
        output.status.code(),
    );
    let combined = format!("{stdout}\n{stderr}");
    assert!(
        combined.contains("All read_cargo_version tests passed."),
        "expected success marker, got:\nstdout:\n{stdout}\nstderr:\n{stderr}"
    );
}
