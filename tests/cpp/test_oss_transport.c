/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#include "lance/lance.h"
#include <stdio.h>
#include <string.h>

/* The Python harness serves a missing manifest on a local HTTP endpoint. */
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    const char *options[] = {
        "oss_endpoint", argv[1],
        "oss_region", "cn-test",
        "oss_access_key_id", "test-key",
        "oss_secret_access_key", "test-secret",
        "addressing_style", "path",
        NULL
    };
    LanceSession *session = NULL;
    LanceDataset *dataset = NULL;
    const char *uri = "oss://test-bucket/missing.lance";
    if (strcmp(argv[2], "shared") == 0) {
        session = lance_session_new(0, 0);
        if (session == NULL) return 3;
        dataset = lance_dataset_open_with_session(uri, options, 1, session);
    } else {
        dataset = lance_dataset_open(uri, options, 1);
    }
    /* The object does not exist, but the request must reach the HTTP server. */
    const char *error = lance_last_error_message();
    int failed = dataset != NULL || error == NULL;
    if (error != NULL) {
        fprintf(stderr, "%s\n", error);
        failed |= strstr(error, "default HTTP transport is not installed") != NULL;
        lance_free_string(error);
    }
    lance_dataset_close(dataset);
    lance_session_close(session);
    return failed ? 1 : 0;
}
