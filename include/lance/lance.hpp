/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file lance.hpp
 * @brief C++ RAII wrappers for the Lance C API.
 *
 * This umbrella keeps the existing include and header-only distribution.
 * Shared class declarations live in lance/detail/declarations.hpp; inline
 * definitions are grouped under lance/detail/inline/ and included here.
 */

#ifndef LANCE_HPP
#define LANCE_HPP

#include "lance/detail/declarations.hpp"
#include "lance/detail/inline/base.hpp"
#include "lance/detail/inline/values.hpp"
#include "lance/detail/inline/write.hpp"
#include "lance/detail/inline/transaction.hpp"
#include "lance/detail/inline/operation.hpp"
#include "lance/detail/inline/udf.hpp"
#include "lance/detail/inline/lock.hpp"
#include "lance/detail/inline/storage.hpp"
#include "lance/detail/inline/session.hpp"
#include "lance/detail/inline/search.hpp"
#include "lance/detail/inline/diagnostics.hpp"
#include "lance/detail/inline/blob.hpp"
#include "lance/detail/inline/fragment.hpp"
#include "lance/detail/inline/maintenance.hpp"
#include "lance/detail/inline/mem_wal.hpp"
#include "lance/detail/inline/dataset.hpp"
#include "lance/detail/inline/sql.hpp"
#include "lance/detail/inline/delta.hpp"
#include "lance/detail/inline/index.hpp"
#include "lance/detail/inline/scanner.hpp"
#include "lance/detail/inline/batch.hpp"
#include "lance/detail/inline/file.hpp"
#include "lance/detail/inline/namespace.hpp"

#endif /* LANCE_HPP */
