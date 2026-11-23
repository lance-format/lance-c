// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! `file://` URI construction for local test paths.

use std::path::Path;

/// `file://` URI for a local filesystem path.
///
/// A Unix path already starts with `/`, so the URI reads `file:///tmp/data`.
/// A Windows path starts with a drive letter: `file://C:\Temp` would parse as
/// host `C` with port `\Temp`, so the drive-letter form `file:///C:/Temp` is
/// required instead.
pub fn file_uri(path: &Path) -> String {
    let path = path
        .to_str()
        .unwrap_or_else(|| panic!("test path is not valid UTF-8: {}", path.display()))
        .replace('\\', "/");
    if path.starts_with('/') {
        format!("file://{path}")
    } else {
        format!("file:///{path}")
    }
}
