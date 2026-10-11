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

/** Only the next ride is stored. Absolute times distinguish repeated visits to an order.
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
	bool operator==(const PassengerJourneyExecution &) const = default;
};

#endif /* PASSENGER_JOURNEY_TYPE_H */
