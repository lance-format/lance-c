#!/usr/bin/env bash
# Validation-only negative control for issue #102.
#
# Mutates a temporary copy of the public header in two ways that do not change
# any covered size/align/offset record in PR #68's probe:
#   1. changes LANCE_ERR_PANIC from 9 to 10;
#   2. swaps two uint64_t fields in LanceSessionCacheStats.
#
# PR #68's abi_layout.c output must stay byte-identical. The current-main
# abi_layout_current.c probe must detect both mutations.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cp -R "$root/include" "$work/include"
header="$work/include/lance/lance.h"

sed -i 's/LANCE_ERR_PANIC = 9/LANCE_ERR_PANIC = 10/' "$header"
python3 - "$header" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text()
hits = "    uint64_t index_cache_hits;\n"
misses = "    uint64_t index_cache_misses;\n"
if text.count(hits) != 1 or text.count(misses) != 1:
    raise SystemExit("cannot locate LanceSessionCacheStats fields")
text = text.replace(hits, "__TMP_HITS__\n", 1)
text = text.replace(misses, hits, 1)
text = text.replace("__TMP_HITS__\n", misses, 1)
path.write_text(text)
PY

compile() {
    local source="$1"
    local output="$2"
    local include="$3"
    clang -std=c11 -Wall -Wextra -Werror -o "$output" "$source" -I"$include"
}

compile "$root/tests/cpp/abi_layout.c" "$work/pr68-original" "$root/include"
compile "$root/tests/cpp/abi_layout.c" "$work/pr68-mutated" "$work/include"
compile "$root/tests/cpp/abi_layout_current.c" "$work/current-original" "$root/include"
compile "$root/tests/cpp/abi_layout_current.c" "$work/current-mutated" "$work/include"

"$work/pr68-original" >"$work/pr68-original.txt"
"$work/pr68-mutated" >"$work/pr68-mutated.txt"
"$work/current-original" >"$work/current-original.txt"
"$work/current-mutated" >"$work/current-mutated.txt"

if ! diff -q "$work/pr68-original.txt" "$work/pr68-mutated.txt" >/dev/null; then
    echo "FAIL: PR #68 probe unexpectedly noticed a mutation" >&2
    diff -u "$work/pr68-original.txt" "$work/pr68-mutated.txt" >&2 || true
    exit 1
fi
echo "PASS: PR #68 probe output is identical under both mutations"

for mutation in discriminant struct-layout; do
    if diff -q "$work/current-original.txt" "$work/current-mutated.txt" >/dev/null; then
        echo "FAIL: current-main probe missed the $mutation mutation" >&2
        exit 1
    fi
done
echo "PASS: current-main probe detects both mutations"
diff -u "$work/current-original.txt" "$work/current-mutated.txt" \
    | sed -n '1,12p;220,240p' || true
exit 0
