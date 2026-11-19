// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Binding release version reported to C and C++ callers.

use std::ffi::c_char;

/// The Cargo package version baked into this build.
///
/// Keeping the trailing NUL in static storage makes the returned pointer valid
/// for the process lifetime without an allocation on each call.
static VERSION: &[u8] = concat!(env!("CARGO_PKG_VERSION"), "\0").as_bytes();

/// Return the binding's package version as a NUL-terminated string.
///
/// The string includes any Cargo pre-release suffix and is owned by the
/// library. It remains valid for the process lifetime and must not be freed.
/// This is the lance-c release, not the Lance core writer version or a Git pin.
#[unsafe(no_mangle)]
pub extern "C" fn lance_c_version() -> *const c_char {
    VERSION.as_ptr().cast()
}
