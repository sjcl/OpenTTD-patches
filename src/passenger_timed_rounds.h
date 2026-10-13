/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
/** @file passenger_timed_rounds.h Bounded boarding rounds over published periodic services. */
#ifndef PASSENGER_TIMED_ROUNDS_H
#define PASSENGER_TIMED_ROUNDS_H

#include "passenger_service_database.h"

struct PassengerTimedRoundResult {
	PassengerPeriodicPlan plan;
	PassengerTimedRoundOutcome outcome = PassengerTimedRoundOutcome::Invalid;
	PassengerTimedRoundDiagnostic diagnostic;
};

/** One synchronous query over published services, with optional cached relaxed lower bounds. */
class PassengerTimedRounds {
public:
	static PassengerTimedRoundResult Plan(PassengerServiceDatabase &db, CargoType cargo, StationID origin,
			StationID destination, StateTicks ready, PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation,
			OrderListID excluded);
};

#endif /* PASSENGER_TIMED_ROUNDS_H */
