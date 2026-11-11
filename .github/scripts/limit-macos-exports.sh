#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# rustc generates a Mach-O `-exported_symbols_list` containing every reachable
# no_mangle symbol from the static dependency graph. ld64 unions that list with
# any additional list, so the only way to keep a dependency export out of the
# final dylib is to replace rustc's list before invoking the linker.
#
# This wrapper is installed as CARGO_TARGET_AARCH64_APPLE_DARWIN_LINKER for
# macOS release and CI builds. It only changes the `liblance_c.dylib` link and
# leaves build scripts, procedural macros, tests, and executables untouched.

set -euo pipefail

real_linker=${LANCE_C_MACOS_LINKER:-/usr/bin/cc}
args=("$@")
output=""

for ((i = 0; i < ${#args[@]}; i++)); do
    case "${args[$i]}" in
        -o)
            if [ $((i + 1)) -lt "${#args[@]}" ]; then
                output="${args[$((i + 1))]}"
            fi
            ;;
    esac
done

if [ "$(basename "${output:-}")" != "liblance_c.dylib" ]; then
    exec "$real_linker" "${args[@]}"
fi

rewritten=0
for ((i = 0; i < ${#args[@]}; i++)); do
    case "${args[$i]}" in
        -Wl,*)
            IFS=',' read -r -a parts <<<"${args[$i]}"
            for ((j = 0; j < ${#parts[@]}; j++)); do
                if [ "${parts[$j]}" = "-exported_symbols_list" ] &&
                    [ $((j + 1)) -lt "${#parts[@]}" ]; then
                    printf '_lance_*\n' >"${parts[$((j + 1))]}"
                    rewritten=$((rewritten + 1))
                fi
            done
            ;;
    esac
done

if [ "$rewritten" -eq 0 ]; then
    echo "error: liblance_c.dylib link did not contain rustc's exported-symbol list" >&2
    exit 1
fi

exec "$real_linker" "${args[@]}"
