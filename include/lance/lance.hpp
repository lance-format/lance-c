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

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lance {

class Scanner;

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

/// Release and free a library-allocated ArrowArrayStream returned by
/// Scanner::scan_async. NULL-safe; do not use for caller-allocated streams.
inline void scanner_async_stream_free(ArrowArrayStream* stream) noexcept {
    lance_scanner_async_stream_free(stream);
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
class IndexModel;
class IndexSegmentBuilder;
class IndexSegmentMetadata;
class FtsQueryContext;
class FtsQuery;

// ─── Version history ─────────────────────────────────────────────────────────

/// Metadata for a single dataset version.
/// `id` mirrors the upstream Version::version (monotonic manifest version);
/// `timestamp_ms` is Unix epoch milliseconds.
struct VersionInfo {
    uint64_t id;
    int64_t  timestamp_ms;
};

/// A copied version snapshot with native-precision timestamp and metadata.
struct VersionDetails {
    uint64_t id = 0;
    int64_t timestamp_ns = 0;
    std::map<std::string, std::string> metadata;
};

/// Copied native TagContents plus its tag name. Present timestamps must fit
/// i64 nanoseconds; tag readers throw lance::Error when they do not.
struct TagInfo {
    std::string name;
    std::optional<std::string> branch;
    uint64_t version = 0;
    std::optional<int64_t> created_at_ns;
    std::optional<int64_t> updated_at_ns;
    uint64_t manifest_size = 0;
    std::map<std::string, std::string> metadata;
};

/// Owned native `lance::dataset::refs::Ref`.
class Ref {
    Handle<LanceRef, lance_ref_close> handle_;

public:
    explicit Ref(LanceRef* reference) : handle_(reference) {}

    Ref(Ref&&) noexcept = default;
    Ref& operator=(Ref&&) noexcept = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;

    static Ref version_number(uint64_t version) {
        auto* raw = lance_ref_version_number(version);
        if (!raw) check_error();
        return Ref(raw);
    }

    static Ref version(
        std::optional<std::string> branch,
        std::optional<uint64_t> version_number) {
        auto* raw = lance_ref_version(
            branch ? branch->c_str() : nullptr,
            version_number.has_value(),
            version_number.value_or(0));
        if (!raw) check_error();
        return Ref(raw);
    }

    static Ref tag(const std::string& tag_name) {
        auto* raw = lance_ref_tag(tag_name.c_str());
        if (!raw) check_error();
        return Ref(raw);
    }

    const LanceRef* c_handle() const { return handle_.get(); }
};

/// Per-field storage statistics for query planning.
/// `id` is the schema field id; `bytes_on_disk` is the compressed on-disk size
/// (0 for datasets written with the legacy v1 storage format).
struct FieldStatistics {
    uint32_t id;
    uint64_t bytes_on_disk;
};

// ─── Write mode ──────────────────────────────────────────────────────────────

enum class WriteMode : int32_t {
    Create    = LANCE_WRITE_CREATE,
    Append    = LANCE_WRITE_APPEND,
    Overwrite = LANCE_WRITE_OVERWRITE,
};

enum class MetadataUpdateMode : int32_t {
    Merge   = LANCE_METADATA_UPDATE_MERGE,
    Replace = LANCE_METADATA_UPDATE_REPLACE,
};

enum class FtsCoverageMode : int32_t {
    Strict    = LANCE_FTS_COVERAGE_STRICT,
    IndexOnly = LANCE_FTS_COVERAGE_INDEX_ONLY,
};

enum class FtsMatchOperator : int32_t {
    Or  = LANCE_FTS_MATCH_OPERATOR_OR,
    And = LANCE_FTS_MATCH_OPERATOR_AND,
};

enum class FtsDocumentGranularity : int32_t {
    Unspecified = LANCE_FTS_DOCUMENT_GRANULARITY_UNSPECIFIED,
    Row         = LANCE_FTS_DOCUMENT_GRANULARITY_ROW,
    ListElement = LANCE_FTS_DOCUMENT_GRANULARITY_LIST_ELEMENT,
};

enum class FtsOccur : int32_t {
    Should  = LANCE_FTS_OCCUR_SHOULD,
    Must    = LANCE_FTS_OCCUR_MUST,
    MustNot = LANCE_FTS_OCCUR_MUST_NOT,
};

/// Options for FtsQuery::match(). `std::nullopt` represents the core AUTO
/// setting. The Lance 14.0.2 dataset planner resolves AUTO to exact matching.
/// The default `0` selects exact matching.
struct FtsMatchOptions {
    float boost = 1.0f;
    std::optional<uint32_t> fuzziness = uint32_t{0};
    uint64_t max_expansions = 50;
    FtsMatchOperator match_operator = FtsMatchOperator::Or;
    uint32_t prefix_length = 0;
    FtsDocumentGranularity document_granularity =
        FtsDocumentGranularity::Unspecified;
};

/// Options for FtsQuery::phrase().
struct FtsPhraseOptions {
    uint32_t slop = 0;
    FtsDocumentGranularity document_granularity =
        FtsDocumentGranularity::Unspecified;
};

/// Options for FtsQuery::multi_match(). `std::nullopt` uses the native default
/// of 1.0 per column; a provided vector must contain one entry per column.
struct FtsMultiMatchOptions {
    std::optional<std::vector<float>> boosts;
    FtsMatchOperator match_operator = FtsMatchOperator::Or;
};

/// Options for FtsQuery::combined_fields(). `std::nullopt` uses the native
/// default of 1.0 per column; a provided vector must contain one entry per
/// column.
struct FtsCombinedFieldsOptions {
    std::optional<std::vector<float>> boosts;
    FtsMatchOperator match_operator = FtsMatchOperator::Or;
};

/// Options for FtsQuery::boost().
struct FtsBoostOptions {
    float negative_boost = 0.5f;
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

/// Native auto-cleanup settings for `WriteOptions`.
///
/// `interval` is the number of committed versions between cleanup runs; 0 is
/// valid and considers cleanup on every commit.
///
/// `older_than_seconds` and `older_than_nanos` express the native
/// `chrono::TimeDelta` retention window as
/// `older_than_seconds + older_than_nanos / 1000000000`. Both fields carry the
/// sign; `older_than_nanos` must be in [-999999999, 999999999] and the combined
/// value must fit in the native `TimeDelta` range (about +/-292 million years).
/// Negative durations are rejected by Lance when the write transaction is
/// built.
struct AutoCleanupParams {
    uint64_t interval = 0;
    int64_t  older_than_seconds = 0;
    int32_t  older_than_nanos = 0;
};

/// Native write settings for `Dataset::write_with_options`, which selects
/// Rust's `WriteParams::default()`.
///
/// A default-constructed `WriteOptions` sets `auto_cleanup = std::nullopt`
/// (native `None`) and `skip_auto_cleanup = false`. `params` retains the
/// zero-means-upstream-default mapping of `WriteParams`.
struct WriteOptions {
    WriteParams                      params;
    std::optional<AutoCleanupParams> auto_cleanup;
    bool                             skip_auto_cleanup = false;
};

/// One native `Dataset::update_config` entry. `value == std::nullopt` removes
/// the key; otherwise the value is upserted.
using ConfigUpdate = std::pair<std::string, std::optional<std::string>>;

// ─── Column alteration ───────────────────────────────────────────────────────

/// A single alteration applied to one column by `Dataset::alter_columns`.
/// Every non-`path` field is optional; at least one must request a change.
///
/// `data_type`, when non-null, borrows an Arrow C Data Interface `ArrowSchema`
/// describing the target type. The caller owns it and must keep it alive for
/// the duration of the `alter_columns` call; the wrapper does not release it.
struct ColumnAlteration {
    std::string                path;
    std::optional<std::string> rename;
    LanceColumnNullableMode    nullable_mode = LANCE_COLUMN_NULLABLE_UNCHANGED;
    const ArrowSchema*         data_type     = nullptr;
};

// ─── New column (SQL) ────────────────────────────────────────────────────────

/// A single new column defined by a SQL expression over the dataset's existing
/// columns, added by `Dataset::add_columns_sql`. Both fields are required and
/// non-empty, e.g. `{ "doubled", "x * 2" }`.
struct SqlColumn {
    std::string name;
    std::string expression;
};

// ─── Shared Session ──────────────────────────────────────────────────────────

enum class SessionCacheSpecKind : int32_t {
    Default = LANCE_SESSION_CACHE_SPEC_DEFAULT,
    Size    = LANCE_SESSION_CACHE_SPEC_SIZE,
    Uri     = LANCE_SESSION_CACHE_SPEC_URI,
    Config  = LANCE_SESSION_CACHE_SPEC_CONFIG,
};

/// Cache selection for one session cache tier. `default_cache()` uses the
/// native tier default; `with_size(0)` requests an explicit zero capacity.
struct SessionCacheSpec {
    SessionCacheSpecKind kind = SessionCacheSpecKind::Default;
    uint64_t size_bytes = 0;
    std::string backend_uri;
    std::string backend_kind;
    std::vector<std::pair<std::string, std::string>> backend_options;

    static SessionCacheSpec default_cache() { return {}; }

    static SessionCacheSpec with_size(uint64_t capacity_bytes) {
        SessionCacheSpec spec;
        spec.kind = SessionCacheSpecKind::Size;
        spec.size_bytes = capacity_bytes;
        return spec;
    }

    static SessionCacheSpec from_uri(std::string uri) {
        SessionCacheSpec spec;
        spec.kind = SessionCacheSpecKind::Uri;
        spec.backend_uri = std::move(uri);
        return spec;
    }

    static SessionCacheSpec from_config(
        std::string backend_kind,
        std::vector<std::pair<std::string, std::string>> backend_options = {}) {
        SessionCacheSpec spec;
        spec.kind = SessionCacheSpecKind::Config;
        spec.backend_kind = std::move(backend_kind);
        spec.backend_options = std::move(backend_options);
        return spec;
    }
};

class Session {
    Handle<LanceSession, lance_session_close> handle_;

    static LanceSessionCacheSpec c_cache_spec(
        const SessionCacheSpec& spec,
        std::vector<const char*>& option_keys,
        std::vector<const char*>& option_values) {
        LanceSessionCacheSpec c_spec{};
        c_spec.kind = static_cast<int32_t>(spec.kind);
        switch (spec.kind) {
            case SessionCacheSpecKind::Default:
                break;
            case SessionCacheSpecKind::Size:
                c_spec.size_bytes = spec.size_bytes;
                break;
            case SessionCacheSpecKind::Uri:
                c_spec.backend_uri = spec.backend_uri.c_str();
                break;
            case SessionCacheSpecKind::Config:
                c_spec.backend_kind = spec.backend_kind.c_str();
                option_keys.reserve(spec.backend_options.size());
                option_values.reserve(spec.backend_options.size());
                for (const auto& [key, value] : spec.backend_options) {
                    option_keys.push_back(key.c_str());
                    option_values.push_back(value.c_str());
                }
                if (!option_keys.empty()) {
                    c_spec.option_keys = option_keys.data();
                    c_spec.option_values = option_values.data();
                    c_spec.option_count = option_keys.size();
                }
                break;
        }
        return c_spec;
    }

    static LanceSession* create(
        const SessionCacheSpec& index_cache,
        const SessionCacheSpec& metadata_cache) {
        std::vector<const char*> index_keys;
        std::vector<const char*> index_values;
        std::vector<const char*> metadata_keys;
        std::vector<const char*> metadata_values;
        auto c_index =
            c_cache_spec(index_cache, index_keys, index_values);
        auto c_metadata =
            c_cache_spec(metadata_cache, metadata_keys, metadata_values);
        return lance_session_new_with_cache_specs(&c_index, &c_metadata);
    }

public:
    /// Configure each cache tier independently. The spec values and their
    /// string buffers are consumed during construction.
    Session(
        const SessionCacheSpec& index_cache,
        const SessionCacheSpec& metadata_cache)
        : handle_(create(index_cache, metadata_cache)) {
        if (!handle_) check_error();
    }

    Session(uint64_t index_cache_size_bytes, uint64_t metadata_cache_size_bytes)
        : Session(
              SessionCacheSpec::with_size(index_cache_size_bytes),
              SessionCacheSpec::with_size(metadata_cache_size_bytes)) {}

    LanceSessionCacheStats cache_stats() const {
        LanceSessionCacheStats stats{};
        if (lance_session_get_cache_stats(handle_.get(), &stats) != 0)
            check_error();
        return stats;
    }

    /// Current deep size of the session in bytes. This is not an RSS limit.
    uint64_t size_bytes() const {
        uint64_t size = 0;
        if (lance_session_get_size_bytes(handle_.get(), &size) != 0)
            check_error();
        return size;
    }

    /// Approximate number of items retained in both cache tiers.
    uint64_t approx_num_items() const {
        uint64_t count = 0;
        if (lance_session_get_approx_num_items(handle_.get(), &count) != 0)
            check_error();
        return count;
    }

    const LanceSession* c_handle() const { return handle_.get(); }
};

// ─── Process-local FTS query context ────────────────────────────────────────

/// Immutable, query-specific global BM25 scorer plus pinned FTS segment list.
/// This handle is process-local and intentionally has no serialization API.
class FtsQueryContext {
    Handle<LanceFtsQueryContext, lance_fts_query_context_close> handle_;

public:
    explicit FtsQueryContext(LanceFtsQueryContext* context) : handle_(context) {}

    FtsQueryContext(FtsQueryContext&&) noexcept = default;
    FtsQueryContext& operator=(FtsQueryContext&&) noexcept = default;
    FtsQueryContext(const FtsQueryContext&) = delete;
    FtsQueryContext& operator=(const FtsQueryContext&) = delete;

    const LanceFtsQueryContext* c_handle() const { return handle_.get(); }
};

// ─── Typed full-text query ───────────────────────────────────────────────────

/// Immutable full-text query value. Composite factories copy their child
/// values, and Scanner::full_text_search snapshots the tree, so handles may be
/// destroyed before the scan runs.
class FtsQuery {
    Handle<LanceFtsQuery, lance_fts_query_close> handle_;

    explicit FtsQuery(LanceFtsQuery* query) : handle_(query) {}

    static void copy_boosts(
        const std::optional<std::vector<float>>& boosts,
        float& empty_boost,
        const float*& data,
        size_t& count) {
        data = nullptr;
        count = 0;
        if (!boosts) return;
        empty_boost = 0.0f;
        data = boosts->empty() ? &empty_boost : boosts->data();
        count = boosts->size();
    }

public:
    FtsQuery(FtsQuery&&) noexcept = default;
    FtsQuery& operator=(FtsQuery&&) noexcept = default;
    FtsQuery(const FtsQuery&) = delete;
    FtsQuery& operator=(const FtsQuery&) = delete;

    /// Match query. A null `column` lets scan planning resolve the indexed
    /// columns.
    static FtsQuery match(
        const std::string& terms,
        std::optional<std::string> column = std::nullopt,
        const FtsMatchOptions& options = {}) {
        LanceFtsMatchOptions c_options{};
        lance_fts_query_match_options_init(&c_options);
        c_options.boost = options.boost;
        c_options.fuzziness = options.fuzziness
                                  ? static_cast<int64_t>(*options.fuzziness)
                                  : -1;
        c_options.max_expansions = options.max_expansions;
        c_options.match_operator = static_cast<int32_t>(options.match_operator);
        c_options.prefix_length = options.prefix_length;
        c_options.document_granularity =
            static_cast<int32_t>(options.document_granularity);

        auto* query = lance_fts_query_match_new(
            terms.c_str(), column ? column->c_str() : nullptr, &c_options);
        if (!query) check_error();
        return FtsQuery(query);
    }

    /// Phrase query. A null `column` lets scan planning resolve the indexed
    /// columns.
    static FtsQuery phrase(
        const std::string& terms,
        std::optional<std::string> column = std::nullopt,
        const FtsPhraseOptions& options = {}) {
        LanceFtsPhraseOptions c_options{};
        lance_fts_query_phrase_options_init(&c_options);
        c_options.slop = options.slop;
        c_options.document_granularity =
            static_cast<int32_t>(options.document_granularity);

        auto* query = lance_fts_query_phrase_new(
            terms.c_str(), column ? column->c_str() : nullptr, &c_options);
        if (!query) check_error();
        return FtsQuery(query);
    }

    /// Boolean query. An empty clause list is allowed here; scan planning
    /// rejects a tree without a positive clause.
    static FtsQuery boolean(
        const std::vector<std::pair<FtsOccur, const FtsQuery*>>& clauses) {
        std::vector<int32_t> occurs;
        std::vector<const LanceFtsQuery*> queries;
        occurs.reserve(clauses.size());
        queries.reserve(clauses.size());
        for (const auto& [occur, query] : clauses) {
            occurs.push_back(static_cast<int32_t>(occur));
            queries.push_back(query ? query->c_handle() : nullptr);
        }

        auto* result = lance_fts_query_boolean_new(
            occurs.data(), queries.data(), clauses.size());
        if (!result) check_error();
        return FtsQuery(result);
    }

    /// Boost (boosting) query: the negative child's score is multiplied by
    /// negative_boost and subtracted from the positive child's score.
    static FtsQuery boost(
        const FtsQuery& positive,
        const FtsQuery& negative,
        const FtsBoostOptions& options = {}) {
        LanceFtsBoostOptions c_options{};
        lance_fts_query_boost_options_init(&c_options);
        c_options.negative_boost = options.negative_boost;

        auto* query = lance_fts_query_boost_new(
            positive.c_handle(), negative.c_handle(), &c_options);
        if (!query) check_error();
        return FtsQuery(query);
    }

    /// MultiMatch query: scores each field independently and keeps the maximum
    /// field score.
    static FtsQuery multi_match(
        const std::string& terms,
        const std::vector<std::string>& columns,
        const FtsMultiMatchOptions& options = {}) {
        std::vector<const char*> column_ptrs;
        column_ptrs.reserve(columns.size());
        for (const auto& column : columns) column_ptrs.push_back(column.c_str());

        LanceFtsMultiMatchOptions c_options{};
        lance_fts_query_multi_match_options_init(&c_options);
        float empty_boost = 0.0f;
        copy_boosts(options.boosts, empty_boost, c_options.boosts,
                    c_options.boost_count);
        c_options.match_operator = static_cast<int32_t>(options.match_operator);

        auto* query = lance_fts_query_multi_match_new(
            terms.c_str(), column_ptrs.data(), column_ptrs.size(), &c_options);
        if (!query) check_error();
        return FtsQuery(query);
    }

    /// CombinedFields (BM25F) query: blends term statistics across all target
    /// columns.
    static FtsQuery combined_fields(
        const std::string& terms,
        const std::vector<std::string>& columns,
        const FtsCombinedFieldsOptions& options = {}) {
        std::vector<const char*> column_ptrs;
        column_ptrs.reserve(columns.size());
        for (const auto& column : columns) column_ptrs.push_back(column.c_str());

        LanceFtsCombinedFieldsOptions c_options{};
        lance_fts_query_combined_fields_options_init(&c_options);
        float empty_boost = 0.0f;
        copy_boosts(options.boosts, empty_boost, c_options.boosts,
                    c_options.boost_count);
        c_options.match_operator = static_cast<int32_t>(options.match_operator);

        auto* query = lance_fts_query_combined_fields_new(
            terms.c_str(), column_ptrs.data(), column_ptrs.size(), &c_options);
        if (!query) check_error();
        return FtsQuery(query);
    }

    const LanceFtsQuery* c_handle() const { return handle_.get(); }
};

// ─── Blob file ───────────────────────────────────────────────────────────────

/// RAII handle over one value of a Blob v2 column, from `Dataset::take_blobs()`
/// or `take_blobs_by_indices()`. Stays usable after the Dataset is destroyed.
/// `read()` and `read_up_to()` advance the cursor, `read_range()` does not.
/// Not thread-safe per handle.
class BlobFile {
    Handle<LanceBlobFile, lance_blob_file_close> handle_;

public:
    /// Adopt a handle from the C API; closed on destruction.
    explicit BlobFile(LanceBlobFile* blob) : handle_(blob) {}

    /// Size of the blob in bytes. Independent of the cursor.
    uint64_t size() const {
        uint64_t n = lance_blob_file_size(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return n;
    }

    /// Read from the cursor to the end (the whole blob when the cursor is 0).
    std::vector<uint8_t> read() {
        uint64_t blob_size = size();
        uint64_t cursor = tell();
        uint64_t remaining = cursor >= blob_size ? 0 : blob_size - cursor;
        std::vector<uint8_t> out(static_cast<size_t>(remaining));
        if (lance_blob_file_read(handle_.get(), out.data(), out.size()) != 0)
            check_error();
        return out;
    }

    /// Read at most `len` bytes from the current cursor, advancing it past
    /// them. The result is shorter than `len` at the end of the blob.
    std::vector<uint8_t> read_up_to(size_t len) {
        std::vector<uint8_t> out(len);
        size_t bytes_read = 0;
        if (lance_blob_file_read_up_to(
                handle_.get(), out.data(), len, &bytes_read) != 0)
            check_error();
        out.resize(bytes_read);
        return out;
    }

    /// Read exactly `len` bytes at `offset` without moving the cursor. The
    /// range must lie within the blob.
    std::vector<uint8_t> read_range(uint64_t offset, size_t len) const {
        std::vector<uint8_t> out(len);
        if (lance_blob_file_read_range(
                handle_.get(), offset, out.data(), len) != 0)
            check_error();
        return out;
    }

    /// Move the cursor. Seeking past the end is allowed; reads then return
    /// no bytes.
    void seek(uint64_t pos) {
        if (lance_blob_file_seek(handle_.get(), pos) != 0) check_error();
    }

    /// Current cursor position, in bytes from the start of the blob.
    uint64_t tell() const {
        uint64_t pos = 0;
        if (lance_blob_file_tell(handle_.get(), &pos) != 0) check_error();
        return pos;
    }
};

// ─── Read-only fragment snapshot ─────────────────────────────────────────────

/// RAII handle over one fragment from a dataset snapshot. Native
/// `FileFragment` owns an `Arc<Dataset>`, so this handle remains valid and
/// keeps reading the same dataset version after the source Dataset is closed
/// or mutated. Methods expose only read-only metadata and row counts.
class Fragment {
    Handle<LanceFragment, lance_fragment_close> handle_;

public:
    /// Adopt a handle from the C API; closed on destruction.
    explicit Fragment(LanceFragment* fragment) : handle_(fragment) {}

    Fragment(Fragment&&) noexcept = default;
    Fragment& operator=(Fragment&&) noexcept = default;
    Fragment(const Fragment&) = delete;
    Fragment& operator=(const Fragment&) = delete;

    /// Native fragment ID.
    uint64_t id() const {
        uint64_t value = lance_fragment_id(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    /// Number of Native base data files; excludes overlays and the deletion file.
    uint64_t data_file_count() const {
        uint64_t value = lance_fragment_data_file_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    /// Native `Fragment` metadata serialized as JSON. Includes base data
    /// files, overlays, deletion-file metadata, row-id/version metadata, and
    /// the optional physical_rows value.
    std::string metadata_json() const {
        const char* raw = lance_fragment_metadata_json(handle_.get());
        if (!raw) check_error();
        std::unique_ptr<const char, void (*)(const char*)> guard(
            raw, lance_free_string);
        return std::string(raw);
    }

    /// Export the logical dataset schema through Arrow C Data Interface.
    /// `out` is caller-owned; release it exactly once.
    void schema(ArrowSchema* out) const {
        if (lance_fragment_schema(handle_.get(), out) != 0) check_error();
    }

    /// Physical row count, including deleted rows. May perform data-file I/O.
    uint64_t physical_rows() const {
        uint64_t value = 0;
        if (lance_fragment_physical_rows(handle_.get(), &value) != 0)
            check_error();
        return value;
    }

    /// Deleted-row count. May load the deletion vector.
    uint64_t deleted_rows() const {
        uint64_t value = 0;
        if (lance_fragment_deleted_rows(handle_.get(), &value) != 0)
            check_error();
        return value;
    }

    /// Logical row count from Native count_rows(None).
    uint64_t logical_rows() const {
        uint64_t value = 0;
        if (lance_fragment_logical_rows(handle_.get(), &value) != 0)
            check_error();
        return value;
    }

    /// Start a scanner over this fragment's owned snapshot.
    Scanner scan() const;

    /// Take fragment-local logical rows by index with an explicit projection.
    /// `out` is caller-owned and its non-null `release` must be called exactly
    /// once. An empty `columns` vector is an explicit empty projection.
    void take(const uint32_t* indices, size_t num_indices,
              const std::vector<std::string>& columns,
              ArrowArrayStream* out) const;

    /// Take fragment-local logical rows with the full logical schema.
    void take(const uint32_t* indices, size_t num_indices,
              ArrowArrayStream* out) const;

    /// Count fragment rows. `std::nullopt` calls native count_rows(None); an
    /// empty string is passed as an explicit native filter.
    uint64_t count_rows(std::optional<std::string> filter = std::nullopt) const {
        uint64_t value = 0;
        if (lance_fragment_count_rows(
                handle_.get(), filter ? filter->c_str() : nullptr, &value) != 0)
            check_error();
        return value;
    }

    const LanceFragment* c_handle() const { return handle_.get(); }
};

// ─── Dataset ─────────────────────────────────────────────────────────────────

class Dataset {
    Handle<LanceDataset, lance_dataset_close> handle_;

    static std::vector<const char*> to_c_key_values(
        const std::vector<std::pair<std::string, std::string>>& entries) {
        std::vector<const char*> kv;
        for (auto& [k, v] : entries) {
            kv.push_back(k.c_str());
            kv.push_back(v.c_str());
        }
        kv.push_back(nullptr);
        return kv;
    }

    static VersionDetails read_version(const LanceVersion* version) {
        VersionDetails out;
        out.id = lance_version_id(version);
        if (lance_last_error_code() != LANCE_OK) check_error();
        out.timestamp_ns = lance_version_timestamp_ns(version);
        if (lance_last_error_code() != LANCE_OK) check_error();

        uint64_t count = lance_version_metadata_count(version);
        if (lance_last_error_code() != LANCE_OK) check_error();
        for (uint64_t i = 0; i < count; ++i) {
            const char* key = lance_version_metadata_key_at(
                version, static_cast<size_t>(i));
            if (!key) check_error();
            const char* value = lance_version_metadata_value_at(
                version, static_cast<size_t>(i));
            if (!value) check_error();
            out.metadata.emplace(key, value);
        }
        return out;
    }

    static TagInfo read_tag(const LanceTag* tag) {
        TagInfo out;
        const char* name = lance_tag_name(tag);
        if (!name) check_error();
        out.name = name;

        out.version = lance_tag_version(tag);
        if (lance_last_error_code() != LANCE_OK) check_error();
        out.manifest_size = lance_tag_manifest_size(tag);
        if (lance_last_error_code() != LANCE_OK) check_error();

        const char* branch = nullptr;
        int32_t has_branch = lance_tag_branch(tag, &branch);
        if (has_branch < 0) check_error();
        if (has_branch) {
            if (!branch) {
                throw Error(LANCE_ERR_INTERNAL,
                            "lance_tag_branch reported a present branch with a null pointer");
            }
            out.branch = std::string(branch);
        }

        int64_t timestamp = 0;
        int32_t has_created = lance_tag_created_at_ns(tag, &timestamp);
        if (has_created < 0) check_error();
        if (has_created) out.created_at_ns = timestamp;

        timestamp = 0;
        int32_t has_updated = lance_tag_updated_at_ns(tag, &timestamp);
        if (has_updated < 0) check_error();
        if (has_updated) out.updated_at_ns = timestamp;

        uint64_t metadata_count = lance_tag_metadata_count(tag);
        if (lance_last_error_code() != LANCE_OK) check_error();
        for (uint64_t i = 0; i < metadata_count; ++i) {
            const char* key = lance_tag_metadata_key_at(
                tag, static_cast<size_t>(i));
            if (!key) check_error();
            const char* value = lance_tag_metadata_value_at(
                tag, static_cast<size_t>(i));
            if (!value) check_error();
            out.metadata.emplace(key, value);
        }
        return out;
    }

    /// Move raw handles into RAII owners. `blobs` must be reserved up front so
    /// nothing can throw while handles are still unowned.
    ///
    /// Leak-freedom also rests on the C side: `lance_dataset_take_blobs*` fill
    /// `out` all-or-nothing and leave it untouched on error. A partial fill
    /// before an error would leak, because `check_error()` throws before this
    /// runs and `raw` owns nothing.
    static void adopt_blobs(const std::vector<LanceBlobFile*>& raw,
                            std::vector<std::optional<BlobFile>>& blobs) {
        for (auto* blob : raw) {
            if (blob) {
                blobs.emplace_back(BlobFile(blob));
            } else {
                blobs.emplace_back(std::nullopt);
            }
        }
    }

    static std::string run_metadata_update_json(
        LanceDataset* dataset,
        const std::vector<std::pair<std::string, std::optional<std::string>>>& values,
        MetadataUpdateMode mode,
        bool schema_metadata) {

        std::vector<const char*> keys;
        std::vector<const char*> raw_values;
        keys.reserve(values.size());
        raw_values.reserve(values.size());
        for (const auto& [key, value] : values) {
            keys.push_back(key.c_str());
            raw_values.push_back(value ? value->c_str() : nullptr);
        }

        const char* json = schema_metadata
            ? lance_dataset_update_schema_metadata_json(
                  dataset, keys.data(), raw_values.data(), values.size(),
                  static_cast<LanceMetadataUpdateMode>(mode))
            : lance_dataset_update_metadata_json(
                  dataset, keys.data(), raw_values.data(), values.size(),
                  static_cast<LanceMetadataUpdateMode>(mode));
        if (!json) check_error();
        std::string out(json);
        lance_free_string(json);
        return out;
    }

    /// Shared write path. `native_options` selects the native-defaults entry
    /// point; otherwise `legacy_params` selects the legacy 20/14 entry point.
    static Dataset write_impl(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const LanceWriteParams* legacy_params,
        const LanceWriteOptions* native_options,
        const std::vector<std::pair<std::string, std::string>>& storage_opts) {

        if (stream == nullptr) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT, "stream must not be null");
        }

        // RAII guard for the stream. Until the selected write entry point is
        // called, any exception (failed `get_schema`, `std::bad_alloc` while
        // building `kv`, etc.) must release the stream. After that call Rust
        // owns it, so we `disarm()` immediately before invoking the C API.
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

        // The C API consumes the stream on every return path, so disarm the
        // guard before calling. After this point the stream pointer is logically
        // owned by Rust and any C++-side exception must not re-release it.
        stream_guard.disarm();

        LanceDataset* out = nullptr;
        int32_t rc;
        const char* entry_point;
        if (native_options) {
            entry_point = "lance_dataset_write_with_options";
            rc = lance_dataset_write_with_options(
                uri.c_str(),
                &schema,
                stream,
                static_cast<int32_t>(mode),
                native_options,
                opts_ptr,
                &out);
        } else {
            entry_point = "lance_dataset_write_with_params";
            rc = lance_dataset_write_with_params(
                uri.c_str(),
                &schema,
                stream,
                static_cast<int32_t>(mode),
                legacy_params,
                opts_ptr,
                &out);
        }
        if (rc != 0) check_error();
        // Defensive null guard: a conforming Rust impl never returns rc == 0
        // with `out == nullptr`, but constructing a Dataset around a null
        // handle would silently crash on the first method call. Throw
        // explicitly rather than going through `check_error()` because the
        // thread-local code is `LANCE_OK` on this path (rc == 0).
        if (!out) {
            throw Error(LANCE_ERR_INTERNAL,
                        std::string(entry_point) +
                            " returned success with null out_dataset");
        }
        return Dataset(out);
    }

public:
    /// Open a dataset at the given URI. Pass `version` = 0 (the default) for
    /// the latest, or a specific version id from `versions()` to check out
    /// that version, e.g. `lance::Dataset::open("data.lance", {}, /*version=*/42)`.
    static Dataset open(
        const std::string& uri,
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {},
        uint64_t version = 0) {

        auto kv = to_c_key_values(storage_opts);
        const char* const* opts_ptr =
            storage_opts.empty() ? nullptr : kv.data();

        auto* ds = lance_dataset_open(uri.c_str(), opts_ptr, version);
        if (!ds) check_error();
        return Dataset(ds);
    }

    /// Open a dataset with caches owned by `session`. The returned dataset
    /// remains valid if the Session wrapper is destroyed first.
    static Dataset open_with_session(
        const Session& session,
        const std::string& uri,
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {},
        uint64_t version = 0) {

        auto kv = to_c_key_values(storage_opts);
        const char* const* opts_ptr =
            storage_opts.empty() ? nullptr : kv.data();

        auto* ds = lance_dataset_open_with_session(
            uri.c_str(), opts_ptr, version, session.c_handle());
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
    ///
    /// This overload preserves the legacy lance-c 20-version / 14-day
    /// auto-cleanup policy. Use `write_with_options` for native
    /// `WriteParams::default()` semantics.
    static Dataset write(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const WriteParams& params,
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {}) {

        LanceWriteParams c_params = {};
        c_params.max_rows_per_file    = params.max_rows_per_file;
        c_params.max_rows_per_group   = params.max_rows_per_group;
        c_params.max_bytes_per_file   = params.max_bytes_per_file;
        c_params.data_storage_version =
            params.data_storage_version ? params.data_storage_version->c_str() : nullptr;
        c_params.enable_stable_row_ids = params.enable_stable_row_ids;

        return write_impl(
            uri, stream, mode, &c_params, nullptr, storage_opts);
    }

    /// Write using native `WriteParams::default()` semantics.
    ///
    /// A default-constructed `WriteOptions` sets `auto_cleanup =
    /// std::nullopt` (native `None`) and `skip_auto_cleanup = false`.
    /// `auto_cleanup` is persisted only by CREATE; APPEND and OVERWRITE leave
    /// an existing dataset's policy unchanged. `skip_auto_cleanup` affects only
    /// the current commit. Throws `lance::Error` on failure.
    static Dataset write_with_options(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const WriteOptions& options,
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {}) {

        LanceWriteParams c_params = {};
        c_params.max_rows_per_file    = options.params.max_rows_per_file;
        c_params.max_rows_per_group   = options.params.max_rows_per_group;
        c_params.max_bytes_per_file   = options.params.max_bytes_per_file;
        c_params.data_storage_version =
            options.params.data_storage_version
                ? options.params.data_storage_version->c_str()
                : nullptr;
        c_params.enable_stable_row_ids = options.params.enable_stable_row_ids;

        LanceAutoCleanupParams c_cleanup = {};
        LanceWriteOptions c_options = {};
        c_options.params = c_params;
        c_options.skip_auto_cleanup = options.skip_auto_cleanup;
        if (options.auto_cleanup) {
            c_cleanup.interval = options.auto_cleanup->interval;
            c_cleanup.older_than_seconds =
                options.auto_cleanup->older_than_seconds;
            c_cleanup.older_than_nanos =
                options.auto_cleanup->older_than_nanos;
            c_options.auto_cleanup = &c_cleanup;
        }

        return write_impl(
            uri, stream, mode, nullptr, &c_options, storage_opts);
    }

    /// Number of rows in the dataset.
    uint64_t count_rows() const {
        uint64_t n = lance_dataset_count_rows(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return n;
    }

    /// Version of this dataset snapshot.
    uint64_t version() const {
        uint64_t v = lance_dataset_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return v;
    }

    /// Latest version ID (queries object store).
    uint64_t latest_version() const {
        uint64_t v = lance_dataset_latest_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return v;
    }

    /// Table metadata for this snapshot as a JSON object.
    /// Schema metadata is available through `schema()` instead.
    /// Throws lance::Error on failure.
    std::string metadata_json() const {
        const char* json = lance_dataset_metadata_json(handle_.get());
        if (!json) check_error();
        std::string out(json);
        lance_free_string(json);
        return out;
    }

    /// Merge or replace table metadata and return the complete updated map as
    /// a JSON object. A `std::nullopt` value removes its key; an empty string
    /// stores a real empty value. `MetadataUpdateMode::Replace` with an empty
    /// vector clears the map. Mutates this dataset in place; existing
    /// snapshots keep their prior view. Throws lance::Error on failure.
    std::string update_metadata_json(
        const std::vector<std::pair<std::string, std::optional<std::string>>>& values,
        MetadataUpdateMode mode = MetadataUpdateMode::Merge) {
        return run_metadata_update_json(
            handle_.get(), values, mode, /*schema_metadata=*/false);
    }

    /// Merge or replace schema metadata and return the complete updated map as
    /// a JSON object. Input semantics match `update_metadata_json`. Read the
    /// committed schema metadata through `schema()`. Throws lance::Error on
    /// failure.
    std::string update_schema_metadata_json(
        const std::vector<std::pair<std::string, std::optional<std::string>>>& values,
        MetadataUpdateMode mode = MetadataUpdateMode::Merge) {
        return run_metadata_update_json(
            handle_.get(), values, mode, /*schema_metadata=*/true);
    }

    /// Snapshot the dataset's version history, ordered by version id.
    /// Throws lance::Error on failure.
    std::vector<VersionInfo> versions() const {
        auto* raw = lance_dataset_versions(handle_.get());
        if (!raw) check_error();
        Handle<LanceVersions, lance_versions_close> snap(raw);

        uint64_t n = lance_versions_count(snap.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        std::vector<VersionInfo> out;
        out.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n; i++) {
            VersionInfo info;
            info.id = lance_versions_id_at(snap.get(), static_cast<size_t>(i));
            if (lance_last_error_code() != LANCE_OK) check_error();
            info.timestamp_ms =
                lance_versions_timestamp_ms_at(snap.get(), static_cast<size_t>(i));
            if (lance_last_error_code() != LANCE_OK) check_error();
            out.push_back(info);
        }
        return out;
    }

    /// Snapshot the dataset's manifest config map.
    ///
    /// The returned map owns copies of all keys and values. The snapshot is
    /// independent of this handle. Entry order is unspecified. A config entry
    /// containing an embedded NUL byte cannot be represented as a C string and
    /// throws with LANCE_ERR_INTERNAL. Throws lance::Error on failure.
    std::map<std::string, std::string> config() const {
        auto* raw = lance_dataset_config(handle_.get());
        if (!raw) check_error();
        Handle<LanceDatasetConfig, lance_dataset_config_close> snap(raw);

        uint64_t n = lance_dataset_config_count(snap.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        std::map<std::string, std::string> out;
        for (uint64_t i = 0; i < n; i++) {
            const char* key =
                lance_dataset_config_key_at(snap.get(), static_cast<size_t>(i));
            if (!key) check_error();
            const char* value =
                lance_dataset_config_value_at(snap.get(), static_cast<size_t>(i));
            if (!value) check_error();
            out.emplace(key, value);
        }
        return out;
    }

    /// Apply native merge-style config updates, committing a new manifest.
    ///
    /// `value == std::nullopt` removes the key; any string value (including an
    /// empty one) upserts it. The handle points at the new version after a
    /// successful call. Throws lance::Error on failure (NULL dataset, invalid
    /// C strings, commit conflict, ...).
    void update_config(const std::vector<ConfigUpdate>& updates) {
        std::vector<const char*> keys;
        std::vector<const char*> values;
        keys.reserve(updates.size());
        values.reserve(updates.size());
        for (const auto& [key, value] : updates) {
            keys.push_back(key.c_str());
            values.push_back(value ? value->c_str() : nullptr);
        }
        const char* const* keys_ptr = updates.empty() ? nullptr : keys.data();
        const char* const* values_ptr =
            updates.empty() ? nullptr : values.data();
        if (lance_dataset_update_config(
                handle_.get(), keys_ptr, values_ptr, updates.size()) != 0) {
            check_error();
        }
    }

    /// Current version details, including native nanosecond timestamp and
    /// manifest-summary metadata. Timestamps must fit i64 nanoseconds.
    /// Throws lance::Error on failure.
    VersionDetails current_version() const {
        auto* raw = lance_dataset_current_version(handle_.get());
        if (!raw) check_error();
        Handle<LanceVersion, lance_version_close> version(raw);
        return read_version(version.get());
    }

    /// Full version-history details ordered by version id. Each entry contains
    /// native nanosecond precision and manifest-summary metadata. Timestamps
    /// must fit i64 nanoseconds; versions() retains millisecond precision.
    std::vector<VersionDetails> version_details() const {
        auto* raw = lance_dataset_versions(handle_.get());
        if (!raw) check_error();
        Handle<LanceVersions, lance_versions_close> snap(raw);

        uint64_t count = lance_versions_count(snap.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        std::vector<VersionDetails> out;
        out.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            const LanceVersion* version =
                lance_versions_at(snap.get(), static_cast<size_t>(i));
            if (!version) check_error();
            out.push_back(read_version(version));
        }
        return out;
    }

    /// Lightweight attached-version refs ordered by version id.
    std::vector<uint64_t> version_refs() const {
        auto* raw = lance_dataset_version_refs(handle_.get());
        if (!raw) check_error();
        Handle<LanceVersionRefs, lance_version_refs_close> refs(raw);

        uint64_t count = lance_version_refs_count(refs.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        std::vector<uint64_t> out;
        out.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t id =
                lance_version_refs_id_at(refs.get(), static_cast<size_t>(i));
            if (lance_last_error_code() != LANCE_OK) check_error();
            out.push_back(id);
        }
        return out;
    }

    /// Check out a native ref as an independent dataset snapshot.
    Dataset checkout(const Ref& reference) const {
        auto* out = lance_dataset_checkout_ref(
            handle_.get(), reference.c_handle());
        if (!out) check_error();
        return Dataset(out);
    }

    /// Snapshot all tags. Entry order is unspecified.
    std::vector<TagInfo> tags() const {
        auto* raw = lance_dataset_tags(handle_.get());
        if (!raw) check_error();
        Handle<LanceTags, lance_tags_close> tags(raw);

        uint64_t count = lance_tags_count(tags.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        std::vector<TagInfo> out;
        out.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            const LanceTag* tag = lance_tags_at(tags.get(), static_cast<size_t>(i));
            if (!tag) check_error();
            out.push_back(read_tag(tag));
        }
        return out;
    }

    /// Fetch one tag by name.
    TagInfo get_tag(const std::string& name) const {
        auto* raw = lance_dataset_tag_get(handle_.get(), name.c_str());
        if (!raw) check_error();
        Handle<LanceTag, lance_tag_close> tag(raw);
        return read_tag(tag.get());
    }

    /// Create a tag from a native ref.
    void create_tag(const std::string& name, const Ref& reference) const {
        if (lance_dataset_tag_create(
                handle_.get(), name.c_str(), reference.c_handle()) != 0)
            check_error();
    }

    /// Update an existing tag to a native ref. Metadata is preserved natively.
    void update_tag(const std::string& name, const Ref& reference) const {
        if (lance_dataset_tag_update(
                handle_.get(), name.c_str(), reference.c_handle()) != 0)
            check_error();
    }

    /// Delete a tag.
    void delete_tag(const std::string& name) const {
        if (lance_dataset_tag_delete(handle_.get(), name.c_str()) != 0)
            check_error();
    }

    /// Replace all metadata entries on a tag. An empty map writes empty
    /// metadata; native reads treat absent and empty metadata the same.
    void replace_tag_metadata(
        const std::string& name,
        const std::vector<std::pair<std::string, std::string>>& metadata) const {
        auto kv = to_c_key_values(metadata);
        const char* const* metadata_ptr = metadata.empty() ? nullptr : kv.data();
        if (lance_dataset_tag_replace_metadata(
                handle_.get(), name.c_str(), metadata_ptr) != 0)
            check_error();
    }

    /// Compute per-field data statistics (compressed on-disk byte size) for
    /// query planning, ordered by schema field id. Performs I/O over every
    /// fragment. Throws lance::Error on failure.
    std::vector<FieldStatistics> calculate_data_stats() const {
        auto* raw = lance_dataset_calculate_data_stats(handle_.get());
        if (!raw) check_error();
        Handle<LanceDataStatistics, lance_data_statistics_close> snap(raw);

        uint64_t n = lance_data_statistics_count(snap.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        std::vector<FieldStatistics> out;
        out.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n; i++) {
            FieldStatistics fs;
            fs.id = lance_data_statistics_field_id_at(snap.get(), static_cast<size_t>(i));
            if (lance_last_error_code() != LANCE_OK) check_error();
            fs.bytes_on_disk =
                lance_data_statistics_bytes_on_disk_at(snap.get(), static_cast<size_t>(i));
            if (lance_last_error_code() != LANCE_OK) check_error();
            out.push_back(fs);
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

    /// Compact small or deleted-heavy fragments into larger ones, committing
    /// a new manifest. A clean dataset is a no-op — all-zero metrics and the
    /// version is unchanged. Pass `nullptr` for upstream defaults.
    /// Throws lance::Error on failure (commit conflict, ...).
    LanceCompactionMetrics compact_files(
        const LanceCompactionOptions* options = nullptr) {
        LanceCompactionMetrics metrics{};
        if (lance_dataset_compact_files(handle_.get(), options, &metrics) != 0) {
            check_error();
        }
        return metrics;
    }

    /// Drop columns from the dataset's schema and commit a new manifest.
    /// Metadata-only — data files remain until a later `compact_files()`
    /// call rewrites them. Mutates this dataset in place; the handle
    /// continues to point at the new version.
    ///
    /// `columns` must be non-empty. Throws lance::Error on failure (empty
    /// list, unknown column, attempt to drop every column, commit
    /// conflict, ...).
    void drop_columns(const std::vector<std::string>& columns) {
        std::vector<const char*> col_ptrs;
        col_ptrs.reserve(columns.size());
        for (const auto& c : columns) {
            col_ptrs.push_back(c.c_str());
        }
        // Pass `col_ptrs.data()` unconditionally — matches the `update`
        // and `merge_insert` siblings whose inputs are also required to
        // be non-empty. The Rust layer rejects `num_columns == 0` before
        // dereferencing the pointer, so an empty vector still surfaces
        // INVALID_ARGUMENT with the precise "num_columns must be > 0"
        // message rather than the misleading "columns must not be NULL".
        if (lance_dataset_drop_columns(
                handle_.get(), col_ptrs.data(), columns.size()) != 0) {
            check_error();
        }
    }

    /// Apply one or more column alterations (rename / nullability / type
    /// change) and commit a new manifest. Rename and nullability-only
    /// changes are zero-copy and preserve any indices on the affected
    /// columns; a type change rewrites the column's data files and drops
    /// any indices that referenced it.
    ///
    /// `alterations` must be non-empty and each entry must request at least
    /// one change. Any `data_type` pointer must remain valid for the
    /// duration of this call. Throws lance::Error on failure (empty list,
    /// no-op alteration, unknown column, incompatible cast, tightening
    /// nullability when NULLs exist, commit conflict, ...).
    void alter_columns(const std::vector<ColumnAlteration>& alterations) {
        // The C strings we install in each entry borrow from `alterations`
        // (the caller's std::strings), which outlive this call. The entries
        // themselves are copied by value into `raw`, so any reallocation
        // during push_back just moves the raw bytes — pointer values are
        // preserved. `reserve` is a performance hint, not a lifetime guard.
        std::vector<LanceColumnAlteration> raw;
        raw.reserve(alterations.size());
        for (const auto& a : alterations) {
            LanceColumnAlteration entry{};
            entry.path          = a.path.c_str();
            entry.rename        = a.rename ? a.rename->c_str() : nullptr;
            entry.nullable_mode = static_cast<int32_t>(a.nullable_mode);
            entry.data_type     = a.data_type;
            raw.push_back(entry);
        }
        if (lance_dataset_alter_columns(
                handle_.get(), raw.data(), raw.size()) != 0) {
            check_error();
        }
    }

    /// Add columns computed from SQL expressions over the dataset's existing
    /// columns, committing a new manifest. `batch_size = 0` uses the upstream
    /// default scan batch size.
    ///
    /// `columns` must be non-empty and each entry's `name` and `expression`
    /// must be non-empty. Throws lance::Error on failure (empty list, empty
    /// name/expression, malformed SQL syntax, a reference to a non-existent
    /// column, name collision with an existing column, commit conflict, ...).
    void add_columns_sql(const std::vector<SqlColumn>& columns,
                         uint64_t batch_size = 0) {
        // The C strings we install in each entry borrow from `columns` (the
        // caller's std::strings), which outlive this call. The entries are
        // copied by value into `raw`, so any reallocation during push_back
        // just moves the raw bytes — pointer values are preserved.
        std::vector<LanceSqlColumn> raw;
        raw.reserve(columns.size());
        for (const auto& c : columns) {
            LanceSqlColumn entry{};
            entry.name       = c.name.c_str();
            entry.expression = c.expression.c_str();
            raw.push_back(entry);
        }
        // Pass `raw.data()` unconditionally — matches the `alter_columns` and
        // `drop_columns` siblings whose inputs are also required to be
        // non-empty. An empty `columns` yields `num_columns == 0`, which the
        // Rust layer rejects before it indexes the pointer.
        if (lance_dataset_add_columns_sql(
                handle_.get(), raw.data(), raw.size(), batch_size) != 0) {
            check_error();
        }
    }

    /// Add all-null columns described by an Arrow schema, committing a new
    /// manifest. Metadata-only on non-legacy datasets. Every field in `schema`
    /// must be nullable. The caller owns `schema` and must keep it alive for
    /// the duration of the call; the wrapper does not release it.
    ///
    /// Throws lance::Error on failure (invalid schema, non-nullable field, name
    /// collision with an existing column, commit conflict, ...). A legacy-format
    /// dataset throws with code `LANCE_ERR_NOT_SUPPORTED` (all-null columns are
    /// metadata-only and the legacy format cannot represent them that way).
    void add_columns_nulls(const ArrowSchema* schema) {
        if (lance_dataset_add_columns_nulls(handle_.get(), schema) != 0) {
            check_error();
        }
    }

    /// Add columns by splicing precomputed data from an Arrow C stream into the
    /// dataset, committing a new manifest. `batch_size = 0` uses the upstream
    /// default. When non-null, `stream` is consumed (released) on every return
    /// path — including a null-dataset error and when this method throws — so do
    /// not use it again afterward. Only a null `stream` is rejected without
    /// consuming anything.
    ///
    /// The stream's total row count must match the dataset exactly. Throws
    /// lance::Error on failure (row-count mismatch, name collision with an
    /// existing column, commit conflict, ...).
    void add_columns_stream(ArrowArrayStream* stream, uint64_t batch_size = 0) {
        // Forward `stream` straight to the C API, which owns the stream and
        // releases it on every path. No RAII guard is needed here (unlike
        // `write`, which builds vectors before its C call): nothing between this
        // method's entry and the call below can throw, so the stream can never
        // be stranded by an exception. WARNING: do not add any throwing code
        // before the C call without first arming a stream-release guard.
        if (lance_dataset_add_columns_stream(
                handle_.get(), stream, batch_size) != 0) {
            check_error();
        }
    }

    /// Export the schema as an Arrow C Data Interface struct.
    void schema(ArrowSchema* out) const {
        if (lance_dataset_schema(handle_.get(), out) != 0) {
            check_error();
        }
    }

    /// Take rows by indices. `out` is caller-owned and its non-null `release`
    /// must be called exactly once. Deferred iteration/cleanup panics are
    /// contained by the exported stream guard.
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

    /// Take all columns with the same stream ownership as the overload above.
    void take(const uint64_t* indices, size_t num_indices,
              ArrowArrayStream* out) const {
        if (lance_dataset_take(handle_.get(), indices, num_indices, nullptr, out) != 0) {
            check_error();
        }
    }

    /// Take rows by dataset row IDs. `out` is caller-owned and its non-null
    /// `release` must be called exactly once. Deferred iteration/cleanup panics
    /// are contained by the exported stream guard.
    void take_rows(const uint64_t* row_ids, size_t num_row_ids,
                   const std::vector<std::string>& columns,
                   ArrowArrayStream* out) const {
        std::vector<const char*> col_ptrs;
        for (auto& c : columns) col_ptrs.push_back(c.c_str());
        col_ptrs.push_back(nullptr);
        const char* const* cols_ptr = columns.empty() ? nullptr : col_ptrs.data();

        if (lance_dataset_take_rows(
                handle_.get(), row_ids, num_row_ids, cols_ptr, out) != 0) {
            check_error();
        }
    }

    /// Take all columns by dataset row IDs with the same stream ownership as
    /// the overload above.
    void take_rows(const uint64_t* row_ids, size_t num_row_ids,
                   ArrowArrayStream* out) const {
        if (lance_dataset_take_rows(
                handle_.get(), row_ids, num_row_ids, nullptr, out) != 0) {
            check_error();
        }
    }

    /// Take blob handles by dataset row ID; element `i` is for `row_ids[i]`,
    /// `std::nullopt` for a null blob value.
    std::vector<std::optional<BlobFile>> take_blobs(
        const uint64_t* row_ids, size_t num_row_ids,
        const std::string& column) const {
        std::vector<LanceBlobFile*> raw(num_row_ids, nullptr);
        std::vector<std::optional<BlobFile>> blobs;
        blobs.reserve(num_row_ids);
        // An empty vector's data() may be null, which the C side rejects.
        if (num_row_ids > 0 &&
            lance_dataset_take_blobs(handle_.get(), row_ids, num_row_ids,
                                     column.c_str(), raw.data()) != 0) {
            check_error();
        }
        adopt_blobs(raw, blobs);
        return blobs;
    }

    /// Take blob handles by 0-based row index, with the same ownership and
    /// null-slot semantics as the overload above.
    std::vector<std::optional<BlobFile>> take_blobs_by_indices(
        const uint64_t* indices, size_t num_indices,
        const std::string& column) const {
        std::vector<LanceBlobFile*> raw(num_indices, nullptr);
        std::vector<std::optional<BlobFile>> blobs;
        blobs.reserve(num_indices);
        // Same empty-request shortcut as take_blobs().
        if (num_indices > 0 &&
            lance_dataset_take_blobs_by_indices(
                handle_.get(), indices, num_indices,
                column.c_str(), raw.data()) != 0) {
            check_error();
        }
        adopt_blobs(raw, blobs);
        return blobs;
    }

    /// Create a Scanner builder for this dataset.
    Scanner scan() const;

    /// Create a Scanner builder with an explicit column projection and optional
    /// primary SQL filter. An empty projection selects zero columns. A supplied
    /// filter is forwarded verbatim, including an empty expression. Invalid
    /// projections or filters are reported when the scanner is materialized.
    Scanner scan(const std::vector<std::string>& columns,
                 const std::optional<std::string>& filter = std::nullopt) const;

    /// Create a Scanner builder for all columns, with an optional SQL filter.
    Scanner scan(std::nullopt_t,
                 const std::optional<std::string>& filter = std::nullopt) const;

    /// Compatibility wrapper for an OR Match query.
    [[deprecated("Use prepare_fts_match_query() to select the Match operator")]]
    FtsQueryContext prepare_fts_query(
        const std::string& column,
        const std::string& query,
        uint32_t max_fuzzy_distance = 0,
        FtsCoverageMode coverage_mode = FtsCoverageMode::Strict) const {
        return prepare_fts_match_query(column, query, FtsMatchOperator::Or,
                                       max_fuzzy_distance, coverage_mode);
    }

    /// Prepare a Match query with a global BM25 scorer. AND and OR are
    /// supported. `max_fuzzy_distance` is reserved and currently must be zero;
    /// keeping it here avoids another API change when canonical cross-segment
    /// fuzzy vocabulary injection becomes available.
    FtsQueryContext prepare_fts_match_query(
        const std::string& column,
        const std::string& query,
        FtsMatchOperator match_operator = FtsMatchOperator::Or,
        uint32_t max_fuzzy_distance = 0,
        FtsCoverageMode coverage_mode = FtsCoverageMode::Strict) const {
        auto* context = lance_dataset_prepare_fts_match_query(
            handle_.get(), column.c_str(), query.c_str(),
            static_cast<int32_t>(match_operator), max_fuzzy_distance,
            static_cast<int32_t>(coverage_mode));
        if (!context) check_error();
        return FtsQueryContext(context);
    }

    /// Prepare a Phrase query. Its FTS index must store token positions and
    /// slop must be non-negative.
    FtsQueryContext prepare_fts_phrase_query(
        const std::string& column,
        const std::string& query,
        int32_t slop = 0,
        FtsCoverageMode coverage_mode = FtsCoverageMode::Strict) const {
        auto* context = lance_dataset_prepare_fts_phrase_query(
            handle_.get(), column.c_str(), query.c_str(), slop,
            static_cast<int32_t>(coverage_mode));
        if (!context) check_error();
        return FtsQueryContext(context);
    }

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

    /// Resolve a read-only fragment snapshot by ID. The returned handle stays
    /// bound to this dataset version and remains valid after this Dataset is
    /// destroyed. A missing ID throws LANCE_ERR_NOT_FOUND.
    Fragment open_fragment(uint64_t fragment_id) const {
        auto* fragment =
            lance_dataset_open_fragment(handle_.get(), fragment_id);
        if (!fragment) check_error();
        return Fragment(fragment);
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

    /// Create a single-use builder for an uncommitted scalar index segment.
    /// The builder owns a snapshot, so it remains valid independently of this
    /// Dataset object's lifetime.
    IndexSegmentBuilder new_scalar_index_segment_builder(
        const std::string& column,
        LanceScalarIndexType index_type,
        const std::string& index_name = "",
        const std::string& params_json = "",
        const LanceIndexSegmentBuildOptions* options = nullptr) const;

    /// Create a single-use builder for an uncommitted vector index segment.
    IndexSegmentBuilder new_vector_index_segment_builder(
        const std::string& column,
        const LanceVectorIndexParams& params,
        const std::string& index_name = "",
        const LanceIndexSegmentBuildOptions* options = nullptr) const;

    /// Train shared IVF centroids as an Arrow C Data Interface array/schema.
    IndexModel train_ivf_model(
        const std::string& column,
        uint32_t num_partitions,
        LanceMetricType metric,
        const std::vector<uint32_t>& fragment_ids = {}) const;

    /// Train a shared PQ codebook as an Arrow C Data Interface array/schema.
    /// L2 and cosine use IVF residuals; DOT trains on raw vectors.
    IndexModel train_pq_model(
        const std::string& column,
        uint32_t num_sub_vectors,
        uint32_t num_bits,
        LanceMetricType metric,
        IndexModel& ivf_centroids,
        const std::vector<uint32_t>& fragment_ids = {}) const;

    /// Drop an index by name.
    void drop_index(const std::string& name) {
        if (lance_dataset_drop_index(handle_.get(), name.c_str()) != 0)
            check_error();
    }

    /// Synchronously prewarm all segments of an index in this snapshot.
    /// Uses the dataset's session index cache; entries remain evictable.
    /// Throws lance::Error on failure, including an absent or empty index name.
    void prewarm_index(const std::string& index_name) const {
        if (lance_dataset_prewarm_index(handle_.get(), index_name.c_str()) != 0)
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

    /// JSON array with one entry per non-system physical index segment.
    /// Each entry includes the recorded fragment coverage and physical index
    /// format version. `fragment_ids` is null for unknown coverage and empty
    /// for known-empty coverage; historical fragment IDs are reported as
    /// recorded. Throws lance::Error on failure.
    std::string index_segments_json() const {
        const char* json = lance_dataset_index_segments_json(handle_.get());
        if (!json) check_error();
        std::string out(json);
        lance_free_string(json);
        return out;
    }

    /// Number of segments that make up a logical vector index.
    /// Throws lance::Error with code NotFound if the index does not exist.
    uint64_t index_segment_count(const std::string& index_name) const {
        uint64_t n = lance_dataset_index_segment_count(handle_.get(), index_name.c_str());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return n;
    }

    /// UUIDs of the physical segments that make up a logical vector index.
    /// Each UUID is a 16-byte array (RFC 4122 layout). Used by distributed
    /// query engines to fan k-NN out across workers — see
    /// `Scanner::index_segments`.
    std::vector<std::array<uint8_t, 16>> index_segments(const std::string& index_name) const {
        uint64_t count = index_segment_count(index_name);
        std::vector<std::array<uint8_t, 16>> out(count);
        if (count == 0) return out;
        uint64_t written = 0;
        if (lance_dataset_index_segments(handle_.get(), index_name.c_str(),
                                          reinterpret_cast<uint8_t*>(out.data()),
                                          static_cast<size_t>(count), &written) != 0)
            check_error();
        out.resize(static_cast<size_t>(written));
        return out;
    }

    /// Commit previously built uncommitted index segments as one logical
    /// index under `index_name` on `column`. Each entry of
    /// `segment_metadata` is the protobuf-encoded IndexMetadata produced by
    /// `IndexSegmentBuilder::execute_uncommitted()`. The commit is a single
    /// dataset version bump. Every segment must have been built for `column`.
    /// Coexisting vector segments, including retained existing segments, must
    /// have compatible metrics, dimensions, sub-index types, and quantizer
    /// kinds; independently trained IVF centroids and PQ codebooks may differ.
    /// Replacement of existing same-name segments is automatic and
    /// coverage-driven: fully covered segments are replaced, disjoint ones
    /// are retained as deltas, and partial overlap is rejected. A commit
    /// whose index type differs from the existing same-name index replaces
    /// that index entirely, so it must cover every current fragment; a
    /// partial-coverage type change is rejected.
    /// Fully replaced vector segments do not constrain the new metric.
    /// Throws lance::Error on validation failures (empty set, duplicate
    /// segment UUIDs, overlapping fragment coverage, unknown or mismatched
    /// column, incompatible vector segments), leaving the version and index
    /// unchanged.
    void commit_index_segments(
        const std::string& index_name,
        const std::string& column,
        const std::vector<std::vector<uint8_t>>& segment_metadata) {
        std::vector<const uint8_t*> bytes(segment_metadata.size());
        std::vector<size_t> lens(segment_metadata.size());
        for (size_t i = 0; i < segment_metadata.size(); ++i) {
            bytes[i] = segment_metadata[i].data();
            lens[i] = segment_metadata[i].size();
        }
        if (lance_dataset_commit_index_segments(
                handle_.get(), index_name.c_str(), column.c_str(), bytes.data(),
                lens.data(), segment_metadata.size()) != 0)
            check_error();
    }

    /// Access the underlying C handle (does not transfer ownership).
    const LanceDataset* c_handle() const { return handle_.get(); }

private:
    explicit Dataset(LanceDataset* ptr) : handle_(ptr) {}
};

// ─── Uncommitted index segments ──────────────────────────────────────────────

/// Move-only owner of an Arrow C Data Interface model array and schema.
class IndexModel {
    ArrowArray array_ = {};
    ArrowSchema schema_ = {};

    void reset() noexcept {
        if (array_.release) array_.release(&array_);
        if (schema_.release) schema_.release(&schema_);
    }

    friend class Dataset;

    IndexModel(ArrowArray array, ArrowSchema schema) noexcept
        : array_(array), schema_(schema) {}

public:
    ~IndexModel() { reset(); }

    IndexModel(IndexModel&& other) noexcept
        : array_(other.array_), schema_(other.schema_) {
        other.array_ = {};
        other.schema_ = {};
    }
    IndexModel& operator=(IndexModel&& other) noexcept {
        if (this != &other) {
            reset();
            array_ = other.array_;
            schema_ = other.schema_;
            other.array_ = {};
            other.schema_ = {};
        }
        return *this;
    }
    IndexModel(const IndexModel&) = delete;
    IndexModel& operator=(const IndexModel&) = delete;

    ArrowArray* array() noexcept { return &array_; }
    const ArrowSchema* schema() const noexcept { return &schema_; }
};

/// Parsed metadata for one physical index segment.
class IndexSegmentMetadata {
    Handle<LanceIndexSegmentMetadata, lance_index_segment_metadata_free> handle_;

public:
    explicit IndexSegmentMetadata(LanceIndexSegmentMetadata* metadata)
        : handle_(metadata) {}

    IndexSegmentMetadata(IndexSegmentMetadata&&) noexcept = default;
    IndexSegmentMetadata& operator=(IndexSegmentMetadata&&) noexcept = default;
    IndexSegmentMetadata(const IndexSegmentMetadata&) = delete;
    IndexSegmentMetadata& operator=(const IndexSegmentMetadata&) = delete;

    /// Parse protobuf-encoded IndexMetadata bytes.
    static IndexSegmentMetadata parse(const uint8_t* bytes, size_t len) {
        LanceIndexSegmentMetadata* metadata = nullptr;
        if (lance_index_segment_metadata_parse(bytes, len, &metadata) != 0)
            check_error();
        if (!metadata) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_index_segment_metadata_parse returned success with null metadata");
        }
        return IndexSegmentMetadata(metadata);
    }

    /// Vector overload for protobuf-encoded IndexMetadata bytes.
    static IndexSegmentMetadata parse(const std::vector<uint8_t>& bytes) {
        return parse(bytes.data(), bytes.size());
    }

    std::array<uint8_t, 16> uuid() const {
        std::array<uint8_t, 16> out = {};
        if (lance_index_segment_metadata_uuid(handle_.get(), out.data()) != 0)
            check_error();
        return out;
    }

    std::string name() const {
        const char* value = lance_index_segment_metadata_name(handle_.get());
        if (!value) check_error();
        return std::string(value);
    }

    uint64_t dataset_version() const {
        uint64_t version =
            lance_index_segment_metadata_dataset_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return version;
    }

    int32_t index_version() const {
        int32_t version =
            lance_index_segment_metadata_index_version(handle_.get());
        if (version < 0) check_error();
        return version;
    }

    int32_t index_type() const {
        int32_t type = lance_index_segment_metadata_index_type(handle_.get());
        if (type < 0) check_error();
        return type;
    }

    std::string index_details_type_url() const {
        const char* value =
            lance_index_segment_metadata_index_details_type_url(handle_.get());
        if (!value) check_error();
        return std::string(value);
    }

    std::vector<int32_t> field_ids() const {
        size_t count = lance_index_segment_metadata_field_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();

        std::vector<int32_t> out(count);
        size_t written = 0;
        if (lance_index_segment_metadata_field_ids(
                handle_.get(), out.data(), out.size(), &written) != 0)
            check_error();
        out.resize(written);
        return out;
    }

    std::vector<uint32_t> fragment_ids() const {
        size_t count =
            lance_index_segment_metadata_fragment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();

        std::vector<uint32_t> out(count);
        size_t written = 0;
        if (lance_index_segment_metadata_fragment_ids(
                handle_.get(), out.data(), out.size(), &written) != 0)
            check_error();
        out.resize(written);
        return out;
    }

    const LanceIndexSegmentMetadata* c_handle() const { return handle_.get(); }
};

/// Move-only builder for one uncommitted physical index segment.
class IndexSegmentBuilder {
    Handle<LanceIndexSegmentBuilder, lance_index_segment_builder_free> handle_;

public:
    explicit IndexSegmentBuilder(LanceIndexSegmentBuilder* builder)
        : handle_(builder) {}

    IndexSegmentBuilder(IndexSegmentBuilder&&) noexcept = default;
    IndexSegmentBuilder& operator=(IndexSegmentBuilder&&) noexcept = default;
    IndexSegmentBuilder(const IndexSegmentBuilder&) = delete;
    IndexSegmentBuilder& operator=(const IndexSegmentBuilder&) = delete;

    /// Execute the single-use builder without committing a dataset manifest.
    /// Native bytes are released even if error handling or vector allocation
    /// throws an exception.
    std::vector<uint8_t> execute_uncommitted() {
        struct BytesGuard {
            uint8_t* bytes = nullptr;
            ~BytesGuard() noexcept { lance_free_bytes(bytes); }

            BytesGuard() = default;
            BytesGuard(const BytesGuard&) = delete;
            BytesGuard& operator=(const BytesGuard&) = delete;
        } guard;

        size_t len = 0;
        if (lance_index_segment_builder_execute_uncommitted(
                handle_.get(), &guard.bytes, &len) != 0)
            check_error();
        if (!guard.bytes || len == 0) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_index_segment_builder_execute_uncommitted returned empty metadata");
        }
        return std::vector<uint8_t>(guard.bytes, guard.bytes + len);
    }

    /// Register a non-null index-build progress callback. Must be called
    /// before execute_uncommitted; the builder is single-use. The callback is
    /// invoked from internal worker threads and may be called concurrently
    /// from parallel worker tasks, so it must be thread-safe, non-blocking,
    /// and must not re-enter any lance_* function. It must return normally;
    /// unwinding or throwing across this boundary can abort the host process.
    /// Invocations occur only while execute_uncommitted runs, and this is
    /// enforced: lance-c disables the callback and drains in-flight
    /// invocations before that call returns, including on error, so a worker
    /// task detached by core can never invoke it afterwards. The callback and
    /// the context (if non-null) must remain valid until execute_uncommitted
    /// returns. Progress reporting is advisory and cannot affect the build
    /// outcome.
    IndexSegmentBuilder& progress_callback(LanceIndexBuildProgressCallback callback,
                                           void* callback_ctx) {
        if (lance_index_segment_builder_set_progress_callback(handle_.get(), callback,
                                                              callback_ctx) != 0)
            check_error();
        return *this;
    }

    LanceIndexSegmentBuilder* c_handle() { return handle_.get(); }
};

inline IndexSegmentBuilder Dataset::new_scalar_index_segment_builder(
    const std::string& column,
    LanceScalarIndexType index_type,
    const std::string& index_name,
    const std::string& params_json,
    const LanceIndexSegmentBuildOptions* options) const {
    const char* index_name_c = index_name.empty() ? nullptr : index_name.c_str();
    const char* params_json_c = params_json.empty() ? nullptr : params_json.c_str();
    auto* builder = lance_index_segment_builder_new_scalar(
        handle_.get(), column.c_str(), index_name_c,
        static_cast<int32_t>(index_type), params_json_c, options);
    if (!builder) check_error();
    return IndexSegmentBuilder(builder);
}

inline IndexSegmentBuilder Dataset::new_vector_index_segment_builder(
    const std::string& column,
    const LanceVectorIndexParams& params,
    const std::string& index_name,
    const LanceIndexSegmentBuildOptions* options) const {
    const char* index_name_c = index_name.empty() ? nullptr : index_name.c_str();
    LanceVectorIndexSegmentParams segment_params = {
        static_cast<int32_t>(params.index_type),
        static_cast<int32_t>(params.metric),
        params.num_partitions,
        params.num_sub_vectors,
        params.num_bits,
        params.max_iterations,
        params.hnsw_m,
        params.hnsw_ef_construction,
        params.sample_rate,
    };
    auto* builder = lance_index_segment_builder_new_vector(
        handle_.get(), column.c_str(), index_name_c, &segment_params, options);
    if (!builder) check_error();
    return IndexSegmentBuilder(builder);
}

inline IndexModel Dataset::train_ivf_model(
    const std::string& column,
    uint32_t num_partitions,
    LanceMetricType metric,
    const std::vector<uint32_t>& fragment_ids) const {
    ArrowArray array = {};
    ArrowSchema schema = {};
    const uint32_t* ids = fragment_ids.empty() ? nullptr : fragment_ids.data();
    if (lance_index_train_ivf_model(
            handle_.get(), column.c_str(), num_partitions,
            static_cast<int32_t>(metric), ids,
            fragment_ids.size(), &array, &schema) != 0)
        check_error();
    return IndexModel(array, schema);
}

inline IndexModel Dataset::train_pq_model(
    const std::string& column,
    uint32_t num_sub_vectors,
    uint32_t num_bits,
    LanceMetricType metric,
    IndexModel& ivf_centroids,
    const std::vector<uint32_t>& fragment_ids) const {
    ArrowArray array = {};
    ArrowSchema schema = {};
    const uint32_t* ids = fragment_ids.empty() ? nullptr : fragment_ids.data();
    if (lance_index_train_pq_model(
            handle_.get(), column.c_str(), num_sub_vectors, num_bits,
            static_cast<int32_t>(metric), ids, fragment_ids.size(),
            ivf_centroids.array(), ivf_centroids.schema(), &array, &schema) != 0)
        check_error();
    return IndexModel(array, schema);
}

// ─── Scanner ─────────────────────────────────────────────────────────────────

/// One native scanner ordering key. Entries are applied in vector order.
struct ColumnOrdering {
    std::string column_name;
    LanceOrderDirection direction = LANCE_ORDER_ASCENDING;
    LanceNullOrder null_order = LANCE_ORDER_NULLS_LAST;
};

class Scanner {
    Handle<LanceScanner, lance_scanner_close> handle_;

public:
    friend class Fragment;

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

    /// Set the target output batch size in bytes.
    Scanner& batch_size_bytes(uint64_t bytes) {
        if (lance_scanner_set_batch_size_bytes(handle_.get(), bytes) != 0)
            check_error();
        return *this;
    }

    /// Set the scanner I/O buffer size in bytes, in the range [1, INT64_MAX].
    Scanner& io_buffer_size(uint64_t bytes) {
        if (lance_scanner_set_io_buffer_size(handle_.get(), bytes) != 0)
            check_error();
        return *this;
    }

    /// Set the number of batches decoded concurrently.
    Scanner& batch_readahead(size_t batches) {
        if (lance_scanner_set_batch_readahead(handle_.get(), batches) != 0)
            check_error();
        return *this;
    }

    /// Set fragment readahead for unordered scans.
    Scanner& fragment_readahead(size_t fragments) {
        if (lance_scanner_set_fragment_readahead(handle_.get(), fragments) != 0)
            check_error();
        return *this;
    }

    /// Set the target number of physical execution partitions.
    Scanner& target_parallelism(size_t partitions) {
        if (lance_scanner_set_target_parallelism(handle_.get(), partitions) != 0)
            check_error();
        return *this;
    }

    /// Configure whether batches are returned in storage order.
    Scanner& scan_in_order(bool ordered = true) {
        if (lance_scanner_set_scan_in_order(handle_.get(), ordered) != 0)
            check_error();
        return *this;
    }

    /// Configure whether scalar indices may be used to optimize filters.
    /// False also disables explicit scalar segment search, retaining its fragment domain.
    Scanner& use_scalar_index(bool enable = true) {
        if (lance_scanner_set_use_scalar_index(handle_.get(), enable) != 0)
            check_error();
        return *this;
    }

    /// Configure whether row-based output batches are strict.
    Scanner& strict_batch_size(bool strict_batch_size = true) {
        if (lance_scanner_set_strict_batch_size(handle_.get(), strict_batch_size) != 0)
            check_error();
        return *this;
    }

    /// Configure whether file statistics may optimize the scan.
    Scanner& use_stats(bool use_stats = true) {
        if (lance_scanner_set_use_stats(handle_.get(), use_stats) != 0)
            check_error();
        return *this;
    }

    /// Replace the native result ordering. `std::nullopt` clears it; an empty
    /// vector is the explicit native empty ordering. Lance validates column
    /// names when the scan is materialized. Not supported with
    /// nearest_multivector(); that scan fails on start.
    Scanner& order_by(std::nullopt_t) {
        if (lance_scanner_set_order_by(handle_.get(), nullptr, 0) != 0)
            check_error();
        return *this;
    }

    Scanner& order_by(const std::vector<ColumnOrdering>& orderings) {
        std::vector<LanceColumnOrdering> native;
        native.reserve(orderings.size());
        for (const auto& ordering : orderings) {
            native.push_back({
                ordering.column_name.c_str(),
                static_cast<int32_t>(ordering.direction),
                static_cast<int32_t>(ordering.null_order),
            });
        }
        LanceColumnOrdering explicit_empty = {};
        const LanceColumnOrdering* data =
            native.empty() ? &explicit_empty : native.data();
        if (lance_scanner_set_order_by(handle_.get(), data, native.size()) != 0)
            check_error();
        return *this;
    }

    /// Choose how blob columns are materialized (default: descriptors for blob
    /// columns, bytes for every other binary column).
    Scanner& blob_handling(LanceBlobHandling handling) {
        if (lance_scanner_set_blob_handling(handle_.get(), handling) != 0)
            check_error();
        return *this;
    }

    /// Configure native materialization for ordinary payload columns.
    /// For ALL_EARLY_EXCEPT, names are resolved by Lance, including nested paths.
    /// An empty list is an explicit empty exception list. See lance.h.
    Scanner& materialization_style(
        LanceMaterializationStyle style,
        const std::vector<std::string>& early_except_columns = {}) {
        std::vector<const char*> column_ptrs;
        column_ptrs.reserve(early_except_columns.size());
        for (const auto& column : early_except_columns) {
            column_ptrs.push_back(column.c_str());
        }
        const char* const* columns =
            column_ptrs.empty() ? nullptr : column_ptrs.data();
        if (lance_scanner_set_materialization_style(
                handle_.get(), style, columns, column_ptrs.size()) != 0)
            check_error();
        return *this;
    }

    /// Enable/disable row ID in output.
    Scanner& with_row_id(bool enable = true) {
        if (lance_scanner_with_row_id(handle_.get(), enable) != 0)
            check_error();
        return *this;
    }

    /// Include or omit the `_rowaddr` metadata column.
    Scanner& with_row_address(bool enable = true) {
        if (lance_scanner_with_row_address(handle_.get(), enable) != 0)
            check_error();
        return *this;
    }

    /// Configure whether deleted rows still present in storage are returned.
    /// Requires with_row_id(true); use_scalar_index(false) is needed for filtered scans.
    /// Incompatible with scalar_index_segment. See lance.h.
    Scanner& include_deleted_rows(bool include_deleted_rows = true) {
        if (lance_scanner_set_include_deleted_rows(handle_.get(), include_deleted_rows) != 0)
            check_error();
        return *this;
    }

    /// Generate exact candidates from one BTree/Bitmap/LabelList segment.
    /// fragment_ids is required and defines the complete read/fallback domain. See lance.h.
    /// Requires live rows only: include_deleted_rows(true) is rejected at stream creation.
    /// use_scalar_index(false) selects the scoped fallback without searching the segment.
    Scanner& scalar_index_segment(const std::array<uint8_t, 16>& segment_uuid) {
        if (lance_scanner_set_scalar_index_segment(handle_.get(), segment_uuid.data()) != 0)
            check_error();
        return *this;
    }

    /// Enable bounded page-cost selection among BTree segments covering the read domain.
    /// Applies only to scalar_index_segment scans; fragment_ids remains required. Disabled by default.
    Scanner& scalar_index_auto_selection(bool enabled = true) {
        if (lance_scanner_set_scalar_index_auto_selection(handle_.get(), enabled) != 0)
            check_error();
        return *this;
    }

    Scanner& clear_scalar_index_segment() {
        if (lance_scanner_set_scalar_index_segment(handle_.get(), nullptr) != 0)
            check_error();
        return *this;
    }

    /// Restrict the scan/search domain to specific fragment IDs. For nearest
    /// queries, they limit candidates before Top-K regardless of prefilter().
    Scanner& fragment_ids(const uint64_t* ids, size_t len) {
        if (lance_scanner_set_fragment_ids(handle_.get(), ids, len) != 0)
            check_error();
        return *this;
    }

    /// Restrict the scan/search domain to specific fragment IDs (vector overload).
    Scanner& fragment_ids(const std::vector<uint64_t>& ids) {
        return fragment_ids(ids.data(), ids.size());
    }

    /// Restrict the scan or search to rows whose `_rowid` is in `row_ids`
    /// (applied before ranking). `len == 0` allows no rows.
    Scanner& row_addr_allowlist(const uint64_t* row_ids, size_t len) {
        if (lance_scanner_set_row_addr_allowlist(handle_.get(), row_ids, len) != 0)
            check_error();
        return *this;
    }

    /// Restrict the scan or search to rows whose `_rowid` is in `row_ids` (vector overload).
    Scanner& row_addr_allowlist(const std::vector<uint64_t>& row_ids) {
        return row_addr_allowlist(row_ids.data(), row_ids.size());
    }

    /// Exclude rows whose `_rowid` is in `row_ids` (applied before ranking).
    Scanner& row_addr_blocklist(const uint64_t* row_ids, size_t len) {
        if (lance_scanner_set_row_addr_blocklist(handle_.get(), row_ids, len) != 0)
            check_error();
        return *this;
    }

    /// Exclude rows whose `_rowid` is in `row_ids` (vector overload).
    Scanner& row_addr_blocklist(const std::vector<uint64_t>& row_ids) {
        return row_addr_blocklist(row_ids.data(), row_ids.size());
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

    /// Add an SQL filter that is combined with the selected primary filter using AND.
    Scanner& additional_sql_filter(const std::string& filter) {
        if (lance_scanner_additional_sql_filter(handle_.get(), filter.c_str()) != 0)
            check_error();
        return *this;
    }

    /// Register a non-null callback for scan statistics after successful full exhaustion.
    /// The registration applies to every stream derived from this scanner, including
    /// concurrent streams and streams created after an earlier callback returns. The
    /// callback is not guaranteed on error, cancellation, or early release. It may
    /// run on the thread that observes EOF, must be thread-safe, must not throw, and
    /// must not re-enter the originating scanner. The callback and a non-null context
    /// must remain valid until the scanner is closed, all async scan requests have
    /// delivered their completion callbacks, and all derived streams are released.
    /// From callback entry until the enclosing operation that observes EOF has
    /// returned to its caller, the callback must not directly or indirectly cause
    /// any ArrowArrayStream derived from this Scanner to be accessed, called,
    /// released, moved, or destroyed. This includes signaling or scheduling another
    /// thread to act based only on callback completion: the callback returns before
    /// the enclosing stream operation does. Such interaction is reentrant and has
    /// undefined behavior. Normal access may resume only after the enclosing
    /// ArrowArrayStream `get_next`, `lance_scanner_next`, or
    /// `lance_scanner_poll_next` call returns.
    Scanner& statistics_callback(LanceScanStatisticsCallback callback, void* callback_ctx) {
        if (lance_scanner_set_statistics_callback(handle_.get(), callback, callback_ctx) != 0)
            check_error();
        return *this;
    }

    /// Restrict the next k-NN query to a subset of vector index segments.
    /// Pass `len` 16-byte UUIDs concatenated as a single byte buffer
    /// (total bytes = `len * 16`). Pass len=0 (and any pointer) to clear.
    Scanner& index_segments(const uint8_t* uuids, size_t len) {
        if (lance_scanner_set_index_segments(handle_.get(), uuids, len) != 0)
            check_error();
        return *this;
    }

    /// Restrict the next k-NN query to a subset of vector index segments
    /// (typed vector overload).
    Scanner& index_segments(const std::vector<std::array<uint8_t, 16>>& uuids) {
        return index_segments(reinterpret_cast<const uint8_t*>(uuids.data()), uuids.size());
    }

    /// Materialize an independent ArrowArrayStream (blocking). The scanner remains valid.
    /// `out` is caller-owned; call its non-null `release` callback exactly once.
    void to_arrow_stream(ArrowArrayStream* out) {
        if (lance_scanner_to_arrow_stream(handle_.get(), out) != 0)
            check_error();
    }

    /// Start an async scan with a non-null callback. On success, the callback's
    /// ArrowArrayStream result is library-allocated and must be passed exactly
    /// once to `lance::scanner_async_stream_free`, which also invokes `release`
    /// when necessary. The callback normally runs on the dispatcher thread,
    /// but a rare infrastructure fallback may invoke it on the calling or
    /// producing thread, possibly before this method returns, so it must be
    /// thread-safe. Exactly one completion is delivered; callback and non-null
    /// context storage must remain valid until it returns.
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

    /// Restrict single-vector nearest results to [lower_bound, upper_bound).
    /// Call after nearest() and before scanning. Omitted bounds are unbounded;
    /// distance_range() clears both. Bounds must be finite and lower < upper
    /// when both are present. Uses metric distance units (squared L2 for L2).
    /// The nearest query's k cap and ANN candidate selection still apply.
    Scanner& distance_range(std::optional<float> lower_bound = std::nullopt,
                            std::optional<float> upper_bound = std::nullopt) {
        if (lance_scanner_set_distance_range(handle_.get(),
                                            lower_bound ? &*lower_bound : nullptr,
                                            upper_bound ? &*upper_bound : nullptr) != 0)
            check_error();
        return *this;
    }

    /// Independent queries; each returns up to k rows tagged with query_index.
    Scanner& nearest_batch(const std::string& column, const void* query_data,
                           size_t dimension, size_t num_queries,
                           LanceDataType element_type, uint32_t k) {
        if (lance_scanner_nearest_batch(handle_.get(), column.c_str(), query_data,
                                       dimension, num_queries, element_type, k) != 0)
            check_error();
        return *this;
    }

    /// Float32 batch convenience overload; input values are copied before returning.
    Scanner& nearest_batch(const std::string& column, const float* query_data,
                           size_t dimension, size_t num_queries, uint32_t k) {
        return nearest_batch(column, query_data, dimension, num_queries, LANCE_DTYPE_FLOAT32, k);
    }

    /// One multi-vector query, copied from dimension * num_vectors row-major elements.
    Scanner& nearest_multivector(const std::string& column, const void* query_data,
                                size_t dimension, size_t num_vectors,
                                LanceDataType element_type, uint32_t k) {
        if (lance_scanner_nearest_multivector(handle_.get(), column.c_str(), query_data,
                                            dimension, num_vectors, element_type, k) != 0)
            check_error();
        return *this;
    }

    /// Replace both minimum and maximum partition-search bounds.
    Scanner& nprobes(uint32_t nprobes) {
        if (lance_scanner_set_nprobes(handle_.get(), nprobes) != 0) check_error();
        return *this;
    }
    /// Replace only the minimum partition-search bound.
    Scanner& minimum_nprobes(uint32_t minimum_nprobes) {
        if (lance_scanner_set_minimum_nprobes(handle_.get(), minimum_nprobes) != 0) check_error();
        return *this;
    }
    /// Replace only the maximum partition-search bound.
    Scanner& maximum_nprobes(uint32_t maximum_nprobes) {
        if (lance_scanner_set_maximum_nprobes(handle_.get(), maximum_nprobes) != 0) check_error();
        return *this;
    }
    Scanner& approx_mode(LanceApproxMode approx_mode) {
        if (lance_scanner_set_approx_mode(handle_.get(), approx_mode) != 0) check_error();
        return *this;
    }
    Scanner& query_parallelism(int32_t parallelism) {
        if (lance_scanner_set_query_parallelism(handle_.get(), parallelism) != 0)
            check_error();
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
    /// Use native indexed-only fast search for vector queries, scalar-indexed
    /// filters and full-text search. Index-backed paths can omit unindexed data.
    /// Ordinary scans and non-indexable expressions follow native planning.
    /// Disabled by default. See lance.h.
    Scanner& fast_search(bool enable = true) {
        if (lance_scanner_set_fast_search(handle_.get(), enable) != 0) check_error();
        return *this;
    }
    /// Apply scanner filters before ANN when enabled, or refine ANN candidates
    /// afterward when disabled; postfiltering may return fewer than k rows. This
    /// does not change the fragment search domain.
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

    /// Attach an immutable typed FTS query. The scanner snapshots the tree, so
    /// the query and its source handles may be destroyed after success.
    Scanner& full_text_search(const FtsQuery& query) {
        if (lance_scanner_set_fts_query(handle_.get(), query.c_handle()) != 0)
            check_error();
        return *this;
    }

    /// Attach a process-local prepared FTS query context. The scanner retains
    /// shared ownership, so the context object may be destroyed after success.
    Scanner& fts_query_context(const FtsQueryContext& context) {
        if (lance_scanner_set_fts_query_context(handle_.get(), context.c_handle()) != 0)
            check_error();
        return *this;
    }

    /// Restrict a context-backed FTS query to a segment UUID subset.
    Scanner& fts_index_segments(const uint8_t* segment_uuids, size_t segment_count) {
        if (lance_scanner_set_fts_index_segments(
                handle_.get(), segment_uuids, segment_count) != 0)
            check_error();
        return *this;
    }

    Scanner& fts_index_segments(
        const std::vector<std::array<uint8_t, 16>>& segment_uuids) {
        return fts_index_segments(
            reinterpret_cast<const uint8_t*>(segment_uuids.data()),
            segment_uuids.size());
    }

    /// Access the underlying C handle.
    LanceScanner* c_handle() { return handle_.get(); }
};

inline Scanner Fragment::scan() const {
    auto* scanner = lance_fragment_scan(handle_.get());
    if (!scanner) check_error();
    return Scanner(scanner);
}

inline void Fragment::take(
    const uint32_t* indices,
    size_t num_indices,
    const std::vector<std::string>& columns,
    ArrowArrayStream* out) const {
    std::vector<const char*> column_ptrs;
    column_ptrs.reserve(columns.size() + 1);
    for (const auto& column : columns) column_ptrs.push_back(column.c_str());
    column_ptrs.push_back(nullptr);

    const char* const empty_projection[] = {nullptr};
    const char* const* columns_ptr =
        columns.empty() ? empty_projection : column_ptrs.data();
    if (lance_fragment_take(
            handle_.get(), indices, num_indices, columns_ptr, out) != 0) {
        check_error();
    }
}

inline void Fragment::take(
    const uint32_t* indices,
    size_t num_indices,
    ArrowArrayStream* out) const {
    if (lance_fragment_take(
            handle_.get(), indices, num_indices, nullptr, out) != 0) {
        check_error();
    }
}

inline Scanner Dataset::scan() const {
    return scan(std::nullopt);
}

inline Scanner Dataset::scan(const std::vector<std::string>& columns,
                             const std::optional<std::string>& filter) const {
    std::vector<const char*> col_ptrs;
    col_ptrs.reserve(columns.size() + 1);
    for (const auto& column : columns) col_ptrs.push_back(column.c_str());
    col_ptrs.push_back(nullptr);

    auto* s = lance_scanner_new(
        handle_.get(),
        col_ptrs.data(),
        filter ? filter->c_str() : nullptr);
    if (!s) check_error();
    return Scanner(s);
}

inline Scanner Dataset::scan(std::nullopt_t,
                             const std::optional<std::string>& filter) const {
    auto* s = lance_scanner_new(handle_.get(), nullptr,
                               filter ? filter->c_str() : nullptr);
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
