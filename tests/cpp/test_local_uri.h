/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#ifndef LANCE_C_TESTS_TEST_LOCAL_URI_H
#define LANCE_C_TESTS_TEST_LOCAL_URI_H

#include <stddef.h>
#include <stdio.h>

/* Write a `file://` URI for a local filesystem path.
 *
 * A Unix path starts with `/`, so `file://%s` yields `file:///tmp/data`.
 * A Windows path starts with a drive letter and uses backslashes. The URL
 * parser reads `file://C:\data` as host `C`, so Windows needs the drive-letter
 * form `file:///C:/data` instead.
 */
static inline void lance_test_local_uri(char *buffer, size_t size,
                                       const char *path) {
#if defined(_WIN32)
    int written = snprintf(buffer, size, "file:///");
    if (written < 0 || (size_t)written >= size) {
        buffer[0] = '\0';
        return;
    }
    size_t used = (size_t)written;
    for (const char *cursor = path; *cursor != '\0'; ++cursor) {
        if (used + 1 >= size) {
            buffer[0] = '\0';
            return;
        }
        buffer[used++] = (*cursor == '\\') ? '/' : *cursor;
    }
    buffer[used] = '\0';
#else
    int written = snprintf(buffer, size, "file://%s", path);
    if (written < 0 || (size_t)written >= size) {
        buffer[0] = '\0';
    }
#endif
}

#if defined(__cplusplus)
#include <string>

/* C++ overload returning the same URI as a std::string. */
static inline std::string lance_test_local_uri(const std::string &path) {
#if defined(_WIN32)
    std::string uri = "file:///";
    for (char character : path) {
        uri.push_back(character == '\\' ? '/' : character);
    }
    return uri;
#else
    return "file://" + path;
#endif
}
#endif

#endif
