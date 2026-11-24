/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the lock domain.
#pragma once

#include "lance/detail/declarations.hpp"
#include "lance/detail/inline/base.hpp"
#include "lance/detail/inline/udf.hpp"

namespace lance {

inline const LanceCommitLock* CommitLock::c_handle() const { return handle_.get(); }

namespace commit_lock_detail {

extern "C" inline int32_t acquire_trampoline(
    void* callback_ctx,
    uint64_t version,
    void** out_lease,
    LanceCallbackError* error) noexcept {
    if (out_lease == nullptr) {
        callback_detail::set_callback_error(
            error, LANCE_ERR_INVALID_ARGUMENT, "out_lease must not be null");
        return -1;
    }
    *out_lease = nullptr;
    auto* state = static_cast<State*>(callback_ctx);
    try {
        auto lease = state->acquire(version);
        if (lease) {
            *out_lease = new LeaseHolder{std::move(lease)};
        }
        return 0;
    } catch (const lance::Error& exception) {
        callback_detail::set_callback_error(
            error, static_cast<int32_t>(exception.code), exception.what());
        return -1;
    } catch (const std::exception& exception) {
        callback_detail::set_callback_error(
            error, LANCE_ERR_INTERNAL, exception.what());
        return -1;
    } catch (...) {
        callback_detail::set_callback_error(
            error, LANCE_ERR_INTERNAL, "unknown C++ exception");
        return -1;
    }
}

}

namespace commit_lock_detail {

extern "C" inline int32_t release_trampoline(
    void*,
    void* lease,
    bool success,
    LanceCallbackError* error) noexcept {
    if (lease == nullptr) {
        return 0;
    }
    auto* holder = static_cast<LeaseHolder*>(lease);
    try {
        holder->lease->release(success);
        return 0;
    } catch (const lance::Error& exception) {
        callback_detail::set_callback_error(
            error, static_cast<int32_t>(exception.code), exception.what());
        return -1;
    } catch (const std::exception& exception) {
        callback_detail::set_callback_error(
            error, LANCE_ERR_INTERNAL, exception.what());
        return -1;
    } catch (...) {
        callback_detail::set_callback_error(
            error, LANCE_ERR_INTERNAL, "unknown C++ exception");
        return -1;
    }
}

}

namespace commit_lock_detail {

extern "C" inline void destroy_lease_trampoline(void*, void* lease) noexcept {
    delete static_cast<LeaseHolder*>(lease);
}

}

namespace commit_lock_detail {

extern "C" inline void release_context_trampoline(void* callback_ctx) noexcept {
    delete static_cast<State*>(callback_ctx);
}

}

inline CommitLock::CommitLock(Acquire acquire)
    : handle_(nullptr),
      state_(new commit_lock_detail::State{std::move(acquire)}) {
    LanceCommitLockCallbacks callbacks = {};
    callbacks.lock = &commit_lock_detail::acquire_trampoline;
    callbacks.release = &commit_lock_detail::release_trampoline;
    callbacks.destroy_lease = &commit_lock_detail::destroy_lease_trampoline;
    callbacks.callback_ctx = state_.get();
    callbacks.release_context = &commit_lock_detail::release_context_trampoline;

    auto* raw = lance_commit_lock_new(&callbacks);
    // On failure the library never takes context ownership, so `state_`
    // remains local and is destroyed here.
    if (raw == nullptr) detail::throw_error();
    handle_ = Handle<LanceCommitLock, lance_commit_lock_close>(raw);
    // On success the native context owns the state and releases it exactly
    // once when the last owner drops.
    (void)state_.release();
}

}  // namespace lance
