# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get

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
            self.options.rm_safe("fPIC")

    def layout(self):
        if self.options.from_source:
            cmake_layout(self)

    def source(self):
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
        license_src = self.source_folder if self.options.from_source else self.build_folder
        copy(self, "LICENSE",
             src=license_src,
             dst=os.path.join(self.package_folder, "licenses"),
             keep_path=False)
        if self.options.from_source:
            cmake = CMake(self)
            cmake.install()
        else:
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
