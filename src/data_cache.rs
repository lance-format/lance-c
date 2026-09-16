// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Private bridge between a shared data-cache backend and dataset handles.

use std::fmt::Debug;
use std::sync::Arc;

use lance::Dataset;
use lance_core::Result;

use crate::dataset::LanceDataset;
use crate::error::ffi_try;

/// Data-cache statistics owned by one opened dataset.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LanceDataCacheStatistics {
    /// Requested bytes returned from usable data-cache entries.
    pub bytes_read_from_cache: u64,
    /// Requested bytes returned after a data-cache miss or fallback.
    pub bytes_read_from_remote: u64,
}

pub(crate) trait DatasetDataCache: Debug + Send + Sync {
    fn snapshot(&self) -> LanceDataCacheStatistics;

    fn attach_fresh(&self, dataset: Dataset) -> (Dataset, Arc<dyn DatasetDataCache>);
}

pub(crate) trait DataCacheFactory: Debug + Send + Sync {
    fn attach(&self, dataset: Dataset) -> (Dataset, Arc<dyn DatasetDataCache>);
}

/// Copy this dataset handle's cumulative data-cache statistics into
/// `out_statistics`.
///
/// A dataset not opened with a data cache reports all-zero statistics.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_dataset_get_data_cache_statistics(
    dataset: *const LanceDataset,
    out_statistics: *mut LanceDataCacheStatistics,
) -> i32 {
    ffi_try!(
        unsafe { dataset_get_data_cache_statistics_inner(dataset, out_statistics) },
        neg
    )
}

unsafe fn dataset_get_data_cache_statistics_inner(
    dataset: *const LanceDataset,
    out_statistics: *mut LanceDataCacheStatistics,
) -> Result<i32> {
    if dataset.is_null() || out_statistics.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "dataset and out_statistics must not be NULL".into(),
        ));
    }
    let dataset = unsafe { &*dataset };
    let statistics = dataset
        .data_cache
        .as_ref()
        .map_or_else(LanceDataCacheStatistics::default, |cache| cache.snapshot());
    unsafe {
        std::ptr::write_unaligned(out_statistics, statistics);
    }
    Ok(0)
}
