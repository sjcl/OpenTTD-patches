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
            if before.startswith("function:"):
                name = before.removeprefix("function:")
                if text.count(name) != 1:
                    raise ValueError(f"Expected one definition of {name}")
                opening = text.index("\n{", text.index(name)) + 2
                text = text[:opening] + "\n\t" + after + text[opening:]
            else:
                text = replace(text, before, after)
        target.write_text(text, encoding="utf-8")

    enumeration = "PassengerBoardingProfile::Scope profile(PassengerBoardingProfile::Enumeration);"
    if 'bool defer_hyperpath)' in (source / 'src/cargopacket.cpp').read_text(encoding='utf-8'):
        enumeration += '\n\t\tPassengerBoardingProfile::CapacityEnumerationScope capacity_profile(!defer_hyperpath);'
    cargo_edits = [
        ("function:bool StationCargoList::VisitBoardingCandidates(", "++PassengerBoardingProfile::visits; PassengerBoardingProfile::Scope whole_profile(PassengerBoardingProfile::Visit);"),
        ("if (vehicle != nullptr && service != OrderListID::Invalid()) {", "if (vehicle != nullptr && service != OrderListID::Invalid()) {\n\t\t" + enumeration),
        ("for (const auto &[key, policy] : groups) {", "for (const auto &[key, policy] : groups) {\n\t\t\t\t++PassengerBoardingProfile::groups;"),
    ]
    for name, stage in [("StationCargoList::IndexPacket(", "Index"), ("StationCargoList::ForgetWaitingPacket(", "Forget"),
                        ("StationCargoList::RefreshWaitingPacket(", "Refresh"), ("StationCargoList::UpdatePassengerRoutingSchedule(", "Schedule"),
                        ("CargoPacket::StationMergeHash(", "MergeHash"), ("CargoPacket::Merge(", "Merge"),
                        ("StationCargoList::ShiftIndexedJourneyCargo(", "Shift"), ("StationCargoList::EnsureWaitingIndex(", "Ensure"),
                        ("StationCargoList::AddBoardingEntry(", "AddEntry"), ("StationCargoList::RemoveBoardingEntry(", "RemoveEntry")]:
        cargo_edits.append(("function:" + name, f"PassengerBoardingProfile::Scope boarding_profile_scope(PassengerBoardingProfile::{stage});"))
    if 'bool defer_hyperpath)' in (source / 'src/cargopacket.cpp').read_text(encoding='utf-8'):
        cargo_edits.append(('if (!loading.CanUseHyperpathPolicy(*packet, vehicle, this->passenger_index_cargo, current.hyperpath & 1, current.hyperpath & 2)) continue;',
            'if (!loading.CanUseHyperpathPolicy(*packet, vehicle, this->passenger_index_cargo, current.hyperpath & 1, current.hyperpath & 2)) { ++PassengerBoardingProfile::counters[PassengerBoardingProfile::RejectedCohort].calls; continue; }'))
    instrument("cargopacket.cpp", cargo_edits)
    predicates = [("function:bool PassengerJourneyLoadingContext::MayUseHyperpathPolicy(", "PassengerBoardingProfile::Scope profile(PassengerBoardingProfile::Predicate);\n\tif (cargo < NUM_CARGO && vehicle != nullptr && vehicle->First()->current_order.GetCargoLoadType(cargo) == OrderLoadType::NoLoad) ++PassengerBoardingProfile::counters[PassengerBoardingProfile::NoLoad].calls;"),
                  ("function:std::optional<PassengerJourneyExecution> PassengerJourneyLoadingContext::ResolveHyperpath(", "PassengerBoardingProfile::Scope profile(PassengerBoardingProfile::Resolve);")]
    if "bool PassengerJourneyLoadingContext::CanUseHyperpathPolicy(" in (source / "src/passenger_journey_active.cpp").read_text(encoding="utf-8"):
        predicates.append(("function:bool PassengerJourneyLoadingContext::CanUseHyperpathPolicy(", "PassengerBoardingProfile::Scope profile(PassengerBoardingProfile::FullPredicate);"))
        predicates.append(('if (current && intent.boarding != vehicle->First()->last_station_visited) return false;',
            'if (current && intent.boarding != vehicle->First()->last_station_visited) { ++PassengerBoardingProfile::counters[PassengerBoardingProfile::WrongBoarding].calls; return false; }'))
    instrument("passenger_journey_active.cpp", predicates)
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
