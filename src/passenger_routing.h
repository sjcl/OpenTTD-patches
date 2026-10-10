/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file passenger_routing.h Deterministic, timetable-based passenger transfer decisions. */

#ifndef PASSENGER_ROUTING_H
#define PASSENGER_ROUTING_H

#include "order_base.h"
#include <map>
#include <optional>

/** The simulation state needed to predict an individual service, independent of GUI settings. */
struct PassengerRouteState {
	std::span<const Order> orders;
	std::span<const DispatchSchedule> schedules;
	VehicleOrderID current_order;
	uint32_t elapsed;
	Ticks lateness;
	bool loading;
	bool scheduled_dispatch;
	uint shared_vehicles;
};

/** A journey to a selected CargoDist via, on one vehicle and one boarding order. */
struct PassengerJourney {
	bool reachable = false;
	std::optional<StateTicks> departure;
	std::optional<StateTicks> arrival;
};

PassengerJourney PredictPassengerJourney(const PassengerRouteState &state, VehicleOrderID boarding_order, StationID target,
		CargoType cargo, StateTicks now, bool allow_intermediate);

enum class PassengerTransferDecision : uint8_t { Unknown, Stay, Transfer };

PassengerTransferDecision ComparePassengerJourneys(const PassengerJourney &stay, std::span<const PassengerJourney> alternatives,
		StateTicks now, Ticks transfer_penalty);

/** Invalidate the station-to-order-list index when order destinations change or pools are reset. */
void InvalidatePassengerServiceIndex();

/**
 * Cache for one PrepareUnload call. Shared by all coaches and packets of a train.
 * Destruction invalidates all time estimates; no mutable state is read by LinkGraph jobs.
 */
class PassengerRoutingContext {
	const Vehicle *vehicle;
	StationID station;
	std::map<std::pair<CargoType, StationID>, PassengerTransferDecision> decisions;
	std::map<std::pair<CargoType, StationID>, PassengerJourney> journeys;
	size_t prediction_budget = 32768; ///< Bound worst-case order visits for all comparisons at this arrival.
	std::optional<PassengerRouteState> GetState(const Vehicle *v) const;

public:
	PassengerRoutingContext(const Vehicle *vehicle, StationID station) : vehicle(vehicle), station(station) {}
	bool IsEnabled(CargoType cargo) const;
	bool CanContinue(CargoType cargo, StationID target);
	PassengerTransferDecision Decide(CargoType cargo, StationID target);
};

#endif /* PASSENGER_ROUTING_H */
