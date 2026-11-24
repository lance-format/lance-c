/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the storage domain.
#pragma once

#include "lance/detail/declarations.hpp"
#include "lance/detail/inline/base.hpp"
#include "lance/detail/inline/udf.hpp"

namespace lance {

inline StorageOptionsProvider::StorageOptionsProvider(LanceStorageOptionsProvider* provider) : handle_(provider) {
        if (!handle_) detail::throw_error();
    }

inline const LanceStorageOptionsProvider* StorageOptionsProvider::c_handle() const { return handle_.get(); }

inline std::string StorageOptionsProvider::provider_id() const {
        char* value = lance_storage_options_provider_id(handle_.get());
        if (value == nullptr) detail::throw_error();
        return detail::take_string(value);
    }

namespace storage_options_detail {

inline void release_fetch_snapshot(void* raw) noexcept {
    delete static_cast<FetchSnapshot*>(raw);
}

}

namespace storage_options_detail {

inline int32_t fetch_impl(
    const StorageOptionsProvider::Fetch& fetch,
    LanceStorageOptionsFetchResult* out,
    LanceCallbackError* error,
    const char* context) noexcept {
    (void)context;
    try {
        auto options = fetch();
        if (!options) {
            out->has_options = 0;
            return 0;
        }

        std::unique_ptr<FetchSnapshot> snapshot(new FetchSnapshot());
        snapshot->strings.reserve(options->size() * 2);
        snapshot->keys.reserve(options->size());
        snapshot->values.reserve(options->size());
        for (const auto& [key, value] : *options) {
            snapshot->strings.push_back(key);
            snapshot->strings.push_back(value);
        }
        for (std::size_t index = 0; index < options->size(); index++) {
            snapshot->keys.push_back(snapshot->strings[index * 2].c_str());
            snapshot->values.push_back(snapshot->strings[index * 2 + 1].c_str());
        }
        out->has_options = 1;
        out->keys = snapshot->keys.data();
        out->values = snapshot->values.data();
        out->count = snapshot->keys.size();
        out->snapshot_ctx = snapshot.release();
        out->release_snapshot = &release_fetch_snapshot;
        return 0;
    } catch (const lance::Error& exception) {
        callback_detail::set_callback_error(
            error, static_cast<int32_t>(exception.code), exception.what());
        return -1;
    } catch (const std::exception& exception) {
        callback_detail::set_callback_error(error, LANCE_ERR_INTERNAL, exception.what());
        return -1;
    } catch (...) {
        callback_detail::set_callback_error(
            error, LANCE_ERR_INTERNAL, "unknown C++ exception in storage options fetch");
        return -1;
    }
}

}

namespace storage_options_detail {

inline int32_t fetch_callback(
    void* callback_ctx,
    LanceStorageOptionsFetchResult* out,
    LanceCallbackError* error) noexcept {
    auto* context = static_cast<ProviderContext*>(callback_ctx);
    return fetch_impl(context->fetch, out, error, "storage options fetch");
}

}

namespace storage_options_detail {

inline int32_t force_fetch_callback(
    void* callback_ctx,
    LanceStorageOptionsFetchResult* out,
    LanceCallbackError* error) noexcept {
    auto* context = static_cast<ProviderContext*>(callback_ctx);
    return fetch_impl(
        context->force_fetch, out, error, "storage options force fetch");
}

}

namespace storage_options_detail {

inline void provider_release(void* callback_ctx) noexcept {
    delete static_cast<ProviderContext*>(callback_ctx);
}

}

namespace storage_options_detail {

inline LanceStorageOptionsProvider* create_provider(
    const std::string& provider_id,
    StorageOptionsProvider::Fetch fetch,
    StorageOptionsProvider::Fetch force_fetch) {
    auto* context = new ProviderContext{
        std::move(fetch), std::move(force_fetch)};
    LanceStorageOptionsProviderSpec spec{};
    spec.fetch = &fetch_callback;
    spec.force_fetch =
        context->force_fetch ? &force_fetch_callback : nullptr;
    spec.release = &provider_release;
    spec.provider_ctx = context;
    spec.provider_id = provider_id.c_str();
    auto* provider = lance_storage_options_provider_new(&spec);
    if (provider == nullptr) {
        // Ownership transfers only on success.
        delete context;
    }
    return provider;
}

}

inline StorageOptionsProvider::StorageOptionsProvider(
    std::string provider_id,
    Fetch fetch,
    Fetch force_fetch)
    : handle_(storage_options_detail::create_provider(
          provider_id, std::move(fetch), std::move(force_fetch))) {
    if (!handle_) detail::throw_error();
}

inline StorageOptionsAccessor::StorageOptionsAccessor(
        std::optional<StorageOptions> initial,
        const StorageOptionsProvider* provider) : handle_(nullptr) {
        std::vector<const char*> keys;
        std::vector<const char*> values;
        LanceStorageOptionsInput input{};
        const LanceStorageOptionsInput* input_ptr = nullptr;
        if (initial) {
            keys.reserve(initial->size());
            values.reserve(initial->size());
            for (const auto& [key, value] : *initial) {
                keys.push_back(key.c_str());
                values.push_back(value.c_str());
            }
            input.keys = keys.data();
            input.values = values.data();
            input.count = keys.size();
            input_ptr = &input;
        }
        auto* raw = lance_storage_options_accessor_new(
            input_ptr, provider ? provider->c_handle() : nullptr);
        if (raw == nullptr) detail::throw_error();
        handle_ = Handle<LanceStorageOptionsAccessor,
                         lance_storage_options_accessor_close>(raw);
    }

inline std::optional<StorageOptions> StorageOptionsAccessor::snapshot() const {
        auto* raw = lance_storage_options_accessor_snapshot(handle_.get());
        if (raw == nullptr) detail::throw_error();
        Handle<LanceStorageOptionsSnapshot,
               lance_storage_options_snapshot_close>
            snapshot(raw);
        int32_t has_options =
            lance_storage_options_snapshot_has_options(snapshot.get());
        if (has_options < 0) detail::throw_error();
        if (has_options == 0) return std::nullopt;
        StorageOptions options;
        uint64_t count =
            lance_storage_options_snapshot_count(snapshot.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        for (uint64_t index = 0; index < count; index++) {
            const char* key = lance_storage_options_snapshot_key_at(
                snapshot.get(), static_cast<std::size_t>(index));
            if (!key) detail::throw_error();
            const char* value = lance_storage_options_snapshot_value_at(
                snapshot.get(), static_cast<std::size_t>(index));
            if (!value) detail::throw_error();
            options.emplace(key, value);
        }
        return options;
    }

inline const LanceStorageOptionsAccessor* StorageOptionsAccessor::c_handle() const { return handle_.get(); }

inline StorageOptionsProvider StorageOptionsProvider::from_namespace(
    const Namespace& ns,
    const std::vector<std::string>& table_id) {
    auto table_json = namespace_detail::json_string_array(table_id);
    auto* raw = lance_namespace_storage_options_provider_new(
        ns.c_handle(), table_json.c_str());
    return StorageOptionsProvider(raw);
}

}  // namespace lance
