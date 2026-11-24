/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the udf domain.
#pragma once

#include "lance/detail/declarations.hpp"

namespace lance {

namespace callback_detail {

inline void release_owned_string(LanceOwnedString* value) noexcept {
    if (value == nullptr) return;
    std::free(value->data);
    value->data = nullptr;
    value->release = nullptr;
}

}

namespace callback_detail {

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

}

namespace callback_detail {

inline void release_array_after_exception(ArrowArray* value) noexcept {
    if (value != nullptr && value->release != nullptr) {
        value->release(value);
    }
}

}

namespace callback_detail {

inline void release_schema_after_exception(ArrowSchema* value) noexcept {
    if (value != nullptr && value->release != nullptr) {
        value->release(value);
    }
}

}

namespace callback_detail {

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

}

namespace callback_detail {

inline int32_t get_batch_v2_trampoline(
    void* callback_ctx,
    const LanceUDFBatchInfo* raw_info,
    ArrowArray* output,
    ArrowSchema* output_schema,
    LanceCallbackError* error) noexcept {
    try {
        auto* context = static_cast<CheckpointContextV2*>(callback_ctx);
        BatchUDFInfo info{raw_info->fragment_id, raw_info->batch_index};
        return context->store->get_batch(info, *output, *output_schema) ? 1 : 0;
    } catch (const lance::Error& exception) {
        release_array_after_exception(output);
        release_schema_after_exception(output_schema);
        set_callback_error(error, static_cast<int32_t>(exception.code), exception.what());
        return -1;
    } catch (const std::exception& exception) {
        release_array_after_exception(output);
        release_schema_after_exception(output_schema);
        set_callback_error(error, LANCE_ERR_INTERNAL, exception.what());
        return -1;
    } catch (...) {
        release_array_after_exception(output);
        release_schema_after_exception(output_schema);
        set_callback_error(error, LANCE_ERR_INTERNAL, "unknown C++ exception");
        return -1;
    }
}

}

namespace callback_detail {

inline BatchUDFCall::BatchUDFCall(const BatchUDF& udf,
                 std::optional<std::vector<std::string>> columns,
                 std::optional<uint32_t> batch_size) : read_columns(std::move(columns)) {
        if (!udf.mapper) {
            throw Error(
                LANCE_ERR_INVALID_ARGUMENT, "BatchUDF mapper must not be empty");
        }
        if (udf.output_schema == nullptr) {
            throw Error(
                LANCE_ERR_INVALID_ARGUMENT,
                "BatchUDF output_schema must not be null");
        }

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
            raw_store.get_batch = &get_batch_trampoline;
            raw_store.insert_batch = &insert_batch_trampoline<CheckpointContext>;
            raw_store.get_fragment = &get_fragment_trampoline<CheckpointContext>;
            raw_store.insert_fragment =
                &insert_fragment_trampoline<CheckpointContext>;
            raw_store.callback_ctx = &checkpoint_context;
        }

        mapper_context.udf = &udf;
        raw.mapper = &mapper_trampoline<MapperContext>;
        raw.mapper_ctx = &mapper_context;
        raw.output_schema = udf.output_schema;
        raw.checkpoint_store = store == nullptr ? nullptr : &raw_store;

        prepare_arguments(batch_size);
    }

}

namespace callback_detail {

inline BatchUDFCall::BatchUDFCall(const BatchUDFV2& udf,
                 std::optional<std::vector<std::string>> columns,
                 std::optional<uint32_t> batch_size) : read_columns(std::move(columns)) {
        if (!udf.mapper) {
            throw Error(
                LANCE_ERR_INVALID_ARGUMENT, "BatchUDF mapper must not be empty");
        }
        if (udf.output_schema == nullptr) {
            throw Error(
                LANCE_ERR_INVALID_ARGUMENT,
                "BatchUDF output_schema must not be null");
        }

        const UDFCheckpointStoreV2* store = nullptr;
        if (udf.checkpoint_store) {
            store = &*udf.checkpoint_store;
            if (!store->get_batch || !store->insert_batch ||
                !store->get_fragment || !store->insert_fragment) {
                throw Error(
                    LANCE_ERR_INVALID_ARGUMENT,
                    "UDFCheckpointStoreV2 must provide all four callbacks");
            }
            checkpoint_context_v2.store = store;
            raw_store_v2.get_batch = &get_batch_v2_trampoline;
            raw_store_v2.insert_batch = &insert_batch_trampoline<CheckpointContextV2>;
            raw_store_v2.get_fragment = &get_fragment_trampoline<CheckpointContextV2>;
            raw_store_v2.insert_fragment =
                &insert_fragment_trampoline<CheckpointContextV2>;
            raw_store_v2.callback_ctx = &checkpoint_context_v2;
        }

        mapper_context_v2.udf = &udf;
        raw_v2.mapper = &mapper_trampoline<MapperContextV2>;
        raw_v2.mapper_ctx = &mapper_context_v2;
        raw_v2.output_schema = udf.output_schema;
        raw_v2.checkpoint_store = store == nullptr ? nullptr : &raw_store_v2;

        prepare_arguments(batch_size);
    }

}

namespace callback_detail {

inline void BatchUDFCall::prepare_arguments(std::optional<uint32_t> batch_size) {
        if (read_columns) {
            read_column_ptrs.reserve(read_columns->size() + 1);
            for (const auto& column : *read_columns) {
                read_column_ptrs.push_back(column.c_str());
            }
            read_column_ptrs.push_back(nullptr);
            read_columns_ptr = read_column_ptrs.data();
        }
        if (batch_size) {
            batch_size_value = *batch_size;
            batch_size_ptr = &batch_size_value;
        }
    }

}

}  // namespace lance
