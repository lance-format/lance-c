/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the write domain.
#pragma once

#include "lance/detail/declarations.hpp"
#include "lance/detail/inline/base.hpp"
#include "lance/detail/inline/storage.hpp"
#include "lance/detail/inline/udf.hpp"

namespace lance {

inline FieldReference::FieldReference(FieldReferenceKind kind, int32_t field_id, std::string path) : kind_(kind), field_id_(field_id), path_(std::move(path)) {}

inline FieldReference FieldReference::by_id(int32_t field_id) {
        return FieldReference(FieldReferenceKind::Id, field_id, {});
    }

inline FieldReference FieldReference::by_path(std::string path) {
        return FieldReference(FieldReferenceKind::Path, 0, std::move(path));
    }

inline LanceFieldReference FieldReference::c_handle() const {
        return LanceFieldReference{
            static_cast<LanceFieldReferenceKind>(kind_),
            field_id_,
            kind_ == FieldReferenceKind::Path ? path_.c_str() : nullptr,
        };
    }

inline LanceWriteOptions to_c_write_options(
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
        cleanup.older_than_seconds = options.auto_cleanup->older_than_seconds;
        cleanup.older_than_nanos = options.auto_cleanup->older_than_nanos;
        c_options.auto_cleanup = &cleanup;
    }
    return c_options;
}

inline const LanceNativeWriteOptions* NativeWriteOptions::c_handle() const { return handle_.get(); }

namespace native_write_options_detail {

inline int32_t invoke_fragment(
    const NativeWriteProgress::FragmentCallback& callback,
    const LanceWriteFragmentProgressInfo* info,
    LanceCallbackError* error,
    const char* context) noexcept {
    try {
        FragmentWriteProgressInfo progress{};
        if (info != nullptr) {
            progress.fragment_id = info->fragment_id;
            if (info->has_physical_rows != 0) {
                progress.physical_rows = info->physical_rows;
            }
            progress.file_count = info->file_count;
        }
        callback(progress);
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
            error, LANCE_ERR_INTERNAL, context);
        return -1;
    }
}

}

namespace native_write_options_detail {

extern "C" inline int32_t begin_fragment_trampoline(
    void* callback_ctx,
    const LanceWriteFragmentProgressInfo* info,
    LanceCallbackError* error) noexcept {
    auto* context = static_cast<ProgressContext*>(callback_ctx);
    return invoke_fragment(
        context->progress.begin_fragment,
        info,
        error,
        "unknown C++ exception in write fragment begin callback");
}

}

namespace native_write_options_detail {

extern "C" inline int32_t complete_fragment_trampoline(
    void* callback_ctx,
    const LanceWriteFragmentProgressInfo* info,
    LanceCallbackError* error) noexcept {
    auto* context = static_cast<ProgressContext*>(callback_ctx);
    return invoke_fragment(
        context->progress.complete_fragment,
        info,
        error,
        "unknown C++ exception in write fragment complete callback");
}

}

namespace native_write_options_detail {

extern "C" inline void write_stats_trampoline(
    void* callback_ctx,
    const LanceWriteStats* stats) noexcept {
    auto* context = static_cast<ProgressContext*>(callback_ctx);
    try {
        NativeWriteStats value{};
        if (stats != nullptr) {
            value.bytes_written = stats->bytes_written;
            value.rows_written = stats->rows_written;
            value.files_written = stats->files_written;
        }
        context->progress.write_stats(value);
    } catch (...) {
        // Native write progress has no error channel. Swallowing keeps the C
        // ABI boundary sound; use fragment callbacks for fallible progress.
    }
}

}

namespace native_write_options_detail {

extern "C" inline void release_progress_context(void* callback_ctx) noexcept {
    delete static_cast<ProgressContext*>(callback_ctx);
}

}

namespace native_write_options_detail {

inline void build_base_store_params(
    const std::optional<std::vector<DatasetBaseStoreParams>>& base_store_params,
    BaseStoreParamsHolder& holder) {
    if (!base_store_params) return;
    holder.paths.reserve(base_store_params->size());
    holder.views.reserve(base_store_params->size());
    for (const auto& entry : *base_store_params) {
        holder.paths.push_back(entry.base_path);
    }
    for (std::size_t index = 0; index < base_store_params->size(); ++index) {
        const auto& entry = (*base_store_params)[index];
        LanceBaseStoreParams view{};
        view.base_path = holder.paths[index].c_str();
        view.storage_options_accessor =
            entry.storage_options_accessor
                ? entry.storage_options_accessor->c_handle()
                : nullptr;
        if (entry.block_size) {
            view.has_block_size = 1;
            view.block_size = *entry.block_size;
        }
        if (entry.s3_credentials_refresh_offset) {
            view.has_s3_credentials_refresh_offset = 1;
            view.s3_credentials_refresh_seconds =
                entry.s3_credentials_refresh_offset->seconds;
            view.s3_credentials_refresh_nanos =
                entry.s3_credentials_refresh_offset->nanos;
        }
        if (entry.use_constant_size_upload_parts) {
            view.has_use_constant_size_upload_parts = 1;
            view.use_constant_size_upload_parts =
                *entry.use_constant_size_upload_parts;
        }
        if (entry.list_is_lexically_ordered) {
            view.has_list_is_lexically_ordered = 1;
            view.list_is_lexically_ordered =
                *entry.list_is_lexically_ordered;
        }
        holder.views.push_back(view);
    }
    holder.list.items =
        holder.views.empty() ? nullptr : holder.views.data();
    holder.list.count = holder.views.size();
}

}

namespace native_write_options_detail {

inline void build_initial_bases(
    const std::optional<std::vector<BasePath>>& initial_bases,
    BuildState& state) {
    if (!initial_bases) return;
    state.base_paths.reserve(initial_bases->size());
    state.base_names.reserve(initial_bases->size());
    state.base_views.reserve(initial_bases->size());
    for (const auto& base : *initial_bases) {
        state.base_paths.push_back(base.path);
        state.base_names.push_back(base.name);
    }
    for (std::size_t index = 0; index < state.base_paths.size(); ++index) {
        LanceBasePath view{};
        view.id = (*initial_bases)[index].id;
        view.path = state.base_paths[index].c_str();
        view.name = state.base_names[index]
                        ? state.base_names[index]->c_str()
                        : nullptr;
        view.is_dataset_root = (*initial_bases)[index].is_dataset_root;
        state.base_views.push_back(view);
    }
    state.base_list.items =
        state.base_views.empty() ? &state.empty_base : state.base_views.data();
    state.base_list.count = state.base_views.size();
}

}

inline NativeWriteOptions::NativeWriteOptions(
    const NativeWriteOptionsSpec& spec)
    : handle_(nullptr) {
    using namespace native_write_options_detail;

    BuildState state;
    state.write = lance::to_c_write_options(spec.write, state.cleanup);

    if (spec.transaction_properties) {
        state.has_transaction_properties = true;
        state.property_keys.reserve(spec.transaction_properties->size());
        state.property_values.reserve(spec.transaction_properties->size());
        for (const auto& [key, value] : *spec.transaction_properties) {
            state.property_keys.push_back(key.c_str());
            state.property_values.push_back(value.c_str());
        }
        state.properties.keys = state.property_keys.empty()
                                    ? nullptr
                                    : state.property_keys.data();
        state.properties.values = state.property_values.empty()
                                      ? nullptr
                                      : state.property_values.data();
        state.properties.count = spec.transaction_properties->size();
    }

    build_initial_bases(spec.initial_bases, state);

    if (spec.target_bases) {
        state.target_ids = *spec.target_bases;
        state.target_list.items = state.target_ids.empty()
                                      ? nullptr
                                      : state.target_ids.data();
        state.target_list.count = state.target_ids.size();
    }

    if (spec.target_base_names_or_paths) {
        state.target_names = *spec.target_base_names_or_paths;
        state.target_name_ptrs.reserve(state.target_names.size());
        for (const auto& value : state.target_names) {
            state.target_name_ptrs.push_back(value.c_str());
        }
        state.target_name_list.items = state.target_name_ptrs.empty()
                                           ? nullptr
                                           : state.target_name_ptrs.data();
        state.target_name_list.count = state.target_name_ptrs.size();
    }

    build_base_store_params(spec.base_store_params, state.base_store);

    if (spec.file_writer_options) {
        if (spec.file_writer_options->data_cache_bytes) {
            state.file_options.has_data_cache_bytes = 1;
            state.file_options.data_cache_bytes =
                *spec.file_writer_options->data_cache_bytes;
        }
        if (spec.file_writer_options->max_page_bytes) {
            state.file_options.has_max_page_bytes = 1;
            state.file_options.max_page_bytes =
                *spec.file_writer_options->max_page_bytes;
        }
        if (spec.file_writer_options->keep_original_array) {
            state.file_options.has_keep_original_array = 1;
            state.file_options.keep_original_array =
                *spec.file_writer_options->keep_original_array ? 1 : 0;
        }
    }

    const bool has_progress =
        static_cast<bool>(spec.progress.begin_fragment) ||
        static_cast<bool>(spec.progress.complete_fragment) ||
        static_cast<bool>(spec.progress.write_stats);
    if (has_progress) {
        state.progress_ctx = std::make_unique<ProgressContext>();
        state.progress_ctx->progress = spec.progress;
        if (spec.progress.begin_fragment) {
            state.progress.begin_fragment = &begin_fragment_trampoline;
        }
        if (spec.progress.complete_fragment) {
            state.progress.complete_fragment = &complete_fragment_trampoline;
        }
        if (spec.progress.write_stats) {
            state.progress.write_stats = &write_stats_trampoline;
        }
        state.progress.release = &release_progress_context;
        state.progress.callback_ctx = state.progress_ctx.get();
    }

    LanceNativeWriteOptionsSpec c_spec{};
    c_spec.write_options = &state.write;
    c_spec.transaction_properties =
        state.has_transaction_properties ? &state.properties : nullptr;
    c_spec.has_enable_v2_manifest_paths =
        spec.enable_v2_manifest_paths ? 1 : 0;
    c_spec.enable_v2_manifest_paths =
        spec.enable_v2_manifest_paths.value_or(false);
    c_spec.initial_bases =
        spec.initial_bases ? &state.base_list : nullptr;
    c_spec.target_bases =
        spec.target_bases ? &state.target_list : nullptr;
    c_spec.target_base_names_or_paths =
        spec.target_base_names_or_paths ? &state.target_name_list : nullptr;
    c_spec.has_target_all_bases = spec.target_all_bases ? 1 : 0;
    c_spec.target_all_bases = spec.target_all_bases.value_or(false);
    c_spec.base_store_params =
        spec.base_store_params ? &state.base_store.list : nullptr;
    c_spec.allow_external_blob_outside_bases =
        spec.allow_external_blob_outside_bases;
    c_spec.external_blob_mode =
        static_cast<int32_t>(spec.external_blob_mode);
    c_spec.has_blob_pack_file_size_threshold =
        spec.blob_pack_file_size_threshold ? 1 : 0;
    c_spec.blob_pack_file_size_threshold =
        spec.blob_pack_file_size_threshold.value_or(0);
    c_spec.file_writer_options =
        spec.file_writer_options ? &state.file_options : nullptr;
    c_spec.session = spec.session ? spec.session->c_handle() : nullptr;
    c_spec.storage_options_accessor =
        spec.storage_options_accessor
            ? spec.storage_options_accessor->c_handle()
            : nullptr;
    c_spec.commit_lock =
        spec.commit_lock ? spec.commit_lock->c_handle() : nullptr;
    c_spec.progress = has_progress ? &state.progress : nullptr;

    auto* raw = lance_native_write_options_new(&c_spec);
    if (raw == nullptr) {
        // Ownership of the progress context transfers only on success.
        detail::throw_error();
    }
    handle_ = Handle<LanceNativeWriteOptions, lance_native_write_options_close>(
        raw);
    (void)state.progress_ctx.release();
}

}  // namespace lance
