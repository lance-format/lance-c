// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include <lance/lance.h>

#include <inttypes.h>
#include <stdio.h>

static void print_last_error(void) {
    LanceErrorCode code = lance_last_error_code();
    const char* message = lance_last_error_message();
    fprintf(stderr, "Lance error %d: %s\n", (int)code,
            message != NULL ? message : "(no message)");
    lance_free_string(message);
}

static void release_stream(struct ArrowArrayStream* stream) {
    if (stream->release != NULL) {
        stream->release(stream);
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <dataset_path>\n", argv[0]);
        return 2;
    }
    const char* dataset_path = argv[1];

    int status = 1;
    LanceDataset* dataset = NULL;
    LanceScanner* scanner = NULL;
    struct ArrowArrayStream stream = {0};

    dataset = lance_dataset_open(dataset_path, NULL, 0);
    if (dataset == NULL) {
        fprintf(stderr, "cannot open dataset %s: ", dataset_path);
        print_last_error();
        goto cleanup;
    }

    uint64_t version = lance_dataset_version(dataset);
    if (version == 0) {
        fprintf(stderr, "cannot read the version of %s: ", dataset_path);
        print_last_error();
        goto cleanup;
    }

    scanner = lance_scanner_new(dataset, NULL, NULL);
    if (scanner == NULL) {
        fprintf(stderr, "cannot create a scanner for %s: ", dataset_path);
        print_last_error();
        goto cleanup;
    }

    if (lance_scanner_to_arrow_stream(scanner, &stream) != 0) {
        fprintf(stderr, "cannot start the scan of %s: ", dataset_path);
        print_last_error();
        goto cleanup;
    }

    uint64_t rows = 0;
    for (;;) {
        struct ArrowArray batch = {0};
        int result = stream.get_next(&stream, &batch);
        if (result != 0) {
            const char* message = stream.get_last_error != NULL
                                      ? stream.get_last_error(&stream)
                                      : NULL;
            fprintf(stderr, "scan of %s failed: %s\n", dataset_path,
                    message != NULL ? message : "(no stream error message)");
            goto cleanup;
        }

        // A successful get_next with no release callback marks end of stream.
        if (batch.release == NULL) {
            break;
        }
        if (batch.length < 0) {
            fprintf(stderr, "scan of %s returned a negative batch length: %" PRId64 "\n",
                    dataset_path, (int64_t)batch.length);
            batch.release(&batch);
            goto cleanup;
        }

        rows += (uint64_t)batch.length;
        batch.release(&batch);
    }

    printf("rows: %" PRIu64 ", version: %" PRIu64 "\n", rows, version);
    status = 0;

cleanup:
    // Release everything acquired above in reverse order, including on failure.
    release_stream(&stream);
    lance_scanner_close(scanner);
    lance_dataset_close(dataset);
    return status;
}
