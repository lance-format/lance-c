/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file batch.h
 * @brief LanceBatch conversion to and from Arrow C Data Interface structs.
 */

#ifndef LANCE_BATCH_H
#define LANCE_BATCH_H

#include <stdint.h>
#include "lance/arrow_abi.h"
#include "lance/fwd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Batch (Arrow C Data Interface) ─── */

/**
 * Export a batch as Arrow C Data Interface structs.
 * @return 0 on success, -1 on error
 */
int32_t lance_batch_to_arrow(
    const LanceBatch* batch,
    struct ArrowArray* out_array,
    struct ArrowSchema* out_schema
);

/**
 * Import one Arrow record batch.
 *
 * A non-NULL `array` is consumed on every return, including validation and
 * conversion failures. The source slot is left in its canonical released
 * state. `schema` is borrowed and is never released or modified. `out` is
 * written only on success and is unchanged on failure.
 *
 * The complete schema is retained, including schema metadata, field metadata,
 * and extension metadata. A zero-column batch retains its row count.
 *
 * @return 0 on success, -1 on error.
 */
int32_t lance_batch_from_arrow(
    struct ArrowArray* array,
    const struct ArrowSchema* schema,
    LanceBatch** out
);

/** Free a batch handle. */
void lance_batch_free(LanceBatch* batch);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LANCE_BATCH_H */
