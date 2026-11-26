# Changelog

`lance-c` follows [Semantic Versioning](https://semver.org/). Every published tag has release notes on the [GitHub releases page](https://github.com/lance-format/lance-c/releases); this file summarizes the published lines and links to the authoritative notes.

## 0.2.x

### 0.2.1 previews

The 0.2.1 preview series follows Lance 15.0 previews and release candidates through Lance 15.1.0-beta.2 while keeping the `v0.2.0` C ABI. It adds the published-export baseline and public type/signature checks, splits the C and C++ headers into domain declarations and opt-in inline definitions, and corrects CMake package and consumer linkage for installed and embedded builds.

Published previews: [beta.1](https://github.com/lance-format/lance-c/releases/tag/v0.2.1-beta.1), [beta.2](https://github.com/lance-format/lance-c/releases/tag/v0.2.1-beta.2), [beta.3](https://github.com/lance-format/lance-c/releases/tag/v0.2.1-beta.3), [beta.4](https://github.com/lance-format/lance-c/releases/tag/v0.2.1-beta.4), [beta.5](https://github.com/lance-format/lance-c/releases/tag/v0.2.1-beta.5), [beta.6](https://github.com/lance-format/lance-c/releases/tag/v0.2.1-beta.6). The [comparison from v0.2.0](https://github.com/lance-format/lance-c/compare/v0.2.0...v0.2.1-beta.6) lists the full set of changes.

### 0.2.0

The first stable release, published on 2026-11-20, after the [v0.2.0-beta.1 preview](https://github.com/lance-format/lance-c/releases/tag/v0.2.0-beta.1). It established the 0.2.x C ABI and the `v0.2.0` export baseline, and published prebuilt archives for Linux x86_64 and AArch64 (glibc 2.28+), macOS arm64, and Windows x64 and ARM64 with the static `/MT` CRT. The release completed C++20 wrapper coverage for the remaining C exports and added the binding version macros, opt-in diagnostic logging, and the installed-package consumer checks.

See the [v0.2.0 release notes](https://github.com/lance-format/lance-c/releases/tag/v0.2.0) and the [ABI compatibility](https://github.com/lance-format/lance-c/blob/main/docs/building-and-packaging.md#abi-compatibility) section for the contract that later releases preserve.

## 0.1.x

The 0.1.x development line (`v0.1.2` through `v0.1.9`) preceded the stable 0.2.0 baseline. These were development releases, not 0.2.0 previews; `v0.1.9` is a normal, non-prerelease tag. The header-only C++ RAII wrapper was already present in the early tags, and 0.2.0 completed wrapper coverage for the remaining C exports. The releases, archives and notes remain available on the [releases page](https://github.com/lance-format/lance-c/releases?q=v0.1), starting with [v0.1.2](https://github.com/lance-format/lance-c/releases/tag/v0.1.2) and ending with [v0.1.9](https://github.com/lance-format/lance-c/releases/tag/v0.1.9).

## Reporting a change

Changes that affect the public C ABI, the C++ wrapper behavior, or the published package layout belong in the release notes for the tag that carries them. This summary is updated when a release line changes; the linked release notes remain the complete per-release record.
