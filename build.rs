// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Build script for lance-c.
//!
//! Optionally generates `include/lance.h` via cbindgen.
//! If cbindgen is not available, the pre-committed header is used.

use std::collections::BTreeSet;
use std::fmt::Write as _;
use std::fs;
use std::path::PathBuf;

fn main() {
    // cbindgen header generation is optional.
    // Run `cargo install cbindgen && cbindgen --crate lance-c -o include/lance.h`
    // to regenerate the header manually.

    configure_shared_library_link();
}

/// Configure the `cdylib` link without changing the static archive.
fn configure_shared_library_link() {
    // CARGO_CFG_TARGET_OS rather than cfg!(target_os), because a build script
    // is compiled for the host and these artifacts are cross-compiled.
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let target_env = std::env::var("CARGO_CFG_TARGET_ENV").unwrap_or_default();

    match target_os.as_str() {
        "linux" | "android" => {
            println!("cargo::rustc-cdylib-link-arg=-Wl,-soname,liblance_c.so");
            // rustc exports every reachable `#[no_mangle]` symbol from the
            // statically linked dependency graph. `crc-fast` exposes an FFI
            // surface through its default features, so its symbols otherwise
            // appear in `liblance_c.so`. Hiding archive members keeps the
            // public `lance_*` ABI while preventing dependency symbols from
            // colliding with a `dlopen`-ing host.
            println!("cargo::rustc-cdylib-link-arg=-Wl,--exclude-libs=ALL");
        }
        "macos" | "ios" => {
            // ld64 unions rustc's generated `-exported_symbols_list` with any
            // list supplied here, so a link argument cannot remove dependency
            // exports. The release linker shim
            // (.github/scripts/limit-macos-exports.sh) rewrites rustc's list
            // for the `liblance_c.dylib` link instead.
            println!("cargo::rustc-cdylib-link-arg=-Wl,-install_name,@rpath/liblance_c.dylib");
        }
        "windows" if target_env == "msvc" => {
            write_msvc_export_definition();
        }
        _ => {}
    }
}

/// Give `link.exe` an exact public export set.
///
/// rustc writes a `/DEF` file containing every reachable `no_mangle` symbol,
/// including symbols from statically linked Rust dependencies. Windows C
/// dependencies can also put `/EXPORT` directives in their object files (the
/// aws-lc jitter-entropy objects do this). MSVC link.exe uses the last `/DEF`
/// on the command line and does not add object `/EXPORT` directives when a DEF
/// is supplied, so a later header-derived DEF leaves only the declared C ABI.
/// This is link.exe behavior; lld-link unions those exports instead.
fn write_msvc_export_definition() {
    let manifest_dir = PathBuf::from(
        std::env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR is set by Cargo"),
    );
    let header_path = manifest_dir.join("include/lance/lance.h");
    println!("cargo::rerun-if-changed={}", header_path.display());

    let header = fs::read_to_string(&header_path)
        .unwrap_or_else(|error| panic!("failed to read {}: {error}", header_path.display()));
    let exports = public_c_names(&header);
    if exports.is_empty() {
        panic!(
            "no public lance_* declarations found in {}",
            header_path.display()
        );
    }

    let out_dir = PathBuf::from(std::env::var("OUT_DIR").expect("OUT_DIR is set by Cargo"));
    let definition_path = out_dir.join("lance_c.def");
    let mut definition = String::from("LIBRARY lance_c\nEXPORTS\n");
    for export in &exports {
        writeln!(definition, "  {export}").expect("writing a DEF entry to a String cannot fail");
    }
    fs::write(&definition_path, definition).unwrap_or_else(|error| {
        panic!(
            "failed to write MSVC export definition {}: {error}",
            definition_path.display()
        )
    });

    // This is a post-link argument, after rustc's generated `/DEF`.
    println!(
        "cargo::rustc-cdylib-link-arg=/DEF:{}",
        definition_path.display()
    );
}

fn public_c_names(header: &str) -> BTreeSet<String> {
    let header = strip_c_comments(header);
    let bytes = header.as_bytes();
    let prefix = b"lance_";
    let mut names = BTreeSet::new();
    let mut index = 0;

    while index + prefix.len() < bytes.len() {
        let starts_name = bytes[index..].starts_with(prefix)
            && (index == 0 || !is_identifier_byte(bytes[index - 1]));
        if !starts_name {
            index += 1;
            continue;
        }

        let start = index;
        index += prefix.len();
        while index < bytes.len() && is_identifier_byte(bytes[index]) {
            index += 1;
        }
        let end = index;
        let mut next = index;
        while next < bytes.len() && bytes[next].is_ascii_whitespace() {
            next += 1;
        }
        if end > start + prefix.len() && next < bytes.len() && bytes[next] == b'(' {
            names.insert(header[start..end].to_string());
        }
    }

    names
}

fn is_identifier_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'_'
}

fn strip_c_comments(source: &str) -> String {
    let bytes = source.as_bytes();
    let mut result = Vec::with_capacity(bytes.len());
    let mut index = 0;

    while index < bytes.len() {
        if bytes[index..].starts_with(b"//") {
            result.push(b' ');
            index += 2;
            while index < bytes.len() && bytes[index] != b'\n' {
                index += 1;
            }
        } else if bytes[index..].starts_with(b"/*") {
            result.push(b' ');
            index += 2;
            while index + 1 < bytes.len() && !bytes[index..].starts_with(b"*/") {
                if bytes[index] == b'\n' {
                    result.push(b'\n');
                }
                index += 1;
            }
            index = (index + 2).min(bytes.len());
        } else {
            result.push(bytes[index]);
            index += 1;
        }
    }

    String::from_utf8(result).expect("include/lance/lance.h must be valid UTF-8")
}
