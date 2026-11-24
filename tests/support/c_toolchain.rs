// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Compiler selection and invocation for the native C/C++ FFI tests.
//!
//! Linux and macOS use a GNU-style Clang driver. The MSVC targets use `cl.exe`
//! (or any MSVC-compatible compiler such as `clang-cl`) and link the
//! `lance_c.dll.lib` import library explicitly.
//!
//! `CC` and `CXX` override the compiler on Linux and macOS. The MSVC targets
//! always use the target-aware toolchain that `build.rs` resolves, so the
//! harness never picks a `cl.exe` from `PATH` that targets another
//! architecture.

use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::Command;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Language {
    C,
    Cpp,
}

/// One compiler invocation.
///
/// `output` set means compile and link a test binary. `output` unset means a
/// syntax-only check that writes no binary and links nothing.
pub(crate) struct Compilation<'a> {
    pub(crate) language: Language,
    pub(crate) sources: &'a [&'a Path],
    pub(crate) includes: &'a [&'a Path],
    pub(crate) output: Option<&'a Path>,
    /// Shared library to link. The Windows import library is derived from it.
    pub(crate) shared_library: Option<&'a Path>,
    /// Turn compiler warnings into errors.
    pub(crate) deny_warnings: bool,
}

impl Compilation<'_> {
    fn writes_binary(&self) -> bool {
        self.output.is_some()
    }
}

/// Compile, link, or syntax-check a native consumer.
///
/// Returns whether the compiler reported success. Compiler diagnostics go to
/// the test's stderr, so a failure is visible in the CI log.
pub(crate) fn compile(compilation: &Compilation<'_>) -> bool {
    let mut command = compiler_command(compilation.language);
    let mut arguments = compiler_arguments(compilation);
    arguments.extend(sanitizer_arguments(
        std::env::var("LANCE_C_SANITIZERS").ok().as_deref(),
    ));
    command.args(&arguments);

    let status = command.status().unwrap_or_else(|error| {
        panic!(
            "failed to run the C/C++ compiler for {:?}: {error}\ncommand: {command:?}",
            compilation.language
        )
    });
    status.success()
}

/// Extra flags requested through `LANCE_C_SANITIZERS` (for example
/// `address,undefined`).
///
/// The Rust library itself stays uninstrumented; these flags instrument the
/// native consumer that crosses the FFI boundary, so heap and lifetime errors
/// in C/C++ ownership code are reported. MSVC has no equivalent flag set and
/// is left untouched.
fn sanitizer_arguments(sanitizers: Option<&str>) -> Vec<OsString> {
    let Some(sanitizers) = sanitizers else {
        return Vec::new();
    };
    if cfg!(windows) {
        return Vec::new();
    }

    vec![
        format!("-fsanitize={sanitizers}").into(),
        "-fno-sanitize-recover=all".into(),
        "-fno-omit-frame-pointer".into(),
        "-g".into(),
    ]
}

/// Path of a compiled test binary. MSVC appends `.exe`; the harness passes the
/// same path to `Command`, so it must name the file exactly.
pub(crate) fn executable_path(directory: &Path, stem: &str) -> PathBuf {
    if cfg!(windows) {
        directory.join(format!("{stem}.exe"))
    } else {
        directory.join(stem)
    }
}

/// Import library that MSVC links against for `lance_c.dll`.
///
/// Rust emits `lance_c.dll` together with `lance_c.dll.lib`, so the import
/// library is the DLL file name plus `.lib`.
pub(crate) fn import_library(shared_library: &Path) -> PathBuf {
    let file_name = shared_library
        .file_name()
        .unwrap_or_else(|| panic!("shared library path has no file name: {shared_library:?}"));
    shared_library.with_file_name(format!("{}.lib", file_name.to_string_lossy()))
}

/// Copy the shared library beside a compiled test binary.
///
/// The Windows loader finds `lance_c.dll` next to the executable without
/// depending on `PATH`, which matches how the installed-package checks run.
/// Other platforms load the library through the rpath the compiler recorded.
pub(crate) fn install_runtime_library(binary: &Path, shared_library: &Path) {
    #[cfg(windows)]
    {
        let file_name = shared_library
            .file_name()
            .unwrap_or_else(|| panic!("shared library path has no file name: {shared_library:?}"));
        let destination = binary.with_file_name(file_name);
        std::fs::copy(shared_library, &destination).unwrap_or_else(|error| {
            panic!(
                "failed to copy {} to {}: {error}",
                shared_library.display(),
                destination.display()
            )
        });
    }
    #[cfg(not(windows))]
    let _ = (binary, shared_library);
}

fn compiler_command(language: Language) -> Command {
    let variable = match language {
        Language::C => "CC",
        Language::Cpp => "CXX",
    };

    #[cfg(windows)]
    {
        let _ = variable;
        msvc_compiler_command()
    }
    #[cfg(not(windows))]
    {
        let default = match language {
            Language::C => "clang",
            Language::Cpp => "c++",
        };
        let tool = std::env::var_os(variable).unwrap_or_else(|| default.into());
        Command::new(tool)
    }
}

fn compiler_arguments(compilation: &Compilation<'_>) -> Vec<OsString> {
    #[cfg(windows)]
    {
        msvc_arguments(compilation)
    }
    #[cfg(not(windows))]
    {
        gnu_arguments(compilation)
    }
}

/// GNU-style Clang arguments shared by Linux and macOS.
fn gnu_arguments(compilation: &Compilation<'_>) -> Vec<OsString> {
    let mut arguments: Vec<OsString> = Vec::new();
    arguments.push(
        match compilation.language {
            Language::C => "-std=c11",
            Language::Cpp => "-std=c++20",
        }
        .into(),
    );
    arguments.push("-Wall".into());
    arguments.push("-Wextra".into());
    if compilation.deny_warnings {
        arguments.push("-Werror".into());
    }
    if compilation.language == Language::Cpp {
        arguments.push("-pthread".into());
    }

    if compilation.writes_binary() {
        let output = compilation.output.expect("checked by writes_binary");
        arguments.push("-o".into());
        arguments.push(output.into());
    } else {
        arguments.push("-fsyntax-only".into());
    }

    for include in compilation.includes {
        arguments.push(format!("-I{}", include.display()).into());
    }
    arguments.extend(compilation.sources.iter().map(|source| source.into()));

    if let Some(shared_library) = compilation.shared_library
        && compilation.writes_binary()
    {
        let library_dir = shared_library
            .parent()
            .unwrap_or_else(|| panic!("shared library path has no parent: {shared_library:?}"));
        arguments.push(format!("-L{}", library_dir.display()).into());
        arguments.push("-llance_c".into());
        // macOS has no default search path for the build tree; recording the
        // rpath also keeps local runs working on Linux.
        arguments.push(format!("-Wl,-rpath,{}", library_dir.display()).into());
    }

    arguments
}

/// MSVC arguments. `clang-cl` accepts the same switches.
fn msvc_arguments(compilation: &Compilation<'_>) -> Vec<OsString> {
    let mut arguments: Vec<OsString> = Vec::new();
    arguments.push("/nologo".into());
    arguments.push(
        match compilation.language {
            Language::C => "/std:c11",
            Language::Cpp => "/std:c++20",
        }
        .into(),
    );
    match compilation.language {
        Language::C => {
            // MSVC exposes C11 <stdatomic.h> behind this switch; the native
            // consumers rely on the lock-free integer atomics it provides.
            arguments.push("/experimental:c11atomics".into());
        }
        Language::Cpp => {
            // The C++ wrapper throws exceptions for every non-zero error code.
            arguments.push("/EHsc".into());
        }
    }
    arguments.push("/W4".into());
    if compilation.deny_warnings {
        arguments.push("/WX".into());
    }
    // Keep sources with UTF-8 comments compiling under the default code page.
    arguments.push("/utf-8".into());
    // The consumers use POSIX names (`strdup`, `stat`) and CRT functions that
    // MSVC flags as insecure. They are supported; only the aliases are noisy.
    arguments.push("/D_CRT_SECURE_NO_WARNINGS".into());
    arguments.push("/D_CRT_NONSTDC_NO_WARNINGS".into());

    for include in compilation.includes {
        arguments.push(format!("/I{}", include.display()).into());
    }
    arguments.extend(compilation.sources.iter().map(|source| source.into()));

    match compilation.output {
        None => arguments.push("/Zs".into()),
        Some(output) => {
            let build_dir = output
                .parent()
                .unwrap_or_else(|| panic!("test binary path has no parent: {output:?}"));
            arguments.push(format!("/Fe:{}", output.display()).into());
            // Keep intermediate objects in the harness build directory.
            arguments.push(format!("/Fo{}\\", build_dir.display()).into());
            if let Some(shared_library) = compilation.shared_library {
                arguments.push("/link".into());
                arguments.push(import_library(shared_library).into());
            }
        }
    }

    arguments
}

/// Compiler command for the MSVC targets.
///
/// `cc::windows_registry` resolves the Visual Studio installation that matches
/// the target architecture and returns the developer environment with it, the
/// same lookup `build.rs` uses for `lib.exe`. That avoids a `cl.exe` picked up
/// from `PATH`, which can target a different architecture than the library the
/// harness links against.
#[cfg(windows)]
fn msvc_compiler_command() -> Command {
    let target = format!("{}-pc-windows-msvc", std::env::consts::ARCH);

    let tool = cc::windows_registry::find_tool(&target, "cl.exe").unwrap_or_else(|| {
        panic!(
            "MSVC cl.exe was not found for {target}; install the Visual Studio C++ \
             toolset for this architecture"
        )
    });
    tool.to_command()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn executable_path_names_the_binary_the_host_runs() {
        let path = executable_path(Path::new("build"), "test_c_api");
        let expected_name = if cfg!(windows) {
            "test_c_api.exe"
        } else {
            "test_c_api"
        };

        assert_eq!(path.parent(), Some(Path::new("build")));
        assert_eq!(path.file_name().unwrap(), expected_name);
    }

    #[test]
    fn import_library_appends_the_msvc_suffix() {
        let import = import_library(Path::new("build/lance_c.dll"));

        assert_eq!(import.parent(), Some(Path::new("build")));
        assert_eq!(import.file_name().unwrap(), "lance_c.dll.lib");
    }

    fn as_strings(arguments: Vec<OsString>) -> Vec<String> {
        arguments
            .into_iter()
            .map(|argument| argument.to_string_lossy().into_owned())
            .collect()
    }

    fn source(language: Language) -> &'static Path {
        match language {
            Language::C => Path::new("src/test_c_api.c"),
            Language::Cpp => Path::new("src/test_cpp_api.cpp"),
        }
    }

    /// GNU-style invocation for a build under `build/`, mirroring Linux and
    /// macOS.
    fn reference_gnu(language: Language, links: bool, deny_warnings: bool) -> Vec<String> {
        let source = source(language);
        let compilation = Compilation {
            language,
            sources: &[source],
            includes: &[Path::new("include")],
            output: links.then(|| Path::new("build/test_c_api")),
            shared_library: Some(Path::new("build/liblance_c.so")),
            deny_warnings,
        };
        as_strings(gnu_arguments(&compilation))
    }

    /// MSVC invocation for the same build layout.
    fn reference_msvc(language: Language, links: bool, deny_warnings: bool) -> Vec<String> {
        let source = source(language);
        let compilation = Compilation {
            language,
            sources: &[source],
            includes: &[Path::new("include")],
            output: links.then(|| Path::new("build/test_c_api.exe")),
            shared_library: Some(Path::new("build/lance_c.dll")),
            deny_warnings,
        };
        as_strings(msvc_arguments(&compilation))
    }

    #[test]
    fn gnu_arguments_link_the_build_tree_library() {
        let arguments = reference_gnu(Language::C, true, false);

        assert!(arguments.contains(&"-std=c11".to_string()));
        assert!(arguments.contains(&"-o".to_string()));
        assert!(arguments.contains(&"build/test_c_api".to_string()));
        assert!(arguments.contains(&"-Iinclude".to_string()));
        assert!(arguments.contains(&"src/test_c_api.c".to_string()));
        assert!(arguments.contains(&"-Lbuild".to_string()));
        assert!(arguments.contains(&"-llance_c".to_string()));
        assert!(!arguments.contains(&"-fsyntax-only".to_string()));
    }

    #[test]
    fn gnu_syntax_check_writes_nothing_and_links_nothing() {
        let arguments = reference_gnu(Language::C, false, false);

        assert!(arguments.contains(&"-fsyntax-only".to_string()));
        assert!(!arguments.contains(&"-o".to_string()));
        assert!(!arguments.iter().any(|argument| argument == "-llance_c"));
    }

    #[test]
    fn gnu_cpp_arguments_select_cpp20() {
        let arguments = reference_gnu(Language::Cpp, true, false);

        assert!(arguments.contains(&"-std=c++20".to_string()));
        assert!(arguments.contains(&"-pthread".to_string()));
    }

    #[test]
    fn gnu_denied_warnings_add_werror() {
        let denied = reference_gnu(Language::C, true, true);
        assert!(denied.iter().any(|argument| argument == "-Werror"));

        let allowed = reference_gnu(Language::C, true, false);
        assert!(!allowed.iter().any(|argument| argument == "-Werror"));
    }

    #[test]
    fn sanitizer_arguments_are_empty_when_not_requested() {
        assert!(sanitizer_arguments(None).is_empty());
    }

    #[test]
    fn sanitizer_arguments_instrument_gnu_consumers() {
        let arguments = as_strings(sanitizer_arguments(Some("address,undefined")));

        if cfg!(windows) {
            assert!(arguments.is_empty());
            return;
        }
        assert!(arguments.contains(&"-fsanitize=address,undefined".to_string()));
        assert!(arguments.contains(&"-fno-sanitize-recover=all".to_string()));
        assert!(arguments.contains(&"-fno-omit-frame-pointer".to_string()));
        assert!(arguments.contains(&"-g".to_string()));
    }

    #[test]
    fn msvc_arguments_link_the_import_library() {
        let arguments = reference_msvc(Language::C, true, false);

        assert!(arguments.contains(&"/std:c11".to_string()));
        assert!(arguments.contains(&"/experimental:c11atomics".to_string()));
        assert!(arguments.contains(&"/W4".to_string()));
        assert!(arguments.contains(&"/Iinclude".to_string()));
        assert!(arguments.contains(&"/Fe:build/test_c_api.exe".to_string()));
        assert!(arguments.contains(&"/Fobuild\\".to_string()));
        assert!(arguments.contains(&"/link".to_string()));
        // `with_file_name` inserts the host separator before the new name.
        assert!(
            arguments
                .iter()
                .any(|argument| argument.ends_with("lance_c.dll.lib"))
        );
        assert!(!arguments.contains(&"/Zs".to_string()));
    }

    #[test]
    fn msvc_syntax_check_uses_syntax_only_mode() {
        let arguments = reference_msvc(Language::C, false, false);

        assert!(arguments.contains(&"/Zs".to_string()));
        assert!(!arguments.contains(&"/link".to_string()));
        assert!(
            !arguments
                .iter()
                .any(|argument| argument.starts_with("/Fe:"))
        );
    }

    #[test]
    fn msvc_denied_warnings_add_wx() {
        let denied = reference_msvc(Language::C, true, true);
        assert!(denied.iter().any(|argument| argument == "/WX"));

        let allowed = reference_msvc(Language::C, true, false);
        assert!(!allowed.iter().any(|argument| argument == "/WX"));
    }

    #[test]
    fn msvc_cpp_arguments_select_cpp20_and_exceptions() {
        let arguments = reference_msvc(Language::Cpp, true, false);

        assert!(arguments.iter().any(|argument| argument == "/std:c++20"));
        assert!(arguments.iter().any(|argument| argument == "/EHsc"));
        assert!(
            !arguments
                .iter()
                .any(|argument| argument == "/experimental:c11atomics")
        );
    }
}
