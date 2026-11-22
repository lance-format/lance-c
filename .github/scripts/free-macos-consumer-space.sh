#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Reclaim large test and build executables before the installed-package
# consumer checks. verify-cmake-consumer.sh only reads the Cargo-built
# liblance_c.{a,dylib,so} files; dependency rlibs, build scripts, and those
# shipped libraries stay in place so the Cargo cache remains useful.

set -euo pipefail

target_dir=${1:-target/debug}
if [ ! -d "$target_dir" ]; then
  echo "target directory is missing: $target_dir" >&2
  exit 1
fi

echo "==> Disk usage before cleanup"
df -h .
du -sh "$target_dir" 2>/dev/null || true

candidates=$(mktemp "${TMPDIR:-/tmp}/lance-c-space.XXXXXX")
trap 'rm -f "$candidates"' EXIT

# Test executables are the large, no-extension files left after cargo test.
# Exclude archives, shared libraries, and rlib/rmeta metadata so the reusable
# dependency cache survives for later jobs.
find "$target_dir" -type f -size +10M -perm -0100 \
  ! -name 'liblance_c.a' \
  ! -name 'liblance_c.dylib' \
  ! -name 'liblance_c.so' \
  ! -name '*.a' \
  ! -name '*.dylib' \
  ! -name '*.so' \
  ! -name '*.rlib' \
  ! -name '*.rmeta' \
  ! -name 'build-script-build' \
  -print0 >"$candidates"

if [ -s "$candidates" ]; then
  echo "==> Reclaiming large executable test/build intermediates"
  xargs -0 du -ch <"$candidates" | tail -n 1
  while IFS= read -r -d '' path; do
    echo "  $path"
  done <"$candidates"
  xargs -0 rm -f <"$candidates"
else
  echo "==> No large executable test/build intermediates found"
fi

echo "==> Disk usage after cleanup"
df -h .
du -sh "$target_dir" 2>/dev/null || true
for library in "$target_dir"/liblance_c.a "$target_dir"/liblance_c.dylib "$target_dir"/liblance_c.so; do
  if [ -e "$library" ]; then
    ls -lh "$library"
  fi
done
