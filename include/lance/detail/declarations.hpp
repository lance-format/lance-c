/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Shared C++ declarations for lance.hpp (generated from lance.hpp).
#pragma once

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


#include "lance/lance.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lance {

class Scanner;
struct BatchCommitResult;

// ─── Version ─────────────────────────────────────────────────────────────────

/// Version of the loaded lance-c library, including any pre-release suffix.
/// The returned view refers to process-lifetime storage owned by the library.
std::string_view version() noexcept;

// ─── Error ───────────────────────────────────────────────────────────────────

class Error : public std::runtime_error {
public:
    LanceErrorCode code;

    Error(LanceErrorCode code_value, std::string message);
};

namespace detail {

/// Deleter for strings allocated by the C API.
struct FreeString {
    void operator() (const char* ptr) const noexcept;
};

/// Owner of a string allocated by the C API.
using OwnedString = std::unique_ptr<const char, FreeString>;

/// Throw the pending thread-local error.
///
/// Call this only after a documented failure sentinel. If the native call
/// reported failure without setting a thread-local error, throw
/// `LANCE_ERR_INTERNAL` so the missing detail cannot turn into a null
/// dereference at the call site.
[[noreturn]] void throw_error();

/// Take ownership of a string allocated by the C API.
std::string take_string(const char* raw);

/// Deleter for byte buffers allocated by the C API.
struct FreeBytes {
    void operator() (uint8_t* ptr) const noexcept;
};

/// Owner of a byte buffer allocated by the C API.
using OwnedBytes = std::unique_ptr<uint8_t, FreeBytes>;

/// Take ownership of a byte buffer allocated by the C API. A null buffer is
/// valid only with `len == 0`, where it denotes an empty result.
std::vector<uint8_t> take_bytes(uint8_t* raw, size_t len);

} // namespace detail

/// Check the thread-local error state and throw if it is not `LANCE_OK`.
///
/// Returns normally when the last call succeeded, so it is valid to call after
/// a successful operation. Wrapper failure branches use a non-returning helper
/// instead, so they cannot fall through into an invalid result.
void check_error();

/// Opt in to native diagnostic logging on stderr. `filter` uses the same
/// `RUST_LOG`-style directive syntax as `lance_init_logging`; the default
/// `"info"` matches a NULL filter in C. Throws `lance::Error` on an invalid
/// filter.
void init_logging(std::string_view filter = "info");

/// Build a portable IVF_RQ fast-rotation model as native JSON. `num_bits == 0`
/// selects the native RQ default of 5.
std::string build_rq_model(uint32_t dimension, uint32_t num_bits = 5);

/// Release and free a library-allocated ArrowArrayStream returned by
/// Scanner::scan_async. NULL-safe; do not use for caller-allocated streams.
void scanner_async_stream_free(ArrowArrayStream* stream) noexcept;

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

    explicit ArrowStream(ArrowArrayStream stream) noexcept;

    ~ArrowStream();

    ArrowStream(const ArrowStream&) = delete;
    ArrowStream& operator=(const ArrowStream&) = delete;

    ArrowStream(ArrowStream&& other) noexcept;

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
class FragmentUpdateColumnsResult;
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
class UpdatedFragmentOffsets;
class KeyExistenceFilter;
class RewriteGroup;
class RewrittenIndex;
struct UpdateOperationSpec;
struct CompactedSsTable;
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
class StorageOptionsAccessor;
class CommitLock;
class Session;

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
    explicit Ref(LanceRef* reference);

    Ref(Ref&&) noexcept = default;
    Ref& operator=(Ref&&) noexcept = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;

    static Ref version_number(uint64_t version);

    static Ref version(
        std::optional<std::string> branch,
        std::optional<uint64_t> version_number);

    static Ref tag(const std::string& tag_name);

    const LanceRef* c_handle() const;
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

    FieldReference(FieldReferenceKind kind, int32_t field_id, std::string path);

public:
    static FieldReference by_id(int32_t field_id);

    static FieldReference by_path(std::string path);

    LanceFieldReference c_handle() const;
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

/// Convert the shared C++ write-option value into the C representation.
///
/// `cleanup` must outlive the returned value. Both the legacy write entry
/// points and the owned native options handle use this mapping so their
/// defaults cannot drift.
LanceWriteOptions to_c_write_options(
    const WriteOptions& options,
    LanceAutoCleanupParams& cleanup);

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

enum class ExternalBlobMode : int32_t {
    Reference = LANCE_EXTERNAL_BLOB_REFERENCE,
    Ingest    = LANCE_EXTERNAL_BLOB_INGEST,
};

/// Native current-format file-writer options for a dataset write.
///
/// Dataset file-version selection belongs in `WriteParams::data_storage_version`;
/// the raw-file `version` field is intentionally absent.
struct DatasetFileWriterOptions {
    std::optional<uint64_t> data_cache_bytes;
    std::optional<uint64_t> max_page_bytes;
    std::optional<bool>     keep_original_array;
};

/// One per-base native `ObjectStoreParams` value.
///
/// `base_path` is the exact key used by native `WriteParams::base_store_params`.
/// The accessor is borrowed only during `NativeWriteOptions` construction.
struct DatasetBaseStoreParams {
    std::string                    base_path;
    const StorageOptionsAccessor*  storage_options_accessor = nullptr;
    std::optional<uint64_t>        block_size;
    std::optional<CommitDuration>  s3_credentials_refresh_offset;
    std::optional<bool>            use_constant_size_upload_parts;
    std::optional<bool>            list_is_lexically_ordered;
};

/// Cumulative native write statistics.
struct NativeWriteStats {
    uint64_t bytes_written = 0;
    uint64_t rows_written = 0;
    uint32_t files_written = 0;
};

/// Native fragment progress payload.
struct FragmentWriteProgressInfo {
    uint64_t                    fragment_id = 0;
    std::optional<uint64_t>     physical_rows;
    uint64_t                    file_count = 0;
};

/// C++ progress callbacks for a dataset write.
///
/// Fragment callbacks may throw `lance::Error` or another exception; the
/// wrapper converts it to a native callback error. The stats callback has no
/// native error channel, so exceptions from it are swallowed to keep the C ABI
/// boundary sound. Callbacks may run concurrently from native worker threads.
struct NativeWriteProgress {
    using FragmentCallback =
        std::function<void(const FragmentWriteProgressInfo&)>;
    using StatsCallback = std::function<void(const NativeWriteStats&)>;

    FragmentCallback begin_fragment;
    FragmentCallback complete_fragment;
    StatsCallback    write_stats;
};

/// Configuration for an owned `NativeWriteOptions` handle.
struct NativeWriteOptionsSpec {
    WriteOptions write;
    std::optional<std::vector<std::pair<std::string, std::string>>>
        transaction_properties;
    std::optional<bool> enable_v2_manifest_paths;
    std::optional<std::vector<BasePath>> initial_bases;
    std::optional<std::vector<uint32_t>> target_bases;
    std::optional<std::vector<std::string>> target_base_names_or_paths;
    std::optional<bool> target_all_bases;
    std::optional<std::vector<DatasetBaseStoreParams>> base_store_params;
    bool allow_external_blob_outside_bases = false;
    ExternalBlobMode external_blob_mode = ExternalBlobMode::Reference;
    std::optional<uint64_t> blob_pack_file_size_threshold;
    std::optional<DatasetFileWriterOptions> file_writer_options;
    const Session* session = nullptr;
    const StorageOptionsAccessor* storage_options_accessor = nullptr;
    const CommitLock* commit_lock = nullptr;
    NativeWriteProgress progress;
};

/// Owned native-backed dataset write options. Move-only.
///
/// The handle copies or clones every value from `NativeWriteOptionsSpec`.
/// Source Session, accessor and commit-lock wrappers may be destroyed after
/// construction. Destroy this object only after all writes that borrowed it
/// have returned; callback captures are released from the destructor.
class NativeWriteOptions {
    Handle<LanceNativeWriteOptions, lance_native_write_options_close> handle_;

public:
    explicit NativeWriteOptions(const NativeWriteOptionsSpec& spec);

    NativeWriteOptions(NativeWriteOptions&&) noexcept = default;
    NativeWriteOptions& operator=(NativeWriteOptions&&) noexcept = default;
    NativeWriteOptions(const NativeWriteOptions&) = delete;
    NativeWriteOptions& operator=(const NativeWriteOptions&) = delete;

    const LanceNativeWriteOptions* c_handle() const;
};

/// Options for native fragment creation.
///
/// `schema == nullptr` leaves native schema inference in place; in Append mode
/// native inference may reuse the existing dataset's schema and field IDs. A
/// non-null schema is borrowed for the call and cloned by the C API before the
/// asynchronous write starts. `write` maps through the same native option
/// conversion as `Dataset::write_with_options`, so a default-constructed value
/// selects native `WriteParams::default()`.
struct FragmentCreateOptions {
    const LanceSchema* schema = nullptr;
    WriteMode          mode = WriteMode::Create;
    WriteOptions       write;
};

// ─── Native transactions ─────────────────────────────────────────────────────

/// Owned native `DataFile` metadata snapshot. Move-only.
class DataFile {
    Handle<LanceDataFile, lance_data_file_close> handle_;

public:
    explicit DataFile(LanceDataFile* file);

    DataFile(DataFile&&) noexcept = default;
    DataFile& operator=(DataFile&&) noexcept = default;
    DataFile(const DataFile&) = delete;
    DataFile& operator=(const DataFile&) = delete;

    /// Native data-file path relative to its selected data directory.
    std::string path() const;

    /// Native field IDs in stored order.
    std::vector<int32_t> fields() const;

    /// Native column indices in stored order, including the `-1` marker.
    std::vector<int32_t> column_indices() const;

    /// Native major/minor data-file format version.
    std::pair<uint32_t, uint32_t> file_version() const;

    /// Native optional file size in bytes.
    std::optional<uint64_t> file_size() const;

    /// Native optional base path ID.
    std::optional<uint32_t> base_id() const;

    /// Clone this snapshot into an independently owned handle.
    DataFile clone() const;

    const LanceDataFile* c_handle() const;
};

/// Owned native `DataReplacementGroup` snapshot. Move-only.
class DataReplacementGroup {
    Handle<LanceDataReplacementGroup, lance_data_replacement_group_close>
        handle_;

public:
    explicit DataReplacementGroup(LanceDataReplacementGroup* group);

    /// Construct a group by cloning `file` into native metadata.
    DataReplacementGroup(uint64_t fragment_id, const DataFile& file);

    DataReplacementGroup(DataReplacementGroup&&) noexcept = default;
    DataReplacementGroup& operator=(DataReplacementGroup&&) noexcept = default;
    DataReplacementGroup(const DataReplacementGroup&) = delete;
    DataReplacementGroup& operator=(const DataReplacementGroup&) = delete;

    uint64_t fragment_id() const;

    DataFile file() const;

    /// Clone this snapshot into an independently owned handle.
    DataReplacementGroup clone() const;

    const LanceDataReplacementGroup* c_handle() const;
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
    explicit UpdateMap(LanceUpdateMap* map);

    /// Construct from parallel key/value lists. Every key is required; a
    /// `std::nullopt` value is a native `None` entry. Duplicate keys and input
    /// order are preserved.
    UpdateMap(const std::vector<std::string>& keys,
              const std::vector<std::optional<std::string>>& values,
              bool replace);

    UpdateMap(UpdateMap&&) noexcept = default;
    UpdateMap& operator=(UpdateMap&&) noexcept = default;
    UpdateMap(const UpdateMap&) = delete;
    UpdateMap& operator=(const UpdateMap&) = delete;

    size_t count() const;

    std::string key_at(size_t index) const;

    std::optional<std::string> value_at(size_t index) const;

    bool replace() const;

    /// Clone this snapshot into an independently owned handle.
    UpdateMap clone() const;

    const LanceUpdateMap* c_handle() const;
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
    explicit DataOverlayFile(LanceDataOverlayFile* file);

    DataOverlayFile(const DataFile& data_file,
                    OverlayCoverageKind coverage_kind,
                    const std::vector<std::vector<uint8_t>>& bitmaps,
                    uint64_t committed_version = 0);

    DataOverlayFile(DataOverlayFile&&) noexcept = default;
    DataOverlayFile& operator=(DataOverlayFile&&) noexcept = default;
    DataOverlayFile(const DataOverlayFile&) = delete;
    DataOverlayFile& operator=(const DataOverlayFile&) = delete;

    /// Complete native data-file metadata owned by the overlay.
    DataFile data_file() const;

    OverlayCoverageKind coverage_kind() const;

    /// Number of coverage bitmaps: 1 for shared, field count for per-field.
    uint64_t bitmap_count() const;

    /// Copy one portable Roaring coverage bitmap into owned bytes.
    std::vector<uint8_t> bitmap_at(size_t index) const;

    /// Native committed-version stamp. Zero is the pre-commit placeholder.
    uint64_t committed_version() const;

    /// Clone this snapshot into an independently owned handle.
    DataOverlayFile clone() const;

    const LanceDataOverlayFile* c_handle() const;
};

/// Owned native `DataOverlayGroup` snapshot. Move-only.
class DataOverlayGroup {
    Handle<LanceDataOverlayGroup, lance_data_overlay_group_close> handle_;

public:
    explicit DataOverlayGroup(LanceDataOverlayGroup* group);

    /// Construct a group by cloning the given overlay values in order.
    DataOverlayGroup(uint64_t fragment_id,
                     const std::vector<const DataOverlayFile*>& overlays);

    DataOverlayGroup(DataOverlayGroup&&) noexcept = default;
    DataOverlayGroup& operator=(DataOverlayGroup&&) noexcept = default;
    DataOverlayGroup(const DataOverlayGroup&) = delete;
    DataOverlayGroup& operator=(const DataOverlayGroup&) = delete;

    uint64_t fragment_id() const;

    uint64_t overlay_count() const;

    /// Clone one overlay into an independent owned value.
    DataOverlayFile overlay_at(size_t index) const;

    /// Clone this snapshot into an independently owned handle.
    DataOverlayGroup clone() const;

    const LanceDataOverlayGroup* c_handle() const;
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

// ─── Native update and rewrite values ───────────────────────────────────────

/// Native update mode of an Update operation.
enum class UpdateMode : int32_t {
    RewriteRows    = LANCE_UPDATE_MODE_REWRITE_ROWS,
    RewriteColumns = LANCE_UPDATE_MODE_REWRITE_COLUMNS,
};

/// One index file recorded by a rewritten index.
struct IndexFile {
    std::string path;
    uint64_t size_bytes = 0;
};

/// Bloom-filter parameters of a key-existence filter.
struct BloomFilterValues {
    std::vector<uint8_t> bitmap;
    uint32_t num_bits = 0;
    uint64_t number_of_items = 0;
    double probability = 0.0;
};

/// Owned native `UpdatedFragmentOffsets` value. Move-only.
///
/// Native absent, present-but-empty, and populated values are distinct and are
/// preserved here. Offsets are treated as sets, and a repeated fragment ID in
/// the constructor input keeps the last entry, matching native `HashMap`
/// representation.
class UpdatedFragmentOffsets {
    Handle<LanceUpdatedFragmentOffsets,
           lance_updated_fragment_offsets_close>
        handle_;

public:
    explicit UpdatedFragmentOffsets(LanceUpdatedFragmentOffsets* offsets);

    /// Build from (fragment ID, offsets) entries in input order.
    explicit UpdatedFragmentOffsets(
        const std::vector<std::pair<uint64_t, std::vector<uint32_t>>>& entries);

    UpdatedFragmentOffsets(UpdatedFragmentOffsets&&) noexcept = default;
    UpdatedFragmentOffsets& operator=(UpdatedFragmentOffsets&&) noexcept =
        default;
    UpdatedFragmentOffsets(const UpdatedFragmentOffsets&) = delete;
    UpdatedFragmentOffsets& operator=(const UpdatedFragmentOffsets&) = delete;

    /// Number of fragment entries. Entry order is unspecified.
    size_t size() const;

    uint64_t fragment_id_at(size_t index) const;

    size_t offset_count_at(size_t index) const;

    /// Offsets of one entry in ascending native bitmap order, copied in one pass.
    std::vector<uint32_t> offsets_at(size_t index) const;

    /// Clone into independently owned native storage.
    UpdatedFragmentOffsets clone() const;

    const LanceUpdatedFragmentOffsets* c_handle() const;
};

/// Owned native key-existence filter. Move-only.
///
/// Values built from explicit protobuf bytes follow the native `TryFrom`
/// defaults: an absent bloom `number_of_items` becomes 8192, an absent bloom
/// `probability` becomes 0.00057, and an absent filter body becomes an empty
/// exact set.
class KeyExistenceFilter {
    Handle<LanceKeyExistenceFilter, lance_key_existence_filter_close> handle_;

public:
    explicit KeyExistenceFilter(LanceKeyExistenceFilter* filter);

    KeyExistenceFilter(KeyExistenceFilter&&) noexcept = default;
    KeyExistenceFilter& operator=(KeyExistenceFilter&&) noexcept = default;
    KeyExistenceFilter(const KeyExistenceFilter&) = delete;
    KeyExistenceFilter& operator=(const KeyExistenceFilter&) = delete;

    /// Decode one encoded native key-existence-filter message.
    static KeyExistenceFilter from_proto(const std::vector<uint8_t>& bytes);

    LanceKeyExistenceFilterKind kind() const;

    std::vector<int32_t> field_ids() const;

    /// Keys of an exact-set filter in ascending order. Throws
    /// LANCE_ERR_NOT_SUPPORTED for a bloom filter.
    std::vector<uint64_t> exact_keys() const;

    /// Bloom parameters. Throws LANCE_ERR_NOT_SUPPORTED for an exact set.
    BloomFilterValues bloom() const;

    /// Encode with the native protobuf conversion used by persisted Update
    /// operations.
    std::vector<uint8_t> to_proto() const;

    /// Clone into independently owned native storage.
    KeyExistenceFilter clone() const;

    const LanceKeyExistenceFilter* c_handle() const;
};

/// Owned native `RewriteGroup`. Move-only.
///
/// Native equality treats both fragment lists as unordered; this wrapper keeps
/// the stored order unchanged.
class RewriteGroup {
    Handle<LanceRewriteGroup, lance_rewrite_group_close> handle_;

public:
    explicit RewriteGroup(LanceRewriteGroup* group);

    /// Build from borrowed fragment handles. Complete native fragment metadata
    /// is copied.
    RewriteGroup(const std::vector<const Fragment*>& old_fragments,
                 const std::vector<const Fragment*>& new_fragments);

    RewriteGroup(RewriteGroup&&) noexcept = default;
    RewriteGroup& operator=(RewriteGroup&&) noexcept = default;
    RewriteGroup(const RewriteGroup&) = delete;
    RewriteGroup& operator=(const RewriteGroup&) = delete;

    size_t old_fragment_count() const;
    Fragment old_fragment_at(size_t index) const;
    std::vector<Fragment> old_fragments() const;
    size_t new_fragment_count() const;
    Fragment new_fragment_at(size_t index) const;
    std::vector<Fragment> new_fragments() const;

    /// Clone into independently owned native storage.
    RewriteGroup clone() const;

    const LanceRewriteGroup* c_handle() const;
};

/// Owned native `RewrittenIndex`. Move-only.
class RewrittenIndex {
    Handle<LanceRewrittenIndex, lance_rewritten_index_close> handle_;

public:
    explicit RewrittenIndex(LanceRewrittenIndex* index);

    /// Build one rewritten-index record. `index_files == std::nullopt` maps to
    /// native None, while an engaged empty vector is the distinct
    /// `Some(empty)` state older writers persisted.
    RewrittenIndex(const std::array<uint8_t, 16>& old_id,
                   const std::array<uint8_t, 16>& new_id,
                   std::string_view details_type_url,
                   const std::vector<uint8_t>& details_value,
                   uint32_t index_version,
                   std::optional<std::vector<IndexFile>> index_files);

    RewrittenIndex(RewrittenIndex&&) noexcept = default;
    RewrittenIndex& operator=(RewrittenIndex&&) noexcept = default;
    RewrittenIndex(const RewrittenIndex&) = delete;
    RewrittenIndex& operator=(const RewrittenIndex&) = delete;

    std::array<uint8_t, 16> old_id() const;

    std::array<uint8_t, 16> new_id() const;

    std::string details_type_url() const;

    std::vector<uint8_t> details_value() const;

    uint32_t version() const;

    bool has_files() const;

    /// Index files. Returns an empty vector both for native None and
    /// Some(empty); use has_files() to tell them apart.
    std::vector<IndexFile> files() const;

    /// Clone into independently owned native storage.
    RewrittenIndex clone() const;

    const LanceRewrittenIndex* c_handle() const;
};

/// Owned native Operation snapshot. Move-only; closing the source Transaction
/// does not invalidate this handle.
class Operation {
    Handle<LanceOperation, lance_operation_close> handle_;

public:
    explicit Operation(LanceOperation* operation);

    Operation(Operation&&) noexcept = default;
    Operation& operator=(Operation&&) noexcept = default;
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;

    OperationKind kind() const;

    /// Fragment count for Append/Overwrite. Other native variants throw
    /// LANCE_ERR_NOT_SUPPORTED until their typed wrappers are added.
    uint64_t fragment_count() const;

    /// Native `preserves_nullability` assertion from a Merge or Project
    /// operation. Other kinds throw LANCE_ERR_NOT_SUPPORTED.
    bool preserves_nullability() const;

    /// Number of fragments in an Append operation.
    uint64_t append_fragment_count() const;

    /// Clone one Append fragment as an independent detached fragment.
    Fragment append_fragment_at(size_t index) const;

    /// Target version of a Restore operation.
    uint64_t restore_version() const;

    /// Reserved fragment count of a ReserveFragments operation.
    uint32_t reserve_fragments_count() const;

    /// Number of new base paths in an UpdateBases operation.
    uint64_t update_bases_count() const;

    /// Borrowed new base path from an UpdateBases operation. The strings
    /// remain valid until this operation handle is closed.
    LanceBasePathView update_bases_at(size_t index) const;

    /// New base paths cloned from an UpdateBases operation.
    std::vector<BasePath> update_bases() const;

    /// Number of fragments in an Overwrite operation.
    uint64_t overwrite_fragment_count() const;

    /// Clone one Overwrite fragment as an independent detached fragment.
    Fragment overwrite_fragment_at(size_t index) const;

    /// Clone the in-memory schema of an Overwrite operation.
    LanceSchema overwrite_schema() const;

    /// Whether the Overwrite operation carries native `Some` config upserts.
    /// `Some(empty)` is distinct from `None`.
    bool has_config_upsert_values() const;

    /// Number of config upsert entries. Native `None` reports 0.
    uint64_t config_upsert_count() const;

    /// Borrowed config upsert key by native iteration position.
    std::string config_upsert_key_at(size_t index) const;

    /// Borrowed config upsert value by native iteration position.
    std::string config_upsert_value_at(size_t index) const;

    /// Whether the Overwrite operation carries native `Some` initial bases.
    /// `Some(empty)` is distinct from `None`.
    bool has_initial_bases() const;

    /// Number of initial base paths. Native `None` reports 0.
    uint64_t initial_base_count() const;

    /// Borrowed initial base path from an Overwrite operation. The strings
    /// remain valid until this operation handle is closed.
    LanceBasePathView initial_base_at(size_t index) const;

    /// Initial base paths cloned from an Overwrite operation.
    std::vector<BasePath> initial_bases() const;

    /// Number of fragments in a Merge operation.
    uint64_t merge_fragment_count() const;

    /// Clone one Merge fragment as an independent detached fragment.
    Fragment merge_fragment_at(size_t index) const;

    /// Clone the in-memory schema of a Merge operation.
    LanceSchema merge_schema() const;

    /// Native `preserves_nullability` assertion from a Merge operation.
    bool merge_preserves_nullability() const;

    /// Clone the in-memory schema of a Project operation.
    LanceSchema project_schema() const;

    /// Shallow-clone flag of a Clone operation.
    bool clone_is_shallow() const;

    /// Optional native reference name of a Clone operation.
    std::optional<std::string> clone_ref_name() const;

    /// Native reference version of a Clone operation.
    uint64_t clone_ref_version() const;

    /// Borrowed native reference path of a Clone operation.
    std::string clone_ref_path() const;

    /// Optional native branch name of a Clone operation.
    std::optional<std::string> clone_branch_name() const;

    /// Optional native config updates map from an UpdateConfig operation.
    std::optional<UpdateMap> config_updates() const;

    /// Optional native table metadata updates map.
    std::optional<UpdateMap> table_metadata_updates() const;

    /// Optional native schema metadata updates map.
    std::optional<UpdateMap> schema_metadata_updates() const;

    /// Per-field native metadata update maps keyed by field ID.
    std::map<int32_t, UpdateMap> field_metadata_updates() const;

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
    static Operation make_restore(uint64_t version);

    /// Construct an owned native ReserveFragments operation.
    static Operation make_reserve_fragments(uint32_t num_fragments);

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
    uint64_t data_replacement_count() const;

    /// Clone one data replacement group.
    DataReplacementGroup data_replacement_at(size_t index) const;

    /// Number of data overlay groups.
    uint64_t data_overlay_group_count() const;

    /// Clone one data overlay group.
    DataOverlayGroup data_overlay_group_at(size_t index) const;

    /// Number of updated fragments in a Delete operation.
    uint64_t delete_updated_fragment_count() const;

    /// Native `Fragment` metadata JSON for one updated Delete fragment.
    std::string delete_updated_fragment_metadata_json(size_t index) const;

    /// Number of fully deleted fragment IDs in a Delete operation.
    uint64_t delete_deleted_fragment_count() const;

    /// One fully deleted fragment ID from a Delete operation.
    uint64_t delete_deleted_fragment_id(size_t index) const;

    /// Delete predicate captured by the native operation.
    std::string delete_predicate() const;

    /// Construct an owned native Update operation. Fragment handles are
    /// borrowed and their complete native metadata is copied. Native commit
    /// validation owns schema coverage, conflict detection and row-version
    /// behavior; no policy is applied here.
    static Operation make_update(const UpdateOperationSpec& spec);

    /// Construct an owned native Rewrite operation. Groups and rewritten
    /// indices are cloned from their handles. `frag_reuse_index` may be null
    /// for native None. It is in-memory only, so an explicit protobuf round
    /// trip drops it.
    static Operation make_rewrite(
        const std::vector<const RewriteGroup*>& groups,
        const std::vector<const RewrittenIndex*>& rewritten_indices,
        const IndexSegmentMetadata* frag_reuse_index = nullptr);

    /// Construct an owned native UpdateMemWalState operation from flushed
    /// generation markers. Native merge-insert validation owns shard
    /// existence, generation ordering and atomicity.
    static Operation make_update_mem_wal_state(
        const std::vector<CompactedSsTable>& compacted_sstables);

    /// Construct an UpdateMemWalState operation for a specific MemWAL
    /// directory. Pass an empty `dir_id` for a legacy MemWAL. Native commit
    /// rejects a stale or removed identifier.
    static Operation make_update_mem_wal_state_with_dir_id(
        const std::vector<CompactedSsTable>& compacted_sstables,
        const std::string& dir_id);

    // Update inspection. Inspecting a different operation kind throws
    // LANCE_ERR_NOT_SUPPORTED; every getter below reports native fields
    // unchanged.

    /// Number of removed fragment IDs in an Update operation.
    uint64_t removed_fragment_count() const;

    /// One removed fragment ID by position.
    uint64_t removed_fragment_id(size_t index) const;

    /// Number of updated fragments in an Update operation.
    uint64_t updated_fragment_count() const;

    /// Clone one updated fragment as an independent snapshot.
    Fragment updated_fragment_at(size_t index) const;

    /// Number of new fragments in an Update operation.
    uint64_t new_fragment_count() const;

    /// Clone one new fragment as an independent snapshot.
    Fragment new_fragment_at(size_t index) const;

    /// Number of modified field IDs in an Update operation.
    uint64_t fields_modified_count() const;

    /// One modified field ID by position.
    uint32_t fields_modified(size_t index) const;

    /// Number of compacted SSTable entries in an Update operation.
    uint64_t compacted_sstable_count() const;

    /// One compacted SSTable entry by position.
    CompactedSsTable compacted_sstable_at(size_t index) const;

    /// Number of preserved-fragment-bitmap field IDs in an Update operation.
    uint64_t fields_for_preserving_frag_bitmap_count() const;

    /// One preserved-fragment-bitmap field ID by position.
    uint32_t fields_for_preserving_frag_bitmap(size_t index) const;

    /// Native update mode, distinguishing absent from both variants.
    std::optional<UpdateMode> update_mode() const;

    /// Cloned inserted-rows filter, or nullopt for native None.
    std::optional<KeyExistenceFilter> inserted_rows_filter() const;

    /// Cloned updated-fragment offsets, or nullopt for native None. A present
    /// but empty value is preserved.
    std::optional<UpdatedFragmentOffsets> updated_fragment_offsets() const;

    /// MemWAL directory identifier named by an Update operation, or nullopt
    /// for native None. An engaged empty string denotes a legacy MemWAL.
    std::optional<std::string> update_mem_wal_dir_id() const;

    /// Number of groups in a Rewrite operation.
    uint64_t rewrite_group_count() const;

    /// Clone one Rewrite group by position.
    RewriteGroup rewrite_group_at(size_t index) const;

    /// Number of rewritten indices in a Rewrite operation.
    uint64_t rewritten_index_count() const;

    /// Clone one rewritten index by position.
    RewrittenIndex rewritten_index_at(size_t index) const;

    /// Cloned in-memory fragment-reuse index, or nullopt for native None.
    std::optional<IndexSegmentMetadata> frag_reuse_index() const;

    // UpdateMemWalState inspection. Inspecting a different operation kind
    // throws LANCE_ERR_NOT_SUPPORTED.

    /// Number of flushed generation markers in an UpdateMemWalState operation.
    uint64_t update_mem_wal_state_compacted_sstable_count() const;

    /// Copy one flushed generation marker by position.
    CompactedSsTable update_mem_wal_state_compacted_sstable_at(
        size_t index) const;

    /// MemWAL directory identifier named by an UpdateMemWalState operation.
    /// An empty string denotes the legacy `_mem_wal` directory.
    std::string update_mem_wal_state_dir_id() const;

    const LanceOperation* c_handle() const;
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
    explicit Transaction(LanceTransaction* transaction);

    Transaction(Transaction&&) noexcept = default;
    Transaction& operator=(Transaction&&) noexcept = default;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    uint64_t read_version() const;

    std::string uuid() const;

    std::optional<std::string> tag() const;

    bool has_properties() const;

    std::map<std::string, std::string> properties() const;

    Operation operation() const;

    /// Construct a native transaction from a copied operation.
    static Transaction from_operation(
        uint64_t read_version,
        const Operation& operation,
        const TransactionOptions& options = {});

    /// Serialize with the selected core's native protobuf conversion.
    std::vector<uint8_t> to_proto() const;

    /// Parse one complete native protobuf Transaction.
    static Transaction from_proto(const std::vector<uint8_t>& bytes);

    const LanceTransaction* c_handle() const;
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
/// unset until native progress has a known total; the native listing races the
/// walk, so it may stay unset for some or all updates. The callback must not
/// throw; it runs on a native background task.
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

/// Schema-aware checkpoint lookup. A hit must return both an owned array and
/// the exact schema received by `insert_batch`, because native binds a cached
/// batch to a context marker stored in that schema's metadata. Returns true on
/// a hit, false on a miss, and throws on error.
using BatchUDFGetBatchV2 = std::function<bool(
    const BatchUDFInfo&,
    ArrowArray&,
    ArrowSchema&)>;

/// Schema-aware native UDF checkpoint store. Insert and fragment callbacks are
/// shared with `UDFCheckpointStore`; only the batch lookup differs. All four
/// callbacks are required when the store is engaged.
struct UDFCheckpointStoreV2 {
    BatchUDFGetBatchV2 get_batch;

    std::function<void(
        const BatchUDFInfo&,
        ArrowArray&,
        ArrowSchema&)> insert_batch;

    std::function<std::optional<std::string>(uint32_t)> get_fragment;

    std::function<void(uint64_t, const std::string&)> insert_fragment;
};

/// Schema-aware native BatchUDF declaration.
struct BatchUDFV2 {
    BatchUDFMapper mapper;
    const ArrowSchema* output_schema = nullptr;
    std::optional<UDFCheckpointStoreV2> checkpoint_store;
};

namespace callback_detail {

void release_owned_string(LanceOwnedString* value) noexcept;

void set_callback_error(
    LanceCallbackError* target,
    int32_t code,
    const char* message) noexcept;

void release_array_after_exception(ArrowArray* value) noexcept;

void release_schema_after_exception(ArrowSchema* value) noexcept;

struct MapperContext {
    const BatchUDF* udf;
};

struct MapperContextV2 {
    const BatchUDFV2* udf;
};

struct CheckpointContext {
    const UDFCheckpointStore* store;
};

struct CheckpointContextV2 {
    const UDFCheckpointStoreV2* store;
};

template <typename Context>
inline int32_t mapper_trampoline(
    void* callback_ctx,
    const ArrowArray* input,
    const ArrowSchema* input_schema,
    ArrowArray* output,
    LanceCallbackError* error) noexcept {
    try {
        auto* context = static_cast<Context*>(callback_ctx);
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

int32_t get_batch_trampoline(
    void* callback_ctx,
    const LanceUDFBatchInfo* raw_info,
    ArrowArray* output,
    LanceCallbackError* error) noexcept;

int32_t get_batch_v2_trampoline(
    void* callback_ctx,
    const LanceUDFBatchInfo* raw_info,
    ArrowArray* output,
    ArrowSchema* output_schema,
    LanceCallbackError* error) noexcept;

template <typename Context>
inline int32_t insert_batch_trampoline(
    void* callback_ctx,
    const LanceUDFBatchInfo* raw_info,
    ArrowArray* batch,
    ArrowSchema* batch_schema,
    LanceCallbackError* error) noexcept {
    try {
        auto* context = static_cast<Context*>(callback_ctx);
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

template <typename Context>
inline int32_t get_fragment_trampoline(
    void* callback_ctx,
    uint32_t fragment_id,
    LanceOwnedString* out_json,
    LanceCallbackError* error) noexcept {
    try {
        auto* context = static_cast<Context*>(callback_ctx);
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

template <typename Context>
inline int32_t insert_fragment_trampoline(
    void* callback_ctx,
    uint64_t fragment_id,
    const char* fragment_json,
    LanceCallbackError* error) noexcept {
    try {
        auto* context = static_cast<Context*>(callback_ctx);
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

/// Complete call-scoped state for one native BatchUDF invocation.
///
/// The raw callback table points only into this object. Keep it on the stack
/// for the duration of the synchronous add-columns call; it is neither movable
/// nor copyable.
struct BatchUDFCall {
    std::optional<std::vector<std::string>> read_columns;
    std::vector<const char*> read_column_ptrs;
    uint32_t batch_size_value = 0;
    const char* const* read_columns_ptr = nullptr;
    const uint32_t* batch_size_ptr = nullptr;
    MapperContext mapper_context{};
    CheckpointContext checkpoint_context{};
    LanceUDFCheckpointStore raw_store{};
    LanceBatchUDF raw{};
    MapperContextV2 mapper_context_v2{};
    CheckpointContextV2 checkpoint_context_v2{};
    LanceUDFCheckpointStoreV2 raw_store_v2{};
    LanceBatchUDFV2 raw_v2{};

    BatchUDFCall(const BatchUDF& udf,
                 std::optional<std::vector<std::string>> columns,
                 std::optional<uint32_t> batch_size);

    BatchUDFCall(const BatchUDFV2& udf,
                 std::optional<std::vector<std::string>> columns,
                 std::optional<uint32_t> batch_size);

    BatchUDFCall(const BatchUDFCall&) = delete;
    BatchUDFCall& operator=(const BatchUDFCall&) = delete;
    BatchUDFCall(BatchUDFCall&&) = delete;
    BatchUDFCall& operator=(BatchUDFCall&&) = delete;

private:
    void prepare_arguments(std::optional<uint32_t> batch_size);
};

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

    const LanceCommitLock* c_handle() const;
};

namespace commit_lock_detail {

struct State {
    CommitLock::Acquire acquire;
};

struct LeaseHolder {
    std::unique_ptr<CommitLease> lease;
};

extern "C" int32_t acquire_trampoline(
    void* callback_ctx,
    uint64_t version,
    void** out_lease,
    LanceCallbackError* error) noexcept;

extern "C" int32_t release_trampoline(
    void*,
    void* lease,
    bool success,
    LanceCallbackError* error) noexcept;

extern "C" void destroy_lease_trampoline(void*, void* lease) noexcept;

extern "C" void release_context_trampoline(void* callback_ctx) noexcept;

}  // namespace commit_lock_detail



inline CommitLock::~CommitLock() = default;

inline CommitLock::CommitLock(CommitLock&&) noexcept = default;

inline CommitLock& CommitLock::operator=(CommitLock&&) noexcept = default;

// ─── Storage options ─────────────────────────────────────────────────────────

using StorageOptions = std::map<std::string, std::string>;

class StorageOptionsProvider {
    Handle<LanceStorageOptionsProvider, lance_storage_options_provider_close> handle_;

    explicit StorageOptionsProvider(LanceStorageOptionsProvider* provider);

public:
    using Fetch = std::function<std::optional<StorageOptions>()>;

    StorageOptionsProvider(
        std::string provider_id,
        Fetch fetch,
        Fetch force_fetch = {});

    /// Create a provider backed by a native namespace. The provider retains
    /// the namespace, so it may outlive this C++ handle. `table_id` is the
    /// native table path; every component is borrowed only for this call.
    static StorageOptionsProvider from_namespace(
        const Namespace& ns,
        const std::vector<std::string>& table_id);

    const LanceStorageOptionsProvider* c_handle() const;

    std::string provider_id() const;
};

namespace storage_options_detail {

struct FetchSnapshot {
    std::vector<std::string> strings;
    std::vector<const char*> keys;
    std::vector<const char*> values;
};

void release_fetch_snapshot(void* raw) noexcept;

int32_t fetch_impl(
    const StorageOptionsProvider::Fetch& fetch,
    LanceStorageOptionsFetchResult* out,
    LanceCallbackError* error,
    const char* context) noexcept;

struct ProviderContext {
    StorageOptionsProvider::Fetch fetch;
    StorageOptionsProvider::Fetch force_fetch;
};

int32_t fetch_callback(
    void* callback_ctx,
    LanceStorageOptionsFetchResult* out,
    LanceCallbackError* error) noexcept;

int32_t force_fetch_callback(
    void* callback_ctx,
    LanceStorageOptionsFetchResult* out,
    LanceCallbackError* error) noexcept;

void provider_release(void* callback_ctx) noexcept;

LanceStorageOptionsProvider* create_provider(
    const std::string& provider_id,
    StorageOptionsProvider::Fetch fetch,
    StorageOptionsProvider::Fetch force_fetch);

}  // namespace storage_options_detail



class StorageOptionsAccessor {
    Handle<LanceStorageOptionsAccessor, lance_storage_options_accessor_close> handle_;

public:
    /// NULL optional means native None; an engaged empty map means Some(empty).
    explicit StorageOptionsAccessor(
        std::optional<StorageOptions> initial = std::nullopt,
        const StorageOptionsProvider* provider = nullptr);

    /// Resolve with native cache and expiration behavior.
    std::optional<StorageOptions> snapshot() const;

    const LanceStorageOptionsAccessor* c_handle() const;
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
    /// Optional per-base native `ObjectStoreParams` bindings for existing
    /// shared bases. `std::nullopt` selects native None (no bindings); an
    /// engaged empty vector selects native `Some(empty)`, which also adds no
    /// bindings. Entries match exact `BasePath.path` keys from the manifest,
    /// not base ids or names, and an exact entry takes precedence over the
    /// dataset-level accessor. The bindings are runtime-only and are not
    /// persisted; reopening the dataset later must supply them again when the
    /// base needs them. The returned dataset retains the accessor Arcs, so the
    /// `DatasetBaseStoreParams` values and accessors may be destroyed after
    /// `open_with_options` returns.
    std::optional<std::vector<DatasetBaseStoreParams>> base_store_params;
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

    static SessionCacheSpec default_cache();

    static SessionCacheSpec with_size(uint64_t capacity_bytes);

    static SessionCacheSpec from_uri(std::string uri);

    static SessionCacheSpec from_config(
        std::string backend_kind,
        std::vector<std::pair<std::string, std::string>> backend_options = {});
};

class Session {
    Handle<LanceSession, lance_session_close> handle_;

    static LanceSessionCacheSpec c_cache_spec(
        const SessionCacheSpec& spec,
        std::vector<const char*>& option_keys,
        std::vector<const char*>& option_values);

    static LanceSession* create(
        const SessionCacheSpec& index_cache,
        const SessionCacheSpec& metadata_cache);

public:
    /// Configure each cache tier independently. The spec values and their
    /// string buffers are consumed during construction.
    Session(
        const SessionCacheSpec& index_cache,
        const SessionCacheSpec& metadata_cache);

    Session(uint64_t index_cache_size_bytes, uint64_t metadata_cache_size_bytes);

    LanceSessionCacheStats cache_stats() const;

    /// Current deep size of the session in bytes. This is not an RSS limit.
    uint64_t size_bytes() const;

    /// Approximate number of items retained in both cache tiers.
    uint64_t approx_num_items() const;

    const LanceSession* c_handle() const;
};

// ─── Owned native dataset-write options ─────────────────────────────────────

namespace native_write_options_detail {

struct ProgressContext {
    NativeWriteProgress progress;
};

int32_t invoke_fragment(
    const NativeWriteProgress::FragmentCallback& callback,
    const LanceWriteFragmentProgressInfo* info,
    LanceCallbackError* error,
    const char* context) noexcept;

extern "C" int32_t begin_fragment_trampoline(
    void* callback_ctx,
    const LanceWriteFragmentProgressInfo* info,
    LanceCallbackError* error) noexcept;

extern "C" int32_t complete_fragment_trampoline(
    void* callback_ctx,
    const LanceWriteFragmentProgressInfo* info,
    LanceCallbackError* error) noexcept;

extern "C" void write_stats_trampoline(
    void* callback_ctx,
    const LanceWriteStats* stats) noexcept;

extern "C" void release_progress_context(void* callback_ctx) noexcept;

/// Owns the strings, C views and list passed to `LanceBaseStoreParamsList`
/// for one C call. Keep it alive until that call returns.
struct BaseStoreParamsHolder {
    std::vector<std::string> paths;
    std::vector<LanceBaseStoreParams> views;
    LanceBaseStoreParamsList list{};
};

/// Convert C++ `DatasetBaseStoreParams` values into borrowed C views.
///
/// Shared by `NativeWriteOptions` and `DatasetOpenOptions`. An absent optional
/// leaves `holder.list` zeroed and means native None; an engaged vector, even
/// empty, fills the list and means native Some (empty or populated).
void build_base_store_params(
    const std::optional<std::vector<DatasetBaseStoreParams>>& base_store_params,
    BaseStoreParamsHolder& holder);

struct BuildState {
    LanceAutoCleanupParams cleanup{};
    LanceWriteOptions write{};

    bool has_transaction_properties = false;
    std::vector<const char*> property_keys;
    std::vector<const char*> property_values;
    LanceTransactionProperties properties{};

    std::vector<std::string> base_paths;
    std::vector<std::optional<std::string>> base_names;
    std::vector<LanceBasePath> base_views;
    LanceBasePath empty_base{};
    LanceBasePathList base_list{};

    std::vector<uint32_t> target_ids;
    LanceUInt32List target_list{};

    std::vector<std::string> target_names;
    std::vector<const char*> target_name_ptrs;
    LanceStringList target_name_list{};

    BaseStoreParamsHolder base_store;

    LanceFileWriterOptions file_options{};

    std::unique_ptr<ProgressContext> progress_ctx;
    LanceNativeWriteProgress progress{};
};

void build_initial_bases(
    const std::optional<std::vector<BasePath>>& initial_bases,
    BuildState& state);

}  // namespace native_write_options_detail



// ─── Process-local FTS query context ────────────────────────────────────────

/// Immutable, query-specific global BM25 scorer plus pinned FTS segment list.
/// This handle is process-local and intentionally has no serialization API.
class FtsQueryContext {
    Handle<LanceFtsQueryContext, lance_fts_query_context_close> handle_;

public:
    explicit FtsQueryContext(LanceFtsQueryContext* context);

    FtsQueryContext(FtsQueryContext&&) noexcept = default;
    FtsQueryContext& operator=(FtsQueryContext&&) noexcept = default;
    FtsQueryContext(const FtsQueryContext&) = delete;
    FtsQueryContext& operator=(const FtsQueryContext&) = delete;

    const LanceFtsQueryContext* c_handle() const;
};

// ─── Typed full-text query ───────────────────────────────────────────────────

/// Immutable full-text query value. Composite factories copy their child
/// values, and Scanner::full_text_search snapshots the tree, so handles may be
/// destroyed before the scan runs.
class FtsQuery {
    Handle<LanceFtsQuery, lance_fts_query_close> handle_;

    explicit FtsQuery(LanceFtsQuery* query);

    static void copy_boosts(
        const std::optional<std::vector<float>>& boosts,
        float& empty_boost,
        const float*& data,
        size_t& count);

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
        const FtsMatchOptions& options = {});

    /// Phrase query. A null `column` lets scan planning resolve the indexed
    /// columns.
    static FtsQuery phrase(
        const std::string& terms,
        std::optional<std::string> column = std::nullopt,
        const FtsPhraseOptions& options = {});

    /// Boolean query. An empty clause list is allowed here; scan planning
    /// rejects a tree without a positive clause.
    static FtsQuery boolean(
        const std::vector<std::pair<FtsOccur, const FtsQuery*>>& clauses);

    /// Boost (boosting) query: the negative child's score is multiplied by
    /// negative_boost and subtracted from the positive child's score.
    static FtsQuery boost(
        const FtsQuery& positive,
        const FtsQuery& negative,
        const FtsBoostOptions& options = {});

    /// MultiMatch query: scores each field independently and keeps the maximum
    /// field score.
    static FtsQuery multi_match(
        const std::string& terms,
        const std::vector<std::string>& columns,
        const FtsMultiMatchOptions& options = {});

    /// CombinedFields (BM25F) query: blends term statistics across all target
    /// columns.
    static FtsQuery combined_fields(
        const std::string& terms,
        const std::vector<std::string>& columns,
        const FtsCombinedFieldsOptions& options = {});

    const LanceFtsQuery* c_handle() const;
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

    explicit FtsTokenList(LanceFtsTokenList* tokens);

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
        std::optional<std::string> params_json = std::nullopt);

    /// Number of tokens. Throws on a NULL handle.
    size_t size() const;

    bool empty() const;

    /// Copy token `index`. Throws on an out-of-range index.
    FtsToken at(size_t index) const;

    /// Copy all tokens in native order.
    std::vector<FtsToken> tokens() const;

    const LanceFtsTokenList* c_handle() const;
};

/// Native process-wide object-store IOPS counter, with no reset semantics.
uint64_t process_iops_counter();

/// Native process-wide object-store bytes-read counter, with no reset
/// semantics.
uint64_t process_bytes_read_counter();

/// One read of both native process counters.
struct ProcessIoCounters {
    uint64_t iops = 0;
    uint64_t bytes_read = 0;
};

ProcessIoCounters process_io_counters();

/// Native SIMD report as the exact JSON object produced by
/// `lance_simd_info_json()`: `tier`, `target_arch`, and `host_features`.
std::string simd_info_json();

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
void blob_field(
    const std::string& name,
    bool nullable,
    ArrowSchema* out);

/// Export the native blob v2 field with explicit storage thresholds.
///
/// `out` is caller-owned and must be released exactly once. It is written only
/// on success.
void blob_field_with_options(
    const std::string& name,
    bool nullable,
    const BlobFieldOptions& options,
    ArrowSchema* out);

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
    explicit BlobDescriptorArrayBuilder(const std::string& column);

    void append_inline(const uint8_t* data, size_t len);

    void append_inline(const std::vector<uint8_t>& data);

    void append_null();

    void append_packed(uint32_t blob_id, uint64_t offset, uint64_t length);

    void append_packed(uint32_t blob_id, const BlobRange& range);

    void extend_packed(uint32_t blob_id,
                       const std::vector<BlobRange>& ranges);

    void append_dedicated(uint32_t blob_id, uint64_t length);

    void append_managed(
        const std::string& uri,
        uint64_t offset,
        uint64_t length,
        std::optional<uint32_t> base_id = std::nullopt);

    void append_external(uint32_t base_id,
                         const std::string& uri,
                         uint64_t offset,
                         uint64_t length);

    void push_external(const std::string& uri,
                       std::optional<BlobRange> range = std::nullopt);

    /// Export the prepared field. `out` is caller-owned and must be released.
    void field(ArrowSchema* out) const;

    /// Consume the native builder and export its prepared array and field.
    /// Both outputs are caller-owned and must each be released exactly once.
    void finish(ArrowArray* out_array, ArrowSchema* out_schema);
};

/// Move-only owner of a native packed blob writer.
///
/// The destructor frees the C allocation without finishing. `finish_array()`
/// consumes the native writer but leaves this object allocation valid, so
/// later calls throw `LANCE_ERR_INVALID_ARGUMENT`.
class PackedBlobWriter {
    Handle<LancePackedBlobWriter, lance_packed_blob_writer_close> handle_;

public:
    explicit PackedBlobWriter(LancePackedBlobWriter* writer);

    PackedBlobWriter(PackedBlobWriter&&) noexcept = default;
    PackedBlobWriter& operator=(PackedBlobWriter&&) noexcept = default;
    PackedBlobWriter(const PackedBlobWriter&) = delete;
    PackedBlobWriter& operator=(const PackedBlobWriter&) = delete;

    /// Native blob ID. Throws when the writer is NULL or finished.
    uint32_t blob_id() const;

    /// Native sidecar path. Throws when the writer is NULL or finished.
    std::string path() const;

    /// Append one payload. A null pointer with zero length is valid.
    void write_blob(const uint8_t* data, size_t len);

    void write_blob(const std::vector<uint8_t>& data);

    /// Move one binary ArrowArray into the writer; `schema` is borrowed.
    void write_blobs(ArrowArray* payloads, const ArrowSchema* schema);

    /// Consume the native writer and export caller-owned prepared parts.
    void finish_array(
        const std::string& field_name,
        ArrowArray* out_array,
        ArrowSchema* out_schema);
};

/// Move-only owner of a native dedicated blob writer.
///
/// Repeated writes concatenate into one sidecar. The destructor frees the C
/// allocation without finishing.
class DedicatedBlobWriter {
    Handle<LanceDedicatedBlobWriter, lance_dedicated_blob_writer_close> handle_;

public:
    explicit DedicatedBlobWriter(LanceDedicatedBlobWriter* writer);

    DedicatedBlobWriter(DedicatedBlobWriter&&) noexcept = default;
    DedicatedBlobWriter& operator=(DedicatedBlobWriter&&) noexcept = default;
    DedicatedBlobWriter(const DedicatedBlobWriter&) = delete;
    DedicatedBlobWriter& operator=(const DedicatedBlobWriter&) = delete;

    /// Native blob ID. Throws when the writer is NULL or finished.
    uint32_t blob_id() const;

    /// Native sidecar path. Throws when the writer is NULL or finished.
    std::string path() const;

    /// Append bytes. Repeated calls concatenate.
    void write(const uint8_t* data, size_t len);

    void write(const std::vector<uint8_t>& data);

    /// Consume the native writer and export caller-owned prepared parts.
    void finish_array(
        const std::string& field_name,
        ArrowArray* out_array,
        ArrowSchema* out_schema);
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
    explicit BlobFile(LanceBlobFile* blob);

    /// Size of the blob in bytes. Independent of the cursor.
    uint64_t size() const;

    /// Read from the cursor to the end (the whole blob when the cursor is 0).
    std::vector<uint8_t> read();

    /// Read at most `len` bytes from the current cursor, advancing it past
    /// them. The result is shorter than `len` at the end of the blob.
    std::vector<uint8_t> read_up_to(size_t len);

    /// Read at most `len` bytes from the current cursor into caller storage.
    /// Returns the number of bytes written, which is shorter than `len` at the
    /// end of the blob. Reuses the C `read_up_to` operation.
    size_t read_into(uint8_t* dst, size_t len);

    /// Read exactly `len` bytes at `offset` without moving the cursor. The
    /// range must lie within the blob.
    std::vector<uint8_t> read_range(uint64_t offset, size_t len) const;

    /// Read multiple ranges without moving the cursor. The result for
    /// `ranges[i]` contains exactly `ranges[i].length` bytes; order,
    /// duplicates and overlaps are preserved.
    std::vector<std::vector<uint8_t>> read_ranges(
        const std::vector<BlobRange>& ranges) const;

    /// Set the native sequential read-ahead size. Zero disables read-ahead.
    void set_buffer_size(size_t buffer_size);

    /// Move the cursor. Seeking past the end is allowed; reads then return
    /// no bytes.
    void seek(uint64_t pos);

    /// Current cursor position, in bytes from the start of the blob.
    uint64_t tell() const;

    /// Mark the native blob file closed. Idempotent. The allocation remains
    /// owned until destruction.
    void close();

    /// Native closed state.
    bool closed() const;
};

// ─── Fragment handles ────────────────────────────────────────────────────────

/// RAII handle over a dataset-bound or independently constructed fragment.
/// Native `FileFragment` owns an `Arc<Dataset>`, so bound handles remain valid
/// and keep reading the same dataset version after the source Dataset is closed
/// or mutated. Detached handles own copied native metadata and support
/// operation construction without dataset access.
class Fragment {
    Handle<LanceFragment, lance_fragment_close> handle_;

public:
    /// Adopt a handle from the C API; closed on destruction.
    explicit Fragment(LanceFragment* fragment);

    Fragment(Fragment&&) noexcept = default;
    Fragment& operator=(Fragment&&) noexcept = default;
    Fragment(const Fragment&) = delete;
    Fragment& operator=(const Fragment&) = delete;

    /// Construct detached native Fragment metadata from borrowed data files.
    ///
    /// The data files are cloned in input order. `physical_rows ==
    /// std::nullopt` maps to native `None`; all other metadata retains native
    /// `Fragment::new` defaults. A fragment ID of zero requests native
    /// commit-time allocation. Schema coverage and `physical_rows` are caller
    /// metadata; this constructor does not infer or revalidate them.
    static Fragment from_data_files(
        uint64_t fragment_id,
        const std::vector<const DataFile*>& data_files,
        std::optional<uint64_t> physical_rows = std::nullopt);

    /// Native fragment ID.
    uint64_t id() const;

    /// Number of Native base data files; excludes overlays and the deletion file.
    uint64_t data_file_count() const;

    /// Whether this handle retains a dataset-bound native fragment.
    bool is_bound() const;

    /// Clone one native base data file from owned fragment metadata.
    DataFile data_file_at(size_t index) const;

    /// Find the native base data file that stores a field ID. Missing fields
    /// return std::nullopt; a negative field ID is an invalid argument.
    std::optional<DataFile> data_file_for_field(int32_t field_id) const;

    /// Native `Fragment` metadata serialized as JSON. Includes base data
    /// files, overlays, deletion-file metadata, row-id/version metadata, and
    /// the optional physical_rows value.
    std::string metadata_json() const;

    /// Export the logical dataset schema through Arrow C Data Interface.
    /// `out` is caller-owned; release it exactly once.
    void schema(ArrowSchema* out) const;

    /// Clone the fragment's native schema from its staged dataset snapshot.
    LanceSchema native_schema() const;

    /// Physical row count, including deleted rows. May perform data-file I/O.
    uint64_t physical_rows() const;

    /// Deleted-row count. May load the deletion vector.
    uint64_t deleted_rows() const;

    /// Logical row count from Native count_rows(None).
    uint64_t logical_rows() const;

    /// Dataset read version captured by this bound fragment.
    uint64_t read_version() const;

    /// Start a scanner over this fragment's owned snapshot.
    Scanner scan() const;

    /// Take fragment-local logical rows by index with an explicit projection.
    /// Indices retain the selected core's ordering and duplicate constraints.
    /// `out` is caller-owned and its non-null `release` must be called exactly
    /// once. An empty `columns` vector is an explicit empty projection and
    /// produces a zero-column batch with the requested row count.
    void take(const uint32_t* indices, size_t num_indices,
              const std::vector<std::string>& columns,
              ArrowArrayStream* out) const;

    /// Take fragment-local logical rows with the full logical schema.
    void take(const uint32_t* indices, size_t num_indices,
              ArrowArrayStream* out) const;

    /// Take fragment-local logical rows and return the owning stream.
    ///
    /// The projection, explicit-empty-projection behavior and native
    /// validation are the same as the raw-out overload. The returned
    /// `ArrowStream` releases the native stream exactly once, on destruction
    /// or `reset()`; moving it transfers ownership and leaves the source empty.
    /// `out` from the raw-out overload remains caller-owned.
    ArrowStream take(const uint32_t* indices, size_t num_indices,
                     const std::vector<std::string>& columns) const;

    /// Take fragment-local logical rows with all columns and return the owning
    /// stream. Ownership and validation follow the projecting overload above.
    ArrowStream take(const uint32_t* indices, size_t num_indices) const;

    /// Borrow indices from a contiguous range and return the owning stream.
    /// The range is read only during this call. Empty-range and projection
    /// behavior are inherited from the pointer/count overload above.
    ArrowStream take(std::span<const uint32_t> indices,
                     const std::vector<std::string>& columns) const;

    /// Borrow all-column indices from a contiguous range and return the
    /// owning stream. Ownership follows the overload above.
    ArrowStream take(std::span<const uint32_t> indices) const;

    /// Count fragment rows. `std::nullopt` calls native count_rows(None); an
    /// empty string is passed as an explicit native filter.
    uint64_t count_rows(std::optional<std::string> filter = std::nullopt) const;

    /// Delete rows from this fragment without publishing a transaction.
    ///
    /// The source handle remains usable. The returned fragment owns the new
    /// deletion-file metadata and can be scanned before publication. A full
    /// deletion returns `std::nullopt`.
    std::optional<Fragment> delete_if(const std::string& predicate) const;

    /// Add SQL-derived columns to this bound fragment.
    ///
    /// `batch_size == std::nullopt` maps to native `None`; an engaged zero is
    /// forwarded as `Some(0)`. Returns detached fragment metadata and the
    /// complete native final schema for a later Merge operation.
    std::pair<Fragment, LanceSchema> add_columns_sql(
        const std::vector<SqlColumn>& columns,
        std::optional<uint32_t> batch_size = std::nullopt) const;

    /// Add all-null columns described by a borrowed Arrow schema.
    std::pair<Fragment, LanceSchema> add_columns_nulls(
        const ArrowSchema* columns) const;

    /// Add columns from a consumed Arrow stream using Native Reader semantics.
    std::pair<Fragment, LanceSchema> add_columns_reader(
        ArrowArrayStream* stream,
        std::optional<uint32_t> batch_size = std::nullopt) const;

    /// Add columns through the shared BatchUDF callback adapter.
    std::pair<Fragment, LanceSchema> add_columns_batch_udf(
        const BatchUDF& udf,
        std::optional<std::vector<std::string>> read_columns = std::nullopt,
        std::optional<uint32_t> batch_size = std::nullopt) const;

    /// Add columns through the shared schema-aware BatchUDF adapter.
    std::pair<Fragment, LanceSchema> add_columns_batch_udf(
        const BatchUDFV2& udf,
        std::optional<std::vector<std::string>> read_columns = std::nullopt,
        std::optional<uint32_t> batch_size = std::nullopt) const;

    /// Merge a consumed right-hand Arrow stream into this bound fragment.
    std::pair<Fragment, LanceSchema> merge_columns(
        ArrowArrayStream* stream,
        const std::string& left_on,
        const std::string& right_on) const;

    /// Rewrite existing columns through a native join and return matched
    /// physical offsets with the updated metadata.
    FragmentUpdateColumnsResult update_columns(
        ArrowArrayStream* stream,
        const std::string& left_on,
        const std::string& right_on) const;

    /// Stage a replacement data file using the manifest's default version.
    DataReplacementGroup write_columns(
        const LanceSchema& schema,
        ArrowArrayStream* stream) const;

    /// Stage a replacement data file using an explicit native file version.
    DataReplacementGroup write_column_with_version(
        const LanceSchema& schema,
        ArrowArrayStream* stream,
        LanceFileFormatVersion file_version) const;

    /// Add physical row offsets to a clone and return updated bound metadata.
    /// A full deletion returns `std::nullopt`.
    std::optional<Fragment> delete_rows(
        const std::vector<uint32_t>& offsets) const;

    /// Open an overlay writer for this bound fragment. The schema may be a
    /// compatible subset of the fragment schema. Commit the resulting overlay
    /// with the dataset read version used to open the writer.
    /// Native overlays are experimental. For release builds, set
    /// `LANCE_ENABLE_UNSTABLE_DATA_OVERLAY_FILES=1` in every process that reads
    /// or modifies an overlay dataset, including before publication.
    FragmentOverlayWriter write_overlay(const LanceSchema& schema) const;

    /// Create one fragment without publishing a version.
    ///
    /// Maps to native `FragmentCreateBuilder::write(source, Some(fragment_id))`.
    /// Native owns schema inference and compatibility, file format, physical row
    /// counts and error semantics; an empty input fails with the native "Input
    /// data was empty." error. The stream is consumed on every return path,
    /// including a null stream pointer (which is rejected unconsumed) and local
    /// conversion failures. A null `options.schema` keeps native inference.
    ///
    /// Returns independently owned detached metadata that stays valid after the
    /// source dataset, schema and session are closed. Closing it never deletes
    /// the written data file; publish the fragment with an Append transaction.
    static Fragment create(
        const std::string& dataset_uri,
        ArrowArrayStream* stream,
        uint64_t fragment_id,
        const FragmentCreateOptions& options = {},
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {},
        const Session* session = nullptr);

    /// Create zero or more fragments without publishing a version.
    ///
    /// Maps to native `FragmentCreateBuilder::write_fragments`, so
    /// `options.write.params.max_rows_per_file` splitting and fragment ID
    /// assignment stay native. The stream follows the same consumption contract
    /// as `create`; an empty input returns an empty vector.
    static std::vector<Fragment> create_many(
        const std::string& dataset_uri,
        ArrowArrayStream* stream,
        const FragmentCreateOptions& options = {},
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {},
        const Session* session = nullptr);

    /// Create one fragment from an existing Lance data file in `dataset`'s data
    /// directory.
    ///
    /// Maps to native `FileFragment::create_from_file`. `filename` is relative
    /// to the dataset's data directory. `physical_rows == std::nullopt` maps to
    /// native `None`; an engaged value maps to `Some(value)`. Native owns file
    /// version, schema compatibility and physical-row validation. The dataset
    /// supplies the target snapshot and is not modified. A file whose stored
    /// schema native cannot project to the dataset schema is rejected, as with
    /// Blob v2 data files.
    static Fragment create_from_file(
        const Dataset& dataset,
        const std::string& filename,
        uint64_t fragment_id,
        std::optional<uint64_t> physical_rows = std::nullopt);

    const LanceFragment* c_handle() const;
};





/// Move-only owner of a native `FragmentUpdateColumnsResult`.
class FragmentUpdateColumnsResult {
    Handle<LanceFragmentUpdateColumnsResult,
           lance_fragment_update_columns_result_close>
        handle_;

public:
    explicit FragmentUpdateColumnsResult(LanceFragmentUpdateColumnsResult* result);

    FragmentUpdateColumnsResult(FragmentUpdateColumnsResult&&) noexcept = default;
    FragmentUpdateColumnsResult& operator=(
        FragmentUpdateColumnsResult&&) noexcept = default;
    FragmentUpdateColumnsResult(const FragmentUpdateColumnsResult&) = delete;
    FragmentUpdateColumnsResult& operator=(
        const FragmentUpdateColumnsResult&) = delete;

    /// Independent detached fragment metadata.
    Fragment fragment() const;

    /// Native modified field IDs in result order.
    std::vector<uint32_t> fields_modified() const;

    /// Matched physical row offsets in their native u32 domain, copied in one pass.
    std::vector<uint32_t> matched_offsets() const;

    /// Portable Roaring interoperability export. Normal Update construction
    /// should use matched_offsets() directly.
    std::vector<uint8_t> matched_offsets_serialized() const;

    const LanceFragmentUpdateColumnsResult* c_handle() const;
};

/// Move-only owner of a native Lance schema.
///
/// Unlike the Arrow export, a native schema preserves field and parent IDs,
/// logical types, encodings, dictionary values, nested metadata, field order
/// and nullability. A clone remains valid after the dataset, fragment or
/// operation that produced it is closed.
class LanceSchema {
    Handle<::LanceSchema, lance_schema_close> handle_;

public:
    explicit LanceSchema(::LanceSchema* schema);

    LanceSchema(LanceSchema&&) noexcept = default;
    LanceSchema& operator=(LanceSchema&&) noexcept = default;
    LanceSchema(const LanceSchema&) = delete;
    LanceSchema& operator=(const LanceSchema&) = delete;

    /// Independently owned native clone.
    LanceSchema clone() const;

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
                               const std::vector<std::string>& columns);

    /// Native Lance field ID for a column path. Throws when the path does not
    /// resolve.
    int32_t field_id(const std::string& column) const;

    /// Build a native schema from a borrowed Arrow schema. Fields without a
    /// `lance:field_id` metadata entry receive fresh native IDs; existing
    /// IDs, dictionaries, and metadata are preserved. Throws on native
    /// validation failure.
    static LanceSchema from_arrow(const ArrowSchema* schema);

    /// Export an Arrow interoperability view. `out` is caller-owned and must
    /// be released exactly once. Arrow cannot represent every native detail.
    void to_arrow(ArrowSchema* out) const;

    const ::LanceSchema* c_handle() const;
};



namespace fragment_mutation_detail {

std::pair<Fragment, LanceSchema> take_fragment_schema_pair(
    ::LanceFragment* fragment, ::LanceSchema* schema);

DataReplacementGroup take_replacement_group(
    LanceDataReplacementGroup* group);

}  // namespace fragment_mutation_detail



























































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
    VectorIndexOptions& accelerator(std::string device);
};

// ─── Native maintenance options and plans ───────────────────────────────────

/// Presence-preserving owner of native `CompactionOptions`.
class CompactionOptions {
    Handle<LanceCompactionOptionsV2, lance_compaction_options_v2_free> handle_;

public:
    CompactionOptions();

    explicit CompactionOptions(LanceCompactionOptionsV2* options);

    CompactionOptions(CompactionOptions&&) noexcept = default;
    CompactionOptions& operator=(CompactionOptions&&) noexcept = default;
    CompactionOptions(const CompactionOptions&) = delete;
    CompactionOptions& operator=(const CompactionOptions&) = delete;

    /// Native `CompactionOptions::from_dataset_config(dataset.config())`.
    static CompactionOptions from_dataset(const Dataset& dataset);

    CompactionOptions clone() const;

    CompactionOptions& set_target_rows_per_fragment(uint64_t value);

    CompactionOptions& set_max_rows_per_group(uint64_t value);

    CompactionOptions& set_max_bytes_per_file(std::optional<uint64_t> value);

    CompactionOptions& set_materialize_deletions(bool value);

    CompactionOptions& set_materialize_deletions_threshold(float value);

    /// Store the override; nullopt selects the native default. Native
    /// compaction rejects an explicit zero when executed, not in this setter.
    CompactionOptions& set_num_threads(std::optional<uint64_t> value);

    CompactionOptions& set_batch_size(std::optional<uint64_t> value);

    CompactionOptions& set_io_buffer_size(std::optional<uint64_t> value);

    CompactionOptions& set_defer_index_remap(bool value);

    CompactionOptions& set_index_remap_mode(LanceIndexRemapMode value);

    CompactionOptions& set_compaction_mode(
        std::optional<LanceCompactionMode> value);

    CompactionOptions& set_binary_copy_read_batch_bytes(
        std::optional<uint64_t> value);

    CompactionOptions& set_max_source_fragments(
        std::optional<uint64_t> value);

    CompactionOptions& set_max_source_rows(std::optional<uint64_t> value);

    CompactionOptions& set_max_source_bytes(std::optional<uint64_t> value);

    CompactionOptions& set_excluded_fragment_ids(
        const std::vector<uint32_t>& fragment_ids);

    CompactionOptions& set_max_overlays_per_fragment(
        std::optional<uint64_t> value);

    CompactionOptions& set_data_storage_version(
        std::optional<std::string> selector);

    CompactionOptions& set_transaction_properties(
        std::optional<std::vector<std::pair<std::string, std::string>>> value =
            std::nullopt);

    const LanceCompactionOptionsV2* c_handle() const;
};

/// Owned native compaction rewrite result.
class RewriteResult {
    Handle<LanceRewriteResult, lance_rewrite_result_close> handle_;

public:
    explicit RewriteResult(LanceRewriteResult* result);

    RewriteResult(RewriteResult&&) noexcept = default;
    RewriteResult& operator=(RewriteResult&&) noexcept = default;
    RewriteResult(const RewriteResult&) = delete;
    RewriteResult& operator=(const RewriteResult&) = delete;

    uint64_t read_version() const;

    LanceCompactionMetrics metrics() const;

    size_t original_fragment_count() const;

    size_t new_fragment_count() const;

    std::string original_fragment_metadata_json(size_t index) const;

    std::string new_fragment_metadata_json(size_t index) const;

    /// Clone one original fragment as an owned native snapshot. The handle
    /// stays valid after this rewrite result is closed.
    Fragment original_fragment_at(size_t index) const;

    /// Clone one new (staged) fragment native compaction produced. Such a
    /// fragment can seed a `RewriteGroup`.
    Fragment new_fragment_at(size_t index) const;

    std::string json() const;

    static RewriteResult from_json(std::string_view json);

    const LanceRewriteResult* c_handle() const;
};

/// Owned snapshot of one native compaction task.
class CompactionTask {
    Handle<LanceCompactionTask, lance_compaction_task_close> handle_;

public:
    explicit CompactionTask(LanceCompactionTask* task);

    CompactionTask(CompactionTask&&) noexcept = default;
    CompactionTask& operator=(CompactionTask&&) noexcept = default;
    CompactionTask(const CompactionTask&) = delete;
    CompactionTask& operator=(const CompactionTask&) = delete;

    uint64_t read_version() const;

    size_t fragment_count() const;

    std::string fragment_metadata_json(size_t index) const;

    std::vector<std::string> fragment_metadata_json() const;

    CompactionOptions options() const;

    RewriteResult execute(const Dataset& dataset) const;

    std::string json() const;

    static CompactionTask from_json(std::string_view json);

    const LanceCompactionTask* c_handle() const;
};

/// Owned native compaction plan.
class CompactionPlan {
    Handle<LanceCompactionPlan, lance_compaction_plan_close> handle_;

public:
    explicit CompactionPlan(LanceCompactionPlan* plan);

    CompactionPlan(CompactionPlan&&) noexcept = default;
    CompactionPlan& operator=(CompactionPlan&&) noexcept = default;
    CompactionPlan(const CompactionPlan&) = delete;
    CompactionPlan& operator=(const CompactionPlan&) = delete;

    uint64_t read_version() const;

    size_t num_tasks() const;

    CompactionTask task(size_t index) const;

    CompactionOptions options() const;

    std::string json() const;

    static CompactionPlan from_json(std::string_view json);

    const LanceCompactionPlan* c_handle() const;
};

/// Presence-preserving owner of native `OptimizeOptions`.
class OptimizeIndicesOptions {
    Handle<LanceOptimizeIndicesOptions, lance_optimize_indices_options_free>
        handle_;

public:
    OptimizeIndicesOptions();

    explicit OptimizeIndicesOptions(LanceOptimizeIndicesOptions* options);

    OptimizeIndicesOptions(OptimizeIndicesOptions&&) noexcept = default;
    OptimizeIndicesOptions& operator=(OptimizeIndicesOptions&&) noexcept =
        default;
    OptimizeIndicesOptions(const OptimizeIndicesOptions&) = delete;
    OptimizeIndicesOptions& operator=(const OptimizeIndicesOptions&) = delete;

    OptimizeIndicesOptions clone() const;

    OptimizeIndicesOptions& set_num_indices_to_merge(
        std::optional<uint64_t> value);

    OptimizeIndicesOptions& set_enable_partition_rebalancing(bool value);

    OptimizeIndicesOptions& set_index_names(
        std::optional<std::vector<std::string>> names);

    OptimizeIndicesOptions& set_retrain(bool value);

    OptimizeIndicesOptions& set_transaction_properties(
        std::optional<std::vector<std::pair<std::string, std::string>>> value =
            std::nullopt);

    OptimizeIndicesOptions& set_max_rows_per_task(
        std::optional<uint64_t> value);

    OptimizeIndicesOptions& set_max_rows_per_segment(
        std::optional<uint64_t> value);

    const LanceOptimizeIndicesOptions* c_handle() const;
};

/// Owned native optimized index segment result.
class IndexSegmentResult {
    Handle<LanceIndexSegmentResult, lance_index_segment_result_close> handle_;

public:
    explicit IndexSegmentResult(LanceIndexSegmentResult* result);

    IndexSegmentResult(IndexSegmentResult&&) noexcept = default;
    IndexSegmentResult& operator=(IndexSegmentResult&&) noexcept = default;
    IndexSegmentResult(const IndexSegmentResult&) = delete;
    IndexSegmentResult& operator=(const IndexSegmentResult&) = delete;

    std::array<uint8_t, 16> plan_id() const;

    uint64_t read_version() const;

    uint32_t group() const;

    IndexSegmentMetadata segment() const;

    size_t replace_count() const;

    std::array<uint8_t, 16> replace_uuid(size_t index) const;

    std::string json() const;

    static IndexSegmentResult from_json(std::string_view json);

    const LanceIndexSegmentResult* c_handle() const;
};

/// Owned snapshot of one native distributed index build task.
class OptimizeIndexTask {
    Handle<LanceOptimizeIndexTask, lance_optimize_index_task_close> handle_;

public:
    explicit OptimizeIndexTask(LanceOptimizeIndexTask* task);

    OptimizeIndexTask(OptimizeIndexTask&&) noexcept = default;
    OptimizeIndexTask& operator=(OptimizeIndexTask&&) noexcept = default;
    OptimizeIndexTask(const OptimizeIndexTask&) = delete;
    OptimizeIndexTask& operator=(const OptimizeIndexTask&) = delete;

    std::array<uint8_t, 16> plan_id() const;

    uint64_t read_version() const;

    uint32_t group() const;

    size_t fragment_count() const;

    uint32_t fragment_id(size_t index) const;

    std::vector<uint32_t> fragment_ids() const;

    IndexSegmentMetadata reference() const;

    size_t old_segment_count() const;

    IndexSegmentMetadata old_segment(size_t index) const;

    std::optional<uint64_t> num_indices_to_merge() const;

    bool enable_partition_rebalancing() const;

    bool retrain() const;

    std::optional<IndexSegmentResult> execute(const Dataset& dataset) const;

    std::string json() const;

    static OptimizeIndexTask from_json(std::string_view json);

    const LanceOptimizeIndexTask* c_handle() const;
};

/// Owned snapshot of one native distributed index merge.
class OptimizeIndexMerge {
    Handle<LanceOptimizeIndexMerge, lance_optimize_index_merge_close> handle_;

public:
    explicit OptimizeIndexMerge(LanceOptimizeIndexMerge* merge);

    OptimizeIndexMerge(OptimizeIndexMerge&&) noexcept = default;
    OptimizeIndexMerge& operator=(OptimizeIndexMerge&&) noexcept = default;
    OptimizeIndexMerge(const OptimizeIndexMerge&) = delete;
    OptimizeIndexMerge& operator=(const OptimizeIndexMerge&) = delete;

    std::array<uint8_t, 16> plan_id() const;

    uint64_t read_version() const;

    uint32_t group() const;

    size_t old_segment_count() const;

    IndexSegmentMetadata old_segment(size_t index) const;

    size_t replace_count() const;

    std::array<uint8_t, 16> replace_uuid(size_t index) const;

    std::optional<IndexSegmentResult> execute(
        const Dataset& dataset,
        const std::vector<IndexSegmentResult>& task_results) const;

    std::string json() const;

    static OptimizeIndexMerge from_json(std::string_view json);

    const LanceOptimizeIndexMerge* c_handle() const;
};

/// Owned native distributed index optimization plan.
class OptimizeIndicesPlan {
    Handle<LanceOptimizeIndicesPlan, lance_optimize_indices_plan_close>
        handle_;

public:
    explicit OptimizeIndicesPlan(LanceOptimizeIndicesPlan* plan);

    OptimizeIndicesPlan(OptimizeIndicesPlan&&) noexcept = default;
    OptimizeIndicesPlan& operator=(OptimizeIndicesPlan&&) noexcept = default;
    OptimizeIndicesPlan(const OptimizeIndicesPlan&) = delete;
    OptimizeIndicesPlan& operator=(const OptimizeIndicesPlan&) = delete;

    std::array<uint8_t, 16> plan_id() const;

    uint64_t read_version() const;

    size_t num_tasks() const;

    size_t num_merges() const;

    OptimizeIndexTask task(size_t index) const;

    OptimizeIndexMerge merge(size_t index) const;

    std::string json() const;

    static OptimizeIndicesPlan from_json(std::string_view json);

    const LanceOptimizeIndicesPlan* c_handle() const;
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
                                      uint64_t generation);

    ::LanceCompactedSsTable to_c() const;
};

/// Complete input for Operation::make_update().
///
/// Every member maps to one native field unchanged. Native commit validation
/// owns schema coverage, conflicts and row-version behavior, so no default is
/// applied: an engaged empty vector means the caller supplied no entries.
/// `update_mode == std::nullopt`, a null `inserted_rows_filter` and a null
/// `updated_fragment_offsets` map to native `None`, which is distinct from
/// empty populated values. `mem_wal_dir_id == std::nullopt` maps to native
/// `None`; an engaged value (including an empty string) is required by Lance
/// 15 when compaction progress is present.
struct UpdateOperationSpec {
    std::vector<uint64_t> removed_fragment_ids;
    std::vector<const Fragment*> updated_fragments;
    std::vector<const Fragment*> new_fragments;
    std::vector<uint32_t> fields_modified;
    std::vector<CompactedSsTable> compacted_sstables;
    std::vector<uint32_t> fields_for_preserving_frag_bitmap;
    std::optional<UpdateMode> update_mode;
    std::optional<KeyExistenceFilter> inserted_rows_filter;
    std::optional<UpdatedFragmentOffsets> updated_fragment_offsets;
    std::optional<std::string> mem_wal_dir_id;
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

    ShardWriterConfig& set_durable_write(bool value);
    ShardWriterConfig& set_max_wal_buffer_size(uint64_t value);
    ShardWriterConfig& set_max_wal_flush_interval_ms(uint64_t value);
    ShardWriterConfig& set_max_memtable_size(uint64_t value);
    ShardWriterConfig& set_max_memtable_rows(uint64_t value);
    ShardWriterConfig& set_max_memtable_batches(uint64_t value);
    ShardWriterConfig& set_max_unflushed_memtable_bytes(uint64_t value);
    ShardWriterConfig& set_manifest_scan_batch_size(uint64_t value);
    ShardWriterConfig& set_backpressure_log_interval_ms(uint64_t value);
    ShardWriterConfig& set_stats_log_interval_ms(uint64_t value);
    ShardWriterConfig& add_hnsw_params(HnswParams value);

    LanceShardWriterConfig to_c(std::vector<std::string>& names,
                                std::vector<::LanceMemWalHnswParams>& params) const;
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
    explicit ShardingField(::LanceMemWalShardingField* field);

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
        std::map<std::string, std::string> parameters = {});

    ShardingField clone() const;

    std::string id() const;

    std::vector<int32_t> source_ids() const;

    std::optional<std::string> transform() const;

    std::optional<std::string> expression() const;

    std::string result_type() const;

    std::map<std::string, std::string> parameters() const;

    std::optional<std::string> parameter(const std::string& key) const;

    const ::LanceMemWalShardingField* c_handle() const;
};

/// Owned native sharding spec.
class ShardingSpec {
    Handle<::LanceMemWalShardingSpec, lance_mem_wal_sharding_spec_free> handle_;

public:
    explicit ShardingSpec(::LanceMemWalShardingSpec* spec);

    ShardingSpec(ShardingSpec&&) noexcept = default;
    ShardingSpec& operator=(ShardingSpec&&) noexcept = default;
    ShardingSpec(const ShardingSpec&) = delete;
    ShardingSpec& operator=(const ShardingSpec&) = delete;

    static ShardingSpec create(uint32_t spec_id,
                               std::vector<ShardingField> fields);

    ShardingSpec clone() const;

    /// Evaluate against the dataset's native snapshot schema. The dataset and
    /// batch are borrowed for the call; the returned batch owns the native
    /// result.
    Batch evaluate(const Dataset& dataset, const Batch& batch) const;

    /// Evaluate against an explicit native schema instead of a dataset schema.
    Batch evaluate(const Batch& batch, const LanceSchema& schema) const;

    uint32_t id() const;

    size_t field_count() const;

    ShardingField field_at(size_t index) const;

    std::vector<ShardingField> fields() const;

    void add_field(ShardingField field);

    const ::LanceMemWalShardingSpec* c_handle() const;

private:
    friend class Dataset;
};

/// Owned native shard snapshot.
class ShardSnapshot {
    Handle<::LanceMemWalShardSnapshot, lance_mem_wal_shard_snapshot_free> handle_;

public:
    explicit ShardSnapshot(::LanceMemWalShardSnapshot* snapshot);

    ShardSnapshot(ShardSnapshot&&) noexcept = default;
    ShardSnapshot& operator=(ShardSnapshot&&) noexcept = default;
    ShardSnapshot(const ShardSnapshot&) = delete;
    ShardSnapshot& operator=(const ShardSnapshot&) = delete;

    static ShardSnapshot create(const std::string& shard_id);

    ShardSnapshot clone() const;

    std::string shard_id() const;

    uint32_t spec_id() const;

    ShardSnapshot& set_spec_id(uint32_t value);

    uint64_t current_generation() const;

    ShardSnapshot& set_current_generation(uint64_t value);

    ShardSnapshot& add_sstable(uint64_t generation, const std::string& path);

    std::vector<SsTable> sstables() const;

    const ::LanceMemWalShardSnapshot* c_handle() const;
};

/// Owned copy of native MemWAL index details.
class MemWalIndexDetails {
    uint32_t num_shards_ = 0;
    bool maintain_all_ = false;
    std::vector<std::string> maintained_;
    std::vector<ShardingSpec> specs_;
    std::map<std::string, std::string> defaults_;
    std::string dir_id_;

public:
    MemWalIndexDetails(uint32_t num_shards,
                       bool maintain_all,
                       std::vector<std::string> maintained,
                       std::vector<ShardingSpec> specs,
                       std::map<std::string, std::string> defaults,
                       std::string dir_id = {});

    MemWalIndexDetails(MemWalIndexDetails&&) noexcept = default;
    MemWalIndexDetails& operator=(MemWalIndexDetails&&) noexcept = default;
    MemWalIndexDetails(const MemWalIndexDetails&) = delete;
    MemWalIndexDetails& operator=(const MemWalIndexDetails&) = delete;

    /// Directory identifier of this MemWAL. Empty denotes the legacy
    /// `_mem_wal` directory.
    const std::string& dir_id() const;
    uint32_t num_shards() const;
    bool maintain_all_indexes() const;
    const std::vector<std::string>& maintained_indexes() const;
    const std::map<std::string, std::string>& writer_config_defaults() const;
    size_t sharding_spec_count() const;
    ShardingSpec sharding_spec_at(size_t index) const;
    std::vector<ShardingSpec> sharding_specs() const;
};

/// Owned native shard writer.
class ShardWriter {
    Handle<::LanceShardWriter, lance_shard_writer_free> handle_;

public:
    explicit ShardWriter(::LanceShardWriter* writer);

    ~ShardWriter() = default;
    ShardWriter(ShardWriter&&) noexcept = default;
    ShardWriter& operator=(ShardWriter&&) noexcept = default;
    ShardWriter(const ShardWriter&) = delete;
    ShardWriter& operator=(const ShardWriter&) = delete;

    /// Open a native writer for one shard. The dataset handle is borrowed.
    static ShardWriter open(const Dataset& dataset,
                            const std::string& shard_id,
                            const ShardWriterConfig& config = {});

    std::string shard_id() const;

    ShardWriteResult put(ArrowArrayStream* stream);

    ShardWriteResult delete_rows(ArrowArrayStream* stream);

    ShardWriterStats stats() const;

    ShardMemTableStats memtable_stats() const;

    /// Snapshot the writer's latest manifest. Returns nullopt when the writer
    /// has not committed yet; an uncommitted shard is composed by the caller's
    /// scanner using a default snapshot.
    std::optional<ShardSnapshot> snapshot() const;

    LsmScanner lsm_scanner(std::vector<ShardSnapshot> snapshots);

    void close();

    const ::LanceShardWriter* c_handle() const;
};

/// Owned native LSM scanner.
class LsmScanner {
    Handle<::LanceLsmScanner, lance_lsm_scanner_free> handle_;

public:
    explicit LsmScanner(::LanceLsmScanner* scanner);
    ~LsmScanner() = default;
    LsmScanner(LsmScanner&&) noexcept = default;
    LsmScanner& operator=(LsmScanner&&) noexcept = default;
    LsmScanner(const LsmScanner&) = delete;
    LsmScanner& operator=(const LsmScanner&) = delete;

    static LsmScanner from_snapshots(const Dataset& dataset,
                                     std::vector<ShardSnapshot> snapshots);

    LsmScanner& project(const std::vector<std::string>& columns);

    LsmScanner& filter(const std::string& expression);

    LsmScanner& limit(std::optional<int64_t> limit,
                      std::optional<int64_t> offset = std::nullopt);

    LsmScanner& with_row_address();

    LsmScanner& with_memtable_gen();

    Batch to_batch() const;
    std::vector<Batch> to_batches() const;
    Table to_table() const;
    uint64_t count_rows() const;

    ArrowStream to_reader() const;

    const ::LanceLsmScanner* c_handle() const;
};

/// Owned native execution plan.
class ExecutionPlan {
    Handle<::LanceMemWalPlan, lance_mem_wal_plan_free> handle_;

public:
    explicit ExecutionPlan(::LanceMemWalPlan* plan);
    ~ExecutionPlan() = default;
    ExecutionPlan(ExecutionPlan&&) noexcept = default;
    ExecutionPlan& operator=(ExecutionPlan&&) noexcept = default;
    ExecutionPlan(const ExecutionPlan&) = delete;
    ExecutionPlan& operator=(const ExecutionPlan&) = delete;

    ArrowSchemaOwner schema() const;
    ArrowSchemaOwner dataset_schema() const;

    std::string explain() const;

    Batch to_batch() const;
    std::vector<Batch> to_batches() const;
    Table to_table() const;

    ArrowStream to_reader() const;

    const ::LanceMemWalPlan* c_handle() const;
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
    explicit LsmPointLookupPlanner(::LanceLsmPointLookupPlanner* planner);
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
    explicit LsmVectorSearchPlanner(::LanceLsmVectorSearchPlanner* planner);
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
        explicit ArrowStreamInput(ArrowArrayStream* stream);

        ~ArrowStreamInput();

        ArrowStreamInput(const ArrowStreamInput&) = delete;
        ArrowStreamInput& operator=(const ArrowStreamInput&) = delete;
        ArrowStreamInput(ArrowStreamInput&&) = delete;
        ArrowStreamInput& operator=(ArrowStreamInput&&) = delete;

        const ArrowSchema* read_schema();

        ArrowArrayStream* consume() noexcept;
    };

    static std::vector<const char*> to_c_key_values(
        const std::vector<std::pair<std::string, std::string>>& entries);

    static LanceWriteOptions to_c_write_options(
        const WriteOptions& options,
        LanceAutoCleanupParams& cleanup);

    static LanceUncommittedWriteOptions to_c_uncommitted_write_options(
        const UncommittedWriteOptions& options,
        LanceAutoCleanupParams& cleanup,
        std::vector<const char*>& properties);

    static LanceCommitOptions to_c_commit_options(const CommitOptions& options);

    static RemovalStats from_c_removal_stats(const LanceRemovalStats& stats);

    static LanceCleanupPolicy to_c_cleanup_policy(
        const CleanupPolicy& policy,
        std::vector<uint64_t>& versions,
        uint64_t& empty_versions_sentinel);

    static LanceCleanupOperationOptions to_c_cleanup_operation(
        const CleanupOperationOptions& options);

    static CleanupExplanation read_cleanup_explanation(
        const LanceCleanupExplanation* explanation);

    static VersionDetails read_version(const LanceVersion* version);

    static TagInfo read_tag(const LanceTag* tag);

    /// Move raw handles into RAII owners. `blobs` must be reserved up front so
    /// nothing can throw while handles are still unowned.
    ///
    /// Leak-freedom also rests on the C side: all `lance_dataset_take_blobs*`
    /// entry points fill `out` all-or-nothing and leave it untouched on error.
    /// A partial fill before an error would leak, because `detail::throw_error()`
    /// throws before this runs and `raw` owns nothing.
    static void adopt_blobs(const std::vector<LanceBlobFile*>& raw,
                            std::vector<std::optional<BlobFile>>& blobs);

    static std::string run_metadata_update_json(
        LanceDataset* dataset,
        const std::vector<std::pair<std::string, std::optional<std::string>>>& values,
        MetadataUpdateMode mode,
        bool schema_metadata);

    static void run_field_metadata_update(
        LanceDataset* dataset,
        const std::vector<FieldMetadataUpdate>& updates);

    struct TrackedFilesProgressCapture {
        TrackedFilesProgressCallback callback;
    };

    static void tracked_files_progress_trampoline(
        void* callback_ctx,
        uint64_t manifests_processed,
        const uint64_t* manifests_total) noexcept;

    static void tracked_files_progress_release(
        void* callback_ctx) noexcept;

    /// Shared write path. `native_options` selects the native-defaults entry
    /// point; otherwise `legacy_params` selects the legacy 20/14 entry point.
    static Dataset write_impl(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const LanceWriteParams* legacy_params,
        const LanceWriteOptions* native_options,
        const std::vector<std::pair<std::string, std::string>>& storage_opts,
        const StorageOptionsAccessor* storage_options_accessor = nullptr);

public:
    /// Open a dataset at the given URI. Pass `version` = 0 (the default) for
    /// the latest, or a specific version id from `versions()` to check out
    /// that version, e.g. `lance::Dataset::open("data.lance", {}, /*version=*/42)`.
    static Dataset open(
        const std::string& uri,
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {},
        uint64_t version = 0);

    /// Open a dataset with caches owned by `session`. The returned dataset
    /// remains valid if the Session wrapper is destroyed first.
    static Dataset open_with_session(
        const Session& session,
        const std::string& uri,
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {},
        uint64_t version = 0);

    /// Open a dataset with additive options and an optional shared session.
    ///
    /// A NULL `storage_options_accessor` selects native storage defaults. A
    /// non-NULL accessor is retained by the returned dataset, so the C++
    /// wrapper may be destroyed immediately after this call; later I/O uses
    /// the native provider refresh, error and cache policy.
    static Dataset open_with_options(
        const std::string& uri,
        const DatasetOpenOptions& options = {},
        const Session* session = nullptr);

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
        const CommitLock* commit_lock);

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
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {});

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
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {});

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
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {});

    /// Write with a retained native dynamic storage-options accessor. This
    /// keeps the legacy 20/14 auto-cleanup policy. The returned dataset retains
    /// the accessor and provider for later I/O.
    static Dataset write_with_accessor(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const StorageOptionsAccessor& storage_options_accessor);

    /// Write with explicit legacy `WriteParams` and a retained native dynamic
    /// storage-options accessor.
    static Dataset write_with_params_and_accessor(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const WriteParams& params,
        const StorageOptionsAccessor& storage_options_accessor);

    /// Write with native `WriteParams::default()` semantics and a retained
    /// native dynamic storage-options accessor.
    static Dataset write_with_options_and_accessor(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const WriteOptions& options,
        const StorageOptionsAccessor& storage_options_accessor);

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
        const CommitLock* commit_lock);

    /// Write with an owned native-backed options handle.
    ///
    /// The handle supplies native write defaults plus the session, storage
    /// accessor, commit lock, base placement, blob policy, file-writer options
    /// and progress callbacks. The handle is borrowed only for this call. The
    /// stream is consumed on every return path. Throws `lance::Error` on
    /// failure.
    static Dataset write_with_native_options(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const NativeWriteOptions& options);

    /// Stage an Arrow stream with an owned native-backed options handle.
    ///
    /// The returned transaction owns uncommitted data-file metadata and does
    /// not retain the options handle. Publication naming remains controlled by
    /// `Dataset::commit` / `Transaction` commit options. The stream is consumed
    /// on every return path. Throws `lance::Error` on failure; a failed stage
    /// does not publish a version.
    static Transaction write_uncommitted_with_native_options(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const NativeWriteOptions& options);

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
        const Session* session = nullptr);

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
        const Session* session = nullptr);

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
        const CommitLock* commit_lock);

    /// Publish one native transaction through the URI's native CommitBuilder.
    /// The transaction handle remains valid and is cloned by the C API.
    static Dataset commit(
        const std::string& uri,
        const Transaction& transaction,
        const CommitOptions& options = {},
        const std::vector<std::pair<std::string, std::string>>& storage_opts = {},
        const Session* session = nullptr);

    /// Publish one native transaction with a retained dynamic storage-options
    /// accessor. The returned dataset retains the accessor and provider.
    static Dataset commit_with_accessor(
        const std::string& uri,
        const Transaction& transaction,
        const StorageOptionsAccessor& storage_options_accessor,
        const CommitOptions& options = {},
        const Session* session = nullptr);

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
        const CommitLock* commit_lock);

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
    Dataset clone_handle() const;

    /// Number of rows in the dataset.
    uint64_t count_rows() const;

    /// Number of rows matching `filter` through native
    /// `Dataset::count_rows(Some(filter))`. An empty string is an explicit
    /// filter, not the unfiltered metadata fast path; use count_rows() for
    /// native `None`.
    uint64_t count_rows(const std::string& filter) const;

    /// Version of this dataset snapshot.
    uint64_t version() const;

    /// Fully qualified URI resolved when this dataset was opened. For a
    /// namespace-opened dataset this is the `describe_table` location, not the
    /// table ID. The returned copy remains valid after this handle is destroyed.
    /// Throws lance::Error on failure.
    std::string uri() const;

    /// Latest version ID (queries object store).
    uint64_t latest_version() const;

    /// Table metadata for this snapshot as a JSON object.
    /// Schema metadata is available through `schema()` instead.
    /// Throws lance::Error on failure.
    std::string metadata_json() const;

    /// Merge or replace table metadata and return the complete updated map as
    /// a JSON object. A `std::nullopt` value removes its key; an empty string
    /// stores a real empty value. `MetadataUpdateMode::Replace` with an empty
    /// vector clears the map. Mutates this dataset in place; existing
    /// snapshots keep their prior view. Throws lance::Error on failure.
    std::string update_metadata_json(
        const std::vector<std::pair<std::string, std::optional<std::string>>>& values,
        MetadataUpdateMode mode = MetadataUpdateMode::Merge);

    /// Merge or replace schema metadata and return the complete updated map as
    /// a JSON object. Input semantics match `update_metadata_json`. Read the
    /// committed schema metadata through `schema()`. Throws lance::Error on
    /// failure.
    std::string update_schema_metadata_json(
        const std::vector<std::pair<std::string, std::optional<std::string>>>& values,
        MetadataUpdateMode mode = MetadataUpdateMode::Merge);

    /// Apply a batch of per-field metadata updates in one native commit.
    /// Duplicate references to the same resolved field follow native
    /// last-write-wins behavior. Read the committed field metadata through
    /// `schema()`. Throws lance::Error on failure.
    void update_field_metadata(const std::vector<FieldMetadataUpdate>& updates);

    /// Current snapshot's canonical native Lance data-storage version string.
    /// Throws lance::Error on failure.
    std::string data_storage_version() const;

    /// Whether the current snapshot uses stable row IDs.
    /// Throws lance::Error on failure.
    bool has_stable_row_ids() const;

    /// Field IDs in the current schema's native pre-order traversal.
    /// Throws lance::Error on failure.
    std::vector<int32_t> field_ids() const;

    /// Resolve a field reference against the current snapshot's schema.
    /// Throws lance::Error on an unknown ID or path.
    int32_t resolve_field_id(const FieldReference& field) const;

    /// Native minimal Lance field path for `field_id`, including nested fields.
    /// Throws lance::Error on failure.
    std::string field_path(int32_t field_id) const;

    /// Snapshot the dataset's version history, ordered by version id.
    /// Throws lance::Error on failure.
    std::vector<VersionInfo> versions() const;

    /// Snapshot the dataset's native object-store I/O counters without
    /// resetting them. The counters are the current native accumulator for the
    /// store returned by `Dataset::object_store(None)`; a preceding incremental
    /// call resets that accumulator.
    LanceObjectStoreIoStats io_stats_snapshot() const;

    /// Return dataset object-store counters accumulated since the previous
    /// incremental call and reset the native accumulator. No binding-side
    /// baseline is kept.
    LanceObjectStoreIoStats io_stats_incremental() const;

    /// Snapshot the dataset's manifest config map.
    ///
    /// The returned map owns copies of all keys and values. The snapshot is
    /// independent of this handle. Entry order is unspecified. A config entry
    /// containing an embedded NUL byte cannot be represented as a C string and
    /// throws with LANCE_ERR_INTERNAL. Throws lance::Error on failure.
    std::map<std::string, std::string> config() const;

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
        MetadataUpdateMode mode = MetadataUpdateMode::Merge);

    /// Apply native merge-style config updates, committing a new manifest.
    /// Existing callers keep the original void-returning contract.
    void update_config(const std::vector<ConfigUpdate>& updates);

    /// Snapshot native base paths as JSON keyed by decimal ID. Each value has
    /// `id`, `name`, `path` and `is_dataset_root`. Throws lance::Error on
    /// failure.
    std::string base_paths_json() const;

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
        const AddBasesOptions& options = {});

    /// Export the native tracked-files stream. The returned owner releases the
    /// stream exactly once. The optional progress callback's capture is owned
    /// until the native emitter releases it, which can follow stream cleanup.
    ArrowStream tracked_files_stream(
        std::optional<uint64_t> min_version = std::nullopt,
        TrackedFilesProgressCallback progress = {}) const;

    /// Export the native physical-file stream for the primary dataset root.
    /// The returned owner releases the stream exactly once. Throws
    /// lance::Error on failure.
    ArrowStream all_files_stream() const;

    /// Read this snapshot's native transaction. Returns nullopt when the
    /// manifest has no transaction file. The result is independent of this
    /// Dataset handle.
    std::optional<Transaction> read_transaction() const;

    /// Read a specific version's native transaction. Returns nullopt when that
    /// version has no transaction file. Missing versions follow native errors.
    std::optional<Transaction> read_transaction_by_version(
        uint64_t version) const;

    /// Current version details, including native nanosecond timestamp and
    /// manifest-summary metadata. Timestamps must fit i64 nanoseconds.
    /// Throws lance::Error on failure.
    VersionDetails current_version() const;

    /// Full version-history details ordered by version id. Each entry contains
    /// native nanosecond precision and manifest-summary metadata. Timestamps
    /// must fit i64 nanoseconds; versions() retains millisecond precision.
    std::vector<VersionDetails> version_details() const;

    /// Lightweight attached-version refs ordered by version id.
    std::vector<uint64_t> version_refs() const;

    /// Check out a native ref as an independent dataset snapshot.
    Dataset checkout(const Ref& reference) const;

    /// Shallow clone this handle's snapshot into `target_path`.
    ///
    /// Native `Dataset::shallow_clone` publishes a metadata-only target that
    /// reuses the source base paths instead of copying data files.
    /// `reference == nullptr` clones the handle's current snapshot (branch and
    /// version id pinned); it does not select the latest head. Pass
    /// `Ref::version(std::nullopt, std::nullopt)` to clone the current head of
    /// the default branch. `storage_options == nullptr` passes native None,
    /// which inherits the source dataset-level store params as native shallow
    /// clone does; otherwise the accessor's native Arc is forwarded as-is when
    /// it holds static options or a provider (an accessor with neither passes
    /// None), without resolving or copying credentials.
    ///
    /// The target is resolved through the source handle's session registry.
    /// The returned dataset retains runtime source store params for its base
    /// paths, so it can read shared source files after the source handle is
    /// closed. Those params are not persisted: a later reopen needs
    /// `DatasetOpenOptions::base_store_params` when the default store
    /// configuration cannot read a base. A target with a published dataset is
    /// rejected before writes; later failures may leave partial files,
    /// including tagged index files, without rollback. No reopen, credential
    /// persistence, cloud guarantee, or custom ObjectStore API is added here.
    Dataset shallow_clone(
        const std::string& target_path,
        const Ref* reference = nullptr,
        const StorageOptionsAccessor* storage_options = nullptr) const;

    /// Deep clone this handle's snapshot into `target_path`.
    ///
    /// `reference == nullptr` clones the handle's current snapshot (branch and
    /// version id pinned); it does not select the latest head. Pass
    /// `Ref::version(std::nullopt, std::nullopt)` to clone the current head of
    /// the default branch. `storage_options == nullptr` passes native None;
    /// otherwise the accessor's native Arc is forwarded as-is when it holds
    /// static options or a provider (an accessor with neither passes None),
    /// without resolving or copying credentials. The returned dataset keeps
    /// the cloned snapshot's version id (native clone commit semantics), so
    /// `version()` is not necessarily 1. Copy or commit errors can leave the
    /// target's publication state uncertain; reopen it to check its state.
    /// Partial objects may remain under `target_path` and are not rolled back.
    /// The target URI must use a scheme known to the default object-store
    /// registry, because native clone commits resolve it there.
    /// For datasets with additional base paths, native deep clone reads them
    /// through the source dataset's object store; per-base distinct source
    /// credentials are not supported.
    Dataset deep_clone(
        const std::string& target_path,
        const Ref* reference = nullptr,
        const StorageOptionsAccessor* storage_options = nullptr) const;

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
            std::nullopt) const;

    /// Check out the latest version of a branch as an independent dataset.
    /// Pass "main" for the default branch.
    Dataset checkout_branch(const std::string& branch_name) const;

    /// JSON object keyed by branch name. Values are native BranchContents
    /// serde objects; map order is unspecified.
    std::string branches_json() const;

    /// JSON object for one native BranchContents value.
    std::string get_branch_json(const std::string& branch_name) const;

    /// JSON object for this dataset snapshot's native BranchIdentifier.
    std::string branch_identifier_json() const;

    /// Delete a branch. Child-branch and tag references surface as
    /// LANCE_ERR_COMMIT_CONFLICT.
    void delete_branch(const std::string& branch_name) const;

    /// Delete a branch while bypassing only a missing BranchContents. Native
    /// child-branch and tag reference checks still apply.
    void force_delete_branch(const std::string& branch_name) const;

    /// Replace all metadata entries on a branch. An empty vector writes an
    /// empty map; this is not a merge or NULL-delete operation.
    void replace_branch_metadata(
        const std::string& branch_name,
        const std::vector<std::pair<std::string, std::string>>& metadata) const;

    /// Snapshot all tags. Entry order is unspecified.
    std::vector<TagInfo> tags() const;

    /// Fetch one tag by name.
    TagInfo get_tag(const std::string& name) const;

    /// Create a tag from a native ref.
    void create_tag(const std::string& name, const Ref& reference) const;

    /// Update an existing tag to a native ref. Metadata is preserved natively.
    void update_tag(const std::string& name, const Ref& reference) const;

    /// Delete a tag.
    void delete_tag(const std::string& name) const;

    /// Replace all metadata entries on a tag. An empty map writes empty
    /// metadata; native reads treat absent and empty metadata the same.
    void replace_tag_metadata(
        const std::string& name,
        const std::vector<std::pair<std::string, std::string>>& metadata) const;

    /// Compute per-field data statistics (compressed on-disk byte size) for
    /// query planning, ordered by schema field id. Performs I/O over every
    /// fragment. Throws lance::Error on failure.
    std::vector<FieldStatistics> calculate_data_stats() const;

    /// Clean versions older than a relative duration through native
    /// `Dataset::cleanup_old_versions`. Lance core owns the timestamp cutoff.
    /// Throws lance::Error on failure.
    RemovalStats cleanup_old_versions(
        const CleanupOldVersionsOptions& options) const;

    /// Execute native manual cleanup with an explicit policy. Current versions,
    /// tags, referenced branches and unverified files follow Lance core.
    /// Throws lance::Error on failure.
    RemovalStats cleanup(
        const CleanupPolicy& policy,
        const CleanupOperationOptions& operation = {}) const;

    /// Explain cleanup without deleting files. The returned value owns copies
    /// of every native field and remains valid after this Dataset is closed.
    /// A later cleanup re-evaluates current references and storage state.
    /// Throws lance::Error on failure.
    CleanupExplanation explain_cleanup(
        const CleanupPolicy& policy,
        const CleanupOperationOptions& operation = {}) const;

    /// Validate native fragments, stable row IDs and indices. Throws
    /// lance::Error on failure.
    void validate() const;

    /// Count rows hidden by deletion files across all fragments.
    /// Throws lance::Error on failure.
    uint64_t count_deleted_rows() const;

    /// Count fragments with fewer physical rows than `max_rows_per_group`.
    /// Native `max_rows_per_group == 0` returns 0. Throws lance::Error only
    /// for binding input errors.
    uint64_t num_small_files(size_t max_rows_per_group) const;

    /// Commit a new manifest carrying the fragments of `version`, even when
    /// `version` is already the latest. The returned Dataset points at the
    /// newly committed version; this handle is unchanged.
    /// Throws lance::Error on failure.
    Dataset restore(uint64_t version) const;

    /// Delete rows matching the SQL `predicate`, committing a new manifest.
    /// Mutates this dataset in place; the handle continues to point at the
    /// new version. Returns the number of rows that were deleted.
    /// Throws lance::Error on failure (empty predicate, malformed SQL,
    /// commit conflict, ...).
    ///
    /// Named `delete_rows` to avoid the C++ `delete` keyword.
    uint64_t delete_rows(const std::string& predicate);

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
        const std::vector<std::pair<std::string, std::string>>& updates);

    /// One-shot native `Dataset::merge` left join. The two-argument overload
    /// maps an omitted `right_on` to `left_on`.
    ///
    /// A non-null `source` is consumed on return or throw; a null source is
    /// rejected without consumption. Native owns matching, schema union, and
    /// commit behavior. Existing scanners and independently opened handles
    /// retain their previous snapshots. Throws lance::Error on failure.
    void merge(ArrowArrayStream* source, const std::string& left_on);

    /// One-shot native `Dataset::merge` left join with distinct keys.
    ///
    /// Duplicate right-side join keys, including duplicate nulls, are rejected
    /// by native with LANCE_ERR_INVALID_ARGUMENT.
    /// Empty key strings are forwarded to native validation. Throws
    /// lance::Error on failure.
    void merge(
        ArrowArrayStream* source,
        const std::string& left_on,
        const std::string& right_on);

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
        const LanceMergeInsertParams* params = nullptr);

    /// Convenience: classic upsert (when_matched=UpdateAll, when_not_matched=InsertAll).
    LanceMergeInsertResult upsert(
        const std::vector<std::string>& on_columns,
        ArrowArrayStream* source);

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
        const std::vector<CompactedSsTable>& compacted_sstables);

    /// Compact small or deleted-heavy fragments into larger ones, committing
    /// a new manifest. A clean dataset is a no-op — all-zero metrics and the
    /// version is unchanged. Pass `nullptr` for upstream defaults.
    /// Throws lance::Error on failure (commit conflict, ...).
    LanceCompactionMetrics compact_files(
        const LanceCompactionOptions* options = nullptr);

    /// Compact using the complete native options surface. NULL selects
    /// `CompactionOptions::from_dataset_config`.
    LanceCompactionMetrics compact_files_with_options(
        const CompactionOptions* options = nullptr);

    /// Plan native compaction. NULL options select dataset config.
    CompactionPlan plan_compaction(
        const CompactionOptions* options = nullptr) const;

    /// Commit native rewrite results. NULL options select native defaults.
    LanceCompactionMetrics commit_compaction(
        const std::vector<RewriteResult>& results,
        const CompactionOptions* options = nullptr);

    /// Run native single-process index optimization. NULL uses native defaults.
    void optimize_indices(
        const OptimizeIndicesOptions* options = nullptr);

    /// Plan the native distributed index optimization workflow.
    OptimizeIndicesPlan plan_optimize_indices(
        const OptimizeIndicesOptions* options = nullptr) const;

    /// Commit merged native index segments.
    void commit_optimize_indices(
        const std::vector<IndexSegmentResult>& results,
        const OptimizeIndicesOptions* options = nullptr);

    /// Migrate manifest paths to native v2 naming through core.
    void migrate_manifest_paths_v2();

    /// Delete every row through native `Dataset::truncate_table`.
    void truncate_table();

    /// Drop columns from the dataset's schema and commit a new manifest.
    /// Metadata-only — data files remain until a later `compact_files()`
    /// call rewrites them. Mutates this dataset in place; the handle
    /// continues to point at the new version.
    ///
    /// `columns` must be non-empty. Throws lance::Error on failure (empty
    /// list, unknown column, attempt to drop every column, commit
    /// conflict, ...).
    void drop_columns(const std::vector<std::string>& columns);

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
    void alter_columns(const std::vector<ColumnAlteration>& alterations);

    /// Add columns computed from SQL expressions over the dataset's existing
    /// columns, committing a new manifest. `batch_size = 0` uses the upstream
    /// default scan batch size.
    ///
    /// `columns` must be non-empty and each entry's `name` and `expression`
    /// must be non-empty. Throws lance::Error on failure (empty list, empty
    /// name/expression, malformed SQL syntax, a reference to a non-existent
    /// column, name collision with an existing column, commit conflict, ...).
    void add_columns_sql(const std::vector<SqlColumn>& columns,
                         uint64_t batch_size = 0);

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
        std::optional<uint32_t> batch_size = std::nullopt);

    /// Add columns through the schema-aware shared BatchUDF adapter. Batch
    /// checkpoints are restored from the batch schema, preserving the native
    /// context marker; the array-only overload keeps its original behavior.
    void add_columns_batch_udf(
        const BatchUDFV2& udf,
        std::optional<std::vector<std::string>> read_columns = std::nullopt,
        std::optional<uint32_t> batch_size = std::nullopt);

    /// Add all-null columns described by an Arrow schema, committing a new
    /// manifest. Metadata-only on non-legacy datasets. Every field in `schema`
    /// must be nullable. The caller owns `schema` and must keep it alive for
    /// the duration of the call; the wrapper does not release it.
    ///
    /// Throws lance::Error on failure (invalid schema, non-nullable field, name
    /// collision with an existing column, commit conflict, ...). A legacy-format
    /// dataset throws with code `LANCE_ERR_NOT_SUPPORTED` (all-null columns are
    /// metadata-only and the legacy format cannot represent them that way).
    void add_columns_nulls(const ArrowSchema* schema);

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
    void add_columns_stream(ArrowArrayStream* stream, uint64_t batch_size = 0);

    /// Export the schema as an Arrow C Data Interface struct.
    void schema(ArrowSchema* out) const;

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
        std::optional<uint32_t> base_id = std::nullopt) const;

    /// Clone the current snapshot schema as an owned native value.
    LanceSchema native_schema() const;

    /// Take rows by indices. `out` is caller-owned and its non-null `release`
    /// must be called exactly once. Deferred iteration/cleanup panics are
    /// contained by the exported stream guard.
    void take(const uint64_t* indices, size_t num_indices,
              const std::vector<std::string>& columns,
              ArrowArrayStream* out) const;

    /// Take all columns with the same stream ownership as the overload above.
    void take(const uint64_t* indices, size_t num_indices,
              ArrowArrayStream* out) const;

    /// Take rows by indices and return the owning stream.
    ///
    /// The projection, empty-vector behavior and native validation are the
    /// same as the raw-out overload. The returned `ArrowStream` releases the
    /// native stream exactly once, on destruction or `reset()`; moving it
    /// transfers ownership and leaves the source empty. `out` from the raw-out
    /// overload remains caller-owned.
    ArrowStream take(const uint64_t* indices, size_t num_indices,
                     const std::vector<std::string>& columns) const;

    /// Take all columns by indices and return the owning stream.
    /// Ownership and validation follow the projecting overload above.
    ArrowStream take(const uint64_t* indices, size_t num_indices) const;

    /// Borrow indices from a contiguous range and return the owning stream.
    /// The range is read only during this call. Empty-range and projection
    /// behavior are inherited from the pointer/count overload above.
    ArrowStream take(std::span<const uint64_t> indices,
                     const std::vector<std::string>& columns) const;

    /// Borrow all-column indices from a contiguous range and return the
    /// owning stream. Ownership follows the overload above.
    ArrowStream take(std::span<const uint64_t> indices) const;

    /// Take rows by dataset row IDs. `out` is caller-owned and its non-null
    /// `release` must be called exactly once. Deferred iteration/cleanup panics
    /// are contained by the exported stream guard.
    void take_rows(const uint64_t* row_ids, size_t num_row_ids,
                   const std::vector<std::string>& columns,
                   ArrowArrayStream* out) const;

    /// Borrow row IDs from a contiguous range. Accepted sources include
    /// `std::span`, `std::vector`, `std::array` and C arrays; the range is only
    /// read during this call, so no copy or lifetime extension is required.
    /// An empty range, including one whose `data()` is null, produces an empty
    /// stream. `out` has the same ownership as the pointer/count overload.
    void take_rows(std::span<const uint64_t> row_ids,
                   const std::vector<std::string>& columns,
                   ArrowArrayStream* out) const;

    /// Take all columns by dataset row IDs with the same stream ownership as
    /// the overload above.
    void take_rows(const uint64_t* row_ids, size_t num_row_ids,
                   ArrowArrayStream* out) const;

    /// Borrow all-column row IDs with the same range lifetime, empty-range and
    /// stream-ownership rules as the span overload above.
    void take_rows(std::span<const uint64_t> row_ids,
                   ArrowArrayStream* out) const;

    /// Take rows by dataset row IDs and return the owning stream.
    ///
    /// The projection, empty-vector behavior and native validation are the
    /// same as the raw-out overload. The returned `ArrowStream` releases the
    /// native stream exactly once, on destruction or `reset()`; moving it
    /// transfers ownership and leaves the source empty. `out` from the raw-out
    /// overload remains caller-owned.
    ArrowStream take_rows(const uint64_t* row_ids, size_t num_row_ids,
                          const std::vector<std::string>& columns) const;

    /// Take all columns by dataset row IDs and return the owning stream.
    /// Ownership and validation follow the projecting overload above.
    ArrowStream take_rows(const uint64_t* row_ids, size_t num_row_ids) const;

    /// Borrow row IDs from a contiguous range and return the owning stream.
    /// The range is read only during this call. Empty-range and projection
    /// behavior are inherited from the pointer/count overload above.
    ArrowStream take_rows(std::span<const uint64_t> row_ids,
                          const std::vector<std::string>& columns) const;

    /// Borrow all-column row IDs from a contiguous range and return the
    /// owning stream. Ownership follows the overload above.
    ArrowStream take_rows(std::span<const uint64_t> row_ids) const;

    /// Take blob handles by dataset row ID; element `i` is for `row_ids[i]`,
    /// `std::nullopt` for a null blob value.
    std::vector<std::optional<BlobFile>> take_blobs(
        const uint64_t* row_ids, size_t num_row_ids,
        const std::string& column) const;

    /// Take blob handles by 0-based row index, with the same ownership and
    /// null-slot semantics as the overload above.
    std::vector<std::optional<BlobFile>> take_blobs_by_indices(
        const uint64_t* indices, size_t num_indices,
        const std::string& column) const;

    /// Take blob handles by physical row address; element `i` is for
    /// `row_addresses[i]`, `std::nullopt` for a null blob value. An empty
    /// selection still validates `column` through native.
    std::vector<std::optional<BlobFile>> take_blobs_by_addresses(
        const uint64_t* row_addresses, size_t num_row_addresses,
        const std::string& column) const;

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
        const BlobReadOptions& options = {}) const;

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
        const BlobReadOptions& options = {}) const;

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
    [[deprecated("Use prepare_fts_match_query() to select the Match operator")]] FtsQueryContext prepare_fts_query(
        const std::string& column,
        const std::string& query,
        uint32_t max_fuzzy_distance = 0,
        FtsCoverageMode coverage_mode = FtsCoverageMode::Strict) const;

    /// Prepare a Match query with a global BM25 scorer. AND and OR are
    /// supported. `max_fuzzy_distance` is reserved and currently must be zero;
    /// keeping it here avoids another API change when canonical cross-segment
    /// fuzzy vocabulary injection becomes available.
    FtsQueryContext prepare_fts_match_query(
        const std::string& column,
        const std::string& query,
        FtsMatchOperator match_operator = FtsMatchOperator::Or,
        uint32_t max_fuzzy_distance = 0,
        FtsCoverageMode coverage_mode = FtsCoverageMode::Strict) const;

    /// Prepare a Phrase query. Its FTS index must store token positions and
    /// slop must be non-negative.
    FtsQueryContext prepare_fts_phrase_query(
        const std::string& column,
        const std::string& query,
        int32_t slop = 0,
        FtsCoverageMode coverage_mode = FtsCoverageMode::Strict) const;

    /// Number of fragments in the dataset.
    uint64_t fragment_count() const;

    /// Get all fragment IDs.
    std::vector<uint64_t> fragment_ids() const;

    /// Resolve a read-only fragment snapshot by ID. The returned handle stays
    /// bound to this dataset version and remains valid after this Dataset is
    /// destroyed. A missing ID throws LANCE_ERR_NOT_FOUND.
    Fragment open_fragment(uint64_t fragment_id) const;

    /// Create a vector index on a column.
    void create_vector_index(const std::string& column,
                             const LanceVectorIndexParams& params,
                             const std::string& name = "",
                             bool replace = false);

    /// Create a vector index from legacy parameters with optional progress.
    ///
    /// A null callback disables observation. A non-null callback may run
    /// concurrently on internal runtime worker threads while this call holds
    /// the dataset mutation lock; it must be thread-safe, non-blocking, and
    /// must not access this dataset, call runtime-backed lance_* entry points,
    /// or start a nested runtime. Non-runtime leaf utilities are unaffected. The
    /// callback and context are borrowed only through this synchronous call
    /// and are retired before it returns. Native publication/reload errors can
    /// leave the returned state uncertain; reopen to inspect the version.
    void create_vector_index_with_progress(
        const std::string& column,
        const LanceVectorIndexParams& params,
        LanceIndexBuildProgressCallback callback,
        void* callback_ctx,
        const std::string& name = "",
        bool replace = false);

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

    /// Create a vector index from additive native options with optional
    /// progress. See create_vector_index_with_progress for the callback and
    /// publication-error contract.
    void create_vector_index_with_options_and_progress(
        const std::string& column,
        const VectorIndexOptions& options,
        LanceIndexBuildProgressCallback callback,
        void* callback_ctx,
        const std::string& name = "",
        bool replace = false);

    /// Create a scalar index on a column. An empty `params_json` selects the
    /// native plugin defaults (the wrapper passes NULL); a non-empty value is
    /// forwarded unchanged as JSON.
    void create_scalar_index(const std::string& column,
                             LanceScalarIndexType index_type,
                             const std::string& name = "",
                             const std::string& params_json = "",
                             bool replace = false);

    /// Create a scalar index with optional progress. A null callback disables
    /// observation; a non-null callback has the same synchronous lifetime and
    /// non-reentrancy rules as create_vector_index_with_progress. `params_json`
    /// follows create_scalar_index.
    void create_scalar_index_with_progress(
        const std::string& column,
        LanceScalarIndexType index_type,
        LanceIndexBuildProgressCallback callback,
        void* callback_ctx,
        const std::string& name = "",
        const std::string& params_json = "",
        bool replace = false);

    /// Create a scalar index with the additive native train control.
    /// `params_json` follows create_scalar_index: empty selects plugin
    /// defaults and a non-empty value is forwarded unchanged as JSON.
    void create_scalar_index_with_options(
        const std::string& column,
        LanceScalarIndexType index_type,
        const ScalarIndexOptions& options,
        const std::string& index_name = "",
        const std::string& params_json = "",
        bool replace = false);

    /// Create a scalar index with native train controls and optional progress.
    /// A null callback disables observation. The callback follows the same
    /// synchronous lifetime and non-reentrancy rules as
    /// create_vector_index_with_progress. `params_json` follows
    /// create_scalar_index.
    void create_scalar_index_with_options_and_progress(
        const std::string& column,
        LanceScalarIndexType index_type,
        const ScalarIndexOptions& options,
        LanceIndexBuildProgressCallback callback,
        void* callback_ctx,
        const std::string& index_name = "",
        const std::string& params_json = "",
        bool replace = false);

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
    void drop_index(const std::string& name);

    /// Synchronously prewarm all segments of an index in this snapshot.
    /// Uses the dataset's session index cache; entries remain evictable.
    /// Throws lance::Error on failure, including an absent or empty index name.
    void prewarm_index(const std::string& index_name) const;

    /// Prewarm selected segments with native FTS position controls.
    void prewarm_index_with_options(
        const std::string& index_name,
        const IndexPrewarmOptions& options = {}) const;

    /// Number of user indexes (excludes system indexes).
    uint64_t index_count() const;

    /// JSON array describing all user indexes.
    std::string list_indices_json() const;

    /// JSON array with one entry per non-system physical index segment.
    /// Each entry includes the recorded fragment coverage and physical index
    /// format version. `fragment_ids` is null for unknown coverage and empty
    /// for known-empty coverage; historical fragment IDs are reported as
    /// recorded. Throws lance::Error on failure.
    std::string index_segments_json() const;

    /// Merge distributed Inverted index files. Files only; does not commit.
    void merge_index_metadata(
        const std::array<uint8_t, 16>& index_uuid,
        const std::string& index_type,
        std::optional<size_t> batch_readhead = std::nullopt,
        LanceIndexBuildProgressCallback callback = nullptr,
        void* callback_ctx = nullptr) const;

    /// Merge existing uncommitted segments into one uncommitted segment.
    std::vector<uint8_t> merge_index_segments(
        const std::vector<std::vector<uint8_t>>& segment_metadata) const;

    /// Number of segments that make up a logical vector index.
    /// Throws lance::Error with code NotFound if the index does not exist.
    uint64_t index_segment_count(const std::string& index_name) const;

    /// UUIDs of the physical segments that make up a logical vector index.
    /// Each UUID is a 16-byte array (RFC 4122 layout). Used by distributed
    /// query engines to fan k-NN out across workers — see
    /// `Scanner::index_segments`.
    std::vector<std::array<uint8_t, 16>> index_segments(const std::string& index_name) const;

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
        const std::vector<std::vector<uint8_t>>& segment_metadata);

    /// Access the underlying C handle (does not transfer ownership).
    const LanceDataset* c_handle() const;

    friend class Fragment;

private:
    explicit Dataset(LanceDataset* ptr);
};

// ─── Dataset SQL ─────────────────────────────────────────────────────────────

/// Planned native SQL query result. Created by SqlQueryBuilder::build();
/// `into_stream` moves the query's native stream into a caller-owned
/// ArrowStream and consumes this object, so it must not be used afterwards.
class SqlQuery {
    Handle<LanceSqlQuery, lance_sql_query_close> handle_;

public:
    explicit SqlQuery(LanceSqlQuery* query);

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
    ArrowStream into_stream();
};

/// Builder mirroring the native `dataset::sql::SqlQueryBuilder`. All setters
/// return `*this`; a setter that fails (foreign conversion errors) preserves
/// the builder's prior state. `build` plans the query and consumes this
/// builder, so it must not be used afterwards.
class SqlQueryBuilder {
    Handle<LanceSqlQueryBuilder, lance_sql_query_builder_close> handle_;

public:
    explicit SqlQueryBuilder(LanceSqlQueryBuilder* builder);

    SqlQueryBuilder(SqlQueryBuilder&&) noexcept = default;
    SqlQueryBuilder& operator=(SqlQueryBuilder&&) noexcept = default;
    SqlQueryBuilder(const SqlQueryBuilder&) = delete;
    SqlQueryBuilder& operator=(const SqlQueryBuilder&) = delete;

    /// Name the table in the SELECT statement. The string is copied; an
    /// empty name is passed to native unchanged. The native default is
    /// "dataset".
    SqlQueryBuilder& table_name(const std::string& name);

    /// Ask the native planner to inject the `_rowid` system column where
    /// safe.
    SqlQueryBuilder& with_row_id(bool enabled = true);

    /// Ask the native planner to inject the `_rowaddr` system column
    /// where safe.
    SqlQueryBuilder& with_row_addr(bool enabled = true);

    /// Set how blob columns are materialized (LanceBlobHandling).
    SqlQueryBuilder& blob_handling(LanceBlobHandling handling);

    /// Set the native target output batch size (rows). The native builder
    /// validates the value's range on build.
    SqlQueryBuilder& batch_size(uint64_t rows);

    /// Set the native target output batch size in bytes.
    SqlQueryBuilder& batch_size_bytes(uint64_t bytes);

    /// Plan the query and consume this builder: the C entry point frees the
    /// builder on every return path, including errors, so the handle is
    /// released before the call and never closed again.
    SqlQuery build();
};

// ─── Dataset delta ───────────────────────────────────────────────────────────

/// Native version-diff handle. Reusable: `list_transactions` and every
/// `get_*` stream may be called repeatedly and in any order; each returned
/// stream owns its state after this (and the dataset) handle closes.
/// Row-change and deleted-ID streams require stable row IDs at both nonzero
/// endpoints; version 0 is the empty snapshot. Native rejection surfaces as
/// lance::Error with LANCE_ERR_INVALID_ARGUMENT when requesting a stream.
class DatasetDelta {
    Handle<LanceDatasetDelta, lance_dataset_delta_close> handle_;

public:
    explicit DatasetDelta(LanceDatasetDelta* delta);

    DatasetDelta(DatasetDelta&&) noexcept = default;
    DatasetDelta& operator=(DatasetDelta&&) noexcept = default;
    DatasetDelta(const DatasetDelta&) = delete;
    DatasetDelta& operator=(const DatasetDelta&) = delete;

    /// List the transactions committed between the delta's begin and end
    /// versions. Each element is an independently owned Transaction. Every
    /// element is adopted before the shared array guard releases the
    /// untouched remainder, so a mid-loop exception frees the rest exactly
    /// once.
    std::vector<Transaction> list_transactions() const;

    /// Stream the rows inserted across the delta's range into a returned
    /// `ArrowStream`. The stream owns the native state it needs, so it stays
    /// valid after this delta (and its dataset) handle closes. On error
    /// nothing is returned and this delta remains usable.
    ArrowStream get_inserted_rows() const;

    /// Stream the rows updated across the delta's range into a returned
    /// `ArrowStream`. The stream owns its native state and stays valid after
    /// this delta (and its dataset) handle closes. On error nothing is
    /// returned and this delta remains usable.
    ArrowStream get_updated_rows() const;

    /// Stream the rows upserted across the delta's range into a returned
    /// `ArrowStream`. The stream owns its native state and stays valid after
    /// this delta (and its dataset) handle closes. On error nothing is
    /// returned and this delta remains usable.
    ArrowStream get_upserted_rows() const;

    /// Stream the deleted row ids across the delta's range into a returned
    /// `ArrowStream`. The single column is the native stable row-id; native
    /// resolution, version-zero behavior and error stages are authoritative.
    /// The stream stays valid after this delta (and its dataset) handle
    /// closes. On error nothing is returned and this delta remains usable.
    ArrowStream get_deleted_row_ids() const;
};

/// Builder mirroring the native `dataset::delta::DatasetDeltaBuilder`. Range
/// combinations are validated synchronously on build. `build` consumes this
/// builder, so it must not be used afterwards.
class DatasetDeltaBuilder {
    Handle<LanceDatasetDeltaBuilder, lance_dataset_delta_builder_close> handle_;

public:
    explicit DatasetDeltaBuilder(LanceDatasetDeltaBuilder* builder);

    DatasetDeltaBuilder(DatasetDeltaBuilder&&) noexcept = default;
    DatasetDeltaBuilder& operator=(DatasetDeltaBuilder&&) noexcept = default;
    DatasetDeltaBuilder(const DatasetDeltaBuilder&) = delete;
    DatasetDeltaBuilder& operator=(const DatasetDeltaBuilder&) = delete;

    /// Compare against the named version. Native build rejects combining
    /// this with any explicit begin/end version or date.
    DatasetDeltaBuilder& compared_against_version(uint64_t version);

    /// Set the explicit begin version. Native build requires both begin and
    /// end versions, or a compared-against version.
    DatasetDeltaBuilder& begin_version(uint64_t version);

    /// Set the explicit end version.
    DatasetDeltaBuilder& end_version(uint64_t version);

    /// Set the begin date for date-mode ranges. Native build requires a
    /// begin date when an end date is provided. A timestamp the native
    /// chrono constructor cannot represent is rejected by this setter.
    DatasetDeltaBuilder& begin_timestamp(const LanceDeltaTimestamp& timestamp);

    /// Set the end date for date-mode ranges.
    DatasetDeltaBuilder& end_timestamp(const LanceDeltaTimestamp& timestamp);

    /// Build the delta and consume this builder: the C entry point frees the
    /// builder on every return path, including errors, so the handle is
    /// released before the call and never closed again. Native build is
    /// synchronous and validates the configured range combination.
    DatasetDelta build();
};













/// Result of Dataset::commit_batch. The dataset and the actual native merged
/// transaction are independently owned.
struct BatchCommitResult {
    Dataset dataset;
    Transaction transaction;

    BatchCommitResult(Dataset&& dataset_value, Transaction&& transaction_value);

    BatchCommitResult(BatchCommitResult&&) noexcept = default;
    BatchCommitResult& operator=(BatchCommitResult&&) noexcept = default;
    BatchCommitResult(const BatchCommitResult&) = delete;
    BatchCommitResult& operator=(const BatchCommitResult&) = delete;
};







// ─── Uncommitted index segments ──────────────────────────────────────────────

/// Move-only owner of an Arrow C Data Interface model array and schema.
class IndexModel {
    ArrowArray array_ = {};
    ArrowSchema schema_ = {};

    void reset() noexcept;

    friend class Dataset;
    friend class IvfModel;

    IndexModel() noexcept = default;

    IndexModel(ArrowArray array, ArrowSchema schema) noexcept;

public:
    /// Adopt caller-produced native Arrow model structs, transferring their
    /// release responsibility to this object. Both inputs are required and are
    /// cleared to an empty state only after a complete transfer. This allows
    /// arbitrary native model dtypes and shapes to be passed to
    /// VectorIndexOptions.
    static IndexModel adopt(ArrowArray* array, ArrowSchema* schema);

    ~IndexModel();

    IndexModel(IndexModel&& other) noexcept;
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

    ArrowArray* array() noexcept;
    const ArrowSchema* schema() const noexcept;
};

/// Move-only owner of a cloned native IVF model.
class IvfModel {
    Handle<LanceIvfModel, lance_ivf_model_free> handle_;

    explicit IvfModel(LanceIvfModel* model);
    friend class Dataset;

public:
    IvfModel(IvfModel&&) noexcept = default;
    IvfModel& operator=(IvfModel&&) noexcept = default;
    IvfModel(const IvfModel&) = delete;
    IvfModel& operator=(const IvfModel&) = delete;

    bool has_centroids() const;

    /// Owned Arrow centroids, or nullopt when the model has none.
    std::optional<IndexModel> centroids() const;

    uint64_t num_partitions() const;

    uint64_t dimension() const;

    /// Native partition size. An absent partition returns 0.
    uint64_t partition_size(uint64_t partition_id) const;

    const LanceIvfModel* c_handle() const;
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
                             const std::optional<uint64_t>& value);

    static void set_map(const std::map<std::string, std::string>& source,
                        std::vector<const char*>& keys,
                        std::vector<const char*>& values,
                        LanceStringMap& out);

public:
    explicit VectorIndexOptionsStorage(const VectorIndexOptions& options);

    VectorIndexOptionsStorage(const VectorIndexOptionsStorage&) = delete;
    VectorIndexOptionsStorage& operator=(const VectorIndexOptionsStorage&) =
        delete;

    const LanceVectorIndexOptions* get() const;
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
    explicit IndexSegmentMetadata(LanceIndexSegmentMetadata* metadata);

    IndexSegmentMetadata(IndexSegmentMetadata&&) noexcept = default;
    IndexSegmentMetadata& operator=(IndexSegmentMetadata&&) noexcept = default;
    IndexSegmentMetadata(const IndexSegmentMetadata&) = delete;
    IndexSegmentMetadata& operator=(const IndexSegmentMetadata&) = delete;

    /// Parse protobuf-encoded IndexMetadata bytes.
    static IndexSegmentMetadata parse(const uint8_t* bytes, size_t len);

    /// Vector overload for protobuf-encoded IndexMetadata bytes.
    static IndexSegmentMetadata parse(const std::vector<uint8_t>& bytes);

    std::array<uint8_t, 16> uuid() const;

    std::string name() const;

    uint64_t dataset_version() const;

    int32_t index_version() const;

    int32_t index_type() const;

    std::string index_details_type_url() const;

    std::vector<int32_t> field_ids() const;

    std::vector<uint32_t> fragment_ids() const;

    /// Clone the complete in-memory native metadata value.
    IndexSegmentMetadata clone() const;

    /// Clone with caller-supplied coverage. `std::nullopt` copies the source
    /// fragment-coverage option unchanged; an empty vector selects
    /// present-empty coverage. Every other native field is copied.
    IndexSegmentMetadata clone_with_fragment_ids(
        std::optional<std::vector<uint32_t>> fragment_ids) const;

    /// Encode the complete in-memory metadata as protobuf-encoded
    /// IndexMetadata bytes. The returned vector owns its storage.
    std::vector<uint8_t> encode() const;

    std::vector<int32_t> covering_field_ids() const;

    /// Native `index_details.value` bytes. Empty when the present Any value is
    /// empty; throws when `index_details` is absent.
    std::vector<uint8_t> index_details_value() const;

    std::vector<IndexSegmentFile> files() const;

    std::optional<IndexTimestamp> created_at() const;

    std::optional<uint32_t> base_id() const;

    const LanceIndexSegmentMetadata* c_handle() const;
};















/// Move-only builder for one uncommitted physical index segment.
class IndexSegmentBuilder {
    Handle<LanceIndexSegmentBuilder, lance_index_segment_builder_free> handle_;

public:
    explicit IndexSegmentBuilder(LanceIndexSegmentBuilder* builder);

    IndexSegmentBuilder(IndexSegmentBuilder&&) noexcept = default;
    IndexSegmentBuilder& operator=(IndexSegmentBuilder&&) noexcept = default;
    IndexSegmentBuilder(const IndexSegmentBuilder&) = delete;
    IndexSegmentBuilder& operator=(const IndexSegmentBuilder&) = delete;

    /// Execute the single-use builder without committing a dataset manifest.
    /// Native bytes are released even if error handling or vector allocation
    /// throws an exception.
    std::vector<uint8_t> execute_uncommitted();

    /// Register a non-null index-build progress callback. Must be called
    /// before execute_uncommitted; the builder is single-use. The callback is
    /// invoked from internal worker threads and may be called concurrently
    /// from parallel worker tasks, so it must be thread-safe, non-blocking,
    /// and must not access the indexed dataset or call runtime-backed lance_*
    /// entry points. Non-runtime leaf utilities are unaffected. Callbacks must
    /// return normally: unwinding or throwing can abort the host process.
    /// Invocations occur only while execute_uncommitted runs, and this is
    /// enforced: lance-c disables the callback and drains in-flight
    /// invocations before that call returns, including on error, so a worker
    /// task detached by core can never invoke it afterwards. The callback and
    /// the context (if non-null) must remain valid until execute_uncommitted
    /// returns. Progress reporting is advisory and cannot affect the build
    /// outcome.
    IndexSegmentBuilder& progress_callback(LanceIndexBuildProgressCallback callback,
                                           void* callback_ctx);

    LanceIndexSegmentBuilder* c_handle();
};































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

public:
    friend class Fragment;

    explicit Scanner(LanceScanner* s);

    /// Set the row limit.
    Scanner& limit(int64_t n);

    /// Set the row offset.
    Scanner& offset(int64_t n);

    /// Set the batch size.
    Scanner& batch_size(int64_t n);

    /// Set the target output batch size in bytes.
    Scanner& batch_size_bytes(uint64_t bytes);

    /// Set the scanner I/O buffer size in bytes, in the range [1, INT64_MAX].
    Scanner& io_buffer_size(uint64_t bytes);

    /// Set the number of batches decoded concurrently.
    Scanner& batch_readahead(size_t batches);

    /// Set fragment readahead for unordered scans.
    Scanner& fragment_readahead(size_t fragments);

    /// Set the target number of physical execution partitions.
    Scanner& target_parallelism(size_t partitions);

    /// Configure whether batches are returned in storage order.
    Scanner& scan_in_order(bool ordered = true);

    /// Configure whether scalar indices may be used to optimize filters.
    /// False also disables explicit scalar segment search, retaining its fragment domain.
    Scanner& use_scalar_index(bool enable = true);

    /// Configure whether row-based output batches are strict.
    Scanner& strict_batch_size(bool strict_batch_size = true);

    /// Configure whether file statistics may optimize the scan.
    Scanner& use_stats(bool use_stats = true);

    /// Clear the projection and scan all dataset columns (native unset).
    Scanner& project(std::nullopt_t);

    /// Replace the projection with ordered output-name / SQL-expression pairs.
    /// An empty vector is an explicit empty projection; use std::nullopt to
    /// restore the unset projection. Strings are copied during this call.
    Scanner& project(const std::vector<ProjectionColumn>& columns);

    /// Stop adding `_score` / `_distance` to explicit search projections.
    Scanner& disable_scoring_autoprojection();

    /// Replace the native result ordering. `std::nullopt` clears it; an empty
    /// vector is the explicit native empty ordering. Lance validates column
    /// names when the scan is materialized. Not supported with
    /// nearest_multivector(); that scan fails on start.
    Scanner& order_by(std::nullopt_t);

    Scanner& order_by(const std::vector<ColumnOrdering>& orderings);

    /// Choose how blob columns are materialized (default: descriptors for blob
    /// columns, bytes for every other binary column).
    Scanner& blob_handling(LanceBlobHandling handling);

    /// Configure native materialization for ordinary payload columns.
    /// For ALL_EARLY_EXCEPT, names are resolved by Lance, including nested paths.
    /// An empty list is an explicit empty exception list. See lance.h.
    Scanner& materialization_style(
        LanceMaterializationStyle style,
        const std::vector<std::string>& early_except_columns = {});

    /// Enable/disable row ID in output.
    Scanner& with_row_id(bool enable = true);

    /// Include or omit the `_rowaddr` metadata column.
    Scanner& with_row_address(bool enable = true);

    /// Configure whether deleted rows still present in storage are returned.
    /// Requires with_row_id(true); use_scalar_index(false) is needed for filtered scans.
    /// Incompatible with scalar_index_segment. See lance.h.
    Scanner& include_deleted_rows(bool include_deleted_rows = true);

    /// Generate exact candidates from one BTree/Bitmap/LabelList segment.
    /// fragment_ids is required and defines the complete read/fallback domain. See lance.h.
    /// Requires live rows only: vector/MinHash/FTS queries and
    /// include_deleted_rows(true) are rejected at stream creation.
    /// use_scalar_index(false) selects the scoped fallback without searching the segment.
    Scanner& scalar_index_segment(const std::array<uint8_t, 16>& segment_uuid);

    /// Enable bounded page-cost selection among BTree segments covering the read domain.
    /// Applies only to scalar_index_segment scans; fragment_ids remains required. Disabled by default.
    Scanner& scalar_index_auto_selection(bool enabled = true);

    Scanner& clear_scalar_index_segment();

    /// Restrict the scan/search domain to specific fragment IDs. For nearest
    /// queries, they limit candidates before Top-K regardless of prefilter().
    Scanner& fragment_ids(const uint64_t* ids, size_t len);

    /// Restrict the scan/search domain to specific fragment IDs (vector overload).
    Scanner& fragment_ids(const std::vector<uint64_t>& ids);

    /// Restrict the scan or search to rows whose `_rowid` is in `row_ids`
    /// (applied before ranking). `len == 0` allows no rows.
    Scanner& row_addr_allowlist(const uint64_t* row_ids, size_t len);

    /// Restrict the scan or search to rows whose `_rowid` is in `row_ids` (vector overload).
    Scanner& row_addr_allowlist(const std::vector<uint64_t>& row_ids);

    /// Exclude rows whose `_rowid` is in `row_ids` (applied before ranking).
    Scanner& row_addr_blocklist(const uint64_t* row_ids, size_t len);

    /// Exclude rows whose `_rowid` is in `row_ids` (vector overload).
    Scanner& row_addr_blocklist(const std::vector<uint64_t>& row_ids);

    /// Set a Substrait filter (serialized ExtendedExpression bytes).
    /// Wins over any SQL filter passed to the Scanner constructor.
    Scanner& substrait_filter(const uint8_t* bytes, size_t len);

    /// Set a Substrait filter (vector overload).
    Scanner& substrait_filter(const std::vector<uint8_t>& bytes);

    /// Set a Substrait aggregate Plan (serialized bytes).
    /// The plan is validated immediately; a failed replacement keeps the
    /// previous aggregate. Count and analyze-count replace it with COUNT(*).
    Scanner& substrait_aggregate(const uint8_t* bytes, size_t len);

    /// Set a Substrait aggregate Plan (vector overload).
    Scanner& substrait_aggregate(const std::vector<uint8_t>& bytes);

    /// Add an SQL filter that is combined with the selected primary filter using AND.
    Scanner& additional_sql_filter(const std::string& filter);

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
    Scanner& statistics_callback(LanceScanStatisticsCallback callback, void* callback_ctx);

    /// Restrict the next k-NN query to a subset of vector index segments.
    /// Pass `len` 16-byte UUIDs concatenated as a single byte buffer
    /// (total bytes = `len * 16`). Pass len=0 (and any pointer) to clear.
    Scanner& index_segments(const uint8_t* uuids, size_t len);

    /// Restrict the next k-NN query to a subset of vector index segments
    /// (typed vector overload).
    Scanner& index_segments(const std::vector<std::array<uint8_t, 16>>& uuids);

    /// Export the configured output schema from its actual native plan.
    /// Planning-only; does not start iteration or advance an existing stream.
    /// `out` is caller-owned; release it exactly once on success.
    void schema(ArrowSchema* out) const;

    /// Explain the configured native plan without executing it.
    std::string explain_plan(bool verbose = false) const;

    /// Execute and format the configured native plan with runtime metrics.
    /// Does not consume or advance an existing stream.
    std::string analyze_plan() const;

    /// Execute the native COUNT(*) plan for this scanner. Native aggregate
    /// planning rejects configured limit/offset/ordering. Throws lance::Error
    /// on failure.
    uint64_t count_rows() const;

    /// Execute and format the native COUNT(*) plan, not the ordinary plan.
    std::string analyze_count_plan() const;

    /// Materialize an independent ArrowArrayStream (blocking). The scanner remains valid.
    /// `out` is caller-owned; call its non-null `release` callback exactly once.
    void to_arrow_stream(ArrowArrayStream* out);

    /// Materialize an independent stream and return the owning handle.
    ///
    /// The returned `ArrowStream` releases the native stream exactly once, on
    /// destruction or `reset()`; moving it transfers ownership and leaves the
    /// source empty. The raw-out overload above keeps its caller-owned
    /// contract. The scanner remains valid.
    ArrowStream to_arrow_stream();

    /// Start an async scan with a non-null callback. On success, the callback's
    /// ArrowArrayStream result is library-allocated and must be passed exactly
    /// once to `lance::scanner_async_stream_free`, which also invokes `release`
    /// when necessary. The callback normally runs on the dispatcher thread,
    /// but a rare infrastructure fallback may invoke it on the calling or
    /// producing thread, possibly before this method returns, so it must be
    /// thread-safe. Exactly one completion is delivered; callback and non-null
    /// context storage must remain valid until it returns.
    void scan_async(LanceCallback callback, void* ctx) const;

    /// k-NN search (Float32 sugar).
    Scanner& nearest(const std::string& column, const float* q, size_t dim, uint32_t k);

    /// k-NN search over a contiguous Float32 query vector. The span length is
    /// used as the query dimension and must match the column.
    Scanner& nearest(const std::string& column, std::span<const float> q, uint32_t k);

    /// k-NN search (typed).
    Scanner& nearest(const std::string& column, const void* q, size_t dim,
                     LanceDataType dtype, uint32_t k);

    /// Restrict single-vector nearest results to [lower_bound, upper_bound).
    /// Call after nearest() and before scanning. Omitted bounds are unbounded;
    /// distance_range() clears both. Bounds must be finite and lower < upper
    /// when both are present. Uses metric distance units (squared L2 for L2).
    /// The nearest query's k cap and ANN candidate selection still apply.
    Scanner& distance_range(std::optional<float> lower_bound = std::nullopt,
                            std::optional<float> upper_bound = std::nullopt);

    /// Independent queries; each returns up to k rows tagged with query_index.
    Scanner& nearest_batch(const std::string& column, const void* query_data,
                           size_t dimension, size_t num_queries,
                           LanceDataType element_type, uint32_t k);

    /// Float32 batch convenience overload; input values are copied before returning.
    Scanner& nearest_batch(const std::string& column, const float* query_data,
                           size_t dimension, size_t num_queries, uint32_t k);

    /// Float32 batch sugar over a contiguous row-major matrix. The number of
    /// queries is derived from the span length, which must be a multiple of
    /// dimension. Values are copied before returning.
    Scanner& nearest_batch(const std::string& column, std::span<const float> queries,
                           size_t dimension, uint32_t k);

    /// One multi-vector query, copied from dimension * num_vectors row-major elements.
    Scanner& nearest_multivector(const std::string& column, const void* query_data,
                                size_t dimension, size_t num_vectors,
                                LanceDataType element_type, uint32_t k);

    /// Replace both minimum and maximum partition-search bounds.
    Scanner& nprobes(uint32_t nprobes);
    /// Replace only the minimum partition-search bound.
    Scanner& minimum_nprobes(uint32_t minimum_nprobes);
    /// Replace only the maximum partition-search bound.
    Scanner& maximum_nprobes(uint32_t maximum_nprobes);
    /// Set native continuous IVF search effort. Any explicit value, including
    /// 0.5, ignores fixed nprobes while explicit minimum/maximum remain.
    Scanner& search_effort(double effort);
    Scanner& approx_mode(LanceApproxMode approx_mode);
    Scanner& query_parallelism(int32_t parallelism);
    Scanner& refine_factor(uint32_t f);
    Scanner& ef(uint32_t e);
    Scanner& metric(LanceMetricType m);
    Scanner& use_index(bool enable);
    /// Use native indexed-only fast search for vector, scalar-filtered,
    /// full-text and MinHash queries. Index-backed paths can omit unindexed
    /// data. Ordinary scans and non-indexable expressions follow native
    /// planning. Disabled by default. See lance.h.
    Scanner& fast_search(bool enable = true);
    /// Apply scanner filters before vector/MinHash ranking when enabled, or
    /// refine ranked candidates afterward when disabled; postfiltering may
    /// return fewer than the configured limit. This does not change the fragment
    /// search domain.
    Scanner& prefilter(bool enable);

    /// BM25 full-text search.
    /// `columns` empty → search all FTS-indexed columns.
    /// `max_fuzzy_distance` 0 = exact; >0 = MatchQuery::with_fuzziness.
    Scanner& full_text_search(const std::string& query,
                              const std::vector<std::string>& columns = {},
                              uint32_t max_fuzzy_distance = 0);

    /// Attach an immutable typed FTS query. The scanner snapshots the tree, so
    /// the query and its source handles may be destroyed after success.
    Scanner& full_text_search(const FtsQuery& query);

    /// Native MinHash LSH text-similarity search. `column` must have a MinHash
    /// LSH scalar index and the scanner must have a limit. Results are ordered
    /// by native `_distance` (1 - estimated Jaccard similarity).
    Scanner& minhash_search(const std::string& column, const std::string& text);

    /// Attach a process-local prepared FTS query context. The scanner retains
    /// shared ownership, so the context object may be destroyed after success.
    Scanner& fts_query_context(const FtsQueryContext& context);

    /// Restrict a context-backed FTS query to a segment UUID subset.
    Scanner& fts_index_segments(const uint8_t* segment_uuids, size_t segment_count);

    Scanner& fts_index_segments(
        const std::vector<std::array<uint8_t, 16>>& segment_uuids);

    /// Access the underlying C handle.
    LanceScanner* c_handle();
};































// ─── Batch ───────────────────────────────────────────────────────────────────

class Batch {
    Handle<LanceBatch, lance_batch_free> handle_;

public:
    explicit Batch(LanceBatch* b);

    /// Adopt a non-NULL Arrow array. `schema` is borrowed and must outlive the
    /// call. The C API consumes the array on every return; on failure the
    /// source array is already released and this method throws.
    static Batch from_arrow(ArrowArray* array, const ArrowSchema* schema);

    /// Export the complete record-batch schema and top-level struct array.
    void to_arrow(ArrowArray* out_array, ArrowSchema* out_schema) const;

    const LanceBatch* c_handle() const;
};

/// Move-only owner of a staged fragment overlay writer.
///
/// A live writer is closed without finishing on destruction. `finish()` and
/// `abort()` release the C allocation before invoking consuming native calls.
class FragmentOverlayWriter {
    Handle<LanceFragmentOverlayWriter, lance_fragment_overlay_writer_close>
        handle_;

public:
    explicit FragmentOverlayWriter(LanceFragmentOverlayWriter* writer);

    FragmentOverlayWriter(FragmentOverlayWriter&&) noexcept = default;
    FragmentOverlayWriter& operator=(FragmentOverlayWriter&&) noexcept =
        default;
    FragmentOverlayWriter(const FragmentOverlayWriter&) = delete;
    FragmentOverlayWriter& operator=(const FragmentOverlayWriter&) = delete;

    /// Write one borrowed batch of overlay values.
    void write_batch(const Batch& batch);

    /// Finish the staged overlay. Returns `std::nullopt` when the native
    /// coverage was empty. The writer is consumed on every return.
    std::optional<DataOverlayGroup> finish();

    /// Discard staged data best-effort. The writer is consumed.
    void abort();

    const LanceFragmentOverlayWriter* c_handle() const;
};



// ─── Raw file ownership and snapshots ───────────────────────────────────────

/// Move-only owner of an Arrow C Data Interface schema.
class ArrowSchemaOwner {
    ArrowSchema schema_{};
    bool active_ = false;

public:
    ArrowSchemaOwner() noexcept = default;
    explicit ArrowSchemaOwner(ArrowSchema schema) noexcept;
    ~ArrowSchemaOwner();

    ArrowSchemaOwner(const ArrowSchemaOwner&) = delete;
    ArrowSchemaOwner& operator=(const ArrowSchemaOwner&) = delete;

    ArrowSchemaOwner(ArrowSchemaOwner&& other) noexcept;

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

FileBufferDescriptor buffer_descriptor(
    const LanceBufferDescriptor& descriptor);

FileEncodingDescriptor encoding_descriptor(
    const LanceEncodingDescriptor& descriptor);

LanceFileReaderOptions reader_options(
    const FileReaderOptions& options,
    std::vector<const char*>& columns);

LanceFileWriterOptions writer_options(
    const FileWriterOptions& options);

}  // namespace file_detail

class FileMetadata {
    Handle<LanceFileMetadata, lance_file_metadata_close> handle_;

public:
    explicit FileMetadata(LanceFileMetadata* metadata);

    FileMetadataInfo info() const;

    ArrowSchemaOwner schema() const;

    std::vector<FileBufferDescriptor> global_buffers() const;

    std::vector<FileColumnMetadata> columns() const;
};

class FileStatistics {
    Handle<LanceFileStatistics, lance_file_statistics_close> handle_;

public:
    explicit FileStatistics(LanceFileStatistics* statistics);

    std::vector<FileColumnStatistics> columns() const;
};

class ByteBuffer {
    Handle<LanceByteBuffer, lance_byte_buffer_close> handle_;

public:
    explicit ByteBuffer(LanceByteBuffer* buffer);

    std::vector<uint8_t> bytes() const;
};

class PathList {
    Handle<LancePathList, lance_path_list_close> handle_;

public:
    explicit PathList(LancePathList* list);

    std::vector<std::string> paths() const;
};

class DelimitedList {
    Handle<LanceDelimitedList, lance_delimited_list_close> handle_;

public:
    explicit DelimitedList(LanceDelimitedList* list);

    std::vector<std::string> common_prefixes() const;

    std::vector<std::string> objects() const;
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
        std::optional<FileSchedulerConfig> scheduler_config = std::nullopt);

    FileSession(FileSession&&) noexcept = default;
    FileSession& operator=(FileSession&&) noexcept = default;
    FileSession(const FileSession&) = delete;
    FileSession& operator=(const FileSession&) = delete;

    void reset() noexcept;
    explicit operator bool() const noexcept;

    std::string base_path() const;

    FileReader open_reader(
        const std::string& relative_path,
        const FileReaderOptions& options = {}) const;
    FileWriter open_writer(
        const std::string& relative_path,
        const ArrowSchema* schema,
        const FileWriterOptions& options = {}) const;

    PackedBlobWriter open_packed_blob_writer(
        const std::string& data_file_path,
        uint32_t blob_id) const;

    DedicatedBlobWriter open_dedicated_blob_writer(
        const std::string& data_file_path,
        uint32_t blob_id) const;

    bool contains(const std::string& relative_path) const;

    PathList list(std::optional<std::string> relative_path = std::nullopt) const;

    DelimitedList list_with_delimiter(
        std::optional<std::string> relative_path = std::nullopt) const;

    ByteBuffer read_range(
        const std::string& relative_path,
        uint64_t offset,
        uint64_t length) const;

    void delete_file(const std::string& relative_path) const;

    void upload_file(
        const std::string& local_path,
        const std::string& relative_remote_path) const;

    void download_file(
        const std::string& relative_remote_path,
        const std::string& local_path) const;

    LanceScanStats scheduler_stats() const;

    LanceObjectStoreRegistryStats registry_stats() const;

    FileStoreInfo store_info() const;

    LanceObjectStoreIoStats store_io_stats() const;
};

class FileReader {
    Handle<LanceFileReader, lance_file_reader_close> handle_;

public:
    explicit FileReader(LanceFileReader* reader);
    FileReader(FileReader&&) noexcept = default;
    FileReader& operator=(FileReader&&) noexcept = default;
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    void reset() noexcept;
    explicit operator bool() const noexcept;

    uint64_t num_rows() const;

    FileMetadata metadata() const;

    FileStatistics statistics() const;

    ByteBuffer read_global_buffer(uint32_t index) const;

    /// `batch_size` and `batch_readahead` must be greater than zero. A zero
    /// value throws `lance::Error` with LANCE_ERR_INVALID_ARGUMENT.
    ArrowStream read_all(
        uint32_t batch_size = 1024,
        uint32_t batch_readahead = 4) const;

    /// `batch_size` and `batch_readahead` must be greater than zero. A zero
    /// value throws `lance::Error` with LANCE_ERR_INVALID_ARGUMENT.
    ArrowStream read_range(
        uint64_t offset,
        uint64_t num_rows,
        uint32_t batch_size = 1024,
        uint32_t batch_readahead = 4) const;

    /// `batch_size` and `batch_readahead` must be greater than zero. A zero
    /// value throws `lance::Error` with LANCE_ERR_INVALID_ARGUMENT.
    ArrowStream read_indices(
        const std::vector<uint32_t>& indices,
        uint32_t batch_size = 1024,
        uint32_t batch_readahead = 4) const;

    LanceScanStats io_stats() const;
};

class FileWriter {
    Handle<LanceFileWriter, lance_file_writer_close> handle_;

public:
    explicit FileWriter(LanceFileWriter* writer);
    FileWriter(FileWriter&&) noexcept = default;
    FileWriter& operator=(FileWriter&&) noexcept = default;
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;

    void reset() noexcept;
    explicit operator bool() const noexcept;

    void write_batch(const Batch& batch);

    void add_schema_metadata(
        const std::string& key,
        const std::string& value);

    uint32_t add_global_buffer(const std::vector<uint8_t>& bytes);

    FileWriteSummary finish();

    void abort();
};






// ─── Fragment writer (free functions) ────────────────────────────────────────


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
void write_fragments(
    const std::string& uri,
    const ArrowSchema* schema,
    ArrowArrayStream* stream,
    const std::vector<std::pair<std::string, std::string>>& storage_opts = {});

// ─── Native namespaces ───────────────────────────────────────────────────────

namespace namespace_detail {

std::string json_escape(std::string_view value);

std::string json_string(std::string_view value);

std::string json_string_array(const std::vector<std::string>& values);

std::string json_string_object(
    const std::vector<std::pair<std::string, std::string>>& entries);

void free_owned_string(LanceOwnedString* value) noexcept;

void set_owned_string(LanceOwnedString* out, std::string value);

void set_callback_error(
    LanceCallbackError* error, int32_t code, const std::string& message) noexcept;

} // namespace namespace_detail

/// Namespace operation error that preserves both the generic C error class and
/// the native NamespaceError code/message.
class NamespaceError : public Error {
public:
    bool has_native_code;
    uint32_t native_code;
    std::string native_message;

    NamespaceError(
        LanceErrorCode code_value,
        std::string message_value,
        uint32_t native_code_value,
        std::string native_message_value);
};

/// Owning copy of one native namespace call result. The C result is released
/// before this object is returned, so no native resources outlive it.
class NamespaceResult {
    std::optional<std::string> json_;
    std::vector<uint8_t> bytes_;

    friend class Namespace;

    NamespaceResult(std::optional<std::string> json, std::vector<uint8_t> bytes);

public:
    NamespaceResult() = default;
    NamespaceResult(NamespaceResult&&) noexcept = default;
    NamespaceResult& operator=(NamespaceResult&&) noexcept = default;
    NamespaceResult(const NamespaceResult&) = delete;
    NamespaceResult& operator=(const NamespaceResult&) = delete;

    bool has_json() const noexcept;
    const std::optional<std::string>& json() const noexcept;
    const std::vector<uint8_t>& bytes() const noexcept;
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
    explicit DynamicContextProvider(Callback callback);
    const Callback& callback() const noexcept;
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

    struct CustomDispatchContext {
        std::function<std::string(const std::string&, const std::string&)> dispatch;
    };

    static void context_provider_release(void* ctx) noexcept;

    static void custom_client_release(void* ctx) noexcept;

    static void custom_dispatch_release(void* ctx) noexcept;

    static int32_t context_provider_call(
        void* raw_ctx,
        const char* operation,
        const char* object_id,
        LanceOwnedString* out_json,
        LanceCallbackError* out_error) noexcept;

    template <typename Callback>
    static int32_t invoke_custom(
        Callback&& callback,
        const char* request_json,
        LanceOwnedString* out_json,
        LanceCallbackError* out_error) noexcept {
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
        void* ctx, const char* req, LanceOwnedString* out, LanceCallbackError* err) noexcept;
    static int32_t custom_describe_table_version(
        void* ctx, const char* req, LanceOwnedString* out, LanceCallbackError* err) noexcept;
    static int32_t custom_create_table_version(
        void* ctx, const char* req, LanceOwnedString* out, LanceCallbackError* err) noexcept;
    static int32_t custom_list_table_versions(
        void* ctx, const char* req, LanceOwnedString* out, LanceCallbackError* err) noexcept;
    static int32_t custom_dispatch_call(
        void* ctx,
        const char* operation,
        const char* req,
        LanceOwnedString* out,
        LanceCallbackError* err) noexcept;

    static LanceNamespaceContextProviderSpec make_context_spec(
        const DynamicContextProvider* provider);

    explicit Namespace(LanceNamespace* handle);

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

    /// Foreign client using one callback for the same four managed-version
    /// operations. The callback receives the exact native snake_case operation
    /// name and native request JSON; both strings are borrowed for the call
    /// only. Other operations keep native Unsupported.
    struct CustomDispatchSpec {
        std::function<std::string(const std::string&, const std::string&)> dispatch;
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
        const DynamicContextProvider* context_provider = nullptr);

    static Namespace rest(
        const std::vector<std::pair<std::string, std::string>>& properties,
        const DynamicContextProvider* context_provider = nullptr);

    static Namespace connect(
        const std::string& impl_name,
        const std::vector<std::pair<std::string, std::string>>& properties = {},
        const Session* session = nullptr,
        const DynamicContextProvider* context_provider = nullptr);

    static Namespace custom(const CustomClientSpec& spec);

    static Namespace custom_dispatch(const CustomDispatchSpec& spec);

    std::string id() const;

    std::string ops_metrics_json() const;

    void reset_ops_metrics();

    NamespaceResult call(
        const std::string& operation,
        const std::string& request_json = "{}",
        const uint8_t* request_bytes = nullptr,
        size_t request_bytes_len = 0) const;

    NamespaceResult call(
        const std::string& operation,
        const std::string& request_json,
        const std::vector<uint8_t>& request_bytes) const;

    const LanceNamespace* c_handle() const noexcept;
};



/// Move-only owner of a running native REST adapter.
class RestAdapter {
    Handle<LanceRestAdapter, lance_rest_adapter_close> handle_;
    uint16_t port_ = 0;

    explicit RestAdapter(LanceRestAdapter* raw);

public:
    RestAdapter(RestAdapter&& other) noexcept;
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
        uint16_t port = 0);

    uint16_t port() const noexcept;

    void shutdown();
};








// ─── MemWAL out-of-line definitions ──────────────────────────────────────────

/// One Arrow schema plus a materialized batch list.
struct Table {
    ArrowSchemaOwner schema;
    std::vector<Batch> batches;
};












































// ─── Native update and rewrite value definitions ────────────────────────────





















































}  // namespace lance
