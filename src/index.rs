// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Index lifecycle C API: create vector / scalar indexes, drop, list.
//!
//! Index creation mutates the dataset under an exclusive write lock; existing
//! scanners that already cloned the inner Arc keep their snapshot view.

use std::ffi::{CString, c_char};

use lance_core::Result;
use lance_index::scalar::{BuiltinIndexType, ScalarIndexParams};
use lance_index::{DatasetIndexExt, IndexType};

use crate::dataset::LanceDataset;
use crate::error::{LanceErrorCode, ffi_try, set_last_error};
use crate::helpers;
use crate::runtime::block_on;

/// Scalar index type, matching the C enum `LanceScalarIndexType`.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceScalarIndexType {
    BTree = 1,
    Bitmap = 2,
    LabelList = 3,
    Inverted = 4,
}

impl LanceScalarIndexType {
    fn from_c(v: i32) -> Result<Self> {
        match v {
            1 => Ok(Self::BTree),
            2 => Ok(Self::Bitmap),
            3 => Ok(Self::LabelList),
            4 => Ok(Self::Inverted),
            _ => Err(lance_core::Error::InvalidInput {
                source: format!("invalid scalar index type: {}", v).into(),
                location: snafu::location!(),
            }),
        }
    }

    fn to_builtin(self) -> BuiltinIndexType {
        match self {
            Self::BTree => BuiltinIndexType::BTree,
            Self::Bitmap => BuiltinIndexType::Bitmap,
            Self::LabelList => BuiltinIndexType::LabelList,
            Self::Inverted => BuiltinIndexType::Inverted,
        }
    }

    fn to_index_type(self) -> IndexType {
        match self {
            Self::BTree => IndexType::BTree,
            Self::Bitmap => IndexType::Bitmap,
            Self::LabelList => IndexType::LabelList,
            Self::Inverted => IndexType::Inverted,
        }
    }
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
        return Err(lance_core::Error::InvalidInput {
            source: "dataset and column must not be NULL".into(),
            location: snafu::location!(),
        });
    }
    let ds = unsafe { &*dataset };
    let column_str = unsafe { helpers::parse_c_string(column)? }.ok_or_else(|| {
        lance_core::Error::InvalidInput {
            source: "column must not be empty".into(),
            location: snafu::location!(),
        }
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

/// Number of user indexes (excludes system indexes).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_index_count(dataset: *const LanceDataset) -> u64 {
    if dataset.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "dataset is NULL");
        return 0;
    }
    let ds = unsafe { &*dataset };
    let snap = ds.snapshot();
    match block_on(snap.load_indices()) {
        Ok(indices) => {
            crate::error::clear_last_error();
            indices
                .iter()
                .filter(|i| !lance_index::is_system_index(i))
                .count() as u64
        }
        Err(err) => {
            crate::error::set_lance_error(&err);
            0
        }
    }
}

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
        return Err(lance_core::Error::InvalidInput {
            source: "dataset and name must not be NULL".into(),
            location: snafu::location!(),
        });
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
    if dataset.is_null() {
        set_last_error(LanceErrorCode::InvalidArgument, "dataset is NULL");
        return std::ptr::null();
    }
    let ds = unsafe { &*dataset };
    let snap = ds.snapshot();

    let result = (|| -> Result<String> {
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
        Ok(format!("[{}]", entries.join(",")))
    })();

    match result {
        Ok(json) => {
            crate::error::clear_last_error();
            CString::new(json)
                .map(|s| s.into_raw() as *const c_char)
                .unwrap_or(std::ptr::null())
        }
        Err(err) => {
            crate::error::set_lance_error(&err);
            std::ptr::null()
        }
    }
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
/// * `num_bits`        — PQ/SQ; 0 → 8.
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

impl LanceMetricType {
    fn to_distance(self) -> DistanceType {
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
        Err(lance_core::Error::InvalidInput {
            source: format!("{} is required for this index type and must be > 0", name).into(),
            location: snafu::location!(),
        })
    } else {
        Ok(value)
    }
}

fn build_ivf(p: &LanceVectorIndexParams) -> Result<IvfBuildParams> {
    let num_partitions = require_field("num_partitions", p.num_partitions)? as usize;
    let mut ivf = IvfBuildParams::new(num_partitions);
    if p.max_iterations != 0 {
        ivf.max_iters = p.max_iterations as usize;
    }
    if p.sample_rate != 0 {
        ivf.sample_rate = p.sample_rate as usize;
    }
    Ok(ivf)
}

fn build_pq(p: &LanceVectorIndexParams) -> Result<PQBuildParams> {
    let num_sub_vectors = require_field("num_sub_vectors", p.num_sub_vectors)? as usize;
    let num_bits = if p.num_bits == 0 {
        8
    } else {
        p.num_bits as usize
    };
    let max_iters = if p.max_iterations == 0 {
        50
    } else {
        p.max_iterations as usize
    };
    Ok(PQBuildParams {
        num_sub_vectors,
        num_bits,
        max_iters,
        ..Default::default()
    })
}

fn build_sq(p: &LanceVectorIndexParams) -> SQBuildParams {
    let mut sq = SQBuildParams::default();
    if p.num_bits != 0 {
        sq.num_bits = p.num_bits as u16;
    }
    if p.sample_rate != 0 {
        sq.sample_rate = p.sample_rate as usize;
    }
    sq
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
    let metric = p.metric.to_distance();
    use LanceVectorIndexType::*;
    let core = match p.index_type {
        IvfFlat => {
            let ivf = build_ivf(p)?;
            LanceCoreVectorIndexParams::with_ivf_flat_params(metric, ivf)
        }
        IvfPq => {
            let ivf = build_ivf(p)?;
            let pq = build_pq(p)?;
            LanceCoreVectorIndexParams::with_ivf_pq_params(metric, ivf, pq)
        }
        IvfSq => {
            let ivf = build_ivf(p)?;
            let sq = build_sq(p);
            LanceCoreVectorIndexParams::with_ivf_sq_params(metric, ivf, sq)
        }
        IvfHnswFlat => {
            let ivf = build_ivf(p)?;
            let hnsw = build_hnsw(p)?;
            LanceCoreVectorIndexParams::ivf_hnsw(metric, ivf, hnsw)
        }
        IvfHnswPq => {
            let ivf = build_ivf(p)?;
            let hnsw = build_hnsw(p)?;
            let pq = build_pq(p)?;
            LanceCoreVectorIndexParams::with_ivf_hnsw_pq_params(metric, ivf, hnsw, pq)
        }
        IvfHnswSq => {
            let ivf = build_ivf(p)?;
            let hnsw = build_hnsw(p)?;
            let sq = build_sq(p);
            LanceCoreVectorIndexParams::with_ivf_hnsw_sq_params(metric, ivf, hnsw, sq)
        }
    };
    Ok(core)
}

/// Create a vector index on a column.
///
/// `params.index_type` selects the variant; the rest of the fields are
/// validated against that variant by [`build_vector_params`].
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

unsafe fn create_vector_index_inner(
    dataset: *mut LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    params: *const LanceVectorIndexParams,
    replace: bool,
) -> Result<i32> {
    if dataset.is_null() || column.is_null() || params.is_null() {
        return Err(lance_core::Error::InvalidInput {
            source: "dataset, column, and params must not be NULL".into(),
            location: snafu::location!(),
        });
    }
    let ds = unsafe { &*dataset };
    let column_str = unsafe { helpers::parse_c_string(column)? }.ok_or_else(|| {
        lance_core::Error::InvalidInput {
            source: "column must not be empty".into(),
            location: snafu::location!(),
        }
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
