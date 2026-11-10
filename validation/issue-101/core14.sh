#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

expected_head=$1
evidence_dir=$2
actual_head=$(git rev-parse HEAD)
if [[ "$actual_head" != "$expected_head" ]]; then
    printf 'Expected %s, found %s\n' "$expected_head" "$actual_head" >&2
    exit 1
fi
test -z "$(git status --porcelain)"
mkdir -p "$evidence_dir"
printf '%s\n' "$actual_head" > "$evidence_dir/source-commit.txt"
export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4
export RUSTUP_TOOLCHAIN=1.94.0

run_check() {
    local name=$1
    shift
    printf '%q ' "$@" > "$evidence_dir/$name.command"
    printf '\n' >> "$evidence_dir/$name.command"
    local result=0
    "$@" > "$evidence_dir/$name.log" 2>&1 || result=$?
    printf '%s\n' "$result" > "$evidence_dir/$name.status"
    tail -n 8 "$evidence_dir/$name.log"
    return "$result"
}

run_check format cargo fmt -- --check
run_check clippy cargo clippy --locked --all-targets -- -D warnings
run_check tests cargo test --locked
run_check native cargo test --locked --test compile_and_run_test -- --ignored
run_check rustdoc env RUSTDOCFLAGS=-Dwarnings cargo doc --locked --no-deps
