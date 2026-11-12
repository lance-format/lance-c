// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Distributed index segment build and metadata C API.

use std::collections::HashSet;
use std::ffi::{CStr, CString, c_char, c_void};
use std::ptr;
use std::slice;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

use arrow::ffi::{FFI_ArrowArray, FFI_ArrowSchema, from_ffi, to_ffi};
use arrow_array::{Array, ArrayRef, FixedSizeListArray, make_array};
use arrow_schema::{DataType, Field};
use chrono::{DateTime, Utc};
use lance::Dataset;
use lance::index::DatasetIndexExt;
use lance::index::vector::{StageParams, VectorIndexParams as LanceCoreVectorIndexParams};
use lance_core::{Error, Result};
use lance_index::IndexType;
use lance_index::progress::{IndexBuildProgress, noop_progress};
use lance_index::scalar::ScalarIndexParams;
use lance_table::format::{IndexMetadata, pb};
use prost::Message;
use uuid::Uuid;

use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
use crate::helpers;
use crate::index::{
    LanceMetricType, LanceScalarIndexOptions, LanceScalarIndexType, LanceVectorIndexOptions,
    LanceVectorIndexParams, LanceVectorIndexType, build_vector_params_with_models,
    prepare_vector_build, scalar_train,
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
    progress: Option<SendIndexBuildProgressCallback>,
    executed: bool,
}

enum SegmentKind {
    Scalar {
        scalar_type: LanceScalarIndexType,
        params_json: Option<String>,
        train: bool,
    },
    Vector {
        params: LanceCoreVectorIndexParams,
        train: bool,
    },
}

/// Fragment selection and optional UUID for the additive vector segment
/// builder. A NULL optional pointer preserves native `None`; a non-NULL scope
/// with `fragment_count == 0` represents an explicit empty selection.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceVectorIndexSegmentScope {
    pub fragment_ids: *const u32,
    pub fragment_count: usize,
    pub index_uuid: *const u8,
}

/// Event code delivered to `LanceIndexBuildProgressCallback`; mirrors
/// `LanceIndexBuildProgressEvent` in lance.h.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IndexBuildProgressEvent {
    /// A build stage began.
    StageStart = 0,
    /// Work units completed within the active stage.
    StageProgress = 1,
    /// The active stage finished.
    StageComplete = 2,
}

/// Index build progress callback bridged to a C function pointer.
///
/// The public C type is an `Option` of the raw function pointer so NULL can be
/// rejected at the setter; once stored here the callback is known non-NULL.
pub type LanceIndexBuildProgressCallback = Option<
    unsafe extern "C" fn(
        callback_ctx: *mut c_void,
        event: i32,
        stage: *const c_char,
        total: u64,
        unit: *const c_char,
        completed: u64,
    ),
>;

/// Shared retirement gate for the C progress callback bridge.
///
/// Lance core clones the installed progress handle into spawned worker tasks
/// (e.g. the inverted index's `tokenize_docs` workers), and error paths can
/// drop their `JoinHandle`s before the workers finish, so a detached clone
/// can outlive the build that installed it. The gate turns callback
/// retirement from a documentation contract into an enforced boundary: once
/// `retire` returns, no callback invocation is in flight and every later
/// entry becomes a no-op, so a detached clone can never touch the raw
/// `callback`/`ctx` after the owning blocking build call has returned.
#[derive(Debug, Default)]
struct ProgressCallbackGate {
    /// Set by `retire`; entries that observe it become no-ops.
    retired: AtomicBool,
    /// Number of C callback invocations currently executing.
    in_flight: AtomicUsize,
}

impl ProgressCallbackGate {
    /// Admit one callback invocation, or report that the gate is retired.
    ///
    /// The increment is ordered before the `retired` check (both SeqCst), so
    /// an entry racing with `retire` either observes `retired` and backs out,
    /// or is counted in `in_flight` and therefore awaited by `retire`'s
    /// drain loop.
    fn enter(&self) -> bool {
        self.in_flight.fetch_add(1, Ordering::SeqCst);
        if self.retired.load(Ordering::SeqCst) {
            self.in_flight.fetch_sub(1, Ordering::SeqCst);
            return false;
        }
        true
    }

    fn exit(&self) {
        self.in_flight.fetch_sub(1, Ordering::SeqCst);
    }

    /// Disable new entries and wait for in-flight invocations to drain.
    ///
    /// A callback that blocks (violating its documented non-blocking
    /// contract) stalls this drain rather than causing a use-after-free.
    fn retire(&self) {
        self.retired.store(true, Ordering::SeqCst);
        while self.in_flight.load(Ordering::SeqCst) != 0 {
            std::thread::yield_now();
        }
    }
}

/// Retires the shared progress gate on drop, covering the success, error,
/// and panic exits of `execute_uncommitted_inner`.
struct ProgressRetireGuard(Arc<ProgressCallbackGate>);

impl Drop for ProgressRetireGuard {
    fn drop(&mut self) {
        self.0.retire();
    }
}

/// Callback bridge that retires when the blocking native call returns.
pub(crate) struct ProgressCallbackBridge {
    callback: Arc<SendIndexBuildProgressCallback>,
    _retire: ProgressRetireGuard,
}

impl ProgressCallbackBridge {
    pub(crate) fn new(
        callback: unsafe extern "C" fn(
            callback_ctx: *mut c_void,
            event: i32,
            stage: *const c_char,
            total: u64,
            unit: *const c_char,
            completed: u64,
        ),
        ctx: *mut c_void,
    ) -> Self {
        let callback = Arc::new(SendIndexBuildProgressCallback {
            callback,
            ctx,
            gate: Arc::new(ProgressCallbackGate::default()),
        });
        let retire = ProgressRetireGuard(Arc::clone(&callback.gate));
        Self {
            callback,
            _retire: retire,
        }
    }

    pub(crate) fn progress(&self) -> Arc<dyn IndexBuildProgress> {
        self.callback.clone()
    }
}

#[derive(Debug, Clone)]
struct SendIndexBuildProgressCallback {
    callback: unsafe extern "C" fn(
        callback_ctx: *mut c_void,
        event: i32,
        stage: *const c_char,
        total: u64,
        unit: *const c_char,
        completed: u64,
    ),
    ctx: *mut c_void,
    /// Shared with every clone core hands to worker tasks; retired before the
    /// owning `execute_uncommitted` call returns.
    gate: Arc<ProgressCallbackGate>,
}

// SAFETY: The C API requires the callback and its context to remain valid and
// safe to invoke until the owning blocking build call returns, and the shared
// gate guarantees no invocation is in flight — and no new one can start —
// once that boundary is reached, so no invocation can touch the raw context
// after retirement. Certain build stages report progress concurrently from
// parallel worker tasks, so the callback may be invoked concurrently; the C
// contract requires it to be thread-safe.
unsafe impl Send for SendIndexBuildProgressCallback {}
unsafe impl Sync for SendIndexBuildProgressCallback {}

/// Build a `CString` for a stage or unit name without panicking.
///
/// Lance core never emits interior NUL bytes, but the FFI contract is
/// panic-free, so any that appear are stripped rather than aborting the
/// process. The strip removes every NUL byte, so `CString::new` cannot fail;
/// the `map_err` below is defensive-only and unreachable.
fn progress_c_string(value: &str) -> Result<CString> {
    let cleaned = if value.contains('\0') {
        value.replace('\0', "")
    } else {
        value.to_owned()
    };
    CString::new(cleaned)
        .map_err(|_| Error::internal("index build progress stage/unit contained an interior NUL"))
}

#[async_trait::async_trait]
impl IndexBuildProgress for SendIndexBuildProgressCallback {
    async fn stage_start(&self, stage: &str, total: Option<u64>, unit: &str) -> Result<()> {
        // Strings are built before entering the gate so the defensive `?`
        // paths cannot leak an in-flight count; after `enter` the only code
        // is the FFI call, so `exit` always runs.
        let stage = progress_c_string(stage)?;
        let unit = progress_c_string(unit)?;
        if !self.gate.enter() {
            return Ok(());
        }
        unsafe {
            (self.callback)(
                self.ctx,
                IndexBuildProgressEvent::StageStart as i32,
                stage.as_ptr(),
                total.unwrap_or(0),
                unit.as_ptr(),
                0,
            )
        };
        self.gate.exit();
        Ok(())
    }

    async fn stage_progress(&self, stage: &str, completed: u64) -> Result<()> {
        let stage = progress_c_string(stage)?;
        if !self.gate.enter() {
            return Ok(());
        }
        unsafe {
            (self.callback)(
                self.ctx,
                IndexBuildProgressEvent::StageProgress as i32,
                stage.as_ptr(),
                0,
                c"".as_ptr(),
                completed,
            )
        };
        self.gate.exit();
        Ok(())
    }

    async fn stage_complete(&self, stage: &str) -> Result<()> {
        let stage = progress_c_string(stage)?;
        if !self.gate.enter() {
            return Ok(());
        }
        unsafe {
            (self.callback)(
                self.ctx,
                IndexBuildProgressEvent::StageComplete as i32,
                stage.as_ptr(),
                0,
                c"".as_ptr(),
                0,
            )
        };
        self.gate.exit();
        Ok(())
    }
}

/// Presence-aware timestamp split into whole Unix seconds and nanoseconds.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LanceOptionalTimestamp {
    pub is_set: bool,
    pub unix_seconds: i64,
    pub nanos: u32,
}

/// Opaque parsed index segment metadata.
pub struct LanceIndexSegmentMetadata {
    metadata: IndexMetadata,
    name: CString,
    index_details_type_url: Option<CString>,
    fragment_ids: Vec<u32>,
    file_paths: Vec<CString>,
}

impl LanceIndexSegmentMetadata {
    pub(crate) fn native_metadata(&self) -> &IndexMetadata {
        &self.metadata
    }
}

pub(crate) fn metadata_handle(metadata: IndexMetadata) -> Result<*mut LanceIndexSegmentMetadata> {
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
    let file_paths = metadata
        .files
        .as_ref()
        .map(|files| {
            files
                .iter()
                .map(|file| {
                    CString::new(file.path.as_str()).map_err(|_| {
                        invalid_input("index metadata file path contains an embedded NUL byte")
                    })
                })
                .collect::<Result<Vec<_>>>()
        })
        .transpose()?
        .unwrap_or_default();
    Ok(Box::into_raw(Box::new(LanceIndexSegmentMetadata {
        metadata,
        name,
        index_details_type_url,
        fragment_ids,
        file_paths,
    })))
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
    _dataset: &Dataset,
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
        (false, 0) => Some(Vec::new()),
        (false, count) => {
            if count > isize::MAX as usize / std::mem::size_of::<u32>() {
                return Err(invalid_input(format!(
                    "fragment_count {count} exceeds the maximum addressable u32 slice length"
                )));
            }
            Some(unsafe { slice::from_raw_parts(options.fragment_ids, count) }.to_vec())
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
                true,
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
    train: bool,
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
            train,
        },
        fragment_ids: parsed.fragment_ids,
        index_uuid: parsed.index_uuid,
        progress: None,
        executed: false,
    })))
}

/// Create a scalar index segment builder with native train control.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_builder_new_scalar_with_options(
    dataset: *const LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    index_type: i32,
    params_json: *const c_char,
    scalar_options: *const LanceScalarIndexOptions,
    options: *const LanceIndexSegmentBuildOptions,
) -> *mut LanceIndexSegmentBuilder {
    ffi_try!(
        unsafe {
            new_scalar_builder_with_options_inner(
                dataset,
                column,
                index_name,
                index_type,
                params_json,
                scalar_options,
                options,
            )
        },
        ptr::null_mut()
    )
}

#[allow(clippy::too_many_arguments)]
unsafe fn new_scalar_builder_with_options_inner(
    dataset: *const LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    index_type: i32,
    params_json: *const c_char,
    scalar_options: *const LanceScalarIndexOptions,
    options: *const LanceIndexSegmentBuildOptions,
) -> Result<*mut LanceIndexSegmentBuilder> {
    let train = scalar_train(scalar_options)?;
    unsafe {
        new_scalar_builder_inner(
            dataset,
            column,
            index_name,
            index_type,
            params_json,
            options,
            train,
        )
    }
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
    let DataType::FixedSizeList(_, list_size) = field.data_type() else {
        return Err(invalid_input(format!(
            "{name}_schema must describe FixedSizeList, got {:?}",
            field.data_type()
        )));
    };
    if *list_size <= 0 {
        return Err(invalid_input(format!(
            "{name}_schema must describe FixedSizeList with positive list_size, got {:?}",
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
            "{name}.children[0] primitive values must have two buffers and a non-NULL buffers pointer; n_buffers={}",
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
                "{name} must be FixedSizeList, got {:?}",
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
    // Construct now so all supported parameter/model mismatches fail while
    // creating the builder. Fragment validation itself stays in Lance core.
    let core_params = build_vector_params_with_models(&params, centroids, codebook)?;

    validate_precomputed_pq_fragment_scope(&dataset, &core_params, parsed.fragment_ids.as_deref())?;

    Ok(Box::into_raw(Box::new(LanceIndexSegmentBuilder {
        dataset,
        column,
        index_name,
        kind: SegmentKind::Vector {
            params: core_params,
            train: true,
        },
        fragment_ids: parsed.fragment_ids,
        index_uuid: parsed.index_uuid,
        progress: None,
        executed: false,
    })))
}

// The selected core's distributed path reconstructs supplied PQ codebooks with
// the query metric. Index readers and ordinary builds use L2 PQ assignment,
// so preserve the existing restriction for DOT models on fragment subsets
// until the core's make_global_pq uses the same assignment metric.
fn validate_precomputed_pq_fragment_scope(
    dataset: &Dataset,
    params: &LanceCoreVectorIndexParams,
    fragment_ids: Option<&[u32]>,
) -> Result<()> {
    let has_codebook = params
        .stages
        .iter()
        .any(|stage| matches!(stage, StageParams::PQ(pq) if pq.codebook.is_some()));
    if params.metric_type == LanceMetricType::Dot.to_distance()
        && has_codebook
        && let Some(fragment_ids) = fragment_ids
    {
        let all_fragment_ids: HashSet<u32> = dataset
            .get_fragments()
            .iter()
            .filter_map(|fragment| u32::try_from(fragment.id()).ok())
            .collect();
        let selected_fragment_ids: HashSet<u32> = fragment_ids.iter().copied().collect();
        if selected_fragment_ids != all_fragment_ids {
            return Err(invalid_input(format!(
                "pq_codebook with metric=DOT and an effective strict fragment subset ({} of {} fragments) is unsupported on Lance revision 40f15ed1. Its distributed make_global_pq path reconstructs the supplied codebook with a DOT ProductQuantizer, breaking the L2 PQ-assignment contract. Cover the full dataset in one segment by passing NULL fragment_ids or every fragment.",
                selected_fragment_ids.len(),
                all_fragment_ids.len()
            )));
        }
    }
    Ok(())
}

unsafe fn parse_vector_segment_scope(
    scope: *const LanceVectorIndexSegmentScope,
) -> Result<(Option<Vec<u32>>, Option<Uuid>)> {
    if scope.is_null() {
        return Ok((None, None));
    }
    let scope = unsafe { &*scope };
    let fragment_ids = match (scope.fragment_ids.is_null(), scope.fragment_count) {
        (true, 0) => None,
        (true, count) => {
            return Err(invalid_input(format!(
                "scope.fragment_ids is NULL but scope.fragment_count is {count}"
            )));
        }
        (false, 0) => Some(Vec::new()),
        (false, count) => {
            if count > isize::MAX as usize / std::mem::size_of::<u32>() {
                return Err(invalid_input(format!(
                    "scope.fragment_count {count} exceeds the maximum addressable u32 slice length"
                )));
            }
            Some(unsafe { slice::from_raw_parts(scope.fragment_ids, count) }.to_vec())
        }
    };
    let index_uuid = if scope.index_uuid.is_null() {
        None
    } else {
        let bytes: [u8; 16] = unsafe { slice::from_raw_parts(scope.index_uuid, 16) }
            .try_into()
            .expect("slice has a fixed length of 16");
        Some(Uuid::from_bytes(bytes))
    };
    Ok((fragment_ids, index_uuid))
}

/// Create a vector segment builder from additive native options.
///
/// The builder copies strings, maps, paths, fragment IDs and UUIDs at creation.
/// Arrow models are borrowed synchronously and leave a live caller-owned
/// equivalent in place. The dataset is snapshotted and need not outlive the
/// builder.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_builder_new_vector_with_options(
    dataset: *const LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    options: *const LanceVectorIndexOptions,
    scope: *const LanceVectorIndexSegmentScope,
) -> *mut LanceIndexSegmentBuilder {
    ffi_try!(
        unsafe {
            new_vector_builder_with_options_inner(dataset, column, index_name, options, scope)
        },
        null
    )
}

unsafe fn new_vector_builder_with_options_inner(
    dataset: *const LanceDataset,
    column: *const c_char,
    index_name: *const c_char,
    options: *const LanceVectorIndexOptions,
    scope: *const LanceVectorIndexSegmentScope,
) -> Result<*mut LanceIndexSegmentBuilder> {
    if dataset.is_null() || column.is_null() || options.is_null() {
        return Err(invalid_input(
            "dataset, column, and options must not be NULL",
        ));
    }
    let column = unsafe { helpers::parse_c_string(column)? }
        .filter(|value| !value.is_empty())
        .ok_or_else(|| invalid_input("column must not be NULL or empty"))?
        .to_string();
    let index_name = unsafe { helpers::parse_c_string(index_name)? }.map(str::to_string);
    let prepared = unsafe { prepare_vector_build(options)? };
    let (fragment_ids, index_uuid) = unsafe { parse_vector_segment_scope(scope)? };
    let dataset = unsafe { &*dataset }.snapshot().as_ref().clone();
    if prepared.train {
        validate_precomputed_pq_fragment_scope(
            &dataset,
            &prepared.params,
            fragment_ids.as_deref(),
        )?;
    }

    Ok(Box::into_raw(Box::new(LanceIndexSegmentBuilder {
        dataset,
        column,
        index_name,
        kind: SegmentKind::Vector {
            params: prepared.params,
            train: prepared.train,
        },
        fragment_ids,
        index_uuid,
        progress: None,
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
    // Retire the shared progress gate before this call returns — on success,
    // error, and panic paths alike — so a detached core worker holding a
    // progress clone can no longer enter the C callback afterwards.
    let _progress_retire = builder
        .progress
        .as_ref()
        .map(|progress| ProgressRetireGuard(progress.gate.clone()));
    let result = match &builder.kind {
        SegmentKind::Scalar {
            scalar_type,
            params_json,
            train,
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
            if let Some(progress) = builder.progress.clone() {
                core_builder = core_builder.progress(Arc::new(progress));
            }
            block_on(core_builder.train(*train).execute_uncommitted())
        }
        SegmentKind::Vector { params, train } => {
            let mut core_builder = builder.dataset.create_index_builder(
                &columns,
                lance_index::IndexType::Vector,
                params,
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
            if let Some(progress) = builder.progress.clone() {
                core_builder = core_builder.progress(Arc::new(progress));
            }
            block_on(core_builder.train(*train).execute_uncommitted())
        }
    };
    let metadata = result?;
    unsafe { export_index_metadata(&metadata, out_bytes, out_len)? };
    Ok(0)
}

unsafe fn export_index_metadata(
    metadata: &IndexMetadata,
    out_bytes: *mut *mut u8,
    out_len: *mut usize,
) -> Result<()> {
    let bytes = pb::IndexMetadata::from(metadata).encode_to_vec();
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
    Ok(())
}

/// Install (or replace) the index-build progress callback for a segment
/// builder.
///
/// The callback is invoked from lance-c's internal tokio runtime worker
/// threads while `lance_index_segment_builder_execute_uncommitted` runs, and
/// only while it runs: lance-c disables and drains the callback through a
/// shared retirement gate before `execute_uncommitted` returns (including on
/// error), so a core worker task detached by an error path can never enter
/// the callback afterwards. The callback and `callback_ctx` must therefore
/// remain valid until `execute_uncommitted` returns. Certain stages report
/// progress concurrently from parallel worker tasks, so the callback must be
/// thread-safe and reentrant, must be non-blocking, and must not call back
/// into any `lance_*` function. It is invoked without a panic guard: it must
/// return normally, because unwinding or throwing across this boundary can
/// abort the host process. Progress reporting is advisory and cannot affect
/// the build outcome or abort the build.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_builder_set_progress_callback(
    builder: *mut LanceIndexSegmentBuilder,
    callback: LanceIndexBuildProgressCallback,
    callback_ctx: *mut c_void,
) -> i32 {
    ffi_try!(
        unsafe { set_progress_callback_inner(builder, callback, callback_ctx) },
        neg
    )
}

unsafe fn set_progress_callback_inner(
    builder: *mut LanceIndexSegmentBuilder,
    callback: LanceIndexBuildProgressCallback,
    callback_ctx: *mut c_void,
) -> Result<i32> {
    if builder.is_null() {
        return Err(invalid_input("builder must not be NULL"));
    }
    let Some(callback) = callback else {
        return Err(invalid_input("progress callback must not be NULL"));
    };
    let builder = unsafe { &mut *builder };
    if builder.executed {
        return Err(invalid_input(
            "progress callback must be set before the builder is executed",
        ));
    }
    builder.progress = Some(SendIndexBuildProgressCallback {
        callback,
        ctx: callback_ctx,
        // A replacement gets a fresh gate: the previous callback was never
        // shared with a build, so there is nothing to retire.
        gate: Arc::new(ProgressCallbackGate::default()),
    });
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
        swallow_unwind("lance_index_segment_builder_free", || unsafe {
            drop(Box::from_raw(builder));
        });
    }
}

/// Commit previously built uncommitted index segments as one logical index.
///
/// All segments are committed in a single dataset version. Validation of the
/// segment set (distinct UUIDs, disjoint fragment coverage, consistent index
/// details) is performed by the Lance core; replacement of existing same-name
/// segments is automatic and coverage-driven.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_commit_index_segments(
    dataset: *mut LanceDataset,
    index_name: *const c_char,
    column: *const c_char,
    segment_metadata_bytes: *const *const u8,
    segment_metadata_lens: *const usize,
    segment_count: usize,
) -> i32 {
    ffi_try!(
        unsafe {
            commit_index_segments_inner(
                dataset,
                index_name,
                column,
                segment_metadata_bytes,
                segment_metadata_lens,
                segment_count,
            )
        },
        neg
    )
}

unsafe fn commit_index_segments_inner(
    dataset: *mut LanceDataset,
    index_name: *const c_char,
    column: *const c_char,
    segment_metadata_bytes: *const *const u8,
    segment_metadata_lens: *const usize,
    segment_count: usize,
) -> Result<i32> {
    if dataset.is_null() || index_name.is_null() || column.is_null() {
        return Err(invalid_input(
            "dataset, index_name, and column must not be NULL",
        ));
    }
    let index_name = unsafe { helpers::parse_c_string(index_name)? }
        .filter(|value| !value.is_empty())
        .ok_or_else(|| invalid_input("index_name must not be NULL or empty"))?;
    let column = unsafe { helpers::parse_c_string(column)? }
        .filter(|value| !value.is_empty())
        .ok_or_else(|| invalid_input("column must not be NULL or empty"))?;
    if segment_count == 0 {
        return Err(invalid_input(
            "segment_count must be > 0; at least one index segment is required to commit an index",
        ));
    }
    if segment_metadata_bytes.is_null() || segment_metadata_lens.is_null() {
        return Err(invalid_input(format!(
            "segment_metadata_bytes and segment_metadata_lens must not be NULL when segment_count is {segment_count}"
        )));
    }
    if segment_count > isize::MAX as usize / std::mem::size_of::<*const u8>() {
        return Err(invalid_input(format!(
            "segment_count {segment_count} exceeds the maximum addressable pointer slice length"
        )));
    }
    let bytes_array = unsafe { slice::from_raw_parts(segment_metadata_bytes, segment_count) };
    let lens_array = unsafe { slice::from_raw_parts(segment_metadata_lens, segment_count) };
    let mut segments = Vec::with_capacity(segment_count);
    for (position, (&bytes, &len)) in bytes_array.iter().zip(lens_array.iter()).enumerate() {
        if bytes.is_null() || len == 0 {
            return Err(invalid_input(format!(
                "segment_metadata_bytes[{position}] must be non-NULL and segment_metadata_lens[{position}] must be > 0; bytes={bytes:p}, len={len}"
            )));
        }
        if len > isize::MAX as usize {
            return Err(invalid_input(format!(
                "segment_metadata_lens[{position}]={len} exceeds the maximum addressable byte slice length"
            )));
        }
        let metadata = decode_segment_metadata(unsafe { slice::from_raw_parts(bytes, len) })
            .map_err(|error| {
                invalid_input(format!("segment_metadata_bytes[{position}]: {error}"))
            })?;
        segments.push(metadata);
    }

    let ds = unsafe { &*dataset };
    ds.with_mut(|dataset| {
        block_on(dataset.commit_existing_index_segments(index_name, column, segments))
    })?;
    Ok(0)
}

/// Merge distributed Inverted index files into a single root artifact.
///
/// This writes and merges index files only. It does not commit a dataset
/// version; publication requires the separate native CreateIndex operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_merge_index_metadata(
    dataset: *const LanceDataset,
    index_uuid: *const u8,
    index_type: *const c_char,
    has_batch_readhead: bool,
    batch_readhead: usize,
    callback: LanceIndexBuildProgressCallback,
    callback_ctx: *mut c_void,
) -> i32 {
    ffi_try!(
        unsafe {
            merge_index_metadata_inner(
                dataset,
                index_uuid,
                index_type,
                has_batch_readhead,
                batch_readhead,
                callback,
                callback_ctx,
            )
        },
        neg
    )
}

#[allow(clippy::too_many_arguments)]
unsafe fn merge_index_metadata_inner(
    dataset: *const LanceDataset,
    index_uuid: *const u8,
    index_type: *const c_char,
    has_batch_readhead: bool,
    batch_readhead: usize,
    callback: LanceIndexBuildProgressCallback,
    callback_ctx: *mut c_void,
) -> Result<i32> {
    if dataset.is_null() || index_uuid.is_null() {
        return Err(invalid_input("dataset and index_uuid must not be NULL"));
    }
    let index_type = unsafe { helpers::parse_c_string(index_type)? }
        .filter(|value| !value.is_empty())
        .ok_or_else(|| invalid_input("index_type must not be NULL or empty"))?;
    let index_type = IndexType::try_from(index_type)?;
    let uuid = Uuid::from_bytes(
        unsafe { slice::from_raw_parts(index_uuid, 16) }
            .try_into()
            .expect("fixed-length UUID slice"),
    );
    let snapshot = unsafe { &*dataset }.snapshot();
    let bridge = callback.map(|callback| ProgressCallbackBridge::new(callback, callback_ctx));
    let progress = bridge
        .as_ref()
        .map(ProgressCallbackBridge::progress)
        .unwrap_or_else(noop_progress);
    block_on(snapshot.merge_index_metadata(
        &uuid,
        index_type,
        has_batch_readhead.then_some(batch_readhead),
        progress,
    ))?;
    Ok(0)
}

/// Merge existing uncommitted physical segments into one uncommitted segment.
///
/// Native Lance validates UUID uniqueness, coverage, type compatibility,
/// current-snapshot replay and staleness rules. The returned bytes still need
/// `lance_dataset_commit_index_segments` to publish them.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_merge_index_segments(
    dataset: *const LanceDataset,
    segment_metadata_bytes: *const *const u8,
    segment_metadata_lens: *const usize,
    segment_count: usize,
    out_bytes: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    ffi_try!(
        unsafe {
            merge_index_segments_inner(
                dataset,
                segment_metadata_bytes,
                segment_metadata_lens,
                segment_count,
                out_bytes,
                out_len,
            )
        },
        neg
    )
}

unsafe fn merge_index_segments_inner(
    dataset: *const LanceDataset,
    segment_metadata_bytes: *const *const u8,
    segment_metadata_lens: *const usize,
    segment_count: usize,
    out_bytes: *mut *mut u8,
    out_len: *mut usize,
) -> Result<i32> {
    if dataset.is_null() || out_bytes.is_null() || out_len.is_null() {
        return Err(invalid_input(
            "dataset, out_bytes, and out_len must not be NULL",
        ));
    }
    let mut segments = Vec::new();
    if segment_count > 0 {
        if segment_metadata_bytes.is_null() || segment_metadata_lens.is_null() {
            return Err(invalid_input(format!(
                "segment_metadata_bytes and segment_metadata_lens must not be NULL when segment_count is {segment_count}"
            )));
        }
        if segment_count > isize::MAX as usize / std::mem::size_of::<*const u8>() {
            return Err(invalid_input(format!(
                "segment_count {segment_count} exceeds the maximum addressable pointer slice length"
            )));
        }
        let byte_ptrs = unsafe { slice::from_raw_parts(segment_metadata_bytes, segment_count) };
        let lens = unsafe { slice::from_raw_parts(segment_metadata_lens, segment_count) };
        for (position, (&bytes, &len)) in byte_ptrs.iter().zip(lens.iter()).enumerate() {
            if bytes.is_null() || len == 0 {
                return Err(invalid_input(format!(
                    "segment_metadata_bytes[{position}] must be non-NULL and segment_metadata_lens[{position}] must be > 0; bytes={bytes:p}, len={len}"
                )));
            }
            if len > isize::MAX as usize {
                return Err(invalid_input(format!(
                    "segment_metadata_lens[{position}]={len} exceeds the maximum addressable byte slice length"
                )));
            }
            segments.push(
                decode_segment_metadata(unsafe { slice::from_raw_parts(bytes, len) }).map_err(
                    |error| invalid_input(format!("segment_metadata_bytes[{position}]: {error}")),
                )?,
            );
        }
    }

    let snapshot = unsafe { &*dataset }.snapshot();
    let merged = block_on(snapshot.merge_existing_index_segments(segments))?;
    unsafe { export_index_metadata(&merged, out_bytes, out_len)? };
    Ok(0)
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

/// Decode protobuf-encoded `IndexMetadata` bytes into the table-format type,
/// rejecting values whose ranges cannot be represented safely.
fn decode_segment_metadata(bytes: &[u8]) -> Result<IndexMetadata> {
    let proto = pb::IndexMetadata::decode(bytes)
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
    IndexMetadata::try_from(proto)
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
    let metadata = decode_segment_metadata(unsafe { slice::from_raw_parts(bytes, len) })?;
    let handle = metadata_handle(metadata)?;
    unsafe { ptr::write(out_metadata, handle) };
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
    ffi_try!(
        (|| -> Result<*const c_char> {
            if metadata.is_null() {
                return Err(invalid_input("metadata is NULL"));
            }
            Ok(unsafe { (*metadata).name.as_ptr() })
        })(),
        ptr::null()
    )
}

/// Return the dataset version recorded in the metadata.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_dataset_version(
    metadata: *const LanceIndexSegmentMetadata,
) -> u64 {
    ffi_try!(
        (|| -> Result<u64> {
            if metadata.is_null() {
                return Err(invalid_input("metadata is NULL"));
            }
            Ok(unsafe { (*metadata).metadata.dataset_version })
        })(),
        0
    )
}

/// Return the physical index version recorded in the metadata.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_index_version(
    metadata: *const LanceIndexSegmentMetadata,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if metadata.is_null() {
                return Err(invalid_input("metadata is NULL"));
            }
            Ok(unsafe { (*metadata).metadata.index_version })
        })(),
        neg
    )
}

/// Return the concrete scalar/vector index enum value, or -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_index_type(
    metadata: *const LanceIndexSegmentMetadata,
) -> i32 {
    ffi_try!(unsafe { metadata_index_type_inner(metadata) }, neg)
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
    if let Some(scalar_type) = LanceScalarIndexType::from_details_type_url(type_url) {
        return Ok(scalar_type as i32);
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
            (false, Some(Compression::Rq(_))) => Ok(107),
            (true, Some(Compression::Sq(_))) => Ok(104),
            (true, Some(Compression::Pq(_))) => Ok(105),
            (true, None | Some(Compression::Flat(_))) => Ok(106),
            (true, Some(Compression::Rq(_))) => Err(Error::not_supported(
                "IVF_HNSW_RQ vector metadata is not supported by the C index type enum",
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
    ffi_try!(
        (|| -> Result<*const c_char> {
            if metadata.is_null() {
                return Err(invalid_input("metadata is NULL"));
            }
            let type_url = unsafe { &(*metadata).index_details_type_url }
                .as_ref()
                .ok_or_else(|| {
                    Error::index_not_found("index metadata does not contain index_details")
                })?;
            Ok(type_url.as_ptr())
        })(),
        ptr::null()
    )
}

/// Return the number of indexed field IDs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_field_count(
    metadata: *const LanceIndexSegmentMetadata,
) -> usize {
    ffi_try!(
        (|| -> Result<usize> {
            if metadata.is_null() {
                return Err(invalid_input("metadata is NULL"));
            }
            Ok(unsafe { (*metadata).metadata.fields.len() })
        })(),
        0
    )
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
    ffi_try!(
        (|| -> Result<usize> {
            if metadata.is_null() {
                return Err(invalid_input("metadata is NULL"));
            }
            Ok(unsafe { (*metadata).fragment_ids.len() })
        })(),
        0
    )
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

/// Clone parsed index metadata into an independent handle.
///
/// The clone copies the complete in-memory native value, including optional
/// `index_details`, `fragment_bitmap`, `created_at`, `base_id`, and `files`.
/// It does not re-encode through protobuf. Safe to call with NULL; that returns
/// NULL with an error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_clone(
    metadata: *const LanceIndexSegmentMetadata,
) -> *mut LanceIndexSegmentMetadata {
    ffi_try!(
        (|| -> Result<*mut LanceIndexSegmentMetadata> {
            if metadata.is_null() {
                return Err(invalid_input("metadata must not be NULL"));
            }
            metadata_handle(unsafe { (*metadata).metadata.clone() })
        })(),
        ptr::null_mut()
    )
}

/// Clone parsed index metadata with caller-supplied fragment coverage.
///
/// When `has_fragment_ids` is false, the source `fragment_bitmap` option is
/// copied unchanged and `fragment_ids`/`fragment_count` are ignored. When true,
/// the clone's coverage becomes `Some` of the caller's IDs; a zero count maps
/// to `Some(empty)`, and a positive count requires a non-NULL pointer. The
/// native bitmap sorts the IDs and treats them as a set. Every other field is
/// copied exactly, including in-memory optionals; no coverage, overlap, or
/// index-family validation is performed here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_clone_with_fragment_ids(
    metadata: *const LanceIndexSegmentMetadata,
    has_fragment_ids: bool,
    fragment_ids: *const u32,
    fragment_count: usize,
) -> *mut LanceIndexSegmentMetadata {
    ffi_try!(
        (|| -> Result<*mut LanceIndexSegmentMetadata> {
            if metadata.is_null() {
                return Err(invalid_input("metadata must not be NULL"));
            }
            let mut cloned = unsafe { (*metadata).metadata.clone() };
            if has_fragment_ids {
                if fragment_count > isize::MAX as usize / std::mem::size_of::<u32>() {
                    return Err(invalid_input(format!(
                        "fragment_count {fragment_count} exceeds the maximum addressable uint32_t slice length"
                    )));
                }
                if fragment_count > 0 && fragment_ids.is_null() {
                    return Err(invalid_input(format!(
                        "fragment_ids must not be NULL when fragment_count is {fragment_count}"
                    )));
                }
                let ids = if fragment_count == 0 {
                    Vec::new()
                } else {
                    unsafe { slice::from_raw_parts(fragment_ids, fragment_count) }.to_vec()
                };
                cloned.fragment_bitmap = Some(ids.into_iter().collect());
            }
            metadata_handle(cloned)
        })(),
        ptr::null_mut()
    )
}

/// Encode parsed index metadata as protobuf-encoded `IndexMetadata` bytes.
///
/// Encoding is an explicit request to serialize through the native protobuf
/// representation. Successful output is library-owned and must be released
/// with `lance_free_bytes`; `out_bytes` and `out_len` are success-only and
/// remain unchanged on error. The protobuf format cannot distinguish some
/// in-memory optionals, so encoding may normalize them (for example
/// `Some(empty)` files decode as absent). Cloning does not normalize them.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_encode(
    metadata: *const LanceIndexSegmentMetadata,
    out_bytes: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    ffi_try!(
        unsafe { metadata_encode_inner(metadata, out_bytes, out_len) },
        neg
    )
}

unsafe fn metadata_encode_inner(
    metadata: *const LanceIndexSegmentMetadata,
    out_bytes: *mut *mut u8,
    out_len: *mut usize,
) -> Result<i32> {
    if metadata.is_null() {
        return Err(invalid_input("metadata must not be NULL"));
    }
    if out_bytes.is_null() || out_len.is_null() {
        return Err(invalid_input("out_bytes and out_len must not be NULL"));
    }
    unsafe { export_index_metadata((*metadata).native_metadata(), out_bytes, out_len)? };
    Ok(0)
}

/// Return the number of covering field IDs. Returns 0 on error; zero is also a
/// valid count, so check lance_last_error_code().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_covering_field_count(
    metadata: *const LanceIndexSegmentMetadata,
) -> usize {
    ffi_try!(
        (|| -> Result<usize> {
            if metadata.is_null() {
                return Err(invalid_input("metadata is NULL"));
            }
            Ok(unsafe { (*metadata).metadata.covering_fields.len() })
        })(),
        0
    )
}

/// Copy one covering field ID in metadata order. Output is success-only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_covering_field_id_at(
    metadata: *const LanceIndexSegmentMetadata,
    index: usize,
    out_field_id: *mut i32,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_field_id.is_null() {
                return Err(invalid_input("out_field_id must not be NULL"));
            }
            if metadata.is_null() {
                return Err(invalid_input("metadata is NULL"));
            }
            let covering_fields = unsafe { &(*metadata).metadata.covering_fields };
            let field_id = covering_fields.get(index).ok_or_else(|| {
                invalid_input(format!(
                    "covering field index {index} is out of range for {} covering fields",
                    covering_fields.len()
                ))
            })?;
            unsafe { ptr::write(out_field_id, *field_id) };
            Ok(0)
        })(),
        neg
    )
}

/// Return the borrowed `index_details.value` bytes.
///
/// The pointer remains valid until the metadata handle is freed. A present but
/// empty value returns a non-NULL pointer and writes `*out_len = 0`. Absent
/// `index_details` returns NULL with LANCE_ERR_NOT_FOUND. `*out_len` is
/// unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_index_details_value(
    metadata: *const LanceIndexSegmentMetadata,
    out_len: *mut usize,
) -> *const u8 {
    ffi_try!(
        (|| -> Result<*const u8> {
            if out_len.is_null() {
                return Err(invalid_input("out_len must not be NULL"));
            }
            if metadata.is_null() {
                return Err(invalid_input("metadata is NULL"));
            }
            let details = unsafe { &(*metadata).metadata }
                .index_details
                .as_ref()
                .ok_or_else(|| {
                    Error::index_not_found("index metadata does not contain index_details")
                })?;
            unsafe { ptr::write(out_len, details.value.len()) };
            Ok(details.value.as_ptr())
        })(),
        ptr::null()
    )
}

/// Return the number of recorded files. Returns 0 on error or when the native
/// `files` option is absent; check lance_last_error_code() to distinguish.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_file_count(
    metadata: *const LanceIndexSegmentMetadata,
) -> usize {
    ffi_try!(
        (|| -> Result<usize> {
            if metadata.is_null() {
                return Err(invalid_input("metadata is NULL"));
            }
            Ok(unsafe { &(*metadata).metadata }
                .files
                .as_ref()
                .map(|files| files.len())
                .unwrap_or(0))
        })(),
        0
    )
}

/// Return one file path, borrowed until the metadata handle is freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_file_path_at(
    metadata: *const LanceIndexSegmentMetadata,
    index: usize,
) -> *const c_char {
    ffi_try!(
        (|| -> Result<*const c_char> {
            if metadata.is_null() {
                return Err(invalid_input("metadata is NULL"));
            }
            let metadata = unsafe { &*metadata };
            let files = metadata.metadata.files.as_ref().ok_or_else(|| {
                invalid_input(format!(
                    "index metadata file index {index} is out of range because files are absent"
                ))
            })?;
            let path = metadata.file_paths.get(index).ok_or_else(|| {
                invalid_input(format!(
                    "index metadata file index {index} is out of range for {} files",
                    files.len()
                ))
            })?;
            Ok(path.as_ptr())
        })(),
        ptr::null()
    )
}

/// Copy one recorded file size. Output is success-only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_file_size_at(
    metadata: *const LanceIndexSegmentMetadata,
    index: usize,
    out_size: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_size.is_null() {
                return Err(invalid_input("out_size must not be NULL"));
            }
            if metadata.is_null() {
                return Err(invalid_input("metadata is NULL"));
            }
            let files = unsafe { &(*metadata).metadata }
                .files
                .as_ref()
                .ok_or_else(|| {
                    invalid_input(format!(
                        "index metadata file index {index} is out of range because files are absent"
                    ))
                })?;
            let file = files.get(index).ok_or_else(|| {
                invalid_input(format!(
                    "index metadata file index {index} is out of range for {} files",
                    files.len()
                ))
            })?;
            unsafe { ptr::write(out_size, file.size_bytes) };
            Ok(0)
        })(),
        neg
    )
}

/// Read the optional native `created_at` timestamp.
///
/// On success, absent is reported with `is_set == false` and zeroed fields;
/// present is reported with `is_set == true`. The output is unchanged on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_created_at(
    metadata: *const LanceIndexSegmentMetadata,
    out_timestamp: *mut LanceOptionalTimestamp,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_timestamp.is_null() {
                return Err(invalid_input("out_timestamp must not be NULL"));
            }
            if metadata.is_null() {
                return Err(invalid_input("metadata is NULL"));
            }
            let timestamp = unsafe { &mut *out_timestamp };
            match unsafe { (*metadata).metadata.created_at } {
                Some(created_at) => {
                    timestamp.is_set = true;
                    timestamp.unix_seconds = created_at.timestamp();
                    timestamp.nanos = created_at.timestamp_subsec_nanos();
                }
                None => {
                    timestamp.is_set = false;
                    timestamp.unix_seconds = 0;
                    timestamp.nanos = 0;
                }
            }
            Ok(0)
        })(),
        neg
    )
}

/// Read the optional native `base_id`.
///
/// On success, absent is reported with `is_set == false` and `base_id == 0`;
/// present is reported with `is_set == true`. Both outputs are unchanged on
/// error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_base_id(
    metadata: *const LanceIndexSegmentMetadata,
    out_is_set: *mut bool,
    out_base_id: *mut u32,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            if out_is_set.is_null() || out_base_id.is_null() {
                return Err(invalid_input("out_is_set and out_base_id must not be NULL"));
            }
            if metadata.is_null() {
                return Err(invalid_input("metadata is NULL"));
            }
            let base_id = unsafe { (*metadata).metadata.base_id };
            unsafe {
                ptr::write(out_is_set, base_id.is_some());
                ptr::write(out_base_id, base_id.unwrap_or(0));
            }
            Ok(0)
        })(),
        neg
    )
}

/// Free parsed index metadata. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_index_segment_metadata_free(
    metadata: *mut LanceIndexSegmentMetadata,
) {
    if !metadata.is_null() {
        swallow_unwind("lance_index_segment_metadata_free", || unsafe {
            drop(Box::from_raw(metadata));
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc;
    use std::time::Duration;

    #[test]
    fn metadata_clone_preserves_in_memory_optionals_without_protobuf() {
        let metadata = IndexMetadata {
            uuid: Uuid::new_v4(),
            name: "optional_presence".to_string(),
            fields: vec![3, 7],
            covering_fields: vec![7],
            dataset_version: 42,
            fragment_bitmap: Some([1, 4, 9].into_iter().collect()),
            index_details: None,
            index_version: 5,
            created_at: None,
            base_id: Some(13),
            files: Some(Vec::new()),
        };
        let original = metadata_handle(metadata.clone()).unwrap();
        let cloned = unsafe { lance_index_segment_metadata_clone(original) };
        assert!(!cloned.is_null());
        assert_eq!(unsafe { (*cloned).native_metadata() }, &metadata);
        assert_eq!(
            unsafe { (*cloned).native_metadata().files.as_ref() }.map(Vec::len),
            Some(0),
            "an in-memory Some(empty) files value must not normalize to None"
        );

        unsafe {
            lance_index_segment_metadata_free(cloned);
            lance_index_segment_metadata_free(original);
        }
    }

    fn complete_metadata() -> IndexMetadata {
        IndexMetadata {
            uuid: Uuid::from_u128(0x00112233445566778899aabbccddeeff),
            name: "complete_metadata".to_string(),
            fields: vec![3, 7],
            covering_fields: vec![7],
            dataset_version: 42,
            fragment_bitmap: Some([1, 4, 9].into_iter().collect()),
            index_details: Some(Arc::new(prost_types::Any {
                type_url: "/lance.table.InvertedIndexDetails".to_string(),
                value: b"native details".to_vec(),
            })),
            index_version: 5,
            created_at: DateTime::<Utc>::from_timestamp_millis(1_700_000_123_456),
            base_id: Some(13),
            files: Some(vec![
                lance_table::format::IndexFile {
                    path: "index.idx".to_string(),
                    size_bytes: 11,
                },
                lance_table::format::IndexFile {
                    path: "metadata.idx".to_string(),
                    size_bytes: 22,
                },
            ]),
        }
    }

    #[test]
    fn metadata_clone_with_fragment_ids_preserves_complete_value() {
        let source_value = complete_metadata();
        let source = metadata_handle(source_value.clone()).unwrap();
        let replacement = [8_u32, 2, 8, 4];
        let cloned = unsafe {
            lance_index_segment_metadata_clone_with_fragment_ids(
                source,
                true,
                replacement.as_ptr(),
                replacement.len(),
            )
        };
        assert!(!cloned.is_null());
        let expected = IndexMetadata {
            fragment_bitmap: Some([2_u32, 4, 8].into_iter().collect()),
            ..source_value.clone()
        };
        assert_eq!(unsafe { (*cloned).native_metadata() }, &expected);
        assert_eq!(unsafe { (*source).native_metadata() }, &source_value);

        let unchanged = unsafe {
            lance_index_segment_metadata_clone_with_fragment_ids(
                source,
                false,
                std::ptr::NonNull::<u32>::dangling().as_ptr(),
                usize::MAX,
            )
        };
        assert!(!unchanged.is_null());
        assert_eq!(unsafe { (*unchanged).native_metadata() }, &source_value);

        let empty = unsafe {
            lance_index_segment_metadata_clone_with_fragment_ids(source, true, ptr::null(), 0)
        };
        assert!(!empty.is_null());
        assert_eq!(
            unsafe { (*empty).native_metadata().fragment_bitmap.as_ref() }
                .map(|bitmap| bitmap.iter().collect::<Vec<_>>()),
            Some(Vec::<u32>::new())
        );

        let mut encoded = ptr::null_mut();
        let mut encoded_len = 0_usize;
        assert_eq!(
            unsafe { lance_index_segment_metadata_encode(source, &mut encoded, &mut encoded_len) },
            0
        );
        assert!(!encoded.is_null() && encoded_len > 0);
        unsafe {
            lance_free_bytes(encoded);
            lance_index_segment_metadata_free(empty);
            lance_index_segment_metadata_free(unchanged);
            lance_index_segment_metadata_free(cloned);
            lance_index_segment_metadata_free(source);
        }
    }

    #[test]
    fn metadata_encode_round_trips_complete_value_and_normalizes_empty_options() {
        let source_value = complete_metadata();
        let source = metadata_handle(source_value.clone()).unwrap();
        let replacement = [1_u32, 3];
        let cloned = unsafe {
            lance_index_segment_metadata_clone_with_fragment_ids(
                source,
                true,
                replacement.as_ptr(),
                replacement.len(),
            )
        };
        assert!(!cloned.is_null());
        let expected = IndexMetadata {
            fragment_bitmap: Some([1_u32, 3].into_iter().collect()),
            ..source_value.clone()
        };
        assert_eq!(unsafe { (*cloned).native_metadata() }, &expected);

        let mut encoded = ptr::null_mut();
        let mut encoded_len = 0_usize;
        assert_eq!(
            unsafe { lance_index_segment_metadata_encode(cloned, &mut encoded, &mut encoded_len) },
            0
        );
        let decoded =
            decode_segment_metadata(unsafe { slice::from_raw_parts(encoded, encoded_len) })
                .unwrap();
        assert_eq!(decoded, expected);

        let empty_value = IndexMetadata {
            uuid: Uuid::from_u128(7),
            name: "empty_optionals".to_string(),
            fields: vec![2],
            covering_fields: Vec::new(),
            dataset_version: 1,
            fragment_bitmap: Some(Default::default()),
            index_details: None,
            index_version: 0,
            created_at: None,
            base_id: None,
            files: Some(Vec::new()),
        };
        let empty_source = metadata_handle(empty_value.clone()).unwrap();
        let empty_copy = unsafe {
            lance_index_segment_metadata_clone_with_fragment_ids(
                empty_source,
                false,
                ptr::null(),
                0,
            )
        };
        assert!(!empty_copy.is_null());
        assert_eq!(unsafe { (*empty_copy).native_metadata() }, &empty_value);

        let mut empty_encoded = ptr::null_mut();
        let mut empty_encoded_len = 0_usize;
        assert_eq!(
            unsafe {
                lance_index_segment_metadata_encode(
                    empty_copy,
                    &mut empty_encoded,
                    &mut empty_encoded_len,
                )
            },
            0
        );
        let empty_decoded = decode_segment_metadata(unsafe {
            slice::from_raw_parts(empty_encoded, empty_encoded_len)
        })
        .unwrap();
        assert_eq!(
            empty_decoded.fragment_bitmap,
            Some(Default::default()),
            "an empty roaring bitmap has a non-empty protobuf encoding"
        );
        assert_eq!(empty_decoded.files, None);

        unsafe {
            lance_free_bytes(empty_encoded);
            lance_free_bytes(encoded);
            lance_index_segment_metadata_free(empty_copy);
            lance_index_segment_metadata_free(empty_source);
            lance_index_segment_metadata_free(cloned);
            lance_index_segment_metadata_free(source);
        }
    }

    #[test]
    fn metadata_clone_with_fragment_ids_and_encode_validate_foreign_inputs() {
        assert!(
            unsafe {
                lance_index_segment_metadata_clone_with_fragment_ids(
                    ptr::null(),
                    false,
                    ptr::null(),
                    0,
                )
            }
            .is_null()
        );

        let source = metadata_handle(complete_metadata()).unwrap();
        let oversized = isize::MAX as usize / std::mem::size_of::<u32>() + 1;
        assert!(
            unsafe {
                lance_index_segment_metadata_clone_with_fragment_ids(
                    source,
                    true,
                    std::ptr::NonNull::<u32>::dangling().as_ptr(),
                    oversized,
                )
            }
            .is_null()
        );
        let unchanged = unsafe {
            lance_index_segment_metadata_clone_with_fragment_ids(
                source,
                false,
                std::ptr::NonNull::<u32>::dangling().as_ptr(),
                usize::MAX,
            )
        };
        assert!(!unchanged.is_null());

        let sentinel = std::ptr::NonNull::<u8>::dangling().as_ptr();
        let mut out_bytes = sentinel;
        let mut out_len = usize::MAX;
        assert_eq!(
            unsafe {
                lance_index_segment_metadata_encode(ptr::null(), &mut out_bytes, &mut out_len)
            },
            -1
        );
        assert_eq!(out_bytes, sentinel);
        assert_eq!(out_len, usize::MAX);

        assert_eq!(
            unsafe { lance_index_segment_metadata_encode(source, ptr::null_mut(), &mut out_len) },
            -1
        );
        assert_eq!(out_len, usize::MAX);

        assert_eq!(
            unsafe { lance_index_segment_metadata_encode(source, &mut out_bytes, ptr::null_mut()) },
            -1
        );
        assert_eq!(out_bytes, sentinel);

        unsafe {
            lance_index_segment_metadata_free(unchanged);
            lance_index_segment_metadata_free(source);
        }
    }

    struct LateCallState {
        entered_after_retire: AtomicBool,
    }

    unsafe extern "C" fn record_late_entry(
        ctx: *mut c_void,
        _event: i32,
        _stage: *const c_char,
        _total: u64,
        _unit: *const c_char,
        _completed: u64,
    ) {
        let state = unsafe { &*ctx.cast::<LateCallState>() };
        state.entered_after_retire.store(true, Ordering::SeqCst);
    }

    /// Regression test for the review finding that a progress clone held by a
    /// detached core worker task (e.g. the inverted index's tokenize_docs
    /// workers) must not enter the C callback once the owning build has
    /// retired the gate at the end of `execute_uncommitted`.
    #[test]
    fn detached_clone_cannot_enter_callback_after_retire() {
        let state = Box::new(LateCallState {
            entered_after_retire: AtomicBool::new(false),
        });
        let ctx = Box::into_raw(state);
        let bridge = SendIndexBuildProgressCallback {
            callback: record_late_entry,
            ctx: ctx.cast(),
            gate: Arc::new(ProgressCallbackGate::default()),
        };
        let detached = Arc::new(bridge.clone());
        // Simulate the retirement boundary at the end of execute_uncommitted:
        // the builder's own handle is gone and the gate is retired, while a
        // detached worker still holds its clone.
        drop(bridge);
        detached.gate.retire();
        crate::runtime::block_on(async move {
            tokio::task::spawn(async move {
                detached
                    .stage_progress("tokenize_docs", 1)
                    .await
                    .expect("a late progress call must be a no-op, not an error");
            })
            .await
            .unwrap();
        });
        let state = unsafe { Box::from_raw(ctx) };
        assert!(
            !state.entered_after_retire.load(Ordering::SeqCst),
            "detached clone entered the C callback after retirement"
        );
    }

    struct BlockingState {
        entered: mpsc::Sender<()>,
        release: mpsc::Receiver<()>,
    }

    unsafe extern "C" fn blocking_callback(
        ctx: *mut c_void,
        _event: i32,
        _stage: *const c_char,
        _total: u64,
        _unit: *const c_char,
        _completed: u64,
    ) {
        let state = unsafe { &*ctx.cast::<BlockingState>() };
        state.entered.send(()).unwrap();
        state.release.recv().unwrap();
    }

    /// `retire` must disable new entries and then wait until every in-flight
    /// invocation has exited before returning.
    #[test]
    fn retire_drains_in_flight_invocation() {
        let (entered_tx, entered_rx) = mpsc::channel::<()>();
        let (release_tx, release_rx) = mpsc::channel::<()>();
        let state = Box::new(BlockingState {
            entered: entered_tx,
            release: release_rx,
        });
        let ctx = Box::into_raw(state);
        let bridge = SendIndexBuildProgressCallback {
            callback: blocking_callback,
            ctx: ctx.cast(),
            gate: Arc::new(ProgressCallbackGate::default()),
        };
        let gate = bridge.gate.clone();

        // Park one invocation inside the C callback.
        let caller = std::thread::spawn(move || {
            crate::runtime::block_on(bridge.stage_progress("shuffle", 1)).unwrap();
        });
        entered_rx.recv().unwrap();

        let retire_returned = Arc::new(AtomicBool::new(false));
        let retire_thread = {
            let gate = gate.clone();
            let retire_returned = retire_returned.clone();
            std::thread::spawn(move || {
                gate.retire();
                retire_returned.store(true, Ordering::SeqCst);
            })
        };
        // Wait until retirement has actually disabled new entries...
        while !gate.retired.load(Ordering::SeqCst) {
            std::thread::yield_now();
        }
        // ...then prove it is still draining the in-flight invocation.
        std::thread::sleep(Duration::from_millis(50));
        assert!(
            !retire_returned.load(Ordering::SeqCst),
            "retire returned while a callback invocation was in flight"
        );
        // Unblock the callback; the drain must now complete.
        release_tx.send(()).unwrap();
        caller.join().unwrap();
        retire_thread.join().unwrap();
        assert!(retire_returned.load(Ordering::SeqCst));
        drop(unsafe { Box::from_raw(ctx) });
    }
}
