/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file passenger_service_graph.cpp Value-owned multimodal passenger services and predictions. */

#include "stdafx.h"
#include "passenger_service_graph.h"
#include "vehicle_base.h"
#include <chrono>

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

PassengerServiceGraph PassengerServiceGraph::Build(CargoType cargo, StateTicks now, PassengerServiceGraphLimits limits,
		std::optional<PassengerServiceScope> scope)
{
	const auto started = std::chrono::steady_clock::now();
	PassengerServiceGraph graph;
	graph.cargo = cargo; graph.start = graph.end = now; graph.scope = scope;
	limits.work = std::min<size_t>(limits.work, 1 << 24);
	limits.services = std::min<size_t>(limits.services, 65536);
	limits.trips = std::min<size_t>(limits.trips, 65536);
	limits.total_visits = std::min<size_t>(limits.total_visits, 1 << 20);
	limits.visits_per_trip = std::min<size_t>(limits.visits_per_trip, 4096);
	limits.steps_per_trip = std::min<size_t>(limits.steps_per_trip, 65536);
	limits.horizon = std::clamp<Ticks>(limits.horizon, 0, 1 << 24);
	if (cargo >= NUM_CARGO || !IsCargoInClass(cargo, CargoClass::Passengers) || now.base() < 0 || now.base() > INT64_MAX - (int64_t{1} << 50)) {
		graph.status = PassengerPredictionStatus::Unknown;
		return graph;
	}
	graph.end += limits.horizon;
	size_t budget = limits.work, remaining_visits = limits.total_visits;
	size_t resident_bound = 0;
	static_assert(3 * sizeof(PassengerPredictedVisit) + 6 * sizeof(VisitID) <= 320);
	static_assert(3 * sizeof(Trip) + 3 * sizeof(size_t) + 128 <= 1024);
	size_t predicting_visits = 0;
	PassengerGraphStage stage = PassengerGraphStage::Discovery;
	auto fail = [&](PassengerGraphFailure reason, PassengerPredictionStatus status) {
		graph.status = MergeGraphStatus(graph.status, status);
		if (graph.diagnostic.first_failure.reason == PassengerGraphFailure::None) {
			graph.diagnostic.first_failure = {reason, stage, limits.work - budget, resident_bound, graph.trips.size(), graph.diagnostic.visits + predicting_visits};
		}
	};
	/* Logical bytes reserve owned values, geometric vector growth, station/map indices,
	 * component/uncertainty scratch and predictor scratch BEFORE allocation. Traversal,
	 * including repeated circuits/dispatch searches, consumes work but not stored bytes.
	 * These constants are portable envelopes, never allocator capacity observations. */
	auto admit = [&](size_t bytes) {
		if (bytes > limits.memory_bytes - resident_bound) {
			fail(PassengerGraphFailure::Memory, PassengerPredictionStatus::LimitReached); return false;
		}
		resident_bound += bytes;
		graph.memory_bound = std::max(graph.memory_bound, resident_bound);
		return true;
	};
	struct Candidate { const Vehicle *vehicle; uint64_t capacity; uint64_t occupants; };
	struct FleetService { const OrderList *list; bool implicit; bool unbounded; std::vector<Candidate> candidates; };
	std::vector<FleetService> fleets;
	std::vector<OrderListID> eligible;
	bool opaque_scope = false;
	/* Read cargo/refit eligibility before building topology: freight must not join
	 * otherwise unrelated passenger corridors. These pointers live only in Build. */
	for (const OrderList *list : OrderList::Iterate()) {
		if (!SpendGraphWork(budget)) { fail(PassengerGraphFailure::Work, PassengerPredictionStatus::BudgetExhausted); break; }
		const Vehicle *first = list->GetFirstSharedVehicle();
		if (!IsPassengerServiceVehicle(first)) continue;
		/* Unscoped snapshots expose actual capacity only. Exclude freight before
		 * reading orders or dispatch slots; scoped routing must also retain future
		 * refits as uncertain alternatives, so it checks the declared pattern below. */
		if (!scope) {
			bool passenger_service = false;
			size_t excluded = 0;
			for (const Vehicle *v = first; v != nullptr; v = v->NextShared()) {
				if (!SpendGraphWork(budget)) break;
				if (!IsPassengerServiceVehicle(v)) continue;
				if (HasPassengerVehicleCapacity(v, cargo, budget)) { passenger_service = true; break; }
				++excluded;
				if (budget == 0) break;
			}
			if (budget == 0) { fail(PassengerGraphFailure::Work, PassengerPredictionStatus::BudgetExhausted); break; }
			if (!passenger_service) {
				graph.diagnostic.trains += excluded;
				graph.diagnostic.excluded_cargo += excluded;
				continue;
			}
		}
		bool may_refit = false, implicit = false, unbounded = false, manual_stops = false;
		if (!SpendGraphWork(budget, list->GetNumOrders())) { fail(PassengerGraphFailure::Work, PassengerPredictionStatus::BudgetExhausted); break; }
		for (const Order &order : list->GetOrderVector()) {
			may_refit |= order.IsRefit();
			manual_stops |= order.IsType(OT_GOTO_STATION);
			implicit |= order.IsType(OT_IMPLICIT);
			unbounded |= order.IsType(OT_IMPLICIT) && order.GetDestination().ToStationID() == StationID::Invalid();
		}
		/* Implicit-only wandering has no declared route to bound future stops. */
		unbounded |= implicit && !manual_stops;
		std::vector<Candidate> candidates;
		for (const Vehicle *v = first; v != nullptr; v = v->NextShared()) {
			if (!SpendGraphWork(budget)) { fail(PassengerGraphFailure::Work, PassengerPredictionStatus::BudgetExhausted); break; }
			if (!IsPassengerServiceVehicle(v)) continue;
			++graph.diagnostic.trains;
			uint64_t capacity = 0, occupants = 0;
			bool captured = true;
			for (const Vehicle *u = v; u != nullptr; u = u->Next()) {
				if (!SpendGraphWork(budget)) { captured = false; break; }
				if (u->cargo_type == cargo) { capacity += u->cargo_cap; occupants += u->cargo.TotalCount(); }
			}
			if (!captured) { fail(PassengerGraphFailure::Work, PassengerPredictionStatus::BudgetExhausted); ++graph.diagnostic.omitted_trains; break; }
			if (capacity != 0) ++graph.diagnostic.cargo_capable;
			else if (scope && (may_refit || v->current_order.IsRefit())) ++graph.diagnostic.possible_refit;
			else { ++graph.diagnostic.excluded_cargo; continue; }
			if (scope && v->vehstatus.Any({VehState::Stopped, VehState::Crashed})) { ++graph.diagnostic.inactive; continue; }
			if (!admit(128)) break; // Temporary candidate vector, including geometric growth.
			candidates.push_back({v, capacity, occupants});
		}
		if (graph.status != PassengerPredictionStatus::Complete) break;
		if (candidates.empty()) continue;
		if (!admit(256)) break; // Fleet discovery metadata, vector growth and eligible IDs.
		eligible.push_back(list->index);
		fleets.push_back({list, implicit, unbounded, std::move(candidates)});
	}
	PassengerServiceSelection selection;
	graph.diagnostic.eligible_services = eligible.size();
	if (scope && graph.status == PassengerPredictionStatus::Complete) {
		stage = PassengerGraphStage::Structure;
		selection = SelectPassengerServices(*scope, eligible, budget, limits.memory_bytes - resident_bound);
		graph.status = selection.status;
		if (selection.status != PassengerPredictionStatus::Complete) fail(selection.status == PassengerPredictionStatus::BudgetExhausted ?
				PassengerGraphFailure::Work : PassengerGraphFailure::Index, selection.status);
		graph.diagnostic.static_cache_hit = selection.cache_hit;
		graph.diagnostic.static_allocation_bytes = selection.allocation_bytes;
		graph.diagnostic.reused_work = selection.reused_work;
		graph.diagnostic.largest_block_services = selection.largest_block_services;
		if (selection.status == PassengerPredictionStatus::Complete) {
			/* Discovery coexists with index scratch; scratch is gone before dynamic
			 * capture. Charge the same peak warm, independently of cache residency. */
			graph.memory_bound = std::max(graph.memory_bound, resident_bound + selection.memory_bound);
			admit(selection.resident_bound);
		}
	}
	if (graph.status == PassengerPredictionStatus::Complete) for (const FleetService &fleet : fleets) {
		if (!scope || fleet.unbounded || selection.services.contains(fleet.list->index)) {
			++graph.diagnostic.selected_services;
			graph.diagnostic.selected_trips += fleet.candidates.size();
		}
	}
	if (graph.status == PassengerPredictionStatus::Complete) for (FleetService &fleet : fleets) {
		stage = PassengerGraphStage::Orders;
		const OrderList *list = fleet.list;
		auto &candidates = fleet.candidates;
		if (scope && !fleet.unbounded && !selection.services.contains(list->index)) {
			graph.scoped_omissions = true;
			graph.diagnostic.excluded_scope += candidates.size();
			continue;
		}
		/* A bounded, disconnected implicit service was pruned above. Unknown stops
		 * and implicit-only wandering cannot establish that separation. */
		opaque_scope |= scope.has_value() && fleet.unbounded;
		if (graph.services.size() >= limits.services) { fail(PassengerGraphFailure::Services, PassengerPredictionStatus::LimitReached); graph.diagnostic.omitted_trains += candidates.size(); break; }
		if (!SpendGraphWork(budget, 3 * static_cast<size_t>(list->GetNumOrders()))) { fail(PassengerGraphFailure::Work, PassengerPredictionStatus::BudgetExhausted); graph.diagnostic.omitted_trains += candidates.size(); break; }
		if (!admit(512 + 1024 * static_cast<size_t>(list->GetNumOrders()))) break;
		std::vector<Order> orders(list->GetOrderVector());
		Service service;
		service.id = list->index;
		bool copied_schedules = true;
		stage = PassengerGraphStage::Schedules;
		for (const DispatchSchedule &source : list->GetScheduledDispatchScheduleSet()) {
			if (!SpendGraphWork(budget, 1 + source.GetScheduledDispatch().size())) { fail(PassengerGraphFailure::Work, PassengerPredictionStatus::BudgetExhausted); copied_schedules = false; break; }
			if (!admit(512 + 64 * source.GetScheduledDispatch().size())) { copied_schedules = false; break; }
			DispatchSchedule schedule;
			schedule.SetScheduledDispatchStartTick(source.GetScheduledDispatchStartTick());
			schedule.SetScheduledDispatchDuration(source.GetScheduledDispatchDuration());
			schedule.SetScheduledDispatchLastDispatch(source.GetScheduledDispatchLastDispatch());
			schedule.SetScheduledDispatchDelay(source.GetScheduledDispatchDelay());
			schedule.SetScheduledDispatchReuseSlots(source.GetScheduledDispatchReuseSlots());
			schedule.GetScheduledDispatchMutable() = source.GetScheduledDispatch();
			service.schedules.push_back(std::move(schedule));
		}
		if (!copied_schedules) break;
		for (VehicleOrderID i = 0; i < orders.size(); ++i) {
			const Order &order = orders[i];
			if (order.IsType(OT_IMPLICIT)) {
				if (order.GetDestination().ToStationID() != StationID::Invalid()) service.stops.push_back({order.GetDestination().ToStationID(), i, true, true, false, std::nullopt, -1});
				continue;
			}
			if (order.IsType(OT_GOTO_WAYPOINT) && order.GetWaypointFlags().Test(OrderWaypointFlag::Reverse)) service.reverse_waypoints.push_back(i);
			if (!order.IsType(OT_GOTO_STATION) || (order.GetNonStopType() & ONSF_NO_STOP_AT_DESTINATION_STATION)) continue;
			const OrderUnloadType unload = order.GetCargoUnloadType(cargo);
			service.stops.push_back({ order.GetDestination().ToStationID(), i, order.GetCargoLoadType(cargo) != OrderLoadType::NoLoad,
					unload != OrderUnloadType::NoUnload, unload == OrderUnloadType::Unload || unload == OrderUnloadType::Transfer,
					order.IsWaitTimetabled() ? std::optional(order.GetWaitTime()) : std::nullopt, order.GetDispatchScheduleIndex() });
		}
		if (!service.stops.empty()) {
			stage = PassengerGraphStage::Pattern;
			PassengerRouteState nominal{ orders, {}, service.stops.front().order, 0, 0, true, false, 1 };
			auto pattern = PredictPassengerVisits(nominal, cargo, StateTicks{0}, StateTicks{int64_t{1} << 50}, service.stops.size() + 1,
					limits.steps_per_trip, budget);
			service.pattern_status = pattern.status;
			for (size_t i = 1; i < pattern.visits.size(); ++i) {
				const auto &prev = pattern.visits[i - 1], &visit = pattern.visits[i];
				service.segments.push_back({ prev.order, visit.order, visit.travel_from_previous });
				if (visit.order == service.stops.front().order) { service.cycle_known = true; service.pattern_status = PassengerPredictionStatus::Complete; break; }
			}
			if (pattern.status == PassengerPredictionStatus::BudgetExhausted) { fail(PassengerGraphFailure::Work, pattern.status); break; }
		}
		graph.service_index.emplace(service.id, graph.services.size());
		graph.services.push_back(std::move(service));
		Service &stored = graph.services.back();
		for (const Stop &stop : stored.stops) {
			auto &ids = graph.station_services[stop.station];
			if (ids.empty() || ids.back() != stored.id) ids.push_back(stored.id);
		}
		/* Legacy snapshots keep capture order. Scoped snapshots rotate each fleet's
		 * starting vehicle deterministically; a tight allowance cannot always favour its head. */
		if (scope && !candidates.empty()) {
			const size_t offset = (static_cast<uint64_t>(now.base() / 64) + scope->origin.base() + scope->destination.base()) % candidates.size();
			std::rotate(candidates.begin(), candidates.begin() + offset, candidates.end());
		}
		for (size_t c = 0; c < candidates.size(); ++c) {
			stage = PassengerGraphStage::Trips;
			const auto &[v, capacity, occupants] = candidates[c];
			if (graph.trips.size() >= limits.trips) { fail(PassengerGraphFailure::Trips, PassengerPredictionStatus::LimitReached); graph.diagnostic.omitted_trains += candidates.size() - c; break; }
			const size_t visit_limit = std::min(remaining_visits, limits.visits_per_trip);
			Operation operation = v->vehstatus.Test(VehState::Crashed) ? Operation::Crashed : v->vehstatus.Test(VehState::Stopped) ? Operation::Stopped : Operation::Running;
			if (!admit(1024)) { graph.diagnostic.omitted_trains += candidates.size() - c; break; }
			Trip trip{ v->index, stored.id, v->tile, v->GetMovingDirection(), v->cur_real_order_index, v->cur_implicit_order_index,
					v->cur_timetable_order_index, v->current_order_time, v->lateness_counter, v->current_order.IsAnyLoadingType(),
					v->vehicle_flags.Test(VehicleFlag::ScheduledDispatch), capacity, operation, {} };
			trip.occupants = occupants; trip.current_free = capacity - std::min(capacity, occupants);
			trip.type = v->type;
			trip.prediction.start = trip.prediction.complete_until = now;
			trip.prediction.stopped_at = now;
			if (operation == Operation::Running && capacity == 0 && v->current_order.IsRefit()) {
				/* A current refit outside the declared pattern can create seats before
				 * departure. Capture uncertainty instead of certifying zero future capacity. */
				trip.prediction.stop = PassengerPredictionStop::Refit;
			} else if (operation == Operation::Running) {
				stage = PassengerGraphStage::Prediction;
				auto state = GetPassengerRouteState(v, &budget, &trip.prediction_reason);
				if (state) {
					state->orders = orders; state->schedules = stored.schedules;
					++graph.diagnostic.predicted;
					trip.prediction = PredictPassengerVisits(*state, cargo, now, graph.end, visit_limit, limits.steps_per_trip, budget, [&]() {
						if (!admit(320)) return false;
						++graph.diagnostic.admitted_visits;
						++predicting_visits;
						return true;
					});
					if (trip.prediction.status == PassengerPredictionStatus::LimitReached && trip.prediction.visits.size() == visit_limit && visit_limit < limits.visits_per_trip) {
						fail(PassengerGraphFailure::TotalVisits, PassengerPredictionStatus::LimitReached);
					}
					if (trip.prediction.status != PassengerPredictionStatus::Complete && trip.prediction_reason == PassengerPredictionReason::None) {
						trip.prediction_reason = PassengerPredictionReason::OrderOrTiming;
					}
				} else {
					trip.operation = Operation::Unknown;
					trip.prediction.stop = trip.prediction_reason == PassengerPredictionReason::OrderProgress ? PassengerPredictionStop::OrderProgress :
							trip.prediction_reason == PassengerPredictionReason::StationFacilities ? PassengerPredictionStop::Facilities : PassengerPredictionStop::VehicleState;
					if (budget == 0) { trip.prediction.status = PassengerPredictionStatus::BudgetExhausted; trip.prediction.stop = PassengerPredictionStop::Work; }
				}
			} else {
				++graph.diagnostic.inactive;
				trip.prediction.status = PassengerPredictionStatus::Complete; trip.prediction.complete_until = graph.end;
			}
			remaining_visits -= trip.prediction.visits.size();
			graph.diagnostic.visits += trip.prediction.visits.size();
			predicting_visits = 0;
			if (trip.prediction.status >= PassengerPredictionStatus::LimitReached) ++graph.diagnostic.truncated_predictions;
			if (trip.prediction.status != PassengerPredictionStatus::Complete) {
				if (graph.diagnostic.first_prediction_failure.reason == PassengerPredictionStop::None) {
					graph.diagnostic.first_prediction_failure = {trip.prediction.stop, trip.vehicle, limits.work - budget, resident_bound,
							graph.trips.size() + 1, graph.diagnostic.visits, trip.prediction.stopped_at, trip.prediction.complete_until, graph.end};
				}
				++graph.diagnostic.prediction_stops[static_cast<size_t>(trip.prediction.stop)];
				graph.diagnostic.maximum_unpredicted_ticks = std::max<Ticks>(graph.diagnostic.maximum_unpredicted_ticks, (graph.end - trip.prediction.complete_until).base());
			}
			const size_t trip_index = graph.trips.size();
			stored.trips.push_back(trip_index); graph.trips.push_back(std::move(trip));
			const Trip &saved = graph.trips.back();
			/* A known implicit order outside the proven horizon (or an unreachable
			 * unconditional branch) does not invalidate already complete coverage. */
			opaque_scope |= scope.has_value() && fleet.implicit && !saved.prediction.Covers(graph.start, graph.end);
			for (size_t i = 0; i < saved.prediction.visits.size(); ++i) graph.station_visits[graph.GetStop(saved, saved.prediction.visits[i].order).station].push_back({trip_index, i});
			if (saved.prediction.status == PassengerPredictionStatus::BudgetExhausted) { fail(PassengerGraphFailure::Work, saved.prediction.status); graph.diagnostic.omitted_trains += candidates.size() - c - 1; break; }
			/* This scoped capture already cannot certify any query. Do not predict the
			 * rest of the fleet/network merely to rediscover that fact. No failure is
			 * retained across events: the next build reads orders and progress afresh. */
			if (opaque_scope) {
				graph.diagnostic.opaque_service = stored.id;
				graph.diagnostic.opaque_vehicle = saved.vehicle;
				fail(PassengerGraphFailure::Opaque, PassengerPredictionStatus::Unknown);
				graph.diagnostic.omitted_trains += candidates.size() - c - 1;
				break;
			}
		}
		if (graph.status != PassengerPredictionStatus::Complete) break;
	}
	graph.work = limits.work - budget;
	if (opaque_scope) fail(PassengerGraphFailure::Opaque, PassengerPredictionStatus::Unknown);
	stage = PassengerGraphStage::Indices;
	/* Failed capture cannot certify any query. Avoid sorting/indexing its prefix;
	 * declared stops and capture-order visits remain available for diagnostics. */
	if (!opaque_scope && graph.status < PassengerPredictionStatus::LimitReached) graph.BuildDepartureIndex();
	/* Allocation observations and elapsed time are diagnostics, excluded from all decisions. */
	graph.diagnostic.allocation_bytes = sizeof(graph) + graph.services.capacity() * sizeof(Service) + graph.trips.capacity() * sizeof(Trip);
	for (const auto &service : graph.services) {
		graph.diagnostic.allocation_bytes += service.stops.capacity() * sizeof(Stop) + service.segments.capacity() * sizeof(Segment) +
				service.reverse_waypoints.capacity() * sizeof(VehicleOrderID) + service.schedules.capacity() * sizeof(DispatchSchedule) + service.trips.capacity() * sizeof(size_t);
		for (const auto &schedule : service.schedules) graph.diagnostic.allocation_bytes += schedule.GetScheduledDispatch().capacity() * sizeof(DispatchSlot);
	}
	for (const auto &trip : graph.trips) graph.diagnostic.allocation_bytes += trip.prediction.visits.capacity() * sizeof(PassengerPredictedVisit);
	auto map_bytes = [](const auto &map) { return map.size() * (sizeof(typename std::decay_t<decltype(map)>::value_type) + 4 * sizeof(void *)); };
	graph.diagnostic.allocation_bytes += map_bytes(graph.service_index) + map_bytes(graph.loading_visits) + map_bytes(graph.component_roots) + map_bytes(graph.uncertain_components);
	for (const auto &[station, ids] : graph.station_services) graph.diagnostic.allocation_bytes += 96 + ids.capacity() * sizeof(OrderListID);
	for (const auto &[station, ids] : graph.station_visits) graph.diagnostic.allocation_bytes += 96 + ids.capacity() * sizeof(VisitID);
	for (const auto &[station, departures] : graph.station_departures) graph.diagnostic.allocation_bytes += 128 + (departures.timed.capacity() + departures.unknown.capacity()) * sizeof(VisitID);
	for (const auto &[root, component] : graph.uncertain_components) graph.diagnostic.allocation_bytes += component.stations.capacity() * sizeof(StationID);
	graph.diagnostic.allocation_bytes += graph.uncertain_stations.capacity() * sizeof(StationID);
	graph.diagnostic.build_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
	return graph;
}

PassengerPredictionStatus PassengerServiceGraph::GetStatus() const
{
	return this->scoped_omissions ? MergeGraphStatus(this->status, PassengerPredictionStatus::Unknown) : this->status;
}

PassengerPredictionStatus PassengerServiceGraph::GetQueryStatus(StationID origin, StationID destination, bool weighted_waiting) const
{
	if (origin == StationID::Invalid() || destination == StationID::Invalid()) return MergeGraphStatus(this->status, PassengerPredictionStatus::Unknown);
	if (this->scope && (this->scope->origin != origin || this->scope->destination != destination)) return MergeGraphStatus(this->status, PassengerPredictionStatus::Unknown);
	/* Weighted waiting can prefer riding an otherwise useless positive-time excursion.
	 * Active mode has zero waiting weight. Other evaluators need an unscoped graph to
	 * certify that these excluded excursions cannot improve their preference score. */
	if (weighted_waiting && this->scoped_omissions) return MergeGraphStatus(this->status, PassengerPredictionStatus::Unknown);
	return this->status;
}

PassengerServiceGraph::Coverage PassengerServiceGraph::GetCoverage(StationID origin, StationID destination) const
{
	if (this->GetQueryStatus(origin, destination) != PassengerPredictionStatus::Complete || this->GetUncertainStatus(destination) != PassengerPredictionStatus::Complete) return Coverage::Incomplete;
	return this->GetStatus() == PassengerPredictionStatus::Complete ? Coverage::GlobalComplete : Coverage::ScopedComplete;
}

std::optional<PassengerServiceGraph::VisitID> PassengerServiceGraph::FindOnboardVisit(VehicleID vehicle, StationID station, VehicleOrderID order) const
{
	auto found = this->loading_visits.find(vehicle);
	if (found == this->loading_visits.end() || !found->second) return {};
	VisitID id = *found->second;
	const Trip &trip = this->trips[id.trip];
	const auto &visit = trip.prediction.visits[id.visit];
	if (trip.operation != Operation::Running || !trip.loading || trip.current_order != order ||
			visit.order != order || this->GetVisitStop(id).station != station) return {};
	return id;
}

/** Build once, without changing capture-order VisitIDs or legacy query ordering. */
void PassengerServiceGraph::BuildDepartureIndex()
{
	/* Union declared stops, ignoring direction, load/unload rules and times. This is
	 * an overapproximation, including unresolved conditional branches; it can only
	 * remove uncertainty on physically disconnected services, never infer a time. */
	std::map<StationID, size_t> ranks;
	auto root = [&](StationID station) {
		StationID id = station;
		while (this->component_roots.at(id) != id) id = this->component_roots.at(id);
		while (station != id) {
			StationID parent = this->component_roots.at(station);
			this->component_roots.at(station) = id;
			station = parent;
		}
		return id;
	};
	for (const auto &service : this->services) {
		for (const auto &stop : service.stops) {
			this->component_roots.try_emplace(stop.station, stop.station);
			ranks.try_emplace(stop.station, 0);
		}
		if (service.stops.empty()) continue;
		for (const auto &stop : service.stops) {
			StationID a = root(service.stops.front().station), b = root(stop.station);
			if (a == b) continue;
			if (ranks[a] < ranks[b]) std::swap(a, b);
			this->component_roots[b] = a;
			if (ranks[a] == ranks[b]) ++ranks[a];
		}
	}
	for (auto &[station, parent] : this->component_roots) parent = root(station);
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
			if (trip.prediction.visits[id.visit].loading_now) {
				auto [loading, inserted] = this->loading_visits.try_emplace(trip.vehicle, id);
				if (!inserted) loading->second.reset(); // Multiple current events are ambiguous.
			}
			if (trip.operation != Operation::Running || !this->GetVisitStop(id).can_board || trip.capacity == 0) continue;
			(trip.prediction.visits[id.visit].departure ? index.timed : index.unknown).push_back(id);
		}
		std::sort(index.timed.begin(), index.timed.end(), [&](VisitID a, VisitID b) {
			const Trip &ta = this->trips[a.trip], &tb = this->trips[b.trip];
			return std::tuple(*ta.prediction.visits[a.visit].departure, ta.vehicle, a.trip, a.visit) <
					std::tuple(*tb.prediction.visits[b.visit].departure, tb.vehicle, b.trip, b.visit);
		});
	}
	for (const auto &[station, index] : this->station_departures) {
		auto status = index.unknown.empty() ? index.status : MergeGraphStatus(index.status, PassengerPredictionStatus::Unknown);
		if (status == PassengerPredictionStatus::Complete) continue;
		this->uncertain_stations.push_back(station);
		this->uncertain_status = MergeGraphStatus(this->uncertain_status, status);
		auto &component = this->uncertain_components[this->component_roots.at(station)];
		component.stations.push_back(station);
		component.status = MergeGraphStatus(component.status, status);
	}
}

std::span<const StationID> PassengerServiceGraph::GetUncertainStations(StationID destination) const
{
	auto root = this->component_roots.find(destination);
	if (root == this->component_roots.end()) return {};
	auto component = this->uncertain_components.find(root->second);
	return component == this->uncertain_components.end() ? std::span<const StationID>{} : component->second.stations;
}

PassengerPredictionStatus PassengerServiceGraph::GetUncertainStatus(StationID destination) const
{
	auto root = this->component_roots.find(destination);
	if (root == this->component_roots.end()) return PassengerPredictionStatus::Complete;
	auto component = this->uncertain_components.find(root->second);
	return component == this->uncertain_components.end() ? PassengerPredictionStatus::Complete : component->second.status;
}

std::span<const PassengerServiceGraph::VisitID> PassengerServiceGraph::GetStationVisits(StationID station) const
{
	auto it = this->station_visits.find(station);
	return it == this->station_visits.end() ? std::span<const VisitID>{} : it->second;
}

PassengerServiceGraph::Departures PassengerServiceGraph::GetDepartures(StationID station, StateTicks from, StateTicks to, bool scoped_query) const
{
	Departures result{ {}, {}, scoped_query ? this->status : this->GetStatus() };
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
	result.status = this->GetStatus();
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
