// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include <lance/detail/declarations.hpp>

// The declaration-only path must preserve the same [[noreturn]] contract.
int fail_without_return() {
    lance::detail::throw_error();
}
