// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Owned native transaction and operation values.
//!
//! The handles in this module are snapshots: they own cloned native values and
//! remain usable after the dataset or writer that produced them is closed.
//! Dataset reads and transaction publication are implemented in `commit.rs`.

use std::collections::HashMap;
use std::ffi::{CString, c_char};
use std::ptr;
use std::sync::Arc;

use arrow::ffi::FFI_ArrowSchema;
use arrow_schema::Schema as ArrowSchema;
use lance::dataset::transaction::{
    Operation, RewriteGroup, RewrittenIndex, Transaction, TransactionBuilder, UpdateMode,
    UpdatedFragmentOffsets,
};
use lance_core::Result;
use lance_core::datatypes::Schema;
use lance_table::format::key_existence::{FilterType, KeyExistenceFilter};
use lance_table::format::{BasePath, Fragment, IndexFile, IndexMetadata, pb, pb_actions};
use lance_table::transaction::{DataOverlayGroup, DataReplacementGroup, UpdateMap};
use prost::Message;
use roaring::RoaringBitmap;

use crate::base_paths::{
    LanceBasePath, LanceBasePathList, LanceBasePathView, OwnedBasePath, own_base_paths,
    parse_base_path_list, parse_base_paths,
};
use crate::data_file::LanceDataFile;
use crate::data_overlay::LanceDataOverlayGroup;
use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
use crate::fragment::{LanceFragment, fragment_metadata_json};
use crate::helpers;
use crate::index::{LanceStringMap, parse_string_map};
use crate::index_segment::{LanceIndexSegmentMetadata, metadata_handle};
use crate::mem_wal::LanceCompactedSsTable;
use crate::runtime::block_on;
use crate::schema::{LanceSchema, schema_ref};
use crate::update_map::{LanceUpdateMap, parse_update_map, update_map_at};

/// Owned native transaction value.
pub struct LanceTransaction {
    pub(crate) inner: Transaction,
}

/// Owned native operation value cloned from a transaction.
pub struct LanceOperation {
    pub(crate) inner: Operation,
    /// Owned C-string storage backing borrowed `UpdateBases` and Overwrite
    /// initial-base views.
    base_paths: Vec<Result<OwnedBasePath>>,
    /// Owned C-string storage backing borrowed Overwrite config views. The
    /// order matches native `HashMap` iteration for the operation's lifetime.
    config_upserts: Vec<Result<(CString, CString)>>,
    /// Owned C-string storage backing borrowed Clone string views.
    clone_strings: CloneStrings,
    /// Owned C-string storage for a MemWAL directory identifier carried by an
    /// Update or UpdateMemWalState operation.
    mem_wal_dir_id: Option<Result<CString>>,
}

/// Owned C-string conversions for the native Clone string fields.
///
/// Each conversion result is retained independently so that a field which
/// cannot be represented as a C string only fails its own getter.
#[derive(Default)]
struct CloneStrings {
    ref_name: Option<Result<CString>>,
    ref_path: Option<Result<CString>>,
    branch_name: Option<Result<CString>>,
}

impl CloneStrings {
    fn from_native(operation: &Operation) -> Self {
        match operation {
            Operation::Clone {
                ref_name,
                ref_path,
                branch_name,
                ..
            } => Self {
                ref_name: ref_name
                    .as_deref()
                    .map(|value| owned_c_string(value, "Clone ref_name")),
                ref_path: Some(owned_c_string(ref_path, "Clone ref_path")),
                branch_name: branch_name
                    .as_deref()
                    .map(|value| owned_c_string(value, "Clone branch_name")),
            },
            _ => Self::default(),
        }
    }
}

impl LanceOperation {
    /// Wrap a native operation, retaining C-string storage for borrowed views.
    pub(crate) fn from_native(inner: Operation) -> Self {
        let base_paths = match &inner {
            Operation::UpdateBases { new_bases } => own_base_paths(new_bases),
            Operation::Overwrite {
                initial_bases: Some(bases),
                ..
            } => own_base_paths(bases),
            _ => Vec::new(),
        };
        let config_upserts = match &inner {
            Operation::Overwrite {
                config_upsert_values: Some(values),
                ..
            } => values
                .iter()
                .map(|(key, value)| {
                    Ok((
                        owned_c_string(key, "Overwrite config key")?,
                        owned_c_string(value, "Overwrite config value")?,
                    ))
                })
                .collect(),
            _ => Vec::new(),
        };
        let clone_strings = CloneStrings::from_native(&inner);
        let mem_wal_dir_id = match &inner {
            Operation::Update {
                mem_wal_dir_id: Some(dir_id),
                ..
            } => Some(owned_c_string(dir_id, "Update mem_wal_dir_id")),
            Operation::UpdateMemWalState { dir_id, .. } => {
                Some(owned_c_string(dir_id, "UpdateMemWalState dir_id"))
            }
            _ => None,
        };
        Self {
            inner,
            base_paths,
            config_upserts,
            clone_strings,
            mem_wal_dir_id,
        }
    }
}

/// Owned native `DataReplacementGroup` snapshot.
pub struct LanceDataReplacementGroup {
    pub(crate) inner: DataReplacementGroup,
}

/// Options for constructing one native transaction.
///
/// `uuid == NULL` lets Lance generate a v4 UUID. A non-NULL pointer, including
/// an empty string, is passed through unchanged. `tag == NULL` maps to native
/// `None`; an empty non-NULL string maps to `Some("")`.
///
/// `has_transaction_properties == false` maps to native `None`.
/// `has_transaction_properties == true` maps to `Some(map)`, including an
/// explicitly empty map when `property_count == 0`. When the count is non-zero,
/// `property_keys` and `property_values` must both contain that many non-NULL
/// UTF-8 strings. Duplicate keys follow native `HashMap` behavior.
#[repr(C)]
pub struct LanceTransactionOptions {
    pub uuid: *const c_char,
    pub tag: *const c_char,
    pub has_transaction_properties: bool,
    pub property_keys: *const *const c_char,
    pub property_values: *const *const c_char,
    pub property_count: usize,
}

/// Native operation variant.
///
/// Discriminants are ABI-stable and do not depend on Rust enum layout.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceOperationKind {
    Unknown = 0,
    Append = 1,
    Delete = 2,
    Overwrite = 3,
    CreateIndex = 4,
    Rewrite = 5,
    DataReplacement = 6,
    DataOverlay = 7,
    Merge = 8,
    Restore = 9,
    ReserveFragments = 10,
    Update = 11,
    Project = 12,
    UpdateConfig = 13,
    UpdateMemWalState = 14,
    Clone = 15,
    UpdateBases = 16,
}

fn operation_kind(operation: &Operation) -> LanceOperationKind {
    match operation {
        Operation::Append { .. } => LanceOperationKind::Append,
        Operation::Delete { .. } => LanceOperationKind::Delete,
        Operation::Overwrite { .. } => LanceOperationKind::Overwrite,
        Operation::CreateIndex { .. } => LanceOperationKind::CreateIndex,
        Operation::Rewrite { .. } => LanceOperationKind::Rewrite,
        Operation::DataReplacement { .. } => LanceOperationKind::DataReplacement,
        Operation::DataOverlay { .. } => LanceOperationKind::DataOverlay,
        Operation::Merge { .. } => LanceOperationKind::Merge,
        Operation::Restore { .. } => LanceOperationKind::Restore,
        Operation::ReserveFragments { .. } => LanceOperationKind::ReserveFragments,
        Operation::Update { .. } => LanceOperationKind::Update,
        Operation::Project { .. } => LanceOperationKind::Project,
        Operation::UpdateConfig { .. } => LanceOperationKind::UpdateConfig,
        Operation::UpdateMemWalState { .. } => LanceOperationKind::UpdateMemWalState,
        Operation::Clone { .. } => LanceOperationKind::Clone,
        Operation::UpdateBases { .. } => LanceOperationKind::UpdateBases,
        Operation::Unknown { .. } => LanceOperationKind::Unknown,
    }
}

fn transaction_at<'a>(transaction: *const LanceTransaction) -> Result<&'a LanceTransaction> {
    if transaction.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "transaction must not be NULL".into(),
        ));
    }
    Ok(unsafe { &*transaction })
}

pub(crate) fn operation_at<'a>(operation: *const LanceOperation) -> Result<&'a LanceOperation> {
    if operation.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "operation must not be NULL".into(),
        ));
    }
    Ok(unsafe { &*operation })
}

fn data_replacement_group_at<'a>(
    group: *const LanceDataReplacementGroup,
) -> Result<&'a LanceDataReplacementGroup> {
    if group.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "data replacement group must not be NULL".into(),
        ));
    }
    Ok(unsafe { &*group })
}

fn delete_operation_parts(operation: &LanceOperation) -> Result<(&[Fragment], &[u64], &str)> {
    match &operation.inner {
        Operation::Delete {
            updated_fragments,
            deleted_fragment_ids,
            predicate,
        } => Ok((updated_fragments, deleted_fragment_ids, predicate)),
        other => Err(lance_core::Error::not_supported_source(
            format!(
                "Delete inspection is not supported for {} operations",
                other.name()
            )
            .into(),
        )),
    }
}

fn create_index_operation_parts(
    operation: &LanceOperation,
) -> Result<(&[IndexMetadata], &[IndexMetadata])> {
    match &operation.inner {
        Operation::CreateIndex {
            new_indices,
            removed_indices,
        } => Ok((new_indices, removed_indices)),
        other => Err(lance_core::Error::not_supported_source(
            format!(
                "CreateIndex inspection is not supported for {} operations",
                other.name()
            )
            .into(),
        )),
    }
}

fn data_replacement_groups(operation: &LanceOperation) -> Result<&[DataReplacementGroup]> {
    match &operation.inner {
        Operation::DataReplacement { replacements } => Ok(replacements),
        other => Err(lance_core::Error::not_supported_source(
            format!(
                "DataReplacement inspection is not supported for {} operations",
                other.name()
            )
            .into(),
        )),
    }
}

fn owned_c_string(value: &str, label: &str) -> Result<CString> {
    CString::new(value)
        .map_err(|_| lance_core::Error::internal(format!("{label} contains an embedded NUL byte")))
}

fn required_c_string<'a>(ptr: *const c_char, label: &str) -> Result<&'a str> {
    unsafe { helpers::parse_c_string(ptr) }?.ok_or_else(|| {
        lance_core::Error::invalid_input_source(format!("{label} must not be NULL").into())
    })
}

/// Close a transaction handle. NULL-safe. A non-NULL handle must be closed
/// exactly once and must not be used afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_transaction_close(transaction: *mut LanceTransaction) {
    if !transaction.is_null() {
        swallow_unwind("lance_transaction_close", || unsafe {
            let _ = Box::from_raw(transaction);
        });
    }
}

/// Close an operation handle. NULL-safe. Clearing the transaction that
/// produced the operation does not invalidate the operation handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_close(operation: *mut LanceOperation) {
    if !operation.is_null() {
        swallow_unwind("lance_operation_close", || unsafe {
            let _ = Box::from_raw(operation);
        });
    }
}

/// Close a data replacement group handle. NULL-safe. A non-NULL handle must be
/// closed exactly once and must not be used afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_replacement_group_close(group: *mut LanceDataReplacementGroup) {
    if !group.is_null() {
        swallow_unwind("lance_data_replacement_group_close", || unsafe {
            let _ = Box::from_raw(group);
        });
    }
}

/// Construct a native data replacement group.
///
/// `new_file` is borrowed and its complete native metadata is cloned before
/// this function returns. The native fragment ID is copied unchanged; no
/// binding-side ordering, uniqueness or coverage policy is added. Returns NULL
/// on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_replacement_group_new(
    fragment_id: u64,
    new_file: *const LanceDataFile,
) -> *mut LanceDataReplacementGroup {
    ffi_try!(
        unsafe { data_replacement_group_new_inner(fragment_id, new_file) },
        null
    )
}

unsafe fn data_replacement_group_new_inner(
    fragment_id: u64,
    new_file: *const LanceDataFile,
) -> Result<*mut LanceDataReplacementGroup> {
    if new_file.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "new_file must not be NULL".into(),
        ));
    }
    let new_file = unsafe { &*new_file }.native().clone();
    Ok(Box::into_raw(Box::new(LanceDataReplacementGroup {
        inner: DataReplacementGroup(fragment_id, new_file),
    })))
}

/// Clone a data replacement group into an independent snapshot.
///
/// Returns NULL on error. The caller closes the returned handle exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_replacement_group_clone(
    group: *const LanceDataReplacementGroup,
) -> *mut LanceDataReplacementGroup {
    ffi_try!(unsafe { data_replacement_group_clone_inner(group) }, null)
}

unsafe fn data_replacement_group_clone_inner(
    group: *const LanceDataReplacementGroup,
) -> Result<*mut LanceDataReplacementGroup> {
    let group = data_replacement_group_at(group)?;
    Ok(Box::into_raw(Box::new(LanceDataReplacementGroup {
        inner: group.inner.clone(),
    })))
}

/// Read the native fragment ID from a data replacement group.
///
/// Returns 0 on error; check `lance_last_error_code()`. Fragment ID 0 is a
/// valid native value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_replacement_group_fragment_id(
    group: *const LanceDataReplacementGroup,
) -> u64 {
    ffi_try!(Ok(data_replacement_group_at(group)?.inner.0), 0)
}

/// Clone the data file owned by a data replacement group.
///
/// Returns 0 on success, -1 on error. `out_file` is written only on success
/// and must be closed independently.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_replacement_group_file(
    group: *const LanceDataReplacementGroup,
    out_file: *mut *mut LanceDataFile,
) -> i32 {
    ffi_try!(
        unsafe { data_replacement_group_file_inner(group, out_file) },
        neg
    )
}

unsafe fn data_replacement_group_file_inner(
    group: *const LanceDataReplacementGroup,
    out_file: *mut *mut LanceDataFile,
) -> Result<i32> {
    if out_file.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_file must not be NULL".into(),
        ));
    }
    let group = data_replacement_group_at(group)?;
    let file = Box::new(LanceDataFile::from_native(group.inner.1.clone())?);
    unsafe {
        ptr::write(out_file, Box::into_raw(file));
    }
    Ok(0)
}

/// Construct an owned native DataReplacement operation.
///
/// Group handles are borrowed and their native fragment IDs and data files are
/// copied in input order. A NULL array is accepted only with a zero count.
/// Empty replacement lists are retained because the native operation permits
/// them. No sorting, deduplication, non-empty, field-ID or coverage policy is
/// added. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_data_replacement_new(
    replacements: *const *const LanceDataReplacementGroup,
    replacement_count: usize,
) -> *mut LanceOperation {
    ffi_try!(
        unsafe { operation_data_replacement_new_inner(replacements, replacement_count) },
        null
    )
}

unsafe fn operation_data_replacement_new_inner(
    replacements: *const *const LanceDataReplacementGroup,
    replacement_count: usize,
) -> Result<*mut LanceOperation> {
    if replacements.is_null() && replacement_count > 0 {
        return Err(lance_core::Error::invalid_input_source(
            format!("replacements must not be NULL when replacement_count = {replacement_count}")
                .into(),
        ));
    }
    let max_groups = isize::MAX as usize / std::mem::size_of::<*const LanceDataReplacementGroup>();
    if replacement_count > max_groups {
        return Err(lance_core::Error::invalid_input_source(
            format!(
                "replacement_count {replacement_count} exceeds the maximum addressable pointer slice length"
            )
            .into(),
        ));
    }

    let replacements = if replacement_count == 0 {
        Vec::new()
    } else {
        let groups = unsafe { std::slice::from_raw_parts(replacements, replacement_count) };
        groups
            .iter()
            .enumerate()
            .map(|(index, group)| {
                if group.is_null() {
                    return Err(lance_core::Error::invalid_input_source(
                        format!(
                            "replacements[{index}] must not be NULL when replacement_count = {replacement_count}"
                        )
                        .into(),
                    ));
                }
                Ok(unsafe { &**group }.inner.clone())
            })
            .collect::<Result<Vec<_>>>()?
    };

    Ok(Box::into_raw(Box::new(LanceOperation::from_native(
        Operation::DataReplacement { replacements },
    ))))
}

/// Construct an owned native DataOverlay operation.
///
/// Group handles are borrowed and cloned in input order. A NULL array is
/// accepted only with a zero count. Empty groups and empty group lists are
/// retained because the native operation permits them. Native code owns
/// fragment existence, field compatibility, coverage validity, layout, and
/// ordering checks. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_data_overlay_new(
    groups: *const *const LanceDataOverlayGroup,
    group_count: usize,
) -> *mut LanceOperation {
    ffi_try!(
        unsafe { operation_data_overlay_new_inner(groups, group_count) },
        null
    )
}

unsafe fn operation_data_overlay_new_inner(
    groups: *const *const LanceDataOverlayGroup,
    group_count: usize,
) -> Result<*mut LanceOperation> {
    if groups.is_null() && group_count > 0 {
        return Err(lance_core::Error::invalid_input_source(
            format!("groups must not be NULL when group_count = {group_count}").into(),
        ));
    }
    let max_groups = isize::MAX as usize / std::mem::size_of::<*const LanceDataOverlayGroup>();
    if group_count > max_groups {
        return Err(lance_core::Error::invalid_input_source(
            format!(
                "group_count {group_count} exceeds the maximum addressable pointer slice length"
            )
            .into(),
        ));
    }

    let groups = if group_count == 0 {
        Vec::new()
    } else {
        let groups = unsafe { std::slice::from_raw_parts(groups, group_count) };
        groups
            .iter()
            .enumerate()
            .map(|(index, group)| {
                if group.is_null() {
                    return Err(lance_core::Error::invalid_input_source(
                        format!(
                            "groups[{index}] must not be NULL when group_count = {group_count}"
                        )
                        .into(),
                    ));
                }
                Ok(unsafe { &**group }.inner.clone())
            })
            .collect::<Result<Vec<_>>>()?
    };

    Ok(Box::into_raw(Box::new(LanceOperation::from_native(
        Operation::DataOverlay { groups },
    ))))
}

/// Construct an owned native Delete operation.
///
/// Updated fragment handles are borrowed; their complete native metadata is
/// copied before this function returns. Deleted IDs and the predicate are also
/// copied. NULL arrays are accepted only with a zero count. The predicate must
/// be non-NULL UTF-8; an empty predicate is passed through.
///
/// Native validation owns fragment existence, uniqueness, overlap and predicate
/// semantics. This constructor does not add binding-side policies. Returns NULL
/// on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_delete_new(
    updated_fragments: *const *const LanceFragment,
    updated_fragment_count: usize,
    deleted_fragment_ids: *const u64,
    deleted_fragment_count: usize,
    predicate: *const c_char,
) -> *mut LanceOperation {
    ffi_try!(
        unsafe {
            operation_delete_new_inner(
                updated_fragments,
                updated_fragment_count,
                deleted_fragment_ids,
                deleted_fragment_count,
                predicate,
            )
        },
        null
    )
}

unsafe fn operation_delete_new_inner(
    updated_fragments: *const *const LanceFragment,
    updated_fragment_count: usize,
    deleted_fragment_ids: *const u64,
    deleted_fragment_count: usize,
    predicate: *const c_char,
) -> Result<*mut LanceOperation> {
    if updated_fragments.is_null() && updated_fragment_count > 0 {
        return Err(lance_core::Error::invalid_input_source(
            format!(
                "updated_fragments must not be NULL when updated_fragment_count = {updated_fragment_count}"
            )
            .into(),
        ));
    }
    if deleted_fragment_ids.is_null() && deleted_fragment_count > 0 {
        return Err(lance_core::Error::invalid_input_source(
            format!(
                "deleted_fragment_ids must not be NULL when deleted_fragment_count = {deleted_fragment_count}"
            )
            .into(),
        ));
    }
    let predicate = required_c_string(predicate, "predicate")?;

    let updated_fragments = if updated_fragment_count == 0 {
        Vec::new()
    } else {
        let fragments =
            unsafe { std::slice::from_raw_parts(updated_fragments, updated_fragment_count) };
        fragments
            .iter()
            .enumerate()
            .map(|(index, fragment)| {
                if fragment.is_null() {
                    return Err(lance_core::Error::invalid_input_source(
                        format!(
                            "updated_fragments[{index}] must not be NULL when updated_fragment_count = {updated_fragment_count}"
                        )
                        .into(),
                    ));
                }
                Ok(unsafe { &**fragment }.metadata().clone())
            })
            .collect::<Result<Vec<_>>>()?
    };
    let deleted_fragment_ids = if deleted_fragment_count == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(deleted_fragment_ids, deleted_fragment_count) }.to_vec()
    };

    Ok(Box::into_raw(Box::new(LanceOperation::from_native(
        Operation::Delete {
            updated_fragments,
            deleted_fragment_ids,
            predicate: predicate.to_string(),
        },
    ))))
}

unsafe fn clone_index_metadata_array(
    metadata: *const *const LanceIndexSegmentMetadata,
    count: usize,
    label: &str,
) -> Result<Vec<IndexMetadata>> {
    if metadata.is_null() {
        if count == 0 {
            return Ok(Vec::new());
        }
        return Err(lance_core::Error::invalid_input_source(
            format!("{label} must not be NULL when {label}_count = {count}").into(),
        ));
    }
    if count == 0 {
        return Ok(Vec::new());
    }
    if count > isize::MAX as usize / std::mem::size_of::<*const LanceIndexSegmentMetadata>() {
        return Err(lance_core::Error::invalid_input_source(
            format!("{label}_count {count} exceeds the maximum addressable pointer slice length")
                .into(),
        ));
    }
    let metadata = unsafe { std::slice::from_raw_parts(metadata, count) };
    let mut out = Vec::with_capacity(count);
    for (position, entry) in metadata.iter().enumerate() {
        if entry.is_null() {
            return Err(lance_core::Error::invalid_input_source(
                format!("{label}[{position}] must not be NULL when {label}_count = {count}").into(),
            ));
        }
        out.push(unsafe { &**entry }.native_metadata().clone());
    }
    Ok(out)
}

/// Construct an owned native CreateIndex operation.
///
/// `new_indices` and `removed_indices` are borrowed arrays of metadata handles;
/// every entry is cloned before this function returns. A NULL array is valid
/// only with a zero count, and a non-NULL array with a zero count is ignored.
/// Native `Operation::CreateIndex` owns index replacement, name handling and
/// coverage semantics; this constructor does not add binding-side validation.
/// Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_create_index_new(
    new_indices: *const *const LanceIndexSegmentMetadata,
    new_index_count: usize,
    removed_indices: *const *const LanceIndexSegmentMetadata,
    removed_index_count: usize,
) -> *mut LanceOperation {
    ffi_try!(
        unsafe {
            operation_create_index_new_inner(
                new_indices,
                new_index_count,
                removed_indices,
                removed_index_count,
            )
        },
        null
    )
}

unsafe fn operation_create_index_new_inner(
    new_indices: *const *const LanceIndexSegmentMetadata,
    new_index_count: usize,
    removed_indices: *const *const LanceIndexSegmentMetadata,
    removed_index_count: usize,
) -> Result<*mut LanceOperation> {
    let new_indices =
        unsafe { clone_index_metadata_array(new_indices, new_index_count, "new_indices") }?;
    let removed_indices = unsafe {
        clone_index_metadata_array(removed_indices, removed_index_count, "removed_indices")
    }?;
    Ok(Box::into_raw(Box::new(LanceOperation::from_native(
        Operation::CreateIndex {
            new_indices,
            removed_indices,
        },
    ))))
}

/// Clone a borrowed array of fragment handles into complete native metadata.
unsafe fn clone_fragment_array(
    fragments: *const *const LanceFragment,
    fragment_count: usize,
    label: &str,
) -> Result<Vec<Fragment>> {
    if fragments.is_null() {
        if fragment_count == 0 {
            return Ok(Vec::new());
        }
        return Err(lance_core::Error::invalid_input_source(
            format!("{label} must not be NULL when {label}_count = {fragment_count}").into(),
        ));
    }
    if fragment_count == 0 {
        return Ok(Vec::new());
    }
    if fragment_count > isize::MAX as usize / std::mem::size_of::<*const LanceFragment>() {
        return Err(lance_core::Error::invalid_input_source(
            format!(
                "fragment_count {fragment_count} exceeds the maximum addressable pointer slice length"
            )
            .into(),
        ));
    }
    if fragment_count > isize::MAX as usize / std::mem::size_of::<Fragment>() {
        return Err(lance_core::Error::invalid_input_source(
            format!("fragment_count {fragment_count} exceeds the maximum cloneable fragment count")
                .into(),
        ));
    }
    let fragments = unsafe { std::slice::from_raw_parts(fragments, fragment_count) };
    let mut cloned = Vec::with_capacity(fragment_count);
    for (index, fragment) in fragments.iter().enumerate() {
        if fragment.is_null() {
            return Err(lance_core::Error::invalid_input_source(
                format!("{label}[{index}] must not be NULL when {label}_count = {fragment_count}")
                    .into(),
            ));
        }
        cloned.push(unsafe { &**fragment }.metadata().clone());
    }
    Ok(cloned)
}

/// Construct an owned native Append operation.
///
/// Every fragment handle is borrowed; its complete native metadata, including
/// pre-assigned IDs, row ID metadata and data files, is copied before this
/// function returns. NULL arrays are accepted only with a zero count. Native
/// commit validation owns schema coverage, ID assignment and row-ID assignment;
/// explicit fields and non-zero reserved IDs are passed through unchanged.
/// Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_append_new(
    fragments: *const *const LanceFragment,
    fragment_count: usize,
) -> *mut LanceOperation {
    ffi_try!(
        unsafe { operation_append_new_inner(fragments, fragment_count) },
        null
    )
}

unsafe fn operation_append_new_inner(
    fragments: *const *const LanceFragment,
    fragment_count: usize,
) -> Result<*mut LanceOperation> {
    let fragments = unsafe { clone_fragment_array(fragments, fragment_count, "fragments") }?;
    Ok(Box::into_raw(Box::new(LanceOperation::from_native(
        Operation::Append { fragments },
    ))))
}

/// Construct an owned native Restore operation.
///
/// `version` is passed through unchanged; native commit validation owns whether
/// the version exists and is restorable. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_restore_new(version: u64) -> *mut LanceOperation {
    ffi_try!(
        {
            Ok(Box::into_raw(Box::new(LanceOperation::from_native(
                Operation::Restore { version },
            ))))
        },
        null
    )
}

/// Construct an owned native ReserveFragments operation.
///
/// `num_fragments == 0` is a native no-op version; larger values advance the
/// manifest's maximum fragment ID so later commits allocate from the reserved
/// range. Allocation remains native. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_reserve_fragments_new(
    num_fragments: u32,
) -> *mut LanceOperation {
    ffi_try!(
        {
            Ok(Box::into_raw(Box::new(LanceOperation::from_native(
                Operation::ReserveFragments { num_fragments },
            ))))
        },
        null
    )
}

/// Construct an owned native UpdateBases operation.
///
/// Each `LanceBasePath` entry is parsed into native `BasePath` storage that the
/// returned operation owns. NULL arrays are accepted only with a zero count.
/// Native commit validation owns conflict detection and zero-ID allocation;
/// this constructor does not add binding-side policies. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_bases_new(
    new_bases: *const LanceBasePath,
    new_base_count: usize,
) -> *mut LanceOperation {
    ffi_try!(
        unsafe { operation_update_bases_new_inner(new_bases, new_base_count) },
        null
    )
}

unsafe fn operation_update_bases_new_inner(
    new_bases: *const LanceBasePath,
    new_base_count: usize,
) -> Result<*mut LanceOperation> {
    let new_bases = unsafe { parse_base_paths(new_bases, new_base_count) }?;
    Ok(Box::into_raw(Box::new(LanceOperation::from_native(
        Operation::UpdateBases { new_bases },
    ))))
}

/// Construct an owned native transaction from an operation.
///
/// The operation is cloned and remains usable. `options == NULL` selects native
/// defaults: a generated UUID, no tag and no transaction properties. Native
/// `TransactionBuilder` construction and later commit validation remain
/// authoritative. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_transaction_new(
    read_version: u64,
    operation: *const LanceOperation,
    options: *const LanceTransactionOptions,
) -> *mut LanceTransaction {
    ffi_try!(
        unsafe { transaction_new_inner(read_version, operation, options) },
        null
    )
}

unsafe fn transaction_new_inner(
    read_version: u64,
    operation: *const LanceOperation,
    options: *const LanceTransactionOptions,
) -> Result<*mut LanceTransaction> {
    let operation = operation_at(operation)?.inner.clone();
    let mut builder = TransactionBuilder::new(read_version, operation);
    if !options.is_null() {
        let options = unsafe { &*options };
        if !options.uuid.is_null() {
            builder = builder.uuid(required_c_string(options.uuid, "uuid")?.to_string());
        }
        let tag = unsafe { helpers::parse_c_string(options.tag) }?.map(str::to_string);
        builder = builder.tag(tag);
        if options.has_transaction_properties {
            if options.property_count > 0
                && (options.property_keys.is_null() || options.property_values.is_null())
            {
                return Err(lance_core::Error::invalid_input_source(
                    format!(
                        "property_keys and property_values must not be NULL when property_count = {}",
                        options.property_count
                    )
                    .into(),
                ));
            }
            let mut properties = HashMap::with_capacity(options.property_count);
            for index in 0..options.property_count {
                let key = unsafe { *options.property_keys.add(index) };
                let value = unsafe { *options.property_values.add(index) };
                if key.is_null() || value.is_null() {
                    return Err(lance_core::Error::invalid_input_source(
                        format!(
                            "transaction property {index} must have non-NULL key and value pointers"
                        )
                        .into(),
                    ));
                }
                let key = required_c_string(key, "transaction property key")?;
                let value = required_c_string(value, "transaction property value")?;
                properties.insert(key.to_string(), value.to_string());
            }
            builder = builder.transaction_properties(Some(Arc::new(properties)));
        }
    }
    Ok(Box::into_raw(Box::new(LanceTransaction {
        inner: builder.build(),
    })))
}

/// Read the transaction's native read version into `out_read_version`.
///
/// Returns 0 on success, -1 on error. `out_read_version` is written only on
/// success; 0 is a valid native value for an uncommitted create transaction.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_transaction_read_version(
    transaction: *const LanceTransaction,
    out_read_version: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { transaction_read_version_inner(transaction, out_read_version) },
        neg
    )
}

unsafe fn transaction_read_version_inner(
    transaction: *const LanceTransaction,
    out_read_version: *mut u64,
) -> Result<i32> {
    if out_read_version.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_read_version must not be NULL".into(),
        ));
    }
    let transaction = transaction_at(transaction)?;
    unsafe {
        ptr::write(out_read_version, transaction.inner.read_version);
    }
    Ok(0)
}

/// Return a newly allocated copy of the transaction UUID.
///
/// The caller must free the returned string with `lance_free_string()`.
/// Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_transaction_uuid(
    transaction: *const LanceTransaction,
) -> *const c_char {
    ffi_try!(
        unsafe { transaction_uuid_inner(transaction) },
        std::ptr::null()
    )
}

unsafe fn transaction_uuid_inner(transaction: *const LanceTransaction) -> Result<*const c_char> {
    let transaction = transaction_at(transaction)?;
    Ok(owned_c_string(&transaction.inner.uuid, "transaction UUID")?.into_raw())
}

/// Copy the optional transaction tag.
///
/// Returns 1 and allocates `*out_tag` when a tag is present, 0 and writes NULL
/// when absent, or -1 on error. The caller frees a present string with
/// `lance_free_string()`. A tag is a native `Option<String>`; an absent tag is
/// distinct from an empty string. The protobuf conversion normalizes an empty
/// encoded tag back to absent.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_transaction_tag(
    transaction: *const LanceTransaction,
    out_tag: *mut *const c_char,
) -> i32 {
    ffi_try!(unsafe { transaction_tag_inner(transaction, out_tag) }, neg)
}

unsafe fn transaction_tag_inner(
    transaction: *const LanceTransaction,
    out_tag: *mut *const c_char,
) -> Result<i32> {
    if out_tag.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_tag must not be NULL".into(),
        ));
    }
    let transaction = transaction_at(transaction)?;
    match transaction.inner.tag.as_deref() {
        Some(tag) => {
            let tag = owned_c_string(tag, "transaction tag")?;
            unsafe {
                ptr::write(out_tag, tag.into_raw());
            }
            Ok(1)
        }
        None => {
            unsafe {
                ptr::write(out_tag, ptr::null());
            }
            Ok(0)
        }
    }
}

/// Report whether transaction properties are present.
///
/// Native `Option<Arc<HashMap<...>>>` distinguishes `None` from
/// `Some(empty)`. This function preserves that distinction. Returns 0 on
/// success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_transaction_has_properties(
    transaction: *const LanceTransaction,
    out_has_properties: *mut bool,
) -> i32 {
    ffi_try!(
        unsafe { transaction_has_properties_inner(transaction, out_has_properties) },
        neg
    )
}

unsafe fn transaction_has_properties_inner(
    transaction: *const LanceTransaction,
    out_has_properties: *mut bool,
) -> Result<i32> {
    if out_has_properties.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_has_properties must not be NULL".into(),
        ));
    }
    let transaction = transaction_at(transaction)?;
    unsafe {
        ptr::write(
            out_has_properties,
            transaction.inner.transaction_properties.is_some(),
        );
    }
    Ok(0)
}

/// Read the number of transaction properties into `out_count`.
///
/// Returns 0 on success, -1 on error. The count is 0 for both `None` and
/// `Some(empty)`; use `lance_transaction_has_properties()` to distinguish.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_transaction_property_count(
    transaction: *const LanceTransaction,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { transaction_property_count_inner(transaction, out_count) },
        neg
    )
}

unsafe fn transaction_property_count_inner(
    transaction: *const LanceTransaction,
    out_count: *mut u64,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let transaction = transaction_at(transaction)?;
    let count = transaction
        .inner
        .transaction_properties
        .as_ref()
        .map_or(0, |properties| properties.len());
    unsafe {
        ptr::write(out_count, count as u64);
    }
    Ok(0)
}

fn property_at(transaction: &LanceTransaction, index: usize) -> Result<(&str, &str)> {
    let properties = transaction
        .inner
        .transaction_properties
        .as_ref()
        .ok_or_else(|| {
            lance_core::Error::invalid_input_source(
                "transaction has no transaction_properties map".into(),
            )
        })?;
    properties
        .iter()
        .nth(index)
        .map(|(key, value)| (key.as_str(), value.as_str()))
        .ok_or_else(|| {
            lance_core::Error::invalid_input_source(
                format!(
                    "property index {index} is out of range for {} properties",
                    properties.len()
                )
                .into(),
            )
        })
}

/// Return a newly allocated property key at `index`.
///
/// Map enumeration order is unspecified but stable for this handle. The caller
/// frees the returned string with `lance_free_string()`. Returns NULL on error,
/// including an out-of-range index.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_transaction_property_key_at(
    transaction: *const LanceTransaction,
    index: usize,
) -> *const c_char {
    ffi_try!(
        unsafe { transaction_property_key_at_inner(transaction, index) },
        std::ptr::null()
    )
}

unsafe fn transaction_property_key_at_inner(
    transaction: *const LanceTransaction,
    index: usize,
) -> Result<*const c_char> {
    let transaction = transaction_at(transaction)?;
    let (key, _) = property_at(transaction, index)?;
    Ok(owned_c_string(key, "transaction property key")?.into_raw())
}

/// Return a newly allocated property value at `index`.
///
/// Map enumeration order is unspecified but stable for this handle. The caller
/// frees the returned string with `lance_free_string()`. Returns NULL on error,
/// including an out-of-range index.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_transaction_property_value_at(
    transaction: *const LanceTransaction,
    index: usize,
) -> *const c_char {
    ffi_try!(
        unsafe { transaction_property_value_at_inner(transaction, index) },
        std::ptr::null()
    )
}

unsafe fn transaction_property_value_at_inner(
    transaction: *const LanceTransaction,
    index: usize,
) -> Result<*const c_char> {
    let transaction = transaction_at(transaction)?;
    let (_, value) = property_at(transaction, index)?;
    Ok(owned_c_string(value, "transaction property value")?.into_raw())
}

/// Clone the transaction's operation into an independently owned handle.
///
/// Returns 0 on success and writes `*out_operation`; -1 on error and leaves
/// the output slot unchanged. The operation remains valid after the source
/// transaction is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_transaction_operation(
    transaction: *const LanceTransaction,
    out_operation: *mut *mut LanceOperation,
) -> i32 {
    ffi_try!(
        unsafe { transaction_operation_inner(transaction, out_operation) },
        neg
    )
}

unsafe fn transaction_operation_inner(
    transaction: *const LanceTransaction,
    out_operation: *mut *mut LanceOperation,
) -> Result<i32> {
    if out_operation.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_operation must not be NULL".into(),
        ));
    }
    let transaction = transaction_at(transaction)?;
    let operation = Box::new(LanceOperation::from_native(
        transaction.inner.operation.clone(),
    ));
    unsafe {
        ptr::write(out_operation, Box::into_raw(operation));
    }
    Ok(0)
}

/// Read the operation variant into `out_kind`.
///
/// Unknown is both the decoded `Operation::Unknown` variant and the fallback
/// for a future variant unknown to this binding. Returns 0 on success, -1 on
/// error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_kind(
    operation: *const LanceOperation,
    out_kind: *mut LanceOperationKind,
) -> i32 {
    ffi_try!(unsafe { operation_kind_inner(operation, out_kind) }, neg)
}

unsafe fn operation_kind_inner(
    operation: *const LanceOperation,
    out_kind: *mut LanceOperationKind,
) -> Result<i32> {
    if out_kind.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_kind must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    unsafe {
        ptr::write(out_kind, operation_kind(&operation.inner));
    }
    Ok(0)
}

/// Read the fragment count for an Append or Overwrite operation.
///
/// Returns 0 on success, -1 on error. Other operation variants return
/// `LANCE_ERR_NOT_SUPPORTED`; use their typed getters.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_fragment_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_fragment_count_inner(operation, out_count) },
        neg
    )
}

unsafe fn operation_fragment_count_inner(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let count = match &operation.inner {
        Operation::Append { fragments } | Operation::Overwrite { fragments, .. } => {
            u64::try_from(fragments.len()).unwrap_or(u64::MAX)
        }
        other => {
            return Err(lance_core::Error::not_supported_source(
                format!(
                    "fragment inspection is not supported for {} operations",
                    other.name()
                )
                .into(),
            ));
        }
    };
    unsafe {
        ptr::write(out_count, count);
    }
    Ok(0)
}

/// Read the number of new index metadata entries in a CreateIndex operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_create_index_new_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_create_index_new_count_inner(operation, out_count) },
        neg
    )
}

unsafe fn operation_create_index_new_count_inner(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let (new_indices, _) = create_index_operation_parts(operation)?;
    unsafe {
        ptr::write(out_count, new_indices.len() as u64);
    }
    Ok(0)
}

/// Clone one new index metadata entry from a CreateIndex operation.
///
/// Returns 0 on success and writes `*out_metadata`; -1 on error and leaves the
/// output slot unchanged. The returned metadata remains valid after the
/// operation handle is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_create_index_new_at(
    operation: *const LanceOperation,
    index: usize,
    out_metadata: *mut *mut LanceIndexSegmentMetadata,
) -> i32 {
    ffi_try!(
        unsafe { operation_create_index_new_at_inner(operation, index, out_metadata) },
        neg
    )
}

unsafe fn operation_create_index_new_at_inner(
    operation: *const LanceOperation,
    index: usize,
    out_metadata: *mut *mut LanceIndexSegmentMetadata,
) -> Result<i32> {
    if out_metadata.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_metadata must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let (new_indices, _) = create_index_operation_parts(operation)?;
    let metadata = new_indices.get(index).ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!(
                "new index metadata index {index} is out of range for {} entries",
                new_indices.len()
            )
            .into(),
        )
    })?;
    let metadata = metadata_handle(metadata.clone())?;
    unsafe {
        ptr::write(out_metadata, metadata);
    }
    Ok(0)
}

/// Read the number of removed index metadata entries in a CreateIndex
/// operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_create_index_removed_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_create_index_removed_count_inner(operation, out_count) },
        neg
    )
}

unsafe fn operation_create_index_removed_count_inner(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let (_, removed_indices) = create_index_operation_parts(operation)?;
    unsafe {
        ptr::write(out_count, removed_indices.len() as u64);
    }
    Ok(0)
}

/// Clone one removed index metadata entry from a CreateIndex operation.
///
/// Returns 0 on success and writes `*out_metadata`; -1 on error and leaves the
/// output slot unchanged. The returned metadata remains valid after the
/// operation handle is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_create_index_removed_at(
    operation: *const LanceOperation,
    index: usize,
    out_metadata: *mut *mut LanceIndexSegmentMetadata,
) -> i32 {
    ffi_try!(
        unsafe { operation_create_index_removed_at_inner(operation, index, out_metadata) },
        neg
    )
}

unsafe fn operation_create_index_removed_at_inner(
    operation: *const LanceOperation,
    index: usize,
    out_metadata: *mut *mut LanceIndexSegmentMetadata,
) -> Result<i32> {
    if out_metadata.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_metadata must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let (_, removed_indices) = create_index_operation_parts(operation)?;
    let metadata = removed_indices.get(index).ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!(
                "removed index metadata index {index} is out of range for {} entries",
                removed_indices.len()
            )
            .into(),
        )
    })?;
    let metadata = metadata_handle(metadata.clone())?;
    unsafe {
        ptr::write(out_metadata, metadata);
    }
    Ok(0)
}

/// Read the number of updated fragments in a Delete operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_delete_updated_fragment_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_delete_updated_fragment_count_inner(operation, out_count) },
        neg
    )
}

unsafe fn operation_delete_updated_fragment_count_inner(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let (updated, _, _) = delete_operation_parts(operation)?;
    unsafe {
        ptr::write(out_count, updated.len() as u64);
    }
    Ok(0)
}

/// Serialize one updated Delete fragment's native metadata as JSON.
///
/// This uses the same native `Fragment` serialization as
/// `lance_fragment_metadata_json`. Caller frees with `lance_free_string()`.
/// Returns NULL on error, including a wrong operation kind or bad index.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_delete_updated_fragment_metadata_json_at(
    operation: *const LanceOperation,
    index: usize,
) -> *const c_char {
    ffi_try!(
        unsafe { operation_delete_updated_fragment_metadata_json_at_inner(operation, index) },
        null
    )
}

unsafe fn operation_delete_updated_fragment_metadata_json_at_inner(
    operation: *const LanceOperation,
    index: usize,
) -> Result<*const c_char> {
    let operation = operation_at(operation)?;
    let (updated, _, _) = delete_operation_parts(operation)?;
    let fragment = updated.get(index).ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!(
                "updated fragment index {index} is out of range for {} fragments",
                updated.len()
            )
            .into(),
        )
    })?;
    fragment_metadata_json(fragment)
}

/// Read the number of fully deleted fragment IDs in a Delete operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_delete_deleted_fragment_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_delete_deleted_fragment_count_inner(operation, out_count) },
        neg
    )
}

unsafe fn operation_delete_deleted_fragment_count_inner(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let (_, deleted, _) = delete_operation_parts(operation)?;
    unsafe {
        ptr::write(out_count, deleted.len() as u64);
    }
    Ok(0)
}

/// Read one fully deleted fragment ID from a Delete operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_delete_deleted_fragment_id_at(
    operation: *const LanceOperation,
    index: usize,
    out_id: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_delete_deleted_fragment_id_at_inner(operation, index, out_id) },
        neg
    )
}

unsafe fn operation_delete_deleted_fragment_id_at_inner(
    operation: *const LanceOperation,
    index: usize,
    out_id: *mut u64,
) -> Result<i32> {
    if out_id.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_id must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let (_, deleted, _) = delete_operation_parts(operation)?;
    let id = deleted.get(index).ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!(
                "deleted fragment index {index} is out of range for {} fragments",
                deleted.len()
            )
            .into(),
        )
    })?;
    unsafe {
        ptr::write(out_id, *id);
    }
    Ok(0)
}

/// Return a newly allocated copy of a Delete operation's predicate.
///
/// Caller frees with `lance_free_string()`. Returns NULL on error, including a
/// wrong operation kind.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_delete_predicate(
    operation: *const LanceOperation,
) -> *const c_char {
    ffi_try!(
        unsafe { operation_delete_predicate_inner(operation) },
        std::ptr::null()
    )
}

unsafe fn operation_delete_predicate_inner(
    operation: *const LanceOperation,
) -> Result<*const c_char> {
    let operation = operation_at(operation)?;
    let (_, _, predicate) = delete_operation_parts(operation)?;
    Ok(owned_c_string(predicate, "Delete predicate")?.into_raw())
}

/// Read the number of data replacement groups.
///
/// Returns 0 on success, -1 on error. A non-DataReplacement operation returns
/// `LANCE_ERR_NOT_SUPPORTED`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_data_replacement_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_data_replacement_count_inner(operation, out_count) },
        neg
    )
}

unsafe fn operation_data_replacement_count_inner(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let replacements = data_replacement_groups(operation)?;
    unsafe {
        ptr::write(out_count, replacements.len() as u64);
    }
    Ok(0)
}

/// Clone one data replacement group from a DataReplacement operation.
///
/// Returns 0 on success, -1 on error. The output is written only on success and
/// must be closed independently. A wrong operation variant returns
/// `LANCE_ERR_NOT_SUPPORTED`; an out-of-range index returns
/// `LANCE_ERR_INVALID_ARGUMENT`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_data_replacement_at(
    operation: *const LanceOperation,
    index: usize,
    out_group: *mut *mut LanceDataReplacementGroup,
) -> i32 {
    ffi_try!(
        unsafe { operation_data_replacement_at_inner(operation, index, out_group) },
        neg
    )
}

unsafe fn operation_data_replacement_at_inner(
    operation: *const LanceOperation,
    index: usize,
    out_group: *mut *mut LanceDataReplacementGroup,
) -> Result<i32> {
    if out_group.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_group must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let replacements = data_replacement_groups(operation)?;
    let group = replacements.get(index).ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!(
                "data replacement index {index} is out of range for {} replacements",
                replacements.len()
            )
            .into(),
        )
    })?;
    let group = Box::new(LanceDataReplacementGroup {
        inner: group.clone(),
    });
    unsafe {
        ptr::write(out_group, Box::into_raw(group));
    }
    Ok(0)
}

/// Read the number of data overlay groups.
///
/// Returns 0 on success, -1 on error. A non-DataOverlay operation returns
/// `LANCE_ERR_NOT_SUPPORTED`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_data_overlay_group_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_data_overlay_group_count_inner(operation, out_count) },
        neg
    )
}

unsafe fn operation_data_overlay_group_count_inner(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let groups = data_overlay_groups(operation)?;
    unsafe {
        ptr::write(out_count, groups.len() as u64);
    }
    Ok(0)
}

/// Clone one data overlay group from a DataOverlay operation.
///
/// Returns 0 on success, -1 on error. The output is written only on success and
/// must be closed independently. A wrong operation variant returns
/// `LANCE_ERR_NOT_SUPPORTED`; an out-of-range index returns
/// `LANCE_ERR_INVALID_ARGUMENT`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_data_overlay_group_at(
    operation: *const LanceOperation,
    index: usize,
    out_group: *mut *mut LanceDataOverlayGroup,
) -> i32 {
    ffi_try!(
        unsafe { operation_data_overlay_group_at_inner(operation, index, out_group) },
        neg
    )
}

unsafe fn operation_data_overlay_group_at_inner(
    operation: *const LanceOperation,
    index: usize,
    out_group: *mut *mut LanceDataOverlayGroup,
) -> Result<i32> {
    if out_group.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_group must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let groups = data_overlay_groups(operation)?;
    let group = groups.get(index).ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!(
                "data overlay group index {index} is out of range for {} groups",
                groups.len()
            )
            .into(),
        )
    })?;
    let group = Box::new(LanceDataOverlayGroup {
        inner: group.clone(),
    });
    unsafe {
        ptr::write(out_group, Box::into_raw(group));
    }
    Ok(0)
}

fn data_overlay_groups(operation: &LanceOperation) -> Result<&[DataOverlayGroup]> {
    match &operation.inner {
        Operation::DataOverlay { groups } => Ok(groups),
        other => Err(lance_core::Error::not_supported_source(
            format!(
                "DataOverlay inspection is not supported for {} operations",
                other.name()
            )
            .into(),
        )),
    }
}

fn append_operation_parts(operation: &LanceOperation) -> Result<&[Fragment]> {
    match &operation.inner {
        Operation::Append { fragments } => Ok(fragments),
        other => Err(lance_core::Error::not_supported_source(
            format!(
                "Append inspection is not supported for {} operations",
                other.name()
            )
            .into(),
        )),
    }
}

fn restore_operation_version(operation: &LanceOperation) -> Result<u64> {
    match &operation.inner {
        Operation::Restore { version } => Ok(*version),
        other => Err(lance_core::Error::not_supported_source(
            format!(
                "Restore inspection is not supported for {} operations",
                other.name()
            )
            .into(),
        )),
    }
}

fn reserve_fragments_operation_count(operation: &LanceOperation) -> Result<u32> {
    match &operation.inner {
        Operation::ReserveFragments { num_fragments } => Ok(*num_fragments),
        other => Err(lance_core::Error::not_supported_source(
            format!(
                "ReserveFragments inspection is not supported for {} operations",
                other.name()
            )
            .into(),
        )),
    }
}

fn update_bases_operation_parts(operation: &LanceOperation) -> Result<&[BasePath]> {
    match &operation.inner {
        Operation::UpdateBases { new_bases } => Ok(new_bases),
        other => Err(lance_core::Error::not_supported_source(
            format!(
                "UpdateBases inspection is not supported for {} operations",
                other.name()
            )
            .into(),
        )),
    }
}

/// Read the number of fragments in an Append operation.
///
/// Returns 0 on success, -1 on error. Other variants return
/// `LANCE_ERR_NOT_SUPPORTED`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_append_fragment_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_append_fragment_count_inner(operation, out_count) },
        neg
    )
}

unsafe fn operation_append_fragment_count_inner(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let fragments = append_operation_parts(operation)?;
    unsafe {
        ptr::write(out_count, fragments.len() as u64);
    }
    Ok(0)
}

/// Clone one fragment from an Append operation as an independent detached
/// handle.
///
/// The returned handle owns the complete native `Fragment` metadata and remains
/// valid after this operation and every source dataset are closed. It supports
/// metadata inspection and operation construction but not dataset-dependent
/// reads. Missing indices are invalid arguments. Returns 0 on success, -1 on
/// error, and leaves `out_fragment` unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_append_fragment_at(
    operation: *const LanceOperation,
    index: usize,
    out_fragment: *mut *mut LanceFragment,
) -> i32 {
    ffi_try!(
        unsafe { operation_append_fragment_at_inner(operation, index, out_fragment) },
        neg
    )
}

unsafe fn operation_append_fragment_at_inner(
    operation: *const LanceOperation,
    index: usize,
    out_fragment: *mut *mut LanceFragment,
) -> Result<i32> {
    if out_fragment.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_fragment must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let fragments = append_operation_parts(operation)?;
    let fragment = fragments.get(index).ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!(
                "append fragment index {index} is out of range for {} fragments",
                fragments.len()
            )
            .into(),
        )
    })?;
    let fragment = Box::into_raw(Box::new(LanceFragment::from_metadata(fragment.clone())));
    unsafe {
        ptr::write(out_fragment, fragment);
    }
    Ok(0)
}

/// Read the target version of a Restore operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_restore_version(
    operation: *const LanceOperation,
    out_version: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_restore_version_inner(operation, out_version) },
        neg
    )
}

unsafe fn operation_restore_version_inner(
    operation: *const LanceOperation,
    out_version: *mut u64,
) -> Result<i32> {
    if out_version.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_version must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let version = restore_operation_version(operation)?;
    unsafe {
        ptr::write(out_version, version);
    }
    Ok(0)
}

/// Read the reserved fragment count of a ReserveFragments operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_reserve_fragments_count(
    operation: *const LanceOperation,
    out_count: *mut u32,
) -> i32 {
    ffi_try!(
        unsafe { operation_reserve_fragments_count_inner(operation, out_count) },
        neg
    )
}

unsafe fn operation_reserve_fragments_count_inner(
    operation: *const LanceOperation,
    out_count: *mut u32,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let count = reserve_fragments_operation_count(operation)?;
    unsafe {
        ptr::write(out_count, count);
    }
    Ok(0)
}

/// Read the number of new base paths in an UpdateBases operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_bases_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_update_bases_count_inner(operation, out_count) },
        neg
    )
}

unsafe fn operation_update_bases_count_inner(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let new_bases = update_bases_operation_parts(operation)?;
    unsafe {
        ptr::write(out_count, new_bases.len() as u64);
    }
    Ok(0)
}

/// Read one borrowed new base path from an UpdateBases operation.
///
/// `path` and `name` point into the operation handle and remain valid until it
/// is closed. `name == NULL` represents native `None`; an empty string is an
/// explicit empty name. Returns 0 on success, -1 on error, and leaves
/// `out_base` unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_bases_at(
    operation: *const LanceOperation,
    index: usize,
    out_base: *mut LanceBasePathView,
) -> i32 {
    ffi_try!(
        unsafe { operation_update_bases_at_inner(operation, index, out_base) },
        neg
    )
}

unsafe fn operation_update_bases_at_inner(
    operation: *const LanceOperation,
    index: usize,
    out_base: *mut LanceBasePathView,
) -> Result<i32> {
    if out_base.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_base must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let new_bases = update_bases_operation_parts(operation)?;
    if operation.base_paths.len() != new_bases.len() {
        return Err(lance_core::Error::internal(
            "UpdateBases base path view storage is inconsistent with native metadata".to_string(),
        ));
    }
    if index >= new_bases.len() {
        return Err(lance_core::Error::invalid_input_source(
            format!(
                "update bases index {index} is out of range for {} base paths",
                new_bases.len()
            )
            .into(),
        ));
    }
    let view = operation.base_paths[index]
        .as_ref()
        .map_err(|error| lance_core::Error::internal(error.to_string()))?
        .view();
    unsafe {
        ptr::write(out_base, view);
    }
    Ok(0)
}

// ─── Native Overwrite, Merge, Clone and UpdateConfig operations ─────────────

/// Native parts of one Overwrite operation.
struct OverwriteOperationParts<'a> {
    fragments: &'a [Fragment],
    schema: &'a Schema,
    config_upsert_values: &'a Option<HashMap<String, String>>,
    initial_bases: &'a Option<Vec<BasePath>>,
}

fn overwrite_operation_parts(operation: &LanceOperation) -> Result<OverwriteOperationParts<'_>> {
    match &operation.inner {
        Operation::Overwrite {
            fragments,
            schema,
            config_upsert_values,
            initial_bases,
        } => Ok(OverwriteOperationParts {
            fragments,
            schema,
            config_upsert_values,
            initial_bases,
        }),
        other => Err(lance_core::Error::not_supported_source(
            format!(
                "Overwrite inspection is not supported for {} operations",
                other.name()
            )
            .into(),
        )),
    }
}

/// Native parts of one Merge operation.
struct MergeOperationParts<'a> {
    fragments: &'a [Fragment],
    schema: &'a Schema,
    preserves_nullability: bool,
}

fn merge_operation_parts(operation: &LanceOperation) -> Result<MergeOperationParts<'_>> {
    match &operation.inner {
        Operation::Merge {
            fragments,
            schema,
            preserves_nullability,
        } => Ok(MergeOperationParts {
            fragments,
            schema,
            preserves_nullability: *preserves_nullability,
        }),
        other => Err(lance_core::Error::not_supported_source(
            format!(
                "Merge inspection is not supported for {} operations",
                other.name()
            )
            .into(),
        )),
    }
}

/// Native parts of one Project operation.
fn project_operation_parts(operation: &LanceOperation) -> Result<(&Schema, bool)> {
    match &operation.inner {
        Operation::Project {
            schema,
            preserves_nullability,
        } => Ok((schema, *preserves_nullability)),
        other => Err(lance_core::Error::not_supported_source(
            format!(
                "Project inspection is not supported for {} operations",
                other.name()
            )
            .into(),
        )),
    }
}

/// Native parts of one Clone operation.
struct CloneOperationParts<'a> {
    is_shallow: bool,
    ref_name: &'a Option<String>,
    ref_version: u64,
    ref_path: &'a String,
    branch_name: &'a Option<String>,
}

fn clone_operation_parts(operation: &LanceOperation) -> Result<CloneOperationParts<'_>> {
    match &operation.inner {
        Operation::Clone {
            is_shallow,
            ref_name,
            ref_version,
            ref_path,
            branch_name,
        } => Ok(CloneOperationParts {
            is_shallow: *is_shallow,
            ref_name,
            ref_version: *ref_version,
            ref_path,
            branch_name,
        }),
        other => Err(lance_core::Error::not_supported_source(
            format!(
                "Clone inspection is not supported for {} operations",
                other.name()
            )
            .into(),
        )),
    }
}

/// Native parts of one UpdateConfig operation.
struct UpdateConfigOperationParts<'a> {
    config_updates: &'a Option<UpdateMap>,
    table_metadata_updates: &'a Option<UpdateMap>,
    schema_metadata_updates: &'a Option<UpdateMap>,
    field_metadata_updates: &'a HashMap<i32, UpdateMap>,
}

fn update_config_operation_parts(
    operation: &LanceOperation,
) -> Result<UpdateConfigOperationParts<'_>> {
    match &operation.inner {
        Operation::UpdateConfig {
            config_updates,
            table_metadata_updates,
            schema_metadata_updates,
            field_metadata_updates,
        } => Ok(UpdateConfigOperationParts {
            config_updates,
            table_metadata_updates,
            schema_metadata_updates,
            field_metadata_updates,
        }),
        other => Err(lance_core::Error::not_supported_source(
            format!(
                "UpdateConfig inspection is not supported for {} operations",
                other.name()
            )
            .into(),
        )),
    }
}

/// Construct an owned native Overwrite operation from a native schema and
/// fragment handles.
///
/// The schema is cloned, so the caller may close `schema` immediately. Every
/// fragment handle is borrowed and its complete native metadata is copied;
/// NULL arrays are accepted only with a zero count. `config_upsert_values ==
/// NULL` maps to native `None`; a non-NULL map with `count == 0` maps to
/// native `Some(empty map)`. `initial_bases == NULL` maps to native `None`; a
/// non-NULL list with `count == 0` maps to native `Some(empty list)`.
///
/// Native commit code assigns fresh fragment IDs to the copied fragments,
/// rejects fragments that carry deletion files, and replaces the manifest's
/// schema and config. This constructor adds no binding-side ID, deletion-file
/// or schema policy. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_overwrite_new(
    schema: *const LanceSchema,
    fragments: *const *const LanceFragment,
    fragment_count: usize,
    config_upsert_values: *const LanceStringMap,
    initial_bases: *const LanceBasePathList,
) -> *mut LanceOperation {
    ffi_try!(
        unsafe {
            operation_overwrite_new_inner(
                schema,
                fragments,
                fragment_count,
                config_upsert_values,
                initial_bases,
            )
        },
        null
    )
}

unsafe fn operation_overwrite_new_inner(
    schema: *const LanceSchema,
    fragments: *const *const LanceFragment,
    fragment_count: usize,
    config_upsert_values: *const LanceStringMap,
    initial_bases: *const LanceBasePathList,
) -> Result<*mut LanceOperation> {
    let schema = unsafe { schema_ref(schema) }?.inner.clone();
    let fragments = unsafe { clone_fragment_array(fragments, fragment_count, "fragments") }?;
    let config_upsert_values =
        unsafe { parse_string_map(config_upsert_values, "config_upsert_values") }?;
    let initial_bases = unsafe { parse_base_path_list(initial_bases) }?;
    Ok(Box::into_raw(Box::new(LanceOperation::from_native(
        Operation::Overwrite {
            fragments,
            schema,
            config_upsert_values,
            initial_bases,
        },
    ))))
}

/// Construct an owned native Merge operation from a native schema and fragment
/// handles.
///
/// The schema is cloned, so the caller may close `schema` immediately. Every
/// fragment handle is borrowed and its complete native metadata is copied;
/// NULL arrays are accepted only with a zero count. `preserves_nullability` is
/// stored unchanged as the caller's explicit assertion that the merge makes no
/// nullability-affecting schema change. Native commit code owns schema-change
/// validation and concurrent-append conflict handling. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_merge_new(
    schema: *const LanceSchema,
    fragments: *const *const LanceFragment,
    fragment_count: usize,
    preserves_nullability: bool,
) -> *mut LanceOperation {
    ffi_try!(
        unsafe {
            operation_merge_new_inner(schema, fragments, fragment_count, preserves_nullability)
        },
        null
    )
}

unsafe fn operation_merge_new_inner(
    schema: *const LanceSchema,
    fragments: *const *const LanceFragment,
    fragment_count: usize,
    preserves_nullability: bool,
) -> Result<*mut LanceOperation> {
    let schema = unsafe { schema_ref(schema) }?.inner.clone();
    let fragments = unsafe { clone_fragment_array(fragments, fragment_count, "fragments") }?;
    Ok(Box::into_raw(Box::new(LanceOperation::from_native(
        Operation::Merge {
            fragments,
            schema,
            preserves_nullability,
        },
    ))))
}

/// Construct an owned native Clone operation.
///
/// `ref_name == NULL` maps to native `None`; a non-NULL pointer, including an
/// empty string, maps to `Some(name)`. `ref_path` is required and may be empty.
/// `branch_name` follows the same NULL/`Some` mapping as `ref_name`. Native
/// commit code owns shallow-clone publication and deep-clone rejection; this
/// constructor adds no binding-side policy. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_clone_new(
    is_shallow: bool,
    ref_name: *const c_char,
    ref_version: u64,
    ref_path: *const c_char,
    branch_name: *const c_char,
) -> *mut LanceOperation {
    ffi_try!(
        unsafe {
            operation_clone_new_inner(is_shallow, ref_name, ref_version, ref_path, branch_name)
        },
        null
    )
}

unsafe fn operation_clone_new_inner(
    is_shallow: bool,
    ref_name: *const c_char,
    ref_version: u64,
    ref_path: *const c_char,
    branch_name: *const c_char,
) -> Result<*mut LanceOperation> {
    let ref_name = unsafe { helpers::parse_c_string(ref_name) }?.map(str::to_owned);
    let ref_path = required_c_string(ref_path, "ref_path")?.to_string();
    let branch_name = unsafe { helpers::parse_c_string(branch_name) }?.map(str::to_owned);
    Ok(Box::into_raw(Box::new(LanceOperation::from_native(
        Operation::Clone {
            is_shallow,
            ref_name,
            ref_version,
            ref_path,
            branch_name,
        },
    ))))
}

/// One field's metadata updates for `lance_operation_update_config_new`.
///
/// `updates` is borrowed and cloned into native `field_metadata_updates`.
/// Duplicate field IDs follow native `HashMap` last-write-wins semantics where
/// the last complete map replaces earlier maps.
#[repr(C)]
pub struct LanceFieldMetadataUpdateMap {
    pub field_id: i32,
    pub updates: *const LanceUpdateMap,
}

/// Borrowed UpdateConfig operation inputs.
///
/// Each `LanceUpdateMap` pointer is optional: NULL maps to native `None` and a
/// non-NULL handle is cloned, including an explicitly empty map. The field
/// metadata array may be NULL only with a zero count.
#[repr(C)]
pub struct LanceUpdateConfigOperationOptions {
    pub config_updates: *const LanceUpdateMap,
    pub table_metadata_updates: *const LanceUpdateMap,
    pub schema_metadata_updates: *const LanceUpdateMap,
    pub field_metadata_updates: *const LanceFieldMetadataUpdateMap,
    pub field_metadata_update_count: usize,
}

/// Construct an owned native UpdateConfig operation.
///
/// The three metadata maps and every per-field map are cloned, so all input
/// handles may be closed immediately. Native commit code applies replacement
/// and incremental semantics, including None-entry handling within each map.
/// Duplicate field IDs keep the last complete map. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_config_new(
    options: *const LanceUpdateConfigOperationOptions,
) -> *mut LanceOperation {
    ffi_try!(unsafe { operation_update_config_new_inner(options) }, null)
}

unsafe fn operation_update_config_new_inner(
    options: *const LanceUpdateConfigOperationOptions,
) -> Result<*mut LanceOperation> {
    if options.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "options must not be NULL".into(),
        ));
    }
    let options = unsafe { &*options };
    let config_updates = unsafe { parse_update_map(options.config_updates) }?;
    let table_metadata_updates = unsafe { parse_update_map(options.table_metadata_updates) }?;
    let schema_metadata_updates = unsafe { parse_update_map(options.schema_metadata_updates) }?;

    let field_count = options.field_metadata_update_count;
    if options.field_metadata_updates.is_null() && field_count > 0 {
        return Err(lance_core::Error::invalid_input_source(
            format!(
                "field_metadata_updates must not be NULL when field_metadata_update_count = {field_count}"
            )
            .into(),
        ));
    }
    if field_count > isize::MAX as usize / std::mem::size_of::<LanceFieldMetadataUpdateMap>() {
        return Err(lance_core::Error::invalid_input_source(
            format!(
                "field_metadata_update_count {field_count} exceeds the maximum addressable field metadata length"
            )
            .into(),
        ));
    }
    let mut field_metadata_updates = HashMap::with_capacity(field_count);
    if field_count > 0 {
        let entries =
            unsafe { std::slice::from_raw_parts(options.field_metadata_updates, field_count) };
        for (index, entry) in entries.iter().enumerate() {
            if entry.updates.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    format!(
                        "field_metadata_updates[{index}].updates must not be NULL when field_metadata_update_count = {field_count}"
                    )
                    .into(),
                ));
            }
            let updates = update_map_at(entry.updates)?.inner.clone();
            field_metadata_updates.insert(entry.field_id, updates);
        }
    }

    Ok(Box::into_raw(Box::new(LanceOperation::from_native(
        Operation::UpdateConfig {
            config_updates,
            table_metadata_updates,
            schema_metadata_updates,
            field_metadata_updates,
        },
    ))))
}

/// Read the number of fragments in an Overwrite operation.
///
/// Returns 0 on success, -1 on error. Other variants return
/// `LANCE_ERR_NOT_SUPPORTED`. The generic `lance_operation_fragment_count()`
/// keeps its Append/Overwrite behavior.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_overwrite_fragment_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_overwrite_fragment_count_inner(operation, out_count) },
        neg
    )
}

unsafe fn operation_overwrite_fragment_count_inner(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = overwrite_operation_parts(operation)?;
    unsafe {
        ptr::write(out_count, parts.fragments.len() as u64);
    }
    Ok(0)
}

/// Clone one Overwrite fragment as an independent detached handle.
///
/// The clone owns the complete native `Fragment` metadata and remains valid
/// after the operation and every source dataset are closed. It supports
/// metadata inspection and operation construction but not dataset-dependent
/// reads. Missing indices are invalid arguments. Returns 0 on success, -1 on
/// error, and leaves `out_fragment` unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_overwrite_fragment_at(
    operation: *const LanceOperation,
    index: usize,
    out_fragment: *mut *mut LanceFragment,
) -> i32 {
    ffi_try!(
        unsafe { operation_overwrite_fragment_at_inner(operation, index, out_fragment) },
        neg
    )
}

unsafe fn operation_overwrite_fragment_at_inner(
    operation: *const LanceOperation,
    index: usize,
    out_fragment: *mut *mut LanceFragment,
) -> Result<i32> {
    if out_fragment.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_fragment must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = overwrite_operation_parts(operation)?;
    let fragment = parts.fragments.get(index).ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!(
                "overwrite fragment index {index} is out of range for {} fragments",
                parts.fragments.len()
            )
            .into(),
        )
    })?;
    let fragment = Box::into_raw(Box::new(LanceFragment::from_metadata(fragment.clone())));
    unsafe {
        ptr::write(out_fragment, fragment);
    }
    Ok(0)
}

/// Clone the in-memory schema of an Overwrite operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_overwrite_schema_clone(
    operation: *const LanceOperation,
    out_schema: *mut *mut LanceSchema,
) -> i32 {
    ffi_try!(
        unsafe { operation_overwrite_schema_clone_inner(operation, out_schema) },
        neg
    )
}

unsafe fn operation_overwrite_schema_clone_inner(
    operation: *const LanceOperation,
    out_schema: *mut *mut LanceSchema,
) -> Result<i32> {
    if out_schema.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_schema must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = overwrite_operation_parts(operation)?;
    let schema = Box::new(LanceSchema {
        inner: parts.schema.clone(),
    });
    unsafe {
        ptr::write(out_schema, Box::into_raw(schema));
    }
    Ok(0)
}

/// Report whether an Overwrite operation carries native `Some` config upserts.
///
/// `Some(empty)` is distinct from `None`. Returns 0 on success, -1 on error;
/// the output is written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_overwrite_has_config_upsert_values(
    operation: *const LanceOperation,
    out_has_values: *mut bool,
) -> i32 {
    ffi_try!(
        unsafe { operation_overwrite_has_config_upsert_values_inner(operation, out_has_values) },
        neg
    )
}

unsafe fn operation_overwrite_has_config_upsert_values_inner(
    operation: *const LanceOperation,
    out_has_values: *mut bool,
) -> Result<i32> {
    if out_has_values.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_has_values must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = overwrite_operation_parts(operation)?;
    unsafe {
        ptr::write(out_has_values, parts.config_upsert_values.is_some());
    }
    Ok(0)
}

/// Read the number of config upsert entries in an Overwrite operation.
///
/// Native `None` reports 0. Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_overwrite_config_upsert_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_overwrite_config_upsert_count_inner(operation, out_count) },
        neg
    )
}

unsafe fn operation_overwrite_config_upsert_count_inner(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = overwrite_operation_parts(operation)?;
    let count = parts.config_upsert_values.as_ref().map_or(0, HashMap::len);
    unsafe {
        ptr::write(out_count, count as u64);
    }
    Ok(0)
}

fn overwrite_config_entry(operation: &LanceOperation, index: usize) -> Result<&(CString, CString)> {
    let parts = overwrite_operation_parts(operation)?;
    let count = parts.config_upsert_values.as_ref().map_or(0, HashMap::len);
    if operation.config_upserts.len() != count {
        return Err(lance_core::Error::internal(
            "Overwrite config view storage is inconsistent with native metadata".to_string(),
        ));
    }
    let entry = operation.config_upserts.get(index).ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!("Overwrite config index {index} is out of range for {count} entries").into(),
        )
    })?;
    entry
        .as_ref()
        .map_err(|error| lance_core::Error::internal(error.to_string()))
}

/// Read one borrowed config upsert key from an Overwrite operation.
///
/// The pointer remains valid until the operation handle is closed. Returns
/// NULL on error. Keys are native UTF-8 strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_overwrite_config_upsert_key_at(
    operation: *const LanceOperation,
    index: usize,
) -> *const c_char {
    ffi_try!(
        unsafe { operation_overwrite_config_upsert_key_at_inner(operation, index) },
        ptr::null()
    )
}

unsafe fn operation_overwrite_config_upsert_key_at_inner(
    operation: *const LanceOperation,
    index: usize,
) -> Result<*const c_char> {
    let operation = operation_at(operation)?;
    Ok(overwrite_config_entry(operation, index)?.0.as_ptr())
}

/// Read one borrowed config upsert value from an Overwrite operation.
///
/// The pointer remains valid until the operation handle is closed. Returns
/// NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_overwrite_config_upsert_value_at(
    operation: *const LanceOperation,
    index: usize,
) -> *const c_char {
    ffi_try!(
        unsafe { operation_overwrite_config_upsert_value_at_inner(operation, index) },
        ptr::null()
    )
}

unsafe fn operation_overwrite_config_upsert_value_at_inner(
    operation: *const LanceOperation,
    index: usize,
) -> Result<*const c_char> {
    let operation = operation_at(operation)?;
    Ok(overwrite_config_entry(operation, index)?.1.as_ptr())
}

/// Report whether an Overwrite operation carries native `Some` initial bases.
///
/// `Some(empty)` is distinct from `None`. Returns 0 on success, -1 on error;
/// the output is written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_overwrite_has_initial_bases(
    operation: *const LanceOperation,
    out_has_bases: *mut bool,
) -> i32 {
    ffi_try!(
        unsafe { operation_overwrite_has_initial_bases_inner(operation, out_has_bases) },
        neg
    )
}

unsafe fn operation_overwrite_has_initial_bases_inner(
    operation: *const LanceOperation,
    out_has_bases: *mut bool,
) -> Result<i32> {
    if out_has_bases.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_has_bases must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = overwrite_operation_parts(operation)?;
    unsafe {
        ptr::write(out_has_bases, parts.initial_bases.is_some());
    }
    Ok(0)
}

/// Read the number of initial base paths in an Overwrite operation.
///
/// Native `None` reports 0. Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_overwrite_initial_base_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_overwrite_initial_base_count_inner(operation, out_count) },
        neg
    )
}

unsafe fn operation_overwrite_initial_base_count_inner(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = overwrite_operation_parts(operation)?;
    let count = parts.initial_bases.as_ref().map_or(0, Vec::len);
    unsafe {
        ptr::write(out_count, count as u64);
    }
    Ok(0)
}

/// Read one borrowed initial base path from an Overwrite operation.
///
/// `path` and `name` point into the operation handle and remain valid until it
/// is closed. `name == NULL` represents native `None`; an empty string is an
/// explicit empty name. Returns 0 on success, -1 on error, and leaves
/// `out_base` unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_overwrite_initial_base_at(
    operation: *const LanceOperation,
    index: usize,
    out_base: *mut LanceBasePathView,
) -> i32 {
    ffi_try!(
        unsafe { operation_overwrite_initial_base_at_inner(operation, index, out_base) },
        neg
    )
}

unsafe fn operation_overwrite_initial_base_at_inner(
    operation: *const LanceOperation,
    index: usize,
    out_base: *mut LanceBasePathView,
) -> Result<i32> {
    if out_base.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_base must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = overwrite_operation_parts(operation)?;
    let count = parts.initial_bases.as_ref().map_or(0, Vec::len);
    if operation.base_paths.len() != count {
        return Err(lance_core::Error::internal(
            "Overwrite initial base view storage is inconsistent with native metadata".to_string(),
        ));
    }
    if index >= count {
        return Err(lance_core::Error::invalid_input_source(
            format!("overwrite initial base index {index} is out of range for {count} base paths")
                .into(),
        ));
    }
    let view = operation.base_paths[index]
        .as_ref()
        .map_err(|error| lance_core::Error::internal(error.to_string()))?
        .view();
    unsafe {
        ptr::write(out_base, view);
    }
    Ok(0)
}

/// Read the number of fragments in a Merge operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_merge_fragment_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_merge_fragment_count_inner(operation, out_count) },
        neg
    )
}

unsafe fn operation_merge_fragment_count_inner(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = merge_operation_parts(operation)?;
    unsafe {
        ptr::write(out_count, parts.fragments.len() as u64);
    }
    Ok(0)
}

/// Clone one Merge fragment as an independent detached handle.
///
/// The clone owns the complete native `Fragment` metadata. Missing indices are
/// invalid arguments. Returns 0 on success, -1 on error, and leaves
/// `out_fragment` unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_merge_fragment_at(
    operation: *const LanceOperation,
    index: usize,
    out_fragment: *mut *mut LanceFragment,
) -> i32 {
    ffi_try!(
        unsafe { operation_merge_fragment_at_inner(operation, index, out_fragment) },
        neg
    )
}

unsafe fn operation_merge_fragment_at_inner(
    operation: *const LanceOperation,
    index: usize,
    out_fragment: *mut *mut LanceFragment,
) -> Result<i32> {
    if out_fragment.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_fragment must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = merge_operation_parts(operation)?;
    let fragment = parts.fragments.get(index).ok_or_else(|| {
        lance_core::Error::invalid_input_source(
            format!(
                "merge fragment index {index} is out of range for {} fragments",
                parts.fragments.len()
            )
            .into(),
        )
    })?;
    let fragment = Box::into_raw(Box::new(LanceFragment::from_metadata(fragment.clone())));
    unsafe {
        ptr::write(out_fragment, fragment);
    }
    Ok(0)
}

/// Clone the in-memory schema of a Merge operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_merge_schema_clone(
    operation: *const LanceOperation,
    out_schema: *mut *mut LanceSchema,
) -> i32 {
    ffi_try!(
        unsafe { operation_merge_schema_clone_inner(operation, out_schema) },
        neg
    )
}

unsafe fn operation_merge_schema_clone_inner(
    operation: *const LanceOperation,
    out_schema: *mut *mut LanceSchema,
) -> Result<i32> {
    if out_schema.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_schema must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = merge_operation_parts(operation)?;
    let schema = Box::new(LanceSchema {
        inner: parts.schema.clone(),
    });
    unsafe {
        ptr::write(out_schema, Box::into_raw(schema));
    }
    Ok(0)
}

/// Read the native `preserves_nullability` assertion from a Merge operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_merge_preserves_nullability(
    operation: *const LanceOperation,
    out_preserves_nullability: *mut bool,
) -> i32 {
    ffi_try!(
        unsafe {
            operation_merge_preserves_nullability_inner(operation, out_preserves_nullability)
        },
        neg
    )
}

unsafe fn operation_merge_preserves_nullability_inner(
    operation: *const LanceOperation,
    out_preserves_nullability: *mut bool,
) -> Result<i32> {
    if out_preserves_nullability.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_preserves_nullability must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = merge_operation_parts(operation)?;
    unsafe {
        ptr::write(out_preserves_nullability, parts.preserves_nullability);
    }
    Ok(0)
}

/// Clone the in-memory schema of a Project operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_project_schema_clone(
    operation: *const LanceOperation,
    out_schema: *mut *mut LanceSchema,
) -> i32 {
    ffi_try!(
        unsafe { operation_project_schema_clone_inner(operation, out_schema) },
        neg
    )
}

unsafe fn operation_project_schema_clone_inner(
    operation: *const LanceOperation,
    out_schema: *mut *mut LanceSchema,
) -> Result<i32> {
    if out_schema.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_schema must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let (schema, _) = project_operation_parts(operation)?;
    let schema = Box::new(LanceSchema {
        inner: schema.clone(),
    });
    unsafe {
        ptr::write(out_schema, Box::into_raw(schema));
    }
    Ok(0)
}

/// Read the native `preserves_nullability` assertion from a Project operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_project_preserves_nullability(
    operation: *const LanceOperation,
    out_preserves_nullability: *mut bool,
) -> i32 {
    ffi_try!(
        unsafe {
            operation_project_preserves_nullability_inner(operation, out_preserves_nullability)
        },
        neg
    )
}

unsafe fn operation_project_preserves_nullability_inner(
    operation: *const LanceOperation,
    out_preserves_nullability: *mut bool,
) -> Result<i32> {
    if out_preserves_nullability.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_preserves_nullability must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let (_, preserves_nullability) = project_operation_parts(operation)?;
    unsafe {
        ptr::write(out_preserves_nullability, preserves_nullability);
    }
    Ok(0)
}

/// Read the shallow-clone flag of a Clone operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_clone_is_shallow(
    operation: *const LanceOperation,
    out_is_shallow: *mut bool,
) -> i32 {
    ffi_try!(
        unsafe { operation_clone_is_shallow_inner(operation, out_is_shallow) },
        neg
    )
}

unsafe fn operation_clone_is_shallow_inner(
    operation: *const LanceOperation,
    out_is_shallow: *mut bool,
) -> Result<i32> {
    if out_is_shallow.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_is_shallow must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = clone_operation_parts(operation)?;
    unsafe {
        ptr::write(out_is_shallow, parts.is_shallow);
    }
    Ok(0)
}

/// Read the optional native reference name of a Clone operation.
///
/// Both output pointers are required. On success `*out_present` reports
/// native `Some`/`None`; `*out_value` receives a borrowed pointer into the
/// operation when present and NULL otherwise. Returns 0 on success, -1 on
/// error, and leaves both outputs unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_clone_ref_name(
    operation: *const LanceOperation,
    out_present: *mut bool,
    out_value: *mut *const c_char,
) -> i32 {
    ffi_try!(
        unsafe { operation_clone_ref_name_inner(operation, out_present, out_value) },
        neg
    )
}

unsafe fn operation_clone_ref_name_inner(
    operation: *const LanceOperation,
    out_present: *mut bool,
    out_value: *mut *const c_char,
) -> Result<i32> {
    if out_present.is_null() || out_value.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_present and out_value must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = clone_operation_parts(operation)?;
    if parts.ref_name.is_some() != operation.clone_strings.ref_name.is_some() {
        return Err(lance_core::Error::internal(
            "Clone ref_name view storage is inconsistent with native metadata".to_string(),
        ));
    }
    let (present, value) = match &operation.clone_strings.ref_name {
        Some(Ok(value)) => (true, value.as_ptr()),
        Some(Err(error)) => {
            return Err(lance_core::Error::internal(error.to_string()));
        }
        None => (false, ptr::null()),
    };
    unsafe {
        ptr::write(out_present, present);
        ptr::write(out_value, value);
    }
    Ok(0)
}

/// Read the native reference version of a Clone operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_clone_ref_version(
    operation: *const LanceOperation,
    out_version: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_clone_ref_version_inner(operation, out_version) },
        neg
    )
}

unsafe fn operation_clone_ref_version_inner(
    operation: *const LanceOperation,
    out_version: *mut u64,
) -> Result<i32> {
    if out_version.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_version must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = clone_operation_parts(operation)?;
    unsafe {
        ptr::write(out_version, parts.ref_version);
    }
    Ok(0)
}

/// Read the borrowed native reference path of a Clone operation.
///
/// The pointer remains valid until the operation handle is closed. Returns
/// NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_clone_ref_path(
    operation: *const LanceOperation,
) -> *const c_char {
    ffi_try!(
        unsafe { operation_clone_ref_path_inner(operation) },
        ptr::null()
    )
}

unsafe fn operation_clone_ref_path_inner(
    operation: *const LanceOperation,
) -> Result<*const c_char> {
    let operation = operation_at(operation)?;
    let parts = clone_operation_parts(operation)?;
    if !parts.ref_path.is_empty() && operation.clone_strings.ref_path.is_none() {
        return Err(lance_core::Error::internal(
            "Clone ref_path view storage is inconsistent with native metadata".to_string(),
        ));
    }
    match &operation.clone_strings.ref_path {
        Some(Ok(value)) => Ok(value.as_ptr()),
        Some(Err(error)) => Err(lance_core::Error::internal(error.to_string())),
        None => Err(lance_core::Error::internal(
            "Clone ref_path view storage is missing".to_string(),
        )),
    }
}

/// Read the optional native branch name of a Clone operation.
///
/// Both output pointers are required. On success `*out_present` reports
/// native `Some`/`None`; `*out_value` receives a borrowed pointer into the
/// operation when present and NULL otherwise. Returns 0 on success, -1 on
/// error, and leaves both outputs unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_clone_branch_name(
    operation: *const LanceOperation,
    out_present: *mut bool,
    out_value: *mut *const c_char,
) -> i32 {
    ffi_try!(
        unsafe { operation_clone_branch_name_inner(operation, out_present, out_value) },
        neg
    )
}

unsafe fn operation_clone_branch_name_inner(
    operation: *const LanceOperation,
    out_present: *mut bool,
    out_value: *mut *const c_char,
) -> Result<i32> {
    if out_present.is_null() || out_value.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_present and out_value must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = clone_operation_parts(operation)?;
    if parts.branch_name.is_some() != operation.clone_strings.branch_name.is_some() {
        return Err(lance_core::Error::internal(
            "Clone branch_name view storage is inconsistent with native metadata".to_string(),
        ));
    }
    let (present, value) = match &operation.clone_strings.branch_name {
        Some(Ok(value)) => (true, value.as_ptr()),
        Some(Err(error)) => {
            return Err(lance_core::Error::internal(error.to_string()));
        }
        None => (false, ptr::null()),
    };
    unsafe {
        ptr::write(out_present, present);
        ptr::write(out_value, value);
    }
    Ok(0)
}

enum UpdateConfigMapField {
    Config,
    TableMetadata,
    SchemaMetadata,
}

unsafe fn update_config_map_getter_inner(
    operation: *const LanceOperation,
    out_map: *mut *mut LanceUpdateMap,
    field: UpdateConfigMapField,
) -> Result<i32> {
    if out_map.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_map must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = update_config_operation_parts(operation)?;
    let updates = match field {
        UpdateConfigMapField::Config => parts.config_updates,
        UpdateConfigMapField::TableMetadata => parts.table_metadata_updates,
        UpdateConfigMapField::SchemaMetadata => parts.schema_metadata_updates,
    };
    let map = updates
        .as_ref()
        .map(|updates| Box::into_raw(Box::new(LanceUpdateMap::from_native(updates.clone()))));
    unsafe {
        ptr::write(out_map, map.unwrap_or(ptr::null_mut()));
    }
    Ok(0)
}

/// Clone the optional native config updates map from an UpdateConfig operation.
///
/// On success `*out_map` receives either an independently owned map handle or
/// NULL for native `None`. An owned handle may wrap an explicitly empty map.
/// Returns 0 on success, -1 on error, and leaves the output unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_config_config_updates(
    operation: *const LanceOperation,
    out_map: *mut *mut LanceUpdateMap,
) -> i32 {
    ffi_try!(
        unsafe { update_config_map_getter_inner(operation, out_map, UpdateConfigMapField::Config) },
        neg
    )
}

/// Clone the optional native table metadata updates map from an UpdateConfig
/// operation. `NULL` represents native `None`. Returns 0 on success, -1 on
/// error, and leaves the output unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_config_table_metadata_updates(
    operation: *const LanceOperation,
    out_map: *mut *mut LanceUpdateMap,
) -> i32 {
    ffi_try!(
        unsafe {
            update_config_map_getter_inner(operation, out_map, UpdateConfigMapField::TableMetadata)
        },
        neg
    )
}

/// Clone the optional native schema metadata updates map from an UpdateConfig
/// operation. `NULL` represents native `None`. Returns 0 on success, -1 on
/// error, and leaves the output unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_config_schema_metadata_updates(
    operation: *const LanceOperation,
    out_map: *mut *mut LanceUpdateMap,
) -> i32 {
    ffi_try!(
        unsafe {
            update_config_map_getter_inner(operation, out_map, UpdateConfigMapField::SchemaMetadata)
        },
        neg
    )
}

/// Read the number of per-field metadata update maps.
///
/// The count follows native `HashMap` contents; iteration order is unspecified
/// but stable for one operation handle. Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_config_field_metadata_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { operation_update_config_field_metadata_count_inner(operation, out_count) },
        neg
    )
}

unsafe fn operation_update_config_field_metadata_count_inner(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_count must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = update_config_operation_parts(operation)?;
    unsafe {
        ptr::write(out_count, parts.field_metadata_updates.len() as u64);
    }
    Ok(0)
}

/// Clone one per-field metadata update map.
///
/// On success both outputs are written: the native field ID and an
/// independently owned map handle. Native `HashMap` iteration order is
/// unspecified but stable for one operation handle. Returns 0 on success, -1 on
/// error, and leaves both outputs unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_config_field_metadata_at(
    operation: *const LanceOperation,
    index: usize,
    out_field_id: *mut i32,
    out_map: *mut *mut LanceUpdateMap,
) -> i32 {
    ffi_try!(
        unsafe {
            operation_update_config_field_metadata_at_inner(operation, index, out_field_id, out_map)
        },
        neg
    )
}

unsafe fn operation_update_config_field_metadata_at_inner(
    operation: *const LanceOperation,
    index: usize,
    out_field_id: *mut i32,
    out_map: *mut *mut LanceUpdateMap,
) -> Result<i32> {
    if out_field_id.is_null() || out_map.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_field_id and out_map must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let parts = update_config_operation_parts(operation)?;
    let count = parts.field_metadata_updates.len();
    let (field_id, updates) = parts
        .field_metadata_updates
        .iter()
        .nth(index)
        .ok_or_else(|| {
            lance_core::Error::invalid_input_source(
                format!(
                    "field metadata update index {index} is out of range for {count} field IDs"
                )
                .into(),
            )
        })?;
    let map = Box::into_raw(Box::new(LanceUpdateMap::from_native(updates.clone())));
    unsafe {
        ptr::write(out_field_id, *field_id);
        ptr::write(out_map, map);
    }
    Ok(0)
}

/// Export an Overwrite operation's schema as Arrow C Data Interface.
///
/// The caller provides the output `ArrowSchema` and owns it. It must release
/// it exactly once. Returns 0 on success, -1 on error. Append operations have
/// no schema; other variants return LANCE_ERR_NOT_SUPPORTED.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_schema(
    operation: *const LanceOperation,
    out: *mut FFI_ArrowSchema,
) -> i32 {
    ffi_try!(unsafe { operation_schema_inner(operation, out) }, neg)
}

unsafe fn operation_schema_inner(
    operation: *const LanceOperation,
    out: *mut FFI_ArrowSchema,
) -> Result<i32> {
    if out.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let schema = match &operation.inner {
        Operation::Overwrite { schema, .. } => schema,
        other => {
            return Err(lance_core::Error::not_supported_source(
                format!(
                    "schema inspection is not supported for {} operations",
                    other.name()
                )
                .into(),
            ));
        }
    };
    let arrow_schema: ArrowSchema = schema.into();
    let ffi_schema = FFI_ArrowSchema::try_from(&arrow_schema)?;
    unsafe {
        ptr::write_unaligned(out, ffi_schema);
    }
    Ok(0)
}

/// Serialize a transaction with the native protobuf conversion.
///
/// On success, `*out_bytes` receives a newly allocated native
/// `pb.Transaction` and `*out_len` its byte length. Free the buffer with
/// `lance_free_bytes()`. On error both output slots are unchanged.
///
/// The native conversion is the transport contract. `Operation::Unknown`
/// cannot be re-encoded and is rejected. The selected core's conversion also
/// normalizes empty tags/property maps to absent and does not carry Overwrite
/// schema metadata through its separate protobuf field. Callers should treat
/// the round trip as native `TryFrom` semantics, not lossless JSON-like state.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_transaction_to_proto(
    transaction: *const LanceTransaction,
    out_bytes: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    ffi_try!(
        unsafe { transaction_to_proto_inner(transaction, out_bytes, out_len) },
        neg
    )
}

unsafe fn transaction_to_proto_inner(
    transaction: *const LanceTransaction,
    out_bytes: *mut *mut u8,
    out_len: *mut usize,
) -> Result<i32> {
    if out_bytes.is_null() || out_len.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_bytes and out_len must not be NULL".into(),
        ));
    }
    let transaction = transaction_at(transaction)?;
    let message = pb::Transaction::try_from(&transaction.inner)?;
    let bytes = message.encode_to_vec();
    if bytes.is_empty() {
        return Err(lance_core::Error::internal(
            "native transaction protobuf encoding produced no bytes".to_string(),
        ));
    }

    let allocation = unsafe { libc::malloc(bytes.len()).cast::<u8>() };
    if allocation.is_null() {
        return Err(lance_core::Error::internal(format!(
            "failed to allocate {} bytes for transaction protobuf",
            bytes.len()
        )));
    }
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), allocation, bytes.len());
        ptr::write(out_bytes, allocation);
        ptr::write(out_len, bytes.len());
    }
    Ok(0)
}

/// Parse a transaction with the native protobuf conversion.
///
/// `bytes` must contain one complete protobuf `Transaction` message and must
/// not be NULL when `len` is non-zero. Empty input decodes to the native default
/// transaction with an Unknown operation. The returned handle owns native state
/// and must be closed with `lance_transaction_close()`. Returns NULL on error.
///
/// The native decoder drops unknown protobuf oneof variants, which become
/// `Operation::Unknown`. That value can be inspected but cannot be serialized
/// again or committed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_transaction_from_proto(
    bytes: *const u8,
    len: usize,
) -> *mut LanceTransaction {
    ffi_try!(unsafe { transaction_from_proto_inner(bytes, len) }, null)
}

unsafe fn transaction_from_proto_inner(
    bytes: *const u8,
    len: usize,
) -> Result<*mut LanceTransaction> {
    if len > isize::MAX as usize {
        return Err(lance_core::Error::invalid_input_source(
            format!(
                "len={len} exceeds the maximum Rust slice length {}",
                isize::MAX
            )
            .into(),
        ));
    }
    if bytes.is_null() && len > 0 {
        return Err(lance_core::Error::invalid_input_source(
            format!("bytes must not be NULL when len = {len}").into(),
        ));
    }
    let bytes = if len == 0 {
        &[][..]
    } else {
        // SAFETY: the caller provides `len` readable bytes, and the checks
        // above establish non-NULL storage and a representable slice length.
        unsafe { std::slice::from_raw_parts(bytes, len) }
    };
    let message = pb::Transaction::decode(bytes).map_err(|error| {
        lance_core::Error::invalid_input_source(
            format!("invalid transaction protobuf: {error}").into(),
        )
    })?;
    let transaction = Transaction::try_from(message)?;
    Ok(Box::into_raw(Box::new(LanceTransaction {
        inner: transaction,
    })))
}

/// Read the transaction for the dataset snapshot's current version.
///
/// Returns 0 on success. On success `*out_transaction` receives either an
/// independently owned transaction handle or NULL when the manifest has no
/// transaction file. On error the output slot is unchanged.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_read_transaction(
    dataset: *const LanceDataset,
    out_transaction: *mut *mut LanceTransaction,
) -> i32 {
    ffi_try!(
        unsafe { dataset_read_transaction_inner(dataset, out_transaction, None) },
        neg
    )
}

/// Read the transaction for a specific dataset version.
///
/// `version` is resolved against the dataset handle's branch/location rules.
/// Returns 0 on success. On success `*out_transaction` receives an
/// independently owned handle or NULL when that version has no transaction
/// file. Missing versions and invalid version IDs follow native errors, and
/// the output slot is unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_read_transaction_by_version(
    dataset: *const LanceDataset,
    version: u64,
    out_transaction: *mut *mut LanceTransaction,
) -> i32 {
    ffi_try!(
        unsafe { dataset_read_transaction_inner(dataset, out_transaction, Some(version)) },
        neg
    )
}

unsafe fn dataset_read_transaction_inner(
    dataset: *const LanceDataset,
    out_transaction: *mut *mut LanceTransaction,
    version: Option<u64>,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    if out_transaction.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_transaction must not be NULL".into(),
        ));
    }

    let snapshot = unsafe { &*dataset }.snapshot();
    let transaction = match version {
        Some(version) => block_on(snapshot.read_transaction_by_version(version))?,
        None => block_on(snapshot.read_transaction())?,
    };

    let transaction = match transaction {
        Some(transaction) => Box::into_raw(Box::new(LanceTransaction { inner: transaction })),
        None => ptr::null_mut(),
    };
    unsafe {
        ptr::write(out_transaction, transaction);
    }
    Ok(0)
}

// ─── Update, Rewrite and UpdateMemWalState operation values ──────────────────

/// Native update mode for an Update operation. Values are pinned for the C ABI.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum LanceUpdateMode {
    /// Rewrite whole rows (vertical update).
    RewriteRows = 0,
    /// Rewrite selected columns and record matched offsets (horizontal update).
    RewriteColumns = 1,
}

/// Borrowed row offsets for one fragment in a `LanceUpdatedFragmentOffsets`.
///
/// `offsets` may be NULL only when `offset_count` is 0, which is a real empty
/// bitmap entry rather than an absent one.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceFragmentOffsetsView {
    pub fragment_id: u64,
    pub offsets: *const u32,
    pub offset_count: usize,
}

/// Borrowed index-file entry for a `LanceRewrittenIndex`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceIndexFile {
    pub path: *const c_char,
    pub size_bytes: u64,
}

/// Kind of a native key-existence filter. Values are pinned for the C ABI.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum LanceKeyExistenceFilterKind {
    /// An explicit set of key hashes.
    Exact = 0,
    /// A bloom filter over key hashes.
    Bloom = 1,
}

/// Owned native key-existence filter value.
///
/// The constructor that accepts protobuf bytes converts through native
/// `TryFrom`, so absent bloom fields pick up the native defaults
/// (`number_of_items = 8192`, `probability = 0.00057`) and an absent filter
/// body becomes an empty exact set. `lance_key_existence_filter_to_proto`
/// converts through native `From`, which is the encoding an Update operation
/// persists.
pub struct LanceKeyExistenceFilter {
    pub(crate) inner: KeyExistenceFilter,
}

/// Owned native `UpdatedFragmentOffsets` value.
///
/// Native representation is `HashMap<fragment_id, RoaringBitmap>`. The ordered
/// array constructor therefore keeps the last entry for a repeated fragment ID,
/// and each entry's offsets are a set: repeated offsets collapse.
pub struct LanceUpdatedFragmentOffsets {
    pub(crate) inner: UpdatedFragmentOffsets,
}

/// Owned native `RewriteGroup` value.
pub struct LanceRewriteGroup {
    pub(crate) inner: RewriteGroup,
}

/// Owned native `RewrittenIndex` value.
pub struct LanceRewrittenIndex {
    pub(crate) inner: RewrittenIndex,
}

/// Complete input for `lance_operation_update_new`.
///
/// Every array is borrowed for the duration of the call and copied into native
/// owned values. A NULL array is accepted only with a zero count. `NULL`
/// `inserted_rows_filter` and `updated_fragment_offsets` map to native `None`;
/// `has_update_mode == false` maps to native `None` as well. This struct is
/// extended only by adding fields at the end.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceUpdateOperationOptions {
    pub removed_fragment_ids: *const u64,
    pub removed_fragment_count: usize,
    pub updated_fragments: *const *const LanceFragment,
    pub updated_fragment_count: usize,
    pub new_fragments: *const *const LanceFragment,
    pub new_fragment_count: usize,
    pub fields_modified: *const u32,
    pub fields_modified_count: usize,
    pub compacted_sstables: *const LanceCompactedSsTable,
    pub compacted_sstable_count: usize,
    pub fields_for_preserving_frag_bitmap: *const u32,
    pub fields_for_preserving_frag_bitmap_count: usize,
    /// When false, native `update_mode` is `None` and `update_mode` is ignored.
    pub has_update_mode: bool,
    /// One of `LanceUpdateMode`; required when `has_update_mode` is true.
    pub update_mode: i32,
    /// Optional native `inserted_rows_filter`; NULL means `None`.
    pub inserted_rows_filter: *const LanceKeyExistenceFilter,
    /// Optional native `updated_fragment_offsets`; NULL means `None`.
    pub updated_fragment_offsets: *const LanceUpdatedFragmentOffsets,
}

fn parse_update_mode(value: i32) -> Result<UpdateMode> {
    match value {
        0 => Ok(UpdateMode::RewriteRows),
        1 => Ok(UpdateMode::RewriteColumns),
        other => Err(lance_core::Error::invalid_input_source(
            format!(
                "update_mode must be LANCE_UPDATE_MODE_REWRITE_ROWS (0) or \
                 LANCE_UPDATE_MODE_REWRITE_COLUMNS (1) when has_update_mode is true, got {other}"
            )
            .into(),
        )),
    }
}

fn update_mode_code(mode: &UpdateMode) -> i32 {
    match mode {
        UpdateMode::RewriteRows => 0,
        UpdateMode::RewriteColumns => 1,
    }
}

fn update_kind_error(operation: &Operation) -> lance_core::Error {
    lance_core::Error::not_supported_source(
        format!(
            "Update inspection is not supported for {} operations",
            operation.name()
        )
        .into(),
    )
}

fn rewrite_kind_error(operation: &Operation) -> lance_core::Error {
    lance_core::Error::not_supported_source(
        format!(
            "Rewrite inspection is not supported for {} operations",
            operation.name()
        )
        .into(),
    )
}

fn update_mem_wal_state_kind_error(operation: &Operation) -> lance_core::Error {
    lance_core::Error::not_supported_source(
        format!(
            "UpdateMemWalState inspection is not supported for {} operations",
            operation.name()
        )
        .into(),
    )
}

fn count_out(len: usize, out_count: *mut u64, label: &str) -> Result<i32> {
    if out_count.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            format!("{label} must not be NULL").into(),
        ));
    }
    unsafe {
        ptr::write(out_count, u64::try_from(len).unwrap_or(u64::MAX));
    }
    Ok(0)
}

unsafe fn fragment_out(fragment: &Fragment, out_fragment: *mut *mut LanceFragment) -> Result<i32> {
    if out_fragment.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_fragment must not be NULL".into(),
        ));
    }
    let handle = Box::into_raw(Box::new(LanceFragment::from_metadata(fragment.clone())));
    unsafe {
        ptr::write(out_fragment, handle);
    }
    Ok(0)
}

// ─── UpdatedFragmentOffsets ─────────────────────────────────────────────────

/// Construct owned native `UpdatedFragmentOffsets` from ordered entries.
///
/// Entries are copied: each `offsets` array is read as a set of row offsets, and
/// a repeated `fragment_id` replaces the earlier entry exactly as the native
/// `HashMap` collection does. `Some(empty)` and an empty bitmap entry are both
/// representable here; only an explicit protobuf round trip normalizes them to
/// native `None`. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_updated_fragment_offsets_new(
    entries: *const LanceFragmentOffsetsView,
    entry_count: usize,
) -> *mut LanceUpdatedFragmentOffsets {
    ffi_try!(
        (|| -> Result<*mut LanceUpdatedFragmentOffsets> {
            let entries =
                unsafe { helpers::required_slice(entries, entry_count, "entries", "entry_count")? };
            let mut offsets = HashMap::with_capacity(entries.len());
            for (index, entry) in entries.iter().enumerate() {
                let values = unsafe {
                    helpers::required_slice(
                        entry.offsets,
                        entry.offset_count,
                        &format!("entries[{index}].offsets"),
                        "offset_count",
                    )?
                };
                offsets.insert(
                    entry.fragment_id,
                    RoaringBitmap::from_iter(values.iter().copied()),
                );
            }
            Ok(Box::into_raw(Box::new(LanceUpdatedFragmentOffsets {
                inner: UpdatedFragmentOffsets(offsets),
            })))
        })(),
        null
    )
}

/// Clone an owned native `UpdatedFragmentOffsets` value. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_updated_fragment_offsets_clone(
    offsets: *const LanceUpdatedFragmentOffsets,
) -> *mut LanceUpdatedFragmentOffsets {
    ffi_try!(
        (|| -> Result<*mut LanceUpdatedFragmentOffsets> {
            if offsets.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "offsets must not be NULL".into(),
                ));
            }
            let offsets = unsafe { &*offsets };
            Ok(Box::into_raw(Box::new(LanceUpdatedFragmentOffsets {
                inner: offsets.inner.clone(),
            })))
        })(),
        null
    )
}

/// Free an owned `UpdatedFragmentOffsets`. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_updated_fragment_offsets_close(
    offsets: *mut LanceUpdatedFragmentOffsets,
) {
    if !offsets.is_null() {
        unsafe { drop(Box::from_raw(offsets)) };
    }
}

/// Return the number of fragment entries in an `UpdatedFragmentOffsets`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_updated_fragment_offsets_count(
    offsets: *const LanceUpdatedFragmentOffsets,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if offsets.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "offsets must not be NULL".into(),
                ));
            }
            count_out(unsafe { &*offsets }.inner.0.len(), out_count, "out_count")
        })(),
        neg
    )
}

/// Read one fragment ID by entry index. Entry order is unspecified.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_updated_fragment_offsets_fragment_id_at(
    offsets: *const LanceUpdatedFragmentOffsets,
    index: usize,
    out_fragment_id: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if offsets.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "offsets must not be NULL".into(),
                ));
            }
            if out_fragment_id.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_fragment_id must not be NULL".into(),
                ));
            }
            let offsets = unsafe { &*offsets };
            let fragment_id = offsets.inner.0.keys().nth(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "fragment entry index {index} is out of range for {} entries",
                        offsets.inner.0.len()
                    )
                    .into(),
                )
            })?;
            unsafe {
                ptr::write(out_fragment_id, *fragment_id);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Return the number of offsets recorded for one entry index.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_updated_fragment_offsets_offset_count_at(
    offsets: *const LanceUpdatedFragmentOffsets,
    index: usize,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if offsets.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "offsets must not be NULL".into(),
                ));
            }
            let offsets = unsafe { &*offsets };
            let bitmap = offsets.inner.0.values().nth(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "fragment entry index {index} is out of range for {} entries",
                        offsets.inner.0.len()
                    )
                    .into(),
                )
            })?;
            count_out(
                usize::try_from(bitmap.len()).unwrap_or(usize::MAX),
                out_count,
                "out_count",
            )
        })(),
        neg
    )
}

/// Copy all offsets for one entry in ascending native bitmap order.
///
/// The caller owns `out_offsets` and supplies its exact writable element count.
/// A NULL output is accepted only for a valid empty entry with `count == 0`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_updated_fragment_offsets_copy_at(
    offsets: *const LanceUpdatedFragmentOffsets,
    index: usize,
    out_offsets: *mut u32,
    count: usize,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if offsets.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "offsets must not be NULL".into(),
                ));
            }
            let offsets = unsafe { &*offsets };
            let bitmap = offsets.inner.0.values().nth(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "fragment entry index {index} is out of range for {} entries",
                        offsets.inner.0.len()
                    )
                    .into(),
                )
            })?;
            let expected_count = usize::try_from(bitmap.len()).unwrap_or(usize::MAX);
            if count != expected_count {
                return Err(lance_core::Error::invalid_input_source(
                    format!(
                        "offset output count {count} must equal {expected_count} for fragment entry index {index}"
                    )
                    .into(),
                ));
            }
            helpers::validate_slice_length::<u32>(count, "out_offsets")?;
            if count != 0 {
                if out_offsets.is_null() {
                    return Err(lance_core::Error::invalid_input_source(
                        format!("out_offsets must not be NULL when count = {count}").into(),
                    ));
                }
                for (position, offset) in bitmap.iter().enumerate() {
                    unsafe {
                        ptr::write(out_offsets.add(position), offset);
                    }
                }
            }
            Ok(0)
        })(),
        neg
    )
}

/// Read one offset by entry index and ascending offset position.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_updated_fragment_offsets_offset_at(
    offsets: *const LanceUpdatedFragmentOffsets,
    index: usize,
    position: usize,
    out_offset: *mut u32,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if offsets.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "offsets must not be NULL".into(),
                ));
            }
            if out_offset.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_offset must not be NULL".into(),
                ));
            }
            let offsets = unsafe { &*offsets };
            let bitmap = offsets.inner.0.values().nth(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "fragment entry index {index} is out of range for {} entries",
                        offsets.inner.0.len()
                    )
                    .into(),
                )
            })?;
            let offset = bitmap.iter().nth(position).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "offset position {position} is out of range for {} offsets",
                        bitmap.len()
                    )
                    .into(),
                )
            })?;
            unsafe {
                ptr::write(out_offset, offset);
            }
            Ok(0)
        })(),
        neg
    )
}

// ─── KeyExistenceFilter ─────────────────────────────────────────────────────

fn parse_key_existence_filter(
    bytes: *const u8,
    len: usize,
    label: &str,
) -> Result<KeyExistenceFilter> {
    let bytes = unsafe { helpers::required_slice(bytes, len, label, "len") }?;
    let message = pb_actions::KeyExistenceFilter::decode(bytes).map_err(|error| {
        lance_core::Error::invalid_input_source(
            format!("{label} is not a native key-existence filter: {error}").into(),
        )
    })?;
    KeyExistenceFilter::try_from(&message).map_err(|error| {
        lance_core::Error::invalid_input_source(
            format!("{label} is not a valid native key-existence filter: {error}").into(),
        )
    })
}

fn exact_keys(filter: &KeyExistenceFilter) -> Result<&std::collections::HashSet<u64>> {
    match &filter.filter {
        FilterType::ExactSet(keys) => Ok(keys),
        FilterType::Bloom { .. } => Err(lance_core::Error::not_supported_source(
            "exact key inspection is not supported for bloom filters".into(),
        )),
    }
}

struct BloomValues<'a> {
    bitmap: &'a [u8],
    num_bits: u32,
    number_of_items: u64,
    probability: f64,
}

fn bloom_values(filter: &KeyExistenceFilter) -> Result<BloomValues<'_>> {
    match &filter.filter {
        FilterType::Bloom {
            bitmap,
            num_bits,
            number_of_items,
            probability,
        } => Ok(BloomValues {
            bitmap,
            num_bits: *num_bits,
            number_of_items: *number_of_items,
            probability: *probability,
        }),
        FilterType::ExactSet(_) => Err(lance_core::Error::not_supported_source(
            "bloom inspection is not supported for exact-set filters".into(),
        )),
    }
}

/// Build an owned native key-existence filter from explicit protobuf bytes.
///
/// The bytes are one encoded `lance.table.transaction.KeyExistenceFilter`
/// message, converted through native `TryFrom`: absent bloom fields take the
/// native defaults and an absent filter body becomes an empty exact set. NULL
/// bytes are accepted only with a zero length. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_key_existence_filter_from_proto(
    bytes: *const u8,
    len: usize,
    out_filter: *mut *mut LanceKeyExistenceFilter,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_filter.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_filter must not be NULL".into(),
                ));
            }
            let filter = parse_key_existence_filter(bytes, len, "bytes")?;
            let handle = Box::into_raw(Box::new(LanceKeyExistenceFilter { inner: filter }));
            unsafe {
                ptr::write(out_filter, handle);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Clone an owned native key-existence filter. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_key_existence_filter_clone(
    filter: *const LanceKeyExistenceFilter,
) -> *mut LanceKeyExistenceFilter {
    ffi_try!(
        (|| -> Result<*mut LanceKeyExistenceFilter> {
            if filter.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "filter must not be NULL".into(),
                ));
            }
            let filter = unsafe { &*filter };
            Ok(Box::into_raw(Box::new(LanceKeyExistenceFilter {
                inner: filter.inner.clone(),
            })))
        })(),
        null
    )
}

/// Free an owned key-existence filter. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_key_existence_filter_close(filter: *mut LanceKeyExistenceFilter) {
    if !filter.is_null() {
        unsafe { drop(Box::from_raw(filter)) };
    }
}

/// Read the filter kind.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_key_existence_filter_kind(
    filter: *const LanceKeyExistenceFilter,
    out_kind: *mut LanceKeyExistenceFilterKind,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if filter.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "filter must not be NULL".into(),
                ));
            }
            if out_kind.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_kind must not be NULL".into(),
                ));
            }
            let filter = unsafe { &*filter };
            let kind = match &filter.inner.filter {
                FilterType::ExactSet(_) => LanceKeyExistenceFilterKind::Exact,
                FilterType::Bloom { .. } => LanceKeyExistenceFilterKind::Bloom,
            };
            unsafe {
                ptr::write(out_kind, kind);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Return the number of filter field IDs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_key_existence_filter_field_id_count(
    filter: *const LanceKeyExistenceFilter,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if filter.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "filter must not be NULL".into(),
                ));
            }
            count_out(
                unsafe { &*filter }.inner.field_ids.len(),
                out_count,
                "out_count",
            )
        })(),
        neg
    )
}

/// Read one filter field ID by position.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_key_existence_filter_field_id_at(
    filter: *const LanceKeyExistenceFilter,
    index: usize,
    out_field_id: *mut i32,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if filter.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "filter must not be NULL".into(),
                ));
            }
            if out_field_id.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_field_id must not be NULL".into(),
                ));
            }
            let filter = unsafe { &*filter };
            let field_id = filter.inner.field_ids.get(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "field ID index {index} is out of range for {} field IDs",
                        filter.inner.field_ids.len()
                    )
                    .into(),
                )
            })?;
            unsafe {
                ptr::write(out_field_id, *field_id);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Return the number of keys in an exact-set filter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_key_existence_filter_exact_count(
    filter: *const LanceKeyExistenceFilter,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if filter.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "filter must not be NULL".into(),
                ));
            }
            let filter = unsafe { &*filter };
            count_out(exact_keys(&filter.inner)?.len(), out_count, "out_count")
        })(),
        neg
    )
}

/// Read one exact key by ascending position.
///
/// Native exact sets are unordered, so keys are reported in ascending order to
/// make repeated inspections stable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_key_existence_filter_exact_key_at(
    filter: *const LanceKeyExistenceFilter,
    index: usize,
    out_key: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if filter.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "filter must not be NULL".into(),
                ));
            }
            if out_key.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_key must not be NULL".into(),
                ));
            }
            let filter = unsafe { &*filter };
            let keys = exact_keys(&filter.inner)?;
            let mut sorted: Vec<u64> = keys.iter().copied().collect();
            sorted.sort_unstable();
            let key = sorted.get(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "exact key index {index} is out of range for {} keys",
                        sorted.len()
                    )
                    .into(),
                )
            })?;
            unsafe {
                ptr::write(out_key, *key);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Copy a bloom filter's bitmap and parameters.
///
/// The bitmap is allocated with `malloc` and released with `lance_free_bytes`.
/// All output pointers are required and are written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_key_existence_filter_bloom(
    filter: *const LanceKeyExistenceFilter,
    out_bitmap: *mut *mut u8,
    out_bitmap_len: *mut usize,
    out_num_bits: *mut u32,
    out_number_of_items: *mut u64,
    out_probability: *mut f64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if filter.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "filter must not be NULL".into(),
                ));
            }
            if out_bitmap.is_null()
                || out_bitmap_len.is_null()
                || out_num_bits.is_null()
                || out_number_of_items.is_null()
                || out_probability.is_null()
            {
                return Err(lance_core::Error::invalid_input_source(
                    "bloom output pointers must not be NULL".into(),
                ));
            }
            let filter = unsafe { &*filter };
            let values = bloom_values(&filter.inner)?;
            let len = values.bitmap.len();
            let allocation = if len == 0 {
                ptr::null_mut()
            } else {
                let allocation = unsafe { libc::malloc(len) }.cast::<u8>();
                if allocation.is_null() {
                    return Err(lance_core::Error::internal(format!(
                        "failed to allocate {len} bitmap bytes"
                    )));
                }
                unsafe { ptr::copy_nonoverlapping(values.bitmap.as_ptr(), allocation, len) };
                allocation
            };
            unsafe {
                ptr::write(out_bitmap, allocation);
                ptr::write(out_bitmap_len, len);
                ptr::write(out_num_bits, values.num_bits);
                ptr::write(out_number_of_items, values.number_of_items);
                ptr::write(out_probability, values.probability);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Encode a key-existence filter with the native protobuf conversion.
///
/// The bytes are allocated with `malloc` and released with `lance_free_bytes`.
/// This is the same encoding an Update operation persists, so a filter that was
/// built from protobuf bytes round-trips exactly.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_key_existence_filter_to_proto(
    filter: *const LanceKeyExistenceFilter,
    out_bytes: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if filter.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "filter must not be NULL".into(),
                ));
            }
            if out_bytes.is_null() || out_len.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_bytes and out_len must not be NULL".into(),
                ));
            }
            let filter = unsafe { &*filter };
            let message: pb_actions::KeyExistenceFilter = (&filter.inner).into();
            let encoded = message.encode_to_vec();
            let len = encoded.len();
            let allocation = if len == 0 {
                ptr::null_mut()
            } else {
                let allocation = unsafe { libc::malloc(len) }.cast::<u8>();
                if allocation.is_null() {
                    return Err(lance_core::Error::internal(format!(
                        "failed to allocate {len} filter bytes"
                    )));
                }
                unsafe { ptr::copy_nonoverlapping(encoded.as_ptr(), allocation, len) };
                allocation
            };
            unsafe {
                ptr::write(out_bytes, allocation);
                ptr::write(out_len, len);
            }
            Ok(0)
        })(),
        neg
    )
}

// ─── Update operation ───────────────────────────────────────────────────────

/// Construct an owned native Update operation.
///
/// `options` is required. Every array in it is borrowed for this call and
/// copied into native owned values; see `LanceUpdateOperationOptions` for the
/// tri-state fields. No field is defaulted: an empty `fields_modified`,
/// `compacted_sstables` or `removed_fragment_ids` means the caller supplied
/// none. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_new(
    options: *const LanceUpdateOperationOptions,
) -> *mut LanceOperation {
    unsafe { lance_operation_update_new_with_mem_wal_dir_id(options, ptr::null()) }
}

/// Construct an Update operation that names its MemWAL directory.
///
/// Identical to `lance_operation_update_new()` except that `mem_wal_dir_id`
/// maps to native `Some(...)`; NULL maps to `None`. Native code requires the
/// identifier when `compacted_sstables` carries compaction progress.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_new_with_mem_wal_dir_id(
    options: *const LanceUpdateOperationOptions,
    mem_wal_dir_id: *const c_char,
) -> *mut LanceOperation {
    ffi_try!(
        (|| -> Result<*mut LanceOperation> {
            let mem_wal_dir_id =
                unsafe { helpers::parse_c_string(mem_wal_dir_id)? }.map(str::to_string);
            if options.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "options must not be NULL".into(),
                ));
            }
            let options = unsafe { &*options };
            let removed_fragment_ids = unsafe {
                helpers::required_slice(
                    options.removed_fragment_ids,
                    options.removed_fragment_count,
                    "removed_fragment_ids",
                    "removed_fragment_count",
                )?
            }
            .to_vec();
            let updated_fragments = unsafe {
                clone_fragment_array(
                    options.updated_fragments,
                    options.updated_fragment_count,
                    "updated_fragments",
                )?
            };
            let new_fragments = unsafe {
                clone_fragment_array(
                    options.new_fragments,
                    options.new_fragment_count,
                    "new_fragments",
                )?
            };
            let fields_modified = unsafe {
                helpers::required_slice(
                    options.fields_modified,
                    options.fields_modified_count,
                    "fields_modified",
                    "fields_modified_count",
                )?
            }
            .to_vec();
            let compacted_sstables = unsafe {
                crate::mem_wal::parse_compacted_sstables(
                    options.compacted_sstables,
                    options.compacted_sstable_count,
                )?
            };
            let fields_for_preserving_frag_bitmap = unsafe {
                helpers::required_slice(
                    options.fields_for_preserving_frag_bitmap,
                    options.fields_for_preserving_frag_bitmap_count,
                    "fields_for_preserving_frag_bitmap",
                    "fields_for_preserving_frag_bitmap_count",
                )?
            }
            .to_vec();
            let update_mode = if options.has_update_mode {
                Some(parse_update_mode(options.update_mode)?)
            } else {
                None
            };
            let inserted_rows_filter = if options.inserted_rows_filter.is_null() {
                None
            } else {
                Some(unsafe { &*options.inserted_rows_filter }.inner.clone())
            };
            let updated_fragment_offsets = if options.updated_fragment_offsets.is_null() {
                None
            } else {
                Some(unsafe { &*options.updated_fragment_offsets }.inner.clone())
            };
            Ok(Box::into_raw(Box::new(LanceOperation::from_native(
                Operation::Update {
                    removed_fragment_ids,
                    updated_fragments,
                    new_fragments,
                    fields_modified,
                    compacted_sstables,
                    fields_for_preserving_frag_bitmap,
                    update_mode,
                    inserted_rows_filter,
                    updated_fragment_offsets,
                    mem_wal_dir_id,
                },
            ))))
        })(),
        null
    )
}

/// Return the number of removed fragment IDs in an Update operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_removed_fragment_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let operation = operation_at(operation)?;
            let Operation::Update {
                removed_fragment_ids,
                ..
            } = &operation.inner
            else {
                return Err(update_kind_error(&operation.inner));
            };
            count_out(removed_fragment_ids.len(), out_count, "out_count")
        })(),
        neg
    )
}

/// Read one removed fragment ID by position.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_removed_fragment_id_at(
    operation: *const LanceOperation,
    index: usize,
    out_fragment_id: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_fragment_id.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_fragment_id must not be NULL".into(),
                ));
            }
            let operation = operation_at(operation)?;
            let Operation::Update {
                removed_fragment_ids,
                ..
            } = &operation.inner
            else {
                return Err(update_kind_error(&operation.inner));
            };
            let fragment_id = removed_fragment_ids.get(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "removed fragment ID index {index} is out of range for {} entries",
                        removed_fragment_ids.len()
                    )
                    .into(),
                )
            })?;
            unsafe {
                ptr::write(out_fragment_id, *fragment_id);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Return the number of updated fragments in an Update operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_updated_fragment_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let operation = operation_at(operation)?;
            let Operation::Update {
                updated_fragments, ..
            } = &operation.inner
            else {
                return Err(update_kind_error(&operation.inner));
            };
            count_out(updated_fragments.len(), out_count, "out_count")
        })(),
        neg
    )
}

/// Clone one updated fragment by position.
///
/// The returned handle owns a complete native fragment snapshot and remains
/// valid after the operation handle is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_updated_fragment_at(
    operation: *const LanceOperation,
    index: usize,
    out_fragment: *mut *mut LanceFragment,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let operation = operation_at(operation)?;
            let Operation::Update {
                updated_fragments, ..
            } = &operation.inner
            else {
                return Err(update_kind_error(&operation.inner));
            };
            let fragment = updated_fragments.get(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "updated fragment index {index} is out of range for {} fragments",
                        updated_fragments.len()
                    )
                    .into(),
                )
            })?;
            unsafe { fragment_out(fragment, out_fragment) }
        })(),
        neg
    )
}

/// Return the number of new fragments in an Update operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_new_fragment_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let operation = operation_at(operation)?;
            let Operation::Update { new_fragments, .. } = &operation.inner else {
                return Err(update_kind_error(&operation.inner));
            };
            count_out(new_fragments.len(), out_count, "out_count")
        })(),
        neg
    )
}

/// Clone one new fragment by position.
///
/// The returned handle owns a complete native fragment snapshot and remains
/// valid after the operation handle is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_new_fragment_at(
    operation: *const LanceOperation,
    index: usize,
    out_fragment: *mut *mut LanceFragment,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let operation = operation_at(operation)?;
            let Operation::Update { new_fragments, .. } = &operation.inner else {
                return Err(update_kind_error(&operation.inner));
            };
            let fragment = new_fragments.get(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "new fragment index {index} is out of range for {} fragments",
                        new_fragments.len()
                    )
                    .into(),
                )
            })?;
            unsafe { fragment_out(fragment, out_fragment) }
        })(),
        neg
    )
}

/// Return the number of modified field IDs in an Update operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_fields_modified_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let operation = operation_at(operation)?;
            let Operation::Update {
                fields_modified, ..
            } = &operation.inner
            else {
                return Err(update_kind_error(&operation.inner));
            };
            count_out(fields_modified.len(), out_count, "out_count")
        })(),
        neg
    )
}

/// Read one modified field ID by position.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_fields_modified_at(
    operation: *const LanceOperation,
    index: usize,
    out_field_id: *mut u32,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_field_id.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_field_id must not be NULL".into(),
                ));
            }
            let operation = operation_at(operation)?;
            let Operation::Update {
                fields_modified, ..
            } = &operation.inner
            else {
                return Err(update_kind_error(&operation.inner));
            };
            let field_id = fields_modified.get(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "modified field ID index {index} is out of range for {} entries",
                        fields_modified.len()
                    )
                    .into(),
                )
            })?;
            unsafe {
                ptr::write(out_field_id, *field_id);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Return the number of compacted SSTables in an Update operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_compacted_sstable_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let operation = operation_at(operation)?;
            let Operation::Update {
                compacted_sstables, ..
            } = &operation.inner
            else {
                return Err(update_kind_error(&operation.inner));
            };
            count_out(compacted_sstables.len(), out_count, "out_count")
        })(),
        neg
    )
}

/// Copy one compacted SSTable entry by position.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_compacted_sstable_at(
    operation: *const LanceOperation,
    index: usize,
    out_sstable: *mut LanceCompactedSsTable,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_sstable.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_sstable must not be NULL".into(),
                ));
            }
            let operation = operation_at(operation)?;
            let Operation::Update {
                compacted_sstables, ..
            } = &operation.inner
            else {
                return Err(update_kind_error(&operation.inner));
            };
            let sstable = compacted_sstables.get(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "compacted SSTable index {index} is out of range for {} entries",
                        compacted_sstables.len()
                    )
                    .into(),
                )
            })?;
            unsafe {
                ptr::write(out_sstable, LanceCompactedSsTable::from_native(sstable));
            }
            Ok(0)
        })(),
        neg
    )
}

/// Return the number of preserved-fragment-bitmap field IDs in an Update.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_fields_for_preserving_frag_bitmap_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let operation = operation_at(operation)?;
            let Operation::Update {
                fields_for_preserving_frag_bitmap,
                ..
            } = &operation.inner
            else {
                return Err(update_kind_error(&operation.inner));
            };
            count_out(
                fields_for_preserving_frag_bitmap.len(),
                out_count,
                "out_count",
            )
        })(),
        neg
    )
}

/// Read one preserved-fragment-bitmap field ID by position.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_fields_for_preserving_frag_bitmap_at(
    operation: *const LanceOperation,
    index: usize,
    out_field_id: *mut u32,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_field_id.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_field_id must not be NULL".into(),
                ));
            }
            let operation = operation_at(operation)?;
            let Operation::Update {
                fields_for_preserving_frag_bitmap,
                ..
            } = &operation.inner
            else {
                return Err(update_kind_error(&operation.inner));
            };
            let field_id = fields_for_preserving_frag_bitmap.get(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "preserved-fragment-bitmap field ID index {index} is out of range for {} entries",
                        fields_for_preserving_frag_bitmap.len()
                    )
                    .into(),
                )
            })?;
            unsafe {
                ptr::write(out_field_id, *field_id);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Read the native update mode, distinguishing `None` from both variants.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_mode(
    operation: *const LanceOperation,
    out_present: *mut bool,
    out_mode: *mut i32,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_present.is_null() || out_mode.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_present and out_mode must not be NULL".into(),
                ));
            }
            let operation = operation_at(operation)?;
            let Operation::Update { update_mode, .. } = &operation.inner else {
                return Err(update_kind_error(&operation.inner));
            };
            unsafe {
                ptr::write(out_present, update_mode.is_some());
                if let Some(update_mode) = update_mode {
                    ptr::write(out_mode, update_mode_code(update_mode));
                }
            }
            Ok(0)
        })(),
        neg
    )
}

/// Clone the inserted-rows key-existence filter of an Update operation.
///
/// Writes `false` and a NULL handle when the native field is `None`; otherwise
/// writes `true` and an independently owned filter. On error both outputs are
/// unchanged.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_inserted_rows_filter(
    operation: *const LanceOperation,
    out_present: *mut bool,
    out_filter: *mut *mut LanceKeyExistenceFilter,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_present.is_null() || out_filter.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_present and out_filter must not be NULL".into(),
                ));
            }
            let operation = operation_at(operation)?;
            let Operation::Update {
                inserted_rows_filter,
                ..
            } = &operation.inner
            else {
                return Err(update_kind_error(&operation.inner));
            };
            let handle = inserted_rows_filter.as_ref().map(|filter| {
                Box::into_raw(Box::new(LanceKeyExistenceFilter {
                    inner: filter.clone(),
                }))
            });
            unsafe {
                ptr::write(out_present, handle.is_some());
                ptr::write(out_filter, handle.unwrap_or(ptr::null_mut()));
            }
            Ok(0)
        })(),
        neg
    )
}

/// Clone the updated-fragment offsets of an Update operation.
///
/// Writes `false` and a NULL handle when the native field is `None`, preserving
/// the distinction between `None` and `Some(empty)`; otherwise writes `true`
/// and an independently owned offsets value. On error both outputs are
/// unchanged.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_updated_fragment_offsets(
    operation: *const LanceOperation,
    out_present: *mut bool,
    out_offsets: *mut *mut LanceUpdatedFragmentOffsets,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_present.is_null() || out_offsets.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_present and out_offsets must not be NULL".into(),
                ));
            }
            let operation = operation_at(operation)?;
            let Operation::Update {
                updated_fragment_offsets,
                ..
            } = &operation.inner
            else {
                return Err(update_kind_error(&operation.inner));
            };
            let handle = updated_fragment_offsets.as_ref().map(|offsets| {
                Box::into_raw(Box::new(LanceUpdatedFragmentOffsets {
                    inner: offsets.clone(),
                }))
            });
            unsafe {
                ptr::write(out_present, handle.is_some());
                ptr::write(out_offsets, handle.unwrap_or(ptr::null_mut()));
            }
            Ok(0)
        })(),
        neg
    )
}

// ─── Rewrite operation ──────────────────────────────────────────────────────

/// Construct an owned native `RewriteGroup` from two fragment arrays.
///
/// Every fragment handle is borrowed and copied, so the group owns complete
/// native fragment metadata. NULL arrays are accepted only with a zero count.
/// Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_group_new(
    old_fragments: *const *const LanceFragment,
    old_fragment_count: usize,
    new_fragments: *const *const LanceFragment,
    new_fragment_count: usize,
) -> *mut LanceRewriteGroup {
    ffi_try!(
        (|| -> Result<*mut LanceRewriteGroup> {
            let old_fragments = unsafe {
                clone_fragment_array(old_fragments, old_fragment_count, "old_fragments")?
            };
            let new_fragments = unsafe {
                clone_fragment_array(new_fragments, new_fragment_count, "new_fragments")?
            };
            Ok(Box::into_raw(Box::new(LanceRewriteGroup {
                inner: RewriteGroup {
                    old_fragments,
                    new_fragments,
                },
            })))
        })(),
        null
    )
}

/// Clone an owned native `RewriteGroup`. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_group_clone(
    group: *const LanceRewriteGroup,
) -> *mut LanceRewriteGroup {
    ffi_try!(
        (|| -> Result<*mut LanceRewriteGroup> {
            if group.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "group must not be NULL".into(),
                ));
            }
            let group = unsafe { &*group };
            Ok(Box::into_raw(Box::new(LanceRewriteGroup {
                inner: group.inner.clone(),
            })))
        })(),
        null
    )
}

/// Free an owned `RewriteGroup`. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_group_close(group: *mut LanceRewriteGroup) {
    if !group.is_null() {
        unsafe { drop(Box::from_raw(group)) };
    }
}

/// Return the number of replaced fragments in a `RewriteGroup`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_group_old_fragment_count(
    group: *const LanceRewriteGroup,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if group.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "group must not be NULL".into(),
                ));
            }
            count_out(
                unsafe { &*group }.inner.old_fragments.len(),
                out_count,
                "out_count",
            )
        })(),
        neg
    )
}

/// Clone one replaced fragment of a `RewriteGroup` by position.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_group_old_fragment_at(
    group: *const LanceRewriteGroup,
    index: usize,
    out_fragment: *mut *mut LanceFragment,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if group.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "group must not be NULL".into(),
                ));
            }
            let group = unsafe { &*group };
            let fragment = group.inner.old_fragments.get(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "old fragment index {index} is out of range for {} fragments",
                        group.inner.old_fragments.len()
                    )
                    .into(),
                )
            })?;
            unsafe { fragment_out(fragment, out_fragment) }
        })(),
        neg
    )
}

/// Return the number of replacement fragments in a `RewriteGroup`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_group_new_fragment_count(
    group: *const LanceRewriteGroup,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if group.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "group must not be NULL".into(),
                ));
            }
            count_out(
                unsafe { &*group }.inner.new_fragments.len(),
                out_count,
                "out_count",
            )
        })(),
        neg
    )
}

/// Clone one replacement fragment of a `RewriteGroup` by position.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewrite_group_new_fragment_at(
    group: *const LanceRewriteGroup,
    index: usize,
    out_fragment: *mut *mut LanceFragment,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if group.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "group must not be NULL".into(),
                ));
            }
            let group = unsafe { &*group };
            let fragment = group.inner.new_fragments.get(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "new fragment index {index} is out of range for {} fragments",
                        group.inner.new_fragments.len()
                    )
                    .into(),
                )
            })?;
            unsafe { fragment_out(fragment, out_fragment) }
        })(),
        neg
    )
}

unsafe fn required_uuid(value: *const u8, label: &str) -> Result<uuid::Uuid> {
    if value.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            format!("{label} must not be NULL").into(),
        ));
    }
    let mut bytes = [0_u8; 16];
    unsafe { ptr::copy_nonoverlapping(value, bytes.as_mut_ptr(), bytes.len()) };
    Ok(uuid::Uuid::from_bytes(bytes))
}

/// Construct an owned native `RewrittenIndex`.
///
/// `old_id` and `new_id` must each point to 16 canonical UUID bytes.
/// `details_type_url` must not be NULL but may be empty, matching the native
/// `prost_types::Any` field. `details_value` may be NULL only with a zero
/// length. `has_index_files == false` maps to native `None` and requires a zero
/// count; `has_index_files == true` with a zero count is the distinct
/// `Some(empty)` state that older writers persisted. Returns NULL on error.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn lance_rewritten_index_new(
    old_id: *const u8,
    new_id: *const u8,
    details_type_url: *const c_char,
    details_value: *const u8,
    details_value_len: usize,
    index_version: u32,
    has_index_files: bool,
    index_files: *const LanceIndexFile,
    index_file_count: usize,
) -> *mut LanceRewrittenIndex {
    ffi_try!(
        (|| -> Result<*mut LanceRewrittenIndex> {
            let old_id = unsafe { required_uuid(old_id, "old_id") }?;
            let new_id = unsafe { required_uuid(new_id, "new_id") }?;
            let details_type_url = required_c_string(details_type_url, "details_type_url")?;
            let details_value = unsafe {
                helpers::required_slice(details_value, details_value_len, "details_value", "len")
            }?
            .to_vec();
            if !has_index_files && index_file_count != 0 {
                return Err(lance_core::Error::invalid_input_source(
                    format!(
                        "index_file_count must be 0 when has_index_files is false, got {index_file_count}"
                    )
                    .into(),
                ));
            }
            let files = if !has_index_files {
                None
            } else {
                let entries = unsafe {
                    helpers::required_slice(
                        index_files,
                        index_file_count,
                        "index_files",
                        "index_file_count",
                    )?
                };
                let mut files = Vec::with_capacity(entries.len());
                for (index, entry) in entries.iter().enumerate() {
                    if entry.path.is_null() {
                        return Err(lance_core::Error::invalid_input_source(
                            format!("index_files[{index}].path must not be NULL").into(),
                        ));
                    }
                    let path = unsafe { std::ffi::CStr::from_ptr(entry.path) }
                        .to_str()
                        .map_err(|error| {
                            lance_core::Error::invalid_input_source(
                                format!("index_files[{index}].path is not valid UTF-8: {error}")
                                    .into(),
                            )
                        })?;
                    files.push(IndexFile {
                        path: path.to_string(),
                        size_bytes: entry.size_bytes,
                        file_metadata_size_bytes: None,
                    });
                }
                Some(files)
            };
            Ok(Box::into_raw(Box::new(LanceRewrittenIndex {
                inner: RewrittenIndex {
                    old_id,
                    new_id,
                    new_index_details: prost_types::Any {
                        type_url: details_type_url.to_string(),
                        value: details_value,
                    },
                    new_index_version: index_version,
                    new_index_files: files,
                },
            })))
        })(),
        null
    )
}

/// Clone an owned native `RewrittenIndex`. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewritten_index_clone(
    index: *const LanceRewrittenIndex,
) -> *mut LanceRewrittenIndex {
    ffi_try!(
        (|| -> Result<*mut LanceRewrittenIndex> {
            if index.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "index must not be NULL".into(),
                ));
            }
            let index = unsafe { &*index };
            Ok(Box::into_raw(Box::new(LanceRewrittenIndex {
                inner: index.inner.clone(),
            })))
        })(),
        null
    )
}

/// Free an owned `RewrittenIndex`. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewritten_index_close(index: *mut LanceRewrittenIndex) {
    if !index.is_null() {
        unsafe { drop(Box::from_raw(index)) };
    }
}

/// Copy the old index UUID as 16 bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewritten_index_old_id(
    index: *const LanceRewrittenIndex,
    out_uuid: *mut u8,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if index.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "index must not be NULL".into(),
                ));
            }
            let bytes = unsafe { &*index }.inner.old_id.as_bytes();
            unsafe {
                required_out_bytes(out_uuid, "out_uuid")?;
                ptr::copy_nonoverlapping(bytes.as_ptr(), out_uuid, 16);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Copy the new index UUID as 16 bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewritten_index_new_id(
    index: *const LanceRewrittenIndex,
    out_uuid: *mut u8,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if index.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "index must not be NULL".into(),
                ));
            }
            let bytes = unsafe { &*index }.inner.new_id.as_bytes();
            unsafe {
                required_out_bytes(out_uuid, "out_uuid")?;
                ptr::copy_nonoverlapping(bytes.as_ptr(), out_uuid, 16);
            }
            Ok(0)
        })(),
        neg
    )
}

unsafe fn required_out_bytes(out: *mut u8, label: &str) -> Result<()> {
    if out.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            format!("{label} must not be NULL").into(),
        ));
    }
    Ok(())
}

/// Copy the index-details type URL. The string is allocated with `malloc` and
/// released with `lance_free_string`. Returns NULL on error or an embedded NUL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewritten_index_details_type_url(
    index: *const LanceRewrittenIndex,
) -> *const c_char {
    ffi_try!(
        (|| -> Result<*const c_char> {
            if index.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "index must not be NULL".into(),
                ));
            }
            let index = unsafe { &*index };
            Ok(owned_c_string(
                &index.inner.new_index_details.type_url,
                "new_index_details.type_url",
            )?
            .into_raw())
        })(),
        ptr::null()
    )
}

/// Copy the index-details value bytes. The buffer is allocated with `malloc`
/// and released with `lance_free_bytes`; requirement rules match
/// `lance_key_existence_filter_to_proto`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewritten_index_details_value(
    index: *const LanceRewrittenIndex,
    out_bytes: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if index.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "index must not be NULL".into(),
                ));
            }
            if out_bytes.is_null() || out_len.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_bytes and out_len must not be NULL".into(),
                ));
            }
            let index = unsafe { &*index };
            let value = &index.inner.new_index_details.value;
            let len = value.len();
            let allocation = if len == 0 {
                ptr::null_mut()
            } else {
                let allocation = unsafe { libc::malloc(len) }.cast::<u8>();
                if allocation.is_null() {
                    return Err(lance_core::Error::internal(format!(
                        "failed to allocate {len} index-details bytes"
                    )));
                }
                unsafe { ptr::copy_nonoverlapping(value.as_ptr(), allocation, len) };
                allocation
            };
            unsafe {
                ptr::write(out_bytes, allocation);
                ptr::write(out_len, len);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Read the new index version.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewritten_index_version(
    index: *const LanceRewrittenIndex,
    out_version: *mut u32,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if index.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "index must not be NULL".into(),
                ));
            }
            if out_version.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_version must not be NULL".into(),
                ));
            }
            unsafe {
                ptr::write(out_version, (&*index).inner.new_index_version);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Report whether the rewritten index carries an index-file list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewritten_index_has_files(
    index: *const LanceRewrittenIndex,
    out_present: *mut bool,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if index.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "index must not be NULL".into(),
                ));
            }
            if out_present.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_present must not be NULL".into(),
                ));
            }
            unsafe {
                ptr::write(out_present, (&*index).inner.new_index_files.is_some());
            }
            Ok(0)
        })(),
        neg
    )
}

/// Return the number of index files; 0 also when the native list is `None`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewritten_index_file_count(
    index: *const LanceRewrittenIndex,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if index.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "index must not be NULL".into(),
                ));
            }
            let index = unsafe { &*index };
            count_out(
                index
                    .inner
                    .new_index_files
                    .as_ref()
                    .map(Vec::len)
                    .unwrap_or(0),
                out_count,
                "out_count",
            )
        })(),
        neg
    )
}

/// Copy one index-file path. The string is allocated with `malloc` and released
/// with `lance_free_string`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewritten_index_file_path_at(
    index: *const LanceRewrittenIndex,
    position: usize,
) -> *const c_char {
    ffi_try!(
        (|| -> Result<*const c_char> {
            if index.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "index must not be NULL".into(),
                ));
            }
            let index = unsafe { &*index };
            let files = index.inner.new_index_files.as_ref().ok_or_else(|| {
                lance_core::Error::not_supported_source(
                    "index file inspection requires a present index-file list".into(),
                )
            })?;
            let file = files.get(position).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "index file index {position} is out of range for {} files",
                        files.len()
                    )
                    .into(),
                )
            })?;
            Ok(owned_c_string(&file.path, "index file path")?.into_raw())
        })(),
        ptr::null()
    )
}

/// Read one index-file size by position.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_rewritten_index_file_size_at(
    index: *const LanceRewrittenIndex,
    position: usize,
    out_size: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if index.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "index must not be NULL".into(),
                ));
            }
            if out_size.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_size must not be NULL".into(),
                ));
            }
            let index = unsafe { &*index };
            let files = index.inner.new_index_files.as_ref().ok_or_else(|| {
                lance_core::Error::not_supported_source(
                    "index file inspection requires a present index-file list".into(),
                )
            })?;
            let file = files.get(position).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "index file index {position} is out of range for {} files",
                        files.len()
                    )
                    .into(),
                )
            })?;
            unsafe {
                ptr::write(out_size, file.size_bytes);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Construct an owned native Rewrite operation.
///
/// Groups and rewritten indices are cloned from their handles. `frag_reuse_index`
/// may be NULL for native `None`; otherwise the complete native index metadata is
/// copied. The index is in-memory only: native serialization never writes it, so
/// a protobuf round trip yields `None` (see
/// `lance_operation_rewrite_frag_reuse_index`). Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_rewrite_new(
    groups: *const *const LanceRewriteGroup,
    group_count: usize,
    rewritten_indices: *const *const LanceRewrittenIndex,
    rewritten_index_count: usize,
    frag_reuse_index: *const LanceIndexSegmentMetadata,
) -> *mut LanceOperation {
    ffi_try!(
        (|| -> Result<*mut LanceOperation> {
            let groups =
                unsafe { helpers::required_slice(groups, group_count, "groups", "group_count")? };
            let mut cloned_groups = Vec::with_capacity(groups.len());
            for (index, group) in groups.iter().enumerate() {
                if group.is_null() {
                    return Err(lance_core::Error::invalid_input_source(
                        format!(
                            "groups[{index}] must not be NULL when group_count = {}",
                            groups.len()
                        )
                        .into(),
                    ));
                }
                cloned_groups.push(unsafe { &**group }.inner.clone());
            }
            let rewritten_indices = unsafe {
                helpers::required_slice(
                    rewritten_indices,
                    rewritten_index_count,
                    "rewritten_indices",
                    "rewritten_index_count",
                )?
            };
            let mut cloned_indices = Vec::with_capacity(rewritten_indices.len());
            for (index, rewritten) in rewritten_indices.iter().enumerate() {
                if rewritten.is_null() {
                    return Err(lance_core::Error::invalid_input_source(
                        format!(
                            "rewritten_indices[{index}] must not be NULL when rewritten_index_count = {}",
                            rewritten_indices.len()
                        )
                        .into(),
                    ));
                }
                cloned_indices.push(unsafe { &**rewritten }.inner.clone());
            }
            let frag_reuse_index = if frag_reuse_index.is_null() {
                None
            } else {
                Some(unsafe { &*frag_reuse_index }.native_metadata().clone())
            };
            Ok(Box::into_raw(Box::new(LanceOperation::from_native(
                Operation::Rewrite {
                    groups: cloned_groups,
                    rewritten_indices: cloned_indices,
                    frag_reuse_index,
                },
            ))))
        })(),
        null
    )
}

/// Return the number of groups in a Rewrite operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_rewrite_group_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let operation = operation_at(operation)?;
            let Operation::Rewrite { groups, .. } = &operation.inner else {
                return Err(rewrite_kind_error(&operation.inner));
            };
            count_out(groups.len(), out_count, "out_count")
        })(),
        neg
    )
}

/// Clone one Rewrite group by position.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_rewrite_group_at(
    operation: *const LanceOperation,
    index: usize,
    out_group: *mut *mut LanceRewriteGroup,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_group.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_group must not be NULL".into(),
                ));
            }
            let operation = operation_at(operation)?;
            let Operation::Rewrite { groups, .. } = &operation.inner else {
                return Err(rewrite_kind_error(&operation.inner));
            };
            let group = groups.get(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "rewrite group index {index} is out of range for {} groups",
                        groups.len()
                    )
                    .into(),
                )
            })?;
            let handle = Box::into_raw(Box::new(LanceRewriteGroup {
                inner: group.clone(),
            }));
            unsafe {
                ptr::write(out_group, handle);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Return the number of rewritten indices in a Rewrite operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_rewrite_rewritten_index_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let operation = operation_at(operation)?;
            let Operation::Rewrite {
                rewritten_indices, ..
            } = &operation.inner
            else {
                return Err(rewrite_kind_error(&operation.inner));
            };
            count_out(rewritten_indices.len(), out_count, "out_count")
        })(),
        neg
    )
}

/// Clone one rewritten index by position.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_rewrite_rewritten_index_at(
    operation: *const LanceOperation,
    index: usize,
    out_index: *mut *mut LanceRewrittenIndex,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_index.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_index must not be NULL".into(),
                ));
            }
            let operation = operation_at(operation)?;
            let Operation::Rewrite {
                rewritten_indices, ..
            } = &operation.inner
            else {
                return Err(rewrite_kind_error(&operation.inner));
            };
            let rewritten = rewritten_indices.get(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "rewritten index index {index} is out of range for {} entries",
                        rewritten_indices.len()
                    )
                    .into(),
                )
            })?;
            let handle = Box::into_raw(Box::new(LanceRewrittenIndex {
                inner: rewritten.clone(),
            }));
            unsafe {
                ptr::write(out_index, handle);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Clone the in-memory fragment-reuse index of a Rewrite operation.
///
/// Writes `false` and a NULL handle when the native field is `None`. The
/// returned handle owns a complete native `IndexMetadata` snapshot, including
/// fragment bitmap, index details and files.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_rewrite_frag_reuse_index(
    operation: *const LanceOperation,
    out_present: *mut bool,
    out_metadata: *mut *mut LanceIndexSegmentMetadata,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_present.is_null() || out_metadata.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_present and out_metadata must not be NULL".into(),
                ));
            }
            let operation = operation_at(operation)?;
            let Operation::Rewrite {
                frag_reuse_index, ..
            } = &operation.inner
            else {
                return Err(rewrite_kind_error(&operation.inner));
            };
            let handle = match frag_reuse_index {
                Some(metadata) => metadata_handle(metadata.clone())?,
                None => ptr::null_mut(),
            };
            unsafe {
                ptr::write(out_present, !handle.is_null());
                ptr::write(out_metadata, handle);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Borrow the MemWAL directory identifier named by an Update operation.
///
/// Writes `false` and NULL when native `mem_wal_dir_id` is `None`. The string
/// remains valid until the operation is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_mem_wal_dir_id(
    operation: *const LanceOperation,
    out_present: *mut bool,
    out_dir_id: *mut *const c_char,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_present.is_null() || out_dir_id.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_present and out_dir_id must not be NULL".into(),
                ));
            }
            let operation = operation_at(operation)?;
            let Operation::Update { mem_wal_dir_id, .. } = &operation.inner else {
                return Err(update_kind_error(&operation.inner));
            };
            let value = match mem_wal_dir_id {
                None => ptr::null(),
                Some(_) => match operation.mem_wal_dir_id.as_ref() {
                    Some(Ok(value)) => value.as_ptr(),
                    Some(Err(_)) | None => {
                        return Err(lance_core::Error::internal(String::from(
                            "Update mem_wal_dir_id contains an embedded NUL byte",
                        )));
                    }
                },
            };
            unsafe {
                ptr::write(out_present, mem_wal_dir_id.is_some());
                ptr::write(out_dir_id, value);
            }
            Ok(0)
        })(),
        neg
    )
}

// ─── UpdateMemWalState operation ────────────────────────────────────────────

/// Construct an owned native UpdateMemWalState operation.
///
/// `compacted_sstables` uses the shared `LanceCompactedSsTable` value and the
/// same conversion as merge-insert compaction. A NULL array is accepted only
/// with a zero count. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_mem_wal_state_new(
    compacted_sstables: *const LanceCompactedSsTable,
    compacted_sstable_count: usize,
) -> *mut LanceOperation {
    unsafe {
        lance_operation_update_mem_wal_state_new_with_dir_id(
            compacted_sstables,
            compacted_sstable_count,
            c"".as_ptr(),
        )
    }
}

/// Construct an UpdateMemWalState operation for a specific MemWAL directory.
///
/// `dir_id` is required; pass an empty string for a legacy MemWAL created
/// before directory identifiers existed. Native commit rejects a stale or
/// removed identifier.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_mem_wal_state_new_with_dir_id(
    compacted_sstables: *const LanceCompactedSsTable,
    compacted_sstable_count: usize,
    dir_id: *const c_char,
) -> *mut LanceOperation {
    ffi_try!(
        (|| -> Result<*mut LanceOperation> {
            let dir_id = required_c_string(dir_id, "dir_id")?.to_string();
            let compacted_sstables = unsafe {
                crate::mem_wal::parse_compacted_sstables(
                    compacted_sstables,
                    compacted_sstable_count,
                )?
            };
            Ok(Box::into_raw(Box::new(LanceOperation::from_native(
                Operation::UpdateMemWalState {
                    compacted_sstables,
                    dir_id,
                },
            ))))
        })(),
        null
    )
}

/// Borrow the MemWAL directory identifier named by an UpdateMemWalState
/// operation. An empty string denotes the legacy `_mem_wal` directory. The
/// string remains valid until the operation is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_mem_wal_state_dir_id(
    operation: *const LanceOperation,
    out_dir_id: *mut *const c_char,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_dir_id.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_dir_id must not be NULL".into(),
                ));
            }
            let operation = operation_at(operation)?;
            let Operation::UpdateMemWalState { .. } = &operation.inner else {
                return Err(update_mem_wal_state_kind_error(&operation.inner));
            };
            let value = match operation.mem_wal_dir_id.as_ref() {
                Some(Ok(value)) => value.as_ptr(),
                Some(Err(_)) | None => {
                    return Err(lance_core::Error::internal(String::from(
                        "UpdateMemWalState dir_id contains an embedded NUL byte",
                    )));
                }
            };
            unsafe { ptr::write(out_dir_id, value) };
            Ok(0)
        })(),
        neg
    )
}

/// Return the number of compacted SSTables in an UpdateMemWalState operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_mem_wal_state_compacted_sstable_count(
    operation: *const LanceOperation,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let operation = operation_at(operation)?;
            let Operation::UpdateMemWalState {
                compacted_sstables, ..
            } = &operation.inner
            else {
                return Err(update_mem_wal_state_kind_error(&operation.inner));
            };
            count_out(compacted_sstables.len(), out_count, "out_count")
        })(),
        neg
    )
}

/// Copy one compacted SSTable entry of an UpdateMemWalState operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_update_mem_wal_state_compacted_sstable_at(
    operation: *const LanceOperation,
    index: usize,
    out_sstable: *mut LanceCompactedSsTable,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_sstable.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    "out_sstable must not be NULL".into(),
                ));
            }
            let operation = operation_at(operation)?;
            let Operation::UpdateMemWalState {
                compacted_sstables, ..
            } = &operation.inner
            else {
                return Err(update_mem_wal_state_kind_error(&operation.inner));
            };
            let sstable = compacted_sstables.get(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "compacted SSTable index {index} is out of range for {} entries",
                        compacted_sstables.len()
                    )
                    .into(),
                )
            })?;
            unsafe {
                ptr::write(out_sstable, LanceCompactedSsTable::from_native(sstable));
            }
            Ok(0)
        })(),
        neg
    )
}
