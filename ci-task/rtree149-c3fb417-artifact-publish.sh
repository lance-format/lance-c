#!/usr/bin/env bash
# Publish phase for the PR149 bounded C artifact task (2026-11-16).
#
# Records runner provenance, regenerates the checksum manifest, verifies every
# staged file, then publishes the candidate prefix with a single same-filesystem
# rename. Never invokes cargo. Task-specific branch only.
set -uo pipefail

EXPECT_COMMIT=c3fb417ffc3e33c6575c426d04b6a8e77c178526
EXPECT_TREE=65df5ef281c1af18bb647845964d661c0d80066d
EXPECT_H=99c36cfa1ce40e56f06582b3632c4fa66587998329259227051d33b4d1036f39
EXPECT_HPP=0c0465347fcbb501ac05b843638740c5cd82a7b53d142a85e17810e6e8a40008
EXPECT_LOCK=75535b0a24d82054033a3f38156d2684f7eb4ccf26718c9daca23b3041f97c4e

SOURCE=${SOURCE:-/home/xuanwo/Code/lance-c-validation/rtree149-beta20-c3fb417-source}
PREFIX_PARENT=${PREFIX_PARENT:-/home/xuanwo/Code/lance-c-validation/rtree149-beta20-c3fb417-artifact-20261116}
STAGE="$PREFIX_PARENT/stage-c3fb417"
FINAL="$PREFIX_PARENT/candidate-c3fb417"
RUNNER_DIR=${RUNNER_DIR:-/home/xuanwo/Code/lance-c-exec/rtree149-c3fb417-runner}
RUNNER_COMMIT=cd3bbc3d9ed72685d5a93989a4d9d5b95431349e

fail() {
    echo "FAIL: $*" >&2
    exit 90
}

[ -d "$STAGE" ] || fail "stage missing: $STAGE"
[ -e "$FINAL" ] && fail "final prefix already exists: $FINAL"
grep -qx "build_exit=0" "$STAGE/build/exit_status" || fail "build record is not exit 0"

head=$(git -C "$SOURCE" rev-parse HEAD)
tree=$(git -C "$SOURCE" rev-parse HEAD^{tree})
dirty=$(git -C "$SOURCE" status --porcelain -uall | wc -l)
[ "$head" = "$EXPECT_COMMIT" ] || fail "HEAD $head != $EXPECT_COMMIT"
[ "$tree" = "$EXPECT_TREE" ] || fail "tree $tree != $EXPECT_TREE"
[ "$dirty" = "0" ] || fail "source dirty ($dirty lines)"

# ── 1. Runner provenance ───────────────────────────────────────────────────
build_script_sha=$(sha256sum "$RUNNER_DIR/ci-task/rtree149-c3fb417-artifact-build.sh" | cut -d' ' -f1)
package_script_sha=$(sha256sum "$RUNNER_DIR/ci-task/rtree149-c3fb417-artifact-package.sh" | cut -d' ' -f1)
publish_script_sha=$(sha256sum "$RUNNER_DIR/ci-task/rtree149-c3fb417-artifact-publish.sh" | cut -d' ' -f1)
{
    echo "runner_repo=https://github.com/lance-format/lance-c"
    echo "runner_branch=xuanwo/task/rtree149-c3fb417-artifact"
    echo "runner_commit=$RUNNER_COMMIT"
    echo "note=task-only branch; these scripts are not part of the product PR"
    echo "build_script=ci-task/rtree149-c3fb417-artifact-build.sh sha256=$build_script_sha"
    echo "package_script=ci-task/rtree149-c3fb417-artifact-package.sh sha256=$package_script_sha"
    echo "publish_script=ci-task/rtree149-c3fb417-artifact-publish.sh sha256=$publish_script_sha"
    echo "build_invocations=1 (cargo +1.95.0 build --locked --lib)"
    echo "packaging_note=phase 1 stopped after the successful build on a missing file(1) probe; phase 2 and this phase never re-ran cargo"
} >"$STAGE/RUNNER.txt"
chmod 0644 "$STAGE/RUNNER.txt"

# ── 2. Files that the manifest covers ─────────────────────────────────────
cd "$STAGE" || fail "cannot enter stage"
FILES=(
    lib/liblance_c.so lib/liblance_c.a
    include/lance/lance.h include/lance/lance.hpp
    Cargo.lock "source-$EXPECT_COMMIT.tar.gz"
    IDENTITY.txt SIZES.txt EXPORTS.txt RUNNER.txt
    build/command.txt build/exit_status build/env.txt build/file-types.txt
    build/source-before.txt build/source-after.txt build/build.log
    build/attempt1-runner.log build/nm-dynamic.so.txt build/nm-static.selected.txt
    build/session.txt build/started_utc.txt build/finished_utc.txt build/summary.txt
)
for f in "${FILES[@]}"; do
    [ -f "$f" ] || fail "manifest input missing: $f"
done
sha256sum "${FILES[@]}" >SHA256SUMS || fail "manifest write failed"
chmod 0644 SHA256SUMS

# ── 3. Verify staged content ──────────────────────────────────────────────
verify_out=$(sha256sum -c SHA256SUMS 2>&1)
verify_ex=$?
echo "$verify_out" >"build/verify-publish.log"
[ "$verify_ex" = "0" ] || fail "sha256sum -c failed on stage"
! printf '%s\n' "$verify_out" | grep -q "FAILED" || fail "sha256sum -c reported FAILED entries"
[ "$(printf '%s\n' "$verify_out" | grep -c ': OK$')" = "${#FILES[@]}" ] \
    || fail "verified entry count mismatch"

for f in lib/liblance_c.so lib/liblance_c.a; do
    [ -L "$f" ] && fail "symlink in payload: $f"
    [ -f "$f" ] || fail "not a regular file: $f"
    [ "$(stat -c %h "$f")" = "1" ] || fail "unexpected link count on $f"
done
[ "$(head -c4 lib/liblance_c.so | od -An -tx1 | tr -d ' \n')" = "7f454c46" ] || fail ".so lost ELF magic"
[ "$(head -c8 lib/liblance_c.a)" = '!<arch>' ] || fail ".a lost ar magic"
[ "$(sha256sum include/lance/lance.h | cut -d' ' -f1)" = "$EXPECT_H" ] || fail "lance.h hash drift"
[ "$(sha256sum include/lance/lance.hpp | cut -d' ' -f1)" = "$EXPECT_HPP" ] || fail "lance.hpp hash drift"
[ "$(sha256sum Cargo.lock | cut -d' ' -f1)" = "$EXPECT_LOCK" ] || fail "Cargo.lock hash drift"

# Manifest covers everything above; the verify log is added afterwards so it
# stays outside the manifest (it is a publication-time record).
rm -f build/verify-publish.log

# ── 4. Atomic publish (same filesystem rename) ─────────────────────────────────
cd "$PREFIX_PARENT" || fail "cannot enter prefix parent"
mv "$STAGE" "$FINAL" || fail "atomic rename failed"
[ -d "$FINAL" ] || fail "final prefix missing after rename"
[ -e "$STAGE" ] && fail "stage still present after rename"

# Read-only payload: the prefix is a retained candidate, not a work directory.
chmod -R a-w "$FINAL/lib" "$FINAL/include" || fail "cannot set read-only payload"
chmod 0444 "$FINAL/IDENTITY.txt" "$FINAL/SIZES.txt" "$FINAL/EXPORTS.txt" \
    "$FINAL/RUNNER.txt" "$FINAL/SHA256SUMS" "$FINAL/Cargo.lock" \
    "$FINAL/source-$EXPECT_COMMIT.tar.gz" || fail "cannot set read-only metadata"

cd "$FINAL" || fail "cannot enter final prefix"
final_verify=$(sha256sum -c SHA256SUMS 2>&1)
final_ex=$?
[ "$final_ex" = "0" ] || fail "post-publish sha256sum -c failed"

RECEIPT="$PREFIX_PARENT/PUBLISH-$(date -u +%Y%m%dT%H%M%SZ).txt"
{
    echo "publish_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "source_commit=$EXPECT_COMMIT"
    echo "source_tree=$EXPECT_TREE"
    echo "prefix=$FINAL"
    echo "publish=atomic rename from $STAGE in the same parent directory"
    echo "payload_regular_files=yes link_count=1 symlinks=0"
    echo "payload_permissions=read-only (lib/ and include/ a-w, metadata 0444)"
    echo "manifest_entries=$(wc -l <"$FINAL/SHA256SUMS")"
    echo "post_publish_sha256sum_c=exit $final_ex"
    printf '%s\n' "$final_verify" | tail -25
    echo
    echo "payload_listing:"
    ls -la "$FINAL" "$FINAL/lib" "$FINAL/include/lance"
} >"$RECEIPT"
chmod 0444 "$RECEIPT"

echo "PUBLISHED=$FINAL"
echo "RECEIPT=$RECEIPT"
sed -n '1,8p' "$RECEIPT"
