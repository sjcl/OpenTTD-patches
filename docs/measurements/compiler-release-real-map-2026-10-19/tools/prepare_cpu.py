from pathlib import Path
import re
ROOT = Path(r'C:\Users\bebe\.codex\worktrees\6d2b\OpenTTD-patches')
BASE = ROOT / 'build-compiler-comparison'
HEADER = r'''#pragma once
#include <windows.h>
#include <chrono>
struct MeasurementCpuClock {
    using rep = int64_t;
    using period = std::nano;
    using duration = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<MeasurementCpuClock>;
    static constexpr bool is_steady = true;
    static uint64_t Value(FILETIME value) { return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; }
    static time_point now() {
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) std::abort();
        return time_point(duration((Value(kernel) + Value(user)) * 100));
    }
    static uint64_t process_us() {
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) std::abort();
        return (Value(kernel) + Value(user)) / 10;
    }
};
'''
for compiler in ('msvc', 'clang'):
    build = BASE / compiler
    folder = build / 'cpu-measurement'
    folder.mkdir(exist_ok=True)
    header = folder / 'cpu_clock.h'
    header.write_text(HEADER)
    project = build / 'openttd_lib.vcxproj'
    xml = project.read_text(encoding='utf-8-sig')
    for name in ('passenger_journey_active.cpp', 'openttd.cpp'):
        original = ROOT / 'src' / name
        content = original.read_text(encoding='utf-8')
        content = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (original.parent / m[1]).resolve().as_posix() + '"' if (original.parent / m[1]).is_file() else m[0], content)
        content = content.replace('#include "' + (ROOT / 'src/stdafx.h').as_posix() + '"', '#include "' + (ROOT / 'src/stdafx.h').as_posix() + '"\n#include "' + header.as_posix() + '"')
        if name == 'passenger_journey_active.cpp':
            content = content.replace('std::chrono::steady_clock', 'MeasurementCpuClock')
        else:
            content = content.replace('const auto passenger_tick_started = std::chrono::steady_clock::now();', 'const auto passenger_tick_started = MeasurementCpuClock::now();')
            content = content.replace('std::chrono::steady_clock::now() - passenger_tick_started', 'MeasurementCpuClock::now() - passenger_tick_started')
        target = folder / name
        target.write_text(content, encoding='utf-8')
        xml = xml.replace(str(target), str(original))
        assert xml.count(str(original)) == 1
        xml = xml.replace(str(original), str(target))
    project.write_text(xml, encoding='utf-8-sig')
    console = build / 'save-integrity/console_cmds.cpp'
    content = console.read_text(encoding='utf-8')
    content = content.replace('#include "' + (ROOT / 'src/stdafx.h').as_posix() + '"', '#include "' + (ROOT / 'src/stdafx.h').as_posix() + '"\n#include "' + header.as_posix() + '"')
    content = content.replace('const auto started = std::chrono::steady_clock::now();\n        for (int i = 0; i < 4096;', 'const auto started = std::chrono::steady_clock::now();\n        const auto cpu_started = MeasurementCpuClock::process_us();\n        for (int i = 0; i < 4096;')
    content = content.replace('Precomputed warm: ready={}; elapsed_us={}', 'Precomputed warm: ready={}; elapsed_us={}; cpu_us={}')
    content = content.replace('std::chrono::steady_clock::now() - started).count());\n        return ready;', 'std::chrono::steady_clock::now() - started).count(), MeasurementCpuClock::process_us() - cpu_started);\n        return ready;')
    console.write_text(content, encoding='utf-8')
print('Prepared copied CPU instrumentation for both compilers')
