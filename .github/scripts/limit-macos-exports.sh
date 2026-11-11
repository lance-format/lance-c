#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# rustc generates a Mach-O `-exported_symbols_list` containing every reachable
# no_mangle symbol from the static dependency graph. ld64 unions that list with
# any additional list, so the only way to keep a dependency export out of the
# final dylib is to replace rustc's list before invoking the linker.
#
# Direct Cargo CI builds select this wrapper through
# CARGO_TARGET_AARCH64_APPLE_DARWIN_LINKER. CMake builds use corrosion_set_linker
# so Corrosion preserves the selection; LANCE_C_MACOS_LINKER names the underlying
# compiler driver. Only the `liblance_c.dylib` link is changed. Build scripts,
# procedural macros, tests, and executables are left untouched.

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

# Normalize compiler-driver forwarding before inspecting linker arguments.
# rustc can place the flag and filename in separate -Wl arguments.
linker_args=()
for arg in "${args[@]}"; do
    case "$arg" in
        -Wl,*)
            IFS=',' read -r -a parts <<<"${arg#-Wl,}"
            linker_args+=("${parts[@]}")
            ;;
        -Xlinker) ;;
        *) linker_args+=("$arg") ;;
    esac
done

rewritten=0
for ((i = 0; i < ${#linker_args[@]}; i++)); do
    if [ "${linker_args[$i]}" = "-exported_symbols_list" ] &&
        [ $((i + 1)) -lt "${#linker_args[@]}" ]; then
        export_list="${linker_args[$((i + 1))]}"
        if [ ! -f "$export_list" ]; then
            echo "error: exported-symbol list does not exist: $export_list" >&2
            exit 1
        fi
        printf '_lance_*\n' >"$export_list"
        rewritten=$((rewritten + 1))
    fi
done

if [ "$rewritten" -eq 0 ]; then
    echo "error: liblance_c.dylib link did not contain rustc's exported-symbol list" >&2
    exit 1
fi

exec "$real_linker" "${args[@]}"
