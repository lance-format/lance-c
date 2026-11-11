// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Build script for lance-c.
//!
//! Optionally generates `include/lance.h` via cbindgen.
//! If cbindgen is not available, the pre-committed header is used.

fn main() {
    // cbindgen header generation is optional.
    // Run `cargo install cbindgen && cbindgen --crate lance-c -o include/lance.h`
    // to regenerate the header manually.

    set_shared_library_install_name();
}

/// Give the `cdylib` an install name, which rustc does not set on its own.
fn set_shared_library_install_name() {
    // CARGO_CFG_TARGET_OS rather than cfg!(target_os), because a build script
    // is compiled for the host and these artifacts are cross-compiled.
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();

    match target_os.as_str() {
        "linux" | "android" => {
            println!("cargo::rustc-cdylib-link-arg=-Wl,-soname,liblance_c.so");
            // rustc exports every reachable `#[no_mangle]` symbol from the
            // statically linked dependency graph. `crc-fast` exposes an FFI
            // surface through its default features, so its symbols otherwise
            // appear in `liblance_c.so`. Hiding archive members keeps the
            // public `lance_*` ABI while preventing dependency symbols from
            // colliding with a `dlopen`-ing host.
            println!("cargo::rustc-cdylib-link-arg=-Wl,--exclude-libs,ALL");
        }
        "macos" | "ios" => {
            // ld64 unions rustc's generated `-exported_symbols_list` with any
            // list supplied here, so a link argument cannot remove dependency
            // exports. The release linker shim
            // (.github/scripts/limit-macos-exports.sh) rewrites rustc's list
            // for the `liblance_c.dylib` link instead.
            println!("cargo::rustc-cdylib-link-arg=-Wl,-install_name,@rpath/liblance_c.dylib");
        }
        _ => {}
    }
}
