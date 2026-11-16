#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage $0 <install-prefix> <architecture>" >&2
    exit 2
fi

prefix=$1
architecture=$2

for library in "$prefix/lib/liblance_c.a" "$prefix/lib/liblance_c.dylib"; do
    if [ ! -f "$library" ]; then
        echo "staged library is missing: $library" >&2
        exit 1
    fi
    echo "==> $library"
    file "$library"
    lipo -info "$library"
    actual=$(lipo -archs "$library")
    if [ "$actual" != "$architecture" ]; then
        echo "Expected $architecture in $library, found $actual" >&2
        exit 1
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$library"
    else
        shasum -a 256 "$library"
    fi
    wc -c "$library"
done

shared="$prefix/lib/liblance_c.dylib"
expected_install_name="@rpath/liblance_c.dylib"

echo "==> Install name"
otool -D "$shared"
install_name=$(otool -D "$shared" | sed -n '2p' | tr -d '[:space:]')
if [ "$install_name" != "$expected_install_name" ]; then
    echo "Expected install name $expected_install_name in $shared, found $install_name" >&2
    exit 1
fi

echo "==> Runtime dependencies"
otool -L "$shared"
dependencies=$(otool -L "$shared" | awk 'NR > 1 {print $1}')
if [ -z "$dependencies" ]; then
    echo "No runtime dependencies found in $shared" >&2
    exit 1
fi
while IFS= read -r dependency; do
    case "$dependency" in
        "$expected_install_name" | /usr/lib/* | /System/Library/* | /Library/Apple/*) ;;
        *)
            echo "Unexpected non-system runtime dependency in $shared: $dependency" >&2
            exit 1
            ;;
    esac
done <<<"$dependencies"

echo "==> Build metadata"
xcrun vtool -show-build "$shared"
