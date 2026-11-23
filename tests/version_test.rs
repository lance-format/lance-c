// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Protect the public version identity against drift from `Cargo.toml`.
//!
//! The version header is hand-maintained, so a package version bump can otherwise
//! leave compile-time macros and the runtime query disagreeing. The release
//! workflow updates the header, and these tests fail if that step is skipped
//! or produces a different version.

use std::ffi::CStr;
use std::path::PathBuf;

use lance_c::lance_c_version;

fn header_macro(header: &str, name: &str) -> String {
    let mut values = header.lines().filter_map(|line| {
        let rest = line.trim().strip_prefix("#define ")?;
        let (defined_name, value) = rest.split_once(char::is_whitespace)?;
        (defined_name == name).then(|| value.trim().to_owned())
    });
    let value = values
        .next()
        .unwrap_or_else(|| panic!("missing #define {name} in include/lance/version.h"));
    assert!(
        values.next().is_none(),
        "duplicate #define {name} in include/lance/version.h"
    );
    value
}

fn package_version_parts(version: &str) -> (&str, &str) {
    let (without_build, _) = version.split_once('+').unwrap_or((version, ""));
    without_build.split_once('-').unwrap_or((without_build, ""))
}

#[test]
fn header_version_matches_cargo_package_version() {
    let header_path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("include/lance/version.h");
    let header = std::fs::read_to_string(&header_path)
        .unwrap_or_else(|error| panic!("failed to read {}: {error}", header_path.display()));

    let version = env!("CARGO_PKG_VERSION");
    let (base, pre) = package_version_parts(version);
    let mut base_parts = base.split('.');
    let major = base_parts.next().expect("major version");
    let minor = base_parts.next().expect("minor version");
    let patch = base_parts.next().expect("patch version");
    assert!(
        base_parts.next().is_none(),
        "Cargo package version {version} must have three numeric components"
    );

    assert_eq!(header_macro(&header, "LANCE_C_VERSION_MAJOR"), major);
    assert_eq!(header_macro(&header, "LANCE_C_VERSION_MINOR"), minor);
    assert_eq!(header_macro(&header, "LANCE_C_VERSION_PATCH"), patch);
    assert_eq!(
        header_macro(&header, "LANCE_C_VERSION_PRE"),
        format!("\"{pre}\"")
    );
    assert_eq!(
        header_macro(&header, "LANCE_C_VERSION_STRING"),
        format!("\"{version}\"")
    );
}

#[test]
fn runtime_version_is_the_cargo_package_version() {
    let runtime = unsafe { CStr::from_ptr(lance_c_version()) }
        .to_str()
        .expect("lance_c_version must return UTF-8");
    assert_eq!(runtime, env!("CARGO_PKG_VERSION"));
    assert_eq!(
        lance_c_version(),
        lance_c_version(),
        "lance_c_version must return process-lifetime static storage"
    );
}
