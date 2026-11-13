#!/usr/bin/env bash
# Run the last-as-prefix oracle on a disposable copy of the pinned Lance checkout.
#
# Usage: run-nier.sh <lance-src-copy> <artifact-dir> <status-file>
#
# <lance-src-copy> is a copy of the 40f15ed checkout; this script never touches
# the cargo checkout itself. <artifact-dir> holds native-last-as-prefix.patch and
# fts_last_as_prefix.rs, transported through Git.
set -euo pipefail

src=${1:?lance source copy}
artifacts=${2:?artifact dir}
status=${3:?status file}

expected=40f15ed1b71b66950961b175829fdf17a763f108
head=$(git -C "$src" rev-parse HEAD)
if [[ $head != "$expected" ]]; then
    echo "unexpected HEAD: $head (expected $expected)" >&2
    exit 2
fi

patch_file="$artifacts/native-last-as-prefix.patch"
test_file="$artifacts/fts_last_as_prefix.rs"

if git -C "$src" apply --reverse --check "$patch_file" 2>/dev/null; then
    echo "patch already applied"
else
    git -C "$src" apply --check "$patch_file"
    git -C "$src" apply "$patch_file"
fi

cp "$test_file" "$src/rust/lance/tests/fts_last_as_prefix.rs"

log="$artifacts/oracle.log"
set +e
(
    cd "$src"
    mbx cargo test -p lance --test fts_last_as_prefix -- --test-threads=4
) 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
set -e

summary=$(grep -E '^test result:' "$log" | tail -1)
tmp="$status.tmp"
{
    printf '{\n'
    printf '  "head": "%s",\n' "$head"
    printf '  "exit_code": %d,\n' "$rc"
    printf '  "summary": "%s",\n' "${summary//\"/\\\"}"
    printf '  "log": "%s"\n' "$log"
    printf '}\n'
} >"$tmp"
mv "$tmp" "$status"
exit "$rc"
