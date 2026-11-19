/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/* Compile as C and C++, with Lance first (0), Arrow first (1), or only
   Arrow's data interface before Lance (2). */
#if ARROW_INCLUDE_ORDER == 1 || ARROW_INCLUDE_ORDER == 2
#include "fixtures/arrow_c_data.h"
#endif
#if ARROW_INCLUDE_ORDER == 1
#include "fixtures/arrow_c_stream.h"
#endif

#ifdef TEST_LANCE_CPP_HEADER
#include <lance/lance.hpp>
#else
#include <lance/lance.h>
#endif

/* Check before the remaining Arrow headers can supply missing definitions. */
#ifndef ARROW_C_DATA_INTERFACE
#error "lance.h must define ARROW_C_DATA_INTERFACE"
#endif
#ifndef ARROW_C_STREAM_INTERFACE
#error "lance.h must define ARROW_C_STREAM_INTERFACE"
#endif

#ifdef __cplusplus
#define ASSERT_STATIC static_assert
#else
#define ASSERT_STATIC _Static_assert
#endif

ASSERT_STATIC(ARROW_FLAG_DICTIONARY_ORDERED == 1, "dictionary-ordered flag");
ASSERT_STATIC(ARROW_FLAG_NULLABLE == 2, "nullable flag");
ASSERT_STATIC(ARROW_FLAG_MAP_KEYS_SORTED == 4, "map-keys-sorted flag");
ASSERT_STATIC(sizeof(struct ArrowSchema) > 0, "complete ArrowSchema");
ASSERT_STATIC(sizeof(struct ArrowArray) > 0, "complete ArrowArray");
ASSERT_STATIC(sizeof(struct ArrowArrayStream) > 0, "complete ArrowArrayStream");

/* Neither interface may be redefined by a subsequent canonical header. */
#include "fixtures/arrow_c_data.h"
#include "fixtures/arrow_c_stream.h"
