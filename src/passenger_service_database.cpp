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
#include "date_func.h"
#include "settings_type.h"
#include <chrono>
#include <queue>
#include <bitset>

#include "safeguards.h"

namespace {
	PassengerServiceDatabase database;
	constexpr int64_t MAX_PERIOD = int64_t{1} << 40;
	bool AddTicks(int64_t &value, int64_t delta)
	{
		if (delta < 0 || value > INT64_MAX - delta) return false;
		value += delta;
		return true;
	}
	size_t ServiceBytes(const PassengerPeriodicService &s)
	{
		/* Include vector slack, station map/set nodes and each set's sentinel even
		 * when every stop introduces a different station. */
		return 512 + 512 * s.timetable.stops.size() + 2 * (
				s.vehicles.size() * sizeof(PassengerPeriodicVehicle) + s.dispatch_slots.size() * sizeof(uint32_t));
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
	auto phase = std::find_if(service->vehicles.begin(), service->vehicles.end(), [&](const auto &v) { return v.id == vehicle->index; });
	if (phase == service->vehicles.end()) return;
	std::array<uint64_t, NUM_CARGO> capacity{};
	size_t parts = 4096;
	for (const Vehicle *part = vehicle; part != nullptr; part = part->Next()) {
		if (parts-- == 0) return;
		if (part->cargo_type < NUM_CARGO) capacity[part->cargo_type] += part->cargo_cap;
	}
	if (capacity != phase->capacity) { database.Invalidate(service->id); return; }
	if (service->timetable.period == 0 || phase->phase_known || vehicle->vehicle_flags.Test(VehicleFlag::ScheduledDispatch)) return;
	if (std::none_of(service->timetable.stops.begin(), service->timetable.stops.end(), [&](const auto &stop) { return stop.order == vehicle->cur_real_order_index && stop.station == vehicle->last_station_visited; })) return;
	if (vehicle->HasUnbunchingOrder()) return;
	size_t work = 4096;
	auto state = GetPassengerRouteState(vehicle, &work);
	/* Observing the phase must not remove a safe estimated intent just before
	 * its real carrier boards. Publish the newly observed phase at the ordinary
	 * timing epoch, retaining the live incarnation and occurrence permissions. */
	if (state && state->loading && std::all_of(state->usable_stops.begin(), state->usable_stops.end(), [](uint8_t usable) { return usable != 0; })) database.TimetableChanged(service->id);
}

void PassengerServiceDatabase::ShiftTicks(StateTicksDelta delta)
{
	for (auto &[id, service] : this->services) {
		service.dispatch_base += delta;
		service.dispatch_floor += delta;
		for (auto &vehicle : service.vehicles) vehicle.base += delta;
	}
	this->sweeping_since += delta;
	for (auto &[id, since] : this->dirty_since) since += delta;
}

PassengerPeriodicTimetable PassengerPeriodicTimetable::Compile(std::span<const Order> orders)
{
	PassengerPeriodicTimetable result;
	int64_t cursor = 0, first_travel = 0;
	bool first = true;
	for (size_t i = 0; i < orders.size(); ++i) {
		const Order &order = orders[i];
		if (order.IsType(OT_IMPLICIT) || order.IsType(OT_LABEL)) continue;
		const bool depot = order.IsType(OT_GOTO_DEPOT);
		if (depot && order.IsScheduledDispatchOrder(true)) return {}; // No station occurrence can anchor this dispatch.
		if ((depot ? ClassifyPassengerDepot(order) != PassengerDepotTraversal::Timed :
				!order.IsType(OT_GOTO_STATION) && !order.IsType(OT_GOTO_WAYPOINT)) || order.IsRefit() ||
				!order.IsTravelTimetabled()) return {};
		if (first) { first_travel = order.GetTravelTime(); first = false; }
		else if (!AddTicks(cursor, order.GetTravelTime())) return {};
		bool stopping = order.IsType(OT_GOTO_STATION) && !(order.GetNonStopType() & ONSF_NO_STOP_AT_DESTINATION_STATION);
		if (stopping) {
			if (!order.IsWaitTimetabled()) return {};
			PassengerPeriodicStop stop;
			stop.station = order.GetDestination().ToStationID(); stop.order = static_cast<VehicleOrderID>(i);
			stop.arrival = cursor;
			if (!AddTicks(cursor, order.GetWaitTime())) return {};
			stop.departure = cursor;
			for (CargoType cargo : EnumRange(NUM_CARGO)) {
				OrderUnloadType unload = order.GetCargoUnloadType(cargo);
				if (unload == OrderUnloadType::Unload || unload == OrderUnloadType::Transfer) stop.forced |= uint64_t{1} << cargo;
				if (unload != OrderUnloadType::NoUnload) stop.alight |= uint64_t{1} << cargo;
				if (order.GetCargoLoadType(cargo) != OrderLoadType::NoLoad && !(stop.forced & (uint64_t{1} << cargo))) stop.board |= uint64_t{1} << cargo;
			}
			result.stops.push_back(stop);
		} else if (depot || order.GetWaitTime() != 0) {
			if (!order.IsWaitTimetabled() || !AddTicks(cursor, order.GetWaitTime())) return {};
		}
		if (cursor > MAX_PERIOD) return {};
	}
	if (!AddTicks(cursor, first_travel) || cursor == 0 || cursor > MAX_PERIOD || result.stops.size() < 2) return {};
	/* The phase origin is the arrival at the first explicit stopping order. */
	int64_t origin = result.stops.front().arrival;
	for (auto &stop : result.stops) { stop.arrival -= origin; stop.departure -= origin; }
	result.period = cursor;
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

void PassengerServiceDatabase::ClearPolicies() { this->journey_policies.clear(); this->policy_bytes = 0; this->policy_clock = 0; this->schedule.Clear(); }
void PassengerServiceDatabase::Reset()
{
	ClearPassengerOdFallbackCache();
	this->services.clear(); this->stations.clear(); this->dirty.clear(); this->dirty_since.clear(); this->ClearPolicies();
	this->sweeping_since = _state_ticks;
	this->cursor = 0; this->dirty_cursor = 65534; this->sweeping = true; this->restoring = false; this->generation = 0; this->bytes = 0;
	this->diagnostic = {};
	this->schedule.ResetDiagnostic();
}

void PassengerServiceDatabase::Remove(OrderListID id)
{
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
	++this->diagnostic.invalidations;
	this->ClearPolicies();
	if (id == OrderListID::Invalid()) {
		++this->diagnostic.global_invalidations;
		this->services.clear(); this->stations.clear(); this->dirty.clear(); this->dirty_since.clear(); this->bytes = 0;
		this->sweeping_since = _state_ticks;
		this->cursor = 0; this->sweeping = true;
	} else {
		this->Remove(id);
		this->dirty.insert(id);
		this->dirty_since.try_emplace(id, _state_ticks);
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
	/* A timing edit changes estimates, not real load/unload permission. Preserve
	 * the published cycle and policies while coalescing automatic measurements.
	 * Order/facility/fleet mutations still remove their affected records at once. */
	this->dirty.insert(id);
	this->dirty_since.try_emplace(id, _state_ticks);
}

void PassengerServiceDatabase::Tick(size_t allowance)
{
	if (this->restoring || !_settings_game.linkgraph.passenger_journey_routing || !_settings_game.linkgraph.passenger_journey_periodic) return;
	PassengerJourneyWorkTimer timer;
	const auto started = std::chrono::steady_clock::now();
	const size_t original = allowance;
	auto observe_age = [&](StateTicks since) {
		if (_state_ticks >= since) this->diagnostic.maximum_pending_ticks = std::max(this->diagnostic.maximum_pending_ticks, uint64_t(_state_ticks.base()) - uint64_t(since.base()));
	};
	if (this->sweeping) observe_age(this->sweeping_since);
	size_t deferred_examined = 0;
	while (allowance > 0) {
		OrderListID id;
		bool from_dirty = !this->dirty.empty() && deferred_examined < this->dirty.size();
		if (from_dirty) {
			auto next = this->dirty.upper_bound(OrderListID{this->dirty_cursor});
			if (next == this->dirty.end()) next = this->dirty.begin();
			id = *next;
			observe_age(this->dirty_since.at(id));
		}
		else {
			if (!this->sweeping) break;
			if (this->cursor >= OrderListID::Invalid().base()) { this->sweeping = false; break; }
			id = OrderListID{static_cast<uint16_t>(this->cursor)};
		}
		const OrderList *list = OrderList::GetIfValid(id);
		if (from_dirty && this->services.contains(id)) {
			/* Publish coalesced timing in epochs, then finish the batch with the
			 * ordinary build budget. Repeated edits never reset its first epoch. */
			auto epoch = [](StateTicks ticks) { return ticks.base() / TIMETABLE_EPOCH - (ticks.base() % TIMETABLE_EPOCH < 0); };
			if (epoch(_state_ticks) <= epoch(this->dirty_since.at(id))) {
				--allowance; this->dirty_cursor = id.base(); ++deferred_examined; continue;
			}
			this->Remove(id); this->ClearPolicies();
		}
		if (list == nullptr || this->services.contains(id)) {
			--allowance;
			if (from_dirty) { this->dirty.erase(id); this->dirty_since.erase(id); this->dirty_cursor = id.base(); } else ++this->cursor;
			continue;
		}
		/* Atomic service tasks have a fixed upper envelope. A pathological list
		 * cannot stall the queue or allocate outside the per-service ceiling. */
		std::vector<const Vehicle *> fleet;
		size_t work = 64 + list->GetNumOrders() * (NUM_CARGO + 32);
		size_t preflight_work = 64;
		if (work <= BUILD_WORK) for (const DispatchSchedule &schedule : list->GetScheduledDispatchScheduleSet()) {
			work += 16 + schedule.GetScheduledDispatch().size(); preflight_work += 16;
			if (work > BUILD_WORK) break;
		}
		if (work <= BUILD_WORK) {
			for (const Vehicle *v = list->GetFirstSharedVehicle(); v != nullptr; v = v->NextShared()) {
				fleet.push_back(v);
				preflight_work += 32;
				work += 3 * NUM_CARGO + 32;
				for (const Vehicle *part = v; part != nullptr && work <= BUILD_WORK; part = part->Next()) { work += 4; preflight_work += 4; }
				if (work > BUILD_WORK) break;
			}
		}
		const size_t charged_work = work > BUILD_WORK ? std::min(BUILD_WORK, preflight_work) : work;
		if (charged_work > allowance) break;
		std::sort(fleet.begin(), fleet.end(), [](const auto *a, const auto *b) { return a->index < b->index; });
		if (from_dirty) { this->dirty.erase(id); this->dirty_since.erase(id); this->dirty_cursor = id.base(); } else ++this->cursor;
		PassengerPeriodicService service; service.id = id;
		assert(this->generation != UINT64_MAX);
		service.incarnation = list->GetCargoDistRouteGeneration(); service.generation = ++this->generation;
		if (work > BUILD_WORK) { ++this->diagnostic.rejected_work; allowance -= charged_work; }
		else {
			allowance -= work;
			service.timetable = PassengerPeriodicTimetable::Compile(list->GetOrderVector());
			if (ServiceBytes(service) + this->bytes > MAX_BYTES) { service.timetable = {}; ++this->diagnostic.rejected_memory; }
			for (const Vehicle *v : fleet) {
				if (service.timetable.stops.empty()) break;
				if (!IsPassengerServiceVehicle(v) || v->vehstatus.Any({VehState::Stopped, VehState::Crashed})) continue;
				if (v->type == VehicleType::Aircraft && Aircraft::From(v)->flags.Test(VehicleAirFlag::DestinationTooFar)) continue;
				PassengerPeriodicVehicle phase; phase.id = v->index; phase.type = v->type;
				for (const Vehicle *part = v; part != nullptr; part = part->Next()) {
					if (part->cargo_type < NUM_CARGO) phase.capacity[part->cargo_type] += part->cargo_cap;
				}
				/* Capture an actual known order phase once. Subsequent movement,
				 * occupancy and lateness do not regenerate a future Visit column. */
				auto stops = std::span(service.timetable.stops);
				auto stop = std::find_if(stops.begin(), stops.end(), [&](const auto &s) { return s.order == v->cur_real_order_index; });
				if (stop != stops.end() && v->current_order_time <= static_cast<uint64_t>(INT64_MAX) &&
						(v->current_order.IsAnyLoadingType() || v->current_order.IsType(OT_GOTO_STATION))) {
					int64_t delta = int64_t(v->current_order_time) + stop->arrival;
					if (_state_ticks.base() >= INT64_MIN + delta) {
						int64_t base = _state_ticks.base() - delta;
						if (v->current_order.IsAnyLoadingType() || AddTicks(base, list->GetOrderAt(stop->order)->GetTravelTime())) {
							phase.base = StateTicks{base}; phase.phase_known = true;
						}
					}
				}
				if (v->HasUnbunchingOrder()) phase.phase_known = false;
				/* A phase is trusted only for a certified live explicit order state.
				 * Facility checks are bounded and never forecast implicit stops. */
				size_t facility_work = std::min<size_t>(allowance, 4096);
				const size_t facility_allowance = facility_work;
				auto state = GetPassengerRouteState(v, &facility_work);
				bool unusable = state && facility_work != 0 && std::any_of(state->usable_stops.begin(), state->usable_stops.end(), [](uint8_t usable) { return usable == 0; });
				if (!state) {
					/* An unknown current occurrence is not a promise that every declared
					 * facility is usable. Check static vehicle/facility compatibility
					 * without needing a current phase. Exhaustion remains uncertainty. */
					for (const auto &stop : service.timetable.stops) {
						if (facility_work == 0) break;
						if (!CanPassengerVehicleUseStation(v, stop.station, list->GetOrderAt(stop.order), facility_work) && facility_work != 0) { unusable = true; break; }
					}
				}
				allowance -= facility_allowance - facility_work;
				if (unusable) continue;
				if (!state || !std::all_of(state->usable_stops.begin(), state->usable_stops.end(), [](uint8_t usable) { return usable != 0; })) phase.phase_known = false;
				service.vehicles.push_back(phase);
			}
			std::sort(service.vehicles.begin(), service.vehicles.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
			bool dispatch = false;
			for (const Vehicle *v = list->GetFirstSharedVehicle(); v != nullptr; v = v->NextShared()) dispatch |= v->vehicle_flags.Test(VehicleFlag::ScheduledDispatch);
			if (dispatch) {
				service.uncertain_dispatch_period = service.timetable.period;
				for (const auto &stop : service.timetable.stops) {
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
						if (slot.offset >= duration || slot.flags != 0) valid = false;
						service.dispatch_slots.push_back(slot.offset);
					}
					if (service.dispatch_slots.empty()) valid = false;
					for (size_t i = 0; i < service.dispatch_slots.size(); ++i) {
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
		if (ServiceBytes(service) + this->bytes > MAX_BYTES) { ++this->diagnostic.rejected_memory; continue; }
		this->bytes += ServiceBytes(service); this->Index(service);
		this->services.emplace(id, std::move(service)); ++this->diagnostic.builds;
		this->ClearPolicies();
	}
	this->diagnostic.build_work += original - allowance;
	this->diagnostic.build_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
}

const PassengerPeriodicService *PassengerServiceDatabase::Get(OrderListID id) const
{
	auto found = this->services.find(id);
	return found == this->services.end() ? nullptr : &found->second;
}

const std::set<PassengerServiceDatabase::StopRef> &PassengerServiceDatabase::StopsAt(StationID station) const
{
	static const std::set<StopRef> empty;
	auto row = this->stations.find(station);
	return row == this->stations.end() ? empty : row->second;
}

bool PassengerServiceDatabase::IsCurrent(const PassengerJourneyExecution &ride) const
{
	const auto *service = this->Get(ride.service);
	return service != nullptr && service->incarnation == ride.signature && service->generation == ride.generation;
}

PassengerServiceDatabase::JourneyPolicy PassengerServiceDatabase::CompilePolicy(CargoType cargo, StationID origin,
		StationID destination, PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation) const
{
	JourneyPolicy policy;
	policy.bytes = 256;
	auto spend = [&]() { if (policy.work >= limits.work) { policy.limited = true; return false; } ++policy.work; return true; };
	/* A destination absent from the published timed station index cannot have a
	 * timed candidate. Do not expand the national timed network for that OD.
	 * This says nothing about untimed services or later published generations. */
	if (!spend() || !this->stations.contains(origin) || !this->stations.contains(destination)) return policy;
	bool known_alighting = false;
	for (const auto &ref : this->stations.at(destination)) {
		if (!spend()) return policy;
		const auto &service = this->services.at(ref.service);
		if (!(service.timetable.stops[ref.stop].alight & (uint64_t{1} << cargo))) continue;
		for (const auto &vehicle : service.vehicles) {
			if (!spend()) return policy;
			if (vehicle.phase_known && vehicle.capacity[cargo] != 0) { known_alighting = true; break; }
		}
		if (known_alighting) break;
	}
	if (!known_alighting) return policy;
	/* Cheap connectivity over the station/service incidence index rejects isolated
	 * timed components before the more expensive alternative-path queue. Cargo
	 * direction masks exclude operating-only events even from this overapproximation.
	 * This is part of cold policy work and is replayed identically on cache hits. */
	constexpr size_t FLOOD_BITS = sizeof(std::bitset<65536>) * 2;
	if (256 + FLOOD_BITS + 4 * this->stations.size() <= limits.memory_bytes) {
		std::bitset<65536> visited_stations, visited_services;
		std::vector<StationID> frontier; frontier.reserve(this->stations.size());
		frontier.push_back(origin); visited_stations.set(origin.base());
		bool reached = origin == destination;
		for (size_t i = 0; i < frontier.size() && !reached; ++i) {
			if (!spend()) return policy;
			for (const auto &ref : this->stations.at(frontier[i])) {
				if (!spend()) return policy;
				const auto &service = this->services.at(ref.service);
				if (!(service.timetable.stops[ref.stop].board & (uint64_t{1} << cargo))) continue;
				if (visited_services.test(ref.service.base())) continue;
				visited_services.set(ref.service.base());
				bool timed = false;
				for (const auto &vehicle : service.vehicles) {
					if (!spend()) return policy;
					if (vehicle.phase_known && vehicle.capacity[cargo]) { timed = true; break; }
				}
				if (!timed) continue;
				for (const auto &stop : service.timetable.stops) {
					if (!spend()) return policy;
					if (!(stop.alight & (uint64_t{1} << cargo))) continue;
					if (stop.station == destination) { reached = true; break; }
					if (visited_stations.test(stop.station.base())) continue;
					visited_stations.set(stop.station.base()); frontier.push_back(stop.station);
				}
				if (reached) break;
			}
		}
		if (!reached) return policy;
	}
	/* Candidates are timeless service occurrences. Absolute departures, physical
	 * vehicle IDs and a previously winning route are never cached here. */
	struct Path { StationID station; int64_t cost; std::vector<PolicyRide> rides; };
	std::vector<Path> paths{{origin, 0, {}}};
	using QueueItem = std::tuple<int64_t, StationID, size_t>;
	std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> queue;
	queue.emplace(0, origin, 0);
	std::map<StationID, size_t> visits;
	size_t memory = 256;
	const size_t candidates = std::clamp<size_t>(limits.candidates, 1, 16);
	while (!queue.empty() && !policy.limited && policy.routes.size() < candidates) {
		if (!spend()) break;
		auto [cost, station, index] = queue.top(); queue.pop();
		Path current = paths[index];
		if (station == destination) {
			if (std::find(policy.routes.begin(), policy.routes.end(), current.rides) == policy.routes.end()) policy.routes.push_back(std::move(current.rides));
			continue;
		}
		if (current.rides.size() > limits.max_transfers) continue;
		if (++visits[station] > limits.labels_per_station) continue;
		auto row = this->stations.find(station);
		if (row == this->stations.end()) continue;
		for (const auto &ref : row->second) {
			if (!spend()) break;
			const auto &service = this->services.at(ref.service);
			const auto &pattern = service.timetable;
			const auto &board = pattern.stops[ref.stop];
			if (!current.rides.empty() && current.rides.back().service == service.id && service.vehicles.size() == 1) continue;
			if (!(board.board & (uint64_t{1} << cargo)) || !std::any_of(service.vehicles.begin(), service.vehicles.end(),
					[&](const auto &v) { return v.phase_known && v.capacity[cargo] != 0; })) continue;
			for (size_t step = 1; step < pattern.stops.size(); ++step) {
				if (!spend()) break;
				size_t stop_index = (ref.stop + step) % pattern.stops.size();
				const auto &alight = pattern.stops[stop_index];
				if (!(alight.alight & (uint64_t{1} << cargo))) continue;
				bool cycle = alight.station == origin;
				for (auto previous = current.rides.rbegin(); !cycle && previous != current.rides.rend(); ++previous) {
					if (!spend()) break;
					cycle = this->services.at(previous->service).timetable.stops[previous->alight].station == alight.station;
				}
				if (!cycle && (alight.alight & (uint64_t{1} << cargo))) {
					int64_t duration = alight.arrival - board.departure;
					if (stop_index <= ref.stop) duration += pattern.period;
					int64_t next_cost = cost;
					if (!AddTicks(next_cost, duration) || (!current.rides.empty() && !AddTicks(next_cost, evaluation.transfer_penalty))) continue;
					size_t bytes = 512 + (current.rides.size() + 1) * sizeof(PolicyRide) * 2;
					if (paths.size() >= limits.labels || memory + bytes > limits.memory_bytes) { policy.limited = true; break; }
					memory += bytes;
					auto rides = current.rides; rides.push_back({service.id, ref.stop, stop_index});
					/* Retain a proven direct alternative before expanding transfer
					 * combinations. A long line must not lose its through ride to a
					 * combinatorial collection of shorter nominal transfer prefixes. */
					if (current.rides.empty() && alight.station == destination && policy.routes.size() < candidates &&
							std::find(policy.routes.begin(), policy.routes.end(), rides) == policy.routes.end()) policy.routes.push_back(rides);
					queue.emplace(next_cost, alight.station, paths.size());
					paths.push_back({alight.station, next_cost, std::move(rides)});
				}
				if (alight.forced & (uint64_t{1} << cargo)) break;
			}
		}
	}
	policy.bytes = 256;
	for (const auto &route : policy.routes) policy.bytes += 64 + 2 * route.size() * sizeof(PolicyRide);
	return policy;
}

PassengerPeriodicPlan PassengerServiceDatabase::Plan(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
		PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, OrderListID excluded)
{
	PassengerPeriodicPlan result;
	if (cargo >= NUM_CARGO || origin == destination || !Station::IsValidID(origin) || !Station::IsValidID(destination) ||
			evaluation.minimum_transfer < 0 || evaluation.transfer_penalty < 0) return result;
	/* Active uses a capacity upper bound and demand of one, with no occupancy
	 * forecast penalty. Unsupported provider evaluations remain explicitly unknown. */
	if (evaluation.crowding_penalty != 0 || evaluation.capacity_shortfall_penalty != 0) { result.deferred = true; return result; }
	const auto started = std::chrono::steady_clock::now(); ++this->diagnostic.searches;
	PolicyKey key{cargo, origin, destination, limits, evaluation};
	auto cached = this->journey_policies.find(key);
	JourneyPolicy temporary;
	const JourneyPolicy *policy;
	if (cached != this->journey_policies.end()) { cached->second.last_used = ++this->policy_clock; policy = &cached->second; ++this->diagnostic.policy_hits; }
	else {
		auto compilation = limits; compilation.work /= 2; // Reserve bounded work for evaluating retained alternatives.
		temporary = this->CompilePolicy(cargo, origin, destination, compilation, evaluation); policy = &temporary; ++this->diagnostic.policy_misses;
	}
	result.work = policy->work;
	/* Replay compilation work on hits. Optional cache residency/eviction and
	 * unsaved policies cannot alter admission, budgets, ties or the candidates. */
	auto spend = [&]() { if (result.work >= limits.work) { result.deferred = true; return false; } ++result.work; return true; };
	for (const auto &route : policy->routes) {
		if (!route.empty() && route.front().service == excluded) continue;
		StateTicks time = ready;
		int64_t cost = 0;
		bool usable = true;
		PassengerJourneyExecution first;
		for (size_t i = 0; i < route.size(); ++i) {
			if (!spend()) { usable = false; break; }
			const auto &ref = route[i];
			const auto &service = this->services.at(ref.service);
			const auto &pattern = service.timetable;
			const auto &board = pattern.stops[ref.board], &alight = pattern.stops[ref.alight];
			int64_t earliest = time.base();
			if (i && !AddTicks(earliest, evaluation.minimum_transfer)) { usable = false; break; }
			std::optional<StateTicks> departure;
			for (const auto &vehicle : service.vehicles) {
				if (!spend()) { usable = false; break; }
				if (!vehicle.phase_known || vehicle.capacity[cargo] == 0) continue;
				if (service.dispatch_period > 0) {
					int64_t offset = board.departure - pattern.stops[service.dispatch_stop].departure;
					if (offset < 0) offset += pattern.period;
					for (uint32_t slot : service.dispatch_slots) {
						if (!spend()) { usable = false; break; }
						int64_t floor = service.dispatch_floor.base();
						if (!AddTicks(floor, offset)) continue;
						++this->diagnostic.next_departures;
						auto next = PassengerPeriodicTimetable::NextDeparture(service.dispatch_base, slot + offset, service.dispatch_period, StateTicks{std::max(earliest, floor)});
						if (next && (!departure || *next < *departure)) departure = next;
					}
				} else {
					++this->diagnostic.next_departures;
					auto next = PassengerPeriodicTimetable::NextDeparture(vehicle.base, board.departure, pattern.period, StateTicks{earliest});
					if (next && (!departure || *next < *departure)) departure = next;
				}
			}
			if (!usable || !departure) { usable = false; break; }
			int64_t duration = alight.arrival - board.departure;
			if (ref.alight <= ref.board) duration += pattern.period;
			int64_t end = departure->base();
			if (!AddTicks(end, duration) || end < time.base() || uint64_t(end) - uint64_t(time.base()) > INT64_MAX) { usable = false; break; }
			int64_t elapsed = end - time.base(), wait = departure->base() - time.base();
			if (!AddTicks(cost, elapsed) || (i && !AddTicks(cost, evaluation.transfer_penalty)) ||
					(evaluation.waiting_weight && (wait > INT64_MAX / evaluation.waiting_weight || !AddTicks(cost, wait * evaluation.waiting_weight / 1000))) ||
					(i && !AddTicks(cost, int64_t(evaluation.minimum_transfer) * evaluation.transfer_time_weight / 1000))) { usable = false; break; }
			if (i == 0) {
				first = {VehicleID::Invalid(), service.id, board.station, alight.station, board.order, alight.order, *departure, StateTicks{end}, service.incarnation};
				first.kind = PassengerRideKind::ServiceIntent; first.generation = service.generation;
				first.not_before = ready;
			}
			time = StateTicks{end};
		}
		size_t transfers = route.size() - 1;
		if (usable && (!result.ride || std::tie(cost, time, transfers, first.departure, first.service, first.boarding_order, first.alighting_order) <
				std::tie(result.cost, result.arrival, result.transfers, result.ride->departure, result.ride->service, result.ride->boarding_order, result.ride->alighting_order))) {
			result.ride = first; result.arrival = time; result.cost = cost; result.transfers = transfers;
		}
	}
	result.deferred |= policy->limited && !result.ride;
	if (cached == this->journey_policies.end() && temporary.bytes <= POLICY_BYTES) {
		while (!this->journey_policies.empty() && (this->journey_policies.size() >= POLICY_ENTRIES || this->policy_bytes + temporary.bytes > POLICY_BYTES)) {
			auto oldest = std::min_element(this->journey_policies.begin(), this->journey_policies.end(), [](const auto &a, const auto &b) { return a.second.last_used < b.second.last_used; });
			this->policy_bytes -= oldest->second.bytes; this->journey_policies.erase(oldest);
		}
		temporary.last_used = ++this->policy_clock;
		this->policy_bytes += temporary.bytes; this->journey_policies.emplace(key, std::move(temporary));
	}
	this->diagnostic.query_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
	return result;
}

PassengerDatabaseDiagnostic PassengerServiceDatabase::GetDiagnostic() const
{
	auto d = this->diagnostic;
	d.services = this->services.size(); d.bytes = this->bytes; d.policy_bytes = this->policy_bytes;
	d.pending = this->dirty.size();
	if (this->sweeping) for (const OrderList *list : OrderList::Iterate()) if (list->index.base() >= this->cursor && !this->services.contains(list->index) && !this->dirty.contains(list->index)) ++d.pending;
	auto age = [&](StateTicks since) { return _state_ticks >= since ? uint64_t(_state_ticks.base()) - uint64_t(since.base()) : uint64_t{0}; };
	for (const auto &[id, since] : this->dirty_since) d.maximum_pending_ticks = std::max(d.maximum_pending_ticks, age(since));
	if (this->sweeping && d.pending > this->dirty.size()) d.maximum_pending_ticks = std::max(d.maximum_pending_ticks, age(this->sweeping_since));
	uint64_t passenger_mask = 0;
	for (CargoType cargo : EnumRange(NUM_CARGO)) if (IsCargoInClass(cargo, CargoClass::Passengers)) passenger_mask |= uint64_t{1} << cargo;
	for (const auto &[id, service] : this->services) {
		d.vehicles += service.vehicles.size();
		d.known_phase_vehicles += std::count_if(service.vehicles.begin(), service.vehicles.end(), [](const auto &v) { return v.phase_known; });
		d.timed_services += std::any_of(service.vehicles.begin(), service.vehicles.end(), [](const auto &v) { return v.phase_known; });
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
	put(2); put(this->generation); put(this->cursor); put(this->sweeping); put(this->dirty_cursor);
	put(this->sweeping_since.base());
	put(this->dirty.size()); for (OrderListID id : this->dirty) { put(id.base()); put(this->dirty_since.at(id).base()); }
	put(this->services.size());
	for (const auto &[id, service] : this->services) {
		put(id.base()); put(service.incarnation); put(service.generation); put(service.timetable.period);
		put(service.timetable.stops.size());
		for (const auto &stop : service.timetable.stops) {
			put(stop.station.base()); put(stop.order); put(stop.arrival); put(stop.departure);
			put(stop.board); put(stop.alight); put(stop.forced);
		}
		put(service.vehicles.size());
		for (const auto &vehicle : service.vehicles) {
			put(vehicle.id.base()); put(vehicle.base.base()); put(vehicle.phase_known); put(to_underlying(vehicle.type));
			for (uint64_t capacity : vehicle.capacity) put(capacity);
		}
		put(service.dispatch_base.base()); put(service.dispatch_floor.base()); put(service.dispatch_period); put(service.dispatch_stop);
		put(service.dispatch_slots.size()); for (uint32_t slot : service.dispatch_slots) put(slot);
		put(service.uncertain_dispatch_period);
	}
	return data;
}

bool PassengerServiceDatabase::Load(std::span<const uint8_t> data)
{
	if (data.size() > MAX_BYTES || data.size() % 8 != 0) return false;
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
	uint64_t version = get(); if (version != 1 && version != 2) return false;
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
	for (size_t n = count(OrderListID::Invalid().base()); n > 0 && valid; --n) {
		PassengerPeriodicService service;
		uint64_t id = get(); if (id >= OrderListID::Invalid().base()) return false;
		service.id = OrderListID{static_cast<uint16_t>(id)};
		service.incarnation = get(); service.generation = get(); service.timetable.period = static_cast<int64_t>(get());
		if (service.generation == 0 || service.generation > restored.generation || service.timetable.period < 0 || service.timetable.period > MAX_PERIOD) return false;
		int64_t previous = 0;
		for (size_t stops = count(4096); stops > 0 && valid; --stops) {
			PassengerPeriodicStop stop;
			uint64_t station = get(), order = get();
			if (station >= StationID::Invalid().base() || order >= INVALID_VEH_ORDER_ID) return false;
			stop.station = StationID{static_cast<uint16_t>(station)}; stop.order = static_cast<VehicleOrderID>(order);
			stop.arrival = static_cast<int64_t>(get()); stop.departure = static_cast<int64_t>(get());
			stop.board = get(); stop.alight = get(); stop.forced = get();
			if (stop.arrival < previous || stop.departure < stop.arrival || stop.departure > service.timetable.period) return false;
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
		service.dispatch_base = StateTicks{static_cast<int64_t>(get())}; service.dispatch_floor = StateTicks{static_cast<int64_t>(get())}; service.dispatch_period = static_cast<int64_t>(get()); service.dispatch_stop = get();
		if (service.dispatch_period < 0 || service.dispatch_period > MAX_PERIOD) return false;
		for (size_t slots = count(65536); slots > 0 && valid; --slots) {
			uint64_t slot = get(); if (slot > UINT32_MAX || slot >= static_cast<uint64_t>(service.dispatch_period)) return false;
			service.dispatch_slots.push_back(static_cast<uint32_t>(slot));
		}
		if (service.dispatch_period && (service.dispatch_slots.empty() || service.vehicles.size() != 1 || service.dispatch_stop >= service.timetable.stops.size())) return false;
		if (version >= 2) service.uncertain_dispatch_period = static_cast<int64_t>(get());
		if (service.uncertain_dispatch_period < 0 || service.uncertain_dispatch_period > MAX_PERIOD) return false;
		size_t bytes = ServiceBytes(service);
		if (restored.bytes + bytes > MAX_BYTES || !restored.services.emplace(service.id, std::move(service)).second) return false;
		restored.bytes += bytes;
	}
	if (!valid || cursor != data.size()) return false;
	if (version == 1) {
		/* Old publications used unknown phase for both timing uncertainty and
		 * incompatible facilities. Recheck their uncertified fleet with ordinary
		 * build admission. Certified vehicles and the saved sweep keep progress. */
		for (auto it = restored.services.begin(); it != restored.services.end();) {
			auto &service = it->second;
			if (std::none_of(service.vehicles.begin(), service.vehicles.end(), [](const auto &v) { return !v.phase_known; })) { ++it; continue; }
			restored.bytes -= ServiceBytes(service);
			std::erase_if(service.vehicles, [](const auto &v) { return !v.phase_known; });
			restored.dirty.insert(service.id); restored.dirty_since.try_emplace(service.id, restored.sweeping_since);
			if (service.vehicles.empty()) it = restored.services.erase(it);
			else { restored.bytes += ServiceBytes(service); ++it; }
		}
	}
	for (const auto &[id, service] : restored.services) restored.Index(service);
	restored.restoring = true;
	*this = std::move(restored);
	return true;
}

void PassengerServiceDatabase::FinishLoad(bool present)
{
	if (!present) { this->Reset(); return; }
	this->restoring = false;
	/* Pool construction/AfterLoad may call invalidation hooks. Those must not
	 * consume saved publication progress or recapture phases on a network join. */
}
