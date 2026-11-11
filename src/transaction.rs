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
use lance::dataset::transaction::{Operation, Transaction, TransactionBuilder};
use lance_core::Result;
use lance_table::format::{BasePath, Fragment, IndexMetadata, pb};
use lance_table::transaction::DataReplacementGroup;
use prost::Message;

use crate::base_paths::{
    LanceBasePath, LanceBasePathView, OwnedBasePath, own_base_paths, parse_base_paths,
};
use crate::data_file::LanceDataFile;
use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
use crate::fragment::{LanceFragment, fragment_metadata_json};
use crate::helpers;
use crate::index_segment::{LanceIndexSegmentMetadata, metadata_handle};
use crate::runtime::block_on;

/// Owned native transaction value.
pub struct LanceTransaction {
    pub(crate) inner: Transaction,
}

/// Owned native operation value cloned from a transaction.
pub struct LanceOperation {
    pub(crate) inner: Operation,
    /// Owned C-string storage backing borrowed `UpdateBases` views.
    base_paths: Vec<OwnedBasePath>,
}

impl LanceOperation {
    /// Wrap a native operation, retaining C-string storage for borrowed views.
    pub(crate) fn from_native(inner: Operation) -> Result<Self> {
        let base_paths = match &inner {
            Operation::UpdateBases { new_bases } => own_base_paths(new_bases)?,
            _ => Vec::new(),
        };
        Ok(Self { inner, base_paths })
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
    )?)))
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
    )?)))
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
    )?)))
}

/// Clone a borrowed array of fragment handles into complete native metadata.
unsafe fn clone_fragment_array(
    fragments: *const *const LanceFragment,
    fragment_count: usize,
) -> Result<Vec<Fragment>> {
    if fragments.is_null() {
        if fragment_count == 0 {
            return Ok(Vec::new());
        }
        return Err(lance_core::Error::invalid_input_source(
            format!("fragments must not be NULL when fragment_count = {fragment_count}").into(),
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
                format!(
                    "fragments[{index}] must not be NULL when fragment_count = {fragment_count}"
                )
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
    let fragments = unsafe { clone_fragment_array(fragments, fragment_count) }?;
    Ok(Box::into_raw(Box::new(LanceOperation::from_native(
        Operation::Append { fragments },
    )?)))
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
            )?)))
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
            )?)))
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
    )?)))
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
    )?);
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
    let view = operation.base_paths[index].view();
    unsafe {
        ptr::write(out_base, view);
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
