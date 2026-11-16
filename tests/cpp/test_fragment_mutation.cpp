// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include "lance/lance.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            std::fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__,         \
                         __LINE__, #condition);                                  \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

namespace {

constexpr int32_t kArrowFlagNullable = 2;

void release_schema(ArrowSchema* schema) { schema->release = nullptr; }
void release_array(ArrowArray* array) { array->release = nullptr; }

int64_t bit_value(const ArrowArray& array, int64_t row) {
    if (array.null_count == 0 || array.buffers == nullptr ||
        array.buffers[0] == nullptr) {
        return 1;
    }
    const auto* bits = static_cast<const uint8_t*>(array.buffers[0]);
    const int64_t index = array.offset + row;
    return (bits[index / 8] >> (index % 8)) & 1;
}

int64_t integer_value(const ArrowArray& array, int64_t row,
                      const char* format) {
    const int64_t index = array.offset + row;
    if (std::strcmp(format, "i") == 0) {
        return static_cast<const int32_t*>(array.buffers[1])[index];
    }
    if (std::strcmp(format, "l") == 0) {
        return static_cast<const int64_t*>(array.buffers[1])[index];
    }
    if (std::strcmp(format, "L") == 0) {
        return static_cast<int64_t>(
            static_cast<const uint64_t*>(array.buffers[1])[index]);
    }
    throw lance::Error(LANCE_ERR_INVALID_ARGUMENT,
                       std::string("unsupported integer format ") + format);
}

int child_index(const ArrowSchema& schema, const char* name) {
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (std::strcmp(schema.children[i]->name, name) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

struct Value {
    bool found = false;
    bool is_null = false;
    int64_t value = 0;
};

Value find_value(ArrowArrayStream* stream, const std::string& column,
                 int64_t wanted_id) {
    ArrowSchema schema{};
    if (stream->get_schema(stream, &schema) != 0) lance::check_error();
    std::unique_ptr<ArrowSchema, void (*)(ArrowSchema*)> schema_guard(
        &schema, [](ArrowSchema* value) {
            if (value->release) value->release(value);
        });
    const int id_index = child_index(schema, "id");
    const int value_index = child_index(schema, column.c_str());
    CHECK(id_index >= 0 && value_index >= 0);
    const char* id_format = schema.children[id_index]->format;
    const char* value_format = schema.children[value_index]->format;

    Value result;
    while (true) {
        ArrowArray batch{};
        if (stream->get_next(stream, &batch) != 0) lance::check_error();
        if (!batch.release) break;
        std::unique_ptr<ArrowArray, void (*)(ArrowArray*)> batch_guard(
            &batch, [](ArrowArray* value) {
                if (value->release) value->release(value);
            });
        for (int64_t row = 0; row < batch.length; ++row) {
            const int64_t id =
                integer_value(*batch.children[id_index], row, id_format);
            if (id != wanted_id) continue;
            result.found = true;
            const ArrowArray& value = *batch.children[value_index];
            if (!bit_value(value, row)) {
                result.is_null = true;
            } else {
                result.value = integer_value(value, row, value_format);
            }
            break;
        }
        if (result.found) break;
    }
    return result;
}

Value scan_dataset_value(const std::string& uri, const std::string& column,
                         int64_t wanted_id) {
    auto dataset = lance::Dataset::open(uri);
    auto scanner = dataset.scan();
    if (column == "_rowid") scanner.with_row_id();
    ArrowArrayStream raw{};
    scanner.to_arrow_stream(&raw);
    lance::ArrowStream stream(raw);
    return find_value(stream.get(), column, wanted_id);
}

Value scan_fragment_value(const lance::Fragment& fragment,
                          const std::string& column, int64_t wanted_id) {
    auto scanner = fragment.scan();
    ArrowArrayStream raw{};
    scanner.to_arrow_stream(&raw);
    lance::ArrowStream stream(raw);
    return find_value(stream.get(), column, wanted_id);
}

struct OneFieldSchema {
    ArrowSchema schema{};
    ArrowSchema child{};
    ArrowSchema* children[1] = {&child};

    OneFieldSchema(const char* name, const char* format, bool nullable)
        : schema{}, child{} {
        schema.format = "+s";
        schema.name = "";
        schema.n_children = 1;
        schema.children = children;
        schema.release = release_schema;
        child.format = format;
        child.name = name;
        child.flags = nullable ? kArrowFlagNullable : 0;
        child.release = release_schema;
    }
};

struct ConstantInt64Array {
    std::vector<int64_t> values;
    ArrowArray child{};
    ArrowArray root{};
    std::array<const void*, 1> root_buffers{};
    std::array<const void*, 2> child_buffers{};
    std::array<ArrowArray*, 1> children{};

    ConstantInt64Array(size_t rows, int64_t value) : values(rows, value) {
        root_buffers[0] = nullptr;
        child_buffers[0] = nullptr;
        child_buffers[1] = values.data();
        child.length = static_cast<int64_t>(rows);
        child.n_buffers = 2;
        child.buffers = child_buffers.data();
        child.release = release_array;
        children[0] = &child;
        root.length = static_cast<int64_t>(rows);
        root.n_buffers = 1;
        root.buffers = root_buffers.data();
        root.n_children = 1;
        root.children = children.data();
        root.release = &release_owned;
        root.private_data = this;
    }

    static void release_owned(ArrowArray* array) {
        auto* self = static_cast<ConstantInt64Array*>(array->private_data);
        array->release = nullptr;
        delete self;
    }
};

void emit_constant_int64(ArrowArray* output, size_t rows, int64_t value) {
    auto* owned = new ConstantInt64Array(rows, value);
    *output = owned->root;
}

class OneColumnStream {
    ArrowArrayStream stream_{};
    ArrowSchema schema_{};
    ArrowSchema child_schema_{};
    ArrowSchema* schema_children_[1] = {&child_schema_};
    ArrowArray batch_{};
    ArrowArray child_array_{};
    ArrowArray* batch_children_[1] = {&child_array_};
    const void* root_buffers_[1] = {nullptr};
    const void* child_buffers_[2] = {};
    std::vector<int32_t> values_;
    bool emitted_ = false;

    static int get_schema(ArrowArrayStream* stream, ArrowSchema* out) {
        auto* self = static_cast<OneColumnStream*>(stream->private_data);
        *out = self->schema_;
        return 0;
    }

    static int get_next(ArrowArrayStream* stream, ArrowArray* out) {
        auto* self = static_cast<OneColumnStream*>(stream->private_data);
        std::memset(out, 0, sizeof(*out));
        if (self->emitted_) return 0;
        *out = self->batch_;
        self->emitted_ = true;
        return 0;
    }

    static const char* last_error(ArrowArrayStream*) { return nullptr; }

    static void release(ArrowArrayStream* stream) {
        stream->release = nullptr;
    }

public:
    OneColumnStream(const char* name, const char* format,
                    std::vector<int32_t> values)
        : values_(std::move(values)) {
        schema_.format = "+s";
        schema_.name = "";
        schema_.n_children = 1;
        schema_.children = schema_children_;
        schema_.release = release_schema;
        child_schema_.format = format;
        child_schema_.name = name;
        child_schema_.flags = kArrowFlagNullable;
        child_schema_.release = release_schema;
        child_buffers_[0] = nullptr;
        child_buffers_[1] = values_.data();
        child_array_.length = static_cast<int64_t>(values_.size());
        child_array_.n_buffers = 2;
        child_array_.buffers = child_buffers_;
        child_array_.release = release_array;
        batch_.length = static_cast<int64_t>(values_.size());
        batch_.n_buffers = 1;
        batch_.buffers = root_buffers_;
        batch_.n_children = 1;
        batch_.children = batch_children_;
        batch_.release = release_array;
        stream_.get_schema = &get_schema;
        stream_.get_next = &get_next;
        stream_.get_last_error = &last_error;
        stream_.release = &release;
        stream_.private_data = this;
    }

    OneColumnStream(const OneColumnStream&) = delete;
    OneColumnStream& operator=(const OneColumnStream&) = delete;

    ArrowArrayStream* get() { return &stream_; }
};

class TwoColumnStream {
    ArrowArrayStream stream_{};
    ArrowSchema schema_{};
    std::array<ArrowSchema, 2> child_schema_{};
    std::array<ArrowSchema*, 2> schema_children_{};
    ArrowArray batch_{};
    std::array<ArrowArray, 2> child_array_{};
    std::array<ArrowArray*, 2> batch_children_{};
    const void* root_buffers_[1] = {nullptr};
    std::array<const void*, 2> child0_buffers_{};
    std::array<const void*, 2> child1_buffers_{};
    std::vector<int64_t> ids_;
    std::vector<int32_t> values_;
    bool emitted_ = false;

    static int get_schema(ArrowArrayStream* stream, ArrowSchema* out) {
        auto* self = static_cast<TwoColumnStream*>(stream->private_data);
        *out = self->schema_;
        return 0;
    }

    static int get_next(ArrowArrayStream* stream, ArrowArray* out) {
        auto* self = static_cast<TwoColumnStream*>(stream->private_data);
        std::memset(out, 0, sizeof(*out));
        if (self->emitted_) return 0;
        *out = self->batch_;
        self->emitted_ = true;
        return 0;
    }

    static const char* last_error(ArrowArrayStream*) { return nullptr; }

    static void release(ArrowArrayStream* stream) {
        stream->release = nullptr;
    }

public:
    TwoColumnStream(const char* left_name, const char* left_format,
                    const char* right_name, const char* right_format,
                    std::vector<int64_t> ids, std::vector<int32_t> values)
        : ids_(std::move(ids)), values_(std::move(values)) {
        CHECK(ids_.size() == values_.size());
        schema_.format = "+s";
        schema_.name = "";
        schema_.n_children = 2;
        schema_children_[0] = &child_schema_[0];
        schema_children_[1] = &child_schema_[1];
        schema_.children = schema_children_.data();
        schema_.release = release_schema;
        child_schema_[0].format = left_format;
        child_schema_[0].name = left_name;
        child_schema_[0].release = release_schema;
        child_schema_[1].format = right_format;
        child_schema_[1].name = right_name;
        child_schema_[1].flags = kArrowFlagNullable;
        child_schema_[1].release = release_schema;

        child0_buffers_[0] = nullptr;
        child0_buffers_[1] = ids_.data();
        child1_buffers_[0] = nullptr;
        child1_buffers_[1] = values_.data();
        child_array_[0].length = static_cast<int64_t>(ids_.size());
        child_array_[0].n_buffers = 2;
        child_array_[0].buffers = child0_buffers_.data();
        child_array_[0].release = release_array;
        child_array_[1].length = static_cast<int64_t>(values_.size());
        child_array_[1].n_buffers = 2;
        child_array_[1].buffers = child1_buffers_.data();
        child_array_[1].release = release_array;
        batch_children_[0] = &child_array_[0];
        batch_children_[1] = &child_array_[1];
        batch_.length = static_cast<int64_t>(ids_.size());
        batch_.n_buffers = 1;
        batch_.buffers = root_buffers_;
        batch_.n_children = 2;
        batch_.children = batch_children_.data();
        batch_.release = release_array;
        stream_.get_schema = &get_schema;
        stream_.get_next = &get_next;
        stream_.get_last_error = &last_error;
        stream_.release = &release;
        stream_.private_data = this;
    }

    TwoColumnStream(const TwoColumnStream&) = delete;
    TwoColumnStream& operator=(const TwoColumnStream&) = delete;

    ArrowArrayStream* get() { return &stream_; }
};

void commit_operation(const std::string& uri, uint64_t read_version,
                      const lance::Operation& operation) {
    auto transaction =
        lance::Transaction::from_operation(read_version, operation);
    auto dataset = lance::Dataset::commit(uri, transaction);
    CHECK(dataset.version() == read_version + 1);
}

void publish_merge(const std::string& uri, lance::Dataset& dataset,
                   lance::Fragment updated, const lance::LanceSchema& schema) {
    const auto ids = dataset.fragment_ids();
    CHECK(ids.size() == 3);
    auto second = dataset.open_fragment(ids[1]);
    auto third = dataset.open_fragment(ids[2]);
    std::vector<const lance::Fragment*> final_fragments{
        &updated, &second, &third};
    auto operation =
        lance::Operation::make_merge(schema, final_fragments, false);
    commit_operation(uri, dataset.version(), operation);
}

void test_add_columns(const std::string& uri) {
    auto dataset = lance::Dataset::open(uri);
    CHECK(dataset.version() == 5);
    CHECK(dataset.fragment_count() == 3);
    const auto ids = dataset.fragment_ids();
    auto fragment0 = dataset.open_fragment(ids[0]);
    CHECK(fragment0.read_version() == dataset.version());

    OneFieldSchema null_field("null_value", "i", true);
    auto [null_updated, null_schema] =
        fragment0.add_columns_nulls(&null_field.schema);
    CHECK(null_schema.field_id("null_value") > 5);
    CHECK(null_schema.field_id("profile.score") == 4);
    publish_merge(uri, dataset, std::move(null_updated), null_schema);

    dataset = lance::Dataset::open(uri);
    fragment0 = dataset.open_fragment(dataset.fragment_ids()[0]);
    OneColumnStream reader_stream(
        "reader_value", "i", {100, 300, 400});
    auto [reader_updated, reader_schema] =
        fragment0.add_columns_reader(reader_stream.get());
    CHECK(reader_schema.field_id("reader_value") > 5);
    publish_merge(uri, dataset, std::move(reader_updated), reader_schema);

    dataset = lance::Dataset::open(uri);
    fragment0 = dataset.open_fragment(dataset.fragment_ids()[0]);
    auto independent = lance::Dataset::open(uri);
    OneFieldSchema udf_field("udf_value", "l", true);
    lance::BatchUDF udf;
    udf.output_schema = &udf_field.schema;
    udf.mapper = [&independent](const ArrowArray& input,
                                const ArrowSchema& output_schema,
                                ArrowArray& output) {
        CHECK(independent.count_rows() == 10);
        CHECK(output_schema.n_children == 1);
        emit_constant_int64(&output, static_cast<size_t>(input.length), 77);
    };
    auto [udf_updated, udf_schema] = fragment0.add_columns_batch_udf(
        udf, std::vector<std::string>{"id"}, std::optional<uint32_t>(2));
    CHECK(udf_schema.field_id("udf_value") > 5);
    publish_merge(uri, dataset, std::move(udf_updated), udf_schema);

    auto reopened = lance::Dataset::open(uri);
    CHECK(reopened.count_rows() == 10);
    CHECK(reopened.has_stable_row_ids());
    auto id1_null = scan_dataset_value(uri, "null_value", 1);
    auto id1_reader = scan_dataset_value(uri, "reader_value", 1);
    auto id1_udf = scan_dataset_value(uri, "udf_value", 1);
    auto id3_reader = scan_dataset_value(uri, "reader_value", 3);
    auto id4_reader = scan_dataset_value(uri, "reader_value", 4);
    auto id5_udf = scan_dataset_value(uri, "udf_value", 5);
    CHECK(id1_null.found && id1_null.is_null);
    CHECK(id1_reader.found && !id1_reader.is_null && id1_reader.value == 100);
    CHECK(id1_udf.found && !id1_udf.is_null && id1_udf.value == 77);
    CHECK(id3_reader.found && !id3_reader.is_null && id3_reader.value == 300);
    CHECK(id4_reader.found && !id4_reader.is_null && id4_reader.value == 400);
    CHECK(id5_udf.found && id5_udf.is_null);
    printf("C++ AllNulls/Reader/BatchUDF -> typed Merge -> reopen OK\n");
}

void test_merge_columns(const std::string& uri) {
    auto dataset = lance::Dataset::open(uri);
    auto fragment0 = dataset.open_fragment(dataset.fragment_ids()[0]);
    TwoColumnStream stream("id", "l", "joined", "i", {1, 2, 3, 4},
                           {11, 22, 33, 44});
    auto [updated, schema] =
        fragment0.merge_columns(stream.get(), "id", "id");
    CHECK(stream.get()->release == nullptr);
    publish_merge(uri, dataset, std::move(updated), schema);

    auto id1 = scan_dataset_value(uri, "joined", 1);
    auto id3 = scan_dataset_value(uri, "joined", 3);
    auto id5 = scan_dataset_value(uri, "joined", 5);
    CHECK(id1.found && !id1.is_null && id1.value == 11);
    CHECK(id3.found && !id3.is_null && id3.value == 33);
    CHECK(id5.found && id5.is_null);
    printf("C++ hash merge -> typed Merge -> reopen OK\n");
}

void test_update_columns(const std::string& uri) {
    auto dataset = lance::Dataset::open(uri);
    auto fragment0 = dataset.open_fragment(dataset.fragment_ids()[0]);
    const uint64_t read_version = fragment0.read_version();
    // Dense live rows, sparse matches, and no matches use the same native update.
    const std::vector<std::vector<int64_t>> keys{{1, 3, 4}, {1, 4}, {99}};
    const std::vector<std::vector<uint32_t>> expected{{0, 2, 3}, {0, 3}, {}};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        std::vector<uint32_t> copied;
        {
            TwoColumnStream input("id", "l", "value", "i", keys[i],
                                  std::vector<int32_t>(keys[i].size(), 123));
            auto temporary = fragment0.update_columns(input.get(), "id", "id");
            copied = temporary.matched_offsets();
        }
        CHECK(copied == expected[i]);
    }
    TwoColumnStream stream("id", "l", "value", "i", {1, 3}, {901, 903});
    auto result = fragment0.update_columns(stream.get(), "id", "id");
    CHECK(stream.get()->release == nullptr);
    auto updated = result.fragment();
    auto fields = result.fields_modified();
    auto offsets = result.matched_offsets();
    CHECK(fields == std::vector<uint32_t>{1});
    CHECK(offsets == std::vector<uint32_t>({0, 2}));
    CHECK(!result.matched_offsets_serialized().empty());

    lance::UpdatedFragmentOffsets typed_offsets(
        std::vector<std::pair<uint64_t, std::vector<uint32_t>>>{{0, offsets}});
    CHECK(typed_offsets.size() == 1);
    CHECK(typed_offsets.fragment_id_at(0) == 0);
    CHECK(typed_offsets.offset_count_at(0) == offsets.size());
    CHECK(typed_offsets.offsets_at(0) == offsets);
    lance::UpdatedFragmentOffsets empty_entry_offsets(
        std::vector<std::pair<uint64_t, std::vector<uint32_t>>>{{0, {}}});
    CHECK(empty_entry_offsets.offsets_at(0).empty());
    auto cloned_typed_offsets = typed_offsets.clone();
    CHECK(cloned_typed_offsets.offsets_at(0) == offsets);
    bool missing_entry_rejected = false;
    try {
        (void)typed_offsets.offsets_at(typed_offsets.size());
    } catch (const lance::Error& error) {
        missing_entry_rejected = error.code == LANCE_ERR_INVALID_ARGUMENT;
    }
    CHECK(missing_entry_rejected);

    lance::UpdateOperationSpec spec;
    spec.updated_fragments = {&updated};
    spec.fields_modified = fields;
    spec.update_mode = lance::UpdateMode::RewriteColumns;
    spec.updated_fragment_offsets = std::move(typed_offsets);
    auto operation = lance::Operation::make_update(spec);
    commit_operation(uri, read_version, operation);

    auto id1 = scan_dataset_value(uri, "value", 1);
    auto id3 = scan_dataset_value(uri, "value", 3);
    auto id4 = scan_dataset_value(uri, "value", 4);
    auto id2 = scan_dataset_value(uri, "value", 2);
    CHECK(id1.found && !id1.is_null && id1.value == 901);
    CHECK(id3.found && !id3.is_null && id3.value == 903);
    CHECK(id4.found && !id4.is_null && id4.value == 40);
    CHECK(!id2.found);
    auto reopened = lance::Dataset::open(uri);
    CHECK(reopened.has_stable_row_ids());
    printf("C++ update offsets -> typed Update -> reopen OK\n");
}

void test_write_columns(const std::string& uri) {
    auto dataset = lance::Dataset::open(uri);
    auto fragment0 = dataset.open_fragment(dataset.fragment_ids()[0]);
    const uint64_t read_version = fragment0.read_version();
    auto value_schema =
        lance::LanceSchema::project(fragment0.native_schema(), {"value"});
    OneColumnStream stream("value", "i", {111, 222, 333, 444});
    auto group = fragment0.write_column_with_version(
        value_schema, stream.get(), LANCE_FILE_FORMAT_V2_2);
    CHECK(stream.get()->release == nullptr);
    CHECK(group.file().file_version() == std::make_pair(2u, 2u));
    std::vector<const lance::DataReplacementGroup*> groups{&group};
    auto operation = lance::Operation::make_data_replacement(groups);
    commit_operation(uri, read_version, operation);

    auto id1 = scan_dataset_value(uri, "value", 1);
    auto id3 = scan_dataset_value(uri, "value", 3);
    auto id4 = scan_dataset_value(uri, "value", 4);
    auto id5 = scan_dataset_value(uri, "value", 5);
    CHECK(id1.found && !id1.is_null && id1.value == 111);
    CHECK(id3.found && !id3.is_null && id3.value == 333);
    CHECK(id4.found && !id4.is_null && id4.value == 444);
    CHECK(id5.found && !id5.is_null && id5.value == 50);
    auto old = scan_fragment_value(fragment0, "value", 1);
    CHECK(old.found && !old.is_null && old.value == 10);
    printf("C++ explicit write-columns -> DataReplacement -> reopen OK\n");
}

void test_delete_rows(const std::string& uri) {
    auto dataset = lance::Dataset::open(uri);
    const auto ids = dataset.fragment_ids();
    auto second = dataset.open_fragment(ids[1]);
    auto third = dataset.open_fragment(ids[2]);
    const uint64_t read_version = second.read_version();
    auto partial = second.delete_rows({0, 0});
    CHECK(partial.has_value());
    CHECK(partial->logical_rows() == 3);
    auto full = third.delete_rows({0, 1, 2, 3});
    CHECK(!full.has_value());

    std::vector<const lance::Fragment*> updated{&*partial};
    auto operation = lance::Operation::make_delete(
        updated, std::vector<uint64_t>{ids[2]}, "delete_rows");
    commit_operation(uri, read_version, operation);

    auto reopened = lance::Dataset::open(uri);
    CHECK(reopened.count_rows() == 6);
    auto id5 = scan_dataset_value(uri, "value", 5);
    auto id6 = scan_dataset_value(uri, "value", 6);
    CHECK(!id5.found);
    CHECK(id6.found && !id6.is_null && id6.value == 60);
    CHECK(second.logical_rows() == 4);
    printf("C++ offset Delete -> reopen OK\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr,
                     "usage: %s ADD_URI MERGE_URI UPDATE_URI REPLACE_URI "
                     "DELETE_URI\n",
                     argv[0]);
        return 2;
    }
    try {
        test_add_columns(argv[1]);
        test_merge_columns(argv[2]);
        test_update_columns(argv[3]);
        test_write_columns(argv[4]);
        test_delete_rows(argv[5]);
        for (int i = 1; i < argc; ++i) {
            auto row_id = scan_dataset_value(argv[i], "_rowid", 3);
            CHECK(row_id.found && !row_id.is_null && row_id.value == 2);
        }
    } catch (const lance::Error& error) {
        std::fprintf(stderr, "lance::Error (%d): %s\n",
                     static_cast<int>(error.code), error.what());
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "std::exception: %s\n", error.what());
        return 1;
    }
    std::printf("all C++17 fragment mutation publication checks passed\n");
    return 0;
}
