#!/usr/bin/env python3
"""Run isolated build comparisons, recording raw counters and process peaks."""
import argparse
import csv
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time
import tempfile
sys.dont_write_bytecode = True
from passenger_precomputed_benchmark import observe

FIELDS = "services occurrences nodes edges graph_ns rows_ns init_ns search_ns alternative_ns publication_ns relaxations improvements queue_updates pops stale max_queue label_copies section_checks array_bytes operations".split()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--fixture", type=pathlib.Path)
    parser.add_argument("--matrix", action="store_true")
    parser.add_argument("--stages", action="store_true")
    parser.add_argument("--experiments", action="store_true")
    parser.add_argument("--skip-plan", action="store_true")
    parser.add_argument("--destinations", type=int, help="Coarse performance probe of a deterministic row prefix; requires --skip-plan")
    parser.add_argument("--test", default="[.table-real]")
    parser.add_argument("--isolated-timing", action="store_true", help="Reserve the existing shared measurement lock and reject competing workloads")
    parser.add_argument("--measurement-wait", type=int, default=60)
    args = parser.parse_args()
    if args.destinations is not None and (args.destinations <= 0 or not args.skip_plan):
        parser.error("--destinations must be positive and requires --skip-plan")
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if args.fixture:
        env["OPENTTD_TABLE_DATABASE"] = str((args.fixture / "database.bin").resolve())
        env["OPENTTD_TABLE_STATIONS"] = str((args.fixture / "stations.txt").resolve())
    if args.matrix:
        env["OPENTTD_TABLE_HEAP_MATRIX"] = "1"
    if args.stages:
        env["OPENTTD_TABLE_STAGES"] = "1"
    if args.experiments:
        env["OPENTTD_TABLE_EXPERIMENTS"] = "1"
    if args.skip_plan:
        env["OPENTTD_TABLE_SKIP_PLAN"] = "1"
    else:
        env.pop("OPENTTD_TABLE_SKIP_PLAN", None)
    env.pop("OPENTTD_TABLE_MAX_DESTINATIONS", None)
    if args.destinations is not None:
        env["OPENTTD_TABLE_MAX_DESTINATIONS"] = str(args.destinations)
    command = [str(args.binary.resolve()), args.test]
    lock, lock_path, profile = None, None, None
    if args.isolated_timing:
        from passenger_precomputed_real_map import PROFILE as profile
        lock_path = pathlib.Path(tempfile.gettempdir()) / "openttd-passenger-routing-measurement.lock"
        lock = profile.acquire_measurement_lock(lock_path, args.measurement_wait)
        os.write(lock, json.dumps(dict(pid=os.getpid(), output=str(args.output.resolve()))).encode())
        try:
            profile.wait_for_idle(60, 5)
        except BaseException:
            os.close(lock)
            lock_path.unlink()
            raise
    started = time.monotonic()
    observed = {}
    contamination, census_at, process = [], 0, None
    try:
        with (args.output / "raw.log").open("wb") as stream:
            process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT, env=env,
                                       creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            own = f"{args.binary.stem}:{process.pid}".lower()
            while process.poll() is None:
                for key, value in observe(process).items():
                    observed[key] = max(observed.get(key, 0), value)
                if profile is not None and time.monotonic() >= census_at:
                    busy = [p for p in profile.competing_processes() if p.lower() != own]
                    if busy:
                        contamination.append(dict(seconds=time.monotonic() - started, processes=busy))
                    census_at = time.monotonic() + 5
                time.sleep(.1)
            observed.update(observe(process))
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait()
        if lock is not None:
            os.close(lock)
            lock_path.unlink()
    rows, details = [], {}
    for row in csv.reader((args.output / "raw.log").read_text(encoding="utf-8", errors="replace").splitlines()):
        if row and row[0] == "table_build":
            rows.append(dict(name=row[1], **dict(zip(FIELDS, map(int, row[2:])))))
        elif row and row[0] in ("heap_stats", "heap_sample", "queue_order", "range_probes", "row_distribution", "memory", "graph_layout", "phases", "packed_bounds", "table_window"):
            details.setdefault(row[1], {})[row[0]] = list(map(int, row[2:]))
    result = dict(command=command, binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(), exit_code=process.returncode,
                  environment={key: value for key, value in env.items() if key.startswith("OPENTTD_TABLE_")},
                  fixture_sha256={name: hashlib.sha256((args.fixture / name).read_bytes()).hexdigest() for name in ("database.bin", "stations.txt")} if args.fixture else None,
                  elapsed_seconds=time.monotonic() - started, skip_plan=args.skip_plan, destination_limit=args.destinations, rows=rows, details=details, **observed,
                  isolated_timing=args.isolated_timing, contamination=contamination,
                  performance_valid=args.isolated_timing and not contamination and process.returncode == 0,
                  limitations=["Array residency excludes allocator overhead; OS peak includes both compared tables and the map.", "This runner does not exclude unrelated host activity."])
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    raise SystemExit(process.returncode or bool(contamination))


if __name__ == "__main__":
    main()
