/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_journey_schedule.h Duration-only OD policies over the published periodic database. */
#ifndef PASSENGER_JOURNEY_SCHEDULE_H
#define PASSENGER_JOURNEY_SCHEDULE_H

#include "passenger_journey_planner.h"
#include "passenger_journey_type.h"

class PassengerServiceDatabase;
struct PassengerSchedulePlan {
	std::optional<PassengerJourneyExecution> ride;
	int64_t cost = 0, riding = 0, waiting = 0;
	size_t transfers = 0, work = 0;
	bool deferred = false;
};
struct PassengerScheduleDiagnostic {
	uint64_t hits = 0, misses = 0, work = 0, query_microseconds = 0, compile_microseconds = 0;
	size_t bytes = 0;
};

/** Derived cache owned by the DB. Both planners share the DB residency and station revisions. Cache work
 * is replayed, so eviction and network-join cold caches cannot affect admission.
 * Stores OD policies only, never a separate service graph or physical binding. */
class PassengerSchedulePlanner {
	PassengerScheduleDiagnostic diagnostic;
	PassengerSchedulePlan Search(const PassengerServiceDatabase &db, CargoType cargo, StationID origin, StationID destination,
			OrderListID excluded, PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, std::map<StationID, uint64_t> &dependencies) const;
public:
	PassengerSchedulePlan Plan(PassengerServiceDatabase &db, CargoType cargo, StationID origin, StationID destination,
			StateTicks ready, OrderListID excluded, PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation);
	void ResetDiagnostic() { this->diagnostic = {}; }
	PassengerScheduleDiagnostic GetDiagnostic() const { return this->diagnostic; }
};

#endif
