# vcpkg overlay port for lance-c

This directory is the canonical copy of the vcpkg port that ships in the
upstream `microsoft/vcpkg` registry. Use it as an overlay against any vcpkg
checkout to install lance-c locally:

```bash
vcpkg install lance-c --overlay-ports=path/to/lance-c/ports
```

The v0.1.9 port downloads prebuilt archives for `x64-linux`, `arm64-linux`,
and `arm64-osx`. Intel macOS and other supported Linux triples without a
prebuilt asset can use the `from-source` feature; that path requires a Rust
toolchain, CMake, and `protoc`. Test both `x64-linux` and `x64-linux-dynamic`
to cover static and shared installation.

The release workflow now builds an `x86_64-apple-darwin` archive for future
tags. It is not part of the v0.1.9 assets. Add an `x64-osx` mapping and its
release SHA512 only after that asset is published.

The `x64-linux` and `x64-linux-dynamic` mappings select the Debian 12 x86_64
archive so the default prebuilt keeps the lower Debian 12 libc floor. The
`arm64-linux` mapping still selects the standard release archive; static
inspection of the `v0.1.9` aarch64 shared library records `GLIBC_2.39` there
and `GLIBC_2.35` in the Debian 12 archive.

After each release, the contents are mirrored into `microsoft/vcpkg` via PR.
