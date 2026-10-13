"""Prepare copied TUs in a freshly configured VS build; repository code stays intact."""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("source", type=pathlib.Path)
p.add_argument("build", type=pathlib.Path)
p.add_argument("--mode", choices=["baseline", "pruned", "detail", "baseline-detail", "quality"], required=True)
p.add_argument("--base", default="56b0f287b013d4775cbd62c4ec5d5801c03f8e40")
a = p.parse_args()
source, build = a.source.resolve(), a.build.resolve()
spec = importlib.util.spec_from_file_location("instrument", source / "docs/measurements/passenger-routing-profile-2026-10-13/Instrument-Profile.py")
instrument = importlib.util.module_from_spec(spec)
spec.loader.exec_module(instrument)
instrument.prepare(source, build, "minimal")
output = build / "journey-profile"
project = build / "openttd_lib.vcxproj"
if a.mode != "pruned":
    name = "passenger_service_database.cpp"
    text = subprocess.check_output(["git", "-C", str(source), "show", f"{a.base}:src/{name}"], encoding="utf-8") if a.mode.startswith("baseline") else (source / "src" / name).read_text(encoding="utf-8")
    text = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (source / "src" / m[1]).as_posix() + '"' if (source / "src" / m[1]).is_file() else m[0], text)
    if a.mode.endswith("detail"):
        header = output / "pruning-profile.h"
        header.write_text('''#pragma once
#include <chrono>
#include <cstdint>
namespace PruningProfile {
inline uint64_t bound_ns = 0, phase_ns = 0, bound_calls = 0, phase_calls = 0;
inline void Reset() { bound_ns = phase_ns = bound_calls = phase_calls = 0; }
struct Scope {
    uint64_t &ns;
    std::chrono::steady_clock::time_point start;
    Scope(uint64_t &ns, uint64_t &calls) : ns(ns), start(std::chrono::steady_clock::now()) { ++calls; }
    ~Scope() { ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count(); }
};
}
''', encoding="utf-8")
        text = instrument.replace(text, '#include "' + (source / "src/stdafx.h").as_posix() + '"', '#include "' + (source / "src/stdafx.h").as_posix() + '"\n#include "' + header.as_posix() + '"')
        if a.mode == "detail":
            text = instrument.replace(text, '\t++this->diagnostic.service_bound_checks;', '\tPruningProfile::Scope scope{PruningProfile::bound_ns, PruningProfile::bound_calls};\n\t++this->diagnostic.service_bound_checks;')
        text = instrument.replace(text, '\t\t\t\tconst Station *loading_station =', '\t\t\t\t{\n\t\t\t\tPruningProfile::Scope scope{PruningProfile::phase_ns, PruningProfile::phase_calls};\n\t\t\t\tconst Station *loading_station =')
        text = instrument.replace(text, '\t\t\t\tif (result.work >= limits.work) { limited(Reason::Work, 0); break; }', '\t\t\t\t}\n\t\t\t\tif (result.work >= limits.work) { limited(Reason::Work, 0); break; }')
        console_file = output / "console_cmds.cpp"
        console = console_file.read_text(encoding="utf-8")
        console = instrument.replace(console, '#include "' + (source / "src/stdafx.h").as_posix() + '"', '#include "' + (source / "src/stdafx.h").as_posix() + '"\n#include "' + header.as_posix() + '"')
        console = instrument.replace(console, 'if (operation == "reset") { ResetPassengerJourneyDiagnostic(); return true; }', 'if (operation == "reset") { ResetPassengerJourneyDiagnostic(); PruningProfile::Reset(); return true; }')
        console = instrument.replace(console, '\tIConsolePrint(CC_DEFAULT, "Periodic global invalidations: {}", periodic.global_invalidations);', '\tIConsolePrint(CC_DEFAULT, "Periodic pruning profile: bound_ns={}; phase_ns={}; bound_calls={}; phase_calls={}", PruningProfile::bound_ns, PruningProfile::phase_ns, PruningProfile::bound_calls, PruningProfile::phase_calls);\n\tIConsolePrint(CC_DEFAULT, "Periodic global invalidations: {}", periodic.global_invalidations);')
        console_file.write_text(console, encoding="utf-8")
    target = output / name
    target.write_text(text, encoding="utf-8")
    project.write_text(instrument.replace(project.read_text(encoding="utf-8-sig"), str(source / "src" / name), str(target)), encoding="utf-8-sig")
if a.mode == "quality":
    console_file = output / "console_cmds.cpp"
    console = console_file.read_text(encoding="utf-8")
    console = instrument.replace(console, '\t\tauto paired = ComparePassengerPlanners(db, cargo, origin, destination, _state_ticks, limits, evaluation);', '''
        PassengerPlannerComparison paired;
        PassengerServiceDatabase isolated;
        if (!isolated.Load(db.Save())) return false;
        isolated.FinishLoad(true);
        paired.periodic = isolated.Plan(cargo, origin, destination, _state_ticks, limits, evaluation);
        auto reference = isolated.PlanWithoutServicePruning(cargo, origin, destination, _state_ticks, limits, evaluation);
        paired.legacy_found = reference.ride.has_value() && !reference.deferred;
        paired.legacy_route = reference.route; paired.legacy_arrival = reference.arrival; paired.legacy_cost = reference.cost;
        IConsolePrint(CC_DEFAULT, "Timed pruning quality: cargo={}; origin={}; destination={}; old_found={}; new_found={}; old_cost={}; new_cost={}; old_arrival={}; new_arrival={}; old_transfers={}; new_transfers={}; old_work={}; new_work={}; old_deferred={}; new_deferred={}; routes_equal={}",
            cargo, origin, destination, paired.legacy_found, paired.periodic.ride.has_value() && !paired.periodic.deferred,
            reference.cost, paired.periodic.cost, reference.arrival.base(), paired.periodic.arrival.base(),
            reference.transfers, paired.periodic.transfers, reference.work, paired.periodic.work,
            reference.deferred, paired.periodic.deferred, reference.route == paired.periodic.route);''')
    console = instrument.replace(console, 'print_route("snapshot", paired.legacy_route);', 'print_route("unpruned", paired.legacy_route);')
    console_file.write_text(console, encoding="utf-8")
metadata = json.loads((output / "instrumentation.json").read_text(encoding="utf-8"))
metadata.update(comparison_mode=a.mode, algorithm_base=a.base, mode="detail" if a.mode.endswith("detail") else "minimal")
project_text = project.read_text(encoding="utf-8-sig")
metadata["copies"] = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in output.glob("*.cpp") if str(f) in project_text}
metadata["database_source_sha256"] = hashlib.sha256((source / "src/passenger_service_database.cpp").read_bytes()).hexdigest()
metadata["database_header_sha256"] = hashlib.sha256((source / "src/passenger_service_database.h").read_bytes()).hexdigest()
(output / "instrumentation.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
print(a.mode, metadata["copies"])
