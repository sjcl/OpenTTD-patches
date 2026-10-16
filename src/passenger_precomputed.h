/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_precomputed.h Shared destination trees over service occurrences. */
#ifndef PASSENGER_PRECOMPUTED_H
#define PASSENGER_PRECOMPUTED_H

#include "passenger_journey_type.h"
#include "cargo_type.h"
#include "passenger_build_task.h"
#include <queue>
#include <functional>
#include <unordered_map>

struct PassengerPeriodicService;

struct PassengerPrecomputedPlan {
	enum class State : uint8_t { Ready, NoRoute, Building, Unsupported, Memory };
	State state = State::Building;
	std::optional<PassengerJourneyExecution> ride;
	int64_t cost = 0, riding = 0, waiting = 0;
	uint32_t distance = 0, transfers = 0;
	bool estimated_service = false; ///< First carrier has an incomplete timetable; no certified departure exists.
};

struct PassengerPrecomputedDiagnostic {
	uint64_t builds = 0, operations = 0, build_nanoseconds = 0;
	uint64_t queries = 0, query_nanoseconds = 0, hits = 0, missing = 0, no_route = 0;
	size_t stations = 0, patterns = 0, states = 0, edges = 0, destinations = 0, stored_states = 0;
	size_t bytes = 0;
	size_t shared_sections = 0, section_rides = 0; ///< Build-time estimates and concrete, non-spliceable rides.
	size_t certified_patterns = 0, estimated_patterns = 0;
	uint64_t envelope = 0;
	size_t retained_destinations = 0, retained_bytes = 0;
	uint64_t retained_envelope = 0;
};

/** Mandatory derived state, independent of the optional Timed/Schedule caches.
 * A fixed number of reverse-tree operations is executed per tick. The operation
 * cursor is saved; replay from the saved published DB restores precisely the same
 * ready rows and frontier before simulation resumes. No wall-clock admission.
 * Waiting states remember the incoming pattern, so alternatives are not limited
 * to K routes and the no-immediate-reboarding constraint is part of the graph.
 * Shared stop sections add estimated boarding costs to concrete whole rides.
 * Their landing states permit alighting only; frequency sharing cannot splice
 * one vehicle's prefix into another vehicle's on-board continuation.
 */
class PassengerPrecomputedTable {
public:
	static constexpr uint32_t INVALID = UINT32_MAX;
	static constexpr uint64_t MAX_BYTES = uint64_t{16} * 1024 * 1024 * 1024;
	void Begin(std::span<const PassengerPeriodicService *const> services, CargoType cargo, int64_t transfer, uint64_t memory = MAX_BYTES, bool sections = true, bool cooperative = false, std::shared_ptr<uint64_t> memory_budget = {}, bool estimates = true, bool optimized = true);
	bool PrepareGraph(size_t quantum = 1024);
	bool IsPreparingGraph() const { return static_cast<bool>(this->graph_build); }
	uint64_t GetGraphWork() const { return this->graph_work; }
	size_t Step(size_t operations, bool legacy_publication = false, const std::function<void(StationID)> &published = {});
	void ReleaseDestination(StationID destination); ///< Retire a row immediately after its replacement is published.
	uint64_t LegacyPublicationWork() const { return uint64_t(this->rows.size()) * this->waiting_state_count; }
	PassengerPrecomputedPlan Plan(StationID origin, StationID destination, StateTicks ready, OrderListID excluded = OrderListID::Invalid(), uint32_t sample = 0) const;
	bool Finished() const { return !this->graph_build && (this->failed || this->destination == this->stations.size()); }
	bool MemoryRejected() const { return this->failed; }
	const PassengerPrecomputedDiagnostic &GetDiagnostic() const { return this->diagnostic; }
	OrderListID BoardingKey(OrderListID service) const;
	std::span<const OrderListID> BoardingServices(OrderListID service) const;
	static uint64_t RawSectionBoardingKey(const PassengerJourneyExecution &ride);
	bool Rebind(PassengerJourneyExecution &ride, OrderListID service, VehicleOrderID boarding = INVALID_VEH_ORDER_ID) const;
	std::optional<uint64_t> SectionBoardingKey(const PassengerJourneyExecution &ride) const;
	std::span<const uint64_t> SectionBoardingKeys(OrderListID service, VehicleOrderID boarding) const;
	bool RefreshService(const PassengerPeriodicService &service); ///< Refresh bindings, retaining the frozen estimated costs.
	std::span<const StationID> ReachableDestinations(StationID origin) const;
	std::optional<int64_t> RouteCost(StationID origin, StationID destination) const; ///< Missing row differs from a proved infinite cost.
private:
	struct Member { OrderListID id; uint64_t incarnation, generation; bool estimated; };
	struct Pattern {
		std::vector<Member> members;
		std::vector<OrderListID> boarding_services;
		std::vector<uint32_t> occurrences;
		int64_t wait = 0, circuit = 0, risk = 0;
		uint64_t weight = 1;
		bool estimated = false;
	};
	struct Occurrence { StationID station; VehicleOrderID order; uint32_t pattern, arrival, departure; bool board, alight, forced, continues; };
	enum class EdgeKind : uint8_t { Board, Ride, Wait, Alight, Section };
	struct Edge { uint32_t from, to, occurrence; int64_t cost; uint32_t distance; EdgeKind kind; int64_t uncertainty; };
	struct SectionRide { uint32_t board, alight; int64_t riding, waiting, cost; };
	struct SectionBinding { uint32_t board, alight; };
	struct Section { uint64_t key; std::vector<SectionBinding> bindings; };
	struct SectionPath { uint32_t section, board, alight; };
	struct Label {
		int64_t cost = INT64_MAX, riding = 0, waiting = 0;
		int64_t first_wait = 0;
		uint32_t board = INVALID, alight = INVALID, legs = 0, distance = 0;
	};
	struct Row { std::vector<Label> labels, alternative; };
	struct LabelMetadata {
		int64_t riding = 0, waiting = 0, first_wait = 0;
		uint32_t board = INVALID, alight = INVALID, distance = 0;
	};
	struct SearchWorkspace {
		std::vector<int64_t> cost;
		std::vector<uint32_t> legs;
		std::vector<LabelMetadata> metadata;
	} search;
	using QueueItem = std::tuple<int64_t, uint32_t, uint32_t>;
	std::vector<uint32_t> station_index, service_pattern;
	std::vector<StationID> stations;
	std::vector<Pattern> patterns;
	std::vector<Occurrence> occurrences;
	std::vector<Edge> edges;
	std::vector<SectionRide> section_rides;
	std::vector<Section> sections;
	std::unordered_map<uint64_t, SectionPath> section_paths; ///< Packed pattern and saved order pair; lookup only, never iterated for decisions.
	std::vector<std::vector<uint64_t>> boarding_sections; ///< Keys for each concrete boarding occurrence, independent of fleet size.
	size_t binding_bytes = 0; ///< Computed once during graph preparation.
	std::vector<uint32_t> landings; ///< Section arrivals have no on-board continuation.
	std::vector<uint32_t> incoming_offsets, incoming;
	std::vector<std::vector<std::pair<uint32_t, uint32_t>>> waiting_states;
	std::vector<Row> rows;
	std::vector<std::vector<StationID>> reachable_destinations; ///< Inverse published rows; no waiting-packet dependency.
	size_t reachable_bytes = 0;
	Row scratch, prepared;
	size_t waiting_state_count = 0;
	std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> queue;
	size_t destination = 0, cursor = 0;
	uint32_t expanding = INVALID;
	size_t finishing = 0;
	bool finishing_row = false;
	bool initializing_row = false;
	size_t seeding = 0;
	bool failed = false, unsupported = false;
	bool estimates = true;
	bool optimized = true; ///< Saved algorithm identity; old work cursors always use the original graph and operation sequence.
	CargoType cargo = INVALID_CARGO;
	PassengerBuildTask graph_build; ///< Derived preparation only. No row can be published until this completes.
	uint64_t graph_work = 0; ///< Actual coroutine steps, used only for bounded join progress reports.
	PassengerBuildTask BuildGraph(std::vector<const PassengerPeriodicService *> services, CargoType cargo, int64_t transfer, uint64_t memory, bool sections, bool cooperative, std::shared_ptr<uint64_t> memory_budget);
	mutable PassengerPrecomputedDiagnostic diagnostic;
	uint32_t StateFor(uint32_t station, uint32_t pattern) const;
	const Member *FindMember(OrderListID service) const;
	const Section *FindSection(const PassengerJourneyExecution &ride) const;
	void StartDestination();
	int64_t SearchCost(uint32_t state) const { return this->optimized ? this->search.cost[state] : this->scratch.labels[state].cost; }
	uint32_t SearchLegs(uint32_t state) const { return this->optimized ? this->search.legs[state] : this->scratch.labels[state].legs; }
	Label SearchLabel(uint32_t state) const;
	void SetSearchLabel(uint32_t state, const Label &label);
	void ContractGraph(size_t &nodes);
	void AddEdge(uint32_t from, uint32_t to, int64_t cost, EdgeKind kind, uint32_t occurrence = INVALID, uint32_t distance = 0, int64_t uncertainty = 0);
	PassengerBuildTask BuildSections(const std::vector<std::vector<const PassengerPeriodicService *>> &members, int64_t transfer,
			size_t &nodes, uint64_t &envelope, uint64_t memory, bool cooperative, bool &success);
	PassengerPrecomputedPlan Select(const Label &label, StateTicks ready, OrderListID excluded, uint32_t sample) const;
};

#endif
