/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file passenger_service_graph.h Read-only, bounded multimodal passenger service snapshot. */

#ifndef PASSENGER_SERVICE_GRAPH_H
#define PASSENGER_SERVICE_GRAPH_H

#include "passenger_routing.h"
#include "passenger_aircraft_cost.h"
#include "direction_type.h"
#include "tile_type.h"
#include "passenger_service_index.h"

/** Limits include discovery, consist traversal, timetable traversal and dispatch slot searches. */
struct PassengerServiceGraphLimits {
	size_t work = 1048576;
	size_t services = 65536;
	size_t trips = 65536; ///< One Trip per physical primary vehicle, never synthetic fleet headways.
	size_t total_visits = 262144;
	size_t visits_per_trip = 128;
	size_t steps_per_trip = 4096;
	Ticks horizon = 65536; ///< State ticks, independent of GUI time/date settings; hard cap is 2^24.
	size_t memory_bytes = 1024 * 1024 * 1024; ///< Deterministic allocation envelope, independent of traversal work.
};

enum class PassengerGraphFailure : uint8_t { None, Work, Memory, Services, Trips, TotalVisits, Index, Opaque, End };
enum class PassengerGraphStage : uint8_t { Discovery, Structure, Orders, Schedules, Pattern, Trips, Prediction, Indices };

/** Elapsed time only; these counters never enter simulation admission or routing. */
enum class PassengerGraphBuildPhase : uint8_t { Discovery, Capacity, Selection, Orders, Pattern, Prediction, Departures, End };
using PassengerGraphPhaseTimes = std::array<uint64_t, static_cast<size_t>(PassengerGraphBuildPhase::End)>;
class PassengerServiceCapture;

struct PassengerGraphFailureDetail {
	PassengerGraphFailure reason = PassengerGraphFailure::None;
	PassengerGraphStage stage = PassengerGraphStage::Discovery;
	size_t work = 0, memory = 0, trips = 0, visits = 0;
};

struct PassengerTripPredictionDetail {
	PassengerPredictionStop reason = PassengerPredictionStop::None;
	VehicleID vehicle = VehicleID::Invalid();
	size_t work = 0, memory = 0, trips = 0, visits = 0;
	StateTicks stopped_at{}, complete_until{}, required_until{};
	VehicleOrderID stopped_order = INVALID_VEH_ORDER_ID, uncertain_order = INVALID_VEH_ORDER_ID;
	PassengerPredictionStop uncertain_reason = PassengerPredictionStop::None;
	TileIndex tile{};
	VehicleOrderID current_order = INVALID_VEH_ORDER_ID;
};

struct PassengerServiceGraphDiagnostic {
	PassengerGraphFailureDetail first_failure;
	PassengerTripPredictionDetail first_prediction_failure;
	OrderListID opaque_service = OrderListID::Invalid();
	VehicleID opaque_vehicle = VehicleID::Invalid();
	std::array<size_t, static_cast<size_t>(PassengerPredictionStop::End)> prediction_stops{};
	size_t eligible_services = 0, selected_services = 0, selected_trips = 0, largest_block_services = 0;
	size_t admitted_visits = 0; ///< Logical reservations, made immediately before generating each visit.
	Ticks maximum_unpredicted_ticks = 0;
	size_t trains = 0;
	size_t cargo_capable = 0;
	size_t possible_refit = 0;
	size_t predicted = 0;
	size_t prediction_executions = 0, prediction_cache_hits = 0, prediction_work = 0, prediction_executed_visits = 0;
	uint64_t prediction_nanoseconds = 0; ///< Physical predictor time only; diagnostics never affect admission.
	size_t prediction_cache_bytes = 0; ///< Conservative retained envelope, independently bounded.
	size_t visits = 0;
	size_t excluded_cargo = 0;
	size_t excluded_scope = 0;
	size_t inactive = 0;
	size_t omitted_trains = 0; ///< Lower bound if discovery itself exhausted its allowance.
	size_t truncated_predictions = 0; ///< Missing visit counts are unknown without doing the omitted prediction work.
	size_t allocation_bytes = 0; ///< Retained sizeof/capacity/map-node estimate, never used for admission.
	size_t static_allocation_bytes = 0; ///< Shared structural index, including capacity observations.
	size_t reused_work = 0; ///< Logical preprocessing allowance avoided by a resident index, diagnostic only.
	uint64_t build_microseconds = 0; ///< Development diagnostics only.
	PassengerGraphPhaseTimes phase_microseconds{};
	size_t capture_service_hits = 0, capture_reused_work = 0, capture_allocation_bytes = 0;
	bool capture_discovery_hit = false;
	bool static_cache_hit = false;
	size_t timing_uncertainty = 0, path_uncertainty = 0;
};

/**
 * Immutable after construction. All IDs, flags, durations and schedules are owned values.
 * Services describe declared order patterns; only Trip predictions certify future visit occurrences.
 * Build checks capacity before copying an order list, then captures its eligible physical fleet
 * and runs bounded predictions per vehicle. Freight-only lists skip pattern construction entirely.
 * With O orders, V consist parts, D dispatch slots inspected and P predicted order steps, build work
 * is O(O + V + D + P), plus ordered index insertion/lookup costs. Storage is bounded by copied
 * orders/schedules and total visits. Queries inspect only the station's indexed services/visits,
 * plus the selected trips' downstream occurrences, with their own shared work allowance.
 * The optional event capture owns IDs, orders and patterns. No live pointers, random sampling,
 * NewGRF callbacks or LinkGraph workers are retained in a snapshot.
 */
class PassengerServiceGraph {
public:
	struct Stop {
		StationID station;
		VehicleOrderID order; ///< Physical occurrence, retained even without exchange; VisitIDs resolve through this order.
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

	struct ServicePattern {
		OrderListID id;
		std::vector<Order> orders;
		std::vector<Stop> stops; ///< Declared stopping orders, in order-list order; pass-through orders omitted.
		std::vector<Segment> segments; ///< Only the path prefix verified by the common predictor.
		PassengerPredictionStatus pattern_status = PassengerPredictionStatus::Unknown; ///< Complete means one nominal cycle was captured.
		bool cycle_known = false; ///< True if traversal actually returned to its initial stop.
		std::vector<VehicleOrderID> reverse_waypoints; ///< Explicit reversals; out-and-back station visits retain distinct order IDs.
		std::vector<DispatchSchedule> schedules; ///< Timing/slot data only, without GUI names.
	};
	struct Service {
		OrderListID id;
		std::shared_ptr<const ServicePattern> pattern; ///< Shared owned values, independent of game pools and the event owner.
		std::span<const Stop> stops;
		std::span<const Segment> segments;
		PassengerPredictionStatus pattern_status = PassengerPredictionStatus::Unknown;
		bool cycle_known = false;
		std::span<const VehicleOrderID> reverse_waypoints;
		std::span<const DispatchSchedule> schedules;
		std::vector<size_t> trips; ///< Indices into GetTrips().
		std::optional<StateTicks> uncertain_from; ///< Earliest uncertified event across the fleet; never the nominal prediction cursor.
		PassengerPredictionStatus coverage_status = PassengerPredictionStatus::Complete; ///< Fleet coverage, computed once rather than once per station.
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
		VehicleType type = VehicleType::Invalid; ///< Diagnostics and connection evaluation; traversal uses the common visit contract.
		PassengerPredictionReason prediction_reason = PassengerPredictionReason::None;
		size_t prediction_work = 0; ///< Logical predictor debit, identical on a hit.
		uint64_t prediction_nanoseconds = 0; ///< Zero on a hit; live validation is in the build phase time.
		bool prediction_cache_hit = false; ///< Per-vehicle/cargo/start observation; aggregate across OD builds to count duplicates.
		bool prediction_executed = false;
		std::vector<int64_t> aircraft_adjustments; ///< Arrival prefix; empty for default costs and other modes.
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
	static PassengerServiceGraph Build(CargoType cargo, StateTicks now, PassengerServiceGraphLimits limits = {},
			std::optional<PassengerServiceScope> scope = {}, PassengerServiceCapture *capture = nullptr,
			PassengerTimingPolicy timing = PassengerTimingPolicy::Conservative);
	/** Running, passenger-capable aircraft in the captured corridor, including departures beyond this horizon. */
	bool HasAircraftDiscount() const { return this->has_aircraft_discount; }
	int64_t AircraftAdjustment(VisitID from, VisitID to) const
	{
		const auto &prefix = this->trips[from.trip].aircraft_adjustments;
		return prefix.empty() ? 0 : prefix[to.visit] - prefix[from.visit];
	}
	CargoType GetCargo() const { return this->cargo; }
	StateTicks GetStart() const { return this->start; }
	StateTicks GetEnd() const { return this->end; }
	/** Completeness of fleet discovery/capture. Prediction completeness is also reported on each Trip and query. */
	PassengerPredictionStatus GetStatus() const;
	/** Capture completeness for this OD. Complete inventories unknown alternatives too;
	 * it does not certify their timings or a winning journey. The planner ranks validated timed routes separately. */
	PassengerPredictionStatus GetQueryStatus(StationID origin, StationID destination, bool weighted_waiting = false) const;
	enum class Coverage : uint8_t { GlobalComplete, ScopedComplete, Incomplete };
	Coverage GetCoverage(StationID origin, StationID destination) const;
	size_t GetWork() const { return this->work; }
	size_t GetMemoryBound() const { return this->memory_bound; }
	const PassengerServiceGraphDiagnostic &GetDiagnostic() const { return this->diagnostic; }
	/** Unique currently loading occurrence, never a later circuit through the same station. */
	std::optional<VisitID> FindOnboardVisit(VehicleID vehicle, StationID station, VehicleOrderID order) const;
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
	Departures GetDepartures(StationID station, StateTicks from, StateTicks to, bool scoped_query = false) const;
	/** Capture-order occurrences for reverse reachability; flags/times must still be checked. */
	std::span<const VisitID> GetStationVisits(StationID station) const;
	/** Declared stations whose services have incomplete prediction coverage, including missing visits. */
	std::span<const StationID> GetUncertainStations() const { return this->uncertain_stations; }
	PassengerPredictionStatus GetUncertainStatus() const { return this->uncertain_status; }
	/** Conservative declared-service component: disconnected uncertain lines cannot affect this destination. */
	std::span<const StationID> GetUncertainStations(StationID destination) const;
	PassengerPredictionStatus GetUncertainStatus(StationID destination) const;
	const Stop &GetVisitStop(VisitID id) const { return this->GetStop(this->trips[id.trip], this->trips[id.trip].prediction.visits[id.visit].order); }

private:
	friend struct PassengerServiceGraphTestAccess;
	PassengerAircraftCost aircraft_cost;
	bool has_aircraft_discount = false;
	CargoType cargo = INVALID_CARGO;
	StateTicks start{};
	StateTicks end{};
	PassengerPredictionStatus status = PassengerPredictionStatus::Complete;
	size_t work = 0;
	size_t memory_bound = 0;
	std::optional<PassengerServiceScope> scope;
	bool scoped_omissions = false;
	PassengerServiceGraphDiagnostic diagnostic;
	std::vector<Service> services;
	std::vector<Trip> trips;
	std::map<OrderListID, size_t> service_index;
	std::map<StationID, std::vector<OrderListID>> station_services;
	struct IndexRange {
		size_t begin = 0;
		size_t count = 0;
	};
	struct StationIndex {
		StationID station;
		IndexRange visits, timed, unknown;
		size_t component = 0; ///< Dense union root during construction; uncertain-component row or SIZE_MAX afterwards.
		PassengerPredictionStatus status = PassengerPredictionStatus::Complete;
	};
	std::vector<StationIndex> station_index; ///< Sorted occupied IDs only; no allocation proportional to the largest StationID.
	std::vector<VisitID> station_visits; ///< Contiguous station slices, each in capture order.
	std::vector<VisitID> timed_departures, unknown_departures;
	std::vector<std::pair<VehicleID, std::optional<VisitID>>> loading_visits; ///< Sorted by vehicle; duplicates remain ambiguous.
	std::vector<StationID> uncertain_stations;
	PassengerPredictionStatus uncertain_status = PassengerPredictionStatus::Complete;
	struct UncertainComponent {
		IndexRange stations;
		PassengerPredictionStatus status = PassengerPredictionStatus::Complete;
	};
	std::vector<UncertainComponent> uncertain_components; ///< Only components containing uncertain stations.
	std::vector<StationID> component_stations;
	/** Optional observer for the allocation/timing benchmark; absent in normal builds. */
	enum class IndexStage { Visits, Departures, Sort, Loading, Components, Uncertain, End };
	using IndexObserver = void (*)(IndexStage, bool);
	void BuildDepartureIndex(bool departures = true, IndexObserver observer = nullptr);
	const StationIndex *FindStationIndex(StationID station) const;
	const Stop &GetStop(const Trip &trip, VehicleOrderID order) const;
	QueryResult<VisitID> SelectVisits(StationID station, StateTicks from, StateTicks to, size_t &budget) const;
};

/** One synchronous arrival or waiting-queue routing pass, owned by ActiveContext.
 * Vehicle/order pools, consists, refits and dispatch settings must be stable throughout
 * that pass (as they are in BeginLoading and RoutePassengerJourneys). Never share across
 * events, even within one tick. Clear before any caller-induced game-state mutation.
 * Only IDs and owned values are retained. Trip progress/facilities are always read live.
 * Predictions use exactly matching owned/live inputs, period and traversal limits.
 * Resource-stopped results are never resident; hits replay work and visit admissions.
 * Storage is optional and bounded; admission always pays the original logical envelope. */
class PassengerServiceCapture {
	friend class PassengerServiceGraph;
	struct Data;
	std::unique_ptr<Data> data;
	size_t maximum_bytes;
	size_t prediction_maximum_bytes;
public:
	static constexpr size_t MAXIMUM_BYTES = 256 * 1024 * 1024;
	static constexpr size_t MAXIMUM_PREDICTION_BYTES = 128 * 1024 * 1024;
	/** The second allowance is part of the first. Zero disables only Trip reuse for paired measurements. */
	explicit PassengerServiceCapture(size_t maximum_bytes = MAXIMUM_BYTES, size_t prediction_maximum_bytes = MAXIMUM_PREDICTION_BYTES);
	~PassengerServiceCapture();
	void Clear();
	size_t GetAllocationBytes() const;
	size_t GetPredictionBytes() const;
};

#endif /* PASSENGER_SERVICE_GRAPH_H */
