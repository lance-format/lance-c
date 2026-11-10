# lance-c

The C/C++ binding to [Lance](https://github.com/lancedb/lance), providing native access to the Lance columnar format via a stable C ABI and header-only C++ RAII wrappers.

- **C header:** [`include/lance/lance.h`](include/lance/lance.h)
- **C++ wrappers:** [`include/lance/lance.hpp`](include/lance/lance.hpp) (header-only, RAII, exceptions)
- **Data exchange:** [Arrow C Data Interface](https://arrow.apache.org/docs/format/CDataInterface.html) for zero-copy interop

## Roadmap

Based on the [liblance RFC](https://github.com/lance-format/lance/discussions/6035).

### Phase 1: Core Read Path + C++ Wrappers (MVP)

| Status | Component | Description |
|--------|-----------|-------------|
| [x] | Infrastructure | `lance-c` crate with Cargo.toml, Tokio runtime initialization |
| [x] | Error handling | Thread-local error codes/messages for cross-FFI safety |
| [x] | C header | `lance.h` with Arrow C Data Interface structs |
| [x] | Dataset operations | Open/close with URI + storage options + version support |
| [x] | Schema export | Arrow C Data Interface for zero-copy schema exchange |
| [x] | Scanner builder | Column projection, SQL filters, limit/offset, batch size, row ID, fragment filtering |
| [x] | ArrowArrayStream export | `lance_scanner_to_arrow_stream()` blocking API |
| [x] | Batch iteration | `lance_scanner_next()` blocking function |
| [x] | Poll + waker iteration | `lance_scanner_poll_next()` for async engines (Velox, Presto) |
| [x] | Random access | Index-based row retrieval via `lance_dataset_take()` |
| [x] | C++ wrappers | Header-only RAII library (`lance::Dataset`, `lance::Scanner`, `lance::Batch`) |
| [x] | Builder pattern | Fluent Scanner API (`.limit().offset().batch_size().with_row_id()`) |

### Phase 2: Vector Search & Indexing

| Status | Component | Description |
|--------|-----------|-------------|
| [x] | Vector search | Nearest-neighbor via scanner with metric/k/nprobes |
| [x] | Full-text search | FTS queries through scanner interface |
| [x] | Vector index creation | IVF_PQ, IVF_FLAT, IVF_SQ, HNSW variants |
| [x] | Scalar index creation | BTree, Bitmap, Inverted, Label-List, NGram, ZoneMap, BloomFilter indexes |
| [x] | Index management | List and drop index operations |
| [x] | Distributed index build | Train shared IVF/PQ models, build uncommitted fragment-scoped segments, and exchange protobuf metadata |
| [x] | C++ wrappers | `create_vector_index()` and `create_scalar_index()` methods |

### Phase 3: Write Path & Mutations

| Status | Component | Description |
|--------|-----------|-------------|
| [x] | Dataset write | Create / append / overwrite from ArrowArrayStream via `lance_dataset_write()`; tunable variant `lance_dataset_write_with_params()` for file/row-group sizing, Lance format version, and stable row IDs |
| [x] | Native transactions | Uncommitted writer transaction handles, inspection/protobuf handoff, single and append-batch publication, and dataset history reads. Additional operation constructors remain issue #112 follow-up scope |
| [x] | Fragment writer | Batch-at-a-time fragment file writing (no commit) via `lance_write_fragments()` |
| [x] | Delete operations | Predicate-based deletion via `lance_dataset_delete()` |
| [x] | Update operations | Expression-based row updates via `lance_dataset_update()` |
| [x] | Merge-insert | Upsert via configurable when-matched / when-not-matched policies with `lance_dataset_merge_insert()` |
| [x] | Schema evolution | Add columns via SQL / all-null / `ArrowArrayStream` with `lance_dataset_add_columns_*()`, drop via `lance_dataset_drop_columns()`, rename / retype / set nullability via `lance_dataset_alter_columns()` |
| [x] | Version management | Version details/metadata, lightweight refs, native ref checkout, tag and branch lifecycle, rollback, and numeric-version open |

### Phase 4: Advanced Features

| Status | Component | Description |
|--------|-----------|-------------|
| [x] | Fragment-level access | Fragment enumeration, ID listing, read-only metadata/schema/counts, scanner fragment filtering |
| [x] | Compaction | Fragment consolidation via `lance_dataset_compact_files()` |
| [x] | Dataset maintenance | Native manual cleanup/explain, dataset validation, deleted-row and small-file counts |
| [x] | Statistics export | Per-field compressed on-disk size for query planning via `lance_dataset_calculate_data_stats()` |
| [x] | Cloud storage | S3, GCS, Azure via storage options pass-through |
| [x] | Package distribution | vcpkg and Conan recipe packaging |

### Additional (not in RFC)

| Status | Component | Description |
|--------|-----------|-------------|
| [x] | Async scan | Callback-based `lance_scanner_scan_async()` for non-blocking scans |
| [x] | Dataset metadata | Table/schema/field metadata updates plus native storage-version, stable-row-ID and field-ID inspection |
| [x] | Filter pushdown | `lance_scanner_set_substrait_filter()` accepts a serialized Substrait `ExtendedExpression`; `lance_scanner_additional_sql_filter()` adds SQL predicates with AND before scanning starts |
| [x] | Substrait aggregation | `lance_scanner_set_substrait_aggregate()` accepts a serialized Substrait `Plan`; group/measure semantics, aliases, validation, and aggregate output come from the native planner |
| [x] | Row prefilter | `lance_scanner_set_row_addr_allowlist()` / `lance_scanner_set_row_addr_blocklist()` restrict scans, nearest-neighbor and full-text searches to (or exclude) a set of `_rowid`s before ranking |
| [x] | Native ordering | `lance_scanner_set_order_by()` and C++ `Scanner::order_by()` sort by one or more columns with direction and NULL placement |
| [x] | Expression projection | `lance_scanner_set_projection()` binds named SQL expressions, aliases and nested fields; `lance_scanner_disable_scoring_autoprojection()` controls automatic `_score` / `_distance` output |

## Dataset metadata

Table metadata is available through `lance_dataset_metadata_json()` or
`Dataset::metadata_json()`. Updates return the complete post-commit map:

```c
const char* keys[] = {"owner", "retired", "empty"};
const char* values[] = {"c-consumer", NULL, ""};
const char* updated = lance_dataset_update_metadata_json(
    dataset, keys, values, 3, LANCE_METADATA_UPDATE_MERGE);
if (!updated) {
    /* Check lance_last_error_code()/message. */
}
lance_free_string(updated);
```

```cpp
ds.update_metadata_json({
    {"owner", std::string("c++")},
    {"retired", std::nullopt},
    {"empty", std::string("")},
}, lance::MetadataUpdateMode::Merge);

ds.update_schema_metadata_json({
    {"schema-owner", std::string("c++")},
});
```

A NULL value removes the key. An empty string remains a real empty value.
`LANCE_METADATA_UPDATE_REPLACE` / `MetadataUpdateMode::Replace` replaces the
whole map; an empty update in replace mode clears it. Updates mutate the same
dataset handle under Lance's native snapshot and commit semantics: the handle
sees the new version, while existing dataset snapshots and scanners keep their
prior view. Table and schema maps are independent.

The exported Arrow schema is still the schema metadata read path. Use
`lance_dataset_schema()` or `Dataset::schema()` to inspect the committed schema
metadata; no duplicate C-side metadata cache is maintained.

Field metadata uses one native batch and one commit. Each entry selects a field
by ID or path and carries its own merge or replace mode:

```cpp
auto id = ds.resolve_field_id(lance::FieldReference::by_path("id"));
ds.update_field_metadata({
    {lance::FieldReference::by_id(id),
     {{"owner", std::string("c++")}, {"empty", std::string("")}},
     lance::MetadataUpdateMode::Merge},
    {lance::FieldReference::by_path("profile.name"),
     {{"nested", std::string("value")}},
     lance::MetadataUpdateMode::Replace},
});
```

A NULL value removes a key, an empty string remains a real value, and duplicate
references to the same resolved field follow the native builder's
last-update-wins behavior. Read field metadata through the Arrow schema's field
metadata. `lance_dataset_data_storage_version()`,
`lance_dataset_has_stable_row_ids()`, `lance_dataset_field_id_count()` /
`lance_dataset_field_id_at()`, `lance_dataset_resolve_field_id()` and
`lance_dataset_field_path()` expose the current native snapshot; the C++
wrappers provide the corresponding `Dataset` methods.

## Dataset maintenance

Manual version cleanup and its read-only explanation bind the native
`CleanupPolicy`, `CleanupOperation`, `Dataset::cleanup_old_versions`,
`Dataset::validate`, `Dataset::count_deleted_rows` and
`Dataset::num_small_files` operations:

```cpp
lance::CleanupPolicy policy;
policy.versions = std::vector<uint64_t>{2, 3};
policy.before_version = 5;

lance::CleanupOperationOptions operation;
operation.max_candidate_files = 100;

auto preview = ds.explain_cleanup(policy, operation);
auto removed = ds.cleanup(policy, operation);

ds.validate();
uint64_t deleted_rows = ds.count_deleted_rows();
uint64_t small_files = ds.num_small_files(/*max_rows_per_group=*/1000);
```

Policy selectors combine with AND. A default-constructed policy inherits native
defaults rather than the legacy writer's automatic 20-version / 14-day policy.
`std::nullopt` versus an engaged empty vector distinguishes absent from explicit
empty version selection, and `retain_versions` uses Lance's asynchronous native
builder. Use `CleanupOldVersionsOptions` /
`lance_dataset_cleanup_old_versions()` for relative-duration cleanup; Lance core
owns the clock cutoff.

Explanations copy all aggregate stats, candidate files, truncation state,
referenced branches and warnings, so they remain valid after the dataset handle
closes. They do not reserve files. Execution re-evaluates current references
and storage state, protects current/tagged/branch/unverified data according to
Lance, and reports partial deletion through `failed_deletes`; it does not roll
back successful deletes. Historical snapshot handles do not own a storage lease.

## Scanner ordering

Use `lance_scanner_set_order_by()` or the fluent C++ `Scanner::order_by()` to
apply Lance's native `Scanner::order_by` behavior:

```cpp
auto scanner = dataset.scan();
scanner.order_by({
    {"category", LANCE_ORDER_ASCENDING, LANCE_ORDER_NULLS_LAST},
    {"score", LANCE_ORDER_DESCENDING, LANCE_ORDER_NULLS_FIRST},
    {"id", LANCE_ORDER_ASCENDING, LANCE_ORDER_NULLS_LAST},
});
```

Tie ordering must be explicit when rows can compare equal; the binding does not
add a row-id or stable-order tie-breaker. Lance validates referenced columns
when the scan starts. With limit/offset, Lance sorts all rows before applying
the window. With nearest or full-text search, Lance sorts the rows selected by
the query.

Pass `std::nullopt` or `lance_scanner_set_order_by(scanner, NULL, 0)` to clear
the ordering. An explicit empty vector is the native empty ordering and has the
same scan effect. Invalid enum values or a NULL column name are rejected without
replacing the previous ordering. Multi-vector search is the one unsupported
mode: `nearest_multivector()` always restores distance order for its result
window, so combining it with a non-empty ordering fails when the scan starts
instead of silently ignoring the ordering.

## Scanner projection and scoring columns

`lance_scanner_set_projection()` and the C++ `Scanner::project()` bind Lance's
native `Scanner::project_with_transform()`:

```cpp
auto scanner = dataset.scan();
scanner.project({
    {"id_alias", "id"},
    {"doubled", "id * 2"},
    {"x", "metadata.location.x"},
});
```

Entries preserve input order. `output_name` is the output field name and may be
an alias; `expression` is a Lance SQL expression. The binder accepts stored
columns, metadata columns, nested field paths and search-generated scoring
columns. Pass `std::nullopt` or `lance_scanner_set_projection(scanner, NULL, 0)`
to restore the unset projection and scan all dataset columns. An empty vector is
an explicit empty projection. The arrays and strings are copied before the
setter returns; invalid entries leave the previous projection unchanged. The
older column-list argument to `lance_scanner_new()` keeps its escaped
field-path behavior and is unchanged.

By default, vector and full-text searches still append `_distance` or `_score`
to an explicit projection, matching the published legacy behavior. Call
`Scanner::disable_scoring_autoprojection()` or
`lance_scanner_disable_scoring_autoprojection()` before starting the scan to
include those columns only when the projection names them explicitly. The
setting is one-way for that scanner.

Multi-vector search keeps the hidden distance required for its final result
window internally. It is removed from output when scoring autoprojection is
disabled and the caller did not request `_distance`.

## Segment-scoped array label filters

Ordinary scans can use a `LabelList` index for array membership through the
existing SQL and Substrait filter interfaces. For example, on a `List<Utf8>`
column named `labels`:

```sql
array_contains(labels, 'red')
array_contains(labels, 'red') AND array_contains(labels, 'blue')
array_contains(labels, 'red') OR array_contains(labels, 'blue')
array_has_all(labels, ['red', 'blue'])
array_has_any(labels, ['red', 'blue'])
```

Configure `lance_scanner_set_fragment_ids` and
`lance_scanner_set_scalar_index_segment` with the selected LabelList segment.
Pass a SQL filter to `lance_scanner_new`, or attach a serialized Substrait
`ExtendedExpression` with `lance_scanner_set_substrait_filter`. The Substrait
schema must describe the list field and its element type; replacing that field
with an unsupported-type placeholder cannot express a label predicate. Use
Lance/DataFusion's `array_has` (the canonical name of `array_contains`),
`array_has_all`, or `array_has_any` functions with correctly typed arguments.
Substrait takes precedence over the primary SQL filter; use
`lance_scanner_additional_sql_filter` when an additional condition must be ANDed
with it.

AND/OR membership expressions can reuse the same selected LabelList segment.
Other columns remain residual filters, evaluated before LIMIT/OFFSET. This API
selects one physical segment; it does not intersect indices on different columns.
Incomplete segment coverage falls back to scanning the entire explicit fragment
scope. Check `scalar_segments_searched` and `scalar_segment_fallbacks` in the
statistics callback to distinguish index acceleration from filter execution.

Bindings preserve Lance's function semantics, not the semantics of similarly
named functions in another SQL engine. In particular, a NULL search value in
`array_contains` does not match NULL array elements. `array_has_all` tests set
containment, not an ordered contiguous subsequence. The pinned Lance version
also treats an empty all-label query as true even for NULL lists. Integrators
should initially push only non-NULL constant labels with matching element types
and retain conditions whose semantics have not been verified in the calling engine.

## Runtime BTree segment selection

Ordinary segment-scoped scans can opt in to comparing BTree page costs at scan
preparation time. Configure `lance_scanner_set_scalar_index_auto_selection(scanner,
true)`, or call `Scanner::scalar_index_auto_selection(true)` in C++, together with
`scalar_index_segment` and an explicit, nonempty fragment list. The selected
segment remains the default when estimation is unavailable. The option is disabled
by default and does not affect vector or full-text searches.

The estimator reads `page_lookup.lance` min/max and NULL-count metadata for equality,
range, bounded IN, IS NULL, and supported AND/OR expressions. Estimated work is
matching pages times the index's configured rows per page. It can select a cheaper
BTree segment only when its known coverage contains the entire read domain. A best
estimate at least as large as the domain's physical row count selects a scoped
non-indexed scan. The full filter, deletion handling, explicit fragment scope and
LIMIT/OFFSET remain in effect on every path; estimates never remove result rows.

Each preparation considers at most eight physical segments, an aggregate 8 MiB
of lookup files, and a separate 8 MiB allowance for decoded lookup buffers. The
decoded allowance is checked from schema and row count before materialization and
charged for both cached and newly loaded lookups. Only fixed-width scalar lookup
columns have a known bound with the current reader API. Variable-width keys such
as strings/binary therefore have unknown costs: a default on such a key is kept,
and an alternative is skipped without opening its lookup for estimation. Such
indices remain usable through the caller's existing segment-selection path.

One lookup is limited to 65,536 pages, IN predicates to 256 values, and one estimate
to 1,000,000 page/predicate checks. Lookup batches share the Session's bounded
metadata cache. Unknown manifest file sizes, unsupported formats, probe errors,
or an unestimable default retain the original segment path. Bitmap and LabelList
segments retain their existing behavior and are not cost-ranked.

Statistics callbacks report `scalar_index_auto_candidates`,
`scalar_index_auto_changed`, `scalar_index_auto_estimated_pages`,
`scalar_index_auto_estimated_rows`, and `scalar_index_auto_full_scans`. Estimated
rows are a page-level approximation, not an exact result count or measured latency.
The model does not account for cache residency, projected-row width, or random
row-fetch cost. Warm lookups avoid storage I/O but still require estimation work.

## Distance-bounded vector search

After configuring a single-vector nearest-neighbor query, use
`lance_scanner_set_distance_range` or C++ `Scanner::distance_range` to restrict
results to `lower_bound <= _distance < upper_bound`:

```cpp
const float query[] = {1.0f, 0.0f};
auto scanner = dataset.scan();
scanner.nearest("embedding", query, 2, 100)
       .metric(LANCE_METRIC_L2)
       .distance_range(std::nullopt, 0.5f);
```

This returns **at most 100** neighbors with distance below `0.5`. It is a
range-constrained Top-K search, not an unbounded enumeration of all matches.
For an annulus, use `.distance_range(0.2f, 0.5f)`; for a lower bound only, use
`.distance_range(0.2f)`; `.distance_range()` clears both bounds. In C, pass
pointers to bounds and `NULL` for an unbounded side:

```c
float upper_bound = 0.5f;
int32_t status = lance_scanner_set_distance_range(scanner, NULL, &upper_bound);
/* Check status and lance_last_error_* before starting the scan. */
```

Bounds are copied and must be finite. When both are set, the lower bound must
be strictly smaller than the upper bound. Negative bounds are allowed (for
example, Dot distances can be negative). Distances use the selected metric's
units: L2 reports **squared Euclidean distance**, so a geometric radius `r`
corresponds to an upper bound of `r * r`, with the boundary excluded. Index-based
search retains Lance's approximate candidate selection; the range does not
guarantee exhaustive recall. Use `.use_index(false)` for an exact scan, still
subject to `k`.

Set bounds after `nearest` and before starting the scan. Replacing the nearest
query clears the bounds; an invalid range leaves the previous range unchanged.
Multi-vector queries are not supported by this setter because their scores
aggregate distances across subvectors.

## Multi-vector search

Use `lance_scanner_nearest_multivector` or the C++ `Scanner::nearest_multivector`
method for a `List<FixedSizeList<float16|float32|float64, D>>` column:

```cpp
const float query[] = {1.0f, 0.0f, 0.0f, 1.0f};
auto scanner = dataset.scan();
scanner.nearest_multivector("embeddings", query, 2, 2, LANCE_DTYPE_FLOAT32, 10)
       .metric(LANCE_METRIC_COSINE)
       .prefilter(true);
```

The copied, row-major matrix is **one query** containing two subvectors. Results
rank logical rows by the sum of each query subvector's minimum distance to a
stored subvector. Empty or null outer rows do not rank. Inner vectors must be
non-nullable; actual stored null or non-finite elements encountered during
scoring fail the stream. Float types and dimensions must match the column.
Cosine pairs with zero norm have undefined distance and are ignored. A row is
excluded if any query subvector has no defined match; a zero-norm query subvector
therefore produces no results. Column names use Lance field-path syntax,
including nested paths such as `payload.embeddings` and backtick-quoted names.

L2 is the default on every fragment. Cosine multi-vector indexes are supported
by the pinned Lance version; incompatible metrics use exact search. Indexed
candidates are refined against stored values (`refine_factor` defaults to 1).
ANN candidate selection remains approximate. Limit and offset apply after
restoring distance order, including fragment-scoped searches. Strict row batching
is applied after that final result window, preserving full batches except the last.

Queries accept at most 128 subvectors. Both `num_vectors * k` and
`refine_factor * k` must be at most 100,000 to bound plan expansion and candidate
allocation. The existing single-vector API and its defaults are unchanged.

## Batch nearest-neighbor search

Use `lance_scanner_nearest_batch` or `Scanner::nearest_batch` to search a
`FixedSizeList<T, dimension>` column with independent query vectors:

```cpp
const float queries[] = {1.0f, 0.0f, 0.0f, 1.0f};
auto scanner = dataset.scan();
scanner.nearest_batch("embedding", queries, 2, 2, 10)
       .use_index(false);
```

Here there are two queries of dimension two, each returning up to ten rows.
The result includes the zero-based `query_index` (`int32`) and, under the legacy
scoring default, `_distance`, alongside the requested dataset columns. A batch
of one query still includes `query_index`. After
`disable_scoring_autoprojection()`, `_distance` appears only when named
explicitly in the projection. Duplicate query vectors retain distinct indices;
queries with no matching rows produce no result rows. Result row order is not
part of the API contract.

This differs from `nearest_multivector`: that API scores one logical query
against a `List<FixedSizeList<...>>` column and returns one ranked result set.
Batch input requires a fixed-size vector column and rejects a dataset with a
reserved `query_index` column. Float16, Float32, Float64, Int8, and UInt8 are accepted;
the query element type must match the column. UInt8 uses the Hamming metric.
Floating-point query values must be finite. Values are copied before the setter
returns, so the caller can immediately release its input buffer.

Configure the query before scanning. Successful nearest-query setters replace
the previous query; an invalid batch request leaves it unchanged. FTS and
nearest queries remain mutually exclusive. Distance ranges currently require a
single-vector query; replacing it with a batch clears its bounds. All queries
share the scanner's metric, filter, index selection, and tuning parameters.
Existing prefilter and postfilter semantics apply; postfiltering can return
fewer than `k` rows.

The scanner's global `limit` and `offset` settings are rejected in batch mode,
in either configuration order: they cannot represent independent result windows.
For per-query pagination, request enough candidates with `k` and apply the window
separately to each `query_index` in the caller.

Bounds are 128 queries, 64 MiB of copied query values, and 100,000 candidates for
`num_queries * k * refine_factor` (use 1 when refinement is unset). Explicit
refinement must be positive. These bounds do not cap total index/payload memory;
callers should also configure scanner resource controls. Arrow streams use the
existing ownership and cancellation contract, including early stream release.

Execution delegates to the pinned Lance scanner. Flat search can share data
scans. Eligible IVF queries can share partition scans; HNSW, refinement, adaptive
partition probing, and other ineligible plans use Lance's per-query fallback.
The batch API does not guarantee shared execution or a particular speedup.

## Building

There are four supported entry points; pick whichever matches your toolchain.
Building this source tree requires Rust 1.94.0 or newer. The `from-source`
recipes fetch the v0.1.9 tag, whose `Cargo.toml` declares
`rust-version = "1.91.0"`. Prebuilt archives do not require a Rust toolchain.

### Published prebuilt archives

The v0.1.9 release publishes five archives. vcpkg and Conan currently select
three of them:

| Archive | Selected by |
| --- | --- |
| `lance-c-v0.1.9-x86_64-unknown-linux-gnu.tar.xz` | Not selected by the current recipes |
| `lance-c-v0.1.9-x86_64-unknown-linux-gnu-debian12.tar.xz` | vcpkg `x64-linux`, `x64-linux-dynamic`, Conan `Linux/x86_64` |
| `lance-c-v0.1.9-aarch64-unknown-linux-gnu.tar.xz` | vcpkg `arm64-linux`, `arm64-linux-dynamic`, Conan `Linux/armv8` |
| `lance-c-v0.1.9-aarch64-unknown-linux-gnu-debian12.tar.xz` | Not selected by the current recipes |
| `lance-c-v0.1.9-aarch64-apple-darwin.tar.xz` | vcpkg `arm64-osx`, `arm64-osx-dynamic`, Conan `Macos/armv8` |

Intel macOS (`x86_64-apple-darwin`) uses the `from-source` path for v0.1.9.
The archives contain headers, static and shared libraries,
CMake package files, pkg-config metadata, and the license. The package recipes
install only the linkage selected for that package.

Static inspection of the published Linux shared libraries found:

| Archive | Highest GLIBC symbol reference | Direct dynamic dependencies |
| --- | --- | --- |
| x86_64 standard | `GLIBC_2.39` | `libgcc_s.so.1`, `libm.so.6`, `libc.so.6`, `ld-linux-x86-64.so.2` |
| x86_64 Debian 12 | `GLIBC_2.35` | `libgcc_s.so.1`, `libm.so.6`, `libc.so.6`, `ld-linux-x86-64.so.2` |
| aarch64 standard | `GLIBC_2.39` | `libgcc_s.so.1`, `libm.so.6`, `libc.so.6` |
| aarch64 Debian 12 | `GLIBC_2.35` | `libgcc_s.so.1`, `libm.so.6`, `libc.so.6` |

None of the Linux libraries records GLIBCXX or CXXABI version requirements.
The macOS dylib is a thin `arm64` Mach-O binary with install name
`@rpath/liblance_c.dylib`, `minos 11.0`, SDK `14.5`, and dependencies on
`SystemConfiguration`, `libobjc.A.dylib`, `IOKit`, `CoreFoundation`,
`Security`, `libiconv.2.dylib`, and `libSystem.B.dylib`.

The highest GLIBC symbol reference records a dynamic-loader requirement.
The Mach-O `minos` field records the binary's declared deployment target.
Runtime compatibility requires consumer verification on the intended OS.
Static archive inspection established object architecture. The complete
binary metadata is recorded in the [artifact
audit](https://github.com/lance-format/lance-c/blob/2c00796/validation/issue-103/RESULTS.md).
Runtime consumer evidence covers Linux x86_64 and is recorded in the
[issue 100 validation
record](https://github.com/lance-format/lance-c/blob/5a29b67/validation/issue-100/RESULTS.md).

### From source via cargo (Rust developers)

```bash
cargo build --release
```

Produces `target/release/liblance_c.{so,dylib,dll}` and a `liblance_c.a`.
Headers stay in `include/lance/`.

### From source via CMake (C/C++ developers)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /your/prefix
```

Installs headers, both linkages, a `LanceCConfig.cmake` package config, and
a `lance-c.pc` pkg-config file. Consumers then:

```cmake
find_package(LanceC 0.1 REQUIRED)
target_link_libraries(myapp PRIVATE LanceC::lance_c)
```

See [`examples/cmake-consumer/`](examples/cmake-consumer/) for a minimal
working example.

### vcpkg

```bash
vcpkg install lance-c
```

Downloads a prebuilt binary for your triplet from
[GitHub Releases](https://github.com/lance-format/lance-c/releases). For
unsupported triples, opt into a source build with the `from-source` feature:

```bash
vcpkg install 'lance-c[from-source]'  # requires Rust toolchain
```

The prebuilt archives and vcpkg mappings are listed under [Published prebuilt
archives](#published-prebuilt-archives). Intel macOS has no prebuilt asset in
this release, so use `lance-c[from-source]` there. Source builds also require
CMake and `protoc` on the build host.

### Conan

```bash
conan install --requires=lance-c/0.1.9
```

Default path downloads prebuilts; `-o lance-c/*:from_source=True` builds
from source via cargo. As with vcpkg, Intel macOS must use the source path in
this release.

### Header path

```c
#include <lance/lance.h>     // C
#include <lance/lance.hpp>   // C++
```

The C++ wrappers require C++17. The C header is usable from C.

## Usage

### C

```c
#include <lance/lance.h>

LanceDataset* ds = lance_dataset_open("data.lance", NULL, 0);
if (!ds) {
    printf("Error: %s\n", lance_last_error_message());
    return 1;
}

struct ArrowArrayStream stream;
LanceScanner* scanner = lance_scanner_new(ds, NULL, NULL);
lance_scanner_to_arrow_stream(scanner, &stream);
// consume stream...

lance_scanner_close(scanner);
lance_dataset_close(ds);
```

### C++

```cpp
#include <lance/lance.hpp>

auto ds = lance::Dataset::open("data.lance");
printf("rows: %llu, version: %llu\n", ds.count_rows(), ds.version());

ArrowArrayStream stream;
ds.scan()
  .limit(100)
  .batch_size(1024)
  .to_arrow_stream(&stream);
// consume stream...
```

### Native transactions

`Dataset::write_uncommitted` maps to `InsertBuilder::execute_uncommitted_stream`
and returns an owned native transaction. The stream is consumed, but data files
remain uncommitted until the transaction is published:

```cpp
ArrowArrayStream stream = source_stream();
lance::UncommittedWriteOptions write_options;
write_options.write.params.max_rows_per_file = 1;
write_options.transaction_properties =
    std::vector<std::pair<std::string, std::string>>{{"engine", "cpp"}};

auto transaction = lance::Dataset::write_uncommitted(
    "data.lance", &stream, lance::WriteMode::Append, write_options);
auto operation = transaction.operation();
assert(operation.kind() == lance::OperationKind::Append);
assert(operation.fragment_count() > 0);

auto bytes = transaction.to_proto();
auto parsed = lance::Transaction::from_proto(bytes);
auto dataset = lance::Dataset::commit("data.lance", parsed);
auto committed = dataset.read_transaction();
auto version_one = dataset.read_transaction_by_version(1);
```

Append batches use native `CommitBuilder::execute_batch` and return both the
actual merged transaction and a new dataset:

```cpp
std::vector<std::reference_wrapper<const lance::Transaction>> batch{
    std::cref(first), std::cref(second)};
auto result = lance::Dataset::commit_batch("data.lance", batch);
```

The C entry points are `lance_dataset_write_uncommitted_stream`,
`lance_transaction_commit`, `lance_transaction_commit_batch`,
`lance_dataset_read_transaction`, and
`lance_dataset_read_transaction_by_version`. Native defaults remain omitted in
`LanceCommitOptions`; explicit zero retries, zero retry budget, disabled timeout
and explicit timeout values are forwarded without reinterpretation.

This first delivery exposes writer-generated Append and Overwrite operations.
Typed Delete/Update/Merge/Restore/Clone/Rewrite/index/schema operation
constructors and callback-based commit locks remain later issue #112 scope.
Transaction values are independent snapshots: closing their source dataset,
stream, or derived operation handle does not invalidate them.

Native protobuf conversion is the transport contract. `Operation::Unknown`
decodes but cannot be re-encoded or committed, empty tag/property maps normalize
to absent, and the pinned core does not preserve Overwrite schema metadata
through its separate protobuf field. The pinned core's CommitBuilder also does
not apply its separate `with_transaction_properties` field in `execute` or
`execute_batch`; this binding therefore carries properties on the uncommitted
transaction.

A failed commit performs no binding-side rollback. Data files may remain, and
an I/O failure can leave publication uncertain. Reopen the URI to establish the
actual version before retrying. Detached commits follow native behavior: they do
not appear in main history, and the pinned core rejects using one to create a
new dataset.

### Share metadata and index caches

Use a session to share Lance's metadata and index caches across dataset
handles. Cache limits are bytes; zero requests zero capacity for the
corresponding cache.

```c
LanceSession* session = lance_session_new(
    6ULL * 1024 * 1024 * 1024,
    1ULL * 1024 * 1024 * 1024);
LanceDataset* ds =
    lance_dataset_open_with_session("data.lance", NULL, 0, session);

LanceSessionCacheStats stats;
lance_session_get_cache_stats(session, &stats);

lance_session_close(session);  /* ds remains valid */
lance_dataset_close(ds);
```

```cpp
lance::Session session(
    6ULL * 1024 * 1024 * 1024,
    1ULL * 1024 * 1024 * 1024);
auto ds = lance::Dataset::open_with_session(session, "data.lance");
auto stats = session.cache_stats();
```

Each tier can independently use the native default, an explicit capacity, a
backend URI, or a backend kind plus options. `Default` and an explicit zero
capacity are different choices.

```c
LanceSessionCacheSpec index = {0};
index.kind = LANCE_SESSION_CACHE_SPEC_URI;
index.backend_uri = "moka://?capacity=67108864";

LanceSessionCacheSpec metadata = {0};
metadata.kind = LANCE_SESSION_CACHE_SPEC_SIZE;
metadata.size_bytes = 0; /* explicit zero capacity */

LanceSession* session = lance_session_new_with_cache_specs(&index, &metadata);
uint64_t size_bytes = 0;
uint64_t approx_items = 0;
lance_session_get_size_bytes(session, &size_bytes);
lance_session_get_approx_num_items(session, &approx_items);
```

```cpp
lance::Session session(
    lance::SessionCacheSpec::from_uri("moka://?capacity=67108864"),
    lance::SessionCacheSpec::with_size(0));
auto size_bytes = session.size_bytes();
auto approx_items = session.approx_num_items();
```

`size_bytes()` is the native deep size of live session state, not an RSS
measurement or a configured limit. `approx_num_items()` is explicitly
approximate and may change as cache maintenance runs.

### Prewarm an index synchronously

Prewarm a logical index before serving queries to move cold index reads out of
foreground query execution. The call blocks until the Rust SDK's
`Dataset::prewarm_index` finishes for all segments with that name in the dataset
handle's snapshot. It does not create an index or change the dataset version.

```c
LanceSession* session = lance_session_new(64 * 1024 * 1024, 16 * 1024 * 1024);
if (!session) return -1;
LanceDataset* ds = lance_dataset_open_with_session("data.lance", NULL, 0, session);
if (!ds) {
    lance_session_close(session);
    return -1;
}
/* Assumes an index named embedding_idx already exists. */
int32_t status = lance_dataset_prewarm_index(ds, "embedding_idx");
if (status != 0) {
    const char* message = lance_last_error_message();
    fprintf(stderr, "%s\n", message);
    lance_free_string(message);
}
lance_dataset_close(ds);
/* Keep session alive and reuse it when opening subsequent query datasets. */
lance_session_close(session);
```

```cpp
lance::Session session(64 * 1024 * 1024, 16 * 1024 * 1024);
auto ds = lance::Dataset::open_with_session(session, "data.lance");
ds.prewarm_index("embedding_idx"); // Blocks; throws lance::Error on failure.
```

The destination is the dataset's process-local session index cache. Reusing the
same session across handles preserves warmed entries after a dataset closes.
Independent sessions and other processes do not share that cache. Entries remain
evictable: success guarantees completion, not that the entire index fits in the
cache or remains resident. Replacing an index creates new segments that must be
warmed separately; historical handles still use their own snapshots. A failure
can leave partially warmed entries in the cache; retrying is safe.

This binding uses the SDK's index-specific prewarm behavior without additional
options. Tests cover multi-segment IVF-Flat and B-tree indexes; other index types
and formats follow the underlying SDK's support and error behavior. It does not
prewarm ordinary data columns or eliminate result-row reads, and it adds no
persistent cache, automatic refresh, or background task management.

### Open at a specific version

`lance_dataset_open` takes a `version` argument — `0` means the latest, any
other value checks out that specific version id (e.g. one returned by
`lance_dataset_versions`):

```c
LanceDataset* ds = lance_dataset_open("data.lance", NULL, 42);
```

```cpp
auto ds = lance::Dataset::open("data.lance", {}, /*version=*/42);
```

Native refs can also select a version, the latest version on a branch, or a tag:

```c
LanceRef* ref = lance_ref_tag("release-2026-11");
LanceDataset* ds = lance_dataset_checkout_ref(source, ref);
lance_ref_close(ref);
```

```cpp
auto ds = source.checkout(lance::Ref::tag("release-2026-11"));
auto tags = ds.tags();
```

`lance_dataset_current_version()`, `lance_dataset_version_refs()` and
`lance_dataset_tags()` return copied snapshots. They remain valid after the
originating dataset handle is closed. Timestamp accessors expose native
nanosecond precision; the existing millisecond version accessor is unchanged.
Nanosecond accessors report an error when a timestamp does not fit i64
nanoseconds. Snapshot creation and millisecond reads retain the native range.
There is no C-side timestamp/asof selector.

Branches use the public Lance 14.0.2 branch lifecycle:

```cpp
auto branch = source.create_branch(
    "feature/next", lance::Ref::version(
        std::optional<std::string>("main"), std::optional<uint64_t>(2)));
auto checked_out = source.checkout_branch("feature/next");
branch.replace_branch_metadata("feature/next", {{"owner", "team"}});
auto branches = source.branches_json();
auto contents = source.get_branch_json("feature/next");
auto identifier = branch.branch_identifier_json();
```

`lance_dataset_branch_create()` accepts the source dataset's store params when
its storage-options pointer is `NULL`; a non-`NULL`, empty array requests
explicit empty object-store params. Native creation is a non-atomic shallow
clone followed by a `BranchContents` write. If creation leaves a branch
dataset without `BranchContents`, use `lance_dataset_branch_force_delete()` to
remove it. Normal deletion still rejects branches referenced by a child branch
or a tag. Reusing an existing branch name may fail in the native shallow-clone
phase rather than reaching the `BranchContents` conflict check.

Branch JSON is the selected Lance core's serde representation, not a new C
metadata model. The list is an object keyed by branch name; each
`BranchContents` includes `parentBranch`, `identifier.version_mapping`,
`parentVersion`, `createAt` in Unix seconds, `manifestSize`, and `metadata`.
Map enumeration order is unspecified. Branch metadata replacement writes the
whole native map, so an empty map clears all entries.

## Releasing

Pushing a `v*.*.*` tag starts [`release.yml`](.github/workflows/release.yml), which builds five prebuilt tarballs for Linux x86_64 and aarch64, their Debian 12 variants, and macOS arm64. The workflow attaches them to a GitHub Release. Beta tags (`v*-beta.*`) are published as pre-releases. Intel macOS consumers use the source build described above.

### Recommended: cut a release via Actions UI

[`create-release.yml`](.github/workflows/create-release.yml) is a `workflow_dispatch` entry point that bumps `Cargo.toml`, commits, tags, and pushes — replacing the manual edit/commit/tag steps below.

1. Open Actions → **Create Release** → **Run workflow** on `main`.
2. Choose:
   - **release_type**: `patch` / `minor` / `major` (or `current` to cut another beta on the same base, e.g. `v0.2.0-beta.2` after `v0.2.0-beta.1`).
   - **release_channel**: `preview` (tags `vX.Y.Z-beta.N`, auto-incremented) or `stable` (tags `vX.Y.Z`).
   - **dry_run**: leave on for the first run to preview the computed tag/version without pushing anything.
3. Re-run with **dry_run** off. The workflow:
   - Bumps `version = ...` in `Cargo.toml` and refreshes `Cargo.lock` via `cargo set-version` (skipped for `current` if the version is already correct).
   - Commits as `github-actions[bot]` with message `chore: bump version to <version>`.
   - Pushes the commit to `main` and pushes the tag.
   - Dispatches `release.yml` at the new tag's ref. (Direct tag push by `GITHUB_TOKEN` does not trigger workflows — GitHub's recursion guard — so we explicitly `gh workflow run` instead.)
4. `release.yml` builds artifacts. ~20 minutes later the [GitHub Release](https://github.com/lance-format/lance-c/releases) has all published `.tar.xz` artifacts (Linux x86_64/aarch64, their Debian 12 variants, and macOS arm64) plus a `SHA512SUMS` file.
5. The `publish` job's log emits a paste-ready `set(LANCE_C_SHA512_... "...")` snippet. Copy it into:
   - [`ports/lance-c/portfile.cmake`](ports/lance-c/portfile.cmake) (SHA512s)
   - [`recipes/lance-c/all/conandata.yml`](recipes/lance-c/all/conandata.yml) (SHA256s, derived from the `.sha256` files in the release assets)
6. Open follow-up PRs to `microsoft/vcpkg` and `conan-io/conan-center-index` mirroring the updated `ports/` and `recipes/` directories.

> **Branch protection:** if `main` is a protected branch, allow `github-actions[bot]` (or the GitHub Actions integration) to bypass push restrictions, or replace the default `GITHUB_TOKEN` in `create-release.yml` with a PAT that has `contents: write`.

### Manual fallback

If you need to cut a release without the workflow (e.g. local tag with extra commits):

1. Decide the new version (semver). Pre-1.0 (`0.x.y`): bump **minor** for breaking changes or new features, **patch** for bug fixes only.
2. On `main`, bump `version = ...` in [`Cargo.toml`](Cargo.toml) and refresh `Cargo.lock`:
   ```bash
   git checkout main && git pull
   # edit Cargo.toml: change version = "0.1.0" to "0.2.0"
   cargo update -p lance-c
   git checkout -b chore/release-0.2.0
   git commit -am "chore(release): v0.2.0"
   ```
3. Open a PR with the bump (and any `CHANGELOG.md` edits if you maintain one), get it reviewed and merged.
4. Tag the merge commit and push:
   ```bash
   git checkout main && git pull
   git tag v0.2.0
   git push origin v0.2.0
   ```
5. `release.yml` fires on the tag push and builds the five prebuilt tarballs as above.

A `workflow_dispatch` trigger on `release.yml` lets you do dry-run builds without cutting a tag — Actions tab → "Release" → "Run workflow" → enter a version like `0.0.1-dev`. The `publish` job is skipped (gated on `refs/tags/v`), but the build matrix runs end-to-end so you can validate it before the real tag.

## License

Apache-2.0
