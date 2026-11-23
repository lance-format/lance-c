/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/* A C11 consumer that calls the deprecated API. Compiling this with
 * -Werror=deprecated-declarations must fail; the compile test asserts the
 * diagnostic. */

#include <lance/lance.h>

int main(void) {
    LanceFtsQueryContext* query =
        lance_dataset_prepare_fts_query(0, 0, 0, 0, 0);
    return query == 0 ? 0 : 1;
}
