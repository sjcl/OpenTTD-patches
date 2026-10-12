#!/usr/bin/env python3
"""Collect paired Trip-cache benchmark observations without touching simulation state."""
import argparse
import csv
import json
import pathlib
import re
import statistics


def numbers(text):
    return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", text)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("synthetic", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--real", type=pathlib.Path)
    args = parser.parse_args()
    rows = []
    for line in args.synthetic.read_text(encoding="utf-8", errors="replace").splitlines():
        if "Trip benchmark: run=" in line:
            rows.append(dict(fixture="synthetic_100_od", **numbers(line)))
        elif "Trip synthetic routing tick: run=" in line:
            rows.append(dict(fixture="synthetic_routing_tick", **numbers(line)))
    if args.real:
        runs = json.loads((args.real / "runs.json").read_text(encoding="utf-8"))
        for run in runs:
            stage = f"run-{run['run']}-{'enabled' if run['enabled'] else 'disabled'}"
            console = (args.real / stage / "console.log").read_text(encoding="utf-8", errors="replace")
            row = dict(fixture="real_simulation", **run)
            for prefix in ("Simulation tick time:", "Active journey total:", "Trip prediction:", "Graph work=", "Train observations="):
                line = [line for line in console.splitlines() if line.startswith(prefix)][-1]
                values = numbers(line)
                if prefix == "Graph work=":
                    for key in ("mean_us", "max_us"):
                        values[f"graph_{key}"] = values.pop(key)
                row.update(values)
                if prefix == "Simulation tick time:":
                    row.update({f"simulation_{key}": value for key, value in numbers(line).items()})
            assigned = re.findall(r"Active journey total: applied=(\d+) packets/(\d+) passengers", console)[-1]
            row.update(applied_packets=int(assigned[0]), applied_passengers=int(assigned[1]))
            ticks = re.findall(r"state_ticks=(\d+)", console)
            row.update(initial_tick=int(ticks[0]), final_tick=int(ticks[-1]))
            rows.append(row)
        for repeat in range(3):
            pair = [row for row in rows if row["fixture"] == "real_simulation" and row["run"] == repeat]
            assert len(pair) == 2
            for key in ("save_sha256", "initial_tick", "final_tick", "completed_ticks", "builds", "searches", "applied_packets", "applied_passengers", "visits", "logical_work", "work", "query_allowance"):
                assert pair[0][key] == pair[1][key], (repeat, key)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "runs.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    columns = sorted({key for row in rows for key in row})
    with (args.output / "runs.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)
    resident = [r for r in rows if r["fixture"] == "synthetic_100_od" and r["cache_ceiling"] == 8388608]
    summary = {"synthetic": {key: statistics.median(r[key] for r in resident) for key in resident[0] if isinstance(resident[0][key], int)}}
    for enabled in (False, True):
        real = [r for r in rows if r["fixture"] == "real_simulation" and r["enabled"] == enabled]
        if real:
            summary[f"real_{'enabled' if enabled else 'disabled'}"] = {key: statistics.median(r[key] for r in real) for key in real[0] if isinstance(real[0][key], (int, float))}
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
