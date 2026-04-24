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
| [ ] | Vector search | Nearest-neighbor via scanner with metric/k/nprobes |
| [ ] | Full-text search | FTS queries through scanner interface |
| [ ] | Vector index creation | IVF_PQ, IVF_FLAT, IVF_SQ, HNSW variants |
| [ ] | Scalar index creation | BTree, Bitmap, Inverted, Label-List indexes |
| [ ] | Index management | List and drop index operations |
| [ ] | C++ wrappers | `create_vector_index()` and `create_scalar_index()` methods |

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
| [ ] | Package distribution | vcpkg and Conan recipe packaging |

### Additional (not in RFC)

| Status | Component | Description |
|--------|-----------|-------------|
| [x] | Async scan | Callback-based `lance_scanner_scan_async()` for non-blocking scans |
| [x] | Dataset metadata | `lance_dataset_version()`, `lance_dataset_count_rows()`, `lance_dataset_latest_version()` |

## Building

```bash
cargo build --release
```

The build produces `liblance_c.{so,dylib,dll}` and the headers in `include/`.

## Usage

### C

```c
#include "lance.h"

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
#include "lance.hpp"

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

## License

Apache-2.0
