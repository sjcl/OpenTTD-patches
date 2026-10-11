/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file passenger_service_graph.cpp Value-owned train services and predictions; no routing policy changes. */

#include "stdafx.h"
#include "passenger_service_graph.h"
#include "vehicle_base.h"

#include "safeguards.h"

static bool SpendGraphWork(size_t &budget, size_t work = 1)
{
	if (work > budget) { budget = 0; return false; }
	budget -= work;
	return true;
}

/** Limits dominate uncertainty, which dominates a complete result (including an empty one). */
static PassengerPredictionStatus MergeGraphStatus(PassengerPredictionStatus a, PassengerPredictionStatus b)
{
	return static_cast<PassengerPredictionStatus>(std::max(to_underlying(a), to_underlying(b)));
}

PassengerServiceGraph PassengerServiceGraph::Build(CargoType cargo, StateTicks now, PassengerServiceGraphLimits limits)
{
	PassengerServiceGraph graph;
	graph.cargo = cargo;
	graph.start = now;
	/* Clamp caller allowances as well as defaults. Avoid signed tick overflow near the end of time. */
	limits.work = std::min<size_t>(limits.work, 1 << 24);
	limits.services = std::min<size_t>(limits.services, 65536);
	limits.trips = std::min<size_t>(limits.trips, 65536);
	limits.total_visits = std::min<size_t>(limits.total_visits, 1 << 20);
	limits.visits_per_trip = std::min<size_t>(limits.visits_per_trip, 4096);
	limits.steps_per_trip = std::min<size_t>(limits.steps_per_trip, 65536);
	limits.horizon = std::clamp<Ticks>(limits.horizon, 0, 1 << 24);
	graph.end = now;
	if (cargo >= NUM_CARGO || now.base() < 0 || now.base() > INT64_MAX - (int64_t{1} << 50)) {
		graph.status = PassengerPredictionStatus::Unknown;
		return graph;
	}
	graph.end += limits.horizon;
	size_t budget = limits.work;
	size_t remaining_visits = limits.total_visits;
	for (const OrderList *list : OrderList::Iterate()) {
		if (!SpendGraphWork(budget)) { graph.status = PassengerPredictionStatus::BudgetExhausted; break; }
		const Vehicle *first = list->GetFirstSharedVehicle();
		if (first == nullptr || first->type != VehicleType::Train) continue;
		if (graph.services.size() >= limits.services) { graph.status = PassengerPredictionStatus::LimitReached; break; }
		/* Order owns its extra cargo/dispatch flags. Copy only timing fields from schedules:
		 * GUI strings are irrelevant and cannot consume unbounded copying work. */
		if (!SpendGraphWork(budget, 3 * static_cast<size_t>(list->GetNumOrders()))) { graph.status = PassengerPredictionStatus::BudgetExhausted; break; }
		std::vector<Order> orders(list->GetOrderVector());
		Service service;
		service.id = list->index;
		bool copied_schedules = true;
		for (const DispatchSchedule &source : list->GetScheduledDispatchScheduleSet()) {
			if (!SpendGraphWork(budget, 1 + source.GetScheduledDispatch().size())) { copied_schedules = false; break; }
			DispatchSchedule schedule;
			schedule.SetScheduledDispatchStartTick(source.GetScheduledDispatchStartTick());
			schedule.SetScheduledDispatchDuration(source.GetScheduledDispatchDuration());
			schedule.SetScheduledDispatchLastDispatch(source.GetScheduledDispatchLastDispatch());
			schedule.SetScheduledDispatchDelay(source.GetScheduledDispatchDelay());
			schedule.SetScheduledDispatchReuseSlots(source.GetScheduledDispatchReuseSlots());
			schedule.GetScheduledDispatchMutable() = source.GetScheduledDispatch();
			service.schedules.push_back(std::move(schedule));
		}
		if (!copied_schedules) { graph.status = PassengerPredictionStatus::BudgetExhausted; break; }
		for (VehicleOrderID i = 0; i < orders.size(); ++i) {
			const Order &order = orders[i];
			if (order.IsType(OT_GOTO_WAYPOINT) && order.GetWaypointFlags().Test(OrderWaypointFlag::Reverse)) service.reverse_waypoints.push_back(i);
			if (!order.IsType(OT_GOTO_STATION) || (order.GetNonStopType() & ONSF_NO_STOP_AT_DESTINATION_STATION)) continue;
			const OrderUnloadType unload = order.GetCargoUnloadType(cargo);
			service.stops.push_back({ order.GetDestination().ToStationID(), i, order.GetCargoLoadType(cargo) != OrderLoadType::NoLoad,
					unload != OrderUnloadType::NoUnload, unload == OrderUnloadType::Unload || unload == OrderUnloadType::Transfer,
					order.IsWaitTimetabled() ? std::optional(order.GetWaitTime()) : std::nullopt, order.GetDispatchScheduleIndex() });
		}
		/* Obtain nominal segments through the very same traversal as live trips. No LinkGraph
		 * station averages, distance estimates or second conditional/dispatch implementation. */
		if (!service.stops.empty()) {
			PassengerRouteState nominal{ orders, {}, service.stops.front().order, 0, 0, true, false, 1 };
			auto pattern = PredictPassengerVisits(nominal, cargo, StateTicks{0}, StateTicks{int64_t{1} << 50}, service.stops.size() + 1,
					limits.steps_per_trip, budget);
			service.pattern_status = pattern.status;
			for (size_t i = 1; i < pattern.visits.size(); ++i) {
				const auto &prev = pattern.visits[i - 1];
				const auto &visit = pattern.visits[i];
				service.segments.push_back({ prev.order, visit.order, visit.travel_from_previous });
				if (visit.order == service.stops.front().order) {
					service.cycle_known = true;
					service.pattern_status = PassengerPredictionStatus::Complete;
					break;
				}
			}
			if (pattern.status == PassengerPredictionStatus::BudgetExhausted) { graph.status = pattern.status; break; }
		}
		const size_t service_index = graph.services.size();
		graph.service_index.emplace(service.id, service_index);
		graph.services.push_back(std::move(service));
		Service &stored = graph.services.back();
		for (const Stop &stop : stored.stops) {
			auto &ids = graph.station_services[stop.station];
			if (ids.empty() || ids.back() != stored.id) ids.push_back(stored.id);
		}
		/* Pool order, shared-chain order, then prediction order: stable on all multiplayer peers. */
		for (const Vehicle *v = first; v != nullptr; v = v->NextShared()) {
			if (!SpendGraphWork(budget)) { graph.status = PassengerPredictionStatus::BudgetExhausted; break; }
			if (!v->IsPrimaryVehicle() || v->type != VehicleType::Train) continue;
			if (graph.trips.size() >= limits.trips) { graph.status = PassengerPredictionStatus::LimitReached; break; }
			uint64_t capacity = 0, occupants = 0;
			for (const Vehicle *u = v; u != nullptr; u = u->Next()) {
				if (!SpendGraphWork(budget)) break;
				if (u->cargo_type == cargo) {
					capacity += u->cargo_cap;
					occupants += u->cargo.TotalCount();
				}
			}
			if (budget == 0) { graph.status = PassengerPredictionStatus::BudgetExhausted; break; }
			Operation operation = v->vehstatus.Test(VehState::Crashed) ? Operation::Crashed :
					v->vehstatus.Test(VehState::Stopped) ? Operation::Stopped : Operation::Running;
			Trip trip{ v->index, stored.id, v->tile, v->GetMovingDirection(), v->cur_real_order_index, v->cur_implicit_order_index,
					v->cur_timetable_order_index, v->current_order_time, v->lateness_counter, v->current_order.IsAnyLoadingType(),
					v->vehicle_flags.Test(VehicleFlag::ScheduledDispatch), capacity, operation, {} };
			trip.occupants = occupants;
			trip.current_free = capacity - std::min(capacity, occupants);
			trip.prediction.start = trip.prediction.complete_until = now;
			if (operation == Operation::Running) {
				auto state = GetPassengerRouteState(v);
				if (state) {
					state->orders = orders;
					state->schedules = stored.schedules;
					trip.prediction = PredictPassengerVisits(*state, cargo, now, graph.end,
							std::min(remaining_visits, limits.visits_per_trip), limits.steps_per_trip, budget);
				} else {
					trip.operation = Operation::Unknown;
				}
			} else {
				trip.prediction.status = PassengerPredictionStatus::Complete;
				trip.prediction.complete_until = graph.end;
			}
			remaining_visits -= trip.prediction.visits.size();
			const size_t trip_index = graph.trips.size();
			stored.trips.push_back(trip_index);
			graph.trips.push_back(std::move(trip));
			const Trip &saved = graph.trips.back();
			for (size_t i = 0; i < saved.prediction.visits.size(); ++i) {
				graph.station_visits[graph.GetStop(saved, saved.prediction.visits[i].order).station].push_back({ trip_index, i });
			}
			if (saved.prediction.status == PassengerPredictionStatus::BudgetExhausted) { graph.status = saved.prediction.status; break; }
		}
		if (graph.status != PassengerPredictionStatus::Complete) break;
	}
	graph.BuildDepartureIndex();
	return graph;
}

/** Build once, without changing capture-order VisitIDs or legacy query ordering. */
void PassengerServiceGraph::BuildDepartureIndex()
{
	for (const auto &[station, services] : this->station_services) {
		auto &index = this->station_departures[station];
		for (OrderListID service : services) {
			for (size_t id : this->GetService(service)->trips) {
				const auto &prediction = this->trips[id].prediction;
				if (!prediction.Covers(this->start, this->end)) index.status = MergeGraphStatus(index.status, prediction.status);
			}
		}
	}
	for (const auto &[station, visits] : this->station_visits) {
		auto &index = this->station_departures[station];
		for (VisitID id : visits) {
			const Trip &trip = this->trips[id.trip];
			if (trip.operation != Operation::Running || !this->GetVisitStop(id).can_board || trip.capacity == 0) continue;
			(trip.prediction.visits[id.visit].departure ? index.timed : index.unknown).push_back(id);
		}
		std::sort(index.timed.begin(), index.timed.end(), [&](VisitID a, VisitID b) {
			const Trip &ta = this->trips[a.trip], &tb = this->trips[b.trip];
			return std::tuple(*ta.prediction.visits[a.visit].departure, ta.vehicle, a.trip, a.visit) <
					std::tuple(*tb.prediction.visits[b.visit].departure, tb.vehicle, b.trip, b.visit);
		});
	}
}

PassengerServiceGraph::Departures PassengerServiceGraph::GetDepartures(StationID station, StateTicks from, StateTicks to) const
{
	Departures result{ {}, {}, this->status };
	if (from < this->start || to > this->end || from >= to) {
		result.status = MergeGraphStatus(result.status, PassengerPredictionStatus::Unknown);
		return result;
	}
	auto it = this->station_departures.find(station);
	if (it == this->station_departures.end()) return result;
	const auto &index = it->second;
	result.status = MergeGraphStatus(result.status, index.status);
	result.unknown = index.unknown;
	if (!index.unknown.empty()) result.status = MergeGraphStatus(result.status, PassengerPredictionStatus::Unknown);
	auto lower = [&](StateTicks time) {
		return std::lower_bound(index.timed.begin(), index.timed.end(), time, [&](VisitID id, StateTicks tick) {
			return *this->trips[id.trip].prediction.visits[id.visit].departure < tick;
		});
	};
	result.timed = std::span<const VisitID>(lower(from), lower(to));
	return result;
}

const PassengerServiceGraph::Service *PassengerServiceGraph::GetService(OrderListID id) const
{
	auto it = this->service_index.find(id);
	return it == this->service_index.end() ? nullptr : &this->services[it->second];
}

std::span<const OrderListID> PassengerServiceGraph::GetServicesAtStation(StationID station) const
{
	auto it = this->station_services.find(station);
	return it == this->station_services.end() ? std::span<const OrderListID>{} : it->second;
}

const PassengerServiceGraph::Stop &PassengerServiceGraph::GetStop(const Trip &trip, VehicleOrderID order) const
{
	const auto &stops = this->GetService(trip.service)->stops;
	auto it = std::lower_bound(stops.begin(), stops.end(), order, [](const Stop &stop, VehicleOrderID id) { return stop.order < id; });
	assert(it != stops.end() && it->order == order);
	return *it;
}

PassengerServiceGraph::QueryResult<PassengerServiceGraph::VisitID> PassengerServiceGraph::GetVisits(
		StationID station, StateTicks from, StateTicks to, size_t budget) const
{
	budget = std::min<size_t>(budget, 1 << 24);
	return this->SelectVisits(station, from, to, budget);
}

PassengerServiceGraph::QueryResult<PassengerServiceGraph::VisitID> PassengerServiceGraph::SelectVisits(
		StationID station, StateTicks from, StateTicks to, size_t &budget) const
{
	QueryResult<VisitID> result;
	result.status = this->status;
	if (from < this->start || to > this->end || from >= to) {
		result.status = MergeGraphStatus(result.status, PassengerPredictionStatus::Unknown);
		return result;
	}
	/* Even a trip whose dynamic branch prevented reaching this station makes absence uncertain. */
	for (OrderListID id : this->GetServicesAtStation(station)) {
		if (!SpendGraphWork(budget)) { result.status = PassengerPredictionStatus::BudgetExhausted; return result; }
		for (size_t index : this->GetService(id)->trips) {
			if (!SpendGraphWork(budget)) { result.status = PassengerPredictionStatus::BudgetExhausted; return result; }
			const auto &prediction = this->trips[index].prediction;
			if (!prediction.Covers(from, to)) result.status = MergeGraphStatus(result.status, prediction.status);
		}
	}
	auto it = this->station_visits.find(station);
	if (it == this->station_visits.end()) return result;
	for (VisitID id : it->second) {
		if (!SpendGraphWork(budget)) { result.status = PassengerPredictionStatus::BudgetExhausted; return result; }
		const auto &prediction = this->trips[id.trip].prediction;
		const auto &visit = prediction.visits[id.visit];
		const bool covered = prediction.Covers(from, to);
		/* A currently loading arrival happened before the snapshot. Its absent historical
		 * timestamp does not create a possible future arrival in every query window. */
		const bool unknown_arrival = !covered && !visit.arrival && !visit.loading_now;
		const bool unknown_departure = !covered && !visit.departure;
		if (unknown_arrival || unknown_departure || (visit.arrival && *visit.arrival >= from && *visit.arrival < to) ||
				(visit.departure && *visit.departure >= from && *visit.departure < to)) {
			result.items.push_back(id);
			if (unknown_arrival || unknown_departure) result.status = MergeGraphStatus(result.status, PassengerPredictionStatus::Unknown);
		}
	}
	return result;
}

PassengerServiceGraph::QueryResult<PassengerServiceGraph::BoardableTrip> PassengerServiceGraph::GetBoardableTrips(
		StationID station, StateTicks from, StateTicks to, size_t budget) const
{
	budget = std::min<size_t>(budget, 1 << 24);
	QueryResult<BoardableTrip> result;
	auto visits = this->SelectVisits(station, from, to, budget);
	result.status = visits.status;
	for (VisitID id : visits.items) {
		if (!SpendGraphWork(budget)) { result.status = PassengerPredictionStatus::BudgetExhausted; break; }
		const Trip &trip = this->trips[id.trip];
		const auto &visit = trip.prediction.visits[id.visit];
		if (trip.capacity == 0 || !this->GetStop(trip, visit.order).can_board) continue;
		if (visit.departure && (*visit.departure < from || *visit.departure >= to)) continue;
		BoardableTrip boarding{ id, {}, trip.prediction.status };
		if (!visit.departure) boarding.status = MergeGraphStatus(boarding.status, PassengerPredictionStatus::Unknown);
		for (size_t i = id.visit + 1; i < trip.prediction.visits.size(); ++i) {
			if (!SpendGraphWork(budget)) { boarding.status = PassengerPredictionStatus::BudgetExhausted; break; }
			const auto &next = trip.prediction.visits[i];
			const Stop &stop = this->GetStop(trip, next.order);
			if (stop.can_alight) boarding.reachable.push_back({ id.trip, i });
			if (!next.arrival) boarding.status = MergeGraphStatus(boarding.status, PassengerPredictionStatus::Unknown);
			if (stop.forced_transfer) break;
		}
		/* A truncated future suffix does not invalidate complete boarding-window coverage.
		 * Its status stays on the candidate, but unfinished query work affects the whole result. */
		if (boarding.status == PassengerPredictionStatus::BudgetExhausted) result.status = boarding.status;
		result.items.push_back(std::move(boarding));
	}
	return result;
}
