/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the delta domain.
#pragma once

#include "lance/detail/declarations.hpp"

namespace lance {

inline DatasetDelta::DatasetDelta(LanceDatasetDelta* delta) : handle_(delta) {}

inline std::vector<Transaction> DatasetDelta::list_transactions() const {
        LanceTransaction** transactions = nullptr;
        size_t count = 0;
        if (lance_dataset_delta_list_transactions(handle_.get(), &transactions,
                                                   &count) != 0) {
            detail::throw_error();
        }
        auto free_transactions = [count](LanceTransaction** items) {
            lance_transaction_array_free(items, count);
        };
        std::unique_ptr<LanceTransaction*, decltype(free_transactions)>
            guard(transactions, free_transactions);
        std::vector<Transaction> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            LanceTransaction* item = transactions[i];
            transactions[i] = nullptr;
            out.push_back(Transaction(item));
        }
        return out;
    }

inline ArrowStream DatasetDelta::get_inserted_rows() const {
        ArrowArrayStream stream{};
        if (lance_dataset_delta_get_inserted_rows(handle_.get(), &stream) != 0) {
            detail::throw_error();
        }
        return ArrowStream(stream);
    }

inline ArrowStream DatasetDelta::get_updated_rows() const {
        ArrowArrayStream stream{};
        if (lance_dataset_delta_get_updated_rows(handle_.get(), &stream) != 0) {
            detail::throw_error();
        }
        return ArrowStream(stream);
    }

inline ArrowStream DatasetDelta::get_upserted_rows() const {
        ArrowArrayStream stream{};
        if (lance_dataset_delta_get_upserted_rows(handle_.get(), &stream) != 0) {
            detail::throw_error();
        }
        return ArrowStream(stream);
    }

inline ArrowStream DatasetDelta::get_deleted_row_ids() const {
        ArrowArrayStream stream{};
        if (lance_dataset_delta_get_deleted_row_ids(handle_.get(), &stream) != 0) {
            detail::throw_error();
        }
        return ArrowStream(stream);
    }

inline DatasetDeltaBuilder::DatasetDeltaBuilder(LanceDatasetDeltaBuilder* builder) : handle_(builder) {}

inline DatasetDeltaBuilder& DatasetDeltaBuilder::compared_against_version(uint64_t version) {
        if (lance_dataset_delta_builder_set_compared_against_version(
                handle_.get(), version) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline DatasetDeltaBuilder& DatasetDeltaBuilder::begin_version(uint64_t version) {
        if (lance_dataset_delta_builder_set_begin_version(handle_.get(),
                                                           version) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline DatasetDeltaBuilder& DatasetDeltaBuilder::end_version(uint64_t version) {
        if (lance_dataset_delta_builder_set_end_version(handle_.get(),
                                                         version) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline DatasetDeltaBuilder& DatasetDeltaBuilder::begin_timestamp(const LanceDeltaTimestamp& timestamp) {
        if (lance_dataset_delta_builder_set_begin_timestamp(handle_.get(),
                                                             &timestamp) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline DatasetDeltaBuilder& DatasetDeltaBuilder::end_timestamp(const LanceDeltaTimestamp& timestamp) {
        if (lance_dataset_delta_builder_set_end_timestamp(handle_.get(),
                                                           &timestamp) != 0) {
            detail::throw_error();
        }
        return *this;
    }

inline DatasetDelta DatasetDeltaBuilder::build() {
        auto* raw = handle_.release();
        LanceDatasetDelta* delta = nullptr;
        if (lance_dataset_delta_builder_build(raw, &delta) != 0) {
            detail::throw_error();
        }
        return DatasetDelta(delta);
    }

inline BatchCommitResult::BatchCommitResult(Dataset&& dataset_value, Transaction&& transaction_value) : dataset(std::move(dataset_value)),
          transaction(std::move(transaction_value)) {}

}  // namespace lance
