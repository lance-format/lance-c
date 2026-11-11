#!/usr/bin/env bash
set -euo pipefail

PRODUCT_ROOT=${PRODUCT_ROOT:?set PRODUCT_ROOT to the exact product checkout}
EVIDENCE_DIR=${EVIDENCE_DIR:?set EVIDENCE_DIR outside the target cache}
BASE_COMMIT=930b02c27eb59f939ef4cf1dd227063c3f94460e
EXPECTED_HEAD=9bdca24304cad51bfde8078d874152cb5675fa37
EXPECTED_TREE=7918f21b1562987af6d33769b873e563c9f08871
NATIVE_PIN=40f15ed1b71b66950961b175829fdf17a763f108

for name in CARGO_TARGET_DIR CARGO_HOME RUSTFLAGS LANCE_C_LINK; do
    if printenv "$name" >/dev/null 2>&1; then
        printf 'unexpected environment override: %s\n' "$name" >&2
        exit 2
    fi
done
export RUSTUP_TOOLCHAIN=1.94.0
export CARGO_BUILD_JOBS=8

mkdir -p "$EVIDENCE_DIR/logs" "$EVIDENCE_DIR/status"
STATUS_TMP="$EVIDENCE_DIR/status/overall.tmp"
printf '1\n' >"$STATUS_TMP"

{
    printf 'product_root=%s\n' "$PRODUCT_ROOT"
    printf 'evidence_dir=%s\n' "$EVIDENCE_DIR"
    printf 'base_commit=%s\n' "$BASE_COMMIT"
    printf 'expected_head=%s\n' "$EXPECTED_HEAD"
    printf 'expected_tree=%s\n' "$EXPECTED_TREE"
    printf 'native_pin=%s\n' "$NATIVE_PIN"
    printf 'rustup_toolchain=%s\n' "$RUSTUP_TOOLCHAIN"
    printf 'cargo_build_jobs=%s\n' "$CARGO_BUILD_JOBS"
    printf 'path=%s\n' "$PATH"
    date -u '+utc_start=%Y-%m-%dT%H:%M:%SZ'
    git -C "$PRODUCT_ROOT" status --short --branch
    git -C "$PRODUCT_ROOT" rev-parse HEAD
    git -C "$PRODUCT_ROOT" rev-parse 'HEAD^{tree}'
    git -C "$PRODUCT_ROOT" log -1 --format='%H%n%s'
    git -C "$PRODUCT_ROOT" diff --stat "$BASE_COMMIT..$EXPECTED_HEAD"
    git -C "$PRODUCT_ROOT" diff --name-status "$BASE_COMMIT..$EXPECTED_HEAD"
    printf 'native_pin_zaezf_count='
    grep -c "$NATIVE_PIN" "$PRODUCT_ROOT/Cargo.toml"
    printf 'native_pin_lock_count='
    grep -c "$NATIVE_PIN" "$PRODUCT_ROOT/Cargo.lock"
    command -v cargo
    cargo --version
    rustc --version
    clang --version
    clang++ --version
    uname -a
    lscpu
    free -h
    uptime
} >"$EVIDENCE_DIR/environment.txt" 2>&1

[[ $(git -C "$PRODUCT_ROOT" rev-parse HEAD) == "$EXPECTED_HEAD" ]]
[[ $(git -C "$PRODUCT_ROOT" rev-parse 'HEAD^{tree}') == "$EXPECTED_TREE" ]]
[[ -z $(git -C "$PRODUCT_ROOT" status --porcelain) ]]
git -C "$PRODUCT_ROOT" merge-base --is-ancestor "$BASE_COMMIT" "$EXPECTED_HEAD"
[[ -z $(git -C "$PRODUCT_ROOT" diff --name-only "$BASE_COMMIT..$EXPECTED_HEAD" -- Cargo.toml Cargo.lock) ]]
grep -q "$NATIVE_PIN" "$PRODUCT_ROOT/Cargo.toml"
grep -q "$NATIVE_PIN" "$PRODUCT_ROOT/Cargo.lock"

run_step() {
    local name=$1
    shift
    local log="$EVIDENCE_DIR/logs/$name.log"
    {
        printf '$'
        printf ' %q' "$@"
        printf '\n'
    } >"$log"
    printf 'starting %s\n' "$name"
    local rc
    set +e
    "$@" >>"$log" 2>&1
    rc=$?
    set -e
    printf '%s\n' "$rc" >"$EVIDENCE_DIR/status/$name"
    if ((rc != 0)); then
        printf 'failed step=%s exit=%s log=%s\n' "$name" "$rc" "$log" >&2
        printf '%s\n' "$rc" >"$STATUS_TMP"
        mv "$STATUS_TMP" "$EVIDENCE_DIR/status/overall"
        exit "$rc"
    fi
    printf 'finished %s exit=0\n' "$name"
}

cd "$PRODUCT_ROOT"
run_step fmt cargo fmt --all -- --check
run_step check cargo check --all-targets --locked -j 8
run_step clippy cargo clippy --all-targets --locked -j 8 -- -D warnings
run_step offset_contract cargo test --locked --test c_api_test -j 8 \
    test_native_updated_fragment_offsets_copy_at_contract -- \
    --test-threads=4
run_step offset_adjacent cargo test --locked --test c_api_test -j 8 \
    test_native_update_offsets_and_key_existence_filter_values -- \
    --test-threads=4
run_step c_cpp_consumers cargo test --locked --test compile_and_run_test \
    test_native_fragment_mutation_consumers -j 8 -- --ignored --nocapture \
    --test-threads=1

grep -Fq 'running 1 test' "$EVIDENCE_DIR/logs/offset_contract.log"
grep -Fq 'running 1 test' "$EVIDENCE_DIR/logs/offset_adjacent.log"
grep -Fq 'running 1 test' "$EVIDENCE_DIR/logs/c_cpp_consumers.log"
grep -Fq 'fragment update offsets -> typed Update -> reopen OK' \
    "$EVIDENCE_DIR/logs/c_cpp_consumers.log"
grep -Fq 'C++ update offsets -> typed Update -> reopen OK' \
    "$EVIDENCE_DIR/logs/c_cpp_consumers.log"

{
    printf 'target_directory='
    cargo metadata --format-version 1 --no-deps | jq -r '.target_directory'
    git status --short --branch
    git rev-parse HEAD
    git rev-parse 'HEAD^{tree}'
    date -u '+utc_end=%Y-%m-%dT%H:%M:%SZ'
    uptime
} >>"$EVIDENCE_DIR/environment.txt" 2>&1
[[ -z $(git status --porcelain) ]]

printf '0\n' >"$STATUS_TMP"
mv "$STATUS_TMP" "$EVIDENCE_DIR/status/overall"
printf 'validation passed\n'
