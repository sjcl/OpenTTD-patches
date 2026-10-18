#!/usr/bin/env python3
"""Prepare copied TUs for asymmetric async-worker loopback tests (never product code)."""
import argparse
import pathlib
import re
import sys
import shutil

sys.dont_write_bytecode = True
import passenger_precomputed_join_prepare as join


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("build", type=pathlib.Path)
    parser.add_argument("revision")
    args = parser.parse_args()
    source, build = args.source.resolve(), args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    cache = source / "build-parallel/CMakeCache.txt"
    if cache.exists() and not (build / "CMakeCache.txt").exists():
        shutil.copyfile(cache, build / "CMakeCache.txt")
    originals = ("console_cmds.cpp", "cargopacket.cpp", "sl/cargopacket_sl.cpp", "video/null_v.cpp", "os/windows/win32.cpp")
    project = build / "openttd_lib.vcxproj"
    if not project.exists():
        project.write_text("\n".join(f'<ClCompile Include="{source / "src" / name}" />' for name in originals), encoding="utf-8")
    saved = sys.argv
    try:
        sys.argv = [str(join.__file__), str(source), str(build), args.revision]
        join.main()
    finally:
        sys.argv = saved

    header = build / "save-integrity/integrity.h"
    text = header.read_text(encoding="utf-8")
    includes = "".join(f'#include "{(source / "src" / name).as_posix()}"\n' for name in
                       ("tests/passenger_async_access.h", "passenger_journey_active.h"))
    text = text.replace("namespace SaveIntegrity {", includes + "namespace SaveIntegrity {")
    anchor = '    std::ofstream out("save-integrity.json");'
    assert text.count(anchor) == 1
    text = text.replace(anchor, r'''
    uint64_t table_digest = 14695981039346656037ULL, retained_table_digest = table_digest;
    uint64_t cache_hits = 0, serial_rows = 0, worker_failures = 0, finance_digest = table_digest;
    for (const Company *company : Company::Iterate()) Mix(finance_digest, company->index, company->money, company->money_fraction, company->current_loan,
        company->cur_economy.income, company->cur_economy.expenses);
    for (const Vehicle *vehicle : Vehicle::Iterate()) Mix(finance_digest, vehicle->index, vehicle->profit_this_year);
    for (CargoType cargo : EnumRange(NUM_CARGO)) {
        auto &table = PassengerServiceDatabaseTestAccess::Table(db, cargo);
        Mix(table_digest, PassengerPrecomputedTestAccess::Digest(table));
        Mix(retained_table_digest, PassengerPrecomputedTestAccess::Digest(PassengerServiceDatabaseTestAccess::Retained(db, cargo)));
        auto parallel = table.GetParallelDiagnostic();
        cache_hits += parallel.cache_hits; serial_rows += parallel.serial_rows; worker_failures += parallel.fallbacks;
    }
    const auto movement = GetPassengerJourneyTotalDiagnostic();
''' + anchor)
    anchor = r'        << ",\"database_digest\":"'
    assert text.count(anchor) == 1
    text = text.replace(anchor, r'''
        << ",\"finance_digest\":" << finance_digest << ",\"table_digest\":" << table_digest << ",\"retained_table_digest\":" << retained_table_digest
        << ",\"worker_cache_hits\":" << cache_hits << ",\"worker_serial_rows\":" << serial_rows << ",\"worker_failures\":" << worker_failures
        << ",\"journey_boarded\":" << movement.boarded_passengers + movement.untimed_boarded_passengers + movement.schedule_boarded_passengers
        << ",\"journey_transferred\":" << movement.transferred_passengers + movement.untimed_transferred_passengers + movement.schedule_transferred_passengers
        << ",\"journey_delivered\":" << movement.delivered_passengers + movement.untimed_delivered_passengers + movement.schedule_delivered_passengers
''' + anchor)
    header.write_text(text, encoding="utf-8")

    # Seed real known-OD packets on the host before its map is downloaded.
    # Saved packets, native loading, transfer, delivery and payment then run
    # through the ordinary game paths on both peers.
    console = build / "save-integrity/console_cmds.cpp"
    text = console.read_text(encoding="utf-8")
    anchor = "        return db.PrecomputedFinished();\n    }"
    assert text.count(anchor) == 1
    text = text.replace(anchor, r'''
        uint seeded = 0;
        for (Station *from : Station::Iterate()) for (CargoType cargo : EnumRange(NUM_CARGO)) {
            if (!IsCargoInClass(cargo, CargoClass::Passengers)) continue;
            for (const Station *to : Station::Iterate()) {
                if (from == to) continue;
                bool paired = false;
                for (const Vehicle *vehicle : Vehicle::Iterate()) {
                    if (!vehicle->IsPrimaryVehicle() || vehicle->orders == nullptr) continue;
                    bool carries = false;
                    for (const Vehicle *part = vehicle; part != nullptr; part = part->Next()) carries |= part->cargo_type == cargo && part->cargo_cap != 0;
                    if (!carries) continue;
                    bool board = false, alight = false;
                    for (const Order *order : vehicle->Orders()) if (order->IsType(OT_GOTO_STATION)) {
                        board |= order->GetDestination().ToStationID() == from->index && order->GetCargoLoadType(cargo) != OrderLoadType::NoLoad;
                        alight |= order->GetDestination().ToStationID() == to->index && order->GetCargoUnloadType(cargo) != OrderUnloadType::NoUnload;
                    }
                    paired |= board && alight;
                }
                if (!paired) continue;
                IConsolePrint(CC_DEFAULT, "Async fixture OD {} -> {}, acceptance={}", from->index, to->index, to->goods[cargo].status.Test(GoodsEntry::State::Acceptance));
                auto *packet = CargoPacket::Create(16, 0, from->index, from->xy, Money{0});
                if (!packet->SetPassengerDestination(cargo, to->index)) { delete packet; continue; }
                from->goods[cargo].CreateData().cargo.Append(packet, to->index); ++seeded;
            }
        }
        IConsolePrint(CC_DEFAULT, "Async fixture seeded {} known-OD packets", seeded);
        return db.PrecomputedFinished();
    }''')
    console.write_text(text, encoding="utf-8")

    original = source / "src/passenger_precomputed.cpp"
    text = original.read_text(encoding="utf-8")
    text = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (original.parent / m[1]).resolve().as_posix() + '"', text)
    anchor = "\tconst auto started = Clock::now();\n\ttry {\n\t\tif (!this->parallel) {"
    assert text.count(anchor) == 1
    text = text.replace(anchor, r'''
    if (!this->parallel) this->parallel_quantum_hook = +[](size_t destination, uint64_t work) {
        static const uint delay = []() { uint value = 0; if (const char *text = getenv("OPENTTD_PASSENGER_WORKER_DELAY_US")) std::from_chars(text, text + strlen(text), value); return value; }();
        static const uint64_t fail = []() { uint64_t value = 0; if (const char *text = getenv("OPENTTD_PASSENGER_WORKER_FAIL_WORK")) std::from_chars(text, text + strlen(text), value); return value; }();
        if (delay != 0) std::this_thread::sleep_for(std::chrono::microseconds(delay));
        if (fail != 0 && destination == 1 && work >= fail) throw std::bad_alloc{};
    };
''' + anchor)
    anchor = "size_t PassengerPrecomputedTable::AdvanceSearch(size_t allowance, uint workers)\n{"
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + r'''
    if (workers != 0 && this->serial_destination != this->destination) {
        static const uint headstart = []() { uint value = 0; if (const char *text = getenv("OPENTTD_PASSENGER_MAIN_HEADSTART_US")) std::from_chars(text, text + strlen(text), value); return value; }();
        if (headstart != 0) std::this_thread::sleep_for(std::chrono::microseconds(headstart));
    }
''')
    text = text.replace("#include <chrono>", "#include <chrono>\n#include <charconv>")
    target = build / "save-integrity/passenger_precomputed.cpp"
    target.write_text(text, encoding="utf-8")
    replacements = ("save-integrity/console_cmds.cpp", "save-integrity/cargopacket.cpp", "save-integrity/cargopacket_sl.cpp",
                    "real-map-driver/null_v.cpp", "real-map-driver/win32.cpp", "save-integrity/passenger_precomputed.cpp")
    cmake = [f'include("{(source / "build-parallel/parallel-reference/reference.cmake").as_posix()}")',
             "function(use_async_probe)", "  get_target_property(probe_sources openttd_lib SOURCES)"]
    for original, target in zip((*originals, "passenger_precomputed.cpp"), replacements):
        cmake += [f'  list(REMOVE_ITEM probe_sources "{(source / "src" / original).as_posix()}")',
                  f'  list(APPEND probe_sources "{(build / target).as_posix()}")']
    cmake += ['  set_property(TARGET openttd_lib PROPERTY SOURCES "${probe_sources}")', "endfunction()",
              "cmake_language(DEFER CALL use_async_probe)"]
    (build / "probe.cmake").write_text("\n".join(cmake) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
