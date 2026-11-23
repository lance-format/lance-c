/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/* A C11 consumer that ignores a LANCE_NODISCARD result. Compiling this with
 * -Werror=unused-result must fail; the compile test asserts the diagnostic. */

#include <lance/lance.h>

int main(void) {
    lance_c_version();
    return 0;
}
