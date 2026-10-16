"""Opt-in, read-only Windows host observations for passenger measurements.

PDH counters are recorded as reported, without treating them as exact hardware
cycles or adjusting any simulation samples. No scheduling or power policy is set.
"""
import ctypes
import os
import time


class PowerState(ctypes.Structure):
    _fields_ = [("version", ctypes.c_uint32), ("control_mask", ctypes.c_uint32),
                ("state_mask", ctypes.c_uint32)]


class GroupAffinity(ctypes.Structure):
    _fields_ = [("mask", ctypes.c_size_t), ("group", ctypes.c_ushort),
                ("reserved", ctypes.c_ushort * 3)]


class CounterValue(ctypes.Structure):
    _fields_ = [("status", ctypes.c_uint32), ("value", ctypes.c_double)]


class ThreadEntry(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in
                ("size", "usage", "thread_id", "process_id")] + [
                ("priority", ctypes.c_int32), ("delta", ctypes.c_int32),
                ("flags", ctypes.c_uint32)]


class HostObserver:
    def __init__(self, process_handle, process_id, thread_id):
        if os.name != "nt":
            raise ValueError("Host observation requires Windows")
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.pdh = ctypes.WinDLL("pdh", use_last_error=True)
        self.process = process_handle
        self.process_id = process_id
        self.thread = None
        self.query = ctypes.c_void_p()
        self.counters = {}
        self.counter_errors = {}
        self.worker_affinities = None
        self.samples = []
        self.kernel.OpenThread.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
        self.kernel.OpenThread.restype = ctypes.c_void_p
        self.kernel.CloseHandle.argtypes = (ctypes.c_void_p,)
        self.kernel.GetThreadTimes.argtypes = (ctypes.c_void_p,) + (ctypes.POINTER(ctypes.c_uint64),) * 4
        self.kernel.GetProcessTimes.argtypes = self.kernel.GetThreadTimes.argtypes
        self.kernel.QueryThreadCycleTime.argtypes = (ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64))
        self.kernel.GetThreadGroupAffinity.argtypes = (ctypes.c_void_p, ctypes.POINTER(GroupAffinity))
        self.kernel.GetProcessInformation.argtypes = (ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32)
        self.kernel.GetThreadInformation.argtypes = (ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32)
        self.kernel.GetPriorityClass.argtypes = (ctypes.c_void_p,)
        self.kernel.CreateToolhelp32Snapshot.argtypes = (ctypes.c_uint32, ctypes.c_uint32)
        self.kernel.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
        self.kernel.Thread32First.argtypes = (ctypes.c_void_p, ctypes.POINTER(ThreadEntry))
        self.kernel.Thread32Next.argtypes = (ctypes.c_void_p, ctypes.POINTER(ThreadEntry))
        self.descriptions = hasattr(self.kernel, "GetThreadDescription")
        if self.descriptions:
            self.kernel.GetThreadDescription.argtypes = (ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))
            self.kernel.LocalFree.argtypes = (ctypes.c_void_p,)
            self.kernel.LocalFree.restype = ctypes.c_void_p
        self.pdh.PdhOpenQueryW.argtypes = (ctypes.c_wchar_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_void_p))
        self.pdh.PdhAddEnglishCounterW.argtypes = (ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_void_p))
        self.pdh.PdhCollectQueryData.argtypes = (ctypes.c_void_p,)
        self.pdh.PdhGetFormattedCounterValue.argtypes = (ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(CounterValue))
        self.pdh.PdhCloseQuery.argtypes = (ctypes.c_void_p,)
        try:
            self.thread = self.kernel.OpenThread(0x0040, False, thread_id)
            if not self.thread:
                raise ctypes.WinError(ctypes.get_last_error())
            status = self.pdh.PdhOpenQueryW(None, 0, ctypes.byref(self.query))
            if status:
                raise OSError("PdhOpenQueryW: " + hex(status & 0xffffffff))
            paths = {
                "cpu0_performance_percent": r"\Processor Information(0,0)\% Processor Performance",
                "cpu0_frequency_mhz": r"\Processor Information(0,0)\Processor Frequency",
                "cpu0_time_percent": r"\Processor Information(0,0)\% Processor Time",
                "cpu0_utility_percent": r"\Processor Information(0,0)\% Processor Utility",
                "total_time_percent": r"\Processor Information(_Total)\% Processor Time",
                "total_performance_percent": r"\Processor Information(_Total)\% Processor Performance",
            }
            self.paths = paths
            for name, path in paths.items():
                handle = ctypes.c_void_p()
                status = self.pdh.PdhAddEnglishCounterW(self.query, path, 0, ctypes.byref(handle))
                if status:
                    self.counter_errors[name] = hex(status & 0xffffffff)
                else:
                    self.counters[name] = handle
            status = self.pdh.PdhCollectQueryData(self.query)
            if status:
                self.counter_errors["initial_collect"] = hex(status & 0xffffffff)
        except Exception:
            self.close()
            raise

    def _power(self, thread=False):
        state = PowerState(1, 0, 0)
        function = self.kernel.GetThreadInformation if thread else self.kernel.GetProcessInformation
        ok = function(self.thread if thread else self.process, 3 if thread else 4,
                      ctypes.byref(state), ctypes.sizeof(state))
        if not ok:
            return dict(error=ctypes.get_last_error())
        return dict(version=state.version, control_mask=state.control_mask, state_mask=state.state_mask)

    def _affinities(self):
        snapshot = self.kernel.CreateToolhelp32Snapshot(4, 0)
        if snapshot in (None, ctypes.c_void_p(-1).value):
            return dict(error=ctypes.get_last_error())
        rows = []
        try:
            entry = ThreadEntry()
            entry.size = ctypes.sizeof(entry)
            valid = self.kernel.Thread32First(snapshot, ctypes.byref(entry))
            while valid:
                if entry.process_id == self.process_id:
                    thread = self.kernel.OpenThread(0x0040, False, entry.thread_id)
                    if thread:
                        try:
                            affinity = GroupAffinity()
                            if self.kernel.GetThreadGroupAffinity(thread, ctypes.byref(affinity)):
                                row = dict(thread_id=entry.thread_id, mask=affinity.mask, group=affinity.group)
                                times = [ctypes.c_uint64() for _ in range(4)]
                                if self.kernel.GetThreadTimes(thread, *(ctypes.byref(x) for x in times)):
                                    row["cpu_100ns"] = times[2].value + times[3].value
                                if self.descriptions:
                                    description = ctypes.c_void_p()
                                    if self.kernel.GetThreadDescription(thread, ctypes.byref(description)) >= 0 and description.value:
                                        try:
                                            row["description"] = ctypes.wstring_at(description.value)
                                        finally:
                                            self.kernel.LocalFree(description)
                                rows.append(row)
                        finally:
                            self.kernel.CloseHandle(thread)
                valid = self.kernel.Thread32Next(snapshot, ctypes.byref(entry))
        finally:
            self.kernel.CloseHandle(snapshot)
        return rows

    def sample(self, elapsed, stage):
        started = time.perf_counter_ns()
        row = dict(elapsed_seconds=elapsed, stage=stage)
        status = self.pdh.PdhCollectQueryData(self.query)
        if status:
            row["collect_error"] = hex(status & 0xffffffff)
        row["counters"] = {}
        for name, handle in self.counters.items():
            value = CounterValue()
            status = self.pdh.PdhGetFormattedCounterValue(handle, 0x200 | 0x8000, None, ctypes.byref(value))
            row["counters"][name] = dict(api_status=hex(status & 0xffffffff), data_status=value.status,
                value=value.value if not status and value.status in (0, 1) else None)
        times = [ctypes.c_uint64() for _ in range(4)]
        if self.kernel.GetThreadTimes(self.thread, *(ctypes.byref(x) for x in times)):
            row["main_cpu_100ns"] = times[2].value + times[3].value
        if self.kernel.GetProcessTimes(self.process, *(ctypes.byref(x) for x in times)):
            row["process_cpu_100ns"] = times[2].value + times[3].value
        cycles = ctypes.c_uint64()
        if self.kernel.QueryThreadCycleTime(self.thread, ctypes.byref(cycles)):
            row["main_cycles"] = cycles.value
        row["process_power"] = self._power()
        row["main_thread_power"] = self._power(True)
        row["priority_class"] = self.kernel.GetPriorityClass(self.process)
        # Enumerating thread descriptions can be expensive. Do it only during
        # the large real-map fixture's paused preparation, never in tick timing.
        if self.worker_affinities is None and stage == "preparation" and elapsed >= 40:
            self.worker_affinities = self._affinities()
        row["observation_cost_us"] = (time.perf_counter_ns() - started) / 1000
        self.samples.append(row)

    def result(self):
        return dict(counter_paths=self.paths, counter_errors=self.counter_errors,
                    worker_affinity_snapshot=self.worker_affinities, samples=self.samples,
                    note="Read-only OS observations at approximately 1s intervals. Values are unadjusted provider data; unavailable counters are null, not zero. No power or scheduler setting was changed.")

    def close(self):
        if self.query.value:
            self.pdh.PdhCloseQuery(self.query)
            self.query = ctypes.c_void_p()
        if self.thread:
            self.kernel.CloseHandle(self.thread)
            self.thread = None
