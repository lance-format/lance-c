#!/usr/bin/env bash
set -euo pipefail

readonly PRODUCT_COMMIT="103e832a81e18ca5731a5c1f325601a3ba0b60f7"
readonly BASE_COMMIT="684fabc5aff9954b09a5a57fefd255678a35b7c5"
readonly NATIVE_COMMIT="40f15ed1b71b66950961b175829fdf17a763f108"
readonly CARGO_TOOLCHAIN="1.94.0"
readonly EVIDENCE_DIR="${EVIDENCE_DIR:-/home/xuanwo/Code/validation-evidence/lance-c/native-fragment-scan-take-count-103e832a81e1}"

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"
mkdir -p "$EVIDENCE_DIR/logs"
: >"$EVIDENCE_DIR/commands.log"
: >"$EVIDENCE_DIR/runner.log"
rm -f "$EVIDENCE_DIR/status" "$EVIDENCE_DIR/summary"
exec > >(tee -a "$EVIDENCE_DIR/runner.log") 2>&1

STARTED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
SUCCESS=0

write_status() {
    local rc=$?
    local state="failure"
    if [[ "$SUCCESS" -eq 1 && "$rc" -eq 0 ]]; then
        state="success"
    fi
    local tmp="$EVIDENCE_DIR/.status.tmp"
    {
        printf 'state=%s\n' "$state"
        printf 'exit_code=%s\n' "$rc"
        printf 'product_commit=%s\n' "$PRODUCT_COMMIT"
        printf 'validation_commit=%s\n' "$(git rev-parse HEAD)"
        printf 'base_commit=%s\n' "$BASE_COMMIT"
        printf 'native_commit=%s\n' "$NATIVE_COMMIT"
        printf 'root=%s\n' "$ROOT"
        printf 'started_at=%s\n' "$STARTED_AT"
        printf 'finished_at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >"$tmp"
    mv "$tmp" "$EVIDENCE_DIR/status"
}
trap write_status EXIT

fail() {
    printf 'validation failure: %s\n' "$*" >&2
    return 1
}

run_step() {
    local name="$1"
    shift
    local log="$EVIDENCE_DIR/logs/$name.log"
    printf '\n[%s] %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$name" | tee "$log"
    printf 'cd %q && ' "$ROOT" >>"$EVIDENCE_DIR/commands.log"
    printf '%q ' "$@" >>"$EVIDENCE_DIR/commands.log"
    printf '\n' >>"$EVIDENCE_DIR/commands.log"
    set +e
    "$@" 2>&1 | tee -a "$log"
    local rc=${PIPESTATUS[0]}
    set -e
    printf 'exit_code=%s\n' "$rc" >>"$log"
    return "$rc"
}

log_command() {
    local name="$1"
    shift
    local log="$EVIDENCE_DIR/logs/$name.log"
    printf 'cd %q && ' "$ROOT" >>"$EVIDENCE_DIR/commands.log"
    printf '%q ' "$@" >>"$EVIDENCE_DIR/commands.log"
    printf '\n' >>"$EVIDENCE_DIR/commands.log"
    "$@" 2>&1 | tee "$log"
}

for variable in \
    CARGO_TARGET_DIR \
    CARGO_BUILD_BUILD_DIR \
    RUSTFLAGS \
    CARGO_ENCODED_RUSTFLAGS \
    CARGO_PROFILE_DEV_LTO \
    CARGO_PROFILE_RELEASE_LTO \
    CARGO_PROFILE_TEST_LTO
do
    if [[ -n "${!variable:-}" ]]; then
        fail "unexpected build override $variable=${!variable}"
    fi
done

git merge-base --is-ancestor "$PRODUCT_COMMIT" HEAD \
    || fail "validation checkout does not contain the expected product commit"
validation_only_changes="$(git diff --name-only "$PRODUCT_COMMIT" HEAD)"
[[ "$validation_only_changes" == "scripts/validate_fragment_scan_take_count.sh" ]] \
    || fail "validation commit changed product files: $validation_only_changes"
git merge-base --is-ancestor "$BASE_COMMIT" "$PRODUCT_COMMIT" \
    || fail "product commit does not descend from the accepted base"
git diff --exit-code "$BASE_COMMIT" "$PRODUCT_COMMIT" -- Cargo.toml Cargo.lock \
    || fail "product changed Cargo.toml or Cargo.lock"
[[ -z "$(git status --porcelain)" ]] || fail "validation checkout is dirty"
rg -q "$NATIVE_COMMIT" Cargo.toml || fail "ffi manifest is not pinned to the accepted native commit"

log_command 00-host bash -lc \
    'hostname; whoami; uname -srm; pwd; command -v cargo; command -v clang; command -v c++; command -v cmake; command -v tmux; command -v mbx'
log_command 01-git bash -lc \
    'git rev-parse HEAD; git rev-parse HEAD^; git merge-base HEAD 684fabc5aff9954b09a5a57fefd255678a35b7c5; git status --short --branch; git remote get-url origin; git log --oneline -3'
log_command 02-toolchain cargo "+$CARGO_TOOLCHAIN" --version
log_command 02-toolchain rustc "+$CARGO_TOOLCHAIN" -Vv
log_command 02-toolchain rustc "+$CARGO_TOOLCHAIN" --print target-libdir

run_step 10-fmt cargo "+$CARGO_TOOLCHAIN" fmt --all -- --check
run_step 11-check cargo "+$CARGO_TOOLCHAIN" check --all-targets -j 8
run_step 12-clippy cargo "+$CARGO_TOOLCHAIN" clippy --all-targets -j 8 -- -D warnings
run_step 20-full-rust cargo "+$CARGO_TOOLCHAIN" test -j 8 -- --test-threads=4
run_step 21-c-cpp-consumers cargo "+$CARGO_TOOLCHAIN" test --test compile_and_run_test -j 8 -- --ignored --test-threads=4 --nocapture

for test_name in \
    test_fragment_take_matches_native_projection_and_logical_order \
    test_fragment_take_uses_deletion_aware_local_indices_and_errors \
    test_fragment_count_rows_matches_native_filters \
    test_fragment_scan_uses_normal_scanner_and_owned_snapshot
do
    rg -q "^test ${test_name} \\.\\.\\. ok$" "$EVIDENCE_DIR/logs/20-full-rust.log" \
        || fail "full Rust test did not pass: $test_name"
done

for test_name in \
    test_c_compilation_and_execution \
    test_cpp_compilation_and_execution \
    test_typed_fts_native_compilation_and_execution \
    test_static_oss_transport
do
    rg -q "^test ${test_name} \\.\\.\\. ok$" "$EVIDENCE_DIR/logs/21-c-cpp-consumers.log" \
        || fail "ignored C/C++ consumer did not pass: $test_name"
done

rg -q 'All C tests passed!' "$EVIDENCE_DIR/logs/21-c-cpp-consumers.log" \
    || fail "C consumer completion marker is missing"
rg -q 'All C++ tests passed!' "$EVIDENCE_DIR/logs/21-c-cpp-consumers.log" \
    || fail "C++ consumer completion marker is missing"
fragment_markers="$(rg -o 'test_fragment_read_operations\.\.\. OK' "$EVIDENCE_DIR/logs/21-c-cpp-consumers.log" | wc -l)"
[[ "$fragment_markers" -ge 2 ]] \
    || fail "C and C++ fragment read probes did not both execute"

rust_tests="$(rg -c '^test .* \.\.\. ok$' "$EVIDENCE_DIR/logs/20-full-rust.log")"
ignored_tests="$(rg -c '^test .* \.\.\. ok$' "$EVIDENCE_DIR/logs/21-c-cpp-consumers.log")"
{
    printf 'product_commit=%s\n' "$PRODUCT_COMMIT"
    printf 'validation_commit=%s\n' "$(git rev-parse HEAD)"
    printf 'base_commit=%s\n' "$BASE_COMMIT"
    printf 'native_commit=%s\n' "$NATIVE_COMMIT"
    printf 'rust_tests_ok=%s\n' "$rust_tests"
    printf 'ignored_c_cpp_consumers_ok=%s\n' "$ignored_tests"
    printf 'fragment_read_probe_markers=%s\n' "$fragment_markers"
    printf 'toolchain=cargo+%s\n' "$CARGO_TOOLCHAIN"
    printf 'cargo_jobs=8\n'
    printf 'test_threads=4\n'
    printf 'build_storage=mbx-normal\n'
} >"$EVIDENCE_DIR/.summary.tmp"
mv "$EVIDENCE_DIR/.summary.tmp" "$EVIDENCE_DIR/summary"

SUCCESS=1
