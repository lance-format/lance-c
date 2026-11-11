// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Dataset-free FTS tokenization and process-wide diagnostic counters.
//!
//! These entry points expose the native tokenizer and native CPU/process
//! diagnostics directly. They deliberately do not add a binding-side
//! tokenizer, CPU detection, or counter cache.

use std::ffi::c_char;
use std::ptr;

use lance::io::{bytes_read_counter, iops_counter};
use lance_core::utils::cpu::simd_info;
use lance_core::{Error, Result};
use lance_index::scalar::InvertedIndexParams;
use lance_index::scalar::inverted::query::collect_query_tokens;

use crate::error::{ffi_try, swallow_unwind};
use crate::helpers;
use crate::metadata::owned_c_string;

/// One owned token produced by the native analyzer chain.
#[derive(Clone, Debug, PartialEq, Eq)]
struct DiagnosticToken {
    text: String,
    position: u32,
}

/// Opaque, owned result of `lance_fts_tokenize`.
pub struct LanceFtsTokenList {
    tokens: Vec<DiagnosticToken>,
}

/// Borrowed view of one token in a [`LanceFtsTokenList`].
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LanceFtsTokenView {
    /// Token bytes. Not NUL-terminated; use `text_len`.
    pub text: *const c_char,
    /// Token byte length, including any embedded NUL bytes.
    pub text_len: usize,
    /// Native normalized token position.
    pub position: u32,
}

/// Tokenize text with the native Lance inverted-index analyzer.
///
/// `text` is an explicit UTF-8 byte range and may contain embedded NUL bytes.
/// NULL is accepted only when `text_len == 0` (empty text). `params_json` is a
/// NULL-terminated JSON document matching native `InvertedIndexParams`; NULL
/// selects `InvertedIndexParams::default()`. Deserialization, defaults,
/// optional-field presence, validation, and analyzer errors are native.
///
/// Returns an owned token list, or NULL on error. Free it with
/// `lance_fts_token_list_close`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_tokenize(
    text: *const c_char,
    text_len: usize,
    params_json: *const c_char,
) -> *mut LanceFtsTokenList {
    ffi_try!(
        unsafe { fts_tokenize_inner(text, text_len, params_json) },
        null
    )
}

unsafe fn fts_tokenize_inner(
    text: *const c_char,
    text_len: usize,
    params_json: *const c_char,
) -> Result<*mut LanceFtsTokenList> {
    let text = unsafe { helpers::parse_utf8_text(text, text_len, "text") }?;
    let params = match unsafe { helpers::parse_c_string(params_json) }? {
        Some(json) => serde_json::from_str::<InvertedIndexParams>(json).map_err(|error| {
            Error::invalid_input(format!("invalid FTS tokenizer parameters: {error}"))
        })?,
        None => InvertedIndexParams::default(),
    };

    let mut tokenizer = params.build()?;
    let tokens = collect_query_tokens(text, &mut tokenizer);
    let tokens = (0..tokens.len())
        .map(|index| DiagnosticToken {
            text: tokens.get_token(index).to_owned(),
            position: tokens.position(index),
        })
        .collect();
    Ok(Box::into_raw(Box::new(LanceFtsTokenList { tokens })))
}

/// Return the number of tokens in the list.
///
/// Returns 0 with `LANCE_ERR_INVALID_ARGUMENT` when `tokens` is NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_token_list_count(tokens: *const LanceFtsTokenList) -> u64 {
    ffi_try!(unsafe { fts_token_list_count_inner(tokens) }, 0)
}

unsafe fn fts_token_list_count_inner(tokens: *const LanceFtsTokenList) -> Result<u64> {
    if tokens.is_null() {
        return Err(Error::invalid_input_source(
            "token list must not be NULL".into(),
        ));
    }
    let tokens = unsafe { &*tokens };
    Ok(tokens.tokens.len() as u64)
}

/// Borrow one token view from the list.
///
/// The returned pointers remain valid until `lance_fts_token_list_close`.
/// Returns 0 on success or -1 on a NULL handle/out pointer or out-of-range
/// index. `out` is written only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_token_list_get(
    tokens: *const LanceFtsTokenList,
    index: usize,
    out: *mut LanceFtsTokenView,
) -> i32 {
    ffi_try!(unsafe { fts_token_list_get_inner(tokens, index, out) }, neg)
}

unsafe fn fts_token_list_get_inner(
    tokens: *const LanceFtsTokenList,
    index: usize,
    out: *mut LanceFtsTokenView,
) -> Result<i32> {
    if tokens.is_null() || out.is_null() {
        return Err(Error::invalid_input_source(
            "token list and out must not be NULL".into(),
        ));
    }
    let tokens = unsafe { &*tokens };
    let token = tokens.tokens.get(index).ok_or_else(|| {
        Error::invalid_input(format!(
            "token index {index} is out of bounds for {} tokens",
            tokens.tokens.len()
        ))
    })?;
    unsafe {
        ptr::write(
            out,
            LanceFtsTokenView {
                text: token.text.as_ptr().cast::<c_char>(),
                text_len: token.text.len(),
                position: token.position,
            },
        );
    }
    Ok(0)
}

/// Close and free a token list. Safe to call with NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lance_fts_token_list_close(tokens: *mut LanceFtsTokenList) {
    if !tokens.is_null() {
        swallow_unwind("lance_fts_token_list_close", || unsafe {
            let _ = Box::from_raw(tokens);
        });
    }
}

/// Native SIMD tier, target architecture, and host feature names as JSON.
///
/// The object has exactly `tier` (native `Display` string), `target_arch`, and
/// `host_features` (native order). The returned string is owned by the caller
/// and must be freed with `lance_free_string`.
#[unsafe(no_mangle)]
pub extern "C" fn lance_simd_info_json() -> *const c_char {
    ffi_try!(simd_info_json_inner(), null)
}

#[derive(serde::Serialize)]
struct SimdInfoJson<'a> {
    tier: String,
    target_arch: &'a str,
    host_features: &'a [&'a str],
}

fn simd_info_json_inner() -> Result<*const c_char> {
    let info = simd_info();
    let report = SimdInfoJson {
        tier: info.tier.to_string(),
        target_arch: info.target_arch,
        host_features: &info.host_features,
    };
    let json = serde_json::to_string(&report).map_err(|error| {
        Error::internal(format!("failed to serialize native SIMD info: {error}"))
    })?;
    owned_c_string(json, "native SIMD info JSON")
}

/// Native process-wide object-store IOPS counter.
#[unsafe(no_mangle)]
pub extern "C" fn lance_process_iops_counter() -> u64 {
    ffi_try!(process_iops_counter_inner(), 0)
}

fn process_iops_counter_inner() -> Result<u64> {
    Ok(iops_counter())
}

/// Native process-wide object-store bytes-read counter.
#[unsafe(no_mangle)]
pub extern "C" fn lance_process_bytes_read_counter() -> u64 {
    ffi_try!(process_bytes_read_counter_inner(), 0)
}

fn process_bytes_read_counter_inner() -> Result<u64> {
    Ok(bytes_read_counter())
}
