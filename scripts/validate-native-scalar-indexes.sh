#!/usr/bin/env bash
set -uo pipefail

# Validate the native NGram/ZoneMap/BloomFilter scalar-index slice on a clean
# checkout. The execution host is expected to be a non-inference worker.
#
# Usage:
#   scripts/validate-native-scalar-indexes.sh <evidence-dir> <product-sha> <expected-sha>

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
RUN_DIR=${1:?usage: $0 <evidence-dir> <product-sha> <expected-sha>}
PRODUCT_SHA=${2:?usage: $0 <evidence-dir> <product-sha> <expected-sha>}
EXPECTED_SHA=${3:-$(git rev-parse HEAD)}
LOG_DIR="$RUN_DIR/logs"
mkdir -p "$LOG_DIR"

cd "$REPO_ROOT"
export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4
# Select the project MSRV for this run, including the `cargo build` invoked by
# the C/C++ integration test. This does not change the rustup default.
export RUSTUP_TOOLCHAIN=1.94.0

FAILED=0
STEPS=()

run_step() {
    local name=$1
    shift
    local log="$LOG_DIR/$name.log"
    STEPS+=("$name")
    {
        printf 'started_at=%s\n' "$(date -u +%FT%TZ)"
        printf 'cwd=%s\n' "$PWD"
        printf 'command='
        printf '%q ' "$@"
        printf '\n'
    } >"$log"
    "$@" >>"$log" 2>&1
    local rc=$?
    printf '%s\n' "$rc" >"$LOG_DIR/$name.exit.tmp"
    mv "$LOG_DIR/$name.exit.tmp" "$LOG_DIR/$name.exit"
    printf 'finished_at=%s\n' "$(date -u +%FT%TZ)" >>"$log"
    printf 'exit=%s\n' "$rc" >>"$log"
    return "$rc"
}

preflight_fail() {
    printf 'preflight failed: %s\n' "$1" | tee "$RUN_DIR/preflight.txt"
    exit 2
}

: >"$RUN_DIR/preflight.txt"
{
    printf 'host=%s\n' "$(hostname)"
    printf 'user=%s\n' "$(id -un)"
    printf 'repo_root=%s\n' "$REPO_ROOT"
    printf 'head=%s\n' "$(git rev-parse HEAD)"
    printf 'head_tree=%s\n' "$(git rev-parse 'HEAD^{tree}')"
    printf 'product=%s\n' "$PRODUCT_SHA"
    printf 'product_tree=%s\n' "$(git rev-parse "$PRODUCT_SHA^{tree}")"
    printf 'expected=%s\n' "$EXPECTED_SHA"
    printf 'status=%s\n' "$(git status --porcelain=v1 | tr '\n' ';')"
    printf 'cargo=%s\n' "$(command -v cargo)"
    printf 'cargo_version=%s\n' "$(cargo +1.94.0 --version)"
    printf 'rustc_version=%s\n' "$(rustc +1.94.0 --version)"
    printf 'nproc=%s\n' "$(nproc)"
    printf 'cargo_build_jobs=%s\n' "$CARGO_BUILD_JOBS"
    printf 'rust_test_threads=%s\n' "$RUST_TEST_THREADS"
    printf 'rustup_toolchain=%s\n' "$RUSTUP_TOOLCHAIN"
} >>"$RUN_DIR/preflight.txt" 2>&1

HEAD_SHA=$(git rev-parse HEAD)
[ "$HEAD_SHA" = "$EXPECTED_SHA" ] || preflight_fail "HEAD does not match expected commit"
[ -z "$(git status --porcelain=v1)" ] || preflight_fail "worktree is not clean"
[ -z "${CARGO_TARGET_DIR:-}" ] || preflight_fail "CARGO_TARGET_DIR is set"
[ -z "${CARGO_BUILD_TARGET_DIR:-}" ] || preflight_fail "CARGO_BUILD_TARGET_DIR is set"
[ -z "${CARGO_BUILD_BUILD_DIR:-}" ] || preflight_fail "CARGO_BUILD_BUILD_DIR is set"
if env | grep -q '^CARGO_PROFILE_'; then
    preflight_fail "CARGO_PROFILE_* is set"
fi
if [ "$(git diff --name-only "$PRODUCT_SHA" "$HEAD_SHA")" != \
    "scripts/validate-native-scalar-indexes.sh" ]; then
    preflight_fail "branch delta is not confined to the validation script"
fi
printf 'preflight=ok\n' >>"$RUN_DIR/preflight.txt"

run_step fmt cargo +1.94.0 fmt -- --check || FAILED=1
run_step check cargo +1.94.0 check --all-targets || FAILED=1
run_step clippy cargo +1.94.0 clippy --all-targets -- -D warnings || FAILED=1
run_step focused_lifecycle \
    cargo +1.94.0 test --test c_api_test \
    test_native_scalar_index_families_create_query_and_lifecycle -- --exact --nocapture ||
    FAILED=1
run_step focused_metadata \
    cargo +1.94.0 test --test c_api_test \
    test_native_scalar_index_segment_metadata_types -- --exact --nocapture ||
    FAILED=1
run_step focused_validation \
    cargo +1.94.0 test --test c_api_test \
    test_native_scalar_index_native_parameter_and_type_validation -- --exact --nocapture ||
    FAILED=1
run_step focused_scoped \
    cargo +1.94.0 test --test c_api_test \
    test_scalar_segment_zonemap_and_bloom_filter_fall_back -- --exact --nocapture ||
    FAILED=1
run_step full_test cargo +1.94.0 test || FAILED=1
run_step c_cpp_consumers \
    cargo +1.94.0 test --test compile_and_run_test -- --ignored ||
    FAILED=1

{
    printf 'overall=%s\n' "$([ "$FAILED" -eq 0 ] && printf pass || printf fail)"
    printf 'head=%s\n' "$HEAD_SHA"
    printf 'head_tree=%s\n' "$(git rev-parse 'HEAD^{tree}')"
    printf 'product=%s\n' "$PRODUCT_SHA"
    printf 'product_tree=%s\n' "$(git rev-parse "$PRODUCT_SHA^{tree}")"
    for name in "${STEPS[@]}"; do
        printf '%s=%s\n' "$name" "$(cat "$LOG_DIR/$name.exit")"
    done
} >"$RUN_DIR/summary.txt.tmp"
mv "$RUN_DIR/summary.txt.tmp" "$RUN_DIR/summary.txt"

exit "$FAILED"
