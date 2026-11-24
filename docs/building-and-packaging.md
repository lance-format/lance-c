# Building, Packaging, and Releasing

This document provides platform-specific build instructions, binary packaging contracts, and release procedures for `lance-c`. For the top-level overview and quickstart, see [`README.md`](../README.md).

## Contents

- [Published prebuilt archives](#published-prebuilt-archives)
- [ABI compatibility](#abi-compatibility)
- [Linux runtime requirements](#linux-runtime-requirements)
- [Try the published package](#try-the-published-package)
- [Choosing static or shared linkage](#choosing-static-or-shared-linkage)
- [Building from source via Cargo](#from-source-via-cargo-rust-developers)
- [Building from source via CMake](#from-source-via-cmake-cc-developers-and-packagers)
- [Windows (`x86_64-pc-windows-msvc` and `aarch64-pc-windows-msvc`)](#windows-x86_64-pc-windows-msvc-and-aarch64-pc-windows-msvc)
- [Releasing](#releasing)

---

## Published prebuilt archives

Each release `.tar.xz` archive unpacks an install prefix containing:

- Public headers under `include/lance/` (the v0.2.0 archive contains `lance.h` and `lance.hpp`; check the archive for its exact header set)
- Static and shared libraries under `lib/` (plus `lib/lance_c.dll.lib` on Windows)
- `lib/cmake/LanceC/` (`LanceCConfig.cmake` and `LanceCConfigVersion.cmake`)
- `lib/pkgconfig/lance-c.pc`
- `share/licenses/lance-c/LICENSE` (and `share/doc/lance-c/README.md` in Windows archives)

In the published archives, Linux and macOS release shared libraries (`liblance_c.so`, `liblance_c.dylib`) are stripped of debug/local symbols and restrict their defined dynamic exports to the public `lance_*` C ABI. Windows requires both `lance_c.dll` and `lance_c.dll.lib` to expose only the header-declared `lance_*` symbols. Static archives (`liblance_c.a`, `lance_c.lib`) are unstripped so they retain the symbol table required for static linking.

### Stable release archives

Stable downloads are on the [latest stable release](https://github.com/lance-format/lance-c/releases/latest); the [releases page](https://github.com/lance-format/lance-c/releases) lists every release and its notes. Stable releases publish the five prebuilt archives listed below, plus per-archive `.sha256` and `.sha512` files and a five-line `SHA512SUMS` that verifies all five archives. Replace `<release-tag>` below with the tag you downloaded, including its `v` prefix.

| Platform | Archive | Runtime & ABI contract |
| --- | --- | --- |
| Linux `x86_64` | `lance-c-<release-tag>-x86_64-unknown-linux-gnu.tar.xz` | `glibc >= 2.28`; links only `libgcc_s.so.1`, `libm.so.6`, `libc.so.6`, `ld-linux-x86-64.so.2` (`pthread`/`dl` on older glibc); C++20 for `lance.hpp` |
| Linux `aarch64` | `lance-c-<release-tag>-aarch64-unknown-linux-gnu.tar.xz` | `glibc >= 2.28`; links only `libgcc_s.so.1`, `libm.so.6`, `libc.so.6`; C++20 for `lance.hpp` |
| macOS `arm64` | `lance-c-<release-tag>-aarch64-apple-darwin.tar.xz` | Apple silicon (`minos 11.0`); C++20 for `lance.hpp` |
| Windows `x86_64` | `lance-c-<release-tag>-x86_64-pc-windows-msvc.tar.xz` | Windows x64, MSVC static CRT (`/MT`, `LanceC_STATIC_CRT=ON`); C++20 for `lance.hpp` |
| Windows `ARM64` | `lance-c-<release-tag>-aarch64-pc-windows-msvc.tar.xz` | Windows 11 ARM64, MSVC static CRT (`/MT`, `LanceC_STATIC_CRT=ON`); C++20 for `lance.hpp` |

The stable prebuilt baseline is these five targets: Linux x86_64 and AArch64 with glibc 2.28+, Apple silicon macOS, and Windows x64 and ARM64 with the static `/MT` CRT. C consumers need C11; the C++ wrapper needs C++20.

The v0.2.0 release was the first stable release of the 0.2.x line and established the ABI baseline described below. It pinned Lance 15.0.0-beta.25 (`f5dbd55d`).

<a id="abi-compatibility"></a>

## ABI compatibility

The public C ABI is the `lance_*` function set declared by the installed `include/lance/` headers. The 0.2.x line, introduced by v0.2.0, preserves it: releases may add entry points, but must not remove or change an existing one.

- Release and CI builds compare each staged Linux, macOS, and Windows shared library with [`.github/abi-baseline/v0.2.0-exports.txt`](../.github/abi-baseline/v0.2.0-exports.txt), the export names published by the verified `v0.2.0` release. Dropping a published name fails the build even when the declaration and the library are changed together; new exports are allowed and reported. This is an export-name comparison only: it does not detect changed signatures, struct or enum layouts, or changed behavior. The same build still requires the library to match the installed header set exactly, and `tests/support/abi_layout.rs` checks struct sizes, alignments, field offsets and enum values against the current Rust definitions.
- `LANCE_API`, `LANCE_DEPRECATED(message)` and `LANCE_NODISCARD` are defined in `include/lance/attributes.h`, which every public declaration header includes. On Windows, declarations are ordinary by default, so one header works with both the static archive and the DLL import library. The installed CMake shared target (`LanceC::lance_c_shared`, or `LanceC::lance_c` with `-DLanceC_LINK=shared`) defines `LANCE_C_SHARED`, which opts into `__declspec(dllimport)`; direct DLL consumers may define it themselves. Static consumers need no macro.
- The shared-library file names stay unversioned (`liblance_c.so`, `liblance_c.dylib`, `lance_c.dll`) in the 0.2.x line. A versioned SONAME or install name changes a loader contract that published archives and downstream native modules currently pin, so it needs its own migration plan.
- The Go bindings already parse declaration attributes and modular headers (lance-go #120, #132 and #129). A production repin still needs the published archive identities for the split header set; this repository does not publish a native pin.

<a id="linux-runtime-requirements"></a>

## Linux runtime requirements

The two Linux archives target glibc 2.28 on x86_64 and AArch64. Source builds link against the build host’s libc.

<a id="try-the-published-package"></a>

## Try the published package

Follow the [quickstart](../README.md#quickstart) to verify the archive and open the sample dataset. For shared linkage, pass `-DLanceC_LINK=shared` to CMake. On macOS arm64, choose the Apple silicon archive and verify its per-archive checksum with `shasum -a 512 -c "$ARCHIVE.sha512"` after downloading both files.

## Choosing static or shared linkage

Every package installs both the static archive and the shared library, and the two differ in what they define and what they need at load time.

| | Static (default) | Shared |
| --- | --- | --- |
| Link input | `liblance_c.a` / `lance_c.lib` | `liblance_c.so` / `liblance_c.dylib` / `lance_c.dll.lib` |
| Runtime file | none; the executable carries the pulled archive members | `liblance_c.so` / `liblance_c.dylib` / `lance_c.dll` |
| Public symbols | the global symbols of the archive members the linker pulls in, including embedded native libraries | only the `lance_*` exports; internal definitions are not exported |
| Loader requirement | the platform system libraries still apply | the loader must find the runtime file |

The release static archive is deliberately unstripped: it keeps the symbol table static linking requires and carries the compiled Rust graph together with the native C libraries it embeds, currently AWS-LC and zstd/LZ4. A statically linked consumer defines the global symbols of the archive members it pulls in. Overlap is about symbol names, not about both sides using the same kind of library: most AWS-LC symbols carry a versioned prefix (`aws_lc_0_39_0_*`), so linking another OpenSSL/BoringSSL-family library is not by itself a name conflict, while zstd and LZ4 keep their usual `ZSTD_*` and `LZ4_*` names that another copy also defines. When the host links a library exporting the same names, the link can resolve references against either copy depending on link order and the pulled archive members, or fail with duplicate-definition errors.

Use shared linkage when the host itself links zstd, LZ4 or another library that exports the same symbol names, or when several components in one process link lance-c. The shared library exports only the `lance_*` C ABI; its embedded native symbols are hidden from those public exports and do not appear in its dynamic symbol table. Make the installed shared library available to the loader through the system library path, an `$ORIGIN`-based RUNPATH, `LD_LIBRARY_PATH`, or the local equivalent. Copying it next to an ELF executable does not make it loadable unless the loader also searches that directory.

Use static linkage when the host does not link libraries exporting those names and should not depend on the lance-c shared library. Static targets propagate the required platform system libraries, such as `-lpthread -ldl -lm` on Linux; shared linkage resolves those dependencies through the shared library.

With CMake, set `LanceC_LINK` before `find_package`:

```cmake
set(LanceC_LINK shared)
find_package(LanceC CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE LanceC::lance_c)
```

For an embedded build (`add_subdirectory()` or `FetchContent_MakeAvailable()`), set `LANCE_C_LINK` to `shared` before adding the subdirectory; the same `LanceC::` targets are created.

C applications and C++ applications that use only `<lance/lance.h>` link `LanceC::lance_c`. For `<lance/lance.hpp>`, link `LanceC::lance_cpp`; it adds the C++20 compile feature and links `LanceC::lance_c`, so the same linkage selection applies to the wrapper. The [CMake source build section](#from-source-via-cmake-cc-developers-and-packagers) lists the targets and the C++17/C++20 migration.

On Windows, shared linkage also removes the `/MT` static-CRT requirement for consumers; see [Windows linking](#consuming-the-windows-package-and-mt-crt-rules). pkg-config already selects the shared library when both are installed; [Using pkg-config](#using-pkg-config) shows the explicit static invocation.

<a id="from-source-via-cargo-rust-developers"></a>
## Building from source via Cargo

Source builds of the current tree require **Rust 1.95.0+**, **CMake 3.22+**, and **`protoc`**. Baseline build targets are defined in [`.github/baseline.env`](../.github/baseline.env):

| Component | Target / minimum |
| --- | --- |
| Rust | 1.95.0+ |
| CMake | 3.22+ |
| C / C++ | C11 or C++20 |
| Compiler | GCC / Clang 20; Apple Clang 15; MSVC (ClangCL on Windows ARM64) |
| macOS | Apple silicon (`arm64`) only (`MACOSX_DEPLOYMENT_TARGET=11.0`) |

To build the library directly with Cargo:

```bash
cargo build --release
```

This produces `target/release/liblance_c.{so,dylib}` (or `lance_c.dll`) and `target/release/liblance_c.a` (or `lance_c.lib`). Public headers are in [`include/lance/`](../include/lance/).

<a id="from-source-via-cmake-cc-developers-and-packagers"></a>
## Building from source via CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build --prefix /your/prefix
```

An installed Corrosion 0.5 package in `CMAKE_PREFIX_PATH` is used before
fetching the pinned Corrosion 0.5.1 sources. This also works on CMake 3.22;
offline builds must provide the Rust dependencies separately.

The equivalent preset commands are `cmake --preset release`,
`cmake --build --preset release`, and `ctest --preset release`.
Standalone builds enable `BUILD_TESTING` by default. CTest runs the CMake
metadata and consumer compilation tests without rebuilding Rust; use
`-DBUILD_TESTING=OFF` to omit them. Embedded builds leave testing and compiler
defaults under the parent project's control.

On macOS, export `MACOSX_DEPLOYMENT_TARGET` in the environment that invokes CMake. With the pinned Corrosion 0.5.1, setting only `CMAKE_OSX_DEPLOYMENT_TARGET` does not propagate to Cargo build scripts:

```bash
export MACOSX_DEPLOYMENT_TARGET=11.0
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET"
cmake --build build --parallel
cmake --install build --prefix /your/prefix
```

`cmake --install` installs the headers, both static and shared libraries, `LanceCConfig.cmake`, and `lance-c.pc`. Consumers can then link via:

```cmake
find_package(LanceC CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE LanceC::lance_c)
```

`LanceC::lance_c`, `LanceC::lance_c_static`, and `LanceC::lance_c_shared`
expose the C11 API in `<lance/lance.h>`. They also support C++ applications
using that header without raising the application's C++ standard.
For the header-only C++20 wrappers in `<lance/lance.hpp>`, use:

```cmake
target_link_libraries(myapp PRIVATE LanceC::lance_cpp)
```

`LanceC::lance_cpp` links the default C target, so `LanceC_LINK=shared` selects
shared linkage for the wrapper too. Static targets propagate the required
platform system libraries; shared targets keep those dependencies private.
The same targets are available through `add_subdirectory()` and `FetchContent`;
set `LANCE_C_LINK` for their default linkage.

Migration: C++ consumers that previously relied on `LanceC::lance_c` to enable
C++20 should switch to `LanceC::lance_cpp`, or explicitly request
`target_compile_features(myapp PRIVATE cxx_std_20)` while linking their chosen
C target. Older release packages without `LanceC::lance_cpp` still need that
explicit C++ standard.

An unversioned `find_package(LanceC CONFIG REQUIRED)` accepts the package in the selected prefix. Pass a version number only to require a specific minor release line; `SameMinorVersion` compatibility rejects a `0.1` request against a `0.2` package.

### Using pkg-config

The install prefix also contains `lib/pkgconfig/lance-c.pc`. Point `pkg-config` at that directory and use the flags directly:

```bash
export PKG_CONFIG_PATH=/your/prefix/lib/pkgconfig
cc myapp.c $(pkg-config --cflags --libs lance-c) -o myapp
```

`pkg-config --static` adds the platform system libraries recorded in `Libs.private`. The linker still prefers the shared library when both are installed; name `liblance_c.a` explicitly or use your linker's static-library option. On Windows the `.pc` records the same system libraries as `LanceCConfig.cmake`, but MSVC consumers normally use the CMake package.

<a id="windows-x86_64-pc-windows-msvc-and-aarch64-pc-windows-msvc"></a>
## Windows (`x86_64-pc-windows-msvc` and `aarch64-pc-windows-msvc`)

Windows prebuilt archives are published in stable releases; see the [latest stable release](https://github.com/lance-format/lance-c/releases/latest) and the [archive table](#published-prebuilt-archives).

### Building from source on Windows

The Windows CMake build links the MSVC C runtime statically (`/MT`) by default for both the DLL and static archive (`-DLANCE_C_CRT_STATIC=ON`). Pass `-DLANCE_C_CRT_STATIC=OFF` in a source build to opt into the dynamic CRT (`/MD`).

For Windows `x64`:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_BUILD_TYPE=Release -DLANCE_C_CRT_STATIC=ON
cmake --build build --config Release
cmake --install build --prefix C:\path\to\prefix --config Release
```

For native Windows `ARM64` (AWS-LC requires `clang-cl` for its Windows ARM64 assembly sources, so select the Visual Studio `ClangCL` toolset):

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A ARM64 -T ClangCL `
  -DCMAKE_BUILD_TYPE=Release -DLANCE_C_CRT_STATIC=ON
cmake --build build --config Release
cmake --install build --prefix C:\path\to\prefix --config Release
```

For a direct Cargo build without CMake, enable the `+crt-static` target feature explicitly so Cargo, `cc`, and AWS-LC's CMake build select the static CRT:

```powershell
$env:RUSTFLAGS = '-Ctarget-feature=+crt-static'
cargo build --release --target x86_64-pc-windows-msvc
```

When staging existing Windows libraries with `-DLANCE_C_USE_PREBUILT=ON`, you must also pass `-DLANCE_C_CRT_STATIC=ON` (for Release `/MT` inputs) or `OFF` (for dynamic-CRT inputs) so the installed `LanceCConfig.cmake` records accurate metadata. The Windows package export filter wraps MSVC `link.exe`; overriding the linker with `lld-link` is not supported.

### Running the native test suite on Windows

`cargo test` runs the Rust suite, and `cargo test --test compile_and_run_test -- --ignored` compiles every C and C++ consumer under `tests/cpp/` against the freshly built `lance_c.dll` and runs it. The harness resolves `cl.exe` with the same target-aware MSVC lookup that `build.rs` uses for the export library and keeps the tool environment that lookup returns (`INCLUDE`, `LIB`, and `PATH`), so both commands work from a plain PowerShell window without a developer prompt and never depend on a `cl.exe` from `PATH` that targets another architecture. It links `lance_c.dll.lib` and copies the DLL beside each test binary. This applies to `cargo test`; the CMake build commands above choose their own generator and toolset, including `-T ClangCL` for ARM64. CI runs both test commands on Windows x64 and Windows ARM64.

### Consuming the Windows package and `/MT` CRT rules

The install prefix contains `lib/lance_c.dll`, its import library `lib/lance_c.dll.lib`, the static library `lib/lance_c.lib`, and the public headers.

- **Static linkage (default, `LanceC::lance_c` → `LanceC::lance_c_static`):** The release static archive is a Release `/MT` library, and `LanceCConfig.cmake` sets `LanceC_STATIC_CRT ON`. Because CMake imported libraries cannot propagate `CMAKE_MSVC_RUNTIME_LIBRARY` to consuming targets, set the consumer's runtime library explicitly:
  ```cmake
  find_package(LanceC CONFIG REQUIRED)
  if(WIN32 AND LanceC_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
  endif()
  ```
  Do not link a `/MD` or debug `/MTd` consumer against the Release `/MT` static archive.
- **Shared linkage (`-DLanceC_LINK=shared`, `LanceC::lance_c` → `LanceC::lance_c_shared`):** Links through `lance_c.dll.lib`. Because `lance_c.dll` is statically linked against the CRT, it has no redistributable `VCRUNTIME*`, `MSVCP*`, `MSVCR*`, `CONCRT*`, or `VCOMP*` dependency. A shared consumer may use either CRT (`/MT` or `/MD`) and requires `lance_c.dll` beside its executable or on its DLL search path.

<a id="releasing"></a>
## Releasing

Pushing a `v*.*.*` tag runs [`release.yml`](../.github/workflows/release.yml), which builds five prebuilt `.tar.xz` archives (Linux `x86_64` and `aarch64` targeting `glibc 2.28`, macOS `arm64`, and Windows `x86_64` and `aarch64`), verifies exports/runtime dependencies/consumers, and attaches the archives and checksums to a GitHub Release. Beta tags (`v*-beta.*`) are published as GitHub pre-releases; stable tags publish a normal release. Generated notes are grouped by [`.github/release.yml`](../.github/release.yml).

<a id="pre-tag-documentation-check"></a>
### Pre-tag documentation check

Run this on `main` before starting the release workflow or tagging manually:

1. Scan the user-facing docs for release-specific versions and promises:
   ```bash
   grep -RInE 'v[0-9]+\.[0-9]+\.[0-9]+|0\.[0-9]+\.x|current release|latest release|next release|unreleased|will (adopt|publish|ship)|maintained release' README.md docs examples ports recipes .github/abi-baseline/README.md
   ```
   The scan also matches this checklist and historical compatibility facts; review each match rather than deleting it. Every remaining match must be a historical fact, a placeholder such as `vX.Y.Z`, or the exact release being cut. Remove or rewrite claims about the current stable release, the next release, or future core adoption.
2. Confirm release links: stable downloads use `/releases/latest`, history uses `/releases`, and `/releases/tag/<tag>` links are only for historical facts.
3. Compare the archive names and checksum conventions in the docs with `release.yml` and the planned release assets. If the supported platform matrix, core version, or ABI baseline changes, update the docs in the same release commit.
4. Confirm the README and guide do not name the release being cut as current and do not state what an unreleased version will contain.

<a id="recommended-cut-a-release-via-actions-ui"></a>
### Recommended: cut a release via Actions UI

[`create-release.yml`](../.github/workflows/create-release.yml) is a `workflow_dispatch` entry point that bumps `Cargo.toml`, commits, tags, pushes, and dispatches `release.yml`:

1. Open Actions → **Create Release** → **Run workflow** on `main`.
2. Choose:
   - **release_type**: `patch` / `minor` / `major`. Use `current` to keep the version in `Cargo.toml`: with `stable` after the release PR has set the baseline version, or with `preview` to cut another beta on the same base version.
   - **release_channel**: `preview` (tags `vX.Y.Z-beta.N`, auto-incremented) or `stable` (tags `vX.Y.Z`).
   - **dry_run**: leave on for the first run to preview the computed tag and version without pushing.
3. Re-run with **dry_run** off. When the version changes, the workflow updates `Cargo.toml`, `Cargo.lock`, and the public version macros in `include/lance/version.h`, commits as `github-actions[bot]`, pushes the commit and tag, and triggers `release.yml` at the new tag's ref. With `release_type: current`, it tags the current release commit without another version commit.
4. Run the post-publication check below before announcing the release.

> **Branch protection:** If `main` is protected, allow `github-actions[bot]` (or the GitHub Actions integration) to bypass push restrictions, or configure `create-release.yml` with a PAT that has `contents: write`.

<a id="manual-fallback"></a>
### Manual fallback

To cut a release manually without `create-release.yml`:

1. Choose the new semver version (for `0.x.y`, bump **minor** for breaking changes or new features, **patch** for bug fixes). In each block below, set `LANCE_C_VERSION` to that version without the `v` prefix, and let `TAG` name the release and its archives:
   ```bash
   LANCE_C_VERSION=X.Y.Z
   TAG="v$LANCE_C_VERSION"
   ```
2. On `main`, update `version = ...` in [`Cargo.toml`](../Cargo.toml), refresh `Cargo.lock`, and synchronize the public header macros:
   ```bash
   LANCE_C_VERSION=X.Y.Z
   TAG="v$LANCE_C_VERSION"
   git checkout main && git pull
   # edit Cargo.toml version to "$LANCE_C_VERSION"
   cargo update -p lance-c
   python3 .github/scripts/sync-version-header.py "$LANCE_C_VERSION"
   python3 .github/scripts/sync-version-header.py --check
   git checkout -b "chore/release-$LANCE_C_VERSION"
   git commit -am "chore(release): $TAG"
   ```
3. Merge the version-bump PR into `main`, then tag and push:
   ```bash
   LANCE_C_VERSION=X.Y.Z
   TAG="v$LANCE_C_VERSION"
   git checkout main && git pull
   git tag "$TAG"
   git push origin "$TAG"
   ```

You can also run a dry-run build of [`release.yml`](../.github/workflows/release.yml) via Actions → **Release** → **Run workflow** with a test version such as `0.0.1-dev` (the `publish` job is skipped unless the ref is `refs/tags/v*`).

<a id="post-publication-check"></a>
### Post-publication check

After `release.yml` publishes the GitHub Release:

1. Open the release page and compare every archive, `.sha256`, `.sha512`, and `SHA512SUMS` name with the docs table and quickstart. Fix any mismatch in a follow-up docs commit before announcing the release.
2. Download `SHA512SUMS` and at least one archive from the published release. Verify the checksum, unpack the archive, and run the README quickstart from the published tag, including `git clone --branch`.
3. Confirm `https://github.com/lance-format/lance-c/releases/latest` resolves to the intended stable release. A preview release must not become the latest stable download.
4. Re-run the stale-release scan from the pre-tag check. Fix any doc that names the wrong tag, calls a version current, or promises an unreleased version, then merge the docs fix and re-check its links.
5. Check every local link and release/download URL touched by the release. The examples and API guide must match the installed archive's headers.
