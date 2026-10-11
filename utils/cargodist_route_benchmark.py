#!/usr/bin/env python3
"""Run the CargoDist synthetic benchmarks and measure Windows process CPU/RSS.

Usage: python utils/cargodist_route_benchmark.py EXE OUTPUT_DIR
Catch2 reports solver wall time and scratch payload. Process CPU and peak RSS
also include fixture construction, FlowStat setup and cleanup.
"""

import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time


class MemoryCounters(ctypes.Structure):
    _fields_ = [('cb', wintypes.DWORD), ('faults', wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in ('peak_rss', 'rss', 'peak_paged', 'paged',
                                          'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_pagefile')]


def main():
    assert os.name == 'nt', 'Process measurements in this utility require Windows'
    exe, directory = [Path(value).resolve() for value in sys.argv[1:]]
    directory.mkdir(parents=True, exist_ok=True)
    memory_api = ctypes.WinDLL('psapi').GetProcessMemoryInfo
    memory_api.argtypes = [wintypes.HANDLE, ctypes.POINTER(MemoryCounters), wintypes.DWORD]
    times_api = ctypes.WinDLL('kernel32').GetProcessTimes
    times_api.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    results = []
    cases = [(size, section, False) for size in (2048, 8192)
             for section in ('Station-only baseline', 'Service states',
                             'Station-only capacity baseline', 'Service capacity states')]
    cases += [(2048, section, True) for section in ('Station-only baseline', 'Service states')]
    for size, section, all_sources in cases:
        env = os.environ.copy()
        if size == 8192:
            env['OPENTTD_ROUTE_BENCH_LARGE'] = '1'
        else:
            env.pop('OPENTTD_ROUTE_BENCH_LARGE', None)
        env.pop('OPENTTD_ROUTE_BENCH_ALL_SOURCES', None)
        if all_sources:
            env['OPENTTD_ROUTE_BENCH_ALL_SOURCES'] = '1'
        label = f'{size}-' + ('all-sources-' if all_sources else '') + section.lower().replace(' ', '-')
        log_path = directory / (label + '.log')
        with log_path.open('wb') as output:
            begin = time.perf_counter()
            process = subprocess.Popen([str(exe), 'CargoDist sparse multi-service search performance',
                                        '-c', section], stdout=output, stderr=subprocess.STDOUT, env=env)
            handle = wintypes.HANDLE(int(process._handle))
            peak = 0
            while True:
                counters = MemoryCounters()
                counters.cb = ctypes.sizeof(counters)
                if memory_api(handle, ctypes.byref(counters), counters.cb):
                    peak = max(peak, counters.peak_rss)
                if process.poll() is not None:
                    break
                time.sleep(0.02)
            assert process.returncode == 0, log_path.read_text(errors='replace')
            filetimes = [wintypes.FILETIME() for _ in range(4)]
            assert times_api(handle, *[ctypes.byref(value) for value in filetimes])
            ticks = lambda value: (value.dwHighDateTime << 32) | value.dwLowDateTime
            result = dict(nodes=size, active_sources=size - 1 if all_sources else 32, section=section, cpu_ms=(ticks(filetimes[2]) + ticks(filetimes[3])) / 10000,
                          process_wall_ms=round((time.perf_counter() - begin) * 1000, 1), peak_rss_bytes=peak)
            results.append(result)
            print(json.dumps(result), flush=True)
    (directory / 'results.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
