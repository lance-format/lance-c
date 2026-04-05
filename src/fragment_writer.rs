// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Fragment writer C API: write Arrow data to local fragment files without committing.
//!
//! # Workflow
//!
//! **Writer process (C/C++):**
//! ```c
//! int32_t rc = lance_write_fragments("file:///staging/robot.lance", &schema, &stream, NULL);
//! ```
//!
//! **Finalizer process (Rust):**
//! ```ignore
//! // Read sidecar metadata written by lance_write_fragments
//! let json = std::fs::read_to_string("staging/robot.lance/_fragments/xxx.json")?;
//! let frags: Vec<Fragment> = serde_json::from_str(&json)?;
//! let txn = Transaction::new(0, Operation::Append { fragments: frags }, None);
//! CommitBuilder::new(uri).execute(txn).await?;
//! ```

use std::ffi::c_char;
use std::sync::Arc;

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use arrow::record_batch::RecordBatchReader;
use arrow_schema::Schema as ArrowSchema;
use lance::dataset::transaction::Operation;
use lance::dataset::{InsertBuilder, WriteParams};
use lance_core::Result;
use lance_io::object_store::{ObjectStore, ObjectStoreParams, StorageOptionsAccessor};

use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::block_on;

/// Directory name for fragment metadata sidecar files.
const FRAGMENTS_META_DIR: &str = "_fragments";

/// Write an Arrow record batch stream to fragment files at `uri`.
///
/// The data is written but **not committed** — no dataset manifest is created
/// or updated. Fragment metadata is written as a JSON sidecar file under
/// `<uri>/_fragments/<uuid>.json`, which a Rust finalizer can read and commit.
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

    let store_params = if opts.is_empty() {
        None
    } else {
        Some(ObjectStoreParams {
            storage_options_accessor: Some(Arc::new(StorageOptionsAccessor::with_static_options(
                opts.clone(),
            ))),
            ..ObjectStoreParams::default()
        })
    };

    let mut params = WriteParams::default();
    if let Some(ref sp) = store_params {
        params.store_params = Some(sp.clone());
    }

    let transaction = block_on(
        InsertBuilder::new(uri_str)
            .with_params(&params)
            .execute_uncommitted_stream(reader),
    )?;

    let fragments = match transaction.operation {
        Operation::Append { fragments } => fragments,
        Operation::Overwrite { fragments, .. } => fragments,
        other => {
            return Err(lance_core::Error::Internal {
                message: format!("unexpected operation from write_fragments: {other}"),
                location: snafu::location!(),
            });
        }
    };

    // Serialize fragment metadata and write as a sidecar JSON file.
    let json =
        serde_json::to_string_pretty(&fragments).map_err(|e| lance_core::Error::Internal {
            message: format!("failed to serialize fragments to JSON: {e}"),
            location: snafu::location!(),
        })?;

    let (object_store, base_path) = block_on(ObjectStore::from_uri_and_params(
        Arc::new(Default::default()),
        uri_str,
        &store_params.unwrap_or_default(),
    ))?;

    let sidecar_filename = format!("{}.json", uuid::Uuid::new_v4());
    let sidecar_path = base_path
        .child(FRAGMENTS_META_DIR)
        .child(sidecar_filename.as_str());
    block_on(object_store.put(&sidecar_path, json.as_bytes()))?;

    Ok(0)
}
