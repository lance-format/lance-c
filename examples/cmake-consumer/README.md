# lance-c CMake consumer example

The example builds three executables:

- `consumer` opens the dataset URI passed on the command line.
- `acceptance` creates, closes, reopens and scans a dataset, verifies Arrow
  release callback counts, consumes a thread-local error message, and uses an
  independent cloned dataset handle. Pass a writable `file://` URI.
- `workflow` writes a small dataset with `id`, `text` and `embedding` columns,
  builds BTree, full-text and IVF vector indices, then reopens the dataset and
  runs a filtered vector query and a full-text query. Pass a new writable
  `file://` URI; creation fails if the path already contains a dataset.

This directory is standalone. Inside the lance-c source tree it reads
`Cargo.toml` and requests the package version built from that tree. Once copied
elsewhere, it calls `find_package(LanceC CONFIG REQUIRED)` without a version and
accepts the package in `CMAKE_PREFIX_PATH`; the repository layout is not
required.

The executables link `LanceC::lance_cpp` for the C++20 wrappers. Applications
using only `<lance/lance.h>` link `LanceC::lance_c` and can use C11 or C++17.

## Build

After unpacking the [0.2.x release archive](../../README.md#quickstart), use its install prefix:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/lance-c-install
cmake --build build
./build/consumer /path/to/dataset.lance
./build/workflow file:///tmp/lance-c-workflow.lance
```

The `workflow` target uses public calls present in v0.2.0 and prints:

```text
write: rows=6 version=1
indices: count=3 version=4
vector query: 4 5
fts query: 1 2 4
workflow: PASS
```

On Windows, the package config reports whether the libraries were built with
the static MSVC CRT. The example selects `MultiThreaded` when
`LanceC_STATIC_CRT` is on, because the installed static archive is a Release
`/MT` library and an imported target cannot propagate the consumer runtime
setting. The shared DLL does not require a redistributable VC Runtime.
