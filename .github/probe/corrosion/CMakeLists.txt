cmake_minimum_required(VERSION 3.22)

project(LanceCExportProbe LANGUAGES C)

set(PROBE_CRATE_DIR "" CACHE PATH "Probe Cargo manifest directory")
set(LANCE_C_SOURCE_DIR "" CACHE PATH "Correction source checkout")
if(NOT PROBE_CRATE_DIR OR NOT LANCE_C_SOURCE_DIR)
    message(FATAL_ERROR "PROBE_CRATE_DIR and LANCE_C_SOURCE_DIR are required")
endif()

include(FetchContent)
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.5.1
)
FetchContent_MakeAvailable(Corrosion)

corrosion_import_crate(
    MANIFEST_PATH "${PROBE_CRATE_DIR}/Cargo.toml"
    CRATE_TYPES staticlib cdylib
    PROFILE release
    IMPORTED_CRATES PROBE_TARGETS
)

if(NOT PROBE_TARGETS)
    message(FATAL_ERROR "Corrosion imported no probe targets")
endif()

foreach(probe_target IN LISTS PROBE_TARGETS)
    message(STATUS "Corrosion imported target: ${probe_target}")
    if(Rust_CARGO_TARGET MATCHES "x86_64-pc-windows-msvc$")
        corrosion_set_env_vars(${probe_target}
            "CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_LINKER=${LANCE_C_SOURCE_DIR}/.github/scripts/limit-windows-exports.cmd"
            "LANCE_C_WINDOWS_MACHINE=X64")
    elseif(Rust_CARGO_TARGET MATCHES "aarch64-pc-windows-msvc$")
        corrosion_set_env_vars(${probe_target}
            "CARGO_TARGET_AARCH64_PC_WINDOWS_MSVC_LINKER=${LANCE_C_SOURCE_DIR}/.github/scripts/limit-windows-exports.cmd"
            "LANCE_C_WINDOWS_MACHINE=ARM64")
    endif()
    if(TARGET "${probe_target}-static")
        set(PROBE_STATIC_TARGET "${probe_target}-static")
    endif()
    if(TARGET "${probe_target}-shared")
        set(PROBE_SHARED_TARGET "${probe_target}-shared")
    endif()
endforeach()
if(NOT PROBE_STATIC_TARGET OR NOT PROBE_SHARED_TARGET)
    message(FATAL_ERROR "Corrosion probe static/shared targets were not created")
endif()

install(FILES $<TARGET_FILE:${PROBE_STATIC_TARGET}>
    DESTINATION lib
    RENAME lance_c.lib)
install(FILES $<TARGET_FILE:${PROBE_SHARED_TARGET}>
    DESTINATION lib
    RENAME lance_c.dll)
install(FILES $<TARGET_LINKER_FILE:${PROBE_SHARED_TARGET}>
    DESTINATION lib
    RENAME lance_c.dll.lib)
