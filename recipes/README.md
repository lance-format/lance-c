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

The Linux x86_64 entry uses the Debian 12 archive to keep the lower Debian 12
libc floor. The arm64 entry remains on the standard release archive until its
exact floor is verified.
