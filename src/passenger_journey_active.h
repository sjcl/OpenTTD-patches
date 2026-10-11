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

enum class PassengerJourneyFallbackReason : uint8_t {
	GraphWork, GraphMemory, SnapshotLimit, QueryWork, QueryLimit, QueryMemory, PredictionIncomplete, PredictionHorizon, NoJourney, InvalidLeg,
	StaleLeg, Disabled, VehicleDeleted, End,
};

struct PassengerJourneyTickDiagnostic {
	size_t builds = 0, static_cache_hits = 0, queries = 0, query_cache_hits = 0;
	size_t trains = 0, cargo_capable = 0, possible_refit = 0, predicted = 0, visits = 0;
	size_t excluded_cargo = 0, excluded_scope = 0, inactive = 0, omitted_trains = 0, truncated_predictions = 0;
	size_t graph_work = 0, query_work = 0, maximum_graph_work = 0;
	size_t reused_graph_work = 0, static_allocation_bytes = 0, query_scratch_bytes = 0, query_cache_bytes = 0;
	size_t journeys = 0, passengers = 0, graph_memory_bound = 0, graph_allocation_bytes = 0;
	uint64_t build_microseconds = 0, maximum_build_microseconds = 0;
	uint64_t query_microseconds = 0, maximum_tick_microseconds = 0;
	std::array<size_t, static_cast<size_t>(PassengerJourneyFallbackReason::End)> fallbacks{};
	std::array<size_t, static_cast<size_t>(PassengerJourneyFallbackReason::End)> deferred{};
	size_t fair_share_work = 0, fair_share_claims = 0;
	size_t fair_share_releases = 0, retained_rides = 0;
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
PassengerServiceGraphLimits ActivePassengerServiceGraphLimits();

/** Lazy prediction for one physical passenger service/cargo in one loading pass. */
class PassengerJourneyBoardingContext {
	const Vehicle *vehicle;
	CargoType cargo;
	mutable bool initialized = false;
	mutable uint64_t signature = 0;
	mutable PassengerVisitPrediction prediction;
	mutable size_t predictions = 0;
	void Initialize() const;
public:
	explicit PassengerJourneyBoardingContext(const Vehicle *vehicle, CargoType cargo = INVALID_CARGO);
	bool CanBoard(const CargoPacket &packet) const;
	bool CanContinue(const CargoPacket &packet) const;
	size_t GetPredictionCount() const { return this->predictions; }
};

/** Owned by LoadUnloadStation, shared by validation, refit queries, reservations and all coaches.
 * Refit invalidates the affected consist; nothing is retained across simulation events. */
class PassengerJourneyLoadingContext {
	std::map<std::pair<VehicleID, CargoType>, PassengerJourneyBoardingContext> boarding;
public:
	bool CanBoard(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo = INVALID_CARGO);
	void Invalidate(const Vehicle *vehicle);
	size_t GetPredictionCount() const;
};

/** Shares snapshots/query caches across all coaches and packets at an arrival. */
class PassengerJourneyActiveContext {
	struct Snapshot;
	const Vehicle *vehicle;
	StationID station;
	std::map<std::pair<CargoType, StationID>, std::unique_ptr<Snapshot>> snapshots;
	std::map<CargoType, PassengerJourneyBoardingContext> continuation;
	size_t work_left = 4194304;
	size_t memory_left = 256 * 1024 * 1024;
	size_t queries_left = 128;
	PassengerJourneyFallbackReason failure = PassengerJourneyFallbackReason::GraphWork;
	bool deferred = false;
	Snapshot *GetSnapshot(CargoType cargo, StationID destination);
	std::optional<PassengerJourneyExecution> Execute(Snapshot &snapshot, CargoType cargo, const PassengerJourneyLeg &leg);
	bool AdmitQuery();
public:
	PassengerJourneyActiveContext(const Vehicle *vehicle, StationID station);
	~PassengerJourneyActiveContext();
	size_t GetWorkAllowanceUsed() const { return 4194304 - this->work_left; }
	bool PlanWaiting(CargoPacket &packet, CargoType cargo, StationID legacy_hop);
	std::optional<VehicleCargoList::MoveToAction> Decide(CargoPacket &packet, CargoType cargo, bool accepted, OrderUnloadType unload);
};

/** Available waiting counts only. Reservations are reported separately by StationCargoList. */
std::array<uint, 3> CountPassengerRoutingModes(const StationCargoList &cargo);
std::array<uint, 3> CountReservedPassengerRoutingModes(const Station &station, CargoType cargo);

#endif /* PASSENGER_JOURNEY_ACTIVE_H */
