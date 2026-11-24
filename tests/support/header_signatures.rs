// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Compiler-checked comparison of the public C function declarations with the
//! Rust `extern "C"` definitions.
//!
//! `tests/support/abi_layout.rs` checks structs and enums. This module covers
//! the other half of the public ABI: the `lance_*` function declarations. It reads every
//! modular header under `include/lance/`, collects the Rust definitions from
//! `src/`, requires both name sets to agree, and lets the C compiler compare
//! each header prototype with a function-pointer type generated from the Rust
//! signature. A mismatch fails the probe compilation and names the function.
//!
//! The comparison rewrites only one intentional C/Rust representation
//! difference in a temporary header tree: C `enum` types become `int32_t`
//! typedefs, matching the fixed-width discriminants the Rust side accepts.
//! Pointer qualifiers are compared exactly at every level, so a header that
//! drops `const` from a pointee fails the probe.
//!
//! The repository headers are never modified. Normalizing by text (rather than
//! compiler flags) is required because the compiler cannot treat an enum and
//! `int32_t` as compatible types.
//!
//! The pending `LANCE_API` / `LANCE_NODISCARD` / `LANCE_DEPRECATED` macros do
//! not affect the check: they expand to attributes, and the attribute fixture
//! exercise in [`assert_public_header_signatures`] compiles a declaration that
//! carries all three. The probe runs through the shared `c_toolchain` harness
//! and uses C11 `_Generic` plus `_Static_assert`, so GCC, Clang and MSVC
//! compile the same comparison.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

use crate::c_text::{is_ident, strip_comments};
use crate::c_toolchain;
use tempfile::TempDir;

/// One Rust `extern "C"` definition as written in `src/`.
#[derive(Clone, Debug, PartialEq, Eq)]
struct RustFunction {
    return_type: String,
    params: Vec<String>,
}

/// Verify every public header function declaration against its Rust
/// definition, then prove the checker detects declaration mismatches.
pub(crate) fn assert_public_header_signatures() {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let header_dir = manifest_dir.join("include").join("lance");
    let source_dir = manifest_dir.join("src");

    let rust = collect_rust_fns(&source_dir).unwrap_or_else(|error| panic!("{error}"));
    let header_names = collect_header_names(&header_dir).unwrap_or_else(|error| panic!("{error}"));
    assert!(
        !rust.is_empty(),
        "no `pub extern \"C\" fn` definitions found under {}",
        source_dir.display()
    );
    assert!(
        !header_names.is_empty(),
        "no public `lance_*` declarations found under {}",
        header_dir.display()
    );
    if let Err(error) = compare_function_sets(&header_names, &rust) {
        panic!("{error}");
    }

    let probe = generate_probe(&rust).unwrap_or_else(|error| panic!("{error}"));
    let (headers, enum_typedefs) =
        normalize_headers(&header_dir).unwrap_or_else(|error| panic!("{error}"));
    assert!(
        enum_typedefs > 0,
        "normalization found no C enum typedefs in {}",
        header_dir.display()
    );
    let include_dir = headers.path().join("include");
    if let Err(stderr) = compile_probe(&include_dir, &probe) {
        panic!(
            "public C header declarations and Rust extern \"C\" signatures disagree \
             ({} header declarations, {} Rust definitions); the compiler reported:\n{stderr}",
            header_names.len(),
            rust.len()
        );
    }

    self_test_declaration_mismatches(headers.path(), &probe);
    self_test_attribute_fixture();
}

/// Collect `pub extern "C" fn` definitions, including macro-generated ones.
fn collect_rust_fns(source_dir: &Path) -> Result<BTreeMap<String, RustFunction>, String> {
    let mut paths: Vec<PathBuf> = fs::read_dir(source_dir)
        .map_err(|error| format!("failed to read {}: {error}", source_dir.display()))?
        .map(|entry| {
            entry
                .map(|entry| entry.path())
                .map_err(|error| format!("failed to read {}: {error}", source_dir.display()))
        })
        .collect::<Result<_, _>>()?;
    paths.retain(|path| path.extension().is_some_and(|extension| extension == "rs"));
    paths.sort();

    let mut fns = BTreeMap::new();
    for path in paths {
        let source = fs::read_to_string(&path)
            .map_err(|error| format!("failed to read {}: {error}", path.display()))?;
        for (name, function) in parse_rust_file(&source, &path)? {
            if let Some(previous) = fns.insert(name.clone(), function.clone())
                && previous != function
            {
                return Err(format!(
                    "`{name}` has conflicting extern \"C\" definitions; {} disagrees with an earlier definition",
                    path.display()
                ));
            }
        }
    }
    Ok(fns)
}

/// Parse one Rust source file for top-level `extern "C"` definitions and for
/// `macro_rules!` templates invoked with a `lance_*` name. The scanner works on
/// tokens, so formatting, comments, and multi-line signatures do not matter.
fn parse_rust_file(source: &str, path: &Path) -> Result<Vec<(String, RustFunction)>, String> {
    let tokens = tokenize_rust(&strip_rust_comments(source));
    let mut fns = Vec::new();
    let mut index = 0;
    while index + 3 < tokens.len() {
        if tokens[index] == "extern" && tokens[index + 1] == "\"C\"" && tokens[index + 2] == "fn" {
            let name = tokens.get(index + 3).map(String::as_str).unwrap_or("");
            if name.starts_with("lance_") && is_ident(name) && is_public_definition(&tokens, index)
            {
                fns.push(parse_signature(&tokens, index, path)?);
            }
        }
        index += 1;
    }

    for (macro_name, template) in parse_extern_macros(&tokens, path)? {
        for index in 0..tokens.len().saturating_sub(3) {
            if tokens[index] == macro_name
                && tokens[index + 1] == "!"
                && tokens[index + 2] == "("
                && tokens[index + 3].starts_with("lance_")
                && is_ident(&tokens[index + 3])
            {
                fns.push((tokens[index + 3].clone(), template.clone()));
            }
        }
    }

    Ok(fns)
}

/// Whether the `extern "C"` token sequence belongs to a `pub` definition.
fn is_public_definition(tokens: &[String], extern_index: usize) -> bool {
    if extern_index == 0 {
        return false;
    }
    if tokens[extern_index - 1] == "pub" {
        return true;
    }
    extern_index >= 2 && tokens[extern_index - 1] == "unsafe" && tokens[extern_index - 2] == "pub"
}

/// Find `macro_rules!` bodies that emit a public `extern "C" fn $name`.
fn parse_extern_macros(
    tokens: &[String],
    path: &Path,
) -> Result<Vec<(String, RustFunction)>, String> {
    let mut macros = Vec::new();
    let mut index = 0;
    while index + 3 < tokens.len() {
        let is_macro = tokens[index] == "macro_rules"
            && tokens[index + 1] == "!"
            && is_ident(&tokens[index + 2])
            && tokens.get(index + 3).map(String::as_str) == Some("{");
        if !is_macro {
            index += 1;
            continue;
        }
        let name = tokens[index + 2].clone();
        let end = matching_token(tokens, index + 3, "{", "}")
            .ok_or_else(|| format!("{}: unterminated macro_rules! {name} body", path.display()))?;
        let body = &tokens[index + 4..end];
        let mut body_index = 0;
        while body_index + 3 < body.len() {
            let is_template = body[body_index] == "extern"
                && body[body_index + 1] == "\"C\""
                && body[body_index + 2] == "fn"
                && body[body_index + 3] == "$";
            if is_template {
                let (function_name, function) =
                    parse_signature(tokens, index + 4 + body_index, path)?;
                if function_name != "$name" {
                    return Err(format!(
                        "{}: macro_rules! {name} uses `{function_name}` instead of `$name`",
                        path.display()
                    ));
                }
                macros.push((name.clone(), function));
                break;
            }
            body_index += 1;
        }
        index = end + 1;
    }
    Ok(macros)
}

/// Parse one `extern "C" fn <name>(...) -> <type>` from a token stream.
fn parse_signature(
    tokens: &[String],
    extern_index: usize,
    path: &Path,
) -> Result<(String, RustFunction), String> {
    let mut cursor = extern_index + 3;
    let name_start = cursor;
    if tokens.get(cursor).map(String::as_str) == Some("$") {
        cursor += 1;
    }
    if !tokens.get(cursor).is_some_and(|token| is_ident(token)) {
        return Err(format!(
            "{}: extern \"C\" declaration at token {extern_index} has no function name",
            path.display()
        ));
    }
    cursor += 1;
    let name = tokens[name_start..cursor].concat();
    if tokens.get(cursor).map(String::as_str) != Some("(") {
        return Err(format!(
            "{}: `{name}` has no parameter list after its name",
            path.display()
        ));
    }
    let params_end = matching_token(tokens, cursor, "(", ")").ok_or_else(|| {
        format!(
            "{}: `{name}` has an unterminated parameter list",
            path.display()
        )
    })?;
    let mut params = Vec::new();
    for param in split_top_level_tokens(&tokens[cursor + 1..params_end], ",") {
        if param.is_empty() || (param.len() == 3 && param.iter().all(|token| token == ".")) {
            continue;
        }
        let colon = top_level_position(param, ":").ok_or_else(|| {
            format!(
                "{}: `{name}` parameter `{}` has no `name: type` form",
                path.display(),
                param.join(" ")
            )
        })?;
        params.push(join_type_tokens(&param[colon + 1..]));
    }

    cursor = params_end + 1;
    let mut return_type = "()".to_owned();
    if tokens.get(cursor).map(String::as_str) == Some("-")
        && tokens.get(cursor + 1).map(String::as_str) == Some(">")
    {
        cursor += 2;
        let return_end = find_top_level_token(tokens, cursor, "{").ok_or_else(|| {
            format!(
                "{}: `{name}` has no body after its return type",
                path.display()
            )
        })?;
        return_type = join_type_tokens(&tokens[cursor..return_end]);
    }
    Ok((
        name,
        RustFunction {
            return_type,
            params,
        },
    ))
}

/// Strip Rust line comments and nested block comments, preserving string
/// literals. `extern "C"` is the only string the scanner needs to keep.
fn strip_rust_comments(source: &str) -> String {
    let mut output = String::with_capacity(source.len());
    let mut chars = source.chars().peekable();
    while let Some(ch) = chars.next() {
        match ch {
            '/' => match chars.peek() {
                Some('/') => {
                    chars.next();
                    for comment_ch in chars.by_ref() {
                        if comment_ch == '\n' {
                            output.push('\n');
                            break;
                        }
                    }
                }
                Some('*') => {
                    chars.next();
                    let mut depth = 1usize;
                    let mut previous = '\0';
                    for comment_ch in chars.by_ref() {
                        if previous == '/' && comment_ch == '*' {
                            depth += 1;
                            previous = '\0';
                            continue;
                        }
                        if previous == '*' && comment_ch == '/' {
                            depth -= 1;
                            if depth == 0 {
                                break;
                            }
                            previous = '\0';
                            continue;
                        }
                        if comment_ch == '\n' {
                            output.push('\n');
                        }
                        previous = comment_ch;
                    }
                    output.push(' ');
                }
                _ => output.push(ch),
            },
            '"' => {
                output.push(ch);
                let mut escaped = false;
                for literal_ch in chars.by_ref() {
                    output.push(literal_ch);
                    if escaped {
                        escaped = false;
                    } else if literal_ch == '\\' {
                        escaped = true;
                    } else if literal_ch == '"' {
                        break;
                    }
                }
            }
            _ => output.push(ch),
        }
    }
    output
}

/// Tokenize Rust source into identifiers, string literals, and single
/// punctuation characters. Whitespace is dropped, so formatting is irrelevant.
fn tokenize_rust(source: &str) -> Vec<String> {
    let mut tokens = Vec::new();
    let mut chars = source.char_indices().peekable();
    while let Some((start, ch)) = chars.next() {
        if ch.is_whitespace() {
            continue;
        }
        if ch == '"' {
            let mut end = start + ch.len_utf8();
            let mut escaped = false;
            for (index, literal_ch) in chars.by_ref() {
                end = index + literal_ch.len_utf8();
                if escaped {
                    escaped = false;
                } else if literal_ch == '\\' {
                    escaped = true;
                } else if literal_ch == '"' {
                    break;
                }
            }
            tokens.push(source[start..end].to_owned());
            continue;
        }
        if is_ident_start_ascii(ch) {
            let mut end = start + ch.len_utf8();
            while let Some(&(index, next)) = chars.peek() {
                if is_ident_continue_ascii(next) {
                    end = index + next.len_utf8();
                    chars.next();
                } else {
                    break;
                }
            }
            tokens.push(source[start..end].to_owned());
            continue;
        }
        tokens.push(ch.to_string());
    }
    tokens
}

/// Join a type's tokens into the compact spelling used by [`c_type`].
fn join_type_tokens(tokens: &[String]) -> String {
    let mut value = tokens.join(" ");
    while value.contains(" : : ") {
        value = value.replace(" : : ", "::");
    }
    for _ in 0..2 {
        value = value
            .replace(" :: ", "::")
            .replace(":: ", "::")
            .replace(" ::", "::");
    }
    value = value
        .replace(" <", "<")
        .replace("< ", "<")
        .replace(" >", ">")
        .replace("> ", ">")
        .replace("* const", "*const")
        .replace("* mut", "*mut")
        .replace("& ", "&");
    value.trim().to_owned()
}

fn matching_token(
    tokens: &[String],
    open_index: usize,
    opening: &str,
    closing: &str,
) -> Option<usize> {
    let mut depth = 0usize;
    for (index, token) in tokens[open_index..].iter().enumerate() {
        if token == opening {
            depth += 1;
        } else if token == closing {
            depth -= 1;
            if depth == 0 {
                return Some(open_index + index);
            }
        }
    }
    None
}

fn split_top_level_tokens<'a>(tokens: &'a [String], separator: &str) -> Vec<&'a [String]> {
    let mut groups = Vec::new();
    let mut depth = 0usize;
    let mut start = 0;
    for (index, token) in tokens.iter().enumerate() {
        match token.as_str() {
            "(" | "[" | "{" | "<" => depth += 1,
            ")" | "]" | "}" | ">" => depth = depth.saturating_sub(1),
            _ if token == separator && depth == 0 => {
                groups.push(&tokens[start..index]);
                start = index + 1;
            }
            _ => {}
        }
    }
    groups.push(&tokens[start..]);
    groups
}

fn top_level_position(tokens: &[String], target: &str) -> Option<usize> {
    let mut depth = 0usize;
    for (index, token) in tokens.iter().enumerate() {
        match token.as_str() {
            "(" | "[" | "{" | "<" => depth += 1,
            ")" | "]" | "}" | ">" => depth = depth.saturating_sub(1),
            _ if token == target && depth == 0 => return Some(index),
            _ => {}
        }
    }
    None
}

fn find_top_level_token(tokens: &[String], start: usize, target: &str) -> Option<usize> {
    let mut depth = 0usize;
    for (index, token) in tokens[start..].iter().enumerate() {
        match token.as_str() {
            "(" | "[" | "<" => depth += 1,
            ")" | "]" | ">" => depth = depth.saturating_sub(1),
            _ if token == target && depth == 0 => return Some(start + index),
            _ => {}
        }
    }
    None
}

fn is_ident_start_ascii(ch: char) -> bool {
    ch.is_ascii_alphabetic() || ch == '_'
}

fn is_ident_continue_ascii(ch: char) -> bool {
    ch.is_ascii_alphanumeric() || ch == '_'
}

/// Collect every `lance_*` function declared by the public headers. String
/// literals are skipped so a deprecated message that mentions a function name
/// is not mistaken for a declaration.
fn collect_header_names(header_dir: &Path) -> Result<BTreeSet<String>, String> {
    let mut names = BTreeSet::new();
    for path in header_paths(header_dir)? {
        let source = fs::read_to_string(&path)
            .map_err(|error| format!("failed to read {}: {error}", path.display()))?;
        names.extend(scan_lance_declarations(&strip_comments(&source)));
    }
    Ok(names)
}

fn scan_lance_declarations(source: &str) -> BTreeSet<String> {
    let bytes = source.as_bytes();
    let mut names = BTreeSet::new();
    let mut cursor = 0;
    while cursor < bytes.len() {
        if bytes[cursor] == b'"' || bytes[cursor] == b'\'' {
            skip_string_literal(bytes, &mut cursor);
            continue;
        }
        if is_ident_start(bytes[cursor]) {
            let start = cursor;
            read_ident(bytes, &mut cursor);
            let word = &source[start..cursor];
            if word.starts_with("lance_") {
                let mut next = cursor;
                skip_ascii_whitespace(bytes, &mut next);
                if bytes.get(next) == Some(&b'(') {
                    names.insert(word.to_owned());
                }
            }
            continue;
        }
        cursor += 1;
    }
    names
}

fn skip_string_literal(bytes: &[u8], cursor: &mut usize) {
    let quote = bytes[*cursor];
    *cursor += 1;
    while *cursor < bytes.len() {
        match bytes[*cursor] {
            b'\\' => *cursor += 2,
            byte if byte == quote => {
                *cursor += 1;
                return;
            }
            _ => *cursor += 1,
        }
    }
}

/// Require the header and Rust definition name sets to be identical.
fn compare_function_sets(
    header_names: &BTreeSet<String>,
    rust_fns: &BTreeMap<String, RustFunction>,
) -> Result<(), String> {
    let rust_names: BTreeSet<String> = rust_fns.keys().cloned().collect();
    let header_only: Vec<&String> = header_names.difference(&rust_names).collect();
    let rust_only: Vec<&String> = rust_names.difference(header_names).collect();
    if header_only.is_empty() && rust_only.is_empty() {
        return Ok(());
    }
    let mut message = String::from(
        "public C headers and Rust extern \"C\" definitions have different function sets:",
    );
    for name in header_only {
        writeln!(
            message,
            "  declared in include/lance but missing a Rust extern \"C\" definition: {name}"
        )
        .unwrap();
    }
    for name in rust_only {
        writeln!(
            message,
            "  defined as Rust extern \"C\" but missing from include/lance: {name}"
        )
        .unwrap();
    }
    Err(message)
}

/// Copy every public header to a temporary tree with enum typedefs replaced by
/// `int32_t` typedefs. All other text, including pointer qualifiers, is kept.
fn normalize_headers(header_dir: &Path) -> Result<(TempDir, usize), String> {
    let temp = tempfile::tempdir()
        .map_err(|error| format!("failed to create signature-check directory: {error}"))?;
    let output_dir = temp.path().join("include").join("lance");
    fs::create_dir_all(&output_dir).map_err(|error| {
        format!(
            "failed to create normalized header directory {}: {error}",
            output_dir.display()
        )
    })?;

    let mut enum_typedefs = 0;
    for path in header_paths(header_dir)? {
        let file_name = path.file_name().unwrap_or_default();
        let source = fs::read_to_string(&path)
            .map_err(|error| format!("failed to read {}: {error}", path.display()))?;
        let normalized = normalize_header(&source)
            .map_err(|error| format!("failed to normalize {}: {error}", path.display()))?;
        enum_typedefs += normalized.enum_typedefs;
        let destination = output_dir.join(file_name);
        fs::write(&destination, normalized.text)
            .map_err(|error| format!("failed to write {}: {error}", destination.display()))?;
    }
    Ok((temp, enum_typedefs))
}

struct NormalizedHeader {
    text: String,
    enum_typedefs: usize,
}

/// Rewrite `typedef enum ... { ... } Name;` to `typedef int32_t Name;`.
/// Comments are stripped first; string literals are preserved so attribute
/// messages stay intact. Pointer qualifiers are preserved.
fn normalize_header(source: &str) -> Result<NormalizedHeader, String> {
    let source = strip_comments(source);
    let bytes = source.as_bytes();
    let mut output = String::with_capacity(source.len());
    let mut enum_typedefs = 0;
    let mut cursor = 0;

    while cursor < bytes.len() {
        if bytes[cursor] == b'"' || bytes[cursor] == b'\'' {
            let quote = bytes[cursor];
            let start = cursor;
            cursor += 1;
            while cursor < bytes.len() {
                match bytes[cursor] {
                    b'\\' => cursor += 2,
                    byte if byte == quote => {
                        cursor += 1;
                        break;
                    }
                    _ => cursor += 1,
                }
            }
            output.push_str(&source[start..cursor.min(source.len())]);
            continue;
        }
        if is_ident_start(bytes[cursor]) {
            let start = cursor;
            cursor += 1;
            while cursor < bytes.len() && is_ident_byte(bytes[cursor]) {
                cursor += 1;
            }
            let word = &source[start..cursor];
            if word == "typedef" {
                let mut lookahead = cursor;
                skip_ascii_whitespace(bytes, &mut lookahead);
                let is_enum_keyword = source[lookahead..].starts_with("enum")
                    && !bytes
                        .get(lookahead + "enum".len())
                        .is_some_and(|byte| is_ident_byte(*byte));
                if is_enum_keyword {
                    let (end, alias) = parse_enum_typedef(bytes, start).ok_or_else(|| {
                        format!(
                            "unsupported enum typedef near `{}`",
                            &source[start..source.len().min(start + 60)]
                        )
                    })?;
                    output.push_str("typedef int32_t ");
                    output.push_str(&alias);
                    output.push(';');
                    enum_typedefs += 1;
                    cursor = end;
                    continue;
                }
            }
            output.push_str(word);
            continue;
        }
        let ch = source[cursor..]
            .chars()
            .next()
            .expect("cursor is inside the source");
        output.push(ch);
        cursor += ch.len_utf8();
    }

    Ok(NormalizedHeader {
        text: output,
        enum_typedefs,
    })
}

/// Match `typedef enum [tag] { ... } alias;` starting at `start`.
fn parse_enum_typedef(bytes: &[u8], start: usize) -> Option<(usize, String)> {
    let mut cursor = start + "typedef".len();
    skip_ascii_whitespace(bytes, &mut cursor);
    if !bytes[cursor..].starts_with(b"enum") {
        return None;
    }
    cursor += "enum".len();
    skip_ascii_whitespace(bytes, &mut cursor);
    if bytes.get(cursor).is_some_and(|byte| is_ident_start(*byte)) {
        read_ident(bytes, &mut cursor);
        skip_ascii_whitespace(bytes, &mut cursor);
    }
    if bytes.get(cursor) != Some(&b'{') {
        return None;
    }
    let mut depth = 0usize;
    while cursor < bytes.len() {
        match bytes[cursor] {
            b'{' => depth += 1,
            b'}' => {
                depth -= 1;
                if depth == 0 {
                    cursor += 1;
                    break;
                }
            }
            _ => {}
        }
        cursor += 1;
    }
    if depth != 0 {
        return None;
    }
    skip_ascii_whitespace(bytes, &mut cursor);
    let alias_start = cursor;
    read_ident(bytes, &mut cursor);
    if alias_start == cursor {
        return None;
    }
    let alias = String::from_utf8_lossy(&bytes[alias_start..cursor]).into_owned();
    skip_ascii_whitespace(bytes, &mut cursor);
    if bytes.get(cursor) != Some(&b';') {
        return None;
    }
    Some((cursor + 1, alias))
}

/// Build one `_Static_assert` per Rust definition comparing the header's
/// function pointer type with the type implied by the Rust signature.
fn generate_probe(fns: &BTreeMap<String, RustFunction>) -> Result<String, String> {
    let mut probe = String::from(
        "#include <stdbool.h>\n\
         #include <stddef.h>\n\
         #include <stdint.h>\n\n\
         #include \"lance/lance.h\"\n\n\
         #if defined(_MSC_VER)\n\
         #pragma warning(disable : 4996)\n\
         #else\n\
         #pragma GCC diagnostic ignored \"-Wdeprecated-declarations\"\n\
         #endif\n\n",
    );
    for (name, function) in fns {
        let return_type = c_type(&function.return_type)
            .map_err(|error| format!("`{name}` return type: {error}"))?;
        let params = function
            .params
            .iter()
            .map(|param| c_type(param))
            .collect::<Result<Vec<_>, _>>()
            .map_err(|error| format!("`{name}` parameter type: {error}"))?;
        let params = if params.is_empty() {
            "void".to_owned()
        } else {
            params.join(", ")
        };
        writeln!(probe, "typedef {return_type} (*expected_{name})({params});").unwrap();
        writeln!(
            probe,
            "_Static_assert(_Generic(&{name}, expected_{name}: 1, default: 0), \"{name}\");"
        )
        .unwrap();
    }
    Ok(probe)
}

/// Map one Rust FFI type to the C type that the public header must declare.
/// Every pointer level keeps its pointee constness: `*const T` becomes
/// `T const*` and nested pointers keep the qualifier on the correct level.
fn c_type(rust_type: &str) -> Result<String, String> {
    let mut rust_type = rust_type.trim();
    if rust_type == "()" {
        return Ok("void".to_owned());
    }
    if let Some(inner) = rust_type.strip_prefix("Option<") {
        rust_type = inner
            .strip_suffix('>')
            .ok_or_else(|| format!("unterminated Option type `{rust_type}`"))?
            .trim();
    }
    // Layers are collected outermost first; rendering walks them backwards.
    let mut layers = Vec::new();
    loop {
        if let Some(rest) = rust_type.strip_prefix("*const ") {
            layers.push(true);
            rust_type = rest.trim();
        } else if let Some(rest) = rust_type.strip_prefix("*mut ") {
            layers.push(false);
            rust_type = rest.trim();
        } else {
            break;
        }
    }
    let base = rust_type.rsplit("::").next().unwrap_or(rust_type).trim();
    let mapped = match base {
        "i8" => "int8_t",
        "u8" => "uint8_t",
        "i16" => "int16_t",
        "u16" => "uint16_t",
        "i32" => "int32_t",
        "u32" => "uint32_t",
        "i64" => "int64_t",
        "u64" => "uint64_t",
        "isize" => "intptr_t",
        "usize" => "size_t",
        "f32" => "float",
        "f64" => "double",
        "bool" => "bool",
        "c_char" => "char",
        "c_void" => "void",
        "c_int" => "int32_t",
        // The C declarations use the typedef name; the normalized header turns
        // it into an `int32_t` typedef.
        "LanceErrorCode" | "LancePollStatus" => "int32_t",
        "FFI_ArrowArray" => "struct ArrowArray",
        "FFI_ArrowSchema" => "struct ArrowSchema",
        "FFI_ArrowArrayStream" => "struct ArrowArrayStream",
        _ if is_ident(base) => base,
        _ => {
            return Err(format!(
                "unsupported Rust type `{rust_type}`; extend the mapping in tests/support/header_signatures.rs"
            ));
        }
    };
    let mut c_type = mapped.to_owned();
    for pointee_is_const in layers.iter().rev() {
        if *pointee_is_const {
            c_type.push_str(" const");
        }
        c_type.push('*');
    }
    Ok(c_type)
}

/// Compile a probe against a normalized include tree.
fn compile_probe(include_dir: &Path, probe: &str) -> Result<(), String> {
    let build_dir = tempfile::tempdir()
        .map_err(|error| format!("failed to create signature probe directory: {error}"))?;
    let source_path = build_dir.path().join("header_signature_probe.c");
    fs::write(&source_path, probe)
        .map_err(|error| format!("failed to write {}: {error}", source_path.display()))?;

    let sources: [&Path; 1] = [&source_path];
    let includes: [&Path; 1] = [include_dir];
    let output = c_toolchain::compile_command(&c_toolchain::Compilation {
        language: c_toolchain::Language::C,
        sources: &sources,
        includes: &includes,
        output: None,
        shared_library: None,
        deny_warnings: true,
    })
    .output()
    .map_err(|error| format!("failed to run the C compiler: {error}"))?;
    if output.status.success() {
        Ok(())
    } else {
        // MSVC writes diagnostics to stdout while GCC and Clang use stderr.
        // Keep both streams so the expected-failure checks can find the
        // function name regardless of the compiler.
        Err(format!(
            "stdout:\n{}\nstderr:\n{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        ))
    }
}

/// Prove the compiler check fails for a changed return type, a changed
/// parameter list, a removed declaration, a writable-pointee drift, and a
/// dropped nested pointer qualifier. Each mutation runs on a copy of the
/// normalized headers, so the repository is untouched.
fn self_test_declaration_mismatches(normalized_root: &Path, probe: &str) {
    let cases = [
        (
            "dataset.h",
            "uint64_t lance_dataset_version(",
            "uint32_t lance_dataset_version(",
            "lance_dataset_version",
        ),
        (
            "dataset.h",
            "void lance_dataset_close(LanceDataset* dataset);",
            "void lance_dataset_close(void);",
            "lance_dataset_close",
        ),
        (
            "version.h",
            "const char* lance_c_version(void);",
            "",
            "lance_c_version",
        ),
        (
            "dataset.h",
            "const char* lance_dataset_uri(",
            "char* lance_dataset_uri(",
            "lance_dataset_uri",
        ),
        (
            "dataset.h",
            "const char* const* storage_opts",
            "const char** storage_opts",
            "lance_dataset_open",
        ),
    ];
    for (file, from, to, function) in cases {
        let copy = copy_include_tree(normalized_root);
        let path = copy.path().join("include").join("lance").join(file);
        let text = fs::read_to_string(&path)
            .unwrap_or_else(|error| panic!("failed to read {}: {error}", path.display()));
        assert!(
            text.contains(from),
            "self-test anchor `{from}` is missing from {}",
            path.display()
        );
        fs::write(&path, text.replacen(from, to, 1))
            .unwrap_or_else(|error| panic!("failed to write {}: {error}", path.display()));
        match compile_probe(&copy.path().join("include"), probe) {
            Ok(()) => panic!(
                "signature check accepted a changed declaration of `{function}`; the compiler probe never fails"
            ),
            Err(diagnostics) => assert!(
                diagnostics.contains(function),
                "signature check rejected `{function}` but neither compiler stream named it:\n{diagnostics}"
            ),
        }
    }
}

/// Copy a normalized include tree so a self-test mutation cannot leak.
fn copy_include_tree(root: &Path) -> TempDir {
    let destination = tempfile::tempdir().expect("failed to create header copy directory");
    copy_directory(&root.join("include"), &destination.path().join("include"));
    destination
}

fn copy_directory(source: &Path, destination: &Path) {
    fs::create_dir_all(destination)
        .unwrap_or_else(|error| panic!("failed to create {}: {error}", destination.display()));
    for entry in fs::read_dir(source)
        .unwrap_or_else(|error| panic!("failed to read {}: {error}", source.display()))
    {
        let entry = entry.unwrap_or_else(|error| panic!("failed to read directory entry: {error}"));
        let path = entry.path();
        let target = destination.join(entry.file_name());
        if path.is_dir() {
            copy_directory(&path, &target);
        } else {
            fs::copy(&path, &target).unwrap_or_else(|error| {
                panic!(
                    "failed to copy {} to {}: {error}",
                    path.display(),
                    target.display()
                )
            });
        }
    }
}

/// A declaration carrying all three pending #289 attributes must parse and
/// compile. This fixture does not depend on #289 being merged.
fn self_test_attribute_fixture() {
    let fixture = tempfile::tempdir().expect("failed to create attribute fixture directory");
    let include_dir = fixture.path().join("include").join("lance");
    fs::create_dir_all(&include_dir).expect("failed to create fixture include directory");
    fs::write(
        include_dir.join("attributes.h"),
        "#ifndef LANCE_ATTRIBUTES_H\n\
         #define LANCE_ATTRIBUTES_H\n\
         #if defined(__GNUC__) || defined(__clang__)\n\
         #  define LANCE_API __attribute__((visibility(\"default\")))\n\
         #  define LANCE_NODISCARD __attribute__((warn_unused_result))\n\
         #  define LANCE_DEPRECATED(message) __attribute__((deprecated(message)))\n\
         #else\n\
         #  define LANCE_API\n\
         #  define LANCE_NODISCARD\n\
         #  define LANCE_DEPRECATED(message)\n\
         #endif\n\
         #endif\n",
    )
    .expect("failed to write fixture attributes");
    fs::write(
        include_dir.join("lance.h"),
        "#ifndef LANCE_FIXTURE_H\n\
         #define LANCE_FIXTURE_H\n\
         #include <stdint.h>\n\
         #include \"lance/attributes.h\"\n\
         #ifdef __cplusplus\n\
         extern \"C\" {\n\
         #endif\n\
         LANCE_NODISCARD LANCE_API int32_t lance_fixture(void);\n\
         LANCE_DEPRECATED(\"use lance_fixture\") LANCE_API const char* lance_legacy(void);\n\
         #ifdef __cplusplus\n\
         }\n\
         #endif\n\
         #endif\n",
    )
    .expect("failed to write fixture header");

    let (headers, _) =
        normalize_headers(&include_dir).expect("failed to normalize attribute fixture");
    let fns = BTreeMap::from([
        (
            "lance_fixture".to_owned(),
            RustFunction {
                return_type: "i32".to_owned(),
                params: Vec::new(),
            },
        ),
        (
            "lance_legacy".to_owned(),
            RustFunction {
                return_type: "*const c_char".to_owned(),
                params: Vec::new(),
            },
        ),
    ]);
    let probe = generate_probe(&fns).expect("failed to generate attribute probe");
    if let Err(stderr) = compile_probe(&headers.path().join("include"), &probe) {
        panic!("the signature check does not accept attributed declarations:\n{stderr}");
    }
}

fn header_paths(header_dir: &Path) -> Result<Vec<PathBuf>, String> {
    let mut paths: Vec<PathBuf> = fs::read_dir(header_dir)
        .map_err(|error| format!("failed to read {}: {error}", header_dir.display()))?
        .map(|entry| {
            entry
                .map(|entry| entry.path())
                .map_err(|error| format!("failed to read {}: {error}", header_dir.display()))
        })
        .collect::<Result<_, _>>()?;
    paths.retain(|path| path.extension().is_some_and(|extension| extension == "h"));
    paths.sort();
    if paths.is_empty() {
        return Err(format!(
            "no public headers found in {}",
            header_dir.display()
        ));
    }
    Ok(paths)
}

fn skip_ascii_whitespace(bytes: &[u8], cursor: &mut usize) {
    while bytes
        .get(*cursor)
        .is_some_and(|byte| byte.is_ascii_whitespace())
    {
        *cursor += 1;
    }
}

fn read_ident(bytes: &[u8], cursor: &mut usize) {
    while bytes.get(*cursor).is_some_and(|byte| is_ident_byte(*byte)) {
        *cursor += 1;
    }
}

fn is_ident_start(byte: u8) -> bool {
    byte.is_ascii_alphabetic() || byte == b'_'
}

fn is_ident_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'_'
}

#[cfg(test)]
mod tests {
    use super::*;

    fn function(return_type: &str, params: &[&str]) -> RustFunction {
        RustFunction {
            return_type: return_type.to_owned(),
            params: params.iter().map(|param| (*param).to_owned()).collect(),
        }
    }

    #[test]
    fn c_type_maps_ffi_shapes() {
        assert_eq!(c_type("()").unwrap(), "void");
        assert_eq!(c_type("i32").unwrap(), "int32_t");
        assert_eq!(c_type("usize").unwrap(), "size_t");
        assert_eq!(c_type("bool").unwrap(), "bool");
        assert_eq!(c_type("c_char").unwrap(), "char");
        assert_eq!(c_type("c_void").unwrap(), "void");
        assert_eq!(c_type("*const c_char").unwrap(), "char const*");
        assert_eq!(c_type("*mut *mut LanceDataset").unwrap(), "LanceDataset**");
        assert_eq!(
            c_type("*const *const c_char").unwrap(),
            "char const* const*"
        );
        assert_eq!(c_type("*mut *const c_char").unwrap(), "char const**");
        assert_eq!(c_type("*const *mut c_char").unwrap(), "char* const*");
        assert_eq!(c_type("Option<LanceCallback>").unwrap(), "LanceCallback");
        assert_eq!(
            c_type("*const FFI_ArrowSchema").unwrap(),
            "struct ArrowSchema const*"
        );
        assert_eq!(
            c_type("*mut crate::file_reader::LanceFileReader").unwrap(),
            "LanceFileReader*"
        );
        assert_eq!(c_type("LanceErrorCode").unwrap(), "int32_t");
        assert!(c_type("unsafe extern \"C\" fn()").is_err());
    }

    #[test]
    fn rust_parser_collects_definitions_and_macro_outputs() {
        let source = r#"
pub unsafe
    extern "C"
    fn lance_alpha(
        a: * const c_char, // comment: pub extern "C" fn lance_phantom(
        b: u64,
    ) -> i32 {
    0
}

/* block comment: pub extern "C" fn lance_also_phantom() {} */
pub extern "C" fn lance_beta() {}

const PHANTOM: &str = "pub extern \"C\" fn lance_string_phantom()";

macro_rules! set_u32 {
    ($name:ident, $field:ident) => {
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $name(
            scanner: *mut LanceScanner,
            value: u32,
        ) -> i32 {
            0
        }
    };
}
set_u32!(lance_set_ef, ef);
"#;
        let fns = parse_rust_file(source, Path::new("fixture.rs")).unwrap();
        assert_eq!(fns.len(), 3);
        assert_eq!(
            fns[0],
            (
                "lance_alpha".to_owned(),
                function("i32", &["*const c_char", "u64"])
            )
        );
        assert_eq!(fns[1], ("lance_beta".to_owned(), function("()", &[])));
        assert_eq!(
            fns[2],
            (
                "lance_set_ef".to_owned(),
                function("i32", &["*mut LanceScanner", "u32"])
            )
        );
    }

    #[test]
    fn header_names_tolerate_attributes_and_comments() {
        let source = r#"
/* lance_not_a_function( */
LANCE_NODISCARD LANCE_API int32_t lance_fixture(void);
LANCE_DEPRECATED("call lance_old() instead") LANCE_API const char* lance_old(void);
LANCE_API int32_t lance_multi(
    const LanceDataset* dataset,
    uint64_t version
);
"#;
        let temp = tempfile::tempdir().unwrap();
        fs::write(temp.path().join("lance.h"), source).unwrap();
        let names = collect_header_names(temp.path()).unwrap();
        assert_eq!(
            names,
            BTreeSet::from([
                "lance_fixture".to_owned(),
                "lance_multi".to_owned(),
                "lance_old".to_owned(),
            ])
        );
    }

    #[test]
    fn normalizer_rewrites_enums_and_preserves_const() {
        let source = r#"
typedef enum LanceKeyExistenceFilterKind {
    LANCE_KEY_FILTER_EXACT = 0,
    LANCE_KEY_FILTER_BLOOM = 1,
} LanceKeyExistenceFilterKind;
int32_t lance_take(const char* const* names, const LanceDataset* dataset);
"#;
        let normalized = normalize_header(source).unwrap();
        assert_eq!(normalized.enum_typedefs, 1);
        assert!(
            normalized
                .text
                .contains("typedef int32_t LanceKeyExistenceFilterKind;")
        );
        let compact: String = normalized.text.split_whitespace().collect();
        assert!(
            compact.contains("int32_tlance_take(constchar*const*names,constLanceDataset*dataset);")
        );
    }

    #[test]
    fn normalizer_rejects_unparseable_enum_typedefs() {
        let source = "typedef enum Broken { A = 0 } ;\n";
        assert!(normalize_header(source).is_err());
    }

    #[test]
    fn function_set_comparison_reports_both_directions() {
        let headers = BTreeSet::from(["lance_both".to_owned(), "lance_header_only".to_owned()]);
        let rust = BTreeMap::from([
            ("lance_both".to_owned(), function("i32", &[])),
            ("lance_rust_only".to_owned(), function("i32", &[])),
        ]);
        let error = compare_function_sets(&headers, &rust).unwrap_err();
        assert!(error.contains(
            "include/lance but missing a Rust extern \"C\" definition: lance_header_only"
        ));
        assert!(
            error.contains("Rust extern \"C\" but missing from include/lance: lance_rust_only")
        );
    }

    #[test]
    fn generated_probe_rewrites_enums_and_preserves_const() {
        let fns = BTreeMap::from([(
            "lance_fixture".to_owned(),
            function("*mut LanceErrorCode", &["*const *const c_char"]),
        )]);
        let probe = generate_probe(&fns).unwrap();
        assert!(probe.contains("typedef int32_t* (*expected_lance_fixture)(char const* const*);"));
        assert!(probe.contains(
            "_Static_assert(_Generic(&lance_fixture, expected_lance_fixture: 1, default: 0), \"lance_fixture\");"
        ));
        assert!(probe.contains("#pragma warning(disable : 4996)"));
        assert!(probe.contains("#pragma GCC diagnostic ignored \"-Wdeprecated-declarations\""));
    }
}
