/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: Copyright The Lance Authors */

// Opt-in inline definitions for the batch domain.
#pragma once

#include "lance/detail/declarations.hpp"
#include "lance/detail/inline/base.hpp"

namespace lance {

inline Batch::Batch(LanceBatch* b) : handle_(b) {}

inline Batch Batch::from_arrow(ArrowArray* array, const ArrowSchema* schema) {
        LanceBatch* raw = nullptr;
        if (lance_batch_from_arrow(array, schema, &raw) != 0) {
            detail::throw_error();
        }
        return Batch(raw);
    }

inline void Batch::to_arrow(ArrowArray* out_array, ArrowSchema* out_schema) const {
        if (lance_batch_to_arrow(handle_.get(), out_array, out_schema) != 0)
            detail::throw_error();
    }

inline const LanceBatch* Batch::c_handle() const { return handle_.get(); }

inline FragmentOverlayWriter::FragmentOverlayWriter(LanceFragmentOverlayWriter* writer) : handle_(writer) {}

inline void FragmentOverlayWriter::write_batch(const Batch& batch) {
        if (lance_fragment_overlay_writer_write_batch(
                handle_.get(), batch.c_handle()) != 0) {
            detail::throw_error();
        }
    }

inline std::optional<DataOverlayGroup> FragmentOverlayWriter::finish() {
        auto* raw = handle_.release();
        LanceDataOverlayGroup* group = nullptr;
        if (lance_fragment_overlay_writer_finish(raw, &group) != 0) {
            detail::throw_error();
        }
        if (!group) return std::nullopt;
        return DataOverlayGroup(group);
    }

inline void FragmentOverlayWriter::abort() {
        auto* raw = handle_.release();
        if (raw) lance_fragment_overlay_writer_abort(raw);
    }

inline const LanceFragmentOverlayWriter* FragmentOverlayWriter::c_handle() const {
        return handle_.get();
    }

}  // namespace lance
