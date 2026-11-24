/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the session domain.
#pragma once

#include "lance/detail/declarations.hpp"
#include "lance/detail/inline/base.hpp"

namespace lance {

inline SessionCacheSpec SessionCacheSpec::default_cache() { return {}; }

inline SessionCacheSpec SessionCacheSpec::with_size(uint64_t capacity_bytes) {
        SessionCacheSpec spec;
        spec.kind = SessionCacheSpecKind::Size;
        spec.size_bytes = capacity_bytes;
        return spec;
    }

inline SessionCacheSpec SessionCacheSpec::from_uri(std::string uri) {
        SessionCacheSpec spec;
        spec.kind = SessionCacheSpecKind::Uri;
        spec.backend_uri = std::move(uri);
        return spec;
    }

inline SessionCacheSpec SessionCacheSpec::from_config(
        std::string backend_kind,
        std::vector<std::pair<std::string, std::string>> backend_options) {
        SessionCacheSpec spec;
        spec.kind = SessionCacheSpecKind::Config;
        spec.backend_kind = std::move(backend_kind);
        spec.backend_options = std::move(backend_options);
        return spec;
    }

inline LanceSessionCacheSpec Session::c_cache_spec(
        const SessionCacheSpec& spec,
        std::vector<const char*>& option_keys,
        std::vector<const char*>& option_values) {
        LanceSessionCacheSpec c_spec{};
        c_spec.kind = static_cast<int32_t>(spec.kind);
        switch (spec.kind) {
            case SessionCacheSpecKind::Default:
                break;
            case SessionCacheSpecKind::Size:
                c_spec.size_bytes = spec.size_bytes;
                break;
            case SessionCacheSpecKind::Uri:
                c_spec.backend_uri = spec.backend_uri.c_str();
                break;
            case SessionCacheSpecKind::Config:
                c_spec.backend_kind = spec.backend_kind.c_str();
                option_keys.reserve(spec.backend_options.size());
                option_values.reserve(spec.backend_options.size());
                for (const auto& [key, value] : spec.backend_options) {
                    option_keys.push_back(key.c_str());
                    option_values.push_back(value.c_str());
                }
                if (!option_keys.empty()) {
                    c_spec.option_keys = option_keys.data();
                    c_spec.option_values = option_values.data();
                    c_spec.option_count = option_keys.size();
                }
                break;
        }
        return c_spec;
    }

inline LanceSession* Session::create(
        const SessionCacheSpec& index_cache,
        const SessionCacheSpec& metadata_cache) {
        std::vector<const char*> index_keys;
        std::vector<const char*> index_values;
        std::vector<const char*> metadata_keys;
        std::vector<const char*> metadata_values;
        auto c_index =
            c_cache_spec(index_cache, index_keys, index_values);
        auto c_metadata =
            c_cache_spec(metadata_cache, metadata_keys, metadata_values);
        return lance_session_new_with_cache_specs(&c_index, &c_metadata);
    }

inline Session::Session(
        const SessionCacheSpec& index_cache,
        const SessionCacheSpec& metadata_cache) : handle_(create(index_cache, metadata_cache)) {
        if (!handle_) detail::throw_error();
    }

inline Session::Session(uint64_t index_cache_size_bytes, uint64_t metadata_cache_size_bytes) : Session(
              SessionCacheSpec::with_size(index_cache_size_bytes),
              SessionCacheSpec::with_size(metadata_cache_size_bytes)) {}

inline LanceSessionCacheStats Session::cache_stats() const {
        LanceSessionCacheStats stats{};
        if (lance_session_get_cache_stats(handle_.get(), &stats) != 0)
            detail::throw_error();
        return stats;
    }

inline uint64_t Session::size_bytes() const {
        uint64_t size = 0;
        if (lance_session_get_size_bytes(handle_.get(), &size) != 0)
            detail::throw_error();
        return size;
    }

inline uint64_t Session::approx_num_items() const {
        uint64_t count = 0;
        if (lance_session_get_approx_num_items(handle_.get(), &count) != 0)
            detail::throw_error();
        return count;
    }

inline const LanceSession* Session::c_handle() const { return handle_.get(); }

}  // namespace lance
