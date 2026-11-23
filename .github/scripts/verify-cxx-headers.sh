#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Compile the public C and C++ headers with every supported compiler under
# strict warnings, so compiler-specific diagnostics fail CI instead of reaching
# consumers. Each check is a syntax-only compile of a header consumer from
# tests/cpp/ against the vendored Arrow C ABI stubs; no library build or link
# is required.
#
# Usage:
#   verify-cxx-headers.sh [<cc>:<cxx> ...]
#
# The default pairs are gcc:g++ and clang:clang++. Pass explicit pairs to test
# versioned compilers or wrappers, for example:
#   verify-cxx-headers.sh gcc14-gcc:gcc14-g++
#
# A failure prints the compiler, the exact command and the full diagnostic so
# it can be reproduced locally with one copy/paste.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

include_dir="$repo_root/include"
tests_dir="$repo_root/tests/cpp"
arrow_abi_dir="$tests_dir/arrow_abi"
arrow_abi_data_dir="$tests_dir/arrow_abi_data"

c_flags=(
    -std=c11
    -Wall -Wextra -Wpedantic -Wstrict-prototypes
    -Wconversion -Wsign-conversion
    -Werror
)

cpp_flags=(
    -std=c++20
    -Wall -Wextra -Wpedantic -Wconversion
    -Werror
    # #273 fixes the remaining lance.hpp diagnostics. Report -Wshadow and
    # -Wsign-conversion meanwhile, but keep them non-fatal; once #273 is
    # merged, drop the two -Wno-error= overrides so the full set is fatal.
    -Wshadow -Wno-error=shadow
    -Wsign-conversion -Wno-error=sign-conversion
)

failures=0
checks=0

# run_check <compiler> <standard> <source> <flag>...
# Compiles <source> with <compiler> and the given flags.
run_check() {
    local compiler=$1
    local standard=$2
    local source=$3
    shift 3
    local -a flags=("$@")
    local -a command=("$compiler" "${flags[@]}" -fsyntax-only "$source")
    local output

    checks=$((checks + 1))
    if output=$("${command[@]}" 2>&1); then
        printf 'ok:   %s %s\n' "$standard" "${source#"$repo_root/"}"
    else
        printf 'FAIL: %s %s\n' "$standard" "${source#"$repo_root/"}"
        printf '      command:'
        printf ' %q' "${command[@]}"
        printf '\n%s\n' "$output"
        failures=$((failures + 1))
    fi
}

check_c_header_sources() {
    local compiler=$1
    printf '==> %s\n' "$("$compiler" --version | head -n 1)"
    run_check "$compiler" C11 "$tests_dir/header_lance_before_arrow.c" \
        "${c_flags[@]}" -I"$include_dir" -I"$arrow_abi_dir"
    run_check "$compiler" C11 "$tests_dir/header_arrow_before_lance.c" \
        "${c_flags[@]}" -I"$include_dir" -I"$arrow_abi_dir"
    run_check "$compiler" C11 "$tests_dir/header_data_only_before_lance.c" \
        "${c_flags[@]}" -I"$include_dir" -I"$arrow_abi_data_dir"
}

check_cpp_header_sources() {
    local compiler=$1
    printf '==> %s\n' "$("$compiler" --version | head -n 1)"
    run_check "$compiler" C++20 "$tests_dir/header_lance_before_arrow.cpp" \
        "${cpp_flags[@]}" -I"$include_dir" -I"$arrow_abi_dir"
    run_check "$compiler" C++20 "$tests_dir/header_arrow_before_lance.cpp" \
        "${cpp_flags[@]}" -I"$include_dir" -I"$arrow_abi_dir"
    run_check "$compiler" C++20 "$tests_dir/header_data_only_before_lance.cpp" \
        "${cpp_flags[@]}" -I"$include_dir" -I"$arrow_abi_data_dir"
}

if [ "$#" -eq 0 ]; then
    set -- gcc:g++ clang:clang++
fi

for pair in "$@"; do
    c_compiler=${pair%%:*}
    cxx_compiler=${pair##*:}
    if [ -z "$c_compiler" ] || [ "$c_compiler" = "$pair" ] ||
        [ -z "$cxx_compiler" ]; then
        echo "invalid compiler pair '$pair'; expected <cc>:<cxx>" >&2
        exit 2
    fi

    if command -v "$c_compiler" >/dev/null 2>&1; then
        check_c_header_sources "$c_compiler"
    else
        echo "FAIL: C compiler '$c_compiler' from pair '$pair'" \
            "not found in PATH" >&2
        failures=$((failures + 1))
    fi

    if command -v "$cxx_compiler" >/dev/null 2>&1; then
        check_cpp_header_sources "$cxx_compiler"
    else
        echo "FAIL: C++ compiler '$cxx_compiler' from pair '$pair'" \
            "not found in PATH" >&2
        failures=$((failures + 1))
    fi
done

if [ "$failures" -ne 0 ]; then
    printf 'FAILED: %d failure(s), %d compile check(s) run\n' "$failures" "$checks" >&2
    exit 1
fi

printf 'All %d header checks passed\n' "$checks"
