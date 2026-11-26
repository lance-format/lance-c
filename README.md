# lance-c

Read, write, and search [Lance](https://lance.org/) datasets from C or C++. The library exposes a C ABI and header-only C++ RAII wrappers. Scans and writes exchange Arrow batches through the [Arrow C Data Interface](https://arrow.apache.org/docs/format/CDataInterface.html).

Prebuilt packages support Linux x86_64 and AArch64 (glibc 2.28+), macOS arm64, and Windows x64 and ARM64 with the static `/MT` CRT. The C API requires C11; the C++ wrapper requires C++20. Download stable packages from the [latest stable release](https://github.com/lance-format/lance-c/releases/latest); the [releases page](https://github.com/lance-format/lance-c/releases) lists previews, older releases, and their notes.

<a id="quickstart"></a>

## Quickstart

On Linux x86_64, use a C++20 compiler and CMake 3.22+ to build this C++ consumer against the prebuilt library. The release archive supplies the compiled library; the C API itself requires C11. A shallow checkout of the same release tag supplies the standalone example and sample dataset; copy the example out before building. You can run this without a Rust toolchain.

The [C and C++ quickstart](docs/quickstart.md) follows the same installed-package flow and adds the C++ write/index/query workflow, the C scan example, and links to the generated API reference. The commands below are the short archive verification path.

```bash
mkdir lance-c-quickstart && cd lance-c-quickstart

# Replace vX.Y.Z with a stable tag from https://github.com/lance-format/lance-c/releases/latest
TAG=vX.Y.Z
ARCHIVE=lance-c-$TAG-x86_64-unknown-linux-gnu.tar.xz

curl -fLO "https://github.com/lance-format/lance-c/releases/download/$TAG/$ARCHIVE"
curl -fLO "https://github.com/lance-format/lance-c/releases/download/$TAG/SHA512SUMS"
sha512sum --ignore-missing -c SHA512SUMS
mkdir install
tar -xJf "$ARCHIVE" -C install

git clone --depth 1 --branch "$TAG" https://github.com/lance-format/lance-c.git source
cp -R source/examples/cmake-consumer consumer

cmake -S consumer -B build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/install"
cmake --build build --target consumer
./build/consumer "source/test_data/v0.27.1/pq_in_schema"
```

Expected output:

```text
rows: 512, version: 2
```

The `consumer` directory has no dependency on the checkout layout and also runs against any other Lance dataset. For other platforms, see the [release archives and platform requirements](docs/building-and-packaging.md#published-prebuilt-archives). Pair the library with examples whose API matches the release you downloaded.

## Add Lance to a project

With CMake, set `CMAKE_PREFIX_PATH` to the unpacked archive. For an existing target named `my_app`:

```cmake
find_package(LanceC CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE LanceC::lance_c)
```

C and C++ applications using `<lance/lance.h>` link `LanceC::lance_c`.
For the C++20 wrapper `<lance/lance.hpp>`, link `LanceC::lance_cpp` instead;
it selects the required C++ standard and links the C library.

CMake selects the static library by default. Configure with `-DLanceC_LINK=shared` to link the shared library, which exports only the `lance_*` C ABI and hides its embedded native symbols from those exports. Prefer it when the host itself links a library that exports the same native symbol names, such as `ZSTD_*` or `LZ4_*`. See [choosing static or shared linkage](docs/building-and-packaging.md#choosing-static-or-shared-linkage). On Windows, static consumers must use the Release `/MT` runtime. See [Windows linking instructions](docs/building-and-packaging.md#consuming-the-windows-package-and-mt-crt-rules). The public C ABI is checked against the published export baseline; see [ABI compatibility](docs/building-and-packaging.md#abi-compatibility).

With pkg-config, point `PKG_CONFIG_PATH` at the archive's `lib/pkgconfig`:

```bash
export PKG_CONFIG_PATH=/path/to/lance-c-install/lib/pkgconfig
cc my_app.c $(pkg-config --cflags --libs lance-c) -o my_app
```

Add `--static` and link `liblance_c.a` explicitly when using the static archive; see [pkg-config usage](docs/building-and-packaging.md#using-pkg-config).

- **C++** — Include [lance/lance.hpp](include/lance/lance.hpp). Open a dataset with `lance::Dataset::open(uri)`, call `count_rows()` or create a scanner with `scan()`. Handles own their resources and failures throw `lance::Error`. The [CMake consumer](examples/cmake-consumer/main.cpp) is a complete program; the same directory also has a [write/index/query workflow](examples/cmake-consumer/workflow.cpp).
- **C** — Include [lance/lance.h](include/lance/lance.h). The [C11 scan example](examples/c-scan/) opens a dataset, iterates all Arrow batches, prints errors, and releases the batches, stream, and Lance handles. Set `LANCE_C_PREFIX` to the unpacked archive when building it with Make.

## Documentation

- [C and C++ quickstart](docs/quickstart.md) for an installed-package C++ write/query example and a C scan example.
- [C and C++ API guide](docs/api-guide.md) for scans, vector search, writes, transactions, dataset maintenance, [opt-in diagnostic logging](docs/api-guide.md#diagnostic-logging), [Tokio worker-thread configuration](docs/api-guide.md#tokio-runtime-worker-threads), and [process lifetime during embedding](docs/api-guide.md#process-lifetime-and-embedding).
- [API reference](https://lance-format.github.io/lance-c/) for the hosted Doxygen HTML reference (see [Generated API reference](docs/api-reference.md) for local rendering and CI checks).
- [Segment-scoped scalar scans](docs/scalar-segment-scans.md) and [publishing blob data](docs/blob-writer-dataset-publication.md) for specialized workflows.
- [Building and packaging](docs/building-and-packaging.md) for platform toolchains, source builds, and releases.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for local build, test, Doxygen and ABI expectations. Published changes are summarized in [CHANGELOG.md](CHANGELOG.md).

## License

[Apache License 2.0](LICENSE)
