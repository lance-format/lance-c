// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Dataset SQL C API: native `dataset::sql` planning and streaming.
//!
//! The C builder mirrors `lance::dataset::sql::SqlQueryBuilder`; `build`
//! performs the native async planning and `into_stream` exposes the planned
//! query as a guarded Arrow C stream. No binding-side SQL engine, row
//! rewriting, filtering or whole-result materialization is introduced: the
//! system-column injection rules (including aggregates, joins and DISTINCT)
//! are entirely the native planner's.

use std::ffi::c_char;
use std::ptr;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;

use arrow::ffi_stream::FFI_ArrowArrayStream;
use lance::dataset::sql::{SqlQuery, SqlQueryBuilder};
use lance_core::Result;
use lance_core::datatypes::BlobHandling;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::runtime::{RT, block_on};
use crate::stream_guard::GuardedReader;

/// C builder wrapping a native `SqlQueryBuilder`, which owns the caller's
/// dataset snapshot (clone) and a private copy of the SQL text.
pub struct LanceSqlQueryBuilder {
    /// `None` once `build` has consumed the native builder. The handle
    /// allocation itself is freed on `build`'s consuming return.
    inner: Option<SqlQueryBuilder>,
}

/// C handle wrapping a planned native `SqlQuery`.
pub struct LanceSqlQuery {
    /// `None` once `into_stream` has consumed the native query.
    inner: Option<SqlQuery>,
}

/// Construct a SQL builder from a dataset snapshot and a query string.
///
/// The SQL text is copied before this function returns. The builder owns a
/// native `Dataset` clone — its snapshot, object store and session — so a
/// closed `LanceDataset` handle does not invalidate it.
///
/// Returns NULL on error, including NULL arguments.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_sql_builder_new(
    dataset: *const LanceDataset,
    sql: *const c_char,
) -> *mut LanceSqlQueryBuilder {
    ffi_try!(unsafe { dataset_sql_builder_new_inner(dataset, sql) }, null)
}

unsafe fn dataset_sql_builder_new_inner(
    dataset: *const LanceDataset,
    sql: *const c_char,
) -> Result<*mut LanceSqlQueryBuilder> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    if sql.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "sql must not be NULL".into(),
        ));
    }
    // SAFETY: non-NULL (checked above); the caller guarantees a live handle
    // and a NUL-terminated string valid for this call.
    let sql = unsafe { helpers::parse_c_string(sql)? }.expect("non-NULL sql parses to a string");
    let snapshot = unsafe { &*dataset }.snapshot();
    let builder = SqlQueryBuilder::new((*snapshot).clone(), sql);
    Ok(Box::into_raw(Box::new(LanceSqlQueryBuilder {
        inner: Some(builder),
    })))
}

/// Close a SQL query builder handle. NULL-safe. Close exactly once.
/// `build` consumes the allocation, so a consumed builder must not be closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_sql_query_builder_close(builder: *mut LanceSqlQueryBuilder) {
    if !builder.is_null() {
        crate::error::swallow_unwind("lance_sql_query_builder_close", || unsafe {
            let _ = Box::from_raw(builder);
        });
    }
}

/// Close a SQL query handle. NULL-safe. Close exactly once.
/// `into_stream` consumes the allocation, so a consumed query must not be
/// closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_sql_query_close(query: *mut LanceSqlQuery) {
    if !query.is_null() {
        crate::error::swallow_unwind("lance_sql_query_close", || unsafe {
            let _ = Box::from_raw(query);
        });
    }
}

/// Name the table in the SELECT statement. The string is copied. An empty
/// name is passed to native unchanged. The native default is `"dataset"`.
///
/// Returns 0 on success, -1 on error. A NULL name is rejected and the
/// builder keeps its prior table name.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_sql_query_builder_set_table_name(
    builder: *mut LanceSqlQueryBuilder,
    table_name: *const c_char,
) -> i32 {
    ffi_try!(
        unsafe { sql_builder_set_table_name_inner(builder, table_name) },
        neg
    )
}

unsafe fn sql_builder_set_table_name_inner(
    builder: *mut LanceSqlQueryBuilder,
    table_name: *const c_char,
) -> Result<i32> {
    if table_name.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "table_name must not be NULL".into(),
        ));
    }
    // SAFETY: non-NULL (checked above); caller guarantees a NUL-terminated
    // string valid for this call.
    let name = unsafe { helpers::parse_c_string(table_name)? }
        .expect("non-NULL table_name parses to a string");
    apply_sql_builder(builder, |inner| inner.table_name(name))
}

/// Inject the native `_rowid` system column into the scan output where the
/// native planner permits it. Passed to the native builder unchanged.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_sql_query_builder_set_with_row_id(
    builder: *mut LanceSqlQueryBuilder,
    enabled: bool,
) -> i32 {
    ffi_try!(
        unsafe { apply_sql_builder(builder, |inner| inner.with_row_id(enabled)) },
        neg
    )
}

/// Inject the native `_row_address` system column into the scan output where
/// the native planner permits it. Passed to the native builder unchanged.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_sql_query_builder_set_with_row_addr(
    builder: *mut LanceSqlQueryBuilder,
    enabled: bool,
) -> i32 {
    ffi_try!(
        unsafe { apply_sql_builder(builder, |inner| inner.with_row_addr(enabled)) },
        neg
    )
}

/// Set how blob columns are materialized, as a `LanceBlobHandling` integer:
/// 0 descriptors for blob columns (the native default), 1 bytes for every
/// blob column, 2 descriptors for every binary column. Other values are
/// rejected and the builder keeps its prior handling. `None` until set.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_sql_query_builder_set_blob_handling(
    builder: *mut LanceSqlQueryBuilder,
    handling: i32,
) -> i32 {
    ffi_try!(
        unsafe { sql_builder_set_blob_handling_inner(builder, handling) },
        neg
    )
}

unsafe fn sql_builder_set_blob_handling_inner(
    builder: *mut LanceSqlQueryBuilder,
    handling: i32,
) -> Result<i32> {
    let parsed = match handling {
        0 => BlobHandling::BlobsDescriptions,
        1 => BlobHandling::AllBinary,
        2 => BlobHandling::AllDescriptions,
        other => {
            return Err(lance_core::Error::invalid_input(format!(
                "blob_handling must be 0 (blobs as descriptions), 1 (all binary) or 2 (all descriptions); got {other}"
            )));
        }
    };
    apply_sql_builder(builder, |inner| inner.blob_handling(parsed))
}

/// Set the native target output batch size (rows). The native builder
/// validates the value's range when the query is built; a foreign integer
/// that does not fit the native `usize` is rejected here and the builder
/// keeps its prior value. `None` until set.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_sql_query_builder_set_batch_size(
    builder: *mut LanceSqlQueryBuilder,
    batch_size: u64,
) -> i32 {
    ffi_try!(
        unsafe { sql_builder_set_batch_size_inner(builder, batch_size) },
        neg
    )
}

unsafe fn sql_builder_set_batch_size_inner(
    builder: *mut LanceSqlQueryBuilder,
    batch_size: u64,
) -> Result<i32> {
    let batch_size = usize::try_from(batch_size).map_err(|_| {
        lance_core::Error::invalid_input(format!(
            "batch_size = {batch_size} does not fit the native usize; expected 0..={}",
            usize::MAX
        ))
    })?;
    apply_sql_builder(builder, |inner| inner.batch_size(batch_size))
}

/// Set the native target output batch size in bytes. `None` until set; the
/// native builder owns the value's interpretation and validation.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_sql_query_builder_set_batch_size_bytes(
    builder: *mut LanceSqlQueryBuilder,
    batch_size_bytes: u64,
) -> i32 {
    ffi_try!(
        unsafe { apply_sql_builder(builder, |inner| inner.batch_size_bytes(batch_size_bytes)) },
        neg
    )
}

/// Apply a native builder method to the stored builder. The stored builder
/// value is taken before the call and restored afterwards, so a panic inside
/// the native method cannot leave a half-mutated builder behind.
unsafe fn apply_sql_builder(
    builder: *mut LanceSqlQueryBuilder,
    f: impl FnOnce(SqlQueryBuilder) -> SqlQueryBuilder,
) -> Result<i32> {
    if builder.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "builder must not be NULL".into(),
        ));
    }
    // SAFETY: non-NULL (checked above); caller guarantees a live handle.
    let builder = unsafe { &mut *builder };
    let inner = builder.inner.take().ok_or_else(|| {
        lance_core::Error::invalid_input_source("builder was already consumed".into())
    })?;
    builder.inner = Some(f(inner));
    Ok(0)
}

/// Plan the configured query, consuming `builder` on every return path —
/// including foreign-pointer errors and native planning failures. On success
/// `*out_query` receives an independently owned query handle; on error the
/// slot is left unchanged.
///
/// Native planning classifies malformed SQL as invalid input. Batch-size
/// validation happens here.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_sql_query_builder_build(
    builder: *mut LanceSqlQueryBuilder,
    out_query: *mut *mut LanceSqlQuery,
) -> i32 {
    ffi_try!(
        unsafe { sql_query_builder_build_inner(builder, out_query) },
        neg
    )
}

unsafe fn sql_query_builder_build_inner(
    builder: *mut LanceSqlQueryBuilder,
    out_query: *mut *mut LanceSqlQuery,
) -> Result<i32> {
    // Consume the caller's allocation before any operation that can fail, so
    // the freed handle is never observed by the caller again.
    let builder = unsafe { consume_sql_builder(builder)? };
    let inner = builder.inner.ok_or_else(|| {
        lance_core::Error::invalid_input_source("builder was already consumed".into())
    })?;
    if out_query.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_query must not be NULL".into(),
        ));
    }
    let query = block_on(inner.build())?;
    // SAFETY: `out_query` is non-NULL (checked above) and the caller
    // guarantees it points to writable storage.
    unsafe {
        ptr::write(
            out_query,
            Box::into_raw(Box::new(LanceSqlQuery { inner: Some(query) })),
        );
    }
    Ok(0)
}

/// Move the SQL query's native stream into `out_stream`, consuming `query` on
/// every return path. On success `*out_stream` is a guarded Arrow C stream
/// (release it exactly once); it keeps the query's dataset snapshot alive and
/// remains valid after the query, builder and dataset handles close. On error
/// the stream slot is left unchanged.
///
/// The stream is not materialized here; batches are produced lazily by the
/// native execution plan. Malformed queries can also fail at this native
/// physical-planning stage.
///
/// Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_sql_query_into_stream(
    query: *mut LanceSqlQuery,
    out_stream: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe { sql_query_into_stream_inner(query, out_stream) },
        neg
    )
}

unsafe fn sql_query_into_stream_inner(
    query: *mut LanceSqlQuery,
    out_stream: *mut FFI_ArrowArrayStream,
) -> Result<i32> {
    let query = unsafe { consume_sql_query(query)? };
    let inner = query.inner.ok_or_else(|| {
        lance_core::Error::invalid_input_source("query was already consumed".into())
    })?;
    if out_stream.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_stream must not be NULL".into(),
        ));
    }
    let stream = block_on(inner.into_stream())?;
    let schema = stream.schema();
    let guarded = GuardedReader::new(
        stream,
        schema,
        RT.handle().clone(),
        Arc::new(AtomicBool::new(false)),
    );
    let ffi_stream = FFI_ArrowArrayStream::new(Box::new(guarded));
    // SAFETY: non-NULL (checked above); caller guarantees writable storage.
    unsafe {
        ptr::write_unaligned(out_stream, ffi_stream);
    }
    Ok(0)
}

/// Take ownership of a SQL builder allocation. The allocation is freed on
/// every return path; callers must not use the handle afterwards.
unsafe fn consume_sql_builder(builder: *mut LanceSqlQueryBuilder) -> Result<LanceSqlQueryBuilder> {
    if builder.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "builder must not be NULL".into(),
        ));
    }
    // SAFETY: the caller transfers ownership of a live allocation to this
    // call exactly once; the box frees it on return.
    Ok(unsafe { *Box::from_raw(builder) })
}

/// Take ownership of a SQL query allocation, freeing it on every return.
unsafe fn consume_sql_query(query: *mut LanceSqlQuery) -> Result<LanceSqlQuery> {
    if query.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "query must not be NULL".into(),
        ));
    }
    // SAFETY: the caller transfers ownership of a live allocation to this
    // call exactly once; the box frees it on return.
    Ok(unsafe { *Box::from_raw(query) })
}
