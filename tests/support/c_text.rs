// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The Lance Authors

//! Small C text helpers shared by the public-header tests.
//!
//! The public headers are hand-maintained, so tests that inventory or compare
//! them need a deterministic comment stripper and tokenizer. These helpers
//! understand string and character literals, which matters for attribute
//! messages such as `LANCE_DEPRECATED("...")`.

/// Remove C and C++ comments while preserving newlines and string literals.
pub(crate) fn strip_comments(source: &str) -> String {
    let mut output = String::with_capacity(source.len());
    let mut chars = source.chars().peekable();
    while let Some(ch) = chars.next() {
        match ch {
            '"' | '\'' => {
                output.push(ch);
                let quote = ch;
                let mut escaped = false;
                for literal_ch in chars.by_ref() {
                    output.push(literal_ch);
                    if escaped {
                        escaped = false;
                    } else if literal_ch == '\\' {
                        escaped = true;
                    } else if literal_ch == quote || literal_ch == '\n' {
                        break;
                    }
                }
            }
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
                    let mut previous = '\0';
                    for comment_ch in chars.by_ref() {
                        if previous == '*' && comment_ch == '/' {
                            break;
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
            _ => output.push(ch),
        }
    }
    output
}

/// Split C source into identifier and punctuation tokens, dropping whitespace.
pub(crate) fn tokenize(source: &str) -> Vec<String> {
    let mut tokens = Vec::new();
    let mut chars = source.char_indices().peekable();
    while let Some((start, ch)) = chars.next() {
        if ch.is_ascii_alphanumeric() || ch == '_' {
            let mut end = start + ch.len_utf8();
            while let Some(&(next, next_ch)) = chars.peek() {
                if next_ch.is_ascii_alphanumeric() || next_ch == '_' {
                    end = next + next_ch.len_utf8();
                    chars.next();
                } else {
                    break;
                }
            }
            tokens.push(source[start..end].to_owned());
        } else if !ch.is_whitespace() {
            tokens.push(ch.to_string());
        }
    }
    tokens
}

/// Whether a token is a C identifier.
pub(crate) fn is_ident(token: &str) -> bool {
    let mut chars = token.chars();
    match chars.next() {
        Some(first) if first.is_ascii_alphabetic() || first == '_' => {
            chars.all(|ch| ch.is_ascii_alphanumeric() || ch == '_')
        }
        _ => false,
    }
}
