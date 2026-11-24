/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the file domain.
#pragma once

#include "lance/detail/declarations.hpp"

namespace lance {

inline ArrowSchemaOwner::ArrowSchemaOwner(ArrowSchema schema) noexcept : schema_(schema), active_(schema.release != nullptr) {}

inline ArrowSchemaOwner::~ArrowSchemaOwner() { reset(); }

inline ArrowSchemaOwner::ArrowSchemaOwner(ArrowSchemaOwner&& other) noexcept : schema_(other.schema_), active_(other.active_) {
        other.schema_ = {};
        other.active_ = false;
    }

namespace file_detail {

inline FileBufferDescriptor buffer_descriptor(
    const LanceBufferDescriptor& descriptor) {
    return FileBufferDescriptor{descriptor.position, descriptor.size};
}

}

namespace file_detail {

inline FileEncodingDescriptor encoding_descriptor(
    const LanceEncodingDescriptor& descriptor) {
    FileEncodingDescriptor output;
    output.kind = static_cast<FileEncodingKind>(descriptor.kind);
    output.buffer_location = descriptor.buffer_location;
    output.buffer_length = descriptor.buffer_length;
    if (descriptor.direct_bytes_len != 0) {
        if (!descriptor.direct_bytes) detail::throw_error();
        output.direct_bytes.assign(
            descriptor.direct_bytes,
            descriptor.direct_bytes + descriptor.direct_bytes_len);
    }
    return output;
}

}

namespace file_detail {

inline LanceFileReaderOptions reader_options(
    const FileReaderOptions& options,
    std::vector<const char*>& columns) {
    LanceFileReaderOptions raw{};
    if (options.columns) {
        columns.reserve(options.columns->size());
        for (const auto& column : *options.columns) {
            columns.push_back(column.c_str());
        }
        raw.has_columns = 1;
        raw.columns = columns.data();
        raw.column_count = columns.size();
    }
    raw.cache_mode = static_cast<int32_t>(options.cache_mode);
    if (options.read_chunk_size) {
        raw.has_read_chunk_size = 1;
        raw.read_chunk_size = *options.read_chunk_size;
    }
    if (options.batch_size_bytes) {
        raw.has_batch_size_bytes = 1;
        raw.batch_size_bytes = *options.batch_size_bytes;
    }
    if (options.cache_repetition_index) {
        raw.has_cache_repetition_index = 1;
        raw.cache_repetition_index = *options.cache_repetition_index ? 1 : 0;
    }
    if (options.validate_on_decode) {
        raw.has_validate_on_decode = 1;
        raw.validate_on_decode = *options.validate_on_decode ? 1 : 0;
    }
    if (options.inline_scheduling) {
        raw.has_inline_scheduling = 1;
        raw.inline_scheduling = *options.inline_scheduling ? 1 : 0;
    }
    return raw;
}

}

namespace file_detail {

inline LanceFileWriterOptions writer_options(
    const FileWriterOptions& options) {
    LanceFileWriterOptions raw{};
    if (options.data_cache_bytes) {
        raw.has_data_cache_bytes = 1;
        raw.data_cache_bytes = *options.data_cache_bytes;
    }
    if (options.max_page_bytes) {
        raw.has_max_page_bytes = 1;
        raw.max_page_bytes = *options.max_page_bytes;
    }
    if (options.keep_original_array) {
        raw.has_keep_original_array = 1;
        raw.keep_original_array = *options.keep_original_array ? 1 : 0;
    }
    if (options.version) raw.version = options.version->c_str();
    return raw;
}

}

inline FileMetadata::FileMetadata(LanceFileMetadata* metadata) : handle_(metadata) {}

inline FileMetadataInfo FileMetadata::info() const {
        LanceFileMetadataInfo raw{};
        if (lance_file_metadata_get_info(handle_.get(), &raw) != 0)
            detail::throw_error();
        return FileMetadataInfo{
            static_cast<FileFormatVersion>(raw.format_version),
            raw.major_version,
            raw.minor_version,
            raw.num_rows,
            raw.num_data_bytes,
            raw.num_column_metadata_bytes,
            raw.num_global_buffer_bytes,
            raw.num_footer_bytes,
            raw.file_size_bytes,
            raw.num_columns,
            raw.num_physical_columns,
            raw.num_global_buffers,
        };
    }

inline ArrowSchemaOwner FileMetadata::schema() const {
        ArrowSchema raw{};
        if (lance_file_metadata_get_schema(handle_.get(), &raw) != 0)
            detail::throw_error();
        return ArrowSchemaOwner(raw);
    }

inline std::vector<FileBufferDescriptor> FileMetadata::global_buffers() const {
        const LanceBufferDescriptor* descriptors = nullptr;
        std::size_t count = 0;
        if (lance_file_metadata_get_global_buffers(
                handle_.get(), &descriptors, &count) != 0) {
            detail::throw_error();
        }
        std::vector<FileBufferDescriptor> output;
        output.reserve(count);
        for (std::size_t index = 0; index < count; index++) {
            output.push_back(
                file_detail::buffer_descriptor(descriptors[index]));
        }
        return output;
    }

inline std::vector<FileColumnMetadata> FileMetadata::columns() const {
        const LanceColumnMetadata* columns = nullptr;
        std::size_t count = 0;
        if (lance_file_metadata_get_columns(
                handle_.get(), &columns, &count) != 0) {
            detail::throw_error();
        }
        std::vector<FileColumnMetadata> output;
        output.reserve(count);
        for (std::size_t index = 0; index < count; index++) {
            const auto& raw_column = columns[index];
            FileColumnMetadata column;
            column.column_buffers.reserve(raw_column.column_buffer_count);
            for (std::size_t buffer = 0;
                 buffer < raw_column.column_buffer_count;
                 buffer++) {
                column.column_buffers.push_back(
                    file_detail::buffer_descriptor(
                        raw_column.column_buffers[buffer]));
            }
            column.encoding =
                file_detail::encoding_descriptor(raw_column.encoding);
            column.pages.reserve(raw_column.page_count);
            for (std::size_t page = 0; page < raw_column.page_count; page++) {
                const auto& raw_page = raw_column.pages[page];
                FilePageMetadata output_page;
                output_page.num_rows = raw_page.num_rows;
                output_page.priority = raw_page.priority;
                output_page.encoding =
                    file_detail::encoding_descriptor(raw_page.encoding);
                if (raw_page.encoding_description) {
                    output_page.encoding_description =
                        raw_page.encoding_description;
                }
                output_page.buffers.reserve(raw_page.buffer_count);
                for (std::size_t buffer = 0;
                     buffer < raw_page.buffer_count;
                     buffer++) {
                    output_page.buffers.push_back(
                        file_detail::buffer_descriptor(
                            raw_page.buffers[buffer]));
                }
                column.pages.push_back(std::move(output_page));
            }
            output.push_back(std::move(column));
        }
        return output;
    }

inline FileStatistics::FileStatistics(LanceFileStatistics* statistics) : handle_(statistics) {}

inline std::vector<FileColumnStatistics> FileStatistics::columns() const {
        const LanceColumnStatistics* columns = nullptr;
        std::size_t count = 0;
        if (lance_file_statistics_get_columns(
                handle_.get(), &columns, &count) != 0) {
            detail::throw_error();
        }
        std::vector<FileColumnStatistics> output;
        output.reserve(count);
        for (std::size_t index = 0; index < count; index++) {
            output.push_back(
                FileColumnStatistics{columns[index].num_pages,
                                     columns[index].size_bytes});
        }
        return output;
    }

inline ByteBuffer::ByteBuffer(LanceByteBuffer* buffer) : handle_(buffer) {}

inline std::vector<uint8_t> ByteBuffer::bytes() const {
        std::size_t length = lance_byte_buffer_len(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        if (length == 0) return {};
        const uint8_t* data = lance_byte_buffer_data(handle_.get());
        if (!data) detail::throw_error();
        return std::vector<uint8_t>(data, data + length);
    }

inline PathList::PathList(LancePathList* list) : handle_(list) {}

inline std::vector<std::string> PathList::paths() const {
        uint64_t count = lance_path_list_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        std::vector<std::string> output;
        output.reserve(static_cast<std::size_t>(count));
        for (uint64_t index = 0; index < count; index++) {
            const char* path = lance_path_list_at(
                handle_.get(), static_cast<std::size_t>(index));
            if (!path) detail::throw_error();
            output.emplace_back(path);
        }
        return output;
    }

inline DelimitedList::DelimitedList(LanceDelimitedList* list) : handle_(list) {}

inline std::vector<std::string> DelimitedList::common_prefixes() const {
        uint64_t count =
            lance_delimited_list_common_prefix_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        std::vector<std::string> output;
        output.reserve(static_cast<std::size_t>(count));
        for (uint64_t index = 0; index < count; index++) {
            const char* path = lance_delimited_list_common_prefix_at(
                handle_.get(), static_cast<std::size_t>(index));
            if (!path) detail::throw_error();
            output.emplace_back(path);
        }
        return output;
    }

inline std::vector<std::string> DelimitedList::objects() const {
        uint64_t count = lance_delimited_list_object_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        std::vector<std::string> output;
        output.reserve(static_cast<std::size_t>(count));
        for (uint64_t index = 0; index < count; index++) {
            const char* path = lance_delimited_list_object_at(
                handle_.get(), static_cast<std::size_t>(index));
            if (!path) detail::throw_error();
            output.emplace_back(path);
        }
        return output;
    }

inline FileSession::FileSession(
        const std::string& base_uri,
        const StorageOptionsAccessor* storage_options,
        const Session* session,
        std::optional<FileSchedulerConfig> scheduler_config) : handle_(nullptr) {
        LanceSchedulerConfig raw_config{};
        const LanceSchedulerConfig* config_ptr = nullptr;
        if (scheduler_config) {
            raw_config.io_buffer_size_bytes =
                scheduler_config->io_buffer_size_bytes;
            raw_config.use_lite_scheduler =
                static_cast<int32_t>(scheduler_config->use_lite_scheduler);
            config_ptr = &raw_config;
        }
        auto* raw = lance_file_session_new(
            base_uri.c_str(),
            storage_options ? storage_options->c_handle() : nullptr,
            session ? session->c_handle() : nullptr,
            config_ptr);
        if (!raw) detail::throw_error();
        handle_ = Handle<LanceFileSession, lance_file_session_close>(raw);
    }

inline void FileSession::reset() noexcept {
        handle_ = Handle<LanceFileSession, lance_file_session_close>();
    }

inline  FileSession::operator bool() const noexcept { return bool(handle_); }

inline std::string FileSession::base_path() const {
        return detail::take_string(
            lance_file_session_base_path(handle_.get()));
    }

inline PackedBlobWriter FileSession::open_packed_blob_writer(
        const std::string& data_file_path,
        uint32_t blob_id) const {
        auto* raw = lance_file_session_open_packed_blob_writer(
            handle_.get(), data_file_path.c_str(), blob_id);
        if (!raw) detail::throw_error();
        return PackedBlobWriter(raw);
    }

inline DedicatedBlobWriter FileSession::open_dedicated_blob_writer(
        const std::string& data_file_path,
        uint32_t blob_id) const {
        auto* raw = lance_file_session_open_dedicated_blob_writer(
            handle_.get(), data_file_path.c_str(), blob_id);
        if (!raw) detail::throw_error();
        return DedicatedBlobWriter(raw);
    }

inline bool FileSession::contains(const std::string& relative_path) const {
        int32_t exists = 0;
        if (lance_file_session_contains(
                handle_.get(), relative_path.c_str(), &exists) != 0) {
            detail::throw_error();
        }
        return exists != 0;
    }

inline PathList FileSession::list(std::optional<std::string> relative_path) const {
        auto* raw = lance_file_session_list(
            handle_.get(),
            relative_path ? relative_path->c_str() : nullptr);
        if (!raw) detail::throw_error();
        return PathList(raw);
    }

inline DelimitedList FileSession::list_with_delimiter(
        std::optional<std::string> relative_path) const {
        auto* raw = lance_file_session_list_with_delimiter(
            handle_.get(),
            relative_path ? relative_path->c_str() : nullptr);
        if (!raw) detail::throw_error();
        return DelimitedList(raw);
    }

inline ByteBuffer FileSession::read_range(
        const std::string& relative_path,
        uint64_t offset,
        uint64_t length) const {
        auto* raw = lance_file_session_read_range(
            handle_.get(), relative_path.c_str(), offset, length);
        if (!raw) detail::throw_error();
        return ByteBuffer(raw);
    }

inline void FileSession::delete_file(const std::string& relative_path) const {
        if (lance_file_session_delete_file(
                handle_.get(), relative_path.c_str()) != 0) {
            detail::throw_error();
        }
    }

inline void FileSession::upload_file(
        const std::string& local_path,
        const std::string& relative_remote_path) const {
        if (lance_file_session_upload_file(
                handle_.get(),
                local_path.c_str(),
                relative_remote_path.c_str()) != 0) {
            detail::throw_error();
        }
    }

inline void FileSession::download_file(
        const std::string& relative_remote_path,
        const std::string& local_path) const {
        if (lance_file_session_download_file(
                handle_.get(),
                relative_remote_path.c_str(),
                local_path.c_str()) != 0) {
            detail::throw_error();
        }
    }

inline LanceScanStats FileSession::scheduler_stats() const {
        LanceScanStats stats{};
        if (lance_file_session_get_scheduler_stats(handle_.get(), &stats) != 0)
            detail::throw_error();
        return stats;
    }

inline LanceObjectStoreRegistryStats FileSession::registry_stats() const {
        LanceObjectStoreRegistryStats stats{};
        if (lance_file_session_get_registry_stats(handle_.get(), &stats) != 0)
            detail::throw_error();
        return stats;
    }

inline FileStoreInfo FileSession::store_info() const {
        LanceObjectStoreInfo info{};
        if (lance_file_session_get_store_info(handle_.get(), &info) != 0)
            detail::throw_error();
        return FileStoreInfo{
            info.block_size,
            info.max_iop_size,
            info.io_parallelism,
            info.is_local != 0,
            info.is_cloud != 0,
            info.prefers_lite_scheduler != 0,
        };
    }

inline LanceObjectStoreIoStats FileSession::store_io_stats() const {
        LanceObjectStoreIoStats stats{};
        if (lance_file_session_get_store_io_stats(handle_.get(), &stats) != 0)
            detail::throw_error();
        return stats;
    }

inline FileReader::FileReader(LanceFileReader* reader) : handle_(reader) {}

inline void FileReader::reset() noexcept {
        handle_ = Handle<LanceFileReader, lance_file_reader_close>();
    }

inline  FileReader::operator bool() const noexcept { return bool(handle_); }

inline uint64_t FileReader::num_rows() const {
        uint64_t rows = 0;
        if (lance_file_reader_num_rows(handle_.get(), &rows) != 0)
            detail::throw_error();
        return rows;
    }

inline FileMetadata FileReader::metadata() const {
        LanceFileMetadata* metadata = nullptr;
        if (lance_file_reader_get_metadata(handle_.get(), &metadata) != 0)
            detail::throw_error();
        return FileMetadata(metadata);
    }

inline FileStatistics FileReader::statistics() const {
        LanceFileStatistics* statistics = nullptr;
        if (lance_file_reader_get_statistics(handle_.get(), &statistics) != 0)
            detail::throw_error();
        return FileStatistics(statistics);
    }

inline ByteBuffer FileReader::read_global_buffer(uint32_t index) const {
        LanceByteBuffer* buffer = nullptr;
        if (lance_file_reader_read_global_buffer(
                handle_.get(), index, &buffer) != 0) {
            detail::throw_error();
        }
        return ByteBuffer(buffer);
    }

inline ArrowStream FileReader::read_all(
        uint32_t batch_size,
        uint32_t batch_readahead) const {
        ArrowArrayStream stream{};
        if (lance_file_reader_read_all(
                handle_.get(), batch_size, batch_readahead, &stream) != 0) {
            detail::throw_error();
        }
        return ArrowStream(stream);
    }

inline ArrowStream FileReader::read_range(
        uint64_t offset,
        uint64_t num_rows,
        uint32_t batch_size,
        uint32_t batch_readahead) const {
        ArrowArrayStream stream{};
        if (lance_file_reader_read_range(
                handle_.get(),
                offset,
                num_rows,
                batch_size,
                batch_readahead,
                &stream) != 0) {
            detail::throw_error();
        }
        return ArrowStream(stream);
    }

inline ArrowStream FileReader::read_indices(
        const std::vector<uint32_t>& indices,
        uint32_t batch_size,
        uint32_t batch_readahead) const {
        ArrowArrayStream stream{};
        if (lance_file_reader_read_indices(
                handle_.get(),
                indices.empty() ? nullptr : indices.data(),
                indices.size(),
                batch_size,
                batch_readahead,
                &stream) != 0) {
            detail::throw_error();
        }
        return ArrowStream(stream);
    }

inline LanceScanStats FileReader::io_stats() const {
        LanceScanStats stats{};
        if (lance_file_reader_get_io_stats(handle_.get(), &stats) != 0)
            detail::throw_error();
        return stats;
    }

inline FileWriter::FileWriter(LanceFileWriter* writer) : handle_(writer) {}

inline void FileWriter::reset() noexcept {
        handle_ = Handle<LanceFileWriter, lance_file_writer_close>();
    }

inline  FileWriter::operator bool() const noexcept { return bool(handle_); }

inline void FileWriter::write_batch(const Batch& batch) {
        if (lance_file_writer_write_batch(handle_.get(), batch.c_handle()) != 0)
            detail::throw_error();
    }

inline void FileWriter::add_schema_metadata(
        const std::string& key,
        const std::string& value) {
        if (lance_file_writer_add_schema_metadata(
                handle_.get(), key.c_str(), value.c_str()) != 0) {
            detail::throw_error();
        }
    }

inline uint32_t FileWriter::add_global_buffer(const std::vector<uint8_t>& bytes) {
        uint32_t index = 0;
        if (lance_file_writer_add_global_buffer(
                handle_.get(),
                bytes.empty() ? nullptr : bytes.data(),
                bytes.size(),
                &index) != 0) {
            detail::throw_error();
        }
        return index;
    }

inline FileWriteSummary FileWriter::finish() {
        LanceFileWriteSummary summary{};
        if (lance_file_writer_finish(handle_.get(), &summary) != 0)
            detail::throw_error();
        return FileWriteSummary{summary.num_rows, summary.size_bytes};
    }

inline void FileWriter::abort() {
        if (lance_file_writer_abort(handle_.get()) != 0) detail::throw_error();
    }

inline FileReader FileSession::open_reader(
    const std::string& relative_path,
    const FileReaderOptions& options) const {
    std::vector<const char*> columns;
    auto raw_options = file_detail::reader_options(options, columns);
    auto* raw = lance_file_session_open_reader(
        handle_.get(), relative_path.c_str(), &raw_options);
    if (!raw) detail::throw_error();
    return FileReader(raw);
}

inline FileWriter FileSession::open_writer(
    const std::string& relative_path,
    const ArrowSchema* schema,
    const FileWriterOptions& options) const {
    auto raw_options = file_detail::writer_options(options);
    auto* raw = lance_file_session_open_writer(
        handle_.get(), relative_path.c_str(), schema, &raw_options);
    if (!raw) detail::throw_error();
    return FileWriter(raw);
}

}  // namespace lance
