#!/usr/bin/env bash
# Packaging phase for the PR149 bounded C artifact task (2026-11-16).
#
# Phase 1 (rtree149-c3fb417-artifact-build.sh) ran exactly one
# `cargo +1.95.0 build --locked --lib` (exit 0, 7m59s) and stopped at a
# `file(1)` probe on a host where file(1) is not installed. This script never
# invokes cargo again: it packages the artifacts that the recorded build
# produced, using only portable header magic checks.
#
# Task-specific branch only; never part of the product PR.
set -uo pipefail

EXPECT_COMMIT=c3fb417ffc3e33c6575c426d04b6a8e77c178526
EXPECT_TREE=65df5ef281c1af18bb647845964d661c0d80066d
EXPECT_H=99c36cfa1ce40e56f06582b3632c4fa66587998329259227051d33b4d1036f39
EXPECT_HPP=0c0465347fcbb501ac05b843638740c5cd82a7b53d142a85e17810e6e8a40008
EXPECT_LOCK=75535b0a24d82054033a3f38156d2684f7eb4ccf26718c9daca23b3041f97c4e

SOURCE=${SOURCE:-/home/xuanwo/Code/lance-c-validation/rtree149-beta20-c3fb417-source}
PREFIX_PARENT=${PREFIX_PARENT:-/home/xuanwo/Code/lance-c-validation/rtree149-beta20-c3fb417-artifact-20261116}
STAGE="$PREFIX_PARENT/stage-c3fb417"
RUNNER_LOG=${RUNNER_LOG:-/home/xuanwo/Code/lance-c-exec/rtree149-c3fb417-runner/launcher.log}

fail() {
    echo "FAIL: $*" >&2
    exit 90
}

P2LOG="$STAGE/build/phase2.log"
exec > >(tee -a "$P2LOG") 2>&1

echo "phase2_started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
[ -d "$STAGE" ] || fail "stage missing: $STAGE"
[ -f "$STAGE/build/exit_status" ] || fail "phase 1 build record missing"
grep -qx "build_exit=0" "$STAGE/build/exit_status" || fail "phase 1 build did not exit 0"

# Keep the phase 1 runner failure as evidence, once.
if [ -f "$RUNNER_LOG" ] && [ ! -f "$STAGE/build/attempt1-runner.log" ]; then
    cp "$RUNNER_LOG" "$STAGE/build/attempt1-runner.log"
    chmod 0644 "$STAGE/build/attempt1-runner.log"
fi

# ── 1. Source still immutable ───────────────────────────────────────────────
head=$(git -C "$SOURCE" rev-parse HEAD) || fail "cannot read HEAD"
tree=$(git -C "$SOURCE" rev-parse HEAD^{tree})
dirty=$(git -C "$SOURCE" status --porcelain -uall | wc -l)
[ "$head" = "$EXPECT_COMMIT" ] || fail "HEAD $head != $EXPECT_COMMIT"
[ "$tree" = "$EXPECT_TREE" ] || fail "tree $tree != $EXPECT_TREE"
[ "$dirty" = "0" ] || fail "source dirty ($dirty lines)"
[ "$(sha256sum "$SOURCE/include/lance/lance.h" | cut -d' ' -f1)" = "$EXPECT_H" ] || fail "lance.h drift"
[ "$(sha256sum "$SOURCE/include/lance/lance.hpp" | cut -d' ' -f1)" = "$EXPECT_HPP" ] || fail "lance.hpp drift"
[ "$(sha256sum "$SOURCE/Cargo.lock" | cut -d' ' -f1)" = "$EXPECT_LOCK" ] || fail "Cargo.lock drift"

# ── 2. Copy the artifacts the recorded build produced ──────────────────────
TARGET_REAL=$(readlink -f "$SOURCE/target")
SO_SRC="$TARGET_REAL/debug/liblance_c.so"
A_SRC="$TARGET_REAL/debug/liblance_c.a"
[ -f "$SO_SRC" ] || fail "missing $SO_SRC"
[ -f "$A_SRC" ] || fail "missing $A_SRC"
[ "$SO_SRC" -nt "$STAGE/build/session.txt" ] || fail "$SO_SRC predates this run"
[ "$A_SRC" -nt "$STAGE/build/session.txt" ] || fail "$A_SRC predates this run"

cp -L --remove-destination "$SO_SRC" "$STAGE/lib/liblance_c.so" || fail "copy .so failed"
cp -L --remove-destination "$A_SRC" "$STAGE/lib/liblance_c.a" || fail "copy .a failed"
chmod 0755 "$STAGE/lib/liblance_c.so"
chmod 0644 "$STAGE/lib/liblance_c.a"

for f in "$STAGE/lib/liblance_c.so" "$STAGE/lib/liblance_c.a"; do
    [ -L "$f" ] && fail "staged artifact is a symlink: $f"
    [ -f "$f" ] || fail "staged artifact is not a regular file: $f"
    nlink=$(stat -c %h "$f")
    [ "$nlink" = "1" ] || fail "staged artifact has $nlink links: $f"
done

so_magic=$(head -c4 "$STAGE/lib/liblance_c.so" | od -An -tx1 | tr -d ' \n')
a_magic=$(head -c8 "$STAGE/lib/liblance_c.a")
[ "$so_magic" = "7f454c46" ] || fail ".so magic $so_magic is not ELF"
[ "$a_magic" = '!<arch>' ] || fail ".a magic is not ar archive"
{
    echo "so_magic=$so_magic (ELF)"
    echo "so_class=$(readelf -h "$STAGE/lib/liblance_c.so" | sed -n 's/.*Class: *//p')"
    echo "so_type=$(readelf -h "$STAGE/lib/liblance_c.so" | sed -n 's/.*Type: *//p')"
    echo "so_machine=$(readelf -h "$STAGE/lib/liblance_c.so" | sed -n 's/.*Machine: *//p')"
    echo "a_magic=$a_magic (ar)"
    echo "a_members=$(ar t "$STAGE/lib/liblance_c.a" 2>/dev/null | wc -l)"
    echo "file_utility=absent on this host; checks use ELF/ar magic and readelf"
} | tee "$STAGE/build/file-types.txt" >/dev/null

# ── 3. Headers, lock, source archive ───────────────────────────────────────
cp --remove-destination "$SOURCE/include/lance/lance.h" "$STAGE/include/lance/lance.h" || fail "copy lance.h failed"
cp --remove-destination "$SOURCE/include/lance/lance.hpp" "$STAGE/include/lance/lance.hpp" || fail "copy lance.hpp failed"
cp --remove-destination "$SOURCE/Cargo.lock" "$STAGE/Cargo.lock" || fail "copy Cargo.lock failed"
chmod 0644 "$STAGE/include/lance/lance.h" "$STAGE/include/lance/lance.hpp" "$STAGE/Cargo.lock"
[ "$(sha256sum "$STAGE/include/lance/lance.h" | cut -d' ' -f1)" = "$EXPECT_H" ] || fail "staged lance.h drift"
[ "$(sha256sum "$STAGE/include/lance/lance.hpp" | cut -d' ' -f1)" = "$EXPECT_HPP" ] || fail "staged lance.hpp drift"
[ "$(sha256sum "$STAGE/Cargo.lock" | cut -d' ' -f1)" = "$EXPECT_LOCK" ] || fail "staged Cargo.lock drift"

git -C "$SOURCE" archive --format=tar --prefix=lance-c-c3fb417/ "$EXPECT_COMMIT" \
    | gzip -9n >"$STAGE/source-$EXPECT_COMMIT.tar.gz" || fail "git archive failed"

# ── 4. Export receipt ──────────────────────────────────────────────────────
nm -D --defined-only "$STAGE/lib/liblance_c.so" >"$STAGE/build/nm-dynamic.so.txt" || fail "nm -D failed"
nm --defined-only "$STAGE/lib/liblance_c.a" \
    | grep -E ' T lance_(dataset_create_scalar_index|scanner_new|dataset_drop_index|dataset_index_list_json|index_segment_metadata_index_type)$' \
    | head -20 >"$STAGE/build/nm-static.selected.txt"

EXPORTS="$STAGE/EXPORTS.txt"
{
    echo "export_receipt_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "source_commit=$EXPECT_COMMIT"
    echo "header_scalar_rtree=$(grep -oE 'LANCE_SCALAR_RTREE += *9' "$STAGE/include/lance/lance.h" | head -1)"
    echo "exported_lance_symbols=$(grep -cE ' lance_' "$STAGE/build/nm-dynamic.so.txt")"
    echo "exported_rtree_named_symbols=$(grep -icE 'rtree' "$STAGE/build/nm-dynamic.so.txt")"
    echo "note=enum value 9 adds no RTree-specific C symbol; the five entry points below are the pre-existing scalar surface"
    echo
    echo "dynamic (.so, nm -D --defined-only):"
} >"$EXPORTS"

missing=""
for sym in lance_dataset_create_scalar_index lance_scanner_new lance_dataset_drop_index \
    lance_dataset_index_list_json lance_index_segment_metadata_index_type; do
    line=$(grep -E " ${sym}\$" "$STAGE/build/nm-dynamic.so.txt" | head -1)
    if [ -z "$line" ]; then
        missing="$missing dynamic:$sym"
        echo "  MISSING $sym" >>"$EXPORTS"
    else
        echo "  $line" >>"$EXPORTS"
    fi
done
echo "" >>"$EXPORTS"
echo "static (.a, nm --defined-only, T definitions):" >>"$EXPORTS"
for sym in lance_dataset_create_scalar_index lance_scanner_new lance_dataset_drop_index \
    lance_dataset_index_list_json lance_index_segment_metadata_index_type; do
    line=$(grep -E " T ${sym}\$" "$STAGE/build/nm-static.selected.txt" | head -1)
    if [ -z "$line" ]; then
        missing="$missing static:$sym"
        echo "  MISSING $sym" >>"$EXPORTS"
    else
        echo "  $line" >>"$EXPORTS"
    fi
done
echo "missing=$missing" >>"$EXPORTS"
[ -z "$missing" ] || fail "export receipt incomplete:$missing"

# ── 5. Source unchanged ────────────────────────────────────────────────────
head_after=$(git -C "$SOURCE" rev-parse HEAD)
tree_after=$(git -C "$SOURCE" rev-parse HEAD^{tree})
dirty_after=$(git -C "$SOURCE" status --porcelain -uall | wc -l)
{
    echo "head=$head_after"
    echo "tree=$tree_after"
    echo "dirty_lines=$dirty_after"
} | tee "$STAGE/build/source-after.txt" >/dev/null
[ "$head_after" = "$EXPECT_COMMIT" ] || fail "HEAD changed during packaging"
[ "$tree_after" = "$EXPECT_TREE" ] || fail "tree changed during packaging"
[ "$dirty_after" = "0" ] || fail "source dirty after packaging ($dirty_after lines)"

# ── 6. Identity, sizes, checksums ─────────────────────────────────────────
so_size=$(stat -c %s "$STAGE/lib/liblance_c.so")
a_size=$(stat -c %s "$STAGE/lib/liblance_c.a")
so_sha=$(sha256sum "$STAGE/lib/liblance_c.so" | cut -d' ' -f1)
a_sha=$(sha256sum "$STAGE/lib/liblance_c.a" | cut -d' ' -f1)
archive_sha=$(sha256sum "$STAGE/source-$EXPECT_COMMIT.tar.gz" | cut -d' ' -f1)
build_secs=$(sed -n 's/.*Finished `dev` profile \[unoptimized + debuginfo\] target(s) in //p' "$STAGE/build/build.log" | tail -1)

{
    echo "source_commit=$EXPECT_COMMIT"
    echo "source_tree=$EXPECT_TREE"
    echo "pr=https://github.com/lance-format/lance-c/pull/149"
    echo "task=PR149 bounded C artifact recovery; candidate artifact, not an accepted-contract, merged or release claim"
    echo "profile=debug (dev, unoptimized + debuginfo; restores target/debug/liblance_c.{so,a})"
    echo "build=CARGO_BUILD_JOBS=8 CARGO_TERM_COLOR=never RUSTUP_TOOLCHAIN=1.95.0 cargo +1.95.0 build --locked --lib"
    echo "build_invocations=1"
    echo "build_exit=0"
    echo "build_duration=$build_secs"
    echo "build_storage=mbx default target/cache, no overrides; target symlink ->$TARGET_REAL"
    echo "mbx_cache_result=186 hits, 1023 misses, 34 bypassed, 4.8 GiB stored locally"
    echo "rustc=$(rustc +1.95.0 --version)"
    echo "cargo=$(cargo +1.95.0 --version)"
    echo "toolchain=+1.95.0"
    echo "target=x86_64-unknown-linux-gnu"
    echo "glibc=$(ldd --version | head -1 | tr -s ' ')"
    echo "os=$( . /etc/os-release && echo "$PRETTY_NAME")"
    echo "kernel=$(uname -r)"
    echo "cargo_lock_sha256=$EXPECT_LOCK"
    echo "lance_h_sha256=$EXPECT_H"
    echo "lance_hpp_sha256=$EXPECT_HPP"
    echo "source_archive=source-$EXPECT_COMMIT.tar.gz"
    echo "source_archive_sha256=$archive_sha"
    echo "lib_liblance_c_so_sha256=$so_sha"
    echo "lib_liblance_c_so_size=$so_size"
    echo "lib_liblance_c_a_sha256=$a_sha"
    echo "lib_liblance_c_a_size=$a_size"
} >"$STAGE/IDENTITY.txt"

{
    printf '%-22s size=%-12s sha256=%s\n' "lib/liblance_c.so" "$so_size" "$so_sha"
    printf '%-22s size=%-12s sha256=%s\n' "lib/liblance_c.a" "$a_size" "$a_sha"
    printf '%-22s size=%-12s sha256=%s\n' "include/lance/lance.h" "$(stat -c %s "$STAGE/include/lance/lance.h")" "$EXPECT_H"
    printf '%-22s size=%-12s sha256=%s\n' "include/lance/lance.hpp" "$(stat -c %s "$STAGE/include/lance/lance.hpp")" "$EXPECT_HPP"
    printf '%-22s size=%-12s sha256=%s\n' "Cargo.lock" "$(stat -c %s "$STAGE/Cargo.lock")" "$EXPECT_LOCK"
    printf '%-22s size=%-12s sha256=%s\n' "source archive" "$(stat -c %s "$STAGE/source-$EXPECT_COMMIT.tar.gz")" "$archive_sha"
} >"$STAGE/SIZES.txt"

cd "$STAGE" || fail "cannot enter stage"
sha256sum lib/liblance_c.so lib/liblance_c.a \
    include/lance/lance.h include/lance/lance.hpp \
    Cargo.lock "source-$EXPECT_COMMIT.tar.gz" \
    IDENTITY.txt SIZES.txt EXPORTS.txt \
    build/command.txt build/exit_status build/env.txt build/file-types.txt \
    build/source-before.txt build/source-after.txt build/build.log \
    build/attempt1-runner.log build/nm-dynamic.so.txt build/nm-static.selected.txt \
    >SHA256SUMS || fail "sha256sum manifest failed"

{
    echo "stage=$STAGE"
    echo "so_sha256=$so_sha"
    echo "so_size=$so_size"
    echo "a_sha256=$a_sha"
    echo "a_size=$a_size"
    echo "source_archive_sha256=$archive_sha"
    echo "exported_lance_symbols=$(grep -cE ' lance_' "$STAGE/build/nm-dynamic.so.txt")"
    echo "exported_rtree_named_symbols=$(grep -icE 'rtree' "$STAGE/build/nm-dynamic.so.txt")"
    echo "packaged_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "result=STAGED"
} | tee "$STAGE/build/summary.txt"
