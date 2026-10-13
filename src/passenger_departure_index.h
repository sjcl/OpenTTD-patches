/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_departure_index.h Derived, cargo-specific index of an immutable published fleet. */
#ifndef PASSENGER_DEPARTURE_INDEX_H
#define PASSENGER_DEPARTURE_INDEX_H

#include "date_type.h"
#include "vehicle_type.h"
#include "cargo_type.h"
#include "order_type.h"
#include <span>
#include <vector>
#include <algorithm>
#include <optional>

struct PassengerPeriodicService;

class PassengerDepartureIndex {
public:
	PassengerDepartureIndex(const PassengerPeriodicService &service, CargoType cargo);
	bool Matches(const PassengerPeriodicService &service) const;
	bool Contains(VehicleID id) const { return std::binary_search(this->ids.begin(), this->ids.end(), id); }
	std::optional<StateTicks> Next(int64_t offset, StateTicks ready, std::span<const VehicleID> excluded, uint64_t &calculations) const;
	size_t Known() const { return this->phases.size(); }
	bool Unknown() const { return this->unknown; }
	size_t Bytes() const;
	static size_t Envelope(size_t vehicles) { return 512 + 128 * vehicles; }
private:
	struct Phase { int64_t remainder, base; VehicleID id; };
	OrderListID service;
	uint64_t incarnation, generation;
	int64_t period;
	bool unknown = false;
	std::vector<Phase> phases; ///< Sorted by nonnegative remainder, then first base and vehicle ID; duplicates retained.
	std::vector<size_t> starts; ///< Same entries, sorted by first base; never allow a cycle before this lower bound.
	std::vector<VehicleID> ids; ///< Known vehicles with positive capacity for this cargo, for local live corrections.
	std::vector<int64_t> minimum_base; ///< Segment tree over phases: find a successor whose first base is <= ready-offset.
	size_t leaves = 1;
	size_t Find(size_t node, size_t begin, size_t end, size_t from, size_t until, int64_t floor, std::span<const VehicleID> excluded) const;
};

#endif /* PASSENGER_DEPARTURE_INDEX_H */
