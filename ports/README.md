# vcpkg overlay port for lance-c

This directory is the canonical copy of the vcpkg port that ships in the
upstream `microsoft/vcpkg` registry. Use it as an overlay against any vcpkg
checkout to install lance-c locally:

```bash
vcpkg install lance-c --overlay-ports=path/to/lance-c/ports
```

The v0.1.9 port downloads prebuilt archives for `x64-linux`, `arm64-linux`,
and `arm64-osx`. The port's `supports` field allows macOS only on `arm64`;
Intel macOS (`x64-osx`) is not supported and is rejected in both prebuilt and
`from-source` modes. Other Linux triples without a prebuilt asset can use the
`from-source` feature; that path requires a Rust toolchain, CMake, and
`protoc`. Test both `x64-linux` and `x64-linux-dynamic` to cover static and
shared installation.

The release workflow no longer builds an `x86_64-apple-darwin` archive, so no
`x64-osx` mapping or release SHA512 will be added. The v0.1.9 mappings and
hashes stay as published.

The `x64-linux` and `x64-linux-dynamic` mappings select the Debian 12 x86_64
`v0.1.9` archive so the default prebuilt keeps the lower Debian 12 libc floor.
The `arm64-linux` mapping selects the standard `v0.1.9` release archive; static
inspection of its shared library records `GLIBC_2.39` there and `GLIBC_2.35` in
the Debian 12 archive.

These mappings and hashes stay pinned to the `v0.1.9` assets. Future releases
publish one generic Linux archive per architecture targeting glibc 2.28 and no
Debian 12 variants; change the mappings and hashes only after such an archive
is published. See [Linux baseline for future
packages](../README.md#linux-baseline-for-future-packages).

After each release, the contents are mirrored into `microsoft/vcpkg` via PR.
