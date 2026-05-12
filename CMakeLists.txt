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
    include(FetchContent)
    FetchContent_Declare(
        Corrosion
        GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
        GIT_TAG v0.5.1
    )
    FetchContent_MakeAvailable(Corrosion)

    corrosion_import_crate(
        MANIFEST_PATH "${CMAKE_CURRENT_LIST_DIR}/Cargo.toml"
        CRATES lance-c
        CRATE_TYPES staticlib cdylib
        PROFILE release
    )
endif()

# Imported targets and install rules are added in subsequent tasks.

# ─── Build-tree imported targets ─────────────────────────────────────────────
# These are for consumers using add_subdirectory(lance-c). Install-tree
# consumers get equivalent targets re-created in LanceCConfig.cmake.
# IMPORTED targets cannot be threaded through install(EXPORT), so the install
# path keeps a separate hand-written config template.

if(LANCE_C_USE_PREBUILT)
    set(_lance_c_static_real "${LANCE_C_PREBUILT_DIR}/lib/liblance_c${CMAKE_STATIC_LIBRARY_SUFFIX}")
    set(_lance_c_shared_real "${LANCE_C_PREBUILT_DIR}/lib/liblance_c${CMAKE_SHARED_LIBRARY_SUFFIX}")
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
# IMPORTED INTERFACE libraries because they aggregate other libs + flags but
# have no compile inputs of their own.
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
    install(FILES $<TARGET_FILE:lance_c-static>
        DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES $<TARGET_FILE:lance_c-shared>
        DESTINATION ${CMAKE_INSTALL_LIBDIR})
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
