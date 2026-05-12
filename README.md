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
| [x] | C++ wrappers | `create_vector_index()` and `create_scalar_index()` methods |

### Phase 3: Write Path & Mutations

| Status | Component | Description |
|--------|-----------|-------------|
| [x] | Dataset write | Create / append / overwrite from ArrowArrayStream via `lance_dataset_write()`; tunable variant `lance_dataset_write_with_params()` for file/row-group sizing, Lance format version, and stable row IDs |
| [x] | Fragment writer | Batch-at-a-time fragment file writing (no commit) via `lance_write_fragments()` |
| [ ] | Delete operations | Predicate-based deletion |
| [ ] | Update operations | Expression-based row updates |
| [ ] | Merge-insert | Upsert functionality with builder pattern |
| [ ] | Schema evolution | Add/drop/alter columns with expressions |
| [x] | Version management | List via `lance_dataset_versions()`, rollback via `lance_dataset_restore()`, checkout via `lance_dataset_open(uri, opts, version)` |

### Phase 4: Advanced Features

| Status | Component | Description |
|--------|-----------|-------------|
| [x] | Fragment-level access | Fragment enumeration, ID listing, scanner fragment filtering |
| [ ] | Compaction | Fragment consolidation operations |
| [ ] | Statistics export | Row counts, column stats for query planning |
| [x] | Cloud storage | S3, GCS, Azure via storage options pass-through |
| [x] | Package distribution | vcpkg and Conan recipe packaging |

### Additional (not in RFC)

| Status | Component | Description |
|--------|-----------|-------------|
| [x] | Async scan | Callback-based `lance_scanner_scan_async()` for non-blocking scans |
| [x] | Dataset metadata | `lance_dataset_version()`, `lance_dataset_count_rows()`, `lance_dataset_latest_version()` |
| [x] | Substrait filter pushdown | `lance_scanner_set_substrait_filter()` accepts a serialized Substrait `ExtendedExpression` (preferred over SQL strings for query engines) |

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
