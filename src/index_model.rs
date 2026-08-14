// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Standalone vector model training C API.

use std::collections::{HashMap, HashSet};
use std::ffi::c_char;
use std::ptr;
use std::slice;
use std::sync::Arc;

use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema, to_ffi};
use arrow_array::{Array, FixedSizeListArray};
use arrow_schema::{DataType, Field};
use lance::Dataset;
use lance::index::vector::ivf::build_ivf_model;
use lance::index::vector::pq::build_pq_model_in_fragments;
use lance::index::vector::utils::{get_vector_dim, get_vector_type, validate_distance_type_for};
use lance_core::{Error, Result};
use lance_index::progress::noop_progress;
use lance_index::vector::ivf::IvfBuildParams;
use lance_index::vector::ivf::storage::IvfModel;
use lance_index::vector::pq::PQBuildParams;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;
use crate::helpers;
use crate::index::LanceMetricType;
use crate::index_segment::{
    IVF_MODEL_ID_KEY, MODEL_DIMENSION_KEY, MODEL_KIND_KEY, MODEL_METRIC_KEY, PQ_BITS_KEY,
    PQ_SUB_VECTORS_KEY, borrow_model_array, fixed_size_list_model, model_provenance,
};
use crate::runtime::block_on;

fn invalid_input(message: impl Into<String>) -> Error {
    Error::invalid_input(message)
}

type CommonTrainerInput = (Dataset, String, usize, LanceMetricType, Option<Vec<u32>>);

unsafe fn parse_fragments(
    dataset: &Dataset,
    fragment_ids: *const u32,
    fragment_count: usize,
) -> Result<Option<Vec<u32>>> {
    let fragment_ids = match (fragment_ids.is_null(), fragment_count) {
        (true, 0) => return Ok(None),
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
            unsafe { slice::from_raw_parts(fragment_ids, count) }.to_vec()
        }
    };

    let mut unique = HashSet::with_capacity(fragment_ids.len());
    let existing: HashSet<u32> = dataset
        .get_fragments()
        .iter()
        .filter_map(|fragment| u32::try_from(fragment.id()).ok())
        .collect();
    for (position, fragment_id) in fragment_ids.iter().copied().enumerate() {
        if !unique.insert(fragment_id) {
            return Err(invalid_input(format!(
                "fragment_ids[{position}] is duplicate fragment id {fragment_id}"
            )));
        }
        if !existing.contains(&fragment_id) {
            return Err(invalid_input(format!(
                "fragment_ids[{position}]={fragment_id} does not exist in dataset version {}",
                dataset.version().version
            )));
        }
    }
    Ok(Some(fragment_ids))
}

unsafe fn parse_common(
    dataset: *const LanceDataset,
    column: *const c_char,
    metric: i32,
    fragment_ids: *const u32,
    fragment_count: usize,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> Result<CommonTrainerInput> {
    if dataset.is_null() || column.is_null() || out_array.is_null() || out_schema.is_null() {
        return Err(invalid_input(
            "dataset, column, out_array, and out_schema must not be NULL",
        ));
    }
    if !(unsafe { &*out_array }).is_released() || (unsafe { &*out_schema }).release.is_some() {
        return Err(invalid_input(
            "out_array and out_schema must be empty (release callbacks must be NULL)",
        ));
    }
    let metric = LanceMetricType::from_c(metric)?;
    let column = unsafe { helpers::parse_c_string(column)? }
        .filter(|value| !value.is_empty())
        .ok_or_else(|| invalid_input("column must not be NULL or empty"))?
        .to_string();
    let dataset = unsafe { &*dataset }.snapshot().as_ref().clone();
    let dim = get_vector_dim(dataset.schema(), &column)?;
    let (_, element_type) = get_vector_type(dataset.schema(), &column)?;
    if element_type != DataType::Float32 {
        return Err(invalid_input(format!(
            "column '{column}' must have Float32 vector elements, got {element_type:?}"
        )));
    }
    validate_distance_type_for(metric.to_distance(), &element_type)?;
    let fragments = unsafe { parse_fragments(&dataset, fragment_ids, fragment_count)? };
    Ok((dataset, column, dim, metric, fragments))
}

unsafe fn export_model(
    model: FixedSizeListArray,
    metadata: HashMap<String, String>,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> Result<i32> {
    let data_type = model.data_type().clone();
    let (array, generated_schema) = to_ffi(&model.into_data())?;
    drop(generated_schema);
    let field = Field::new("model", data_type, false).with_metadata(metadata);
    let schema = FFI_ArrowSchema::try_from(&field)?;
    unsafe {
        ptr::write_unaligned(out_array, array);
        ptr::write_unaligned(out_schema, schema);
    }
    Ok(0)
}

/// Train IVF centroids and export `FixedSizeList<Float32>[dimension]`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_train_ivf_model(
    dataset: *const LanceDataset,
    column: *const c_char,
    num_partitions: u32,
    metric: i32,
    fragment_ids: *const u32,
    fragment_count: usize,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> i32 {
    ffi_try!(
        unsafe {
            train_ivf_model_inner(
                dataset,
                column,
                num_partitions,
                metric,
                fragment_ids,
                fragment_count,
                out_array,
                out_schema,
            )
        },
        neg
    )
}

#[allow(clippy::too_many_arguments)]
unsafe fn train_ivf_model_inner(
    dataset: *const LanceDataset,
    column: *const c_char,
    num_partitions: u32,
    metric: i32,
    fragment_ids: *const u32,
    fragment_count: usize,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> Result<i32> {
    if num_partitions == 0 {
        return Err(invalid_input("num_partitions must be > 0, got 0"));
    }
    let (dataset, column, dim, metric, fragments) = unsafe {
        parse_common(
            dataset,
            column,
            metric,
            fragment_ids,
            fragment_count,
            out_array,
            out_schema,
        )?
    };
    let params = IvfBuildParams::new(num_partitions as usize);
    let model = block_on(build_ivf_model(
        &dataset,
        &column,
        dim,
        metric.to_distance(),
        &params,
        fragments.as_deref(),
        noop_progress(),
    ))?;
    let centroids = model
        .centroids_array()
        .cloned()
        .ok_or_else(|| Error::internal("IVF trainer returned no centroids"))?;
    if centroids.len() != num_partitions as usize || centroids.value_length() != dim as i32 {
        return Err(Error::internal(format!(
            "IVF trainer returned length {}, list_size {}; expected length {num_partitions}, list_size {dim}",
            centroids.len(),
            centroids.value_length()
        )));
    }
    let mut metadata = HashMap::new();
    metadata.insert(MODEL_KIND_KEY.to_string(), "ivf".to_string());
    metadata.insert(MODEL_METRIC_KEY.to_string(), (metric as i32).to_string());
    metadata.insert(MODEL_DIMENSION_KEY.to_string(), dim.to_string());
    metadata.insert(
        IVF_MODEL_ID_KEY.to_string(),
        uuid::Uuid::new_v4().to_string(),
    );
    unsafe { export_model(centroids, metadata, out_array, out_schema) }
}

/// Train a PQ codebook and export it as
/// `FixedSizeList<Float32>[dimension / num_sub_vectors]` with
/// `num_sub_vectors * 2^num_bits` rows. L2 and cosine train on IVF residuals;
/// DOT trains on raw vectors.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_train_pq_model(
    dataset: *const LanceDataset,
    column: *const c_char,
    num_sub_vectors: u32,
    num_bits: u32,
    metric: i32,
    fragment_ids: *const u32,
    fragment_count: usize,
    ivf_centroids: *mut FFI_ArrowArray,
    ivf_centroids_schema: *const FFI_ArrowSchema,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> i32 {
    ffi_try!(
        unsafe {
            train_pq_model_inner(
                dataset,
                column,
                num_sub_vectors,
                num_bits,
                metric,
                fragment_ids,
                fragment_count,
                ivf_centroids,
                ivf_centroids_schema,
                out_array,
                out_schema,
            )
        },
        neg
    )
}

#[allow(clippy::too_many_arguments)]
unsafe fn train_pq_model_inner(
    dataset: *const LanceDataset,
    column: *const c_char,
    num_sub_vectors: u32,
    num_bits: u32,
    metric: i32,
    fragment_ids: *const u32,
    fragment_count: usize,
    ivf_centroids: *mut FFI_ArrowArray,
    ivf_centroids_schema: *const FFI_ArrowSchema,
    out_array: *mut FFI_ArrowArray,
    out_schema: *mut FFI_ArrowSchema,
) -> Result<i32> {
    if num_sub_vectors == 0 {
        return Err(invalid_input("num_sub_vectors must be > 0, got 0"));
    }
    if !matches!(num_bits, 4 | 8) {
        return Err(invalid_input(format!(
            "num_bits must be 4 or 8 for Lance PQ indexes, got {num_bits}"
        )));
    }
    let (dataset, column, dim, metric, fragments) = unsafe {
        parse_common(
            dataset,
            column,
            metric,
            fragment_ids,
            fragment_count,
            out_array,
            out_schema,
        )?
    };
    let num_sub_vectors = num_sub_vectors as usize;
    if dim % num_sub_vectors != 0 {
        return Err(invalid_input(format!(
            "dimension {dim} must be divisible by num_sub_vectors {num_sub_vectors}"
        )));
    }
    let provenance = unsafe { model_provenance(ivf_centroids_schema, "ivf_centroids")? };
    if provenance.kind != "ivf" || provenance.metric != metric as i32 || provenance.dimension != dim
    {
        return Err(invalid_input(format!(
            "ivf_centroids provenance must be kind=ivf, metric={}, dimension={dim}; got kind={}, metric={}, dimension={}",
            metric as i32, provenance.kind, provenance.metric, provenance.dimension
        )));
    }
    let centroids =
        unsafe { borrow_model_array(ivf_centroids, ivf_centroids_schema, "ivf_centroids")? };
    let centroids = fixed_size_list_model(centroids, "ivf_centroids")?;
    if centroids.len() == 0
        || centroids.value_length() != dim as i32
        || centroids.value_type() != DataType::Float32
    {
        return Err(invalid_input(format!(
            "ivf_centroids must be a non-empty FixedSizeList<Float32> with list_size {dim}, got length {}, type {:?}",
            centroids.len(),
            centroids.data_type()
        )));
    }
    let ivf = IvfModel::new(centroids, None);
    let params = PQBuildParams::new(num_sub_vectors, num_bits as usize);
    let ivf_residual = match metric {
        LanceMetricType::L2 | LanceMetricType::Cosine => Some(&ivf),
        LanceMetricType::Dot | LanceMetricType::Hamming => None,
    };
    let model = block_on(build_pq_model_in_fragments(
        &dataset,
        &column,
        dim,
        metric.to_distance(),
        &params,
        ivf_residual,
        fragments.as_deref(),
    ))?;
    let subvector_dim = dim / num_sub_vectors;
    let values = model.codebook.values().clone();
    let item = Arc::new(Field::new("item", values.data_type().clone(), true));
    let codebook = FixedSizeListArray::try_new(item, subvector_dim as i32, values, None)?;
    let codewords = 1_usize
        .checked_shl(num_bits)
        .ok_or_else(|| invalid_input(format!("1 << num_bits ({num_bits}) overflows usize")))?;
    let expected_len = num_sub_vectors.checked_mul(codewords).ok_or_else(|| {
        invalid_input(format!(
            "num_sub_vectors {num_sub_vectors} * codewords {codewords} overflows usize"
        ))
    })?;
    if codebook.len() != expected_len {
        return Err(Error::internal(format!(
            "PQ trainer returned length {}; expected {expected_len}",
            codebook.len()
        )));
    }
    let mut metadata = HashMap::new();
    metadata.insert(MODEL_KIND_KEY.to_string(), "pq".to_string());
    metadata.insert(MODEL_METRIC_KEY.to_string(), (metric as i32).to_string());
    metadata.insert(MODEL_DIMENSION_KEY.to_string(), dim.to_string());
    metadata.insert(IVF_MODEL_ID_KEY.to_string(), provenance.ivf_id.to_string());
    metadata.insert(PQ_SUB_VECTORS_KEY.to_string(), num_sub_vectors.to_string());
    metadata.insert(PQ_BITS_KEY.to_string(), num_bits.to_string());
    unsafe { export_model(codebook, metadata, out_array, out_schema) }
}
