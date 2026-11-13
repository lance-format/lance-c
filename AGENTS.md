# AGENTS.md

## Design Principles

- This project provides C/C++ bindings for Lance. New product capabilities must
  originate in the Lance Rust project; binding or parity work must not initiate
  native feature development, including local native patches or prototypes.
- Before adding an API, identify the public Rust entry point in the selected
  Lance revision and its behavior, defaults, validation and error semantics.
  If that capability is missing, record the upstream prerequisite and leave it
  outside binding implementation until Lance provides it.
- Use pylance to discover capabilities, not to define new core behavior.
  Python-side orchestration does not establish a public Rust capability, and
  implementing the same policy in C/C++ is not a substitute for one.
- Keep binding responsibilities to foreign-language representation, ownership,
  lifetimes, FFI input validation and error conversion. Delegate domain logic
  and operation semantics to Rust rather than adding binding-specific policy.
- Validate observable C/C++ behavior against the selected Rust implementation.
  A successful experiment with a patched core does not establish support in
  the project's selected dependency.

## Structure
Rust FFI source: `src/`
C header (stable ABI): `include/lance/lance.h`
C++ RAII wrappers (header-only): `include/lance/lance.hpp`
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
- `panic = "unwind"` is required so guarded FFI boundaries can translate
  panics to `LANCE_ERR_PANIC`; `panic = "abort"` builds are rejected.

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
