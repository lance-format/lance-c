/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#include <arrow/c/abi.h>
#include <lance/lance.h>

#ifndef ARROW_C_STREAM_INTERFACE
#error "lance.h must define ARROW_C_STREAM_INTERFACE after a data-only Arrow header"
#endif

static int stream_get_schema(struct ArrowArrayStream *stream,
                             struct ArrowSchema *out) {
    (void)stream;
    (void)out;
    return 0;
}

static int stream_get_next(struct ArrowArrayStream *stream,
                           struct ArrowArray *out) {
    (void)stream;
    (void)out;
    return 0;
}

static const char *stream_get_last_error(struct ArrowArrayStream *stream) {
    (void)stream;
    return NULL;
}

static void stream_release(struct ArrowArrayStream *stream) {
    stream->release = NULL;
}

int main(void) {
    struct ArrowArrayStream stream = {stream_get_schema, stream_get_next,
                                      stream_get_last_error, stream_release,
                                      NULL};
    struct ArrowSchema schema = {0};
    struct ArrowArray array = {0};

    if (stream.get_schema(&stream, &schema) != 0) return 1;
    if (stream.get_next(&stream, &array) != 0) return 1;
    (void)stream.get_last_error(&stream);
    stream.release(&stream);

    return stream.release == NULL ? 0 : 1;
}
