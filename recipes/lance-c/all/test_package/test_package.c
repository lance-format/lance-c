// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include <stdio.h>

#include <lance/lance.h>

int main(void) {
    const char* message = lance_last_error_message();
    if (message != NULL) {
        fprintf(stderr, "unexpected initial error: %s\n", message);
        lance_free_string(message);
        return 1;
    }

    LanceDataset* dataset = lance_dataset_open(NULL, NULL, 0);
    if (dataset != NULL) {
        fprintf(stderr, "opening a NULL URI unexpectedly succeeded\n");
        lance_dataset_close(dataset);
        return 1;
    }
    const char* error = lance_last_error_message();
    if (error == NULL) {
        fprintf(stderr, "expected an error message after NULL URI\n");
        return 1;
    }
    printf("expected C API error: %s\n", error);
    lance_free_string(error);
    return 0;
}
