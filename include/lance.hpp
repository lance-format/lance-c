/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

/**
 * @file lance.hpp
 * @brief C++ RAII wrappers for the Lance C API.
 *
 * Header-only library providing:
 *   - lance::Error exception class
 *   - lance::Dataset RAII handle with builder-pattern Scanner
 *   - lance::Scanner fluent API
 *   - All data exchange via Arrow C Data Interface
 */

#ifndef LANCE_HPP
#define LANCE_HPP

#include "lance.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lance {

// ─── Error ───────────────────────────────────────────────────────────────────

class Error : public std::runtime_error {
public:
    LanceErrorCode code;

    Error(LanceErrorCode code, std::string msg)
        : std::runtime_error(std::move(msg)), code(code) {}
};

/// Check thread-local error and throw if non-OK.
inline void check_error() {
    LanceErrorCode code = lance_last_error_code();
    if (code != LANCE_OK) {
        const char* msg = lance_last_error_message();
        std::string owned(msg ? msg : "Unknown error");
        if (msg) lance_free_string(msg);
        throw Error(code, std::move(owned));
    }
}

// ─── RAII Handle Template ────────────────────────────────────────────────────

template <typename T, void (*Deleter)(T*)>
class Handle {
    T* ptr_;

public:
    explicit Handle(T* ptr = nullptr) : ptr_(ptr) {}
    ~Handle() {
        if (ptr_) Deleter(ptr_);
    }

    Handle(Handle&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            if (ptr_) Deleter(ptr_);
            ptr_ = o.ptr_;
            o.ptr_ = nullptr;
        }
        return *this;
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    T* get() const { return ptr_; }
    T* release() {
        auto p = ptr_;
        ptr_ = nullptr;
        return p;
    }
    explicit operator bool() const { return ptr_ != nullptr; }
};

// ─── Forward Declarations ────────────────────────────────────────────────────

class Scanner;

// ─── Version history ─────────────────────────────────────────────────────────

/// Metadata for a single dataset version.
/// `id` mirrors the upstream Version::version (monotonic manifest version);
/// `timestamp_ms` is Unix epoch milliseconds.
struct VersionInfo {
    uint64_t id;
    int64_t  timestamp_ms;
};

// ─── Dataset ─────────────────────────────────────────────────────────────────

class Dataset {
    Handle<LanceDataset, lance_dataset_close> handle_;

public:
    /// Open a dataset at the given URI.
    static Dataset open(
        const std::string& uri,
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {},
        uint64_t version = 0) {

        // Build NULL-terminated key-value array for storage options.
        std::vector<const char*> kv;
        for (auto& [k, v] : storage_opts) {
            kv.push_back(k.c_str());
            kv.push_back(v.c_str());
        }
        kv.push_back(nullptr);

        const char* const* opts_ptr =
            storage_opts.empty() ? nullptr : kv.data();

        auto* ds = lance_dataset_open(uri.c_str(), opts_ptr, version);
        if (!ds) check_error();
        return Dataset(ds);
    }

    /// Number of rows in the dataset.
    uint64_t count_rows() const {
        uint64_t n = lance_dataset_count_rows(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return n;
    }

    /// Version of this dataset snapshot.
    uint64_t version() const {
        return lance_dataset_version(handle_.get());
    }

    /// Latest version ID (queries object store).
    uint64_t latest_version() const {
        uint64_t v = lance_dataset_latest_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return v;
    }

    /// Snapshot the dataset's version history, ordered by version id.
    /// Throws lance::Error on failure.
    std::vector<VersionInfo> versions() const {
        auto* raw = lance_dataset_versions(handle_.get());
        if (!raw) check_error();
        Handle<LanceVersions, lance_versions_close> snap(raw);

        uint64_t n = lance_versions_count(snap.get());
        std::vector<VersionInfo> out;
        out.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n; i++) {
            VersionInfo info;
            info.id = lance_versions_id_at(snap.get(), static_cast<size_t>(i));
            info.timestamp_ms =
                lance_versions_timestamp_ms_at(snap.get(), static_cast<size_t>(i));
            if (lance_last_error_code() != LANCE_OK) check_error();
            out.push_back(info);
        }
        return out;
    }

    /// Export the schema as an Arrow C Data Interface struct.
    void schema(ArrowSchema* out) const {
        if (lance_dataset_schema(handle_.get(), out) != 0) {
            check_error();
        }
    }

    /// Take rows by indices. Results exported as ArrowArrayStream.
    void take(const uint64_t* indices, size_t num_indices,
              const std::vector<std::string>& columns,
              ArrowArrayStream* out) const {
        std::vector<const char*> col_ptrs;
        for (auto& c : columns) col_ptrs.push_back(c.c_str());
        col_ptrs.push_back(nullptr);
        const char* const* cols_ptr = columns.empty() ? nullptr : col_ptrs.data();

        if (lance_dataset_take(handle_.get(), indices, num_indices, cols_ptr, out) != 0) {
            check_error();
        }
    }

    /// Take all columns.
    void take(const uint64_t* indices, size_t num_indices,
              ArrowArrayStream* out) const {
        if (lance_dataset_take(handle_.get(), indices, num_indices, nullptr, out) != 0) {
            check_error();
        }
    }

    /// Create a Scanner builder for this dataset.
    Scanner scan() const;

    /// Number of fragments in the dataset.
    uint64_t fragment_count() const {
        uint64_t n = lance_dataset_fragment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return n;
    }

    /// Get all fragment IDs.
    std::vector<uint64_t> fragment_ids() const {
        auto count = fragment_count();
        std::vector<uint64_t> ids(count);
        if (count > 0) {
            if (lance_dataset_fragment_ids(handle_.get(), ids.data()) != 0)
                check_error();
        }
        return ids;
    }

    /// Create a vector index on a column.
    void create_vector_index(const std::string& column,
                             const LanceVectorIndexParams& params,
                             const std::string& name = "",
                             bool replace = false) {
        const char* name_c = name.empty() ? nullptr : name.c_str();
        if (lance_dataset_create_vector_index(handle_.get(), column.c_str(),
                                               name_c, &params, replace) != 0)
            check_error();
    }

    /// Create a scalar index on a column.
    void create_scalar_index(const std::string& column,
                             LanceScalarIndexType index_type,
                             const std::string& name = "",
                             const std::string& params_json = "",
                             bool replace = false) {
        const char* name_c = name.empty() ? nullptr : name.c_str();
        const char* json_c = params_json.empty() ? nullptr : params_json.c_str();
        if (lance_dataset_create_scalar_index(handle_.get(), column.c_str(),
                                               name_c, index_type,
                                               json_c, replace) != 0)
            check_error();
    }

    /// Drop an index by name.
    void drop_index(const std::string& name) {
        if (lance_dataset_drop_index(handle_.get(), name.c_str()) != 0)
            check_error();
    }

    /// Number of user indexes (excludes system indexes).
    uint64_t index_count() const {
        uint64_t n = lance_dataset_index_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return n;
    }

    /// JSON array describing all user indexes.
    std::string list_indices_json() const {
        const char* json = lance_dataset_index_list_json(handle_.get());
        if (!json) check_error();
        std::string out(json);
        lance_free_string(json);
        return out;
    }

    /// Access the underlying C handle (does not transfer ownership).
    const LanceDataset* c_handle() const { return handle_.get(); }

private:
    explicit Dataset(LanceDataset* ptr) : handle_(ptr) {}
};

// ─── Scanner ─────────────────────────────────────────────────────────────────

class Scanner {
    Handle<LanceScanner, lance_scanner_close> handle_;

public:
    explicit Scanner(LanceScanner* s) : handle_(s) {}

    /// Set the row limit.
    Scanner& limit(int64_t n) {
        if (lance_scanner_set_limit(handle_.get(), n) != 0)
            check_error();
        return *this;
    }

    /// Set the row offset.
    Scanner& offset(int64_t n) {
        if (lance_scanner_set_offset(handle_.get(), n) != 0)
            check_error();
        return *this;
    }

    /// Set the batch size.
    Scanner& batch_size(int64_t n) {
        if (lance_scanner_set_batch_size(handle_.get(), n) != 0)
            check_error();
        return *this;
    }

    /// Enable/disable row ID in output.
    Scanner& with_row_id(bool enable = true) {
        if (lance_scanner_with_row_id(handle_.get(), enable) != 0)
            check_error();
        return *this;
    }

    /// Restrict scan to specific fragment IDs.
    Scanner& fragment_ids(const uint64_t* ids, size_t len) {
        if (lance_scanner_set_fragment_ids(handle_.get(), ids, len) != 0)
            check_error();
        return *this;
    }

    /// Restrict scan to specific fragment IDs (vector overload).
    Scanner& fragment_ids(const std::vector<uint64_t>& ids) {
        return fragment_ids(ids.data(), ids.size());
    }

    /// Materialize the scan as an ArrowArrayStream (blocking).
    void to_arrow_stream(ArrowArrayStream* out) {
        if (lance_scanner_to_arrow_stream(handle_.get(), out) != 0)
            check_error();
    }

    /// Start an async scan. Callback fires when ArrowArrayStream is ready.
    void scan_async(LanceCallback callback, void* ctx) const {
        lance_scanner_scan_async(handle_.get(), callback, ctx);
    }

    /// k-NN search (Float32 sugar).
    Scanner& nearest(const std::string& column, const float* q, size_t dim, uint32_t k) {
        if (lance_scanner_nearest(handle_.get(), column.c_str(),
                                   q, dim, LANCE_DTYPE_FLOAT32, k) != 0)
            check_error();
        return *this;
    }

    /// k-NN search (typed).
    Scanner& nearest(const std::string& column, const void* q, size_t dim,
                     LanceDataType dtype, uint32_t k) {
        if (lance_scanner_nearest(handle_.get(), column.c_str(),
                                   q, dim, dtype, k) != 0)
            check_error();
        return *this;
    }

    Scanner& nprobes(uint32_t n) {
        if (lance_scanner_set_nprobes(handle_.get(), n) != 0) check_error();
        return *this;
    }
    Scanner& refine_factor(uint32_t f) {
        if (lance_scanner_set_refine_factor(handle_.get(), f) != 0) check_error();
        return *this;
    }
    Scanner& ef(uint32_t e) {
        if (lance_scanner_set_ef(handle_.get(), e) != 0) check_error();
        return *this;
    }
    Scanner& metric(LanceMetricType m) {
        if (lance_scanner_set_metric(handle_.get(), m) != 0) check_error();
        return *this;
    }
    Scanner& use_index(bool enable) {
        if (lance_scanner_set_use_index(handle_.get(), enable) != 0) check_error();
        return *this;
    }
    Scanner& prefilter(bool enable) {
        if (lance_scanner_set_prefilter(handle_.get(), enable) != 0) check_error();
        return *this;
    }

    /// BM25 full-text search.
    /// `columns` empty → search all FTS-indexed columns.
    /// `max_fuzzy_distance` 0 = exact; >0 = MatchQuery::with_fuzziness.
    Scanner& full_text_search(const std::string& query,
                              const std::vector<std::string>& columns = {},
                              uint32_t max_fuzzy_distance = 0) {
        std::vector<const char*> col_ptrs;
        for (auto& c : columns) col_ptrs.push_back(c.c_str());
        col_ptrs.push_back(nullptr);
        const char* const* cols_c =
            columns.empty() ? nullptr : col_ptrs.data();
        if (lance_scanner_full_text_search(handle_.get(), query.c_str(),
                                            cols_c, max_fuzzy_distance) != 0)
            check_error();
        return *this;
    }

    /// Access the underlying C handle.
    LanceScanner* c_handle() { return handle_.get(); }
};

inline Scanner Dataset::scan() const {
    auto* s = lance_scanner_new(handle_.get(), nullptr, nullptr);
    if (!s) check_error();
    return Scanner(s);
}

// ─── Batch ───────────────────────────────────────────────────────────────────

class Batch {
    Handle<LanceBatch, lance_batch_free> handle_;

public:
    explicit Batch(LanceBatch* b) : handle_(b) {}

    /// Export as Arrow C Data Interface structs.
    void to_arrow(ArrowArray* out_array, ArrowSchema* out_schema) const {
        if (lance_batch_to_arrow(handle_.get(), out_array, out_schema) != 0)
            check_error();
    }
};

} // namespace lance

// ─── Fragment writer (free functions) ────────────────────────────────────────

namespace lance {

/**
 * Write an Arrow record batch stream to fragment files at `uri`.
 *
 * Data files are written under `<uri>/data/`. A Rust finalizer reconstructs
 * Fragment metadata from the file footers and commits via CommitBuilder.
 * No dynamic memory is returned to the caller.
 *
 * @param uri          Directory URI (file://, s3://, etc.)
 * @param schema       Required Arrow schema — stream schema must match.
 * @param stream       ArrowArrayStream to consume. Must not be used after this call.
 * @param storage_opts Key-value storage options, or empty for defaults.
 * @throws lance::Error on failure.
 */
inline void write_fragments(
    const std::string& uri,
    const ArrowSchema* schema,
    ArrowArrayStream* stream,
    const std::vector<std::pair<std::string, std::string>>& storage_opts = {})
{
    std::vector<const char*> kv;
    for (auto& [k, v] : storage_opts) {
        kv.push_back(k.c_str());
        kv.push_back(v.c_str());
    }
    kv.push_back(nullptr);

    const char* const* opts_ptr = storage_opts.empty() ? nullptr : kv.data();
    if (lance_write_fragments(uri.c_str(), schema, stream, opts_ptr) != 0) {
        check_error();
    }
}

} // namespace lance

#endif /* LANCE_HPP */
