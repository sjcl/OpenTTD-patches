#!/usr/bin/env python3
"""Extract the service-index benchmark CSV and summarize seven paired repetitions.

Usage: python utils/passenger_service_index_benchmark.py LOG --csv OUTPUT.csv
Summary JSON is written to stdout. Timings without allocation tracking have the
suffix _untracked; allocation counters come from the corresponding tracked pass.
"""

import argparse
import collections
import csv
import io
import json
from pathlib import Path
import statistics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--csv", type=Path)
    args = parser.parse_args()
    lines = args.log.read_text(encoding="utf-8").splitlines()
    raw = "\n".join(line.removeprefix("INDEX_CSV,") for line in lines if line.startswith("INDEX_CSV,")) + "\n"
    rows = list(csv.DictReader(io.StringIO(raw)))
    if not rows:
        parser.error("No INDEX_CSV rows found")
    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        args.csv.write_text(raw, encoding="utf-8")
    captures = {}
    for line in lines:
        if line.startswith("INDEX_CAPTURE_CSV,"):
            _, fixture, calls, allocated, retained, peak = line.split(",")
            captures[fixture] = dict(allocations=int(calls), allocated_bytes=int(allocated), retained_bytes=int(retained), peak_bytes=int(peak))
    groups = collections.defaultdict(list)
    for row in rows:
        groups[row["fixture"], row["cache"], row["algorithm"], row["stage"]].append(row)
    summary = []
    for (fixture, cache, algorithm, stage), values in sorted(groups.items()):
        assert len(values) == 7, (fixture, cache, algorithm, stage, len(values))
        times = [int(row["ns"]) for row in values]
        entry = dict(fixture=fixture, cache=cache, algorithm=algorithm, stage=stage,
                     median_ns=statistics.median(times), min_ns=min(times), max_ns=max(times))
        for key in ("allocations", "allocated_bytes", "retained_bytes", "peak_bytes"):
            observed = {int(row[key]) for row in values}
            assert len(observed) == 1, (fixture, cache, algorithm, stage, key, observed)
            entry[key] = observed.pop()
        summary.append(entry)
    print(json.dumps(dict(common_capture=captures, stages=summary), indent=2))


if __name__ == "__main__":
    main()
