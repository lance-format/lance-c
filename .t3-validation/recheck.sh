#!/bin/bash
set -u
source_dir=/home/xuanwo/Code/executions/lance-c-fragment-mutation-7ac9e26
evidence=/home/xuanwo/Code/evidence/lance-c/pr196-stream-descriptor-6424865
mkdir -p "$evidence/bin"
cd "$source_dir" || exit 2
test -z "$(git status --porcelain)" || exit 2
git show f550cf9078381b556368565c2186b757314ee2eb:.t3-validation/clang > "$evidence/bin/clang" || exit 2
chmod +x "$evidence/bin/clang"
export PATH="$evidence/bin:$PATH"
export RUSTUP_TOOLCHAIN=1.94.0 CARGO_BUILD_JOBS=8 RUST_TEST_THREADS=1 RAYON_NUM_THREADS=1
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
cargo +1.94.0 --version > "$evidence/toolchain.txt"
rustc +1.94.0 --version >> "$evidence/toolchain.txt"
for ref in fd89ad5be37db86a47af929f778ee55123fea456 64248659b32e5d3551f56b954f312733eb1d0f98; do
    git switch --detach "$ref" || exit 2
    git rev-parse HEAD 'HEAD^{tree}' > "$evidence/$ref.identity"
    git status --porcelain >> "$evidence/$ref.identity"
    (
        set -x
        cargo +1.94.0 test --locked --test compile_and_run_test test_native_fragment_mutation_consumers -- --ignored --exact --nocapture --test-threads=1
    ) > "$evidence/$ref.log" 2>&1
    result=$?
    printf '%s\n' "$result" > "$evidence/$ref.rc"
done
git status --porcelain > "$evidence/final-status.txt"
grep -q 'heap-use-after-free' "$evidence/fd89ad5be37db86a47af929f778ee55123fea456.log" || exit 3
test "$(cat "$evidence/64248659b32e5d3551f56b954f312733eb1d0f98.rc")" = 0 || exit 4
grep -q '1 passed; 0 failed' "$evidence/64248659b32e5d3551f56b954f312733eb1d0f98.log" || exit 5
printf 'RED/GREEN full consumer acceptance passed\n' > "$evidence/result.txt"
