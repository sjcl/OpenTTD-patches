#!/usr/bin/env python3
"""Add opt-in per-tick retirement observations to copied runtime TUs only.

Run after passenger_estimated_prepare.py --cpu. Product files are never edited.
"""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("build", type=pathlib.Path)
    parser.add_argument("--reference", help="Embed the pre-A3 retirement path for same-image comparisons")
    args = parser.parse_args()
    source, build = args.source.resolve(), args.build.resolve()
    original = source / "src/passenger_precomputed.cpp"
    cpp = original.read_text(encoding="utf-8")
    if args.reference:
        before = subprocess.check_output(["git", "show", f"{args.reference}:src/passenger_precomputed.cpp"], cwd=source).decode()
        release = before.split("void PassengerPrecomputedTable::ReleaseDestination(StationID destination)\n{", 1)[1].split("\n}\n", 1)[0]
        reachable = before.split("std::span<const StationID> PassengerPrecomputedTable::ReachableDestinations(StationID origin) const\n{", 1)[1].split("\n}\n", 1)[0]
        reachable = reachable.replace("return result;", "return {result, {}};")
        mode = 'getenv("OPENTTD_RETIREMENT_REFERENCE") != nullptr'
        anchor = "void PassengerPrecomputedTable::ReleaseDestination(StationID destination)\n{"
        cpp = cpp.replace(anchor, anchor + f"\n    if ({mode}) {{" + release + "\n        return;\n    }\n")
        anchor = "PassengerPrecomputedTable::DestinationSpans PassengerPrecomputedTable::ReachableDestinations(StationID origin) const\n{"
        assert cpp.count(anchor) == 1
        cpp = cpp.replace(anchor, anchor + f"\n    if ({mode}) {{" + reachable + "\n    }\n")
        anchor = "void PassengerPrecomputedTable::PrepareRetirement(const PassengerPrecomputedTable &replacement)\n{"
        assert cpp.count(anchor) == 1
        cpp = cpp.replace(anchor, anchor + f"\n    if ({mode}) return;\n")
    cpp = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (original.parent / m[1]).resolve().as_posix() + '"' if (original.parent / m[1]).is_file() else m[0], cpp)
    anchor = "void PassengerPrecomputedTable::ReleaseDestination(StationID destination)\n{"
    assert cpp.count(anchor) == 1
    cpp = cpp.replace(anchor, "uint64_t _passenger_retirement_ns = 0, _passenger_retirement_calls = 0;\n" + anchor + """
    struct Observation {
        bool enabled = getenv("OPENTTD_RETIREMENT_PROFILE") != nullptr;
        Clock::time_point start = enabled ? Clock::now() : Clock::time_point{};
        ~Observation() { if (enabled) { _passenger_retirement_ns += Elapsed(start); ++_passenger_retirement_calls; } }
    } observation;
""")
    target = build / "real-map-driver/passenger_precomputed.cpp"
    target.write_text(cpp, encoding="utf-8")
    project = build / "openttd_lib.vcxproj"
    xml = project.read_text(encoding="utf-8-sig")
    assert xml.count(str(original)) == 1
    project.write_text(xml.replace(str(original), str(target)), encoding="utf-8-sig")
    driver = build / "real-map-driver/null_v.cpp"
    cpp = driver.read_text(encoding="utf-8")
    cpp = cpp.replace("struct Sample { int64_t tick; uint64_t simulation, journey; };", "extern uint64_t _passenger_retirement_ns, _passenger_retirement_calls;\n        struct Sample { int64_t tick; uint64_t simulation, journey, retirement_ns, retirement_calls; };")
    cpp = cpp.replace("int64_t before = _state_ticks.base();", "int64_t before = _state_ticks.base();\n            _passenger_retirement_ns = _passenger_retirement_calls = 0;")
    cpp = cpp.replace("stats.simulation_microseconds, stats.journey_microseconds}", "stats.simulation_microseconds, stats.journey_microseconds, _passenger_retirement_ns, _passenger_retirement_calls}")
    cpp = cpp.replace("<< sample.journey << ']'", "<< sample.journey << ',' << sample.retirement_ns << ',' << sample.retirement_calls << ']'")
    driver.write_text(cpp, encoding="utf-8")
    console = build / "save-integrity/console_cmds.cpp"
    cpp = console.read_text(encoding="utf-8")
    anchor = 'if (operation == "reset") { ResetPassengerJourneyDiagnostic(); return true; }'
    assert cpp.count(anchor) == 1
    cpp = cpp.replace(anchor, 'if (operation == "reset") { extern uint64_t _passenger_retirement_ns, _passenger_retirement_calls; _passenger_retirement_ns = _passenger_retirement_calls = 0; ResetPassengerJourneyDiagnostic(); return true; }')
    console.write_text(cpp, encoding="utf-8")
    metadata = dict(reference=args.reference, product_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),
                    files={p.relative_to(build).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in (target, driver, console)},
                    note="Copied translation units only; profiling requires OPENTTD_RETIREMENT_PROFILE; reference path requires OPENTTD_RETIREMENT_REFERENCE")
    (build / "retirement-instrumentation.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
