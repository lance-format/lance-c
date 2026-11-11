// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include "lance/lance.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
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

constexpr uint64_t row_address(uint64_t fragment_id, uint64_t offset) {
    return (fragment_id << 32) | offset;
}

void release_schema_noop(ArrowSchema* schema) { schema->release = nullptr; }

void release_array_noop(ArrowArray* array) { array->release = nullptr; }

struct OverlayBatch {
    std::array<ArrowSchema, 3> schema_children{};
    std::array<ArrowArray, 3> array_children{};
    std::array<const void*, 1> struct_buffers{{nullptr}};
    std::array<const void*, 2> a_buffers{};
    std::array<const void*, 2> b_buffers{};
    ArrowSchema schema{};
    ArrowArray array{};
    std::vector<uint64_t> rowaddrs;
    std::vector<int32_t> a_values;
    std::vector<int32_t> b_values;
    std::vector<uint8_t> a_validity;
    std::vector<uint8_t> b_validity;
    size_t len = 0;
    bool with_a = false;
    bool with_b = false;

    OverlayBatch(size_t length, bool has_a, bool has_b)
        : len(length), with_a(has_a), with_b(has_b) {
        rowaddrs.resize(len);
        const size_t children = 1 + static_cast<size_t>(with_a) +
                                static_cast<size_t>(with_b);

        schema.format = "+s";
        schema.name = "";
        schema.n_children = static_cast<int64_t>(children);
        schema.children = schema_children.data();
        schema.release = release_schema_noop;

        init_schema_child(0, "L", "_rowaddr", 0);
        init_array_child(0, len, nullptr, rowaddrs.data());

        size_t child = 1;
        if (with_a) {
            a_values.resize(len);
            a_validity.assign((len + 7) / 8, 0xff);
            a_buffers = {{a_validity.data(), a_values.data()}};
            init_schema_child(child, "i", "a", ARROW_FLAG_NULLABLE);
            init_array_child(child, len, a_buffers[0], a_buffers[1]);
            ++child;
        }
        if (with_b) {
            b_values.resize(len);
            b_validity.assign((len + 7) / 8, 0xff);
            b_buffers = {{b_validity.data(), b_values.data()}};
            init_schema_child(child, "i", "b", ARROW_FLAG_NULLABLE);
            init_array_child(child, len, b_buffers[0], b_buffers[1]);
        }

        array.length = static_cast<int64_t>(len);
        array.n_buffers = 1;
        array.buffers = struct_buffers.data();
        array.n_children = static_cast<int64_t>(children);
        array.children = array_children.data();
        array.release = release_array_noop;
    }

    OverlayBatch(const OverlayBatch&) = delete;
    OverlayBatch& operator=(const OverlayBatch&) = delete;

    void set_rowaddr(size_t index, uint64_t value) {
        rowaddrs.at(index) = value;
    }

    void set_a(size_t index, int32_t value) {
        CHECK(with_a);
        a_values.at(index) = value;
    }

    void set_a_null(size_t index) {
        CHECK(with_a);
        a_validity.at(index / 8) &=
            static_cast<uint8_t>(~(1u << (index % 8)));
        array_children[1].null_count += 1;
    }

    void set_b(size_t index, int32_t value) {
        CHECK(with_b);
        b_values.at(index) = value;
    }

private:
    void init_schema_child(size_t index, const char* format, const char* name,
                           int64_t flags) {
        ArrowSchema& child = schema_children.at(index);
        child = {};
        child.format = format;
        child.name = name;
        child.flags = flags;
        child.release = release_schema_noop;
    }

    static void init_array_child(ArrowArray& child, size_t length,
                                 const void* validity, const void* values) {
        const void** buffers =
            static_cast<const void**>(std::malloc(2 * sizeof(void*)));
        CHECK(buffers != nullptr);
        buffers[0] = validity;
        buffers[1] = values;
        child = {};
        child.length = static_cast<int64_t>(length);
        child.n_buffers = 2;
        child.buffers = buffers;
        child.release = release_array_noop;
    }
};

void write_batch(lance::FragmentOverlayWriter& writer, OverlayBatch& batch) {
    auto imported = lance::Batch::from_arrow(&batch.array, &batch.schema);
    writer.write_batch(imported);
}

struct Row {
    bool found = false;
    int32_t id = 0;
    bool a_null = false;
    int32_t a = 0;
    bool b_null = false;
    int32_t b = 0;
    uint64_t rowid = 0;
};

bool bit_at(const ArrowArray& array, size_t index) {
    if (!array.buffers || !array.buffers[0]) return true;
    const auto* bitmap = static_cast<const uint8_t*>(array.buffers[0]);
    return (bitmap[index / 8] >> (index % 8)) & 1;
}

Row find_row(const lance::Dataset& dataset, int32_t wanted) {
    auto scanner = dataset.scan({"id", "a", "b"});
    scanner.with_row_id(true);
    ArrowArrayStream raw{};
    scanner.to_arrow_stream(&raw);
    lance::ArrowStream stream(raw);

    Row result;
    while (true) {
        ArrowArray array{};
        ArrowSchema schema{};
        if (stream->get_next(stream.get(), &array, &schema) != 0) {
            CHECK(lance_last_error_code() == LANCE_OK);
            break;
        }
        if (!array.release) break;

        int id_child = -1;
        int a_child = -1;
        int b_child = -1;
        int rowid_child = -1;
        for (int64_t c = 0; c < array.n_children; ++c) {
            const char* name = schema.children[c]->name;
            if (std::strcmp(name, "id") == 0) id_child = static_cast<int>(c);
            if (std::strcmp(name, "a") == 0) a_child = static_cast<int>(c);
            if (std::strcmp(name, "b") == 0) b_child = static_cast<int>(c);
            if (std::strcmp(name, "_rowid") == 0)
                rowid_child = static_cast<int>(c);
        }
        CHECK(id_child >= 0 && a_child >= 0 && b_child >= 0 &&
              rowid_child >= 0);

        for (int64_t i = 0; i < array.length; ++i) {
            const auto* ids =
                static_cast<const int32_t*>(array.children[id_child]->buffers[1]);
            if (!bit_at(*array.children[id_child], static_cast<size_t>(i)))
                continue;
            if (ids[i] != wanted) continue;
            result.found = true;
            result.id = ids[i];
            result.a_null =
                !bit_at(*array.children[a_child], static_cast<size_t>(i));
            result.a =
                result.a_null
                    ? 0
                    : static_cast<const int32_t*>(
                          array.children[a_child]->buffers[1])[i];
            result.b_null =
                !bit_at(*array.children[b_child], static_cast<size_t>(i));
            result.b =
                result.b_null
                    ? 0
                    : static_cast<const int32_t*>(
                          array.children[b_child]->buffers[1])[i];
            result.rowid = static_cast<const uint64_t*>(
                array.children[rowid_child]->buffers[1])[i];
            break;
        }
        if (array.release) array.release(&array);
        if (schema.release) schema.release(&schema);
        if (result.found) break;
    }
    return result;
}

void assert_row(const lance::Dataset& dataset, int32_t id, bool a_null,
                int32_t a, int32_t b, uint64_t rowid) {
    const Row row = find_row(dataset, id);
    CHECK(row.found);
    CHECK(row.id == id);
    CHECK(row.a_null == a_null);
    if (!a_null) CHECK(row.a == a);
    CHECK(!row.b_null);
    CHECK(row.b == b);
    CHECK(row.rowid == rowid);
}

void check_group(const lance::DataOverlayGroup& group, uint64_t fragment_id,
                 lance::OverlayCoverageKind kind, uint64_t bitmap_count) {
    CHECK(group.fragment_id() == fragment_id);
    CHECK(group.overlay_count() == 1);
    auto file = group.overlay_at(0);
    CHECK(file.coverage_kind() == kind);
    CHECK(file.bitmap_count() == bitmap_count);
    CHECK(file.committed_version() == 0);
    for (uint64_t i = 0; i < file.bitmap_count(); ++i) {
        CHECK(!file.bitmap_at(static_cast<size_t>(i)).empty());
    }
    auto data_file = file.data_file();
    CHECK(!data_file.path().empty());
    CHECK(!data_file.fields().empty());
}

lance::DataOverlayGroup rebuild_group(const lance::DataOverlayGroup& source,
                                      lance::OverlayCoverageKind kind) {
    auto file = source.overlay_at(0);
    std::vector<std::vector<uint8_t>> bitmaps;
    bitmaps.reserve(file.bitmap_count());
    for (uint64_t i = 0; i < file.bitmap_count(); ++i) {
        bitmaps.push_back(file.bitmap_at(static_cast<size_t>(i)));
    }
    auto data_file = file.data_file();
    lance::DataOverlayFile rebuilt(data_file, kind, bitmaps, 0);
    return lance::DataOverlayGroup(source.fragment_id(), {&rebuilt});
}

void check_history(const lance::Dataset& dataset, uint64_t version,
                   uint64_t expected_committed_version) {
    auto transaction = dataset.read_transaction_by_version(version);
    CHECK(transaction.has_value());
    auto operation = transaction->operation();
    CHECK(operation.kind() == lance::OperationKind::DataOverlay);
    CHECK(operation.data_overlay_group_count() == 2);
    for (size_t i = 0; i < 2; ++i) {
        auto group = operation.data_overlay_group_at(i);
        CHECK(group.overlay_count() == 1);
        auto file = group.overlay_at(0);
        CHECK(file.committed_version() == expected_committed_version);
    }
}

} // namespace

int main(int argc, char** argv) {
    CHECK(argc >= 2);
    const std::string uri = argv[1];

    try {
        auto dataset = lance::Dataset::open(uri);
        CHECK(dataset.version() == 2);
        CHECK(dataset.fragment_count() == 2);
        const auto fragment_ids = dataset.fragment_ids();

        std::array<Row, 21> before{};
        for (int32_t id = 1; id <= 20; ++id) {
            before[static_cast<size_t>(id)] = find_row(dataset, id);
            if (id == 10) {
                CHECK(!before[static_cast<size_t>(id)].found);
            } else {
                CHECK(before[static_cast<size_t>(id)].found);
            }
        }

        auto fragment0 = dataset.open_fragment(fragment_ids[0]);
        auto fragment1 = dataset.open_fragment(fragment_ids[1]);
        auto schema0 = fragment0.native_schema().project({"a", "b"});
        auto schema1 = fragment1.native_schema().project({"a", "b"});

        auto writer0 = fragment0.write_overlay(schema0);
        OverlayBatch batch0(5, true, true);
        const std::array<uint32_t, 5> offsets0 = {0, 2, 4, 7, 9};
        for (size_t i = 0; i < offsets0.size(); ++i) {
            batch0.set_rowaddr(i,
                              row_address(fragment_ids[0], offsets0[i]));
        }
        batch0.set_a(0, 111);
        batch0.set_a(1, 0);
        batch0.set_a_null(1);
        batch0.set_a(2, 113);
        batch0.set_a(3, 114);
        batch0.set_a(4, 115);
        for (size_t i = 0; i < 5; ++i) {
            batch0.set_b(i, static_cast<int32_t>(211 + i));
        }
        write_batch(writer0, batch0);
        auto finished0 = writer0.finish();
        CHECK(finished0.has_value());
        check_group(*finished0, fragment_ids[0],
                    lance::OverlayCoverageKind::Shared, 1);
        auto group0 = rebuild_group(*finished0,
                                    lance::OverlayCoverageKind::Shared);

        auto writer1 = fragment1.write_overlay(schema1);
        OverlayBatch batch1(3, true, false);
        const std::array<uint32_t, 3> offsets1 = {1, 3, 5};
        for (size_t i = 0; i < offsets1.size(); ++i) {
            batch1.set_rowaddr(i,
                              row_address(fragment_ids[1], offsets1[i]));
        }
        batch1.set_a(0, 1000);
        batch1.set_a(1, 1001);
        batch1.set_a(2, 1002);
        write_batch(writer1, batch1);

        OverlayBatch batch2(2, false, true);
        const std::array<uint32_t, 2> offsets2 = {6, 8};
        for (size_t i = 0; i < offsets2.size(); ++i) {
            batch2.set_rowaddr(i,
                              row_address(fragment_ids[1], offsets2[i]));
        }
        batch2.set_b(0, 2000);
        batch2.set_b(1, 2001);
        write_batch(writer1, batch2);
        auto finished1 = writer1.finish();
        CHECK(finished1.has_value());
        check_group(*finished1, fragment_ids[1],
                    lance::OverlayCoverageKind::PerField, 2);
        auto group1 = rebuild_group(*finished1,
                                    lance::OverlayCoverageKind::PerField);

        auto empty_schema = fragment1.native_schema().project({"a"});
        auto empty_writer = fragment1.write_overlay(empty_schema);
        auto empty_group = empty_writer.finish();
        CHECK(!empty_group.has_value());

        auto operation = lance::Operation::make_data_overlay({&group0, &group1});
        CHECK(operation.kind() == lance::OperationKind::DataOverlay);
        CHECK(operation.data_overlay_group_count() == 2);
        {
            auto inspected = operation.data_overlay_group_at(1);
            CHECK(inspected.fragment_id() == fragment_ids[1]);
            CHECK(inspected.overlay_count() == 1);
        }

        auto transaction = lance::Transaction::from_operation(2, operation);
        CHECK(transaction.read_version() == 2);
        auto committed = lance::Dataset::commit(uri, transaction);
        CHECK(committed.version() == 3);
        CHECK(committed.fragment_count() == 2);

        assert_row(committed, 1, false, 111, 211,
                   before[1].rowid);
        assert_row(committed, 2, false, 20, 200, before[2].rowid);
        assert_row(committed, 3, true, 0, 212, before[3].rowid);
        assert_row(committed, 4, false, 40, 400, before[4].rowid);
        assert_row(committed, 5, false, 113, 213, before[5].rowid);
        assert_row(committed, 6, false, 60, 600, before[6].rowid);
        assert_row(committed, 7, false, 70, 700, before[7].rowid);
        assert_row(committed, 8, false, 114, 214, before[8].rowid);
        assert_row(committed, 9, false, 90, 900, before[9].rowid);
        CHECK(!find_row(committed, 10).found);
        assert_row(committed, 11, false, 110, 1100, before[11].rowid);
        assert_row(committed, 12, false, 1000, 1200, before[12].rowid);
        assert_row(committed, 13, false, 130, 1300, before[13].rowid);
        assert_row(committed, 14, false, 1001, 1400, before[14].rowid);
        assert_row(committed, 15, false, 150, 1500, before[15].rowid);
        assert_row(committed, 16, false, 1002, 1600, before[16].rowid);
        assert_row(committed, 17, false, 170, 2000, before[17].rowid);
        assert_row(committed, 18, false, 180, 1800, before[18].rowid);
        assert_row(committed, 19, false, 190, 2001, before[19].rowid);
        assert_row(committed, 20, false, 200, 2000, before[20].rowid);

        check_history(committed, 3, 3);

        auto committed_fragment0 =
            committed.open_fragment(committed.fragment_ids()[0]);
        auto second_schema = committed_fragment0.native_schema().project({"a"});
        auto second_writer =
            committed_fragment0.write_overlay(second_schema);
        OverlayBatch batch3(1, true, false);
        batch3.set_rowaddr(0, row_address(committed.fragment_ids()[0], 0));
        batch3.set_a(0, 9999);
        write_batch(second_writer, batch3);
        auto finished2 = second_writer.finish();
        CHECK(finished2.has_value());
        check_group(*finished2, committed.fragment_ids()[0],
                    lance::OverlayCoverageKind::Shared, 1);
        auto second_operation =
            lance::Operation::make_data_overlay({&*finished2});
        auto second_transaction =
            lance::Transaction::from_operation(3, second_operation);
        auto committed2 = lance::Dataset::commit(uri, second_transaction);
        CHECK(committed2.version() == 4);
        assert_row(committed2, 1, false, 9999, 211, before[1].rowid);
        assert_row(committed2, 2, false, 20, 200, before[2].rowid);
        assert_row(committed2, 3, true, 0, 212, before[3].rowid);
        check_history(committed2, 4, 4);
        check_history(committed2, 3, 3);

        auto old = lance::Dataset::open(uri, 1);
        CHECK(old.version() == 1);
        auto old_fragment = old.open_fragment(old.fragment_ids()[0]);
        auto old_schema = old_fragment.native_schema().project({"a"});
        auto stale_writer = old_fragment.write_overlay(old_schema);
        OverlayBatch stale_batch(1, true, false);
        stale_batch.set_rowaddr(0, row_address(old.fragment_ids()[0], 1));
        stale_batch.set_a(0, 777);
        write_batch(stale_writer, stale_batch);
        auto stale_finished = stale_writer.finish();
        CHECK(stale_finished.has_value());
        auto stale_operation =
            lance::Operation::make_data_overlay({&*stale_finished});
        auto stale_transaction =
            lance::Transaction::from_operation(1, stale_operation);
        bool conflict = false;
        try {
            auto unexpected = lance::Dataset::commit(uri, stale_transaction);
            (void)unexpected;
        } catch (const lance::Error& error) {
            conflict = error.code == LANCE_ERR_COMMIT_CONFLICT;
        }
        CHECK(conflict);

        auto reopened = lance::Dataset::open(uri);
        CHECK(reopened.version() == 4);
        assert_row(reopened, 1, false, 9999, 211, before[1].rowid);
        assert_row(reopened, 2, false, 20, 200, before[2].rowid);
        assert_row(reopened, 3, true, 0, 212, before[3].rowid);

        std::fprintf(stdout, "data overlay C++ workflow passed\n");
        return 0;
    } catch (const lance::Error& error) {
        std::fprintf(stderr, "lance error %d: %s\n",
                     static_cast<int>(error.code), error.what());
        return 1;
    }
}
