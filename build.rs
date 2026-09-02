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
        }
        "macos" | "ios" => {
            println!("cargo::rustc-cdylib-link-arg=-Wl,-install_name,@rpath/liblance_c.dylib");
        }
        _ => {}
    }
}
