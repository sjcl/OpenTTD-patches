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
	std::map<CargoType, std::unique_ptr<Snapshot>> snapshots;
	size_t work_left = 2097152;
	size_t queries_left = 128;
	Snapshot *GetSnapshot(CargoType cargo);
	std::optional<PassengerJourneyExecution> Execute(CargoType cargo, const PassengerJourneyLeg &leg);
public:
	PassengerJourneyActiveContext(const Vehicle *vehicle, StationID station);
	~PassengerJourneyActiveContext();
	size_t GetWorkAllowanceUsed() const { return 2097152 - this->work_left; }
	bool PlanWaiting(CargoPacket &packet, CargoType cargo, StationID legacy_hop);
	std::optional<VehicleCargoList::MoveToAction> Decide(CargoPacket &packet, CargoType cargo, bool accepted, OrderUnloadType unload);
};

/** Available waiting counts only. Reservations are reported separately by StationCargoList. */
std::array<uint, 3> CountPassengerRoutingModes(const StationCargoList &cargo);
std::array<uint, 3> CountReservedPassengerRoutingModes(const Station &station, CargoType cargo);

#endif /* PASSENGER_JOURNEY_ACTIVE_H */
