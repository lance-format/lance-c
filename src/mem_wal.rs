// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! MemWAL / LSM public workflow.
//!
//! This module wraps the native MemWAL building blocks that the Python
//! binding exposes: initialization and sharding configuration, index
//! details, shard writer mutation and durability, snapshots, LSM scanners,
//! point and vector lookup plans, and merge-insert with atomic SSTable
//! compaction progress.
//!
//! The binding is deliberately thin. Foreign input validation (pointers,
//! counts, overflow, UUID syntax, enum values, Arrow shapes) happens here;
//! sharding ranges, index rules, planner validation, and execution errors
//! come from native code. Native owners such as `ShardWriter`, `LsmScanner`,
//! `SharedWriteStats`, and `Arc<dyn ExecutionPlan>` are retained directly
//! instead of being reconstructed after close.

use std::collections::HashMap;
use std::ffi::{CString, c_char};
use std::slice;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use arrow::array::{Array, FixedSizeListArray, Float32Array, RecordBatch, StructArray};
use arrow::compute::concat_batches;
use arrow::datatypes::{DataType, Field, Schema as ArrowSchema};
use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi_stream::FFI_ArrowArrayStream;
use arrow::record_batch::RecordBatchIterator;
use arrow_array::RecordBatchReader;
use datafusion::common::ScalarValue;
use datafusion::physical_plan::display::DisplayableExecutionPlan;
use datafusion::physical_plan::{ExecutionPlan, collect};
use datafusion::prelude::SessionContext;
use lance::Dataset;
use lance::dataset::mem_wal::scanner::{
    LsmDataSourceCollector, LsmPointLookupPlanner, LsmVectorSearchPlanner, SsTable,
    parse_filter_expr,
};
use lance::dataset::mem_wal::write::WriteStatsSnapshot;
use lance::dataset::mem_wal::write::stats::SharedWriteStats;
use lance::dataset::mem_wal::{
    DatasetMemWalExt, LsmScanner, ShardSnapshot, ShardWriter, ShardWriterConfig,
    arrow_schema_with_field_ids, evaluate_sharding_spec,
};
use lance_index::mem_wal::{ShardingField, ShardingSpec};
use lance_index::vector::hnsw::builder::HnswBuildParams;
use lance_linalg::distance::DistanceType;
use lance_table::system_index::mem_wal::CompactedSsTable;
use uuid::Uuid;

use crate::batch::LanceBatch;
use crate::dataset::LanceDataset;
use crate::error::{ffi_try, set_last_error, swallow_unwind};
use crate::helpers::{parse_c_string, take_arrow_array_stream, validate_slice_length};
use crate::runtime::{RT, block_on};
use crate::schema::LanceSchema;
use crate::stream_guard::{GuardedReader, guarded_ffi_stream_from_reader};

// ─── Value types ─────────────────────────────────────────────────────────────

/// Native compacted SSTable value transferred through the C ABI.
///
/// `shard_id` holds the 16 canonical UUID bytes. This is the single shared
/// value type for MemWAL compaction progress.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LanceCompactedSsTable {
    pub shard_id: [u8; 16],
    pub generation: u64,
}

impl LanceCompactedSsTable {
    /// Project one native compaction-progress value into the shared C ABI form.
    pub(crate) fn from_native(value: &CompactedSsTable) -> Self {
        Self {
            shard_id: *value.shard_id.as_bytes(),
            generation: value.generation,
        }
    }
}

/// Convert one caller-supplied compaction-progress value into native form.
///
/// This is the single conversion used by every C ABI surface that accepts a
/// `LanceCompactedSsTable`: merge-insert compaction and the
/// `UpdateMemWalState` operation share the same native value.
pub(crate) fn compacted_ss_table_native(value: &LanceCompactedSsTable) -> CompactedSsTable {
    CompactedSsTable::new(Uuid::from_bytes(value.shard_id), value.generation)
}

/// Copy caller-supplied compacted SSTable values into native form.
pub(crate) unsafe fn parse_compacted_sstables(
    compacted_sstables: *const LanceCompactedSsTable,
    num_compacted_sstables: usize,
) -> lance_core::Result<Vec<CompactedSsTable>> {
    if num_compacted_sstables == 0 {
        return Ok(Vec::new());
    }
    if compacted_sstables.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            format!(
                "compacted_sstables must not be NULL when num_compacted_sstables is {num_compacted_sstables}"
            )
            .into(),
        ));
    }
    validate_slice_length::<LanceCompactedSsTable>(num_compacted_sstables, "compacted_sstables")?;
    let entries = unsafe { slice::from_raw_parts(compacted_sstables, num_compacted_sstables) };
    Ok(entries.iter().map(compacted_ss_table_native).collect())
}

/// Per-index HNSW build parameters.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceMemWalHnswParams {
    pub index_name: *const c_char,
    pub has_num_edges: i32,
    pub num_edges: u32,
    pub has_ef_construction: i32,
    pub ef_construction: u32,
    pub has_max_level: i32,
    pub max_level: u32,
}

/// Optional overrides for a native `ShardWriterConfig`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct LanceShardWriterConfig {
    pub has_durable_write: i32,
    pub durable_write: i32,
    pub has_max_wal_buffer_size: i32,
    pub max_wal_buffer_size: u64,
    pub has_max_wal_flush_interval_ms: i32,
    pub max_wal_flush_interval_ms: u64,
    pub has_max_memtable_size: i32,
    pub max_memtable_size: u64,
    pub has_max_memtable_rows: i32,
    pub max_memtable_rows: u64,
    pub has_max_memtable_batches: i32,
    pub max_memtable_batches: u64,
    pub has_max_unflushed_memtable_bytes: i32,
    pub max_unflushed_memtable_bytes: u64,
    pub has_manifest_scan_batch_size: i32,
    pub manifest_scan_batch_size: u64,
    pub has_backpressure_log_interval_ms: i32,
    pub backpressure_log_interval_ms: u64,
    pub has_stats_log_interval_ms: i32,
    pub stats_log_interval_ms: u64,
    pub hnsw_params: *const LanceMemWalHnswParams,
    pub num_hnsw_params: usize,
}

/// Snapshot of native write counters.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct LanceShardWriterStats {
    pub put_count: u64,
    pub put_time_nanos: u64,
    pub wal_flush_count: u64,
    pub wal_flush_time_nanos: u64,
    pub wal_flush_bytes: u64,
    pub wal_io_time_nanos: u64,
    pub wal_io_count: u64,
    pub index_update_time_nanos: u64,
    pub index_update_count: u64,
    pub index_update_rows: u64,
    pub memtable_flush_count: u64,
    pub memtable_flush_time_nanos: u64,
    pub memtable_flush_rows: u64,
    pub memtable_flush_failures: u64,
}

/// Snapshot of live MemTable and memory statistics.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct LanceShardMemTableStats {
    pub is_live: i32,
    pub row_count: u64,
    pub batch_count: u64,
    pub estimated_size_bytes: u64,
    pub index_bytes: u64,
    pub frozen_bytes: u64,
    pub grace_bytes: u64,
    pub retained_bytes: u64,
    pub generation: u64,
    pub has_max_buffered_batch_position: i32,
    pub max_buffered_batch_position: u64,
    pub durable_batch_count: u64,
    pub global_offset: u64,
    pub has_pending_wal_start_batch_position: i32,
    pub pending_wal_start_batch_position: u64,
    pub has_pending_wal_end_batch_position: i32,
    pub pending_wal_end_batch_position: u64,
    pub pending_wal_batch_count: u64,
    pub pending_wal_row_count: u64,
    pub pending_wal_estimated_bytes: u64,
    pub frozen_count: u64,
}

/// Batch-position range produced by a native put or delete.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct LanceShardWriteResult {
    pub batch_position_begin: u64,
    pub batch_position_end: u64,
}

// ─── Small helpers ───────────────────────────────────────────────────────────

fn invalid(message: impl Into<String>) -> lance_core::Error {
    lance_core::Error::invalid_input(message.into())
}

fn closed_writer_error() -> lance_core::Error {
    invalid("ShardWriter is already closed")
}

fn required<'a, T>(ptr: *const T, name: &str) -> lance_core::Result<&'a T> {
    if ptr.is_null() {
        return Err(invalid(format!("{name} must not be NULL")));
    }
    Ok(unsafe { &*ptr })
}

fn required_mut<'a, T>(ptr: *mut T, name: &str) -> lance_core::Result<&'a mut T> {
    if ptr.is_null() {
        return Err(invalid(format!("{name} must not be NULL")));
    }
    Ok(unsafe { &mut *ptr })
}

fn require_out<T>(out: *mut T, name: &str) -> lance_core::Result<()> {
    if out.is_null() {
        return Err(invalid(format!("{name} must not be NULL")));
    }
    Ok(())
}

fn cstring(value: &str, label: &str) -> lance_core::Result<CString> {
    CString::new(value).map_err(|_| invalid(format!("{label} contains an embedded NUL byte")))
}

fn bool_flag(value: i32, name: &str) -> lance_core::Result<bool> {
    match value {
        0 => Ok(false),
        1 => Ok(true),
        other => Err(invalid(format!("{name} must be 0 or 1, got {other}"))),
    }
}

fn parse_uuid(value: &str, label: &str) -> lance_core::Result<Uuid> {
    Uuid::parse_str(value)
        .map_err(|error| invalid(format!("{label} '{value}' is not a valid UUID: {error}")))
}

fn usize_from_u64(value: u64, name: &str) -> lance_core::Result<usize> {
    usize::try_from(value).map_err(|_| {
        invalid(format!(
            "{name} value {value} exceeds the platform usize maximum {}",
            usize::MAX
        ))
    })
}

/// Read `count` non-NULL C strings from an explicit-length array.
unsafe fn c_string_array(
    ptr: *const *const c_char,
    count: usize,
    name: &str,
) -> lance_core::Result<Vec<String>> {
    if count == 0 {
        return Ok(Vec::new());
    }
    if ptr.is_null() {
        return Err(invalid(format!(
            "{name} must not be NULL when its count is {count}"
        )));
    }
    crate::helpers::validate_slice_length::<*const c_char>(count, name)?;
    let entries = unsafe { slice::from_raw_parts(ptr, count) };
    let mut values = Vec::with_capacity(count);
    for (index, entry) in entries.iter().enumerate() {
        let value = unsafe { parse_c_string(*entry)? }
            .ok_or_else(|| invalid(format!("{name}[{index}] must not be NULL")))?;
        values.push(value.to_string());
    }
    Ok(values)
}

/// Clone `count` snapshot handles. Every entry must be non-NULL.
unsafe fn snapshots_from_c(
    snapshots: *const *const LanceMemWalShardSnapshot,
    count: usize,
) -> lance_core::Result<Vec<ShardSnapshot>> {
    if count == 0 {
        return Ok(Vec::new());
    }
    if snapshots.is_null() {
        return Err(invalid(format!(
            "snapshots must not be NULL when num_snapshots is {count}"
        )));
    }
    crate::helpers::validate_slice_length::<*const LanceMemWalShardSnapshot>(count, "snapshots")?;
    let entries = unsafe { slice::from_raw_parts(snapshots, count) };
    let mut values = Vec::with_capacity(count);
    for (index, entry) in entries.iter().enumerate() {
        let handle = required(*entry, &format!("snapshots[{index}]"))?;
        values.push(handle.snapshot.clone());
    }
    Ok(values)
}

fn get_pk_columns(dataset: &Dataset) -> lance_core::Result<Vec<String>> {
    let fields = dataset.schema().unenforced_primary_key();
    if fields.is_empty() {
        return Err(invalid(
            "Dataset has no primary key. Set 'lance-schema:unenforced-primary-key' metadata \
             on the primary key field(s).",
        ));
    }
    Ok(fields.iter().map(|field| field.name.clone()).collect())
}

/// Directory identifier of the dataset's MemWAL layout.
///
/// An empty string is the legacy `_mem_wal` layout. Native scanners and
/// planners default to that legacy value, so every binding constructor must
/// pass the dataset's actual identifier or reads resolve against the wrong
/// directory.
fn mem_wal_dir_id(dataset: &Dataset) -> lance_core::Result<String> {
    Ok(block_on(dataset.mem_wal_index_details())?
        .map(|details| details.dir_id)
        .unwrap_or_default())
}

fn write_ffi_schema(out: *mut FFI_ArrowSchema, schema: &ArrowSchema) -> lance_core::Result<()> {
    if out.is_null() {
        return Err(invalid("out must not be NULL"));
    }
    if unsafe { &*out }.release.is_some() {
        return Err(invalid(
            "out must be an empty ArrowSchema (release callback must be NULL)",
        ));
    }
    let ffi_schema = FFI_ArrowSchema::try_from(schema)?;
    unsafe {
        std::ptr::write_unaligned(out, ffi_schema);
    }
    Ok(())
}

fn write_ffi_stream<S>(out: *mut FFI_ArrowArrayStream, stream: S) -> lance_core::Result<()>
where
    S: RecordBatchReader + Send + 'static,
{
    if out.is_null() {
        return Err(invalid("out must not be NULL"));
    }
    let ffi_stream = guarded_ffi_stream_from_reader(stream)?;
    unsafe {
        std::ptr::write_unaligned(out, ffi_stream);
    }
    Ok(())
}

fn duration_nanos(value: Duration) -> u64 {
    u64::try_from(value.as_nanos()).unwrap_or(u64::MAX)
}

fn stats_snapshot_to_c(stats: &WriteStatsSnapshot) -> LanceShardWriterStats {
    LanceShardWriterStats {
        put_count: stats.put_count,
        put_time_nanos: duration_nanos(stats.put_time),
        wal_flush_count: stats.wal_flush_count,
        wal_flush_time_nanos: duration_nanos(stats.wal_flush_time),
        wal_flush_bytes: stats.wal_flush_bytes,
        wal_io_time_nanos: duration_nanos(stats.wal_io_time),
        wal_io_count: stats.wal_io_count,
        index_update_time_nanos: duration_nanos(stats.index_update_time),
        index_update_count: stats.index_update_count,
        index_update_rows: stats.index_update_rows,
        memtable_flush_count: stats.memtable_flush_count,
        memtable_flush_time_nanos: duration_nanos(stats.memtable_flush_time),
        memtable_flush_rows: stats.memtable_flush_rows,
        memtable_flush_failures: stats.memtable_flush_failures,
    }
}

fn snapshot_from_manifest(manifest: lance_index::mem_wal::ShardManifest) -> ShardSnapshot {
    ShardSnapshot {
        shard_id: manifest.shard_id,
        spec_id: manifest.shard_spec_id,
        current_generation: manifest.current_generation,
        sstables: manifest
            .sstables
            .into_iter()
            .map(|sstable| SsTable {
                generation: sstable.generation,
                path: sstable.path,
            })
            .collect(),
    }
}

// ─── Index details ───────────────────────────────────────────────────────────

/// Owned copy of native `MemWalIndexDetails` with C-string views.
pub struct LanceMemWalIndexDetails {
    num_shards: u32,
    maintain_all: bool,
    dir_id: CString,
    maintained: Vec<CString>,
    specs: Vec<ShardingSpec>,
    defaults: Vec<(CString, CString)>,
}

fn details_ref<'a>(
    details: *const LanceMemWalIndexDetails,
) -> lance_core::Result<&'a LanceMemWalIndexDetails> {
    required(details, "details")
}

impl LanceMemWalIndexDetails {
    fn from_native(details: lance_index::mem_wal::MemWalIndexDetails) -> lance_core::Result<Self> {
        let maintained = details
            .maintained_indexes
            .iter()
            .map(|name| cstring(name, "maintained index name"))
            .collect::<lance_core::Result<Vec<_>>>()?;
        let mut defaults = details
            .writer_config_defaults
            .iter()
            .map(|(key, value)| {
                Ok((
                    cstring(key, "writer config default key")?,
                    cstring(value, "writer config default value")?,
                ))
            })
            .collect::<lance_core::Result<Vec<_>>>()?;
        defaults.sort_by(|a, b| a.0.as_bytes().cmp(b.0.as_bytes()));
        Ok(Self {
            num_shards: details.num_shards,
            maintain_all: details.maintain_all_indexes,
            dir_id: cstring(&details.dir_id, "MemWAL dir_id")?,
            maintained,
            specs: details.sharding_specs,
            defaults,
        })
    }
}

/// Initialize MemWAL on a dataset.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_initialize(
    dataset: *mut LanceDataset,
    mode: i32,
    column: *const c_char,
    num_buckets: u32,
    has_maintained_indexes: i32,
    maintained_indexes: *const *const c_char,
    num_maintained_indexes: usize,
    config: *const LanceShardWriterConfig,
) -> i32 {
    ffi_try!(
        unsafe {
            initialize_inner(
                dataset,
                mode,
                column,
                num_buckets,
                has_maintained_indexes,
                maintained_indexes,
                num_maintained_indexes,
                config,
            )
        },
        neg
    )
}

#[allow(clippy::too_many_arguments)]
unsafe fn initialize_inner(
    dataset: *mut LanceDataset,
    mode: i32,
    column: *const c_char,
    num_buckets: u32,
    has_maintained_indexes: i32,
    maintained_indexes: *const *const c_char,
    num_maintained_indexes: usize,
    config: *const LanceShardWriterConfig,
) -> lance_core::Result<i32> {
    let dataset = required(dataset, "dataset")?;
    let column = unsafe { parse_c_string(column)? };
    let maintain = bool_flag(has_maintained_indexes, "has_maintained_indexes")?;
    let maintained = if maintain {
        Some(unsafe {
            c_string_array(
                maintained_indexes,
                num_maintained_indexes,
                "maintained_indexes",
            )?
        })
    } else {
        None
    };
    let config = if config.is_null() {
        None
    } else {
        Some(unsafe { parse_writer_config(config)? })
    };

    // The builder requires a column for bucket and identity sharding and has
    // no way to observe an unused one for manual/unsharded sharding.
    match mode {
        0 | 1 => {
            if column.is_some() {
                return Err(invalid(
                    "column must be NULL for manual and unsharded sharding",
                ));
            }
            if num_buckets != 0 {
                return Err(invalid(
                    "num_buckets must be 0 for manual and unsharded sharding",
                ));
            }
        }
        2 | 3 => {
            if column.is_none() {
                return Err(invalid(
                    "column must not be NULL for bucket and identity sharding",
                ));
            }
        }
        other => {
            return Err(invalid(format!(
                "invalid sharding mode {other}; expected 0 (manual), 1 (unsharded), \
                 2 (bucket), or 3 (identity)"
            )));
        }
    }

    dataset.with_mut(|native| {
        block_on(async {
            let mut builder = native.initialize_mem_wal();
            builder = match mode {
                0 => builder,
                1 => builder.unsharded(),
                2 => builder.bucket_sharding(column.unwrap(), num_buckets),
                3 => builder.identity_sharding(column.unwrap()),
                _ => unreachable!("mode validated above"),
            };
            if let Some(indexes) = maintained {
                builder = builder.maintained_indexes(indexes);
            }
            if let Some(config) = config {
                builder = builder.writer_config_defaults(config);
            }
            builder.execute().await
        })
    })?;
    Ok(0)
}

/// Read the native MemWAL index details.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_index_details(
    dataset: *mut LanceDataset,
    out: *mut *mut LanceMemWalIndexDetails,
) -> i32 {
    ffi_try!(unsafe { index_details_inner(dataset, out) }, neg)
}

unsafe fn index_details_inner(
    dataset: *mut LanceDataset,
    out: *mut *mut LanceMemWalIndexDetails,
) -> lance_core::Result<i32> {
    require_out(out, "out")?;
    let dataset = required(dataset, "dataset")?;
    let snapshot = dataset.snapshot();
    match block_on(snapshot.mem_wal_index_details())? {
        Some(details) => {
            let handle = LanceMemWalIndexDetails::from_native(details)?;
            unsafe {
                *out = Box::into_raw(Box::new(handle));
            }
        }
        None => unsafe {
            *out = std::ptr::null_mut();
        },
    }
    Ok(0)
}

/// Free an index-details handle. NULL-safe.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_index_details_free(details: *mut LanceMemWalIndexDetails) {
    if !details.is_null() {
        swallow_unwind("lance_mem_wal_index_details_free", || unsafe {
            drop(Box::from_raw(details));
        });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_index_details_num_shards(
    details: *const LanceMemWalIndexDetails,
) -> u32 {
    ffi_try!(Ok(details_ref(details)?.num_shards), 0)
}

/// Borrow the MemWAL state directory identifier.
///
/// An empty string denotes the legacy `_mem_wal` directory. The value remains
/// valid until the details handle is freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_index_details_dir_id(
    details: *const LanceMemWalIndexDetails,
) -> *const c_char {
    ffi_try!(Ok(details_ref(details)?.dir_id.as_ptr()), null)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_index_details_maintain_all_indexes(
    details: *const LanceMemWalIndexDetails,
) -> i32 {
    ffi_try!(Ok(details_ref(details)?.maintain_all as i32), 0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_index_details_maintained_index_count(
    details: *const LanceMemWalIndexDetails,
) -> usize {
    ffi_try!(Ok(details_ref(details)?.maintained.len()), 0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_index_details_maintained_index_at(
    details: *const LanceMemWalIndexDetails,
    index: usize,
    out_name: *mut *const c_char,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out_name, "out_name")?;
            let details = details_ref(details)?;
            let name = details.maintained.get(index).ok_or_else(|| {
                invalid(format!(
                    "maintained index index {index} is out of range for {} entries",
                    details.maintained.len()
                ))
            })?;
            *out_name = name.as_ptr();
            Ok(0)
        },
        neg
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_index_details_sharding_spec_count(
    details: *const LanceMemWalIndexDetails,
) -> usize {
    ffi_try!(Ok(details_ref(details)?.specs.len()), 0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_index_details_sharding_spec_at(
    details: *const LanceMemWalIndexDetails,
    index: usize,
    out_spec: *mut *mut LanceMemWalShardingSpec,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out_spec, "out_spec")?;
            let details = details_ref(details)?;
            let spec = details.specs.get(index).ok_or_else(|| {
                invalid(format!(
                    "sharding spec index {index} is out of range for {} entries",
                    details.specs.len()
                ))
            })?;
            *out_spec = Box::into_raw(Box::new(LanceMemWalShardingSpec { spec: spec.clone() }));
            Ok(0)
        },
        neg
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_index_details_writer_config_default_count(
    details: *const LanceMemWalIndexDetails,
) -> usize {
    ffi_try!(Ok(details_ref(details)?.defaults.len()), 0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_index_details_writer_config_default_at(
    details: *const LanceMemWalIndexDetails,
    index: usize,
    out_key: *mut *const c_char,
    out_value: *mut *const c_char,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out_key, "out_key")?;
            require_out(out_value, "out_value")?;
            let details = details_ref(details)?;
            let (key, value) = details.defaults.get(index).ok_or_else(|| {
                invalid(format!(
                    "writer config default index {index} is out of range for {} entries",
                    details.defaults.len()
                ))
            })?;
            *out_key = key.as_ptr();
            *out_value = value.as_ptr();
            Ok(0)
        },
        neg
    )
}

// ─── Sharding field values ──────────────────────────────────────────────────

/// Owned sharding field with stable C-string views.
pub struct LanceMemWalShardingField {
    field: ShardingField,
    field_id: CString,
    transform: Option<CString>,
    expression: Option<CString>,
    result_type: CString,
    parameters: Vec<(CString, CString)>,
}

fn field_ref<'a>(
    field: *const LanceMemWalShardingField,
) -> lance_core::Result<&'a LanceMemWalShardingField> {
    required(field, "field")
}

impl LanceMemWalShardingField {
    fn from_native(field: ShardingField) -> lance_core::Result<Self> {
        let field_id = cstring(&field.field_id, "field_id")?;
        let transform = field
            .transform
            .as_deref()
            .map(|value| cstring(value, "transform"))
            .transpose()?;
        let expression = field
            .expression
            .as_deref()
            .map(|value| cstring(value, "expression"))
            .transpose()?;
        let result_type = cstring(&field.result_type, "result_type")?;
        let mut parameters = field
            .parameters
            .iter()
            .map(|(key, value)| {
                Ok((
                    cstring(key, "parameter key")?,
                    cstring(value, "parameter value")?,
                ))
            })
            .collect::<lance_core::Result<Vec<_>>>()?;
        parameters.sort_by(|a, b| a.0.as_bytes().cmp(b.0.as_bytes()));
        Ok(Self {
            field,
            field_id,
            transform,
            expression,
            result_type,
            parameters,
        })
    }
}

/// Create a sharding field value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_field_new(
    field_id: *const c_char,
    source_ids: *const i32,
    num_source_ids: usize,
    transform: *const c_char,
    expression: *const c_char,
    result_type: *const c_char,
    parameter_keys: *const *const c_char,
    parameter_values: *const *const c_char,
    num_parameters: usize,
    out: *mut *mut LanceMemWalShardingField,
) -> i32 {
    ffi_try!(
        unsafe {
            sharding_field_new_inner(
                field_id,
                source_ids,
                num_source_ids,
                transform,
                expression,
                result_type,
                parameter_keys,
                parameter_values,
                num_parameters,
                out,
            )
        },
        neg
    )
}

#[allow(clippy::too_many_arguments)]
unsafe fn sharding_field_new_inner(
    field_id: *const c_char,
    source_ids: *const i32,
    num_source_ids: usize,
    transform: *const c_char,
    expression: *const c_char,
    result_type: *const c_char,
    parameter_keys: *const *const c_char,
    parameter_values: *const *const c_char,
    num_parameters: usize,
    out: *mut *mut LanceMemWalShardingField,
) -> lance_core::Result<i32> {
    require_out(out, "out")?;
    let field_id = unsafe { parse_c_string(field_id)? }
        .ok_or_else(|| invalid("field_id must not be NULL"))?
        .to_string();
    let source_ids = if num_source_ids == 0 {
        Vec::new()
    } else {
        if source_ids.is_null() {
            return Err(invalid(format!(
                "source_ids must not be NULL when num_source_ids is {num_source_ids}"
            )));
        }
        crate::helpers::validate_slice_length::<i32>(num_source_ids, "source_ids")?;
        unsafe { slice::from_raw_parts(source_ids, num_source_ids) }.to_vec()
    };
    let transform = unsafe { parse_c_string(transform)? }.map(str::to_string);
    let expression = unsafe { parse_c_string(expression)? }.map(str::to_string);
    let result_type = unsafe { parse_c_string(result_type)? }
        .unwrap_or("")
        .to_string();

    if num_parameters > 0 && (parameter_keys.is_null() || parameter_values.is_null()) {
        return Err(invalid(
            "parameter_keys and parameter_values must not be NULL when num_parameters > 0",
        ));
    }
    crate::helpers::validate_slice_length::<*const c_char>(num_parameters, "parameters")?;
    let mut parameters = HashMap::with_capacity(num_parameters);
    for index in 0..num_parameters {
        let key_ptr = unsafe { *parameter_keys.add(index) };
        let value_ptr = unsafe { *parameter_values.add(index) };
        let key = unsafe { parse_c_string(key_ptr)? }
            .ok_or_else(|| invalid(format!("parameter_keys[{index}] must not be NULL")))?;
        let value = unsafe { parse_c_string(value_ptr)? }
            .ok_or_else(|| invalid(format!("parameter_values[{index}] must not be NULL")))?;
        parameters.insert(key.to_string(), value.to_string());
    }

    let field = ShardingField {
        field_id,
        source_ids,
        transform,
        expression,
        result_type,
        parameters,
    };
    let handle = LanceMemWalShardingField::from_native(field)?;
    unsafe {
        *out = Box::into_raw(Box::new(handle));
    }
    Ok(0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_field_clone(
    field: *const LanceMemWalShardingField,
    out: *mut *mut LanceMemWalShardingField,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out, "out")?;
            let field = field_ref(field)?;
            let handle = LanceMemWalShardingField::from_native(field.field.clone())?;
            *out = Box::into_raw(Box::new(handle));
            Ok(0)
        },
        neg
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_field_free(field: *mut LanceMemWalShardingField) {
    if !field.is_null() {
        swallow_unwind("lance_mem_wal_sharding_field_free", || unsafe {
            drop(Box::from_raw(field));
        });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_field_id(
    field: *const LanceMemWalShardingField,
) -> *const c_char {
    ffi_try!(Ok(field_ref(field)?.field_id.as_ptr()), std::ptr::null())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_field_source_ids(
    field: *const LanceMemWalShardingField,
    out_count: *mut usize,
) -> *const i32 {
    ffi_try!(
        unsafe {
            require_out(out_count, "out_count")?;
            let field = field_ref(field)?;
            *out_count = field.field.source_ids.len();
            Ok(if field.field.source_ids.is_empty() {
                std::ptr::null()
            } else {
                field.field.source_ids.as_ptr()
            })
        },
        std::ptr::null()
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_field_transform(
    field: *const LanceMemWalShardingField,
) -> *const c_char {
    ffi_try!(
        Ok(field_ref(field)?
            .transform
            .as_ref()
            .map_or(std::ptr::null(), |value| value.as_ptr())),
        std::ptr::null()
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_field_expression(
    field: *const LanceMemWalShardingField,
) -> *const c_char {
    ffi_try!(
        Ok(field_ref(field)?
            .expression
            .as_ref()
            .map_or(std::ptr::null(), |value| value.as_ptr())),
        std::ptr::null()
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_field_result_type(
    field: *const LanceMemWalShardingField,
) -> *const c_char {
    ffi_try!(Ok(field_ref(field)?.result_type.as_ptr()), std::ptr::null())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_field_parameter_count(
    field: *const LanceMemWalShardingField,
) -> usize {
    ffi_try!(Ok(field_ref(field)?.parameters.len()), 0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_field_parameter_at(
    field: *const LanceMemWalShardingField,
    index: usize,
    out_key: *mut *const c_char,
    out_value: *mut *const c_char,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out_key, "out_key")?;
            require_out(out_value, "out_value")?;
            let field = field_ref(field)?;
            let (key, value) = field.parameters.get(index).ok_or_else(|| {
                invalid(format!(
                    "parameter index {index} is out of range for {} entries",
                    field.parameters.len()
                ))
            })?;
            *out_key = key.as_ptr();
            *out_value = value.as_ptr();
            Ok(0)
        },
        neg
    )
}

// ─── Sharding specs ─────────────────────────────────────────────────────────

/// Owned native sharding spec.
pub struct LanceMemWalShardingSpec {
    spec: ShardingSpec,
}

fn spec_ref<'a>(
    spec: *const LanceMemWalShardingSpec,
) -> lance_core::Result<&'a LanceMemWalShardingSpec> {
    required(spec, "spec")
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_spec_new(
    spec_id: u32,
    fields: *const *const LanceMemWalShardingField,
    num_fields: usize,
    out: *mut *mut LanceMemWalShardingSpec,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out, "out")?;
            crate::helpers::validate_slice_length::<*const LanceMemWalShardingField>(
                num_fields, "fields",
            )?;
            let mut cloned = Vec::with_capacity(num_fields);
            if num_fields > 0 {
                if fields.is_null() {
                    return Err(invalid(format!(
                        "fields must not be NULL when num_fields is {num_fields}"
                    )));
                }
                for (index, entry) in slice::from_raw_parts(fields, num_fields).iter().enumerate() {
                    let field = required(*entry, &format!("fields[{index}]"))?;
                    cloned.push(field.field.clone());
                }
            }
            *out = Box::into_raw(Box::new(LanceMemWalShardingSpec {
                spec: ShardingSpec {
                    spec_id,
                    fields: cloned,
                },
            }));
            Ok(0)
        },
        neg
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_spec_clone(
    spec: *const LanceMemWalShardingSpec,
    out: *mut *mut LanceMemWalShardingSpec,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out, "out")?;
            let spec = spec_ref(spec)?;
            *out = Box::into_raw(Box::new(LanceMemWalShardingSpec {
                spec: spec.spec.clone(),
            }));
            Ok(0)
        },
        neg
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_spec_free(spec: *mut LanceMemWalShardingSpec) {
    if !spec.is_null() {
        swallow_unwind("lance_mem_wal_sharding_spec_free", || unsafe {
            drop(Box::from_raw(spec));
        });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_spec_id(
    spec: *const LanceMemWalShardingSpec,
) -> u32 {
    ffi_try!(Ok(spec_ref(spec)?.spec.spec_id), 0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_spec_field_count(
    spec: *const LanceMemWalShardingSpec,
) -> usize {
    ffi_try!(Ok(spec_ref(spec)?.spec.fields.len()), 0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_sharding_spec_field_at(
    spec: *const LanceMemWalShardingSpec,
    index: usize,
    out_field: *mut *mut LanceMemWalShardingField,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out_field, "out_field")?;
            let spec = spec_ref(spec)?;
            let field = spec.spec.fields.get(index).ok_or_else(|| {
                invalid(format!(
                    "field index {index} is out of range for {} entries",
                    spec.spec.fields.len()
                ))
            })?;
            let handle = LanceMemWalShardingField::from_native(field.clone())?;
            *out_field = Box::into_raw(Box::new(handle));
            Ok(0)
        },
        neg
    )
}

/// Evaluate a sharding spec against the dataset schema.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_evaluate_sharding_spec(
    dataset: *mut LanceDataset,
    batch: *const LanceBatch,
    spec: *const LanceMemWalShardingSpec,
    out_batch: *mut *mut LanceBatch,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out_batch, "out_batch")?;
            let dataset = required(dataset, "dataset")?;
            let batch = required(batch, "batch")?;
            let spec = spec_ref(spec)?;
            let result =
                evaluate_sharding_spec(&batch.inner, &spec.spec, dataset.snapshot().schema())?;
            *out_batch = Box::into_raw(Box::new(LanceBatch { inner: result }));
            Ok(0)
        },
        neg
    )
}

/// Evaluate a sharding spec against an explicit native schema.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_evaluate_sharding_spec_with_schema(
    batch: *const LanceBatch,
    spec: *const LanceMemWalShardingSpec,
    schema: *const LanceSchema,
    out_batch: *mut *mut LanceBatch,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out_batch, "out_batch")?;
            let batch = required(batch, "batch")?;
            let spec = spec_ref(spec)?;
            let schema = required(schema, "schema")?;
            let result = evaluate_sharding_spec(&batch.inner, &spec.spec, &schema.inner)?;
            *out_batch = Box::into_raw(Box::new(LanceBatch { inner: result }));
            Ok(0)
        },
        neg
    )
}

// ─── Shard snapshots ────────────────────────────────────────────────────────

/// Owned native `ShardSnapshot` with C-string views for its SSTable paths.
pub struct LanceMemWalShardSnapshot {
    snapshot: ShardSnapshot,
    shard_id: CString,
    paths: Vec<CString>,
}

fn snapshot_ref<'a>(
    snapshot: *const LanceMemWalShardSnapshot,
) -> lance_core::Result<&'a LanceMemWalShardSnapshot> {
    required(snapshot, "snapshot")
}

impl LanceMemWalShardSnapshot {
    fn new(snapshot: ShardSnapshot) -> lance_core::Result<Self> {
        let shard_id = cstring(&snapshot.shard_id.to_string(), "shard_id")?;
        let paths = snapshot
            .sstables
            .iter()
            .map(|sstable| cstring(&sstable.path, "sstable path"))
            .collect::<lance_core::Result<Vec<_>>>()?;
        Ok(Self {
            snapshot,
            shard_id,
            paths,
        })
    }

    fn sync_paths(&mut self) -> lance_core::Result<()> {
        self.paths = self
            .snapshot
            .sstables
            .iter()
            .map(|sstable| cstring(&sstable.path, "sstable path"))
            .collect::<lance_core::Result<Vec<_>>>()?;
        Ok(())
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_shard_snapshot_new(
    shard_id: *const c_char,
    out: *mut *mut LanceMemWalShardSnapshot,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out, "out")?;
            let shard_id =
                parse_c_string(shard_id)?.ok_or_else(|| invalid("shard_id must not be NULL"))?;
            let uuid = parse_uuid(shard_id, "shard_id")?;
            let handle = LanceMemWalShardSnapshot::new(ShardSnapshot::new(uuid))?;
            *out = Box::into_raw(Box::new(handle));
            Ok(0)
        },
        neg
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_shard_snapshot_clone(
    snapshot: *const LanceMemWalShardSnapshot,
    out: *mut *mut LanceMemWalShardSnapshot,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out, "out")?;
            let snapshot = snapshot_ref(snapshot)?;
            let handle = LanceMemWalShardSnapshot::new(snapshot.snapshot.clone())?;
            *out = Box::into_raw(Box::new(handle));
            Ok(0)
        },
        neg
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_shard_snapshot_free(
    snapshot: *mut LanceMemWalShardSnapshot,
) {
    if !snapshot.is_null() {
        swallow_unwind("lance_mem_wal_shard_snapshot_free", || unsafe {
            drop(Box::from_raw(snapshot));
        });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_shard_snapshot_shard_id(
    snapshot: *const LanceMemWalShardSnapshot,
) -> *const c_char {
    ffi_try!(
        Ok(snapshot_ref(snapshot)?.shard_id.as_ptr()),
        std::ptr::null()
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_shard_snapshot_spec_id(
    snapshot: *const LanceMemWalShardSnapshot,
) -> u32 {
    ffi_try!(Ok(snapshot_ref(snapshot)?.snapshot.spec_id), 0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_shard_snapshot_current_generation(
    snapshot: *const LanceMemWalShardSnapshot,
) -> u64 {
    ffi_try!(Ok(snapshot_ref(snapshot)?.snapshot.current_generation), 0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_shard_snapshot_with_spec_id(
    snapshot: *mut LanceMemWalShardSnapshot,
    spec_id: u32,
) -> i32 {
    ffi_try!(
        {
            let snapshot = required_mut(snapshot, "snapshot")?;
            snapshot.snapshot.spec_id = spec_id;
            Ok(0)
        },
        neg
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_shard_snapshot_with_current_generation(
    snapshot: *mut LanceMemWalShardSnapshot,
    generation: u64,
) -> i32 {
    ffi_try!(
        {
            let snapshot = required_mut(snapshot, "snapshot")?;
            snapshot.snapshot.current_generation = generation;
            Ok(0)
        },
        neg
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_shard_snapshot_with_sstable(
    snapshot: *mut LanceMemWalShardSnapshot,
    generation: u64,
    path: *const c_char,
) -> i32 {
    ffi_try!(
        {
            let snapshot = required_mut(snapshot, "snapshot")?;
            let path = unsafe { parse_c_string(path)? }
                .ok_or_else(|| invalid("path must not be NULL"))?
                .to_string();
            snapshot
                .snapshot
                .sstables
                .push(SsTable { generation, path });
            snapshot.sync_paths()?;
            Ok(0)
        },
        neg
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_shard_snapshot_sstable_count(
    snapshot: *const LanceMemWalShardSnapshot,
) -> usize {
    ffi_try!(Ok(snapshot_ref(snapshot)?.snapshot.sstables.len()), 0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_shard_snapshot_sstable_at(
    snapshot: *const LanceMemWalShardSnapshot,
    index: usize,
    out_generation: *mut u64,
    out_path: *mut *const c_char,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out_generation, "out_generation")?;
            require_out(out_path, "out_path")?;
            let snapshot = snapshot_ref(snapshot)?;
            let sstable = snapshot.snapshot.sstables.get(index).ok_or_else(|| {
                invalid(format!(
                    "sstable index {index} is out of range for {} entries",
                    snapshot.snapshot.sstables.len()
                ))
            })?;
            *out_generation = sstable.generation;
            *out_path = snapshot.paths[index].as_ptr();
            Ok(0)
        },
        neg
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_compacted_sstable_new(
    shard_id: *const c_char,
    generation: u64,
    out: *mut LanceCompactedSsTable,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out, "out")?;
            let shard_id =
                parse_c_string(shard_id)?.ok_or_else(|| invalid("shard_id must not be NULL"))?;
            let uuid = parse_uuid(shard_id, "shard_id")?;
            *out = LanceCompactedSsTable {
                shard_id: *uuid.as_bytes(),
                generation,
            };
            Ok(0)
        },
        neg
    )
}

// ─── Writer configuration ──────────────────────────────────────────────────

unsafe fn parse_writer_config(
    config: *const LanceShardWriterConfig,
) -> lance_core::Result<ShardWriterConfig> {
    let raw = required(config, "config")?;
    let mut native = ShardWriterConfig::default();

    if bool_flag(raw.has_durable_write, "has_durable_write")? {
        native.durable_write = bool_flag(raw.durable_write, "durable_write")?;
    }
    if bool_flag(raw.has_max_wal_buffer_size, "has_max_wal_buffer_size")? {
        native.max_wal_buffer_size =
            usize_from_u64(raw.max_wal_buffer_size, "max_wal_buffer_size")?;
    }
    if bool_flag(
        raw.has_max_wal_flush_interval_ms,
        "has_max_wal_flush_interval_ms",
    )? {
        native.max_wal_flush_interval = Some(Duration::from_millis(raw.max_wal_flush_interval_ms));
    }
    if bool_flag(raw.has_max_memtable_size, "has_max_memtable_size")? {
        native.max_memtable_size = usize_from_u64(raw.max_memtable_size, "max_memtable_size")?;
    }
    if bool_flag(raw.has_max_memtable_rows, "has_max_memtable_rows")? {
        native.max_memtable_rows = usize_from_u64(raw.max_memtable_rows, "max_memtable_rows")?;
    }
    if bool_flag(raw.has_max_memtable_batches, "has_max_memtable_batches")? {
        native.max_memtable_batches =
            usize_from_u64(raw.max_memtable_batches, "max_memtable_batches")?;
    }
    if bool_flag(
        raw.has_max_unflushed_memtable_bytes,
        "has_max_unflushed_memtable_bytes",
    )? {
        native.max_unflushed_memtable_bytes = usize_from_u64(
            raw.max_unflushed_memtable_bytes,
            "max_unflushed_memtable_bytes",
        )?;
    }
    if bool_flag(
        raw.has_manifest_scan_batch_size,
        "has_manifest_scan_batch_size",
    )? {
        native.manifest_scan_batch_size =
            usize_from_u64(raw.manifest_scan_batch_size, "manifest_scan_batch_size")?;
    }
    if bool_flag(
        raw.has_backpressure_log_interval_ms,
        "has_backpressure_log_interval_ms",
    )? {
        native.backpressure_log_interval = Duration::from_millis(raw.backpressure_log_interval_ms);
    }
    if bool_flag(raw.has_stats_log_interval_ms, "has_stats_log_interval_ms")? {
        native.stats_log_interval = if raw.stats_log_interval_ms == 0 {
            None
        } else {
            Some(Duration::from_millis(raw.stats_log_interval_ms))
        };
    }

    if raw.num_hnsw_params > 0 {
        if raw.hnsw_params.is_null() {
            return Err(invalid(format!(
                "hnsw_params must not be NULL when num_hnsw_params is {}",
                raw.num_hnsw_params
            )));
        }
        crate::helpers::validate_slice_length::<LanceMemWalHnswParams>(
            raw.num_hnsw_params,
            "hnsw_params",
        )?;
        let entries = unsafe { slice::from_raw_parts(raw.hnsw_params, raw.num_hnsw_params) };
        for (index, entry) in entries.iter().enumerate() {
            let name = unsafe { parse_c_string(entry.index_name)? }
                .filter(|name| !name.is_empty())
                .ok_or_else(|| {
                    invalid(format!(
                        "hnsw_params[{index}].index_name must not be NULL or empty"
                    ))
                })?;
            let mut params = HnswBuildParams::default();
            if bool_flag(
                entry.has_num_edges,
                &format!("hnsw_params[{index}].has_num_edges"),
            )? {
                params = params.num_edges(usize::try_from(entry.num_edges).map_err(|_| {
                    invalid(format!(
                        "hnsw_params[{index}].num_edges value {} exceeds usize::MAX",
                        entry.num_edges
                    ))
                })?);
            }
            if bool_flag(
                entry.has_ef_construction,
                &format!("hnsw_params[{index}].has_ef_construction"),
            )? {
                params = params.ef_construction(usize::try_from(entry.ef_construction).map_err(
                    |_| {
                        invalid(format!(
                            "hnsw_params[{index}].ef_construction value {} exceeds usize::MAX",
                            entry.ef_construction
                        ))
                    },
                )?);
            }
            if bool_flag(
                entry.has_max_level,
                &format!("hnsw_params[{index}].has_max_level"),
            )? {
                let max_level = u16::try_from(entry.max_level).map_err(|_| {
                    invalid(format!(
                        "hnsw_params[{index}].max_level value {} does not fit in u16",
                        entry.max_level
                    ))
                })?;
                params = params.max_level(max_level);
            }
            native.hnsw_params.insert(name.to_string(), params);
        }
    }

    Ok(native)
}

// ─── Shard writer ──────────────────────────────────────────────────────────

/// Owned native shard writer with its retained statistics owner.
pub struct LanceShardWriter {
    shard_id: Uuid,
    shard_id_c: CString,
    dataset: Arc<Dataset>,
    writer: Option<ShardWriter>,
    stats: SharedWriteStats,
}

fn writer_ref<'a>(writer: *const LanceShardWriter) -> lance_core::Result<&'a LanceShardWriter> {
    required(writer, "writer")
}

impl LanceShardWriter {
    fn live(&self) -> lance_core::Result<&ShardWriter> {
        self.writer.as_ref().ok_or_else(closed_writer_error)
    }

    fn live_mut_slot(&mut self) -> lance_core::Result<&mut ShardWriter> {
        self.writer.as_mut().ok_or_else(closed_writer_error)
    }
}

/// Open a native shard writer for one shard.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_shard_writer_open(
    dataset: *mut LanceDataset,
    shard_id: *const c_char,
    config: *const LanceShardWriterConfig,
    out: *mut *mut LanceShardWriter,
) -> i32 {
    ffi_try!(
        unsafe { writer_open_inner(dataset, shard_id, config, out) },
        neg
    )
}

unsafe fn writer_open_inner(
    dataset: *mut LanceDataset,
    shard_id: *const c_char,
    config: *const LanceShardWriterConfig,
    out: *mut *mut LanceShardWriter,
) -> lance_core::Result<i32> {
    require_out(out, "out")?;
    let dataset = required(dataset, "dataset")?;
    let shard_id_text =
        unsafe { parse_c_string(shard_id)? }.ok_or_else(|| invalid("shard_id must not be NULL"))?;
    let shard_id = parse_uuid(shard_id_text, "shard_id")?;
    let config = if config.is_null() {
        ShardWriterConfig::default()
    } else {
        unsafe { parse_writer_config(config)? }
    };

    let snapshot = dataset.snapshot();
    let writer = block_on(snapshot.mem_wal_writer(shard_id, config))?;
    let stats = writer.stats_handle();
    let handle = LanceShardWriter {
        shard_id,
        shard_id_c: cstring(shard_id_text, "shard_id")?,
        dataset: snapshot,
        writer: Some(writer),
        stats,
    };
    unsafe {
        *out = Box::into_raw(Box::new(handle));
    }
    Ok(0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_shard_writer_shard_id(
    writer: *const LanceShardWriter,
) -> *const c_char {
    ffi_try!(
        Ok(writer_ref(writer)?.shard_id_c.as_ptr()),
        std::ptr::null()
    )
}

/// Write one stream of batches.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_shard_writer_put(
    writer: *mut LanceShardWriter,
    stream: *mut FFI_ArrowArrayStream,
    out_result: *mut LanceShardWriteResult,
) -> i32 {
    ffi_try!(unsafe { writer_put_inner(writer, stream, out_result) }, neg)
}

unsafe fn writer_put_inner(
    writer: *mut LanceShardWriter,
    stream: *mut FFI_ArrowArrayStream,
    out_result: *mut LanceShardWriteResult,
) -> lance_core::Result<i32> {
    let writer = required_mut(writer, "writer")?;
    // Consume the stream before touching the live writer: once a non-NULL
    // stream is accepted it is released exactly once on every return path.
    let reader = unsafe { take_arrow_array_stream(stream)? };
    let batches: Vec<RecordBatch> = reader.collect::<Result<Vec<_>, _>>()?;
    let live = writer.live_mut_slot()?;
    let result = block_on(live.put(batches))?;
    if !out_result.is_null() {
        unsafe {
            *out_result = LanceShardWriteResult {
                batch_position_begin: result.batch_positions.start as u64,
                batch_position_end: result.batch_positions.end as u64,
            };
        }
    }
    Ok(0)
}

/// Delete rows by primary key from one stream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_shard_writer_delete(
    writer: *mut LanceShardWriter,
    stream: *mut FFI_ArrowArrayStream,
    out_result: *mut LanceShardWriteResult,
) -> i32 {
    ffi_try!(
        unsafe { writer_delete_inner(writer, stream, out_result) },
        neg
    )
}

unsafe fn writer_delete_inner(
    writer: *mut LanceShardWriter,
    stream: *mut FFI_ArrowArrayStream,
    out_result: *mut LanceShardWriteResult,
) -> lance_core::Result<i32> {
    let writer = required_mut(writer, "writer")?;
    let reader = unsafe { take_arrow_array_stream(stream)? };
    let batches: Vec<RecordBatch> = reader.collect::<Result<Vec<_>, _>>()?;
    let live = writer.live_mut_slot()?;
    let result = block_on(live.delete(batches))?;
    if !out_result.is_null() {
        unsafe {
            *out_result = LanceShardWriteResult {
                batch_position_begin: result.batch_positions.start as u64,
                batch_position_end: result.batch_positions.end as u64,
            };
        }
    }
    Ok(0)
}

/// Snapshot native write counters.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_shard_writer_stats(
    writer: *const LanceShardWriter,
    out: *mut LanceShardWriterStats,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out, "out")?;
            let writer = writer_ref(writer)?;
            *out = stats_snapshot_to_c(&writer.stats.snapshot());
            Ok(0)
        },
        neg
    )
}

/// Snapshot live MemTable and memory statistics.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_shard_writer_memtable_stats(
    writer: *const LanceShardWriter,
    out: *mut LanceShardMemTableStats,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out, "out")?;
            let writer = writer_ref(writer)?;
            let live = writer.live()?;
            let stats = block_on(live.memtable_stats())?;
            let memory = live.memory();
            *out = LanceShardMemTableStats {
                is_live: 1,
                row_count: stats.row_count as u64,
                batch_count: stats.batch_count as u64,
                estimated_size_bytes: memory.row_bytes() as u64,
                index_bytes: memory.index_bytes() as u64,
                frozen_bytes: memory.frozen_bytes() as u64,
                grace_bytes: memory.grace_bytes() as u64,
                retained_bytes: memory.retained_bytes() as u64,
                generation: stats.generation,
                has_max_buffered_batch_position: stats.max_buffered_batch_position.is_some() as i32,
                max_buffered_batch_position: stats.max_buffered_batch_position.unwrap_or(0) as u64,
                durable_batch_count: stats.durable_batch_count as u64,
                global_offset: stats.global_offset as u64,
                has_pending_wal_start_batch_position: stats
                    .pending_wal_start_batch_position
                    .is_some() as i32,
                pending_wal_start_batch_position: stats
                    .pending_wal_start_batch_position
                    .unwrap_or(0) as u64,
                has_pending_wal_end_batch_position: stats.pending_wal_end_batch_position.is_some()
                    as i32,
                pending_wal_end_batch_position: stats.pending_wal_end_batch_position.unwrap_or(0)
                    as u64,
                pending_wal_batch_count: stats.pending_wal_batch_count as u64,
                pending_wal_row_count: stats.pending_wal_row_count as u64,
                pending_wal_estimated_bytes: stats.pending_wal_estimated_bytes as u64,
                frozen_count: stats.frozen_count as u64,
            };
            Ok(0)
        },
        neg
    )
}

/// Build a snapshot from the writer's latest manifest.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_shard_writer_snapshot(
    writer: *const LanceShardWriter,
    out: *mut *mut LanceMemWalShardSnapshot,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out, "out")?;
            let writer = writer_ref(writer)?;
            let live = writer.live()?;
            match block_on(live.manifest())? {
                Some(manifest) => {
                    let handle = LanceMemWalShardSnapshot::new(snapshot_from_manifest(manifest))?;
                    *out = Box::into_raw(Box::new(handle));
                }
                None => {
                    *out = std::ptr::null_mut();
                }
            }
            Ok(0)
        },
        neg
    )
}

/// Create an LSM scanner over supplied snapshots plus the writer's state.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_shard_writer_lsm_scanner(
    writer: *mut LanceShardWriter,
    snapshots: *const *const LanceMemWalShardSnapshot,
    num_snapshots: usize,
    out: *mut *mut LanceLsmScanner,
) -> i32 {
    ffi_try!(
        unsafe { writer_lsm_scanner_inner(writer, snapshots, num_snapshots, out) },
        neg
    )
}

unsafe fn writer_lsm_scanner_inner(
    writer: *mut LanceShardWriter,
    snapshots: *const *const LanceMemWalShardSnapshot,
    num_snapshots: usize,
    out: *mut *mut LanceLsmScanner,
) -> lance_core::Result<i32> {
    require_out(out, "out")?;
    let writer = required(writer, "writer")?;
    let live = writer.live()?;
    let mut combined = unsafe { snapshots_from_c(snapshots, num_snapshots)? };
    let refs = block_on(live.in_memory_memtable_refs())?;
    let manifest = block_on(live.manifest())?;
    combined.retain(|snapshot| snapshot.shard_id != writer.shard_id);
    match manifest {
        Some(manifest) => combined.push(snapshot_from_manifest(manifest)),
        None => combined.push(ShardSnapshot::new(writer.shard_id)),
    }
    let scanner = LsmScanner::new(
        writer.dataset.clone(),
        combined,
        get_pk_columns(&writer.dataset)?,
    )
    .with_mem_wal_dir_id(live.config().dir_id.clone())
    .with_in_memory_memtables(writer.shard_id, refs);
    unsafe { *out = Box::into_raw(Box::new(LanceLsmScanner::new(scanner))) };
    Ok(0)
}

/// Flush and close the native writer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_shard_writer_close(writer: *mut LanceShardWriter) -> i32 {
    ffi_try!(unsafe { writer_close_inner(writer) }, neg)
}

unsafe fn writer_close_inner(writer: *mut LanceShardWriter) -> lance_core::Result<i32> {
    let writer = required_mut(writer, "writer")?;
    let Some(live) = writer.writer.take() else {
        return Ok(0);
    };
    block_on(live.close())?;
    Ok(0)
}

/// Free a shard writer without flushing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_shard_writer_free(writer: *mut LanceShardWriter) {
    if !writer.is_null() {
        swallow_unwind("lance_shard_writer_free", || unsafe {
            let mut writer = Box::from_raw(writer);
            if let Some(live) = writer.writer.take()
                && let Err(error) = block_on(live.abort())
            {
                log::error!("lance_shard_writer_free: native abort failed: {error}");
            }
        });
    }
}

// ─── LSM scanner ───────────────────────────────────────────────────────────

/// Owned native LSM scanner.
pub struct LanceLsmScanner {
    scanner: Option<LsmScanner>,
    poison: Arc<AtomicBool>,
}

fn scanner_ref<'a>(scanner: *const LanceLsmScanner) -> lance_core::Result<&'a LanceLsmScanner> {
    let scanner = required(scanner, "scanner")?;
    if scanner.poison.load(Ordering::SeqCst) {
        set_last_error(
            crate::error::LanceErrorCode::Panic,
            "scanner is poisoned by an earlier panic",
        );
        return Err(lance_core::Error::internal(
            "scanner is poisoned by an earlier panic",
        ));
    }
    Ok(scanner)
}

fn scanner_mut<'a>(scanner: *mut LanceLsmScanner) -> lance_core::Result<&'a mut LanceLsmScanner> {
    let scanner = required_mut(scanner, "scanner")?;
    if scanner.poison.load(Ordering::SeqCst) {
        return Err(lance_core::Error::internal(
            "scanner is poisoned by an earlier panic",
        ));
    }
    Ok(scanner)
}

impl LanceLsmScanner {
    fn new(scanner: LsmScanner) -> Self {
        Self {
            scanner: Some(scanner),
            poison: Arc::new(AtomicBool::new(false)),
        }
    }

    fn current(&self) -> lance_core::Result<&LsmScanner> {
        self.scanner
            .as_ref()
            .ok_or_else(|| lance_core::Error::internal("scanner is no longer usable"))
    }

    fn take(&mut self) -> lance_core::Result<LsmScanner> {
        self.scanner
            .take()
            .ok_or_else(|| lance_core::Error::internal("scanner is no longer usable"))
    }
}

/// Create an LSM scanner over a dataset and snapshots.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_scanner_from_snapshots(
    dataset: *mut LanceDataset,
    snapshots: *const *const LanceMemWalShardSnapshot,
    num_snapshots: usize,
    out: *mut *mut LanceLsmScanner,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out, "out")?;
            let dataset = required(dataset, "dataset")?;
            let snapshots = snapshots_from_c(snapshots, num_snapshots)?;
            let snapshot = dataset.snapshot();
            let pk_columns = get_pk_columns(&snapshot)?;
            let dir_id = mem_wal_dir_id(&snapshot)?;
            let scanner =
                LsmScanner::new(snapshot, snapshots, pk_columns).with_mem_wal_dir_id(dir_id);
            *out = Box::into_raw(Box::new(LanceLsmScanner::new(scanner)));
            Ok(0)
        },
        neg
    )
}

/// Project the scan onto ordered column names.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_scanner_project(
    scanner: *mut LanceLsmScanner,
    columns: *const *const c_char,
    num_columns: usize,
) -> i32 {
    ffi_try!(
        {
            let scanner = scanner_mut(scanner)?;
            let columns = unsafe { c_string_array(columns, num_columns, "columns")? };
            let current = scanner.take()?;
            match current.project(&columns) {
                Ok(next) => {
                    scanner.scanner = Some(next);
                    Ok(0)
                }
                Err(error) => Err(error),
            }
        },
        neg
    )
}

/// Set a native SQL filter expression.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_scanner_filter(
    scanner: *mut LanceLsmScanner,
    filter: *const c_char,
) -> i32 {
    ffi_try!(
        {
            let scanner = scanner_mut(scanner)?;
            let filter = unsafe { parse_c_string(filter)? }
                .ok_or_else(|| invalid("filter must not be NULL"))?;
            // Validate before consuming the native scanner so an invalid
            // expression leaves the handle unchanged.
            parse_filter_expr(scanner.current()?.schema().as_ref(), filter)?;
            let current = scanner.take()?;
            match current.filter(filter) {
                Ok(next) => {
                    scanner.scanner = Some(next);
                    Ok(0)
                }
                Err(error) => Err(error),
            }
        },
        neg
    )
}

/// Limit the scan, optionally with an offset.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_scanner_limit(
    scanner: *mut LanceLsmScanner,
    limit: i64,
    has_limit: i32,
    offset: i64,
    has_offset: i32,
) -> i32 {
    ffi_try!(
        {
            let scanner = scanner_mut(scanner)?;
            let limit = bool_flag(has_limit, "has_limit")?.then_some(limit);
            let offset = bool_flag(has_offset, "has_offset")?.then_some(offset);
            if let Some(value) = limit
                && value < 0
            {
                return Err(invalid("limit must be non-negative"));
            }
            if let Some(value) = offset
                && value < 0
            {
                return Err(invalid("offset must be non-negative"));
            }
            let current = scanner.take()?;
            match current.limit(limit, offset) {
                Ok(next) => {
                    scanner.scanner = Some(next);
                    Ok(0)
                }
                Err(error) => Err(error),
            }
        },
        neg
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_scanner_with_row_address(scanner: *mut LanceLsmScanner) -> i32 {
    ffi_try!(
        {
            let scanner = scanner_mut(scanner)?;
            let current = scanner.take()?;
            scanner.scanner = Some(current.with_row_address());
            Ok(0)
        },
        neg
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_scanner_with_memtable_gen(scanner: *mut LanceLsmScanner) -> i32 {
    ffi_try!(
        {
            let scanner = scanner_mut(scanner)?;
            let current = scanner.take()?;
            scanner.scanner = Some(current.with_memtable_gen());
            Ok(0)
        },
        neg
    )
}

/// Schema produced by the scanner's native composition.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_scanner_schema(
    scanner: *const LanceLsmScanner,
    out: *mut FFI_ArrowSchema,
) -> i32 {
    ffi_try!(
        {
            let scanner = scanner_ref(scanner)?;
            let schema = scanner.current()?.schema();
            write_ffi_schema(out, schema.as_ref())?;
            Ok(0)
        },
        neg
    )
}

/// Execute the scan and return one batch.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_scanner_to_batch(
    scanner: *const LanceLsmScanner,
    out: *mut *mut LanceBatch,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out, "out")?;
            let scanner = scanner_ref(scanner)?;
            let batch = block_on(scanner.current()?.try_into_batch())?;
            *out = Box::into_raw(Box::new(LanceBatch { inner: batch }));
            Ok(0)
        },
        neg
    )
}

/// Execute the scan and export an Arrow stream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_scanner_to_arrow_stream(
    scanner: *const LanceLsmScanner,
    out: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        {
            let scanner = scanner_ref(scanner)?;
            let stream = block_on(scanner.current()?.try_into_stream())?;
            let schema = stream.schema();
            let reader = GuardedReader::new(
                stream,
                schema,
                RT.handle().clone(),
                Arc::clone(&scanner.poison),
            );
            write_ffi_stream(out, reader)?;
            Ok(0)
        },
        neg
    )
}

/// Count matching rows without materializing them.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_scanner_count_rows(
    scanner: *const LanceLsmScanner,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out_count, "out_count")?;
            let scanner = scanner_ref(scanner)?;
            *out_count = block_on(scanner.current()?.count_rows())?;
            Ok(0)
        },
        neg
    )
}

/// Free an LSM scanner. NULL-safe.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_scanner_free(scanner: *mut LanceLsmScanner) {
    if !scanner.is_null() {
        swallow_unwind("lance_lsm_scanner_free", || unsafe {
            drop(Box::from_raw(scanner));
        });
    }
}

// ─── Point lookup planner ──────────────────────────────────────────────────

/// Owned native point-lookup planner.
pub struct LanceLsmPointLookupPlanner {
    planner: LsmPointLookupPlanner,
    dataset_schema: Arc<ArrowSchema>,
    pk_columns: Vec<String>,
}

fn point_planner_ref<'a>(
    planner: *const LanceLsmPointLookupPlanner,
) -> lance_core::Result<&'a LanceLsmPointLookupPlanner> {
    required(planner, "planner")
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_point_lookup_planner_new(
    dataset: *mut LanceDataset,
    snapshots: *const *const LanceMemWalShardSnapshot,
    num_snapshots: usize,
    has_pk_columns: i32,
    pk_columns: *const *const c_char,
    num_pk_columns: usize,
    out: *mut *mut LanceLsmPointLookupPlanner,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out, "out")?;
            let dataset = required(dataset, "dataset")?;
            let snapshots = snapshots_from_c(snapshots, num_snapshots)?;
            let native = dataset.snapshot();
            let pk_columns = if bool_flag(has_pk_columns, "has_pk_columns")? {
                c_string_array(pk_columns, num_pk_columns, "pk_columns")?
            } else {
                get_pk_columns(&native)?
            };
            let base_schema = Arc::new(ArrowSchema::from(native.schema()));
            let identity_schema = Arc::new(arrow_schema_with_field_ids(native.schema()));
            let dir_id = mem_wal_dir_id(&native)?;
            let collector =
                LsmDataSourceCollector::new(native.clone(), snapshots).with_mem_wal_dir_id(dir_id);
            let planner =
                LsmPointLookupPlanner::new(collector, pk_columns.clone(), base_schema.clone())?
                    .with_identity_schema(identity_schema);
            *out = Box::into_raw(Box::new(LanceLsmPointLookupPlanner {
                planner,
                dataset_schema: base_schema,
                pk_columns,
            }));
            Ok(0)
        },
        neg
    )
}

/// Plan a point lookup for exactly one key row.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_point_lookup_planner_plan(
    planner: *mut LanceLsmPointLookupPlanner,
    pk_value: *const LanceBatch,
    has_columns: i32,
    columns: *const *const c_char,
    num_columns: usize,
    out_plan: *mut *mut LanceMemWalPlan,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out_plan, "out_plan")?;
            let planner = point_planner_ref(planner)?;
            let pk_value = required(pk_value, "pk_value")?;
            let columns = if bool_flag(has_columns, "has_columns")? {
                Some(c_string_array(columns, num_columns, "columns")?)
            } else {
                None
            };
            let values = scalar_values_from_pk_value(&pk_value.inner, &planner.pk_columns)?;
            let plan = block_on(planner.planner.plan_lookup(&values, columns.as_deref()))?;
            *out_plan = Box::into_raw(Box::new(LanceMemWalPlan {
                plan,
                dataset_schema: Arc::clone(&planner.dataset_schema),
            }));
            Ok(0)
        },
        neg
    )
}

fn scalar_values_from_pk_value(
    pk_value: &RecordBatch,
    pk_columns: &[String],
) -> lance_core::Result<Vec<ScalarValue>> {
    if pk_value.num_rows() != 1 {
        return Err(invalid(format!(
            "pk_value must contain exactly one row, got {}",
            pk_value.num_rows()
        )));
    }

    if pk_columns.len() == 1 {
        if pk_value.num_columns() != 1 {
            return Err(invalid(format!(
                "Point lookup for a single-column primary key requires exactly one column, got {}",
                pk_value.num_columns()
            )));
        }
        let column = pk_value.column(0);
        if matches!(column.data_type(), DataType::Struct(_)) {
            return Err(invalid(
                "Point lookup for a single-column primary key requires a non-Struct column",
            ));
        }
        let scalar = ScalarValue::try_from_array(column.as_ref(), 0)
            .map_err(|error| invalid(format!("Cannot convert pk_value: {error}")))?;
        return Ok(vec![scalar]);
    }

    let struct_array = pk_value
        .column(0)
        .as_any()
        .downcast_ref::<StructArray>()
        .ok_or_else(|| {
            invalid(format!(
                "Composite primary key lookup requires a StructArray with exactly one row and {} fields",
                pk_columns.len()
            ))
        })?;
    if pk_value.num_columns() != 1 {
        return Err(invalid(format!(
            "Composite primary key lookup requires exactly one Struct column, got {} columns",
            pk_value.num_columns()
        )));
    }
    if struct_array.num_columns() != pk_columns.len() {
        return Err(invalid(format!(
            "Composite primary key lookup expected {} fields, got {}",
            pk_columns.len(),
            struct_array.num_columns()
        )));
    }
    let mut values = Vec::with_capacity(pk_columns.len());
    for column_name in pk_columns {
        let column = struct_array.column_by_name(column_name).ok_or_else(|| {
            invalid(format!(
                "Composite primary key lookup requires field '{column_name}' in pk_value"
            ))
        })?;
        let scalar = ScalarValue::try_from_array(column.as_ref(), 0)
            .map_err(|error| invalid(format!("Cannot convert composite pk_value: {error}")))?;
        values.push(scalar);
    }
    Ok(values)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_point_lookup_planner_free(
    planner: *mut LanceLsmPointLookupPlanner,
) {
    if !planner.is_null() {
        swallow_unwind("lance_lsm_point_lookup_planner_free", || unsafe {
            drop(Box::from_raw(planner));
        });
    }
}

// ─── Vector search planner ─────────────────────────────────────────────────

/// Owned native vector-search planner.
pub struct LanceLsmVectorSearchPlanner {
    planner: LsmVectorSearchPlanner,
    dataset_schema: Arc<ArrowSchema>,
    vector_dim: usize,
}

fn vector_planner_ref<'a>(
    planner: *const LanceLsmVectorSearchPlanner,
) -> lance_core::Result<&'a LanceLsmVectorSearchPlanner> {
    required(planner, "planner")
}

fn distance_from_c(value: i32) -> lance_core::Result<DistanceType> {
    match value {
        0 => Ok(DistanceType::L2),
        1 => Ok(DistanceType::Cosine),
        2 => Ok(DistanceType::Dot),
        3 => Ok(DistanceType::Hamming),
        other => Err(invalid(format!(
            "invalid distance_type {other}; expected 0 (l2), 1 (cosine), 2 (dot), or 3 (hamming)"
        ))),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_vector_search_planner_new(
    dataset: *mut LanceDataset,
    snapshots: *const *const LanceMemWalShardSnapshot,
    num_snapshots: usize,
    vector_column: *const c_char,
    has_pk_columns: i32,
    pk_columns: *const *const c_char,
    num_pk_columns: usize,
    distance_type: i32,
    filter: *const c_char,
    out: *mut *mut LanceLsmVectorSearchPlanner,
) -> i32 {
    ffi_try!(
        unsafe {
            vector_planner_new_inner(
                dataset,
                snapshots,
                num_snapshots,
                vector_column,
                has_pk_columns,
                pk_columns,
                num_pk_columns,
                distance_type,
                filter,
                out,
            )
        },
        neg
    )
}

#[allow(clippy::too_many_arguments)]
unsafe fn vector_planner_new_inner(
    dataset: *mut LanceDataset,
    snapshots: *const *const LanceMemWalShardSnapshot,
    num_snapshots: usize,
    vector_column: *const c_char,
    has_pk_columns: i32,
    pk_columns: *const *const c_char,
    num_pk_columns: usize,
    distance_type: i32,
    filter: *const c_char,
    out: *mut *mut LanceLsmVectorSearchPlanner,
) -> lance_core::Result<i32> {
    require_out(out, "out")?;
    let dataset = required(dataset, "dataset")?;
    let vector_column = unsafe { parse_c_string(vector_column)? }
        .ok_or_else(|| invalid("vector_column must not be NULL"))?
        .to_string();
    let filter = unsafe { parse_c_string(filter)? }.map(str::to_string);
    let snapshots = unsafe { snapshots_from_c(snapshots, num_snapshots)? };
    let native = dataset.snapshot();
    let pk_columns = if bool_flag(has_pk_columns, "has_pk_columns")? {
        unsafe { c_string_array(pk_columns, num_pk_columns, "pk_columns")? }
    } else {
        get_pk_columns(&native)?
    };

    let base_schema = Arc::new(ArrowSchema::from(native.schema()));
    let identity_schema = Arc::new(arrow_schema_with_field_ids(native.schema()));
    let field = base_schema.field_with_name(&vector_column).map_err(|_| {
        invalid(format!(
            "Column '{vector_column}' not found in dataset schema"
        ))
    })?;
    let vector_dim = match field.data_type() {
        DataType::FixedSizeList(_, size) => *size as usize,
        other => {
            return Err(invalid(format!(
                "Column '{vector_column}' is not a FixedSizeList (got {other:?}). \
                 Vector columns must be FixedSizeList<float32>."
            )));
        }
    };

    let distance = distance_from_c(distance_type)?;
    let filter_expr = match filter {
        Some(filter) => Some(parse_filter_expr(base_schema.as_ref(), &filter)?),
        None => None,
    };
    let collector = LsmDataSourceCollector::new(native.clone(), snapshots)
        .with_mem_wal_dir_id(mem_wal_dir_id(&native)?);
    let planner = LsmVectorSearchPlanner::new(
        collector,
        pk_columns,
        base_schema.clone(),
        vector_column,
        distance,
    )
    .with_identity_schema(identity_schema)
    .with_dataset(native)
    .with_filter(filter_expr);
    unsafe {
        *out = Box::into_raw(Box::new(LanceLsmVectorSearchPlanner {
            planner,
            dataset_schema: base_schema,
            vector_dim,
        }))
    };
    Ok(0)
}

/// Plan a KNN search over the captured LSM tiers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_vector_search_planner_plan(
    planner: *mut LanceLsmVectorSearchPlanner,
    query: *const f32,
    query_len: usize,
    k: u64,
    nprobes: u64,
    has_columns: i32,
    columns: *const *const c_char,
    num_columns: usize,
    refine_base_table: i32,
    overfetch_factor: f64,
    out_plan: *mut *mut LanceMemWalPlan,
) -> i32 {
    ffi_try!(
        unsafe {
            vector_plan_inner(
                planner,
                query,
                query_len,
                k,
                nprobes,
                has_columns,
                columns,
                num_columns,
                refine_base_table,
                overfetch_factor,
                out_plan,
            )
        },
        neg
    )
}

#[allow(clippy::too_many_arguments)]
unsafe fn vector_plan_inner(
    planner: *mut LanceLsmVectorSearchPlanner,
    query: *const f32,
    query_len: usize,
    k: u64,
    nprobes: u64,
    has_columns: i32,
    columns: *const *const c_char,
    num_columns: usize,
    refine_base_table: i32,
    overfetch_factor: f64,
    out_plan: *mut *mut LanceMemWalPlan,
) -> lance_core::Result<i32> {
    require_out(out_plan, "out_plan")?;
    let planner = vector_planner_ref(planner)?;
    if query_len != planner.vector_dim {
        return Err(invalid(format!(
            "Query vector has {query_len} dimensions, expected {}",
            planner.vector_dim
        )));
    }
    let values: &[f32] = if query_len == 0 {
        &[]
    } else {
        if query.is_null() {
            return Err(invalid(format!(
                "query must not be NULL when query_len is {query_len}"
            )));
        }
        crate::helpers::validate_slice_length::<f32>(query_len, "query")?;
        unsafe { slice::from_raw_parts(query, query_len) }
    };
    let refine = bool_flag(refine_base_table, "refine_base_table")?;
    let columns = if bool_flag(has_columns, "has_columns")? {
        Some(unsafe { c_string_array(columns, num_columns, "columns")? })
    } else {
        None
    };
    let k = usize_from_u64(k, "k")?;
    let nprobes = usize_from_u64(nprobes, "nprobes")?;
    let values = Float32Array::from(values.to_vec());
    let field = Arc::new(Field::new("item", DataType::Float32, true));
    let query = FixedSizeListArray::try_new(
        field,
        i32::try_from(query_len)
            .map_err(|_| invalid(format!("query_len {query_len} exceeds i32::MAX")))?,
        Arc::new(values),
        None,
    )
    .map_err(|error| invalid(format!("Cannot create query vector: {error}")))?;
    let plan = block_on(planner.planner.plan_search(
        &query,
        k,
        nprobes,
        columns.as_deref(),
        refine,
        overfetch_factor,
    ))?;
    unsafe {
        *out_plan = Box::into_raw(Box::new(LanceMemWalPlan {
            plan,
            dataset_schema: Arc::clone(&planner.dataset_schema),
        }))
    };
    Ok(0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_lsm_vector_search_planner_free(
    planner: *mut LanceLsmVectorSearchPlanner,
) {
    if !planner.is_null() {
        swallow_unwind("lance_lsm_vector_search_planner_free", || unsafe {
            drop(Box::from_raw(planner));
        });
    }
}

// ─── Execution plans ───────────────────────────────────────────────────────

/// Owned native execution plan plus the planner's dataset schema.
pub struct LanceMemWalPlan {
    plan: Arc<dyn ExecutionPlan>,
    dataset_schema: Arc<ArrowSchema>,
}

fn plan_ref<'a>(plan: *const LanceMemWalPlan) -> lance_core::Result<&'a LanceMemWalPlan> {
    required(plan, "plan")
}

/// Export the plan's output schema as Arrow.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_plan_schema(
    plan: *const LanceMemWalPlan,
    out: *mut FFI_ArrowSchema,
) -> i32 {
    ffi_try!(
        {
            let plan = plan_ref(plan)?;
            write_ffi_schema(out, plan.plan.schema().as_ref())?;
            Ok(0)
        },
        neg
    )
}

/// Export the dataset schema captured by the planner as Arrow.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_plan_dataset_schema(
    plan: *const LanceMemWalPlan,
    out: *mut FFI_ArrowSchema,
) -> i32 {
    ffi_try!(
        {
            let plan = plan_ref(plan)?;
            write_ffi_schema(out, plan.dataset_schema.as_ref())?;
            Ok(0)
        },
        neg
    )
}

/// Format the native plan tree without executing it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_plan_explain(
    plan: *const LanceMemWalPlan,
    out: *mut *mut c_char,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out, "out")?;
            let plan = plan_ref(plan)?;
            let text = DisplayableExecutionPlan::new(plan.plan.as_ref())
                .indent(true)
                .to_string();
            *out = cstring(&text, "explain output")?.into_raw();
            Ok(0)
        },
        neg
    )
}

fn execute_plan_batches(plan: &Arc<dyn ExecutionPlan>) -> lance_core::Result<Vec<RecordBatch>> {
    block_on(async {
        let context = SessionContext::new();
        collect(plan.clone(), context.task_ctx())
            .await
            .map_err(|error| lance_core::Error::io(format!("Plan execution failed: {error}")))
    })
}

/// Execute the plan and concatenate its batches into one.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_plan_to_batch(
    plan: *const LanceMemWalPlan,
    out: *mut *mut LanceBatch,
) -> i32 {
    ffi_try!(
        unsafe {
            require_out(out, "out")?;
            let plan = plan_ref(plan)?;
            let batches = execute_plan_batches(&plan.plan)?;
            let batch = concat_batches(&plan.plan.schema(), &batches).map_err(|error| {
                lance_core::Error::internal(format!(
                    "Cannot concatenate plan output batches: {error}"
                ))
            })?;
            *out = Box::into_raw(Box::new(LanceBatch { inner: batch }));
            Ok(0)
        },
        neg
    )
}

/// Execute the plan and export an Arrow stream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_plan_to_arrow_stream(
    plan: *const LanceMemWalPlan,
    out: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        {
            let plan = plan_ref(plan)?;
            let batches = execute_plan_batches(&plan.plan)?;
            let schema = plan.plan.schema();
            let reader = RecordBatchIterator::new(batches.into_iter().map(Ok), schema);
            write_ffi_stream(out, reader)?;
            Ok(0)
        },
        neg
    )
}

/// Free an execution plan. NULL-safe.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_mem_wal_plan_free(plan: *mut LanceMemWalPlan) {
    if !plan.is_null() {
        swallow_unwind("lance_mem_wal_plan_free", || unsafe {
            drop(Box::from_raw(plan));
        });
    }
}
