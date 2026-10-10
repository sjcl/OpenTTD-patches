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
#include "settings_type.h"
#include "timetable.h"
#include "date_func.h"

#include "safeguards.h"

/** Only actual station stops can carry or exchange passengers. */
static bool IsPassengerStop(const Order &order)
{
	return order.IsType(OT_GOTO_STATION) && !(order.GetNonStopType() & ONSF_NO_STOP_AT_DESTINATION_STATION);
}

/**
 * Predict a journey without averaging local and express services. Timings are absolute state ticks.
 * Unknown timings remain unknown: distance and station-level LinkGraph times are never substituted.
 * A visit to the boarding station again ends the journey, including out-and-back order lists.
 */
PassengerJourney PredictPassengerJourney(const PassengerRouteState &state, VehicleOrderID boarding_order, StationID target,
		CargoType cargo, StateTicks now, bool allow_intermediate)
{
	PassengerJourney result;
	if (state.orders.empty() || state.current_order >= state.orders.size() || boarding_order >= state.orders.size()) return result;
	const Order &boarding = state.orders[boarding_order];
	if (!IsPassengerStop(boarding)) return result;
	if (!allow_intermediate && boarding.GetCargoLoadType(cargo) == OrderLoadType::NoLoad) return result;

	/* Unsupported orders may change the path. Report uncertainty if the target occurs anywhere,
	 * so a known slow service cannot conceal an unpredicted faster alternative. */
	auto uncertain = [&]() -> PassengerJourney {
		for (const Order &order : state.orders) {
			if (IsPassengerStop(order) && order.GetDestination() == target && order.GetCargoUnloadType(cargo) != OrderUnloadType::NoUnload) {
				return { true, std::nullopt, std::nullopt };
			}
		}
		return {};
	};

	StateTicks tick = now;
	int64_t lateness = state.lateness;
	bool timing_known = true;
	bool boarded = false;
	bool skip_travel = state.loading;
	VehicleOrderID index = state.current_order;
	/* At most one loop to board, and one loop to reach the via. Also bounds unconditional jumps. */
	for (size_t steps = 0; steps < state.orders.size() * 2; ++steps) {
		const Order &order = state.orders[index];
		if (order.IsType(OT_CONDITIONAL)) {
			if (order.GetConditionVariable() != OrderConditionVariable::Unconditionally) return uncertain();
			if (!order.IsWaitTimetabled()) timing_known = false;
			tick += order.GetWaitTime();
			index = order.GetConditionSkipToOrder();
			if (index >= state.orders.size()) return uncertain();
			skip_travel = true; // Branch time replaces the destination's travel time.
			continue;
		}
		/* Depot detours, implicit stops, refits and slot/counter operations are not predictable here. */
		if (order.IsType(OT_GOTO_DEPOT) || order.IsType(OT_IMPLICIT) || order.IsRefit() || order.IsSlotCounterOrder() || order.IsType(OT_DUMMY)) return uncertain();
		if (order.IsType(OT_LABEL)) {
			skip_travel = false;
			index = (index + 1) % state.orders.size();
			continue;
		}
		if (!order.IsType(OT_GOTO_STATION) && !order.IsType(OT_GOTO_WAYPOINT)) return uncertain();

		if (!skip_travel) {
			if (order.GetTravelTime() == 0 && !order.IsTravelTimetabled()) timing_known = false;
			int64_t travel = order.GetTravelTime();
			if (steps == 0) {
				/* The recorded journey has already elapsed, but the train has not arrived.
				 * Its remaining physical delay is unknown; don't invent an immediate arrival. */
				if (state.elapsed > travel) timing_known = false;
				travel = std::max<int64_t>(0, travel - state.elapsed);
			}
			tick += travel;
		}
		skip_travel = false;

		const bool stop = IsPassengerStop(order);
		if (boarded && stop) {
			if (order.GetDestination() == boarding.GetDestination()) return {};
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
						state.schedules[schedule_index].GetScheduledDispatchDuration() == 0 || state.schedules[schedule_index].GetScheduledDispatch().empty()) return uncertain();
			}
			const OrderLoadType load = order.GetCargoLoadType(cargo);
			if (IsFullLoadOrderLoadType(load) || order.GetLeaveType() != OLT_NORMAL) timing_known = false;
			if (order.GetLoadType() == OrderLoadType::CargoTypeLoad) {
				/* Full loading another cargo in this consist can also hold up the passenger coach. */
				for (CargoType other{}; other < NUM_CARGO; ++other) {
					if (IsFullLoadOrderLoadType(order.GetCargoLoadType(other))) timing_known = false;
				}
			}
			if (!order.IsWaitTimetabled()) timing_known = false;
			const bool future_dispatch = state.scheduled_dispatch && order.IsScheduledDispatchOrder(true) && !(steps == 0 && state.loading);
			int64_t wait = future_dispatch ? order.GetTimetabledWait() : std::max<int64_t>(0, static_cast<int64_t>(order.GetWaitTime()) - lateness);
			if (steps == 0 && state.loading) wait = std::max<int64_t>(0, wait - state.elapsed);
			tick += wait;
			lateness = std::max<int64_t>(0, lateness - order.GetWaitTime());

			if (future_dispatch) {
				const int schedule_index = order.GetDispatchScheduleIndex();
				if (schedule_index < 0 || static_cast<size_t>(schedule_index) >= state.schedules.size()) return uncertain();
				const DispatchSchedule &schedule = state.schedules[schedule_index];
				/* Multiple vehicles can compete for the same future slot. Until a slot is assigned,
				 * don't guess their booking order. Already assigned loading departures remain usable. */
				if (schedule.GetScheduledDispatchDuration() == 0 || (state.shared_vehicles > 1 && !schedule.GetScheduledDispatchReuseSlots())) return uncertain();
				const StateTicks slot = GetScheduledDispatchTime(schedule, tick).first;
				if (slot == INVALID_STATE_TICKS) return uncertain();
				tick = std::max(tick, slot);
				lateness = 0;
			}
		} else if (order.IsType(OT_GOTO_WAYPOINT)) {
			if (state.scheduled_dispatch && order.IsScheduledDispatchOrder(true)) return uncertain();
			if (order.GetWaitTime() > 0 && !order.IsWaitTimetabled()) timing_known = false;
			tick += order.GetWaitTime();
		}
		if (!boarded && index == boarding_order) {
			boarded = true;
			if (timing_known) result.departure = tick;
		}
		index = (index + 1) % state.orders.size();
	}
	return {};
}

/** Choose the earliest usable direct alternative, applying a minimum transfer time as well as a cost. */
PassengerTransferDecision ComparePassengerJourneys(const PassengerJourney &stay, std::span<const PassengerJourney> alternatives,
		StateTicks now, Ticks transfer_penalty)
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
	if (!best) return PassengerTransferDecision::Unknown;
	return *stay.arrival <= *best ? PassengerTransferDecision::Stay : PassengerTransferDecision::Transfer;
}

/* This index contains IDs only, and is independent of timetables, consists, load flags and dispatch.
 * Those are read afresh for each arrival. Order destination registration invalidates the index. */
static std::map<StationID, std::vector<OrderListID>> _passenger_service_index;
static bool _passenger_service_index_valid = false;

void InvalidatePassengerServiceIndex()
{
	if (!_passenger_service_index_valid) return;
	_passenger_service_index_valid = false;
	_passenger_service_index.clear();
}

static const std::vector<OrderListID> &GetPassengerServices(StationID station)
{
	if (!_passenger_service_index_valid) {
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
	return _passenger_service_index[station];
}

bool PassengerRoutingContext::IsEnabled(CargoType cargo) const
{
	return _settings_game.linkgraph.passenger_transfer_timetables && this->vehicle->type == VehicleType::Train &&
			IsCargoInClass(cargo, CargoClass::Passengers) && _settings_game.linkgraph.GetDistributionType(cargo) != DistributionType::Manual;
}

std::optional<PassengerRouteState> PassengerRoutingContext::GetState(const Vehicle *v) const
{
	if (v->orders == nullptr || v->GetNumOrders() == 0 || v->vehstatus.Any({VehState::Stopped, VehState::Crashed})) return std::nullopt;
	if (v->cur_implicit_order_index != v->cur_real_order_index || v->cur_real_order_index >= v->GetNumOrders()) return std::nullopt;
	if (v->cur_timetable_order_index != INVALID_VEH_ORDER_ID && v->cur_timetable_order_index != v->cur_real_order_index) return std::nullopt;
	const Order *order = v->GetOrder(v->cur_real_order_index);
	bool loading = v->current_order.IsAnyLoadingType();
	if (loading ? (!IsPassengerStop(*order) || order->GetDestination() != v->last_station_visited) :
			(v->current_order.GetType() != order->GetType() || v->current_order.GetDestination() != order->GetDestination())) return std::nullopt;
	return PassengerRouteState{ v->orders->GetOrderVector(), v->orders->GetScheduledDispatchScheduleSet(), v->cur_real_order_index,
			v->current_order_time, v->lateness_counter, loading, v->vehicle_flags.Test(VehicleFlag::ScheduledDispatch), v->orders->GetNumVehicles() };
}

bool PassengerRoutingContext::CanContinue(CargoType cargo, StationID target)
{
	auto key = std::pair(cargo, target);
	auto [it, inserted] = this->journeys.try_emplace(key);
	if (inserted) {
		auto state = this->GetState(this->vehicle);
		if (state) {
			const size_t work = state->orders.size() * 3; // Two traversal loops plus a possible uncertainty scan.
			if (work <= this->prediction_budget) {
				this->prediction_budget -= work;
				it->second = PredictPassengerJourney(*state, state->current_order, target, cargo, _state_ticks, true);
			}
		}
	}
	return it->second.reachable && it->second.arrival.has_value();
}

PassengerTransferDecision PassengerRoutingContext::Decide(CargoType cargo, StationID target)
{
	auto key = std::pair(cargo, target);
	auto [it, inserted] = this->decisions.try_emplace(key, PassengerTransferDecision::Unknown);
	if (!inserted || !this->CanContinue(cargo, target)) return it->second;
	std::vector<PassengerJourney> alternatives;
	for (OrderListID id : GetPassengerServices(this->station)) {
		const OrderList *list = OrderList::GetIfValid(id);
		if (list == nullptr) continue;
		if (list->GetNumOrders() > this->prediction_budget) return it->second;
		this->prediction_budget -= list->GetNumOrders();
		std::vector<VehicleOrderID> boarding_orders;
		for (VehicleOrderID index = 0; index < list->GetNumOrders(); ++index) {
			const Order *order = list->GetOrderAt(index);
			if (IsPassengerStop(*order) && order->GetDestination() == this->station && order->GetCargoLoadType(cargo) != OrderLoadType::NoLoad) boarding_orders.push_back(index);
		}
		if (boarding_orders.empty()) continue;
		for (const Vehicle *v = list->GetFirstSharedVehicle(); v != nullptr; v = v->NextShared()) {
			if (v == this->vehicle || !v->IsPrimaryVehicle()) continue;
			bool capacity = false;
			for (const Vehicle *u = v; u != nullptr; u = u->Next()) {
				if (this->prediction_budget == 0) return it->second;
				--this->prediction_budget;
				if (u->cargo_type == cargo && u->cargo_cap > 0) { capacity = true; break; }
			}
			if (!capacity || v->vehstatus.Any({VehState::Stopped, VehState::Crashed})) continue;
			auto state = this->GetState(v);
			for (VehicleOrderID index : boarding_orders) {
				const size_t work = list->GetNumOrders() * 3;
				if (work > this->prediction_budget) return it->second;
				this->prediction_budget -= work;
				if (state) {
					PassengerJourney journey = PredictPassengerJourney(*state, index, target, cargo, _state_ticks, false);
					if (v->type != VehicleType::Train && journey.reachable) {
						/* A connection using another transport mode may be faster. This feature only
						 * predicts trains, so preserve CargoDist's decision when such a connection exists. */
						journey.departure.reset();
						journey.arrival.reset();
					}
					alternatives.push_back(journey);
				} else {
					/* Check the route from this boarding order before declaring an unknown
					 * connection. An unrelated service with unknown phase must not block it. */
					PassengerRouteState route{ list->GetOrderVector(), list->GetScheduledDispatchScheduleSet(), index,
							0, 0, true, false, list->GetNumVehicles() };
					PassengerJourney journey = PredictPassengerJourney(route, index, target, cargo, _state_ticks, false);
					journey.departure.reset();
					journey.arrival.reset();
					alternatives.push_back(journey);
				}
			}
		}
	}
	it->second = ComparePassengerJourneys(this->journeys.at(key), alternatives, _state_ticks, _settings_game.linkgraph.passenger_transfer_penalty);
	return it->second;
}
