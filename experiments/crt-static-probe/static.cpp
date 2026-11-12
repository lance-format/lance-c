// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include <cstring>
#include <iostream>

extern "C" int probe_rust_crt_style();
extern "C" int probe_native_crt_style();
extern "C" char* probe_alloc_string();
extern "C" void probe_free_string(char*);
extern "C" char* probe_copy_string(const char*);
extern "C" int probe_catch_panic();

int main() {
    if (probe_rust_crt_style() != 1 || probe_native_crt_style() != 1) {
        std::cerr << "probe CRT configuration was not static\n";
        return 1;
    }
    char* owned = probe_alloc_string();
    if (owned == nullptr || std::strcmp(owned, "probe-owned") != 0) {
        std::cerr << "probe-owned allocation failed\n";
        return 2;
    }
    probe_free_string(owned);
    char* copied = probe_copy_string("host-owned");
    if (copied == nullptr || std::strcmp(copied, "host-owned") != 0) {
        std::cerr << "host input copy failed\n";
        return 3;
    }
    probe_free_string(copied);
    if (probe_catch_panic() != 1) {
        std::cerr << "probe panic was not caught\n";
        return 4;
    }
    std::cout << "static probe passed\n";
    return 0;
}
