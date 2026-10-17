/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */

/** @file passenger_route_overlay.h Non-persistent, read-only passenger route estimates for the GUI. */
#ifndef PASSENGER_ROUTE_OVERLAY_H
#define PASSENGER_ROUTE_OVERLAY_H

#include "../passenger_journey_type.h"
#include "../cargo_type.h"
#include <map>
#include <vector>

struct Station;
class PassengerServiceDatabase;

struct StationPassengerRoutes {
	using Link = std::tuple<StationID, StationID, StationID>; ///< First movement, from, to.
	std::map<Link, double> links;
	std::map<StationID, double> alighting, first_hops;
};

/** Validate a whole saved ride before displaying it. Empty means unresolved, never direct to alighting.
 * If requested, count all usable live carriers; incomplete enumeration returns no route and a zero count. */
std::vector<StationID> GetPassengerRouteStops(const PassengerJourneyExecution &ride, CargoType cargo, size_t &work, size_t *usable_vehicles = nullptr);
StationPassengerRoutes BuildStationPassengerRoutes(const Station &station, const PassengerServiceDatabase &database);

#endif
