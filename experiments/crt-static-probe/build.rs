// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

fn main() {
    let mut build = cc::Build::new();
    build.file("native.c").compile("crt_probe_native");

    let features = std::env::var("CARGO_CFG_TARGET_FEATURE").unwrap_or_default();
    println!("cargo:warning=crt probe target features: {features}");
}
