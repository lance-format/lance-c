# Issue 103 published artifact audit

Run: 2026-11-10T19:49:21Z through 2026-11-10T19:50:44Z (UTC)

- Execution host: `nier` (Linux x86_64, GNU readelf 2.42, LLVM 18.1.3).
- Audit script revision: `1f0c282293b5e458ae5765aecd9f369aef76f314`.
- Work root: `/home/xuanwo/work/lance-c-103/run-20261110`.
- Log: `/home/xuanwo/work/lance-c-103/run-20261110.log`.
- Recorded exit status: `0` in
  `/home/xuanwo/work/lance-c-103/run-20261110.exit`.
- Completion marker: `status=ok` in
  `/home/xuanwo/work/lance-c-103/run-20261110/logs/status.txt`.

The audit downloaded all five v0.1.9 archives and their `.sha256`, `.sha512`,
and aggregate `SHA512SUMS` files. Checksum verification, extraction, ELF
inspection, and Mach-O inspection all completed successfully. No build or
target-platform program was executed.

## Published artifact identity

All five archives matched their published SHA256, SHA512, and aggregate
SHA512SUMS entries. The aggregate check is recorded in
`logs/checksums.txt`; locally computed digests are in
`logs/downloaded-digests.txt`.

| Archive | Bytes | SHA256 | Shared-library SHA256 |
| --- | ---: | --- | --- |
| `lance-c-v0.1.9-x86_64-unknown-linux-gnu.tar.xz` | 141817376 | `4ced4f4954096506ba130bd9f6bae2d18d1afc440dcb24439c3a1f8efa97f6c5` | `59311e31136d86a2aa73c413a6f5cee1534d682a0c74cfb5a5fc57e728fba081` |
| `lance-c-v0.1.9-x86_64-unknown-linux-gnu-debian12.tar.xz` | 142023364 | `51e8175efe3420149039525524b803878c1054dc2cd881a92295a27301b780aa` | `052d48e37e80eb165e0989c82e9090e857f775a6ac1bacc5332d377376614a4f` |
| `lance-c-v0.1.9-aarch64-unknown-linux-gnu.tar.xz` | 134659936 | `fb48f848c128f12b209a4b6dd12bf7fa547434eba1483326a9c39b645bb142bd` | `ce6cd283c5d22f8402783ceac87cb8bea122f66d0c847e6f567c47f1741349d7` |
| `lance-c-v0.1.9-aarch64-unknown-linux-gnu-debian12.tar.xz` | 134553024 | `8c14c600f0005cb272619c4e2b27f34026817ac6dd642ad267ed71f0a9985dd7` | `3e1056529f55d89cabf412528b5fa2e059059c4d916da318ac79bd4990bcdc19` |
| `lance-c-v0.1.9-aarch64-apple-darwin.tar.xz` | 118638868 | `dbbf6b62d091c134643e4ce84eda868fe72c3dcb43414ecacce83f60756b6b31` | `9c1a102601971abdd39fc6508ebf91048e792f1154c6d766d98924f24e455db8` |

Published SHA512 values, all verified:

- x86_64 standard: `edcd1bb4c8097486c80aaf4b41c72bec42561370ba97cfddc68015a5df958a2b14c38d85ee5625dba59544bfee403f779e2e73f5508a29142b10c94f5ab5b5e7`
- x86_64 Debian 12: `5aba369dd70b244d905298eacac93a3f5ddaed4b3df43e6a9fe0184303f1ce06f60794da6ea6288f15ade20f29120a8683bc1fbfdcef65b06f1e1e7944493595`
- aarch64 standard: `0a8c91a07ca82a15a635a2110cd70ed85bc3eaf4009284a8509395f9da4ef9fa85510f68cddec466a3609b22c5c2c56c0e4e004279dbd8d074d2c61097f30ee9`
- aarch64 Debian 12: `e170c35b61dbaa237a19d59f67940b96636dddd999fd4e855cb6ccdac478f3f08860c2ce5731aa1f3731bf2b86214bcd28033aee250b84aef91cf1e1b1c0172f`
- macOS arm64: `488463b062e25a100b56837613b37efd332e031a82a669eb2a882435d72d11e72dd3c966f9e53ca7197845126cecf2a64be722086f45f83deb2d8171a19d542b`

Every archive contains the C and C++ headers, `lib/liblance_c.a`, the
platform shared library, `lib/cmake/LanceC/`, `lib/pkgconfig/lance-c.pc`, and
the license. The macOS archive is a thin `arm64` Mach-O dylib, not a universal
binary.

## ELF metadata

`lib/liblance_c.so` was inspected with `readelf --file-header`,
`readelf --dynamic`, and `readelf --version-info`. Raw output is in
`logs/lance-c-v0.1.9-<target>.elf-metadata.txt`.

| Archive | Machine | SONAME | Direct `DT_NEEDED` | Highest GLIBC reference | GLIBCXX | CXXABI |
| --- | --- | --- | --- | --- | --- | --- |
| x86_64 standard | x86-64 | `liblance_c.so` | `libgcc_s.so.1`, `libm.so.6`, `libc.so.6`, `ld-linux-x86-64.so.2` | `GLIBC_2.39` | none | none |
| x86_64 Debian 12 | x86-64 | `liblance_c.so` | `libgcc_s.so.1`, `libm.so.6`, `libc.so.6`, `ld-linux-x86-64.so.2` | `GLIBC_2.35` | none | none |
| aarch64 standard | AArch64 | `liblance_c.so` | `libgcc_s.so.1`, `libm.so.6`, `libc.so.6` | `GLIBC_2.39` | none | none |
| aarch64 Debian 12 | AArch64 | `liblance_c.so` | `libgcc_s.so.1`, `libm.so.6`, `libc.so.6` | `GLIBC_2.35` | none | none |

The x86_64 rows reuse the accepted findings from #103 comment 6766726097.
This run re-read the same published bytes and recorded their library hashes;
it did not repeat the Debian 12 loader or package-consumer runs.

The arm64 standard library references both `GLIBC_2.38` and `GLIBC_2.39`;
the Debian 12 variant references `GLIBC_2.35` as its newest glibc version.
The highest referenced symbol version is a static loading constraint, not a
fully supported runtime floor. No dynamic loader, API, or consumer run was
performed for either arm64 archive.

The first member of each arm64 `liblance_c.a` is an AArch64 ELF relocatable
object. The macOS `liblance_c.a` is a Mach-O archive containing thin arm64
objects. Static archives were not linked or executed on either target.

## Mach-O metadata

The macOS dylib was inspected with LLVM 18 `llvm-lipo`, `llvm-objdump
--macho --dylib-id`, `--macho --rpaths`, `--macho --dylibs-used`, and
`--macho --private-headers`. Raw output is in
`logs/lance-c-v0.1.9-aarch64-apple-darwin.macho-metadata.txt`.

- Architecture: thin `arm64`.
- Install name: `@rpath/liblance_c.dylib`.
- `LC_BUILD_VERSION`: platform `macos`, `minos 11.0`, `sdk 14.5`,
  linker 1053.12.
- `LC_RPATH`: none.
- Dependencies: `SystemConfiguration`, `libobjc.A.dylib`, `IOKit`,
  `CoreFoundation`, `Security`, `libiconv.2.dylib`, and `libSystem.B.dylib`.
- No `LC_VERSION_MIN_MACOSX` command is present.

This is the deployment target declared in the published dylib, not evidence
that every API path was exercised on macOS 11. No macOS consumer was executed
for v0.1.9.

## x86_64 consumer evidence reused

The #100 validation record at
`validation/issue-100/RESULTS.md` (revision
`5a29b67`) tested source `438fa5534a0c0d087fe48470cb285d445b40f165` with
vcpkg and Conan prebuilt static/shared packages. It also tested source builds
and ran native Linux x86_64 C and C++ consumers. That evidence applies to the
Debian 12 x86_64 archive and the v0.1.9 source tag, not to the arm64 or macOS
archives audited here.

## Release runner preparation evidence

`.github/workflows/release.yml` still selects `macos-14` for
`aarch64-apple-darwin`. GitHub Actions macOS 14 images retired on November 2,
2026 (`actions/runner-images#15226`).

The accepted consumer workflow uses `macos-15`. Its #120 head
`0d54b6570fb69bf2c90c5b382ebf1937b9f73621` ran job
`find_package(LanceC) smoke test (macos-15, macos)` successfully in
[run 42827861818](https://github.com/lance-format/lance-c/actions/runs/42827861818).
The job log identifies `Image: macos-15-arm64` and macOS `15.8.2`; configure,
build, install, consumer build, and consumer run steps all succeeded.

A minimal release preparation should change only that matrix row to
`macos-15` and add a pre-tar assertion on the staged artifact:

- `lipo -archs stage/lib/liblance_c.dylib` must be exactly `arm64`.
- `otool -l stage/lib/liblance_c.dylib` (or `vtool -show-build`) must report
  the intended `minos`; the v0.1.9 archive declares `11.0`.

The check belongs in the release job because CI consumers are built from the
source tree, not from the published archive. The final minimum macOS floor
remains a separate decision.
