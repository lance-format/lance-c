// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Index optimization C API: native options plus single-process and
//! plan/task/merge/result/commit workflows.

use std::ffi::c_char;
use std::ptr;
use std::sync::Arc;

use lance::index::DatasetIndexExt;
use lance::index::optimize::{
    IndexSegmentResult, OptimizeIndexMerge, OptimizeIndexTask, OptimizeIndicesPlan,
    commit_optimize_indices, plan_optimize_indices,
};
use lance_core::Result;
use lance_index::optimize::OptimizeOptions;
use uuid::Uuid;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::index_segment::{LanceIndexSegmentMetadata, metadata_handle};
use crate::metadata::owned_c_string;
use crate::runtime::block_on;
use crate::transaction_properties::{LanceTransactionProperties, parse_transaction_properties};

/// Opaque native index-optimization options.
pub struct LanceOptimizeIndicesOptions {
    options: OptimizeOptions,
}

/// Opaque owned snapshot of a native `OptimizeIndicesPlan`.
pub struct LanceOptimizeIndicesPlan {
    plan: OptimizeIndicesPlan,
}

/// Opaque owned snapshot of a native `OptimizeIndexTask`.
pub struct LanceOptimizeIndexTask {
    task: OptimizeIndexTask,
}

/// Opaque owned snapshot of a native `OptimizeIndexMerge`.
pub struct LanceOptimizeIndexMerge {
    merge: OptimizeIndexMerge,
}

/// Opaque owned snapshot of a native `IndexSegmentResult`.
pub struct LanceIndexSegmentResult {
    result: IndexSegmentResult,
}

fn invalid_input(message: impl Into<String>) -> lance_core::Error {
    lance_core::Error::invalid_input(message)
}

unsafe fn options_mut<'a>(
    options: *mut LanceOptimizeIndicesOptions,
) -> Result<&'a mut OptimizeOptions> {
    if options.is_null() {
        return Err(invalid_input("options must not be NULL"));
    }
    Ok(unsafe { &mut (*options).options })
}

unsafe fn options_ref<'a>(
    options: *const LanceOptimizeIndicesOptions,
) -> Result<&'a OptimizeOptions> {
    if options.is_null() {
        return Err(invalid_input("options must not be NULL"));
    }
    Ok(unsafe { &(*options).options })
}

fn options_box(options: OptimizeOptions) -> *mut LanceOptimizeIndicesOptions {
    Box::into_raw(Box::new(LanceOptimizeIndicesOptions { options }))
}

fn optional_usize(has_value: bool, value: u64, field: &'static str) -> Result<Option<usize>> {
    if has_value {
        Ok(Some(usize::try_from(value).map_err(|_| {
            invalid_input(format!("{field}={value} exceeds usize::MAX on this target"))
        })?))
    } else {
        Ok(None)
    }
}

fn copy_uuid(uuid: Uuid, out: *mut u8) -> Result<()> {
    if out.is_null() {
        return Err(invalid_input("out_uuid must not be NULL"));
    }
    unsafe { ptr::copy_nonoverlapping(uuid.as_bytes().as_ptr(), out, 16) };
    Ok(())
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

fn clone_options(options: *const LanceOptimizeIndicesOptions) -> Result<OptimizeOptions> {
    if options.is_null() {
        Ok(OptimizeOptions::default())
    } else {
        Ok(unsafe { options_ref(options) }?.clone())
    }
}

/// Create options with native `OptimizeOptions::default()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_options_new() -> *mut LanceOptimizeIndicesOptions {
    ffi_try!(
        Ok::<_, lance_core::Error>(options_box(OptimizeOptions::default())),
        null
    )
}

/// Clone native index-optimization options.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_options_clone(
    options: *const LanceOptimizeIndicesOptions,
) -> *mut LanceOptimizeIndicesOptions {
    ffi_try!(
        (|| -> Result<*mut LanceOptimizeIndicesOptions> {
            Ok(options_box(unsafe { options_ref(options) }?.clone()))
        })(),
        null
    )
}

/// Free index-optimization options. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_options_free(
    options: *mut LanceOptimizeIndicesOptions,
) {
    if !options.is_null() {
        unsafe { drop(Box::from_raw(options)) };
    }
}

/// Set optional `num_indices_to_merge`; explicit zero is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_options_set_num_indices_to_merge(
    options: *mut LanceOptimizeIndicesOptions,
    has_value: bool,
    value: u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.num_indices_to_merge =
                optional_usize(has_value, value, "num_indices_to_merge")?;
            Ok(0)
        })(),
        neg
    )
}

/// Set `enable_partition_rebalancing`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_options_set_enable_partition_rebalancing(
    options: *mut LanceOptimizeIndicesOptions,
    value: bool,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.enable_partition_rebalancing = value;
            Ok(0)
        })(),
        neg
    )
}

/// Set optional index names. NULL with `has_value == false` means native
/// None; `has_value == true` and `count == 0` means native `Some(empty)`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_options_set_index_names(
    options: *mut LanceOptimizeIndicesOptions,
    has_value: bool,
    names: *const *const c_char,
    count: usize,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let parsed = if has_value {
                if count > 0 && names.is_null() {
                    return Err(invalid_input(format!(
                        "names must not be NULL when count = {count}"
                    )));
                }
                if count > isize::MAX as usize / std::mem::size_of::<*const c_char>() {
                    return Err(invalid_input(format!(
                        "count {count} exceeds the maximum addressable pointer slice length"
                    )));
                }
                let mut parsed = Vec::with_capacity(count);
                for index in 0..count {
                    let name = unsafe { *names.add(index) };
                    let name = unsafe { helpers::parse_c_string(name) }
                        .map_err(|error| invalid_input(format!("names[{index}]: {error}")))?
                        .ok_or_else(|| invalid_input(format!("names[{index}] must not be NULL")))?;
                    parsed.push(name.to_owned());
                }
                Some(parsed)
            } else {
                None
            };
            unsafe { options_mut(options) }?.index_names = parsed;
            Ok(0)
        })(),
        neg
    )
}

/// Set `retrain`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_options_set_retrain(
    options: *mut LanceOptimizeIndicesOptions,
    value: bool,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.retrain = value;
            Ok(0)
        })(),
        neg
    )
}

/// Set optional transaction properties for the optimize commit.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_options_set_transaction_properties(
    options: *mut LanceOptimizeIndicesOptions,
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

/// Set optional `max_rows_per_task`; explicit zero is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_options_set_max_rows_per_task(
    options: *mut LanceOptimizeIndicesOptions,
    has_value: bool,
    value: u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.max_rows_per_task = has_value.then_some(value);
            Ok(0)
        })(),
        neg
    )
}

/// Set optional `max_rows_per_segment`; explicit zero is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_options_set_max_rows_per_segment(
    options: *mut LanceOptimizeIndicesOptions,
    has_value: bool,
    value: u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            unsafe { options_mut(options) }?.max_rows_per_segment = has_value.then_some(value);
            Ok(0)
        })(),
        neg
    )
}

/// Run native `Dataset::optimize_indices` through the single-process path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_optimize_indices(
    dataset: *mut LanceDataset,
    options: *const LanceOptimizeIndicesOptions,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if dataset.is_null() {
                return Err(invalid_input("dataset must not be NULL"));
            }
            let options = clone_options(options)?;
            unsafe { &*dataset }
                .with_mut(|dataset| block_on(dataset.optimize_indices(&options)))?;
            Ok(0)
        })(),
        neg
    )
}

/// Plan native distributed index optimization.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_plan_optimize_indices(
    dataset: *const LanceDataset,
    options: *const LanceOptimizeIndicesOptions,
) -> *mut LanceOptimizeIndicesPlan {
    ffi_try!(
        (|| -> Result<*mut LanceOptimizeIndicesPlan> {
            if dataset.is_null() {
                return Err(invalid_input("dataset must not be NULL"));
            }
            let snapshot = unsafe { &*dataset }.snapshot();
            let options = clone_options(options)?;
            let plan = block_on(plan_optimize_indices(&snapshot, &options))?;
            Ok(Box::into_raw(Box::new(LanceOptimizeIndicesPlan { plan })))
        })(),
        null
    )
}

/// Copy the plan UUID as 16 raw RFC 4122 bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_plan_id(
    plan: *const LanceOptimizeIndicesPlan,
    out_uuid: *mut u8,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if plan.is_null() {
                return Err(invalid_input("plan must not be NULL"));
            }
            copy_uuid(unsafe { &*plan }.plan.plan_id, out_uuid)?;
            Ok(0)
        })(),
        neg
    )
}

/// Return the plan's read version.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_plan_read_version(
    plan: *const LanceOptimizeIndicesPlan,
) -> u64 {
    ffi_try!(
        (|| -> Result<u64> {
            if plan.is_null() {
                return Err(invalid_input("plan must not be NULL"));
            }
            Ok(unsafe { &*plan }.plan.read_version)
        })(),
        0
    )
}

/// Return the number of build tasks in the plan.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_plan_task_count(
    plan: *const LanceOptimizeIndicesPlan,
) -> usize {
    ffi_try!(
        (|| -> Result<usize> {
            if plan.is_null() {
                return Err(invalid_input("plan must not be NULL"));
            }
            Ok(unsafe { &*plan }.plan.tasks.len())
        })(),
        0
    )
}

/// Return the number of merge units in the plan.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_plan_merge_count(
    plan: *const LanceOptimizeIndicesPlan,
) -> usize {
    ffi_try!(
        (|| -> Result<usize> {
            if plan.is_null() {
                return Err(invalid_input("plan must not be NULL"));
            }
            Ok(unsafe { &*plan }.plan.merges.len())
        })(),
        0
    )
}

/// Return an owned snapshot of the build task at `index`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_plan_task_at(
    plan: *const LanceOptimizeIndicesPlan,
    index: usize,
) -> *mut LanceOptimizeIndexTask {
    ffi_try!(
        (|| -> Result<*mut LanceOptimizeIndexTask> {
            if plan.is_null() {
                return Err(invalid_input("plan must not be NULL"));
            }
            let plan = unsafe { &*plan };
            let task = plan.plan.tasks.get(index).cloned().ok_or_else(|| {
                invalid_input(format!(
                    "task index {index} is out of range; task_count = {}",
                    plan.plan.tasks.len()
                ))
            })?;
            Ok(Box::into_raw(Box::new(LanceOptimizeIndexTask { task })))
        })(),
        null
    )
}

/// Return an owned snapshot of the merge at `index`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_plan_merge_at(
    plan: *const LanceOptimizeIndicesPlan,
    index: usize,
) -> *mut LanceOptimizeIndexMerge {
    ffi_try!(
        (|| -> Result<*mut LanceOptimizeIndexMerge> {
            if plan.is_null() {
                return Err(invalid_input("plan must not be NULL"));
            }
            let plan = unsafe { &*plan };
            let merge = plan.plan.merges.get(index).cloned().ok_or_else(|| {
                invalid_input(format!(
                    "merge index {index} is out of range; merge_count = {}",
                    plan.plan.merges.len()
                ))
            })?;
            Ok(Box::into_raw(Box::new(LanceOptimizeIndexMerge { merge })))
        })(),
        null
    )
}

/// Serialize the index-optimization plan using native serde.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_plan_json(
    plan: *const LanceOptimizeIndicesPlan,
) -> *const c_char {
    ffi_try!(
        (|| -> Result<*const c_char> {
            if plan.is_null() {
                return Err(invalid_input("plan must not be NULL"));
            }
            owned_json(&unsafe { &*plan }.plan, "index optimization plan")
        })(),
        null
    )
}

/// Decode an index-optimization plan from native serde JSON.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_plan_from_json(
    json: *const c_char,
) -> *mut LanceOptimizeIndicesPlan {
    ffi_try!(
        (|| -> Result<*mut LanceOptimizeIndicesPlan> {
            Ok(Box::into_raw(Box::new(LanceOptimizeIndicesPlan {
                plan: parse_json(json, "index optimization plan")?,
            })))
        })(),
        null
    )
}

/// Free an index-optimization plan. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_indices_plan_close(plan: *mut LanceOptimizeIndicesPlan) {
    if !plan.is_null() {
        unsafe { drop(Box::from_raw(plan)) };
    }
}

/// Copy a build task's plan UUID.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_task_plan_id(
    task: *const LanceOptimizeIndexTask,
    out_uuid: *mut u8,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if task.is_null() {
                return Err(invalid_input("task must not be NULL"));
            }
            copy_uuid(unsafe { &*task }.task.plan_id, out_uuid)?;
            Ok(0)
        })(),
        neg
    )
}

/// Return a build task's read version.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_task_read_version(
    task: *const LanceOptimizeIndexTask,
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

/// Return a build task's merge group.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_task_group(
    task: *const LanceOptimizeIndexTask,
) -> u32 {
    ffi_try!(
        (|| -> Result<u32> {
            if task.is_null() {
                return Err(invalid_input("task must not be NULL"));
            }
            Ok(unsafe { &*task }.task.group)
        })(),
        0
    )
}

/// Return the number of source fragments in a build task.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_task_fragment_count(
    task: *const LanceOptimizeIndexTask,
) -> usize {
    ffi_try!(
        (|| -> Result<usize> {
            if task.is_null() {
                return Err(invalid_input("task must not be NULL"));
            }
            Ok(unsafe { &*task }.task.fragments.len())
        })(),
        0
    )
}

/// Copy one source fragment ID from a build task.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_task_fragment_id_at(
    task: *const LanceOptimizeIndexTask,
    index: usize,
    out_id: *mut u32,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if task.is_null() || out_id.is_null() {
                return Err(invalid_input("task and out_id must not be NULL"));
            }
            let task = unsafe { &*task };
            let id = *task.task.fragments.get(index).ok_or_else(|| {
                invalid_input(format!(
                    "fragment index {index} is out of range; fragment_count = {}",
                    task.task.fragments.len()
                ))
            })?;
            unsafe { ptr::write_unaligned(out_id, id) };
            Ok(0)
        })(),
        neg
    )
}

/// Return an owned copy of the task's reference segment metadata.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_task_reference(
    task: *const LanceOptimizeIndexTask,
) -> *mut LanceIndexSegmentMetadata {
    ffi_try!(
        (|| -> Result<*mut LanceIndexSegmentMetadata> {
            if task.is_null() {
                return Err(invalid_input("task must not be NULL"));
            }
            metadata_handle(unsafe { &*task }.task.reference.clone())
        })(),
        null
    )
}

/// Return the number of committed segments listed by a build task.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_task_old_segment_count(
    task: *const LanceOptimizeIndexTask,
) -> usize {
    ffi_try!(
        (|| -> Result<usize> {
            if task.is_null() {
                return Err(invalid_input("task must not be NULL"));
            }
            Ok(unsafe { &*task }.task.old_segments.len())
        })(),
        0
    )
}

/// Return an owned copy of one old segment's metadata.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_task_old_segment_at(
    task: *const LanceOptimizeIndexTask,
    index: usize,
) -> *mut LanceIndexSegmentMetadata {
    ffi_try!(
        (|| -> Result<*mut LanceIndexSegmentMetadata> {
            if task.is_null() {
                return Err(invalid_input("task must not be NULL"));
            }
            let task = unsafe { &*task };
            let metadata = task.task.old_segments.get(index).cloned().ok_or_else(|| {
                invalid_input(format!(
                    "old segment index {index} is out of range; count = {}",
                    task.task.old_segments.len()
                ))
            })?;
            metadata_handle(metadata)
        })(),
        null
    )
}

/// Read `num_indices_to_merge` and its presence bit.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_task_num_indices_to_merge(
    task: *const LanceOptimizeIndexTask,
    out_has_value: *mut bool,
    out_value: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if task.is_null() || out_has_value.is_null() || out_value.is_null() {
                return Err(invalid_input(
                    "task, out_has_value, and out_value must not be NULL",
                ));
            }
            let value = unsafe { &*task }.task.num_indices_to_merge;
            unsafe {
                ptr::write_unaligned(out_has_value, value.is_some());
                ptr::write_unaligned(out_value, value.map_or(0, |value| value as u64));
            }
            Ok(0)
        })(),
        neg
    )
}

/// Read `enable_partition_rebalancing`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_task_enable_partition_rebalancing(
    task: *const LanceOptimizeIndexTask,
    out_value: *mut bool,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if task.is_null() || out_value.is_null() {
                return Err(invalid_input("task and out_value must not be NULL"));
            }
            unsafe { ptr::write_unaligned(out_value, (*task).task.enable_partition_rebalancing) };
            Ok(0)
        })(),
        neg
    )
}

/// Read `retrain`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_task_retrain(
    task: *const LanceOptimizeIndexTask,
    out_value: *mut bool,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if task.is_null() || out_value.is_null() {
                return Err(invalid_input("task and out_value must not be NULL"));
            }
            unsafe { ptr::write_unaligned(out_value, (*task).task.retrain) };
            Ok(0)
        })(),
        neg
    )
}

/// Execute a build task. `out_result` is set to NULL when native returns None.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_task_execute(
    dataset: *const LanceDataset,
    task: *const LanceOptimizeIndexTask,
    out_result: *mut *mut LanceIndexSegmentResult,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if dataset.is_null() || task.is_null() || out_result.is_null() {
                return Err(invalid_input(
                    "dataset, task, and out_result must not be NULL",
                ));
            }
            let snapshot = unsafe { &*dataset }.snapshot();
            let result = block_on(unsafe { &*task }.task.execute(&snapshot))?;
            let result = result.map_or(ptr::null_mut(), |result| {
                Box::into_raw(Box::new(LanceIndexSegmentResult { result }))
            });
            unsafe { ptr::write_unaligned(out_result, result) };
            Ok(0)
        })(),
        neg
    )
}

/// Serialize a build task using native serde.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_task_json(
    task: *const LanceOptimizeIndexTask,
) -> *const c_char {
    ffi_try!(
        (|| -> Result<*const c_char> {
            if task.is_null() {
                return Err(invalid_input("task must not be NULL"));
            }
            owned_json(&unsafe { &*task }.task, "index optimization task")
        })(),
        null
    )
}

/// Decode a build task from native serde JSON.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_task_from_json(
    json: *const c_char,
) -> *mut LanceOptimizeIndexTask {
    ffi_try!(
        (|| -> Result<*mut LanceOptimizeIndexTask> {
            Ok(Box::into_raw(Box::new(LanceOptimizeIndexTask {
                task: parse_json(json, "index optimization task")?,
            })))
        })(),
        null
    )
}

/// Free a build task. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_task_close(task: *mut LanceOptimizeIndexTask) {
    if !task.is_null() {
        unsafe { drop(Box::from_raw(task)) };
    }
}

/// Copy a merge's plan UUID.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_merge_plan_id(
    merge: *const LanceOptimizeIndexMerge,
    out_uuid: *mut u8,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if merge.is_null() {
                return Err(invalid_input("merge must not be NULL"));
            }
            copy_uuid(unsafe { &*merge }.merge.plan_id, out_uuid)?;
            Ok(0)
        })(),
        neg
    )
}

/// Return a merge's read version.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_merge_read_version(
    merge: *const LanceOptimizeIndexMerge,
) -> u64 {
    ffi_try!(
        (|| -> Result<u64> {
            if merge.is_null() {
                return Err(invalid_input("merge must not be NULL"));
            }
            Ok(unsafe { &*merge }.merge.read_version)
        })(),
        0
    )
}

/// Return a merge's group.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_merge_group(
    merge: *const LanceOptimizeIndexMerge,
) -> u32 {
    ffi_try!(
        (|| -> Result<u32> {
            if merge.is_null() {
                return Err(invalid_input("merge must not be NULL"));
            }
            Ok(unsafe { &*merge }.merge.group)
        })(),
        0
    )
}

/// Return the number of committed segments a merge lists.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_merge_old_segment_count(
    merge: *const LanceOptimizeIndexMerge,
) -> usize {
    ffi_try!(
        (|| -> Result<usize> {
            if merge.is_null() {
                return Err(invalid_input("merge must not be NULL"));
            }
            Ok(unsafe { &*merge }.merge.old_segments.len())
        })(),
        0
    )
}

/// Return an owned copy of one old segment's metadata.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_merge_old_segment_at(
    merge: *const LanceOptimizeIndexMerge,
    index: usize,
) -> *mut LanceIndexSegmentMetadata {
    ffi_try!(
        (|| -> Result<*mut LanceIndexSegmentMetadata> {
            if merge.is_null() {
                return Err(invalid_input("merge must not be NULL"));
            }
            let merge = unsafe { &*merge };
            let metadata = merge
                .merge
                .old_segments
                .get(index)
                .cloned()
                .ok_or_else(|| {
                    invalid_input(format!(
                        "old segment index {index} is out of range; count = {}",
                        merge.merge.old_segments.len()
                    ))
                })?;
            metadata_handle(metadata)
        })(),
        null
    )
}

/// Return the number of segments a merge supersedes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_merge_replace_count(
    merge: *const LanceOptimizeIndexMerge,
) -> usize {
    ffi_try!(
        (|| -> Result<usize> {
            if merge.is_null() {
                return Err(invalid_input("merge must not be NULL"));
            }
            Ok(unsafe { &*merge }.merge.replaces.len())
        })(),
        0
    )
}

/// Copy one superseded segment UUID from a merge.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_merge_replace_uuid_at(
    merge: *const LanceOptimizeIndexMerge,
    index: usize,
    out_uuid: *mut u8,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if merge.is_null() {
                return Err(invalid_input("merge must not be NULL"));
            }
            let merge = unsafe { &*merge };
            let uuid = merge.merge.replaces.get(index).copied().ok_or_else(|| {
                invalid_input(format!(
                    "replace index {index} is out of range; replace_count = {}",
                    merge.merge.replaces.len()
                ))
            })?;
            copy_uuid(uuid, out_uuid)?;
            Ok(0)
        })(),
        neg
    )
}

/// Execute one merge with its task results.
///
/// `out_result` is set to NULL when native returns None. Plan/group/version
/// validation remains native.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_merge_execute(
    dataset: *const LanceDataset,
    merge: *const LanceOptimizeIndexMerge,
    task_results: *const *const LanceIndexSegmentResult,
    task_result_count: usize,
    out_result: *mut *mut LanceIndexSegmentResult,
) -> i32 {
    ffi_try!(
        unsafe { merge_execute_inner(dataset, merge, task_results, task_result_count, out_result) },
        neg
    )
}

unsafe fn merge_execute_inner(
    dataset: *const LanceDataset,
    merge: *const LanceOptimizeIndexMerge,
    task_results: *const *const LanceIndexSegmentResult,
    task_result_count: usize,
    out_result: *mut *mut LanceIndexSegmentResult,
) -> Result<i32> {
    if dataset.is_null() || merge.is_null() || out_result.is_null() {
        return Err(invalid_input(
            "dataset, merge, and out_result must not be NULL",
        ));
    }
    if task_result_count > 0 && task_results.is_null() {
        return Err(invalid_input(format!(
            "task_results must not be NULL when task_result_count = {task_result_count}"
        )));
    }
    if task_result_count
        > isize::MAX as usize / std::mem::size_of::<*const LanceIndexSegmentResult>()
    {
        return Err(invalid_input(format!(
            "task_result_count {task_result_count} exceeds the maximum addressable pointer slice length"
        )));
    }
    let mut parsed = Vec::with_capacity(task_result_count);
    for index in 0..task_result_count {
        let result = unsafe { *task_results.add(index) };
        if result.is_null() {
            return Err(invalid_input(format!(
                "task_results[{index}] must not be NULL"
            )));
        }
        parsed.push(unsafe { &*result }.result.clone());
    }

    let snapshot = unsafe { &*dataset }.snapshot();
    let result = block_on(unsafe { &*merge }.merge.execute(&snapshot, &parsed))?;
    let result = result.map_or(ptr::null_mut(), |result| {
        Box::into_raw(Box::new(LanceIndexSegmentResult { result }))
    });
    unsafe { ptr::write_unaligned(out_result, result) };
    Ok(0)
}

/// Serialize a merge using native serde.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_merge_json(
    merge: *const LanceOptimizeIndexMerge,
) -> *const c_char {
    ffi_try!(
        (|| -> Result<*const c_char> {
            if merge.is_null() {
                return Err(invalid_input("merge must not be NULL"));
            }
            owned_json(&unsafe { &*merge }.merge, "index optimization merge")
        })(),
        null
    )
}

/// Decode an index optimization merge from native serde JSON.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_merge_from_json(
    json: *const c_char,
) -> *mut LanceOptimizeIndexMerge {
    ffi_try!(
        (|| -> Result<*mut LanceOptimizeIndexMerge> {
            Ok(Box::into_raw(Box::new(LanceOptimizeIndexMerge {
                merge: parse_json(json, "index optimization merge")?,
            })))
        })(),
        null
    )
}

/// Free a merge snapshot. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_optimize_index_merge_close(merge: *mut LanceOptimizeIndexMerge) {
    if !merge.is_null() {
        unsafe { drop(Box::from_raw(merge)) };
    }
}

/// Copy a segment result's plan UUID.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_result_plan_id(
    result: *const LanceIndexSegmentResult,
    out_uuid: *mut u8,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if result.is_null() {
                return Err(invalid_input("result must not be NULL"));
            }
            copy_uuid(unsafe { &*result }.result.plan_id, out_uuid)?;
            Ok(0)
        })(),
        neg
    )
}

/// Return a segment result's read version.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_result_read_version(
    result: *const LanceIndexSegmentResult,
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

/// Return a segment result's group.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_result_group(
    result: *const LanceIndexSegmentResult,
) -> u32 {
    ffi_try!(
        (|| -> Result<u32> {
            if result.is_null() {
                return Err(invalid_input("result must not be NULL"));
            }
            Ok(unsafe { &*result }.result.group)
        })(),
        0
    )
}

/// Return an owned copy of the result's segment metadata.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_result_segment(
    result: *const LanceIndexSegmentResult,
) -> *mut LanceIndexSegmentMetadata {
    ffi_try!(
        (|| -> Result<*mut LanceIndexSegmentMetadata> {
            if result.is_null() {
                return Err(invalid_input("result must not be NULL"));
            }
            metadata_handle(unsafe { &*result }.result.segment.clone())
        })(),
        null
    )
}

/// Return the number of segments this result supersedes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_result_replace_count(
    result: *const LanceIndexSegmentResult,
) -> usize {
    ffi_try!(
        (|| -> Result<usize> {
            if result.is_null() {
                return Err(invalid_input("result must not be NULL"));
            }
            Ok(unsafe { &*result }.result.replaces.len())
        })(),
        0
    )
}

/// Copy one superseded segment UUID from a result.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_result_replace_uuid_at(
    result: *const LanceIndexSegmentResult,
    index: usize,
    out_uuid: *mut u8,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if result.is_null() {
                return Err(invalid_input("result must not be NULL"));
            }
            let result = unsafe { &*result };
            let uuid = result.result.replaces.get(index).copied().ok_or_else(|| {
                invalid_input(format!(
                    "replace index {index} is out of range; replace_count = {}",
                    result.result.replaces.len()
                ))
            })?;
            copy_uuid(uuid, out_uuid)?;
            Ok(0)
        })(),
        neg
    )
}

/// Serialize a segment result using native serde.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_result_json(
    result: *const LanceIndexSegmentResult,
) -> *const c_char {
    ffi_try!(
        (|| -> Result<*const c_char> {
            if result.is_null() {
                return Err(invalid_input("result must not be NULL"));
            }
            owned_json(&unsafe { &*result }.result, "index segment result")
        })(),
        null
    )
}

/// Decode an index segment result from native serde JSON.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_result_from_json(
    json: *const c_char,
) -> *mut LanceIndexSegmentResult {
    ffi_try!(
        (|| -> Result<*mut LanceIndexSegmentResult> {
            Ok(Box::into_raw(Box::new(LanceIndexSegmentResult {
                result: parse_json(json, "index segment result")?,
            })))
        })(),
        null
    )
}

/// Free a segment result. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_result_close(result: *mut LanceIndexSegmentResult) {
    if !result.is_null() {
        unsafe { drop(Box::from_raw(result)) };
    }
}

/// Commit merged index segments through native `commit_optimize_indices`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_commit_optimize_indices(
    dataset: *mut LanceDataset,
    results: *const *const LanceIndexSegmentResult,
    result_count: usize,
    options: *const LanceOptimizeIndicesOptions,
) -> i32 {
    ffi_try!(
        unsafe { commit_optimize_indices_inner(dataset, results, result_count, options) },
        neg
    )
}

unsafe fn commit_optimize_indices_inner(
    dataset: *mut LanceDataset,
    results: *const *const LanceIndexSegmentResult,
    result_count: usize,
    options: *const LanceOptimizeIndicesOptions,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(invalid_input("dataset must not be NULL"));
    }
    if result_count > 0 && results.is_null() {
        return Err(invalid_input(format!(
            "results must not be NULL when result_count = {result_count}"
        )));
    }
    if result_count > isize::MAX as usize / std::mem::size_of::<*const LanceIndexSegmentResult>() {
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
    let options = clone_options(options)?;
    unsafe { &*dataset }
        .with_mut(|dataset| block_on(commit_optimize_indices(dataset, parsed, &options)))?;
    Ok(0)
}
