#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Exercise the published-export baseline check in verify-shared-exports.sh.
# The fixture library exports lance_alpha and lance_beta; the cases cover an
# exact baseline, an allowed addition, a removed published symbol, an empty
# baseline and a baseline with an invalid entry.

set -euo pipefail
export LC_ALL=C

if [ "$#" -ne 1 ] || { [ "$1" != linux ] && [ "$1" != macos ]; }; then
    echo "usage: $0 <linux|macos>" >&2
    exit 2
fi
platform=$1

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/lance-c-baseline-test.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT

cat >"$work_dir/fixture.c" <<'EOF'
int lance_alpha(void) { return 1; }
int lance_beta(void) { return 2; }
EOF
cat >"$work_dir/fixture.h" <<'EOF'
int lance_alpha(void);
int lance_beta(void);
EOF

case "$platform" in
    linux)
        cc -shared -fPIC -o "$work_dir/libfixture.so" "$work_dir/fixture.c"
        library="$work_dir/libfixture.so"
        ;;
    macos)
        cc -dynamiclib -o "$work_dir/libfixture.dylib" "$work_dir/fixture.c"
        library="$work_dir/libfixture.dylib"
        ;;
esac

verify="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/verify-shared-exports.sh"

printf 'lance_alpha\nlance_beta\n' >"$work_dir/exact.txt"
printf 'lance_alpha\n' >"$work_dir/additive.txt"
printf 'lance_alpha\nlance_gamma\n' >"$work_dir/removed.txt"
: >"$work_dir/empty.txt"
printf 'lance_alpha\nother_symbol\n' >"$work_dir/invalid.txt"

run_verify() {
    bash "$verify" "$library" "$work_dir/fixture.h" "$platform" "$1" \
        >"$work_dir/stdout" 2>"$work_dir/stderr"
}

# An exact baseline passes.
run_verify "$work_dir/exact.txt"

# A baseline subset passes and reports the exports added beyond it.
if ! run_verify "$work_dir/additive.txt"; then
    echo "an additive baseline should pass" >&2
    cat "$work_dir/stderr" >&2
    exit 1
fi
if ! grep -q '^  lance_beta$' "$work_dir/stdout"; then
    echo "the additive pass did not report lance_beta" >&2
    cat "$work_dir/stdout" >&2
    exit 1
fi

# Removing a published symbol fails and names the symbol.
if run_verify "$work_dir/removed.txt"; then
    echo "removing a published symbol should fail" >&2
    exit 1
fi
if ! grep -q '^  lance_gamma$' "$work_dir/stderr"; then
    echo "the removal failure did not name lance_gamma" >&2
    cat "$work_dir/stderr" >&2
    exit 1
fi

# An empty baseline and a baseline with a non-lance entry both fail.
for fixture in empty invalid; do
    if run_verify "$work_dir/$fixture.txt"; then
        echo "a $fixture baseline should fail" >&2
        exit 1
    fi
done

echo "Export baseline verification tests passed"
