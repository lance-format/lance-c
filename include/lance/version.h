/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file version.h
 * @brief lance-c binding version macros and the runtime version accessor.
 */

#ifndef LANCE_VERSION_H
#define LANCE_VERSION_H

#include "lance/attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Binding version ─── */

/* Version of the lance-c binding, synchronized with Cargo.toml. These macros
 * describe this C API release, not the Lance core dataset/file writer version
 * or a Git pin. */
#define LANCE_C_VERSION_MAJOR 0
#define LANCE_C_VERSION_MINOR 2
#define LANCE_C_VERSION_PATCH 1
/* Pre-release suffix without the leading '-'; empty for a stable release. */
#define LANCE_C_VERSION_PRE "beta.9"
/* Full Cargo package version, including any pre-release or build suffix. */
#define LANCE_C_VERSION_STRING "0.2.1-beta.9"
#define LANCE_C_VERSION_ENCODE(major, minor, patch) \
    (((major) * 1000000) + ((minor) * 1000) + (patch))
/* Numeric form for #if comparisons. The pre-release suffix is not encoded, so
 * a pre-release compares equal to its base release; use LANCE_C_VERSION_PRE or
 * LANCE_C_VERSION_STRING when the exact release identity matters. Major,
 * minor, and patch must each be less than 1000. */
#define LANCE_C_VERSION \
    LANCE_C_VERSION_ENCODE(LANCE_C_VERSION_MAJOR, LANCE_C_VERSION_MINOR, LANCE_C_VERSION_PATCH)

/**
 * Return the version of the loaded lance-c library as a NUL-terminated string,
 * including any pre-release suffix (for example "0.2.0-beta.1").
 *
 * The returned string is owned by the library and remains valid for the
 * process lifetime; do not free it. Compare it with LANCE_C_VERSION_STRING
 * from the headers used at compile time to detect a header/library mismatch.
 *
 * This reports the lance-c binding release, not the Lance core writer version
 * or a Git revision.
 */
LANCE_NODISCARD LANCE_API const char* lance_c_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LANCE_VERSION_H */
