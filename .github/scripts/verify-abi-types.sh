#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Compare the released v0.2.0 public C type and function surface with the
# current headers. Both sides are compiled into DWARF-bearing, compile-only
# probes; libabigail compares the types. The stripped release shared library is
# not used because it carries no DWARF.
set -euo pipefail
export LC_ALL=C

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
old_include="$repo_root/tests/abi/v0.2.0/include"
old_header="$old_include/lance/lance.h"
baseline="$repo_root/.github/abi-baseline/v0.2.0-exports.txt"
current_include="$repo_root/include"
abidiff=${ABIDIFF:-abidiff}
cc=${CC:-cc}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

for file in "$old_header" "$baseline"; do
    [ -f "$file" ] || { echo "missing input: $file" >&2; exit 1; }
done

# The released fixture is immutable; controls mutate only temporary copies.
expected_header_sha=947a912a14cb53f97237dbc7c03e5c09ef4337d46b459edc600d4d3c2c888d5c
actual_header_sha=$(sha256_file "$old_header")
if [ "$actual_header_sha" != "$expected_header_sha" ]; then
    echo "v0.2.0 header fixture changed: $actual_header_sha" >&2
    echo "expected $expected_header_sha; restore the released bytes" >&2
    exit 1
fi
command -v "$abidiff" >/dev/null 2>&1 || { echo "abidiff not found: $abidiff" >&2; exit 1; }
command -v "$cc" >/dev/null 2>&1 || { echo "C compiler not found: $cc" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/lance-c-abi-types.XXXXXX")
trap 'rm -rf "$work"' EXIT

# Public typedefs with an inline body: structs, unions and enums.
grep -oE '^\} Lance[A-Za-z0-9_]+;' "$old_header" |
    sed 's/^} //; s/;$//' | sort -u >"$work/type_names"
# Function-pointer typedefs such as LanceWaker and the callback families.
grep -oE '\(\* ?Lance[A-Za-z0-9_]+\)' "$old_header" |
    sed -E 's/\(\* ?//; s/\)//' | sort -u >"$work/callback_names"
# Named structs with an inline body: the two named option/result structs and
# the Arrow C Data Interface structs.
grep -oE '^struct (Lance|Arrow)[A-Za-z0-9_]+ \{' "$old_header" |
    awk '{print $2}' | sort -u >"$work/named_struct_names"
# Every function published by v0.2.0.
sort -u "$baseline" >"$work/function_names"

[ -s "$work/type_names" ] || { echo "no public type names found" >&2; exit 1; }
[ -s "$work/callback_names" ] || { echo "no public callback names found" >&2; exit 1; }
[ -s "$work/named_struct_names" ] || { echo "no public named struct names found" >&2; exit 1; }
[ -s "$work/function_names" ] || { echo "no published functions found" >&2; exit 1; }

generate_probe() {
    local output=$1
    local name
    {
        echo '#include <lance/lance.h>'
        while IFS= read -r name; do
            printf '%s abi_type_%s;\n' "$name" "$name"
        done <"$work/type_names"
        while IFS= read -r name; do
            printf '%s abi_callback_%s;\n' "$name" "$name"
        done <"$work/callback_names"
        while IFS= read -r name; do
            printf 'struct %s abi_named_%s;\n' "$name" "$name"
        done <"$work/named_struct_names"
        while IFS= read -r name; do
            printf '__typeof__(&%s) abi_function_%s = &%s;\n' "$name" "$name" "$name"
        done <"$work/function_names"
    } >"$output"
}

compile_probe() {
    local include_dir=$1 output=$2
    "$cc" -std=gnu11 -g -O0 -Wno-deprecated-declarations \
        -I"$include_dir" -c "$work/probe.c" -o "$output"
}

generate_probe "$work/probe.c"
compile_probe "$old_include" "$work/old.o"
compile_probe "$current_include" "$work/current.o"

# abidiff returns an 8-bit bitmask: 1 ERROR, 2 USAGE_ERROR, 4 ABI_CHANGE and
# 8 ABI_INCOMPATIBLE_CHANGE. Only status 0 means no ABI change and no tool or
# usage error.
set +e
"$abidiff" "$work/old.o" "$work/current.o" >"$work/abidiff.out" 2>&1
status=$?
set -e
if [ "$status" -ne 0 ]; then
    if (( (status & 3) != 0 )); then
        echo "abidiff failed with a tool or usage error (exit $status):" >&2
    else
        echo "public C ABI types changed since v0.2.0 (abidiff exit $status):" >&2
    fi
    cat "$work/abidiff.out" >&2
    exit 1
fi

echo "Verified $(wc -l <"$work/type_names" | tr -d ' ') types," \
    "$(wc -l <"$work/named_struct_names" | tr -d ' ') named structs," \
    "$(wc -l <"$work/callback_names" | tr -d ' ') callback typedefs and" \
    "$(wc -l <"$work/function_names" | tr -d ' ') published functions against v0.2.0"

# Negative control: a deliberate public struct member insertion must be
# reported. The mutation is made in a temporary copy of the frozen v0.2.0
# fixture, independent of current header formatting.
cp -R "$old_include/lance" "$work/mutated-lance"
sed -i.bak 's/^} LanceOptionalU64;/    uint32_t abi_drift_control;\n} LanceOptionalU64;/' \
    "$work/mutated-lance/lance.h"
rm -f "$work/mutated-lance/lance.h.bak"
mkdir -p "$work/mutated-include"
mv "$work/mutated-lance" "$work/mutated-include/lance"
compile_probe "$work/mutated-include" "$work/mutated.o"
set +e
"$abidiff" "$work/old.o" "$work/mutated.o" >"$work/abidiff-mutated.out" 2>&1
status=$?
set -e
if (( (status & 3) != 0 )) || (( (status & 4) == 0 )) ||
    ! grep -q 'LanceOptionalU64' "$work/abidiff-mutated.out" ||
    ! grep -q 'abi_drift_control' "$work/abidiff-mutated.out"; then
    echo "negative control was not detected (abidiff exit $status):" >&2
    cat "$work/abidiff-mutated.out" >&2
    exit 1
fi
echo "Negative control: detected the intentional LanceOptionalU64 member insertion"

# Negative control: a deliberate function parameter type change must be
# reported with the affected function in the diagnostic. This also mutates a
# temporary copy of the frozen fixture.
cp -R "$old_include/lance" "$work/signature-lance"
sed -i.bak 's/\(lance_scanner_set_limit(LanceScanner\* scanner, \)int64_t/\1int32_t/' \
    "$work/signature-lance/lance.h"
rm -f "$work/signature-lance/lance.h.bak"
mkdir -p "$work/signature-include"
mv "$work/signature-lance" "$work/signature-include/lance"
compile_probe "$work/signature-include" "$work/signature.o"
set +e
"$abidiff" "$work/old.o" "$work/signature.o" >"$work/abidiff-signature.out" 2>&1
status=$?
set -e
if (( (status & 3) != 0 )) || (( (status & 4) == 0 )) ||
    ! grep -q 'abi_function_lance_scanner_set_limit' "$work/abidiff-signature.out"; then
    echo "signature negative control was not detected (abidiff exit $status):" >&2
    cat "$work/abidiff-signature.out" >&2
    exit 1
fi
echo "Negative control: detected the intentional lance_scanner_set_limit parameter change"
