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
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lance {

class Scanner;
struct BatchCommitResult;

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

/// Build a portable IVF_RQ fast-rotation model as native JSON. `num_bits == 0`
/// selects the native RQ default of 5.
inline std::string build_rq_model(uint32_t dimension, uint32_t num_bits = 5) {
    const char* json = lance_index_build_rq_model(dimension, num_bits);
    if (!json) check_error();
    std::unique_ptr<const char, void (*)(const char*)> guard(json,
                                                             lance_free_string);
    return std::string(json);
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

/// Move-only owner of an Arrow C Data Interface stream.
///
/// The wrapped stream's `release` callback is invoked exactly once, either
/// explicitly with `reset()` or from the destructor. Moving transfers
/// ownership and leaves the source empty.
class ArrowStream {
    ArrowArrayStream stream_{};
    bool active_ = false;

public:
    ArrowStream() noexcept = default;

    explicit ArrowStream(ArrowArrayStream stream) noexcept
        : stream_(stream), active_(stream.release != nullptr) {}

    ~ArrowStream() { reset(); }

    ArrowStream(const ArrowStream&) = delete;
    ArrowStream& operator=(const ArrowStream&) = delete;

    ArrowStream(ArrowStream&& other) noexcept
        : stream_(other.stream_), active_(other.active_) {
        other.stream_ = {};
        other.active_ = false;
    }

    ArrowStream& operator=(ArrowStream&& other) noexcept {
        if (this != &other) {
            reset();
            stream_ = other.stream_;
            active_ = other.active_;
            other.stream_ = {};
            other.active_ = false;
        }
        return *this;
    }

    void reset() noexcept {
        if (active_ && stream_.release) {
            stream_.release(&stream_);
        }
        stream_ = {};
        active_ = false;
    }

    ArrowArrayStream* get() noexcept { return &stream_; }
    const ArrowArrayStream* get() const noexcept { return &stream_; }
    ArrowArrayStream* operator->() noexcept { return &stream_; }
    const ArrowArrayStream* operator->() const noexcept { return &stream_; }
    explicit operator bool() const noexcept { return active_; }
};

// ─── Forward Declarations ────────────────────────────────────────────────────

class Scanner;
class Dataset;
class Fragment;
class FragmentOverlayWriter;
class LanceSchema;
class Batch;
struct Table;
class ArrowSchemaOwner;
class ShardWriter;
class LsmScanner;
class ExecutionPlan;
class IndexModel;
class IndexSegmentBuilder;
class IndexSegmentMetadata;
class CompactionOptions;
class CompactionPlan;
class CompactionTask;
class RewriteResult;
struct BasePath;
class OptimizeIndicesOptions;
class OptimizeIndicesPlan;
class OptimizeIndexTask;
class OptimizeIndexMerge;
class IndexSegmentResult;
class IvfModel;
class FtsQueryContext;
class FtsQuery;
class Namespace;
class SqlQueryBuilder;
class SqlQuery;
class DatasetDeltaBuilder;
class DatasetDelta;

/// One native `BasePath` for `Dataset::add_bases` and `Operation`
/// construction.
///
/// `id == 0` requests native allocation. `name == std::nullopt` maps to native
/// `None`; an engaged empty string is an explicit empty name.
struct BasePath {
    uint32_t id = 0;
    std::string path;
    std::optional<std::string> name;
    bool is_dataset_root = false;
};

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

// ─── Dataset maintenance ─────────────────────────────────────────────────────

/// Copied native `RemovalStats`.
struct RemovalStats {
    uint64_t bytes_removed = 0;
    uint64_t old_versions = 0;
    uint64_t data_files_removed = 0;
    uint64_t transaction_files_removed = 0;
    uint64_t index_files_removed = 0;
    uint64_t deletion_files_removed = 0;
    uint64_t failed_deletes = 0;
};

/// Native cleanup file kinds.
enum class CleanupFileKind : int32_t {
    Manifest          = LANCE_CLEANUP_FILE_MANIFEST,
    Data              = LANCE_CLEANUP_FILE_DATA,
    Transaction       = LANCE_CLEANUP_FILE_TRANSACTION,
    Index             = LANCE_CLEANUP_FILE_INDEX,
    Deletion          = LANCE_CLEANUP_FILE_DELETION,
    TemporaryManifest = LANCE_CLEANUP_FILE_TEMPORARY_MANIFEST,
};

/// Whole seconds plus nanoseconds since the Unix epoch.
struct CleanupTimestamp {
    int64_t  seconds = 0;
    uint32_t nanos = 0;
};

/// Native manual cleanup policy.
///
/// A default-constructed value leaves all optional selectors unset and
/// inherits native defaults, including
/// `error_if_tagged_old_versions = true`. `before_timestamp`, `before_version`
/// and `versions` combine with AND. `retain_versions` is resolved by the native
/// asynchronous builder and cannot be combined with `before_version`.
///
/// `versions == std::nullopt` means no exact-version selector. An engaged empty
/// vector is an explicit empty selection and is rejected by Lance.
struct CleanupPolicy {
    std::optional<CleanupTimestamp>       before_timestamp;
    std::optional<uint64_t>               before_version;
    std::optional<std::vector<uint64_t>>  versions;
    std::optional<uint64_t>               retain_versions;
    std::optional<bool>                   delete_unverified;
    std::optional<bool>                   error_if_tagged_old_versions;
    std::optional<bool>                   clean_referenced_branches;
    std::optional<uint64_t>               delete_rate_limit;
};

/// Optional native `CleanupOperation` overrides.
///
/// `max_candidate_files` affects explanations and zero is valid.
struct CleanupOperationOptions {
    std::optional<uint64_t> delete_concurrency;
    std::optional<uint64_t> delete_rate_limit;
    std::optional<uint64_t> max_candidate_files;
};

/// Relative-duration inputs for `Dataset::cleanup_old_versions`.
struct CleanupOldVersionsOptions {
    int64_t             older_than_seconds = 0;
    int32_t             older_than_nanos = 0;
    std::optional<bool> delete_unverified;
    std::optional<bool> error_if_tagged_old_versions;
};

/// One copied cleanup candidate.
struct CleanupCandidateFile {
    std::string      path;
    CleanupFileKind  kind = CleanupFileKind::Manifest;
    bool             unverified = false;
    uint64_t         size_bytes = 0;
};

/// One copied referenced child branch.
struct CleanupReferencedBranch {
    std::string name;
    uint64_t    referenced_version = 0;
    bool        cleanup_candidate = false;
};

/// Fully copied native `CleanupExplanation`.
struct CleanupExplanation {
    uint64_t read_version = 0;
    RemovalStats stats;
    std::vector<CleanupCandidateFile> candidate_files;
    bool candidate_files_truncated = false;
    uint64_t candidate_file_limit = 0;
    std::vector<CleanupReferencedBranch> referenced_branches;
    std::vector<std::string> warnings;
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

enum class FieldReferenceKind : int32_t {
    Id   = LANCE_FIELD_REFERENCE_ID,
    Path = LANCE_FIELD_REFERENCE_PATH,
};

/// Owned field reference for field metadata updates and field-ID resolution.
/// The wrapper keeps the path string alive for the duration of C calls.
class FieldReference {
    FieldReferenceKind kind_;
    int32_t field_id_ = 0;
    std::string path_;

    FieldReference(FieldReferenceKind kind, int32_t field_id, std::string path)
        : kind_(kind), field_id_(field_id), path_(std::move(path)) {}

public:
    static FieldReference by_id(int32_t field_id) {
        return FieldReference(FieldReferenceKind::Id, field_id, {});
    }

    static FieldReference by_path(std::string path) {
        return FieldReference(FieldReferenceKind::Path, 0, std::move(path));
    }

    LanceFieldReference c_handle() const {
        return LanceFieldReference{
            static_cast<LanceFieldReferenceKind>(kind_),
            field_id_,
            kind_ == FieldReferenceKind::Path ? path_.c_str() : nullptr,
        };
    }
};

/// One field's metadata update in a batch. A `std::nullopt` value removes its
/// key; an empty string stores a real empty value. The mode applies only to
/// this field.
struct FieldMetadataUpdate {
    FieldReference field;
    std::vector<std::pair<std::string, std::optional<std::string>>> values;
    MetadataUpdateMode mode = MetadataUpdateMode::Merge;
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

/// A native duration expressed as whole seconds plus a subsecond nanosecond
/// component. Nanoseconds must be less than 1,000,000,000.
struct CommitDuration {
    uint64_t seconds = 0;
    uint32_t nanos = 0;
};

enum class CommitTimeoutMode : int32_t {
    Default  = LANCE_COMMIT_TIMEOUT_DEFAULT,
    Disabled = LANCE_COMMIT_TIMEOUT_DISABLED,
    Explicit = LANCE_COMMIT_TIMEOUT_EXPLICIT,
};

/// Settings forwarded directly to the native CommitBuilder.
///
/// Omitted optionals keep native defaults. `max_retries = 0` is forwarded as a
/// real value and selects strict overwrite behavior when applicable.
struct CommitOptions {
    std::optional<bool> use_stable_row_ids;
    std::optional<bool> enable_v2_manifest_paths;
    std::optional<uint32_t> max_retries;
    std::optional<CommitDuration> retry_timeout;
    CommitTimeoutMode timeout_mode = CommitTimeoutMode::Default;
    CommitDuration timeout;
    bool detached = false;
    bool skip_auto_cleanup = false;
    std::optional<std::string> data_storage_version;
};

/// Options for Dataset::write_uncommitted.
///
/// `write` reuses the committing writer's native option mapping.
/// `transaction_properties == std::nullopt` selects native None; a supplied
/// empty vector selects native Some(empty).
struct UncommittedWriteOptions {
    WriteOptions write;
    std::optional<std::vector<std::pair<std::string, std::string>>>
        transaction_properties;
};

// ─── Native transactions ─────────────────────────────────────────────────────

/// Owned native `DataFile` metadata snapshot. Move-only.
class DataFile {
    Handle<LanceDataFile, lance_data_file_close> handle_;

public:
    explicit DataFile(LanceDataFile* file) : handle_(file) {}

    DataFile(DataFile&&) noexcept = default;
    DataFile& operator=(DataFile&&) noexcept = default;
    DataFile(const DataFile&) = delete;
    DataFile& operator=(const DataFile&) = delete;

    /// Native data-file path relative to its selected data directory.
    std::string path() const {
        const char* path = lance_data_file_path(handle_.get());
        if (!path) check_error();
        return path;
    }

    /// Native field IDs in stored order.
    std::vector<int32_t> fields() const {
        size_t count = lance_data_file_field_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        std::vector<int32_t> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            int32_t field_id = 0;
            if (lance_data_file_field_id_at(
                    handle_.get(), i, &field_id) != 0) {
                check_error();
            }
            out.push_back(field_id);
        }
        return out;
    }

    /// Native column indices in stored order, including the `-1` marker.
    std::vector<int32_t> column_indices() const {
        size_t count = lance_data_file_column_index_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        std::vector<int32_t> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            int32_t column_index = 0;
            if (lance_data_file_column_index_at(
                    handle_.get(), i, &column_index) != 0) {
                check_error();
            }
            out.push_back(column_index);
        }
        return out;
    }

    /// Native major/minor data-file format version.
    std::pair<uint32_t, uint32_t> file_version() const {
        uint32_t major = 0;
        uint32_t minor = 0;
        if (lance_data_file_file_version(
                handle_.get(), &major, &minor) != 0) {
            check_error();
        }
        return {major, minor};
    }

    /// Native optional file size in bytes.
    std::optional<uint64_t> file_size() const {
        bool present = false;
        uint64_t size = 0;
        if (lance_data_file_file_size(
                handle_.get(), &present, &size) != 0) {
            check_error();
        }
        if (!present) return std::nullopt;
        return size;
    }

    /// Native optional base path ID.
    std::optional<uint32_t> base_id() const {
        bool present = false;
        uint32_t base_id = 0;
        if (lance_data_file_base_id(
                handle_.get(), &present, &base_id) != 0) {
            check_error();
        }
        if (!present) return std::nullopt;
        return base_id;
    }

    /// Clone this snapshot into an independently owned handle.
    DataFile clone() const {
        auto* raw = lance_data_file_clone(handle_.get());
        if (!raw) check_error();
        return DataFile(raw);
    }

    const LanceDataFile* c_handle() const { return handle_.get(); }
};

/// Owned native `DataReplacementGroup` snapshot. Move-only.
class DataReplacementGroup {
    Handle<LanceDataReplacementGroup, lance_data_replacement_group_close>
        handle_;

public:
    explicit DataReplacementGroup(LanceDataReplacementGroup* group)
        : handle_(group) {}

    /// Construct a group by cloning `file` into native metadata.
    DataReplacementGroup(uint64_t fragment_id, const DataFile& file)
        : handle_(nullptr) {
        auto* raw =
            lance_data_replacement_group_new(fragment_id, file.c_handle());
        if (!raw) check_error();
        handle_ =
            Handle<LanceDataReplacementGroup, lance_data_replacement_group_close>(
                raw);
    }

    DataReplacementGroup(DataReplacementGroup&&) noexcept = default;
    DataReplacementGroup& operator=(DataReplacementGroup&&) noexcept = default;
    DataReplacementGroup(const DataReplacementGroup&) = delete;
    DataReplacementGroup& operator=(const DataReplacementGroup&) = delete;

    uint64_t fragment_id() const {
        uint64_t id = lance_data_replacement_group_fragment_id(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return id;
    }

    DataFile file() const {
        LanceDataFile* file = nullptr;
        if (lance_data_replacement_group_file(handle_.get(), &file) != 0) {
            check_error();
        }
        return DataFile(file);
    }

    /// Clone this snapshot into an independently owned handle.
    DataReplacementGroup clone() const {
        auto* raw = lance_data_replacement_group_clone(handle_.get());
        if (!raw) check_error();
        return DataReplacementGroup(raw);
    }

    const LanceDataReplacementGroup* c_handle() const {
        return handle_.get();
    }
};

/// Owned native `UpdateMap` snapshot. Move-only.
///
/// Entries are an ordered list, not a hash map: duplicate keys, order and
/// absent values are native data. `replace == true` makes native commit code
/// clear the target and insert only entries with values; `replace == false`
/// upserts values and removes keys whose value is `std::nullopt`.
class UpdateMap {
    Handle<LanceUpdateMap, lance_update_map_close> handle_;

public:
    explicit UpdateMap(LanceUpdateMap* map) : handle_(map) {}

    /// Construct from parallel key/value lists. Every key is required; a
    /// `std::nullopt` value is a native `None` entry. Duplicate keys and input
    /// order are preserved.
    UpdateMap(const std::vector<std::string>& keys,
              const std::vector<std::optional<std::string>>& values,
              bool replace)
        : handle_(nullptr) {
        if (keys.size() != values.size()) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "UpdateMap keys and values must have the same length");
        }
        std::vector<const char*> key_ptrs;
        std::vector<const char*> value_ptrs;
        key_ptrs.reserve(keys.size());
        value_ptrs.reserve(values.size());
        for (size_t i = 0; i < keys.size(); ++i) {
            key_ptrs.push_back(keys[i].c_str());
            value_ptrs.push_back(values[i] ? values[i]->c_str() : nullptr);
        }
        auto* raw = lance_update_map_new(
            key_ptrs.empty() ? nullptr : key_ptrs.data(),
            value_ptrs.empty() ? nullptr : value_ptrs.data(), keys.size(),
            replace);
        if (!raw) check_error();
        handle_ = Handle<LanceUpdateMap, lance_update_map_close>(raw);
    }

    UpdateMap(UpdateMap&&) noexcept = default;
    UpdateMap& operator=(UpdateMap&&) noexcept = default;
    UpdateMap(const UpdateMap&) = delete;
    UpdateMap& operator=(const UpdateMap&) = delete;

    size_t count() const {
        size_t count = lance_update_map_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return count;
    }

    std::string key_at(size_t index) const {
        const char* key = lance_update_map_key_at(handle_.get(), index);
        if (!key) check_error();
        return std::string(key);
    }

    std::optional<std::string> value_at(size_t index) const {
        bool present = false;
        const char* value = nullptr;
        if (lance_update_map_value_at(handle_.get(), index, &present, &value) !=
            0) {
            check_error();
        }
        if (!present) return std::nullopt;
        return std::string(value);
    }

    bool replace() const {
        bool replace = lance_update_map_replace(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return replace;
    }

    /// Clone this snapshot into an independently owned handle.
    UpdateMap clone() const {
        auto* raw = lance_update_map_clone(handle_.get());
        if (!raw) check_error();
        return UpdateMap(raw);
    }

    const LanceUpdateMap* c_handle() const { return handle_.get(); }
};

/// Options for Operation::make_update_config.
///
/// A disengaged optional maps to native `None`; an engaged empty `UpdateMap`
/// maps to native `Some(empty)`. Duplicate field IDs follow native `HashMap`
/// last-write-wins semantics where the last complete map wins.
struct UpdateConfigOptions {
    std::optional<UpdateMap> config_updates;
    std::optional<UpdateMap> table_metadata_updates;
    std::optional<UpdateMap> schema_metadata_updates;
    std::map<int32_t, UpdateMap> field_metadata_updates;
};

/// Coverage layout of a native data overlay value.
enum class OverlayCoverageKind : int32_t {
    Shared = LANCE_DATA_OVERLAY_COVERAGE_SHARED,
    PerField = LANCE_DATA_OVERLAY_COVERAGE_PER_FIELD,
};

/// Owned native `DataOverlayFile` snapshot. Move-only.
///
/// Coverage is supplied as portable 32-bit Roaring bitmap bytes. Shared
/// coverage takes exactly one bitmap; per-field coverage takes one bitmap per
/// `data_file.fields` entry, in order.
class DataOverlayFile {
    Handle<LanceDataOverlayFile, lance_data_overlay_file_close> handle_;

public:
    explicit DataOverlayFile(LanceDataOverlayFile* file) : handle_(file) {}

    DataOverlayFile(const DataFile& data_file,
                    OverlayCoverageKind coverage_kind,
                    const std::vector<std::vector<uint8_t>>& bitmaps,
                    uint64_t committed_version = 0)
        : handle_(nullptr) {
        std::vector<const uint8_t*> pointers;
        std::vector<size_t> lengths;
        pointers.reserve(bitmaps.size());
        lengths.reserve(bitmaps.size());
        for (const auto& bitmap : bitmaps) {
            pointers.push_back(bitmap.empty() ? nullptr : bitmap.data());
            lengths.push_back(bitmap.size());
        }
        auto* raw = lance_data_overlay_file_new(
            data_file.c_handle(), static_cast<int32_t>(coverage_kind),
            pointers.empty() ? nullptr : pointers.data(),
            lengths.empty() ? nullptr : lengths.data(), bitmaps.size(),
            committed_version);
        if (!raw) check_error();
        handle_ = Handle<LanceDataOverlayFile, lance_data_overlay_file_close>(
            raw);
    }

    DataOverlayFile(DataOverlayFile&&) noexcept = default;
    DataOverlayFile& operator=(DataOverlayFile&&) noexcept = default;
    DataOverlayFile(const DataOverlayFile&) = delete;
    DataOverlayFile& operator=(const DataOverlayFile&) = delete;

    /// Complete native data-file metadata owned by the overlay.
    DataFile data_file() const {
        LanceDataFile* file = lance_data_overlay_file_data_file(handle_.get());
        if (!file) check_error();
        return DataFile(file);
    }

    OverlayCoverageKind coverage_kind() const {
        int32_t kind = 0;
        if (lance_data_overlay_file_coverage_kind(
                handle_.get(), &kind) != 0) {
            check_error();
        }
        return static_cast<OverlayCoverageKind>(kind);
    }

    /// Number of coverage bitmaps: 1 for shared, field count for per-field.
    uint64_t bitmap_count() const {
        uint64_t count = 0;
        if (lance_data_overlay_file_bitmap_count(
                handle_.get(), &count) != 0) {
            check_error();
        }
        return count;
    }

    /// Copy one portable Roaring coverage bitmap into owned bytes.
    std::vector<uint8_t> bitmap_at(size_t index) const {
        uint8_t* bytes = nullptr;
        size_t len = 0;
        if (lance_data_overlay_file_bitmap_at(
                handle_.get(), index, &bytes, &len) != 0) {
            check_error();
        }
        std::vector<uint8_t> out;
        if (len != 0) out.assign(bytes, bytes + len);
        if (bytes) lance_free_bytes(bytes);
        return out;
    }

    /// Native committed-version stamp. Zero is the pre-commit placeholder.
    uint64_t committed_version() const {
        uint64_t version = 0;
        if (lance_data_overlay_file_committed_version(
                handle_.get(), &version) != 0) {
            check_error();
        }
        return version;
    }

    /// Clone this snapshot into an independently owned handle.
    DataOverlayFile clone() const {
        auto* raw = lance_data_overlay_file_clone(handle_.get());
        if (!raw) check_error();
        return DataOverlayFile(raw);
    }

    const LanceDataOverlayFile* c_handle() const { return handle_.get(); }
};

/// Owned native `DataOverlayGroup` snapshot. Move-only.
class DataOverlayGroup {
    Handle<LanceDataOverlayGroup, lance_data_overlay_group_close> handle_;

public:
    explicit DataOverlayGroup(LanceDataOverlayGroup* group) : handle_(group) {}

    /// Construct a group by cloning the given overlay values in order.
    DataOverlayGroup(uint64_t fragment_id,
                     const std::vector<const DataOverlayFile*>& overlays)
        : handle_(nullptr) {
        std::vector<const LanceDataOverlayFile*> handles;
        handles.reserve(overlays.size());
        for (const auto* overlay : overlays) {
            if (!overlay) {
                throw Error(LANCE_ERR_INVALID_ARGUMENT,
                            "data overlay file must not be null");
            }
            handles.push_back(overlay->c_handle());
        }
        auto* raw = lance_data_overlay_group_new(
            fragment_id, handles.empty() ? nullptr : handles.data(),
            handles.size());
        if (!raw) check_error();
        handle_ = Handle<LanceDataOverlayGroup, lance_data_overlay_group_close>(
            raw);
    }

    DataOverlayGroup(DataOverlayGroup&&) noexcept = default;
    DataOverlayGroup& operator=(DataOverlayGroup&&) noexcept = default;
    DataOverlayGroup(const DataOverlayGroup&) = delete;
    DataOverlayGroup& operator=(const DataOverlayGroup&) = delete;

    uint64_t fragment_id() const {
        uint64_t id = 0;
        if (lance_data_overlay_group_fragment_id(handle_.get(), &id) != 0) {
            check_error();
        }
        return id;
    }

    uint64_t overlay_count() const {
        uint64_t count = 0;
        if (lance_data_overlay_group_overlay_count(
                handle_.get(), &count) != 0) {
            check_error();
        }
        return count;
    }

    /// Clone one overlay into an independent owned value.
    DataOverlayFile overlay_at(size_t index) const {
        LanceDataOverlayFile* overlay = nullptr;
        if (lance_data_overlay_group_overlay_at(
                handle_.get(), index, &overlay) != 0) {
            check_error();
        }
        return DataOverlayFile(overlay);
    }

    /// Clone this snapshot into an independently owned handle.
    DataOverlayGroup clone() const {
        auto* raw = lance_data_overlay_group_clone(handle_.get());
        if (!raw) check_error();
        return DataOverlayGroup(raw);
    }

    const LanceDataOverlayGroup* c_handle() const { return handle_.get(); }
};

enum class OperationKind : int32_t {
    Unknown             = LANCE_OPERATION_UNKNOWN,
    Append              = LANCE_OPERATION_APPEND,
    Delete              = LANCE_OPERATION_DELETE,
    Overwrite           = LANCE_OPERATION_OVERWRITE,
    CreateIndex         = LANCE_OPERATION_CREATE_INDEX,
    Rewrite             = LANCE_OPERATION_REWRITE,
    DataReplacement     = LANCE_OPERATION_DATA_REPLACEMENT,
    DataOverlay         = LANCE_OPERATION_DATA_OVERLAY,
    Merge               = LANCE_OPERATION_MERGE,
    Restore             = LANCE_OPERATION_RESTORE,
    ReserveFragments    = LANCE_OPERATION_RESERVE_FRAGMENTS,
    Update              = LANCE_OPERATION_UPDATE,
    Project             = LANCE_OPERATION_PROJECT,
    UpdateConfig        = LANCE_OPERATION_UPDATE_CONFIG,
    UpdateMemWalState   = LANCE_OPERATION_UPDATE_MEM_WAL_STATE,
    Clone               = LANCE_OPERATION_CLONE,
    UpdateBases         = LANCE_OPERATION_UPDATE_BASES,
};

/// Owned native Operation snapshot. Move-only; closing the source Transaction
/// does not invalidate this handle.
class Operation {
    Handle<LanceOperation, lance_operation_close> handle_;

public:
    explicit Operation(LanceOperation* operation) : handle_(operation) {}

    Operation(Operation&&) noexcept = default;
    Operation& operator=(Operation&&) noexcept = default;
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;

    OperationKind kind() const {
        LanceOperationKind raw{};
        if (lance_operation_kind(handle_.get(), &raw) != 0) check_error();
        return static_cast<OperationKind>(raw);
    }

    /// Fragment count for Append/Overwrite. Other native variants throw
    /// LANCE_ERR_NOT_SUPPORTED until their typed wrappers are added.
    uint64_t fragment_count() const {
        uint64_t count = 0;
        if (lance_operation_fragment_count(handle_.get(), &count) != 0) {
            check_error();
        }
        return count;
    }

    /// Native `preserves_nullability` assertion from a Merge or Project
    /// operation. Other kinds throw LANCE_ERR_NOT_SUPPORTED.
    bool preserves_nullability() const {
        bool value = false;
        if (lance_operation_preserves_nullability(
                handle_.get(), &value) != 0) {
            check_error();
        }
        return value;
    }

    /// Number of fragments in an Append operation.
    uint64_t append_fragment_count() const {
        uint64_t count = 0;
        if (lance_operation_append_fragment_count(handle_.get(), &count) != 0) {
            check_error();
        }
        return count;
    }

    /// Clone one Append fragment as an independent detached fragment.
    Fragment append_fragment_at(size_t index) const;

    /// Target version of a Restore operation.
    uint64_t restore_version() const {
        uint64_t version = 0;
        if (lance_operation_restore_version(handle_.get(), &version) != 0) {
            check_error();
        }
        return version;
    }

    /// Reserved fragment count of a ReserveFragments operation.
    uint32_t reserve_fragments_count() const {
        uint32_t count = 0;
        if (lance_operation_reserve_fragments_count(handle_.get(), &count) != 0) {
            check_error();
        }
        return count;
    }

    /// Number of new base paths in an UpdateBases operation.
    uint64_t update_bases_count() const {
        uint64_t count = 0;
        if (lance_operation_update_bases_count(handle_.get(), &count) != 0) {
            check_error();
        }
        return count;
    }

    /// Borrowed new base path from an UpdateBases operation. The strings
    /// remain valid until this operation handle is closed.
    LanceBasePathView update_bases_at(size_t index) const {
        LanceBasePathView base{};
        if (lance_operation_update_bases_at(handle_.get(), index, &base) != 0) {
            check_error();
        }
        return base;
    }

    /// New base paths cloned from an UpdateBases operation.
    std::vector<BasePath> update_bases() const;

    /// Number of fragments in an Overwrite operation.
    uint64_t overwrite_fragment_count() const {
        uint64_t count = 0;
        if (lance_operation_overwrite_fragment_count(
                handle_.get(), &count) != 0) {
            check_error();
        }
        return count;
    }

    /// Clone one Overwrite fragment as an independent detached fragment.
    Fragment overwrite_fragment_at(size_t index) const;

    /// Clone the in-memory schema of an Overwrite operation.
    LanceSchema overwrite_schema() const;

    /// Whether the Overwrite operation carries native `Some` config upserts.
    /// `Some(empty)` is distinct from `None`.
    bool has_config_upsert_values() const {
        bool present = false;
        if (lance_operation_overwrite_has_config_upsert_values(
                handle_.get(), &present) != 0) {
            check_error();
        }
        return present;
    }

    /// Number of config upsert entries. Native `None` reports 0.
    uint64_t config_upsert_count() const {
        uint64_t count = 0;
        if (lance_operation_overwrite_config_upsert_count(
                handle_.get(), &count) != 0) {
            check_error();
        }
        return count;
    }

    /// Borrowed config upsert key by native iteration position.
    std::string config_upsert_key_at(size_t index) const {
        const char* key = lance_operation_overwrite_config_upsert_key_at(
            handle_.get(), index);
        if (!key) check_error();
        return std::string(key);
    }

    /// Borrowed config upsert value by native iteration position.
    std::string config_upsert_value_at(size_t index) const {
        const char* value = lance_operation_overwrite_config_upsert_value_at(
            handle_.get(), index);
        if (!value) check_error();
        return std::string(value);
    }

    /// Whether the Overwrite operation carries native `Some` initial bases.
    /// `Some(empty)` is distinct from `None`.
    bool has_initial_bases() const {
        bool present = false;
        if (lance_operation_overwrite_has_initial_bases(
                handle_.get(), &present) != 0) {
            check_error();
        }
        return present;
    }

    /// Number of initial base paths. Native `None` reports 0.
    uint64_t initial_base_count() const {
        uint64_t count = 0;
        if (lance_operation_overwrite_initial_base_count(
                handle_.get(), &count) != 0) {
            check_error();
        }
        return count;
    }

    /// Borrowed initial base path from an Overwrite operation. The strings
    /// remain valid until this operation handle is closed.
    LanceBasePathView initial_base_at(size_t index) const {
        LanceBasePathView base{};
        if (lance_operation_overwrite_initial_base_at(
                handle_.get(), index, &base) != 0) {
            check_error();
        }
        return base;
    }

    /// Initial base paths cloned from an Overwrite operation.
    std::vector<BasePath> initial_bases() const {
        std::vector<BasePath> out;
        const uint64_t count = initial_base_count();
        out.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            LanceBasePathView view = initial_base_at(static_cast<size_t>(i));
            BasePath base;
            base.id = view.id;
            base.path = view.path ? view.path : "";
            base.name = view.name ? std::optional<std::string>(view.name)
                                  : std::nullopt;
            base.is_dataset_root = view.is_dataset_root;
            out.push_back(std::move(base));
        }
        return out;
    }

    /// Number of fragments in a Merge operation.
    uint64_t merge_fragment_count() const {
        uint64_t count = 0;
        if (lance_operation_merge_fragment_count(handle_.get(), &count) != 0) {
            check_error();
        }
        return count;
    }

    /// Clone one Merge fragment as an independent detached fragment.
    Fragment merge_fragment_at(size_t index) const;

    /// Clone the in-memory schema of a Merge operation.
    LanceSchema merge_schema() const;

    /// Native `preserves_nullability` assertion from a Merge operation.
    bool merge_preserves_nullability() const {
        bool value = false;
        if (lance_operation_merge_preserves_nullability(
                handle_.get(), &value) != 0) {
            check_error();
        }
        return value;
    }

    /// Clone the in-memory schema of a Project operation.
    LanceSchema project_schema() const;

    /// Shallow-clone flag of a Clone operation.
    bool clone_is_shallow() const {
        bool value = false;
        if (lance_operation_clone_is_shallow(handle_.get(), &value) != 0) {
            check_error();
        }
        return value;
    }

    /// Optional native reference name of a Clone operation.
    std::optional<std::string> clone_ref_name() const {
        bool present = false;
        const char* value = nullptr;
        if (lance_operation_clone_ref_name(
                handle_.get(), &present, &value) != 0) {
            check_error();
        }
        if (!present) return std::nullopt;
        return std::string(value);
    }

    /// Native reference version of a Clone operation.
    uint64_t clone_ref_version() const {
        uint64_t version = 0;
        if (lance_operation_clone_ref_version(handle_.get(), &version) != 0) {
            check_error();
        }
        return version;
    }

    /// Borrowed native reference path of a Clone operation.
    std::string clone_ref_path() const {
        const char* path = lance_operation_clone_ref_path(handle_.get());
        if (!path) check_error();
        return std::string(path);
    }

    /// Optional native branch name of a Clone operation.
    std::optional<std::string> clone_branch_name() const {
        bool present = false;
        const char* value = nullptr;
        if (lance_operation_clone_branch_name(
                handle_.get(), &present, &value) != 0) {
            check_error();
        }
        if (!present) return std::nullopt;
        return std::string(value);
    }

    /// Optional native config updates map from an UpdateConfig operation.
    std::optional<UpdateMap> config_updates() const {
        LanceUpdateMap* map = nullptr;
        if (lance_operation_update_config_config_updates(
                handle_.get(), &map) != 0) {
            check_error();
        }
        if (!map) return std::nullopt;
        return UpdateMap(map);
    }

    /// Optional native table metadata updates map.
    std::optional<UpdateMap> table_metadata_updates() const {
        LanceUpdateMap* map = nullptr;
        if (lance_operation_update_config_table_metadata_updates(
                handle_.get(), &map) != 0) {
            check_error();
        }
        if (!map) return std::nullopt;
        return UpdateMap(map);
    }

    /// Optional native schema metadata updates map.
    std::optional<UpdateMap> schema_metadata_updates() const {
        LanceUpdateMap* map = nullptr;
        if (lance_operation_update_config_schema_metadata_updates(
                handle_.get(), &map) != 0) {
            check_error();
        }
        if (!map) return std::nullopt;
        return UpdateMap(map);
    }

    /// Per-field native metadata update maps keyed by field ID.
    std::map<int32_t, UpdateMap> field_metadata_updates() const {
        uint64_t count = 0;
        if (lance_operation_update_config_field_metadata_count(
                handle_.get(), &count) != 0) {
            check_error();
        }
        std::map<int32_t, UpdateMap> out;
        for (uint64_t i = 0; i < count; ++i) {
            int32_t field_id = 0;
            LanceUpdateMap* map = nullptr;
            if (lance_operation_update_config_field_metadata_at(
                    handle_.get(), static_cast<size_t>(i), &field_id, &map) !=
                0) {
                check_error();
            }
            if (!map) {
                throw Error(LANCE_ERR_INTERNAL,
                            "UpdateConfig field metadata returned a null map");
            }
            out.emplace(field_id, UpdateMap(map));
        }
        return out;
    }

    /// Construct an owned native Overwrite operation from a native schema and
    /// borrowed fragment handles. Fragment metadata is copied. Native commit
    /// assigns fresh fragment IDs and rejects fragments with deletion files.
    /// `config_upsert_values == std::nullopt` maps to native `None`; an
    /// engaged empty vector maps to `Some(empty)`. `initial_bases` follows the
    /// same presence mapping.
    static Operation make_overwrite(
        const LanceSchema& schema,
        const std::vector<const Fragment*>& fragments,
        std::optional<std::vector<std::pair<std::string, std::string>>>
            config_upsert_values = std::nullopt,
        std::optional<std::vector<BasePath>> initial_bases = std::nullopt);

    /// Construct an owned native Merge operation. `fragments` is the final
    /// native fragment list. `preserves_nullability` is stored unchanged as
    /// the caller's assertion; native commit owns schema-change and conflict
    /// validation.
    static Operation make_merge(
        const LanceSchema& schema,
        const std::vector<const Fragment*>& fragments,
        bool preserves_nullability = false);

    /// Construct an owned native Clone operation. `ref_name == std::nullopt`
    /// maps to native `None`; an engaged empty string is `Some("")`.
    /// `branch_name` follows the same mapping. Native commit owns shallow
    /// publication and deep-clone rejection.
    static Operation make_clone(
        bool is_shallow,
        std::optional<std::string> ref_name,
        uint64_t ref_version,
        std::string ref_path,
        std::optional<std::string> branch_name);

    /// Construct an owned native UpdateConfig operation. Every map is cloned;
    /// native commit applies replacement and incremental semantics.
    static Operation make_update_config(const UpdateConfigOptions& options);

    /// Construct an owned native Append operation. Fragment handles are
    /// borrowed and their complete native metadata is copied.
    static Operation make_append(const std::vector<const Fragment*>& fragments);

    /// Construct an owned native Restore operation.
    static Operation make_restore(uint64_t version) {
        auto* operation = lance_operation_restore_new(version);
        if (!operation) check_error();
        return Operation(operation);
    }

    /// Construct an owned native ReserveFragments operation.
    static Operation make_reserve_fragments(uint32_t num_fragments) {
        auto* operation = lance_operation_reserve_fragments_new(num_fragments);
        if (!operation) check_error();
        return Operation(operation);
    }

    /// Construct an owned native UpdateBases operation. Native commit
    /// validation owns conflict detection and zero-ID allocation.
    static Operation make_update_bases(const std::vector<BasePath>& new_bases);

    /// Construct an owned native Delete operation. The fragment handles are
    /// borrowed and their complete native metadata is copied. Deleted IDs and
    /// the predicate are copied. Native commit validation remains authoritative.
    static Operation make_delete(
        const std::vector<const Fragment*>& updated_fragments,
        const std::vector<uint64_t>& deleted_fragment_ids,
        const std::string& predicate);

    /// Construct an owned native CreateIndex operation. Metadata handles are
    /// borrowed and their complete native values are copied. Native
    /// `Operation::CreateIndex` owns replacement, name and coverage semantics.
    static Operation make_create_index(
        const std::vector<const IndexSegmentMetadata*>& new_indices,
        const std::vector<const IndexSegmentMetadata*>& removed_indices);

    /// Construct an owned native DataReplacement operation. Group handles are
    /// borrowed and copied in input order. Native commit validation remains
    /// authoritative.
    static Operation make_data_replacement(
        const std::vector<const DataReplacementGroup*>& replacements);

    /// Construct an owned native DataOverlay operation. Group handles are
    /// borrowed and cloned in input order. Native commit validation remains
    /// authoritative for fragment existence, coverage and layout.
    static Operation make_data_overlay(
        const std::vector<const DataOverlayGroup*>& groups);

    /// Construct an owned native Project operation from a native schema.
    /// `preserves_nullability` is stored unchanged as the caller's explicit
    /// assertion that the projection does not tighten nullability; conflict
    /// and rebase semantics belong to Lance core. The schema is cloned.
    static Operation make_project(const LanceSchema& schema,
                                  bool preserves_nullability = false);

    /// New-index metadata cloned from a CreateIndex operation. Each returned
    /// value is independent of this operation handle.
    std::vector<IndexSegmentMetadata> create_index_new() const;

    /// Removed-index metadata cloned from a CreateIndex operation.
    std::vector<IndexSegmentMetadata> create_index_removed() const;

    /// Number of data replacement groups.
    uint64_t data_replacement_count() const {
        uint64_t count = 0;
        if (lance_operation_data_replacement_count(
                handle_.get(), &count) != 0) {
            check_error();
        }
        return count;
    }

    /// Clone one data replacement group.
    DataReplacementGroup data_replacement_at(size_t index) const {
        LanceDataReplacementGroup* group = nullptr;
        if (lance_operation_data_replacement_at(
                handle_.get(), index, &group) != 0) {
            check_error();
        }
        return DataReplacementGroup(group);
    }

    /// Number of data overlay groups.
    uint64_t data_overlay_group_count() const {
        uint64_t count = 0;
        if (lance_operation_data_overlay_group_count(
                handle_.get(), &count) != 0) {
            check_error();
        }
        return count;
    }

    /// Clone one data overlay group.
    DataOverlayGroup data_overlay_group_at(size_t index) const {
        LanceDataOverlayGroup* group = nullptr;
        if (lance_operation_data_overlay_group_at(
                handle_.get(), index, &group) != 0) {
            check_error();
        }
        return DataOverlayGroup(group);
    }

    /// Number of updated fragments in a Delete operation.
    uint64_t delete_updated_fragment_count() const {
        uint64_t count = 0;
        if (lance_operation_delete_updated_fragment_count(
                handle_.get(), &count) != 0) {
            check_error();
        }
        return count;
    }

    /// Native `Fragment` metadata JSON for one updated Delete fragment.
    std::string delete_updated_fragment_metadata_json(size_t index) const {
        const char* raw = lance_operation_delete_updated_fragment_metadata_json_at(
            handle_.get(), index);
        if (!raw) check_error();
        std::string out(raw);
        lance_free_string(raw);
        return out;
    }

    /// Number of fully deleted fragment IDs in a Delete operation.
    uint64_t delete_deleted_fragment_count() const {
        uint64_t count = 0;
        if (lance_operation_delete_deleted_fragment_count(
                handle_.get(), &count) != 0) {
            check_error();
        }
        return count;
    }

    /// One fully deleted fragment ID from a Delete operation.
    uint64_t delete_deleted_fragment_id(size_t index) const {
        uint64_t id = 0;
        if (lance_operation_delete_deleted_fragment_id_at(
                handle_.get(), index, &id) != 0) {
            check_error();
        }
        return id;
    }

    /// Delete predicate captured by the native operation.
    std::string delete_predicate() const {
        const char* raw = lance_operation_delete_predicate(handle_.get());
        if (!raw) check_error();
        std::string out(raw);
        lance_free_string(raw);
        return out;
    }

    const LanceOperation* c_handle() const { return handle_.get(); }
};

/// Options for Transaction::from_operation.
///
/// `uuid == std::nullopt` requests native UUID generation; an engaged empty
/// string is passed through. `tag == std::nullopt` maps to native None; an
/// engaged empty string maps to Some(""). An engaged empty properties vector
/// maps to native Some(empty), while nullopt maps to None.
struct TransactionOptions {
    std::optional<std::string> uuid;
    std::optional<std::string> tag;
    std::optional<std::vector<std::pair<std::string, std::string>>>
        transaction_properties;
};

/// Owned native Transaction snapshot. Move-only and independent of the dataset
/// or writer that produced it.
class Transaction {
    Handle<LanceTransaction, lance_transaction_close> handle_;

public:
    explicit Transaction(LanceTransaction* transaction) : handle_(transaction) {}

    Transaction(Transaction&&) noexcept = default;
    Transaction& operator=(Transaction&&) noexcept = default;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    uint64_t read_version() const {
        uint64_t version = 0;
        if (lance_transaction_read_version(handle_.get(), &version) != 0) {
            check_error();
        }
        return version;
    }

    std::string uuid() const {
        const char* uuid = lance_transaction_uuid(handle_.get());
        if (!uuid) check_error();
        std::string out(uuid);
        lance_free_string(uuid);
        return out;
    }

    std::optional<std::string> tag() const {
        const char* tag = nullptr;
        int32_t present = lance_transaction_tag(handle_.get(), &tag);
        if (present < 0) check_error();
        if (present == 0) {
            if (tag) {
                lance_free_string(tag);
                throw Error(LANCE_ERR_INTERNAL,
                            "lance_transaction_tag reported absent with a non-null pointer");
            }
            return std::nullopt;
        }
        if (!tag) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_transaction_tag reported present with a null pointer");
        }
        std::string out(tag);
        lance_free_string(tag);
        return out;
    }

    bool has_properties() const {
        bool present = false;
        if (lance_transaction_has_properties(handle_.get(), &present) != 0) {
            check_error();
        }
        return present;
    }

    std::map<std::string, std::string> properties() const {
        uint64_t count = 0;
        if (lance_transaction_property_count(handle_.get(), &count) != 0) {
            check_error();
        }

        std::map<std::string, std::string> out;
        for (uint64_t i = 0; i < count; ++i) {
            const char* key = lance_transaction_property_key_at(
                handle_.get(), static_cast<size_t>(i));
            if (!key) check_error();
            const char* value = lance_transaction_property_value_at(
                handle_.get(), static_cast<size_t>(i));
            if (!value) {
                lance_free_string(key);
                check_error();
            }
            out.emplace(key, value);
            lance_free_string(key);
            lance_free_string(value);
        }
        return out;
    }

    Operation operation() const {
        LanceOperation* raw = nullptr;
        if (lance_transaction_operation(handle_.get(), &raw) != 0) {
            check_error();
        }
        if (!raw) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_transaction_operation returned success with null output");
        }
        return Operation(raw);
    }

    /// Construct a native transaction from a copied operation.
    static Transaction from_operation(
        uint64_t read_version,
        const Operation& operation,
        const TransactionOptions& options = {}) {
        std::vector<const char*> property_keys;
        std::vector<const char*> property_values;
        LanceTransactionOptions raw{};
        if (options.uuid) raw.uuid = options.uuid->c_str();
        if (options.tag) raw.tag = options.tag->c_str();
        if (options.transaction_properties) {
            const auto& entries = *options.transaction_properties;
            property_keys.reserve(entries.size());
            property_values.reserve(entries.size());
            for (const auto& [key, value] : entries) {
                property_keys.push_back(key.c_str());
                property_values.push_back(value.c_str());
            }
            raw.has_transaction_properties = true;
            raw.property_keys =
                property_keys.empty() ? nullptr : property_keys.data();
            raw.property_values =
                property_values.empty() ? nullptr : property_values.data();
            raw.property_count = entries.size();
        }

        auto* transaction = lance_transaction_new(
            read_version, operation.c_handle(), &raw);
        if (!transaction) check_error();
        return Transaction(transaction);
    }

    /// Serialize with the selected core's native protobuf conversion.
    std::vector<uint8_t> to_proto() const {
        uint8_t* bytes = nullptr;
        size_t len = 0;
        if (lance_transaction_to_proto(handle_.get(), &bytes, &len) != 0) {
            check_error();
        }
        if (len > 0 && !bytes) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_transaction_to_proto returned a null buffer");
        }
        std::vector<uint8_t> out;
        if (len > 0) out.assign(bytes, bytes + len);
        lance_free_bytes(bytes);
        return out;
    }

    /// Parse one complete native protobuf Transaction.
    static Transaction from_proto(const std::vector<uint8_t>& bytes) {
        auto* raw = lance_transaction_from_proto(bytes.data(), bytes.size());
        if (!raw) check_error();
        return Transaction(raw);
    }

    const LanceTransaction* c_handle() const { return handle_.get(); }
};

/// One native `Dataset::update_config` entry. `value == std::nullopt` removes
/// the key; otherwise the value is upserted.
using ConfigUpdate = std::pair<std::string, std::optional<std::string>>;

/// Optional native transaction properties for one base registration.
///
/// `std::nullopt` maps to native `None`; an engaged empty vector maps to
/// native `Some(empty map)`.
struct AddBasesOptions {
    std::optional<std::vector<std::pair<std::string, std::string>>>
        transaction_properties;
};

/// Owned progress callback for tracked-file enumeration. `manifests_total` is
/// unset until native progress has a known total. The callback must not throw;
/// it runs on a native background task.
using TrackedFilesProgressCallback = std::function<void(
    uint64_t manifests_processed,
    std::optional<uint64_t> manifests_total)>;

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

// ─── Schema-evolution callbacks ─────────────────────────────────────────────

/// Native checkpoint batch identity. Mapper and checkpoint callbacks receive a
/// borrowed value.
struct BatchUDFInfo {
    uint32_t fragment_id = 0;
    std::size_t batch_index = 0;
};

/// Mapper callback. The input Arrow values are borrowed; `output` is an
/// uninitialized ArrowArray that the callback must initialize with owned data
/// under the declared BatchUDF output schema. Throw to fail.
using BatchUDFMapper = std::function<void(
    const ArrowArray& input,
    const ArrowSchema& input_schema,
    ArrowArray& output)>;

/// Optional native UDF checkpoint store.
///
/// `get_batch` returns true on a hit, false on a miss, and throws on error.
/// `insert_batch` receives mutable owned Arrow values: it may move both structs
/// into its storage and clear the source release callbacks. The library
/// releases any ownership left in those arguments after the callback returns.
/// `get_fragment` returns opaque native Fragment JSON or std::nullopt on a
/// miss. `insert_fragment` receives the native u64 Fragment ID and borrowed
/// JSON. All four callbacks are required when the store is engaged.
struct UDFCheckpointStore {
    std::function<bool(
        const BatchUDFInfo&,
        ArrowArray&)> get_batch;

    std::function<void(
        const BatchUDFInfo&,
        ArrowArray&,
        ArrowSchema&)> insert_batch;

    std::function<std::optional<std::string>(uint32_t)> get_fragment;

    std::function<void(uint64_t, const std::string&)> insert_fragment;
};

/// Native BatchUDF declaration. `output_schema` is the complete schema of every
/// returned batch and remains borrowed for the enclosing add-columns call.
struct BatchUDF {
    BatchUDFMapper mapper;
    const ArrowSchema* output_schema = nullptr;
    std::optional<UDFCheckpointStore> checkpoint_store;
};

namespace callback_detail {

inline void release_owned_string(LanceOwnedString* value) noexcept {
    if (value == nullptr) return;
    std::free(value->data);
    value->data = nullptr;
    value->release = nullptr;
}

inline void set_callback_error(
    LanceCallbackError* target,
    int32_t code,
    const char* message) noexcept {
    if (target == nullptr) return;
    target->code = code;
    target->message.data = nullptr;
    target->message.release = nullptr;
    if (message == nullptr) return;

    const std::size_t len = std::strlen(message);
    char* copy = static_cast<char*>(std::malloc(len + 1));
    if (copy == nullptr) {
        return;
    }
    std::memcpy(copy, message, len + 1);
    target->message.data = copy;
    target->message.release = &release_owned_string;
}

inline void release_array_after_exception(ArrowArray* value) noexcept {
    if (value != nullptr && value->release != nullptr) {
        value->release(value);
    }
}

struct MapperContext {
    const BatchUDF* udf;
};

struct CheckpointContext {
    const UDFCheckpointStore* store;
};

inline int32_t mapper_trampoline(
    void* callback_ctx,
    const ArrowArray* input,
    const ArrowSchema* input_schema,
    ArrowArray* output,
    LanceCallbackError* error) noexcept {
    try {
        auto* context = static_cast<MapperContext*>(callback_ctx);
        context->udf->mapper(*input, *input_schema, *output);
        return 0;
    } catch (const lance::Error& exception) {
        release_array_after_exception(output);
        set_callback_error(error, static_cast<int32_t>(exception.code), exception.what());
        return -1;
    } catch (const std::exception& exception) {
        release_array_after_exception(output);
        set_callback_error(error, LANCE_ERR_INTERNAL, exception.what());
        return -1;
    } catch (...) {
        release_array_after_exception(output);
        set_callback_error(error, LANCE_ERR_INTERNAL, "unknown C++ exception");
        return -1;
    }
}

inline int32_t get_batch_trampoline(
    void* callback_ctx,
    const LanceUDFBatchInfo* raw_info,
    ArrowArray* output,
    LanceCallbackError* error) noexcept {
    try {
        auto* context = static_cast<CheckpointContext*>(callback_ctx);
        BatchUDFInfo info{raw_info->fragment_id, raw_info->batch_index};
        return context->store->get_batch(info, *output) ? 1 : 0;
    } catch (const lance::Error& exception) {
        release_array_after_exception(output);
        set_callback_error(error, static_cast<int32_t>(exception.code), exception.what());
        return -1;
    } catch (const std::exception& exception) {
        release_array_after_exception(output);
        set_callback_error(error, LANCE_ERR_INTERNAL, exception.what());
        return -1;
    } catch (...) {
        release_array_after_exception(output);
        set_callback_error(error, LANCE_ERR_INTERNAL, "unknown C++ exception");
        return -1;
    }
}

inline int32_t insert_batch_trampoline(
    void* callback_ctx,
    const LanceUDFBatchInfo* raw_info,
    ArrowArray* batch,
    ArrowSchema* batch_schema,
    LanceCallbackError* error) noexcept {
    try {
        auto* context = static_cast<CheckpointContext*>(callback_ctx);
        BatchUDFInfo info{raw_info->fragment_id, raw_info->batch_index};
        context->store->insert_batch(info, *batch, *batch_schema);
        return 0;
    } catch (const lance::Error& exception) {
        set_callback_error(error, static_cast<int32_t>(exception.code), exception.what());
        return -1;
    } catch (const std::exception& exception) {
        set_callback_error(error, LANCE_ERR_INTERNAL, exception.what());
        return -1;
    } catch (...) {
        set_callback_error(error, LANCE_ERR_INTERNAL, "unknown C++ exception");
        return -1;
    }
}

inline int32_t get_fragment_trampoline(
    void* callback_ctx,
    uint32_t fragment_id,
    LanceOwnedString* out_json,
    LanceCallbackError* error) noexcept {
    try {
        auto* context = static_cast<CheckpointContext*>(callback_ctx);
        auto json = context->store->get_fragment(fragment_id);
        if (!json) return 0;

        char* copy = static_cast<char*>(std::malloc(json->size() + 1));
        if (copy == nullptr) {
            set_callback_error(
                error, LANCE_ERR_INTERNAL, "failed to allocate callback JSON");
            return -1;
        }
        std::memcpy(copy, json->data(), json->size());
        copy[json->size()] = '\0';
        out_json->data = copy;
        out_json->release = &release_owned_string;
        return 1;
    } catch (const lance::Error& exception) {
        set_callback_error(error, static_cast<int32_t>(exception.code), exception.what());
        return -1;
    } catch (const std::exception& exception) {
        set_callback_error(error, LANCE_ERR_INTERNAL, exception.what());
        return -1;
    } catch (...) {
        set_callback_error(error, LANCE_ERR_INTERNAL, "unknown C++ exception");
        return -1;
    }
}

inline int32_t insert_fragment_trampoline(
    void* callback_ctx,
    uint64_t fragment_id,
    const char* fragment_json,
    LanceCallbackError* error) noexcept {
    try {
        auto* context = static_cast<CheckpointContext*>(callback_ctx);
        context->store->insert_fragment(
            fragment_id, std::string(fragment_json == nullptr ? "" : fragment_json));
        return 0;
    } catch (const lance::Error& exception) {
        set_callback_error(error, static_cast<int32_t>(exception.code), exception.what());
        return -1;
    } catch (const std::exception& exception) {
        set_callback_error(error, LANCE_ERR_INTERNAL, exception.what());
        return -1;
    } catch (...) {
        set_callback_error(error, LANCE_ERR_INTERNAL, "unknown C++ exception");
        return -1;
    }
}

}  // namespace callback_detail

// ─── Commit lock ─────────────────────────────────────────────────────────────

/// One acquired commit lease.
///
/// `release` mirrors native `CommitLease::release`: `success` is true after the
/// manifest is published and false when the attempt is abandoned. It may be
/// called more than once and must be idempotent. Throw `lance::Error` to report
/// a specific code; any other exception maps to LANCE_ERR_INTERNAL. The object
/// is destroyed exactly once, after the final release callback.
class CommitLease {
public:
    virtual ~CommitLease() noexcept = default;
    virtual void release(bool success) = 0;
};

namespace commit_lock_detail {
struct State;
}  // namespace commit_lock_detail

class CommitLock {
    Handle<LanceCommitLock, lance_commit_lock_close> handle_;
    std::unique_ptr<commit_lock_detail::State> state_;

public:
    /// Acquire a lease for a manifest base version. Returning `nullptr` is a
    /// stateless lock. Throw `lance::Error` to report a specific code.
    using Acquire = std::function<std::unique_ptr<CommitLease>(uint64_t)>;

    explicit CommitLock(Acquire acquire);
    ~CommitLock();

    CommitLock(CommitLock&&) noexcept;
    CommitLock& operator=(CommitLock&&) noexcept;
    CommitLock(const CommitLock&) = delete;
    CommitLock& operator=(const CommitLock&) = delete;

    const LanceCommitLock* c_handle() const { return handle_.get(); }
};

namespace commit_lock_detail {

struct State {
    CommitLock::Acquire acquire;
};

struct LeaseHolder {
    std::unique_ptr<CommitLease> lease;
};

extern "C" inline int32_t acquire_trampoline(
    void* callback_ctx,
    uint64_t version,
    void** out_lease,
    LanceCallbackError* error) noexcept {
    if (out_lease == nullptr) {
        callback_detail::set_callback_error(
            error, LANCE_ERR_INVALID_ARGUMENT, "out_lease must not be null");
        return -1;
    }
    *out_lease = nullptr;
    auto* state = static_cast<State*>(callback_ctx);
    try {
        auto lease = state->acquire(version);
        if (lease) {
            *out_lease = new LeaseHolder{std::move(lease)};
        }
        return 0;
    } catch (const lance::Error& exception) {
        callback_detail::set_callback_error(
            error, static_cast<int32_t>(exception.code), exception.what());
        return -1;
    } catch (const std::exception& exception) {
        callback_detail::set_callback_error(
            error, LANCE_ERR_INTERNAL, exception.what());
        return -1;
    } catch (...) {
        callback_detail::set_callback_error(
            error, LANCE_ERR_INTERNAL, "unknown C++ exception");
        return -1;
    }
}

extern "C" inline int32_t release_trampoline(
    void*,
    void* lease,
    bool success,
    LanceCallbackError* error) noexcept {
    if (lease == nullptr) {
        return 0;
    }
    auto* holder = static_cast<LeaseHolder*>(lease);
    try {
        holder->lease->release(success);
        return 0;
    } catch (const lance::Error& exception) {
        callback_detail::set_callback_error(
            error, static_cast<int32_t>(exception.code), exception.what());
        return -1;
    } catch (const std::exception& exception) {
        callback_detail::set_callback_error(
            error, LANCE_ERR_INTERNAL, exception.what());
        return -1;
    } catch (...) {
        callback_detail::set_callback_error(
            error, LANCE_ERR_INTERNAL, "unknown C++ exception");
        return -1;
    }
}

extern "C" inline void destroy_lease_trampoline(void*, void* lease) noexcept {
    delete static_cast<LeaseHolder*>(lease);
}

extern "C" inline void release_context_trampoline(void* callback_ctx) noexcept {
    delete static_cast<State*>(callback_ctx);
}

}  // namespace commit_lock_detail

inline CommitLock::CommitLock(Acquire acquire)
    : handle_(nullptr),
      state_(new commit_lock_detail::State{std::move(acquire)}) {
    LanceCommitLockCallbacks callbacks = {};
    callbacks.lock = &commit_lock_detail::acquire_trampoline;
    callbacks.release = &commit_lock_detail::release_trampoline;
    callbacks.destroy_lease = &commit_lock_detail::destroy_lease_trampoline;
    callbacks.callback_ctx = state_.get();
    callbacks.release_context = &commit_lock_detail::release_context_trampoline;

    auto* raw = lance_commit_lock_new(&callbacks);
    // On failure the library never takes context ownership, so `state_`
    // remains local and is destroyed here.
    if (raw == nullptr) check_error();
    handle_ = Handle<LanceCommitLock, lance_commit_lock_close>(raw);
    // On success the native context owns the state and releases it exactly
    // once when the last owner drops.
    (void)state_.release();
}

inline CommitLock::~CommitLock() = default;

inline CommitLock::CommitLock(CommitLock&&) noexcept = default;

inline CommitLock& CommitLock::operator=(CommitLock&&) noexcept = default;

// ─── Storage options ─────────────────────────────────────────────────────────

using StorageOptions = std::map<std::string, std::string>;

class StorageOptionsProvider {
    Handle<LanceStorageOptionsProvider, lance_storage_options_provider_close> handle_;

public:
    using Fetch = std::function<std::optional<StorageOptions>()>;

    StorageOptionsProvider(
        std::string provider_id,
        Fetch fetch,
        Fetch force_fetch = {});

    const LanceStorageOptionsProvider* c_handle() const { return handle_.get(); }

    std::string provider_id() const {
        char* value = lance_storage_options_provider_id(handle_.get());
        if (value == nullptr) check_error();
        std::string out(value);
        lance_free_string(value);
        return out;
    }
};

namespace storage_options_detail {

struct FetchSnapshot {
    std::vector<std::string> strings;
    std::vector<const char*> keys;
    std::vector<const char*> values;
};

inline void release_fetch_snapshot(void* raw) noexcept {
    delete static_cast<FetchSnapshot*>(raw);
}

inline int32_t fetch_impl(
    const StorageOptionsProvider::Fetch& fetch,
    LanceStorageOptionsFetchResult* out,
    LanceCallbackError* error,
    const char* context) noexcept {
    (void)context;
    try {
        auto options = fetch();
        if (!options) {
            out->has_options = 0;
            return 0;
        }

        std::unique_ptr<FetchSnapshot> snapshot(new FetchSnapshot());
        snapshot->strings.reserve(options->size() * 2);
        snapshot->keys.reserve(options->size());
        snapshot->values.reserve(options->size());
        for (const auto& [key, value] : *options) {
            snapshot->strings.push_back(key);
            snapshot->strings.push_back(value);
        }
        for (std::size_t index = 0; index < options->size(); index++) {
            snapshot->keys.push_back(snapshot->strings[index * 2].c_str());
            snapshot->values.push_back(snapshot->strings[index * 2 + 1].c_str());
        }
        out->has_options = 1;
        out->keys = snapshot->keys.data();
        out->values = snapshot->values.data();
        out->count = snapshot->keys.size();
        out->snapshot_ctx = snapshot.release();
        out->release_snapshot = &release_fetch_snapshot;
        return 0;
    } catch (const lance::Error& exception) {
        callback_detail::set_callback_error(
            error, static_cast<int32_t>(exception.code), exception.what());
        return -1;
    } catch (const std::exception& exception) {
        callback_detail::set_callback_error(error, LANCE_ERR_INTERNAL, exception.what());
        return -1;
    } catch (...) {
        callback_detail::set_callback_error(
            error, LANCE_ERR_INTERNAL, "unknown C++ exception in storage options fetch");
        return -1;
    }
}

struct ProviderContext {
    StorageOptionsProvider::Fetch fetch;
    StorageOptionsProvider::Fetch force_fetch;
};

inline int32_t fetch_callback(
    void* callback_ctx,
    LanceStorageOptionsFetchResult* out,
    LanceCallbackError* error) noexcept {
    auto* context = static_cast<ProviderContext*>(callback_ctx);
    return fetch_impl(context->fetch, out, error, "storage options fetch");
}

inline int32_t force_fetch_callback(
    void* callback_ctx,
    LanceStorageOptionsFetchResult* out,
    LanceCallbackError* error) noexcept {
    auto* context = static_cast<ProviderContext*>(callback_ctx);
    return fetch_impl(
        context->force_fetch, out, error, "storage options force fetch");
}

inline void provider_release(void* callback_ctx) noexcept {
    delete static_cast<ProviderContext*>(callback_ctx);
}

inline LanceStorageOptionsProvider* create_provider(
    const std::string& provider_id,
    StorageOptionsProvider::Fetch fetch,
    StorageOptionsProvider::Fetch force_fetch) {
    auto* context = new ProviderContext{
        std::move(fetch), std::move(force_fetch)};
    LanceStorageOptionsProviderSpec spec{};
    spec.fetch = &fetch_callback;
    spec.force_fetch =
        context->force_fetch ? &force_fetch_callback : nullptr;
    spec.release = &provider_release;
    spec.provider_ctx = context;
    spec.provider_id = provider_id.c_str();
    auto* provider = lance_storage_options_provider_new(&spec);
    if (provider == nullptr) {
        // Ownership transfers only on success.
        delete context;
    }
    return provider;
}

}  // namespace storage_options_detail

inline StorageOptionsProvider::StorageOptionsProvider(
    std::string provider_id,
    Fetch fetch,
    Fetch force_fetch)
    : handle_(storage_options_detail::create_provider(
          provider_id, std::move(fetch), std::move(force_fetch))) {
    if (!handle_) check_error();
}

class StorageOptionsAccessor {
    Handle<LanceStorageOptionsAccessor, lance_storage_options_accessor_close> handle_;

public:
    /// NULL optional means native None; an engaged empty map means Some(empty).
    explicit StorageOptionsAccessor(
        std::optional<StorageOptions> initial = std::nullopt,
        const StorageOptionsProvider* provider = nullptr)
        : handle_(nullptr) {
        std::vector<const char*> keys;
        std::vector<const char*> values;
        LanceStorageOptionsInput input{};
        const LanceStorageOptionsInput* input_ptr = nullptr;
        if (initial) {
            keys.reserve(initial->size());
            values.reserve(initial->size());
            for (const auto& [key, value] : *initial) {
                keys.push_back(key.c_str());
                values.push_back(value.c_str());
            }
            input.keys = keys.data();
            input.values = values.data();
            input.count = keys.size();
            input_ptr = &input;
        }
        auto* raw = lance_storage_options_accessor_new(
            input_ptr, provider ? provider->c_handle() : nullptr);
        if (raw == nullptr) check_error();
        handle_ = Handle<LanceStorageOptionsAccessor,
                         lance_storage_options_accessor_close>(raw);
    }

    /// Resolve with native cache and expiration behavior.
    std::optional<StorageOptions> snapshot() const {
        auto* raw = lance_storage_options_accessor_snapshot(handle_.get());
        if (raw == nullptr) check_error();
        Handle<LanceStorageOptionsSnapshot,
               lance_storage_options_snapshot_close>
            snapshot(raw);
        int32_t has_options =
            lance_storage_options_snapshot_has_options(snapshot.get());
        if (has_options < 0) check_error();
        if (has_options == 0) return std::nullopt;
        StorageOptions options;
        uint64_t count =
            lance_storage_options_snapshot_count(snapshot.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        for (uint64_t index = 0; index < count; index++) {
            const char* key = lance_storage_options_snapshot_key_at(
                snapshot.get(), static_cast<std::size_t>(index));
            if (!key) check_error();
            const char* value = lance_storage_options_snapshot_value_at(
                snapshot.get(), static_cast<std::size_t>(index));
            if (!value) check_error();
            options.emplace(key, value);
        }
        return options;
    }

    const LanceStorageOptionsAccessor* c_handle() const { return handle_.get(); }
};

/// Additive options for `Dataset::open_with_options`.
///
/// Zero-initialization opens the latest version with native storage defaults.
/// A NULL accessor selects native defaults; a non-NULL accessor is retained by
/// the native dataset and later I/O uses its provider, refresh and error
/// behavior. Static options belong in the accessor's initial options.
struct DatasetOpenOptions {
    uint64_t version = 0;
    const StorageOptionsAccessor* storage_options_accessor = nullptr;
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

// ─── Native tokenization and process diagnostics ────────────────────────────

/// One token produced by the native Lance analyzer chain.
struct FtsToken {
    std::string text;
    uint32_t position = 0;
};

/// Owned token list produced by `FtsTokenList::tokenize`.
///
/// The list owns every token string. `at()` copies one token into a value, so
/// the value remains usable after the list is destroyed.
class FtsTokenList {
    Handle<LanceFtsTokenList, lance_fts_token_list_close> handle_;

    explicit FtsTokenList(LanceFtsTokenList* tokens) : handle_(tokens) {}

public:
    FtsTokenList(FtsTokenList&&) noexcept = default;
    FtsTokenList& operator=(FtsTokenList&&) noexcept = default;
    FtsTokenList(const FtsTokenList&) = delete;
    FtsTokenList& operator=(const FtsTokenList&) = delete;

    /// Tokenize an explicit UTF-8 byte range with native defaults or native
    /// `InvertedIndexParams` JSON. Embedded NUL bytes are preserved. A null
    /// `params_json` selects native defaults.
    static FtsTokenList tokenize(
        std::string_view text,
        std::optional<std::string> params_json = std::nullopt) {
        auto* tokens = lance_fts_tokenize(
            text.data(), text.size(),
            params_json ? params_json->c_str() : nullptr);
        if (!tokens) check_error();
        return FtsTokenList(tokens);
    }

    /// Number of tokens. Throws on a NULL handle.
    size_t size() const {
        uint64_t count = lance_fts_token_list_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return static_cast<size_t>(count);
    }

    bool empty() const { return size() == 0; }

    /// Copy token `index`. Throws on an out-of-range index.
    FtsToken at(size_t index) const {
        LanceFtsTokenView view{};
        if (lance_fts_token_list_get(handle_.get(), index, &view) != 0)
            check_error();
        if (view.text == nullptr && view.text_len != 0)
            throw std::runtime_error(
                "native token view has NULL text with a nonzero length");
        return FtsToken{
            std::string(view.text_len == 0 ? "" : view.text, view.text_len),
            view.position};
    }

    /// Copy all tokens in native order.
    std::vector<FtsToken> tokens() const {
        std::vector<FtsToken> out;
        size_t count = size();
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) out.push_back(at(i));
        return out;
    }

    const LanceFtsTokenList* c_handle() const { return handle_.get(); }
};

/// Native process-wide object-store IOPS counter, with no reset semantics.
inline uint64_t process_iops_counter() {
    uint64_t value = lance_process_iops_counter();
    if (lance_last_error_code() != LANCE_OK) check_error();
    return value;
}

/// Native process-wide object-store bytes-read counter, with no reset
/// semantics.
inline uint64_t process_bytes_read_counter() {
    uint64_t value = lance_process_bytes_read_counter();
    if (lance_last_error_code() != LANCE_OK) check_error();
    return value;
}

/// One read of both native process counters.
struct ProcessIoCounters {
    uint64_t iops = 0;
    uint64_t bytes_read = 0;
};

inline ProcessIoCounters process_io_counters() {
    ProcessIoCounters counters;
    counters.iops = lance_process_iops_counter();
    if (lance_last_error_code() != LANCE_OK) check_error();
    counters.bytes_read = lance_process_bytes_read_counter();
    if (lance_last_error_code() != LANCE_OK) check_error();
    return counters;
}

/// Native SIMD report as the exact JSON object produced by
/// `lance_simd_info_json()`: `tier`, `target_arch`, and `host_features`.
inline std::string simd_info_json() {
    const char* raw = lance_simd_info_json();
    if (!raw) check_error();
    std::unique_ptr<const char, void (*)(const char*)> guard(
        raw, lance_free_string);
    return std::string(raw);
}

// ─── Blob file ───────────────────────────────────────────────────────────────

/// One blob-relative byte range for `BlobFile::read_ranges()`.
struct BlobRange {
    uint64_t offset;
    uint64_t length;
};

/// Native storage thresholds for `blob_field`.
///
/// An engaged inline threshold of zero is valid. An engaged dedicated
/// threshold must be non-zero. Values not representable by `size_t` are
/// rejected by the C boundary.
struct BlobFieldOptions {
    std::optional<size_t> inline_size_threshold;
    std::optional<size_t> dedicated_size_threshold;
};

/// Export the native blob v2 field with default storage thresholds.
///
/// `out` is caller-owned and must be released exactly once. It is written only
/// on success.
inline void blob_field(
    const std::string& name,
    bool nullable,
    ArrowSchema* out) {
    if (lance_blob_field(name.c_str(), nullable, out) != 0) check_error();
}

/// Export the native blob v2 field with explicit storage thresholds.
///
/// `out` is caller-owned and must be released exactly once. It is written only
/// on success.
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
        check_error();
}

/// RAII owner of a native writer-prepared blob descriptor array builder.
///
/// This produces the prepared Arrow field/array consumed by dataset writes.
/// `finish()` consumes the native builder; `field()` remains available
/// afterwards, while later appends throw. The wrapper does not write payload
/// objects or commit data.
class BlobDescriptorArrayBuilder {
    Handle<LanceBlobDescriptorArrayBuilder,
           lance_blob_descriptor_array_builder_close>
        handle_;

public:
    explicit BlobDescriptorArrayBuilder(const std::string& column)
        : handle_(lance_blob_descriptor_array_builder_new(column.c_str())) {
        if (!handle_) check_error();
    }

    void append_inline(const uint8_t* data, size_t len) {
        if (lance_blob_descriptor_array_builder_append_inline(
                handle_.get(), data, len) != 0) {
            check_error();
        }
    }

    void append_inline(const std::vector<uint8_t>& data) {
        append_inline(data.empty() ? nullptr : data.data(), data.size());
    }

    void append_null() {
        if (lance_blob_descriptor_array_builder_append_null(handle_.get()) != 0)
            check_error();
    }

    void append_packed(uint32_t blob_id, uint64_t offset, uint64_t length) {
        if (lance_blob_descriptor_array_builder_append_packed(
                handle_.get(), blob_id, offset, length) != 0) {
            check_error();
        }
    }

    void append_packed(uint32_t blob_id, const BlobRange& range) {
        append_packed(blob_id, range.offset, range.length);
    }

    void extend_packed(uint32_t blob_id,
                       const std::vector<BlobRange>& ranges) {
        std::vector<LanceBlobRange> raw;
        raw.reserve(ranges.size());
        for (const auto& range : ranges) {
            raw.push_back({range.offset, range.length});
        }
        if (lance_blob_descriptor_array_builder_extend_packed(
                handle_.get(), blob_id, raw.data(), raw.size()) != 0) {
            check_error();
        }
    }

    void append_dedicated(uint32_t blob_id, uint64_t length) {
        if (lance_blob_descriptor_array_builder_append_dedicated(
                handle_.get(), blob_id, length) != 0) {
            check_error();
        }
    }

    void append_managed(
        const std::string& uri,
        uint64_t offset,
        uint64_t length,
        std::optional<uint32_t> base_id = std::nullopt) {
        if (lance_blob_descriptor_array_builder_append_managed(
                handle_.get(), uri.c_str(), offset, length, base_id.has_value(),
                base_id.value_or(0)) != 0) {
            check_error();
        }
    }

    void append_external(uint32_t base_id,
                         const std::string& uri,
                         uint64_t offset,
                         uint64_t length) {
        if (lance_blob_descriptor_array_builder_append_external(
                handle_.get(), base_id, uri.c_str(), offset, length) != 0) {
            check_error();
        }
    }

    void push_external(const std::string& uri,
                       std::optional<BlobRange> range = std::nullopt) {
        if (lance_blob_descriptor_array_builder_push_external(
                handle_.get(), uri.c_str(), range.has_value(),
                range ? range->offset : 0, range ? range->length : 0) != 0) {
            check_error();
        }
    }

    /// Export the prepared field. `out` is caller-owned and must be released.
    void field(ArrowSchema* out) const {
        if (lance_blob_descriptor_array_builder_field(handle_.get(), out) != 0)
            check_error();
    }

    /// Consume the native builder and export its prepared array and field.
    /// Both outputs are caller-owned and must each be released exactly once.
    void finish(ArrowArray* out_array, ArrowSchema* out_schema) {
        if (lance_blob_descriptor_array_builder_finish(
                handle_.get(), out_array, out_schema) != 0) {
            check_error();
        }
    }
};

/// Move-only owner of a native packed blob writer.
///
/// The destructor frees the C allocation without finishing. `finish_array()`
/// consumes the native writer but leaves this object allocation valid, so
/// later calls throw `LANCE_ERR_INVALID_ARGUMENT`.
class PackedBlobWriter {
    Handle<LancePackedBlobWriter, lance_packed_blob_writer_close> handle_;

public:
    explicit PackedBlobWriter(LancePackedBlobWriter* writer) : handle_(writer) {}

    PackedBlobWriter(PackedBlobWriter&&) noexcept = default;
    PackedBlobWriter& operator=(PackedBlobWriter&&) noexcept = default;
    PackedBlobWriter(const PackedBlobWriter&) = delete;
    PackedBlobWriter& operator=(const PackedBlobWriter&) = delete;

    /// Native blob ID. Throws when the writer is NULL or finished.
    uint32_t blob_id() const {
        uint32_t value = lance_packed_blob_writer_blob_id(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    /// Native sidecar path. Throws when the writer is NULL or finished.
    std::string path() const {
        const char* raw = lance_packed_blob_writer_path(handle_.get());
        if (!raw) check_error();
        std::unique_ptr<const char, void (*)(const char*)> guard(
            raw, lance_free_string);
        return std::string(raw);
    }

    /// Append one payload. A null pointer with zero length is valid.
    void write_blob(const uint8_t* data, size_t len) {
        if (lance_packed_blob_writer_write_blob(handle_.get(), data, len) != 0)
            check_error();
    }

    void write_blob(const std::vector<uint8_t>& data) {
        write_blob(data.empty() ? nullptr : data.data(), data.size());
    }

    /// Move one binary ArrowArray into the writer; `schema` is borrowed.
    void write_blobs(ArrowArray* payloads, const ArrowSchema* schema) {
        if (lance_packed_blob_writer_write_blobs(
                handle_.get(), payloads, schema) != 0) {
            check_error();
        }
    }

    /// Consume the native writer and export caller-owned prepared parts.
    void finish_array(
        const std::string& field_name,
        ArrowArray* out_array,
        ArrowSchema* out_schema) {
        if (lance_packed_blob_writer_finish_array(
                handle_.get(), field_name.c_str(), out_array, out_schema) != 0) {
            check_error();
        }
    }
};

/// Move-only owner of a native dedicated blob writer.
///
/// Repeated writes concatenate into one sidecar. The destructor frees the C
/// allocation without finishing.
class DedicatedBlobWriter {
    Handle<LanceDedicatedBlobWriter, lance_dedicated_blob_writer_close> handle_;

public:
    explicit DedicatedBlobWriter(LanceDedicatedBlobWriter* writer)
        : handle_(writer) {}

    DedicatedBlobWriter(DedicatedBlobWriter&&) noexcept = default;
    DedicatedBlobWriter& operator=(DedicatedBlobWriter&&) noexcept = default;
    DedicatedBlobWriter(const DedicatedBlobWriter&) = delete;
    DedicatedBlobWriter& operator=(const DedicatedBlobWriter&) = delete;

    /// Native blob ID. Throws when the writer is NULL or finished.
    uint32_t blob_id() const {
        uint32_t value = lance_dedicated_blob_writer_blob_id(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    /// Native sidecar path. Throws when the writer is NULL or finished.
    std::string path() const {
        const char* raw = lance_dedicated_blob_writer_path(handle_.get());
        if (!raw) check_error();
        std::unique_ptr<const char, void (*)(const char*)> guard(
            raw, lance_free_string);
        return std::string(raw);
    }

    /// Append bytes. Repeated calls concatenate.
    void write(const uint8_t* data, size_t len) {
        if (lance_dedicated_blob_writer_write(handle_.get(), data, len) != 0)
            check_error();
    }

    void write(const std::vector<uint8_t>& data) {
        write(data.empty() ? nullptr : data.data(), data.size());
    }

    /// Consume the native writer and export caller-owned prepared parts.
    void finish_array(
        const std::string& field_name,
        ArrowArray* out_array,
        ArrowSchema* out_schema) {
        if (lance_dedicated_blob_writer_finish_array(
                handle_.get(), field_name.c_str(), out_array, out_schema) != 0) {
            check_error();
        }
    }
};

/// Row selector for planned whole-blob and range reads.
enum class BlobRowSelector : int32_t {
    None      = LANCE_BLOB_ROWS_NONE,
    Ids       = LANCE_BLOB_ROWS_IDS,
    Addresses = LANCE_BLOB_ROWS_ADDRESSES,
    Indices   = LANCE_BLOB_ROWS_INDICES,
};

/// Options for `Dataset::read_blobs()` and `Dataset::read_blob_ranges()`.
///
/// `io_buffer_size` defaults to native `None`; an engaged zero is forwarded to
/// native and rejected there.
struct BlobReadOptions {
    std::optional<uint64_t> io_buffer_size;
    bool preserve_order = true;
};

/// One row-specific blob-range request for `Dataset::read_blob_ranges()`.
struct BlobRangeRequest {
    uint64_t row = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
};

/// RAII handle over one value of a Blob v2 column, from `Dataset::take_blobs()`,
/// `take_blobs_by_indices()` or `take_blobs_by_addresses()`. Stays usable
/// after the Dataset is destroyed. `read()`, `read_up_to()` and `read_into()`
/// advance the cursor, while `read_range()` and `read_ranges()` do not.
/// `close()` marks the native file closed; the destructor frees the handle.
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

    /// Read at most `len` bytes from the current cursor into caller storage.
    /// Returns the number of bytes written, which is shorter than `len` at the
    /// end of the blob. Reuses the C `read_up_to` operation.
    size_t read_into(uint8_t* dst, size_t len) {
        size_t bytes_read = 0;
        if (lance_blob_file_read_up_to(
                handle_.get(), dst, len, &bytes_read) != 0)
            check_error();
        return bytes_read;
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

    /// Read multiple ranges without moving the cursor. The result for
    /// `ranges[i]` contains exactly `ranges[i].length` bytes; order,
    /// duplicates and overlaps are preserved.
    std::vector<std::vector<uint8_t>> read_ranges(
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
            check_error();
        }
        return out;
    }

    /// Set the native sequential read-ahead size. Zero disables read-ahead.
    void set_buffer_size(size_t buffer_size) {
        if (lance_blob_file_set_buffer_size(handle_.get(), buffer_size) != 0)
            check_error();
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

    /// Mark the native blob file closed. Idempotent. The allocation remains
    /// owned until destruction.
    void close() {
        if (lance_blob_file_mark_closed(handle_.get()) != 0) check_error();
    }

    /// Native closed state.
    bool closed() const {
        bool value = false;
        if (lance_blob_file_is_closed(handle_.get(), &value) != 0)
            check_error();
        return value;
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

    /// Whether this handle retains a dataset-bound native fragment.
    bool is_bound() const {
        bool value = false;
        if (lance_fragment_is_bound(handle_.get(), &value) != 0) check_error();
        return value;
    }

    /// Clone one native base data file from owned fragment metadata.
    DataFile data_file_at(size_t index) const {
        LanceDataFile* raw = nullptr;
        if (lance_fragment_data_file_at(handle_.get(), index, &raw) != 0)
            check_error();
        if (!raw) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_fragment_data_file_at returned success with "
                        "null data file");
        }
        return DataFile(raw);
    }

    /// Find the native base data file that stores a field ID. Missing fields
    /// return std::nullopt; a negative field ID is an invalid argument.
    std::optional<DataFile> data_file_for_field(int32_t field_id) const {
        LanceDataFile* raw = nullptr;
        if (lance_fragment_data_file_for_field(
                handle_.get(), field_id, &raw) != 0)
            check_error();
        if (!raw) return std::nullopt;
        return DataFile(raw);
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

    /// Clone the fragment's native schema from its staged dataset snapshot.
    LanceSchema native_schema() const;

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
    /// Indices retain the selected core's ordering and duplicate constraints.
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

    /// Delete rows from this fragment without publishing a transaction.
    ///
    /// The source handle remains usable. The returned fragment owns the new
    /// deletion-file metadata and can be scanned before publication. A full
    /// deletion returns `std::nullopt`.
    std::optional<Fragment> delete_if(const std::string& predicate) const;

    /// Open an overlay writer for this bound fragment. The schema may be a
    /// compatible subset of the fragment schema. Commit the resulting overlay
    /// with the dataset read version used to open the writer.
    /// Native overlays are experimental. For release builds, set
    /// `LANCE_ENABLE_UNSTABLE_DATA_OVERLAY_FILES=1` in every process that reads
    /// or modifies an overlay dataset, including before publication.
    FragmentOverlayWriter write_overlay(const LanceSchema& schema) const;

    const LanceFragment* c_handle() const { return handle_.get(); }
};

inline std::optional<Fragment> Fragment::delete_if(
    const std::string& predicate) const {
    LanceFragment* raw = nullptr;
    if (lance_fragment_delete(
            handle_.get(), predicate.c_str(), &raw) != 0) {
        check_error();
    }
    if (!raw) return std::nullopt;
    return Fragment(raw);
}

/// Move-only owner of a native Lance schema.
///
/// Unlike the Arrow export, a native schema preserves field and parent IDs,
/// logical types, encodings, dictionary values, nested metadata, field order
/// and nullability. A clone remains valid after the dataset, fragment or
/// operation that produced it is closed.
class LanceSchema {
    Handle<::LanceSchema, lance_schema_close> handle_;

public:
    explicit LanceSchema(::LanceSchema* schema) : handle_(schema) {
        if (!handle_) check_error();
    }

    LanceSchema(LanceSchema&&) noexcept = default;
    LanceSchema& operator=(LanceSchema&&) noexcept = default;
    LanceSchema(const LanceSchema&) = delete;
    LanceSchema& operator=(const LanceSchema&) = delete;

    /// Independently owned native clone.
    LanceSchema clone() const {
        ::LanceSchema* raw = lance_schema_clone(handle_.get());
        if (!raw) check_error();
        return LanceSchema(raw);
    }

    /// Clone the current snapshot schema of a dataset.
    static LanceSchema from_dataset(const Dataset& dataset);

    /// Clone the staged snapshot schema of a fragment.
    static LanceSchema from_fragment(const Fragment& fragment);

    /// Clone the in-memory schema of an Overwrite, Merge or Project operation.
    /// Other kinds throw LANCE_ERR_NOT_SUPPORTED.
    static LanceSchema from_operation(const Operation& operation);

    /// Project onto ordered native column paths. Paths, order, unknown-column
    /// errors and empty-selection behavior are native `Schema::project`.
    static LanceSchema project(const LanceSchema& schema,
                               const std::vector<std::string>& columns) {
        std::vector<const char*> paths;
        paths.reserve(columns.size());
        for (const auto& column : columns) paths.push_back(column.c_str());
        ::LanceSchema* raw = nullptr;
        if (lance_schema_project(
                schema.c_handle(), paths.empty() ? nullptr : paths.data(),
                paths.size(), &raw) != 0) {
            check_error();
        }
        return LanceSchema(raw);
    }

    /// Native Lance field ID for a column path. Throws when the path does not
    /// resolve.
    int32_t field_id(const std::string& column) const {
        int32_t id = 0;
        if (lance_schema_field_id(
                handle_.get(), column.c_str(), &id) != 0) {
            check_error();
        }
        return id;
    }

    /// Build a native schema from a borrowed Arrow schema. Fields without a
    /// `lance:field_id` metadata entry receive fresh native IDs; existing
    /// IDs, dictionaries, and metadata are preserved. Throws on native
    /// validation failure.
    static LanceSchema from_arrow(const ArrowSchema* schema);

    /// Export an Arrow interoperability view. `out` is caller-owned and must
    /// be released exactly once. Arrow cannot represent every native detail.
    void to_arrow(ArrowSchema* out) const {
        if (lance_schema_to_arrow(handle_.get(), out) != 0) check_error();
    }

    const ::LanceSchema* c_handle() const { return handle_.get(); }
};

inline LanceSchema Fragment::native_schema() const {
    return LanceSchema::from_fragment(*this);
}

inline LanceSchema LanceSchema::from_fragment(const Fragment& fragment) {
    ::LanceSchema* raw = nullptr;
    if (lance_fragment_schema_clone(fragment.c_handle(), &raw) != 0) {
        check_error();
    }
    return LanceSchema(raw);
}

inline LanceSchema LanceSchema::from_operation(const Operation& operation) {
    ::LanceSchema* raw = nullptr;
    if (lance_operation_schema_clone(operation.c_handle(), &raw) != 0) {
        check_error();
    }
    return LanceSchema(raw);
}

inline Operation Operation::make_project(
    const LanceSchema& schema, bool preserves_nullability) {
    LanceOperation* raw = nullptr;
    if (lance_operation_project_new(
            schema.c_handle(), preserves_nullability, &raw) != 0) {
        check_error();
    }
    return Operation(raw);
}

inline Fragment Operation::append_fragment_at(size_t index) const {
    LanceFragment* raw = nullptr;
    if (lance_operation_append_fragment_at(handle_.get(), index, &raw) != 0)
        check_error();
    if (!raw) {
        throw Error(LANCE_ERR_INTERNAL,
                    "lance_operation_append_fragment_at returned success "
                    "with null fragment");
    }
    return Fragment(raw);
}

inline std::vector<BasePath> Operation::update_bases() const {
    uint64_t count = 0;
    if (lance_operation_update_bases_count(handle_.get(), &count) != 0)
        check_error();

    std::vector<BasePath> out;
    out.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        LanceBasePathView view{};
        if (lance_operation_update_bases_at(
                handle_.get(), static_cast<size_t>(i), &view) != 0)
            check_error();
        BasePath base;
        base.id = view.id;
        base.path = view.path ? view.path : "";
        base.name = view.name ? std::optional<std::string>(view.name)
                              : std::nullopt;
        base.is_dataset_root = view.is_dataset_root;
        out.push_back(std::move(base));
    }
    return out;
}

inline Fragment Operation::overwrite_fragment_at(size_t index) const {
    LanceFragment* raw = nullptr;
    if (lance_operation_overwrite_fragment_at(handle_.get(), index, &raw) != 0)
        check_error();
    if (!raw) {
        throw Error(LANCE_ERR_INTERNAL,
                    "lance_operation_overwrite_fragment_at returned success "
                    "with null fragment");
    }
    return Fragment(raw);
}

inline LanceSchema Operation::overwrite_schema() const {
    ::LanceSchema* raw = nullptr;
    if (lance_operation_overwrite_schema_clone(handle_.get(), &raw) != 0)
        check_error();
    if (!raw) {
        throw Error(LANCE_ERR_INTERNAL,
                    "lance_operation_overwrite_schema_clone returned success "
                    "with null schema");
    }
    return LanceSchema(raw);
}

inline Fragment Operation::merge_fragment_at(size_t index) const {
    LanceFragment* raw = nullptr;
    if (lance_operation_merge_fragment_at(handle_.get(), index, &raw) != 0)
        check_error();
    if (!raw) {
        throw Error(LANCE_ERR_INTERNAL,
                    "lance_operation_merge_fragment_at returned success "
                    "with null fragment");
    }
    return Fragment(raw);
}

inline LanceSchema Operation::merge_schema() const {
    ::LanceSchema* raw = nullptr;
    if (lance_operation_merge_schema_clone(handle_.get(), &raw) != 0)
        check_error();
    if (!raw) {
        throw Error(LANCE_ERR_INTERNAL,
                    "lance_operation_merge_schema_clone returned success "
                    "with null schema");
    }
    return LanceSchema(raw);
}

inline LanceSchema Operation::project_schema() const {
    ::LanceSchema* raw = nullptr;
    if (lance_operation_project_schema_clone(handle_.get(), &raw) != 0)
        check_error();
    if (!raw) {
        throw Error(LANCE_ERR_INTERNAL,
                    "lance_operation_project_schema_clone returned success "
                    "with null schema");
    }
    return LanceSchema(raw);
}

inline Operation Operation::make_overwrite(
    const LanceSchema& schema,
    const std::vector<const Fragment*>& fragments,
    std::optional<std::vector<std::pair<std::string, std::string>>>
        config_upsert_values,
    std::optional<std::vector<BasePath>> initial_bases) {
    std::vector<const LanceFragment*> fragment_handles;
    fragment_handles.reserve(fragments.size());
    for (const Fragment* fragment : fragments) {
        if (!fragment) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "Operation::make_overwrite received a null fragment");
        }
        fragment_handles.push_back(fragment->c_handle());
    }

    // The native constructor copies all strings before returning.
    std::vector<std::string> config_keys;
    std::vector<std::string> config_values;
    std::vector<const char*> config_key_ptrs;
    std::vector<const char*> config_value_ptrs;
    LanceStringMap config_storage{};
    const LanceStringMap* config_map = nullptr;
    if (config_upsert_values) {
        config_keys.reserve(config_upsert_values->size());
        config_values.reserve(config_upsert_values->size());
        for (const auto& [key, value] : *config_upsert_values) {
            config_keys.push_back(key);
            config_values.push_back(value);
        }
        for (size_t i = 0; i < config_keys.size(); ++i) {
            config_key_ptrs.push_back(config_keys[i].c_str());
            config_value_ptrs.push_back(config_values[i].c_str());
        }
        config_storage.keys =
            config_key_ptrs.empty() ? nullptr : config_key_ptrs.data();
        config_storage.values =
            config_value_ptrs.empty() ? nullptr : config_value_ptrs.data();
        config_storage.count = config_key_ptrs.size();
        config_map = &config_storage;
    }

    std::vector<LanceBasePath> base_views;
    LanceBasePath empty_base{};
    LanceBasePathList base_list{};
    const LanceBasePathList* initial_base_list = nullptr;
    if (initial_bases) {
        base_views.reserve(initial_bases->size());
        for (const BasePath& base : *initial_bases) {
            LanceBasePath view{};
            view.id = base.id;
            view.path = base.path.c_str();
            view.name = base.name ? base.name->c_str() : nullptr;
            view.is_dataset_root = base.is_dataset_root;
            base_views.push_back(view);
        }
        base_list.items =
            base_views.empty() ? &empty_base : base_views.data();
        base_list.count = base_views.size();
        initial_base_list = &base_list;
    }

    auto* raw = lance_operation_overwrite_new(
        schema.c_handle(),
        fragment_handles.empty() ? nullptr : fragment_handles.data(),
        fragment_handles.size(), config_map, initial_base_list);
    if (!raw) check_error();
    return Operation(raw);
}

inline Operation Operation::make_merge(
    const LanceSchema& schema,
    const std::vector<const Fragment*>& fragments,
    bool preserves_nullability) {
    std::vector<const LanceFragment*> fragment_handles;
    fragment_handles.reserve(fragments.size());
    for (const Fragment* fragment : fragments) {
        if (!fragment) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "Operation::make_merge received a null fragment");
        }
        fragment_handles.push_back(fragment->c_handle());
    }
    auto* raw = lance_operation_merge_new(
        schema.c_handle(),
        fragment_handles.empty() ? nullptr : fragment_handles.data(),
        fragment_handles.size(), preserves_nullability);
    if (!raw) check_error();
    return Operation(raw);
}

inline Operation Operation::make_clone(
    bool is_shallow, std::optional<std::string> ref_name,
    uint64_t ref_version, std::string ref_path,
    std::optional<std::string> branch_name) {
    auto* raw = lance_operation_clone_new(
        is_shallow, ref_name ? ref_name->c_str() : nullptr, ref_version,
        ref_path.c_str(), branch_name ? branch_name->c_str() : nullptr);
    if (!raw) check_error();
    return Operation(raw);
}

inline Operation Operation::make_update_config(
    const UpdateConfigOptions& options) {
    std::vector<LanceFieldMetadataUpdateMap> field_updates;
    field_updates.reserve(options.field_metadata_updates.size());
    for (const auto& [field_id, updates] : options.field_metadata_updates) {
        LanceFieldMetadataUpdateMap entry{};
        entry.field_id = field_id;
        entry.updates = updates.c_handle();
        field_updates.push_back(entry);
    }

    LanceUpdateConfigOperationOptions c_options{};
    c_options.config_updates =
        options.config_updates ? options.config_updates->c_handle() : nullptr;
    c_options.table_metadata_updates =
        options.table_metadata_updates
            ? options.table_metadata_updates->c_handle()
            : nullptr;
    c_options.schema_metadata_updates =
        options.schema_metadata_updates
            ? options.schema_metadata_updates->c_handle()
            : nullptr;
    c_options.field_metadata_updates =
        field_updates.empty() ? nullptr : field_updates.data();
    c_options.field_metadata_update_count = field_updates.size();

    auto* raw = lance_operation_update_config_new(&c_options);
    if (!raw) check_error();
    return Operation(raw);
}

inline Operation Operation::make_append(
    const std::vector<const Fragment*>& fragments) {
    std::vector<const LanceFragment*> handles;
    handles.reserve(fragments.size());
    for (const auto* fragment : fragments) {
        if (!fragment) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "append fragment must not be null");
        }
        handles.push_back(fragment->c_handle());
    }
    auto* operation = lance_operation_append_new(
        handles.empty() ? nullptr : handles.data(), handles.size());
    if (!operation) check_error();
    return Operation(operation);
}

inline Operation Operation::make_update_bases(
    const std::vector<BasePath>& new_bases) {
    std::vector<LanceBasePath> raw;
    raw.reserve(new_bases.size());
    for (const auto& base : new_bases) {
        LanceBasePath entry{};
        entry.id = base.id;
        entry.path = base.path.c_str();
        entry.name = base.name ? base.name->c_str() : nullptr;
        entry.is_dataset_root = base.is_dataset_root;
        raw.push_back(entry);
    }
    auto* operation = lance_operation_update_bases_new(
        raw.empty() ? nullptr : raw.data(), raw.size());
    if (!operation) check_error();
    return Operation(operation);
}

inline Operation Operation::make_delete(
    const std::vector<const Fragment*>& updated_fragments,
    const std::vector<uint64_t>& deleted_fragment_ids,
    const std::string& predicate) {
    std::vector<const LanceFragment*> handles;
    handles.reserve(updated_fragments.size());
    for (const auto* fragment : updated_fragments) {
        if (!fragment) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "updated fragment must not be null");
        }
        handles.push_back(fragment->c_handle());
    }
    auto* operation = lance_operation_delete_new(
        handles.empty() ? nullptr : handles.data(), handles.size(),
        deleted_fragment_ids.empty() ? nullptr : deleted_fragment_ids.data(),
        deleted_fragment_ids.size(), predicate.c_str());
    if (!operation) check_error();
    return Operation(operation);
}

inline Operation Operation::make_data_replacement(
    const std::vector<const DataReplacementGroup*>& replacements) {
    std::vector<const LanceDataReplacementGroup*> handles;
    handles.reserve(replacements.size());
    for (const auto* replacement : replacements) {
        if (!replacement) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "data replacement group must not be null");
        }
        handles.push_back(replacement->c_handle());
    }
    auto* operation = lance_operation_data_replacement_new(
        handles.empty() ? nullptr : handles.data(), handles.size());
    if (!operation) check_error();
    return Operation(operation);
}

inline Operation Operation::make_data_overlay(
    const std::vector<const DataOverlayGroup*>& groups) {
    std::vector<const LanceDataOverlayGroup*> handles;
    handles.reserve(groups.size());
    for (const auto* group : groups) {
        if (!group) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "data overlay group must not be null");
        }
        handles.push_back(group->c_handle());
    }
    auto* operation = lance_operation_data_overlay_new(
        handles.empty() ? nullptr : handles.data(), handles.size());
    if (!operation) check_error();
    return Operation(operation);
}

/// Additive native vector-build options.
///
/// A default-constructed value leaves every optional native field absent.
/// `train == std::nullopt` retains the native train=true default. Arrow models
/// are borrowed for the call; `IndexModel` values must outlive the call or
/// builder-creation call that receives them.
struct VectorIndexOptions {
    LanceVectorIndexType index_type = LANCE_INDEX_IVF_FLAT;
    LanceMetricType metric = LANCE_METRIC_L2;

    std::optional<uint64_t> num_partitions;
    std::optional<uint64_t> target_partition_size;
    std::optional<uint64_t> max_iters;
    std::optional<uint64_t> sample_rate;
    std::optional<uint64_t> streaming_sample_rate;
    std::optional<uint64_t> streaming_coreset_rate;
    uint64_t streaming_refine_passes = 0;

    std::optional<std::string> precomputed_partitions_file;
    std::optional<std::pair<std::string, std::vector<std::string>>>
        precomputed_shuffle_buffers;
    std::optional<uint64_t> shuffle_partition_batches;
    std::optional<uint64_t> shuffle_partition_concurrency;
    std::optional<std::map<std::string, std::string>> storage_options;

    std::optional<uint64_t> num_sub_vectors;
    std::optional<uint64_t> num_bits;
    IndexModel* ivf_centroids = nullptr;
    IndexModel* pq_codebook = nullptr;
    std::optional<std::string> rabitq_model_json;

    std::optional<uint64_t> hnsw_max_level;
    std::optional<uint64_t> hnsw_m;
    std::optional<uint64_t> hnsw_ef_construction;

    LanceIndexFileVersion index_file_version = LANCE_INDEX_FILE_VERSION_V3;
    bool skip_transpose = false;
    std::map<std::string, std::string> runtime_hints;
    std::optional<bool> train;

    /// Record a caller-provided accelerator hint. This does not enable C-side
    /// accelerator execution.
    VectorIndexOptions& accelerator(std::string device) {
        runtime_hints["lancedb.accelerator"] = std::move(device);
        return *this;
    }
};

// ─── Native maintenance options and plans ───────────────────────────────────

/// Presence-preserving owner of native `CompactionOptions`.
class CompactionOptions {
    Handle<LanceCompactionOptionsV2, lance_compaction_options_v2_free> handle_;

public:
    CompactionOptions() : handle_(lance_compaction_options_v2_new()) {
        if (!handle_) check_error();
    }

    explicit CompactionOptions(LanceCompactionOptionsV2* options)
        : handle_(options) {}

    CompactionOptions(CompactionOptions&&) noexcept = default;
    CompactionOptions& operator=(CompactionOptions&&) noexcept = default;
    CompactionOptions(const CompactionOptions&) = delete;
    CompactionOptions& operator=(const CompactionOptions&) = delete;

    /// Native `CompactionOptions::from_dataset_config(dataset.config())`.
    static CompactionOptions from_dataset(const Dataset& dataset);

    CompactionOptions& set_target_rows_per_fragment(uint64_t value) {
        if (lance_compaction_options_v2_set_target_rows_per_fragment(
                handle_.get(), value) != 0) check_error();
        return *this;
    }

    CompactionOptions& set_max_rows_per_group(uint64_t value) {
        if (lance_compaction_options_v2_set_max_rows_per_group(
                handle_.get(), value) != 0) check_error();
        return *this;
    }

    CompactionOptions& set_max_bytes_per_file(std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_max_bytes_per_file(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            check_error();
        return *this;
    }

    CompactionOptions& set_materialize_deletions(bool value) {
        if (lance_compaction_options_v2_set_materialize_deletions(
                handle_.get(), value) != 0) check_error();
        return *this;
    }

    CompactionOptions& set_materialize_deletions_threshold(float value) {
        if (lance_compaction_options_v2_set_materialize_deletions_threshold(
                handle_.get(), value) != 0) check_error();
        return *this;
    }

    CompactionOptions& set_num_threads(std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_num_threads(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            check_error();
        return *this;
    }

    CompactionOptions& set_batch_size(std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_batch_size(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            check_error();
        return *this;
    }

    CompactionOptions& set_io_buffer_size(std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_io_buffer_size(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            check_error();
        return *this;
    }

    CompactionOptions& set_defer_index_remap(bool value) {
        if (lance_compaction_options_v2_set_defer_index_remap(
                handle_.get(), value) != 0) check_error();
        return *this;
    }

    CompactionOptions& set_index_remap_mode(LanceIndexRemapMode value) {
        if (lance_compaction_options_v2_set_index_remap_mode(
                handle_.get(), static_cast<int32_t>(value)) != 0)
            check_error();
        return *this;
    }

    CompactionOptions& set_compaction_mode(
        std::optional<LanceCompactionMode> value) {
        int32_t raw = value ? static_cast<int32_t>(*value) : 0;
        if (lance_compaction_options_v2_set_compaction_mode(
                handle_.get(), value.has_value(), raw) != 0)
            check_error();
        return *this;
    }

    CompactionOptions& set_binary_copy_read_batch_bytes(
        std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_binary_copy_read_batch_bytes(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            check_error();
        return *this;
    }

    CompactionOptions& set_max_source_fragments(
        std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_max_source_fragments(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            check_error();
        return *this;
    }

    CompactionOptions& set_max_source_rows(std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_max_source_rows(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            check_error();
        return *this;
    }

    CompactionOptions& set_max_source_bytes(std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_max_source_bytes(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            check_error();
        return *this;
    }

    CompactionOptions& set_excluded_fragment_ids(
        const std::vector<uint32_t>& fragment_ids) {
        if (lance_compaction_options_v2_set_excluded_fragment_ids(
                handle_.get(),
                fragment_ids.empty() ? nullptr : fragment_ids.data(),
                fragment_ids.size()) != 0)
            check_error();
        return *this;
    }

    CompactionOptions& set_max_overlays_per_fragment(
        std::optional<uint64_t> value) {
        if (lance_compaction_options_v2_set_max_overlays_per_fragment(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            check_error();
        return *this;
    }

    CompactionOptions& set_data_storage_version(
        std::optional<std::string> selector) {
        if (lance_compaction_options_v2_set_data_storage_version(
                handle_.get(), selector ? selector->c_str() : nullptr) != 0)
            check_error();
        return *this;
    }

    CompactionOptions& set_transaction_properties(
        std::optional<std::vector<std::pair<std::string, std::string>>> value =
            std::nullopt) {
        std::vector<const char*> keys;
        std::vector<const char*> values;
        LanceTransactionProperties properties{};
        const LanceTransactionProperties* properties_ptr = nullptr;
        if (value) {
            keys.reserve(value->size());
            values.reserve(value->size());
            for (const auto& [key, property_value] : *value) {
                keys.push_back(key.c_str());
                values.push_back(property_value.c_str());
            }
            properties.keys = keys.empty() ? nullptr : keys.data();
            properties.values = values.empty() ? nullptr : values.data();
            properties.count = value->size();
            properties_ptr = &properties;
        }
        if (lance_compaction_options_v2_set_transaction_properties(
                handle_.get(), properties_ptr) != 0)
            check_error();
        return *this;
    }

    const LanceCompactionOptionsV2* c_handle() const { return handle_.get(); }
};

/// Owned native compaction rewrite result.
class RewriteResult {
    Handle<LanceRewriteResult, lance_rewrite_result_close> handle_;

public:
    explicit RewriteResult(LanceRewriteResult* result) : handle_(result) {}

    RewriteResult(RewriteResult&&) noexcept = default;
    RewriteResult& operator=(RewriteResult&&) noexcept = default;
    RewriteResult(const RewriteResult&) = delete;
    RewriteResult& operator=(const RewriteResult&) = delete;

    uint64_t read_version() const {
        uint64_t value = lance_rewrite_result_read_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    LanceCompactionMetrics metrics() const {
        LanceCompactionMetrics value{};
        if (lance_rewrite_result_metrics(handle_.get(), &value) != 0)
            check_error();
        return value;
    }

    size_t original_fragment_count() const {
        size_t count =
            lance_rewrite_result_original_fragment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return count;
    }

    size_t new_fragment_count() const {
        size_t count = lance_rewrite_result_new_fragment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return count;
    }

    std::string original_fragment_metadata_json(size_t index) const {
        const char* json =
            lance_rewrite_result_original_fragment_metadata_json_at(
                handle_.get(), index);
        if (!json) check_error();
        std::string out(json);
        lance_free_string(json);
        return out;
    }

    std::string new_fragment_metadata_json(size_t index) const {
        const char* json =
            lance_rewrite_result_new_fragment_metadata_json_at(
                handle_.get(), index);
        if (!json) check_error();
        std::string out(json);
        lance_free_string(json);
        return out;
    }

    std::string json() const {
        const char* raw = lance_rewrite_result_json(handle_.get());
        if (!raw) check_error();
        std::string out(raw);
        lance_free_string(raw);
        return out;
    }

    static RewriteResult from_json(std::string_view json) {
        std::string owned(json);
        auto* result = lance_rewrite_result_from_json(owned.c_str());
        if (!result) check_error();
        return RewriteResult(result);
    }

    const LanceRewriteResult* c_handle() const { return handle_.get(); }
};

/// Owned snapshot of one native compaction task.
class CompactionTask {
    Handle<LanceCompactionTask, lance_compaction_task_close> handle_;

public:
    explicit CompactionTask(LanceCompactionTask* task) : handle_(task) {}

    CompactionTask(CompactionTask&&) noexcept = default;
    CompactionTask& operator=(CompactionTask&&) noexcept = default;
    CompactionTask(const CompactionTask&) = delete;
    CompactionTask& operator=(const CompactionTask&) = delete;

    uint64_t read_version() const {
        uint64_t value = lance_compaction_task_read_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    size_t fragment_count() const {
        size_t count = lance_compaction_task_fragment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return count;
    }

    std::string fragment_metadata_json(size_t index) const {
        const char* raw =
            lance_compaction_task_fragment_metadata_json_at(
                handle_.get(), index);
        if (!raw) check_error();
        std::string out(raw);
        lance_free_string(raw);
        return out;
    }

    std::vector<std::string> fragment_metadata_json() const {
        std::vector<std::string> out;
        out.reserve(fragment_count());
        for (size_t index = 0; index < fragment_count(); ++index)
            out.push_back(fragment_metadata_json(index));
        return out;
    }

    CompactionOptions options() const {
        auto* options =
            lance_compaction_task_options_clone(handle_.get());
        if (!options) check_error();
        return CompactionOptions(options);
    }

    RewriteResult execute(const Dataset& dataset) const;

    std::string json() const {
        const char* raw = lance_compaction_task_json(handle_.get());
        if (!raw) check_error();
        std::string out(raw);
        lance_free_string(raw);
        return out;
    }

    static CompactionTask from_json(std::string_view json) {
        std::string owned(json);
        auto* task = lance_compaction_task_from_json(owned.c_str());
        if (!task) check_error();
        return CompactionTask(task);
    }

    const LanceCompactionTask* c_handle() const { return handle_.get(); }
};

/// Owned native compaction plan.
class CompactionPlan {
    Handle<LanceCompactionPlan, lance_compaction_plan_close> handle_;

public:
    explicit CompactionPlan(LanceCompactionPlan* plan) : handle_(plan) {}

    CompactionPlan(CompactionPlan&&) noexcept = default;
    CompactionPlan& operator=(CompactionPlan&&) noexcept = default;
    CompactionPlan(const CompactionPlan&) = delete;
    CompactionPlan& operator=(const CompactionPlan&) = delete;

    uint64_t read_version() const {
        uint64_t value = lance_compaction_plan_read_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    size_t num_tasks() const {
        size_t count = lance_compaction_plan_task_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return count;
    }

    CompactionTask task(size_t index) const {
        auto* task = lance_compaction_plan_task_at(handle_.get(), index);
        if (!task) check_error();
        return CompactionTask(task);
    }

    CompactionOptions options() const {
        auto* options =
            lance_compaction_plan_options_clone(handle_.get());
        if (!options) check_error();
        return CompactionOptions(options);
    }

    std::string json() const {
        const char* raw = lance_compaction_plan_json(handle_.get());
        if (!raw) check_error();
        std::string out(raw);
        lance_free_string(raw);
        return out;
    }

    static CompactionPlan from_json(std::string_view json) {
        std::string owned(json);
        auto* plan = lance_compaction_plan_from_json(owned.c_str());
        if (!plan) check_error();
        return CompactionPlan(plan);
    }

    const LanceCompactionPlan* c_handle() const { return handle_.get(); }
};

/// Presence-preserving owner of native `OptimizeOptions`.
class OptimizeIndicesOptions {
    Handle<LanceOptimizeIndicesOptions, lance_optimize_indices_options_free>
        handle_;

public:
    OptimizeIndicesOptions() : handle_(lance_optimize_indices_options_new()) {
        if (!handle_) check_error();
    }

    explicit OptimizeIndicesOptions(LanceOptimizeIndicesOptions* options)
        : handle_(options) {}

    OptimizeIndicesOptions(OptimizeIndicesOptions&&) noexcept = default;
    OptimizeIndicesOptions& operator=(OptimizeIndicesOptions&&) noexcept =
        default;
    OptimizeIndicesOptions(const OptimizeIndicesOptions&) = delete;
    OptimizeIndicesOptions& operator=(const OptimizeIndicesOptions&) = delete;

    OptimizeIndicesOptions& set_num_indices_to_merge(
        std::optional<uint64_t> value) {
        if (lance_optimize_indices_options_set_num_indices_to_merge(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            check_error();
        return *this;
    }

    OptimizeIndicesOptions& set_enable_partition_rebalancing(bool value) {
        if (lance_optimize_indices_options_set_enable_partition_rebalancing(
                handle_.get(), value) != 0)
            check_error();
        return *this;
    }

    OptimizeIndicesOptions& set_index_names(
        std::optional<std::vector<std::string>> names) {
        std::vector<const char*> raw;
        if (names) {
            raw.reserve(names->size());
            for (const auto& name : *names) raw.push_back(name.c_str());
        }
        if (lance_optimize_indices_options_set_index_names(
                handle_.get(),
                names.has_value(),
                raw.empty() ? nullptr : raw.data(),
                raw.size()) != 0)
            check_error();
        return *this;
    }

    OptimizeIndicesOptions& set_retrain(bool value) {
        if (lance_optimize_indices_options_set_retrain(
                handle_.get(), value) != 0)
            check_error();
        return *this;
    }

    OptimizeIndicesOptions& set_transaction_properties(
        std::optional<std::vector<std::pair<std::string, std::string>>> value =
            std::nullopt) {
        std::vector<const char*> keys;
        std::vector<const char*> values;
        LanceTransactionProperties properties{};
        const LanceTransactionProperties* properties_ptr = nullptr;
        if (value) {
            keys.reserve(value->size());
            values.reserve(value->size());
            for (const auto& [key, property_value] : *value) {
                keys.push_back(key.c_str());
                values.push_back(property_value.c_str());
            }
            properties.keys = keys.empty() ? nullptr : keys.data();
            properties.values = values.empty() ? nullptr : values.data();
            properties.count = value->size();
            properties_ptr = &properties;
        }
        if (lance_optimize_indices_options_set_transaction_properties(
                handle_.get(), properties_ptr) != 0)
            check_error();
        return *this;
    }

    OptimizeIndicesOptions& set_max_rows_per_task(
        std::optional<uint64_t> value) {
        if (lance_optimize_indices_options_set_max_rows_per_task(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            check_error();
        return *this;
    }

    OptimizeIndicesOptions& set_max_rows_per_segment(
        std::optional<uint64_t> value) {
        if (lance_optimize_indices_options_set_max_rows_per_segment(
                handle_.get(), value.has_value(), value.value_or(0)) != 0)
            check_error();
        return *this;
    }

    const LanceOptimizeIndicesOptions* c_handle() const {
        return handle_.get();
    }
};

/// Owned native optimized index segment result.
class IndexSegmentResult {
    Handle<LanceIndexSegmentResult, lance_index_segment_result_close> handle_;

public:
    explicit IndexSegmentResult(LanceIndexSegmentResult* result)
        : handle_(result) {}

    IndexSegmentResult(IndexSegmentResult&&) noexcept = default;
    IndexSegmentResult& operator=(IndexSegmentResult&&) noexcept = default;
    IndexSegmentResult(const IndexSegmentResult&) = delete;
    IndexSegmentResult& operator=(const IndexSegmentResult&) = delete;

    std::array<uint8_t, 16> plan_id() const {
        std::array<uint8_t, 16> out = {};
        if (lance_index_segment_result_plan_id(
                handle_.get(), out.data()) != 0)
            check_error();
        return out;
    }

    uint64_t read_version() const {
        uint64_t value = lance_index_segment_result_read_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    uint32_t group() const {
        uint32_t value = lance_index_segment_result_group(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    IndexSegmentMetadata segment() const;

    size_t replace_count() const {
        size_t count = lance_index_segment_result_replace_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return count;
    }

    std::array<uint8_t, 16> replace_uuid(size_t index) const {
        std::array<uint8_t, 16> out = {};
        if (lance_index_segment_result_replace_uuid_at(
                handle_.get(), index, out.data()) != 0)
            check_error();
        return out;
    }

    std::string json() const {
        const char* raw = lance_index_segment_result_json(handle_.get());
        if (!raw) check_error();
        std::string out(raw);
        lance_free_string(raw);
        return out;
    }

    static IndexSegmentResult from_json(std::string_view json) {
        std::string owned(json);
        auto* result = lance_index_segment_result_from_json(owned.c_str());
        if (!result) check_error();
        return IndexSegmentResult(result);
    }

    const LanceIndexSegmentResult* c_handle() const { return handle_.get(); }
};

/// Owned snapshot of one native distributed index build task.
class OptimizeIndexTask {
    Handle<LanceOptimizeIndexTask, lance_optimize_index_task_close> handle_;

public:
    explicit OptimizeIndexTask(LanceOptimizeIndexTask* task) : handle_(task) {}

    OptimizeIndexTask(OptimizeIndexTask&&) noexcept = default;
    OptimizeIndexTask& operator=(OptimizeIndexTask&&) noexcept = default;
    OptimizeIndexTask(const OptimizeIndexTask&) = delete;
    OptimizeIndexTask& operator=(const OptimizeIndexTask&) = delete;

    std::array<uint8_t, 16> plan_id() const {
        std::array<uint8_t, 16> out = {};
        if (lance_optimize_index_task_plan_id(
                handle_.get(), out.data()) != 0)
            check_error();
        return out;
    }

    uint64_t read_version() const {
        uint64_t value = lance_optimize_index_task_read_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    uint32_t group() const {
        uint32_t value = lance_optimize_index_task_group(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    size_t fragment_count() const {
        size_t count = lance_optimize_index_task_fragment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return count;
    }

    uint32_t fragment_id(size_t index) const {
        uint32_t value = 0;
        if (lance_optimize_index_task_fragment_id_at(
                handle_.get(), index, &value) != 0)
            check_error();
        return value;
    }

    std::vector<uint32_t> fragment_ids() const {
        std::vector<uint32_t> out;
        out.reserve(fragment_count());
        for (size_t index = 0; index < fragment_count(); ++index)
            out.push_back(fragment_id(index));
        return out;
    }

    IndexSegmentMetadata reference() const;

    size_t old_segment_count() const {
        size_t count =
            lance_optimize_index_task_old_segment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return count;
    }

    IndexSegmentMetadata old_segment(size_t index) const;

    std::optional<uint64_t> num_indices_to_merge() const {
        bool has_value = false;
        uint64_t value = 0;
        if (lance_optimize_index_task_num_indices_to_merge(
                handle_.get(), &has_value, &value) != 0)
            check_error();
        return has_value ? std::optional<uint64_t>(value) : std::nullopt;
    }

    bool enable_partition_rebalancing() const {
        bool value = false;
        if (lance_optimize_index_task_enable_partition_rebalancing(
                handle_.get(), &value) != 0)
            check_error();
        return value;
    }

    bool retrain() const {
        bool value = false;
        if (lance_optimize_index_task_retrain(handle_.get(), &value) != 0)
            check_error();
        return value;
    }

    std::optional<IndexSegmentResult> execute(const Dataset& dataset) const;

    std::string json() const {
        const char* raw = lance_optimize_index_task_json(handle_.get());
        if (!raw) check_error();
        std::string out(raw);
        lance_free_string(raw);
        return out;
    }

    static OptimizeIndexTask from_json(std::string_view json) {
        std::string owned(json);
        auto* task = lance_optimize_index_task_from_json(owned.c_str());
        if (!task) check_error();
        return OptimizeIndexTask(task);
    }

    const LanceOptimizeIndexTask* c_handle() const { return handle_.get(); }
};

/// Owned snapshot of one native distributed index merge.
class OptimizeIndexMerge {
    Handle<LanceOptimizeIndexMerge, lance_optimize_index_merge_close> handle_;

public:
    explicit OptimizeIndexMerge(LanceOptimizeIndexMerge* merge)
        : handle_(merge) {}

    OptimizeIndexMerge(OptimizeIndexMerge&&) noexcept = default;
    OptimizeIndexMerge& operator=(OptimizeIndexMerge&&) noexcept = default;
    OptimizeIndexMerge(const OptimizeIndexMerge&) = delete;
    OptimizeIndexMerge& operator=(const OptimizeIndexMerge&) = delete;

    std::array<uint8_t, 16> plan_id() const {
        std::array<uint8_t, 16> out = {};
        if (lance_optimize_index_merge_plan_id(
                handle_.get(), out.data()) != 0)
            check_error();
        return out;
    }

    uint64_t read_version() const {
        uint64_t value = lance_optimize_index_merge_read_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    uint32_t group() const {
        uint32_t value = lance_optimize_index_merge_group(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    size_t old_segment_count() const {
        size_t count =
            lance_optimize_index_merge_old_segment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return count;
    }

    IndexSegmentMetadata old_segment(size_t index) const;

    size_t replace_count() const {
        size_t count = lance_optimize_index_merge_replace_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return count;
    }

    std::array<uint8_t, 16> replace_uuid(size_t index) const {
        std::array<uint8_t, 16> out = {};
        if (lance_optimize_index_merge_replace_uuid_at(
                handle_.get(), index, out.data()) != 0)
            check_error();
        return out;
    }

    std::optional<IndexSegmentResult> execute(
        const Dataset& dataset,
        const std::vector<IndexSegmentResult>& task_results) const;

    std::string json() const {
        const char* raw = lance_optimize_index_merge_json(handle_.get());
        if (!raw) check_error();
        std::string out(raw);
        lance_free_string(raw);
        return out;
    }

    static OptimizeIndexMerge from_json(std::string_view json) {
        std::string owned(json);
        auto* merge = lance_optimize_index_merge_from_json(owned.c_str());
        if (!merge) check_error();
        return OptimizeIndexMerge(merge);
    }

    const LanceOptimizeIndexMerge* c_handle() const { return handle_.get(); }
};

/// Owned native distributed index optimization plan.
class OptimizeIndicesPlan {
    Handle<LanceOptimizeIndicesPlan, lance_optimize_indices_plan_close>
        handle_;

public:
    explicit OptimizeIndicesPlan(LanceOptimizeIndicesPlan* plan)
        : handle_(plan) {}

    OptimizeIndicesPlan(OptimizeIndicesPlan&&) noexcept = default;
    OptimizeIndicesPlan& operator=(OptimizeIndicesPlan&&) noexcept = default;
    OptimizeIndicesPlan(const OptimizeIndicesPlan&) = delete;
    OptimizeIndicesPlan& operator=(const OptimizeIndicesPlan&) = delete;

    std::array<uint8_t, 16> plan_id() const {
        std::array<uint8_t, 16> out = {};
        if (lance_optimize_indices_plan_id(
                handle_.get(), out.data()) != 0)
            check_error();
        return out;
    }

    uint64_t read_version() const {
        uint64_t value =
            lance_optimize_indices_plan_read_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    size_t num_tasks() const {
        size_t count = lance_optimize_indices_plan_task_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return count;
    }

    size_t num_merges() const {
        size_t count = lance_optimize_indices_plan_merge_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return count;
    }

    OptimizeIndexTask task(size_t index) const {
        auto* task = lance_optimize_indices_plan_task_at(handle_.get(), index);
        if (!task) check_error();
        return OptimizeIndexTask(task);
    }

    OptimizeIndexMerge merge(size_t index) const {
        auto* merge =
            lance_optimize_indices_plan_merge_at(handle_.get(), index);
        if (!merge) check_error();
        return OptimizeIndexMerge(merge);
    }

    std::string json() const {
        const char* raw = lance_optimize_indices_plan_json(handle_.get());
        if (!raw) check_error();
        std::string out(raw);
        lance_free_string(raw);
        return out;
    }

    static OptimizeIndicesPlan from_json(std::string_view json) {
        std::string owned(json);
        auto* plan = lance_optimize_indices_plan_from_json(owned.c_str());
        if (!plan) check_error();
        return OptimizeIndicesPlan(plan);
    }

    const LanceOptimizeIndicesPlan* c_handle() const { return handle_.get(); }
};

/// Additive standalone IVF/PQ trainer controls.
struct IndexTrainerOptions {
    std::optional<uint64_t> sample_rate;
    std::optional<uint64_t> max_iters;
};

/// Additive scalar-index build controls.
struct ScalarIndexOptions {
    std::optional<bool> train;
};

/// Prewarm controls. An engaged empty segment list maps to Some(empty) and
/// performs no segment work.
struct IndexPrewarmOptions {
    bool with_position = false;
    std::optional<std::vector<std::array<uint8_t, 16>>> index_segments;
};

// ─── MemWAL / LSM ───────────────────────────────────────────────────────────

/// Native distance metric for MemWAL vector search.
enum class MemWalDistanceType : int32_t {
    L2 = LANCE_MEM_WAL_DISTANCE_L2,
    Cosine = LANCE_MEM_WAL_DISTANCE_COSINE,
    Dot = LANCE_MEM_WAL_DISTANCE_DOT,
    Hamming = LANCE_MEM_WAL_DISTANCE_HAMMING,
};

/// A compacted SSTable marker advanced atomically with a merge-insert commit.
struct CompactedSsTable {
    std::array<uint8_t, 16> shard_id{};
    uint64_t generation = 0;

    static CompactedSsTable from_uuid(const std::string& shard_id,
                                      uint64_t generation) {
        ::LanceCompactedSsTable raw{};
        if (lance_mem_wal_compacted_sstable_new(shard_id.c_str(), generation,
                                                &raw) != 0) {
            check_error();
        }
        CompactedSsTable value;
        std::memcpy(value.shard_id.data(), raw.shard_id,
                    value.shard_id.size());
        value.generation = raw.generation;
        return value;
    }

    ::LanceCompactedSsTable to_c() const {
        ::LanceCompactedSsTable raw{};
        std::memcpy(raw.shard_id, shard_id.data(), shard_id.size());
        raw.generation = generation;
        return raw;
    }
};

/// One SSTable referenced by a shard snapshot.
struct SsTable {
    uint64_t generation = 0;
    std::string path;
};

/// Per-index HNSW build controls.
struct HnswParams {
    std::string index_name;
    std::optional<uint32_t> num_edges;
    std::optional<uint32_t> ef_construction;
    std::optional<uint32_t> max_level;
};

/// Snapshot of native shard-writer counters.
struct ShardWriterStats {
    uint64_t put_count = 0;
    uint64_t put_time_nanos = 0;
    uint64_t wal_flush_count = 0;
    uint64_t wal_flush_time_nanos = 0;
    uint64_t wal_flush_bytes = 0;
    uint64_t wal_io_time_nanos = 0;
    uint64_t wal_io_count = 0;
    uint64_t index_update_time_nanos = 0;
    uint64_t index_update_count = 0;
    uint64_t index_update_rows = 0;
    uint64_t memtable_flush_count = 0;
    uint64_t memtable_flush_time_nanos = 0;
    uint64_t memtable_flush_rows = 0;
    uint64_t memtable_flush_failures = 0;
};

/// Snapshot of live MemTable and memory statistics.
struct ShardMemTableStats {
    bool is_live = false;
    uint64_t row_count = 0;
    uint64_t batch_count = 0;
    uint64_t estimated_size_bytes = 0;
    uint64_t index_bytes = 0;
    uint64_t frozen_bytes = 0;
    uint64_t grace_bytes = 0;
    uint64_t retained_bytes = 0;
    uint64_t generation = 0;
    std::optional<uint64_t> max_buffered_batch_position;
    uint64_t durable_batch_count = 0;
    uint64_t global_offset = 0;
    std::optional<uint64_t> pending_wal_start_batch_position;
    std::optional<uint64_t> pending_wal_end_batch_position;
    uint64_t pending_wal_batch_count = 0;
    uint64_t pending_wal_row_count = 0;
    uint64_t pending_wal_estimated_bytes = 0;
    uint64_t frozen_count = 0;
};

/// Native put/delete batch-position range.
struct ShardWriteResult {
    uint64_t batch_position_begin = 0;
    uint64_t batch_position_end = 0;
};

/// Optional overrides for a native `ShardWriterConfig`. Unset fields keep the
/// native defaults. `hnsw_params` entries replace same-named native defaults.
class ShardWriterConfig {
public:
    std::optional<bool> durable_write;
    std::optional<uint64_t> max_wal_buffer_size;
    std::optional<uint64_t> max_wal_flush_interval_ms;
    std::optional<uint64_t> max_memtable_size;
    std::optional<uint64_t> max_memtable_rows;
    std::optional<uint64_t> max_memtable_batches;
    std::optional<uint64_t> max_unflushed_memtable_bytes;
    std::optional<uint64_t> manifest_scan_batch_size;
    std::optional<uint64_t> backpressure_log_interval_ms;
    std::optional<uint64_t> stats_log_interval_ms;
    std::vector<HnswParams> hnsw_params;

    ShardWriterConfig& set_durable_write(bool value) { durable_write = value; return *this; }
    ShardWriterConfig& set_max_wal_buffer_size(uint64_t value) { max_wal_buffer_size = value; return *this; }
    ShardWriterConfig& set_max_wal_flush_interval_ms(uint64_t value) { max_wal_flush_interval_ms = value; return *this; }
    ShardWriterConfig& set_max_memtable_size(uint64_t value) { max_memtable_size = value; return *this; }
    ShardWriterConfig& set_max_memtable_rows(uint64_t value) { max_memtable_rows = value; return *this; }
    ShardWriterConfig& set_max_memtable_batches(uint64_t value) { max_memtable_batches = value; return *this; }
    ShardWriterConfig& set_max_unflushed_memtable_bytes(uint64_t value) { max_unflushed_memtable_bytes = value; return *this; }
    ShardWriterConfig& set_manifest_scan_batch_size(uint64_t value) { manifest_scan_batch_size = value; return *this; }
    ShardWriterConfig& set_backpressure_log_interval_ms(uint64_t value) { backpressure_log_interval_ms = value; return *this; }
    ShardWriterConfig& set_stats_log_interval_ms(uint64_t value) { stats_log_interval_ms = value; return *this; }
    ShardWriterConfig& add_hnsw_params(HnswParams value) { hnsw_params.push_back(std::move(value)); return *this; }

    LanceShardWriterConfig to_c(std::vector<std::string>& names,
                                std::vector<::LanceMemWalHnswParams>& params) const {
        LanceShardWriterConfig raw{};
        if (durable_write) {
            raw.has_durable_write = 1;
            raw.durable_write = *durable_write ? 1 : 0;
        }
        auto set_u64 = [](std::optional<uint64_t> value, int32_t& has, uint64_t& out) {
            if (value) { has = 1; out = *value; }
        };
        set_u64(max_wal_buffer_size, raw.has_max_wal_buffer_size, raw.max_wal_buffer_size);
        set_u64(max_wal_flush_interval_ms, raw.has_max_wal_flush_interval_ms,
                raw.max_wal_flush_interval_ms);
        set_u64(max_memtable_size, raw.has_max_memtable_size, raw.max_memtable_size);
        set_u64(max_memtable_rows, raw.has_max_memtable_rows, raw.max_memtable_rows);
        set_u64(max_memtable_batches, raw.has_max_memtable_batches, raw.max_memtable_batches);
        set_u64(max_unflushed_memtable_bytes, raw.has_max_unflushed_memtable_bytes,
                raw.max_unflushed_memtable_bytes);
        set_u64(manifest_scan_batch_size, raw.has_manifest_scan_batch_size,
                raw.manifest_scan_batch_size);
        set_u64(backpressure_log_interval_ms, raw.has_backpressure_log_interval_ms,
                raw.backpressure_log_interval_ms);
        set_u64(stats_log_interval_ms, raw.has_stats_log_interval_ms,
                raw.stats_log_interval_ms);
        names.clear();
        names.reserve(hnsw_params.size());
        for (const auto& entry : hnsw_params) names.push_back(entry.index_name);
        params.clear();
        params.reserve(hnsw_params.size());
        for (size_t i = 0; i < hnsw_params.size(); ++i) {
            const auto& entry = hnsw_params[i];
            ::LanceMemWalHnswParams raw_entry{};
            raw_entry.index_name = names[i].c_str();
            if (entry.num_edges) {
                raw_entry.has_num_edges = 1;
                raw_entry.num_edges = *entry.num_edges;
            }
            if (entry.ef_construction) {
                raw_entry.has_ef_construction = 1;
                raw_entry.ef_construction = *entry.ef_construction;
            }
            if (entry.max_level) {
                raw_entry.has_max_level = 1;
                raw_entry.max_level = *entry.max_level;
            }
            params.push_back(raw_entry);
        }
        raw.hnsw_params = params.empty() ? nullptr : params.data();
        raw.num_hnsw_params = params.size();
        return raw;
    }
};

/// Options for `Dataset::initialize_mem_wal`.
struct MemWalInitOptions {
    ::LanceMemWalShardingMode mode = LANCE_MEM_WAL_SHARDING_MANUAL;
    std::optional<std::string> column;
    uint32_t num_buckets = 0;
    /// Unset keeps native maintain-all behavior; an empty engaged vector
    /// maintains no indexes.
    std::optional<std::vector<std::string>> maintained_indexes;
    std::optional<ShardWriterConfig> writer_config;
};

/// Owned native sharding field.
class ShardingField {
    Handle<::LanceMemWalShardingField, lance_mem_wal_sharding_field_free> handle_;

public:
    explicit ShardingField(::LanceMemWalShardingField* field) : handle_(field) {
        if (!handle_) check_error();
    }

    ShardingField(ShardingField&&) noexcept = default;
    ShardingField& operator=(ShardingField&&) noexcept = default;
    ShardingField(const ShardingField&) = delete;
    ShardingField& operator=(const ShardingField&) = delete;

    static ShardingField create(
        std::string field_id,
        std::vector<int32_t> source_ids,
        std::optional<std::string> transform,
        std::optional<std::string> expression,
        std::string result_type,
        std::map<std::string, std::string> parameters = {}) {
        std::vector<const char*> keys;
        std::vector<const char*> values;
        keys.reserve(parameters.size());
        values.reserve(parameters.size());
        for (const auto& [key, value] : parameters) {
            keys.push_back(key.c_str());
            values.push_back(value.c_str());
        }
        ::LanceMemWalShardingField* raw = nullptr;
        if (lance_mem_wal_sharding_field_new(
                field_id.c_str(),
                source_ids.empty() ? nullptr : source_ids.data(),
                source_ids.size(),
                transform ? transform->c_str() : nullptr,
                expression ? expression->c_str() : nullptr,
                result_type.c_str(),
                keys.empty() ? nullptr : keys.data(),
                values.empty() ? nullptr : values.data(),
                keys.size(),
                &raw) != 0) {
            check_error();
        }
        return ShardingField(raw);
    }

    ShardingField clone() const {
        ::LanceMemWalShardingField* raw = nullptr;
        if (lance_mem_wal_sharding_field_clone(handle_.get(), &raw) != 0) {
            check_error();
        }
        return ShardingField(raw);
    }

    std::string id() const {
        const char* value = lance_mem_wal_sharding_field_id(handle_.get());
        if (!value) check_error();
        return value;
    }

    std::vector<int32_t> source_ids() const {
        size_t count = 0;
        const int32_t* values = lance_mem_wal_sharding_field_source_ids(handle_.get(), &count);
        if (!values && count != 0) check_error();
        return values ? std::vector<int32_t>(values, values + count)
                      : std::vector<int32_t>{};
    }

    std::optional<std::string> transform() const {
        const char* value = lance_mem_wal_sharding_field_transform(handle_.get());
        if (value) return std::string(value);
        return std::nullopt;
    }

    std::optional<std::string> expression() const {
        const char* value = lance_mem_wal_sharding_field_expression(handle_.get());
        if (value) return std::string(value);
        return std::nullopt;
    }

    std::string result_type() const {
        const char* value = lance_mem_wal_sharding_field_result_type(handle_.get());
        if (!value) check_error();
        return value;
    }

    std::map<std::string, std::string> parameters() const {
        std::map<std::string, std::string> result;
        const size_t count = lance_mem_wal_sharding_field_parameter_count(handle_.get());
        for (size_t i = 0; i < count; ++i) {
            const char* key = nullptr;
            const char* value = nullptr;
            if (lance_mem_wal_sharding_field_parameter_at(
                    handle_.get(), i, &key, &value) != 0) {
                check_error();
            }
            if (!key || !value) check_error();
            result.emplace(key, value);
        }
        return result;
    }

    std::optional<std::string> parameter(const std::string& key) const {
        auto all = parameters();
        auto it = all.find(key);
        if (it == all.end()) return std::nullopt;
        return it->second;
    }

    const ::LanceMemWalShardingField* c_handle() const { return handle_.get(); }
};

/// Owned native sharding spec.
class ShardingSpec {
    Handle<::LanceMemWalShardingSpec, lance_mem_wal_sharding_spec_free> handle_;

public:
    explicit ShardingSpec(::LanceMemWalShardingSpec* spec) : handle_(spec) {
        if (!handle_) check_error();
    }

    ShardingSpec(ShardingSpec&&) noexcept = default;
    ShardingSpec& operator=(ShardingSpec&&) noexcept = default;
    ShardingSpec(const ShardingSpec&) = delete;
    ShardingSpec& operator=(const ShardingSpec&) = delete;

    static ShardingSpec create(uint32_t spec_id,
                               std::vector<ShardingField> fields) {
        std::vector<const ::LanceMemWalShardingField*> raw;
        raw.reserve(fields.size());
        for (const auto& field : fields) raw.push_back(field.c_handle());
        ::LanceMemWalShardingSpec* spec = nullptr;
        if (lance_mem_wal_sharding_spec_new(
                spec_id, raw.empty() ? nullptr : raw.data(), raw.size(), &spec) != 0) {
            check_error();
        }
        return ShardingSpec(spec);
    }

    ShardingSpec clone() const {
        ::LanceMemWalShardingSpec* raw = nullptr;
        if (lance_mem_wal_sharding_spec_clone(handle_.get(), &raw) != 0) {
            check_error();
        }
        return ShardingSpec(raw);
    }

    uint32_t id() const {
        return lance_mem_wal_sharding_spec_id(handle_.get());
    }

    size_t field_count() const {
        return lance_mem_wal_sharding_spec_field_count(handle_.get());
    }

    ShardingField field_at(size_t index) const {
        ::LanceMemWalShardingField* raw = nullptr;
        if (lance_mem_wal_sharding_spec_field_at(handle_.get(), index, &raw) != 0) {
            check_error();
        }
        return ShardingField(raw);
    }

    std::vector<ShardingField> fields() const {
        std::vector<ShardingField> result;
        const size_t count = field_count();
        result.reserve(count);
        for (size_t i = 0; i < count; ++i) result.push_back(field_at(i));
        return result;
    }

    void add_field(ShardingField field) {
        auto current = fields();
        current.push_back(std::move(field));
        auto next = ShardingSpec::create(id(), std::move(current));
        handle_ = std::move(next.handle_);
    }

    const ::LanceMemWalShardingSpec* c_handle() const { return handle_.get(); }

private:
    friend class Dataset;
};

/// Owned native shard snapshot.
class ShardSnapshot {
    Handle<::LanceMemWalShardSnapshot, lance_mem_wal_shard_snapshot_free> handle_;

public:
    explicit ShardSnapshot(::LanceMemWalShardSnapshot* snapshot) : handle_(snapshot) {
        if (!handle_) check_error();
    }

    ShardSnapshot(ShardSnapshot&&) noexcept = default;
    ShardSnapshot& operator=(ShardSnapshot&&) noexcept = default;
    ShardSnapshot(const ShardSnapshot&) = delete;
    ShardSnapshot& operator=(const ShardSnapshot&) = delete;

    static ShardSnapshot create(const std::string& shard_id) {
        ::LanceMemWalShardSnapshot* raw = nullptr;
        if (lance_mem_wal_shard_snapshot_new(shard_id.c_str(), &raw) != 0) {
            check_error();
        }
        return ShardSnapshot(raw);
    }

    ShardSnapshot clone() const {
        ::LanceMemWalShardSnapshot* raw = nullptr;
        if (lance_mem_wal_shard_snapshot_clone(handle_.get(), &raw) != 0) {
            check_error();
        }
        return ShardSnapshot(raw);
    }

    std::string shard_id() const {
        const char* value = lance_mem_wal_shard_snapshot_shard_id(handle_.get());
        if (!value) check_error();
        return value;
    }

    uint32_t spec_id() const {
        return lance_mem_wal_shard_snapshot_spec_id(handle_.get());
    }

    ShardSnapshot& set_spec_id(uint32_t value) {
        if (lance_mem_wal_shard_snapshot_with_spec_id(handle_.get(), value) != 0) {
            check_error();
        }
        return *this;
    }

    uint64_t current_generation() const {
        return lance_mem_wal_shard_snapshot_current_generation(handle_.get());
    }

    ShardSnapshot& set_current_generation(uint64_t value) {
        if (lance_mem_wal_shard_snapshot_with_current_generation(
                handle_.get(), value) != 0) {
            check_error();
        }
        return *this;
    }

    ShardSnapshot& add_sstable(uint64_t generation, const std::string& path) {
        if (lance_mem_wal_shard_snapshot_with_sstable(
                handle_.get(), generation, path.c_str()) != 0) {
            check_error();
        }
        return *this;
    }

    std::vector<SsTable> sstables() const {
        std::vector<SsTable> result;
        const size_t count = lance_mem_wal_shard_snapshot_sstable_count(handle_.get());
        result.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            uint64_t generation = 0;
            const char* path = nullptr;
            if (lance_mem_wal_shard_snapshot_sstable_at(
                    handle_.get(), i, &generation, &path) != 0) {
                check_error();
            }
            if (!path) check_error();
            result.push_back(SsTable{generation, path});
        }
        return result;
    }

    const ::LanceMemWalShardSnapshot* c_handle() const { return handle_.get(); }
};

/// Owned copy of native MemWAL index details.
class MemWalIndexDetails {
    uint32_t num_shards_ = 0;
    bool maintain_all_ = false;
    std::vector<std::string> maintained_;
    std::vector<ShardingSpec> specs_;
    std::map<std::string, std::string> defaults_;

public:
    MemWalIndexDetails(uint32_t num_shards,
                       bool maintain_all,
                       std::vector<std::string> maintained,
                       std::vector<ShardingSpec> specs,
                       std::map<std::string, std::string> defaults)
        : num_shards_(num_shards),
          maintain_all_(maintain_all),
          maintained_(std::move(maintained)),
          specs_(std::move(specs)),
          defaults_(std::move(defaults)) {}

    MemWalIndexDetails(MemWalIndexDetails&&) noexcept = default;
    MemWalIndexDetails& operator=(MemWalIndexDetails&&) noexcept = default;
    MemWalIndexDetails(const MemWalIndexDetails&) = delete;
    MemWalIndexDetails& operator=(const MemWalIndexDetails&) = delete;

    uint32_t num_shards() const { return num_shards_; }
    bool maintain_all_indexes() const { return maintain_all_; }
    const std::vector<std::string>& maintained_indexes() const { return maintained_; }
    const std::map<std::string, std::string>& writer_config_defaults() const {
        return defaults_;
    }
    size_t sharding_spec_count() const { return specs_.size(); }
    ShardingSpec sharding_spec_at(size_t index) const {
        if (index >= specs_.size()) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "sharding spec index out of range");
        }
        return specs_[index].clone();
    }
    std::vector<ShardingSpec> sharding_specs() const {
        std::vector<ShardingSpec> result;
        result.reserve(specs_.size());
        for (const auto& spec : specs_) result.push_back(spec.clone());
        return result;
    }
};

/// Owned native shard writer.
class ShardWriter {
    Handle<::LanceShardWriter, lance_shard_writer_free> handle_;

public:
    explicit ShardWriter(::LanceShardWriter* writer) : handle_(writer) {
        if (!handle_) check_error();
    }

    ~ShardWriter() = default;
    ShardWriter(ShardWriter&&) noexcept = default;
    ShardWriter& operator=(ShardWriter&&) noexcept = default;
    ShardWriter(const ShardWriter&) = delete;
    ShardWriter& operator=(const ShardWriter&) = delete;

    /// Open a native writer for one shard. The dataset handle is borrowed.
    static ShardWriter open(const Dataset& dataset,
                            const std::string& shard_id,
                            const ShardWriterConfig& config = {});

    std::string shard_id() const {
        const char* value = lance_shard_writer_shard_id(handle_.get());
        if (!value) check_error();
        return value;
    }

    ShardWriteResult put(ArrowArrayStream* stream) {
        ::LanceShardWriteResult raw{};
        if (lance_shard_writer_put(handle_.get(), stream, &raw) != 0) {
            check_error();
        }
        return ShardWriteResult{raw.batch_position_begin,
                                raw.batch_position_end};
    }

    ShardWriteResult delete_rows(ArrowArrayStream* stream) {
        ::LanceShardWriteResult raw{};
        if (lance_shard_writer_delete(handle_.get(), stream, &raw) != 0) {
            check_error();
        }
        return ShardWriteResult{raw.batch_position_begin,
                                raw.batch_position_end};
    }

    ShardWriterStats stats() const {
        ::LanceShardWriterStats raw{};
        if (lance_shard_writer_stats(handle_.get(), &raw) != 0) check_error();
        return ShardWriterStats{
            raw.put_count, raw.put_time_nanos,
            raw.wal_flush_count, raw.wal_flush_time_nanos, raw.wal_flush_bytes,
            raw.wal_io_time_nanos, raw.wal_io_count,
            raw.index_update_time_nanos, raw.index_update_count,
            raw.index_update_rows,
            raw.memtable_flush_count, raw.memtable_flush_time_nanos,
            raw.memtable_flush_rows, raw.memtable_flush_failures};
    }

    ShardMemTableStats memtable_stats() const {
        ::LanceShardMemTableStats raw{};
        if (lance_shard_writer_memtable_stats(handle_.get(), &raw) != 0) {
            check_error();
        }
        ShardMemTableStats stats;
        stats.is_live = raw.is_live != 0;
        stats.row_count = raw.row_count;
        stats.batch_count = raw.batch_count;
        stats.estimated_size_bytes = raw.estimated_size_bytes;
        stats.index_bytes = raw.index_bytes;
        stats.frozen_bytes = raw.frozen_bytes;
        stats.grace_bytes = raw.grace_bytes;
        stats.retained_bytes = raw.retained_bytes;
        stats.generation = raw.generation;
        if (raw.has_max_buffered_batch_position) {
            stats.max_buffered_batch_position = raw.max_buffered_batch_position;
        }
        stats.durable_batch_count = raw.durable_batch_count;
        stats.global_offset = raw.global_offset;
        if (raw.has_pending_wal_start_batch_position) {
            stats.pending_wal_start_batch_position =
                raw.pending_wal_start_batch_position;
        }
        if (raw.has_pending_wal_end_batch_position) {
            stats.pending_wal_end_batch_position =
                raw.pending_wal_end_batch_position;
        }
        stats.pending_wal_batch_count = raw.pending_wal_batch_count;
        stats.pending_wal_row_count = raw.pending_wal_row_count;
        stats.pending_wal_estimated_bytes = raw.pending_wal_estimated_bytes;
        stats.frozen_count = raw.frozen_count;
        return stats;
    }

    /// Snapshot the writer's latest manifest. Returns nullopt when the writer
    /// has not committed yet; an uncommitted shard is composed by the caller's
    /// scanner using a default snapshot.
    std::optional<ShardSnapshot> snapshot() const {
        ::LanceMemWalShardSnapshot* raw = nullptr;
        if (lance_shard_writer_snapshot(handle_.get(), &raw) != 0) check_error();
        if (!raw) return std::nullopt;
        return ShardSnapshot(raw);
    }

    LsmScanner lsm_scanner(std::vector<ShardSnapshot> snapshots);

    void close() {
        if (lance_shard_writer_close(handle_.get()) != 0) check_error();
    }

    const ::LanceShardWriter* c_handle() const { return handle_.get(); }
};

/// Owned native LSM scanner.
class LsmScanner {
    Handle<::LanceLsmScanner, lance_lsm_scanner_free> handle_;

public:
    explicit LsmScanner(::LanceLsmScanner* scanner) : handle_(scanner) {
        if (!handle_) check_error();
    }
    ~LsmScanner() = default;
    LsmScanner(LsmScanner&&) noexcept = default;
    LsmScanner& operator=(LsmScanner&&) noexcept = default;
    LsmScanner(const LsmScanner&) = delete;
    LsmScanner& operator=(const LsmScanner&) = delete;

    static LsmScanner from_snapshots(const Dataset& dataset,
                                     std::vector<ShardSnapshot> snapshots);

    LsmScanner& project(const std::vector<std::string>& columns) {
        std::vector<const char*> raw;
        raw.reserve(columns.size());
        for (const auto& column : columns) raw.push_back(column.c_str());
        if (lance_lsm_scanner_project(
                handle_.get(), raw.empty() ? nullptr : raw.data(),
                raw.size()) != 0) {
            check_error();
        }
        return *this;
    }

    LsmScanner& filter(const std::string& expression) {
        if (lance_lsm_scanner_filter(handle_.get(), expression.c_str()) != 0) {
            check_error();
        }
        return *this;
    }

    LsmScanner& limit(std::optional<int64_t> limit,
                      std::optional<int64_t> offset = std::nullopt) {
        if (lance_lsm_scanner_limit(
                handle_.get(), limit.value_or(0), limit.has_value(),
                offset.value_or(0), offset.has_value()) != 0) {
            check_error();
        }
        return *this;
    }

    LsmScanner& with_row_address() {
        if (lance_lsm_scanner_with_row_address(handle_.get()) != 0) {
            check_error();
        }
        return *this;
    }

    LsmScanner& with_memtable_gen() {
        if (lance_lsm_scanner_with_memtable_gen(handle_.get()) != 0) {
            check_error();
        }
        return *this;
    }

    Batch to_batch() const;
    std::vector<Batch> to_batches() const;
    Table to_table() const;
    uint64_t count_rows() const {
        uint64_t count = 0;
        if (lance_lsm_scanner_count_rows(handle_.get(), &count) != 0) {
            check_error();
        }
        return count;
    }

    ArrowStream to_reader() const {
        ArrowArrayStream raw{};
        if (lance_lsm_scanner_to_arrow_stream(handle_.get(), &raw) != 0) {
            check_error();
        }
        return ArrowStream(raw);
    }

    const ::LanceLsmScanner* c_handle() const { return handle_.get(); }
};

/// Owned native execution plan.
class ExecutionPlan {
    Handle<::LanceMemWalPlan, lance_mem_wal_plan_free> handle_;

public:
    explicit ExecutionPlan(::LanceMemWalPlan* plan) : handle_(plan) {
        if (!handle_) check_error();
    }
    ~ExecutionPlan() = default;
    ExecutionPlan(ExecutionPlan&&) noexcept = default;
    ExecutionPlan& operator=(ExecutionPlan&&) noexcept = default;
    ExecutionPlan(const ExecutionPlan&) = delete;
    ExecutionPlan& operator=(const ExecutionPlan&) = delete;

    ArrowSchemaOwner schema() const;
    ArrowSchemaOwner dataset_schema() const;

    std::string explain() const {
        char* text = nullptr;
        if (lance_mem_wal_plan_explain(handle_.get(), &text) != 0) {
            check_error();
        }
        std::unique_ptr<char, decltype(&lance_free_string)> guard(text, lance_free_string);
        return std::string(text);
    }

    Batch to_batch() const;
    std::vector<Batch> to_batches() const;
    Table to_table() const;

    ArrowStream to_reader() const {
        ArrowArrayStream raw{};
        if (lance_mem_wal_plan_to_arrow_stream(handle_.get(), &raw) != 0) {
            check_error();
        }
        return ArrowStream(raw);
    }

    const ::LanceMemWalPlan* c_handle() const { return handle_.get(); }
};

/// Owned native point-lookup planner.
class LsmPointLookupPlanner {
    Handle<::LanceLsmPointLookupPlanner, lance_lsm_point_lookup_planner_free> handle_;

public:
    LsmPointLookupPlanner(const Dataset& dataset,
                          std::vector<ShardSnapshot> snapshots,
                          std::optional<std::vector<std::string>> pk_columns = std::nullopt);

    ~LsmPointLookupPlanner() = default;
    LsmPointLookupPlanner(LsmPointLookupPlanner&&) noexcept = default;
    LsmPointLookupPlanner& operator=(LsmPointLookupPlanner&&) noexcept = default;
    LsmPointLookupPlanner(const LsmPointLookupPlanner&) = delete;
    LsmPointLookupPlanner& operator=(const LsmPointLookupPlanner&) = delete;

    ExecutionPlan plan_lookup(
        const Batch& pk_value,
        std::optional<std::vector<std::string>> columns = std::nullopt) const;

private:
    explicit LsmPointLookupPlanner(::LanceLsmPointLookupPlanner* planner)
        : handle_(planner) {
        if (!handle_) check_error();
    }
    friend class Dataset;
};

/// Owned native vector-search planner.
class LsmVectorSearchPlanner {
    Handle<::LanceLsmVectorSearchPlanner, lance_lsm_vector_search_planner_free> handle_;

public:
    LsmVectorSearchPlanner(const Dataset& dataset,
                           std::vector<ShardSnapshot> snapshots,
                           std::string vector_column,
                           std::optional<std::vector<std::string>> pk_columns = std::nullopt,
                           MemWalDistanceType distance_type = MemWalDistanceType::L2,
                           std::optional<std::string> filter = std::nullopt);

    ~LsmVectorSearchPlanner() = default;
    LsmVectorSearchPlanner(LsmVectorSearchPlanner&&) noexcept = default;
    LsmVectorSearchPlanner& operator=(LsmVectorSearchPlanner&&) noexcept = default;
    LsmVectorSearchPlanner(const LsmVectorSearchPlanner&) = delete;
    LsmVectorSearchPlanner& operator=(const LsmVectorSearchPlanner&) = delete;

    ExecutionPlan plan_search(
        const std::vector<float>& query,
        uint64_t k = 10,
        uint64_t nprobes = 20,
        std::optional<std::vector<std::string>> columns = std::nullopt,
        bool refine_base_table = false,
        double overfetch_factor = 1.0) const;

private:
    explicit LsmVectorSearchPlanner(::LanceLsmVectorSearchPlanner* planner)
        : handle_(planner) {
        if (!handle_) check_error();
    }
    friend class Dataset;
};

// ─── Dataset ─────────────────────────────────────────────────────────────────

class Dataset {
    Handle<LanceDataset, lance_dataset_close> handle_;

    /// Own the caller's stream until a C entry point consumes it, and own the
    /// stream-provided schema for the duration of that call. This preserves the
    /// "consumed on return or throw" contract even when schema extraction or
    /// option conversion throws before the Rust call.
    class ArrowStreamInput {
        ArrowArrayStream* stream_;
        bool armed_ = true;
        ArrowSchema schema_ = {};

    public:
        explicit ArrowStreamInput(ArrowArrayStream* stream) : stream_(stream) {
            if (!stream_) {
                throw Error(LANCE_ERR_INVALID_ARGUMENT,
                            "stream must not be null");
            }
        }

        ~ArrowStreamInput() {
            if (schema_.release) schema_.release(&schema_);
            if (armed_ && stream_ && stream_->release) {
                stream_->release(stream_);
            }
        }

        ArrowStreamInput(const ArrowStreamInput&) = delete;
        ArrowStreamInput& operator=(const ArrowStreamInput&) = delete;
        ArrowStreamInput(ArrowStreamInput&&) = delete;
        ArrowStreamInput& operator=(ArrowStreamInput&&) = delete;

        const ArrowSchema* read_schema() {
            if (!stream_->get_schema) {
                throw Error(LANCE_ERR_INVALID_ARGUMENT,
                            "stream get_schema callback is null");
            }
            if (stream_->get_schema(stream_, &schema_) != 0) {
                const char* error = stream_->get_last_error
                    ? stream_->get_last_error(stream_)
                    : nullptr;
                throw Error(
                    LANCE_ERR_INVALID_ARGUMENT,
                    std::string("failed to read stream schema: ") +
                        (error ? error : "unknown"));
            }
            return &schema_;
        }

        ArrowArrayStream* consume() noexcept {
            armed_ = false;
            return stream_;
        }
    };

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

    static LanceWriteOptions to_c_write_options(
        const WriteOptions& options,
        LanceAutoCleanupParams& cleanup) {
        LanceWriteOptions c_options = {};
        c_options.params.max_rows_per_file = options.params.max_rows_per_file;
        c_options.params.max_rows_per_group = options.params.max_rows_per_group;
        c_options.params.max_bytes_per_file = options.params.max_bytes_per_file;
        c_options.params.data_storage_version =
            options.params.data_storage_version
                ? options.params.data_storage_version->c_str()
                : nullptr;
        c_options.params.enable_stable_row_ids =
            options.params.enable_stable_row_ids;
        c_options.skip_auto_cleanup = options.skip_auto_cleanup;
        if (options.auto_cleanup) {
            cleanup.interval = options.auto_cleanup->interval;
            cleanup.older_than_seconds =
                options.auto_cleanup->older_than_seconds;
            cleanup.older_than_nanos = options.auto_cleanup->older_than_nanos;
            c_options.auto_cleanup = &cleanup;
        }
        return c_options;
    }

    static LanceUncommittedWriteOptions to_c_uncommitted_write_options(
        const UncommittedWriteOptions& options,
        LanceAutoCleanupParams& cleanup,
        std::vector<const char*>& properties) {

        LanceWriteOptions c_write = to_c_write_options(options.write, cleanup);
        if (options.transaction_properties) {
            properties.reserve(options.transaction_properties->size() * 2 + 1);
            for (const auto& [key, value] : *options.transaction_properties) {
                properties.push_back(key.c_str());
                properties.push_back(value.c_str());
            }
            properties.push_back(nullptr);
        }
        LanceUncommittedWriteOptions c_options = {};
        c_options.write = c_write;
        c_options.transaction_properties =
            options.transaction_properties ? properties.data() : nullptr;
        return c_options;
    }

    static LanceCommitOptions to_c_commit_options(const CommitOptions& options) {
        LanceCommitOptions c_options = {};
        if (options.use_stable_row_ids) {
            c_options.has_use_stable_row_ids = true;
            c_options.use_stable_row_ids = *options.use_stable_row_ids;
        }
        if (options.enable_v2_manifest_paths) {
            c_options.has_enable_v2_manifest_paths = true;
            c_options.enable_v2_manifest_paths =
                *options.enable_v2_manifest_paths;
        }
        if (options.max_retries) {
            c_options.has_max_retries = true;
            c_options.max_retries = *options.max_retries;
        }
        if (options.retry_timeout) {
            c_options.has_retry_timeout = true;
            c_options.retry_timeout_seconds = options.retry_timeout->seconds;
            c_options.retry_timeout_nanos = options.retry_timeout->nanos;
        }
        c_options.timeout_mode = static_cast<int32_t>(options.timeout_mode);
        c_options.timeout_seconds = options.timeout.seconds;
        c_options.timeout_nanos = options.timeout.nanos;
        c_options.detached = options.detached;
        c_options.skip_auto_cleanup = options.skip_auto_cleanup;
        c_options.data_storage_version =
            options.data_storage_version
                ? options.data_storage_version->c_str()
                : nullptr;
        return c_options;
    }

    static RemovalStats from_c_removal_stats(const LanceRemovalStats& stats) {
        RemovalStats out;
        out.bytes_removed = stats.bytes_removed;
        out.old_versions = stats.old_versions;
        out.data_files_removed = stats.data_files_removed;
        out.transaction_files_removed = stats.transaction_files_removed;
        out.index_files_removed = stats.index_files_removed;
        out.deletion_files_removed = stats.deletion_files_removed;
        out.failed_deletes = stats.failed_deletes;
        return out;
    }

    static LanceCleanupPolicy to_c_cleanup_policy(
        const CleanupPolicy& policy,
        std::vector<uint64_t>& versions,
        uint64_t& empty_versions_sentinel) {
        LanceCleanupPolicy out{};
        if (policy.before_timestamp) {
            out.has_before_timestamp = 1;
            out.before_timestamp_seconds = policy.before_timestamp->seconds;
            out.before_timestamp_nanos = policy.before_timestamp->nanos;
        }
        if (policy.before_version) {
            out.has_before_version = 1;
            out.before_version = *policy.before_version;
        }
        if (policy.versions) {
            versions = *policy.versions;
            out.versions =
                versions.empty() ? &empty_versions_sentinel : versions.data();
            out.num_versions = versions.size();
        }
        if (policy.retain_versions) {
            out.has_retain_versions = 1;
            out.retain_versions = *policy.retain_versions;
        }
        if (policy.delete_unverified) {
            out.has_delete_unverified = 1;
            out.delete_unverified = *policy.delete_unverified;
        }
        if (policy.error_if_tagged_old_versions) {
            out.has_error_if_tagged_old_versions = 1;
            out.error_if_tagged_old_versions =
                *policy.error_if_tagged_old_versions;
        }
        if (policy.clean_referenced_branches) {
            out.has_clean_referenced_branches = 1;
            out.clean_referenced_branches =
                *policy.clean_referenced_branches;
        }
        if (policy.delete_rate_limit) {
            out.has_delete_rate_limit = 1;
            out.delete_rate_limit = *policy.delete_rate_limit;
        }
        return out;
    }

    static LanceCleanupOperationOptions to_c_cleanup_operation(
        const CleanupOperationOptions& options) {
        LanceCleanupOperationOptions out{};
        if (options.delete_concurrency) {
            out.has_delete_concurrency = 1;
            out.delete_concurrency = *options.delete_concurrency;
        }
        if (options.delete_rate_limit) {
            out.has_delete_rate_limit = 1;
            out.delete_rate_limit = *options.delete_rate_limit;
        }
        if (options.max_candidate_files) {
            out.has_max_candidate_files = 1;
            out.max_candidate_files = *options.max_candidate_files;
        }
        return out;
    }

    static CleanupExplanation read_cleanup_explanation(
        const LanceCleanupExplanation* explanation) {
        CleanupExplanation out;
        out.read_version =
            lance_cleanup_explanation_read_version(explanation);
        if (lance_last_error_code() != LANCE_OK) check_error();

        LanceRemovalStats stats{};
        if (lance_cleanup_explanation_stats(explanation, &stats) != 0)
            check_error();
        out.stats = from_c_removal_stats(stats);

        const uint64_t candidate_count =
            lance_cleanup_explanation_candidate_count(explanation);
        if (lance_last_error_code() != LANCE_OK) check_error();
        out.candidate_files.reserve(static_cast<size_t>(candidate_count));
        for (uint64_t i = 0; i < candidate_count; ++i) {
            const auto index = static_cast<size_t>(i);
            CleanupCandidateFile candidate;
            const char* path =
                lance_cleanup_explanation_candidate_path_at(explanation, index);
            if (!path) check_error();
            candidate.path = path;

            const int32_t kind =
                lance_cleanup_explanation_candidate_kind_at(explanation, index);
            if (kind < 0) check_error();
            candidate.kind = static_cast<CleanupFileKind>(kind);
            const int32_t unverified =
                lance_cleanup_explanation_candidate_unverified_at(
                    explanation, index);
            if (unverified < 0) check_error();
            candidate.unverified = unverified != 0;
            candidate.size_bytes =
                lance_cleanup_explanation_candidate_size_bytes_at(
                    explanation, index);
            if (lance_last_error_code() != LANCE_OK) check_error();
            out.candidate_files.push_back(std::move(candidate));
        }

        const int32_t truncated =
            lance_cleanup_explanation_candidate_files_truncated(explanation);
        if (truncated < 0) check_error();
        out.candidate_files_truncated = truncated != 0;
        out.candidate_file_limit =
            lance_cleanup_explanation_candidate_file_limit(explanation);
        if (lance_last_error_code() != LANCE_OK) check_error();

        const uint64_t branch_count =
            lance_cleanup_explanation_referenced_branch_count(explanation);
        if (lance_last_error_code() != LANCE_OK) check_error();
        out.referenced_branches.reserve(static_cast<size_t>(branch_count));
        for (uint64_t i = 0; i < branch_count; ++i) {
            const auto index = static_cast<size_t>(i);
            CleanupReferencedBranch branch;
            const char* name =
                lance_cleanup_explanation_referenced_branch_name_at(
                    explanation, index);
            if (!name) check_error();
            branch.name = name;
            branch.referenced_version =
                lance_cleanup_explanation_referenced_branch_version_at(
                    explanation, index);
            if (lance_last_error_code() != LANCE_OK) check_error();
            const int32_t candidate =
                lance_cleanup_explanation_referenced_branch_cleanup_candidate_at(
                    explanation, index);
            if (candidate < 0) check_error();
            branch.cleanup_candidate = candidate != 0;
            out.referenced_branches.push_back(std::move(branch));
        }

        const uint64_t warning_count =
            lance_cleanup_explanation_warning_count(explanation);
        if (lance_last_error_code() != LANCE_OK) check_error();
        out.warnings.reserve(static_cast<size_t>(warning_count));
        for (uint64_t i = 0; i < warning_count; ++i) {
            const char* warning = lance_cleanup_explanation_warning_at(
                explanation, static_cast<size_t>(i));
            if (!warning) check_error();
            out.warnings.emplace_back(warning);
        }
        return out;
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
    /// Leak-freedom also rests on the C side: all `lance_dataset_take_blobs*`
    /// entry points fill `out` all-or-nothing and leave it untouched on error.
    /// A partial fill before an error would leak, because `check_error()`
    /// throws before this runs and `raw` owns nothing.
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

    static void run_field_metadata_update(
        LanceDataset* dataset,
        const std::vector<FieldMetadataUpdate>& updates) {

        std::vector<std::vector<const char*>> keys(updates.size());
        std::vector<std::vector<const char*>> values(updates.size());
        std::vector<LanceFieldMetadataUpdate> raw;
        raw.reserve(updates.size());

        for (size_t i = 0; i < updates.size(); ++i) {
            keys[i].reserve(updates[i].values.size());
            values[i].reserve(updates[i].values.size());
            for (const auto& [key, value] : updates[i].values) {
                keys[i].push_back(key.c_str());
                values[i].push_back(value ? value->c_str() : nullptr);
            }
            raw.push_back(LanceFieldMetadataUpdate{
                updates[i].field.c_handle(),
                keys[i].data(),
                values[i].data(),
                updates[i].values.size(),
                static_cast<LanceMetadataUpdateMode>(updates[i].mode),
            });
        }

        if (lance_dataset_update_field_metadata(
                dataset,
                updates.empty() ? nullptr : raw.data(),
                updates.size()) != 0) {
            check_error();
        }
    }

    struct TrackedFilesProgressCapture {
        TrackedFilesProgressCallback callback;
    };

    static void tracked_files_progress_trampoline(
        void* callback_ctx,
        uint64_t manifests_processed,
        const uint64_t* manifests_total) noexcept {
        auto* capture =
            static_cast<TrackedFilesProgressCapture*>(callback_ctx);
        std::optional<uint64_t> total;
        if (manifests_total != nullptr) {
            total = *manifests_total;
        }
        capture->callback(manifests_processed, total);
    }

    static void tracked_files_progress_release(
        void* callback_ctx) noexcept {
        delete static_cast<TrackedFilesProgressCapture*>(callback_ctx);
    }

    static std::string take_owned_string(const char* raw) {
        if (!raw) check_error();
        std::string out(raw);
        lance_free_string(raw);
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
        const std::vector<std::pair<std::string, std::string>>& storage_opts,
        const StorageOptionsAccessor* storage_options_accessor = nullptr) {

        ArrowStreamInput input(stream);
        const ArrowSchema* schema = input.read_schema();

        std::vector<const char*> kv;
        for (auto& [k, v] : storage_opts) {
            kv.push_back(k.c_str());
            kv.push_back(v.c_str());
        }
        kv.push_back(nullptr);
        const char* const* opts_ptr =
            storage_opts.empty() ? nullptr : kv.data();

        // The C API consumes the stream on every return path.
        ArrowArrayStream* owned_stream = input.consume();

        LanceDataset* out = nullptr;
        int32_t rc;
        const char* entry_point;
        if (storage_options_accessor) {
            if (!storage_opts.empty()) {
                throw Error(
                    LANCE_ERR_INVALID_ARGUMENT,
                    "storage_opts must be empty when a storage options "
                    "accessor is provided");
            }
            if (native_options) {
                entry_point = "lance_dataset_write_with_options_and_accessor";
                rc = lance_dataset_write_with_options_and_accessor(
                    uri.c_str(),
                    schema,
                    owned_stream,
                    static_cast<int32_t>(mode),
                    native_options,
                    storage_options_accessor->c_handle(),
                    &out);
            } else {
                entry_point = "lance_dataset_write_with_params_and_accessor";
                rc = lance_dataset_write_with_params_and_accessor(
                    uri.c_str(),
                    schema,
                    owned_stream,
                    static_cast<int32_t>(mode),
                    legacy_params,
                    storage_options_accessor->c_handle(),
                    &out);
            }
        } else if (native_options) {
            entry_point = "lance_dataset_write_with_options";
            rc = lance_dataset_write_with_options(
                uri.c_str(),
                schema,
                owned_stream,
                static_cast<int32_t>(mode),
                native_options,
                opts_ptr,
                &out);
        } else {
            entry_point = "lance_dataset_write_with_params";
            rc = lance_dataset_write_with_params(
                uri.c_str(),
                schema,
                owned_stream,
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

    /// Open a dataset with additive options and an optional shared session.
    ///
    /// A NULL `storage_options_accessor` selects native storage defaults. A
    /// non-NULL accessor is retained by the returned dataset, so the C++
    /// wrapper may be destroyed immediately after this call; later I/O uses
    /// the native provider refresh, error and cache policy.
    static Dataset open_with_options(
        const std::string& uri,
        const DatasetOpenOptions& options = {},
        const Session* session = nullptr) {

        LanceDatasetOpenOptions c_options = {};
        c_options.version = options.version;
        c_options.storage_options_accessor =
            options.storage_options_accessor
                ? options.storage_options_accessor->c_handle()
                : nullptr;
        auto* ds = lance_dataset_open_with_options(
            uri.c_str(),
            &c_options,
            session ? session->c_handle() : nullptr);
        if (!ds) check_error();
        return Dataset(ds);
    }

    /// Open through a foreign commit lock and optional dynamic storage accessor.
    ///
    /// The returned dataset retains the native handler and accessor owners, so
    /// destroying either input afterwards does not invalidate this dataset,
    /// its scanners, or later commits. NULL pointers keep native defaults.
    static Dataset open_with_commit_lock(
        const std::string& uri,
        const StorageOptionsAccessor* storage_accessor,
        uint64_t version,
        const Session* session,
        const CommitLock* commit_lock) {

        LanceDataset* ds = nullptr;
        int32_t rc = lance_dataset_open_with_commit_lock(
            uri.c_str(),
            storage_accessor ? storage_accessor->c_handle() : nullptr,
            version,
            session ? session->c_handle() : nullptr,
            commit_lock ? commit_lock->c_handle() : nullptr,
            &ds);
        if (rc != 0) check_error();
        if (ds == nullptr) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_dataset_open_with_commit_lock returned success "
                        "with a null dataset");
        }
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

        LanceAutoCleanupParams c_cleanup = {};
        LanceWriteOptions c_options = to_c_write_options(options, c_cleanup);

        return write_impl(
            uri, stream, mode, nullptr, &c_options, storage_opts);
    }

    /// Write with a retained native dynamic storage-options accessor. This
    /// keeps the legacy 20/14 auto-cleanup policy. The returned dataset retains
    /// the accessor and provider for later I/O.
    static Dataset write_with_accessor(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const StorageOptionsAccessor& storage_options_accessor) {

        return write_impl(
            uri,
            stream,
            mode,
            nullptr,
            nullptr,
            {},
            &storage_options_accessor);
    }

    /// Write with explicit legacy `WriteParams` and a retained native dynamic
    /// storage-options accessor.
    static Dataset write_with_params_and_accessor(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const WriteParams& params,
        const StorageOptionsAccessor& storage_options_accessor) {

        LanceWriteParams c_params = {};
        c_params.max_rows_per_file = params.max_rows_per_file;
        c_params.max_rows_per_group = params.max_rows_per_group;
        c_params.max_bytes_per_file = params.max_bytes_per_file;
        c_params.data_storage_version =
            params.data_storage_version ? params.data_storage_version->c_str()
                                        : nullptr;
        c_params.enable_stable_row_ids = params.enable_stable_row_ids;

        return write_impl(
            uri,
            stream,
            mode,
            &c_params,
            nullptr,
            {},
            &storage_options_accessor);
    }

    /// Write with native `WriteParams::default()` semantics and a retained
    /// native dynamic storage-options accessor.
    static Dataset write_with_options_and_accessor(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const WriteOptions& options,
        const StorageOptionsAccessor& storage_options_accessor) {

        LanceAutoCleanupParams c_cleanup = {};
        LanceWriteOptions c_options = to_c_write_options(options, c_cleanup);

        return write_impl(
            uri,
            stream,
            mode,
            nullptr,
            &c_options,
            {},
            &storage_options_accessor);
    }

    /// Write using native defaults through a foreign commit lock and optional
    /// dynamic storage accessor.
    ///
    /// The accessor and lock owners are retained by the committed dataset.
    /// NULL pointers keep native defaults. The stream is consumed on every
    /// return path. Throws `lance::Error` on failure.
    static Dataset write_with_options_with_commit_lock(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const WriteOptions& options,
        const StorageOptionsAccessor* storage_accessor,
        const Session* session,
        const CommitLock* commit_lock) {

        ArrowStreamInput input(stream);
        const ArrowSchema* schema = input.read_schema();

        LanceAutoCleanupParams c_cleanup = {};
        LanceWriteOptions c_options = to_c_write_options(options, c_cleanup);

        ArrowArrayStream* owned_stream = input.consume();
        LanceDataset* out = nullptr;
        if (lance_dataset_write_with_options_with_commit_lock(
                uri.c_str(),
                schema,
                owned_stream,
                static_cast<int32_t>(mode),
                &c_options,
                storage_accessor ? storage_accessor->c_handle() : nullptr,
                session ? session->c_handle() : nullptr,
                commit_lock ? commit_lock->c_handle() : nullptr,
                &out) != 0) {
            check_error();
        }
        if (!out) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_dataset_write_with_options_with_commit_lock returned "
                "success with null out_dataset");
        }
        return Dataset(out);
    }

    /// Open through a native namespace. Namespace location/storage options and
    /// managed-version behavior follow `DatasetBuilder::from_namespace`.
    static Dataset open_namespace(
        const Namespace& ns,
        const std::vector<std::string>& table_id,
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {},
        uint64_t version = 0,
        const Session* session = nullptr);

    /// Write into a namespace-managed table. CREATE uses native declare_table;
    /// APPEND/OVERWRITE use native describe/commit-handler behavior. A
    /// non-empty branch selects native write_into_namespace_on_branch. Native
    /// write defaults apply (the legacy 20/14 cleanup policy is not added).
    static Dataset write_namespace(
        const Namespace& ns,
        const std::vector<std::string>& table_id,
        ArrowArrayStream* stream,
        WriteMode mode,
        const WriteOptions& options = {},
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {},
        const std::optional<std::string>& branch = std::nullopt,
        const Session* session = nullptr);

    /// Commit a transaction through a namespace-managed table URI using the
    /// native external manifest store and existing CommitBuilder semantics.
    static Dataset commit_namespace_uri(
        const Namespace& ns,
        const std::vector<std::string>& table_id,
        const std::string& uri,
        const Transaction& transaction,
        const CommitOptions& options = {},
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {},
        const Session* session = nullptr);

    /// Write an Arrow stream without committing, returning the owned native
    /// transaction for later publication with `Dataset::commit`.
    ///
    /// The stream is consumed on success and on every C-side error path. The
    /// C++ guard releases it if schema extraction or local conversion throws
    /// before the call. `session` is borrowed only for this call; native values
    /// retain shared ownership of its caches.
    static Transaction write_uncommitted(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const UncommittedWriteOptions& options = {},
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {},
        const Session* session = nullptr) {

        ArrowStreamInput input(stream);
        const ArrowSchema* schema = input.read_schema();

        LanceAutoCleanupParams cleanup = {};
        std::vector<const char*> properties;
        LanceUncommittedWriteOptions c_options =
            to_c_uncommitted_write_options(options, cleanup, properties);

        auto storage_kv = to_c_key_values(storage_opts);
        const char* const* storage_ptr =
            storage_opts.empty() ? nullptr : storage_kv.data();

        ArrowArrayStream* owned_stream = input.consume();
        LanceTransaction* transaction = nullptr;
        if (lance_dataset_write_uncommitted_stream(
                uri.c_str(),
                schema,
                owned_stream,
                static_cast<int32_t>(mode),
                &c_options,
                storage_ptr,
                session ? session->c_handle() : nullptr,
                &transaction) != 0) {
            check_error();
        }
        if (!transaction) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_dataset_write_uncommitted_stream returned success with "
                "null out_transaction");
        }
        return Transaction(transaction);
    }

    /// Write uncommitted data with a native dynamic accessor for data-file
    /// writes performed by this call.
    ///
    /// The returned transaction carries transaction metadata only and does not
    /// retain the accessor; pass it again to `commit_with_accessor` when commit
    /// I/O needs dynamic storage options. All other stream and transaction
    /// semantics match `write_uncommitted`.
    static Transaction write_uncommitted_with_accessor(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const StorageOptionsAccessor& storage_options_accessor,
        const UncommittedWriteOptions& options = {},
        const Session* session = nullptr) {

        ArrowStreamInput input(stream);
        const ArrowSchema* schema = input.read_schema();

        LanceAutoCleanupParams cleanup = {};
        LanceWriteOptions c_write = to_c_write_options(options.write, cleanup);

        std::vector<const char*> properties;
        if (options.transaction_properties) {
            properties.reserve(options.transaction_properties->size() * 2 + 1);
            for (const auto& [key, value] : *options.transaction_properties) {
                properties.push_back(key.c_str());
                properties.push_back(value.c_str());
            }
            properties.push_back(nullptr);
        }

        LanceUncommittedWriteOptions c_options = {};
        c_options.write = c_write;
        c_options.transaction_properties =
            options.transaction_properties ? properties.data() : nullptr;

        ArrowArrayStream* owned_stream = input.consume();
        LanceTransaction* transaction = nullptr;
        if (lance_dataset_write_uncommitted_stream_with_accessor(
                uri.c_str(),
                schema,
                owned_stream,
                static_cast<int32_t>(mode),
                &c_options,
                storage_options_accessor.c_handle(),
                session ? session->c_handle() : nullptr,
                &transaction) != 0) {
            check_error();
        }
        if (!transaction) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_dataset_write_uncommitted_stream_with_accessor "
                "returned success with null out_transaction");
        }
        return Transaction(transaction);
    }

    /// Stage an Arrow stream through a foreign commit lock and optional
    /// dynamic storage accessor.
    ///
    /// Staging uses the accessor and session exactly like `write_uncommitted`.
    /// The lock is retained for this call. Keep its handle alive and pass it
    /// to the matching commit method when publishing the returned transaction.
    /// NULL pointers keep native defaults. The stream is consumed on every
    /// return path.
    static Transaction write_uncommitted_with_commit_lock(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const UncommittedWriteOptions& options,
        const StorageOptionsAccessor* storage_accessor,
        const Session* session,
        const CommitLock* commit_lock) {

        ArrowStreamInput input(stream);
        const ArrowSchema* schema = input.read_schema();

        LanceAutoCleanupParams cleanup = {};
        std::vector<const char*> properties;
        LanceUncommittedWriteOptions c_options =
            to_c_uncommitted_write_options(options, cleanup, properties);

        ArrowArrayStream* owned_stream = input.consume();
        LanceTransaction* transaction = nullptr;
        if (lance_dataset_write_uncommitted_stream_with_commit_lock(
                uri.c_str(),
                schema,
                owned_stream,
                static_cast<int32_t>(mode),
                &c_options,
                storage_accessor ? storage_accessor->c_handle() : nullptr,
                session ? session->c_handle() : nullptr,
                commit_lock ? commit_lock->c_handle() : nullptr,
                &transaction) != 0) {
            check_error();
        }
        if (!transaction) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_dataset_write_uncommitted_stream_with_commit_lock "
                "returned success with null out_transaction");
        }
        return Transaction(transaction);
    }

    /// Publish one native transaction through the URI's native CommitBuilder.
    /// The transaction handle remains valid and is cloned by the C API.
    static Dataset commit(
        const std::string& uri,
        const Transaction& transaction,
        const CommitOptions& options = {},
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {},
        const Session* session = nullptr) {

        LanceCommitOptions c_options = to_c_commit_options(options);
        auto storage_kv = to_c_key_values(storage_opts);
        const char* const* storage_ptr =
            storage_opts.empty() ? nullptr : storage_kv.data();

        LanceDataset* dataset = nullptr;
        if (lance_transaction_commit(
                uri.c_str(),
                transaction.c_handle(),
                &c_options,
                storage_ptr,
                session ? session->c_handle() : nullptr,
                &dataset) != 0) {
            check_error();
        }
        if (!dataset) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_transaction_commit returned success with null "
                        "out_dataset");
        }
        return Dataset(dataset);
    }

    /// Publish one native transaction with a retained dynamic storage-options
    /// accessor. The returned dataset retains the accessor and provider.
    static Dataset commit_with_accessor(
        const std::string& uri,
        const Transaction& transaction,
        const StorageOptionsAccessor& storage_options_accessor,
        const CommitOptions& options = {},
        const Session* session = nullptr) {

        LanceCommitOptions c_options = to_c_commit_options(options);
        LanceDataset* dataset = nullptr;
        if (lance_transaction_commit_with_accessor(
                uri.c_str(),
                transaction.c_handle(),
                &c_options,
                storage_options_accessor.c_handle(),
                session ? session->c_handle() : nullptr,
                &dataset) != 0) {
            check_error();
        }
        if (!dataset) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_transaction_commit_with_accessor returned success with "
                "null out_dataset");
        }
        return Dataset(dataset);
    }

    /// Publish one native transaction through a foreign commit lock and
    /// optional dynamic storage accessor.
    ///
    /// Conflict handling, retries, manifest construction, cancellation and the
    /// final version stay native. NULL pointers keep native defaults. The
    /// transaction handle remains valid.
    static Dataset commit_with_commit_lock(
        const std::string& uri,
        const Transaction& transaction,
        const CommitOptions& options,
        const StorageOptionsAccessor* storage_accessor,
        const Session* session,
        const CommitLock* commit_lock) {

        LanceCommitOptions c_options = to_c_commit_options(options);
        LanceDataset* dataset = nullptr;
        if (lance_transaction_commit_with_commit_lock(
                uri.c_str(),
                transaction.c_handle(),
                &c_options,
                storage_accessor ? storage_accessor->c_handle() : nullptr,
                session ? session->c_handle() : nullptr,
                commit_lock ? commit_lock->c_handle() : nullptr,
                &dataset) != 0) {
            check_error();
        }
        if (!dataset) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_transaction_commit_with_commit_lock returned success "
                "with null out_dataset");
        }
        return Dataset(dataset);
    }

    /// Commit native append transactions as one version. Native eligibility and
    /// merge semantics apply; non-append batches throw LANCE_ERR_NOT_SUPPORTED.
    static BatchCommitResult commit_batch(
        const std::string& uri,
        const std::vector<std::reference_wrapper<const Transaction>>& transactions,
        const CommitOptions& options = {},
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {},
        const Session* session = nullptr);

    /// Commit append transactions as one version with a retained dynamic
    /// storage-options accessor. The returned dataset retains the accessor and
    /// provider; the merged transaction carries transaction metadata only.
    static BatchCommitResult commit_batch_with_accessor(
        const std::string& uri,
        const std::vector<std::reference_wrapper<const Transaction>>& transactions,
        const StorageOptionsAccessor& storage_options_accessor,
        const CommitOptions& options = {},
        const Session* session = nullptr);

    /// Commit native append transactions as one version through a foreign
    /// commit lock and optional dynamic storage accessor.
    static BatchCommitResult commit_batch_with_commit_lock(
        const std::string& uri,
        const std::vector<std::reference_wrapper<const Transaction>>& transactions,
        const CommitOptions& options,
        const StorageOptionsAccessor* storage_accessor,
        const Session* session,
        const CommitLock* commit_lock);

    /// Clone this handle as an independent in-memory snapshot.
    ///
    /// The returned handle shares this dataset's native snapshot and
    /// Arc-backed session, object-store registry, caches, and commit handler,
    /// but has its own lock. Cloning performs no storage I/O. Mutating either
    /// handle does not change the other's version, schema, or rows, and does
    /// not hold the other's lock. Source and clone may be destroyed in either
    /// order.
    ///
    /// This duplicates an in-memory handle; it is not Lance's on-disk shallow
    /// or deep clone operation. Throws lance::Error on failure.
    Dataset clone_handle() const {
        auto* ds = lance_dataset_clone_handle(handle_.get());
        if (!ds) check_error();
        return Dataset(ds);
    }

    /// Number of rows in the dataset.
    uint64_t count_rows() const {
        uint64_t n = lance_dataset_count_rows(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return n;
    }

    /// Number of rows matching `filter` through native
    /// `Dataset::count_rows(Some(filter))`. An empty string is an explicit
    /// filter, not the unfiltered metadata fast path; use count_rows() for
    /// native `None`.
    uint64_t count_rows(const std::string& filter) const {
        uint64_t n =
            lance_dataset_count_rows_filtered(handle_.get(), filter.c_str());
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

    /// Apply a batch of per-field metadata updates in one native commit.
    /// Duplicate references to the same resolved field follow native
    /// last-write-wins behavior. Read the committed field metadata through
    /// `schema()`. Throws lance::Error on failure.
    void update_field_metadata(const std::vector<FieldMetadataUpdate>& updates) {
        run_field_metadata_update(handle_.get(), updates);
    }

    /// Current snapshot's canonical native Lance data-storage version string.
    /// Throws lance::Error on failure.
    std::string data_storage_version() const {
        const char* version =
            lance_dataset_data_storage_version(handle_.get());
        if (!version) check_error();
        std::string out(version);
        lance_free_string(version);
        return out;
    }

    /// Whether the current snapshot uses stable row IDs.
    /// Throws lance::Error on failure.
    bool has_stable_row_ids() const {
        bool stable = false;
        if (lance_dataset_has_stable_row_ids(handle_.get(), &stable) != 0) {
            check_error();
        }
        return stable;
    }

    /// Field IDs in the current schema's native pre-order traversal.
    /// Throws lance::Error on failure.
    std::vector<int32_t> field_ids() const {
        size_t count = 0;
        if (lance_dataset_field_id_count(handle_.get(), &count) != 0) {
            check_error();
        }
        std::vector<int32_t> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            int32_t field_id = 0;
            if (lance_dataset_field_id_at(handle_.get(), i, &field_id) != 0) {
                check_error();
            }
            out.push_back(field_id);
        }
        return out;
    }

    /// Resolve a field reference against the current snapshot's schema.
    /// Throws lance::Error on an unknown ID or path.
    int32_t resolve_field_id(const FieldReference& field) const {
        int32_t field_id = 0;
        LanceFieldReference raw = field.c_handle();
        if (lance_dataset_resolve_field_id(handle_.get(), &raw, &field_id) != 0) {
            check_error();
        }
        return field_id;
    }

    /// Native minimal Lance field path for `field_id`, including nested fields.
    /// Throws lance::Error on failure.
    std::string field_path(int32_t field_id) const {
        const char* path = lance_dataset_field_path(handle_.get(), field_id);
        if (!path) check_error();
        std::string out(path);
        lance_free_string(path);
        return out;
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

    /// Snapshot the dataset's native object-store I/O counters without
    /// resetting them. The counters are the current native accumulator for the
    /// store returned by `Dataset::object_store(None)`; a preceding incremental
    /// call resets that accumulator.
    LanceObjectStoreIoStats io_stats_snapshot() const {
        LanceObjectStoreIoStats stats{};
        if (lance_dataset_io_stats_snapshot(handle_.get(), &stats) != 0)
            check_error();
        return stats;
    }

    /// Return dataset object-store counters accumulated since the previous
    /// incremental call and reset the native accumulator. No binding-side
    /// baseline is kept.
    LanceObjectStoreIoStats io_stats_incremental() const {
        LanceObjectStoreIoStats stats{};
        if (lance_dataset_io_stats_incremental(handle_.get(), &stats) != 0)
            check_error();
        return stats;
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

    /// Merge or replace config and return the complete updated map as JSON.
    ///
    /// `value == std::nullopt` removes the key; any string value (including an
    /// empty one) upserts it. `MetadataUpdateMode::Replace` with an empty
    /// vector clears the map. Duplicate keys follow the native builder's
    /// last-write-wins behavior. The handle points at the new version after a
    /// successful call; existing snapshots retain their prior view.
    /// Throws lance::Error on failure.
    std::string update_config_json(
        const std::vector<ConfigUpdate>& updates,
        MetadataUpdateMode mode = MetadataUpdateMode::Merge) {
        std::vector<const char*> keys;
        std::vector<const char*> values;
        keys.reserve(updates.size());
        values.reserve(updates.size());
        for (const auto& [key, value] : updates) {
            keys.push_back(key.c_str());
            values.push_back(value ? value->c_str() : nullptr);
        }
        const char* json = lance_dataset_update_config_json(
            handle_.get(),
            updates.empty() ? nullptr : keys.data(),
            updates.empty() ? nullptr : values.data(),
            updates.size(),
            static_cast<LanceMetadataUpdateMode>(mode));
        return take_owned_string(json);
    }

    /// Apply native merge-style config updates, committing a new manifest.
    /// Existing callers keep the original void-returning contract.
    void update_config(const std::vector<ConfigUpdate>& updates) {
        (void)update_config_json(updates, MetadataUpdateMode::Merge);
    }

    /// Snapshot native base paths as JSON keyed by decimal ID. Each value has
    /// `id`, `name`, `path` and `is_dataset_root`. Throws lance::Error on
    /// failure.
    std::string base_paths_json() const {
        return take_owned_string(lance_dataset_base_paths_json(handle_.get()));
    }

    /// Register native base paths in one add-bases transaction.
    ///
    /// `id == 0` requests native allocation. `name == std::nullopt` maps to
    /// native `None`; an engaged empty string is preserved. An empty `bases`
    /// vector follows the native transaction's commit behavior.
    /// Transaction properties distinguish native `None`
    /// (unset optional) from `Some(empty map)` (engaged empty vector). Native
    /// duplicate/conflict validation and ID allocation remain authoritative.
    /// Throws lance::Error on failure.
    void add_bases(
        const std::vector<BasePath>& bases,
        const AddBasesOptions& options = {}) {
        std::vector<LanceBasePath> raw;
        raw.reserve(bases.size());
        for (const auto& base : bases) {
            raw.push_back(LanceBasePath{
                base.id,
                base.path.c_str(),
                base.name ? base.name->c_str() : nullptr,
                base.is_dataset_root,
            });
        }

        std::vector<const char*> property_keys;
        std::vector<const char*> property_values;
        LanceTransactionProperties properties{};
        const LanceTransactionProperties* properties_ptr = nullptr;
        if (options.transaction_properties) {
            const auto& entries = *options.transaction_properties;
            property_keys.reserve(entries.size());
            property_values.reserve(entries.size());
            for (const auto& [key, value] : entries) {
                property_keys.push_back(key.c_str());
                property_values.push_back(value.c_str());
            }
            properties.keys = property_keys.data();
            properties.values = property_values.data();
            properties.count = entries.size();
            properties_ptr = &properties;
        }

        if (lance_dataset_add_bases(
                handle_.get(),
                raw.data(),
                raw.size(),
                properties_ptr) != 0) {
            check_error();
        }
    }

    /// Export the native tracked-files stream. The returned owner releases the
    /// stream exactly once. The optional progress callback's capture is owned
    /// until the native emitter releases it, which can follow stream cleanup.
    ArrowStream tracked_files_stream(
        std::optional<uint64_t> min_version = std::nullopt,
        TrackedFilesProgressCallback progress = {}) const {
        LanceTrackedFilesOptions options{};
        if (min_version) {
            options.has_min_version = 1;
            options.min_version = *min_version;
        }

        TrackedFilesProgressCapture* capture = nullptr;
        LanceTrackedFilesProgress raw_progress{};
        if (progress) {
            capture = new TrackedFilesProgressCapture{std::move(progress)};
            raw_progress.callback = tracked_files_progress_trampoline;
            raw_progress.release = tracked_files_progress_release;
            raw_progress.callback_ctx = capture;
            options.progress = &raw_progress;
        }

        ArrowArrayStream raw{};
        if (lance_dataset_tracked_files_stream(
                handle_.get(), &options, &raw) != 0) {
            check_error();
        }
        return ArrowStream(raw);
    }

    /// Export the native physical-file stream for the primary dataset root.
    /// The returned owner releases the stream exactly once. Throws
    /// lance::Error on failure.
    ArrowStream all_files_stream() const {
        ArrowArrayStream raw{};
        if (lance_dataset_all_files_stream(handle_.get(), &raw) != 0) {
            check_error();
        }
        return ArrowStream(raw);
    }

    /// Read this snapshot's native transaction. Returns nullopt when the
    /// manifest has no transaction file. The result is independent of this
    /// Dataset handle.
    std::optional<Transaction> read_transaction() const {
        LanceTransaction* transaction = nullptr;
        if (lance_dataset_read_transaction(
                handle_.get(), &transaction) != 0) {
            check_error();
        }
        if (!transaction) return std::nullopt;
        return Transaction(transaction);
    }

    /// Read a specific version's native transaction. Returns nullopt when that
    /// version has no transaction file. Missing versions follow native errors.
    std::optional<Transaction> read_transaction_by_version(
        uint64_t version) const {
        LanceTransaction* transaction = nullptr;
        if (lance_dataset_read_transaction_by_version(
                handle_.get(), version, &transaction) != 0) {
            check_error();
        }
        if (!transaction) return std::nullopt;
        return Transaction(transaction);
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

    /// Create a native branch from `reference`.
    ///
    /// `storage_opts == std::nullopt` uses the source dataset's native store
    /// params. An explicitly empty vector passes explicit empty object-store
    /// params. Native branch creation is a non-atomic shallow clone followed by
    /// a BranchContents write; see the C API documentation for zombie recovery.
    Dataset create_branch(
        const std::string& branch_name,
        const Ref& reference,
        std::optional<std::vector<std::pair<std::string, std::string>>> storage_opts =
            std::nullopt) const {
        std::vector<const char*> kv;
        const char* const* storage_opts_ptr = nullptr;
        if (storage_opts.has_value()) {
            kv = to_c_key_values(*storage_opts);
            storage_opts_ptr = kv.data();
        }
        auto* out = lance_dataset_branch_create(
            handle_.get(),
            branch_name.c_str(),
            reference.c_handle(),
            storage_opts_ptr);
        if (!out) check_error();
        return Dataset(out);
    }

    /// Check out the latest version of a branch as an independent dataset.
    /// Pass "main" for the default branch.
    Dataset checkout_branch(const std::string& branch_name) const {
        auto* out =
            lance_dataset_checkout_branch(handle_.get(), branch_name.c_str());
        if (!out) check_error();
        return Dataset(out);
    }

    /// JSON object keyed by branch name. Values are native BranchContents
    /// serde objects; map order is unspecified.
    std::string branches_json() const {
        const char* json = lance_dataset_branches_json(handle_.get());
        if (!json) check_error();
        std::unique_ptr<const char, void (*)(const char*)> guard(
            json, lance_free_string);
        return std::string(json);
    }

    /// JSON object for one native BranchContents value.
    std::string get_branch_json(const std::string& branch_name) const {
        const char* json =
            lance_dataset_branch_get_json(handle_.get(), branch_name.c_str());
        if (!json) check_error();
        std::unique_ptr<const char, void (*)(const char*)> guard(
            json, lance_free_string);
        return std::string(json);
    }

    /// JSON object for this dataset snapshot's native BranchIdentifier.
    std::string branch_identifier_json() const {
        const char* json =
            lance_dataset_branch_identifier_json(handle_.get());
        if (!json) check_error();
        std::unique_ptr<const char, void (*)(const char*)> guard(
            json, lance_free_string);
        return std::string(json);
    }

    /// Delete a branch. Child-branch and tag references surface as
    /// LANCE_ERR_COMMIT_CONFLICT.
    void delete_branch(const std::string& branch_name) const {
        if (lance_dataset_branch_delete(
                handle_.get(), branch_name.c_str()) != 0)
            check_error();
    }

    /// Delete a branch while bypassing only a missing BranchContents. Native
    /// child-branch and tag reference checks still apply.
    void force_delete_branch(const std::string& branch_name) const {
        if (lance_dataset_branch_force_delete(
                handle_.get(), branch_name.c_str()) != 0)
            check_error();
    }

    /// Replace all metadata entries on a branch. An empty vector writes an
    /// empty map; this is not a merge or NULL-delete operation.
    void replace_branch_metadata(
        const std::string& branch_name,
        const std::vector<std::pair<std::string, std::string>>& metadata) const {
        auto kv = to_c_key_values(metadata);
        const char* const* metadata_ptr = metadata.empty() ? nullptr : kv.data();
        if (lance_dataset_branch_replace_metadata(
                handle_.get(), branch_name.c_str(), metadata_ptr) != 0)
            check_error();
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

    /// Clean versions older than a relative duration through native
    /// `Dataset::cleanup_old_versions`. Lance core owns the timestamp cutoff.
    /// Throws lance::Error on failure.
    RemovalStats cleanup_old_versions(
        const CleanupOldVersionsOptions& options) const {
        LanceCleanupOldVersionsOptions raw{};
        raw.older_than_seconds = options.older_than_seconds;
        raw.older_than_nanos = options.older_than_nanos;
        if (options.delete_unverified) {
            raw.has_delete_unverified = 1;
            raw.delete_unverified = *options.delete_unverified;
        }
        if (options.error_if_tagged_old_versions) {
            raw.has_error_if_tagged_old_versions = 1;
            raw.error_if_tagged_old_versions =
                *options.error_if_tagged_old_versions;
        }
        LanceRemovalStats stats{};
        if (lance_dataset_cleanup_old_versions(
                handle_.get(), &raw, &stats) != 0) {
            check_error();
        }
        return from_c_removal_stats(stats);
    }

    /// Execute native manual cleanup with an explicit policy. Current versions,
    /// tags, referenced branches and unverified files follow Lance core.
    /// Throws lance::Error on failure.
    RemovalStats cleanup(
        const CleanupPolicy& policy,
        const CleanupOperationOptions& operation = {}) const {
        std::vector<uint64_t> versions;
        uint64_t empty_versions_sentinel = 0;
        const LanceCleanupPolicy raw_policy = to_c_cleanup_policy(
            policy, versions, empty_versions_sentinel);
        const LanceCleanupOperationOptions raw_operation =
            to_c_cleanup_operation(operation);
        LanceRemovalStats stats{};
        if (lance_dataset_cleanup(
                handle_.get(),
                &raw_policy,
                &raw_operation,
                &stats) != 0) {
            check_error();
        }
        return from_c_removal_stats(stats);
    }

    /// Explain cleanup without deleting files. The returned value owns copies
    /// of every native field and remains valid after this Dataset is closed.
    /// A later cleanup re-evaluates current references and storage state.
    /// Throws lance::Error on failure.
    CleanupExplanation explain_cleanup(
        const CleanupPolicy& policy,
        const CleanupOperationOptions& operation = {}) const {
        std::vector<uint64_t> versions;
        uint64_t empty_versions_sentinel = 0;
        const LanceCleanupPolicy raw_policy = to_c_cleanup_policy(
            policy, versions, empty_versions_sentinel);
        const LanceCleanupOperationOptions raw_operation =
            to_c_cleanup_operation(operation);
        auto* raw = lance_dataset_cleanup_explain(
            handle_.get(), &raw_policy, &raw_operation);
        if (!raw) check_error();
        Handle<LanceCleanupExplanation, lance_cleanup_explanation_close> snap(
            raw);
        return read_cleanup_explanation(snap.get());
    }

    /// Validate native fragments, stable row IDs and indices. Throws
    /// lance::Error on failure.
    void validate() const {
        if (lance_dataset_validate(handle_.get()) != 0) check_error();
    }

    /// Count rows hidden by deletion files across all fragments.
    /// Throws lance::Error on failure.
    uint64_t count_deleted_rows() const {
        uint64_t rows = 0;
        if (lance_dataset_count_deleted_rows(handle_.get(), &rows) != 0)
            check_error();
        return rows;
    }

    /// Count fragments with fewer physical rows than `max_rows_per_group`.
    /// Native `max_rows_per_group == 0` returns 0. Throws lance::Error only
    /// for binding input errors.
    uint64_t num_small_files(size_t max_rows_per_group) const {
        uint64_t count = 0;
        if (lance_dataset_num_small_files(
                handle_.get(), max_rows_per_group, &count) != 0) {
            check_error();
        }
        return count;
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

    /// One-shot native `Dataset::merge` left join. The two-argument overload
    /// maps an omitted `right_on` to `left_on`.
    ///
    /// A non-null `source` is consumed on return or throw; a null source is
    /// rejected without consumption. Native owns matching, schema union, and
    /// commit behavior. Existing scanners and independently opened handles
    /// retain their previous snapshots. Throws lance::Error on failure.
    void merge(ArrowArrayStream* source, const std::string& left_on) {
        merge(source, left_on, left_on);
    }

    /// One-shot native `Dataset::merge` left join with distinct keys.
    ///
    /// Empty key strings are forwarded to native validation. Throws
    /// lance::Error on failure.
    void merge(
        ArrowArrayStream* source,
        const std::string& left_on,
        const std::string& right_on) {
        if (lance_dataset_merge(
                handle_.get(), left_on.c_str(), right_on.c_str(), source) != 0) {
            check_error();
        }
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

    // ─── MemWAL / LSM ────────────────────────────────────────────────────────

    /// Initialize native MemWAL on this dataset. The handle is updated in
    /// place; existing scanners keep their previous snapshot. Sharding mode,
    /// column, bucket bounds, maintained index names, and writer defaults are
    /// validated by native code.
    void initialize_mem_wal(const MemWalInitOptions& options);

    /// Read native MemWAL index details. Returns nullopt when MemWAL is not
    /// initialized on the current snapshot.
    std::optional<MemWalIndexDetails> mem_wal_index_details() const;

    /// Open a native shard writer. The supplied shard ID must be a UUID;
    /// `config` overrides native defaults for this writer only.
    ShardWriter mem_wal_writer(const std::string& shard_id,
                               const ShardWriterConfig& config = {}) const;

    /// Merge `source` and atomically advance MemWAL compaction progress.
    /// `source` is consumed on return or throw. An empty list is a plain merge.
    LanceMergeInsertResult merge_insert_with_compaction(
        const std::vector<std::string>& on_columns,
        ArrowArrayStream* source,
        const LanceMergeInsertParams* params,
        const std::vector<CompactedSsTable>& compacted_sstables) {
        std::vector<const char*> col_ptrs;
        col_ptrs.reserve(on_columns.size());
        for (const auto& column : on_columns) col_ptrs.push_back(column.c_str());
        std::vector<::LanceCompactedSsTable> raw_compacted;
        raw_compacted.reserve(compacted_sstables.size());
        for (const auto& entry : compacted_sstables) {
            raw_compacted.push_back(entry.to_c());
        }
        LanceMergeInsertResult result{};
        if (lance_dataset_merge_insert_with_compaction(
                handle_.get(),
                col_ptrs.empty() ? nullptr : col_ptrs.data(),
                col_ptrs.size(),
                source,
                params,
                raw_compacted.empty() ? nullptr : raw_compacted.data(),
                raw_compacted.size(),
                &result) != 0) {
            check_error();
        }
        return result;
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

    /// Compact using the complete native options surface. NULL selects
    /// `CompactionOptions::from_dataset_config`.
    LanceCompactionMetrics compact_files_with_options(
        const CompactionOptions* options = nullptr) {
        LanceCompactionMetrics metrics{};
        if (lance_dataset_compact_files_v2(
                handle_.get(),
                options ? options->c_handle() : nullptr,
                &metrics) != 0)
            check_error();
        return metrics;
    }

    /// Plan native compaction. NULL options select dataset config.
    CompactionPlan plan_compaction(
        const CompactionOptions* options = nullptr) const {
        auto* plan = lance_dataset_plan_compaction(
            handle_.get(), options ? options->c_handle() : nullptr);
        if (!plan) check_error();
        return CompactionPlan(plan);
    }

    /// Commit native rewrite results. NULL options select native defaults.
    LanceCompactionMetrics commit_compaction(
        const std::vector<RewriteResult>& results,
        const CompactionOptions* options = nullptr) {
        std::vector<const LanceRewriteResult*> raw;
        raw.reserve(results.size());
        for (const auto& result : results) raw.push_back(result.c_handle());
        LanceCompactionMetrics metrics{};
        if (lance_dataset_commit_compaction(
                handle_.get(),
                raw.empty() ? nullptr : raw.data(),
                raw.size(),
                options ? options->c_handle() : nullptr,
                &metrics) != 0)
            check_error();
        return metrics;
    }

    /// Run native single-process index optimization. NULL uses native defaults.
    void optimize_indices(
        const OptimizeIndicesOptions* options = nullptr) {
        if (lance_dataset_optimize_indices(
                handle_.get(),
                options ? options->c_handle() : nullptr) != 0)
            check_error();
    }

    /// Plan the native distributed index optimization workflow.
    OptimizeIndicesPlan plan_optimize_indices(
        const OptimizeIndicesOptions* options = nullptr) const {
        auto* plan = lance_dataset_plan_optimize_indices(
            handle_.get(), options ? options->c_handle() : nullptr);
        if (!plan) check_error();
        return OptimizeIndicesPlan(plan);
    }

    /// Commit merged native index segments.
    void commit_optimize_indices(
        const std::vector<IndexSegmentResult>& results,
        const OptimizeIndicesOptions* options = nullptr) {
        std::vector<const LanceIndexSegmentResult*> raw;
        raw.reserve(results.size());
        for (const auto& result : results) raw.push_back(result.c_handle());
        if (lance_dataset_commit_optimize_indices(
                handle_.get(),
                raw.empty() ? nullptr : raw.data(),
                raw.size(),
                options ? options->c_handle() : nullptr) != 0)
            check_error();
    }

    /// Migrate manifest paths to native v2 naming through core.
    void migrate_manifest_paths_v2() {
        if (lance_dataset_migrate_manifest_paths_v2(handle_.get()) != 0)
            check_error();
    }

    /// Delete every row through native `Dataset::truncate_table`.
    void truncate_table() {
        if (lance_dataset_truncate_table(handle_.get()) != 0) check_error();
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

    /// Add columns through native `NewColumnTransform::BatchUDF`.
    ///
    /// `udf.output_schema` is the complete schema of every returned batch and
    /// must remain valid for the call. An engaged checkpoint store requires all
    /// four callbacks. `std::nullopt` read columns maps to native `None`; an
    /// engaged empty vector maps to `Some(empty)`. `std::nullopt` batch size
    /// maps to `None`; `std::optional<uint32_t>(0)` is forwarded as explicit
    /// zero, leaving validation to native.
    ///
    /// Callbacks execute synchronously under this handle's exclusive mutation
    /// lock. They must not re-enter the same handle; use an independent handle
    /// or an existing fragment/scanner snapshot for callback reads. C++
    /// exceptions are contained and converted to native callback errors.
    /// Throws lance::Error on failure.
    void add_columns_batch_udf(
        const BatchUDF& udf,
        std::optional<std::vector<std::string>> read_columns = std::nullopt,
        std::optional<uint32_t> batch_size = std::nullopt) {
        if (!udf.mapper) {
            throw Error(
                LANCE_ERR_INVALID_ARGUMENT, "BatchUDF mapper must not be empty");
        }
        if (udf.output_schema == nullptr) {
            throw Error(
                LANCE_ERR_INVALID_ARGUMENT,
                "BatchUDF output_schema must not be null");
        }

        callback_detail::MapperContext mapper_context{&udf};
        callback_detail::CheckpointContext checkpoint_context{nullptr};
        LanceUDFCheckpointStore raw_store{};
        const UDFCheckpointStore* store = nullptr;
        if (udf.checkpoint_store) {
            store = &*udf.checkpoint_store;
            if (!store->get_batch || !store->insert_batch ||
                !store->get_fragment || !store->insert_fragment) {
                throw Error(
                    LANCE_ERR_INVALID_ARGUMENT,
                    "UDFCheckpointStore must provide all four callbacks");
            }
            checkpoint_context.store = store;
            raw_store.get_batch = &callback_detail::get_batch_trampoline;
            raw_store.insert_batch = &callback_detail::insert_batch_trampoline;
            raw_store.get_fragment = &callback_detail::get_fragment_trampoline;
            raw_store.insert_fragment = &callback_detail::insert_fragment_trampoline;
            raw_store.callback_ctx = &checkpoint_context;
        }

        LanceBatchUDF raw{};
        raw.mapper = &callback_detail::mapper_trampoline;
        raw.mapper_ctx = &mapper_context;
        raw.output_schema = udf.output_schema;
        raw.checkpoint_store = store == nullptr ? nullptr : &raw_store;

        std::vector<const char*> read_column_ptrs;
        if (read_columns) {
            read_column_ptrs.reserve(read_columns->size() + 1);
            for (const auto& column : *read_columns) {
                read_column_ptrs.push_back(column.c_str());
            }
            read_column_ptrs.push_back(nullptr);
        }
        const uint32_t batch_size_value = batch_size.value_or(0);
        const char* const* read_columns_ptr =
            read_columns ? read_column_ptrs.data() : nullptr;
        const uint32_t* batch_size_ptr = batch_size ? &batch_size_value : nullptr;

        if (lance_dataset_add_columns_batch_udf(
                handle_.get(), &raw, read_columns_ptr, batch_size_ptr) != 0) {
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

    /// Create native data-file metadata from an existing Lance file.
    ///
    /// `path` is relative to the selected native data directory.
    /// `base_id == std::nullopt` maps to native `None`; an engaged value maps
    /// to native `Some`, including zero. Empty paths, missing or malformed
    /// files, schema mismatches and unregistered base IDs retain native error
    /// timing. The returned snapshot remains valid after this dataset is
    /// destroyed.
    DataFile create_data_file(
        const std::string& path,
        std::optional<uint32_t> base_id = std::nullopt) const {
        LanceDataFile* file = nullptr;
        const uint32_t* base_id_ptr = base_id ? &*base_id : nullptr;
        if (lance_dataset_create_data_file(
                handle_.get(), path.c_str(), base_id_ptr, &file) != 0) {
            check_error();
        }
        return DataFile(file);
    }

    /// Clone the current snapshot schema as an owned native value.
    LanceSchema native_schema() const;

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

    /// Take blob handles by physical row address; element `i` is for
    /// `row_addresses[i]`, `std::nullopt` for a null blob value. An empty
    /// selection still validates `column` through native.
    std::vector<std::optional<BlobFile>> take_blobs_by_addresses(
        const uint64_t* row_addresses, size_t num_row_addresses,
        const std::string& column) const {
        std::vector<LanceBlobFile*> raw(num_row_addresses, nullptr);
        std::vector<std::optional<BlobFile>> blobs;
        blobs.reserve(num_row_addresses);
        // Keep the native call for an empty selection so column validation
        // still runs; an empty vector's data() may be null.
        LanceBlobFile* empty_out = nullptr;
        LanceBlobFile** out =
            num_row_addresses == 0 ? &empty_out : raw.data();
        if (lance_dataset_take_blobs_by_addresses(
                handle_.get(), row_addresses, num_row_addresses,
                column.c_str(), out) != 0) {
            check_error();
        }
        adopt_blobs(raw, blobs);
        return blobs;
    }

    /// Plan and stream whole-blob reads. Returns the existing move-only
    /// ArrowStream. Each stream record contains `row_address` and nullable
    /// LargeBinary `data`; native order, duplicates and NULL-versus-empty
    /// values are preserved. The stream remains valid after this Dataset is
    /// destroyed. Native payload bytes are retained directly in Arrow buffers
    /// without a payload copy; exported batches remain valid after the stream
    /// is released.
    ArrowStream read_blobs(
        const std::string& column,
        BlobRowSelector selector,
        const std::vector<uint64_t>& rows,
        const BlobReadOptions& options = {}) const {
        LanceBlobReadOptions raw{};
        if (options.io_buffer_size) {
            raw.has_io_buffer_size = true;
            raw.io_buffer_size = *options.io_buffer_size;
        }
        raw.order = options.preserve_order
            ? LANCE_BLOB_READ_PRESERVE_ORDER
            : LANCE_BLOB_READ_UNORDERED;
        const uint64_t* rows_ptr =
            selector == BlobRowSelector::None ? nullptr : rows.data();
        ArrowArrayStream stream{};
        if (lance_dataset_read_blobs(
                handle_.get(), column.c_str(), static_cast<int32_t>(selector),
                rows_ptr, rows.size(), &raw, &stream) != 0) {
            check_error();
        }
        return ArrowStream(stream);
    }

    /// Plan and stream one blob-local range read per request. Returns the
    /// existing move-only ArrowStream. Each record contains `request_index`,
    /// `row_address`, `offset`, `length` and nullable LargeBinary `data`.
    /// `request_index` identifies duplicate/unordered requests. The stream
    /// remains valid after this Dataset is destroyed. Native payload bytes are
    /// retained directly in Arrow buffers without a payload copy; exported
    /// batches remain valid after the stream is released.
    ArrowStream read_blob_ranges(
        const std::string& column,
        BlobRowSelector selector,
        const std::vector<BlobRangeRequest>& requests,
        const BlobReadOptions& options = {}) const {
        std::vector<LanceBlobRangeRequest> raw_requests;
        raw_requests.reserve(requests.size());
        for (const auto& request : requests) {
            raw_requests.push_back(
                {request.row, request.offset, request.length});
        }
        LanceBlobReadOptions raw{};
        if (options.io_buffer_size) {
            raw.has_io_buffer_size = true;
            raw.io_buffer_size = *options.io_buffer_size;
        }
        raw.order = options.preserve_order
            ? LANCE_BLOB_READ_PRESERVE_ORDER
            : LANCE_BLOB_READ_UNORDERED;
        const LanceBlobRangeRequest* requests_ptr =
            selector == BlobRowSelector::None ? nullptr : raw_requests.data();
        ArrowArrayStream stream{};
        if (lance_dataset_read_blob_ranges(
                handle_.get(), column.c_str(), static_cast<int32_t>(selector),
                requests_ptr, raw_requests.size(), &raw, &stream) != 0) {
            check_error();
        }
        return ArrowStream(stream);
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

    /// Plan a SQL query over this dataset with the native `dataset::sql`
    /// planner. `sql` is copied; the returned builder owns its own dataset
    /// snapshot, so this dataset handle may close first. System-column
    /// injection, batch limits and blob handling are configured on the
    /// builder; the native planner owns their semantics.
    SqlQueryBuilder sql(const std::string& sql) const;

    /// Explore row changes between dataset versions with the native
    /// `dataset::delta` support. The returned builder owns its own dataset
    /// snapshot. Version and date ranges are validated synchronously on
    /// build; transaction listing and the four row-change streams are
    /// native.
    DatasetDeltaBuilder delta() const;

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

    /// Create a vector index from additive native options.
    ///
    /// Native Lance performs the commit before reloading index metadata. An
    /// exception can therefore be thrown after the dataset version was
    /// published; reopen the dataset to determine its current version.
    void create_vector_index_with_options(
        const std::string& column,
        const VectorIndexOptions& options,
        const std::string& name = "",
        bool replace = false);

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

    /// Create a scalar index with the additive native train control.
    void create_scalar_index_with_options(
        const std::string& column,
        LanceScalarIndexType index_type,
        const ScalarIndexOptions& options,
        const std::string& index_name = "",
        const std::string& params_json = "",
        bool replace = false) {
        const char* name_c = index_name.empty() ? nullptr : index_name.c_str();
        const char* json_c = params_json.empty() ? nullptr : params_json.c_str();
        LanceScalarIndexOptions scalar_options{};
        if (options.train) {
            scalar_options.has_train = true;
            scalar_options.train = *options.train;
        }
        if (lance_dataset_create_scalar_index_with_options(
                handle_.get(), column.c_str(), name_c, index_type, json_c,
                &scalar_options, replace) != 0)
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

    /// Create a scalar segment builder with the additive native train control.
    IndexSegmentBuilder new_scalar_index_segment_builder_with_options(
        const std::string& column,
        LanceScalarIndexType index_type,
        const ScalarIndexOptions& options,
        const std::string& index_name = "",
        const std::string& params_json = "",
        const LanceIndexSegmentBuildOptions* build_options = nullptr) const;

    /// Create a single-use builder for an uncommitted vector index segment.
    IndexSegmentBuilder new_vector_index_segment_builder(
        const std::string& column,
        const LanceVectorIndexParams& params,
        const std::string& index_name = "",
        const LanceIndexSegmentBuildOptions* options = nullptr) const;

    /// Create a single-use vector segment builder from additive native options.
    /// `fragment_ids == std::nullopt` preserves native None; an engaged empty
    /// vector preserves Some(empty). Native validation decides duplicate and
    /// missing-ID behavior and timing.
    IndexSegmentBuilder new_vector_index_segment_builder_with_options(
        const std::string& column,
        const VectorIndexOptions& options,
        const std::string& index_name = "",
        std::optional<std::vector<uint32_t>> fragment_ids = std::nullopt,
        const std::array<uint8_t, 16>* index_uuid = nullptr) const;

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

    /// Train IVF centroids with native sample-rate and iteration controls.
    IndexModel train_ivf_model_with_options(
        const std::string& column,
        uint32_t num_partitions,
        LanceMetricType metric,
        const IndexTrainerOptions& options = {},
        const std::vector<uint32_t>& fragment_ids = {}) const;

    /// Train a PQ codebook with native trainer controls.
    IndexModel train_pq_model_with_options(
        const std::string& column,
        uint32_t num_sub_vectors,
        uint32_t num_bits,
        LanceMetricType metric,
        IndexModel& ivf_centroids,
        const IndexTrainerOptions& options = {},
        const std::vector<uint32_t>& fragment_ids = {}) const;

    /// JSON array of every native index description, including system indexes.
    std::string describe_indices_json() const;

    /// Native index-family-specific statistics JSON. May publish a native
    /// migration version for old corrupt fragment coverage.
    std::string index_statistics_json(const std::string& index_name);

    /// Owned IVF model snapshot for the first physical vector segment.
    IvfModel get_ivf_model(const std::string& index_name) const;

    /// Centroids from get_ivf_model(index_name), or nullopt when absent.
    std::optional<IndexModel> centroids(const std::string& index_name) const;

    /// Read one logical IVF partition; the stream owns its native snapshot.
    ArrowStream read_index_partition(
        const std::string& index_name,
        uint64_t partition_id,
        bool with_vector = false) const;

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

    /// Prewarm selected segments with native FTS position controls.
    void prewarm_index_with_options(
        const std::string& index_name,
        const IndexPrewarmOptions& options = {}) const {
        LanceIndexPrewarmOptions prewarm{};
        prewarm.with_position = options.with_position;
        if (options.index_segments) {
            prewarm.has_segment_selection = true;
            prewarm.segment_count = options.index_segments->size();
            if (!options.index_segments->empty()) {
                prewarm.segment_uuids = reinterpret_cast<const uint8_t*>(
                    options.index_segments->data());
            }
        }
        if (lance_dataset_prewarm_index_with_options(
                handle_.get(), index_name.c_str(), &prewarm) != 0)
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

    /// Merge distributed Inverted index files. Files only; does not commit.
    void merge_index_metadata(
        const std::array<uint8_t, 16>& index_uuid,
        const std::string& index_type,
        std::optional<size_t> batch_readhead = std::nullopt,
        LanceIndexBuildProgressCallback callback = nullptr,
        void* callback_ctx = nullptr) const {
        if (lance_dataset_merge_index_metadata(
                handle_.get(), index_uuid.data(), index_type.c_str(),
                batch_readhead.has_value(), batch_readhead.value_or(0), callback,
                callback_ctx) != 0)
            check_error();
    }

    /// Merge existing uncommitted segments into one uncommitted segment.
    std::vector<uint8_t> merge_index_segments(
        const std::vector<std::vector<uint8_t>>& segment_metadata) const {
        std::vector<const uint8_t*> bytes;
        std::vector<size_t> lens;
        bytes.reserve(segment_metadata.size());
        lens.reserve(segment_metadata.size());
        for (const auto& metadata : segment_metadata) {
            bytes.push_back(metadata.data());
            lens.push_back(metadata.size());
        }
        uint8_t* output = nullptr;
        size_t output_len = 0;
        struct BytesGuard {
            uint8_t* bytes = nullptr;
            ~BytesGuard() { lance_free_bytes(bytes); }
        } guard;
        if (lance_dataset_merge_index_segments(
                handle_.get(), bytes.empty() ? nullptr : bytes.data(),
                lens.empty() ? nullptr : lens.data(), bytes.size(), &output,
                &output_len) != 0)
            check_error();
        guard.bytes = output;
        if (!output || output_len == 0) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_dataset_merge_index_segments returned empty metadata");
        }
        return std::vector<uint8_t>(output, output + output_len);
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

// ─── Dataset SQL ─────────────────────────────────────────────────────────────

/// Planned native SQL query result. Created by SqlQueryBuilder::build();
/// `into_stream` moves the query's native stream into a caller-owned
/// ArrowStream and consumes this object, so it must not be used afterwards.
class SqlQuery {
    Handle<LanceSqlQuery, lance_sql_query_close> handle_;

public:
    explicit SqlQuery(LanceSqlQuery* query) : handle_(query) {}

    SqlQuery(SqlQuery&&) noexcept = default;
    SqlQuery& operator=(SqlQuery&&) noexcept = default;
    SqlQuery(const SqlQuery&) = delete;
    SqlQuery& operator=(const SqlQuery&) = delete;

    /// Move the query's native stream into a returned `ArrowStream`,
    /// consuming this query: the C entry point frees the query allocation on
    /// every return path, including errors, so the handle is released before
    /// the call and never closed again. The returned stream is owned by the
    /// caller and released exactly once. On error this object is already
    /// consumed; do not reuse it.
    ArrowStream into_stream() {
        auto* raw = handle_.release();
        ArrowArrayStream stream{};
        if (lance_sql_query_into_stream(raw, &stream) != 0) {
            check_error();
        }
        return ArrowStream(stream);
    }
};

/// Builder mirroring the native `dataset::sql::SqlQueryBuilder`. All setters
/// return `*this`; a setter that fails (foreign conversion errors) preserves
/// the builder's prior state. `build` plans the query and consumes this
/// builder, so it must not be used afterwards.
class SqlQueryBuilder {
    Handle<LanceSqlQueryBuilder, lance_sql_query_builder_close> handle_;

public:
    explicit SqlQueryBuilder(LanceSqlQueryBuilder* builder) : handle_(builder) {}

    SqlQueryBuilder(SqlQueryBuilder&&) noexcept = default;
    SqlQueryBuilder& operator=(SqlQueryBuilder&&) noexcept = default;
    SqlQueryBuilder(const SqlQueryBuilder&) = delete;
    SqlQueryBuilder& operator=(const SqlQueryBuilder&) = delete;

    /// Name the table in the SELECT statement. The string is copied; an
    /// empty name is passed to native unchanged. The native default is
    /// "dataset".
    SqlQueryBuilder& table_name(const std::string& name) {
        if (lance_sql_query_builder_set_table_name(handle_.get(),
                                                    name.c_str()) != 0) {
            check_error();
        }
        return *this;
    }

    /// Ask the native planner to inject the `_rowid` system column where
    /// safe.
    SqlQueryBuilder& with_row_id(bool enabled = true) {
        if (lance_sql_query_builder_set_with_row_id(handle_.get(), enabled) !=
            0) {
            check_error();
        }
        return *this;
    }

    /// Ask the native planner to inject the `_rowaddr` system column
    /// where safe.
    SqlQueryBuilder& with_row_addr(bool enabled = true) {
        if (lance_sql_query_builder_set_with_row_addr(handle_.get(), enabled) !=
            0) {
            check_error();
        }
        return *this;
    }

    /// Set how blob columns are materialized (LanceBlobHandling).
    SqlQueryBuilder& blob_handling(LanceBlobHandling handling) {
        if (lance_sql_query_builder_set_blob_handling(handle_.get(),
                                                       handling) != 0) {
            check_error();
        }
        return *this;
    }

    /// Set the native target output batch size (rows). The native builder
    /// validates the value's range on build.
    SqlQueryBuilder& batch_size(uint64_t rows) {
        if (lance_sql_query_builder_set_batch_size(handle_.get(), rows) != 0) {
            check_error();
        }
        return *this;
    }

    /// Set the native target output batch size in bytes.
    SqlQueryBuilder& batch_size_bytes(uint64_t bytes) {
        if (lance_sql_query_builder_set_batch_size_bytes(handle_.get(),
                                                         bytes) != 0) {
            check_error();
        }
        return *this;
    }

    /// Plan the query and consume this builder: the C entry point frees the
    /// builder on every return path, including errors, so the handle is
    /// released before the call and never closed again.
    SqlQuery build() {
        auto* raw = handle_.release();
        LanceSqlQuery* query = nullptr;
        if (lance_sql_query_builder_build(raw, &query) != 0) {
            check_error();
        }
        return SqlQuery(query);
    }
};

// ─── Dataset delta ───────────────────────────────────────────────────────────

/// Native version-diff handle. Reusable: `list_transactions` and every
/// `get_*` stream may be called repeatedly and in any order; each returned
/// stream owns its state after this (and the dataset) handle closes.
class DatasetDelta {
    Handle<LanceDatasetDelta, lance_dataset_delta_close> handle_;

public:
    explicit DatasetDelta(LanceDatasetDelta* delta) : handle_(delta) {}

    DatasetDelta(DatasetDelta&&) noexcept = default;
    DatasetDelta& operator=(DatasetDelta&&) noexcept = default;
    DatasetDelta(const DatasetDelta&) = delete;
    DatasetDelta& operator=(const DatasetDelta&) = delete;

    /// List the transactions committed between the delta's begin and end
    /// versions. Each element is an independently owned Transaction. Every
    /// element is adopted before the shared array guard releases the
    /// untouched remainder, so a mid-loop exception frees the rest exactly
    /// once.
    std::vector<Transaction> list_transactions() const {
        LanceTransaction** transactions = nullptr;
        size_t count = 0;
        if (lance_dataset_delta_list_transactions(handle_.get(), &transactions,
                                                   &count) != 0) {
            check_error();
        }
        auto free_transactions = [count](LanceTransaction** items) {
            lance_transaction_array_free(items, count);
        };
        std::unique_ptr<LanceTransaction*, decltype(free_transactions)>
            guard(transactions, free_transactions);
        std::vector<Transaction> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            LanceTransaction* item = transactions[i];
            transactions[i] = nullptr;
            out.push_back(Transaction(item));
        }
        return out;
    }

    /// Stream the rows inserted across the delta's range into a returned
    /// `ArrowStream`. The stream owns the native state it needs, so it stays
    /// valid after this delta (and its dataset) handle closes. On error
    /// nothing is returned and this delta remains usable.
    ArrowStream get_inserted_rows() const {
        ArrowArrayStream stream{};
        if (lance_dataset_delta_get_inserted_rows(handle_.get(), &stream) != 0) {
            check_error();
        }
        return ArrowStream(stream);
    }

    /// Stream the rows updated across the delta's range into a returned
    /// `ArrowStream`. The stream owns its native state and stays valid after
    /// this delta (and its dataset) handle closes. On error nothing is
    /// returned and this delta remains usable.
    ArrowStream get_updated_rows() const {
        ArrowArrayStream stream{};
        if (lance_dataset_delta_get_updated_rows(handle_.get(), &stream) != 0) {
            check_error();
        }
        return ArrowStream(stream);
    }

    /// Stream the rows upserted across the delta's range into a returned
    /// `ArrowStream`. The stream owns its native state and stays valid after
    /// this delta (and its dataset) handle closes. On error nothing is
    /// returned and this delta remains usable.
    ArrowStream get_upserted_rows() const {
        ArrowArrayStream stream{};
        if (lance_dataset_delta_get_upserted_rows(handle_.get(), &stream) != 0) {
            check_error();
        }
        return ArrowStream(stream);
    }

    /// Stream the deleted row ids across the delta's range into a returned
    /// `ArrowStream`. The single column is the native stable row-id; native
    /// resolution, version-zero behavior and error stages are authoritative.
    /// The stream stays valid after this delta (and its dataset) handle
    /// closes. On error nothing is returned and this delta remains usable.
    ArrowStream get_deleted_row_ids() const {
        ArrowArrayStream stream{};
        if (lance_dataset_delta_get_deleted_row_ids(handle_.get(), &stream) != 0) {
            check_error();
        }
        return ArrowStream(stream);
    }
};

/// Builder mirroring the native `dataset::delta::DatasetDeltaBuilder`. Range
/// combinations are validated synchronously on build. `build` consumes this
/// builder, so it must not be used afterwards.
class DatasetDeltaBuilder {
    Handle<LanceDatasetDeltaBuilder, lance_dataset_delta_builder_close> handle_;

public:
    explicit DatasetDeltaBuilder(LanceDatasetDeltaBuilder* builder)
        : handle_(builder) {}

    DatasetDeltaBuilder(DatasetDeltaBuilder&&) noexcept = default;
    DatasetDeltaBuilder& operator=(DatasetDeltaBuilder&&) noexcept = default;
    DatasetDeltaBuilder(const DatasetDeltaBuilder&) = delete;
    DatasetDeltaBuilder& operator=(const DatasetDeltaBuilder&) = delete;

    /// Compare against the named version. Native build rejects combining
    /// this with any explicit begin/end version or date.
    DatasetDeltaBuilder& compared_against_version(uint64_t version) {
        if (lance_dataset_delta_builder_set_compared_against_version(
                handle_.get(), version) != 0) {
            check_error();
        }
        return *this;
    }

    /// Set the explicit begin version. Native build requires both begin and
    /// end versions, or a compared-against version.
    DatasetDeltaBuilder& begin_version(uint64_t version) {
        if (lance_dataset_delta_builder_set_begin_version(handle_.get(),
                                                           version) != 0) {
            check_error();
        }
        return *this;
    }

    /// Set the explicit end version.
    DatasetDeltaBuilder& end_version(uint64_t version) {
        if (lance_dataset_delta_builder_set_end_version(handle_.get(),
                                                         version) != 0) {
            check_error();
        }
        return *this;
    }

    /// Set the begin date for date-mode ranges. Native build requires a
    /// begin date when an end date is provided. A timestamp the native
    /// chrono constructor cannot represent is rejected by this setter.
    DatasetDeltaBuilder& begin_timestamp(const LanceDeltaTimestamp& timestamp) {
        if (lance_dataset_delta_builder_set_begin_timestamp(handle_.get(),
                                                             &timestamp) != 0) {
            check_error();
        }
        return *this;
    }

    /// Set the end date for date-mode ranges.
    DatasetDeltaBuilder& end_timestamp(const LanceDeltaTimestamp& timestamp) {
        if (lance_dataset_delta_builder_set_end_timestamp(handle_.get(),
                                                           &timestamp) != 0) {
            check_error();
        }
        return *this;
    }

    /// Build the delta and consume this builder: the C entry point frees the
    /// builder on every return path, including errors, so the handle is
    /// released before the call and never closed again. Native build is
    /// synchronous and validates the configured range combination.
    DatasetDelta build() {
        auto* raw = handle_.release();
        LanceDatasetDelta* delta = nullptr;
        if (lance_dataset_delta_builder_build(raw, &delta) != 0) {
            check_error();
        }
        return DatasetDelta(delta);
    }
};

inline LanceSchema Dataset::native_schema() const {
    return LanceSchema::from_dataset(*this);
}

inline LanceSchema LanceSchema::from_dataset(const Dataset& dataset) {
    ::LanceSchema* raw = nullptr;
    if (lance_dataset_schema_clone(dataset.c_handle(), &raw) != 0) {
        check_error();
    }
    return LanceSchema(raw);
}

inline CompactionOptions CompactionOptions::from_dataset(
    const Dataset& dataset) {
    auto* options =
        lance_compaction_options_v2_from_dataset(dataset.c_handle());
    if (!options) check_error();
    return CompactionOptions(options);
}

inline RewriteResult CompactionTask::execute(const Dataset& dataset) const {
    auto* result =
        lance_compaction_task_execute(dataset.c_handle(), handle_.get());
    if (!result) check_error();
    return RewriteResult(result);
}

inline std::optional<IndexSegmentResult> OptimizeIndexTask::execute(
    const Dataset& dataset) const {
    LanceIndexSegmentResult* result = nullptr;
    if (lance_optimize_index_task_execute(
            dataset.c_handle(), handle_.get(), &result) != 0)
        check_error();
    if (!result) return std::nullopt;
    return IndexSegmentResult(result);
}

inline std::optional<IndexSegmentResult> OptimizeIndexMerge::execute(
    const Dataset& dataset,
    const std::vector<IndexSegmentResult>& task_results) const {
    std::vector<const LanceIndexSegmentResult*> raw;
    raw.reserve(task_results.size());
    for (const auto& result : task_results) raw.push_back(result.c_handle());
    LanceIndexSegmentResult* result = nullptr;
    if (lance_optimize_index_merge_execute(
            dataset.c_handle(),
            handle_.get(),
            raw.empty() ? nullptr : raw.data(),
            raw.size(),
            &result) != 0)
        check_error();
    if (!result) return std::nullopt;
    return IndexSegmentResult(result);
}

/// Result of Dataset::commit_batch. The dataset and the actual native merged
/// transaction are independently owned.
struct BatchCommitResult {
    Dataset dataset;
    Transaction transaction;

    BatchCommitResult(Dataset&& dataset, Transaction&& transaction)
        : dataset(std::move(dataset)),
          transaction(std::move(transaction)) {}

    BatchCommitResult(BatchCommitResult&&) noexcept = default;
    BatchCommitResult& operator=(BatchCommitResult&&) noexcept = default;
    BatchCommitResult(const BatchCommitResult&) = delete;
    BatchCommitResult& operator=(const BatchCommitResult&) = delete;
};

inline BatchCommitResult Dataset::commit_batch(
    const std::string& uri,
    const std::vector<std::reference_wrapper<const Transaction>>& transactions,
    const CommitOptions& options,
    const std::vector<std::pair<std::string, std::string>>& storage_opts,
    const Session* session) {

    std::vector<const LanceTransaction*> raw;
    raw.reserve(transactions.size());
    for (const auto& transaction : transactions) {
        raw.push_back(transaction.get().c_handle());
    }

    LanceCommitOptions c_options = to_c_commit_options(options);
    auto storage_kv = to_c_key_values(storage_opts);
    const char* const* storage_ptr =
        storage_opts.empty() ? nullptr : storage_kv.data();

    LanceDataset* dataset = nullptr;
    LanceTransaction* merged = nullptr;
    if (lance_transaction_commit_batch(
            uri.c_str(),
            raw.data(),
            raw.size(),
            &c_options,
            storage_ptr,
            session ? session->c_handle() : nullptr,
            &dataset,
            &merged) != 0) {
        check_error();
    }
    if (!dataset || !merged) {
        if (dataset) lance_dataset_close(dataset);
        if (merged) lance_transaction_close(merged);
        throw Error(
            LANCE_ERR_INTERNAL,
            "lance_transaction_commit_batch returned success with null output");
    }
    return BatchCommitResult(Dataset(dataset), Transaction(merged));
}

inline BatchCommitResult Dataset::commit_batch_with_accessor(
    const std::string& uri,
    const std::vector<std::reference_wrapper<const Transaction>>& transactions,
    const StorageOptionsAccessor& storage_options_accessor,
    const CommitOptions& options,
    const Session* session) {

    std::vector<const LanceTransaction*> raw;
    raw.reserve(transactions.size());
    for (const auto& transaction : transactions) {
        raw.push_back(transaction.get().c_handle());
    }

    LanceCommitOptions c_options = to_c_commit_options(options);
    LanceDataset* dataset = nullptr;
    LanceTransaction* merged = nullptr;
    if (lance_transaction_commit_batch_with_accessor(
            uri.c_str(),
            raw.data(),
            raw.size(),
            &c_options,
            storage_options_accessor.c_handle(),
            session ? session->c_handle() : nullptr,
            &dataset,
            &merged) != 0) {
        check_error();
    }
    if (!dataset || !merged) {
        if (dataset) lance_dataset_close(dataset);
        if (merged) lance_transaction_close(merged);
        throw Error(
            LANCE_ERR_INTERNAL,
            "lance_transaction_commit_batch_with_accessor returned success "
            "with null output");
    }
    return BatchCommitResult(Dataset(dataset), Transaction(merged));
}

inline BatchCommitResult Dataset::commit_batch_with_commit_lock(
    const std::string& uri,
    const std::vector<std::reference_wrapper<const Transaction>>& transactions,
    const CommitOptions& options,
    const StorageOptionsAccessor* storage_accessor,
    const Session* session,
    const CommitLock* commit_lock) {

    std::vector<const LanceTransaction*> raw;
    raw.reserve(transactions.size());
    for (const auto& transaction : transactions) {
        raw.push_back(transaction.get().c_handle());
    }

    LanceCommitOptions c_options = to_c_commit_options(options);
    LanceDataset* dataset = nullptr;
    LanceTransaction* merged = nullptr;
    if (lance_transaction_commit_batch_with_commit_lock(
            uri.c_str(),
            raw.data(),
            raw.size(),
            &c_options,
            storage_accessor ? storage_accessor->c_handle() : nullptr,
            session ? session->c_handle() : nullptr,
            commit_lock ? commit_lock->c_handle() : nullptr,
            &dataset,
            &merged) != 0) {
        check_error();
    }
    if (!dataset || !merged) {
        if (dataset) lance_dataset_close(dataset);
        if (merged) lance_transaction_close(merged);
        throw Error(
            LANCE_ERR_INTERNAL,
            "lance_transaction_commit_batch_with_commit_lock returned success "
            "with null output");
    }
    return BatchCommitResult(Dataset(dataset), Transaction(merged));
}

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
    friend class IvfModel;

    IndexModel() noexcept = default;

    IndexModel(ArrowArray array, ArrowSchema schema) noexcept
        : array_(array), schema_(schema) {}

public:
    /// Adopt caller-produced native Arrow model structs, transferring their
    /// release responsibility to this object. Both inputs are required and are
    /// cleared to an empty state only after a complete transfer. This allows
    /// arbitrary native model dtypes and shapes to be passed to
    /// VectorIndexOptions.
    static IndexModel adopt(ArrowArray* array, ArrowSchema* schema) {
        if (!array || !schema) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "IndexModel::adopt requires non-null ArrowArray and ArrowSchema");
        }
        if (!array->release || !schema->release) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "IndexModel::adopt requires initialized Arrow structures");
        }
        IndexModel model;
        model.array_ = *array;
        model.schema_ = *schema;
        *array = {};
        *schema = {};
        return model;
    }

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

/// Move-only owner of a cloned native IVF model.
class IvfModel {
    Handle<LanceIvfModel, lance_ivf_model_free> handle_;

    explicit IvfModel(LanceIvfModel* model) : handle_(model) {}
    friend class Dataset;

public:
    IvfModel(IvfModel&&) noexcept = default;
    IvfModel& operator=(IvfModel&&) noexcept = default;
    IvfModel(const IvfModel&) = delete;
    IvfModel& operator=(const IvfModel&) = delete;

    bool has_centroids() const {
        bool value = lance_ivf_model_has_centroids(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    /// Owned Arrow centroids, or nullopt when the model has none.
    std::optional<IndexModel> centroids() const {
        if (!has_centroids()) return std::nullopt;
        ArrowArray array = {};
        ArrowSchema schema = {};
        if (lance_ivf_model_export_centroids(handle_.get(), &array, &schema) != 0)
            check_error();
        return IndexModel(array, schema);
    }

    uint64_t num_partitions() const {
        uint64_t value = lance_ivf_model_num_partitions(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    uint64_t dimension() const {
        uint64_t value = lance_ivf_model_dimension(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    /// Native partition size. An absent partition returns 0.
    uint64_t partition_size(uint64_t partition_id) const {
        uint64_t value =
            lance_ivf_model_partition_size(handle_.get(), partition_id);
        if (lance_last_error_code() != LANCE_OK) check_error();
        return value;
    }

    const LanceIvfModel* c_handle() const { return handle_.get(); }
};

namespace detail {

/// Stable C views over one VectorIndexOptions value. The source options and
/// IndexModel objects must outlive this storage and the C call that uses it.
class VectorIndexOptionsStorage {
    LanceVectorIndexOptions options_{};
    std::vector<const char*> storage_keys_;
    std::vector<const char*> storage_values_;
    LanceStringMap storage_{};
    std::vector<const char*> runtime_keys_;
    std::vector<const char*> runtime_values_;
    LanceStringMap runtime_{};
    std::vector<const char*> shuffle_buffers_;
    LancePrecomputedShuffleBuffers shuffle_{};

    static void set_optional(LanceOptionalU64& out,
                             const std::optional<uint64_t>& value) {
        out = {};
        if (value) {
            out.value = *value;
            out.is_set = true;
        }
    }

    static void set_map(const std::map<std::string, std::string>& source,
                        std::vector<const char*>& keys,
                        std::vector<const char*>& values,
                        LanceStringMap& out) {
        for (const auto& [key, value] : source) {
            keys.push_back(key.c_str());
            values.push_back(value.c_str());
        }
        out.keys = keys.empty() ? nullptr : keys.data();
        out.values = values.empty() ? nullptr : values.data();
        out.count = keys.size();
    }

public:
    explicit VectorIndexOptionsStorage(const VectorIndexOptions& options) {
        options_.index_type = static_cast<int32_t>(options.index_type);
        options_.metric = static_cast<int32_t>(options.metric);
        set_optional(options_.num_partitions, options.num_partitions);
        set_optional(options_.target_partition_size, options.target_partition_size);
        set_optional(options_.max_iters, options.max_iters);
        set_optional(options_.sample_rate, options.sample_rate);
        set_optional(options_.streaming_sample_rate,
                     options.streaming_sample_rate);
        set_optional(options_.streaming_coreset_rate,
                     options.streaming_coreset_rate);
        options_.streaming_refine_passes = options.streaming_refine_passes;
        options_.precomputed_partitions_file =
            options.precomputed_partitions_file
                ? options.precomputed_partitions_file->c_str()
                : nullptr;
        if (options.precomputed_shuffle_buffers) {
            shuffle_.path = options.precomputed_shuffle_buffers->first.c_str();
            for (const auto& buffer : options.precomputed_shuffle_buffers->second) {
                shuffle_buffers_.push_back(buffer.c_str());
            }
            shuffle_.buffers =
                shuffle_buffers_.empty() ? nullptr : shuffle_buffers_.data();
            shuffle_.buffer_count = shuffle_buffers_.size();
            options_.precomputed_shuffle_buffers = &shuffle_;
        }
        set_optional(options_.shuffle_partition_batches,
                     options.shuffle_partition_batches);
        set_optional(options_.shuffle_partition_concurrency,
                     options.shuffle_partition_concurrency);
        if (options.storage_options) {
            set_map(*options.storage_options, storage_keys_, storage_values_,
                    storage_);
            options_.storage_options = &storage_;
        }
        set_optional(options_.num_sub_vectors, options.num_sub_vectors);
        set_optional(options_.num_bits, options.num_bits);
        if (options.ivf_centroids) {
            options_.ivf_centroids = options.ivf_centroids->array();
            options_.ivf_centroids_schema = options.ivf_centroids->schema();
        }
        if (options.pq_codebook) {
            options_.pq_codebook = options.pq_codebook->array();
            options_.pq_codebook_schema = options.pq_codebook->schema();
        }
        options_.rabitq_model_json = options.rabitq_model_json
                                         ? options.rabitq_model_json->c_str()
                                         : nullptr;
        set_optional(options_.hnsw_max_level, options.hnsw_max_level);
        set_optional(options_.hnsw_m, options.hnsw_m);
        set_optional(options_.hnsw_ef_construction,
                     options.hnsw_ef_construction);
        options_.index_file_version =
            static_cast<int32_t>(options.index_file_version);
        options_.skip_transpose = options.skip_transpose;
        set_map(options.runtime_hints, runtime_keys_, runtime_values_, runtime_);
        options_.runtime_hints = &runtime_;
        if (options.train) {
            options_.has_train = true;
            options_.train = *options.train;
        }
    }

    VectorIndexOptionsStorage(const VectorIndexOptionsStorage&) = delete;
    VectorIndexOptionsStorage& operator=(const VectorIndexOptionsStorage&) =
        delete;

    const LanceVectorIndexOptions* get() const { return &options_; }
};

}  // namespace detail

/// One file recorded in an index segment's native metadata.
struct IndexSegmentFile {
    std::string path;
    uint64_t size_bytes = 0;

    bool operator==(const IndexSegmentFile& other) const {
        return path == other.path && size_bytes == other.size_bytes;
    }

    bool operator!=(const IndexSegmentFile& other) const {
        return !(*this == other);
    }
};

/// Whole seconds plus nanoseconds since the Unix epoch.
struct IndexTimestamp {
    int64_t unix_seconds = 0;
    uint32_t nanos = 0;
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

    /// Clone the complete in-memory native metadata value.
    IndexSegmentMetadata clone() const {
        LanceIndexSegmentMetadata* raw =
            lance_index_segment_metadata_clone(handle_.get());
        if (!raw) check_error();
        return IndexSegmentMetadata(raw);
    }

    /// Clone with caller-supplied coverage. `std::nullopt` copies the source
    /// fragment-coverage option unchanged; an empty vector selects
    /// present-empty coverage. Every other native field is copied.
    IndexSegmentMetadata clone_with_fragment_ids(
        std::optional<std::vector<uint32_t>> fragment_ids) const {
        LanceIndexSegmentMetadata* raw = nullptr;
        if (fragment_ids.has_value()) {
            raw = lance_index_segment_metadata_clone_with_fragment_ids(
                handle_.get(), true, fragment_ids->data(),
                fragment_ids->size());
        } else {
            raw = lance_index_segment_metadata_clone_with_fragment_ids(
                handle_.get(), false, nullptr, 0);
        }
        if (!raw) check_error();
        return IndexSegmentMetadata(raw);
    }

    /// Encode the complete in-memory metadata as protobuf-encoded
    /// IndexMetadata bytes. The returned vector owns its storage.
    std::vector<uint8_t> encode() const {
        struct BytesGuard {
            uint8_t* bytes = nullptr;
            ~BytesGuard() noexcept { lance_free_bytes(bytes); }

            BytesGuard() = default;
            BytesGuard(const BytesGuard&) = delete;
            BytesGuard& operator=(const BytesGuard&) = delete;
        } guard;

        size_t len = 0;
        if (lance_index_segment_metadata_encode(handle_.get(), &guard.bytes,
                                                &len) != 0)
            check_error();
        if (!guard.bytes || len == 0) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_index_segment_metadata_encode returned empty metadata");
        }
        return std::vector<uint8_t>(guard.bytes, guard.bytes + len);
    }

    std::vector<int32_t> covering_field_ids() const {
        size_t count =
            lance_index_segment_metadata_covering_field_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();

        std::vector<int32_t> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            int32_t field_id = 0;
            if (lance_index_segment_metadata_covering_field_id_at(
                    handle_.get(), i, &field_id) != 0)
                check_error();
            out.push_back(field_id);
        }
        return out;
    }

    /// Native `index_details.value` bytes. Empty when the present Any value is
    /// empty; throws when `index_details` is absent.
    std::vector<uint8_t> index_details_value() const {
        size_t len = 0;
        const uint8_t* bytes =
            lance_index_segment_metadata_index_details_value(handle_.get(),
                                                              &len);
        if (!bytes) check_error();
        if (len == 0) return {};
        return std::vector<uint8_t>(bytes, bytes + len);
    }

    std::vector<IndexSegmentFile> files() const {
        size_t count = lance_index_segment_metadata_file_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();

        std::vector<IndexSegmentFile> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const char* path =
                lance_index_segment_metadata_file_path_at(handle_.get(), i);
            if (!path) check_error();
            uint64_t size = 0;
            if (lance_index_segment_metadata_file_size_at(
                    handle_.get(), i, &size) != 0)
                check_error();
            out.push_back(IndexSegmentFile{path, size});
        }
        return out;
    }

    std::optional<IndexTimestamp> created_at() const {
        LanceOptionalTimestamp timestamp{};
        if (lance_index_segment_metadata_created_at(handle_.get(),
                                                    &timestamp) != 0)
            check_error();
        if (!timestamp.is_set) return std::nullopt;
        return IndexTimestamp{timestamp.unix_seconds, timestamp.nanos};
    }

    std::optional<uint32_t> base_id() const {
        bool is_set = false;
        uint32_t base_id = 0;
        if (lance_index_segment_metadata_base_id(handle_.get(), &is_set,
                                                 &base_id) != 0)
            check_error();
        if (!is_set) return std::nullopt;
        return base_id;
    }

    const LanceIndexSegmentMetadata* c_handle() const { return handle_.get(); }
};

inline IndexSegmentMetadata IndexSegmentResult::segment() const {
    auto* metadata = lance_index_segment_result_segment(handle_.get());
    if (!metadata) check_error();
    return IndexSegmentMetadata(metadata);
}

inline IndexSegmentMetadata OptimizeIndexTask::reference() const {
    auto* metadata = lance_optimize_index_task_reference(handle_.get());
    if (!metadata) check_error();
    return IndexSegmentMetadata(metadata);
}

inline IndexSegmentMetadata OptimizeIndexTask::old_segment(
    size_t index) const {
    auto* metadata =
        lance_optimize_index_task_old_segment_at(handle_.get(), index);
    if (!metadata) check_error();
    return IndexSegmentMetadata(metadata);
}

inline IndexSegmentMetadata OptimizeIndexMerge::old_segment(
    size_t index) const {
    auto* metadata =
        lance_optimize_index_merge_old_segment_at(handle_.get(), index);
    if (!metadata) check_error();
    return IndexSegmentMetadata(metadata);
}

inline Operation Operation::make_create_index(
    const std::vector<const IndexSegmentMetadata*>& new_indices,
    const std::vector<const IndexSegmentMetadata*>& removed_indices) {
    std::vector<const LanceIndexSegmentMetadata*> new_raw;
    new_raw.reserve(new_indices.size());
    for (const auto* metadata : new_indices) {
        if (!metadata) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "new index metadata entries must not be null");
        }
        new_raw.push_back(metadata->c_handle());
    }
    std::vector<const LanceIndexSegmentMetadata*> removed_raw;
    removed_raw.reserve(removed_indices.size());
    for (const auto* metadata : removed_indices) {
        if (!metadata) {
            throw Error(LANCE_ERR_INVALID_ARGUMENT,
                        "removed index metadata entries must not be null");
        }
        removed_raw.push_back(metadata->c_handle());
    }

    LanceOperation* operation = lance_operation_create_index_new(
        new_raw.data(), new_raw.size(), removed_raw.data(), removed_raw.size());
    if (!operation) check_error();
    return Operation(operation);
}

inline std::vector<IndexSegmentMetadata> Operation::create_index_new() const {
    uint64_t count = 0;
    if (lance_operation_create_index_new_count(handle_.get(), &count) != 0)
        check_error();

    std::vector<IndexSegmentMetadata> out;
    out.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        LanceIndexSegmentMetadata* metadata = nullptr;
        if (lance_operation_create_index_new_at(
                handle_.get(), static_cast<size_t>(i), &metadata) != 0)
            check_error();
        if (!metadata) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_operation_create_index_new_at returned success "
                        "with null metadata");
        }
        out.emplace_back(IndexSegmentMetadata(metadata));
    }
    return out;
}

inline std::vector<IndexSegmentMetadata> Operation::create_index_removed() const {
    uint64_t count = 0;
    if (lance_operation_create_index_removed_count(handle_.get(), &count) != 0)
        check_error();

    std::vector<IndexSegmentMetadata> out;
    out.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        LanceIndexSegmentMetadata* metadata = nullptr;
        if (lance_operation_create_index_removed_at(
                handle_.get(), static_cast<size_t>(i), &metadata) != 0)
            check_error();
        if (!metadata) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_operation_create_index_removed_at returned "
                        "success with null metadata");
        }
        out.emplace_back(IndexSegmentMetadata(metadata));
    }
    return out;

}

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

inline IndexSegmentBuilder Dataset::new_scalar_index_segment_builder_with_options(
    const std::string& column,
    LanceScalarIndexType index_type,
    const ScalarIndexOptions& options,
    const std::string& index_name,
    const std::string& params_json,
    const LanceIndexSegmentBuildOptions* build_options) const {
    const char* index_name_c = index_name.empty() ? nullptr : index_name.c_str();
    const char* params_json_c = params_json.empty() ? nullptr : params_json.c_str();
    LanceScalarIndexOptions scalar_options{};
    if (options.train) {
        scalar_options.has_train = true;
        scalar_options.train = *options.train;
    }
    auto* builder = lance_index_segment_builder_new_scalar_with_options(
        handle_.get(), column.c_str(), index_name_c,
        static_cast<int32_t>(index_type), params_json_c, &scalar_options,
        build_options);
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

inline void Dataset::create_vector_index_with_options(
    const std::string& column,
    const VectorIndexOptions& options,
    const std::string& name,
    bool replace) {
    detail::VectorIndexOptionsStorage storage(options);
    const char* name_c = name.empty() ? nullptr : name.c_str();
    if (lance_dataset_create_vector_index_with_options(
            handle_.get(), column.c_str(), name_c, storage.get(), replace) != 0) {
        check_error();
    }
}

inline IndexSegmentBuilder Dataset::new_vector_index_segment_builder_with_options(
    const std::string& column,
    const VectorIndexOptions& options,
    const std::string& index_name,
    std::optional<std::vector<uint32_t>> fragment_ids,
    const std::array<uint8_t, 16>* index_uuid) const {
    detail::VectorIndexOptionsStorage storage(options);
    const char* index_name_c = index_name.empty() ? nullptr : index_name.c_str();
    static const uint32_t empty_fragment_id = 0;
    LanceVectorIndexSegmentScope scope{};
    if (fragment_ids) {
        scope.fragment_ids = fragment_ids->empty()
                                 ? &empty_fragment_id
                                 : fragment_ids->data();
        scope.fragment_count = fragment_ids->size();
    }
    if (index_uuid) {
        scope.index_uuid = index_uuid->data();
    }
    const LanceVectorIndexSegmentScope* scope_ptr =
        (fragment_ids || index_uuid) ? &scope : nullptr;
    auto* builder = lance_index_segment_builder_new_vector_with_options(
        handle_.get(), column.c_str(), index_name_c, storage.get(), scope_ptr);
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

inline IndexModel Dataset::train_ivf_model_with_options(
    const std::string& column,
    uint32_t num_partitions,
    LanceMetricType metric,
    const IndexTrainerOptions& options,
    const std::vector<uint32_t>& fragment_ids) const {
    ArrowArray array = {};
    ArrowSchema schema = {};
    LanceIndexTrainerOptions trainer{};
    if (options.sample_rate) {
        trainer.sample_rate.value = *options.sample_rate;
        trainer.sample_rate.is_set = true;
    }
    if (options.max_iters) {
        trainer.max_iters.value = *options.max_iters;
        trainer.max_iters.is_set = true;
    }
    const uint32_t* ids = fragment_ids.empty() ? nullptr : fragment_ids.data();
    if (lance_index_train_ivf_model_with_options(
            handle_.get(), column.c_str(), num_partitions,
            static_cast<int32_t>(metric), ids, fragment_ids.size(), &trainer,
            &array, &schema) != 0)
        check_error();
    return IndexModel(array, schema);
}

inline IndexModel Dataset::train_pq_model_with_options(
    const std::string& column,
    uint32_t num_sub_vectors,
    uint32_t num_bits,
    LanceMetricType metric,
    IndexModel& ivf_centroids,
    const IndexTrainerOptions& options,
    const std::vector<uint32_t>& fragment_ids) const {
    ArrowArray array = {};
    ArrowSchema schema = {};
    LanceIndexTrainerOptions trainer{};
    if (options.sample_rate) {
        trainer.sample_rate.value = *options.sample_rate;
        trainer.sample_rate.is_set = true;
    }
    if (options.max_iters) {
        trainer.max_iters.value = *options.max_iters;
        trainer.max_iters.is_set = true;
    }
    const uint32_t* ids = fragment_ids.empty() ? nullptr : fragment_ids.data();
    if (lance_index_train_pq_model_with_options(
            handle_.get(), column.c_str(), num_sub_vectors, num_bits,
            static_cast<int32_t>(metric), ids, fragment_ids.size(),
            ivf_centroids.array(), ivf_centroids.schema(), &trainer, &array,
            &schema) != 0)
        check_error();
    return IndexModel(array, schema);
}

inline std::string Dataset::describe_indices_json() const {
    const char* json = lance_dataset_describe_indices_json(handle_.get());
    if (!json) check_error();
    std::unique_ptr<const char, void (*)(const char*)> guard(json,
                                                             lance_free_string);
    return std::string(json);
}

inline std::string Dataset::index_statistics_json(
    const std::string& index_name) {
    const char* json = lance_dataset_index_statistics_json(
        handle_.get(), index_name.c_str());
    if (!json) check_error();
    std::unique_ptr<const char, void (*)(const char*)> guard(json,
                                                             lance_free_string);
    return std::string(json);
}

inline IvfModel Dataset::get_ivf_model(const std::string& index_name) const {
    auto* model =
        lance_dataset_get_ivf_model(handle_.get(), index_name.c_str());
    if (!model) check_error();
    return IvfModel(model);
}

inline std::optional<IndexModel> Dataset::centroids(
    const std::string& index_name) const {
    return get_ivf_model(index_name).centroids();
}

inline ArrowStream Dataset::read_index_partition(
    const std::string& index_name,
    uint64_t partition_id,
    bool with_vector) const {
    ArrowArrayStream stream = {};
    if (lance_dataset_read_index_partition(
            handle_.get(), index_name.c_str(), partition_id, with_vector,
            &stream) != 0)
        check_error();
    if (!stream.release) {
        throw Error(LANCE_ERR_INTERNAL,
                    "lance_dataset_read_index_partition returned an empty stream");
    }
    return ArrowStream(stream);
}

// ─── Scanner ─────────────────────────────────────────────────────────────────

/// One native scanner ordering key. Entries are applied in vector order.
struct ColumnOrdering {
    std::string column_name;
    LanceOrderDirection direction = LANCE_ORDER_ASCENDING;
    LanceNullOrder null_order = LANCE_ORDER_NULLS_LAST;
};

/// One ordered scanner projection entry. `output_name` may be an alias.
struct ProjectionColumn {
    std::string output_name;
    std::string expression;
};

class Scanner {
    Handle<LanceScanner, lance_scanner_close> handle_;

    static std::string take_owned_string(const char* raw) {
        if (!raw) {
            if (lance_last_error_code() == LANCE_OK) {
                throw Error(LANCE_ERR_INTERNAL,
                            "scanner plan API returned NULL without an error");
            }
            check_error();
        }
        std::unique_ptr<const char, void (*)(const char*)> guard(
            raw, lance_free_string);
        return std::string(raw);
    }

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

    /// Clear the projection and scan all dataset columns (native unset).
    Scanner& project(std::nullopt_t) {
        if (lance_scanner_set_projection(handle_.get(), nullptr, 0) != 0)
            check_error();
        return *this;
    }

    /// Replace the projection with ordered output-name / SQL-expression pairs.
    /// An empty vector is an explicit empty projection; use std::nullopt to
    /// restore the unset projection. Strings are copied during this call.
    Scanner& project(const std::vector<ProjectionColumn>& columns) {
        std::vector<LanceProjectionColumn> native;
        native.reserve(columns.size());
        for (const auto& column : columns) {
            native.push_back({
                column.output_name.c_str(),
                column.expression.c_str(),
            });
        }
        LanceProjectionColumn explicit_empty = {};
        const LanceProjectionColumn* data =
            native.empty() ? &explicit_empty : native.data();
        if (lance_scanner_set_projection(handle_.get(), data, native.size()) != 0)
            check_error();
        return *this;
    }

    /// Stop adding `_score` / `_distance` to explicit search projections.
    Scanner& disable_scoring_autoprojection() {
        if (lance_scanner_disable_scoring_autoprojection(handle_.get()) != 0)
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
    /// Requires live rows only: vector/MinHash/FTS queries and
    /// include_deleted_rows(true) are rejected at stream creation.
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

    /// Set a Substrait aggregate Plan (serialized bytes).
    /// The plan is validated immediately; a failed replacement keeps the
    /// previous aggregate. Count and analyze-count replace it with COUNT(*).
    Scanner& substrait_aggregate(const uint8_t* bytes, size_t len) {
        if (lance_scanner_set_substrait_aggregate(handle_.get(), bytes, len) != 0)
            check_error();
        return *this;
    }

    /// Set a Substrait aggregate Plan (vector overload).
    Scanner& substrait_aggregate(const std::vector<uint8_t>& bytes) {
        return substrait_aggregate(bytes.data(), bytes.size());
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

    /// Export the configured output schema from its actual native plan.
    /// Planning-only; does not start iteration or advance an existing stream.
    /// `out` is caller-owned; release it exactly once on success.
    void schema(ArrowSchema* out) const {
        if (lance_scanner_schema(handle_.get(), out) != 0)
            check_error();
    }

    /// Explain the configured native plan without executing it.
    std::string explain_plan(bool verbose = false) const {
        return take_owned_string(
            lance_scanner_explain_plan(handle_.get(), verbose));
    }

    /// Execute and format the configured native plan with runtime metrics.
    /// Does not consume or advance an existing stream.
    std::string analyze_plan() const {
        return take_owned_string(lance_scanner_analyze_plan(handle_.get()));
    }

    /// Execute the native COUNT(*) plan for this scanner. Native aggregate
    /// planning rejects configured limit/offset/ordering. Throws lance::Error
    /// on failure.
    uint64_t count_rows() const {
        uint64_t count = lance_scanner_count_rows(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        return count;
    }

    /// Execute and format the native COUNT(*) plan, not the ordinary plan.
    std::string analyze_count_plan() const {
        return take_owned_string(
            lance_scanner_analyze_count_plan(handle_.get()));
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
    /// Set native continuous IVF search effort. Any explicit value, including
    /// 0.5, ignores fixed nprobes while explicit minimum/maximum remain.
    Scanner& search_effort(double effort) {
        if (lance_scanner_set_search_effort(handle_.get(), effort) != 0) check_error();
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
    /// Use native indexed-only fast search for vector, scalar-filtered,
    /// full-text and MinHash queries. Index-backed paths can omit unindexed
    /// data. Ordinary scans and non-indexable expressions follow native
    /// planning. Disabled by default. See lance.h.
    Scanner& fast_search(bool enable = true) {
        if (lance_scanner_set_fast_search(handle_.get(), enable) != 0) check_error();
        return *this;
    }
    /// Apply scanner filters before vector/MinHash ranking when enabled, or
    /// refine ranked candidates afterward when disabled; postfiltering may
    /// return fewer than the configured limit. This does not change the fragment
    /// search domain.
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

    /// Native MinHash LSH text-similarity search. `column` must have a MinHash
    /// LSH scalar index and the scanner must have a limit. Results are ordered
    /// by native `_distance` (1 - estimated Jaccard similarity).
    Scanner& minhash_search(const std::string& column, const std::string& text) {
        if (lance_scanner_set_minhash_query(handle_.get(), column.c_str(),
                                            text.c_str()) != 0)
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

inline SqlQueryBuilder Dataset::sql(const std::string& sql) const {
    auto* builder = lance_dataset_sql_builder_new(c_handle(), sql.c_str());
    if (!builder) check_error();
    return SqlQueryBuilder(builder);
}

inline DatasetDeltaBuilder Dataset::delta() const {
    auto* builder = lance_dataset_delta_builder_new(c_handle());
    if (!builder) check_error();
    return DatasetDeltaBuilder(builder);
}

// ─── Batch ───────────────────────────────────────────────────────────────────

class Batch {
    Handle<LanceBatch, lance_batch_free> handle_;

public:
    explicit Batch(LanceBatch* b) : handle_(b) {}

    /// Adopt a non-NULL Arrow array. `schema` is borrowed and must outlive the
    /// call. The C API consumes the array on every return; on failure the
    /// source array is already released and this method throws.
    static Batch from_arrow(ArrowArray* array, const ArrowSchema* schema) {
        LanceBatch* raw = nullptr;
        if (lance_batch_from_arrow(array, schema, &raw) != 0) {
            check_error();
        }
        return Batch(raw);
    }

    /// Export the complete record-batch schema and top-level struct array.
    void to_arrow(ArrowArray* out_array, ArrowSchema* out_schema) const {
        if (lance_batch_to_arrow(handle_.get(), out_array, out_schema) != 0)
            check_error();
    }

    const LanceBatch* c_handle() const { return handle_.get(); }
};

/// Move-only owner of a staged fragment overlay writer.
///
/// A live writer is closed without finishing on destruction. `finish()` and
/// `abort()` release the C allocation before invoking consuming native calls.
class FragmentOverlayWriter {
    Handle<LanceFragmentOverlayWriter, lance_fragment_overlay_writer_close>
        handle_;

public:
    explicit FragmentOverlayWriter(LanceFragmentOverlayWriter* writer)
        : handle_(writer) {}

    FragmentOverlayWriter(FragmentOverlayWriter&&) noexcept = default;
    FragmentOverlayWriter& operator=(FragmentOverlayWriter&&) noexcept =
        default;
    FragmentOverlayWriter(const FragmentOverlayWriter&) = delete;
    FragmentOverlayWriter& operator=(const FragmentOverlayWriter&) = delete;

    /// Write one borrowed batch of overlay values.
    void write_batch(const Batch& batch) {
        if (lance_fragment_overlay_writer_write_batch(
                handle_.get(), batch.c_handle()) != 0) {
            check_error();
        }
    }

    /// Finish the staged overlay. Returns `std::nullopt` when the native
    /// coverage was empty. The writer is consumed on every return.
    std::optional<DataOverlayGroup> finish() {
        auto* raw = handle_.release();
        LanceDataOverlayGroup* group = nullptr;
        if (lance_fragment_overlay_writer_finish(raw, &group) != 0) {
            check_error();
        }
        if (!group) return std::nullopt;
        return DataOverlayGroup(group);
    }

    /// Discard staged data best-effort. The writer is consumed.
    void abort() {
        auto* raw = handle_.release();
        if (raw) lance_fragment_overlay_writer_abort(raw);
    }

    const LanceFragmentOverlayWriter* c_handle() const {
        return handle_.get();
    }
};

inline FragmentOverlayWriter Fragment::write_overlay(
    const LanceSchema& schema) const {
    LanceFragmentOverlayWriter* raw = nullptr;
    if (lance_fragment_overlay_writer_new(
            handle_.get(), schema.c_handle(), &raw) != 0) {
        check_error();
    }
    if (!raw) {
        throw Error(LANCE_ERR_INTERNAL,
                    "native overlay writer returned NULL on success");
    }
    return FragmentOverlayWriter(raw);
}

// ─── Raw file ownership and snapshots ───────────────────────────────────────

/// Move-only owner of an Arrow C Data Interface schema.
class ArrowSchemaOwner {
    ArrowSchema schema_{};
    bool active_ = false;

public:
    ArrowSchemaOwner() noexcept = default;
    explicit ArrowSchemaOwner(ArrowSchema schema) noexcept
        : schema_(schema), active_(schema.release != nullptr) {}
    ~ArrowSchemaOwner() { reset(); }

    ArrowSchemaOwner(const ArrowSchemaOwner&) = delete;
    ArrowSchemaOwner& operator=(const ArrowSchemaOwner&) = delete;

    ArrowSchemaOwner(ArrowSchemaOwner&& other) noexcept
        : schema_(other.schema_), active_(other.active_) {
        other.schema_ = {};
        other.active_ = false;
    }

    ArrowSchemaOwner& operator=(ArrowSchemaOwner&& other) noexcept {
        if (this != &other) {
            reset();
            schema_ = other.schema_;
            active_ = other.active_;
            other.schema_ = {};
            other.active_ = false;
        }
        return *this;
    }

    void reset() noexcept {
        if (active_ && schema_.release) schema_.release(&schema_);
        schema_ = {};
        active_ = false;
    }

    ArrowSchema* get() noexcept { return &schema_; }
    const ArrowSchema* get() const noexcept { return &schema_; }
    explicit operator bool() const noexcept { return active_; }
};

enum class FileCacheMode : int32_t {
    None = LANCE_FILE_CACHE_NONE,
    Session = LANCE_FILE_CACHE_SESSION,
};

enum class FileFormatVersion : int32_t {
    V1 = LANCE_FILE_FORMAT_V1,
    V2_0 = LANCE_FILE_FORMAT_V2_0,
    V2_1 = LANCE_FILE_FORMAT_V2_1,
    V2_2 = LANCE_FILE_FORMAT_V2_2,
    V2_3 = LANCE_FILE_FORMAT_V2_3,
};

enum class FileEncodingKind : int32_t {
    Absent = LANCE_FILE_ENCODING_ABSENT,
    None = LANCE_FILE_ENCODING_NONE,
    Direct = LANCE_FILE_ENCODING_DIRECT,
    Indirect = LANCE_FILE_ENCODING_INDIRECT,
};

struct FileSchedulerConfig {
    uint64_t io_buffer_size_bytes = 0;
    LanceSchedulerLiteMode use_lite_scheduler = LANCE_SCHEDULER_LITE_AUTO;
};

struct FileReaderOptions {
    std::optional<std::vector<std::string>> columns;
    FileCacheMode cache_mode = FileCacheMode::None;
    std::optional<uint64_t> read_chunk_size;
    std::optional<uint64_t> batch_size_bytes;
    std::optional<bool> cache_repetition_index;
    std::optional<bool> validate_on_decode;
    std::optional<bool> inline_scheduling;
};

struct FileWriterOptions {
    std::optional<uint64_t> data_cache_bytes;
    std::optional<uint64_t> max_page_bytes;
    std::optional<bool> keep_original_array;
    std::optional<std::string> version;
};

struct FileWriteSummary {
    uint64_t num_rows = 0;
    uint64_t size_bytes = 0;
};

struct FileMetadataInfo {
    FileFormatVersion format_version = FileFormatVersion::V1;
    uint16_t major_version = 0;
    uint16_t minor_version = 0;
    uint64_t num_rows = 0;
    uint64_t num_data_bytes = 0;
    uint64_t num_column_metadata_bytes = 0;
    uint64_t num_global_buffer_bytes = 0;
    uint64_t num_footer_bytes = 0;
    uint64_t file_size_bytes = 0;
    std::size_t num_columns = 0;
    std::size_t num_physical_columns = 0;
    std::size_t num_global_buffers = 0;
};

struct FileBufferDescriptor {
    uint64_t position = 0;
    uint64_t size = 0;
};

struct FileEncodingDescriptor {
    FileEncodingKind kind = FileEncodingKind::Absent;
    uint64_t buffer_location = 0;
    uint64_t buffer_length = 0;
    std::vector<uint8_t> direct_bytes;
};

struct FilePageMetadata {
    std::vector<FileBufferDescriptor> buffers;
    uint64_t num_rows = 0;
    uint64_t priority = 0;
    FileEncodingDescriptor encoding;
    std::string encoding_description;
};

struct FileColumnMetadata {
    std::vector<FileBufferDescriptor> column_buffers;
    std::vector<FilePageMetadata> pages;
    FileEncodingDescriptor encoding;
};

struct FileColumnStatistics {
    std::size_t num_pages = 0;
    uint64_t size_bytes = 0;
};

struct FileStoreInfo {
    uint64_t block_size = 0;
    uint64_t max_iop_size = 0;
    uint64_t io_parallelism = 0;
    bool is_local = false;
    bool is_cloud = false;
    bool prefers_lite_scheduler = false;
};

namespace file_detail {

inline std::string take_string(const char* value) {
    if (!value) check_error();
    std::string output(value);
    lance_free_string(value);
    return output;
}

inline FileBufferDescriptor buffer_descriptor(
    const LanceBufferDescriptor& descriptor) {
    return FileBufferDescriptor{descriptor.position, descriptor.size};
}

inline FileEncodingDescriptor encoding_descriptor(
    const LanceEncodingDescriptor& descriptor) {
    FileEncodingDescriptor output;
    output.kind = static_cast<FileEncodingKind>(descriptor.kind);
    output.buffer_location = descriptor.buffer_location;
    output.buffer_length = descriptor.buffer_length;
    if (descriptor.direct_bytes_len != 0) {
        if (!descriptor.direct_bytes) check_error();
        output.direct_bytes.assign(
            descriptor.direct_bytes,
            descriptor.direct_bytes + descriptor.direct_bytes_len);
    }
    return output;
}

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

}  // namespace file_detail

class FileMetadata {
    Handle<LanceFileMetadata, lance_file_metadata_close> handle_;

public:
    explicit FileMetadata(LanceFileMetadata* metadata) : handle_(metadata) {}

    FileMetadataInfo info() const {
        LanceFileMetadataInfo raw{};
        if (lance_file_metadata_get_info(handle_.get(), &raw) != 0)
            check_error();
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

    ArrowSchemaOwner schema() const {
        ArrowSchema raw{};
        if (lance_file_metadata_get_schema(handle_.get(), &raw) != 0)
            check_error();
        return ArrowSchemaOwner(raw);
    }

    std::vector<FileBufferDescriptor> global_buffers() const {
        const LanceBufferDescriptor* descriptors = nullptr;
        std::size_t count = 0;
        if (lance_file_metadata_get_global_buffers(
                handle_.get(), &descriptors, &count) != 0) {
            check_error();
        }
        std::vector<FileBufferDescriptor> output;
        output.reserve(count);
        for (std::size_t index = 0; index < count; index++) {
            output.push_back(
                file_detail::buffer_descriptor(descriptors[index]));
        }
        return output;
    }

    std::vector<FileColumnMetadata> columns() const {
        const LanceColumnMetadata* columns = nullptr;
        std::size_t count = 0;
        if (lance_file_metadata_get_columns(
                handle_.get(), &columns, &count) != 0) {
            check_error();
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
};

class FileStatistics {
    Handle<LanceFileStatistics, lance_file_statistics_close> handle_;

public:
    explicit FileStatistics(LanceFileStatistics* statistics)
        : handle_(statistics) {}

    std::vector<FileColumnStatistics> columns() const {
        const LanceColumnStatistics* columns = nullptr;
        std::size_t count = 0;
        if (lance_file_statistics_get_columns(
                handle_.get(), &columns, &count) != 0) {
            check_error();
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
};

class ByteBuffer {
    Handle<LanceByteBuffer, lance_byte_buffer_close> handle_;

public:
    explicit ByteBuffer(LanceByteBuffer* buffer) : handle_(buffer) {}

    std::vector<uint8_t> bytes() const {
        std::size_t length = lance_byte_buffer_len(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        if (length == 0) return {};
        const uint8_t* data = lance_byte_buffer_data(handle_.get());
        if (!data) check_error();
        return std::vector<uint8_t>(data, data + length);
    }
};

class PathList {
    Handle<LancePathList, lance_path_list_close> handle_;

public:
    explicit PathList(LancePathList* list) : handle_(list) {}

    std::vector<std::string> paths() const {
        uint64_t count = lance_path_list_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        std::vector<std::string> output;
        output.reserve(static_cast<std::size_t>(count));
        for (uint64_t index = 0; index < count; index++) {
            const char* path = lance_path_list_at(
                handle_.get(), static_cast<std::size_t>(index));
            if (!path) check_error();
            output.emplace_back(path);
        }
        return output;
    }
};

class DelimitedList {
    Handle<LanceDelimitedList, lance_delimited_list_close> handle_;

public:
    explicit DelimitedList(LanceDelimitedList* list) : handle_(list) {}

    std::vector<std::string> common_prefixes() const {
        uint64_t count =
            lance_delimited_list_common_prefix_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        std::vector<std::string> output;
        output.reserve(static_cast<std::size_t>(count));
        for (uint64_t index = 0; index < count; index++) {
            const char* path = lance_delimited_list_common_prefix_at(
                handle_.get(), static_cast<std::size_t>(index));
            if (!path) check_error();
            output.emplace_back(path);
        }
        return output;
    }

    std::vector<std::string> objects() const {
        uint64_t count = lance_delimited_list_object_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) check_error();
        std::vector<std::string> output;
        output.reserve(static_cast<std::size_t>(count));
        for (uint64_t index = 0; index < count; index++) {
            const char* path = lance_delimited_list_object_at(
                handle_.get(), static_cast<std::size_t>(index));
            if (!path) check_error();
            output.emplace_back(path);
        }
        return output;
    }
};

class FileReader;
class FileWriter;

class FileSession {
    Handle<LanceFileSession, lance_file_session_close> handle_;

public:
    FileSession(
        const std::string& base_uri,
        const StorageOptionsAccessor* storage_options = nullptr,
        const Session* session = nullptr,
        std::optional<FileSchedulerConfig> scheduler_config = std::nullopt)
        : handle_(nullptr) {
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
        if (!raw) check_error();
        handle_ = Handle<LanceFileSession, lance_file_session_close>(raw);
    }

    FileSession(FileSession&&) noexcept = default;
    FileSession& operator=(FileSession&&) noexcept = default;
    FileSession(const FileSession&) = delete;
    FileSession& operator=(const FileSession&) = delete;

    void reset() noexcept {
        handle_ = Handle<LanceFileSession, lance_file_session_close>();
    }
    explicit operator bool() const noexcept { return bool(handle_); }

    std::string base_path() const {
        return file_detail::take_string(
            lance_file_session_base_path(handle_.get()));
    }

    FileReader open_reader(
        const std::string& relative_path,
        const FileReaderOptions& options = {}) const;
    FileWriter open_writer(
        const std::string& relative_path,
        const ArrowSchema* schema,
        const FileWriterOptions& options = {}) const;

    PackedBlobWriter open_packed_blob_writer(
        const std::string& data_file_path,
        uint32_t blob_id) const {
        auto* raw = lance_file_session_open_packed_blob_writer(
            handle_.get(), data_file_path.c_str(), blob_id);
        if (!raw) check_error();
        return PackedBlobWriter(raw);
    }

    DedicatedBlobWriter open_dedicated_blob_writer(
        const std::string& data_file_path,
        uint32_t blob_id) const {
        auto* raw = lance_file_session_open_dedicated_blob_writer(
            handle_.get(), data_file_path.c_str(), blob_id);
        if (!raw) check_error();
        return DedicatedBlobWriter(raw);
    }

    bool contains(const std::string& relative_path) const {
        int32_t exists = 0;
        if (lance_file_session_contains(
                handle_.get(), relative_path.c_str(), &exists) != 0) {
            check_error();
        }
        return exists != 0;
    }

    PathList list(std::optional<std::string> relative_path = std::nullopt) const {
        auto* raw = lance_file_session_list(
            handle_.get(),
            relative_path ? relative_path->c_str() : nullptr);
        if (!raw) check_error();
        return PathList(raw);
    }

    DelimitedList list_with_delimiter(
        std::optional<std::string> relative_path = std::nullopt) const {
        auto* raw = lance_file_session_list_with_delimiter(
            handle_.get(),
            relative_path ? relative_path->c_str() : nullptr);
        if (!raw) check_error();
        return DelimitedList(raw);
    }

    ByteBuffer read_range(
        const std::string& relative_path,
        uint64_t offset,
        uint64_t length) const {
        auto* raw = lance_file_session_read_range(
            handle_.get(), relative_path.c_str(), offset, length);
        if (!raw) check_error();
        return ByteBuffer(raw);
    }

    void delete_file(const std::string& relative_path) const {
        if (lance_file_session_delete_file(
                handle_.get(), relative_path.c_str()) != 0) {
            check_error();
        }
    }

    void upload_file(
        const std::string& local_path,
        const std::string& relative_remote_path) const {
        if (lance_file_session_upload_file(
                handle_.get(),
                local_path.c_str(),
                relative_remote_path.c_str()) != 0) {
            check_error();
        }
    }

    void download_file(
        const std::string& relative_remote_path,
        const std::string& local_path) const {
        if (lance_file_session_download_file(
                handle_.get(),
                relative_remote_path.c_str(),
                local_path.c_str()) != 0) {
            check_error();
        }
    }

    LanceScanStats scheduler_stats() const {
        LanceScanStats stats{};
        if (lance_file_session_get_scheduler_stats(handle_.get(), &stats) != 0)
            check_error();
        return stats;
    }

    LanceObjectStoreRegistryStats registry_stats() const {
        LanceObjectStoreRegistryStats stats{};
        if (lance_file_session_get_registry_stats(handle_.get(), &stats) != 0)
            check_error();
        return stats;
    }

    FileStoreInfo store_info() const {
        LanceObjectStoreInfo info{};
        if (lance_file_session_get_store_info(handle_.get(), &info) != 0)
            check_error();
        return FileStoreInfo{
            info.block_size,
            info.max_iop_size,
            info.io_parallelism,
            info.is_local != 0,
            info.is_cloud != 0,
            info.prefers_lite_scheduler != 0,
        };
    }

    LanceObjectStoreIoStats store_io_stats() const {
        LanceObjectStoreIoStats stats{};
        if (lance_file_session_get_store_io_stats(handle_.get(), &stats) != 0)
            check_error();
        return stats;
    }
};

class FileReader {
    Handle<LanceFileReader, lance_file_reader_close> handle_;

public:
    explicit FileReader(LanceFileReader* reader) : handle_(reader) {}
    FileReader(FileReader&&) noexcept = default;
    FileReader& operator=(FileReader&&) noexcept = default;
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    void reset() noexcept {
        handle_ = Handle<LanceFileReader, lance_file_reader_close>();
    }
    explicit operator bool() const noexcept { return bool(handle_); }

    uint64_t num_rows() const {
        uint64_t rows = 0;
        if (lance_file_reader_num_rows(handle_.get(), &rows) != 0)
            check_error();
        return rows;
    }

    FileMetadata metadata() const {
        LanceFileMetadata* metadata = nullptr;
        if (lance_file_reader_get_metadata(handle_.get(), &metadata) != 0)
            check_error();
        return FileMetadata(metadata);
    }

    FileStatistics statistics() const {
        LanceFileStatistics* statistics = nullptr;
        if (lance_file_reader_get_statistics(handle_.get(), &statistics) != 0)
            check_error();
        return FileStatistics(statistics);
    }

    ByteBuffer read_global_buffer(uint32_t index) const {
        LanceByteBuffer* buffer = nullptr;
        if (lance_file_reader_read_global_buffer(
                handle_.get(), index, &buffer) != 0) {
            check_error();
        }
        return ByteBuffer(buffer);
    }

    ArrowStream read_all(
        uint32_t batch_size = 1024,
        uint32_t batch_readahead = 4) const {
        ArrowArrayStream stream{};
        if (lance_file_reader_read_all(
                handle_.get(), batch_size, batch_readahead, &stream) != 0) {
            check_error();
        }
        return ArrowStream(stream);
    }

    ArrowStream read_range(
        uint64_t offset,
        uint64_t num_rows,
        uint32_t batch_size = 1024,
        uint32_t batch_readahead = 4) const {
        ArrowArrayStream stream{};
        if (lance_file_reader_read_range(
                handle_.get(),
                offset,
                num_rows,
                batch_size,
                batch_readahead,
                &stream) != 0) {
            check_error();
        }
        return ArrowStream(stream);
    }

    ArrowStream read_indices(
        const std::vector<uint32_t>& indices,
        uint32_t batch_size = 1024,
        uint32_t batch_readahead = 4) const {
        ArrowArrayStream stream{};
        if (lance_file_reader_read_indices(
                handle_.get(),
                indices.empty() ? nullptr : indices.data(),
                indices.size(),
                batch_size,
                batch_readahead,
                &stream) != 0) {
            check_error();
        }
        return ArrowStream(stream);
    }

    LanceScanStats io_stats() const {
        LanceScanStats stats{};
        if (lance_file_reader_get_io_stats(handle_.get(), &stats) != 0)
            check_error();
        return stats;
    }
};

class FileWriter {
    Handle<LanceFileWriter, lance_file_writer_close> handle_;

public:
    explicit FileWriter(LanceFileWriter* writer) : handle_(writer) {}
    FileWriter(FileWriter&&) noexcept = default;
    FileWriter& operator=(FileWriter&&) noexcept = default;
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;

    void reset() noexcept {
        handle_ = Handle<LanceFileWriter, lance_file_writer_close>();
    }
    explicit operator bool() const noexcept { return bool(handle_); }

    void write_batch(const Batch& batch) {
        if (lance_file_writer_write_batch(handle_.get(), batch.c_handle()) != 0)
            check_error();
    }

    void add_schema_metadata(
        const std::string& key,
        const std::string& value) {
        if (lance_file_writer_add_schema_metadata(
                handle_.get(), key.c_str(), value.c_str()) != 0) {
            check_error();
        }
    }

    uint32_t add_global_buffer(const std::vector<uint8_t>& bytes) {
        uint32_t index = 0;
        if (lance_file_writer_add_global_buffer(
                handle_.get(),
                bytes.empty() ? nullptr : bytes.data(),
                bytes.size(),
                &index) != 0) {
            check_error();
        }
        return index;
    }

    FileWriteSummary finish() {
        LanceFileWriteSummary summary{};
        if (lance_file_writer_finish(handle_.get(), &summary) != 0)
            check_error();
        return FileWriteSummary{summary.num_rows, summary.size_bytes};
    }

    void abort() {
        if (lance_file_writer_abort(handle_.get()) != 0) check_error();
    }
};

inline FileReader FileSession::open_reader(
    const std::string& relative_path,
    const FileReaderOptions& options) const {
    std::vector<const char*> columns;
    auto raw_options = file_detail::reader_options(options, columns);
    auto* raw = lance_file_session_open_reader(
        handle_.get(), relative_path.c_str(), &raw_options);
    if (!raw) check_error();
    return FileReader(raw);
}

inline FileWriter FileSession::open_writer(
    const std::string& relative_path,
    const ArrowSchema* schema,
    const FileWriterOptions& options) const {
    auto raw_options = file_detail::writer_options(options);
    auto* raw = lance_file_session_open_writer(
        handle_.get(), relative_path.c_str(), schema, &raw_options);
    if (!raw) check_error();
    return FileWriter(raw);
}

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

// ─── Native namespaces ───────────────────────────────────────────────────────

namespace namespace_detail {

inline std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

inline std::string json_string(std::string_view value) {
    return std::string("\"") + json_escape(value) + "\"";
}

inline std::string json_string_array(const std::vector<std::string>& values) {
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out += ",";
        out += json_string(values[i]);
    }
    out += "]";
    return out;
}

inline std::string json_string_object(
    const std::vector<std::pair<std::string, std::string>>& entries) {
    std::string out = "{";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i != 0) out += ",";
        out += json_string(entries[i].first);
        out += ":";
        out += json_string(entries[i].second);
    }
    out += "}";
    return out;
}

inline void free_owned_string(LanceOwnedString* value) noexcept {
    delete[] value->data;
    value->data = nullptr;
    value->release = nullptr;
}

inline void set_owned_string(LanceOwnedString* out, std::string value) {
    auto* data = new char[value.size() + 1];
    std::memcpy(data, value.data(), value.size());
    data[value.size()] = '\0';
    out->data = data;
    out->release = &free_owned_string;
}

inline void set_callback_error(
    LanceCallbackError* error, int32_t code, const std::string& message) noexcept {
    error->code = code;
    try {
        set_owned_string(&error->message, message);
    } catch (...) {
        error->message.data = nullptr;
        error->message.release = nullptr;
    }
}

} // namespace namespace_detail

/// Namespace operation error that preserves both the generic C error class and
/// the native NamespaceError code/message.
class NamespaceError : public Error {
public:
    bool has_native_code;
    uint32_t native_code;
    std::string native_message;

    NamespaceError(
        LanceErrorCode code,
        std::string message,
        uint32_t native_code,
        std::string native_message)
        : Error(code, std::move(message)),
          has_native_code(true),
          native_code(native_code),
          native_message(std::move(native_message)) {}
};

/// Owning copy of one native namespace call result. The C result is released
/// before this object is returned, so no native resources outlive it.
class NamespaceResult {
    std::optional<std::string> json_;
    std::vector<uint8_t> bytes_;

    friend class Namespace;

    NamespaceResult(std::optional<std::string> json, std::vector<uint8_t> bytes)
        : json_(std::move(json)), bytes_(std::move(bytes)) {}

public:
    NamespaceResult() = default;
    NamespaceResult(NamespaceResult&&) noexcept = default;
    NamespaceResult& operator=(NamespaceResult&&) noexcept = default;
    NamespaceResult(const NamespaceResult&) = delete;
    NamespaceResult& operator=(const NamespaceResult&) = delete;

    bool has_json() const noexcept { return json_.has_value(); }
    const std::optional<std::string>& json() const noexcept { return json_; }
    const std::vector<uint8_t>& bytes() const noexcept { return bytes_; }
};

/// Synchronous, infallible dynamic context provider. C++ exceptions are
/// converted to a callback error; the native client logs it and sends an empty
/// map. The callback receives the native operation name and object id.
class DynamicContextProvider {
public:
    using Callback = std::function<std::map<std::string, std::string>(
        const std::string&, const std::string&)>;

private:
    Callback callback_;

public:
    explicit DynamicContextProvider(Callback callback)
        : callback_(std::move(callback)) {}
    const Callback& callback() const noexcept { return callback_; }
};

/// Move-only namespace handle with native concrete-implementation retention.
class Namespace {
    Handle<LanceNamespace, lance_namespace_close> handle_;

    struct ContextProviderContext {
        std::function<std::map<std::string, std::string>(
            const std::string&, const std::string&)> callback;
    };

    struct CustomClientContext {
        std::function<std::string(const std::string&)> describe_table;
        std::function<std::string(const std::string&)> describe_table_version;
        std::function<std::string(const std::string&)> create_table_version;
        std::function<std::string(const std::string&)> list_table_versions;
    };

    static void context_provider_release(void* ctx) noexcept {
        delete static_cast<ContextProviderContext*>(ctx);
    }

    static void custom_client_release(void* ctx) noexcept {
        delete static_cast<CustomClientContext*>(ctx);
    }

    static int32_t context_provider_call(
        void* raw_ctx,
        const char* operation,
        const char* object_id,
        LanceOwnedString* out_json,
        LanceCallbackError* out_error) noexcept {
        auto* ctx = static_cast<ContextProviderContext*>(raw_ctx);
        try {
            std::map<std::string, std::string> context = ctx->callback(
                operation ? operation : "", object_id ? object_id : "");
            std::string json = "{";
            bool first = true;
            for (const auto& [key, value] : context) {
                if (!first) json += ",";
                first = false;
                json += namespace_detail::json_string(key);
                json += ":";
                json += namespace_detail::json_string(value);
            }
            json += "}";
            namespace_detail::set_owned_string(out_json, std::move(json));
            return 0;
        } catch (const lance::Error& error) {
            namespace_detail::set_callback_error(
                out_error, static_cast<int32_t>(error.code), error.what());
        } catch (const std::exception& error) {
            namespace_detail::set_callback_error(
                out_error, static_cast<int32_t>(LANCE_ERR_INTERNAL), error.what());
        } catch (...) {
            namespace_detail::set_callback_error(
                out_error,
                static_cast<int32_t>(LANCE_ERR_INTERNAL),
                "unknown C++ exception in namespace context provider");
        }
        return 1;
    }

    static int32_t invoke_custom(
        CustomClientContext* ctx,
        std::function<std::string(const std::string&)>& callback,
        const char* request_json,
        LanceOwnedString* out_json,
        LanceCallbackError* out_error) noexcept {
        (void)ctx;
        try {
            std::string response = callback(request_json ? request_json : "{}");
            namespace_detail::set_owned_string(out_json, std::move(response));
            return 0;
        } catch (const lance::Error& error) {
            namespace_detail::set_callback_error(
                out_error, static_cast<int32_t>(error.code), error.what());
        } catch (const std::exception& error) {
            namespace_detail::set_callback_error(
                out_error, static_cast<int32_t>(LANCE_ERR_INTERNAL), error.what());
        } catch (...) {
            namespace_detail::set_callback_error(
                out_error,
                static_cast<int32_t>(LANCE_ERR_INTERNAL),
                "unknown C++ exception in namespace callback");
        }
        return 1;
    }

    static int32_t custom_describe_table(
        void* ctx, const char* req, LanceOwnedString* out, LanceCallbackError* err) noexcept {
        auto* client = static_cast<CustomClientContext*>(ctx);
        return invoke_custom(client, client->describe_table, req, out, err);
    }
    static int32_t custom_describe_table_version(
        void* ctx, const char* req, LanceOwnedString* out, LanceCallbackError* err) noexcept {
        auto* client = static_cast<CustomClientContext*>(ctx);
        return invoke_custom(client, client->describe_table_version, req, out, err);
    }
    static int32_t custom_create_table_version(
        void* ctx, const char* req, LanceOwnedString* out, LanceCallbackError* err) noexcept {
        auto* client = static_cast<CustomClientContext*>(ctx);
        return invoke_custom(client, client->create_table_version, req, out, err);
    }
    static int32_t custom_list_table_versions(
        void* ctx, const char* req, LanceOwnedString* out, LanceCallbackError* err) noexcept {
        auto* client = static_cast<CustomClientContext*>(ctx);
        return invoke_custom(client, client->list_table_versions, req, out, err);
    }

    static LanceNamespaceContextProviderSpec make_context_spec(
        const DynamicContextProvider* provider) {
        LanceNamespaceContextProviderSpec spec{};
        if (provider) {
            auto* ctx = new ContextProviderContext{provider->callback()};
            spec.provide_context = &context_provider_call;
            spec.release = &context_provider_release;
            spec.provider_ctx = ctx;
        }
        return spec;
    }

    explicit Namespace(LanceNamespace* handle) : handle_(handle) {}

public:
    /// Foreign client for the four Python-compatible managed-version
    /// callbacks. Each callback receives native request JSON and returns
    /// native response JSON. Other operations keep native Unsupported.
    struct CustomClientSpec {
        std::function<std::string(const std::string&)> describe_table;
        std::function<std::string(const std::string&)> describe_table_version;
        std::function<std::string(const std::string&)> create_table_version;
        std::function<std::string(const std::string&)> list_table_versions;
        /// Empty assigns a unique identity stable for this client's lifetime.
        std::string namespace_id;
    };

    Namespace(Namespace&&) noexcept = default;
    Namespace& operator=(Namespace&&) noexcept = default;
    Namespace(const Namespace&) = delete;
    Namespace& operator=(const Namespace&) = delete;

    static Namespace directory(
        const std::vector<std::pair<std::string, std::string>>& properties = {},
        const Session* session = nullptr,
        const DynamicContextProvider* context_provider = nullptr) {
        auto json = namespace_detail::json_string_object(properties);
        auto spec = make_context_spec(context_provider);
        auto* spec_ptr = context_provider ? &spec : nullptr;
        auto* raw = lance_namespace_directory_new(
            json.c_str(), session ? session->c_handle() : nullptr, spec_ptr);
        if (!raw) check_error();
        return Namespace(raw);
    }

    static Namespace rest(
        const std::vector<std::pair<std::string, std::string>>& properties,
        const DynamicContextProvider* context_provider = nullptr) {
        auto json = namespace_detail::json_string_object(properties);
        auto spec = make_context_spec(context_provider);
        auto* spec_ptr = context_provider ? &spec : nullptr;
        auto* raw = lance_namespace_rest_new(json.c_str(), spec_ptr);
        if (!raw) check_error();
        return Namespace(raw);
    }

    static Namespace connect(
        const std::string& impl_name,
        const std::vector<std::pair<std::string, std::string>>& properties = {},
        const Session* session = nullptr,
        const DynamicContextProvider* context_provider = nullptr) {
        auto json = namespace_detail::json_string_object(properties);
        auto spec = make_context_spec(context_provider);
        auto* spec_ptr = context_provider ? &spec : nullptr;
        auto* raw = lance_namespace_connect_new(
            impl_name.c_str(),
            json.c_str(),
            session ? session->c_handle() : nullptr,
            spec_ptr);
        if (!raw) check_error();
        return Namespace(raw);
    }

    static Namespace custom(const CustomClientSpec& spec) {
        if (!spec.describe_table || !spec.describe_table_version ||
            !spec.create_table_version || !spec.list_table_versions) {
            throw Error(
                LANCE_ERR_INVALID_ARGUMENT,
                "custom namespace requires all four managed-version callbacks");
        }
        auto* ctx = new CustomClientContext{
            spec.describe_table,
            spec.describe_table_version,
            spec.create_table_version,
            spec.list_table_versions};
        std::unique_ptr<CustomClientContext> guard(ctx);
        LanceNamespaceCustomClientSpec c_spec{};
        c_spec.describe_table = &custom_describe_table;
        c_spec.describe_table_version = &custom_describe_table_version;
        c_spec.create_table_version = &custom_create_table_version;
        c_spec.list_table_versions = &custom_list_table_versions;
        c_spec.release = &custom_client_release;
        c_spec.client_ctx = ctx;
        c_spec.namespace_id = spec.namespace_id.c_str();
        auto* raw = lance_namespace_custom_new(&c_spec);
        // The C constructor consumes the context on every invocation.
        guard.release();
        if (!raw) check_error();
        return Namespace(raw);
    }

    std::string id() const {
        const char* raw = lance_namespace_id(handle_.get());
        if (!raw) check_error();
        std::string out(raw);
        lance_free_string(raw);
        return out;
    }

    std::string ops_metrics_json() const {
        const char* raw = lance_namespace_retrieve_ops_metrics(handle_.get());
        if (!raw) check_error();
        std::string out(raw);
        lance_free_string(raw);
        return out;
    }

    void reset_ops_metrics() {
        if (lance_namespace_reset_ops_metrics(handle_.get()) != 0) check_error();
    }

    NamespaceResult call(
        const std::string& operation,
        const std::string& request_json = "{}",
        const uint8_t* request_bytes = nullptr,
        size_t request_bytes_len = 0) const {
        LanceNamespaceResult raw{};
        struct ResultGuard {
            LanceNamespaceResult* result;
            ~ResultGuard() { if (result) lance_namespace_result_close(result); }
        } guard{&raw};
        int32_t rc = lance_namespace_call(
            handle_.get(),
            operation.c_str(),
            request_json.c_str(),
            request_bytes,
            request_bytes_len,
            &raw);
        std::optional<std::string> json;
        std::vector<uint8_t> bytes;
        if (raw.result_json) json = std::string(raw.result_json);
        if (raw.result_bytes && raw.result_bytes_len != 0) {
            bytes.assign(raw.result_bytes, raw.result_bytes + raw.result_bytes_len);
        }
        bool has_native = raw.has_namespace_error != 0;
        uint32_t native_code = raw.namespace_error_code;
        std::string native_message =
            raw.namespace_error_message ? raw.namespace_error_message : "";
        lance_namespace_result_close(&raw);
        guard.result = nullptr;
        if (rc != 0) {
            LanceErrorCode code = lance_last_error_code();
            const char* message = lance_last_error_message();
            std::string owned(message ? message : "Unknown error");
            if (message) lance_free_string(message);
            if (has_native) {
                throw NamespaceError(
                    code, std::move(owned), native_code, std::move(native_message));
            }
            throw Error(code, std::move(owned));
        }
        return NamespaceResult(std::move(json), std::move(bytes));
    }

    NamespaceResult call(
        const std::string& operation,
        const std::string& request_json,
        const std::vector<uint8_t>& request_bytes) const {
        return call(
            operation,
            request_json,
            request_bytes.empty() ? nullptr : request_bytes.data(),
            request_bytes.size());
    }

    const LanceNamespace* c_handle() const noexcept { return handle_.get(); }
};

/// Move-only owner of a running native REST adapter.
class RestAdapter {
    Handle<LanceRestAdapter, lance_rest_adapter_close> handle_;
    uint16_t port_ = 0;

    explicit RestAdapter(LanceRestAdapter* raw) : handle_(raw) {}

public:
    RestAdapter(RestAdapter&& other) noexcept
        : handle_(std::move(other.handle_)), port_(other.port_) {
        other.port_ = 0;
    }
    RestAdapter& operator=(RestAdapter&& other) noexcept {
        if (this != &other) {
            handle_ = std::move(other.handle_);
            port_ = other.port_;
            other.port_ = 0;
        }
        return *this;
    }
    RestAdapter(const RestAdapter&) = delete;
    RestAdapter& operator=(const RestAdapter&) = delete;

    static RestAdapter start(
        const Namespace& backend,
        const std::string& host = "127.0.0.1",
        uint16_t port = 0) {
        LanceRestAdapter* raw = nullptr;
        if (lance_rest_adapter_start(
                backend.c_handle(), host.c_str(), port, &raw) != 0) {
            check_error();
        }
        if (!raw) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_rest_adapter_start returned success with null adapter");
        }
        RestAdapter adapter(raw);
        adapter.port_ = lance_rest_adapter_port(raw);
        return adapter;
    }

    uint16_t port() const noexcept { return port_; }

    void shutdown() {
        lance_rest_adapter_shutdown(handle_.get());
        port_ = 0;
    }
};

inline Dataset Dataset::open_namespace(
    const Namespace& ns,
    const std::vector<std::string>& table_id,
    const std::vector<std::pair<std::string, std::string>>& storage_opts,
    uint64_t version,
    const Session* session) {
    auto table_json = namespace_detail::json_string_array(table_id);
    auto storage_json = namespace_detail::json_string_object(storage_opts);
    auto* raw = lance_dataset_open_namespace(
        ns.c_handle(),
        table_json.c_str(),
        storage_json.c_str(),
        version,
        session ? session->c_handle() : nullptr);
    if (!raw) check_error();
    return Dataset(raw);
}

inline Dataset Dataset::write_namespace(
    const Namespace& ns,
    const std::vector<std::string>& table_id,
    ArrowArrayStream* stream,
    WriteMode mode,
    const WriteOptions& options,
    const std::vector<std::pair<std::string, std::string>>& storage_opts,
    const std::optional<std::string>& branch,
    const Session* session) {
    ArrowStreamInput input(stream);
    const ArrowSchema* schema = input.read_schema();
    auto table_json = namespace_detail::json_string_array(table_id);
    auto storage_json = namespace_detail::json_string_object(storage_opts);
    LanceAutoCleanupParams cleanup{};
    LanceWriteOptions c_options = to_c_write_options(options, cleanup);
    LanceDataset* out = nullptr;
    auto* owned_stream = input.consume();
    int32_t rc = lance_dataset_write_namespace(
        ns.c_handle(),
        table_json.c_str(),
        schema,
        owned_stream,
        static_cast<int32_t>(mode),
        nullptr,
        &c_options,
        storage_json.c_str(),
        session ? session->c_handle() : nullptr,
        branch ? branch->c_str() : nullptr,
        &out);
    if (rc != 0) check_error();
    if (!out) {
        throw Error(
            LANCE_ERR_INTERNAL,
            "lance_dataset_write_namespace returned success with null out_dataset");
    }
    return Dataset(out);
}

inline Dataset Dataset::commit_namespace_uri(
    const Namespace& ns,
    const std::vector<std::string>& table_id,
    const std::string& uri,
    const Transaction& transaction,
    const CommitOptions& options,
    const std::vector<std::pair<std::string, std::string>>& storage_opts,
    const Session* session) {
    auto table_json = namespace_detail::json_string_array(table_id);
    auto kv = to_c_key_values(storage_opts);
    const char* const* opts_ptr = storage_opts.empty() ? nullptr : kv.data();
    LanceCommitOptions c_options = to_c_commit_options(options);
    LanceDataset* out = nullptr;
    int32_t rc = lance_transaction_commit_namespace_uri(
        ns.c_handle(),
        table_json.c_str(),
        uri.c_str(),
        transaction.c_handle(),
        &c_options,
        opts_ptr,
        session ? session->c_handle() : nullptr,
        &out);
    if (rc != 0) check_error();
    if (!out) {
        throw Error(
            LANCE_ERR_INTERNAL,
            "lance_transaction_commit_namespace_uri returned success with null out_dataset");
    }
    return Dataset(out);
}


// ─── MemWAL out-of-line definitions ──────────────────────────────────────────

/// One Arrow schema plus a materialized batch list.
struct Table {
    ArrowSchemaOwner schema;
    std::vector<Batch> batches;
};

inline LanceSchema LanceSchema::from_arrow(const ArrowSchema* schema) {
    ::LanceSchema* raw = nullptr;
    if (lance_schema_from_arrow(schema, &raw) != 0) check_error();
    return LanceSchema(raw);
}

inline void Dataset::initialize_mem_wal(const MemWalInitOptions& options) {
    std::vector<const char*> maintained;
    maintained.reserve(options.maintained_indexes.value_or(
        std::vector<std::string>{}).size());
    if (options.maintained_indexes) {
        for (const auto& name : *options.maintained_indexes) {
            maintained.push_back(name.c_str());
        }
    }
    std::vector<std::string> names;
    std::vector<::LanceMemWalHnswParams> hnsw;
    ::LanceShardWriterConfig config{};
    const ::LanceShardWriterConfig* config_ptr = nullptr;
    if (options.writer_config) {
        config = options.writer_config->to_c(names, hnsw);
        config_ptr = &config;
    }
    if (lance_mem_wal_initialize(
            const_cast<::LanceDataset*>(handle_.get()), options.mode,
            options.column ? options.column->c_str() : nullptr,
            options.num_buckets,
            options.maintained_indexes.has_value() ? 1 : 0,
            maintained.empty() ? nullptr : maintained.data(), maintained.size(),
            config_ptr) != 0) {
        check_error();
    }
}

inline std::optional<MemWalIndexDetails> Dataset::mem_wal_index_details() const {
    ::LanceMemWalIndexDetails* raw = nullptr;
    if (lance_mem_wal_index_details(
            const_cast<::LanceDataset*>(handle_.get()), &raw) != 0) {
        check_error();
    }
    if (!raw) return std::nullopt;
    Handle<::LanceMemWalIndexDetails, lance_mem_wal_index_details_free> details(raw);

    std::vector<std::string> maintained;
    const size_t maintained_count =
        lance_mem_wal_index_details_maintained_index_count(details.get());
    maintained.reserve(maintained_count);
    for (size_t i = 0; i < maintained_count; ++i) {
        const char* name = nullptr;
        if (lance_mem_wal_index_details_maintained_index_at(
                details.get(), i, &name) != 0) {
            check_error();
        }
        maintained.emplace_back(name);
    }

    std::vector<ShardingSpec> specs;
    const size_t spec_count =
        lance_mem_wal_index_details_sharding_spec_count(details.get());
    specs.reserve(spec_count);
    for (size_t i = 0; i < spec_count; ++i) {
        ::LanceMemWalShardingSpec* spec = nullptr;
        if (lance_mem_wal_index_details_sharding_spec_at(
                details.get(), i, &spec) != 0) {
            check_error();
        }
        specs.emplace_back(spec);
    }

    std::map<std::string, std::string> defaults;
    const size_t default_count =
        lance_mem_wal_index_details_writer_config_default_count(details.get());
    for (size_t i = 0; i < default_count; ++i) {
        const char* key = nullptr;
        const char* value = nullptr;
        if (lance_mem_wal_index_details_writer_config_default_at(
                details.get(), i, &key, &value) != 0) {
            check_error();
        }
        defaults.emplace(key, value);
    }

    const uint32_t num_shards =
        lance_mem_wal_index_details_num_shards(details.get());
    const int32_t maintain_all =
        lance_mem_wal_index_details_maintain_all_indexes(details.get());
    return MemWalIndexDetails(num_shards, maintain_all != 0,
                              std::move(maintained), std::move(specs),
                              std::move(defaults));
}

inline ShardWriter Dataset::mem_wal_writer(
    const std::string& shard_id, const ShardWriterConfig& config) const {
    std::vector<std::string> names;
    std::vector<::LanceMemWalHnswParams> hnsw;
    auto raw_config = config.to_c(names, hnsw);
    ::LanceShardWriter* raw = nullptr;
    if (lance_shard_writer_open(
            const_cast<::LanceDataset*>(handle_.get()), shard_id.c_str(),
            &raw_config, &raw) != 0) {
        check_error();
    }
    return ShardWriter(raw);
}

inline ShardWriter ShardWriter::open(const Dataset& dataset,
                                     const std::string& shard_id,
                                     const ShardWriterConfig& config) {
    return dataset.mem_wal_writer(shard_id, config);
}

inline LsmScanner ShardWriter::lsm_scanner(
    std::vector<ShardSnapshot> snapshots) {
    std::vector<const ::LanceMemWalShardSnapshot*> raw;
    raw.reserve(snapshots.size());
    for (const auto& snapshot : snapshots) raw.push_back(snapshot.c_handle());
    ::LanceLsmScanner* scanner = nullptr;
    if (lance_shard_writer_lsm_scanner(
            handle_.get(), raw.empty() ? nullptr : raw.data(), raw.size(),
            &scanner) != 0) {
        check_error();
    }
    return LsmScanner(scanner);
}

inline LsmScanner LsmScanner::from_snapshots(
    const Dataset& dataset, std::vector<ShardSnapshot> snapshots) {
    std::vector<const ::LanceMemWalShardSnapshot*> raw;
    raw.reserve(snapshots.size());
    for (const auto& snapshot : snapshots) raw.push_back(snapshot.c_handle());
    ::LanceLsmScanner* scanner = nullptr;
    if (lance_lsm_scanner_from_snapshots(
            const_cast<::LanceDataset*>(dataset.c_handle()),
            raw.empty() ? nullptr : raw.data(), raw.size(), &scanner) != 0) {
        check_error();
    }
    return LsmScanner(scanner);
}

inline Batch LsmScanner::to_batch() const {
    ::LanceBatch* raw = nullptr;
    if (lance_lsm_scanner_to_batch(handle_.get(), &raw) != 0) check_error();
    return Batch(raw);
}

inline std::vector<Batch> LsmScanner::to_batches() const {
    std::vector<Batch> batches;
    batches.push_back(to_batch());
    return batches;
}

inline Table LsmScanner::to_table() const {
    Table table;
    ArrowSchema raw{};
    if (lance_lsm_scanner_schema(handle_.get(), &raw) != 0) {
        check_error();
    }
    table.schema = ArrowSchemaOwner(raw);
    table.batches = to_batches();
    return table;
}

inline ArrowSchemaOwner ExecutionPlan::schema() const {
    ArrowSchema raw{};
    if (lance_mem_wal_plan_schema(handle_.get(), &raw) != 0) {
        check_error();
    }
    return ArrowSchemaOwner(raw);
}

inline ArrowSchemaOwner ExecutionPlan::dataset_schema() const {
    ArrowSchema raw{};
    if (lance_mem_wal_plan_dataset_schema(handle_.get(), &raw) != 0) {
        check_error();
    }
    return ArrowSchemaOwner(raw);
}

inline Batch ExecutionPlan::to_batch() const {
    ::LanceBatch* raw = nullptr;
    if (lance_mem_wal_plan_to_batch(handle_.get(), &raw) != 0) check_error();
    return Batch(raw);
}

inline std::vector<Batch> ExecutionPlan::to_batches() const {
    std::vector<Batch> batches;
    batches.push_back(to_batch());
    return batches;
}

inline Table ExecutionPlan::to_table() const {
    Table table;
    table.schema = schema();
    table.batches = to_batches();
    return table;
}

inline LsmPointLookupPlanner::LsmPointLookupPlanner(
    const Dataset& dataset, std::vector<ShardSnapshot> snapshots,
    std::optional<std::vector<std::string>> pk_columns) {
    std::vector<const ::LanceMemWalShardSnapshot*> raw;
    raw.reserve(snapshots.size());
    for (const auto& snapshot : snapshots) raw.push_back(snapshot.c_handle());
    std::vector<const char*> columns;
    if (pk_columns) {
        columns.reserve(pk_columns->size());
        for (const auto& column : *pk_columns) columns.push_back(column.c_str());
    }
    ::LanceLsmPointLookupPlanner* planner = nullptr;
    if (lance_lsm_point_lookup_planner_new(
            const_cast<::LanceDataset*>(dataset.c_handle()),
            raw.empty() ? nullptr : raw.data(), raw.size(),
            pk_columns.has_value() ? 1 : 0,
            columns.empty() ? nullptr : columns.data(), columns.size(),
            &planner) != 0) {
        check_error();
    }
    handle_ = Handle<::LanceLsmPointLookupPlanner,
                     lance_lsm_point_lookup_planner_free>(planner);
}

inline ExecutionPlan LsmPointLookupPlanner::plan_lookup(
    const Batch& pk_value,
    std::optional<std::vector<std::string>> columns) const {
    std::vector<const char*> raw_columns;
    if (columns) {
        raw_columns.reserve(columns->size());
        for (const auto& column : *columns) raw_columns.push_back(column.c_str());
    }
    ::LanceMemWalPlan* plan = nullptr;
    if (lance_lsm_point_lookup_planner_plan(
            handle_.get(), pk_value.c_handle(),
            columns.has_value() ? 1 : 0,
            raw_columns.empty() ? nullptr : raw_columns.data(),
            raw_columns.size(), &plan) != 0) {
        check_error();
    }
    return ExecutionPlan(plan);
}

inline LsmVectorSearchPlanner::LsmVectorSearchPlanner(
    const Dataset& dataset, std::vector<ShardSnapshot> snapshots,
    std::string vector_column,
    std::optional<std::vector<std::string>> pk_columns,
    MemWalDistanceType distance_type, std::optional<std::string> filter) {
    std::vector<const ::LanceMemWalShardSnapshot*> raw;
    raw.reserve(snapshots.size());
    for (const auto& snapshot : snapshots) raw.push_back(snapshot.c_handle());
    std::vector<const char*> columns;
    if (pk_columns) {
        columns.reserve(pk_columns->size());
        for (const auto& column : *pk_columns) columns.push_back(column.c_str());
    }
    ::LanceLsmVectorSearchPlanner* planner = nullptr;
    if (lance_lsm_vector_search_planner_new(
            const_cast<::LanceDataset*>(dataset.c_handle()),
            raw.empty() ? nullptr : raw.data(), raw.size(),
            vector_column.c_str(), pk_columns.has_value() ? 1 : 0,
            columns.empty() ? nullptr : columns.data(), columns.size(),
            static_cast<::LanceMemWalDistanceType>(distance_type),
            filter ? filter->c_str() : nullptr, &planner) != 0) {
        check_error();
    }
    handle_ = Handle<::LanceLsmVectorSearchPlanner,
                     lance_lsm_vector_search_planner_free>(planner);
}

inline ExecutionPlan LsmVectorSearchPlanner::plan_search(
    const std::vector<float>& query, uint64_t k, uint64_t nprobes,
    std::optional<std::vector<std::string>> columns, bool refine_base_table,
    double overfetch_factor) const {
    std::vector<const char*> raw_columns;
    if (columns) {
        raw_columns.reserve(columns->size());
        for (const auto& column : *columns) raw_columns.push_back(column.c_str());
    }
    ::LanceMemWalPlan* plan = nullptr;
    if (lance_lsm_vector_search_planner_plan(
            handle_.get(), query.empty() ? nullptr : query.data(), query.size(),
            k, nprobes, columns.has_value() ? 1 : 0,
            raw_columns.empty() ? nullptr : raw_columns.data(),
            raw_columns.size(), refine_base_table ? 1 : 0, overfetch_factor,
            &plan) != 0) {
        check_error();
    }
    return ExecutionPlan(plan);
}


} // namespace lance

#endif /* LANCE_HPP */
