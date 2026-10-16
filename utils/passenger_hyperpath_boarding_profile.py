#!/usr/bin/env python3
"""Instrument copied TUs after passenger_hyperpath_prepare.py (no product edits).

All counters are process-local observations, reset after paused setup. CSV rows
are cumulative snapshots after each measured tick; difference them for windows.
Enumeration includes Predicate and EnumerationRide: never add nested timings.
"""
import argparse
from pathlib import Path
import re
import shutil


def replace(text, before, after):
    if text.count(before) != 1:
        raise ValueError(f"Profile anchor changed: {before[:100]}")
    return text.replace(before, after)


def prepare(source, build):
    source, build = source.resolve(), build.resolve()
    header = build / "save-integrity/boarding_profile.h"
    shutil.copy2(source / "utils/passenger_hyperpath_boarding_profile.h", header)
    include = f'#include "{header.as_posix()}"\n'

    def instrument(name, edits):
        project = build / "openttd_lib.vcxproj"
        xml = project.read_text(encoding="utf-8-sig")
        paths = re.findall(r'<ClCompile Include="([^"]+)"', xml)
        matches = [Path(path) for path in paths if Path(path).name == name]
        if len(matches) != 1:
            raise ValueError(f"Expected one compiled TU for {name}: {matches}")
        original = matches[0]
        text = original.read_text(encoding="utf-8")
        if original.parent == source / "src":
            text = re.sub(r'#include "([^"]+)"', lambda m: f'#include "{(original.parent / m[1]).resolve().as_posix()}"', text)
            target = build / "save-integrity" / name
            xml = replace(xml, str(original), str(target))
            project.write_text(xml, encoding="utf-8-sig")
        else:
            target = original
        # Include after stdafx: safeguards.h intentionally belongs last.
        anchor = f'#include "{(source / "src/stdafx.h").as_posix()}"\n'
        text = replace(text, anchor, anchor + include)
        for before, after in edits:
            text = replace(text, before, after)
        target.write_text(text, encoding="utf-8")

    instrument("cargopacket.cpp", [
        ("Visitor &&visitor) const\n{", "Visitor &&visitor) const\n{\n\t++PassengerBoardingProfile::visits;"),
        ("if (vehicle != nullptr && service != OrderListID::Invalid()) {\n\t\t/* Scan only", "if (vehicle != nullptr && service != OrderListID::Invalid()) {\n\t\tPassengerBoardingProfile::Scope profile(PassengerBoardingProfile::Enumeration);\n\t\t/* Scan only"),
        ("for (const auto &[key, policy] : groups) {", "for (const auto &[key, policy] : groups) {\n\t\t\t\t++PassengerBoardingProfile::groups;"),
    ])
    instrument("passenger_journey_active.cpp", [
        ("CargoType cargo, bool base, bool adjusted)\n{", "CargoType cargo, bool base, bool adjusted)\n{\n\tPassengerBoardingProfile::Scope profile(PassengerBoardingProfile::Predicate);"),
    ])
    instrument("passenger_precomputed.cpp", [
        ("uint32_t board, StationID destination) const\n{", "uint32_t board, StationID destination) const\n{\n\tPassengerBoardingProfile::Scope profile(PassengerBoardingProfile::Ride);"),
        ("for (size_t step = 0; step < stops.size(); ++step) {", "for (size_t step = 0; step < stops.size(); ++step) {\n\t\tPassengerBoardingProfile::Stop();"),
    ])
    instrument("console_cmds.cpp", [
        ("ResetPassengerJourneyDiagnostic(); return true;", "PassengerBoardingProfile::Reset(); ResetPassengerJourneyDiagnostic(); return true;"),
    ])
    target = build / "real-map-driver/null_v.cpp"
    text = target.read_text(encoding="utf-8")
    text = include + text
    before = "samples.push_back({_state_ticks.base(), stats.simulation_microseconds, stats.journey_microseconds});"
    text = replace(text, before, before + "\n                PassengerBoardingProfile::Snapshot(_state_ticks.base());")
    text = replace(text, 'std::ofstream out("actual-ticks.json");', 'PassengerBoardingProfile::Dump();\n        std::ofstream out("actual-ticks.json");')
    target.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("build", type=Path)
    args = parser.parse_args()
    prepare(args.source, args.build)
