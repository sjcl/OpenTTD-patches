/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file passenger_routing.cpp Passenger arrival-time comparison on the simulation thread. */

#include "stdafx.h"
#include "passenger_routing.h"
#include "vehicle_base.h"
#include "station_base.h"
#include "settings_type.h"
#include "timetable.h"
#include "date_func.h"
#include "passenger_service_index.h"
#include "roadveh.h"
#include "roadstop_base.h"
#include "ship.h"
#include "aircraft.h"
#include "airport.h"
#include "infrastructure_func.h"

#include "safeguards.h"

/** Only actual station stops can carry or exchange passengers. */
static bool IsPassengerStop(const Order &order)
{
	return IsPassengerStationStop(order);
}

PassengerDepotTraversal ClassifyPassengerDepot(const Order &order)
{
	/* Match VehicleEnterDepot and UpdateOrderDest: only a regular instruction
	 * resumes the same order list, and a refit can discard passengers or fail. */
	if (!order.IsType(OT_GOTO_DEPOT) || !order.GetDepotOrderType().Test(OrderDepotTypeFlag::PartOfOrders) ||
			order.GetDepotOrderType().Test(OrderDepotTypeFlag::Breakdown) || order.IsRefit() ||
			(order.GetDepotActionType() & (ODATFB_HALT | ODATFB_SELL))) return PassengerDepotTraversal::Blocked;
	if (order.GetDepotOrderType().Test(OrderDepotTypeFlag::Service) ||
			(order.GetDepotActionType() & (ODATFB_NEAREST_DEPOT | ODATFB_UNBUNCH))) return PassengerDepotTraversal::Untimed;
	return PassengerDepotTraversal::Timed;
}

static bool ConsumePassengerWork(size_t &budget, size_t work = 1)
{
	if (work > budget) { budget = 0; return false; }
	budget -= work;
	return true;
}

/**
 * Predict a journey without averaging local and express services. Timings are absolute state ticks.
 * Unknown timings remain unknown: distance and station-level LinkGraph times are never substituted.
 * Journey mode ends at a revisit to the boarding station, preserving the CargoDist contract.
 * Visit mode exposes the same vehicle traversal to future routers, without boarding/transfer policy.
 */
static PassengerJourney PredictPassengerJourneyImpl(const PassengerRouteState &state, VehicleOrderID boarding_order, StationID target,
		CargoType cargo, StateTicks now, bool allow_intermediate, size_t &budget,
		std::optional<StateTicks> earliest_boarding = {}, std::optional<StateTicks> arrival_limit = {}, bool skip_current_departure = false,
		PassengerVisitPrediction *visits = nullptr, size_t max_visits = 0, size_t max_steps = 0,
		const std::function<bool()> &admit_visit = {}, bool prefix_only = false,
		PassengerTimingPolicy timing = PassengerTimingPolicy::Conservative)
{
	PassengerJourney result;
	if (state.orders.empty() || state.current_order >= state.orders.size() || boarding_order >= state.orders.size()) return result;
	if (!state.usable_stops.empty() && state.usable_stops.size() != state.orders.size()) return result;
	const Order &boarding = state.orders[boarding_order];
	if (!visits && !IsPassengerStop(boarding)) return result;
	if (!visits && !allow_intermediate && boarding.GetCargoLoadType(cargo) == OrderLoadType::NoLoad) return result;

	auto exhausted = [&]() -> PassengerJourney {
		if (visits) { visits->status = PassengerPredictionStatus::BudgetExhausted; visits->stop = PassengerPredictionStop::Work; }
		return { true, std::nullopt, std::nullopt };
	};
	auto horizon = [&]() -> PassengerJourney {
		if (visits) {
			visits->status = PassengerPredictionStatus::Complete;
			visits->complete_until = *arrival_limit;
		}
		return {};
	};

	/* Unsupported orders may change the path. Report uncertainty if the target occurs anywhere,
	 * so a known slow service cannot conceal an unpredicted faster alternative. */
	auto uncertain = [&](PassengerPredictionStop why = PassengerPredictionStop::Unsupported) -> PassengerJourney {
		if (visits) {
			visits->stop = why;
			if (visits->uncertain_order == INVALID_VEH_ORDER_ID) {
				visits->uncertain_order = visits->stopped_order;
				visits->uncertain_reason = why;
			}
			return {};
		}
		if (earliest_boarding) return { true, std::nullopt, std::nullopt };
		for (const Order &order : state.orders) {
			if (!ConsumePassengerWork(budget)) return { true, std::nullopt, std::nullopt };
			if (IsPassengerStop(order) && order.GetDestination() == target && order.GetCargoUnloadType(cargo) != OrderUnloadType::NoUnload) {
				return { true, std::nullopt, std::nullopt };
			}
		}
		return {};
	};

	StateTicks tick = now;
	const bool assume_timetable = timing == PassengerTimingPolicy::AssumeTimetable;
	int64_t lateness = state.lateness;
	bool timing_known = true;
	bool boarded = false;
	bool skip_travel = state.loading;
	VehicleOrderID index = state.current_order;
	std::map<int, int32_t> dispatch_history;
	std::optional<StateTicks> previous_boarding;
	StateTicksDelta leg_time{0}, movement_time{0};
	bool leg_known = true;
	bool have_previous_stop = false;
	auto unknown_timing = [&]() {
		timing_known = false;
		if (visits && visits->uncertain_order == INVALID_VEH_ORDER_ID) {
			visits->uncertain_order = index;
			visits->uncertain_reason = PassengerPredictionStop::Timing;
		}
	};
	/* Certify only the prefix before the next possible event. Never advance through an
	 * unknown dwell, dispatch assignment or path, nor include unprocessed events at tick. */
	auto cover_prefix = [&]() {
		if (visits) { visits->stopped_at = tick; visits->stopped_order = index; }
		if (visits && timing_known) visits->complete_until = std::min(tick, *arrival_limit);
	};
	/* Collect independent segment durations while advancing the common timing engine.
	 * An unknown absolute arrival must not hide recorded times on later individual legs. */
	auto advance_leg = [&](int64_t duration, bool known) {
		tick += duration;
		if (visits) { leg_time += StateTicksDelta{duration}; leg_known &= known; }
	};
	/* Connections may repeat the actual vehicle's circuit, never an averaged fleet headway.
	 * The time horizon and shared work budget also bound zero-time loops and conditional jumps. */
	for (size_t steps = 0; visits || earliest_boarding || steps < state.orders.size() * 2; ++steps) {
		if (visits) { visits->stopped_at = tick; visits->stopped_order = index; }
		if (arrival_limit && timing_known && tick >= *arrival_limit) return horizon();
		if (visits && (steps >= max_steps || visits->visits.size() >= max_visits)) {
			visits->status = PassengerPredictionStatus::LimitReached;
			visits->stop = steps >= max_steps ? PassengerPredictionStop::Steps : PassengerPredictionStop::Visits;
			return {};
		}
		if (!ConsumePassengerWork(budget)) return exhausted();
		const Order &order = state.orders[index];
		if (order.IsType(OT_CONDITIONAL)) {
			if (order.GetConditionVariable() != OrderConditionVariable::Unconditionally) return uncertain(PassengerPredictionStop::Conditional);
			if (!order.IsWaitTimetabled()) unknown_timing();
			if (prefix_only && !timing_known) return uncertain(PassengerPredictionStop::Timing);
			advance_leg(order.GetWaitTime(), order.IsWaitTimetabled());
			cover_prefix();
			index = order.GetConditionSkipToOrder();
			if (index >= state.orders.size()) return uncertain();
			skip_travel = true; // Branch time replaces the destination's travel time.
			continue;
		}
		/* Implicit orders are historical observations, not promises of future stops.
		 * The planner's operating model follows explicit instructions only. Real
		 * deviations are validated at the next loading event, with signature invalidation. */
		if (order.IsType(OT_IMPLICIT)) { index = (index + 1) % state.orders.size(); continue; }
		/* A depot is an operating event, never a passenger stop. Optional entry
		 * and variable release times lose timing without losing the known suffix. */
		if (order.IsRefit()) return uncertain(PassengerPredictionStop::Refit);
		const bool depot = order.IsType(OT_GOTO_DEPOT);
		const auto depot_traversal = depot ? ClassifyPassengerDepot(order) : PassengerDepotTraversal::Blocked;
		if ((depot && depot_traversal == PassengerDepotTraversal::Blocked) || order.IsSlotCounterOrder() || order.IsType(OT_DUMMY)) return uncertain();
		if (depot_traversal == PassengerDepotTraversal::Untimed) unknown_timing();
		if (order.IsType(OT_LABEL)) {
			skip_travel = false;
			index = (index + 1) % state.orders.size();
			continue;
		}
		if (!order.IsType(OT_GOTO_STATION) && !order.IsType(OT_GOTO_WAYPOINT) && !depot) return uncertain();
		/* A removed/incompatible facility changes the actual path (the controller may skip
		 * this order). Keep the certified prefix, but never invent the skipped leg's time. */
		if (IsPassengerStop(order) && !state.usable_stops.empty() && !state.usable_stops[index]) return uncertain(PassengerPredictionStop::Facilities);

		if (!skip_travel) {
			const bool travel_known = depot_traversal != PassengerDepotTraversal::Untimed &&
					(order.IsTravelTimetabled() || (!assume_timetable && order.GetTravelTime() != 0));
			if (!travel_known) unknown_timing();
			int64_t travel = order.GetTravelTime();
			if (steps == 0) {
				/* The recorded journey has already elapsed, but the train has not arrived.
				 * Its remaining physical delay is unknown; don't invent an immediate arrival. */
				if (state.elapsed > travel) unknown_timing();
				travel = std::max<int64_t>(0, travel - state.elapsed);
			}
			if (prefix_only && !timing_known) return uncertain(PassengerPredictionStop::Timing);
			advance_leg(travel, travel_known);
			movement_time += StateTicksDelta{travel};
		}
		skip_travel = false;

		const bool stop = IsPassengerStop(order);
		cover_prefix();
		if (arrival_limit && timing_known && tick >= *arrival_limit) return horizon();
		if (visits && stop) {
			/* Reserve before vector growth. The caller covers owned visits and all their indices;
			 * allocator capacity and measured RSS never participate in admission. */
			if (admit_visit && !admit_visit()) {
				visits->status = PassengerPredictionStatus::LimitReached;
				visits->stop = PassengerPredictionStop::Memory;
				visits->stopped_at = tick;
				return {};
			}
			/* Explicit geometric requests bound the old/new buffer overlap. Grow
			 * from the logical visit count, never from observed allocator capacity. */
			const size_t count = visits->visits.size();
			if (count == 0 || (count & (count - 1)) == 0) visits->visits.reserve(std::min(max_visits, std::max<size_t>(1, count * 2)));
			visits->visits.push_back({ index, timing_known && !(steps == 0 && state.loading) ? std::optional(tick) : std::nullopt,
					std::nullopt, steps == 0 && state.loading, have_previous_stop && leg_known ? std::optional(leg_time) : std::nullopt });
			visits->visits.back().movement_from_previous = movement_time;
			movement_time = StateTicksDelta{0};
			leg_time = StateTicksDelta{0};
			leg_known = true;
			have_previous_stop = true;
		}
		if (boarded && stop) {
			if (order.GetDestination() == boarding.GetDestination() && order.GetCargoUnloadType(cargo) != OrderUnloadType::NoUnload) return {};
			if (order.GetDestination() == target && order.GetCargoUnloadType(cargo) != OrderUnloadType::NoUnload) {
				result.reachable = true;
				if (timing_known) result.arrival = tick;
				return result;
			}
			if (order.GetCargoUnloadType(cargo) == OrderUnloadType::Unload || order.GetCargoUnloadType(cargo) == OrderUnloadType::Transfer) return {};
			/* Cargo waiting for a via can only board a vehicle whose next cargo stop is that via.
			 * Keep the existing station loading contract; through riding is only for onboard cargo. */
			if (!allow_intermediate && (order.GetCargoLoadType(cargo) != OrderLoadType::NoLoad || order.GetCargoUnloadType(cargo) != OrderUnloadType::NoUnload)) return {};
		}

		if (stop) {
			if (state.scheduled_dispatch && order.IsScheduledDispatchOrder(true)) {
				const int schedule_index = order.GetDispatchScheduleIndex();
				if (schedule_index < 0 || static_cast<size_t>(schedule_index) >= state.schedules.size() ||
						state.schedules[schedule_index].GetScheduledDispatchDuration() == 0 || state.schedules[schedule_index].GetScheduledDispatch().empty()) return uncertain(PassengerPredictionStop::Dispatch);
			}
			const OrderLoadType load = order.GetCargoLoadType(cargo);
			if (!assume_timetable && (IsFullLoadOrderLoadType(load) || order.GetLeaveType() != OLT_NORMAL)) unknown_timing();
			if (!assume_timetable && order.GetLoadType() == OrderLoadType::CargoTypeLoad) {
				/* Full loading another cargo in this consist can also hold up the passenger coach. */
				for (CargoType other{}; other < NUM_CARGO; ++other) {
					if (!ConsumePassengerWork(budget)) return exhausted();
					if (IsFullLoadOrderLoadType(order.GetCargoLoadType(other))) unknown_timing();
				}
			}
			if (!order.IsWaitTimetabled()) unknown_timing();
			if (prefix_only && !timing_known) return uncertain(PassengerPredictionStop::Timing);
			const bool future_dispatch = state.scheduled_dispatch && order.IsScheduledDispatchOrder(true) && !(steps == 0 && state.loading);
			/* An assigned current dispatch departure is encoded by lateness. Preserve its
			 * slot; ordinary lateness never shortens or extends the planner's dwell. */
			const bool assigned_dispatch = state.scheduled_dispatch && order.IsScheduledDispatchOrder(true) && steps == 0 && state.loading;
			int64_t wait = future_dispatch ? order.GetTimetabledWait() : assume_timetable && !assigned_dispatch ?
					order.GetWaitTime() : std::max<int64_t>(0, static_cast<int64_t>(order.GetWaitTime()) - lateness);
			if (steps == 0 && state.loading) wait = std::max<int64_t>(0, wait - state.elapsed);
			tick += wait;
			if (visits) visits->stopped_at = tick;
			lateness = std::max<int64_t>(0, lateness - order.GetWaitTime());

			if (future_dispatch) {
				const int schedule_index = order.GetDispatchScheduleIndex();
				if (schedule_index < 0 || static_cast<size_t>(schedule_index) >= state.schedules.size()) return uncertain(PassengerPredictionStop::Dispatch);
				const DispatchSchedule &schedule = state.schedules[schedule_index];
				/* Multiple vehicles can compete for the same future slot. Until a slot is assigned,
				 * don't guess their booking order. Already assigned loading departures remain usable. */
				if (schedule.GetScheduledDispatchDuration() == 0 || (state.shared_vehicles > 1 && !schedule.GetScheduledDispatchReuseSlots())) return uncertain(PassengerPredictionStop::Dispatch);
				/* Slot search is linear in the dispatch slot count, not just the order count. */
				if (!ConsumePassengerWork(budget, schedule.GetScheduledDispatch().size())) return exhausted();
				auto history = dispatch_history.try_emplace(schedule_index, schedule.GetScheduledDispatchLastDispatch()).first;
				const StateTicks slot = GetScheduledDispatchTime(schedule, tick, history->second).first;
				if (slot == INVALID_STATE_TICKS) return uncertain(PassengerPredictionStop::Dispatch);
				const auto offset = (slot - schedule.GetScheduledDispatchStartTick()).base();
				if (offset < INT32_MIN || offset > INT32_MAX) return uncertain(PassengerPredictionStop::Dispatch);
				history->second = static_cast<int32_t>(offset);
				tick = std::max(tick, slot);
				lateness = 0;
			}
		} else if (depot) {
			/* VehicleEnterDepot advances on entry when no wait is declared.
			 * Raw unconfirmed waits do not hold the vehicle; departure from the
			 * depot is included in the next order's recorded travel time. */
			const bool wait_known = depot_traversal == PassengerDepotTraversal::Timed;
			if (!wait_known) unknown_timing();
			if (state.scheduled_dispatch && order.IsScheduledDispatchOrder(true)) return uncertain(PassengerPredictionStop::Dispatch);
			if (prefix_only && !timing_known) return uncertain(PassengerPredictionStop::Timing);
			advance_leg(order.GetTimetabledWait(), wait_known);
		} else if (order.IsType(OT_GOTO_WAYPOINT)) {
			if (state.scheduled_dispatch && order.IsScheduledDispatchOrder(true)) return uncertain(PassengerPredictionStop::Dispatch);
			if (order.GetWaitTime() > 0 && !order.IsWaitTimetabled()) unknown_timing();
			if (prefix_only && !timing_known) return uncertain(PassengerPredictionStop::Timing);
			advance_leg(order.GetWaitTime(), order.GetWaitTime() == 0 || order.IsWaitTimetabled());
		}
		cover_prefix();
		if (visits && stop) {
			if (timing_known) visits->visits.back().departure = tick;
			/* With unknown times, one order circuit is enough to expose the known path prefix.
			 * Subsequent occurrences cannot be assigned to a time window. */
			if (!timing_known && steps + 1 >= state.orders.size()) return uncertain(PassengerPredictionStop::Timing);
		}
		if (!visits && !boarded && index == boarding_order) {
			if (earliest_boarding) {
				if (!timing_known) return uncertain();
				if (tick >= *arrival_limit) return {};
				if (previous_boarding && tick <= *previous_boarding) return uncertain();
				previous_boarding = tick;
			}
			if ((earliest_boarding && tick < *earliest_boarding) || (steps == 0 && state.loading && skip_current_departure)) {
				index = (index + 1) % state.orders.size();
				continue;
			}
			boarded = true;
			result.boarding_now = steps == 0 && state.loading;
			if (timing_known) result.departure = tick;
		}
		index = (index + 1) % state.orders.size();
	}
	return {};
}

PassengerVisitPrediction PredictPassengerVisits(const PassengerRouteState &state, CargoType cargo, StateTicks now,
		StateTicks end, size_t max_visits, size_t max_steps, size_t &budget, const std::function<bool()> &admit_visit, bool prefix_only, PassengerTimingPolicy timing)
{
	PassengerVisitPrediction result;
	result.start = result.complete_until = result.stopped_at = now;
	if (now >= end) { result.status = PassengerPredictionStatus::Complete; return result; }
	if (cargo >= NUM_CARGO) return result;
	PredictPassengerJourneyImpl(state, state.current_order, StationID::Invalid(), cargo, now, true, budget,
			{}, end, false, &result, max_visits, max_steps, admit_visit, prefix_only, timing);
	return result;
}

PassengerJourney PredictPassengerJourney(const PassengerRouteState &state, VehicleOrderID boarding_order, StationID target,
		CargoType cargo, StateTicks now, bool allow_intermediate, PassengerTimingPolicy timing)
{
	size_t budget = std::numeric_limits<size_t>::max();
	return PredictPassengerJourneyImpl(state, boarding_order, target, cargo, now, allow_intermediate, budget,
			{}, {}, false, nullptr, 0, 0, {}, false, timing);
}

PassengerJourney PredictPassengerConnection(const PassengerRouteState &state, VehicleOrderID boarding_order, StationID target,
		CargoType cargo, StateTicks now, StateTicks earliest_boarding, StateTicks arrival_limit, size_t &budget, bool skip_current_departure)
{
	if (earliest_boarding >= arrival_limit) return {};
	return PredictPassengerJourneyImpl(state, boarding_order, target, cargo, now, false, budget, earliest_boarding, arrival_limit, skip_current_departure);
}

/** Choose the earliest usable direct alternative, applying a minimum transfer time as well as a cost. */
PassengerTransferDecision ComparePassengerJourneys(const PassengerJourney &stay, std::span<const PassengerJourney> alternatives,
		StateTicks now, Ticks transfer_penalty, bool alternatives_complete)
{
	if (!stay.reachable || !stay.arrival) return PassengerTransferDecision::Unknown;
	std::optional<StateTicks> best;
	for (const PassengerJourney &alternative : alternatives) {
		if (!alternative.reachable) continue;
		if (!alternative.departure || !alternative.arrival) return PassengerTransferDecision::Unknown;
		if (*alternative.departure < now + transfer_penalty) continue;
		const StateTicks arrival = *alternative.arrival + transfer_penalty;
		if (!best || arrival < *best) best = arrival;
	}
	if (!best) return alternatives_complete ? PassengerTransferDecision::Stay : PassengerTransferDecision::Unknown;
	return *stay.arrival <= *best ? PassengerTransferDecision::Stay : PassengerTransferDecision::Transfer;
}

/* This index contains IDs only, and is independent of timetables, consists, load flags and dispatch.
 * Those are read afresh for each arrival. Order destination registration invalidates the index. */
static std::map<StationID, std::vector<OrderListID>> _passenger_service_index;
static bool _passenger_service_index_valid = false;
static size_t _passenger_service_index_work = 0;

void InvalidatePassengerServiceIndex()
{
	InvalidatePassengerGraphServiceIndex();
	if (!_passenger_service_index_valid) return;
	_passenger_service_index_valid = false;
	_passenger_service_index.clear();
}

static bool PreparePassengerServiceIndex(size_t &budget)
{
	if (!_passenger_service_index_valid) {
		/* Preflight bounds a cold rebuild. Charge the same logical discovery cost when warm:
		 * peers (including a newly joined client) must not decide differently due to cache state. */
		size_t work = 0;
		for (const OrderList *list : OrderList::Iterate()) {
			if (budget - work < 2 || list->GetNumOrders() > budget - work - 2) { budget = 0; return false; }
			work += 2 + list->GetNumOrders(); // Both preflight and rebuild visit this list.
		}
		_passenger_service_index_work = work;
		for (const OrderList *list : OrderList::Iterate()) {
			const Vehicle *v = list->GetFirstSharedVehicle();
			if (v == nullptr || v->type >= VehicleType::CompanyEnd) continue;
			for (const Order *order : list->Orders()) {
				if (!order->IsType(OT_GOTO_STATION)) continue;
				auto &services = _passenger_service_index[order->GetDestination().ToStationID()];
				if (services.empty() || services.back() != list->index) services.push_back(list->index);
			}
		}
		_passenger_service_index_valid = true;
	}
	return ConsumePassengerWork(budget, _passenger_service_index_work);
}

static const std::vector<OrderListID> &GetPassengerServices(StationID station)
{
	static const std::vector<OrderListID> empty;
	auto it = _passenger_service_index.find(station);
	return it == _passenger_service_index.end() ? empty : it->second;
}

bool PassengerRoutingContext::IsEnabled(CargoType cargo) const
{
	return _settings_game.linkgraph.passenger_transfer_timetables && IsPassengerServiceVehicle(this->vehicle) &&
			IsCargoInClass(cargo, CargoClass::Passengers) && _settings_game.linkgraph.GetDistributionType(cargo) != DistributionType::Manual;
}

bool IsPassengerServiceVehicle(const Vehicle *v)
{
	return v != nullptr && v->type < VehicleType::CompanyEnd && v->IsPrimaryVehicle();
}

bool HasPassengerVehicleCapacity(const Vehicle *v, CargoType cargo, size_t &budget)
{
	if (!IsPassengerServiceVehicle(v) || cargo >= NUM_CARGO || !IsCargoInClass(cargo, CargoClass::Passengers)) return false;
	for (const Vehicle *part = v; part != nullptr; part = part->Next()) {
		if (!ConsumePassengerWork(budget)) return false;
		if (part->cargo_type == cargo && part->cargo_cap > 0) return true;
	}
	return false;
}

bool CanPassengerVehicleUseStation(const Vehicle *v, StationID station, const Order *order, size_t &budget)
{
	if (!ConsumePassengerWork(budget) || !IsPassengerServiceVehicle(v)) return false;
	const Station *st = Station::GetIfValid(station);
	if (st == nullptr || (order != nullptr && !IsPassengerStop(*order)) || !IsInfraUsageAllowed(v->type, v->owner, st->owner)) return false;
	switch (v->type) {
		case VehicleType::Train: return st->facilities.Test(StationFacility::Train);
		case VehicleType::Road: {
			const RoadVehicle *rv = RoadVehicle::From(v);
			/* The controller chooses facilities from the front compartment's actual cargo
			 * class; a mixed articulated truck can carry passengers at a truck stop too. */
			RoadStopType type = IsCargoInClass(v->cargo_type, CargoClass::Passengers) ? RoadStopType::Bus : RoadStopType::Truck;
			for (const RoadStop *rs = st->GetPrimaryRoadStop(type); rs != nullptr; rs = rs->next) {
				if (!ConsumePassengerWork(budget)) return false;
				if (!IsValidTile(rs->xy) || !IsStationRoadStopTile(rs->xy) || GetStationIndex(rs->xy) != station ||
						!HasTileAnyRoadType(rs->xy, rv->compatible_roadtypes) || !IsInfraTileUsageAllowed(v->type, v->owner, rs->xy)) continue;
				if (IsBayRoadStopTile(rs->xy) && rv->HasArticulatedPart()) continue;
				if (order != nullptr && order->GetRoadVehTravelDirection() != DiagDirection::Invalid &&
						IsDriveThroughStopTile(rs->xy) && DiagDirToAxis(order->GetRoadVehTravelDirection()) != GetDriveThroughStopAxis(rs->xy)) continue;
				if (order != nullptr && order->GetRoadVehTravelDirection() != DiagDirection::Invalid &&
						IsBayRoadStopTile(rs->xy) && order->GetRoadVehTravelDirection() != ReverseDiagDir(GetBayRoadStopDir(rs->xy))) continue;
				return true;
			}
			return false;
		}
		case VehicleType::Ship: return st->facilities.Test(StationFacility::Dock);
		case VehicleType::Aircraft: {
			if (!st->facilities.Test(StationFacility::Airport) || st->airport.tile == INVALID_TILE || st->airport.blocks.Test(AirportBlock::AirportClosed)) return false;
			const Aircraft *aircraft = Aircraft::From(v);
			return st->airport.GetFTA()->flags.Test(aircraft->subtype == AIR_HELICOPTER ? AirportFTAClass::Flag::Helicopters : AirportFTAClass::Flag::Airplanes);
		}
		default: return false;
	}
}

/** Loading is a physical stop, rather than a direction or an aircraft movement heading.
 * Taxi/landing/holding time belongs to the current travel order until BeginLoading. */
static bool IsPassengerVehicleLoadingAtStop(const Vehicle *v, const Order &order)
{
	if (!v->current_order.IsAnyLoadingType() || v->IsInDepot()) return false;
	switch (v->type) {
		case VehicleType::Train: return true; // Train platform/reversal checks remain in LoadUnloadVehicle.
		case VehicleType::Road: {
			const RoadVehicle *rv = RoadVehicle::From(v);
			/* BeginLoading can stop behind another vehicle before RVS_ENTERED_STOP is set.
			 * The loading order and physical stop checks also cover this case. */
			return (IsInsideMM(rv->state, RVSB_IN_ROAD_STOP, RVSB_IN_ROAD_STOP_END) ||
					IsInsideMM(rv->state, RVSB_IN_DT_ROAD_STOP, RVSB_IN_DT_ROAD_STOP_END)) &&
					IsStationRoadStopTile(v->tile) && GetStationIndex(v->tile) == v->last_station_visited &&
					HasTileAnyRoadType(v->tile, rv->compatible_roadtypes) && IsInfraTileUsageAllowed(v->type, v->owner, v->tile) &&
					GetRoadStopType(v->tile) == (IsCargoInClass(v->cargo_type, CargoClass::Passengers) ? RoadStopType::Bus : RoadStopType::Truck) &&
					(order.GetRoadVehTravelDirection() == DiagDirection::Invalid || order.GetRoadVehTravelDirection() == DirToDiagDir(v->direction));
		}
		case VehicleType::Ship: {
			const Station *st = Station::GetIfValid(v->last_station_visited);
			return st != nullptr && IsDockingTile(v->tile) && st->docking_station.Contains(v->tile) && IsShipDestinationTile(v->tile, st->index);
		}
		case VehicleType::Aircraft: {
			const Aircraft *a = Aircraft::From(v);
			return a->targetairport == v->last_station_visited && ((a->state >= TERM1 && a->state <= TERM6) ||
					a->state == TERM7 || a->state == TERM8 || a->state == HELIPAD1 || a->state == HELIPAD2 || a->state == HELIPAD3);
		}
		default: return false;
	}
}

std::optional<PassengerRouteState> GetPassengerRouteState(const Vehicle *v, size_t *budget, PassengerPredictionReason *reason, bool allow_implicit_current)
{
	size_t local_budget = 65536;
	if (budget == nullptr) budget = &local_budget;
	auto fail = [&](PassengerPredictionReason why) -> std::optional<PassengerRouteState> { if (reason != nullptr) *reason = why; return {}; };
	if (!IsPassengerServiceVehicle(v) || v->orders == nullptr || v->GetNumOrders() == 0 ||
			v->vehstatus.Any({VehState::Stopped, VehState::Crashed}) || v->IsInDepot()) return fail(PassengerPredictionReason::VehicleState);
	if (v->type == VehicleType::Aircraft && Aircraft::From(v)->flags.Test(VehicleAirFlag::DestinationTooFar)) return fail(PassengerPredictionReason::VehicleState);
	bool loading = v->current_order.IsAnyLoadingType();
	bool implicit_current = allow_implicit_current && loading && v->cur_implicit_order_index < v->GetNumOrders() &&
			v->GetOrder(v->cur_implicit_order_index)->IsType(OT_IMPLICIT);
	if (v->cur_real_order_index >= v->GetNumOrders() || (!implicit_current && v->cur_implicit_order_index != v->cur_real_order_index)) return fail(PassengerPredictionReason::OrderProgress);
	if (!implicit_current && v->cur_timetable_order_index != INVALID_VEH_ORDER_ID && v->cur_timetable_order_index != v->cur_real_order_index) return fail(PassengerPredictionReason::OrderProgress);
	VehicleOrderID current = implicit_current ? v->cur_implicit_order_index : v->cur_real_order_index;
	const Order *order = v->GetOrder(current);
	if (loading ? ((!implicit_current && !IsPassengerStop(*order)) || order->GetDestination() != v->last_station_visited) :
			(v->current_order.GetType() != order->GetType() || v->current_order.GetDestination() != order->GetDestination())) return fail(PassengerPredictionReason::OrderProgress);
	if (loading && !IsPassengerVehicleLoadingAtStop(v, *order)) return fail(PassengerPredictionReason::VehicleState);
	PassengerRouteState state{ v->orders->GetOrderVector(), v->orders->GetScheduledDispatchScheduleSet(), current,
			v->current_order_time, v->lateness_counter, loading, v->vehicle_flags.Test(VehicleFlag::ScheduledDispatch), v->orders->GetNumVehicles() };
	if (!ConsumePassengerWork(*budget, state.orders.size())) return fail(PassengerPredictionReason::Budget);
	state.usable_stops.reserve(state.orders.size());
	for (const Order &stop : state.orders) {
		bool usable = !IsPassengerStop(stop) || CanPassengerVehicleUseStation(v, stop.GetDestination().ToStationID(), &stop, *budget);
		state.usable_stops.push_back(usable);
		if (!usable && reason != nullptr) *reason = PassengerPredictionReason::StationFacilities;
	}
	if (*budget == 0) return fail(PassengerPredictionReason::Budget);
	return state;
}

const std::optional<PassengerRouteState> &PassengerRoutingContext::GetState(const Vehicle *v)
{
	auto [it, inserted] = this->states.try_emplace(v);
	if (inserted) it->second = GetPassengerRouteState(v, &this->prediction_budget);
	return it->second;
}

const PassengerRoutingContext::Capacity &PassengerRoutingContext::GetCapacity(const Vehicle *v, CargoType cargo)
{
	auto [it, inserted] = this->capacities.try_emplace(std::pair(v, cargo));
	if (!inserted) return it->second;
	const bool loading_here = v->current_order.IsAnyLoadingType() && v->last_station_visited == this->station;
	for (const Vehicle *u = v; u != nullptr; u = u->Next()) {
		if (!ConsumePassengerWork(this->prediction_budget)) break;
		if (u->cargo_type != cargo) continue;
		it->second.total += u->cargo_cap;
		/* TotalCount includes reserved loads. Only a staged unload at this visit is certain;
		 * neither present occupancy nor packet flows predict unloading on a future circuit. */
		const uint unloading = loading_here && u->vehicle_flags.Test(VehicleFlag::CargoUnloading) &&
				v->current_order.GetCargoUnloadType(cargo) != OrderUnloadType::NoUnload ? u->cargo.UnloadCount() : 0;
		const uint remaining = u->cargo.TotalCount() - unloading;
		if (remaining < u->cargo_cap) it->second.free += u->cargo_cap - remaining;
	}
	return it->second;
}

uint64_t PassengerRoutingContext::GetDemand(CargoType cargo)
{
	auto [it, inserted] = this->demands.try_emplace(cargo, 0);
	if (inserted) {
		for (const Vehicle *u = this->vehicle; u != nullptr; u = u->Next()) {
			if (!ConsumePassengerWork(this->prediction_budget)) break;
			if (u->cargo_type == cargo) it->second += u->cargo.StoredCount();
		}
	}
	return it->second;
}

uint64_t PassengerRoutingContext::GetWaiting(CargoType cargo, StationID target)
{
	const Station *st = Station::GetIfValid(this->station);
	if (st == nullptr || st->goods[cargo].data == nullptr) return 0;
	const auto *packets = st->goods[cargo].data->cargo.Packets();
	/* The loader matches the next cargo stop, not an order list. These are competing eligible
	 * queues, not station-wide passengers and not a reservation for a particular express. */
	auto count = [&](StationID via) {
		auto [it, inserted] = this->waiting.try_emplace(std::pair(cargo, via), 0);
		if (!inserted) return it->second;
		for (StationCargoList::ConstIterator cp(packets->lower_bound(via)); cp != packets->end() && cp.GetKey() == via; ++cp) {
			if (!ConsumePassengerWork(this->prediction_budget)) return it->second;
			it->second += (*cp)->Count();
		}
		return it->second;
	};
	const uint64_t via_count = count(target);
	return target == StationID::Invalid() ? via_count : via_count + count(StationID::Invalid());
}

bool PassengerRoutingContext::CanContinue(CargoType cargo, StationID target)
{
	auto key = std::pair(cargo, target);
	auto [it, inserted] = this->journeys.try_emplace(key);
	if (inserted) {
		auto state = this->GetState(this->vehicle);
		if (state) {
			it->second = PredictPassengerJourneyImpl(*state, state->current_order, target, cargo, _state_ticks, true, this->prediction_budget);
		}
	}
	return it->second.reachable && it->second.arrival.has_value();
}

PassengerTransferDecision PassengerRoutingContext::Decide(CargoType cargo, StationID target)
{
	auto key = std::pair(cargo, target);
	auto [it, inserted] = this->decisions.try_emplace(key, PassengerTransferDecision::Unknown);
	if (!inserted || !this->IsEnabled(cargo) || !this->CanContinue(cargo, target)) return it->second;
	const PassengerJourney &stay = this->journeys.at(key);
	const Ticks penalty = _settings_game.linkgraph.passenger_transfer_penalty;
	const StateTicks earliest_boarding = _state_ticks + penalty;
	const StateTicks arrival_limit = *stay.arrival - penalty;
	if (earliest_boarding >= arrival_limit) return it->second = PassengerTransferDecision::Stay;
	if (!this->services_ready) {
		if (!PreparePassengerServiceIndex(this->prediction_budget)) return it->second;
		this->services_ready = true;
	}
	const auto &target_services = GetPassengerServices(target);
	const uint64_t demand = this->GetDemand(cargo);
	const uint64_t waiting = this->GetWaiting(cargo, target);
	if (this->prediction_budget == 0) return it->second;
	PassengerJourney best_alternative;
	bool evaluated_connection = false;
	for (OrderListID id : GetPassengerServices(this->station)) {
		if (!ConsumePassengerWork(this->prediction_budget)) return it->second;
		/* IDs are indexed in pool order. Reject unrelated routes before inspecting their fleet. */
		if (!std::binary_search(target_services.begin(), target_services.end(), id)) continue;
		const OrderList *list = OrderList::GetIfValid(id);
		if (list == nullptr) continue;
		auto [service, new_service] = this->services.try_emplace(std::pair(id, cargo));
		if (new_service) {
			if (!ConsumePassengerWork(this->prediction_budget, list->GetNumOrders())) return it->second;
			for (VehicleOrderID index = 0; index < list->GetNumOrders(); ++index) {
				const Order *order = list->GetOrderAt(index);
				service->second.may_refit |= order->IsRefit();
				if (IsPassengerStop(*order) && order->GetDestination() == this->station && order->GetCargoLoadType(cargo) != OrderLoadType::NoLoad) service->second.boarding_orders.push_back(index);
			}
		}
		std::vector<VehicleOrderID> boarding_orders;
		/* Validate the loading contract once per shared order list, not once per vehicle.
		 * Only the next cargo stop can be a connection; dynamic branches remain relevant/unknown. */
		for (VehicleOrderID index : service->second.boarding_orders) {
			PassengerRouteState route{ list->GetOrderVector(), {}, index, 0, 0, true, false, list->GetNumVehicles() };
			if (PredictPassengerJourneyImpl(route, index, target, cargo, _state_ticks, false, this->prediction_budget).reachable) boarding_orders.push_back(index);
			if (this->prediction_budget == 0) return it->second;
		}
		if (boarding_orders.empty()) continue;
		for (const Vehicle *v = list->GetFirstSharedVehicle(); v != nullptr; v = v->NextShared()) {
			if (!ConsumePassengerWork(this->prediction_budget)) return it->second;
			if (v == this->vehicle || !IsPassengerServiceVehicle(v)) continue;
			if (v->vehstatus.Any({VehState::Stopped, VehState::Crashed})) continue;
			const Capacity &capacity = this->GetCapacity(v, cargo);
			if (this->prediction_budget == 0) return it->second;
			if (capacity.total == 0) {
				/* A refit can create passenger capacity before a future boarding visit. */
				if (service->second.may_refit) return it->second;
				continue;
			}
			evaluated_connection = true;
			auto state = this->GetState(v);
			for (VehicleOrderID index : boarding_orders) {
				if (!state) {
					return it->second;
				}
				const bool full_here = state->loading && state->current_order == index && capacity.free == 0;
				PassengerJourney journey = PredictPassengerConnection(*state, index, target, cargo, _state_ticks,
						earliest_boarding, arrival_limit, this->prediction_budget, full_here);
				if (journey.reachable && (!journey.boarding_now || capacity.free < demand || capacity.free - demand < waiting)) {
					/* Future loading/unloading and competition cannot be assigned to a service.
					 * Present free seats must cover the whole arriving consist, so a cached
					 * decision for many packets/coaches cannot promise the same few seats twice. */
					journey.departure.reset();
					journey.arrival.reset();
				}
				if (this->prediction_budget == 0) return it->second;
				if (!journey.reachable) continue;
				if (!journey.departure || !journey.arrival) return it->second;
				if (!best_alternative.arrival || *journey.arrival < *best_alternative.arrival) best_alternative = journey;
			}
		}
	}
	/* With no active connection at all, keep CargoDist's route-change behaviour. Complete
	 * exclusion of observed connections (missed/full/slower visits) is a positive comparison. */
	it->second = ComparePassengerJourneys(stay, std::span(&best_alternative, 1), _state_ticks, penalty, evaluated_connection);
	return it->second;
}
