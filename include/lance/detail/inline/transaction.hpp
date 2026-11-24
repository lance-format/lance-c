/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the transaction domain.
#pragma once

#include "lance/detail/declarations.hpp"

namespace lance {

inline DataFile::DataFile(LanceDataFile* file) : handle_(file) {}

inline std::string DataFile::path() const {
        const char* path = lance_data_file_path(handle_.get());
        if (!path) detail::throw_error();
        return path;
    }

inline std::vector<int32_t> DataFile::fields() const {
        size_t count = lance_data_file_field_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        std::vector<int32_t> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            int32_t field_id = 0;
            if (lance_data_file_field_id_at(
                    handle_.get(), i, &field_id) != 0) {
                detail::throw_error();
            }
            out.push_back(field_id);
        }
        return out;
    }

inline std::vector<int32_t> DataFile::column_indices() const {
        size_t count = lance_data_file_column_index_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        std::vector<int32_t> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            int32_t column_index = 0;
            if (lance_data_file_column_index_at(
                    handle_.get(), i, &column_index) != 0) {
                detail::throw_error();
            }
            out.push_back(column_index);
        }
        return out;
    }

inline std::pair<uint32_t, uint32_t> DataFile::file_version() const {
        uint32_t major = 0;
        uint32_t minor = 0;
        if (lance_data_file_file_version(
                handle_.get(), &major, &minor) != 0) {
            detail::throw_error();
        }
        return {major, minor};
    }

inline std::optional<uint64_t> DataFile::file_size() const {
        bool present = false;
        uint64_t size = 0;
        if (lance_data_file_file_size(
                handle_.get(), &present, &size) != 0) {
            detail::throw_error();
        }
        if (!present) return std::nullopt;
        return size;
    }

inline std::optional<uint32_t> DataFile::base_id() const {
        bool present = false;
        uint32_t base_id = 0;
        if (lance_data_file_base_id(
                handle_.get(), &present, &base_id) != 0) {
            detail::throw_error();
        }
        if (!present) return std::nullopt;
        return base_id;
    }

inline DataFile DataFile::clone() const {
        auto* raw = lance_data_file_clone(handle_.get());
        if (!raw) detail::throw_error();
        return DataFile(raw);
    }

inline const LanceDataFile* DataFile::c_handle() const { return handle_.get(); }

inline DataReplacementGroup::DataReplacementGroup(LanceDataReplacementGroup* group) : handle_(group) {}

inline DataReplacementGroup::DataReplacementGroup(uint64_t fragment_id, const DataFile& file) : handle_(nullptr) {
        auto* raw =
            lance_data_replacement_group_new(fragment_id, file.c_handle());
        if (!raw) detail::throw_error();
        handle_ =
            Handle<LanceDataReplacementGroup, lance_data_replacement_group_close>(
                raw);
    }

inline uint64_t DataReplacementGroup::fragment_id() const {
        uint64_t id = lance_data_replacement_group_fragment_id(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return id;
    }

inline DataFile DataReplacementGroup::file() const {
        LanceDataFile* file = nullptr;
        if (lance_data_replacement_group_file(handle_.get(), &file) != 0) {
            detail::throw_error();
        }
        return DataFile(file);
    }

inline DataReplacementGroup DataReplacementGroup::clone() const {
        auto* raw = lance_data_replacement_group_clone(handle_.get());
        if (!raw) detail::throw_error();
        return DataReplacementGroup(raw);
    }

inline const LanceDataReplacementGroup* DataReplacementGroup::c_handle() const {
        return handle_.get();
    }

inline UpdateMap::UpdateMap(LanceUpdateMap* map) : handle_(map) {}

inline UpdateMap::UpdateMap(const std::vector<std::string>& keys,
              const std::vector<std::optional<std::string>>& values,
              bool replace) : handle_(nullptr) {
        if (keys.size() != values.size()) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "UpdateMap keys and values must have the same length");
        }
        std::vector<const char*> key_ptrs;
        std::vector<const char*> value_ptrs;
        key_ptrs.reserve(keys.size());
        value_ptrs.reserve(values.size());
        for (size_t i = 0; i < keys.size(); ++i) {
            key_ptrs.push_back(keys[i].c_str());
            value_ptrs.push_back(values[i] ? values[i]->c_str() : nullptr);
        }
        auto* raw = lance_update_map_new(
            key_ptrs.empty() ? nullptr : key_ptrs.data(),
            value_ptrs.empty() ? nullptr : value_ptrs.data(), keys.size(),
            replace);
        if (!raw) detail::throw_error();
        handle_ = Handle<LanceUpdateMap, lance_update_map_close>(raw);
    }

inline size_t UpdateMap::count() const {
        size_t count = lance_update_map_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return count;
    }

inline std::string UpdateMap::key_at(size_t index) const {
        const char* key = lance_update_map_key_at(handle_.get(), index);
        if (!key) detail::throw_error();
        return std::string(key);
    }

inline std::optional<std::string> UpdateMap::value_at(size_t index) const {
        bool present = false;
        const char* value = nullptr;
        if (lance_update_map_value_at(handle_.get(), index, &present, &value) !=
            0) {
            detail::throw_error();
        }
        if (!present) return std::nullopt;
        return std::string(value);
    }

inline bool UpdateMap::replace() const {
        bool replace = lance_update_map_replace(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return replace;
    }

inline UpdateMap UpdateMap::clone() const {
        auto* raw = lance_update_map_clone(handle_.get());
        if (!raw) detail::throw_error();
        return UpdateMap(raw);
    }

inline const LanceUpdateMap* UpdateMap::c_handle() const { return handle_.get(); }

inline DataOverlayFile::DataOverlayFile(LanceDataOverlayFile* file) : handle_(file) {}

inline DataOverlayFile::DataOverlayFile(const DataFile& data_file,
                    OverlayCoverageKind coverage_kind,
                    const std::vector<std::vector<uint8_t>>& bitmaps,
                    uint64_t committed_version) : handle_(nullptr) {
        std::vector<const uint8_t*> pointers;
        std::vector<size_t> lengths;
        pointers.reserve(bitmaps.size());
        lengths.reserve(bitmaps.size());
        for (const auto& bitmap : bitmaps) {
            pointers.push_back(bitmap.empty() ? nullptr : bitmap.data());
            lengths.push_back(bitmap.size());
        }
        auto* raw = lance_data_overlay_file_new(
            data_file.c_handle(), static_cast<int32_t>(coverage_kind),
            pointers.empty() ? nullptr : pointers.data(),
            lengths.empty() ? nullptr : lengths.data(), bitmaps.size(),
            committed_version);
        if (!raw) detail::throw_error();
        handle_ = Handle<LanceDataOverlayFile, lance_data_overlay_file_close>(
            raw);
    }

inline DataFile DataOverlayFile::data_file() const {
        LanceDataFile* file = lance_data_overlay_file_data_file(handle_.get());
        if (!file) detail::throw_error();
        return DataFile(file);
    }

inline OverlayCoverageKind DataOverlayFile::coverage_kind() const {
        int32_t kind = 0;
        if (lance_data_overlay_file_coverage_kind(
                handle_.get(), &kind) != 0) {
            detail::throw_error();
        }
        return static_cast<OverlayCoverageKind>(kind);
    }

inline uint64_t DataOverlayFile::bitmap_count() const {
        uint64_t count = 0;
        if (lance_data_overlay_file_bitmap_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline std::vector<uint8_t> DataOverlayFile::bitmap_at(size_t index) const {
        uint8_t* bytes = nullptr;
        size_t len = 0;
        if (lance_data_overlay_file_bitmap_at(
                handle_.get(), index, &bytes, &len) != 0) {
            detail::throw_error();
        }
        return detail::take_bytes(bytes, len);
    }

inline uint64_t DataOverlayFile::committed_version() const {
        uint64_t version = 0;
        if (lance_data_overlay_file_committed_version(
                handle_.get(), &version) != 0) {
            detail::throw_error();
        }
        return version;
    }

inline DataOverlayFile DataOverlayFile::clone() const {
        auto* raw = lance_data_overlay_file_clone(handle_.get());
        if (!raw) detail::throw_error();
        return DataOverlayFile(raw);
    }

inline const LanceDataOverlayFile* DataOverlayFile::c_handle() const { return handle_.get(); }

inline DataOverlayGroup::DataOverlayGroup(LanceDataOverlayGroup* group) : handle_(group) {}

inline DataOverlayGroup::DataOverlayGroup(uint64_t fragment_id,
                     const std::vector<const DataOverlayFile*>& overlays) : handle_(nullptr) {
        std::vector<const LanceDataOverlayFile*> handles;
        handles.reserve(overlays.size());
        for (const auto* overlay : overlays) {
            if (!overlay) {
                throw Error(LANCE_ERR_INVALID_ARGUMENT,
                            "data overlay file must not be null");
            }
            handles.push_back(overlay->c_handle());
        }
        auto* raw = lance_data_overlay_group_new(
            fragment_id, handles.empty() ? nullptr : handles.data(),
            handles.size());
        if (!raw) detail::throw_error();
        handle_ = Handle<LanceDataOverlayGroup, lance_data_overlay_group_close>(
            raw);
    }

inline uint64_t DataOverlayGroup::fragment_id() const {
        uint64_t id = 0;
        if (lance_data_overlay_group_fragment_id(handle_.get(), &id) != 0) {
            detail::throw_error();
        }
        return id;
    }

inline uint64_t DataOverlayGroup::overlay_count() const {
        uint64_t count = 0;
        if (lance_data_overlay_group_overlay_count(
                handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline DataOverlayFile DataOverlayGroup::overlay_at(size_t index) const {
        LanceDataOverlayFile* overlay = nullptr;
        if (lance_data_overlay_group_overlay_at(
                handle_.get(), index, &overlay) != 0) {
            detail::throw_error();
        }
        return DataOverlayFile(overlay);
    }

inline DataOverlayGroup DataOverlayGroup::clone() const {
        auto* raw = lance_data_overlay_group_clone(handle_.get());
        if (!raw) detail::throw_error();
        return DataOverlayGroup(raw);
    }

inline const LanceDataOverlayGroup* DataOverlayGroup::c_handle() const { return handle_.get(); }

}  // namespace lance
