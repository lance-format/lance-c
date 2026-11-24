/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the dataset domain.
#pragma once

#include "lance/detail/declarations.hpp"
#include "lance/detail/inline/base.hpp"
#include "lance/detail/inline/udf.hpp"
#include "lance/detail/inline/scanner.hpp"
#include "lance/detail/inline/storage.hpp"
#include "lance/detail/inline/session.hpp"
#include "lance/detail/inline/lock.hpp"
#include "lance/detail/inline/write.hpp"

namespace lance {

inline Dataset::ArrowStreamInput::ArrowStreamInput(ArrowArrayStream* stream) : stream_(stream) {
            if (!stream_) {
                throw Error(LANCE_ERR_INVALID_ARGUMENT,
                            "stream must not be null");
            }
        }

inline Dataset::ArrowStreamInput::~ArrowStreamInput() {
            if (schema_.release) schema_.release(&schema_);
            if (armed_ && stream_ && stream_->release) {
                stream_->release(stream_);
            }
        }

inline const ArrowSchema* Dataset::ArrowStreamInput::read_schema() {
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

inline ArrowArrayStream* Dataset::ArrowStreamInput::consume() noexcept {
            armed_ = false;
            return stream_;
        }

inline std::vector<const char*> Dataset::to_c_key_values(
        const std::vector<std::pair<std::string, std::string>>& entries) {
        std::vector<const char*> kv;
        for (auto& [k, v] : entries) {
            kv.push_back(k.c_str());
            kv.push_back(v.c_str());
        }
        kv.push_back(nullptr);
        return kv;
    }

inline LanceWriteOptions Dataset::to_c_write_options(
        const WriteOptions& options,
        LanceAutoCleanupParams& cleanup) {
        return lance::to_c_write_options(options, cleanup);
    }

inline LanceUncommittedWriteOptions Dataset::to_c_uncommitted_write_options(
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

inline LanceCommitOptions Dataset::to_c_commit_options(const CommitOptions& options) {
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

inline RemovalStats Dataset::from_c_removal_stats(const LanceRemovalStats& stats) {
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

inline LanceCleanupPolicy Dataset::to_c_cleanup_policy(
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

inline LanceCleanupOperationOptions Dataset::to_c_cleanup_operation(
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

inline CleanupExplanation Dataset::read_cleanup_explanation(
        const LanceCleanupExplanation* explanation) {
        CleanupExplanation out;
        out.read_version =
            lance_cleanup_explanation_read_version(explanation);
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();

        LanceRemovalStats stats{};
        if (lance_cleanup_explanation_stats(explanation, &stats) != 0)
            detail::throw_error();
        out.stats = from_c_removal_stats(stats);

        const uint64_t candidate_count =
            lance_cleanup_explanation_candidate_count(explanation);
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        out.candidate_files.reserve(static_cast<size_t>(candidate_count));
        for (uint64_t i = 0; i < candidate_count; ++i) {
            const auto index = static_cast<size_t>(i);
            CleanupCandidateFile candidate;
            const char* path =
                lance_cleanup_explanation_candidate_path_at(explanation, index);
            if (!path) detail::throw_error();
            candidate.path = path;

            const int32_t kind =
                lance_cleanup_explanation_candidate_kind_at(explanation, index);
            if (kind < 0) detail::throw_error();
            candidate.kind = static_cast<CleanupFileKind>(kind);
            const int32_t unverified =
                lance_cleanup_explanation_candidate_unverified_at(
                    explanation, index);
            if (unverified < 0) detail::throw_error();
            candidate.unverified = unverified != 0;
            candidate.size_bytes =
                lance_cleanup_explanation_candidate_size_bytes_at(
                    explanation, index);
            if (lance_last_error_code() != LANCE_OK) detail::throw_error();
            out.candidate_files.push_back(std::move(candidate));
        }

        const int32_t truncated =
            lance_cleanup_explanation_candidate_files_truncated(explanation);
        if (truncated < 0) detail::throw_error();
        out.candidate_files_truncated = truncated != 0;
        out.candidate_file_limit =
            lance_cleanup_explanation_candidate_file_limit(explanation);
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();

        const uint64_t branch_count =
            lance_cleanup_explanation_referenced_branch_count(explanation);
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        out.referenced_branches.reserve(static_cast<size_t>(branch_count));
        for (uint64_t i = 0; i < branch_count; ++i) {
            const auto index = static_cast<size_t>(i);
            CleanupReferencedBranch branch;
            const char* name =
                lance_cleanup_explanation_referenced_branch_name_at(
                    explanation, index);
            if (!name) detail::throw_error();
            branch.name = name;
            branch.referenced_version =
                lance_cleanup_explanation_referenced_branch_version_at(
                    explanation, index);
            if (lance_last_error_code() != LANCE_OK) detail::throw_error();
            const int32_t candidate =
                lance_cleanup_explanation_referenced_branch_cleanup_candidate_at(
                    explanation, index);
            if (candidate < 0) detail::throw_error();
            branch.cleanup_candidate = candidate != 0;
            out.referenced_branches.push_back(std::move(branch));
        }

        const uint64_t warning_count =
            lance_cleanup_explanation_warning_count(explanation);
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        out.warnings.reserve(static_cast<size_t>(warning_count));
        for (uint64_t i = 0; i < warning_count; ++i) {
            const char* warning = lance_cleanup_explanation_warning_at(
                explanation, static_cast<size_t>(i));
            if (!warning) detail::throw_error();
            out.warnings.emplace_back(warning);
        }
        return out;
    }

inline VersionDetails Dataset::read_version(const LanceVersion* version) {
        VersionDetails out;
        out.id = lance_version_id(version);
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        out.timestamp_ns = lance_version_timestamp_ns(version);
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();

        uint64_t count = lance_version_metadata_count(version);
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        for (uint64_t i = 0; i < count; ++i) {
            const char* key = lance_version_metadata_key_at(
                version, static_cast<size_t>(i));
            if (!key) detail::throw_error();
            const char* value = lance_version_metadata_value_at(
                version, static_cast<size_t>(i));
            if (!value) detail::throw_error();
            out.metadata.emplace(key, value);
        }
        return out;
    }

inline TagInfo Dataset::read_tag(const LanceTag* tag) {
        TagInfo out;
        const char* name = lance_tag_name(tag);
        if (!name) detail::throw_error();
        out.name = name;

        out.version = lance_tag_version(tag);
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        out.manifest_size = lance_tag_manifest_size(tag);
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();

        const char* branch = nullptr;
        int32_t has_branch = lance_tag_branch(tag, &branch);
        if (has_branch < 0) detail::throw_error();
        if (has_branch) {
            if (!branch) {
                throw Error(LANCE_ERR_INTERNAL,
                            "lance_tag_branch reported a present branch with a null pointer");
            }
            out.branch = std::string(branch);
        }

        int64_t timestamp = 0;
        int32_t has_created = lance_tag_created_at_ns(tag, &timestamp);
        if (has_created < 0) detail::throw_error();
        if (has_created) out.created_at_ns = timestamp;

        timestamp = 0;
        int32_t has_updated = lance_tag_updated_at_ns(tag, &timestamp);
        if (has_updated < 0) detail::throw_error();
        if (has_updated) out.updated_at_ns = timestamp;

        uint64_t metadata_count = lance_tag_metadata_count(tag);
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        for (uint64_t i = 0; i < metadata_count; ++i) {
            const char* key = lance_tag_metadata_key_at(
                tag, static_cast<size_t>(i));
            if (!key) detail::throw_error();
            const char* value = lance_tag_metadata_value_at(
                tag, static_cast<size_t>(i));
            if (!value) detail::throw_error();
            out.metadata.emplace(key, value);
        }
        return out;
    }

inline void Dataset::adopt_blobs(const std::vector<LanceBlobFile*>& raw,
                            std::vector<std::optional<BlobFile>>& blobs) {
        for (auto* blob : raw) {
            if (blob) {
                blobs.emplace_back(BlobFile(blob));
            } else {
                blobs.emplace_back(std::nullopt);
            }
        }
    }

inline std::string Dataset::run_metadata_update_json(
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
        return detail::take_string(json);
    }

inline void Dataset::run_field_metadata_update(
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
            detail::throw_error();
        }
    }

inline void Dataset::tracked_files_progress_trampoline(
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

inline void Dataset::tracked_files_progress_release(
        void* callback_ctx) noexcept {
        delete static_cast<TrackedFilesProgressCapture*>(callback_ctx);
    }

inline Dataset Dataset::write_impl(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const LanceWriteParams* legacy_params,
        const LanceWriteOptions* native_options,
        const std::vector<std::pair<std::string, std::string>>& storage_opts,
        const StorageOptionsAccessor* storage_options_accessor) {

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
        if (rc != 0) detail::throw_error();
        // Defensive null guard: a conforming Rust impl never returns rc == 0
        // with `out == nullptr`, but constructing a Dataset around a null
        // handle would silently crash on the first method call. Throw
        // explicitly rather than going through `detail::throw_error()` because the
        // thread-local code is `LANCE_OK` on this path (rc == 0).
        if (!out) {
            throw Error(LANCE_ERR_INTERNAL,
                        std::string(entry_point) +
                            " returned success with null out_dataset");
        }
        return Dataset(out);
    }

inline Dataset Dataset::open(
        const std::string& uri,
        const std::vector<std::pair<std::string, std::string>>& storage_opts,
        uint64_t version) {

        auto kv = to_c_key_values(storage_opts);
        const char* const* opts_ptr =
            storage_opts.empty() ? nullptr : kv.data();

        auto* ds = lance_dataset_open(uri.c_str(), opts_ptr, version);
        if (!ds) detail::throw_error();
        return Dataset(ds);
    }

inline Dataset Dataset::open_with_session(
        const Session& session,
        const std::string& uri,
        const std::vector<std::pair<std::string, std::string>>& storage_opts,
        uint64_t version) {

        auto kv = to_c_key_values(storage_opts);
        const char* const* opts_ptr =
            storage_opts.empty() ? nullptr : kv.data();

        auto* ds = lance_dataset_open_with_session(
            uri.c_str(), opts_ptr, version, session.c_handle());
        if (!ds) detail::throw_error();
        return Dataset(ds);
    }

inline Dataset Dataset::open_with_options(
        const std::string& uri,
        const DatasetOpenOptions& options,
        const Session* session) {

        LanceDatasetOpenOptions c_options = {};
        c_options.version = options.version;
        c_options.storage_options_accessor =
            options.storage_options_accessor
                ? options.storage_options_accessor->c_handle()
                : nullptr;
        native_write_options_detail::BaseStoreParamsHolder base_store;
        native_write_options_detail::build_base_store_params(
            options.base_store_params, base_store);
        auto* ds = lance_dataset_open_with_base_store_params(
            uri.c_str(),
            &c_options,
            session ? session->c_handle() : nullptr,
            options.base_store_params ? &base_store.list : nullptr);
        if (!ds) detail::throw_error();
        return Dataset(ds);
    }

inline Dataset Dataset::open_with_commit_lock(
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
        if (rc != 0) detail::throw_error();
        if (ds == nullptr) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_dataset_open_with_commit_lock returned success "
                        "with a null dataset");
        }
        return Dataset(ds);
    }

inline Dataset Dataset::write(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const std::vector<std::pair<std::string, std::string>>& storage_opts) {

        return write(uri, stream, mode, WriteParams{}, storage_opts);
    }

inline Dataset Dataset::write(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const WriteParams& params,
        const std::vector<std::pair<std::string, std::string>>& storage_opts) {

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

inline Dataset Dataset::write_with_options(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const WriteOptions& options,
        const std::vector<std::pair<std::string, std::string>>& storage_opts) {

        LanceAutoCleanupParams c_cleanup = {};
        LanceWriteOptions c_options = to_c_write_options(options, c_cleanup);

        return write_impl(
            uri, stream, mode, nullptr, &c_options, storage_opts);
    }

inline Dataset Dataset::write_with_accessor(
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

inline Dataset Dataset::write_with_params_and_accessor(
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

inline Dataset Dataset::write_with_options_and_accessor(
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

inline Dataset Dataset::write_with_options_with_commit_lock(
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
            detail::throw_error();
        }
        if (!out) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_dataset_write_with_options_with_commit_lock returned "
                "success with null out_dataset");
        }
        return Dataset(out);
    }

inline Dataset Dataset::write_with_native_options(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const NativeWriteOptions& options) {

        ArrowStreamInput input(stream);
        const ArrowSchema* schema = input.read_schema();
        ArrowArrayStream* owned_stream = input.consume();
        LanceDataset* out = nullptr;
        if (lance_dataset_write_with_native_options(
                uri.c_str(),
                schema,
                owned_stream,
                static_cast<int32_t>(mode),
                options.c_handle(),
                &out) != 0) {
            detail::throw_error();
        }
        if (!out) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_dataset_write_with_native_options returned success "
                "with null out_dataset");
        }
        return Dataset(out);
    }

inline Transaction Dataset::write_uncommitted_with_native_options(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const NativeWriteOptions& options) {

        ArrowStreamInput input(stream);
        const ArrowSchema* schema = input.read_schema();
        ArrowArrayStream* owned_stream = input.consume();
        LanceTransaction* transaction = nullptr;
        if (lance_dataset_write_uncommitted_stream_with_native_options(
                uri.c_str(),
                schema,
                owned_stream,
                static_cast<int32_t>(mode),
                options.c_handle(),
                &transaction) != 0) {
            detail::throw_error();
        }
        if (!transaction) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_dataset_write_uncommitted_stream_with_native_options "
                "returned success with null out_transaction");
        }
        return Transaction(transaction);
    }

inline Transaction Dataset::write_uncommitted(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const UncommittedWriteOptions& options,
        const std::vector<std::pair<std::string, std::string>>& storage_opts,
        const Session* session) {

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
            detail::throw_error();
        }
        if (!transaction) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_dataset_write_uncommitted_stream returned success with "
                "null out_transaction");
        }
        return Transaction(transaction);
    }

inline Transaction Dataset::write_uncommitted_with_accessor(
        const std::string& uri,
        ArrowArrayStream* stream,
        WriteMode mode,
        const StorageOptionsAccessor& storage_options_accessor,
        const UncommittedWriteOptions& options,
        const Session* session) {

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
            detail::throw_error();
        }
        if (!transaction) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_dataset_write_uncommitted_stream_with_accessor "
                "returned success with null out_transaction");
        }
        return Transaction(transaction);
    }

inline Transaction Dataset::write_uncommitted_with_commit_lock(
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
            detail::throw_error();
        }
        if (!transaction) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_dataset_write_uncommitted_stream_with_commit_lock "
                "returned success with null out_transaction");
        }
        return Transaction(transaction);
    }

inline Dataset Dataset::commit(
        const std::string& uri,
        const Transaction& transaction,
        const CommitOptions& options,
        const std::vector<std::pair<std::string, std::string>>& storage_opts,
        const Session* session) {

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
            detail::throw_error();
        }
        if (!dataset) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_transaction_commit returned success with null "
                        "out_dataset");
        }
        return Dataset(dataset);
    }

inline Dataset Dataset::commit_with_accessor(
        const std::string& uri,
        const Transaction& transaction,
        const StorageOptionsAccessor& storage_options_accessor,
        const CommitOptions& options,
        const Session* session) {

        LanceCommitOptions c_options = to_c_commit_options(options);
        LanceDataset* dataset = nullptr;
        if (lance_transaction_commit_with_accessor(
                uri.c_str(),
                transaction.c_handle(),
                &c_options,
                storage_options_accessor.c_handle(),
                session ? session->c_handle() : nullptr,
                &dataset) != 0) {
            detail::throw_error();
        }
        if (!dataset) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_transaction_commit_with_accessor returned success with "
                "null out_dataset");
        }
        return Dataset(dataset);
    }

inline Dataset Dataset::commit_with_commit_lock(
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
            detail::throw_error();
        }
        if (!dataset) {
            throw Error(
                LANCE_ERR_INTERNAL,
                "lance_transaction_commit_with_commit_lock returned success "
                "with null out_dataset");
        }
        return Dataset(dataset);
    }

inline Dataset Dataset::clone_handle() const {
        auto* ds = lance_dataset_clone_handle(handle_.get());
        if (!ds) detail::throw_error();
        return Dataset(ds);
    }

inline uint64_t Dataset::count_rows() const {
        uint64_t n = lance_dataset_count_rows(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return n;
    }

inline uint64_t Dataset::count_rows(const std::string& filter) const {
        uint64_t n =
            lance_dataset_count_rows_filtered(handle_.get(), filter.c_str());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return n;
    }

inline uint64_t Dataset::version() const {
        uint64_t v = lance_dataset_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return v;
    }

inline std::string Dataset::uri() const {
        const char* value = lance_dataset_uri(handle_.get());
        return detail::take_string(value);
    }

inline uint64_t Dataset::latest_version() const {
        uint64_t v = lance_dataset_latest_version(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return v;
    }

inline std::string Dataset::metadata_json() const {
        const char* json = lance_dataset_metadata_json(handle_.get());
        return detail::take_string(json);
    }

inline std::string Dataset::update_metadata_json(
        const std::vector<std::pair<std::string, std::optional<std::string>>>& values,
        MetadataUpdateMode mode) {
        return run_metadata_update_json(
            handle_.get(), values, mode, /*schema_metadata=*/false);
    }

inline std::string Dataset::update_schema_metadata_json(
        const std::vector<std::pair<std::string, std::optional<std::string>>>& values,
        MetadataUpdateMode mode) {
        return run_metadata_update_json(
            handle_.get(), values, mode, /*schema_metadata=*/true);
    }

inline void Dataset::update_field_metadata(const std::vector<FieldMetadataUpdate>& updates) {
        run_field_metadata_update(handle_.get(), updates);
    }

inline std::string Dataset::data_storage_version() const {
        const char* version =
            lance_dataset_data_storage_version(handle_.get());
        return detail::take_string(version);
    }

inline bool Dataset::has_stable_row_ids() const {
        bool stable = false;
        if (lance_dataset_has_stable_row_ids(handle_.get(), &stable) != 0) {
            detail::throw_error();
        }
        return stable;
    }

inline std::vector<int32_t> Dataset::field_ids() const {
        size_t count = 0;
        if (lance_dataset_field_id_count(handle_.get(), &count) != 0) {
            detail::throw_error();
        }
        std::vector<int32_t> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            int32_t field_id = 0;
            if (lance_dataset_field_id_at(handle_.get(), i, &field_id) != 0) {
                detail::throw_error();
            }
            out.push_back(field_id);
        }
        return out;
    }

inline int32_t Dataset::resolve_field_id(const FieldReference& field) const {
        int32_t field_id = 0;
        LanceFieldReference raw = field.c_handle();
        if (lance_dataset_resolve_field_id(handle_.get(), &raw, &field_id) != 0) {
            detail::throw_error();
        }
        return field_id;
    }

inline std::string Dataset::field_path(int32_t field_id) const {
        const char* path = lance_dataset_field_path(handle_.get(), field_id);
        return detail::take_string(path);
    }

inline std::vector<VersionInfo> Dataset::versions() const {
        auto* raw = lance_dataset_versions(handle_.get());
        if (!raw) detail::throw_error();
        Handle<LanceVersions, lance_versions_close> snap(raw);

        uint64_t n = lance_versions_count(snap.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        std::vector<VersionInfo> out;
        out.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n; i++) {
            VersionInfo info;
            info.id = lance_versions_id_at(snap.get(), static_cast<size_t>(i));
            if (lance_last_error_code() != LANCE_OK) detail::throw_error();
            info.timestamp_ms =
                lance_versions_timestamp_ms_at(snap.get(), static_cast<size_t>(i));
            if (lance_last_error_code() != LANCE_OK) detail::throw_error();
            out.push_back(info);
        }
        return out;
    }

inline LanceObjectStoreIoStats Dataset::io_stats_snapshot() const {
        LanceObjectStoreIoStats stats{};
        if (lance_dataset_io_stats_snapshot(handle_.get(), &stats) != 0)
            detail::throw_error();
        return stats;
    }

inline LanceObjectStoreIoStats Dataset::io_stats_incremental() const {
        LanceObjectStoreIoStats stats{};
        if (lance_dataset_io_stats_incremental(handle_.get(), &stats) != 0)
            detail::throw_error();
        return stats;
    }

inline std::map<std::string, std::string> Dataset::config() const {
        auto* raw = lance_dataset_config(handle_.get());
        if (!raw) detail::throw_error();
        Handle<LanceDatasetConfig, lance_dataset_config_close> snap(raw);

        uint64_t n = lance_dataset_config_count(snap.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        std::map<std::string, std::string> out;
        for (uint64_t i = 0; i < n; i++) {
            const char* key =
                lance_dataset_config_key_at(snap.get(), static_cast<size_t>(i));
            if (!key) detail::throw_error();
            const char* value =
                lance_dataset_config_value_at(snap.get(), static_cast<size_t>(i));
            if (!value) detail::throw_error();
            out.emplace(key, value);
        }
        return out;
    }

inline std::string Dataset::update_config_json(
        const std::vector<ConfigUpdate>& updates,
        MetadataUpdateMode mode) {
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
        return detail::take_string(json);
    }

inline void Dataset::update_config(const std::vector<ConfigUpdate>& updates) {
        (void)update_config_json(updates, MetadataUpdateMode::Merge);
    }

inline std::string Dataset::base_paths_json() const {
        return detail::take_string(
            lance_dataset_base_paths_json(handle_.get()));
    }

inline void Dataset::add_bases(
        const std::vector<BasePath>& bases,
        const AddBasesOptions& options) {
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
            detail::throw_error();
        }
    }

inline ArrowStream Dataset::tracked_files_stream(
        std::optional<uint64_t> min_version,
        TrackedFilesProgressCallback progress) const {
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
            detail::throw_error();
        }
        return ArrowStream(raw);
    }

inline ArrowStream Dataset::all_files_stream() const {
        ArrowArrayStream raw{};
        if (lance_dataset_all_files_stream(handle_.get(), &raw) != 0) {
            detail::throw_error();
        }
        return ArrowStream(raw);
    }

inline std::optional<Transaction> Dataset::read_transaction() const {
        LanceTransaction* transaction = nullptr;
        if (lance_dataset_read_transaction(
                handle_.get(), &transaction) != 0) {
            detail::throw_error();
        }
        if (!transaction) return std::nullopt;
        return Transaction(transaction);
    }

inline std::optional<Transaction> Dataset::read_transaction_by_version(
        uint64_t version) const {
        LanceTransaction* transaction = nullptr;
        if (lance_dataset_read_transaction_by_version(
                handle_.get(), version, &transaction) != 0) {
            detail::throw_error();
        }
        if (!transaction) return std::nullopt;
        return Transaction(transaction);
    }

inline VersionDetails Dataset::current_version() const {
        auto* raw = lance_dataset_current_version(handle_.get());
        if (!raw) detail::throw_error();
        Handle<LanceVersion, lance_version_close> version(raw);
        return read_version(version.get());
    }

inline std::vector<VersionDetails> Dataset::version_details() const {
        auto* raw = lance_dataset_versions(handle_.get());
        if (!raw) detail::throw_error();
        Handle<LanceVersions, lance_versions_close> snap(raw);

        uint64_t count = lance_versions_count(snap.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        std::vector<VersionDetails> out;
        out.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            const LanceVersion* version =
                lance_versions_at(snap.get(), static_cast<size_t>(i));
            if (!version) detail::throw_error();
            out.push_back(read_version(version));
        }
        return out;
    }

inline std::vector<uint64_t> Dataset::version_refs() const {
        auto* raw = lance_dataset_version_refs(handle_.get());
        if (!raw) detail::throw_error();
        Handle<LanceVersionRefs, lance_version_refs_close> refs(raw);

        uint64_t count = lance_version_refs_count(refs.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        std::vector<uint64_t> out;
        out.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t id =
                lance_version_refs_id_at(refs.get(), static_cast<size_t>(i));
            if (lance_last_error_code() != LANCE_OK) detail::throw_error();
            out.push_back(id);
        }
        return out;
    }

inline Dataset Dataset::checkout(const Ref& reference) const {
        auto* out = lance_dataset_checkout_ref(
            handle_.get(), reference.c_handle());
        if (!out) detail::throw_error();
        return Dataset(out);
    }

inline Dataset Dataset::shallow_clone(
        const std::string& target_path,
        const Ref* reference,
        const StorageOptionsAccessor* storage_options) const {
        LanceDataset* out = nullptr;
        if (lance_dataset_shallow_clone(
                handle_.get(),
                target_path.c_str(),
                reference ? reference->c_handle() : nullptr,
                storage_options ? storage_options->c_handle() : nullptr,
                &out) != 0) {
            detail::throw_error();
        }
        return Dataset(out);
    }

inline Dataset Dataset::deep_clone(
        const std::string& target_path,
        const Ref* reference,
        const StorageOptionsAccessor* storage_options) const {
        LanceDataset* out = nullptr;
        if (lance_dataset_deep_clone(
                handle_.get(),
                target_path.c_str(),
                reference ? reference->c_handle() : nullptr,
                storage_options ? storage_options->c_handle() : nullptr,
                &out) != 0) {
            detail::throw_error();
        }
        return Dataset(out);
    }

inline Dataset Dataset::create_branch(
        const std::string& branch_name,
        const Ref& reference,
        std::optional<std::vector<std::pair<std::string, std::string>>> storage_opts) const {
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
        if (!out) detail::throw_error();
        return Dataset(out);
    }

inline Dataset Dataset::checkout_branch(const std::string& branch_name) const {
        auto* out =
            lance_dataset_checkout_branch(handle_.get(), branch_name.c_str());
        if (!out) detail::throw_error();
        return Dataset(out);
    }

inline std::string Dataset::branches_json() const {
        const char* json = lance_dataset_branches_json(handle_.get());
        return detail::take_string(json);
    }

inline std::string Dataset::get_branch_json(const std::string& branch_name) const {
        const char* json =
            lance_dataset_branch_get_json(handle_.get(), branch_name.c_str());
        return detail::take_string(json);
    }

inline std::string Dataset::branch_identifier_json() const {
        const char* json =
            lance_dataset_branch_identifier_json(handle_.get());
        return detail::take_string(json);
    }

inline void Dataset::delete_branch(const std::string& branch_name) const {
        if (lance_dataset_branch_delete(
                handle_.get(), branch_name.c_str()) != 0)
            detail::throw_error();
    }

inline void Dataset::force_delete_branch(const std::string& branch_name) const {
        if (lance_dataset_branch_force_delete(
                handle_.get(), branch_name.c_str()) != 0)
            detail::throw_error();
    }

inline void Dataset::replace_branch_metadata(
        const std::string& branch_name,
        const std::vector<std::pair<std::string, std::string>>& metadata) const {
        auto kv = to_c_key_values(metadata);
        const char* const* metadata_ptr = metadata.empty() ? nullptr : kv.data();
        if (lance_dataset_branch_replace_metadata(
                handle_.get(), branch_name.c_str(), metadata_ptr) != 0)
            detail::throw_error();
    }

inline std::vector<TagInfo> Dataset::tags() const {
        auto* raw = lance_dataset_tags(handle_.get());
        if (!raw) detail::throw_error();
        Handle<LanceTags, lance_tags_close> tags(raw);

        uint64_t count = lance_tags_count(tags.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        std::vector<TagInfo> out;
        out.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            const LanceTag* tag = lance_tags_at(tags.get(), static_cast<size_t>(i));
            if (!tag) detail::throw_error();
            out.push_back(read_tag(tag));
        }
        return out;
    }

inline TagInfo Dataset::get_tag(const std::string& name) const {
        auto* raw = lance_dataset_tag_get(handle_.get(), name.c_str());
        if (!raw) detail::throw_error();
        Handle<LanceTag, lance_tag_close> tag(raw);
        return read_tag(tag.get());
    }

inline void Dataset::create_tag(const std::string& name, const Ref& reference) const {
        if (lance_dataset_tag_create(
                handle_.get(), name.c_str(), reference.c_handle()) != 0)
            detail::throw_error();
    }

inline void Dataset::update_tag(const std::string& name, const Ref& reference) const {
        if (lance_dataset_tag_update(
                handle_.get(), name.c_str(), reference.c_handle()) != 0)
            detail::throw_error();
    }

inline void Dataset::delete_tag(const std::string& name) const {
        if (lance_dataset_tag_delete(handle_.get(), name.c_str()) != 0)
            detail::throw_error();
    }

inline void Dataset::replace_tag_metadata(
        const std::string& name,
        const std::vector<std::pair<std::string, std::string>>& metadata) const {
        auto kv = to_c_key_values(metadata);
        const char* const* metadata_ptr = metadata.empty() ? nullptr : kv.data();
        if (lance_dataset_tag_replace_metadata(
                handle_.get(), name.c_str(), metadata_ptr) != 0)
            detail::throw_error();
    }

inline std::vector<FieldStatistics> Dataset::calculate_data_stats() const {
        auto* raw = lance_dataset_calculate_data_stats(handle_.get());
        if (!raw) detail::throw_error();
        Handle<LanceDataStatistics, lance_data_statistics_close> snap(raw);

        uint64_t n = lance_data_statistics_count(snap.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        std::vector<FieldStatistics> out;
        out.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n; i++) {
            FieldStatistics fs;
            fs.id = lance_data_statistics_field_id_at(snap.get(), static_cast<size_t>(i));
            if (lance_last_error_code() != LANCE_OK) detail::throw_error();
            fs.bytes_on_disk =
                lance_data_statistics_bytes_on_disk_at(snap.get(), static_cast<size_t>(i));
            if (lance_last_error_code() != LANCE_OK) detail::throw_error();
            out.push_back(fs);
        }
        return out;
    }

inline RemovalStats Dataset::cleanup_old_versions(
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
            detail::throw_error();
        }
        return from_c_removal_stats(stats);
    }

inline RemovalStats Dataset::cleanup(
        const CleanupPolicy& policy,
        const CleanupOperationOptions& operation) const {
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
            detail::throw_error();
        }
        return from_c_removal_stats(stats);
    }

inline CleanupExplanation Dataset::explain_cleanup(
        const CleanupPolicy& policy,
        const CleanupOperationOptions& operation) const {
        std::vector<uint64_t> versions;
        uint64_t empty_versions_sentinel = 0;
        const LanceCleanupPolicy raw_policy = to_c_cleanup_policy(
            policy, versions, empty_versions_sentinel);
        const LanceCleanupOperationOptions raw_operation =
            to_c_cleanup_operation(operation);
        auto* raw = lance_dataset_cleanup_explain(
            handle_.get(), &raw_policy, &raw_operation);
        if (!raw) detail::throw_error();
        Handle<LanceCleanupExplanation, lance_cleanup_explanation_close> snap(
            raw);
        return read_cleanup_explanation(snap.get());
    }

inline void Dataset::validate() const {
        if (lance_dataset_validate(handle_.get()) != 0) detail::throw_error();
    }

inline uint64_t Dataset::count_deleted_rows() const {
        uint64_t rows = 0;
        if (lance_dataset_count_deleted_rows(handle_.get(), &rows) != 0)
            detail::throw_error();
        return rows;
    }

inline uint64_t Dataset::num_small_files(size_t max_rows_per_group) const {
        uint64_t count = 0;
        if (lance_dataset_num_small_files(
                handle_.get(), max_rows_per_group, &count) != 0) {
            detail::throw_error();
        }
        return count;
    }

inline Dataset Dataset::restore(uint64_t version) const {
        auto* out = lance_dataset_restore(handle_.get(), version);
        if (!out) detail::throw_error();
        return Dataset(out);
    }

inline uint64_t Dataset::delete_rows(const std::string& predicate) {
        uint64_t num_deleted = 0;
        if (lance_dataset_delete(handle_.get(), predicate.c_str(), &num_deleted) != 0) {
            detail::throw_error();
        }
        return num_deleted;
    }

inline uint64_t Dataset::update(
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
            detail::throw_error();
        }
        return num_updated;
    }

inline void Dataset::merge(ArrowArrayStream* source, const std::string& left_on) {
        merge(source, left_on, left_on);
    }

inline void Dataset::merge(
        ArrowArrayStream* source,
        const std::string& left_on,
        const std::string& right_on) {
        if (lance_dataset_merge(
                handle_.get(), left_on.c_str(), right_on.c_str(), source) != 0) {
            detail::throw_error();
        }
    }

inline LanceMergeInsertResult Dataset::merge_insert(
        const std::vector<std::string>& on_columns,
        ArrowArrayStream* source,
        const LanceMergeInsertParams* params) {
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
            detail::throw_error();
        }
        return result;
    }

inline LanceMergeInsertResult Dataset::upsert(
        const std::vector<std::string>& on_columns,
        ArrowArrayStream* source) {
        LanceMergeInsertParams params{};
        params.when_matched = LANCE_MERGE_WHEN_MATCHED_UPDATE_ALL;
        params.when_not_matched = LANCE_MERGE_WHEN_NOT_MATCHED_INSERT_ALL;
        params.when_not_matched_by_source = LANCE_MERGE_WHEN_NOT_MATCHED_BY_SOURCE_KEEP;
        return merge_insert(on_columns, source, &params);
    }

inline LanceMergeInsertResult Dataset::merge_insert_with_compaction(
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
            detail::throw_error();
        }
        return result;
    }

inline LanceCompactionMetrics Dataset::compact_files(
        const LanceCompactionOptions* options) {
        LanceCompactionMetrics metrics{};
        if (lance_dataset_compact_files(handle_.get(), options, &metrics) != 0) {
            detail::throw_error();
        }
        return metrics;
    }

inline LanceCompactionMetrics Dataset::compact_files_with_options(
        const CompactionOptions* options) {
        LanceCompactionMetrics metrics{};
        if (lance_dataset_compact_files_v2(
                handle_.get(),
                options ? options->c_handle() : nullptr,
                &metrics) != 0)
            detail::throw_error();
        return metrics;
    }

inline CompactionPlan Dataset::plan_compaction(
        const CompactionOptions* options) const {
        auto* plan = lance_dataset_plan_compaction(
            handle_.get(), options ? options->c_handle() : nullptr);
        if (!plan) detail::throw_error();
        return CompactionPlan(plan);
    }

inline LanceCompactionMetrics Dataset::commit_compaction(
        const std::vector<RewriteResult>& results,
        const CompactionOptions* options) {
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
            detail::throw_error();
        return metrics;
    }

inline void Dataset::optimize_indices(
        const OptimizeIndicesOptions* options) {
        if (lance_dataset_optimize_indices(
                handle_.get(),
                options ? options->c_handle() : nullptr) != 0)
            detail::throw_error();
    }

inline OptimizeIndicesPlan Dataset::plan_optimize_indices(
        const OptimizeIndicesOptions* options) const {
        auto* plan = lance_dataset_plan_optimize_indices(
            handle_.get(), options ? options->c_handle() : nullptr);
        if (!plan) detail::throw_error();
        return OptimizeIndicesPlan(plan);
    }

inline void Dataset::commit_optimize_indices(
        const std::vector<IndexSegmentResult>& results,
        const OptimizeIndicesOptions* options) {
        std::vector<const LanceIndexSegmentResult*> raw;
        raw.reserve(results.size());
        for (const auto& result : results) raw.push_back(result.c_handle());
        if (lance_dataset_commit_optimize_indices(
                handle_.get(),
                raw.empty() ? nullptr : raw.data(),
                raw.size(),
                options ? options->c_handle() : nullptr) != 0)
            detail::throw_error();
    }

inline void Dataset::migrate_manifest_paths_v2() {
        if (lance_dataset_migrate_manifest_paths_v2(handle_.get()) != 0)
            detail::throw_error();
    }

inline void Dataset::truncate_table() {
        if (lance_dataset_truncate_table(handle_.get()) != 0) detail::throw_error();
    }

inline void Dataset::drop_columns(const std::vector<std::string>& columns) {
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
            detail::throw_error();
        }
    }

inline void Dataset::alter_columns(const std::vector<ColumnAlteration>& alterations) {
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
            detail::throw_error();
        }
    }

inline void Dataset::add_columns_sql(const std::vector<SqlColumn>& columns,
                         uint64_t batch_size) {
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
            detail::throw_error();
        }
    }

inline void Dataset::add_columns_batch_udf(
        const BatchUDF& udf,
        std::optional<std::vector<std::string>> read_columns,
        std::optional<uint32_t> batch_size) {
        callback_detail::BatchUDFCall call(
            udf, std::move(read_columns), batch_size);

        if (lance_dataset_add_columns_batch_udf(
                handle_.get(), &call.raw, call.read_columns_ptr,
                call.batch_size_ptr) != 0) {
            detail::throw_error();
        }
    }

inline void Dataset::add_columns_batch_udf(
        const BatchUDFV2& udf,
        std::optional<std::vector<std::string>> read_columns,
        std::optional<uint32_t> batch_size) {
        callback_detail::BatchUDFCall call(
            udf, std::move(read_columns), batch_size);

        if (lance_dataset_add_columns_batch_udf_v2(
                handle_.get(), &call.raw_v2, call.read_columns_ptr,
                call.batch_size_ptr) != 0) {
            detail::throw_error();
        }
    }

inline void Dataset::add_columns_nulls(const ArrowSchema* schema) {
        if (lance_dataset_add_columns_nulls(handle_.get(), schema) != 0) {
            detail::throw_error();
        }
    }

inline void Dataset::add_columns_stream(ArrowArrayStream* stream, uint64_t batch_size) {
        // Forward `stream` straight to the C API, which owns the stream and
        // releases it on every path. No RAII guard is needed here (unlike
        // `write`, which builds vectors before its C call): nothing between this
        // method's entry and the call below can throw, so the stream can never
        // be stranded by an exception. WARNING: do not add any throwing code
        // before the C call without first arming a stream-release guard.
        if (lance_dataset_add_columns_stream(
                handle_.get(), stream, batch_size) != 0) {
            detail::throw_error();
        }
    }

inline void Dataset::schema(ArrowSchema* out) const {
        if (lance_dataset_schema(handle_.get(), out) != 0) {
            detail::throw_error();
        }
    }

inline DataFile Dataset::create_data_file(
        const std::string& path,
        std::optional<uint32_t> base_id) const {
        LanceDataFile* file = nullptr;
        const uint32_t* base_id_ptr = base_id ? &*base_id : nullptr;
        if (lance_dataset_create_data_file(
                handle_.get(), path.c_str(), base_id_ptr, &file) != 0) {
            detail::throw_error();
        }
        return DataFile(file);
    }

inline void Dataset::take(const uint64_t* indices, size_t num_indices,
              const std::vector<std::string>& columns,
              ArrowArrayStream* out) const {
        std::vector<const char*> col_ptrs;
        for (auto& c : columns) col_ptrs.push_back(c.c_str());
        col_ptrs.push_back(nullptr);
        const char* const* cols_ptr = columns.empty() ? nullptr : col_ptrs.data();

        if (lance_dataset_take(handle_.get(), indices, num_indices, cols_ptr, out) != 0) {
            detail::throw_error();
        }
    }

inline void Dataset::take(const uint64_t* indices, size_t num_indices,
              ArrowArrayStream* out) const {
        if (lance_dataset_take(handle_.get(), indices, num_indices, nullptr, out) != 0) {
            detail::throw_error();
        }
    }

inline ArrowStream Dataset::take(const uint64_t* indices, size_t num_indices,
                     const std::vector<std::string>& columns) const {
        ArrowArrayStream raw{};
        take(indices, num_indices, columns, &raw);
        return ArrowStream(raw);
    }

inline ArrowStream Dataset::take(const uint64_t* indices, size_t num_indices) const {
        ArrowArrayStream raw{};
        take(indices, num_indices, &raw);
        return ArrowStream(raw);
    }

inline ArrowStream Dataset::take(std::span<const uint64_t> indices,
                     const std::vector<std::string>& columns) const {
        return take(indices.data(), indices.size(), columns);
    }

inline ArrowStream Dataset::take(std::span<const uint64_t> indices) const {
        return take(indices.data(), indices.size());
    }

inline void Dataset::take_rows(const uint64_t* row_ids, size_t num_row_ids,
                   const std::vector<std::string>& columns,
                   ArrowArrayStream* out) const {
        std::vector<const char*> col_ptrs;
        for (auto& c : columns) col_ptrs.push_back(c.c_str());
        col_ptrs.push_back(nullptr);
        const char* const* cols_ptr = columns.empty() ? nullptr : col_ptrs.data();

        if (lance_dataset_take_rows(
                handle_.get(), row_ids, num_row_ids, cols_ptr, out) != 0) {
            detail::throw_error();
        }
    }

inline void Dataset::take_rows(std::span<const uint64_t> row_ids,
                   const std::vector<std::string>& columns,
                   ArrowArrayStream* out) const {
        take_rows(row_ids.data(), row_ids.size(), columns, out);
    }

inline void Dataset::take_rows(const uint64_t* row_ids, size_t num_row_ids,
                   ArrowArrayStream* out) const {
        if (lance_dataset_take_rows(
                handle_.get(), row_ids, num_row_ids, nullptr, out) != 0) {
            detail::throw_error();
        }
    }

inline void Dataset::take_rows(std::span<const uint64_t> row_ids,
                   ArrowArrayStream* out) const {
        take_rows(row_ids.data(), row_ids.size(), out);
    }

inline ArrowStream Dataset::take_rows(const uint64_t* row_ids, size_t num_row_ids,
                          const std::vector<std::string>& columns) const {
        ArrowArrayStream raw{};
        take_rows(row_ids, num_row_ids, columns, &raw);
        return ArrowStream(raw);
    }

inline ArrowStream Dataset::take_rows(const uint64_t* row_ids, size_t num_row_ids) const {
        ArrowArrayStream raw{};
        take_rows(row_ids, num_row_ids, &raw);
        return ArrowStream(raw);
    }

inline ArrowStream Dataset::take_rows(std::span<const uint64_t> row_ids,
                          const std::vector<std::string>& columns) const {
        return take_rows(row_ids.data(), row_ids.size(), columns);
    }

inline ArrowStream Dataset::take_rows(std::span<const uint64_t> row_ids) const {
        return take_rows(row_ids.data(), row_ids.size());
    }

inline std::vector<std::optional<BlobFile>> Dataset::take_blobs(
        const uint64_t* row_ids, size_t num_row_ids,
        const std::string& column) const {
        std::vector<LanceBlobFile*> raw(num_row_ids, nullptr);
        std::vector<std::optional<BlobFile>> blobs;
        blobs.reserve(num_row_ids);
        // An empty vector's data() may be null, which the C side rejects.
        if (num_row_ids > 0 &&
            lance_dataset_take_blobs(handle_.get(), row_ids, num_row_ids,
                                     column.c_str(), raw.data()) != 0) {
            detail::throw_error();
        }
        adopt_blobs(raw, blobs);
        return blobs;
    }

inline std::vector<std::optional<BlobFile>> Dataset::take_blobs_by_indices(
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
            detail::throw_error();
        }
        adopt_blobs(raw, blobs);
        return blobs;
    }

inline std::vector<std::optional<BlobFile>> Dataset::take_blobs_by_addresses(
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
            detail::throw_error();
        }
        adopt_blobs(raw, blobs);
        return blobs;
    }

inline ArrowStream Dataset::read_blobs(
        const std::string& column,
        BlobRowSelector selector,
        const std::vector<uint64_t>& rows,
        const BlobReadOptions& options) const {
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
            detail::throw_error();
        }
        return ArrowStream(stream);
    }

inline ArrowStream Dataset::read_blob_ranges(
        const std::string& column,
        BlobRowSelector selector,
        const std::vector<BlobRangeRequest>& requests,
        const BlobReadOptions& options) const {
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
            detail::throw_error();
        }
        return ArrowStream(stream);
    }

[[deprecated("Use prepare_fts_match_query() to select the Match operator")]] inline FtsQueryContext Dataset::prepare_fts_query(
        const std::string& column,
        const std::string& query,
        uint32_t max_fuzzy_distance,
        FtsCoverageMode coverage_mode) const {
        return prepare_fts_match_query(column, query, FtsMatchOperator::Or,
                                       max_fuzzy_distance, coverage_mode);
    }

inline FtsQueryContext Dataset::prepare_fts_match_query(
        const std::string& column,
        const std::string& query,
        FtsMatchOperator match_operator,
        uint32_t max_fuzzy_distance,
        FtsCoverageMode coverage_mode) const {
        auto* context = lance_dataset_prepare_fts_match_query(
            handle_.get(), column.c_str(), query.c_str(),
            static_cast<int32_t>(match_operator), max_fuzzy_distance,
            static_cast<int32_t>(coverage_mode));
        if (!context) detail::throw_error();
        return FtsQueryContext(context);
    }

inline FtsQueryContext Dataset::prepare_fts_phrase_query(
        const std::string& column,
        const std::string& query,
        int32_t slop,
        FtsCoverageMode coverage_mode) const {
        auto* context = lance_dataset_prepare_fts_phrase_query(
            handle_.get(), column.c_str(), query.c_str(), slop,
            static_cast<int32_t>(coverage_mode));
        if (!context) detail::throw_error();
        return FtsQueryContext(context);
    }

inline uint64_t Dataset::fragment_count() const {
        uint64_t n = lance_dataset_fragment_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return n;
    }

inline std::vector<uint64_t> Dataset::fragment_ids() const {
        auto count = fragment_count();
        std::vector<uint64_t> ids(count);
        if (count > 0) {
            if (lance_dataset_fragment_ids(handle_.get(), ids.data()) != 0)
                detail::throw_error();
        }
        return ids;
    }

inline Fragment Dataset::open_fragment(uint64_t fragment_id) const {
        auto* fragment =
            lance_dataset_open_fragment(handle_.get(), fragment_id);
        if (!fragment) detail::throw_error();
        return Fragment(fragment);
    }

inline void Dataset::create_vector_index(const std::string& column,
                             const LanceVectorIndexParams& params,
                             const std::string& name,
                             bool replace) {
        const char* name_c = name.empty() ? nullptr : name.c_str();
        if (lance_dataset_create_vector_index(handle_.get(), column.c_str(),
                                               name_c, &params, replace) != 0)
            detail::throw_error();
    }

inline void Dataset::create_vector_index_with_progress(
        const std::string& column,
        const LanceVectorIndexParams& params,
        LanceIndexBuildProgressCallback callback,
        void* callback_ctx,
        const std::string& name,
        bool replace) {
        const char* name_c = name.empty() ? nullptr : name.c_str();
        if (lance_dataset_create_vector_index_with_progress(
                handle_.get(), column.c_str(), name_c, &params, callback,
                callback_ctx, replace) != 0)
            detail::throw_error();
    }

inline void Dataset::create_scalar_index(const std::string& column,
                             LanceScalarIndexType index_type,
                             const std::string& name,
                             const std::string& params_json,
                             bool replace) {
        const char* name_c = name.empty() ? nullptr : name.c_str();
        const char* json_c = params_json.empty() ? nullptr : params_json.c_str();
        if (lance_dataset_create_scalar_index(handle_.get(), column.c_str(),
                                               name_c, index_type,
                                               json_c, replace) != 0)
            detail::throw_error();
    }

inline void Dataset::create_scalar_index_with_progress(
        const std::string& column,
        LanceScalarIndexType index_type,
        LanceIndexBuildProgressCallback callback,
        void* callback_ctx,
        const std::string& name,
        const std::string& params_json,
        bool replace) {
        const char* name_c = name.empty() ? nullptr : name.c_str();
        const char* json_c = params_json.empty() ? nullptr : params_json.c_str();
        if (lance_dataset_create_scalar_index_with_progress(
                handle_.get(), column.c_str(), name_c, index_type, json_c,
                callback, callback_ctx, replace) != 0)
            detail::throw_error();
    }

inline void Dataset::create_scalar_index_with_options(
        const std::string& column,
        LanceScalarIndexType index_type,
        const ScalarIndexOptions& options,
        const std::string& index_name,
        const std::string& params_json,
        bool replace) {
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
            detail::throw_error();
    }

inline void Dataset::create_scalar_index_with_options_and_progress(
        const std::string& column,
        LanceScalarIndexType index_type,
        const ScalarIndexOptions& options,
        LanceIndexBuildProgressCallback callback,
        void* callback_ctx,
        const std::string& index_name,
        const std::string& params_json,
        bool replace) {
        const char* name_c = index_name.empty() ? nullptr : index_name.c_str();
        const char* json_c = params_json.empty() ? nullptr : params_json.c_str();
        LanceScalarIndexOptions scalar_options{};
        if (options.train) {
            scalar_options.has_train = true;
            scalar_options.train = *options.train;
        }
        if (lance_dataset_create_scalar_index_with_options_and_progress(
                handle_.get(), column.c_str(), name_c, index_type, json_c,
                &scalar_options, callback, callback_ctx, replace) != 0)
            detail::throw_error();
    }

inline void Dataset::drop_index(const std::string& name) {
        if (lance_dataset_drop_index(handle_.get(), name.c_str()) != 0)
            detail::throw_error();
    }

inline void Dataset::prewarm_index(const std::string& index_name) const {
        if (lance_dataset_prewarm_index(handle_.get(), index_name.c_str()) != 0)
            detail::throw_error();
    }

inline void Dataset::prewarm_index_with_options(
        const std::string& index_name,
        const IndexPrewarmOptions& options) const {
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
            detail::throw_error();
    }

inline uint64_t Dataset::index_count() const {
        uint64_t n = lance_dataset_index_count(handle_.get());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return n;
    }

inline std::string Dataset::list_indices_json() const {
        const char* json = lance_dataset_index_list_json(handle_.get());
        return detail::take_string(json);
    }

inline std::string Dataset::index_segments_json() const {
        const char* json = lance_dataset_index_segments_json(handle_.get());
        return detail::take_string(json);
    }

inline void Dataset::merge_index_metadata(
        const std::array<uint8_t, 16>& index_uuid,
        const std::string& index_type,
        std::optional<size_t> batch_readhead,
        LanceIndexBuildProgressCallback callback,
        void* callback_ctx) const {
        if (lance_dataset_merge_index_metadata(
                handle_.get(), index_uuid.data(), index_type.c_str(),
                batch_readhead.has_value(), batch_readhead.value_or(0), callback,
                callback_ctx) != 0)
            detail::throw_error();
    }

inline std::vector<uint8_t> Dataset::merge_index_segments(
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
            detail::throw_error();
        guard.bytes = output;
        if (!output || output_len == 0) {
            throw Error(LANCE_ERR_INTERNAL,
                        "lance_dataset_merge_index_segments returned empty metadata");
        }
        return std::vector<uint8_t>(output, output + output_len);
    }

inline uint64_t Dataset::index_segment_count(const std::string& index_name) const {
        uint64_t n = lance_dataset_index_segment_count(handle_.get(), index_name.c_str());
        if (lance_last_error_code() != LANCE_OK) detail::throw_error();
        return n;
    }

inline std::vector<std::array<uint8_t, 16>> Dataset::index_segments(const std::string& index_name) const {
        uint64_t count = index_segment_count(index_name);
        std::vector<std::array<uint8_t, 16>> out(count);
        if (count == 0) return out;
        uint64_t written = 0;
        if (lance_dataset_index_segments(handle_.get(), index_name.c_str(),
                                          reinterpret_cast<uint8_t*>(out.data()),
                                          static_cast<size_t>(count), &written) != 0)
            detail::throw_error();
        out.resize(static_cast<size_t>(written));
        return out;
    }

inline void Dataset::commit_index_segments(
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
            detail::throw_error();
    }

inline const LanceDataset* Dataset::c_handle() const { return handle_.get(); }

inline Dataset::Dataset(LanceDataset* ptr) : handle_(ptr) {}

inline LanceSchema Dataset::native_schema() const {
    return LanceSchema::from_dataset(*this);
}

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
        detail::throw_error();
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
        detail::throw_error();
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
        detail::throw_error();
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
    if (!builder) detail::throw_error();
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
    if (!builder) detail::throw_error();
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
    if (!builder) detail::throw_error();
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
        detail::throw_error();
    }
}

inline void Dataset::create_vector_index_with_options_and_progress(
    const std::string& column,
    const VectorIndexOptions& options,
    LanceIndexBuildProgressCallback callback,
    void* callback_ctx,
    const std::string& name,
    bool replace) {
    detail::VectorIndexOptionsStorage storage(options);
    const char* name_c = name.empty() ? nullptr : name.c_str();
    if (lance_dataset_create_vector_index_with_options_and_progress(
            handle_.get(), column.c_str(), name_c, storage.get(), callback,
            callback_ctx, replace) != 0) {
        detail::throw_error();
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
    if (!builder) detail::throw_error();
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
        detail::throw_error();
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
        detail::throw_error();
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
        detail::throw_error();
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
        detail::throw_error();
    return IndexModel(array, schema);
}

inline std::string Dataset::describe_indices_json() const {
    const char* json = lance_dataset_describe_indices_json(handle_.get());
    return detail::take_string(json);
}

inline std::string Dataset::index_statistics_json(
    const std::string& index_name) {
    const char* json = lance_dataset_index_statistics_json(
        handle_.get(), index_name.c_str());
    return detail::take_string(json);
}

inline IvfModel Dataset::get_ivf_model(const std::string& index_name) const {
    auto* model =
        lance_dataset_get_ivf_model(handle_.get(), index_name.c_str());
    if (!model) detail::throw_error();
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
        detail::throw_error();
    if (!stream.release) {
        throw Error(LANCE_ERR_INTERNAL,
                    "lance_dataset_read_index_partition returned an empty stream");
    }
    return ArrowStream(stream);
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
    if (!s) detail::throw_error();
    return Scanner(s);
}

inline Scanner Dataset::scan(std::nullopt_t,
                             const std::optional<std::string>& filter) const {
    auto* s = lance_scanner_new(handle_.get(), nullptr,
                               filter ? filter->c_str() : nullptr);
    if (!s) detail::throw_error();
    return Scanner(s);
}

inline SqlQueryBuilder Dataset::sql(const std::string& sql) const {
    auto* builder = lance_dataset_sql_builder_new(c_handle(), sql.c_str());
    if (!builder) detail::throw_error();
    return SqlQueryBuilder(builder);
}

inline DatasetDeltaBuilder Dataset::delta() const {
    auto* builder = lance_dataset_delta_builder_new(c_handle());
    if (!builder) detail::throw_error();
    return DatasetDeltaBuilder(builder);
}

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
    if (!raw) detail::throw_error();
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
    if (rc != 0) detail::throw_error();
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
    if (rc != 0) detail::throw_error();
    if (!out) {
        throw Error(
            LANCE_ERR_INTERNAL,
            "lance_transaction_commit_namespace_uri returned success with null out_dataset");
    }
    return Dataset(out);
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
        detail::throw_error();
    }
}

inline std::optional<MemWalIndexDetails> Dataset::mem_wal_index_details() const {
    ::LanceMemWalIndexDetails* raw = nullptr;
    if (lance_mem_wal_index_details(
            const_cast<::LanceDataset*>(handle_.get()), &raw) != 0) {
        detail::throw_error();
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
            detail::throw_error();
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
            detail::throw_error();
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
            detail::throw_error();
        }
        defaults.emplace(key, value);
    }

    const uint32_t num_shards =
        lance_mem_wal_index_details_num_shards(details.get());
    const int32_t maintain_all =
        lance_mem_wal_index_details_maintain_all_indexes(details.get());
    const char* dir_id = lance_mem_wal_index_details_dir_id(details.get());
    if (!dir_id) detail::throw_error();
    return MemWalIndexDetails(num_shards, maintain_all != 0,
                              std::move(maintained), std::move(specs),
                              std::move(defaults), std::string(dir_id));
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
        detail::throw_error();
    }
    return ShardWriter(raw);
}

}  // namespace lance
