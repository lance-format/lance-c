// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Native `Dataset::merge` C API.
//!
//! This is a one-shot left join that appends precomputed RHS columns. It is
//! separate from merge-insert: native owns key matching, duplicate and NULL
//! semantics, schema union, fragment rewrites, and commit behavior.

use std::ffi::c_char;

use arrow::ffi_stream::FFI_ArrowArrayStream;
use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::block_on;

/// Left-join precomputed columns from an Arrow stream into a dataset.
///
/// Both key pointers are required NUL-terminated UTF-8 strings. Empty strings
/// are forwarded to native validation. The non-NULL source stream transfers
/// ownership to this call and is released on every return path, including
/// invalid dataset/key arguments and native failures.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_merge(
    dataset: *mut LanceDataset,
    left_on: *const c_char,
    right_on: *const c_char,
    source: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe { merge_inner(dataset, left_on, right_on, source) },
        neg
    )
}

unsafe fn merge_inner(
    dataset: *mut LanceDataset,
    left_on: *const c_char,
    right_on: *const c_char,
    source: *mut FFI_ArrowArrayStream,
) -> Result<i32> {
    // Ownership transfers before any other validation. Every later error drops
    // `reader`, which invokes the stream's release callback exactly once.
    let reader = unsafe { helpers::take_arrow_array_stream(source)? };

    let left_on = unsafe { helpers::parse_c_string(left_on)? }
        .ok_or_else(|| lance_core::Error::invalid_input_source("left_on must not be NULL".into()))?
        .to_owned();
    let right_on = unsafe { helpers::parse_c_string(right_on)? }
        .ok_or_else(|| lance_core::Error::invalid_input_source("right_on must not be NULL".into()))?
        .to_owned();

    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }

    // SAFETY: `dataset` is non-NULL and points to a live LanceDataset. The
    // write lock remains held while native reads the source and applies the
    // commit, so this handle cannot be mutated concurrently by another thread.
    let dataset = unsafe { &*dataset };
    dataset.with_mut(|dataset| block_on(dataset.merge(reader, &left_on, &right_on)))?;
    Ok(0)
}
