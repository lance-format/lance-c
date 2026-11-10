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

After each release, the contents are mirrored into `microsoft/vcpkg` via PR.
