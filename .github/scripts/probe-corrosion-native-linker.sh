#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <source-dir> <evidence-dir> <expect: shim|default>" >&2
    exit 2
fi

source_dir=$(cd "$1" && pwd)
evidence_dir=$2
expect=$3

case "$expect" in
    shim|default) ;;
    *)
        echo "unsupported expectation: $expect" >&2
        exit 2
        ;;
esac

mkdir -p "$evidence_dir/bin"
real_rustc=$(rustup which --toolchain 1.94.0 rustc)
real_cargo=$(rustup which --toolchain 1.94.0 cargo)
fake_rustc="$evidence_dir/bin/rustc"

cat >"$fake_rustc" <<EOF
#!/usr/bin/env bash
set -euo pipefail

if [ "\${1:-}" = "--version" ] && [ "\${2:-}" = "--verbose" ]; then
    cat <<'RUSTC_VERSION'
rustc 1.94.0 (4a4ef493e 2026-03-02)
binary: rustc
commit-hash: 4a4ef493e3a1488c6e321570238084b38948f6db
commit-date: 2026-03-02
host: x86_64-apple-darwin
release: 1.94.0
LLVM version: 21.1.8
RUSTC_VERSION
    exit 0
fi

exec "$real_rustc" "\$@"
EOF
chmod +x "$fake_rustc"

cmake -S "$source_dir" -B "$evidence_dir/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DRust_COMPILER="$fake_rustc" \
    -DRust_CARGO="$real_cargo"

printf '\n== FindRust cache values ==\n'
grep '^Rust_CARGO_TARGET' "$evidence_dir/build/CMakeCache.txt" || true

printf '\n== Generated linker environment ==\n'
cmake --build "$evidence_dir/build" --target _cargo-build_lance_c -- -n \
    >"$evidence_dir/dry-run.txt" 2>&1
sed 's/ /\n/g' "$evidence_dir/dry-run.txt" |
    grep -E 'CARGO_TARGET_.*_LINKER=|LANCE_C_MACOS_LINKER=' |
    tee "$evidence_dir/linker-env.txt"

if grep -Fq 'limit-macos-exports.sh' "$evidence_dir/linker-env.txt"; then
    observed=shim
else
    observed=default
fi

printf '\nObserved linker selection: %s (expected %s)\n' "$observed" "$expect"
test "$observed" = "$expect"
