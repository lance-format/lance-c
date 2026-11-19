# lance-c

Read, write, and search [Lance](https://lance.org/) datasets from C or C++. The library exposes a C ABI and header-only C++ RAII wrappers. Scans and writes exchange Arrow batches through the [Arrow C Data Interface](https://arrow.apache.org/docs/format/CDataInterface.html).

The maintained release line is **0.2.x**. The current [v0.2.0 release](https://github.com/lance-format/lance-c/releases/tag/v0.2.0) uses Lance 15.0.0-beta.25 (`f5dbd55d`); a later 0.2.x patch will adopt upstream Lance 15.0.0 while preserving the C ABI. Prebuilt packages support Linux x86_64 and AArch64 (glibc 2.28+), macOS arm64, and Windows x64 and ARM64 with the static `/MT` CRT. The C API requires C11; the C++ wrapper requires C++20.

<a id="quickstart"></a>

## Quickstart

On Linux x86_64, use a C++20 compiler and CMake 3.22+ to build this C++ consumer against the prebuilt library. The release archive supplies the compiled library; the C API itself requires C11. A shallow checkout of the same release tag supplies the standalone example and sample dataset; copy the example out before building. You can run this without a Rust toolchain.

```bash
mkdir lance-c-quickstart && cd lance-c-quickstart

TAG=v0.2.0
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

The `consumer` directory has no dependency on the checkout layout and also runs against any other Lance dataset. For other platforms, see the [release archives and platform requirements](docs/building-and-packaging.md#published-prebuilt-archives). Pair the library with examples whose API matches its release.

## Add Lance to a project

With CMake, set `CMAKE_PREFIX_PATH` to the unpacked archive. For an existing target named `my_app`:

```cmake
find_package(LanceC CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE LanceC::lance_c)
```

CMake selects the static library by default. Configure with `-DLanceC_LINK=shared` to use the shared library. On Windows, static consumers must use the Release `/MT` runtime. See [Windows linking instructions](docs/building-and-packaging.md#consuming-the-windows-package-and-mt-crt-rules).

With pkg-config, point `PKG_CONFIG_PATH` at the archive's `lib/pkgconfig`:

```bash
export PKG_CONFIG_PATH=/path/to/lance-c-install/lib/pkgconfig
cc my_app.c $(pkg-config --cflags --libs lance-c) -o my_app
```

Add `--static` and link `liblance_c.a` explicitly when using the static archive; see [pkg-config usage](docs/building-and-packaging.md#using-pkg-config).

- **C++** — Include [lance/lance.hpp](include/lance/lance.hpp). Open a dataset with `lance::Dataset::open(uri)`, call `count_rows()` or create a scanner with `scan()`. Handles own their resources and failures throw `lance::Error`. The [CMake consumer](examples/cmake-consumer/main.cpp) is a complete program; the same directory also has a [write/index/query workflow](examples/cmake-consumer/workflow.cpp).
- **C** — Include [lance/lance.h](include/lance/lance.h). The [C11 scan example](examples/c-scan/) opens a dataset, iterates all Arrow batches, prints errors, and releases the batches, stream, and Lance handles. Set `LANCE_C_PREFIX` to the unpacked archive when building it with Make.

## Documentation

- [C and C++ API guide](docs/api-guide.md) for scans, vector search, writes, transactions, dataset maintenance, [opt-in diagnostic logging](docs/api-guide.md#diagnostic-logging), and [Tokio worker-thread configuration](docs/api-guide.md#tokio-runtime-worker-threads).
- [Segment-scoped scalar scans](docs/scalar-segment-scans.md) and [publishing blob data](docs/blob-writer-dataset-publication.md) for specialized workflows.
- [Building and packaging](docs/building-and-packaging.md) for platform toolchains, source builds, and releases.

## License

[Apache License 2.0](LICENSE)
