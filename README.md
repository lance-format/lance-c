# lance-c

The C/C++ binding to [Lance](https://github.com/lancedb/lance), providing native access to the Lance columnar format via a stable C ABI and header-only C++ RAII wrappers.

- **C header:** [`include/lance.h`](include/lance.h)
- **C++ wrappers:** [`include/lance.hpp`](include/lance.hpp) (header-only, RAII, exceptions)
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
| [ ] | Dataset write | Create / append / overwrite from ArrowArrayStream |
| [x] | Fragment writer | Batch-at-a-time fragment file writing (no commit) via `lance_write_fragments()` |
| [ ] | Delete operations | Predicate-based deletion |
| [ ] | Update operations | Expression-based row updates |
| [ ] | Merge-insert | Upsert functionality with builder pattern |
| [ ] | Schema evolution | Add/drop/alter columns with expressions |
| [ ] | Version management | Checkout, restore, list version operations |

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

Releases are tag-driven via [`release.yml`](.github/workflows/release.yml).

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
5. `release.yml` fires on the tag push and builds prebuilt tarballs for `linux-{x86_64,aarch64}` and `macos-{x86_64,aarch64}`. ~20 minutes later, the [GitHub Release](https://github.com/lance-format/lance-c/releases) has all four `.tar.xz` artifacts plus a `SHA512SUMS` file.
6. The `publish` job's log emits a paste-ready `set(LANCE_C_SHA512_... "...")` snippet. Copy it into:
   - [`ports/lance-c/portfile.cmake`](ports/lance-c/portfile.cmake) (SHA512s)
   - [`recipes/lance-c/all/conandata.yml`](recipes/lance-c/all/conandata.yml) (SHA256s, derived from the `.sha256` files in the release assets)
7. Open follow-up PRs to `microsoft/vcpkg` and `conan-io/conan-center-index` mirroring the updated `ports/` and `recipes/` directories.

A `workflow_dispatch` trigger on `release.yml` lets you do dry-run builds without cutting a tag — Actions tab → "Release" → "Run workflow" → enter a version like `0.0.1-dev`. The `publish` job is skipped (gated on `refs/tags/v`), but the build matrix runs end-to-end so you can validate it before the real tag.

## License

Apache-2.0
