// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

use std::ffi::{CString, c_char};
use std::ptr;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use arrow::ffi::FFI_ArrowSchema;
use arrow::ffi_stream::FFI_ArrowArrayStream;
use arrow_array::UInt32Array;
use arrow_schema::Schema as ArrowSchema;
use lance_core::cache::LanceCache;
use lance_core::{Error, Result};
use lance_encoding::decoder::{DecoderConfig, DecoderPlugins, FilterExpression};
use lance_file::format::pbfile;
use lance_file::reader::{
    CachedFileMetadata, ColumnStatistics, FileReader, FileReaderOptions, FileStatistics,
    describe_encoding,
};
use lance_file::version::ConcreteFileVersion;
use lance_file::versions;
use lance_io::ReadBatchParams;
use lance_io::scheduler::IoStats;
use lance_io::stream::RecordBatchStream;
use lance_io::utils::CachedFileSize;
use object_store::path::Path;

use crate::error::{ffi_try, swallow_unwind};
use crate::file_session::{
    FileSessionInner, LanceByteBuffer, LanceScanStats, byte_buffer_from_bytes,
    open_file_session_inner, scan_stats, shared_session_from_ffi, storage_options_from_ffi,
};
use crate::helpers::parse_c_string;
use crate::runtime::{RT, block_on};
use crate::storage_options::LanceStorageOptionsAccessor;
use crate::stream_guard::GuardedReader;
use crate::{file_session::LanceSchedulerConfig, session::LanceSession};

/// Reader cache selection.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceFileCacheMode {
    None = 0,
    Session = 1,
}

impl LanceFileCacheMode {
    fn from_raw(raw: i32) -> Result<Self> {
        match raw {
            0 => Ok(Self::None),
            1 => Ok(Self::Session),
            other => Err(Error::invalid_input(format!(
                "invalid file cache mode {other}; expected 0 (none) or 1 (session)"
            ))),
        }
    }
}

/// Reader options. A NULL options pointer uses native defaults and no
/// projection.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LanceFileReaderOptions {
    pub has_columns: i32,
    pub columns: *const *const c_char,
    pub column_count: usize,
    pub cache_mode: i32,
    pub has_read_chunk_size: i32,
    pub read_chunk_size: u64,
    pub has_batch_size_bytes: i32,
    pub batch_size_bytes: u64,
    pub has_cache_repetition_index: i32,
    pub cache_repetition_index: i32,
    pub has_validate_on_decode: i32,
    pub validate_on_decode: i32,
    pub has_inline_scheduling: i32,
    pub inline_scheduling: i32,
}

/// Native concrete file-format identity.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceFileFormatVersion {
    V1 = 0,
    V2_0 = 1,
    V2_1 = 2,
    V2_2 = 3,
    V2_3 = 4,
}

/// Snapshot of native `CachedFileMetadata` summary fields.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LanceFileMetadataInfo {
    pub format_version: i32,
    pub major_version: u16,
    pub minor_version: u16,
    pub num_rows: u64,
    pub num_data_bytes: u64,
    pub num_column_metadata_bytes: u64,
    pub num_global_buffer_bytes: u64,
    pub num_footer_bytes: u64,
    pub file_size_bytes: u64,
    pub num_columns: usize,
    pub num_physical_columns: usize,
    pub num_global_buffers: usize,
}

/// Native buffer position and size.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LanceBufferDescriptor {
    pub position: u64,
    pub size: u64,
}

/// Native protobuf encoding location.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LanceFileEncodingKind {
    Absent = 0,
    None = 1,
    Direct = 2,
    Indirect = 3,
}

/// Borrowed encoding description.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceEncodingDescriptor {
    pub kind: i32,
    pub buffer_location: u64,
    pub buffer_length: u64,
    pub direct_bytes: *const u8,
    pub direct_bytes_len: usize,
}

/// Borrowed page metadata.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LancePageMetadata {
    pub buffers: *const LanceBufferDescriptor,
    pub buffer_count: usize,
    pub num_rows: u64,
    pub priority: u64,
    pub encoding: LanceEncodingDescriptor,
    pub encoding_description: *const c_char,
}

/// Borrowed column metadata.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceColumnMetadata {
    pub column_buffers: *const LanceBufferDescriptor,
    pub column_buffer_count: usize,
    pub pages: *const LancePageMetadata,
    pub page_count: usize,
    pub encoding: LanceEncodingDescriptor,
}

/// Per-column native statistics.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LanceColumnStatistics {
    pub num_pages: usize,
    pub size_bytes: u64,
}

pub struct LanceFileReader {
    inner: Arc<FileReader>,
    poison: Arc<AtomicBool>,
    io_stats: IoStats,
}

pub struct LanceFileMetadata {
    inner: Arc<CachedFileMetadata>,
    info: LanceFileMetadataInfo,
    global_buffers: Vec<LanceBufferDescriptor>,
    columns: Vec<ColumnBacking>,
    column_headers: Vec<LanceColumnMetadata>,
}

pub struct LanceFileStatistics {
    columns: Vec<LanceColumnStatistics>,
}

struct EncodingBacking {
    direct_bytes: Vec<u8>,
    header: LanceEncodingDescriptor,
}

impl EncodingBacking {
    fn new(encoding: Option<&pbfile::Encoding>) -> Self {
        let mut direct_bytes = Vec::new();
        let mut header = LanceEncodingDescriptor {
            kind: LanceFileEncodingKind::Absent as i32,
            buffer_location: 0,
            buffer_length: 0,
            direct_bytes: ptr::null(),
            direct_bytes_len: 0,
        };

        match encoding.and_then(|encoding| encoding.location.as_ref()) {
            None => {}
            Some(pbfile::encoding::Location::None(_)) => {
                header.kind = LanceFileEncodingKind::None as i32;
            }
            Some(pbfile::encoding::Location::Direct(direct)) => {
                header.kind = LanceFileEncodingKind::Direct as i32;
                direct_bytes = direct.encoding.clone();
            }
            Some(pbfile::encoding::Location::Indirect(indirect)) => {
                header.kind = LanceFileEncodingKind::Indirect as i32;
                header.buffer_location = indirect.buffer_location;
                header.buffer_length = indirect.buffer_length;
            }
        }

        if !direct_bytes.is_empty() {
            header.direct_bytes = direct_bytes.as_ptr();
            header.direct_bytes_len = direct_bytes.len();
        }
        Self {
            direct_bytes,
            header,
        }
    }
}

struct PageBacking {
    buffers: Vec<LanceBufferDescriptor>,
    description: CString,
    encoding: EncodingBacking,
    header: LancePageMetadata,
}

impl PageBacking {
    fn new(page: &pbfile::column_metadata::Page) -> Result<Self> {
        let buffers = buffer_descriptors(&page.buffer_offsets, &page.buffer_sizes, "page")?;
        let description = CString::new(describe_encoding(page)).map_err(|error| {
            Error::internal(format!(
                "page encoding description contains a NUL byte: {error}"
            ))
        })?;
        let encoding = EncodingBacking::new(page.encoding.as_ref());
        let header = LancePageMetadata {
            buffers: buffers.as_ptr(),
            buffer_count: buffers.len(),
            num_rows: page.length,
            priority: page.priority,
            encoding: encoding.header,
            encoding_description: description.as_ptr(),
        };
        Ok(Self {
            buffers,
            description,
            encoding,
            header,
        })
    }
}

struct ColumnBacking {
    buffers: Vec<LanceBufferDescriptor>,
    pages: Vec<PageBacking>,
    page_headers: Vec<LancePageMetadata>,
    encoding: EncodingBacking,
    header: LanceColumnMetadata,
}

impl ColumnBacking {
    fn new(column: &pbfile::ColumnMetadata) -> Result<Self> {
        let buffers = buffer_descriptors(&column.buffer_offsets, &column.buffer_sizes, "column")?;
        let pages = column
            .pages
            .iter()
            .map(PageBacking::new)
            .collect::<Result<Vec<_>>>()?;
        let page_headers = pages.iter().map(|page| page.header).collect::<Vec<_>>();
        let encoding = EncodingBacking::new(column.encoding.as_ref());
        let header = LanceColumnMetadata {
            column_buffers: buffers.as_ptr(),
            column_buffer_count: buffers.len(),
            pages: page_headers.as_ptr(),
            page_count: page_headers.len(),
            encoding: encoding.header,
        };
        Ok(Self {
            buffers,
            pages,
            page_headers,
            encoding,
            header,
        })
    }
}

fn buffer_descriptors(
    positions: &[u64],
    sizes: &[u64],
    context: &str,
) -> Result<Vec<LanceBufferDescriptor>> {
    if positions.len() != sizes.len() {
        return Err(Error::internal(format!(
            "{context} metadata has {} buffer offsets but {} buffer sizes",
            positions.len(),
            sizes.len()
        )));
    }
    Ok(positions
        .iter()
        .copied()
        .zip(sizes.iter().copied())
        .map(|(position, size)| LanceBufferDescriptor { position, size })
        .collect())
}

struct ParsedReaderOptions {
    columns: Option<Vec<String>>,
    cache_mode: LanceFileCacheMode,
    native: FileReaderOptions,
}

fn parse_bool_option(has_value: i32, value: i32, field: &str) -> Result<Option<bool>> {
    if has_value == 0 {
        return Ok(None);
    }
    match value {
        0 => Ok(Some(false)),
        1 => Ok(Some(true)),
        other => Err(Error::invalid_input(format!(
            "{field} must be 0 or 1 when set, got {other}"
        ))),
    }
}

unsafe fn parse_reader_options(
    options: *const LanceFileReaderOptions,
) -> Result<ParsedReaderOptions> {
    let mut native = FileReaderOptions::default();
    if options.is_null() {
        return Ok(ParsedReaderOptions {
            columns: None,
            cache_mode: LanceFileCacheMode::None,
            native,
        });
    }

    let options = unsafe { &*options };
    let columns = if options.has_columns == 0 {
        if !options.columns.is_null() || options.column_count != 0 {
            return Err(Error::invalid_input(
                "columns and column_count must be unset when has_columns is 0",
            ));
        }
        None
    } else {
        let mut columns = Vec::with_capacity(options.column_count);
        if options.column_count != 0 && options.columns.is_null() {
            return Err(Error::invalid_input(format!(
                "columns must not be NULL when has_columns is set and column_count={}",
                options.column_count
            )));
        }
        for index in 0..options.column_count {
            let column = unsafe { *options.columns.add(index) };
            let column = unsafe { parse_c_string(column)? }.ok_or_else(|| {
                Error::invalid_input(format!("columns[{index}] must not be NULL"))
            })?;
            columns.push(column.to_owned());
        }
        Some(columns)
    };

    let cache_mode = LanceFileCacheMode::from_raw(options.cache_mode)?;
    if options.has_read_chunk_size != 0 {
        native.read_chunk_size = options.read_chunk_size;
    }
    if options.has_batch_size_bytes != 0 {
        native.batch_size_bytes = Some(options.batch_size_bytes);
    }
    if let Some(value) = parse_bool_option(
        options.has_cache_repetition_index,
        options.cache_repetition_index,
        "cache_repetition_index",
    )? {
        native.decoder_config.cache_repetition_index = value;
    }
    if let Some(value) = parse_bool_option(
        options.has_validate_on_decode,
        options.validate_on_decode,
        "validate_on_decode",
    )? {
        native.decoder_config.validate_on_decode = value;
    }
    native.decoder_config.inline_scheduling = parse_bool_option(
        options.has_inline_scheduling,
        options.inline_scheduling,
        "inline_scheduling",
    )?;

    Ok(ParsedReaderOptions {
        columns,
        cache_mode,
        native,
    })
}

pub(crate) unsafe fn open_reader_at(
    session: &FileSessionInner,
    path: Path,
    options: *const LanceFileReaderOptions,
) -> Result<*mut LanceFileReader> {
    let options = unsafe { parse_reader_options(options)? };
    let cache = match options.cache_mode {
        LanceFileCacheMode::None => LanceCache::no_cache(),
        LanceFileCacheMode::Session => session
            .session
            .as_ref()
            .ok_or_else(|| {
                Error::invalid_input(
                    "file cache mode SESSION requires a LanceSession bound to the file session",
                )
            })?
            .file_metadata_cache()
            .clone(),
    };

    let (file_scheduler, metadata) = block_on(async {
        let file_scheduler = session
            .scheduler
            .open_file(&path, &CachedFileSize::unknown())
            .await?;
        let metadata = FileReader::read_all_metadata(&file_scheduler).await?;
        Ok::<_, Error>((file_scheduler, metadata))
    })?;

    let projection = if let Some(columns) = &options.columns {
        let column_refs = columns.iter().map(String::as_str).collect::<Vec<_>>();
        Some(versions::reader_projection_from_column_names(
            metadata.version(),
            &metadata.file_schema,
            &column_refs,
        )?)
    } else {
        None
    };

    let io = Arc::new(lance_file::LanceEncodingsIo::new(file_scheduler));
    let native = block_on(FileReader::try_open_with_file_metadata(
        io,
        path,
        projection,
        Arc::<DecoderPlugins>::default(),
        Arc::new(metadata),
        &cache,
        options.native,
    ))?;
    let io_stats = IoStats::new();
    let native = native.with_io_stats(io_stats.recorder());
    Ok(Box::into_raw(Box::new(LanceFileReader {
        inner: Arc::new(native),
        poison: Arc::new(AtomicBool::new(false)),
        io_stats,
    })))
}

/// Open a raw Lance file reader directly.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_reader_open(
    uri_or_path: *const c_char,
    storage_options: *const LanceStorageOptionsAccessor,
    options: *const LanceFileReaderOptions,
    session: *const LanceSession,
    scheduler_config: *const LanceSchedulerConfig,
) -> *mut LanceFileReader {
    ffi_try!(
        unsafe {
            file_reader_open_inner(
                uri_or_path,
                storage_options,
                options,
                session,
                scheduler_config,
            )
        },
        null
    )
}

unsafe fn file_reader_open_inner(
    uri_or_path: *const c_char,
    storage_options: *const LanceStorageOptionsAccessor,
    options: *const LanceFileReaderOptions,
    session: *const LanceSession,
    scheduler_config: *const LanceSchedulerConfig,
) -> Result<*mut LanceFileReader> {
    let uri_or_path = unsafe { parse_c_string(uri_or_path)? }
        .ok_or_else(|| Error::invalid_input_source("uri_or_path must not be NULL".into()))?;
    let session = open_file_session_inner(
        uri_or_path,
        storage_options_from_ffi(storage_options),
        shared_session_from_ffi(session),
        scheduler_config,
    )?;
    let path = session.base_path.clone();
    unsafe { open_reader_at(&session, path, options) }
}

/// Return the native row count.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_reader_num_rows(
    reader: *const LanceFileReader,
    out_rows: *mut u64,
) -> i32 {
    ffi_try!(unsafe { file_reader_num_rows_inner(reader, out_rows) }, neg)
}

unsafe fn file_reader_num_rows_inner(
    reader: *const LanceFileReader,
    out_rows: *mut u64,
) -> Result<i32> {
    let reader = unsafe { checked_reader(reader)? };
    if out_rows.is_null() {
        return Err(Error::invalid_input("out_rows must not be NULL"));
    }
    unsafe { ptr::write(out_rows, reader.inner.num_rows()) };
    Ok(0)
}

/// Create an owned metadata snapshot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_reader_get_metadata(
    reader: *const LanceFileReader,
    out_metadata: *mut *mut LanceFileMetadata,
) -> i32 {
    ffi_try!(
        unsafe { file_reader_get_metadata_inner(reader, out_metadata) },
        neg
    )
}

unsafe fn file_reader_get_metadata_inner(
    reader: *const LanceFileReader,
    out_metadata: *mut *mut LanceFileMetadata,
) -> Result<i32> {
    let reader = unsafe { checked_reader(reader)? };
    if out_metadata.is_null() {
        return Err(Error::invalid_input("out_metadata must not be NULL"));
    }
    let metadata = LanceFileMetadata::new(reader.inner.metadata().clone())?;
    unsafe { ptr::write(out_metadata, Box::into_raw(Box::new(metadata))) };
    Ok(0)
}

/// Create an owned file-statistics snapshot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_reader_get_statistics(
    reader: *const LanceFileReader,
    out_statistics: *mut *mut LanceFileStatistics,
) -> i32 {
    ffi_try!(
        unsafe { file_reader_get_statistics_inner(reader, out_statistics) },
        neg
    )
}

unsafe fn file_reader_get_statistics_inner(
    reader: *const LanceFileReader,
    out_statistics: *mut *mut LanceFileStatistics,
) -> Result<i32> {
    let reader = unsafe { checked_reader(reader)? };
    if out_statistics.is_null() {
        return Err(Error::invalid_input("out_statistics must not be NULL"));
    }
    let statistics = LanceFileStatistics::new(reader.inner.file_statistics());
    unsafe { ptr::write(out_statistics, Box::into_raw(Box::new(statistics))) };
    Ok(0)
}

/// Read a native global buffer.
///
/// Index 0 is the serialized schema. User-added writer buffers start at 1.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_reader_read_global_buffer(
    reader: *const LanceFileReader,
    index: u32,
    out_bytes: *mut *mut LanceByteBuffer,
) -> i32 {
    ffi_try!(
        unsafe { file_reader_read_global_buffer_inner(reader, index, out_bytes) },
        neg
    )
}

unsafe fn file_reader_read_global_buffer_inner(
    reader: *const LanceFileReader,
    index: u32,
    out_bytes: *mut *mut LanceByteBuffer,
) -> Result<i32> {
    let reader = unsafe { checked_reader(reader)? };
    if out_bytes.is_null() {
        return Err(Error::invalid_input("out_bytes must not be NULL"));
    }
    let bytes = block_on(reader.inner.read_global_buffer(index))?;
    unsafe { ptr::write(out_bytes, byte_buffer_from_bytes(bytes.to_vec())) };
    Ok(0)
}

/// Return this reader's cumulative native I/O counters.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_reader_get_io_stats(
    reader: *const LanceFileReader,
    out_stats: *mut LanceScanStats,
) -> i32 {
    ffi_try!(
        unsafe { file_reader_get_io_stats_inner(reader, out_stats) },
        neg
    )
}

unsafe fn file_reader_get_io_stats_inner(
    reader: *const LanceFileReader,
    out_stats: *mut LanceScanStats,
) -> Result<i32> {
    let reader = unsafe { checked_reader(reader)? };
    if out_stats.is_null() {
        return Err(Error::invalid_input("out_stats must not be NULL"));
    }
    unsafe { ptr::write(out_stats, scan_stats(reader.io_stats.snapshot())) };
    Ok(0)
}

/// Stream all rows as Arrow C Data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_reader_read_all(
    reader: *const LanceFileReader,
    batch_size: u32,
    batch_readahead: u32,
    out: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe {
            export_reader_stream(
                reader,
                ReadBatchParams::RangeFull,
                batch_size,
                batch_readahead,
                out,
            )
        },
        neg
    )
}

/// Stream a checked native row range.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_reader_read_range(
    reader: *const LanceFileReader,
    offset: u64,
    num_rows: u64,
    batch_size: u32,
    batch_readahead: u32,
    out: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe {
            file_reader_read_range_inner(reader, offset, num_rows, batch_size, batch_readahead, out)
        },
        neg
    )
}

unsafe fn file_reader_read_range_inner(
    reader: *const LanceFileReader,
    offset: u64,
    num_rows: u64,
    batch_size: u32,
    batch_readahead: u32,
    out: *mut FFI_ArrowArrayStream,
) -> Result<i32> {
    let end = offset.checked_add(num_rows).ok_or_else(|| {
        Error::invalid_input(format!(
            "row range offset={offset} and num_rows={num_rows} overflow u64"
        ))
    })?;
    let start = usize::try_from(offset)
        .map_err(|_| Error::invalid_input(format!("row offset {offset} does not fit in usize")))?;
    let end = usize::try_from(end)
        .map_err(|_| Error::invalid_input(format!("row range end {end} does not fit in usize")))?;
    unsafe {
        export_reader_stream(
            reader,
            ReadBatchParams::Range(start..end),
            batch_size,
            batch_readahead,
            out,
        )
    }
}

/// Stream native u32 row indices in the supplied order.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_reader_read_indices(
    reader: *const LanceFileReader,
    indices: *const u32,
    count: usize,
    batch_size: u32,
    batch_readahead: u32,
    out: *mut FFI_ArrowArrayStream,
) -> i32 {
    ffi_try!(
        unsafe {
            file_reader_read_indices_inner(reader, indices, count, batch_size, batch_readahead, out)
        },
        neg
    )
}

unsafe fn file_reader_read_indices_inner(
    reader: *const LanceFileReader,
    indices: *const u32,
    count: usize,
    batch_size: u32,
    batch_readahead: u32,
    out: *mut FFI_ArrowArrayStream,
) -> Result<i32> {
    if count != 0 && indices.is_null() {
        return Err(Error::invalid_input(format!(
            "indices must not be NULL when count={count}"
        )));
    }
    let values = if count == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(indices, count) }.to_vec()
    };
    let params = ReadBatchParams::Indices(UInt32Array::from_iter_values(values));
    unsafe { export_reader_stream(reader, params, batch_size, batch_readahead, out) }
}

unsafe fn export_reader_stream(
    reader: *const LanceFileReader,
    params: ReadBatchParams,
    batch_size: u32,
    batch_readahead: u32,
    out: *mut FFI_ArrowArrayStream,
) -> Result<i32> {
    let reader = unsafe { checked_reader(reader)? };
    if out.is_null() {
        return Err(Error::invalid_input("out must not be NULL"));
    }
    let stream = block_on(reader.inner.read_stream(
        params,
        batch_size,
        batch_readahead,
        FilterExpression::no_filter(),
    ))?;
    let schema = stream.schema();
    let guarded = GuardedReader::new(stream, schema, RT.handle().clone(), reader.poison.clone());
    let ffi_stream = FFI_ArrowArrayStream::new(Box::new(guarded));
    unsafe { ptr::write_unaligned(out, ffi_stream) };
    Ok(0)
}

fn checked_reader(reader: *const LanceFileReader) -> Result<&'static LanceFileReader> {
    if reader.is_null() {
        return Err(Error::invalid_input("reader must not be NULL"));
    }
    let reader = unsafe { &*reader };
    if reader.poison.load(Ordering::SeqCst) {
        return Err(Error::internal(
            "file reader was poisoned by an earlier stream panic",
        ));
    }
    Ok(reader)
}

impl LanceFileMetadata {
    fn new(inner: Arc<CachedFileMetadata>) -> Result<Self> {
        let info = LanceFileMetadataInfo {
            format_version: format_version(inner.version) as i32,
            major_version: inner.major_version,
            minor_version: inner.minor_version,
            num_rows: inner.num_rows,
            num_data_bytes: inner.num_data_bytes,
            num_column_metadata_bytes: inner.num_column_metadata_bytes,
            num_global_buffer_bytes: inner.num_global_buffer_bytes,
            num_footer_bytes: inner.num_footer_bytes,
            file_size_bytes: inner.file_size_bytes,
            num_columns: inner.file_schema.fields.len(),
            num_physical_columns: inner.column_metadatas.len(),
            num_global_buffers: inner.file_buffers.len(),
        };
        let global_buffers = inner
            .file_buffers
            .iter()
            .map(|buffer| LanceBufferDescriptor {
                position: buffer.position,
                size: buffer.size,
            })
            .collect();
        let columns = inner
            .column_metadatas
            .iter()
            .map(ColumnBacking::new)
            .collect::<Result<Vec<_>>>()?;
        let column_headers = columns.iter().map(|column| column.header).collect();
        Ok(Self {
            inner,
            info,
            global_buffers,
            columns,
            column_headers,
        })
    }
}

fn format_version(version: ConcreteFileVersion) -> LanceFileFormatVersion {
    match version {
        ConcreteFileVersion::V1 => LanceFileFormatVersion::V1,
        ConcreteFileVersion::V2_0 => LanceFileFormatVersion::V2_0,
        ConcreteFileVersion::V2_1 => LanceFileFormatVersion::V2_1,
        ConcreteFileVersion::V2_2 => LanceFileFormatVersion::V2_2,
        ConcreteFileVersion::V2_3 => LanceFileFormatVersion::V2_3,
    }
}

impl LanceFileStatistics {
    fn new(statistics: FileStatistics) -> Self {
        Self {
            columns: statistics
                .columns
                .into_iter()
                .map(column_statistics)
                .collect(),
        }
    }
}

fn column_statistics(statistics: ColumnStatistics) -> LanceColumnStatistics {
    LanceColumnStatistics {
        num_pages: statistics.num_pages,
        size_bytes: statistics.size_bytes,
    }
}

/// Copy metadata summary fields.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_metadata_get_info(
    metadata: *const LanceFileMetadata,
    out_info: *mut LanceFileMetadataInfo,
) -> i32 {
    ffi_try!(
        unsafe { file_metadata_get_info_inner(metadata, out_info) },
        neg
    )
}

unsafe fn file_metadata_get_info_inner(
    metadata: *const LanceFileMetadata,
    out_info: *mut LanceFileMetadataInfo,
) -> Result<i32> {
    if metadata.is_null() || out_info.is_null() {
        return Err(Error::invalid_input(
            "metadata and out_info must not be NULL",
        ));
    }
    unsafe { ptr::write(out_info, (&*metadata).info) };
    Ok(0)
}

/// Export the complete native file schema as Arrow C Data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_metadata_get_schema(
    metadata: *const LanceFileMetadata,
    out_schema: *mut FFI_ArrowSchema,
) -> i32 {
    ffi_try!(
        unsafe { file_metadata_get_schema_inner(metadata, out_schema) },
        neg
    )
}

unsafe fn file_metadata_get_schema_inner(
    metadata: *const LanceFileMetadata,
    out_schema: *mut FFI_ArrowSchema,
) -> Result<i32> {
    if metadata.is_null() || out_schema.is_null() {
        return Err(Error::invalid_input(
            "metadata and out_schema must not be NULL",
        ));
    }
    let metadata = unsafe { &*metadata };
    let arrow_schema: ArrowSchema = metadata.inner.file_schema.as_ref().into();
    let schema = FFI_ArrowSchema::try_from(&arrow_schema)?;
    unsafe { ptr::write_unaligned(out_schema, schema) };
    Ok(0)
}

/// Borrow the global-buffer descriptors from the snapshot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_metadata_get_global_buffers(
    metadata: *const LanceFileMetadata,
    out_buffers: *mut *const LanceBufferDescriptor,
    out_count: *mut usize,
) -> i32 {
    ffi_try!(
        unsafe { file_metadata_get_global_buffers_inner(metadata, out_buffers, out_count) },
        neg
    )
}

unsafe fn file_metadata_get_global_buffers_inner(
    metadata: *const LanceFileMetadata,
    out_buffers: *mut *const LanceBufferDescriptor,
    out_count: *mut usize,
) -> Result<i32> {
    if metadata.is_null() || out_buffers.is_null() || out_count.is_null() {
        return Err(Error::invalid_input(
            "metadata, out_buffers, and out_count must not be NULL",
        ));
    }
    let buffers = &unsafe { &*metadata }.global_buffers;
    unsafe {
        ptr::write(out_buffers, buffers.as_ptr());
        ptr::write(out_count, buffers.len());
    }
    Ok(0)
}

/// Borrow the column/page descriptors from the snapshot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_metadata_get_columns(
    metadata: *const LanceFileMetadata,
    out_columns: *mut *const LanceColumnMetadata,
    out_count: *mut usize,
) -> i32 {
    ffi_try!(
        unsafe { file_metadata_get_columns_inner(metadata, out_columns, out_count) },
        neg
    )
}

unsafe fn file_metadata_get_columns_inner(
    metadata: *const LanceFileMetadata,
    out_columns: *mut *const LanceColumnMetadata,
    out_count: *mut usize,
) -> Result<i32> {
    if metadata.is_null() || out_columns.is_null() || out_count.is_null() {
        return Err(Error::invalid_input(
            "metadata, out_columns, and out_count must not be NULL",
        ));
    }
    let metadata = unsafe { &*metadata };
    unsafe {
        ptr::write(out_columns, metadata.column_headers.as_ptr());
        ptr::write(out_count, metadata.column_headers.len());
    }
    Ok(0)
}

/// Close a file metadata snapshot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_metadata_close(metadata: *mut LanceFileMetadata) {
    if !metadata.is_null() {
        swallow_unwind("lance_file_metadata_close", || unsafe {
            let _ = Box::from_raw(metadata);
        });
    }
}

/// Borrow per-column statistics.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_statistics_get_columns(
    statistics: *const LanceFileStatistics,
    out_columns: *mut *const LanceColumnStatistics,
    out_count: *mut usize,
) -> i32 {
    ffi_try!(
        unsafe { file_statistics_get_columns_inner(statistics, out_columns, out_count) },
        neg
    )
}

unsafe fn file_statistics_get_columns_inner(
    statistics: *const LanceFileStatistics,
    out_columns: *mut *const LanceColumnStatistics,
    out_count: *mut usize,
) -> Result<i32> {
    if statistics.is_null() || out_columns.is_null() || out_count.is_null() {
        return Err(Error::invalid_input(
            "statistics, out_columns, and out_count must not be NULL",
        ));
    }
    let statistics = unsafe { &*statistics };
    unsafe {
        ptr::write(out_columns, statistics.columns.as_ptr());
        ptr::write(out_count, statistics.columns.len());
    }
    Ok(0)
}

/// Close a file-statistics snapshot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_statistics_close(statistics: *mut LanceFileStatistics) {
    if !statistics.is_null() {
        swallow_unwind("lance_file_statistics_close", || unsafe {
            let _ = Box::from_raw(statistics);
        });
    }
}

/// Close a reader handle. Exported streams retain their own native ownership.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_file_reader_close(reader: *mut LanceFileReader) {
    if !reader.is_null() {
        swallow_unwind("lance_file_reader_close", || unsafe {
            let _ = Box::from_raw(reader);
        });
    }
}
