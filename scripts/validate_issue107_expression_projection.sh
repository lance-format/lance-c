#!/usr/bin/env bash
set -euo pipefail

PRODUCT_DIR=${1:?product directory}
EVIDENCE_DIR=${2:?evidence directory}
EXPECTED_HEAD=${3:?expected product commit}

mkdir -p "$EVIDENCE_DIR"
overall_exit="$EVIDENCE_DIR/runner.exit"
record_runner_exit() {
    local code=$?
    printf '%s\n' "$code" >"$overall_exit.tmp"
    mv "$overall_exit.tmp" "$overall_exit"
}
trap record_runner_exit EXIT

actual_head=$(git -C "$PRODUCT_DIR" rev-parse HEAD)
if [[ "$actual_head" != "$EXPECTED_HEAD" ]]; then
    printf 'product HEAD mismatch: expected %s, got %s\n' "$EXPECTED_HEAD" "$actual_head" >&2
    exit 2
fi

cd "$PRODUCT_DIR"
export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4
export CARGO_TERM_COLOR=never

run_phase() {
    local name=$1
    shift
    local log="$EVIDENCE_DIR/$name.log"
    local log_tmp="$log.tmp"
    local exit_tmp="$EVIDENCE_DIR/$name.exit.tmp"
    local code

    set +e
    "$@" >"$log_tmp" 2>&1
    code=$?
    set -e
    mv "$log_tmp" "$log"
    printf '%s\n' "$code" >"$exit_tmp"
    mv "$exit_tmp" "$EVIDENCE_DIR/$name.exit"
    printf '[%s] exit %s\n' "$name" "$code"
    return "$code"
}

run_phase fmt cargo +1.94.0 fmt --all -- --check
run_phase check cargo +1.94.0 check --all-targets
run_phase clippy cargo +1.94.0 clippy --all-targets -- -D warnings
run_phase focused_expression_projection cargo +1.94.0 test --test c_api_test test_scanner_expression_projection -- --nocapture
run_phase focused_projection_setter cargo +1.94.0 test --test c_api_test test_scanner_projection_setter_contract -- --nocapture
run_phase focused_vector_scoring cargo +1.94.0 test --test c_api_test test_scanner_vector_projection_and_scoring_autoprojection -- --nocapture
run_phase focused_fts_scoring cargo +1.94.0 test --test c_api_test test_scanner_fts_projection_and_scoring_autoprojection -- --nocapture
run_phase focused_prepared_fts cargo +1.94.0 test --test c_api_test test_prepared_fts_expression_projection_and_scoring_control -- --nocapture
run_phase focused_scalar_segment cargo +1.94.0 test --test c_api_test test_scalar_segment_scan_applies_expression_projection -- --nocapture
run_phase focused_batch_nearest cargo +1.94.0 test --test batch_nearest_test batch_nearest_projection_and_scoring_autoprojection -- --nocapture
run_phase focused_multivector cargo +1.94.0 test --test multivector_test multivector_projection_keeps_the_internal_scoring_window -- --nocapture
run_phase full_rust_tests cargo +1.94.0 test
run_phase native_consumers cargo +1.94.0 test --test compile_and_run_test -- --ignored

summary_tmp="$EVIDENCE_DIR/summary.txt.tmp"
{
    printf 'product_head=%s\n' "$EXPECTED_HEAD"
    printf 'product_status='; git status --porcelain=v1 --branch | head -1
    printf 'rust_version='; rustc +1.94.0 --version
    printf 'cargo_version='; cargo +1.94.0 --version
    printf 'host='; hostname
    printf 'completed_at_utc='; date -u +%Y-%m-%dT%H:%M:%SZ
} >"$summary_tmp"
mv "$summary_tmp" "$EVIDENCE_DIR/summary.txt"
