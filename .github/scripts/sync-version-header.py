#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

"""Synchronize the public lance-c version macros with a release version.

The release workflow runs this after `cargo set-version`. With --check it only
verifies the header, which is useful before tagging a release manually.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

VERSION_RE = re.compile(
    r"^(?P<major>0|[1-9]\d*)\.(?P<minor>0|[1-9]\d*)\.(?P<patch>0|[1-9]\d*)"
    r"(?:-(?P<pre>[0-9A-Za-z.-]+))?(?:\+(?P<build>[0-9A-Za-z.-]+))?$"
)
MAX_COMPONENT = 999


def parse_version(version: str) -> tuple[int, int, int, str]:
    match = VERSION_RE.fullmatch(version)
    if match is None:
        raise ValueError(f"not a Cargo package version: {version!r}")
    major = int(match.group("major"))
    minor = int(match.group("minor"))
    patch = int(match.group("patch"))
    for name, value in (("major", major), ("minor", minor), ("patch", patch)):
        if value > MAX_COMPONENT:
            raise ValueError(
                f"{name} component {value} exceeds the {MAX_COMPONENT} limit "
                "used by LANCE_C_VERSION"
            )
    return major, minor, patch, match.group("pre") or ""


def replace_macro(text: str, name: str, value: str) -> str:
    pattern = re.compile(rf"^#define[ \t]+{re.escape(name)}[ \t]+.*$", re.MULTILINE)
    matches = pattern.findall(text)
    if len(matches) != 1:
        raise ValueError(f"expected exactly one #define {name}, found {len(matches)}")
    return pattern.sub(lambda _: f"#define {name} {value}", text)


def expected_macros(version: str) -> dict[str, str]:
    major, minor, patch, pre = parse_version(version)
    return {
        "LANCE_C_VERSION_MAJOR": str(major),
        "LANCE_C_VERSION_MINOR": str(minor),
        "LANCE_C_VERSION_PATCH": str(patch),
        "LANCE_C_VERSION_PRE": f'"{pre}"',
        "LANCE_C_VERSION_STRING": f'"{version}"',
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "version",
        nargs="?",
        help="version to write; read from Cargo.toml when omitted with --check",
    )
    parser.add_argument(
        "header",
        nargs="?",
        default="include/lance/lance.h",
        help="header to update (default: include/lance/lance.h)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the header without modifying it",
    )
    args = parser.parse_args()

    header_path = Path(args.header)
    version = args.version
    if version is None:
        if not args.check:
            parser.error("version is required unless --check is used")
        cargo_toml = Path("Cargo.toml")
        match = re.search(
            r'^version = "([^"]+)"$', cargo_toml.read_text(), re.MULTILINE
        )
        if match is None:
            print(f"failed to read the package version from {cargo_toml}", file=sys.stderr)
            return 1
        version = match.group(1)

    try:
        replacements = expected_macros(version)
        original = header_path.read_text()
        updated = original
        for name, value in replacements.items():
            updated = replace_macro(updated, name, value)
    except (OSError, ValueError) as error:
        print(f"version header sync failed: {error}", file=sys.stderr)
        return 1

    if args.check:
        if updated != original:
            print(
                f"{header_path} version macros do not match {version}; "
                "run .github/scripts/sync-version-header.py",
                file=sys.stderr,
            )
            return 1
        print(f"{header_path} version macros match {version}")
        return 0

    header_path.write_text(updated)
    print(f"updated {header_path} to {version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
