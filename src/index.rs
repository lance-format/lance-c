// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Index lifecycle C API: create vector / scalar indexes, drop, list.
//!
//! Index creation mutates the dataset under an exclusive write lock; existing
//! scanners that already cloned the inner Arc keep their snapshot view.

use std::collections::{HashMap, HashSet};
use std::ffi::{CStr, CString, c_char};
use std::future::Future;
use std::slice;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;

use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema, to_ffi};
use arrow::ffi_stream::FFI_ArrowArrayStream;
use arrow_array::{Array, ArrayRef, FixedSizeListArray};
use arrow_schema::Field;
use chrono::SecondsFormat;
use lance::index::vector::IndexFileVersion;
use lance::index::{DatasetIndexExt, DatasetIndexInternalExt};
use lance_core::Result;
use lance_index::metrics::NoOpMetricsCollector;
use lance_index::scalar::{BuiltinIndexType, ScalarIndexParams};
use lance_index::vector::bq::RQBuildParams;
use lance_index::vector::bq::storage::RabitQuantizationMetadata;
use lance_index::vector::ivf::storage::IvfModel as NativeIvfModel;
use lance_index::{FtsPrewarmOptions, IndexType, PrewarmOptions};
use lance_table::format::IndexMetadata;
use object_store::path::Path;
use prost::Message;
use serde_json::json;
use uuid::Uuid;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::index_segment::{borrow_model_array, fixed_size_list_model};
use crate::runtime::{RT, block_on};
use crate::stream_guard::GuardedReader;

/// Scalar index type, matching the C enum `LanceScalarIndexType`.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceScalarIndexType {
    BTree = 1,
    Bitmap = 2,
    LabelList = 3,
    Inverted = 4,
    NGram = 5,
    ZoneMap = 6,
    BloomFilter = 7,
    MinHashLsh = 8,
}

impl LanceScalarIndexType {
    pub(crate) fn from_c(v: i32) -> Result<Self> {
        match v {
            1 => Ok(Self::BTree),
            2 => Ok(Self::Bitmap),
            3 => Ok(Self::LabelList),
            4 => Ok(Self::Inverted),
            5 => Ok(Self::NGram),
            6 => Ok(Self::ZoneMap),
            7 => Ok(Self::BloomFilter),
            8 => Ok(Self::MinHashLsh),
            _ => Err(lance_core::Error::invalid_input_source(
                format!("invalid scalar index type: {}", v).into(),
            )),
        }
    }

    pub(crate) fn from_details_type_url(type_url: &str) -> Option<Self> {
        let details_name = type_url.rsplit('.').next().unwrap_or(type_url);
        match details_name {
            "BTreeIndexDetails" => Some(Self::BTree),
            "BitmapIndexDetails" => Some(Self::Bitmap),
            "LabelListIndexDetails" => Some(Self::LabelList),
            "InvertedIndexDetails" => Some(Self::Inverted),
            "NGramIndexDetails" => Some(Self::NGram),
            "ZoneMapIndexDetails" => Some(Self::ZoneMap),
            "BloomFilterIndexDetails" => Some(Self::BloomFilter),
            "MinHashLshIndexDetails" => Some(Self::MinHashLsh),
            _ => None,
        }
    }

    pub(crate) fn to_builtin(self) -> BuiltinIndexType {
        match self {
            Self::BTree => BuiltinIndexType::BTree,
            Self::Bitmap => BuiltinIndexType::Bitmap,
            Self::LabelList => BuiltinIndexType::LabelList,
            Self::Inverted => BuiltinIndexType::Inverted,
            Self::NGram => BuiltinIndexType::NGram,
            Self::ZoneMap => BuiltinIndexType::ZoneMap,
            Self::BloomFilter => BuiltinIndexType::BloomFilter,
            Self::MinHashLsh => BuiltinIndexType::MinHashLsh,
        }
    }

    pub(crate) fn to_index_type(self) -> IndexType {
        match self {
            Self::BTree => IndexType::BTree,
            Self::Bitmap => IndexType::Bitmap,
            Self::LabelList => IndexType::LabelList,
            Self::Inverted => IndexType::Inverted,
            Self::NGram => IndexType::NGram,
            Self::ZoneMap => IndexType::ZoneMap,
            Self::BloomFilter => IndexType::BloomFilter,
            Self::MinHashLsh => IndexType::MinHashLsh,
        }
    }
}

/// Additive scalar-index controls. `options == NULL` or
/// `has_train == false` preserves the native default (`train=true`).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceScalarIndexOptions {
    pub has_train: bool,
    pub train: bool,
}

pub(crate) fn scalar_train(options: *const LanceScalarIndexOptions) -> Result<bool> {
    let options = unsafe { options.as_ref() };
    Ok(options
        .filter(|options| options.has_train)
        .map(|options| options.train)
        .unwrap_or(true))
}

/// Create a scalar index on a column.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_create_scalar_index(
    dataset: *mut LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    index_type: i32,
    params_json: *const c_char,
    replace: bool,
) -> i32 {
    ffi_try!(
        unsafe {
            create_scalar_index_inner(
                dataset,
                column,
                index_name,
                index_type,
                params_json,
                replace,
            )
        },
        neg
    )
}

unsafe fn create_scalar_index_inner(
    dataset: *mut LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    index_type: i32,
    params_json: *const c_char,
    replace: bool,
) -> Result<i32> {
    if dataset.is_null() || column.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset and column must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let column_str = unsafe { helpers::parse_c_string(column)? }.ok_or_else(|| {
        lance_core::Error::invalid_input_source("column must not be empty".into())
    })?;
    let name = unsafe { helpers::parse_c_string(index_name)? }.map(|s| s.to_string());
    let params_str = unsafe { helpers::parse_c_string(params_json)? };

    let scalar_type = LanceScalarIndexType::from_c(index_type)?;

    let mut params = ScalarIndexParams::for_builtin(scalar_type.to_builtin());
    if let Some(json) = params_str {
        params.params = Some(json.to_string());
    }

    ds.with_mut(|d| {
        block_on(d.create_index(
            &[column_str],
            scalar_type.to_index_type(),
            name,
            &params,
            replace,
        ))
    })?;

    Ok(0)
}

/// Create a scalar index with additive native build controls.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_create_scalar_index_with_options(
    dataset: *mut LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    index_type: i32,
    params_json: *const c_char,
    options: *const LanceScalarIndexOptions,
    replace: bool,
) -> i32 {
    ffi_try!(
        unsafe {
            create_scalar_index_with_options_inner(
                dataset,
                column,
                index_name,
                index_type,
                params_json,
                options,
                replace,
            )
        },
        neg
    )
}

#[allow(clippy::too_many_arguments)]
unsafe fn create_scalar_index_with_options_inner(
    dataset: *mut LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    index_type: i32,
    params_json: *const c_char,
    options: *const LanceScalarIndexOptions,
    replace: bool,
) -> Result<i32> {
    if dataset.is_null() || column.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset and column must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let column = unsafe { helpers::parse_c_string(column)? }
        .filter(|value| !value.is_empty())
        .ok_or_else(|| {
            lance_core::Error::invalid_input_source("column must not be NULL or empty".into())
        })?
        .to_string();
    let index_name = unsafe { helpers::parse_c_string(index_name)? }.map(str::to_string);
    let params_json = unsafe { helpers::parse_c_string(params_json)? };
    let scalar_type = LanceScalarIndexType::from_c(index_type)?;
    let train = scalar_train(options)?;

    let mut params = ScalarIndexParams::for_builtin(scalar_type.to_builtin());
    params.params = params_json.map(str::to_string);

    ds.with_mut(|dataset| {
        let columns = [column.as_str()];
        let mut builder =
            dataset.create_index_builder(&columns, scalar_type.to_index_type(), &params);
        if let Some(index_name) = index_name.clone() {
            builder = builder.name(index_name);
        }
        builder = builder.replace(replace).train(train);
        block_on(std::future::IntoFuture::into_future(builder))
    })?;
    Ok(0)
}

/// Number of user indexes (excludes system indexes).
/// Returns 0 on error — check `lance_last_error_code()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_index_count(dataset: *const LanceDataset) -> u64 {
    ffi_try!(unsafe { index_count_inner(dataset) }, 0)
}

unsafe fn index_count_inner(dataset: *const LanceDataset) -> Result<u64> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset is NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let snap = ds.snapshot();
    let indices = block_on(snap.load_indices())?;
    Ok(indices
        .iter()
        .filter(|i| !lance_index::is_system_index(i))
        .count() as u64)
}

/// Count the segments that make up a logical index.
///
/// A logical index is a set of physical segments (one per distributed-build worker
/// or one per fragment range). Each segment has a stable UUID. Returns 0 if the
/// index does not exist (also sets `LANCE_ERR_NOT_FOUND`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_index_segment_count(
    dataset: *const LanceDataset,
    index_name: *const c_char,
) -> u64 {
    ffi_try!(unsafe { index_segment_count_inner(dataset, index_name) }, 0)
}

unsafe fn index_segment_count_inner(
    dataset: *const LanceDataset,
    index_name: *const c_char,
) -> Result<u64> {
    if dataset.is_null() || index_name.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset and index_name must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let name = unsafe { helpers::parse_c_string(index_name)? }.ok_or_else(|| {
        lance_core::Error::invalid_input_source("index_name must not be empty".into())
    })?;
    let snap = ds.snapshot();
    let indices = block_on(snap.load_indices())?;
    let count = indices
        .iter()
        .filter(|i| !lance_index::is_system_index(i) && i.name == name)
        .count();
    if count == 0 {
        return Err(lance_core::Error::index_not_found(format!(
            "name='{}'",
            name
        )));
    }
    Ok(count as u64)
}

/// Fill `out_uuids` with the UUIDs of the segments that make up a logical index.
///
/// Each UUID is written as 16 raw bytes (RFC 4122 layout).
///
/// - `capacity`: number of UUIDs the caller allocated space for in `out_uuids`
///   (byte length must be at least `capacity * 16`).
/// - `out_count`: if non-NULL, receives the number of UUIDs actually written.
///
/// Returns 0 on success, -1 on error. If the index has more segments than
/// `capacity`, returns `LANCE_ERR_INVALID_ARGUMENT` without writing anything.
/// Callers can retry with a larger buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_index_segments(
    dataset: *const LanceDataset,
    index_name: *const c_char,
    out_uuids: *mut u8,
    capacity: usize,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe {
            dataset_index_segments_inner(dataset, index_name, out_uuids, capacity, out_count)
        },
        neg
    )
}

unsafe fn dataset_index_segments_inner(
    dataset: *const LanceDataset,
    index_name: *const c_char,
    out_uuids: *mut u8,
    capacity: usize,
    out_count: *mut u64,
) -> Result<i32> {
    if dataset.is_null() || index_name.is_null() || out_uuids.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset, index_name, and out_uuids must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let name = unsafe { helpers::parse_c_string(index_name)? }.ok_or_else(|| {
        lance_core::Error::invalid_input_source("index_name must not be empty".into())
    })?;
    let snap = ds.snapshot();
    let indices = block_on(snap.load_indices())?;
    let segments: Vec<_> = indices
        .iter()
        .filter(|i| !lance_index::is_system_index(i) && i.name == name)
        .collect();
    if segments.is_empty() {
        return Err(lance_core::Error::index_not_found(format!(
            "name='{}'",
            name
        )));
    }
    if segments.len() > capacity {
        return Err(lance_core::Error::invalid_input_source(
            format!(
                "out_uuids capacity ({}) too small for {} segments",
                capacity,
                segments.len()
            )
            .into(),
        ));
    }
    // SAFETY: caller guarantees out_uuids has at least `capacity * 16` bytes,
    // and we verified `segments.len() <= capacity` above.
    for (i, seg) in segments.iter().enumerate() {
        let bytes = seg.uuid.as_bytes();
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), out_uuids.add(i * 16), 16);
        }
    }
    if !out_count.is_null() {
        unsafe { *out_count = segments.len() as u64 };
    }
    Ok(0)
}

/// Optional controls for prewarming a logical index.
///
/// When `has_segment_selection` is false, `segment_uuids` and
/// `segment_count` are ignored. When true, a count of zero represents
/// `Some(empty)`, which performs no segment work.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceIndexPrewarmOptions {
    pub with_position: bool,
    pub has_segment_selection: bool,
    pub segment_uuids: *const u8,
    pub segment_count: usize,
}

/// Synchronously prewarm all segments of a logical index in this snapshot.
///
/// Uses the dataset's session index cache. Success does not pin cache entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_prewarm_index(
    dataset: *const LanceDataset,
    index_name: *const c_char,
) -> i32 {
    ffi_try!(unsafe { prewarm_index_inner(dataset, index_name) }, neg)
}

unsafe fn prewarm_index_inner(
    dataset: *const LanceDataset,
    index_name: *const c_char,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    let index_name = unsafe { helpers::parse_c_string(index_name)? }
        .filter(|name| !name.is_empty())
        .ok_or_else(|| {
            lance_core::Error::invalid_input_source("index_name must not be NULL or empty".into())
        })?;
    // Keep one snapshot and its shared session alive without holding the
    // dataset lock across potentially long-running index IO.
    let snapshot = unsafe { &*dataset }.snapshot();
    block_on(prewarm_index_by_name(&snapshot, index_name))?;
    Ok(0)
}

/// Synchronously prewarm a logical index with native position and physical
/// segment controls.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_prewarm_index_with_options(
    dataset: *const LanceDataset,
    index_name: *const c_char,
    options: *const LanceIndexPrewarmOptions,
) -> i32 {
    ffi_try!(
        unsafe { prewarm_index_with_options_inner(dataset, index_name, options) },
        neg
    )
}

unsafe fn prewarm_index_with_options_inner(
    dataset: *const LanceDataset,
    index_name: *const c_char,
    options: *const LanceIndexPrewarmOptions,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    let index_name = unsafe { helpers::parse_c_string(index_name)? }
        .filter(|name| !name.is_empty())
        .ok_or_else(|| {
            lance_core::Error::invalid_input_source("index_name must not be NULL or empty".into())
        })?;
    let (with_position, segment_selection) = unsafe { parse_prewarm_options(options)? };
    let snapshot = unsafe { &*dataset }.snapshot();
    block_on(prewarm_index_by_name_with_options(
        &snapshot,
        index_name,
        segment_selection.as_deref(),
        with_position,
    ))?;
    Ok(0)
}

unsafe fn parse_prewarm_options(
    options: *const LanceIndexPrewarmOptions,
) -> Result<(bool, Option<Vec<Uuid>>)> {
    let Some(options) = (unsafe { options.as_ref() }) else {
        return Ok((false, None));
    };
    if !options.has_segment_selection {
        return Ok((options.with_position, None));
    }
    if options.segment_count == 0 {
        return Ok((options.with_position, Some(Vec::new())));
    }
    let byte_len = options.segment_count.checked_mul(16).ok_or_else(|| {
        lance_core::Error::invalid_input(format!(
            "segment_count {} * 16 overflows usize",
            options.segment_count
        ))
    })?;
    if byte_len > isize::MAX as usize {
        return Err(lance_core::Error::invalid_input(format!(
            "segment_count {} * 16 exceeds the maximum addressable byte slice length",
            options.segment_count
        )));
    }
    if options.segment_uuids.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "segment_uuids must not be NULL when segment_count is {}",
            options.segment_count
        )));
    }
    let bytes = unsafe { slice::from_raw_parts(options.segment_uuids, byte_len) };
    let segments = bytes
        .as_chunks::<16>()
        .0
        .iter()
        .map(|uuid| Uuid::from_bytes(*uuid))
        .collect();
    Ok((options.with_position, Some(segments)))
}

async fn prewarm_index_by_name(dataset: &lance::Dataset, index_name: &str) -> Result<()> {
    prewarm_index_by_name_with_options(dataset, index_name, None, false).await
}

async fn prewarm_index_by_name_with_options(
    dataset: &lance::Dataset,
    index_name: &str,
    segment_selection: Option<&[Uuid]>,
    with_position: bool,
) -> Result<()> {
    let Some(named_field) = dataset.schema().field(index_name) else {
        return prewarm_named_index(dataset, index_name, segment_selection, with_position).await;
    };
    let indices = dataset.load_indices_by_name(index_name).await?;
    if indices.is_empty() {
        return prewarm_named_index(dataset, index_name, segment_selection, with_position).await;
    }
    let indices = select_prewarm_segments(index_name, indices, segment_selection)?;
    if indices.is_empty() {
        return Ok(());
    }

    let sdk_segments =
        prewarm_colliding_segments(indices, named_field.id, |field_id, uuid| async move {
            // The pinned SDK interprets an index name as a schema column first.
            // Open colliding vector segments by their indexed field, even on a cold cache.
            let field_path = dataset.schema().field_path(field_id)?;
            let index = dataset
                .open_generic_index(&field_path, &uuid, &NoOpMetricsCollector)
                .await?;
            if index.index_type().is_vector() {
                index.prewarm().await?;
                Ok(true)
            } else {
                Ok(false)
            }
        })
        .await?;
    if sdk_segments.is_empty() {
        return Ok(());
    }
    // Keep scalar/FTS validation, including aggregate FTS residency checks, in the SDK.
    if with_position {
        let options = PrewarmOptions::Fts(FtsPrewarmOptions::new().with_position(true));
        dataset
            .prewarm_index_segments_with_options(index_name, &sdk_segments, &options)
            .await?;
    } else {
        dataset
            .prewarm_index_segments(index_name, &sdk_segments)
            .await?;
    }
    Ok(())
}

async fn prewarm_named_index(
    dataset: &lance::Dataset,
    index_name: &str,
    segment_selection: Option<&[Uuid]>,
    with_position: bool,
) -> Result<()> {
    if with_position {
        let options = PrewarmOptions::Fts(FtsPrewarmOptions::new().with_position(true));
        if let Some(segment_selection) = segment_selection {
            dataset
                .prewarm_index_segments_with_options(index_name, segment_selection, &options)
                .await
        } else {
            dataset
                .prewarm_index_with_options(index_name, &options)
                .await
        }
    } else if let Some(segment_selection) = segment_selection {
        dataset
            .prewarm_index_segments(index_name, segment_selection)
            .await
    } else {
        dataset.prewarm_index(index_name).await
    }
}

fn select_prewarm_segments(
    index_name: &str,
    indices: Vec<IndexMetadata>,
    segment_selection: Option<&[Uuid]>,
) -> Result<Vec<IndexMetadata>> {
    let Some(segment_selection) = segment_selection else {
        return Ok(indices);
    };
    if segment_selection.is_empty() {
        return Ok(Vec::new());
    }
    let requested = segment_selection.iter().copied().collect::<HashSet<_>>();
    let mut matched = HashSet::with_capacity(requested.len());
    let selected = indices
        .into_iter()
        .filter(|metadata| {
            if requested.contains(&metadata.uuid) {
                matched.insert(metadata.uuid);
                true
            } else {
                false
            }
        })
        .collect::<Vec<_>>();
    if matched.len() != requested.len() {
        let mut missing = requested
            .difference(&matched)
            .map(ToString::to_string)
            .collect::<Vec<_>>();
        missing.sort();
        return Err(lance_core::Error::index_not_found(format!(
            "name={}, segment_ids=[{}]",
            index_name,
            missing.join(", ")
        )));
    }
    Ok(selected)
}

async fn prewarm_colliding_segments<F, Fut>(
    indices: Vec<IndexMetadata>,
    named_field_id: i32,
    open_and_prewarm: F,
) -> Result<Vec<Uuid>>
where
    F: Fn(i32, Uuid) -> Fut,
    Fut: Future<Output = Result<bool>>,
{
    // Both opening and prewarming can perform remote I/O. Keep the entire
    // segment operation concurrent, with the SDK's fail-fast cancellation.
    let segments = futures::future::try_join_all(indices.into_iter().map(|metadata| {
        let open_and_prewarm = &open_and_prewarm;
        async move {
            let Some(&field_id) = metadata.fields.first() else {
                return Ok::<_, lance_core::Error>(Some(metadata.uuid));
            };
            if field_id == named_field_id || !open_and_prewarm(field_id, metadata.uuid).await? {
                Ok(Some(metadata.uuid))
            } else {
                Ok(None)
            }
        }
    }))
    .await?;
    Ok(segments.into_iter().flatten().collect())
}

/// Owned snapshot of one physical IVF segment's native model.
pub struct LanceIvfModel {
    model: NativeIvfModel,
}

/// Open the first physical vector segment for `index_name` and clone its IVF
/// model. The returned handle owns its centroids and does not borrow the
/// dataset.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_get_ivf_model(
    dataset: *const LanceDataset,
    index_name: *const c_char,
) -> *mut LanceIvfModel {
    ffi_try!(
        unsafe { dataset_get_ivf_model_inner(dataset, index_name) },
        std::ptr::null_mut()
    )
}

unsafe fn dataset_get_ivf_model_inner(
    dataset: *const LanceDataset,
    index_name: *const c_char,
) -> Result<*mut LanceIvfModel> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    let index_name = unsafe { helpers::parse_c_string(index_name)? }
        .filter(|name| !name.is_empty())
        .ok_or_else(|| {
            lance_core::Error::invalid_input_source("index_name must not be NULL or empty".into())
        })?;
    let snapshot = unsafe { &*dataset }.snapshot();
    let indices = block_on(snapshot.load_indices())?;
    let metadata = indices
        .iter()
        .find(|metadata| metadata.name == index_name)
        .ok_or_else(|| lance_core::Error::index_not_found(format!("name={index_name}")))?;
    let field_id = *metadata.fields.first().ok_or_else(|| {
        lance_core::Error::invalid_input(format!(
            "index '{index_name}' segment {} has no fields",
            metadata.uuid
        ))
    })?;
    let field_path = snapshot.schema().field_path_minimal(field_id)?;
    let vector_index =
        block_on(snapshot.open_vector_index(&field_path, &metadata.uuid, &NoOpMetricsCollector))?;
    let model = vector_index.ivf_model().clone();
    Ok(Box::into_raw(Box::new(LanceIvfModel { model })))
}

/// Free an IVF model returned by `lance_dataset_get_ivf_model`. NULL-safe.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_ivf_model_free(model: *mut LanceIvfModel) {
    if !model.is_null() {
        crate::error::swallow_unwind("lance_ivf_model_free", || unsafe {
            drop(Box::from_raw(model));
        });
    }
}

/// Whether the model contains IVF centroids.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_ivf_model_has_centroids(model: *const LanceIvfModel) -> bool {
    ffi_try!(
        (|| -> Result<bool> {
            let model = unsafe { model.as_ref() }.ok_or_else(|| {
                lance_core::Error::invalid_input_source("model must not be NULL".into())
            })?;
            Ok(model.model.centroids_array().is_some())
        })(),
        false
    )
}

/// Export an owned Arrow copy of the model's centroids.
///
/// Both output structs must be zero-initialized. A model without centroids
/// returns `LANCE_ERR_NOT_FOUND`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_ivf_model_export_centroids(
    model: *const LanceIvfModel,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> i32 {
    ffi_try!(
        unsafe { ivf_model_export_centroids_inner(model, out_array, out_schema) },
        neg
    )
}

unsafe fn ivf_model_export_centroids_inner(
    model: *const LanceIvfModel,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> Result<i32> {
    if model.is_null() || out_array.is_null() || out_schema.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "model, out_array, and out_schema must not be NULL".into(),
        ));
    }
    if !unsafe { &*out_array }.is_released() || unsafe { &*out_schema }.release.is_some() {
        return Err(lance_core::Error::invalid_input(
            "out_array and out_schema must be empty (release callbacks must be NULL)",
        ));
    }
    let model = unsafe { &*model };
    let centroids = model
        .model
        .centroids_array()
        .ok_or_else(|| lance_core::Error::index_not_found("IVF model has no centroids"))?
        .clone();
    let data_type = centroids.data_type().clone();
    let (array, generated_schema) = to_ffi(&centroids.into_data())?;
    drop(generated_schema);
    let field = Field::new("centroids", data_type, false);
    let schema = FFI_ArrowSchema::try_from(&field)?;
    unsafe {
        std::ptr::write_unaligned(out_array, array);
        std::ptr::write_unaligned(out_schema, schema);
    }
    Ok(0)
}

/// Number of native IVF partitions. Returns 0 on error; a valid model may also
/// report 0 partitions, so check `lance_last_error_code()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_ivf_model_num_partitions(model: *const LanceIvfModel) -> u64 {
    ffi_try!(
        (|| -> Result<u64> {
            let model = unsafe { model.as_ref() }.ok_or_else(|| {
                lance_core::Error::invalid_input_source("model must not be NULL".into())
            })?;
            u64::try_from(model.model.num_partitions()).map_err(|_| {
                lance_core::Error::internal("IVF partition count cannot be represented as u64")
            })
        })(),
        0
    )
}

/// Native IVF centroid dimension. Returns 0 on error or when centroids are
/// absent; check `lance_last_error_code()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_ivf_model_dimension(model: *const LanceIvfModel) -> u64 {
    ffi_try!(
        (|| -> Result<u64> {
            let model = unsafe { model.as_ref() }.ok_or_else(|| {
                lance_core::Error::invalid_input_source("model must not be NULL".into())
            })?;
            u64::try_from(model.model.dimension()).map_err(|_| {
                lance_core::Error::internal("IVF centroid dimension cannot be represented as u64")
            })
        })(),
        0
    )
}

/// Native IVF partition size. An absent partition returns 0, matching
/// `IvfModel::partition_size`. Returns 0 on a representation error; check
/// `lance_last_error_code()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_ivf_model_partition_size(
    model: *const LanceIvfModel,
    partition_id: u64,
) -> u64 {
    ffi_try!(
        (|| -> Result<u64> {
            let model = unsafe { model.as_ref() }.ok_or_else(|| {
                lance_core::Error::invalid_input_source("model must not be NULL".into())
            })?;
            let partition_id = usize::try_from(partition_id).map_err(|_| {
                lance_core::Error::invalid_input(format!(
                    "partition_id={partition_id} cannot be represented as usize"
                ))
            })?;
            u64::try_from(model.model.partition_size(partition_id)).map_err(|_| {
                lance_core::Error::internal("IVF partition size cannot be represented as u64")
            })
        })(),
        0
    )
}

/// Read one logical IVF partition across all physical segments.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_read_index_partition(
    dataset: *const LanceDataset,
    index_name: *const c_char,
    partition_id: u64,
    with_vector: bool,
    out_stream: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe {
            dataset_read_index_partition_inner(
                dataset,
                index_name,
                partition_id,
                with_vector,
                out_stream,
            )
        },
        neg
    )
}

unsafe fn dataset_read_index_partition_inner(
    dataset: *const LanceDataset,
    index_name: *const c_char,
    partition_id: u64,
    with_vector: bool,
    out_stream: *mut FFI_ArrowArrayStream,
) -> Result<i32> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    let index_name = unsafe { helpers::parse_c_string(index_name)? }
        .filter(|name| !name.is_empty())
        .ok_or_else(|| {
            lance_core::Error::invalid_input_source("index_name must not be NULL or empty".into())
        })?;
    if out_stream.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_stream must not be NULL".into(),
        ));
    }
    let partition_id = usize::try_from(partition_id).map_err(|_| {
        lance_core::Error::invalid_input(format!(
            "partition_id={partition_id} cannot be represented as usize"
        ))
    })?;
    let snapshot = unsafe { &*dataset }.snapshot();
    let stream = block_on(snapshot.read_index_partition(index_name, partition_id, with_vector))?;
    let schema = stream.schema();
    let reader = GuardedReader::new(
        stream,
        schema,
        RT.handle().clone(),
        Arc::new(AtomicBool::new(false)),
    );
    let ffi_stream = FFI_ArrowArrayStream::new(Box::new(reader));
    unsafe {
        std::ptr::write_unaligned(out_stream, ffi_stream);
    }
    Ok(0)
}

#[cfg(test)]
#[path = "index_tests.rs"]
mod tests;

/// Drop an index by name.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_drop_index(
    dataset: *mut LanceDataset,
    name: *const c_char,
) -> i32 {
    ffi_try!(unsafe { drop_index_inner(dataset, name) }, neg)
}

unsafe fn drop_index_inner(dataset: *mut LanceDataset, name: *const c_char) -> Result<i32> {
    if dataset.is_null() || name.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset and name must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let name_str = unsafe { helpers::parse_c_string(name)? }.unwrap();

    ds.with_mut(|d| block_on(d.drop_index(name_str)))?;
    Ok(0)
}

/// JSON array describing all user indexes. Caller must free with lance_free_string().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_index_list_json(
    dataset: *const LanceDataset,
) -> *const c_char {
    ffi_try!(unsafe { index_list_json_inner(dataset) }, null)
}

unsafe fn index_list_json_inner(dataset: *const LanceDataset) -> Result<*const c_char> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset is NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let snap = ds.snapshot();

    let indices = block_on(snap.load_indices())?;
    let schema = snap.schema();
    let descriptions = block_on(snap.describe_indices(None))?;
    // Map name -> index_type string from describe_indices (one I/O batch)
    let type_by_name: std::collections::HashMap<String, String> = descriptions
        .iter()
        .map(|d| (d.name().to_string(), d.index_type().to_string()))
        .collect();

    let mut entries: Vec<String> = Vec::new();
    for idx in indices.iter() {
        if lance_index::is_system_index(idx) {
            continue;
        }
        let columns: Vec<String> = idx
            .fields
            .iter()
            .filter_map(|fid| schema.field_by_id(*fid).map(|f| f.name.clone()))
            .collect();
        let type_str = type_by_name
            .get(&idx.name)
            .cloned()
            .unwrap_or_else(|| "Unknown".to_string());
        let cols_json = columns
            .iter()
            .map(|c| format!("\"{}\"", c.replace('"', "\\\"")))
            .collect::<Vec<_>>()
            .join(",");
        entries.push(format!(
            "{{\"name\":\"{}\",\"uuid\":\"{}\",\"columns\":[{}],\"type\":\"{}\",\"dataset_version\":{}}}",
            idx.name.replace('"', "\\\""),
            idx.uuid,
            cols_json,
            type_str,
            idx.dataset_version,
        ));
    }
    let json = format!("[{}]", entries.join(","));

    // A JSON payload with an interior NUL cannot cross the C boundary as a
    // `char*`; return NULL without an error message, as before.
    Ok(CString::new(json)
        .map(|s| s.into_raw() as *const c_char)
        .unwrap_or(std::ptr::null()))
}

fn export_owned_string(value: String) -> Result<*const c_char> {
    CString::new(value)
        .map(|value| value.into_raw().cast_const())
        .map_err(|_| {
            lance_core::Error::invalid_input(
                "JSON output contains an embedded NUL byte".to_string(),
            )
        })
}

/// JSON array describing all indexes, including system indexes, with the
/// native `IndexDescription` fields and physical segment metadata.
///
/// Caller must free the returned string with `lance_free_string()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_describe_indices_json(
    dataset: *const LanceDataset,
) -> *const c_char {
    ffi_try!(unsafe { describe_indices_json_inner(dataset) }, null)
}

unsafe fn describe_indices_json_inner(dataset: *const LanceDataset) -> Result<*const c_char> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset is NULL".into(),
        ));
    }
    let dataset = unsafe { &*dataset };
    let snapshot = dataset.snapshot();
    let descriptions = block_on(snapshot.describe_indices(None))?;
    let schema = snapshot.schema();
    let mut entries = Vec::with_capacity(descriptions.len());
    for description in descriptions {
        let details = description.details()?;
        let details = serde_json::from_str::<serde_json::Value>(&details).map_err(|error| {
            lance_core::Error::internal(format!(
                "native index description for '{}' returned invalid JSON: {error}",
                description.name()
            ))
        })?;
        let field_names = description
            .field_ids()
            .iter()
            .map(|field_id| {
                let field_id = i32::try_from(*field_id).map_err(|_| {
                    lance_core::Error::invalid_input(format!(
                        "index '{}' field id {field_id} cannot be represented as i32",
                        description.name()
                    ))
                })?;
                schema.field_path_minimal(field_id)
            })
            .collect::<Result<Vec<_>>>()?;
        let segments = description
            .segments()
            .iter()
            .map(|segment| {
                let fragment_ids = segment
                    .fragment_bitmap
                    .as_ref()
                    .map(|bitmap| bitmap.iter().collect::<Vec<_>>());
                let created_at = segment
                    .created_at
                    .map(|created_at| created_at.to_rfc3339_opts(SecondsFormat::AutoSi, true));
                json!({
                    "uuid": segment.uuid.to_string(),
                    "dataset_version_at_last_update": segment.dataset_version,
                    "fragment_ids": fragment_ids,
                    "index_version": segment.index_version,
                    "created_at": created_at,
                    "size_bytes": segment.total_size_bytes(),
                    "base_id": segment.base_id,
                    "covering_fields": segment.covering_fields,
                })
            })
            .collect::<Vec<_>>();
        entries.push(json!({
            "name": description.name(),
            "type_url": description.type_url(),
            "index_type": description.index_type(),
            "fields": description.field_ids(),
            "field_names": field_names,
            "num_rows_indexed": description.rows_indexed(),
            "details": details,
            "total_size_bytes": description.total_size_bytes(),
            "segments": segments,
        }));
    }
    let json = serde_json::to_string(&entries).map_err(|error| {
        lance_core::Error::internal(format!("failed to serialize index descriptions: {error}"))
    })?;
    export_owned_string(json)
}

/// Return the index-family-specific native statistics JSON for `index_name`.
///
/// Old corrupt fragment coverage can make Lance publish a migration version
/// before returning. The dataset handle is not automatically refreshed.
/// Caller must free a successful string with `lance_free_string()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_index_statistics_json(
    dataset: *mut LanceDataset,
    index_name: *const c_char,
) -> *const c_char {
    ffi_try!(
        unsafe { index_statistics_json_inner(dataset, index_name) },
        null
    )
}

unsafe fn index_statistics_json_inner(
    dataset: *mut LanceDataset,
    index_name: *const c_char,
) -> Result<*const c_char> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset is NULL".into(),
        ));
    }
    let index_name = unsafe { helpers::parse_c_string(index_name)? }
        .filter(|value| !value.is_empty())
        .ok_or_else(|| {
            lance_core::Error::invalid_input_source("index_name must not be NULL or empty".into())
        })?
        .to_string();
    let snapshot = unsafe { &*dataset }.snapshot();
    let json = block_on(snapshot.index_statistics(&index_name))?;
    export_owned_string(json)
}

/// JSON array describing non-system physical index segments visible to core.
/// Caller must free with lance_free_string().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_index_segments_json(
    dataset: *const LanceDataset,
) -> *const c_char {
    ffi_try!(unsafe { index_segments_json_inner(dataset) }, null)
}

unsafe fn index_segments_json_inner(dataset: *const LanceDataset) -> Result<*const c_char> {
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset is NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let snap = ds.snapshot();
    let indices = block_on(snap.load_indices())?;

    let schema = snap.schema();
    let json = serialize_index_segments_json(&indices, |field_id| {
        schema.field_path_minimal(field_id).ok()
    })?;
    CString::new(json)
        .map(|value| value.into_raw() as *const c_char)
        .map_err(|_| {
            lance_core::Error::invalid_input(
                "index segment listing contains an embedded NUL byte".to_string(),
            )
        })
}

fn serialized_index_type(index: &IndexMetadata) -> String {
    let Some(details) = &index.index_details else {
        // Core descriptions recognize pre-details vector indices by this file.
        return if index.files.as_ref().is_some_and(|files| {
            files
                .iter()
                .any(|file| file.path == lance_index::INDEX_FILE_NAME)
        }) {
            "Vector"
        } else {
            "Unknown"
        }
        .to_string();
    };
    let details = lance::index::scalar::IndexDetails(Arc::clone(details));
    if details.is_vector() {
        // Describe the stored protobuf independently of coverage, using core's
        // vector description format. Core's formatter is crate-private.
        if details.0.value.is_empty() {
            return "Vector".to_string();
        }
        let Ok(vector) = lance_index::pb::VectorIndexDetails::decode(details.0.value.as_slice())
        else {
            return "Vector".to_string();
        };
        use lance_index::pb::vector_index_details::Compression;
        let compression = match vector.compression {
            None | Some(Compression::Flat(_)) => "FLAT",
            Some(Compression::Pq(_)) => "PQ",
            Some(Compression::Sq(_)) => "SQ",
            Some(Compression::Rq(_)) => "RQ",
        };
        let prefix = if vector.hnsw_index_config.is_some() {
            "IVF_HNSW_"
        } else {
            "IVF_"
        };
        format!("{prefix}{compression}")
    } else {
        details
            .get_plugin()
            .map(|plugin| plugin.name().to_string())
            .unwrap_or_else(|_| {
                lance_index::registry::display_type_from_url(&details.0.type_url).to_string()
            })
    }
}

fn serialize_index_segments_json(
    indices: &[IndexMetadata],
    field_name: impl Fn(i32) -> Option<String>,
) -> Result<String> {
    let entries = indices
        .iter()
        .filter(|index| !lance_index::is_system_index(index))
        .map(|index| {
            let columns = index
                .keyed_fields()
                .iter()
                .filter_map(|field_id| field_name(*field_id))
                .collect::<Vec<_>>();
            let fragment_ids = index
                .fragment_bitmap
                .as_ref()
                .map(|bitmap| bitmap.iter().collect::<Vec<_>>());
            json!({
                "name": index.name,
                "uuid": index.uuid.to_string(),
                "columns": columns,
                "type": serialized_index_type(index),
                "dataset_version": index.dataset_version,
                "index_version": index.index_version,
                "fragment_ids": fragment_ids,
            })
        })
        .collect::<Vec<_>>();
    serde_json::to_string(&entries).map_err(|error| {
        lance_core::Error::internal(format!(
            "failed to serialize index segment listing: {error}"
        ))
    })
}

// ---------------------------------------------------------------------------
// Vector index creation
// ---------------------------------------------------------------------------

use lance::index::vector::VectorIndexParams as LanceCoreVectorIndexParams;
use lance_index::vector::hnsw::builder::HnswBuildParams;
use lance_index::vector::ivf::IvfBuildParams;
use lance_index::vector::pq::PQBuildParams;
use lance_index::vector::sq::builder::SQBuildParams;
use lance_linalg::distance::DistanceType;

/// Vector index variant tag, mirroring the C enum `LanceVectorIndexType`.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceVectorIndexType {
    IvfFlat = 101,
    IvfSq = 102,
    IvfPq = 103,
    IvfHnswSq = 104,
    IvfHnswPq = 105,
    IvfHnswFlat = 106,
    IvfRq = 107,
}

impl LanceVectorIndexType {
    pub(crate) fn from_c(value: i32) -> Result<Self> {
        match value {
            101 => Ok(Self::IvfFlat),
            102 => Ok(Self::IvfSq),
            103 => Ok(Self::IvfPq),
            104 => Ok(Self::IvfHnswSq),
            105 => Ok(Self::IvfHnswPq),
            106 => Ok(Self::IvfHnswFlat),
            107 => Ok(Self::IvfRq),
            _ => Err(lance_core::Error::invalid_input_source(
                format!("invalid vector index type: {value}").into(),
            )),
        }
    }
}

/// Distance metric, mirroring the C enum `LanceMetricType`.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceMetricType {
    L2 = 0,
    Cosine = 1,
    Dot = 2,
    Hamming = 3,
}

/// Tagged-union of vector-index build parameters; mirrors the C struct
/// `LanceVectorIndexParams` field-for-field.
///
/// Field semantics (zero = "use Lance default" unless otherwise noted):
/// * `num_partitions`  — IVF; **required (must be > 0)**.
/// * `num_sub_vectors` — PQ; **required for any PQ variant** (must be > 0).
/// * `num_bits`        — PQ: 0 → 8, otherwise 4 or 8; SQ: 0 or 8.
/// * `max_iterations`  — IVF/PQ kmeans; 0 → 50.
/// * `hnsw_m`          — HNSW; **required for any HNSW variant** (must be > 0).
/// * `hnsw_ef_construction` — HNSW; 0 → 150.
/// * `sample_rate`     — IVF/SQ; 0 → 256.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceVectorIndexParams {
    pub index_type: LanceVectorIndexType,
    pub metric: LanceMetricType,
    pub num_partitions: u32,
    pub num_sub_vectors: u32,
    pub num_bits: u32,
    pub max_iterations: u32,
    pub hnsw_m: u32,
    pub hnsw_ef_construction: u32,
    pub sample_rate: u32,
}

/// Presence-aware unsigned option for native `Option<usize>` and similar
/// fields. `is_set == false` means the value was not specified and `value` is
/// ignored, including values that would not fit the native type.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct LanceOptionalU64 {
    pub value: u64,
    pub is_set: bool,
}

/// Borrowed UTF-8 string map. Duplicate keys follow Rust `HashMap` insertion
/// semantics, so the last value wins.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceStringMap {
    pub keys: *const *const c_char,
    pub values: *const *const c_char,
    pub count: usize,
}

/// Borrowed precomputed shuffle-buffer input. `path` must be non-NULL.
/// `buffer_count == 0` represents an explicitly empty buffer list.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LancePrecomputedShuffleBuffers {
    pub path: *const c_char,
    pub buffers: *const *const c_char,
    pub buffer_count: usize,
}

#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceIndexFileVersion {
    V3 = 0,
    Legacy = 1,
}

impl LanceIndexFileVersion {
    fn from_c(value: i32) -> Result<Self> {
        match value {
            0 => Ok(Self::V3),
            1 => Ok(Self::Legacy),
            _ => Err(lance_core::Error::invalid_input(format!(
                "index_file_version must be 0 (V3) or 1 (Legacy), got {value}"
            ))),
        }
    }

    fn to_native(self) -> IndexFileVersion {
        match self {
            Self::V3 => IndexFileVersion::V3,
            Self::Legacy => IndexFileVersion::Legacy,
        }
    }
}

/// Additive vector-build options. The whole object is zero-initializable:
/// every `LanceOptionalU64` defaults to "not specified", `index_file_version`
/// zero selects V3, and absent model/map pointers select native defaults.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceVectorIndexOptions {
    pub index_type: i32,
    pub metric: i32,

    pub num_partitions: LanceOptionalU64,
    pub target_partition_size: LanceOptionalU64,
    pub max_iters: LanceOptionalU64,
    pub sample_rate: LanceOptionalU64,
    pub streaming_sample_rate: LanceOptionalU64,
    pub streaming_coreset_rate: LanceOptionalU64,
    pub streaming_refine_passes: u64,

    pub precomputed_partitions_file: *const c_char,
    pub precomputed_shuffle_buffers: *const LancePrecomputedShuffleBuffers,
    pub shuffle_partition_batches: LanceOptionalU64,
    pub shuffle_partition_concurrency: LanceOptionalU64,
    pub storage_options: *const LanceStringMap,

    pub num_sub_vectors: LanceOptionalU64,
    pub num_bits: LanceOptionalU64,

    pub ivf_centroids: *mut FFI_ArrowArray,
    pub ivf_centroids_schema: *const FFI_ArrowSchema,
    pub pq_codebook: *mut FFI_ArrowArray,
    pub pq_codebook_schema: *const FFI_ArrowSchema,
    pub rabitq_model_json: *const c_char,

    pub hnsw_max_level: LanceOptionalU64,
    pub hnsw_m: LanceOptionalU64,
    pub hnsw_ef_construction: LanceOptionalU64,

    pub index_file_version: i32,
    pub skip_transpose: bool,
    pub runtime_hints: *const LanceStringMap,
    pub has_train: bool,
    pub train: bool,
}

pub(crate) struct PreparedVectorBuild {
    pub params: LanceCoreVectorIndexParams,
    pub train: bool,
}

impl LanceMetricType {
    pub(crate) fn from_c(value: i32) -> Result<Self> {
        match value {
            0 => Ok(Self::L2),
            1 => Ok(Self::Cosine),
            2 => Ok(Self::Dot),
            3 => Ok(Self::Hamming),
            _ => Err(lance_core::Error::invalid_input_source(
                format!(
                    "metric must be one of 0 (L2), 1 (Cosine), 2 (Dot), or 3 (Hamming); got {value}"
                )
                .into(),
            )),
        }
    }

    pub(crate) fn to_distance(self) -> DistanceType {
        match self {
            Self::L2 => DistanceType::L2,
            Self::Cosine => DistanceType::Cosine,
            Self::Dot => DistanceType::Dot,
            Self::Hamming => DistanceType::Hamming,
        }
    }
}

/// Reject zero-valued required fields with a descriptive error naming the field.
/// Per CLAUDE.md: never silently clamp or substitute when the user passed an
/// invalid value at the API boundary.
fn require_field(name: &str, value: u32) -> Result<u32> {
    if value == 0 {
        Err(lance_core::Error::invalid_input_source(
            format!("{} is required for this index type and must be > 0", name).into(),
        ))
    } else {
        Ok(value)
    }
}

fn build_ivf(
    p: &LanceVectorIndexParams,
    centroids: Option<Arc<FixedSizeListArray>>,
) -> Result<IvfBuildParams> {
    let num_partitions = require_field("num_partitions", p.num_partitions)? as usize;
    let mut ivf = IvfBuildParams::new(num_partitions);
    if let Some(centroids) = centroids {
        if centroids.len() != num_partitions {
            return Err(lance_core::Error::invalid_input(format!(
                "ivf_centroids length {} does not match num_partitions {num_partitions}",
                centroids.len()
            )));
        }
        ivf.centroids = Some(centroids);
    }
    if p.max_iterations != 0 {
        ivf.max_iters = p.max_iterations as usize;
    }
    if p.sample_rate != 0 {
        ivf.sample_rate = p.sample_rate as usize;
    }
    Ok(ivf)
}

fn build_rq(p: &LanceVectorIndexParams) -> Result<RQBuildParams> {
    let mut rq = RQBuildParams::default();
    if p.num_bits != 0 {
        rq.num_bits = u8::try_from(p.num_bits).map_err(|_| {
            lance_core::Error::invalid_input(format!(
                "num_bits {} cannot be represented as the native IVF_RQ u8 bit width",
                p.num_bits
            ))
        })?;
    }
    Ok(rq)
}

fn build_pq(p: &LanceVectorIndexParams, codebook: Option<ArrayRef>) -> Result<PQBuildParams> {
    let num_sub_vectors = require_field("num_sub_vectors", p.num_sub_vectors)? as usize;
    let num_bits = if p.num_bits == 0 {
        8
    } else {
        p.num_bits as usize
    };
    if !matches!(num_bits, 4 | 8) {
        return Err(lance_core::Error::invalid_input_source(
            format!("num_bits must be 4 or 8 for Lance PQ indexes, got {num_bits}").into(),
        ));
    }
    let max_iters = if p.max_iterations == 0 {
        50
    } else {
        p.max_iterations as usize
    };
    Ok(PQBuildParams {
        num_sub_vectors,
        num_bits,
        max_iters,
        codebook,
        ..Default::default()
    })
}

fn build_sq(p: &LanceVectorIndexParams) -> Result<SQBuildParams> {
    let mut sq = SQBuildParams::default();
    if p.num_bits != 0 {
        if p.num_bits != 8 {
            return Err(lance_core::Error::invalid_input_source(
                format!(
                    "num_bits must be 0 or 8 for Lance SQ indexes, got {}",
                    p.num_bits
                )
                .into(),
            ));
        }
        sq.num_bits = p.num_bits as u16;
    }
    if p.sample_rate != 0 {
        sq.sample_rate = p.sample_rate as usize;
    }
    Ok(sq)
}

fn build_hnsw(p: &LanceVectorIndexParams) -> Result<HnswBuildParams> {
    let m = require_field("hnsw_m", p.hnsw_m)? as usize;
    let mut hnsw = HnswBuildParams {
        m,
        ..Default::default()
    };
    if p.hnsw_ef_construction != 0 {
        hnsw.ef_construction = p.hnsw_ef_construction as usize;
    }
    Ok(hnsw)
}

fn build_vector_params(p: &LanceVectorIndexParams) -> Result<LanceCoreVectorIndexParams> {
    build_vector_params_with_models(p, None, None)
}

pub(crate) fn build_vector_params_with_models(
    p: &LanceVectorIndexParams,
    centroids: Option<Arc<FixedSizeListArray>>,
    codebook: Option<ArrayRef>,
) -> Result<LanceCoreVectorIndexParams> {
    let metric = p.metric.to_distance();
    use LanceVectorIndexType::*;
    let core = match p.index_type {
        IvfFlat => {
            let ivf = build_ivf(p, centroids)?;
            LanceCoreVectorIndexParams::with_ivf_flat_params(metric, ivf)
        }
        IvfPq => {
            let ivf = build_ivf(p, centroids)?;
            let pq = build_pq(p, codebook)?;
            LanceCoreVectorIndexParams::with_ivf_pq_params(metric, ivf, pq)
        }
        IvfSq => {
            let ivf = build_ivf(p, centroids)?;
            let sq = build_sq(p)?;
            LanceCoreVectorIndexParams::with_ivf_sq_params(metric, ivf, sq)
        }
        IvfRq => {
            let ivf = build_ivf(p, centroids)?;
            let rq = build_rq(p)?;
            LanceCoreVectorIndexParams::with_ivf_rq_params(metric, ivf, rq)
        }
        IvfHnswFlat => {
            let ivf = build_ivf(p, centroids)?;
            let hnsw = build_hnsw(p)?;
            LanceCoreVectorIndexParams::ivf_hnsw(metric, ivf, hnsw)
        }
        IvfHnswPq => {
            let ivf = build_ivf(p, centroids)?;
            let hnsw = build_hnsw(p)?;
            let pq = build_pq(p, codebook)?;
            LanceCoreVectorIndexParams::with_ivf_hnsw_pq_params(metric, ivf, hnsw, pq)
        }
        IvfHnswSq => {
            let ivf = build_ivf(p, centroids)?;
            let hnsw = build_hnsw(p)?;
            let sq = build_sq(p)?;
            LanceCoreVectorIndexParams::with_ivf_hnsw_sq_params(metric, ivf, hnsw, sq)
        }
    };
    Ok(core)
}

fn parse_optional_u64(field: LanceOptionalU64) -> Result<Option<u64>> {
    Ok(if field.is_set {
        Some(field.value)
    } else {
        None
    })
}

fn parse_optional_usize(field: LanceOptionalU64, name: &str) -> Result<Option<usize>> {
    parse_optional_u64(field)?
        .map(|value| {
            usize::try_from(value).map_err(|_| {
                lance_core::Error::invalid_input(format!(
                    "{name}={value} cannot be represented as usize on this platform"
                ))
            })
        })
        .transpose()
}

fn parse_optional_u16(field: LanceOptionalU64, name: &str) -> Result<Option<u16>> {
    parse_optional_u64(field)?
        .map(|value| {
            u16::try_from(value).map_err(|_| {
                lance_core::Error::invalid_input(format!(
                    "{name}={value} cannot be represented as u16"
                ))
            })
        })
        .transpose()
}

fn parse_optional_u8(field: LanceOptionalU64, name: &str) -> Result<Option<u8>> {
    parse_optional_u64(field)?
        .map(|value| {
            u8::try_from(value).map_err(|_| {
                lance_core::Error::invalid_input(format!(
                    "{name}={value} cannot be represented as u8"
                ))
            })
        })
        .transpose()
}

pub(crate) unsafe fn parse_string_map(
    map: *const LanceStringMap,
    label: &str,
) -> Result<Option<HashMap<String, String>>> {
    if map.is_null() {
        return Ok(None);
    }
    let map = unsafe { &*map };
    if map.count > isize::MAX as usize / std::mem::size_of::<*const c_char>() {
        return Err(lance_core::Error::invalid_input(format!(
            "{label}.count {} exceeds the maximum addressable pointer slice length",
            map.count
        )));
    }
    if map.count > 0 && (map.keys.is_null() || map.values.is_null()) {
        return Err(lance_core::Error::invalid_input(format!(
            "{label}.keys and {label}.values must be non-NULL when count is {}",
            map.count
        )));
    }
    let mut parsed = HashMap::with_capacity(map.count);
    for index in 0..map.count {
        let key = unsafe { *map.keys.add(index) };
        let value = unsafe { *map.values.add(index) };
        if key.is_null() || value.is_null() {
            return Err(lance_core::Error::invalid_input(format!(
                "{label}.keys[{index}] and {label}.values[{index}] must not be NULL"
            )));
        }
        let key = unsafe { CStr::from_ptr(key) }.to_str().map_err(|error| {
            lance_core::Error::invalid_input(format!(
                "{label}.keys[{index}] is not valid UTF-8: {error}"
            ))
        })?;
        let value = unsafe { CStr::from_ptr(value) }.to_str().map_err(|error| {
            lance_core::Error::invalid_input(format!(
                "{label}.values[{index}] is not valid UTF-8: {error}"
            ))
        })?;
        parsed.insert(key.to_string(), value.to_string());
    }
    Ok(Some(parsed))
}

unsafe fn parse_precomputed_shuffle_buffers(
    buffers: *const LancePrecomputedShuffleBuffers,
) -> Result<Option<(Path, Vec<String>)>> {
    if buffers.is_null() {
        return Ok(None);
    }
    let buffers = unsafe { &*buffers };
    let raw_path = unsafe { helpers::parse_c_string(buffers.path)? }.ok_or_else(|| {
        lance_core::Error::invalid_input("precomputed_shuffle_buffers.path must not be NULL")
    })?;
    let path = Path::parse(raw_path).map_err(|error| {
        lance_core::Error::invalid_input(format!(
            "precomputed_shuffle_buffers.path '{raw_path}' is invalid: {error}"
        ))
    })?;
    if buffers.buffer_count > isize::MAX as usize / std::mem::size_of::<*const c_char>() {
        return Err(lance_core::Error::invalid_input(format!(
            "precomputed_shuffle_buffers.buffer_count {} exceeds the maximum addressable pointer slice length",
            buffers.buffer_count
        )));
    }
    if buffers.buffer_count > 0 && buffers.buffers.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "precomputed_shuffle_buffers.buffers must be non-NULL when buffer_count is {}",
            buffers.buffer_count
        )));
    }
    let mut names = Vec::with_capacity(buffers.buffer_count);
    for index in 0..buffers.buffer_count {
        let buffer = unsafe { *buffers.buffers.add(index) };
        let name = unsafe { helpers::parse_c_string(buffer)? }.ok_or_else(|| {
            lance_core::Error::invalid_input(format!(
                "precomputed_shuffle_buffers.buffers[{index}] must not be NULL"
            ))
        })?;
        names.push(name.to_string());
    }
    Ok(Some((path, names)))
}

unsafe fn parse_native_fixed_size_list_model(
    array: *mut FFI_ArrowArray,
    schema: *const FFI_ArrowSchema,
    name: &str,
) -> Result<Option<FixedSizeListArray>> {
    if array.is_null() != schema.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "{name} and {name}_schema must both be NULL or both be non-NULL"
        )));
    }
    if array.is_null() {
        return Ok(None);
    }
    let model = unsafe { borrow_model_array(array, schema, name)? };
    fixed_size_list_model(model, name).map(Some)
}

/// Convert the additive C options into native builder parameters. All native
/// semantic validation stays in Lance. This layer only validates representation
/// (UTF-8, pointer pairing, addressability and Rust type ranges).
pub(crate) unsafe fn prepare_vector_build(
    options: *const LanceVectorIndexOptions,
) -> Result<PreparedVectorBuild> {
    if options.is_null() {
        return Err(lance_core::Error::invalid_input(
            "options must not be NULL for the vector index options entry point",
        ));
    }
    let options = unsafe { &*options };
    let index_type = LanceVectorIndexType::from_c(options.index_type)?;
    let metric = LanceMetricType::from_c(options.metric)?;
    let train = if options.has_train {
        options.train
    } else {
        true
    };

    let mut ivf = IvfBuildParams {
        num_partitions: parse_optional_usize(options.num_partitions, "num_partitions")?,
        target_partition_size: parse_optional_usize(
            options.target_partition_size,
            "target_partition_size",
        )?,
        ..Default::default()
    };
    if let Some(value) = parse_optional_usize(options.max_iters, "max_iters")? {
        ivf.max_iters = value;
    }
    if let Some(value) = parse_optional_usize(options.sample_rate, "sample_rate")? {
        ivf.sample_rate = value;
    }
    ivf.streaming_sample_rate =
        parse_optional_usize(options.streaming_sample_rate, "streaming_sample_rate")?;
    ivf.streaming_coreset_rate =
        parse_optional_usize(options.streaming_coreset_rate, "streaming_coreset_rate")?;
    ivf.streaming_refine_passes =
        usize::try_from(options.streaming_refine_passes).map_err(|_| {
            lance_core::Error::invalid_input(format!(
                "streaming_refine_passes={} cannot be represented as usize",
                options.streaming_refine_passes
            ))
        })?;
    ivf.precomputed_partitions_file =
        unsafe { helpers::parse_c_string(options.precomputed_partitions_file)? }
            .map(str::to_string);
    ivf.precomputed_shuffle_buffers =
        unsafe { parse_precomputed_shuffle_buffers(options.precomputed_shuffle_buffers)? };
    if let Some(value) = parse_optional_usize(
        options.shuffle_partition_batches,
        "shuffle_partition_batches",
    )? {
        ivf.shuffle_partition_batches = value;
    }
    if let Some(value) = parse_optional_usize(
        options.shuffle_partition_concurrency,
        "shuffle_partition_concurrency",
    )? {
        ivf.shuffle_partition_concurrency = value;
    }
    ivf.storage_options = unsafe { parse_string_map(options.storage_options, "storage_options")? };

    if let Some(centroids) = unsafe {
        parse_native_fixed_size_list_model(
            options.ivf_centroids,
            options.ivf_centroids_schema,
            "ivf_centroids",
        )?
    } {
        ivf.centroids = Some(Arc::new(centroids));
    }

    let codebook = unsafe {
        parse_native_fixed_size_list_model(
            options.pq_codebook,
            options.pq_codebook_schema,
            "pq_codebook",
        )?
    }
    .map(|codebook| codebook.values().clone());

    let mut pq = PQBuildParams::default();
    if let Some(value) = parse_optional_usize(options.num_sub_vectors, "num_sub_vectors")? {
        pq.num_sub_vectors = value;
    }
    if let Some(value) = parse_optional_usize(options.num_bits, "num_bits")? {
        pq.num_bits = value;
    }
    if let Some(value) = parse_optional_usize(options.max_iters, "max_iters")? {
        pq.max_iters = value;
    }
    if let Some(value) = parse_optional_usize(options.sample_rate, "sample_rate")? {
        pq.sample_rate = value;
    }
    pq.codebook = codebook;

    let mut sq = SQBuildParams::default();
    if let Some(value) = parse_optional_usize(options.sample_rate, "sample_rate")? {
        sq.sample_rate = value;
    }

    let mut hnsw = HnswBuildParams::default();
    if let Some(value) = parse_optional_u16(options.hnsw_max_level, "hnsw_max_level")? {
        hnsw.max_level = value;
    }
    if let Some(value) = parse_optional_usize(options.hnsw_m, "hnsw_m")? {
        hnsw.m = value;
    }
    if let Some(value) = parse_optional_usize(options.hnsw_ef_construction, "hnsw_ef_construction")?
    {
        hnsw.ef_construction = value;
    }

    let mut rq = RQBuildParams::default();
    if matches!(index_type, LanceVectorIndexType::IvfRq) {
        if let Some(value) = parse_optional_u8(options.num_bits, "num_bits")? {
            rq.num_bits = value;
        }
        if let Some(json) = unsafe { helpers::parse_c_string(options.rabitq_model_json)? } {
            rq.rotation = Some(
                serde_json::from_str::<RabitQuantizationMetadata>(json).map_err(|error| {
                    lance_core::Error::invalid_input(format!(
                        "rabitq_model_json is not a valid RabitQuantizationMetadata value: {error}"
                    ))
                })?,
            );
        }
    } else if !options.rabitq_model_json.is_null() {
        return Err(lance_core::Error::invalid_input(format!(
            "rabitq_model_json is only valid for IVF_RQ, got index_type={index_type:?}"
        )));
    }

    use LanceVectorIndexType::*;
    let is_pq = matches!(index_type, IvfPq | IvfHnswPq);
    if !is_pq && pq.codebook.is_some() {
        return Err(lance_core::Error::invalid_input(format!(
            "pq_codebook is only valid for IVF_PQ and IVF_HNSW_PQ, got index_type={index_type:?}"
        )));
    }

    let native_metric = metric.to_distance();
    let mut params = match index_type {
        IvfFlat => LanceCoreVectorIndexParams::with_ivf_flat_params(native_metric, ivf),
        IvfSq => LanceCoreVectorIndexParams::with_ivf_sq_params(native_metric, ivf, sq),
        IvfPq => LanceCoreVectorIndexParams::with_ivf_pq_params(native_metric, ivf, pq),
        IvfRq => LanceCoreVectorIndexParams::with_ivf_rq_params(native_metric, ivf, rq),
        IvfHnswFlat => LanceCoreVectorIndexParams::ivf_hnsw(native_metric, ivf, hnsw),
        IvfHnswPq => {
            LanceCoreVectorIndexParams::with_ivf_hnsw_pq_params(native_metric, ivf, hnsw, pq)
        }
        IvfHnswSq => {
            LanceCoreVectorIndexParams::with_ivf_hnsw_sq_params(native_metric, ivf, hnsw, sq)
        }
    };
    params.version = LanceIndexFileVersion::from_c(options.index_file_version)?.to_native();
    params.skip_transpose = options.skip_transpose;
    params.runtime_hints =
        unsafe { parse_string_map(options.runtime_hints, "runtime_hints")? }.unwrap_or_default();
    Ok(PreparedVectorBuild { params, train })
}

/// Create a vector index on a column.
///
/// `params.index_type` selects the variant; the rest of the fields are
/// validated against that variant (missing required fields produce a
/// descriptive error naming the field).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_create_vector_index(
    dataset: *mut LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    params: *const LanceVectorIndexParams,
    replace: bool,
) -> i32 {
    ffi_try!(
        unsafe { create_vector_index_inner(dataset, column, index_name, params, replace) },
        neg
    )
}

/// Create a vector index from additive, presence-aware native options.
///
/// The native builder commits the index and then reloads committed index
/// metadata. An error can therefore be returned after the dataset version has
/// already been published. Reopen the dataset to determine its current
/// version; this entry point does not claim storage rollback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_create_vector_index_with_options(
    dataset: *mut LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    options: *const LanceVectorIndexOptions,
    replace: bool,
) -> i32 {
    ffi_try!(
        unsafe {
            create_vector_index_with_options_inner(dataset, column, index_name, options, replace)
        },
        neg
    )
}

unsafe fn create_vector_index_inner(
    dataset: *mut LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    params: *const LanceVectorIndexParams,
    replace: bool,
) -> Result<i32> {
    if dataset.is_null() || column.is_null() || params.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset, column, and params must not be NULL".into(),
        ));
    }
    let ds = unsafe { &*dataset };
    let column_str = unsafe { helpers::parse_c_string(column)? }.ok_or_else(|| {
        lance_core::Error::invalid_input_source("column must not be empty".into())
    })?;
    let name = unsafe { helpers::parse_c_string(index_name)? }.map(|s| s.to_string());
    let p = unsafe { &*params };
    let core_params = build_vector_params(p)?;

    ds.with_mut(|d| {
        block_on(d.create_index(
            &[column_str],
            IndexType::Vector,
            name,
            &core_params,
            replace,
        ))
    })?;
    Ok(0)
}

unsafe fn create_vector_index_with_options_inner(
    dataset: *mut LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    options: *const LanceVectorIndexOptions,
    replace: bool,
) -> Result<i32> {
    if dataset.is_null() || column.is_null() || options.is_null() {
        return Err(lance_core::Error::invalid_input(
            "dataset, column, and options must not be NULL",
        ));
    }
    let ds = unsafe { &*dataset };
    let column = unsafe { helpers::parse_c_string(column)? }
        .filter(|value| !value.is_empty())
        .ok_or_else(|| lance_core::Error::invalid_input("column must not be NULL or empty"))?
        .to_string();
    let index_name = unsafe { helpers::parse_c_string(index_name)? }.map(str::to_string);
    let prepared = unsafe { prepare_vector_build(options)? };

    ds.with_mut(|dataset| {
        let columns = [column.as_str()];
        let mut builder =
            dataset.create_index_builder(&columns, IndexType::Vector, &prepared.params);
        if let Some(index_name) = index_name.clone() {
            builder = builder.name(index_name);
        }
        builder = builder.replace(replace).train(prepared.train);
        // `execute` is private in the pinned core, while `IntoFuture` is the
        // public commit path and performs the same commit-plus-metadata reload.
        block_on(std::future::IntoFuture::into_future(builder))
    })?;
    Ok(0)
}
