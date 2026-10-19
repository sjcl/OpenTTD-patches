/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_service_index.h Conservative, untimed OD corridors. */
#ifndef PASSENGER_SERVICE_INDEX_H
#define PASSENGER_SERVICE_INDEX_H

#include "passenger_routing.h"
#include <set>

struct PassengerServiceScope {
	StationID origin;
	StationID destination;
	OrderListID onboard_service = OrderListID::Invalid(); ///< Capture the current occurrence even if transferring immediately is best.
	bool operator==(const PassengerServiceScope &) const = default;
};

struct PassengerServiceSelection {
	std::set<OrderListID> services;
	bool onboard_on_corridor = false; ///< The onboard service was selected by the OD corridor, rather than added only for capture.
	PassengerPredictionStatus status = PassengerPredictionStatus::Complete;
	size_t memory_bound = 0;
	size_t resident_bound = 0; ///< Cold decomposition scratch is gone before dynamic capture begins.
	size_t allocation_bytes = 0;
	size_t reused_work = 0;
	bool cache_hit = false;
	size_t largest_block_services = 0; ///< Diagnostic count; does not select or rank routes.
};

/** Includes every service on any OD corridor in the undirected declared-stop supergraph.
 * An excluded excursion must return to the same articulation station: waiting there costs no
 * more under unweighted waiting and preserves all onward departures. Weighted waiting
 * can prefer riding an excursion; the planner reports such scoped searches as Unknown.
 * Service articulation blocks are contracted:
 * changing to an express and back to the same local service cannot prune that express.
 * All cycles on the corridor are retained.
 * Cargo exchange flags also filter the incidence graph. Direction, consists, refits
 * and timings are read live by the graph builder; this remains a conservative corridor.
 * Eligible IDs are canonical pool order and are compared exactly before cache reuse. */
/* Prediction policy: explicit instructions only. Historical implicit stops are
 * skipped, and future implicit order creation is not forecast. */
PassengerServiceSelection SelectPassengerServices(CargoType cargo, PassengerServiceScope scope, std::span<const OrderListID> eligible, size_t &work, size_t memory_bytes);
void InvalidatePassengerGraphServiceIndex();
uint64_t PassengerGraphServiceIndexRevision(); ///< Local cache invalidation only, never simulation state.

#endif /* PASSENGER_SERVICE_INDEX_H */
