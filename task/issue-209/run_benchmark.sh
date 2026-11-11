#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PRODUCT_ROOT=${PRODUCT_ROOT:?set PRODUCT_ROOT to the exact product checkout}
EVIDENCE_DIR=${EVIDENCE_DIR:?set EVIDENCE_DIR outside the target cache}
BASE_COMMIT=930b02c27eb59f939ef4cf1dd227063c3f94460e
EXPECTED_HEAD=9bdca24304cad51bfde8078d874152cb5675fa37
EXPECTED_TREE=7918f21b1562987af6d33769b873e563c9f08871
NATIVE_PIN=40f15ed1b71b66950961b175829fdf17a763f108
SIZES=($((4 * 1024)) $((16 * 1024)) $((32 * 1024)))
SAMPLES=5

for name in CARGO_TARGET_DIR CARGO_HOME RUSTFLAGS LANCE_C_LINK; do
    if printenv "$name" >/dev/null 2>&1; then
        printf 'unexpected environment override: %s\n' "$name" >&2
        exit 2
    fi
done
export RUSTUP_TOOLCHAIN=1.94.0
export CARGO_BUILD_JOBS=8

mkdir -p "$EVIDENCE_DIR/bin" "$EVIDENCE_DIR/include_old/lance" \
    "$EVIDENCE_DIR/logs" "$EVIDENCE_DIR/source" "$EVIDENCE_DIR/status"
STATUS_TMP="$EVIDENCE_DIR/status/overall.tmp"
printf '1\n' >"$STATUS_TMP"

{
    printf 'product_root=%s\n' "$PRODUCT_ROOT"
    printf 'benchmark_source_dir=%s\n' "$SCRIPT_DIR"
    printf 'evidence_dir=%s\n' "$EVIDENCE_DIR"
    printf 'base_commit=%s\n' "$BASE_COMMIT"
    printf 'expected_head=%s\n' "$EXPECTED_HEAD"
    printf 'expected_tree=%s\n' "$EXPECTED_TREE"
    printf 'native_pin=%s\n' "$NATIVE_PIN"
    printf 'profile=release\n'
    printf 'cpp_flags=-std=c++17 -O3 -DNDEBUG -Wall -Wextra\n'
    printf 'sizes=%s\n' "${SIZES[*]}"
    printf 'samples_per_workflow=%s\n' "$SAMPLES"
    printf 'rustup_toolchain=%s\n' "$RUSTUP_TOOLCHAIN"
    printf 'cargo_build_jobs=%s\n' "$CARGO_BUILD_JOBS"
    printf 'path=%s\n' "$PATH"
    date -u '+utc_start=%Y-%m-%dT%H:%M:%SZ'
    git -C "$PRODUCT_ROOT" status --short --branch
    git -C "$PRODUCT_ROOT" rev-parse HEAD
    git -C "$PRODUCT_ROOT" rev-parse 'HEAD^{tree}'
    git -C "$PRODUCT_ROOT" log -1 --format='%H%n%s'
    git -C "$PRODUCT_ROOT" diff --stat "$BASE_COMMIT..$EXPECTED_HEAD"
    git -C "$PRODUCT_ROOT" diff --numstat "$BASE_COMMIT..$EXPECTED_HEAD" -- \
        src/transaction.rs
    git -C "$PRODUCT_ROOT" diff --name-only "$BASE_COMMIT..$EXPECTED_HEAD" -- \
        Cargo.toml Cargo.lock
    command -v cargo
    cargo --version
    rustc --version
    c++ --version
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

extract_offset_at() {
    awk '
        /pub unsafe extern "C" fn lance_updated_fragment_offsets_offset_at\(/ {
            printing = 1
        }
        printing { print }
        printing && /^}/ { exit }
    '
}

git -C "$PRODUCT_ROOT" show "$BASE_COMMIT:src/transaction.rs" |
    extract_offset_at >"$EVIDENCE_DIR/offset_at_old.rs"
extract_offset_at <"$PRODUCT_ROOT/src/transaction.rs" \
    >"$EVIDENCE_DIR/offset_at_candidate.rs"
cmp "$EVIDENCE_DIR/offset_at_old.rs" "$EVIDENCE_DIR/offset_at_candidate.rs"
sha256sum "$EVIDENCE_DIR/offset_at_old.rs" \
    "$EVIDENCE_DIR/offset_at_candidate.rs" >"$EVIDENCE_DIR/offset_at_sha256.txt"

git -C "$PRODUCT_ROOT" show "$BASE_COMMIT:include/lance/lance.h" \
    >"$EVIDENCE_DIR/include_old/lance/lance.h"
git -C "$PRODUCT_ROOT" show "$BASE_COMMIT:include/lance/lance.hpp" \
    >"$EVIDENCE_DIR/include_old/lance/lance.hpp"
cp "$SCRIPT_DIR/benchmark_updated_offsets.cpp" \
    "$SCRIPT_DIR/run_benchmark.sh" "$SCRIPT_DIR/summarize_benchmark.py" \
    "$EVIDENCE_DIR/source/"
sha256sum "$EVIDENCE_DIR/source/benchmark_updated_offsets.cpp" \
    "$EVIDENCE_DIR/source/run_benchmark.sh" \
    "$EVIDENCE_DIR/source/summarize_benchmark.py" \
    >"$EVIDENCE_DIR/source/sha256.txt"

cd "$PRODUCT_ROOT"
cargo build --release --locked --lib -j 8 \
    >"$EVIDENCE_DIR/logs/release_build.log" 2>&1
TARGET_DIR=$(cargo metadata --format-version 1 --no-deps |
    jq -r '.target_directory')
LIB_DIR="$TARGET_DIR/release"
LIB="$LIB_DIR/liblance_c.so"
[[ -f $LIB ]]
sha256sum "$LIB" >"$EVIDENCE_DIR/library_sha256.txt"
nm -D "$LIB" | grep -E \
    'lance_updated_fragment_offsets_(copy_at|offset_at|offset_count_at)$' \
    >"$EVIDENCE_DIR/library_symbols.txt"

COMMON_FLAGS=(-std=c++17 -O3 -DNDEBUG -Wall -Wextra)
c++ "${COMMON_FLAGS[@]}" "$SCRIPT_DIR/benchmark_updated_offsets.cpp" \
    -I"$PRODUCT_ROOT/include" -L"$LIB_DIR" -llance_c \
    -Wl,-rpath,"$LIB_DIR" -o"$EVIDENCE_DIR/bin/benchmark_new" \
    >"$EVIDENCE_DIR/logs/compile_new.log" 2>&1
c++ "${COMMON_FLAGS[@]}" "$SCRIPT_DIR/benchmark_updated_offsets.cpp" \
    -I"$EVIDENCE_DIR/include_old" -L"$LIB_DIR" -llance_c \
    -Wl,-rpath,"$LIB_DIR" -o"$EVIDENCE_DIR/bin/benchmark_old" \
    >"$EVIDENCE_DIR/logs/compile_old.log" 2>&1
sha256sum "$EVIDENCE_DIR/bin/benchmark_new" \
    "$EVIDENCE_DIR/bin/benchmark_old" >"$EVIDENCE_DIR/binary_sha256.txt"
sha256sum "$EVIDENCE_DIR/include_old/lance/lance.h" \
    "$EVIDENCE_DIR/include_old/lance/lance.hpp" \
    "$PRODUCT_ROOT/include/lance/lance.h" \
    "$PRODUCT_ROOT/include/lance/lance.hpp" \
    >"$EVIDENCE_DIR/header_sha256.txt"
ldd "$EVIDENCE_DIR/bin/benchmark_new" >"$EVIDENCE_DIR/ldd_new.txt"
ldd "$EVIDENCE_DIR/bin/benchmark_old" >"$EVIDENCE_DIR/ldd_old.txt"

RAW_TMP="$EVIDENCE_DIR/benchmark_raw.csv.tmp"
SUMMARY_TMP="$EVIDENCE_DIR/benchmark_summary.json.tmp"
printf 'workflow,count,sample,nanos,alloc_calls,alloc_bytes,capacity,hash\n' \
    >"$RAW_TMP"
{
    printf 'product_head=%s\n' "$EXPECTED_HEAD"
    printf 'product_tree=%s\n' "$EXPECTED_TREE"
    printf 'base_commit=%s\n' "$BASE_COMMIT"
    printf 'library=%s\n' "$LIB"
    printf 'new_binary=%s\n' "$EVIDENCE_DIR/bin/benchmark_new"
    printf 'old_binary=%s\n' "$EVIDENCE_DIR/bin/benchmark_old"
    printf 'new_header=%s\n' "$PRODUCT_ROOT/include/lance/lance.hpp"
    printf 'old_header=%s\n' "$EVIDENCE_DIR/include_old/lance/lance.hpp"
    printf 'same_candidate_library=true\n'
    printf 'old_getter_source_identical=true\n'
} >"$EVIDENCE_DIR/artifact_identity.txt"

for sample in $(seq 1 "$SAMPLES"); do
    for count in "${SIZES[@]}"; do
        if ((sample % 2 == 1)); then
            workflows=(old new)
        else
            workflows=(new old)
        fi
        for workflow in "${workflows[@]}"; do
            binary="$EVIDENCE_DIR/bin/benchmark_$workflow"
            log="$EVIDENCE_DIR/logs/${workflow}_${count}_${sample}.log"
            printf '%q %q %q %q\n' "$binary" "$workflow" "$count" "$sample" \
                >>"$EVIDENCE_DIR/commands.txt"
            "$binary" "$workflow" "$count" "$sample" >>"$RAW_TMP" 2>"$log"
        done
    done
done

python3 "$SCRIPT_DIR/summarize_benchmark.py" "$RAW_TMP" "$SUMMARY_TMP" \
    >"$EVIDENCE_DIR/logs/summary.log" 2>&1
mv "$RAW_TMP" "$EVIDENCE_DIR/benchmark_raw.csv"
mv "$SUMMARY_TMP" "$EVIDENCE_DIR/benchmark_summary.json"
cat "$EVIDENCE_DIR/benchmark_summary.json"

{
    git status --short --branch
    git rev-parse HEAD
    git rev-parse 'HEAD^{tree}'
    date -u '+utc_end=%Y-%m-%dT%H:%M:%SZ'
    uptime
} >>"$EVIDENCE_DIR/environment.txt" 2>&1
[[ -z $(git status --porcelain) ]]

printf '0\n' >"$STATUS_TMP"
mv "$STATUS_TMP" "$EVIDENCE_DIR/status/overall"
printf 'benchmark passed\n'
