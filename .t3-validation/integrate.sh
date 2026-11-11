#!/bin/bash
set -euo pipefail
evidence=/home/xuanwo/Code/evidence/lance-c/pr196-integration-6424865-c13182c
mkdir -p "$evidence"
trap 'result=$?; echo "$result" > "$evidence/runner.rc"' EXIT
cd /home/xuanwo/Code/executions/lance-c-fragment-mutation-7ac9e26
test -z "$(git status --porcelain)"
git switch --detach e765c3f003bb158a1e4a3d818c2567ab588d3382
git rev-parse HEAD 'HEAD^{tree}' > "$evidence/identity.txt"
export RUSTUP_TOOLCHAIN=1.94.0 CARGO_BUILD_JOBS=8 RUST_TEST_THREADS=4 RAYON_NUM_THREADS=1
(set -x; cargo +1.94.0 check --all-targets --locked) > "$evidence/check.log" 2>&1
echo 0 > "$evidence/check.rc"
(set -x; cargo +1.94.0 test --locked --test c_api_test --test fragment_mutation_test -- --test-threads=4) > "$evidence/rust.log" 2>&1
echo 0 > "$evidence/rust.rc"
(set -x; cargo +1.94.0 test --locked --test compile_and_run_test -- --ignored --exact test_c_compilation_and_execution test_cpp_compilation_and_execution test_native_fragment_mutation_consumers test_native_transaction_compilation_and_execution --test-threads=1 --nocapture) > "$evidence/native.log" 2>&1
echo 0 > "$evidence/native.rc"
git status --porcelain > "$evidence/final-status.txt"
