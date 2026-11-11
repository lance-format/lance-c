#!/usr/bin/env python3
"""Validate the raw matched-offsets benchmark and write a summary."""

from __future__ import annotations

import csv
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path


FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
MASK = (1 << 64) - 1
EXPECTED_COUNTS = {4096, 16384, 32768}
SAMPLES_PER_WORKFLOW = 3


def expected_hash(count: int) -> str:
    value = FNV_OFFSET
    for offset in range(count):
        value ^= offset
        value = (value * FNV_PRIME) & MASK
    return f"{value:016x}"


def median(rows: list[dict[str, str]], key: str) -> int | float:
    return statistics.median(int(row[key]) for row in rows)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: summarize_matched_offsets.py RAW_CSV OUT_JSON")

    raw_path = Path(sys.argv[1])
    out_path = Path(sys.argv[2])
    rows = list(csv.DictReader(raw_path.open(newline="")))
    if not rows:
        raise SystemExit("no benchmark rows")

    groups: dict[tuple[int, str], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        workflow = row["workflow"]
        count = int(row["count"])
        sample = int(row["sample"])
        assert workflow in {"old", "new"}
        assert count in EXPECTED_COUNTS
        assert 1 <= sample <= SAMPLES_PER_WORKFLOW
        assert int(row["capacity"]) == count
        assert int(row["nanos"]) > 0
        assert int(row["alloc_calls"]) >= 0
        assert int(row["alloc_bytes"]) >= 0
        assert row["hash"] == expected_hash(count)
        groups[(count, workflow)].append(row)

    for count in sorted(EXPECTED_COUNTS):
        hashes = set()
        for workflow in ("old", "new"):
            group = groups.get((count, workflow), [])
            assert len(group) == SAMPLES_PER_WORKFLOW, (
                count,
                workflow,
                len(group),
            )
            assert {int(row["sample"]) for row in group} == {1, 2, 3}
            hashes.update(row["hash"] for row in group)
        assert hashes == {expected_hash(count)}

    comparisons = []
    for count in sorted(EXPECTED_COUNTS):
        old_rows = groups[(count, "old")]
        new_rows = groups[(count, "new")]
        old_median = int(median(old_rows, "nanos"))
        new_median = int(median(new_rows, "nanos"))
        comparisons.append(
            {
                "matched_offsets": count,
                "old_header": {
                    "median_ns": old_median,
                    "median_alloc_calls": median(old_rows, "alloc_calls"),
                    "median_alloc_bytes": median(old_rows, "alloc_bytes"),
                    "median_capacity": median(old_rows, "capacity"),
                },
                "new_header": {
                    "median_ns": new_median,
                    "median_alloc_calls": median(new_rows, "alloc_calls"),
                    "median_alloc_bytes": median(new_rows, "alloc_bytes"),
                    "median_capacity": median(new_rows, "capacity"),
                },
                "old_over_new_time": old_median / new_median,
                "time_reduction_percent": (old_median - new_median)
                / old_median
                * 100,
                "results_equal": True,
            }
        )

    summary = {
        "raw_csv": str(raw_path),
        "samples_per_workflow": SAMPLES_PER_WORKFLOW,
        "workflows": ["old_header_native_indexed", "new_header_bulk"],
        "comparisons": comparisons,
        "all_results_equal": True,
    }
    with out_path.open("w") as output:
        json.dump(summary, output, indent=2)
        output.write("\n")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
