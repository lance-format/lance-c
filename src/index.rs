// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Index lifecycle C API: create vector / scalar indexes, drop, list.
//!
//! Index creation mutates the dataset under an exclusive write lock; existing
//! scanners that already cloned the inner Arc keep their snapshot view.

use std::ffi::c_char;

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
