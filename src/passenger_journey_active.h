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
void RouteWaitingPassengerJourneys(Station *station);
void AfterLoadPassengerJourneys();
void InvalidatePassengerJourneyVehicle(VehicleID vehicle);

/** Evaluate once per coach loading operation; packets only compare the captured execution. */
class PassengerJourneyBoardingContext {
	const Vehicle *vehicle;
	CargoType cargo;
	uint64_t signature = 0;
	PassengerVisitPrediction prediction;
public:
	explicit PassengerJourneyBoardingContext(const Vehicle *vehicle, CargoType cargo = INVALID_CARGO);
	bool CanBoard(const CargoPacket &packet) const;
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
