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
| [x] | Scalar index creation | BTree, Bitmap, Inverted, Label-List indexes |
| [x] | Index management | List and drop index operations |
| [x] | Distributed index build | Train shared IVF/PQ models, build uncommitted fragment-scoped segments, and exchange protobuf metadata |
| [x] | C++ wrappers | `create_vector_index()` and `create_scalar_index()` methods |

### Phase 3: Write Path & Mutations

| Status | Component | Description |
|--------|-----------|-------------|
| [x] | Dataset write | Create / append / overwrite from ArrowArrayStream via `lance_dataset_write()`; tunable variant `lance_dataset_write_with_params()` for file/row-group sizing, Lance format version, and stable row IDs |
| [x] | Fragment writer | Batch-at-a-time fragment file writing (no commit) via `lance_write_fragments()` |
| [x] | Delete operations | Predicate-based deletion via `lance_dataset_delete()` |
| [x] | Update operations | Expression-based row updates via `lance_dataset_update()` |
| [x] | Merge-insert | Upsert via configurable when-matched / when-not-matched policies with `lance_dataset_merge_insert()` |
| [x] | Schema evolution | Add columns via SQL / all-null / `ArrowArrayStream` with `lance_dataset_add_columns_*()`, drop via `lance_dataset_drop_columns()`, rename / retype / set nullability via `lance_dataset_alter_columns()` |
| [x] | Version management | List via `lance_dataset_versions()`, rollback via `lance_dataset_restore()`, checkout via `lance_dataset_open(uri, opts, version)` |

### Phase 4: Advanced Features

| Status | Component | Description |
|--------|-----------|-------------|
| [x] | Fragment-level access | Fragment enumeration, ID listing, scanner fragment filtering |
| [x] | Compaction | Fragment consolidation via `lance_dataset_compact_files()` |
| [x] | Statistics export | Per-field compressed on-disk size for query planning via `lance_dataset_calculate_data_stats()` |
| [x] | Cloud storage | S3, GCS, Azure via storage options pass-through |
| [x] | Package distribution | vcpkg and Conan recipe packaging |

### Additional (not in RFC)

| Status | Component | Description |
|--------|-----------|-------------|
| [x] | Async scan | Callback-based `lance_scanner_scan_async()` for non-blocking scans |
| [x] | Dataset metadata | `lance_dataset_version()`, `lance_dataset_count_rows()`, `lance_dataset_latest_version()` |
| [x] | Filter pushdown | `lance_scanner_set_substrait_filter()` accepts a serialized Substrait `ExtendedExpression`; `lance_scanner_additional_sql_filter()` adds SQL predicates with AND before scanning starts |

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
The result includes the zero-based `query_index` (`int32`) and `_distance`,
alongside the requested dataset columns. A batch of one query still includes
`query_index`. Duplicate query vectors retain distinct indices; queries with no
matching rows produce no result rows. Result row order is not part of the API contract.

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

### Conan

```bash
conan install --requires=lance-c/0.1.0
```

Default path downloads prebuilts; `-o lance-c/*:from_source=True` builds
from source via cargo.

### Header path

```c
#include <lance/lance.h>     // C
#include <lance/lance.hpp>   // C++
```

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

## Releasing

Releases are tag-driven: pushing a `v*.*.*` tag fires [`release.yml`](.github/workflows/release.yml), which builds prebuilt tarballs for `linux-{x86_64,aarch64}` and `macos-aarch64` and attaches them to a GitHub Release. Beta tags (`v*-beta.*`) are published as pre-releases. (`macos-x86_64` is temporarily disabled — see the matrix comment in `release.yml`.)

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
4. `release.yml` builds artifacts. ~20 minutes later the [GitHub Release](https://github.com/lance-format/lance-c/releases) has all four `.tar.xz` artifacts plus a `SHA512SUMS` file.
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
5. `release.yml` fires on the tag push and builds the four prebuilt tarballs as above.

A `workflow_dispatch` trigger on `release.yml` lets you do dry-run builds without cutting a tag — Actions tab → "Release" → "Run workflow" → enter a version like `0.0.1-dev`. The `publish` job is skipped (gated on `refs/tags/v`), but the build matrix runs end-to-end so you can validate it before the real tag.

## License

Apache-2.0
