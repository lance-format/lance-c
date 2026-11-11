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

#[cfg(not(panic = "unwind"))]
compile_error!(
    "lance-c requires panic=\"unwind\" so its C ABI panic firewall can honor LANCE_ERR_PANIC"
);

mod add_columns;
mod alter_columns;
mod async_dispatcher;
mod base_paths;
mod batch;
mod batch_udf;
mod blob;
mod blob_read;
mod blob_writer;
mod callback;
mod cleanup;
mod commit;
mod compact;
mod config;
mod data_file;
mod data_statistics;
mod dataset;
mod delete;
mod diagnostics;
mod drop_columns;
mod error;
mod file_reader;
mod file_session;
mod file_writer;
mod files;
mod fragment;
mod fragment_writer;
mod fts_query;
mod helpers;
mod index;
mod index_model;
mod index_optimize;
mod index_segment;
mod merge;
mod merge_insert;
mod metadata;
mod multivector;
mod namespace;
mod restore;
pub mod runtime;
mod scalar_index_selection;
mod scalar_segment;
mod scanner;
mod schema;
mod session;
mod storage_options;
pub mod stream_guard;
mod transaction;
mod transaction_properties;
mod update;
mod update_map;
mod versions;
mod writer;

// Re-export all extern "C" symbols so they appear in the cdylib.
pub use add_columns::*;
pub use alter_columns::*;
pub use base_paths::*;
pub use batch::*;
pub use batch_udf::*;
pub use blob::*;
pub use blob_read::*;
pub use blob_writer::*;
pub use callback::*;
pub use cleanup::*;
pub use commit::*;
pub use compact::*;
pub use config::*;
pub use data_file::*;
pub use data_statistics::*;
pub use dataset::*;
pub use delete::*;
pub use diagnostics::*;
pub use drop_columns::*;
pub use error::{
    LanceErrorCode, lance_free_string, lance_last_error_code, lance_last_error_message,
};
pub use file_reader::*;
pub use file_session::*;
pub use file_writer::*;
pub use files::*;
pub use fragment::*;
pub use fragment_writer::*;
pub use fts_query::*;
pub use index::*;
pub use index_model::*;
pub use index_optimize::*;
pub use index_segment::*;
pub use merge::*;
pub use merge_insert::*;
pub use metadata::*;
pub use namespace::*;
pub use restore::*;
pub use scanner::*;
pub use schema::*;
pub use session::*;
pub use storage_options::*;
pub use transaction::*;
pub use transaction_properties::*;
pub use update::*;
pub use update_map::*;
pub use versions::*;
pub use writer::*;
