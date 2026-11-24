/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the search domain.
#pragma once

#include "lance/detail/declarations.hpp"

namespace lance {

inline FtsQueryContext::FtsQueryContext(LanceFtsQueryContext* context) : handle_(context) {}

inline const LanceFtsQueryContext* FtsQueryContext::c_handle() const { return handle_.get(); }

inline FtsQuery::FtsQuery(LanceFtsQuery* query) : handle_(query) {}

inline void FtsQuery::copy_boosts(
        const std::optional<std::vector<float>>& boosts,
        float& empty_boost,
        const float*& data,
        size_t& count) {
        data = nullptr;
        count = 0;
        if (!boosts) return;
        empty_boost = 0.0f;
        data = boosts->empty() ? &empty_boost : boosts->data();
        count = boosts->size();
    }

inline FtsQuery FtsQuery::match(
        const std::string& terms,
        std::optional<std::string> column,
        const FtsMatchOptions& options) {
        LanceFtsMatchOptions c_options{};
        lance_fts_query_match_options_init(&c_options);
        c_options.boost = options.boost;
        c_options.fuzziness = options.fuzziness
                                  ? static_cast<int64_t>(*options.fuzziness)
                                  : -1;
        c_options.max_expansions = options.max_expansions;
        c_options.match_operator = static_cast<int32_t>(options.match_operator);
        c_options.prefix_length = options.prefix_length;
        c_options.document_granularity =
            static_cast<int32_t>(options.document_granularity);

        auto* query = lance_fts_query_match_new(
            terms.c_str(), column ? column->c_str() : nullptr, &c_options);
        if (!query) detail::throw_error();
        return FtsQuery(query);
    }

inline FtsQuery FtsQuery::phrase(
        const std::string& terms,
        std::optional<std::string> column,
        const FtsPhraseOptions& options) {
        LanceFtsPhraseOptions c_options{};
        lance_fts_query_phrase_options_init(&c_options);
        c_options.slop = options.slop;
        c_options.document_granularity =
            static_cast<int32_t>(options.document_granularity);

        auto* query = lance_fts_query_phrase_new(
            terms.c_str(), column ? column->c_str() : nullptr, &c_options);
        if (!query) detail::throw_error();
        return FtsQuery(query);
    }

inline FtsQuery FtsQuery::boolean(
        const std::vector<std::pair<FtsOccur, const FtsQuery*>>& clauses) {
        std::vector<int32_t> occurs;
        std::vector<const LanceFtsQuery*> queries;
        occurs.reserve(clauses.size());
        queries.reserve(clauses.size());
        for (const auto& [occur, query] : clauses) {
            occurs.push_back(static_cast<int32_t>(occur));
            queries.push_back(query ? query->c_handle() : nullptr);
        }

        auto* result = lance_fts_query_boolean_new(
            occurs.data(), queries.data(), clauses.size());
        if (!result) detail::throw_error();
        return FtsQuery(result);
    }

inline FtsQuery FtsQuery::boost(
        const FtsQuery& positive,
        const FtsQuery& negative,
        const FtsBoostOptions& options) {
        LanceFtsBoostOptions c_options{};
        lance_fts_query_boost_options_init(&c_options);
        c_options.negative_boost = options.negative_boost;

        auto* query = lance_fts_query_boost_new(
            positive.c_handle(), negative.c_handle(), &c_options);
        if (!query) detail::throw_error();
        return FtsQuery(query);
    }

inline FtsQuery FtsQuery::multi_match(
        const std::string& terms,
        const std::vector<std::string>& columns,
        const FtsMultiMatchOptions& options) {
        std::vector<const char*> column_ptrs;
        column_ptrs.reserve(columns.size());
        for (const auto& column : columns) column_ptrs.push_back(column.c_str());

        LanceFtsMultiMatchOptions c_options{};
        lance_fts_query_multi_match_options_init(&c_options);
        float empty_boost = 0.0f;
        copy_boosts(options.boosts, empty_boost, c_options.boosts,
                    c_options.boost_count);
        c_options.match_operator = static_cast<int32_t>(options.match_operator);

        auto* query = lance_fts_query_multi_match_new(
            terms.c_str(), column_ptrs.data(), column_ptrs.size(), &c_options);
        if (!query) detail::throw_error();
        return FtsQuery(query);
    }

inline FtsQuery FtsQuery::combined_fields(
        const std::string& terms,
        const std::vector<std::string>& columns,
        const FtsCombinedFieldsOptions& options) {
        std::vector<const char*> column_ptrs;
        column_ptrs.reserve(columns.size());
        for (const auto& column : columns) column_ptrs.push_back(column.c_str());

        LanceFtsCombinedFieldsOptions c_options{};
        lance_fts_query_combined_fields_options_init(&c_options);
        float empty_boost = 0.0f;
        copy_boosts(options.boosts, empty_boost, c_options.boosts,
                    c_options.boost_count);
        c_options.match_operator = static_cast<int32_t>(options.match_operator);

        auto* query = lance_fts_query_combined_fields_new(
            terms.c_str(), column_ptrs.data(), column_ptrs.size(), &c_options);
        if (!query) detail::throw_error();
        return FtsQuery(query);
    }

inline const LanceFtsQuery* FtsQuery::c_handle() const { return handle_.get(); }

}  // namespace lance
