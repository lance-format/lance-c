#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

set -euo pipefail
export LC_ALL=C

if [ "$#" -ne 6 ]; then
    echo "usage: $0 <archive> <dylib> <header> <architecture> <max-minos> <output-dir>" >&2
    exit 2
fi

archive=$1
dylib=$2
header=$3
expected_architecture=$4
expected_max_minos=$5
output_dir=$6

for file in "$archive" "$dylib" "$header"; do
    if [ ! -f "$file" ]; then
        echo "required artifact is missing: $file" >&2
        exit 1
    fi
done

mkdir -p "$output_dir"
members_tsv="$output_dir/archive-members.tsv"
dylib_build_txt="$output_dir/dylib-build.txt"
audit_json="$output_dir/archive-audit.json"
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/lance-c-macos-audit.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT

parse_macos_build() {
    local binary=$1
    local info

    info=$(xcrun vtool -show-build "$binary" 2>/dev/null || true)
    if ! grep -q '^minos ' <<<"$info"; then
        info=$(otool -l "$binary" 2>/dev/null | awk '
            $1 == "cmd" && $2 == "LC_BUILD_VERSION" {
                mode = 1
                next
            }
            $1 == "cmd" && $2 == "LC_VERSION_MIN_MACOSX" {
                mode = 2
                next
            }
            mode == 1 && $1 == "platform" {
                print "platform " $2
                next
            }
            mode == 1 && $1 == "minos" {
                print "minos " $2
                next
            }
            mode == 1 && $1 == "sdk" {
                print "sdk " $2
                mode = 0
                next
            }
            mode == 2 && $1 == "version" {
                print "platform 1"
                print "minos " $2
                next
            }
            mode == 2 && $1 == "sdk" {
                print "sdk " $2
                mode = 0
            }
        ')
    fi

    printf '%s\n' "$info"
}

printf 'sequence\tmember\tsha256\tfile_type\tarchitectures\tplatform\tsdk\tminos\n' \
    >"$members_tsv"

entry_count=$(ar -t "$archive" | wc -l | tr -d ' ')
total=0
macho=0
non_macho=0

while IFS= read -r member; do
    case "$member" in
        '' | __.SYMDEF*) continue ;;
    esac

    total=$((total + 1))
    sequence=$(printf '%06d' "$total")
    member_file="$work_dir/member"
    if ! ar -p "$archive" "$member" >"$member_file"; then
        echo "failed to extract archive member: $member" >&2
        exit 1
    fi

    member_sha256=$(shasum -a 256 "$member_file" | awk '{print $1}')
    file_type=$(file -b "$member_file" | tr '\t\n' '  ')
    if [[ "$file_type" == *Mach-O* ]]; then
        macho=$((macho + 1))
        architectures=$(lipo -archs "$member_file" 2>/dev/null || true)
        build_info=$(parse_macos_build "$member_file")
        platform=$(awk '$1 == "platform" {print $2; exit}' <<<"$build_info")
        sdk=$(awk '$1 == "sdk" {print $2; exit}' <<<"$build_info")
        minos=$(awk '$1 == "minos" {print $2; exit}' <<<"$build_info")
    else
        non_macho=$((non_macho + 1))
        architectures=
        platform=
        sdk=
        minos=
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$sequence" "$member" "$member_sha256" "$file_type" \
        "$architectures" "$platform" "$sdk" "$minos" >>"$members_tsv"

    if [ $((total % 500)) -eq 0 ]; then
        echo "audited $total archive members" >&2
    fi
done < <(ar -t "$archive")

if [ "$total" -eq 0 ]; then
    echo "static archive contains no object members: $archive" >&2
    exit 1
fi

parse_macos_build "$dylib" >"$dylib_build_txt"
file "$dylib" >"$output_dir/dylib-file.txt"
lipo -info "$dylib" >"$output_dir/dylib-lipo.txt"
otool -D "$dylib" >"$output_dir/dylib-install-name.txt"
otool -L "$dylib" >"$output_dir/dylib-dependencies.txt"

python3 - \
    "$archive" "$dylib" "$header" "$members_tsv" "$dylib_build_txt" \
    "$audit_json" "$expected_architecture" "$expected_max_minos" \
    "$entry_count" <<'PY'
import hashlib
import json
import os
import subprocess
import sys
from collections import Counter

(
    archive,
    dylib,
    header,
    members_tsv,
    dylib_build_txt,
    audit_json,
    expected_architecture,
    expected_max_minos,
    entry_count,
) = sys.argv[1:]


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def version_tuple(value):
    return tuple(int(component) for component in value.split("."))


def normalized_platform(value):
    if value in {"1", "MACOS", "macos"}:
        return "MACOS"
    return value


with open(dylib_build_txt, encoding="utf-8") as handle:
    dylib_info = {}
    for line in handle:
        fields = line.split()
        if fields and fields[0] in {"platform", "minos", "sdk"}:
            dylib_info[fields[0]] = fields[1]

dylib_architectures = subprocess.check_output(
    ["lipo", "-archs", dylib], text=True
).strip().split()
if dylib_architectures != [expected_architecture]:
    raise SystemExit(
        f"dylib architectures {dylib_architectures} do not match "
        f"{expected_architecture}"
    )

dylib_platform = normalized_platform(dylib_info.get("platform", ""))
if dylib_platform != "MACOS":
    raise SystemExit(f"unexpected dylib platform: {dylib_platform!r}")

dylib_minos = dylib_info.get("minos")
if not dylib_minos:
    raise SystemExit("dylib has no deployment-target metadata")
if version_tuple(dylib_minos) > version_tuple(expected_max_minos):
    raise SystemExit(
        f"dylib minos {dylib_minos} exceeds {expected_max_minos}"
    )

install_name_output = subprocess.check_output(
    ["otool", "-D", dylib], text=True
).splitlines()
install_name = (
    install_name_output[1].strip() if len(install_name_output) > 1 else ""
)
if install_name != "@rpath/liblance_c.dylib":
    raise SystemExit(f"unexpected dylib install name: {install_name!r}")

dependency_output = subprocess.check_output(
    ["otool", "-L", dylib], text=True
).splitlines()
dependencies = [
    line.strip().split()[0]
    for line in dependency_output[1:]
    if line.strip()
]
allowed_dependency_prefixes = (
    "@rpath/liblance_c.dylib",
    "/usr/lib/",
    "/System/Library/",
    "/Library/Apple/",
)
for dependency in dependencies:
    if not dependency.startswith(allowed_dependency_prefixes):
        raise SystemExit(f"unexpected dylib dependency: {dependency}")

rows = []
with open(members_tsv, encoding="utf-8") as handle:
    header_fields = handle.readline().rstrip("\n").split("\t")
    for line in handle:
        fields = line.rstrip("\n").split("\t")
        rows.append(dict(zip(header_fields, fields)))

if not rows:
    raise SystemExit("archive member metadata is empty")

macho_rows = [row for row in rows if "Mach-O" in row["file_type"]]
non_macho_rows = [row for row in rows if "Mach-O" not in row["file_type"]]
if not macho_rows:
    raise SystemExit("archive contains no Mach-O members")
if non_macho_rows:
    names = ", ".join(row["member"] for row in non_macho_rows[:5])
    raise SystemExit(f"archive contains non-Mach-O members: {names}")

for row in macho_rows:
    architectures = row["architectures"].split()
    if architectures != [expected_architecture]:
        raise SystemExit(
            f"archive member {row['member']} has architectures "
            f"{architectures}, expected {expected_architecture}"
        )
    if normalized_platform(row["platform"]) != "MACOS":
        raise SystemExit(
            f"archive member {row['member']} has platform {row['platform']!r}"
        )
    if not row["minos"]:
        raise SystemExit(
            f"archive member {row['member']} has no deployment-target metadata"
        )

minos_distribution = Counter(row["minos"] for row in macho_rows)
sdk_distribution = Counter(row["sdk"] for row in macho_rows)
platform_distribution = Counter(
    normalized_platform(row["platform"]) for row in macho_rows
)
architecture_distribution = Counter(
    row["architectures"] for row in macho_rows
)
members_above_expected = [
    {
        "sequence": row["sequence"],
        "member": row["member"],
        "minos": row["minos"],
    }
    for row in macho_rows
    if version_tuple(row["minos"]) > version_tuple(expected_max_minos)
]
if members_above_expected:
    first = members_above_expected[0]
    raise SystemExit(
        f"{len(members_above_expected)} archive members exceed "
        f"minos {expected_max_minos}; first: {first}"
    )

max_minos = max(macho_rows, key=lambda row: version_tuple(row["minos"]))["minos"]

result = {
    "archive": {
        "file_name": os.path.basename(archive),
        "sha256": sha256(archive),
        "bytes": os.path.getsize(archive),
        "ar_entry_count": int(entry_count),
        "macho_member_count": len(macho_rows),
        "non_macho_member_count": len(non_macho_rows),
        "member_metadata_sha256": sha256(members_tsv),
        "architecture_distribution": dict(sorted(architecture_distribution.items())),
        "platform_distribution": dict(sorted(platform_distribution.items())),
        "sdk_distribution": dict(
            sorted(sdk_distribution.items(), key=lambda item: item[0])
        ),
        "minos_distribution": dict(
            sorted(minos_distribution.items(), key=lambda item: version_tuple(item[0]))
        ),
        "max_minos": max_minos,
        "members_above_expected": members_above_expected,
    },
    "dylib": {
        "file_name": os.path.basename(dylib),
        "sha256": sha256(dylib),
        "bytes": os.path.getsize(dylib),
        "architectures": dylib_architectures,
        "platform": dylib_platform,
        "sdk": dylib_info.get("sdk"),
        "minos": dylib_minos,
        "install_name": install_name,
        "dependencies": dependencies,
    },
    "header": {
        "file_name": os.path.basename(header),
        "sha256": sha256(header),
        "bytes": os.path.getsize(header),
    },
    "expected": {
        "architecture": expected_architecture,
        "max_minos": expected_max_minos,
    },
}

with open(audit_json, "w", encoding="utf-8") as handle:
    json.dump(result, handle, indent=2, sort_keys=True)
    handle.write("\n")

print(
    f"archive: {result['archive']['macho_member_count']} Mach-O "
    f"{expected_architecture} members; max minos {max_minos}"
)
print(f"archive minos distribution: {dict(minos_distribution)}")
print(f"archive sdk distribution: {dict(sdk_distribution)}")
print(
    f"dylib: {expected_architecture}, {dylib_minos}, "
    f"{result['dylib']['install_name']}"
)
print(f"header sha256: {result['header']['sha256']}")
PY
