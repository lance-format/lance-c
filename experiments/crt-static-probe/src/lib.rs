// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

use std::ffi::{CStr, CString, c_char};
use std::panic::{AssertUnwindSafe, catch_unwind};

unsafe extern "C" {
    static probe_native_static_crt: i32;
}

#[unsafe(no_mangle)]
pub extern "C" fn probe_rust_crt_style() -> i32 {
    i32::from(cfg!(target_feature = "crt-static"))
}

#[unsafe(no_mangle)]
pub extern "C" fn probe_native_crt_style() -> i32 {
    unsafe { probe_native_static_crt }
}

#[unsafe(no_mangle)]
pub extern "C" fn probe_alloc_string() -> *mut c_char {
    CString::new("probe-owned").unwrap().into_raw()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn probe_free_string(value: *mut c_char) {
    if !value.is_null() {
        drop(unsafe { CString::from_raw(value) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn probe_copy_string(value: *const c_char) -> *mut c_char {
    if value.is_null() {
        return std::ptr::null_mut();
    }
    let copied = unsafe { CStr::from_ptr(value) }
        .to_string_lossy()
        .into_owned();
    CString::new(copied).unwrap().into_raw()
}

#[unsafe(no_mangle)]
pub extern "C" fn probe_catch_panic() -> i32 {
    i32::from(catch_unwind(AssertUnwindSafe(|| panic!("probe panic"))).is_err())
}
