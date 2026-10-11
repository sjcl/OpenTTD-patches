/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file passenger_service_graph.h Read-only, bounded train service snapshot for future timetable routers. */

#ifndef PASSENGER_SERVICE_GRAPH_H
#define PASSENGER_SERVICE_GRAPH_H

#include "passenger_routing.h"
#include "direction_type.h"
#include "tile_type.h"

/** Limits include discovery, consist traversal, timetable traversal and dispatch slot searches. */
struct PassengerServiceGraphLimits {
	size_t work = 1048576;
	size_t services = 65536;
	size_t trips = 65536; ///< One Trip per physical primary vehicle, never synthetic fleet headways.
	size_t total_visits = 262144;
	size_t visits_per_trip = 128;
	size_t steps_per_trip = 4096;
	Ticks horizon = 65536; ///< State ticks, independent of GUI time/date settings; hard cap is 2^24.
};

/**
 * Immutable after construction. All IDs, flags, durations and schedules are owned values.
 * Services describe declared order patterns; only Trip predictions certify future visit occurrences.
 * Build scans order lists once, their train fleets once, then runs bounded predictions per vehicle.
 * With O orders, V consist parts, D dispatch slots inspected and P predicted order steps, build work
 * is O(O + V + D + P), plus ordered index insertion/lookup costs. Storage is bounded by copied
 * orders/schedules and total visits. Queries inspect only the station's indexed services/visits,
 * plus the selected trips' downstream occurrences, with their own shared work allowance.
 * No live pointers, caches, random sampling, NewGRF callbacks or LinkGraph workers are involved.
 */
class PassengerServiceGraph {
public:
	struct Stop {
		StationID station;
		VehicleOrderID order; ///< Identifies a stop even if another order visits the same station.
		bool can_board;
		bool can_alight;
		bool forced_transfer; ///< Both forced unload and transfer end a through journey.
		std::optional<TimetableTicks> dwell; ///< Nominal timetable wait; full load/dispatch can make actual dwell unknown.
		int dispatch_schedule;
	};

	struct Segment {
		VehicleOrderID from;
		VehicleOrderID to;
		std::optional<StateTicksDelta> travel; ///< Includes intervening waypoint waits, excludes station dwell.
	};

	struct Service {
		OrderListID id;
		std::vector<Stop> stops; ///< Declared stopping orders, in order-list order; pass-through orders omitted.
		std::vector<Segment> segments; ///< Only the path prefix verified by the common predictor.
		PassengerPredictionStatus pattern_status = PassengerPredictionStatus::Unknown; ///< Complete means one nominal cycle was captured.
		bool cycle_known = false; ///< True if traversal actually returned to its initial stop.
		std::vector<VehicleOrderID> reverse_waypoints; ///< Explicit reversals; out-and-back station visits retain distinct order IDs.
		std::vector<DispatchSchedule> schedules; ///< Timing/slot data only, without GUI names.
		std::vector<size_t> trips; ///< Indices into GetTrips().
	};

	enum class Operation : uint8_t { Running, Stopped, Crashed, Unknown };
	struct Trip {
		VehicleID vehicle;
		OrderListID service;
		TileIndex tile;
		Direction direction;
		VehicleOrderID current_order;
		VehicleOrderID implicit_order;
		VehicleOrderID timetable_order;
		uint32_t elapsed;
		Ticks lateness;
		bool loading;
		bool scheduled_dispatch;
		uint64_t capacity; ///< Current total capacity for GetCargo(), not a promise of future free seats/refits.
		Operation operation;
		PassengerVisitPrediction prediction;
		std::optional<uint64_t> occupants; ///< Observed cargo count at capture, including passengers staged for unloading.
		std::optional<uint64_t> current_free; ///< Free at capture only; never a future seat guarantee.
	};

	struct VisitID {
		size_t trip; ///< Stable index within this snapshot; independent of any station/time index.
		size_t visit; ///< Occurrence within Trip::prediction.visits, including repeated circuits.
		bool operator==(const VisitID &) const = default;
	};

	struct BoardableTrip {
		VisitID boarding;
		std::vector<VisitID> reachable; ///< Proven downstream alighting occurrences, up to forced unloading.
		PassengerPredictionStatus status; ///< Completeness of the captured downstream suffix within the prediction horizon, independently of the station query window.
	};

	template <typename T> struct QueryResult {
		std::vector<T> items;
		PassengerPredictionStatus status = PassengerPredictionStatus::Complete;
	};

	/** Call on the simulation thread, while vehicle/order pools are stable. Queries need no game state. */
	static PassengerServiceGraph Build(CargoType cargo, StateTicks now, PassengerServiceGraphLimits limits = {});
	CargoType GetCargo() const { return this->cargo; }
	StateTicks GetStart() const { return this->start; }
	StateTicks GetEnd() const { return this->end; }
	/** Completeness of fleet discovery/capture. Prediction completeness is also reported on each Trip and query. */
	PassengerPredictionStatus GetStatus() const { return this->status; }
	std::span<const Service> GetServices() const { return this->services; }
	std::span<const Trip> GetTrips() const { return this->trips; }
	const Service *GetService(OrderListID id) const;
	/** Declared stops, with no timing/path guarantee. Check GetStatus() when discovery was truncated. */
	std::span<const OrderListID> GetServicesAtStation(StationID station) const;

	/**
	 * Half-open [from, to); unknown times remain candidates outside the certified prediction prefix.
	 * Outside the captured window returns Unknown. Status describes coverage of the requested window.
	 * Results retain capture order: ascending (VisitID::trip, VisitID::visit). Departure-time order is not guaranteed.
	 * Trip indices follow order-list pool order, then shared vehicle chain order. IDs remain stable
	 * for the snapshot's lifetime, allowing a future time-sorted index without changing occurrences.
	 */
	QueryResult<VisitID> GetVisits(StationID station, StateTicks from, StateTicks to, size_t budget = 32768) const;
	/**
	 * Same window and ordering contract as GetVisits; reachable occurrences are in visit order.
	 * Query status covers boarding candidates and query work; inspect each BoardableTrip::status
	 * for completeness beyond the window. Query work exhaustion always returns BudgetExhausted.
	 */
	QueryResult<BoardableTrip> GetBoardableTrips(StationID station, StateTicks from, StateTicks to, size_t budget = 32768) const;

	struct Departures {
		std::span<const VisitID> timed; ///< Known departures in [from, to), sorted by time, vehicle, then VisitID.
		std::span<const VisitID> unknown; ///< Untimed boardable occurrences; never certified as catchable.
		PassengerPredictionStatus status;
	};
	/** Allocation-free O(log stations + log departures) lookup; conservative coverage for the full snapshot horizon. */
	Departures GetDepartures(StationID station, StateTicks from, StateTicks to) const;
	/** Capture-order occurrences for reverse reachability; flags/times must still be checked. */
	std::span<const VisitID> GetStationVisits(StationID station) const;
	/** Declared stations whose services have incomplete prediction coverage, including missing visits. */
	std::span<const StationID> GetUncertainStations() const { return this->uncertain_stations; }
	PassengerPredictionStatus GetUncertainStatus() const { return this->uncertain_status; }
	const Stop &GetVisitStop(VisitID id) const { return this->GetStop(this->trips[id.trip], this->trips[id.trip].prediction.visits[id.visit].order); }

private:
	CargoType cargo = INVALID_CARGO;
	StateTicks start{};
	StateTicks end{};
	PassengerPredictionStatus status = PassengerPredictionStatus::Complete;
	std::vector<Service> services;
	std::vector<Trip> trips;
	std::map<OrderListID, size_t> service_index;
	std::map<StationID, std::vector<OrderListID>> station_services;
	std::map<StationID, std::vector<VisitID>> station_visits; ///< Capture-order index; VisitIDs can also back a future time index.
	struct DepartureIndex {
		std::vector<VisitID> timed;
		std::vector<VisitID> unknown;
		PassengerPredictionStatus status = PassengerPredictionStatus::Complete;
	};
	std::map<StationID, DepartureIndex> station_departures;
	std::vector<StationID> uncertain_stations;
	PassengerPredictionStatus uncertain_status = PassengerPredictionStatus::Complete;
	void BuildDepartureIndex();
	const Stop &GetStop(const Trip &trip, VehicleOrderID order) const;
	QueryResult<VisitID> SelectVisits(StationID station, StateTicks from, StateTicks to, size_t &budget) const;
};

#endif /* PASSENGER_SERVICE_GRAPH_H */
