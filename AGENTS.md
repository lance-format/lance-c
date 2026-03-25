# AGENTS.md

## Structure
Rust FFI source: `src/`
C header (stable ABI): `include/lance.h`
C++ RAII wrappers (header-only): `include/lance.hpp`
Tests (Rust): `tests/c_api_test.rs`
Tests (C/C++): `tests/cpp/`
Historical test data: `test_data/`

## Commands
check: `cargo check --all-targets`
format: `cargo fmt`
lint: `cargo clippy --all-targets -- -D warnings`
test: `cargo test`
test C/C++ compilation: `cargo test --test compile_and_run_test -- --ignored`

## Key Patterns
- Opaque handles with `lance_*_open`/`lance_*_close` lifecycle.
- Thread-local error handling via `ffi_try!` macro.
- Arrow C Data Interface for zero-copy data exchange.
- `panic = "abort"` in release to prevent unwinding across FFI.

## Adding New APIs
1. Add `extern "C"` function in `src/`.
2. Add declaration to `include/lance.h`.
3. Add C++ wrapper to `include/lance.hpp`.
4. Add test in `tests/c_api_test.rs`.

