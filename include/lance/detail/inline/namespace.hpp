/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the namespace domain.
#pragma once

#include "lance/detail/declarations.hpp"

namespace lance {

namespace namespace_detail {

inline std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (char ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

}

namespace namespace_detail {

inline std::string json_string(std::string_view value) {
    return std::string("\"") + json_escape(value) + "\"";
}

}

namespace namespace_detail {

inline std::string json_string_array(const std::vector<std::string>& values) {
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out += ",";
        out += json_string(values[i]);
    }
    out += "]";
    return out;
}

}

namespace namespace_detail {

inline std::string json_string_object(
    const std::vector<std::pair<std::string, std::string>>& entries) {
    std::string out = "{";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i != 0) out += ",";
        out += json_string(entries[i].first);
        out += ":";
        out += json_string(entries[i].second);
    }
    out += "}";
    return out;
}

}

namespace namespace_detail {

inline void free_owned_string(LanceOwnedString* value) noexcept {
    delete[] value->data;
    value->data = nullptr;
    value->release = nullptr;
}

}

namespace namespace_detail {

inline void set_owned_string(LanceOwnedString* out, std::string value) {
    auto* data = new char[value.size() + 1];
    std::memcpy(data, value.data(), value.size());
    data[value.size()] = '\0';
    out->data = data;
    out->release = &free_owned_string;
}

}

namespace namespace_detail {

inline void set_callback_error(
    LanceCallbackError* error, int32_t code, const std::string& message) noexcept {
    error->code = code;
    try {
        set_owned_string(&error->message, message);
    } catch (...) {
        error->message.data = nullptr;
        error->message.release = nullptr;
    }
}

}

inline NamespaceError::NamespaceError(
        LanceErrorCode code_value,
        std::string message_value,
        uint32_t native_code_value,
        std::string native_message_value) : Error(code_value, std::move(message_value)),
          has_native_code(true),
          native_code(native_code_value),
          native_message(std::move(native_message_value)) {}

inline NamespaceResult::NamespaceResult(std::optional<std::string> json, std::vector<uint8_t> bytes) : json_(std::move(json)), bytes_(std::move(bytes)) {}

inline bool NamespaceResult::has_json() const noexcept { return json_.has_value(); }

inline const std::optional<std::string>& NamespaceResult::json() const noexcept { return json_; }

inline const std::vector<uint8_t>& NamespaceResult::bytes() const noexcept { return bytes_; }

inline DynamicContextProvider::DynamicContextProvider(Callback callback) : callback_(std::move(callback)) {}

inline const DynamicContextProvider::Callback& DynamicContextProvider::callback() const noexcept { return callback_; }

inline void Namespace::context_provider_release(void* ctx) noexcept {
        delete static_cast<ContextProviderContext*>(ctx);
    }

inline void Namespace::custom_client_release(void* ctx) noexcept {
        delete static_cast<CustomClientContext*>(ctx);
    }

inline void Namespace::custom_dispatch_release(void* ctx) noexcept {
        delete static_cast<CustomDispatchContext*>(ctx);
    }

inline int32_t Namespace::context_provider_call(
        void* raw_ctx,
        const char* operation,
        const char* object_id,
        LanceOwnedString* out_json,
        LanceCallbackError* out_error) noexcept {
        auto* ctx = static_cast<ContextProviderContext*>(raw_ctx);
        try {
            std::map<std::string, std::string> context = ctx->callback(
                operation ? operation : "", object_id ? object_id : "");
            std::string json = "{";
            bool first = true;
            for (const auto& [key, value] : context) {
                if (!first) json += ",";
                first = false;
                json += namespace_detail::json_string(key);
                json += ":";
                json += namespace_detail::json_string(value);
            }
            json += "}";
            namespace_detail::set_owned_string(out_json, std::move(json));
            return 0;
        } catch (const lance::Error& error) {
            namespace_detail::set_callback_error(
                out_error, static_cast<int32_t>(error.code), error.what());
        } catch (const std::exception& error) {
            namespace_detail::set_callback_error(
                out_error, static_cast<int32_t>(LANCE_ERR_INTERNAL), error.what());
        } catch (...) {
            namespace_detail::set_callback_error(
                out_error,
                static_cast<int32_t>(LANCE_ERR_INTERNAL),
                "unknown C++ exception in namespace context provider");
        }
        return 1;
    }

inline int32_t Namespace::custom_describe_table(
        void* ctx, const char* req, LanceOwnedString* out, LanceCallbackError* err) noexcept {
        auto* client = static_cast<CustomClientContext*>(ctx);
        return invoke_custom([client](const char* request) {
            return client->describe_table(request);
        }, req, out, err);
    }

inline int32_t Namespace::custom_describe_table_version(
        void* ctx, const char* req, LanceOwnedString* out, LanceCallbackError* err) noexcept {
        auto* client = static_cast<CustomClientContext*>(ctx);
        return invoke_custom([client](const char* request) {
            return client->describe_table_version(request);
        }, req, out, err);
    }

inline int32_t Namespace::custom_create_table_version(
        void* ctx, const char* req, LanceOwnedString* out, LanceCallbackError* err) noexcept {
        auto* client = static_cast<CustomClientContext*>(ctx);
        return invoke_custom([client](const char* request) {
            return client->create_table_version(request);
        }, req, out, err);
    }

inline int32_t Namespace::custom_list_table_versions(
        void* ctx, const char* req, LanceOwnedString* out, LanceCallbackError* err) noexcept {
        auto* client = static_cast<CustomClientContext*>(ctx);
        return invoke_custom([client](const char* request) {
            return client->list_table_versions(request);
        }, req, out, err);
    }

inline int32_t Namespace::custom_dispatch_call(
        void* ctx,
        const char* operation,
        const char* req,
        LanceOwnedString* out,
        LanceCallbackError* err) noexcept {
        auto* client = static_cast<CustomDispatchContext*>(ctx);
        return invoke_custom([client, operation](const char* request) {
            return client->dispatch(operation ? operation : "", request);
        }, req, out, err);
    }

inline LanceNamespaceContextProviderSpec Namespace::make_context_spec(
        const DynamicContextProvider* provider) {
        LanceNamespaceContextProviderSpec spec{};
        if (provider) {
            auto* ctx = new ContextProviderContext{provider->callback()};
            spec.provide_context = &context_provider_call;
            spec.release = &context_provider_release;
            spec.provider_ctx = ctx;
        }
        return spec;
    }

inline Namespace::Namespace(LanceNamespace* handle) : handle_(handle) {}

inline Namespace Namespace::directory(
        const std::vector<std::pair<std::string, std::string>>& properties,
        const Session* session,
        const DynamicContextProvider* context_provider) {
        auto json = namespace_detail::json_string_object(properties);
        auto spec = make_context_spec(context_provider);
        auto* spec_ptr = context_provider ? &spec : nullptr;
        auto* raw = lance_namespace_directory_new(
            json.c_str(), session ? session->c_handle() : nullptr, spec_ptr);
        if (!raw) detail::throw_error();
        return Namespace(raw);
    }

inline Namespace Namespace::rest(
        const std::vector<std::pair<std::string, std::string>>& properties,
        const DynamicContextProvider* context_provider) {
        auto json = namespace_detail::json_string_object(properties);
        auto spec = make_context_spec(context_provider);
        auto* spec_ptr = context_provider ? &spec : nullptr;
        auto* raw = lance_namespace_rest_new(json.c_str(), spec_ptr);
        if (!raw) detail::throw_error();
        return Namespace(raw);
    }

inline Namespace Namespace::connect(
        const std::string& impl_name,
        const std::vector<std::pair<std::string, std::string>>& properties,
        const Session* session,
        const DynamicContextProvider* context_provider) {
        auto json = namespace_detail::json_string_object(properties);
        auto spec = make_context_spec(context_provider);
        auto* spec_ptr = context_provider ? &spec : nullptr;
        auto* raw = lance_namespace_connect_new(
            impl_name.c_str(),
            json.c_str(),
            session ? session->c_handle() : nullptr,
            spec_ptr);
        if (!raw) detail::throw_error();
        return Namespace(raw);
    }

inline Namespace Namespace::custom(const CustomClientSpec& spec) {
        if (!spec.describe_table || !spec.describe_table_version ||
            !spec.create_table_version || !spec.list_table_versions) {
            throw Error(
                LANCE_ERR_INVALID_ARGUMENT,
                "custom namespace requires all four managed-version callbacks");
        }
        auto* ctx = new CustomClientContext{
            spec.describe_table,
            spec.describe_table_version,
            spec.create_table_version,
            spec.list_table_versions};
        std::unique_ptr<CustomClientContext> guard(ctx);
        LanceNamespaceCustomClientSpec c_spec{};
        c_spec.describe_table = &custom_describe_table;
        c_spec.describe_table_version = &custom_describe_table_version;
        c_spec.create_table_version = &custom_create_table_version;
        c_spec.list_table_versions = &custom_list_table_versions;
        c_spec.release = &custom_client_release;
        c_spec.client_ctx = ctx;
        c_spec.namespace_id = spec.namespace_id.c_str();
        auto* raw = lance_namespace_custom_new(&c_spec);
        // The C constructor consumes the context on every invocation.
        guard.release();
        if (!raw) detail::throw_error();
        return Namespace(raw);
    }

inline Namespace Namespace::custom_dispatch(const CustomDispatchSpec& spec) {
        if (!spec.dispatch) {
            throw Error(
                LANCE_ERR_INVALID_ARGUMENT,
                "custom dispatch namespace requires a dispatch callback");
        }
        auto* ctx = new CustomDispatchContext{spec.dispatch};
        std::unique_ptr<CustomDispatchContext> guard(ctx);
        LanceNamespaceCustomClientDispatchSpec c_spec{};
        c_spec.dispatch = &custom_dispatch_call;
        c_spec.release = &custom_dispatch_release;
        c_spec.client_ctx = ctx;
        c_spec.namespace_id = spec.namespace_id.c_str();
        auto* raw = lance_namespace_custom_dispatch_new(&c_spec);
        // The C constructor consumes the context on every invocation.
        guard.release();
        if (!raw) detail::throw_error();
        return Namespace(raw);
    }

inline std::string Namespace::id() const {
        const char* raw = lance_namespace_id(handle_.get());
        return detail::take_string(raw);
    }

inline std::string Namespace::ops_metrics_json() const {
        const char* raw = lance_namespace_retrieve_ops_metrics(handle_.get());
        return detail::take_string(raw);
    }

inline void Namespace::reset_ops_metrics() {
        if (lance_namespace_reset_ops_metrics(handle_.get()) != 0) detail::throw_error();
    }

inline NamespaceResult Namespace::call(
        const std::string& operation,
        const std::string& request_json,
        const uint8_t* request_bytes,
        size_t request_bytes_len) const {
        LanceNamespaceResult raw{};
        struct ResultGuard {
            LanceNamespaceResult* result;
            ~ResultGuard() { if (result) lance_namespace_result_close(result); }
        } guard{&raw};
        int32_t rc = lance_namespace_call(
            handle_.get(),
            operation.c_str(),
            request_json.c_str(),
            request_bytes,
            request_bytes_len,
            &raw);
        std::optional<std::string> json;
        std::vector<uint8_t> bytes;
        if (raw.result_json) json = std::string(raw.result_json);
        if (raw.result_bytes && raw.result_bytes_len != 0) {
            bytes.assign(raw.result_bytes, raw.result_bytes + raw.result_bytes_len);
        }
        bool has_native = raw.has_namespace_error != 0;
        uint32_t native_code = raw.namespace_error_code;
        std::string native_message =
            raw.namespace_error_message ? raw.namespace_error_message : "";
        lance_namespace_result_close(&raw);
        guard.result = nullptr;
        if (rc != 0) {
            LanceErrorCode code = lance_last_error_code();
            detail::OwnedString message(lance_last_error_message());
            std::string owned(message ? message.get() : "Unknown error");
            if (has_native) {
                throw NamespaceError(
                    code, std::move(owned), native_code, std::move(native_message));
            }
            throw Error(code, std::move(owned));
        }
        return NamespaceResult(std::move(json), std::move(bytes));
    }

inline NamespaceResult Namespace::call(
        const std::string& operation,
        const std::string& request_json,
        const std::vector<uint8_t>& request_bytes) const {
        return call(
            operation,
            request_json,
            request_bytes.empty() ? nullptr : request_bytes.data(),
            request_bytes.size());
    }

inline const LanceNamespace* Namespace::c_handle() const noexcept { return handle_.get(); }

inline RestAdapter::RestAdapter(LanceRestAdapter* raw) : handle_(raw) {}

inline RestAdapter::RestAdapter(RestAdapter&& other) noexcept : handle_(std::move(other.handle_)), port_(other.port_) {
        other.port_ = 0;
    }

inline RestAdapter RestAdapter::start(
        const Namespace& backend,
        const std::string& host,
        uint16_t port) {
        LanceRestAdapter* raw = nullptr;
        if (lance_rest_adapter_start(
                backend.c_handle(), host.c_str(), port, &raw) != 0) {
            detail::throw_error();
        }
        if (!raw) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_rest_adapter_start returned success with null adapter");
        }
        RestAdapter adapter(raw);
        adapter.port_ = lance_rest_adapter_port(raw);
        return adapter;
    }

inline uint16_t RestAdapter::port() const noexcept { return port_; }

inline void RestAdapter::shutdown() {
        lance_rest_adapter_shutdown(handle_.get());
        port_ = 0;
    }

}  // namespace lance
