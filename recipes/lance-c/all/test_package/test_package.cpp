// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include <cstdio>

#include <lance/lance.hpp>

int main() {
    const char* msg = lance_last_error_message();
    if (msg != nullptr) {
        std::fprintf(stderr, "unexpected initial error: %s\n", msg);
        lance_free_string(msg);
        return 1;
    }

    // A NULL URI must fail through the C API instead of throwing across the
    // FFI boundary. This exercises the runtime, error path, and linkage.
    LanceDataset* dataset = lance_dataset_open(nullptr, nullptr, 0);
    if (dataset != nullptr) {
        std::fprintf(stderr, "opening a NULL URI unexpectedly succeeded\n");
        lance_dataset_close(dataset);
        return 1;
    }
    const char* error = lance_last_error_message();
    if (error == nullptr) {
        std::fprintf(stderr, "expected an error message after NULL URI\n");
        return 1;
    }
    std::printf("expected error: %s\n", error);
    lance_free_string(error);
    return 0;
}
