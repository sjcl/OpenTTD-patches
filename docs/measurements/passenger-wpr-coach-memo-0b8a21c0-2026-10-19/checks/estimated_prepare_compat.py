"""Prepare copied measurement TUs for estimated-pattern quality and MP checks.

Product sources are unchanged. The generated Visual Studio project is redirected
to copied translation units. Run after CMake generation, then build openttd.
Reconfigure CMake to restore the product translation units.
"""
import argparse
import pathlib
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("build", type=pathlib.Path)
    parser.add_argument("revision")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--mp", action="store_true", help="Keep the join inspector/mailbox driver; omit the physical trace driver")
    mode.add_argument("--cpu", action="store_true", help="Use the exact StateTicks driver without physical trace hooks")
    args = parser.parse_args()
    source, build = args.source.resolve(), args.build.resolve()
    steps = [("passenger_precomputed_join_prepare.py", [str(source), str(build), args.revision])]
    if not args.mp:
        steps.append(("passenger_precomputed_real_map.py", ["driver", str(source), str(build), args.revision]))
        if not args.cpu:
            steps.append(("passenger_transport_trace.py", [str(source), str(build)]))
    for tool, extra in steps:
        subprocess.run([sys.executable, "-B", str(source / "utils" / tool), *extra], check=True)
    target = build / "save-integrity/console_cmds.cpp"
    content = target.read_text(encoding="utf-8")
    anchor = '    if (operation == "partial_precomputed") {'
    assert content.count(anchor) == 1
    probe = r'''
    if (operation == "fixture_estimated") {
        /* Synthetic MP fixture only; never used for the real-map comparison. */
        for (OrderList *list : OrderList::Iterate()) {
            for (VehicleOrderID i = 0; i < list->GetNumOrders(); ++i) {
                Order *order = list->GetOrderAt(i);
                if (order->GetTravelTime() == 0) order->SetTravelTime(128);
                if (order->GetWaitTime() == 0) order->SetWaitTime(32);
                order->SetTravelTimetabled(false); order->SetWaitTimetabled(false);
            }
            list->RecalculateTimetableDuration();
        }
        GetPassengerServiceDatabase().Reset();
        InvalidatePassengerServiceDatabase();
        return true;
    }
    if (operation == "fixture_estimated_od") {
        uint64_t assigned = 0;
        auto &db = GetPassengerServiceDatabase();
        for (Station *station : Station::Iterate()) for (CargoType cargo : EnumRange(NUM_CARGO)) {
            if (!IsCargoInClass(cargo, CargoClass::Passengers) || station->goods[cargo].data == nullptr) continue;
            StationID target = StationID::Invalid();
            for (auto span : db.PrecomputedReachableDestinations(cargo, station->index)) for (auto destination : span) {
                if (destination != station->index && Station::IsValidID(destination)) { target = destination; break; }
            }
            if (target == StationID::Invalid()) continue;
            auto &list = station->goods[cargo].data->cargo;
            for (const auto &[via, packets] : *list.Packets()) for (CargoPacket *packet : packets) {
                if (packet->SetPassengerDestination(cargo, target)) assigned += packet->Count();
            }
            list.ClearWaitingIndex(); list.RebuildWaitingIndex();
        }
        IConsolePrint(CC_DEFAULT, "Estimated MP fixture: assigned_units={}", assigned);
        return assigned > 0;
    }
    if (operation == "quality_probe") {
        auto &db = GetPassengerServiceDatabase();
        for (uint16_t id : {302, 300, 160, 524, 525}) {
            const auto *s = db.Get(OrderListID{id});
            if (s == nullptr) { IConsolePrint(CC_DEFAULT, "Quality service: id={}; absent=true", id); continue; }
            IConsolePrint(CC_DEFAULT, "Quality service: id={}; certainty={}; period={}; estimate={}; stops={}; fleet={}; incarnation={}; generation={}",
                id, to_underlying(s->timetable.GetCertainty()), s->timetable.period, s->timetable.estimated_period,
                s->timetable.stops.size(), s->vehicles.size(), s->incarnation, s->generation);
            for (const auto &stop : s->timetable.stops) IConsolePrint(CC_DEFAULT,
                "Quality occurrence: id={}; station={}; order={}; arrival={}; departure={}; board={}; alight={}; forced={}; continues={}; quality={}",
                id, stop.station, stop.order, stop.arrival, stop.departure, stop.board, stop.alight, stop.forced, stop.continues, stop.travel_quality);
        }
        for (auto [origin, destination] : {std::pair{3222, 400}, std::pair{3221, 400}, std::pair{971, 916}, std::pair{387, 916}, std::pair{385, 916}, std::pair{3393, 3394}}) {
            StationID station{uint16_t(origin)}; OrderListID previous = OrderListID::Invalid();
            for (int leg = 0; leg < 16 && station != StationID{uint16_t(destination)}; ++leg) {
                auto p = db.PlanPrecomputed(CargoType{0}, station, StationID{uint16_t(destination)}, _state_ticks, previous, UINT32_MAX);
                if (!p.ride) { IConsolePrint(CC_DEFAULT, "Quality plan: origin={}; destination={}; station={}; leg={}; state={}; no_ride=true", origin, destination, station, leg, to_underlying(p.state)); break; }
                IConsolePrint(CC_DEFAULT, "Quality plan: origin={}; destination={}; station={}; leg={}; service={}; board_order={}; alight={}; alight_order={}; estimated={}; cost={}; riding={}; waiting={}; first_wait={}; transfers={}",
                    origin, destination, station, leg, p.ride->service, p.ride->boarding_order, p.ride->alighting, p.ride->alighting_order,
                    p.estimated_service, p.cost, p.riding, p.waiting, p.ride->estimated_wait, p.transfers);
                station = p.ride->alighting; previous = p.ride->service;
            }
        }
        return true;
    }
'''
    content = content.replace(anchor, probe + anchor)
    # Complete the product's initial capture barrier before warming cold rows.
    # This is paused setup; StateTicks and all packet metadata stay unchanged.
    anchor = '        bool ready = false;\n        const auto started = std::chrono::steady_clock::now();'
    assert content.count(anchor) == 1
    content = content.replace(anchor, anchor + '\n        while (!db.PreparePrecomputed()) {}')
    target.write_text(content, encoding="utf-8")
    # Explain why a ready row's binding is unusable without weakening any
    # production validation. At most 64 focused observations, copied TU only.
    original = source / "src/passenger_service_database.cpp"
    content = original.read_text(encoding="utf-8")
    anchor = 'if (result.ride && !this->IsCurrent(*result.ride)) { result.ride.reset(); result.state = PassengerPrecomputedPlan::State::Building; }'
    assert content.count(anchor) == 1
    content = content.replace(anchor, r'''if (result.ride && !this->IsCurrent(*result.ride)) {
        static std::set<std::tuple<StationID, StationID, OrderListID, int64_t>> reported;
        if (getenv("OPENTTD_PASSENGER_TRACE") != nullptr && reported.size() < 64 &&
                ((origin == StationID{387} && destination == StationID{916}) ||
                 (origin == StationID{3393} && destination == StationID{3394}) ||
                 (origin == StationID{3221} && destination == StationID{400})) &&
                reported.emplace(origin, destination, result.ride->service, _state_ticks.base() / 256).second) {
            const auto &r = *result.ride; const auto *s = this->Get(r.service); const auto *list = OrderList::GetIfValid(r.service);
            Debug(misc, 0, "Quality binding: station={}; destination={}; tick={}; service={}; intent_signature={}; intent_generation={}; database_signature={}; database_generation={}; live_signature={}; unsafe={}; certainty={}",
                origin, destination, _state_ticks.base(), r.service, r.signature, r.generation, s != nullptr ? s->incarnation : 0,
                s != nullptr ? s->generation : 0, list != nullptr ? list->GetCargoDistRouteGeneration() : 0,
                this->unsafe_lookup.test(r.service.base()), s != nullptr ? to_underlying(s->timetable.GetCertainty()) : 0);
        }
        result.ride.reset(); result.state = PassengerPrecomputedPlan::State::Building;
    }''')
    content = content.replace('#include "stdafx.h"', '#include "stdafx.h"\n#include "debug.h"')
    content = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (original.parent / m[1]).resolve().as_posix() + '"' if (original.parent / m[1]).is_file() else m[0], content)
    target = build / "save-integrity/passenger_service_database.cpp"
    target.write_text(content, encoding="utf-8")
    project = build / "openttd_lib.vcxproj"
    xml = project.read_text(encoding="utf-8-sig")
    assert xml.count(str(original)) == 1
    project.write_text(xml.replace(str(original), str(target)), encoding="utf-8-sig")
    original = source / "src/command.cpp"
    content = original.read_text(encoding="utf-8")
    for result in ("res", "res2"):
        anchor = f'if ({result}.Succeeded() && passenger_changed) InvalidatePassengerServiceDatabase();'
        assert content.count(anchor) == 1
        diagnostic = '''if (RESULT.Succeeded() && (passenger_changed || cmd == Commands::BuildRoad || cmd == Commands::TerraformLand)) {
        static size_t reports = 0;
        if (getenv("OPENTTD_PASSENGER_TRACE") != nullptr && reports++ < 32)
            Debug(misc, 0, "Quality construction: command={}; tile={}; global_invalidation={}", GetCommandName(cmd), tile, passenger_changed);
    }
    '''.replace("RESULT", result)
        content = content.replace(anchor, diagnostic + anchor)
    content = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (original.parent / m[1]).resolve().as_posix() + '"' if (original.parent / m[1]).is_file() else m[0], content)
    target = build / "save-integrity/command.cpp"
    target.write_text(content, encoding="utf-8")
    xml = project.read_text(encoding="utf-8-sig")
    assert xml.count(str(original)) == 1
    project.write_text(xml.replace(str(original), str(target)), encoding="utf-8-sig")


if __name__ == "__main__":
    main()
