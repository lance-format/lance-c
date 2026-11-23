// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! `file://` URIs for local test paths.
//!
//! The native consumers receive these URIs from the Rust harness instead of
//! assembling them from paths, so the conversion and its escaping live in one
//! place. `Url::from_file_path` produces the drive-letter form on Windows and
//! percent-encodes characters that would otherwise change meaning when the URI
//! travels through JSON or a substring assertion.

use std::path::Path;

/// `file://` URI for an absolute local path.
///
/// A Unix path becomes `file:///tmp/data`; a Windows path becomes
/// `file:///C:/Temp/data`. `#`, `%`, spaces, and a literal backslash inside a
/// Unix path are percent-encoded rather than rewritten.
pub fn file_uri(path: &Path) -> String {
    url::Url::from_file_path(path)
        .unwrap_or_else(|()| panic!("test path must be absolute: {}", path.display()))
        .to_string()
}
