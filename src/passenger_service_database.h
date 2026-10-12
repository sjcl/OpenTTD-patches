/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_service_database.h Persistent periodic services; no expanded future visits. */
#ifndef PASSENGER_SERVICE_DATABASE_H
#define PASSENGER_SERVICE_DATABASE_H

#include "passenger_journey_planner.h"
#include "passenger_journey_type.h"
#include <set>

struct PassengerPeriodicStop {
	StationID station = StationID::Invalid();
	VehicleOrderID order = INVALID_VEH_ORDER_ID;
	int64_t arrival = 0, departure = 0;
	uint64_t board = 0, alight = 0, forced = 0; ///< Cargo-slot masks, independent of cargo labels.
	bool operator==(const PassengerPeriodicStop &) const = default;
};

/** Offsets cover exactly one circuit, including the travel back to the first order.
 * Missing flags, unsupported controls and zero-length circuits remain untimed. */
struct PassengerPeriodicTimetable {
	std::vector<PassengerPeriodicStop> stops;
	int64_t period = 0;
	enum class Failure : uint8_t { None, Control, Refit, Travel, Wait, Overflow, ZeroPeriod, TooFewStops, End };
	static PassengerPeriodicTimetable Compile(std::span<const Order> orders, Failure *failure = nullptr);
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
	bool operator==(const PassengerPeriodicService &) const = default;
};

struct PassengerPeriodicPlan {
	std::optional<PassengerJourneyExecution> ride;
	StateTicks arrival{};
	int64_t cost = 0;
	size_t transfers = 0;
	size_t work = 0;
	bool deferred = false;
	enum class Reason : uint8_t { Found, Invalid, OriginMissing, DestinationMissing, NoAlighting, Unreachable, NoCandidate,
		UnknownPhase, Evaluation, Candidates, Labels, Work, Memory, Transfers, End };
	Reason reason = Reason::Invalid;
	std::array<bool, static_cast<size_t>(Reason::End)> observations{}; ///< Overlapping observations, once per OD call.
	std::vector<PassengerJourneyExecution> route; ///< Selected path, also used by explicit read-only comparisons.
	size_t labels = 0, candidates = 0, memory = 0;
};

struct PassengerDatabaseDiagnostic {
	uint64_t builds = 0, build_work = 0, build_microseconds = 0;
	uint64_t searches = 0, query_microseconds = 0, next_departures = 0, policy_hits = 0, policy_misses = 0;
	size_t services = 0, timed_services = 0, vehicles = 0, bytes = 0, policy_bytes = 0, pending = 0;
	size_t known_phase_vehicles = 0;
	uint64_t invalidations = 0, global_invalidations = 0, rejected_work = 0, rejected_memory = 0;
	uint64_t maximum_pending_ticks = 0;
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
	static constexpr size_t POLICY_BYTES = 16 * 1024 * 1024, POLICY_ENTRIES = 4096;
	static constexpr int64_t TIMETABLE_EPOCH = 1024;
	void Reset();
	void Invalidate(OrderListID id = OrderListID::Invalid());
	void TimetableChanged(OrderListID id); ///< Keep safe published timing until the next deterministic update epoch.
	void Tick(size_t allowance = BUILD_WORK);
	PassengerPeriodicPlan Plan(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
			PassengerJourneyLimits limits = {}, PassengerJourneyEvaluation evaluation = {});
	const PassengerPeriodicService *Get(OrderListID id) const;
	bool IsCurrent(const PassengerJourneyExecution &ride) const;
	bool IsBuilding() const { return this->sweeping || !this->dirty.empty(); }
	std::vector<uint8_t> Save() const;
	bool Load(std::span<const uint8_t> bytes);
	void FinishLoad(bool present);
	PassengerDatabaseDiagnostic GetDiagnostic() const;
	void ResetDiagnostic() { this->diagnostic = {}; }
	void ClearPolicies(); ///< Residency must never change search admission or its result.
	void ShiftTicks(StateTicksDelta delta);
	void RecordExpiry(uint16_t passengers) { ++this->diagnostic.expired_packets; this->diagnostic.expired_passengers += passengers; }
	void RecordBoardingFailure() { ++this->diagnostic.boarding_failures; }
	PassengerDatabaseDiagnostic::Phase ExplainPhase(const Vehicle *vehicle, const PassengerPeriodicService &service) const;
	enum class Publication : uint8_t { Published, TimingDirty, Dirty, SweepPending, Unpublished };
	Publication GetPublication(OrderListID id) const;
private:
	struct StopRef { OrderListID service; size_t stop; auto operator<=>(const StopRef &) const = default; };
	struct JourneyPolicy {
		std::map<StationID, int64_t> lower_bounds; ///< Relaxed reverse adjacent-stop distances, never winning routes.
		size_t work = 0, bytes = 0, scratch_bytes = 0;
		uint64_t last_used = 0; ///< Optional residency, never admission or a saved simulation decision.
		bool limited = false;
		bool complete = false;
		int64_t unsettled_bound = 0;
		bool unknown_phase = false;
		PassengerPeriodicPlan::Reason diagnostic_limit = PassengerPeriodicPlan::Reason::NoCandidate;
		PassengerPeriodicPlan::Reason reason = PassengerPeriodicPlan::Reason::NoCandidate;
	};
	using PolicyKey = std::tuple<CargoType, StationID, StationID, PassengerJourneyLimits, PassengerJourneyEvaluation>;
	std::map<OrderListID, PassengerPeriodicService> services;
	std::map<StationID, std::set<StopRef>> stations;
	std::set<OrderListID> dirty;
	std::map<OrderListID, StateTicks> dirty_since;
	StateTicks sweeping_since{};
	uint32_t cursor = 0; ///< Bounded pool-ID sweep; no national discovery per query/tick.
	uint16_t dirty_cursor = 65534; ///< Rotating deterministic dirty queue; repeated edits cannot starve higher IDs.
	bool sweeping = true, restoring = false;
	uint64_t generation = 0;
	size_t bytes = 0, policy_bytes = 0;
	uint64_t policy_clock = 0;
	std::map<PolicyKey, JourneyPolicy> journey_policies;
	PassengerDatabaseDiagnostic diagnostic;
	void Remove(OrderListID id);
	void Index(const PassengerPeriodicService &service);
	JourneyPolicy CompilePolicy(CargoType cargo, StationID origin, StationID destination, PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation) const;
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
