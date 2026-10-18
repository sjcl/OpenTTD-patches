#!/usr/bin/env python3
"""Run/resume asymmetric loopback joins using the copied async measurement TUs."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "package", "input", "output", "lang"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--ticks", type=int, default=2048)
    parser.add_argument("--port", type=int, default=17600)
    parser.add_argument("--jobs", type=int, choices=range(1, 5), default=1)
    args = parser.parse_args()
    if args.ticks <= 0:
        parser.error("--ticks must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    binary_sha, input_sha = digest(args.binary), digest(args.input)
    # Local sleeps/failures alter CPU scheduling alone, never simulation Work.
    schedules = (("8-0", 8, 0, 0, 0, 0, 1000, 0),
                 ("0-8", 0, 8, 0, 0, 0, 0, 1000),
                 ("4-1-slow-client", 4, 1, 0, 5000, 0, 1000, 0),
                 ("worker-failure", 4, 4, 0, 0, 1, 1000, 2000),
                 ("slow-host-fast-client", 4, 4, 5000, 0, 0, 0, 1000))
    cases = [(mode, schedule) for mode in ("legacy", "hyperpath") for schedule in schedules]

    def run(index, case):
        mode, schedule = case
        label, host, client, host_delay, client_delay, client_failure, host_headstart, client_headstart = schedule
        name = f"{mode}-{label}"
        output = args.output / name
        path = output / "result.json"
        if path.exists():
            record = json.loads(path.read_text(encoding="utf-8"))
            if (record.get("pass") and record["binary_sha256"] == binary_sha and record["input_sha256"] == input_sha
                    and record["server_workers"] == host and record["client_workers"] == client and record["requested_ticks"] == args.ticks
                    and record["hyperpath"] == (mode == "hyperpath")
                    and record.get("journey_movement_server", {}).get("journey_boarded", 0) > 0
                    and record.get("journey_movement_server", {}).get("journey_delivered", 0) > 0
                    and record["worker_hooks"]["server"] == dict(worker_delay_us=host_delay, worker_fail_work=0, main_headstart_us=host_headstart)
                    and record["worker_hooks"]["client"] == dict(worker_delay_us=client_delay, worker_fail_work=client_failure, main_headstart_us=client_headstart)):
                return name, record
            raise RuntimeError(f"Existing result is incomplete or differs: {path}")
        command = [sys.executable, str(Path(__file__).with_name("passenger_precomputed_join.py")),
                   str(args.binary), str(args.package), str(args.input), str(output), str(args.lang),
                   "--server-workers", str(host), "--client-workers", str(client), "--" + ("hyperpath" if mode == "hyperpath" else "no-hyperpath"),
                   "--fixture-timetable", "--initial-build", "--toggle-precomputed", "--replace-precomputed", "--frame-freq", "1",
                   "--ticks", str(args.ticks), "--port", str(args.port + index), "--require-journey-movement",
                   "--server-worker-delay-us", str(host_delay), "--client-worker-delay-us", str(client_delay),
                   "--client-worker-fail-work", str(client_failure), "--server-main-headstart-us", str(host_headstart),
                   "--client-main-headstart-us", str(client_headstart)]
        with (args.output / f"{name}.log").open("wb") as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
        record = json.loads(path.read_text(encoding="utf-8"))
        if result.returncode or not record["pass"]:
            raise RuntimeError(f"Loopback failed: {path}: {record.get('error', '')}")
        return name, record

    results, failures = {}, {}
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        pending = {pool.submit(run, index, case): case for index, case in enumerate(cases)}
        for future in as_completed(pending):
            case = pending[future]
            try:
                name, record = future.result()
                results[name] = record
                print(f"PASS {name}: ticks={record['actual_ticks']}, movement={record['journey_movement_server']}", flush=True)
            except Exception as error:
                failures[f"{case[0]}-{case[1][0]}"] = str(error)
                print(str(error), flush=True)
            summary = dict(binary_sha256=binary_sha, input_sha256=input_sha, results=results, failures=failures)
            (args.output / "matrix.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
