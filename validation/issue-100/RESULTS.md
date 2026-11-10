# Issue 100 validation record

Task branch: `xuanwo/fix-vcpkg-conan-v019`

Tested revision: `438fa5534a0c0d087fe48470cb285d445b40f165`

Base revision: `a51ab47a9c814a746cb1714c99fad93dd3a188b8`

Execution host: `Ubuntu-2404-noble-amd64-base` (Ubuntu 24.04, x86_64, 32 cores). All
compilation and consumer execution ran there. The worktree was clean apart from
generated Conan test-package presets, which were removed after the run.

## Artifact identity

The recipes use the following verified v0.1.9 release assets:

| Artifact | SHA256 | SHA512 |
| --- | --- | --- |
| `lance-c-v0.1.9-x86_64-unknown-linux-gnu-debian12.tar.xz` | `51e8175efe3420149039525524b803878c1054dc2cd881a92295a27301b780aa` | `5aba369dd70b244d905298eacac93a3f5ddaed4b3df43e6a9fe0184303f1ce06f60794da6ea6288f15ade20f29120a8683bc1fbfdcef65b06f1e1e7944493595` |
| `lance-c-v0.1.9-aarch64-unknown-linux-gnu.tar.xz` | `fb48f848c128f12b209a4b6dd12bf7fa547434eba1483326a9c39b645bb142bd` | `0a8c91a07ca82a15a635a2110cd70ed85bc3eaf4009284a8509395f9da4ef9fa85510f68cddec466a3609b22c5c2c56c0e4e004279dbd8d074d2c61097f30ee9` |
| `lance-c-v0.1.9-aarch64-apple-darwin.tar.xz` | `dbbf6b62d091c134643e4ce84eda868fe72c3dcb43414ecacce83f60756b6b31` | `488463b062e25a100b56837613b37efd332e031a82a669eb2a882435d72d11e72dd3c966f9e53ca7197845126cecf2a64be722086f45f83deb2d8171a19d542b` |
| `v0.1.9.tar.gz` source archive | `c81fd9c9b4e1aa89c1ce6b5c7d71b8c62d4bfd2ebc55925db75d53c0c7383bfb` | `0262e1df6dddb9caac5bd375af6f0bf2860138599edd8a67e216fb53d91acb14fd48032409f8fe37bd0080119599a65b259795a53df76e88031af7314efd3b3b` |
| Corrosion `v0.5.1` source archive | `843334a9f0f5efbc225dccfa88031fe0f2ec6fd787ca1e7d55ed27b2c25d9c97` | `1f9337c779d619af3e69b0b0929d97150d289900d8b0c1a63c2faa7443c9053684c70dec6f27ee020c8fad33d597438293b9bc3c5dfd31fbeb406929bbca3ac5` |

The v0.1.9 tag resolves to `6de5efc3d573066aa75b170f0837a76dd2f837ec`.
`https://github.com/lance-format/lance-c/archive/v0.1.9.tar.gz` and
`https://github.com/lance-format/lance-c/archive/refs/tags/v0.1.9.tar.gz` are
byte-identical. The tag's `Cargo.toml` reports version `0.1.9`.

The Debian 12 x86_64 shared library reports maximum `GLIBC_2.35`; the standard
x86_64 archive reports `GLIBC_2.39`. The arm64 floor is not verified here.

## vcpkg

Tool: vcpkg `2026-10-29-139f2e53438b1529e178f6295bc563f4e0de734f`.

Prebuilt installation used a separate install root:

```bash
vcpkg install lance-c \
  --overlay-ports=/home/xuanwo/work/lance-c-100/src-base/ports \
  --triplet x64-linux \
  --x-install-root=/home/xuanwo/work/lance-c-100/vcpkg-prebuilt-root
vcpkg install lance-c \
  --overlay-ports=/home/xuanwo/work/lance-c-100/src-base/ports \
  --triplet x64-linux-dynamic \
  --x-install-root=/home/xuanwo/work/lance-c-100/vcpkg-prebuilt-root
```

Both exited 0. Logs:

- `/home/xuanwo/work/lance-c-100/logs/vcpkg-prebuilt-static.log`
- `/home/xuanwo/work/lance-c-100/logs/vcpkg-prebuilt-shared.log`

Consumers were configured with the vcpkg toolchain and
`VCPKG_INSTALLED_DIR=/home/xuanwo/work/lance-c-100/vcpkg-prebuilt-root`.
Both built and ran:

- static: `build_rc:0`, no-argument `2`, missing dataset `1`
- shared: `build_rc:0`, no-argument `2`, missing dataset `1`
- logs: `/home/xuanwo/work/lance-c-100/logs/consumer-vcpkg-prebuilt-{static,shared}.log`
- the runtime error path names `/github/home/.cargo/...`, identifying the
  Debian 12 release build environment.

Source installation:

```bash
vcpkg install 'lance-c[from-source]' \
  --overlay-ports=/home/xuanwo/work/lance-c-100/src-base/ports \
  --triplet x64-linux --recurse
vcpkg install 'lance-c[from-source]' \
  --overlay-ports=/home/xuanwo/work/lance-c-100/src-base/ports \
  --triplet x64-linux-dynamic --recurse
```

Both exited 0. Logs:

- `/home/xuanwo/work/lance-c-100/logs/vcpkg-source-x64-linux.log`
- `/home/xuanwo/work/lance-c-100/logs/vcpkg-source-x64-linux-dynamic.log`

The source build fetched and verified both the v0.1.9 tag archive and the
Corrosion v0.5.1 archive, then used `FETCHCONTENT_SOURCE_DIR_CORROSION` to avoid
configure-time network access. Source consumers built and ran:

- static: `build` succeeded, no-argument `2`, missing dataset `1`
- shared: `build` succeeded, no-argument `2`, missing dataset `1`
- logs: `/home/xuanwo/work/lance-c-100/logs/consumer-vcpkg-source-{static,shared}.log`

Installed source layouts contain both headers and exactly one selected library:
`lib/liblance_c.a` for `x64-linux`, `lib/liblance_c.so` for
`x64-linux-dynamic`. Both contain `share/LanceC/LanceCConfig.cmake`,
`lib/pkgconfig/lance-c.pc`, `share/lance-c/copyright`, and
`share/lance-c/usage`.

Unsupported-prebuilt behavior was exercised with an overlay triplet
`x64-linux-noprebuilt` that has no release mapping. The port stopped with:

```text
lance-c 0.1.9 does not publish a prebuilt archive for triplet
'x64-linux-noprebuilt'. Re-run with the 'from-source' feature to build the
v0.1.9 source tag (requires Rust, CMake, and protoc).
```

## Conan

Tool: Conan 2.34.0 in an isolated `uv` virtual environment.

Prebuilt static and shared:

```bash
conan create recipes/lance-c/all --version=0.1.9 -o 'lance-c/*:shared=False'
conan create recipes/lance-c/all --version=0.1.9 -o 'lance-c/*:shared=True'
```

Both exited 0 and ran the C and C++ test packages. Logs:

- `/home/xuanwo/work/lance-c-100/logs/conan-prebuilt-shared-False.log`
- `/home/xuanwo/work/lance-c-100/logs/conan-prebuilt-shared-True.log`

The logs show the Debian 12 x86_64 archive being downloaded. Package folders
contain headers, `licenses/LICENSE`, and exactly one selected library.

Source static and shared:

```bash
conan create recipes/lance-c/all --version=0.1.9 \
  -o 'lance-c/*:from_source=True' -o 'lance-c/*:shared=False'
conan create recipes/lance-c/all --version=0.1.9 \
  -o 'lance-c/*:from_source=True' -o 'lance-c/*:shared=True'
```

Both exited 0 and ran the C and C++ test packages. Logs:

- `/home/xuanwo/work/lance-c-100/logs/conan-source-shared-False.log`
- `/home/xuanwo/work/lance-c-100/logs/conan-source-shared-True.log`

Source packages contain headers, `licenses/LICENSE`, and only the selected
library. Upstream CMake and pkg-config files are removed so Conan-generated
metadata is authoritative.

Conan-generated pkg-config metadata was also exercised directly:

- static: `conan install ... -g PkgConfigDeps`, `pkg-config --static`, C
  consumer compiled and ran
- shared: `conan install ... -g PkgConfigDeps`, `pkg-config --cflags --libs`,
  C consumer compiled and ran with the reported libdir

## Limits

- Only native Linux x86_64 execution was possible. macOS, Linux arm64, and
  Intel macOS were not executed.
- The arm64 prebuilt remains the standard release archive; its exact libc floor
  is not verified.
- `x64-osx` compiler detection cannot run on this Linux host; the
  unsupported-prebuilt branch was verified with the overlay triplet above.
- Source builds use the host Rust/CMake/protoc toolchain and therefore inherit
  that host's libc; they are not prebuilt ABI claims.
- C++17 remains the consumer language requirement.
