// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Native data-overlay values and fragment overlay writers.
//!
//! An overlay supplies replacement values for a subset of a fragment's
//! `(offset_in_fragment, field)` cells. The value handles here own a native
//! `DataOverlayFile` or `DataOverlayGroup` snapshot, so they remain valid after
//! the dataset, fragment, or writer that produced them is closed.
//!
//! Coverage is passed across the ABI as portable 32-bit Roaring bitmap bytes.
//! Native code owns field compatibility, row layout, and publication checks;
//! this module only converts representations and enforces the documented ABI
//! ownership rules.

use std::ptr;
use std::slice;
use std::sync::Arc;

use lance::dataset::OverlayWriter;
use lance_core::Result;
use lance_table::format::overlay::{DataOverlayFile, OverlayCoverage};
use lance_table::transaction::DataOverlayGroup;
use roaring::RoaringBitmap;

use crate::batch::LanceBatch;
use crate::data_file::LanceDataFile;
use crate::error::{ffi_try, swallow_unwind};
use crate::fragment::{LanceFragment, fragment_ref};
use crate::runtime::block_on;
use crate::schema::LanceSchema;

/// Owned native `DataOverlayFile` snapshot.
pub struct LanceDataOverlayFile {
    pub(crate) inner: DataOverlayFile,
}

/// Owned native `DataOverlayGroup` snapshot.
pub struct LanceDataOverlayGroup {
    pub(crate) inner: DataOverlayGroup,
}

/// A staged overlay for one bound fragment.
///
/// The writer owns one native `OverlayWriter`. `finish` and `abort` consume the
/// native writer; `close` drops it without publishing anything.
pub struct LanceFragmentOverlayWriter {
    inner: Option<OverlayWriter>,
}

const SHARED_COVERAGE: i32 = 0;
const PER_FIELD_COVERAGE: i32 = 1;

fn overlay_coverage_name(kind: i32) -> &'static str {
    match kind {
        SHARED_COVERAGE => "shared",
        PER_FIELD_COVERAGE => "per-field",
        _ => "unknown",
    }
}

fn decode_bitmap(bytes: &[u8], position: usize) -> Result<RoaringBitmap> {
    RoaringBitmap::deserialize_from(bytes).map_err(|error| {
        lance_core::Error::invalid_input(format!(
            "coverage bitmap at index {position} is not a valid portable Roaring bitmap: {error}"
        ))
    })
}

fn encode_bitmap(bitmap: &RoaringBitmap) -> Result<Vec<u8>> {
    let mut bytes = Vec::new();
    bitmap.serialize_into(&mut bytes).map_err(|error| {
        lance_core::Error::internal(format!(
            "failed to serialize portable Roaring coverage bitmap: {error}"
        ))
    })?;
    Ok(bytes)
}

unsafe fn overlay_file_at<'a>(
    file: *const LanceDataOverlayFile,
) -> Result<&'a LanceDataOverlayFile> {
    if file.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "data overlay file must not be NULL".into(),
        ));
    }
    Ok(unsafe { &*file })
}

unsafe fn overlay_group_at<'a>(
    group: *const LanceDataOverlayGroup,
) -> Result<&'a LanceDataOverlayGroup> {
    if group.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "data overlay group must not be NULL".into(),
        ));
    }
    Ok(unsafe { &*group })
}

fn required_output<T>(out: *mut T, label: &str) -> Result<()> {
    if out.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            format!("{label} must not be NULL").into(),
        ));
    }
    Ok(())
}

unsafe fn read_bitmaps(
    bitmaps: *const *const u8,
    bitmap_lens: *const usize,
    bitmap_count: usize,
) -> Result<Vec<RoaringBitmap>> {
    if bitmap_count == 0 {
        return Ok(Vec::new());
    }
    if bitmaps.is_null() || bitmap_lens.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            format!("bitmaps and bitmap_lens must not be NULL when bitmap_count = {bitmap_count}")
                .into(),
        ));
    }
    let max_pointers = isize::MAX as usize / std::mem::size_of::<*const u8>();
    if bitmap_count > max_pointers {
        return Err(lance_core::Error::invalid_input_source(
            format!(
                "bitmap_count {bitmap_count} exceeds the maximum addressable pointer slice length"
            )
            .into(),
        ));
    }
    let max_lens = isize::MAX as usize / std::mem::size_of::<usize>();
    if bitmap_count > max_lens {
        return Err(lance_core::Error::invalid_input_source(
            format!("bitmap_count {bitmap_count} exceeds the maximum addressable length slice")
                .into(),
        ));
    }

    let bitmaps = unsafe { slice::from_raw_parts(bitmaps, bitmap_count) };
    let bitmap_lens = unsafe { slice::from_raw_parts(bitmap_lens, bitmap_count) };
    let mut out = Vec::with_capacity(bitmap_count);
    for (position, (&bitmap, &len)) in bitmaps.iter().zip(bitmap_lens).enumerate() {
        if bitmap.is_null() {
            return Err(lance_core::Error::invalid_input_source(
                format!("bitmap at index {position} must not be NULL").into(),
            ));
        }
        let bytes = unsafe { slice::from_raw_parts(bitmap, len) };
        out.push(decode_bitmap(bytes, position)?);
    }
    Ok(out)
}

/// Create an owned data-overlay value.
///
/// `coverage_kind` is 0 for shared coverage (exactly one bitmap) and 1 for
/// per-field coverage (bitmaps in `data_file.fields` order). Each bitmap is
/// portable 32-bit Roaring serialization. The inputs are borrowed and may be
/// released after this call returns. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_overlay_file_new(
    data_file: *const LanceDataFile,
    coverage_kind: i32,
    bitmaps: *const *const u8,
    bitmap_lens: *const usize,
    bitmap_count: usize,
    committed_version: u64,
) -> *mut LanceDataOverlayFile {
    ffi_try!(
        unsafe {
            data_overlay_file_new_inner(
                data_file,
                coverage_kind,
                bitmaps,
                bitmap_lens,
                bitmap_count,
                committed_version,
            )
        },
        null
    )
}

unsafe fn data_overlay_file_new_inner(
    data_file: *const LanceDataFile,
    coverage_kind: i32,
    bitmaps: *const *const u8,
    bitmap_lens: *const usize,
    bitmap_count: usize,
    committed_version: u64,
) -> Result<*mut LanceDataOverlayFile> {
    if data_file.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "data_file must not be NULL".into(),
        ));
    }
    let data_file = unsafe { &*data_file };
    let bitmaps = unsafe { read_bitmaps(bitmaps, bitmap_lens, bitmap_count)? };

    let coverage = match coverage_kind {
        SHARED_COVERAGE => {
            if bitmaps.len() != 1 {
                return Err(lance_core::Error::invalid_input_source(
                    format!(
                        "shared overlay coverage requires exactly one bitmap, got {bitmap_count}"
                    )
                    .into(),
                ));
            }
            OverlayCoverage::Shared(Arc::new(bitmaps.into_iter().next().expect("one bitmap")))
        }
        PER_FIELD_COVERAGE => OverlayCoverage::PerField(
            bitmaps
                .into_iter()
                .map(Arc::new)
                .collect::<Vec<Arc<RoaringBitmap>>>(),
        ),
        other => {
            return Err(lance_core::Error::invalid_input_source(
                format!(
                    "coverage_kind {other} ({}) is invalid; expected 0 (shared) or 1 (per-field)",
                    overlay_coverage_name(other)
                )
                .into(),
            ));
        }
    };

    let inner = DataOverlayFile {
        data_file: data_file.native().clone(),
        coverage,
        committed_version,
    };
    Ok(Box::into_raw(Box::new(LanceDataOverlayFile { inner })))
}

/// Close a data-overlay handle. NULL-safe.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_overlay_file_close(file: *mut LanceDataOverlayFile) {
    if !file.is_null() {
        swallow_unwind("lance_data_overlay_file_close", || unsafe {
            let _ = Box::from_raw(file);
        });
    }
}

/// Clone a data-overlay handle into an independent snapshot. Returns NULL on
/// error; the caller closes the result exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_overlay_file_clone(
    file: *const LanceDataOverlayFile,
) -> *mut LanceDataOverlayFile {
    ffi_try!(unsafe { data_overlay_file_clone_inner(file) }, null)
}

unsafe fn data_overlay_file_clone_inner(
    file: *const LanceDataOverlayFile,
) -> Result<*mut LanceDataOverlayFile> {
    let file = unsafe { overlay_file_at(file)? };
    Ok(Box::into_raw(Box::new(LanceDataOverlayFile {
        inner: file.inner.clone(),
    })))
}

/// Clone the overlay's data-file metadata. The output is independent and must
/// be closed by the caller. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_overlay_file_data_file(
    file: *const LanceDataOverlayFile,
) -> *mut LanceDataFile {
    ffi_try!(unsafe { data_overlay_file_data_file_inner(file) }, null)
}

unsafe fn data_overlay_file_data_file_inner(
    file: *const LanceDataOverlayFile,
) -> Result<*mut LanceDataFile> {
    let file = unsafe { overlay_file_at(file)? };
    let data_file = LanceDataFile::from_native(file.inner.data_file.clone())?;
    Ok(Box::into_raw(Box::new(data_file)))
}

/// Read the overlay coverage kind: 0 for shared, 1 for per-field. Returns 0 on
/// success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_overlay_file_coverage_kind(
    file: *const LanceDataOverlayFile,
    out_kind: *mut i32,
) -> i32 {
    ffi_try!(
        unsafe { data_overlay_file_coverage_kind_inner(file, out_kind) },
        neg
    )
}

unsafe fn data_overlay_file_coverage_kind_inner(
    file: *const LanceDataOverlayFile,
    out_kind: *mut i32,
) -> Result<i32> {
    required_output(out_kind, "out_kind")?;
    let file = unsafe { overlay_file_at(file)? };
    let kind = match file.inner.coverage {
        OverlayCoverage::Shared(_) => SHARED_COVERAGE,
        OverlayCoverage::PerField(_) => PER_FIELD_COVERAGE,
    };
    unsafe { ptr::write(out_kind, kind) };
    Ok(0)
}

/// Read the number of coverage bitmaps. Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_overlay_file_bitmap_count(
    file: *const LanceDataOverlayFile,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { data_overlay_file_bitmap_count_inner(file, out_count) },
        neg
    )
}

unsafe fn data_overlay_file_bitmap_count_inner(
    file: *const LanceDataOverlayFile,
    out_count: *mut u64,
) -> Result<i32> {
    required_output(out_count, "out_count")?;
    let file = unsafe { overlay_file_at(file)? };
    let count = match &file.inner.coverage {
        OverlayCoverage::Shared(_) => 1,
        OverlayCoverage::PerField(bitmaps) => bitmaps.len(),
    };
    unsafe { ptr::write(out_count, count as u64) };
    Ok(0)
}

/// Serialize one coverage bitmap to a fresh `libc::malloc` buffer. The caller
/// owns the bytes and must release them with `lance_free_bytes`. Outputs are
/// written only on success. Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_overlay_file_bitmap_at(
    file: *const LanceDataOverlayFile,
    index: usize,
    out_bytes: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    ffi_try!(
        unsafe { data_overlay_file_bitmap_at_inner(file, index, out_bytes, out_len) },
        neg
    )
}

unsafe fn data_overlay_file_bitmap_at_inner(
    file: *const LanceDataOverlayFile,
    index: usize,
    out_bytes: *mut *mut u8,
    out_len: *mut usize,
) -> Result<i32> {
    required_output(out_bytes, "out_bytes")?;
    required_output(out_len, "out_len")?;
    let file = unsafe { overlay_file_at(file)? };
    let bitmap = match &file.inner.coverage {
        OverlayCoverage::Shared(bitmap) => {
            if index != 0 {
                return Err(lance_core::Error::invalid_input_source(
                    format!(
                        "bitmap index {index} is out of range for shared coverage with 1 bitmap"
                    )
                    .into(),
                ));
            }
            bitmap
        }
        OverlayCoverage::PerField(bitmaps) => bitmaps.get(index).ok_or_else(|| {
            lance_core::Error::invalid_input_source(
                format!(
                    "bitmap index {index} is out of range for per-field coverage with {} bitmaps",
                    bitmaps.len()
                )
                .into(),
            )
        })?,
    };

    let bytes = encode_bitmap(bitmap)?;
    let len = bytes.len();
    let buffer = unsafe { libc::malloc(len) }.cast::<u8>();
    if buffer.is_null() {
        return Err(lance_core::Error::internal(
            "failed to allocate coverage bitmap buffer".to_string(),
        ));
    }
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), buffer, len);
        ptr::write(out_bytes, buffer);
        ptr::write(out_len, len);
    }
    Ok(0)
}

/// Read the overlay's committed-version stamp. Returns 0 on success, -1 on
/// error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_overlay_file_committed_version(
    file: *const LanceDataOverlayFile,
    out_version: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { data_overlay_file_committed_version_inner(file, out_version) },
        neg
    )
}

unsafe fn data_overlay_file_committed_version_inner(
    file: *const LanceDataOverlayFile,
    out_version: *mut u64,
) -> Result<i32> {
    required_output(out_version, "out_version")?;
    let file = unsafe { overlay_file_at(file)? };
    unsafe { ptr::write(out_version, file.inner.committed_version) };
    Ok(0)
}

/// Create an owned data-overlay group from cloned overlay values.
///
/// The input overlay handles are borrowed and may be closed after this call
/// returns. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_overlay_group_new(
    fragment_id: u64,
    overlays: *const *const LanceDataOverlayFile,
    overlay_count: usize,
) -> *mut LanceDataOverlayGroup {
    ffi_try!(
        unsafe { data_overlay_group_new_inner(fragment_id, overlays, overlay_count) },
        null
    )
}

unsafe fn data_overlay_group_new_inner(
    fragment_id: u64,
    overlays: *const *const LanceDataOverlayFile,
    overlay_count: usize,
) -> Result<*mut LanceDataOverlayGroup> {
    if overlays.is_null() && overlay_count > 0 {
        return Err(lance_core::Error::invalid_input_source(
            format!("overlays must not be NULL when overlay_count = {overlay_count}").into(),
        ));
    }
    let max_overlays = isize::MAX as usize / std::mem::size_of::<*const LanceDataOverlayFile>();
    if overlay_count > max_overlays {
        return Err(lance_core::Error::invalid_input_source(
            format!(
                "overlay_count {overlay_count} exceeds the maximum addressable pointer slice length"
            )
            .into(),
        ));
    }

    let mut cloned = Vec::with_capacity(overlay_count);
    if overlay_count > 0 {
        let overlays = unsafe { slice::from_raw_parts(overlays, overlay_count) };
        for (position, &overlay) in overlays.iter().enumerate() {
            if overlay.is_null() {
                return Err(lance_core::Error::invalid_input_source(
                    format!("overlay at index {position} must not be NULL").into(),
                ));
            }
            cloned.push(unsafe { (*overlay).inner.clone() });
        }
    }

    Ok(Box::into_raw(Box::new(LanceDataOverlayGroup {
        inner: DataOverlayGroup {
            fragment_id,
            overlays: cloned,
        },
    })))
}

/// Close a data-overlay group handle. NULL-safe.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_overlay_group_close(group: *mut LanceDataOverlayGroup) {
    if !group.is_null() {
        swallow_unwind("lance_data_overlay_group_close", || unsafe {
            let _ = Box::from_raw(group);
        });
    }
}

/// Clone a data-overlay group into an independent snapshot. Returns NULL on
/// error; the caller closes the result exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_overlay_group_clone(
    group: *const LanceDataOverlayGroup,
) -> *mut LanceDataOverlayGroup {
    ffi_try!(unsafe { data_overlay_group_clone_inner(group) }, null)
}

unsafe fn data_overlay_group_clone_inner(
    group: *const LanceDataOverlayGroup,
) -> Result<*mut LanceDataOverlayGroup> {
    let group = unsafe { overlay_group_at(group)? };
    Ok(Box::into_raw(Box::new(LanceDataOverlayGroup {
        inner: group.inner.clone(),
    })))
}

/// Read the group's target fragment id. Returns 0 on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_overlay_group_fragment_id(
    group: *const LanceDataOverlayGroup,
    out_fragment_id: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { data_overlay_group_fragment_id_inner(group, out_fragment_id) },
        neg
    )
}

unsafe fn data_overlay_group_fragment_id_inner(
    group: *const LanceDataOverlayGroup,
    out_fragment_id: *mut u64,
) -> Result<i32> {
    required_output(out_fragment_id, "out_fragment_id")?;
    let group = unsafe { overlay_group_at(group)? };
    unsafe { ptr::write(out_fragment_id, group.inner.fragment_id) };
    Ok(0)
}

/// Read the number of overlays in the group. Returns 0 on success, -1 on
/// error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_overlay_group_overlay_count(
    group: *const LanceDataOverlayGroup,
    out_count: *mut u64,
) -> i32 {
    ffi_try!(
        unsafe { data_overlay_group_overlay_count_inner(group, out_count) },
        neg
    )
}

unsafe fn data_overlay_group_overlay_count_inner(
    group: *const LanceDataOverlayGroup,
    out_count: *mut u64,
) -> Result<i32> {
    required_output(out_count, "out_count")?;
    let group = unsafe { overlay_group_at(group)? };
    unsafe { ptr::write(out_count, group.inner.overlays.len() as u64) };
    Ok(0)
}

/// Clone one overlay from the group. The output is independent and must be
/// closed by the caller. Returns NULL on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_data_overlay_group_overlay_at(
    group: *const LanceDataOverlayGroup,
    index: usize,
    out_overlay: *mut *mut LanceDataOverlayFile,
) -> i32 {
    ffi_try!(
        unsafe { data_overlay_group_overlay_at_inner(group, index, out_overlay) },
        neg
    )
}

unsafe fn data_overlay_group_overlay_at_inner(
    group: *const LanceDataOverlayGroup,
    index: usize,
    out_overlay: *mut *mut LanceDataOverlayFile,
) -> Result<i32> {
    required_output(out_overlay, "out_overlay")?;
    let group = unsafe { overlay_group_at(group)? };
    let overlay = group
        .inner
        .overlays
        .get(index)
        .ok_or_else(|| {
            lance_core::Error::invalid_input_source(
                format!(
                    "overlay index {index} is out of range for {} overlays",
                    group.inner.overlays.len()
                )
                .into(),
            )
        })?
        .clone();
    let overlay = Box::new(LanceDataOverlayFile { inner: overlay });
    unsafe { ptr::write(out_overlay, Box::into_raw(overlay)) };
    Ok(0)
}

/// Open an overlay writer for a bound fragment.
///
/// The fragment's current native snapshot and the supplied schema are captured;
/// the caller must commit with the dataset read version used to open this
/// writer. `schema` may be a subset of the dataset schema. Returns 0 on
/// success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_overlay_writer_new(
    fragment: *const LanceFragment,
    schema: *const LanceSchema,
    out_writer: *mut *mut LanceFragmentOverlayWriter,
) -> i32 {
    ffi_try!(
        unsafe { fragment_overlay_writer_new_inner(fragment, schema, out_writer) },
        neg
    )
}

unsafe fn fragment_overlay_writer_new_inner(
    fragment: *const LanceFragment,
    schema: *const LanceSchema,
    out_writer: *mut *mut LanceFragmentOverlayWriter,
) -> Result<i32> {
    required_output(out_writer, "out_writer")?;
    if schema.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "schema must not be NULL".into(),
        ));
    }
    let fragment = unsafe { fragment_ref(fragment)? };
    let schema = unsafe { &*schema };
    let writer = block_on(fragment.bound()?.write_overlay(&schema.inner))
        .map_err(lance_core::Error::from)?;
    let writer = Box::new(LanceFragmentOverlayWriter {
        inner: Some(writer),
    });
    unsafe { ptr::write(out_writer, Box::into_raw(writer)) };
    Ok(0)
}

/// Write one batch of overlay values.
///
/// The batch is borrowed for the duration of the call and may be closed
/// afterwards. Rows are keyed by `_rowaddr`; the writer's native checks own
/// row ordering, type, nullability, fragment, and range validation. Returns 0
/// on success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_overlay_writer_write_batch(
    writer: *mut LanceFragmentOverlayWriter,
    batch: *const LanceBatch,
) -> i32 {
    ffi_try!(
        unsafe { fragment_overlay_writer_write_batch_inner(writer, batch) },
        neg
    )
}

unsafe fn fragment_overlay_writer_write_batch_inner(
    writer: *mut LanceFragmentOverlayWriter,
    batch: *const LanceBatch,
) -> Result<i32> {
    if writer.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "writer must not be NULL".into(),
        ));
    }
    if batch.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "batch must not be NULL".into(),
        ));
    }
    let writer = unsafe { &mut *writer };
    let native = writer.inner.as_mut().ok_or_else(|| {
        lance_core::Error::internal("overlay writer was already consumed".to_string())
    })?;
    let batch = unsafe { &*batch };
    block_on(native.write_batch(&batch.inner))?;
    Ok(0)
}

/// Finish the writer, consuming its C allocation.
///
/// On success `out_group` receives an owned overlay group, or NULL when the
/// staged coverage was empty. The output slot is written only on success. The
/// C handle is consumed on every return, including errors. Returns 0 on
/// success, -1 on error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_overlay_writer_finish(
    writer: *mut LanceFragmentOverlayWriter,
    out_group: *mut *mut LanceDataOverlayGroup,
) -> i32 {
    ffi_try!(
        unsafe { fragment_overlay_writer_finish_inner(writer, out_group) },
        neg
    )
}

unsafe fn fragment_overlay_writer_finish_inner(
    writer: *mut LanceFragmentOverlayWriter,
    out_group: *mut *mut LanceDataOverlayGroup,
) -> Result<i32> {
    if out_group.is_null() {
        if !writer.is_null() {
            drop(unsafe { Box::from_raw(writer) });
        }
        return Err(lance_core::Error::invalid_input_source(
            "out_group must not be NULL".into(),
        ));
    }
    if writer.is_null() {
        return Err(lance_core::Error::invalid_input_source(
            "writer must not be NULL".into(),
        ));
    }

    let mut writer = unsafe { Box::from_raw(writer) };
    let native = writer.inner.take().ok_or_else(|| {
        lance_core::Error::internal("overlay writer was already consumed".to_string())
    })?;
    let group = block_on(native.finish()).map_err(lance_core::Error::from)?;
    match group {
        Some(group) => unsafe {
            ptr::write(
                out_group,
                Box::into_raw(Box::new(LanceDataOverlayGroup { inner: group })),
            );
        },
        None => unsafe { ptr::write(out_group, ptr::null_mut()) },
    }
    Ok(0)
}

/// Abort the writer, consuming its C allocation and discarding staged data
/// best-effort. NULL-safe. No error is reported.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_overlay_writer_abort(
    writer: *mut LanceFragmentOverlayWriter,
) {
    if writer.is_null() {
        return;
    }
    swallow_unwind("lance_fragment_overlay_writer_abort", || unsafe {
        let mut writer = Box::from_raw(writer);
        if let Some(native) = writer.inner.take() {
            block_on(native.abort());
        }
    });
}

/// Close the writer, consuming its C allocation.
///
/// This drops the native writer without finishing or publishing it. NULL-safe.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fragment_overlay_writer_close(
    writer: *mut LanceFragmentOverlayWriter,
) {
    if !writer.is_null() {
        swallow_unwind("lance_fragment_overlay_writer_close", || unsafe {
            let _ = Box::from_raw(writer);
        });
    }
}
