/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the sql domain.
#pragma once

#include "lance/detail/declarations.hpp"

namespace lance {

inline SqlQuery::SqlQuery(LanceSqlQuery* query) : handle_(query) {}

inline ArrowStream SqlQuery::into_stream() {
        auto* raw = handle_.release();
        ArrowArrayStream stream{};
        if (lance_sql_query_into_stream(raw, &stream) != 0) {
            detail::throw_error();
        }
        return ArrowStream(stream);
    }

inline SqlQueryBuilder::SqlQueryBuilder(LanceSqlQueryBuilder* builder) : handle_(builder) {}

inline SqlQueryBuilder& SqlQueryBuilder::table_name(const std::string& name) {
        if (lance_sql_query_builder_set_table_name(handle_.get(),
                                                    name.c_str()) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline SqlQueryBuilder& SqlQueryBuilder::with_row_id(bool enabled) {
        if (lance_sql_query_builder_set_with_row_id(handle_.get(), enabled) !=
            0) {
            detail::throw_error();
        }
        return *this;
    }

inline SqlQueryBuilder& SqlQueryBuilder::with_row_addr(bool enabled) {
        if (lance_sql_query_builder_set_with_row_addr(handle_.get(), enabled) !=
            0) {
            detail::throw_error();
        }
        return *this;
    }

inline SqlQueryBuilder& SqlQueryBuilder::blob_handling(LanceBlobHandling handling) {
        if (lance_sql_query_builder_set_blob_handling(handle_.get(),
                                                       handling) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline SqlQueryBuilder& SqlQueryBuilder::batch_size(uint64_t rows) {
        if (lance_sql_query_builder_set_batch_size(handle_.get(), rows) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline SqlQueryBuilder& SqlQueryBuilder::batch_size_bytes(uint64_t bytes) {
        if (lance_sql_query_builder_set_batch_size_bytes(handle_.get(),
                                                         bytes) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline SqlQuery SqlQueryBuilder::build() {
        auto* raw = handle_.release();
        LanceSqlQuery* query = nullptr;
        if (lance_sql_query_builder_build(raw, &query) != 0) {
            detail::throw_error();
        }
        return SqlQuery(query);
    }

}  // namespace lance
