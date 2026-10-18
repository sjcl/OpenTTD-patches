#pragma once
#include <map>
#include <fstream>
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/passenger_service_database.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/station_base.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/vehicle_base.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/company_base.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/settings_type.h"
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
            Mix(ownership, list.passenger_routing_dirty, list.passenger_routing_revision, list.passenger_routing_rows,
                list.passenger_routing_due, list.passenger_routing_sweeping, list.passenger_routing_cursor, list.passenger_routing_end,
                list.passenger_routing_tick, list.passenger_routing_work, list.passenger_boarding_last);
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
    std::ofstream out("save-integrity.json");
    out << "{\"runtime_routing_feature\":" << feature << ",\"state_ticks\":" << tick << ",\"packets\":" << CargoPacket::GetNumItems() << ",\"cargo_units\":" << units << ",\"known_od_units\":" << known
        << ",\"journey_packets\":" << journeys << ",\"packet_state_digest\":" << digest << ",\"payment_digest\":" << Payments()
        << ",\"payments_valid\":" << (CargoPacket::ValidateDeferredCargoPayments() ? "true" : "false")

        << ",\"database_digest\":" << database << ",\"ownership_digest\":" << ownership << ",\"loading_digest\":" << loading
        << ",\"completed_rows\":" << rows << ",\"retained_rows\":" << retained << ",\"table_operations\":" << operations
        << ",\"published_revision\":" << db.GetPublishedRevision() << ",\"company_deliveries\":" << deliveries
        << ",\"journey_routing\":" << _settings_game.linkgraph.passenger_journey_routing
        << ",\"precomputed_routing\":" << _settings_game.linkgraph.passenger_journey_precomputed
        << ",\"legacy_true_packets\":" << true_packets << ",\"legacy_false_packets\":" << false_packets
        << ",\"legacy_true_units\":" << true_units << ",\"legacy_false_units\":" << false_units << ",\"opposite_history_mergeable_pairs\":" << pairs << "}\n";
}
}
