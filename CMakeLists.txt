# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

cmake_minimum_required(VERSION 3.22)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(cargo-version)
include(baseline)
include(platform-deps)
read_cargo_version("${CMAKE_CURRENT_LIST_DIR}/Cargo.toml" LANCE_C_VERSION)

lance_c_load_baseline_env("${CMAKE_CURRENT_LIST_DIR}/.github/baseline.env")

# Deployment defaults must precede project() / compiler detection. This is
# the pre-project equivalent of PROJECT_IS_TOP_LEVEL.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR
        AND APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${MACOSX_DEPLOYMENT_TARGET}" CACHE STRING "Minimum macOS deployment target" FORCE)
endif()

project(LanceC
    VERSION ${LANCE_C_VERSION}
    DESCRIPTION "C/C++ bindings for the Lance columnar data format"
    HOMEPAGE_URL "https://github.com/lance-format/lance-c"
    LANGUAGES NONE
)

# Embedded builds use the parent's enabled languages. Enabling another language
# only in this directory leaves its compiler features unavailable to targets in
# the parent directory that consume our imported targets.
if(PROJECT_IS_TOP_LEVEL)
    enable_language(C CXX)
    if(NOT DEFINED CMAKE_C_STANDARD OR CMAKE_C_STANDARD LESS 11)
        set(CMAKE_C_STANDARD 11)
    endif()
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)

    if(NOT DEFINED CMAKE_CXX_STANDARD OR CMAKE_CXX_STANDARD LESS 20)
        set(CMAKE_CXX_STANDARD 20)
    endif()
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    set(CMAKE_C_VISIBILITY_PRESET hidden)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif()

include(GNUInstallDirs)

# ─── Options ─────────────────────────────────────────────────────────────────
option(LANCE_C_USE_PREBUILT
    "Skip cargo build and use a prebuilt liblance_c at LANCE_C_PREBUILT_DIR"
    OFF)
if(WIN32 AND LANCE_C_USE_PREBUILT AND NOT DEFINED LANCE_C_CRT_STATIC)
    message(FATAL_ERROR
        "Windows prebuilt CRT must be declared with LANCE_C_CRT_STATIC=ON or OFF. "
        "Match the input libraries: ON for Release /MT, OFF for dynamic CRT. "
        "Prebuilt mode copies libraries unchanged; it does not select their CRT.")
endif()
set(_lance_c_crt_static_default OFF)
if(WIN32 AND (MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    set(_lance_c_crt_static_default ON)
endif()
option(LANCE_C_CRT_STATIC
    "Use the static MSVC CRT (in prebuilt mode, declare the input libraries' CRT)"
    ${_lance_c_crt_static_default})
unset(_lance_c_crt_static_default)
set(LANCE_C_PREBUILT_DIR "" CACHE PATH
    "Directory containing prebuilt liblance_c.{a,so,dylib} (when LANCE_C_USE_PREBUILT=ON)")
set(LANCE_C_LINK "static" CACHE STRING
    "Default linkage for the LanceC::lance_c alias: static or shared")
set_property(CACHE LANCE_C_LINK PROPERTY STRINGS static shared)

if(LANCE_C_CRT_STATIC AND NOT WIN32)
    message(FATAL_ERROR "LANCE_C_CRT_STATIC is only supported on Windows")
endif()
if(LANCE_C_CRT_STATIC
        AND NOT (MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    message(FATAL_ERROR
        "LANCE_C_CRT_STATIC requires an MSVC-compatible C/C++ toolchain")
endif()
set(LanceC_STATIC_CRT "${LANCE_C_CRT_STATIC}")

# ─── Build via Corrosion (or skip for prebuilt mode) ─────────────────────────
if(NOT LANCE_C_USE_PREBUILT)
    # Explicit lookup also works on CMake 3.22/3.23, before FetchContent's
    # FIND_PACKAGE_ARGS support. Offline builds can supply Corrosion locally.
    find_package(Corrosion 0.5 QUIET CONFIG)
    if(NOT Corrosion_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            Corrosion
            GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
            GIT_TAG v0.5.1
        )
        FetchContent_MakeAvailable(Corrosion)
    endif()

    corrosion_import_crate(
        MANIFEST_PATH "${CMAKE_CURRENT_LIST_DIR}/Cargo.toml"
        CRATES lance-c
        CRATE_TYPES staticlib cdylib
        PROFILE release
    )
    if(APPLE)
        corrosion_set_env_vars(lance_c "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    if(LANCE_C_CRT_STATIC)
        if(NOT Rust_CARGO_TARGET_ENV STREQUAL "msvc")
            message(FATAL_ERROR
                "LANCE_C_CRT_STATIC is only supported for MSVC Rust targets")
        endif()
        # Apply the flag to the whole Rust target graph so build-script
        # dependencies also observe crt-static in CARGO_CFG_TARGET_FEATURE.
        corrosion_add_target_rustflags(
            lance_c
            "-Ctarget-feature=+crt-static"
        )
        # Corrosion discovers native-static-libs without target RUSTFLAGS, so
        # it exports the dynamic CRT flag even when the archive is built with
        # crt-static. Remove that stale directive rather than mixing MSVCRT
        # with the LIBCMT directives in the archive.
        get_target_property(_lance_c_static_link_options lance_c-static
            INTERFACE_LINK_OPTIONS)
        set(_lance_c_original_link_options "${_lance_c_static_link_options}")
        list(FILTER _lance_c_static_link_options
            EXCLUDE REGEX "^/defaultlib:[mM][sS][vV][cC][rR][tT][dD]?$")
        if("${_lance_c_static_link_options}" STREQUAL
                "${_lance_c_original_link_options}")
            message(FATAL_ERROR
                "Corrosion did not expose the expected /defaultlib:msvcrt "
                "interface on lance_c-static")
        endif()
        set_target_properties(lance_c-static PROPERTIES
            INTERFACE_LINK_OPTIONS "${_lance_c_static_link_options}")
    endif()
    # FindRust stores an inferred native target only in Corrosion's directory
    # scope. The cache entry is visible here for both inferred and explicit targets.
    if(Rust_CARGO_TARGET_CACHED MATCHES "apple-darwin$")
        # Corrosion selects its own linker, overriding Cargo's environment.
        # Keep the dylib export policy on the actual imported build target.
        corrosion_set_linker(lance_c
            "${CMAKE_CURRENT_LIST_DIR}/.github/scripts/limit-macos-exports.sh")
        corrosion_set_env_vars(lance_c
            "LANCE_C_MACOS_LINKER=${CMAKE_C_COMPILER}")
    elseif(Rust_CARGO_TARGET_CACHED MATCHES "x86_64-pc-windows-msvc$")
        corrosion_set_env_vars(lance_c
            "CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_LINKER=${CMAKE_CURRENT_LIST_DIR}/.github/scripts/limit-windows-exports.cmd"
            "LANCE_C_WINDOWS_MACHINE=X64")
    elseif(Rust_CARGO_TARGET_CACHED MATCHES "aarch64-pc-windows-msvc$")
        corrosion_set_env_vars(lance_c
            "CARGO_TARGET_AARCH64_PC_WINDOWS_MSVC_LINKER=${CMAKE_CURRENT_LIST_DIR}/.github/scripts/limit-windows-exports.cmd"
            "LANCE_C_WINDOWS_MACHINE=ARM64")
    endif()
endif()

# ─── Build-tree imported targets ─────────────────────────────────────────────
# These are for consumers using add_subdirectory(lance-c). Install-tree
# consumers get equivalent targets re-created in LanceCConfig.cmake.
# IMPORTED targets cannot be threaded through install(EXPORT), so the install
# path keeps a separate hand-written config template.

if(LANCE_C_USE_PREBUILT)
    if(WIN32)
        set(_lance_c_static_real "${LANCE_C_PREBUILT_DIR}/lib/lance_c${CMAKE_STATIC_LIBRARY_SUFFIX}")
        set(_lance_c_shared_real "${LANCE_C_PREBUILT_DIR}/lib/lance_c${CMAKE_SHARED_LIBRARY_SUFFIX}")
        set(_lance_c_import_real "${LANCE_C_PREBUILT_DIR}/lib/lance_c.dll${CMAKE_STATIC_LIBRARY_SUFFIX}")
    else()
        set(_lance_c_static_real "${LANCE_C_PREBUILT_DIR}/lib/liblance_c${CMAKE_STATIC_LIBRARY_SUFFIX}")
        set(_lance_c_shared_real "${LANCE_C_PREBUILT_DIR}/lib/liblance_c${CMAKE_SHARED_LIBRARY_SUFFIX}")
    endif()
    set(_lance_c_include_dirs "${LANCE_C_PREBUILT_DIR}/include")
else()
    # Corrosion replaces hyphens with underscores in target names (since Rust
    # 1.79 / Corrosion ports a Cargo behavior change), so 'lance-c' becomes
    # 'lance_c' and the generated targets are 'lance_c-static' and
    # 'lance_c-shared'.
    set(_lance_c_static_real lance_c-static)
    set(_lance_c_shared_real lance_c-shared)
    set(_lance_c_include_dirs "${CMAKE_CURRENT_LIST_DIR}/include")
endif()

# Static consumers need the platform link requirements. Shared libraries
# resolve these dependencies themselves. The same list is installed in
# LanceCConfig.cmake and formatted into lance-c.pc below.
lance_c_platform_dependencies(LANCE_C_PLATFORM_DEPS "${CMAKE_SYSTEM_NAME}")
lance_c_pkg_config_private_libs(LANCE_C_PC_LIBS_PRIVATE "${CMAKE_SYSTEM_NAME}")
add_library(LanceC_platform_deps INTERFACE)
target_link_libraries(LanceC_platform_deps INTERFACE ${LANCE_C_PLATFORM_DEPS})

# Public namespaced targets for build-tree (add_subdirectory) consumers.
# IMPORTED INTERFACE libraries because they aggregate other libs + flags but
# have no compile inputs of their own.
add_library(LanceC::lance_c_static INTERFACE IMPORTED GLOBAL)
set_target_properties(LanceC::lance_c_static PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_lance_c_include_dirs}"
    INTERFACE_COMPILE_FEATURES c_std_11
    INTERFACE_LINK_LIBRARIES "${_lance_c_static_real};LanceC_platform_deps")

add_library(LanceC::lance_c_shared INTERFACE IMPORTED GLOBAL)
set_target_properties(LanceC::lance_c_shared PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_lance_c_include_dirs}"
    INTERFACE_COMPILE_FEATURES c_std_11
    INTERFACE_LINK_LIBRARIES "${_lance_c_shared_real}")

if(LANCE_C_LINK STREQUAL "shared")
    add_library(LanceC::lance_c ALIAS LanceC::lance_c_shared)
else()
    add_library(LanceC::lance_c ALIAS LanceC::lance_c_static)
endif()

# Header-only C++ wrappers add their language requirement to the selected C
# library. C and C++ applications using only lance.h keep the C target.
add_library(LanceC::lance_cpp INTERFACE IMPORTED GLOBAL)
set_target_properties(LanceC::lance_cpp PROPERTIES
    INTERFACE_COMPILE_FEATURES cxx_std_20
    INTERFACE_LINK_LIBRARIES LanceC::lance_c)

# ─── Install rules ───────────────────────────────────────────────────────────
include(CMakePackageConfigHelpers)

# Headers (preserves the lance/ subdirectory).
install(DIRECTORY include/lance
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")

# Library artifacts. Corrosion 0.5.1's corrosion_install() only handles
# EXECUTABLE targets, so use vanilla install(FILES) with TARGET_FILE generator
# expressions that resolve to the .a / .dylib / .so paths cargo emits.
if(NOT LANCE_C_USE_PREBUILT)
    if(WIN32)
        # Rust emits `lance_c.dll` plus `lance_c.dll.lib` for a cdylib on
        # windows-msvc. Rename the import library explicitly so the installed
        # package config does not depend on rustc's artifact suffix.
        install(FILES $<TARGET_FILE:lance_c-static>
            DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RENAME lance_c${CMAKE_STATIC_LIBRARY_SUFFIX})
        install(FILES $<TARGET_FILE:lance_c-shared>
            DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RENAME lance_c${CMAKE_SHARED_LIBRARY_SUFFIX})
        install(FILES $<TARGET_LINKER_FILE:lance_c-shared>
            DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RENAME lance_c.dll${CMAKE_STATIC_LIBRARY_SUFFIX})
    else()
        install(FILES $<TARGET_FILE:lance_c-static>
            DESTINATION ${CMAKE_INSTALL_LIBDIR})
        install(FILES $<TARGET_FILE:lance_c-shared>
            DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
else()
    if(WIN32)
        install(FILES "${_lance_c_static_real}"
            OPTIONAL
            DESTINATION ${CMAKE_INSTALL_LIBDIR})
        install(FILES "${_lance_c_shared_real}"
            OPTIONAL
            DESTINATION ${CMAKE_INSTALL_LIBDIR})
        install(FILES "${_lance_c_import_real}"
            OPTIONAL
            DESTINATION ${CMAKE_INSTALL_LIBDIR})
    else()
        install(FILES "${_lance_c_static_real}"
            OPTIONAL
            DESTINATION ${CMAKE_INSTALL_LIBDIR})
        install(FILES "${_lance_c_shared_real}"
            OPTIONAL
            DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
endif()

# License.
install(FILES LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/lance-c)

# Windows archives are distributed as standalone SDK bundles.
if(WIN32)
    install(FILES README.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/lance-c)
endif()

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

# ─── pkg-config ──────────────────────────────────────────────────────────────
# LANCE_C_PC_LIBS_PRIVATE comes from cmake/platform-deps.cmake, the same list
# used by LanceC_platform_deps and the installed CMake package.
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/cmake/lance-c.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/lance-c.pc"
    @ONLY)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/lance-c.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# Do not introduce BUILD_TESTING or enable testing in an embedding project.
if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
    if(BUILD_TESTING)
        foreach(_test cargo_version consumer_version package_metadata target_interfaces)
            add_test(NAME cmake.${_test}
                COMMAND "${CMAKE_COMMAND}"
                    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/${_test}"
                    -P "${CMAKE_CURRENT_LIST_DIR}/cmake/test_${_test}.cmake")
        endforeach()
    endif()
endif()
