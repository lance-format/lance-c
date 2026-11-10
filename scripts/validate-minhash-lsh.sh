#!/usr/bin/env bash
set -uo pipefail

# Validate the MinHash LSH index and text-search slice from the exact product
# commit in a detached Git worktree. The execution host is a non-inference
# worker; evidence and source remain outside the registered checkout.
#
# Usage:
#   scripts/validate-minhash-lsh.sh <evidence-dir> <product-sha> [<validation-sha>]

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
RUN_DIR=${1:?usage: $0 <evidence-dir> <product-sha> [<validation-sha>]}
PRODUCT_SHA=${2:?usage: $0 <evidence-dir> <product-sha> [<validation-sha>]}
EXPECTED_VALIDATION_SHA=${3:-$(git rev-parse HEAD)}
SOURCE_DIR="$RUN_DIR/product-source"
LOG_DIR="$RUN_DIR/logs"
mkdir -p "$LOG_DIR"

export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4
# Select the project MSRV for this run, including the `cargo build` invoked by
# the C/C++ integration test. This does not change the rustup default.
export RUSTUP_TOOLCHAIN=1.94.0

FAILED=0
STEPS=()

preflight_fail() {
    printf 'preflight failed: %s\n' "$1" | tee "$RUN_DIR/preflight.txt"
    printf '2\n' >"$RUN_DIR/overall.exit.tmp"
    mv "$RUN_DIR/overall.exit.tmp" "$RUN_DIR/overall.exit"
    exit 2
}

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

: >"$RUN_DIR/preflight.txt"
{
    printf 'host=%s\n' "$(hostname)"
    printf 'user=%s\n' "$(id -un)"
    printf 'validation_repo=%s\n' "$REPO_ROOT"
    printf 'validation_head=%s\n' "$(git -C "$REPO_ROOT" rev-parse HEAD)"
    printf 'validation_tree=%s\n' "$(git -C "$REPO_ROOT" rev-parse 'HEAD^{tree}')"
    printf 'validation_status=%s\n' \
        "$(git -C "$REPO_ROOT" status --porcelain=v1 | tr '\n' ';')"
    printf 'expected_validation=%s\n' "$EXPECTED_VALIDATION_SHA"
    printf 'product=%s\n' "$PRODUCT_SHA"
    printf 'cargo=%s\n' "$(command -v cargo)"
    printf 'cargo_version=%s\n' "$(cargo +1.94.0 --version)"
    printf 'rustc_version=%s\n' "$(rustc +1.94.0 --version)"
    printf 'mbx=%s\n' "$(command -v mbx)"
    printf 'nproc=%s\n' "$(nproc)"
    printf 'cargo_build_jobs=%s\n' "$CARGO_BUILD_JOBS"
    printf 'rust_test_threads=%s\n' "$RUST_TEST_THREADS"
    printf 'rustup_toolchain=%s\n' "$RUSTUP_TOOLCHAIN"
} >>"$RUN_DIR/preflight.txt" 2>&1

VALIDATION_HEAD=$(git -C "$REPO_ROOT" rev-parse HEAD)
[ "$VALIDATION_HEAD" = "$EXPECTED_VALIDATION_SHA" ] ||
    preflight_fail "validation HEAD does not match expected commit"
[ -z "$(git -C "$REPO_ROOT" status --porcelain=v1)" ] ||
    preflight_fail "validation worktree is not clean"
git -C "$REPO_ROOT" cat-file -e "$PRODUCT_SHA^{commit}" 2>/dev/null ||
    preflight_fail "product commit is unavailable"
if [ "$(git -C "$REPO_ROOT" diff --name-only "$PRODUCT_SHA" "$VALIDATION_HEAD")" != \
    "scripts/validate-minhash-lsh.sh" ]; then
    preflight_fail "validation delta is not confined to the script"
fi
[ ! -e "$SOURCE_DIR" ] || preflight_fail "product source path already exists"
[ -z "${CARGO_TARGET_DIR:-}" ] || preflight_fail "CARGO_TARGET_DIR is set"
[ -z "${CARGO_BUILD_TARGET_DIR:-}" ] || preflight_fail "CARGO_BUILD_TARGET_DIR is set"
[ -z "${CARGO_BUILD_BUILD_DIR:-}" ] || preflight_fail "CARGO_BUILD_BUILD_DIR is set"
if env | grep -q '^CARGO_PROFILE_'; then
    preflight_fail "CARGO_PROFILE_* is set"
fi

git -C "$REPO_ROOT" worktree add --detach "$SOURCE_DIR" "$PRODUCT_SHA" \
    >>"$RUN_DIR/preflight.txt" 2>&1 ||
    preflight_fail "cannot create exact product worktree"
[ "$(git -C "$SOURCE_DIR" rev-parse HEAD)" = "$PRODUCT_SHA" ] ||
    preflight_fail "product worktree HEAD does not match requested commit"
[ -z "$(git -C "$SOURCE_DIR" status --porcelain=v1)" ] ||
    preflight_fail "product worktree is not clean"
[ "$(git -C "$SOURCE_DIR" rev-parse 'HEAD^{tree}')" = \
    "$(git -C "$REPO_ROOT" rev-parse "$PRODUCT_SHA^{tree}")" ] ||
    preflight_fail "product worktree tree does not match requested commit"
cd "$SOURCE_DIR" || preflight_fail "cannot enter product worktree"

{
    printf 'product_source=%s\n' "$SOURCE_DIR"
    printf 'product_head=%s\n' "$(git rev-parse HEAD)"
    printf 'product_tree=%s\n' "$(git rev-parse 'HEAD^{tree}')"
    printf 'product_status=%s\n' "$(git status --porcelain=v1 | tr '\n' ';')"
    printf 'preflight=ok\n'
} >>"$RUN_DIR/preflight.txt"

run_step fmt cargo +1.94.0 fmt -- --check || FAILED=1
run_step check cargo +1.94.0 check --all-targets || FAILED=1
run_step clippy cargo +1.94.0 clippy --all-targets -- -D warnings || FAILED=1
run_step focused_index_lifecycle \
    cargo +1.94.0 test --test c_api_test \
    test_minhash_lsh_index_search_and_lifecycle -- --exact --nocapture ||
    FAILED=1
run_step focused_coverage_composition \
    cargo +1.94.0 test --test c_api_test \
    test_minhash_search_partial_coverage_filters_masks_and_projection -- --exact --nocapture ||
    FAILED=1
run_step focused_validation_copy_errors \
    cargo +1.94.0 test --test c_api_test \
    test_minhash_search_validation_copy_and_native_errors -- --exact --nocapture ||
    FAILED=1
run_step focused_scalar_metadata \
    cargo +1.94.0 test --test c_api_test \
    test_native_scalar_index_segment_metadata_types -- --exact --nocapture ||
    FAILED=1
run_step focused_scalar_validation \
    cargo +1.94.0 test --test c_api_test \
    test_native_scalar_index_native_parameter_and_type_validation -- --exact --nocapture ||
    FAILED=1
run_step full_test cargo +1.94.0 test || FAILED=1
run_step native_consumers \
    cargo +1.94.0 test --test compile_and_run_test -- --ignored --nocapture ||
    FAILED=1

{
    printf 'overall=%s\n' "$([ "$FAILED" -eq 0 ] && printf pass || printf fail)"
    printf 'validation_head=%s\n' "$VALIDATION_HEAD"
    printf 'validation_tree=%s\n' "$(git -C "$REPO_ROOT" rev-parse 'HEAD^{tree}')"
    printf 'product=%s\n' "$PRODUCT_SHA"
    printf 'product_head=%s\n' "$(git -C "$SOURCE_DIR" rev-parse HEAD)"
    printf 'product_tree=%s\n' "$(git -C "$SOURCE_DIR" rev-parse 'HEAD^{tree}')"
    printf 'product_status=%s\n' \
        "$(git -C "$SOURCE_DIR" status --porcelain=v1 | tr '\n' ';')"
    for name in "${STEPS[@]}"; do
        printf '%s=%s\n' "$name" "$(cat "$LOG_DIR/$name.exit")"
    done
} >"$RUN_DIR/summary.txt.tmp"
mv "$RUN_DIR/summary.txt.tmp" "$RUN_DIR/summary.txt"
printf '%s\n' "$FAILED" >"$RUN_DIR/overall.exit.tmp"
mv "$RUN_DIR/overall.exit.tmp" "$RUN_DIR/overall.exit"

exit "$FAILED"
