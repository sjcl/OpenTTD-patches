"""Instrument copied translation units in an already configured VS project.

No repository source is edited. Reconfigure before changing mode. Anchor checks
deliberately fail when A/B changes need a reviewed instrumentation adaptation.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess


def replace(text, old, new, count=1):
    actual = text.count(old)
    if actual != count:
        raise ValueError(f"Instrumentation anchor count {actual}, expected {count}: {old[:100]!r}")
    return text.replace(old, new)


def function_scope(text, signature, name):
    start = text.index(signature)
    brace = text.index("{", start) + 1
    return text[:brace] + f'\n\tJourneyProfile::Scope profile_scope{{"{name}"}};' + text[brace:]


def observe_calls(text, function, metric):
    """Wrap one call without duplicating or changing short-circuit evaluation."""
    cursor = 0
    while True:
        start = text.find(function + '(', cursor)
        if start < 0:
            return text
        depth, end = 1, start + len(function) + 1
        while depth:
            if end >= len(text):
                raise ValueError(f'Unclosed call to {function}')
            depth += (text[end] == '(') - (text[end] == ')')
            end += 1
        wrapped = f'JourneyProfile::Observe("{metric}", {text[start:end]})'
        text = text[:start] + wrapped + text[end:]
        cursor = start + len(wrapped)


def prepare(source, build, mode):
    source, build = pathlib.Path(os.path.abspath(source)), pathlib.Path(os.path.abspath(build))
    output = build / "journey-profile"
    output.mkdir(parents=True, exist_ok=True)
    texts = {name: (source / "src" / name).read_text(encoding="utf-8") for name in
             ["console_cmds.cpp", "passenger_service_database.cpp", "passenger_journey_active.cpp", "cargopacket.cpp",
              "passenger_journey_schedule.cpp", "passenger_routing.cpp"]}
    console = texts["console_cmds.cpp"]
    # The opt-in benchmark already has the archived NewGRF continuation.
    if 'Command<Commands::Pause>::Post(PauseMode::Error, false);' not in console:
        raise ValueError("Expected existing benchmark continuation")
    console = replace(console, '\tconst auto periodic = GetPassengerServiceDatabase().GetDiagnostic();',
                      '\tIConsolePrint(CC_DEFAULT, "Profile tick quantiles: simulation_p50_us={}; journey_p50_us={}",\n'
                      '\t\tquantile(stats.simulation_histogram, 50), quantile(stats.journey_histogram, 50));\n'
                      '\tfor (size_t i = 0; i < stats.simulation_histogram.size(); ++i) if (stats.simulation_histogram[i] || stats.journey_histogram[i])\n'
                      '\t\tIConsolePrint(CC_DEFAULT, "Profile tick bucket: index={}; simulation={}; journey={}", i, stats.simulation_histogram[i], stats.journey_histogram[i]);\n'
                      '\tconst auto periodic = GetPassengerServiceDatabase().GetDiagnostic();')
    texts["console_cmds.cpp"] = console
    changed = ["console_cmds.cpp"]
    if mode == "detail":
        header = r'''#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <cstdio>
namespace JourneyProfile {
struct Metric { uint64_t calls = 0, ns = 0; };
inline std::map<std::string, Metric, std::less<>> metrics;
inline bool enabled = false;
inline Metric &Get(const char *name) {
    auto it = metrics.find(name);
    if (it == metrics.end()) it = metrics.emplace(name, Metric{}).first;
    return it->second;
}
inline void Count(const char *name) { if (enabled) ++Get(name).calls; }
template <typename T> inline T Observe(const char *name, T value) { Count(name); return value; }
struct Scope {
    Metric *metric;
    std::chrono::steady_clock::time_point start;
    explicit Scope(const char *name) : metric(enabled ? &Get(name) : nullptr) {
        if (metric) { ++metric->calls; start = std::chrono::steady_clock::now(); }
    }
    ~Scope() { if (metric) metric->ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count(); }
};
inline void Reset() { metrics.clear(); enabled = true; }
inline void Dump() {
    FILE *f = std::fopen("detail-scopes.json", "w");
    if (f == nullptr) return;
    std::fputs("{\"schema_version\":1,\"kind\":\"detail\",\"clock\":\"steady_clock elapsed on simulation thread\",\"metrics\":{", f);
    bool first = true;
    for (const auto &[name, metric] : metrics) {
        std::fprintf(f, "%s\"%s\":{\"calls\":%llu,\"inclusive_ns\":%llu}", first ? "" : ",", name.c_str(),
            static_cast<unsigned long long>(metric.calls), static_cast<unsigned long long>(metric.ns)); first = false;
    }
    std::fputs("}}\n", f); std::fclose(f);
}
}
'''
        (output / "profile.h").write_text(header, encoding="utf-8")
        texts["console_cmds.cpp"] = replace(texts["console_cmds.cpp"],
            'if (operation == "reset") { ResetPassengerJourneyDiagnostic(); return true; }',
            'if (operation == "reset") { ResetPassengerJourneyDiagnostic(); JourneyProfile::Reset(); return true; }')
        texts["console_cmds.cpp"] = replace(texts["console_cmds.cpp"],
            '\tconst auto stats = operation == "tick" ? GetPassengerJourneyTickDiagnostic() : GetPassengerJourneyTotalDiagnostic();',
            '\tJourneyProfile::Dump();\n\tconst auto stats = operation == "tick" ? GetPassengerJourneyTickDiagnostic() : GetPassengerJourneyTotalDiagnostic();')
        db = texts["passenger_service_database.cpp"]
        db = function_scope(db, "PassengerPeriodicPlan PassengerServiceDatabase::Plan(", "timed.plan")
        db = function_scope(db, "PassengerServiceDatabase::JourneyPolicy PassengerServiceDatabase::CompilePolicy(", "timed.reverse_lower_bound")
        db = function_scope(db, "std::optional<StateTicks> PassengerPeriodicTimetable::NextDeparture(", "timed.next_departure")
        db = function_scope(db, "bool PassengerServiceDatabase::IsCurrent(", "loading.intent_current")
        db = function_scope(db, "bool ValidatePassengerServiceRide(", "loading.live_vehicle_validation")
        db = replace(db, '\t\tauto limited = [&](Reason reason, int64_t lower) {',
                     '\t\tauto limited = [&](Reason reason, int64_t lower) {\n'
                     '\t\t\tif (reason == Reason::Work) JourneyProfile::Count("timed.forward_work_stops");\n'
                     '\t\t\tif (reason == Reason::Labels) JourneyProfile::Count("timed.forward_label_stops");\n'
                     '\t\t\tif (reason == Reason::Memory) JourneyProfile::Count("timed.forward_memory_stops");')
        db = replace(db, '\t\tstruct Label {', '\t\tJourneyProfile::Scope forward_scope{"timed.forward"};\n\t\tstruct Label {')
        db = replace(db, '\t\tauto insert = [&](Label next) {', '\t\tauto insert = [&](Label next) {\n\t\t\tJourneyProfile::Count("timed.label_attempts");')
        db = replace(db, '\t\t\t\tconst auto &other = labels[index];', '\t\t\t\tJourneyProfile::Count("timed.dominance_checks");\n\t\t\t\tconst auto &other = labels[index];')
        db = replace(db, '\t\t\tqueue.emplace(lower, next.cost,', '\t\t\tJourneyProfile::Count("timed.queue_push");\n\t\t\tqueue.emplace(lower, next.cost,')
        db = replace(db, '\t\t\tsize_t index = std::get<8>(queue.top()); queue.pop();', '\t\t\tJourneyProfile::Count("timed.queue_pop");\n\t\t\tsize_t index = std::get<8>(queue.top()); queue.pop();')
        db = replace(db, '\t\t\tfor (const auto &ref : row->second) {', '\t\t\tfor (const auto &ref : row->second) {\n\t\t\t\tJourneyProfile::Count("timed.candidate_service_visits");')
        db = replace(db, '\t\t\t\tfor (const auto &vehicle : service.vehicles) {', '\t\t\t\tJourneyProfile::Scope phase_scope{"timed.vehicle_phase_scan"};\n\t\t\t\tfor (const auto &vehicle : service.vehicles) {\n\t\t\t\t\tJourneyProfile::Count("timed.vehicle_phase_visits");')
        # Explicit scope boundary avoids counting the following alighting loop.
        db = replace(db, '\t\t\t\tJourneyProfile::Scope phase_scope', '\t\t\t\t{ JourneyProfile::Scope phase_scope')
        db = replace(db, '\t\t\t\tif (result.work >= limits.work) { limited(Reason::Work, 0); break; }', '\t\t\t\t}\n\t\t\t\tif (result.work >= limits.work) { limited(Reason::Work, 0); break; }')
        db = replace(db, '\t\t\t\t\t\tconst Vehicle *live = Vehicle::GetIfValid(vehicle.id);', '\t\t\t\t\t\tJourneyProfile::Count("timed.live_vehicle_reads");\n\t\t\t\t\t\tconst Vehicle *live = Vehicle::GetIfValid(vehicle.id);')
        # Count actual short-circuit evaluations, preserving the boolean expression.
        start, end = db.index('PassengerPeriodicPlan PassengerServiceDatabase::Plan('), db.index('PassengerPlannerComparison ComparePassengerPlanners(')
        plan = db[start:end]
        plan = replace(plan, 'auto spend = [&]() {', 'auto spend = [&](const char *stage) {')
        plan = replace(plan, 'if (result.work >= limits.work) { limited(Reason::Work, 0); return false; }',
                       'if (result.work >= limits.work) { JourneyProfile::Count(stage); limited(Reason::Work, 0); return false; }')
        plan = replace(plan, 'if (!spend()) return;', 'if (!spend("timed.work_stop.dominance")) return;')
        for indent, stage in [(3, 'queue'), (4, 'candidate_service'), (5, 'vehicle_phase'), (7, 'dispatch_slot'), (5, 'alighting')]:
            # Vehicle and alighting have equal indentation; use their adjacent anchors.
            if stage == 'vehicle_phase':
                old = 'if (!spend()) break;\n\t\t\t\t\tif (!vehicle.capacity[cargo])'
                new = 'if (!spend("timed.work_stop.vehicle_phase")) break;\n\t\t\t\t\tif (!vehicle.capacity[cargo])'
            elif stage == 'alighting':
                old = 'if (!spend()) break;\n\t\t\t\t\tsize_t stop_index'
                new = 'if (!spend("timed.work_stop.alighting")) break;\n\t\t\t\t\tsize_t stop_index'
            else:
                old = '\n' + '\t' * indent + 'if (!spend()) break;'
                new = '\n' + '\t' * indent + f'if (!spend("timed.work_stop.{stage}")) break;'
            plan = replace(plan, old, new)
        for station in ('origin', 'destination'):
            plan = replace(plan, f'Station::IsValidID({station})',
                           f'JourneyProfile::Observe("timed.live_station_checks", Station::IsValidID({station}))')
        db = db[:start] + plan + db[end:]
        texts["passenger_service_database.cpp"] = db
        active = texts["passenger_journey_active.cpp"]
        for signature, name in [
            ("uint32_t PassengerOdFallbackContext::Distance(", "fallback.distance"),
            ("bool PassengerJourneyBoardingContext::ValidateServiceRide(", "loading.intent_and_live_ride"),
            ("bool PassengerJourneyBoardingContext::CanBoard(", "loading.can_board"),
            ("size_t StationCargoList::RoutePassengerJourneys(", "loading.waiting_scan")]:
            active = function_scope(active, signature, name)
        active = replace(active, '\t\tif (event_binding == this->bindings.end()) {', '\t\tif (event_binding == this->bindings.end()) {\n\t\t\tJourneyProfile::Scope binding_scope{"fallback.node_binding"};')
        active = replace(active, '\t\t\tfor (NodeID node = 0; matches && node < graph->Size(); ++node) matches = (*resident_binding->second.nodes)[node] == std::pair((*graph)[node].Station(), valid_node(node));',
            '\t\t\t{ JourneyProfile::Scope validate_scope{"fallback.node_binding_validation"};\n\t\t\tfor (NodeID node = 0; matches && node < graph->Size(); ++node) { JourneyProfile::Count("fallback.node_binding_nodes"); matches = (*resident_binding->second.nodes)[node] == std::pair((*graph)[node].Station(), valid_node(node)); } }')
        active = replace(active, '\t\t\tif (!matches) {', '\t\t\tif (!matches) {\n\t\t\t\tJourneyProfile::Scope rebuild_scope{"fallback.node_binding_rebuild"};')
        active = replace(active, '\t\tif (resident != resident_distances.end()) {', '\t\tif (resident != resident_distances.end()) {\n\t\t\tJourneyProfile::Scope reuse_scope{"fallback.cache_reuse"};')
        active = replace(active, '\t\t\tauto built = std::make_shared<ReverseIndex>();', '\t\t\tJourneyProfile::Scope reverse_scope{"fallback.reverse_index"};\n\t\t\tauto built = std::make_shared<ReverseIndex>();')
        active = replace(active, '\t\tfor (size_t head = 0; head < queue.size(); ++head) {', '\t\t{ JourneyProfile::Scope bfs_scope{"fallback.bfs"};\n\t\tfor (size_t head = 0; head < queue.size(); ++head) {')
        active = replace(active, '\t\tcached->second.graph = graph->index;\n\t\tcached->second.hops = std::make_shared', '\t\t}\n\t\tcached->second.graph = graph->index;\n\t\tcached->second.hops = std::make_shared')
        active = replace(active, '\t\tfor (CargoPacket *packet : list) {', '\t\tfor (CargoPacket *packet : list) {\n\t\t\tJourneyProfile::Count("loading.waiting_packet_visits");')
        active = replace(active, '\tfor (const auto &[key, packet] : moved) this->packets.Insert(key, packet);',
                         '\t{ JourneyProfile::Scope reclass_scope{"loading.waiting_reinsert"};\n\tfor (const auto &[key, packet] : moved) { JourneyProfile::Count("loading.waiting_reinserted_packets"); this->packets.Insert(key, packet); } }')
        texts["passenger_journey_active.cpp"] = active
        cargo = texts["cargopacket.cpp"]
        # Actual movement/reclassification scopes include validation children.
        for signature, name in [("void StationCargoList::Append(", "loading.packet_append"),
                                ("bool VehicleCargoList::Stage(", "loading.packet_stage"),
                                ("uint StationCargoList::Load(", "loading.packet_load")]:
            cargo = function_scope(cargo, signature, name)
        texts["cargopacket.cpp"] = cargo
        schedule = texts["passenger_journey_schedule.cpp"]
        for signature, name in [('std::optional<Frequency> Estimate(', 'schedule.wait_estimate'),
                                ('PassengerSchedulePlan PassengerSchedulePlanner::Search(', 'schedule.search'),
                                ('PassengerSchedulePlan PassengerSchedulePlanner::Plan(', 'schedule.plan')]:
            schedule = function_scope(schedule, signature, name)
        texts["passenger_journey_schedule.cpp"] = schedule
        routing = texts["passenger_routing.cpp"]
        routing = function_scope(routing, 'bool CanPassengerVehicleUseStation(', 'loading.station_facilities')
        routing = replace(routing, '\tconst Station *st = Station::GetIfValid(station);',
                          '\tJourneyProfile::Count("loading.live_station_reads");\n\tconst Station *st = Station::GetIfValid(station);')
        texts["passenger_routing.cpp"] = routing
        changed = list(texts)
    project = build / "openttd_lib.vcxproj"
    project_text = project.read_text(encoding="utf-8-sig")
    hashes = {}
    for name in changed:
        text = texts[name]
        if mode == 'detail' and name != 'console_cmds.cpp':
            for function, metric in [('Station::GetIfValid', 'live.station_get_if_valid'), ('Station::Get', 'live.station_get'),
                                     ('Station::IsValidID', 'live.station_is_valid')]:
                text = observe_calls(text, function, metric)
        # Absolute includes let copied TUs keep the real source's relative includes.
        text = re.sub(r'#include "([^"]+)"',
                      lambda m: '#include "' + (source / "src" / m[1]).as_posix() + '"' if (source / "src" / m[1]).is_file() else m[0], text)
        if mode == "detail":
            text = replace(text, '#include "' + (source / "src/stdafx.h").as_posix() + '"',
                           '#include "' + (source / "src/stdafx.h").as_posix() + '"\n#include "' + (output / "profile.h").as_posix() + '"')
        target = output / name
        target.write_text(text, encoding="utf-8")
        project_text = replace(project_text, str(source / "src" / name), str(target))
        hashes[name] = {"original": hashlib.sha256((source / "src" / name).read_bytes()).hexdigest(),
                        "copy": hashlib.sha256(target.read_bytes()).hexdigest()}
    project.write_text(project_text, encoding="utf-8-sig")
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    diff = subprocess.check_output(['git', '-C', str(source), 'diff', 'HEAD', '--', 'src', 'cmake', 'CMakeLists.txt'])
    (output / "instrumentation.json").write_text(json.dumps({"mode": mode, "files": hashes,
        "source_revision": revision, "source_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "instrumenter_sha256": hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()}, indent=2), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("build", type=pathlib.Path)
    parser.add_argument("--mode", choices=["minimal", "detail"], default="minimal")
    args = parser.parse_args()
    prepare(args.source, args.build, args.mode)
