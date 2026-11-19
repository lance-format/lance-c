/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#include <arrow/c/abi.h>
#include <lance/lance.h>

_Static_assert(ARROW_FLAG_DICTIONARY_ORDERED == 1,
               "ARROW_FLAG_DICTIONARY_ORDERED must match Arrow");
_Static_assert(ARROW_FLAG_NULLABLE == 2,
               "ARROW_FLAG_NULLABLE must match Arrow");
_Static_assert(ARROW_FLAG_MAP_KEYS_SORTED == 4,
               "ARROW_FLAG_MAP_KEYS_SORTED must match Arrow");

int main(void) {
    struct ArrowArrayStream stream = {0};
    struct ArrowSchema schema = {0};
    struct ArrowArray array = {0};

    stream.private_data = &schema;
    schema.private_data = &array;
    array.private_data = &stream;

    return stream.release == NULL && schema.release == NULL &&
                   array.release == NULL
               ? 0
               : 1;
}
