#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Verify that a staged release shared library exports exactly the public C ABI
# declared by the installed headers. <header-or-dir> accepts an installed
# include/lance directory or any public header inside it. A header that
# includes sibling domain headers is resolved to its installed header set; a
# standalone pre-split single header is scanned as-is.

set -euo pipefail
export LC_ALL=C

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <library> <header-or-dir> <linux|macos>" >&2
    exit 2
fi

library=$1
header_source=$2
platform=$3

if [ ! -f "$library" ]; then
    echo "shared library is missing: $library" >&2
    exit 1
fi

header_files=()
if [ -d "$header_source" ]; then
    header_dir=$header_source
    while IFS= read -r -d '' header_file; do
        header_files+=("$header_file")
    done < <(find "$header_dir" -maxdepth 1 -type f -name '*.h' -print0 | sort -z)
elif [ -f "$header_source" ]; then
    if grep -qE '^[[:space:]]*#[[:space:]]*include[[:space:]]*"lance/[^"]+\.h"' "$header_source"; then
        # The modular umbrella (or a domain header) names sibling headers, so
        # the installed set beside it is the expected public ABI.
        header_dir=$(dirname "$header_source")
        while IFS= read -r -d '' header_file; do
            header_files+=("$header_file")
        done < <(find "$header_dir" -maxdepth 1 -type f -name '*.h' -print0 | sort -z)
    else
        # Pre-split artifacts shipped one self-contained header.
        header_files=("$header_source")
    fi
else
    echo "public C headers are missing: $header_source" >&2
    exit 1
fi
if [ "${#header_files[@]}" -eq 0 ]; then
    echo "no public headers found: $header_source" >&2
    exit 1
fi

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/lance-c-exports.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT

expected="$work_dir/expected"
actual="$work_dir/actual"

for header in "${header_files[@]}"; do
    sed '/^[[:space:]]*\*/d' "$header"
done |
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
