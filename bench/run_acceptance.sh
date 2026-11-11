#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BENCH_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
PRODUCT_ROOT=${PRODUCT_ROOT:?set PRODUCT_ROOT to the exact product checkout}
EVIDENCE_DIR=${EVIDENCE_DIR:?set EVIDENCE_DIR outside the target cache}

CANDIDATE=5ee746000d64f78897095f3a57469a3b77f4c8ec
CANDIDATE_TREE=bd6b3a71fdb5019fc8595cadffde10337a99fe00
BASELINE=27b2754652e886dc955502c18c02bf5517dbb8e0

for name in CARGO_TARGET_DIR CARGO_HOME RUSTFLAGS LANCE_C_LINK; do
    if printenv "$name" >/dev/null 2>&1; then
        printf 'unexpected environment override: %s\n' "$name" >&2
        exit 2
    fi
done
export RUSTUP_TOOLCHAIN=1.94.0

mkdir -p "$EVIDENCE_DIR/logs" "$EVIDENCE_DIR/status"
STATUS_TMP="$EVIDENCE_DIR/status/overall.tmp"
printf '1\n' >"$STATUS_TMP"

{
    printf 'product_root=%s\n' "$PRODUCT_ROOT"
    printf 'bench_root=%s\n' "$BENCH_ROOT"
    printf 'evidence_dir=%s\n' "$EVIDENCE_DIR"
    printf 'candidate=%s\n' "$CANDIDATE"
    printf 'candidate_tree=%s\n' "$CANDIDATE_TREE"
    printf 'baseline=%s\n' "$BASELINE"
    printf 'rustup_toolchain=%s\n' "$RUSTUP_TOOLCHAIN"
    printf 'path=%s\n' "$PATH"
    date -u '+utc_start=%Y-%m-%dT%H:%M:%SZ'
    git -C "$PRODUCT_ROOT" status --short --branch
    git -C "$PRODUCT_ROOT" rev-parse HEAD
    git -C "$PRODUCT_ROOT" rev-parse 'HEAD^{tree}'
    git -C "$PRODUCT_ROOT" log -1 --format='%H%n%s'
    git -C "$PRODUCT_ROOT" diff --stat "$BASELINE..$CANDIDATE"
    command -v cargo
    cargo --version
    rustc --version
    uname -a
    lscpu
    free -h
    uptime
} >"$EVIDENCE_DIR/environment.txt" 2>&1

[[ $(git -C "$PRODUCT_ROOT" rev-parse HEAD) == "$CANDIDATE" ]]
[[ $(git -C "$PRODUCT_ROOT" rev-parse 'HEAD^{tree}') == "$CANDIDATE_TREE" ]]
[[ -z $(git -C "$PRODUCT_ROOT" status --porcelain) ]]
git -C "$PRODUCT_ROOT" merge-base --is-ancestor "$BASELINE" "$CANDIDATE"
[[ -z $(git -C "$PRODUCT_ROOT" diff --name-only "$BASELINE..$CANDIDATE" -- Cargo.toml Cargo.lock) ]]
grep -q '40f15ed1b71b66950961b175829fdf17a763f108' "$PRODUCT_ROOT/Cargo.toml"

run_step() {
    local name=$1
    shift
    local rc
    {
        printf '$'
        printf ' %q' "$@"
        printf '\n'
    } | tee "$EVIDENCE_DIR/logs/$name.log"
    set +e
    "$@" 2>&1 | tee -a "$EVIDENCE_DIR/logs/$name.log"
    rc=${PIPESTATUS[0]}
    set -e
    printf '%s\n' "$rc" >"$EVIDENCE_DIR/status/$name"
    if ((rc != 0)); then
        printf 'failed step=%s exit=%s\n' "$name" "$rc" | tee -a \
            "$EVIDENCE_DIR/logs/$name.log"
        printf '%s\n' "$rc" >"$STATUS_TMP"
        mv "$STATUS_TMP" "$EVIDENCE_DIR/status/overall"
        exit "$rc"
    fi
}

cd "$PRODUCT_ROOT"
run_step fmt cargo fmt --all -- --check
run_step check cargo check --all-targets --locked -j 8
run_step clippy cargo clippy --all-targets --locked -j 8 -- -D warnings
run_step focused_matched_offsets cargo test --locked --lib -j 8 \
    matched_offsets_copy_preserves_values_and_error_outputs -- \
    --test-threads=4
run_step fragment_mutation_tests cargo test --locked \
    --test fragment_mutation_test -j 8 -- --test-threads=4
run_step c_cpp_consumers cargo test --locked --test compile_and_run_test \
    test_native_fragment_mutation_consumers -j 8 -- --ignored --nocapture \
    --test-threads=1

printf '0\n' >"$STATUS_TMP"
mv "$STATUS_TMP" "$EVIDENCE_DIR/status/overall"
printf 'acceptance passed\n'
