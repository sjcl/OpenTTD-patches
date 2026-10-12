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
	uint64_t hits = 0, misses = 0, work = 0, query_microseconds = 0;
	size_t bytes = 0;
};

/** Derived cache owned by the DB. Publication/invalidation clears it. Cache work
 * is replayed, so eviction and network-join cold caches cannot affect admission.
 * Stores OD policies only, never a separate service graph or physical binding. */
class PassengerSchedulePlanner {
	using Key = std::tuple<CargoType, StationID, StationID, OrderListID, PassengerJourneyLimits, PassengerJourneyEvaluation>;
	std::map<Key, PassengerSchedulePlan> cache;
	size_t bytes = 0;
	PassengerScheduleDiagnostic diagnostic;
	PassengerSchedulePlan Search(const PassengerServiceDatabase &db, CargoType cargo, StationID origin, StationID destination,
			OrderListID excluded, PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation) const;
public:
	PassengerSchedulePlan Plan(const PassengerServiceDatabase &db, CargoType cargo, StationID origin, StationID destination,
			StateTicks ready, OrderListID excluded, PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation);
	void Clear() { this->cache.clear(); this->bytes = 0; }
	void ResetDiagnostic() { this->diagnostic = {}; }
	PassengerScheduleDiagnostic GetDiagnostic() const { auto d = this->diagnostic; d.bytes = this->bytes; return d; }
};

#endif
