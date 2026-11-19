/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#include <arrow/c/abi.h>
#include <lance/lance.hpp>

#ifndef ARROW_C_STREAM_INTERFACE
#error "lance.h must define ARROW_C_STREAM_INTERFACE after a data-only Arrow header"
#endif

static int stream_get_schema(ArrowArrayStream*, ArrowSchema*) { return 0; }

static int stream_get_next(ArrowArrayStream*, ArrowArray*) { return 0; }

static const char* stream_get_last_error(ArrowArrayStream*) { return nullptr; }

static void stream_release(ArrowArrayStream* stream) { stream->release = nullptr; }

int main() {
    ArrowArrayStream stream{stream_get_schema, stream_get_next,
                            stream_get_last_error, stream_release, nullptr};
    ArrowSchema schema{};
    ArrowArray array{};

    if (stream.get_schema(&stream, &schema) != 0) return 1;
    if (stream.get_next(&stream, &array) != 0) return 1;
    (void)stream.get_last_error(&stream);
    stream.release(&stream);

    return stream.release == nullptr ? 0 : 1;
}
