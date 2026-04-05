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

## Coding Standards

### Cross-Language Bindings
- Keep C/C++ bindings as thin wrappers — centralize validation and logic in the Rust core.
- Keep parameter names consistent across all bindings (Rust, C, C++) — rename everywhere or nowhere.
- Never break public API signatures — deprecate with `#[deprecated]` and add a new method.
- Replace mutually exclusive boolean flags with a single enum/mode parameter.

### Naming
- Name variables after what the value *is* (e.g., `partition_id` not `mask`).
- Drop redundant prefixes when the struct/module already implies the domain.
- Use `indices` (not `indexes`) consistently in all APIs and docs.

### Error Handling
- Validate inputs and reject invalid values with descriptive errors at API boundaries — never silently clamp or adjust.
- Include full context in error messages: variable names, values, sizes, types.

### Testing
- All bugfixes and features must have corresponding tests.
- Cover NULL/empty edge cases.
- Include multi-fragment scenarios for dataset operations.

### Dependencies
- Prefer the standard library or existing workspace dependencies before adding new external crates.
- Keep `Cargo.lock` changes intentional; revert unrelated dependency bumps.

## Adding New APIs
1. Add `extern "C"` function in `src/`.
2. Add declaration to `include/lance.h`.
3. Add C++ wrapper to `include/lance.hpp`.
4. Add test in `tests/c_api_test.rs`.

