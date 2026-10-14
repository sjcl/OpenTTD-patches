/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_journey_type.h Persistent passenger execution state. */
#ifndef PASSENGER_JOURNEY_TYPE_H
#define PASSENGER_JOURNEY_TYPE_H

#include "vehicle_type.h"
#include "order_type.h"
#include "station_type.h"
#include "date_type.h"

enum class PassengerRoutingMode : uint8_t { Legacy, Journey, Fallback };

enum class PassengerRideKind : uint8_t { VehicleSnapshot, ServiceIntent, BoundService, EstimatedServiceIntent, EstimatedBoundService };
enum class PassengerTransportKind : uint8_t { None, Timed, Schedule, Untimed };
inline bool IsEstimatedPassengerRide(PassengerRideKind kind) { return kind == PassengerRideKind::EstimatedServiceIntent || kind == PassengerRideKind::EstimatedBoundService; }
inline bool IsBoundPassengerRide(PassengerRideKind kind) { return kind == PassengerRideKind::BoundService || kind == PassengerRideKind::EstimatedBoundService; }

/** Only the next ride is stored. The kind distinguishes old absolute vehicle visits
 * from recurring service intentions and their actual bound carrier. New intentions
 * retain occurrences and a boarding deadline, without requiring a matching forecast time.
 * No snapshot indices, live pointers, forecast reservations or complete itineraries. */
struct PassengerJourneyExecution {
	VehicleID vehicle = VehicleID::Invalid();
	OrderListID service = OrderListID::Invalid();
	StationID boarding = StationID::Invalid();
	StationID alighting = StationID::Invalid();
	VehicleOrderID boarding_order = INVALID_VEH_ORDER_ID;
	VehicleOrderID alighting_order = INVALID_VEH_ORDER_ID;
	StateTicks departure{};
	StateTicks arrival{};
	uint64_t signature = 0;
	PassengerRideKind kind = PassengerRideKind::VehicleSnapshot;
	uint64_t generation = 0;
	StateTicks not_before{}; ///< Earliest real boarding, including an actual interchange's minimum transfer time.
	StateTicks planned_at{}; ///< Schedule-aware decision time for observing waiting/expiry; not set for Timed intents.
	int64_t estimated_wait = 0; ///< Duration in ticks; never an absolute departure.
	int64_t waiting_limit = 0; ///< Re-evaluate after two conservative service circuits.
	bool timed_deferred = false; ///< Timed work was insufficient; absence of a timed route is not asserted.
	bool operator==(const PassengerJourneyExecution &) const = default;
};

/** Schedule-aware intent age, not continuous passenger waiting time. Timed
 * intents have no decision timestamp, so they cannot supply this diagnostic. */
inline bool IsPassengerScheduleIntentAged(const PassengerJourneyExecution &ride, StateTicks now, int64_t minimum_age)
{
	return IsEstimatedPassengerRide(ride.kind) && now >= ride.planned_at && now.base() - ride.planned_at.base() >= minimum_age;
}

#endif /* PASSENGER_JOURNEY_TYPE_H */
