#!/usr/bin/env bash
# Rebuild the lance-c shared library from one immutable commit and publish a
# self-describing artifact prefix outside the mbx build cache.
#
# Two phases keep the single build and the publication separate:
#
#   immutable_artifact_recovery.sh build
#       Verify the source checkout, run the one build, copy the payload, write
#       the receipts and summary into a stage directory, and stop. The stage
#       path is printed and nothing is published.
#
#   immutable_artifact_recovery.sh finalize <stage> <prefix> [note ...]
#       Re-verify the stage contents, generate sizes and checksums, verify a
#       copy of the staged payload, and publish the stage by renaming it onto
#       the prefix. A failed build is never published, and an existing prefix is
#       never overwritten.
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
readonly ARCHIVE=lance-c-eb59ccb6-source.tar.gz

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >&2; }
die() { log "FAIL: $*"; exit 1; }

sha_of() { sha256sum "$1" | cut -d' ' -f1; }

expect_hash() { # file expected label
  local actual
  actual=$(sha_of "$1") || die "cannot hash $1"
  [ "$actual" = "$2" ] || die "$3 hash mismatch: expected $2, got $actual ($1)"
  printf '%s  %s\n' "$actual" "$3" >&2
}

usage() {
  cat >&2 <<'EOF'
usage: immutable_artifact_recovery.sh build
       immutable_artifact_recovery.sh finalize <stage> <prefix> [note ...]
EOF
  exit 64
}

# ------------------------------------------------------------------- build mode
cmd_build() {
  [ -d "$EXEC_ROOT" ] || die "execution root missing: $EXEC_ROOT"
  [ ! -e "$PREFIX" ] || die "refusing to overwrite existing prefix: $PREFIX"
  [ -d "$REPO/.git" ] || die "source checkout missing: $REPO"
  [ -z "${CARGO_TARGET_DIR:-}" ] || die "CARGO_TARGET_DIR is set; refusing to override build storage"
  [ -z "${CARGO_BUILD_BUILD_DIR:-}" ] || die "CARGO_BUILD_BUILD_DIR is set; refusing to override build storage"
  command -v cargo >/dev/null || die "cargo not on PATH"
  command -v git >/dev/null || die "git not on PATH"

  cd "$REPO" || die "cannot enter $REPO"
  local head_before tree_before dirty_before native_hits stage
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

  local start_utc end_utc start_epoch end_epoch build_status head_after tree_after dirty_after
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

  local so_src=$REPO/target/debug/liblance_c.so
  local a_src=$REPO/target/debug/liblance_c.a static_note
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

  git archive --format=tar.gz --prefix=lance-c-eb59ccb6/ "$SRC_SHA" > "$stage/$ARCHIVE" \
    || die "git archive failed"
  [ -s "$stage/$ARCHIVE" ] || die "git archive produced an empty file"

  expect_hash "$stage/include/lance/lance.h" "$EXPECT_LANCE_H" staged-lance.h
  expect_hash "$stage/include/lance/lance.hpp" "$EXPECT_LANCE_HPP" staged-lance.hpp
  expect_hash "$stage/Cargo.lock" "$EXPECT_CARGO_LOCK" staged-Cargo.lock

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
    echo "source_archive     $ARCHIVE (git archive of $SRC_SHA, prefix lance-c-eb59ccb6/)"
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

  echo "STAGE $stage"
  echo "BUILD exit=$build_status duration=$((end_epoch - start_epoch))s"
  echo "NEXT $0 finalize $stage $PREFIX"
}

# ---------------------------------------------------------------- finalize mode
cmd_finalize() {
  local stage=${1:-} prefix=${2:-}
  shift 2 2>/dev/null || true
  [ -n "$stage" ] && [ -n "$prefix" ] || usage
  [ -d "$stage" ] || die "stage directory missing: $stage"
  case "$prefix" in /*) ;; *) die "prefix must be an absolute path: $prefix" ;; esac
  [ ! -e "$prefix" ] || die "refusing to overwrite existing prefix: $prefix"
  [ "$(cat "$stage/build.exit" 2>/dev/null)" = "0" ] || die "stage has no successful build record: $stage/build.exit"
  [ -s "$stage/build.log" ] || die "stage has no build log: $stage/build.log"

  local f
  for f in lib/liblance_c.so include/lance/lance.h include/lance/lance.hpp Cargo.lock \
           source-pre.txt source-post.txt build.txt toolchain.txt elf.txt exports.txt \
           SUMMARY.txt "$ARCHIVE"; do
    [ -f "$stage/$f" ] || die "stage is missing $f"
  done
  expect_hash "$stage/include/lance/lance.h" "$EXPECT_LANCE_H" staged-lance.h
  expect_hash "$stage/include/lance/lance.hpp" "$EXPECT_LANCE_HPP" staged-lance.hpp
  expect_hash "$stage/Cargo.lock" "$EXPECT_CARGO_LOCK" staged-Cargo.lock
  grep -q " ${REQUIRED_SYMBOL}$" "$stage/exports.txt" \
    || die "stage export receipt lacks $REQUIRED_SYMBOL"
  grep -q '^unchanged_head  yes$' "$stage/source-post.txt" || die "stage records a changed HEAD"
  grep -q '^unchanged_tree  yes$' "$stage/source-post.txt" || die "stage records a changed tree"

  # No link may enter the published payload: every file is a real copy.
  [ -z "$(find "$stage" -type l)" ] || die "stage contains symbolic links"

  {
    echo "PACKAGING"
    echo "packaged_utc      $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "finalize_script   $0 sha256 $(sha_of "$0")"
    echo "stage             $stage"
    echo "phase_note        This phase did not build anything: the payload is the one"
    echo "                  produced by the build phase recorded in build.txt."
    local note
    for note in "$@"; do
      printf 'note              %s\n' "$note"
    done
  } > "$stage/PACKAGING.txt" || die "cannot write PACKAGING.txt"

  # Manifest every regular file except the manifests themselves. The file list
  # lives outside the stage so it cannot end up inside its own manifest.
  local filelist
  filelist=$(mktemp) || die "cannot create a temporary file list"
  find "$stage" -type f ! -name SIZES.txt ! -name SHA256SUMS -printf '%P\n' \
    | LC_ALL=C sort > "$filelist" || die "cannot list stage files"
  [ -s "$filelist" ] || die "stage file list is empty"
  (
    cd "$stage" || exit 1
    while IFS= read -r f; do stat -c '%s  %n' "$f"; done < "$filelist" > SIZES.txt
  ) || die "cannot write SIZES.txt"
  (
    cd "$stage" || exit 1
    while IFS= read -r f; do sha256sum "$f"; done < "$filelist" > SHA256SUMS
  ) || die "cannot write SHA256SUMS"

  while IFS= read -r f; do
    [ "$(stat -c '%F' "$stage/$f")" = "regular file" ] || die "not a regular file: $f"
    [ "$(stat -c '%h' "$stage/$f")" = "1" ] || die "unexpected hard link count for $f"
  done < "$filelist"
  rm -f "$filelist"

  # Verify a copy of the staged payload, then publish the verified stage.
  local verify=$EXEC_ROOT/.verify-artifact-eb59ccb6-$$
  mkdir -p "$verify" || die "cannot create verification copy $verify"
  cp -a "$stage/." "$verify/" || die "cannot copy stage to verification directory"
  ( cd "$verify" && sha256sum --strict -c SHA256SUMS > "$verify/verify.log" 2>&1 ) \
    || { tail -n 20 "$verify/verify.log" >&2; die "verification copy failed manifest check"; }
  [ "$(wc -l < "$verify/verify.log")" -eq "$(wc -l < "$stage/SHA256SUMS")" ] \
    || die "verification copy checked a different number of files"
  rm -rf "$verify" || die "cannot remove verification copy"

  mv -T "$stage" "$prefix" || die "cannot publish $stage as $prefix"
  sync
  ( cd "$prefix" && sha256sum --strict -c SHA256SUMS > /dev/null ) \
    || die "published prefix failed manifest check"

  log "published: $prefix"
  echo "PUBLISHED $prefix"
}

case "${1:-}" in
  build)    shift; [ $# -eq 0 ] || usage; cmd_build ;;
  finalize) shift; cmd_finalize "$@" ;;
  *)        usage ;;
esac
