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
#include <functional>

/** Physical station events remain in timing/occurrence vectors even without cargo exchange. */
inline bool IsPassengerStationStop(const Order &order)
{
	return order.IsType(OT_GOTO_STATION) && !(order.GetNonStopType() & ONSF_NO_STOP_AT_DESTINATION_STATION);
}

inline bool HasPassengerExchange(const Order &order, CargoType cargo)
{
	return IsPassengerStationStop(order) && (order.GetCargoLoadType(cargo) != OrderLoadType::NoLoad ||
			order.GetCargoUnloadType(cargo) != OrderUnloadType::NoUnload);
}

/** Depot entry keeps cargo unless it refits, stops or sells. Optional servicing,
 * nearest-depot selection and unbunching preserve the suffix but cannot promise its time. */
enum class PassengerDepotTraversal : uint8_t { Blocked, Untimed, Timed };
PassengerDepotTraversal ClassifyPassengerDepot(const Order &order);

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
	std::vector<uint8_t> usable_stops; ///< Owned facility checks, indexed by order. Empty for nominal/test traversal.
};

enum class PassengerPredictionReason : uint8_t { None, VehicleState, OrderProgress, StationFacilities, OrderOrTiming, Budget, End };

/** Physical services only; effects, rotors and articulated parts are never independent trips. */
bool IsPassengerServiceVehicle(const Vehicle *vehicle);
/** Use actual cargo compartments, not the engine's advertised/default cargo. Work is bounded. */
bool HasPassengerVehicleCapacity(const Vehicle *vehicle, CargoType cargo, size_t &budget);
/** Shared by forecasting and execution. Road compatibility includes all stops at this StationID. */
bool CanPassengerVehicleUseStation(const Vehicle *vehicle, StationID station, const Order *order, size_t &budget);

/** Validate the live order progress. The returned spans are only for immediate simulation-thread use. */
std::optional<PassengerRouteState> GetPassengerRouteState(const Vehicle *vehicle, size_t *budget = nullptr, PassengerPredictionReason *reason = nullptr, bool allow_implicit_current = false);

/** Why a bounded prediction ended. Unknown never certifies the unvisited suffix of a route. */
enum class PassengerPredictionStatus : uint8_t { Complete, Unknown, LimitReached, BudgetExhausted };

/** JourneyPlanner can assume declared timings without changing live vehicle control. */
enum class PassengerTimingPolicy : uint8_t { Conservative, AssumeTimetable };

/** Diagnostic only; never replaces status or the certified time prefix. */
enum class PassengerPredictionStop : uint8_t {
	None, Work, Memory, Visits, Steps, Timing, Conditional, Refit, Implicit, Unsupported, Dispatch, Facilities, VehicleState, OrderProgress, End,
};

/** One actual stop occurrence; vector position distinguishes subsequent circuits and repeated stations. */
struct PassengerPredictedVisit {
	VehicleOrderID order;
	std::optional<StateTicks> arrival;
	std::optional<StateTicks> departure;
	bool loading_now = false;
	std::optional<StateTicksDelta> travel_from_previous; ///< Recorded leg duration can be known even when absolute times are not.
	StateTicksDelta movement_from_previous{}; ///< Travel only; excludes waypoint/depot waits and station dwell.
	bool operator==(const PassengerPredictedVisit &) const = default;
};

struct PassengerVisitPrediction {
	std::vector<PassengerPredictedVisit> visits;
	PassengerPredictionStatus status = PassengerPredictionStatus::Unknown;
	StateTicks start{};
	StateTicks complete_until{}; ///< All arrival/departure occurrences in [start, complete_until) are captured, even if the suffix is incomplete.
	PassengerPredictionStop stop = PassengerPredictionStop::None;
	StateTicks stopped_at{}; ///< Prediction cursor, which can exceed complete_until when timing is unknown.
	VehicleOrderID stopped_order = INVALID_VEH_ORDER_ID; ///< Diagnostic only: cursor order at termination.
	VehicleOrderID uncertain_order = INVALID_VEH_ORDER_ID; ///< Diagnostic only: first unknown timing/path order.
	PassengerPredictionStop uncertain_reason = PassengerPredictionStop::None; ///< Diagnostic only.
	bool operator==(const PassengerVisitPrediction &) const = default;
	/** The exclusive endpoint also excludes unprocessed zero-time events at the truncation tick. */
	bool Covers(StateTicks from, StateTicks to) const { return this->start <= from && from < to && to <= this->complete_until; }
};

/** Same traversal/timing engine as journeys and connections, without the CargoDist boarding contract. */
PassengerVisitPrediction PredictPassengerVisits(const PassengerRouteState &state, CargoType cargo, StateTicks now,
		StateTicks end, size_t max_visits, size_t max_steps, size_t &budget, const std::function<bool()> &admit_visit = {}, bool prefix_only = false,
		PassengerTimingPolicy timing = PassengerTimingPolicy::Conservative);

/** A journey to a selected CargoDist via, on one vehicle and one boarding order. */
struct PassengerJourney {
	bool reachable = false;
	std::optional<StateTicks> departure;
	std::optional<StateTicks> arrival;
	bool boarding_now = false; ///< Boarding on the currently loading visit, before any intervening stops.
};

PassengerJourney PredictPassengerJourney(const PassengerRouteState &state, VehicleOrderID boarding_order, StationID target,
		CargoType cargo, StateTicks now, bool allow_intermediate, PassengerTimingPolicy timing = PassengerTimingPolicy::Conservative);

/** Find the first catchable visit before arrival_limit. Exhaustion/unsupported predictions remain unknown. */
PassengerJourney PredictPassengerConnection(const PassengerRouteState &state, VehicleOrderID boarding_order, StationID target,
		CargoType cargo, StateTicks now, StateTicks earliest_boarding, StateTicks arrival_limit, size_t &budget,
		bool skip_current_departure = false);

enum class PassengerTransferDecision : uint8_t { Unknown, Stay, Transfer };

/** alternatives_complete requires evaluating subsequent visits up to the stay arrival horizon. */
PassengerTransferDecision ComparePassengerJourneys(const PassengerJourney &stay, std::span<const PassengerJourney> alternatives,
		StateTicks now, Ticks transfer_penalty, bool alternatives_complete = false);

/** Invalidate the station-to-order-list index when order destinations change or pools are reset. */
void InvalidatePassengerServiceIndex();

/**
 * Cache for one PrepareUnload call. Shared by all compartments and packets of a vehicle.
 * Destruction invalidates all time estimates; no mutable state is read by LinkGraph jobs.
 */
class PassengerRoutingContext {
	struct Service {
		std::vector<VehicleOrderID> boarding_orders;
		bool may_refit = false;
	};
	struct Capacity {
		uint64_t total = 0;
		uint64_t free = 0; ///< Includes confirmed unloading here, and subtracts reservations.
	};
	const Vehicle *vehicle;
	StationID station;
	std::map<std::pair<CargoType, StationID>, PassengerTransferDecision> decisions;
	std::map<std::pair<CargoType, StationID>, PassengerJourney> journeys;
	std::map<std::pair<OrderListID, CargoType>, Service> services;
	std::map<const Vehicle *, std::optional<PassengerRouteState>> states;
	std::map<std::pair<const Vehicle *, CargoType>, Capacity> capacities;
	std::map<CargoType, uint64_t> demands;
	std::map<std::pair<CargoType, StationID>, uint64_t> waiting;
	bool services_ready = false;
	size_t prediction_budget = 32768; ///< Shared deterministic allowance, including discovery and dispatch slots.
	const std::optional<PassengerRouteState> &GetState(const Vehicle *v);
	const Capacity &GetCapacity(const Vehicle *v, CargoType cargo);
	uint64_t GetDemand(CargoType cargo);
	uint64_t GetWaiting(CargoType cargo, StationID target);

public:
	PassengerRoutingContext(const Vehicle *vehicle, StationID station) : vehicle(vehicle), station(station) {}
	bool IsEnabled(CargoType cargo) const;
	bool CanContinue(CargoType cargo, StationID target);
	PassengerTransferDecision Decide(CargoType cargo, StationID target);
};

#endif /* PASSENGER_ROUTING_H */
