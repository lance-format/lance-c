# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

# Map vcpkg triplets to our release tarball target names.
set(LANCE_C_TRIPLE_x64-linux           "x86_64-unknown-linux-gnu-debian12")
set(LANCE_C_TRIPLE_x64-linux-dynamic   "x86_64-unknown-linux-gnu-debian12")
set(LANCE_C_TRIPLE_arm64-linux         "aarch64-unknown-linux-gnu")
set(LANCE_C_TRIPLE_arm64-linux-dynamic "aarch64-unknown-linux-gnu")
set(LANCE_C_TRIPLE_arm64-osx           "aarch64-apple-darwin")
set(LANCE_C_TRIPLE_arm64-osx-dynamic   "aarch64-apple-darwin")

# SHA512s of each prebuilt tarball. Updated by the release script after
# .github/workflows/release.yml publishes a tag.
set(LANCE_C_SHA512_x86_64-unknown-linux-gnu-debian12 "5aba369dd70b244d905298eacac93a3f5ddaed4b3df43e6a9fe0184303f1ce06f60794da6ea6288f15ade20f29120a8683bc1fbfdcef65b06f1e1e7944493595")
set(LANCE_C_SHA512_aarch64-unknown-linux-gnu         "0a8c91a07ca82a15a635a2110cd70ed85bc3eaf4009284a8509395f9da4ef9fa85510f68cddec466a3609b22c5c2c56c0e4e004279dbd8d074d2c61097f30ee9")
set(LANCE_C_SHA512_aarch64-apple-darwin              "488463b062e25a100b56837613b37efd332e031a82a669eb2a882435d72d11e72dd3c966f9e53ca7197845126cecf2a64be722086f45f83deb2d8171a19d542b")
set(LANCE_C_SHA512_SOURCE                            "0262e1df6dddb9caac5bd375af6f0bf2860138599edd8a67e216fb53d91acb14fd48032409f8fe37bd0080119599a65b259795a53df76e88031af7314efd3b3b")
set(LANCE_C_SHA512_CORROSION                         "1f9337c779d619af3e69b0b0929d97150d289900d8b0c1a63c2faa7443c9053684c70dec6f27ee020c8fad33d597438293b9bc3c5dfd31fbeb406929bbca3ac5")

# The release publishes one release-mode archive. Reusing it for debug avoids
# a second full cargo build while keeping the imported target configuration-
# independent.
set(VCPKG_BUILD_TYPE release)
set(VCPKG_POLICY_MISMATCHED_NUMBER_OF_BINARIES enabled)

if("from-source" IN_LIST FEATURES)
    # Source build via cargo. The Rust toolchain stays a documented host
    # requirement because vcpkg does not provide one.
    vcpkg_from_github(
        OUT_SOURCE_PATH SOURCE_PATH
        REPO lance-format/lance-c
        REF "v${VERSION}"
        SHA512 "${LANCE_C_SHA512_SOURCE}"
    )
    # CMakeLists.txt declares Corrosion through FetchContent. vcpkg configures
    # with FETCHCONTENT_FULLY_DISCONNECTED=ON, so provide the pinned source
    # archive explicitly instead of allowing configure-time network access.
    vcpkg_from_github(
        OUT_SOURCE_PATH CORROSION_SOURCE_PATH
        REPO corrosion-rs/corrosion
        REF v0.5.1
        SHA512 "${LANCE_C_SHA512_CORROSION}"
    )
    # lance-encoding's build script imports protobuf's well-known .proto files
    # through protoc. This dependency is only needed for source builds.
    vcpkg_add_to_path(PREPEND "${CURRENT_HOST_INSTALLED_DIR}/tools/protobuf")
    if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
        set(LANCE_C_LINK shared)
    else()
        set(LANCE_C_LINK static)
    endif()
    vcpkg_cmake_configure(
        SOURCE_PATH "${SOURCE_PATH}"
        OPTIONS
            -DLANCE_C_USE_PREBUILT=OFF
            -DLANCE_C_LINK=${LANCE_C_LINK}
            -DFETCHCONTENT_SOURCE_DIR_CORROSION=${CORROSION_SOURCE_PATH}
    )
    vcpkg_cmake_install()
    # The CMake install includes both linkages. Keep the layout consistent with
    # the triplet that vcpkg is building.
    if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
        file(REMOVE
            "${CURRENT_PACKAGES_DIR}/lib/liblance_c.so"
            "${CURRENT_PACKAGES_DIR}/lib/liblance_c.dylib")
    else()
        file(REMOVE "${CURRENT_PACKAGES_DIR}/lib/liblance_c.a")
    endif()
else()
    set(_TRIPLE "${LANCE_C_TRIPLE_${TARGET_TRIPLET}}")
    if(NOT _TRIPLE)
        message(FATAL_ERROR
            "lance-c ${VERSION} does not publish a prebuilt archive for triplet "
            "'${TARGET_TRIPLET}'. Re-run with the 'from-source' feature to build "
            "the v${VERSION} source tag (requires Rust, CMake, and protoc).")
    endif()

    set(_SHA512 "${LANCE_C_SHA512_${_TRIPLE}}")
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
    else()
        file(GLOB _shared "${SOURCE_PATH}/lib/liblance_c.so*"
                          "${SOURCE_PATH}/lib/liblance_c.dylib")
        file(INSTALL ${_shared} DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
    endif()

    file(INSTALL "${SOURCE_PATH}/lib/cmake/" DESTINATION "${CURRENT_PACKAGES_DIR}/lib/cmake")
    file(INSTALL "${SOURCE_PATH}/lib/pkgconfig/" DESTINATION "${CURRENT_PACKAGES_DIR}/lib/pkgconfig")
    file(INSTALL "${SOURCE_PATH}/share/licenses/lance-c/LICENSE"
        DESTINATION "${CURRENT_PACKAGES_DIR}/share/licenses/lance-c")
endif()

# vcpkg expects the copyright file under share/${PORT}, independent of the
# archive's own license layout.
file(INSTALL "${CURRENT_PACKAGES_DIR}/share/licenses/lance-c/LICENSE"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

vcpkg_cmake_config_fixup(PACKAGE_NAME LanceC CONFIG_PATH lib/cmake/LanceC)
vcpkg_fixup_pkgconfig()

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
