/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_service_database.cpp One-cycle compilation, bounded publication and on-demand policies. */
#include "stdafx.h"
#include "passenger_service_database.h"
#include "passenger_journey_active.h"
#include "vehicle_base.h"
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
			if (!AddTicks(offset, order.GetWaitTime())) return {};
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
	auto phase = std::find_if(service->vehicles.begin(), service->vehicles.end(), [&](const auto &v) { return v.id == vehicle->index; });
	if (phase == service->vehicles.end()) return;
	std::array<uint64_t, NUM_CARGO> capacity{};
	size_t parts = 4096;
	for (const Vehicle *part = vehicle; part != nullptr; part = part->Next()) {
		if (parts-- == 0) return;
		if (part->cargo_type < NUM_CARGO) capacity[part->cargo_type] += part->cargo_cap;
	}
	if (capacity != phase->capacity) { database.Invalidate(service->id); return; }
	if (service->timetable.period == 0 || vehicle->vehicle_flags.Test(VehicleFlag::ScheduledDispatch)) return;
	if (database.GetPublication(service->id) == PassengerServiceDatabase::Publication::TimingDirty) return;
	if (phase->phase_known && vehicle->current_order_time != 0) return;
	if (std::none_of(service->timetable.stops.begin(), service->timetable.stops.end(), [&](const auto &stop) { return stop.order == vehicle->cur_real_order_index && stop.station == vehicle->last_station_visited; })) return;
	if (vehicle->HasUnbunchingOrder()) return;
	size_t work = 4096;
	auto state = GetPassengerRouteState(vehicle, &work);
	if (state && state->loading && std::all_of(state->usable_stops.begin(), state->usable_stops.end(), [](uint8_t usable) { return usable != 0; })) {
		auto observed = CapturePeriodicBase(vehicle, service->timetable);
		auto modulo = [&](StateTicks ticks) { int64_t value = ticks.base() % service->timetable.period; return value < 0 ? value + service->timetable.period : value; };
		if (observed && (!phase->phase_known || modulo(*observed) != modulo(phase->base))) database.Invalidate(service->id);
	}
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

PassengerPeriodicTimetable PassengerPeriodicTimetable::Compile(std::span<const Order> orders, Failure *failure)
{
	if (failure != nullptr) *failure = Failure::None;
	auto fail = [&](Failure why) { if (failure != nullptr) *failure = why; return PassengerPeriodicTimetable{}; };
	PassengerPeriodicTimetable result;
	int64_t cursor = 0, first_travel = 0;
	bool first = true;
	for (size_t i = 0; i < orders.size(); ++i) {
		const Order &order = orders[i];
		if (order.IsType(OT_IMPLICIT) || order.IsType(OT_LABEL)) continue;
		if (!order.IsType(OT_GOTO_STATION) && !order.IsType(OT_GOTO_WAYPOINT)) return fail(Failure::Control);
		if (order.IsRefit()) return fail(Failure::Refit);
		if (!order.IsTravelTimetabled()) return fail(Failure::Travel);
		if (first) { first_travel = order.GetTravelTime(); first = false; }
		else if (!AddTicks(cursor, order.GetTravelTime())) return fail(Failure::Overflow);
		bool stopping = order.IsType(OT_GOTO_STATION) && !(order.GetNonStopType() & ONSF_NO_STOP_AT_DESTINATION_STATION);
		if (stopping) {
			if (!order.IsWaitTimetabled()) return fail(Failure::Wait);
			PassengerPeriodicStop stop;
			stop.station = order.GetDestination().ToStationID(); stop.order = static_cast<VehicleOrderID>(i);
			stop.arrival = cursor;
			if (!AddTicks(cursor, order.GetWaitTime())) return fail(Failure::Overflow);
			stop.departure = cursor;
			for (CargoType cargo : EnumRange(NUM_CARGO)) {
				OrderUnloadType unload = order.GetCargoUnloadType(cargo);
				if (unload == OrderUnloadType::Unload || unload == OrderUnloadType::Transfer) stop.forced |= uint64_t{1} << cargo;
				if (unload != OrderUnloadType::NoUnload) stop.alight |= uint64_t{1} << cargo;
				if (order.GetCargoLoadType(cargo) != OrderLoadType::NoLoad && !(stop.forced & (uint64_t{1} << cargo))) stop.board |= uint64_t{1} << cargo;
			}
			result.stops.push_back(stop);
		} else if (order.GetWaitTime() != 0) {
			if (!order.IsWaitTimetabled()) return fail(Failure::Wait);
			if (!AddTicks(cursor, order.GetWaitTime())) return fail(Failure::Overflow);
		}
		if (cursor > MAX_PERIOD) return fail(Failure::Overflow);
	}
	if (!AddTicks(cursor, first_travel) || cursor > MAX_PERIOD) return fail(Failure::Overflow);
	if (cursor == 0) return fail(Failure::ZeroPeriod);
	if (result.stops.size() < 2) return fail(Failure::TooFewStops);
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

void PassengerServiceDatabase::ClearPolicies() { this->journey_policies.clear(); this->policy_bytes = 0; this->policy_clock = 0; }
void PassengerServiceDatabase::Reset()
{
	ClearPassengerOdFallbackCache();
	this->services.clear(); this->stations.clear(); this->dirty.clear(); this->dirty_since.clear(); this->ClearPolicies();
	this->sweeping_since = _state_ticks;
	this->cursor = 0; this->dirty_cursor = 65534; this->sweeping = true; this->restoring = false; this->generation = 0; this->bytes = 0;
	this->diagnostic = {};
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
		auto &row = this->stations[service.timetable.stops[i].station];
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
		bool passenger_fleet = false;
		size_t facility_envelope = 0;
		size_t work = 64 + list->GetNumOrders() * (NUM_CARGO + 32);
		size_t preflight_work = 64;
		if (work <= BUILD_WORK) for (const DispatchSchedule &schedule : list->GetScheduledDispatchScheduleSet()) {
			work += 16 + schedule.GetScheduledDispatch().size(); preflight_work += 16;
			if (work > BUILD_WORK) break;
		}
		if (work <= BUILD_WORK) {
			for (const Vehicle *v = list->GetFirstSharedVehicle(); v != nullptr; v = v->NextShared()) {
				fleet.push_back(v);
				if (IsPassengerServiceVehicle(v) && !v->vehstatus.Any({VehState::Stopped, VehState::Crashed})) {
					facility_envelope += v->type == VehicleType::Train ? 2 * list->GetNumOrders() + 1 : 4096;
				}
				preflight_work += 32;
				work += 3 * NUM_CARGO + 32;
				for (const Vehicle *part = v; part != nullptr && work <= BUILD_WORK; part = part->Next()) {
					work += 4; preflight_work += 4;
					passenger_fleet |= part->cargo_type < NUM_CARGO && part->cargo_cap != 0 && IsCargoInClass(part->cargo_type, CargoClass::Passengers);
				}
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
			bool retry_facilities = false;
			bool capture_work_limited = false;
			const bool facilities_fit_tick = facility_envelope <= BUILD_WORK - work;
			PassengerPeriodicTimetable::Failure failure;
			service.timetable = PassengerPeriodicTimetable::Compile(list->GetOrderVector(), &failure);
			if (passenger_fleet) {
				++this->diagnostic.compilation_attempts;
				this->diagnostic.compilation_successes += service.timetable.period != 0;
				++this->diagnostic.compilation_stops[static_cast<size_t>(failure)];
			}
			if (ServiceBytes(service) + this->bytes > MAX_BYTES) { service.timetable = {}; ++this->diagnostic.rejected_memory; }
			for (const Vehicle *v : fleet) {
				if (service.timetable.stops.empty()) break;
				if (!IsPassengerServiceVehicle(v) || v->vehstatus.Any({VehState::Stopped, VehState::Crashed})) continue;
				PassengerPeriodicVehicle phase; phase.id = v->index; phase.type = v->type;
				for (const Vehicle *part = v; part != nullptr; part = part->Next()) {
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
				allowance -= facility_allowance - facility_work;
				if (capture_reason == PassengerPredictionReason::Budget) {
					++this->diagnostic.phase_capture_work; capture_work_limited = true;
					if (facilities_fit_tick && facility_allowance < 4096) retry_facilities = true;
				}
				if (!state || !std::all_of(state->usable_stops.begin(), state->usable_stops.end(), [](uint8_t usable) { return usable != 0; })) phase.phase_known = false;
				service.vehicles.push_back(phase);
			}
			if (retry_facilities) {
				/* A task started near the end of a tick must not permanently
				 * publish Unknown phases just because previous tasks used its
				 * facility allowance. Retry before publishing, with a fresh tick. */
				++this->diagnostic.rejected_work;
				this->dirty.insert(id); this->dirty_since.try_emplace(id, _state_ticks);
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

PassengerServiceDatabase::Publication PassengerServiceDatabase::GetPublication(OrderListID id) const
{
	if (this->dirty.contains(id)) return this->services.contains(id) ? Publication::TimingDirty : Publication::Dirty;
	if (this->services.contains(id)) return Publication::Published;
	return this->sweeping && id.base() >= this->cursor ? Publication::SweepPending : Publication::Unpublished;
}

bool PassengerServiceDatabase::IsCurrent(const PassengerJourneyExecution &ride) const
{
	const auto *service = this->Get(ride.service);
	return service != nullptr && service->incarnation == ride.signature && service->generation == ride.generation;
}

PassengerServiceDatabase::JourneyPolicy PassengerServiceDatabase::CompilePolicy(CargoType cargo, StationID origin,
		StationID destination, PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation) const
{
	using Reason = PassengerPeriodicPlan::Reason;
	JourneyPolicy policy;
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
	std::map<StationID, int64_t> distances;
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
		if (distances.at(station) != distance || policy.lower_bounds.contains(station)) continue;
		policy.lower_bounds.emplace(station, distance);
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
			int64_t duration = stops[ref.stop].arrival - stops[previous].departure;
			if (ref.stop == 0) duration += service.timetable.period;
			int64_t next = distance;
			if (AddTicks(next, duration) && !insert(stops[previous].station, next)) break;
		}
	}
	policy.complete = queue.empty() && !policy.limited && !policy.lower_bounds.contains(origin);
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
	policy.bytes = 256 + 128 * policy.lower_bounds.size();
	policy.scratch_bytes = memory;
	return policy;
}

PassengerPeriodicPlan PassengerServiceDatabase::Plan(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
		PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation)
{
	using Reason = PassengerPeriodicPlan::Reason;
	PassengerPeriodicPlan result;
	const auto started = std::chrono::steady_clock::now(); ++this->diagnostic.searches;
	auto observe = [&](Reason reason) { result.observations[static_cast<size_t>(reason)] = true; };
	auto finish = [&]() {
		observe(result.reason);
		++this->diagnostic.outcomes[static_cast<size_t>(result.reason)];
		for (size_t i = 0; i < result.observations.size(); ++i) this->diagnostic.observations[i] += result.observations[i];
		this->diagnostic.query_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
		return result;
	};
	if (cargo >= NUM_CARGO || origin == destination || !Station::IsValidID(origin) || !Station::IsValidID(destination) ||
			evaluation.minimum_transfer < 0 || evaluation.transfer_penalty < 0) return finish();
	if (evaluation.crowding_penalty != 0 || evaluation.capacity_shortfall_penalty != 0) {
		result.deferred = true; result.reason = Reason::Evaluation; return finish();
	}
	if (limits.work == 0 || limits.memory_bytes < 256) {
		result.deferred = true; result.reason = limits.work == 0 ? Reason::Work : Reason::Memory; return finish();
	}
	PolicyKey key{cargo, origin, destination, limits, evaluation};
	auto cached = this->journey_policies.find(key);
	JourneyPolicy temporary;
	const JourneyPolicy *policy;
	if (cached != this->journey_policies.end()) { cached->second.last_used = ++this->policy_clock; policy = &cached->second; ++this->diagnostic.policy_hits; }
	else {
		auto compilation = limits; compilation.work /= 2;
		temporary = this->CompilePolicy(cargo, origin, destination, compilation, evaluation); policy = &temporary; ++this->diagnostic.policy_misses;
	}
	result.work = policy->work; result.memory = policy->bytes;
	result.reason = policy->reason;
	/* Replaying the same reverse work keeps optional residency irrelevant to
	 * admission, including cold/warm/evicted caches and network joins. */
	if (policy->limited) observe(policy->reason);
	if (policy->unknown_phase) observe(Reason::UnknownPhase);
	if (policy->diagnostic_limit != Reason::NoCandidate) observe(policy->diagnostic_limit);
	if (!policy->limited && policy->reason != Reason::NoCandidate) {
		// No timed route in the published generation, independently of dirty work.
	} else {
		struct Label {
			StationID station;
			StateTicks time;
			int64_t cost;
			size_t parent, rides;
			PassengerJourneyExecution leg, first;
			bool active = true;
		};
		static_assert(sizeof(Label) <= 512);
		std::vector<Label> labels;
		std::map<StationID, std::vector<size_t>> frontiers;
		using Item = std::tuple<int64_t, int64_t, StateTicks, size_t, StateTicks, OrderListID, VehicleOrderID, VehicleOrderID, size_t>;
		std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
		int64_t omitted_bound = INT64_MAX;
		auto bound = [&](StationID station, int64_t cost) {
			auto it = policy->lower_bounds.find(station);
			int64_t distance = it != policy->lower_bounds.end() ? it->second : policy->unsettled_bound;
			if (!AddTicks(cost, distance)) return INT64_MAX;
			return cost;
		};
		auto limited = [&](Reason reason, int64_t lower) {
			observe(reason); omitted_bound = std::min(omitted_bound, lower);
			if (result.reason == Reason::NoCandidate || result.reason == Reason::Work || result.reason == Reason::Memory) result.reason = reason;
		};
		auto spend = [&]() {
			if (result.work >= limits.work) { limited(Reason::Work, 0); return false; }
			++result.work; return true;
		};
		auto first_key = [](const Label &a) { return std::tie(a.first.departure, a.first.service, a.first.boarding_order, a.first.alighting_order); };
		auto insert = [&](Label next) {
			int64_t lower = bound(next.station, next.cost);
			if (result.ride && lower > result.cost) return;
			auto found = frontiers.find(next.station);
			size_t retained = 0;
			if (found != frontiers.end() && next.station != destination) for (size_t index : found->second) {
				if (!spend()) return;
				const auto &other = labels[index];
				if (!other.active) continue;
				++retained;
				/* Earliest arrival dominates only when the extra wait has no
				 * preference cost. Weighted waits retain different arrival times.
				 * Continuations allow any service, so no path-history assumption
				 * or forbidden-station set makes this dominance unsound. */
				bool times = evaluation.waiting_weight == 0 ? other.time <= next.time : other.time == next.time;
				if (times && other.cost <= next.cost && other.rides <= next.rides &&
						(other.rides < next.rides || first_key(other) <= first_key(next) ||
						(evaluation.waiting_weight != 0 && other.cost < next.cost))) return;
			}
			if (next.station != destination && retained >= limits.labels_per_station) { limited(Reason::Labels, lower); return; }
			if (labels.size() >= limits.labels) { limited(Reason::Labels, lower); return; }
			constexpr size_t LABEL_BYTES = 1536; // vector overlap, queue, frontier map/vectors, reconstruction
			if (result.memory > limits.memory_bytes || LABEL_BYTES > limits.memory_bytes - result.memory) { limited(Reason::Memory, lower); return; }
			result.memory += LABEL_BYTES;
			queue.emplace(lower, next.cost, next.time, next.rides, next.first.departure, next.first.service,
				next.first.boarding_order, next.first.alighting_order, labels.size());
			frontiers[next.station].push_back(labels.size()); labels.push_back(std::move(next));
		};
		insert({origin, ready, 0, SIZE_MAX, 0, {}, {}});
		size_t winner = SIZE_MAX;
		const size_t candidates = std::clamp<size_t>(limits.candidates, 1, 16);
		while (!queue.empty()) {
			if (result.ride && std::get<0>(queue.top()) > result.cost) break;
			if (!spend()) break;
			size_t index = std::get<8>(queue.top()); queue.pop();
			Label current = labels[index];
			if (!current.active) continue;
			if (current.station == destination) {
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
				if (!spend()) break;
				const auto &service = this->services.at(ref.service);
				const auto &pattern = service.timetable;
				const auto &board = pattern.stops[ref.stop];
				if (!(board.board & (uint64_t{1} << cargo))) continue;
				int64_t earliest = current.time.base();
				if (current.rides && !AddTicks(earliest, evaluation.minimum_transfer)) { observe(Reason::Evaluation); continue; }
				std::optional<StateTicks> departure;
				for (const auto &vehicle : service.vehicles) {
					if (!spend()) break;
					if (!vehicle.capacity[cargo]) continue;
					if (!vehicle.phase_known) { observe(Reason::UnknownPhase); continue; }
					if (service.dispatch_period > 0) {
						int64_t offset = board.departure - pattern.stops[service.dispatch_stop].departure;
						if (offset < 0) offset += pattern.period;
						for (uint32_t slot : service.dispatch_slots) {
							if (!spend()) break;
							int64_t floor = service.dispatch_floor.base();
							if (!AddTicks(floor, offset)) continue;
							++this->diagnostic.next_departures;
							auto next = PassengerPeriodicTimetable::NextDeparture(service.dispatch_base, slot + offset, service.dispatch_period, StateTicks{std::max(earliest, floor)});
							if (next && (!departure || *next < *departure)) departure = next;
						}
					} else {
						++this->diagnostic.next_departures;
						StateTicks base = vehicle.base;
						const Vehicle *live = Vehicle::GetIfValid(vehicle.id);
						/* A published phase must not make an actually loading,
						 * overdue vehicle wait a whole circuit in a new query. This
						 * constant-size live observation changes no saved phase or
						 * cache: use the remaining published dwell at this occurrence.
						 * Future-ready queries still require the next real cycle. */
						if (live != nullptr && live->orders != nullptr && live->orders->index == service.id &&
								live->orders->GetCargoDistRouteGeneration() == service.incarnation &&
								!live->vehstatus.Any({VehState::Stopped, VehState::Crashed}) && !live->vehicle_flags.Test(VehicleFlag::ScheduledDispatch) &&
								live->current_order.IsAnyLoadingType() && !live->current_order.IsType(OT_LOADING_ADVANCE) &&
								live->cur_real_order_index == board.order && live->cur_implicit_order_index == board.order && live->last_station_visited == board.station &&
								(live->cur_timetable_order_index == INVALID_VEH_ORDER_ID || live->cur_timetable_order_index == board.order)) {
							int64_t departure_now = _state_ticks.base();
							int64_t remaining = std::max<int64_t>(0, board.departure - board.arrival - live->current_order_time);
							if (AddTicks(departure_now, remaining) && departure_now >= INT64_MIN + board.departure) base = StateTicks{departure_now - board.departure};
						}
						auto next = PassengerPeriodicTimetable::NextDeparture(base, board.departure, pattern.period, StateTicks{earliest});
						if (next && (!departure || *next < *departure)) departure = next;
					}
				}
				if (result.work >= limits.work) { limited(Reason::Work, 0); break; }
				if (!departure) { observe(Reason::Evaluation); continue; }
				if (current.rides && current.leg.service == service.id && current.leg.alighting_order == board.order &&
						!(board.forced & (uint64_t{1} << cargo))) {
					int64_t through = current.time.base();
					/* Staying aboard was already enumerated by the parent. Only a
					 * genuinely earlier shared-vehicle departure can improve it. */
					if (AddTicks(through, board.departure - board.arrival) && departure->base() >= through) continue;
				}
				for (size_t step = 1; step < pattern.stops.size(); ++step) {
					if (!spend()) break;
					size_t stop_index = (ref.stop + step) % pattern.stops.size();
					const auto &alight = pattern.stops[stop_index];
					if ((alight.alight & (uint64_t{1} << cargo)) && (!policy->complete || policy->lower_bounds.contains(alight.station))) {
						int64_t duration = alight.arrival - board.departure;
						if (stop_index <= ref.stop) duration += pattern.period;
						int64_t end = departure->base(), cost = current.cost;
						if (!AddTicks(end, duration) || end < current.time.base() || uint64_t(end) - uint64_t(current.time.base()) > INT64_MAX) { observe(Reason::Evaluation); continue; }
						int64_t elapsed = end - current.time.base(), wait = departure->base() - current.time.base();
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
		if (result.ride) {
			for (size_t index = winner; labels[index].parent != SIZE_MAX; index = labels[index].parent) result.route.push_back(labels[index].leg);
			std::reverse(result.route.begin(), result.route.end());
			result.deferred = omitted_bound <= result.cost;
			if (!result.deferred) result.reason = Reason::Found;
		} else {
			result.deferred = omitted_bound != INT64_MAX;
			if (!result.deferred) result.reason = result.observations[static_cast<size_t>(Reason::UnknownPhase)] ? Reason::UnknownPhase :
					result.observations[static_cast<size_t>(Reason::Evaluation)] ? Reason::Evaluation : Reason::NoCandidate;
		}
	}
	if (cached == this->journey_policies.end() && temporary.bytes <= POLICY_BYTES) {
		while (!this->journey_policies.empty() && (this->journey_policies.size() >= POLICY_ENTRIES || this->policy_bytes + temporary.bytes > POLICY_BYTES)) {
			auto oldest = std::min_element(this->journey_policies.begin(), this->journey_policies.end(), [](const auto &a, const auto &b) { return a.second.last_used < b.second.last_used; });
			this->policy_bytes -= oldest->second.bytes; this->journey_policies.erase(oldest);
		}
		temporary.last_used = ++this->policy_clock;
		this->policy_bytes += temporary.bytes; this->journey_policies.emplace(key, std::move(temporary));
	}
	result.memory = std::max(result.memory, policy->scratch_bytes);
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
				legacy.journeys.front().generalized_time.base() < horizon && graph.GetQueryStatus(origin, destination) < PassengerPredictionStatus::LimitReached;
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
			if (comparison.legacy_cost < horizon || legacy.GetOutcome() != PassengerJourneyOutcome::TimedRouteFound) break;
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

PassengerDatabaseDiagnostic PassengerServiceDatabase::GetDiagnostic() const
{
	auto d = this->diagnostic;
	d.services = this->services.size(); d.bytes = this->bytes; d.policy_bytes = this->policy_bytes;
	d.pending = this->dirty.size();
	if (this->sweeping) for (const OrderList *list : OrderList::Iterate()) if (list->index.base() >= this->cursor && !this->services.contains(list->index) && !this->dirty.contains(list->index)) ++d.pending;
	auto age = [&](StateTicks since) { return _state_ticks >= since ? uint64_t(_state_ticks.base()) - uint64_t(since.base()) : uint64_t{0}; };
	for (const auto &[id, since] : this->dirty_since) d.maximum_pending_ticks = std::max(d.maximum_pending_ticks, age(since));
	if (this->sweeping && d.pending > this->dirty.size()) d.maximum_pending_ticks = std::max(d.maximum_pending_ticks, age(this->sweeping_since));
	for (const auto &[id, service] : this->services) {
		d.vehicles += service.vehicles.size();
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
		d.dirty_services += this->dirty.contains(list->index);
		d.pending_passenger += this->dirty.contains(list->index) || (this->sweeping && list->index.base() >= this->cursor && published == nullptr);
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
			(ride.kind == PassengerRideKind::BoundService && ride.vehicle != vehicle->index) ||
			!Station::IsValidID(ride.alighting) || ride.alighting_order >= vehicle->GetNumOrders()) return false;
	if (boarding && (_state_ticks < ride.not_before || ride.boarding != vehicle->last_station_visited || ride.boarding_order != vehicle->cur_real_order_index ||
			vehicle->current_order.GetCargoLoadType(cargo) == OrderLoadType::NoLoad ||
			vehicle->current_order.GetCargoUnloadType(cargo) == OrderUnloadType::Unload ||
			vehicle->current_order.GetCargoUnloadType(cargo) == OrderUnloadType::Transfer)) return false;
	size_t budget = 65536;
	if (!HasPassengerVehicleCapacity(vehicle, cargo, budget)) return false;
	auto state = GetPassengerRouteState(vehicle, &budget);
	if (!state || !state->loading) return false;
	if (boarding && (!state->usable_stops[state->current_order] ||
			state->orders[state->current_order].GetCargoLoadType(cargo) == OrderLoadType::NoLoad)) return false;
	const size_t count = state->orders.size();
	for (size_t step = 1; step < count && budget > 0; ++step) {
		--budget;
		VehicleOrderID index = static_cast<VehicleOrderID>((state->current_order + step) % count);
		const Order &order = state->orders[index];
		if (order.IsType(OT_IMPLICIT) || order.IsType(OT_LABEL)) continue;
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
	put(1); put(this->generation); put(this->cursor); put(this->sweeping); put(this->dirty_cursor);
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
	if (get() != 1) return false;
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
		size_t bytes = ServiceBytes(service);
		if (restored.bytes + bytes > MAX_BYTES || !restored.services.emplace(service.id, std::move(service)).second) return false;
		restored.bytes += bytes;
	}
	if (!valid || cursor != data.size()) return false;
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
