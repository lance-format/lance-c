#!/usr/bin/env python3
"""Validate issue-209 raw offsets benchmark and write a summary."""

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
COUNTS = {4096, 16384, 32768}
SAMPLES = 5


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
        raise SystemExit("usage: summarize_benchmark.py RAW_CSV OUT_JSON")

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
        assert count in COUNTS
        assert 1 <= sample <= SAMPLES
        assert int(row["capacity"]) == count
        assert int(row["nanos"]) > 0
        assert int(row["alloc_calls"]) == 1
        assert int(row["alloc_bytes"]) == count * 4
        assert row["hash"] == expected_hash(count)
        groups[(count, workflow)].append(row)

    for count in sorted(COUNTS):
        hashes = set()
        for workflow in ("old", "new"):
            group = groups.get((count, workflow), [])
            assert len(group) == SAMPLES, (count, workflow, len(group))
            assert {int(row["sample"]) for row in group} == set(
                range(1, SAMPLES + 1)
            )
            hashes.update(row["hash"] for row in group)
        assert hashes == {expected_hash(count)}

    comparisons = []
    for count in sorted(COUNTS):
        old_rows = groups[(count, "old")]
        new_rows = groups[(count, "new")]
        old_median = int(median(old_rows, "nanos"))
        new_median = int(median(new_rows, "nanos"))
        comparisons.append(
            {
                "offsets": count,
                "old": {
                    "median_ns": old_median,
                    "min_ns": min(int(row["nanos"]) for row in old_rows),
                    "max_ns": max(int(row["nanos"]) for row in old_rows),
                    "median_alloc_calls": median(old_rows, "alloc_calls"),
                    "median_alloc_bytes": median(old_rows, "alloc_bytes"),
                    "median_capacity": median(old_rows, "capacity"),
                },
                "new": {
                    "median_ns": new_median,
                    "min_ns": min(int(row["nanos"]) for row in new_rows),
                    "max_ns": max(int(row["nanos"]) for row in new_rows),
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
        "samples_per_workflow": SAMPLES,
        "workflows": ["old_header_indexed_loop", "new_header_single_copy"],
        "comparisons": comparisons,
        "all_results_equal": True,
        "allocation_strategy_equal": True,
    }
    with out_path.open("w") as output:
        json.dump(summary, output, indent=2)
        output.write("\n")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
