// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Owned callback values shared by callback-based C APIs.
//!
//! Foreign callbacks return strings through [`LanceOwnedString`] and report
//! failures through [`LanceCallbackError`]. The library copies every borrowed
//! value before invoking its release hook, so callback-owned storage is never
//! read after it has been released or after the callback has returned.

use std::ffi::{CStr, c_char};
use std::ptr;

use lance_core::{Error, Result};

/// Owned, optional UTF-8 string produced by a foreign callback.
#[repr(C)]
pub struct LanceOwnedString {
    /// NUL-terminated string data, or NULL when absent.
    pub data: *mut c_char,
    /// Releases `data`. Required whenever `data` is non-NULL.
    pub release: Option<unsafe extern "C" fn(*mut LanceOwnedString)>,
}

impl LanceOwnedString {
    pub(crate) fn empty() -> Self {
        Self {
            data: ptr::null_mut(),
            release: None,
        }
    }
}

/// Structured failure returned by a foreign callback.
#[repr(C)]
pub struct LanceCallbackError {
    /// A nonzero `LanceErrorCode` value.
    pub code: i32,
    /// Callback-owned message copied before `release` is invoked.
    pub message: LanceOwnedString,
}

impl LanceCallbackError {
    pub(crate) fn empty() -> Self {
        Self {
            code: 0,
            message: LanceOwnedString::empty(),
        }
    }
}

pub(crate) struct CallbackString {
    text: Option<String>,
    invalid_utf8: bool,
}

impl CallbackString {
    pub(crate) fn into_display_string(self, fallback: &str) -> String {
        if self.invalid_utf8 {
            return format!("{fallback}: callback message is not valid UTF-8");
        }
        self.text.unwrap_or_else(|| fallback.to_owned())
    }

    pub(crate) fn into_string(self, fallback: &str) -> Result<String> {
        if self.invalid_utf8 {
            return Err(Error::invalid_input(format!(
                "{fallback}: message is not valid UTF-8"
            )));
        }
        Ok(self.text.unwrap_or_else(|| fallback.to_owned()))
    }
}

pub(crate) unsafe fn take_owned_string(value: &mut LanceOwnedString) -> CallbackString {
    let mut text = None;
    let mut invalid_utf8 = false;
    if !value.data.is_null() {
        // SAFETY: a callback-provided string must be NUL-terminated while its
        // release callback is still live.
        match unsafe { CStr::from_ptr(value.data) }.to_str() {
            Ok(value) => text = Some(value.to_owned()),
            Err(_) => invalid_utf8 = true,
        }
    }
    unsafe { release_owned_string_slot(value) };
    CallbackString { text, invalid_utf8 }
}

pub(crate) unsafe fn release_message_only(error: &mut LanceCallbackError) {
    unsafe { release_owned_string_slot(&mut error.message) };
}

pub(crate) unsafe fn release_owned_string_slot(value: &mut LanceOwnedString) {
    if let Some(release) = value.release.take() {
        unsafe { release(value) };
    }
    value.data = ptr::null_mut();
}

pub(crate) unsafe fn callback_error(
    error: &mut LanceCallbackError,
    status: i32,
    context: &str,
) -> Error {
    let code = error.code;
    let message = unsafe { take_owned_string(&mut error.message) }.into_display_string(&format!(
        "{context} callback returned error status {status} without a message"
    ));

    match code {
        1 => Error::invalid_input(message),
        2 => Error::io(message),
        3 => Error::not_found(message),
        6 => Error::internal(message),
        7 => Error::not_supported(message),
        other => Error::internal(format!(
            "{context} callback returned unsupported error code {other} (status {status}): {message}"
        )),
    }
}
