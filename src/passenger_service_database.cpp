/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_service_database.cpp One-cycle compilation, bounded publication and on-demand policies. */
#include "stdafx.h"
#include "passenger_service_database.h"
#include "passenger_journey_active.h"
#include "vehicle_base.h"
#include "aircraft.h"
#include "station_base.h"
#include "map_func.h"
#include "date_func.h"
#include "settings_type.h"
#include "sl/saveload.h"
#include "error_func.h"
#include <chrono>
#include <queue>
#include <bitset>

#include "safeguards.h"

namespace {
	PassengerServiceDatabase database;
	constexpr uint64_t DATABASE_FORMAT_VERSION = 2;
	constexpr int64_t MAX_PERIOD = int64_t{1} << 40;
	bool AddTicks(int64_t &value, int64_t delta)
	{
		if (delta < 0 || value > INT64_MAX - delta) return false;
		value += delta;
		return true;
	}

	/** A live dwell replaces (rather than supplements) this vehicle's static
	 * phase, including for future-ready queries. These are the original scan's
	 * safety conditions; production loading states belong to loading_vehicles. */
	std::optional<StateTicks> LoadingBase(const Vehicle *live, const PassengerPeriodicService &service, const PassengerPeriodicStop &board)
	{
		if (live == nullptr || live->orders == nullptr || live->orders->index != service.id ||
				live->orders->GetCargoDistRouteGeneration() != service.incarnation ||
				live->vehstatus.Any({VehState::Stopped, VehState::Crashed}) || live->vehicle_flags.Test(VehicleFlag::ScheduledDispatch) ||
				!live->current_order.IsAnyLoadingType() || live->current_order.IsType(OT_LOADING_ADVANCE) ||
				live->cur_real_order_index != board.order || live->cur_implicit_order_index != board.order || live->last_station_visited != board.station ||
				(live->cur_timetable_order_index != INVALID_VEH_ORDER_ID && live->cur_timetable_order_index != board.order)) return {};
		int64_t departure_now = _state_ticks.base();
		int64_t remaining = std::max<int64_t>(0, board.departure - board.arrival - live->current_order_time);
		if (!AddTicks(departure_now, remaining) || departure_now < INT64_MIN + board.departure) return {};
		return StateTicks{departure_now - board.departure};
	}

	/** The depot controller advances the order on entry unless a wait is declared.
	 * Its departure/acceleration belongs to the next order's recorded travel. */
	std::optional<int64_t> TimetableEventWait(const Order &order, PassengerPeriodicTimetable::Failure *failure = nullptr)
	{
		using Failure = PassengerPeriodicTimetable::Failure;
		auto fail = [&](Failure why) { if (failure != nullptr) *failure = why; return std::optional<int64_t>{}; };
		const bool depot = order.IsType(OT_GOTO_DEPOT);
		if (depot && order.IsScheduledDispatchOrder(true)) return fail(Failure::Control);
		if (order.IsRefit()) return fail(Failure::Refit);
		if (depot ? ClassifyPassengerDepot(order) != PassengerDepotTraversal::Timed :
				!order.IsType(OT_GOTO_STATION) && !order.IsType(OT_GOTO_WAYPOINT)) return fail(Failure::Control);
		if (!order.IsTravelTimetabled()) return fail(Failure::Travel);
		if (depot) return order.GetTimetabledWait();
		if ((IsPassengerStationStop(order) || order.GetWaitTime() != 0) && !order.IsWaitTimetabled()) return fail(Failure::Wait);
		return order.GetWaitTime();
	}
	size_t ServiceBytes(const PassengerPeriodicService &s)
	{
		/* Include vector slack, station map/set nodes and each set's sentinel even
		 * when every stop introduces a different station. */
		return 512 + 512 * s.timetable.stops.size() + 2 * (
				s.vehicles.size() * sizeof(PassengerPeriodicVehicle) + s.dispatch_slots.size() * sizeof(uint32_t));
	}
	bool SamePrecomputedStructure(const PassengerPeriodicService &a, const PassengerPeriodicService &b)
	{
		if (a.incarnation != b.incarnation || a.timetable.GetCertainty() != b.timetable.GetCertainty() || a.timetable.stops.size() != b.timetable.stops.size()) return false;
		for (size_t i = 0; i < a.timetable.stops.size(); ++i) {
			const auto &x = a.timetable.stops[i], &y = b.timetable.stops[i];
			if (std::tie(x.station, x.order, x.board, x.alight, x.forced, x.continues) != std::tie(y.station, y.order, y.board, y.alight, y.forced, y.continues)) return false;
		}
		return true;
	}
	/** Actual progress in a complete explicit cycle. Waypoints and non-stopping
	 * station orders have a timing offset too. Overdue travel has no certified
	 * arrival; an already loading vehicle can depart now after its declared wait. */
	std::optional<StateTicks> CapturePeriodicBase(const Vehicle *v, const PassengerPeriodicTimetable &table)
	{
		if (v->orders == nullptr || table.period == 0 || v->cur_real_order_index >= v->GetNumOrders()) return {};
		const bool loading = v->current_order.IsAnyLoadingType();
		const Order *current = v->GetOrder(v->cur_real_order_index);
		if (!loading && (!current->IsTravelTimetabled() || v->current_order_time > current->GetTravelTime())) return {};
		int64_t offset = 0, origin = 0, arrival = 0;
		bool first = true;
		for (size_t i = 0; i < v->GetNumOrders(); ++i) {
			const Order &order = *v->GetOrder(static_cast<VehicleOrderID>(i));
			if (order.IsType(OT_IMPLICIT) || order.IsType(OT_LABEL)) continue;
			if (!first && !AddTicks(offset, order.GetTravelTime())) return {};
			first = false;
			if (i == table.stops.front().order) origin = offset;
			if (i == v->cur_real_order_index) arrival = offset;
			auto wait = TimetableEventWait(order);
			if (!wait || !AddTicks(offset, *wait)) return {};
		}
		int64_t elapsed = loading ? std::min<uint32_t>(v->current_order_time, current->GetWaitTime()) : v->current_order_time;
		int64_t delta = arrival - origin + elapsed - (loading ? 0 : current->GetTravelTime());
		if ((delta > 0 && _state_ticks.base() < INT64_MIN + delta) || (delta < 0 && _state_ticks.base() > INT64_MAX + delta)) return {};
		return StateTicks{_state_ticks.base() - delta};
	}
}

PassengerServiceDatabase &GetPassengerServiceDatabase() { return database; }
void InvalidatePassengerServiceDatabase(OrderListID id) { database.Invalidate(id); }
void ResetPassengerServiceDatabase() { database.Reset(); }

void ObservePassengerPeriodicVehicle(const Vehicle *vehicle)
{
	if (!_settings_game.linkgraph.passenger_journey_routing || vehicle == nullptr || vehicle->orders == nullptr) return;
	const auto *service = database.Get(vehicle->orders->index);
	if (service == nullptr) return;
	auto phase = std::lower_bound(service->vehicles.begin(), service->vehicles.end(), vehicle->index, [](const auto &v, VehicleID id) { return v.id < id; });
	if (phase != service->vehicles.end() && phase->id != vehicle->index) phase = service->vehicles.end();
	if (phase == service->vehicles.end() && service->timetable.period != 0) return;
	std::array<uint64_t, NUM_CARGO> capacity{};
	bool passengers = false;
	size_t parts = 4096;
	for (const Vehicle *part = vehicle; part != nullptr; part = part->Next()) {
		if (parts-- == 0) return;
		if (part->cargo_type < NUM_CARGO) {
			capacity[part->cargo_type] += part->cargo_cap;
			passengers |= part->cargo_cap > 0 && IsCargoInClass(part->cargo_type, CargoClass::Passengers);
		}
	}
	if (phase != service->vehicles.end() && capacity != phase->capacity) { database.Invalidate(service->id); return; }
	if (database.GetPublication(service->id) != PassengerServiceDatabase::Publication::Published) return;
	/* Estimated patterns are refreshed by the saved service update queue, not
	 * by rescanning their orders at each live loading observation. */
	if (service->timetable.estimated_period > 0) return;
	if (service->timetable.period == 0) {
		/* Old JPSD records may have rejected a regular depot with no wait. Do
		 * not rewrite publication or queues on load/join: reconsider only at a
		 * real loading event, using the normal coalesced, saved build queue.
		 * This bounded flag/duration pass allocates no stop or cargo-mask vector. */
		if (!vehicle->current_order.IsAnyLoadingType() || vehicle->vehstatus.Any({VehState::Stopped, VehState::Crashed}) ||
				vehicle->GetNumOrders() > 4096) return;
		if (!passengers) return;
		bool depot = false;
		size_t stops = 0;
		int64_t period = 0;
		for (const Order &order : vehicle->orders->GetOrderVector()) {
			if (order.IsType(OT_IMPLICIT) || order.IsType(OT_LABEL)) continue;
			auto wait = TimetableEventWait(order);
			if (!wait || !AddTicks(period, order.GetTravelTime()) || !AddTicks(period, *wait) || period > MAX_PERIOD) return;
			depot |= order.IsType(OT_GOTO_DEPOT);
			stops += IsPassengerStationStop(order);
		}
		if (depot && stops >= 2 && period > 0) database.TimetableChanged(service->id);
		return;
	}
	if (vehicle->vehicle_flags.Test(VehicleFlag::ScheduledDispatch)) return;
	if (phase->phase_known && vehicle->current_order_time != 0) return;
	if (std::none_of(service->timetable.stops.begin(), service->timetable.stops.end(), [&](const auto &stop) { return stop.order == vehicle->cur_real_order_index && stop.station == vehicle->last_station_visited; })) return;
	if (vehicle->HasUnbunchingOrder()) return;
	size_t work = 4096;
	auto state = GetPassengerRouteState(vehicle, &work);
	if (state && state->loading && std::all_of(state->usable_stops.begin(), state->usable_stops.end(), [](uint8_t usable) { return usable != 0; })) {
		auto observed = CapturePeriodicBase(vehicle, service->timetable);
		auto modulo = [&](StateTicks ticks) { int64_t value = ticks.base() % service->timetable.period; return value < 0 ? value + service->timetable.period : value; };
		if (observed && (!phase->phase_known || modulo(*observed) != modulo(phase->base))) {
			/* Phase drift changes timing estimates, not the live boarding contract.
			 * Coalesce it with timetable measurements instead of dropping the network. */
			database.TimetableChanged(service->id);
		}
	}
}

void PassengerServiceDatabase::ShiftTicks(StateTicksDelta delta)
{
	this->ClearDepartureIndices(); // Remainders and first-circuit bounds both move.
	auto shift = [&](auto &network) {
		for (auto &[id, service] : network) {
			service.dispatch_base += delta;
			service.dispatch_floor += delta;
			for (auto &vehicle : service.vehicles) vehicle.base += delta;
		}
	};
	shift(this->services); shift(this->building); shift(this->precomputed_services); shift(this->retained_precomputed_services);
	this->precomputed_refresh += delta;
	this->batch_started += delta;
	for (auto &[id, since] : this->build_since) since += delta;
	for (auto &[id, since] : this->staged_since) since += delta;
	this->sweeping_since += delta;
	for (auto &[id, since] : this->dirty_since) since += delta;
}

void PassengerPeriodicService::BuildAircraftCosts(PassengerAircraftCost cost)
{
	this->aircraft_adjustments.clear();
	const auto &stops = this->timetable.stops;
	if (cost.IsDefault() || !this->IsAircraft() || this->timetable.Circuit() <= 0 || stops.empty()) return;
	this->aircraft_adjustments.resize(stops.size() + 1);
	for (size_t i = 0; i < stops.size(); ++i) {
		size_t next = (i + 1) % stops.size();
		int64_t travel = stops[next].arrival - stops[i].departure + (next == 0 ? this->timetable.Circuit() : 0);
		this->aircraft_adjustments[i + 1] = this->aircraft_adjustments[i] + cost.Adjustment(travel - stops[next].intermediate_wait);
	}
}

PassengerPeriodicTimetable PassengerPeriodicTimetable::Compile(std::span<const Order> orders, Failure *failure)
{
	if (failure != nullptr) *failure = Failure::None;
	auto fail = [&](Failure why) { if (failure != nullptr) *failure = why; return PassengerPeriodicTimetable{}; };
	PassengerPeriodicTimetable result;
	int64_t cursor = 0, first_travel = 0, intermediate_wait = 0, leading_wait = 0;
	bool first = true;
	for (size_t i = 0; i < orders.size(); ++i) {
		const Order &order = orders[i];
		if (order.IsType(OT_IMPLICIT) || order.IsType(OT_LABEL)) continue;
		auto wait = TimetableEventWait(order, failure);
		if (!wait) return {};
		if (first) { first_travel = order.GetTravelTime(); first = false; }
		else if (!AddTicks(cursor, order.GetTravelTime())) return fail(Failure::Overflow);
		bool stopping = order.IsType(OT_GOTO_STATION) && !(order.GetNonStopType() & ONSF_NO_STOP_AT_DESTINATION_STATION);
		if (stopping) {
			PassengerPeriodicStop stop;
			stop.station = order.GetDestination().ToStationID(); stop.order = static_cast<VehicleOrderID>(i);
			stop.boarding_facility = order.GetRoadVehTravelDirection() == DiagDirection::Invalid ? 1 : 2 + static_cast<uint8_t>(order.GetRoadVehTravelDirection());
			stop.intermediate_wait = intermediate_wait;
			if (result.stops.empty()) leading_wait = intermediate_wait;
			intermediate_wait = 0;
			stop.arrival = cursor;
			if (!AddTicks(cursor, *wait)) return fail(Failure::Overflow);
			stop.departure = cursor;
			for (CargoType cargo : EnumRange(NUM_CARGO)) {
				OrderUnloadType unload = order.GetCargoUnloadType(cargo);
				if (unload == OrderUnloadType::Unload || unload == OrderUnloadType::Transfer) stop.forced |= uint64_t{1} << cargo;
				if (unload != OrderUnloadType::NoUnload) stop.alight |= uint64_t{1} << cargo;
				if (order.GetCargoLoadType(cargo) != OrderLoadType::NoLoad && !(stop.forced & (uint64_t{1} << cargo))) stop.board |= uint64_t{1} << cargo;
			}
			result.stops.push_back(stop);
		} else {
			if (!AddTicks(cursor, *wait) || !AddTicks(intermediate_wait, *wait)) return fail(Failure::Overflow);
		}
		if (cursor > MAX_PERIOD) return fail(Failure::Overflow);
	}
	if (!AddTicks(cursor, first_travel) || cursor > MAX_PERIOD) return fail(Failure::Overflow);
	if (cursor == 0) return fail(Failure::ZeroPeriod);
	if (result.stops.size() < 2) return fail(Failure::TooFewStops);
	/* The phase origin is the arrival at the first explicit stopping order. */
	result.stops.front().intermediate_wait = intermediate_wait + leading_wait;
	int64_t origin = result.stops.front().arrival;
	for (auto &stop : result.stops) { stop.arrival -= origin; stop.departure -= origin; }
	result.period = cursor;
	return result;
}

/** Preserve unconditional order cycles. A conditional/jump, refit or terminating
 * depot remains Unsupported: a linear fragment behind an always-taken jump or
 * halt is not evidence of an operating service. Partial patterns require a
 * separate control-flow/recurrence proof, not just cutting the preceding edge.
 * Recorded positive durations outrank geometry. Unknown zero dwell uses 10 ticks
 * (aircraft 32); geometric travel uses conservative mode-specific ticks/tile and
 * a fixed acceleration/terminal allowance. No live position or local cache input. */
PassengerPeriodicTimetable PassengerPeriodicTimetable::CompileEstimated(std::span<const Order> orders, VehicleType type)
{
	PassengerPeriodicTimetable result;
	if (orders.size() > 4096 || type >= VehicleType::CompanyEnd) return result;
	int64_t cursor = 0, first_travel = 0, intermediate_wait = 0, leading_wait = 0;
	bool first = true;
	const bool zero_circuit = std::none_of(orders.begin(), orders.end(), [](const Order &o) {
		return !o.IsType(OT_IMPLICIT) && !o.IsType(OT_LABEL) && (o.GetTravelTime() != 0 ||
				(o.IsType(OT_GOTO_DEPOT) ? o.GetTimetabledWait() : o.GetWaitTime()) != 0);
	});
	uint8_t quality = 0, leading_quality = 0;
	const BaseStation *previous = nullptr;
	for (const auto &order : orders) if (order.IsType(OT_GOTO_STATION) || order.IsType(OT_GOTO_WAYPOINT)) {
		if (const auto *position = BaseStation::GetIfValid(order.GetDestination().ToStationID())) previous = position;
	}
	for (size_t i = 0; i < orders.size(); ++i) {
		const Order &order = orders[i];
		if (order.IsType(OT_IMPLICIT) || order.IsType(OT_LABEL)) continue;
		bool safe = !order.IsRefit() && (order.IsType(OT_GOTO_STATION) || order.IsType(OT_GOTO_WAYPOINT) ||
				(order.IsType(OT_GOTO_DEPOT) && ClassifyPassengerDepot(order) != PassengerDepotTraversal::Blocked && !order.IsScheduledDispatchOrder(true)));
		if (!safe) return {};
		const Station *station = order.IsType(OT_GOTO_STATION) ? Station::GetIfValid(order.GetDestination().ToStationID()) : nullptr;
		const BaseStation *position = order.IsBaseStationOrder() ? BaseStation::GetIfValid(order.GetDestination().ToStationID()) : nullptr;
		int64_t travel = order.GetTravelTime();
		if (!order.IsTravelTimetabled() || zero_circuit) {
			quality = std::max<uint8_t>(quality, travel > 0 ? 1 : 2);
			if (travel == 0) {
				int64_t distance = previous != nullptr && position != nullptr ? DistanceManhattan(previous->xy, position->xy) : 8;
				int64_t per_tile = type == VehicleType::Road ? 8 : type == VehicleType::Ship ? 16 : type == VehicleType::Aircraft ? 2 : 4;
				travel = std::max<int64_t>(1, distance) * per_tile + (type == VehicleType::Aircraft ? 128 : 16);
			}
		}
		if (first) { first_travel = travel; first = false; }
		else if (!AddTicks(cursor, travel)) return {};
		int64_t wait = order.IsType(OT_GOTO_DEPOT) ? order.GetTimetabledWait() : order.GetWaitTime();
		if (IsPassengerStationStop(order) && !order.IsWaitTimetabled() && wait == 0) wait = type == VehicleType::Aircraft ? 32 : 10;
		if (IsPassengerStationStop(order) && station != nullptr) {
			PassengerPeriodicStop stop;
			stop.station = order.GetDestination().ToStationID(); stop.order = static_cast<VehicleOrderID>(i);
			stop.boarding_facility = order.GetRoadVehTravelDirection() == DiagDirection::Invalid ? 1 : 2 + static_cast<uint8_t>(order.GetRoadVehTravelDirection());
			stop.intermediate_wait = intermediate_wait;
			if (result.stops.empty()) leading_wait = intermediate_wait;
			intermediate_wait = 0;
			stop.arrival = cursor; stop.travel_quality = quality;
			if (!AddTicks(cursor, wait)) return {};
			stop.departure = cursor;
			for (CargoType cargo : EnumRange(NUM_CARGO)) {
				uint64_t mask = uint64_t{1} << cargo;
				auto unload = order.GetCargoUnloadType(cargo);
				if (unload == OrderUnloadType::Unload || unload == OrderUnloadType::Transfer) stop.forced |= mask;
				if (unload != OrderUnloadType::NoUnload) stop.alight |= mask;
				if (order.GetCargoLoadType(cargo) != OrderLoadType::NoLoad && !(stop.forced & mask)) stop.board |= mask;
			}
			if (result.stops.empty()) leading_quality = quality;
			result.stops.push_back(stop); quality = 0;
		} else {
			if (order.IsType(OT_GOTO_STATION) && station == nullptr) return {};
			if (!AddTicks(cursor, wait) || !AddTicks(intermediate_wait, wait)) return {};
		}
		/* Keep the last known position through a depot without coordinates. An
		 * unknown waypoint/depot must not replace a long station-to-station leg
		 * with just two short constant estimates. */
		if (position != nullptr) previous = position;
		if (cursor > MAX_PERIOD) return {};
	}
	if (!AddTicks(cursor, first_travel) || cursor <= 0 || cursor > MAX_PERIOD || result.stops.size() < 2) return {};
	result.stops.front().travel_quality = std::max(quality, leading_quality);
	result.stops.front().intermediate_wait = intermediate_wait + leading_wait;
	int64_t origin = result.stops.front().arrival;
	for (auto &stop : result.stops) { stop.arrival -= origin; stop.departure -= origin; }
	result.estimated_period = cursor;
	return result;
}

std::optional<StateTicks> PassengerPeriodicTimetable::NextDeparture(StateTicks base, int64_t offset, int64_t period, StateTicks ready)
{
	if (period <= 0 || period > MAX_PERIOD || offset < 0) return {};
	int64_t first = base.base();
	if (!AddTicks(first, offset)) return {};
	int64_t time = std::max(first, ready.base());
	/* Modulo each operand separately: ready - first can overflow StateTicks. */
	int64_t remainder = (time % period - first % period) % period;
	if (remainder < 0) remainder += period;
	if (!AddTicks(time, (period - remainder) % period)) return {};
	return StateTicks{time};
}

void PassengerServiceDatabase::ClearPolicies() { this->ClearPolicies(PassengerDatabaseDiagnostic::ClearReason::Explicit); }

void PassengerServiceDatabase::ClearDepartureIndices()
{
	this->departure_indices.clear(); this->departure_index_bytes = 0;
}

const PassengerDepartureIndex *PassengerServiceDatabase::DepartureIndex(const PassengerPeriodicService &service, CargoType cargo)
{
	if (!this->departure_index_enabled || service.dispatch_period > 0 || service.timetable.period <= 0) return nullptr;
	auto key = std::pair{service.id, cargo};
	auto found = this->departure_indices.find(key);
	if (found != this->departure_indices.end()) {
		if (found->second.Matches(service)) return &found->second;
		this->departure_index_bytes -= found->second.Bytes(); this->departure_indices.erase(found);
	}
	/* Check a conservative allocation envelope before building. Optional
	 * residency never spends simulation work or changes query memory admission.
	 * At the local cache ceiling use the original scan. */
	if (PassengerDepartureIndex::Envelope(service.vehicles.size()) > DEPARTURE_INDEX_BYTES - this->departure_index_bytes) return nullptr;
	const auto started = std::chrono::steady_clock::now();
	auto [it, inserted] = this->departure_indices.try_emplace(key, service, cargo);
	assert(inserted);
	this->departure_index_bytes += it->second.Bytes();
	assert(this->departure_index_bytes <= DEPARTURE_INDEX_BYTES);
	++this->diagnostic.departure_index_builds;
	this->diagnostic.departure_index_build_nanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
	this->diagnostic.departure_index_peak_bytes = std::max(this->diagnostic.departure_index_peak_bytes, this->departure_index_bytes);
	return &it->second;
}
void PassengerServiceDatabase::ClearPolicies(PassengerDatabaseDiagnostic::ClearReason reason)
{
	++this->diagnostic.policy_clears; ++this->diagnostic.clear_reasons[static_cast<size_t>(reason)];
	this->diagnostic.policies_cleared += this->policies.size();
	this->diagnostic.maximum_policies_cleared = std::max<uint64_t>(this->diagnostic.maximum_policies_cleared, this->policies.size());
	this->policies.clear(); this->policy_lru.clear(); this->policy_bytes = 0;
}

uint64_t PassengerServiceDatabase::StationRevision(StationID station) const
{
	return this->station_revisions.empty() ? 0 : this->station_revisions[station.base()];
}
void PassengerServiceDatabase::TouchStations(const PassengerPeriodicService &service)
{
	if (this->station_revisions.empty()) this->station_revisions.resize(StationID::Invalid().base());
	for (const auto &stop : service.timetable.stops) if (stop.board | stop.alight) this->station_revisions[stop.station.base()] = this->published_revision;
}
void PassengerServiceDatabase::ErasePolicy(std::map<PolicyKey, CachedPolicy>::iterator it)
{
	this->policy_bytes -= it->second.bytes;
	this->policy_lru.erase(it->second.lru); this->policies.erase(it);
}
PassengerServiceDatabase::CachedPolicy *PassengerServiceDatabase::FindPolicy(const PolicyKey &key)
{
	auto it = this->policies.find(key);
	if (it == this->policies.end()) return nullptr;
	const auto start = std::chrono::steady_clock::now();
	bool current = std::all_of(it->second.dependencies.begin(), it->second.dependencies.end(), [&](const auto &d) { return this->StationRevision(d.first) == d.second; });
	this->diagnostic.policy_validation_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	if (!current) { ++this->diagnostic.policy_stale; this->ErasePolicy(it); return nullptr; }
	this->policy_lru.splice(this->policy_lru.end(), this->policy_lru, it->second.lru);
	return &it->second;
}
void PassengerServiceDatabase::StorePolicy(const PolicyKey &key, PolicyValue value, Dependencies dependencies, size_t bytes)
{
	/* A separate, bounded observation history measures repeated OD builds,
	 * including changes in query limits. It never admits or orders searches. */
	auto od = std::tuple{std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key)};
	if (this->recent_policy_builds.contains(od)) ++this->diagnostic.policy_rebuilds;
	else { if (this->recent_policy_builds.size() >= POLICY_ENTRIES) this->recent_policy_builds.erase(this->recent_policy_builds.begin()); this->recent_policy_builds.emplace(od, 0); }
	++this->recent_policy_builds.at(od);
	/* Map nodes, dependency nodes and the shared LRU list are included. */
	bytes += 512 + sizeof(std::pair<StationID, uint64_t>) * dependencies.size();
	if (!this->policy_cache_enabled || bytes > POLICY_STORAGE_BYTES) return;
	const auto start = std::chrono::steady_clock::now();
	while (!this->policies.empty() && (this->policies.size() >= POLICY_ENTRIES || this->policy_bytes + bytes > POLICY_STORAGE_BYTES)) {
		this->ErasePolicy(this->policies.find(this->policy_lru.front())); ++this->diagnostic.policy_evictions;
	}
	this->diagnostic.policy_eviction_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	this->policy_lru.push_back(key);
	std::vector<std::pair<StationID, uint64_t>> stamps(dependencies.begin(), dependencies.end());
	this->policies.emplace(key, CachedPolicy{std::move(value), std::move(stamps), bytes, std::prev(this->policy_lru.end())});
	this->policy_bytes += bytes;
}

void PassengerServiceDatabase::Reset()
{
	this->preparation_capture = {};
	this->cooperative_graph = this->load_graph_refreshed = this->load_current_graph_started = false;
	this->timed_scratch.Release();
	this->ClearDepartureIndices();
	ClearPassengerOdFallbackCache();
	this->ClearPolicies(PassengerDatabaseDiagnostic::ClearReason::Reset);
	this->services.clear(); this->stations.clear(); this->dirty.clear(); this->dirty_since.clear();
	this->service_lookup.clear(); this->unsafe_lookup.reset();
	this->building.clear(); this->build_queue.clear(); this->build_since.clear(); this->staged_since.clear(); this->unsafe.clear(); this->station_revisions.clear(); this->recent_policy_builds.clear();
	this->sweeping_since = this->batch_started = _state_ticks;
	this->cursor = 0; this->dirty_cursor = 65534; this->sweeping = true; this->publishing = false; this->bootstrap = true; this->restoring = false;
	this->load_state = LoadState::Ready; this->load_remaining = this->load_work = 0;
	this->generation = this->published_revision = 0; this->bytes = this->building_bytes = 0;
	this->diagnostic = {}; this->schedule.ResetDiagnostic();
	this->DiscardPrecomputed(); this->precomputed_refresh = {};
	this->aircraft_cost = PassengerAircraftCost::Current();
	this->precomputed_aircraft_cost = this->retained_precomputed_aircraft_cost = {};
	this->legacy_aircraft_times = false;
}

void PassengerServiceDatabase::Remove(OrderListID id)
{
	if (id.base() < this->service_lookup.size()) this->service_lookup[id.base()] = nullptr;
	for (auto it = this->departure_indices.lower_bound({id, CargoType{0}}); it != this->departure_indices.end() && it->first.first == id;) {
		this->departure_index_bytes -= it->second.Bytes(); it = this->departure_indices.erase(it);
	}
	auto found = this->services.find(id);
	if (found == this->services.end()) return;
	for (const auto &stop : found->second.timetable.stops) {
		auto row = this->stations.find(stop.station);
		if (row == this->stations.end()) continue;
		for (auto ref = row->second.lower_bound({id, 0}); ref != row->second.end() && ref->service == id;) ref = row->second.erase(ref);
		if (row->second.empty()) this->stations.erase(row);
	}
	this->bytes -= ServiceBytes(found->second);
	this->services.erase(found);
}

void PassengerServiceDatabase::Invalidate(OrderListID id)
{
	if (this->restoring) return;
	/* UI edits may run between preparation slices in single player. A draft
	 * has not consumed its saved service cursor; restart before any live pointer
	 * can be deleted or its order vector changed. Network commands stay queued. */
	this->preparation_capture = {};
	++this->boarding_revision;
	RequestPassengerWaitingGeneration();
	++this->diagnostic.invalidations;
	if (id == OrderListID::Invalid()) {
		++this->diagnostic.global_invalidations;
		this->building.clear(); this->build_queue.clear(); this->build_since.clear(); this->staged_since.clear(); this->building_bytes = 0;
		/* Retain the immutable publication for planning, but reject real boarding
		 * immediately while a facility/global mutation is being checked. */
		for (const auto &[service_id, service] : this->services) { this->unsafe.insert(service_id); this->unsafe_lookup.set(service_id.base()); this->dirty.insert(service_id); this->dirty_since.try_emplace(service_id, _state_ticks); }
		this->sweeping_since = _state_ticks; this->cursor = 0; this->sweeping = true; this->publishing = false;
	} else {
		this->unsafe.insert(id);
		this->unsafe_lookup.set(id.base());
		if (auto it = this->building.find(id); it != this->building.end()) { this->building_bytes -= ServiceBytes(it->second); this->building.erase(it); this->staged_since.erase(id); }
		this->build_queue.erase(id); this->build_since.erase(id);
		this->dirty.insert(id); this->dirty_since.try_emplace(id, _state_ticks);
	}
}

void PassengerServiceDatabase::Index(const PassengerPeriodicService &service)
{
	for (size_t i = 0; i < service.timetable.stops.size(); ++i) {
		const auto &stop = service.timetable.stops[i];
		/* Keep physical occurrences (including phase/dispatch anchors) in JPSD.
		 * The shared index is a union over cargo; every query filters its own bit. */
		if ((stop.board | stop.alight) == 0) continue;
		auto &row = this->stations[stop.station];
		row.insert({service.id, i});
	}
}

void PassengerServiceDatabase::TimetableChanged(OrderListID id)
{
	if (this->restoring) return;
	this->preparation_capture = {};
	++this->diagnostic.timetable_notifications;
	/* A timing edit changes estimates, not real load/unload permission. Preserve
	 * the published cycle and policies while coalescing automatic measurements.
	 * Order/facility/fleet mutations instead block real boarding immediately. */
	this->dirty.insert(id);
	this->dirty_since.try_emplace(id, _state_ticks);
}

void PassengerServiceDatabase::Tick(size_t allowance, bool build_precomputed, bool prepare_graphs)
{
	PassengerJourneyWorkTimer timer;
	const auto started = std::chrono::steady_clock::now();
	this->UpdateAircraftCosts();
	const bool previous_cooperative = std::exchange(this->cooperative_graph, prepare_graphs);
	auto task = this->TickBuild(allowance, build_precomputed, false);
	task.Resume(SIZE_MAX);
	this->cooperative_graph = previous_cooperative;
	this->diagnostic.build_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
}

PassengerBuildTask PassengerServiceDatabase::TickBuild(size_t allowance, bool build_precomputed, bool cooperative)
{
	if (this->restoring || !_settings_game.linkgraph.passenger_journey_routing ||
			(!_settings_game.linkgraph.passenger_journey_periodic && !_settings_game.linkgraph.passenger_journey_precomputed)) co_return;
	size_t original = allowance;
	const size_t task_limit = _settings_game.linkgraph.passenger_journey_precomputed ? PRECOMPUTED_CAPTURE_WORK : BUILD_WORK;
	auto observe_age = [&](StateTicks since) {
		if (_state_ticks >= since) this->diagnostic.maximum_pending_ticks = std::max(this->diagnostic.maximum_pending_ticks, uint64_t(_state_ticks.base()) - uint64_t(since.base()));
	};
	if (this->sweeping) observe_age(this->sweeping_since);
	auto epoch = [](StateTicks ticks) { return ticks.base() / TIMETABLE_EPOCH - (ticks.base() % TIMETABLE_EPOCH < 0); };
	/* Freeze each work queue at a StateTicks boundary. Later notifications go
	 * to the next batch; continuous automatic measurements cannot starve it. */
	if (!this->publishing && this->build_queue.empty() && this->building.empty()) {
		if (this->sweeping || (!this->dirty.empty() && epoch(_state_ticks) > epoch(this->batch_started))) {
			this->batch_started = _state_ticks;
			this->build_queue.swap(this->dirty); this->build_since.swap(this->dirty_since);
		}
	}
	while (allowance > 0 && !this->publishing) {
		if (cooperative) co_await std::suspend_always{};
		OrderListID id;
		bool from_dirty = !this->build_queue.empty();
		if (from_dirty) {
			auto next = this->build_queue.upper_bound(OrderListID{this->dirty_cursor});
			if (next == this->build_queue.end()) next = this->build_queue.begin();
			id = *next; observe_age(this->build_since.at(id));
		} else {
			if (!this->sweeping) break;
			if (this->cursor >= OrderListID::Invalid().base()) { this->sweeping = false; break; }
			id = OrderListID{static_cast<uint16_t>(this->cursor)};
		}
		const OrderList *list = OrderList::GetIfValid(id);
		if (this->building.contains(id) || (!from_dirty && this->bootstrap && this->services.contains(id) && !this->unsafe.contains(id))) { --allowance; ++this->cursor; continue; }
		StateTicks since = from_dirty ? this->build_since.at(id) : this->sweeping_since;
		if (list == nullptr) {
			--allowance;
			if (from_dirty) {
				/* Deletions need staged tombstones too. Drain a full staging batch
				 * before admitting another, so a mid-update save stays loadable. */
				if (this->building_bytes + 512 > MAX_BYTES) { this->publishing = true; break; }
				assert(this->generation != UINT64_MAX);
				PassengerPeriodicService removed; removed.id = id; removed.generation = ++this->generation;
				this->building_bytes += ServiceBytes(removed); this->building.emplace(id, std::move(removed)); this->staged_since.emplace(id, since);
				this->build_queue.erase(id); this->build_since.erase(id); this->dirty_cursor = id.base();
			} else ++this->cursor;
			continue;
		}
		/* Atomic service tasks have a fixed upper envelope. A pathological list
		 * cannot stall the queue or allocate outside the per-service ceiling. */
		if (cooperative) {
			/* Initial preparation commits whole services. A suspended draft is
			 * absent from saves, so its allowance must be the same fresh service
			 * allowance after a reload, independent of earlier discovery work. */
			this->diagnostic.build_work += original - allowance;
			original = allowance = BUILD_WORK;
		}
		std::vector<const Vehicle *> fleet;
		bool passenger_fleet = false;
		size_t facility_envelope = 0;
		size_t work = 64 + list->GetNumOrders() * (NUM_CARGO + 32) * (_settings_game.linkgraph.passenger_journey_precomputed ? 2 : 1);
		size_t preflight_work = 64;
		if (work <= task_limit) for (const DispatchSchedule &schedule : list->GetScheduledDispatchScheduleSet()) {
			work += 16 + schedule.GetScheduledDispatch().size(); preflight_work += 16;
			if (work > task_limit) break;
		}
		if (work <= task_limit) {
			for (const Vehicle *v = list->GetFirstSharedVehicle(); v != nullptr; v = v->NextShared()) {
				if (cooperative) co_await std::suspend_always{};
				fleet.push_back(v);
				if (IsPassengerServiceVehicle(v) && !v->vehstatus.Any({VehState::Stopped, VehState::Crashed})) {
					facility_envelope += v->type == VehicleType::Train ? 2 * list->GetNumOrders() + 1 : 4096;
				}
				preflight_work += 32;
				work += 3 * NUM_CARGO + 32;
				for (const Vehicle *part = v; part != nullptr && work <= task_limit; part = part->Next()) {
					if (cooperative) co_await std::suspend_always{};
					work += 4; preflight_work += 4;
					passenger_fleet |= part->cargo_type < NUM_CARGO && part->cargo_cap != 0 && IsCargoInClass(part->cargo_type, CargoClass::Passengers);
				}
				if (work > task_limit) break;
			}
		}
		const size_t charged_work = work > task_limit ? std::min(task_limit, preflight_work) : work;
		/* A shared fleet must not disappear merely because its one-time capture
		 * exceeds an OD search budget. Admit one larger atomic generation task at
		 * a fresh tick boundary; ordinary packet/table lookups remain unchanged. */
		if ((charged_work > BUILD_WORK || facility_envelope > BUILD_WORK - std::min(charged_work, BUILD_WORK)) &&
				charged_work <= task_limit && allowance == original && task_limit > BUILD_WORK) {
			size_t needed = std::min(task_limit, charged_work + facility_envelope + BUILD_WORK);
			if (needed > allowance) { original += needed - allowance; allowance = needed; }
		}
		if (charged_work > allowance) break;
		std::sort(fleet.begin(), fleet.end(), [](const auto *a, const auto *b) { return a->index < b->index; });
		PassengerPeriodicService service; service.id = id;
		assert(this->generation != UINT64_MAX);
		service.incarnation = list->GetCargoDistRouteGeneration(); service.generation = this->generation + 1;
		if (work > task_limit) { ++this->diagnostic.rejected_work; allowance -= charged_work; }
		else {
			allowance -= work;
			bool retry_facilities = false;
			bool capture_work_limited = false;
			const bool facilities_fit_tick = work <= task_limit && facility_envelope <= task_limit - work;
			PassengerPeriodicTimetable::Failure failure;
			service.timetable = PassengerPeriodicTimetable::Compile(list->GetOrderVector(), &failure);
			if (_settings_game.linkgraph.passenger_journey_precomputed && service.timetable.period == 0 && passenger_fleet && !fleet.empty())
				service.timetable = PassengerPeriodicTimetable::CompileEstimated(list->GetOrderVector(), fleet.front()->type);
			if (passenger_fleet) {
				++this->diagnostic.compilation_attempts;
				this->diagnostic.compilation_successes += service.timetable.period != 0;
				++this->diagnostic.compilation_stops[static_cast<size_t>(failure)];
			}
			if (ServiceBytes(service) + this->building_bytes > MAX_BYTES) { service.timetable = {}; ++this->diagnostic.rejected_memory; }
			for (const Vehicle *v : fleet) {
				if (cooperative) co_await std::suspend_always{};
				if (service.timetable.stops.empty()) break;
				if (!IsPassengerServiceVehicle(v) || v->vehstatus.Any({VehState::Stopped, VehState::Crashed})) continue;
				if (v->type == VehicleType::Aircraft && Aircraft::From(v)->flags.Test(VehicleAirFlag::DestinationTooFar)) continue;
				PassengerPeriodicVehicle phase; phase.id = v->index; phase.type = v->type;
				for (const Vehicle *part = v; part != nullptr; part = part->Next()) {
					if (cooperative) co_await std::suspend_always{};
					if (part->cargo_type < NUM_CARGO) phase.capacity[part->cargo_type] += part->cargo_cap;
				}
				/* Capture an actual known order phase once. Subsequent movement,
				 * occupancy and lateness do not regenerate a future Visit column. */
				if (auto base = CapturePeriodicBase(v, service.timetable)) { phase.base = *base; phase.phase_known = true; }
				if (v->HasUnbunchingOrder()) phase.phase_known = false;
				/* A phase is trusted only for a certified live explicit order state.
				 * Facility checks are bounded and never forecast implicit stops. */
				size_t facility_work = std::min<size_t>(allowance, 4096);
				const size_t facility_allowance = facility_work;
				PassengerPredictionReason capture_reason{};
				auto state = GetPassengerRouteState(v, &facility_work, &capture_reason);
				bool unusable = state && facility_work != 0 && std::any_of(state->usable_stops.begin(), state->usable_stops.end(), [](uint8_t usable) { return usable == 0; });
				if (!state) {
					/* An unknown current occurrence is not a promise that every declared
					 * facility is usable. Check static vehicle/facility compatibility
					 * without needing a current phase. Exhaustion remains uncertainty. */
					for (const auto &stop : service.timetable.stops) {
						if (cooperative) co_await std::suspend_always{};
						if (facility_work == 0) break;
						if (!CanPassengerVehicleUseStation(v, stop.station, list->GetOrderAt(stop.order), facility_work) && facility_work != 0) { unusable = true; break; }
					}
				}
				allowance -= facility_allowance - facility_work;
				if (capture_reason == PassengerPredictionReason::Budget || facility_work == 0) {
					++this->diagnostic.phase_capture_work; capture_work_limited = true;
					if (facilities_fit_tick && facility_allowance < 4096) retry_facilities = true;
				}
				if (unusable) continue;
				if (!state || !std::all_of(state->usable_stops.begin(), state->usable_stops.end(), [](uint8_t usable) { return usable != 0; })) phase.phase_known = false;
				service.vehicles.push_back(phase);
			}
			if (retry_facilities) {
				/* A task started near the end of a tick must not permanently
				 * publish Unknown phases just because previous tasks used its
				 * facility allowance. Retry before publishing, with a fresh tick. */
				++this->generation;
				++this->diagnostic.rejected_work;
				this->build_queue.insert(id); this->build_since.try_emplace(id, since);
				break;
			}
			/* A fleet exceeding a full tick's certified envelope must not retry
			 * forever and starve discovery. Publish only individually verified
			 * phases; the remaining vehicles stay explicitly unknown. */
			if (capture_work_limited) ++this->diagnostic.rejected_work;
			std::sort(service.vehicles.begin(), service.vehicles.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
			bool dispatch = false;
			for (const Vehicle *v = list->GetFirstSharedVehicle(); v != nullptr; v = v->NextShared()) dispatch |= v->vehicle_flags.Test(VehicleFlag::ScheduledDispatch);
			if (dispatch) {
				service.uncertain_dispatch_period = service.timetable.period;
				for (const auto &stop : service.timetable.stops) {
					if (cooperative) co_await std::suspend_always{};
					int assigned = list->GetOrderAt(stop.order)->GetDispatchScheduleIndex();
					if (assigned >= 0 && static_cast<size_t>(assigned) < list->GetScheduledDispatchScheduleCount()) {
						int64_t duration = list->GetDispatchScheduleByIndex(assigned).GetScheduledDispatchDuration();
						service.uncertain_dispatch_period = std::max(service.uncertain_dispatch_period, std::clamp<int64_t>(duration, 0, MAX_PERIOD));
					}
				}
				bool physical_phase_known = service.vehicles.size() == 1 && service.vehicles[0].phase_known;
				/* Multiple vehicles have unresolved slot ownership. No fleet headway,
				 * synthetic vehicle or optimistic future dispatch is manufactured. */
				for (auto &phase : service.vehicles) phase.phase_known = false;
				int dispatch_index = -1;
				for (size_t i = 0; i < service.timetable.stops.size(); ++i) {
					if (cooperative) co_await std::suspend_always{};
					int assigned = list->GetOrderAt(service.timetable.stops[i].order)->GetDispatchScheduleIndex();
					if (assigned < 0) continue;
					if (dispatch_index != -1) { dispatch_index = -2; break; }
					dispatch_index = assigned; service.dispatch_stop = i;
				}
				if (physical_phase_known && list->GetNumVehicles() == 1 && service.vehicles.size() == 1 && dispatch_index >= 0 &&
						static_cast<size_t>(dispatch_index) < list->GetScheduledDispatchScheduleCount()) {
					const auto &schedule = list->GetDispatchScheduleByIndex(dispatch_index);
					int64_t duration = schedule.GetScheduledDispatchDuration();
					bool valid = duration > 0;
					service.dispatch_floor = schedule.GetScheduledDispatchStartTick();
					if (!schedule.GetScheduledDispatchReuseSlots() && schedule.GetScheduledDispatchLastDispatch() != INVALID_SCHEDULED_DISPATCH_OFFSET) {
						int64_t base = schedule.GetScheduledDispatchStartTick().base(), last = schedule.GetScheduledDispatchLastDispatch();
						if ((last >= 0 && !AddTicks(base, last)) || (last < 0 && base < INT64_MIN - last)) valid = false;
						else {
							if (last < 0) base += last;
							service.dispatch_floor = StateTicks{base};
							const Vehicle *v = list->GetFirstSharedVehicle();
							if (base > _state_ticks.base() && (!v->current_order.IsAnyLoadingType() || v->cur_real_order_index != service.timetable.stops[service.dispatch_stop].order)) valid = false;
						}
					}
					for (const auto &slot : schedule.GetScheduledDispatch()) {
						if (cooperative) co_await std::suspend_always{};
						if (slot.offset >= duration || slot.flags != 0) valid = false;
						service.dispatch_slots.push_back(slot.offset);
					}
					if (service.dispatch_slots.empty()) valid = false;
					for (size_t i = 0; i < service.dispatch_slots.size(); ++i) {
						if (cooperative) co_await std::suspend_always{};
						int64_t next = i + 1 < service.dispatch_slots.size() ? service.dispatch_slots[i + 1] : duration + service.dispatch_slots[0];
						if (next - service.dispatch_slots[i] < service.timetable.period) valid = false;
					}
					if (valid) { service.dispatch_base = schedule.GetScheduledDispatchStartTick(); service.dispatch_period = duration; service.vehicles[0].phase_known = true; }
					else service.dispatch_slots.clear();
				}
			}
			bool passengers = false;
			for (const auto &vehicle : service.vehicles) for (CargoType cargo : EnumRange(NUM_CARGO)) passengers |= vehicle.capacity[cargo] != 0 && IsCargoInClass(cargo, CargoClass::Passengers);
			if (!passengers) {
				service.timetable = {}; service.dispatch_period = 0; service.dispatch_slots.clear();
				for (auto &vehicle : service.vehicles) vehicle.phase_known = false;
			}
		}
		if (from_dirty) { this->build_queue.erase(id); this->build_since.erase(id); this->dirty_cursor = id.base(); } else ++this->cursor;
		++this->generation;
		if (ServiceBytes(service) + this->building_bytes > MAX_BYTES) { ++this->diagnostic.rejected_memory; continue; }
		this->building_bytes += ServiceBytes(service);
		this->building.emplace(id, std::move(service)); this->staged_since.emplace(id, since); ++this->diagnostic.builds;
		/* Initial discovery must make the same usable prefixes available as the
		 * previous scheduler. Later refreshes use the frozen publication barrier. */
		if (this->bootstrap) this->Publish(allowance);
		if (cooperative) break;
	}
	if (!this->sweeping && this->build_queue.empty()) { this->bootstrap = false; this->publishing = !this->building.empty(); }
	if (this->publishing) this->Publish(allowance);
	this->diagnostic.build_work += original - allowance;
	if (build_precomputed && _settings_game.linkgraph.passenger_journey_precomputed) this->TickPrecomputed();
}

bool PassengerServiceDatabase::IsPreparingPrecomputed() const
{
	return _settings_game.linkgraph.passenger_journey_routing && _settings_game.linkgraph.passenger_journey_precomputed &&
			(this->load_state == LoadState::Replaying ||
			std::any_of(this->precomputed.begin(), this->precomputed.end(), [](const auto &table) { return table.IsPreparingGraph(); }) ||
			(this->retained_precomputed_revision == UINT64_MAX && !this->PrecomputedFinished() &&
			(!this->services.empty() || this->sweeping || !this->build_queue.empty() || !this->building.empty())));
}

PassengerPrecomputedProgress PassengerServiceDatabase::GetPrecomputedProgress() const
{
	using Phase = PassengerPrecomputedProgress::Phase;
	PassengerPrecomputedProgress result;
	if (!_settings_game.linkgraph.passenger_journey_routing || !_settings_game.linkgraph.passenger_journey_precomputed) return result;
	result.preparing = this->IsPreparingPrecomputed();
	if (this->load_state == LoadState::Invalid) { result.phase = Phase::Invalid; return result; }
	if (this->precomputed_revision == UINT64_MAX) {
		result.phase = result.preparing ? Phase::Services : Phase::Complete;
		return result;
	}
	/* Only current-generation public rows count. In particular, speculative
	 * worker results and retained rows must not inflate this fraction. During
	 * replay the retained generation is restored first; its Work is not a
	 * percentage of the current generation's destination tables. */
	bool graph = this->load_state == LoadState::Replaying && !this->load_current_graph_started;
	bool rejected = false;
	for (const auto &table : this->precomputed) {
		graph |= table.IsPreparingGraph();
		rejected |= table.MemoryRejected();
		const auto &diagnostic = table.GetDiagnostic();
		result.destinations += diagnostic.destinations;
		result.stations += diagnostic.stations;
	}
	if (this->load_state == LoadState::Replaying) {
		for (const auto &table : this->retained_precomputed) {
			graph |= table.IsPreparingGraph();
			rejected |= table.MemoryRejected();
		}
	}
	const bool restoring = this->load_state == LoadState::Replaying && this->load_retained;
	result.total_known = !graph && !restoring;
	result.phase = rejected ? Phase::MemoryRejected : graph ? Phase::Graph : restoring ? Phase::Restoring :
			result.preparing ? Phase::Tables : Phase::Complete;
	return result;
}

bool PassengerServiceDatabase::PreparePrecomputed()
{
	if (!this->IsPreparingPrecomputed()) return true;
	PassengerJourneyWorkTimer timer;
	const auto started = std::chrono::steady_clock::now();
	/* Complete initial discovery before freezing its input. Subsequent timing
	 * dirtiness is deliberately left to the ordinary publication epochs. */
	if (this->load_state == LoadState::Replaying) {
		if (this->StepLoad() == LoadState::Invalid) SlErrorCorrupt("Invalid passenger routing construction cursor");
	} else if (this->retained_precomputed_revision != UINT64_MAX) {
		/* A normal replacement pauses only its graph preparation. Keep every
		 * old row; row publication resumes at the ordinary saved tick budget. */
		for (auto &table : this->precomputed) {
			if (table.IsPreparingGraph() && !table.PrepareGraph(1024)) break;
			/* The frozen graph can already warm its local cache while the UI /
			 * network preparation barrier returns control. Consume no row Work. */
			if (!table.Finished()) table.StepParallel(0, _settings_client.gui.passenger_precomputed_workers);
		}
	} else if (this->preparation_capture || (this->precomputed_revision == UINT64_MAX && (this->sweeping || !this->build_queue.empty() || !this->building.empty()))) {
		if (!this->preparation_capture) this->preparation_capture = this->TickBuild(BUILD_WORK, false, true);
		if (this->preparation_capture.Resume(64)) this->preparation_capture = {};
		this->diagnostic.build_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
	} else {
		this->cooperative_graph = true;
		this->TickPrecomputed(BUILD_WORK);
		this->cooperative_graph = false;
	}
	this->precomputed_preparation_nanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
	return !this->IsPreparingPrecomputed();
}

void PassengerServiceDatabase::BeginPrecomputed(int64_t transfer)
{
	++this->hyperpath_lookup_epoch;
	++this->boarding_revision;
	/* A replacement owns only its new rows. Retire old rows as soon as their
	 * replacements are public; never allocate a second complete label matrix.
	 * Structural edits arriving during a build are coalesced until it finishes,
	 * bounding residency to two graphs and at most one row per generation/OD. */
	if (this->precomputed_revision != UINT64_MAX && !this->HasRejectedPrecomputedMemory()) {
		this->ClearRetainedPrecomputed();
		this->retained_precomputed = std::move(this->precomputed);
		this->retained_precomputed_services = std::move(this->precomputed_services);
		this->retained_precomputed_revision = this->precomputed_revision;
		this->retained_precomputed_transfer = this->precomputed_transfer;
		this->retained_precomputed_aircraft_cost = this->precomputed_aircraft_cost;
		this->retained_precomputed_hyperpath = this->precomputed_hyperpath;
		this->retained_precomputed_policy_generation = this->precomputed_policy_generation;
	} else {
		for (CargoType cargo : EnumRange(NUM_CARGO)) {
			/* Reuse may discard retired destinations only after this cargo's
			 * replacement has completed (including a memory rejection). */
			assert(this->precomputed[cargo].Finished());
			this->retained_precomputed[cargo].ReuseRetainedDestinations();
		}
	}
	this->precomputed_hyperpath = _settings_game.linkgraph.passenger_journey_hyperpath;
	if (this->precomputed_policy_generation == UINT64_MAX) FatalError("Passenger policy generation exhausted.");
	++this->precomputed_policy_generation;
	this->precomputed_revision = this->published_revision;
	this->precomputed_transfer = transfer; this->precomputed_invalidated = false;
	this->precomputed_aircraft_cost = this->aircraft_cost;
	int64_t refresh = _state_ticks.base();
	if (!AddTicks(refresh, TIMETABLE_EPOCH)) refresh = INT64_MAX;
	this->precomputed_refresh = StateTicks{refresh};
	this->precomputed_services = this->services;
	this->BuildPrecomputedTables();
	/* Timing-only regrouping can change canonical boarding keys even when the
	 * live revision and ready-row count are unchanged at the end of this tick. */
	InvalidatePassengerWaitingRouting();
}

void PassengerServiceDatabase::BuildPrecomputedTables()
{
	this->precomputed = {}; this->precomputed_operations = 0;
	/* A mixed replacement still owns the larger strategy generation. Switching
	 * back must not strand it behind the non-Hyperpath combined envelope. */
	uint64_t remaining = this->precomputed_hyperpath || this->retained_precomputed_hyperpath ? PassengerPrecomputedTable::MAX_HYPERPATH_BYTES : PassengerPrecomputedTable::MAX_BYTES;
	for (const auto &table : this->retained_precomputed) remaining -= std::min(remaining, table.GetDiagnostic().envelope);
	for (const auto &[id, service] : this->retained_precomputed_services) {
		uint64_t bytes = 512 + uint64_t{256} * service.timetable.stops.size() + uint64_t{1024} * service.vehicles.size() + uint64_t{32} * service.dispatch_slots.size();
		remaining -= std::min(remaining, bytes);
	}
	this->BuildPrecomputedTables(this->precomputed, this->precomputed_services, this->precomputed_transfer, remaining, this->precomputed_hyperpath, this->cooperative_graph, this->precomputed_aircraft_cost);
}

void PassengerServiceDatabase::BuildPrecomputedTables(std::array<PassengerPrecomputedTable, NUM_CARGO> &tables,
		const std::map<OrderListID, PassengerPeriodicService> &snapshot, int64_t transfer, uint64_t remaining, bool hyperpath, bool cooperative, PassengerAircraftCost aircraft_cost)
{
	std::vector<const PassengerPeriodicService *> records;
	for (const auto &[id, service] : snapshot) {
		records.push_back(&service);
		/* Fixed charges for the additional saved snapshot, independent of host ABI. */
		uint64_t bytes = 512 + uint64_t{256} * service.timetable.stops.size() + uint64_t{1024} * service.vehicles.size() + uint64_t{32} * service.dispatch_slots.size();
		remaining -= std::min<uint64_t>(remaining, bytes);
	}
	auto memory_budget = cooperative ? std::make_shared<uint64_t>(remaining) : nullptr;
	for (CargoType cargo : EnumRange(NUM_CARGO)) {
		if (!IsCargoInClass(cargo, CargoClass::Passengers)) continue;
		tables[cargo].Begin(records, cargo, transfer, remaining, true, cooperative, memory_budget, true, true, true, hyperpath, true, true, true, aircraft_cost);
		remaining -= std::min(remaining, tables[cargo].GetDiagnostic().envelope);
	}
}

void PassengerServiceDatabase::ClearRetainedPrecomputed()
{
	/* Called every tick while the current generation is finished. Without a retained revision
	 * or generation no lookup consults the retained tables, so only an effective clear changes
	 * station lookups. */
	if (this->retained_precomputed_revision != UINT64_MAX || this->retained_precomputed_policy_generation != 0) ++this->hyperpath_lookup_epoch;
	this->retained_precomputed = {};
	this->retained_precomputed_services.clear();
	this->retained_precomputed_revision = UINT64_MAX;
	this->retained_precomputed_transfer = 0;
	this->retained_precomputed_hyperpath = false; this->retained_precomputed_policy_generation = 0;
}

bool PassengerServiceDatabase::UpdateAircraftCosts()
{
	const auto cost = PassengerAircraftCost::Current();
	if (this->restoring || this->load_state == LoadState::Replaying || cost == this->aircraft_cost) return false;
	/* Cancel graph coroutines and speculative workers before replacing their inputs.
	 * No old-cost rows remain eligible, even during a cooperative rebuild. */
	this->DiscardPrecomputed();
	this->aircraft_cost = cost;
	for (auto &[id, service] : this->services) service.BuildAircraftCosts(cost);
	this->ClearPolicies(PassengerDatabaseDiagnostic::ClearReason::Explicit);
	InvalidatePassengerWaitingRouting();
	return true;
}

void PassengerServiceDatabase::DiscardPrecomputed()
{
	++this->hyperpath_lookup_epoch;
	this->preparation_capture = {};
	++this->boarding_revision;
	this->ClearRetainedPrecomputed();
	this->precomputed = {}; this->precomputed_services.clear();
	this->precomputed_revision = UINT64_MAX; this->precomputed_operations = 0; this->precomputed_invalidated = false;
}

bool PassengerServiceDatabase::PrecomputedFinished() const
{
	return this->precomputed_revision != UINT64_MAX &&
			std::all_of(this->precomputed.begin(), this->precomputed.end(), [](const auto &t) { return t.Finished(); });
}

void PassengerServiceDatabase::TickPrecomputed(size_t allowance)
{
	/* Startup callbacks may run before a deferred load has replayed its saved
	 * frontier. Only StepLoad may advance it; starting a replacement here would
	 * retain an empty local table which a joining peer reconstructs in full. */
	if (this->load_state == LoadState::Replaying) return;
	/* Timing refresh queues need not become empty on a busy map. Build from a
	 * saved immutable publication snapshot and never restart it for time drift.
	 * Actual order/boarding changes still restart; cost-only changes coalesce. */
	if (this->precomputed_revision == UINT64_MAX && this->services.empty()) return;
	this->UpdateAircraftCosts();
	const bool finished = std::all_of(this->precomputed.begin(), this->precomputed.end(), [](const auto &t) { return t.Finished(); });
	if (this->precomputed_revision == UINT64_MAX || (finished && (this->precomputed_hyperpath != _settings_game.linkgraph.passenger_journey_hyperpath || this->precomputed_invalidated ||
			this->precomputed_transfer != _settings_game.linkgraph.passenger_transfer_penalty ||
			(_state_ticks >= this->precomputed_refresh && this->precomputed_revision != this->published_revision))))
		this->BeginPrecomputed(_settings_game.linkgraph.passenger_transfer_penalty);
	const bool had_work = std::any_of(this->precomputed.begin(), this->precomputed.end(), [](const auto &t) { return !t.Finished(); });
	for (CargoType cargo : EnumRange(NUM_CARGO)) {
		auto &table = this->precomputed[cargo];
		if (table.Finished()) continue;
		const auto rows = table.GetDiagnostic().destinations;
		auto published = [&](StationID destination) {
			auto &retained = this->retained_precomputed[cargo];
			retained.PrepareRetirement(table); retained.ReleaseDestination(destination);
			this->BumpHyperpathDestination(destination);
		};
		/* Speculation is a local CPU cache in initial and replacement builds.
		 * A miss executes the same serial search; BUILD_WORK and all publication
		 * decisions are independent of completion order or local worker count. */
		size_t used = table.StepParallel(allowance, _settings_client.gui.passenger_precomputed_workers, published);
		if (table.GetDiagnostic().destinations != rows) WakePassengerWaitingCargo(cargo);
		allowance -= used; this->precomputed_operations += used;
		if (allowance == 0 || table.IsPreparingGraph()) break;
	}
	/* A national build may take longer than an epoch. Retain its completed
	 * rows for a full epoch instead of discarding them on the following tick. */
	if (had_work && std::all_of(this->precomputed.begin(), this->precomputed.end(), [](const auto &t) { return t.Finished(); })) {
		int64_t refresh = _state_ticks.base();
		if (!AddTicks(refresh, TIMETABLE_EPOCH)) refresh = INT64_MAX;
		this->precomputed_refresh = std::max(this->precomputed_refresh, StateTicks{refresh});
	}
	/* A deterministic memory rejection must keep a usable old generation.
	 * Otherwise all old rows have replacements and their graph can be freed. */
	if (this->PrecomputedFinished() && !this->HasRejectedPrecomputedMemory()) this->ClearRetainedPrecomputed();
}

PassengerServiceDatabase::BoardingKeys PassengerServiceDatabase::PrecomputedBoardingKeys(CargoType cargo, OrderListID service, VehicleOrderID boarding) const
{
	BoardingKeys keys{};
	if (cargo >= NUM_CARGO) return keys;
	keys.services = {this->precomputed[cargo].BoardingServices(service), this->retained_precomputed[cargo].BoardingServices(service)};
	keys.sections = {this->precomputed[cargo].SectionBoardingKeys(service, boarding), this->retained_precomputed[cargo].SectionBoardingKeys(service, boarding)};
	return keys;
}

bool PassengerServiceDatabase::RebindPrecomputed(PassengerJourneyExecution &ride, OrderListID service, CargoType cargo, VehicleOrderID boarding) const
{
	if (cargo >= NUM_CARGO || !this->IsCurrent(ride)) return false;
	for (const auto *table : {&this->precomputed[cargo], &this->retained_precomputed[cargo]}) {
		auto candidate = ride;
		if (table->Rebind(candidate, service, boarding) && this->IsCurrent(candidate)) { ride = candidate; return true; }
	}
	return false;
}

/** A shared prefix may bind the very carrier needed by its planned suffix.
 * Alighting and then excluding that carrier can manufacture a pointless return
 * trip. Compare staying aboard with the frozen cost of the onward transfer.
 * Only the actual service's compiled cycle is inspected, once per loading cohort;
 * live orders, facilities, forced unload and generation are checked by boarding. */
std::optional<PassengerJourneyExecution> PassengerServiceDatabase::ExtendPrecomputedRide(
		const PassengerJourneyExecution &ride, CargoType cargo, StationID destination) const
{
	if (cargo >= NUM_CARGO || ride.kind != PassengerRideKind::EstimatedServiceIntent || ride.alighting == destination ||
			!Station::IsValidID(destination) || !this->IsCurrent(ride)) return {};
	const uint64_t mask = uint64_t{1} << cargo;
	for (bool retained : {false, true}) {
		const auto &snapshot = retained ? this->retained_precomputed_services : this->precomputed_services;
		const auto &table = (retained ? this->retained_precomputed : this->precomputed)[cargo];
		auto suffix = table.RouteCost(ride.alighting, destination);
		if (!suffix) continue; // Do not replace missing construction work with a search.
		auto found = snapshot.find(ride.service);
		if (found == snapshot.end() || found->second.incarnation != ride.signature) continue;
		const auto &t = found->second.timetable;
		auto first = std::find_if(t.stops.begin(), t.stops.end(), [&](const auto &stop) {
			return stop.order == ride.boarding_order && stop.station == ride.boarding;
		});
		if (first == t.stops.end() || t.Circuit() <= 0 || !(first->board & mask)) continue;
		size_t previous = first - t.stops.begin();
		int64_t cost = 0, prefix = -1;
		for (size_t step = 1; step < t.stops.size(); ++step) {
			const auto &from = t.stops[previous];
			if (!from.continues || (from.forced & mask)) break;
			size_t next = (previous + 1) % t.stops.size(); const auto &to = t.stops[next];
			int64_t travel = std::max<int64_t>(1, to.arrival - from.departure + (next == 0 ? t.Circuit() : 0));
			int64_t uncertainty = to.travel_quality == 1 ? (travel + 7) / 8 : to.travel_quality == 2 ? (travel + 3) / 4 : 0;
			travel += found->second.AircraftAdjustment(previous, next);
			if (!AddTicks(cost, travel) || !AddTicks(cost, uncertainty)) break;
			if (prefix < 0 && to.station == destination && (to.alight & mask)) break;
			if (prefix >= 0 && to.station == destination && (to.alight & mask)) {
				if (*suffix == INT64_MAX || prefix > INT64_MAX - *suffix || cost <= prefix + *suffix) {
					auto extended = ride; extended.alighting = destination; extended.alighting_order = to.order;
					return extended;
				}
				break; // A later circuit occurrence cannot make staying aboard faster.
			}
			if (to.order == ride.alighting_order && to.station == ride.alighting && (to.alight & mask)) prefix = cost;
			if (!AddTicks(cost, to.departure - to.arrival)) break;
			previous = next;
		}
	}
	return {};
}

PassengerPrecomputedPlan PassengerServiceDatabase::PlanPrecomputed(CargoType cargo, StationID origin, StationID destination,
		StateTicks ready, OrderListID excluded, uint32_t sample, OrderListID failed) const
{
	if (cargo >= NUM_CARGO || this->precomputed_revision == UINT64_MAX) return {};
	auto result = this->precomputed[cargo].Plan(origin, destination, ready, excluded, sample, failed);
	if (result.hyperpath && result.ride) result.ride->generation = this->precomputed_policy_generation;
	if ((result.state == PassengerPrecomputedPlan::State::Building || result.state == PassengerPrecomputedPlan::State::Memory ||
			result.state == PassengerPrecomputedPlan::State::Unsupported) && this->retained_precomputed_revision != UINT64_MAX) {
		auto retained = this->retained_precomputed[cargo].Plan(origin, destination, ready, excluded, sample, failed);
		if (retained.hyperpath && retained.ride) retained.ride->generation = this->retained_precomputed_policy_generation;
		if (retained.ride) result = std::move(retained);
	}
	if (result.state == PassengerPrecomputedPlan::State::NoRoute && this->IsBuilding()) result.state = PassengerPrecomputedPlan::State::Building;
	if (result.ride && !this->IsCurrent(*result.ride)) { result.ride.reset(); result.state = PassengerPrecomputedPlan::State::Building; }
	return result;
}

std::vector<PassengerPrecomputedTable::RouteCandidate> PassengerServiceDatabase::InspectRoutes(CargoType cargo,
		StationID origin, StationID destination, OrderListID previous, OrderListID failed, size_t &work, const PassengerJourneyExecution *intent) const
{
	using State = PassengerPrecomputedPlan::State;
	if (cargo >= NUM_CARGO || this->precomputed_revision == UINT64_MAX) return {};
	const PassengerPrecomputedTable *table = &this->precomputed[cargo];
	/* A waiting Hyperpath cohort belongs to exactly one generation. Mixing the
	 * two policies would count the same departure streams twice. */
	if (intent != nullptr && intent->kind == PassengerRideKind::HyperpathIntent && this->IsCurrent(*intent)) {
		if (intent->signature != uint64_t(cargo) + 1 || intent->boarding != origin || intent->alighting != destination) return {};
		if (intent->generation == this->retained_precomputed_policy_generation) table = &this->retained_precomputed[cargo];
	}
	auto result = table->InspectRoutes(origin, destination, previous, failed, work);
	if (table == &this->precomputed[cargo] && (result.state == State::Building || result.state == State::Memory || result.state == State::Unsupported) &&
			this->retained_precomputed_revision != UINT64_MAX) {
		table = &this->retained_precomputed[cargo];
		result = table->InspectRoutes(origin, destination, previous, failed, work);
	}
	std::vector<PassengerPrecomputedTable::RouteCandidate> routes;
	for (auto &candidate : result.candidates) {
		if (!this->IsCurrent(candidate.ride)) {
			bool found = false;
			for (OrderListID service : table->BoardingServices(candidate.ride.service)) {
				if (work == 0) break;
				--work;
				auto rebound = candidate.ride;
				if (table->Rebind(rebound, service) && this->IsCurrent(rebound)) { candidate.ride = rebound; found = true; break; }
			}
			if (!found) continue;
		}
		/* Shared-section legacy rows can select a carrier which also operates the
		 * onward leg. Reuse the read-only through-ride check, under the GUI budget. */
		if (!table->IsHyperpath() && candidate.ride.alighting != destination) {
			const auto *service = this->Get(candidate.ride.service);
			if (service != nullptr && service->timetable.stops.size() <= work / 4) {
				work -= service->timetable.stops.size() * 4;
				if (auto extended = this->ExtendPrecomputedRide(candidate.ride, cargo, destination)) candidate.ride = *extended;
			}
		}
		routes.push_back(std::move(candidate));
	}
	return routes;
}

std::optional<int64_t> PassengerServiceDatabase::HyperpathServiceWait(const PassengerJourneyExecution &intent, CargoType cargo, OrderListID service) const
{
	if (cargo >= NUM_CARGO || intent.kind != PassengerRideKind::HyperpathIntent || intent.signature != uint64_t(cargo) + 1) return {};
	if (intent.generation == this->precomputed_policy_generation) return this->precomputed[cargo].ServiceWait(service);
	if (intent.generation == this->retained_precomputed_policy_generation) return this->retained_precomputed[cargo].ServiceWait(service);
	return {};
}

std::array<uint32_t, 2> PassengerServiceDatabase::HyperpathBoardingOccurrences(CargoType cargo, StationID origin, OrderListID service, VehicleOrderID order) const
{
	if (cargo >= NUM_CARGO) return {PassengerPrecomputedTable::INVALID, PassengerPrecomputedTable::INVALID};
	return {this->precomputed[cargo].BoardingOccurrence(origin, service, order), this->retained_precomputed[cargo].BoardingOccurrence(origin, service, order)};
}

bool PassengerServiceDatabase::MayBoardHyperpath(const PassengerJourneyExecution &intent, CargoType cargo,
		const std::array<uint32_t, 2> &occurrences, OrderListID previous, OrderListID failed, bool cached_only) const
{
	if (cached_only) return this->CachedHyperpathMembership(intent, cargo, occurrences, previous, failed).value_or(true);
	if (cargo >= NUM_CARGO || intent.kind != PassengerRideKind::HyperpathIntent || intent.signature != uint64_t(cargo) + 1) return false;
	for (size_t index = 0; index < 2; ++index) {
		if (intent.generation != (index != 0 ? this->retained_precomputed_policy_generation : this->precomputed_policy_generation)) continue;
		const auto &table = (index != 0 ? this->retained_precomputed : this->precomputed)[cargo];
		const auto candidates = table.Candidates(intent.boarding, intent.alighting, previous, failed);
		return std::binary_search(candidates.begin(), candidates.end(), occurrences[index]);
	}
	return false;
}

const PassengerPrecomputedPlan *PassengerServiceDatabase::SharedHyperpathPlan(CargoType cargo, StationID origin, StationID destination, OrderListID previous) const
{
	if (this->RevisionAffectsWaitingPlans()) return nullptr;
	const uint64_t changed = destination.base() < this->hyperpath_destination_changed.size() ? this->hyperpath_destination_changed[destination.base()] : 0;
	const auto key = std::tuple(cargo, origin, destination, previous);
	auto found = this->shared_hyperpath_plans.find(key);
	if (found != this->shared_hyperpath_plans.end() && found->second.epoch == this->hyperpath_lookup_epoch && found->second.changes >= changed) return &found->second.plan;
	if (found == this->shared_hyperpath_plans.end()) {
		if (this->shared_hyperpath_plans.size() >= 32768) {
			/* Evict the next key, not always the lowest stations. Residency never changes an identity. */
			auto victim = this->shared_hyperpath_plans.upper_bound(key);
			if (victim == this->shared_hyperpath_plans.end()) victim = this->shared_hyperpath_plans.begin();
			this->shared_hyperpath_plans.erase(victim);
		}
		found = this->shared_hyperpath_plans.try_emplace(key).first;
	}
	found->second.epoch = this->hyperpath_lookup_epoch; found->second.changes = this->hyperpath_destination_changes;
	found->second.plan = this->PlanPrecomputed(cargo, origin, destination, _state_ticks, previous);
	return &found->second.plan;
}

std::optional<bool> PassengerServiceDatabase::CachedHyperpathMembership(const PassengerJourneyExecution &intent, CargoType cargo,
		const std::array<uint32_t, 2> &occurrences, OrderListID previous, OrderListID failed) const
{
	if (cargo >= NUM_CARGO || intent.kind != PassengerRideKind::HyperpathIntent || intent.signature != uint64_t(cargo) + 1) return false;
	for (size_t index = 0; index < 2; ++index) {
		if (intent.generation != (index != 0 ? this->retained_precomputed_policy_generation : this->precomputed_policy_generation)) continue;
		const auto &table = (index != 0 ? this->retained_precomputed : this->precomputed)[cargo];
		return table.ContainsCached(intent.boarding, intent.alighting, occurrences[index], previous, failed);
	}
	return false;
}

int PassengerServiceDatabase::HyperpathLookupCandidates(CargoType cargo, const PassengerJourneyExecution &intent, OrderListID arriving, uint64_t incarnation,
		StationID station, std::vector<uint32_t> &out) const
{
	/* Mirrors PassengerJourneyLoadingContext::MayUseHyperpathPolicy's base membership: the
	 * intent itself while current, otherwise the plan that replaces it. An arriving carrier can
	 * only become invalid, which selects the base state, so a transfer group takes the union of
	 * both states. Failed-carrier groups never come here; they are always checked. */
	out.clear();
	if (cargo >= NUM_CARGO || intent.kind != PassengerRideKind::HyperpathIntent || intent.boarding != station) return HYPERPATH_LOOKUP_NONE;
	const auto *list = OrderList::GetIfValid(arriving);
	const OrderListID previous = list != nullptr && list->GetCargoDistRouteGeneration() == incarnation ? list->index : OrderListID::Invalid();
	auto slot_of = [&](const PassengerJourneyExecution &policy) {
		if (policy.signature != uint64_t(cargo) + 1 || policy.boarding != station) return HYPERPATH_LOOKUP_NONE;
		if (policy.generation == this->precomputed_policy_generation) return 0;
		if (policy.generation == this->retained_precomputed_policy_generation) return 1;
		return HYPERPATH_LOOKUP_NONE;
	};
	auto collect = [&](int slot, const PassengerJourneyExecution &policy) {
		const auto &table = (slot != 0 ? this->retained_precomputed : this->precomputed)[cargo];
		table.StoredCandidates(policy.boarding, policy.alighting, previous, out);
		if (previous != OrderListID::Invalid()) table.StoredCandidates(policy.boarding, policy.alighting, OrderListID::Invalid(), out);
	};
	int slot = HYPERPATH_LOOKUP_NONE;
	if (this->IsCurrent(intent)) {
		slot = slot_of(intent);
		if (slot != HYPERPATH_LOOKUP_NONE) collect(slot, intent);
	} else {
		/* A legacy plan's currency also depends on its service, which no lookup change tracks. */
		if ((this->precomputed_revision != UINT64_MAX && !this->precomputed_hyperpath) ||
				(this->retained_precomputed_revision != UINT64_MAX && !this->retained_precomputed_hyperpath)) return HYPERPATH_LOOKUP_ALWAYS;
		const std::array<OrderListID, 2> states{previous, OrderListID::Invalid()};
		for (size_t i = 0; i < (previous == OrderListID::Invalid() ? 1 : 2); ++i) {
			/* Only the replacement's identity is read: shared across ticks and stations. */
			PassengerPrecomputedPlan own;
			const auto *shared = this->SharedHyperpathPlan(cargo, intent.boarding, intent.alighting, states[i]);
			if (shared == nullptr) { own = this->PlanPrecomputed(cargo, intent.boarding, intent.alighting, _state_ticks, states[i]); shared = &own; }
			const auto &plan = *shared;
			if (!plan.ride) continue;
			if (plan.ride->kind != PassengerRideKind::HyperpathIntent) { out.clear(); return HYPERPATH_LOOKUP_ALWAYS; }
			const int found = slot_of(*plan.ride);
			if (found == HYPERPATH_LOOKUP_NONE) continue;
			if (slot != HYPERPATH_LOOKUP_NONE && slot != found) { out.clear(); return HYPERPATH_LOOKUP_ALWAYS; }
			slot = found; collect(slot, *plan.ride);
		}
	}
	std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
	return out.empty() ? HYPERPATH_LOOKUP_NONE : slot;
}

PassengerPrecomputedTable::DisplayRead PassengerServiceDatabase::NextHyperpathDisplayCandidate(CargoType cargo,
		const PassengerJourneyExecution &intent, OrderListID previous, OrderListID failed, PassengerPrecomputedTable::DisplayCursor &cursor,
		PassengerPrecomputedTable::DisplayCandidate &out, size_t &work) const
{
	using Read = PassengerPrecomputedTable::DisplayRead;
	if (cargo >= NUM_CARGO || intent.kind != PassengerRideKind::HyperpathIntent || intent.signature != uint64_t(cargo) + 1 || !this->IsCurrent(intent)) return Read::Unavailable;
	bool changed = cursor.cargo != cargo || cursor.epoch != this->HyperpathLookupEpoch() || cursor.generation != intent.generation ||
			this->HyperpathDestinationChanges() - cursor.changes > HYPERPATH_DESTINATION_LOG;
	if (!changed) for (uint64_t change = cursor.changes; change < this->HyperpathDestinationChanges(); ++change) {
		if (this->HyperpathChangedDestination(change) == intent.alighting) { changed = true; break; }
	}
	if (changed) cursor = {};
	cursor.cargo = cargo; cursor.epoch = this->HyperpathLookupEpoch(); cursor.generation = intent.generation; cursor.changes = this->HyperpathDestinationChanges();
	const auto &table = intent.generation == this->precomputed_policy_generation ? this->precomputed[cargo] : this->retained_precomputed[cargo];
	return table.NextDisplayCandidate(intent.boarding, intent.alighting, previous, failed, cursor, out, work);
}

bool PassengerServiceDatabase::HyperpathDisplayCandidates(CargoType cargo, const PassengerJourneyExecution &intent,
		OrderListID previous, OrderListID failed, std::vector<PassengerPrecomputedTable::DisplayCandidate> &out, size_t &work) const
{
	out.clear();
	if (cargo >= NUM_CARGO || intent.kind != PassengerRideKind::HyperpathIntent || intent.signature != uint64_t(cargo) + 1 ||
			!this->IsCurrent(intent)) return false;
	const auto &table = intent.generation == this->precomputed_policy_generation ? this->precomputed[cargo] : this->retained_precomputed[cargo];
	return table.DisplayCandidates(intent.boarding, intent.alighting, previous, failed, out, work);
}

PassengerPrecomputedPlan PassengerServiceDatabase::BoardHyperpath(const PassengerJourneyExecution &intent, CargoType cargo,
		OrderListID service, VehicleOrderID boarding, OrderListID previous, OrderListID failed) const
{
	if (cargo >= NUM_CARGO || intent.kind != PassengerRideKind::HyperpathIntent || intent.signature != uint64_t(cargo) + 1) return {};
	for (bool retained : {false, true}) {
		if (intent.generation != (retained ? this->retained_precomputed_policy_generation : this->precomputed_policy_generation)) continue;
		const auto &table = (retained ? this->retained_precomputed : this->precomputed)[cargo];
		auto plan = table.Board(intent.boarding, intent.alighting, intent.not_before, service, boarding, previous, failed);
		if (plan.ride && !this->IsCurrent(*plan.ride)) { plan.ride.reset(); plan.state = PassengerPrecomputedPlan::State::Building; }
		return plan;
	}
	return {};
}

void PassengerServiceDatabase::Publish(size_t &allowance)
{
	const auto start = std::chrono::steady_clock::now();
	bool changed = false;
	while (!this->building.empty()) {
		auto it = this->building.begin(); const OrderListID id = it->first;
		auto &service = it->second; const size_t staged_bytes = ServiceBytes(service); auto old = this->services.find(id);
		/* Fleet comparisons/copies and contiguous dispatch blocks have a fixed
		 * publication charge, in addition to old/new station-index mutations. */
		size_t work = 64 + 2 * (service.timetable.stops.size() + (old == this->services.end() ? 0 : old->second.timetable.stops.size())) +
				4 * service.vehicles.size() + (service.dispatch_slots.size() + 15) / 16;
		if (work > allowance) {
			if (!_settings_game.linkgraph.passenger_journey_precomputed || work > PRECOMPUTED_CAPTURE_WORK) break;
			this->diagnostic.build_work += work - allowance; allowance = work;
		}
		allowance -= work;
		/* Captures differing only by whole circuits are the same publication.
		 * Keeping its generation also preserves already waiting intents. */
		bool same = false;
		if (old != this->services.end()) {
			/* Old wires did not capture road access. Learning that metadata is
			 * not a new service or timing publication. Enrich the live catalogue
			 * without changing the immutable graph snapshot or its saved Work.
			 * Actual order edits still change the incarnation. */
			if (service.incarnation == old->second.incarnation && service.timetable.stops.size() == old->second.timetable.stops.size()) {
				for (size_t i = 0; i < service.timetable.stops.size(); ++i) {
					auto &before = old->second.timetable.stops[i]; const auto &after = service.timetable.stops[i];
					if (before.boarding_facility == 0 && before.station == after.station && before.order == after.order) before.boarding_facility = after.boarding_facility;
				}
			}
			auto normalized = service;
			normalized.BuildAircraftCosts(this->aircraft_cost);
			normalized.generation = old->second.generation;
			if (normalized.timetable.period > 0 && normalized.vehicles.size() == old->second.vehicles.size()) {
				for (size_t i = 0; i < normalized.vehicles.size(); ++i) {
					int64_t difference = normalized.vehicles[i].base.base() % normalized.timetable.period - old->second.vehicles[i].base.base() % normalized.timetable.period;
					if (difference % normalized.timetable.period == 0) normalized.vehicles[i].base = old->second.vehicles[i].base;
				}
			}
			same = normalized == old->second;
		}
		if (!same) {
			if (old == this->services.end() || !SamePrecomputedStructure(old->second, service)) this->precomputed_invalidated = true;
			if (!changed) { assert(this->published_revision != UINT64_MAX); ++this->published_revision; changed = true; }
			if (old != this->services.end()) { this->TouchStations(old->second); this->diagnostic.service_updates += service.incarnation != 0; this->diagnostic.service_removals += service.incarnation == 0; }
			else this->diagnostic.service_additions += service.incarnation != 0;
			this->Remove(id);
			if (service.incarnation != 0 && this->bytes + ServiceBytes(service) <= MAX_BYTES) {
				service.BuildAircraftCosts(this->aircraft_cost);
				this->TouchStations(service); this->bytes += ServiceBytes(service); this->Index(service);
				auto published = this->services.emplace(id, std::move(service)).first;
				if (this->service_lookup.empty()) this->service_lookup.resize(OrderListID::Invalid().base(), nullptr);
				this->service_lookup[id.base()] = &published->second;
				for (auto &table : this->precomputed) table.RefreshService(published->second);
				for (auto &table : this->retained_precomputed) table.RefreshService(published->second);
			}
		}
		StateTicks since = this->staged_since.at(id);
		if (_state_ticks >= since) {
			uint64_t wait = uint64_t(_state_ticks.base()) - uint64_t(since.base());
			this->diagnostic.published_wait_ticks += wait;
			this->diagnostic.maximum_pending_ticks = std::max(this->diagnostic.maximum_pending_ticks, wait);
		}
		++this->diagnostic.published_wait_samples;
		this->building_bytes -= staged_bytes;
		this->unsafe.erase(id); this->staged_since.erase(id); this->building.erase(it);
		this->unsafe_lookup.reset(id.base());
	}
	if (changed) ++this->diagnostic.publications;
	this->publishing = !this->building.empty();
	this->diagnostic.publication_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}

const PassengerPeriodicService *PassengerServiceDatabase::Get(OrderListID id) const
{
	if (!this->service_lookup.empty()) return id.base() < this->service_lookup.size() ? this->service_lookup[id.base()] : nullptr;
	auto found = this->services.find(id);
	return found == this->services.end() ? nullptr : &found->second;
}

const std::set<PassengerServiceDatabase::StopRef> &PassengerServiceDatabase::StopsAt(StationID station) const
{
	static const std::set<StopRef> empty;
	auto row = this->stations.find(station);
	return row == this->stations.end() ? empty : row->second;
}

PassengerServiceDatabase::Publication PassengerServiceDatabase::GetPublication(OrderListID id) const
{
	if (this->unsafe.contains(id) || this->dirty.contains(id) || this->build_queue.contains(id) || this->building.contains(id)) return this->services.contains(id) && !this->unsafe.contains(id) ? Publication::TimingDirty : Publication::Dirty;
	if (this->services.contains(id)) return Publication::Published;
	return this->sweeping && id.base() >= this->cursor ? Publication::SweepPending : Publication::Unpublished;
}

bool PassengerServiceDatabase::IsCurrent(const PassengerJourneyExecution &ride) const
{
	if (ride.kind == PassengerRideKind::HyperpathIntent) {
		if (ride.signature == 0 || ride.signature > NUM_CARGO || ride.generation == 0) return false;
		CargoType cargo = static_cast<CargoType>(ride.signature - 1);
		return (ride.generation == this->precomputed_policy_generation && this->precomputed[cargo].HasPolicy(ride.boarding, ride.alighting)) ||
				(ride.generation == this->retained_precomputed_policy_generation && this->retained_precomputed[cargo].HasPolicy(ride.boarding, ride.alighting));
	}
	const auto *service = this->Get(ride.service);
	const auto *list = OrderList::GetIfValid(ride.service);
	return !this->unsafe_lookup.test(ride.service.base()) && list != nullptr && service != nullptr && list->GetCargoDistRouteGeneration() == ride.signature && service->incarnation == ride.signature && service->generation == ride.generation;
}

PassengerServiceDatabase::JourneyPolicy PassengerServiceDatabase::CompilePolicy(CargoType cargo, StationID origin,
		StationID destination, PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, Dependencies &dependencies) const
{
	using Reason = PassengerPeriodicPlan::Reason;
	JourneyPolicy policy;
	auto depend = [&](StationID station) { dependencies.try_emplace(station, this->StationRevision(station)); };
	depend(origin); depend(destination);
	policy.bytes = 256;
	auto spend = [&]() { if (policy.work >= limits.work) { policy.limited = true; policy.reason = Reason::Work; return false; } ++policy.work; return true; };
	if (!spend()) return policy;
	if (!this->stations.contains(origin)) { policy.reason = Reason::OriginMissing; return policy; }
	if (!this->stations.contains(destination)) { policy.reason = Reason::DestinationMissing; return policy; }
	bool alighting = false, known = false;
	for (const auto &ref : this->stations.at(destination)) {
		if (!spend()) return policy;
		const auto &service = this->services.at(ref.service);
		if (!(service.timetable.stops[ref.stop].alight & (uint64_t{1} << cargo))) continue;
		alighting = true;
		for (const auto &v : service.vehicles) {
			if (!spend()) return policy;
			if (v.phase_known && v.capacity[cargo]) { known = true; break; }
		}
		if (known) break;
	}
	if (!known) { policy.reason = alighting ? Reason::UnknownPhase : Reason::NoAlighting; return policy; }
	/* Relaxed reverse Dijkstra on adjacent periodic stops. Dwell, boarding,
	 * transfer restrictions and waiting are omitted, so these are admissible
	 * time lower bounds, never candidates or nominally selected winners.
	 * No edges or physical visits are materialised. O(stop incidences), even
	 * for a long line whose through ride has hundreds of intermediate stops. */
	using Item = std::pair<int64_t, StationID>;
	std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
	std::map<StationID, int64_t> distances, settled;
	size_t memory = 256;
	auto insert = [&](StationID station, int64_t distance) {
		auto it = distances.find(station);
		if (it != distances.end() && it->second <= distance) return true;
		size_t extra = 64 + (it == distances.end() ? 192 : 0);
		if (memory > limits.memory_bytes || extra > limits.memory_bytes - memory) {
			policy.limited = true; policy.reason = Reason::Memory; return false;
		}
		memory += extra;
		distances[station] = distance; queue.emplace(distance, station);
		return true;
	};
	if (!insert(destination, 0)) return policy;
	while (!queue.empty() && !policy.limited) {
		if (!spend()) break;
		auto [distance, station] = queue.top(); queue.pop();
		depend(station);
		if (distances.at(station) != distance || settled.contains(station)) continue;
		settled.emplace(station, distance);
		policy.unsettled_bound = distance;
		/* Settled distances are safe even in a partial reverse search. Once
		 * the origin is settled, further national expansion is unnecessary:
		 * every unsettled station has distance at least this frontier value. */
		if (station == origin) break;
		for (const auto &ref : this->stations.at(station)) {
			if (!spend()) break;
			const auto &service = this->services.at(ref.service);
			bool timed = false;
			for (const auto &v : service.vehicles) {
				if (!spend()) break;
				policy.unknown_phase |= !v.phase_known && v.capacity[cargo] != 0;
				if (v.phase_known && v.capacity[cargo]) { timed = true; break; }
			}
			if (policy.limited) break;
			if (!timed) continue;
			const auto &stops = service.timetable.stops;
			size_t previous = (ref.stop + stops.size() - 1) % stops.size();
			/* Operating-only occurrences retain their dwell and phase, but have
			 * no station-index row. Follow the same service back to an exchange. */
			while ((stops[previous].board | stops[previous].alight) == 0) {
				if (!spend()) break;
				previous = (previous + stops.size() - 1) % stops.size();
			}
			if (policy.limited) break;
			int64_t duration = stops[ref.stop].arrival - stops[previous].departure;
			if (previous >= ref.stop) duration += service.timetable.period;
			duration += service.AircraftAdjustment(previous, ref.stop);
			int64_t next = distance;
			if (AddTicks(next, duration) && !insert(stops[previous].station, next)) break;
		}
	}
	policy.complete = queue.empty() && !policy.limited && !settled.contains(origin);
	if (!policy.limited) policy.reason = policy.complete ? Reason::Unreachable : Reason::NoCandidate;
	if (policy.complete && policy.unknown_phase) {
		/* Only after proving no known-phase connection, check the same stop
		 * permissions with phase omitted. This identifies phase-only gaps,
		 * rather than labelling every disconnected OD as a timing problem.
		 * Diagnostic truncation cannot undo the known-phase absence proof. */
		std::set<StationID> seen;
		std::vector<StationID> frontier;
		auto diagnostic_spend = [&]() {
			if (policy.work >= limits.work) { policy.diagnostic_limit = Reason::Work; return false; }
			++policy.work; return true;
		};
		auto visit = [&](StationID station) {
			depend(station);
			if (seen.contains(station)) return true;
			if (memory > limits.memory_bytes || 192 > limits.memory_bytes - memory) { policy.diagnostic_limit = Reason::Memory; return false; }
			memory += 192; seen.insert(station); frontier.push_back(station); return true;
		};
		visit(origin);
		bool reached = false;
		for (size_t i = 0; i < frontier.size() && !reached && policy.diagnostic_limit == Reason::NoCandidate; ++i) {
			if (!diagnostic_spend()) break;
			for (const auto &ref : this->stations.at(frontier[i])) {
				if (!diagnostic_spend()) break;
				const auto &service = this->services.at(ref.service);
				const auto &stops = service.timetable.stops;
				if (!(stops[ref.stop].board & (uint64_t{1} << cargo))) continue;
				bool capable = false;
				for (const auto &v : service.vehicles) { if (!diagnostic_spend()) break; capable |= v.capacity[cargo] != 0; }
				if (!capable || policy.diagnostic_limit != Reason::NoCandidate) continue;
				for (size_t step = 1; step < stops.size(); ++step) {
					if (!diagnostic_spend()) break;
					const auto &stop = stops[(ref.stop + step) % stops.size()];
					if (stop.alight & (uint64_t{1} << cargo)) {
						if (stop.station == destination) { reached = true; break; }
						if (!visit(stop.station)) break;
					}
					if (stop.forced & (uint64_t{1} << cargo)) break;
				}
				if (reached || policy.diagnostic_limit != Reason::NoCandidate) break;
			}
		}
		if (reached) policy.reason = Reason::UnknownPhase;
	}
	policy.lower_bounds.assign(settled.begin(), settled.end());
	policy.bytes = 256 + 128 * settled.size();
	policy.scratch_bytes = memory;
	return policy;
}

PassengerPeriodicPlan PassengerServiceDatabase::Plan(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
		PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, OrderListID excluded)
{
	return this->PlanInternal(cargo, origin, destination, ready, limits, evaluation, excluded, true);
}

PassengerPeriodicPlan PassengerServiceDatabase::PlanWithoutServicePruning(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
		PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, OrderListID excluded)
{
	return this->PlanInternal(cargo, origin, destination, ready, limits, evaluation, excluded, false);
}

PassengerPeriodicPlan PassengerServiceDatabase::PlanWithLegacySearchStructures(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
		PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, OrderListID excluded)
{
	return this->PlanInternal(cargo, origin, destination, ready, limits, evaluation, excluded, true, true);
}

PassengerPeriodicPlan PassengerServiceDatabase::PlanWithLegacyCompletion(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
		PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, OrderListID excluded)
{
	return this->PlanInternal(cargo, origin, destination, ready, limits, evaluation, excluded, true, false, false);
}

PassengerPeriodicPlan PassengerServiceDatabase::PlanWithOriginalCompletion(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
		PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, OrderListID excluded)
{
	return this->PlanInternal(cargo, origin, destination, ready, limits, evaluation, excluded, true, false, false, false);
}

bool PassengerServiceDatabase::ServiceCannotImprove(const PassengerPeriodicService &service, size_t boarding, CargoType cargo,
		StationID destination, int64_t cost, bool transfer, int64_t best, const JourneyPolicy &policy, PassengerJourneyEvaluation evaluation)
{
	const auto &pattern = service.timetable;
	++this->diagnostic.service_bound_checks;
	/* Waiting is relaxed to zero; a required interchange alone guarantees
	 * minimum_transfer elapsed ticks and its nonnegative preference penalties.
	 * These are cost bounds, never departure times or physical waiting claims. */
	int64_t interchange = evaluation.minimum_transfer;
	if (!AddTicks(interchange, evaluation.transfer_penalty) ||
			!AddTicks(interchange, int64_t(evaluation.minimum_transfer) * evaluation.transfer_time_weight / 1000) ||
			!AddTicks(interchange, int64_t(evaluation.minimum_transfer) * evaluation.waiting_weight / 1000)) return false;
	if (transfer && !AddTicks(cost, interchange)) return true;
	if (cost > best) return true;
	const uint64_t mask = uint64_t{1} << cargo;
	const auto &board = pattern.stops[boarding];
	for (size_t step = 1; step < pattern.stops.size(); ++step) {
		++this->diagnostic.service_bound_stops;
		size_t index = (boarding + step) % pattern.stops.size();
		const auto &alight = pattern.stops[index];
		int64_t duration = alight.arrival - board.departure;
		if (index <= boarding) duration += pattern.period;
		duration += service.AircraftAdjustment(boarding, index);
		int64_t lower = cost;
		/* Arrival offsets increase around this one circuit. Once the ride
		 * alone exceeds the incumbent, no later occurrence can improve it;
		 * avoid both the remaining stop scan and reverse-bound lookups. */
		if (!AddTicks(lower, duration) || lower > best) return true;
		if (alight.alight & mask) {
			/* The exact one-circuit ride includes intervening dwell, depots and
			 * waypoints. Repeated stations remain distinct alighting occurrences.
			 * Any continuation needs another ride, even onto the same service. */
			if (AddTicks(lower, policy.DistanceLowerBound(alight.station)) &&
					(alight.station == destination || AddTicks(lower, interchange)) && lower <= best) return false;
		}
		if (alight.forced & mask) break;
	}
	/* STRICT comparison: equal costs can still win the canonical tie. */
	return true;
}

PassengerPeriodicPlan PassengerServiceDatabase::PlanInternal(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
		PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, OrderListID excluded, bool prune_services, bool legacy_structures, bool anytime, bool validate_completes)
{
	using Reason = PassengerPeriodicPlan::Reason;
	using WorkStop = PassengerPeriodicPlan::WorkStop;
	PassengerPeriodicPlan result;
	const auto started = std::chrono::steady_clock::now(); ++this->diagnostic.searches;
	auto observe = [&](Reason reason) { result.observations[static_cast<size_t>(reason)] = true; };
	auto finish = [&]() {
		if (result.ride) result.state = result.deferred ? PassengerPeriodicPlan::State::Provisional : PassengerPeriodicPlan::State::Certified;
		else if (result.deferred && (result.reason == Reason::Work || result.reason == Reason::Labels || result.reason == Reason::Memory || result.reason == Reason::Transfers)) result.state = PassengerPeriodicPlan::State::Deferred;
		else if (!result.deferred && !result.observations[static_cast<size_t>(Reason::UnknownPhase)] && !result.observations[static_cast<size_t>(Reason::Evaluation)] &&
				(result.reason == Reason::OriginMissing || result.reason == Reason::DestinationMissing || result.reason == Reason::NoAlighting ||
				result.reason == Reason::Unreachable || result.reason == Reason::NoCandidate)) result.state = PassengerPeriodicPlan::State::NoRoute;
		++this->diagnostic.result_states[static_cast<size_t>(result.state)];
		observe(result.reason);
		++this->diagnostic.outcomes[static_cast<size_t>(result.reason)];
		for (size_t i = 0; i < result.observations.size(); ++i) this->diagnostic.observations[i] += result.observations[i];
		this->diagnostic.search_work += result.work;
		++this->diagnostic.search_work_histogram[std::min<size_t>((result.work + 1023) / 1024, 64)];
		for (size_t i = 0; i < result.search.work_stops.size(); ++i) {
			this->diagnostic.work_stop_observations[i] += result.search.work_stops[i];
			this->diagnostic.work_stop_calls[i] += result.search.work_stops[i] != 0;
		}
		if (result.reason == Reason::Work) {
			this->diagnostic.work_with_destination_label += result.search.destination_labels != 0;
			this->diagnostic.work_with_complete += result.search.complete_labels != 0;
			this->diagnostic.work_without_complete += result.search.complete_labels == 0;
			this->diagnostic.work_with_queued_complete += result.search.queued_complete != 0;
			this->diagnostic.work_with_popped_complete += result.search.popped_complete != 0;
			this->diagnostic.work_with_improvement += result.search.improvement_possible;
			this->diagnostic.work_reverse += result.search.reverse_work;
			this->diagnostic.work_forward += result.search.forward_work;
			if (this->diagnostic.work_sample_count < this->diagnostic.work_samples.size()) this->diagnostic.work_samples[this->diagnostic.work_sample_count++] = result.search;
		}
		this->diagnostic.query_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
		return result;
	};
	if (cargo >= NUM_CARGO || origin == destination || !Station::IsValidID(origin) || !Station::IsValidID(destination) ||
			evaluation.minimum_transfer < 0 || evaluation.transfer_penalty < 0) return finish();
	if (evaluation.crowding_penalty != 0 || evaluation.capacity_shortfall_penalty != 0) {
		result.deferred = true; result.reason = Reason::Evaluation; return finish();
	}
	if (limits.work == 0 || limits.memory_bytes < 256) {
		if (limits.work == 0) { result.search.first_work_stop = WorkStop::Admission; ++result.search.work_stops[static_cast<size_t>(WorkStop::Admission)]; }
		result.deferred = true; result.reason = limits.work == 0 ? Reason::Work : Reason::Memory; return finish();
	}
	PolicyKey key{false, cargo, origin, destination, OrderListID::Invalid(), limits, evaluation};
	auto *cached = this->FindPolicy(key);
	Dependencies dependencies;
	JourneyPolicy temporary;
	const JourneyPolicy *policy;
	if (cached != nullptr) { policy = &std::get<JourneyPolicy>(cached->value); ++this->diagnostic.policy_hits; }
	else {
		auto compilation = limits; compilation.work /= 2;
		const auto compile_start = std::chrono::steady_clock::now();
		temporary = this->CompilePolicy(cargo, origin, destination, compilation, evaluation, dependencies); policy = &temporary; ++this->diagnostic.policy_misses;
		this->diagnostic.policy_compile_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - compile_start).count();
	}
	result.work = policy->work; result.memory = policy->bytes;
	result.search.reverse_work = policy->work;
	result.reason = policy->reason;
	/* Replaying the same reverse work keeps optional residency irrelevant to
	 * admission, including cold/warm/evicted caches and network joins. */
	if (policy->limited) observe(policy->reason);
	if (policy->unknown_phase) observe(Reason::UnknownPhase);
	if (policy->diagnostic_limit != Reason::NoCandidate) observe(policy->diagnostic_limit);
	const auto forward_started = std::chrono::steady_clock::now();
	if (!policy->limited && policy->reason != Reason::NoCandidate) {
		// No timed route in the published generation, independently of dirty work.
	} else {
		auto forward = [&](auto &scratch) {
			using Label = PassengerTimedLabel;
			auto &labels = scratch.labels;
			auto &queue = scratch.queue;
			std::optional<int64_t> incumbent_cost; ///< Cost of an admitted complete label, independently of queue order.
			size_t best_complete = SIZE_MAX;
			int64_t omitted_bound = INT64_MAX;
			int64_t non_work_omitted_bound = INT64_MAX;
			int64_t interrupted_bound = INT64_MAX;
			auto bound = [&](StationID station, int64_t cost) {
				int64_t distance = policy->DistanceLowerBound(station);
				if (!AddTicks(cost, distance)) return INT64_MAX;
				return cost;
			};
			auto limited = [&](Reason reason, int64_t lower) {
				observe(reason); omitted_bound = std::min(omitted_bound, lower);
				if (reason != Reason::Work) non_work_omitted_bound = std::min(non_work_omitted_bound, lower);
				if (result.reason == Reason::NoCandidate || result.reason == Reason::Work || result.reason == Reason::Memory) result.reason = reason;
			};
			auto work_stop = [&](WorkStop where, int64_t lower) {
				if (result.search.first_work_stop == WorkStop::None) result.search.first_work_stop = where;
				++result.search.work_stops[static_cast<size_t>(where)];
				interrupted_bound = std::min(interrupted_bound, lower);
			};
			auto spend = [&](WorkStop where, auto lower) {
				/* Bounds are observations on a failed spend, not additional work on
				 * every vehicle/dominance visit in the hot loop. */
				if (result.work >= limits.work) { work_stop(where, lower()); limited(Reason::Work, 0); return false; }
				++result.work; return true;
			};
			auto first_key = [](const Label &a) { return std::tie(a.first.departure, a.first.service, a.first.boarding_order, a.first.alighting_order); };
			auto complete_key = [](const Label &a) {
				return std::tie(a.cost, a.time, a.rides, a.first.departure, a.first.service, a.first.boarding_order, a.first.alighting_order);
			};
			auto complete_valid = [&](size_t index) {
				if (labels[index].station != destination || labels[index].rides == 0) return false;
				const auto &first = labels[index].first;
				while (labels[index].parent != SIZE_MAX) {
					const auto &child = labels[index];
					if (child.parent >= index) return false;
					const auto &parent = labels[child.parent]; const auto &leg = child.leg;
					if (child.rides != parent.rides + 1 || leg.boarding != parent.station || leg.alighting != child.station ||
							leg.arrival != child.time || leg.departure < parent.time || leg.departure < leg.not_before ||
							leg.arrival < leg.departure || !this->IsCurrent(leg)) return false;
					if (parent.parent == SIZE_MAX && leg != first) return false;
					index = child.parent;
				}
				return labels[index].station == origin && labels[index].time == ready && labels[index].rides == 0;
			};
			auto insert = [&](Label next) {
				int64_t lower = bound(next.station, next.cost);
				if (result.ride && lower > result.cost) return;
				++this->diagnostic.frontier_lookups;
				auto frontier = scratch.At(next.station);
				size_t retained = 0;
				if (next.station != destination) for (size_t index : frontier) {
					if (!spend(WorkStop::Dominance, [&]() { return lower; })) return;
					const auto &other = labels[index];
					if (!other.active) continue;
					++retained;
					/* Earliest arrival dominates only when the extra wait has no
					 * preference cost. Weighted waits retain different arrival times.
					 * Continuations allow any service, so no path-history assumption
					 * or forbidden-station set makes this dominance unsound. */
					bool times = evaluation.waiting_weight == 0 ? other.time <= next.time : other.time == next.time;
					/* Compare at the later clock time: a discounted later arrival
					 * may cost less than waiting on the earlier, cheaper prefix. */
					bool costs = other.cost <= next.cost;
					if (times && !this->aircraft_cost.IsDefault()) {
						int64_t wait = (next.time - other.time).base();
						costs = next.cost >= wait && other.cost <= next.cost - wait;
					}
					if (times && costs && other.rides <= next.rides &&
							(other.rides < next.rides || first_key(other) <= first_key(next) ||
							(evaluation.waiting_weight != 0 && other.cost < next.cost))) return;
				}
				if (next.station != destination && retained >= limits.labels_per_station) { limited(Reason::Labels, lower); return; }
				if (labels.size() >= limits.labels) { limited(Reason::Labels, lower); return; }
				constexpr size_t LABEL_BYTES = 1536; // Original logical envelope: never discount reused storage or direct indexing.
				if (result.memory > limits.memory_bytes || LABEL_BYTES > limits.memory_bytes - result.memory) { limited(Reason::Memory, lower); return; }
				result.memory += LABEL_BYTES;
				/* Admission precedes storage and completion. A rejected label is never
				 * an incumbent, and each parent's index precedes its admitted child. */
				const size_t index = labels.size();
				labels.push_back(std::move(next)); scratch.Append(labels.back().station, index);
				auto &admitted = labels[index];
				if (admitted.station == destination) {
					++result.search.destination_labels;
					admitted.complete_valid = complete_valid(index);
					result.search.complete_labels += admitted.complete_valid;
					if (admitted.complete_valid && (best_complete == SIZE_MAX || complete_key(admitted) < complete_key(labels[best_complete]))) {
						best_complete = index;
						result.search.best_complete_cost = admitted.cost; result.search.best_complete_arrival = admitted.time;
					}
					if ((!validate_completes || admitted.complete_valid) && (!incumbent_cost || admitted.cost < *incumbent_cost)) incumbent_cost = admitted.cost;
				}
				queue.emplace(lower, admitted.cost, admitted.time, admitted.rides, admitted.first.departure, admitted.first.service,
					admitted.first.boarding_order, admitted.first.alighting_order, index);
				++this->diagnostic.queue_pushes;
			};
			insert({origin, ready, 0, SIZE_MAX, 0, {}, {}});
			size_t winner = SIZE_MAX;
			const size_t candidates = std::clamp<size_t>(limits.candidates, 1, 16);
			while (!queue.empty()) {
				if (result.ride && std::get<0>(queue.top()) > result.cost) break;
				if (!spend(WorkStop::Queue, [&]() {
					const auto &item = queue.top();
					return labels[std::get<8>(item)].station == destination ? INT64_MAX : std::get<0>(item);
				})) break;
				size_t index = std::get<8>(queue.top()); queue.pop();
				++this->diagnostic.queue_pops;
				Label current = labels[index];
				if (!current.active) continue;
				if (current.station == destination) {
					result.search.popped_complete += current.complete_valid;
					if (validate_completes && !current.complete_valid) { observe(Reason::Evaluation); continue; }
					/* Completion only when popped by actual cost plus time lower bound.
					 * A full candidate list never prevents a better transfer prefix. */
					if (result.candidates < candidates) ++result.candidates; else observe(Reason::Candidates);
					if (!result.ride || std::tie(current.cost, current.time, current.rides, current.first.departure, current.first.service,
							current.first.boarding_order, current.first.alighting_order) < std::tie(result.cost, result.arrival, labels[winner].rides,
							result.ride->departure, result.ride->service, result.ride->boarding_order, result.ride->alighting_order)) {
						result.ride = current.first; result.arrival = current.time; result.cost = current.cost;
						result.transfers = current.rides - 1; winner = index;
					}
					continue;
				}
				if (current.rides > limits.max_transfers) { limited(Reason::Transfers, bound(current.station, current.cost)); continue; }
				auto row = this->stations.find(current.station);
				if (row == this->stations.end()) continue;
				for (const auto &ref : row->second) {
					if (!spend(WorkStop::Service, [&]() { return bound(current.station, current.cost); })) break;
					if (current.rides == 0 && ref.service == excluded) continue;
					const auto &service = this->services.at(ref.service);
					if (validate_completes) {
						const auto *list = OrderList::GetIfValid(service.id);
						if (this->unsafe.contains(service.id) || list == nullptr || list->GetCargoDistRouteGeneration() != service.incarnation) {
							observe(Reason::Evaluation); continue;
						}
					}
					const auto &pattern = service.timetable;
					const auto &board = pattern.stops[ref.stop];
					if (!(board.board & (uint64_t{1} << cargo))) continue;
					/* Deterministic algorithmic work reduction. Bounds, like bound()
					 * above, are not logical Work; skipped vehicle/slot/alighting visits
					 * are not replayed. No cache presence or physical timing is consulted. */
					if (prune_services && incumbent_cost && this->ServiceCannotImprove(service, ref.stop, cargo, destination,
							current.cost, current.rides != 0, *incumbent_cost, *policy, evaluation)) {
						++this->diagnostic.pruned_services;
						this->diagnostic.pruned_vehicle_visits += service.vehicles.size();
						continue;
					}
					int64_t earliest = current.time.base();
					if (current.rides && !AddTicks(earliest, evaluation.minimum_transfer)) { observe(Reason::Evaluation); continue; }
					std::optional<StateTicks> departure;
					/* A full scan spends one unit per vehicle, in saved vehicle order.
					 * Only batch when it finishes STRICTLY below the work limit: equality
					 * also triggers the original post-scan Work stop. Otherwise preserve
					 * every partial prefix and dispatch-slot spend with the old loop. */
					const Station *loading_station = Station::GetIfValid(current.station);
					const PassengerDepartureIndex *phases = loading_station != nullptr && result.work < limits.work && service.vehicles.size() < limits.work - result.work ? this->DepartureIndex(service, cargo) : nullptr;
					if (phases != nullptr) {
						++this->diagnostic.departure_index_hits;
						result.work += service.vehicles.size(); this->diagnostic.vehicle_candidates += service.vehicles.size();
						this->diagnostic.next_departures += phases->Known();
						if (phases->Unknown()) observe(Reason::UnknownPhase);
						std::vector<VehicleID> temporary_loading;
						auto &loading = scratch.Loading(temporary_loading);
						loading.clear();
						for (const Vehicle *live : loading_station->loading_vehicles) {
							if (Vehicle::GetIfValid(live->index) != live) continue;
							if (!phases->Contains(live->index)) continue;
							auto base = LoadingBase(live, service, board);
							if (!base) continue;
							loading.push_back(live->index);
							++this->diagnostic.departure_calculations;
							auto next = PassengerPeriodicTimetable::NextDeparture(*base, board.departure, pattern.period, StateTicks{earliest});
							if (next && (!departure || *next < *departure)) departure = next;
						}
						std::sort(loading.begin(), loading.end());
						auto next = phases->Next(board.departure, StateTicks{earliest}, loading, this->diagnostic.departure_calculations);
						if (next && (!departure || *next < *departure)) departure = next;
					} else {
						++this->diagnostic.departure_index_fallbacks;
						for (const auto &vehicle : service.vehicles) {
							if (!spend(WorkStop::Vehicle, [&]() { return bound(current.station, current.cost); })) break;
							++this->diagnostic.vehicle_candidates;
							if (!vehicle.capacity[cargo]) continue;
							if (!vehicle.phase_known) { observe(Reason::UnknownPhase); continue; }
							if (service.dispatch_period > 0) {
								int64_t offset = board.departure - pattern.stops[service.dispatch_stop].departure;
								if (offset < 0) offset += pattern.period;
								for (uint32_t slot : service.dispatch_slots) {
									if (!spend(WorkStop::Dispatch, [&]() { return bound(current.station, current.cost); })) break;
									int64_t floor = service.dispatch_floor.base();
									if (!AddTicks(floor, offset)) continue;
									++this->diagnostic.next_departures;
									++this->diagnostic.departure_calculations;
									auto next = PassengerPeriodicTimetable::NextDeparture(service.dispatch_base, slot + offset, service.dispatch_period, StateTicks{std::max(earliest, floor)});
									if (next && (!departure || *next < *departure)) departure = next;
								}
							} else {
								++this->diagnostic.next_departures;
								++this->diagnostic.departure_calculations;
								StateTicks base = vehicle.base;
								const Vehicle *live = Vehicle::GetIfValid(vehicle.id);
								/* A published phase must not make an actually loading,
								 * overdue vehicle wait a whole circuit in a new query. This
								 * constant-size live observation changes no saved phase or
								 * cache: use the remaining published dwell at this occurrence.
								 * Future-ready queries still require the next real cycle. */
								if (auto loading = LoadingBase(live, service, board)) base = *loading;
								auto next = PassengerPeriodicTimetable::NextDeparture(base, board.departure, pattern.period, StateTicks{earliest});
								if (next && (!departure || *next < *departure)) departure = next;
							}
						}
					}
					if (result.work >= limits.work) { work_stop(WorkStop::Departure, bound(current.station, current.cost)); limited(Reason::Work, 0); break; }
					if (!departure) { observe(Reason::Evaluation); continue; }
					if (current.rides && current.leg.service == service.id && current.leg.alighting_order == board.order &&
							!(board.forced & (uint64_t{1} << cargo))) {
						int64_t through = current.time.base();
						/* Staying aboard was already enumerated by the parent. Only a
						 * genuinely earlier shared-vehicle departure can improve it. */
						if (AddTicks(through, board.departure - board.arrival) && departure->base() >= through) continue;
					}
					for (size_t step = 1; step < pattern.stops.size(); ++step) {
						if (!spend(WorkStop::Alighting, [&]() { return bound(current.station, current.cost); })) break;
						size_t stop_index = (ref.stop + step) % pattern.stops.size();
						const auto &alight = pattern.stops[stop_index];
						if ((alight.alight & (uint64_t{1} << cargo)) && (!policy->complete || policy->LowerBound(alight.station) != nullptr)) {
							int64_t duration = alight.arrival - board.departure;
							if (stop_index <= ref.stop) duration += pattern.period;
							int64_t end = departure->base(), cost = current.cost;
							if (!AddTicks(end, duration) || end < current.time.base() || uint64_t(end) - uint64_t(current.time.base()) > INT64_MAX) { observe(Reason::Evaluation); continue; }
							int64_t elapsed = end - current.time.base(), wait = departure->base() - current.time.base();
							int64_t adjustment = service.AircraftAdjustment(ref.stop, stop_index);
							if (adjustment > 0 && elapsed > INT64_MAX - adjustment) { observe(Reason::Evaluation); continue; }
							elapsed += adjustment; // Discounts replace only movement, so this stays nonnegative.
							if (!AddTicks(cost, elapsed) || (current.rides && !AddTicks(cost, evaluation.transfer_penalty)) ||
									(evaluation.waiting_weight && (wait > INT64_MAX / evaluation.waiting_weight || !AddTicks(cost, wait * evaluation.waiting_weight / 1000))) ||
									(current.rides && !AddTicks(cost, int64_t(evaluation.minimum_transfer) * evaluation.transfer_time_weight / 1000))) { observe(Reason::Evaluation); continue; }
							PassengerJourneyExecution leg{VehicleID::Invalid(), service.id, board.station, alight.station, board.order, alight.order, *departure, StateTicks{end}, service.incarnation};
							leg.kind = PassengerRideKind::ServiceIntent; leg.generation = service.generation; leg.not_before = StateTicks{earliest};
							insert({alight.station, StateTicks{end}, cost, index, current.rides + 1, leg, current.rides ? current.first : leg});
						}
						if (alight.forced & (uint64_t{1} << cargo)) break;
					}
					if (result.work >= limits.work) break;
				}
			}
			result.labels = labels.size();
			result.search.forward_work = result.work - result.search.reverse_work;
			result.search.remaining_lower_bound = std::min(interrupted_bound, non_work_omitted_bound);
			for (const auto &item : queue.Items()) {
				const auto &label = labels[std::get<8>(item)];
				if (!label.active) continue;
				result.search.queued_complete += label.complete_valid;
				if (label.station != destination) result.search.remaining_lower_bound = std::min(result.search.remaining_lower_bound, std::get<0>(item));
			}
			result.search.improvement_possible = result.search.best_complete_cost && result.search.remaining_lower_bound <= *result.search.best_complete_cost;
			this->diagnostic.timed_labels += result.labels;
			const bool rescued = anytime && best_complete != SIZE_MAX && winner == SIZE_MAX;
			if (anytime && best_complete != SIZE_MAX) {
				winner = best_complete;
				const auto &complete = labels[winner];
				result.ride = complete.first; result.arrival = complete.time; result.cost = complete.cost;
				result.transfers = complete.rides - 1;
			}
			if (result.ride) {
				/* Destination membership alone is insufficient: require a finite chain
				 * of connected admitted legs back to the exact origin/ready root. No
				 * later boarding is guaranteed by this forecast itinerary. */
				size_t index = winner;
				bool valid = labels[index].station == destination && labels[index].rides != 0;
				while (valid && labels[index].parent != SIZE_MAX) {
					const auto &child = labels[index];
					if (child.parent >= index) { valid = false; break; }
					const auto &parent = labels[child.parent]; const auto &leg = child.leg;
					valid = child.rides == parent.rides + 1 && leg.boarding == parent.station && leg.alighting == child.station &&
							leg.arrival == child.time && leg.departure >= parent.time && leg.departure >= leg.not_before &&
							leg.arrival >= leg.departure && (!validate_completes || this->IsCurrent(leg));
					if (valid) result.route.push_back(leg);
					index = child.parent;
				}
				valid = valid && labels[index].station == origin && labels[index].time == ready && labels[index].rides == 0 &&
						!result.route.empty() && result.route.back() == *result.ride;
				if (!valid) {
					result.ride.reset(); result.route.clear(); result.deferred = true; observe(Reason::Evaluation);
					if (result.reason == Reason::NoCandidate) result.reason = Reason::Evaluation;
					return;
				}
				this->diagnostic.rescued_unpopped += rescued;
				std::reverse(result.route.begin(), result.route.end());
				result.deferred = omitted_bound <= result.cost;
				if (!result.deferred) result.reason = Reason::Found;
			} else {
				result.deferred = omitted_bound != INT64_MAX;
				if (!result.deferred) result.reason = result.observations[static_cast<size_t>(Reason::UnknownPhase)] ? Reason::UnknownPhase :
						result.observations[static_cast<size_t>(Reason::Evaluation)] ? Reason::Evaluation : Reason::NoCandidate;
			}
		};
		if (legacy_structures) {
			PassengerTimedLegacySearch legacy;
			forward(legacy);
		} else if (this->timed_scratch_enabled && !this->timed_scratch.in_use) {
			PassengerTimedSearchScratch::Scope scope{this->timed_scratch};
			forward(this->timed_scratch);
			this->diagnostic.search_scratch_peak_bytes = std::max(this->diagnostic.search_scratch_peak_bytes, this->timed_scratch.CapacityBytes());
		} else {
			PassengerTimedSearchScratch temporary_scratch;
			PassengerTimedSearchScratch::Scope scope{temporary_scratch};
			forward(temporary_scratch);
			this->diagnostic.search_scratch_peak_bytes = std::max(this->diagnostic.search_scratch_peak_bytes, temporary_scratch.CapacityBytes());
		}
		this->diagnostic.search_scratch_bytes = this->timed_scratch.CapacityBytes();
	}
	this->diagnostic.forward_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - forward_started).count();
	result.memory = std::max(result.memory, policy->scratch_bytes);
	if (cached == nullptr) { const size_t bytes = 256 + sizeof(std::pair<StationID, int64_t>) * temporary.lower_bounds.size(); this->StorePolicy(key, std::move(temporary), std::move(dependencies), bytes); }
	return finish();
}
PassengerPlannerComparison ComparePassengerPlanners(const PassengerServiceDatabase &source, CargoType cargo,
		StationID origin, StationID destination, StateTicks ready, PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation)
{
	PassengerPlannerComparison comparison;
	PassengerServiceDatabase isolated;
	if (!isolated.Load(source.Save())) return comparison;
	isolated.FinishLoad(true);
	comparison.periodic = isolated.Plan(cargo, origin, destination, ready, limits, evaluation);
	PassengerServiceCapture capture;
	for (Ticks horizon = 4096;; horizon = std::min<Ticks>(65536, horizon * 4)) {
		auto graph_limits = ActivePassengerServiceGraphLimits(); graph_limits.horizon = horizon;
		auto graph = PassengerServiceGraph::Build(cargo, _state_ticks, graph_limits, PassengerServiceScope{origin, destination}, &capture, PassengerTimingPolicy::AssumeTimetable);
		PassengerJourneyPlanner planner(graph);
		auto old_limits = limits; old_limits.candidates = 1; // Match the old Active path's admission and K.
		auto legacy = planner.Plan(cargo, origin, destination, ready, old_limits, evaluation);
		comparison.legacy_found = legacy.GetOutcome() == PassengerJourneyOutcome::TimedRouteFound &&
				(horizon == 65536 || (!graph.HasAircraftDiscount() && legacy.journeys.front().generalized_time.base() < horizon)) && graph.GetQueryStatus(origin, destination) < PassengerPredictionStatus::LimitReached;
		comparison.horizon = horizon; comparison.legacy_stop = legacy.stop_reason; comparison.legacy_status = legacy.status;
		comparison.legacy_graph_work += graph.GetWork(); comparison.legacy_search_work += legacy.diagnostic.work;
		comparison.legacy_route.clear();
		if (legacy.HasJourney()) {
			const auto &journey = legacy.journeys.front();
			comparison.legacy_arrival = journey.arrival; comparison.legacy_cost = journey.generalized_time.base();
			for (const auto &leg : journey.legs) {
				const auto &trip = graph.GetTrips()[leg.boarding.trip];
				comparison.legacy_route.push_back({leg.vehicle, trip.service, leg.boarding_station, leg.alighting_station,
						trip.prediction.visits[leg.boarding.visit].order, trip.prediction.visits[leg.alighting.visit].order,
						leg.departure, leg.arrival, 0});
			}
			if ((!graph.HasAircraftDiscount() && comparison.legacy_cost < horizon) || legacy.GetOutcome() != PassengerJourneyOutcome::TimedRouteFound) break;
		} else if (legacy.stop_reason != PassengerJourneyStopReason::PredictionHorizon) break;
		if (horizon == 65536) break;
	}
	return comparison;
}

PassengerDatabaseDiagnostic::Phase PassengerServiceDatabase::ExplainPhase(const Vehicle *v, const PassengerPeriodicService &service) const
{
	using Phase = PassengerDatabaseDiagnostic::Phase;
	if (v == nullptr || !IsPassengerServiceVehicle(v) || v->vehstatus.Any({VehState::Stopped, VehState::Crashed}) || v->IsInDepot()) return Phase::VehicleState;
	if (service.timetable.period == 0) return Phase::NoTimetable;
	if (v->HasUnbunchingOrder()) return Phase::Unbunching;
	if (v->vehicle_flags.Test(VehicleFlag::ScheduledDispatch)) {
		if (v->orders->GetNumVehicles() > 1) return Phase::DispatchCompetition;
		if (service.dispatch_period == 0) return Phase::DispatchUnsupported;
	}
	size_t budget = 4096;
	PassengerPredictionReason reason{};
	auto state = GetPassengerRouteState(v, &budget, &reason);
	if (!state) {
		if (reason == PassengerPredictionReason::Budget) return Phase::Work;
		return reason == PassengerPredictionReason::VehicleState ? Phase::VehicleState : Phase::OrderProgress;
	}
	if (!std::all_of(state->usable_stops.begin(), state->usable_stops.end(), [](uint8_t usable) { return usable != 0; })) return Phase::Facilities;
	if (!state->loading && state->elapsed > state->orders[state->current_order].GetTravelTime()) return Phase::ElapsedTravel;
	return CapturePeriodicBase(v, service.timetable) ? Phase::AwaitCapture : Phase::Overflow;
}

PassengerScheduleDiagnostic PassengerServiceDatabase::GetScheduleDiagnostic() const
{
	auto d = this->schedule.GetDiagnostic();
	/* Explicit diagnostic only, never part of a simulation tick. */
	for (const auto &[key, policy] : this->policies) if (std::get<0>(key)) d.bytes += policy.bytes;
	return d;
}

PassengerDatabaseDiagnostic PassengerServiceDatabase::GetDiagnostic() const
{
	auto d = this->diagnostic;
	d.search_scratch_bytes = this->timed_scratch.CapacityBytes();
	d.departure_index_bytes = this->departure_index_bytes;
	d.departure_index_peak_bytes = std::max(d.departure_index_peak_bytes, this->departure_index_bytes);
	d.services = this->services.size(); d.bytes = this->bytes; d.policy_bytes = this->policy_bytes + this->station_revisions.capacity() * sizeof(uint64_t) + 96 * this->recent_policy_builds.size();
	d.pending = this->dirty.size() + this->build_queue.size() + this->building.size();
	d.staged = this->building.size(); d.staged_bytes = this->building_bytes; d.published_revision = this->published_revision; d.policy_entries = this->policies.size();
	if (this->sweeping) for (const OrderList *list : OrderList::Iterate()) if (list->index.base() >= this->cursor && !this->services.contains(list->index) && !this->dirty.contains(list->index) && !this->build_queue.contains(list->index) && !this->building.contains(list->index)) ++d.pending;
	auto age = [&](StateTicks since) { return _state_ticks >= since ? uint64_t(_state_ticks.base()) - uint64_t(since.base()) : uint64_t{0}; };
	for (const auto &[id, since] : this->build_since) d.maximum_pending_ticks = std::max(d.maximum_pending_ticks, age(since));
	for (const auto &[id, since] : this->staged_since) d.maximum_pending_ticks = std::max(d.maximum_pending_ticks, age(since));
	for (const auto &[id, since] : this->dirty_since) d.maximum_pending_ticks = std::max(d.maximum_pending_ticks, age(since));
	if (this->sweeping && d.pending > this->dirty.size()) d.maximum_pending_ticks = std::max(d.maximum_pending_ticks, age(this->sweeping_since));
	uint64_t passenger_mask = 0;
	for (CargoType cargo : EnumRange(NUM_CARGO)) if (IsCargoInClass(cargo, CargoClass::Passengers)) passenger_mask |= uint64_t{1} << cargo;
	for (const auto &[id, service] : this->services) {
		d.vehicles += service.vehicles.size();
		for (const auto &stop : service.timetable.stops) {
			d.physical_passenger_occurrences += std::popcount(passenger_mask);
			d.exchange_passenger_occurrences += std::popcount((stop.board | stop.alight) & passenger_mask);
		}
		if (const auto *list = OrderList::GetIfValid(id)) {
			bool depot = false; auto traversal = PassengerDepotTraversal::Timed;
			for (const auto &order : list->GetOrderVector()) if (order.IsType(OT_GOTO_DEPOT)) {
				depot = true; traversal = std::min(traversal, ClassifyPassengerDepot(order));
			}
			if (depot) {
				if (traversal == PassengerDepotTraversal::Blocked) ++d.depot_blocked;
				else if (traversal == PassengerDepotTraversal::Timed && service.timetable.period != 0) ++d.depot_timed;
				else ++d.depot_untimed;
			}
		}
		auto passenger_phase = [](const auto &v) {
			if (!v.phase_known) return false;
			for (CargoType cargo : EnumRange(NUM_CARGO)) if (v.capacity[cargo] != 0 && IsCargoInClass(cargo, CargoClass::Passengers)) return true;
			return false;
		};
		d.known_phase_vehicles += std::count_if(service.vehicles.begin(), service.vehicles.end(), passenger_phase);
		d.timed_services += std::any_of(service.vehicles.begin(), service.vehicles.end(), passenger_phase);
	}
	/* Explicit diagnostic census only. Cargo-only lists never enter passenger
	 * denominators. Counts below describe live eligibility; published timing
	 * can intentionally lag automatic timing edits until its saved epoch. */
	for (const OrderList *list : OrderList::Iterate()) {
		++d.orderlists;
		bool passengers = false;
		for (const Vehicle *v = list->GetFirstSharedVehicle(); v != nullptr; v = v->NextShared()) {
			if (!IsPassengerServiceVehicle(v)) continue;
			for (const Vehicle *part = v; part != nullptr; part = part->Next()) passengers |= part->cargo_type < NUM_CARGO && part->cargo_cap != 0 && IsCargoInClass(part->cargo_type, CargoClass::Passengers);
		}
		if (!passengers) continue;
		++d.passenger_orderlists;
		d.declared_complete_timetables += list->IsCompleteTimetable();
		PassengerPeriodicTimetable::Failure failure;
		auto table = PassengerPeriodicTimetable::Compile(list->GetOrderVector(), &failure);
		++d.compile_failures[static_cast<size_t>(failure)];
		if (table.period) { ++d.complete_timetables; ++d.compile_success; } else ++d.compile_failure;
		const auto *published = this->Get(list->index);
		d.published_passenger += published != nullptr;
		d.published_cycles += published != nullptr && published->timetable.period != 0;
		d.unpublished += published == nullptr;
		d.dirty_services += this->GetPublication(list->index) != Publication::Published;
		d.pending_passenger += this->dirty.contains(list->index) || this->build_queue.contains(list->index) || this->building.contains(list->index) || (this->sweeping && list->index.base() >= this->cursor && published == nullptr);
		PassengerPeriodicService live; live.timetable = std::move(table);
		for (const Vehicle *v = list->GetFirstSharedVehicle(); v != nullptr; v = v->NextShared()) {
			bool capable = false;
			if (!IsPassengerServiceVehicle(v)) continue;
			for (const Vehicle *part = v; part != nullptr; part = part->Next()) capable |= part->cargo_type < NUM_CARGO && part->cargo_cap != 0 && IsCargoInClass(part->cargo_type, CargoClass::Passengers);
			if (!capable) continue;
			bool known = published != nullptr && std::any_of(published->vehicles.begin(), published->vehicles.end(), [&](const auto &p) { return p.id == v->index && p.phase_known; });
			auto reason = known ? PassengerDatabaseDiagnostic::Phase::Known : this->ExplainPhase(v, published == nullptr ? live : *published);
			++d.phases[static_cast<size_t>(reason)];
			d.unknown_phase_vehicles += !known;
		}
	}
	return d;
}

bool ValidatePassengerServiceRide(const PassengerJourneyExecution &ride, const Vehicle *vehicle, CargoType cargo, bool boarding)
{
	if (cargo >= NUM_CARGO || !IsPassengerJourneyRoutingEnabled(cargo) || !IsPassengerServiceVehicle(vehicle) || Vehicle::GetIfValid(vehicle->index) != vehicle || vehicle->orders == nullptr ||
			vehicle->orders->index != ride.service || vehicle->orders->GetCargoDistRouteGeneration() != ride.signature ||
			vehicle->vehstatus.Any({VehState::Stopped, VehState::Crashed}) || !vehicle->current_order.IsAnyLoadingType() || vehicle->current_order.IsType(OT_LOADING_ADVANCE) ||
			(IsBoundPassengerRide(ride.kind) && ride.vehicle != vehicle->index) ||
			(boarding && !database.IsCurrent(ride)) ||
			!Station::IsValidID(ride.alighting) || ride.alighting_order >= vehicle->GetNumOrders()) return false;
	if (boarding && (_state_ticks < ride.not_before || ride.boarding != vehicle->last_station_visited || ride.boarding_order != vehicle->cur_real_order_index ||
			vehicle->current_order.GetCargoLoadType(cargo) == OrderLoadType::NoLoad ||
			vehicle->current_order.GetCargoUnloadType(cargo) == OrderUnloadType::Unload ||
			vehicle->current_order.GetCargoUnloadType(cargo) == OrderUnloadType::Transfer)) return false;
	size_t budget = 65536;
	if (!HasPassengerVehicleCapacity(vehicle, cargo, budget)) return false;
	/* A real implicit arrival may continue toward a declared explicit alighting.
	 * This option is used only for live prefix validation, never forecasting. */
	auto state = GetPassengerRouteState(vehicle, &budget, nullptr, !boarding);
	if (!state || !state->loading) return false;
	if (boarding && (!state->usable_stops[state->current_order] ||
			state->orders[state->current_order].GetCargoLoadType(cargo) == OrderLoadType::NoLoad)) return false;
	const size_t count = state->orders.size();
	for (size_t step = 1; step < count && budget > 0; ++step) {
		--budget;
		VehicleOrderID index = static_cast<VehicleOrderID>((state->current_order + step) % count);
		const Order &order = state->orders[index];
		if (order.IsType(OT_IMPLICIT) || order.IsType(OT_LABEL)) continue;
		if (order.IsType(OT_GOTO_DEPOT)) {
			if (ClassifyPassengerDepot(order) == PassengerDepotTraversal::Blocked) return false;
			continue;
		}
		if ((!order.IsType(OT_GOTO_STATION) && !order.IsType(OT_GOTO_WAYPOINT)) || order.IsRefit()) return false;
		if (!order.IsType(OT_GOTO_STATION) || (order.GetNonStopType() & ONSF_NO_STOP_AT_DESTINATION_STATION)) continue;
		if (!state->usable_stops[index]) return false;
		if (index == ride.alighting_order && order.GetDestination() == ride.alighting && order.GetCargoUnloadType(cargo) != OrderUnloadType::NoUnload) return true;
		if (order.GetCargoUnloadType(cargo) == OrderUnloadType::Unload || order.GetCargoUnloadType(cargo) == OrderUnloadType::Transfer) return false;
	}
	return false;
}

/** Explicit little-endian wire values, never pointers, allocator capacities or padding.
 * The compiled generation is saved, rather than rebuilt from an edited live timetable.
 * Optional policies and wall-clock observations are deliberately absent. */
std::vector<uint8_t> PassengerServiceDatabase::Save() const
{
	std::vector<uint8_t> data;
	auto put = [&](uint64_t value) { for (uint i = 0; i < 8; ++i) { data.push_back(value & 255); value >>= 8; } };
	const bool precomputed = this->precomputed_revision != UINT64_MAX;
	put(DATABASE_FORMAT_VERSION); put(this->generation); put(this->cursor); put(this->sweeping); put(this->dirty_cursor);
	put(this->sweeping_since.base());
	put(this->dirty.size()); for (OrderListID id : this->dirty) { put(id.base()); put(this->dirty_since.at(id).base()); }
	auto put_service = [&](const PassengerPeriodicService &service) {
		put(service.id.base()); put(service.incarnation); put(service.generation); put(service.timetable.period);
		put(service.timetable.estimated_period);
		put(service.timetable.stops.size());
		for (const auto &stop : service.timetable.stops) {
			put(stop.station.base()); put(stop.order); put(stop.arrival); put(stop.departure);
			put(stop.board); put(stop.alight); put(stop.forced);
			put(stop.continues); put(stop.travel_quality); put(stop.intermediate_wait);
		}
		put(service.vehicles.size());
		for (const auto &vehicle : service.vehicles) {
			put(vehicle.id.base()); put(vehicle.base.base()); put(vehicle.phase_known); put(to_underlying(vehicle.type));
			for (uint64_t capacity : vehicle.capacity) put(capacity);
		}
		put(service.dispatch_base.base()); put(service.dispatch_floor.base()); put(service.dispatch_period); put(service.dispatch_stop);
		put(service.dispatch_slots.size()); for (uint32_t slot : service.dispatch_slots) put(slot);
		put(service.uncertain_dispatch_period);
	};
	put(this->services.size()); for (const auto &[id, service] : this->services) put_service(service);
	put(this->published_revision); put(this->batch_started.base()); put(this->publishing); put(this->bootstrap);
	put(this->build_queue.size()); for (OrderListID id : this->build_queue) { put(id.base()); put(this->build_since.at(id).base()); }
	put(this->unsafe.size()); for (OrderListID id : this->unsafe) put(id.base());
	put(this->building.size()); for (const auto &[id, service] : this->building) { put_service(service); put(this->staged_since.at(id).base()); }
	{
		put(precomputed ? this->precomputed_operations : UINT64_MAX); put(this->precomputed_transfer);
		put(this->precomputed_revision); put(this->precomputed_refresh.base()); put(this->precomputed_invalidated);
		put(this->precomputed_services.size()); for (const auto &[id, service] : this->precomputed_services) put_service(service);
		put(this->retained_precomputed_revision); put(this->retained_precomputed_transfer);
		put(this->retained_precomputed_services.size()); for (const auto &[id, service] : this->retained_precomputed_services) put_service(service);
		put(this->precomputed_hyperpath); put(this->retained_precomputed_hyperpath);
		put(this->precomputed_policy_generation); put(this->retained_precomputed_policy_generation);
	}
	/* Freeze facility keys with the service snapshots. Twenty-one 3-bit keys fit a word. */
	for (const auto *snapshot : {&this->services, &this->building, &this->precomputed_services, &this->retained_precomputed_services}) {
		for (const auto &[id, service] : *snapshot) for (size_t at = 0; at < service.timetable.stops.size(); at += 21) {
			uint64_t keys = 0;
			for (size_t i = 0; i < 21 && at + i < service.timetable.stops.size(); ++i) keys |= uint64_t(service.timetable.stops[at + i].boarding_facility) << (3 * i);
			put(keys);
		}
	}
	for (const auto &cost : {this->aircraft_cost, this->precomputed_aircraft_cost, this->retained_precomputed_aircraft_cost}) {
		put(cost.scale); put(cost.addition);
	}
	return data;
}

bool PassengerServiceDatabase::Load(std::span<const uint8_t> data)
{
	if (data.size() > 4 * MAX_BYTES || data.size() % 8 != 0) return false;
	size_t cursor = 0;
	bool valid = true;
	auto get = [&]() -> uint64_t {
		if (cursor + 8 > data.size()) { valid = false; return 0; }
		uint64_t value = 0;
		for (uint i = 0; i < 8; ++i) value |= uint64_t(data[cursor++]) << (8 * i);
		return value;
	};
	auto count = [&](size_t max) { uint64_t n = get(); if (n > max || n > (data.size() - cursor) / 8) { valid = false; return size_t{0}; } return static_cast<size_t>(n); };
	PassengerServiceDatabase restored;
	const uint64_t version = get();
	restored.legacy_aircraft_times = version == 1;
	if (version != 1 && version != DATABASE_FORMAT_VERSION) return false;
	restored.generation = get();
	uint64_t saved_cursor = get(); if (saved_cursor > OrderListID::Invalid().base()) return false;
	restored.cursor = static_cast<uint32_t>(saved_cursor);
	uint64_t sweeping = get(); restored.sweeping = sweeping != 0;
	uint64_t dirty_cursor = get(); if (dirty_cursor >= OrderListID::Invalid().base()) return false;
	restored.dirty_cursor = static_cast<uint16_t>(dirty_cursor);
	restored.sweeping_since = StateTicks{static_cast<int64_t>(get())};
	if (restored.generation == UINT64_MAX || restored.cursor > OrderListID::Invalid().base() || sweeping > 1) return false;
	for (size_t n = count(OrderListID::Invalid().base()); n > 0 && valid; --n) {
		uint64_t id = get(); if (id >= OrderListID::Invalid().base() || !restored.dirty.insert(OrderListID{static_cast<uint16_t>(id)}).second) return false;
		restored.dirty_since.emplace(OrderListID{static_cast<uint16_t>(id)}, StateTicks{static_cast<int64_t>(get())});
	}
	OrderListID loaded_id;
	auto get_service = [&](auto &network, size_t &network_bytes) -> bool {
		PassengerPeriodicService service;
		uint64_t id = get(); if (id >= OrderListID::Invalid().base()) return false;
		service.id = loaded_id = OrderListID{static_cast<uint16_t>(id)};
		service.incarnation = get(); service.generation = get(); service.timetable.period = static_cast<int64_t>(get());
		if (service.generation == 0 || service.generation > restored.generation || service.timetable.period < 0 || service.timetable.period > MAX_PERIOD) return false;
		service.timetable.estimated_period = static_cast<int64_t>(get());
		if (service.timetable.estimated_period < 0 || service.timetable.estimated_period > MAX_PERIOD ||
				(service.timetable.period > 0 && service.timetable.estimated_period != 0)) return false;
		int64_t previous = 0;
		for (size_t stops = count(4096); stops > 0 && valid; --stops) {
			PassengerPeriodicStop stop;
			uint64_t station = get(), order = get();
			if (station >= StationID::Invalid().base() || order >= INVALID_VEH_ORDER_ID) return false;
			stop.station = StationID{static_cast<uint16_t>(station)}; stop.order = static_cast<VehicleOrderID>(order);
			stop.arrival = static_cast<int64_t>(get()); stop.departure = static_cast<int64_t>(get());
			stop.board = get(); stop.alight = get(); stop.forced = get();
			uint64_t continues = get(), quality = get();
			if (continues > 1 || quality > 2 || (service.timetable.period > 0 && (!continues || quality != 0))) return false;
			stop.continues = continues != 0; stop.travel_quality = static_cast<uint8_t>(quality);
			if (version >= 2) { stop.intermediate_wait = static_cast<int64_t>(get()); if (stop.intermediate_wait < 0 || stop.intermediate_wait > MAX_PERIOD) return false; }
			if (stop.arrival < previous || stop.departure < stop.arrival || stop.departure > service.timetable.Circuit()) return false;
			previous = stop.departure; service.timetable.stops.push_back(stop);
		}
		for (size_t fleet = count(65536); fleet > 0 && valid; --fleet) {
			PassengerPeriodicVehicle vehicle;
			uint64_t vehicle_id = get(); if (vehicle_id >= VehicleID::Invalid().base()) return false;
			vehicle.id = VehicleID{static_cast<uint32_t>(vehicle_id)}; vehicle.base = StateTicks{static_cast<int64_t>(get())};
			uint64_t phase = get(), type = get(); if (phase > 1 || type >= to_underlying(VehicleType::CompanyEnd)) return false;
			vehicle.phase_known = phase != 0; vehicle.type = static_cast<VehicleType>(type);
			for (uint64_t &capacity : vehicle.capacity) capacity = get();
			if (vehicle.phase_known && service.timetable.period == 0) return false;
			service.vehicles.push_back(vehicle);
		}
		if (!std::is_sorted(service.vehicles.begin(), service.vehicles.end(), [](const auto &a, const auto &b) { return a.id < b.id; })) return false;
		service.dispatch_base = StateTicks{static_cast<int64_t>(get())}; service.dispatch_floor = StateTicks{static_cast<int64_t>(get())}; service.dispatch_period = static_cast<int64_t>(get()); service.dispatch_stop = get();
		if (service.dispatch_period < 0 || service.dispatch_period > MAX_PERIOD) return false;
		for (size_t slots = count(65536); slots > 0 && valid; --slots) {
			uint64_t slot = get(); if (slot > UINT32_MAX || slot >= static_cast<uint64_t>(service.dispatch_period)) return false;
			service.dispatch_slots.push_back(static_cast<uint32_t>(slot));
		}
		if (service.dispatch_period && (service.timetable.period == 0 || service.dispatch_slots.empty() || service.vehicles.size() != 1 || service.dispatch_stop >= service.timetable.stops.size())) return false;
		service.uncertain_dispatch_period = static_cast<int64_t>(get());
		if (service.uncertain_dispatch_period < 0 || service.uncertain_dispatch_period > MAX_PERIOD) return false;
		size_t bytes = ServiceBytes(service);
		if (network_bytes + bytes > MAX_BYTES || !network.emplace(service.id, std::move(service)).second) return false;
		network_bytes += bytes; return valid;
	};
	for (size_t n = count(OrderListID::Invalid().base()); n > 0 && valid; --n) if (!get_service(restored.services, restored.bytes)) return false;
	restored.published_revision = get(); restored.batch_started = StateTicks{static_cast<int64_t>(get())};
	uint64_t publishing = get(); if (publishing > 1 || restored.published_revision == UINT64_MAX) return false;
	restored.publishing = publishing != 0;
	uint64_t bootstrap = get(); if (bootstrap > 1) return false; restored.bootstrap = bootstrap != 0;
	for (size_t n = count(OrderListID::Invalid().base()); n > 0 && valid; --n) {
		uint64_t id = get(); if (id >= OrderListID::Invalid().base()) return false;
		OrderListID key{static_cast<uint16_t>(id)};
		if (!restored.build_queue.insert(key).second) return false;
		restored.build_since.emplace(key, StateTicks{static_cast<int64_t>(get())});
	}
	for (size_t n = count(OrderListID::Invalid().base()); n > 0 && valid; --n) {
		uint64_t id = get(); if (id >= OrderListID::Invalid().base() || !restored.unsafe.insert(OrderListID{static_cast<uint16_t>(id)}).second) return false;
	}
	for (size_t n = count(OrderListID::Invalid().base()); n > 0 && valid; --n) {
		size_t before = restored.building.size();
		if (!get_service(restored.building, restored.building_bytes) || restored.building.size() != before + 1) return false;
		/* The wire is ordered by service ID, independently of map allocation. */
		OrderListID id = loaded_id;
		if (restored.build_queue.contains(id)) return false;
		const auto &staged = restored.building.at(id); const auto *old = restored.Get(id);
		size_t publication_work = 64 + 2 * (staged.timetable.stops.size() + (old == nullptr ? 0 : old->timetable.stops.size())) +
				4 * staged.vehicles.size() + (staged.dispatch_slots.size() + 15) / 16;
		if (publication_work > PRECOMPUTED_CAPTURE_WORK) return false;
		restored.staged_since.emplace(id, StateTicks{static_cast<int64_t>(get())});
	}
	if (restored.publishing && restored.building.empty()) return false;

	restored.precomputed_operations = get(); restored.precomputed_transfer = static_cast<int64_t>(get());
	const bool started = restored.precomputed_operations != UINT64_MAX;
	if (restored.precomputed_transfer < 0 || restored.precomputed_transfer > 65535) return false;
	if (restored.precomputed_operations == UINT64_MAX) restored.precomputed_operations = 0;
	restored.precomputed_revision = get(); restored.precomputed_refresh = StateTicks{static_cast<int64_t>(get())};
	if (started != (restored.precomputed_revision != UINT64_MAX)) return false;
	uint64_t invalidated = get(); if (invalidated > 1) return false;
	restored.precomputed_invalidated = invalidated != 0;
	size_t bytes = 0;
	for (size_t n = count(OrderListID::Invalid().base()); n > 0 && valid; --n) if (!get_service(restored.precomputed_services, bytes)) return false;
	if (restored.precomputed_revision == UINT64_MAX) {
		if (!restored.precomputed_services.empty() || restored.precomputed_operations != 0) return false;
	} else if (restored.precomputed_revision > restored.published_revision) return false;
	restored.retained_precomputed_revision = get(); restored.retained_precomputed_transfer = static_cast<int64_t>(get());
	if (restored.retained_precomputed_transfer < 0 || restored.retained_precomputed_transfer > 65535) return false;
	bytes = 0;
	for (size_t n = count(OrderListID::Invalid().base()); n > 0 && valid; --n) if (!get_service(restored.retained_precomputed_services, bytes)) return false;
	if (restored.retained_precomputed_revision == UINT64_MAX) {
		if (!restored.retained_precomputed_services.empty() || restored.retained_precomputed_transfer != 0) return false;
	} else if (restored.precomputed_revision == UINT64_MAX || restored.retained_precomputed_revision > restored.precomputed_revision) return false;

	uint64_t current = get(), retained = get();
	if (current > 1 || retained > 1) return false;
	restored.precomputed_hyperpath = current != 0; restored.retained_precomputed_hyperpath = retained != 0;
	restored.precomputed_policy_generation = get(); restored.retained_precomputed_policy_generation = get();
	if ((current && restored.precomputed_policy_generation == 0) || (retained && restored.retained_precomputed_policy_generation == 0) ||
			restored.retained_precomputed_policy_generation > restored.precomputed_policy_generation) return false;
	for (auto *snapshot : {&restored.services, &restored.building, &restored.precomputed_services, &restored.retained_precomputed_services}) {
		for (auto &[id, service] : *snapshot) for (size_t at = 0; at < service.timetable.stops.size(); at += 21) {
			uint64_t keys = get();
			for (size_t i = 0; i < 21 && at + i < service.timetable.stops.size(); ++i) {
				uint8_t key = keys & 7; keys >>= 3; if (key > 5) return false;
				service.timetable.stops[at + i].boarding_facility = key;
			}
			if (keys != 0) return false;
		}
	}
	if (version >= 2) for (auto *cost : {&restored.aircraft_cost, &restored.precomputed_aircraft_cost, &restored.retained_precomputed_aircraft_cost}) {
		uint64_t scale = get(), addition = get();
		if (scale > UINT16_MAX || addition > uint64_t{UINT16_MAX} * DAY_TICKS * UINT8_MAX) return false;
		*cost = {static_cast<uint16_t>(scale), static_cast<uint32_t>(addition)};
	}
	if (!valid || cursor != data.size()) return false;
	/* Validate physical segment waits before constructing signed preference prefixes. */
	for (auto *snapshot : {&restored.services, &restored.building, &restored.precomputed_services, &restored.retained_precomputed_services}) {
		for (auto &[id, service] : *snapshot) {
			const auto &t = service.timetable;
			for (size_t i = 0; i < t.stops.size(); ++i) {
				size_t next = (i + 1) % t.stops.size();
				if (t.stops[next].intermediate_wait > t.stops[next].arrival - t.stops[i].departure + (next == 0 ? t.Circuit() : 0)) return false;
			}
			service.BuildAircraftCosts(snapshot == &restored.precomputed_services ? restored.precomputed_aircraft_cost :
					snapshot == &restored.retained_precomputed_services ? restored.retained_precomputed_aircraft_cost : restored.aircraft_cost);
		}
	}
	for (const auto &[id, service] : restored.services) { restored.Index(service); restored.TouchStations(service); }
	restored.service_lookup.resize(OrderListID::Invalid().base(), nullptr);
	for (const auto &[id, service] : restored.services) restored.service_lookup[id.base()] = &service;
	for (OrderListID id : restored.unsafe) restored.unsafe_lookup.set(id.base());
	restored.restoring = true;
	restored.policy_cache_enabled = this->policy_cache_enabled; // Local optional preference survives loading; absent from simulation state.
	restored.departure_index_enabled = this->departure_index_enabled;
	restored.timed_scratch_enabled = this->timed_scratch_enabled;
	restored.hyperpath_boarding_filter = this->hyperpath_boarding_filter;
	restored.hyperpath_lookup_epoch = this->hyperpath_lookup_epoch + 1; restored.hyperpath_destination_changes = this->hyperpath_destination_changes + 1;
	*this = std::move(restored);
	return true;
}

uint64_t PassengerServiceDatabase::GetLoadProgress() const
{
	uint64_t graph = 0;
	for (const auto &table : this->precomputed) graph += table.GetGraphWork();
	for (const auto &table : this->retained_precomputed) graph += table.GetGraphWork();
	/* The cap is a finite reporting credit, not a claim that remote work is
	 * trustworthy. The server's independent absolute deadline always applies. */
	return this->load_work + std::min<uint64_t>(graph, UINT32_MAX);
}

uint64_t PassengerServiceDatabase::GetLoadWorkLimit() const
{
	uint64_t work = this->precomputed_operations + uint64_t{UINT32_MAX};
	for (const auto &table : this->retained_precomputed) work += table.GetDiagnostic().operations;
	return work;
}

void PassengerServiceDatabase::FinishLoad(bool present, bool defer_precomputed)
{
	/* v1 has no movement/wait split. Recapture once, including at default costs,
	 * so a subsequent settings change or save cannot scale its unknown waits. */
	if (!present || this->legacy_aircraft_times) { this->Reset(); return; }
	this->restoring = false;
	this->load_state = LoadState::Ready;
	this->load_work = 0;
	if (this->precomputed_revision != UINT64_MAX) {
		this->load_remaining = this->precomputed_operations;
		this->load_retained = this->retained_precomputed_revision != UINT64_MAX;
		this->load_cargo = CargoType{0};
		if (this->retained_precomputed_revision != UINT64_MAX) {
			this->BuildPrecomputedTables(this->retained_precomputed, this->retained_precomputed_services,
					this->retained_precomputed_transfer, this->retained_precomputed_hyperpath ? PassengerPrecomputedTable::MAX_HYPERPATH_BYTES : PassengerPrecomputedTable::MAX_BYTES, this->retained_precomputed_hyperpath, defer_precomputed, this->retained_precomputed_aircraft_cost);
		}
		this->load_current_graph_started = !defer_precomputed;
		if (!defer_precomputed) this->BuildPrecomputedTables();
		/* Keep the serialized cursor intact throughout replay. No save or
		 * network command may observe CPU-dependent partial replay progress. */
		this->precomputed_operations = this->load_remaining;
		this->load_graph_refreshed = false;
		this->load_state = LoadState::Replaying;
	}
	if (!defer_precomputed) {
		while (this->StepLoad() == LoadState::Replaying) {}
		if (this->load_state == LoadState::Invalid) SlErrorCorrupt("Invalid passenger routing construction cursor");
	}
	/* Pool construction/AfterLoad may call invalidation hooks. Those must not
	 * consume saved publication progress or recapture phases on a network join. */
}

PassengerServiceDatabase::LoadState PassengerServiceDatabase::StepLoad(size_t allowance)
{
	if (this->load_state != LoadState::Replaying) return this->load_state;
	++this->hyperpath_lookup_epoch; // Replay publishes rows in both tables without per-row notifications.
	if (!this->load_graph_refreshed) {
		for (auto &table : this->retained_precomputed) {
			if (table.IsPreparingGraph() && !table.PrepareGraph(1024)) return this->load_state;
		}
		if (!this->load_current_graph_started) {
			this->cooperative_graph = true; this->BuildPrecomputedTables(); this->cooperative_graph = false;
			this->precomputed_operations = this->load_remaining;
			this->load_current_graph_started = true;
		}
		for (auto &table : this->precomputed) {
			if (table.IsPreparingGraph() && !table.PrepareGraph(1024)) return this->load_state;
		}
		for (const auto &[id, service] : this->services) {
			for (auto &table : this->precomputed) table.RefreshService(service);
			for (auto &table : this->retained_precomputed) table.RefreshService(service);
		}
		this->load_graph_refreshed = true;
	}
	while (true) {
		if (this->load_cargo == NUM_CARGO || (!this->load_retained && this->load_remaining == 0)) {
			if (this->load_retained) { this->load_retained = false; this->load_cargo = CargoType{0}; continue; }
			if (this->load_remaining != 0) return this->load_state = LoadState::Invalid;
			return this->load_state = LoadState::Ready;
		}
		auto &table = (this->load_retained ? this->retained_precomputed : this->precomputed)[this->load_cargo];
		if (table.Finished()) { ++this->load_cargo; continue; }
		if (allowance == 0) return this->load_state;
		size_t used;
		if (this->load_retained) {
			used = table.Step(allowance);
		} else {
			used = table.Step(static_cast<size_t>(std::min<uint64_t>(this->load_remaining, allowance)), false,
					[&](StationID destination) {
						auto &retained = this->retained_precomputed[this->load_cargo];
						retained.PrepareRetirement(table); retained.ReleaseDestination(destination);
					});
			this->load_remaining -= used;
		}
		this->load_work += used;
		allowance -= used;
	}
}
