# Building, Packaging, and Releasing

This document provides platform-specific build instructions, binary packaging contracts, vcpkg and Conan consumer walkthroughs, and release procedures for `lance-c`. For the top-level overview and quickstart, see [`README.md`](../README.md).

## Contents

- [Published prebuilt archives](#published-prebuilt-archives)
- [Linux baseline for 0.2 and future packages](#linux-baseline-for-future-packages)
- [Preview quickstart (`v0.2.0-beta.1`)](#preview-quickstart-v020-beta1)
- [Building from source via Cargo](#from-source-via-cargo-rust-developers)
- [Building from source via CMake](#from-source-via-cmake-cc-developers-and-packagers)
- [Windows (`x86_64-pc-windows-msvc` and `aarch64-pc-windows-msvc`)](#windows-x86_64-pc-windows-msvc-and-aarch64-pc-windows-msvc)
- [vcpkg overlay port (`v0.1.9`)](#vcpkg-overlay-port-v019)
- [Conan local recipe (`v0.1.9`)](#conan-local-recipe-v019)
- [Releasing](#releasing)

---

## Published prebuilt archives

Each release `.tar.xz` archive unpacks an install prefix containing:

- `include/lance/lance.h` and `include/lance/lance.hpp`
- Static and shared libraries under `lib/` (plus `lib/lance_c.dll.lib` on Windows)
- `lib/cmake/LanceC/` (`LanceCConfig.cmake` and `LanceCConfigVersion.cmake`)
- `lib/pkgconfig/lance-c.pc`
- `share/licenses/lance-c/LICENSE` (and `share/doc/lance-c/README.md` in Windows archives)

In the preview, Linux and macOS release shared libraries (`liblance_c.so`, `liblance_c.dylib`) are stripped of debug/local symbols and restrict their defined dynamic exports to the public `lance_*` C ABI. Windows requires both `lance_c.dll` and `lance_c.dll.lib` to expose only the header-declared `lance_*` symbols. Static archives (`liblance_c.a`, `lance_c.lib`) are unstripped so they retain the symbol table required for static linking.

### Preview release (`v0.2.0-beta.1`)

[`v0.2.0-beta.1`](https://github.com/lance-format/lance-c/releases/tag/v0.2.0-beta.1) is a GitHub pre-release (so GitHub's *Latest* release badge still resolves to `v0.1.9`). It publishes five prebuilt archives, per-archive `.sha256` and `.sha512` files, and a five-line `SHA512SUMS`:

| Platform | Archive | Runtime & ABI contract |
| --- | --- | --- |
| Linux `x86_64` | `lance-c-v0.2.0-beta.1-x86_64-unknown-linux-gnu.tar.xz` | `glibc >= 2.28`; links only `libgcc_s.so.1`, `libm.so.6`, `libc.so.6`, `ld-linux-x86-64.so.2` (`pthread`/`dl` on older glibc); C++20 for `lance.hpp` |
| Linux `aarch64` | `lance-c-v0.2.0-beta.1-aarch64-unknown-linux-gnu.tar.xz` | `glibc >= 2.28`; links only `libgcc_s.so.1`, `libm.so.6`, `libc.so.6`; C++20 for `lance.hpp` |
| macOS `arm64` | `lance-c-v0.2.0-beta.1-aarch64-apple-darwin.tar.xz` | Apple silicon (`minos 11.0`); C++20 for `lance.hpp` |
| Windows `x86_64` | `lance-c-v0.2.0-beta.1-x86_64-pc-windows-msvc.tar.xz` | Windows x64, MSVC static CRT (`/MT`, `LanceC_STATIC_CRT=ON`); C++20 for `lance.hpp` |
| Windows `ARM64` | `lance-c-v0.2.0-beta.1-aarch64-pc-windows-msvc.tar.xz` | Windows 11 ARM64, MSVC static CRT (`/MT`, `LanceC_STATIC_CRT=ON`); C++20 for `lance.hpp` |

### Stable release (`v0.1.9`)

[`v0.1.9`](https://github.com/lance-format/lance-c/releases/tag/v0.1.9) is the current stable release. It predates the Windows release rows and the generic `glibc 2.28` Linux build container, and publishes five archives plus `SHA512SUMS`:

| Archive | Highest symbol / target | Selected by (`v0.1.9` recipes) |
| --- | --- | --- |
| `lance-c-v0.1.9-x86_64-unknown-linux-gnu-debian12.tar.xz` | `GLIBC_2.35` | vcpkg `x64-linux`, `x64-linux-dynamic`; Conan `Linux/x86_64` |
| `lance-c-v0.1.9-x86_64-unknown-linux-gnu.tar.xz` | `GLIBC_2.39` | Not selected by current recipes |
| `lance-c-v0.1.9-aarch64-unknown-linux-gnu-debian12.tar.xz` | `GLIBC_2.35` | Not selected by current recipes |
| `lance-c-v0.1.9-aarch64-unknown-linux-gnu.tar.xz` | `GLIBC_2.39` | vcpkg `arm64-linux`, `arm64-linux-dynamic`; Conan `Linux/armv8` |
| `lance-c-v0.1.9-aarch64-apple-darwin.tar.xz` | Apple silicon; dylib `minos 11.0` (416 of 3,298 static archive members declare `14.5`) | vcpkg `arm64-osx`, `arm64-osx-dynamic`; Conan `Macos/armv8` |

None of the Linux shared libraries records `GLIBCXX`, `CXXABI`, `libssl`, or `libcrypto` dynamic dependencies. The `v0.1.9` C++ header (`lance/lance.hpp`) requires C++17.

<a id="linux-baseline-for-future-packages"></a>
## Linux baseline for 0.2 and future packages

Starting with the `0.2` release line (`v0.2.0-beta.1` and subsequent tags), the release workflow builds one generic Linux archive per architecture (`x86_64-unknown-linux-gnu` and `aarch64-unknown-linux-gnu`) targeting **glibc 2.28** (`manylinux_2_28`) and does not publish separate `-debian12` variants:

- The `v0.1.9` archives (including the `-debian12` assets referencing `GLIBC_2.35` and standard assets referencing `GLIBC_2.39`) remain immutable at their published URLs and hashes.
- The `ports/lance-c` vcpkg overlay port and `recipes/lance-c` Conan recipe currently select the `v0.1.9` assets.
- Packages keep the `lib/` install layout (with no `lib64` or compatibility symlinks) and propagate the `cxx_std_20` compile feature in `0.2+`.
- Source builds link against the build host's libc and are unaffected by the prebuilt container baseline.

<a id="preview-quickstart-v020-beta1"></a>
## Preview quickstart (`v0.2.0-beta.1`)

To verify the `v0.2.0-beta.1` pre-release from a clean directory on Linux `x86_64` (requires CMake 3.22+ and a C++20 compiler, no Rust toolchain):

```bash
mkdir lance-c-preview && cd lance-c-preview

TAG=v0.2.0-beta.1
VERSION=0.2.0-beta.1
ARCHIVE=lance-c-v0.2.0-beta.1-x86_64-unknown-linux-gnu.tar.xz

curl -fLO "https://github.com/lance-format/lance-c/releases/download/$TAG/$ARCHIVE"
curl -fLO "https://github.com/lance-format/lance-c/releases/download/$TAG/SHA512SUMS"
sha512sum --ignore-missing -c SHA512SUMS
mkdir install
tar -xJf "$ARCHIVE" -C install

curl -fL -o "lance-c-$TAG-source.tar.gz" \
  "https://github.com/lance-format/lance-c/archive/refs/tags/$TAG.tar.gz"
tar -xzf "lance-c-$TAG-source.tar.gz"

cmake -S "lance-c-$VERSION/examples/cmake-consumer" -B build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/install"
cmake --build build --target consumer
./build/consumer "lance-c-$VERSION/test_data/v0.27.1/pq_in_schema"
```

Expected output:

```text
rows: 512, version: 2
```

For shared linkage, configure a separate build directory and pass `-DLanceC_LINK=shared`. On macOS `arm64`, set `ARCHIVE=lance-c-v0.2.0-beta.1-aarch64-apple-darwin.tar.xz` and verify the checksum with `grep "  $ARCHIVE$" SHA512SUMS | shasum -a 512 -c -`.

<a id="from-source-via-cargo-rust-developers"></a>
## Building from source via Cargo

Source builds of the current tree require **Rust 1.95.0+**, **CMake 3.22+**, and **`protoc`** (the `v0.1.9` tag declares `rust-version = "1.91.0"`). Baseline build targets are defined in [`.github/baseline.env`](../.github/baseline.env):

| Component | Target / minimum |
| --- | --- |
| Rust | 1.95.0+ (`0.2` line / `main`); 1.91.0+ (`v0.1.9`) |
| CMake | 3.22+ |
| C / C++ | C11; C++20 (`0.2` line / `main`) or C++17 (`v0.1.9`) |
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

<a id="windows-x86_64-pc-windows-msvc-and-aarch64-pc-windows-msvc"></a>
## Windows (`x86_64-pc-windows-msvc` and `aarch64-pc-windows-msvc`)

Windows prebuilt archives are published starting with [`v0.2.0-beta.1`](https://github.com/lance-format/lance-c/releases/tag/v0.2.0-beta.1) (`v0.1.9` published no Windows archives, and the vcpkg/Conan recipes do not yet map Windows archives).

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

<a id="package-managers-v019"></a>
<a id="vcpkg-overlay-port-v019"></a>
<a id="vcpkg"></a>
## vcpkg overlay port (`v0.1.9`)

`lance-c` is not in the public vcpkg registry yet. Use the overlay port in [`ports/lance-c`](../ports/README.md) from a current `main` checkout (do not use the copy inside the `v0.1.9` source tarball, which predates the published release hashes):

```bash
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
git clone https://github.com/lance-format/lance-c.git

# x64-linux installs static; x64-linux-dynamic installs shared.
TRIPLET=x64-linux
./vcpkg/vcpkg install lance-c \
  --overlay-ports="$PWD/lance-c/ports" --triplet "$TRIPLET"
```

Supported prebuilt triplets for `0.1.9`:

- `x64-linux` (static) and `x64-linux-dynamic` (shared) — uses the Debian 12 `x86_64` archive (`glibc >= 2.35`)
- `arm64-linux` (static) and `arm64-linux-dynamic` (shared) — uses the standard `aarch64` archive (`glibc >= 2.39`)
- `arm64-osx` (static) and `arm64-osx-dynamic` (shared) — Apple silicon macOS

Intel macOS (`x64-osx`) is rejected in both prebuilt and `from-source` modes; Windows is not supported by the `0.1.9` port. Other Linux triples can build from source via `vcpkg install 'lance-c[from-source]'` (requires Rust, CMake, and `protoc`).

Configure a minimal C++17 consumer against the vcpkg installation:

```bash
mkdir consumer && cd consumer
curl -fLO https://raw.githubusercontent.com/lance-format/lance-c/v0.1.9/examples/cmake-consumer/main.cpp

cat > CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.22)
project(consumer LANGUAGES CXX)
find_package(LanceC CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_compile_features(consumer PRIVATE cxx_std_17)
target_link_libraries(consumer PRIVATE LanceC::lance_c)
EOF

TRIPLET=${TRIPLET:-x64-linux}
VCPKG_ROOT="$(cd ../vcpkg && pwd)"
cmake -S . -B "build-$TRIPLET" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET="$TRIPLET"
cmake --build "build-$TRIPLET"
./"build-$TRIPLET"/consumer /path/to/dataset.lance
```

<a id="conan-local-recipe-v019"></a>
<a id="conan"></a>
## Conan local recipe (`v0.1.9`)

ConanCenter does not yet provide `lance-c/0.1.9`. Create the local package from [`recipes/lance-c/all`](../recipes/README.md):

```bash
python3 -m venv .conan-venv
. .conan-venv/bin/activate
python -m pip install "conan>=2,<3"
conan profile detect
git clone https://github.com/lance-format/lance-c.git
conan create lance-c/recipes/lance-c/all --version=0.1.9
```

`conan create` downloads the `v0.1.9` prebuilt archive for Linux `x86_64`/`armv8` or macOS `armv8` and runs the recipe's C and C++ test package. It creates the static package by default; add `-o "&:shared=True"` for shared linkage, or `-o "lance-c/*:from_source=True"` to build from source (requires Rust, CMake, and `protoc`). Intel macOS (`Macos`/`x86_64`) is rejected.

To consume `lance-c/0.1.9` in a project:

```bash
mkdir consumer && cd consumer
curl -fLO https://raw.githubusercontent.com/lance-format/lance-c/v0.1.9/examples/cmake-consumer/main.cpp
```

Create `conanfile.txt`:

```ini
[requires]
lance-c/0.1.9

[generators]
CMakeDeps
CMakeToolchain
```

Create `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.22)
project(consumer LANGUAGES CXX)
find_package(LanceC CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_compile_features(consumer PRIVATE cxx_std_17)
target_link_libraries(consumer PRIVATE LanceC::lance_c)
```

Install and build (add `-o "lance-c/*:shared=True"` to `conan install` for shared linkage):

```bash
conan install . --output-folder=build --build=missing
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

<a id="releasing"></a>
## Releasing

Pushing a `v*.*.*` tag runs [`release.yml`](../.github/workflows/release.yml), which builds five prebuilt `.tar.xz` archives (Linux `x86_64` and `aarch64` targeting `glibc 2.28`, macOS `arm64`, and Windows `x86_64` and `aarch64`), verifies exports/runtime dependencies/consumers, and attaches the archives and checksums to a GitHub Release. Beta tags (`v*-beta.*`) are published as GitHub pre-releases.

<a id="recommended-cut-a-release-via-actions-ui"></a>
### Recommended: cut a release via Actions UI

[`create-release.yml`](../.github/workflows/create-release.yml) is a `workflow_dispatch` entry point that bumps `Cargo.toml`, commits, tags, pushes, and dispatches `release.yml`:

1. Open Actions → **Create Release** → **Run workflow** on `main`.
2. Choose:
   - **release_type**: `patch` / `minor` / `major` (or `current` to cut another beta on the same base version, e.g. `v0.2.0-beta.2` after `v0.2.0-beta.1`).
   - **release_channel**: `preview` (tags `vX.Y.Z-beta.N`, auto-incremented) or `stable` (tags `vX.Y.Z`).
   - **dry_run**: leave on for the first run to preview the computed tag and version without pushing.
3. Re-run with **dry_run** off. The workflow updates `Cargo.toml` and `Cargo.lock`, commits as `github-actions[bot]`, pushes the commit and tag, and triggers `release.yml` at the new tag's ref.
4. Once `release.yml` finishes, copy the `publish` job's emitted hash snippets into:
   - [`ports/lance-c/portfile.cmake`](../ports/lance-c/portfile.cmake) (SHA512s)
   - [`recipes/lance-c/all/conandata.yml`](../recipes/lance-c/all/conandata.yml) (SHA256s from the release `.sha256` assets)
5. Open follow-up PRs to `microsoft/vcpkg` and `conan-io/conan-center-index` mirroring the updated `ports/` and `recipes/` directories.

> **Branch protection:** If `main` is protected, allow `github-actions[bot]` (or the GitHub Actions integration) to bypass push restrictions, or configure `create-release.yml` with a PAT that has `contents: write`.

<a id="manual-fallback"></a>
### Manual fallback

To cut a release manually without `create-release.yml`:

1. Choose the new semver version (for `0.x.y`, bump **minor** for breaking changes or new features, **patch** for bug fixes).
2. On `main`, update `version = ...` in [`Cargo.toml`](../Cargo.toml) and refresh `Cargo.lock`:
   ```bash
   git checkout main && git pull
   # edit Cargo.toml version
   cargo update -p lance-c
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
