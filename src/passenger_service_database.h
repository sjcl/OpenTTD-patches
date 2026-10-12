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
	static PassengerPeriodicTimetable Compile(std::span<const Order> orders);
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
};

struct PassengerDatabaseDiagnostic {
	uint64_t builds = 0, build_work = 0, build_microseconds = 0;
	uint64_t searches = 0, query_microseconds = 0, next_departures = 0, policy_hits = 0, policy_misses = 0;
	size_t services = 0, timed_services = 0, vehicles = 0, bytes = 0, policy_bytes = 0, pending = 0;
	size_t known_phase_vehicles = 0;
	uint64_t invalidations = 0, global_invalidations = 0, rejected_work = 0, rejected_memory = 0;
	uint64_t maximum_pending_ticks = 0;
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
private:
	struct StopRef { OrderListID service; size_t stop; auto operator<=>(const StopRef &) const = default; };
	struct PolicyRide { OrderListID service; size_t board, alight; bool operator==(const PolicyRide &) const = default; };
	struct JourneyPolicy {
		std::vector<std::vector<PolicyRide>> routes;
		size_t work = 0, bytes = 0;
		uint64_t last_used = 0; ///< Optional residency, never admission or a saved simulation decision.
		bool limited = false;
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

#endif /* PASSENGER_SERVICE_DATABASE_H */
