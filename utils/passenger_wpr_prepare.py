"""Add bounded WPR handshakes to already-prepared measurement copies; product sources stay unchanged."""
import argparse
from pathlib import Path


def replace(path, before, after):
    source = path.read_text(encoding='utf-8')
    if source.count(before) != 1:
        raise ValueError('Measurement anchor changed: ' + str(path))
    path.write_text(source.replace(before, after), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('build', type=Path)
    args = parser.parse_args()
    header = args.build / 'save-integrity/wpr_gate.h'
    header.write_text('''#pragma once
#include <chrono>
#include <fstream>
#include <thread>
#include <windows.h>
namespace PassengerWpr {
inline bool Gate(const char *ready, const char *release)
{
    if (getenv("OPENTTD_WPR_PROFILE") == nullptr) return true;
    { std::ofstream out(ready); out << GetCurrentProcessId() << ' ' << GetCurrentThreadId() << std::endl; }
    const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(15);
    while (std::chrono::steady_clock::now() < until) {
        if (std::ifstream(release).good()) return true;
        if (std::ifstream("wpr-error.txt").good()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
}
''', encoding='utf-8')
    console = args.build / 'save-integrity/console_cmds.cpp'
    replace(console, 'static bool ConPassengerJourneyStats(std::span<std::string_view> argv)\n{',
            '#include "' + header.resolve().as_posix() + '"\nstatic bool ConPassengerJourneyStats(std::span<std::string_view> argv)\n{')
    replace(console, '\tif (operation == "scratch_on" || operation == "scratch_off") {',
            '    if (operation == "wpr_begin") {\n'
            '        if (!PassengerWpr::Gate("wpr-ready.txt", "wpr-go.txt")) { _exit_game = true; return false; }\n'
            '        return true;\n    }\n\tif (operation == "scratch_on" || operation == "scratch_off") {')
    driver = args.build / 'real-map-driver/null_v.cpp'
    text = driver.read_text(encoding='utf-8')
    include = '#include "' + header.resolve().as_posix() + '"\n'
    stdafx = next(line for line in text.splitlines(keepends=True)
                  if line.startswith('#include ') and line.rstrip().endswith('/stdafx.h"'))
    text = text.replace(stdafx, stdafx + include, 1)
    anchor = '        std::ofstream out("actual-ticks.json");'
    if text.count(anchor) != 1:
        raise ValueError('Exact-tick driver anchor changed')
    text = text.replace(anchor, '        if (!PassengerWpr::Gate("wpr-finished.txt", "wpr-saved.txt")) _exit_game = true;\n' + anchor)
    driver.write_text(text, encoding='utf-8')
    packet = args.build / 'save-integrity/cargopacket_sl.cpp'
    anchor = '        const auto &r = cp.passenger_journey;'
    replace(packet, anchor, '        const auto &failure = cp.passenger_boarding_failure;\n'
            '        Mix(merge, failure.service, failure.count, failure.since, failure.incarnation);\n' + anchor)


if __name__ == '__main__':
    main()
