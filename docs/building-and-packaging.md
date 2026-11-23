# Building, Packaging, and Releasing

This document provides platform-specific build instructions, binary packaging contracts, and release procedures for `lance-c`. For the top-level overview and quickstart, see [`README.md`](../README.md).

## Contents

- [Published prebuilt archives](#published-prebuilt-archives)
- [Linux runtime requirements](#linux-baseline-for-future-packages)
- [Try the published package](#try-the-published-package)
- [Building from source via Cargo](#from-source-via-cargo-rust-developers)
- [Building from source via CMake](#from-source-via-cmake-cc-developers-and-packagers)
- [Windows (`x86_64-pc-windows-msvc` and `aarch64-pc-windows-msvc`)](#windows-x86_64-pc-windows-msvc-and-aarch64-pc-windows-msvc)
- [Releasing](#releasing)

---

## Published prebuilt archives

Each release `.tar.xz` archive unpacks an install prefix containing:

- `include/lance/lance.h` (umbrella), `include/lance/lance.hpp`, and the domain headers under `include/lance/`
- Static and shared libraries under `lib/` (plus `lib/lance_c.dll.lib` on Windows)
- `lib/cmake/LanceC/` (`LanceCConfig.cmake` and `LanceCConfigVersion.cmake`)
- `lib/pkgconfig/lance-c.pc`
- `share/licenses/lance-c/LICENSE` (and `share/doc/lance-c/README.md` in Windows archives)

In the published archives, Linux and macOS release shared libraries (`liblance_c.so`, `liblance_c.dylib`) are stripped of debug/local symbols and restrict their defined dynamic exports to the public `lance_*` C ABI. Windows requires both `lance_c.dll` and `lance_c.dll.lib` to expose only the header-declared `lance_*` symbols. Static archives (`liblance_c.a`, `lance_c.lib`) are unstripped so they retain the symbol table required for static linking.

### Stable release (`v0.2.0`)

[`v0.2.0`](https://github.com/lance-format/lance-c/releases/tag/v0.2.0) is the first stable release of the 0.2.x line. It pins Lance 15.0.0-beta.25 (`f5dbd55d`); a later 0.2.x patch will adopt upstream Lance 15.0.0 while preserving the C ABI. It publishes five prebuilt archives, per-archive `.sha256` and `.sha512` files, and a five-line `SHA512SUMS`:

| Platform | Archive | Runtime & ABI contract |
| --- | --- | --- |
| Linux `x86_64` | `lance-c-v0.2.0-x86_64-unknown-linux-gnu.tar.xz` | `glibc >= 2.28`; links only `libgcc_s.so.1`, `libm.so.6`, `libc.so.6`, `ld-linux-x86-64.so.2` (`pthread`/`dl` on older glibc); C++20 for `lance.hpp` |
| Linux `aarch64` | `lance-c-v0.2.0-aarch64-unknown-linux-gnu.tar.xz` | `glibc >= 2.28`; links only `libgcc_s.so.1`, `libm.so.6`, `libc.so.6`; C++20 for `lance.hpp` |
| macOS `arm64` | `lance-c-v0.2.0-aarch64-apple-darwin.tar.xz` | Apple silicon (`minos 11.0`); C++20 for `lance.hpp` |
| Windows `x86_64` | `lance-c-v0.2.0-x86_64-pc-windows-msvc.tar.xz` | Windows x64, MSVC static CRT (`/MT`, `LanceC_STATIC_CRT=ON`); C++20 for `lance.hpp` |
| Windows `ARM64` | `lance-c-v0.2.0-aarch64-pc-windows-msvc.tar.xz` | Windows 11 ARM64, MSVC static CRT (`/MT`, `LanceC_STATIC_CRT=ON`); C++20 for `lance.hpp` |

The 0.2.x prebuilt baseline is these five targets: Linux x86_64 and AArch64 with glibc 2.28+, Apple silicon macOS, and Windows x64 and ARM64 with the static `/MT` CRT. C consumers need C11; the C++ wrapper needs C++20.

<a id="linux-baseline-for-future-packages"></a>

## Linux runtime requirements

The two Linux archives target glibc 2.28 on x86_64 and AArch64. Source builds link against the build host’s libc.

<a id="try-the-published-package"></a>

## Try the published package

Follow the [quickstart](../README.md#quickstart) to verify the archive and open the sample dataset. For shared linkage, pass `-DLanceC_LINK=shared` to CMake. On macOS arm64, choose the Apple silicon archive and verify its per-archive checksum with `shasum -a 512 -c "$ARCHIVE.sha512"` after downloading both files.

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

Windows prebuilt archives are published in the [v0.2.0 release](https://github.com/lance-format/lance-c/releases/tag/v0.2.0) and later 0.2.x releases.

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

<a id="recommended-cut-a-release-via-actions-ui"></a>
### Recommended: cut a release via Actions UI

[`create-release.yml`](../.github/workflows/create-release.yml) is a `workflow_dispatch` entry point that bumps `Cargo.toml`, commits, tags, pushes, and dispatches `release.yml`:

1. Open Actions → **Create Release** → **Run workflow** on `main`.
2. Choose:
   - **release_type**: `patch` / `minor` / `major`. Use `current` to keep the version in `Cargo.toml`: with `stable` after the release PR has set the baseline version, or with `preview` to cut another beta on the same base version.
   - **release_channel**: `preview` (tags `vX.Y.Z-beta.N`, auto-incremented) or `stable` (tags `vX.Y.Z`).
   - **dry_run**: leave on for the first run to preview the computed tag and version without pushing.
3. Re-run with **dry_run** off. When the version changes, the workflow updates `Cargo.toml`, `Cargo.lock`, and the public version macros in `include/lance/version.h`, commits as `github-actions[bot]`, pushes the commit and tag, and triggers `release.yml` at the new tag's ref. With `release_type: current`, it tags the current release commit without another version commit.
4. Verify that the release page contains all expected platform archives and checksums. Download an archive and run the consumer from the same tag.

> **Branch protection:** If `main` is protected, allow `github-actions[bot]` (or the GitHub Actions integration) to bypass push restrictions, or configure `create-release.yml` with a PAT that has `contents: write`.

<a id="manual-fallback"></a>
### Manual fallback

To cut a release manually without `create-release.yml`:

1. Choose the new semver version (for `0.x.y`, bump **minor** for breaking changes or new features, **patch** for bug fixes).
2. On `main`, update `version = ...` in [`Cargo.toml`](../Cargo.toml), refresh `Cargo.lock`, and synchronize the public header macros:
   ```bash
   git checkout main && git pull
   # edit Cargo.toml version
   cargo update -p lance-c
   python3 .github/scripts/sync-version-header.py 0.2.0
   python3 .github/scripts/sync-version-header.py --check
   git checkout -b chore/release-0.2.0
   git commit -am "chore(release): v0.2.0"
   ```
3. Merge the version-bump PR into `main`, then tag and push:
   ```bash
   git checkout main && git pull
   git tag v0.2.0
   git push origin v0.2.0
   ```

You can also run a dry-run build of [`release.yml`](../.github/workflows/release.yml) via Actions → **Release** → **Run workflow** with a test version such as `0.0.1-dev` (the `publish` job is skipped unless the ref is `refs/tags/v*`).
