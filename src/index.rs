// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Index lifecycle C API: create vector / scalar indexes, drop, list.
//!
//! Index creation mutates the dataset under an exclusive write lock; existing
//! scanners that already cloned the inner Arc keep their snapshot view.

use std::ffi::{CString, c_char};

use lance_core::Result;
use lance_index::scalar::{BuiltinIndexType, ScalarIndexParams};
use lance_index::{DatasetIndexExt, IndexType};

use crate::dataset::LanceDataset;
use crate::error::{LanceErrorCode, ffi_try, set_last_error};
use crate::helpers;
use crate::runtime::block_on;
