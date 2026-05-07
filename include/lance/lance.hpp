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

#include "lance/lance.h"

#include <cstdint>
#include <memory>
#include <optional>
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

// ─── Write mode ──────────────────────────────────────────────────────────────

enum class WriteMode : int32_t {
    Create    = LANCE_WRITE_CREATE,
    Append    = LANCE_WRITE_APPEND,
    Overwrite = LANCE_WRITE_OVERWRITE,
};

/// Tunable parameters for Dataset::write. Numeric fields default-out via 0;
/// `data_storage_version` defaults out via `std::nullopt`.
///
/// `enable_stable_row_ids` has no default sentinel — whatever value the
/// caller writes is forwarded to upstream. Today this matches upstream's
/// default (`false`), so a default-constructed WriteParams is a no-op; if
/// upstream ever changes its default, callers must set this field explicitly.
struct WriteParams {
    uint64_t                   max_rows_per_file    = 0;
    uint64_t                   max_rows_per_group   = 0;
    uint64_t                   max_bytes_per_file   = 0;
    /// Lance file format version, e.g. "2.0", "2.1", "stable", "legacy".
    std::optional<std::string> data_storage_version;
    bool                       enable_stable_row_ids = false;
};

// ─── Dataset ─────────────────────────────────────────────────────────────────

class Dataset {
    Handle<LanceDataset, lance_dataset_close> handle_;

public:
    /// Open a dataset at the given URI. Pass `version` = 0 (the default) for
    /// the latest, or a specific version id from `versions()` to check out
    /// that version, e.g. `lance::Dataset::open("data.lance", {}, /*version=*/42)`.
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

    /// Write an Arrow record batch stream to a Lance dataset and return the
    /// open dataset at the committed version.
    ///
    /// The stream must be self-describing; its own schema is used. Treat the
    /// stream as consumed once this call returns or throws — do not reuse it.
    /// Throws lance::Error on failure (including if `stream` is null).
    static Dataset write(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {}) {

        return write(uri, stream, mode, WriteParams{}, storage_opts);
    }

    /// Same as the four-argument `write` but tunes the output via `params`.
    /// Pass a default-constructed `WriteParams{}` to inherit upstream defaults.
    static Dataset write(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const WriteParams& params,
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {}) {

        if (stream == nullptr) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT, "stream must not be null");
        }

        // RAII guard for the stream. Until `lance_dataset_write_with_params`
        // is called, any exception (failed `get_schema`, `std::bad_alloc`
        // while building `kv`, etc.) must release the stream. After that call
        // Rust owns it, so we `disarm()` immediately before invoking the C API.
        struct StreamGuard {
            ArrowArrayStream* s;
            bool armed = true;
            // Explicit constructor: `= delete`d copy/move ctors disqualify
            // this from being an aggregate under C++20, so brace-init like
            // `StreamGuard{stream}` would otherwise fail to compile there.
            explicit StreamGuard(ArrowArrayStream* p) noexcept : s(p) {}
            ~StreamGuard() noexcept {
                if (armed && s && s->release) s->release(s);
            }
            void disarm() noexcept { armed = false; }
            StreamGuard(const StreamGuard&) = delete;
            StreamGuard& operator=(const StreamGuard&) = delete;
            StreamGuard(StreamGuard&&) = delete;
            StreamGuard& operator=(StreamGuard&&) = delete;
        } stream_guard{stream};

        // Defensive: a non-conforming or already-released producer may have a
        // null `get_schema`. Without this guard a bad caller would crash with
        // a null function-pointer dereference on the next line.
        if (stream->get_schema == nullptr) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "stream get_schema callback is null");
        }

        // Arm SchemaGuard before calling `get_schema` so a non-conforming
        // producer that partially populates the schema before returning an
        // error still has its `release` fired on unwind. The zero-init keeps
        // the destructor a no-op on the clean-error path (release == null).
        struct SchemaGuard {
            ArrowSchema* s;
            // Explicit constructor for the same C++20 aggregate-init reason
            // documented on StreamGuard above.
            explicit SchemaGuard(ArrowSchema* p) noexcept : s(p) {}
            ~SchemaGuard() noexcept {
                if (s && s->release) s->release(s);
            }
            SchemaGuard(const SchemaGuard&) = delete;
            SchemaGuard& operator=(const SchemaGuard&) = delete;
            SchemaGuard(SchemaGuard&&) = delete;
            SchemaGuard& operator=(SchemaGuard&&) = delete;
        };
        ArrowSchema schema = {};
        SchemaGuard schema_guard{&schema};

        // On failure, StreamGuard releases the stream and SchemaGuard
        // releases any partial schema state — preserving the "consumed on
        // return or throw" contract for both resources.
        if (stream->get_schema(stream, &schema) != 0) {
            const char* err = stream->get_last_error
                ? stream->get_last_error(stream)
                : nullptr;
            std::string msg = std::string("failed to read stream schema: ") +
                              (err ? err : "unknown");
            throw Error(LANCE_ERR_INVALID_ARGUMENT, msg);
        }

        std::vector<const char*> kv;
        for (auto& [k, v] : storage_opts) {
            kv.push_back(k.c_str());
            kv.push_back(v.c_str());
        }
        kv.push_back(nullptr);
        const char* const* opts_ptr =
            storage_opts.empty() ? nullptr : kv.data();

        LanceWriteParams c_params = {};
        c_params.max_rows_per_file    = params.max_rows_per_file;
        c_params.max_rows_per_group   = params.max_rows_per_group;
        c_params.max_bytes_per_file   = params.max_bytes_per_file;
        c_params.data_storage_version =
            params.data_storage_version ? params.data_storage_version->c_str() : nullptr;
        c_params.enable_stable_row_ids = params.enable_stable_row_ids;

        // The C API consumes the stream on every return path, so disarm the
        // guard before calling. After this point the stream pointer is logically
        // owned by Rust and any C++-side exception must not re-release it.
        stream_guard.disarm();

        LanceDataset* out = nullptr;
        int32_t rc = lance_dataset_write_with_params(
            uri.c_str(),
            &schema,
            stream,
            static_cast<int32_t>(mode),
            &c_params,
            opts_ptr,
            &out);
        if (rc != 0) check_error();
        // Defensive null guard: a conforming Rust impl never returns rc == 0
        // with `out == nullptr`, but constructing a Dataset around a null
        // handle would silently crash on the first method call. Throw
        // explicitly rather than going through `check_error()` because the
        // thread-local code is `LANCE_OK` on this path (rc == 0).
        if (!out) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_dataset_write_with_params returned success with null out_dataset");
        }
        return Dataset(out);
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

    /// Commit a new manifest that aliases `version` as the latest. The
    /// returned Dataset points at the target version; this handle is
    /// unchanged. If `version` is already the latest, no new manifest is
    /// written. Throws lance::Error on failure.
    Dataset restore(uint64_t version) const {
        auto* out = lance_dataset_restore(handle_.get(), version);
        if (!out) check_error();
        return Dataset(out);
    }

    /// Delete rows matching the SQL `predicate`, committing a new manifest.
    /// Mutates this dataset in place; the handle continues to point at the
    /// new version. Returns the number of rows that were deleted.
    /// Throws lance::Error on failure (empty predicate, malformed SQL,
    /// commit conflict, ...).
    ///
    /// Named `delete_rows` to avoid the C++ `delete` keyword.
    uint64_t delete_rows(const std::string& predicate) {
        uint64_t num_deleted = 0;
        if (lance_dataset_delete(handle_.get(), predicate.c_str(), &num_deleted) != 0) {
            check_error();
        }
        return num_deleted;
    }

    /// Update rows matching the SQL `predicate` by applying per-column SQL
    /// expressions. Mutates this dataset in place; the handle continues to
    /// point at the new version. Returns the number of rows updated.
    ///
    /// `predicate` is empty -> updates every row (passed as NULL to the C
    /// API). `updates` must be non-empty; each pair is `{column_name,
    /// sql_expr}`. Throws lance::Error on failure (empty pair entry,
    /// malformed SQL, unknown column, commit conflict, ...).
    uint64_t update(
        const std::string& predicate,
        const std::vector<std::pair<std::string, std::string>>& updates) {
        std::vector<const char*> col_ptrs;
        std::vector<const char*> val_ptrs;
        col_ptrs.reserve(updates.size());
        val_ptrs.reserve(updates.size());
        for (const auto& [col, val] : updates) {
            col_ptrs.push_back(col.c_str());
            val_ptrs.push_back(val.c_str());
        }
        uint64_t num_updated = 0;
        const char* pred_ptr = predicate.empty() ? nullptr : predicate.c_str();
        if (lance_dataset_update(
                handle_.get(),
                pred_ptr,
                col_ptrs.data(),
                val_ptrs.data(),
                updates.size(),
                &num_updated) != 0) {
            check_error();
        }
        return num_updated;
    }

    /// Merge `source` into this dataset keyed on `on_columns`, committing a
    /// new manifest. Defaults to find-or-create semantics (insert rows that
    /// do not match an existing key). Returns the per-call insert / update /
    /// delete counts.
    ///
    /// `on_columns` must be non-empty. `params` controls match behavior; pass
    /// `nullptr` for find-or-create defaults. `source` is consumed.
    /// Throws lance::Error on failure (empty key, schema mismatch, malformed
    /// SQL, missing expression for *_IF mode, commit conflict, ...).
    LanceMergeInsertResult merge_insert(
        const std::vector<std::string>& on_columns,
        ArrowArrayStream* source,
        const LanceMergeInsertParams* params = nullptr) {
        std::vector<const char*> col_ptrs;
        col_ptrs.reserve(on_columns.size());
        for (const auto& c : on_columns) {
            col_ptrs.push_back(c.c_str());
        }
        LanceMergeInsertResult result{};
        if (lance_dataset_merge_insert(
                handle_.get(),
                col_ptrs.data(),
                on_columns.size(),
                source,
                params,
                &result) != 0) {
            check_error();
        }
        return result;
    }

    /// Convenience: classic upsert (when_matched=UpdateAll, when_not_matched=InsertAll).
    LanceMergeInsertResult upsert(
        const std::vector<std::string>& on_columns,
        ArrowArrayStream* source) {
        LanceMergeInsertParams params{};
        params.when_matched = LANCE_MERGE_WHEN_MATCHED_UPDATE_ALL;
        params.when_not_matched = LANCE_MERGE_WHEN_NOT_MATCHED_INSERT_ALL;
        params.when_not_matched_by_source = LANCE_MERGE_WHEN_NOT_MATCHED_BY_SOURCE_KEEP;
        return merge_insert(on_columns, source, &params);
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

    /// Set a Substrait filter (serialized ExtendedExpression bytes).
    /// Wins over any SQL filter passed to the Scanner constructor.
    Scanner& substrait_filter(const uint8_t* bytes, size_t len) {
        if (lance_scanner_set_substrait_filter(handle_.get(), bytes, len) != 0)
            check_error();
        return *this;
    }

    /// Set a Substrait filter (vector overload).
    Scanner& substrait_filter(const std::vector<uint8_t>& bytes) {
        return substrait_filter(bytes.data(), bytes.size());
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
