#!/usr/bin/env python3
"""Summarize a frozen-map Hyperpath diagnostic without changing its raw log."""
import argparse
import csv
from fractions import Fraction
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    histogram, ods, build = {}, {}, {}
    for row in csv.reader(args.log.read_text(encoding="utf-8-sig").splitlines()):
        if not row:
            continue
        if row[0] == "hyperpath_build":
            names = "milliseconds operations envelope_bytes partial_array_bytes board_events accepts range_updates heap_pushes heap_pops base_policy_bytes difference_policy_bytes max_candidates".split()
            build = dict(zip(names, map(int, row[1:])))
        elif row[0] == "hyperpath_histogram":
            histogram[int(row[1])] = int(row[2])
        elif row[0] == "hyperpath_od":
            ods[tuple(map(int, row[1:3]))] = dict(legacy_cost=int(row[3]), cost=int(row[4]), candidates=[])
        elif row[0] == "hyperpath_candidate":
            names = "occurrence service order alight x wait frequency".split()
            ods[tuple(map(int, row[1:3]))]["candidates"].append(dict(zip(names, map(int, row[3:]))))
    total = sum(histogram.values())
    numerator = sum(size * count for size, count in histogram.items())
    cumulative = 0
    p95 = 0
    for size, count in sorted(histogram.items()):
        cumulative += count
        if cumulative * 100 >= total * 95:
            p95 = size
            break
    for od in ods.values():
        n, f = 1 << 48, 0
        for candidate in sorted(od["candidates"], key=lambda c: (c["x"], c["occurrence"])):
            candidate["u_before"] = str(Fraction(n, f)) if f else "infinity"
            assert not f or candidate["x"] * f < n
            n += candidate["frequency"] * candidate["x"]
            f += candidate["frequency"]
        od["numerator"], od["frequency"] = n, f
        assert (n + f - 1) // f == od["cost"]
    result = dict(raw_sha256=hashlib.sha256(args.log.read_bytes()).hexdigest(), build=build,
                  sizes=dict(states=total, mean=str(Fraction(numerator, total)), p95=p95, maximum=max(histogram)),
                  histogram=histogram, ods={f"{a}->{b}": value for (a, b), value in ods.items()},
                  limitations=["Construction/diagnostic mode only; no B-mode transport measurement.",
                               "partial_array_bytes excludes graph, workspace and allocator overhead.",
                               "The legacy costs were evaluated in this run; its build duration was not recorded."])
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result["sizes"]))


if __name__ == "__main__":
    main()
