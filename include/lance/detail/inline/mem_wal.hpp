/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the mem_wal domain.
#pragma once

#include "lance/detail/declarations.hpp"

namespace lance {

inline CompactedSsTable CompactedSsTable::from_uuid(const std::string& shard_id,
                                      uint64_t generation) {
        ::LanceCompactedSsTable raw{};
        if (lance_mem_wal_compacted_sstable_new(shard_id.c_str(), generation,
                                                &raw) != 0) {
            detail::throw_error();
        }
        CompactedSsTable value;
        std::memcpy(value.shard_id.data(), raw.shard_id,
                    value.shard_id.size());
        value.generation = raw.generation;
        return value;
    }

inline ::LanceCompactedSsTable CompactedSsTable::to_c() const {
        ::LanceCompactedSsTable raw{};
        std::memcpy(raw.shard_id, shard_id.data(), shard_id.size());
        raw.generation = generation;
        return raw;
    }

inline ShardWriterConfig& ShardWriterConfig::set_durable_write(bool value) { durable_write = value; return *this; }

inline ShardWriterConfig& ShardWriterConfig::set_max_wal_buffer_size(uint64_t value) { max_wal_buffer_size = value; return *this; }

inline ShardWriterConfig& ShardWriterConfig::set_max_wal_flush_interval_ms(uint64_t value) { max_wal_flush_interval_ms = value; return *this; }

inline ShardWriterConfig& ShardWriterConfig::set_max_memtable_size(uint64_t value) { max_memtable_size = value; return *this; }

inline ShardWriterConfig& ShardWriterConfig::set_max_memtable_rows(uint64_t value) { max_memtable_rows = value; return *this; }

inline ShardWriterConfig& ShardWriterConfig::set_max_memtable_batches(uint64_t value) { max_memtable_batches = value; return *this; }

inline ShardWriterConfig& ShardWriterConfig::set_max_unflushed_memtable_bytes(uint64_t value) { max_unflushed_memtable_bytes = value; return *this; }

inline ShardWriterConfig& ShardWriterConfig::set_manifest_scan_batch_size(uint64_t value) { manifest_scan_batch_size = value; return *this; }

inline ShardWriterConfig& ShardWriterConfig::set_backpressure_log_interval_ms(uint64_t value) { backpressure_log_interval_ms = value; return *this; }

inline ShardWriterConfig& ShardWriterConfig::set_stats_log_interval_ms(uint64_t value) { stats_log_interval_ms = value; return *this; }

inline ShardWriterConfig& ShardWriterConfig::add_hnsw_params(HnswParams value) { hnsw_params.push_back(std::move(value)); return *this; }

inline LanceShardWriterConfig ShardWriterConfig::to_c(std::vector<std::string>& names,
                                std::vector<::LanceMemWalHnswParams>& params) const {
        LanceShardWriterConfig raw{};
        if (durable_write) {
            raw.has_durable_write = 1;
            raw.durable_write = *durable_write ? 1 : 0;
        }
        auto set_u64 = [](std::optional<uint64_t> value, int32_t& has, uint64_t& out) {
            if (value) { has = 1; out = *value; }
        };
        set_u64(max_wal_buffer_size, raw.has_max_wal_buffer_size, raw.max_wal_buffer_size);
        set_u64(max_wal_flush_interval_ms, raw.has_max_wal_flush_interval_ms,
                raw.max_wal_flush_interval_ms);
        set_u64(max_memtable_size, raw.has_max_memtable_size, raw.max_memtable_size);
        set_u64(max_memtable_rows, raw.has_max_memtable_rows, raw.max_memtable_rows);
        set_u64(max_memtable_batches, raw.has_max_memtable_batches, raw.max_memtable_batches);
        set_u64(max_unflushed_memtable_bytes, raw.has_max_unflushed_memtable_bytes,
                raw.max_unflushed_memtable_bytes);
        set_u64(manifest_scan_batch_size, raw.has_manifest_scan_batch_size,
                raw.manifest_scan_batch_size);
        set_u64(backpressure_log_interval_ms, raw.has_backpressure_log_interval_ms,
                raw.backpressure_log_interval_ms);
        set_u64(stats_log_interval_ms, raw.has_stats_log_interval_ms,
                raw.stats_log_interval_ms);
        names.clear();
        names.reserve(hnsw_params.size());
        for (const auto& entry : hnsw_params) names.push_back(entry.index_name);
        params.clear();
        params.reserve(hnsw_params.size());
        for (size_t i = 0; i < hnsw_params.size(); ++i) {
            const auto& entry = hnsw_params[i];
            ::LanceMemWalHnswParams raw_entry{};
            raw_entry.index_name = names[i].c_str();
            if (entry.num_edges) {
                raw_entry.has_num_edges = 1;
                raw_entry.num_edges = *entry.num_edges;
            }
            if (entry.ef_construction) {
                raw_entry.has_ef_construction = 1;
                raw_entry.ef_construction = *entry.ef_construction;
            }
            if (entry.max_level) {
                raw_entry.has_max_level = 1;
                raw_entry.max_level = *entry.max_level;
            }
            params.push_back(raw_entry);
        }
        raw.hnsw_params = params.empty() ? nullptr : params.data();
        raw.num_hnsw_params = params.size();
        return raw;
    }

inline ShardingField::ShardingField(::LanceMemWalShardingField* field) : handle_(field) {
        if (!handle_) detail::throw_error();
    }

inline ShardingField ShardingField::create(
        std::string field_id,
        std::vector<int32_t> source_ids,
        std::optional<std::string> transform,
        std::optional<std::string> expression,
        std::string result_type,
        std::map<std::string, std::string> parameters) {
        std::vector<const char*> keys;
        std::vector<const char*> values;
        keys.reserve(parameters.size());
        values.reserve(parameters.size());
        for (const auto& [key, value] : parameters) {
            keys.push_back(key.c_str());
            values.push_back(value.c_str());
        }
        ::LanceMemWalShardingField* raw = nullptr;
        if (lance_mem_wal_sharding_field_new(
                field_id.c_str(),
                source_ids.empty() ? nullptr : source_ids.data(),
                source_ids.size(),
                transform ? transform->c_str() : nullptr,
                expression ? expression->c_str() : nullptr,
                result_type.c_str(),
                keys.empty() ? nullptr : keys.data(),
                values.empty() ? nullptr : values.data(),
                keys.size(),
                &raw) != 0) {
            detail::throw_error();
        }
        return ShardingField(raw);
    }

inline ShardingField ShardingField::clone() const {
        ::LanceMemWalShardingField* raw = nullptr;
        if (lance_mem_wal_sharding_field_clone(handle_.get(), &raw) != 0) {
            detail::throw_error();
        }
        return ShardingField(raw);
    }

inline std::string ShardingField::id() const {
        const char* value = lance_mem_wal_sharding_field_id(handle_.get());
        if (!value) detail::throw_error();
        return value;
    }

inline std::vector<int32_t> ShardingField::source_ids() const {
        size_t count = 0;
        const int32_t* values = lance_mem_wal_sharding_field_source_ids(handle_.get(), &count);
        if (!values && count != 0) detail::throw_error();
        return values ? std::vector<int32_t>(values, values + count)
                      : std::vector<int32_t>{};
    }

inline std::optional<std::string> ShardingField::transform() const {
        const char* value = lance_mem_wal_sharding_field_transform(handle_.get());
        if (value) return std::string(value);
        return std::nullopt;
    }

inline std::optional<std::string> ShardingField::expression() const {
        const char* value = lance_mem_wal_sharding_field_expression(handle_.get());
        if (value) return std::string(value);
        return std::nullopt;
    }

inline std::string ShardingField::result_type() const {
        const char* value = lance_mem_wal_sharding_field_result_type(handle_.get());
        if (!value) detail::throw_error();
        return value;
    }

inline std::map<std::string, std::string> ShardingField::parameters() const {
        std::map<std::string, std::string> result;
        const size_t count = lance_mem_wal_sharding_field_parameter_count(handle_.get());
        for (size_t i = 0; i < count; ++i) {
            const char* key = nullptr;
            const char* value = nullptr;
            if (lance_mem_wal_sharding_field_parameter_at(
                    handle_.get(), i, &key, &value) != 0) {
                detail::throw_error();
            }
            if (!key || !value) detail::throw_error();
            result.emplace(key, value);
        }
        return result;
    }

inline std::optional<std::string> ShardingField::parameter(const std::string& key) const {
        auto all = parameters();
        auto it = all.find(key);
        if (it == all.end()) return std::nullopt;
        return it->second;
    }

inline const ::LanceMemWalShardingField* ShardingField::c_handle() const { return handle_.get(); }

inline ShardingSpec::ShardingSpec(::LanceMemWalShardingSpec* spec) : handle_(spec) {
        if (!handle_) detail::throw_error();
    }

inline ShardingSpec ShardingSpec::create(uint32_t spec_id,
                               std::vector<ShardingField> fields) {
        std::vector<const ::LanceMemWalShardingField*> raw;
        raw.reserve(fields.size());
        for (const auto& field : fields) raw.push_back(field.c_handle());
        ::LanceMemWalShardingSpec* spec = nullptr;
        if (lance_mem_wal_sharding_spec_new(
                spec_id, raw.empty() ? nullptr : raw.data(), raw.size(), &spec) != 0) {
            detail::throw_error();
        }
        return ShardingSpec(spec);
    }

inline ShardingSpec ShardingSpec::clone() const {
        ::LanceMemWalShardingSpec* raw = nullptr;
        if (lance_mem_wal_sharding_spec_clone(handle_.get(), &raw) != 0) {
            detail::throw_error();
        }
        return ShardingSpec(raw);
    }

inline uint32_t ShardingSpec::id() const {
        return lance_mem_wal_sharding_spec_id(handle_.get());
    }

inline size_t ShardingSpec::field_count() const {
        return lance_mem_wal_sharding_spec_field_count(handle_.get());
    }

inline ShardingField ShardingSpec::field_at(size_t index) const {
        ::LanceMemWalShardingField* raw = nullptr;
        if (lance_mem_wal_sharding_spec_field_at(handle_.get(), index, &raw) != 0) {
            detail::throw_error();
        }
        return ShardingField(raw);
    }

inline std::vector<ShardingField> ShardingSpec::fields() const {
        std::vector<ShardingField> result;
        const size_t count = field_count();
        result.reserve(count);
        for (size_t i = 0; i < count; ++i) result.push_back(field_at(i));
        return result;
    }

inline void ShardingSpec::add_field(ShardingField field) {
        auto current = fields();
        current.push_back(std::move(field));
        auto next = ShardingSpec::create(id(), std::move(current));
        handle_ = std::move(next.handle_);
    }

inline const ::LanceMemWalShardingSpec* ShardingSpec::c_handle() const { return handle_.get(); }

inline ShardSnapshot::ShardSnapshot(::LanceMemWalShardSnapshot* snapshot) : handle_(snapshot) {
        if (!handle_) detail::throw_error();
    }

inline ShardSnapshot ShardSnapshot::create(const std::string& shard_id) {
        ::LanceMemWalShardSnapshot* raw = nullptr;
        if (lance_mem_wal_shard_snapshot_new(shard_id.c_str(), &raw) != 0) {
            detail::throw_error();
        }
        return ShardSnapshot(raw);
    }

inline ShardSnapshot ShardSnapshot::clone() const {
        ::LanceMemWalShardSnapshot* raw = nullptr;
        if (lance_mem_wal_shard_snapshot_clone(handle_.get(), &raw) != 0) {
            detail::throw_error();
        }
        return ShardSnapshot(raw);
    }

inline std::string ShardSnapshot::shard_id() const {
        const char* value = lance_mem_wal_shard_snapshot_shard_id(handle_.get());
        if (!value) detail::throw_error();
        return value;
    }

inline uint32_t ShardSnapshot::spec_id() const {
        return lance_mem_wal_shard_snapshot_spec_id(handle_.get());
    }

inline ShardSnapshot& ShardSnapshot::set_spec_id(uint32_t value) {
        if (lance_mem_wal_shard_snapshot_with_spec_id(handle_.get(), value) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline uint64_t ShardSnapshot::current_generation() const {
        return lance_mem_wal_shard_snapshot_current_generation(handle_.get());
    }

inline ShardSnapshot& ShardSnapshot::set_current_generation(uint64_t value) {
        if (lance_mem_wal_shard_snapshot_with_current_generation(
                handle_.get(), value) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline ShardSnapshot& ShardSnapshot::add_sstable(uint64_t generation, const std::string& path) {
        if (lance_mem_wal_shard_snapshot_with_sstable(
                handle_.get(), generation, path.c_str()) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline std::vector<SsTable> ShardSnapshot::sstables() const {
        std::vector<SsTable> result;
        const size_t count = lance_mem_wal_shard_snapshot_sstable_count(handle_.get());
        result.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            uint64_t generation = 0;
            const char* path = nullptr;
            if (lance_mem_wal_shard_snapshot_sstable_at(
                    handle_.get(), i, &generation, &path) != 0) {
                detail::throw_error();
            }
            if (!path) detail::throw_error();
            result.push_back(SsTable{generation, path});
        }
        return result;
    }

inline const ::LanceMemWalShardSnapshot* ShardSnapshot::c_handle() const { return handle_.get(); }

inline MemWalIndexDetails::MemWalIndexDetails(uint32_t num_shards,
                       bool maintain_all,
                       std::vector<std::string> maintained,
                       std::vector<ShardingSpec> specs,
                       std::map<std::string, std::string> defaults,
                       std::string dir_id) : num_shards_(num_shards),
          maintain_all_(maintain_all),
          maintained_(std::move(maintained)),
          specs_(std::move(specs)),
          defaults_(std::move(defaults)),
          dir_id_(std::move(dir_id)) {}

inline const std::string& MemWalIndexDetails::dir_id() const { return dir_id_; }

inline uint32_t MemWalIndexDetails::num_shards() const { return num_shards_; }

inline bool MemWalIndexDetails::maintain_all_indexes() const { return maintain_all_; }

inline const std::vector<std::string>& MemWalIndexDetails::maintained_indexes() const { return maintained_; }

inline const std::map<std::string, std::string>& MemWalIndexDetails::writer_config_defaults() const {
        return defaults_;
    }

inline size_t MemWalIndexDetails::sharding_spec_count() const { return specs_.size(); }

inline ShardingSpec MemWalIndexDetails::sharding_spec_at(size_t index) const {
        if (index >= specs_.size()) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "sharding spec index out of range");
        }
        return specs_[index].clone();
    }

inline std::vector<ShardingSpec> MemWalIndexDetails::sharding_specs() const {
        std::vector<ShardingSpec> result;
        result.reserve(specs_.size());
        for (const auto& spec : specs_) result.push_back(spec.clone());
        return result;
    }

inline ShardWriter::ShardWriter(::LanceShardWriter* writer) : handle_(writer) {
        if (!handle_) detail::throw_error();
    }

inline std::string ShardWriter::shard_id() const {
        const char* value = lance_shard_writer_shard_id(handle_.get());
        if (!value) detail::throw_error();
        return value;
    }

inline ShardWriteResult ShardWriter::put(ArrowArrayStream* stream) {
        ::LanceShardWriteResult raw{};
        if (lance_shard_writer_put(handle_.get(), stream, &raw) != 0) {
            detail::throw_error();
        }
        return ShardWriteResult{raw.batch_position_begin,
                                raw.batch_position_end};
    }

inline ShardWriteResult ShardWriter::delete_rows(ArrowArrayStream* stream) {
        ::LanceShardWriteResult raw{};
        if (lance_shard_writer_delete(handle_.get(), stream, &raw) != 0) {
            detail::throw_error();
        }
        return ShardWriteResult{raw.batch_position_begin,
                                raw.batch_position_end};
    }

inline ShardWriterStats ShardWriter::stats() const {
        ::LanceShardWriterStats raw{};
        if (lance_shard_writer_stats(handle_.get(), &raw) != 0) detail::throw_error();
        return ShardWriterStats{
            raw.put_count, raw.put_time_nanos,
            raw.wal_flush_count, raw.wal_flush_time_nanos, raw.wal_flush_bytes,
            raw.wal_io_time_nanos, raw.wal_io_count,
            raw.index_update_time_nanos, raw.index_update_count,
            raw.index_update_rows,
            raw.memtable_flush_count, raw.memtable_flush_time_nanos,
            raw.memtable_flush_rows, raw.memtable_flush_failures};
    }

inline ShardMemTableStats ShardWriter::memtable_stats() const {
        ::LanceShardMemTableStats raw{};
        if (lance_shard_writer_memtable_stats(handle_.get(), &raw) != 0) {
            detail::throw_error();
        }
        ShardMemTableStats stats;
        stats.is_live = raw.is_live != 0;
        stats.row_count = raw.row_count;
        stats.batch_count = raw.batch_count;
        stats.estimated_size_bytes = raw.estimated_size_bytes;
        stats.index_bytes = raw.index_bytes;
        stats.frozen_bytes = raw.frozen_bytes;
        stats.grace_bytes = raw.grace_bytes;
        stats.retained_bytes = raw.retained_bytes;
        stats.generation = raw.generation;
        if (raw.has_max_buffered_batch_position) {
            stats.max_buffered_batch_position = raw.max_buffered_batch_position;
        }
        stats.durable_batch_count = raw.durable_batch_count;
        stats.global_offset = raw.global_offset;
        if (raw.has_pending_wal_start_batch_position) {
            stats.pending_wal_start_batch_position =
                raw.pending_wal_start_batch_position;
        }
        if (raw.has_pending_wal_end_batch_position) {
            stats.pending_wal_end_batch_position =
                raw.pending_wal_end_batch_position;
        }
        stats.pending_wal_batch_count = raw.pending_wal_batch_count;
        stats.pending_wal_row_count = raw.pending_wal_row_count;
        stats.pending_wal_estimated_bytes = raw.pending_wal_estimated_bytes;
        stats.frozen_count = raw.frozen_count;
        return stats;
    }

inline std::optional<ShardSnapshot> ShardWriter::snapshot() const {
        ::LanceMemWalShardSnapshot* raw = nullptr;
        if (lance_shard_writer_snapshot(handle_.get(), &raw) != 0) detail::throw_error();
        if (!raw) return std::nullopt;
        return ShardSnapshot(raw);
    }

inline void ShardWriter::close() {
        if (lance_shard_writer_close(handle_.get()) != 0) detail::throw_error();
    }

inline const ::LanceShardWriter* ShardWriter::c_handle() const { return handle_.get(); }

inline LsmScanner::LsmScanner(::LanceLsmScanner* scanner) : handle_(scanner) {
        if (!handle_) detail::throw_error();
    }

inline LsmScanner& LsmScanner::project(const std::vector<std::string>& columns) {
        std::vector<const char*> raw;
        raw.reserve(columns.size());
        for (const auto& column : columns) raw.push_back(column.c_str());
        if (lance_lsm_scanner_project(
                handle_.get(), raw.empty() ? nullptr : raw.data(),
                raw.size()) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline LsmScanner& LsmScanner::filter(const std::string& expression) {
        if (lance_lsm_scanner_filter(handle_.get(), expression.c_str()) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline LsmScanner& LsmScanner::limit(std::optional<int64_t> limit,
                      std::optional<int64_t> offset) {
        if (lance_lsm_scanner_limit(
                handle_.get(), limit.value_or(0), limit.has_value(),
                offset.value_or(0), offset.has_value()) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline LsmScanner& LsmScanner::with_row_address() {
        if (lance_lsm_scanner_with_row_address(handle_.get()) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline LsmScanner& LsmScanner::with_memtable_gen() {
        if (lance_lsm_scanner_with_memtable_gen(handle_.get()) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline uint64_t LsmScanner::count_rows() const {
        uint64_t count = 0;
        if (lance_lsm_scanner_count_rows(handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline ArrowStream LsmScanner::to_reader() const {
        ArrowArrayStream raw{};
        if (lance_lsm_scanner_to_arrow_stream(handle_.get(), &raw) != 0) {
            detail::throw_error();
        }
        return ArrowStream(raw);
    }

inline const ::LanceLsmScanner* LsmScanner::c_handle() const { return handle_.get(); }

inline ExecutionPlan::ExecutionPlan(::LanceMemWalPlan* plan) : handle_(plan) {
        if (!handle_) detail::throw_error();
    }

inline std::string ExecutionPlan::explain() const {
        char* text = nullptr;
        if (lance_mem_wal_plan_explain(handle_.get(), &text) != 0) {
            detail::throw_error();
        }
        return detail::take_string(text);
    }

inline ArrowStream ExecutionPlan::to_reader() const {
        ArrowArrayStream raw{};
        if (lance_mem_wal_plan_to_arrow_stream(handle_.get(), &raw) != 0) {
            detail::throw_error();
        }
        return ArrowStream(raw);
    }

inline const ::LanceMemWalPlan* ExecutionPlan::c_handle() const { return handle_.get(); }

inline LsmPointLookupPlanner::LsmPointLookupPlanner(::LanceLsmPointLookupPlanner* planner) : handle_(planner) {
        if (!handle_) detail::throw_error();
    }

inline LsmVectorSearchPlanner::LsmVectorSearchPlanner(::LanceLsmVectorSearchPlanner* planner) : handle_(planner) {
        if (!handle_) detail::throw_error();
    }

inline Batch ShardingSpec::evaluate(
    const Dataset& dataset, const Batch& batch) const {
    ::LanceBatch* raw = nullptr;
    if (lance_mem_wal_evaluate_sharding_spec(
            const_cast<::LanceDataset*>(dataset.c_handle()),
            batch.c_handle(), handle_.get(), &raw) != 0) {
        detail::throw_error();
    }
    return Batch(raw);
}

inline Batch ShardingSpec::evaluate(
    const Batch& batch, const LanceSchema& schema) const {
    ::LanceBatch* raw = nullptr;
    if (lance_mem_wal_evaluate_sharding_spec_with_schema(
            batch.c_handle(), handle_.get(), schema.c_handle(), &raw) != 0) {
        detail::throw_error();
    }
    return Batch(raw);
}

inline ShardWriter ShardWriter::open(const Dataset& dataset,
                                     const std::string& shard_id,
                                     const ShardWriterConfig& config) {
    return dataset.mem_wal_writer(shard_id, config);
}

inline LsmScanner ShardWriter::lsm_scanner(
    std::vector<ShardSnapshot> snapshots) {
    std::vector<const ::LanceMemWalShardSnapshot*> raw;
    raw.reserve(snapshots.size());
    for (const auto& snapshot : snapshots) raw.push_back(snapshot.c_handle());
    ::LanceLsmScanner* scanner = nullptr;
    if (lance_shard_writer_lsm_scanner(
            handle_.get(), raw.empty() ? nullptr : raw.data(), raw.size(),
            &scanner) != 0) {
        detail::throw_error();
    }
    return LsmScanner(scanner);
}

inline LsmScanner LsmScanner::from_snapshots(
    const Dataset& dataset, std::vector<ShardSnapshot> snapshots) {
    std::vector<const ::LanceMemWalShardSnapshot*> raw;
    raw.reserve(snapshots.size());
    for (const auto& snapshot : snapshots) raw.push_back(snapshot.c_handle());
    ::LanceLsmScanner* scanner = nullptr;
    if (lance_lsm_scanner_from_snapshots(
            const_cast<::LanceDataset*>(dataset.c_handle()),
            raw.empty() ? nullptr : raw.data(), raw.size(), &scanner) != 0) {
        detail::throw_error();
    }
    return LsmScanner(scanner);
}

inline Batch LsmScanner::to_batch() const {
    ::LanceBatch* raw = nullptr;
    if (lance_lsm_scanner_to_batch(handle_.get(), &raw) != 0) detail::throw_error();
    return Batch(raw);
}

inline std::vector<Batch> LsmScanner::to_batches() const {
    std::vector<Batch> batches;
    batches.push_back(to_batch());
    return batches;
}

inline Table LsmScanner::to_table() const {
    Table table;
    ArrowSchema raw{};
    if (lance_lsm_scanner_schema(handle_.get(), &raw) != 0) {
        detail::throw_error();
    }
    table.schema = ArrowSchemaOwner(raw);
    table.batches = to_batches();
    return table;
}

inline ArrowSchemaOwner ExecutionPlan::schema() const {
    ArrowSchema raw{};
    if (lance_mem_wal_plan_schema(handle_.get(), &raw) != 0) {
        detail::throw_error();
    }
    return ArrowSchemaOwner(raw);
}

inline ArrowSchemaOwner ExecutionPlan::dataset_schema() const {
    ArrowSchema raw{};
    if (lance_mem_wal_plan_dataset_schema(handle_.get(), &raw) != 0) {
        detail::throw_error();
    }
    return ArrowSchemaOwner(raw);
}

inline Batch ExecutionPlan::to_batch() const {
    ::LanceBatch* raw = nullptr;
    if (lance_mem_wal_plan_to_batch(handle_.get(), &raw) != 0) detail::throw_error();
    return Batch(raw);
}

inline std::vector<Batch> ExecutionPlan::to_batches() const {
    std::vector<Batch> batches;
    batches.push_back(to_batch());
    return batches;
}

inline Table ExecutionPlan::to_table() const {
    Table table;
    table.schema = schema();
    table.batches = to_batches();
    return table;
}

inline LsmPointLookupPlanner::LsmPointLookupPlanner(
    const Dataset& dataset, std::vector<ShardSnapshot> snapshots,
    std::optional<std::vector<std::string>> pk_columns) {
    std::vector<const ::LanceMemWalShardSnapshot*> raw;
    raw.reserve(snapshots.size());
    for (const auto& snapshot : snapshots) raw.push_back(snapshot.c_handle());
    std::vector<const char*> columns;
    if (pk_columns) {
        columns.reserve(pk_columns->size());
        for (const auto& column : *pk_columns) columns.push_back(column.c_str());
    }
    ::LanceLsmPointLookupPlanner* planner = nullptr;
    if (lance_lsm_point_lookup_planner_new(
            const_cast<::LanceDataset*>(dataset.c_handle()),
            raw.empty() ? nullptr : raw.data(), raw.size(),
            pk_columns.has_value() ? 1 : 0,
            columns.empty() ? nullptr : columns.data(), columns.size(),
            &planner) != 0) {
        detail::throw_error();
    }
    handle_ = Handle<::LanceLsmPointLookupPlanner,
                     lance_lsm_point_lookup_planner_free>(planner);
}

inline ExecutionPlan LsmPointLookupPlanner::plan_lookup(
    const Batch& pk_value,
    std::optional<std::vector<std::string>> columns) const {
    std::vector<const char*> raw_columns;
    if (columns) {
        raw_columns.reserve(columns->size());
        for (const auto& column : *columns) raw_columns.push_back(column.c_str());
    }
    ::LanceMemWalPlan* plan = nullptr;
    if (lance_lsm_point_lookup_planner_plan(
            handle_.get(), pk_value.c_handle(),
            columns.has_value() ? 1 : 0,
            raw_columns.empty() ? nullptr : raw_columns.data(),
            raw_columns.size(), &plan) != 0) {
        detail::throw_error();
    }
    return ExecutionPlan(plan);
}

inline LsmVectorSearchPlanner::LsmVectorSearchPlanner(
    const Dataset& dataset, std::vector<ShardSnapshot> snapshots,
    std::string vector_column,
    std::optional<std::vector<std::string>> pk_columns,
    MemWalDistanceType distance_type, std::optional<std::string> filter) {
    std::vector<const ::LanceMemWalShardSnapshot*> raw;
    raw.reserve(snapshots.size());
    for (const auto& snapshot : snapshots) raw.push_back(snapshot.c_handle());
    std::vector<const char*> columns;
    if (pk_columns) {
        columns.reserve(pk_columns->size());
        for (const auto& column : *pk_columns) columns.push_back(column.c_str());
    }
    ::LanceLsmVectorSearchPlanner* planner = nullptr;
    if (lance_lsm_vector_search_planner_new(
            const_cast<::LanceDataset*>(dataset.c_handle()),
            raw.empty() ? nullptr : raw.data(), raw.size(),
            vector_column.c_str(), pk_columns.has_value() ? 1 : 0,
            columns.empty() ? nullptr : columns.data(), columns.size(),
            static_cast<::LanceMemWalDistanceType>(distance_type),
            filter ? filter->c_str() : nullptr, &planner) != 0) {
        detail::throw_error();
    }
    handle_ = Handle<::LanceLsmVectorSearchPlanner,
                     lance_lsm_vector_search_planner_free>(planner);
}

inline ExecutionPlan LsmVectorSearchPlanner::plan_search(
    const std::vector<float>& query, uint64_t k, uint64_t nprobes,
    std::optional<std::vector<std::string>> columns, bool refine_base_table,
    double overfetch_factor) const {
    std::vector<const char*> raw_columns;
    if (columns) {
        raw_columns.reserve(columns->size());
        for (const auto& column : *columns) raw_columns.push_back(column.c_str());
    }
    ::LanceMemWalPlan* plan = nullptr;
    if (lance_lsm_vector_search_planner_plan(
            handle_.get(), query.empty() ? nullptr : query.data(), query.size(),
            k, nprobes, columns.has_value() ? 1 : 0,
            raw_columns.empty() ? nullptr : raw_columns.data(),
            raw_columns.size(), refine_base_table ? 1 : 0, overfetch_factor,
            &plan) != 0) {
        detail::throw_error();
    }
    return ExecutionPlan(plan);
}

}  // namespace lance
