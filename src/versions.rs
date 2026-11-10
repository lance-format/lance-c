// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Version and tag C API.
//!
//! Version details, lightweight refs, tags, native refs and ref checkout are
//! copied snapshots or independently owned handles. Text accessors borrow from
//! the snapshot handle until it is closed.

use std::ffi::{CString, c_char};
use std::sync::{Arc, RwLock};

use chrono::{DateTime, Utc};
use lance::Dataset;
use lance::dataset::Version;
use lance::dataset::refs::{Ref, TagContents};
use lance_core::Result;
use lance_io::object_store::{ObjectStoreParams, StorageOptionsAccessor};

use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
use crate::helpers;
use crate::runtime::block_on;

/// One copied dataset-version snapshot.
pub struct LanceVersion {
    id: u64,
    timestamp: DateTime<Utc>,
    metadata: Vec<MetadataEntry>,
}

struct MetadataEntry {
    key: CString,
    value: CString,
}

impl LanceVersion {
    fn from_version(version: Version) -> Result<Self> {
        let Version {
            version,
            timestamp,
            metadata,
        } = version;
        Ok(Self {
            id: version,
            timestamp,
            metadata: metadata_entries(metadata, "dataset version")?,
        })
    }
}

/// Opaque snapshot of a dataset's version history.
pub struct LanceVersions {
    entries: Vec<LanceVersion>,
}

/// Opaque snapshot of the lightweight attached-version references.
pub struct LanceVersionRefs {
    entries: Vec<u64>,
}

/// Native `lance::dataset::refs::Ref` with C-owned strings.
pub struct LanceRef {
    reference: Ref,
}

/// One copied tag and its native contents.
pub struct LanceTag {
    name: CString,
    branch: Option<CString>,
    version: u64,
    created_at: Option<DateTime<Utc>>,
    updated_at: Option<DateTime<Utc>>,
    manifest_size: u64,
    metadata: Vec<MetadataEntry>,
}

impl LanceTag {
    fn from_contents(name: String, contents: TagContents) -> Result<Self> {
        let TagContents {
            branch,
            version,
            created_at,
            updated_at,
            manifest_size,
            metadata,
        } = contents;
        let branch = branch
            .map(|branch| c_string(branch, "tag branch"))
            .transpose()?;
        let mut metadata = metadata.into_iter().collect::<Vec<_>>();
        metadata.sort_by(|left, right| left.0.cmp(&right.0));
        Ok(Self {
            name: c_string(name, "tag name")?,
            branch,
            version,
            created_at,
            updated_at,
            manifest_size: manifest_size as u64,
            metadata: metadata_entries(metadata, "tag")?,
        })
    }
}

/// Snapshot of all tags in a dataset.
pub struct LanceTags {
    entries: Vec<LanceTag>,
}

fn invalid(message: impl Into<String>) -> lance_core::Error {
    lance_core::Error::invalid_input_source(message.into().into())
}

fn c_string(value: String, context: &str) -> Result<CString> {
    CString::new(value).map_err(|_| invalid(format!("{context} contains an embedded NUL byte")))
}

fn owned_json(json: serde_json::Result<String>, context: &str) -> Result<*const c_char> {
    let json = json.map_err(|error| {
        lance_core::Error::internal(format!("failed to serialize {context}: {error}"))
    })?;
    CString::new(json)
        .map(|json| json.into_raw() as *const c_char)
        .map_err(|error| {
            lance_core::Error::internal(format!(
                "{context} JSON contains an embedded NUL byte: {error}"
            ))
        })
}

fn timestamp_ns(timestamp: &DateTime<Utc>, context: &str) -> Result<i64> {
    timestamp.timestamp_nanos_opt().ok_or_else(|| {
        invalid(format!(
            "{context} timestamp {timestamp} is outside the i64 nanosecond range"
        ))
    })
}

fn metadata_entries<I>(metadata: I, context: &str) -> Result<Vec<MetadataEntry>>
where
    I: IntoIterator<Item = (String, String)>,
{
    metadata
        .into_iter()
        .map(|(key, value)| {
            Ok(MetadataEntry {
                key: c_string(key, &format!("{context} metadata key"))?,
                value: c_string(value, &format!("{context} metadata value"))?,
            })
        })
        .collect()
}

fn dataset_snapshot(dataset: *const LanceDataset) -> Result<Arc<Dataset>> {
    if dataset.is_null() {
        return Err(invalid("dataset must not be NULL"));
    }
    Ok(unsafe { &*dataset }.snapshot())
}

fn required_c_string<'a>(value: *const c_char, field: &str) -> Result<&'a str> {
    unsafe { helpers::parse_c_string(value)? }
        .ok_or_else(|| invalid(format!("{field} must not be NULL")))
}

unsafe fn version_at<'a>(version: *const LanceVersion) -> Result<&'a LanceVersion> {
    if version.is_null() {
        return Err(invalid("version must not be NULL"));
    }
    Ok(unsafe { &*version })
}

unsafe fn version_entry_at<'a>(
    versions: *const LanceVersions,
    index: usize,
) -> Result<&'a LanceVersion> {
    if versions.is_null() {
        return Err(invalid("versions must not be NULL"));
    }
    let versions = unsafe { &*versions };
    versions.entries.get(index).ok_or_else(|| {
        invalid(format!(
            "version index {index} out of range; count = {}",
            versions.entries.len()
        ))
    })
}

unsafe fn version_metadata_at<'a>(
    version: *const LanceVersion,
    index: usize,
) -> Result<&'a MetadataEntry> {
    let version = unsafe { version_at(version)? };
    version.metadata.get(index).ok_or_else(|| {
        invalid(format!(
            "version metadata index {index} out of range; count = {}",
            version.metadata.len()
        ))
    })
}

unsafe fn tag_at<'a>(tag: *const LanceTag) -> Result<&'a LanceTag> {
    if tag.is_null() {
        return Err(invalid("tag must not be NULL"));
    }
    Ok(unsafe { &*tag })
}

unsafe fn tags_entry_at<'a>(tags: *const LanceTags, index: usize) -> Result<&'a LanceTag> {
    if tags.is_null() {
        return Err(invalid("tags must not be NULL"));
    }
    let tags = unsafe { &*tags };
    tags.entries.get(index).ok_or_else(|| {
        invalid(format!(
            "tag index {index} out of range; count = {}",
            tags.entries.len()
        ))
    })
}

unsafe fn tag_metadata_at<'a>(tag: *const LanceTag, index: usize) -> Result<&'a MetadataEntry> {
    let tag = unsafe { tag_at(tag)? };
    tag.metadata.get(index).ok_or_else(|| {
        invalid(format!(
            "tag metadata index {index} out of range; count = {}",
            tag.metadata.len()
        ))
    })
}

unsafe fn reference_at<'a>(reference: *const LanceRef) -> Result<&'a Ref> {
    if reference.is_null() {
        return Err(invalid("reference must not be NULL"));
    }
    let reference = unsafe { &*reference };
    Ok(&reference.reference)
}

unsafe fn optional_object_store_params(
    storage_opts: *const *const c_char,
) -> Result<Option<ObjectStoreParams>> {
    if storage_opts.is_null() {
        return Ok(None);
    }
    let options = unsafe { helpers::parse_storage_options(storage_opts)? };
    Ok(Some(ObjectStoreParams {
        storage_options_accessor: Some(Arc::new(StorageOptionsAccessor::with_static_options(
            options,
        ))),
        ..ObjectStoreParams::default()
    }))
}

// ---------------------------------------------------------------------------
// Current version details
// ---------------------------------------------------------------------------

/// Return a copied snapshot of the current dataset version. Caller frees with
/// `lance_version_close`. The snapshot remains valid after the dataset closes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_current_version(
    dataset: *const LanceDataset,
) -> *mut LanceVersion {
    ffi_try!(unsafe { current_version_inner(dataset) }, null)
}

unsafe fn current_version_inner(dataset: *const LanceDataset) -> Result<*mut LanceVersion> {
    let dataset = dataset_snapshot(dataset)?;
    let version = LanceVersion::from_version(dataset.version())?;
    Ok(Box::into_raw(Box::new(version)))
}

/// Return the version id. Returns 0 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_version_id(version: *const LanceVersion) -> u64 {
    ffi_try!(
        unsafe { Ok::<_, lance_core::Error>(version_at(version)?.id) },
        0
    )
}

/// Return the commit timestamp as Unix epoch nanoseconds. Returns 0 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_version_timestamp_ns(version: *const LanceVersion) -> i64 {
    ffi_try!(
        unsafe { timestamp_ns(&version_at(version)?.timestamp, "dataset version") },
        0
    )
}

/// Return the number of version metadata entries. Returns 0 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_version_metadata_count(version: *const LanceVersion) -> u64 {
    ffi_try!(
        unsafe { Ok::<_, lance_core::Error>(version_at(version)?.metadata.len() as u64) },
        0
    )
}

/// Return the metadata key at `index`, borrowed until the version is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_version_metadata_key_at(
    version: *const LanceVersion,
    index: usize,
) -> *const c_char {
    ffi_try!(
        unsafe { Ok::<_, lance_core::Error>(version_metadata_at(version, index)?.key.as_ptr()) },
        std::ptr::null()
    )
}

/// Return the metadata value at `index`, borrowed until the version is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_version_metadata_value_at(
    version: *const LanceVersion,
    index: usize,
) -> *const c_char {
    ffi_try!(
        unsafe { Ok::<_, lance_core::Error>(version_metadata_at(version, index)?.value.as_ptr()) },
        std::ptr::null()
    )
}

/// Close and free a current-version snapshot. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_version_close(version: *mut LanceVersion) {
    if !version.is_null() {
        swallow_unwind("lance_version_close", || unsafe {
            drop(Box::from_raw(version));
        });
    }
}

// ---------------------------------------------------------------------------
// Version history
// ---------------------------------------------------------------------------

/// Return a copied snapshot of the dataset's version list. The caller frees
/// the returned handle with `lance_versions_close`; it remains valid after the
/// dataset closes. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_versions(
    dataset: *const LanceDataset,
) -> *mut LanceVersions {
    ffi_try!(unsafe { versions_inner(dataset) }, null)
}

unsafe fn versions_inner(dataset: *const LanceDataset) -> Result<*mut LanceVersions> {
    let dataset = dataset_snapshot(dataset)?;
    let entries = block_on(dataset.versions())?
        .into_iter()
        .map(LanceVersion::from_version)
        .collect::<Result<Vec<_>>>()?;
    Ok(Box::into_raw(Box::new(LanceVersions { entries })))
}

/// Return the number of versions. Returns 0 on error (NULL handle).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_versions_count(versions: *const LanceVersions) -> u64 {
    ffi_try!(
        {
            if versions.is_null() {
                return Err(invalid("versions must not be NULL"));
            }
            Ok(unsafe { &*versions }.entries.len() as u64)
        },
        0
    )
}

/// Return a borrowed version snapshot at `index`, or NULL on error. The
/// pointer remains valid until `lance_versions_close`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_versions_at(
    versions: *const LanceVersions,
    index: usize,
) -> *const LanceVersion {
    ffi_try!(
        {
            let version = unsafe { version_entry_at(versions, index)? };
            Ok(version as *const LanceVersion)
        },
        std::ptr::null()
    )
}

/// Return the monotonic version id at `index` (0 <= index < count).
/// Returns 0 and sets the thread-local error on NULL or out-of-range input.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_versions_id_at(versions: *const LanceVersions, index: usize) -> u64 {
    ffi_try!(
        unsafe { Ok::<_, lance_core::Error>(version_entry_at(versions, index)?.id) },
        0
    )
}

/// Return the Unix epoch millisecond timestamp at `index`.
/// Returns 0 and sets the thread-local error on NULL or out-of-range input.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_versions_timestamp_ms_at(
    versions: *const LanceVersions,
    index: usize,
) -> i64 {
    ffi_try!(
        unsafe {
            Ok::<_, lance_core::Error>(
                version_entry_at(versions, index)?
                    .timestamp
                    .timestamp_millis(),
            )
        },
        0
    )
}

/// Close and free a versions handle. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_versions_close(versions: *mut LanceVersions) {
    if !versions.is_null() {
        swallow_unwind("lance_versions_close", || unsafe {
            let _ = Box::from_raw(versions);
        });
    }
}

/// Return a copied snapshot of the lightweight attached-version refs.
/// Caller frees with `lance_version_refs_close`; it remains valid after the
/// dataset closes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_version_refs(
    dataset: *const LanceDataset,
) -> *mut LanceVersionRefs {
    ffi_try!(unsafe { version_refs_inner(dataset) }, null)
}

unsafe fn version_refs_inner(dataset: *const LanceDataset) -> Result<*mut LanceVersionRefs> {
    let dataset = dataset_snapshot(dataset)?;
    let entries = block_on(dataset.version_refs())?
        .into_iter()
        .map(|version| version.version)
        .collect();
    Ok(Box::into_raw(Box::new(LanceVersionRefs { entries })))
}

/// Return the number of lightweight version refs. Returns 0 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_version_refs_count(refs: *const LanceVersionRefs) -> u64 {
    ffi_try!(
        {
            if refs.is_null() {
                return Err(invalid("version refs must not be NULL"));
            }
            Ok(unsafe { &*refs }.entries.len() as u64)
        },
        0
    )
}

/// Return the version id at `index`. Returns 0 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_version_refs_id_at(
    refs: *const LanceVersionRefs,
    index: usize,
) -> u64 {
    ffi_try!(
        {
            if refs.is_null() {
                return Err(invalid("version refs must not be NULL"));
            }
            let refs = unsafe { &*refs };
            refs.entries.get(index).copied().ok_or_else(|| {
                invalid(format!(
                    "version ref index {index} out of range; count = {}",
                    refs.entries.len()
                ))
            })
        },
        0
    )
}

/// Close and free a lightweight version-ref snapshot. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_version_refs_close(refs: *mut LanceVersionRefs) {
    if !refs.is_null() {
        swallow_unwind("lance_version_refs_close", || unsafe {
            drop(Box::from_raw(refs));
        });
    }
}

// ---------------------------------------------------------------------------
// Native refs
// ---------------------------------------------------------------------------

/// Construct `Ref::VersionNumber(version)`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_ref_version_number(version: u64) -> *mut LanceRef {
    ffi_try!(
        Ok::<_, lance_core::Error>(Box::into_raw(Box::new(LanceRef {
            reference: Ref::from(version),
        }))),
        null
    )
}

/// Construct `Ref::Version`.
///
/// `branch == NULL` maps to `None`; `has_version == false` maps the version to
/// `None`. The native tuple conversion normalizes `"main"` to the default
/// branch, exactly as `checkout_version` and `Tags::{create,update}` do.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_ref_version(
    branch: *const c_char,
    has_version: bool,
    version: u64,
) -> *mut LanceRef {
    ffi_try!(
        unsafe { ref_version_inner(branch, has_version, version) },
        null
    )
}

unsafe fn ref_version_inner(
    branch: *const c_char,
    has_version: bool,
    version: u64,
) -> Result<*mut LanceRef> {
    let branch = unsafe { helpers::parse_c_string(branch)? };
    let version = has_version.then_some(version);
    Ok(Box::into_raw(Box::new(LanceRef {
        reference: Ref::from((branch, version)),
    })))
}

/// Construct `Ref::Tag(tag_name)`. Validation happens in the native operation
/// that consumes the ref.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_ref_tag(tag_name: *const c_char) -> *mut LanceRef {
    ffi_try!(
        {
            let tag_name = required_c_string(tag_name, "tag_name")?;
            Ok(Box::into_raw(Box::new(LanceRef {
                reference: Ref::from(tag_name),
            })))
        },
        null
    )
}

/// Close and free a native ref. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_ref_close(reference: *mut LanceRef) {
    if !reference.is_null() {
        swallow_unwind("lance_ref_close", || unsafe {
            drop(Box::from_raw(reference));
        });
    }
}

/// Check out a native ref as a new dataset handle. The source dataset and ref
/// are unchanged. Caller closes the returned handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_checkout_ref(
    dataset: *const LanceDataset,
    reference: *const LanceRef,
) -> *mut LanceDataset {
    ffi_try!(unsafe { checkout_ref_inner(dataset, reference) }, null)
}

unsafe fn checkout_ref_inner(
    dataset: *const LanceDataset,
    reference: *const LanceRef,
) -> Result<*mut LanceDataset> {
    let dataset = dataset_snapshot(dataset)?;
    let reference = unsafe { reference_at(reference)? }.clone();
    let checked_out = block_on(dataset.checkout_version(reference))?;
    Ok(Box::into_raw(Box::new(LanceDataset {
        inner: RwLock::new(Arc::new(checked_out)),
    })))
}

// ---------------------------------------------------------------------------
// Native branch lifecycle
// ---------------------------------------------------------------------------

/// Create a branch from a native ref.
///
/// Native `Dataset::create_branch` performs a shallow clone followed by a
/// `BranchContents` write. Those phases are not atomic. `storage_opts == NULL`
/// passes `None` so native falls back to the source dataset's store params;
/// a non-NULL array, including an immediately NULL-terminated empty array,
/// passes explicit `ObjectStoreParams`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_branch_create(
    dataset: *const LanceDataset,
    branch_name: *const c_char,
    reference: *const LanceRef,
    storage_opts: *const *const c_char,
) -> *mut LanceDataset {
    ffi_try!(
        unsafe { branch_create_inner(dataset, branch_name, reference, storage_opts) },
        null
    )
}

unsafe fn branch_create_inner(
    dataset: *const LanceDataset,
    branch_name: *const c_char,
    reference: *const LanceRef,
    storage_opts: *const *const c_char,
) -> Result<*mut LanceDataset> {
    if dataset.is_null() {
        return Err(invalid("dataset must not be NULL"));
    }
    let branch_name = required_c_string(branch_name, "branch_name")?;
    let reference = unsafe { reference_at(reference)? }.clone();
    let store_params = unsafe { optional_object_store_params(storage_opts)? };
    let dataset = unsafe { &*dataset };
    let created = dataset.with_mut(|source| {
        block_on(source.create_branch(branch_name, reference, store_params))
    })?;
    Ok(Box::into_raw(Box::new(LanceDataset {
        inner: RwLock::new(Arc::new(created)),
    })))
}

/// Check out the latest version of a branch as an independently owned dataset.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_checkout_branch(
    dataset: *const LanceDataset,
    branch_name: *const c_char,
) -> *mut LanceDataset {
    ffi_try!(unsafe { checkout_branch_inner(dataset, branch_name) }, null)
}

unsafe fn checkout_branch_inner(
    dataset: *const LanceDataset,
    branch_name: *const c_char,
) -> Result<*mut LanceDataset> {
    let branch_name = required_c_string(branch_name, "branch_name")?;
    let dataset = dataset_snapshot(dataset)?;
    let checked_out = block_on(dataset.checkout_branch(branch_name))?;
    Ok(Box::into_raw(Box::new(LanceDataset {
        inner: RwLock::new(Arc::new(checked_out)),
    })))
}

/// Serialize the native branch map as JSON.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_branches_json(
    dataset: *const LanceDataset,
) -> *const c_char {
    ffi_try!(unsafe { branches_json_inner(dataset) }, null)
}

unsafe fn branches_json_inner(dataset: *const LanceDataset) -> Result<*const c_char> {
    let dataset = dataset_snapshot(dataset)?;
    let branches = block_on(dataset.list_branches())?;
    owned_json(serde_json::to_string(&branches), "branch list")
}

/// Serialize one native `BranchContents` as JSON.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_branch_get_json(
    dataset: *const LanceDataset,
    branch_name: *const c_char,
) -> *const c_char {
    ffi_try!(unsafe { branch_get_json_inner(dataset, branch_name) }, null)
}

unsafe fn branch_get_json_inner(
    dataset: *const LanceDataset,
    branch_name: *const c_char,
) -> Result<*const c_char> {
    let branch_name = required_c_string(branch_name, "branch_name")?;
    let dataset = dataset_snapshot(dataset)?;
    let contents = block_on(dataset.branches().get(branch_name))?;
    owned_json(
        serde_json::to_string(&contents),
        &format!("branch {branch_name}"),
    )
}

/// Serialize the current dataset's native `BranchIdentifier` as JSON.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_branch_identifier_json(
    dataset: *const LanceDataset,
) -> *const c_char {
    ffi_try!(unsafe { branch_identifier_json_inner(dataset) }, null)
}

unsafe fn branch_identifier_json_inner(dataset: *const LanceDataset) -> Result<*const c_char> {
    let dataset = dataset_snapshot(dataset)?;
    let identifier = block_on(dataset.branch_identifier())?;
    owned_json(
        serde_json::to_string(&identifier),
        "current branch identifier",
    )
}

/// Delete a branch using native reference-protection semantics.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_branch_delete(
    dataset: *const LanceDataset,
    branch_name: *const c_char,
) -> i32 {
    ffi_try!(unsafe { branch_delete_inner(dataset, branch_name, false) }, neg)
}

/// Delete a branch, bypassing only a missing `BranchContents`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_branch_force_delete(
    dataset: *const LanceDataset,
    branch_name: *const c_char,
) -> i32 {
    ffi_try!(unsafe { branch_delete_inner(dataset, branch_name, true) }, neg)
}

unsafe fn branch_delete_inner(
    dataset: *const LanceDataset,
    branch_name: *const c_char,
    force: bool,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(invalid("dataset must not be NULL"));
    }
    let branch_name = required_c_string(branch_name, "branch_name")?;
    let dataset = unsafe { &*dataset };
    dataset.with_mut(|source| {
        if force {
            block_on(source.force_delete_branch(branch_name))
        } else {
            block_on(source.delete_branch(branch_name))
        }
    })?;
    Ok(0)
}

/// Replace all metadata entries on an existing branch.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_branch_replace_metadata(
    dataset: *const LanceDataset,
    branch_name: *const c_char,
    metadata: *const *const c_char,
) -> i32 {
    ffi_try!(
        unsafe { branch_replace_metadata_inner(dataset, branch_name, metadata) },
        neg
    )
}

unsafe fn branch_replace_metadata_inner(
    dataset: *const LanceDataset,
    branch_name: *const c_char,
    metadata: *const *const c_char,
) -> Result<i32> {
    let branch_name = required_c_string(branch_name, "branch_name")?;
    let metadata = unsafe { helpers::parse_c_string_map(metadata, "branch metadata")? };
    let dataset = dataset_snapshot(dataset)?;
    block_on(dataset.branches().replace_metadata(branch_name, metadata))?;
    Ok(0)
}

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

/// Return a copied snapshot of all tags. Caller frees with `lance_tags_close`;
/// it remains valid after the dataset closes. Entry order is unspecified.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_tags(dataset: *const LanceDataset) -> *mut LanceTags {
    ffi_try!(unsafe { tags_inner(dataset) }, null)
}

unsafe fn tags_inner(dataset: *const LanceDataset) -> Result<*mut LanceTags> {
    let dataset = dataset_snapshot(dataset)?;
    let tags = block_on(dataset.tags().list())?;
    let entries = tags
        .into_iter()
        .map(|(name, contents)| LanceTag::from_contents(name, contents))
        .collect::<Result<Vec<_>>>()?;
    Ok(Box::into_raw(Box::new(LanceTags { entries })))
}

/// Return the number of tags. Returns 0 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_tags_count(tags: *const LanceTags) -> u64 {
    ffi_try!(
        {
            if tags.is_null() {
                return Err(invalid("tags must not be NULL"));
            }
            Ok(unsafe { &*tags }.entries.len() as u64)
        },
        0
    )
}

/// Return a borrowed tag at `index`, or NULL on error. The pointer remains
/// valid until `lance_tags_close`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_tags_at(tags: *const LanceTags, index: usize) -> *const LanceTag {
    ffi_try!(
        {
            let tag = unsafe { tags_entry_at(tags, index)? };
            Ok(tag as *const LanceTag)
        },
        std::ptr::null()
    )
}

/// Close and free a tag-list snapshot. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_tags_close(tags: *mut LanceTags) {
    if !tags.is_null() {
        swallow_unwind("lance_tags_close", || unsafe {
            drop(Box::from_raw(tags));
        });
    }
}

/// Fetch one tag by name. Caller frees the returned handle with
/// `lance_tag_close`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_tag_get(
    dataset: *const LanceDataset,
    tag_name: *const c_char,
) -> *mut LanceTag {
    ffi_try!(unsafe { tag_get_inner(dataset, tag_name) }, null)
}

unsafe fn tag_get_inner(
    dataset: *const LanceDataset,
    tag_name: *const c_char,
) -> Result<*mut LanceTag> {
    let tag_name = required_c_string(tag_name, "tag_name")?;
    let dataset = dataset_snapshot(dataset)?;
    let tags = dataset.tags();
    let contents = block_on(tags.get(tag_name))?;
    let tag = LanceTag::from_contents(tag_name.to_string(), contents)?;
    Ok(Box::into_raw(Box::new(tag)))
}

/// Create a tag from a native ref. The native `Tags::create` behavior defines
/// target resolution, validation and duplicate conflicts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_tag_create(
    dataset: *const LanceDataset,
    tag_name: *const c_char,
    reference: *const LanceRef,
) -> i32 {
    ffi_try!(
        unsafe { tag_create_inner(dataset, tag_name, reference) },
        neg
    )
}

unsafe fn tag_create_inner(
    dataset: *const LanceDataset,
    tag_name: *const c_char,
    reference: *const LanceRef,
) -> Result<i32> {
    let tag_name = required_c_string(tag_name, "tag_name")?;
    let reference = unsafe { reference_at(reference)? }.clone();
    let dataset = dataset_snapshot(dataset)?;
    let tags = dataset.tags();
    block_on(tags.create(tag_name, reference))?;
    Ok(0)
}

/// Update an existing tag to a native ref. Metadata is preserved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_tag_update(
    dataset: *const LanceDataset,
    tag_name: *const c_char,
    reference: *const LanceRef,
) -> i32 {
    ffi_try!(
        unsafe { tag_update_inner(dataset, tag_name, reference) },
        neg
    )
}

unsafe fn tag_update_inner(
    dataset: *const LanceDataset,
    tag_name: *const c_char,
    reference: *const LanceRef,
) -> Result<i32> {
    let tag_name = required_c_string(tag_name, "tag_name")?;
    let reference = unsafe { reference_at(reference)? }.clone();
    let dataset = dataset_snapshot(dataset)?;
    let tags = dataset.tags();
    block_on(tags.update(tag_name, reference))?;
    Ok(0)
}

/// Delete a tag. An unknown tag is an error from the native implementation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_tag_delete(
    dataset: *const LanceDataset,
    tag_name: *const c_char,
) -> i32 {
    ffi_try!(unsafe { tag_delete_inner(dataset, tag_name) }, neg)
}

unsafe fn tag_delete_inner(dataset: *const LanceDataset, tag_name: *const c_char) -> Result<i32> {
    let tag_name = required_c_string(tag_name, "tag_name")?;
    let dataset = dataset_snapshot(dataset)?;
    let tags = dataset.tags();
    block_on(tags.delete(tag_name))?;
    Ok(0)
}

/// Replace all metadata entries on an existing tag. A NULL array means an empty
/// map, matching native `Tags::replace_metadata`. The native implementation
/// preserves all other tag fields.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_tag_replace_metadata(
    dataset: *const LanceDataset,
    tag_name: *const c_char,
    metadata: *const *const c_char,
) -> i32 {
    ffi_try!(
        unsafe { tag_replace_metadata_inner(dataset, tag_name, metadata) },
        neg
    )
}

unsafe fn tag_replace_metadata_inner(
    dataset: *const LanceDataset,
    tag_name: *const c_char,
    metadata: *const *const c_char,
) -> Result<i32> {
    let tag_name = required_c_string(tag_name, "tag_name")?;
    let metadata = unsafe { helpers::parse_c_string_map(metadata, "tag metadata")? };
    let dataset = dataset_snapshot(dataset)?;
    let tags = dataset.tags();
    block_on(tags.replace_metadata(tag_name, metadata))?;
    Ok(0)
}

/// Return the tag name, borrowed until the tag handle is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_tag_name(tag: *const LanceTag) -> *const c_char {
    ffi_try!(
        unsafe { Ok::<_, lance_core::Error>(tag_at(tag)?.name.as_ptr()) },
        std::ptr::null()
    )
}

/// Return the tagged version id. Returns 0 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_tag_version(tag: *const LanceTag) -> u64 {
    ffi_try!(
        unsafe { Ok::<_, lance_core::Error>(tag_at(tag)?.version) },
        0
    )
}

/// Report the optional branch.
///
/// Returns 1 and writes the borrowed branch to `out_branch` when present,
/// 0 and writes NULL when absent, or -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_tag_branch(
    tag: *const LanceTag,
    out_branch: *mut *const c_char,
) -> i32 {
    ffi_try!(unsafe { tag_branch_inner(tag, out_branch) }, neg)
}

unsafe fn tag_branch_inner(tag: *const LanceTag, out_branch: *mut *const c_char) -> Result<i32> {
    if out_branch.is_null() {
        return Err(invalid("out_branch must not be NULL"));
    }
    let tag = unsafe { tag_at(tag)? };
    if let Some(branch) = &tag.branch {
        unsafe { std::ptr::write(out_branch, branch.as_ptr()) };
        Ok(1)
    } else {
        unsafe { std::ptr::write(out_branch, std::ptr::null()) };
        Ok(0)
    }
}

/// Report the optional creation timestamp as Unix epoch nanoseconds.
/// Returns 1 when present, 0 when absent, or -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_tag_created_at_ns(
    tag: *const LanceTag,
    out_timestamp_ns: *mut i64,
) -> i32 {
    ffi_try!(
        unsafe {
            tag_timestamp_inner(tag, out_timestamp_ns, "tag created_at", |tag| {
                tag.created_at
            })
        },
        neg
    )
}

/// Report the optional update timestamp as Unix epoch nanoseconds.
/// Returns 1 when present, 0 when absent, or -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_tag_updated_at_ns(
    tag: *const LanceTag,
    out_timestamp_ns: *mut i64,
) -> i32 {
    ffi_try!(
        unsafe {
            tag_timestamp_inner(tag, out_timestamp_ns, "tag updated_at", |tag| {
                tag.updated_at
            })
        },
        neg
    )
}

unsafe fn tag_timestamp_inner(
    tag: *const LanceTag,
    out_timestamp_ns: *mut i64,
    context: &str,
    timestamp: impl FnOnce(&LanceTag) -> Option<DateTime<Utc>>,
) -> Result<i32> {
    if out_timestamp_ns.is_null() {
        return Err(invalid("out_timestamp_ns must not be NULL"));
    }
    let tag = unsafe { tag_at(tag)? };
    match timestamp(tag) {
        Some(value) => {
            let value = timestamp_ns(&value, context)?;
            unsafe { std::ptr::write(out_timestamp_ns, value) };
            Ok(1)
        }
        None => Ok(0),
    }
}

/// Return the manifest size recorded in the tag. Returns 0 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_tag_manifest_size(tag: *const LanceTag) -> u64 {
    ffi_try!(
        unsafe { Ok::<_, lance_core::Error>(tag_at(tag)?.manifest_size) },
        0
    )
}

/// Return the number of tag metadata entries. Returns 0 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_tag_metadata_count(tag: *const LanceTag) -> u64 {
    ffi_try!(
        unsafe { Ok::<_, lance_core::Error>(tag_at(tag)?.metadata.len() as u64) },
        0
    )
}

/// Return the metadata key at `index`, borrowed until the tag is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_tag_metadata_key_at(
    tag: *const LanceTag,
    index: usize,
) -> *const c_char {
    ffi_try!(
        unsafe { Ok::<_, lance_core::Error>(tag_metadata_at(tag, index)?.key.as_ptr()) },
        std::ptr::null()
    )
}

/// Return the metadata value at `index`, borrowed until the tag is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_tag_metadata_value_at(
    tag: *const LanceTag,
    index: usize,
) -> *const c_char {
    ffi_try!(
        unsafe { Ok::<_, lance_core::Error>(tag_metadata_at(tag, index)?.value.as_ptr()) },
        std::ptr::null()
    )
}

/// Close and free one tag handle. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_tag_close(tag: *mut LanceTag) {
    if !tag.is_null() {
        swallow_unwind("lance_tag_close", || unsafe {
            drop(Box::from_raw(tag));
        });
    }
}
