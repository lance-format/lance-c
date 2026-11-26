# Getting Started with C and C++

`lance-c` is the C and C++ binding for the Lance format. It exposes a stable C11 ABI for reading, writing, and searching Lance datasets and header-only C++20 RAII wrappers over that ABI. Scans and writes exchange Apache Arrow batches through the [Arrow C Data Interface](https://arrow.apache.org/docs/format/CDataInterface.html).

This page installs a prebuilt package and runs the C++ write/query example and the C scan example from the matching release tag. See the [README](https://github.com/lance-format/lance-c/blob/main/README.md) for the platform matrix and the [API guide](https://github.com/lance-format/lance-c/blob/main/docs/api-guide.md) for the full API.

## Install the prebuilt package

The release archives cover Linux x86_64 and AArch64 (glibc 2.28+), macOS arm64, and Windows x64 and ARM64 with the static `/MT` CRT. Download a stable archive and its checksums, verify them, and unpack the install prefix:

```bash
mkdir lance-c-quickstart && cd lance-c-quickstart

# Replace vX.Y.Z with a tag from https://github.com/lance-format/lance-c/releases/latest
TAG=vX.Y.Z
ARCHIVE=lance-c-$TAG-x86_64-unknown-linux-gnu.tar.xz

curl -fLO "https://github.com/lance-format/lance-c/releases/download/$TAG/$ARCHIVE"
curl -fLO "https://github.com/lance-format/lance-c/releases/download/$TAG/SHA512SUMS"
sha512sum --ignore-missing -c SHA512SUMS
mkdir install
tar -xJf "$ARCHIVE" -C install
```

On macOS arm64, set `ARCHIVE=lance-c-$TAG-aarch64-apple-darwin.tar.xz`, download its `.sha512` file, and verify it with `shasum -a 512 -c "$ARCHIVE.sha512"`. See [published prebuilt archives](https://github.com/lance-format/lance-c/blob/main/docs/building-and-packaging.md#published-prebuilt-archives) for the other targets, pkg-config usage, and the Windows `/MT` rules.

For a CMake project, point `CMAKE_PREFIX_PATH` at the unpacked prefix, request C++20 for the wrapper, and link the C target:

```cmake
find_package(LanceC CONFIG REQUIRED)
target_compile_features(my_app PRIVATE cxx_std_20)
target_link_libraries(my_app PRIVATE LanceC::lance_c)
```

`LanceC::lance_c` links the C library and selects static or shared linkage through `LanceC_LINK`. The `v0.2.0` release archive provides this target and its `_static` and `_shared` variants; newer packages also provide `LanceC::lance_cpp`, which adds the same C++20 requirement. Applications that use only `<lance/lance.h>` need C11 and do not request C++20. CMake selects the static library by default; pass `-DLanceC_LINK=shared` to link the shared library.

## Write, index, and query from C++

The release tag contains a standalone CMake consumer with three programs. Copy it out of the source checkout so the build does not depend on the repository layout, then build `workflow`, which writes a small dataset, creates BTree, full-text and IVF vector indices, and runs a filtered vector query and a full-text query:

```bash
git clone --depth 1 --branch "$TAG" https://github.com/lance-format/lance-c.git source
cp -R source/examples/cmake-consumer consumer

cmake -S consumer -B build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/install"
cmake --build build --target workflow
./build/workflow "file://$PWD/quickstart.lance"
```

Expected output:

```text
write: rows=6 version=1
indices: count=3 version=4
vector query: 4 5
fts query: 1 2 4
workflow: PASS
```

The same build also produces `consumer`, which opens an existing dataset and prints its row count and version:

```bash
cmake --build build --target consumer
./build/consumer source/test_data/v0.27.1/pq_in_schema
```

```text
rows: 512, version: 2
```

## Read from C

C callers include `<lance/lance.h>`, the umbrella header, and link `LanceC::lance_c`. The `v0.2.0` release archive ships this header; newer packages and the current source also split the read path into domain headers, described in the [API guide](https://github.com/lance-format/lance-c/blob/main/docs/api-guide.md#header-inclusion). The tag's C example opens a dataset, exports a scanner as an `ArrowArrayStream`, reads every batch, and releases the batches, stream, scanner and dataset on every path:

```bash
cp -R source/examples/c-scan c-scan
make -C c-scan LANCE_C_PREFIX="$PWD/install"
./c-scan/build/lance-c-scan source/test_data/v0.27.1/pq_in_schema
```

```text
rows: 512, version: 2
```

The Makefile embeds the prefix in the runtime search path. If the prefix moves after the build, set `LD_LIBRARY_PATH` on Linux or `DYLD_LIBRARY_PATH` on macOS to its `lib` directory. The [C scan README](https://github.com/lance-format/lance-c/blob/main/examples/c-scan/README.md) shows the equivalent pkg-config and static-link commands.

## Next steps

- [C and C++ API guide](https://github.com/lance-format/lance-c/blob/main/docs/api-guide.md) covers scans, vector and full-text search, writes, transactions, indices, cloud storage, errors and runtime configuration.
- [Generated API reference](https://lance-format.github.io/lance-c/) documents every public C function and C++ type.
- [Building and packaging](https://github.com/lance-format/lance-c/blob/main/docs/building-and-packaging.md) covers source builds, release archives, static versus shared linkage, and the Windows package.
- [Runnable examples](https://github.com/lance-format/lance-c/tree/main/examples) contains the C, C++ and cross-SDK programs used by the test suite.
