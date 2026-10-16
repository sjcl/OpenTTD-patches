"""Prepare measurement-only copied TUs after passenger_precomputed_join_prepare.py.

The product sources and CargoPacket save format stay unchanged. Enable observations
with OPENTTD_PASSENGER_TRACE=1. Run quality separately from CPU measurements.
"""
import argparse
import pathlib
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("build", type=pathlib.Path)
    args = parser.parse_args()
    source, build = args.source.resolve(), args.build.resolve()
    target = build / "transport-trace"
    target.mkdir(exist_ok=True)
    header = target / "trace.h"
    includes = "".join(f'#include "{(source / "src" / name).as_posix()}"\n' for name in (
        "passenger_journey_active.h", "passenger_service_database.h", "settings_type.h", "station_base.h", "station_map.h", "vehicle_base.h", "date_func.h", "group.h", "cargotype.h"))
    header.write_text(includes + (source / "utils/passenger_transport_trace.h").read_text(encoding="utf-8"), encoding="utf-8")
    project = build / "openttd_lib.vcxproj"
    xml = project.read_text(encoding="utf-8-sig")

    def copy(name, edits):
        nonlocal xml
        original = source / "src" / name
        active = original if str(original) in xml else build / 'save-integrity' / pathlib.Path(name).name
        content = active.read_text(encoding="utf-8")
        content = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (original.parent / m[1]).resolve().as_posix() + '"'
                         if (original.parent / m[1]).is_file() else m[0], content)
        content = content.replace('#include "' + (source / 'src/safeguards.h').as_posix() + '"', f'#include "{header.as_posix()}"\n#include "{(source / "src/safeguards.h").as_posix()}"')
        for old, new in edits:
            assert content.count(old) == 1, (name, old, content.count(old))
            content = content.replace(old, new)
        output = target / pathlib.Path(name).name
        output.write_text(content, encoding="utf-8")
        assert xml.count(str(active)) == 1, (name, "project")
        xml = xml.replace(str(active), str(output))

    copy("cargopacket.cpp", [
        ('\t\tpacket->RecordPassengerCapacityFailure(vehicle->First()->orders->index, _state_ticks);', '\t\tpacket->RecordPassengerCapacityFailure(vehicle->First()->orders->index, _state_ticks);\n\t\tPassengerTransportTrace::CapacityMiss(*packet, *vehicle, this->passenger_station);'),
        ('void StationCargoList::IndexPacket(CargoPacket *packet, StationID key, StationCargoPacketList::iterator position, std::optional<uint64_t> sequence) const\n{', 'void StationCargoList::IndexPacket(CargoPacket *packet, StationID key, StationCargoPacketList::iterator position, std::optional<uint64_t> sequence) const\n{\n\tPassengerTransportTrace::Wait(*packet, this->passenger_station, this->passenger_index_cargo);'),
        ('\t\tfirst_station(first_station)\n{\n\tdbg_assert(count != 0);\n}', '\t\tfirst_station(first_station)\n{\n\tdbg_assert(count != 0);\n\tPassengerTransportTrace::Birth(*this);\n}'),
        ('\tif (CleaningPool()) return;\n\tUnregisterPassengerJourneyPacket(*this);', '\tPassengerTransportTrace::Erase(this->index);\n\tif (CleaningPool()) return;\n\tUnregisterPassengerJourneyPacket(*this);'),
        ('\tthis->count -= new_size;', '\tPassengerTransportTrace::Split(*this, *cp_new, new_size);\n\tthis->count -= new_size;'),
        ('\tif (this->passenger_destination != cp->passenger_destination || !this->HasSamePassengerRouting(*cp)) return false;\n\tthis->count += cp->count;', '\tif (this->passenger_destination != cp->passenger_destination || !this->HasSamePassengerRouting(*cp)) return false;\n\tPassengerTransportTrace::Merge(*this, *cp);\n\tthis->count += cp->count;'),
        ('\tthis->passenger_destination = destination;', '\tthis->passenger_destination = destination;\n\tPassengerTransportTrace::Start(*this);'),
        ('void CargoPacket::Reduce(uint count)\n{', 'void CargoPacket::Reduce(uint count)\n{\n\tPassengerTransportTrace::Reduce(*this, count);'),
        ('void VehicleCargoList::Append(CargoPacket *cp, MoveToAction action)\n{', 'void VehicleCargoList::Append(CargoPacket *cp, MoveToAction action)\n{\n\tPassengerTransportTrace::Attach(*cp, this);'),
        ('\t\t\tRecordPassengerJourneyMovement(PassengerJourneyMovement::Board, *packet, count);', '\t\t\tPassengerTransportTrace::Board(*packet, skip, count, this);\n\t\t\tRecordPassengerJourneyMovement(PassengerJourneyMovement::Board, *packet, count);'),
        ('uint StationCargoList::ShiftJourneyCargo(Taction action, std::span<const StationID> next, const Vehicle *vehicle, PassengerJourneyLoadingContext &loading)\n{', 'uint StationCargoList::ShiftJourneyCargo(Taction action, std::span<const StationID> next, const Vehicle *vehicle, PassengerJourneyLoadingContext &loading)\n{\n\tloading.EnableWaitingEvents();'),
    ])
    copy("cargoaction.cpp", [
        ('\tRecordPassengerJourneyMovement(PassengerJourneyMovement::Board, *cp_new, cp_new->Count());', '\tPassengerTransportTrace::Location(*cp_new, this->current_tile);\n\tPassengerTransportTrace::Board(*cp_new, 0, cp_new->Count(), this->destination);\n\tRecordPassengerJourneyMovement(PassengerJourneyMovement::Board, *cp_new, cp_new->Count());'),
        ('\tRecordPassengerJourneyMovement(PassengerJourneyMovement::Transfer, *cp_new, cp_new->Count());', '\tPassengerTransportTrace::Location(*cp_new, this->current_tile);\n\tPassengerTransportTrace::Arrive(*cp_new, cp_new->Count(), false, this->source);\n\tRecordPassengerJourneyMovement(PassengerJourneyMovement::Transfer, *cp_new, cp_new->Count());'),
        ('\tRecordPassengerJourneyMovement(PassengerJourneyMovement::Deliver, *cp, remove);', '\tPassengerTransportTrace::Location(*cp, this->current_tile);\n\tPassengerTransportTrace::Arrive(*cp, remove, true, this->source);\n\tRecordPassengerJourneyMovement(PassengerJourneyMovement::Deliver, *cp, remove);'),
    ])
    copy("vehicle.cpp", [
        ('void Vehicle::LeaveStation()\n{', 'void Vehicle::LeaveStation()\n{\n\tPassengerTransportTrace::DepartureEvent(*this);'),
        ('\tv = nullptr;\n\tif (!_tick_other_veh_cache.empty())', '\tPassengerTransportTrace::Tick();\n\tv = nullptr;\n\tif (!_tick_other_veh_cache.empty())'),
    ])
    copy("passenger_od.cpp", [
        ('void GenerateStationCargo(Station &st, CargoType cargo, uint amount, Source source)\n{', 'void GenerateStationCargo(Station &st, CargoType cargo, uint amount, Source source)\n{\n\tPassengerTransportTrace::Generation(cargo, amount);'),
    ])
    original = source / 'src/passenger_journey_active.cpp'
    text = original.read_text(encoding='utf-8')
    start = text.index('bool PassengerJourneyLoadingContext::CanBoard(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo)')
    end = text.index('\nbool PassengerJourneyLoadingContext::CanBoardUncached', start)
    function = text[start:end]
    body = function.index('\n{') + 2
    function = function[:body] + '\n\tauto check = [&]() -> bool {' + function[body:]
    closing = function.rfind('\n}')
    function = function[:closing] + '\n\t};\n\tconst bool result = check();\n\tif (this->waiting_events) PassengerTransportTrace::Opportunity(packet, vehicle, result);\n\treturn result;' + function[closing:]
    copy('passenger_journey_active.cpp', [(text[start:end], function)])
    console = build / "save-integrity/console_cmds.cpp"
    text = console.read_text(encoding="utf-8")
    anchor = '#include "' + (source / 'src/stdafx.h').as_posix() + '"'
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + f'\n#include "{header.as_posix()}"')
    anchor = '\tif (operation == "inventory") {\n\n        uint64_t company_deliveries'
    assert text.count(anchor) == 1
    controls = '''    if (operation == "trace_prepare") { PassengerTransportTrace::defer_next_dump = true; return true; }
    if (operation == "trace_state") { IConsolePrint(CC_DEFAULT, "Trace recording: final={}", PassengerTransportTrace::recording_finished); return true; }
'''
    text = text.replace(anchor, controls + '\tif (operation == "inventory") {\n\t\tPassengerTransportTrace::BeginObservation();\n\n        uint64_t company_deliveries')
    anchor = '\tconst auto events = GetPassengerWaitingEventDiagnostic();'
    assert text.count(anchor) == 1
    text = text.replace(anchor, '\tPassengerTransportTrace::Dump();\n' + anchor)
    console.write_text(text, encoding="utf-8")
    driver = build / 'real-map-driver/null_v.cpp'
    text = driver.read_text(encoding='utf-8')
    anchor = '#include "' + (source / 'src/stdafx.h').as_posix() + '"'
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + f'\n#include "{header.as_posix()}"')
    anchor = r'<< ",\"samples\":" << samples.size()'
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + r' << ",\"trace_started\":" << PassengerTransportTrace::selected << ",\"trace_fresh\":" << PassengerTransportTrace::complete_started << ",\"trace_initial\":" << PassengerTransportTrace::initial_started << ",\"trace_assigned\":" << PassengerTransportTrace::assigned_packets')
    anchor = '            if (loops == 1 || loops % 256 == 0) {'
    assert text.count(anchor) == 1
    text = text.replace(anchor, '''            if (started && _state_ticks.base() > before && _state_ticks.base() - start == 16384) {
                IConsoleCmdExec("passenger_journey_stats save_integrity");
                std::rename("save-integrity.json", "transport-checkpoint-16384.json");
            }
''' + anchor)
    driver.write_text(text, encoding='utf-8')
    project.write_text(xml, encoding="utf-8-sig")


if __name__ == "__main__":
    main()
