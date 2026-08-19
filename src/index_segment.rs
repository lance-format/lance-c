// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Distributed index segment build and metadata C API.

use std::collections::HashSet;
use std::ffi::{CStr, CString, c_char};
use std::ptr;
use std::slice;
use std::sync::Arc;

use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema, from_ffi, to_ffi};
use arrow_array::{Array, ArrayRef, FixedSizeListArray, make_array};
use arrow_schema::{DataType, Field};
use chrono::{DateTime, Utc};
use lance::Dataset;
use lance::index::DatasetIndexExt;
use lance_core::{Error, Result};
use lance_index::scalar::ScalarIndexParams;
use lance_table::format::{IndexMetadata, pb};
use prost::Message;
use uuid::Uuid;

use crate::dataset::LanceDataset;
use crate::error::{LanceErrorCode, clear_last_error, ffi_try, set_last_error};
use crate::helpers;
use crate::index::{
    LanceMetricType, LanceScalarIndexType, LanceVectorIndexParams, LanceVectorIndexType,
    build_vector_params_with_models,
};
use crate::runtime::block_on;

/// Options shared by scalar and vector index segment builders.
///
/// Arrow arrays are paired with their schemas.  A model is absent only when
/// both pointers in its pair are NULL.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LanceIndexSegmentBuildOptions {
    pub fragment_ids: *const u32,
    pub fragment_count: usize,
    pub index_uuid: *const u8,
    pub ivf_centroids: *mut FFI_ArrowArray,
    pub ivf_centroids_schema: *const FFI_ArrowSchema,
    pub pq_codebook: *mut FFI_ArrowArray,
    pub pq_codebook_schema: *const FFI_ArrowSchema,
    pub mode: i32,
}

/// Raw-discriminant vector parameters for the segment-builder ABI.
///
/// Unlike the legacy one-shot parameter struct, enum-like fields are i32 so
/// arbitrary C input can be validated before constructing Rust enums.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceVectorIndexSegmentParams {
    pub index_type: i32,
    pub metric: i32,
    pub num_partitions: u32,
    pub num_sub_vectors: u32,
    pub num_bits: u32,
    pub max_iterations: u32,
    pub hnsw_m: u32,
    pub hnsw_ef_construction: u32,
    pub sample_rate: u32,
}

impl LanceVectorIndexSegmentParams {
    fn parse(self) -> Result<LanceVectorIndexParams> {
        Ok(LanceVectorIndexParams {
            index_type: LanceVectorIndexType::from_c(self.index_type)?,
            metric: crate::index::LanceMetricType::from_c(self.metric)?,
            num_partitions: self.num_partitions,
            num_sub_vectors: self.num_sub_vectors,
            num_bits: self.num_bits,
            max_iterations: self.max_iterations,
            hnsw_m: self.hnsw_m,
            hnsw_ef_construction: self.hnsw_ef_construction,
            sample_rate: self.sample_rate,
        })
    }
}

/// Controls whether a segment builder trains models or uses supplied models.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceIndexSegmentBuildMode {
    Auto = 0,
    LocalTrain = 1,
    Precomputed = 2,
}

impl LanceIndexSegmentBuildMode {
    fn from_c(value: i32) -> Result<Self> {
        match value {
            0 => Ok(Self::Auto),
            1 => Ok(Self::LocalTrain),
            2 => Ok(Self::Precomputed),
            _ => Err(invalid_input(format!(
                "mode must be 0 (AUTO), 1 (LOCAL_TRAIN), or 2 (PRECOMPUTED); got {value}"
            ))),
        }
    }
}

struct ParsedBuildOptions {
    fragment_ids: Option<Vec<u32>>,
    index_uuid: Option<Uuid>,
    mode: LanceIndexSegmentBuildMode,
}

/// Opaque, single-use index segment builder.
pub struct LanceIndexSegmentBuilder {
    dataset: Dataset,
    column: String,
    index_name: Option<String>,
    kind: SegmentKind,
    fragment_ids: Option<Vec<u32>>,
    index_uuid: Option<Uuid>,
    executed: bool,
}

enum SegmentKind {
    Scalar {
        scalar_type: LanceScalarIndexType,
        params_json: Option<String>,
    },
    Vector {
        params: LanceVectorIndexParams,
        centroids: Option<Arc<FixedSizeListArray>>,
        codebook: Option<ArrayRef>,
    },
}

/// Opaque parsed index segment metadata.
pub struct LanceIndexSegmentMetadata {
    metadata: IndexMetadata,
    name: CString,
    index_details_type_url: Option<CString>,
    fragment_ids: Vec<u32>,
}

fn invalid_input(message: impl Into<String>) -> Error {
    lance_core::Error::invalid_input_source(message.into().into())
}

pub(crate) const MODEL_KIND_KEY: &str = "lance:index_model:kind";
pub(crate) const MODEL_METRIC_KEY: &str = "lance:index_model:metric";
pub(crate) const MODEL_DIMENSION_KEY: &str = "lance:index_model:dimension";
pub(crate) const IVF_MODEL_ID_KEY: &str = "lance:index_model:ivf_id";
pub(crate) const PQ_SUB_VECTORS_KEY: &str = "lance:index_model:num_sub_vectors";
pub(crate) const PQ_BITS_KEY: &str = "lance:index_model:num_bits";

#[derive(Clone, Debug)]
pub(crate) struct ModelProvenance {
    pub kind: String,
    pub metric: i32,
    pub dimension: usize,
    pub ivf_id: Uuid,
    pub num_sub_vectors: Option<u32>,
    pub num_bits: Option<u32>,
}

unsafe fn parse_options(
    options: *const LanceIndexSegmentBuildOptions,
    dataset: &Dataset,
    scalar: bool,
) -> Result<ParsedBuildOptions> {
    if options.is_null() {
        return Ok(ParsedBuildOptions {
            fragment_ids: None,
            index_uuid: None,
            mode: LanceIndexSegmentBuildMode::Auto,
        });
    }

    let options = unsafe { &*options };
    let fragment_ids = match (options.fragment_ids.is_null(), options.fragment_count) {
        (true, 0) => None,
        (true, count) => {
            return Err(invalid_input(format!(
                "fragment_ids is NULL but fragment_count is {count}"
            )));
        }
        (false, 0) => {
            return Err(invalid_input(
                "fragment_ids is non-NULL but fragment_count is 0",
            ));
        }
        (false, count) => {
            if count > isize::MAX as usize / std::mem::size_of::<u32>() {
                return Err(invalid_input(format!(
                    "fragment_count {count} exceeds the maximum addressable u32 slice length"
                )));
            }
            let ids = unsafe { slice::from_raw_parts(options.fragment_ids, count) }.to_vec();
            let mut unique = HashSet::with_capacity(ids.len());
            for (position, fragment_id) in ids.iter().copied().enumerate() {
                if !unique.insert(fragment_id) {
                    return Err(invalid_input(format!(
                        "fragment_ids[{position}] is duplicate fragment id {fragment_id}"
                    )));
                }
            }
            let existing: HashSet<u32> = dataset
                .get_fragments()
                .iter()
                .filter_map(|fragment| u32::try_from(fragment.id()).ok())
                .collect();
            for (position, fragment_id) in ids.iter().copied().enumerate() {
                if !existing.contains(&fragment_id) {
                    return Err(invalid_input(format!(
                        "fragment_ids[{position}]={fragment_id} does not exist in dataset version {}",
                        dataset.version().version
                    )));
                }
            }
            Some(ids)
        }
    };

    let ivf_pair_is_valid =
        options.ivf_centroids.is_null() == options.ivf_centroids_schema.is_null();
    if !ivf_pair_is_valid {
        return Err(invalid_input(
            "ivf_centroids and ivf_centroids_schema must both be NULL or both be non-NULL",
        ));
    }
    let pq_pair_is_valid = options.pq_codebook.is_null() == options.pq_codebook_schema.is_null();
    if !pq_pair_is_valid {
        return Err(invalid_input(
            "pq_codebook and pq_codebook_schema must both be NULL or both be non-NULL",
        ));
    }
    if scalar && (!options.ivf_centroids.is_null() || !options.pq_codebook.is_null()) {
        return Err(invalid_input(
            "ivf_centroids and pq_codebook are not valid for a scalar index segment",
        ));
    }
    let mode = LanceIndexSegmentBuildMode::from_c(options.mode)?;
    if scalar && mode == LanceIndexSegmentBuildMode::Precomputed {
        return Err(invalid_input(
            "mode PRECOMPUTED is not valid for a scalar index segment",
        ));
    }

    let index_uuid = if options.index_uuid.is_null() {
        None
    } else {
        let bytes: [u8; 16] = unsafe { slice::from_raw_parts(options.index_uuid, 16) }
            .try_into()
            .expect("slice has a fixed length of 16");
        Some(Uuid::from_bytes(bytes))
    };

    Ok(ParsedBuildOptions {
        fragment_ids,
        index_uuid,
        mode,
    })
}

/// Create a scalar index segment builder bound to the dataset's current snapshot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_builder_new_scalar(
    dataset: *const LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    index_type: i32,
    params_json: *const c_char,
    options: *const LanceIndexSegmentBuildOptions,
) -> *mut LanceIndexSegmentBuilder {
    ffi_try!(
        unsafe {
            new_scalar_builder_inner(
                dataset,
                column,
                index_name,
                index_type,
                params_json,
                options,
            )
        },
        null
    )
}

unsafe fn new_scalar_builder_inner(
    dataset: *const LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    index_type: i32,
    params_json: *const c_char,
    options: *const LanceIndexSegmentBuildOptions,
) -> Result<*mut LanceIndexSegmentBuilder> {
    if dataset.is_null() || column.is_null() {
        return Err(invalid_input("dataset and column must not be NULL"));
    }
    let column = unsafe { helpers::parse_c_string(column)? }
        .filter(|value| !value.is_empty())
        .ok_or_else(|| invalid_input("column must not be NULL or empty"))?
        .to_string();
    let index_name = unsafe { helpers::parse_c_string(index_name)? }.map(str::to_string);
    let params_json = unsafe { helpers::parse_c_string(params_json)? }.map(str::to_string);
    let scalar_type = LanceScalarIndexType::from_c(index_type)?;
    let dataset = unsafe { &*dataset }.snapshot().as_ref().clone();
    let parsed = unsafe { parse_options(options, &dataset, true)? };
    debug_assert_ne!(parsed.mode, LanceIndexSegmentBuildMode::Precomputed);

    Ok(Box::into_raw(Box::new(LanceIndexSegmentBuilder {
        dataset,
        column,
        index_name,
        kind: SegmentKind::Scalar {
            scalar_type,
            params_json,
        },
        fragment_ids: parsed.fragment_ids,
        index_uuid: parsed.index_uuid,
        executed: false,
    })))
}

/// Create a vector index segment builder bound to the dataset's current snapshot.
///
/// Non-NULL Arrow arrays and schemas are borrowed synchronously. The array
/// structs may be replaced but remain live and caller-owned after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_builder_new_vector(
    dataset: *const LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    params: *const LanceVectorIndexSegmentParams,
    options: *const LanceIndexSegmentBuildOptions,
) -> *mut LanceIndexSegmentBuilder {
    ffi_try!(
        unsafe { new_vector_builder_inner(dataset, column, index_name, params, options) },
        null
    )
}

unsafe fn validate_model_schema(schema: *const FFI_ArrowSchema, name: &str) -> Result<Field> {
    unsafe fn preflight(
        schema: *const FFI_ArrowSchema,
        path: &str,
        visited: &mut HashSet<usize>,
        depth: usize,
    ) -> Result<()> {
        if schema.is_null() {
            return Err(invalid_input(format!("{path} must not be NULL")));
        }
        if depth > 64 {
            return Err(invalid_input(format!(
                "{path} nesting depth exceeds the supported maximum of 64"
            )));
        }
        if !visited.insert(schema as usize) {
            return Err(invalid_input(format!(
                "{path} contains a schema pointer cycle"
            )));
        }
        let value = unsafe { &*schema };
        if value.release.is_none() || value.format.is_null() {
            return Err(invalid_input(format!(
                "{path} is uninitialized or already released"
            )));
        }
        if unsafe { CStr::from_ptr(value.format) }.to_str().is_err() {
            return Err(invalid_input(format!(
                "{path} format string is not valid UTF-8"
            )));
        }
        if !value.name.is_null() && unsafe { CStr::from_ptr(value.name) }.to_str().is_err() {
            return Err(invalid_input(format!(
                "{path} name string is not valid UTF-8"
            )));
        }
        let child_count = usize::try_from(value.n_children).map_err(|_| {
            invalid_input(format!(
                "{path}.n_children must be non-negative, got {}",
                value.n_children
            ))
        })?;
        if child_count > isize::MAX as usize / std::mem::size_of::<*mut FFI_ArrowSchema>() {
            return Err(invalid_input(format!(
                "{path}.n_children {child_count} exceeds the addressable pointer slice length"
            )));
        }
        if child_count > 0 && value.children.is_null() {
            return Err(invalid_input(format!(
                "{path}.children is NULL while n_children is {child_count}"
            )));
        }
        for index in 0..child_count {
            let child = unsafe { *value.children.add(index) };
            unsafe {
                preflight(
                    child,
                    &format!("{path}.children[{index}]"),
                    visited,
                    depth + 1,
                )?
            };
        }
        if !value.dictionary.is_null() {
            unsafe {
                preflight(
                    value.dictionary,
                    &format!("{path}.dictionary"),
                    visited,
                    depth + 1,
                )?
            };
        }
        Ok(())
    }

    let mut visited = HashSet::new();
    unsafe { preflight(schema, &format!("{name}_schema"), &mut visited, 0)? };
    let schema = unsafe { &*schema };
    DataType::try_from(schema).map_err(|error| {
        invalid_input(format!("{name}_schema is not a valid Arrow type: {error}"))
    })?;
    Field::try_from(schema).map_err(|error| {
        invalid_input(format!("{name}_schema is not a valid Arrow field: {error}"))
    })
}

pub(crate) unsafe fn model_provenance(
    schema: *const FFI_ArrowSchema,
    name: &str,
) -> Result<ModelProvenance> {
    let field = unsafe { validate_model_schema(schema, name)? };
    let metadata = field.metadata();
    let required = |key: &str| {
        metadata.get(key).ok_or_else(|| {
            invalid_input(format!(
                "{name}_schema metadata is missing required key '{key}'"
            ))
        })
    };
    let metric = required(MODEL_METRIC_KEY)?.parse().map_err(|error| {
        invalid_input(format!(
            "{name}_schema metadata '{MODEL_METRIC_KEY}' is invalid: {error}"
        ))
    })?;
    let dimension = required(MODEL_DIMENSION_KEY)?.parse().map_err(|error| {
        invalid_input(format!(
            "{name}_schema metadata '{MODEL_DIMENSION_KEY}' is invalid: {error}"
        ))
    })?;
    let ivf_id = Uuid::parse_str(required(IVF_MODEL_ID_KEY)?).map_err(|error| {
        invalid_input(format!(
            "{name}_schema metadata '{IVF_MODEL_ID_KEY}' is invalid: {error}"
        ))
    })?;
    let optional_u32 = |key: &str| -> Result<Option<u32>> {
        metadata
            .get(key)
            .map(|value| {
                value.parse().map_err(|error| {
                    invalid_input(format!(
                        "{name}_schema metadata '{key}' value '{value}' is invalid: {error}"
                    ))
                })
            })
            .transpose()
    };
    Ok(ModelProvenance {
        kind: required(MODEL_KIND_KEY)?.clone(),
        metric,
        dimension,
        ivf_id,
        num_sub_vectors: optional_u32(PQ_SUB_VECTORS_KEY)?,
        num_bits: optional_u32(PQ_BITS_KEY)?,
    })
}

unsafe fn import_model_array(
    array: FFI_ArrowArray,
    schema: *const FFI_ArrowSchema,
    name: &str,
) -> Result<ArrayRef> {
    // Arrow's schema conversion asserts on a NULL format pointer and expects
    // UTF-8. Validate the foreign schema before calling into arrow-rs so bad C
    // input is reported instead of aborting across the FFI boundary.
    unsafe { validate_model_schema(schema, name)? };
    let schema = unsafe { &*schema };
    let data = unsafe { from_ffi(array, schema) }
        .map_err(|error| invalid_input(format!("invalid {name} Arrow C data: {error}")))?;
    Ok(make_array(data))
}

/// Borrow a live Arrow C model and leave an equivalent live array in the
/// caller's slot. The returned ArrayRef owns independent Arrow buffer refs.
pub(crate) unsafe fn borrow_model_array(
    array: *mut FFI_ArrowArray,
    schema: *const FFI_ArrowSchema,
    name: &str,
) -> Result<ArrayRef> {
    if array.is_null() {
        return Err(invalid_input(format!("{name} must not be NULL")));
    }
    let field = unsafe { validate_model_schema(schema, name)? };
    let DataType::FixedSizeList(child_field, list_size) = field.data_type() else {
        return Err(invalid_input(format!(
            "{name}_schema must describe FixedSizeList, got {:?}",
            field.data_type()
        )));
    };
    if *list_size <= 0 || child_field.data_type() != &DataType::Float32 {
        return Err(invalid_input(format!(
            "{name}_schema must describe FixedSizeList<Float32> with positive list_size, got {:?}",
            field.data_type()
        )));
    }
    let array_ref = unsafe { &*array };
    if array_ref.is_released() {
        return Err(invalid_input(format!(
            "{name} is uninitialized or already released"
        )));
    }
    let validate_common = |value: &FFI_ArrowArray, path: &str| -> Result<()> {
        if value.length < 0 || value.offset < 0 {
            return Err(invalid_input(format!(
                "{path} length and offset must be non-negative; length={}, offset={}",
                value.length, value.offset
            )));
        }
        if value.null_count < -1 || value.null_count > value.length {
            return Err(invalid_input(format!(
                "{path}.null_count {} is invalid for length {}",
                value.null_count, value.length
            )));
        }
        if value.n_buffers < 0 || value.n_children < 0 {
            return Err(invalid_input(format!(
                "{path}.n_buffers and n_children must be non-negative; n_buffers={}, n_children={}",
                value.n_buffers, value.n_children
            )));
        }
        Ok(())
    };
    validate_common(array_ref, name)?;
    if array_ref.n_buffers != 1 || array_ref.buffers.is_null() {
        return Err(invalid_input(format!(
            "{name} FixedSizeList must have one buffer and a non-NULL buffers pointer; n_buffers={}",
            array_ref.n_buffers
        )));
    }
    if array_ref.n_children != 1 || array_ref.children.is_null() {
        return Err(invalid_input(format!(
            "{name} FixedSizeList must have one child and a non-NULL children pointer; n_children={}",
            array_ref.n_children
        )));
    }
    if !array_ref.dictionary.is_null() {
        return Err(invalid_input(format!(
            "{name} FixedSizeList dictionary must be NULL"
        )));
    }
    if array_ref.null_count > 0 && unsafe { *array_ref.buffers }.is_null() {
        return Err(invalid_input(format!(
            "{name} has null_count {} but no validity buffer",
            array_ref.null_count
        )));
    }
    let child_ptr = unsafe { *array_ref.children };
    if child_ptr.is_null() {
        return Err(invalid_input(format!("{name}.children[0] is NULL")));
    }
    let child = unsafe { &*child_ptr };
    if child.is_released() {
        return Err(invalid_input(format!(
            "{name}.children[0] is uninitialized or already released"
        )));
    }
    validate_common(child, &format!("{name}.children[0]"))?;
    if child.n_buffers != 2 || child.buffers.is_null() {
        return Err(invalid_input(format!(
            "{name}.children[0] Float32 must have two buffers and a non-NULL buffers pointer; n_buffers={}",
            child.n_buffers
        )));
    }
    if child.n_children != 0 || !child.dictionary.is_null() {
        return Err(invalid_input(format!(
            "{name}.children[0] must have no children or dictionary"
        )));
    }
    if child.null_count > 0 && unsafe { *child.buffers }.is_null() {
        return Err(invalid_input(format!(
            "{name}.children[0] has null_count {} but no validity buffer",
            child.null_count
        )));
    }
    let child_data_buffer = unsafe { *child.buffers.add(1) };
    if child.length > 0 && child_data_buffer.is_null() {
        return Err(invalid_input(format!(
            "{name}.children[0] has length {} but a NULL values buffer",
            child.length
        )));
    }
    let parent_offset = usize::try_from(array_ref.offset)
        .map_err(|_| invalid_input(format!("{name}.offset does not fit usize")))?;
    let parent_len = usize::try_from(array_ref.length)
        .map_err(|_| invalid_input(format!("{name}.length does not fit usize")))?;
    let list_size = usize::try_from(*list_size)
        .map_err(|_| invalid_input(format!("{name} list_size does not fit usize")))?;
    let required_child_len = parent_offset
        .checked_add(parent_len)
        .and_then(|length| length.checked_mul(list_size))
        .ok_or_else(|| {
            invalid_input(format!(
                "{name} (offset + length) * list_size overflows usize"
            ))
        })?;
    let child_offset = usize::try_from(child.offset)
        .map_err(|_| invalid_input(format!("{name}.children[0].offset does not fit usize")))?;
    let child_len = usize::try_from(child.length)
        .map_err(|_| invalid_input(format!("{name}.children[0].length does not fit usize")))?;
    if child_offset.checked_add(child_len).unwrap_or(0) < required_child_len {
        return Err(invalid_input(format!(
            "{name}.children[0] does not cover the parent range: child offset={}, length={}, required elements={required_child_len}",
            child.offset, child.length
        )));
    }
    let owned = unsafe { ptr::replace(array, FFI_ArrowArray::empty()) };
    let imported = unsafe { import_model_array(owned, schema, name)? };
    let (replacement, replacement_schema) = to_ffi(&imported.to_data())?;
    unsafe { ptr::write_unaligned(array, replacement) };
    drop(replacement_schema);
    Ok(imported)
}

pub(crate) fn fixed_size_list_model(array: ArrayRef, name: &str) -> Result<FixedSizeListArray> {
    let model = array
        .as_any()
        .downcast_ref::<FixedSizeListArray>()
        .cloned()
        .ok_or_else(|| {
            invalid_input(format!(
                "{name} must be FixedSizeList<Float32>, got {:?}",
                array.data_type()
            ))
        })?;
    if model.null_count() != 0 {
        return Err(invalid_input(format!(
            "{name} must not contain NULL lists; null_count is {}",
            model.null_count()
        )));
    }
    let list_size = usize::try_from(model.value_length()).map_err(|_| {
        invalid_input(format!(
            "{name} has invalid negative list_size {}",
            model.value_length()
        ))
    })?;
    let values_offset = model.offset().checked_mul(list_size).ok_or_else(|| {
        invalid_input(format!(
            "{name} offset {} * list_size {list_size} overflows usize",
            model.offset()
        ))
    })?;
    let values_len = model.len().checked_mul(list_size).ok_or_else(|| {
        invalid_input(format!(
            "{name} length {} * list_size {list_size} overflows usize",
            model.len()
        ))
    })?;
    let values = model.values().slice(values_offset, values_len);
    if values.null_count() != 0 {
        return Err(invalid_input(format!(
            "{name} values must not contain NULLs; null_count is {}",
            values.null_count()
        )));
    }
    let DataType::FixedSizeList(field, _) = model.data_type() else {
        unreachable!("downcast and data type must agree")
    };
    FixedSizeListArray::try_new(field.clone(), model.value_length(), values, None)
        .map_err(|error| invalid_input(format!("invalid {name}: {error}")))
}

unsafe fn new_vector_builder_inner(
    dataset: *const LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    params: *const LanceVectorIndexSegmentParams,
    options: *const LanceIndexSegmentBuildOptions,
) -> Result<*mut LanceIndexSegmentBuilder> {
    if dataset.is_null() || column.is_null() || params.is_null() {
        return Err(invalid_input(
            "dataset, column, and params must not be NULL",
        ));
    }
    let column = unsafe { helpers::parse_c_string(column)? }
        .filter(|value| !value.is_empty())
        .ok_or_else(|| invalid_input("column must not be NULL or empty"))?
        .to_string();
    let index_name = unsafe { helpers::parse_c_string(index_name)? }.map(str::to_string);
    let params = unsafe { *params }.parse()?;
    let dataset = unsafe { &*dataset }.snapshot().as_ref().clone();
    let parsed = unsafe { parse_options(options, &dataset, false)? };

    let (centroids, codebook, centroid_provenance, codebook_provenance) = if options.is_null() {
        (None, None, None, None)
    } else {
        let options = unsafe { &*options };
        let centroids = if options.ivf_centroids.is_null() {
            None
        } else {
            let provenance =
                unsafe { model_provenance(options.ivf_centroids_schema, "ivf_centroids")? };
            let model = fixed_size_list_model(
                unsafe {
                    borrow_model_array(
                        options.ivf_centroids,
                        options.ivf_centroids_schema,
                        "ivf_centroids",
                    )?
                },
                "ivf_centroids",
            )?;
            Some((model, provenance))
        };
        let codebook = if options.pq_codebook.is_null() {
            None
        } else {
            let provenance =
                unsafe { model_provenance(options.pq_codebook_schema, "pq_codebook")? };
            let model = fixed_size_list_model(
                unsafe {
                    borrow_model_array(
                        options.pq_codebook,
                        options.pq_codebook_schema,
                        "pq_codebook",
                    )?
                },
                "pq_codebook",
            )?;
            Some((model, provenance))
        };
        (
            centroids.as_ref().map(|(model, _)| model.clone()),
            codebook.as_ref().map(|(model, _)| model.clone()),
            centroids.map(|(_, provenance)| provenance),
            codebook.map(|(_, provenance)| provenance),
        )
    };

    let dim = lance::index::vector::utils::get_vector_dim(dataset.schema(), &column)?;
    match &centroid_provenance {
        Some(provenance)
            if provenance.kind != "ivf"
                || provenance.metric != params.metric as i32
                || provenance.dimension != dim =>
        {
            return Err(invalid_input(format!(
                "ivf_centroids provenance must be kind=ivf, metric={}, dimension={dim}; got kind={}, metric={}, dimension={}",
                params.metric as i32, provenance.kind, provenance.metric, provenance.dimension
            )));
        }
        _ => {}
    }
    if let Some(provenance) = &codebook_provenance {
        let expected_bits = if params.num_bits == 0 {
            8
        } else {
            params.num_bits
        };
        if provenance.kind != "pq"
            || provenance.metric != params.metric as i32
            || provenance.dimension != dim
            || provenance.num_sub_vectors != Some(params.num_sub_vectors)
            || provenance.num_bits != Some(expected_bits)
        {
            return Err(invalid_input(format!(
                "pq_codebook provenance does not match metric {}, dimension {dim}, num_sub_vectors {}, num_bits {expected_bits}",
                params.metric as i32, params.num_sub_vectors
            )));
        }
        if centroid_provenance.as_ref().map(|model| model.ivf_id) != Some(provenance.ivf_id) {
            return Err(invalid_input(
                "pq_codebook was not trained with the supplied ivf_centroids",
            ));
        }
    }
    if let Some(centroids) = &centroids {
        if centroids.value_length() != dim as i32
            || centroids.values().data_type() != &DataType::Float32
        {
            return Err(invalid_input(format!(
                "ivf_centroids must have type FixedSizeList<Float32>[{dim}], got {:?}",
                centroids.data_type()
            )));
        }
        if centroids.len() != params.num_partitions as usize {
            return Err(invalid_input(format!(
                "ivf_centroids length {} does not match num_partitions {}",
                centroids.len(),
                params.num_partitions
            )));
        }
    }

    let is_pq = matches!(
        params.index_type,
        LanceVectorIndexType::IvfPq | LanceVectorIndexType::IvfHnswPq
    );
    let effective_num_bits = if params.num_bits == 0 {
        8
    } else {
        params.num_bits
    };
    if is_pq && !matches!(effective_num_bits, 4 | 8) {
        return Err(invalid_input(format!(
            "num_bits must be 4 or 8 for Lance PQ indexes, got {effective_num_bits}"
        )));
    }
    if !is_pq && codebook.is_some() {
        return Err(invalid_input(format!(
            "pq_codebook is not valid for vector index type {:?}",
            params.index_type
        )));
    }
    if is_pq && (centroids.is_some() != codebook.is_some()) {
        return Err(invalid_input(
            "precomputed PQ segment builds require both ivf_centroids and pq_codebook",
        ));
    }
    let has_models = centroids.is_some() || codebook.is_some();
    match parsed.mode {
        LanceIndexSegmentBuildMode::Auto => {}
        LanceIndexSegmentBuildMode::LocalTrain if has_models => {
            return Err(invalid_input(
                "mode LOCAL_TRAIN does not accept precomputed model arrays",
            ));
        }
        LanceIndexSegmentBuildMode::Precomputed if !has_models => {
            return Err(invalid_input(
                "mode PRECOMPUTED requires precomputed model arrays",
            ));
        }
        LanceIndexSegmentBuildMode::LocalTrain | LanceIndexSegmentBuildMode::Precomputed => {}
    }

    let codebook = if let Some(codebook) = codebook {
        let num_sub_vectors = usize::try_from(params.num_sub_vectors)
            .map_err(|_| invalid_input("num_sub_vectors does not fit usize"))?;
        if num_sub_vectors == 0 || dim % num_sub_vectors != 0 {
            return Err(invalid_input(format!(
                "dimension {dim} must be divisible by num_sub_vectors {num_sub_vectors}"
            )));
        }
        let num_bits = effective_num_bits;
        if !matches!(num_bits, 4 | 8) {
            return Err(invalid_input(format!(
                "num_bits must be 4 or 8 for Lance PQ indexes, got {num_bits}"
            )));
        }
        let codewords = 1_usize
            .checked_shl(num_bits)
            .ok_or_else(|| invalid_input(format!("1 << num_bits ({num_bits}) overflows usize")))?;
        let expected_len = num_sub_vectors.checked_mul(codewords).ok_or_else(|| {
            invalid_input(format!(
                "num_sub_vectors {num_sub_vectors} * codewords {codewords} overflows usize"
            ))
        })?;
        let subvector_dim = dim / num_sub_vectors;
        if codebook.len() != expected_len
            || codebook.value_length() != subvector_dim as i32
            || codebook.values().data_type() != &DataType::Float32
        {
            return Err(invalid_input(format!(
                "pq_codebook must be FixedSizeList<Float32> with length {expected_len} and list_size {subvector_dim}, got length {}, type {:?}",
                codebook.len(),
                codebook.data_type()
            )));
        }
        Some(codebook.values().clone())
    } else {
        None
    };

    let centroids = centroids.map(Arc::new);
    // Construct now so all parameter/model mismatches fail at the FFI boundary.
    let _ = build_vector_params_with_models(&params, centroids.clone(), codebook.clone())?;

    // TODO(upstream-lance): Remove this fail-fast once Lance's distributed
    // vector-index path reconstructs a supplied PQ codebook with an L2
    // ProductQuantizer, matching the ordinary full-dataset path. Pinned Lance
    // revision e934cc2c rewraps supplied codebooks with DistanceType::Dot in
    // `make_global_pq`, which silently switches PQ code assignment away from
    // the L2 contract shared by full-dataset builds and index readers.
    if matches!(
        params.index_type,
        LanceVectorIndexType::IvfPq | LanceVectorIndexType::IvfHnswPq
    ) && params.metric == LanceMetricType::Dot
        && codebook.is_some()
        && let Some(fragment_ids) = parsed.fragment_ids.as_ref()
    {
        // `fragment_ids` are unique and known to exist (checked in
        // `parse_options`), so set inequality here is exactly an effective
        // strict subset, mirroring core `effective_vector_fragments`.
        let all_fragment_ids: HashSet<u32> = dataset
            .get_fragments()
            .iter()
            .filter_map(|fragment| u32::try_from(fragment.id()).ok())
            .collect();
        let selected_fragment_ids: HashSet<u32> = fragment_ids.iter().copied().collect();
        if selected_fragment_ids != all_fragment_ids {
            return Err(invalid_input(format!(
                "pq_codebook is supplied for metric=DOT, index_type={:?}, mode={:?}, and an effective strict fragment subset ({} of {} fragments): pinned Lance revision e934cc2c reconstructs the supplied codebook with a DOT ProductQuantizer in the distributed build path (make_global_pq), silently breaking the L2 PQ-assignment contract; cover the full dataset in one segment (pass NULL fragment_ids or list every fragment) or wait for upstream Lance DOT support",
                params.index_type,
                parsed.mode,
                selected_fragment_ids.len(),
                all_fragment_ids.len()
            )));
        }
    }

    Ok(Box::into_raw(Box::new(LanceIndexSegmentBuilder {
        dataset,
        column,
        index_name,
        kind: SegmentKind::Vector {
            params,
            centroids,
            codebook,
        },
        fragment_ids: parsed.fragment_ids,
        index_uuid: parsed.index_uuid,
        executed: false,
    })))
}

/// Build the segment artifacts without committing metadata to the dataset manifest.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_builder_execute_uncommitted(
    builder: *mut LanceIndexSegmentBuilder,
    out_bytes: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    ffi_try!(
        unsafe { execute_uncommitted_inner(builder, out_bytes, out_len) },
        neg
    )
}

unsafe fn execute_uncommitted_inner(
    builder: *mut LanceIndexSegmentBuilder,
    out_bytes: *mut *mut u8,
    out_len: *mut usize,
) -> Result<i32> {
    if builder.is_null() {
        return Err(invalid_input("builder must not be NULL"));
    }
    let builder = unsafe { &mut *builder };
    if builder.executed {
        return Err(invalid_input(
            "index segment builder is single-use and has already been executed",
        ));
    }
    builder.executed = true;
    if out_bytes.is_null() || out_len.is_null() {
        return Err(invalid_input("out_bytes and out_len must not be NULL"));
    }

    let columns = [builder.column.as_str()];
    let metadata = match &builder.kind {
        SegmentKind::Scalar {
            scalar_type,
            params_json,
        } => {
            let mut params = ScalarIndexParams::for_builtin(scalar_type.to_builtin());
            params.params.clone_from(params_json);
            let mut core_builder = builder.dataset.create_index_builder(
                &columns,
                scalar_type.to_index_type(),
                &params,
            );
            if let Some(name) = builder.index_name.clone() {
                core_builder = core_builder.name(name);
            }
            if let Some(fragment_ids) = builder.fragment_ids.clone() {
                core_builder = core_builder.fragments(fragment_ids);
            }
            if let Some(index_uuid) = builder.index_uuid {
                core_builder = core_builder.index_uuid(index_uuid);
            }
            block_on(core_builder.execute_uncommitted())?
        }
        SegmentKind::Vector {
            params,
            centroids,
            codebook,
        } => {
            let core_params =
                build_vector_params_with_models(params, centroids.clone(), codebook.clone())?;
            let mut core_builder = builder.dataset.create_index_builder(
                &columns,
                lance_index::IndexType::Vector,
                &core_params,
            );
            if let Some(name) = builder.index_name.clone() {
                core_builder = core_builder.name(name);
            }
            if let Some(fragment_ids) = builder.fragment_ids.clone() {
                core_builder = core_builder.fragments(fragment_ids);
            }
            if let Some(index_uuid) = builder.index_uuid {
                core_builder = core_builder.index_uuid(index_uuid);
            }
            // Core's train=false means "create an empty index".  Model presence
            // itself controls whether IVF/PQ training is skipped.
            block_on(core_builder.train(true).execute_uncommitted())?
        }
    };
    let bytes = pb::IndexMetadata::from(&metadata).encode_to_vec();
    let allocation = unsafe { libc::malloc(bytes.len()) }.cast::<u8>();
    if allocation.is_null() {
        return Err(Error::internal(format!(
            "failed to allocate {} metadata bytes",
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

/// Free bytes returned by `lance_index_segment_builder_execute_uncommitted`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_free_bytes(bytes: *mut u8) {
    unsafe { libc::free(bytes.cast()) };
}

/// Free an index segment builder. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_builder_free(builder: *mut LanceIndexSegmentBuilder) {
    if !builder.is_null() {
        unsafe { drop(Box::from_raw(builder)) };
    }
}

/// Parse a protobuf-encoded Lance `IndexMetadata` value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_parse(
    bytes: *const u8,
    len: usize,
    out_metadata: *mut *mut LanceIndexSegmentMetadata,
) -> i32 {
    ffi_try!(
        unsafe { parse_metadata_inner(bytes, len, out_metadata) },
        neg
    )
}

unsafe fn parse_metadata_inner(
    bytes: *const u8,
    len: usize,
    out_metadata: *mut *mut LanceIndexSegmentMetadata,
) -> Result<i32> {
    if bytes.is_null() || len == 0 || out_metadata.is_null() {
        return Err(invalid_input(format!(
            "bytes must be non-NULL, len must be > 0, and out_metadata must be non-NULL; bytes={bytes:p}, len={len}, out_metadata={out_metadata:p}"
        )));
    }
    if len > isize::MAX as usize {
        return Err(invalid_input(format!(
            "len {len} exceeds the maximum addressable byte slice length"
        )));
    }
    let proto = pb::IndexMetadata::decode(unsafe { slice::from_raw_parts(bytes, len) })
        .map_err(|error| invalid_input(format!("invalid IndexMetadata protobuf: {error}")))?;
    if let Some(created_at) = proto.created_at {
        let created_at = i64::try_from(created_at).map_err(|_| {
            invalid_input(format!(
                "IndexMetadata created_at {created_at} exceeds i64::MAX milliseconds"
            ))
        })?;
        if DateTime::<Utc>::from_timestamp_millis(created_at).is_none() {
            return Err(invalid_input(format!(
                "IndexMetadata created_at {created_at} is outside chrono's supported range"
            )));
        }
    }
    if let Some(index_version) = proto.index_version.filter(|version| *version < 0) {
        return Err(invalid_input(format!(
            "IndexMetadata index_version must be >= 0, got {index_version}"
        )));
    }
    if let Some((position, field_id)) = proto
        .fields
        .iter()
        .copied()
        .enumerate()
        .find(|(_, field_id)| *field_id < 0)
    {
        return Err(invalid_input(format!(
            "IndexMetadata fields[{position}] must be >= 0, got {field_id}"
        )));
    }
    let metadata = IndexMetadata::try_from(proto)?;
    let name = CString::new(metadata.name.as_str())
        .map_err(|_| invalid_input("index metadata name contains an embedded NUL byte"))?;
    let index_details_type_url = metadata
        .index_details
        .as_ref()
        .map(|details| CString::new(details.type_url.as_str()))
        .transpose()
        .map_err(|_| invalid_input("index details type_url contains an embedded NUL byte"))?;
    let fragment_ids = metadata
        .fragment_bitmap
        .as_ref()
        .map(|bitmap| bitmap.iter().collect())
        .unwrap_or_default();
    let handle = LanceIndexSegmentMetadata {
        metadata,
        name,
        index_details_type_url,
        fragment_ids,
    };
    unsafe { ptr::write(out_metadata, Box::into_raw(Box::new(handle))) };
    Ok(0)
}

/// Copy the metadata UUID as 16 raw RFC 4122 bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_uuid(
    metadata: *const LanceIndexSegmentMetadata,
    out_uuid: *mut u8,
) -> i32 {
    ffi_try!(unsafe { metadata_uuid_inner(metadata, out_uuid) }, neg)
}

unsafe fn metadata_uuid_inner(
    metadata: *const LanceIndexSegmentMetadata,
    out_uuid: *mut u8,
) -> Result<i32> {
    if metadata.is_null() || out_uuid.is_null() {
        return Err(invalid_input("metadata and out_uuid must not be NULL"));
    }
    unsafe {
        ptr::copy_nonoverlapping((*metadata).metadata.uuid.as_bytes().as_ptr(), out_uuid, 16)
    };
    Ok(0)
}

/// Return the metadata name, borrowed until the metadata handle is freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_name(
    metadata: *const LanceIndexSegmentMetadata,
) -> *const c_char {
    if metadata.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "metadata is NULL");
        return ptr::null();
    }
    clear_last_error();
    unsafe { (*metadata).name.as_ptr() }
}

/// Return the dataset version recorded in the metadata.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_dataset_version(
    metadata: *const LanceIndexSegmentMetadata,
) -> u64 {
    if metadata.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "metadata is NULL");
        return 0;
    }
    clear_last_error();
    unsafe { (*metadata).metadata.dataset_version }
}

/// Return the physical index version recorded in the metadata.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_index_version(
    metadata: *const LanceIndexSegmentMetadata,
) -> i32 {
    if metadata.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "metadata is NULL");
        return -1;
    }
    clear_last_error();
    unsafe { (*metadata).metadata.index_version }
}

/// Return the concrete scalar/vector index enum value, or -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_index_type(
    metadata: *const LanceIndexSegmentMetadata,
) -> i32 {
    match unsafe { metadata_index_type_inner(metadata) } {
        Ok(index_type) => {
            clear_last_error();
            index_type
        }
        Err(error) => {
            crate::error::set_lance_error(&error);
            -1
        }
    }
}

unsafe fn metadata_index_type_inner(metadata: *const LanceIndexSegmentMetadata) -> Result<i32> {
    if metadata.is_null() {
        return Err(invalid_input("metadata must not be NULL"));
    }
    let details = unsafe { &*metadata }
        .metadata
        .index_details
        .as_ref()
        .ok_or_else(|| invalid_input("index metadata does not contain index_details"))?;
    let type_url = details.type_url.as_str();
    if type_url.ends_with("BTreeIndexDetails") {
        return Ok(1);
    }
    if type_url.ends_with("BitmapIndexDetails") {
        return Ok(2);
    }
    if type_url.ends_with("LabelListIndexDetails") {
        return Ok(3);
    }
    if type_url.ends_with("InvertedIndexDetails") {
        return Ok(4);
    }
    if type_url.ends_with("VectorIndexDetails") {
        use lance_index::pb::vector_index_details::Compression;

        let details = lance_index::pb::VectorIndexDetails::decode(details.value.as_slice())
            .map_err(|error| {
                invalid_input(format!("invalid VectorIndexDetails protobuf: {error}"))
            })?;
        let hnsw = details.hnsw_index_config.is_some();
        return match (hnsw, details.compression) {
            (false, None | Some(Compression::Flat(_))) => Ok(101),
            (false, Some(Compression::Sq(_))) => Ok(102),
            (false, Some(Compression::Pq(_))) => Ok(103),
            (true, Some(Compression::Sq(_))) => Ok(104),
            (true, Some(Compression::Pq(_))) => Ok(105),
            (true, None | Some(Compression::Flat(_))) => Ok(106),
            (_, Some(Compression::Rq(_))) => Err(Error::not_supported(
                "Rabit-quantized vector metadata has no LanceVectorIndexType value",
            )),
        };
    }
    Err(Error::not_supported(format!(
        "unsupported index_details type_url '{type_url}'"
    )))
}

/// Return the protobuf Any type URL, borrowed until the metadata handle is freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_index_details_type_url(
    metadata: *const LanceIndexSegmentMetadata,
) -> *const c_char {
    if metadata.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "metadata is NULL");
        return ptr::null();
    }
    let Some(type_url) = (unsafe { &(*metadata).index_details_type_url }) else {
        set_last_error(
            LanceErrorCode::NotFound,
            "index metadata does not contain index_details",
        );
        return ptr::null();
    };
    clear_last_error();
    type_url.as_ptr()
}

/// Return the number of indexed field IDs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_field_count(
    metadata: *const LanceIndexSegmentMetadata,
) -> usize {
    if metadata.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "metadata is NULL");
        return 0;
    }
    clear_last_error();
    unsafe { (*metadata).metadata.fields.len() }
}

/// Copy indexed field IDs in metadata order.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_field_ids(
    metadata: *const LanceIndexSegmentMetadata,
    out_field_ids: *mut i32,
    capacity: usize,
    out_count: *mut usize,
) -> i32 {
    ffi_try!(
        unsafe { metadata_field_ids_inner(metadata, out_field_ids, capacity, out_count) },
        neg
    )
}

unsafe fn metadata_field_ids_inner(
    metadata: *const LanceIndexSegmentMetadata,
    out_field_ids: *mut i32,
    capacity: usize,
    out_count: *mut usize,
) -> Result<i32> {
    if metadata.is_null() || out_count.is_null() {
        return Err(invalid_input("metadata and out_count must not be NULL"));
    }
    let field_ids = unsafe { &(*metadata).metadata.fields };
    if capacity < field_ids.len() {
        return Err(invalid_input(format!(
            "capacity {capacity} is smaller than field_count {}",
            field_ids.len()
        )));
    }
    if !field_ids.is_empty() && out_field_ids.is_null() {
        return Err(invalid_input(format!(
            "out_field_ids is NULL but field_count is {}",
            field_ids.len()
        )));
    }
    if !field_ids.is_empty() {
        unsafe { ptr::copy_nonoverlapping(field_ids.as_ptr(), out_field_ids, field_ids.len()) };
    }
    unsafe { ptr::write(out_count, field_ids.len()) };
    Ok(0)
}

/// Return the number of covered fragment IDs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_fragment_count(
    metadata: *const LanceIndexSegmentMetadata,
) -> usize {
    if metadata.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "metadata is NULL");
        return 0;
    }
    clear_last_error();
    unsafe { (*metadata).fragment_ids.len() }
}

/// Copy covered fragment IDs in ascending order.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_fragment_ids(
    metadata: *const LanceIndexSegmentMetadata,
    out_fragment_ids: *mut u32,
    capacity: usize,
    out_count: *mut usize,
) -> i32 {
    ffi_try!(
        unsafe { metadata_fragment_ids_inner(metadata, out_fragment_ids, capacity, out_count) },
        neg
    )
}

unsafe fn metadata_fragment_ids_inner(
    metadata: *const LanceIndexSegmentMetadata,
    out_fragment_ids: *mut u32,
    capacity: usize,
    out_count: *mut usize,
) -> Result<i32> {
    if metadata.is_null() || out_count.is_null() {
        return Err(invalid_input("metadata and out_count must not be NULL"));
    }
    let fragment_ids = unsafe { &(*metadata).fragment_ids };
    if capacity < fragment_ids.len() {
        return Err(invalid_input(format!(
            "capacity {capacity} is smaller than fragment_count {}",
            fragment_ids.len()
        )));
    }
    if !fragment_ids.is_empty() && out_fragment_ids.is_null() {
        return Err(invalid_input(format!(
            "out_fragment_ids is NULL but fragment_count is {}",
            fragment_ids.len()
        )));
    }
    if !fragment_ids.is_empty() {
        unsafe {
            ptr::copy_nonoverlapping(fragment_ids.as_ptr(), out_fragment_ids, fragment_ids.len())
        };
    }
    unsafe { ptr::write(out_count, fragment_ids.len()) };
    Ok(0)
}

/// Free parsed index metadata. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_free(
    metadata: *mut LanceIndexSegmentMetadata,
) {
    if !metadata.is_null() {
        unsafe { drop(Box::from_raw(metadata)) };
    }
}
