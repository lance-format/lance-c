/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the index domain.
#pragma once

#include "lance/detail/declarations.hpp"

namespace lance {

inline void IndexModel::reset() noexcept {
        if (array_.release) array_.release(&array_);
        if (schema_.release) schema_.release(&schema_);
    }

inline IndexModel::IndexModel(ArrowArray array, ArrowSchema schema) noexcept : array_(array), schema_(schema) {}

inline IndexModel IndexModel::adopt(ArrowArray* array, ArrowSchema* schema) {
        if (!array || !schema) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "IndexModel::adopt requires non-null ArrowArray and ArrowSchema");
        }
        if (!array->release || !schema->release) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "IndexModel::adopt requires initialized Arrow structures");
        }
        IndexModel model;
        model.array_ = *array;
        model.schema_ = *schema;
        *array = {};
        *schema = {};
        return model;
    }

inline IndexModel::~IndexModel() { reset(); }

inline IndexModel::IndexModel(IndexModel&& other) noexcept : array_(other.array_), schema_(other.schema_) {
        other.array_ = {};
        other.schema_ = {};
    }

inline ArrowArray* IndexModel::array() noexcept { return &array_; }

inline const ArrowSchema* IndexModel::schema() const noexcept { return &schema_; }

inline IvfModel::IvfModel(LanceIvfModel* model) : handle_(model) {}

inline bool IvfModel::has_centroids() const {
        bool value = lance_ivf_model_has_centroids(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline std::optional<IndexModel> IvfModel::centroids() const {
        if (!has_centroids()) return std::nullopt;
        ArrowArray array = {};
        ArrowSchema schema = {};
        if (lance_ivf_model_export_centroids(handle_.get(), &array, &schema) != 0)
            detail::throw_error();
        return IndexModel(array, schema);
    }

inline uint64_t IvfModel::num_partitions() const {
        uint64_t value = lance_ivf_model_num_partitions(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline uint64_t IvfModel::dimension() const {
        uint64_t value = lance_ivf_model_dimension(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline uint64_t IvfModel::partition_size(uint64_t partition_id) const {
        uint64_t value =
            lance_ivf_model_partition_size(handle_.get(), partition_id);
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline const LanceIvfModel* IvfModel::c_handle() const { return handle_.get(); }

namespace detail {

inline void VectorIndexOptionsStorage::set_optional(LanceOptionalU64& out,
                             const std::optional<uint64_t>& value) {
        out = {};
        if (value) {
            out.value = *value;
            out.is_set = true;
        }
    }

}

namespace detail {

inline void VectorIndexOptionsStorage::set_map(const std::map<std::string, std::string>& source,
                        std::vector<const char*>& keys,
                        std::vector<const char*>& values,
                        LanceStringMap& out) {
        for (const auto& [key, value] : source) {
            keys.push_back(key.c_str());
            values.push_back(value.c_str());
        }
        out.keys = keys.empty() ? nullptr : keys.data();
        out.values = values.empty() ? nullptr : values.data();
        out.count = keys.size();
    }

}

namespace detail {

inline VectorIndexOptionsStorage::VectorIndexOptionsStorage(const VectorIndexOptions& options) {
        options_.index_type = static_cast<int32_t>(options.index_type);
        options_.metric = static_cast<int32_t>(options.metric);
        set_optional(options_.num_partitions, options.num_partitions);
        set_optional(options_.target_partition_size, options.target_partition_size);
        set_optional(options_.max_iters, options.max_iters);
        set_optional(options_.sample_rate, options.sample_rate);
        set_optional(options_.streaming_sample_rate,
                     options.streaming_sample_rate);
        set_optional(options_.streaming_coreset_rate,
                     options.streaming_coreset_rate);
        options_.streaming_refine_passes = options.streaming_refine_passes;
        options_.precomputed_partitions_file =
            options.precomputed_partitions_file
                ? options.precomputed_partitions_file->c_str()
                : nullptr;
        if (options.precomputed_shuffle_buffers) {
            shuffle_.path = options.precomputed_shuffle_buffers->first.c_str();
            for (const auto& buffer : options.precomputed_shuffle_buffers->second) {
                shuffle_buffers_.push_back(buffer.c_str());
            }
            shuffle_.buffers =
                shuffle_buffers_.empty() ? nullptr : shuffle_buffers_.data();
            shuffle_.buffer_count = shuffle_buffers_.size();
            options_.precomputed_shuffle_buffers = &shuffle_;
        }
        set_optional(options_.shuffle_partition_batches,
                     options.shuffle_partition_batches);
        set_optional(options_.shuffle_partition_concurrency,
                     options.shuffle_partition_concurrency);
        if (options.storage_options) {
            set_map(*options.storage_options, storage_keys_, storage_values_,
                    storage_);
            options_.storage_options = &storage_;
        }
        set_optional(options_.num_sub_vectors, options.num_sub_vectors);
        set_optional(options_.num_bits, options.num_bits);
        if (options.ivf_centroids) {
            options_.ivf_centroids = options.ivf_centroids->array();
            options_.ivf_centroids_schema = options.ivf_centroids->schema();
        }
        if (options.pq_codebook) {
            options_.pq_codebook = options.pq_codebook->array();
            options_.pq_codebook_schema = options.pq_codebook->schema();
        }
        options_.rabitq_model_json = options.rabitq_model_json
                                         ? options.rabitq_model_json->c_str()
                                         : nullptr;
        set_optional(options_.hnsw_max_level, options.hnsw_max_level);
        set_optional(options_.hnsw_m, options.hnsw_m);
        set_optional(options_.hnsw_ef_construction,
                     options.hnsw_ef_construction);
        options_.index_file_version =
            static_cast<int32_t>(options.index_file_version);
        options_.skip_transpose = options.skip_transpose;
        set_map(options.runtime_hints, runtime_keys_, runtime_values_, runtime_);
        options_.runtime_hints = &runtime_;
        if (options.train) {
            options_.has_train = true;
            options_.train = *options.train;
        }
    }

}

namespace detail {

inline const LanceVectorIndexOptions* VectorIndexOptionsStorage::get() const { return &options_; }

}

inline IndexSegmentMetadata::IndexSegmentMetadata(LanceIndexSegmentMetadata* metadata) : handle_(metadata) {}

inline IndexSegmentMetadata IndexSegmentMetadata::parse(const uint8_t* bytes, size_t len) {
        LanceIndexSegmentMetadata* metadata = nullptr;
        if (lance_index_segment_metadata_parse(bytes, len, &metadata) != 0)
            detail::throw_error();
        if (!metadata) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_index_segment_metadata_parse returned success with null metadata");
        }
        return IndexSegmentMetadata(metadata);
    }

inline IndexSegmentMetadata IndexSegmentMetadata::parse(const std::vector<uint8_t>& bytes) {
        return parse(bytes.data(), bytes.size());
    }

inline std::array<uint8_t, 16> IndexSegmentMetadata::uuid() const {
        std::array<uint8_t, 16> out = {};
        if (lance_index_segment_metadata_uuid(handle_.get(), out.data()) != 0)
            detail::throw_error();
        return out;
    }

inline std::string IndexSegmentMetadata::name() const {
        const char* value = lance_index_segment_metadata_name(handle_.get());
        if (!value) detail::throw_error();
        return std::string(value);
    }

inline uint64_t IndexSegmentMetadata::dataset_version() const {
        uint64_t version =
            lance_index_segment_metadata_dataset_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return version;
    }

inline int32_t IndexSegmentMetadata::index_version() const {
        int32_t version =
            lance_index_segment_metadata_index_version(handle_.get());
        if (version < 0) detail::throw_error();
        return version;
    }

inline int32_t IndexSegmentMetadata::index_type() const {
        int32_t type = lance_index_segment_metadata_index_type(handle_.get());
        if (type < 0) detail::throw_error();
        return type;
    }

inline std::string IndexSegmentMetadata::index_details_type_url() const {
        const char* value =
            lance_index_segment_metadata_index_details_type_url(handle_.get());
        if (!value) detail::throw_error();
        return std::string(value);
    }

inline std::vector<int32_t> IndexSegmentMetadata::field_ids() const {
        size_t count = lance_index_segment_metadata_field_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();

        std::vector<int32_t> out(count);
        size_t written = 0;
        if (lance_index_segment_metadata_field_ids(
                handle_.get(), out.data(), out.size(), &written) != 0)
            detail::throw_error();
        out.resize(written);
        return out;
    }

inline std::vector<uint32_t> IndexSegmentMetadata::fragment_ids() const {
        size_t count =
            lance_index_segment_metadata_fragment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();

        std::vector<uint32_t> out(count);
        size_t written = 0;
        if (lance_index_segment_metadata_fragment_ids(
                handle_.get(), out.data(), out.size(), &written) != 0)
            detail::throw_error();
        out.resize(written);
        return out;
    }

inline IndexSegmentMetadata IndexSegmentMetadata::clone() const {
        LanceIndexSegmentMetadata* raw =
            lance_index_segment_metadata_clone(handle_.get());
        if (!raw) detail::throw_error();
        return IndexSegmentMetadata(raw);
    }

inline IndexSegmentMetadata IndexSegmentMetadata::clone_with_fragment_ids(
        std::optional<std::vector<uint32_t>> fragment_ids) const {
        LanceIndexSegmentMetadata* raw = nullptr;
        if (fragment_ids.has_value()) {
            raw = lance_index_segment_metadata_clone_with_fragment_ids(
                handle_.get(), true, fragment_ids->data(),
                fragment_ids->size());
        } else {
            raw = lance_index_segment_metadata_clone_with_fragment_ids(
                handle_.get(), false, nullptr, 0);
        }
        if (!raw) detail::throw_error();
        return IndexSegmentMetadata(raw);
    }

inline std::vector<uint8_t> IndexSegmentMetadata::encode() const {
        struct BytesGuard {
            uint8_t* bytes = nullptr;
            ~BytesGuard() noexcept { lance_free_bytes(bytes); }

            BytesGuard() = default;
            BytesGuard(const BytesGuard&) = delete;
            BytesGuard& operator=(const BytesGuard&) = delete;
        } guard;

        size_t len = 0;
        if (lance_index_segment_metadata_encode(handle_.get(), &guard.bytes,
                                                &len) != 0)
            detail::throw_error();
        if (!guard.bytes || len == 0) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_index_segment_metadata_encode returned empty metadata");
        }
        return std::vector<uint8_t>(guard.bytes, guard.bytes + len);
    }

inline std::vector<int32_t> IndexSegmentMetadata::covering_field_ids() const {
        size_t count =
            lance_index_segment_metadata_covering_field_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();

        std::vector<int32_t> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            int32_t field_id = 0;
            if (lance_index_segment_metadata_covering_field_id_at(
                    handle_.get(), i, &field_id) != 0)
                detail::throw_error();
            out.push_back(field_id);
        }
        return out;
    }

inline std::vector<uint8_t> IndexSegmentMetadata::index_details_value() const {
        size_t len = 0;
        const uint8_t* bytes =
            lance_index_segment_metadata_index_details_value(handle_.get(),
                                                              &len);
        if (!bytes) detail::throw_error();
        if (len == 0) return {};
        return std::vector<uint8_t>(bytes, bytes + len);
    }

inline std::vector<IndexSegmentFile> IndexSegmentMetadata::files() const {
        size_t count = lance_index_segment_metadata_file_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();

        std::vector<IndexSegmentFile> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const char* path =
                lance_index_segment_metadata_file_path_at(handle_.get(), i);
            if (!path) detail::throw_error();
            uint64_t size = 0;
            if (lance_index_segment_metadata_file_size_at(
                    handle_.get(), i, &size) != 0)
                detail::throw_error();
            out.push_back(IndexSegmentFile{path, size});
        }
        return out;
    }

inline std::optional<IndexTimestamp> IndexSegmentMetadata::created_at() const {
        LanceOptionalTimestamp timestamp{};
        if (lance_index_segment_metadata_created_at(handle_.get(),
                                                    &timestamp) != 0)
            detail::throw_error();
        if (!timestamp.is_set) return std::nullopt;
        return IndexTimestamp{timestamp.unix_seconds, timestamp.nanos};
    }

inline std::optional<uint32_t> IndexSegmentMetadata::base_id() const {
        bool is_set = false;
        uint32_t base_id = 0;
        if (lance_index_segment_metadata_base_id(handle_.get(), &is_set,
                                                 &base_id) != 0)
            detail::throw_error();
        if (!is_set) return std::nullopt;
        return base_id;
    }

inline const LanceIndexSegmentMetadata* IndexSegmentMetadata::c_handle() const { return handle_.get(); }

inline IndexSegmentBuilder::IndexSegmentBuilder(LanceIndexSegmentBuilder* builder) : handle_(builder) {}

inline std::vector<uint8_t> IndexSegmentBuilder::execute_uncommitted() {
        struct BytesGuard {
            uint8_t* bytes = nullptr;
            ~BytesGuard() noexcept { lance_free_bytes(bytes); }

            BytesGuard() = default;
            BytesGuard(const BytesGuard&) = delete;
            BytesGuard& operator=(const BytesGuard&) = delete;
        } guard;

        size_t len = 0;
        if (lance_index_segment_builder_execute_uncommitted(
                handle_.get(), &guard.bytes, &len) != 0)
            detail::throw_error();
        if (!guard.bytes || len == 0) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_index_segment_builder_execute_uncommitted returned empty metadata");
        }
        return std::vector<uint8_t>(guard.bytes, guard.bytes + len);
    }

inline IndexSegmentBuilder& IndexSegmentBuilder::progress_callback(LanceIndexBuildProgressCallback callback,
                                           void* callback_ctx) {
        if (lance_index_segment_builder_set_progress_callback(handle_.get(), callback,
                                                              callback_ctx) != 0)
            detail::throw_error();
        return *this;
    }

inline LanceIndexSegmentBuilder* IndexSegmentBuilder::c_handle() { return handle_.get(); }

}  // namespace lance
