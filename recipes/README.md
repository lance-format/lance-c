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
