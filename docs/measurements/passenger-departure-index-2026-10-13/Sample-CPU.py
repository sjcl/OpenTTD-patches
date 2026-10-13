"""Sample only an explicitly supplied isolated OpenTTD process's busiest thread.

Windows x64 instruction-pointer samples, matched against an MSVC /MAP file.
No stack unwinding: counts are exclusive, and inlined code belongs to its caller.
Run separately from speed measurements. Every successful suspension is resumed
in a finally block; there are no samples or data from unrelated processes.
"""
import argparse
import bisect
import collections
import ctypes as c
from ctypes import wintypes as w
import json
import pathlib
import re
import struct
import time

p = argparse.ArgumentParser()
p.add_argument("pid", type=int)
p.add_argument("map", type=pathlib.Path)
p.add_argument("output", type=pathlib.Path)
p.add_argument("--seconds", type=float, default=45)
p.add_argument("--interval", type=float, default=.007)
a = p.parse_args()
kernel, psapi, dbghelp = c.WinDLL("kernel32", use_last_error=True), c.WinDLL("psapi"), c.WinDLL("dbghelp")
for fn, args, ret in [
    (kernel.OpenProcess, [w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
    (kernel.OpenThread, [w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
    (kernel.CloseHandle, [w.HANDLE], w.BOOL),
    (kernel.CreateToolhelp32Snapshot, [w.DWORD, w.DWORD], w.HANDLE),
    (kernel.SuspendThread, [w.HANDLE], w.DWORD),
    (kernel.ResumeThread, [w.HANDLE], w.DWORD),
    (kernel.GetThreadContext, [w.HANDLE, c.c_void_p], w.BOOL),
    (kernel.GetThreadTimes, [w.HANDLE, c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p], w.BOOL),
    (psapi.EnumProcessModules, [w.HANDLE, c.c_void_p, w.DWORD, c.c_void_p], w.BOOL),
    (psapi.GetModuleInformation, [w.HANDLE, w.HMODULE, c.c_void_p, w.DWORD], w.BOOL),
    (dbghelp.UnDecorateSymbolName, [c.c_char_p, c.c_char_p, w.DWORD, w.DWORD], w.DWORD),
]:
    fn.argtypes, fn.restype = args, ret

class Thread(c.Structure):
    _fields_ = [("size", w.DWORD), ("usage", w.DWORD), ("tid", w.DWORD), ("pid", w.DWORD),
                ("priority", w.LONG), ("delta", w.LONG), ("flags", w.DWORD)]

kernel.Thread32First.argtypes = kernel.Thread32Next.argtypes = [w.HANDLE, c.POINTER(Thread)]
class Module(c.Structure):
    _fields_ = [("base", c.c_void_p), ("size", w.DWORD), ("entry", c.c_void_p)]

process = kernel.OpenProcess(0x400 | 0x10, False, a.pid)
if not process:
    raise c.WinError(c.get_last_error())
modules, needed, module = (w.HMODULE * 1024)(), w.DWORD(), Module()
assert psapi.EnumProcessModules(process, modules, c.sizeof(modules), c.byref(needed))
assert psapi.GetModuleInformation(process, modules[0], c.byref(module), c.sizeof(module))
kernel.CloseHandle(process)
snapshot = kernel.CreateToolhelp32Snapshot(4, 0)
thread, handles = Thread(), []
thread.size = c.sizeof(thread)
ok = kernel.Thread32First(snapshot, c.byref(thread))
while ok:
    if thread.pid == a.pid:
        handle = kernel.OpenThread(2 | 8 | 0x40, False, thread.tid)
        if handle:
            times = [c.c_uint64() for _ in range(4)]
            if kernel.GetThreadTimes(handle, *(c.byref(t) for t in times)):
                handles.append((times[3].value, thread.tid, handle))
            else:
                kernel.CloseHandle(handle)
    ok = kernel.Thread32Next(snapshot, c.byref(thread))
kernel.CloseHandle(snapshot)
assert handles, "No accessible thread in requested process"
_, tid, handle = max(handles)
for _, _, other in handles:
    if other != handle:
        kernel.CloseHandle(other)

text = a.map.read_text(errors="replace")
preferred = int(re.search(r"Preferred load address is\s+([0-9A-Fa-f]+)", text)[1], 16)
symbols = {}
for match in re.finditer(r"^\s+[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+(\S+)\s+([0-9A-Fa-f]{16})\s+f\s", text, re.M):
    output = c.create_string_buffer(4096)
    name = match[1].encode()
    decoded = dbghelp.UnDecorateSymbolName(name, output, len(output), 0x1000)
    symbols[int(match[2], 16) - preferred] = output.value.decode(errors="replace") if decoded else match[1]
addresses = sorted(symbols)
assert addresses, "No public function symbols in link map"
counts, failures, outside = collections.Counter(), 0, 0
raw = c.create_string_buffer(1248)
context = (c.addressof(raw) + 15) & ~15
start = time.monotonic()
try:
    while time.monotonic() - start < a.seconds:
        if kernel.SuspendThread(handle) == 0xffffffff:
            failures += 1
            break
        try:
            c.memset(context, 0, 1232)
            c.c_uint32.from_address(context + 48).value = 0x100001
            if not kernel.GetThreadContext(handle, context):
                failures += 1
                continue
            pc = c.c_uint64.from_address(context + 248).value
        finally:
            kernel.ResumeThread(handle)
        if module.base <= pc < module.base + module.size:
            index = bisect.bisect_right(addresses, pc - module.base) - 1
            counts[symbols[addresses[index]] if index >= 0 else "before-first-symbol"] += 1
        else:
            outside += 1
        time.sleep(a.interval)
finally:
    kernel.CloseHandle(handle)
result = dict(pid=a.pid, tid=tid, method="Windows x64 suspended-thread RIP; exclusive public function symbols",
              seconds=time.monotonic() - start, interval_seconds=a.interval, executable_samples=sum(counts.values()),
              outside_executable=outside, failures=failures, functions=counts.most_common())
a.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
print(json.dumps({k: v for k, v in result.items() if k != "functions"}))
