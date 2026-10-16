#!/usr/bin/env python3
"""Summarize cumulative boarding-profile CSV counters over exact tick windows."""
import argparse
import csv
import json
from pathlib import Path


def summarize(directory, midpoint):
    process = json.loads((directory / "process.json").read_text(encoding="utf-8"))
    samples = process["actual-ticks.json"]["samples"]
    with (directory / "boarding-profile.csv").open(encoding="utf-8", newline="") as stream:
        rows = [{key: int(value) for key, value in row.items()} for row in csv.DictReader(stream)]
    if len(rows) != len(samples) or any(row["tick"] != sample[0] for row, sample in zip(rows, samples)):
        raise ValueError("Profile snapshots must match every measured simulation tick")
    if process["exit_code"] or process["contamination"] or process["trace_enabled"] or process.get("departure_diagnostic"):
        raise ValueError("Expected an uncontaminated, successful, untraced run")
    windows = {"all": (0, len(rows))}
    if midpoint:
        if not 0 < midpoint < len(rows):
            raise ValueError("Midpoint outside samples")
        windows.update(ordinary=(0, midpoint), ordinary_tail=(midpoint // 2, midpoint), replacement=(midpoint, len(rows)))
    result = {}
    for name, (first, last) in windows.items():
        previous = rows[first - 1] if first else dict.fromkeys(rows[0], 0)
        values = {key: rows[last - 1][key] - previous[key] for key in rows[0] if key != "tick"}
        if 'capacity_enumeration_ride_calls' in values:
            for suffix in ('calls', 'ns', 'stops'):
                values['boarding_enumeration_ride_' + suffix] = values['enumeration_ride_' + suffix] - values['capacity_enumeration_ride_' + suffix]
                if values['boarding_enumeration_ride_' + suffix] < 0:
                    raise ValueError('Capacity enumeration must be a subset of all enumeration ride work')
        result[name] = dict(ticks=last - first, counters=values,
                            simulation_ns=sum(sample[1] * 1000 for sample in samples[first:last]),
                            journey_ns=sum(sample[2] * 1000 for sample in samples[first:last]))
    return dict(input_sha256=process["input_sha256"], binary_sha256=process["binary_sha256"], windows=result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("runs", type=Path, nargs="+")
    parser.add_argument("--midpoint", type=int, default=1024)
    args = parser.parse_args()
    runs = {directory.name: summarize(directory, args.midpoint) for directory in args.runs}
    report = dict(runs=runs, note="Nested enumeration/predicate/ride times overlap. Timers add observer overhead; this is attribution, not an uninstrumented CPU comparison.")
    if "legacy" in runs and "hyperpath" in runs:
        if runs["legacy"]["input_sha256"] != runs["hyperpath"]["input_sha256"] or runs["legacy"]["binary_sha256"] != runs["hyperpath"]["binary_sha256"]:
            raise ValueError("Attribution comparison requires the same input and executable")
        report["attribution"] = {}
        for name, window in runs["hyperpath"]["windows"].items():
            old = runs["legacy"]["windows"][name]
            increase = window["simulation_ns"] - old["simulation_ns"]
            enumeration = window["counters"]["enumeration_ns"] - old["counters"]["enumeration_ns"]
            report["attribution"][name] = dict(simulation_increase_ns=increase, enumeration_increase_ns=enumeration,
                                                fraction=enumeration / increase if increase > 0 else None)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
