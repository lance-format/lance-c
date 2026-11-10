#!/usr/bin/env bash
set -euo pipefail

: "${PRODUCT_COMMIT:?PRODUCT_COMMIT must name the exact product commit}"
: "${PRODUCT_TREE:?PRODUCT_TREE must name the expected product tree}"
: "${REPO:?REPO must name the clean product worktree}"

readonly BASE_COMMIT="6394be44faedf9589389fa4d5aed4bd55cdfd02d"
readonly NATIVE_COMMIT="40f15ed1b71b66950961b175829fdf17a763f108"
readonly RUN_ID="${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)-$$}"
readonly EVIDENCE_ROOT="${EVIDENCE_ROOT:-/home/xuanwo/exec/lance-c-issue113/evidence}"
readonly RUN_DIR="$EVIDENCE_ROOT/$RUN_ID"
readonly LOG_DIR="$RUN_DIR/logs"

mkdir -p "$LOG_DIR"
export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4
export RUSTUP_TOOLCHAIN=1.94.0

FAILED_STEP=""
STARTED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

write_final() {
    local state="$1"
    local exit_code="$2"
    local validation_commit
    validation_commit="$(git -C "$REPO" rev-parse HEAD 2>/dev/null || printf 'unknown')"
    local tmp="$RUN_DIR/final-status.json.tmp"
    printf '{"run_id":"%s","state":"%s","failed_step":"%s","exit":%d,"product_commit":"%s","product_tree":"%s","validation_commit":"%s","base_commit":"%s","native_commit":"%s","started_at":"%s","finished_at":"%s"}\n' \
        "$RUN_ID" "$state" "$FAILED_STEP" "$exit_code" \
        "$PRODUCT_COMMIT" "$PRODUCT_TREE" "$validation_commit" \
        "$BASE_COMMIT" "$NATIVE_COMMIT" "$STARTED_AT" \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$tmp"
    mv "$tmp" "$RUN_DIR/final-status.json"
}

finish() {
    local exit_code=$?
    if [[ "$exit_code" -eq 0 ]]; then
        write_final "success" 0
    else
        write_final "failure" "$exit_code"
    fi
    exit "$exit_code"
}
trap finish EXIT

fail() {
    FAILED_STEP="$1"
    printf '[%s] FAIL %s: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$1" "$2" \
        | tee -a "$RUN_DIR/driver.log" >&2
    exit 1
}

record_preflight() {
    {
        printf 'host=%s\n' "$(hostname)"
        printf 'user=%s\n' "$(id -un)"
        printf 'uname=%s\n' "$(uname -srm)"
        printf 'repo=%s\n' "$REPO"
        printf 'head=%s\n' "$(git -C "$REPO" rev-parse HEAD)"
        printf 'head_tree=%s\n' "$(git -C "$REPO" rev-parse 'HEAD^{tree}')"
        printf 'product=%s\n' "$PRODUCT_COMMIT"
        printf 'product_tree=%s\n' "$PRODUCT_TREE"
        printf 'base=%s\n' "$BASE_COMMIT"
        printf 'native=%s\n' "$NATIVE_COMMIT"
        printf 'status=%s\n' "$(git -C "$REPO" status --porcelain=v1 | tr '\n' ';')"
        printf 'cargo=%s\n' "$(command -v cargo)"
        printf 'mbx=%s\n' "$(command -v mbx)"
        printf 'cargo_version=%s\n' "$(cargo +1.94.0 --version)"
        printf 'rustc_version=%s\n' "$(rustc +1.94.0 --version)"
        printf 'nproc=%s\n' "$(nproc)"
        printf 'cargo_build_jobs=%s\n' "$CARGO_BUILD_JOBS"
        printf 'rust_test_threads=%s\n' "$RUST_TEST_THREADS"
        printf 'rustup_toolchain=%s\n' "$RUSTUP_TOOLCHAIN"
    } >"$RUN_DIR/preflight.txt" 2>&1
}

run_step() {
    local step="$1"
    shift
    local log="$LOG_DIR/$step.log"
    {
        printf 'started_at=%s\n' "$(date -u +%FT%TZ)"
        printf 'cwd=%s\n' "$REPO"
        printf 'command='
        printf '%q ' "$@"
        printf '\n'
    } >"$log"
    printf '[%s] START %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$step" \
        | tee -a "$RUN_DIR/driver.log"
    set +e
    (
        cd "$REPO"
        "$@"
    ) >>"$log" 2>&1
    local exit_code=$?
    set -e
    printf 'finished_at=%s\nexit=%s\n' \
        "$(date -u +%FT%TZ)" "$exit_code" >>"$log"
    printf '%s\n' "$exit_code" >"$LOG_DIR/$step.exit.tmp"
    mv "$LOG_DIR/$step.exit.tmp" "$LOG_DIR/$step.exit"
    printf '[%s] END %s exit=%s\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$step" "$exit_code" \
        | tee -a "$RUN_DIR/driver.log"
    if [[ "$exit_code" -ne 0 ]]; then
        FAILED_STEP="$step"
        return "$exit_code"
    fi
}

record_preflight

[[ "$(hostname)" != "adam.xuanwo.io" ]] \
    || fail "preflight" "heavy validation must not run on Adam"
[[ "$(git -C "$REPO" rev-parse HEAD)" == "$PRODUCT_COMMIT" ]] \
    || fail "commit_identity" "HEAD does not match PRODUCT_COMMIT"
[[ "$(git -C "$REPO" rev-parse 'HEAD^{tree}')" == "$PRODUCT_TREE" ]] \
    || fail "tree_identity" "HEAD tree does not match PRODUCT_TREE"
[[ -z "$(git -C "$REPO" status --porcelain=v1)" ]] \
    || fail "clean_source" "product worktree is not clean"
git -C "$REPO" merge-base --is-ancestor "$BASE_COMMIT" "$PRODUCT_COMMIT" \
    || fail "product_ancestry" "product does not descend from the accepted base"
git -C "$REPO" diff --exit-code "$BASE_COMMIT" "$PRODUCT_COMMIT" -- \
    Cargo.toml Cargo.lock \
    || fail "dependency_contract" "product changed Cargo.toml or Cargo.lock"
grep -q "$NATIVE_COMMIT" "$REPO/Cargo.toml" \
    || fail "native_pin" "Cargo.toml does not reference the accepted native commit"

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
        fail "build_environment" "unexpected override $variable=${!variable}"
    fi
done

if env | grep -q '^CARGO_PROFILE_'; then
    fail "build_environment" "CARGO_PROFILE_* must not be set"
fi

printf '[%s] product=%s tree=%s run_id=%s\n' \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$PRODUCT_COMMIT" "$PRODUCT_TREE" "$RUN_ID" \
    | tee -a "$RUN_DIR/driver.log"

run_step 10-fmt cargo +1.94.0 fmt --all -- --check
run_step 11-check cargo +1.94.0 check --all-targets -j 8
run_step 12-clippy cargo +1.94.0 clippy --all-targets -j 8 -- -D warnings

run_step 20-focused-selectors \
    cargo +1.94.0 test -j 8 --test c_api_test \
    test_cleanup_selectors_explanation_owned_output_and_reexecution \
    -- --exact --nocapture --test-threads=4
run_step 21-focused-retain \
    cargo +1.94.0 test -j 8 --test c_api_test \
    test_cleanup_retain_native_defaults_invalid_forms_and_operation_errors \
    -- --exact --nocapture --test-threads=4
run_step 22-focused-relative \
    cargo +1.94.0 test -j 8 --test c_api_test \
    test_cleanup_relative_duration_and_unverified_candidates \
    -- --exact --nocapture --test-threads=4
run_step 23-focused-lifecycle \
    cargo +1.94.0 test -j 8 --test c_api_test \
    test_dataset_validation_deleted_rows_and_small_files \
    -- --exact --nocapture --test-threads=4
run_step 24-focused-branches \
    cargo +1.94.0 test -j 8 --test c_api_test \
    test_cleanup_branch_cascade_and_copied_explanation \
    -- --exact --nocapture --test-threads=4

run_step 30-full-rust \
    cargo +1.94.0 test -j 8 -- --test-threads=4
run_step 40-native-consumers \
    cargo +1.94.0 test -j 8 --test compile_and_run_test \
    -- --ignored --nocapture --test-threads=4

for test_name in \
    test_cleanup_selectors_explanation_owned_output_and_reexecution \
    test_cleanup_retain_native_defaults_invalid_forms_and_operation_errors \
    test_cleanup_relative_duration_and_unverified_candidates \
    test_dataset_validation_deleted_rows_and_small_files \
    test_cleanup_branch_cascade_and_copied_explanation
do
    case "$test_name" in
        test_cleanup_selectors_*) log_name="20-focused-selectors" ;;
        test_cleanup_retain_*) log_name="21-focused-retain" ;;
        test_cleanup_relative_*) log_name="22-focused-relative" ;;
        test_dataset_validation_*) log_name="23-focused-lifecycle" ;;
        test_cleanup_branch_*) log_name="24-focused-branches" ;;
    esac
    grep -q "^test ${test_name} \\.\\.\\. ok$" "$LOG_DIR/$log_name.log" \
        || fail "focused_assertion" "focused test did not pass: $test_name"
done

for test_name in \
    test_c_compilation_and_execution \
    test_cpp_compilation_and_execution \
    test_typed_fts_native_compilation_and_execution \
    test_static_oss_transport
do
    grep -q "^test ${test_name} \\.\\.\\. ok$" "$LOG_DIR/40-native-consumers.log" \
        || fail "consumer_assertion" "ignored consumer did not pass: $test_name"
done

grep -q 'All C tests passed!' "$LOG_DIR/40-native-consumers.log" \
    || fail "consumer_assertion" "C consumer completion marker is missing"
grep -q 'All C++ tests passed!' "$LOG_DIR/40-native-consumers.log" \
    || fail "consumer_assertion" "C++ consumer completion marker is missing"
cleanup_markers="$(grep -o 'test_cleanup_and_lifecycle\\.\\.\\. OK' \
    "$LOG_DIR/40-native-consumers.log" | wc -l)"
[[ "$cleanup_markers" -ge 2 ]] \
    || fail "consumer_assertion" "C and C++ cleanup probes did not both execute"

rust_tests="$(grep -c '^test .* \\.\\.\\. ok$' "$LOG_DIR/30-full-rust.log" || true)"
consumer_tests="$(grep -c '^test .* \\.\\.\\. ok$' "$LOG_DIR/40-native-consumers.log" || true)"
{
    printf 'product_commit=%s\n' "$PRODUCT_COMMIT"
    printf 'product_tree=%s\n' "$PRODUCT_TREE"
    printf 'validation_commit=%s\n' "$(git -C "$REPO" rev-parse HEAD)"
    printf 'base_commit=%s\n' "$BASE_COMMIT"
    printf 'native_commit=%s\n' "$NATIVE_COMMIT"
    printf 'rust_tests_ok=%s\n' "$rust_tests"
    printf 'native_consumers_ok=%s\n' "$consumer_tests"
    printf 'cleanup_probe_markers=%s\n' "$cleanup_markers"
    printf 'toolchain=cargo+1.94.0\n'
    printf 'cargo_jobs=8\n'
    printf 'test_threads=4\n'
    printf 'build_storage=mbx-normal\n'
    printf 'remote_fault_injection=not-covered\n'
} >"$RUN_DIR/summary.txt.tmp"
mv "$RUN_DIR/summary.txt.tmp" "$RUN_DIR/summary.txt"

printf '0\n' >"$RUN_DIR/overall.exit.tmp"
mv "$RUN_DIR/overall.exit.tmp" "$RUN_DIR/overall.exit"
printf '[%s] ALL STEPS PASSED\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    | tee -a "$RUN_DIR/driver.log"
