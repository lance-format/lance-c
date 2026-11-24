/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the fragment domain.
#pragma once

#include "lance/detail/declarations.hpp"
#include "lance/detail/inline/udf.hpp"

namespace lance {

inline Fragment::Fragment(LanceFragment* fragment) : handle_(fragment) {}

inline uint64_t Fragment::id() const {
        uint64_t value = lance_fragment_id(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline uint64_t Fragment::data_file_count() const {
        uint64_t value = lance_fragment_data_file_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline bool Fragment::is_bound() const {
        bool value = false;
        if (lance_fragment_is_bound(handle_.get(), &value) != 0) detail::throw_error();
        return value;
    }

inline DataFile Fragment::data_file_at(size_t index) const {
        LanceDataFile* raw = nullptr;
        if (lance_fragment_data_file_at(handle_.get(), index, &raw) != 0)
            detail::throw_error();
        if (!raw) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_fragment_data_file_at returned success with "
                        "null data file");
        }
        return DataFile(raw);
    }

inline std::optional<DataFile> Fragment::data_file_for_field(int32_t field_id) const {
        LanceDataFile* raw = nullptr;
        if (lance_fragment_data_file_for_field(
                handle_.get(), field_id, &raw) != 0)
            detail::throw_error();
        if (!raw) return std::nullopt;
        return DataFile(raw);
    }

inline std::string Fragment::metadata_json() const {
        const char* raw = lance_fragment_metadata_json(handle_.get());
        return detail::take_string(raw);
    }

inline void Fragment::schema(ArrowSchema* out) const {
        if (lance_fragment_schema(handle_.get(), out) != 0) detail::throw_error();
    }

inline uint64_t Fragment::physical_rows() const {
        uint64_t value = 0;
        if (lance_fragment_physical_rows(handle_.get(), &value) != 0)
            detail::throw_error();
        return value;
    }

inline uint64_t Fragment::deleted_rows() const {
        uint64_t value = 0;
        if (lance_fragment_deleted_rows(handle_.get(), &value) != 0)
            detail::throw_error();
        return value;
    }

inline uint64_t Fragment::logical_rows() const {
        uint64_t value = 0;
        if (lance_fragment_logical_rows(handle_.get(), &value) != 0)
            detail::throw_error();
        return value;
    }

inline uint64_t Fragment::read_version() const {
        uint64_t value = 0;
        if (lance_fragment_read_version(handle_.get(), &value) != 0)
            detail::throw_error();
        return value;
    }

inline uint64_t Fragment::count_rows(std::optional<std::string> filter) const {
        uint64_t value = 0;
        if (lance_fragment_count_rows(
                handle_.get(), filter ? filter->c_str() : nullptr, &value) != 0)
            detail::throw_error();
        return value;
    }

inline const LanceFragment* Fragment::c_handle() const { return handle_.get(); }

inline Fragment Fragment::from_data_files(
    uint64_t fragment_id,
    const std::vector<const DataFile*>& data_files,
    std::optional<uint64_t> physical_rows) {
    std::vector<const LanceDataFile*> handles;
    handles.reserve(data_files.size());
    for (const auto* file : data_files) {
        handles.push_back(file ? file->c_handle() : nullptr);
    }
    uint64_t rows = physical_rows.value_or(0);
    auto* fragment = lance_fragment_new(
        fragment_id,
        handles.empty() ? nullptr : handles.data(),
        handles.size(),
        physical_rows ? &rows : nullptr);
    if (!fragment) detail::throw_error();
    return Fragment(fragment);
}

inline std::optional<Fragment> Fragment::delete_if(
    const std::string& predicate) const {
    LanceFragment* raw = nullptr;
    if (lance_fragment_delete(
            handle_.get(), predicate.c_str(), &raw) != 0) {
        detail::throw_error();
    }
    if (!raw) return std::nullopt;
    return Fragment(raw);
}

inline FragmentUpdateColumnsResult::FragmentUpdateColumnsResult(LanceFragmentUpdateColumnsResult* result) : handle_(result) {
        if (!handle_) detail::throw_error();
    }

inline Fragment FragmentUpdateColumnsResult::fragment() const {
        LanceFragment* raw = nullptr;
        if (lance_fragment_update_columns_result_fragment(
                handle_.get(), &raw) != 0) {
            detail::throw_error();
        }
        if (!raw) {
            throw Error(LANCE_ERR_INTERNAL,
                        "update-columns result fragment getter returned null");
        }
        return Fragment(raw);
    }

inline std::vector<uint32_t> FragmentUpdateColumnsResult::fields_modified() const {
        uint64_t count = 0;
        if (lance_fragment_update_columns_result_fields_modified_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        std::vector<uint32_t> out;
        out.reserve(static_cast<std::size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            uint32_t field_id = 0;
            if (lance_fragment_update_columns_result_fields_modified_at(
                    handle_.get(), static_cast<std::size_t>(i),
                    &field_id) != 0) {
                detail::throw_error();
            }
            out.push_back(field_id);
        }
        return out;
    }

inline std::vector<uint32_t> FragmentUpdateColumnsResult::matched_offsets() const {
        uint64_t count = 0;
        if (lance_fragment_update_columns_result_matched_offset_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        std::vector<uint32_t> out;
        if (count > out.max_size()) {
            throw std::length_error("matched offset count exceeds vector max_size");
        }
        out.resize(static_cast<std::size_t>(count));
        if (lance_fragment_update_columns_result_matched_offsets(
                handle_.get(), out.data(), out.size()) != 0) {
            detail::throw_error();
        }
        return out;
    }

inline std::vector<uint8_t> FragmentUpdateColumnsResult::matched_offsets_serialized() const {
        uint8_t* bytes = nullptr;
        std::size_t len = 0;
        if (lance_fragment_update_columns_result_matched_offsets_serialize(
                handle_.get(), &bytes, &len) != 0) {
            detail::throw_error();
        }
        std::unique_ptr<uint8_t, void (*)(uint8_t*)> guard(
            bytes, lance_free_bytes);
        if (bytes == nullptr) return {};
        return std::vector<uint8_t>(bytes, bytes + len);
    }

inline const LanceFragmentUpdateColumnsResult* FragmentUpdateColumnsResult::c_handle() const {
        return handle_.get();
    }

inline LanceSchema::LanceSchema(::LanceSchema* schema) : handle_(schema) {
        if (!handle_) detail::throw_error();
    }

inline LanceSchema LanceSchema::clone() const {
        ::LanceSchema* raw = lance_schema_clone(handle_.get());
        if (!raw) detail::throw_error();
        return LanceSchema(raw);
    }

inline LanceSchema LanceSchema::project(const LanceSchema& schema,
                               const std::vector<std::string>& columns) {
        std::vector<const char*> paths;
        paths.reserve(columns.size());
        for (const auto& column : columns) paths.push_back(column.c_str());
        ::LanceSchema* raw = nullptr;
        if (lance_schema_project(
                schema.c_handle(), paths.empty() ? nullptr : paths.data(),
                paths.size(), &raw) != 0) {
            detail::throw_error();
        }
        return LanceSchema(raw);
    }

inline int32_t LanceSchema::field_id(const std::string& column) const {
        int32_t id = 0;
        if (lance_schema_field_id(
                handle_.get(), column.c_str(), &id) != 0) {
            detail::throw_error();
        }
        return id;
    }

inline void LanceSchema::to_arrow(ArrowSchema* out) const {
        if (lance_schema_to_arrow(handle_.get(), out) != 0) detail::throw_error();
    }

inline const ::LanceSchema* LanceSchema::c_handle() const { return handle_.get(); }

inline LanceSchema Fragment::native_schema() const {
    return LanceSchema::from_fragment(*this);
}

namespace fragment_mutation_detail {

inline std::pair<Fragment, LanceSchema> take_fragment_schema_pair(
    ::LanceFragment* fragment, ::LanceSchema* schema) {
    if (!fragment || !schema) {
        if (fragment) lance_fragment_close(fragment);
        if (schema) lance_schema_close(schema);
        throw Error(LANCE_ERR_INTERNAL,
                    "fragment mutation returned a null success output");
    }
    return std::pair<Fragment, LanceSchema>(
        Fragment(fragment), LanceSchema(schema));
}

}

namespace fragment_mutation_detail {

inline DataReplacementGroup take_replacement_group(
    LanceDataReplacementGroup* group) {
    if (!group) {
        throw Error(LANCE_ERR_INTERNAL,
                    "fragment write_columns returned a null group");
    }
    return DataReplacementGroup(group);
}

}

inline std::pair<Fragment, LanceSchema> Fragment::add_columns_sql(
    const std::vector<SqlColumn>& columns,
    std::optional<uint32_t> batch_size) const {
    std::vector<LanceSqlColumn> raw;
    raw.reserve(columns.size());
    for (const auto& column : columns) {
        LanceSqlColumn entry{};
        entry.name = column.name.c_str();
        entry.expression = column.expression.c_str();
        raw.push_back(entry);
    }
    const uint32_t batch_size_value = batch_size.value_or(0);
    const uint32_t* batch_size_ptr = batch_size ? &batch_size_value : nullptr;
    LanceFragment* out_fragment = nullptr;
    ::LanceSchema* out_schema = nullptr;
    if (lance_fragment_add_columns_sql(
            handle_.get(), raw.empty() ? nullptr : raw.data(), raw.size(),
            batch_size_ptr, &out_fragment, &out_schema) != 0) {
        detail::throw_error();
    }
    return fragment_mutation_detail::take_fragment_schema_pair(
        out_fragment, out_schema);
}

inline std::pair<Fragment, LanceSchema> Fragment::add_columns_nulls(
    const ArrowSchema* columns) const {
    LanceFragment* out_fragment = nullptr;
    ::LanceSchema* out_schema = nullptr;
    if (lance_fragment_add_columns_nulls(
            handle_.get(), columns, &out_fragment, &out_schema) != 0) {
        detail::throw_error();
    }
    return fragment_mutation_detail::take_fragment_schema_pair(
        out_fragment, out_schema);
}

inline std::pair<Fragment, LanceSchema> Fragment::add_columns_reader(
    ArrowArrayStream* stream, std::optional<uint32_t> batch_size) const {
    const uint32_t batch_size_value = batch_size.value_or(0);
    const uint32_t* batch_size_ptr = batch_size ? &batch_size_value : nullptr;
    LanceFragment* out_fragment = nullptr;
    ::LanceSchema* out_schema = nullptr;
    if (lance_fragment_add_columns_stream(
            handle_.get(), stream, batch_size_ptr, &out_fragment,
            &out_schema) != 0) {
        detail::throw_error();
    }
    return fragment_mutation_detail::take_fragment_schema_pair(
        out_fragment, out_schema);
}

inline std::pair<Fragment, LanceSchema> Fragment::add_columns_batch_udf(
    const BatchUDF& udf,
    std::optional<std::vector<std::string>> read_columns,
    std::optional<uint32_t> batch_size) const {
    callback_detail::BatchUDFCall call(
        udf, std::move(read_columns), batch_size);
    LanceFragment* out_fragment = nullptr;
    ::LanceSchema* out_schema = nullptr;
    if (lance_fragment_add_columns_batch_udf(
            handle_.get(), &call.raw, call.read_columns_ptr,
            call.batch_size_ptr, &out_fragment, &out_schema) != 0) {
        detail::throw_error();
    }
    return fragment_mutation_detail::take_fragment_schema_pair(
        out_fragment, out_schema);
}

inline std::pair<Fragment, LanceSchema> Fragment::add_columns_batch_udf(
    const BatchUDFV2& udf,
    std::optional<std::vector<std::string>> read_columns,
    std::optional<uint32_t> batch_size) const {
    callback_detail::BatchUDFCall call(
        udf, std::move(read_columns), batch_size);
    LanceFragment* out_fragment = nullptr;
    ::LanceSchema* out_schema = nullptr;
    if (lance_fragment_add_columns_batch_udf_v2(
            handle_.get(), &call.raw_v2, call.read_columns_ptr,
            call.batch_size_ptr, &out_fragment, &out_schema) != 0) {
        detail::throw_error();
    }
    return fragment_mutation_detail::take_fragment_schema_pair(
        out_fragment, out_schema);
}

inline std::pair<Fragment, LanceSchema> Fragment::merge_columns(
    ArrowArrayStream* stream,
    const std::string& left_on,
    const std::string& right_on) const {
    LanceFragment* out_fragment = nullptr;
    ::LanceSchema* out_schema = nullptr;
    if (lance_fragment_merge_columns(
            handle_.get(), stream, left_on.c_str(), right_on.c_str(),
            &out_fragment, &out_schema) != 0) {
        detail::throw_error();
    }
    return fragment_mutation_detail::take_fragment_schema_pair(
        out_fragment, out_schema);
}

inline FragmentUpdateColumnsResult Fragment::update_columns(
    ArrowArrayStream* stream,
    const std::string& left_on,
    const std::string& right_on) const {
    LanceFragmentUpdateColumnsResult* result = nullptr;
    if (lance_fragment_update_columns(
            handle_.get(), stream, left_on.c_str(), right_on.c_str(),
            &result) != 0) {
        detail::throw_error();
    }
    if (!result) {
        throw Error(LANCE_ERR_INTERNAL,
                    "fragment update_columns returned a null result");
    }
    return FragmentUpdateColumnsResult(result);
}

inline DataReplacementGroup Fragment::write_columns(
    const LanceSchema& schema, ArrowArrayStream* stream) const {
    LanceDataReplacementGroup* group = nullptr;
    if (lance_fragment_write_columns(
            handle_.get(), schema.c_handle(), stream, &group) != 0) {
        detail::throw_error();
    }
    return fragment_mutation_detail::take_replacement_group(group);
}

inline DataReplacementGroup Fragment::write_column_with_version(
    const LanceSchema& schema,
    ArrowArrayStream* stream,
    LanceFileFormatVersion file_version) const {
    LanceDataReplacementGroup* group = nullptr;
    if (lance_fragment_write_column_with_version(
            handle_.get(), schema.c_handle(), stream,
            static_cast<int32_t>(file_version), &group) != 0) {
        detail::throw_error();
    }
    return fragment_mutation_detail::take_replacement_group(group);
}

inline std::optional<Fragment> Fragment::delete_rows(
    const std::vector<uint32_t>& offsets) const {
    LanceFragment* raw = nullptr;
    if (lance_fragment_delete_rows(
            handle_.get(), offsets.empty() ? nullptr : offsets.data(),
            offsets.size(), &raw) != 0) {
        detail::throw_error();
    }
    if (!raw) return std::nullopt;
    return Fragment(raw);
}

inline LanceSchema LanceSchema::from_fragment(const Fragment& fragment) {
    ::LanceSchema* raw = nullptr;
    if (lance_fragment_schema_clone(fragment.c_handle(), &raw) != 0) {
        detail::throw_error();
    }
    return LanceSchema(raw);
}

inline LanceSchema LanceSchema::from_operation(const Operation& operation) {
    ::LanceSchema* raw = nullptr;
    if (lance_operation_schema_clone(operation.c_handle(), &raw) != 0) {
        detail::throw_error();
    }
    return LanceSchema(raw);
}

inline VectorIndexOptions& VectorIndexOptions::accelerator(std::string device) {
        runtime_hints["lancedb.accelerator"] = std::move(device);
        return *this;
    }

inline LanceSchema LanceSchema::from_dataset(const Dataset& dataset) {
    ::LanceSchema* raw = nullptr;
    if (lance_dataset_schema_clone(dataset.c_handle(), &raw) != 0) {
        detail::throw_error();
    }
    return LanceSchema(raw);
}

inline Scanner Fragment::scan() const {
    auto* scanner = lance_fragment_scan(handle_.get());
    if (!scanner) detail::throw_error();
    return Scanner(scanner);
}

inline void Fragment::take(
    const uint32_t* indices,
    size_t num_indices,
    const std::vector<std::string>& columns,
    ArrowArrayStream* out) const {
    std::vector<const char*> column_ptrs;
    column_ptrs.reserve(columns.size() + 1);
    for (const auto& column : columns) column_ptrs.push_back(column.c_str());
    column_ptrs.push_back(nullptr);

    const char* const empty_projection[] = {nullptr};
    const char* const* columns_ptr =
        columns.empty() ? empty_projection : column_ptrs.data();
    if (lance_fragment_take(
            handle_.get(), indices, num_indices, columns_ptr, out) != 0) {
        detail::throw_error();
    }
}

inline void Fragment::take(
    const uint32_t* indices,
    size_t num_indices,
    ArrowArrayStream* out) const {
    if (lance_fragment_take(
            handle_.get(), indices, num_indices, nullptr, out) != 0) {
        detail::throw_error();
    }
}

inline ArrowStream Fragment::take(
    const uint32_t* indices,
    size_t num_indices,
    const std::vector<std::string>& columns) const {
    ArrowArrayStream raw{};
    take(indices, num_indices, columns, &raw);
    return ArrowStream(raw);
}

inline ArrowStream Fragment::take(
    const uint32_t* indices,
    size_t num_indices) const {
    ArrowArrayStream raw{};
    take(indices, num_indices, &raw);
    return ArrowStream(raw);
}

inline ArrowStream Fragment::take(
    std::span<const uint32_t> indices,
    const std::vector<std::string>& columns) const {
    return take(indices.data(), indices.size(), columns);
}

inline ArrowStream Fragment::take(std::span<const uint32_t> indices) const {
    return take(indices.data(), indices.size());
}

inline Fragment Fragment::create(
    const std::string& dataset_uri,
    ArrowArrayStream* stream,
    uint64_t fragment_id,
    const FragmentCreateOptions& options,
    const std::vector<std::pair<std::string, std::string>>& storage_opts,
    const Session* session) {
    // Native reads the stream schema itself, so this guard deliberately skips
    // `read_schema()` and only owns the stream until the C call takes it.
    Dataset::ArrowStreamInput input(stream);

    LanceAutoCleanupParams cleanup{};
    LanceWriteOptions c_write =
        Dataset::to_c_write_options(options.write, cleanup);
    auto kv = Dataset::to_c_key_values(storage_opts);
    LanceFragmentCreateOptions c_options{};
    c_options.schema = options.schema ? options.schema->c_handle() : nullptr;
    c_options.mode = static_cast<int32_t>(options.mode);
    c_options.write = &c_write;
    c_options.storage_opts = storage_opts.empty() ? nullptr : kv.data();
    c_options.session = session ? session->c_handle() : nullptr;

    LanceFragment* out = nullptr;
    auto* owned_stream = input.consume();
    if (lance_fragment_create(
            dataset_uri.c_str(), fragment_id, &c_options, owned_stream,
            &out) != 0) {
        detail::throw_error();
    }
    if (!out) {
        throw Error(
            LANCE_ERR_INTERNAL,
            "lance_fragment_create returned success with null out_fragment");
    }
    return Fragment(out);
}

inline std::vector<Fragment> Fragment::create_many(
    const std::string& dataset_uri,
    ArrowArrayStream* stream,
    const FragmentCreateOptions& options,
    const std::vector<std::pair<std::string, std::string>>& storage_opts,
    const Session* session) {
    Dataset::ArrowStreamInput input(stream);

    LanceAutoCleanupParams cleanup{};
    LanceWriteOptions c_write =
        Dataset::to_c_write_options(options.write, cleanup);
    auto kv = Dataset::to_c_key_values(storage_opts);
    LanceFragmentCreateOptions c_options{};
    c_options.schema = options.schema ? options.schema->c_handle() : nullptr;
    c_options.mode = static_cast<int32_t>(options.mode);
    c_options.write = &c_write;
    c_options.storage_opts = storage_opts.empty() ? nullptr : kv.data();
    c_options.session = session ? session->c_handle() : nullptr;

    LanceFragment** raw = nullptr;
    size_t count = 0;
    auto* owned_stream = input.consume();
    if (lance_fragment_create_many(
            dataset_uri.c_str(), &c_options, owned_stream, &raw, &count) !=
        0) {
        detail::throw_error();
    }
    if (count > 0 && !raw) {
        throw Error(LANCE_ERR_INTERNAL,
                    "lance_fragment_create_many returned success with a null "
                    "fragment array");
    }

    std::vector<Fragment> fragments;
    try {
        // Reserve before adopting: `Fragment` only moves a pointer, so once
        // the capacity exists `emplace_back` cannot reallocate. Each slot is
        // cleared right after adoption, which leaves `raw` holding exactly the
        // handles this vector does not own.
        fragments.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            fragments.emplace_back(raw[i]);
            raw[i] = nullptr;
        }
    } catch (...) {
        // Frees every slot that was not adopted; adopted slots are null.
        lance_fragment_array_free(raw, count);
        throw;
    }
    lance_fragment_array_free(raw, count);
    return fragments;
}

inline Fragment Fragment::create_from_file(
    const Dataset& dataset,
    const std::string& filename,
    uint64_t fragment_id,
    std::optional<uint64_t> physical_rows) {
    uint64_t rows = physical_rows.value_or(0);
    LanceFragment* out = nullptr;
    if (lance_fragment_create_from_file(
            dataset.c_handle(),
            filename.c_str(),
            fragment_id,
            physical_rows ? &rows : nullptr,
            &out) != 0) {
        detail::throw_error();
    }
    if (!out) {
        throw Error(
            LANCE_ERR_INTERNAL,
            "lance_fragment_create_from_file returned success with null "
            "out_fragment");
    }
    return Fragment(out);
}

inline FragmentOverlayWriter Fragment::write_overlay(
    const LanceSchema& schema) const {
    LanceFragmentOverlayWriter* raw = nullptr;
    if (lance_fragment_overlay_writer_new(
            handle_.get(), schema.c_handle(), &raw) != 0) {
        detail::throw_error();
    }
    if (!raw) {
        throw Error(LANCE_ERR_INTERNAL,
                    "native overlay writer returned NULL on success");
    }
    return FragmentOverlayWriter(raw);
}

inline void write_fragments(
    const std::string& uri,
    const ArrowSchema* schema,
    ArrowArrayStream* stream,
    const std::vector<std::pair<std::string, std::string>>& storage_opts) {
    std::vector<const char*> kv;
    for (auto& [k, v] : storage_opts) {
        kv.push_back(k.c_str());
        kv.push_back(v.c_str());
    }
    kv.push_back(nullptr);

    const char* const* opts_ptr = storage_opts.empty() ? nullptr : kv.data();
    if (lance_write_fragments(uri.c_str(), schema, stream, opts_ptr) != 0) {
        detail::throw_error();
    }
}

inline LanceSchema LanceSchema::from_arrow(const ArrowSchema* schema) {
    ::LanceSchema* raw = nullptr;
    if (lance_schema_from_arrow(schema, &raw) != 0) detail::throw_error();
    return LanceSchema(raw);
}

}  // namespace lance
