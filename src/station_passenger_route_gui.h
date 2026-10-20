/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */

/** @file station_passenger_route_gui.h Bounded, client-local waiting passenger route examples. */
#ifndef STATION_PASSENGER_ROUTE_GUI_H
#define STATION_PASSENGER_ROUTE_GUI_H

#include "station_cargo_gui.h"

class PassengerServiceDatabase;
struct Station;

/** Only this exact typed destination level can have a route tooltip. */
struct StationPassengerRouteTarget {
	CargoType cargo;
	StationPassengerService service;
	StationID destination;
	auto operator<=>(const StationPassengerRouteTarget &) const = default;
};
std::optional<StationPassengerRouteTarget> GetStationPassengerRouteTarget(bool waiting, std::span<const StationCargoKey> path);
std::optional<size_t> GetStationPassengerRouteRow(int scrolled_row, int position, size_t displayed_count);

/** Value snapshots, recollected from the live waiting list on every hover. No packet IDs or pointers survive. */
struct StationPassengerRouteSample {
	PassengerRoutingMode mode;
	PassengerJourneyExecution ride;
	OrderListID previous, failed;
	uint count;
	bool operator==(const StationPassengerRouteSample &) const = default;
};
struct StationPassengerRouteSamples {
	static constexpr size_t MAX_SAMPLES = 8, SCAN_LIMIT = 4096;
	std::vector<StationPassengerRouteSample> packets;
	bool incomplete = false, multiple = false;
	bool operator==(const StationPassengerRouteSamples &) const = default;
};
StationPassengerRouteSamples CollectStationPassengerRouteSamples(const Station &station, const StationPassengerRouteTarget &target,
		uint row_count, StationPassengerServiceCache &services, size_t scan_limit = StationPassengerRouteSamples::SCAN_LIMIT);

struct StationPassengerRouteExample {
	std::vector<PassengerJourneyExecution> legs;
	bool complete = false;
};
struct StationPassengerRouteExamples {
	static constexpr size_t MAX_ROUTES = 3, MAX_LEGS = 5, QUERY_WORK = 1024, TOTAL_WORK = 8192;
	std::vector<StationPassengerRouteExample> routes;
	bool more = false, unknown = false, multiple = false;
};
StationPassengerRouteExamples EstimateStationPassengerRoutes(CargoType cargo, StationID from, StationID destination,
		const StationPassengerRouteSamples &samples, const PassengerServiceDatabase &database, size_t &work);

/** One last-hover entry per window; bounded by 8 inputs and 3 x 5 legs. No names or game pointers. */
class StationPassengerRouteCache {
public:
	StationPassengerRouteCache();
	~StationPassengerRouteCache();
	const StationPassengerRouteExamples &Get(const Station &station, const StationPassengerRouteTarget &target, uint row_count,
			StationPassengerServiceCache &services);
	void Clear();
private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

#endif /* STATION_PASSENGER_ROUTE_GUI_H */
