/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#include "lance/lance.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition, message)                                               \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": "        \
                      << (message) << std::endl;                                \
            failures++;                                                         \
        }                                                                       \
    } while (0)

void release_schema(ArrowSchema *schema) {
    if (schema) schema->release = nullptr;
}

void release_array(ArrowArray *array) {
    if (array) array->release = nullptr;
}

struct SchemaFixture {
    ArrowSchema id{};
    ArrowSchema label{};
    ArrowSchema root{};
    ArrowSchema *children[2] = {&id, &label};

    SchemaFixture() {
        id.format = "i";
        id.name = "id";
        id.release = &release_schema;

        label.format = "u";
        label.name = "label";
        label.flags = 2;
        label.release = &release_schema;

        root.format = "+s";
        root.name = "";
        root.n_children = 2;
        root.children = children;
        root.release = &release_schema;
    }
};

struct BatchFixture {
    const int32_t ids[3] = {1, 2, 3};
    const int32_t offsets[4] = {0, 3, 6, 6};
    const uint8_t label_validity[1] = {0x03};
    const char labels[7] = "onetwo";

    const void *id_buffers[2]{};
    const void *label_buffers[3]{};
    const void *root_buffers[1]{};
    ArrowArray id{};
    ArrowArray label{};
    ArrowArray root{};
    ArrowArray *children[2] = {&id, &label};

    BatchFixture() {
        id_buffers[0] = nullptr;
        id_buffers[1] = ids;
        id.length = 3;
        id.n_buffers = 2;
        id.buffers = id_buffers;
        id.release = &release_array;

        label_buffers[0] = label_validity;
        label_buffers[1] = offsets;
        label_buffers[2] = labels;
        label.length = 3;
        label.null_count = 1;
        label.n_buffers = 3;
        label.buffers = label_buffers;
        label.release = &release_array;

        root_buffers[0] = nullptr;
        root.length = 3;
        root.n_buffers = 1;
        root.buffers = root_buffers;
        root.n_children = 2;
        root.children = children;
        root.release = &release_array;
    }
};

const char *label_at(const ArrowArray &array, int64_t index) {
    static char output[16];
    const auto *validity = static_cast<const uint8_t *>(array.buffers[0]);
    if (validity && ((validity[index / 8] >> (index % 8)) & 1) == 0) {
        return nullptr;
    }
    const auto *offsets = static_cast<const int32_t *>(array.buffers[1]);
    const auto *values = static_cast<const char *>(array.buffers[2]);
    const int32_t length = offsets[index + 1] - offsets[index];
    std::memcpy(output, values + offsets[index], static_cast<std::size_t>(length));
    output[length] = '\0';
    return output;
}

bool verify_stream(
    lance::ArrowStream &stream,
    int expected_rows,
    int first_id) {
    ArrowSchema schema{};
    if (stream->get_schema(stream.get(), &schema) != 0) return false;
    const bool valid_schema =
        std::strcmp(schema.format, "+s") == 0 && schema.n_children == 2;
    if (schema.release) schema.release(&schema);
    if (!valid_schema) return false;

    int rows = 0;
    while (true) {
        ArrowArray batch{};
        if (stream->get_next(stream.get(), &batch) != 0) return false;
        if (!batch.release) break;
        if (batch.n_children != 2) return false;
        const ArrowArray *ids = batch.children[0];
        const auto *values = static_cast<const int32_t *>(ids->buffers[1]);
        for (int64_t row = 0; row < batch.length; row++) {
            if (values[row] != first_id + rows) return false;
        }
        if (rows == 0 && batch.length > 2) {
            const char *second = label_at(*batch.children[1], 1);
            if (second == nullptr || std::strcmp(second, "two") != 0) return false;
            if (label_at(*batch.children[1], 2) != nullptr) return false;
        }
        rows += static_cast<int>(batch.length);
        if (batch.release) batch.release(&batch);
    }
    return rows == expected_rows;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const std::string base_uri = std::string("file://") + argv[1];

    SchemaFixture schema;
    lance::StorageOptions static_options;
    static_options["block_size"] = "4096";
    lance::StorageOptionsAccessor storage(static_options);
    lance::FileSession files(base_uri, &storage);

    const auto store_info = files.store_info();
    CHECK(store_info.block_size == 4096, "configured block size");
    CHECK(store_info.is_local, "local store");

    BatchFixture first;
    auto batch = lance::Batch::from_arrow(&first.root, &schema.root);
    CHECK(first.root.release == nullptr, "array consumed");
    CHECK(schema.root.release != nullptr, "schema remains borrowed");

    lance::FileWriterOptions writer_options;
    writer_options.keep_original_array = false;
    auto writer = files.open_writer("cpp/data.lance", &schema.root, writer_options);
    writer.write_batch(batch);
    writer.add_schema_metadata("owner", "lance-c");
    const std::vector<uint8_t> global_bytes{0x00, 0x67, 0xff};
    const uint32_t global_index = writer.add_global_buffer(global_bytes);
    CHECK(global_index == 1, "global buffer index");
    const auto summary = writer.finish();
    CHECK(summary.num_rows == 3, "write summary rows");
    CHECK(summary.size_bytes > 0, "write summary size");
    writer.abort();

    lance::FileReaderOptions reader_options;
    reader_options.columns = std::vector<std::string>{"id", "label"};
    auto reader = files.open_reader("cpp/data.lance", reader_options);
    CHECK(reader.num_rows() == 3, "reader rows");
    const auto metadata = reader.metadata();
    CHECK(metadata.info().num_rows == 3, "metadata rows");
    const auto metadata_schema = metadata.schema();
    CHECK(metadata_schema.get()->n_children == 2, "metadata schema children");
    const auto statistics = reader.statistics();
    CHECK(!statistics.columns().empty(), "column statistics");
    const auto global = reader.read_global_buffer(global_index);
    CHECK(global.bytes() == global_bytes, "global buffer bytes");

    lance::ArrowStream stream = reader.read_all(2, 2);
    reader.reset();
    files.reset();
    CHECK(verify_stream(stream, 3, 1), "stream survives reader/session close");

    auto range_reader = lance::FileSession(base_uri, &storage)
                            .open_reader("cpp/data.lance", reader_options);
    auto range = range_reader.read_range(1, 1, 1, 1);
    CHECK(verify_stream(range, 1, 2), "range stream");

    lance::FileSession reopened(base_uri, &storage);
    {
        BatchFixture abandoned_data;
        auto abandoned_batch =
            lance::Batch::from_arrow(&abandoned_data.root, &schema.root);
        auto abandoned =
            reopened.open_writer("cpp/abandoned.lance", &schema.root);
        abandoned.write_batch(abandoned_batch);
    }
    CHECK(!reopened.contains("cpp/abandoned.lance"),
          "destructor must not publish an unfinished writer");

    {
        auto lazy = reopened.open_writer("cpp/lazy.lance", nullptr);
        bool threw = false;
        try {
            (void)lazy.finish();
        } catch (const lance::Error &) {
            threw = true;
        }
        CHECK(threw, "lazy writer without data must fail finish");
    }

    std::unique_ptr<lance::StorageOptionsAccessor> dynamic_accessor;
    {
        lance::StorageOptionsProvider provider(
            "cpp-provider",
            []() {
                lance::StorageOptions options;
                options["block_size"] = "8192";
                return std::optional<lance::StorageOptions>(std::move(options));
            });
        dynamic_accessor = std::make_unique<lance::StorageOptionsAccessor>(
            std::nullopt, &provider);
    }
    const auto dynamic = dynamic_accessor->snapshot();
    CHECK(dynamic.has_value(), "provider snapshot has value");
    CHECK(dynamic && dynamic->at("block_size") == "8192",
          "provider snapshot value");

    return failures == 0 ? 0 : 1;
}
