# Conan recipe for lance-c

Use this local recipe to add lance-c to your Conan cache.

## Local test

```bash
conan create recipes/lance-c/all --version=0.1.9
```

The prebuilt path downloads the v0.1.9 GitHub release archive for Linux
x86_64/aarch64 or macOS arm64. Test both linkage modes with
`-o shared=True` and `-o shared=False`. Intel macOS is not a supported
platform for new releases, so the recipe rejects `Macos`/`x86_64` in both
prebuilt and `from_source` modes. A `from_source` build requires Rust, CMake,
and `protoc`.

Future releases publish no Intel macOS archive, so no `Macos-x86_64` prebuilt
entry will be added to `conandata.yml`. The v0.1.9 entries stay as published.

The Linux x86_64 entry uses the Debian 12 `v0.1.9` archive to keep the lower
Debian 12 libc floor. The `Linux/armv8` entry uses the standard `v0.1.9`
archive; static inspection of its shared library records `GLIBC_2.39` there
and `GLIBC_2.35` in the Debian 12 archive.

The `0.1.9` entries stay pinned to those assets. The `0.2` release line
(starting with the `v0.2.0-beta.1` pre-release) publishes one generic Linux
archive per architecture targeting glibc 2.28 and no Debian 12 variants. See
[Installation and platform requirements](../README.md#linux-baseline-for-future-packages)
and [Linux baseline for 0.2 and future packages](../docs/building-and-packaging.md#linux-baseline-for-future-packages).
