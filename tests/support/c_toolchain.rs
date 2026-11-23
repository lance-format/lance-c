// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Compiler selection and invocation for the native C/C++ FFI tests.
//!
//! Linux and macOS use a GNU-style Clang driver. The MSVC targets use `cl.exe`
//! (or any MSVC-compatible compiler such as `clang-cl`) and link the
//! `lance_c.dll.lib` import library explicitly.
//!
//! `CC` and `CXX` override the compiler on every platform. On Windows they must
//! name an MSVC-compatible compiler; the harness passes `/`-style arguments
//! either way.

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
    let arguments = compiler_arguments(compilation);
    command.args(&arguments);

    let status = command.status().unwrap_or_else(|error| {
        panic!(
            "failed to run the C/C++ compiler for {:?}: {error}\ncommand: {command:?}",
            compilation.language
        )
    });
    status.success()
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
        let default = "cl.exe";
        let tool = std::env::var_os(variable).unwrap_or_else(|| default.into());
        let mut command = Command::new(tool);
        if let Some(environment) = msvc_environment() {
            command.envs(environment);
        }
        command
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

/// Environment from the Visual Studio developer prompt.
///
/// `cl.exe` is seldom on the default `PATH` outside a developer shell, so the
/// harness resolves the installation with `vswhere.exe` and reads the
/// environment that `vcvarsall.bat` would export. Returning `None` keeps the
/// inherited environment, which is correct inside a developer shell and when
/// `CC`/`CXX` point at a compiler that needs no MSVC environment.
#[cfg(windows)]
fn msvc_environment() -> Option<&'static std::collections::BTreeMap<OsString, OsString>> {
    use std::sync::OnceLock;

    static ENVIRONMENT: OnceLock<Option<std::collections::BTreeMap<OsString, OsString>>> =
        OnceLock::new();

    ENVIRONMENT
        .get_or_init(|| {
            if command_exists("cl.exe") {
                return None;
            }
            let installation = visual_studio_installation()?;
            vcvars_environment(&installation)
        })
        .as_ref()
}

#[cfg(windows)]
fn command_exists(name: &str) -> bool {
    Command::new("where.exe")
        .arg(name)
        .output()
        .map(|output| output.status.success())
        .unwrap_or(false)
}

#[cfg(windows)]
fn visual_studio_installation() -> Option<PathBuf> {
    let vswhere = ["ProgramFiles(x86)", "ProgramFiles"]
        .iter()
        .filter_map(|variable| std::env::var_os(variable))
        .map(|root| {
            PathBuf::from(root)
                .join("Microsoft Visual Studio")
                .join("Installer")
                .join("vswhere.exe")
        })
        .find(|candidate| candidate.is_file())?;

    let required_component = match std::env::consts::ARCH {
        "aarch64" => "Microsoft.VisualStudio.Component.VC.Tools.ARM64",
        _ => "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    };
    let output = Command::new(vswhere)
        .args([
            "-latest",
            "-products",
            "*",
            "-requires",
            required_component,
            "-property",
            "installationPath",
        ])
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }

    let installation = String::from_utf8(output.stdout).ok()?;
    let installation = installation.trim();
    if installation.is_empty() {
        return None;
    }
    Some(PathBuf::from(installation))
}

#[cfg(windows)]
fn vcvars_environment(
    installation: &Path,
) -> Option<std::collections::BTreeMap<OsString, OsString>> {
    let vcvars = installation
        .join("VC")
        .join("Auxiliary")
        .join("Build")
        .join("vcvarsall.bat");
    if !vcvars.is_file() {
        return None;
    }

    // `vcvarsall <arch>` picks the native host toolset; on Windows ARM64 the
    // Visual Studio layout can still be x64-hosted, so retry with the explicit
    // x64_arm64 form before giving up on the installation.
    let target = match std::env::consts::ARCH {
        "aarch64" => "arm64",
        _ => "x64",
    };
    let mut architectures = vec![target.to_owned()];
    if target == "arm64" {
        architectures.push("x64_arm64".to_owned());
    }

    // A batch file keeps the quoting for `cl.exe` out of the argument list:
    // cmd strips the outer quotes of a single quoted script path.
    let directory = tempfile::Builder::new()
        .prefix("lance-c-msvc")
        .tempdir()
        .ok()?;
    for architecture in architectures {
        let script = format!(
            "@echo off\r\n\
             call \"{}\" {architecture} >nul || exit /b 1\r\n\
             set\r\n",
            vcvars.display()
        );
        let script_path = directory.path().join("vcvars.cmd");
        std::fs::write(&script_path, script).ok()?;

        let output = Command::new("cmd.exe")
            .args(["/d", "/c"])
            .arg(&script_path)
            .output()
            .ok()?;
        if output.status.success() {
            return Some(parse_set_output(&output.stdout));
        }
    }
    None
}

/// Parse the `set` output exported by a developer prompt.
#[cfg(windows)]
fn parse_set_output(stdout: &[u8]) -> std::collections::BTreeMap<OsString, OsString> {
    String::from_utf8_lossy(stdout)
        .lines()
        .filter_map(|line| line.split_once('='))
        .filter(|(key, _)| !key.is_empty())
        .map(|(key, value)| (OsString::from(key), OsString::from(value)))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn executable_path_names_the_binary_the_platform_runs() {
        let path = executable_path(Path::new("/tmp/build"), "test_c_api");
        if cfg!(windows) {
            assert_eq!(path, Path::new("/tmp/build/test_c_api.exe"));
        } else {
            assert_eq!(path, Path::new("/tmp/build/test_c_api"));
        }
    }

    #[test]
    fn import_library_appends_the_msvc_suffix() {
        assert_eq!(
            import_library(Path::new("/tmp/build/lance_c.dll")),
            Path::new("/tmp/build/lance_c.dll.lib")
        );
    }

    fn as_strings(arguments: Vec<OsString>) -> Vec<String> {
        arguments
            .into_iter()
            .map(|argument| argument.to_string_lossy().into_owned())
            .collect()
    }

    fn source(language: Language) -> &'static Path {
        match language {
            Language::C => Path::new("/src/test_c_api.c"),
            Language::Cpp => Path::new("/src/test_cpp_api.cpp"),
        }
    }

    /// GNU-style invocation with the build-tree library, mirroring what the
    /// harness passes on Linux and macOS.
    fn reference_gnu(language: Language, links: bool, deny_warnings: bool) -> Vec<String> {
        let source = source(language);
        let compilation = Compilation {
            language,
            sources: &[source],
            includes: &[Path::new("/checkout/include")],
            output: links.then(|| Path::new("/tmp/build/test_c_api")),
            shared_library: Some(Path::new("/checkout/target/debug/liblance_c.so")),
            deny_warnings,
        };
        as_strings(gnu_arguments(&compilation))
    }

    /// MSVC invocation with the DLL import library. The paths stay
    /// slash-separated so the expectations hold on every host; MSVC receives
    /// whatever separator the real platform path uses.
    fn reference_msvc(language: Language, links: bool, deny_warnings: bool) -> Vec<String> {
        let source = source(language);
        let compilation = Compilation {
            language,
            sources: &[source],
            includes: &[Path::new("/checkout/include")],
            output: links.then(|| Path::new("/build/test_c_api.exe")),
            shared_library: Some(Path::new("/build/lance_c.dll")),
            deny_warnings,
        };
        as_strings(msvc_arguments(&compilation))
    }

    #[test]
    fn gnu_arguments_link_the_build_tree_library() {
        let arguments = reference_gnu(Language::C, true, false);

        assert!(arguments.contains(&"-std=c11".to_string()));
        assert!(arguments.contains(&"-o".to_string()));
        assert!(arguments.contains(&"/tmp/build/test_c_api".to_string()));
        assert!(arguments.contains(&"-I/checkout/include".to_string()));
        assert!(arguments.contains(&"/src/test_c_api.c".to_string()));
        assert!(arguments.contains(&"-L/checkout/target/debug".to_string()));
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
    fn msvc_arguments_link_the_import_library() {
        let arguments = reference_msvc(Language::C, true, false);

        assert!(arguments.contains(&"/std:c11".to_string()));
        assert!(arguments.contains(&"/experimental:c11atomics".to_string()));
        assert!(arguments.contains(&"/W4".to_string()));
        assert!(arguments.contains(&"/I/checkout/include".to_string()));
        assert!(arguments.contains(&"/Fe:/build/test_c_api.exe".to_string()));
        assert!(arguments.contains(&"/Fo/build\\".to_string()));
        assert!(arguments.contains(&"/link".to_string()));
        assert!(arguments.contains(&"/build/lance_c.dll.lib".to_string()));
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
