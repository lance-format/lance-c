// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

#include <lance/lance.hpp>

// throw_error() must stay [[noreturn]] in the split wrapper; otherwise this
// function is diagnosed with -Werror=return-type.
int fail_without_return() {
    lance::detail::throw_error();
}
