/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file passenger_journey_planner.h Bounded, multi-label timetable journeys, independent of cargo staging. */

#ifndef PASSENGER_JOURNEY_PLANNER_H
#define PASSENGER_JOURNEY_PLANNER_H

#include "passenger_service_graph.h"
#include <tuple>

struct PassengerJourneyLimits {
	uint32_t max_transfers = 65535; ///< Safety ceiling, never a certificate of unreachability. Zero requests direct travel only.
	size_t candidates = 8;
	size_t labels_per_station = 16; ///< Per round; preserve distinct vehicle paths even when slower.
	size_t labels = 65536;
	size_t work = 1048576;
	size_t memory_bytes = 128 * 1024 * 1024; ///< Upper bound, allocated incrementally for reachability and labels.
	uint32_t initial_max_transfers = 4; ///< Search this range for alternatives; extend geometrically if no journey is found.
	auto operator<=>(const PassengerJourneyLimits &) const = default;
};

/** All penalties are nonnegative state ticks; 1000 means one additional tick per tick. */
struct PassengerJourneyEvaluation {
	Ticks minimum_transfer = 0;
	Ticks transfer_penalty = 0;
	uint32_t waiting_weight = 0; ///< Additional preference penalty; elapsed time already includes all waiting.
	uint32_t transfer_time_weight = 0; ///< Additional weight on the minimum interchange time.
	Ticks crowding_penalty = 0; ///< Penalty at 100% occupancy.
	Ticks capacity_shortfall_penalty = 0; ///< Penalty scaled by unmet demand / demand in a supplied window.
	/** Evaluation boundary for future station/mode-specific connections. The common
	 * minimum_transfer remains a lower bound for departure indexing and pruning. */
	Ticks MinimumTransfer(VehicleType from, VehicleType to, StationID station) const { return this->minimum_transfer; }
	auto operator<=>(const PassengerJourneyEvaluation &) const = default;
};

enum class PassengerCapacityCertainty : uint8_t { Unknown, Estimated, Confirmed };

/** Observation at capture and forecast at boarding are deliberately separate. No reservation is implied. */
struct PassengerJourneyCapacity {
	uint64_t observed_total = 0;
	std::optional<uint64_t> observed_occupants;
	std::optional<uint64_t> current_free;
	std::optional<uint64_t> future_total;
	std::optional<uint64_t> future_free;
	PassengerCapacityCertainty certainty = PassengerCapacityCertainty::Unknown;
	/** Observed/predicted real departures in the same window, supplied by the evaluator; never synthetic trips. */
	std::optional<uint64_t> departures_in_window;
	std::optional<uint64_t> seats_per_departure;
	std::optional<uint64_t> demand_in_window;
	StateTicksDelta window_ticks{};
	bool operator==(const PassengerJourneyCapacity &) const = default;
};

/** Pure, bounded, simulation-thread forecast interface. Increment Revision whenever any estimate changes. */
class PassengerJourneyCapacityProvider {
public:
	virtual ~PassengerJourneyCapacityProvider() = default;
	virtual uint64_t Revision() const = 0;
	/** Supply a segment forecast; the planner fills captured observations itself. O(1) provider work is expected. */
	virtual PassengerJourneyCapacity Estimate(const PassengerServiceGraph &graph,
			PassengerServiceGraph::VisitID boarding, PassengerServiceGraph::VisitID alighting) const = 0;
};

struct PassengerJourneyLeg {
	VehicleID vehicle;
	StationID boarding_station;
	StationID alighting_station;
	StateTicks departure;
	StateTicks arrival;
	PassengerServiceGraph::VisitID boarding; ///< Snapshot-only ID. Never serialize a journey.
	PassengerServiceGraph::VisitID alighting;
	PassengerPredictionStatus prediction_status;
	PassengerJourneyCapacity capacity;
	bool operator==(const PassengerJourneyLeg &) const = default;
};

struct PassengerPlannedJourney {
	std::vector<PassengerJourneyLeg> legs;
	StateTicks arrival{};
	StateTicksDelta waiting{}; ///< Includes the initial wait and all interchange time.
	StateTicksDelta transfer_time{}; ///< Minimum interchange time, a subset of waiting.
	StateTicksDelta riding{}; ///< Includes intermediate dwell while remaining aboard.
	StateTicksDelta generalized_time{};
	StateTicksDelta capacity_penalty{};
	uint32_t transfers = 0;
	PassengerPredictionStatus prediction_status = PassengerPredictionStatus::Complete;
	bool capacity_uncertain = false;
	bool operator==(const PassengerPlannedJourney &) const = default;
};

struct PassengerJourneyWork {
	uint32_t rounds = 0; ///< Boarding rounds actually entered, including the initial ride.
	uint32_t extensions = 0; ///< Stage increases; previous rounds are never replayed.
	size_t work = 0;
	size_t reverse_work = 0; ///< Included in work, not a separate allowance.
	size_t boardings = 0;
	size_t pruned_boardings = 0; ///< Departures excluded by destination reachability.
	size_t pruned_labels = 0; ///< Frontiers whose lower bound cannot improve the requested candidates.
	size_t visits = 0;
	size_t labels = 0;
	size_t label_capacity = 0; ///< Peak allocated label slots, independent of the configured cap.
	size_t capacity_rejections = 0;
	size_t cycle_rejections = 0; ///< Station revisits excluded from transfer paths; through rides retain real occurrences.
	size_t scratch_bytes = 0; ///< Conservative peak scratch/returned-path allowance, diagnostic only.
	bool cache_hit = false;
	bool uncertainty_resolved = false; ///< Diagnostic: alternatives proved irrelevant to the requested K candidates.
	std::optional<StateTicks> unknown_arrival_bound; ///< Diagnostic: optimistic arrival using at least one uncertified event.
	OrderListID unknown_service = OrderListID::Invalid(); ///< Diagnostic witness for the optimistic bound.
};

enum class PassengerJourneyStopReason : uint8_t {
	Exhausted, ///< All relevant frontiers exhausted within the snapshot horizon.
	TransferLimit, ///< A stage/safety ceiling leaves possible onward paths.
	WorkLimit,
	LabelLimit,
	MemoryLimit,
	PredictionIncomplete,
	PredictionHorizon, ///< A declared operational corridor exists, but no timed journey fits in this snapshot.
	InvalidQuery,
};

struct PassengerJourneyResult {
	std::vector<PassengerPlannedJourney> journeys;
	PassengerPredictionStatus status = PassengerPredictionStatus::Complete;
	PassengerJourneyStopReason stop_reason = PassengerJourneyStopReason::Exhausted;
	StateTicks prediction_end{}; ///< Exclusive horizon: absence never certifies permanent disconnection.
	PassengerJourneyWork diagnostic;
	/** Every returned candidate reaches the requested destination with validated times; no partial paths are returned.
	 * Incomplete search status concerns alternatives, independently of a usable end-to-end candidate. */
	bool HasJourney() const { return !this->journeys.empty(); }
	/** Absence is certified only for an exhausted search within prediction_end and the supplied conditions. */
	bool IsUnreachable() const { return !this->HasJourney() && this->status == PassengerPredictionStatus::Complete && this->stop_reason == PassengerJourneyStopReason::Exhausted; }
	/** Includes invalid/unknown OD: callers may use CargoDist without altering the passenger destination. */
	bool IsInconclusive() const { return !this->HasJourney() && !this->IsUnreachable(); }
};

struct PassengerJourneyCacheLimits {
	size_t entries = 128;
	size_t bytes = 8 * 1024 * 1024;
};

/**
 * RAPTOR boarding rounds over an immutable multimodal snapshot, with Pareto labels per vehicle path.
 * Slower corridors survive dominance by faster, different corridors, up to explicit label limits.
 * Reverse reachability visits only destination/uncertainty-connected occurrences. No live prediction or cargo staging.
 * The graph and optional provider must outlive this object and remain immutable during each query.
 * Cache eviction affects performance only. Search allowances are per query, including cache misses.
 */
class PassengerJourneyPlanner {
	const PassengerServiceGraph &graph;
	const PassengerJourneyCapacityProvider *capacity_provider;
	PassengerJourneyCacheLimits cache_limits;
	using CacheKey = std::tuple<CargoType, StationID, StationID, StateTicks, uint64_t, uint64_t,
			PassengerJourneyLimits, PassengerJourneyEvaluation, size_t, size_t>;
	std::map<CacheKey, PassengerJourneyResult> cache;
	size_t cache_bytes = 0;
	PassengerJourneyResult Search(StationID origin, StationID destination, StateTicks ready, uint64_t passengers,
			PassengerJourneyLimits limits, const PassengerJourneyEvaluation &evaluation,
			std::optional<PassengerServiceGraph::VisitID> onboard = {}) const;
	void Retain(CacheKey key, const PassengerJourneyResult &result);

public:
	explicit PassengerJourneyPlanner(const PassengerServiceGraph &graph,
			const PassengerJourneyCapacityProvider *capacity_provider = nullptr, PassengerJourneyCacheLimits cache_limits = {}) :
			graph(graph), capacity_provider(capacity_provider), cache_limits(cache_limits) {}
	PassengerJourneyResult Plan(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
			PassengerJourneyLimits limits = {}, PassengerJourneyEvaluation evaluation = {}, uint64_t passengers = 1);
	/** Existing passenger at this snapshot's unique loading occurrence. Staying neither boards nor needs free seats.
	 * Other vehicles require alighting here and the normal interchange, boarding and capacity rules. */
	PassengerJourneyResult PlanOnboard(CargoType cargo, PassengerServiceGraph::VisitID onboard, StationID destination,
			PassengerJourneyLimits limits = {}, PassengerJourneyEvaluation evaluation = {}, uint64_t passengers = 1);
	void ClearCache() { this->cache.clear(); this->cache_bytes = 0; }
	size_t GetCacheEntries() const { return this->cache.size(); }
	size_t GetCacheBytes() const { return this->cache_bytes; }
};

#endif /* PASSENGER_JOURNEY_PLANNER_H */
