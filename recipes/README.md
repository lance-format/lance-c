# Conan recipe for lance-c

Canonical source of the recipe published to `conan-io/conan-center-index`.

## Local test

```bash
conan create recipes/lance-c/all --version=0.1.9
```

The prebuilt path downloads the v0.1.9 GitHub release archive for Linux
x86_64/aarch64 or macOS arm64. Test both linkage modes with
`-o shared=True` and `-o shared=False`. Intel macOS has no prebuilt archive in
this release; use `-o lance-c/*:from_source=True`, which builds the v0.1.9
source tag and requires Rust, CMake, and `protoc`.

The release workflow now builds an `x86_64-apple-darwin` archive for future
tags. It is not part of the v0.1.9 `conandata.yml`. Add the `Macos-x86_64`
prebuilt entry only after that asset is published.

The Linux x86_64 entry uses the Debian 12 `v0.1.9` archive to keep the lower
Debian 12 libc floor. The `Linux/armv8` entry uses the standard `v0.1.9`
archive; static inspection of its shared library records `GLIBC_2.39` there
and `GLIBC_2.35` in the Debian 12 archive.

The `0.1.9` entries stay pinned to those assets. Future releases publish one
generic Linux archive per architecture targeting glibc 2.28 and no Debian 12
variants; add new version entries only after the archives are published. See
[Linux baseline for future
packages](../README.md#linux-baseline-for-future-packages).
