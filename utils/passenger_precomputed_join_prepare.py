"""Prepare copied measurement TUs for real loopback joins, without product hooks.

Adds canonical DB, packet ownership, reservation/loading and delivery digests to
the existing inspector. The null client records replay slices and accepts local
probe scripts after a normal synchronized join. Network protocol/timeouts and
simulation are the product implementation.
"""
import argparse
import pathlib
import re

import passenger_precomputed_real_map as real


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("build", type=pathlib.Path)
    parser.add_argument("revision")
    args = parser.parse_args()
    source, build = args.source.resolve(), args.build.resolve()
    real.prepare(args)
    real.stdio(args)
    header = build / "save-integrity/integrity.h"
    content = header.read_text(encoding="utf-8")
    includes = "".join(f'#include "{(source / "src" / name).as_posix()}"\n' for name in (
        "passenger_service_database.h", "station_base.h", "vehicle_base.h", "company_base.h", "settings_type.h"))
    content = content.replace("namespace SaveIntegrity {", includes + "namespace SaveIntegrity {")
    anchor = '    std::ofstream out("save-integrity.json");'
    assert content.count(anchor) == 1
    content = content.replace(anchor, r'''
    uint64_t database = 14695981039346656037ULL, ownership = database, loading = database, deliveries = 0;
    auto &db = GetPassengerServiceDatabase();
    for (uint8_t byte : db.Save()) { database ^= byte; database *= 1099511628211ULL; }
    uint64_t rows = 0, retained = 0, operations = 0;
    for (CargoType cargo : EnumRange(NUM_CARGO)) {
        auto d = db.GetPrecomputedDiagnostic(cargo);
        rows += d.destinations; retained += d.retained_destinations; operations += d.operations;
    }
    for (const Station *station : Station::Iterate()) {
        Mix(loading, station->index, station->loading_vehicles.size());
        for (const Vehicle *v : station->loading_vehicles) Mix(loading, v->index);
        for (CargoType cargo : EnumRange(NUM_CARGO)) if (station->goods[cargo].data && !station->goods[cargo].data->MayBeRemoved()) {
            const auto &list = station->goods[cargo].data->cargo;
            Mix(ownership, station->index, cargo, list.TotalCount(), list.ReservedCount(), list.Packets()->size());
            for (const auto &[via, packets] : *list.Packets()) {
                Mix(ownership, via, packets.size());
                for (const CargoPacket *cp : packets) Mix(ownership, cp->index);
            }
        }
    }
    for (const Vehicle *v : Vehicle::Iterate()) {
        Mix(ownership, v->index, v->cargo.TotalCount(), v->cargo.StoredCount(), v->cargo.ReservedCount(), v->cargo.Packets()->size());
        for (const CargoPacket *cp : *v->cargo.Packets()) Mix(ownership, cp->index);
        for (auto action : EnumRange(VehicleCargoList::MoveToAction::End)) Mix(loading, v->index, action, v->cargo.ActionCount(action));
        Mix(loading, v->index, v->tile, v->x_pos, v->y_pos, v->z_pos, v->cur_speed, v->load_unload_ticks,
            v->cur_real_order_index, v->cur_implicit_order_index, v->last_station_visited);
    }
    for (const Company *company : Company::Iterate()) for (CargoType cargo : EnumRange(NUM_CARGO)) {
        if (IsCargoInClass(cargo, CargoClass::Passengers)) deliveries += company->cur_economy.delivered_cargo[cargo];
    }
''' + anchor)
    # Insert before the existing historical fields, preserving the original inspector.
    anchor = r'        << ",\"legacy_true_packets\":"'
    assert content.count(anchor) == 1
    content = content.replace(anchor, r'''
        << ",\"database_digest\":" << database << ",\"ownership_digest\":" << ownership << ",\"loading_digest\":" << loading
        << ",\"completed_rows\":" << rows << ",\"retained_rows\":" << retained << ",\"table_operations\":" << operations
        << ",\"published_revision\":" << db.GetPublishedRevision() << ",\"company_deliveries\":" << deliveries
        << ",\"journey_routing\":" << _settings_game.linkgraph.passenger_journey_routing
        << ",\"precomputed_routing\":" << _settings_game.linkgraph.passenger_journey_precomputed
''' + anchor)
    header.write_text(content, encoding="utf-8")
    # v8 schedules packet events across ticks. Compare this saved simulation
    # state too; local diagnostics and reconstructed iterator ranks are excluded.
    if "passenger_routing_event" in (source / "src/cargopacket.h").read_text(encoding="utf-8"):
        packet = build / "save-integrity/cargopacket_sl.cpp"
        text = packet.read_text(encoding="utf-8")
        anchor = '        Mix(state, cp.index, merge,'
        assert text.count(anchor) == 1
        packet.write_text(text.replace(anchor, '        Mix(state, cp.passenger_routing_event);\n' + anchor), encoding="utf-8")
        anchor = '            Mix(ownership, station->index, cargo, list.TotalCount()'
        assert content.count(anchor) == 1
        content = content.replace(anchor, '''            Mix(ownership, list.passenger_routing_dirty, list.passenger_routing_revision, list.passenger_routing_rows,
                list.passenger_routing_due, list.passenger_routing_sweeping, list.passenger_routing_cursor, list.passenger_routing_end,
                list.passenger_routing_tick, list.passenger_routing_work);
''' + anchor)
        header.write_text(content, encoding="utf-8")
    console = build / "save-integrity/console_cmds.cpp"
    text = console.read_text(encoding="utf-8")
    anchor = '    if (operation == "build_precomputed") {'
    assert text.count(anchor) == 1
    text = text.replace(anchor, '''    if (operation == "partial_precomputed") {
        GetPassengerServiceDatabase().TickPrecomputed(64 * 1024 * 1024);
        return true;
    }
''' + anchor)
    console.write_text(text, encoding="utf-8")

    original = source / "src/video/null_v.cpp"
    content = original.read_text(encoding="utf-8")
    content = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (original.parent / m[1]).resolve().as_posix() + '"', content)
    includes = "".join(f'#include "{(source / "src" / name).as_posix()}"\n' for name in (
        "openttd.h", "date_func.h", "passenger_service_database.h", "network/network.h", "network/network_internal.h", "console_func.h"))
    content = content.replace("#include <atomic>", "#include <atomic>\n#include <fstream>\n#include <cstdio>\n" + includes)
    anchor = '''		while (!_exit_game) {
			::GameLoop();
			::InputLoop();
			::UpdateWindows();
		}'''
    assert content.count(anchor) == 1
    content = content.replace(anchor, r'''
        bool observed = false, replay_started = false;
        uint64_t slices = 0, maximum_us = 0;
        int64_t start_tick = 0;
        uint32_t start_frame = 0;
        while (!_exit_game) {
            auto &db = GetPassengerServiceDatabase();
            bool replay = db.GetLoadState() == PassengerServiceDatabase::LoadState::Replaying;
            if (replay && !replay_started) { replay_started = true; start_tick = _state_ticks.base(); start_frame = _frame_counter; }
            auto started = std::chrono::steady_clock::now();
            ::GameLoop(); ::InputLoop(); ::UpdateWindows();
            if (replay) {
                ++slices;
                maximum_us = std::max<uint64_t>(maximum_us, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
            }
            if (replay_started && db.GetLoadState() == PassengerServiceDatabase::LoadState::Ready && !observed) {
                std::ofstream out("join-replay.json");
                out << "{\"slices\":" << slices << ",\"maximum_loop_us\":" << maximum_us << ",\"work\":" << db.GetLoadWork()
                    << ",\"start_tick\":" << start_tick << ",\"end_tick\":" << _state_ticks.base()
                    << ",\"start_frame\":" << start_frame << ",\"end_frame\":" << _frame_counter << "}\n";
                observed = true;
            }
            if (_networking && !_network_server && !_network_first_time) {
                std::ifstream request("probe-request.txt");
                if (request) {
                    std::vector<std::string> commands;
                    std::string command;
                    while (std::getline(request, command)) commands.push_back(command);
                    request.close(); std::remove("probe-request.txt");
                    for (const auto &line : commands) IConsoleCmdExec(line);
                }
            }
            CSleep(1);
        }
''')
    target = build / "real-map-driver/null_v.cpp"
    target.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
