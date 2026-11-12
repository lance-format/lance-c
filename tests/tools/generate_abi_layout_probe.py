#!/usr/bin/env python3
"""Generate the validation-only current-main ABI layout/discriminant probe.

The generated C11 probe prints one record per public enum value, struct size,
and struct field offset. The generated Rust integration test prints the same
records from the Rust ABI types and compares the two sets.

This script is validation tooling for issue #102. It is not part of the
product ABI and does not modify the public header.
"""

from __future__ import annotations

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "include" / "lance" / "lance.h"
C_PROBE = ROOT / "tests" / "cpp" / "abi_layout_current.c"
CPP_PROBE = ROOT / "tests" / "cpp" / "abi_layout_current.cpp"
RUST_TEST = ROOT / "tests" / "abi_layout_current_test.rs"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//.*", "", text)


def parse_c_enums(header: str) -> dict[str, list[tuple[str, int]]]:
    enums: dict[str, list[tuple[str, int]]] = {}
    pattern = re.compile(
        r"typedef\s+enum(?:\s+\w+)?\s*\{(.*?)\}\s*(Lance\w+)\s*;", re.S
    )
    for match in pattern.finditer(header):
        body = strip_comments(match.group(1))
        values: list[tuple[str, int]] = []
        next_value = 0
        for part in body.split(","):
            part = part.strip()
            if not part:
                continue
            item = re.match(r"(\w+)\s*(?:=\s*(-?\d+))?$", part, re.S)
            if item is None:
                raise SystemExit(f"cannot parse C enumerator: {part!r}")
            if item.group(2) is not None:
                next_value = int(item.group(2))
            values.append((item.group(1), next_value))
            next_value += 1
        enums[match.group(2)] = values
    return enums


def parse_c_structs(header: str) -> dict[str, list[str]]:
    structs: dict[str, list[str]] = {}
    pattern = re.compile(
        r"typedef\s+struct(?:\s+\w+)?\s*\{(.*?)\}\s*(Lance\w+)\s*;", re.S
    )
    for match in pattern.finditer(header):
        body = strip_comments(match.group(1))
        fields: list[str] = []
        for part in body.split(";"):
            part = " ".join(part.split()).strip()
            if not part:
                continue
            function_pointer = re.search(r"\(\*\s*(\w+)\s*\)", part)
            if function_pointer is not None:
                fields.append(function_pointer.group(1))
                continue
            field = re.search(r"(\w+)\s*(?:\[[^\]]*\])?\s*$", part)
            if field is None:
                raise SystemExit(f"cannot parse C field: {part!r}")
            fields.append(field.group(1))
        structs[match.group(2)] = fields
    return structs


def parse_rust_enums() -> dict[str, list[tuple[str, int]]]:
    enums: dict[str, list[tuple[str, int]]] = {}
    for source in sorted((ROOT / "src").glob("*.rs")):
        text = source.read_text()
        pattern = re.compile(
            r"#\[repr\((?:C|i32)\)\]\s*(?:#\[[^\]]*\]\s*)*"
            r"pub enum (Lance\w+|IndexBuildProgressEvent)\s*\{(.*?)\n\}",
            re.S,
        )
        for match in pattern.finditer(text):
            body = strip_comments(match.group(2))
            values: list[tuple[str, int]] = []
            next_value = 0
            for part in body.split(","):
                part = part.strip()
                if not part:
                    continue
                item = re.match(r"(\w+)\s*(?:=\s*(-?\d+))?$", part, re.S)
                if item is None:
                    raise SystemExit(
                        f"cannot parse Rust enum {match.group(1)}: {part!r}"
                    )
                if item.group(2) is not None:
                    next_value = int(item.group(2))
                values.append((item.group(1), next_value))
                next_value += 1
            enums[match.group(1)] = values
    return enums


def parse_rust_structs() -> dict[str, list[str]]:
    structs: dict[str, list[str]] = {}
    for source in sorted((ROOT / "src").glob("*.rs")):
        text = source.read_text()
        pattern = re.compile(
            r"#\[repr\(C\)\]\s*(?:#\[[^\]]*\]\s*)*"
            r"pub struct (Lance\w+)\s*\{(.*?)\n\}",
            re.S,
        )
        for match in pattern.finditer(text):
            body = strip_comments(match.group(2))
            fields: list[str] = []
            for part in body.split(","):
                part = part.strip()
                if not part:
                    continue
                field = re.match(r"pub\s+(\w+)\b", part)
                if field is None:
                    raise SystemExit(
                        f"cannot parse Rust field {match.group(1)}: {part!r}"
                    )
                fields.append(field.group(1))
            structs[match.group(1)] = fields
    return structs


# C enums without a same-name public Rust enum. Their discriminants are the
# explicit raw i32 mappings in the Rust parsing code, not native enum values.
RAW_I32_ENUMS = {
    "LanceBlobHandling": [
        ("LANCE_BLOB_HANDLING_BLOBS_DESCRIPTIONS", 0),
        ("LANCE_BLOB_HANDLING_ALL_BINARY", 1),
        ("LANCE_BLOB_HANDLING_ALL_DESCRIPTIONS", 2),
    ],
    "LanceBlobReadOrder": [
        ("LANCE_BLOB_READ_PRESERVE_ORDER", 0),
        ("LANCE_BLOB_READ_UNORDERED", 1),
    ],
    "LanceBlobRowSelector": [
        ("LANCE_BLOB_ROWS_NONE", 0),
        ("LANCE_BLOB_ROWS_IDS", 1),
        ("LANCE_BLOB_ROWS_ADDRESSES", 2),
        ("LANCE_BLOB_ROWS_INDICES", 3),
    ],
    "LanceCleanupFileKind": [
        ("LANCE_CLEANUP_FILE_MANIFEST", 0),
        ("LANCE_CLEANUP_FILE_DATA", 1),
        ("LANCE_CLEANUP_FILE_TRANSACTION", 2),
        ("LANCE_CLEANUP_FILE_INDEX", 3),
        ("LANCE_CLEANUP_FILE_DELETION", 4),
        ("LANCE_CLEANUP_FILE_TEMPORARY_MANIFEST", 5),
    ],
    "LanceDataOverlayCoverageKind": [
        ("LANCE_DATA_OVERLAY_COVERAGE_SHARED", 0),
        ("LANCE_DATA_OVERLAY_COVERAGE_PER_FIELD", 1),
    ],
    "LanceExternalBlobMode": [
        ("LANCE_EXTERNAL_BLOB_REFERENCE", 0),
        ("LANCE_EXTERNAL_BLOB_INGEST", 1),
    ],
    "LanceFieldReferenceKind": [
        ("LANCE_FIELD_REFERENCE_ID", 0),
        ("LANCE_FIELD_REFERENCE_PATH", 1),
    ],
    "LanceMemWalDistanceType": [
        ("LANCE_MEM_WAL_DISTANCE_L2", 0),
        ("LANCE_MEM_WAL_DISTANCE_COSINE", 1),
        ("LANCE_MEM_WAL_DISTANCE_DOT", 2),
        ("LANCE_MEM_WAL_DISTANCE_HAMMING", 3),
    ],
    "LanceMemWalShardingMode": [
        ("LANCE_MEM_WAL_SHARDING_MANUAL", 0),
        ("LANCE_MEM_WAL_SHARDING_UNSHARDED", 1),
        ("LANCE_MEM_WAL_SHARDING_BUCKET", 2),
        ("LANCE_MEM_WAL_SHARDING_IDENTITY", 3),
    ],
}


def emit_c_probe(
    enums: dict[str, list[tuple[str, int]]], structs: dict[str, list[str]]
) -> None:
    lines = [
        "// SPDX-License-Identifier: Apache-2.0",
        "// SPDX-FileCopyrightText: Copyright The Lance Authors",
        "//",
        "// Generated by tests/tools/generate_abi_layout_probe.py.",
        "// Validation-only C11 probe for issue #102.",
        "",
        "#include <stddef.h>",
        "#include <stdio.h>",
        "",
        '#include "lance/lance.h"',
        "",
        "#define PRINT_ENUM(type, variant) \\",
        '    printf("E|%s|%s|%d\\n", #type, #variant, (int)(variant))',
        "",
        "#define PRINT_TYPE(type) \\",
        '    printf("T|%s|%zu|%zu\\n", #type, sizeof(type), _Alignof(type))',
        "",
        "#define PRINT_FIELD(type, field) \\",
        '    printf("F|%s.%s|%zu\\n", #type, #field, offsetof(type, field))',
        "",
        "int main(void) {",
    ]
    for name, values in enums.items():
        lines.append(f"    /* {name} */")
        for variant, _ in values:
            lines.append(f"    PRINT_ENUM({name}, {variant});")
        lines.append("")
    for name, fields in structs.items():
        lines.append(f"    /* {name} */")
        lines.append(f"    PRINT_TYPE({name});")
        for field in fields:
            lines.append(f"    PRINT_FIELD({name}, {field});")
        lines.append("")
    lines.extend(["    return 0;", "}", ""])
    C_PROBE.write_text("\n".join(lines))


def emit_cpp_probe() -> None:
    CPP_PROBE.write_text(
        "\n".join(
            [
                "// SPDX-License-Identifier: Apache-2.0",
                "// SPDX-FileCopyrightText: Copyright The Lance Authors",
                "//",
                "// Generated by tests/tools/generate_abi_layout_probe.py.",
                "// Validation-only C++17 header compile probe for issue #102.",
                "",
                '#include "lance/lance.hpp"',
                "",
                "int main() { return 0; }",
                "",
            ]
        )
    )


def emit_rust_test(
    c_enums: dict[str, list[tuple[str, int]]],
    c_structs: dict[str, list[str]],
    rust_enums: dict[str, list[tuple[str, int]]],
    rust_structs: dict[str, list[str]],
) -> None:
    lines = [
        "// SPDX-License-Identifier: Apache-2.0",
        "// SPDX-FileCopyrightText: Copyright The Lance Authors",
        "//",
        "// Generated by tests/tools/generate_abi_layout_probe.py.",
        "// Validation-only current-main ABI comparison for issue #102.",
        "",
        "use std::collections::BTreeSet;",
        "use std::mem;",
        "use std::path::{Path, PathBuf};",
        "use std::process::Command;",
        "",
        "macro_rules! record_enum {",
        "    ($records:ident, $type:ty, $variant:ident, $c_type:literal, $c_name:literal) => {",
        "        $records.insert(format!(",
        '            "E|{}|{}|{}",',
        "            $c_type,",
        "            $c_name,",
        "            <$type>::$variant as i32,",
        "        ));",
        "    };",
        "}",
        "",
        "macro_rules! record_expected_enum {",
        "    ($records:ident, $type:literal, $c_name:literal, $value:expr) => {",
        "        $records.insert(format!(",
        '            "E|{}|{}|{}",',
        "            $type, $c_name, $value,",
        "        ));",
        "    };",
        "}",
        "",
        "macro_rules! record_type {",
        "    ($records:ident, $type:ty) => {",
        "        $records.insert(format!(",
        '            "T|{}|{}|{}",',
        '            stringify!($type).rsplit("::").next().unwrap(),',
        "            mem::size_of::<$type>(),",
        "            mem::align_of::<$type>(),",
        "        ));",
        "    };",
        "}",
        "",
        "macro_rules! record_field {",
        "    ($records:ident, $type:ty, $field:ident) => {",
        "        $records.insert(format!(",
        '            "F|{}.{}|{}",',
        '            stringify!($type).rsplit("::").next().unwrap(),',
        "            stringify!($field),",
        "            mem::offset_of!($type, $field),",
        "        ));",
        "    };",
        "}",
        "",
        "macro_rules! record_struct {",
        "    ($records:ident, $type:ty, [$($field:ident),+ $(,)?]) => {",
        "        record_type!($records, $type);",
        "        $(record_field!($records, $type, $field);)+",
        "    };",
        "}",
        "",
        "fn compile_probe(source: &Path, output: &Path, include_dir: &Path, cpp: bool) -> bool {",
        '    let compiler = if cpp { "c++" } else { "clang" };',
        "    let std = if cpp { \"-std=c++17\" } else { \"-std=c11\" };",
        "    Command::new(compiler)",
        "        .args([std, \"-Wall\", \"-Wextra\", \"-Werror\", \"-o\"])",
        "        .arg(output)",
        "        .arg(source)",
        "        .arg(format!(\"-I{}\", include_dir.display()))",
        "        .status()",
        "        .expect(\"C/C++ compiler is required for this ignored test\")",
        "        .success()",
        "}",
        "",
        "#[test]",
        "#[ignore = \"requires clang and c++; run with: cargo test --test abi_layout_current_test -- --ignored\"]",
        "fn current_main_c_and_rust_abi_match() {",
        "    let manifest_dir = PathBuf::from(env!(\"CARGO_MANIFEST_DIR\"));",
        "    let include_dir = manifest_dir.join(\"include\");",
        "    let c_source = manifest_dir.join(\"tests\").join(\"cpp\").join(\"abi_layout_current.c\");",
        "    let cpp_source = manifest_dir.join(\"tests\").join(\"cpp\").join(\"abi_layout_current.cpp\");",
        "    let build_dir = tempfile::tempdir().unwrap();",
        "    let c_binary = build_dir.path().join(\"abi_layout_current\");",
        "    let cpp_binary = build_dir.path().join(\"abi_layout_current_cpp\");",
        "",
        "    assert!(",
        "        compile_probe(&c_source, &c_binary, &include_dir, false),",
        '        "C11 ABI probe compilation failed"',
        "    );",
        "    assert!(",
        "        compile_probe(&cpp_source, &cpp_binary, &include_dir, true),",
        '        "C++17 header compilation failed"',
        "    );",
        "",
        "    let output = Command::new(&c_binary)",
        "        .output()",
        '        .expect("failed to run C11 ABI probe");',
        "    assert!(",
        "        output.status.success(),",
        '        "C11 ABI probe failed: {}",',
        "        String::from_utf8_lossy(&output.stderr)",
        "    );",
        "    let c_records = String::from_utf8(output.stdout)",
        '        .expect("C ABI probe output must be UTF-8")',
        "        .lines()",
        "        .map(str::to_owned)",
        "        .collect::<BTreeSet<_>>();",
        "",
        "    let mut rust_records = BTreeSet::new();",
        "",
    ]

    for c_name, c_values in c_enums.items():
        if c_name in RAW_I32_ENUMS:
            continue
        rust_name = (
            "IndexBuildProgressEvent"
            if c_name == "LanceIndexBuildProgressEvent"
            else c_name
        )
        if rust_name not in rust_enums:
            raise SystemExit(f"no Rust enum for {c_name}")
        by_value: dict[int, list[str]] = {}
        for variant, value in rust_enums[rust_name]:
            by_value.setdefault(value, []).append(variant)
        lines.append(f"    // {c_name}")
        for c_variant, value in c_values:
            variants = by_value.get(value)
            if not variants:
                raise SystemExit(
                    f"no Rust variant {rust_name} with value {value} for {c_variant}"
                )
            if len(variants) != 1:
                raise SystemExit(
                    f"ambiguous Rust variants {rust_name} value {value}: {variants}"
                )
            lines.append(
                f'    record_enum!(rust_records, lance_c::{rust_name}, {variants[0]}, "{c_name}", "{c_variant}");'
            )
        lines.append("")

    lines.append("    // C enums whose values are explicit raw i32 mappings in Rust.")
    for c_name, expected in RAW_I32_ENUMS.items():
        lines.append(f"    // {c_name}")
        for variant, value in expected:
            lines.append(
                f'    record_expected_enum!(rust_records, "{c_name}", "{variant}", {value});'
            )
        lines.append("")

    for c_name, fields in c_structs.items():
        if c_name not in rust_structs:
            raise SystemExit(f"no Rust struct for {c_name}")
        if fields != rust_structs[c_name]:
            raise SystemExit(
                f"field mismatch for {c_name}: C={fields} Rust={rust_structs[c_name]}"
            )
        field_list = ", ".join(fields)
        lines.append(
            f"    record_struct!(rust_records, lance_c::{c_name}, [{field_list}]);"
        )

    lines.extend(
        [
            "",
            "    let c_only = c_records.difference(&rust_records).collect::<Vec<_>>();",
            "    let rust_only = rust_records.difference(&c_records).collect::<Vec<_>>();",
            "    assert!(",
            "        c_only.is_empty() && rust_only.is_empty(),",
            '        "public C and Rust ABI layouts or discriminants diverged\\nC-only: {c_only:?}\\nRust-only: {rust_only:?}"',
            "    );",
            "}",
            "",
        ]
    )
    RUST_TEST.write_text("\n".join(lines))


def main() -> None:
    header = HEADER.read_text()
    c_enums = parse_c_enums(header)
    c_structs = parse_c_structs(header)
    rust_enums = parse_rust_enums()
    rust_structs = parse_rust_structs()

    missing = sorted(set(RAW_I32_ENUMS) - set(c_enums))
    if missing:
        raise SystemExit(f"raw-i32 enum table is stale: {missing}")
    mapped_rust_names = set(rust_enums) | {"LanceIndexBuildProgressEvent"}
    unexpected = sorted(set(c_enums) - mapped_rust_names - set(RAW_I32_ENUMS))
    if unexpected:
        raise SystemExit(f"C enums without a Rust mapping: {unexpected}")

    emit_c_probe(c_enums, c_structs)
    emit_cpp_probe()
    emit_rust_test(c_enums, c_structs, rust_enums, rust_structs)
    print(
        f"generated {len(c_enums)} enums and {len(c_structs)} structs; "
        f"{len(RAW_I32_ENUMS)} raw-i32 enum tables"
    )


if __name__ == "__main__":
    main()
