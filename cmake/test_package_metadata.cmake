# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Verify that every supported platform has one dependency list and that its
# pkg-config Libs.private form matches the CMake link items.
#
# Run with: cmake -P cmake/test_package_metadata.cmake

cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_LIST_DIR}/platform-deps.cmake")

function(_check_platform platform expected_deps expected_pc_libs)
    lance_c_platform_dependencies(_actual_deps "${platform}")
    if(NOT "${_actual_deps}" STREQUAL "${expected_deps}")
        message(FATAL_ERROR
            "[${platform}] dependencies differ:\n"
            "  expected: ${expected_deps}\n"
            "  actual:   ${_actual_deps}")
    endif()

    lance_c_pkg_config_private_libs(_actual_pc_libs "${platform}")
    if(NOT "${_actual_pc_libs}" STREQUAL "${expected_pc_libs}")
        message(FATAL_ERROR
            "[${platform}] pkg-config Libs.private differs:\n"
            "  expected: ${expected_pc_libs}\n"
            "  actual:   ${_actual_pc_libs}")
    endif()
    message(STATUS "[${platform}] ${_actual_pc_libs}")
endfunction()

_check_platform("Linux"
    "pthread;dl;m"
    "-lpthread -ldl -lm")

_check_platform("Darwin"
    "-framework CoreFoundation;-framework IOKit;-framework Security;-framework SystemConfiguration"
    "-framework CoreFoundation -framework IOKit -framework Security -framework SystemConfiguration")

_check_platform("Windows"
    "advapi32;bcrypt;dbghelp;kernel32;mswsock;ntdll;ole32;userenv;ws2_32"
    "-ladvapi32 -lbcrypt -ldbghelp -lkernel32 -lmswsock -lntdll -lole32 -luserenv -lws2_32")

message(STATUS "All platform dependency tests passed.")
