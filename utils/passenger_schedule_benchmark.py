#!/usr/bin/env python3
"""Collect real transport, inventory and timing from paired periodic/schedule runs."""
import argparse
import hashlib
import json
import pathlib
import re


def numbers(line):
    return {key: int(value) for key, value in re.findall(r"\b(\w+)=(-?\d+)", line)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("labels", nargs="+")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    runs = []
    for label in args.labels:
        source = args.run / f"{label}-console.txt"
        lines = source.read_text(encoding="utf-8", errors="replace").splitlines()
        run = dict(label=label, **json.loads((args.run / f"{label}-process.json").read_text(encoding="utf-8-sig")))
        for name, prefix in {
            "simulation": "Simulation tick time:", "tick_histogram": "Tick histogram",
            "timed": "Journey transport:", "schedule": "Schedule transport:",
            "untimed": "Untimed transport:", "schedule_routing": "Schedule routing:",
            "periodic": "Periodic database:", "inventory": "Passenger inventory:",
            "schedule_inventory": "Schedule inventory:", "waiting": "Waiting routing:",
			"completed_interchanges": "Completed interchanges:",
			"schedule_assisted": "Schedule-assisted final OD:",
        }.items():
            selected = [line for line in lines if line.startswith(prefix)]
            run[name] = numbers(selected[-1]) if selected else {}
        ticks = [int(n) for n in re.findall(r"State ticks: (\d+)", "\n".join(lines))]
        run.update(initial_tick=ticks[0], final_tick=ticks[-1])
        run["samples"] = [{"simulation": numbers(line)} for line in lines if line.startswith("Simulation tick time:")]
        routing = run["schedule_routing"]
        if routing:
            denominator = routing["hits"] + routing["misses"]
            routing["hit_fraction"] = routing["hits"] / denominator if denominator else None
        stderr = args.run / f"{label}-stderr.txt"
        data = stderr.read_bytes()
        warning_lines = data.decode("utf-8", errors="replace").splitlines()
        run["stderr"] = dict(sha256=hashlib.sha256(data).hexdigest(), bytes=len(data), lines=len(warning_lines))
        excerpt = "\n".join(warning_lines[:12] + ["... repeated diagnostics omitted ..."] + warning_lines[-12:]) + "\n"
        (args.output / f"{label}-stderr-excerpt.txt").write_text(excerpt, encoding="utf-8")
        (args.output / source.name).write_bytes(source.read_bytes())
        (args.output / f"{label}-process.json").write_text(json.dumps(run, indent=2) + "\n", encoding="utf-8")
        runs.append(run)
    (args.output / "summary.json").write_text(json.dumps(runs, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(runs, indent=2))


if __name__ == "__main__":
    main()
