#!/usr/bin/env bash
# Validation-only negative control for issue #102.
#
# Mutates temporary copies of the public header in two independent ways that do
# not change any covered size/align/offset record in PR #68's probe:
#   1. changes LANCE_ERR_PANIC from 9 to 10;
#   2. swaps two uint64_t fields in LanceSessionCacheStats.
#
# PR #68's abi_layout.c output must stay byte-identical. The current-main
# abi_layout_current.c probe must detect both mutations.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cp -R "$root/include" "$work/include-discriminant"
cp -R "$root/include" "$work/include-struct"

sed -i 's/LANCE_ERR_PANIC = 9/LANCE_ERR_PANIC = 10/' \
    "$work/include-discriminant/lance/lance.h"
python3 - "$work/include-struct/lance/lance.h" <<'PY'
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
compile "$root/tests/cpp/abi_layout.c" "$work/pr68-discriminant" "$work/include-discriminant"
compile "$root/tests/cpp/abi_layout.c" "$work/pr68-struct" "$work/include-struct"
compile "$root/tests/cpp/abi_layout_current.c" "$work/current-original" "$root/include"
compile "$root/tests/cpp/abi_layout_current.c" "$work/current-discriminant" "$work/include-discriminant"
compile "$root/tests/cpp/abi_layout_current.c" "$work/current-struct" "$work/include-struct"

"$work/pr68-original" >"$work/pr68-original.txt"
"$work/pr68-discriminant" >"$work/pr68-discriminant.txt"
"$work/pr68-struct" >"$work/pr68-struct.txt"
"$work/current-original" >"$work/current-original.txt"
"$work/current-discriminant" >"$work/current-discriminant.txt"
"$work/current-struct" >"$work/current-struct.txt"

for mutation in discriminant struct; do
    if ! diff -q "$work/pr68-original.txt" "$work/pr68-$mutation.txt" >/dev/null; then
        echo "FAIL: PR #68 probe unexpectedly noticed the $mutation mutation" >&2
        diff -u "$work/pr68-original.txt" "$work/pr68-$mutation.txt" >&2 || true
        exit 1
    fi
    echo "PASS: PR #68 probe is blind to the $mutation mutation"
done

for mutation in discriminant struct; do
    if diff -q "$work/current-original.txt" "$work/current-$mutation.txt" >/dev/null; then
        echo "FAIL: current-main probe missed the $mutation mutation" >&2
        exit 1
    fi
    echo "PASS: current-main probe detects the $mutation mutation"
    diff -u "$work/current-original.txt" "$work/current-$mutation.txt" \
        | sed -n '1,12p' || true
done
exit 0
