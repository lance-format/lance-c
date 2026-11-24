/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the base domain.
#pragma once

#include "lance/detail/declarations.hpp"

namespace lance {

inline std::string_view version() noexcept {
    return std::string_view(lance_c_version());
}

inline Error::Error(LanceErrorCode code_value, std::string message) : std::runtime_error(std::move(message)), code(code_value) {}

namespace detail {

inline void FreeString::operator() (const char* ptr) const noexcept { lance_free_string(ptr); }

}

namespace detail {

[[noreturn]] inline void throw_error() {
    const LanceErrorCode code = lance_last_error_code();
    OwnedString message(lance_last_error_message());
    if (code == LANCE_OK) {
        throw Error(
            LANCE_ERR_INTERNAL,
            "lance-c returned a failure without setting a thread-local error");
    }
    throw Error(code, message ? std::string(message.get()) : "Unknown error");
}

}

namespace detail {

inline std::string take_string(const char* raw) {
    if (!raw) throw_error();
    OwnedString guard(raw);
    return std::string(raw);
}

}

namespace detail {

inline void FreeBytes::operator() (uint8_t* ptr) const noexcept { lance_free_bytes(ptr); }

}

namespace detail {

inline std::vector<uint8_t> take_bytes(uint8_t* raw, size_t len) {
    if (!raw) {
        if (len != 0) throw_error();
        return {};
    }
    OwnedBytes guard(raw);
    return std::vector<uint8_t>(raw, raw + len);
}

}

inline void check_error() {
    if (lance_last_error_code() != LANCE_OK) detail::throw_error();
}

inline void init_logging(std::string_view filter) {
    if (filter.find('\0') != std::string_view::npos) {
        throw Error(LANCE_ERR_INVALID_ARGUMENT,
                    "logging filter must not contain NUL");
    }
    std::string owned(filter);
    if (lance_init_logging(owned.c_str()) != 0) detail::throw_error();
}

inline std::string build_rq_model(uint32_t dimension, uint32_t num_bits) {
    const char* json = lance_index_build_rq_model(dimension, num_bits);
    return detail::take_string(json);
}

inline void scanner_async_stream_free(ArrowArrayStream* stream) noexcept {
    lance_scanner_async_stream_free(stream);
}

inline ArrowStream::ArrowStream(ArrowArrayStream stream) noexcept : stream_(stream), active_(stream.release != nullptr) {}

inline ArrowStream::~ArrowStream() { reset(); }

inline ArrowStream::ArrowStream(ArrowStream&& other) noexcept : stream_(other.stream_), active_(other.active_) {
        other.stream_ = {};
        other.active_ = false;
    }

}  // namespace lance
