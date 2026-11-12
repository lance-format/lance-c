#[unsafe(no_mangle)]
pub extern "C" fn lance_api() {
    unsafe { aws_lc_helper() };
}

#[unsafe(no_mangle)]
pub extern "C" fn crc_fast_dependency() {}

unsafe extern "C" {
    fn aws_lc_helper();
}
