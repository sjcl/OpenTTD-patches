from pathlib import Path
import re
ROOT = Path(r'C:\Users\bebe\.codex\worktrees\6d2b\OpenTTD-patches')
for compiler in ('msvc', 'clang'):
    build = ROOT / 'build-compiler-comparison' / compiler
    folder = build / 'cpu-measurement'
    project = build / 'openttd_lib.vcxproj'
    xml = project.read_text(encoding='utf-8-sig')
    for name in ('passenger_service_database.cpp', 'passenger_journey_schedule.cpp'):
        original = ROOT / 'src' / name
        target = folder / name
        content = original.read_text(encoding='utf-8')
        content = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (original.parent / m[1]).resolve().as_posix() + '"' if (original.parent / m[1]).is_file() else m[0], content)
        anchor = '#include "' + (ROOT / 'src/stdafx.h').as_posix() + '"'
        content = content.replace(anchor, anchor + '\n#include "' + (folder / 'cpu_clock.h').as_posix() + '"')
        # Only query diagnostics: leave construction and all scheduling unchanged.
        if name == 'passenger_service_database.cpp':
            offset = content.index('const auto started = std::chrono::steady_clock::now(); ++this->diagnostic.searches;')
            content = content[:offset] + content[offset:].replace('std::chrono::steady_clock', 'MeasurementCpuClock')
        else:
            content = content.replace('std::chrono::steady_clock', 'MeasurementCpuClock')
        target.write_text(content, encoding='utf-8')
        xml = xml.replace(str(target), str(original))
        assert xml.count(str(original)) == 1
        xml = xml.replace(str(original), str(target))
    project.write_text(xml, encoding='utf-8-sig')
print('Prepared CPU query diagnostics')
