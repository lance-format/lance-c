# Historical public C ABI fixtures

## v0.2.0

`v0.2.0/include/lance/lance.h` is the public C header from the verified
[`v0.2.0` release](https://github.com/lance-format/lance-c/releases/tag/v0.2.0).
It is a test fixture only: the build and installed packages never use it.

- Release archive: `lance-c-v0.2.0-x86_64-unknown-linux-gnu.tar.xz`
  (SHA-256 `950cba8fab68caf67125ea186f3309b8ee383154875519b50a9090b3eb94174f`)
- Header SHA-256: `947a912a14cb53f97237dbc7c03e5c09ef4337d46b459edc600d4d3c2c888d5c`
  (LF line endings; the Windows archive ships the same header with CRLF)
- Extract with:
  `tar -xJf lance-c-v0.2.0-x86_64-unknown-linux-gnu.tar.xz include/lance/lance.h`

`.github/scripts/verify-abi-types.sh` compiles this header and the current
`include/lance` headers into DWARF-bearing, compile-only probes. libabigail
compares the type information; the stripped release shared library is not used
because it carries no DWARF.

What the check compares:

- every `Lance*` struct, union and enum typedef declared with an inline body in
  the released header;
- the named `Lance*` structs with inline bodies and the three Arrow C Data
  Interface structs;
- every `Lance*` function-pointer typedef in the released header;
- every `lance_*` function listed in
  `.github/abi-baseline/v0.2.0-exports.txt`, through its function-pointer type.

It applies libabigail's default harmful-change classification. Struct member
insertions, field type or size changes, enum value changes, and function
parameter or return type changes fail. New public types and functions are not
in the probe and are therefore additive. New enumerators and qualifier-only
changes are reported by libabigail as harmless and are not rejected by
default. Export names are covered by `.github/abi-baseline`; current
header-to-Rust signatures and current struct/enum layout are covered by
`tests/support/abi_layout.rs` and the public-header signature suite.

The script runs in the `ABI types (v0.2.0)` CI job on Linux with
`abigail-tools` and a GCC or Clang compiler. Run it locally with:

```bash
CC=clang ABIDIFF=/path/to/abidiff bash .github/scripts/verify-abi-types.sh
```

The script also compiles two temporary drift controls on copies of the frozen
v0.2.0 fixture: an inserted `LanceOptionalU64` member and a changed
`lance_scanner_set_limit` parameter. Both must be reported by abidiff. The
fixture is verified against its recorded SHA-256 before use and is never
modified; current-header formatting therefore cannot make a control silently
edit nothing. The script interprets abidiff's documented exit-code bitmask so
an actual ABI change is distinguished from a tool or usage error.
