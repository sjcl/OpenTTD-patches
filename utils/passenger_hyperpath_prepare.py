#!/usr/bin/env python3
"""Prepare isolated transport/MP probes including the strategy failure state.

Uses existing measurement helpers; never edits product translation units.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import sys


def replace_once(path, before, after):
    text = path.read_text(encoding="utf-8")
    if text.count(before) != 1:
        raise ValueError(f"Measurement anchor changed in {path}: {before[:60]}")
    path.write_text(text.replace(before, after), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("build", type=Path)
    parser.add_argument("revision")
    parser.add_argument("--mp", action="store_true")
    args = parser.parse_args()
    source, build = args.source.resolve(), args.build.resolve()
    command = [sys.executable, "-B", str(source / "utils/passenger_estimated_prepare.py"), str(source), str(build), args.revision]
    if args.mp:
        command.append("--mp")
    subprocess.run(command, check=True)
    path = build / "save-integrity/cargopacket_sl.cpp"
    before = "        const auto &r = cp.passenger_journey;"
    replace_once(path, before, "        const auto &failure = cp.passenger_boarding_failure;\n        Mix(merge, failure.service, failure.count, failure.since, failure.incarnation);\n" + before)
    path = build / "save-integrity/console_cmds.cpp"
    before = "\tconst auto events = GetPassengerWaitingEventDiagnostic();"
    replace_once(path, before, before + '''
    IConsolePrint(CC_DEFAULT, "Hyperpath capacity: packets={}; passengers={}", events.capacity_missed_packets, events.capacity_missed_passengers);
    for (CargoType cargo : EnumRange(NUM_CARGO)) if (IsCargoInClass(cargo, CargoClass::Passengers)) {
        auto hp = GetPassengerServiceDatabase().GetPrecomputedDiagnostic(cargo);
        IConsolePrint(CC_DEFAULT, "Hyperpath diagnostics: cargo={}; local_replans={}; local_ns={}; board_events={}; accepted={}; range_updates={}; pushes={}; pops={}; bytes={}; envelope={}; base_bytes={}; difference_bytes={}; max_candidates={}",
            cargo, hp.local_replans, hp.local_replan_nanoseconds, hp.board_events, hp.board_accepts, hp.range_updates, hp.heap_pushes, hp.heap_pops, hp.bytes, hp.envelope, hp.base_policy_bytes, hp.difference_policy_bytes, hp.max_candidates);
    }
''')
    if not args.mp:
        path = build / "transport-trace/trace.h"
        if not path.is_file():
            path = next((build / "transport-trace").glob("*.h"))
        before = 'if (!db.RebindPrecomputed(rebound, vehicle->orders->index, vehicle->cargo_type, vehicle->cur_real_order_index)) reason = "intent_mismatch";'
        after = '''if (ride.kind == PassengerRideKind::HyperpathIntent) {
                auto board = db.BoardHyperpath(ride, vehicle->cargo_type, vehicle->orders->index, vehicle->cur_real_order_index, packet.GetPassengerArrivingService(), packet.GetPassengerFailedService(_state_ticks));
                if (!board.ride) reason = "not_attractive";
            } else if (!db.RebindPrecomputed(rebound, vehicle->orders->index, vehicle->cargo_type, vehicle->cur_real_order_index)) reason = "intent_mismatch";'''
        replace_once(path, before, after)
        # Two separate observers share the same immutable simulation. The
        # ordinary 1/64 sample and exhaustive target-OD sample never mix.
        trace = path.read_text(encoding="utf-8")
        focused = trace.replace("namespace PassengerTransportTrace", "namespace PassengerHyperpathFocusTrace")
        focused = focused.replace('getenv("OPENTTD_PASSENGER_FOCUS_TRACE") != nullptr', "true")
        focused = focused.replace('"transport-', '"focused-transport-').replace('"Transport ', '"Focused transport ')
        focus_header = path.with_name("focus_trace.h")
        focus_header.write_text(focused, encoding="utf-8")
        for folder in ("transport-trace", "save-integrity", "real-map-driver"):
            for copied in (build / folder).glob("*.cpp"):
                text = copied.read_text(encoding="utf-8")
                include = '#include "' + path.as_posix() + '"'
                if include not in text:
                    continue
                text = text.replace(include, include + '\n#include "' + focus_header.as_posix() + '"')
                matches = list(re.finditer(r"PassengerTransportTrace::(\w+)\(", text))
                for match in reversed(matches):
                    depth, end = 1, match.end()
                    while depth:
                        depth += (text[end] == "(") - (text[end] == ")")
                        end += 1
                    if text[end:end + 1] != ";":
                        raise ValueError("Trace observer hook must be a void statement")
                    call = text[match.start():end + 1]
                    text = text[:match.start()] + "{ " + call + " " + call.replace("PassengerTransportTrace::", "PassengerHyperpathFocusTrace::") + " }" + text[end + 1:]
                text = text.replace("PassengerTransportTrace::defer_next_dump = true;", "PassengerTransportTrace::defer_next_dump = true; PassengerHyperpathFocusTrace::defer_next_dump = true;")
                copied.write_text(text, encoding="utf-8")
    files = {}
    for folder in ("save-integrity", "transport-trace", "real-map-driver"):
        for path in (build / folder).glob("*"):
            if path.is_file():
                files[str(path.relative_to(build))] = hashlib.sha256(path.read_bytes()).hexdigest()
    (build / "hyperpath-provenance.json").write_text(json.dumps(dict(revision=args.revision, mp=args.mp, files=files), indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
