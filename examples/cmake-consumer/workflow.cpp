// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

// End-to-end installed-package workflow:
//   1. write an Arrow record-batch stream with id, text and embedding columns,
//   2. build BTree, full-text and IVF vector indices,
//   3. reopen the dataset and run a filtered vector query and an FTS query.

#include <lance/lance.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int64_t kArrowFlagNullable = 2;
constexpr int64_t kRowCount = 6;
constexpr char kFtsParams[] =
    R"({"base_tokenizer":"simple","language":"English"})";

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

// ─── Arrow schema fixture ───────────────────────────────────────────────────

struct SchemaHolder {
    ArrowSchema root{};
    ArrowSchema id{};
    ArrowSchema text{};
    ArrowSchema embedding{};
    ArrowSchema item{};
    ArrowSchema* children[3] = {&id, &text, &embedding};
    ArrowSchema* embedding_children[1] = {&item};
};

void release_schema_child(ArrowSchema* schema) {
    schema->release = nullptr;
}

void release_schema(ArrowSchema* schema) {
    auto* holder = static_cast<SchemaHolder*>(schema->private_data);
    if (holder != nullptr) {
        for (ArrowSchema* child : holder->children) {
            if (child->release != nullptr) child->release(child);
        }
        if (holder->item.release != nullptr) holder->item.release(&holder->item);
        delete holder;
    }
    schema->release = nullptr;
    schema->private_data = nullptr;
}

SchemaHolder* make_schema() {
    auto* holder = new SchemaHolder;
    holder->id.format = "i";
    holder->id.name = "id";
    holder->id.flags = kArrowFlagNullable;
    holder->id.release = release_schema_child;

    holder->text.format = "u";
    holder->text.name = "text";
    holder->text.flags = kArrowFlagNullable;
    holder->text.release = release_schema_child;

    holder->embedding.format = "+w:2";
    holder->embedding.name = "embedding";
    holder->embedding.flags = kArrowFlagNullable;
    holder->embedding.n_children = 1;
    holder->embedding.children = holder->embedding_children;
    holder->embedding.release = release_schema_child;

    holder->item.format = "f";
    holder->item.name = "item";
    holder->item.flags = kArrowFlagNullable;
    holder->item.release = release_schema_child;

    holder->root.format = "+s";
    holder->root.n_children = 3;
    holder->root.children = holder->children;
    holder->root.release = release_schema;
    holder->root.private_data = holder;
    return holder;
}

// ─── One Arrow record batch ─────────────────────────────────────────────────

void release_array_child(ArrowArray* array) {
    array->release = nullptr;
}

void release_batch(ArrowArray* array);

struct BatchHolder {
    ArrowArray root{};
    ArrowArray id{};
    ArrowArray text{};
    ArrowArray embedding{};
    ArrowArray item{};
    ArrowArray* children[3] = {&id, &text, &embedding};
    ArrowArray* embedding_children[1] = {&item};
    const void* root_buffers[1] = {nullptr};
    const void* id_buffers[2] = {nullptr, nullptr};
    const void* text_buffers[3] = {nullptr, nullptr, nullptr};
    const void* embedding_buffers[1] = {nullptr};
    const void* item_buffers[2] = {nullptr, nullptr};
    std::vector<int32_t> ids{1, 2, 3, 4, 5, 6};
    std::string texts;
    std::vector<int32_t> offsets;
    std::vector<float> values{
        3.0f, 0.0f,
        2.0f, 0.0f,
        0.1f, 0.0f,
        0.2f, 0.0f,
        0.3f, 0.0f,
        0.4f, 0.0f,
    };

    BatchHolder() {
        const char* values_text[] = {
            "red apple",
            "green apple",
            "blue berry",
            "blue apple",
            "yellow pear",
            "red pear",
        };
        offsets.push_back(0);
        for (const char* value : values_text) {
            texts.append(value);
            offsets.push_back(static_cast<int32_t>(texts.size()));
        }

        id_buffers[1] = ids.data();
        text_buffers[1] = offsets.data();
        text_buffers[2] = texts.data();
        item_buffers[1] = values.data();

        id.length = kRowCount;
        id.n_buffers = 2;
        id.buffers = id_buffers;
        id.release = release_array_child;

        text.length = kRowCount;
        text.n_buffers = 3;
        text.buffers = text_buffers;
        text.release = release_array_child;

        item.length = kRowCount * 2;
        item.n_buffers = 2;
        item.buffers = item_buffers;
        item.release = release_array_child;

        embedding.length = kRowCount;
        embedding.n_buffers = 1;
        embedding.buffers = embedding_buffers;
        embedding.n_children = 1;
        embedding.children = embedding_children;
        embedding.release = release_array_child;

        root.length = kRowCount;
        root.n_buffers = 1;
        root.buffers = root_buffers;
        root.n_children = 3;
        root.children = children;
        root.release = release_batch;
        root.private_data = this;
    }
};

void release_batch(ArrowArray* array) {
    auto* holder = static_cast<BatchHolder*>(array->private_data);
    if (holder != nullptr) {
        for (ArrowArray* child : holder->children) {
            if (child->release != nullptr) child->release(child);
        }
        if (holder->item.release != nullptr) holder->item.release(&holder->item);
        delete holder;
    }
    array->release = nullptr;
    array->private_data = nullptr;
}

struct StreamState {
    BatchHolder* batch = nullptr;
};

int stream_get_schema(ArrowArrayStream*, ArrowSchema* out) {
    if (out == nullptr) return -1;
    SchemaHolder* schema = make_schema();
    *out = schema->root;
    return 0;
}

int stream_get_next(ArrowArrayStream* stream, ArrowArray* out) {
    auto* state = static_cast<StreamState*>(stream->private_data);
    if (state == nullptr) return -1;
    if (state->batch == nullptr) {
        std::memset(out, 0, sizeof(*out));
        return 0;
    }
    *out = state->batch->root;
    state->batch = nullptr;
    return 0;
}

const char* stream_last_error(ArrowArrayStream*) {
    return nullptr;
}

void stream_release(ArrowArrayStream* stream) {
    auto* state = static_cast<StreamState*>(stream->private_data);
    if (state != nullptr) {
        if (state->batch != nullptr) {
            state->batch->root.release(&state->batch->root);
        }
        delete state;
    }
    std::memset(stream, 0, sizeof(*stream));
}

// ─── Workflow helpers ───────────────────────────────────────────────────────

void write_dataset(const std::string& uri) {
    auto* state = new StreamState;
    state->batch = new BatchHolder;

    ArrowArrayStream input{};
    input.get_schema = stream_get_schema;
    input.get_next = stream_get_next;
    input.get_last_error = stream_last_error;
    input.release = stream_release;
    input.private_data = state;

    // Dataset::write consumes `input` on every return path, including errors.
    auto dataset = lance::Dataset::write(uri, &input, lance::WriteMode::Create);
    if (dataset.count_rows() != static_cast<uint64_t>(kRowCount)) {
        fail("write returned an unexpected row count");
    }
    std::printf(
        "write: rows=%llu version=%llu\n",
        static_cast<unsigned long long>(dataset.count_rows()),
        static_cast<unsigned long long>(dataset.version()));
}

void create_indices(lance::Dataset& dataset) {
    dataset.create_scalar_index("id", LANCE_SCALAR_BTREE, "id_btree");
    dataset.create_scalar_index(
        "text", LANCE_SCALAR_INVERTED, "text_fts", kFtsParams);

    lance::VectorIndexOptions options;
    options.index_type = LANCE_INDEX_IVF_FLAT;
    options.metric = LANCE_METRIC_L2;
    // One partition is sufficient for this six-row example. Larger datasets
    // normally use more partitions or native automatic sizing.
    options.num_partitions = 1;
    options.max_iters = 2;
    options.sample_rate = 8;
    dataset.create_vector_index_with_options(
        "embedding", options, "embedding_ivf");

    std::printf(
        "indices: count=%llu version=%llu\n",
        static_cast<unsigned long long>(dataset.index_count()),
        static_cast<unsigned long long>(dataset.version()));
}

std::vector<int32_t> collect_ids(ArrowArrayStream* stream) {
    ArrowSchema schema{};
    if (stream->get_schema == nullptr ||
        stream->get_schema(stream, &schema) != 0) {
        fail("query stream get_schema failed");
    }
    int64_t id_index = -1;
    for (int64_t index = 0; index < schema.n_children; ++index) {
        const ArrowSchema* child = schema.children[index];
        if (child != nullptr && child->name != nullptr &&
            std::strcmp(child->name, "id") == 0) {
            id_index = index;
            break;
        }
    }
    if (schema.release != nullptr) schema.release(&schema);
    if (id_index < 0) fail("query output did not contain the id column");

    std::vector<int32_t> ids;
    for (;;) {
        ArrowArray batch{};
        if (stream->get_next == nullptr ||
            stream->get_next(stream, &batch) != 0) {
            const char* message = stream->get_last_error != nullptr
                                      ? stream->get_last_error(stream)
                                      : nullptr;
            fail(message != nullptr ? message : "query stream get_next failed");
        }
        if (batch.release == nullptr) break;
        if (batch.children == nullptr || batch.children[id_index] == nullptr) {
            batch.release(&batch);
            fail("query batch did not contain the id column");
        }
        const ArrowArray* column = batch.children[id_index];
        const auto* values = static_cast<const int32_t*>(column->buffers[1]);
        for (int64_t row = 0; row < batch.length; ++row) {
            ids.push_back(values[column->offset + row]);
        }
        batch.release(&batch);
    }
    return ids;
}

std::vector<int32_t> vector_query(lance::Dataset& dataset) {
    const float query[] = {0.0f, 0.0f};
    auto scanner = dataset.scan(std::nullopt, "id >= 4");
    scanner.nearest("embedding", query, 2, 2)
        .metric(LANCE_METRIC_L2)
        .prefilter(true)
        .limit(2);

    ArrowArrayStream raw{};
    scanner.to_arrow_stream(&raw);
    lance::ArrowStream stream(raw);
    return collect_ids(stream.get());
}

std::vector<int32_t> fts_query(lance::Dataset& dataset) {
    auto scanner = dataset.scan();
    scanner.full_text_search("apple", {"text"}, 0).limit(5);

    ArrowArrayStream raw{};
    scanner.to_arrow_stream(&raw);
    lance::ArrowStream stream(raw);
    return collect_ids(stream.get());
}

void print_ids(const char* label, std::vector<int32_t> ids) {
    std::sort(ids.begin(), ids.end());
    std::printf("%s:", label);
    for (int32_t id : ids) std::printf(" %d", id);
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: workflow <new-dataset-uri>\n");
        return 2;
    }

    try {
        const std::string uri = argv[1];
        write_dataset(uri);

        {
            auto dataset = lance::Dataset::open(uri);
            create_indices(dataset);
        }

        auto dataset = lance::Dataset::open(uri);
        auto vector_ids = vector_query(dataset);
        print_ids("vector query", vector_ids);
        std::sort(vector_ids.begin(), vector_ids.end());
        if (vector_ids != std::vector<int32_t>{4, 5}) {
            fail("filtered vector query returned unexpected ids");
        }

        auto fts_ids = fts_query(dataset);
        print_ids("fts query", fts_ids);
        std::sort(fts_ids.begin(), fts_ids.end());
        if (fts_ids != std::vector<int32_t>{1, 2, 4}) {
            fail("full-text query returned unexpected ids");
        }

        std::printf("workflow: PASS\n");
        return 0;
    } catch (const lance::Error& error) {
        std::fprintf(
            stderr, "workflow: lance error %d: %s\n",
            static_cast<int>(error.code), error.what());
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "workflow: FAIL: %s\n", error.what());
        return 1;
    }
}
