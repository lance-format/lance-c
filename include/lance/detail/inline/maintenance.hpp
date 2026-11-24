/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the maintenance domain.
#pragma once

#include "lance/detail/declarations.hpp"

namespace lance {

inline CompactionOptions::CompactionOptions() : handle_(lance_compaction_options_v2_new()) {
        if (!handle_) detail::throw_error();
    }

inline CompactionOptions::CompactionOptions(LanceCompactionOptionsV2* options) : handle_(options) {}

inline CompactionOptions CompactionOptions::clone() const {
        auto* raw = lance_compaction_options_v2_clone(handle_.get());
        if (!raw) detail::throw_error();
        return CompactionOptions(raw);
    }

inline CompactionOptions& CompactionOptions::set_target_rows_per_fragment(uint64_t value) {
        if (lance_compaction_options_v2_set_target_rows_per_fragment(
                handle_.get(), value) != 0) detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_max_rows_per_group(uint64_t value) {
        if (lance_compaction_options_v2_set_max_rows_per_group(
                handle_.get(), value) != 0) detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_max_bytes_per_file(std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_max_bytes_per_file(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_materialize_deletions(bool value) {
        if (lance_compaction_options_v2_set_materialize_deletions(
                handle_.get(), value) != 0) detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_materialize_deletions_threshold(float value) {
        if (lance_compaction_options_v2_set_materialize_deletions_threshold(
                handle_.get(), value) != 0) detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_num_threads(std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_num_threads(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_batch_size(std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_batch_size(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_io_buffer_size(std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_io_buffer_size(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_defer_index_remap(bool value) {
        if (lance_compaction_options_v2_set_defer_index_remap(
                handle_.get(), value) != 0) detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_index_remap_mode(LanceIndexRemapMode value) {
        if (lance_compaction_options_v2_set_index_remap_mode(
                handle_.get(), static_cast<int32_t>(value)) != 0)
            detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_compaction_mode(
        std::optional<LanceCompactionMode> value) {
        int32_t raw = value ? static_cast<int32_t>(*value) : 0;
        if (lance_compaction_options_v2_set_compaction_mode(
                handle_.get(), value.has_value(), raw) != 0)
            detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_binary_copy_read_batch_bytes(
        std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_binary_copy_read_batch_bytes(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_max_source_fragments(
        std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_max_source_fragments(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_max_source_rows(std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_max_source_rows(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_max_source_bytes(std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_max_source_bytes(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_excluded_fragment_ids(
        const std::vector<uint32_t>& fragment_ids) {
        if (lance_compaction_options_v2_set_excluded_fragment_ids(
                handle_.get(),
                fragment_ids.empty() ? nullptr : fragment_ids.data(),
                fragment_ids.size()) != 0)
            detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_max_overlays_per_fragment(
        std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_max_overlays_per_fragment(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_data_storage_version(
        std::optional<std::string> selector) {
        if (lance_compaction_options_v2_set_data_storage_version(
                handle_.get(), selector ? selector->c_str() : nullptr) != 0)
            detail::throw_error();
        return *this;
    }

inline CompactionOptions& CompactionOptions::set_transaction_properties(
        std::optional<std::vector<std::pair<std::string, std::string>>> value) {
        std::vector<const char*> keys;
        std::vector<const char*> values;
        LanceTransactionProperties properties{};
        const LanceTransactionProperties* properties_ptr = nullptr;
        if (value) {
            keys.reserve(value->size());
            values.reserve(value->size());
            for (const auto& [key, property_value] : *value) {
                keys.push_back(key.c_str());
                values.push_back(property_value.c_str());
            }
            properties.keys = keys.empty() ? nullptr : keys.data();
            properties.values = values.empty() ? nullptr : values.data();
            properties.count = value->size();
            properties_ptr = &properties;
        }
        if (lance_compaction_options_v2_set_transaction_properties(
                handle_.get(), properties_ptr) != 0)
            detail::throw_error();
        return *this;
    }

inline const LanceCompactionOptionsV2* CompactionOptions::c_handle() const { return handle_.get(); }

inline RewriteResult::RewriteResult(LanceRewriteResult* result) : handle_(result) {}

inline uint64_t RewriteResult::read_version() const {
        uint64_t value = lance_rewrite_result_read_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline LanceCompactionMetrics RewriteResult::metrics() const {
        LanceCompactionMetrics value{};
        if (lance_rewrite_result_metrics(handle_.get(), &value) != 0)
            detail::throw_error();
        return value;
    }

inline size_t RewriteResult::original_fragment_count() const {
        size_t count =
            lance_rewrite_result_original_fragment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return count;
    }

inline size_t RewriteResult::new_fragment_count() const {
        size_t count = lance_rewrite_result_new_fragment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return count;
    }

inline std::string RewriteResult::original_fragment_metadata_json(size_t index) const {
        const char* json =
            lance_rewrite_result_original_fragment_metadata_json_at(
                handle_.get(), index);
        return detail::take_string(json);
    }

inline std::string RewriteResult::new_fragment_metadata_json(size_t index) const {
        const char* json =
            lance_rewrite_result_new_fragment_metadata_json_at(
                handle_.get(), index);
        return detail::take_string(json);
    }

inline std::string RewriteResult::json() const {
        const char* raw = lance_rewrite_result_json(handle_.get());
        return detail::take_string(raw);
    }

inline RewriteResult RewriteResult::from_json(std::string_view json) {
        std::string owned(json);
        auto* result = lance_rewrite_result_from_json(owned.c_str());
        if (!result) detail::throw_error();
        return RewriteResult(result);
    }

inline const LanceRewriteResult* RewriteResult::c_handle() const { return handle_.get(); }

inline CompactionTask::CompactionTask(LanceCompactionTask* task) : handle_(task) {}

inline uint64_t CompactionTask::read_version() const {
        uint64_t value = lance_compaction_task_read_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline size_t CompactionTask::fragment_count() const {
        size_t count = lance_compaction_task_fragment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return count;
    }

inline std::string CompactionTask::fragment_metadata_json(size_t index) const {
        const char* raw =
            lance_compaction_task_fragment_metadata_json_at(
                handle_.get(), index);
        return detail::take_string(raw);
    }

inline std::vector<std::string> CompactionTask::fragment_metadata_json() const {
        std::vector<std::string> out;
        out.reserve(fragment_count());
        for (size_t index = 0; index < fragment_count(); ++index)
            out.push_back(fragment_metadata_json(index));
        return out;
    }

inline CompactionOptions CompactionTask::options() const {
        auto* options =
            lance_compaction_task_options_clone(handle_.get());
        if (!options) detail::throw_error();
        return CompactionOptions(options);
    }

inline std::string CompactionTask::json() const {
        const char* raw = lance_compaction_task_json(handle_.get());
        return detail::take_string(raw);
    }

inline CompactionTask CompactionTask::from_json(std::string_view json) {
        std::string owned(json);
        auto* task = lance_compaction_task_from_json(owned.c_str());
        if (!task) detail::throw_error();
        return CompactionTask(task);
    }

inline const LanceCompactionTask* CompactionTask::c_handle() const { return handle_.get(); }

inline CompactionPlan::CompactionPlan(LanceCompactionPlan* plan) : handle_(plan) {}

inline uint64_t CompactionPlan::read_version() const {
        uint64_t value = lance_compaction_plan_read_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline size_t CompactionPlan::num_tasks() const {
        size_t count = lance_compaction_plan_task_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return count;
    }

inline CompactionTask CompactionPlan::task(size_t index) const {
        auto* task = lance_compaction_plan_task_at(handle_.get(), index);
        if (!task) detail::throw_error();
        return CompactionTask(task);
    }

inline CompactionOptions CompactionPlan::options() const {
        auto* options =
            lance_compaction_plan_options_clone(handle_.get());
        if (!options) detail::throw_error();
        return CompactionOptions(options);
    }

inline std::string CompactionPlan::json() const {
        const char* raw = lance_compaction_plan_json(handle_.get());
        return detail::take_string(raw);
    }

inline CompactionPlan CompactionPlan::from_json(std::string_view json) {
        std::string owned(json);
        auto* plan = lance_compaction_plan_from_json(owned.c_str());
        if (!plan) detail::throw_error();
        return CompactionPlan(plan);
    }

inline const LanceCompactionPlan* CompactionPlan::c_handle() const { return handle_.get(); }

inline OptimizeIndicesOptions::OptimizeIndicesOptions() : handle_(lance_optimize_indices_options_new()) {
        if (!handle_) detail::throw_error();
    }

inline OptimizeIndicesOptions::OptimizeIndicesOptions(LanceOptimizeIndicesOptions* options) : handle_(options) {}

inline OptimizeIndicesOptions OptimizeIndicesOptions::clone() const {
        auto* raw = lance_optimize_indices_options_clone(handle_.get());
        if (!raw) detail::throw_error();
        return OptimizeIndicesOptions(raw);
    }

inline OptimizeIndicesOptions& OptimizeIndicesOptions::set_num_indices_to_merge(
        std::optional<uint64_t> value) {
        if (lance_optimize_indices_options_set_num_indices_to_merge(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            detail::throw_error();
        return *this;
    }

inline OptimizeIndicesOptions& OptimizeIndicesOptions::set_enable_partition_rebalancing(bool value) {
        if (lance_optimize_indices_options_set_enable_partition_rebalancing(
                handle_.get(), value) != 0)
            detail::throw_error();
        return *this;
    }

inline OptimizeIndicesOptions& OptimizeIndicesOptions::set_index_names(
        std::optional<std::vector<std::string>> names) {
        std::vector<const char*> raw;
        if (names) {
            raw.reserve(names->size());
            for (const auto& name : *names) raw.push_back(name.c_str());
        }
        if (lance_optimize_indices_options_set_index_names(
                handle_.get(),
                names.has_value(),
                raw.empty() ? nullptr : raw.data(),
                raw.size()) != 0)
            detail::throw_error();
        return *this;
    }

inline OptimizeIndicesOptions& OptimizeIndicesOptions::set_retrain(bool value) {
        if (lance_optimize_indices_options_set_retrain(
                handle_.get(), value) != 0)
            detail::throw_error();
        return *this;
    }

inline OptimizeIndicesOptions& OptimizeIndicesOptions::set_transaction_properties(
        std::optional<std::vector<std::pair<std::string, std::string>>> value) {
        std::vector<const char*> keys;
        std::vector<const char*> values;
        LanceTransactionProperties properties{};
        const LanceTransactionProperties* properties_ptr = nullptr;
        if (value) {
            keys.reserve(value->size());
            values.reserve(value->size());
            for (const auto& [key, property_value] : *value) {
                keys.push_back(key.c_str());
                values.push_back(property_value.c_str());
            }
            properties.keys = keys.empty() ? nullptr : keys.data();
            properties.values = values.empty() ? nullptr : values.data();
            properties.count = value->size();
            properties_ptr = &properties;
        }
        if (lance_optimize_indices_options_set_transaction_properties(
                handle_.get(), properties_ptr) != 0)
            detail::throw_error();
        return *this;
    }

inline OptimizeIndicesOptions& OptimizeIndicesOptions::set_max_rows_per_task(
        std::optional<uint64_t> value) {
        if (lance_optimize_indices_options_set_max_rows_per_task(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            detail::throw_error();
        return *this;
    }

inline OptimizeIndicesOptions& OptimizeIndicesOptions::set_max_rows_per_segment(
        std::optional<uint64_t> value) {
        if (lance_optimize_indices_options_set_max_rows_per_segment(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            detail::throw_error();
        return *this;
    }

inline const LanceOptimizeIndicesOptions* OptimizeIndicesOptions::c_handle() const {
        return handle_.get();
    }

inline IndexSegmentResult::IndexSegmentResult(LanceIndexSegmentResult* result) : handle_(result) {}

inline std::array<uint8_t, 16> IndexSegmentResult::plan_id() const {
        std::array<uint8_t, 16> out = {};
        if (lance_index_segment_result_plan_id(
                handle_.get(), out.data()) != 0)
            detail::throw_error();
        return out;
    }

inline uint64_t IndexSegmentResult::read_version() const {
        uint64_t value = lance_index_segment_result_read_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline uint32_t IndexSegmentResult::group() const {
        uint32_t value = lance_index_segment_result_group(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline size_t IndexSegmentResult::replace_count() const {
        size_t count = lance_index_segment_result_replace_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return count;
    }

inline std::array<uint8_t, 16> IndexSegmentResult::replace_uuid(size_t index) const {
        std::array<uint8_t, 16> out = {};
        if (lance_index_segment_result_replace_uuid_at(
                handle_.get(), index, out.data()) != 0)
            detail::throw_error();
        return out;
    }

inline std::string IndexSegmentResult::json() const {
        const char* raw = lance_index_segment_result_json(handle_.get());
        return detail::take_string(raw);
    }

inline IndexSegmentResult IndexSegmentResult::from_json(std::string_view json) {
        std::string owned(json);
        auto* result = lance_index_segment_result_from_json(owned.c_str());
        if (!result) detail::throw_error();
        return IndexSegmentResult(result);
    }

inline const LanceIndexSegmentResult* IndexSegmentResult::c_handle() const { return handle_.get(); }

inline OptimizeIndexTask::OptimizeIndexTask(LanceOptimizeIndexTask* task) : handle_(task) {}

inline std::array<uint8_t, 16> OptimizeIndexTask::plan_id() const {
        std::array<uint8_t, 16> out = {};
        if (lance_optimize_index_task_plan_id(
                handle_.get(), out.data()) != 0)
            detail::throw_error();
        return out;
    }

inline uint64_t OptimizeIndexTask::read_version() const {
        uint64_t value = lance_optimize_index_task_read_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline uint32_t OptimizeIndexTask::group() const {
        uint32_t value = lance_optimize_index_task_group(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline size_t OptimizeIndexTask::fragment_count() const {
        size_t count = lance_optimize_index_task_fragment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return count;
    }

inline uint32_t OptimizeIndexTask::fragment_id(size_t index) const {
        uint32_t value = 0;
        if (lance_optimize_index_task_fragment_id_at(
                handle_.get(), index, &value) != 0)
            detail::throw_error();
        return value;
    }

inline std::vector<uint32_t> OptimizeIndexTask::fragment_ids() const {
        std::vector<uint32_t> out;
        out.reserve(fragment_count());
        for (size_t index = 0; index < fragment_count(); ++index)
            out.push_back(fragment_id(index));
        return out;
    }

inline size_t OptimizeIndexTask::old_segment_count() const {
        size_t count =
            lance_optimize_index_task_old_segment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return count;
    }

inline std::optional<uint64_t> OptimizeIndexTask::num_indices_to_merge() const {
        bool has_value = false;
        uint64_t value = 0;
        if (lance_optimize_index_task_num_indices_to_merge(
                handle_.get(), &has_value, &value) != 0)
            detail::throw_error();
        return has_value ? std::optional<uint64_t>(value) : std::nullopt;
    }

inline bool OptimizeIndexTask::enable_partition_rebalancing() const {
        bool value = false;
        if (lance_optimize_index_task_enable_partition_rebalancing(
                handle_.get(), &value) != 0)
            detail::throw_error();
        return value;
    }

inline bool OptimizeIndexTask::retrain() const {
        bool value = false;
        if (lance_optimize_index_task_retrain(handle_.get(), &value) != 0)
            detail::throw_error();
        return value;
    }

inline std::string OptimizeIndexTask::json() const {
        const char* raw = lance_optimize_index_task_json(handle_.get());
        return detail::take_string(raw);
    }

inline OptimizeIndexTask OptimizeIndexTask::from_json(std::string_view json) {
        std::string owned(json);
        auto* task = lance_optimize_index_task_from_json(owned.c_str());
        if (!task) detail::throw_error();
        return OptimizeIndexTask(task);
    }

inline const LanceOptimizeIndexTask* OptimizeIndexTask::c_handle() const { return handle_.get(); }

inline OptimizeIndexMerge::OptimizeIndexMerge(LanceOptimizeIndexMerge* merge) : handle_(merge) {}

inline std::array<uint8_t, 16> OptimizeIndexMerge::plan_id() const {
        std::array<uint8_t, 16> out = {};
        if (lance_optimize_index_merge_plan_id(
                handle_.get(), out.data()) != 0)
            detail::throw_error();
        return out;
    }

inline uint64_t OptimizeIndexMerge::read_version() const {
        uint64_t value = lance_optimize_index_merge_read_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline uint32_t OptimizeIndexMerge::group() const {
        uint32_t value = lance_optimize_index_merge_group(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline size_t OptimizeIndexMerge::old_segment_count() const {
        size_t count =
            lance_optimize_index_merge_old_segment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return count;
    }

inline size_t OptimizeIndexMerge::replace_count() const {
        size_t count = lance_optimize_index_merge_replace_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return count;
    }

inline std::array<uint8_t, 16> OptimizeIndexMerge::replace_uuid(size_t index) const {
        std::array<uint8_t, 16> out = {};
        if (lance_optimize_index_merge_replace_uuid_at(
                handle_.get(), index, out.data()) != 0)
            detail::throw_error();
        return out;
    }

inline std::string OptimizeIndexMerge::json() const {
        const char* raw = lance_optimize_index_merge_json(handle_.get());
        return detail::take_string(raw);
    }

inline OptimizeIndexMerge OptimizeIndexMerge::from_json(std::string_view json) {
        std::string owned(json);
        auto* merge = lance_optimize_index_merge_from_json(owned.c_str());
        if (!merge) detail::throw_error();
        return OptimizeIndexMerge(merge);
    }

inline const LanceOptimizeIndexMerge* OptimizeIndexMerge::c_handle() const { return handle_.get(); }

inline OptimizeIndicesPlan::OptimizeIndicesPlan(LanceOptimizeIndicesPlan* plan) : handle_(plan) {}

inline std::array<uint8_t, 16> OptimizeIndicesPlan::plan_id() const {
        std::array<uint8_t, 16> out = {};
        if (lance_optimize_indices_plan_id(
                handle_.get(), out.data()) != 0)
            detail::throw_error();
        return out;
    }

inline uint64_t OptimizeIndicesPlan::read_version() const {
        uint64_t value =
            lance_optimize_indices_plan_read_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline size_t OptimizeIndicesPlan::num_tasks() const {
        size_t count = lance_optimize_indices_plan_task_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return count;
    }

inline size_t OptimizeIndicesPlan::num_merges() const {
        size_t count = lance_optimize_indices_plan_merge_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return count;
    }

inline OptimizeIndexTask OptimizeIndicesPlan::task(size_t index) const {
        auto* task = lance_optimize_indices_plan_task_at(handle_.get(), index);
        if (!task) detail::throw_error();
        return OptimizeIndexTask(task);
    }

inline OptimizeIndexMerge OptimizeIndicesPlan::merge(size_t index) const {
        auto* merge =
            lance_optimize_indices_plan_merge_at(handle_.get(), index);
        if (!merge) detail::throw_error();
        return OptimizeIndexMerge(merge);
    }

inline std::string OptimizeIndicesPlan::json() const {
        const char* raw = lance_optimize_indices_plan_json(handle_.get());
        return detail::take_string(raw);
    }

inline OptimizeIndicesPlan OptimizeIndicesPlan::from_json(std::string_view json) {
        std::string owned(json);
        auto* plan = lance_optimize_indices_plan_from_json(owned.c_str());
        if (!plan) detail::throw_error();
        return OptimizeIndicesPlan(plan);
    }

inline const LanceOptimizeIndicesPlan* OptimizeIndicesPlan::c_handle() const { return handle_.get(); }

inline CompactionOptions CompactionOptions::from_dataset(
    const Dataset& dataset) {
    auto* options =
        lance_compaction_options_v2_from_dataset(dataset.c_handle());
    if (!options) detail::throw_error();
    return CompactionOptions(options);
}

inline RewriteResult CompactionTask::execute(const Dataset& dataset) const {
    auto* result =
        lance_compaction_task_execute(dataset.c_handle(), handle_.get());
    if (!result) detail::throw_error();
    return RewriteResult(result);
}

inline std::optional<IndexSegmentResult> OptimizeIndexTask::execute(
    const Dataset& dataset) const {
    LanceIndexSegmentResult* result = nullptr;
    if (lance_optimize_index_task_execute(
            dataset.c_handle(), handle_.get(), &result) != 0)
        detail::throw_error();
    if (!result) return std::nullopt;
    return IndexSegmentResult(result);
}

inline std::optional<IndexSegmentResult> OptimizeIndexMerge::execute(
    const Dataset& dataset,
    const std::vector<IndexSegmentResult>& task_results) const {
    std::vector<const LanceIndexSegmentResult*> raw;
    raw.reserve(task_results.size());
    for (const auto& result : task_results) raw.push_back(result.c_handle());
    LanceIndexSegmentResult* result = nullptr;
    if (lance_optimize_index_merge_execute(
            dataset.c_handle(),
            handle_.get(),
            raw.empty() ? nullptr : raw.data(),
            raw.size(),
            &result) != 0)
        detail::throw_error();
    if (!result) return std::nullopt;
    return IndexSegmentResult(result);
}

inline IndexSegmentMetadata IndexSegmentResult::segment() const {
    auto* metadata = lance_index_segment_result_segment(handle_.get());
    if (!metadata) detail::throw_error();
    return IndexSegmentMetadata(metadata);
}

inline IndexSegmentMetadata OptimizeIndexTask::reference() const {
    auto* metadata = lance_optimize_index_task_reference(handle_.get());
    if (!metadata) detail::throw_error();
    return IndexSegmentMetadata(metadata);
}

inline IndexSegmentMetadata OptimizeIndexTask::old_segment(
    size_t index) const {
    auto* metadata =
        lance_optimize_index_task_old_segment_at(handle_.get(), index);
    if (!metadata) detail::throw_error();
    return IndexSegmentMetadata(metadata);
}

inline IndexSegmentMetadata OptimizeIndexMerge::old_segment(
    size_t index) const {
    auto* metadata =
        lance_optimize_index_merge_old_segment_at(handle_.get(), index);
    if (!metadata) detail::throw_error();
    return IndexSegmentMetadata(metadata);
}

inline Fragment RewriteResult::original_fragment_at(size_t index) const {
    auto* raw =
        lance_rewrite_result_original_fragment_at(handle_.get(), index);
    if (!raw) detail::throw_error();
    return Fragment(raw);
}

inline Fragment RewriteResult::new_fragment_at(size_t index) const {
    auto* raw = lance_rewrite_result_new_fragment_at(handle_.get(), index);
    if (!raw) detail::throw_error();
    return Fragment(raw);
}

}  // namespace lance
