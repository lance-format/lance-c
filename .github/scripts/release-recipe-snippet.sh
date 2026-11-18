#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Emit paste-ready vcpkg SHA512 lines for the release archives in a dist
# directory. The archive prefix uses the exact release version; a wildcard
# prefix such as "lance-c-v*-" stops at the first hyphen and turns
# "0.2.0-beta.1" into "beta.1", producing the wrong recipe keys.
#
# usage: release-recipe-snippet.sh <dist-dir> <version>

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <dist-dir> <version>" >&2
    exit 2
fi

dist=$1
version=$2
prefix="lance-c-v${version}-"

cd "$dist"

for archive in *.tar.xz; do
    case "$archive" in
        "$prefix"*.tar.xz) ;;
        *)
            echo "unexpected archive name for version ${version}: ${archive}" >&2
            exit 1
            ;;
    esac

    digest=$(awk '{print $1}' "${archive}.sha512")
    if [[ ! "$digest" =~ ^[0-9a-f]{128}$ ]]; then
        echo "missing or invalid SHA512 for ${archive}: ${digest}" >&2
        exit 1
    fi

    triple_target="${archive#"$prefix"}"
    triple_target="${triple_target%.tar.xz}"
    echo "set(LANCE_C_SHA512_${triple_target}  \"${digest}\")"
done
