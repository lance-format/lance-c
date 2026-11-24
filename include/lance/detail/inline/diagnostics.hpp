/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the diagnostics domain.
#pragma once

#include "lance/detail/declarations.hpp"

namespace lance {

inline FtsTokenList::FtsTokenList(LanceFtsTokenList* tokens) : handle_(tokens) {}

inline FtsTokenList FtsTokenList::tokenize(
        std::string_view text,
        std::optional<std::string> params_json) {
        auto* tokens = lance_fts_tokenize(
            text.data(), text.size(),
            params_json ? params_json->c_str() : nullptr);
        if (!tokens) detail::throw_error();
        return FtsTokenList(tokens);
    }

inline size_t FtsTokenList::size() const {
        uint64_t count = lance_fts_token_list_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return static_cast<size_t>(count);
    }

inline bool FtsTokenList::empty() const { return size() == 0; }

inline FtsToken FtsTokenList::at(size_t index) const {
        LanceFtsTokenView view{};
        if (lance_fts_token_list_get(handle_.get(), index, &view) != 0)
            detail::throw_error();
        if (view.text == nullptr && view.text_len != 0)
            throw std::runtime_error(
                "native token view has NULL text with a nonzero length");
        return FtsToken{
            std::string(view.text_len == 0 ? "" : view.text, view.text_len),
            view.position};
    }

inline std::vector<FtsToken> FtsTokenList::tokens() const {
        std::vector<FtsToken> out;
        size_t count = size();
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) out.push_back(at(i));
        return out;
    }

inline const LanceFtsTokenList* FtsTokenList::c_handle() const { return handle_.get(); }

inline uint64_t process_iops_counter() {
    uint64_t value = lance_process_iops_counter();
    if (lance_last_error_code() != LANCE_OK) detail::throw_error();
    return value;
}

inline uint64_t process_bytes_read_counter() {
    uint64_t value = lance_process_bytes_read_counter();
    if (lance_last_error_code() != LANCE_OK) detail::throw_error();
    return value;
}

inline ProcessIoCounters process_io_counters() {
    ProcessIoCounters counters;
    counters.iops = lance_process_iops_counter();
    if (lance_last_error_code() != LANCE_OK) detail::throw_error();
    counters.bytes_read = lance_process_bytes_read_counter();
    if (lance_last_error_code() != LANCE_OK) detail::throw_error();
    return counters;
}

inline std::string simd_info_json() {
    const char* raw = lance_simd_info_json();
    return detail::take_string(raw);
}

}  // namespace lance
