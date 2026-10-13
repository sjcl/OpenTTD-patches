/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_journey_active.h Simulation-only execution of passenger journeys. */
#ifndef PASSENGER_JOURNEY_ACTIVE_H
#define PASSENGER_JOURNEY_ACTIVE_H

#include "cargopacket.h"
#include "passenger_journey_planner.h"

bool IsPassengerJourneyRoutingEnabled(CargoType cargo);
uint64_t PassengerJourneyServiceSignature(const OrderList &orders, CargoType cargo, size_t *budget = nullptr);
void PassengerJourneySettingsChanged(int32_t);
class PassengerJourneyLoadingContext;
void RouteWaitingPassengerJourneys(Station *station, PassengerJourneyLoadingContext *loading = nullptr);
void AfterLoadPassengerJourneys();
size_t InvalidatePassengerJourneyVehicle(VehicleID vehicle);
void RebuildPassengerJourneyVehicleIndex();
void ClearPassengerOdFallbackCache(); ///< Derived only; cold/warm admission and decisions are identical.

enum class PassengerJourneyFallbackReason : uint8_t {
	GraphWork, GraphMemory, SnapshotLimit, QueryWork, QueryLimit, QueryMemory, PredictionIncomplete, PredictionHorizon, NoJourney, InvalidLeg,
	StaleLeg, Disabled, VehicleDeleted, End,
};

struct PassengerJourneyQueryDetail {
	StationID origin = StationID::Invalid(), destination = StationID::Invalid();
	StateTicks at{};
	PassengerPredictionStatus status{};
	PassengerJourneyStopReason stop{};
	size_t candidates = 0;
	std::optional<StateTicksDelta> score;
	std::optional<StateTicks> unknown_arrival_bound;
	OrderListID unknown_service = OrderListID::Invalid();
};

struct PassengerJourneyTickDiagnostic {
	std::array<size_t, static_cast<size_t>(PassengerGraphFailure::End)> graph_failures{};
	std::array<size_t, static_cast<size_t>(PassengerPredictionStop::End)> prediction_stops{};
	PassengerGraphFailureDetail first_graph_failure;
	PassengerTripPredictionDetail first_prediction_failure;
	OrderListID opaque_service = OrderListID::Invalid();
	VehicleID opaque_vehicle = VehicleID::Invalid();
	size_t complete_graphs = 0, admitted_visits = 0, eligible_services = 0, selected_services = 0, selected_trips = 0, largest_block_services = 0;
	Ticks maximum_unpredicted_ticks = 0;
	size_t builds = 0, static_cache_hits = 0, queries = 0, query_cache_hits = 0;
	size_t trains = 0, cargo_capable = 0, possible_refit = 0, predicted = 0, visits = 0;
	size_t excluded_cargo = 0, excluded_scope = 0, inactive = 0, omitted_trains = 0, truncated_predictions = 0;
	size_t graph_work = 0, query_work = 0, maximum_graph_work = 0;
	size_t reused_graph_work = 0, static_allocation_bytes = 0, query_scratch_bytes = 0, query_cache_bytes = 0;
	size_t journeys = 0, passengers = 0, graph_memory_bound = 0, graph_allocation_bytes = 0;
	uint64_t build_microseconds = 0, maximum_build_microseconds = 0;
	PassengerGraphPhaseTimes graph_phase_microseconds{};
	size_t capture_discovery_hits = 0, capture_service_hits = 0, capture_reused_work = 0, capture_allocation_bytes = 0;
	size_t prediction_executions = 0, prediction_cache_hits = 0, prediction_work = 0, prediction_executed_visits = 0, prediction_cache_bytes = 0;
	uint64_t prediction_nanoseconds = 0;
	uint64_t query_microseconds = 0, maximum_tick_microseconds = 0;
	std::array<size_t, static_cast<size_t>(PassengerJourneyFallbackReason::End)> fallbacks{};
	std::array<size_t, static_cast<size_t>(PassengerJourneyFallbackReason::End)> deferred{};
	size_t fair_share_work = 0, fair_share_claims = 0;
	size_t fair_share_releases = 0, retained_rides = 0;
	size_t od_fallback_work = 0, od_fallback_searches = 0;
	size_t od_fallback_cache_hits = 0, od_fallback_cache_bytes = 0;
	size_t boarded_passengers = 0, transferred_passengers = 0, delivered_passengers = 0;
	size_t depot_service_boarded_passengers = 0, depot_service_delivered_passengers = 0;
	size_t depot_crossing_boarded_passengers = 0, depot_crossing_delivered_passengers = 0; ///< Actual timed moves on a leg whose order suffix includes a depot.
	size_t timing_uncertainty = 0, path_uncertainty = 0, uncertainty_resolved = 0;
	size_t timed_routes_ignoring_unknown = 0, timed_budget_deferred = 0;
	size_t untimed_routes = 0, untimed_no_route = 0, untimed_budget_deferred = 0;
	size_t untimed_boarded_passengers = 0, untimed_transferred_passengers = 0, untimed_delivered_passengers = 0;
	std::array<PassengerJourneyQueryDetail, 16> query_samples{};
	size_t query_sample_count = 0;
	uint64_t simulation_ticks = 0, simulation_microseconds = 0, maximum_simulation_microseconds = 0;
	uint64_t journey_microseconds = 0, maximum_journey_microseconds = 0;
	uint64_t waiting_microseconds = 0, fallback_distance_microseconds = 0, boarding_microseconds = 0; ///< Inclusive diagnostic scopes; do not sum them.
	std::array<uint64_t, 2048> simulation_histogram{}, journey_histogram{}; ///< 1 ms buckets; last bucket includes overflow.
};

/** Nested observations count elapsed time only at the outermost Journey call. */
class PassengerJourneyWorkTimer {
	uint64_t started;
public:
	PassengerJourneyWorkTimer();
	~PassengerJourneyWorkTimer();
};

/** Reset at the simulation tick boundary, including the first tick after network join/load.
 * Budget counters are simulation state; cache residency and elapsed diagnostics never affect them. */
void BeginPassengerJourneyTick();
void PreparePassengerJourneyLoading(); ///< Before any station countdowns/queues change this tick.
void ReleasePassengerJourneyWaitingShare(StationID station); ///< End of this station's waiting pass, including skipped passes.
const PassengerJourneyTickDiagnostic &GetPassengerJourneyTickDiagnostic();
PassengerJourneyTickDiagnostic GetPassengerJourneyTotalDiagnostic();
void ResetPassengerJourneyDiagnostic(); ///< Local observations only; never resets simulation allowances.
void RecordPassengerJourneyFallback(PassengerJourneyFallbackReason reason); ///< Simulation-only lifecycle observations.
enum class PassengerJourneyMovement : uint8_t { Board, Transfer, Deliver };
void RecordPassengerJourneyMovement(PassengerJourneyMovement movement, uint count, bool timed = true); ///< Actual cargo moves only, never reservations/staging.
void RecordPassengerJourneyMovement(PassengerJourneyMovement movement, const CargoPacket &packet, uint count);
void RecordPassengerJourneySimulationTick(uint64_t microseconds); ///< Read-only elapsed CPU-thread time, outside all routing budgets.
PassengerServiceGraphLimits ActivePassengerServiceGraphLimits();

/** Lazy prediction for one physical passenger service/cargo in one loading pass. */
class PassengerJourneyBoardingContext {
	const Vehicle *vehicle;
	CargoType cargo;
	mutable bool initialized = false;
	mutable uint64_t signature = 0;
	mutable PassengerVisitPrediction prediction;
	mutable size_t predictions = 0;
	mutable std::map<std::tuple<OrderListID, uint64_t, VehicleOrderID, VehicleOrderID, StationID, StationID, bool>, bool> service_rides;
	void Initialize() const;
	bool ValidateServiceRide(const PassengerJourneyExecution &ride, bool boarding) const;
public:
	explicit PassengerJourneyBoardingContext(const Vehicle *vehicle, CargoType cargo = INVALID_CARGO);
	bool CanBoard(const CargoPacket &packet) const;
	bool CanContinue(const CargoPacket &packet) const;
	size_t GetPredictionCount() const { return this->predictions; }
};

/** Untimed OD fallback, local to one arrival/loading pass. LinkGraph paths use
 * unrestricted directed links; boarding also requires a live order prefix.
 * No timing, optimum, future capacity or permanent unreachability is asserted. */
class PassengerOdFallbackContext {
	using BindingNodes = std::vector<std::pair<StationID, bool>>;
	struct Binding { std::shared_ptr<const BindingNodes> nodes; uint64_t token; };
	std::map<std::pair<LinkGraphID, uint64_t>, Binding> bindings;
	struct Distances {
		LinkGraphID graph = LinkGraphID::Invalid();
		std::shared_ptr<const std::vector<uint32_t>> hops;
	};
	std::map<std::pair<CargoType, StationID>, Distances> distances;
	std::map<std::pair<VehicleID, CargoType>, std::vector<StationID>> stops;
	std::map<std::tuple<VehicleID, CargoType, StationID>, StationID> routes;
	size_t distance_bytes = 0;
	size_t work_left = 128 * 1024; ///< Includes live state/facility capture and the 4096-order prefix.
	bool resource_limited = false;
	bool Spend(size_t amount = 1);
	bool CanReach(const Vehicle *vehicle, CargoType cargo, StationID target);
	uint32_t Distance(CargoType cargo, StationID origin, StationID destination);
	StationID ProgressingVia(const Vehicle *vehicle, CargoType cargo, StationID destination);
	StationID BoardingVia(const Vehicle *vehicle, CargoType cargo, StationID destination);
public:
	bool CanBoard(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo);
	void PrepareBoard(CargoPacket &packet, const Vehicle *vehicle, CargoType cargo);
	void Invalidate(const Vehicle *vehicle);
	std::optional<VehicleCargoList::MoveToAction> Decide(CargoPacket &packet, const Vehicle *vehicle,
			CargoType cargo, StationID station, bool accepted, OrderUnloadType unload);
};

/** Owned by LoadUnloadStation, shared by validation, refit queries, reservations and all coaches.
 * Refit invalidates the affected consist; nothing is retained across simulation events. */
class PassengerJourneyLoadingContext {
	std::map<std::pair<VehicleID, CargoType>, PassengerJourneyBoardingContext> boarding;
	PassengerOdFallbackContext fallback;
public:
	bool CanBoard(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo = INVALID_CARGO);
	void PrepareBoard(CargoPacket &packet, const Vehicle *vehicle);
	void Invalidate(const Vehicle *vehicle);
	size_t GetPredictionCount() const;
};

/** Shares snapshots/query caches across all coaches and packets at an arrival. */
class PassengerJourneyActiveContext {
	struct Snapshot;
	PassengerServiceCapture capture;
	const Vehicle *vehicle;
	StationID station;
	std::map<std::tuple<CargoType, StationID, Ticks>, std::unique_ptr<Snapshot>> snapshots;
	std::map<CargoType, PassengerJourneyBoardingContext> continuation;
	struct PeriodicWaiting { std::optional<PassengerJourneyExecution> ride; bool deferred; };
	std::map<std::pair<CargoType, StationID>, PeriodicWaiting> periodic_waiting;
	PassengerOdFallbackContext fallback;
	size_t work_left = 4194304;
	size_t memory_left = 256 * 1024 * 1024;
	size_t queries_left = 128;
	PassengerJourneyFallbackReason failure = PassengerJourneyFallbackReason::GraphWork;
	bool deferred = false;
	Snapshot *GetSnapshot(CargoType cargo, StationID destination, Ticks horizon = 4096);
	std::optional<PassengerJourneyExecution> Execute(Snapshot &snapshot, CargoType cargo, const PassengerJourneyLeg &leg);
	bool AdmitQuery();
public:
	/** A zero prediction cache allowance supports paired diagnostics; logical allowances stay identical. */
	PassengerJourneyActiveContext(const Vehicle *vehicle, StationID station, size_t prediction_cache_bytes = PassengerServiceCapture::MAXIMUM_PREDICTION_BYTES);
	~PassengerJourneyActiveContext();
	size_t GetWorkAllowanceUsed() const { return 4194304 - this->work_left; }
	bool PlanWaiting(CargoPacket &packet, CargoType cargo, StationID legacy_hop);
	std::optional<VehicleCargoList::MoveToAction> Decide(CargoPacket &packet, CargoType cargo, bool accepted, OrderUnloadType unload);
};

/** Available waiting counts only. Reservations are reported separately by StationCargoList. */
std::array<uint, 3> CountPassengerRoutingModes(const StationCargoList &cargo);
std::array<uint, 3> CountReservedPassengerRoutingModes(const Station &station, CargoType cargo);

#endif /* PASSENGER_JOURNEY_ACTIVE_H */
