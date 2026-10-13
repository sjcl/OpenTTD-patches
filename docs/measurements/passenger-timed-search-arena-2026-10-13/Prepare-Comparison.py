"""Instrument copied TUs in a freshly configured VS project, never product source."""
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
p.add_argument("--mode", choices=["legacy", "arena", "disabled", "legacy-detail", "arena-detail", "network"], required=True)
p.add_argument("--base", default="604ce993892479656a2c5ba2fbd311cb19c5d896")
a = p.parse_args()
source, build = a.source.resolve(), a.build.resolve()
spec = importlib.util.spec_from_file_location("instrument", source / "docs/measurements/passenger-routing-profile-2026-10-13/Instrument-Profile.py")
instrument = importlib.util.module_from_spec(spec)
spec.loader.exec_module(instrument)
instrument.prepare(source, build, "minimal")
output = build / "journey-profile"
project = build / "openttd_lib.vcxproj"
text = (source / "src/passenger_service_database.cpp").read_text(encoding="utf-8")
if a.mode.startswith("legacy"):
    # Use the immutable baseline TU and its original Label size, not the shared
    # reference adapter. Only bridge the declaration and add the same counters.
    text = subprocess.check_output(["git", "-C", str(source), "show", f"{a.base}:src/passenger_service_database.cpp"], encoding="utf-8")
    text = instrument.replace(text, "OrderListID excluded, bool prune_services)", "OrderListID excluded, bool prune_services, bool)")
    text = instrument.replace(text, "auto found = frontiers.find(next.station);", "++this->diagnostic.frontier_lookups;\n\t\t\tauto found = frontiers.find(next.station);")
    text = instrument.replace(text, "frontiers[next.station].push_back(labels.size());", "++this->diagnostic.queue_pushes;\n\t\t\tfrontiers[next.station].push_back(labels.size());")
    text = instrument.replace(text, "size_t index = std::get<8>(queue.top()); queue.pop();", "size_t index = std::get<8>(queue.top()); queue.pop();\n\t\t\t++this->diagnostic.queue_pops;")
    text = instrument.replace(text, "result.labels = labels.size();", "result.labels = labels.size();\n\t\tthis->diagnostic.timed_labels += result.labels;")
elif a.mode == "disabled":
    text = instrument.replace(text, "return this->PlanInternal(cargo, origin, destination, ready, limits, evaluation, excluded, true);",
                              "this->SetTimedSearchScratchEnabled(false);\n\treturn this->PlanInternal(cargo, origin, destination, ready, limits, evaluation, excluded, true);")

def absolute_includes(text):
    return re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (source / "src" / m[1]).as_posix() + '"' if (source / "src" / m[1]).is_file() else m[0], text)

text = absolute_includes(text)
if a.mode.endswith("detail"):
    header = output / "arena-profile.h"
    header.write_text('''#pragma once
#include <chrono>
#include <cstdint>
namespace ArenaProfile {
inline uint64_t queue_ns = 0, queue_calls = 0;
inline void Reset() { queue_ns = queue_calls = 0; }
struct Scope {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    Scope() { ++queue_calls; }
    ~Scope() { queue_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count(); }
};
}
''', encoding="utf-8")
    text = instrument.replace(text, '#include "' + (source / "src/stdafx.h").as_posix() + '"', '#include "' + (source / "src/stdafx.h").as_posix() + '"\n#include "' + header.as_posix() + '"')
    text = instrument.replace(text, "size_t index = std::get<8>(queue.top()); queue.pop();", "size_t index = std::get<8>(queue.top()); { ArenaProfile::Scope scope; queue.pop(); }")
    call = 'queue.emplace(lower, next.cost, next.time, next.rides, next.first.departure, next.first.service,\n' + ('\t' * (4 if a.mode.startswith("legacy") else 5)) + 'next.first.boarding_order, next.first.alighting_order, labels.size());'
    text = instrument.replace(text, call, "{ ArenaProfile::Scope scope; " + call + " }")
    console_file = output / "console_cmds.cpp"
    console = console_file.read_text(encoding="utf-8")
    console = instrument.replace(console, '#include "' + (source / "src/stdafx.h").as_posix() + '"', '#include "' + (source / "src/stdafx.h").as_posix() + '"\n#include "' + header.as_posix() + '"')
    console = instrument.replace(console, 'if (operation == "reset") { ResetPassengerJourneyDiagnostic(); return true; }', 'if (operation == "reset") { ResetPassengerJourneyDiagnostic(); ArenaProfile::Reset(); return true; }')
    console = instrument.replace(console, '\tIConsolePrint(CC_DEFAULT, "Periodic global invalidations: {}", periodic.global_invalidations);', '\tIConsolePrint(CC_DEFAULT, "Timed queue profile: calls={}; ns={}", ArenaProfile::queue_calls, ArenaProfile::queue_ns);\n\tIConsolePrint(CC_DEFAULT, "Periodic global invalidations: {}", periodic.global_invalidations);')
    console_file.write_text(console, encoding="utf-8")
target = output / "passenger_service_database.cpp"
target.write_text(text, encoding="utf-8")
project.write_text(instrument.replace(project.read_text(encoding="utf-8-sig"), str(source / "src/passenger_service_database.cpp"), str(target)), encoding="utf-8-sig")
metadata = json.loads((output / "instrumentation.json").read_text(encoding="utf-8"))
metadata.update(comparison_mode=a.mode, mode="detail" if a.mode.endswith("detail") else "minimal")
metadata["baseline_commit"] = a.base
metadata["copies"] = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in output.glob("*.cpp") if str(f) in project.read_text(encoding="utf-8-sig")}
metadata["source_sha256"] = {name: hashlib.sha256((source / "src" / name).read_bytes()).hexdigest() for name in ("passenger_service_database.cpp", "passenger_service_database.h", "passenger_timed_search.h")}
(output / "instrumentation.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
print(a.mode, metadata["copies"])
