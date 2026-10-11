/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file passenger_od.h Sparse, deterministic assignment of CargoDist passenger demand. */

#ifndef PASSENGER_OD_H
#define PASSENGER_OD_H

#include "station_type.h"
#include "linkgraph/linkgraph_type.h"
#include "cargotype.h"
#include <array>
#include <vector>

class LinkGraphJob;
struct Station;
struct GoodsEntry;
struct Source;

/** Bounds apply independently of station count and passenger batch size. */
static constexpr size_t MAX_PASSENGER_OD_DESTINATIONS = 256;
static constexpr size_t MAX_PASSENGER_OD_PACKETS = 32;

struct PassengerOdEntry {
	StationID destination = StationID::Invalid();
	NodeID node = INVALID_NODE;
	uint32_t weight = 0;
	int64_t credit = 0; ///< Signed rounding remainder, in units of total_weight.
	bool operator==(const PassengerOdEntry &) const = default;
};

/** Only allocated for passenger goods entries with a usable distribution. */
struct PassengerOdDistribution {
	LinkGraphID graph = LinkGraphID::Invalid();
	NodeID source_node = INVALID_NODE;
	uint64_t revision = 0; ///< Identity of the graph topology that produced this distribution.
	uint64_t total_weight = 0;
	std::vector<PassengerOdEntry> entries;

	bool Validate() const;
	std::array<uint32_t, MAX_PASSENGER_OD_DESTINATIONS> Allocate(uint32_t amount);
	bool operator==(const PassengerOdDistribution &) const = default;
};

/** Diagnostic counters only; never used to make simulation decisions. */
struct PassengerOdStats {
	uint64_t distributions = 0;
	uint64_t entries = 0;
	uint64_t build_microseconds = 0;
	uint64_t allocation_microseconds = 0;
	uint64_t packets = 0;
	uint64_t fallback_passengers = 0;
};
extern PassengerOdStats _passenger_od_stats;
extern uint64_t _passenger_od_revision;

uint64_t NextPassengerOdRevision();
bool IsPassengerOdEnabled(CargoType cargo);
bool ValidatePassengerOd(const Station &source, CargoType cargo, const PassengerOdDistribution &od);
void ApplyPassengerOd(LinkGraphJob &job);
void ClearPassengerOdDistributions();
void AfterLoadPassengerOd();
void GenerateStationCargo(Station &st, CargoType cargo, uint amount, Source source);

#endif /* PASSENGER_OD_H */
