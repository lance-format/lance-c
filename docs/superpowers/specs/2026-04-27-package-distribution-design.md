# Phase 4: Package Distribution — Design

**Status:** Approved
**Date:** 2026-04-27
**Scope:** README "Phase 4" — `Package distribution | vcpkg and Conan recipe packaging`. Closes the last open-source-distribution gap so C++ consumers (Velox, DuckDB, Iceberg-cpp, etc.) can depend on `lance-c` through standard package managers instead of building from a git checkout.

## 1. Goal

Make `lance-c` consumable from a standard CMake project as:

```cmake
find_package(LanceC 0.1 REQUIRED)
target_link_libraries(myapp PRIVATE LanceC::lance_c)
```

with the underlying library obtained through any of:
- `vcpkg install lance-c` (upstream microsoft/vcpkg)
- `conan install --requires=lance-c/0.1.0` (upstream conan-center-index)
- A `cmake --install .` from a manual build of this repo
- A downloaded release tarball, untarred into `CMAKE_PREFIX_PATH`

Every path produces the same install layout, the same `LanceC::lance_c` imported target, and the same transitive platform-deps story. C++ consumers never have to redeclare `pthread`/`dl`/`m`/`-framework Security` themselves.

## 2. Non-goals

- **Windows.** Out of scope for v0; lance-c CI does not run Windows today. Recipes declare `supports: !windows` (vcpkg) and `os in [Linux, Macos]` (Conan).
- **musl Linux / Alpine.** Out of scope for v0.
- **Code-signing / notarization of macOS dylibs.** lance-c is a developer dependency, not a notarized end-user binary.
- **A Velox-side PR.** Velox PR #16556 lands as-is with its hand-rolled `lance_ffi.h` and manual `LANCE_FFI_LIB_PATH`. A future PR can adopt `find_package(LanceC)`; we do not block on it.
- **Re-engineering `cargo build` for non-CMake users.** `cargo build --release` keeps producing the same `liblance_c.{a,so,dylib}` it does today. The CMake superbuild is opt-in for C++ consumers.
- **Compaction / fragment cleanup / write-path expansion.** Those are separate Phase 4 line items handled in their own spec.

## 3. Architecture

Five layered components, each independently testable. The CMake superbuild is the single source of truth; every other distribution path delegates to it.

```
┌──────────────────────────────────────────────────────────────┐
│  Consumer (Velox, DuckDB, ...) — find_package(LanceC)        │
│                                  → LanceC::lance_c           │
└──────────────────────────────────────────────────────────────┘
                              ▲
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
┌───────────────┐    ┌─────────────────┐   ┌───────────────────┐
│ vcpkg port    │    │ Conan recipe    │   │ Tarball + sha256  │
│ ports/lance-c │    │ recipes/lance-c │   │ on GH Releases    │
└───────────────┘    └─────────────────┘   └───────────────────┘
        │                     │                     │
        └─────────────────────┼─────────────────────┘
                              ▼
            ┌──────────────────────────────────┐
            │  CMake superbuild (cmake/)       │
            │  - Corrosion bridges to cargo    │
            │  - install(EXPORT ...) rules     │
            │  - LanceCConfig.cmake generator  │
            │  - lance-c.pc generator          │
            └──────────────────────────────────┘
                              ▲
            ┌──────────────────────────────────┐
            │  Cargo crate (existing)          │
            │  src/, include/lance/{lance.h,   │
            │  lance.hpp}                      │
            └──────────────────────────────────┘
```

This layering matches the convention validated by Slint (`api/cpp/`) and wasmtime (`crates/c-api/`); it diverges from the cargo-c-only path used by rustls-ffi because Velox-class consumers want `find_package` plus a namespaced imported target, which cargo-c does not generate.

## 4. CMake superbuild

A new top-level `CMakeLists.txt` (lance-c has none today) plus a `cmake/` helper directory. The CMake build is **opt-in** — `cargo build` keeps working unchanged for Rust-first users.

### 4.1 New files

```
CMakeLists.txt                    # top-level superbuild
cmake/
  LanceCConfig.cmake.in           # template → installed config
  lance-c.pc.in                   # template → installed pkg-config
  Corrosion.cmake                 # vendored Corrosion via FetchContent
  prebuilt-fetch.cmake            # used by recipes when downloading binaries
```

### 4.2 What `CMakeLists.txt` does

1. **Detect mode.** `LANCE_C_USE_PREBUILT=ON` → skip cargo, expect headers/libs already present at `LANCE_C_PREBUILT_DIR`. Default OFF → invoke cargo.
2. **Bridge to cargo via [Corrosion](https://github.com/corrosion-rs/corrosion).** A `FetchContent_Declare(Corrosion ...)` pulls Corrosion at configure time; `corrosion_import_crate(MANIFEST_PATH Cargo.toml CRATES lance-c CRATE_TYPES staticlib cdylib)` produces the `lance-c-static` and `lance-c-shared` CMake targets. Cargo profile is synced to `CMAKE_BUILD_TYPE` (Release ↔ `--release`). This replaces the hand-rolled `add_custom_command(cargo build ...)` pattern used by wasmtime — Corrosion handles target-triple mapping, rerun-on-change tracking, and `RUSTFLAGS` propagation correctly.
3. **Define imported targets** `LanceC::lance_c_static` and `LanceC::lance_c_shared`, each with `INTERFACE_LINK_LIBRARIES` already populated with the platform deps reqwest/native-tls/object_store pull in:
   - macOS: `"-framework CoreFoundation" "-framework Security" "-framework SystemConfiguration"`
   - Linux: `pthread dl m`
   - Unversioned alias `LanceC::lance_c` resolves via `LANCE_C_LINK=static|shared` (default static — matches Velox PR #16556).
4. **Install rules.**
   ```
   ${CMAKE_INSTALL_INCLUDEDIR}/lance/lance.h
   ${CMAKE_INSTALL_INCLUDEDIR}/lance/lance.hpp
   ${CMAKE_INSTALL_LIBDIR}/liblance_c.a
   ${CMAKE_INSTALL_LIBDIR}/liblance_c.{so,dylib}        # if shared
   ${CMAKE_INSTALL_LIBDIR}/cmake/LanceC/LanceCConfig.cmake
   ${CMAKE_INSTALL_LIBDIR}/cmake/LanceC/LanceCConfigVersion.cmake
   ${CMAKE_INSTALL_LIBDIR}/cmake/LanceC/LanceCTargets.cmake
   ${CMAKE_INSTALL_LIBDIR}/pkgconfig/lance-c.pc
   ${CMAKE_INSTALL_DATADIR}/licenses/lance-c/LICENSE
   ```

### 4.3 Header path migration

Move `include/lance.h` → `include/lance/lance.h` and `include/lance.hpp` → `include/lance/lance.hpp`. Update `lance.hpp`'s internal `#include "lance.h"` to `#include "lance/lance.h"`. Update the existing test sources `tests/cpp/test_c_api.c` and `tests/cpp/test_cpp_api.cpp` to use `#include "lance/lance.h"` / `#include "lance/lance.hpp"`. The `compile_c_test`/`compile_cpp_test` helpers in `tests/compile_and_run_test.rs` need no change — they still pass `-I${manifest_dir}/include` to clang.

This is a **breaking include change** for any external consumer that already vendors `lance.h` from a git checkout. Acceptable at 0.1.0; called out in the release notes. Convention is namespaced subdir (`<lance/lance.h>`) — matches librsvg (`<librsvg/rsvg.h>`), the most widely distro-packaged project in the survey.

### 4.4 cargo-c as a secondary path

Document `cargo cinstall --release --prefix=/usr/local` as an alternative install entry point for distro packagers who prefer the cargo-native flow. Requires adding `[package.metadata.capi]` to `Cargo.toml`:

```toml
[package.metadata.capi.header]
subdirectory = "lance"

[package.metadata.capi.pkg_config]
name = "lance-c"

[package.metadata.capi.library]
name = "lance_c"
```

cargo-c emits the same `liblance_c.{a,so,dylib}`, `lance-c.pc`, and `<prefix>/include/lance/lance.h` layout the CMake superbuild does, but **does not** emit `LanceCConfig.cmake`. CMake-using consumers should prefer the CMake superbuild. This dual-path setup mirrors what librsvg does (meson primary, cargo-c partial).

### 4.5 Deliberately not doing

- No CMake-side regeneration of `include/lance/lance.h` from cbindgen — that stays a manual `cargo install cbindgen && cbindgen ...` step (matches `build.rs` comment). The release CI runs cbindgen and diffs to catch drift.
- No CMake-driven test runner. `cargo test` and `cargo test --test compile_and_run_test -- --ignored` keep being the test entry points; the CMake build is for *consumers*, not for our test loop.

## 5. vcpkg port

Lives at `ports/lance-c/` in the lance-c repo (the upstream-PR copy is identical).

```
ports/lance-c/
  vcpkg.json       # manifest
  portfile.cmake   # install logic
  usage            # post-install message shown to consumers
```

`vcpkg.json`:
```json
{
  "name": "lance-c",
  "version": "0.1.0",
  "description": "C/C++ bindings for the Lance columnar data format",
  "homepage": "https://github.com/lance-format/lance-c",
  "license": "Apache-2.0",
  "supports": "(linux | osx) & (x64 | arm64)",
  "features": {
    "from-source": {
      "description": "Build from source via cargo (requires Rust toolchain on host)"
    }
  }
}
```

`portfile.cmake` shape:
```cmake
if("from-source" IN_LIST FEATURES)
    # Fallback: vcpkg_from_github + vcpkg_execute_required_process(cargo build ...)
    # Used only when explicitly requested by the consumer.
else()
    # Default: download our prebuilt tarball for the matching triple.
    set(_TRIPLE_MAP_x64-linux         "x86_64-unknown-linux-gnu")
    set(_TRIPLE_MAP_arm64-linux       "aarch64-unknown-linux-gnu")
    set(_TRIPLE_MAP_x64-osx           "x86_64-apple-darwin")
    set(_TRIPLE_MAP_arm64-osx         "aarch64-apple-darwin")
    # … plus -dynamic variants for shared linkage …
    set(_TRIPLE "${_TRIPLE_MAP_${VCPKG_TARGET_TRIPLET}}")

    vcpkg_download_distfile(ARCHIVE
        URLS "https://github.com/lance-format/lance-c/releases/download/v${VERSION}/lance-c-v${VERSION}-${_TRIPLE}.tar.xz"
        FILENAME "lance-c-v${VERSION}-${_TRIPLE}.tar.xz"
        SHA512 "${LANCE_C_SHA512_${VCPKG_TARGET_TRIPLET}}")
    vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}")

    file(INSTALL "${SOURCE_PATH}/include/" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
    file(INSTALL "${SOURCE_PATH}/lib/"     DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
    file(INSTALL "${SOURCE_PATH}/share/"   DESTINATION "${CURRENT_PACKAGES_DIR}/share")
endif()

vcpkg_cmake_config_fixup(PACKAGE_NAME LanceC CONFIG_PATH lib/cmake/LanceC)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"   DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(INSTALL "${SOURCE_PATH}/share/licenses/lance-c/LICENSE"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
```

Per-triple SHA512s are written into the portfile by the release script. Static vs shared linkage is delegated to vcpkg's existing triplet system (`x64-linux` static, `x64-linux-dynamic` shared) — the release tarballs contain both `liblance_c.a` and `liblance_c.so`, and `vcpkg_cmake_config_fixup` picks based on `VCPKG_LIBRARY_LINKAGE`. No custom `lance-c[shared]` feature is needed.

**Sidesteps the [vcpkg#33824](https://github.com/microsoft/vcpkg/issues/33824) Rust-toolchain-in-vcpkg controversy** because the default path downloads a tarball, never invoking cargo. This is what unblocks upstream submission.

## 6. Conan recipe

Lives at `recipes/lance-c/all/` for Conan Center submission, with the canonical copy in this repo.

```
recipes/lance-c/all/
  conanfile.py
  conandata.yml          # per-version SHA256s and source URLs
recipes/lance-c/config.yml   # version → folder mapping (Conan Center convention)
```

`conanfile.py` skeleton:
```python
from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.files import get, copy

class LanceCConan(ConanFile):
    name = "lance-c"
    settings = "os", "arch", "compiler", "build_type"
    options = {"shared": [True, False], "from_source": [True, False]}
    default_options = {"shared": False, "from_source": False}

    def validate(self):
        if self.settings.os not in ("Linux", "Macos"):
            raise ConanInvalidConfiguration("lance-c supports Linux and macOS only in 0.x")
        if self.settings.arch not in ("x86_64", "armv8"):
            raise ConanInvalidConfiguration("lance-c supports x86_64 and armv8 only in 0.x")

    def build(self):
        if self.options.from_source:
            self.run("cargo build --release ...")  # requires rustup on host
        else:
            triple = _triple_for(self.settings, self.options.shared)
            get(self, **self.conan_data["sources"][self.version][triple])

    def package(self):
        copy(self, "*.h",   src=..., dst=os.path.join(self.package_folder, "include", "lance"))
        copy(self, "*.hpp", src=..., dst=os.path.join(self.package_folder, "include", "lance"))
        copy(self, "*.a",   src=..., dst=os.path.join(self.package_folder, "lib"))
        if self.options.shared:
            copy(self, "*.so*",   src=..., dst=os.path.join(self.package_folder, "lib"))
            copy(self, "*.dylib", src=..., dst=os.path.join(self.package_folder, "lib"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "LanceC")
        self.cpp_info.set_property("cmake_target_name", "LanceC::lance_c")
        self.cpp_info.libs = ["lance_c"]
        if self.settings.os == "Macos":
            self.cpp_info.frameworks = ["CoreFoundation", "Security", "SystemConfiguration"]
        elif self.settings.os == "Linux":
            self.cpp_info.system_libs = ["pthread", "dl", "m"]
```

The `package_info` block is what makes `LanceC::lance_c` propagate its transitive link requirements through Conan, mirroring the CMake imported target.

## 7. Release CI + prebuilt binary build matrix

A new `.github/workflows/release.yml` triggered on `v*` tag pushes. Existing `ci.yml` (format/clippy/test/rustdoc/msrv/license-headers) keeps running on every PR, unchanged.

### 7.1 Build matrix

`Cargo.toml` already declares `crate-type = ["cdylib", "staticlib", "rlib"]`, so a single `cargo build --release` per triple produces both `liblance_c.a` and `liblance_c.{so,dylib}` in one shot. The release matrix therefore has **4 jobs (one per triple)**, each producing a single tarball that contains both linkages:

| Triple | Runner |
|---|---|
| `x86_64-unknown-linux-gnu` | `ubuntu-24.04` |
| `aarch64-unknown-linux-gnu` | `ubuntu-24.04-arm` (native arm64 runner) |
| `x86_64-apple-darwin` | `macos-13` (last x86_64 runner) |
| `aarch64-apple-darwin` | `macos-14` (arm64 native) |

Each job:
1. Installs Rust + protobuf-compiler.
2. `cargo build --release` (produces `.a` and `.{so,dylib}` together).
3. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --install build --prefix stage` lays out headers, both libs, the CMake config files, and `lance-c.pc` under `stage/`.
4. Tars `stage/` → `lance-c-v${VERSION}-${TRIPLE}.tar.xz`.
5. Computes SHA256 + SHA512.
6. Uploads as a GH Release asset.

vcpkg picks `.a` vs `.so` from the same tarball based on `VCPKG_LIBRARY_LINKAGE`; Conan picks based on the `shared` option. No need for separate static/shared tarballs.

A final `aggregate` job collects all SHA512s into a single `SHA512SUMS` file attached to the release, and emits a JSON snippet ready to paste into `ports/lance-c/portfile.cmake` and `recipes/lance-c/all/conandata.yml`.

### 7.2 cbindgen drift check

A separate CI job (added to `ci.yml`, not release-only) runs `cbindgen --crate lance-c -o /tmp/lance.h` and `diff include/lance/lance.h /tmp/lance.h`, failing if they differ. Catches "added an FFI fn but forgot to regenerate the header" before release.

### 7.3 Registry publishing

After the release CI completes:
1. Release script opens a PR to `microsoft/vcpkg` updating `ports/lance-c/vcpkg.json` version + portfile SHA512s, and bumps the version in `versions/baseline.json` + `versions/l-/lance-c.json`.
2. Release script opens a PR to `conan-io/conan-center-index` adding `recipes/lance-c/all/conandata.yml` entry for the new version, plus updating `config.yml`.

Both PRs are mechanical and reviewed by upstream maintainers. Cadence: per lance-c tag, expected monthly.

## 8. Versioning policy

| Version range | ABI policy | `LanceCConfigVersion.cmake` compatibility | vcpkg version constraint | Conan range |
|---|---|---|---|---|
| `0.x.y` | ABI may break across **MINOR**; stable across **PATCH** | `SameMinorVersion` | `[0.1, 0.2)` | `0.1.x` |
| `>= 1.0` | ABI stable across **PATCH**; may break across **MAJOR** | `SameMajorVersion` | `[1, 2)` | `1.x` |

The Cargo crate version in `Cargo.toml` is the source of truth. The CMake `project(LanceC VERSION ...)` reads it via `cmake/cargo-version.cmake` (small parser, no external deps) so we never have to bump it in two places.

## 9. Migration impact

| Surface | Before | After |
|---|---|---|
| C include path | `#include "lance.h"` | `#include <lance/lance.h>` |
| C++ include path | `#include "lance.hpp"` | `#include <lance/lance.hpp>` |
| Build entry point | `cargo build --release` | `cargo build --release` *or* `cmake --install` *or* `vcpkg install lance-c` *or* Conan |
| Existing tests | `tests/cpp/test_c_api.c` etc. | Same files, updated `#include` path |
| `Cargo.toml` lance dep | `lance = "3.0.1"` | `lance = "4.0.1"` (routine bump bundled in this work) |
| `build.rs` | comment-only | comment-only (cbindgen still manual; release CI verifies) |

External consumers vendoring `lance.h` from a git checkout must update their include path. Documented in CHANGELOG and release notes.

## 10. Acceptance criteria

The work is complete when all of the following are true:

1. **Repo builds** — fresh clone + `cmake -S . -B build && cmake --build build && cmake --install build --prefix /tmp/p` produces `/tmp/p/include/lance/lance.h`, `/tmp/p/lib/liblance_c.a`, `/tmp/p/lib/cmake/LanceC/LanceCConfig.cmake`.
2. **find_package round-trip** — a minimal external consumer using `find_package(LanceC 0.1 REQUIRED)` + `target_link_libraries(... LanceC::lance_c)` compiles and runs against the installed prefix on Linux x86_64 and macOS arm64. (Add as a smoke-test job in `ci.yml`.)
3. **vcpkg port works** — `vcpkg install lance-c --overlay-ports=ports` against a local checkout installs the prebuilt and the trivial CMake consumer above finds it. Validated against both `x64-linux` and `x64-linux-dynamic` triplets.
4. **Conan recipe works** — `conan create recipes/lance-c/all` produces a package the same trivial consumer can `find_package`.
5. **Release CI green on tag** — pushing a `v0.1.0` tag produces 8 tarballs + `SHA512SUMS` on a GH Release.
6. **Upstream submissions land** — `microsoft/vcpkg` PR and `conan-io/conan-center-index` PR are open with green CI (merging is on upstream maintainer cadence and is not gating).
7. **cbindgen drift check passes** in `ci.yml`.
8. **README updated** — Phase 4 row checked off, "Building" section shows all four entry points.

## 11. Open questions / future work

- **Slint-style "FetchContent" path.** Could expose `FetchContent_Declare(LanceC GIT_REPOSITORY ...)` so consumers vendor lance-c into their build. Cheap follow-up; not required for v0.
- **musl Linux + Windows.** Add when there's demonstrated demand and CI capacity.
- **Pre-built Docker image / dev container.** For CI users who want a known-good lance-c preinstalled. Out of scope.
- **Signed releases (cosign / SLSA).** Not in scope at 0.x; revisit at 1.0.
