"""Inspect real v4 input and canonical Packet/payment state in isolated copied TUs.

--legacy-reader reads the retired byte into a temporary scalar solely to count
the input's true/false values. The normal variant retains the product's unknown
field skip. Neither variant adds a Packet member or writes that field in v3.
"""
import argparse
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[3]


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Integrity anchor changed: {old[:80]}')
    return text.replace(old, new)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('build', type=pathlib.Path)
    p.add_argument('--legacy-reader', action='store_true')
    a = p.parse_args()
    build = a.build.resolve()
    folder = build / 'save-integrity'
    folder.mkdir(exist_ok=True)
    header = folder / 'integrity.h'
    header.write_text(r'''#pragma once
#include <map>
#include <fstream>
namespace SaveIntegrity {
inline bool legacy_value = false;
inline std::map<uint32_t, bool> legacy;
inline uint64_t true_packets = 0, false_packets = 0, true_units = 0, false_units = 0;
inline void (*fields)(const CargoPacket &, uint64_t &, uint64_t &) = nullptr;
template<class T> uint64_t Number(T v) { if constexpr (requires { v.base(); }) return uint64_t(v.base()); else if constexpr (std::is_enum_v<T>) return uint64_t(to_underlying(v)); else return uint64_t(v); }
template<class... T> void Mix(uint64_t &h, T... values) {
    auto one = [&](auto value) { uint64_t n = Number(value); for (int i = 0; i < 8; ++i) { h ^= n & 255; h *= 1099511628211ULL; n >>= 8; } };
    (one(values), ...);
}
inline void Legacy(const CargoPacket &cp) {
    legacy[cp.index.base()] = legacy_value;
    (legacy_value ? true_packets : false_packets)++; (legacy_value ? true_units : false_units) += cp.Count();
}
uint64_t Payments();
inline void Dump(uint16_t feature, int64_t tick) {
    uint64_t digest = 14695981039346656037ULL, units = 0, known = 0, journeys = 0, pairs = 0;
    std::map<uint64_t, std::pair<const CargoPacket *, const CargoPacket *>> groups;
    for (const CargoPacket *cp : CargoPacket::Iterate()) {
        units += cp->Count(); known += cp->GetPassengerDestination() != StationID::Invalid() ? cp->Count() : 0;
        journeys += cp->GetPassengerRoutingMode() == PassengerRoutingMode::Journey;
        uint64_t key = 14695981039346656037ULL; fields(*cp, digest, key);
        if (auto history = legacy.find(cp->index.base()); history != legacy.end()) {
            auto &group = groups[key]; (history->second ? group.first : group.second) = cp;
        }
    }
    for (const auto &[key, pair] : groups) if (pair.first && pair.second && pair.first->Count() + pair.second->Count() <= CargoPacket::MAX_COUNT &&
        StationCargoList::AreMergable(pair.first, pair.second) && VehicleCargoList::AreMergable(pair.first, pair.second)) ++pairs;
    std::ofstream out("save-integrity.json");
    out << "{\"runtime_routing_feature\":" << feature << ",\"state_ticks\":" << tick << ",\"packets\":" << CargoPacket::GetNumItems() << ",\"cargo_units\":" << units << ",\"known_od_units\":" << known
        << ",\"journey_packets\":" << journeys << ",\"packet_state_digest\":" << digest << ",\"payment_digest\":" << Payments()
        << ",\"payments_valid\":" << (CargoPacket::ValidateDeferredCargoPayments() ? "true" : "false")
        << ",\"legacy_true_packets\":" << true_packets << ",\"legacy_false_packets\":" << false_packets
        << ",\"legacy_true_units\":" << true_units << ",\"legacy_false_units\":" << false_units << ",\"opposite_history_mergeable_pairs\":" << pairs << "}\n";
}
}
''', encoding='utf-8')
    includes = '#include "' + header.as_posix() + '"\n'
    names = ['console_cmds.cpp', 'cargopacket.cpp', 'sl/cargopacket_sl.cpp']
    project = build / 'openttd_lib.vcxproj'
    xml = project.read_text(encoding='utf-8-sig')
    for name in names:
        target = str(folder / pathlib.Path(name).name)
        if target in xml:
            xml = once(xml, target, str(ROOT / 'src' / name))
    for name in names:
        original = ROOT / 'src' / name
        text = original.read_text(encoding='utf-8')
        text = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (original.parent / m[1]).resolve().as_posix() + '"'
                      if (original.parent / m[1]).is_file() else m[0], text)
        text = once(text, '#include "' + (ROOT / 'src/safeguards.h').as_posix() + '"', includes + '#include "' + (ROOT / 'src/safeguards.h').as_posix() + '"')
        if name == 'console_cmds.cpp':
            text = text.replace(includes, '#include "' + (ROOT / 'src/sl/saveload.h').as_posix() + '"\n' + includes)
            anchor = '\tif (operation == "inventory") {\n\t\t/* Explicit read-only census.'
            text = once(text, anchor, '\tif (operation == "save_integrity") { SaveIntegrity::Dump(_sl_xv_feature_versions[XSLFI_PASSENGER_JOURNEY_ROUTING], _state_ticks.base()); return true; }\n' + anchor)
        elif name == 'cargopacket.cpp':
            anchor = 'btree::btree_map<uint64_t, Money> _cargo_packet_deferred_payments;'
            text = once(text, anchor, anchor + '\nuint64_t SaveIntegrity::Payments() { uint64_t digest = 14695981039346656037ULL; for (const auto &[key, money] : _cargo_packet_deferred_payments) SaveIntegrity::Mix(digest, key, money); return digest; }')
        else:
            anchor = 'NamedSaveLoadTable GetCargoPacketDesc()\n{'
            body = r'''
    SaveIntegrity::fields = [](const CargoPacket &cp, uint64_t &state, uint64_t &merge) {
        using SaveIntegrity::Mix;
        Mix(merge, cp.source_xy, cp.periods_in_transit, cp.first_station, cp.source.type, cp.source.id, cp.passenger_via, cp.passenger_destination,
            cp.passenger_routing_mode, cp.passenger_legacy_hop, cp.passenger_retry_station, cp.passenger_retry_after, cp.passenger_interchange_ready,
            cp.passenger_arriving_service, cp.passenger_arriving_incarnation, cp.passenger_transport, cp.passenger_interchange_transport, cp.passenger_schedule_used);
        const auto &r = cp.passenger_journey;
        Mix(merge, r.vehicle, r.service, r.boarding, r.alighting, r.boarding_order, r.alighting_order, r.departure, r.arrival, r.signature,
            r.kind, r.generation, r.not_before, r.planned_at, r.estimated_wait, r.waiting_limit, r.timed_deferred);
        Mix(state, cp.index, merge, cp.count, cp.feeder_share, cp.next_hop, cp.travelled.x, cp.travelled.y);
    };'''
            text = once(text, anchor, anchor + body)
            if a.legacy_reader:
                text = once(text, '\t\tNSL("passenger_legacy_hop",', '\t\tNSL("passenger_work_exhausted", SLEG_CONDVAR_X(SaveIntegrity::legacy_value, SLE_BOOL, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING, 4, 4))),\n\t\tNSL("passenger_legacy_hop",')
                text = once(text, '\t\tSlObjectLoadFiltered(cp, slt);', '\t\tSlObjectLoadFiltered(cp, slt);\n\t\tif (SlXvIsFeaturePresent(XSLFI_PASSENGER_JOURNEY_ROUTING, 4, 4)) SaveIntegrity::Legacy(*cp);')
        target = folder / pathlib.Path(name).name
        target.write_text(text, encoding='utf-8')
        xml = once(xml, str(original), str(target))
    project.write_text(xml, encoding='utf-8-sig')


if __name__ == '__main__':
    main()
