/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for a dataset read-only consumer.
//
// Include <lance/detail/declarations.hpp> first (or use this header's
// declarations include) and then include this header to compile and link the
// open/scan read path without parsing the remaining inline definitions.
#pragma once

#include "lance/detail/declarations.hpp"
#include "lance/detail/inline/base.hpp"
#include "lance/detail/inline/dataset.hpp"
#include "lance/detail/inline/scanner.hpp"
