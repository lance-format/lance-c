// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! C/C++ bindings for the Lance columnar data format.
//!
//! This crate exposes Lance's functionality through a stable C-ABI with
//! opaque handle patterns and Arrow C Data Interface for zero-copy data exchange.
//!
//! # Safety
//!
//! All `extern "C"` functions in this crate follow the C FFI safety contract:
//! - Pointers must be valid and non-null (unless documented as nullable).
//! - Opaque handles must have been created by the corresponding `lance_*_open`
//!   or `lance_*_new` function and must not be used after `lance_*_close`/`lance_*_free`.
//! - The caller is responsible for freeing returned strings with `lance_free_string()`.
#![allow(clippy::missing_safety_doc)]

mod add_columns;
mod alter_columns;
mod async_dispatcher;
mod batch;
mod compact;
mod dataset;
mod delete;
mod drop_columns;
mod error;
mod fragment_writer;
mod helpers;
mod index;
mod merge_insert;
mod restore;
pub mod runtime;
mod scanner;
mod update;
mod versions;
mod writer;

// Re-export all extern "C" symbols so they appear in the cdylib.
pub use add_columns::*;
pub use alter_columns::*;
pub use batch::*;
pub use compact::*;
pub use dataset::*;
pub use delete::*;
pub use drop_columns::*;
pub use error::{
    LanceErrorCode, lance_free_string, lance_last_error_code, lance_last_error_message,
};
pub use fragment_writer::*;
pub use index::*;
pub use merge_insert::*;
pub use restore::*;
pub use scanner::*;
pub use update::*;
pub use versions::*;
pub use writer::*;
