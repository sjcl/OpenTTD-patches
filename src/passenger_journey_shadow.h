/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_journey_shadow.h Local, read-only observations of the actual cargo staging decision. */
#ifndef PASSENGER_JOURNEY_SHADOW_H
#define PASSENGER_JOURNEY_SHADOW_H

#include "passenger_journey_planner.h"
#include "cargopacket.h"
#include <memory>

enum class PassengerShadowDecision : uint8_t { Stay, Transfer, Arrived, Unknown, Unreachable };
enum class PassengerShadowCategory : uint8_t {
	BothStay, BothTransfer, LegacyTransferPlannerStay, LegacyStayPlannerTransfer, ArrivalMismatch,
	BothArrived, Unknown, Unreachable, Constraints, Skipped, End,
};

struct PassengerShadowCount {
	uint64_t packets = 0;
	uint64_t passengers = 0;
	void Add(uint count) { ++this->packets; this->passengers += count; }
};

struct PassengerJourneyShadowStats {
	PassengerShadowCount observed, known_od, evaluated, unknown, decidable, budget_exhausted, limit_reached, horizon_incomplete;
	PassengerShadowCount eligible_od, unsupported_od, manual_od, uncaptured_od;
	struct Mode {
		PassengerShadowCount evaluated, journeys, fallback;
		uint64_t mixed_transfers = 0;
		std::array<uint64_t, to_underlying(PassengerPredictionReason::End)> prediction_reasons{};
	};
	std::array<Mode, to_underlying(VehicleType::CompanyEnd)> modes{};
	std::array<PassengerShadowCount, to_underlying(PassengerShadowCategory::End)> categories{};
	uint64_t builds = 0, queries = 0, cache_hits = 0, work = 0, max_work = 0;
	uint64_t graph_us = 0, search_us = 0, cpu_ns = 0; ///< Accumulate sub-microsecond skipped observations as well as capture/search/destruction.
	size_t scratch_peak = 0, graph_memory_bound = 0, cache_peak = 0;
};

/** Shared quota per 64 state ticks, across all vehicles/cargoes on this client.
 * Smaller limits are useful for tests; constructor clamps every allowance to these hard defaults. */
struct PassengerJourneyShadowLimits {
	size_t builds = 1;
	size_t queries = 32;
	size_t observations = 512;
	size_t work = 2097152;
	PassengerServiceGraphLimits graph;
	PassengerJourneyLimits journey;
	PassengerJourneyShadowLimits()
	{
		this->graph.work = 1048576;
		this->graph.memory_bytes = 1024 * 1024 * 1024;
		this->journey.work = 65536;
		this->journey.memory_bytes = 8 * 1024 * 1024;
		this->journey.candidates = 1;
	}
};

/** Values only: never retain CargoPacket, vehicle/order pointers or snapshot VisitIDs in diagnostics. */
struct PassengerJourneyShadowDetail {
	StationID station, origin, destination, via;
	VehicleID vehicle;
	VehicleCargoList::MoveToAction legacy;
	PassengerShadowDecision planner;
	PassengerPredictionStatus status;
	PassengerShadowCategory category;
	uint count;
	StateTicks arrival{};
	StateTicksDelta generalized_time{};
	uint8_t transfers = 0;
};

bool IsPassengerJourneyShadowEnabled();
void SetPassengerJourneyShadowEnabled(bool enabled);
void ResetPassengerJourneyShadowStats();
const PassengerJourneyShadowStats &GetPassengerJourneyShadowStats();
std::span<const PassengerJourneyShadowDetail> GetPassengerJourneyShadowDetails();
const char *PassengerShadowCategoryName(PassengerShadowCategory category);
PassengerShadowCategory ComparePassengerShadowDecision(VehicleCargoList::MoveToAction legacy, PassengerShadowDecision planner, bool constrained);

/** Construct BEFORE PrepareUnload mutates loading_vehicles, cargo/payment or consist flags.
 * All coaches share this arrival's frozen snapshots. Destruction discards every planner/cache/VisitID.
 * Observe receives the already chosen legacy action, never calls routing or draws a flow again. */
class PassengerJourneyShadowContext {
	struct Snapshot {
		PassengerServiceGraph graph;
		PassengerJourneyPlanner planner;
		std::optional<PassengerServiceGraph::VisitID> onboard;
		Snapshot(PassengerServiceGraph graph, const Vehicle *vehicle, StationID station);
	};
	const Vehicle *vehicle;
	StationID station;
	StateTicks capture;
	PassengerJourneyShadowLimits limits;
	std::map<CargoType, std::unique_ptr<Snapshot>> snapshots;
	bool active = false;
public:
	PassengerJourneyShadowContext(const Vehicle *vehicle, StationID station, PassengerJourneyShadowLimits limits = {});
	~PassengerJourneyShadowContext();
	void Observe(const CargoPacket &packet, CargoType cargo, VehicleCargoList::MoveToAction legacy,
			StationID via, OrderUnloadType unload);
};

#endif /* PASSENGER_JOURNEY_SHADOW_H */
