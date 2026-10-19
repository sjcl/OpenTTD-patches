/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_journey_active.h Simulation-only execution of passenger journeys. */
#ifndef PASSENGER_JOURNEY_ACTIVE_H
#define PASSENGER_JOURNEY_ACTIVE_H

#include "cargopacket.h"
#include "passenger_journey_planner.h"
#include "passenger_query_admission.h"
#include "passenger_precomputed.h"

bool IsPassengerJourneyRoutingEnabled(CargoType cargo);
uint64_t PassengerJourneyServiceSignature(const OrderList &orders, CargoType cargo, size_t *budget = nullptr);
void PassengerJourneySettingsChanged(int32_t);
class PassengerJourneyLoadingContext;
void RouteWaitingPassengerJourneys(Station *station, PassengerJourneyLoadingContext *loading = nullptr);
void AfterLoadPassengerJourneys();
static constexpr size_t PASSENGER_ROUTING_TICK_WORK = 8192;
void RunPassengerWaitingEvents(); ///< National due queue; never enumerates Station::Iterate().
void RebuildPassengerWaitingScheduler(); ///< Load/clock rebase only; restores the debit from saved goods entries.
bool ConsumePassengerRoutingWork();
void WakePassengerWaitingCargo(CargoType cargo); ///< A newly published row can resolve dormant ODs.
enum class PassengerWaitingCause : uint8_t { New, Expiry, Failure, Migration, Generation, End };
struct PassengerWaitingEventDiagnostic {
	uint64_t pending = 0, overdue = 0, oldest_ticks = 0, maximum_station_backlog = 0;
	uint64_t on_time = 0, packets = 0, groups = 0, scheduler_microseconds = 0;
	uint64_t arrival_candidates = 0, arrival_passengers = 0, unresolved_missed = 0;
	uint64_t capacity_missed_packets = 0, capacity_missed_passengers = 0;
	uint64_t capacity_record_nanoseconds = 0; ///< Outside the national scheduler; includes candidate visits and index/event mutation.
	uint64_t unchanged_generation_packets = 0; ///< Unchanged fallback decision; only retry/merge metadata is refreshed.
	/* Hyperpath boarding enumeration: policy groups whose predicate ran, groups skipped by the
	 * derived destination filter, predicates that admitted a group, and walks with/without the filter. */
	uint64_t hyperpath_groups_checked = 0, hyperpath_groups_skipped = 0, hyperpath_groups_admitted = 0;
	/* Indexed by strategy: complete walk, masked walk, occurrence lookup. Groups are those visited. */
	std::array<uint64_t, 3> hyperpath_walks{}, hyperpath_walk_groups{}, hyperpath_walk_nanoseconds{};
	uint64_t hyperpath_lookup_rebuilds = 0, hyperpath_lookup_indexed_groups = 0; ///< Lookup maintenance.
	uint64_t hyperpath_index_validated = 0, hyperpath_index_violations = 0; ///< Validate mode: admitted groups missing from the lookup.
	/* Group pre-filter memo: answers reused for another coach/pass of the same consist. Validate
	 * mode recomputes each kept answer: a disagreement is conservative when only an evicted
	 * failed-carrier policy made the recomputation unknown and the exact answer equals the kept
	 * one, otherwise a violation. Duplicates: a Hyperpath group reachable twice in one lookup walk. */
	uint64_t hyperpath_memo_hits = 0, hyperpath_memo_validated = 0, hyperpath_memo_conservative = 0, hyperpath_memo_violations = 0;
	uint64_t hyperpath_lookup_duplicates = 0;
	std::array<uint64_t, static_cast<size_t>(PassengerWaitingCause::End)> cause_nanoseconds{}, cause_packets{};
};
void RecordPassengerWaitingCause(PassengerWaitingCause cause, uint64_t nanoseconds, bool unchanged = false);
PassengerWaitingEventDiagnostic GetPassengerWaitingEventDiagnostic();
void ResetPassengerWaitingEventDiagnostic();
void RecordPassengerWaitingEvent(StateTicks due);
void RecordPassengerWaitingGroup();
void RecordPassengerWaitingArrival(uint64_t packets, uint64_t passengers, bool missed);
size_t InvalidatePassengerJourneyVehicle(VehicleID vehicle);
void RebuildPassengerJourneyVehicleIndex();
void ClearPassengerOdFallbackCache(); ///< Derived only; cold/warm admission and decisions are identical.
void SetPassengerOdFallbackCacheEnabled(bool enabled); ///< Local derived-cache preference; never saved or synchronized.
void SetPassengerOdFallbackBindingValidation(bool enabled); ///< Diagnostic reference path: physically check resident nodes.

enum class PassengerJourneyFallbackReason : uint8_t {
	GraphWork, GraphMemory, SnapshotLimit, QueryWork, QueryLimit, QueryMemory, PredictionIncomplete, PredictionHorizon, NoJourney, InvalidLeg,
	StaleLeg, Disabled, VehicleDeleted, End,
};

struct PassengerJourneyQueryDetail {
	StationID origin = StationID::Invalid(), destination = StationID::Invalid();
	StateTicks at{};
	PassengerPredictionStatus status{};
	PassengerJourneyStopReason stop{};
	size_t candidates = 0;
	std::optional<StateTicksDelta> score;
	std::optional<StateTicks> unknown_arrival_bound;
	OrderListID unknown_service = OrderListID::Invalid();
};

enum class PassengerScheduleInvalidation : uint8_t { ServiceMissing, Incarnation, Generation, Station, Disabled, VehicleDeleted, LiveRide, ForcedUnload, WaitingExpired, End };
void RecordPassengerScheduleInvalidation(PassengerScheduleInvalidation reason);

struct PassengerJourneyTickDiagnostic {
	struct Admission {
		struct RetrySample { CargoType cargo; StationID origin, destination; uint64_t attempts; };
		std::array<RetrySample, 16> retry_samples{};
		size_t retry_sample_count = 0;
		uint64_t accepted = 0, rejected = 0, reserved = 0, used = 0, returned = 0;
		uint64_t timed_allocated = 0, schedule_allocated = 0, timed_used = 0, schedule_used = 0;
		uint64_t work_stopped = 0, work_no_route = 0, schedule_rescued = 0;
		uint64_t complete_unproved = 0, incomplete_work = 0, missing_published = 0, missing_building = 0;
		uint64_t retries = 0, maximum_od_attempts = 0, retry_tracking_evictions = 0;
		uint64_t maximum_tick_queries = 0, maximum_tick_work = 0;
		std::array<uint64_t, static_cast<size_t>(PassengerQueryAdmission::Rejection::End)> rejections{};
		std::array<uint64_t, 65> tick_queries{}; ///< Exact OD admission counts; zero includes idle simulation ticks.
		std::array<uint64_t, 65> tick_work{}; ///< 16384-Work upper bounds, including idle simulation ticks.
	} admission;
	std::array<size_t, static_cast<size_t>(PassengerGraphFailure::End)> graph_failures{};
	std::array<size_t, static_cast<size_t>(PassengerPredictionStop::End)> prediction_stops{};
	PassengerGraphFailureDetail first_graph_failure;
	PassengerTripPredictionDetail first_prediction_failure;
	OrderListID opaque_service = OrderListID::Invalid();
	VehicleID opaque_vehicle = VehicleID::Invalid();
	size_t complete_graphs = 0, admitted_visits = 0, eligible_services = 0, selected_services = 0, selected_trips = 0, largest_block_services = 0;
	Ticks maximum_unpredicted_ticks = 0;
	size_t builds = 0, static_cache_hits = 0, queries = 0, query_cache_hits = 0;
	size_t trains = 0, cargo_capable = 0, possible_refit = 0, predicted = 0, visits = 0;
	size_t excluded_cargo = 0, excluded_scope = 0, inactive = 0, omitted_trains = 0, truncated_predictions = 0;
	size_t graph_work = 0, query_work = 0, maximum_graph_work = 0;
	size_t reused_graph_work = 0, static_allocation_bytes = 0, query_scratch_bytes = 0, query_cache_bytes = 0;
	size_t journeys = 0, passengers = 0, graph_memory_bound = 0, graph_allocation_bytes = 0;
	uint64_t build_microseconds = 0, maximum_build_microseconds = 0;
	PassengerGraphPhaseTimes graph_phase_microseconds{};
	size_t capture_discovery_hits = 0, capture_service_hits = 0, capture_reused_work = 0, capture_allocation_bytes = 0;
	size_t prediction_executions = 0, prediction_cache_hits = 0, prediction_work = 0, prediction_executed_visits = 0, prediction_cache_bytes = 0;
	uint64_t prediction_nanoseconds = 0;
	uint64_t query_microseconds = 0, maximum_tick_microseconds = 0;
	std::array<size_t, static_cast<size_t>(PassengerJourneyFallbackReason::End)> fallbacks{};
	std::array<size_t, static_cast<size_t>(PassengerJourneyFallbackReason::End)> deferred{};
	size_t fair_share_work = 0, fair_share_claims = 0;
	size_t fair_share_releases = 0, retained_rides = 0;
	size_t od_fallback_work = 0, od_fallback_searches = 0;
	size_t od_fallback_cache_hits = 0, od_fallback_cache_bytes = 0;
	size_t od_fallback_binding_hits = 0, od_fallback_binding_builds = 0, od_fallback_binding_nodes = 0;
	size_t od_fallback_reverse_hits = 0, od_fallback_reverse_builds = 0;
	uint64_t od_fallback_binding_validation_nanoseconds = 0, od_fallback_binding_build_nanoseconds = 0, od_fallback_reverse_build_nanoseconds = 0;
	size_t boarded_passengers = 0, transferred_passengers = 0, delivered_passengers = 0;
	size_t depot_service_boarded_passengers = 0, depot_service_delivered_passengers = 0;
	size_t depot_crossing_boarded_passengers = 0, depot_crossing_delivered_passengers = 0; ///< Actual timed moves on a leg whose order suffix includes a depot.
	size_t timing_uncertainty = 0, path_uncertainty = 0, uncertainty_resolved = 0;
	size_t timed_routes_ignoring_unknown = 0, timed_budget_deferred = 0;
	size_t timed_certified_packets = 0, timed_provisional_packets = 0, timed_provisional_passengers = 0;
	size_t untimed_routes = 0, untimed_no_route = 0, untimed_budget_deferred = 0;
	size_t untimed_boarded_passengers = 0, untimed_transferred_passengers = 0, untimed_delivered_passengers = 0;
	size_t schedule_packets = 0, schedule_passengers = 0, schedule_deferred_packets = 0;
	size_t schedule_boarded_passengers = 0, schedule_transferred_passengers = 0, schedule_delivered_passengers = 0;
	size_t schedule_connected_passengers = 0, timed_connected_passengers = 0; ///< Next actual boarding after an observed alighting, by previous carrier.
	size_t schedule_assisted_delivered_passengers = 0; ///< Final OD delivery after any observed real schedule leg, including a Timed final carrier.
	size_t schedule_expired = 0, schedule_stale = 0;
	std::array<size_t, static_cast<size_t>(PassengerScheduleInvalidation::End)> schedule_invalidations{};
	uint64_t schedule_expected_wait = 0, schedule_actual_wait = 0, schedule_wait_samples = 0; ///< Passenger-weighted tick durations.
	std::array<PassengerJourneyQueryDetail, 16> query_samples{};
	size_t query_sample_count = 0;
	uint64_t simulation_ticks = 0, simulation_microseconds = 0, maximum_simulation_microseconds = 0;
	uint64_t journey_microseconds = 0, maximum_journey_microseconds = 0;
	uint64_t waiting_microseconds = 0, fallback_distance_microseconds = 0, boarding_microseconds = 0; ///< Inclusive diagnostic scopes; do not sum them.
	std::array<uint64_t, 2048> simulation_histogram{}, journey_histogram{}; ///< 1 ms buckets; last bucket includes overflow.
};

/** Nested observations count elapsed time only at the outermost Journey call. */
class PassengerJourneyWorkTimer {
	uint64_t started;
public:
	PassengerJourneyWorkTimer();
	~PassengerJourneyWorkTimer();
};

/** Reset at the simulation tick boundary, including the first tick after network join/load.
 * Budget counters are simulation state; cache residency and elapsed diagnostics never affect them. */
void BeginPassengerJourneyTick();
void PreparePassengerJourneyLoading(); ///< Before any station countdowns/queues change this tick.
void ReleasePassengerJourneyWaitingShare(StationID station); ///< End of this station's waiting pass, including skipped passes.
const PassengerJourneyTickDiagnostic &GetPassengerJourneyTickDiagnostic();
PassengerJourneyTickDiagnostic GetPassengerJourneyTotalDiagnostic();
void ResetPassengerJourneyDiagnostic(); ///< Local observations only; never resets simulation allowances.
void RecordPassengerJourneyFallback(PassengerJourneyFallbackReason reason); ///< Simulation-only lifecycle observations.
enum class PassengerJourneyMovement : uint8_t { Board, Transfer, Deliver };
void RecordPassengerJourneyMovement(PassengerJourneyMovement movement, uint count, bool timed = true); ///< Actual cargo moves only, never reservations/staging.
void RecordPassengerJourneyMovement(PassengerJourneyMovement movement, const CargoPacket &packet, uint count);
void RecordPassengerJourneySimulationTick(uint64_t microseconds); ///< Read-only elapsed CPU-thread time, outside all routing budgets.
PassengerServiceGraphLimits ActivePassengerServiceGraphLimits();

/** Lazy prediction for one physical passenger service/cargo in one loading pass. */
class PassengerJourneyBoardingContext {
	const Vehicle *vehicle;
	CargoType cargo;
	mutable bool initialized = false;
	mutable uint64_t signature = 0;
	mutable PassengerVisitPrediction prediction;
	mutable size_t predictions = 0;
	mutable std::map<std::tuple<OrderListID, uint64_t, VehicleOrderID, VehicleOrderID, StationID, StationID, bool>, bool> service_rides;
	mutable std::map<std::pair<PassengerJourneyExecution, StationID>, std::optional<PassengerJourneyExecution>> extensions;
	mutable std::tuple<uint64_t, uint64_t, uint64_t> extension_revision{};
	void Initialize() const;
	bool ValidateServiceRide(const PassengerJourneyExecution &ride, bool boarding) const;
public:
	explicit PassengerJourneyBoardingContext(const Vehicle *vehicle, CargoType cargo = INVALID_CARGO);
	bool CanBoard(const CargoPacket &packet) const;
	bool CanBoardIntent(const PassengerJourneyExecution &ride) const { return this->ValidateServiceRide(ride, true); }
	void ExtendRide(PassengerJourneyExecution &ride, StationID destination) const;
	bool CanContinue(const CargoPacket &packet) const;
	size_t GetPredictionCount() const { return this->predictions; }
};

/** Untimed fallback, local to one arrival/loading pass. Prefer the current CargoDist
 * hop and a live order prefix; OD recovery uses unrestricted directed LinkGraph links.
 * No timing, optimum, future capacity or permanent unreachability is asserted. */
class PassengerOdFallbackContext {
	using BindingNodes = std::vector<std::pair<StationID, bool>>;
	struct Binding { std::shared_ptr<const BindingNodes> nodes; uint64_t token; };
	std::map<std::tuple<LinkGraphID, uint64_t, uint64_t, NodeID, CargoType>, Binding> bindings;
	struct Distances {
		LinkGraphID graph = LinkGraphID::Invalid();
		std::shared_ptr<const std::vector<uint32_t>> hops;
	};
	std::map<std::pair<CargoType, StationID>, Distances> distances;
	std::map<std::pair<VehicleID, CargoType>, std::vector<StationID>> stops;
	std::map<std::tuple<VehicleID, CargoType, StationID, StationID>, StationID> routes;
	size_t distance_bytes = 0;
	size_t work_left = 128 * 1024; ///< Includes live state/facility capture and the 4096-order prefix.
	bool resource_limited = false;
	bool Spend(size_t amount = 1);
	bool CanReach(const Vehicle *vehicle, CargoType cargo, StationID target);
	uint32_t Distance(CargoType cargo, StationID origin, StationID destination);
	StationID ProgressingVia(const Vehicle *vehicle, CargoType cargo, StationID destination);
	StationID BoardingVia(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo);
public:
	explicit PassengerOdFallbackContext(size_t work_allowance = 128 * 1024) : work_left(std::min(work_allowance, size_t{128 * 1024})) {}
	std::span<const StationID> BoardingStops(const Vehicle *vehicle, CargoType cargo);
	bool CanBoard(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo);
	void PrepareBoard(CargoPacket &packet, const Vehicle *vehicle, CargoType cargo);
	void Invalidate(const Vehicle *vehicle);
	std::optional<VehicleCargoList::MoveToAction> Decide(CargoPacket &packet, const Vehicle *vehicle,
			CargoType cargo, StationID station, bool accepted, OrderUnloadType unload);
};

/** Owned by LoadUnloadStation, shared by validation, refit queries, reservations and all coaches.
 * Refit invalidates the affected consist; nothing is retained across simulation events. */
class PassengerJourneyLoadingContext {
	std::map<std::pair<VehicleID, CargoType>, PassengerJourneyBoardingContext> boarding;
	PassengerOdFallbackContext fallback;
	bool waiting_events = false;
	using BoardingState = std::tuple<PassengerRoutingMode, StationID, StationID, StateTicks, OrderListID, uint64_t, bool, PassengerBoardingFailure>;
	struct Decision { StateTicks tick; std::tuple<uint64_t, uint64_t, uint64_t> revision; bool allowed; };
	std::map<std::pair<VehicleID, CargoType>, std::map<std::pair<BoardingState, PassengerJourneyExecution>, Decision>> decisions;
	bool CanBoardUncached(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo);
	size_t validations = 0, cache_hits = 0;
	using PendingKey = std::tuple<CargoType, StationID, StationID, StateTicks, OrderListID, OrderListID>;
	std::map<PendingKey, PassengerPrecomputedPlan> pending_plans;
	std::tuple<uint64_t, uint64_t, uint64_t> pending_revision{};
	const PassengerPrecomputedPlan &PendingPlan(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo);
	using HyperpathKey = std::tuple<VehicleID, CargoType, VehicleOrderID, uint64_t, StationID, StationID, OrderListID, OrderListID>;
	std::map<HyperpathKey, PassengerPrecomputedPlan> hyperpath_rides;
	using HyperpathBoardingKey = std::tuple<VehicleID, CargoType, OrderListID, VehicleOrderID, StationID>;
	std::map<HyperpathBoardingKey, std::array<uint32_t, 2>> hyperpath_boardings;
	using HyperpathMembershipKey = std::tuple<HyperpathKey, OrderListID, uint64_t, std::array<uint32_t, 2>>;
	std::map<HyperpathMembershipKey, bool> hyperpath_memberships; ///< Base membership only, local to this loading context.
	uint64_t hyperpath_memo_serial;
	StateTicks hyperpath_memo_ticks{}; ///< A context normally lives within one tick; a later tick renews the serial.
	using RetiredPlanKey = std::tuple<CargoType, StationID, StationID, StateTicks, OrderListID>;
	std::map<RetiredPlanKey, PassengerPrecomputedPlan> retired_plans; ///< Plans replacing a retired Hyperpath intent.
	const PassengerPrecomputedPlan &RetiredPlan(CargoType cargo, const PassengerJourneyExecution &intent, OrderListID previous);
	void RefreshBoardingRevision(); ///< Clears every revision-bound memo when the database's boarding revision moved.
	std::optional<PassengerJourneyExecution> ResolveHyperpath(const PassengerJourneyExecution &intent, const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo, bool override_failure = false, OrderListID failed = OrderListID::Invalid());
public:
	PassengerJourneyLoadingContext();
	/** Identity of this context's revision-bound memos, renewed whenever they are cleared or a
	 * consist is invalidated. Unique per process; only compared for equality, never ordering. */
	uint64_t HyperpathMemoSerial();
	void EnableWaitingEvents() { this->waiting_events = true; } ///< Actual station simulation only; read-only predicates never wake packets.
	int8_t hyperpath_filter_choice = -1; ///< Alternate boarding-filter diagnostic: fixed at this context's first Hyperpath walk.
	bool CanBoard(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo = INVALID_CARGO);
	const std::array<uint32_t, 2> &HyperpathBoardingOccurrences(const Vehicle *vehicle, CargoType cargo);
	/** Conservative group pre-filter. @p certain (optional) is cleared when a cold failed-carrier
	 * policy was admitted as unknown; @p exact builds that policy instead (validation only). */
	bool MayUseHyperpathPolicy(const CargoPacket &representative, const Vehicle *vehicle, CargoType cargo, const std::array<uint32_t, 2> &occurrences, bool base, bool adjusted,
			bool *certain = nullptr, bool exact = false);
	int64_t ExpectedHyperpathWait(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo);
	bool CanUseHyperpathPolicy(const CargoPacket &representative, const Vehicle *vehicle, CargoType cargo, bool base, bool adjusted);
	void ResolveWaiting(CargoPacket &packet, const Vehicle *vehicle); ///< Selected indexed candidate only, detached before changing its cohort.
	void ObserveWaitingGroup(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo, size_t packets);
	void PrepareBoard(CargoPacket &packet, const Vehicle *vehicle);
	void Invalidate(const Vehicle *vehicle);
	size_t GetPredictionCount() const;
	size_t GetValidationCount() const { return this->validations; }
	size_t GetBoardingCacheHits() const { return this->cache_hits; }
	std::span<const StationID> BoardingStops(const Vehicle *vehicle, CargoType cargo) { return this->fallback.BoardingStops(vehicle, cargo); }
};

/** Shares snapshots/query caches across all coaches and packets at an arrival. */
class PassengerJourneyActiveContext {
	struct Snapshot;
	PassengerServiceCapture capture;
	const Vehicle *vehicle;
	StationID station;
	std::map<std::tuple<CargoType, StationID, Ticks>, std::unique_ptr<Snapshot>> snapshots;
	std::map<CargoType, PassengerJourneyBoardingContext> continuation;
	struct PeriodicWaiting {
		std::optional<PassengerJourneyExecution> ride;
		bool deferred, provisional;
		PassengerJourneyFallbackReason failure = PassengerJourneyFallbackReason::QueryWork;
	};
	std::map<std::tuple<CargoType, StationID, StateTicks, OrderListID>, PeriodicWaiting> periodic_waiting;
	std::map<std::tuple<CargoType, StationID, StateTicks, OrderListID, OrderListID>, PassengerPrecomputedPlan> precomputed_waiting;
	PassengerOdFallbackContext fallback;
	size_t work_left = 4194304;
	size_t memory_left = 256 * 1024 * 1024;
	size_t queries_left = 128;
	PassengerJourneyFallbackReason failure = PassengerJourneyFallbackReason::GraphWork;
	bool deferred = false;
	Snapshot *GetSnapshot(CargoType cargo, StationID destination, Ticks horizon = 4096);
	std::optional<PassengerJourneyExecution> Execute(Snapshot &snapshot, CargoType cargo, const PassengerJourneyLeg &leg);
	bool AdmitQuery();
public:
	/** A zero prediction cache allowance supports paired diagnostics; logical allowances stay identical. */
	PassengerJourneyActiveContext(const Vehicle *vehicle, StationID station, size_t prediction_cache_bytes = PassengerServiceCapture::MAXIMUM_PREDICTION_BYTES);
	~PassengerJourneyActiveContext();
	size_t GetWorkAllowanceUsed() const { return 4194304 - this->work_left; }
	const PassengerPrecomputedPlan &QueryWaiting(const CargoPacket &packet, CargoType cargo); ///< Shared frozen-table lookup; never changes a packet or admits a search.
	bool PlanWaiting(CargoPacket &packet, CargoType cargo, StationID legacy_hop, bool require_hyperpath = false);
	std::optional<VehicleCargoList::MoveToAction> Decide(CargoPacket &packet, CargoType cargo, bool accepted, OrderUnloadType unload);
};

/** Available waiting counts only. Reservations are reported separately by StationCargoList. */
std::array<uint, 3> CountPassengerRoutingModes(const StationCargoList &cargo);
std::array<uint, 3> CountReservedPassengerRoutingModes(const Station &station, CargoType cargo);

#endif /* PASSENGER_JOURNEY_ACTIVE_H */
