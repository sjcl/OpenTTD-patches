/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cargopacket.cpp Implementation of the cargo packets. */

#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/stdafx.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/passenger_journey_active.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/passenger_service_database.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/passenger_journey_shadow.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/command_type.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/debug.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/station_base.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/vehicle_base.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/core/pool_func.hpp"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/core/random_func.hpp"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/economy_base.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/cargoaction.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/order_type.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/company_func.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/core/backup_type.hpp"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/string_func.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/strings_func.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/settings_type.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/date_func.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/passenger_routing.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/3rdparty/cpp-btree/btree_map.h"

#include "table/strings.h"

#include <vector>
#include <set>
#include <chrono>

#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/build-wpr-88cb61e3/save-integrity/integrity.h"
#include "C:/Users/bebe/.codex/worktrees/8621/OpenTTD-patches/src/safeguards.h"

/** Derived reverse index. Only Journey packets consume entries; never saved or used for routing. */
static std::map<VehicleID, std::set<CargoPacketID>> _passenger_journey_vehicle_packets;
static std::unordered_map<uint32_t, StationCargoList *> _passenger_waiting_owners;
static std::set<StationCargoList *> _passenger_waiting_lists;
using PassengerWaitingOwner = std::pair<StationID, CargoType>;
using PassengerWaitingKey = std::tuple<StateTicks, StationID, CargoType>;
static std::map<PassengerWaitingOwner, StationCargoList *> _passenger_station_lists;
static std::array<std::set<StationID>, NUM_CARGO> _passenger_unresolved_stations;
static std::map<PassengerWaitingKey, StationCargoList *> _passenger_due_lists;
static StateTicks _passenger_event_tick = INVALID_STATE_TICKS;
static size_t _passenger_event_work = 0;
static PassengerWaitingEventDiagnostic _passenger_event_diagnostic;

bool ConsumePassengerRoutingWork()
{
	if (_passenger_event_tick != _state_ticks) { _passenger_event_tick = _state_ticks; _passenger_event_work = 0; }
	if (_passenger_event_work == PASSENGER_ROUTING_TICK_WORK) return false;
	++_passenger_event_work;
	return true;
}

void StationCargoList::BindPassengerStation(StationID station, CargoType cargo)
{
	this->passenger_station = station; this->passenger_index_cargo = cargo;
	if (station != StationID::Invalid() && cargo < NUM_CARGO) _passenger_station_lists[{station, cargo}] = this;
}

CargoPacketID StationCargoList::NextPassengerGenerationPacket(uint32_t cursor) const
{
	auto next = this->routing_packets.lower_bound(CargoPacketID{cursor});
	CargoPacketID result = next == this->routing_packets.end() ? CargoPacketID::Invalid() : *next;
	if (_settings_game.linkgraph.passenger_journey_hyperpath) {
		auto legacy = this->legacy_journey_packets.lower_bound(CargoPacketID{cursor});
		if (legacy != this->legacy_journey_packets.end() && (result == CargoPacketID::Invalid() || *legacy < result)) result = *legacy;
	}
	return result;
}

void StationCargoList::UpdatePassengerRoutingSchedule() const
{
	if (this->passenger_station == StationID::Invalid() || this->passenger_index_cargo >= NUM_CARGO) return;
	const bool unresolved = this->waiting_index_valid && this->NextPassengerGenerationPacket(0) != CargoPacketID::Invalid();
	if (unresolved != this->passenger_unresolved_registered) {
		if (unresolved) _passenger_unresolved_stations[this->passenger_index_cargo].insert(this->passenger_station);
		else _passenger_unresolved_stations[this->passenger_index_cargo].erase(this->passenger_station);
		this->passenger_unresolved_registered = unresolved;
	}
	StateTicks due = INVALID_STATE_TICKS;
	if (this->waiting_index_valid && _settings_game.linkgraph.passenger_journey_precomputed && IsPassengerJourneyRoutingEnabled(this->passenger_index_cargo)) {
		due = this->routing_events.empty() ? INVALID_STATE_TICKS : this->routing_events.begin()->first;
		if (unresolved && (this->passenger_routing_sweeping || this->passenger_routing_dirty)) {
			StateTicks generation = this->passenger_routing_due == INVALID_STATE_TICKS ? _state_ticks : this->passenger_routing_due;
			due = due == INVALID_STATE_TICKS ? generation : std::min(due, generation);
		}
		/* Unserved stations retain their old deadline ahead of a station that
		 * has used its quantum. Count/payment-only refreshes keep the handle. */
		if (due != INVALID_STATE_TICKS && this->passenger_routing_work >= PASSENGER_ROUTING_SLICE && this->passenger_routing_tick >= due) due = this->passenger_routing_tick + 1;
		_passenger_event_diagnostic.maximum_station_backlog = std::max<uint64_t>(_passenger_event_diagnostic.maximum_station_backlog, this->routing_events.size());
	}
	if (due == this->passenger_scheduled_at) return;
	if (this->passenger_scheduled_at != INVALID_STATE_TICKS) _passenger_due_lists.erase({this->passenger_scheduled_at, this->passenger_station, this->passenger_index_cargo});
	this->passenger_scheduled_at = due;
	if (due != INVALID_STATE_TICKS) _passenger_due_lists.emplace(PassengerWaitingKey{due, this->passenger_station, this->passenger_index_cargo}, const_cast<StationCargoList *>(this));
}

void RebuildPassengerWaitingScheduler()
{
	_passenger_event_tick = _state_ticks; _passenger_event_work = 0;
	_passenger_due_lists.clear();
	for (auto *list : _passenger_waiting_lists) {
		list->passenger_scheduled_at = INVALID_STATE_TICKS;
		if (list->passenger_routing_tick == _state_ticks) _passenger_event_work += list->passenger_routing_work;
		list->UpdatePassengerRoutingSchedule();
	}
	_passenger_event_work = std::min(_passenger_event_work, PASSENGER_ROUTING_TICK_WORK);
}

void RunPassengerWaitingEvents()
{
	if (!_settings_game.linkgraph.passenger_journey_precomputed || !_settings_game.linkgraph.passenger_journey_routing) return;
	PassengerJourneyWorkTimer journey_timer;
	const auto start = std::chrono::steady_clock::now();
	if (_passenger_event_tick != _state_ticks) { _passenger_event_tick = _state_ticks; _passenger_event_work = 0; }
	while (_passenger_event_work < PASSENGER_ROUTING_TICK_WORK && !_passenger_due_lists.empty() && std::get<0>(_passenger_due_lists.begin()->first) <= _state_ticks) {
		auto *list = _passenger_due_lists.begin()->second;
		const auto before = _passenger_event_work;
		list->RoutePassengerJourneys(list->passenger_index_cargo, list->passenger_station);
		/* Empty generation sweeps also cost a national operation. */
		if (_passenger_event_work == before) {
			if (!ConsumePassengerRoutingWork()) break;
			++list->passenger_routing_work;
		}
		list->UpdatePassengerRoutingSchedule();
	}
	_passenger_event_diagnostic.scheduler_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}

void WakePassengerWaitingCargo(CargoType cargo)
{
	for (StationID station : _passenger_unresolved_stations[cargo]) {
		auto *list = _passenger_station_lists.at({station, cargo});
		if (!list->passenger_routing_sweeping && !list->passenger_routing_dirty) list->passenger_routing_due = _state_ticks;
		list->passenger_routing_dirty = true;
		list->UpdatePassengerRoutingSchedule();
	}
}

PassengerWaitingEventDiagnostic GetPassengerWaitingEventDiagnostic()
{
	auto result = _passenger_event_diagnostic;
	if (!_settings_game.linkgraph.passenger_journey_precomputed) return result;
	/* Explicit diagnostics only; simulation never walks this registry per tick. */
	for (const auto &[owner, list] : _passenger_station_lists) {
		if (!IsPassengerJourneyRoutingEnabled(owner.second)) continue;
		uint64_t pending = list->routing_events.size();
		for (const auto &[due, id] : list->routing_events) {
			if (due > _state_ticks) break;
			++result.overdue;
			result.oldest_ticks = std::max<uint64_t>(result.oldest_ticks, uint64_t(_state_ticks.base()) - uint64_t(due.base()));
		}
		if (list->passenger_routing_dirty || list->passenger_routing_sweeping) for (CargoPacketID id = list->NextPassengerGenerationPacket(0); id != CargoPacketID::Invalid(); id = list->NextPassengerGenerationPacket(id.base() + 1)) {
			if (!list->passenger_routing_dirty && (id.base() < list->passenger_routing_cursor || id.base() > list->passenger_routing_end)) continue;
			if (list->waiting_entries.at(id).routing_due != INVALID_STATE_TICKS) continue;
			++pending; ++result.overdue;
			if (list->passenger_routing_due != INVALID_STATE_TICKS && list->passenger_routing_due <= _state_ticks)
				result.oldest_ticks = std::max<uint64_t>(result.oldest_ticks, uint64_t(_state_ticks.base()) - uint64_t(list->passenger_routing_due.base()));
		}
		result.pending += pending;
		result.maximum_station_backlog = std::max(result.maximum_station_backlog, pending);
	}
	return result;
}

void ResetPassengerWaitingEventDiagnostic() { _passenger_event_diagnostic = {}; }
void RecordPassengerWaitingCause(PassengerWaitingCause cause, uint64_t nanoseconds, bool unchanged)
{
	_passenger_event_diagnostic.cause_nanoseconds[static_cast<size_t>(cause)] += nanoseconds;
	++_passenger_event_diagnostic.cause_packets[static_cast<size_t>(cause)];
	_passenger_event_diagnostic.unchanged_generation_packets += unchanged;
}
void RecordPassengerWaitingEvent(StateTicks due)
{
	++_passenger_event_diagnostic.packets;
	if (due == _state_ticks) ++_passenger_event_diagnostic.on_time;
}
void RecordPassengerWaitingGroup() { ++_passenger_event_diagnostic.groups; }
void RecordPassengerWaitingArrival(uint64_t packets, uint64_t passengers, bool missed)
{
	_passenger_event_diagnostic.arrival_candidates += packets;
	_passenger_event_diagnostic.arrival_passengers += passengers;
	if (missed) _passenger_event_diagnostic.unresolved_missed += passengers;
}

void InvalidatePassengerWaitingRouting()
{
	/* Index residency is derived. In particular a joining peer rebuilds lists
	 * that a server may not have referenced yet. Save the same generation event
	 * for every owning goods entry, irrespective of that local residency. */
	for (const auto &[owner, list] : _passenger_station_lists) {
		list->passenger_routing_dirty = true;
		if (!list->passenger_routing_sweeping) list->passenger_routing_due = _state_ticks;
		/* Enabling routing (or evicting a derived index) must also enroll a
		 * station that has no loading vehicle and receives no new packets. */
		if (_settings_game.linkgraph.passenger_journey_precomputed && IsPassengerJourneyRoutingEnabled(owner.second)) list->RebuildWaitingIndex();
		list->UpdatePassengerRoutingSchedule();
	}
	/* Standalone lists are also used by cargo actions and regression fixtures. */
	for (auto *list : _passenger_waiting_lists) list->passenger_routing_dirty = true;
}

void ShiftPassengerWaitingEventTicks(StateTicksDelta delta)
{
	for (Station *station : Station::Iterate()) for (CargoType cargo : EnumRange(NUM_CARGO)) if (station->goods[cargo].data != nullptr) {
		auto &list = station->goods[cargo].data->cargo;
		if (list.passenger_routing_due != INVALID_STATE_TICKS) list.passenger_routing_due += delta;
		if (list.passenger_routing_tick != INVALID_STATE_TICKS) list.passenger_routing_tick += delta;
	}
	RebuildPassengerWaitingScheduler();
}

void PassengerWaitingPacketChanged(const CargoPacket &packet)
{
	auto owner = _passenger_waiting_owners.find(packet.index.base());
	if (owner != _passenger_waiting_owners.end()) owner->second->RefreshWaitingPacket(packet);
}

/** Keep an older pending event ahead of newly arriving work. Count/payment-only
 * changes refresh the indices without waking a dormant routing decision. */
void PassengerWaitingRoutingChanged(CargoPacket &packet)
{
	const auto due = packet.GetPassengerRoutingEvent();
	packet.SchedulePassengerRouting(due == INVALID_STATE_TICKS ? _state_ticks : std::min(due, _state_ticks));
}

static void RegisterPassengerJourneyPacket(const CargoPacket &packet)
{
	if (packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey && packet.index != CargoPacketID::Invalid() && packet.GetPassengerJourney().vehicle != VehicleID::Invalid()) {
		_passenger_journey_vehicle_packets[packet.GetPassengerJourney().vehicle].insert(packet.index);
	}
}

static void UnregisterPassengerJourneyPacket(const CargoPacket &packet)
{
	if (packet.GetPassengerRoutingMode() != PassengerRoutingMode::Journey) return;
	auto found = _passenger_journey_vehicle_packets.find(packet.GetPassengerJourney().vehicle);
	if (found == _passenger_journey_vehicle_packets.end()) return;
	found->second.erase(packet.index);
	if (found->second.empty()) _passenger_journey_vehicle_packets.erase(found);
}

void RebuildPassengerJourneyVehicleIndex()
{
	_passenger_journey_vehicle_packets.clear();
	for (const CargoPacket *packet : CargoPacket::Iterate()) RegisterPassengerJourneyPacket(*packet);
}

size_t InvalidatePassengerJourneyVehicle(VehicleID vehicle)
{
	auto found = _passenger_journey_vehicle_packets.find(vehicle);
	if (found == _passenger_journey_vehicle_packets.end()) return 0;
	/* Detach first: Fallback removes the packet's old index entry too. Pool IDs are
	 * visited in the same order as the old whole-pool scan, before vehicle ID reuse. */
	auto packets = std::move(found->second);
	_passenger_journey_vehicle_packets.erase(found);
	for (CargoPacketID id : packets) {
		RecordPassengerJourneyFallback(PassengerJourneyFallbackReason::VehicleDeleted);
		CargoPacket *packet = CargoPacket::Get(id);
		if (IsEstimatedPassengerRide(packet->GetPassengerJourney().kind)) RecordPassengerScheduleInvalidation(PassengerScheduleInvalidation::VehicleDeleted);
		/* Waiting/reserved passengers must observe the existing station cooldown even
		 * if another vehicle immediately reuses this pool ID. A different interchange
		 * can still retry, as with other journey failures. */
		packet->FallbackPassengerJourney(packet->GetPassengerJourney().boarding, _state_ticks);
	}
	return packets.size();
}

void CargoPacket::PreCleanPool()
{
	_passenger_journey_vehicle_packets.clear();
	_passenger_waiting_owners.clear();
	_passenger_waiting_lists.clear();
}

void CargoPacket::SetPassengerJourney(PassengerJourneyExecution journey, StationID legacy_hop)
{
	bool same_vehicle = this->passenger_routing_mode == PassengerRoutingMode::Journey && this->passenger_journey.vehicle == journey.vehicle;
	if (!same_vehicle) UnregisterPassengerJourneyPacket(*this);
	this->passenger_routing_mode = PassengerRoutingMode::Journey;
	this->passenger_journey = journey;
	this->passenger_legacy_hop = legacy_hop;
	this->passenger_retry_station = StationID::Invalid();
	this->passenger_retry_after = {};
	this->passenger_via = StationID::Invalid();
	if (!same_vehicle) RegisterPassengerJourneyPacket(*this);
	PassengerWaitingRoutingChanged(*this);
}

void CargoPacket::RecordPassengerCapacityFailure(OrderListID service, StateTicks now, int64_t expected_wait)
{
	if (service == OrderListID::Invalid()) return;
	const auto *list = OrderList::GetIfValid(service); if (list == nullptr) return;
	const auto incarnation = list->GetCargoDistRouteGeneration();
	auto &failure = this->passenger_boarding_failure;
	if (!failure.Present() || failure.Expired(now, this->passenger_journey.waiting_limit) || (failure.service == service && failure.incarnation != incarnation)) failure = {service, 1, now, incarnation};
	else if (failure.service == service && failure.count < PassengerBoardingFailure::THRESHOLD) ++failure.count;
	/* Saturate the existing exclusion latch at the recording opportunity only.
	 * The hold/start identity and exact split/merge comparison remain unchanged;
	 * no timer event or saved field is introduced. Actual opportunity counts
	 * remain in the capacity diagnostic, independently of this saturated latch. */
	static constexpr int64_t T_FAIL = 4096;
	/* Subtraction avoids overflowing elapsed time plus the expected wait. */
	if (expected_wait > 0 && failure.service == service && failure.incarnation == incarnation && now >= failure.since &&
			(expected_wait >= T_FAIL || uint64_t(now.base()) - uint64_t(failure.since.base()) >= uint64_t(T_FAIL - expected_wait))) {
		failure.count = PassengerBoardingFailure::THRESHOLD;
	}
	/* Interleaved full services must not reset this counter forever. Track one
	 * identity until success/expiry, including before it reaches the threshold. */
	PassengerWaitingRoutingChanged(*this);
}

OrderListID CargoPacket::GetPassengerFailedService(StateTicks now) const
{
	const auto failed = this->passenger_boarding_failure.Excluded(now, this->passenger_journey.waiting_limit);
	const auto *list = OrderList::GetIfValid(failed);
	return list != nullptr && list->GetCargoDistRouteGeneration() == this->passenger_boarding_failure.incarnation ? failed : OrderListID::Invalid();
}

void CargoPacket::FallbackPassengerJourney(StationID station, StateTicks now, StationID legacy_hop)
{
	UnregisterPassengerJourneyPacket(*this);
	this->passenger_routing_mode = PassengerRoutingMode::Fallback;
	this->passenger_journey = {};
	this->passenger_legacy_hop = legacy_hop == station ? StationID::Invalid() : legacy_hop;
	this->passenger_via = StationID::Invalid();
	this->passenger_retry_station = station;
	this->passenger_retry_after = now + 256;
	PassengerWaitingRoutingChanged(*this);
}

void CargoPacket::DeferPassengerJourney(StationID station, StateTicks now, StationID legacy_hop)
{
	UnregisterPassengerJourneyPacket(*this);
	this->passenger_routing_mode = PassengerRoutingMode::Legacy;
	this->passenger_journey = {};
	this->passenger_legacy_hop = legacy_hop == station ? StationID::Invalid() : legacy_hop;
	this->passenger_via = StationID::Invalid();
	this->passenger_retry_station = station;
	this->passenger_retry_after = now + 1;
	PassengerWaitingRoutingChanged(*this);
}

void CargoPacket::ShiftPassengerJourneyTicks(StateTicksDelta delta)
{
	if (this->passenger_boarding_failure.Present()) this->passenger_boarding_failure.since += delta;
	if (this->passenger_routing_event != INVALID_STATE_TICKS) this->passenger_routing_event += delta;
	if (this->passenger_retry_after != StateTicks{0}) this->passenger_retry_after += delta;
	if (this->passenger_arriving_service != OrderListID::Invalid()) this->passenger_interchange_ready += delta;
	if (this->passenger_routing_mode == PassengerRoutingMode::Journey) {
		if (!IsEstimatedPassengerRide(this->passenger_journey.kind)) { this->passenger_journey.departure += delta; this->passenger_journey.arrival += delta; }
		if (IsEstimatedPassengerRide(this->passenger_journey.kind)) this->passenger_journey.planned_at += delta;
		if (this->passenger_journey.kind != PassengerRideKind::VehicleSnapshot) this->passenger_journey.not_before += delta;
	}
	PassengerWaitingPacketChanged(*this);
}

/* Initialize the cargopacket-pool */
CargoPacketPool _cargopacket_pool("CargoPacket");
INSTANTIATE_POOL_METHODS(CargoPacket)

btree::btree_map<uint64_t, Money> _cargo_packet_deferred_payments;
uint64_t SaveIntegrity::Payments() { uint64_t digest = 14695981039346656037ULL; for (const auto &[key, money] : _cargo_packet_deferred_payments) SaveIntegrity::Mix(digest, key, money); return digest; }

void ClearCargoPacketDeferredPayments() {
	_cargo_packet_deferred_payments.clear();
}

void ChangeOwnershipOfCargoPacketDeferredPayments(Owner old_owner, Owner new_owner)
{
	std::vector<std::pair<uint64_t, Money>> to_merge;
	auto iter = _cargo_packet_deferred_payments.begin();
	while (iter != _cargo_packet_deferred_payments.end()) {
		uint64_t k = iter->first;
		if ((CompanyID) GB(k, 24, 8) == old_owner) {
			if (new_owner != INVALID_OWNER) {
				SB(k, 24, 8, new_owner.base());
				to_merge.push_back({ k, iter->second });
			}
			iter = _cargo_packet_deferred_payments.erase(iter);
		} else {
			++iter;
		}
	}
	for (auto &m : to_merge) {
		_cargo_packet_deferred_payments[m.first] += m.second;
	}
}

inline uint64_t CargoPacketDeferredPaymentKey(CargoPacketID id, CompanyID cid, VehicleType type)
{
	return (((uint64_t) id.base()) << 32) | (cid.base() << 24) | (to_underlying(type) << 22);
}

template <typename F>
inline void IterateCargoPacketDeferredPayments(CargoPacketID index, bool erase_range, F functor)
{
	auto start_iter = _cargo_packet_deferred_payments.lower_bound(CargoPacketDeferredPaymentKey(index, (CompanyID) 0, (VehicleType) 0));
	auto iter = start_iter;
	for (; iter != _cargo_packet_deferred_payments.end() && iter->first >> 32 == index; ++iter) {
		functor(iter->second, (CompanyID) GB(iter->first, 24, 8), (VehicleType) GB(iter->first, 22, 2));
	}
	if (erase_range) {
		_cargo_packet_deferred_payments.erase(start_iter, iter);
	}
}

std::string DumpCargoPacketDeferredPaymentStats()
{
	Money payments[256][4] = {};
	for (auto &it : _cargo_packet_deferred_payments) {
		payments[GB(it.first, 24, 8)][GB(it.first, 22, 2)] += it.second;
	}

	format_buffer buffer;
	for (uint i = 0; i < 256; i++) {
		for (uint j = 0; j < 4; j++) {
			if (payments[i][j] != 0) {
				AppendStringInPlace(buffer, STR_COMPANY_NAME, i);
				buffer.append(" (");
				AppendStringInPlace(buffer, STR_REPLACE_VEHICLE_TRAIN + j);
				buffer.append("): ");
				AppendStringInPlace(buffer, STR_JUST_CURRENCY_LONG, payments[i][j]);
				buffer.push_back('\n');
			}
		}
	}
	buffer.format("Deferred payment count: {}\n", _cargo_packet_deferred_payments.size());
	buffer.format("Total cargo packets: {}\n", CargoPacket::GetNumItems());
	return buffer.to_string();
}

/**
 * Create a new packet for savegame loading.
 * @param index Index into the cargo packet pool.
 */
CargoPacket::CargoPacket(CargoPacketID index) : PoolItemBase(index)
{
}

/**
 * Creates a new cargo packet.
 *
 * @param index Index into the cargo packet pool.
 * @param first_station Source station of the packet.
 * @param count         Number of cargo entities to put in this packet.
 * @param source        Source of the packet (for subsidies).
 * @pre count != 0
 */
CargoPacket::CargoPacket(CargoPacketID index, StationID first_station,uint16_t count, Source source) :
		PoolItemBase(index),
		count(count),
		source(source),
		first_station(first_station)
{
	dbg_assert(count != 0);
}

/**
 * Create a new cargo packet. Used for older savegames to load in their partial data.
 *
 * @param index Index into the cargo packet pool.
 * @param count              Number of cargo entities to put in this packet.
 * @param periods_in_transit Number of cargo aging periods the cargo has been in transit.
 * @param first_station      Station the cargo was initially loaded.
 * @param source_xy          Station location the cargo was initially loaded.
 * @param feeder_share       Feeder share the packet has already accumulated.
 */
CargoPacket::CargoPacket(CargoPacketID index, uint16_t count, uint16_t periods_in_transit, StationID first_station, TileIndex source_xy, Money feeder_share) :
		PoolItemBase(index),
		count(count),
		periods_in_transit(periods_in_transit),
		feeder_share(feeder_share),
		source_xy(source_xy),
		first_station(first_station)
{
	assert(count != 0);
}

/**
 * Creates a new cargo packet. Used when loading or splitting packets.
 *
 * @param index Index into the cargo packet pool.
 * @param count         Number of cargo entities to put in this packet.
 * @param feeder_share  Feeder share the packet has already accumulated.
 * @param original      The original packet we are splitting.
 */
CargoPacket::CargoPacket(CargoPacketID index, uint16_t count, Money feeder_share, const CargoPacket &original) :
		PoolItemBase(index),
		count(count),
		periods_in_transit(original.periods_in_transit),
		feeder_share(feeder_share),
		source_xy(original.source_xy),
		travelled(original.travelled),
		source(original.source),
		first_station(original.first_station),
		next_hop(original.next_hop),
		passenger_via(original.passenger_via),
		passenger_destination(original.passenger_destination),
		passenger_routing_mode(original.passenger_routing_mode),
		passenger_journey(original.passenger_journey),
		passenger_boarding_failure(original.passenger_boarding_failure),
		passenger_legacy_hop(original.passenger_legacy_hop),
		passenger_retry_station(original.passenger_retry_station),
		passenger_retry_after(original.passenger_retry_after),
		passenger_interchange_ready(original.passenger_interchange_ready),
		passenger_arriving_service(original.passenger_arriving_service),
		passenger_arriving_incarnation(original.passenger_arriving_incarnation),
		passenger_transport(original.passenger_transport),
		passenger_interchange_transport(original.passenger_interchange_transport),
		passenger_schedule_used(original.passenger_schedule_used),
		passenger_routing_event(original.passenger_routing_event)
{
	dbg_assert(count != 0);
	RegisterPassengerJourneyPacket(*this);
#ifdef WITH_FULL_ASSERTS
	this->flags |= (original.flags & CPF_IN_VEHICLE);
#endif /* WITH_FULL_ASSERTS */
}

/** Destroy the packet. */
CargoPacket::~CargoPacket()
{
	if (CleaningPool()) return;
	UnregisterPassengerJourneyPacket(*this);
	auto owner = _passenger_waiting_owners.find(this->index.base());
	if (owner != _passenger_waiting_owners.end()) owner->second->ForgetWaitingPacket(this->index);

	if (this->flags & CPF_HAS_DEFERRED_PAYMENT) {
		IterateCargoPacketDeferredPayments(this->index, true, [](Money &payment, CompanyID cid, VehicleType type) { });
	}
}

/**
 * Split this packet in two and return the split off part.
 * @param new_size Size of the split part.
 * @return Split off part, or nullptr if no packet could be allocated!
 */
CargoPacket *CargoPacket::Split(uint new_size)
{
	if (!CargoPacket::CanAllocateItem()) return nullptr;

	Money fs = this->GetFeederShare(new_size);
	CargoPacket *cp_new = CargoPacket::Create(new_size, fs, *this);
	this->feeder_share -= fs;

	if (this->flags & CPF_HAS_DEFERRED_PAYMENT) {
		std::vector<std::pair<uint64_t, Money>> to_add;
		IterateCargoPacketDeferredPayments(this->index, false, [&](Money &payment, CompanyID cid, VehicleType type) {
			Money share = payment * new_size / static_cast<uint>(this->count);
			payment -= share;
			to_add.push_back({ CargoPacketDeferredPaymentKey(cp_new->index, cid, type), share });
		});
		for (auto &m : to_add) {
			_cargo_packet_deferred_payments[m.first] = m.second;
		}
		cp_new->flags |= CPF_HAS_DEFERRED_PAYMENT;
	}

	this->count -= new_size;
	PassengerWaitingPacketChanged(*this);
	return cp_new;
}

/**
 * Merge another packet into this one.
 * @param cp Packet to be merged in.
 * @return False if final destinations differ; both packets remain unchanged.
 * @pre Other merge attributes and the combined count have been checked by the caller.
 */
bool CargoPacket::Merge(CargoPacket *cp)
{
	if (this->passenger_destination != cp->passenger_destination || !this->HasSamePassengerRouting(*cp)) return false;
	this->count += cp->count;
	this->feeder_share += cp->feeder_share;
	if (this->passenger_routing_event == INVALID_STATE_TICKS ||
			(cp->passenger_routing_event != INVALID_STATE_TICKS && cp->passenger_routing_event < this->passenger_routing_event)) this->passenger_routing_event = cp->passenger_routing_event;
	PassengerWaitingPacketChanged(*this);

	if (cp->flags & CPF_HAS_DEFERRED_PAYMENT) {
		std::vector<std::pair<uint64_t, Money>> to_merge;
		IterateCargoPacketDeferredPayments(cp->index, true, [&](Money &payment, CompanyID cid, VehicleType type) {
			to_merge.push_back({ CargoPacketDeferredPaymentKey(this->index, cid, type), payment });
		});
		cp->flags &= ~CPF_HAS_DEFERRED_PAYMENT;

		for (auto &m : to_merge) {
			_cargo_packet_deferred_payments[m.first] += m.second;
		}
		this->flags |= CPF_HAS_DEFERRED_PAYMENT;
	}

	delete cp;
	return true;
}

bool CargoPacket::SetPassengerDestination(CargoType cargo, StationID destination)
{
	if (cargo >= NUM_CARGO) return false;
	const CargoSpec *cs = CargoSpec::Get(cargo);
	if (!cs->IsValid() || !cs->classes.Test(CargoClass::Passengers)) return false;
	if (destination != StationID::Invalid() && !Station::IsValidID(destination)) return false;
	if (this->passenger_destination != StationID::Invalid() && this->passenger_destination != destination) return false;
	this->passenger_destination = destination;
	PassengerWaitingRoutingChanged(*this);
	return true;
}

/**
 * Reduce the packet by the given amount and remove the feeder share.
 * @param count Amount to be removed.
 */
void CargoPacket::Reduce(uint count)
{
	dbg_assert(count < this->count);
	this->feeder_share -= this->GetFeederShare(count);
	if (this->flags & CPF_HAS_DEFERRED_PAYMENT) {
		IterateCargoPacketDeferredPayments(this->index, false, [&](Money &payment, CompanyID cid, VehicleType type) {
			payment -= payment * count / static_cast<uint>(this->count);
		});
	}
	this->count -= count;
	PassengerWaitingPacketChanged(*this);
}

void CargoPacket::RegisterDeferredCargoPayment(CompanyID cid, VehicleType type, Money payment)
{
	this->flags |= CPF_HAS_DEFERRED_PAYMENT;
	_cargo_packet_deferred_payments[CargoPacketDeferredPaymentKey(this->index, cid, type)] += payment;
}

void CargoPacket::PayDeferredPayments()
{
	if (this->flags & CPF_HAS_DEFERRED_PAYMENT) {
		IterateCargoPacketDeferredPayments(this->index, true, [&](Money &payment, CompanyID cid, VehicleType type) {
			ExpensesType exp;
			switch (type) {
				case VehicleType::Train: exp = ExpensesType::TrainRevenue; break;
				case VehicleType::Road: exp = ExpensesType::RoadVehRevenue; break;
				case VehicleType::Ship: exp = ExpensesType::ShipRevenue; break;
				case VehicleType::Aircraft: exp = ExpensesType::AircraftRevenue; break;
				default: NOT_REACHED();
			}
			SubtractMoneyFromCompany(cid, CommandCost(exp, -payment));
		});
		this->flags &= ~CPF_HAS_DEFERRED_PAYMENT;
	}
}

/**
 * Invalidates (sets source_id to INVALID_SOURCE) all cargo packets from given source.
 * @param src Index of source.
 */
/* static */ void CargoPacket::InvalidateAllFrom(Source src)
{
	for (CargoPacket *cp : CargoPacket::Iterate()) {
		if (cp->source == src) { cp->source.MakeInvalid(); PassengerWaitingPacketChanged(*cp); }
	}
}

/**
 * Invalidates the origin, passenger commitment and final destination referencing a removed station.
 * @param sid Station that gets removed.
 */
/* static */ void CargoPacket::InvalidateAllFrom(StationID sid)
{
	for (CargoPacket *cp : CargoPacket::Iterate()) {
		if (cp->first_station == sid) cp->first_station = StationID::Invalid();
		if (cp->passenger_via == sid) cp->passenger_via = StationID::Invalid();
		if (cp->passenger_legacy_hop == sid) cp->passenger_legacy_hop = StationID::Invalid();
		if (cp->passenger_destination == sid) cp->passenger_destination = StationID::Invalid();
		if (cp->passenger_routing_mode == PassengerRoutingMode::Journey &&
				(cp->passenger_journey.boarding == sid || cp->passenger_journey.alighting == sid || cp->passenger_destination == StationID::Invalid())) {
			cp->FallbackPassengerJourney(StationID::Invalid(), _state_ticks);
		}
		PassengerWaitingPacketChanged(*cp);
	}
}

/* static */ bool CargoPacket::ValidateDeferredCargoPayments()
{
	for (auto &it : _cargo_packet_deferred_payments) {
		uint id = it.first >> 32;
		const CargoPacket *cp = CargoPacket::GetIfValid(id);
		if (!cp) return false;
		if (!(cp->flags & CPF_HAS_DEFERRED_PAYMENT)) return false;
	}
	return true;
}

/*
 *
 * Cargo list implementation
 *
 */

/**
 * Destroy the cargolist ("frees" all cargo packets).
 */
template <class Tinst, class Tcont>
CargoList<Tinst, Tcont>::~CargoList()
{
	for (Iterator it(this->packets.begin()); it != this->packets.end(); ++it) {
		delete *it;
	}
}

/**
 * Empty the cargo list, but don't free the cargo packets;
 * the cargo packets are cleaned by CargoPacket's CleanPool.
 */
template <class Tinst, class Tcont>
void CargoList<Tinst, Tcont>::OnCleanPool()
{
	if constexpr (std::is_same_v<Tinst, StationCargoList>) static_cast<Tinst *>(this)->ClearWaitingIndex();
	this->packets.clear();
}

/**
 * Update the cached values to reflect the removal of this packet or part of it.
 * Decreases count and periods_in_transit.
 * @param cp Packet to be removed from cache.
 * @param count Amount of cargo from the given packet to be removed.
 */
template <class Tinst, class Tcont>
void CargoList<Tinst, Tcont>::RemoveFromCache(const CargoPacket *cp, uint count)
{
	dbg_assert(count <= cp->count);
	this->count -= count;
	this->cargo_periods_in_transit -= static_cast<uint64_t>(cp->periods_in_transit) * count;
}

/**
 * Update the cache to reflect adding of this packet.
 * Increases count and periods_in_transit.
 * @param cp New packet to be inserted.
 */
template <class Tinst, class Tcont>
void CargoList<Tinst, Tcont>::AddToCache(const CargoPacket *cp)
{
	this->count += cp->count;
	this->cargo_periods_in_transit += static_cast<uint64_t>(cp->periods_in_transit) * cp->count;
}

/** Invalidates the cached data and rebuilds it. */
template <class Tinst, class Tcont>
void CargoList<Tinst, Tcont>::InvalidateCache()
{
	if constexpr (std::is_same_v<Tinst, StationCargoList>) static_cast<Tinst *>(this)->ClearWaitingIndex();
	this->count = 0;
	this->cargo_periods_in_transit = 0;

	for (ConstIterator it(this->packets.begin()); it != this->packets.end(); it++) {
		static_cast<Tinst *>(this)->AddToCache(*it);
	}
}

/**
 * Tries to merge the second packet into the first and return if that was
 * successful.
 * @param icp Packet to be merged into.
 * @param cp Packet to be eliminated.
 * @return If the packets could be merged.
 */
template <class Tinst, class Tcont>
/* static */ bool CargoList<Tinst, Tcont>::TryMerge(CargoPacket *icp, CargoPacket *cp)
{
	if (Tinst::AreMergable(icp, cp) &&
			icp->count + cp->count <= CargoPacket::MAX_COUNT) {
		return icp->Merge(cp);
	} else {
		return false;
	}
}

/*
 *
 * Vehicle cargo list implementation.
 *
 */

/**
 * Appends the given cargo packet. Tries to merge it with another one in the
 * packets list. If no fitting packet is found, appends it. You can only append
 * packets to the ranges of packets designated for keeping or loading.
 * Furthermore if there are already packets reserved for loading you cannot
 * directly add packets to the "keep" list. You first have to load the reserved
 * ones.
 * @warning After appending this packet may not exist anymore!
 * @note Do not use the cargo packet anymore after it has been appended to this CargoList!
 * @param cp Cargo packet to add.
 * @param action Either MoveToAction::Keep if you want to add the packet directly or MoveToAction::Load
 * if you want to reserve it first.
 * @pre cp != nullptr
 * @pre action == MoveToAction::Load || (action == MoveToAction::Keep && this->designation_counts[MoveToAction::Load] == 0)
 */
void VehicleCargoList::Append(CargoPacket *cp, MoveToAction action)
{
	dbg_assert(cp != nullptr);
	dbg_assert(action == MoveToAction::Load ||
			(action == MoveToAction::Keep && this->action_counts[MoveToAction::Load] == 0));
	this->AddToMeta(cp, action);

	if (this->count == cp->count) {
		this->packets.push_back(cp);
		return;
	}

	uint sum = cp->count;
	for (ReverseIterator it(this->packets.rbegin()); it != this->packets.rend(); it++) {
		CargoPacket *icp = *it;
		if (VehicleCargoList::TryMerge(icp, cp)) return;
		sum += icp->count;
		if (sum >= this->action_counts[action]) {
			this->packets.push_back(cp);
			return;
		}
	}

	NOT_REACHED();
}

/**
 * Shifts cargo from the front of the packet list and applies some action to it.
 * @tparam Taction Action class or function to be used. It should define
 *                 "bool operator()(CargoPacket *)". If true is returned the
 *                 cargo packet will be removed from the list. Otherwise it
 *                 will be kept and the loop will be aborted.
 * @param action Action instance to be applied.
 */
template <class Taction>
void VehicleCargoList::ShiftCargo(Taction action)
{
	Iterator it(this->packets.begin());
	while (it != this->packets.end() && action.MaxMove() > 0) {
		CargoPacket *cp = *it;
		if (action(cp)) {
			it = this->packets.erase(it);
		} else {
			break;
		}
	}
}

/**
 * Shifts cargo from the front of the packet list and applies some action to it.
 * @tparam Taction Action class or function to be used. It should define
 *                 "bool operator()(CargoPacket *, std::vector<CargoPacket*> &)". If true is returned the
 *                 cargo packet will be removed from the list. Otherwise it
 *                 will be kept and the loop will be aborted.
 *                 The second method parameter can be appended to, to prepend items to the packet list
 * @param action Action instance to be applied.
 * @param filter Cargo packet filter.
 */
template <class Taction, class Tfilter>
void VehicleCargoList::ShiftCargoWithFrontInsert(Taction action, Tfilter filter)
{
	std::vector<CargoPacket *> packets_to_front_insert;

	Iterator it(this->packets.begin());
	while (it != this->packets.end() && action.MaxMove() > 0) {
		CargoPacket *cp = *it;
		if (!filter(cp)) {
			++it;
			continue;
		}
		if (action(cp, packets_to_front_insert)) {
			it = this->packets.erase(it);
		} else {
			break;
		}
	}

	for (CargoPacket *cp : packets_to_front_insert) {
		this->packets.push_front(cp);
	}
}

/**
 * Pops cargo from the back of the packet list and applies some action to it.
 * @tparam Taction Action class or function to be used. It should define
 *                 "bool operator()(CargoPacket *)". If true is returned the
 *                 cargo packet will be removed from the list. Otherwise it
 *                 will be kept and the loop will be aborted.
 * @param action Action instance to be applied.
 */
template <class Taction>
void VehicleCargoList::PopCargo(Taction action)
{
	if (this->packets.empty()) return;
	for (auto it = this->packets.end(); it != this->packets.begin();) {
		if (action.MaxMove() <= 0) break;
		--it;
		CargoPacket *cp = *it;
		if (action(cp)) {
			it = this->packets.erase(it);
		} else {
			break;
		}
	}
}

void VehicleCargoList::AssertCountConsistencyError() const
{
	assert_msg(this->action_counts[MoveToAction::Keep] +
			this->action_counts[MoveToAction::Deliver] +
			this->action_counts[MoveToAction::Transfer] +
			this->action_counts[MoveToAction::Load] == this->count,
			"{} + {} + {} + {} != {}, ({} in {} packets)",
			this->action_counts[MoveToAction::Keep],
			this->action_counts[MoveToAction::Deliver],
			this->action_counts[MoveToAction::Transfer],
			this->action_counts[MoveToAction::Load],
			this->count,
			this->RecalculateCargoTotal(),
			this->packets.size());
}

/**
 * Update the cached values to reflect the removal of this packet or part of it.
 * Decreases count, feeder share and periods_in_transit.
 * @param cp Packet to be removed from cache.
 * @param count Amount of cargo from the given packet to be removed.
 */
void VehicleCargoList::RemoveFromCache(const CargoPacket *cp, uint count)
{
	this->feeder_share -= cp->GetFeederShare(count);
	this->Parent::RemoveFromCache(cp, count);
}

/**
 * Update the cache to reflect adding of this packet.
 * Increases count, feeder share and periods_in_transit.
 * @param cp New packet to be inserted.
 */
void VehicleCargoList::AddToCache(const CargoPacket *cp)
{
	this->feeder_share += cp->feeder_share;
	this->Parent::AddToCache(cp);
}

/**
 * Removes a packet or part of it from the metadata.
 * @param cp Packet to be removed.
 * @param action MoveToAction of the packet (for updating the counts).
 * @param count Amount of cargo to be removed.
 */
void VehicleCargoList::RemoveFromMeta(const CargoPacket *cp, MoveToAction action, uint count)
{
	dbg_assert(count <= this->action_counts[action]);
	this->AssertCountConsistency();
	this->RemoveFromCache(cp, count);
	this->action_counts[action] -= count;
	this->AssertCountConsistency();
}

/**
 * Adds a packet to the metadata.
 * @param cp Packet to be added.
 * @param action MoveToAction of the packet.
 */
void VehicleCargoList::AddToMeta(const CargoPacket *cp, MoveToAction action)
{
	this->AssertCountConsistency();
	this->AddToCache(cp);
	this->action_counts[action] += cp->count;
	this->AssertCountConsistency();
}

/**
 * Ages the all cargo in this list.
 */
void VehicleCargoList::AgeCargo()
{
	for (const auto &cp : this->packets) {
		/* If we're at the maximum, then we can't increase no more. */
		if (cp->periods_in_transit == UINT16_MAX) continue;

		cp->periods_in_transit++;
		this->cargo_periods_in_transit += cp->count;
	}
}

/**
 * Choose action to be performed with the given cargo packet.
 * @param cp The packet.
 * @param cargo_next Next hop the cargo wants to pass.
 * @param current_station Current station of the vehicle carrying the cargo.
 * @param accepted If the cargo is accepted at the current station.
 * @param next_station Next station(s) the vehicle may stop at.
 * @return MoveToAction to be performed.
 */
/* static */ VehicleCargoList::MoveToAction VehicleCargoList::ChooseAction(const CargoPacket *cp, StationID cargo_next,
		StationID current_station, bool accepted, std::span<const StationID> next_station)
{
	if (cargo_next == StationID::Invalid()) {
		return (accepted && cp->first_station != current_station) ? MoveToAction::Deliver : MoveToAction::Keep;
	} else if (cargo_next == current_station) {
		return MoveToAction::Deliver;
	} else if (std::ranges::find(next_station, cargo_next) != std::end(next_station)) {
		return MoveToAction::Keep;
	} else {
		return MoveToAction::Transfer;
	}
}

/**
 * Stages cargo for unloading. The cargo is sorted so that packets to be
 * transferred, delivered or kept are in consecutive chunks in the list. At the
 * same time the designation_counts are updated to reflect the size of those
 * chunks.
 * @param accepted If the cargo will be accepted at the station.
 * @param current_station ID of the station.
 * @param next_station ID of the station the vehicle will go to next.
 * @param unload_type OrderUnloadType that will apply to the unload operation.
 * @param ge GoodsEntry for getting the flows.
 * @param cargo The cargo type of the cargo.
 * @param payment Payment object for registering transfers.
 * @param current_tile Current tile the cargo handling is happening on.
 * @param routing Arrival-scoped passenger prediction cache, or nullptr for original behaviour.
 * @return \c true iff any cargo will be unloaded.
 */
bool VehicleCargoList::Stage(bool accepted, StationID current_station, std::span<const StationID> next_station, OrderUnloadType unload_type, const GoodsEntry *ge, CargoType cargo, CargoPayment *payment, TileIndex current_tile, PassengerRoutingContext *routing, PassengerJourneyShadowContext *shadow, PassengerJourneyActiveContext *active)
{
	this->AssertCountConsistency();
	dbg_assert(this->action_counts[MoveToAction::Load] == 0);
	this->action_counts[MoveToAction::Transfer] = this->action_counts[MoveToAction::Deliver] = this->action_counts[MoveToAction::Keep] = 0;
	Iterator it = this->packets.begin();
	uint sum = 0;
	CargoPacketList transfer_deliver;
	std::vector<CargoPacket *> keep;
	std::map<std::pair<StationID, StationID>, bool> transfer_permissions;
	PassengerOdFallbackContext od_fallback;

	const FlowStatMap &flows = ge->ConstFlows();

	bool force_keep = (unload_type == OrderUnloadType::NoUnload);
	bool force_unload = (unload_type == OrderUnloadType::Unload);
	bool force_transfer = (unload_type == OrderUnloadType::Transfer) || (unload_type == OrderUnloadType::Unload);
	bool transfer_cargodist_mode = force_transfer && _settings_game.linkgraph.GetDistributionType(cargo) != DistributionType::Manual;
	const bool predict_passengers = routing != nullptr && routing->IsEnabled(cargo) && !force_transfer && !force_keep;
	dbg_assert(this->count > 0 || it == this->packets.end());
	while (sum < this->count) {
		CargoPacket *cp = *it;
		const bool destination_bound = IsPassengerJourneyRoutingEnabled(cargo) && Station::IsValidID(cp->GetPassengerDestination());
		/* Commit only while the same train still has a predictable journey to the chosen via.
		 * At the via, normal flow selection resumes; orders/settings changing cancel the commitment. */
		const bool committed = !destination_bound && predict_passengers && cp->passenger_via != StationID::Invalid() &&
				cp->passenger_via != current_station && routing->CanContinue(cargo, cp->passenger_via);
		if (!committed && !destination_bound) cp->passenger_via = StationID::Invalid();

		it = this->packets.erase(it);
		StationID cargo_next = StationID::Invalid();
		MoveToAction action = MoveToAction::Load;

		auto handle_forced_transfer = [&]() {
			action = MoveToAction::Transfer;
			/* We cannot send the cargo to any of the possible next hops and
			 * also not to the current station. */
			FlowStatMap::const_iterator flow_it(flows.find(cp->first_station));
			if (flow_it == flows.end()) {
				cargo_next = StationID::Invalid();
			} else {
				FlowStat new_shares = *flow_it;
				new_shares.ChangeShare(current_station, INT_MIN);
				for (StationID st : next_station) {
					if (new_shares.empty()) break;
					new_shares.ChangeShare(st, INT_MIN);
				}
				if (new_shares.empty()) {
					cargo_next = StationID::Invalid();
				} else {
					cargo_next = new_shares.GetVia();
				}
			}
		};

		const bool was_journey = cp->GetPassengerRoutingMode() == PassengerRoutingMode::Journey;
		/* Stage contains stored cargo only (the reserved suffix is asserted empty
		 * above). Mark actual schedule use here, after real loading and before the
		 * next intent can replace the carrier. Returned reservations stay unmarked. */
		if (cp->passenger_transport == PassengerTransportKind::Schedule) cp->passenger_schedule_used = true;
		auto journey_action = active == nullptr ? std::optional<MoveToAction>{} : active->Decide(*cp, cargo, accepted, unload_type);
		if (!journey_action) journey_action = od_fallback.Decide(*cp, payment->front, cargo, current_station, accepted, unload_type);
		if (journey_action) {
			action = *journey_action;
			cargo_next = cp->GetNextHop();
		} else if (force_keep) {
			action = MoveToAction::Keep;
		} else if (force_unload && !transfer_cargodist_mode && accepted && cp->first_station != current_station) {
			action = MoveToAction::Deliver;
		} else if (force_transfer && !transfer_cargodist_mode) {
			handle_forced_transfer();
		} else {
			/* Rewrite an invalid source station to some random other one to
			 * avoid keeping the cargo in the vehicle forever. */
			if (cp->first_station == StationID::Invalid() && !flows.empty()) {
				cp->first_station = flows.FirstStationID();
			}
			bool restricted = false;
			FlowStatMap::const_iterator flow_it(flows.find(cp->first_station));
			if (committed) {
				cargo_next = cp->passenger_via;
				/* Retaining a via does not grant permission to transfer onto a restricted flow. */
				auto [permission, inserted] = transfer_permissions.try_emplace(std::pair(cp->first_station, cargo_next), false);
				if (inserted && flow_it != flows.end()) {
					for (const auto &share_entry : *flow_it) {
						if (share_entry.second == cargo_next && share_entry.first <= flow_it->GetUnrestricted()) permission->second = true;
					}
				}
				restricted = !permission->second;
			} else if (flow_it == flows.end()) {
				cargo_next = StationID::Invalid();
			} else {
				cargo_next = flow_it->GetViaWithRestricted(restricted);
			}
			action = VehicleCargoList::ChooseAction(cp, cargo_next, current_station, accepted, next_station);
			if (!committed && restricted && action == MoveToAction::Transfer) {
				/* If the flow is restricted we can't transfer to it. Choose an
				 * unrestricted one instead. */
				cargo_next = flow_it->GetVia();
				restricted = false;
				action = VehicleCargoList::ChooseAction(cp, cargo_next, current_station, accepted, next_station);
			}
			if (predict_passengers && cargo_next != StationID::Invalid() && cargo_next != current_station) {
				const PassengerTransferDecision decision = routing->Decide(cargo, cargo_next);
				if (decision == PassengerTransferDecision::Stay || (committed && (decision == PassengerTransferDecision::Unknown || restricted))) {
					/* Revalidate reachability at every stop, but don't redraw this via at intermediate stations. */
					if (routing->CanContinue(cargo, cargo_next)) {
						action = MoveToAction::Keep;
						cp->passenger_via = cargo_next;
					}
				} else if (decision == PassengerTransferDecision::Transfer && !restricted && action == MoveToAction::Transfer) {
					/* Only permit a transfer already required by the selected via. If the train's
					 * next cargo stop is this via, the station loader could immediately reload it. */
					action = MoveToAction::Transfer;
					cp->passenger_via = StationID::Invalid();
				}
			}
			if (transfer_cargodist_mode && action == MoveToAction::Keep) {
				handle_forced_transfer();
			}
		}
		/* Final OD remains authoritative even when no active context was supplied. */
		if (destination_bound) {
			if (current_station == cp->GetPassengerDestination() && accepted && !force_keep) action = MoveToAction::Deliver;
			else if (action == MoveToAction::Deliver && current_station != cp->GetPassengerDestination()) {
				if (cp->GetPassengerRoutingMode() != PassengerRoutingMode::Journey || force_transfer) handle_forced_transfer();
				else action = MoveToAction::Keep;
			}
		}
		/* Observe the final action without repeating prediction. */
		if (was_journey && !accepted && action == MoveToAction::Deliver) action = MoveToAction::Keep;
		if (destination_bound && !accepted && action == MoveToAction::Deliver) action = MoveToAction::Keep;
		if (shadow != nullptr) shadow->Observe(*cp, cargo, action, cargo_next, unload_type);
		Money share;
		switch (action) {
			case MoveToAction::Keep:
				keep.push_back(cp);
				break;
			case MoveToAction::Deliver:
				transfer_deliver.push_back(cp);
				break;
			case MoveToAction::Transfer:
				transfer_deliver.push_front(cp);
				/* Add feeder share here to allow reusing field for next station. */
				share = payment->PayTransfer(cargo, cp, cp->count, current_tile);
				// cp->AddFeederShare(share);
				// this->feeder_share += share;
				cp->next_hop = cargo_next;
				break;
			default:
				NOT_REACHED();
		}
		this->action_counts[action] += cp->count;
		sum += cp->count;
	}
	dbg_assert(this->packets.empty());
	this->packets = std::move(transfer_deliver);
	this->packets.insert(this->packets.end(), keep.begin(), keep.end());
	this->AssertCountConsistency();
	return this->action_counts[MoveToAction::Deliver] > 0 || this->action_counts[MoveToAction::Transfer] > 0;
}

/** Invalidates the cached data and rebuild it. */
void VehicleCargoList::InvalidateCache()
{
	this->feeder_share = 0;
	this->Parent::InvalidateCache();
}

/**
 * Moves some cargo from one designation to another. You can only move
 * between adjacent designations. E.g. you can keep cargo that was previously
 * reserved (MoveToAction::Load), but you can't reserve cargo that's marked as to be
 * delivered. Transfer payment/next-hop changes require a specialized method.
 * A physical load may split packets carrying capacity failures so only the
 * successfully boarded people have their failure state cleared.
 * @tparam from Previous designation of cargo.
 * @tparam to New designation of cargo.
 * @param max_move Maximum amount of cargo to reassign.
 * @return Amount of cargo actually reassigned.
 */
template <VehicleCargoList::MoveToAction Tfrom, VehicleCargoList::MoveToAction Tto>
uint VehicleCargoList::Reassign(uint max_move)
{
	static_assert(Tfrom != MoveToAction::Transfer && Tto != MoveToAction::Transfer);
	static_assert(to_underlying(Tfrom) - to_underlying(Tto) == 1 || to_underlying(Tto) - to_underlying(Tfrom) == 1);
	max_move = std::min(this->action_counts[Tfrom], max_move);
	if constexpr (Tfrom == MoveToAction::Load && Tto == MoveToAction::Keep) {
		/* Count only the reserved prefix actually becoming loaded. Observe before
		 * changing counts; mixed Legacy/Journey cargo and partial loads stay exact. */
		uint skip = this->count - this->action_counts[MoveToAction::Load], remaining = max_move;
		for (auto it = this->packets.begin(); it != this->packets.end(); ++it) {
			CargoPacket *packet = *it;
			if (remaining == 0) break;
			if (skip >= packet->Count()) { skip -= packet->Count(); continue; }
			uint count = std::min<uint>(remaining, packet->Count() - skip);
			if (packet->GetPassengerBoardingFailure().Present()) {
				/* A partial physical load must not clear the still-reserved people's
				 * failures. Split only these nonempty failure states; ordinary cargo
				 * retains the original allocation-free reassignment. Pool exhaustion
				 * postpones this partial load, as CargoLoad::Preprocess already does. */
				if (skip != 0) {
					auto *reserved = packet->Split(packet->Count() - skip);
					if (reserved == nullptr) break;
					packet->SetPassengerBoardingFailure({});
					it = this->packets.insert(std::next(it), reserved); packet = reserved; skip = 0;
				}
				if (count != packet->Count()) {
					auto *reserved = packet->Split(packet->Count() - count);
					if (reserved == nullptr) break;
					this->packets.insert(std::next(it), reserved);
				}
				packet->SetPassengerBoardingFailure({});
			}
			RecordPassengerJourneyMovement(PassengerJourneyMovement::Board, *packet, count);
			remaining -= count; skip = 0;
		}
		max_move -= remaining;
	}
	this->action_counts[Tfrom] -= max_move;
	this->action_counts[Tto] += max_move;
	return max_move;
}

/**
 * Reassign cargo from MoveToAction::Deliver to MoveToAction::Transfer and take care of the next
 * station the cargo wants to visit.
 * @param max_move Maximum amount of cargo to reassign.
 * @return Amount of cargo actually reassigned.
 */
template <>
uint VehicleCargoList::Reassign<VehicleCargoList::MoveToAction::Deliver, VehicleCargoList::MoveToAction::Transfer>(uint max_move)
{
	max_move = std::min(this->action_counts[MoveToAction::Deliver], max_move);

	uint sum = 0;
	for (Iterator it(this->packets.begin()); sum < this->action_counts[MoveToAction::Transfer] + max_move;) {
		CargoPacket *cp = *it++;
		sum += cp->Count();
		if (sum <= this->action_counts[MoveToAction::Transfer]) continue;
		if (sum > this->action_counts[MoveToAction::Transfer] + max_move) {
			CargoPacket *cp_split = cp->Split(sum - this->action_counts[MoveToAction::Transfer] + max_move);
			sum -= cp_split->Count();
			it = this->packets.insert(it, cp_split);
			/* it points to the inserted value, which is just before the previous value of it.
			 * Increment it so that it points to the same element as it did before the insert.
			 */
			++it;
		}
		cp->next_hop = StationID::Invalid();
	}

	this->action_counts[MoveToAction::Deliver] -= max_move;
	this->action_counts[MoveToAction::Transfer] += max_move;
	return max_move;
}

/**
 * Returns reserved cargo to the station and removes it from the cache.
 * @param max_move Maximum amount of cargo to move.
 * @param dest Station the cargo is returned to.
 * @param next ID of the next station the cargo wants to go to.
 * @param current_tile Current tile the cargo handling is happening on.
 * @param cargo Cargo type, used to respect the per-cargo Journey routing switch.
 * @return Amount of cargo actually returned.
 */
uint VehicleCargoList::Return(uint max_move, StationCargoList *dest, StationID next, TileIndex current_tile, CargoType cargo)
{
	max_move = std::min(this->action_counts[MoveToAction::Load], max_move);
	this->PopCargo(CargoReturn(this, dest, max_move, next, current_tile, cargo));
	return max_move;
}

uint VehicleCargoList::ReturnReserved(StationCargoList *dest, StationID next, TileIndex current_tile, const std::function<bool(const CargoPacket &)> &select, CargoType cargo)
{
	uint remaining = this->ReservedCount();
	uint returned = 0;
	/* The last packet can straddle stored/reserved cargo after gradual loading.
	 * CargoReturn splits only its reserved portion and preserves payment/count caches. */
	for (auto it = this->packets.end(); remaining > 0 && it != this->packets.begin();) {
		CargoPacket *packet = *--it;
		uint count = std::min(remaining, static_cast<uint>(packet->Count()));
		remaining -= count;
		if (!select(*packet)) continue;
		/* At the pool limit leave this boundary reservation in place rather than
		 * returning its stored portion too. Mode invalidation still permits fallback. */
		if (count < packet->Count() && !CargoPacket::CanAllocateItem()) continue;
		CargoReturn action(this, dest, count, next, current_tile, cargo);
		if (action(packet)) it = this->packets.erase(it);
		returned += count;
	}
	this->AssertCountConsistency();
	return returned;
}

/**
 * Shifts cargo between two vehicles.
 * @param dest Other vehicle's cargo list.
 * @param max_move Maximum amount of cargo to be moved.
 * @return Amount of cargo actually moved.
 */
uint VehicleCargoList::Shift(uint max_move, VehicleCargoList *dest)
{
	max_move = std::min(this->count, max_move);
	this->PopCargo(CargoShift(this, dest, max_move));
	return max_move;
}

/**
 * Unloads cargo at the given station. Deliver or transfer, depending on the
 * ranges defined by designation_counts.
 * @param dest StationCargoList to add transferred cargo to.
 * @param max_move Maximum amount of cargo to move.
 * @param cargo The cargo type of the cargo.
 * @param payment Payment object to register payments in.
 * @param current_tile Current tile the cargo handling is happening on.
 * @return Amount of cargo actually unloaded.
 */
uint VehicleCargoList::Unload(uint max_move, StationCargoList *dest, CargoType cargo, CargoPayment *payment, TileIndex current_tile)
{
	uint moved = 0;
	if (this->action_counts[MoveToAction::Transfer] > 0) {
		uint move = std::min(this->action_counts[MoveToAction::Transfer], max_move);
		this->ShiftCargo(CargoTransfer(this, dest, move, current_tile));
		moved += move;
	}
	if (this->action_counts[MoveToAction::Transfer] == 0 && this->action_counts[MoveToAction::Deliver] > 0 && moved < max_move) {
		uint move = std::min(this->action_counts[MoveToAction::Deliver], max_move - moved);
		this->ShiftCargo(CargoDelivery(this, move, cargo, payment, current_tile));
		moved += move;
	}
	return moved;
}

/**
 * Truncates the cargo in this list to the given amount. It leaves the
 * first cargo entities and removes max_move from the back of the list.
 * @param max_move Maximum amount of entities to be removed from the list.
 * @return Amount of entities actually moved.
 */
uint VehicleCargoList::Truncate(uint max_move)
{
	max_move = std::min(this->count, max_move);
	if (max_move > this->ActionCount(MoveToAction::Keep)) this->KeepAll();
	this->PopCargo(CargoRemoval<VehicleCargoList>(this, max_move));
	return max_move;
}

/**
 * Routes packets with station "avoid" as next hop to a different place.
 * @param max_move Maximum amount of cargo to move.
 * @param dest List to prepend the cargo to.
 * @param avoid Station to exclude from routing and current next hop of packets to reroute.
 * @param avoid2 Additional station to exclude from routing.
 * @param ge GoodsEntry to get the routing info from.
 * @return The number of elements that got rerouted.
 */
uint VehicleCargoList::Reroute(uint max_move, VehicleCargoList *dest, StationID avoid, StationID avoid2, const GoodsEntry *ge)
{
	max_move = std::min(this->action_counts[MoveToAction::Transfer], max_move);
	this->ShiftCargoWithFrontInsert(VehicleCargoReroute(this, dest, max_move, avoid, avoid2, ge), [](CargoPacket *cp) { return true; });
	return max_move;
}

/**
 * Routes packets with station "avoid" as next hop to a different place, for a specific source station.
 * @param max_move Maximum amount of cargo to move.
 * @param dest List to prepend the cargo to.
 * @param source Source station.
 * @param avoid Station to exclude from routing and current next hop of packets to reroute.
 * @param avoid2 Additional station to exclude from routing.
 * @param ge GoodsEntry to get the routing info from.
 */
uint VehicleCargoList::RerouteFromSource(uint max_move, VehicleCargoList *dest, StationID source, StationID avoid, StationID avoid2, const GoodsEntry *ge)
{
	max_move = std::min(this->action_counts[MoveToAction::Transfer], max_move);
	this->ShiftCargoWithFrontInsert(VehicleCargoReroute(this, dest, max_move, avoid, avoid2, ge), [source](CargoPacket *cp) { return cp->GetFirstStation() == source; });
	return max_move;
}

/*
 *
 * Station cargo list implementation.
 *
 */

uint64_t CargoPacket::StationMergeHash() const
{
	uint64_t hash = 1469598103934665603ULL;
	auto add = [&](uint64_t value) { hash ^= value; hash *= 1099511628211ULL; };
	add(this->source_xy.base()); add(this->periods_in_transit); add(this->first_station.base()); add(this->source.id); add(to_underlying(this->source.type));
	add(this->passenger_destination.base()); add(to_underlying(this->passenger_routing_mode));
	const auto &r = this->passenger_journey;
	add(r.vehicle.base()); add(r.service.base()); add(r.boarding.base()); add(r.alighting.base()); add(r.boarding_order); add(r.alighting_order);
	add(r.departure.base()); add(r.arrival.base()); add(r.signature); add(to_underlying(r.kind)); add(r.generation);
	add(r.not_before.base()); add(r.planned_at.base()); add(r.estimated_wait); add(r.waiting_limit); add(r.timed_deferred);
	add(this->passenger_legacy_hop.base()); add(this->passenger_retry_station.base()); add(this->passenger_retry_after.base());
	add(this->passenger_interchange_ready.base()); add(this->passenger_arriving_service.base()); add(this->passenger_arriving_incarnation);
	add(to_underlying(this->passenger_transport)); add(to_underlying(this->passenger_interchange_transport)); add(this->passenger_schedule_used);
	add(this->passenger_boarding_failure.service.base()); add(this->passenger_boarding_failure.count); add(this->passenger_boarding_failure.since.base()); add(this->passenger_boarding_failure.incarnation);
	return hash;
}

StationCargoList::~StationCargoList()
{
	this->ClearWaitingIndex();
	if (this->passenger_station != StationID::Invalid()) _passenger_station_lists.erase({this->passenger_station, this->passenger_index_cargo});
	if (_passenger_station_lists.empty() && _passenger_waiting_lists.empty()) { _passenger_event_tick = INVALID_STATE_TICKS; _passenger_event_work = 0; }
}

void StationCargoList::ClearWaitingIndex()
{
	_passenger_waiting_lists.erase(this);
	for (const auto &[id, entry] : this->waiting_entries) _passenger_waiting_owners.erase(id.base());
	this->waiting_entries.clear(); this->merge_index.clear(); this->boarding_index.clear(); this->waiting_wake.clear();
	this->boarding_services.clear();
	this->section_boarding_index.clear();
	this->hyperpath_boarding_index.clear(); this->hyperpath_lookup = {};
	this->legacy_boarding_index.clear(); this->fallback_boarding_index.clear(); this->fallback_destinations.clear(); this->fallback_hops.clear();
	this->routing_events.clear(); this->routing_packets.clear(); this->legacy_journey_packets.clear();
	this->waiting_next_sequence = 0;
	this->waiting_index_valid = false;
	this->UpdatePassengerRoutingSchedule();
}

void StationCargoList::ForgetWaitingPacket(CargoPacketID id) const
{
	auto it = this->waiting_entries.find(id);
	if (it == this->waiting_entries.end()) return;
	const auto &entry = it->second;
	if (entry.routing_due != INVALID_STATE_TICKS) this->routing_events.erase({entry.routing_due, id});
	this->routing_packets.erase(id);
	this->legacy_journey_packets.erase(id);
	auto merge = this->merge_index.find(entry.hash);
	if (merge != this->merge_index.end()) { merge->second.erase({entry.space, id}); if (merge->second.empty()) this->merge_index.erase(merge); }
	if (entry.pending) this->waiting_wake.erase(entry.wake);
	this->RemoveBoardingEntry(id, entry);
	this->waiting_entries.erase(it); _passenger_waiting_owners.erase(id.base());
	this->UpdatePassengerRoutingSchedule();
}

StationCargoList::JourneyKey StationCargoList::BoardingJourneyKey(const CargoPacket &packet)
{
	const auto &ride = packet.GetPassengerJourney();
	VehicleID physical = IsPassengerRideIntent(ride.kind) ? VehicleID::Invalid() : ride.vehicle;
	return {ride.service, physical, ride.boarding, ride.boarding_order};
}

StationCargoList::HyperpathKey StationCargoList::BoardingHyperpathKey(const JourneyRideKey &key)
{
	const auto &ride = std::get<0>(key); const auto &failure = std::get<3>(key);
	bool adjusted = failure.count >= PassengerBoardingFailure::THRESHOLD;
	return {ride.boarding, ride.signature, ride.generation, std::get<1>(key), std::get<2>(key),
			adjusted ? failure.service : OrderListID::Invalid(), adjusted ? failure.incarnation : 0};
}

StateTicks StationCargoList::BoardingHyperpathExpiry(const JourneyRideKey &key)
{
	const auto &failure = std::get<3>(key); int64_t hold = failure.Hold(std::get<0>(key).waiting_limit);
	return StateTicks{failure.since.base() > INT64_MAX - hold ? INT64_MAX : failure.since.base() + hold};
}

void StationCargoList::AddBoardingEntry(CargoPacketID id, WaitingEntry &entry) const
{
	++this->boarding_index_updates;
	entry.journey_group.reset();
	BoardingOrder order{entry.key, entry.sequence, id};
	if (!entry.journey) this->legacy_boarding_index[{entry.fallback.has_value(), entry.key}].insert(order);
	if (entry.pending) return;
	if (entry.journey) {
		const auto &packet = *CargoPacket::Get(id);
		auto [group, inserted] = this->boarding_index.try_emplace(BoardingJourneyKey(packet));
		if (inserted) ++this->boarding_services[entry.service];
		auto [ride, new_ride] = group->second.try_emplace(JourneyRideKey{packet.GetPassengerJourney(), packet.GetPassengerArrivingService(), packet.GetPassengerArrivingIncarnation(), packet.GetPassengerBoardingFailure()});
		ride->second.insert(order); entry.journey_group = std::pair(group, ride);
		if (new_ride && packet.GetPassengerJourney().kind == PassengerRideKind::EstimatedServiceIntent)
			this->section_boarding_index[PassengerPrecomputedTable::RawSectionBoardingKey(packet.GetPassengerJourney())].emplace(ride->first, &ride->second);
		if (packet.GetPassengerJourney().kind == PassengerRideKind::HyperpathIntent) {
			const StationID alighting = packet.GetPassengerJourney().alighting;
			auto [policy_entry, new_group] = this->hyperpath_boarding_index[alighting].try_emplace(BoardingHyperpathKey(ride->first));
			auto &policy = policy_entry->second;
			if (new_group && this->hyperpath_lookup.epoch != UINT64_MAX) {
				/* A lookup that was never built indexes every group on its first refresh. */
				policy.lookup_slot = HYPERPATH_PENDING; this->hyperpath_lookup.pending.push_back({alighting, policy_entry});
			}
			policy.packets.insert(order);
			if (std::get<3>(ride->first).count >= PassengerBoardingFailure::THRESHOLD) policy.expiries.insert(BoardingHyperpathExpiry(ride->first));
		}
	}
	else if (entry.fallback) {
		auto [group, inserted] = this->fallback_boarding_index.try_emplace(*entry.fallback);
		group->second.insert(order);
		if (inserted) {
			this->fallback_destinations[std::get<0>(*entry.fallback)].insert(*entry.fallback);
			this->fallback_hops[std::get<1>(*entry.fallback)].insert(*entry.fallback);
		}
	}
}

void StationCargoList::RemoveBoardingEntry(CargoPacketID id, const WaitingEntry &entry) const
{
	++this->boarding_index_updates;
	BoardingOrder order{entry.key, entry.sequence, id};
	if (!entry.journey) {
		auto group = this->legacy_boarding_index.find({entry.fallback.has_value(), entry.key});
		if (group != this->legacy_boarding_index.end()) { group->second.erase(order); if (group->second.empty()) this->legacy_boarding_index.erase(group); }
	}
	if (entry.pending) return;
	if (entry.journey) {
		auto [group, ride] = *entry.journey_group;
		if (std::get<0>(ride->first).kind == PassengerRideKind::HyperpathIntent) {
			auto destination = this->hyperpath_boarding_index.find(std::get<0>(ride->first).alighting);
			auto policy = destination->second.find(BoardingHyperpathKey(ride->first));
			policy->second.packets.erase(order);
			if (std::get<3>(ride->first).count >= PassengerBoardingFailure::THRESHOLD) {
				auto expiry = policy->second.expiries.find(BoardingHyperpathExpiry(ride->first));
				assert(expiry != policy->second.expiries.end()); policy->second.expiries.erase(expiry);
			}
			if (policy->second.packets.empty()) { this->UnindexHyperpathGroup(policy); destination->second.erase(policy); }
			if (destination->second.empty()) this->hyperpath_boarding_index.erase(destination);
		}
		ride->second.erase(order);
		if (ride->second.empty()) {
			const auto &intent = std::get<0>(ride->first);
			if (intent.kind == PassengerRideKind::EstimatedServiceIntent) {
				auto section = this->section_boarding_index.find(PassengerPrecomputedTable::RawSectionBoardingKey(intent));
				section->second.erase(ride->first); if (section->second.empty()) this->section_boarding_index.erase(section);
			}
			group->second.erase(ride);
		}
		if (group->second.empty()) {
			this->boarding_index.erase(group);
			auto service = this->boarding_services.find(entry.service); if (--service->second == 0) this->boarding_services.erase(service);
		}
	} else if (entry.fallback) {
		auto group = this->fallback_boarding_index.find(*entry.fallback);
		if (group == this->fallback_boarding_index.end()) return;
		group->second.erase(order);
		if (!group->second.empty()) return;
		this->fallback_boarding_index.erase(group);
		auto remove = [&](auto &index, StationID key) {
			auto bucket = index.find(key); bucket->second.erase(*entry.fallback);
			if (bucket->second.empty()) index.erase(bucket);
		};
		remove(this->fallback_destinations, std::get<0>(*entry.fallback)); remove(this->fallback_hops, std::get<1>(*entry.fallback));
	}
}

void StationCargoList::IndexPacket(CargoPacket *packet, StationID key, StationCargoPacketList::iterator position, std::optional<uint64_t> sequence) const
{
	const bool journey = packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey;
	OrderListID service = journey ? packet->GetPassengerJourney().service : OrderListID::Invalid();
	/* Index the saved raw identity. A changing table's canonical pattern key
	 * cannot live in a derived index: a joining peer would rebuild it earlier
	 * than a server's pending generation sweep and see different candidates. */
	StateTicks ready = journey ? packet->GetPassengerJourney().not_before : StateTicks{};
	if (Station::IsValidID(packet->GetPassengerDestination()) && packet->GetPassengerArrivingService() != OrderListID::Invalid()) ready = std::max(ready, packet->GetPassengerInterchangeReady());
	bool pending = ready > _state_ticks;
	const uint64_t rank = sequence ? *sequence : this->waiting_next_sequence++;
	WaitingEntry entry{key, position, packet->StationMergeHash(), service, static_cast<uint16_t>(CargoPacket::MAX_COUNT - packet->Count()), pending, rank, {}, packet->GetPassengerRoutingEvent(), journey, {}, {}};
	if (!journey && Station::IsValidID(packet->GetPassengerDestination())) entry.fallback = FallbackKey{packet->GetPassengerDestination(), packet->GetPassengerLegacyHop(), packet->GetPassengerArrivingService(), packet->GetPassengerArrivingIncarnation()};
	if (entry.routing_due != INVALID_STATE_TICKS) this->routing_events.emplace(entry.routing_due, packet->index);
	/* Membership depends only on packet state, never table/cache residency.
	 * Legacy mode visits unresolved OD only; Hyperpath also migrates old intents
	 * under the same saved national/station work allowances. */
	if (!journey && Station::IsValidID(packet->GetPassengerDestination())) this->routing_packets.insert(packet->index);
	if (journey && packet->GetPassengerJourney().kind != PassengerRideKind::HyperpathIntent && Station::IsValidID(packet->GetPassengerDestination())) this->legacy_journey_packets.insert(packet->index);
	if (pending) entry.wake = this->waiting_wake.emplace(ready, packet->index);
	this->AddBoardingEntry(packet->index, entry);
	this->merge_index[entry.hash].emplace(entry.space, packet->index);
	this->waiting_entries.emplace(packet->index, entry);
	_passenger_waiting_owners[packet->index.base()] = const_cast<StationCargoList *>(this);
	this->UpdatePassengerRoutingSchedule();
}

void StationCargoList::RefreshWaitingPacket(const CargoPacket &packet) const
{
	auto it = this->waiting_entries.find(packet.index); if (it == this->waiting_entries.end()) return;
	auto &entry = it->second;
	const auto hash = packet.StationMergeHash(); const uint16_t space = CargoPacket::MAX_COUNT - packet.Count();
	if (entry.hash != hash || entry.space != space) {
		auto merge = this->merge_index.find(entry.hash); merge->second.erase({entry.space, packet.index});
		if (merge->second.empty()) this->merge_index.erase(merge);
		entry.hash = hash; entry.space = space; this->merge_index[hash].emplace(space, packet.index);
	}
	if (entry.routing_due != packet.GetPassengerRoutingEvent()) {
		if (entry.routing_due != INVALID_STATE_TICKS) this->routing_events.erase({entry.routing_due, packet.index});
		entry.routing_due = packet.GetPassengerRoutingEvent();
		if (entry.routing_due != INVALID_STATE_TICKS) this->routing_events.emplace(entry.routing_due, packet.index);
	}
	const bool journey = packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey;
	const auto service = journey ? packet.GetPassengerJourney().service : OrderListID::Invalid();
	std::optional<FallbackKey> fallback;
	if (!journey && Station::IsValidID(packet.GetPassengerDestination())) fallback = FallbackKey{packet.GetPassengerDestination(), packet.GetPassengerLegacyHop(), packet.GetPassengerArrivingService(), packet.GetPassengerArrivingIncarnation()};
	if (fallback.has_value() != (!entry.journey && entry.fallback.has_value())) {
		if (fallback) this->routing_packets.insert(packet.index); else this->routing_packets.erase(packet.index);
	}
	if (journey && packet.GetPassengerJourney().kind != PassengerRideKind::HyperpathIntent && Station::IsValidID(packet.GetPassengerDestination())) this->legacy_journey_packets.insert(packet.index);
	else this->legacy_journey_packets.erase(packet.index);
	StateTicks ready = journey ? packet.GetPassengerJourney().not_before : StateTicks{};
	if (Station::IsValidID(packet.GetPassengerDestination()) && packet.GetPassengerArrivingService() != OrderListID::Invalid()) ready = std::max(ready, packet.GetPassengerInterchangeReady());
	const bool pending = ready > _state_ticks;
	bool same_ride = !journey || !entry.journey || entry.pending ||
			(entry.journey_group->first->first == BoardingJourneyKey(packet) && entry.journey_group->second->first == JourneyRideKey{packet.GetPassengerJourney(), packet.GetPassengerArrivingService(), packet.GetPassengerArrivingIncarnation(), packet.GetPassengerBoardingFailure()});
	if (entry.journey != journey || entry.service != service || entry.fallback != fallback || entry.pending != pending || !same_ride || (pending && entry.wake->first != ready)) {
		this->RemoveBoardingEntry(packet.index, entry);
		if (entry.pending) this->waiting_wake.erase(entry.wake);
		entry.journey = journey; entry.service = service; entry.fallback = fallback; entry.pending = pending;
		if (pending) entry.wake = this->waiting_wake.emplace(ready, packet.index);
		this->AddBoardingEntry(packet.index, entry);
	}
	this->UpdatePassengerRoutingSchedule();
}

template <class Taction>
uint StationCargoList::ShiftIndexedJourneyCargo(Taction action, std::span<const StationID> next, const Vehicle *vehicle, PassengerJourneyLoadingContext &loading)
{
	const uint maximum = action.MaxMove();
	if (maximum == 0) return 0;
	loading.EnableWaitingEvents();
	this->VisitBoardingCandidates(next, vehicle, loading, [&](CargoPacketID id) {
		if (action.MaxMove() == 0) return false;
		auto entry = this->waiting_entries.find(id); if (entry == this->waiting_entries.end()) return true;
		const StationID key = entry->second.key;
		CargoPacket *packet = CargoPacket::Get(id); ++this->boarding_candidates_inspected;
		if (!loading.CanBoard(*packet, vehicle)) {
			/* This is an actual loading action, unlike the read-only HasCargoFor
			 * predicate. Queue a stale intent for the next bounded routing slice. */
			if (packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey &&
					!GetPassengerServiceDatabase().IsCurrent(packet->GetPassengerJourney())) PassengerWaitingRoutingChanged(*packet);
			return true;
		}
		const auto position = entry->second.position;
		const auto sequence = entry->second.sequence;
		/* Resolve only the cohort selected by the existing boarding indices.
		 * Detach before changing it; other cursors still refer to the old group. */
		this->ForgetWaitingPacket(id);
		loading.ResolveWaiting(*packet, vehicle);
		const auto intent = packet->GetPassengerJourney(); const auto legacy = packet->GetPassengerLegacyHop();
		const auto failure = packet->GetPassengerBoardingFailure();
		const auto via = packet->GetPassengerVia(); const auto previous_transport = packet->GetPassengerTransport();
		/* Detach before binding/splitting: the physical packet can move to a
		 * vehicle, merge there or be destroyed by the existing action. */
		loading.PrepareBoard(*packet, vehicle);
		const uint before = action.MaxMove();
		if (action(packet)) {
			StationCargoPacketMap::Map &queues = this->packets;
			auto bucket = queues.find(key); bucket->second.erase(position);
			if (bucket->second.empty()) queues.erase(bucket);
		} else {
			if (IsPassengerRideIntent(intent.kind)) packet->SetPassengerJourney(intent, legacy);
			else if (packet->GetPassengerRoutingMode() != PassengerRoutingMode::Journey) packet->SetPassengerVia(via);
			packet->SetPassengerBoardingFailure(failure);
			packet->SetPassengerTransport(previous_transport); this->IndexPacket(packet, key, position, sequence);
			if (action.MaxMove() != before) this->passenger_boarding_last = key;
			return false;
		}
		if (action.MaxMove() != before) this->passenger_boarding_last = key;
		return action.MaxMove() != 0;
	});
	return maximum - action.MaxMove();
}

void StationCargoList::EnsureWaitingIndex() const
{
	if (this->waiting_index_valid) return;
	_passenger_waiting_lists.insert(const_cast<StationCargoList *>(this));
	for (auto &[key, list] : const_cast<StationCargoPacketMap &>(this->packets)) {
		for (auto it = list.begin(); it != list.end(); ++it) this->IndexPacket(*it, key, it);
	}
	this->waiting_index_valid = true;
	this->UpdatePassengerRoutingSchedule();
}

void StationCargoList::RemoveFromCache(const CargoPacket *packet, uint count)
{
	/* Whole moves retain the original ID; partial moves have a fresh ID and
	 * leave the original station entry in place. CargoPacket::Split refreshes it. */
	if (count == packet->Count()) this->ForgetWaitingPacket(packet->index);
	this->Parent::RemoveFromCache(packet, count);
}

void StationCargoList::UnindexHyperpathGroup(HyperpathGroups::iterator group) const
{
	auto &lookup = this->hyperpath_lookup; auto &waiting = group->second;
	auto drop = [&](std::vector<HyperpathGroupRef> &refs) {
		auto found = std::find_if(refs.begin(), refs.end(), [&](const HyperpathGroupRef &ref) { return ref.group == group; });
		assert(found != refs.end());
		*found = refs.back(); refs.pop_back();
	};
	if (waiting.lookup_slot == PassengerServiceDatabase::HYPERPATH_LOOKUP_ALWAYS) drop(lookup.always);
	else if (waiting.lookup_slot == HYPERPATH_PENDING) drop(lookup.pending);
	else if (waiting.lookup_slot == 0 || waiting.lookup_slot == 1) {
		auto &table = lookup.occurrences[waiting.lookup_slot];
		for (uint32_t occurrence : waiting.lookup_occurrences) {
			auto refs = table.find(occurrence); assert(refs != table.end());
			drop(refs->second);
			if (refs->second.empty()) table.erase(refs);
		}
	}
	waiting.lookup_slot = HYPERPATH_UNINDEXED; waiting.lookup_occurrences.clear();
}

void StationCargoList::IndexHyperpathGroup(StationID destination, HyperpathGroups::iterator group) const
{
	auto &lookup = this->hyperpath_lookup; auto &waiting = group->second;
	assert(waiting.lookup_slot == HYPERPATH_UNINDEXED);
	const auto &key = group->first;
	if (std::get<5>(key) != OrderListID::Invalid()) {
		/* A failed-carrier policy may admit occurrences outside every stored policy. */
		waiting.lookup_slot = PassengerServiceDatabase::HYPERPATH_LOOKUP_ALWAYS; lookup.always.push_back({destination, group}); return;
	}
	const auto *packet = CargoPacket::Get(std::get<2>(*waiting.packets.begin()));
	waiting.lookup_slot = static_cast<int8_t>(GetPassengerServiceDatabase().HyperpathLookupCandidates(lookup.cargo, packet->GetPassengerJourney(),
			std::get<3>(key), std::get<4>(key), lookup.station, waiting.lookup_occurrences));
	if (waiting.lookup_slot == PassengerServiceDatabase::HYPERPATH_LOOKUP_ALWAYS) lookup.always.push_back({destination, group});
	else if (waiting.lookup_slot == 0 || waiting.lookup_slot == 1) {
		for (uint32_t occurrence : waiting.lookup_occurrences) lookup.occurrences[waiting.lookup_slot][occurrence].push_back({destination, group});
	}
}

void StationCargoList::RefreshHyperpathLookup(StationID station) const
{
	auto &lookup = this->hyperpath_lookup;
	const auto &db = GetPassengerServiceDatabase();
	const uint64_t changes = db.HyperpathDestinationChanges();
	auto reindex = [&](StationID destination, HyperpathGroups::iterator group) {
		if (group->second.lookup_slot != HYPERPATH_UNINDEXED) this->UnindexHyperpathGroup(group);
		this->IndexHyperpathGroup(destination, group);
		++_passenger_event_diagnostic.hyperpath_lookup_indexed_groups;
	};
	if (lookup.epoch != db.HyperpathLookupEpoch() || lookup.station != station || lookup.cargo != this->passenger_index_cargo ||
			changes < lookup.changes || changes - lookup.changes > PassengerServiceDatabase::HYPERPATH_DESTINATION_LOG) {
		/* Keep bucket and vector storage: rebuilds follow every table generation. */
		for (auto &table : lookup.occurrences) table.clear();
		lookup.always.clear(); lookup.pending.clear();
		lookup.epoch = db.HyperpathLookupEpoch(); lookup.station = station; lookup.cargo = this->passenger_index_cargo; lookup.changes = changes;
		++_passenger_event_diagnostic.hyperpath_lookup_rebuilds;
		for (auto &[destination, groups] : this->hyperpath_boarding_index) {
			for (auto group = groups.begin(); group != groups.end(); ++group) {
				group->second.lookup_slot = HYPERPATH_UNINDEXED; group->second.lookup_occurrences.clear();
				reindex(destination, group);
			}
		}
		return;
	}
	if (lookup.changes != changes) {
		lookup.changed.clear();
		for (uint64_t change = lookup.changes; change != changes; ++change) lookup.changed.push_back(db.HyperpathChangedDestination(change));
		std::sort(lookup.changed.begin(), lookup.changed.end()); lookup.changed.erase(std::unique(lookup.changed.begin(), lookup.changed.end()), lookup.changed.end());
		for (StationID destination : lookup.changed) {
			auto groups = this->hyperpath_boarding_index.find(destination);
			if (groups == this->hyperpath_boarding_index.end()) continue;
			for (auto group = groups->second.begin(); group != groups->second.end(); ++group) reindex(destination, group);
		}
		lookup.changes = changes;
	}
	while (!lookup.pending.empty()) {
		/* Unindex takes the group out of pending, as for removal. */
		const auto ref = lookup.pending.back();
		reindex(ref.destination, ref.group);
	}
}

template <typename Visitor>
bool StationCargoList::VisitBoardingCandidates(std::span<const StationID> next, const Vehicle *vehicle, PassengerJourneyLoadingContext &loading, Visitor &&visitor, bool defer_hyperpath) const
{
	this->EnsureWaitingIndex();
	while (!this->waiting_wake.empty() && this->waiting_wake.begin()->first <= _state_ticks) {
		auto wake = this->waiting_wake.begin(); auto it = this->waiting_entries.find(wake->second);
		if (it != this->waiting_entries.end()) { it->second.pending = false; this->AddBoardingEntry(wake->second, it->second); }
		this->waiting_wake.erase(wake);
	}
	using Position = std::set<BoardingOrder>::const_iterator;
	struct Cursor { BoardingOrder order; Position next, end; std::optional<StationID> ceiling; bool validate; uint8_t hyperpath; size_t packets; };
	/* One cursor per eligible cohort/range, never one element per packet.
	 * Round-robin primary buckets and FIFO inside each bucket survive index
	 * reconstruction. Packet IDs are only unique handles, never priorities. */
	const StationID last = this->passenger_boarding_last;
	/* Identity membership only: pointer values never determine visit order. A
	 * cohort can be reached through its own service and several section keys. */
	std::set<const std::set<BoardingOrder> *> selected_groups;
	auto rank = [last](const BoardingOrder &order) { return std::pair(last != StationID::Invalid() && std::get<0>(order) <= last, order); };
	auto later = [&](const Cursor &a, const Cursor &b) { return rank(a.order) > rank(b.order); };
	std::priority_queue<Cursor, std::vector<Cursor>, decltype(later)> candidates(later);
	auto cursor = [](Position begin, Position end, std::optional<StationID> ceiling, bool validate, uint8_t hyperpath = 0, size_t packets = 0) {
		auto next = std::next(begin);
		if (next != end && ceiling && std::get<0>(*next) > *ceiling) next = end;
		return Cursor{*begin, next, end, ceiling, validate, hyperpath, packets};
	};
	auto add_group = [&](const std::set<BoardingOrder> &group, bool validate = false, uint8_t hyperpath = 0) {
		if (group.empty() || !selected_groups.insert(&group).second) return;
		if (hyperpath == 0) loading.ObserveWaitingGroup(*CargoPacket::Get(std::get<2>(*group.begin())), vehicle, this->passenger_index_cargo, group.size());
		++this->boarding_groups_inspected;
		auto split = last == StationID::Invalid() ? group.begin() : group.upper_bound({last, UINT64_MAX, CargoPacketID::Invalid()});
		if (split != group.end()) candidates.push(cursor(split, group.end(), {}, validate, hyperpath, group.size()));
		/* A real node cannot be an end sentinel: the earlier circular range
		 * may erase that node before this range is visited. */
		if (group.begin() != split) candidates.push(cursor(group.begin(), group.end(), last, validate, hyperpath, group.size()));
	};
	auto add_service = [&](OrderListID id) {
		if (vehicle == nullptr) return;
		const auto *front = vehicle->First();
		for (VehicleID physical : {VehicleID::Invalid(), front->index}) {
			auto group = this->boarding_index.find({id, physical, front->last_station_visited, front->cur_real_order_index});
			if (group == this->boarding_index.end()) continue;
			for (const auto &[ride, packets] : group->second) {
				if (front->orders != nullptr && std::get<1>(ride) == front->orders->index && std::get<2>(ride) == front->orders->GetCargoDistRouteGeneration()) continue;
				add_group(packets, true);
			}
		}
	};
	const auto &db = GetPassengerServiceDatabase();
	OrderListID service = vehicle != nullptr && vehicle->First()->orders != nullptr ? vehicle->First()->orders->index : OrderListID::Invalid();
	const auto keys = db.PrecomputedBoardingKeys(this->passenger_index_cargo, service, vehicle == nullptr ? INVALID_VEH_ORDER_ID : vehicle->First()->cur_real_order_index);
	if (vehicle != nullptr && service != OrderListID::Invalid()) {
		/* Resolve occurrence identities once per arriving consist/cargo, shared
		 * across its loading passes. Enumeration only checks sorted membership.
		 * Concrete rides are deferred until this cohort reaches the FIFO front. */
		const bool scan = !defer_hyperpath || (IsPassengerJourneyRoutingEnabled(this->passenger_index_cargo) &&
				vehicle->First()->current_order.GetCargoLoadType(this->passenger_index_cargo) != OrderLoadType::NoLoad);
		const auto occurrences = defer_hyperpath && scan && !this->hyperpath_boarding_index.empty() ? loading.HyperpathBoardingOccurrences(vehicle, this->passenger_index_cargo) :
				std::array{PassengerPrecomputedTable::INVALID, PassengerPrecomputedTable::INVALID};
		/* Identical decisions in every strategy: base membership (and the plan of a retired intent)
		 * only uses stored policies of the current/retained generations. Masked skips destinations
		 * whose masks lack this departure; Indexed looks up exactly the groups whose stored policies
		 * contain it. A live failure may use a local policy and is always checked. The full
		 * predicate still decides every group that is considered. */
		using Mode = PassengerServiceDatabase::HyperpathFilterMode;
		const bool walk = scan && !this->hyperpath_boarding_index.empty();
		const auto mode = db.GetHyperpathBoardingFilterMode();
		if (walk && mode == Mode::Alternate && loading.hyperpath_filter_choice < 0) {
			static uint64_t contexts = 0; // Local diagnostic sequence only: every mode makes the same decisions.
			uint64_t z = (contexts++ + 1) * 0x9E3779B97F4A7C15ULL; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
			loading.hyperpath_filter_choice = static_cast<int8_t>((z ^ (z >> 31)) & 1);
		}
		enum Strategy : uint8_t { COMPLETE, MASKED, INDEXED };
		Strategy strategy = COMPLETE;
		if (mode == Mode::Index || (mode == Mode::Alternate && loading.hyperpath_filter_choice > 0)) strategy = INDEXED;
		else if ((mode == Mode::On || mode == Mode::Alternate) && db.HyperpathBoardingFilterAvailable(this->passenger_index_cargo)) strategy = MASKED;
		const bool validate = mode == Mode::Validate;
		const StationID station = walk ? vehicle->First()->last_station_visited : StationID::Invalid();
		const auto boarding = strategy == COMPLETE && !validate ? std::array{PassengerPrecomputedTable::INVALID, PassengerPrecomputedTable::INVALID} : defer_hyperpath ? occurrences :
				db.HyperpathBoardingOccurrences(this->passenger_index_cargo, station, service, vehicle->First()->cur_real_order_index);
		const auto walk_started = walk ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		uint64_t walk_groups = 0;
		std::set<const HyperpathWaiting *> admitted_groups; // Validate only.
		auto consider = [&](const HyperpathKey &key, const HyperpathWaiting &policy, bool adjusted) {
			++_passenger_event_diagnostic.hyperpath_groups_checked;
			bool base = policy.expiries.empty() || *policy.expiries.begin() <= _state_ticks;
			const auto *packet = CargoPacket::Get(std::get<2>(*policy.packets.begin()));
			assert(packet->GetPassengerArrivingService() == std::get<3>(key));
			bool admitted;
			if (!defer_hyperpath) {
				admitted = loading.CanUseHyperpathPolicy(*packet, vehicle, this->passenger_index_cargo, base, adjusted);
				if (admitted) add_group(policy.packets);
			} else {
				admitted = loading.MayUseHyperpathPolicy(*packet, vehicle, this->passenger_index_cargo, occurrences, base, adjusted);
				if (admitted) add_group(policy.packets, false, (base ? 1 : 0) | (adjusted ? 2 : 0));
			}
			if (admitted) {
				++_passenger_event_diagnostic.hyperpath_groups_admitted;
				if (validate) admitted_groups.insert(&policy);
			}
		};
		if (walk && strategy == INDEXED) {
			this->RefreshHyperpathLookup(station);
			const auto &lookup = this->hyperpath_lookup;
			auto consider_all = [&](const std::vector<HyperpathGroupRef> &refs) {
				for (const auto &ref : refs) {
					++walk_groups;
					const auto &policy = ref.group->second;
					consider(ref.group->first, policy, !policy.expiries.empty() && *policy.expiries.rbegin() > _state_ticks);
				}
			};
			for (size_t slot = 0; slot < 2; ++slot) {
				auto refs = lookup.occurrences[slot].find(boarding[slot]);
				if (refs != lookup.occurrences[slot].end()) consider_all(refs->second);
			}
			consider_all(lookup.always);
		} else if (walk) for (const auto &[destination, groups] : this->hyperpath_boarding_index) {
			std::array<int8_t, 2> relevant{-1, -1}; // Lazily evaluated: [0] base state, [1] any state.
			for (const auto &[key, policy] : groups) {
				++walk_groups;
				bool adjusted = !policy.expiries.empty() && *policy.expiries.rbegin() > _state_ticks;
				if (strategy == MASKED && !adjusted) {
					/* The group key holds the shared arriving carrier: no packet access to skip. */
					const bool any_state = std::get<3>(key) != OrderListID::Invalid();
					if (relevant[any_state] < 0) relevant[any_state] = db.MayBeHyperpathCandidate(this->passenger_index_cargo, boarding, destination, any_state);
					if (relevant[any_state] == 0) { ++_passenger_event_diagnostic.hyperpath_groups_skipped; continue; }
				}
				consider(key, policy, adjusted);
			}
		}
		if (walk && validate) {
			/* Every group the complete walk admitted must be an Index candidate. */
			this->RefreshHyperpathLookup(station);
			const auto &lookup = this->hyperpath_lookup;
			std::set<const HyperpathWaiting *> candidates;
			for (size_t slot = 0; slot < 2; ++slot) {
				auto refs = lookup.occurrences[slot].find(boarding[slot]);
				if (refs != lookup.occurrences[slot].end()) for (const auto &ref : refs->second) candidates.insert(&ref.group->second);
			}
			for (const auto &ref : lookup.always) candidates.insert(&ref.group->second);
			for (const auto *group : admitted_groups) {
				++_passenger_event_diagnostic.hyperpath_index_validated;
				if (!candidates.contains(group)) ++_passenger_event_diagnostic.hyperpath_index_violations;
			}
		}
		if (walk) {
			++_passenger_event_diagnostic.hyperpath_walks[strategy];
			_passenger_event_diagnostic.hyperpath_walk_groups[strategy] += walk_groups;
			_passenger_event_diagnostic.hyperpath_walk_nanoseconds[strategy] += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - walk_started).count();
		}
	}
	auto merge_keys = [](auto first, auto second, auto &&accept) {
		size_t a = 0, b = 0;
		while (a < first.size() || b < second.size()) {
			if (b == second.size() || (a < first.size() && first[a] < second[b])) accept(first[a++]);
			else if (a == first.size() || second[b] < first[a]) accept(second[b++]);
			else { accept(first[a]); ++a; ++b; }
		}
	};
	auto own_service = [&](OrderListID id) { return id == service || std::binary_search(keys.services[0].begin(), keys.services[0].end(), id) || std::binary_search(keys.services[1].begin(), keys.services[1].end(), id); };
	if (this->boarding_services.size() < 1 + keys.services[0].size() + keys.services[1].size()) {
		for (const auto &[id, groups] : this->boarding_services) {
			if (own_service(id)) add_service(id);
		}
	} else {
		add_service(service);
		merge_keys(keys.services[0], keys.services[1], [&](OrderListID id) { if (id != service) add_service(id); });
	}
	auto add_section = [&](uint64_t key) {
		auto group = this->section_boarding_index.find(key);
		if (group == this->section_boarding_index.end() || vehicle == nullptr) return;
		const auto *front = vehicle->First();
		for (const auto &[ride, queue] : group->second) {
			if (std::get<0>(ride).boarding != front->last_station_visited ||
					(front->orders != nullptr && std::get<1>(ride) == service && std::get<2>(ride) == front->orders->GetCargoDistRouteGeneration())) continue;
			add_group(*queue, true);
		}
	};
	if (this->section_boarding_index.size() < keys.sections[0].size() + keys.sections[1].size()) {
		for (const auto &[key, groups] : this->section_boarding_index) {
			if (std::binary_search(keys.sections[0].begin(), keys.sections[0].end(), key) || std::binary_search(keys.sections[1].begin(), keys.sections[1].end(), key)) add_section(key);
		}
	} else {
		merge_keys(keys.sections[0], keys.sections[1], add_section);
	}
	const bool routing = vehicle != nullptr && IsPassengerJourneyRoutingEnabled(this->passenger_index_cargo);
	std::set<StationID> legacy_keys(next.begin(), next.end()); legacy_keys.insert(StationID::Invalid());
	for (StationID key : legacy_keys) for (bool known : {false, true}) {
		if (known && routing) continue;
		auto group = this->legacy_boarding_index.find({known, key});
		if (group != this->legacy_boarding_index.end()) add_group(group->second);
	}
	if (routing && !this->fallback_boarding_index.empty()) {
		std::set<FallbackKey> selected;
		auto add_fallback = [&](const FallbackKey &key) {
			if (std::get<2>(key) == service && vehicle->First()->orders != nullptr && std::get<3>(key) == vehicle->First()->orders->GetCargoDistRouteGeneration()) return;
			if (!selected.insert(key).second) return;
			auto group = this->fallback_boarding_index.find(key);
			if (group != this->fallback_boarding_index.end()) add_group(group->second, true);
		};
		auto add_destination = [&](StationID destination) {
			auto bucket = this->fallback_destinations.find(destination);
			if (bucket != this->fallback_destinations.end()) for (const auto &key : bucket->second) add_fallback(key);
		};
		for (StationID stop : loading.BoardingStops(vehicle, this->passenger_index_cargo)) {
			add_destination(stop);
			auto hops = this->fallback_hops.find(stop);
			if (hops != this->fallback_hops.end()) for (const auto &key : hops->second) add_fallback(key);
			auto progress = [&](StationID destination) {
				const auto origin = db.PrecomputedRouteCost(this->passenger_index_cargo, vehicle->First()->last_station_visited, destination);
				const auto suffix = db.PrecomputedRouteCost(this->passenger_index_cargo, stop, destination);
				if (suffix && *suffix < origin.value_or(INT64_MAX)) add_destination(destination);
			};
			for (bool retained : {false, true}) {
				auto destinations = db.PrecomputedReachableDestinations(this->passenger_index_cargo, stop, retained);
				if (this->fallback_destinations.size() < destinations[0].size() + destinations[1].size()) {
					for (const auto &[destination, groups] : this->fallback_destinations) {
						if (std::binary_search(destinations[0].begin(), destinations[0].end(), destination) ||
								std::binary_search(destinations[1].begin(), destinations[1].end(), destination)) progress(destination);
					}
				} else for (auto span : destinations) for (StationID destination : span) progress(destination);
			}
		}
	}
	while (!candidates.empty()) {
		auto current = candidates.top(); candidates.pop();
		if (current.hyperpath != 0) {
			const auto *packet = CargoPacket::Get(std::get<2>(current.order));
			/* Retain the original group predicate before exposing a conservative
			 * candidate to any visitor that could queue events or mutate cargo. */
			if (!loading.CanUseHyperpathPolicy(*packet, vehicle, this->passenger_index_cargo, current.hyperpath & 1, current.hyperpath & 2)) continue;
			loading.ObserveWaitingGroup(*packet, vehicle, this->passenger_index_cargo, current.packets);
		}
		/* Validate only when this FIFO cohort reaches the front. A rejected
		 * cohort is skipped as a range, without inspecting its other packets. */
		if (current.validate && !loading.CanBoard(*CargoPacket::Get(std::get<2>(current.order)), vehicle, this->passenger_index_cargo)) continue;
		/* Advance before the action can erase/reinsert its current packet or
		 * delete its empty cohort. Other packet positions remain stable. */
		if (current.next != current.end) candidates.push(cursor(current.next, current.end, current.ceiling, false));
		if (!visitor(std::get<2>(current.order))) return false;
	}
	return true;
}

/**
 * Appends the given cargo packet to the range of packets with the same next station
 * @warning After appending this packet may not exist anymore!
 * @note Do not use the cargo packet anymore after it has been appended to this CargoList!
 * @param next the next hop
 * @param cp the cargo packet to add
 * @pre cp != nullptr
 */
void StationCargoList::Append(CargoPacket *cp, StationID next)
{
	dbg_assert(cp != nullptr);
	/* Appending is a new waiting event, including returned reservations. Never
	 * assign time zero here: new low/recycled pool IDs must not overtake old work. */
	cp->SchedulePassengerRouting(_state_ticks);
	if (cp->GetPassengerRoutingMode() == PassengerRoutingMode::Journey) next = StationID::Invalid();
	if (cp->GetPassengerRoutingMode() != PassengerRoutingMode::Journey && Station::IsValidID(cp->GetPassengerDestination())) {
		cp->SetPassengerLegacyHop(next);
		cp->SetPassengerVia(StationID::Invalid());
	}
	this->AddToCache(cp);
	if (_settings_game.linkgraph.passenger_journey_precomputed) {
		this->EnsureWaitingIndex();
		auto found = this->merge_index.find(cp->StationMergeHash());
		if (found != this->merge_index.end()) {
			/* Order by remaining packet capacity, then saved pool ID. Hash collisions
			 * are always checked with the original strict merge predicate. */
			for (auto it = found->second.lower_bound({cp->Count(), CargoPacketID{0}}); it != found->second.end();) {
				CargoPacket *candidate = CargoPacket::Get(it++->second);
				auto entry = this->waiting_entries.find(candidate->index);
				if (entry != this->waiting_entries.end() && entry->second.key == next && StationCargoList::TryMerge(candidate, cp)) return;
			}
		}
		auto &list = this->packets[next]; list.push_back(cp); this->IndexPacket(cp, next, std::prev(list.end()));
		return;
	}

	StationCargoPacketMap::List &list = this->packets[next];
	for (StationCargoPacketMap::List::reverse_iterator it(list.rbegin());
			it != list.rend(); it++) {
		if (StationCargoList::TryMerge(*it, cp)) return;
	}

	/* The packet could not be merged with another one */
	list.push_back(cp);
}

/**
 * Shifts cargo from the front of the packet list for a specific station and
 * applies some action to it.
 * @tparam Taction Action class or function to be used. It should define
 *                 "bool operator()(CargoPacket *)". If true is returned the
 *                 cargo packet will be removed from the list. Otherwise it
 *                 will be kept and the loop will be aborted.
 * @param action Action instance to be applied.
 * @param next Next hop the cargo wants to visit.
 * @return True if all packets with the given next hop have been removed,
 *         False otherwise.
 */
template <class Taction>
bool StationCargoList::ShiftCargo(Taction &action, StationID next)
{
	for (Iterator it = this->packets.lower_bound(next); it != this->packets.end() && it.GetKey() == next;) {
		if (action.MaxMove() == 0) return false;
		CargoPacket *cp = *it;
		if (action(cp)) {
			it = this->packets.erase(it);
		} else {
			return false;
		}
	}
	return true;
}

/**
 * Shifts cargo from the front of the packet list for a specific station and
 * and optional also from the list for "any station", then applies some action
 * to it.
 * @tparam Taction Action class or function to be used. It should define
 *                 "bool operator()(CargoPacket *)". If true is returned the
 *                 cargo packet will be removed from the list. Otherwise it
 *                 will be kept and the loop will be aborted.
 * @param action Action instance to be applied.
 * @param next Next hop the cargo wants to visit.
 * @param include_invalid If cargo from the StationID::Invalid() list should be
 *                        used if necessary.
 * @return Amount of cargo actually moved.
 */
template <class Taction>
uint StationCargoList::ShiftCargo(Taction action, std::span<const StationID> next, bool include_invalid)
{
	uint max_move = action.MaxMove();
	for (StationID st : next) {
		this->ShiftCargo(action, st);
		if (action.MaxMove() == 0) break;
	}
	if (include_invalid && action.MaxMove() > 0) {
		this->ShiftCargo(action, StationID::Invalid());
	}
	return max_move - action.MaxMove();
}

/**
 * Shifts cargo from the front of the packet list for a specific station
 * from a specific source station and applies some action to it.
 * @tparam Taction Action class or function to be used. It should define
 *                 "bool operator()(CargoPacket *)". If true is returned the
 *                 cargo packet will be removed from the list. Otherwise it
 *                 will be kept and the loop will be aborted.
 * @param action Action instance to be applied.
 * @param source Source station.
 * @param next Next hop the cargo wants to visit.
 * @return True if all packets with the given next hop have been removed,
 *         False otherwise.
 */
template <class Taction>
bool StationCargoList::ShiftCargoFromSource(Taction &action, StationID source, StationID next)
{
	for (Iterator it = this->packets.lower_bound(next); it != this->packets.end() && it.GetKey() == next;) {
		if (action.MaxMove() == 0) return false;
		CargoPacket *cp = *it;
		if (cp->GetFirstStation() != source) {
			++it;
			continue;
		}
		if (action(cp)) {
			it = this->packets.erase(it);
		} else {
			return false;
		}
	}
	return true;
}

/**
 * Shifts cargo from the front of the packet list for a specific station
 * and optional also from the list for "any station", then applies some action
 * to it, for a specific source station.
 * @tparam Taction Action class or function to be used. It should define
 *                 "bool operator()(CargoPacket *)". If true is returned the
 *                 cargo packet will be removed from the list. Otherwise it
 *                 will be kept and the loop will be aborted.
 * @param action Action instance to be applied.
 * @param source Source station.
 * @param next Next hop the cargo wants to visit.
 * @param include_invalid If cargo from the StationID::Invalid() list should be
 *                        used if necessary.
 * @return Amount of cargo actually moved.
 */
template <class Taction>
uint StationCargoList::ShiftCargoFromSource(Taction action, StationID source, std::span<const StationID> next, bool include_invalid)
{
	uint max_move = action.MaxMove();
	for (StationID st : next) {
		this->ShiftCargoFromSource(action, source, st);
		if (action.MaxMove() == 0) break;
	}
	if (include_invalid && action.MaxMove() > 0) {
		this->ShiftCargoFromSource(action, source, StationID::Invalid());
	}
	return max_move - action.MaxMove();
}

uint StationCargoList::AvailableViaCount(StationID next) const
{
	uint count = 0;
	for (ConstIterator it = this->packets.lower_bound(next); it != this->packets.end() && it.GetKey() == next; ++it) {
		count += (*it)->count;
	}
	return count;
}

/**
 * Truncates where each destination loses roughly the same percentage of its
 * cargo. This is done by randomizing the selection of packets to be removed.
 * Optionally count the cargo by origin station.
 * @param max_move Maximum amount of cargo to remove.
 * @param cargo_per_source Container for counting the cargo by origin.
 * @return Amount of cargo actually moved.
 */
uint StationCargoList::Truncate(uint max_move, StationCargoAmountMap *cargo_per_source)
{
	max_move = std::min(max_move, this->count);
	uint prev_count = this->count;
	uint moved = 0;
	uint loop = 0;
	bool do_count = cargo_per_source != nullptr;
	while (max_move > moved) {
		for (Iterator it(this->packets.begin()); it != this->packets.end();) {
			CargoPacket *cp = *it;
			if (prev_count > max_move && RandomRange(prev_count) < prev_count - max_move) {
				if (do_count && loop == 0) {
					(*cargo_per_source)[cp->first_station] += cp->count;
				}
				++it;
				continue;
			}
			uint diff = max_move - moved;
			if (cp->count > diff) {
				if (diff > 0) {
					this->RemoveFromCache(cp, diff);
					cp->Reduce(diff);
					moved += diff;
				}
				if (loop > 0) {
					if (do_count) (*cargo_per_source)[cp->first_station] -= diff;
					return moved;
				} else {
					if (do_count) (*cargo_per_source)[cp->first_station] += cp->count;
					++it;
				}
			} else {
				it = this->packets.erase(it);
				if (do_count && loop > 0) {
					(*cargo_per_source)[cp->first_station] -= cp->count;
				}
				moved += cp->count;
				this->RemoveFromCache(cp, cp->count);
				delete cp;
			}
		}
		loop++;
	}
	return moved;
}

/**
 * Reserves cargo for loading onto the vehicle.
 * @param max_move Maximum amount of cargo to reserve.
 * @param dest VehicleCargoList to reserve for.
 * @param next_station Next station(s) the loading vehicle will visit.
 * @param current_tile Current tile the cargo handling is happening on.
 * @return Amount of cargo actually reserved.
 */
uint StationCargoList::Reserve(uint max_move, VehicleCargoList *dest, std::span<const StationID> next_station, TileIndex current_tile, const Vehicle *vehicle, PassengerJourneyLoadingContext *loading)
{
	PassengerJourneyLoadingContext local_loading;
	return this->ShiftJourneyCargo(CargoReservation(this, dest, max_move, current_tile), next_station, vehicle, loading == nullptr ? local_loading : *loading);
}

/**
 * Loads cargo onto a vehicle. If the vehicle has reserved cargo load that.
 * Otherwise load cargo from the station.
 * @param max_move Amount of cargo to load.
 * @param dest Vehicle cargo list where the cargo resides.
 * @param next_station Next station(s) the loading vehicle will visit.
 * @param current_tile Current tile the cargo handling is happening on.
 * @return Amount of cargo actually loaded.
 * @note Vehicles may or may not reserve, depending on their orders. The two
 *       modes of loading are exclusive, though. If cargo is reserved we don't
 *       need to load unreserved cargo.
 */
uint StationCargoList::Load(uint max_move, VehicleCargoList *dest, std::span<const StationID> next_station, TileIndex current_tile, const Vehicle *vehicle, PassengerJourneyLoadingContext *loading)
{
	PassengerJourneyLoadingContext local_loading;
	if (loading == nullptr) loading = &local_loading;
	/* Return invalid Journey reservations only, retaining Legacy/valid reservations. */
	if (dest->ReservedCount() > 0) {
		dest->ReturnReserved(this, StationID::Invalid(), current_tile,
				[&](const CargoPacket &packet) { return !loading->CanBoard(packet, vehicle); }, vehicle == nullptr ? INVALID_CARGO : vehicle->cargo_type);
	}
	uint move = std::min(dest->ActionCount(VehicleCargoList::MoveToAction::Load), max_move);
	if (move > 0) {
		move = dest->Reassign<VehicleCargoList::MoveToAction::Load, VehicleCargoList::MoveToAction::Keep>(move);
		this->reserved_count -= move;
		return move;
	} else {
		return this->ShiftJourneyCargo(CargoLoad{this, dest, max_move, current_tile}, next_station, vehicle, *loading);
	}
}

/** Skip other vehicles' journey packets even when they share a CargoDist next-hop bucket.
 * Legacy iteration/order and the real split/reservation actions are kept unchanged. */
template <class Taction>
uint StationCargoList::ShiftJourneyCargo(Taction action, std::span<const StationID> next, const Vehicle *vehicle, PassengerJourneyLoadingContext &loading)
{
	if (vehicle != nullptr && this->passenger_index_cargo != vehicle->cargo_type) { this->ClearWaitingIndex(); this->passenger_index_cargo = vehicle->cargo_type; }
	if (_settings_game.linkgraph.passenger_journey_precomputed) return this->ShiftIndexedJourneyCargo(std::move(action), next, vehicle, loading);
	uint maximum = action.MaxMove();
	auto shift = [&](StationID key) {
		for (Iterator it(this->packets.lower_bound(key)); it != this->packets.end() && it.GetKey() == key && action.MaxMove() > 0;) {
			CargoPacket *packet = *it;
			if (!loading.CanBoard(*packet, vehicle)) { ++it; continue; }
			const auto intent = packet->GetPassengerJourney();
			const StationID legacy = packet->GetPassengerLegacyHop();
			const StationID via = packet->GetPassengerVia();
			const auto previous_transport = packet->GetPassengerTransport();
			loading.PrepareBoard(*packet, vehicle);
			if (action(packet)) it = this->packets.erase(it);
			else {
				/* A split copies the bound ride into the moved suffix. Passengers
				 * remaining at the station can still board another shared vehicle. */
				if (IsPassengerRideIntent(intent.kind)) packet->SetPassengerJourney(intent, legacy);
				else if (packet->GetPassengerRoutingMode() != PassengerRoutingMode::Journey) packet->SetPassengerVia(via);
				packet->SetPassengerTransport(previous_transport);
				break;
			}
		}
	};
	for (StationID key : next) { shift(key); if (action.MaxMove() == 0) break; }
	if (action.MaxMove() > 0) shift(StationID::Invalid());
	return maximum - action.MaxMove();
}

void StationCargoList::RecordCapacityMiss(std::span<const StationID> next, const Vehicle *vehicle, CargoType cargo, PassengerJourneyLoadingContext &loading)
{
	if (vehicle == nullptr || vehicle->First()->orders == nullptr || !IsPassengerJourneyRoutingEnabled(cargo)) return;
	if (this->passenger_index_cargo != cargo) { this->ClearWaitingIndex(); this->passenger_index_cargo = cargo; }
	this->EnsureWaitingIndex();
	if (this->hyperpath_boarding_index.empty()) {
		/* Readiness is derived: a warm index can still hold due packets in its
		 * wake queue while a joining peer already indexed them as ready. Wake
		 * them before this fast exit, exactly as VisitBoardingCandidates does.
		 * A legacy-only consist still performs no validation/routing mutation. */
		while (!this->waiting_wake.empty() && this->waiting_wake.begin()->first <= _state_ticks) {
			auto wake = this->waiting_wake.begin(); auto it = this->waiting_entries.find(wake->second);
			if (it != this->waiting_entries.end()) { it->second.pending = false; this->AddBoardingEntry(wake->second, it->second); }
			this->waiting_wake.erase(wake);
		}
		if (this->hyperpath_boarding_index.empty()) return;
	}
	bool capacity = false;
	for (const Vehicle *part = vehicle->First(); part != nullptr; part = part->Next()) if (part->cargo_type == cargo && part->cargo_cap != 0) {
		capacity = true;
		if (part->cargo.RemainingCount() < part->cargo_cap) return;
	}
	if (!capacity) return;
	const auto started = std::chrono::steady_clock::now();
	loading.EnableWaitingEvents();
	this->VisitBoardingCandidates(next, vehicle, loading, [&](CargoPacketID id) {
		auto entry = this->waiting_entries.find(id); if (entry == this->waiting_entries.end()) return true;
		auto *packet = CargoPacket::Get(id);
		if (packet->GetPassengerJourney().kind != PassengerRideKind::HyperpathIntent || !loading.CanBoard(*packet, vehicle, cargo)) return true;
		const auto key = entry->second.key; const auto position = entry->second.position; const auto sequence = entry->second.sequence;
		this->ForgetWaitingPacket(id);
		++_passenger_event_diagnostic.capacity_missed_packets; _passenger_event_diagnostic.capacity_missed_passengers += packet->Count();
		packet->RecordPassengerCapacityFailure(vehicle->First()->orders->index, _state_ticks, loading.ExpectedHyperpathWait(*packet, vehicle, cargo));
		this->IndexPacket(packet, key, position, sequence);
		return true;
	}, false); // Capacity failures retain the original full eligibility scan.
	_passenger_event_diagnostic.capacity_record_nanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
}

bool StationCargoList::HasCargoFor(std::span<const StationID> next, const Vehicle *vehicle, PassengerJourneyLoadingContext *loading, CargoType cargo) const
{
	PassengerJourneyLoadingContext local_loading;
	if (loading == nullptr) loading = &local_loading;
	if (_settings_game.linkgraph.passenger_journey_precomputed) {
		CargoType actual_cargo = cargo < NUM_CARGO ? cargo : vehicle == nullptr ? INVALID_CARGO : vehicle->cargo_type;
		if (actual_cargo != this->passenger_index_cargo) { const_cast<StationCargoList *>(this)->ClearWaitingIndex(); const_cast<StationCargoList *>(this)->passenger_index_cargo = actual_cargo; }
		bool found = false;
		this->VisitBoardingCandidates(next, vehicle, *loading, [&](CargoPacketID id) {
			++this->boarding_candidates_inspected;
			found = loading->CanBoard(*CargoPacket::Get(id), vehicle, cargo);
			return !found;
		});
		return found;
	}
	auto has = [&](StationID key) {
		for (ConstIterator it(this->packets.lower_bound(key)); it != this->packets.end() && it.GetKey() == key; ++it) if (loading->CanBoard(**it, vehicle, cargo)) return true;
		return false;
	};
	for (StationID key : next) if (has(key)) return true;
	return has(StationID::Invalid());
}

/**
 * Routes packets with station "avoid" as next hop to a different place.
 * @param max_move Maximum amount of cargo to move.
 * @param dest List to append the cargo to.
 * @param avoid Station to exclude from routing and current next hop of packets to reroute.
 * @param avoid2 Additional station to exclude from routing.
 * @param ge GoodsEntry to get the routing info from.
 * @return The number of elements that got rerouted.
 */
uint StationCargoList::Reroute(uint max_move, StationCargoList *dest, StationID avoid, StationID avoid2, const GoodsEntry *ge)
{
	return this->ShiftCargo(StationCargoReroute(this, dest, max_move, avoid, avoid2, ge), {&avoid, 1}, false);
}

/**
 * Routes packets with station "avoid" as next hop to a different place.
 * @param max_move Maximum amount of cargo to move.
 * @param dest List to append the cargo to.
 * @param avoid Station to exclude from routing and current next hop of packets to reroute.
 * @param avoid2 Additional station to exclude from routing.
 * @param ge GoodsEntry to get the routing info from.
 */
uint StationCargoList::RerouteFromSource(uint max_move, StationCargoList *dest, StationID source, StationID avoid, StationID avoid2, const GoodsEntry *ge)
{
	return this->ShiftCargoFromSource(StationCargoReroute(this, dest, max_move, avoid, avoid2, ge), source, {&avoid, 1}, false);
}

/*
 * We have to instantiate everything we want to be usable.
 */
template class CargoList<VehicleCargoList, CargoPacketList>;
template class CargoList<StationCargoList, StationCargoPacketMap>;

/**
 * Moves some cargo from one designation to another.
 * @return Amount of cargo actually reassigned.
 */
template uint VehicleCargoList::Reassign<VehicleCargoList::MoveToAction::Deliver, VehicleCargoList::MoveToAction::Keep>(uint);
