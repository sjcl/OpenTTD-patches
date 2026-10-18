/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cargopacket_sl.cpp Code handling saving and loading of cargo packets */

#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/stdafx.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/debug.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/vehicle_base.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/station_base.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/passenger_service_database.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/scope_info.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/3rdparty/cpp-btree/btree_map.h"

#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/sl/saveload.h"

#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-merge-jgrpp-ya-20261018/save-integrity/integrity.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/safeguards.h"

extern btree::btree_map<uint64_t, Money> _cargo_packet_deferred_payments;

/**
 * Savegame conversion for cargopackets.
 */
/* static */ void CargoPacket::AfterLoad()
{
	/* Stations are now available. Old saves have no OD; malformed or stale IDs
	 * must not survive loading or become references to subsequently reused IDs. */
	for (CargoPacket *cp : CargoPacket::Iterate()) {
		if (cp->passenger_boarding_failure.count > PassengerBoardingFailure::THRESHOLD ||
				(cp->passenger_boarding_failure.Present() && (cp->passenger_boarding_failure.service == OrderListID::Invalid() || cp->passenger_boarding_failure.incarnation == 0)) ||
				(!cp->passenger_boarding_failure.Present() && cp->passenger_boarding_failure != PassengerBoardingFailure{})) SlErrorCorrupt("Invalid passenger boarding failure");
		if (cp->passenger_journey.kind == PassengerRideKind::HyperpathIntent &&
				SlXvIsFeatureMissing(XSLFI_PASSENGER_PERIODIC)) SlErrorCorrupt("Hyperpath intent without strategy save format");
		if (to_underlying(cp->passenger_journey.kind) > to_underlying(PassengerRideKind::HyperpathIntent) ||
				to_underlying(cp->passenger_transport) > to_underlying(PassengerTransportKind::Untimed) ||
				to_underlying(cp->passenger_interchange_transport) > to_underlying(PassengerTransportKind::Untimed)) SlErrorCorrupt("Invalid passenger ride kind");
		if (cp->passenger_journey.kind != PassengerRideKind::VehicleSnapshot &&
				(cp->passenger_routing_mode != PassengerRoutingMode::Journey || cp->passenger_journey.signature == 0 || cp->passenger_journey.generation == 0 ||
					(!IsEstimatedPassengerRide(cp->passenger_journey.kind) && (cp->passenger_journey.arrival < cp->passenger_journey.departure || cp->passenger_journey.not_before > cp->passenger_journey.departure)))) SlErrorCorrupt("Invalid passenger service intent");
		if (IsEstimatedPassengerRide(cp->passenger_journey.kind) && (cp->passenger_journey.departure != StateTicks{} || cp->passenger_journey.arrival != StateTicks{} ||
				cp->passenger_journey.estimated_wait < 0 || cp->passenger_journey.waiting_limit <= 0 || cp->passenger_journey.waiting_limit > (int64_t{1} << 41))) SlErrorCorrupt("Invalid estimated passenger intent");
		if (SlXvIsFeatureMissing(XSLFI_PASSENGER_JOURNEY_ROUTING)) {
			cp->passenger_routing_mode = PassengerRoutingMode::Legacy;
			cp->passenger_journey = {};
		} else if (to_underlying(cp->passenger_routing_mode) > to_underlying(PassengerRoutingMode::Fallback)) {
			SlErrorCorrupt("Invalid passenger routing mode");
		}
		if (SlXvIsFeatureMissing(XSLFI_PASSENGER_DESTINATION) || !Station::IsValidID(cp->passenger_destination)) {
			cp->passenger_destination = StationID::Invalid();
		}
	}

	if (IsSavegameVersionBefore(SLV_44)) {
		/* If we remove a station while cargo from it is still en route, payment calculation will assume
		 * 0, 0 to be the first_station of the cargo, resulting in very high payments usually. v->source_xy
		 * stores the coordinates, preserving them even if the station is removed. However, if a game is loaded
		 * where this situation exists, the cargo-first_station information is lost. in this case, we set the first_station
		 * to the current tile of the vehicle to prevent excessive profits
		 */
		for (const Vehicle *v : Vehicle::Iterate()) {
			const CargoPacketList *packets = v->cargo.Packets();
			for (VehicleCargoList::ConstIterator it(packets->begin()); it != packets->end(); it++) {
				CargoPacket *cp = *it;
				cp->source_xy = Station::IsValidID(cp->first_station) ? Station::Get(cp->first_station)->xy : v->tile;
			}
		}

		/* Store position of the station where the goods come from, so there
		 * are no very high payments when stations get removed. However, if the
		 * station where the goods came from is already removed, the first_station
		 * information is lost. In that case we set it to the position of this
		 * station */
		for (Station *st : Station::Iterate()) {
			for (CargoType c{}; c < NUM_CARGO; c++) {
				GoodsEntry *ge = &st->goods[c];

				if (ge->data == nullptr) continue;
				const StationCargoPacketMap *packets = ge->data->cargo.Packets();
				for (StationCargoList::ConstIterator it(packets->begin()); it != packets->end(); it++) {
					CargoPacket *cp = *it;
					cp->source_xy = Station::IsValidID(cp->first_station) ? Station::Get(cp->first_station)->xy : st->xy;
				}
			}
		}
	}

	if (IsSavegameVersionBefore(SLV_120)) {
		/* CargoPacket's first_station should be either StationID::Invalid() or a valid station */
		for (CargoPacket *cp : CargoPacket::Iterate()) {
			if (!Station::IsValidID(cp->first_station)) cp->first_station = StationID::Invalid();
		}
	}

	if (!IsSavegameVersionBefore(SLV_68)) {
		/* Only since version 68 we have cargo packets. Savegames from before used
		 * 'new CargoPacket' + cargolist.Append so their caches are already
		 * correct and do not need rebuilding. */
		for (Vehicle *v : Vehicle::Iterate()) v->cargo.InvalidateCache();

		for (Station *st : Station::Iterate()) {
			for (CargoType c{}; c < NUM_CARGO; c++) {
				if (st->goods[c].data != nullptr) st->goods[c].data->cargo.InvalidateCache();
			}
		}
	}

	if (IsSavegameVersionBefore(SLV_181)) {
		for (Vehicle *v : Vehicle::Iterate()) v->cargo.KeepAll();
	}

	/* Before this version, we didn't track how far cargo actually traveled in vehicles. Make best-effort estimates of this. */
	if (IsSavegameVersionBefore(SLV_CARGO_TRAVELLED) && SlXvIsFeatureMissing(XSLFI_CARGO_TRAVELLED)) {
		/* Update the cargo-traveled in stations as if they arrived from the source tile. */
		for (Station *st : Station::Iterate()) {
			for (size_t i = 0; i < NUM_CARGO; i++) {
				GoodsEntry &ge = st->goods[i];
				if (ge.data == nullptr) continue;
				auto *packets = ge.data->cargo.Packets();
				for (auto it = packets->begin(); it != packets->end(); it++) {
					for (CargoPacket *cp : it->second) {
						if (cp->source_xy != INVALID_TILE && cp->source_xy != st->xy) {
							cp->travelled.x = TileX(cp->source_xy) - TileX(st->xy);
							cp->travelled.y = TileY(cp->source_xy) - TileY(st->xy);
						}
					}
				}
			}
		}

		/* Update the cargo-traveled in vehicles as if they were loaded at the source tile. */
		for (Vehicle *v : Vehicle::Iterate()) {
			auto *packets = v->cargo.Packets();
			for (auto it = packets->begin(); it != packets->end(); it++) {
				if ((*it)->source_xy != INVALID_TILE) {
					(*it)->UpdateLoadingTile((*it)->source_xy);
				}
			}
		}
	}

#ifdef WITH_FULL_ASSERTS
	/* CPF_IN_VEHICLE in flags is a NOSAVE; it tells if cargo is in a vehicle or not. Restore the value in here. */
	for (Vehicle *v : Vehicle::Iterate()) {
		for (auto it = v->cargo.Packets()->begin(); it != v->cargo.Packets()->end(); it++) {
			(*it)->flags |= CPF_IN_VEHICLE;
		}
	}
#endif /* WITH_FULL_ASSERTS */
}

/**
 * Savegame conversion for cargopackets.
 */
/* static */ void CargoPacket::PostVehiclesAfterLoad()
{
	if (SlXvIsFeaturePresent(XSLFI_CHILLPP)) {
		extern std::map<VehicleID, CargoPacketList> _veh_cpp_packets;
		for (auto &iter : _veh_cpp_packets) {
			if (iter.second.empty()) continue;
			Vehicle *v = Vehicle::Get(iter.first);
			Station *st = Station::Get(v->First()->last_station_visited);
			assert_msg(st != nullptr, "{}", VehicleInfoDumper(v));
			for (CargoPacket *cp : iter.second) {
				st->goods[v->cargo_type].CreateData().cargo.AfterLoadIncreaseReservationCount(cp->count);
				v->cargo.Append(cp, VehicleCargoList::MoveToAction::Load);
				if (cp->source_xy != INVALID_TILE) {
					cp->UpdateLoadingTile(cp->source_xy);
				}
#ifdef WITH_FULL_ASSERTS
				/* CPF_IN_VEHICLE in flags is a NOSAVE; it tells if cargo is in a vehicle or not. Restore the value in here. */
				cp->flags |= CPF_IN_VEHICLE;
#endif /* WITH_FULL_ASSERTS */
			}
		}
		_veh_cpp_packets.clear();
	}
}

/**
 * Wrapper function to get the CargoPacket's internal structure while
 * some of the variables itself are private.
 * @return the saveload description for CargoPackets.
 */
NamedSaveLoadTable GetCargoPacketDesc()
{
    SaveIntegrity::fields = [](const CargoPacket &cp, uint64_t &state, uint64_t &merge) {
        using SaveIntegrity::Mix;
        Mix(merge, cp.source_xy, cp.periods_in_transit, cp.first_station, cp.source.type, cp.source.id, cp.passenger_via, cp.passenger_destination,
            cp.passenger_routing_mode, cp.passenger_legacy_hop, cp.passenger_retry_station, cp.passenger_retry_after, cp.passenger_interchange_ready,
            cp.passenger_arriving_service, cp.passenger_arriving_incarnation, cp.passenger_transport, cp.passenger_interchange_transport, cp.passenger_schedule_used);
        const auto &failure = cp.passenger_boarding_failure;
        Mix(merge, failure.service, failure.count, failure.since, failure.incarnation);
        const auto &r = cp.passenger_journey;
        Mix(merge, r.vehicle, r.service, r.boarding, r.alighting, r.boarding_order, r.alighting_order, r.departure, r.arrival, r.signature,
            r.kind, r.generation, r.not_before, r.planned_at, r.estimated_wait, r.waiting_limit, r.timed_deferred);
        Mix(state, cp.passenger_routing_event);
        Mix(state, cp.index, merge, cp.count, cp.feeder_share, cp.next_hop, cp.travelled.x, cp.travelled.y);
    };
	static const NamedSaveLoad _cargopacket_desc[] = {
		NSL("source",             SLE_VAR(CargoPacket, first_station,            SLE_UINT16)),
		NSL("source_xy",          SLE_VAR(CargoPacket, source_xy,                SLE_UINT32)),
		NSL("loaded_at_xy",       SLE_VAR(CargoPacket, next_hop,                 SLE_FILE_U32 | SLE_VAR_U16)),
		NSL("passenger_via",      SLE_CONDVAR_X(CargoPacket, passenger_via,      SLE_UINT16, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_VIA))),
		NSL("passenger_destination", SLE_CONDVAR_X(CargoPacket, passenger_destination, SLE_UINT16, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_DESTINATION))),
		NSL("passenger_routing_mode", SLE_CONDVAR_X(CargoPacket, passenger_routing_mode, SLE_UINT8, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.vehicle", SLE_CONDVAR_X(CargoPacket, passenger_journey.vehicle, SLE_UINT32, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.service", SLE_CONDVAR_X(CargoPacket, passenger_journey.service, SLE_UINT16, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.boarding", SLE_CONDVAR_X(CargoPacket, passenger_journey.boarding, SLE_UINT16, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.alighting", SLE_CONDVAR_X(CargoPacket, passenger_journey.alighting, SLE_UINT16, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.boarding_order", SLE_CONDVAR_X(CargoPacket, passenger_journey.boarding_order, SLE_UINT16, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.alighting_order", SLE_CONDVAR_X(CargoPacket, passenger_journey.alighting_order, SLE_UINT16, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.departure", SLE_CONDVAR_X(CargoPacket, passenger_journey.departure, SLE_INT64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.arrival", SLE_CONDVAR_X(CargoPacket, passenger_journey.arrival, SLE_INT64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.signature", SLE_CONDVAR_X(CargoPacket, passenger_journey.signature, SLE_UINT64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.kind", SLE_CONDVAR_X(CargoPacket, passenger_journey.kind, SLE_UINT8, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.generation", SLE_CONDVAR_X(CargoPacket, passenger_journey.generation, SLE_UINT64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.not_before", SLE_CONDVAR_X(CargoPacket, passenger_journey.not_before, SLE_INT64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.planned_at", SLE_CONDVAR_X(CargoPacket, passenger_journey.planned_at, SLE_INT64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.estimated_wait", SLE_CONDVAR_X(CargoPacket, passenger_journey.estimated_wait, SLE_INT64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.waiting_limit", SLE_CONDVAR_X(CargoPacket, passenger_journey.waiting_limit, SLE_INT64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_journey.timed_deferred", SLE_CONDVAR_X(CargoPacket, passenger_journey.timed_deferred, SLE_BOOL, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_interchange_ready", SLE_CONDVAR_X(CargoPacket, passenger_interchange_ready, SLE_INT64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_arriving_service", SLE_CONDVAR_X(CargoPacket, passenger_arriving_service, SLE_UINT16, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_arriving_incarnation", SLE_CONDVAR_X(CargoPacket, passenger_arriving_incarnation, SLE_UINT64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_transport", SLE_CONDVAR_X(CargoPacket, passenger_transport, SLE_UINT8, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_interchange_transport", SLE_CONDVAR_X(CargoPacket, passenger_interchange_transport, SLE_UINT8, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_schedule_used", SLE_CONDVAR_X(CargoPacket, passenger_schedule_used, SLE_BOOL, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_legacy_hop", SLE_CONDVAR_X(CargoPacket, passenger_legacy_hop, SLE_UINT16, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_retry_station", SLE_CONDVAR_X(CargoPacket, passenger_retry_station, SLE_UINT16, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_retry_after", SLE_CONDVAR_X(CargoPacket, passenger_retry_after, SLE_INT64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING))),
		NSL("passenger_routing_event", SLE_CONDVAR_X(CargoPacket, passenger_routing_event, SLE_INT64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_PERIODIC))),
		NSL("passenger_boarding_failure.service", SLE_CONDVAR_X(CargoPacket, passenger_boarding_failure.service, SLE_UINT16, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_PERIODIC))),
		NSL("passenger_boarding_failure.count", SLE_CONDVAR_X(CargoPacket, passenger_boarding_failure.count, SLE_UINT8, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_PERIODIC))),
		NSL("passenger_boarding_failure.since", SLE_CONDVAR_X(CargoPacket, passenger_boarding_failure.since, SLE_INT64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_PERIODIC))),
		NSL("passenger_boarding_failure.incarnation", SLE_CONDVAR_X(CargoPacket, passenger_boarding_failure.incarnation, SLE_UINT64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_PERIODIC))),
		NSL("count",              SLE_VAR(CargoPacket, count,                    SLE_UINT16)),
		NSL("periods_in_transit", SLE_CONDVAR_X(CargoPacket, periods_in_transit, SLE_FILE_U8 | SLE_VAR_U16, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_MORE_CARGO_AGE, 0, 0))),
		NSL("periods_in_transit", SLE_CONDVAR_X(CargoPacket, periods_in_transit, SLE_UINT16, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_MORE_CARGO_AGE))),
		NSL("feeder_share",       SLE_VAR(CargoPacket, feeder_share,             SLE_INT64)),
		NSL("source_type",        SLE_CONDVAR(CargoPacket, source.type,          SLE_UINT8,  SLV_125, SL_MAX_VERSION)),
		NSL("source_id",          SLE_CONDVAR(CargoPacket, source.id,            SLE_UINT16, SLV_125, SL_MAX_VERSION)),
		NSL("travelled.x",        SLE_CONDVAR_X(CargoPacket, travelled.x,        SLE_INT32, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_CARGO_TRAVELLED))),
		NSL("travelled.y",        SLE_CONDVAR_X(CargoPacket, travelled.y,        SLE_INT32, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_CARGO_TRAVELLED))),

		/* Used to be paid_for, but that got changed. */
		NSL("", SLE_CONDNULL(1, SL_MIN_VERSION, SLV_121)),
	};
	return _cargopacket_desc;
}

/**
 * Save the cargo packets.
 */
static void Save_CAPA()
{
	SaveLoadTableData slt = SlTableHeader(GetCargoPacketDesc());

	for (CargoPacket *cp : CargoPacket::Iterate()) {
		SlSetArrayIndex(cp->index);
		SlObjectSaveFiltered(cp, slt);
	}
}

/**
 * Load the cargo packets.
 */
static void Load_CAPA()
{
	SaveLoadTableData slt = SlTableHeaderOrRiff(GetCargoPacketDesc());

	int index;
	while ((index = SlIterateArray()) != -1) {
		CargoPacket *cp = CargoPacket::CreateAtIndex(CargoPacketID(index));
		SlObjectLoadFiltered(cp, slt);
	}
}

/**
 * Save cargo packet deferred payments.
 */
void Save_CPDP()
{
	SlSetLength(16 * _cargo_packet_deferred_payments.size());

	for (auto &it : _cargo_packet_deferred_payments) {
		SlWriteUint64(it.first);
		SlWriteUint64(it.second);
	}
}

/**
 * Load cargo packet deferred payments.
 */
void Load_CPDP()
{
	uint count = static_cast<uint>(SlGetFieldLength() / 16);
	uint last_cargo_packet_id = std::numeric_limits<uint32_t>::max();

	for (uint i = 0; i < count; i++) {
		uint64_t k = SlReadUint64();
		uint64_t v = SlReadUint64();
		_cargo_packet_deferred_payments[k] = v;
		if (k >> 32 != last_cargo_packet_id) {
			last_cargo_packet_id = k >> 32;
			CargoPacket::Get(last_cargo_packet_id)->flags |= CargoPacket::CPF_HAS_DEFERRED_PAYMENT;
		}
	}
}



/** Chunk handlers related to cargo packets. */
static void Save_JPSD()
{
	auto bytes = GetPassengerServiceDatabase().Save();
	SlSetLength(bytes.size());
	for (uint8_t value : bytes) SlWriteByte(value);
}

static void Load_JPSD()
{
	size_t length = SlGetFieldLength();
	if (length > 2 * PassengerServiceDatabase::MAX_BYTES) SlErrorCorrupt("Passenger database exceeds memory limit");
	std::vector<uint8_t> bytes(length);
	for (uint8_t &value : bytes) value = SlReadByte();
	if (!GetPassengerServiceDatabase().Load(bytes)) SlErrorCorrupt("Invalid passenger periodic database");
}

static const ChunkHandler cargopacket_chunk_handlers[] = {
	{ 'CAPA', Save_CAPA, Load_CAPA, nullptr, nullptr, ChunkType::Table },
	{ 'CPDP', Save_CPDP, Load_CPDP, nullptr, nullptr, ChunkType::Riff  },
	{ 'JPSD', Save_JPSD, Load_JPSD, nullptr, nullptr, ChunkType::Riff  },
};

extern const ChunkHandlerTable _cargopacket_chunk_handlers(cargopacket_chunk_handlers);
