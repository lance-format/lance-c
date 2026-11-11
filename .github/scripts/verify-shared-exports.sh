#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Verify that a staged release shared library exports exactly the public C ABI
# declared by the installed header.

set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <library> <header> <linux|macos>" >&2
    exit 2
fi

library=$1
header=$2
platform=$3

if [ ! -f "$library" ]; then
    echo "shared library is missing: $library" >&2
    exit 1
fi
if [ ! -f "$header" ]; then
    echo "public C header is missing: $header" >&2
    exit 1
fi

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/lance-c-exports.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT

expected="$work_dir/expected"
actual="$work_dir/actual"

sed '/^[[:space:]]*\*/d' "$header" |
    grep -oE 'lance_[A-Za-z0-9_]+[[:space:]]*\(' |
    sed 's/[[:space:]]*($//' |
    sort -u >"$expected"

case "$platform" in
    linux)
        nm -D --defined-only -j "$library" |
            sed 's/@.*//' |
            sort -u >"$actual"
        ;;
    macos)
        nm -gU -j "$library" |
            sed 's/^_//' |
            sort -u >"$actual"
        ;;
    *)
        echo "unsupported platform: $platform" >&2
        exit 2
        ;;
esac

if [ ! -s "$expected" ]; then
    echo "no public lance_* declarations found in $header" >&2
    exit 1
fi
if [ ! -s "$actual" ]; then
    echo "no defined dynamic exports found in $library" >&2
    exit 1
fi

missing="$work_dir/missing"
unexpected="$work_dir/unexpected"
comm -23 "$expected" "$actual" >"$missing"
comm -13 "$expected" "$actual" >"$unexpected"

if [ -s "$missing" ] || [ -s "$unexpected" ]; then
    echo "shared-library export set does not match the public C header" >&2
    if [ -s "$missing" ]; then
        echo "missing public exports:" >&2
        sed 's/^/  /' "$missing" >&2
    fi
    if [ -s "$unexpected" ]; then
        echo "unexpected dependency exports:" >&2
        sed 's/^/  /' "$unexpected" >&2
    fi
    exit 1
fi

echo "Verified $(wc -l <"$actual" | tr -d ' ') public lance_* exports in $library"
