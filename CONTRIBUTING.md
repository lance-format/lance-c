# Contributing to lance-c

`lance-c` exposes the Lance Rust core to C and C++ through a stable C ABI and header-only C++20 wrappers. See the [README](https://github.com/lance-format/lance-c/blob/main/README.md) for the consumer overview and the [C and C++ quickstart](https://github.com/lance-format/lance-c/blob/main/docs/quickstart.md) for a first program.

Read [AGENTS.md](https://github.com/lance-format/lance-c/blob/main/AGENTS.md) before changing binding behavior. The design rules there take precedence over local convenience:

- New capabilities start in the pinned Lance Rust core. If the public Rust entry point is missing, record the upstream prerequisite and leave the C/C++ change out of `lance-c`.
- The selected core revision is recorded in `Cargo.toml` and `Cargo.lock`. Test the behavior of that revision, not a locally patched core.
- Keep the binding to representation, ownership, lifetimes, FFI input validation and error conversion. Do not add binding-only domain policy.
- Add public API in all layers: an `extern "C"` function under `src/`, its declaration in `include/lance/`, the C++ wrapper in `include/lance/lance.hpp` (or the appropriate inline header), and tests.

## Prerequisites

- Rust 1.98.0 or newer, CMake 3.22 or newer, and `protoc`.
- A C11 compiler for the C API and a C++20 compiler for the C++ wrappers.
- Linux x86_64 for the pinned Doxygen installer. Other hosts can install Doxygen 1.12.0 and pass `DOXYGEN_BIN` instead.
- Optional: `abigail-tools` for the ABI type check, and sanitizer runtimes for the native consumer tests.

The baseline toolchain and platform matrix is defined in [`.github/baseline.env`](https://github.com/lance-format/lance-c/blob/main/.github/baseline.env) and documented in [Building, Packaging, and Releasing](https://github.com/lance-format/lance-c/blob/main/docs/building-and-packaging.md).

## Build

Use the Cargo build for day-to-day development:

```bash
cargo check --all-targets
cargo build --release
```

The CMake integration is the installed-consumer path:

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

`cargo build --release` writes the libraries to `target/release/`; public headers stay in `include/lance/`. The CMake presets may need network access for an uncached Corrosion or for Rust dependencies that are not already cached.

## Test

Run the same checks as CI, in this order:

```bash
cargo fmt -- --check
cargo clippy --all-targets -- -D warnings
cargo test
cargo test --test compile_and_run_test -- --ignored
```

The ignored suite compiles the C and C++ consumers under `tests/cpp/`, links them against the freshly built library, and runs them. It is the primary check for ownership and lifetime behavior across the FFI boundary; a Rust-only test is not a substitute.

Additional native checks:

```bash
# Compile the public headers with GCC and Clang under strict warnings.
bash .github/scripts/verify-cxx-headers.sh

# Build a staged package and verify the installed CMake/pkg-config consumers.
cargo build --lib
bash .github/scripts/verify-cmake-consumer.sh --from-target-dir target/debug
```

On Linux, run a native consumer under sanitizers when changing ownership or error paths:

```bash
LANCE_C_SANITIZERS=address,undefined ASAN_OPTIONS=detect_leaks=1 \
  cargo test --test compile_and_run_test -- --ignored test_c_compilation_and_execution
```

On Windows, the same C/C++ tests run on both MSVC targets. See [Running the native test suite on Windows](https://github.com/lance-format/lance-c/blob/main/docs/building-and-packaging.md#running-the-native-test-suite-on-windows) for the MSVC environment setup.

## Doxygen checks

Public headers are part of the API contract. Document every public declaration and run the same check as CI when changing `include/lance/`:

```bash
bash .github/scripts/install-doxygen.sh
# Add the bin directory printed by the installer to PATH, then:
doxygen Doxyfile
```

The HTML output is written to `build/doxygen/html/`. To run the full validation, including the self-test and `WARN_AS_ERROR = FAIL_ON_WARNINGS`:

```bash
DOXYGEN_BIN=/path/to/doxygen bash .github/scripts/check-doxygen.sh
```

A warning fails the `API docs (Doxygen)` CI job. The published render is available at <https://lance-format.github.io/lance-c/>. See [Generated C/C++ API reference](https://github.com/lance-format/lance-c/blob/main/docs/api-reference.md) for the Doxyfile input set and the CI assertions.

## ABI expectations

The `0.2.x` line preserves the public C ABI. Releases may add entry points, but must not remove or rename an existing `lance_*` export, change a public function signature, or change a public struct, enum or field layout. C++ wrapper changes must preserve the documented ownership and error behavior.

Several checks protect that contract:

- `.github/abi-baseline/v0.2.0-exports.txt` records the exports published by `v0.2.0`. The release and CI builds compare every staged shared library against it. Removing a published export fails the build; new exports are reported and allowed.
- `tests/abi/README.md` and `.github/scripts/verify-abi-types.sh` compare the released `v0.2.0` C header with the current headers using libabigail. Run it locally with `abigail-tools` installed:
  ```bash
  bash .github/scripts/verify-abi-types.sh
  ```
- `tests/support/abi_layout.rs` checks current struct sizes, alignments, field offsets and enum values against the Rust definitions.
- `test_public_header_signatures_match_rust` compiles the public header declarations against the Rust `extern "C"` signatures and checks that both name sets agree.

When adding a public function, update the header, the C++ wrapper and the tests. A new export is additive and does not require editing the baseline. Do not update the baseline to make a removal pass: adopting a new baseline is an explicit ABI decision and must be called out in the release notes.

## Documentation

- Keep the [API guide](https://github.com/lance-format/lance-c/blob/main/docs/api-guide.md), the [building and packaging guide](https://github.com/lance-format/lance-c/blob/main/docs/building-and-packaging.md), and the runnable examples under `examples/` consistent with the public headers and the selected core.
- Add a corresponding Doxygen comment when adding a public declaration.
- Relative links under `docs/` must resolve both in the repository and when the directory is assembled into MkDocs. Links to files outside `docs/`, or to repository-only directories, use absolute `https://github.com/lance-format/lance-c/...` URLs.
- Use fenced code blocks with a language identifier and ordinary ATX headings (`##`), so the pages render in both GitHub and MkDocs.

## Submitting a change

Open one pull request per logical change and describe the behavior change, the tests run, and any ABI or compatibility impact. Keep `Cargo.lock` changes intentional. Add the SPDX header to new source, script, CMake and header files; the `Format & License` CI job checks it.

Before requesting review, confirm that formatting, Clippy, the Rust tests, the C/C++ compilation tests and the Doxygen check for changed headers pass. CI also runs the tests on macOS and both Windows targets.
