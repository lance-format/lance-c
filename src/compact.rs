// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Compaction C API: rewrite small/deleted-heavy fragments into larger ones,
//! committing a new manifest. Operates as a no-op (no version bump) when no
//! fragments need compacting.
//!
//! Mutates the dataset in place under an exclusive write lock; existing
//! scanners that already cloned the inner Arc keep their pre-compaction
//! snapshot view.

use std::ffi::c_char;
use std::ptr;
use std::slice;
use std::sync::Arc;

use lance::Dataset;
use lance::dataset::index::DatasetIndexRemapperOptions;
use lance::dataset::optimize::{
    CompactionMode, CompactionOptions, CompactionPlan, CompactionTask, IndexRemapMode,
    RewriteResult, commit_compaction, compact_files, plan_compaction,
};
use lance_core::Result;
use lance_file::version::LanceFileVersion;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::metadata::owned_c_string;
use crate::runtime::block_on;
use crate::transaction_properties::{LanceTransactionProperties, parse_transaction_properties};

/// Tunable parameters for `lance_dataset_compact_files`. Pass NULL to use the
/// upstream defaults. Each numeric field uses `0` as a "keep upstream default"
/// sentinel; explicit overrides are forwarded after a `usize` range check.
///
/// The five-field layout and its zero-as-default behavior are frozen. Callers
/// constructed this struct at compile time, so appending a field would make
/// the binding read beyond an older caller's allocation. Use
/// `LanceCompactionOptionsV2` for options that require presence information.
#[repr(C)]
pub struct LanceCompactionOptions {
    /// Target row count per output fragment. Fragments below this size are
    /// candidates for being merged with neighbors. `0` uses upstream's
    /// default (~1Mi rows).
    pub target_rows_per_fragment: u64,
    /// Soft cap on rows per row group within an output fragment. `0` uses
    /// upstream's default.
    pub max_rows_per_group: u64,
    /// Soft cap on bytes per output fragment file. `0` uses upstream's
    /// default (the writer's per-file cap).
    pub max_bytes_per_file: u64,
    /// Compute parallelism for compaction tasks. `0` uses upstream's default
    /// (the number of compute-intensive CPUs).
    pub num_threads: u64,
    /// Scanner batch size for reading input fragments. `0` uses upstream's
    /// default.
    pub batch_size: u64,
}

/// Per-call compaction metrics returned via the optional out parameter.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LanceCompactionMetrics {
    /// Number of input fragments that were rewritten and dropped.
    pub fragments_removed: u64,
    /// Number of new fragments produced by the rewrite.
    pub fragments_added: u64,
    /// Total files removed across the operation, including deletion files.
    pub files_removed: u64,
    /// Total files added across the operation; one per new fragment.
    pub files_added: u64,
}

/// Compact the dataset's fragments, committing a new manifest if anything
/// changed.
///
/// Each compaction task merges adjacent small fragments and materializes any
/// deletion files in the process. A clean dataset (no fragment under the
/// target size, no deletions worth materializing) is a no-op: the function
/// returns success with all-zero metrics and the dataset's version is
/// unchanged.
///
/// - `dataset`: Open dataset (mutated; same handle remains valid afterward).
///   Must not be NULL.
/// - `options`: Optional. NULL uses upstream defaults; otherwise each field
///   is treated as an override (`0` keeps the default for that field).
/// - `out_metrics`: Optional. If non-NULL, on success receives the
///   `LanceCompactionMetrics` for this call. On error the slot is untouched.
///
/// Returns 0 on success, -1 on error. Error codes:
/// `LANCE_ERR_INVALID_ARGUMENT` for NULL `dataset` or for numeric overrides
/// that exceed `usize::MAX` on the running target;
/// `LANCE_ERR_COMMIT_CONFLICT` for a concurrent writer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_compact_files(
    dataset: *mut LanceDataset,
    options: *const LanceCompactionOptions,
    out_metrics: *mut LanceCompactionMetrics,
) -> i32 {
    ffi_try!(unsafe { compact_inner(dataset, options, out_metrics) }, neg)
}

unsafe fn compact_inner(
    dataset: *mut LanceDataset,
    options: *const LanceCompactionOptions,
    out_metrics: *mut LanceCompactionMetrics,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }

    // SAFETY: `options` is either NULL (use defaults) or points to a valid
    // `LanceCompactionOptions` for the duration of this call. `resolve` only
    // reads through the pointer.
    let resolved = unsafe { resolve_options(options)? };

    // SAFETY: `dataset` is non-NULL (checked above) and the caller guarantees
    // it points to a live `LanceDataset` not aliased mutably elsewhere.
    let ds = unsafe { &*dataset };
    let metrics = ds.with_mut(|d| block_on(compact_files(d, resolved, None)))?;

    if !out_metrics.is_null() {
        // SAFETY: caller guarantees `out_metrics` (when non-NULL) points to
        // caller-owned, writable storage of size `sizeof(LanceCompactionMetrics)`.
        // We only write on success; on the error paths above the slot stays
        // untouched per the documented contract.
        unsafe {
            *out_metrics = LanceCompactionMetrics {
                fragments_removed: metrics.fragments_removed as u64,
                fragments_added: metrics.fragments_added as u64,
                files_removed: metrics.files_removed as u64,
                files_added: metrics.files_added as u64,
            };
        }
    }
    Ok(0)
}

/// Apply caller-provided overrides onto a default `CompactionOptions`. NULL
/// means "no overrides"; the per-field sentinel and overflow contract are
/// documented on `LanceCompactionOptions`.
unsafe fn resolve_options(options: *const LanceCompactionOptions) -> Result<CompactionOptions> {
    let mut resolved = CompactionOptions::default();
    if options.is_null() {
        return Ok(resolved);
    }

    // SAFETY: `options` is non-NULL (checked above) and the caller guarantees
    // it points to a properly-initialized `LanceCompactionOptions` valid for
    // the duration of this call. We read by shared reference.
    let opts = unsafe { &*options };

    if opts.target_rows_per_fragment > 0 {
        resolved.target_rows_per_fragment =
            u64_to_usize(opts.target_rows_per_fragment, "target_rows_per_fragment")?;
    }
    if opts.max_rows_per_group > 0 {
        resolved.max_rows_per_group = u64_to_usize(opts.max_rows_per_group, "max_rows_per_group")?;
    }
    if opts.max_bytes_per_file > 0 {
        resolved.max_bytes_per_file =
            Some(u64_to_usize(opts.max_bytes_per_file, "max_bytes_per_file")?);
    }
    if opts.num_threads > 0 {
        resolved.num_threads = Some(u64_to_usize(opts.num_threads, "num_threads")?);
    }
    if opts.batch_size > 0 {
        resolved.batch_size = Some(u64_to_usize(opts.batch_size, "batch_size")?);
    }
    Ok(resolved)
}

/// Narrow `u64 -> usize` with an explicit error on overflow (32-bit hosts).
/// Realistic compaction tunings fit in `usize` on every supported target,
/// but a silent `as` cast would wrap on a 32-bit host.
fn u64_to_usize(v: u64, field: &'static str) -> Result<usize> {
    usize::try_from(v).map_err(|_| {
        lance_core::Error::invalid_input_source(
            format!("{field}={v} exceeds usize::MAX on this target").into(),
        )
    })
}

/// Opaque native compaction options with presence-preserving setters.
///
/// A new value starts from `CompactionOptions::default()`. The from-dataset
/// constructor applies the dataset's `lance.compaction.*` manifest config on
/// top of those defaults. Setters then override exactly the fields they name;
/// the binding keeps no shadow copy of which values were overridden.
pub struct LanceCompactionOptionsV2 {
    options: CompactionOptions,
}

/// Opaque owned snapshot of a native `CompactionPlan`.
pub struct LanceCompactionPlan {
    plan: CompactionPlan,
}

/// Opaque owned snapshot of a native `CompactionTask`.
pub struct LanceCompactionTask {
    task: CompactionTask,
}

/// Opaque owned snapshot of a native `RewriteResult`.
pub struct LanceRewriteResult {
    result: RewriteResult,
}

/// Compaction rewrite mode. Values are pinned for the C ABI.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceCompactionMode {
    Reencode = 0,
    TryBinaryCopy = 1,
    ForceBinaryCopy = 2,
}

impl LanceCompactionMode {
    fn from_raw(value: i32) -> Result<CompactionMode> {
        match value {
            0 => Ok(CompactionMode::Reencode),
            1 => Ok(CompactionMode::TryBinaryCopy),
            2 => Ok(CompactionMode::ForceBinaryCopy),
            _ => Err(invalid_input(format!(
                "compaction mode must be 0 (REENCODE), 1 (TRY_BINARY_COPY), or 2 (FORCE_BINARY_COPY); got {value}"
            ))),
        }
    }
}

/// Index row-address remap representation. Values are pinned for the C ABI.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceIndexRemapMode {
    Compact = 0,
    Direct = 1,
}

impl LanceIndexRemapMode {
    fn from_raw(value: i32) -> Result<IndexRemapMode> {
        match value {
            0 => Ok(IndexRemapMode::Compact),
            1 => Ok(IndexRemapMode::Direct),
            _ => Err(invalid_input(format!(
                "index remap mode must be 0 (COMPACT) or 1 (DIRECT); got {value}"
            ))),
        }
    }
}

fn invalid_input(message: impl Into<String>) -> lance_core::Error {
    lance_core::Error::invalid_input(message)
}

unsafe fn options_mut<'a>(
    options: *mut LanceCompactionOptionsV2,
) -> Result<&'a mut CompactionOptions> {
    if options.is_null() {
        return Err(invalid_input("options must not be NULL"));
    }
    Ok(unsafe { &mut (*options).options })
}

unsafe fn options_ref<'a>(
    options: *const LanceCompactionOptionsV2,
) -> Result<&'a CompactionOptions> {
    if options.is_null() {
        return Err(invalid_input("options must not be NULL"));
    }
    Ok(unsafe { &(*options).options })
}

fn optional_usize(has_value: bool, value: u64, field: &'static str) -> Result<Option<usize>> {
    if has_value {
        Ok(Some(u64_to_usize(value, field)?))
    } else {
        Ok(None)
    }
}

fn options_box(options: CompactionOptions) -> *mut LanceCompactionOptionsV2 {
    Box::into_raw(Box::new(LanceCompactionOptionsV2 { options }))
}

fn metrics_from_native(
    metrics: lance::dataset::optimize::CompactionMetrics,
) -> LanceCompactionMetrics {
    LanceCompactionMetrics {
        fragments_removed: metrics.fragments_removed as u64,
        fragments_added: metrics.fragments_added as u64,
        files_removed: metrics.files_removed as u64,
        files_added: metrics.files_added as u64,
    }
}

unsafe fn write_metrics_if_requested(
    out_metrics: *mut LanceCompactionMetrics,
    metrics: lance::dataset::optimize::CompactionMetrics,
) {
    if !out_metrics.is_null() {
        unsafe { ptr::write_unaligned(out_metrics, metrics_from_native(metrics)) };
    }
}

unsafe fn compaction_options_from_dataset(dataset: &Dataset) -> Result<CompactionOptions> {
    CompactionOptions::from_dataset_config(dataset.config())
}

unsafe fn resolved_compaction_options(
    dataset: &Dataset,
    options: *const LanceCompactionOptionsV2,
) -> Result<CompactionOptions> {
    if options.is_null() {
        unsafe { compaction_options_from_dataset(dataset) }
    } else {
        Ok(unsafe { options_ref(options) }?.clone())
    }
}

/// Create compaction options with native `CompactionOptions::default()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_new() -> *mut LanceCompactionOptionsV2 {
    ffi_try!(
        (|| -> Result<*mut LanceCompactionOptionsV2> {
            Ok(options_box(CompactionOptions::default()))
        })(),
        null
    )
}

/// Create compaction options from the dataset's native compaction config.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_from_dataset(
    dataset: *const LanceDataset,
) -> *mut LanceCompactionOptionsV2 {
    ffi_try!(
        (|| -> Result<*mut LanceCompactionOptionsV2> {
            if dataset.is_null() {
                return Err(invalid_input("dataset must not be NULL"));
            }
            let dataset = unsafe { &*dataset }.snapshot();
            Ok(options_box(unsafe {
                compaction_options_from_dataset(&dataset)
            }?))
        })(),
        null
    )
}

/// Clone an owned compaction options value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_clone(
    options: *const LanceCompactionOptionsV2,
) -> *mut LanceCompactionOptionsV2 {
    ffi_try!(
        (|| -> Result<*mut LanceCompactionOptionsV2> {
            Ok(options_box(unsafe { options_ref(options) }?.clone()))
        })(),
        null
    )
}

/// Free compaction options. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_free(options: *mut LanceCompactionOptionsV2) {
    if !options.is_null() {
        unsafe { drop(Box::from_raw(options)) };
    }
}

/// Set `target_rows_per_fragment`, including explicit zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_target_rows_per_fragment(
    options: *mut LanceCompactionOptionsV2,
    value: u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.target_rows_per_fragment =
                u64_to_usize(value, "target_rows_per_fragment")?;
            Ok(0)
        })(),
        neg
    )
}

/// Set `max_rows_per_group`, including explicit zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_max_rows_per_group(
    options: *mut LanceCompactionOptionsV2,
    value: u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.max_rows_per_group =
                u64_to_usize(value, "max_rows_per_group")?;
            Ok(0)
        })(),
        neg
    )
}

/// Set optional `max_bytes_per_file`; `has_value == false` means native None.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_max_bytes_per_file(
    options: *mut LanceCompactionOptionsV2,
    has_value: bool,
    value: u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.max_bytes_per_file =
                optional_usize(has_value, value, "max_bytes_per_file")?;
            Ok(0)
        })(),
        neg
    )
}

/// Set `materialize_deletions`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_materialize_deletions(
    options: *mut LanceCompactionOptionsV2,
    value: bool,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.materialize_deletions = value;
            Ok(0)
        })(),
        neg
    )
}

/// Set the deletion materialization threshold.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_materialize_deletions_threshold(
    options: *mut LanceCompactionOptionsV2,
    value: f32,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.materialize_deletions_threshold = value;
            Ok(0)
        })(),
        neg
    )
}

/// Set optional `num_threads`; explicit zero is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_num_threads(
    options: *mut LanceCompactionOptionsV2,
    has_value: bool,
    value: u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.num_threads =
                optional_usize(has_value, value, "num_threads")?;
            Ok(0)
        })(),
        neg
    )
}

/// Set optional scanner `batch_size`; explicit zero is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_batch_size(
    options: *mut LanceCompactionOptionsV2,
    has_value: bool,
    value: u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.batch_size =
                optional_usize(has_value, value, "batch_size")?;
            Ok(0)
        })(),
        neg
    )
}

/// Set optional scanner `io_buffer_size`; explicit zero is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_io_buffer_size(
    options: *mut LanceCompactionOptionsV2,
    has_value: bool,
    value: u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.io_buffer_size =
                if has_value { Some(value) } else { None };
            Ok(0)
        })(),
        neg
    )
}

/// Set `defer_index_remap`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_defer_index_remap(
    options: *mut LanceCompactionOptionsV2,
    value: bool,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.defer_index_remap = value;
            Ok(0)
        })(),
        neg
    )
}

/// Set `index_remap_mode`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_index_remap_mode(
    options: *mut LanceCompactionOptionsV2,
    mode: i32,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let mode = LanceIndexRemapMode::from_raw(mode)?;
            unsafe { options_mut(options) }?.index_remap_mode = mode;
            Ok(0)
        })(),
        neg
    )
}

/// Set optional `compaction_mode`; `has_value == false` means native None.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_compaction_mode(
    options: *mut LanceCompactionOptionsV2,
    has_value: bool,
    mode: i32,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let mode = if has_value {
                Some(LanceCompactionMode::from_raw(mode)?)
            } else {
                None
            };
            unsafe { options_mut(options) }?.compaction_mode = mode;
            Ok(0)
        })(),
        neg
    )
}

/// Set optional binary-copy read batch bytes; explicit zero is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_binary_copy_read_batch_bytes(
    options: *mut LanceCompactionOptionsV2,
    has_value: bool,
    value: u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.binary_copy_read_batch_bytes =
                optional_usize(has_value, value, "binary_copy_read_batch_bytes")?;
            Ok(0)
        })(),
        neg
    )
}

/// Set optional `max_source_fragments`; explicit zero is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_max_source_fragments(
    options: *mut LanceCompactionOptionsV2,
    has_value: bool,
    value: u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.max_source_fragments =
                optional_usize(has_value, value, "max_source_fragments")?;
            Ok(0)
        })(),
        neg
    )
}

/// Set optional `max_source_rows`; explicit zero is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_max_source_rows(
    options: *mut LanceCompactionOptionsV2,
    has_value: bool,
    value: u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.max_source_rows =
                optional_usize(has_value, value, "max_source_rows")?;
            Ok(0)
        })(),
        neg
    )
}

/// Set optional `max_source_bytes`; explicit zero is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_max_source_bytes(
    options: *mut LanceCompactionOptionsV2,
    has_value: bool,
    value: u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.max_source_bytes =
                if has_value { Some(value) } else { None };
            Ok(0)
        })(),
        neg
    )
}

/// Replace excluded fragment IDs. NULL with `count == 0` is the empty list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_excluded_fragment_ids(
    options: *mut LanceCompactionOptionsV2,
    fragment_ids: *const u32,
    count: usize,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if count > 0 && fragment_ids.is_null() {
                return Err(invalid_input(format!(
                    "fragment_ids must not be NULL when count = {count}"
                )));
            }
            if count > isize::MAX as usize / std::mem::size_of::<u32>() {
                return Err(invalid_input(format!(
                    "count {count} exceeds the maximum addressable u32 slice length"
                )));
            }
            let ids = if count == 0 {
                Vec::new()
            } else {
                unsafe { slice::from_raw_parts(fragment_ids, count) }.to_vec()
            };
            unsafe { options_mut(options) }?.excluded_fragment_ids = ids;
            Ok(0)
        })(),
        neg
    )
}

/// Set optional `max_overlays_per_fragment`; explicit zero is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_max_overlays_per_fragment(
    options: *mut LanceCompactionOptionsV2,
    has_value: bool,
    value: u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.max_overlays_per_fragment =
                optional_usize(has_value, value, "max_overlays_per_fragment")?;
            Ok(0)
        })(),
        neg
    )
}

/// Set the optional data storage version selector. NULL means native None.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_data_storage_version(
    options: *mut LanceCompactionOptionsV2,
    selector: *const c_char,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let selector = unsafe { helpers::parse_c_string(selector) }?;
            let version = selector
                .map(str::parse::<LanceFileVersion>)
                .transpose()
                .map_err(|error| {
                    invalid_input(format!("invalid data_storage_version selector: {error}"))
                })?;
            unsafe { options_mut(options) }?.data_storage_version = version;
            Ok(0)
        })(),
        neg
    )
}

/// Set optional transaction properties for a later commit.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_options_v2_set_transaction_properties(
    options: *mut LanceCompactionOptionsV2,
    properties: *const LanceTransactionProperties,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let properties = if properties.is_null() {
                None
            } else {
                Some(Arc::new(unsafe {
                    parse_transaction_properties(&*properties)?
                }))
            };
            unsafe { options_mut(options) }?.transaction_properties = properties;
            Ok(0)
        })(),
        neg
    )
}

/// Compact files using native `CompactionOptions`.
///
/// NULL options select `CompactionOptions::from_dataset_config`. The output
/// metrics slot is written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_compact_files_v2(
    dataset: *mut LanceDataset,
    options: *const LanceCompactionOptionsV2,
    out_metrics: *mut LanceCompactionMetrics,
) -> i32 {
    ffi_try!(
        unsafe { compact_files_v2_inner(dataset, options, out_metrics) },
        neg
    )
}

unsafe fn compact_files_v2_inner(
    dataset: *mut LanceDataset,
    options: *const LanceCompactionOptionsV2,
    out_metrics: *mut LanceCompactionMetrics,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(invalid_input("dataset must not be NULL"));
    }
    let dataset = unsafe { &*dataset };
    let snapshot = dataset.snapshot();
    let options = unsafe { resolved_compaction_options(&snapshot, options) }?;
    let metrics = dataset.with_mut(|dataset| block_on(compact_files(dataset, options, None)))?;
    unsafe { write_metrics_if_requested(out_metrics, metrics) };
    Ok(0)
}

/// Plan compaction against this dataset snapshot.
///
/// NULL options select `CompactionOptions::from_dataset_config`. The returned
/// plan owns a native snapshot and remains valid after this dataset handle is
/// closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_plan_compaction(
    dataset: *const LanceDataset,
    options: *const LanceCompactionOptionsV2,
) -> *mut LanceCompactionPlan {
    ffi_try!(
        (|| -> Result<*mut LanceCompactionPlan> {
            if dataset.is_null() {
                return Err(invalid_input("dataset must not be NULL"));
            }
            let snapshot = unsafe { &*dataset }.snapshot();
            let options = unsafe { resolved_compaction_options(&snapshot, options) }?;
            let plan = block_on(plan_compaction(&snapshot, &options))?;
            Ok(Box::into_raw(Box::new(LanceCompactionPlan { plan })))
        })(),
        null
    )
}

/// Return the dataset version read by the plan.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_plan_read_version(
    plan: *const LanceCompactionPlan,
) -> u64 {
    ffi_try!(
        (|| -> Result<u64> {
            if plan.is_null() {
                return Err(invalid_input("plan must not be NULL"));
            }
            Ok(unsafe { &*plan }.plan.read_version())
        })(),
        0
    )
}

/// Return the number of tasks in the plan.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_plan_task_count(
    plan: *const LanceCompactionPlan,
) -> usize {
    ffi_try!(
        (|| -> Result<usize> {
            if plan.is_null() {
                return Err(invalid_input("plan must not be NULL"));
            }
            Ok(unsafe { &*plan }.plan.num_tasks())
        })(),
        0
    )
}

/// Return an owned snapshot of the task at `index`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_plan_task_at(
    plan: *const LanceCompactionPlan,
    index: usize,
) -> *mut LanceCompactionTask {
    ffi_try!(
        (|| -> Result<*mut LanceCompactionTask> {
            if plan.is_null() {
                return Err(invalid_input("plan must not be NULL"));
            }
            let plan = unsafe { &*plan };
            let task = plan.plan.compaction_tasks().nth(index).ok_or_else(|| {
                invalid_input(format!(
                    "task index {index} is out of range; task_count = {}",
                    plan.plan.num_tasks()
                ))
            })?;
            Ok(Box::into_raw(Box::new(LanceCompactionTask { task })))
        })(),
        null
    )
}

/// Clone the plan's effective native options, including the resolved storage
/// version.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_plan_options_clone(
    plan: *const LanceCompactionPlan,
) -> *mut LanceCompactionOptionsV2 {
    ffi_try!(
        (|| -> Result<*mut LanceCompactionOptionsV2> {
            if plan.is_null() {
                return Err(invalid_input("plan must not be NULL"));
            }
            Ok(options_box(unsafe { &*plan }.plan.options().clone()))
        })(),
        null
    )
}

fn owned_json<T: serde::Serialize>(value: &T, context: &str) -> Result<*const c_char> {
    let json = serde_json::to_string(value).map_err(|error| {
        lance_core::Error::internal(format!("failed to serialize {context}: {error}"))
    })?;
    owned_c_string(json, &format!("{context} JSON"))
}

fn parse_json<T: serde::de::DeserializeOwned>(json: *const c_char, context: &str) -> Result<T> {
    let json = unsafe { helpers::parse_c_string(json) }?
        .ok_or_else(|| invalid_input(format!("{context} JSON must not be NULL")))?;
    serde_json::from_str(json)
        .map_err(|error| invalid_input(format!("invalid {context} JSON: {error}")))
}

/// Serialize the plan using native serde. Caller frees with
/// `lance_free_string()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_plan_json(
    plan: *const LanceCompactionPlan,
) -> *const c_char {
    ffi_try!(
        (|| -> Result<*const c_char> {
            if plan.is_null() {
                return Err(invalid_input("plan must not be NULL"));
            }
            owned_json(&unsafe { &*plan }.plan, "compaction plan")
        })(),
        null
    )
}

/// Decode a compaction plan from native serde JSON.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_plan_from_json(
    json: *const c_char,
) -> *mut LanceCompactionPlan {
    ffi_try!(
        (|| -> Result<*mut LanceCompactionPlan> {
            Ok(Box::into_raw(Box::new(LanceCompactionPlan {
                plan: parse_json(json, "compaction plan")?,
            })))
        })(),
        null
    )
}

/// Free a compaction plan. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_plan_close(plan: *mut LanceCompactionPlan) {
    if !plan.is_null() {
        unsafe { drop(Box::from_raw(plan)) };
    }
}

/// Return the task's planned dataset version.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_task_read_version(
    task: *const LanceCompactionTask,
) -> u64 {
    ffi_try!(
        (|| -> Result<u64> {
            if task.is_null() {
                return Err(invalid_input("task must not be NULL"));
            }
            Ok(unsafe { &*task }.task.read_version)
        })(),
        0
    )
}

/// Return the number of source fragments in the task.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_task_fragment_count(
    task: *const LanceCompactionTask,
) -> usize {
    ffi_try!(
        (|| -> Result<usize> {
            if task.is_null() {
                return Err(invalid_input("task must not be NULL"));
            }
            Ok(unsafe { &*task }.task.task.fragments.len())
        })(),
        0
    )
}

/// Serialize one source fragment's native metadata as JSON.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_task_fragment_metadata_json_at(
    task: *const LanceCompactionTask,
    index: usize,
) -> *const c_char {
    ffi_try!(
        (|| -> Result<*const c_char> {
            if task.is_null() {
                return Err(invalid_input("task must not be NULL"));
            }
            let task = unsafe { &*task };
            let fragment = task.task.task.fragments.get(index).ok_or_else(|| {
                invalid_input(format!(
                    "fragment index {index} is out of range; fragment_count = {}",
                    task.task.task.fragments.len()
                ))
            })?;
            owned_json(fragment, "compaction task fragment metadata")
        })(),
        null
    )
}

/// Clone the task's effective native options.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_task_options_clone(
    task: *const LanceCompactionTask,
) -> *mut LanceCompactionOptionsV2 {
    ffi_try!(
        (|| -> Result<*mut LanceCompactionOptionsV2> {
            if task.is_null() {
                return Err(invalid_input("task must not be NULL"));
            }
            Ok(options_box(unsafe { &*task }.task.options.clone()))
        })(),
        null
    )
}

/// Execute a task against the dataset at its planned read version.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_task_execute(
    dataset: *const LanceDataset,
    task: *const LanceCompactionTask,
) -> *mut LanceRewriteResult {
    ffi_try!(
        (|| -> Result<*mut LanceRewriteResult> {
            if dataset.is_null() || task.is_null() {
                return Err(invalid_input("dataset and task must not be NULL"));
            }
            let snapshot = unsafe { &*dataset }.snapshot();
            let task = unsafe { &*task };
            let result = block_on(task.task.execute(&snapshot))?;
            Ok(Box::into_raw(Box::new(LanceRewriteResult { result })))
        })(),
        null
    )
}

/// Serialize the task using native serde.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_task_json(
    task: *const LanceCompactionTask,
) -> *const c_char {
    ffi_try!(
        (|| -> Result<*const c_char> {
            if task.is_null() {
                return Err(invalid_input("task must not be NULL"));
            }
            owned_json(&unsafe { &*task }.task, "compaction task")
        })(),
        null
    )
}

/// Decode a compaction task from native serde JSON.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_task_from_json(
    json: *const c_char,
) -> *mut LanceCompactionTask {
    ffi_try!(
        (|| -> Result<*mut LanceCompactionTask> {
            Ok(Box::into_raw(Box::new(LanceCompactionTask {
                task: parse_json(json, "compaction task")?,
            })))
        })(),
        null
    )
}

/// Free a compaction task. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_compaction_task_close(task: *mut LanceCompactionTask) {
    if !task.is_null() {
        unsafe { drop(Box::from_raw(task)) };
    }
}

/// Return the rewrite result's read version.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_result_read_version(
    result: *const LanceRewriteResult,
) -> u64 {
    ffi_try!(
        (|| -> Result<u64> {
            if result.is_null() {
                return Err(invalid_input("result must not be NULL"));
            }
            Ok(unsafe { &*result }.result.read_version)
        })(),
        0
    )
}

/// Copy the rewrite metrics. The slot is written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_result_metrics(
    result: *const LanceRewriteResult,
    out_metrics: *mut LanceCompactionMetrics,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if result.is_null() || out_metrics.is_null() {
                return Err(invalid_input("result and out_metrics must not be NULL"));
            }
            unsafe {
                ptr::write_unaligned(
                    out_metrics,
                    metrics_from_native((*result).result.metrics.clone()),
                )
            };
            Ok(0)
        })(),
        neg
    )
}

/// Return the number of original fragments replaced by this result.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_result_original_fragment_count(
    result: *const LanceRewriteResult,
) -> usize {
    ffi_try!(
        (|| -> Result<usize> {
            if result.is_null() {
                return Err(invalid_input("result must not be NULL"));
            }
            Ok(unsafe { &*result }.result.original_fragments.len())
        })(),
        0
    )
}

/// Return the number of new fragments produced by this result.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_result_new_fragment_count(
    result: *const LanceRewriteResult,
) -> usize {
    ffi_try!(
        (|| -> Result<usize> {
            if result.is_null() {
                return Err(invalid_input("result must not be NULL"));
            }
            Ok(unsafe { &*result }.result.new_fragments.len())
        })(),
        0
    )
}

/// Serialize one original fragment's native metadata as JSON.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_result_original_fragment_metadata_json_at(
    result: *const LanceRewriteResult,
    index: usize,
) -> *const c_char {
    ffi_try!(
        (|| -> Result<*const c_char> {
            if result.is_null() {
                return Err(invalid_input("result must not be NULL"));
            }
            let result = unsafe { &*result };
            let fragment = result.result.original_fragments.get(index).ok_or_else(|| {
                invalid_input(format!(
                    "original fragment index {index} is out of range; count = {}",
                    result.result.original_fragments.len()
                ))
            })?;
            owned_json(fragment, "rewrite result original fragment metadata")
        })(),
        null
    )
}

/// Serialize one new fragment's native metadata as JSON.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_result_new_fragment_metadata_json_at(
    result: *const LanceRewriteResult,
    index: usize,
) -> *const c_char {
    ffi_try!(
        (|| -> Result<*const c_char> {
            if result.is_null() {
                return Err(invalid_input("result must not be NULL"));
            }
            let result = unsafe { &*result };
            let fragment = result.result.new_fragments.get(index).ok_or_else(|| {
                invalid_input(format!(
                    "new fragment index {index} is out of range; count = {}",
                    result.result.new_fragments.len()
                ))
            })?;
            owned_json(fragment, "rewrite result new fragment metadata")
        })(),
        null
    )
}

/// Serialize the rewrite result using native serde.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_result_json(
    result: *const LanceRewriteResult,
) -> *const c_char {
    ffi_try!(
        (|| -> Result<*const c_char> {
            if result.is_null() {
                return Err(invalid_input("result must not be NULL"));
            }
            owned_json(&unsafe { &*result }.result, "rewrite result")
        })(),
        null
    )
}

/// Decode a rewrite result from native serde JSON.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_result_from_json(
    json: *const c_char,
) -> *mut LanceRewriteResult {
    ffi_try!(
        (|| -> Result<*mut LanceRewriteResult> {
            Ok(Box::into_raw(Box::new(LanceRewriteResult {
                result: parse_json(json, "rewrite result")?,
            })))
        })(),
        null
    )
}

/// Free a rewrite result. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_result_close(result: *mut LanceRewriteResult) {
    if !result.is_null() {
        unsafe { drop(Box::from_raw(result)) };
    }
}

/// Commit rewrite results through native `commit_compaction`.
///
/// NULL options select `CompactionOptions::default()`, matching the native
/// distributed commit API. The metrics slot is written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_commit_compaction(
    dataset: *mut LanceDataset,
    results: *const *const LanceRewriteResult,
    result_count: usize,
    options: *const LanceCompactionOptionsV2,
    out_metrics: *mut LanceCompactionMetrics,
) -> i32 {
    ffi_try!(
        unsafe { commit_compaction_inner(dataset, results, result_count, options, out_metrics) },
        neg
    )
}

unsafe fn commit_compaction_inner(
    dataset: *mut LanceDataset,
    results: *const *const LanceRewriteResult,
    result_count: usize,
    options: *const LanceCompactionOptionsV2,
    out_metrics: *mut LanceCompactionMetrics,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(invalid_input("dataset must not be NULL"));
    }
    if result_count > 0 && results.is_null() {
        return Err(invalid_input(format!(
            "results must not be NULL when result_count = {result_count}"
        )));
    }
    if result_count > isize::MAX as usize / std::mem::size_of::<*const LanceRewriteResult>() {
        return Err(invalid_input(format!(
            "result_count {result_count} exceeds the maximum addressable pointer slice length"
        )));
    }

    let mut parsed = Vec::with_capacity(result_count);
    for index in 0..result_count {
        let result = unsafe { *results.add(index) };
        if result.is_null() {
            return Err(invalid_input(format!("results[{index}] must not be NULL")));
        }
        parsed.push(unsafe { &*result }.result.clone());
    }

    let options = if options.is_null() {
        CompactionOptions::default()
    } else {
        unsafe { options_ref(options) }?.clone()
    };
    let dataset = unsafe { &*dataset };
    let metrics = dataset.with_mut(|dataset| {
        block_on(commit_compaction(
            dataset,
            parsed,
            Arc::new(DatasetIndexRemapperOptions::default()),
            &options,
        ))
    })?;
    unsafe { write_metrics_if_requested(out_metrics, metrics) };
    Ok(0)
}
