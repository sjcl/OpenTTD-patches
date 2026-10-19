/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_service_database.h Persistent periodic services; no expanded future visits. */
#ifndef PASSENGER_SERVICE_DATABASE_H
#define PASSENGER_SERVICE_DATABASE_H

#include "passenger_precomputed_progress.h"

#include "passenger_journey_planner.h"
#include "passenger_journey_type.h"
#include "passenger_journey_schedule.h"
#include "passenger_departure_index.h"
#include "passenger_timed_search.h"
#include "passenger_precomputed.h"
#include <set>
#include <list>
#include <variant>
#include <bitset>

struct PassengerPeriodicStop {
	StationID station = StationID::Invalid();
	VehicleOrderID order = INVALID_VEH_ORDER_ID;
	int64_t arrival = 0, departure = 0;
	uint64_t board = 0, alight = 0, forced = 0; ///< Cargo-slot masks, independent of cargo labels.
	bool continues = true; ///< Safe order traversal from this occurrence to the next, including wraparound.
	uint8_t travel_quality = 0; ///< Incoming segment: 0 declared, 1 recorded, 2 distance estimate.
	uint8_t boarding_facility = 0; ///< v14 captured road-order access: 0 unknown, 1 any direction, 2..5 a specific direction.
	bool operator==(const PassengerPeriodicStop &) const = default;
};

/** Offsets cover exactly one circuit, including the travel back to the first order.
 * Missing flags, unsupported controls and zero-length circuits remain untimed. */
struct PassengerPeriodicTimetable {
	std::vector<PassengerPeriodicStop> stops; ///< Physical station events; zero exchange masks stay for phases, dispatch and saved occurrence IDs.
	int64_t period = 0;
	int64_t estimated_period = 0; ///< Duration estimate only. Never supplied to NextDeparture or phase capture.
	enum class Certainty : uint8_t { Unsupported, Estimated, Certified };
	Certainty GetCertainty() const { return this->period > 0 ? Certainty::Certified : this->estimated_period > 0 ? Certainty::Estimated : Certainty::Unsupported; }
	int64_t Circuit() const { return this->period > 0 ? this->period : this->estimated_period; }
	enum class Failure : uint8_t { None, Control, Refit, Travel, Wait, Overflow, ZeroPeriod, TooFewStops, End };
	static PassengerPeriodicTimetable Compile(std::span<const Order> orders, Failure *failure = nullptr);
	static PassengerPeriodicTimetable CompileEstimated(std::span<const Order> orders, VehicleType type);
	static std::optional<StateTicks> NextDeparture(StateTicks base, int64_t offset, int64_t period, StateTicks ready);
	bool operator==(const PassengerPeriodicTimetable &) const = default;
};

struct PassengerPeriodicVehicle {
	VehicleID id = VehicleID::Invalid();
	StateTicks base{};
	bool phase_known = false;
	VehicleType type = VehicleType::Invalid;
	std::array<uint64_t, NUM_CARGO> capacity{};
	bool operator==(const PassengerPeriodicVehicle &) const = default;
};

struct PassengerPeriodicService {
	OrderListID id = OrderListID::Invalid();
	uint64_t incarnation = 0, generation = 0;
	PassengerPeriodicTimetable timetable;
	std::vector<PassengerPeriodicVehicle> vehicles;
	StateTicks dispatch_base{};
	StateTicks dispatch_floor{}; ///< Existing non-reusable slot assignment, captured with this generation.
	int64_t dispatch_period = 0;
	size_t dispatch_stop = 0;
	std::vector<uint32_t> dispatch_slots; ///< Certified only for a single physical vehicle with enough turnaround time.
	int64_t uncertain_dispatch_period = 0; ///< Declared dispatch circuit, without claiming slots/fleet ownership.
	bool operator==(const PassengerPeriodicService &) const = default;
};

struct PassengerPeriodicPlan {
	/** Certification is relative to the published Timed search domain. Unavailable
	 * covers invalid/unsupported queries and unknown timing, without a NoRoute claim. */
	enum class State : uint8_t { Unavailable, Certified, Provisional, Deferred, NoRoute, End };
	State state = State::Unavailable;
	enum class WorkStop : uint8_t { None, Admission, Queue, Service, Vehicle, Dispatch, Departure, Alighting, Dominance, End };
	struct SearchDiagnostic {
		size_t reverse_work = 0, forward_work = 0;
		size_t complete_labels = 0, queued_complete = 0, popped_complete = 0;
		size_t destination_labels = 0; ///< Geometric destination labels, including stale service generations.
		std::optional<int64_t> best_complete_cost;
		StateTicks best_complete_arrival{};
		int64_t remaining_lower_bound = INT64_MAX;
		bool improvement_possible = false;
		WorkStop first_work_stop = WorkStop::None;
		std::array<size_t, static_cast<size_t>(WorkStop::End)> work_stops{};
	} search;
	std::optional<PassengerJourneyExecution> ride;
	StateTicks arrival{};
	int64_t cost = 0;
	size_t transfers = 0;
	size_t work = 0;
	bool deferred = false; ///< With a ride: optimality remains unproved. Without one: cannot select a Timed path.
	enum class Reason : uint8_t { Found, Invalid, OriginMissing, DestinationMissing, NoAlighting, Unreachable, NoCandidate,
		UnknownPhase, Evaluation, Candidates, Labels, Work, Memory, Transfers, End };
	Reason reason = Reason::Invalid;
	std::array<bool, static_cast<size_t>(Reason::End)> observations{}; ///< Overlapping observations, once per OD call.
	std::vector<PassengerJourneyExecution> route; ///< Selected path, also used by explicit read-only comparisons.
	size_t labels = 0, candidates = 0, memory = 0;
};

struct PassengerDatabaseDiagnostic {
	std::array<uint64_t, static_cast<size_t>(PassengerPeriodicPlan::State::End)> result_states{};
	uint64_t rescued_unpopped = 0; ///< Selected complete path when the old pop-only search had no winner.
	uint64_t work_with_complete = 0, work_without_complete = 0, work_with_queued_complete = 0, work_with_popped_complete = 0, work_with_improvement = 0;
	uint64_t work_reverse = 0, work_forward = 0;
	uint64_t work_with_destination_label = 0;
	std::array<uint64_t, static_cast<size_t>(PassengerPeriodicPlan::WorkStop::End)> work_stop_calls{}, work_stop_observations{};
	std::array<uint64_t, 65> search_work_histogram{}; ///< 1024-work upper bounds, last bucket includes overflow.
	std::array<PassengerPeriodicPlan::SearchDiagnostic, 16> work_samples{};
	size_t work_sample_count = 0;
	uint64_t builds = 0, build_work = 0, build_microseconds = 0;
	uint64_t searches = 0, query_microseconds = 0, policy_hits = 0, policy_misses = 0;
	uint64_t next_departures = 0; ///< Logical NextDeparture evaluations in the original scan, including skipped indexed candidates.
	uint64_t vehicle_candidates = 0, departure_calculations = 0, departure_index_hits = 0, departure_index_fallbacks = 0;
	uint64_t departure_index_builds = 0, departure_index_build_nanoseconds = 0, forward_microseconds = 0, search_work = 0;
	uint64_t service_bound_checks = 0, service_bound_stops = 0, pruned_services = 0, pruned_vehicle_visits = 0;
	uint64_t timed_labels = 0, frontier_lookups = 0, queue_pushes = 0, queue_pops = 0;
	size_t search_scratch_bytes = 0, search_scratch_peak_bytes = 0; ///< Physical capacity, separate from logical admission.
	size_t departure_index_bytes = 0, departure_index_peak_bytes = 0;
	size_t services = 0, timed_services = 0, vehicles = 0, bytes = 0, policy_bytes = 0, pending = 0;
	size_t known_phase_vehicles = 0;
	size_t physical_passenger_occurrences = 0, exchange_passenger_occurrences = 0; ///< Per passenger cargo bit; observations only.
	size_t depot_timed = 0, depot_untimed = 0, depot_blocked = 0; ///< Published lists containing depot orders, grouped by their most restrictive traversal.
	uint64_t invalidations = 0, global_invalidations = 0, rejected_work = 0, rejected_memory = 0;
	uint64_t maximum_pending_ticks = 0;
	uint64_t publications = 0, published_revision = 0, service_additions = 0, service_removals = 0, service_updates = 0, timetable_notifications = 0;
	uint64_t policy_clears = 0, policies_cleared = 0, maximum_policies_cleared = 0, policy_stale = 0, policy_evictions = 0;
	uint64_t policy_compile_microseconds = 0, policy_validation_microseconds = 0, policy_eviction_microseconds = 0, policy_rebuilds = 0;
	uint64_t publication_microseconds = 0, published_wait_ticks = 0, published_wait_samples = 0;
	size_t policy_entries = 0, staged = 0, staged_bytes = 0;
	enum class ClearReason : uint8_t { Explicit, Reset, Global, End };
	std::array<uint64_t, static_cast<size_t>(ClearReason::End)> clear_reasons{};
	size_t orderlists = 0, passenger_orderlists = 0, complete_timetables = 0, unpublished = 0, dirty_services = 0, pending_passenger = 0;
	size_t declared_complete_timetables = 0, published_passenger = 0, published_cycles = 0;
	size_t compile_success = 0, compile_failure = 0, unknown_phase_vehicles = 0;
	std::array<size_t, static_cast<size_t>(PassengerPeriodicTimetable::Failure::End)> compile_failures{};
	enum class Phase : uint8_t { Known, NoTimetable, VehicleState, OrderProgress, Facilities, Work, Unbunching,
		DispatchCompetition, DispatchUnsupported, Overflow, ElapsedTravel, AwaitCapture, End };
	std::array<size_t, static_cast<size_t>(Phase::End)> phases{};
	std::array<uint64_t, static_cast<size_t>(PassengerPeriodicPlan::Reason::End)> outcomes{}, observations{};
	uint64_t expired_packets = 0, expired_passengers = 0, boarding_failures = 0;
	uint64_t compilation_attempts = 0, compilation_successes = 0;
	uint64_t phase_capture_work = 0; ///< Vehicle capture attempts stopped by work; repeats overlap.
	std::array<uint64_t, static_cast<size_t>(PassengerPeriodicTimetable::Failure::End)> compilation_stops{};
};

/** Simulation-thread owner. Published records and scheduler state are saved because
 * publication/phase capture affects routing. Topology policies are optional derived
 * accelerators: misses and hits consume identical logical search work. */
class PassengerServiceDatabase {
public:
	static constexpr size_t BUILD_WORK = 65536, MAX_BYTES = 64 * 1024 * 1024;
	static constexpr size_t PRECOMPUTED_CAPTURE_WORK = 16 * 1024 * 1024; ///< Generation construction only; packet queries never use this allowance.
	static constexpr size_t POLICY_BYTES = 16 * 1024 * 1024, POLICY_ENTRIES = 4096; ///< Shared by Timed and Schedule-aware, including dependency and LRU storage.
	static constexpr size_t POLICY_STORAGE_BYTES = POLICY_BYTES - 1024 * 1024; ///< Reserve for station stamps and bounded rebuild diagnostics.
	static constexpr int64_t TIMETABLE_EPOCH = 4096;
	void Reset();
	void Invalidate(OrderListID id = OrderListID::Invalid());
	void TimetableChanged(OrderListID id); ///< Keep safe published timing until the next deterministic update epoch.
	void Tick(size_t allowance = BUILD_WORK, bool build_precomputed = true, bool prepare_graphs = false);
	void TickPrecomputed(size_t allowance = BUILD_WORK);
	bool PreparePrecomputed(); ///< Initial trees or replacement graph barrier: one slice, without advancing StateTicks.
	bool IsPreparingPrecomputed() const;
	PassengerPrecomputedProgress GetPrecomputedProgress() const; ///< O(NUM_CARGO), no diagnostic copies or worker reads.
	uint64_t GetPrecomputedPreparationNanoseconds() const { return this->precomputed_preparation_nanoseconds; }
	void DiscardPrecomputed();
	bool PrecomputedFinished() const;
	bool HasRejectedPrecomputedMemory() const
	{ return std::any_of(this->precomputed.begin(), this->precomputed.end(), [](const auto &table) { return table.MemoryRejected(); }); }
	PassengerPrecomputedPlan PlanPrecomputed(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
			OrderListID excluded = OrderListID::Invalid(), uint32_t sample = 0, OrderListID failed = OrderListID::Invalid()) const;
	/** Display-only lookup; never calls Plan, Candidates, or a simulation work admission API. */
	std::vector<PassengerPrecomputedTable::RouteCandidate> InspectRoutes(CargoType cargo, StationID origin, StationID destination,
			OrderListID previous, OrderListID failed, size_t &work, const PassengerJourneyExecution *intent = nullptr) const;
	std::optional<int64_t> HyperpathServiceWait(const PassengerJourneyExecution &intent, CargoType cargo, OrderListID service) const;
	std::array<uint32_t, 2> HyperpathBoardingOccurrences(CargoType cargo, StationID origin, OrderListID service, VehicleOrderID order) const;
	bool MayBoardHyperpath(const PassengerJourneyExecution &intent, CargoType cargo, const std::array<uint32_t, 2> &occurrences, OrderListID previous, OrderListID failed, bool cached_only = true) const;
	/** PlanPrecomputed(cargo, origin, destination, now, previous) for callers that read only the plan's
	 * ride identity (presence, kind, generation, stations, signature), shared across ticks. With only
	 * Hyperpath tables that identity depends on the destination's rows alone, so it is invalidated by
	 * the lookup epoch and the destination's changes. Otherwise (a legacy ride's currency also depends
	 * on its service) nullptr: the caller plans itself. Valid until the next call. */
	const PassengerPrecomputedPlan *SharedHyperpathPlan(CargoType cargo, StationID origin, StationID destination, OrderListID previous) const;
	/** Cached-only membership as MayBoardHyperpath, but std::nullopt where a cold (or evicted) failed-carrier
	 * local policy leaves it unknown; MayBoardHyperpath admits such an unknown conservatively. */
	std::optional<bool> CachedHyperpathMembership(const PassengerJourneyExecution &intent, CargoType cargo, const std::array<uint32_t, 2> &occurrences, OrderListID previous, OrderListID failed) const;
	/** Derived boarding filter: both generations' stored policies are covered by their masks, so a
	 * destination without a set bit cannot pass base membership. Independent of the local mode. */
	bool HyperpathBoardingFilterAvailable(CargoType cargo) const
	{
		return cargo < NUM_CARGO && this->precomputed[cargo].BoardingMaskCovers() && this->retained_precomputed[cargo].BoardingMaskCovers();
	}
	/** Derived station lookup support. The epoch changes with table generations, effective
	 * retained clears, discards and load replay; every row publication or retirement in either
	 * table appends its destination to a bounded change log. Membership of a waiting policy
	 * group depends on nothing else. */
	static constexpr uint64_t HYPERPATH_DESTINATION_LOG = 1024;
	uint64_t HyperpathLookupEpoch() const { return this->hyperpath_lookup_epoch; }
	uint64_t HyperpathDestinationChanges() const { return this->hyperpath_destination_changes; }
	/** Destination of change number @p change; valid while HyperpathDestinationChanges() - change <= HYPERPATH_DESTINATION_LOG. */
	StationID HyperpathChangedDestination(uint64_t change) const { return this->hyperpath_destination_log[change % HYPERPATH_DESTINATION_LOG]; }
	static constexpr int HYPERPATH_LOOKUP_NONE = -1, HYPERPATH_LOOKUP_ALWAYS = 2;
	/** Every vehicle occurrence (in the returned table slot's numbering) for which the base membership
	 * of this waiting intent may hold at its station: NONE, a table slot 0/1 with out filled, or ALWAYS. */
	int HyperpathLookupCandidates(CargoType cargo, const PassengerJourneyExecution &intent, OrderListID arriving, uint64_t incarnation, StationID station, std::vector<uint32_t> &out) const;
	bool MayBeHyperpathCandidate(CargoType cargo, const std::array<uint32_t, 2> &occurrences, StationID destination, bool any_state) const
	{
		return this->precomputed[cargo].MayBeStoredCandidate(occurrences[0], destination, any_state) || this->retained_precomputed[cargo].MayBeStoredCandidate(occurrences[1], destination, any_state);
	}
	/** Local preference, absent from saves; every mode makes identical decisions. Off walks every
	 * policy group, On walks them with the destination mask, Index looks up only the groups whose
	 * stored policies contain the departure. Alternate picks Index or On per loading context, so
	 * both are timed in one process. Validate decides as Off and checks the Index superset. */
	enum class HyperpathFilterMode : uint8_t { Off, On, Alternate, Index, Validate };
	void SetHyperpathBoardingFilterMode(HyperpathFilterMode mode) { this->hyperpath_boarding_filter = mode; }
	void SetHyperpathBoardingFilterEnabled(bool enabled) { this->hyperpath_boarding_filter = enabled ? HyperpathFilterMode::On : HyperpathFilterMode::Off; }
	HyperpathFilterMode GetHyperpathBoardingFilterMode() const { return this->hyperpath_boarding_filter; }
	PassengerPrecomputedTable::DisplayRead NextHyperpathDisplayCandidate(CargoType cargo, const PassengerJourneyExecution &intent,
			OrderListID previous, OrderListID failed, PassengerPrecomputedTable::DisplayCursor &cursor,
			PassengerPrecomputedTable::DisplayCandidate &out, size_t &work) const;

	/** GUI-only read of the intent's current/retained published policy. Never replans a stale intent. */
	bool HyperpathDisplayCandidates(CargoType cargo, const PassengerJourneyExecution &intent, OrderListID previous, OrderListID failed,
			std::vector<PassengerPrecomputedTable::DisplayCandidate> &out, size_t &work) const;

	PassengerPrecomputedPlan BoardHyperpath(const PassengerJourneyExecution &intent, CargoType cargo, OrderListID service,
			VehicleOrderID boarding, OrderListID previous = OrderListID::Invalid(), OrderListID failed = OrderListID::Invalid()) const;
	size_t PrecomputedDestinations(CargoType cargo) const { return cargo < NUM_CARGO ? this->precomputed[cargo].GetDiagnostic().destinations : 0; } ///< No diagnostic copy.
	PassengerPrecomputedDiagnostic GetPrecomputedDiagnostic(CargoType cargo) const
	{
		auto result = this->precomputed[cargo].GetDiagnostic();
		const auto &old = this->retained_precomputed[cargo].GetDiagnostic();
		result.retained_destinations = old.destinations; result.retained_bytes = old.bytes; result.retained_envelope = old.envelope;
		return result;
	}
	PassengerParallelBuildDiagnostic GetPrecomputedParallelDiagnostic(CargoType cargo) const { return this->precomputed[cargo].GetParallelDiagnostic(); } ///< Current generation only.
	PassengerHyperpathMemoryDiagnostic InspectPrecomputedMemory(CargoType cargo, bool retained = false) const
	{ return (retained ? this->retained_precomputed[cargo] : this->precomputed[cargo]).InspectMemory(); }
	uint64_t GetPublishedRevision() const { return this->published_revision; }
	/** Whether a publication alone can change a waiting packet's precomputed plan. Legacy rides
	 * are validated against live service generations; Hyperpath plans change only with their table
	 * (row publication, retirement or a new generation), which wakes waiting lists itself. */
	bool RevisionAffectsWaitingPlans() const
	{
		return (this->precomputed_revision != UINT64_MAX && !this->precomputed_hyperpath) ||
				(this->retained_precomputed_revision != UINT64_MAX && !this->retained_precomputed_hyperpath);
	}
	auto BoardingRevision() const { return std::tuple(this->published_revision, this->boarding_revision, this->precomputed_operations); }
	PassengerPrecomputedTable::DestinationSpans PrecomputedReachableDestinations(CargoType cargo, StationID origin, bool retained = false) const
	{ return cargo < NUM_CARGO ? (retained ? this->retained_precomputed[cargo] : this->precomputed[cargo]).ReachableDestinations(origin) : PassengerPrecomputedTable::DestinationSpans{}; }
	std::optional<int64_t> PrecomputedRouteCost(CargoType cargo, StationID origin, StationID destination) const
	{
		if (cargo >= NUM_CARGO) return {};
		auto cost = this->precomputed[cargo].RouteCost(origin, destination);
		return cost ? cost : this->retained_precomputed[cargo].RouteCost(origin, destination);
	}
	OrderListID PrecomputedBoardingKey(CargoType cargo, OrderListID service) const { return this->precomputed[cargo].BoardingKey(service); }
	std::span<const OrderListID> PrecomputedBoardingServices(CargoType cargo, OrderListID service) const { return this->precomputed[cargo].BoardingServices(service); }
	struct BoardingKeys { std::array<std::span<const OrderListID>, 2> services; std::array<std::span<const uint64_t>, 2> sections; };
	BoardingKeys PrecomputedBoardingKeys(CargoType cargo, OrderListID service, VehicleOrderID boarding) const;
	bool RebindPrecomputed(PassengerJourneyExecution &ride, OrderListID service, CargoType cargo, VehicleOrderID boarding = INVALID_VEH_ORDER_ID) const;
	std::optional<PassengerJourneyExecution> ExtendPrecomputedRide(const PassengerJourneyExecution &ride, CargoType cargo, StationID destination) const;
	PassengerPeriodicPlan Plan(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
			PassengerJourneyLimits limits = {}, PassengerJourneyEvaluation evaluation = {}, OrderListID excluded = OrderListID::Invalid());
	/** Explicit read-only algorithm comparison, never a local cache preference or simulation setting. */
	PassengerPeriodicPlan PlanWithoutServicePruning(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
			PassengerJourneyLimits limits = {}, PassengerJourneyEvaluation evaluation = {}, OrderListID excluded = OrderListID::Invalid());
	/** Read-only data-structure comparison; retains service pruning and all logical budgets. */
	PassengerPeriodicPlan PlanWithLegacySearchStructures(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
			PassengerJourneyLimits limits = {}, PassengerJourneyEvaluation evaluation = {}, OrderListID excluded = OrderListID::Invalid());
	/** Read-only comparison with the previous pop-only completion rule, same budgets
	 * and search order. Never a local preference that could change peer decisions. */
	PassengerPeriodicPlan PlanWithLegacyCompletion(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
			PassengerJourneyLimits limits = {}, PassengerJourneyEvaluation evaluation = {}, OrderListID excluded = OrderListID::Invalid());
	/** Diagnostic only: original pop/pruning and generation handling, never a player setting. */
	PassengerPeriodicPlan PlanWithOriginalCompletion(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
			PassengerJourneyLimits limits = {}, PassengerJourneyEvaluation evaluation = {}, OrderListID excluded = OrderListID::Invalid());
	void SetTimedSearchScratchEnabled(bool enabled)
	{
		if (this->timed_scratch_enabled == enabled) return;
		this->timed_scratch_enabled = enabled;
		if (!enabled) this->timed_scratch.Release();
	}
	PassengerSchedulePlan PlanEstimated(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
			PassengerJourneyLimits limits = {}, PassengerJourneyEvaluation evaluation = {}, OrderListID excluded = OrderListID::Invalid())
	{ return this->schedule.Plan(*this, cargo, origin, destination, ready, excluded, limits, evaluation); }
	PassengerScheduleDiagnostic GetScheduleDiagnostic() const;
	struct StopRef { OrderListID service; size_t stop; auto operator<=>(const StopRef &) const = default; };
	const std::set<StopRef> &StopsAt(StationID station) const;
	const PassengerPeriodicService *Get(OrderListID id) const;
	bool IsCurrent(const PassengerJourneyExecution &ride) const;
	bool IsBuilding() const { return this->sweeping || !this->dirty.empty() || !this->build_queue.empty() || !this->building.empty(); }
	std::vector<uint8_t> Save() const;
	bool Load(std::span<const uint8_t> bytes);
	void FinishLoad(bool present, bool defer_precomputed = false);
	enum class LoadState : uint8_t { Ready, Replaying, Invalid };
	LoadState StepLoad(size_t allowance = BUILD_WORK); ///< Replay saved work only; never advance simulation or capture live input.
	LoadState GetLoadState() const { return this->load_state; }
	uint64_t GetLoadWork() const { return this->load_work; }
	uint64_t GetLoadProgress() const; ///< Tree operations plus capped, actually executed graph preparation steps.
	uint64_t GetLoadWorkLimit() const; ///< Upper bound for a peer replaying the current saved snapshot.
	PassengerDatabaseDiagnostic GetDiagnostic() const;
	void ResetDiagnostic() { this->diagnostic = {}; this->schedule.ResetDiagnostic(); this->recent_policy_builds.clear(); }
	void SetPolicyCacheEnabled(bool enabled) { this->policy_cache_enabled = enabled; if (!enabled) this->ClearPolicies(); }
	void ClearPolicies(); ///< Residency must never change search admission or its result.
	void SetDepartureIndexEnabled(bool enabled) { this->departure_index_enabled = enabled; if (!enabled) this->ClearDepartureIndices(); }
	void ClearDepartureIndices(); ///< Derived cache, absent from saved data and logical work/memory admission.
	void ShiftTicks(StateTicksDelta delta);
	void RecordExpiry(uint16_t passengers) { ++this->diagnostic.expired_packets; this->diagnostic.expired_passengers += passengers; }
	void RecordBoardingFailure() { ++this->diagnostic.boarding_failures; }
	PassengerDatabaseDiagnostic::Phase ExplainPhase(const Vehicle *vehicle, const PassengerPeriodicService &service) const;
	enum class Publication : uint8_t { Published, TimingDirty, Dirty, SweepPending, Unpublished };
	Publication GetPublication(OrderListID id) const;
private:
	friend struct PassengerServiceDatabaseTestAccess;
	friend class PassengerSchedulePlanner;
	PassengerSchedulePlanner schedule;
	std::array<PassengerPrecomputedTable, NUM_CARGO> precomputed;
	std::array<PassengerPrecomputedTable, NUM_CARGO> retained_precomputed;
	std::map<OrderListID, PassengerPeriodicService> retained_precomputed_services;
	uint64_t retained_precomputed_revision = UINT64_MAX;
	int64_t retained_precomputed_transfer = 0;
	std::map<OrderListID, PassengerPeriodicService> precomputed_services; ///< Frozen build input, saved independently of live publications.
	uint64_t precomputed_revision = UINT64_MAX, precomputed_operations = 0;
	int64_t precomputed_transfer = 0;
	StateTicks precomputed_refresh{};
	bool precomputed_invalidated = false;
	bool precomputed_hyperpath = false, retained_precomputed_hyperpath = false; ///< Frozen routing mode for the current and retained generations.
	uint64_t precomputed_policy_generation = 0, retained_precomputed_policy_generation = 0;
	uint64_t boarding_revision = 0; ///< Event-local validation invalidation; never saved or used for simulation admission.
	uint64_t precomputed_preparation_nanoseconds = 0; ///< Local elapsed observation, never saved or used for admission.
	PassengerBuildTask preparation_capture;
	PassengerBuildTask TickBuild(size_t allowance, bool build_precomputed, bool cooperative);
	bool cooperative_graph = false, load_graph_refreshed = false, load_current_graph_started = false;
	void BeginPrecomputed(int64_t transfer);
	void BuildPrecomputedTables();
	void ClearRetainedPrecomputed();
	void BuildPrecomputedTables(std::array<PassengerPrecomputedTable, NUM_CARGO> &tables,
			const std::map<OrderListID, PassengerPeriodicService> &snapshot, int64_t transfer, uint64_t remaining, bool hyperpath, bool cooperative = false);
	using Dependencies = std::map<StationID, uint64_t>;
	struct JourneyPolicy {
		std::vector<std::pair<StationID, int64_t>> lower_bounds; ///< Relaxed reverse adjacent-stop distances, never winning routes.
		size_t work = 0, bytes = 0, scratch_bytes = 0; ///< Replay the original cold tree search's logical memory, independently of compact residency.
		const int64_t *LowerBound(StationID station) const
		{
			auto it = std::lower_bound(this->lower_bounds.begin(), this->lower_bounds.end(), station, [](const auto &bound, StationID id) { return bound.first < id; });
			return it != this->lower_bounds.end() && it->first == station ? &it->second : nullptr;
		}
		bool limited = false;
		bool complete = false;
		int64_t unsettled_bound = 0;
		int64_t DistanceLowerBound(StationID station) const
		{
			if (const auto *lower = this->LowerBound(station)) return *lower;
			/* The frontier is certified on the normal origin-settled exit.
			 * A truncated expansion retains only settled station distances. */
			return !this->limited && !this->complete ? this->unsettled_bound : 0;
		}
		bool unknown_phase = false;
		PassengerPeriodicPlan::Reason diagnostic_limit = PassengerPeriodicPlan::Reason::NoCandidate;
		PassengerPeriodicPlan::Reason reason = PassengerPeriodicPlan::Reason::NoCandidate;
	};
	using PolicyKey = std::tuple<bool, CargoType, StationID, StationID, OrderListID, PassengerJourneyLimits, PassengerJourneyEvaluation>;
	using PolicyValue = std::variant<JourneyPolicy, PassengerSchedulePlan>;
	struct CachedPolicy {
		PolicyValue value;
		std::vector<std::pair<StationID, uint64_t>> dependencies; ///< Sorted compact stamps; no second tree retained.
		size_t bytes;
		std::list<PolicyKey>::iterator lru;
	};
	CachedPolicy *FindPolicy(const PolicyKey &key);
	void StorePolicy(const PolicyKey &key, PolicyValue value, Dependencies dependencies, size_t bytes);
	void ErasePolicy(std::map<PolicyKey, CachedPolicy>::iterator it);
	uint64_t StationRevision(StationID station) const;
	void TouchStations(const PassengerPeriodicService &service);
	void Publish(size_t &allowance);
	void ClearPolicies(PassengerDatabaseDiagnostic::ClearReason reason);
	std::map<OrderListID, PassengerPeriodicService> services; ///< Published network only.
	std::vector<const PassengerPeriodicService *> service_lookup; ///< Derived dense pool-ID lookup, rebuilt before simulation resumes.
	std::bitset<65536> unsafe_lookup; ///< Same saved unsafe set, allocation-free membership for packet lookups.
	std::map<OrderListID, PassengerPeriodicService> building; ///< Completed tasks awaiting the publication barrier.
	std::set<OrderListID> build_queue, unsafe;
	std::map<OrderListID, StateTicks> build_since, staged_since;
	std::vector<uint64_t> station_revisions; ///< Optional derived policy stamps, cold after load.
	StateTicks batch_started{};
	bool publishing = false, bootstrap = true;
	uint64_t published_revision = 0;
	size_t building_bytes = 0;
	std::map<StationID, std::set<StopRef>> stations;
	std::set<OrderListID> dirty;
	std::map<OrderListID, StateTicks> dirty_since;
	StateTicks sweeping_since{};
	uint32_t cursor = 0; ///< Bounded pool-ID sweep; no national discovery per query/tick.
	uint16_t dirty_cursor = 65534; ///< Rotating deterministic dirty queue; repeated edits cannot starve higher IDs.
	bool sweeping = true, restoring = false;
	LoadState load_state = LoadState::Ready;
	bool load_retained = false;
	CargoType load_cargo{0};
	uint64_t load_remaining = 0, load_work = 0;
	uint64_t generation = 0;
	size_t bytes = 0, policy_bytes = 0;
	bool policy_cache_enabled = true; ///< Local optional accelerator; absent from saves and routing admission.
	HyperpathFilterMode hyperpath_boarding_filter = HyperpathFilterMode::Index; ///< Local derived boarding filter; identical decisions, absent from saves.
	uint64_t hyperpath_lookup_epoch = 1, hyperpath_destination_changes = 0; ///< Derived; monotone across resets and loads.
	std::array<StationID, HYPERPATH_DESTINATION_LOG> hyperpath_destination_log{}; ///< Derived ring indexed by change number.
	std::vector<uint64_t> hyperpath_destination_changed; ///< Derived: change count just after each destination's latest change.
	void BumpHyperpathDestination(StationID destination)
	{
		this->hyperpath_destination_log[this->hyperpath_destination_changes % HYPERPATH_DESTINATION_LOG] = destination; ++this->hyperpath_destination_changes;
		if (destination.base() >= this->hyperpath_destination_changed.size()) this->hyperpath_destination_changed.resize(destination.base() + 1, 0);
		this->hyperpath_destination_changed[destination.base()] = this->hyperpath_destination_changes;
	}
	/* Derived direct-mapped caches of plans that depend only on Hyperpath rows, never saved. A slot is
	 * valid for its key while the lookup epoch is unchanged and its destination did not change since. */
	struct RowPlanSlot { std::array<uint64_t, 2> key{}; uint64_t epoch = 0, changes = 0; PassengerPrecomputedPlan plan; };
	static constexpr size_t ROW_PLAN_SLOTS = size_t{1} << 14;
	mutable std::vector<RowPlanSlot> shared_hyperpath_plans, shared_hyperpath_boards;
	RowPlanSlot &RowPlan(std::vector<RowPlanSlot> &cache, const std::array<uint64_t, 2> &key, StationID destination, bool &valid) const;
	std::list<PolicyKey> policy_lru;
	std::map<PolicyKey, CachedPolicy> policies;
	std::map<std::tuple<bool, CargoType, StationID, StationID>, uint64_t> recent_policy_builds; ///< Bounded diagnostic history, never simulation input.
	PassengerDatabaseDiagnostic diagnostic;
	PassengerTimedSearchScratch timed_scratch;
	bool timed_scratch_enabled = true; ///< Local storage reuse preference, never routing input or saved state.
	static constexpr size_t DEPARTURE_INDEX_BYTES = 8 * 1024 * 1024;
	std::map<std::pair<OrderListID, CargoType>, PassengerDepartureIndex> departure_indices;
	size_t departure_index_bytes = 0;
	bool departure_index_enabled = true; ///< Local preference survives Load; neither preference nor derived rows are saved.
	const PassengerDepartureIndex *DepartureIndex(const PassengerPeriodicService &service, CargoType cargo);
	void Remove(OrderListID id);
	void Index(const PassengerPeriodicService &service);
	JourneyPolicy CompilePolicy(CargoType cargo, StationID origin, StationID destination, PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, Dependencies &dependencies) const;
	PassengerPeriodicPlan PlanInternal(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
			PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, OrderListID excluded, bool prune_services, bool legacy_structures = false, bool anytime = true, bool validate_completes = true);
	bool ServiceCannotImprove(const PassengerPeriodicTimetable &pattern, size_t boarding, CargoType cargo,
			StationID destination, int64_t cost, bool transfer, int64_t best, const JourneyPolicy &policy, PassengerJourneyEvaluation evaluation);
};

PassengerServiceDatabase &GetPassengerServiceDatabase();
void InvalidatePassengerServiceDatabase(OrderListID id = OrderListID::Invalid());
void ResetPassengerServiceDatabase();
void ObservePassengerPeriodicVehicle(const Vehicle *vehicle);
/** Live path/facility/cargo verification; no physical timing predictions. */
bool ValidatePassengerServiceRide(const PassengerJourneyExecution &ride, const Vehicle *vehicle, CargoType cargo, bool boarding);

/** Explicit diagnostic only: both planners read the same pools/StateTicks. No
 * Tick, packets, quota admission, RNG, or live periodic cache is modified. */
struct PassengerPlannerComparison {
	PassengerPeriodicPlan periodic;
	bool legacy_found = false;
	std::vector<PassengerJourneyExecution> legacy_route;
	StateTicks legacy_arrival{};
	int64_t legacy_cost = 0;
	PassengerJourneyStopReason legacy_stop = PassengerJourneyStopReason::InvalidQuery;
	PassengerPredictionStatus legacy_status = PassengerPredictionStatus::Unknown;
	size_t legacy_graph_work = 0, legacy_search_work = 0;
	Ticks horizon = 0;
};
PassengerPlannerComparison ComparePassengerPlanners(const PassengerServiceDatabase &database, CargoType cargo,
		StationID origin, StationID destination, StateTicks ready, PassengerJourneyLimits limits = {}, PassengerJourneyEvaluation evaluation = {});

#endif /* PASSENGER_SERVICE_DATABASE_H */
