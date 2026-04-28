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
    # vcpkg does not provide one. Falls through to vcpkg_from_github + cmake +
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
