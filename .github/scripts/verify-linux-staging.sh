#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Check that the staged Linux shared library has only the direct runtime
# dependencies justified by the supported glibc toolchains.

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <install-prefix> <target>" >&2
    exit 2
fi

prefix=$1
target=$2

case "$target" in
    x86_64-unknown-linux-gnu)
        machine_pattern="X86-64"
        loader="ld-linux-x86-64.so.2"
        ;;
    aarch64-unknown-linux-gnu)
        machine_pattern="AArch64"
        loader="ld-linux-aarch64.so.1"
        ;;
    *)
        echo "unsupported Linux release target: $target" >&2
        exit 2
        ;;
esac

library="$prefix/lib/liblance_c.so"

if [ ! -f "$library" ]; then
    echo "staged shared library is missing: $library" >&2
    exit 1
fi

if ! header=$(readelf -h "$library" 2>&1); then
    echo "staged shared library is not a readable ELF file: $library" >&2
    echo "$header" >&2
    exit 1
fi

if ! grep -q "Machine:.*$machine_pattern" <<<"$header"; then
    actual=$(sed -n 's/.*Machine:[[:space:]]*//p' <<<"$header")
    echo "staged shared library architecture mismatch: expected $target, found ${actual:-unknown}" >&2
    exit 1
fi

if ! dynamic=$(readelf -d "$library" 2>&1); then
    echo "staged shared library has no readable dynamic section: $library" >&2
    echo "$dynamic" >&2
    exit 1
fi

if ! grep -q '^Dynamic section' <<<"$dynamic"; then
    echo "staged shared library has no dynamic section: $library" >&2
    exit 1
fi

needed=$(sed -n 's/.*(NEEDED).*Shared library: \[\(.*\)\]/\1/p' <<<"$dynamic")
if [ -z "$needed" ]; then
    echo "staged shared library has no DT_NEEDED entries: $library" >&2
    exit 1
fi

# The supported Linux release rows build on glibc 2.36 or newer. pthread and
# libdl are folded into libc there, but remain valid glibc SONAMEs for a
# toolchain that still names them explicitly. The architecture-specific
# program interpreter is also allowed. Anything else, including libssl and
# libcrypto, is an unexpected runtime dependency.
allowed=(
    "libc.so.6"
    "libm.so.6"
    "libgcc_s.so.1"
    "libpthread.so.0"
    "libdl.so.2"
    "$loader"
)

while IFS= read -r dependency; do
    [ -n "$dependency" ] || continue

    case "$dependency" in
        libssl.so.*|libcrypto.so.*)
            echo "forbidden TLS/crypto DT_NEEDED in $library: $dependency" >&2
            echo "The dependency graph uses rustls; check for native-tls or openssl-sys before merging." >&2
            exit 1
            ;;
    esac

    permitted=false
    for allowed_dependency in "${allowed[@]}"; do
        if [ "$dependency" = "$allowed_dependency" ]; then
            permitted=true
            break
        fi
    done

    if [ "$permitted" != true ]; then
        echo "unexpected DT_NEEDED in $library: $dependency" >&2
        echo "Allowed dependencies for $target: ${allowed[*]}" >&2
        exit 1
    fi
done <<<"$needed"

echo "Verified Linux runtime dependencies for $target:"
printf '  %s\n' $needed
