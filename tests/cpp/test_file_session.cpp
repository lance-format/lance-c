/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

#include "lance/lance.hpp"

lance::FileSession open_file_test_session(
    const std::string& base_uri,
    const lance::StorageOptionsAccessor& storage) {
    return lance::FileSession(base_uri, &storage);
}
