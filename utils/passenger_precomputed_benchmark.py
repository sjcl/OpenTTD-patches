#!/usr/bin/env python3
"""Run opt-in C++ routing/boarding CPU benchmarks and capture process memory/CPU.

Lookup samples are elapsed nanoseconds, including normal routing diagnostics.
Process CPU and peak memory are measured independently on Windows. This synthetic
run does not claim real-map simulation percentiles or transport throughput.
"""
import argparse
import csv
import ctypes
import hashlib
import json
import pathlib
import platform
import subprocess
import time


class Memory(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_uint32), ("faults", ctypes.c_uint32)] + [
        (name, ctypes.c_size_t) for name in
        ("peak_working", "working", "peak_paged", "paged", "peak_nonpaged", "nonpaged", "pagefile", "peak_pagefile", "private")]


def observe(process):
    if platform.system() != "Windows":
        return {}
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    memory = Memory()
    memory.cb = ctypes.sizeof(memory)
    get_memory = kernel.K32GetProcessMemoryInfo
    get_memory.argtypes = [ctypes.c_void_p, ctypes.POINTER(Memory), ctypes.c_uint32]
    result = {}
    if get_memory(int(process._handle), ctypes.byref(memory), memory.cb):
        result.update(peak_working_bytes=memory.peak_working, peak_commit_bytes=memory.peak_pagefile)
    times = [ctypes.c_uint64() for _ in range(4)]
    get_times = kernel.GetProcessTimes
    get_times.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_uint64)] * 4
    if get_times(int(process._handle), *(ctypes.byref(value) for value in times)):
        result["process_cpu_seconds"] = (times[2].value + times[3].value) / 10_000_000
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    command = [str(args.binary.resolve()), "[precomputed][cpu]", "-r", "compact"]
    started = time.perf_counter()
    # A file avoids pipe-buffer stalls and keeps the complete raw measurement.
    with (args.output / "raw.log").open("wb") as stream:
        process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW if platform.system() == "Windows" else 0)
        observed = {}
        while process.poll() is None:
            for key, value in observe(process).items():
                observed[key] = max(observed.get(key, 0), value)
            time.sleep(0.05)
        for key, value in observe(process).items():
            observed[key] = max(observed.get(key, 0), value)
    raw = (args.output / "raw.log").read_text(encoding="utf-8", errors="replace")
    routing, boarding, boarding_prefix = [], [], []
    for row in csv.reader(raw.splitlines()):
        if not row or not row[0].isdigit():
            continue
        if len(row) == 12:
            routing.append(dict(zip(("axis", "size", "build_us", "lookup_ns", "p95_ns", "p99_ns", "flat_array_bytes", "allocation_envelope", "states", "edges", "operations", "hits"), map(int, row))))
        elif len(row) == 4:
            boarding.append(dict(zip(("waiting_packets", "legacy_ns", "indexed_ns", "indexed_candidates"), map(int, row))))
        elif len(row) == 3:
            boarding_prefix.append(dict(zip(("cohort_packets", "prefix_ns", "prefix_candidates"), map(int, row))))
    result = dict(command=command, exit_code=process.returncode, elapsed_seconds=time.perf_counter() - started,
                  platform=platform.platform(), processor=platform.processor(),
                  binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  routing=routing, boarding=boarding, boarding_prefix=boarding_prefix, **observed,
                  axes={0: "served stations", 1: "physical vehicles (size x 16)", 2: "same-OD lookup batches (size x 4 x 256)", 3: "OD diversity"},
                  limitations=["Elapsed per-call samples and total process CPU are distinct measurements.",
                               "Peak process memory includes the test runtime and prior cases; it is not per-table peak memory.",
                               "Other host activity is not excluded by this runner.",
                               "Real-map transport and simulation percentiles require a separate paired run."])
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    if process.returncode or len(routing) != 16 or len(boarding) != 4 or len(boarding_prefix) != 4:
        raise SystemExit("Benchmark failed or incomplete; retain raw.log.")


if __name__ == "__main__":
    main()
