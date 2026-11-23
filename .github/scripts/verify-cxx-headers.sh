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
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/lance-c-header-checks.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT

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
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Werror
)

failures=0
checks=0

# run_check <compiler> <standard> <source> <flag>...
# Compiles <source> with <compiler> and the given flags. Compiler output
# streams through, so non-fatal warnings stay visible on success.
run_check() {
    local compiler=$1
    local standard=$2
    local source=$3
    shift 3
    local -a flags=("$@")
    local -a command=("$compiler" "${flags[@]}" -fsyntax-only "$source")
    local relative=${source#"$repo_root/"}
    local status=0

    checks=$((checks + 1))
    printf 'check: %s %s\n' "$standard" "$relative"
    "${command[@]}" || status=$?
    if [ "$status" -eq 0 ]; then
        return
    fi

    printf 'FAIL: %s %s (exit %d)\n' "$standard" "$relative" "$status"
    printf '      reproduce:'
    printf ' %q' "${command[@]}"
    printf '\n'
    failures=$((failures + 1))
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
    for header in version error arrow_abi fwd attributes dataset scanner batch; do
        source="$work_dir/${header}.c"
        printf '#include <lance/%s.h>\nint main(void) { return 0; }\n' "$header" >"$source"
        run_check "$compiler" C11 "$source" "${c_flags[@]}" -I"$include_dir"
    done
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
    for header in version error arrow_abi fwd attributes dataset scanner batch; do
        source="$work_dir/${header}.cpp"
        printf '#include <lance/%s.h>\nint main() { return 0; }\n' "$header" >"$source"
        run_check "$compiler" C++20 "$source" "${cpp_flags[@]}" -I"$include_dir"
    done
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
