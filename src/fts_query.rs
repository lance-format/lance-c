// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Immutable FTS query values and process-local prepared query contexts.

use std::collections::HashSet;
use std::ffi::c_char;
use std::ptr;
use std::sync::Arc;

use futures::future::try_join_all;
use lance::index::{DatasetIndexExt, DatasetIndexInternalExt};
use lance_core::{Error, Result};
use lance_index::IndexCriteria;
use lance_index::metrics::NoOpMetricsCollector;
use lance_index::scalar::FullTextSearchQuery;
use lance_index::scalar::inverted::DocumentGranularity;
use lance_index::scalar::inverted::query::{
    BooleanQuery, BoostQuery, CombinedFieldsQuery, FtsQuery, MatchQuery, MultiMatchQuery, Occur,
    Operator, PhraseQuery, collect_query_tokens,
};
use lance_index::scalar::inverted::{InvertedIndex, MemBM25Scorer, build_global_bm25_scorer};
use lance_table::format::IndexMetadata;
use uuid::Uuid;

use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
use crate::helpers;
use crate::runtime::block_on;

/// Required relationship between the pinned dataset snapshot and its FTS index.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceFtsCoverageMode {
    /// Every current fragment must be covered by a committed FTS segment.
    Strict = 0,
    /// Search and score only documents covered by committed FTS segments.
    IndexOnly = 1,
}

impl TryFrom<i32> for LanceFtsCoverageMode {
    type Error = Error;

    fn try_from(value: i32) -> Result<Self> {
        match value {
            0 => Ok(Self::Strict),
            1 => Ok(Self::IndexOnly),
            _ => Err(Error::invalid_input(format!(
                "invalid coverage_mode {value}; expected 0 (STRICT) or 1 (INDEX_ONLY)"
            ))),
        }
    }
}

/// Operator used to combine the analyzed terms of a Match query.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceFtsMatchOperator {
    /// At least one analyzed term must match.
    Or = 0,
    /// Every analyzed term must match.
    And = 1,
}

impl TryFrom<i32> for LanceFtsMatchOperator {
    type Error = Error;

    fn try_from(value: i32) -> Result<Self> {
        match value {
            0 => Ok(Self::Or),
            1 => Ok(Self::And),
            _ => Err(Error::invalid_input(format!(
                "invalid match_operator {value}; expected 0 (OR) or 1 (AND)"
            ))),
        }
    }
}

impl From<LanceFtsMatchOperator> for Operator {
    fn from(value: LanceFtsMatchOperator) -> Self {
        match value {
            LanceFtsMatchOperator::Or => Self::Or,
            LanceFtsMatchOperator::And => Self::And,
        }
    }
}

/// Query-specific state that must be shared by every segment-scoped scan.
pub(crate) enum PreparedFtsQuery {
    /// Exact Match queries share one corpus-wide scorer.
    Match(Arc<MemBM25Scorer>),
    /// Phrase does not expand terms, so a shared global scorer is sufficient.
    Phrase(Arc<MemBM25Scorer>),
}

/// Rust-owned immutable state behind [`LanceFtsQueryContext`].
pub(crate) struct FtsQueryContextInner {
    pub(crate) dataset: Arc<lance::Dataset>,
    pub(crate) query: FullTextSearchQuery,
    pub(crate) segments: Vec<IndexMetadata>,
    pub(crate) prepared: PreparedFtsQuery,
}

impl FtsQueryContextInner {
    pub(crate) fn validate_dataset_identity(&self, dataset: &Arc<lance::Dataset>) -> Result<()> {
        if !Arc::ptr_eq(&self.dataset, dataset) {
            return Err(invalid_input(format!(
                "FTS query context and scanner must originate from the same process-local dataset snapshot; context has uri '{}' version {}, scanner has uri '{}' version {}",
                self.dataset.uri(),
                self.dataset.version_id(),
                dataset.uri(),
                dataset.version_id()
            )));
        }
        Ok(())
    }
}

/// Opaque process-local FTS query context.
///
/// The handle owns an `Arc`, and scanners clone that `Arc` when the context is
/// attached. It is therefore safe to close the public handle after all scanner
/// attachments have completed.
pub struct LanceFtsQueryContext {
    pub(crate) inner: Arc<FtsQueryContextInner>,
}

/// Unit treated as one document by a Match or Phrase query.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceFtsDocumentGranularity {
    /// Infer the granularity stored by the target FTS index.
    Unspecified = 0,
    /// All text selected from one dataset row is one document.
    Row = 1,
    /// Each element of the deepest list on the field path is one document.
    ListElement = 2,
}

impl TryFrom<i32> for LanceFtsDocumentGranularity {
    type Error = Error;

    fn try_from(value: i32) -> Result<Self> {
        match value {
            0 => Ok(Self::Unspecified),
            1 => Ok(Self::Row),
            2 => Ok(Self::ListElement),
            _ => Err(invalid_input(format!(
                "invalid document_granularity {value}; expected 0 (UNSPECIFIED), 1 (ROW), or 2 (LIST_ELEMENT)"
            ))),
        }
    }
}

impl LanceFtsDocumentGranularity {
    fn into_native(self) -> Option<DocumentGranularity> {
        match self {
            Self::Unspecified => None,
            Self::Row => Some(DocumentGranularity::Row),
            Self::ListElement => Some(DocumentGranularity::ListElement),
        }
    }
}

/// Membership requirement for one Boolean query clause.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceFtsOccur {
    /// The clause may match and contributes its score when it does.
    Should = 0,
    /// The clause must match and contributes its score.
    Must = 1,
    /// The clause must not match and never contributes to the score.
    MustNot = 2,
}

impl TryFrom<i32> for LanceFtsOccur {
    type Error = Error;

    fn try_from(value: i32) -> Result<Self> {
        match value {
            0 => Ok(Self::Should),
            1 => Ok(Self::Must),
            2 => Ok(Self::MustNot),
            _ => Err(invalid_input(format!(
                "invalid occur value {value}; expected 0 (SHOULD), 1 (MUST), or 2 (MUST_NOT)"
            ))),
        }
    }
}

impl From<LanceFtsOccur> for Occur {
    fn from(value: LanceFtsOccur) -> Self {
        match value {
            LanceFtsOccur::Should => Self::Should,
            LanceFtsOccur::Must => Self::Must,
            LanceFtsOccur::MustNot => Self::MustNot,
        }
    }
}

/// Options for a typed Match query. Initialize with
/// [`lance_fts_query_match_options_init`].
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceFtsMatchOptions {
    /// Finite, non-negative score multiplier validated at scan planning.
    pub boost: f32,
    /// `-1` represents the core AUTO setting. The Lance 14.0.2 dataset
    /// planner resolves AUTO to exact matching. Other values are explicit
    /// edit distances in `[0, u32::MAX]`, with `0` selecting exact matching.
    pub fuzziness: i64,
    /// Final fuzzy vocabulary budget for this Match leaf.
    pub max_expansions: u64,
    /// Fixed-width [`LanceFtsMatchOperator`] discriminant.
    pub match_operator: i32,
    /// Number of beginning characters left unchanged during fuzzy matching.
    pub prefix_length: u32,
    /// Fixed-width [`LanceFtsDocumentGranularity`] discriminant.
    pub document_granularity: i32,
}

impl Default for LanceFtsMatchOptions {
    fn default() -> Self {
        Self {
            boost: 1.0,
            fuzziness: 0,
            max_expansions: 50,
            match_operator: LanceFtsMatchOperator::Or as i32,
            prefix_length: 0,
            document_granularity: LanceFtsDocumentGranularity::Unspecified as i32,
        }
    }
}

impl LanceFtsMatchOptions {
    fn into_query(self, terms: String, column: Option<String>) -> Result<MatchQuery> {
        let operator: Operator = LanceFtsMatchOperator::try_from(self.match_operator)?.into();
        let granularity = LanceFtsDocumentGranularity::try_from(self.document_granularity)?;
        let fuzziness = match self.fuzziness {
            -1 => None,
            value if (0..=i64::from(u32::MAX)).contains(&value) => Some(value as u32),
            value => {
                return Err(invalid_input(format!(
                    "match fuzziness must be -1 (automatic) or in [0, {}], got {value}",
                    u32::MAX
                )));
            }
        };
        let max_expansions = usize::try_from(self.max_expansions).map_err(|_| {
            invalid_input(format!(
                "match max_expansions {} exceeds usize::MAX ({}) on this platform",
                self.max_expansions,
                usize::MAX
            ))
        })?;

        let mut query = MatchQuery::new(terms)
            .with_column(column)
            .with_boost(self.boost)
            .with_fuzziness(fuzziness)
            .with_max_expansions(max_expansions)
            .with_operator(operator)
            .with_prefix_length(self.prefix_length);
        if let Some(granularity) = granularity.into_native() {
            query = query.with_document_granularity(granularity);
        }
        Ok(query)
    }
}

/// Options for a typed Phrase query. Initialize with
/// [`lance_fts_query_phrase_options_init`].
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceFtsPhraseOptions {
    /// Maximum non-negative number of intervening token positions.
    pub slop: u32,
    /// Fixed-width [`LanceFtsDocumentGranularity`] discriminant.
    pub document_granularity: i32,
}

impl Default for LanceFtsPhraseOptions {
    fn default() -> Self {
        Self {
            slop: 0,
            document_granularity: LanceFtsDocumentGranularity::Unspecified as i32,
        }
    }
}

impl LanceFtsPhraseOptions {
    fn into_query(self, terms: String, column: Option<String>) -> Result<PhraseQuery> {
        let granularity = LanceFtsDocumentGranularity::try_from(self.document_granularity)?;
        let mut query = PhraseQuery::new(terms)
            .with_column(column)
            .with_slop(self.slop);
        if let Some(granularity) = granularity.into_native() {
            query = query.with_document_granularity(granularity);
        }
        Ok(query)
    }
}

/// Options for a typed MultiMatch query. Initialize with
/// [`lance_fts_query_multi_match_options_init`].
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceFtsMultiMatchOptions {
    /// Optional per-column boosts. NULL means the native default of 1.0 for
    /// every column; a non-NULL pointer makes `boost_count` significant.
    pub boosts: *const f32,
    /// Number of entries readable through `boosts`.
    pub boost_count: usize,
    /// Fixed-width [`LanceFtsMatchOperator`] discriminant.
    pub match_operator: i32,
}

impl Default for LanceFtsMultiMatchOptions {
    fn default() -> Self {
        Self {
            boosts: ptr::null(),
            boost_count: 0,
            match_operator: LanceFtsMatchOperator::Or as i32,
        }
    }
}

impl LanceFtsMultiMatchOptions {
    fn into_query(self, terms: String, columns: Vec<String>) -> Result<MultiMatchQuery> {
        let operator: Operator = LanceFtsMatchOperator::try_from(self.match_operator)?.into();
        let mut query = MultiMatchQuery::try_new(terms, columns)?.with_operator(operator);
        if !self.boosts.is_null() || self.boost_count != 0 {
            let boosts =
                unsafe { read_f32_values(self.boosts, self.boost_count, "multi_match boosts")? };
            query = query.try_with_boosts(boosts)?;
        }
        Ok(query)
    }
}

/// Options for a typed CombinedFields query. Initialize with
/// [`lance_fts_query_combined_fields_options_init`].
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceFtsCombinedFieldsOptions {
    /// Optional per-column BM25F weights. NULL means the native default of 1.0
    /// for every column; a non-NULL pointer makes `boost_count` significant.
    pub boosts: *const f32,
    /// Number of entries readable through `boosts`.
    pub boost_count: usize,
    /// Fixed-width [`LanceFtsMatchOperator`] discriminant.
    pub match_operator: i32,
}

impl Default for LanceFtsCombinedFieldsOptions {
    fn default() -> Self {
        Self {
            boosts: ptr::null(),
            boost_count: 0,
            match_operator: LanceFtsMatchOperator::Or as i32,
        }
    }
}

impl LanceFtsCombinedFieldsOptions {
    fn into_query(self, terms: String, columns: Vec<String>) -> Result<CombinedFieldsQuery> {
        let operator: Operator = LanceFtsMatchOperator::try_from(self.match_operator)?.into();
        let mut query = CombinedFieldsQuery::try_new(terms, columns)?.with_operator(operator);
        if !self.boosts.is_null() || self.boost_count != 0 {
            let boosts = unsafe {
                read_f32_values(self.boosts, self.boost_count, "combined_fields boosts")?
            };
            query = query.try_with_boosts(boosts)?;
        }
        Ok(query)
    }
}

/// Options for a typed Boost query. Initialize with
/// [`lance_fts_query_boost_options_init`].
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceFtsBoostOptions {
    /// Finite, non-negative multiplier subtracted from the positive score.
    pub negative_boost: f32,
}

impl Default for LanceFtsBoostOptions {
    fn default() -> Self {
        Self {
            negative_boost: 0.5,
        }
    }
}

fn check_array_count<T>(count: usize, name: &str) -> Result<()> {
    if count > isize::MAX as usize / std::mem::size_of::<T>() {
        return Err(invalid_input(format!(
            "{name} count {count} exceeds the maximum addressable array length for element size {}",
            std::mem::size_of::<T>()
        )));
    }
    Ok(())
}

unsafe fn read_f32_values(ptr: *const f32, count: usize, name: &str) -> Result<Vec<f32>> {
    check_array_count::<f32>(count, name)?;
    if count == 0 {
        return Ok(Vec::new());
    }
    if ptr.is_null() {
        return Err(invalid_input(format!(
            "{name} must not be NULL when count is {count}"
        )));
    }
    let mut values = Vec::with_capacity(count);
    for index in 0..count {
        values.push(unsafe { ptr.add(index).read_unaligned() });
    }
    Ok(values)
}

unsafe fn read_i32_values(ptr: *const i32, count: usize, name: &str) -> Result<Vec<i32>> {
    check_array_count::<i32>(count, name)?;
    if count == 0 {
        return Ok(Vec::new());
    }
    if ptr.is_null() {
        return Err(invalid_input(format!(
            "{name} must not be NULL when count is {count}"
        )));
    }
    let mut values = Vec::with_capacity(count);
    for index in 0..count {
        values.push(unsafe { ptr.add(index).read_unaligned() });
    }
    Ok(values)
}

unsafe fn read_query_pointers(
    ptr: *const *const LanceFtsQuery,
    count: usize,
    name: &str,
) -> Result<Vec<*const LanceFtsQuery>> {
    check_array_count::<*const LanceFtsQuery>(count, name)?;
    if count == 0 {
        return Ok(Vec::new());
    }
    if ptr.is_null() {
        return Err(invalid_input(format!(
            "{name} must not be NULL when count is {count}"
        )));
    }
    let mut values = Vec::with_capacity(count);
    for index in 0..count {
        values.push(unsafe { ptr.add(index).read_unaligned() });
    }
    Ok(values)
}

unsafe fn read_strings(ptr: *const *const c_char, count: usize, name: &str) -> Result<Vec<String>> {
    check_array_count::<*const c_char>(count, name)?;
    if count == 0 {
        return Ok(Vec::new());
    }
    if ptr.is_null() {
        return Err(invalid_input(format!(
            "{name} must not be NULL when count is {count}"
        )));
    }
    let mut values = Vec::with_capacity(count);
    for index in 0..count {
        let value = unsafe { ptr.add(index).read_unaligned() };
        if value.is_null() {
            return Err(invalid_input(format!("{name}[{index}] must not be NULL")));
        }
        let parsed = unsafe { helpers::parse_c_string(value)? }
            .ok_or_else(|| invalid_input(format!("{name}[{index}] must not be NULL")))?;
        values.push(parsed.to_string());
    }
    Ok(values)
}

unsafe fn read_required_string(ptr: *const c_char, name: &str) -> Result<String> {
    if ptr.is_null() {
        return Err(invalid_input(format!("{name} must not be NULL")));
    }
    Ok(unsafe { helpers::parse_c_string(ptr)? }
        .ok_or_else(|| invalid_input(format!("{name} must not be NULL")))?
        .to_string())
}

unsafe fn read_options<T: Copy + Default>(options: *const T) -> T {
    if options.is_null() {
        T::default()
    } else {
        unsafe { options.read_unaligned() }
    }
}

/// Immutable typed full-text query value.
///
/// Composite constructors clone their child values. Attaching a query to a
/// scanner snapshots the value, so this handle and every source child handle
/// may be closed before the scan runs.
pub struct LanceFtsQuery {
    pub(crate) query: FtsQuery,
}

fn into_query_handle(query: FtsQuery) -> *mut LanceFtsQuery {
    Box::into_raw(Box::new(LanceFtsQuery { query }))
}

/// Initialize Match options with the native defaults: boost 1.0, fuzziness
/// 0, max_expansions 50, OR, prefix_length 0, and index-inferred document
/// granularity.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_query_match_options_init(options: *mut LanceFtsMatchOptions) {
    if !options.is_null() {
        unsafe { options.write(LanceFtsMatchOptions::default()) };
    }
}

/// Initialize Phrase options with slop 0 and index-inferred document
/// granularity.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_query_phrase_options_init(options: *mut LanceFtsPhraseOptions) {
    if !options.is_null() {
        unsafe { options.write(LanceFtsPhraseOptions::default()) };
    }
}

/// Initialize MultiMatch options with no per-column boosts and the OR
/// operator.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_query_multi_match_options_init(
    options: *mut LanceFtsMultiMatchOptions,
) {
    if !options.is_null() {
        unsafe { options.write(LanceFtsMultiMatchOptions::default()) };
    }
}

/// Initialize CombinedFields options with no per-column weights and the OR
/// operator.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_query_combined_fields_options_init(
    options: *mut LanceFtsCombinedFieldsOptions,
) {
    if !options.is_null() {
        unsafe { options.write(LanceFtsCombinedFieldsOptions::default()) };
    }
}

/// Initialize Boost options with the native default negative_boost of 0.5.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_query_boost_options_init(options: *mut LanceFtsBoostOptions) {
    if !options.is_null() {
        unsafe { options.write(LanceFtsBoostOptions::default()) };
    }
}

/// Create a Match query. `terms` may be empty. A NULL `column` leaves the
/// native column unspecified so scan planning resolves indexed columns. A NULL
/// `options` uses the native defaults.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_query_match_new(
    terms: *const c_char,
    column: *const c_char,
    options: *const LanceFtsMatchOptions,
) -> *mut LanceFtsQuery {
    ffi_try!(
        unsafe { lance_fts_query_match_new_inner(terms, column, options) },
        null
    )
}

unsafe fn lance_fts_query_match_new_inner(
    terms: *const c_char,
    column: *const c_char,
    options: *const LanceFtsMatchOptions,
) -> Result<*mut LanceFtsQuery> {
    let terms = unsafe { read_required_string(terms, "match terms")? };
    let column = unsafe { helpers::parse_c_string(column)? }.map(|value| value.to_string());
    let options = unsafe { read_options(options) };
    Ok(into_query_handle(options.into_query(terms, column)?.into()))
}

/// Create a Phrase query. `terms` may be empty. A NULL `column` leaves the
/// native column unspecified so scan planning resolves indexed columns.
/// A NULL `options` uses the native defaults.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_query_phrase_new(
    terms: *const c_char,
    column: *const c_char,
    options: *const LanceFtsPhraseOptions,
) -> *mut LanceFtsQuery {
    ffi_try!(
        unsafe { lance_fts_query_phrase_new_inner(terms, column, options) },
        null
    )
}

unsafe fn lance_fts_query_phrase_new_inner(
    terms: *const c_char,
    column: *const c_char,
    options: *const LanceFtsPhraseOptions,
) -> Result<*mut LanceFtsQuery> {
    let terms = unsafe { read_required_string(terms, "phrase terms")? };
    let column = unsafe { helpers::parse_c_string(column)? }.map(|value| value.to_string());
    let options = unsafe { read_options(options) };
    Ok(into_query_handle(options.into_query(terms, column)?.into()))
}

/// Create a MultiMatch query. Native construction rejects an empty column
/// list. NULL `options` uses the native defaults.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_query_multi_match_new(
    terms: *const c_char,
    columns: *const *const c_char,
    column_count: usize,
    options: *const LanceFtsMultiMatchOptions,
) -> *mut LanceFtsQuery {
    ffi_try!(
        unsafe { lance_fts_query_multi_match_new_inner(terms, columns, column_count, options) },
        null
    )
}

unsafe fn lance_fts_query_multi_match_new_inner(
    terms: *const c_char,
    columns: *const *const c_char,
    column_count: usize,
    options: *const LanceFtsMultiMatchOptions,
) -> Result<*mut LanceFtsQuery> {
    let terms = unsafe { read_required_string(terms, "multi_match terms")? };
    let columns = unsafe { read_strings(columns, column_count, "multi_match columns")? };
    let options = unsafe { read_options(options) };
    Ok(into_query_handle(
        options.into_query(terms, columns)?.into(),
    ))
}

/// Create a CombinedFields (BM25F) query. Native construction rejects an empty
/// or duplicate column list and weights outside `[1, 2^20]`. NULL `options`
/// uses the native defaults.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_query_combined_fields_new(
    terms: *const c_char,
    columns: *const *const c_char,
    column_count: usize,
    options: *const LanceFtsCombinedFieldsOptions,
) -> *mut LanceFtsQuery {
    ffi_try!(
        unsafe { lance_fts_query_combined_fields_new_inner(terms, columns, column_count, options) },
        null
    )
}

unsafe fn lance_fts_query_combined_fields_new_inner(
    terms: *const c_char,
    columns: *const *const c_char,
    column_count: usize,
    options: *const LanceFtsCombinedFieldsOptions,
) -> Result<*mut LanceFtsQuery> {
    let terms = unsafe { read_required_string(terms, "combined_fields terms")? };
    let columns = unsafe { read_strings(columns, column_count, "combined_fields columns")? };
    let options = unsafe { read_options(options) };
    Ok(into_query_handle(
        options.into_query(terms, columns)?.into(),
    ))
}

/// Create a Boolean query. An empty clause list is accepted here and rejected
/// later by scan planning when it has no positive clause. Both arrays must
/// contain `query_count` entries unless the count is zero. NULL children are
/// rejected.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_query_boolean_new(
    occurs: *const i32,
    queries: *const *const LanceFtsQuery,
    query_count: usize,
) -> *mut LanceFtsQuery {
    ffi_try!(
        unsafe { lance_fts_query_boolean_new_inner(occurs, queries, query_count) },
        null
    )
}

unsafe fn lance_fts_query_boolean_new_inner(
    occurs: *const i32,
    queries: *const *const LanceFtsQuery,
    query_count: usize,
) -> Result<*mut LanceFtsQuery> {
    let occurs = unsafe { read_i32_values(occurs, query_count, "boolean occurs")? };
    let queries = unsafe { read_query_pointers(queries, query_count, "boolean queries")? };
    let mut clauses = Vec::with_capacity(query_count);
    for (index, (occur, query)) in occurs.iter().zip(queries).enumerate() {
        let occur = LanceFtsOccur::try_from(*occur)
            .map_err(|error| invalid_input(format!("occurs[{index}]: {error}")))?;
        let query = unsafe { query_ref(query, &format!("queries[{index}]"))? };
        clauses.push((occur.into(), query.query.clone()));
    }
    Ok(into_query_handle(BooleanQuery::new(clauses).into()))
}

/// Create a Boost query from positive and negative child queries.
/// `negative_boost` is validated by scan planning. NULL `options` uses the
/// native default of 0.5.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_query_boost_new(
    positive: *const LanceFtsQuery,
    negative: *const LanceFtsQuery,
    options: *const LanceFtsBoostOptions,
) -> *mut LanceFtsQuery {
    ffi_try!(
        unsafe { lance_fts_query_boost_new_inner(positive, negative, options) },
        null
    )
}

unsafe fn lance_fts_query_boost_new_inner(
    positive: *const LanceFtsQuery,
    negative: *const LanceFtsQuery,
    options: *const LanceFtsBoostOptions,
) -> Result<*mut LanceFtsQuery> {
    let positive = unsafe { query_ref(positive, "positive")? };
    let negative = unsafe { query_ref(negative, "negative")? };
    let options = unsafe { read_options(options) };
    let query = BoostQuery::new(
        positive.query.clone(),
        negative.query.clone(),
        Some(options.negative_boost),
    );
    Ok(into_query_handle(query.into()))
}

unsafe fn query_ref<'a>(query: *const LanceFtsQuery, name: &str) -> Result<&'a LanceFtsQuery> {
    if query.is_null() {
        return Err(invalid_input(format!("{name} must not be NULL")));
    }
    Ok(unsafe { &*query })
}

/// Close a typed query handle. NULL-safe. Composite children and scanners that
/// already attached this query retain their own cloned values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_query_close(query: *mut LanceFtsQuery) {
    if !query.is_null() {
        swallow_unwind("lance_fts_query_close", || unsafe {
            drop(Box::from_raw(query));
        });
    }
}

pub(crate) unsafe fn clone_query(query: *const LanceFtsQuery) -> Result<FtsQuery> {
    if query.is_null() {
        return Err(invalid_input("query must not be NULL"));
    }
    Ok(unsafe { &*query }.query.clone())
}

fn invalid_input(message: impl Into<String>) -> Error {
    Error::invalid_input(message.into())
}

async fn prepare_fts_query_context(
    dataset: Arc<lance::Dataset>,
    column: String,
    query: FullTextSearchQuery,
    coverage_mode: LanceFtsCoverageMode,
) -> Result<FtsQueryContextInner> {
    let logical_index = dataset
        .load_scalar_index(IndexCriteria::default().for_column(&column).supports_fts())
        .await?
        .ok_or_else(|| {
            invalid_input(format!(
                "no committed FTS index exists for column '{column}' in dataset version {}",
                dataset.version_id()
            ))
        })?;
    let segments = dataset.load_indices_by_name(&logical_index.name).await?;
    if segments.is_empty() {
        return Err(invalid_input(format!(
            "FTS index for column '{column}' has no committed segments in dataset version {}",
            dataset.version_id()
        )));
    }

    let expected_fields = &segments[0].fields;
    if let Some(segment) = segments
        .iter()
        .find(|segment| &segment.fields != expected_fields)
    {
        return Err(invalid_input(format!(
            "FTS index '{}' has inconsistent fields across segments; segment {} has fields {:?}, expected {:?}",
            logical_index.name, segment.uuid, segment.fields, expected_fields
        )));
    }

    let current_fragment_ids: HashSet<u32> = dataset
        .get_fragments()
        .into_iter()
        .map(|fragment| {
            u32::try_from(fragment.id()).map_err(|_| {
                invalid_input(format!(
                    "fragment id {} exceeds the u32 index metadata range",
                    fragment.id()
                ))
            })
        })
        .collect::<Result<_>>()?;

    let mut indexed_fragment_ids = HashSet::new();
    for segment in &segments {
        let fragment_bitmap = segment.fragment_bitmap.as_ref().ok_or_else(|| {
            invalid_input(format!(
                "FTS segment {} for column '{column}' has unknown fragment coverage",
                segment.uuid
            ))
        })?;
        indexed_fragment_ids.extend(
            fragment_bitmap
                .iter()
                .filter(|fragment_id| current_fragment_ids.contains(fragment_id)),
        );
    }
    let mut unindexed_fragment_ids: Vec<u32> = current_fragment_ids
        .difference(&indexed_fragment_ids)
        .copied()
        .collect();
    unindexed_fragment_ids.sort_unstable();

    if coverage_mode == LanceFtsCoverageMode::Strict && !unindexed_fragment_ids.is_empty() {
        return Err(invalid_input(format!(
            "coverage_mode=STRICT requires every fragment in dataset version {} to be indexed; column '{column}' has {} unindexed fragments: {:?}",
            dataset.version_id(),
            unindexed_fragment_ids.len(),
            unindexed_fragment_ids
        )));
    }

    let indices: Vec<Arc<InvertedIndex>> = try_join_all(segments.iter().map(|segment| {
        let dataset = Arc::clone(&dataset);
        let column = column.clone();
        async move {
            let index = dataset
                .open_scalar_index(&column, &segment.uuid, &NoOpMetricsCollector)
                .await?;
            let inverted = index
                .as_any()
                .downcast_ref::<InvertedIndex>()
                .ok_or_else(|| {
                    invalid_input(format!(
                        "index segment {} for column '{column}' is not an inverted index",
                        segment.uuid
                    ))
                })?;
            Ok::<_, Error>(Arc::new(inverted.clone()))
        }
    }))
    .await?;

    let expected_params = indices[0].params();
    if let Some((position, _)) = indices
        .iter()
        .enumerate()
        .find(|(_, index)| index.params() != expected_params)
    {
        return Err(invalid_input(format!(
            "FTS index '{}' has inconsistent inverted index parameters; segment {} differs from segment {}",
            logical_index.name, segments[position].uuid, segments[0].uuid
        )));
    }

    let prepared = match &query.query {
        FtsQuery::Match(match_query) => {
            let mut tokenizer = indices[0].tokenizer();
            let query_tokens = collect_query_tokens(&match_query.terms, &mut tokenizer);
            let params = query
                .params()
                .with_fuzziness(match_query.fuzziness)
                .with_max_expansions(match_query.max_expansions)
                .with_prefix_length(match_query.prefix_length);
            PreparedFtsQuery::Match(Arc::new(
                build_global_bm25_scorer(&indices, &query_tokens, &params, None).await?,
            ))
        }
        FtsQuery::Phrase(phrase_query) => {
            if !expected_params.has_positions() {
                return Err(invalid_input(format!(
                    "FTS index '{}' for column '{column}' does not store token positions required by Phrase queries; recreate the index with positions enabled",
                    logical_index.name
                )));
            }
            let mut tokenizer = indices[0].tokenizer();
            let query_tokens = collect_query_tokens(&phrase_query.terms, &mut tokenizer);
            let params = query.params().with_phrase_slop(Some(phrase_query.slop));
            PreparedFtsQuery::Phrase(Arc::new(
                build_global_bm25_scorer(&indices, &query_tokens, &params, None).await?,
            ))
        }
        _ => {
            return Err(Error::internal(
                "prepared FTS query must be a single-column Match or Phrase query".to_string(),
            ));
        }
    };

    Ok(FtsQueryContextInner {
        dataset,
        query,
        segments,
        prepared,
    })
}

unsafe fn parse_query_inputs(
    dataset: *const LanceDataset,
    column: *const c_char,
    query: *const c_char,
    coverage_mode: i32,
) -> Result<(Arc<lance::Dataset>, String, String, LanceFtsCoverageMode)> {
    if dataset.is_null() || column.is_null() || query.is_null() {
        return Err(invalid_input("dataset, column, and query must not be NULL"));
    }
    let column = unsafe { helpers::parse_c_string(column)? }
        .filter(|value| !value.is_empty())
        .ok_or_else(|| invalid_input("column must not be empty"))?
        .to_string();
    let query = unsafe { helpers::parse_c_string(query)? }
        .filter(|value| !value.is_empty())
        .ok_or_else(|| invalid_input("query must not be empty"))?
        .to_string();
    let coverage_mode = LanceFtsCoverageMode::try_from(coverage_mode)?;
    let snapshot = unsafe { &*dataset }.snapshot();
    Ok((snapshot, column, query, coverage_mode))
}

fn into_context(inner: FtsQueryContextInner) -> *mut LanceFtsQueryContext {
    Box::into_raw(Box::new(LanceFtsQueryContext {
        inner: Arc::new(inner),
    }))
}

/// Compatibility API for an OR Match query.
#[deprecated(note = "use lance_dataset_prepare_fts_match_query to select the Match operator")]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_prepare_fts_query(
    dataset: *const LanceDataset,
    column: *const c_char,
    query: *const c_char,
    max_fuzzy_distance: u32,
    coverage_mode: i32,
) -> *mut LanceFtsQueryContext {
    ffi_try!(
        unsafe {
            prepare_fts_match_query_inner(
                dataset,
                column,
                query,
                LanceFtsMatchOperator::Or as i32,
                max_fuzzy_distance,
                coverage_mode,
            )
        },
        null
    )
}

/// Prepare a process-local Match query context. AND and OR are supported.
/// `max_fuzzy_distance` is retained for the future prepared-fuzzy path but
/// must be zero with the currently pinned Lance revision.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_prepare_fts_match_query(
    dataset: *const LanceDataset,
    column: *const c_char,
    query: *const c_char,
    match_operator: i32,
    max_fuzzy_distance: u32,
    coverage_mode: i32,
) -> *mut LanceFtsQueryContext {
    ffi_try!(
        unsafe {
            prepare_fts_match_query_inner(
                dataset,
                column,
                query,
                match_operator,
                max_fuzzy_distance,
                coverage_mode,
            )
        },
        null
    )
}

unsafe fn prepare_fts_match_query_inner(
    dataset: *const LanceDataset,
    column: *const c_char,
    query: *const c_char,
    match_operator: i32,
    max_fuzzy_distance: u32,
    coverage_mode: i32,
) -> Result<*mut LanceFtsQueryContext> {
    let (snapshot, column, query_text, coverage_mode) =
        unsafe { parse_query_inputs(dataset, column, query, coverage_mode)? };
    let operator: Operator = LanceFtsMatchOperator::try_from(match_operator)?.into();
    // The parameter remains in the public API so callers do not need another
    // ABI change when Lance-C moves to a Lance revision that can inject the
    // same canonical fuzzy vocabulary into every segment-scoped scan. The
    // pinned Lance revision can share only the scorer, so accepting fuzzy here
    // would allow different segments to choose different capped expansions.
    if max_fuzzy_distance != 0 {
        return Err(invalid_input(format!(
            "max_fuzzy_distance must be 0 for prepared FTS with the pinned Lance revision, got {max_fuzzy_distance}; the parameter is reserved until canonical fuzzy vocabulary injection is available"
        )));
    }
    let query = FullTextSearchQuery::new_query(
        MatchQuery::new(query_text)
            .with_column(Some(column.clone()))
            .with_operator(operator)
            .with_fuzziness(Some(0))
            .into(),
    );
    let inner = block_on(prepare_fts_query_context(
        snapshot,
        column,
        query,
        coverage_mode,
    ))?;
    Ok(into_context(inner))
}

/// Prepare a process-local Phrase query context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_prepare_fts_phrase_query(
    dataset: *const LanceDataset,
    column: *const c_char,
    query: *const c_char,
    slop: i32,
    coverage_mode: i32,
) -> *mut LanceFtsQueryContext {
    ffi_try!(
        unsafe { prepare_fts_phrase_query_inner(dataset, column, query, slop, coverage_mode) },
        null
    )
}

unsafe fn prepare_fts_phrase_query_inner(
    dataset: *const LanceDataset,
    column: *const c_char,
    query: *const c_char,
    slop: i32,
    coverage_mode: i32,
) -> Result<*mut LanceFtsQueryContext> {
    let slop = u32::try_from(slop)
        .map_err(|_| invalid_input(format!("slop must be non-negative, got {slop}")))?;
    let (snapshot, column, query_text, coverage_mode) =
        unsafe { parse_query_inputs(dataset, column, query, coverage_mode)? };
    let query = FullTextSearchQuery::new_query(
        PhraseQuery::new(query_text)
            .with_column(Some(column.clone()))
            .with_slop(slop)
            .into(),
    );
    let inner = block_on(prepare_fts_query_context(
        snapshot,
        column,
        query,
        coverage_mode,
    ))?;
    Ok(into_context(inner))
}

/// Close a context handle. NULL-safe. Scanners that already attached the
/// context retain their own shared reference.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_query_context_close(context: *mut LanceFtsQueryContext) {
    if !context.is_null() {
        swallow_unwind("lance_fts_query_context_close", || unsafe {
            drop(Box::from_raw(context));
        });
    }
}

pub(crate) unsafe fn clone_context(
    context: *const LanceFtsQueryContext,
) -> Result<Arc<FtsQueryContextInner>> {
    if context.is_null() {
        return Err(invalid_input("context must not be NULL"));
    }
    Ok(Arc::clone(&unsafe { &*context }.inner))
}

pub(crate) fn parse_segment_uuids(segment_uuids: *const u8, len: usize) -> Result<Vec<Uuid>> {
    if segment_uuids.is_null() && len > 0 {
        return Err(invalid_input(
            "segment_uuids is NULL but len is greater than 0",
        ));
    }
    if len > isize::MAX as usize / 16 {
        return Err(invalid_input(format!(
            "segment UUID count {len} exceeds the maximum addressable byte slice length"
        )));
    }
    let mut uuids = Vec::with_capacity(len);
    for position in 0..len {
        let mut bytes = [0_u8; 16];
        unsafe {
            ptr::copy_nonoverlapping(segment_uuids.add(position * 16), bytes.as_mut_ptr(), 16);
        }
        uuids.push(Uuid::from_bytes(bytes));
    }
    let unique: HashSet<Uuid> = uuids.iter().copied().collect();
    if unique.len() != uuids.len() {
        return Err(invalid_input(format!(
            "segment_uuids contains duplicate UUIDs; len={}, unique={}",
            uuids.len(),
            unique.len()
        )));
    }
    Ok(uuids)
}
