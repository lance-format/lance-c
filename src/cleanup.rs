// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Dataset cleanup C API: native manual version cleanup and read-only
//! explanations.
//!
//! Policy and operation inputs are copied into owned Rust values before any
//! asynchronous work. Explanations are copied into an owned snapshot so their
//! stats, candidate paths, branches and warnings remain readable independently
//! of the dataset handle.

use std::ffi::{CString, c_char};
use std::ptr;

use chrono::{DateTime, Utc};
use lance::Dataset;
use lance::dataset::cleanup::{
    CleanupExplanation as NativeCleanupExplanation, CleanupFileKind, CleanupOperation,
    CleanupPolicy, CleanupPolicyBuilder, RemovalStats,
};
use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
use crate::helpers;
use crate::runtime::block_on;

/// Native cleanup policy fields with explicit presence for every optional
/// selector. A zero-initialized value leaves every optional field unset, so
/// native defaults apply (including `error_if_tagged_old_versions = true`).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct LanceCleanupPolicy {
    /// Non-zero selects `before_timestamp_seconds` + `before_timestamp_nanos`.
    pub has_before_timestamp: i32,
    /// Whole seconds since the Unix epoch.
    pub before_timestamp_seconds: i64,
    /// Nanosecond component in `[0, 999_999_999]`.
    pub before_timestamp_nanos: u32,
    /// Non-zero selects `before_version`.
    pub has_before_version: i32,
    /// Remove versions strictly older than this version number.
    pub before_version: u64,
    /// Exact version set. NULL means absent; non-NULL with `num_versions == 0`
    /// is an explicit empty selection and is rejected by the native builder.
    pub versions: *const u64,
    /// Number of entries in `versions`.
    pub num_versions: usize,
    /// Non-zero selects `retain_versions`.
    pub has_retain_versions: i32,
    /// Keep this many native version refs. Zero is rejected by Lance.
    pub retain_versions: u64,
    /// Non-zero selects `delete_unverified`.
    pub has_delete_unverified: i32,
    /// Whether recent unverified files may be removed.
    pub delete_unverified: bool,
    /// Non-zero selects `error_if_tagged_old_versions`.
    pub has_error_if_tagged_old_versions: i32,
    /// Whether matching tagged versions produce a native cleanup error.
    pub error_if_tagged_old_versions: bool,
    /// Non-zero selects `clean_referenced_branches`.
    pub has_clean_referenced_branches: i32,
    /// Whether cleanup cascades into referenced child branches.
    pub clean_referenced_branches: bool,
    /// Non-zero selects the policy-level `delete_rate_limit`.
    pub has_delete_rate_limit: i32,
    /// Maximum delete requests per second. Zero is rejected by Lance.
    pub delete_rate_limit: u64,
}

/// Optional native `CleanupOperation` overrides.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct LanceCleanupOperationOptions {
    /// Non-zero selects `delete_concurrency`.
    pub has_delete_concurrency: i32,
    /// Maximum in-flight native file deletions.
    pub delete_concurrency: u64,
    /// Non-zero selects the operation-level `delete_rate_limit` override.
    pub has_delete_rate_limit: i32,
    /// Maximum delete requests per second. Zero is rejected by Lance.
    pub delete_rate_limit: u64,
    /// Non-zero selects `max_candidate_files`.
    pub has_max_candidate_files: i32,
    /// Candidate limit used by explanations. Zero is valid.
    pub max_candidate_files: u64,
}

/// Relative older-than cleanup inputs for `Dataset::cleanup_old_versions`.
///
/// A zero-initialized value uses a zero-length window and native optional
/// defaults for both boolean arguments.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct LanceCleanupOldVersionsOptions {
    /// Whole-second component of the native `chrono::TimeDelta` window.
    pub older_than_seconds: i64,
    /// Signed normalized nanosecond component in
    /// `[-999_999_999, 999_999_999]`.
    pub older_than_nanos: i32,
    /// Non-zero selects `delete_unverified`.
    pub has_delete_unverified: i32,
    /// Native `delete_unverified` value when present.
    pub delete_unverified: bool,
    /// Non-zero selects `error_if_tagged_old_versions`.
    pub has_error_if_tagged_old_versions: i32,
    /// Native `error_if_tagged_old_versions` value when present.
    pub error_if_tagged_old_versions: bool,
}

/// Copied native `RemovalStats`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LanceRemovalStats {
    pub bytes_removed: u64,
    pub old_versions: u64,
    pub data_files_removed: u64,
    pub transaction_files_removed: u64,
    pub index_files_removed: u64,
    pub deletion_files_removed: u64,
    pub failed_deletes: u64,
}

/// Owned snapshot of a native `CleanupExplanation`.
pub struct LanceCleanupExplanation {
    read_version: u64,
    stats: LanceRemovalStats,
    candidate_files: Vec<CleanupCandidateFile>,
    candidate_files_truncated: bool,
    candidate_file_limit: u64,
    referenced_branches: Vec<CleanupReferencedBranch>,
    warnings: Vec<CString>,
}

struct CleanupCandidateFile {
    path: CString,
    kind: i32,
    unverified: bool,
    size_bytes: u64,
}

struct CleanupReferencedBranch {
    name: CString,
    referenced_version: u64,
    cleanup_candidate: bool,
}

struct CleanupPolicyInput {
    before_timestamp: Option<DateTime<Utc>>,
    before_version: Option<u64>,
    versions: Option<Vec<u64>>,
    retain_versions: Option<usize>,
    delete_unverified: Option<bool>,
    error_if_tagged_old_versions: Option<bool>,
    clean_referenced_branches: Option<bool>,
    delete_rate_limit: Option<u64>,
}

#[derive(Clone, Copy, Default)]
struct CleanupOperationInput {
    delete_concurrency: Option<usize>,
    delete_rate_limit: Option<u64>,
    max_candidate_files: Option<usize>,
}

fn invalid(message: impl Into<String>) -> lance_core::Error {
    lance_core::Error::invalid_input_source(message.into().into())
}

fn internal(message: impl Into<String>) -> lance_core::Error {
    lance_core::Error::internal(message.into())
}

fn u64_to_usize(value: u64, field: &'static str) -> Result<usize> {
    usize::try_from(value)
        .map_err(|_| invalid(format!("{field}={value} exceeds usize::MAX on this target")))
}

fn c_string(value: String, context: &str) -> Result<CString> {
    CString::new(value).map_err(|_| invalid(format!("{context} contains an embedded NUL byte")))
}

fn cleanup_file_kind_code(kind: CleanupFileKind) -> i32 {
    match kind {
        CleanupFileKind::Manifest => 0,
        CleanupFileKind::Data => 1,
        CleanupFileKind::Transaction => 2,
        CleanupFileKind::Index => 3,
        CleanupFileKind::Deletion => 4,
        CleanupFileKind::TemporaryManifest => 5,
    }
}

fn bool_code(value: bool) -> i32 {
    if value { 1 } else { 0 }
}

impl From<RemovalStats> for LanceRemovalStats {
    fn from(stats: RemovalStats) -> Self {
        Self {
            bytes_removed: stats.bytes_removed,
            old_versions: stats.old_versions,
            data_files_removed: stats.data_files_removed,
            transaction_files_removed: stats.transaction_files_removed,
            index_files_removed: stats.index_files_removed,
            deletion_files_removed: stats.deletion_files_removed,
            failed_deletes: stats.failed_deletes,
        }
    }
}

impl LanceCleanupExplanation {
    fn from_native(explanation: NativeCleanupExplanation) -> Result<Self> {
        let candidate_files = explanation
            .candidate_files
            .into_iter()
            .map(|file| {
                Ok(CleanupCandidateFile {
                    path: c_string(file.path, "cleanup candidate path")?,
                    kind: cleanup_file_kind_code(file.kind),
                    unverified: file.unverified,
                    size_bytes: file.size_bytes,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        let referenced_branches = explanation
            .referenced_branches
            .into_iter()
            .map(|branch| {
                Ok(CleanupReferencedBranch {
                    name: c_string(branch.name, "cleanup referenced branch name")?,
                    referenced_version: branch.referenced_version,
                    cleanup_candidate: branch.cleanup_candidate,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        let warnings = explanation
            .warnings
            .into_iter()
            .map(|warning| c_string(warning, "cleanup explanation warning"))
            .collect::<Result<Vec<_>>>()?;

        Ok(Self {
            read_version: explanation.read_version,
            stats: explanation.stats.into(),
            candidate_files,
            candidate_files_truncated: explanation.candidate_files_truncated,
            candidate_file_limit: u64::try_from(explanation.candidate_file_limit)
                .map_err(|_| internal("cleanup candidate_file_limit exceeds u64::MAX"))?,
            referenced_branches,
            warnings,
        })
    }
}

unsafe fn policy_input(policy: *const LanceCleanupPolicy) -> Result<CleanupPolicyInput> {
    if policy.is_null() {
        return Err(invalid("policy must not be NULL"));
    }
    // SAFETY: the caller guarantees a non-NULL policy points to a valid
    // `LanceCleanupPolicy` for this call. Every borrowed value is copied
    // before returning.
    let raw = unsafe { &*policy };

    let before_timestamp = if raw.has_before_timestamp != 0 {
        if raw.before_timestamp_nanos >= 1_000_000_000 {
            return Err(invalid(format!(
                "before_timestamp_nanos={} must be in [0, 999999999]",
                raw.before_timestamp_nanos
            )));
        }
        Some(
            DateTime::<Utc>::from_timestamp(
                raw.before_timestamp_seconds,
                raw.before_timestamp_nanos,
            )
            .ok_or_else(|| {
                invalid(format!(
                    "before_timestamp_seconds={} is outside the chrono DateTime range",
                    raw.before_timestamp_seconds
                ))
            })?,
        )
    } else {
        None
    };

    let versions = if raw.versions.is_null() {
        None
    } else {
        // SAFETY: the caller guarantees that a non-NULL versions pointer
        // references `num_versions` readable u64 values. Copy them so no C
        // memory is borrowed across asynchronous execution.
        Some(unsafe { std::slice::from_raw_parts(raw.versions, raw.num_versions) }.to_vec())
    };
    let before_version = (raw.has_before_version != 0).then_some(raw.before_version);
    let retain_versions = if raw.has_retain_versions != 0 {
        Some(u64_to_usize(raw.retain_versions, "retain_versions")?)
    } else {
        None
    };
    if before_version.is_some() && retain_versions.is_some() {
        return Err(invalid(
            "before_version and retain_versions both set the native before_version selector; set only one",
        ));
    }

    Ok(CleanupPolicyInput {
        before_timestamp,
        before_version,
        versions,
        retain_versions,
        delete_unverified: (raw.has_delete_unverified != 0).then_some(raw.delete_unverified),
        error_if_tagged_old_versions: (raw.has_error_if_tagged_old_versions != 0)
            .then_some(raw.error_if_tagged_old_versions),
        clean_referenced_branches: (raw.has_clean_referenced_branches != 0)
            .then_some(raw.clean_referenced_branches),
        delete_rate_limit: (raw.has_delete_rate_limit != 0).then_some(raw.delete_rate_limit),
    })
}

unsafe fn operation_input(
    options: *const LanceCleanupOperationOptions,
) -> Result<CleanupOperationInput> {
    if options.is_null() {
        return Ok(CleanupOperationInput::default());
    }
    // SAFETY: the caller guarantees a non-NULL options pointer is valid for
    // this call. All values are copied before returning.
    let raw = unsafe { &*options };
    Ok(CleanupOperationInput {
        delete_concurrency: if raw.has_delete_concurrency != 0 {
            Some(u64_to_usize(raw.delete_concurrency, "delete_concurrency")?)
        } else {
            None
        },
        delete_rate_limit: (raw.has_delete_rate_limit != 0).then_some(raw.delete_rate_limit),
        max_candidate_files: if raw.has_max_candidate_files != 0 {
            Some(u64_to_usize(
                raw.max_candidate_files,
                "max_candidate_files",
            )?)
        } else {
            None
        },
    })
}

async fn build_policy(dataset: &Dataset, input: CleanupPolicyInput) -> Result<CleanupPolicy> {
    let mut builder = CleanupPolicyBuilder::default();
    if let Some(timestamp) = input.before_timestamp {
        builder = builder.before_timestamp(timestamp);
    }
    if let Some(retain_versions) = input.retain_versions {
        builder = builder.retain_n_versions(dataset, retain_versions).await?;
    }
    if let Some(delete_unverified) = input.delete_unverified {
        builder = builder.delete_unverified(delete_unverified);
    }
    if let Some(error_if_tagged_old_versions) = input.error_if_tagged_old_versions {
        builder = builder.error_if_tagged_old_versions(error_if_tagged_old_versions);
    }
    if let Some(clean_referenced_branches) = input.clean_referenced_branches {
        builder = builder.clean_referenced_branches(clean_referenced_branches);
    }
    if let Some(versions) = input.versions {
        builder = builder.versions(versions)?;
    }
    if let Some(delete_rate_limit) = input.delete_rate_limit {
        builder = builder.delete_rate_limit(delete_rate_limit)?;
    }
    let mut policy = builder.build();
    if let Some(before_version) = input.before_version {
        policy.before_version = Some(before_version);
    }
    Ok(policy)
}

fn build_operation<'a>(
    dataset: &'a Dataset,
    policy: CleanupPolicy,
    input: CleanupOperationInput,
) -> Result<CleanupOperation<'a>> {
    let mut operation = dataset.cleanup(policy);
    if let Some(delete_concurrency) = input.delete_concurrency {
        operation = operation.with_delete_concurrency(delete_concurrency)?;
    }
    if let Some(delete_rate_limit) = input.delete_rate_limit {
        operation = operation.with_delete_rate_limit(delete_rate_limit)?;
    }
    if let Some(max_candidate_files) = input.max_candidate_files {
        operation = operation.with_max_candidate_files(max_candidate_files);
    }
    Ok(operation)
}

unsafe fn dataset_snapshot(dataset: *const LanceDataset) -> Result<std::sync::Arc<Dataset>> {
    if dataset.is_null() {
        return Err(invalid("dataset must not be NULL"));
    }
    Ok(unsafe { &*dataset }.snapshot())
}

unsafe fn explanation_snapshot<'a>(
    explanation: *const LanceCleanupExplanation,
) -> Result<&'a LanceCleanupExplanation> {
    if explanation.is_null() {
        return Err(invalid("explanation must not be NULL"));
    }
    Ok(unsafe { &*explanation })
}

unsafe fn candidate_at<'a>(
    explanation: *const LanceCleanupExplanation,
    index: usize,
) -> Result<&'a CleanupCandidateFile> {
    let explanation = unsafe { explanation_snapshot(explanation)? };
    explanation.candidate_files.get(index).ok_or_else(|| {
        invalid(format!(
            "candidate index {index} out of range; count = {}",
            explanation.candidate_files.len()
        ))
    })
}

unsafe fn branch_at<'a>(
    explanation: *const LanceCleanupExplanation,
    index: usize,
) -> Result<&'a CleanupReferencedBranch> {
    let explanation = unsafe { explanation_snapshot(explanation)? };
    explanation.referenced_branches.get(index).ok_or_else(|| {
        invalid(format!(
            "referenced branch index {index} out of range; count = {}",
            explanation.referenced_branches.len()
        ))
    })
}

unsafe fn warning_at<'a>(
    explanation: *const LanceCleanupExplanation,
    index: usize,
) -> Result<&'a CString> {
    let explanation = unsafe { explanation_snapshot(explanation)? };
    explanation.warnings.get(index).ok_or_else(|| {
        invalid(format!(
            "warning index {index} out of range; count = {}",
            explanation.warnings.len()
        ))
    })
}

/// Clean versions older than a native `chrono::TimeDelta` window.
///
/// This calls `Dataset::cleanup_old_versions`, so Lance core computes the
/// timestamp cutoff. `options` and `out_stats` must not be NULL. Stats are
/// written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_cleanup_old_versions(
    dataset: *const LanceDataset,
    options: *const LanceCleanupOldVersionsOptions,
    out_stats: *mut LanceRemovalStats,
) -> i32 {
    ffi_try!(
        unsafe { cleanup_old_versions_inner(dataset, options, out_stats) },
        neg
    )
}

unsafe fn cleanup_old_versions_inner(
    dataset: *const LanceDataset,
    options: *const LanceCleanupOldVersionsOptions,
    out_stats: *mut LanceRemovalStats,
) -> Result<i32> {
    if options.is_null() {
        return Err(invalid("options must not be NULL"));
    }
    if out_stats.is_null() {
        return Err(invalid("out_stats must not be NULL"));
    }
    // SAFETY: options is non-NULL and valid for this call. Every field is
    // copied before asynchronous work.
    let raw = unsafe { &*options };
    let older_than =
        helpers::time_delta_from_ffi(raw.older_than_seconds, raw.older_than_nanos, "cleanup")?;
    let delete_unverified = (raw.has_delete_unverified != 0).then_some(raw.delete_unverified);
    let error_if_tagged_old_versions =
        (raw.has_error_if_tagged_old_versions != 0).then_some(raw.error_if_tagged_old_versions);
    let dataset = unsafe { dataset_snapshot(dataset)? };
    let stats = block_on(dataset.cleanup_old_versions(
        older_than,
        delete_unverified,
        error_if_tagged_old_versions,
    ))?;
    // SAFETY: out_stats is non-NULL and caller-owned. It is written only after
    // native cleanup succeeds.
    unsafe {
        ptr::write_unaligned(out_stats, stats.into());
    }
    Ok(0)
}

/// Execute native manual cleanup for an explicit policy.
///
/// `policy` and `out_stats` must not be NULL. `operation_options` may be NULL
/// to use native operation defaults. Stats are written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_cleanup(
    dataset: *const LanceDataset,
    policy: *const LanceCleanupPolicy,
    operation_options: *const LanceCleanupOperationOptions,
    out_stats: *mut LanceRemovalStats,
) -> i32 {
    ffi_try!(
        unsafe { cleanup_inner(dataset, policy, operation_options, out_stats) },
        neg
    )
}

unsafe fn cleanup_inner(
    dataset: *const LanceDataset,
    policy: *const LanceCleanupPolicy,
    operation_options: *const LanceCleanupOperationOptions,
    out_stats: *mut LanceRemovalStats,
) -> Result<i32> {
    if out_stats.is_null() {
        return Err(invalid("out_stats must not be NULL"));
    }
    let policy = unsafe { policy_input(policy)? };
    let operation = unsafe { operation_input(operation_options)? };
    let dataset = unsafe { dataset_snapshot(dataset)? };
    let stats = block_on(async {
        let policy = build_policy(dataset.as_ref(), policy).await?;
        build_operation(dataset.as_ref(), policy, operation)?
            .execute()
            .await
    })?;
    // SAFETY: out_stats is non-NULL and caller-owned. It is written only after
    // native execution succeeds.
    unsafe {
        ptr::write_unaligned(out_stats, stats.into());
    }
    Ok(0)
}

/// Explain native manual cleanup without deleting files.
///
/// Returns an owned explanation snapshot on success or NULL on error. Close it
/// with `lance_cleanup_explanation_close`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_cleanup_explain(
    dataset: *const LanceDataset,
    policy: *const LanceCleanupPolicy,
    operation_options: *const LanceCleanupOperationOptions,
) -> *mut LanceCleanupExplanation {
    ffi_try!(
        unsafe { cleanup_explain_inner(dataset, policy, operation_options) },
        null
    )
}

unsafe fn cleanup_explain_inner(
    dataset: *const LanceDataset,
    policy: *const LanceCleanupPolicy,
    operation_options: *const LanceCleanupOperationOptions,
) -> Result<*mut LanceCleanupExplanation> {
    let policy = unsafe { policy_input(policy)? };
    let operation = unsafe { operation_input(operation_options)? };
    let dataset = unsafe { dataset_snapshot(dataset)? };
    let explanation = block_on(async {
        let policy = build_policy(dataset.as_ref(), policy).await?;
        build_operation(dataset.as_ref(), policy, operation)?
            .explain()
            .await
    })?;
    Ok(Box::into_raw(Box::new(
        LanceCleanupExplanation::from_native(explanation)?,
    )))
}

/// Return the dataset version observed when the explanation was produced.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_read_version(
    explanation: *const LanceCleanupExplanation,
) -> u64 {
    ffi_try!(
        unsafe { explanation_snapshot(explanation).map(|explanation| explanation.read_version) },
        0
    )
}

/// Copy aggregate removal stats from an explanation. The output is written
/// only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_stats(
    explanation: *const LanceCleanupExplanation,
    out_stats: *mut LanceRemovalStats,
) -> i32 {
    ffi_try!(
        unsafe { explanation_stats_inner(explanation, out_stats) },
        neg
    )
}

unsafe fn explanation_stats_inner(
    explanation: *const LanceCleanupExplanation,
    out_stats: *mut LanceRemovalStats,
) -> Result<i32> {
    if out_stats.is_null() {
        return Err(invalid("out_stats must not be NULL"));
    }
    let explanation = unsafe { explanation_snapshot(explanation)? };
    // SAFETY: out_stats is non-NULL and caller-owned; the handle is valid.
    unsafe {
        ptr::write_unaligned(out_stats, explanation.stats);
    }
    Ok(0)
}

/// Number of candidate files included in the explanation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_candidate_count(
    explanation: *const LanceCleanupExplanation,
) -> u64 {
    ffi_try!(
        unsafe {
            explanation_snapshot(explanation)
                .map(|explanation| explanation.candidate_files.len() as u64)
        },
        0
    )
}

/// Whether the candidate list was truncated.
///
/// Returns 1/0 on success and -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_candidate_files_truncated(
    explanation: *const LanceCleanupExplanation,
) -> i32 {
    ffi_try!(
        unsafe {
            explanation_snapshot(explanation)
                .map(|explanation| bool_code(explanation.candidate_files_truncated))
        },
        neg
    )
}

/// Native candidate-file limit used for the explanation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_candidate_file_limit(
    explanation: *const LanceCleanupExplanation,
) -> u64 {
    ffi_try!(
        unsafe {
            explanation_snapshot(explanation).map(|explanation| explanation.candidate_file_limit)
        },
        0
    )
}

/// Candidate file path at `index`. The string is owned by `explanation` and
/// remains valid until it is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_candidate_path_at(
    explanation: *const LanceCleanupExplanation,
    index: usize,
) -> *const c_char {
    ffi_try!(
        unsafe { candidate_at(explanation, index).map(|candidate| candidate.path.as_ptr()) },
        null
    )
}

/// Candidate file kind at `index` (`LanceCleanupFileKind` discriminant).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_candidate_kind_at(
    explanation: *const LanceCleanupExplanation,
    index: usize,
) -> i32 {
    ffi_try!(
        unsafe { candidate_at(explanation, index).map(|candidate| candidate.kind) },
        neg
    )
}

/// Candidate unverified flag at `index`. Returns 1/0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_candidate_unverified_at(
    explanation: *const LanceCleanupExplanation,
    index: usize,
) -> i32 {
    ffi_try!(
        unsafe {
            candidate_at(explanation, index).map(|candidate| bool_code(candidate.unverified))
        },
        neg
    )
}

/// Candidate file size in bytes at `index`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_candidate_size_bytes_at(
    explanation: *const LanceCleanupExplanation,
    index: usize,
) -> u64 {
    ffi_try!(
        unsafe { candidate_at(explanation, index).map(|candidate| candidate.size_bytes) },
        0
    )
}

/// Number of referenced child branches observed by the explanation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_referenced_branch_count(
    explanation: *const LanceCleanupExplanation,
) -> u64 {
    ffi_try!(
        unsafe {
            explanation_snapshot(explanation)
                .map(|explanation| explanation.referenced_branches.len() as u64)
        },
        0
    )
}

/// Referenced branch name at `index`; owned by `explanation`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_referenced_branch_name_at(
    explanation: *const LanceCleanupExplanation,
    index: usize,
) -> *const c_char {
    ffi_try!(
        unsafe { branch_at(explanation, index).map(|branch| branch.name.as_ptr()) },
        null
    )
}

/// Version of the current lineage referenced by the branch at `index`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_referenced_branch_version_at(
    explanation: *const LanceCleanupExplanation,
    index: usize,
) -> u64 {
    ffi_try!(
        unsafe { branch_at(explanation, index).map(|branch| branch.referenced_version) },
        0
    )
}

/// Whether the branch at `index` is a cascade-cleanup candidate.
///
/// Returns 1/0 on success and -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_referenced_branch_cleanup_candidate_at(
    explanation: *const LanceCleanupExplanation,
    index: usize,
) -> i32 {
    ffi_try!(
        unsafe { branch_at(explanation, index).map(|branch| bool_code(branch.cleanup_candidate)) },
        neg
    )
}

/// Number of non-fatal warnings in the explanation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_warning_count(
    explanation: *const LanceCleanupExplanation,
) -> u64 {
    ffi_try!(
        unsafe {
            explanation_snapshot(explanation).map(|explanation| explanation.warnings.len() as u64)
        },
        0
    )
}

/// Warning at `index`; owned by `explanation`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_warning_at(
    explanation: *const LanceCleanupExplanation,
    index: usize,
) -> *const c_char {
    ffi_try!(
        unsafe { warning_at(explanation, index).map(|warning| warning.as_ptr()) },
        null
    )
}

/// Close and free an explanation snapshot. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_cleanup_explanation_close(
    explanation: *mut LanceCleanupExplanation,
) {
    if !explanation.is_null() {
        swallow_unwind("lance_cleanup_explanation_close", || unsafe {
            let _ = Box::from_raw(explanation);
        });
    }
}
