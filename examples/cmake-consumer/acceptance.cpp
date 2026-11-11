// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

// Portable C++17 acceptance consumer for an installed LanceC package.
//
// It creates a dataset through the public C++ wrapper, closes and reopens it,
// reads every row through an ArrowArrayStream, verifies the Arrow release
// callbacks, exercises last-error message ownership, and uses an independent
// cloned dataset handle after the source handle has been destroyed.

#include <lance/lance.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int64_t kArrowFlagNullable = 2;

using StreamRelease = void (*)(ArrowArrayStream*);
using SchemaRelease = void (*)(ArrowSchema*);
using ArrayRelease = void (*)(ArrowArray*);

struct ReleaseCounts {
    uint64_t schema = 0;
    uint64_t arrays = 0;
    uint64_t stream = 0;
};

ReleaseCounts g_counts;
SchemaRelease g_original_schema_release = nullptr;
ArrayRelease g_original_array_release = nullptr;
StreamRelease g_original_stream_release = nullptr;
bool g_release_callback_error = false;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void counted_schema_release(ArrowSchema* schema) {
    ++g_counts.schema;
    auto original = g_original_schema_release;
    g_original_schema_release = nullptr;
    if (original == nullptr) {
        g_release_callback_error = true;
        return;
    }
    original(schema);
}

void counted_array_release(ArrowArray* array) {
    ++g_counts.arrays;
    if (g_original_array_release == nullptr) {
        g_release_callback_error = true;
        return;
    }
    auto original = g_original_array_release;
    g_original_array_release = nullptr;
    original(array);
}

void counted_stream_release(ArrowArrayStream* stream) {
    ++g_counts.stream;
    if (g_original_stream_release == nullptr) {
        g_release_callback_error = true;
        return;
    }
    auto original = g_original_stream_release;
    g_original_stream_release = nullptr;
    original(stream);
}

struct Int32Schema {
    ArrowSchema schema{};
    ArrowSchema child{};
    ArrowSchema* children[1] = {&child};
};

struct Int32Batch {
    ArrowArray array{};
    ArrowArray child{};
    ArrowArray* children[1] = {&child};
    const void* buffers[1] = {nullptr};
    const void* child_buffers[2] = {nullptr, nullptr};
    int32_t* values = nullptr;
};

void release_child_schema(ArrowSchema* schema) {
    schema->release = nullptr;
}

void release_child_array(ArrowArray* array) {
    array->release = nullptr;
}

void release_int32_schema(ArrowSchema* schema) {
    if (schema == nullptr || schema->release == nullptr) return;
    auto* holder = static_cast<Int32Schema*>(schema->private_data);
    if (holder != nullptr) {
        if (holder->child.release != nullptr) {
            holder->child.release(&holder->child);
        }
        delete holder;
    }
    schema->release = nullptr;
    schema->private_data = nullptr;
}

void release_int32_batch(ArrowArray* array) {
    if (array == nullptr || array->release == nullptr) return;
    auto* holder = static_cast<Int32Batch*>(array->private_data);
    if (holder != nullptr) {
        if (holder->child.release != nullptr) {
            holder->child.release(&holder->child);
        }
        std::free(holder->values);
        delete holder;
    }
    array->release = nullptr;
    array->private_data = nullptr;
}

Int32Schema* make_schema() {
    auto* holder = new Int32Schema;
    holder->child.format = "i";
    holder->child.name = "id";
    holder->child.flags = kArrowFlagNullable;
    holder->child.release = release_child_schema;

    holder->schema.format = "+s";
    holder->schema.n_children = 1;
    holder->schema.children = holder->children;
    holder->schema.release = release_int32_schema;
    holder->schema.private_data = holder;
    return holder;
}

Int32Batch* make_batch() {
    auto* holder = new Int32Batch;
    holder->values = static_cast<int32_t*>(std::malloc(sizeof(int32_t) * 4));
    if (holder->values == nullptr) {
        delete holder;
        fail("failed to allocate fixture values");
    }
    holder->values[0] = 10;
    holder->values[1] = 20;
    holder->values[2] = 30;
    holder->values[3] = 40;

    holder->child.length = 4;
    holder->child.n_buffers = 2;
    holder->child.buffers = holder->child_buffers;
    holder->child_buffers[1] = holder->values;
    holder->child.release = release_child_array;

    holder->array.length = 4;
    holder->array.n_buffers = 1;
    holder->array.buffers = holder->buffers;
    holder->array.n_children = 1;
    holder->array.children = holder->children;
    holder->array.release = release_int32_batch;
    holder->array.private_data = holder;
    return holder;
}

struct WriteStreamState {
    Int32Schema* schema = nullptr;
    Int32Batch* batch = nullptr;
    bool schema_taken = false;
    bool batch_taken = false;
};

int write_stream_get_schema(ArrowArrayStream* stream, ArrowSchema* out) {
    auto* state = static_cast<WriteStreamState*>(stream->private_data);
    if (state == nullptr || state->schema == nullptr || state->schema_taken) {
        return -1;
    }
    *out = state->schema->schema;
    state->schema = nullptr;
    state->schema_taken = true;
    return 0;
}

int write_stream_get_next(ArrowArrayStream* stream, ArrowArray* out) {
    auto* state = static_cast<WriteStreamState*>(stream->private_data);
    if (state == nullptr) return -1;
    if (state->batch == nullptr || state->batch_taken) {
        std::memset(out, 0, sizeof(*out));
        return 0;
    }
    *out = state->batch->array;
    state->batch = nullptr;
    state->batch_taken = true;
    return 0;
}

const char* write_stream_last_error(ArrowArrayStream*) {
    return nullptr;
}

void write_stream_release(ArrowArrayStream* stream) {
    auto* state = static_cast<WriteStreamState*>(stream->private_data);
    if (state == nullptr) return;
    if (state->schema != nullptr && state->schema->schema.release != nullptr) {
        state->schema->schema.release(&state->schema->schema);
        state->schema = nullptr;
    }
    if (state->batch != nullptr && state->batch->array.release != nullptr) {
        state->batch->array.release(&state->batch->array);
        state->batch = nullptr;
    }
    delete state;
    std::memset(stream, 0, sizeof(*stream));
}

void write_dataset(const std::string& uri) {
    auto* state = new WriteStreamState;
    state->schema = make_schema();
    state->batch = make_batch();

    ArrowArrayStream input{};
    input.get_schema = write_stream_get_schema;
    input.get_next = write_stream_get_next;
    input.get_last_error = write_stream_last_error;
    input.release = write_stream_release;
    input.private_data = state;

    auto dataset =
        lance::Dataset::write(uri, &input, lance::WriteMode::Create);
    if (dataset.count_rows() != 4) {
        fail("create returned an unexpected row count");
    }
    std::printf(
        "acceptance: create rows=%llu version=%llu\n",
        static_cast<unsigned long long>(dataset.count_rows()),
        static_cast<unsigned long long>(dataset.version()));
}

struct ScanSummary {
    uint64_t rows = 0;
    uint64_t batches = 0;
    std::vector<int32_t> values;
};

void verify_schema(const ArrowSchema& schema) {
    if (schema.n_children != 1 || schema.children == nullptr ||
        schema.children[0] == nullptr) {
        fail("scan schema did not contain exactly one field");
    }
    const ArrowSchema& field = *schema.children[0];
    if (field.name == nullptr || std::strcmp(field.name, "id") != 0) {
        fail("scan schema field name mismatch");
    }
    if (field.format == nullptr || std::strcmp(field.format, "i") != 0) {
        fail("scan schema field type mismatch");
    }
}

ScanSummary scan_dataset(const lance::Dataset& dataset) {
    g_counts = {};
    g_release_callback_error = false;
    auto scanner = dataset.scan();
    scanner.scan_in_order(true).batch_size(2);

    ArrowArrayStream raw{};
    scanner.to_arrow_stream(&raw);
    if (raw.release == nullptr) {
        fail("scanner returned a stream without a release callback");
    }
    g_original_stream_release = raw.release;
    raw.release = counted_stream_release;
    lance::ArrowStream stream(raw);

    ArrowSchema schema{};
    if (stream->get_schema == nullptr ||
        stream->get_schema(stream.get(), &schema) != 0) {
        fail("stream get_schema failed");
    }
    if (schema.release == nullptr) {
        fail("stream schema has no release callback");
    }
    verify_schema(schema);

    g_original_schema_release = schema.release;
    schema.release = counted_schema_release;
    counted_schema_release(&schema);
    if (schema.release != nullptr || g_release_callback_error) {
        fail("schema release callback failed");
    }

    ScanSummary summary;
    while (true) {
        ArrowArray batch{};
        if (stream->get_next == nullptr ||
            stream->get_next(stream.get(), &batch) != 0) {
            fail("stream get_next failed");
        }
        if (batch.release == nullptr) break;

        ++summary.batches;
        summary.rows += static_cast<uint64_t>(batch.length);
        if (batch.n_children != 1 || batch.children == nullptr ||
            batch.children[0] == nullptr) {
            fail("batch did not contain exactly one field");
        }
        const ArrowArray& ids = *batch.children[0];
        if (ids.n_buffers < 2 || ids.buffers == nullptr ||
            ids.buffers[1] == nullptr || ids.null_count != 0) {
            fail("id column buffers are invalid");
        }
        const auto* data = static_cast<const uint8_t*>(ids.buffers[1]);
        for (int64_t index = 0; index < batch.length; ++index) {
            int32_t value = 0;
            std::memcpy(
                &value,
                data + (ids.offset + index) *
                           static_cast<int64_t>(sizeof(value)),
                sizeof(value));
            summary.values.push_back(value);
        }

        g_original_array_release = batch.release;
        batch.release = counted_array_release;
        counted_array_release(&batch);
        if (batch.release != nullptr || g_release_callback_error) {
            fail("array release callback failed");
        }
    }

    stream.reset();
    if (g_counts.stream != 1) {
        fail("stream release callback was not invoked exactly once");
    }
    if (g_counts.schema != 1) {
        fail("schema release callback was not invoked exactly once");
    }
    if (g_counts.arrays != summary.batches) {
        fail("array release count does not match the batch count");
    }
    if (g_release_callback_error) {
        fail("an Arrow release callback was missing");
    }

    std::sort(summary.values.begin(), summary.values.end());
    if (summary.values != std::vector<int32_t>({10, 20, 30, 40})) {
        fail("scan returned unexpected values");
    }

    std::printf(
        "acceptance: scan rows=%llu batches=%llu array_releases=%llu "
        "schema_releases=%llu stream_releases=%llu\n",
        static_cast<unsigned long long>(summary.rows),
        static_cast<unsigned long long>(summary.batches),
        static_cast<unsigned long long>(g_counts.arrays),
        static_cast<unsigned long long>(g_counts.schema),
        static_cast<unsigned long long>(g_counts.stream));
    return summary;
}

void verify_last_error(const std::string& uri) {
    const std::string missing = uri + ".missing";
    LanceDataset* dataset = lance_dataset_open(missing.c_str(), nullptr, 0);
    if (dataset != nullptr) {
        lance_dataset_close(dataset);
        fail("opening a missing dataset unexpectedly succeeded");
    }

    const LanceErrorCode code = lance_last_error_code();
    if (code == LANCE_OK) {
        fail("missing dataset did not set an error code");
    }
    const char* message = lance_last_error_message();
    if (message == nullptr) {
        fail("missing dataset did not set an error message");
    }
    const std::string text(message);
    lance_free_string(message);
    if (text.empty()) {
        fail("last error message was empty");
    }
    if (lance_last_error_message() != nullptr) {
        fail("last error message was not consumed");
    }
    if (lance_last_error_code() != LANCE_OK) {
        fail("last error code was not cleared by message consumption");
    }

    std::printf(
        "acceptance: last_error code=%d message_bytes=%llu consumed=yes\n",
        static_cast<int>(code),
        static_cast<unsigned long long>(text.size()));
}

ScanSummary verify_clone(const std::string& uri) {
    lance::Dataset clone = [&uri] {
        lance::Dataset source = lance::Dataset::open(uri);
        return source.clone_handle();
    }();
    if (clone.count_rows() != 4) {
        fail("cloned handle returned an unexpected row count");
    }
    ScanSummary summary = scan_dataset(clone);
    std::printf(
        "acceptance: clone rows=%llu source_closed_before_use=yes\n",
        static_cast<unsigned long long>(summary.rows));
    return summary;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: acceptance <dataset-uri>\n");
        return 2;
    }

    try {
        const std::string uri = argv[1];
        write_dataset(uri);

        {
            auto dataset = lance::Dataset::open(uri);
            if (dataset.count_rows() != 4) {
                fail("reopened dataset returned an unexpected row count");
            }
            scan_dataset(dataset);
        }

        {
            auto dataset = lance::Dataset::open(uri);
            if (dataset.count_rows() != 4) {
                fail("second reopen returned an unexpected row count");
            }
        }

        verify_clone(uri);
        verify_last_error(uri);
        std::printf("acceptance: PASS\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "acceptance: FAIL: %s\n", error.what());
        return 1;
    }
}
