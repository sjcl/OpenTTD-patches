#!/usr/bin/env python3
"""Run the native synthetic preparation/replay comparisons in fresh processes.

No real saves or WPR are used. Run after functional tests and other builds finish.
The nonblocking initial-construction reference and serial replay reference retain
the baseline algorithms. Save/row digests are checked by the native tests as well.
"""
import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def competing_processes(excluded_pid=0):
    """Reject concurrent builds/tests/games; never stop somebody else's process."""
    if os.name != "nt":
        return []
    class Process(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD), ("pid", wintypes.DWORD),
                    ("heap", ctypes.c_size_t), ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                    ("parent", wintypes.DWORD), ("priority", wintypes.LONG), ("flags", wintypes.DWORD), ("exe", wintypes.WCHAR * 260)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Process)]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    names = {"openttd.exe", "openttd_test.exe", "msbuild.exe", "ninja.exe", "clang-cl.exe", "cl.exe", "lld-link.exe", "link.exe", "wpaexporter.exe"}
    entry = Process()
    entry.size = ctypes.sizeof(entry)
    found = []
    try:
        valid = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while valid:
            if entry.pid != excluded_pid and entry.exe.lower() in names:
                handle = kernel.OpenProcess(0x1000, False, entry.pid)
                if handle:
                    status = wintypes.DWORD()
                    try:
                        if kernel.GetExitCodeProcess(handle, ctypes.byref(status)) and status.value == 259:
                            found.append({"pid": entry.pid, "exe": entry.exe})
                    finally:
                        kernel.CloseHandle(handle)
            valid = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    return found


def peak_memory(pid):
    if os.name != "nt":
        return 0
    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in ("peak", "working", "pool_peak", "pool", "nonpool_peak", "nonpool", "pagefile", "pagefile_peak")]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    handle = kernel.OpenProcess(0x0400 | 0x0010, False, pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    try:
        if not kernel.K32GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            raise ctypes.WinError(ctypes.get_last_error())
        return counters.peak
    finally:
        kernel.CloseHandle(handle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    parser.add_argument("output", type=Path, help="New evidence directory")
    parser.add_argument("--side", type=int, default=16)
    parser.add_argument("--lines", type=int, default=100)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--workers", default="0,1,4,8")
    parser.add_argument("--timeout", type=float, default=300, help="Maximum seconds for each fresh process")
    parser.add_argument("--pinned-reference", action="store_true", help="Binary includes passenger_parallel_reference_prepare.py reference TU")
    parser.add_argument("--gui-only", action="store_true", help="Measure complete live-service startup with old/new GUI cadence")
    args = parser.parse_args()
    args.exe = args.exe.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    identity = {"exe": str(args.exe), "sha256": hashlib.sha256(args.exe.read_bytes()).hexdigest(),
                "side": args.side, "lines": args.lines, "seed": "0x0d16e57", "logical_cpus": os.cpu_count(),
                "rounds": args.rounds, "workers": args.workers,
                "pinned_reference": args.pinned_reference,
                "gui_only": args.gui_only,
                "reference": "initial: unchanged nonblocking StepParallel; retained replay: serial Step via workers=0",
                "peak_memory_scope": "whole fresh process, including unmeasured fixture construction"}
    identity["started_utc"] = datetime.now(timezone.utc).isoformat()
    (args.output / "identity.json").write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8", newline="\n")
    rows = []
    scenarios = ["initial-reference", "initial-preparation", "replay"]
    if args.pinned_reference:
        scenarios.insert(1, "nonblocking-current")
    if args.gui_only:
        scenarios = ["gui-tick-cadence", "gui-current"]
    cases = [(mode, workers, scenario) for mode in ("legacy", "hyperpath")
             for workers in map(int, args.workers.split(",")) for scenario in scenarios]
    for repeat in range(args.rounds):
        for mode, workers, scenario in (cases if repeat % 2 == 0 else list(reversed(cases))):
            name = f"{repeat + 1}-{mode}-{workers}-{scenario}"
            def check_competition(excluded=0):
                competing = competing_processes(excluded)
                if competing:
                    (args.output / "interference.json").write_text(json.dumps({"run": name, "utc": datetime.now(timezone.utc).isoformat(), "processes": competing}, indent=2) + "\n", encoding="utf-8", newline="\n")
                    raise RuntimeError(f"Competing process during {name}: {competing}")
            check_competition()
            env = {key: value for key, value in os.environ.items() if not key.startswith("OPENTTD_")}
            env.update(OPENTTD_PARALLEL_WORKERS=str(workers), OPENTTD_PARALLEL_HYPERPATH=str(int(mode == "hyperpath")),
                       OPENTTD_PREPARATION_MODE=str(int(scenario == "initial-preparation")),
                       OPENTTD_PINNED_REFERENCE=str(int(args.pinned_reference and scenario == "initial-reference")),
                       OPENTTD_PREPARATION_OLD_CADENCE=str(int(scenario == "gui-tick-cadence")),
                       OPENTTD_PARALLEL_SIDE=str(args.side), OPENTTD_PARALLEL_LINES=str(args.lines))
            tag = "[.passenger-replay-benchmark]" if scenario == "replay" else "[.passenger-parallel-benchmark]"
            if args.gui_only:
                tag = "[.passenger-preparation-loop-benchmark]"
            command = [str(args.exe), tag, "--reporter", "compact"]
            peak = 0
            with (args.output / f"{name}.log").open("wb") as log:
                process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
                started = time.monotonic()
                checked = started
                try:
                    while process.poll() is None:
                        if time.monotonic() - checked >= 0.1:
                            check_competition(process.pid)
                            checked = time.monotonic()
                        if time.monotonic() - started > args.timeout:
                            raise TimeoutError(f"{name} exceeded {args.timeout}s")
                        try:
                            peak = max(peak, peak_memory(process.pid))
                        except OSError:
                            if process.poll() is None:
                                raise
                        time.sleep(0.02)
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait()
            if process.returncode:
                raise RuntimeError(f"{name} failed with {process.returncode}; see its log")
            output = (args.output / f"{name}.log").read_text(encoding="utf-8", errors="replace")
            line = next(line for line in output.splitlines() if line.startswith(("passenger_parallel,", "passenger_replay,", "passenger_gui,")))
            row = {"run": name, "scenario": scenario, "round": repeat + 1, "peak_working_set": peak, "command": command}
            for field in line.split(",")[1:]:
                key, value = field.split("=", 1)
                row[key] = int(value) if value.isdecimal() else value
            rows.append(row)
            (args.output / "results.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8", newline="\n")
            print(name, "wall_ms=", round(row["wall_ns"] / 1e6, 3), "hits=", row["cache_hits"], "serial=", row["serial_rows"], flush=True)
    for mode in ("legacy", "hyperpath"):
        for scenario in scenarios:
            group = [row for row in rows if row["mode"] == mode and row["scenario"] == scenario]
            if len({row["digest"] for row in group}) != 1 or len({row["operations"] for row in group}) != 1:
                raise RuntimeError(f"Work/digest mismatch in {mode}/{scenario}")
        initial = [row for row in rows if row["mode"] == mode and row["scenario"] != "replay"]
        if len({row["digest"] for row in initial}) != 1:
            raise RuntimeError(f"Initial scalar/parallel digest mismatch in {mode}")
    (args.output / "validation.json").write_text(json.dumps({"runs": len(rows), "work_and_published_digests_equal": True,
        "competing_process_checks": "before each case and every 100ms during it", "finished_utc": datetime.now(timezone.utc).isoformat()}, indent=2) + "\n", encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
