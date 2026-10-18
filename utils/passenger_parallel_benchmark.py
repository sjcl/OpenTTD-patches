#!/usr/bin/env python3
"""Run each engine/worker count in a fresh process; measure CPU and peak memory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from passenger_precomputed_benchmark import observe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--side", type=int, default=32)
    parser.add_argument("--lines", type=int, default=240)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--scenario", choices=("initial", "natural", "ahead", "late"), default="initial")
    parser.add_argument("--uneven", action="store_true")
    parser.add_argument("--workers", type=int, nargs="+", choices=range(9), default=[0, 1, 2, 4, 8])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    for repetition in range(args.repeat):
        for hyperpath in (0, 1):
            # Rotate the order to reduce a fixed thermal/order advantage.
            counts = args.workers
            counts = counts[repetition % len(counts):] + counts[:repetition % len(counts)]
            for workers in counts:
                name = f"{args.scenario}-{'uneven' if args.uneven else 'uniform'}-{'hyperpath' if hyperpath else 'legacy'}-{workers}-{repetition}"
                env = dict(os.environ, OPENTTD_PARALLEL_WORKERS=str(workers), OPENTTD_PARALLEL_HYPERPATH=str(hyperpath),
                           OPENTTD_PARALLEL_SIDE=str(args.side), OPENTTD_PARALLEL_LINES=str(args.lines),
                           OPENTTD_PARALLEL_UNEVEN=str(int(args.uneven)),
                           OPENTTD_ASYNC_SCENARIO=str({"initial": 0, "natural": 0, "ahead": 1, "late": 2}[args.scenario]))
                case = "[.passenger-parallel-benchmark]" if args.scenario == "initial" else "[.passenger-async-tick-benchmark]"
                command = [str(args.binary.resolve()), case, "-r", "compact"]
                observed = {}
                started = time.perf_counter()
                with (args.output / f"{name}.log").open("wb") as stream:
                    process = subprocess.Popen(command, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                               creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
                    while process.poll() is None:
                        for key, value in observe(process).items():
                            observed[key] = max(observed.get(key, 0), value)
                        time.sleep(0.02)
                    for key, value in observe(process).items():
                        observed[key] = max(observed.get(key, 0), value)
                raw = (args.output / f"{name}.log").read_text(encoding="utf-8", errors="replace")
                row = next((line for line in raw.splitlines() if line.startswith("passenger_parallel,")), None)
                if process.returncode or row is None:
                    raise RuntimeError(f"Failed trial {name}; see its log")
                record = dict(repetition=repetition, process_wall_seconds=time.perf_counter() - started, **observed)
                for pair in row.split(",")[1:]:
                    key, value = pair.split("=", 1)
                    record[key] = value if key == "mode" else int(value)
                results.append(record)
                print(json.dumps(record), flush=True)
                summary = dict(binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(), host=platform.platform(),
                               cpus=os.cpu_count(), side=args.side, lines=args.lines, scenario=args.scenario, uneven=args.uneven, results=results)
                (args.output / "result.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    for mode in ("legacy", "hyperpath"):
        same = {(r["operations"], r["digest"], r.get("database_digest"), r.get("ticks"), r["progress_digest"]) for r in results if r["mode"] == mode}
        if len(same) != 1:
            raise RuntimeError(f"Work or table contents differ for {mode}")


if __name__ == "__main__":
    main()
