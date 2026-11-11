// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Owned native Lance schemas and schema-derived operations.
//!
//! `LanceSchema` owns a cloned [`lance_core::datatypes::Schema`] value. Unlike
//! the Arrow export, this preserves field and parent IDs, logical types,
//! encodings, dictionary values, nested metadata, and nullability exactly as
//! the native dataset or operation holds them.

use std::ffi::{c_char, c_int};
use std::slice;

use arrow::ffi::FFI_ArrowSchema;
use arrow_schema::Schema as ArrowSchema;
use lance::dataset::transaction::Operation;
use lance_core::Result;
use lance_core::datatypes::Schema as NativeSchema;

use crate::dataset::LanceDataset;
use crate::error::{ffi_try, swallow_unwind};
use crate::fragment::{LanceFragment, fragment_ref};
use crate::helpers::parse_c_string;
use crate::transaction::{LanceOperation, operation_at};

/// Opaque handle owning a native Lance schema.
pub struct LanceSchema {
    pub(crate) inner: NativeSchema,
}

unsafe fn schema_ref<'a>(schema: *const LanceSchema) -> Result<&'a LanceSchema> {
    if schema.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "schema must not be NULL".into(),
        ));
    }
    Ok(unsafe { &*schema })
}

/// Close a native schema handle. NULL-safe.
///
/// Closing the dataset, fragment, or operation that produced the schema does
/// not invalidate an independently cloned handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_schema_close(schema: *mut LanceSchema) {
    if !schema.is_null() {
        swallow_unwind("lance_schema_close", || unsafe {
            let _ = Box::from_raw(schema);
        });
    }
}

/// Clone a native schema handle into an independently owned handle.
///
/// Returns NULL on error. The clone preserves the complete native schema value;
/// it does not pass through Arrow.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_schema_clone(schema: *const LanceSchema) -> *mut LanceSchema {
    ffi_try!(unsafe { schema_clone_inner(schema) }, null)
}

unsafe fn schema_clone_inner(schema: *const LanceSchema) -> Result<*mut LanceSchema> {
    let schema = unsafe { schema_ref(schema) }?;
    Ok(Box::into_raw(Box::new(LanceSchema {
        inner: schema.inner.clone(),
    })))
}

/// Clone the current snapshot schema of `dataset` into an owned native handle.
///
/// On success writes `*out_schema` and returns 0. On error the output slot is
/// untouched. Close the result with `lance_schema_close()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_schema_clone(
    dataset: *const LanceDataset,
    out_schema: *mut *mut LanceSchema,
) -> i32 {
    ffi_try!(
        unsafe { dataset_schema_clone_inner(dataset, out_schema) },
        neg
    )
}

unsafe fn dataset_schema_clone_inner(
    dataset: *const LanceDataset,
    out_schema: *mut *mut LanceSchema,
) -> Result<i32> {
    if out_schema.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_schema must not be NULL".into(),
        ));
    }
    if dataset.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset must not be NULL".into(),
        ));
    }
    let dataset = unsafe { &*dataset };
    let schema = dataset.snapshot().schema().clone();
    unsafe {
        *out_schema = Box::into_raw(Box::new(LanceSchema { inner: schema }));
    }
    Ok(0)
}

/// Clone the native schema of a fragment from its staged snapshot.
///
/// The fragment schema is the logical dataset schema at the version the
/// fragment was opened from, not a physical file schema. On success writes
/// `*out_schema` and returns 0; on error the output slot is untouched.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_schema_clone(
    fragment: *const LanceFragment,
    out_schema: *mut *mut LanceSchema,
) -> i32 {
    ffi_try!(
        unsafe { fragment_schema_clone_inner(fragment, out_schema) },
        neg
    )
}

unsafe fn fragment_schema_clone_inner(
    fragment: *const LanceFragment,
    out_schema: *mut *mut LanceSchema,
) -> Result<i32> {
    if out_schema.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_schema must not be NULL".into(),
        ));
    }
    let fragment = unsafe { fragment_ref(fragment) }?;
    let schema = fragment.schema().clone();
    unsafe {
        *out_schema = Box::into_raw(Box::new(LanceSchema { inner: schema }));
    }
    Ok(0)
}

/// Clone the in-memory schema of an Overwrite, Merge, or Project operation.
///
/// Other operation kinds return `LANCE_ERR_NOT_SUPPORTED` and leave the output
/// untouched. The clone stays valid after the operation handle is closed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_schema_clone(
    operation: *const LanceOperation,
    out_schema: *mut *mut LanceSchema,
) -> i32 {
    ffi_try!(
        unsafe { operation_schema_clone_inner(operation, out_schema) },
        neg
    )
}

unsafe fn operation_schema_clone_inner(
    operation: *const LanceOperation,
    out_schema: *mut *mut LanceSchema,
) -> Result<i32> {
    if out_schema.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_schema must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let schema = match &operation.inner {
        Operation::Overwrite { schema, .. }
        | Operation::Merge { schema, .. }
        | Operation::Project { schema, .. } => schema.clone(),
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
    unsafe {
        *out_schema = Box::into_raw(Box::new(LanceSchema { inner: schema }));
    }
    Ok(0)
}

/// Export a native schema as Arrow C Data Interface metadata.
///
/// This is an interoperability view only. Arrow cannot represent every native
/// detail (field IDs, encodings, dictionary values), so callers that need a
/// lossless value must clone the native schema instead. The caller owns the
/// written `ArrowSchema` and must release it exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_schema_to_arrow(
    schema: *const LanceSchema,
    out: *mut FFI_ArrowSchema,
) -> i32 {
    ffi_try!(unsafe { schema_to_arrow_inner(schema, out) }, neg)
}

unsafe fn schema_to_arrow_inner(
    schema: *const LanceSchema,
    out: *mut FFI_ArrowSchema,
) -> Result<i32> {
    let schema = unsafe { schema_ref(schema) }?;
    if out.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out must not be NULL".into(),
        ));
    }
    let arrow_schema = ArrowSchema::from(&schema.inner);
    let ffi_schema = FFI_ArrowSchema::try_from(&arrow_schema)?;
    unsafe {
        std::ptr::write_unaligned(out, ffi_schema);
    }
    Ok(0)
}

/// Project a native schema onto an ordered list of column paths.
///
/// `columns` is an array of `num_columns` non-NULL UTF-8 strings. A NULL array
/// is accepted only with `num_columns == 0`. An empty selection is forwarded
/// to native `Schema::project` unchanged. Field paths, ordering, unknown-column
/// errors, and nested projection semantics all come from the native
/// implementation; this function does not rebuild the schema through Arrow.
///
/// On success writes an independently owned handle to `*out_schema` and
/// returns 0. On error the output slot is untouched.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_schema_project(
    schema: *const LanceSchema,
    columns: *const *const c_char,
    num_columns: usize,
    out_schema: *mut *mut LanceSchema,
) -> i32 {
    ffi_try!(
        unsafe { schema_project_inner(schema, columns, num_columns, out_schema) },
        neg
    )
}

unsafe fn schema_project_inner(
    schema: *const LanceSchema,
    columns: *const *const c_char,
    num_columns: usize,
    out_schema: *mut *mut LanceSchema,
) -> Result<i32> {
    let schema = unsafe { schema_ref(schema) }?;
    if out_schema.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_schema must not be NULL".into(),
        ));
    }
    if num_columns > 0 && columns.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            format!("columns must not be NULL when num_columns is {num_columns}").into(),
        ));
    }
    if num_columns > isize::MAX as usize / std::mem::size_of::<*const c_char>() {
        return Err(lance_core::Error::invalid_input_source(
            format!("num_columns {num_columns} overflows the pointer slice length").into(),
        ));
    }

    let mut projected_columns = Vec::with_capacity(num_columns);
    if num_columns > 0 {
        let entries = unsafe { slice::from_raw_parts(columns, num_columns) };
        for (index, entry) in entries.iter().enumerate() {
            if entry.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    format!("columns[{index}] must not be NULL").into(),
                ));
            }
            let column = unsafe { parse_c_string(*entry) }?;
            let column = column.ok_or_else(|| {
                lance_core::Error::invalid_input_source(
                    format!("columns[{index}] must not be NULL").into(),
                )
            })?;
            projected_columns.push(column);
        }
    }

    let projected = schema.inner.project(&projected_columns)?;
    unsafe {
        *out_schema = Box::into_raw(Box::new(LanceSchema { inner: projected }));
    }
    Ok(0)
}

/// Resolve a column path to its native Lance field ID.
///
/// `column` must be a non-NULL UTF-8 field path. Nested paths use the native
/// field-path syntax. On success writes `*out_field_id` and returns 0. On error
/// the output slot is untouched.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_schema_field_id(
    schema: *const LanceSchema,
    column: *const c_char,
    out_field_id: *mut c_int,
) -> i32 {
    ffi_try!(
        unsafe { schema_field_id_inner(schema, column, out_field_id) },
        neg
    )
}

unsafe fn schema_field_id_inner(
    schema: *const LanceSchema,
    column: *const c_char,
    out_field_id: *mut c_int,
) -> Result<i32> {
    let schema = unsafe { schema_ref(schema) }?;
    if out_field_id.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_field_id must not be NULL".into(),
        ));
    }
    if column.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "column must not be NULL".into(),
        ));
    }
    let column = unsafe { parse_c_string(column) }?
        .ok_or_else(|| lance_core::Error::invalid_input_source("column must not be NULL".into()))?;
    let field_id = schema.inner.field_id(column)?;
    unsafe {
        *out_field_id = field_id;
    }
    Ok(0)
}

/// Construct a native Project operation from an owned native schema.
///
/// `preserves_nullability` is the caller's explicit declaration that the
/// projection does not tighten field nullability. It is stored in the native
/// `Operation::Project` value unchanged; conflict resolution and rebase
/// semantics belong to Lance core. The schema is cloned, so the caller may
/// close `schema` immediately after this call.
///
/// On success writes `*out_operation` and returns 0. On error the output slot
/// is untouched. Close the result with `lance_operation_close()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_project_new(
    schema: *const LanceSchema,
    preserves_nullability: bool,
    out_operation: *mut *mut LanceOperation,
) -> i32 {
    ffi_try!(
        unsafe { operation_project_new_inner(schema, preserves_nullability, out_operation) },
        neg
    )
}

unsafe fn operation_project_new_inner(
    schema: *const LanceSchema,
    preserves_nullability: bool,
    out_operation: *mut *mut LanceOperation,
) -> Result<i32> {
    let schema = unsafe { schema_ref(schema) }?;
    if out_operation.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_operation must not be NULL".into(),
        ));
    }
    let operation = Operation::Project {
        schema: schema.inner.clone(),
        preserves_nullability,
    };
    unsafe {
        *out_operation = Box::into_raw(Box::new(LanceOperation { inner: operation }));
    }
    Ok(0)
}

/// Read the native `preserves_nullability` assertion from a Merge or Project
/// operation.
///
/// On success writes `*out_preserves_nullability` and returns 0. Other
/// operation kinds return `LANCE_ERR_NOT_SUPPORTED` and leave the output
/// untouched.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_operation_preserves_nullability(
    operation: *const LanceOperation,
    out_preserves_nullability: *mut bool,
) -> i32 {
    ffi_try!(
        unsafe { operation_preserves_nullability_inner(operation, out_preserves_nullability) },
        neg
    )
}

unsafe fn operation_preserves_nullability_inner(
    operation: *const LanceOperation,
    out_preserves_nullability: *mut bool,
) -> Result<i32> {
    if out_preserves_nullability.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "out_preserves_nullability must not be NULL".into(),
        ));
    }
    let operation = operation_at(operation)?;
    let preserves_nullability = match &operation.inner {
        Operation::Merge {
            preserves_nullability,
            ..
        }
        | Operation::Project {
            preserves_nullability,
            ..
        } => *preserves_nullability,
        other => {
            return Err(lance_core::Error::not_supported_source(
                format!(
                    "preserves_nullability is not supported for {} operations",
                    other.name()
                )
                .into(),
            ));
        }
    };
    unsafe {
        *out_preserves_nullability = preserves_nullability;
    }
    Ok(0)
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;
    use std::ffi::{CString, c_char};
    use std::sync::{Arc, RwLock};

    use arrow::ffi::FFI_ArrowSchema;
    use arrow::record_batch::RecordBatchIterator;
    use arrow_array::builder::ListBuilder;
    use arrow_array::types::Int32Type;
    use arrow_array::{
        ArrayRef, DictionaryArray, Int32Array, Int64Array, RecordBatch, StringArray, StructArray,
    };
    use arrow_schema::{DataType, Field, Fields, Schema as ArrowSchema};
    use lance::Dataset;
    use lance::dataset::WriteParams;
    use lance_core::datatypes::Schema as NativeSchema;

    use super::*;
    use crate::dataset::LanceDataset;
    use crate::error::{LanceErrorCode, error_code_from_lance, lance_last_error_code};
    use crate::runtime::block_on;

    const FIXTURE_FIELD_IDS: &[(&str, i32)] = &[
        ("id", 0),
        ("name", 2),
        ("profile", 3),
        ("profile.score", 4),
        ("profile.tags", 5),
    ];

    fn field(name: &str, data_type: DataType, nullable: bool, extra: &[(&str, &str)]) -> Field {
        let mut metadata = HashMap::new();
        for (key, value) in extra {
            metadata.insert((*key).to_string(), (*value).to_string());
        }
        Field::new(name, data_type, nullable).with_metadata(metadata)
    }

    fn fixture_arrow_schema() -> Arc<ArrowSchema> {
        let score = field(
            "score",
            DataType::Int32,
            true,
            &[("fixture_role", "nested-score")],
        );
        let tags = field(
            "tags",
            DataType::List(Arc::new(Field::new("item", DataType::Utf8, true))),
            true,
            &[("fixture_role", "nested-tags")],
        );
        let profile = field(
            "profile",
            DataType::Struct(Fields::from(vec![score, tags])),
            true,
            &[("fixture_role", "profile")],
        );
        let id = field("id", DataType::Int64, false, &[("fixture_role", "row-id")]);
        let name = field(
            "name",
            DataType::Dictionary(Box::new(DataType::Int32), Box::new(DataType::Utf8)),
            true,
            &[("fixture_role", "dictionary-name")],
        );
        let drop_me = field("drop_me", DataType::Utf8, true, &[]);
        let mut metadata = HashMap::new();
        metadata.insert("fixture".to_string(), "native-schema".to_string());
        metadata.insert("owner".to_string(), "lance-c".to_string());
        Arc::new(ArrowSchema::new_with_metadata(
            vec![id, drop_me, name, profile],
            metadata,
        ))
    }

    fn dictionary_names(keys: &[i32]) -> ArrayRef {
        let values = Arc::new(StringArray::from(vec!["alpha", "beta", "gamma"]));
        let keys = Int32Array::from(keys.to_vec());
        Arc::new(DictionaryArray::<Int32Type>::try_new(keys, values).unwrap())
    }

    fn tags_list(rows: &[&[&str]]) -> ArrayRef {
        let mut builder = ListBuilder::new(arrow_array::builder::StringBuilder::new());
        for tags in rows {
            for tag in *tags {
                builder.values().append_value(*tag);
            }
            builder.append(true);
        }
        Arc::new(builder.finish())
    }

    fn fixture_batch(
        arrow_schema: &Arc<ArrowSchema>,
        ids: &[i64],
        name_keys: &[i32],
    ) -> RecordBatch {
        let names = dictionary_names(name_keys);
        let scores = Arc::new(Int32Array::from(
            ids.iter()
                .map(|id| Some(*id as i32 * 10))
                .collect::<Vec<_>>(),
        ));
        let tag_rows: Vec<Vec<&str>> = ids
            .iter()
            .map(|id| vec!["tag-a", if id % 2 == 0 { "even" } else { "odd" }])
            .collect();
        let tag_refs: Vec<&[&str]> = tag_rows.iter().map(|tags| tags.as_slice()).collect();
        let tags = tags_list(&tag_refs);
        let profile_fields = match &arrow_schema.field(3).data_type() {
            DataType::Struct(fields) => fields.clone(),
            other => panic!("profile must be a struct, got {other:?}"),
        };
        let profile =
            Arc::new(StructArray::try_new(profile_fields, vec![scores, tags], None).unwrap());
        let drop_me = Arc::new(StringArray::from(vec!["drop"; ids.len()]));
        RecordBatch::try_new(
            arrow_schema.clone(),
            vec![
                Arc::new(Int64Array::from(ids.to_vec())),
                drop_me,
                names,
                profile,
            ],
        )
        .unwrap()
    }

    fn create_fixture() -> (tempfile::TempDir, String, NativeSchema) {
        let tmp = tempfile::tempdir().unwrap();
        let uri = tmp
            .path()
            .join("native_schema_fixture")
            .to_str()
            .unwrap()
            .to_string();
        let arrow_schema = fixture_arrow_schema();
        let first = fixture_batch(&arrow_schema, &[1, 2, 3], &[0, 1, 2]);
        let second = fixture_batch(&arrow_schema, &[4, 5], &[1, 0]);
        let third = fixture_batch(&arrow_schema, &[6, 7, 8, 9], &[2, 2, 0, 1]);
        // Writing a new dataset assigns dense field IDs; dropping `drop_me`
        // leaves a real gap, so the surviving IDs are non-contiguous without
        // fabricating them in a binding.
        let dataset = block_on(async {
            let params = WriteParams {
                enable_stable_row_ids: true,
                ..Default::default()
            };
            let mut dataset = Dataset::write(
                RecordBatchIterator::new(vec![Ok(first)], arrow_schema.clone()),
                &uri,
                Some(params),
            )
            .await
            .unwrap();
            dataset
                .append(
                    RecordBatchIterator::new(vec![Ok(second)], arrow_schema.clone()),
                    None,
                )
                .await
                .unwrap();
            dataset
                .append(
                    RecordBatchIterator::new(vec![Ok(third)], arrow_schema),
                    None,
                )
                .await
                .unwrap();
            dataset.drop_columns(&["drop_me"]).await.unwrap();
            dataset.delete("id = 2").await.unwrap();
            assert_eq!(dataset.get_fragments().len(), 3);
            assert_eq!(dataset.count_rows(None).await.unwrap(), 8);
            dataset
        });
        let schema = dataset.schema().clone();
        (tmp, uri, schema)
    }

    fn schema_handle(schema: NativeSchema) -> *mut LanceSchema {
        Box::into_raw(Box::new(LanceSchema { inner: schema }))
    }

    unsafe fn cloned_schema(handle: *const LanceSchema) -> NativeSchema {
        let clone = unsafe { lance_schema_clone(handle) };
        assert!(!clone.is_null());
        unsafe {
            let schema = (*clone).inner.clone();
            lance_schema_close(clone);
            schema
        }
    }

    #[test]
    fn native_schema_clone_preserves_ids_dictionary_and_metadata() {
        let (_tmp, uri, expected) = create_fixture();
        assert_eq!(
            expected.metadata.get("fixture").map(String::as_str),
            Some("native-schema")
        );
        let name = expected.field("name").unwrap();
        assert!(
            name.dictionary.is_some(),
            "fixture must carry dictionary values"
        );
        assert_eq!(
            name.metadata.get("fixture_role").map(String::as_str),
            Some("dictionary-name")
        );
        assert_eq!(expected.field("profile.tags").unwrap().parent_id, 3);

        let dataset = block_on(Dataset::open(&uri)).unwrap();
        let reopened_schema = dataset.schema().clone();
        let handle = Box::new(LanceDataset {
            inner: RwLock::new(Arc::new(dataset)),
        });
        let mut cloned = std::ptr::null_mut();
        assert_eq!(
            unsafe { lance_dataset_schema_clone(&*handle, &mut cloned) },
            0
        );
        assert!(!cloned.is_null());
        let cloned_value = unsafe { (*cloned).inner.clone() };
        assert_eq!(cloned_value, reopened_schema);
        assert!(
            cloned_value.field("name").unwrap().dictionary.is_some(),
            "reopened schema must retain the dictionary encoding"
        );

        // The clone is independent of the dataset handle.
        unsafe { lance_schema_close(cloned) };
        drop(handle);

        let schema = schema_handle(expected.clone());
        let clone = unsafe { cloned_schema(schema) };
        assert_eq!(clone, expected);
        assert_eq!(clone.field_id("profile.tags").unwrap(), 5);
        assert_eq!(clone.field_id("id").unwrap(), 0);
        assert_eq!(clone.field_id("name").unwrap(), 2);

        for (path, id) in FIXTURE_FIELD_IDS {
            let mut out = -1;
            let column = CString::new(*path).unwrap();
            assert_eq!(
                unsafe { lance_schema_field_id(schema, column.as_ptr(), &mut out) },
                0,
                "field_id failed for {path}"
            );
            assert_eq!(out, *id, "field ID mismatch for {path}");
        }

        let missing = CString::new("does_not_exist").unwrap();
        let mut sentinel = -123;
        assert_eq!(
            unsafe { lance_schema_field_id(schema, missing.as_ptr(), &mut sentinel) },
            -1
        );
        assert_eq!(sentinel, -123);
        let native_error = expected.field_id("does_not_exist").unwrap_err();
        assert_eq!(
            lance_last_error_code(),
            error_code_from_lance(&native_error)
        );
        unsafe { lance_schema_close(schema) };
    }

    #[test]
    fn native_schema_project_matches_core_semantics() {
        let (_tmp, _uri, expected) = create_fixture();
        let schema = schema_handle(expected.clone());
        let columns = [
            CString::new("profile.tags").unwrap(),
            CString::new("id").unwrap(),
        ];
        let pointers: Vec<*const c_char> = columns.iter().map(|column| column.as_ptr()).collect();
        let mut projected = std::ptr::null_mut();
        assert_eq!(
            unsafe {
                lance_schema_project(schema, pointers.as_ptr(), pointers.len(), &mut projected)
            },
            0
        );
        let native_projected = expected.project(&["profile.tags", "id"]).unwrap();
        let projected_value = unsafe { (*projected).inner.clone() };
        assert_eq!(projected_value, native_projected);
        assert_eq!(
            projected_value
                .fields
                .iter()
                .map(|field| field.name.as_str())
                .collect::<Vec<_>>(),
            vec!["profile", "id"]
        );
        assert_eq!(projected_value.field_id("profile.tags").unwrap(), 5);
        assert_eq!(
            projected_value.metadata.get("fixture").map(String::as_str),
            Some("native-schema")
        );
        unsafe {
            lance_schema_close(projected);
            lance_schema_close(schema);
        }

        // Unknown columns are rejected by native `Schema::project`; the output
        // slot must remain untouched.
        let schema = schema_handle(expected.clone());
        let missing = CString::new("missing.column").unwrap();
        let missing_ptr = missing.as_ptr();
        let mut sentinel = Box::into_raw(Box::new(LanceSchema {
            inner: expected.clone(),
        }));
        let sentinel_value = sentinel;
        assert_eq!(
            unsafe { lance_schema_project(schema, &missing_ptr, 1, &mut sentinel) },
            -1
        );
        assert_eq!(sentinel, sentinel_value);
        assert!(unsafe { (*sentinel).inner == expected });
        let native_error = expected.project(&["missing.column"]).unwrap_err();
        assert_eq!(
            lance_last_error_code(),
            error_code_from_lance(&native_error)
        );
        unsafe {
            lance_schema_close(sentinel_value);
            lance_schema_close(schema);
        }

        // Empty selection is forwarded as-is; native project yields an empty
        // field list with root metadata preserved.
        let schema = schema_handle(expected.clone());
        let mut empty = std::ptr::null_mut();
        assert_eq!(
            unsafe { lance_schema_project(schema, std::ptr::null(), 0, &mut empty) },
            0
        );
        let empty_value = unsafe { (*empty).inner.clone() };
        assert!(empty_value.fields.is_empty());
        assert_eq!(empty_value.metadata, expected.metadata);
        unsafe {
            lance_schema_close(empty);
            lance_schema_close(schema);
        }

        // Pointer/count guards run before slicing.
        let schema = schema_handle(expected.clone());
        let mut untouched = std::ptr::null_mut();
        assert_eq!(
            unsafe { lance_schema_project(schema, std::ptr::null(), 1, &mut untouched) },
            -1
        );
        assert!(untouched.is_null());
        assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);
        assert_eq!(
            unsafe { lance_schema_project(schema, std::ptr::null(), usize::MAX, &mut untouched,) },
            -1
        );
        assert!(untouched.is_null());
        assert_eq!(lance_last_error_code(), LanceErrorCode::InvalidArgument);

        let null_entry = [std::ptr::null::<c_char>()];
        let mut null_output = std::ptr::null_mut();
        assert_eq!(
            unsafe { lance_schema_project(schema, null_entry.as_ptr(), 1, &mut null_output) },
            -1
        );
        assert!(null_output.is_null());
        unsafe { lance_schema_close(schema) };
    }

    #[test]
    fn operation_schema_and_nullability_inspection() {
        let (_tmp, _uri, expected) = create_fixture();
        let project = LanceOperation {
            inner: Operation::Project {
                schema: expected.clone(),
                preserves_nullability: true,
            },
        };
        let mut cloned = std::ptr::null_mut();
        assert_eq!(
            unsafe { lance_operation_schema_clone(&project, &mut cloned) },
            0
        );
        assert_eq!(unsafe { (*cloned).inner.clone() }, expected);
        unsafe { lance_schema_close(cloned) };

        let mut preserves = false;
        assert_eq!(
            unsafe { lance_operation_preserves_nullability(&project, &mut preserves) },
            0
        );
        assert!(preserves);

        let merge = LanceOperation {
            inner: Operation::Merge {
                fragments: vec![],
                schema: expected.clone(),
                preserves_nullability: false,
            },
        };
        let mut merge_clone = std::ptr::null_mut();
        assert_eq!(
            unsafe { lance_operation_schema_clone(&merge, &mut merge_clone) },
            0
        );
        assert_eq!(unsafe { (*merge_clone).inner.clone() }, expected);
        unsafe { lance_schema_close(merge_clone) };
        assert_eq!(
            unsafe { lance_operation_preserves_nullability(&merge, &mut preserves) },
            0
        );
        assert!(!preserves);

        let overwrite = LanceOperation {
            inner: Operation::Overwrite {
                fragments: vec![],
                schema: expected.clone(),
                config_upsert_values: None,
                initial_bases: None,
            },
        };
        let mut overwrite_clone = std::ptr::null_mut();
        assert_eq!(
            unsafe { lance_operation_schema_clone(&overwrite, &mut overwrite_clone) },
            0
        );
        assert_eq!(unsafe { (*overwrite_clone).inner.clone() }, expected);
        unsafe { lance_schema_close(overwrite_clone) };

        let delete = LanceOperation {
            inner: Operation::Delete {
                updated_fragments: vec![],
                deleted_fragment_ids: vec![],
                predicate: "id = 1".to_string(),
            },
        };
        let sentinel = Box::into_raw(Box::new(LanceSchema {
            inner: expected.clone(),
        }));
        let mut out = sentinel;
        assert_eq!(
            unsafe { lance_operation_schema_clone(&delete, &mut out) },
            -1
        );
        assert_eq!(out, sentinel);
        assert_eq!(lance_last_error_code(), LanceErrorCode::NotSupported);
        let mut preserves_sentinel = true;
        assert_eq!(
            unsafe { lance_operation_preserves_nullability(&delete, &mut preserves_sentinel) },
            -1
        );
        assert!(preserves_sentinel);
        unsafe {
            lance_schema_close(sentinel);
        }
    }

    #[test]
    fn project_constructor_keeps_explicit_nullability_and_schema() {
        let (_tmp, _uri, expected) = create_fixture();
        let schema = schema_handle(expected.clone());
        for preserves in [false, true] {
            let mut out: *mut LanceOperation = std::ptr::null_mut();
            assert_eq!(
                unsafe { lance_operation_project_new(schema, preserves, &mut out) },
                0
            );
            assert!(!out.is_null());
            match unsafe { &(*out).inner } {
                Operation::Project {
                    schema: stored,
                    preserves_nullability,
                } => {
                    assert_eq!(*stored, expected);
                    assert_eq!(*preserves_nullability, preserves);
                }
                other => panic!("expected Project, got {other:?}"),
            }
            unsafe { crate::transaction::lance_operation_close(out) };
        }
        // Schema can be closed immediately; the operation owns its clone.
        unsafe { lance_schema_close(schema) };
    }

    #[test]
    fn schema_to_arrow_is_an_interop_view() {
        let (_tmp, _uri, expected) = create_fixture();
        let schema = schema_handle(expected.clone());
        let mut out = std::mem::MaybeUninit::<FFI_ArrowSchema>::uninit();
        assert_eq!(
            unsafe { lance_schema_to_arrow(schema, out.as_mut_ptr()) },
            0
        );
        let arrow = unsafe { out.assume_init() };
        // Dropping `FFI_ArrowSchema` releases the exported Arrow schema.
        drop(arrow);
        unsafe { lance_schema_close(schema) };
    }
}
