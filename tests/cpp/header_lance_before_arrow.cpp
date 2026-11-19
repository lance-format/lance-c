/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#include <lance/lance.hpp>
#include <arrow/c/abi.h>

static_assert(ARROW_FLAG_DICTIONARY_ORDERED == 1,
              "ARROW_FLAG_DICTIONARY_ORDERED must match Arrow");
static_assert(ARROW_FLAG_NULLABLE == 2,
              "ARROW_FLAG_NULLABLE must match Arrow");
static_assert(ARROW_FLAG_MAP_KEYS_SORTED == 4,
              "ARROW_FLAG_MAP_KEYS_SORTED must match Arrow");

int main() {
    ArrowArrayStream stream{};
    ArrowSchema schema{};
    ArrowArray array{};

    stream.private_data = &schema;
    schema.private_data = &array;
    array.private_data = &stream;

    return stream.release == nullptr && schema.release == nullptr &&
                   array.release == nullptr
               ? 0
               : 1;
}
