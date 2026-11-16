#!/usr/bin/env bash
# Runner for the PR149 bounded C artifact task (2026-11-16).
#
# Purpose: rebuild liblance_c.so and liblance_c.a from the immutable, already
# accepted source checkout at c3fb417ffc3e33c6575c426d04b6a8e77c178526 and stage
# a source-bound candidate prefix outside the mbx cache.
#
# This file lives on a task-specific branch only. It is not product code and
# must never be squashed into the product PR branch.
#
# Invocation (single build per run, no retries):
#   bash rtree149-c3fb417-artifact-build.sh
#
# Environment knobs (defaults match the accepted 2026-11-13 run):
#   SOURCE       immutable source checkout (must be clean at EXPECT_COMMIT)
#   PREFIX_PARENT new directory that receives the staged candidate prefix
#   JOBS         cargo build parallelism (default 8)
set -uo pipefail

EXPECT_COMMIT=c3fb417ffc3e33c6575c426d04b6a8e77c178526
EXPECT_TREE=65df5ef281c1af18bb647845964d661c0d80066d
EXPECT_H=99c36cfa1ce40e56f06582b3632c4fa66587998329259227051d33b4d1036f39
EXPECT_HPP=0c0465347fcbb501ac05b843638740c5cd82a7b53d142a85e17810e6e8a40008
EXPECT_LOCK=75535b0a24d82054033a3f38156d2684f7eb4ccf26718c9daca23b3041f97c4e

SOURCE=${SOURCE:-/home/xuanwo/Code/lance-c-validation/rtree149-beta20-c3fb417-source}
PREFIX_PARENT=${PREFIX_PARENT:-/home/xuanwo/Code/lance-c-validation/rtree149-beta20-c3fb417-artifact-20261116}
JOBS=${JOBS:-8}
STAGE="$PREFIX_PARENT/stage-c3fb417"
TOOLCHAIN=1.95.0

fail() {
    echo "FAIL: $*" >&2
    exit 90
}

# ── Stage directory (outside the mbx cache, same filesystem as the final name) ──
[ -e "$PREFIX_PARENT" ] && fail "prefix parent already exists: $PREFIX_PARENT"
mkdir -p -m 0755 "$STAGE/build" "$STAGE/lib" "$STAGE/include/lance" \
    || fail "cannot create stage layout under $PREFIX_PARENT"
chmod 0755 "$PREFIX_PARENT" "$STAGE" "$STAGE/build" "$STAGE/lib" \
    "$STAGE/include" "$STAGE/include/lance"

LOG="$STAGE/build/build.log"
EX=0
{
    echo "runner=$0"
    echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} | tee "$STAGE/build/session.txt"

# ── 1. Immutable source preflight ────────────────────────────────────────────
head=$(git -C "$SOURCE" rev-parse HEAD 2>/dev/null) || fail "cannot read HEAD of $SOURCE"
tree=$(git -C "$SOURCE" rev-parse HEAD^{tree}) || fail "cannot read tree of $SOURCE"
dirty=$(git -C "$SOURCE" status --porcelain -uall | wc -l)
{
    echo "head=$head"
    echo "tree=$tree"
    echo "dirty_lines=$dirty"
    echo "target_link=$(readlink "$SOURCE/target" 2>/dev/null || echo none)"
    echo "target_link_resolves_to=$(readlink -f "$SOURCE/target" 2>/dev/null || echo none)"
    echo "target_link_exists=$([ -e "$SOURCE/target" ] && echo yes || echo no)"
} | tee "$STAGE/build/source-before.txt"

[ "$head" = "$EXPECT_COMMIT" ] || fail "HEAD $head != $EXPECT_COMMIT"
[ "$tree" = "$EXPECT_TREE" ] || fail "tree $tree != $EXPECT_TREE"
[ "$dirty" = "0" ] || fail "source checkout is not clean ($dirty status lines)"

h_sha=$(sha256sum "$SOURCE/include/lance/lance.h" | cut -d' ' -f1)
hpp_sha=$(sha256sum "$SOURCE/include/lance/lance.hpp" | cut -d' ' -f1)
lock_sha=$(sha256sum "$SOURCE/Cargo.lock" | cut -d' ' -f1)
[ "$h_sha" = "$EXPECT_H" ] || fail "lance.h sha256 $h_sha != $EXPECT_H"
[ "$hpp_sha" = "$EXPECT_HPP" ] || fail "lance.hpp sha256 $hpp_sha != $EXPECT_HPP"
[ "$lock_sha" = "$EXPECT_LOCK" ] || fail "Cargo.lock sha256 $lock_sha != $EXPECT_LOCK"

# ── 2. Toolchain / platform record ──────────────────────────────────────────
cd "$SOURCE" || fail "cannot enter $SOURCE"
{
    echo "recorded_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "hostname=$(hostname)"
    echo "uname=$(uname -srm)"
    echo "os_release=$(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"
    echo "glibc=$(ldd --version 2>/dev/null | head -1)"
    echo "nproc=$(nproc)"
    echo "cargo_shim=$(command -v cargo)"
    echo "cargo_195=$(cargo +$TOOLCHAIN --version 2>&1)"
    echo "rustc_195=$(rustc +$TOOLCHAIN --version 2>&1)"
    echo "cargo_default=$(cargo --version 2>&1)"
    echo "rustc_default=$(rustc --version 2>&1)"
    echo "target_dir_link=$(readlink -f target 2>/dev/null || echo none)"
    echo "mbx_version=$(mbx --version 2>&1 | head -1)"
    echo "jobs=$JOBS"
} | tee "$STAGE/build/env.txt"

# ── 3. One build: dev profile, locked, lib only ─────────────────────────────
BUILD_CMD="CARGO_BUILD_JOBS=$JOBS CARGO_TERM_COLOR=never RUSTUP_TOOLCHAIN=$TOOLCHAIN cargo +$TOOLCHAIN build --locked --lib"
echo "$BUILD_CMD" >"$STAGE/build/command.txt"
echo "build_started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$STAGE/build/started_utc.txt"

CARGO_BUILD_JOBS=$JOBS CARGO_TERM_COLOR=never RUSTUP_TOOLCHAIN=$TOOLCHAIN \
    cargo +$TOOLCHAIN build --locked --lib 2>&1 | tee "$LOG"
EX=${PIPESTATUS[0]}
echo "build_exit=$EX" >"$STAGE/build/exit_status"
echo "build_finished_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$STAGE/build/finished_utc.txt"

TARGET_REAL=$(readlink -f target 2>/dev/null || true)
echo "target_dir_after=$TARGET_REAL" >>"$STAGE/build/env.txt"
if [ "$EX" != "0" ]; then
    echo "FAIL: cargo build exited $EX; raw log kept at $LOG" >&2
    exit "$EX"
fi

# ── 4. Locate the freshly built library pair ────────────────────────────────
SO="$TARGET_REAL/debug/liblance_c.so"
A="$TARGET_REAL/debug/liblance_c.a"
[ -f "$SO" ] || fail "missing $SO"
[ -f "$A" ] || fail "missing $A"

# Copy through any mbx-provided link so the published files are plain regular
# files owned by this prefix, not references into the mutable cache.
cp -L --remove-destination "$SO" "$STAGE/lib/liblance_c.so" || fail "copy .so failed"
cp -L --remove-destination "$A" "$STAGE/lib/liblance_c.a" || fail "copy .a failed"
chmod 0755 "$STAGE/lib/liblance_c.so"
chmod 0644 "$STAGE/lib/liblance_c.a"

for f in "$STAGE/lib/liblance_c.so" "$STAGE/lib/liblance_c.a"; do
    [ -L "$f" ] && fail "staged artifact is a symlink: $f"
    nlink=$(stat -c %h "$f")
    [ "$nlink" = "1" ] || fail "staged artifact has $nlink links (cache alias?): $f"
done

file "$STAGE/lib/liblance_c.so" "$STAGE/lib/liblance_c.a" >"$STAGE/build/file-types.txt"
grep -q "ELF 64-bit" "$STAGE/build/file-types.txt" || fail ".so is not ELF 64-bit"
grep -q "current ar archive" "$STAGE/build/file-types.txt" || fail ".a is not an ar archive"

# ── 5. Headers, lock, source archive ───────────────────────────────────────
cp --remove-destination "$SOURCE/include/lance/lance.h" "$STAGE/include/lance/lance.h" || fail "copy lance.h failed"
cp --remove-destination "$SOURCE/include/lance/lance.hpp" "$STAGE/include/lance/lance.hpp" || fail "copy lance.hpp failed"
cp --remove-destination "$SOURCE/Cargo.lock" "$STAGE/Cargo.lock" || fail "copy Cargo.lock failed"
chmod 0644 "$STAGE/include/lance/lance.h" "$STAGE/include/lance/lance.hpp" "$STAGE/Cargo.lock"

[ "$(sha256sum "$STAGE/include/lance/lance.h" | cut -d' ' -f1)" = "$EXPECT_H" ] || fail "staged lance.h hash drift"
[ "$(sha256sum "$STAGE/include/lance/lance.hpp" | cut -d' ' -f1)" = "$EXPECT_HPP" ] || fail "staged lance.hpp hash drift"
[ "$(sha256sum "$STAGE/Cargo.lock" | cut -d' ' -f1)" = "$EXPECT_LOCK" ] || fail "staged Cargo.lock hash drift"

git -C "$SOURCE" archive --format=tar --prefix=lance-c-c3fb417/ "$EXPECT_COMMIT" \
    | gzip -9n >"$STAGE/source-$EXPECT_COMMIT.tar.gz" || fail "git archive failed"

# ── 6. Export receipt for the existing scalar entry points ──────────────────
nm -D --defined-only "$STAGE/lib/liblance_c.so" >"$STAGE/build/nm-dynamic.so.txt" \
    || fail "nm -D failed"
nm --defined-only "$STAGE/lib/liblance_c.a" >"$STAGE/build/nm-static.a.txt" \
    || fail "nm static failed"

EXPORTS="$STAGE/EXPORTS.txt"
{
    echo "export_receipt_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "source_commit=$EXPECT_COMMIT"
    echo "header_scalar_rtree=$(grep -oE 'LANCE_SCALAR_RTREE *= *9' "$STAGE/include/lance/lance.h" | head -1)"
    echo "exported_lance_symbols=$(grep -cE ' lance_' "$STAGE/build/nm-dynamic.so.txt")"
    echo "exported_rtree_named_symbols=$(grep -icE 'rtree' "$STAGE/build/nm-dynamic.so.txt")"
    echo "note=enum value 9 introduces no RTree-specific C symbol; the five entry points below are the pre-existing scalar surface"
    echo
    echo "dynamic (.so, nm -D --defined-only):"
} >"$EXPORTS"

missing=""
for sym in lance_dataset_create_scalar_index lance_scanner_new lance_dataset_drop_index \
    lance_dataset_index_list_json lance_index_segment_metadata_index_type; do
    line=$(grep -E " ${sym}\$" "$STAGE/build/nm-dynamic.so.txt" | head -1)
    if [ -z "$line" ]; then
        missing="$missing $sym"
        echo "  MISSING $sym" >>"$EXPORTS"
    else
        echo "  $line" >>"$EXPORTS"
    fi
done
{
    echo
    echo "static (.a, nm --defined-only, first definition per symbol):"
} >>"$EXPORTS"
for sym in lance_dataset_create_scalar_index lance_scanner_new lance_dataset_drop_index \
    lance_dataset_index_list_json lance_index_segment_metadata_index_type; do
    line=$(grep -E " T ${sym}\$" "$STAGE/build/nm-static.a.txt" | head -1)
    if [ -z "$line" ]; then
        missing="$missing static:$sym"
        echo "  MISSING $sym" >>"$EXPORTS"
    else
        echo "  $line" >>"$EXPORTS"
    fi
done
echo "missing=$missing" >>"$EXPORTS"
[ -z "$missing" ] || fail "export receipt incomplete:$missing"

# ── 7. Source unchanged after the build ────────────────────────────────────
head_after=$(git -C "$SOURCE" rev-parse HEAD)
tree_after=$(git -C "$SOURCE" rev-parse HEAD^{tree})
dirty_after=$(git -C "$SOURCE" status --porcelain -uall | wc -l)
{
    echo "head=$head_after"
    echo "tree=$tree_after"
    echo "dirty_lines=$dirty_after"
    echo "target_link_exists=$([ -e "$SOURCE/target" ] && echo yes || echo no)"
} | tee "$STAGE/build/source-after.txt"
[ "$head_after" = "$EXPECT_COMMIT" ] || fail "HEAD changed during build"
[ "$tree_after" = "$EXPECT_TREE" ] || fail "tree changed during build"
[ "$dirty_after" = "0" ] || fail "source checkout dirty after build ($dirty_after status lines)"

# ── 8. Identity, sizes, checksums ──────────────────────────────────────────
so_size=$(stat -c %s "$STAGE/lib/liblance_c.so")
a_size=$(stat -c %s "$STAGE/lib/liblance_c.a")
so_sha=$(sha256sum "$STAGE/lib/liblance_c.so" | cut -d' ' -f1)
a_sha=$(sha256sum "$STAGE/lib/liblance_c.a" | cut -d' ' -f1)
archive_sha=$(sha256sum "$STAGE/source-$EXPECT_COMMIT.tar.gz" | cut -d' ' -f1)

{
    echo "source_commit=$EXPECT_COMMIT"
    echo "source_tree=$EXPECT_TREE"
    echo "pr=https://github.com/lance-format/lance-c/pull/149"
    echo "task=PR149 bounded C artifact recovery (candidate, not accepted-contract or merged/release claim)"
    echo "rustc=$(rustc +$TOOLCHAIN --version)"
    echo "cargo=$(cargo +$TOOLCHAIN --version)"
    echo "toolchain=+$TOOLCHAIN"
    echo "target=x86_64-unknown-linux-gnu"
    echo "glibc=$(ldd --version | head -1 | tr -s ' ')"
    echo "os=$( . /etc/os-release && echo "$PRETTY_NAME")"
    echo "kernel=$(uname -r)"
    echo "profile=debug (dev; artifacts restored as target/debug/liblance_c.{so,a})"
    echo "build=$BUILD_CMD"
    echo "build_storage=mbx default target/cache (source target symlink ->$TARGET_REAL)"
    echo "build_exit=$EX"
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
    echo "lib_liblance_c.so size=$so_size sha256=$so_sha"
    echo "lib_liblance_c.a  size=$a_size sha256=$a_sha"
    echo "include_lance.h      size=$(stat -c %s "$STAGE/include/lance/lance.h") sha256=$EXPECT_H"
    echo "include_lance.hpp    size=$(stat -c %s "$STAGE/include/lance/lance.hpp") sha256=$EXPECT_HPP"
    echo "Cargo.lock           size=$(stat -c %s "$STAGE/Cargo.lock") sha256=$EXPECT_LOCK"
    echo "source archive       size=$(stat -c %s "$STAGE/source-$EXPECT_COMMIT.tar.gz") sha256=$archive_sha"
} >"$STAGE/SIZES.txt"

cd "$STAGE" || fail "cannot enter stage"
sha256sum lib/liblance_c.so lib/liblance_c.a \
    include/lance/lance.h include/lance/lance.hpp \
    Cargo.lock "source-$EXPECT_COMMIT.tar.gz" \
    IDENTITY.txt SIZES.txt EXPORTS.txt \
    build/command.txt build/exit_status build/env.txt build/file-types.txt \
    build/source-before.txt build/source-after.txt build/build.log \
    >"SHA256SUMS" || fail "sha256sum manifest failed"

{
    echo "stage=$STAGE"
    echo "so_sha256=$so_sha"
    echo "so_size=$so_size"
    echo "a_sha256=$a_sha"
    echo "a_size=$a_size"
    echo "source_archive_sha256=$archive_sha"
    echo "exported_lance_symbols=$(grep -cE ' lance_' "$STAGE/build/nm-dynamic.so.txt")"
    echo "exported_rtree_named_symbols=$(grep -icE 'rtree' "$STAGE/build/nm-dynamic.so.txt")"
    echo "staged_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "result=STAGED"
} | tee "$STAGE/build/summary.txt"

echo "STAGE_READY=$STAGE"
