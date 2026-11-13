/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#include "lance/lance.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sys/resource.h>
#endif

lance::FileSession open_file_test_session(
    const std::string& base_uri,
    const lance::StorageOptionsAccessor& storage);

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

struct BinaryFixture {
    const char values[9] = "skipbeta";
    const int32_t offsets[4] = {0, 4, 4, 8};
    const uint8_t validity[1] = {0x05};
    const void *buffers[3] = {validity, offsets, values};
    ArrowArray payloads{};
    ArrowSchema schema{};

    BinaryFixture() {
        payloads.length = 2;
        payloads.offset = 1;
        payloads.n_buffers = 3;
        payloads.buffers = buffers;
        payloads.release = &release_array;

        schema.format = "z";
        schema.name = "payload";
        schema.flags = 2;
        schema.release = &release_schema;
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

const char *const blob_v2_descriptor_children[] = {
    "kind", "position", "size", "blob_id", "blob_uri"};

bool blob_descriptor_schema(const ArrowSchema *field, const char *label) {
    if (!field || !field->format || std::strcmp(field->format, "+s") != 0) {
        std::cerr << "FAIL " << label << " is not a struct" << std::endl;
        return false;
    }
    if (field->n_children != 5) {
        std::cerr << "FAIL " << label << " children=" << field->n_children
                  << std::endl;
        return false;
    }
    for (int64_t index = 0; index < field->n_children; index++) {
        const ArrowSchema *child = field->children[index];
        if (!child->name ||
            std::strcmp(child->name, blob_v2_descriptor_children[index]) != 0) {
            std::cerr << "FAIL " << label << " child[" << index << "]="
                      << (child->name ? child->name : "null") << std::endl;
            return false;
        }
    }
    return true;
}

bool struct_is_valid(const ArrowArray &array, int64_t row) {
    const auto *validity = static_cast<const uint8_t *>(array.buffers[0]);
    return validity == nullptr ||
           ((validity[row / 8] >> (row % 8)) & 1) != 0;
}

int64_t descriptor_size_at(const ArrowArray &descriptor, int64_t row) {
    const auto *values =
        static_cast<const int64_t *>(descriptor.children[2]->buffers[1]);
    return values[row];
}

bool verify_released_blob_v2_stream(lance::ArrowStream &stream) {
    ArrowSchema schema{};
    if (stream->get_schema(stream.get(), &schema) != 0) return false;
    const bool valid_schema =
        std::strcmp(schema.format, "+s") == 0 && schema.n_children == 4;
    if (schema.release) schema.release(&schema);
    if (!valid_schema) return false;

    static const int32_t expected_ids[] = {10, 20, 30};
    int rows = 0;
    bool blob_a_valid[3] = {false, false, false};
    bool blob_b_valid[3] = {false, false, false};
    bool payload_valid[3] = {false, false, false};
    int64_t blob_a_size[3] = {-1, -1, -1};
    while (true) {
        ArrowArray batch{};
        if (stream->get_next(stream.get(), &batch) != 0) return false;
        if (!batch.release) break;
        if (batch.n_children != 4) {
            batch.release(&batch);
            return false;
        }
        const auto *ids =
            static_cast<const int32_t *>(batch.children[0]->buffers[1]);
        const ArrowArray &blob_a = *batch.children[1];
        const ArrowArray &blob_b = *batch.children[2];
        const ArrowArray &payload = *batch.children[3]->children[0];
        for (int64_t row = 0; row < batch.length; row++) {
            const int index = rows + static_cast<int>(row);
            if (index >= 3 || ids[row] != expected_ids[index]) {
                batch.release(&batch);
                return false;
            }
            blob_a_valid[index] = struct_is_valid(blob_a, row);
            blob_b_valid[index] = struct_is_valid(blob_b, row);
            payload_valid[index] = struct_is_valid(payload, row);
            if (blob_a_valid[index]) {
                blob_a_size[index] = descriptor_size_at(blob_a, row);
            }
        }
        rows += static_cast<int>(batch.length);
        batch.release(&batch);
    }
    return rows == 3 && blob_a_valid[0] && !blob_a_valid[1] &&
           blob_a_valid[2] && blob_b_valid[0] && blob_b_valid[1] &&
           !blob_b_valid[2] && payload_valid[0] && !payload_valid[1] &&
           payload_valid[2] && blob_a_size[0] == 5 && blob_a_size[2] == 0;
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
            if (values[row] != first_id + rows + row) return false;
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

template <typename Call>
bool rejects_reader_stream_options(Call call, const char *option) {
    try {
        call();
    } catch (const lance::Error &error) {
        const std::string message = error.what();
        return error.code == LANCE_ERR_INVALID_ARGUMENT &&
               message.find(option) != std::string::npos &&
               message.find("greater than 0") != std::string::npos;
    }
    return false;
}

#if defined(__unix__) || defined(__APPLE__)
std::vector<uint8_t> read_local_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::filesystem::path object_path_on_disk(const std::string& object_path) {
    if (!object_path.empty() && object_path.front() == '/') {
        return std::filesystem::path(object_path);
    }
    return std::filesystem::path("/") / object_path;
}

int test_transfer_write_failures(const std::string& base_path) {
    const auto directory = std::filesystem::path(base_path) / "write-failures";
    std::filesystem::create_directories(directory);
    const std::string expected(4096, 'x');
    const auto download_path = directory / "download.bin";
    const auto upload_path = directory / "upload.bin";
    for (const auto& path : {download_path, upload_path}) {
        std::ofstream output(path, std::ios::binary);
        output << expected;
        output.close();
        CHECK(output.good(), "create complete write-failure fixture");
    }
    if (failures) return 1;
    lance::FileSession files("file://" + directory.string());

    struct rlimit original{};
    if (getrlimit(RLIMIT_FSIZE, &original) != 0) return 2;
    struct rlimit limited = original;
    limited.rlim_cur = 1024;
    const auto previous_handler = std::signal(SIGXFSZ, SIG_IGN);
    if (previous_handler == SIG_ERR) return 2;
    if (setrlimit(RLIMIT_FSIZE, &limited) != 0) return 2;

    LanceErrorCode download_error = LANCE_OK;
    try {
        files.download_file("download.bin", download_path.string());
    } catch (const lance::Error& error) {
        download_error = error.code;
    }
    LanceErrorCode upload_error = LANCE_OK;
    try {
        files.upload_file(upload_path.string(), "upload.bin");
    } catch (const lance::Error& error) {
        upload_error = error.code;
    }

    auto failed_writer =
        files.open_packed_blob_writer("blob-write-failure.lance", 73);
    bool blob_writes_threw = false;
    LanceErrorCode blob_write_error = LANCE_OK;
    try {
        failed_writer.write_blob(
            std::vector<uint8_t>(16 * 1024, static_cast<uint8_t>('x')));
        failed_writer.write_blob(std::vector<uint8_t>{'y'});
    } catch (const lance::Error& error) {
        blob_writes_threw = true;
        blob_write_error = error.code;
    }
    const auto sentinel = reinterpret_cast<void*>(
        static_cast<uintptr_t>(0x5a5a));
    ArrowArray failed_array{};
    failed_array.private_data = sentinel;
    ArrowSchema failed_schema{};
    failed_schema.private_data = sentinel;
    bool blob_finish_threw = false;
    try {
        failed_writer.finish_array(
            "blob", &failed_array, &failed_schema);
    } catch (const lance::Error& error) {
        blob_finish_threw = true;
        std::cout << "packed blob finish error=" << error.code << ": "
                  << error.what() << std::endl;
    }
    std::cout << "packed blob write failure: writes_threw="
              << blob_writes_threw << ", write_error=" << blob_write_error
              << ", finish_threw=" << blob_finish_threw << std::endl;
    CHECK(!blob_writes_threw,
          "buffered packed writes defer the file-size failure to finish");
    CHECK(blob_finish_threw, "packed finish after write failure reports error");
    CHECK(failed_array.private_data == sentinel &&
              failed_array.release == nullptr,
          "failed packed finish leaves array output untouched");
    CHECK(failed_schema.private_data == sentinel &&
              failed_schema.release == nullptr,
          "failed packed finish leaves schema output untouched");
    if (failed_array.release) failed_array.release(&failed_array);
    if (failed_schema.release) failed_schema.release(&failed_schema);

    if (setrlimit(RLIMIT_FSIZE, &original) != 0) return 2;
    std::signal(SIGXFSZ, previous_handler);
    const auto read_file = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const auto downloaded = read_file(download_path);
    const auto uploaded = read_file(upload_path);
    std::cout << "buffered transfer failures: download_error=" << download_error
              << ", download_bytes=" << downloaded.size()
              << ", upload_error=" << upload_error
              << ", upload_bytes=" << uploaded.size() << std::endl;
    CHECK(download_error == LANCE_ERR_IO, "download reports buffered I/O failure");
    CHECK(downloaded == expected, "failed download preserves original bytes");
    CHECK(upload_error == LANCE_ERR_IO, "upload reports buffered I/O failure");
    CHECK(uploaded == expected, "failed upload preserves original bytes");
    return failures == 0 ? 0 : 1;
}
#endif

}  // namespace

int main(int argc, char **argv) {
#if defined(__unix__) || defined(__APPLE__)
    if (argc == 3 && std::string(argv[2]) == "write-failure") {
        return test_transfer_write_failures(argv[1]);
    }
#endif
    if (argc != 2) return 2;
    const std::string base_uri = std::string("file://") + argv[1];

    SchemaFixture schema;
    lance::StorageOptions static_options;
    static_options["block_size"] = "4096";
    lance::StorageOptionsAccessor storage(static_options);
    auto files = open_file_test_session(base_uri, storage);

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

    struct ZeroOptionCase {
        uint32_t batch_size;
        uint32_t batch_readahead;
        const char *option;
    };
    const ZeroOptionCase zero_cases[] = {
        {0, 2, "batch_size"},
        {2, 0, "batch_readahead"},
        {0, 0, "batch_size"},
    };
    const std::vector<uint32_t> indices{1, 2};
    for (const auto &zero : zero_cases) {
        CHECK(rejects_reader_stream_options(
                  [&] { (void)range_reader.read_all(zero.batch_size,
                                                    zero.batch_readahead); },
                  zero.option),
              "read_all rejects zero stream options");
        CHECK(rejects_reader_stream_options(
                  [&] { (void)range_reader.read_range(1, 1, zero.batch_size,
                                                      zero.batch_readahead); },
                  zero.option),
              "read_range rejects zero stream options");
        CHECK(rejects_reader_stream_options(
                  [&] { (void)range_reader.read_indices(
                            indices, zero.batch_size, zero.batch_readahead); },
                  zero.option),
              "read_indices rejects zero stream options");
    }

    auto index_stream = range_reader.read_indices(indices, 2, 1);
    CHECK(verify_stream(index_stream, 2, 2), "index stream");

    auto empty_range = range_reader.read_range(0, 0, 2, 1);
    CHECK(verify_stream(empty_range, 0, 1), "empty range stream");
    auto empty_indices = range_reader.read_indices({}, 2, 1);
    CHECK(verify_stream(empty_indices, 0, 1), "empty indices stream");
    CHECK(rejects_reader_stream_options(
              [&] { (void)range_reader.read_range(0, 0, 0, 1); },
              "batch_size"),
          "empty read_range rejects zero batch_size");
    CHECK(rejects_reader_stream_options(
              [&] { (void)range_reader.read_indices({}, 2, 0); },
              "batch_readahead"),
          "empty read_indices rejects zero readahead");

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

    static_assert(!std::is_copy_constructible_v<lance::PackedBlobWriter>);
    static_assert(!std::is_copy_constructible_v<lance::DedicatedBlobWriter>);

    auto packed = [&]() {
        lance::FileSession owner(base_uri, &storage);
        return owner.open_packed_blob_writer("cpp/packed-blob.lance", 71);
    }();
    auto packed_writer = std::move(packed);
    CHECK(packed_writer.blob_id() == 71, "packed blob writer id");
    const auto packed_path = packed_writer.path();
    BinaryFixture payloads;
    packed_writer.write_blobs(&payloads.payloads, &payloads.schema);
    CHECK(payloads.payloads.release == nullptr, "packed payloads consumed");
    CHECK(payloads.schema.release != nullptr, "packed schema borrowed");
    packed_writer.write_blob(nullptr, 0);
    ArrowArray packed_array{};
    ArrowSchema packed_schema{};
    packed_writer.finish_array("blob", &packed_array, &packed_schema);
    CHECK(packed_array.release != nullptr, "packed prepared array");
    CHECK(packed_schema.release != nullptr, "packed prepared field");
    CHECK(packed_array.length == 3, "packed prepared rows");
    CHECK(packed_schema.name != nullptr &&
              std::string(packed_schema.name) == "blob",
          "packed prepared field name");
#if defined(__unix__) || defined(__APPLE__)
    CHECK(read_local_file(object_path_on_disk(packed_path)) ==
              (std::vector<uint8_t>{'b', 'e', 't', 'a'}),
          "packed sidecar survives session close");
#endif
    bool packed_repeated_finish_threw = false;
    try {
        (void)packed_writer.blob_id();
    } catch (const lance::Error& error) {
        packed_repeated_finish_threw =
            error.code == LANCE_ERR_INVALID_ARGUMENT &&
            std::string(error.what()).find("already finished") !=
                std::string::npos;
    }
    CHECK(packed_repeated_finish_threw,
          "finished packed writer reports its state");
    if (packed_array.release) packed_array.release(&packed_array);
    if (packed_schema.release) packed_schema.release(&packed_schema);

    auto dedicated = [&]() {
        lance::FileSession owner(base_uri, &storage);
        return owner.open_dedicated_blob_writer("cpp/dedicated-blob.lance", 72);
    }();
    auto dedicated_writer = std::move(dedicated);
    CHECK(dedicated_writer.blob_id() == 72, "dedicated blob writer id");
    const auto dedicated_path = dedicated_writer.path();
    dedicated_writer.write(std::vector<uint8_t>{'a', 'l', 'p', 'h', 'a'});
    dedicated_writer.write(std::vector<uint8_t>{});
    dedicated_writer.write(std::vector<uint8_t>{'o', 'm', 'e', 'g', 'a'});
    ArrowArray dedicated_array{};
    ArrowSchema dedicated_schema{};
    dedicated_writer.finish_array("blob", &dedicated_array, &dedicated_schema);
    CHECK(dedicated_array.release != nullptr, "dedicated prepared array");
    CHECK(dedicated_schema.release != nullptr, "dedicated prepared field");
    CHECK(dedicated_array.length == 1, "dedicated prepared rows");
#if defined(__unix__) || defined(__APPLE__)
    CHECK(read_local_file(object_path_on_disk(dedicated_path)) ==
              (std::vector<uint8_t>(
                  {'a', 'l', 'p', 'h', 'a', 'o', 'm', 'e', 'g', 'a'})),
          "dedicated writes concatenate");
#endif
    bool dedicated_write_after_finish_threw = false;
    try {
        dedicated_writer.write(std::vector<uint8_t>{'x'});
    } catch (const lance::Error& error) {
        dedicated_write_after_finish_threw =
            error.code == LANCE_ERR_INVALID_ARGUMENT &&
            std::string(error.what()).find("already finished") !=
                std::string::npos;
    }
    CHECK(dedicated_write_after_finish_threw,
          "finished dedicated writer reports its state");
    if (dedicated_array.release) dedicated_array.release(&dedicated_array);
    if (dedicated_schema.release) dedicated_schema.release(&dedicated_schema);

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

    {
        lance::FileSession blob_session(base_uri, &storage);
        lance::FileReader blob_reader = blob_session.open_reader(
            "blob_v2_raw_reader/blob_v2_raw.lance", lance::FileReaderOptions{});
        CHECK(blob_reader.num_rows() == 3, "released Blob v2 raw rows");
        const auto blob_metadata = blob_reader.metadata();
        const auto blob_schema = blob_metadata.schema();
        CHECK(blob_schema.get()->n_children == 4,
              "released Blob v2 raw root fields");
        CHECK(std::strcmp(blob_schema.get()->children[1]->name, "blob_a") == 0,
              "released Blob v2 raw top-level field");
        CHECK(blob_descriptor_schema(blob_schema.get()->children[1], "blob_a"),
              "released Blob v2 raw blob_a descriptor");
        CHECK(blob_descriptor_schema(blob_schema.get()->children[2], "blob_b"),
              "released Blob v2 raw blob_b descriptor");
        const ArrowSchema *info_field = blob_schema.get()->children[3];
        CHECK(info_field->n_children == 1 &&
                  std::strcmp(info_field->children[0]->name, "payload") == 0,
              "released Blob v2 raw nested field");
        CHECK(blob_descriptor_schema(info_field->children[0], "info.payload"),
              "released Blob v2 raw nested descriptor");
        lance::ArrowStream blob_stream = blob_reader.read_all(2, 2);
        blob_reader.reset();
        CHECK(verify_released_blob_v2_stream(blob_stream),
              "released Blob v2 raw stream");

        lance::FileReaderOptions blob_projection;
        blob_projection.columns =
            std::vector<std::string>{"blob_b", "info.payload"};
        auto projected = blob_session.open_reader(
            "blob_v2_raw_reader/blob_v2_raw.lance", blob_projection);
        const auto projected_metadata = projected.metadata();
        const auto projected_schema = projected_metadata.schema();
        CHECK(projected_schema.get()->n_children == 2,
              "released Blob v2 projected fields");
        CHECK(std::strcmp(projected_schema.get()->children[0]->name,
                          "blob_b") == 0,
              "released Blob v2 projected top-level field");
        CHECK(std::strcmp(projected_schema.get()->children[1]->name, "info") ==
                      0 &&
                  projected_schema.get()->children[1]->n_children == 1,
              "released Blob v2 projected nested field");
        CHECK(blob_descriptor_schema(
                  projected_schema.get()->children[1]->children[0],
                  "projected info.payload"),
              "released Blob v2 projected descriptor");
        auto projected_stream = projected.read_all(3, 2);
        projected.reset();
        ArrowSchema stream_schema{};
        const bool projected_shape =
            projected_stream->get_schema(projected_stream.get(),
                                         &stream_schema) == 0 &&
            std::strcmp(stream_schema.format, "+s") == 0 &&
            stream_schema.n_children == 2;
        if (stream_schema.release) stream_schema.release(&stream_schema);
        CHECK(projected_shape, "released Blob v2 projected stream schema");
    }

    return failures == 0 ? 0 : 1;
}
