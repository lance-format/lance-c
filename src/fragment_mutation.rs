// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Native fragment mutation and staging.
//!
//! Every operation starts from the fragment's owned dataset snapshot. Methods
//! that consume or mutably borrow the native fragment operate on a clone, so
//! the caller's source handle remains unchanged. Data and commit validation
//! stay in Lance; this module owns only foreign representation, ownership and
//! error conversion.

use std::ffi::c_char;
use std::ptr;

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi_stream::FFI_ArrowArrayStream;
use lance::dataset::NewColumnTransform;
use lance::dataset::fragment::FragmentUpdateColumnsResult;
use lance_core::Result;
use lance_file::version::ConcreteFileVersion;
use lance_table::format::Fragment;

use crate::add_columns::{LanceSqlColumn, parse_sql_columns};
use crate::batch_udf::{LanceBatchUDF, batch_udf_transform};
use crate::error::{ffi_try, swallow_unwind};
use crate::fragment::{LanceFragment, fragment_ref};
use crate::helpers;
use crate::runtime::block_on;
use crate::schema::{LanceSchema, schema_ref};
use crate::transaction::LanceDataReplacementGroup;

/// Owned native `FragmentUpdateColumnsResult`.
///
/// The value contains complete updated fragment metadata, the native modified
/// field IDs and the native matched physical row offsets.
pub struct LanceFragmentUpdateColumnsResult {
    pub(crate) inner: FragmentUpdateColumnsResult,
}

fn require_output<T>(output: *mut T, label: &str) -> Result<()> {
    if output.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            format!("{label} must not be NULL").into(),
        ));
    }
    Ok(())
}

fn require_fragment_schema_outputs(
    out_fragment: *mut *mut LanceFragment,
    out_schema: *mut *mut LanceSchema,
) -> Result<()> {
    require_output(out_fragment, "out_fragment")?;
    require_output(out_schema, "out_schema")
}

unsafe fn write_fragment_schema_outputs(
    out_fragment: *mut *mut LanceFragment,
    out_schema: *mut *mut LanceSchema,
    fragment: Fragment,
    schema: lance_core::datatypes::Schema,
) {
    let fragment = Box::new(LanceFragment::from_metadata(fragment));
    let schema = Box::new(LanceSchema { inner: schema });
    unsafe {
        ptr::write(out_fragment, Box::into_raw(fragment));
        ptr::write(out_schema, Box::into_raw(schema));
    }
}

fn optional_u32(value: *const u32) -> Option<u32> {
    if value.is_null() {
        None
    } else {
        // SAFETY: a non-NULL pointer promises one readable u32 for this call.
        Some(unsafe { *value })
    }
}

fn required_string<'a>(value: *const c_char, label: &str) -> Result<&'a str> {
    unsafe { helpers::parse_c_string(value) }?.ok_or_else(|| {
        lance_core::Error::invalid_input_source(format!("{label} must not be NULL").into())
    })
}

fn file_version(value: i32) -> Result<ConcreteFileVersion> {
    match value {
        0 => Ok(ConcreteFileVersion::V1),
        1 => Ok(ConcreteFileVersion::V2_0),
        2 => Ok(ConcreteFileVersion::V2_1),
        3 => Ok(ConcreteFileVersion::V2_2),
        4 => Ok(ConcreteFileVersion::V2_3),
        other => Err(lance_core::Error::invalid_input_source(
            format!(
                "file_version must be one of LanceFileFormatVersion V1=0, \
                 V2_0=1, V2_1=2, V2_2=3, V2_3=4, got {other}"
            )
            .into(),
        )),
    }
}

/// Add columns computed from SQL expressions to one bound fragment.
///
/// `batch_size == NULL` maps to native `None`; a non-NULL pointer is forwarded
/// as `Some(value)`, including zero. Both outputs are success-only and own
/// detached fragment metadata plus the complete native final schema.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_add_columns_sql(
    fragment: *const LanceFragment,
    columns: *const LanceSqlColumn,
    column_count: usize,
    batch_size: *const u32,
    out_fragment: *mut *mut LanceFragment,
    out_schema: *mut *mut LanceSchema,
) -> i32 {
    ffi_try!(
        unsafe {
            fragment_add_columns_sql_inner(
                fragment,
                columns,
                column_count,
                batch_size,
                out_fragment,
                out_schema,
            )
        },
        neg
    )
}

unsafe fn fragment_add_columns_sql_inner(
    fragment: *const LanceFragment,
    columns: *const LanceSqlColumn,
    column_count: usize,
    batch_size: *const u32,
    out_fragment: *mut *mut LanceFragment,
    out_schema: *mut *mut LanceSchema,
) -> Result<i32> {
    let fragment = unsafe { fragment_ref(fragment) }?;
    require_fragment_schema_outputs(out_fragment, out_schema)?;
    let columns = unsafe { parse_sql_columns(columns, column_count) }?;
    let (updated, schema) = block_on(fragment.bound()?.add_columns(
        NewColumnTransform::SqlExpressions(columns),
        None,
        optional_u32(batch_size),
    ))?;
    unsafe { write_fragment_schema_outputs(out_fragment, out_schema, updated, schema) };
    Ok(0)
}

/// Add all-null columns described by a borrowed Arrow schema.
///
/// The Arrow schema supplies only logical field definitions. Native allocates
/// new field identities and returns the complete final native schema.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_add_columns_nulls(
    fragment: *const LanceFragment,
    columns: *const FFI_ArrowSchema,
    out_fragment: *mut *mut LanceFragment,
    out_schema: *mut *mut LanceSchema,
) -> i32 {
    ffi_try!(
        unsafe { fragment_add_columns_nulls_inner(fragment, columns, out_fragment, out_schema) },
        neg
    )
}

unsafe fn fragment_add_columns_nulls_inner(
    fragment: *const LanceFragment,
    columns: *const FFI_ArrowSchema,
    out_fragment: *mut *mut LanceFragment,
    out_schema: *mut *mut LanceSchema,
) -> Result<i32> {
    let fragment = unsafe { fragment_ref(fragment) }?;
    require_fragment_schema_outputs(out_fragment, out_schema)?;
    let columns = unsafe { helpers::import_arrow_schema(columns, "columns") }?;
    let (updated, schema) = block_on(fragment.bound()?.add_columns(
        NewColumnTransform::AllNulls(columns),
        None,
        None,
    ))?;
    unsafe { write_fragment_schema_outputs(out_fragment, out_schema, updated, schema) };
    Ok(0)
}

/// Add columns read from a consumed Arrow stream.
///
/// This follows native `add_columns_from_reader`: `read_columns` is native
/// `None`. A non-NULL stream is consumed exactly once even if another argument
/// is invalid. `batch_size == NULL` maps to `None`; zero remains `Some(0)`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_add_columns_stream(
    fragment: *const LanceFragment,
    stream: *mut FFI_ArrowArrayStream,
    batch_size: *const u32,
    out_fragment: *mut *mut LanceFragment,
    out_schema: *mut *mut LanceSchema,
) -> i32 {
    ffi_try!(
        unsafe {
            fragment_add_columns_stream_inner(
                fragment,
                stream,
                batch_size,
                out_fragment,
                out_schema,
            )
        },
        neg
    )
}

unsafe fn fragment_add_columns_stream_inner(
    fragment: *const LanceFragment,
    stream: *mut FFI_ArrowArrayStream,
    batch_size: *const u32,
    out_fragment: *mut *mut LanceFragment,
    out_schema: *mut *mut LanceSchema,
) -> Result<i32> {
    let reader = unsafe { helpers::take_arrow_array_stream(stream)? };
    let fragment = unsafe { fragment_ref(fragment) }?;
    require_fragment_schema_outputs(out_fragment, out_schema)?;
    let (updated, schema) = block_on(fragment.bound()?.add_columns(
        NewColumnTransform::Reader(Box::new(reader)),
        None,
        optional_u32(batch_size),
    ))?;
    unsafe { write_fragment_schema_outputs(out_fragment, out_schema, updated, schema) };
    Ok(0)
}

/// Add columns through a borrowed BatchUDF declaration.
///
/// The conversion adapter and every checkpoint callback are shared with the
/// dataset add-columns path. `read_columns == NULL` is native `None`; a
/// non-NULL NULL-terminated array is `Some`, including an empty array.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_add_columns_batch_udf(
    fragment: *const LanceFragment,
    udf: *const LanceBatchUDF,
    read_columns: *const *const c_char,
    batch_size: *const u32,
    out_fragment: *mut *mut LanceFragment,
    out_schema: *mut *mut LanceSchema,
) -> i32 {
    ffi_try!(
        unsafe {
            fragment_add_columns_batch_udf_inner(
                fragment,
                udf,
                read_columns,
                batch_size,
                out_fragment,
                out_schema,
            )
        },
        neg
    )
}

unsafe fn fragment_add_columns_batch_udf_inner(
    fragment: *const LanceFragment,
    udf: *const LanceBatchUDF,
    read_columns: *const *const c_char,
    batch_size: *const u32,
    out_fragment: *mut *mut LanceFragment,
    out_schema: *mut *mut LanceSchema,
) -> Result<i32> {
    let fragment = unsafe { fragment_ref(fragment) }?;
    require_fragment_schema_outputs(out_fragment, out_schema)?;
    let transform = unsafe { batch_udf_transform(udf)? };
    let read_columns = unsafe { helpers::parse_c_string_array(read_columns)? };
    let (updated, schema) = block_on(fragment.bound()?.add_columns(
        transform,
        read_columns,
        optional_u32(batch_size),
    ))?;
    unsafe { write_fragment_schema_outputs(out_fragment, out_schema, updated, schema) };
    Ok(0)
}

/// Merge a right-hand Arrow stream into one bound fragment with a native join.
///
/// The native fragment is cloned before the mutable operation. A non-NULL
/// stream is consumed exactly once. Both outputs are success-only and contain
/// detached fragment metadata plus the complete native final schema.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_merge_columns(
    fragment: *const LanceFragment,
    stream: *mut FFI_ArrowArrayStream,
    left_on: *const c_char,
    right_on: *const c_char,
    out_fragment: *mut *mut LanceFragment,
    out_schema: *mut *mut LanceSchema,
) -> i32 {
    ffi_try!(
        unsafe {
            fragment_merge_columns_inner(
                fragment,
                stream,
                left_on,
                right_on,
                out_fragment,
                out_schema,
            )
        },
        neg
    )
}

unsafe fn fragment_merge_columns_inner(
    fragment: *const LanceFragment,
    stream: *mut FFI_ArrowArrayStream,
    left_on: *const c_char,
    right_on: *const c_char,
    out_fragment: *mut *mut LanceFragment,
    out_schema: *mut *mut LanceSchema,
) -> Result<i32> {
    let reader = unsafe { helpers::take_arrow_array_stream(stream)? };
    let fragment = unsafe { fragment_ref(fragment) }?;
    require_fragment_schema_outputs(out_fragment, out_schema)?;
    let left_on = required_string(left_on, "left_on")?;
    let right_on = required_string(right_on, "right_on")?;

    let mut native = fragment.bound()?.clone();
    let (updated, schema) = block_on(native.merge_columns(reader, left_on, right_on))?;
    unsafe { write_fragment_schema_outputs(out_fragment, out_schema, updated, schema) };
    Ok(0)
}

/// Rewrite existing columns in one bound fragment through a native join.
///
/// The native call is `FileFragment::update_columns_with_offsets`; the owned
/// result retains complete fragment metadata, modified field IDs and matched
/// physical row offsets. A non-NULL stream is consumed exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_update_columns(
    fragment: *const LanceFragment,
    stream: *mut FFI_ArrowArrayStream,
    left_on: *const c_char,
    right_on: *const c_char,
    out_result: *mut *mut LanceFragmentUpdateColumnsResult,
) -> i32 {
    ffi_try!(
        unsafe { fragment_update_columns_inner(fragment, stream, left_on, right_on, out_result) },
        neg
    )
}

unsafe fn fragment_update_columns_inner(
    fragment: *const LanceFragment,
    stream: *mut FFI_ArrowArrayStream,
    left_on: *const c_char,
    right_on: *const c_char,
    out_result: *mut *mut LanceFragmentUpdateColumnsResult,
) -> Result<i32> {
    let reader = unsafe { helpers::take_arrow_array_stream(stream)? };
    let fragment = unsafe { fragment_ref(fragment) }?;
    require_output(out_result, "out_result")?;
    let left_on = required_string(left_on, "left_on")?;
    let right_on = required_string(right_on, "right_on")?;

    let mut native = fragment.bound()?.clone();
    let result = block_on(native.update_columns_with_offsets(reader, left_on, right_on))?;
    let result = Box::new(LanceFragmentUpdateColumnsResult { inner: result });
    unsafe {
        ptr::write(out_result, Box::into_raw(result));
    }
    Ok(0)
}

/// Stage a replacement data file for existing fragment columns.
///
/// The supplied native schema is borrowed and forwarded unchanged. The
/// consumed Arrow stream must align with the fragment's physical rows,
/// including deleted slots. Native owns V2 version/layout/Blob validation and
/// staged-file cleanup on failure.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_write_columns(
    fragment: *const LanceFragment,
    schema: *const LanceSchema,
    stream: *mut FFI_ArrowArrayStream,
    out_group: *mut *mut LanceDataReplacementGroup,
) -> i32 {
    ffi_try!(
        unsafe { fragment_write_columns_inner(fragment, schema, stream, out_group, None) },
        neg
    )
}

/// Stage a replacement data file using an explicit native file-format version.
///
/// `file_version` uses the `LanceFileFormatVersion` discriminants. Native
/// rejects V1 and incompatible V2 targets; the binding adds no format policy.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_write_column_with_version(
    fragment: *const LanceFragment,
    schema: *const LanceSchema,
    stream: *mut FFI_ArrowArrayStream,
    file_version_value: i32,
    out_group: *mut *mut LanceDataReplacementGroup,
) -> i32 {
    ffi_try!(
        unsafe {
            fragment_write_columns_inner(
                fragment,
                schema,
                stream,
                out_group,
                Some(file_version_value),
            )
        },
        neg
    )
}

unsafe fn fragment_write_columns_inner(
    fragment: *const LanceFragment,
    schema: *const LanceSchema,
    stream: *mut FFI_ArrowArrayStream,
    out_group: *mut *mut LanceDataReplacementGroup,
    file_version_value: Option<i32>,
) -> Result<i32> {
    let reader = unsafe { helpers::take_arrow_array_stream(stream)? };
    let fragment = unsafe { fragment_ref(fragment) }?;
    let schema = unsafe { schema_ref(schema) }?;
    require_output(out_group, "out_group")?;
    let write_version = file_version_value.map(file_version).transpose()?;
    let data = futures::stream::iter(reader.map(|batch| {
        batch.map_err(|error| {
            lance_core::Error::invalid_input(format!("Arrow replacement stream error: {error}"))
        })
    }));

    let group = match write_version {
        Some(version) => block_on(fragment.bound()?.write_column_with_version(
            data,
            &schema.inner,
            version,
        ))?,
        None => block_on(fragment.bound()?.write_columns(data, &schema.inner))?,
    };
    let group = Box::new(LanceDataReplacementGroup { inner: group });
    unsafe {
        ptr::write(out_group, Box::into_raw(group));
    }
    Ok(0)
}

/// Add physical row offsets to a clone of one bound fragment.
///
/// Offsets are forwarded unchanged to native `FileFragment::extend_deletions`.
/// Native owns duplicate, range, full-deletion and deletion-file behavior. A
/// successful full deletion returns NULL; a partial deletion returns a bound
/// native fragment.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_delete_rows(
    fragment: *const LanceFragment,
    offsets: *const u32,
    offset_count: usize,
    out_fragment: *mut *mut LanceFragment,
) -> i32 {
    ffi_try!(
        unsafe { fragment_delete_rows_inner(fragment, offsets, offset_count, out_fragment) },
        neg
    )
}

unsafe fn fragment_delete_rows_inner(
    fragment: *const LanceFragment,
    offsets: *const u32,
    offset_count: usize,
    out_fragment: *mut *mut LanceFragment,
) -> Result<i32> {
    let fragment = unsafe { fragment_ref(fragment) }?;
    require_output(out_fragment, "out_fragment")?;
    let offsets =
        unsafe { helpers::required_slice(offsets, offset_count, "offsets", "offset_count")? };

    let updated = block_on(
        fragment
            .bound()?
            .clone()
            .extend_deletions(offsets.iter().copied()),
    )?;
    let updated = match updated {
        Some(updated) => Box::into_raw(Box::new(LanceFragment::from_native(updated))),
        None => ptr::null_mut(),
    };
    unsafe {
        ptr::write(out_fragment, updated);
    }
    Ok(0)
}

fn result_ref<'a>(
    result: *const LanceFragmentUpdateColumnsResult,
) -> Result<&'a LanceFragmentUpdateColumnsResult> {
    if result.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "result must not be NULL".into(),
        ));
    }
    Ok(unsafe { &*result })
}

/// Close an update-columns result. NULL-safe.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_update_columns_result_close(
    result: *mut LanceFragmentUpdateColumnsResult,
) {
    if !result.is_null() {
        swallow_unwind("lance_fragment_update_columns_result_close", || unsafe {
            drop(Box::from_raw(result));
        });
    }
}

/// Clone the updated fragment metadata into an independent handle.
///
/// The returned handle is detached and owns complete native metadata. The
/// output slot is written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_update_columns_result_fragment(
    result: *const LanceFragmentUpdateColumnsResult,
    out_fragment: *mut *mut LanceFragment,
) -> i32 {
    ffi_try!(
        unsafe { fragment_update_columns_result_fragment_inner(result, out_fragment) },
        neg
    )
}

unsafe fn fragment_update_columns_result_fragment_inner(
    result: *const LanceFragmentUpdateColumnsResult,
    out_fragment: *mut *mut LanceFragment,
) -> Result<i32> {
    let result = result_ref(result)?;
    require_output(out_fragment, "out_fragment")?;
    let fragment = Box::new(LanceFragment::from_metadata(result.inner.fragment.clone()));
    unsafe {
        ptr::write(out_fragment, Box::into_raw(fragment));
    }
    Ok(0)
}

/// Read the number of modified field IDs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_update_columns_result_fields_modified_count(
    result: *const LanceFragmentUpdateColumnsResult,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let result = result_ref(result)?;
            require_output(out_count, "out_count")?;
            unsafe {
                ptr::write(out_count, result.inner.fields_modified.len() as u64);
            }
            Ok(0)
        })(),
        neg
    )
}

/// Read one modified field ID in native order.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_update_columns_result_fields_modified_at(
    result: *const LanceFragmentUpdateColumnsResult,
    index: usize,
    out_field_id: *mut u32,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let result = result_ref(result)?;
            require_output(out_field_id, "out_field_id")?;
            let field_id = result.inner.fields_modified.get(index).ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!(
                        "modified field index {index} is out of range for {} fields",
                        result.inner.fields_modified.len()
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

/// Read the number of matched physical row offsets.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_update_columns_result_matched_offset_count(
    result: *const LanceFragmentUpdateColumnsResult,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let result = result_ref(result)?;
            require_output(out_count, "out_count")?;
            unsafe {
                ptr::write(out_count, result.inner.matched_offsets.len());
            }
            Ok(0)
        })(),
        neg
    )
}

/// Read one matched physical row offset in stable native bitmap order.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_update_columns_result_matched_offset_at(
    result: *const LanceFragmentUpdateColumnsResult,
    index: usize,
    out_offset: *mut u32,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let result = result_ref(result)?;
            require_output(out_offset, "out_offset")?;
            let offset = result
                .inner
                .matched_offsets
                .iter()
                .nth(index)
                .ok_or_else(|| {
                    lance_core::Error::invalid_input_source(
                        format!(
                            "matched offset index {index} is out of range for {} offsets",
                            result.inner.matched_offsets.len()
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

/// Copy all matched physical row offsets in one native bitmap traversal.
/// The caller owns the output buffer; its element count must match the result.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_update_columns_result_matched_offsets(
    result: *const LanceFragmentUpdateColumnsResult,
    out_offsets: *mut u32,
    count: usize,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let result = result_ref(result)?;
            if count as u64 != result.inner.matched_offsets.len() {
                return Err(lance_core::Error::invalid_input_source(
                    format!(
                        "matched offset output count {count} must equal {}",
                        result.inner.matched_offsets.len()
                    )
                    .into(),
                ));
            }
            helpers::validate_slice_length::<u32>(count, "out_offsets")?;
            if count != 0 {
                require_output(out_offsets, "out_offsets")?;
                for (index, offset) in result.inner.matched_offsets.iter().enumerate() {
                    unsafe { ptr::write(out_offsets.add(index), offset) };
                }
            }
            Ok(0)
        })(),
        neg
    )
}

/// Serialize the matched offsets with the portable Roaring format.
///
/// This is an interoperability export only; typed Update construction consumes
/// the offset getters directly. The malloc-compatible buffer belongs to the
/// caller and must be released with `lance_free_bytes`. Both outputs are
/// success-only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_update_columns_result_matched_offsets_serialize(
    result: *const LanceFragmentUpdateColumnsResult,
    out_bytes: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    ffi_try!(
        (|| -> Result<i32> {
            let result = result_ref(result)?;
            require_output(out_bytes, "out_bytes")?;
            require_output(out_len, "out_len")?;

            let mut bytes = Vec::with_capacity(result.inner.matched_offsets.serialized_size());
            result
                .inner
                .matched_offsets
                .serialize_into(&mut bytes)
                .map_err(|error| {
                    lance_core::Error::internal(format!(
                        "failed to serialize matched row offsets: {error}"
                    ))
                })?;
            let len = bytes.len();
            let allocation = if len == 0 {
                ptr::null_mut()
            } else {
                let allocation = unsafe { libc::malloc(len) }.cast::<u8>();
                if allocation.is_null() {
                    return Err(lance_core::Error::internal(format!(
                        "failed to allocate {len} matched-offset bytes"
                    )));
                }
                unsafe { ptr::copy_nonoverlapping(bytes.as_ptr(), allocation, len) };
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matched_offsets_copy_preserves_values_and_error_outputs() {
        for expected in [
            vec![],
            vec![0, 65_535, 65_536, u32::MAX],
            (0..65_537).collect(),
        ] {
            let result = LanceFragmentUpdateColumnsResult {
                inner: FragmentUpdateColumnsResult {
                    fragment: Fragment::new(0),
                    fields_modified: vec![],
                    matched_offsets: expected.iter().copied().collect(),
                },
            };
            let mut copied = vec![u32::MAX; expected.len()];
            assert_eq!(
                unsafe {
                    lance_fragment_update_columns_result_matched_offsets(
                        &result,
                        copied.as_mut_ptr(),
                        copied.len(),
                    )
                },
                0
            );
            assert_eq!(copied, expected);
            let mut sentinel = [41, 42];
            for count in [0, expected.len() + 1, usize::MAX]
                .into_iter()
                .filter(|&count| count != expected.len())
            {
                assert_eq!(
                    unsafe {
                        lance_fragment_update_columns_result_matched_offsets(
                            &result,
                            sentinel.as_mut_ptr(),
                            count,
                        )
                    },
                    -1
                );
                assert_eq!(sentinel, [41, 42]);
                assert_eq!(
                    crate::lance_last_error_code(),
                    crate::LanceErrorCode::LANCE_ERR_INVALID_ARGUMENT
                );
            }
            assert_eq!(
                unsafe {
                    lance_fragment_update_columns_result_matched_offsets(
                        &result,
                        ptr::null_mut(),
                        expected.len(),
                    )
                },
                if expected.is_empty() { 0 } else { -1 }
            );
        }
        let mut sentinel = 43;
        assert_eq!(
            unsafe {
                lance_fragment_update_columns_result_matched_offsets(ptr::null(), &mut sentinel, 1)
            },
            -1
        );
        assert_eq!(sentinel, 43);
    }
}
