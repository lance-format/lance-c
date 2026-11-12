use std::env;
use std::fs;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-changed=dependency.c");
    cc::Build::new().file("dependency.c").compile("dependency");

    let target = env::var("TARGET").expect("TARGET");
    if target != "x86_64-pc-windows-msvc" && target != "aarch64-pc-windows-msvc" {
        return;
    }

    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let definition = out_dir.join("lance_c.def");
    fs::write(
        &definition,
        "LIBRARY lance_c\n; lance-c exact public exports\nEXPORTS\n  lance_api\n",
    )
    .expect("write probe DEF");

    let machine = if target.starts_with("aarch64") {
        "ARM64"
    } else {
        "X64"
    };
    let import_library = out_dir.join("lance_c.dll.lib");
    let export_file = out_dir.join("lance_c.dll.exp");
    let tool = cc::windows_registry::find_tool(&target, "lib.exe")
        .unwrap_or_else(|| panic!("lib.exe not found for {target}"));
    let output = tool
        .to_command()
        .arg("/NOLOGO")
        .arg(format!("/MACHINE:{machine}"))
        .arg(format!("/DEF:{}", definition.display()))
        .arg(format!("/OUT:{}", import_library.display()))
        .output()
        .expect("run lib.exe");
    if !output.status.success() {
        panic!(
            "lib.exe failed: stdout={} stderr={}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
    }
    for artifact in [&import_library, &export_file] {
        let metadata = fs::metadata(artifact)
            .unwrap_or_else(|error| panic!("missing {}: {error}", artifact.display()));
        assert!(metadata.len() > 0, "{} is empty", artifact.display());
    }

    println!("cargo::rustc-cdylib-link-arg=/DEF:{}", definition.display());
}
