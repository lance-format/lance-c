# Phase 4: Package Distribution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `lance-c` installable via CMake `find_package(LanceC)`, `vcpkg install lance-c`, or `conan install lance-c/0.1.0`, with prebuilt binaries on GitHub Releases for Linux/macOS × x86_64/aarch64.

**Architecture:** Top-level `CMakeLists.txt` uses [Corrosion](https://github.com/corrosion-rs/corrosion) to bridge to `cargo build`, then exports a `LanceC::lance_c` imported target with platform deps already declared. vcpkg port and Conan recipe download prebuilt tarballs from GH Releases by default; a `from_source` opt-in builds via cargo. Release CI builds 4 tarballs per tag.

**Tech Stack:** CMake ≥ 3.22, Corrosion v0.5+, cargo, vcpkg, Conan 2, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-04-27-package-distribution-design.md`

---

## Task 1: Bump lance dependency 3.0.1 → 4.0.1

**Why first:** The packaging work pins behavior to a specific lance release. Bumping first ensures all subsequent tasks build against the version we'll ship.

**Files:**
- Modify: `Cargo.toml`
- Regenerate: `Cargo.lock`

- [ ] **Step 1: Update lance pins in Cargo.toml**

Edit `Cargo.toml`. Replace every `"3.0.1"` referencing a `lance*` crate with `"4.0.1"`. There are 9 occurrences total — 5 under `[dependencies]` and 4 under `[dev-dependencies]`:

```toml
[dependencies]
lance = "4.0.1"
lance-core = "4.0.1"
lance-index = "4.0.1"
lance-io = "4.0.1"
lance-linalg = "4.0.1"
# (other deps unchanged)

[dev-dependencies]
lance = "4.0.1"
lance-datagen = "4.0.1"
lance-file = "4.0.1"
lance-table = "4.0.1"
# (other dev-deps unchanged)
```

- [ ] **Step 2: Regenerate Cargo.lock and check compilation**

Run: `cargo update -p lance -p lance-core -p lance-index -p lance-io -p lance-linalg -p lance-datagen -p lance-file -p lance-table && cargo check --all-targets`

Expected: `Finished` with no errors. If lance 4.0.1 introduced an API break, surface the failure to the user before continuing — do **not** silently rewrite call sites in this task.

- [ ] **Step 3: Run the full test suite**

Run: `cargo test`

Expected: all tests pass.

- [ ] **Step 4: Commit**

```bash
git add Cargo.toml Cargo.lock
git commit -m "chore(deps): bump lance to 4.0.1"
```

---

## Task 2: Move headers to include/lance/ subdirectory

**Files:**
- Move: `include/lance.h` → `include/lance/lance.h`
- Move: `include/lance.hpp` → `include/lance/lance.hpp`
- Modify: `include/lance/lance.hpp` (internal `#include`)
- Modify: `tests/cpp/test_c_api.c`
- Modify: `tests/cpp/test_cpp_api.cpp`

- [ ] **Step 1: Move the header files with `git mv`**

```bash
mkdir -p include/lance
git mv include/lance.h include/lance/lance.h
git mv include/lance.hpp include/lance/lance.hpp
```

- [ ] **Step 2: Update `include/lance/lance.hpp` internal include**

In `include/lance/lance.hpp`, change line 18 from:

```cpp
#include "lance.h"
```

to:

```cpp
#include "lance/lance.h"
```

- [ ] **Step 3: Update `tests/cpp/test_c_api.c`**

In `tests/cpp/test_c_api.c`, change line 14 from:

```c
#include "lance.h"
```

to:

```c
#include "lance/lance.h"
```

- [ ] **Step 4: Update `tests/cpp/test_cpp_api.cpp`**

In `tests/cpp/test_cpp_api.cpp`, change line 13 from:

```cpp
#include "lance.hpp"
```

to:

```cpp
#include "lance/lance.hpp"
```

- [ ] **Step 5: Verify the C/C++ compile-and-run tests still pass**

Run: `cargo test --test compile_and_run_test -- --ignored`

Expected: both `test_c_compilation_and_execution` and `test_cpp_compilation_and_execution` pass. The `compile_c_test`/`compile_cpp_test` helpers in `tests/compile_and_run_test.rs` already pass `-I${manifest_dir}/include` to clang, so the new `lance/` subdir is reachable as `<lance/lance.h>` without any change to the test runner.

- [ ] **Step 6: Commit**

```bash
git add include/ tests/cpp/
git commit -m "refactor(headers): move public headers to include/lance/

Convention is namespaced subdir (<lance/lance.h>) to avoid
header-name collisions when installed system-wide. Matches
librsvg's <librsvg/rsvg.h> layout.

Breaking change for external consumers vendoring lance.h
from a git checkout."
```

---

## Task 3: Add cargo-c metadata to Cargo.toml

**Why:** Documents the lance-c crate as a C-API crate so `cargo cinstall` works for distro packagers who prefer the cargo-native flow. Does not change `cargo build` behavior.

**Files:**
- Modify: `Cargo.toml`

- [ ] **Step 1: Append `[package.metadata.capi]` block**

At the end of `Cargo.toml` (after `[profile.release]`), append:

```toml
[package.metadata.capi.header]
subdirectory = "lance"
generation = false  # we ship a hand-maintained header at include/lance/lance.h

[package.metadata.capi.pkg_config]
name = "lance-c"
filename = "lance-c"
description = "C/C++ bindings for the Lance columnar data format"

[package.metadata.capi.library]
name = "lance_c"
versioning = false  # 0.x ABI is unstable; revisit at 1.0
```

- [ ] **Step 2: Verify the manifest still parses**

Run: `cargo check`

Expected: `Finished`. cargo ignores unknown `package.metadata.*` keys, so this should be a no-op for `cargo build`.

- [ ] **Step 3: Commit**

```bash
git add Cargo.toml
git commit -m "build: add cargo-c metadata for cargo cinstall path

Documents lance-c as a C-API crate so distro packagers can use
'cargo cinstall' as an alternative install entry point. CMake
superbuild remains the primary path for C++ consumers."
```

---

## Task 4: Add top-level CMakeLists.txt with Corrosion bridge

**Files:**
- Create: `CMakeLists.txt`
- Create: `cmake/cargo-version.cmake`
- Create: `cmake/Corrosion.cmake`
- Modify: `.gitignore` (add `build/`)

- [ ] **Step 1: Add `build/` to `.gitignore`**

Append a new line to `.gitignore`:

```
build/
```

- [ ] **Step 2: Create `cmake/cargo-version.cmake`**

This module parses `version = "X.Y.Z"` out of `Cargo.toml` so the CMake project version stays synchronized with the cargo crate version (single source of truth). Create `cmake/cargo-version.cmake` with:

```cmake
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

# Read the version field from Cargo.toml's [package] section.
# Sets CARGO_PKG_VERSION in the parent scope.
function(read_cargo_version cargo_toml out_var)
    file(READ "${cargo_toml}" _cargo_contents)
    if(NOT _cargo_contents MATCHES "\\[package\\][^[]*\nversion[ \t]*=[ \t]*\"([0-9]+\\.[0-9]+\\.[0-9]+[^\"]*)\"")
        message(FATAL_ERROR "Could not parse version from ${cargo_toml}")
    endif()
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
```

- [ ] **Step 3: Create `cmake/Corrosion.cmake`**

Create `cmake/Corrosion.cmake` with:

```cmake
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

# Vendor Corrosion (Rust→CMake bridge) via FetchContent.
# Pinned to a tagged release for reproducibility.
include(FetchContent)

FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.5.1
)
FetchContent_MakeAvailable(Corrosion)
```

- [ ] **Step 4: Create top-level `CMakeLists.txt`**

Create `CMakeLists.txt` with:

```cmake
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

cmake_minimum_required(VERSION 3.22)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(cargo-version)
read_cargo_version("${CMAKE_CURRENT_LIST_DIR}/Cargo.toml" LANCE_C_VERSION)

project(LanceC
    VERSION ${LANCE_C_VERSION}
    DESCRIPTION "C/C++ bindings for the Lance columnar data format"
    HOMEPAGE_URL "https://github.com/lance-format/lance-c"
    LANGUAGES C CXX
)

include(GNUInstallDirs)

# ─── Options ─────────────────────────────────────────────────────────────────
option(LANCE_C_USE_PREBUILT
    "Skip cargo build and use a prebuilt liblance_c at LANCE_C_PREBUILT_DIR"
    OFF)
set(LANCE_C_PREBUILT_DIR "" CACHE PATH
    "Directory containing prebuilt liblance_c.{a,so,dylib} (when LANCE_C_USE_PREBUILT=ON)")
set(LANCE_C_LINK "static" CACHE STRING
    "Default linkage for the LanceC::lance_c alias: static or shared")
set_property(CACHE LANCE_C_LINK PROPERTY STRINGS static shared)

# ─── Build via Corrosion (or skip for prebuilt mode) ─────────────────────────
if(NOT LANCE_C_USE_PREBUILT)
    include(Corrosion)
    corrosion_import_crate(
        MANIFEST_PATH "${CMAKE_CURRENT_LIST_DIR}/Cargo.toml"
        CRATES lance-c
        CRATE_TYPES staticlib cdylib
        PROFILE release
    )
endif()

# Imported targets and install rules are added in subsequent tasks.
```

- [ ] **Step 5: Configure and build**

Run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Expected: configure prints `lance-c: parsed version 0.1.0` (from `read_cargo_version`), Corrosion is fetched into `build/_deps/`, and `cargo build --release` runs end-to-end. The static lib appears at `build/cargo/build/<host-triple>/release/liblance_c.a`.

- [ ] **Step 6: Verify the static library was produced**

Run:

```bash
find build -name 'liblance_c.*' -type f
```

Expected: at least `liblance_c.a` and `liblance_c.{so,dylib}` listed (Corrosion produces both because `Cargo.toml` declares `crate-type = ["cdylib", "staticlib", "rlib"]`).

- [ ] **Step 7: Commit**

```bash
git add .gitignore CMakeLists.txt cmake/
git commit -m "build(cmake): top-level CMakeLists.txt with Corrosion cargo bridge

Adds an opt-in CMake build that delegates the cargo invocation to
Corrosion (a CMake module purpose-built for importing Rust crates
as CMake imported targets). 'cargo build' continues to work
unchanged for Rust-first users.

The CMake project version is parsed from Cargo.toml's [package].version
so we never have to bump it in two places."
```

---

## Task 5: Add LanceC::lance_c imported targets with platform deps

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Append the imported-target definitions to `CMakeLists.txt`**

After the `corrosion_import_crate(...)` call from Task 4, append:

```cmake
# ─── Build-tree imported targets ─────────────────────────────────────────────
# These are for consumers using add_subdirectory(lance-c). Install-tree
# consumers get equivalent targets re-created in LanceCConfig.cmake (Task 6).
# We keep the install path separate because IMPORTED targets cannot be
# threaded through install(EXPORT).

if(LANCE_C_USE_PREBUILT)
    set(_lance_c_static_real "${LANCE_C_PREBUILT_DIR}/lib/liblance_c${CMAKE_STATIC_LIBRARY_SUFFIX}")
    set(_lance_c_shared_real "${LANCE_C_PREBUILT_DIR}/lib/liblance_c${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(_lance_c_include_dirs "${LANCE_C_PREBUILT_DIR}/include")
else()
    # Corrosion produces 'lance-c-static' (staticlib) and 'lance-c' (cdylib)
    # IMPORTED targets from the crate-type declaration in Cargo.toml.
    set(_lance_c_static_real lance-c-static)
    set(_lance_c_shared_real lance-c)
    set(_lance_c_include_dirs "${CMAKE_CURRENT_LIST_DIR}/include")
endif()

# Platform link requirements declared once. INTERFACE library so we can attach
# it to multiple consumer targets without duplication.
add_library(LanceC_platform_deps INTERFACE)
if(APPLE)
    target_link_libraries(LanceC_platform_deps INTERFACE
        "-framework CoreFoundation"
        "-framework Security"
        "-framework SystemConfiguration")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(LanceC_platform_deps INTERFACE pthread dl m)
endif()

# Public namespaced targets for build-tree (add_subdirectory) consumers.
# IMPORTED INTERFACE libraries because they aggregate other libs + flags
# but have no compile inputs of their own.
add_library(LanceC::lance_c_static INTERFACE IMPORTED GLOBAL)
set_target_properties(LanceC::lance_c_static PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_lance_c_include_dirs}"
    INTERFACE_LINK_LIBRARIES "${_lance_c_static_real};LanceC_platform_deps")

add_library(LanceC::lance_c_shared INTERFACE IMPORTED GLOBAL)
set_target_properties(LanceC::lance_c_shared PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_lance_c_include_dirs}"
    INTERFACE_LINK_LIBRARIES "${_lance_c_shared_real};LanceC_platform_deps")

if(LANCE_C_LINK STREQUAL "shared")
    add_library(LanceC::lance_c ALIAS LanceC::lance_c_shared)
else()
    add_library(LanceC::lance_c ALIAS LanceC::lance_c_static)
endif()
```

- [ ] **Step 2: Reconfigure and rebuild**

Run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Expected: clean configure, no errors about unknown targets.

- [ ] **Step 3: Sanity-check the alias resolves**

Run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLANCE_C_LINK=shared --log-level=STATUS 2>&1 | head -20
```

Expected: configure succeeds. (Full validation happens via the consumer example in Task 8.)

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "build(cmake): expose LanceC::lance_c imported target

Defines LanceC::lance_c_static and LanceC::lance_c_shared imported
targets, plus an unversioned LanceC::lance_c alias selected by
LANCE_C_LINK (default static).

Each target's INTERFACE_LINK_LIBRARIES already declares the
transitive platform deps reqwest/native-tls/object_store pull in:
  macOS: CoreFoundation + Security + SystemConfiguration
  Linux: pthread + dl + m

This is what makes the consumer's CMakeLists.txt a one-liner."
```

---

## Task 6: Install rules + LanceCConfig.cmake template

**Files:**
- Create: `cmake/LanceCConfig.cmake.in`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `cmake/LanceCConfig.cmake.in`**

This template hand-rolls the `LanceC::lance_c_*` IMPORTED targets at `find_package` time instead of going through `install(EXPORT)` (CMake doesn't allow `install(EXPORT)` on IMPORTED targets, and Corrosion's targets are IMPORTED — that path is a dead end). Pattern matches what wasmtime does for its consumer-facing CMake.

Create `cmake/LanceCConfig.cmake.in` with:

```cmake
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

@PACKAGE_INIT@

set_and_check(_LANCE_C_INCLUDE_DIR "@PACKAGE_CMAKE_INSTALL_INCLUDEDIR@")
set_and_check(_LANCE_C_LIB_DIR     "@PACKAGE_CMAKE_INSTALL_LIBDIR@")

set(_lance_c_static "${_LANCE_C_LIB_DIR}/liblance_c${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(_lance_c_shared "${_LANCE_C_LIB_DIR}/liblance_c${CMAKE_SHARED_LIBRARY_SUFFIX}")

# Platform link requirements (must match build-tree CMakeLists.txt).
set(_lance_c_platform_deps "")
if(APPLE)
    list(APPEND _lance_c_platform_deps
        "-framework CoreFoundation"
        "-framework Security"
        "-framework SystemConfiguration")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND _lance_c_platform_deps pthread dl m)
endif()

if(EXISTS "${_lance_c_static}" AND NOT TARGET LanceC::lance_c_static)
    add_library(LanceC::lance_c_static UNKNOWN IMPORTED)
    set_target_properties(LanceC::lance_c_static PROPERTIES
        IMPORTED_LOCATION "${_lance_c_static}"
        INTERFACE_INCLUDE_DIRECTORIES "${_LANCE_C_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${_lance_c_platform_deps}")
endif()

if(EXISTS "${_lance_c_shared}" AND NOT TARGET LanceC::lance_c_shared)
    add_library(LanceC::lance_c_shared UNKNOWN IMPORTED)
    set_target_properties(LanceC::lance_c_shared PROPERTIES
        IMPORTED_LOCATION "${_lance_c_shared}"
        INTERFACE_INCLUDE_DIRECTORIES "${_LANCE_C_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${_lance_c_platform_deps}")
endif()

# Resolve LanceC::lance_c alias to whichever linkage the consumer requested.
# Default static; overridden by setting LanceC_LINK=shared before find_package.
if(NOT DEFINED LanceC_LINK)
    set(LanceC_LINK "static")
endif()

if(NOT TARGET LanceC::lance_c)
    if(LanceC_LINK STREQUAL "shared" AND TARGET LanceC::lance_c_shared)
        add_library(LanceC::lance_c ALIAS LanceC::lance_c_shared)
    elseif(TARGET LanceC::lance_c_static)
        add_library(LanceC::lance_c ALIAS LanceC::lance_c_static)
    elseif(TARGET LanceC::lance_c_shared)
        # Fallback: only the shared lib was installed (e.g., vcpkg :dynamic triplet).
        add_library(LanceC::lance_c ALIAS LanceC::lance_c_shared)
    else()
        message(FATAL_ERROR "LanceC: neither liblance_c${CMAKE_STATIC_LIBRARY_SUFFIX} "
            "nor liblance_c${CMAKE_SHARED_LIBRARY_SUFFIX} found in ${_LANCE_C_LIB_DIR}")
    endif()
endif()

check_required_components(LanceC)
```

- [ ] **Step 2: Append install rules to `CMakeLists.txt`**

After the imported-target block from Task 5, append:

```cmake
# ─── Install rules ───────────────────────────────────────────────────────────
include(CMakePackageConfigHelpers)

# Headers (preserves the lance/ subdirectory).
install(DIRECTORY include/lance
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")

# Library artifacts. Corrosion's install helper handles cargo→install path
# mapping (copying liblance_c.{a,so,dylib} from target/release/ to lib/).
if(NOT LANCE_C_USE_PREBUILT)
    corrosion_install(TARGETS lance-c-static
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    corrosion_install(TARGETS lance-c
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# License.
install(FILES LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/lance-c)

# Generate LanceCConfig.cmake from the template.
configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/cmake/LanceCConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/LanceCConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LanceC
    PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR)

# Generate version file. SameMinorVersion for 0.x (ABI may break across MINOR);
# switch to SameMajorVersion when we hit 1.0.
if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(_compat SameMinorVersion)
else()
    set(_compat SameMajorVersion)
endif()
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/LanceCConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY ${_compat})

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/LanceCConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/LanceCConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LanceC)
```

- [ ] **Step 3: Test the install**

Run:

```bash
rm -rf build /tmp/lance-c-prefix
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /tmp/lance-c-prefix
```

Expected: install succeeds; `/tmp/lance-c-prefix/` contains:

- `include/lance/lance.h`
- `include/lance/lance.hpp`
- `lib/liblance_c.a`
- `lib/liblance_c.{so,dylib}`
- `lib/cmake/LanceC/LanceCConfig.cmake`
- `lib/cmake/LanceC/LanceCConfigVersion.cmake`
- `share/licenses/lance-c/LICENSE`

- [ ] **Step 4: Verify install layout**

Run:

```bash
find /tmp/lance-c-prefix -type f | sort
```

Expected: matches the bullet list above.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt cmake/LanceCConfig.cmake.in
git commit -m "build(cmake): install rules + LanceCConfig.cmake

Generates a CMake package config consumers find via
find_package(LanceC). Version file uses SameMinorVersion at 0.x
(ABI may break across MINOR); switches to SameMajorVersion at 1.0."
```

---

## Task 7: pkg-config (lance-c.pc) generation

**Files:**
- Create: `cmake/lance-c.pc.in`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `cmake/lance-c.pc.in`**

Create `cmake/lance-c.pc.in` with:

```
prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=${prefix}
libdir=${prefix}/@CMAKE_INSTALL_LIBDIR@
includedir=${prefix}/@CMAKE_INSTALL_INCLUDEDIR@

Name: lance-c
Description: @PROJECT_DESCRIPTION@
URL: @PROJECT_HOMEPAGE_URL@
Version: @PROJECT_VERSION@
Libs: -L${libdir} -llance_c
Libs.private: @LANCE_C_PC_LIBS_PRIVATE@
Cflags: -I${includedir}
```

- [ ] **Step 2: Append pkg-config install rule to `CMakeLists.txt`**

After the `LanceCConfigVersion.cmake` install rule from Task 6, append:

```cmake
# ─── pkg-config ──────────────────────────────────────────────────────────────
if(APPLE)
    set(LANCE_C_PC_LIBS_PRIVATE
        "-framework CoreFoundation -framework Security -framework SystemConfiguration")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LANCE_C_PC_LIBS_PRIVATE "-lpthread -ldl -lm")
else()
    set(LANCE_C_PC_LIBS_PRIVATE "")
endif()

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/cmake/lance-c.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/lance-c.pc"
    @ONLY)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/lance-c.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
```

- [ ] **Step 3: Re-install and verify pkg-config output**

Run:

```bash
rm -rf build /tmp/lance-c-prefix
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /tmp/lance-c-prefix
PKG_CONFIG_PATH=/tmp/lance-c-prefix/lib/pkgconfig pkg-config --cflags --libs --static lance-c
```

Expected output (on macOS):

```
-I/tmp/lance-c-prefix/include -L/tmp/lance-c-prefix/lib -llance_c -framework CoreFoundation -framework Security -framework SystemConfiguration
```

On Linux it shows `-lpthread -ldl -lm` instead of the framework flags.

- [ ] **Step 4: Commit**

```bash
git add cmake/lance-c.pc.in CMakeLists.txt
git commit -m "build(cmake): generate and install lance-c.pc pkg-config

Documents the static-link transitive deps in Libs.private so
non-CMake consumers (autotools, meson, plain make) can link
correctly via 'pkg-config --libs --static lance-c'."
```

---

## Task 8: CMake consumer example (smoke test artifact)

**Files:**
- Create: `examples/cmake-consumer/CMakeLists.txt`
- Create: `examples/cmake-consumer/main.cpp`
- Create: `examples/cmake-consumer/README.md`

- [ ] **Step 1: Create `examples/cmake-consumer/CMakeLists.txt`**

```cmake
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

cmake_minimum_required(VERSION 3.22)
project(lance_c_consumer LANGUAGES CXX)

find_package(LanceC 0.1 REQUIRED)

add_executable(consumer main.cpp)
target_compile_features(consumer PRIVATE cxx_std_17)
target_link_libraries(consumer PRIVATE LanceC::lance_c)
```

- [ ] **Step 2: Create `examples/cmake-consumer/main.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include <lance/lance.hpp>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: consumer <dataset_uri>\n");
        return 2;
    }
    try {
        auto ds = lance::Dataset::open(argv[1]);
        std::printf("rows: %llu, version: %llu\n",
            static_cast<unsigned long long>(ds.count_rows()),
            static_cast<unsigned long long>(ds.version()));
        return 0;
    } catch (const lance::Error& e) {
        std::fprintf(stderr, "lance error %d: %s\n",
            static_cast<int>(e.code), e.what());
        return 1;
    }
}
```

- [ ] **Step 3: Create `examples/cmake-consumer/README.md`**

```markdown
# lance-c CMake consumer example

Minimal C++ program that opens a Lance dataset via `find_package(LanceC)`.

## Build

After installing lance-c (e.g. via `cmake --install` or `vcpkg install`):

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/lance-c-install
cmake --build build
./build/consumer /path/to/dataset.lance
```
```

- [ ] **Step 4: Verify the consumer builds against the install**

Run:

```bash
cd /tmp
rm -rf consumer-build
cmake -S "$OLDPWD/examples/cmake-consumer" -B consumer-build \
    -DCMAKE_PREFIX_PATH=/tmp/lance-c-prefix \
    -DCMAKE_BUILD_TYPE=Release
cmake --build consumer-build
cd "$OLDPWD"
```

Expected: configure prints `Found LanceC: /tmp/lance-c-prefix/lib/cmake/LanceC/LanceCConfig.cmake (found suitable version "0.1.0", minimum required is "0.1")`. Build produces `/tmp/consumer-build/consumer`.

- [ ] **Step 5: Commit**

```bash
git add examples/cmake-consumer/
git commit -m "examples: minimal find_package(LanceC) consumer

Smoke-tests the install layout end-to-end. Built by the
'consumer-smoke-test' job in ci.yml (added in Task 9)."
```

---

## Task 9: Add find_package smoke-test job to ci.yml

**Files:**
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: Append the smoke-test job to `ci.yml`**

After the `test-macos` job (which ends around line 77), insert before the `rustdoc` job:

```yaml
  consumer-smoke-test:
    name: find_package(LanceC) smoke test
    runs-on: ${{ matrix.runner }}
    timeout-minutes: 45
    strategy:
      fail-fast: false
      matrix:
        include:
          - runner: ubuntu-24.04
            os_label: linux
          - runner: macos-14
            os_label: macos
    env:
      CC: clang
      CXX: clang++
    steps:
      - uses: actions/checkout@v4
      - uses: actions-rust-lang/setup-rust-toolchain@v1
      - uses: Swatinem/rust-cache@v2
      - name: Install protobuf-compiler (Linux)
        if: matrix.os_label == 'linux'
        run: sudo apt-get update && sudo apt-get install -y protobuf-compiler
      - name: Install protobuf (macOS)
        if: matrix.os_label == 'macos'
        run: brew install protobuf
      - name: Configure + build + install lance-c
        run: |
          cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
          cmake --build build
          cmake --install build --prefix "$PWD/_install"
      - name: Build consumer example against the install
        run: |
          cmake -S examples/cmake-consumer -B consumer-build \
            -DCMAKE_PREFIX_PATH="$PWD/_install" \
            -DCMAKE_BUILD_TYPE=Release
          cmake --build consumer-build
      - name: Verify the binary links and runs (no dataset → expect usage error)
        run: |
          set +e
          consumer-build/consumer
          rc=$?
          if [ "$rc" -ne 2 ]; then
            echo "Expected exit code 2 (usage error), got $rc"
            exit 1
          fi
```

- [ ] **Step 2: Push to a feature branch and verify CI**

Skip if running locally — push to your fork's feature branch and confirm the new `consumer-smoke-test (linux)` and `consumer-smoke-test (macos)` jobs both pass on the resulting PR.

For local verification, you can run the same steps inside a GHA-equivalent shell:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix "$PWD/_install"
cmake -S examples/cmake-consumer -B consumer-build -DCMAKE_PREFIX_PATH="$PWD/_install" -DCMAKE_BUILD_TYPE=Release
cmake --build consumer-build
./consumer-build/consumer; echo "exit: $?"
```

Expected: `usage: consumer <dataset_uri>` to stderr, `exit: 2`.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: smoke-test find_package(LanceC) on Linux + macOS

Builds the examples/cmake-consumer project against a freshly
installed lance-c prefix on both ubuntu-24.04 and macos-14.
Catches install-rule regressions before they ship."
```

---

## Task 10: Add cbindgen drift-check job to ci.yml

**Files:**
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: Append the cbindgen-drift job to `ci.yml`**

After the `consumer-smoke-test` job from Task 9, before `rustdoc`, insert:

```yaml
  cbindgen-drift:
    name: cbindgen header drift
    runs-on: ubuntu-24.04
    timeout-minutes: 15
    steps:
      - uses: actions/checkout@v4
      - uses: actions-rust-lang/setup-rust-toolchain@v1
      - uses: Swatinem/rust-cache@v2
      - name: Install protobuf-compiler
        run: sudo apt-get update && sudo apt-get install -y protobuf-compiler
      - name: Install cbindgen
        run: cargo install cbindgen --locked --version "^0.27"
      - name: Regenerate header
        run: cbindgen --crate lance-c --output /tmp/lance.h
      - name: Diff against committed header
        run: |
          if ! diff -u include/lance/lance.h /tmp/lance.h; then
            echo ""
            echo "::error::include/lance/lance.h is out of date."
            echo "Run 'cbindgen --crate lance-c -o include/lance/lance.h' and commit."
            exit 1
          fi
```

- [ ] **Step 2: Sanity-check locally (skip if cbindgen isn't installed)**

Run, only if you have cbindgen available:

```bash
cbindgen --crate lance-c --output /tmp/lance.h
diff -u include/lance/lance.h /tmp/lance.h
```

Expected: no diff (or only trivial whitespace diff). If the diff is substantial, the committed header has drifted — flag it and commit a regenerated header in a separate commit before continuing.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: detect cbindgen header drift

Catches 'added an FFI fn but forgot to regenerate include/lance/lance.h'
on every PR. Header generation stays manual (per build.rs comment);
this job only verifies."
```

---

## Task 11: Release workflow with 4-triple build matrix

**Files:**
- Create: `.github/workflows/release.yml`

- [ ] **Step 1: Create `.github/workflows/release.yml`**

```yaml
name: Release
on:
  push:
    tags:
      - 'v*.*.*'
  workflow_dispatch:
    inputs:
      version:
        description: "Version to build (without leading 'v')"
        required: true

permissions:
  contents: write   # needed for softprops/action-gh-release

env:
  CARGO_TERM_COLOR: always

jobs:
  build:
    name: Build (${{ matrix.target }})
    runs-on: ${{ matrix.runner }}
    timeout-minutes: 90
    strategy:
      fail-fast: false
      matrix:
        include:
          - target: x86_64-unknown-linux-gnu
            runner: ubuntu-24.04
            os_label: linux
          - target: aarch64-unknown-linux-gnu
            runner: ubuntu-24.04-arm
            os_label: linux
          - target: x86_64-apple-darwin
            runner: macos-13
            os_label: macos
          - target: aarch64-apple-darwin
            runner: macos-14
            os_label: macos
    env:
      CC: clang
      CXX: clang++
    steps:
      - uses: actions/checkout@v4
      - uses: actions-rust-lang/setup-rust-toolchain@v1
        with:
          target: ${{ matrix.target }}
      - uses: Swatinem/rust-cache@v2
        with:
          key: ${{ matrix.target }}
      - name: Install protobuf (Linux)
        if: matrix.os_label == 'linux'
        run: sudo apt-get update && sudo apt-get install -y protobuf-compiler
      - name: Install protobuf (macOS)
        if: matrix.os_label == 'macos'
        run: brew install protobuf
      - name: Resolve version
        id: ver
        run: |
          if [ "${{ github.event_name }}" = "workflow_dispatch" ]; then
            echo "version=${{ inputs.version }}" >>"$GITHUB_OUTPUT"
          else
            echo "version=${GITHUB_REF_NAME#v}" >>"$GITHUB_OUTPUT"
          fi
      - name: Configure + build
        run: |
          cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
          cmake --build build --parallel
      - name: Install to staging prefix
        run: |
          cmake --install build --prefix stage
      - name: Tar
        id: pack
        run: |
          VERSION=${{ steps.ver.outputs.version }}
          TARGET=${{ matrix.target }}
          ARCHIVE="lance-c-v${VERSION}-${TARGET}.tar.xz"
          tar -C stage -cJf "${ARCHIVE}" .
          shasum -a 256 "${ARCHIVE}" > "${ARCHIVE}.sha256"
          shasum -a 512 "${ARCHIVE}" > "${ARCHIVE}.sha512"
          echo "archive=${ARCHIVE}" >>"$GITHUB_OUTPUT"
      - name: Upload as workflow artifact
        uses: actions/upload-artifact@v4
        with:
          name: ${{ matrix.target }}
          path: |
            ${{ steps.pack.outputs.archive }}
            ${{ steps.pack.outputs.archive }}.sha256
            ${{ steps.pack.outputs.archive }}.sha512
          if-no-files-found: error
          retention-days: 14

  publish:
    name: Publish GitHub Release
    needs: build
    runs-on: ubuntu-24.04
    if: startsWith(github.ref, 'refs/tags/v')
    steps:
      - uses: actions/checkout@v4
      - uses: actions/download-artifact@v4
        with:
          path: dist
          merge-multiple: true
      - name: Aggregate SHA512SUMS
        run: |
          cd dist
          cat *.sha512 > SHA512SUMS
          ls -lah
      - name: Emit recipe-update snippet
        run: |
          cd dist
          echo "## Suggested recipe updates" > recipe-snippets.md
          echo '```cmake' >> recipe-snippets.md
          echo '# ports/lance-c/portfile.cmake — paste these set() lines' >> recipe-snippets.md
          for f in *.tar.xz; do
            sha=$(awk '{print $1}' "${f}.sha512")
            triple_target="${f#lance-c-v*-}"
            triple_target="${triple_target%.tar.xz}"
            echo "set(LANCE_C_SHA512_${triple_target}  \"${sha}\")"
          done >> recipe-snippets.md
          echo '```' >> recipe-snippets.md
          cat recipe-snippets.md
      - name: Publish release
        uses: softprops/action-gh-release@v2
        with:
          files: |
            dist/*.tar.xz
            dist/*.sha256
            dist/*.sha512
            dist/SHA512SUMS
          generate_release_notes: true
          fail_on_unmatched_files: true
```

- [ ] **Step 2: Lint with `actionlint` if available**

Run (skip if `actionlint` not installed):

```bash
actionlint .github/workflows/release.yml
```

Expected: no findings.

- [ ] **Step 3: Trigger via `workflow_dispatch` from a feature branch (optional dry-run)**

Once pushed, run from the GitHub UI: Actions → Release → "Run workflow" → set version `0.0.1-dev`. Verify all 4 build jobs complete and produce 4 `.tar.xz` artifacts. The `publish` job will be skipped because we're not on a `v*` tag — that's intentional.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/release.yml
git commit -m "ci(release): build prebuilt tarballs on tag push

Produces lance-c-v<VERSION>-<TRIPLE>.tar.xz for:
  - x86_64-unknown-linux-gnu  (ubuntu-24.04)
  - aarch64-unknown-linux-gnu (ubuntu-24.04-arm, native)
  - x86_64-apple-darwin       (macos-13)
  - aarch64-apple-darwin      (macos-14, native)

Each tarball contains both static (.a) and shared (.so/.dylib).
Publish job uploads to GitHub Releases on v*.*.* tag pushes and
emits a recipe-update snippet for paste-in to the vcpkg portfile."
```

---

## Task 12: vcpkg port files

**Files:**
- Create: `ports/lance-c/vcpkg.json`
- Create: `ports/lance-c/portfile.cmake`
- Create: `ports/lance-c/usage`
- Create: `ports/README.md`

- [ ] **Step 1: Create `ports/README.md`**

```markdown
# vcpkg overlay port for lance-c

This directory is the canonical copy of the vcpkg port that ships in the
upstream `microsoft/vcpkg` registry. Use it as an overlay against any vcpkg
checkout to install lance-c locally:

```bash
vcpkg install lance-c --overlay-ports=path/to/lance-c/ports
```

After each release, the contents are mirrored into `microsoft/vcpkg` via PR.
```

- [ ] **Step 2: Create `ports/lance-c/vcpkg.json`**

```json
{
  "$schema": "https://raw.githubusercontent.com/microsoft/vcpkg-tool/main/docs/vcpkg.schema.json",
  "name": "lance-c",
  "version": "0.1.0",
  "description": "C/C++ bindings for the Lance columnar data format",
  "homepage": "https://github.com/lance-format/lance-c",
  "license": "Apache-2.0",
  "supports": "(linux | osx) & (x64 | arm64)",
  "dependencies": [
    {
      "name": "vcpkg-cmake-config",
      "host": true
    }
  ],
  "features": {
    "from-source": {
      "description": "Build from source via cargo (requires Rust toolchain on host instead of downloading prebuilt)"
    }
  }
}
```

- [ ] **Step 3: Create `ports/lance-c/portfile.cmake`**

```cmake
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

# Map vcpkg triplets to our release tarball target names.
set(LANCE_C_TRIPLE_x64-linux           "x86_64-unknown-linux-gnu")
set(LANCE_C_TRIPLE_x64-linux-dynamic   "x86_64-unknown-linux-gnu")
set(LANCE_C_TRIPLE_arm64-linux         "aarch64-unknown-linux-gnu")
set(LANCE_C_TRIPLE_arm64-linux-dynamic "aarch64-unknown-linux-gnu")
set(LANCE_C_TRIPLE_x64-osx             "x86_64-apple-darwin")
set(LANCE_C_TRIPLE_x64-osx-dynamic     "x86_64-apple-darwin")
set(LANCE_C_TRIPLE_arm64-osx           "aarch64-apple-darwin")
set(LANCE_C_TRIPLE_arm64-osx-dynamic   "aarch64-apple-darwin")

# SHA512s of each prebuilt tarball. Updated by the release script after
# .github/workflows/release.yml publishes a tag.
set(LANCE_C_SHA512_x86_64-unknown-linux-gnu  "REPLACE_WITH_REAL_SHA512_AT_RELEASE")
set(LANCE_C_SHA512_aarch64-unknown-linux-gnu "REPLACE_WITH_REAL_SHA512_AT_RELEASE")
set(LANCE_C_SHA512_x86_64-apple-darwin       "REPLACE_WITH_REAL_SHA512_AT_RELEASE")
set(LANCE_C_SHA512_aarch64-apple-darwin      "REPLACE_WITH_REAL_SHA512_AT_RELEASE")

set(_TRIPLE "${LANCE_C_TRIPLE_${TARGET_TRIPLET}}")
if(NOT _TRIPLE)
    message(FATAL_ERROR "lance-c does not provide prebuilts for triplet '${TARGET_TRIPLET}'. "
        "Install with the 'from-source' feature to build via cargo.")
endif()

set(_SHA512 "${LANCE_C_SHA512_${_TRIPLE}}")

if("from-source" IN_LIST FEATURES)
    # Source build via cargo. Requires Rust toolchain on the host machine;
    # vcpkg does not provide one. Fall through to vcpkg_from_github + cmake +
    # corrosion, which will invoke cargo for us.
    vcpkg_from_github(
        OUT_SOURCE_PATH SOURCE_PATH
        REPO lance-format/lance-c
        REF "v${VERSION}"
        SHA512 "REPLACE_WITH_SOURCE_TARBALL_SHA512"
    )
    vcpkg_cmake_configure(
        SOURCE_PATH "${SOURCE_PATH}"
        OPTIONS
            -DLANCE_C_USE_PREBUILT=OFF
            -DLANCE_C_LINK=$<IF:$<STREQUAL:${VCPKG_LIBRARY_LINKAGE},dynamic>,shared,static>
    )
    vcpkg_cmake_install()
else()
    # Default: download the prebuilt tarball for this triplet.
    vcpkg_download_distfile(ARCHIVE
        URLS "https://github.com/lance-format/lance-c/releases/download/v${VERSION}/lance-c-v${VERSION}-${_TRIPLE}.tar.xz"
        FILENAME "lance-c-v${VERSION}-${_TRIPLE}.tar.xz"
        SHA512 "${_SHA512}"
    )
    vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}" NO_REMOVE_ONE_LEVEL)

    file(INSTALL "${SOURCE_PATH}/include/" DESTINATION "${CURRENT_PACKAGES_DIR}/include")

    if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
        file(INSTALL "${SOURCE_PATH}/lib/liblance_c.a"
            DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
        # No debug build for prebuilts; mirror the release build.
        file(INSTALL "${SOURCE_PATH}/lib/liblance_c.a"
            DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
    else()
        file(GLOB _shared "${SOURCE_PATH}/lib/liblance_c.so*"
                          "${SOURCE_PATH}/lib/liblance_c.dylib")
        file(INSTALL ${_shared} DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
        file(INSTALL ${_shared} DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
    endif()

    file(INSTALL "${SOURCE_PATH}/lib/cmake/" DESTINATION "${CURRENT_PACKAGES_DIR}/lib/cmake")
    file(INSTALL "${SOURCE_PATH}/lib/pkgconfig/" DESTINATION "${CURRENT_PACKAGES_DIR}/lib/pkgconfig")

    file(INSTALL "${SOURCE_PATH}/share/licenses/lance-c/LICENSE"
        DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
endif()

vcpkg_cmake_config_fixup(PACKAGE_NAME LanceC CONFIG_PATH lib/cmake/LanceC)
vcpkg_fixup_pkgconfig()

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
```

- [ ] **Step 4: Create `ports/lance-c/usage`**

```
The package lance-c provides CMake targets:

    find_package(LanceC CONFIG REQUIRED)
    target_link_libraries(main PRIVATE LanceC::lance_c)

The default linkage matches the vcpkg triplet:
    x64-linux         → static  (LanceC::lance_c → LanceC::lance_c_static)
    x64-linux-dynamic → shared  (LanceC::lance_c → LanceC::lance_c_shared)

Override per-build by setting LanceC_LINK=static|shared before find_package.
```

- [ ] **Step 5: Local validation against a real vcpkg checkout (optional)**

Skip if you don't have a vcpkg checkout handy. Otherwise:

```bash
vcpkg install lance-c --overlay-ports=$(pwd)/ports
```

Will fail until at least one `v*` release exists with real SHA512s. That's expected — local testing of the from-source path is:

```bash
vcpkg install 'lance-c[from-source]' --overlay-ports=$(pwd)/ports
```

(also requires the source tarball SHA512 placeholder to be replaced — note the `REPLACE_WITH_SOURCE_TARBALL_SHA512` marker).

- [ ] **Step 6: Commit**

```bash
git add ports/
git commit -m "ports: vcpkg overlay for lance-c

Default path downloads prebuilt tarballs from GitHub Releases for
the matching triplet (sidesteps vcpkg#33824 - Rust-in-vcpkg).
'from-source' feature opts into cargo build for unsupported triples.

SHA512s are placeholders; real values populated by the release
script after each tagged build."
```

---

## Task 13: Conan recipe + test_package

**Files:**
- Create: `recipes/lance-c/all/conanfile.py`
- Create: `recipes/lance-c/all/conandata.yml`
- Create: `recipes/lance-c/all/test_package/conanfile.py`
- Create: `recipes/lance-c/all/test_package/CMakeLists.txt`
- Create: `recipes/lance-c/all/test_package/test_package.cpp`
- Create: `recipes/lance-c/config.yml`
- Create: `recipes/README.md`

- [ ] **Step 1: Create `recipes/README.md`**

```markdown
# Conan recipe for lance-c

Canonical source of the recipe published to `conan-io/conan-center-index`.

## Local test

```bash
conan create recipes/lance-c/all --version=0.1.0
```
```

- [ ] **Step 2: Create `recipes/lance-c/config.yml`**

```yaml
versions:
  "0.1.0":
    folder: all
```

- [ ] **Step 3: Create `recipes/lance-c/all/conandata.yml`**

```yaml
sources:
  "0.1.0":
    "Linux-x86_64":
      url: "https://github.com/lance-format/lance-c/releases/download/v0.1.0/lance-c-v0.1.0-x86_64-unknown-linux-gnu.tar.xz"
      sha256: "REPLACE_WITH_REAL_SHA256_AT_RELEASE"
    "Linux-armv8":
      url: "https://github.com/lance-format/lance-c/releases/download/v0.1.0/lance-c-v0.1.0-aarch64-unknown-linux-gnu.tar.xz"
      sha256: "REPLACE_WITH_REAL_SHA256_AT_RELEASE"
    "Macos-x86_64":
      url: "https://github.com/lance-format/lance-c/releases/download/v0.1.0/lance-c-v0.1.0-x86_64-apple-darwin.tar.xz"
      sha256: "REPLACE_WITH_REAL_SHA256_AT_RELEASE"
    "Macos-armv8":
      url: "https://github.com/lance-format/lance-c/releases/download/v0.1.0/lance-c-v0.1.0-aarch64-apple-darwin.tar.xz"
      sha256: "REPLACE_WITH_REAL_SHA256_AT_RELEASE"

source-from-tag:
  "0.1.0":
    url: "https://github.com/lance-format/lance-c/archive/refs/tags/v0.1.0.tar.gz"
    sha256: "REPLACE_WITH_SOURCE_TARBALL_SHA256"
```

- [ ] **Step 4: Create `recipes/lance-c/all/conanfile.py`**

```python
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get
from conan.tools.scm import Version

required_conan_version = ">=2.0"


class LanceCConan(ConanFile):
    name = "lance-c"
    description = "C/C++ bindings for the Lance columnar data format"
    license = "Apache-2.0"
    homepage = "https://github.com/lance-format/lance-c"
    url = "https://github.com/conan-io/conan-center-index"
    topics = ("lance", "ffi", "c", "cpp", "arrow", "columnar")
    settings = "os", "arch", "compiler", "build_type"
    options = {
        "shared": [True, False],
        "from_source": [True, False],
    }
    default_options = {
        "shared": False,
        "from_source": False,
    }

    @property
    def _supported_keys(self):
        return {
            ("Linux", "x86_64"): "Linux-x86_64",
            ("Linux", "armv8"): "Linux-armv8",
            ("Macos", "x86_64"): "Macos-x86_64",
            ("Macos", "armv8"): "Macos-armv8",
        }

    def validate(self):
        key = (str(self.settings.os), str(self.settings.arch))
        if key not in self._supported_keys:
            raise ConanInvalidConfiguration(
                f"lance-c does not provide prebuilts for {key[0]}/{key[1]}. "
                f"Install with -o lance-c/*:from_source=True to build via cargo "
                f"(requires Rust toolchain)."
            )

    def configure(self):
        if self.options.shared:
            del self.options.fPIC

    def layout(self):
        if self.options.from_source:
            cmake_layout(self)

    def source(self):
        # Source-build path needs the git checkout. Prebuilt path skips this.
        if self.options.from_source:
            data = self.conan_data["source-from-tag"][self.version]
            get(self, **data, strip_root=True)

    def generate(self):
        if self.options.from_source:
            tc = CMakeToolchain(self)
            tc.cache_variables["LANCE_C_LINK"] = "shared" if self.options.shared else "static"
            tc.generate()

    def build(self):
        if self.options.from_source:
            cmake = CMake(self)
            cmake.configure()
            cmake.build()
        else:
            key = self._supported_keys[(str(self.settings.os), str(self.settings.arch))]
            data = self.conan_data["sources"][self.version][key]
            get(self, **data, destination=self.build_folder, strip_root=False)

    def package(self):
        copy(self, "LICENSE",
             src=self.source_folder if self.options.from_source else self.build_folder,
             dst=os.path.join(self.package_folder, "licenses"),
             keep_path=False)
        if self.options.from_source:
            cmake = CMake(self)
            cmake.install()
        else:
            # Layout in the prebuilt tarball: include/, lib/, share/
            copy(self, "*", src=os.path.join(self.build_folder, "include"),
                 dst=os.path.join(self.package_folder, "include"))
            if self.options.shared:
                copy(self, "*.so*",   src=os.path.join(self.build_folder, "lib"),
                     dst=os.path.join(self.package_folder, "lib"), keep_path=False)
                copy(self, "*.dylib", src=os.path.join(self.build_folder, "lib"),
                     dst=os.path.join(self.package_folder, "lib"), keep_path=False)
            else:
                copy(self, "*.a", src=os.path.join(self.build_folder, "lib"),
                     dst=os.path.join(self.package_folder, "lib"), keep_path=False)

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "LanceC")
        self.cpp_info.set_property("cmake_target_name", "LanceC::lance_c")
        self.cpp_info.set_property("pkg_config_name", "lance-c")
        self.cpp_info.libs = ["lance_c"]
        if self.settings.os == "Macos":
            self.cpp_info.frameworks = ["CoreFoundation", "Security", "SystemConfiguration"]
        elif self.settings.os == "Linux":
            self.cpp_info.system_libs = ["pthread", "dl", "m"]
```

- [ ] **Step 5: Create `recipes/lance-c/all/test_package/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.22)
project(test_package LANGUAGES CXX)

find_package(LanceC CONFIG REQUIRED)

add_executable(test_package test_package.cpp)
target_compile_features(test_package PRIVATE cxx_std_17)
target_link_libraries(test_package PRIVATE LanceC::lance_c)
```

- [ ] **Step 6: Create `recipes/lance-c/all/test_package/test_package.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include <lance/lance.hpp>
#include <cstdio>

int main() {
    // Don't open a real dataset (no test fixture in Conan's test_package
    // sandbox). Just confirm the headers parse, the library links, and
    // calling lance_last_error_message() on a fresh thread returns NULL.
    const char* msg = lance_last_error_message();
    std::printf("lance_last_error_message(): %s\n", msg ? msg : "(null, ok)");
    return 0;
}
```

- [ ] **Step 7: Create `recipes/lance-c/all/test_package/conanfile.py`**

```python
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

import os

from conan import ConanFile
from conan.tools.build import can_run
from conan.tools.cmake import CMake, cmake_layout


class TestPackageConan(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeDeps", "CMakeToolchain"
    test_type = "explicit"

    def requirements(self):
        self.requires(self.tested_reference_str)

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def test(self):
        if can_run(self):
            bin_path = os.path.join(self.cpp.build.bindir, "test_package")
            self.run(bin_path, env="conanrun")
```

- [ ] **Step 8: Local validation (optional, requires conan + a real release)**

Skip if you don't have Conan installed or no `v0.1.0` release exists yet:

```bash
conan create recipes/lance-c/all --version=0.1.0
```

Will fail until SHA256 placeholders are replaced with real values from a published release.

- [ ] **Step 9: Commit**

```bash
git add recipes/
git commit -m "recipes: Conan recipe for lance-c

Default path downloads prebuilt tarballs from GitHub Releases.
'from_source=True' option builds via cargo + the CMake superbuild.
test_package smoke-tests find_package(LanceC) + LanceC::lance_c
end-to-end.

SHA256s are placeholders; populated by the release script after
each tagged build."
```

---

## Task 14: README updates

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Check off Phase 4 row in the roadmap**

In `README.md`, find this line under the "Phase 4: Advanced Features" table:

```
| [ ] | Package distribution | vcpkg and Conan recipe packaging |
```

Change `[ ]` to `[x]`.

- [ ] **Step 2: Replace the "Building" section with all four entry points**

Find the existing `## Building` section (around line 70-76):

```markdown
## Building

```bash
cargo build --release
```

The build produces `liblance_c.{so,dylib,dll}` and the headers in `include/`.
```

Replace with:

```markdown
## Building

There are four supported entry points; pick whichever matches your toolchain.

### From source via cargo (Rust developers)

```bash
cargo build --release
```

Produces `target/release/liblance_c.{so,dylib,dll}` and a `liblance_c.a`.
Headers stay in `include/lance/`.

### From source via CMake (C/C++ developers)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /your/prefix
```

Installs headers, both linkages, a `LanceCConfig.cmake` package config, and
a `lance-c.pc` pkg-config file. Consumers then:

```cmake
find_package(LanceC 0.1 REQUIRED)
target_link_libraries(myapp PRIVATE LanceC::lance_c)
```

See [`examples/cmake-consumer/`](examples/cmake-consumer/) for a minimal
working example.

### vcpkg

```bash
vcpkg install lance-c
```

Downloads a prebuilt binary for your triplet from
[GitHub Releases](https://github.com/lance-format/lance-c/releases). For
unsupported triples, opt into a source build with the `from-source` feature:

```bash
vcpkg install 'lance-c[from-source]'  # requires Rust toolchain
```

### Conan

```bash
conan install --requires=lance-c/0.1.0
```

Default path downloads prebuilts; `-o lance-c/*:from_source=True` builds
from source via cargo.

### Header path

```c
#include <lance/lance.h>     // C
#include <lance/lance.hpp>   // C++
```
```

- [ ] **Step 3: Update the C and C++ usage snippets to the new include path**

Find the C example (around line 82) — change:

```c
#include "lance.h"
```

to:

```c
#include <lance/lance.h>
```

Find the C++ example (around line 102) — change:

```cpp
#include "lance.hpp"
```

to:

```cpp
#include <lance/lance.hpp>
```

- [ ] **Step 4: Verify the README still renders**

Run (skip if you don't have a markdown renderer handy):

```bash
grep -nE '^\| \[[x ]\]' README.md | head -20
```

Expected: every Phase 4 row has either `[x]` or `[ ]`, the `Package distribution` row shows `[x]`.

- [ ] **Step 5: Commit**

```bash
git add README.md
git commit -m "docs: README reflects the four install paths

Documents cargo, cmake, vcpkg, and conan entry points; updates
the include path to <lance/lance.h>; checks off the Phase 4
'Package distribution' roadmap row."
```

---

## Task 15: Final acceptance walk-through

**Files:** None modified — this task only verifies that the work meets the spec's section 10 acceptance criteria.

- [ ] **Step 1: Acceptance #1 — fresh build + install round-trip**

Run:

```bash
rm -rf build /tmp/lance-c-final
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /tmp/lance-c-final
test -f /tmp/lance-c-final/include/lance/lance.h
test -f /tmp/lance-c-final/include/lance/lance.hpp
test -f /tmp/lance-c-final/lib/liblance_c.a
test -f /tmp/lance-c-final/lib/cmake/LanceC/LanceCConfig.cmake
test -f /tmp/lance-c-final/lib/cmake/LanceC/LanceCConfigVersion.cmake
test -f /tmp/lance-c-final/lib/pkgconfig/lance-c.pc
echo "Acceptance #1: PASS"
```

Expected: prints `Acceptance #1: PASS`.

- [ ] **Step 2: Acceptance #2 — find_package round-trip**

Run:

```bash
rm -rf /tmp/consumer-final
cmake -S examples/cmake-consumer -B /tmp/consumer-final \
    -DCMAKE_PREFIX_PATH=/tmp/lance-c-final \
    -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/consumer-final
/tmp/consumer-final/consumer 2>&1 | grep -q 'usage:' && echo "Acceptance #2: PASS"
```

Expected: `Acceptance #2: PASS`.

- [ ] **Step 3: Acceptance #3, #4 — vcpkg + Conan local builds**

Acceptance #3 and #4 require a published v0.1.0 GitHub Release with real SHA512s/SHA256s. They are validated post-release by:

```bash
# vcpkg (after release script populates SHA512s)
vcpkg install lance-c --overlay-ports=$(pwd)/ports
# Conan (after release script populates SHA256s)
conan create recipes/lance-c/all --version=0.1.0
```

For this PR's purposes, mark these as "validated by ports/portfile.cmake + recipes/lance-c/all/conanfile.py existing and well-formed" — there's no way to fully validate before the first release exists. Document this in the PR description.

- [ ] **Step 4: Acceptance #5 — release CI green on tag (deferred)**

Cannot be tested before the first tag is pushed. Validated when the maintainer cuts `v0.1.0`. Document in the PR description.

- [ ] **Step 5: Acceptance #6 — upstream submissions (deferred)**

Same — happens after the first release. Document in the PR description.

- [ ] **Step 6: Acceptance #7 — cbindgen drift check passes**

Push to a feature branch and confirm `cbindgen-drift` job passes on the resulting PR. If you have cbindgen locally:

```bash
cbindgen --crate lance-c --output /tmp/lance.h && diff -u include/lance/lance.h /tmp/lance.h && echo "Acceptance #7: PASS"
```

- [ ] **Step 7: Acceptance #8 — README reflects all four entry points**

Run:

```bash
grep -c '^### ' README.md  # counts H3s in the README
grep -q 'find_package(LanceC' README.md
grep -q 'vcpkg install lance-c' README.md
grep -q 'conan install' README.md
grep -q '\[x\] | Package distribution' README.md && echo "Acceptance #8: PASS"
```

Expected: all four `grep -q` invocations exit 0; the line ending prints `Acceptance #8: PASS`.

- [ ] **Step 8: No commit needed for this task — just confirm everything is green and update the PR description**

The PR description should explicitly note which acceptance criteria require post-release validation (#3, #4, #5, #6) and which are validated in this PR (#1, #2, #7, #8).
