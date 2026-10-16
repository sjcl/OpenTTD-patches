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
    # Lifetime table counters disappear when a generation is replaced. Keep a
    # measurement-only total across current/retained tables, reset with the
    # ordinary console diagnostic reset. This never enters routing/save state.
    metrics = build / 'save-integrity/hyperpath_metrics.h'
    metrics.write_text('#pragma once\n#include <cstdint>\nnamespace PassengerHyperpathMetrics { inline uint64_t replans = 0, nanoseconds = 0; }\n', encoding='utf-8')
    original = source / 'src/passenger_precomputed.cpp'
    copied = build / 'save-integrity/passenger_precomputed.cpp'
    text = original.read_text(encoding='utf-8')
    text = re.sub(r'#include "([^"]+)"', lambda match: '#include "' + (original.parent / match[1]).resolve().as_posix() + '"', text)
    text = text.replace('#include "' + (source / 'src/stdafx.h').as_posix() + '"', '#include "' + (source / 'src/stdafx.h').as_posix() + '"\n#include "' + metrics.as_posix() + '"')
    anchor = '++this->diagnostic.local_replans; this->diagnostic.local_replan_nanoseconds += Elapsed(started);'
    if text.count(anchor) != 1:
        raise ValueError('Local-policy measurement anchor changed')
    text = text.replace(anchor, 'auto elapsed = Elapsed(started); ++this->diagnostic.local_replans; this->diagnostic.local_replan_nanoseconds += elapsed;\n'
                               '\t++PassengerHyperpathMetrics::replans; PassengerHyperpathMetrics::nanoseconds += elapsed;')
    copied.write_text(text, encoding='utf-8')
    replace_once(build / 'openttd_lib.vcxproj', str(original), str(copied))
    path = build / "save-integrity/cargopacket_sl.cpp"
    before = "        const auto &r = cp.passenger_journey;"
    replace_once(path, before, "        const auto &failure = cp.passenger_boarding_failure;\n        Mix(merge, failure.service, failure.count, failure.since, failure.incarnation);\n" + before)
    before = "        Mix(state, cp.index, merge, cp.count, cp.feeder_share, cp.next_hop, cp.travelled.x, cp.travelled.y);"
    replace_once(path, before, before.replace("cp.travelled.y", "cp.travelled.y, cp.passenger_routing_event"))
    if args.mp:
        header = build / "save-integrity/integrity.h"
        header.write_text(header.read_text(encoding="utf-8") + '\n#include "' + (source / "utils/passenger_hyperpath_merge_census.h").as_posix() + '"\n', encoding="utf-8")
        before = "NamedSaveLoadTable GetCargoPacketDesc()\n{"
        replace_once(path, before, before + '''
    SaveIntegrity::neutral_fields = [](const CargoPacket &cp) {
        std::vector<uint64_t> key;
        auto add = [&](auto... values) { (key.push_back(SaveIntegrity::Number(values)), ...); };
        add(cp.source_xy, cp.periods_in_transit, cp.first_station, cp.source.type, cp.source.id, cp.passenger_via, cp.passenger_destination,
            cp.passenger_routing_mode, cp.passenger_legacy_hop, cp.passenger_retry_station, cp.passenger_retry_after, cp.passenger_interchange_ready,
            cp.passenger_arriving_service, cp.passenger_arriving_incarnation, cp.passenger_transport, cp.passenger_interchange_transport, cp.passenger_schedule_used);
        const auto &r = cp.passenger_journey;
        add(r.vehicle, r.service, r.boarding, r.alighting, r.boarding_order, r.alighting_order, r.departure, r.arrival, r.signature,
            r.kind, r.generation, r.not_before, r.planned_at, r.estimated_wait, r.waiting_limit, r.timed_deferred);
        return key;
    };
''')
    path = build / "save-integrity/console_cmds.cpp"
    before = '    if (operation == "partial_precomputed") {'
    replace_once(path, before, '''    if (operation == "replay_precomputed") {
        auto &db = GetPassengerServiceDatabase();
        auto before = db.Save(); const auto tick = _state_ticks;
        std::ofstream wire("hyperpath-replay-before.bin", std::ios::binary);
        wire.write(reinterpret_cast<const char *>(before.data()), static_cast<std::streamsize>(before.size())); wire.close();
        while (db.StepLoad(65536) == PassengerServiceDatabase::LoadState::Replaying) {}
        const bool identical = before == db.Save() && tick == _state_ticks;
        IConsolePrint(CC_DEFAULT, "Hyperpath replay: ready={}; saved_state_identical={}; tick={}",
            db.GetLoadState() == PassengerServiceDatabase::LoadState::Ready, identical, _state_ticks.base());
        return identical && db.GetLoadState() == PassengerServiceDatabase::LoadState::Ready;
    }
''' + before)
    stdafx = '#include "' + (source / 'src/stdafx.h').as_posix() + '"'
    replace_once(path, stdafx, stdafx + '\n#include "' + metrics.as_posix() + '"')
    replace_once(path, 'if (operation == "reset") { ResetPassengerJourneyDiagnostic(); return true; }',
                 'if (operation == "reset") { PassengerHyperpathMetrics::replans = PassengerHyperpathMetrics::nanoseconds = 0; ResetPassengerJourneyDiagnostic(); return true; }')
    if args.mp:
        before = '\tif (operation == "save_integrity")'
        replace_once(path, before, '\tif (operation == "hyperpath_merge_census") { SaveIntegrity::MergeCensus(); return true; }\n' + before)
    before = "\tconst auto events = GetPassengerWaitingEventDiagnostic();"
    replace_once(path, before, before + '''
    IConsolePrint(CC_DEFAULT, "Hyperpath capacity: packets={}; passengers={}", events.capacity_missed_packets, events.capacity_missed_passengers);
    IConsolePrint(CC_DEFAULT, "Hyperpath local totals: replans={}; nanoseconds={}", PassengerHyperpathMetrics::replans, PassengerHyperpathMetrics::nanoseconds);
    for (CargoType cargo : EnumRange(NUM_CARGO)) if (IsCargoInClass(cargo, CargoClass::Passengers)) {
        auto hp = GetPassengerServiceDatabase().GetPrecomputedDiagnostic(cargo);
        IConsolePrint(CC_DEFAULT, "Hyperpath diagnostics: cargo={}; local_replans={}; local_ns={}; board_events={}; accepted={}; range_updates={}; pushes={}; pops={}; bytes={}; envelope={}; base_bytes={}; difference_bytes={}; max_candidates={}; work={}; build_ns={}; transfer={}",
            cargo, hp.local_replans, hp.local_replan_nanoseconds, hp.board_events, hp.board_accepts, hp.range_updates, hp.heap_pushes, hp.heap_pops, hp.bytes, hp.envelope, hp.base_policy_bytes, hp.difference_policy_bytes, hp.max_candidates, hp.operations, hp.build_nanoseconds, _settings_game.linkgraph.passenger_transfer_penalty);
    }
''')
    if not args.mp:
        path = build / "transport-trace/trace.h"
        if not path.is_file():
            path = next((build / "transport-trace").glob("*.h"))
        before = 'if (!db.RebindPrecomputed(rebound, vehicle->orders->index, vehicle->cargo_type, vehicle->cur_real_order_index)) reason = "intent_mismatch";'
        after = '''if (vehicle->current_order.GetCargoLoadType(vehicle->cargo_type) == OrderLoadType::NoLoad) reason = "no_load";
            else if (ride.kind == PassengerRideKind::HyperpathIntent) {
                auto board = db.BoardHyperpath(ride, vehicle->cargo_type, vehicle->orders->index, vehicle->cur_real_order_index, packet.GetPassengerArrivingService(), packet.GetPassengerFailedService(_state_ticks));
                if (!board.ride) reason = "not_attractive";
            } else if (!db.RebindPrecomputed(rebound, vehicle->orders->index, vehicle->cargo_type, vehicle->cur_real_order_index)) reason = "intent_mismatch";'''
        replace_once(path, before, after)
        replace_once(path, '\n\t\t\telse if (vehicle->current_order.GetCargoLoadType(vehicle->cargo_type) == OrderLoadType::NoLoad) reason = "no_load";', '')
        # Two separate observers share the same immutable simulation. The
        # ordinary 1/64 sample and exhaustive target-OD sample never mix.
        trace = path.read_text(encoding="utf-8")
        focused = trace.replace("namespace PassengerTransportTrace", "namespace PassengerHyperpathFocusTrace")
        focused = focused.replace('getenv("OPENTTD_PASSENGER_FOCUS_TRACE") != nullptr', "true")
        focused = focused.replace('"transport-', '"focused-transport-').replace('"Transport ', '"Focused transport ')
        focused = focused.replace('sampling=deterministic hash of packet ID and current birth tick; approximately 1/64; maximum 65536 sample starts',
                                  'sampling=all packet representatives for the three target ODs; maximum 65536 sample starts')
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
    patch = subprocess.check_output(['git', 'diff', args.revision, '--', 'src'], cwd=source)
    (build / 'hyperpath-source-diff.patch').write_bytes(patch)
    changed = subprocess.check_output(['git', 'diff', '--name-only', args.revision, '--', 'src'], cwd=source, text=True).splitlines()
    source_files = {name: hashlib.sha256((source / name).read_bytes()).hexdigest() for name in changed if (source / name).is_file()}
    (build / "hyperpath-provenance.json").write_text(json.dumps(dict(revision=args.revision, mp=args.mp, files=files,
        working_tree_source_files=source_files, working_tree_patch_sha256=hashlib.sha256(patch).hexdigest()), indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
