#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

expected_head=$1
evidence_dir=$2
test "$(git rev-parse HEAD)" = "$expected_head"
test -z "$(git status --porcelain)"
mkdir -p "$evidence_dir"
printf '%s\n' "$expected_head" > "$evidence_dir/source-head.txt"
export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4
export RUSTUP_TOOLCHAIN=1.94.0

printf '%s\n' 'cargo test --locked --test compile_and_run_test -- --ignored test_cpp_compilation_and_execution --nocapture' > "$evidence_dir/cpp.command"
result=0
cargo test --locked --test compile_and_run_test -- --ignored test_cpp_compilation_and_execution --nocapture > "$evidence_dir/cpp.log" 2>&1 || result=$?
printf '%s\n' "$result" > "$evidence_dir/cpp.status"
tail -n 20 "$evidence_dir/cpp.log"
exit "$result"
