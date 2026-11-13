#!/usr/bin/env bash
# Rebuild the lance-c shared library from one immutable commit and publish a
# self-describing artifact prefix outside the mbx build cache.
#
# The run is single-shot: it refuses to overwrite an existing prefix, records
# every identity the artifact depends on, stages the payload, verifies a copy of
# the staged payload, and only then publishes by renaming the staged directory
# into place. A failed build keeps its stage directory for inspection and never
# publishes.
#
# Nothing here changes the source checkout: HEAD, index and worktree stay at the
# commit under test, and no build directory override is applied (the existing
# mbx-managed target entry is used as-is).
set -uo pipefail

readonly SRC_SHA=eb59ccb6ec17b6604c06ebaa28e30bfd9914c023
readonly NATIVE_SHA=86fff5f40f299a2ff6abb0c10210451f27e605eb
readonly REPO=/home/xuanwo/Code/lance-c-113-shallow-c2217d8
readonly EXEC_ROOT=/home/xuanwo/executions/lance-c-113-shallow-c2217d8-20261113
readonly PREFIX=$EXEC_ROOT/artifact-eb59ccb6-recovery-20261113
readonly EXPECT_LANCE_H=9091736570e0f85897f4d86f76b3a7463720321880a3f85802e81f4ba53ab564
readonly EXPECT_LANCE_HPP=90f7d1599bb8f8ad7580d20c8a5c7d0430ed4ad37be44b2efe08aba4685dad51
readonly EXPECT_CARGO_LOCK=53dbdf056579f64ae0c1ba54a8b26d07f44788f54a83d728047378f9c5b9aed7
readonly BUILD_CMD='cargo +1.95.0 build --locked --lib -j 8'
readonly REQUIRED_SYMBOL=lance_dataset_open_with_base_store_params
readonly EXTRA_SYMBOL=lance_dataset_shallow_clone

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >&2; }
die() { log "FAIL: $*"; exit 1; }

sha_of() { sha256sum "$1" | cut -d' ' -f1; }

expect_hash() { # file expected label
  local actual
  actual=$(sha_of "$1") || die "cannot hash $1"
  [ "$actual" = "$2" ] || die "$3 hash mismatch: expected $2, got $actual ($1)"
  printf '%s  %s\n' "$actual" "$3" >&2
}

# ---------------------------------------------------------------- preconditions
[ -d "$EXEC_ROOT" ] || die "execution root missing: $EXEC_ROOT"
[ ! -e "$PREFIX" ] || die "refusing to overwrite existing prefix: $PREFIX"
[ -d "$REPO/.git" ] || die "source checkout missing: $REPO"
[ -z "${CARGO_TARGET_DIR:-}" ] || die "CARGO_TARGET_DIR is set; refusing to override build storage"
[ -z "${CARGO_BUILD_BUILD_DIR:-}" ] || die "CARGO_BUILD_BUILD_DIR is set; refusing to override build storage"
command -v cargo >/dev/null || die "cargo not on PATH"
command -v git >/dev/null || die "git not on PATH"

cd "$REPO" || die "cannot enter $REPO"
head_before=$(git rev-parse HEAD) || die "git rev-parse HEAD failed"
tree_before=$(git rev-parse HEAD^{tree}) || die "git rev-parse HEAD^{tree} failed"
[ "$head_before" = "$SRC_SHA" ] || die "HEAD $head_before does not match immutable source $SRC_SHA"
dirty_before=$(git status --porcelain)
[ -z "$dirty_before" ] || die "worktree is dirty before build: $dirty_before"
expect_hash include/lance/lance.h "$EXPECT_LANCE_H" lance.h
expect_hash include/lance/lance.hpp "$EXPECT_LANCE_HPP" lance.hpp
expect_hash Cargo.lock "$EXPECT_CARGO_LOCK" Cargo.lock
native_hits=$(grep -c "$NATIVE_SHA" Cargo.lock)
[ "$native_hits" -gt 0 ] || die "native pin $NATIVE_SHA absent from Cargo.lock"

stage=$EXEC_ROOT/.stage-artifact-eb59ccb6-$$
mkdir -p "$stage/lib" "$stage/include/lance" || die "cannot create stage $stage"
log "stage: $stage"

# ------------------------------------------------------------ source identity
{
  echo "immutable_source $SRC_SHA"
  echo "commit           $head_before"
  echo "tree             $tree_before"
  echo "detached         $( [ -z "$(git branch --show-current)" ] && echo yes || echo no )"
  echo "branch           $(git branch --show-current)"
  echo "shallow_repo     $(git rev-parse --is-shallow-repository)"
  echo "worktree_clean   yes"
  echo "remote_origin    $(git config --get remote.origin.url)"
  echo "native_pin       $NATIVE_SHA"
  echo "native_pin_hits  $native_hits"
  echo
  echo "--- git show -s --format=fuller HEAD ---"
  git show -s --format=fuller HEAD
  echo
  echo "--- git describe --always --dirty ---"
  git describe --always --dirty
  echo
  echo "--- git status --porcelain (empty means clean) ---"
  git status --porcelain
} > "$stage/source-pre.txt" 2>&1 || die "cannot write source-pre.txt"

# ---------------------------------------------------- toolchain and platform
{
  echo "build command    $BUILD_CMD"
  echo "cargo path       $(command -v cargo)"
  echo
  echo "--- cargo +1.95.0 -vV ---"
  cargo +1.95.0 -vV
  echo
  echo "--- rustc +1.95.0 -vV ---"
  rustc +1.95.0 -vV
  echo
  echo "--- uname -a ---"
  uname -a
  echo
  echo "--- os-release ---"
  cat /etc/os-release
  echo
  echo "--- glibc ---"
  ldd --version | head -1
  echo
  echo "--- cpu count ---"
  nproc
  echo
  echo "--- git version ---"
  git --version
  echo
  echo "--- mbx ---"
  mbx --version
  echo
  echo "--- mbx doctor ---"
  mbx doctor
  echo
  echo "--- cargo-related environment (must not carry a target override) ---"
  env | grep -E '^(CARGO|RUST)' | sort || true
} > "$stage/toolchain.txt" 2>&1 || die "cannot write toolchain.txt"

{
  echo "target_entry      $REPO/target"
  echo "target_is_symlink $( [ -L "$REPO/target" ] && echo yes || echo no )"
  echo "target_link_value $(readlink "$REPO/target" 2>/dev/null)"
  echo "target_realpath   $(readlink -f "$REPO/target" 2>/dev/null)"
  echo "target_exists_before_build $( [ -e "$REPO/target" ] && echo yes || echo no )"
} > "$stage/target.txt" 2>&1

# --------------------------------------------------------------------- build
start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
start_epoch=$(date +%s)
log "starting build: $BUILD_CMD"
cargo +1.95.0 build --locked --lib -j 8 > "$stage/build.log" 2>&1
build_status=$?
end_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
end_epoch=$(date +%s)

{
  echo "command     $BUILD_CMD"
  echo "exit        $build_status"
  echo "start_utc   $start_utc"
  echo "end_utc     $end_utc"
  echo "duration_s  $((end_epoch - start_epoch))"
  echo
  echo "--- build.log tail ---"
  tail -n 20 "$stage/build.log"
} > "$stage/build.txt" 2>&1
printf '%s\n' "$build_status" > "$stage/build.exit"
log "build exit: $build_status after $((end_epoch - start_epoch))s"

if [ "$build_status" -ne 0 ]; then
  {
    echo "status    failed"
    echo "exit      $build_status"
    echo "stage     $stage"
    echo "published no"
  } > "$stage/FAILED.txt"
  log "build failed; stage kept at $stage (nothing published)"
  tail -n 40 "$stage/build.log" >&2
  exit 2
fi

# ------------------------------------------------------------ post-build state
head_after=$(git rev-parse HEAD) || die "git rev-parse HEAD failed after build"
tree_after=$(git rev-parse HEAD^{tree}) || die "git rev-parse HEAD^{tree} failed after build"
dirty_after=$(git status --porcelain)
{
  echo "commit          $head_after"
  echo "tree            $tree_after"
  echo "worktree_clean  $( [ -z "$dirty_after" ] && echo yes || echo no )"
  echo "unchanged_head  $( [ "$head_after" = "$head_before" ] && echo yes || echo no )"
  echo "unchanged_tree  $( [ "$tree_after" = "$tree_before" ] && echo yes || echo no )"
  echo
  echo "--- git status --porcelain (empty means clean) ---"
  printf '%s' "$dirty_after"
  echo
  echo
  echo "--- target after build ---"
  echo "target_realpath $(readlink -f "$REPO/target")"
  echo "target_exists   $( [ -e "$REPO/target" ] && echo yes || echo no )"
  ls -l "$REPO/target/debug/liblance_c".* 2>&1
} > "$stage/source-post.txt" 2>&1
[ "$head_after" = "$head_before" ] || die "HEAD changed during build: $head_after"
[ "$tree_after" = "$tree_before" ] || die "tree changed during build: $tree_after"

# ------------------------------------------------------------ artifact payload
so_src=$REPO/target/debug/liblance_c.so
a_src=$REPO/target/debug/liblance_c.a
[ -f "$so_src" ] || die "expected library missing: $so_src"
cp -p "$so_src" "$stage/lib/liblance_c.so" || die "cannot copy liblance_c.so"
if [ -f "$a_src" ]; then
  cp -p "$a_src" "$stage/lib/liblance_c.a" || die "cannot copy liblance_c.a"
  static_note='lib/liblance_c.a copied'
else
  static_note='lib/liblance_c.a not produced by --lib'
fi
cp -p include/lance/lance.h "$stage/include/lance/lance.h" || die "cannot copy lance.h"
cp -p include/lance/lance.hpp "$stage/include/lance/lance.hpp" || die "cannot copy lance.hpp"
cp -p Cargo.lock "$stage/Cargo.lock" || die "cannot copy Cargo.lock"

archive=lance-c-eb59ccb6-source.tar.gz
git archive --format=tar.gz --prefix=lance-c-eb59ccb6/ "$SRC_SHA" > "$stage/$archive" \
  || die "git archive failed"
[ -s "$stage/$archive" ] || die "git archive produced an empty file"

# Copied headers must still be the immutable ones.
expect_hash "$stage/include/lance/lance.h" "$EXPECT_LANCE_H" staged-lance.h
expect_hash "$stage/include/lance/lance.hpp" "$EXPECT_LANCE_HPP" staged-lance.hpp
expect_hash "$stage/Cargo.lock" "$EXPECT_CARGO_LOCK" staged-Cargo.lock

# ------------------------------------------------------------------- receipts
nm -D --defined-only "$stage/lib/liblance_c.so" > "$stage/exports-full.txt" 2>&1 \
  || die "nm failed on liblance_c.so"
grep -E " (${REQUIRED_SYMBOL}|${EXTRA_SYMBOL})$" "$stage/exports-full.txt" > "$stage/exports.txt"
grep -q " ${REQUIRED_SYMBOL}$" "$stage/exports.txt" \
  || die "required symbol $REQUIRED_SYMBOL missing from liblance_c.so"
readelf --dyn-syms --wide "$stage/lib/liblance_c.so" 2>&1 \
  | grep -E " (${REQUIRED_SYMBOL}|${EXTRA_SYMBOL})$" > "$stage/exports-readelf.txt"
[ -s "$stage/exports-readelf.txt" ] || die "required symbols missing from readelf dynamic symbol table"

{
  echo "=== readelf -h ==="
  readelf -h "$stage/lib/liblance_c.so"
  echo
  echo "=== readelf -d ==="
  readelf -d "$stage/lib/liblance_c.so"
  echo
  echo "=== readelf -n ==="
  readelf -n "$stage/lib/liblance_c.so"
  echo
  echo "=== file names and sizes ==="
  ls -l "$stage/lib"
} > "$stage/elf.txt" 2>&1 || die "readelf receipt failed"

expect_hash "$stage/lib/liblance_c.so" "$(sha_of "$so_src")" built-liblance_c.so

# -------------------------------------------------------------------- summary
{
  echo "RECOVERED LIBRARY-ONLY BUILD ARTIFACT (not a CMake install, not a release package)"
  echo
  echo "source_commit      $SRC_SHA"
  echo "source_tree        $tree_before"
  echo "source_clean       yes (before and after build)"
  echo "native_pin         lance/lance-* at $NATIVE_SHA (locked by Cargo.lock)"
  echo "toolchain          rustc/cargo 1.95.0"
  echo "build_command      $BUILD_CMD"
  echo "build_exit         $build_status"
  echo "build_start_utc    $start_utc"
  echo "build_end_utc      $end_utc"
  echo "build_storage      existing mbx-managed target entry ($(readlink "$REPO/target")), no override"
  echo
  echo "payload            lib/liblance_c.so"
  echo "payload_static     $static_note"
  echo "headers            include/lance/lance.h, include/lance/lance.hpp (from $SRC_SHA)"
  echo "lock               Cargo.lock (from $SRC_SHA)"
  echo "source_archive     $archive (git archive of $SRC_SHA, prefix lance-c-eb59ccb6/)"
  echo
  echo "expected_hashes    lance.h $EXPECT_LANCE_H"
  echo "                   lance.hpp $EXPECT_LANCE_HPP"
  echo "                   Cargo.lock $EXPECT_CARGO_LOCK"
  echo
  echo "scope_boundaries"
  echo "  This artifact was built from $SRC_SHA only."
  echo "  Merge 765dd9a2ec135cef9cdf5eb95bd93d73fa3b38e1 was NOT built; it differs from"
  echo "  $SRC_SHA only by an independent 225-line Markdown change, so headers and"
  echo "  production sources are identical, but this artifact must not be reported as"
  echo "  a 765dd9a build."
  echo "  Only 'cargo build --locked --lib' ran: no tests, no C/C++ consumer build, no"
  echo "  CMake install, no release packaging."
} > "$stage/SUMMARY.txt" || die "cannot write SUMMARY.txt"

# Manifest: every regular file except the manifests themselves.
find "$stage" -type f ! -name SIZES.txt ! -name SHA256SUMS \
  -printf '%P\n' | LC_ALL=C sort > "$stage/.filelist"
(
  cd "$stage" || exit 1
  while IFS= read -r f; do stat -c '%s  %n' "$f"; done < .filelist > SIZES.txt
) || die "cannot write SIZES.txt"
(
  cd "$stage" || exit 1
  while IFS= read -r f; do sha256sum "$f"; done < .filelist > SHA256SUMS
) || die "cannot write SHA256SUMS"
rm -f "$stage/.filelist"

# Every published file must be a regular file, never a link.
[ -z "$(find "$stage" -type l)" ] || die "stage contains symbolic links"
while IFS= read -r f; do
  [ "$(stat -c '%F' "$stage/$f")" = "regular file" ] || die "not a regular file: $f"
done < <(sed 's/^[^ ]*  //' "$stage/SHA256SUMS")

# ------------------------------------------------- verify a copy, then publish
verify=$EXEC_ROOT/.verify-artifact-eb59ccb6-$$
mkdir -p "$verify" || die "cannot create verification copy $verify"
cp -a "$stage/." "$verify/" || die "cannot copy stage to verification directory"
( cd "$verify" && sha256sum --strict -c SHA256SUMS > "$verify/verify.log" 2>&1 ) \
  || { tail -n 20 "$verify/verify.log" >&2; die "verification copy failed manifest check"; }
[ "$(wc -l < "$verify/verify.log")" -eq "$(wc -l < "$stage/SHA256SUMS")" ] \
  || die "verification copy checked a different number of files"
rm -rf "$verify" || die "cannot remove verification copy"

mv -T "$stage" "$PREFIX" || die "cannot publish $stage as $PREFIX"
sync
( cd "$PREFIX" && sha256sum --strict -c SHA256SUMS > /dev/null ) \
  || die "published prefix failed manifest check"

log "published: $PREFIX"
echo "PUBLISHED $PREFIX"
