#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BENCH_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
PRODUCT_ROOT=${PRODUCT_ROOT:?set PRODUCT_ROOT to the exact product checkout}
EVIDENCE_DIR=${EVIDENCE_DIR:?set EVIDENCE_DIR outside the target cache}

CANDIDATE=5ee746000d64f78897095f3a57469a3b77f4c8ec
CANDIDATE_TREE=bd6b3a71fdb5019fc8595cadffde10337a99fe00
BASELINE=27b2754652e886dc955502c18c02bf5517dbb8e0
SIZES=(4096 16384 32768)

for name in CARGO_TARGET_DIR CARGO_HOME RUSTFLAGS LANCE_C_LINK; do
    if printenv "$name" >/dev/null 2>&1; then
        printf 'unexpected environment override: %s\n' "$name" >&2
        exit 2
    fi
done
export RUSTUP_TOOLCHAIN=1.94.0

mkdir -p "$EVIDENCE_DIR/bin" "$EVIDENCE_DIR/include_old/lance" \
    "$EVIDENCE_DIR/logs" "$EVIDENCE_DIR/status"
STATUS_TMP="$EVIDENCE_DIR/status/overall.tmp"
printf '1\n' >"$STATUS_TMP"

{
    printf 'product_root=%s\n' "$PRODUCT_ROOT"
    printf 'bench_root=%s\n' "$BENCH_ROOT"
    printf 'evidence_dir=%s\n' "$EVIDENCE_DIR"
    printf 'candidate=%s\n' "$CANDIDATE"
    printf 'candidate_tree=%s\n' "$CANDIDATE_TREE"
    printf 'baseline=%s\n' "$BASELINE"
    printf 'profile=release\n'
    printf 'cpp_flags=-std=c++17 -O3 -DNDEBUG -Wall -Wextra\n'
    printf 'rustup_toolchain=%s\n' "$RUSTUP_TOOLCHAIN"
    printf 'path=%s\n' "$PATH"
    date -u '+utc_start=%Y-%m-%dT%H:%M:%SZ'
    git -C "$PRODUCT_ROOT" status --short --branch
    git -C "$PRODUCT_ROOT" rev-parse HEAD
    git -C "$PRODUCT_ROOT" rev-parse 'HEAD^{tree}'
    git -C "$BENCH_ROOT" rev-parse HEAD
    git -C "$BENCH_ROOT" log -1 --format='%H%n%s'
    git -C "$BENCH_ROOT" diff --name-only "$CANDIDATE..HEAD"
    command -v cargo
    cargo --version
    rustc --version
    c++ --version
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

if git -C "$BENCH_ROOT" diff --name-only "$CANDIDATE..HEAD" |
    grep -v '^bench/' >/dev/null; then
    printf 'benchmark branch changes product files\n' >&2
    exit 2
fi

mapfile -t rust_deletions < <(
    git -C "$PRODUCT_ROOT" diff --numstat "$BASELINE..$CANDIDATE" -- \
        src/fragment_mutation.rs
)
IFS=$'\t' read -r _ removed _ <<<"${rust_deletions[0]}"
[[ $removed == 0 ]]

git -C "$PRODUCT_ROOT" show "$BASELINE:include/lance/lance.h" \
    >"$EVIDENCE_DIR/include_old/lance/lance.h"
git -C "$PRODUCT_ROOT" show "$BASELINE:include/lance/lance.hpp" \
    >"$EVIDENCE_DIR/include_old/lance/lance.hpp"
git -C "$PRODUCT_ROOT" diff "$BASELINE..$CANDIDATE" -- \
    src/fragment_mutation.rs >"$EVIDENCE_DIR/fragment_mutation.diff"

cd "$PRODUCT_ROOT"
cargo build --release --locked --lib -j 8 \
    >"$EVIDENCE_DIR/logs/release_build.log" 2>&1
TARGET_DIR=$(cargo metadata --format-version 1 --no-deps |
    jq -r '.target_directory')
LIB_DIR="$TARGET_DIR/release"
LIB="$LIB_DIR/liblance_c.so"
[[ -f $LIB ]]
sha256sum "$LIB" >"$EVIDENCE_DIR/library_sha256.txt"
nm -D "$LIB" | grep \
    -E 'lance_fragment_update_columns_result_matched_offset_(at|count)|lance_fragment_update_columns_result_matched_offsets$' \
    >"$EVIDENCE_DIR/library_symbols.txt"

COMMON_FLAGS=(-std=c++17 -O3 -DNDEBUG -Wall -Wextra)
c++ "${COMMON_FLAGS[@]}" "$BENCH_ROOT/bench/matched_offsets_benchmark.cpp" \
    -I"$PRODUCT_ROOT/include" -L"$LIB_DIR" -llance_c \
    -Wl,-rpath,"$LIB_DIR" -o"$EVIDENCE_DIR/bin/benchmark_new" \
    >"$EVIDENCE_DIR/logs/compile_new.log" 2>&1
c++ "${COMMON_FLAGS[@]}" "$BENCH_ROOT/bench/matched_offsets_benchmark.cpp" \
    -I"$EVIDENCE_DIR/include_old" -L"$LIB_DIR" -llance_c \
    -Wl,-rpath,"$LIB_DIR" -o"$EVIDENCE_DIR/bin/benchmark_old" \
    >"$EVIDENCE_DIR/logs/compile_old.log" 2>&1
sha256sum "$EVIDENCE_DIR/bin/benchmark_new" \
    "$EVIDENCE_DIR/bin/benchmark_old" >"$EVIDENCE_DIR/binary_sha256.txt"
sha256sum "$EVIDENCE_DIR/include_old/lance/lance.h" \
    "$EVIDENCE_DIR/include_old/lance/lance.hpp" \
    >"$EVIDENCE_DIR/old_header_sha256.txt"

DATASET="$EVIDENCE_DIR/dataset"
rm -rf "$DATASET"
"$EVIDENCE_DIR/bin/benchmark_new" create "$DATASET" \
    >"$EVIDENCE_DIR/logs/dataset_create.log" 2>&1

RAW_TMP="$EVIDENCE_DIR/benchmark_raw.csv.tmp"
SUMMARY_TMP="$EVIDENCE_DIR/benchmark_summary.json.tmp"
printf 'workflow,count,sample,nanos,alloc_calls,alloc_bytes,capacity,hash\n' \
    >"$RAW_TMP"
{
    printf 'candidate=%s\n' "$CANDIDATE"
    printf 'baseline=%s\n' "$BASELINE"
    printf 'library=%s\n' "$LIB"
    printf 'new_binary=%s\n' "$EVIDENCE_DIR/bin/benchmark_new"
    printf 'old_binary=%s\n' "$EVIDENCE_DIR/bin/benchmark_old"
    printf 'dataset=%s\n' "$DATASET"
    printf 'sizes=%s\n' "${SIZES[*]}"
} >"$EVIDENCE_DIR/artifact_identity.txt"

for count in "${SIZES[@]}"; do
    for workflow in new old; do
        printf '%s %s %s\n' \
            "$EVIDENCE_DIR/bin/benchmark_$workflow" run "$DATASET" \
            "$count" "$workflow" >>"$EVIDENCE_DIR/commands.txt"
        "$EVIDENCE_DIR/bin/benchmark_$workflow" run "$DATASET" "$count" \
            "$workflow" >>"$RAW_TMP" \
            2>>"$EVIDENCE_DIR/logs/benchmark_${workflow}_${count}.log"
    done
done

python3 "$BENCH_ROOT/bench/summarize_matched_offsets.py" \
    "$RAW_TMP" "$SUMMARY_TMP" >"$EVIDENCE_DIR/logs/summary.log" 2>&1
mv "$RAW_TMP" "$EVIDENCE_DIR/benchmark_raw.csv"
mv "$SUMMARY_TMP" "$EVIDENCE_DIR/benchmark_summary.json"
cat "$EVIDENCE_DIR/benchmark_summary.json"

{
    date -u '+utc_end=%Y-%m-%dT%H:%M:%SZ'
    uptime
} >>"$EVIDENCE_DIR/environment.txt"
printf '0\n' >"$STATUS_TMP"
mv "$STATUS_TMP" "$EVIDENCE_DIR/status/overall"
printf 'benchmark passed\n'
