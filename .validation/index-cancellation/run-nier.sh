#!/usr/bin/env bash
# Apply the native cancellation candidate to a clean Lance 14.0.2 checkout and
# run only the focused native acceptance test against it.
#
# Required environment:
#   LANCE_SRC     clean checkout at 40f15ed1b71b66950961b175829fdf17a763f108
#   EVIDENCE_DIR  directory for logs, patch readback, and the atomic exit file
#
# The script never runs the six-case progress baseline probe, a full suite, or
# any lance-c/C/C++ build.
set -euo pipefail

ARTIFACT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE=40f15ed1b71b66950961b175829fdf17a763f108
PATCH="$ARTIFACT_DIR/native-cancellation.patch"
TEST="$ARTIFACT_DIR/e4_native_cancellation.rs"

: "${LANCE_SRC:?set LANCE_SRC to a clean Lance 14.0.2 checkout}"
: "${EVIDENCE_DIR:?set EVIDENCE_DIR to a task-owned evidence directory}"

mkdir -p "$EVIDENCE_DIR"
exec > >(tee "$EVIDENCE_DIR/run.log") 2>&1
trap 'rc=$?; printf "%s\n" "$rc" > "$EVIDENCE_DIR/exit.txt.tmp"; mv "$EVIDENCE_DIR/exit.txt.tmp" "$EVIDENCE_DIR/exit.txt"' EXIT

export RUSTUP_TOOLCHAIN=1.94.0
set -x

date -u +started_utc=%Y-%m-%dT%H:%M:%SZ
hostname
cargo +1.94.0 -V
rustc +1.94.0 -V

actual_head="$(git -C "$LANCE_SRC" rev-parse HEAD)"
test "$actual_head" = "$BASE"
test -z "$(git -C "$LANCE_SRC" status --porcelain -uall)"
sha256sum "$PATCH" "$TEST" | tee "$EVIDENCE_DIR/artifact-sha256.txt"

# Bind the patch to the exact checkout and prove the applied tree matches it.
patch_id="$(git patch-id --stable < "$PATCH" | awk '{print $1}')"
git -C "$LANCE_SRC" apply --check "$PATCH"
git -C "$LANCE_SRC" apply "$PATCH"
git -C "$LANCE_SRC" diff --binary > "$EVIDENCE_DIR/applied.patch"
applied_patch_id="$(git patch-id --stable < "$EVIDENCE_DIR/applied.patch" | awk '{print $1}')"
test "$patch_id" = "$applied_patch_id"
git -C "$LANCE_SRC" apply --reverse --check "$PATCH"
git -C "$LANCE_SRC" diff --name-only | sort | tee "$EVIDENCE_DIR/changed-production-files.txt"
test "$(git -C "$LANCE_SRC" diff --name-only | wc -l)" -eq 6

cp "$TEST" "$LANCE_SRC/rust/lance/tests/e4_native_cancellation.rs"
git -C "$LANCE_SRC" status --porcelain -uall | sort | tee "$EVIDENCE_DIR/status.txt"

# Check only the files this candidate changes, not the whole workspace.
rustfmt +1.94.0 --edition 2024 --check \
  "$LANCE_SRC/rust/lance-index/src/progress.rs" \
  "$LANCE_SRC/rust/lance-index/src/scalar/btree.rs" \
  "$LANCE_SRC/rust/lance/src/index/create.rs" \
  "$LANCE_SRC/rust/lance/src/index/vector/builder.rs" \
  "$LANCE_SRC/rust/lance/tests/e4_native_cancellation.rs"

# Focused compilation and execution: this target only.
cd "$LANCE_SRC"
cargo +1.94.0 test --locked -p lance --test e4_native_cancellation -j 8 -- "$@" --test-threads=4

# A locked run must leave the native source and fixture exactly as supplied.
git diff --binary > "$EVIDENCE_DIR/after-test.patch"
cmp "$EVIDENCE_DIR/applied.patch" "$EVIDENCE_DIR/after-test.patch"
cmp "$TEST" "$LANCE_SRC/rust/lance/tests/e4_native_cancellation.rs"

date -u +finished_utc=%Y-%m-%dT%H:%M:%SZ
