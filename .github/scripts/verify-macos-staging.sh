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
    actual=$(lipo -archs "$library")
    if [ "$actual" != "$architecture" ]; then
        echo "Expected $architecture in $library, found $actual" >&2
        exit 1
    fi
done

xcrun vtool -show-build "$prefix/lib/liblance_c.dylib"
