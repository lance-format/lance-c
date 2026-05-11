// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Compaction C API: rewrite small/deleted-heavy fragments into larger ones,
//! committing a new manifest. Operates as a no-op (no version bump) when no
//! fragments need compacting.
//!
//! Mutates the dataset in place under an exclusive write lock; existing
//! scanners that already cloned the inner Arc keep their pre-compaction
//! snapshot view.

use lance::dataset::optimize::{CompactionOptions, compact_files};
use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::runtime::block_on;

/// Tunable parameters for `lance_dataset_compact_files`. Pass NULL to use the
/// upstream defaults. Each numeric field uses `0` as a "keep upstream default"
/// sentinel; explicit overrides are forwarded after a `usize` range check.
///
/// The struct is `#[repr(C)]` and ABI-stable within a minor version; new
/// fields can be appended without breaking existing callers.
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
        return Err(lance_core::Error::InvalidInput {
            source: "dataset must not be NULL".into(),
            location: snafu::location!(),
        });
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
    usize::try_from(v).map_err(|_| lance_core::Error::InvalidInput {
        source: format!("{field}={v} exceeds usize::MAX on this target").into(),
        location: snafu::location!(),
    })
}
