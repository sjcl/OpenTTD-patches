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
#include "core/passenger_queue.hpp"
#include "core/passenger_strategy.hpp"
#include <queue>
#include <functional>
#include <unordered_map>
#include <map>

struct PassengerPeriodicService;

struct PassengerPrecomputedPlan {
	enum class State : uint8_t { Ready, NoRoute, Building, Unsupported, Memory };
	State state = State::Building;
	std::optional<PassengerJourneyExecution> ride;
	int64_t cost = 0, riding = 0, waiting = 0;
	uint32_t distance = 0, transfers = 0;
	bool estimated_service = false; ///< First carrier has an incomplete timetable; no certified departure exists.
	bool hyperpath = false;
};

struct PassengerPrecomputedDiagnostic {
	uint64_t builds = 0, operations = 0, build_nanoseconds = 0;
	uint64_t queries = 0, query_nanoseconds = 0, hits = 0, missing = 0, no_route = 0;
	size_t stations = 0, patterns = 0, states = 0, edges = 0, destinations = 0, stored_states = 0;
	size_t bytes = 0;
	size_t shared_sections = 0, section_rides = 0; ///< Build-time estimates and concrete, non-spliceable rides.
	size_t range_edges = 0, logical_edges = 0; ///< Physical CSR descriptors versus their ordered virtual relaxations.
	size_t certified_patterns = 0, estimated_patterns = 0;
	uint64_t envelope = 0;
	size_t retained_destinations = 0, retained_bytes = 0;
	uint64_t retained_envelope = 0;
	uint64_t board_events = 0, board_accepts = 0, range_updates = 0, heap_pushes = 0, heap_pops = 0;
	uint64_t candidate_count = 0, policy_count = 0, base_policy_bytes = 0, difference_policy_bytes = 0;
	size_t max_candidates = 0;
	std::map<size_t, uint64_t> candidate_histogram;
	uint64_t local_replans = 0, local_replan_nanoseconds = 0;
};

/** Explicit, potentially linear diagnostic; never collected by the tick scheduler. */
struct PassengerHyperpathMemoryDiagnostic {
	uint64_t reported = 0, graph = 0, labels = 0, indices = 0, policy_headers = 0, candidate_arrays = 0, local = 0;
	uint64_t differences = 0, additions = 0, maximum_additions = 0, multi_addition_differences = 0, unexpected_removals = 0, overlay_bytes = 0;
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
	static constexpr uint64_t MAX_HYPERPATH_BYTES = uint64_t{32} * 1024 * 1024 * 1024; ///< Canonical admission, including a retained generation; not a physical allocation request.
	void Begin(std::span<const PassengerPeriodicService *const> services, CargoType cargo, int64_t transfer, uint64_t memory = MAX_BYTES, bool sections = true, bool cooperative = false, std::shared_ptr<uint64_t> memory_budget = {}, bool estimates = true, bool optimized = true, bool integrated = true, bool hyperpath = false, bool revisit_dominance = true, bool continuation_witness = true, bool flat_storage = true);
	bool PrepareGraph(size_t quantum = 1024);
	bool IsPreparingGraph() const { return static_cast<bool>(this->graph_build); }
	uint64_t GetGraphWork() const { return this->graph_work; }
	size_t Step(size_t operations, bool legacy_publication = false, const std::function<void(StationID)> &published = {});
	void ReleaseDestination(StationID destination); ///< Retire a row immediately after its replacement is published.
	void PrepareRetirement(const PassengerPrecomputedTable &replacement); ///< Freeze the few destinations absent from the replacement before retiring its ascending rows.
	void ReuseRetainedDestinations(); ///< Start another retirement epoch after the previous replacement finished.
	uint64_t LegacyPublicationWork() const { return uint64_t(this->rows.size()) * this->waiting_state_count; }
	PassengerPrecomputedPlan Plan(StationID origin, StationID destination, StateTicks ready, OrderListID excluded = OrderListID::Invalid(), uint32_t sample = 0, OrderListID failed = OrderListID::Invalid()) const;
	bool Finished() const { return !this->graph_build && (this->failed || this->destination == this->stations.size()); }
	bool MemoryRejected() const { return this->failed; }
	const PassengerPrecomputedDiagnostic &GetDiagnostic() const { return this->diagnostic; }
	PassengerHyperpathMemoryDiagnostic InspectMemory() const;
	std::vector<uint64_t> InspectBuildingPolicyRow(StationID destination) const; ///< Explicit diagnostic, called from a row-publication observer only.
	OrderListID BoardingKey(OrderListID service) const;
	std::span<const OrderListID> BoardingServices(OrderListID service) const;
	static uint64_t RawSectionBoardingKey(const PassengerJourneyExecution &ride);
	bool Rebind(PassengerJourneyExecution &ride, OrderListID service, VehicleOrderID boarding = INVALID_VEH_ORDER_ID) const;
	std::optional<uint64_t> SectionBoardingKey(const PassengerJourneyExecution &ride) const;
	std::span<const uint64_t> SectionBoardingKeys(OrderListID service, VehicleOrderID boarding) const;
	bool RefreshService(const PassengerPeriodicService &service); ///< Refresh bindings, retaining the frozen estimated costs.
	using DestinationSpans = std::array<std::span<const StationID>, 2>; ///< Sorted surviving prefix, then sorted suffix; no overlap.
	DestinationSpans ReachableDestinations(StationID origin) const;
	std::optional<int64_t> RouteCost(StationID origin, StationID destination) const; ///< Missing row differs from a proved infinite cost.
	bool IsHyperpath() const { return this->hyperpath; }
	bool HasPolicy(StationID origin, StationID destination) const { return this->FindRow(destination) != nullptr && this->PolicyState(origin, OrderListID::Invalid()) != INVALID; }
	std::optional<int64_t> ServiceWait(OrderListID service) const;
	uint32_t BoardingOccurrence(StationID origin, OrderListID service, VehicleOrderID order) const;
	/** Null means the local-policy cache is cold: collect conservatively and validate at the FIFO front. */
	std::optional<bool> ContainsCached(StationID origin, StationID destination, uint32_t occurrence, OrderListID previous, OrderListID failed) const;
	/** Derived boarding filter, never saved and never part of admission or Work.
	 * True when the occurrence appears in a stored policy of the destination's published or
	 * building row: only the station's base state, or with any_state also an incoming-carrier
	 * state. A superset of stored membership for those states. Failed-carrier local policies are
	 * not covered. Stale bits of a retired row only cost a check. */
	bool MayBeStoredCandidate(uint32_t occurrence, StationID destination, bool any_state) const;
	/** No plan or stored membership from this table can bypass MayBeStoredCandidate. */
	bool BoardingMaskCovers() const;
	static constexpr uint64_t MAX_BOARDING_MASK_BYTES = uint64_t{64} * 1024 * 1024; ///< Larger maps keep the complete scan.
	std::span<const uint32_t> Candidates(StationID origin, StationID destination, OrderListID previous = OrderListID::Invalid(), OrderListID failed = OrderListID::Invalid()) const;
	PassengerPrecomputedPlan Board(StationID origin, StationID destination, StateTicks ready, OrderListID service, VehicleOrderID order, OrderListID previous = OrderListID::Invalid(), OrderListID failed = OrderListID::Invalid()) const;
	struct CandidateDiagnostic { uint32_t occurrence; OrderListID service; VehicleOrderID order; StationID alight; int64_t cost, wait; uint64_t frequency; };
	std::vector<CandidateDiagnostic> Inspect(StationID origin, StationID destination, OrderListID previous = OrderListID::Invalid()) const;
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
	struct Occurrence { StationID station; VehicleOrderID order; uint32_t pattern, arrival, departure; bool board, alight, forced, continues; int64_t travel = 0, uncertainty = 0, dwell = 0; uint32_t distance = 0; };
	enum class EdgeKind : uint8_t { Board, Ride, Wait, Alight, Section };
	struct Edge { uint32_t from, to, occurrence; int64_t cost; uint32_t distance; EdgeKind kind; int64_t uncertainty; uint32_t range = INVALID; };
	struct RangeEdge { uint32_t begin, end, excluded; }; ///< Base station is the descriptor's from, preceding this contiguous range.
	struct SectionRide { uint32_t board, alight; int64_t riding, waiting, cost; };
	struct SectionBinding { uint32_t board, alight; };
	struct Section { uint64_t key; std::vector<SectionBinding> bindings; };
	struct SectionPath { uint32_t section, board, alight; };
	struct Label {
		int64_t cost = INT64_MAX, riding = 0, waiting = 0;
		int64_t first_wait = 0;
		uint32_t board = INVALID, alight = INVALID, legs = 0, distance = 0;
		bool operator==(const Label &) const = default;
	};
	struct Policy { int64_t cost = INT64_MAX, wait = 0; std::vector<uint32_t> candidates; };
	struct FlatPolicy {
		int64_t cost = INT64_MAX, wait = 0;
		uint32_t offset = 0, count = 0;
		static constexpr uint32_t FULL = uint32_t{1} << 31, MASK = FULL - 1;
	};
	static_assert(sizeof(FlatPolicy) == 24);
	/** One storage layout per table algorithm, never mixed within a table:
	 * - Compressed (integrated, non-Hyperpath): labels per station, sparse alternatives,
	 *   one dense exception per station with waiting states.
	 * - Hyperpath: labels for every waiting state; indices holds the policy index of
	 *   every waiting state; policies (through v14) or flat_* (v15) hold the strategies.
	 * - Older engines: labels per waiting state and alternatives per station. */
	struct Row {
		std::vector<Label> labels, alternative, exception_labels;
		std::vector<uint32_t> indices; ///< Compressed: alternative indices, then exception state IDs. Hyperpath: policy index per waiting state.
		std::vector<Policy> policies; ///< Hyperpath only: exact old storage for replay generations through v14.
		std::vector<FlatPolicy> flat_policies; ///< Hyperpath v15 only.
		std::vector<uint32_t> flat_candidates; ///< Hyperpath v15 only: base payloads and incoming-service addition lists, in policy order.
		size_t bytes = 0; ///< Hyperpath only: payload charged at publication.
		bool Empty() const { return this->labels.empty(); }
		size_t AllocatedBytes() const ///< Non-Hyperpath rows only; Hyperpath rows report bytes.
		{
			return (this->labels.capacity() + this->alternative.capacity() + this->exception_labels.capacity()) * sizeof(Label) +
					this->indices.capacity() * sizeof(uint32_t);
		}
	};
	struct LabelMetadata {
		int64_t riding = 0, waiting = 0, first_wait = 0;
		uint32_t board = INVALID, alight = INVALID, distance = 0;
	};
	struct SearchWorkspace {
		std::vector<int64_t> cost;
		std::vector<uint32_t> legs;
		std::vector<LabelMetadata> metadata;
	} search;
	std::vector<uint32_t> exception_state; ///< At descriptor boundaries, only this wait may differ from the station base.
	using QueueItem = std::tuple<int64_t, uint32_t, uint32_t>;
	std::vector<uint32_t> station_index, service_pattern;
	std::vector<StationID> stations;
	std::vector<Pattern> patterns;
	std::vector<Occurrence> occurrences;
	struct Revisit { uint32_t next = INVALID; int64_t cost = 0; };
	std::vector<Revisit> revisits; ///< Departure-to-departure continuation to the next boardable visit to the same station.
	std::vector<Edge> edges;
	struct HotEdges {
		std::vector<uint32_t> from;
		std::vector<int64_t> cost;
		std::vector<uint8_t> legs;
		std::vector<uint32_t> range;
	} hot_edges;
	std::vector<RangeEdge> range_edges;
	struct ColdEdge { uint32_t occurrence, distance; int64_t uncertainty; EdgeKind kind; };
	std::vector<ColdEdge> cold_edges;
	std::vector<SectionRide> section_rides;
	std::vector<Section> sections;
	std::unordered_map<uint64_t, SectionPath> section_paths; ///< Packed pattern and saved order pair; lookup only, never iterated for decisions.
	std::vector<std::vector<uint64_t>> boarding_sections; ///< Keys for each concrete boarding occurrence, independent of fleet size.
	size_t binding_bytes = 0; ///< Computed once during graph preparation.
	std::vector<uint32_t> landings; ///< Section arrivals have no on-board continuation.
	std::vector<uint32_t> incoming_offsets, incoming;
	std::vector<uint32_t> seed_offsets, seed_occurrences; ///< Alighting occurrences in original order, indexed by destination station.
	std::vector<uint32_t> exception_slots; ///< Dense exception slot only for stations with waiting states.
	uint32_t exception_slot_count = 0;
	std::vector<std::vector<std::pair<uint32_t, uint32_t>>> waiting_states;
	std::vector<uint32_t> waiting_station;
	std::vector<std::vector<uint32_t>> station_boarding;
	mutable std::map<std::tuple<StationID, uint32_t, OrderListID>, Policy> local_policies;
	size_t hyperpath_static_bytes = 0, hyperpath_row_bytes = 0, hyperpath_candidate_bytes = 0, prepared_candidate_bytes = 0;
	mutable size_t local_policy_bytes = 0, expanded_policy_bytes = 0;
	mutable std::map<std::pair<StationID, uint32_t>, std::vector<uint32_t>> expanded_policies;
	bool flat_storage = false; ///< v15 physical representation and frozen memory admission.
	std::vector<uint64_t> boarding_mask; ///< Per occurrence: base-state destination-row bits, then any-state bits.
	size_t boarding_mask_words = 0; ///< Words per occurrence and plane; zero until allocated.
	bool boarding_mask_rejected = false; ///< Too large for MAX_BOARDING_MASK_BYTES.
	void MarkBoardingCandidate(uint32_t occurrence, bool base_state);
	std::span<const uint32_t> FlatPayload(const Row &row, uint32_t policy) const;
	uint32_t PreviousPattern(uint32_t state) const;
	bool ContainsStored(const Row &row, uint32_t state, uint32_t occurrence) const;
	std::pair<int64_t, int64_t> PolicyValues(const Row &row, uint32_t state) const;
	void ExpandPolicy(const Row &row, uint32_t state, std::vector<uint32_t> &result) const;
	std::span<const uint32_t> RowCandidates(const Row &row, StationID destination, uint32_t state) const;
	const Policy &LocalPolicy(const Row &row, StationID destination, uint32_t state, OrderListID failed) const;
	std::vector<Row> rows;
	std::vector<std::vector<StationID>> reachable_destinations; ///< Inverse published rows; no waiting-packet dependency.
	std::vector<std::vector<StationID>> retirement_survivors; ///< Only destinations absent from the frozen replacement.
	std::optional<StationID> retirement_watermark;
	bool retirement_prepared = false;
	size_t reachable_bytes = 0;
	Row scratch, prepared;
	size_t waiting_state_count = 0;
	size_t publication_cursor = 0; ///< Original virtual query-state copy position, independent of compressed storage.
	size_t published_bytes = 0;
	PassengerDestinationQueue<QueueItem> queue;
	size_t destination = 0, cursor = 0;
	uint32_t range_offset = 0; ///< Next virtual edge: zero is the separate base station, then begin..end.
	uint32_t expanding = INVALID;
	size_t finishing = 0;
	bool finishing_row = false;
	bool initializing_row = false;
	size_t seeding = 0;
	bool failed = false, unsupported = false;
	bool estimates = true;
	bool optimized = true; ///< Saved algorithm identity; old work cursors always use the original graph and operation sequence.
	bool integrated = true; ///< v11 bounded range/CSR engine; never reinterpret a v10 operation cursor.
	bool hyperpath = false; ///< A separate algorithm identity; legacy work never enters the strategy queue.
	bool revisit_dominance = true; ///< JPSD v12 strategies replay without the v13 occurrence dominance rule.
	bool continuation_witness = true; ///< v14: require equality and a selected same-run continuation, preserving earlier tied alighting.
	int64_t transfer_cost = 0;
	struct StrategyWorkspace {
		std::vector<PassengerStrategy> sums;
		std::vector<std::vector<uint32_t>> candidates;
		std::vector<bool> settled;
		PassengerDestinationQueue<QueueItem> events;
		uint32_t board_edge = INVALID, offset = 0;
		uint32_t witness_edge = INVALID, witness_at = INVALID;
		int64_t witness_cost = 0;
		int64_t last = 0;
	} strategy;
	CargoType cargo = INVALID_CARGO;
	PassengerBuildTask graph_build; ///< Derived preparation only. No row can be published until this completes.
	uint64_t graph_work = 0; ///< Actual coroutine steps, used only for bounded join progress reports.
	PassengerBuildTask BuildGraph(std::vector<const PassengerPeriodicService *> services, CargoType cargo, int64_t transfer, uint64_t memory, bool sections, bool cooperative, std::shared_ptr<uint64_t> memory_budget);
	mutable PassengerPrecomputedDiagnostic diagnostic;
	uint32_t StateFor(uint32_t station, uint32_t pattern) const;
	const Member *FindMember(OrderListID service) const;
	const Section *FindSection(const PassengerJourneyExecution &ride) const;
	void StartDestination();
	bool UseCompressedRows() const;
	const Label &RowLabel(const Row &row, uint32_t origin, uint32_t state) const;
	const Label &AlternativeLabel(const Row &row, uint32_t origin) const;
	void PrepareCompressedRow();
	void CopyStationLabel(uint32_t origin);
	int64_t SearchCost(uint32_t state) const { return this->optimized ? this->search.cost[state] : this->scratch.labels[state].cost; }
	uint32_t SearchLegs(uint32_t state) const { return this->optimized ? this->search.legs[state] : this->scratch.labels[state].legs; }
	uint32_t SearchBoard(uint32_t state) const { return this->optimized ? this->search.metadata[state].board : this->scratch.labels[state].board; }
	uint32_t SearchAlight(uint32_t state) const { return this->optimized ? this->search.metadata[state].alight : this->scratch.labels[state].alight; }
	int64_t SearchRiding(uint32_t state) const { return this->optimized ? this->search.metadata[state].riding : this->scratch.labels[state].riding; }
	Label SearchLabel(uint32_t state) const;
	void SetSearchLabel(uint32_t state, const Label &label);
	bool WaitingStationInvariant(uint32_t station) const; ///< Defined only in the invariant-checking test TU.
	size_t RelaxIncoming(size_t allowance);
	size_t StepHyperpath(size_t allowance, const std::function<void(StationID)> &published);
	void PushHyperpath(int64_t key, uint32_t kind, uint32_t id);
	uint32_t PolicyState(StationID origin, OrderListID previous) const;
	const Row *FindRow(StationID destination) const;
	Label EvaluateRide(const Row &row, uint32_t board, StationID destination) const;
	bool DominatedRevisit(uint32_t board, int64_t cost, int64_t next_cost) const;
	bool HasSameRunContinuationWitness(uint32_t board, uint32_t alight) const;
	void AddBoardEdges(uint32_t station, uint32_t to, uint32_t pattern, int64_t cost, EdgeKind kind, uint32_t occurrence, uint32_t distance = 0);
	void ContractGraph(size_t &nodes);
	void AddEdge(uint32_t from, uint32_t to, int64_t cost, EdgeKind kind, uint32_t occurrence = INVALID, uint32_t distance = 0, int64_t uncertainty = 0);
	PassengerBuildTask BuildSections(const std::vector<std::vector<const PassengerPeriodicService *>> &members, int64_t transfer,
			size_t &nodes, uint64_t &envelope, uint64_t memory, bool cooperative, bool &success);
	PassengerPrecomputedPlan Select(const Label &label, StateTicks ready, OrderListID excluded, uint32_t sample) const;
};

#endif
