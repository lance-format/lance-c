#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Strip debug/local symbols from the staged release shared library. The static
# archive is intentionally untouched so it remains usable for static linking.

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <install-prefix> <linux|macos>" >&2
    exit 2
fi

prefix=$1
platform=$2

case "$platform" in
    linux)
        library="$prefix/lib/liblance_c.so"
        strip_command=(strip --strip-all)
        ;;
    macos)
        library="$prefix/lib/liblance_c.dylib"
        strip_command=(strip -x -S)
        ;;
    *)
        echo "unsupported platform: $platform" >&2
        exit 2
        ;;
esac

if [ ! -f "$library" ]; then
    echo "staged shared library is missing: $library" >&2
    exit 1
fi

before=$(wc -c <"$library")
"${strip_command[@]}" "$library"
after=$(wc -c <"$library")

echo "Stripped $library: $before -> $after bytes"
