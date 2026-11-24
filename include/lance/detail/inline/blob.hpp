/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the blob domain.
#pragma once

#include "lance/detail/declarations.hpp"

namespace lance {

inline void blob_field(
    const std::string& name,
    bool nullable,
    ArrowSchema* out) {
    if (lance_blob_field(name.c_str(), nullable, out) != 0) detail::throw_error();
}

inline void blob_field_with_options(
    const std::string& name,
    bool nullable,
    const BlobFieldOptions& options,
    ArrowSchema* out) {
    LanceBlobFieldOptions raw{};
    if (options.inline_size_threshold) {
        raw.has_inline_size_threshold = true;
        raw.inline_size_threshold =
            static_cast<uint64_t>(*options.inline_size_threshold);
    }
    if (options.dedicated_size_threshold) {
        raw.has_dedicated_size_threshold = true;
        raw.dedicated_size_threshold =
            static_cast<uint64_t>(*options.dedicated_size_threshold);
    }
    if (lance_blob_field_with_options(name.c_str(), nullable, &raw, out) != 0)
        detail::throw_error();
}

inline BlobDescriptorArrayBuilder::BlobDescriptorArrayBuilder(const std::string& column) : handle_(lance_blob_descriptor_array_builder_new(column.c_str())) {
        if (!handle_) detail::throw_error();
    }

inline void BlobDescriptorArrayBuilder::append_inline(const uint8_t* data, size_t len) {
        if (lance_blob_descriptor_array_builder_append_inline(
                handle_.get(), data, len) != 0) {
            detail::throw_error();
        }
    }

inline void BlobDescriptorArrayBuilder::append_inline(const std::vector<uint8_t>& data) {
        append_inline(data.empty() ? nullptr : data.data(), data.size());
    }

inline void BlobDescriptorArrayBuilder::append_null() {
        if (lance_blob_descriptor_array_builder_append_null(handle_.get()) != 0)
            detail::throw_error();
    }

inline void BlobDescriptorArrayBuilder::append_packed(uint32_t blob_id, uint64_t offset, uint64_t length) {
        if (lance_blob_descriptor_array_builder_append_packed(
                handle_.get(), blob_id, offset, length) != 0) {
            detail::throw_error();
        }
    }

inline void BlobDescriptorArrayBuilder::append_packed(uint32_t blob_id, const BlobRange& range) {
        append_packed(blob_id, range.offset, range.length);
    }

inline void BlobDescriptorArrayBuilder::extend_packed(uint32_t blob_id,
                       const std::vector<BlobRange>& ranges) {
        std::vector<LanceBlobRange> raw;
        raw.reserve(ranges.size());
        for (const auto& range : ranges) {
            raw.push_back({range.offset, range.length});
        }
        if (lance_blob_descriptor_array_builder_extend_packed(
                handle_.get(), blob_id, raw.data(), raw.size()) != 0) {
            detail::throw_error();
        }
    }

inline void BlobDescriptorArrayBuilder::append_dedicated(uint32_t blob_id, uint64_t length) {
        if (lance_blob_descriptor_array_builder_append_dedicated(
                handle_.get(), blob_id, length) != 0) {
            detail::throw_error();
        }
    }

inline void BlobDescriptorArrayBuilder::append_managed(
        const std::string& uri,
        uint64_t offset,
        uint64_t length,
        std::optional<uint32_t> base_id) {
        if (lance_blob_descriptor_array_builder_append_managed(
                handle_.get(), uri.c_str(), offset, length, base_id.has_value(),
                base_id.value_or(0)) != 0) {
            detail::throw_error();
        }
    }

inline void BlobDescriptorArrayBuilder::append_external(uint32_t base_id,
                         const std::string& uri,
                         uint64_t offset,
                         uint64_t length) {
        if (lance_blob_descriptor_array_builder_append_external(
                handle_.get(), base_id, uri.c_str(), offset, length) != 0) {
            detail::throw_error();
        }
    }

inline void BlobDescriptorArrayBuilder::push_external(const std::string& uri,
                       std::optional<BlobRange> range) {
        if (lance_blob_descriptor_array_builder_push_external(
                handle_.get(), uri.c_str(), range.has_value(),
                range ? range->offset : 0, range ? range->length : 0) != 0) {
            detail::throw_error();
        }
    }

inline void BlobDescriptorArrayBuilder::field(ArrowSchema* out) const {
        if (lance_blob_descriptor_array_builder_field(handle_.get(), out) != 0)
            detail::throw_error();
    }

inline void BlobDescriptorArrayBuilder::finish(ArrowArray* out_array, ArrowSchema* out_schema) {
        if (lance_blob_descriptor_array_builder_finish(
                handle_.get(), out_array, out_schema) != 0) {
            detail::throw_error();
        }
    }

inline PackedBlobWriter::PackedBlobWriter(LancePackedBlobWriter* writer) : handle_(writer) {}

inline uint32_t PackedBlobWriter::blob_id() const {
        uint32_t value = lance_packed_blob_writer_blob_id(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline std::string PackedBlobWriter::path() const {
        const char* raw = lance_packed_blob_writer_path(handle_.get());
        return detail::take_string(raw);
    }

inline void PackedBlobWriter::write_blob(const uint8_t* data, size_t len) {
        if (lance_packed_blob_writer_write_blob(handle_.get(), data, len) != 0)
            detail::throw_error();
    }

inline void PackedBlobWriter::write_blob(const std::vector<uint8_t>& data) {
        write_blob(data.empty() ? nullptr : data.data(), data.size());
    }

inline void PackedBlobWriter::write_blobs(ArrowArray* payloads, const ArrowSchema* schema) {
        if (lance_packed_blob_writer_write_blobs(
                handle_.get(), payloads, schema) != 0) {
            detail::throw_error();
        }
    }

inline void PackedBlobWriter::finish_array(
        const std::string& field_name,
        ArrowArray* out_array,
        ArrowSchema* out_schema) {
        if (lance_packed_blob_writer_finish_array(
                handle_.get(), field_name.c_str(), out_array, out_schema) != 0) {
            detail::throw_error();
        }
    }

inline DedicatedBlobWriter::DedicatedBlobWriter(LanceDedicatedBlobWriter* writer) : handle_(writer) {}

inline uint32_t DedicatedBlobWriter::blob_id() const {
        uint32_t value = lance_dedicated_blob_writer_blob_id(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return value;
    }

inline std::string DedicatedBlobWriter::path() const {
        const char* raw = lance_dedicated_blob_writer_path(handle_.get());
        return detail::take_string(raw);
    }

inline void DedicatedBlobWriter::write(const uint8_t* data, size_t len) {
        if (lance_dedicated_blob_writer_write(handle_.get(), data, len) != 0)
            detail::throw_error();
    }

inline void DedicatedBlobWriter::write(const std::vector<uint8_t>& data) {
        write(data.empty() ? nullptr : data.data(), data.size());
    }

inline void DedicatedBlobWriter::finish_array(
        const std::string& field_name,
        ArrowArray* out_array,
        ArrowSchema* out_schema) {
        if (lance_dedicated_blob_writer_finish_array(
                handle_.get(), field_name.c_str(), out_array, out_schema) != 0) {
            detail::throw_error();
        }
    }

inline BlobFile::BlobFile(LanceBlobFile* blob) : handle_(blob) {}

inline uint64_t BlobFile::size() const {
        uint64_t n = lance_blob_file_size(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return n;
    }

inline std::vector<uint8_t> BlobFile::read() {
        uint64_t blob_size = size();
        uint64_t cursor = tell();
        uint64_t remaining = cursor >= blob_size ? 0 : blob_size - cursor;
        std::vector<uint8_t> out(static_cast<size_t>(remaining));
        if (lance_blob_file_read(handle_.get(), out.data(), out.size()) != 0)
            detail::throw_error();
        return out;
    }

inline std::vector<uint8_t> BlobFile::read_up_to(size_t len) {
        std::vector<uint8_t> out(len);
        size_t bytes_read = 0;
        if (lance_blob_file_read_up_to(
                handle_.get(), out.data(), len, &bytes_read) != 0)
            detail::throw_error();
        out.resize(bytes_read);
        return out;
    }

inline size_t BlobFile::read_into(uint8_t* dst, size_t len) {
        size_t bytes_read = 0;
        if (lance_blob_file_read_up_to(
                handle_.get(), dst, len, &bytes_read) != 0)
            detail::throw_error();
        return bytes_read;
    }

inline std::vector<uint8_t> BlobFile::read_range(uint64_t offset, size_t len) const {
        std::vector<uint8_t> out(len);
        if (lance_blob_file_read_range(
                handle_.get(), offset, out.data(), len) != 0)
            detail::throw_error();
        return out;
    }

inline std::vector<std::vector<uint8_t>> BlobFile::read_ranges(
        const std::vector<BlobRange>& ranges) const {
        std::vector<LanceBlobRange> c_ranges;
        std::vector<std::vector<uint8_t>> out;
        c_ranges.reserve(ranges.size());
        out.reserve(ranges.size());
        for (const auto& range : ranges) {
            c_ranges.push_back({range.offset, range.length});
            out.emplace_back(static_cast<size_t>(range.length));
        }

        std::vector<uint8_t*> destinations;
        std::vector<size_t> destination_lens;
        destinations.reserve(out.size());
        destination_lens.reserve(out.size());
        for (auto& buffer : out) {
            destinations.push_back(buffer.empty() ? nullptr : buffer.data());
            destination_lens.push_back(buffer.size());
        }

        if (lance_blob_file_read_ranges(
                handle_.get(), c_ranges.data(), c_ranges.size(),
                destinations.data(), destination_lens.data()) != 0) {
            detail::throw_error();
        }
        return out;
    }

inline void BlobFile::set_buffer_size(size_t buffer_size) {
        if (lance_blob_file_set_buffer_size(handle_.get(), buffer_size) != 0)
            detail::throw_error();
    }

inline void BlobFile::seek(uint64_t pos) {
        if (lance_blob_file_seek(handle_.get(), pos) != 0) detail::throw_error();
    }

inline uint64_t BlobFile::tell() const {
        uint64_t pos = 0;
        if (lance_blob_file_tell(handle_.get(), &pos) != 0) detail::throw_error();
        return pos;
    }

inline void BlobFile::close() {
        if (lance_blob_file_mark_closed(handle_.get()) != 0) detail::throw_error();
    }

inline bool BlobFile::closed() const {
        bool value = false;
        if (lance_blob_file_is_closed(handle_.get(), &value) != 0)
            detail::throw_error();
        return value;
    }

}  // namespace lance
