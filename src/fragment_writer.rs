// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Fragment writer C API: write Arrow data to local fragment files without committing.
//!
//! Designed for embedded / robotics C++ pipelines where sensor data is ingested
//! at high frequency on edge devices. The C++ process writes Lance fragment files
//! locally with minimal overhead (no manifest, no coordination). A separate Rust
//! finalizer process later reads the file footers, reconstructs fragment metadata,
//! and commits them into a dataset on a remote data lake (S3, GCS, etc.).
//!
//! # Two-process workflow
//!
//! **1. Writer process (C/C++ on edge device):**
//! ```c
//! // Stream sensor batches into local fragment files.
//! int32_t rc = lance_write_fragments(
//!     "file:///data/staging/robot.lance", &schema, &stream, NULL);
//! ```
//!
//! **2. Finalizer process (Rust, runs periodically or on sync):**
//! ```ignore
//! // Scan data/*.lance files, reconstruct Fragment metadata from file footers,
//! // then commit via CommitBuilder to publish to the data lake.
//! ```

use std::ffi::c_char;
use std::sync::Arc;

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use arrow::record_batch::RecordBatchReader;
use arrow_schema::Schema as ArrowSchema;
use lance::dataset::{InsertBuilder, WriteParams};
use lance_core::Result;
use lance_io::object_store::{ObjectStoreParams, StorageOptionsAccessor};

use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::block_on;

/// Write an Arrow record batch stream to fragment files at `uri`.
///
/// The data is written but **not committed** — no dataset manifest is created
/// or updated. The written `.lance` files under `<uri>/data/` contain full
/// metadata in their footers (schema with field IDs, row counts, format version).
/// A Rust finalizer can reconstruct `Fragment` metadata by reading these footers
/// and commit via `CommitBuilder`.
///
/// - `uri`: Directory URI where fragment files are written (`file://`, `s3://`, etc.)
/// - `schema`: Required Arrow schema. The stream's schema must match; the call
///   fails fast with `LANCE_ERR_INVALID_ARGUMENT` on mismatch.
/// - `stream`: Arrow C Data Interface stream consumed by this call. The caller
///   must not use the stream after this function returns.
/// - `storage_opts`: NULL-terminated key-value pairs `["key","val",NULL]`, or NULL.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_write_fragments(
    uri: *const c_char,
    schema: *const FFI_ArrowSchema,
    stream: *mut FFI_ArrowArrayStream,
    storage_opts: *const *const c_char,
) -> i32 {
    ffi_try!(
        unsafe { write_fragments_inner(uri, schema, stream, storage_opts) },
        neg
    )
}

unsafe fn write_fragments_inner(
    uri: *const c_char,
    schema: *const FFI_ArrowSchema,
    stream: *mut FFI_ArrowArrayStream,
    storage_opts: *const *const c_char,
) -> Result<i32> {
    if uri.is_null() || schema.is_null() || stream.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "uri, schema, and stream must not be NULL".into(),
            location: snafu::location!(),
        });
    }

    let uri_str = unsafe { helpers::parse_c_string(uri)? }.ok_or_else(|| {
        lance_core::Error::InvalidInput {
            source: "uri must not be empty".into(),
            location: snafu::location!(),
        }
    })?;

    // Import the caller-provided schema from the Arrow C Data Interface.
    let expected_schema = ArrowSchema::try_from(unsafe { &*schema }).map_err(|e| {
        lance_core::Error::InvalidInput {
            source: format!("invalid schema: {e}").into(),
            location: snafu::location!(),
        }
    })?;

    let opts = unsafe { helpers::parse_storage_options(storage_opts)? };

    // Consume the C stream into an Arrow RecordBatch reader.
    let reader = unsafe { ArrowArrayStreamReader::from_raw(stream) }.map_err(|e| {
        lance_core::Error::InvalidInput {
            source: e.to_string().into(),
            location: snafu::location!(),
        }
    })?;

    // Fail fast: compare the stream schema against the caller-provided schema.
    let stream_schema = reader.schema();
    if stream_schema.fields() != expected_schema.fields() {
        return Err(lance_core::Error::InvalidInput {
            source: format!(
                "stream schema does not match the provided schema.\n  expected: {expected_schema}\n  got:      {stream_schema}"
            )
            .into(),
            location: snafu::location!(),
        });
    }

    let mut params = WriteParams::default();
    if !opts.is_empty() {
        params.store_params = Some(ObjectStoreParams {
            storage_options_accessor: Some(Arc::new(StorageOptionsAccessor::with_static_options(
                opts,
            ))),
            ..ObjectStoreParams::default()
        });
    }

    // Write fragment data files. The Transaction result is discarded —
    // the finalizer reconstructs Fragment metadata from the file footers.
    let _transaction = block_on(
        InsertBuilder::new(uri_str)
            .with_params(&params)
            .execute_uncommitted_stream(reader),
    )?;

    Ok(0)
}
