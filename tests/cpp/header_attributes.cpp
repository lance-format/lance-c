/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/* A warning-as-error C++20 consumer: the annotated header stays valid and the
 * attribute macros are available. This translation unit is never linked. */

#include <lance/dataset.h>
#include <lance/version.h>

#ifndef LANCE_API
#error "lance.h must define LANCE_API"
#endif
#ifndef LANCE_DEPRECATED
#error "lance.h must define LANCE_DEPRECATED"
#endif
#ifndef LANCE_NODISCARD
#error "lance.h must define LANCE_NODISCARD"
#endif

int main() {
    const char* version = lance_c_version();
    return version == nullptr ? 0 : 1;
}
