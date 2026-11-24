/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the scanner domain.
#pragma once

#include "lance/detail/declarations.hpp"
#include "lance/detail/inline/base.hpp"

namespace lance {

inline Scanner::Scanner(LanceScanner* s) : handle_(s) {}

inline Scanner& Scanner::limit(int64_t n) {
        if (lance_scanner_set_limit(handle_.get(), n) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::offset(int64_t n) {
        if (lance_scanner_set_offset(handle_.get(), n) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::batch_size(int64_t n) {
        if (lance_scanner_set_batch_size(handle_.get(), n) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::batch_size_bytes(uint64_t bytes) {
        if (lance_scanner_set_batch_size_bytes(handle_.get(), bytes) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::io_buffer_size(uint64_t bytes) {
        if (lance_scanner_set_io_buffer_size(handle_.get(), bytes) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::batch_readahead(size_t batches) {
        if (lance_scanner_set_batch_readahead(handle_.get(), batches) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::fragment_readahead(size_t fragments) {
        if (lance_scanner_set_fragment_readahead(handle_.get(), fragments) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::target_parallelism(size_t partitions) {
        if (lance_scanner_set_target_parallelism(handle_.get(), partitions) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::scan_in_order(bool ordered) {
        if (lance_scanner_set_scan_in_order(handle_.get(), ordered) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::use_scalar_index(bool enable) {
        if (lance_scanner_set_use_scalar_index(handle_.get(), enable) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::strict_batch_size(bool strict_batch_size) {
        if (lance_scanner_set_strict_batch_size(handle_.get(), strict_batch_size) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::use_stats(bool use_stats) {
        if (lance_scanner_set_use_stats(handle_.get(), use_stats) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::project(std::nullopt_t) {
        if (lance_scanner_set_projection(handle_.get(), nullptr, 0) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::project(const std::vector<ProjectionColumn>& columns) {
        std::vector<LanceProjectionColumn> native;
        native.reserve(columns.size());
        for (const auto& column : columns) {
            native.push_back({
                column.output_name.c_str(),
                column.expression.c_str(),
            });
        }
        LanceProjectionColumn explicit_empty = {};
        const LanceProjectionColumn* data =
            native.empty() ? &explicit_empty : native.data();
        if (lance_scanner_set_projection(handle_.get(), data, native.size()) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::disable_scoring_autoprojection() {
        if (lance_scanner_disable_scoring_autoprojection(handle_.get()) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::order_by(std::nullopt_t) {
        if (lance_scanner_set_order_by(handle_.get(), nullptr, 0) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::order_by(const std::vector<ColumnOrdering>& orderings) {
        std::vector<LanceColumnOrdering> native;
        native.reserve(orderings.size());
        for (const auto& ordering : orderings) {
            native.push_back({
                ordering.column_name.c_str(),
                static_cast<int32_t>(ordering.direction),
                static_cast<int32_t>(ordering.null_order),
            });
        }
        LanceColumnOrdering explicit_empty = {};
        const LanceColumnOrdering* data =
            native.empty() ? &explicit_empty : native.data();
        if (lance_scanner_set_order_by(handle_.get(), data, native.size()) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::blob_handling(LanceBlobHandling handling) {
        if (lance_scanner_set_blob_handling(handle_.get(), handling) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::materialization_style(
        LanceMaterializationStyle style,
        const std::vector<std::string>& early_except_columns) {
        std::vector<const char*> column_ptrs;
        column_ptrs.reserve(early_except_columns.size());
        for (const auto& column : early_except_columns) {
            column_ptrs.push_back(column.c_str());
        }
        const char* const* columns =
            column_ptrs.empty() ? nullptr : column_ptrs.data();
        if (lance_scanner_set_materialization_style(
                handle_.get(), style, columns, column_ptrs.size()) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::with_row_id(bool enable) {
        if (lance_scanner_with_row_id(handle_.get(), enable) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::with_row_address(bool enable) {
        if (lance_scanner_with_row_address(handle_.get(), enable) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::include_deleted_rows(bool include_deleted_rows) {
        if (lance_scanner_set_include_deleted_rows(handle_.get(), include_deleted_rows) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::scalar_index_segment(const std::array<uint8_t, 16>& segment_uuid) {
        if (lance_scanner_set_scalar_index_segment(handle_.get(), segment_uuid.data()) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::scalar_index_auto_selection(bool enabled) {
        if (lance_scanner_set_scalar_index_auto_selection(handle_.get(), enabled) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::clear_scalar_index_segment() {
        if (lance_scanner_set_scalar_index_segment(handle_.get(), nullptr) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::fragment_ids(const uint64_t* ids, size_t len) {
        if (lance_scanner_set_fragment_ids(handle_.get(), ids, len) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::fragment_ids(const std::vector<uint64_t>& ids) {
        return fragment_ids(ids.data(), ids.size());
    }

inline Scanner& Scanner::row_addr_allowlist(const uint64_t* row_ids, size_t len) {
        if (lance_scanner_set_row_addr_allowlist(handle_.get(), row_ids, len) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::row_addr_allowlist(const std::vector<uint64_t>& row_ids) {
        return row_addr_allowlist(row_ids.data(), row_ids.size());
    }

inline Scanner& Scanner::row_addr_blocklist(const uint64_t* row_ids, size_t len) {
        if (lance_scanner_set_row_addr_blocklist(handle_.get(), row_ids, len) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::row_addr_blocklist(const std::vector<uint64_t>& row_ids) {
        return row_addr_blocklist(row_ids.data(), row_ids.size());
    }

inline Scanner& Scanner::substrait_filter(const uint8_t* bytes, size_t len) {
        if (lance_scanner_set_substrait_filter(handle_.get(), bytes, len) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::substrait_filter(const std::vector<uint8_t>& bytes) {
        return substrait_filter(bytes.data(), bytes.size());
    }

inline Scanner& Scanner::substrait_aggregate(const uint8_t* bytes, size_t len) {
        if (lance_scanner_set_substrait_aggregate(handle_.get(), bytes, len) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::substrait_aggregate(const std::vector<uint8_t>& bytes) {
        return substrait_aggregate(bytes.data(), bytes.size());
    }

inline Scanner& Scanner::additional_sql_filter(const std::string& filter) {
        if (lance_scanner_additional_sql_filter(handle_.get(), filter.c_str()) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::statistics_callback(LanceScanStatisticsCallback callback, void* callback_ctx) {
        if (lance_scanner_set_statistics_callback(handle_.get(), callback, callback_ctx) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::index_segments(const uint8_t* uuids, size_t len) {
        if (lance_scanner_set_index_segments(handle_.get(), uuids, len) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::index_segments(const std::vector<std::array<uint8_t, 16>>& uuids) {
        return index_segments(reinterpret_cast<const uint8_t*>(uuids.data()), uuids.size());
    }

inline void Scanner::schema(ArrowSchema* out) const {
        if (lance_scanner_schema(handle_.get(), out) != 0)
            detail::throw_error();
    }

inline std::string Scanner::explain_plan(bool verbose) const {
        return detail::take_string(
            lance_scanner_explain_plan(handle_.get(), verbose));
    }

inline std::string Scanner::analyze_plan() const {
        return detail::take_string(
            lance_scanner_analyze_plan(handle_.get()));
    }

inline uint64_t Scanner::count_rows() const {
        uint64_t count = lance_scanner_count_rows(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return count;
    }

inline std::string Scanner::analyze_count_plan() const {
        return detail::take_string(
            lance_scanner_analyze_count_plan(handle_.get()));
    }

inline void Scanner::to_arrow_stream(ArrowArrayStream* out) {
        if (lance_scanner_to_arrow_stream(handle_.get(), out) != 0)
            detail::throw_error();
    }

inline ArrowStream Scanner::to_arrow_stream() {
        ArrowArrayStream raw{};
        to_arrow_stream(&raw);
        return ArrowStream(raw);
    }

inline void Scanner::scan_async(LanceCallback callback, void* ctx) const {
        lance_scanner_scan_async(handle_.get(), callback, ctx);
    }

inline Scanner& Scanner::nearest(const std::string& column, const float* q, size_t dim, uint32_t k) {
        if (lance_scanner_nearest(handle_.get(), column.c_str(),
                                   q, dim, LANCE_DTYPE_FLOAT32, k) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::nearest(const std::string& column, std::span<const float> q, uint32_t k) {
        return nearest(column, q.data(), q.size(), k);
    }

inline Scanner& Scanner::nearest(const std::string& column, const void* q, size_t dim,
                     LanceDataType dtype, uint32_t k) {
        if (lance_scanner_nearest(handle_.get(), column.c_str(),
                                   q, dim, dtype, k) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::distance_range(std::optional<float> lower_bound,
                            std::optional<float> upper_bound) {
        if (lance_scanner_set_distance_range(handle_.get(),
                                            lower_bound ? &*lower_bound : nullptr,
                                            upper_bound ? &*upper_bound : nullptr) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::nearest_batch(const std::string& column, const void* query_data,
                           size_t dimension, size_t num_queries,
                           LanceDataType element_type, uint32_t k) {
        if (lance_scanner_nearest_batch(handle_.get(), column.c_str(), query_data,
                                       dimension, num_queries, element_type, k) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::nearest_batch(const std::string& column, const float* query_data,
                           size_t dimension, size_t num_queries, uint32_t k) {
        return nearest_batch(column, query_data, dimension, num_queries, LANCE_DTYPE_FLOAT32, k);
    }

inline Scanner& Scanner::nearest_batch(const std::string& column, std::span<const float> queries,
                           size_t dimension, uint32_t k) {
        if (dimension != 0 && queries.size() % dimension != 0) {
            throw Error(
                LANCE_ERR_INVALID_ARGUMENT,
                "nearest_batch query_data length " + std::to_string(queries.size()) +
                    " is not a multiple of dimension " + std::to_string(dimension));
        }
        const size_t num_queries =
            dimension == 0 ? 0 : queries.size() / dimension;
        return nearest_batch(column, queries.data(), dimension, num_queries, k);
    }

inline Scanner& Scanner::nearest_multivector(const std::string& column, const void* query_data,
                                size_t dimension, size_t num_vectors,
                                LanceDataType element_type, uint32_t k) {
        if (lance_scanner_nearest_multivector(handle_.get(), column.c_str(), query_data,
                                            dimension, num_vectors, element_type, k) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::nprobes(uint32_t nprobes) {
        if (lance_scanner_set_nprobes(handle_.get(), nprobes) != 0) detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::minimum_nprobes(uint32_t minimum_nprobes) {
        if (lance_scanner_set_minimum_nprobes(handle_.get(), minimum_nprobes) != 0) detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::maximum_nprobes(uint32_t maximum_nprobes) {
        if (lance_scanner_set_maximum_nprobes(handle_.get(), maximum_nprobes) != 0) detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::search_effort(double effort) {
        if (lance_scanner_set_search_effort(handle_.get(), effort) != 0) detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::approx_mode(LanceApproxMode approx_mode) {
        if (lance_scanner_set_approx_mode(handle_.get(), approx_mode) != 0) detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::query_parallelism(int32_t parallelism) {
        if (lance_scanner_set_query_parallelism(handle_.get(), parallelism) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::refine_factor(uint32_t f) {
        if (lance_scanner_set_refine_factor(handle_.get(), f) != 0) detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::ef(uint32_t e) {
        if (lance_scanner_set_ef(handle_.get(), e) != 0) detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::metric(LanceMetricType m) {
        if (lance_scanner_set_metric(handle_.get(), m) != 0) detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::use_index(bool enable) {
        if (lance_scanner_set_use_index(handle_.get(), enable) != 0) detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::fast_search(bool enable) {
        if (lance_scanner_set_fast_search(handle_.get(), enable) != 0) detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::prefilter(bool enable) {
        if (lance_scanner_set_prefilter(handle_.get(), enable) != 0) detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::full_text_search(const std::string& query,
                              const std::vector<std::string>& columns,
                              uint32_t max_fuzzy_distance) {
        std::vector<const char*> col_ptrs;
        for (auto& c : columns) col_ptrs.push_back(c.c_str());
        col_ptrs.push_back(nullptr);
        const char* const* cols_c =
            columns.empty() ? nullptr : col_ptrs.data();
        if (lance_scanner_full_text_search(handle_.get(), query.c_str(),
                                            cols_c, max_fuzzy_distance) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::full_text_search(const FtsQuery& query) {
        if (lance_scanner_set_fts_query(handle_.get(), query.c_handle()) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::minhash_search(const std::string& column, const std::string& text) {
        if (lance_scanner_set_minhash_query(handle_.get(), column.c_str(),
                                            text.c_str()) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::fts_query_context(const FtsQueryContext& context) {
        if (lance_scanner_set_fts_query_context(handle_.get(), context.c_handle()) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::fts_index_segments(const uint8_t* segment_uuids, size_t segment_count) {
        if (lance_scanner_set_fts_index_segments(
                handle_.get(), segment_uuids, segment_count) != 0)
            detail::throw_error();
        return *this;
    }

inline Scanner& Scanner::fts_index_segments(
        const std::vector<std::array<uint8_t, 16>>& segment_uuids) {
        return fts_index_segments(
            reinterpret_cast<const uint8_t*>(segment_uuids.data()),
            segment_uuids.size());
    }

inline LanceScanner* Scanner::c_handle() { return handle_.get(); }

}  // namespace lance
