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
