/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the operation domain.
#pragma once

#include "lance/detail/declarations.hpp"

namespace lance {

inline UpdatedFragmentOffsets::UpdatedFragmentOffsets(LanceUpdatedFragmentOffsets* offsets) : handle_(offsets) {}

inline UpdatedFragmentOffsets::UpdatedFragmentOffsets(
        const std::vector<std::pair<uint64_t, std::vector<uint32_t>>>& entries) : handle_(nullptr) {
        std::vector<::LanceFragmentOffsetsView> views;
        views.reserve(entries.size());
        for (const auto& entry : entries) {
            ::LanceFragmentOffsetsView view{};
            view.fragment_id = entry.first;
            view.offsets = entry.second.empty() ? nullptr
                                                : entry.second.data();
            view.offset_count = entry.second.size();
            views.push_back(view);
        }
        auto* raw = lance_updated_fragment_offsets_new(
            views.empty() ? nullptr : views.data(), views.size());
        if (!raw) detail::throw_error();
        handle_ =
            Handle<LanceUpdatedFragmentOffsets,
                   lance_updated_fragment_offsets_close>(raw);
    }

inline size_t UpdatedFragmentOffsets::size() const {
        uint64_t count = 0;
        if (lance_updated_fragment_offsets_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return static_cast<size_t>(count);
    }

inline uint64_t UpdatedFragmentOffsets::fragment_id_at(size_t index) const {
        uint64_t id = 0;
        if (lance_updated_fragment_offsets_fragment_id_at(
                handle_.get(), index, &id) != 0) {
            detail::throw_error();
        }
        return id;
    }

inline size_t UpdatedFragmentOffsets::offset_count_at(size_t index) const {
        uint64_t count = 0;
        if (lance_updated_fragment_offsets_offset_count_at(
                handle_.get(), index, &count) != 0) {
            detail::throw_error();
        }
        return static_cast<size_t>(count);
    }

inline std::vector<uint32_t> UpdatedFragmentOffsets::offsets_at(size_t index) const {
        std::vector<uint32_t> out;
        const size_t count = offset_count_at(index);
        if (count > out.max_size()) {
            throw std::length_error(
                "updated fragment offset count exceeds vector max_size");
        }
        out.resize(count);
        if (lance_updated_fragment_offsets_copy_at(
                handle_.get(), index, out.data(), out.size()) != 0) {
            detail::throw_error();
        }
        return out;
    }

inline UpdatedFragmentOffsets UpdatedFragmentOffsets::clone() const {
        auto* raw = lance_updated_fragment_offsets_clone(handle_.get());
        if (!raw) detail::throw_error();
        return UpdatedFragmentOffsets(raw);
    }

inline const LanceUpdatedFragmentOffsets* UpdatedFragmentOffsets::c_handle() const {
        return handle_.get();
    }

inline KeyExistenceFilter::KeyExistenceFilter(LanceKeyExistenceFilter* filter) : handle_(filter) {}

inline KeyExistenceFilter KeyExistenceFilter::from_proto(const std::vector<uint8_t>& bytes) {
        LanceKeyExistenceFilter* filter = nullptr;
        if (lance_key_existence_filter_from_proto(
                bytes.empty() ? nullptr : bytes.data(), bytes.size(),
                &filter) != 0) {
            detail::throw_error();
        }
        return KeyExistenceFilter(filter);
    }

inline LanceKeyExistenceFilterKind KeyExistenceFilter::kind() const {
        LanceKeyExistenceFilterKind kind = LANCE_KEY_EXISTENCE_FILTER_EXACT;
        if (lance_key_existence_filter_kind(handle_.get(), &kind) != 0) {
            detail::throw_error();
        }
        return kind;
    }

inline std::vector<int32_t> KeyExistenceFilter::field_ids() const {
        uint64_t count = 0;
        if (lance_key_existence_filter_field_id_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        std::vector<int32_t> out;
        out.reserve(static_cast<size_t>(count));
        for (size_t index = 0; index < count; ++index) {
            int32_t field_id = 0;
            if (lance_key_existence_filter_field_id_at(
                    handle_.get(), index, &field_id) != 0) {
                detail::throw_error();
            }
            out.push_back(field_id);
        }
        return out;
    }

inline std::vector<uint64_t> KeyExistenceFilter::exact_keys() const {
        uint64_t count = 0;
        if (lance_key_existence_filter_exact_count(handle_.get(), &count) !=
            0) {
            detail::throw_error();
        }
        std::vector<uint64_t> out;
        out.reserve(static_cast<size_t>(count));
        for (size_t index = 0; index < count; ++index) {
            uint64_t key = 0;
            if (lance_key_existence_filter_exact_key_at(
                    handle_.get(), index, &key) != 0) {
                detail::throw_error();
            }
            out.push_back(key);
        }
        return out;
    }

inline BloomFilterValues KeyExistenceFilter::bloom() const {
        uint8_t* bitmap = nullptr;
        size_t bitmap_len = 0;
        BloomFilterValues out;
        if (lance_key_existence_filter_bloom(
                handle_.get(), &bitmap, &bitmap_len, &out.num_bits,
                &out.number_of_items, &out.probability) != 0) {
            detail::throw_error();
        }
        if (bitmap) {
            detail::OwnedBytes bitmap_guard(bitmap);
            out.bitmap.assign(bitmap, bitmap + bitmap_len);
        }
        return out;
    }

inline std::vector<uint8_t> KeyExistenceFilter::to_proto() const {
        uint8_t* bytes = nullptr;
        size_t len = 0;
        if (lance_key_existence_filter_to_proto(
                handle_.get(), &bytes, &len) != 0) {
            detail::throw_error();
        }
        return detail::take_bytes(bytes, len);
    }

inline KeyExistenceFilter KeyExistenceFilter::clone() const {
        auto* raw = lance_key_existence_filter_clone(handle_.get());
        if (!raw) detail::throw_error();
        return KeyExistenceFilter(raw);
    }

inline const LanceKeyExistenceFilter* KeyExistenceFilter::c_handle() const {
        return handle_.get();
    }

inline RewriteGroup::RewriteGroup(LanceRewriteGroup* group) : handle_(group) {}

inline RewriteGroup RewriteGroup::clone() const {
        auto* raw = lance_rewrite_group_clone(handle_.get());
        if (!raw) detail::throw_error();
        return RewriteGroup(raw);
    }

inline const LanceRewriteGroup* RewriteGroup::c_handle() const { return handle_.get(); }

inline RewrittenIndex::RewrittenIndex(LanceRewrittenIndex* index) : handle_(index) {}

inline std::array<uint8_t, 16> RewrittenIndex::old_id() const {
        std::array<uint8_t, 16> out = {};
        if (lance_rewritten_index_old_id(handle_.get(), out.data()) != 0)
            detail::throw_error();
        return out;
    }

inline std::array<uint8_t, 16> RewrittenIndex::new_id() const {
        std::array<uint8_t, 16> out = {};
        if (lance_rewritten_index_new_id(handle_.get(), out.data()) != 0)
            detail::throw_error();
        return out;
    }

inline std::string RewrittenIndex::details_type_url() const {
        const char* raw = lance_rewritten_index_details_type_url(handle_.get());
        return detail::take_string(raw);
    }

inline std::vector<uint8_t> RewrittenIndex::details_value() const {
        uint8_t* bytes = nullptr;
        size_t len = 0;
        if (lance_rewritten_index_details_value(
                handle_.get(), &bytes, &len) != 0) {
            detail::throw_error();
        }
        return detail::take_bytes(bytes, len);
    }

inline uint32_t RewrittenIndex::version() const {
        uint32_t version = 0;
        if (lance_rewritten_index_version(handle_.get(), &version) != 0)
            detail::throw_error();
        return version;
    }

inline bool RewrittenIndex::has_files() const {
        bool present = false;
        if (lance_rewritten_index_has_files(handle_.get(), &present) != 0)
            detail::throw_error();
        return present;
    }

inline std::vector<IndexFile> RewrittenIndex::files() const {
        uint64_t count = 0;
        if (lance_rewritten_index_file_count(handle_.get(), &count) != 0)
            detail::throw_error();
        std::vector<IndexFile> out;
        out.reserve(static_cast<size_t>(count));
        for (size_t index = 0; index < count; ++index) {
            const char* path =
                lance_rewritten_index_file_path_at(handle_.get(), index);
            if (!path) detail::throw_error();
            detail::OwnedString path_guard(path);
            IndexFile file;
            file.path.assign(path);
            if (lance_rewritten_index_file_size_at(
                    handle_.get(), index, &file.size_bytes) != 0) {
                detail::throw_error();
            }
            out.push_back(std::move(file));
        }
        return out;
    }

inline RewrittenIndex RewrittenIndex::clone() const {
        auto* raw = lance_rewritten_index_clone(handle_.get());
        if (!raw) detail::throw_error();
        return RewrittenIndex(raw);
    }

inline const LanceRewrittenIndex* RewrittenIndex::c_handle() const { return handle_.get(); }

inline Operation::Operation(LanceOperation* operation) : handle_(operation) {}

inline OperationKind Operation::kind() const {
        LanceOperationKind raw{};
        if (lance_operation_kind(handle_.get(), &raw) != 0) detail::throw_error();
        return static_cast<OperationKind>(raw);
    }

inline uint64_t Operation::fragment_count() const {
        uint64_t count = 0;
        if (lance_operation_fragment_count(handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline bool Operation::preserves_nullability() const {
        bool value = false;
        if (lance_operation_preserves_nullability(
                handle_.get(), &value) != 0) {
            detail::throw_error();
        }
        return value;
    }

inline uint64_t Operation::append_fragment_count() const {
        uint64_t count = 0;
        if (lance_operation_append_fragment_count(handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline uint64_t Operation::restore_version() const {
        uint64_t version = 0;
        if (lance_operation_restore_version(handle_.get(), &version) != 0) {
            detail::throw_error();
        }
        return version;
    }

inline uint32_t Operation::reserve_fragments_count() const {
        uint32_t count = 0;
        if (lance_operation_reserve_fragments_count(handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline uint64_t Operation::update_bases_count() const {
        uint64_t count = 0;
        if (lance_operation_update_bases_count(handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline LanceBasePathView Operation::update_bases_at(size_t index) const {
        LanceBasePathView base{};
        if (lance_operation_update_bases_at(handle_.get(), index, &base) != 0) {
            detail::throw_error();
        }
        return base;
    }

inline uint64_t Operation::overwrite_fragment_count() const {
        uint64_t count = 0;
        if (lance_operation_overwrite_fragment_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline bool Operation::has_config_upsert_values() const {
        bool present = false;
        if (lance_operation_overwrite_has_config_upsert_values(
                handle_.get(), &present) != 0) {
            detail::throw_error();
        }
        return present;
    }

inline uint64_t Operation::config_upsert_count() const {
        uint64_t count = 0;
        if (lance_operation_overwrite_config_upsert_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline std::string Operation::config_upsert_key_at(size_t index) const {
        const char* key = lance_operation_overwrite_config_upsert_key_at(
            handle_.get(), index);
        if (!key) detail::throw_error();
        return std::string(key);
    }

inline std::string Operation::config_upsert_value_at(size_t index) const {
        const char* value = lance_operation_overwrite_config_upsert_value_at(
            handle_.get(), index);
        if (!value) detail::throw_error();
        return std::string(value);
    }

inline bool Operation::has_initial_bases() const {
        bool present = false;
        if (lance_operation_overwrite_has_initial_bases(
                handle_.get(), &present) != 0) {
            detail::throw_error();
        }
        return present;
    }

inline uint64_t Operation::initial_base_count() const {
        uint64_t count = 0;
        if (lance_operation_overwrite_initial_base_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline LanceBasePathView Operation::initial_base_at(size_t index) const {
        LanceBasePathView base{};
        if (lance_operation_overwrite_initial_base_at(
                handle_.get(), index, &base) != 0) {
            detail::throw_error();
        }
        return base;
    }

inline std::vector<BasePath> Operation::initial_bases() const {
        std::vector<BasePath> out;
        const uint64_t count = initial_base_count();
        out.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            LanceBasePathView view = initial_base_at(static_cast<size_t>(i));
            BasePath base;
            base.id = view.id;
            base.path = view.path ? view.path : "";
            base.name = view.name ? std::optional<std::string>(view.name)
                                  : std::nullopt;
            base.is_dataset_root = view.is_dataset_root;
            out.push_back(std::move(base));
        }
        return out;
    }

inline uint64_t Operation::merge_fragment_count() const {
        uint64_t count = 0;
        if (lance_operation_merge_fragment_count(handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline bool Operation::merge_preserves_nullability() const {
        bool value = false;
        if (lance_operation_merge_preserves_nullability(
                handle_.get(), &value) != 0) {
            detail::throw_error();
        }
        return value;
    }

inline bool Operation::clone_is_shallow() const {
        bool value = false;
        if (lance_operation_clone_is_shallow(handle_.get(), &value) != 0) {
            detail::throw_error();
        }
        return value;
    }

inline std::optional<std::string> Operation::clone_ref_name() const {
        bool present = false;
        const char* value = nullptr;
        if (lance_operation_clone_ref_name(
                handle_.get(), &present, &value) != 0) {
            detail::throw_error();
        }
        if (!present) return std::nullopt;
        return std::string(value);
    }

inline uint64_t Operation::clone_ref_version() const {
        uint64_t version = 0;
        if (lance_operation_clone_ref_version(handle_.get(), &version) != 0) {
            detail::throw_error();
        }
        return version;
    }

inline std::string Operation::clone_ref_path() const {
        const char* path = lance_operation_clone_ref_path(handle_.get());
        if (!path) detail::throw_error();
        return std::string(path);
    }

inline std::optional<std::string> Operation::clone_branch_name() const {
        bool present = false;
        const char* value = nullptr;
        if (lance_operation_clone_branch_name(
                handle_.get(), &present, &value) != 0) {
            detail::throw_error();
        }
        if (!present) return std::nullopt;
        return std::string(value);
    }

inline std::optional<UpdateMap> Operation::config_updates() const {
        LanceUpdateMap* map = nullptr;
        if (lance_operation_update_config_config_updates(
                handle_.get(), &map) != 0) {
            detail::throw_error();
        }
        if (!map) return std::nullopt;
        return UpdateMap(map);
    }

inline std::optional<UpdateMap> Operation::table_metadata_updates() const {
        LanceUpdateMap* map = nullptr;
        if (lance_operation_update_config_table_metadata_updates(
                handle_.get(), &map) != 0) {
            detail::throw_error();
        }
        if (!map) return std::nullopt;
        return UpdateMap(map);
    }

inline std::optional<UpdateMap> Operation::schema_metadata_updates() const {
        LanceUpdateMap* map = nullptr;
        if (lance_operation_update_config_schema_metadata_updates(
                handle_.get(), &map) != 0) {
            detail::throw_error();
        }
        if (!map) return std::nullopt;
        return UpdateMap(map);
    }

inline std::map<int32_t, UpdateMap> Operation::field_metadata_updates() const {
        uint64_t count = 0;
        if (lance_operation_update_config_field_metadata_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        std::map<int32_t, UpdateMap> out;
        for (uint64_t i = 0; i < count; ++i) {
            int32_t field_id = 0;
            LanceUpdateMap* map = nullptr;
            if (lance_operation_update_config_field_metadata_at(
                    handle_.get(), static_cast<size_t>(i), &field_id, &map) !=
                0) {
                detail::throw_error();
            }
            if (!map) {
                throw Error(LANCE_ERR_INTERNAL,
                            "UpdateConfig field metadata returned a null map");
            }
            out.emplace(field_id, UpdateMap(map));
        }
        return out;
    }

inline Operation Operation::make_restore(uint64_t version) {
        auto* operation = lance_operation_restore_new(version);
        if (!operation) detail::throw_error();
        return Operation(operation);
    }

inline Operation Operation::make_reserve_fragments(uint32_t num_fragments) {
        auto* operation = lance_operation_reserve_fragments_new(num_fragments);
        if (!operation) detail::throw_error();
        return Operation(operation);
    }

inline uint64_t Operation::data_replacement_count() const {
        uint64_t count = 0;
        if (lance_operation_data_replacement_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline DataReplacementGroup Operation::data_replacement_at(size_t index) const {
        LanceDataReplacementGroup* group = nullptr;
        if (lance_operation_data_replacement_at(
                handle_.get(), index, &group) != 0) {
            detail::throw_error();
        }
        return DataReplacementGroup(group);
    }

inline uint64_t Operation::data_overlay_group_count() const {
        uint64_t count = 0;
        if (lance_operation_data_overlay_group_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline DataOverlayGroup Operation::data_overlay_group_at(size_t index) const {
        LanceDataOverlayGroup* group = nullptr;
        if (lance_operation_data_overlay_group_at(
                handle_.get(), index, &group) != 0) {
            detail::throw_error();
        }
        return DataOverlayGroup(group);
    }

inline uint64_t Operation::delete_updated_fragment_count() const {
        uint64_t count = 0;
        if (lance_operation_delete_updated_fragment_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline std::string Operation::delete_updated_fragment_metadata_json(size_t index) const {
        const char* raw = lance_operation_delete_updated_fragment_metadata_json_at(
            handle_.get(), index);
        return detail::take_string(raw);
    }

inline uint64_t Operation::delete_deleted_fragment_count() const {
        uint64_t count = 0;
        if (lance_operation_delete_deleted_fragment_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline uint64_t Operation::delete_deleted_fragment_id(size_t index) const {
        uint64_t id = 0;
        if (lance_operation_delete_deleted_fragment_id_at(
                handle_.get(), index, &id) != 0) {
            detail::throw_error();
        }
        return id;
    }

inline std::string Operation::delete_predicate() const {
        const char* raw = lance_operation_delete_predicate(handle_.get());
        return detail::take_string(raw);
    }

inline uint64_t Operation::removed_fragment_count() const {
        uint64_t count = 0;
        if (lance_operation_update_removed_fragment_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline uint64_t Operation::removed_fragment_id(size_t index) const {
        uint64_t id = 0;
        if (lance_operation_update_removed_fragment_id_at(
                handle_.get(), index, &id) != 0) {
            detail::throw_error();
        }
        return id;
    }

inline uint64_t Operation::updated_fragment_count() const {
        uint64_t count = 0;
        if (lance_operation_update_updated_fragment_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline uint64_t Operation::new_fragment_count() const {
        uint64_t count = 0;
        if (lance_operation_update_new_fragment_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline uint64_t Operation::fields_modified_count() const {
        uint64_t count = 0;
        if (lance_operation_update_fields_modified_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline uint32_t Operation::fields_modified(size_t index) const {
        uint32_t field_id = 0;
        if (lance_operation_update_fields_modified_at(
                handle_.get(), index, &field_id) != 0) {
            detail::throw_error();
        }
        return field_id;
    }

inline uint64_t Operation::compacted_sstable_count() const {
        uint64_t count = 0;
        if (lance_operation_update_compacted_sstable_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline uint64_t Operation::fields_for_preserving_frag_bitmap_count() const {
        uint64_t count = 0;
        if (lance_operation_update_fields_for_preserving_frag_bitmap_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline uint32_t Operation::fields_for_preserving_frag_bitmap(size_t index) const {
        uint32_t field_id = 0;
        if (lance_operation_update_fields_for_preserving_frag_bitmap_at(
                handle_.get(), index, &field_id) != 0) {
            detail::throw_error();
        }
        return field_id;
    }

inline std::optional<UpdateMode> Operation::update_mode() const {
        bool present = false;
        int32_t mode = 0;
        if (lance_operation_update_mode(
                handle_.get(), &present, &mode) != 0) {
            detail::throw_error();
        }
        if (!present) return std::nullopt;
        return static_cast<UpdateMode>(mode);
    }

inline uint64_t Operation::rewrite_group_count() const {
        uint64_t count = 0;
        if (lance_operation_rewrite_group_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline uint64_t Operation::rewritten_index_count() const {
        uint64_t count = 0;
        if (lance_operation_rewrite_rewritten_index_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline uint64_t Operation::update_mem_wal_state_compacted_sstable_count() const {
        uint64_t count = 0;
        if (lance_operation_update_mem_wal_state_compacted_sstable_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline const LanceOperation* Operation::c_handle() const { return handle_.get(); }

inline Transaction::Transaction(LanceTransaction* transaction) : handle_(transaction) {}

inline uint64_t Transaction::read_version() const {
        uint64_t version = 0;
        if (lance_transaction_read_version(handle_.get(), &version) != 0) {
            detail::throw_error();
        }
        return version;
    }

inline std::string Transaction::uuid() const {
        const char* uuid = lance_transaction_uuid(handle_.get());
        return detail::take_string(uuid);
    }

inline std::optional<std::string> Transaction::tag() const {
        const char* tag = nullptr;
        int32_t present = lance_transaction_tag(handle_.get(), &tag);
        if (present < 0) detail::throw_error();
        if (present == 0) {
            if (tag) {
                detail::OwnedString tag_guard(tag);
                throw Error(LANCE_ERR_INTERNAL,
                            "lance_transaction_tag reported absent with a non-null pointer");
            }
            return std::nullopt;
        }
        if (!tag) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_transaction_tag reported present with a null pointer");
        }
        return detail::take_string(tag);
    }

inline bool Transaction::has_properties() const {
        bool present = false;
        if (lance_transaction_has_properties(handle_.get(), &present) != 0) {
            detail::throw_error();
        }
        return present;
    }

inline std::map<std::string, std::string> Transaction::properties() const {
        uint64_t count = 0;
        if (lance_transaction_property_count(handle_.get(), &count) != 0) {
            detail::throw_error();
        }

        std::map<std::string, std::string> out;
        for (uint64_t i = 0; i < count; ++i) {
            const char* key = lance_transaction_property_key_at(
                handle_.get(), static_cast<size_t>(i));
            if (!key) detail::throw_error();
            detail::OwnedString key_guard(key);
            const char* value = lance_transaction_property_value_at(
                handle_.get(), static_cast<size_t>(i));
            if (!value) detail::throw_error();
            detail::OwnedString value_guard(value);
            out.emplace(key, value);
        }
        return out;
    }

inline Operation Transaction::operation() const {
        LanceOperation* raw = nullptr;
        if (lance_transaction_operation(handle_.get(), &raw) != 0) {
            detail::throw_error();
        }
        if (!raw) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_transaction_operation returned success with null output");
        }
        return Operation(raw);
    }

inline Transaction Transaction::from_operation(
        uint64_t read_version,
        const Operation& operation,
        const TransactionOptions& options) {
        std::vector<const char*> property_keys;
        std::vector<const char*> property_values;
        LanceTransactionOptions raw{};
        if (options.uuid) raw.uuid = options.uuid->c_str();
        if (options.tag) raw.tag = options.tag->c_str();
        if (options.transaction_properties) {
            const auto& entries = *options.transaction_properties;
            property_keys.reserve(entries.size());
            property_values.reserve(entries.size());
            for (const auto& [key, value] : entries) {
                property_keys.push_back(key.c_str());
                property_values.push_back(value.c_str());
            }
            raw.has_transaction_properties = true;
            raw.property_keys =
                property_keys.empty() ? nullptr : property_keys.data();
            raw.property_values =
                property_values.empty() ? nullptr : property_values.data();
            raw.property_count = entries.size();
        }

        auto* transaction = lance_transaction_new(
            read_version, operation.c_handle(), &raw);
        if (!transaction) detail::throw_error();
        return Transaction(transaction);
    }

inline std::vector<uint8_t> Transaction::to_proto() const {
        uint8_t* bytes = nullptr;
        size_t len = 0;
        if (lance_transaction_to_proto(handle_.get(), &bytes, &len) != 0) {
            detail::throw_error();
        }
        if (len > 0 && !bytes) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_transaction_to_proto returned a null buffer");
        }
        return detail::take_bytes(bytes, len);
    }

inline Transaction Transaction::from_proto(const std::vector<uint8_t>& bytes) {
        auto* raw = lance_transaction_from_proto(bytes.data(), bytes.size());
        if (!raw) detail::throw_error();
        return Transaction(raw);
    }

inline const LanceTransaction* Transaction::c_handle() const { return handle_.get(); }

inline Operation Operation::make_project(
    const LanceSchema& schema, bool preserves_nullability) {
    LanceOperation* raw = nullptr;
    if (lance_operation_project_new(
            schema.c_handle(), preserves_nullability, &raw) != 0) {
        detail::throw_error();
    }
    return Operation(raw);
}

inline Fragment Operation::append_fragment_at(size_t index) const {
    LanceFragment* raw = nullptr;
    if (lance_operation_append_fragment_at(handle_.get(), index, &raw) != 0)
        detail::throw_error();
    if (!raw) {
        throw Error(LANCE_ERR_INTERNAL,
                    "lance_operation_append_fragment_at returned success "
                    "with null fragment");
    }
    return Fragment(raw);
}

inline std::vector<BasePath> Operation::update_bases() const {
    uint64_t count = 0;
    if (lance_operation_update_bases_count(handle_.get(), &count) != 0)
        detail::throw_error();

    std::vector<BasePath> out;
    out.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        LanceBasePathView view{};
        if (lance_operation_update_bases_at(
                handle_.get(), static_cast<size_t>(i), &view) != 0)
            detail::throw_error();
        BasePath base;
        base.id = view.id;
        base.path = view.path ? view.path : "";
        base.name = view.name ? std::optional<std::string>(view.name)
                              : std::nullopt;
        base.is_dataset_root = view.is_dataset_root;
        out.push_back(std::move(base));
    }
    return out;
}

inline Fragment Operation::overwrite_fragment_at(size_t index) const {
    LanceFragment* raw = nullptr;
    if (lance_operation_overwrite_fragment_at(handle_.get(), index, &raw) != 0)
        detail::throw_error();
    if (!raw) {
        throw Error(LANCE_ERR_INTERNAL,
                    "lance_operation_overwrite_fragment_at returned success "
                    "with null fragment");
    }
    return Fragment(raw);
}

inline LanceSchema Operation::overwrite_schema() const {
    ::LanceSchema* raw = nullptr;
    if (lance_operation_overwrite_schema_clone(handle_.get(), &raw) != 0)
        detail::throw_error();
    if (!raw) {
        throw Error(LANCE_ERR_INTERNAL,
                    "lance_operation_overwrite_schema_clone returned success "
                    "with null schema");
    }
    return LanceSchema(raw);
}

inline Fragment Operation::merge_fragment_at(size_t index) const {
    LanceFragment* raw = nullptr;
    if (lance_operation_merge_fragment_at(handle_.get(), index, &raw) != 0)
        detail::throw_error();
    if (!raw) {
        throw Error(LANCE_ERR_INTERNAL,
                    "lance_operation_merge_fragment_at returned success "
                    "with null fragment");
    }
    return Fragment(raw);
}

inline LanceSchema Operation::merge_schema() const {
    ::LanceSchema* raw = nullptr;
    if (lance_operation_merge_schema_clone(handle_.get(), &raw) != 0)
        detail::throw_error();
    if (!raw) {
        throw Error(LANCE_ERR_INTERNAL,
                    "lance_operation_merge_schema_clone returned success "
                    "with null schema");
    }
    return LanceSchema(raw);
}

inline LanceSchema Operation::project_schema() const {
    ::LanceSchema* raw = nullptr;
    if (lance_operation_project_schema_clone(handle_.get(), &raw) != 0)
        detail::throw_error();
    if (!raw) {
        throw Error(LANCE_ERR_INTERNAL,
                    "lance_operation_project_schema_clone returned success "
                    "with null schema");
    }
    return LanceSchema(raw);
}

inline Operation Operation::make_overwrite(
    const LanceSchema& schema,
    const std::vector<const Fragment*>& fragments,
    std::optional<std::vector<std::pair<std::string, std::string>>>
        config_upsert_values,
    std::optional<std::vector<BasePath>> initial_bases) {
    std::vector<const LanceFragment*> fragment_handles;
    fragment_handles.reserve(fragments.size());
    for (const Fragment* fragment : fragments) {
        if (!fragment) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "Operation::make_overwrite received a null fragment");
        }
        fragment_handles.push_back(fragment->c_handle());
    }

    // The native constructor copies all strings before returning.
    std::vector<std::string> config_keys;
    std::vector<std::string> config_values;
    std::vector<const char*> config_key_ptrs;
    std::vector<const char*> config_value_ptrs;
    LanceStringMap config_storage{};
    const LanceStringMap* config_map = nullptr;
    if (config_upsert_values) {
        config_keys.reserve(config_upsert_values->size());
        config_values.reserve(config_upsert_values->size());
        for (const auto& [key, value] : *config_upsert_values) {
            config_keys.push_back(key);
            config_values.push_back(value);
        }
        for (size_t i = 0; i < config_keys.size(); ++i) {
            config_key_ptrs.push_back(config_keys[i].c_str());
            config_value_ptrs.push_back(config_values[i].c_str());
        }
        config_storage.keys =
            config_key_ptrs.empty() ? nullptr : config_key_ptrs.data();
        config_storage.values =
            config_value_ptrs.empty() ? nullptr : config_value_ptrs.data();
        config_storage.count = config_key_ptrs.size();
        config_map = &config_storage;
    }

    std::vector<LanceBasePath> base_views;
    LanceBasePath empty_base{};
    LanceBasePathList base_list{};
    const LanceBasePathList* initial_base_list = nullptr;
    if (initial_bases) {
        base_views.reserve(initial_bases->size());
        for (const BasePath& base : *initial_bases) {
            LanceBasePath view{};
            view.id = base.id;
            view.path = base.path.c_str();
            view.name = base.name ? base.name->c_str() : nullptr;
            view.is_dataset_root = base.is_dataset_root;
            base_views.push_back(view);
        }
        base_list.items =
            base_views.empty() ? &empty_base : base_views.data();
        base_list.count = base_views.size();
        initial_base_list = &base_list;
    }

    auto* raw = lance_operation_overwrite_new(
        schema.c_handle(),
        fragment_handles.empty() ? nullptr : fragment_handles.data(),
        fragment_handles.size(), config_map, initial_base_list);
    if (!raw) detail::throw_error();
    return Operation(raw);
}

inline Operation Operation::make_merge(
    const LanceSchema& schema,
    const std::vector<const Fragment*>& fragments,
    bool preserves_nullability) {
    std::vector<const LanceFragment*> fragment_handles;
    fragment_handles.reserve(fragments.size());
    for (const Fragment* fragment : fragments) {
        if (!fragment) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "Operation::make_merge received a null fragment");
        }
        fragment_handles.push_back(fragment->c_handle());
    }
    auto* raw = lance_operation_merge_new(
        schema.c_handle(),
        fragment_handles.empty() ? nullptr : fragment_handles.data(),
        fragment_handles.size(), preserves_nullability);
    if (!raw) detail::throw_error();
    return Operation(raw);
}

inline Operation Operation::make_clone(
    bool is_shallow, std::optional<std::string> ref_name,
    uint64_t ref_version, std::string ref_path,
    std::optional<std::string> branch_name) {
    auto* raw = lance_operation_clone_new(
        is_shallow, ref_name ? ref_name->c_str() : nullptr, ref_version,
        ref_path.c_str(), branch_name ? branch_name->c_str() : nullptr);
    if (!raw) detail::throw_error();
    return Operation(raw);
}

inline Operation Operation::make_update_config(
    const UpdateConfigOptions& options) {
    std::vector<LanceFieldMetadataUpdateMap> field_updates;
    field_updates.reserve(options.field_metadata_updates.size());
    for (const auto& [field_id, updates] : options.field_metadata_updates) {
        LanceFieldMetadataUpdateMap entry{};
        entry.field_id = field_id;
        entry.updates = updates.c_handle();
        field_updates.push_back(entry);
    }

    LanceUpdateConfigOperationOptions c_options{};
    c_options.config_updates =
        options.config_updates ? options.config_updates->c_handle() : nullptr;
    c_options.table_metadata_updates =
        options.table_metadata_updates
            ? options.table_metadata_updates->c_handle()
            : nullptr;
    c_options.schema_metadata_updates =
        options.schema_metadata_updates
            ? options.schema_metadata_updates->c_handle()
            : nullptr;
    c_options.field_metadata_updates =
        field_updates.empty() ? nullptr : field_updates.data();
    c_options.field_metadata_update_count = field_updates.size();

    auto* raw = lance_operation_update_config_new(&c_options);
    if (!raw) detail::throw_error();
    return Operation(raw);
}

inline Operation Operation::make_append(
    const std::vector<const Fragment*>& fragments) {
    std::vector<const LanceFragment*> handles;
    handles.reserve(fragments.size());
    for (const auto* fragment : fragments) {
        if (!fragment) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "append fragment must not be null");
        }
        handles.push_back(fragment->c_handle());
    }
    auto* operation = lance_operation_append_new(
        handles.empty() ? nullptr : handles.data(), handles.size());
    if (!operation) detail::throw_error();
    return Operation(operation);
}

inline Operation Operation::make_update_bases(
    const std::vector<BasePath>& new_bases) {
    std::vector<LanceBasePath> raw;
    raw.reserve(new_bases.size());
    for (const auto& base : new_bases) {
        LanceBasePath entry{};
        entry.id = base.id;
        entry.path = base.path.c_str();
        entry.name = base.name ? base.name->c_str() : nullptr;
        entry.is_dataset_root = base.is_dataset_root;
        raw.push_back(entry);
    }
    auto* operation = lance_operation_update_bases_new(
        raw.empty() ? nullptr : raw.data(), raw.size());
    if (!operation) detail::throw_error();
    return Operation(operation);
}

inline Operation Operation::make_delete(
    const std::vector<const Fragment*>& updated_fragments,
    const std::vector<uint64_t>& deleted_fragment_ids,
    const std::string& predicate) {
    std::vector<const LanceFragment*> handles;
    handles.reserve(updated_fragments.size());
    for (const auto* fragment : updated_fragments) {
        if (!fragment) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "updated fragment must not be null");
        }
        handles.push_back(fragment->c_handle());
    }
    auto* operation = lance_operation_delete_new(
        handles.empty() ? nullptr : handles.data(), handles.size(),
        deleted_fragment_ids.empty() ? nullptr : deleted_fragment_ids.data(),
        deleted_fragment_ids.size(), predicate.c_str());
    if (!operation) detail::throw_error();
    return Operation(operation);
}

inline Operation Operation::make_data_replacement(
    const std::vector<const DataReplacementGroup*>& replacements) {
    std::vector<const LanceDataReplacementGroup*> handles;
    handles.reserve(replacements.size());
    for (const auto* replacement : replacements) {
        if (!replacement) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "data replacement group must not be null");
        }
        handles.push_back(replacement->c_handle());
    }
    auto* operation = lance_operation_data_replacement_new(
        handles.empty() ? nullptr : handles.data(), handles.size());
    if (!operation) detail::throw_error();
    return Operation(operation);
}

inline Operation Operation::make_data_overlay(
    const std::vector<const DataOverlayGroup*>& groups) {
    std::vector<const LanceDataOverlayGroup*> handles;
    handles.reserve(groups.size());
    for (const auto* group : groups) {
        if (!group) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "data overlay group must not be null");
        }
        handles.push_back(group->c_handle());
    }
    auto* operation = lance_operation_data_overlay_new(
        handles.empty() ? nullptr : handles.data(), handles.size());
    if (!operation) detail::throw_error();
    return Operation(operation);
}

inline Operation Operation::make_create_index(
    const std::vector<const IndexSegmentMetadata*>& new_indices,
    const std::vector<const IndexSegmentMetadata*>& removed_indices) {
    std::vector<const LanceIndexSegmentMetadata*> new_raw;
    new_raw.reserve(new_indices.size());
    for (const auto* metadata : new_indices) {
        if (!metadata) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "new index metadata entries must not be null");
        }
        new_raw.push_back(metadata->c_handle());
    }
    std::vector<const LanceIndexSegmentMetadata*> removed_raw;
    removed_raw.reserve(removed_indices.size());
    for (const auto* metadata : removed_indices) {
        if (!metadata) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "removed index metadata entries must not be null");
        }
        removed_raw.push_back(metadata->c_handle());
    }

    LanceOperation* operation = lance_operation_create_index_new(
        new_raw.data(), new_raw.size(), removed_raw.data(), removed_raw.size());
    if (!operation) detail::throw_error();
    return Operation(operation);
}

inline std::vector<IndexSegmentMetadata> Operation::create_index_new() const {
    uint64_t count = 0;
    if (lance_operation_create_index_new_count(handle_.get(), &count) != 0)
        detail::throw_error();

    std::vector<IndexSegmentMetadata> out;
    out.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        LanceIndexSegmentMetadata* metadata = nullptr;
        if (lance_operation_create_index_new_at(
                handle_.get(), static_cast<size_t>(i), &metadata) != 0)
            detail::throw_error();
        if (!metadata) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_operation_create_index_new_at returned success "
                        "with null metadata");
        }
        out.emplace_back(IndexSegmentMetadata(metadata));
    }
    return out;
}

inline std::vector<IndexSegmentMetadata> Operation::create_index_removed() const {
    uint64_t count = 0;
    if (lance_operation_create_index_removed_count(handle_.get(), &count) != 0)
        detail::throw_error();

    std::vector<IndexSegmentMetadata> out;
    out.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        LanceIndexSegmentMetadata* metadata = nullptr;
        if (lance_operation_create_index_removed_at(
                handle_.get(), static_cast<size_t>(i), &metadata) != 0)
            detail::throw_error();
        if (!metadata) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_operation_create_index_removed_at returned "
                        "success with null metadata");
        }
        out.emplace_back(IndexSegmentMetadata(metadata));
    }
    return out;

}

inline RewriteGroup::RewriteGroup(
    const std::vector<const Fragment*>& old_fragments,
    const std::vector<const Fragment*>& new_fragments) {
    std::vector<const ::LanceFragment*> old_raw;
    old_raw.reserve(old_fragments.size());
    for (const auto* fragment : old_fragments) {
        old_raw.push_back(fragment->c_handle());
    }
    std::vector<const ::LanceFragment*> new_raw;
    new_raw.reserve(new_fragments.size());
    for (const auto* fragment : new_fragments) {
        new_raw.push_back(fragment->c_handle());
    }
    auto* raw = lance_rewrite_group_new(
        old_raw.empty() ? nullptr : old_raw.data(), old_raw.size(),
        new_raw.empty() ? nullptr : new_raw.data(), new_raw.size());
    if (!raw) detail::throw_error();
    handle_ = Handle<LanceRewriteGroup, lance_rewrite_group_close>(raw);
}

inline size_t RewriteGroup::old_fragment_count() const {
    uint64_t count = 0;
    if (lance_rewrite_group_old_fragment_count(handle_.get(), &count) != 0)
        detail::throw_error();
    return static_cast<size_t>(count);
}

inline Fragment RewriteGroup::old_fragment_at(size_t index) const {
    LanceFragment* fragment = nullptr;
    if (lance_rewrite_group_old_fragment_at(
            handle_.get(), index, &fragment) != 0) {
        detail::throw_error();
    }
    return Fragment(fragment);
}

inline std::vector<Fragment> RewriteGroup::old_fragments() const {
    std::vector<Fragment> out;
    size_t count = old_fragment_count();
    out.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        out.push_back(old_fragment_at(index));
    }
    return out;
}

inline size_t RewriteGroup::new_fragment_count() const {
    uint64_t count = 0;
    if (lance_rewrite_group_new_fragment_count(handle_.get(), &count) != 0)
        detail::throw_error();
    return static_cast<size_t>(count);
}

inline Fragment RewriteGroup::new_fragment_at(size_t index) const {
    LanceFragment* fragment = nullptr;
    if (lance_rewrite_group_new_fragment_at(
            handle_.get(), index, &fragment) != 0) {
        detail::throw_error();
    }
    return Fragment(fragment);
}

inline std::vector<Fragment> RewriteGroup::new_fragments() const {
    std::vector<Fragment> out;
    size_t count = new_fragment_count();
    out.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        out.push_back(new_fragment_at(index));
    }
    return out;
}

inline RewrittenIndex::RewrittenIndex(
    const std::array<uint8_t, 16>& old_id,
    const std::array<uint8_t, 16>& new_id,
    std::string_view details_type_url,
    const std::vector<uint8_t>& details_value,
    uint32_t index_version,
    std::optional<std::vector<IndexFile>> index_files)
    : handle_(nullptr) {
    std::string owned_type_url(details_type_url);
    std::vector<::LanceIndexFile> raw_files;
    if (index_files) {
        raw_files.reserve(index_files->size());
        for (const auto& file : *index_files) {
            ::LanceIndexFile raw{};
            raw.path = file.path.c_str();
            raw.size_bytes = file.size_bytes;
            raw_files.push_back(raw);
        }
    }
    auto* raw = lance_rewritten_index_new(
        old_id.data(), new_id.data(), owned_type_url.c_str(),
        details_value.empty() ? nullptr : details_value.data(),
        details_value.size(), index_version, index_files.has_value(),
        raw_files.empty() ? nullptr : raw_files.data(), raw_files.size());
    if (!raw) detail::throw_error();
    handle_ = Handle<LanceRewrittenIndex, lance_rewritten_index_close>(raw);
}

inline Fragment Operation::updated_fragment_at(size_t index) const {
    LanceFragment* fragment = nullptr;
    if (lance_operation_update_updated_fragment_at(
            handle_.get(), index, &fragment) != 0) {
        detail::throw_error();
    }
    return Fragment(fragment);
}

inline Fragment Operation::new_fragment_at(size_t index) const {
    LanceFragment* fragment = nullptr;
    if (lance_operation_update_new_fragment_at(
            handle_.get(), index, &fragment) != 0) {
        detail::throw_error();
    }
    return Fragment(fragment);
}

inline CompactedSsTable Operation::compacted_sstable_at(size_t index) const {
    ::LanceCompactedSsTable raw{};
    if (lance_operation_update_compacted_sstable_at(
            handle_.get(), index, &raw) != 0) {
        detail::throw_error();
    }
    CompactedSsTable value;
    std::memcpy(value.shard_id.data(), raw.shard_id, value.shard_id.size());
    value.generation = raw.generation;
    return value;
}

inline CompactedSsTable Operation::update_mem_wal_state_compacted_sstable_at(
    size_t index) const {
    ::LanceCompactedSsTable raw{};
    if (lance_operation_update_mem_wal_state_compacted_sstable_at(
            handle_.get(), index, &raw) != 0) {
        detail::throw_error();
    }
    CompactedSsTable value;
    std::memcpy(value.shard_id.data(), raw.shard_id, value.shard_id.size());
    value.generation = raw.generation;
    return value;
}

inline std::optional<KeyExistenceFilter> Operation::inserted_rows_filter()
    const {
    bool present = false;
    LanceKeyExistenceFilter* filter = nullptr;
    if (lance_operation_update_inserted_rows_filter(
            handle_.get(), &present, &filter) != 0) {
        detail::throw_error();
    }
    if (!present) {
        if (filter) {
            lance_key_existence_filter_close(filter);
            throw Error(LANCE_ERR_INTERNAL,
                        "Update inserted-rows filter reported absent with a "
                        "non-null handle");
        }
        return std::nullopt;
    }
    if (!filter) {
        throw Error(LANCE_ERR_INTERNAL,
                    "Update inserted-rows filter reported present with a "
                    "null handle");
    }
    return KeyExistenceFilter(filter);
}

inline std::optional<UpdatedFragmentOffsets>
Operation::updated_fragment_offsets() const {
    bool present = false;
    LanceUpdatedFragmentOffsets* offsets = nullptr;
    if (lance_operation_update_updated_fragment_offsets(
            handle_.get(), &present, &offsets) != 0) {
        detail::throw_error();
    }
    if (!present) {
        if (offsets) {
            lance_updated_fragment_offsets_close(offsets);
            throw Error(LANCE_ERR_INTERNAL,
                        "Update fragment offsets reported absent with a "
                        "non-null handle");
        }
        return std::nullopt;
    }
    if (!offsets) {
        throw Error(LANCE_ERR_INTERNAL,
                    "Update fragment offsets reported present with a null "
                    "handle");
    }
    return UpdatedFragmentOffsets(offsets);
}

inline std::optional<std::string> Operation::update_mem_wal_dir_id() const {
    bool present = false;
    const char* dir_id = nullptr;
    if (lance_operation_update_mem_wal_dir_id(handle_.get(), &present,
                                               &dir_id) != 0) {
        detail::throw_error();
    }
    if (!present) {
        if (dir_id) {
            throw Error(LANCE_ERR_INTERNAL,
                        "Update MemWAL dir_id reported absent with a non-null "
                        "string");
        }
        return std::nullopt;
    }
    if (!dir_id) {
        throw Error(LANCE_ERR_INTERNAL,
                    "Update MemWAL dir_id reported present with a null "
                    "string");
    }
    return std::string(dir_id);
}

inline std::string Operation::update_mem_wal_state_dir_id() const {
    const char* dir_id = nullptr;
    if (lance_operation_update_mem_wal_state_dir_id(handle_.get(), &dir_id) !=
        0) {
        detail::throw_error();
    }
    if (!dir_id) {
        throw Error(LANCE_ERR_INTERNAL,
                    "UpdateMemWalState dir_id reported present with a null "
                    "string");
    }
    return std::string(dir_id);
}

inline RewriteGroup Operation::rewrite_group_at(size_t index) const {
    LanceRewriteGroup* group = nullptr;
    if (lance_operation_rewrite_group_at(handle_.get(), index, &group) != 0) {
        detail::throw_error();
    }
    return RewriteGroup(group);
}

inline RewrittenIndex Operation::rewritten_index_at(size_t index) const {
    LanceRewrittenIndex* rewritten = nullptr;
    if (lance_operation_rewrite_rewritten_index_at(
            handle_.get(), index, &rewritten) != 0) {
        detail::throw_error();
    }
    return RewrittenIndex(rewritten);
}

inline std::optional<IndexSegmentMetadata> Operation::frag_reuse_index()
    const {
    bool present = false;
    LanceIndexSegmentMetadata* metadata = nullptr;
    if (lance_operation_rewrite_frag_reuse_index(
            handle_.get(), &present, &metadata) != 0) {
        detail::throw_error();
    }
    if (!present) {
        if (metadata) {
            lance_index_segment_metadata_free(metadata);
            throw Error(LANCE_ERR_INTERNAL,
                        "Rewrite fragment-reuse index reported absent with a "
                        "non-null handle");
        }
        return std::nullopt;
    }
    if (!metadata) {
        throw Error(LANCE_ERR_INTERNAL,
                    "Rewrite fragment-reuse index reported present with a "
                    "null handle");
    }
    return IndexSegmentMetadata(metadata);
}

inline Operation Operation::make_update(const UpdateOperationSpec& spec) {
    std::vector<const ::LanceFragment*> updated;
    updated.reserve(spec.updated_fragments.size());
    for (const auto* fragment : spec.updated_fragments) {
        updated.push_back(fragment->c_handle());
    }
    std::vector<const ::LanceFragment*> inserted;
    inserted.reserve(spec.new_fragments.size());
    for (const auto* fragment : spec.new_fragments) {
        inserted.push_back(fragment->c_handle());
    }
    std::vector<::LanceCompactedSsTable> compacted;
    compacted.reserve(spec.compacted_sstables.size());
    for (const auto& sstable : spec.compacted_sstables) {
        compacted.push_back(sstable.to_c());
    }
    int32_t update_mode = 0;
    if (spec.update_mode) {
        if (*spec.update_mode == UpdateMode::RewriteRows) {
            update_mode = LANCE_UPDATE_MODE_REWRITE_ROWS;
        } else if (*spec.update_mode == UpdateMode::RewriteColumns) {
            update_mode = LANCE_UPDATE_MODE_REWRITE_COLUMNS;
        } else {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "update_mode is not a LanceUpdateMode value");
        }
    }

    ::LanceUpdateOperationOptions options{};
    options.removed_fragment_ids = spec.removed_fragment_ids.empty()
                                       ? nullptr
                                       : spec.removed_fragment_ids.data();
    options.removed_fragment_count = spec.removed_fragment_ids.size();
    options.updated_fragments = updated.empty() ? nullptr : updated.data();
    options.updated_fragment_count = updated.size();
    options.new_fragments = inserted.empty() ? nullptr : inserted.data();
    options.new_fragment_count = inserted.size();
    options.fields_modified =
        spec.fields_modified.empty() ? nullptr : spec.fields_modified.data();
    options.fields_modified_count = spec.fields_modified.size();
    options.compacted_sstables = compacted.empty() ? nullptr : compacted.data();
    options.compacted_sstable_count = compacted.size();
    options.fields_for_preserving_frag_bitmap =
        spec.fields_for_preserving_frag_bitmap.empty()
            ? nullptr
            : spec.fields_for_preserving_frag_bitmap.data();
    options.fields_for_preserving_frag_bitmap_count =
        spec.fields_for_preserving_frag_bitmap.size();
    options.has_update_mode = spec.update_mode.has_value();
    options.update_mode = update_mode;
    options.inserted_rows_filter = spec.inserted_rows_filter
                                       ? spec.inserted_rows_filter->c_handle()
                                       : nullptr;
    options.updated_fragment_offsets =
        spec.updated_fragment_offsets
            ? spec.updated_fragment_offsets->c_handle()
            : nullptr;

    auto* raw = spec.mem_wal_dir_id
                   ? lance_operation_update_new_with_mem_wal_dir_id(
                         &options, spec.mem_wal_dir_id->c_str())
                   : lance_operation_update_new(&options);
    if (!raw) detail::throw_error();
    return Operation(raw);
}

inline Operation Operation::make_rewrite(
    const std::vector<const RewriteGroup*>& groups,
    const std::vector<const RewrittenIndex*>& rewritten_indices,
    const IndexSegmentMetadata* frag_reuse_index) {
    std::vector<const ::LanceRewriteGroup*> raw_groups;
    raw_groups.reserve(groups.size());
    for (const auto* group : groups) raw_groups.push_back(group->c_handle());
    std::vector<const ::LanceRewrittenIndex*> raw_indices;
    raw_indices.reserve(rewritten_indices.size());
    for (const auto* index : rewritten_indices) {
        raw_indices.push_back(index->c_handle());
    }
    auto* raw = lance_operation_rewrite_new(
        raw_groups.empty() ? nullptr : raw_groups.data(), raw_groups.size(),
        raw_indices.empty() ? nullptr : raw_indices.data(), raw_indices.size(),
        frag_reuse_index ? frag_reuse_index->c_handle() : nullptr);
    if (!raw) detail::throw_error();
    return Operation(raw);
}

inline Operation Operation::make_update_mem_wal_state(
    const std::vector<CompactedSsTable>& compacted_sstables) {
    std::vector<::LanceCompactedSsTable> raw;
    raw.reserve(compacted_sstables.size());
    for (const auto& sstable : compacted_sstables) {
        raw.push_back(sstable.to_c());
    }
    auto* operation = lance_operation_update_mem_wal_state_new(
        raw.empty() ? nullptr : raw.data(), raw.size());
    if (!operation) detail::throw_error();
    return Operation(operation);
}

inline Operation Operation::make_update_mem_wal_state_with_dir_id(
    const std::vector<CompactedSsTable>& compacted_sstables,
    const std::string& dir_id) {
    std::vector<::LanceCompactedSsTable> raw;
    raw.reserve(compacted_sstables.size());
    for (const auto& sstable : compacted_sstables) {
        raw.push_back(sstable.to_c());
    }
    auto* operation = lance_operation_update_mem_wal_state_new_with_dir_id(
        raw.empty() ? nullptr : raw.data(), raw.size(), dir_id.c_str());
    if (!operation) detail::throw_error();
    return Operation(operation);
}

}  // namespace lance
