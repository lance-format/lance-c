#!/usr/bin/env bash
set -euo pipefail

artifact_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
native_dir=$(realpath -- "${1:?native checkout required}")
evidence_dir=$(realpath -m -- "${2:?evidence directory required}")
mkdir -p -- "$evidence_dir"

read -r native_base native_tree patch_sha < <(python3 - "$artifact_dir/identity.json" <<'PY'
import json, sys
i = json.load(open(sys.argv[1]))
print(i['base'], i['post_tree'], i['patch_sha256'])
PY
)
test "$(sha256sum "$artifact_dir/native-dataset-drop.patch" | cut -d ' ' -f 1)" = "$patch_sha"
cd -- "$native_dir"
test "$(git rev-parse HEAD)" = "$native_base"
test -z "$(git status --porcelain)"
git apply --index --whitespace=error-all "$artifact_dir/native-dataset-drop.patch"
test "$(git write-tree)" = "$native_tree"
git diff --exit-code
git diff --cached --binary > "$evidence_dir/source-before.patch"
cp -- "$artifact_dir/identity.json" "$evidence_dir/identity.json"
date -u +%FT%TZ > "$evidence_dir/started.txt"
rustc +1.94.0 --version > "$evidence_dir/toolchain.txt"
cargo +1.94.0 --version >> "$evidence_dir/toolchain.txt"
printf '%s\n' 'RUSTUP_TOOLCHAIN=1.94.0 CARGO_BUILD_JOBS=8 RUST_TEST_THREADS=4 cargo +1.94.0 test --locked -p lance --test dataset_drop -j 8 -- --test-threads=4' > "$evidence_dir/command.txt"
set +e
RUSTUP_TOOLCHAIN=1.94.0 CARGO_BUILD_JOBS=8 RUST_TEST_THREADS=4 \
    cargo +1.94.0 test --locked -p lance --test dataset_drop -j 8 -- --test-threads=4 \
    > "$evidence_dir/raw.log" 2>&1
test_exit=$?
set -e
printf '%s\n' "$test_exit" > "$evidence_dir/exit_code"
date -u +%FT%TZ > "$evidence_dir/finished.txt"
git diff --exit-code
test "$(git write-tree)" = "$native_tree"
git diff --cached --binary > "$evidence_dir/source-after.patch"
cmp -- "$evidence_dir/source-before.patch" "$evidence_dir/source-after.patch"
test "$test_exit" -eq 0
grep -F 'test result: ok. 5 passed; 0 failed;' "$evidence_dir/raw.log"
