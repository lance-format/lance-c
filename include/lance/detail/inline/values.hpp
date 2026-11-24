/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the values domain.
#pragma once

#include "lance/detail/declarations.hpp"

namespace lance {

inline Ref::Ref(LanceRef* reference) : handle_(reference) {}

inline Ref Ref::version_number(uint64_t version) {
        auto* raw = lance_ref_version_number(version);
        if (!raw) detail::throw_error();
        return Ref(raw);
    }

inline Ref Ref::version(
        std::optional<std::string> branch,
        std::optional<uint64_t> version_number) {
        auto* raw = lance_ref_version(
            branch ? branch->c_str() : nullptr,
            version_number.has_value(),
            version_number.value_or(0));
        if (!raw) detail::throw_error();
        return Ref(raw);
    }

inline Ref Ref::tag(const std::string& tag_name) {
        auto* raw = lance_ref_tag(tag_name.c_str());
        if (!raw) detail::throw_error();
        return Ref(raw);
    }

inline const LanceRef* Ref::c_handle() const { return handle_.get(); }

}  // namespace lance
