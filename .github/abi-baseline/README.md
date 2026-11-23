<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: Copyright The Lance Authors -->

# Published export baseline

These files record the public `lance_*` export names of a published release so
CI can reject accidental removals. The comparison covers names only; it does
not detect changed signatures, struct or enum layouts, or changed behavior.
`v0.2.0-exports.txt` lists the 890 symbols exported by the verified
[v0.2.0 release](https://github.com/lance-format/lance-c/releases/tag/v0.2.0);
all five release archives exported the same set.

The list was extracted from the published archives after their SHA-256 and
SHA-512 values matched the release checksums:

- Linux: `nm -D --defined-only -j liblance_c.so | sed 's/@.*//' | sort -u`
- macOS: `nm -gU -j liblance_c.dylib | sed 's/^_//' | sort -u`
- Windows: the `Name` fields of `llvm-readobj --coff-exports lance_c.dll`

The release workflows and CI compare each staged library with this file.
Removing a published export fails the check; exports added beyond the baseline
are reported and allowed. The existing `verify-shared-exports.sh` check still
requires the staged library to match the installed header exactly, so an
intentional removal must also drop the declaration from the header. Struct and
enum layouts are checked separately by `tests/support/abi_layout.rs`.

Adopting a new baseline is an explicit ABI decision. Update this directory,
the workflow arguments, and the release notes in the same change.
