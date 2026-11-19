# lance-c

Read, write, and search [Lance](https://lance.org/) datasets from C or C++. The library exposes a C ABI and header-only C++ RAII wrappers. Scans and writes exchange Arrow batches through the [Arrow C Data Interface](https://arrow.apache.org/docs/format/CDataInterface.html).

<a id="published-prebuilt-archives"></a>
<a id="linux-baseline-for-future-packages"></a>

| Release | Lance engine | C++ | Prebuilt platforms |
| --- | --- | --- | --- |
| [Stable v0.1.9](https://github.com/lance-format/lance-c/releases/tag/v0.1.9) | 9.1.0-beta.3 | C++17 | Linux x86_64 and arm64 (glibc 2.35 for Debian 12 archives, 2.39 for standard archives), macOS arm64 |
| [Preview v0.2.0-beta.1](https://github.com/lance-format/lance-c/releases/tag/v0.2.0-beta.1) | 15.0.0-beta.25 | C++20 | Linux x86_64 and arm64 (glibc 2.28), macOS arm64, Windows x64 and ARM64 |

<a id="quickstart-v019"></a>

## Quickstart

On Linux x86_64, use a C++17 compiler and CMake 3.22+ to open a dataset with the stable package. The release archive supplies the compiled library. The source archive supplies the matching example and sample data. You can run this from outside the repository without a Rust toolchain.

```bash
mkdir lance-c-quickstart && cd lance-c-quickstart

TAG=v0.1.9
VERSION=0.1.9
ARCHIVE=lance-c-v0.1.9-x86_64-unknown-linux-gnu-debian12.tar.xz

curl -fLO "https://github.com/lance-format/lance-c/releases/download/$TAG/$ARCHIVE"
curl -fLO "https://github.com/lance-format/lance-c/releases/download/$TAG/SHA512SUMS"
sha512sum --ignore-missing -c SHA512SUMS
mkdir install
tar -xJf "$ARCHIVE" -C install

curl -fL -o "lance-c-$TAG-source.tar.gz" \
  "https://github.com/lance-format/lance-c/archive/refs/tags/$TAG.tar.gz"
tar -xzf "lance-c-$TAG-source.tar.gz"

cmake -S "lance-c-$VERSION/examples/cmake-consumer" -B build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/install"
cmake --build build --target consumer
./build/consumer "lance-c-$VERSION/test_data/v0.27.1/pq_in_schema"
```

Expected output:

```text
rows: 512, version: 2
```

<a id="preview-quickstart-v020-beta1"></a>

For the **0.2 preview**, use the [beta quickstart](docs/building-and-packaging.md#preview-quickstart-v020-beta1). For other platforms, see the [release archives and platform requirements](docs/building-and-packaging.md#published-prebuilt-archives). Pair the library with examples from the same tag.

## Add Lance to a project

With CMake, set `CMAKE_PREFIX_PATH` to the unpacked archive. For an existing target named `my_app`:

```cmake
find_package(LanceC CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE LanceC::lance_c)
```

CMake selects the static library by default. Configure with `-DLanceC_LINK=shared` to use the shared library. On Windows, static consumers must use the Release `/MT` runtime. See [Windows linking instructions](docs/building-and-packaging.md#consuming-the-windows-package-and-mt-crt-rules).

- **C++** — Include [lance/lance.hpp](include/lance/lance.hpp). Open a dataset with `lance::Dataset::open(uri)`, call `count_rows()` or create a scanner with `scan()`. Handles own their resources and failures throw `lance::Error`. The [CMake consumer](examples/cmake-consumer/main.cpp) is a complete program.
- **C** — Include [lance/lance.h](include/lance/lance.h). The [C11 scan example](examples/c-scan/) opens a dataset, iterates all Arrow batches, prints errors, and releases the batches, stream, and Lance handles. Set `LANCE_C_PREFIX` to the unpacked archive when building it with Make.

<a id="vcpkg-overlay-port-v019"></a>
<a id="conan-local-recipe-v019"></a>

The [vcpkg overlay](docs/building-and-packaging.md#vcpkg-overlay-port-v019) and [local Conan recipe](docs/building-and-packaging.md#conan-local-recipe-v019) install v0.1.9. Use the GitHub Release archive for the beta.

## Documentation

- [C and C++ API guide](docs/api-guide.md) for scans, vector search, writes, transactions, and dataset maintenance.
- [Segment-scoped scalar scans](docs/scalar-segment-scans.md) and [publishing blob data](docs/blob-writer-dataset-publication.md) for specialized workflows.
- [Building and packaging](docs/building-and-packaging.md) for platform toolchains, building from source, package recipes, and releases.

## License

[Apache License 2.0](LICENSE)
