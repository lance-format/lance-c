// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Fragment writer C API: write Arrow data to local fragment files without committing.
//!
//! # Workflow
//!
//! **Writer process (C/C++):**
//! ```c
//! const char* json = lance_write_fragments("file:///staging/robot.lance", &stream, NULL);
//! // store json to disk / send over socket
//! lance_free_string(json);
//! ```
//!
//! **Finalizer process (Rust):**
//! ```ignore
//! let frags: Vec<Fragment> = serde_json::from_str(json)?;
//! let txn = Transaction::new(0, Operation::Append { fragments: frags }, None);
//! CommitBuilder::new(uri).execute(txn).await?;
//! ```

use std::ffi::{CString, c_char};
use std::sync::Arc;

use arrow::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use lance::dataset::transaction::Operation;
use lance::dataset::{InsertBuilder, WriteParams};
use lance_core::Result;
use lance_io::object_store::{ObjectStoreParams, StorageOptionsAccessor};

use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::block_on;

/// Write an Arrow record batch stream to fragment files at `uri`.
///
/// The data is written but **not committed** — no dataset manifest is created
/// or updated. The returned JSON array describes the written fragments and can
/// be passed to the Lance Rust API to commit them:
///
/// ```ignore
/// let frags: Vec<lance_table::format::Fragment> = serde_json::from_str(json)?;
/// let txn = lance::dataset::transaction::Transaction::new(
///     0,
///     lance::dataset::transaction::Operation::Append { fragments: frags },
///     None,
/// );
/// lance::dataset::CommitBuilder::new(uri).execute(txn).await?;
/// ```
///
/// - `uri`: Directory URI where fragment files are written (`file://`, `s3://`, etc.)
/// - `stream`: Arrow C Data Interface stream consumed by this call. The caller must
///   not use the stream after this function returns.
/// - `storage_opts`: NULL-terminated key-value pairs `["key","val",NULL]`, or NULL.
///
/// Returns a JSON array string `[{...}, ...]`, one object per fragment written.
/// **Caller must free with `lance_free_string()`.**
/// Returns NULL on error — check `lance_last_error_code()` / `lance_last_error_message()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_write_fragments(
    uri: *const c_char,
    stream: *mut FFI_ArrowArrayStream,
    storage_opts: *const *const c_char,
) -> *const c_char {
    ffi_try!(
        unsafe { write_fragments_inner(uri, stream, storage_opts) },
        null
    )
}

unsafe fn write_fragments_inner(
    uri: *const c_char,
    stream: *mut FFI_ArrowArrayStream,
    storage_opts: *const *const c_char,
) -> Result<*const c_char> {
    if uri.is_null() || stream.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "uri and stream must not be NULL".into(),
            location: snafu::location!(),
        });
    }

    let uri_str = unsafe { helpers::parse_c_string(uri)? }.ok_or_else(|| {
        lance_core::Error::InvalidInput {
            source: "uri must not be empty".into(),
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

    let mut params = WriteParams::default();
    if !opts.is_empty() {
        params.store_params = Some(ObjectStoreParams {
            storage_options_accessor: Some(Arc::new(
                StorageOptionsAccessor::with_static_options(opts),
            )),
            ..ObjectStoreParams::default()
        });
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

    let json = serde_json::to_string(&fragments).map_err(|e| lance_core::Error::Internal {
        message: format!("failed to serialize fragments to JSON: {e}"),
        location: snafu::location!(),
    })?;

    let c_str = CString::new(json).map_err(|e| lance_core::Error::Internal {
        message: format!("fragment JSON contained interior null byte: {e}"),
        location: snafu::location!(),
    })?;

    Ok(c_str.into_raw())
}
