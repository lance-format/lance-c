// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Public C ABI checks against the Rust `repr(C)` implementation.
//!
//! `include/lance/lance.h` is a hand-maintained public ABI. A Rust-side change
//! can silently alter a struct layout or an enum value without touching the
//! header. This module holds one explicit association table between public C
//! symbols and their Rust definitions. The test compiles a C11 probe against the
//! real header and compares its sizes, alignments, field offsets and enum
//! values with the actual Rust layout.
//!
//! Enum values are associated by semantic name (`LANCE_ERR_PANIC` with
//! `LanceErrorCode::Panic`), never by numeric value. The header scan only checks
//! completeness: new, renamed, or unmapped symbols fail the test instead of
//! being paired automatically.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::process::Command;

/// Explicit C-to-Rust ABI association table.
#[derive(Debug, Default)]
struct AbiManifest {
    /// Complete public structs and their fields in declaration order.
    structs: BTreeMap<&'static str, Vec<&'static str>>,
    /// Typed C enums and their `(C enumerator, Rust variant)` associations.
    enums: BTreeMap<&'static str, Vec<(&'static str, &'static str)>>,
    /// Raw int32 constant families exposed as C enums without a Rust enum.
    constants: BTreeMap<&'static str, Vec<(&'static str, i64)>>,
    /// Plain `struct` definitions from the Arrow C Data Interface.
    arrow: BTreeSet<&'static str>,
    /// Pointer-only handles whose internals are not part of the public ABI.
    opaque: BTreeSet<&'static str>,
    /// Expected probe records, keyed by semantic identity rather than ordering.
    records: BTreeSet<String>,
}

macro_rules! abi_struct {
    ($manifest:ident, $name:ident { $($field:ident),* $(,)? }) => {{
        $manifest
            .structs
            .insert(stringify!($name), vec![$(stringify!($field)),*]);
        $manifest.records.insert(format!(
            "struct-size|{}|{}",
            stringify!($name),
            std::mem::size_of::<lance_c::$name>()
        ));
        $manifest.records.insert(format!(
            "struct-align|{}|{}",
            stringify!($name),
            std::mem::align_of::<lance_c::$name>()
        ));
        $(
            $manifest.records.insert(format!(
                "field-offset|{}|{}|{}",
                stringify!($name),
                stringify!($field),
                std::mem::offset_of!(lance_c::$name, $field)
            ));
        )*
    }};
}

// The C header names this enum after its public symbol; the Rust type is
// `IndexBuildProgressEvent`. Keep the exception explicit instead of renaming
// the public Rust API or guessing a type from the C name.
macro_rules! abi_rust_enum {
    (LanceIndexBuildProgressEvent) => {
        lance_c::IndexBuildProgressEvent
    };
    ($name:ident) => {
        lance_c::$name
    };
}

macro_rules! abi_rust_enum_value {
    (LanceIndexBuildProgressEvent, $variant:ident) => {
        lance_c::IndexBuildProgressEvent::$variant as i64
    };
    ($name:ident, $variant:ident) => {
        lance_c::$name::$variant as i64
    };
}

macro_rules! abi_enum {
    ($manifest:ident, $name:ident { $($c_name:ident => $rust_name:ident),* $(,)? }) => {{
        $manifest.enums.insert(
            stringify!($name),
            vec![$((stringify!($c_name), stringify!($rust_name))),*],
        );
        $manifest.records.insert(format!(
            "enum-size|{}|{}",
            stringify!($name),
            std::mem::size_of::<abi_rust_enum!($name)>()
        ));
        $manifest.records.insert(format!(
            "enum-align|{}|{}",
            stringify!($name),
            std::mem::align_of::<abi_rust_enum!($name)>()
        ));
        $(
            $manifest.records.insert(format!(
                "enum-value|{}|{}|{}",
                stringify!($name),
                stringify!($c_name),
                abi_rust_enum_value!($name, $rust_name)
            ));
        )*
    }};
}

macro_rules! abi_constants {
    ($manifest:ident, $name:ident { $($c_name:ident = $value:expr),* $(,)? }) => {{
        $manifest.constants.insert(
            stringify!($name),
            vec![$((stringify!($c_name), $value as i64)),*],
        );
        $(
            $manifest.records.insert(format!(
                "constant|{}|{}|{}",
                stringify!($name),
                stringify!($c_name),
                $value as i64
            ));
        )*
    }};
}

macro_rules! abi_arrow {
    ($manifest:ident, $($c_name:ident => $rust_type:ty),* $(,)?) => {{
        $(
            $manifest.arrow.insert(stringify!($c_name));
            $manifest.records.insert(format!(
                "arrow-size|{}|{}",
                stringify!($c_name),
                std::mem::size_of::<$rust_type>()
            ));
            $manifest.records.insert(format!(
                "arrow-align|{}|{}",
                stringify!($c_name),
                std::mem::align_of::<$rust_type>()
            ));
        )*
    }};
}

macro_rules! abi_opaque {
    ($manifest:ident, $($name:ident),* $(,)?) => {{
        $($manifest.opaque.insert(stringify!($name));)*
    }};
}

/// Build the checked-in association table and the expected Rust-side records.
fn manifest() -> AbiManifest {
    let mut manifest = AbiManifest::default();

    // Public structs. Every field is listed so the C offset check covers the
    // complete public layout, including newly added fields.
    abi_struct!(
        manifest,
        LanceVectorIndexParams {
            index_type,
            metric,
            num_partitions,
            num_sub_vectors,
            num_bits,
            max_iterations,
            hnsw_m,
            hnsw_ef_construction,
            sample_rate,
        }
    );
    abi_struct!(manifest, LanceOptionalU64 { value, is_set });
    abi_struct!(
        manifest,
        LanceOptionalTimestamp {
            is_set,
            unix_seconds,
            nanos,
        }
    );
    abi_struct!(
        manifest,
        LanceStringMap {
            keys,
            values,
            count,
        }
    );
    abi_struct!(
        manifest,
        LancePrecomputedShuffleBuffers {
            path,
            buffers,
            buffer_count,
        }
    );
    abi_struct!(
        manifest,
        LanceVectorIndexOptions {
            index_type,
            metric,
            num_partitions,
            target_partition_size,
            max_iters,
            sample_rate,
            streaming_sample_rate,
            streaming_coreset_rate,
            streaming_refine_passes,
            precomputed_partitions_file,
            precomputed_shuffle_buffers,
            shuffle_partition_batches,
            shuffle_partition_concurrency,
            storage_options,
            num_sub_vectors,
            num_bits,
            ivf_centroids,
            ivf_centroids_schema,
            pq_codebook,
            pq_codebook_schema,
            rabitq_model_json,
            hnsw_max_level,
            hnsw_m,
            hnsw_ef_construction,
            index_file_version,
            skip_transpose,
            runtime_hints,
            has_train,
            train,
        }
    );
    abi_struct!(
        manifest,
        LanceIndexTrainerOptions {
            sample_rate,
            max_iters,
        }
    );
    abi_struct!(manifest, LanceScalarIndexOptions { has_train, train });
    abi_struct!(
        manifest,
        LanceSessionCacheStats {
            index_cache_hits,
            index_cache_misses,
            index_cache_entries,
            index_cache_size_bytes,
            metadata_cache_hits,
            metadata_cache_misses,
            metadata_cache_entries,
            metadata_cache_size_bytes,
        }
    );
    abi_struct!(
        manifest,
        LanceSessionCacheSpec {
            kind,
            size_bytes,
            backend_uri,
            backend_kind,
            option_keys,
            option_values,
            option_count,
        }
    );
    abi_struct!(
        manifest,
        LanceDatasetOpenOptions {
            version,
            storage_options_accessor,
        }
    );
    abi_struct!(
        manifest,
        LanceRemovalStats {
            bytes_removed,
            old_versions,
            data_files_removed,
            transaction_files_removed,
            index_files_removed,
            deletion_files_removed,
            failed_deletes,
        }
    );
    abi_struct!(
        manifest,
        LanceCleanupPolicy {
            has_before_timestamp,
            before_timestamp_seconds,
            before_timestamp_nanos,
            has_before_version,
            before_version,
            versions,
            num_versions,
            has_retain_versions,
            retain_versions,
            has_delete_unverified,
            delete_unverified,
            has_error_if_tagged_old_versions,
            error_if_tagged_old_versions,
            has_clean_referenced_branches,
            clean_referenced_branches,
            has_delete_rate_limit,
            delete_rate_limit,
        }
    );
    abi_struct!(
        manifest,
        LanceCleanupOperationOptions {
            has_delete_concurrency,
            delete_concurrency,
            has_delete_rate_limit,
            delete_rate_limit,
            has_max_candidate_files,
            max_candidate_files,
        }
    );
    abi_struct!(
        manifest,
        LanceCleanupOldVersionsOptions {
            older_than_seconds,
            older_than_nanos,
            has_delete_unverified,
            delete_unverified,
            has_error_if_tagged_old_versions,
            error_if_tagged_old_versions,
        }
    );
    abi_struct!(
        manifest,
        LanceFieldReference {
            kind,
            field_id,
            path,
        }
    );
    abi_struct!(
        manifest,
        LanceFieldMetadataUpdate {
            field,
            keys,
            values,
            count,
            mode,
        }
    );
    abi_struct!(
        manifest,
        LanceBasePath {
            id,
            path,
            name,
            is_dataset_root,
        }
    );
    abi_struct!(
        manifest,
        LanceBasePathView {
            id,
            path,
            name,
            is_dataset_root,
        }
    );
    abi_struct!(manifest, LanceBasePathList { items, count });
    abi_struct!(
        manifest,
        LanceTransactionProperties {
            keys,
            values,
            count,
        }
    );
    abi_struct!(
        manifest,
        LanceTrackedFilesProgress {
            callback,
            release,
            callback_ctx,
        }
    );
    abi_struct!(
        manifest,
        LanceTrackedFilesOptions {
            has_min_version,
            min_version,
            progress,
        }
    );
    abi_struct!(
        manifest,
        LanceMergeInsertParams {
            when_matched,
            when_matched_expr,
            when_not_matched,
            when_not_matched_by_source,
            when_not_matched_by_source_expr,
        }
    );
    abi_struct!(
        manifest,
        LanceMergeInsertResult {
            num_inserted_rows,
            num_updated_rows,
            num_deleted_rows,
        }
    );
    abi_struct!(manifest, LanceOwnedString { data, release });
    abi_struct!(manifest, LanceCallbackError { code, message });
    abi_struct!(
        manifest,
        LanceUDFBatchInfo {
            fragment_id,
            batch_index,
        }
    );
    abi_struct!(
        manifest,
        LanceUDFCheckpointStore {
            get_batch,
            insert_batch,
            get_fragment,
            insert_fragment,
            callback_ctx,
        }
    );
    abi_struct!(
        manifest,
        LanceBatchUDF {
            mapper,
            mapper_ctx,
            output_schema,
            checkpoint_store,
        }
    );
    abi_struct!(
        manifest,
        LanceCommitLockCallbacks {
            lock,
            release,
            destroy_lease,
            callback_ctx,
            release_context,
        }
    );
    abi_struct!(
        manifest,
        LanceCompactionOptions {
            target_rows_per_fragment,
            max_rows_per_group,
            max_bytes_per_file,
            num_threads,
            batch_size,
        }
    );
    abi_struct!(
        manifest,
        LanceCompactionMetrics {
            fragments_removed,
            fragments_added,
            files_removed,
            files_added,
        }
    );
    abi_struct!(
        manifest,
        LanceColumnAlteration {
            path,
            rename,
            nullable_mode,
            data_type,
        }
    );
    abi_struct!(manifest, LanceSqlColumn { name, expression });
    abi_struct!(manifest, LanceBlobRange { offset, length });
    abi_struct!(
        manifest,
        LanceBlobFieldOptions {
            has_inline_size_threshold,
            inline_size_threshold,
            has_dedicated_size_threshold,
            dedicated_size_threshold,
        }
    );
    abi_struct!(
        manifest,
        LanceBlobReadOptions {
            has_io_buffer_size,
            io_buffer_size,
            order,
        }
    );
    abi_struct!(
        manifest,
        LanceBlobRangeRequest {
            row,
            offset,
            length,
        }
    );
    abi_struct!(
        manifest,
        LanceColumnOrdering {
            column_name,
            direction,
            null_order,
        }
    );
    abi_struct!(
        manifest,
        LanceProjectionColumn {
            output_name,
            expression,
        }
    );
    abi_struct!(
        manifest,
        LanceScanMetric {
            name,
            name_len,
            kind,
            value,
        }
    );
    abi_struct!(
        manifest,
        LanceScanStatistics {
            iops,
            requests,
            bytes_read,
            indices_loaded,
            index_partitions_loaded,
            index_comparisons,
            metrics,
            metrics_len,
        }
    );
    abi_struct!(
        manifest,
        LanceIndexSegmentBuildOptions {
            fragment_ids,
            fragment_count,
            index_uuid,
            ivf_centroids,
            ivf_centroids_schema,
            pq_codebook,
            pq_codebook_schema,
            mode,
        }
    );
    abi_struct!(
        manifest,
        LanceVectorIndexSegmentParams {
            index_type,
            metric,
            num_partitions,
            num_sub_vectors,
            num_bits,
            max_iterations,
            hnsw_m,
            hnsw_ef_construction,
            sample_rate,
        }
    );
    abi_struct!(
        manifest,
        LanceVectorIndexSegmentScope {
            fragment_ids,
            fragment_count,
            index_uuid,
        }
    );
    abi_struct!(
        manifest,
        LanceIndexPrewarmOptions {
            with_position,
            has_segment_selection,
            segment_uuids,
            segment_count,
        }
    );
    abi_struct!(
        manifest,
        LanceFtsMatchOptions {
            boost,
            fuzziness,
            max_expansions,
            match_operator,
            prefix_length,
            document_granularity,
        }
    );
    abi_struct!(
        manifest,
        LanceFtsPhraseOptions {
            slop,
            document_granularity,
        }
    );
    abi_struct!(
        manifest,
        LanceFtsMultiMatchOptions {
            boosts,
            boost_count,
            match_operator,
        }
    );
    abi_struct!(
        manifest,
        LanceFtsCombinedFieldsOptions {
            boosts,
            boost_count,
            match_operator,
        }
    );
    abi_struct!(manifest, LanceFtsBoostOptions { negative_boost });
    abi_struct!(
        manifest,
        LanceWriteParams {
            max_rows_per_file,
            max_rows_per_group,
            max_bytes_per_file,
            data_storage_version,
            enable_stable_row_ids,
        }
    );
    abi_struct!(
        manifest,
        LanceAutoCleanupParams {
            interval,
            older_than_seconds,
            older_than_nanos,
        }
    );
    abi_struct!(
        manifest,
        LanceWriteOptions {
            params,
            auto_cleanup,
            skip_auto_cleanup,
        }
    );
    abi_struct!(manifest, LanceUInt32List { items, count });
    abi_struct!(manifest, LanceStringList { items, count });
    abi_struct!(
        manifest,
        LanceBaseStoreParams {
            base_path,
            storage_options_accessor,
            has_block_size,
            block_size,
            has_s3_credentials_refresh_offset,
            s3_credentials_refresh_seconds,
            s3_credentials_refresh_nanos,
            has_use_constant_size_upload_parts,
            use_constant_size_upload_parts,
            has_list_is_lexically_ordered,
            list_is_lexically_ordered,
        }
    );
    abi_struct!(manifest, LanceBaseStoreParamsList { items, count });
    abi_struct!(
        manifest,
        LanceWriteStats {
            bytes_written,
            rows_written,
            files_written,
        }
    );
    abi_struct!(
        manifest,
        LanceWriteFragmentProgressInfo {
            fragment_id,
            has_physical_rows,
            physical_rows,
            file_count,
        }
    );
    abi_struct!(
        manifest,
        LanceNativeWriteProgress {
            begin_fragment,
            complete_fragment,
            write_stats,
            release,
            callback_ctx,
        }
    );
    abi_struct!(
        manifest,
        LanceNativeWriteOptionsSpec {
            write_options,
            transaction_properties,
            has_enable_v2_manifest_paths,
            enable_v2_manifest_paths,
            initial_bases,
            target_bases,
            target_base_names_or_paths,
            has_target_all_bases,
            target_all_bases,
            base_store_params,
            allow_external_blob_outside_bases,
            external_blob_mode,
            has_blob_pack_file_size_threshold,
            blob_pack_file_size_threshold,
            file_writer_options,
            session,
            storage_options_accessor,
            commit_lock,
            progress,
        }
    );
    abi_struct!(
        manifest,
        LanceFragmentCreateOptions {
            schema,
            mode,
            write,
            storage_opts,
            session,
        }
    );
    abi_struct!(manifest, LanceFieldMetadataUpdateMap { field_id, updates });
    abi_struct!(
        manifest,
        LanceUpdateConfigOperationOptions {
            config_updates,
            table_metadata_updates,
            schema_metadata_updates,
            field_metadata_updates,
            field_metadata_update_count,
        }
    );
    abi_struct!(
        manifest,
        LanceUncommittedWriteOptions {
            write,
            transaction_properties,
        }
    );
    abi_struct!(
        manifest,
        LanceCommitOptions {
            has_use_stable_row_ids,
            use_stable_row_ids,
            has_enable_v2_manifest_paths,
            enable_v2_manifest_paths,
            has_max_retries,
            max_retries,
            has_retry_timeout,
            retry_timeout_seconds,
            retry_timeout_nanos,
            timeout_mode,
            timeout_seconds,
            timeout_nanos,
            detached,
            skip_auto_cleanup,
            data_storage_version,
        }
    );
    abi_struct!(
        manifest,
        LanceTransactionOptions {
            uuid,
            tag,
            has_transaction_properties,
            property_keys,
            property_values,
            property_count,
        }
    );
    abi_struct!(
        manifest,
        LanceFragmentOffsetsView {
            fragment_id,
            offsets,
            offset_count,
        }
    );
    abi_struct!(manifest, LanceIndexFile { path, size_bytes });
    abi_struct!(
        manifest,
        LanceUpdateOperationOptions {
            removed_fragment_ids,
            removed_fragment_count,
            updated_fragments,
            updated_fragment_count,
            new_fragments,
            new_fragment_count,
            fields_modified,
            fields_modified_count,
            compacted_sstables,
            compacted_sstable_count,
            fields_for_preserving_frag_bitmap,
            fields_for_preserving_frag_bitmap_count,
            has_update_mode,
            update_mode,
            inserted_rows_filter,
            updated_fragment_offsets,
        }
    );
    abi_struct!(
        manifest,
        LanceStorageOptionsInput {
            keys,
            values,
            count,
        }
    );
    abi_struct!(
        manifest,
        LanceStorageOptionsFetchResult {
            has_options,
            keys,
            values,
            count,
            snapshot_ctx,
            release_snapshot,
        }
    );
    abi_struct!(
        manifest,
        LanceStorageOptionsProviderSpec {
            fetch,
            force_fetch,
            release,
            provider_ctx,
            provider_id,
        }
    );
    abi_struct!(
        manifest,
        LanceNamespaceResult {
            result_json,
            result_bytes,
            result_bytes_len,
            has_namespace_error,
            namespace_error_code,
            namespace_error_message,
        }
    );
    abi_struct!(
        manifest,
        LanceNamespaceContextProviderSpec {
            provide_context,
            release,
            provider_ctx,
        }
    );
    abi_struct!(
        manifest,
        LanceNamespaceCustomClientSpec {
            describe_table,
            describe_table_version,
            create_table_version,
            list_table_versions,
            release,
            client_ctx,
            namespace_id,
        }
    );
    abi_struct!(
        manifest,
        LanceNamespaceCustomClientDispatchSpec {
            dispatch,
            release,
            client_ctx,
            namespace_id,
        }
    );
    abi_struct!(
        manifest,
        LanceFileReaderOptions {
            has_columns,
            columns,
            column_count,
            cache_mode,
            has_read_chunk_size,
            read_chunk_size,
            has_batch_size_bytes,
            batch_size_bytes,
            has_cache_repetition_index,
            cache_repetition_index,
            has_validate_on_decode,
            validate_on_decode,
            has_inline_scheduling,
            inline_scheduling,
        }
    );
    abi_struct!(
        manifest,
        LanceFileWriterOptions {
            has_data_cache_bytes,
            data_cache_bytes,
            has_max_page_bytes,
            max_page_bytes,
            has_keep_original_array,
            keep_original_array,
            version,
        }
    );
    abi_struct!(
        manifest,
        LanceFileWriteSummary {
            num_rows,
            size_bytes,
        }
    );
    abi_struct!(
        manifest,
        LanceSchedulerConfig {
            io_buffer_size_bytes,
            use_lite_scheduler,
        }
    );
    abi_struct!(
        manifest,
        LanceScanStats {
            iops,
            requests,
            bytes_read,
        }
    );
    abi_struct!(
        manifest,
        LanceObjectStoreRegistryStats {
            hits,
            misses,
            active_stores,
        }
    );
    abi_struct!(
        manifest,
        LanceObjectStoreIoStats {
            read_iops,
            read_bytes,
            write_iops,
            written_bytes,
        }
    );
    abi_struct!(
        manifest,
        LanceObjectStoreInfo {
            block_size,
            max_iop_size,
            io_parallelism,
            is_local,
            is_cloud,
            prefers_lite_scheduler,
        }
    );
    abi_struct!(
        manifest,
        LanceFileMetadataInfo {
            format_version,
            major_version,
            minor_version,
            num_rows,
            num_data_bytes,
            num_column_metadata_bytes,
            num_global_buffer_bytes,
            num_footer_bytes,
            file_size_bytes,
            num_columns,
            num_physical_columns,
            num_global_buffers,
        }
    );
    abi_struct!(manifest, LanceBufferDescriptor { position, size });
    abi_struct!(
        manifest,
        LanceEncodingDescriptor {
            kind,
            buffer_location,
            buffer_length,
            direct_bytes,
            direct_bytes_len,
        }
    );
    abi_struct!(
        manifest,
        LancePageMetadata {
            buffers,
            buffer_count,
            num_rows,
            priority,
            encoding,
            encoding_description,
        }
    );
    abi_struct!(
        manifest,
        LanceColumnMetadata {
            column_buffers,
            column_buffer_count,
            pages,
            page_count,
            encoding,
        }
    );
    abi_struct!(
        manifest,
        LanceColumnStatistics {
            num_pages,
            size_bytes,
        }
    );
    abi_struct!(
        manifest,
        LanceFtsTokenView {
            text,
            text_len,
            position,
        }
    );
    abi_struct!(
        manifest,
        LanceDeltaTimestamp {
            unix_seconds,
            nanos,
        }
    );
    abi_struct!(
        manifest,
        LanceCompactedSsTable {
            shard_id,
            generation,
        }
    );
    abi_struct!(
        manifest,
        LanceMemWalHnswParams {
            index_name,
            has_num_edges,
            num_edges,
            has_ef_construction,
            ef_construction,
            has_max_level,
            max_level,
        }
    );
    abi_struct!(
        manifest,
        LanceShardWriterConfig {
            has_durable_write,
            durable_write,
            has_max_wal_buffer_size,
            max_wal_buffer_size,
            has_max_wal_flush_interval_ms,
            max_wal_flush_interval_ms,
            has_max_memtable_size,
            max_memtable_size,
            has_max_memtable_rows,
            max_memtable_rows,
            has_max_memtable_batches,
            max_memtable_batches,
            has_max_unflushed_memtable_bytes,
            max_unflushed_memtable_bytes,
            has_manifest_scan_batch_size,
            manifest_scan_batch_size,
            has_backpressure_log_interval_ms,
            backpressure_log_interval_ms,
            has_stats_log_interval_ms,
            stats_log_interval_ms,
            hnsw_params,
            num_hnsw_params,
        }
    );
    abi_struct!(
        manifest,
        LanceShardWriterStats {
            put_count,
            put_time_nanos,
            wal_flush_count,
            wal_flush_time_nanos,
            wal_flush_bytes,
            wal_io_time_nanos,
            wal_io_count,
            index_update_time_nanos,
            index_update_count,
            index_update_rows,
            memtable_flush_count,
            memtable_flush_time_nanos,
            memtable_flush_rows,
            memtable_flush_failures,
        }
    );
    abi_struct!(
        manifest,
        LanceShardMemTableStats {
            is_live,
            row_count,
            batch_count,
            estimated_size_bytes,
            index_bytes,
            frozen_bytes,
            grace_bytes,
            retained_bytes,
            generation,
            has_max_buffered_batch_position,
            max_buffered_batch_position,
            durable_batch_count,
            global_offset,
            has_pending_wal_start_batch_position,
            pending_wal_start_batch_position,
            has_pending_wal_end_batch_position,
            pending_wal_end_batch_position,
            pending_wal_batch_count,
            pending_wal_row_count,
            pending_wal_estimated_bytes,
            frozen_count,
        }
    );
    abi_struct!(
        manifest,
        LanceShardWriteResult {
            batch_position_begin,
            batch_position_end,
        }
    );

    // Typed enums. Each C enumerator is associated with its Rust variant by
    // name. Numeric values are read from the Rust definition and compared with
    // the value compiled from the C header.
    abi_enum!(manifest, LanceErrorCode {
        LANCE_OK => Ok,
        LANCE_ERR_INVALID_ARGUMENT => InvalidArgument,
        LANCE_ERR_IO => IoError,
        LANCE_ERR_NOT_FOUND => NotFound,
        LANCE_ERR_DATASET_ALREADY_EXISTS => DatasetAlreadyExists,
        LANCE_ERR_INDEX => IndexError,
        LANCE_ERR_INTERNAL => Internal,
        LANCE_ERR_NOT_SUPPORTED => NotSupported,
        LANCE_ERR_COMMIT_CONFLICT => CommitConflict,
        LANCE_ERR_PANIC => Panic,
    });
    abi_enum!(manifest, LanceVectorIndexType {
        LANCE_INDEX_IVF_FLAT => IvfFlat,
        LANCE_INDEX_IVF_SQ => IvfSq,
        LANCE_INDEX_IVF_PQ => IvfPq,
        LANCE_INDEX_IVF_HNSW_SQ => IvfHnswSq,
        LANCE_INDEX_IVF_HNSW_PQ => IvfHnswPq,
        LANCE_INDEX_IVF_HNSW_FLAT => IvfHnswFlat,
        LANCE_INDEX_IVF_RQ => IvfRq,
    });
    abi_enum!(manifest, LanceScalarIndexType {
        LANCE_SCALAR_BTREE => BTree,
        LANCE_SCALAR_BITMAP => Bitmap,
        LANCE_SCALAR_LABEL_LIST => LabelList,
        LANCE_SCALAR_INVERTED => Inverted,
        LANCE_SCALAR_NGRAM => NGram,
        LANCE_SCALAR_ZONE_MAP => ZoneMap,
        LANCE_SCALAR_BLOOM_FILTER => BloomFilter,
        LANCE_SCALAR_MINHASH_LSH => MinHashLsh,
    });
    abi_enum!(manifest, LanceMetricType {
        LANCE_METRIC_L2 => L2,
        LANCE_METRIC_COSINE => Cosine,
        LANCE_METRIC_DOT => Dot,
        LANCE_METRIC_HAMMING => Hamming,
    });
    abi_enum!(manifest, LanceApproxMode {
        LANCE_APPROX_MODE_FAST => Fast,
        LANCE_APPROX_MODE_NORMAL => Normal,
        LANCE_APPROX_MODE_ACCURATE => Accurate,
    });
    abi_enum!(manifest, LanceDataType {
        LANCE_DTYPE_FLOAT32 => Float32,
        LANCE_DTYPE_FLOAT16 => Float16,
        LANCE_DTYPE_FLOAT64 => Float64,
        LANCE_DTYPE_UINT8 => UInt8,
        LANCE_DTYPE_INT8 => Int8,
    });
    abi_enum!(manifest, LanceIndexFileVersion {
        LANCE_INDEX_FILE_VERSION_V3 => V3,
        LANCE_INDEX_FILE_VERSION_LEGACY => Legacy,
    });
    abi_enum!(manifest, LanceSessionCacheSpecKind {
        LANCE_SESSION_CACHE_SPEC_DEFAULT => Default,
        LANCE_SESSION_CACHE_SPEC_SIZE => Size,
        LANCE_SESSION_CACHE_SPEC_URI => Uri,
        LANCE_SESSION_CACHE_SPEC_CONFIG => Config,
    });
    abi_enum!(manifest, LanceMetadataUpdateMode {
        LANCE_METADATA_UPDATE_MERGE => Merge,
        LANCE_METADATA_UPDATE_REPLACE => Replace,
    });
    abi_enum!(manifest, LanceMergeWhenMatched {
        LANCE_MERGE_WHEN_MATCHED_DO_NOTHING => DoNothing,
        LANCE_MERGE_WHEN_MATCHED_UPDATE_ALL => UpdateAll,
        LANCE_MERGE_WHEN_MATCHED_UPDATE_IF => UpdateIf,
        LANCE_MERGE_WHEN_MATCHED_FAIL => Fail,
        LANCE_MERGE_WHEN_MATCHED_DELETE => Delete,
    });
    abi_enum!(manifest, LanceMergeWhenNotMatched {
        LANCE_MERGE_WHEN_NOT_MATCHED_INSERT_ALL => InsertAll,
        LANCE_MERGE_WHEN_NOT_MATCHED_DO_NOTHING => DoNothing,
    });
    abi_enum!(manifest, LanceMergeWhenNotMatchedBySource {
        LANCE_MERGE_WHEN_NOT_MATCHED_BY_SOURCE_KEEP => Keep,
        LANCE_MERGE_WHEN_NOT_MATCHED_BY_SOURCE_DELETE => Delete,
        LANCE_MERGE_WHEN_NOT_MATCHED_BY_SOURCE_DELETE_IF => DeleteIf,
    });
    abi_enum!(manifest, LanceCompactionMode {
        LANCE_COMPACTION_REENCODE => Reencode,
        LANCE_COMPACTION_TRY_BINARY_COPY => TryBinaryCopy,
        LANCE_COMPACTION_FORCE_BINARY_COPY => ForceBinaryCopy,
    });
    abi_enum!(manifest, LanceIndexRemapMode {
        LANCE_INDEX_REMAP_COMPACT => Compact,
        LANCE_INDEX_REMAP_DIRECT => Direct,
    });
    abi_enum!(manifest, LanceColumnNullableMode {
        LANCE_COLUMN_NULLABLE_UNCHANGED => Unchanged,
        LANCE_COLUMN_NULLABLE_TRUE => True,
        LANCE_COLUMN_NULLABLE_FALSE => False,
    });
    abi_enum!(manifest, LanceOrderDirection {
        LANCE_ORDER_ASCENDING => Ascending,
        LANCE_ORDER_DESCENDING => Descending,
    });
    abi_enum!(manifest, LanceNullOrder {
        LANCE_ORDER_NULLS_LAST => NullsLast,
        LANCE_ORDER_NULLS_FIRST => NullsFirst,
    });
    abi_enum!(manifest, LanceMaterializationStyle {
        LANCE_MATERIALIZATION_STYLE_HEURISTIC => Heuristic,
        LANCE_MATERIALIZATION_STYLE_ALL_LATE => AllLate,
        LANCE_MATERIALIZATION_STYLE_ALL_EARLY => AllEarly,
        LANCE_MATERIALIZATION_STYLE_ALL_EARLY_EXCEPT => AllEarlyExcept,
    });
    abi_enum!(manifest, LanceScanMetricKind {
        LANCE_SCAN_METRIC_COUNT => Count,
        LANCE_SCAN_METRIC_TIME_NANOSECONDS => TimeNanoseconds,
    });
    abi_enum!(manifest, LancePollStatus {
        LANCE_POLL_READY => Ready,
        LANCE_POLL_PENDING => Pending,
        LANCE_POLL_FINISHED => Finished,
        LANCE_POLL_ERROR => Error,
    });
    abi_enum!(manifest, LanceIndexBuildProgressEvent {
        LANCE_INDEX_BUILD_PROGRESS_STAGE_START => StageStart,
        LANCE_INDEX_BUILD_PROGRESS_STAGE_PROGRESS => StageProgress,
        LANCE_INDEX_BUILD_PROGRESS_STAGE_COMPLETE => StageComplete,
    });
    abi_enum!(manifest, LanceIndexSegmentBuildMode {
        LANCE_INDEX_SEGMENT_BUILD_AUTO => Auto,
        LANCE_INDEX_SEGMENT_BUILD_LOCAL_TRAIN => LocalTrain,
        LANCE_INDEX_SEGMENT_BUILD_PRECOMPUTED => Precomputed,
    });
    abi_enum!(manifest, LanceFtsCoverageMode {
        LANCE_FTS_COVERAGE_STRICT => Strict,
        LANCE_FTS_COVERAGE_INDEX_ONLY => IndexOnly,
    });
    abi_enum!(manifest, LanceFtsMatchOperator {
        LANCE_FTS_MATCH_OPERATOR_OR => Or,
        LANCE_FTS_MATCH_OPERATOR_AND => And,
    });
    abi_enum!(manifest, LanceFtsDocumentGranularity {
        LANCE_FTS_DOCUMENT_GRANULARITY_UNSPECIFIED => Unspecified,
        LANCE_FTS_DOCUMENT_GRANULARITY_ROW => Row,
        LANCE_FTS_DOCUMENT_GRANULARITY_LIST_ELEMENT => ListElement,
    });
    abi_enum!(manifest, LanceFtsOccur {
        LANCE_FTS_OCCUR_SHOULD => Should,
        LANCE_FTS_OCCUR_MUST => Must,
        LANCE_FTS_OCCUR_MUST_NOT => MustNot,
    });
    abi_enum!(manifest, LanceWriteMode {
        LANCE_WRITE_CREATE => Create,
        LANCE_WRITE_APPEND => Append,
        LANCE_WRITE_OVERWRITE => Overwrite,
    });
    abi_enum!(manifest, LanceOperationKind {
        LANCE_OPERATION_UNKNOWN => Unknown,
        LANCE_OPERATION_APPEND => Append,
        LANCE_OPERATION_DELETE => Delete,
        LANCE_OPERATION_OVERWRITE => Overwrite,
        LANCE_OPERATION_CREATE_INDEX => CreateIndex,
        LANCE_OPERATION_REWRITE => Rewrite,
        LANCE_OPERATION_DATA_REPLACEMENT => DataReplacement,
        LANCE_OPERATION_DATA_OVERLAY => DataOverlay,
        LANCE_OPERATION_MERGE => Merge,
        LANCE_OPERATION_RESTORE => Restore,
        LANCE_OPERATION_RESERVE_FRAGMENTS => ReserveFragments,
        LANCE_OPERATION_UPDATE => Update,
        LANCE_OPERATION_PROJECT => Project,
        LANCE_OPERATION_UPDATE_CONFIG => UpdateConfig,
        LANCE_OPERATION_UPDATE_MEM_WAL_STATE => UpdateMemWalState,
        LANCE_OPERATION_CLONE => Clone,
        LANCE_OPERATION_UPDATE_BASES => UpdateBases,
    });
    abi_enum!(manifest, LanceCommitTimeoutMode {
        LANCE_COMMIT_TIMEOUT_DEFAULT => Default,
        LANCE_COMMIT_TIMEOUT_DISABLED => Disabled,
        LANCE_COMMIT_TIMEOUT_EXPLICIT => Explicit,
    });
    abi_enum!(manifest, LanceUpdateMode {
        LANCE_UPDATE_MODE_REWRITE_ROWS => RewriteRows,
        LANCE_UPDATE_MODE_REWRITE_COLUMNS => RewriteColumns,
    });
    abi_enum!(manifest, LanceKeyExistenceFilterKind {
        LANCE_KEY_EXISTENCE_FILTER_EXACT => Exact,
        LANCE_KEY_EXISTENCE_FILTER_BLOOM => Bloom,
    });
    abi_enum!(manifest, LanceFileCacheMode {
        LANCE_FILE_CACHE_NONE => None,
        LANCE_FILE_CACHE_SESSION => Session,
    });
    abi_enum!(manifest, LanceSchedulerLiteMode {
        LANCE_SCHEDULER_LITE_AUTO => Auto,
        LANCE_SCHEDULER_LITE_STANDARD => Standard,
        LANCE_SCHEDULER_LITE_LITE => Lite,
    });
    abi_enum!(manifest, LanceFileFormatVersion {
        LANCE_FILE_FORMAT_V1 => V1,
        LANCE_FILE_FORMAT_V2_0 => V2_0,
        LANCE_FILE_FORMAT_V2_1 => V2_1,
        LANCE_FILE_FORMAT_V2_2 => V2_2,
        LANCE_FILE_FORMAT_V2_3 => V2_3,
    });
    abi_enum!(manifest, LanceFileEncodingKind {
        LANCE_FILE_ENCODING_ABSENT => Absent,
        LANCE_FILE_ENCODING_NONE => None,
        LANCE_FILE_ENCODING_DIRECT => Direct,
        LANCE_FILE_ENCODING_INDIRECT => Indirect,
    });

    // Raw int32 constant families. The public C header declares these as enums,
    // but Rust consumes them as plain `i32` values and has no Rust enum with
    // these semantics. They are checked as named constants, not as Rust enums.
    abi_constants!(manifest, LanceCleanupFileKind {
        LANCE_CLEANUP_FILE_MANIFEST = 0,
        LANCE_CLEANUP_FILE_DATA = 1,
        LANCE_CLEANUP_FILE_TRANSACTION = 2,
        LANCE_CLEANUP_FILE_INDEX = 3,
        LANCE_CLEANUP_FILE_DELETION = 4,
        LANCE_CLEANUP_FILE_TEMPORARY_MANIFEST = 5,
    });
    abi_constants!(manifest, LanceFieldReferenceKind {
        LANCE_FIELD_REFERENCE_ID = 0,
        LANCE_FIELD_REFERENCE_PATH = 1,
    });
    abi_constants!(manifest, LanceBlobRowSelector {
        LANCE_BLOB_ROWS_NONE = 0,
        LANCE_BLOB_ROWS_IDS = 1,
        LANCE_BLOB_ROWS_ADDRESSES = 2,
        LANCE_BLOB_ROWS_INDICES = 3,
    });
    abi_constants!(manifest, LanceBlobReadOrder {
        LANCE_BLOB_READ_PRESERVE_ORDER = 0,
        LANCE_BLOB_READ_UNORDERED = 1,
    });
    abi_constants!(manifest, LanceBlobHandling {
        LANCE_BLOB_HANDLING_BLOBS_DESCRIPTIONS = 0,
        LANCE_BLOB_HANDLING_ALL_BINARY = 1,
        LANCE_BLOB_HANDLING_ALL_DESCRIPTIONS = 2,
    });
    abi_constants!(manifest, LanceExternalBlobMode {
        LANCE_EXTERNAL_BLOB_REFERENCE = 0,
        LANCE_EXTERNAL_BLOB_INGEST = 1,
    });
    abi_constants!(manifest, LanceDataOverlayCoverageKind {
        LANCE_DATA_OVERLAY_COVERAGE_SHARED = 0,
        LANCE_DATA_OVERLAY_COVERAGE_PER_FIELD = 1,
    });
    abi_constants!(manifest, LanceMemWalShardingMode {
        LANCE_MEM_WAL_SHARDING_MANUAL = 0,
        LANCE_MEM_WAL_SHARDING_UNSHARDED = 1,
        LANCE_MEM_WAL_SHARDING_BUCKET = 2,
        LANCE_MEM_WAL_SHARDING_IDENTITY = 3,
    });
    abi_constants!(manifest, LanceMemWalDistanceType {
        LANCE_MEM_WAL_DISTANCE_L2 = 0,
        LANCE_MEM_WAL_DISTANCE_COSINE = 1,
        LANCE_MEM_WAL_DISTANCE_DOT = 2,
        LANCE_MEM_WAL_DISTANCE_HAMMING = 3,
    });

    // Arrow C Data Interface structs are plain C structs, not typedef aliases.
    // Their fields are private in arrow-rs, so the public check covers the
    // complete type size and alignment rather than individual offsets.
    abi_arrow!(
        manifest,
        ArrowSchema => arrow::ffi::FFI_ArrowSchema,
        ArrowArray => arrow::ffi::FFI_ArrowArray,
        ArrowArrayStream => arrow::ffi_stream::FFI_ArrowArrayStream,
    );

    // Pointer-only handles. Their definitions are private to Rust; callers only
    // ever receive pointers, so size and layout are intentionally not compared.
    // The header scan verifies this list covers every forward declaration that
    // has no complete definition.
    abi_opaque!(
        manifest,
        LanceBatch,
        LanceBlobDescriptorArrayBuilder,
        LanceBlobFile,
        LanceByteBuffer,
        LanceCleanupExplanation,
        LanceCommitLock,
        LanceCompactionOptionsV2,
        LanceCompactionPlan,
        LanceCompactionTask,
        LanceDataFile,
        LanceDataOverlayFile,
        LanceDataOverlayGroup,
        LanceDataReplacementGroup,
        LanceDataStatistics,
        LanceDataset,
        LanceDatasetConfig,
        LanceDatasetDelta,
        LanceDatasetDeltaBuilder,
        LanceDedicatedBlobWriter,
        LanceDelimitedList,
        LanceFileMetadata,
        LanceFileReader,
        LanceFileSession,
        LanceFileStatistics,
        LanceFileWriter,
        LanceFragment,
        LanceFragmentOverlayWriter,
        LanceFragmentUpdateColumnsResult,
        LanceFtsQuery,
        LanceFtsQueryContext,
        LanceFtsTokenList,
        LanceIndexSegmentBuilder,
        LanceIndexSegmentMetadata,
        LanceIndexSegmentResult,
        LanceIvfModel,
        LanceKeyExistenceFilter,
        LanceLsmPointLookupPlanner,
        LanceLsmScanner,
        LanceLsmVectorSearchPlanner,
        LanceMemWalIndexDetails,
        LanceMemWalPlan,
        LanceMemWalShardSnapshot,
        LanceMemWalShardingField,
        LanceMemWalShardingSpec,
        LanceNamespace,
        LanceNativeWriteOptions,
        LanceOperation,
        LanceOptimizeIndexMerge,
        LanceOptimizeIndexTask,
        LanceOptimizeIndicesOptions,
        LanceOptimizeIndicesPlan,
        LancePackedBlobWriter,
        LancePathList,
        LanceRef,
        LanceRestAdapter,
        LanceRewriteGroup,
        LanceRewriteResult,
        LanceRewrittenIndex,
        LanceScanner,
        LanceSchema,
        LanceSession,
        LanceShardWriter,
        LanceSqlQuery,
        LanceSqlQueryBuilder,
        LanceStorageOptionsAccessor,
        LanceStorageOptionsProvider,
        LanceStorageOptionsSnapshot,
        LanceTag,
        LanceTags,
        LanceTransaction,
        LanceUpdateMap,
        LanceUpdatedFragmentOffsets,
        LanceVersion,
        LanceVersionRefs,
        LanceVersions,
    );

    manifest
}

/// Public declarations found in the header. This inventory is used only to
/// prove the explicit manifest is complete; probe records never pair C symbols
/// with Rust symbols by position or numeric value.
#[derive(Debug, Default)]
struct HeaderInventory {
    enums: BTreeMap<String, Vec<String>>,
    structs: BTreeMap<String, Vec<String>>,
    plain_structs: BTreeMap<String, Vec<String>>,
    opaque: BTreeSet<String>,
}

/// Compare a C header with the Rust implementation through the explicit table.
pub(crate) fn assert_public_abi() {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let header_path = manifest_dir.join("include").join("lance").join("lance.h");
    let header = std::fs::read_to_string(&header_path)
        .unwrap_or_else(|error| panic!("failed to read {}: {error}", header_path.display()));

    let inventory = scan_header(&header);
    let manifest = manifest();
    validate_inventory(&inventory, &manifest);

    let c_records = run_c_probe(&manifest, &manifest_dir.join("include"));
    let rust_only: Vec<_> = manifest.records.difference(&c_records).cloned().collect();
    let c_only: Vec<_> = c_records.difference(&manifest.records).cloned().collect();

    assert!(
        rust_only.is_empty() && c_only.is_empty(),
        "public C ABI in include/lance/lance.h does not match the Rust repr(C) implementation:\n\
         records present in Rust but not in the compiled C header ({}):\n{}\n\
         records present in the compiled C header but not in Rust ({}):\n{}",
        rust_only.len(),
        rust_only.join("\n"),
        c_only.len(),
        c_only.join("\n"),
    );
}

fn validate_inventory(inventory: &HeaderInventory, manifest: &AbiManifest) {
    let mut problems = Vec::new();

    let mut declared_types = BTreeSet::new();
    for name in manifest
        .structs
        .keys()
        .copied()
        .chain(manifest.enums.keys().copied())
        .chain(manifest.constants.keys().copied())
        .chain(manifest.arrow.iter().copied())
    {
        if !declared_types.insert(name) {
            problems.push(format!(
                "`{name}` appears more than once in the ABI manifest"
            ));
        }
    }
    for name in &manifest.opaque {
        if declared_types.contains(name) {
            problems.push(format!(
                "opaque handle `{name}` is also declared as a complete type"
            ));
        }
    }

    let manifest_enum_families: BTreeSet<String> = manifest
        .enums
        .keys()
        .copied()
        .chain(manifest.constants.keys().copied())
        .map(str::to_owned)
        .collect();
    let header_enum_families: BTreeSet<String> = inventory.enums.keys().cloned().collect();
    compare_names(
        "enum family",
        "header",
        &manifest_enum_families,
        &header_enum_families,
        &mut problems,
    );

    for (name, variants) in &manifest.enums {
        let expected: BTreeSet<String> = variants
            .iter()
            .map(|(c_name, _)| (*c_name).to_owned())
            .collect();
        if expected.len() != variants.len() {
            problems.push(format!(
                "enum `{name}` has duplicate C enumerator names in the ABI manifest"
            ));
        }
        if let Some(actual) = inventory.enums.get(*name) {
            let actual: BTreeSet<String> = actual.iter().cloned().collect();
            compare_names("enumerator", name, &expected, &actual, &mut problems);
        }
    }

    for (name, constants) in &manifest.constants {
        let expected: BTreeSet<String> = constants
            .iter()
            .map(|(c_name, _)| (*c_name).to_owned())
            .collect();
        if expected.len() != constants.len() {
            problems.push(format!(
                "constant family `{name}` has duplicate C names in the ABI manifest"
            ));
        }
        if let Some(actual) = inventory.enums.get(*name) {
            let actual: BTreeSet<String> = actual.iter().cloned().collect();
            compare_names("constant", name, &expected, &actual, &mut problems);
        }
    }

    let manifest_structs: BTreeSet<String> = manifest
        .structs
        .keys()
        .copied()
        .chain(manifest.arrow.iter().copied())
        .map(str::to_owned)
        .collect();
    let header_structs: BTreeSet<String> = inventory
        .structs
        .keys()
        .chain(inventory.plain_structs.keys())
        .cloned()
        .collect();
    compare_names(
        "public struct",
        "header",
        &manifest_structs,
        &header_structs,
        &mut problems,
    );

    for (name, fields) in &manifest.structs {
        let expected: BTreeSet<String> = fields.iter().map(|field| (*field).to_owned()).collect();
        if expected.len() != fields.len() {
            problems.push(format!(
                "struct `{name}` has duplicate field names in the ABI manifest"
            ));
        }
        if let Some(actual) = inventory.structs.get(*name) {
            let actual: BTreeSet<String> = actual.iter().cloned().collect();
            compare_names("field", name, &expected, &actual, &mut problems);
        }
    }

    for name in &manifest.arrow {
        if !inventory.plain_structs.contains_key(*name) {
            problems.push(format!(
                "Arrow C Data Interface struct `struct {name}` is not a plain struct definition in the public header"
            ));
        }
    }

    let manifest_opaque: BTreeSet<String> = manifest
        .opaque
        .iter()
        .map(|name| (*name).to_owned())
        .collect();
    let header_opaque: BTreeSet<String> = inventory
        .opaque
        .iter()
        .filter(|name| !inventory.structs.contains_key(*name))
        .cloned()
        .collect();
    compare_names(
        "opaque handle",
        "header",
        &manifest_opaque,
        &header_opaque,
        &mut problems,
    );

    assert!(
        problems.is_empty(),
        "the explicit ABI manifest does not cover include/lance/lance.h:\n{}",
        problems.join("\n"),
    );
}

fn compare_names(
    kind: &str,
    owner: &str,
    expected: &BTreeSet<String>,
    actual: &BTreeSet<String>,
    problems: &mut Vec<String>,
) {
    for name in expected.difference(actual) {
        problems.push(format!(
            "{kind} `{owner}::{name}` is in the ABI manifest but not in the public header"
        ));
    }
    for name in actual.difference(expected) {
        problems.push(format!(
            "{kind} `{owner}::{name}` is in the public header but not in the ABI manifest"
        ));
    }
}

fn run_c_probe(manifest: &AbiManifest, include_dir: &Path) -> BTreeSet<String> {
    let source = generate_c_probe(manifest);
    let build_dir = tempfile::tempdir().expect("failed to create ABI probe build directory");
    let source_path = build_dir.path().join("abi_layout_probe.c");
    let binary_path = build_dir.path().join("abi_layout_probe");
    std::fs::write(&source_path, &source).expect("failed to write ABI probe source");

    let compiler = std::env::var("CC").unwrap_or_else(|_| "clang".to_owned());
    let compile = Command::new(&compiler)
        .args(["-std=c11", "-Wall", "-Wextra", "-Werror"])
        .arg(format!("-I{}", include_dir.display()))
        .arg(&source_path)
        .arg("-o")
        .arg(&binary_path)
        .output()
        .unwrap_or_else(|error| panic!("failed to run C compiler `{compiler}`: {error}"));
    assert!(
        compile.status.success(),
        "C11 ABI probe failed to compile with `{compiler}`:\n{}\n--- probe source ---\n{source}",
        String::from_utf8_lossy(&compile.stderr),
    );

    let run = Command::new(&binary_path)
        .output()
        .expect("failed to run compiled ABI probe");
    assert!(
        run.status.success(),
        "compiled ABI probe failed:\nstdout:\n{}\nstderr:\n{}",
        String::from_utf8_lossy(&run.stdout),
        String::from_utf8_lossy(&run.stderr),
    );

    String::from_utf8(run.stdout)
        .expect("ABI probe emitted non-UTF-8 output")
        .lines()
        .map(str::to_owned)
        .collect()
}

fn generate_c_probe(manifest: &AbiManifest) -> String {
    let mut source = String::from(
        r#"#include <stddef.h>
#include <stdio.h>

#include "lance/lance.h"

#define ABI_STRUCT(name) \
    do { \
        printf("struct-size|" #name "|%zu\n", sizeof(name)); \
        printf("struct-align|" #name "|%zu\n", _Alignof(name)); \
    } while (0)

#define ABI_FIELD(name, field) \
    printf("field-offset|" #name "|" #field "|%zu\n", offsetof(name, field))

#define ABI_ENUM(name) \
    do { \
        printf("enum-size|" #name "|%zu\n", sizeof(name)); \
        printf("enum-align|" #name "|%zu\n", _Alignof(name)); \
    } while (0)

#define ABI_ENUM_VALUE(name, value) \
    printf("enum-value|" #name "|" #value "|%lld\n", (long long)(value))

#define ABI_CONSTANT(name, value) \
    printf("constant|" #name "|" #value "|%lld\n", (long long)(value))

#define ABI_ARROW(name) \
    do { \
        printf("arrow-size|" #name "|%zu\n", sizeof(struct name)); \
        printf("arrow-align|" #name "|%zu\n", _Alignof(struct name)); \
    } while (0)

int main(void) {
"#,
    );

    for (name, fields) in &manifest.structs {
        writeln!(source, "    ABI_STRUCT({name});").unwrap();
        for field in fields {
            writeln!(source, "    ABI_FIELD({name}, {field});").unwrap();
        }
    }
    for (name, variants) in &manifest.enums {
        writeln!(source, "    ABI_ENUM({name});").unwrap();
        for (c_name, _) in variants {
            writeln!(source, "    ABI_ENUM_VALUE({name}, {c_name});").unwrap();
        }
    }
    for (name, constants) in &manifest.constants {
        for (c_name, _) in constants {
            writeln!(source, "    ABI_CONSTANT({name}, {c_name});").unwrap();
        }
    }
    for name in &manifest.arrow {
        writeln!(source, "    ABI_ARROW({name});").unwrap();
    }
    source.push_str("    return 0;\n}\n");
    source
}

fn scan_header(source: &str) -> HeaderInventory {
    let source = strip_comments(source);
    let tokens = tokenize(&source);
    let mut inventory = HeaderInventory::default();
    let mut index = 0;

    while index < tokens.len() {
        if tokens[index] == "typedef" {
            match tokens.get(index + 1).map(String::as_str) {
                Some("enum") => {
                    let mut position = index + 2;
                    if tokens.get(position).is_some_and(|token| is_ident(token))
                        && tokens.get(position + 1).map(String::as_str) == Some("{")
                    {
                        position += 1;
                    }
                    if tokens.get(position).map(String::as_str) == Some("{") {
                        let (body, after) = take_braced(&tokens, position);
                        if let Some(alias) = tokens.get(after)
                            && is_ident(alias)
                        {
                            inventory
                                .enums
                                .insert(alias.clone(), enumerator_names(&body));
                        }
                        index = after + 1;
                        continue;
                    }
                }
                Some("struct") => {
                    let mut position = index + 2;
                    let tag = if tokens.get(position).is_some_and(|token| is_ident(token)) {
                        position += 1;
                        Some(tokens[position - 1].clone())
                    } else {
                        None
                    };
                    if tokens.get(position).map(String::as_str) == Some("{") {
                        let (body, after) = take_braced(&tokens, position);
                        if let Some(alias) = tokens.get(after)
                            && is_ident(alias)
                        {
                            inventory.structs.insert(alias.clone(), field_names(&body));
                        }
                        index = after + 1;
                        continue;
                    }
                    if tokens.get(position).map(String::as_str) == Some(";") {
                        if let Some(tag) = tag {
                            inventory.opaque.insert(tag);
                        }
                        index = position + 1;
                        continue;
                    }
                    if tokens.get(position).is_some_and(|token| is_ident(token))
                        && tokens.get(position + 1).map(String::as_str) == Some(";")
                    {
                        inventory.opaque.insert(tokens[position].clone());
                        index = position + 2;
                        continue;
                    }
                }
                _ => {}
            }
        } else if tokens[index] == "struct"
            && tokens.get(index + 1).is_some_and(|token| is_ident(token))
            && tokens.get(index + 2).map(String::as_str) == Some("{")
        {
            let name = tokens[index + 1].clone();
            let (body, after) = take_braced(&tokens, index + 2);
            inventory.plain_structs.insert(name, field_names(&body));
            index = after + 1;
            continue;
        }
        index += 1;
    }

    inventory
}

fn strip_comments(source: &str) -> String {
    let mut output = String::with_capacity(source.len());
    let mut chars = source.chars().peekable();
    while let Some(ch) = chars.next() {
        if ch == '/' {
            match chars.peek() {
                Some('/') => {
                    chars.next();
                    for comment_ch in chars.by_ref() {
                        if comment_ch == '\n' {
                            output.push('\n');
                            break;
                        }
                    }
                }
                Some('*') => {
                    chars.next();
                    let mut previous = '\0';
                    for comment_ch in chars.by_ref() {
                        if previous == '*' && comment_ch == '/' {
                            break;
                        }
                        previous = comment_ch;
                    }
                    output.push(' ');
                }
                _ => output.push(ch),
            }
        } else {
            output.push(ch);
        }
    }
    output
}

fn tokenize(source: &str) -> Vec<String> {
    let mut tokens = Vec::new();
    let mut chars = source.char_indices().peekable();
    while let Some((start, ch)) = chars.next() {
        if ch.is_ascii_alphanumeric() || ch == '_' {
            let mut end = start + ch.len_utf8();
            while let Some(&(next, next_ch)) = chars.peek() {
                if next_ch.is_ascii_alphanumeric() || next_ch == '_' {
                    end = next + next_ch.len_utf8();
                    chars.next();
                } else {
                    break;
                }
            }
            tokens.push(source[start..end].to_owned());
        } else if !ch.is_whitespace() {
            tokens.push(ch.to_string());
        }
    }
    tokens
}

fn is_ident(token: &str) -> bool {
    let mut chars = token.chars();
    match chars.next() {
        Some(first) if first.is_ascii_alphabetic() || first == '_' => {
            chars.all(|ch| ch.is_ascii_alphanumeric() || ch == '_')
        }
        _ => false,
    }
}

fn take_braced(tokens: &[String], open: usize) -> (Vec<String>, usize) {
    assert_eq!(tokens[open], "{", "expected an opening brace");
    let mut depth = 0usize;
    let mut body = Vec::new();
    for (offset, token) in tokens[open..].iter().enumerate() {
        match token.as_str() {
            "{" => {
                depth += 1;
                if depth > 1 {
                    body.push(token.clone());
                }
            }
            "}" => {
                depth -= 1;
                if depth == 0 {
                    return (body, open + offset + 1);
                }
                body.push(token.clone());
            }
            _ => {
                if depth > 0 {
                    body.push(token.clone());
                }
            }
        }
    }
    panic!("unterminated brace block in public header");
}

fn enumerator_names(body: &[String]) -> Vec<String> {
    let mut names = Vec::new();
    let mut depth = 0usize;
    let mut expect_name = true;
    for token in body {
        match token.as_str() {
            "(" | "[" | "{" => depth += 1,
            ")" | "]" | "}" => depth = depth.saturating_sub(1),
            "," if depth == 0 => {
                expect_name = true;
                continue;
            }
            _ => {}
        }
        if expect_name && depth == 0 && is_ident(token) {
            names.push(token.clone());
            expect_name = false;
        }
    }
    names
}

fn field_names(body: &[String]) -> Vec<String> {
    let mut declarations = Vec::new();
    let mut depth = 0usize;
    let mut current = Vec::new();
    for token in body {
        match token.as_str() {
            "(" | "[" | "{" => {
                depth += 1;
                current.push(token.clone());
            }
            ")" | "]" | "}" => {
                depth = depth.saturating_sub(1);
                current.push(token.clone());
            }
            ";" if depth == 0 => declarations.push(std::mem::take(&mut current)),
            _ => current.push(token.clone()),
        }
    }
    if !current.is_empty() {
        declarations.push(current);
    }

    let mut fields = Vec::new();
    for declaration in declarations {
        if declaration.is_empty() {
            continue;
        }
        // A function-pointer declarator looks like `(*name)`. Parameter
        // names such as `struct Foo* self` are nested one parenthesis deeper,
        // so only inspect `*` tokens at the declarator's own depth.
        let mut depth = 0usize;
        let mut is_function_pointer = false;
        for (index, token) in declaration.iter().enumerate() {
            match token.as_str() {
                "(" => depth += 1,
                ")" => depth = depth.saturating_sub(1),
                "*" if depth == 1 => {
                    if declaration
                        .get(index + 1)
                        .is_some_and(|name| is_ident(name))
                        && declaration.get(index + 2).map(String::as_str) == Some(")")
                    {
                        fields.push(declaration[index + 1].clone());
                        is_function_pointer = true;
                    }
                }
                _ => {}
            }
        }
        if is_function_pointer {
            continue;
        }

        let mut depth = 0usize;
        let mut start = 0;
        for (index, token) in declaration.iter().enumerate() {
            match token.as_str() {
                "(" | "[" | "{" => depth += 1,
                ")" | "]" | "}" => depth = depth.saturating_sub(1),
                "," if depth == 0 => {
                    fields.push(declarator_name(&declaration[start..index]));
                    start = index + 1;
                }
                _ => {}
            }
        }
        fields.push(declarator_name(&declaration[start..]));
    }
    fields
}

fn declarator_name(declaration: &[String]) -> String {
    let end = declaration
        .iter()
        .position(|token| token == "[" || token == "=")
        .unwrap_or(declaration.len());
    declaration[..end]
        .iter()
        .rev()
        .find(|token| is_ident(token))
        .cloned()
        .unwrap_or_else(|| panic!("cannot find a field name in {declaration:?}"))
}

#[test]
fn header_scanner_recognizes_public_declarations() {
    let header = r#"
        // typedef struct Ignored Ignored;
        typedef enum Tag { A = 1, B = 2 } ExampleEnum;
        typedef struct Forward Forward;
        typedef struct { int value; } ExampleStruct;
        struct ExampleArrow { void (*release)(struct ExampleArrow* self); };
        typedef struct Defined Defined;
        typedef struct Defined { int x; } Defined;
    "#;
    let inventory = scan_header(header);

    assert_eq!(
        inventory.enums.get("ExampleEnum"),
        Some(&vec!["A".to_owned(), "B".to_owned()])
    );
    assert_eq!(
        inventory.structs.get("ExampleStruct"),
        Some(&vec!["value".to_owned()])
    );
    assert_eq!(
        inventory.structs.get("Defined"),
        Some(&vec!["x".to_owned()])
    );
    assert_eq!(
        inventory.plain_structs.get("ExampleArrow"),
        Some(&vec!["release".to_owned()])
    );
    assert!(inventory.opaque.contains("Forward"));
    assert!(inventory.opaque.contains("Defined"));
}
