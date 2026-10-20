/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_precomputed.h Shared destination trees over service occurrences. */
#ifndef PASSENGER_PRECOMPUTED_H
#define PASSENGER_PRECOMPUTED_H

#include "passenger_journey_type.h"
#include "passenger_aircraft_cost.h"
#include "cargo_type.h"
#include "passenger_build_task.h"
#include "core/passenger_queue.hpp"
#include "core/passenger_strategy.hpp"
#include <queue>
#include <functional>
#include <unordered_map>
#include <map>
#include <atomic>
#include <chrono>

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

/** Local performance data only; never saved or used for logical admission. */
struct PassengerParallelBuildDiagnostic {
	uint64_t batches = 0, searches = 0, batch_nanoseconds = 0, search_nanoseconds = 0, integration_nanoseconds = 0, fallbacks = 0;
	uint64_t minimum_work = UINT64_MAX, maximum_work = 0, search_work = 0;
	uint64_t cache_hits = 0, cache_work = 0, serial_rows = 0, main_search_nanoseconds = 0, serial_search_work = 0;
	uint64_t cancelled_search_work = 0, thread_starts = 0, workspace_reuses = 0, throttles = 0;
	uint64_t wait_nanoseconds = 0, waits = 0, wait_timeouts = 0;
	size_t outstanding = 0; ///< Queued or executing searches, observed locally.
	size_t running = 0;
	uint64_t finished_rows = 0, finish_effects = 0; ///< Hyperpath rows finished by a worker, and the effects replayed for them.
	size_t workers = 0, peak_pending = 0, estimated_workspace_bytes = 0;
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
	/** Production uses the current algorithms. Alternate controls are differential
	 * test/benchmark references, never saved construction-format identities. */
	void Begin(std::span<const PassengerPeriodicService *const> services, CargoType cargo, int64_t transfer, uint64_t memory = MAX_BYTES, bool sections = true, bool cooperative = false, std::shared_ptr<uint64_t> memory_budget = {}, bool estimates = true, bool optimized = true, bool integrated = true, bool hyperpath = false, bool revisit_dominance = true, bool continuation_witness = true, bool flat_storage = true, PassengerAircraftCost aircraft_cost = PassengerAircraftCost::Current());
	bool PrepareGraph(size_t quantum = 1024);
	bool IsPreparingGraph() const { return static_cast<bool>(this->graph_build); }
	uint64_t GetGraphWork() const { return this->graph_work; }
	size_t Step(size_t operations, bool legacy_publication = false, const std::function<void(StationID)> &published = {});
	/** Nonblocking speculative searches. Work/publication exactly match Step;
	 * a cache miss latches serial execution for the whole row. Zero selects Step. */
	size_t StepParallel(size_t operations, uint workers = 4, const std::function<void(StationID)> &published = {});
	/** Stopped simulation only: keep lookahead alive and wait at most 1ms per call.
	 * May consume less than operations when yielding for a result. Same Work frontier as Step. */
	size_t StepPreparation(size_t operations, uint workers, const std::function<void(StationID)> &published = {});
	PassengerParallelBuildDiagnostic GetParallelDiagnostic() const;
	void ReleaseDestination(StationID destination); ///< Retire a row immediately after its replacement is published.
	void PrepareRetirement(const PassengerPrecomputedTable &replacement); ///< Freeze the few destinations absent from the replacement before retiring its ascending rows.
	void ReuseRetainedDestinations(); ///< Start another retirement epoch after the previous replacement finished.
	uint64_t LegacyPublicationWork() const { return uint64_t(this->rows.size()) * this->graph->waiting_state_count; }
	PassengerPrecomputedPlan Plan(StationID origin, StationID destination, StateTicks ready, OrderListID excluded = OrderListID::Invalid(), uint32_t sample = 0, OrderListID failed = OrderListID::Invalid()) const;
	bool Finished() const { return !this->graph_build && (this->failed || this->destination == this->graph->stations.size()); }
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
	bool IsHyperpath() const { return this->graph->hyperpath; }
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
	/** Append exactly the occurrences for which ContainsCached(origin, destination, o, previous, Invalid) is true. */
	void StoredCandidates(StationID origin, StationID destination, OrderListID previous, std::vector<uint32_t> &out) const;
	/** No plan or stored membership from this table can bypass MayBeStoredCandidate. */
	bool BoardingMaskCovers() const;
	static constexpr uint64_t MAX_BOARDING_MASK_BYTES = uint64_t{64} * 1024 * 1024; ///< Larger maps keep the complete scan.
	std::span<const uint32_t> Candidates(StationID origin, StationID destination, OrderListID previous = OrderListID::Invalid(), OrderListID failed = OrderListID::Invalid()) const;
	struct DisplayCandidate { OrderListID service; uint64_t incarnation, generation; VehicleOrderID order; };
	/** Binding only; the caller must already know membership in an unchanged display policy. */
	std::optional<uint64_t> DisplayServiceGeneration(OrderListID service, uint64_t incarnation) const;
	enum class DisplayRead : uint8_t { Candidate, Partial, Complete, Unavailable };
	/** Indices only. The database binds this cursor to a policy generation/publication. */
	struct DisplayCursor {
		uint8_t phase = 0;
		bool inspected = false, bound = false;
		CargoType cargo = INVALID_CARGO;
		StationID origin = StationID::Invalid(), destination = StationID::Invalid();
		OrderListID previous = OrderListID::Invalid(), failed = OrderListID::Invalid();
		size_t occurrence = 0, member = 0;
		uint64_t epoch = 0, generation = 0, changes = 0;
	};
	DisplayRead NextDisplayCandidate(StationID origin, StationID destination, OrderListID previous, OrderListID failed,
			DisplayCursor &cursor, DisplayCandidate &out, size_t &work) const;

	/** Read published policy storage without expanding/rebuilding policies or evaluating routes.
	 * False means missing policy or incomplete enumeration. A cold failed-service policy is unknown. */
	bool DisplayCandidates(StationID origin, StationID destination, OrderListID previous, OrderListID failed,
			std::vector<DisplayCandidate> &out, size_t &work) const;

	PassengerPrecomputedPlan Board(StationID origin, StationID destination, StateTicks ready, OrderListID service, VehicleOrderID order, OrderListID previous = OrderListID::Invalid(), OrderListID failed = OrderListID::Invalid()) const;
	struct CandidateDiagnostic { uint32_t occurrence; OrderListID service; VehicleOrderID order; StationID alight; int64_t cost, wait; uint64_t frequency; };
	std::vector<CandidateDiagnostic> Inspect(StationID origin, StationID destination, OrderListID previous = OrderListID::Invalid()) const;
	/** GUI-owned results. No query diagnostics, policy cache writes, or simulation Work.
	 * Probability is relative to the whole policy, including branches omitted by the display limit. */
	struct RouteCandidate { PassengerJourneyExecution ride; double probability; };
	struct RouteInspection { PassengerPrecomputedPlan::State state = PassengerPrecomputedPlan::State::Building; std::vector<RouteCandidate> candidates; };
	RouteInspection InspectRoutes(StationID origin, StationID destination, OrderListID previous, OrderListID failed, size_t &work) const;
private:
	friend struct PassengerPrecomputedTestAccess;
	struct Member { OrderListID id; uint64_t incarnation, generation; bool estimated; };
	struct Pattern {
		std::vector<Member> members;
		std::vector<OrderListID> boarding_services;
		std::vector<uint32_t> occurrences;
		int64_t wait = 0, circuit = 0, risk = 0;
		uint64_t weight = 1;
		bool estimated = false, aircraft = false;
	};
	struct Occurrence { StationID station; VehicleOrderID order; uint32_t pattern, arrival, departure; bool board, alight, forced, continues; int64_t travel = 0, uncertainty = 0, dwell = 0, travel_cost = 0; uint32_t distance = 0; };
	enum class EdgeKind : uint8_t { Board, Ride, Wait, Alight, Section };
	struct Edge { uint32_t from, to, occurrence; int64_t cost; uint32_t distance; EdgeKind kind; int64_t riding; uint32_t range = INVALID; };
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
	 *   every waiting state; flat_* hold the strategies, with policies as a reference.
	 * - Reference engines: labels per waiting state and alternatives per station. */
	struct Row {
		std::vector<Label> labels, alternative, exception_labels;
		std::vector<uint32_t> indices; ///< Compressed: alternative indices, then exception state IDs. Hyperpath: policy index per waiting state.
		std::vector<Policy> policies; ///< Hyperpath only: full policy storage for differential validation.
		std::vector<FlatPolicy> flat_policies; ///< Hyperpath only.
		std::vector<uint32_t> flat_candidates; ///< Hyperpath only: base payloads and incoming-service addition lists, in policy order.
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
		uint32_t legs = 0; ///< Hyperpath only, in former padding: one cache line per relaxation. Legacy priorities keep legs beside cost.
	};
	static_assert(sizeof(LabelMetadata) == 40);
	struct SearchWorkspace {
		std::vector<int64_t> cost;
		std::vector<uint32_t> legs;
		std::vector<LabelMetadata> metadata;
	};
	using QueueItem = std::tuple<int64_t, uint32_t, uint32_t>;

	mutable std::map<std::tuple<StationID, uint32_t, OrderListID>, Policy> local_policies;
	size_t hyperpath_static_bytes = 0, hyperpath_row_bytes = 0;
	mutable size_t local_policy_bytes = 0, expanded_policy_bytes = 0;
	mutable std::map<std::pair<StationID, uint32_t>, std::vector<uint32_t>> expanded_policies;
	/** Derived (destination, waiting station) -> every finite, non-dominated ride from that station,
	 * sorted by (cost, occurrence). Shared by the local policies of all its states and failed carriers. */
	mutable std::map<std::pair<StationID, uint32_t>, std::vector<std::pair<int64_t, uint32_t>>> station_choices;
	const std::vector<std::pair<int64_t, uint32_t>> &StationChoices(const Row &row, StationID destination, uint32_t station) const;
	/** Per block of 64 destination rows: a base-state plane, then an any-state plane, each one
	 * word per occurrence. One row's publication then writes within two occurrence-sized arrays. */
	std::vector<uint64_t> boarding_mask;
	size_t boarding_mask_words = 0; ///< Blocks of 64 destination rows; zero until allocated.
	bool boarding_mask_rejected = false; ///< Too large for MAX_BOARDING_MASK_BYTES.
	std::vector<uint64_t> published_rows; ///< With the mask: rows that FindRow returns, one bit per destination row.
	size_t BoardingMaskWord(uint32_t occurrence, bool any_state, uint32_t row) const { return (size_t{row / 64} * 2 + any_state) * this->graph->occurrences.size() + occurrence; }
	void MarkBoardingCandidate(uint32_t occurrence, bool base_state);
	/** Working capacity of the row being built; each published row receives an exact copy. */
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
	size_t published_bytes = 0;
	size_t destination = 0;
	bool failed = false, unsupported = false;
	struct StrategyWorkspace {
		std::vector<PassengerStrategy> sums;
		std::vector<std::vector<uint32_t>> candidates;
		std::vector<bool> settled;
		PassengerDestinationQueue<QueueItem> events;
		uint32_t board_edge = INVALID, offset = 0;
		uint32_t witness_edge = INVALID, witness_at = INVALID;
		int64_t witness_cost = 0;
		int64_t last = 0;
	};
	/** No pointers to services or live game objects. Search reads the frozen table
	 * and changes only this context; publication stays on the simulation thread. */
	struct DestinationWorkspace {
		SearchWorkspace search;
		StrategyWorkspace strategy;
		PassengerDestinationQueue<QueueItem> queue;
		Row scratch, prepared;
		std::vector<uint32_t> exception_state;
		std::vector<FlatPolicy> flat_policy_buffer;
		std::vector<uint32_t> flat_candidate_buffer;
		size_t destination = 0, cursor = 0, finishing = 0, seeding = 0, publication_cursor = 0;
		uint32_t range_offset = 0, expanding = INVALID;
		bool finishing_row = false, initializing_row = false, queues_configured = false;
		size_t hyperpath_candidate_bytes = 0, prepared_candidate_bytes = 0;
		/* A worker finishes its searched Hyperpath row ahead. The simulation thread then charges the
		 * same Work and applies only the row's table-wide effects, each at its Work position. */
		struct FinishEffect { size_t step; uint32_t value; uint8_t kind; };
		std::vector<FinishEffect> finish_effects;
		std::vector<uint8_t> finish_marked; ///< Per occurrence, while recording: mask planes already marked by this row.
		std::vector<uint32_t> finish_touched;
		PassengerPrecomputedDiagnostic finish_diagnostic; ///< Policy statistics of the prepared row, merged at publication.
		size_t finish_steps = 0, finish_applied = 0, finish_effect = 0;
		bool finish_prepared = false;
		struct Counters {
			uint64_t board_events = 0, board_accepts = 0, range_updates = 0, heap_pushes = 0, heap_pops = 0;
		} counters;
	} work;
	/** Prepared on the simulation thread, then frozen. Jobs own a const shared
	 * reference, so a table move, reset or retirement cannot invalidate searches. */
	struct GraphSnapshot {
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
		struct ColdEdge { uint32_t occurrence, distance; int64_t riding; EdgeKind kind; };
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
		PassengerAircraftCost aircraft_cost;
		bool flat_storage = false; ///< Compact strategy storage.
		size_t waiting_state_count = 0;
		bool estimates = true;
		bool optimized = true; ///< False selects the scalar graph/queue reference.
		bool integrated = true; ///< Bounded ranges/CSR; false selects the explicit-edge reference.
		bool hyperpath = false;
		bool revisit_dominance = true; ///< Suppress dominated repeat departures; disabled only by reference comparisons.
		bool continuation_witness = true; ///< Require equality and a selected same-run continuation, preserving earlier tied alighting.
		int64_t transfer_cost = 0;
		size_t states = 0, edge_count = 0, logical_edges = 0;
		void StartDestination(DestinationWorkspace &work, size_t destination) const;
		size_t SearchDestination(DestinationWorkspace &work, size_t allowance) const;
		size_t SearchHyperpath(DestinationWorkspace &work, size_t allowance) const;
		bool UseCompressedRows() const;
		Label SearchLabel(const DestinationWorkspace &work, uint32_t state) const;
		void SetSearchLabel(DestinationWorkspace &work, uint32_t state, const Label &label) const;
		bool WaitingStationInvariant(const DestinationWorkspace &work, uint32_t station) const;
		size_t RelaxIncoming(DestinationWorkspace &work, size_t allowance) const;
		void PushHyperpath(DestinationWorkspace &work, int64_t key, uint32_t kind, uint32_t id) const;
		uint32_t StateFor(uint32_t station, uint32_t pattern) const;
		uint32_t PreviousPattern(uint32_t state) const;
		static constexpr uint8_t FINISH_REACHABLE = 0, FINISH_MARK_ANY = 1, FINISH_MARK_BASE = 2;
		/** Advances the finishing of a searched Hyperpath row by up to @p steps Work units, stopping
		 * before its publication. Table-wide effects are reported in order as (kind, value, step). */
		template <typename Effect> size_t FinishHyperpathRow(DestinationWorkspace &work, size_t steps, PassengerPrecomputedDiagnostic &diagnostic, Effect &&effect) const;
		bool HyperpathRowFinished(const DestinationWorkspace &work) const { return work.prepared.labels.size() >= this->waiting_state_count && work.finishing >= this->waiting_state_count; }
		void PrepareHyperpathFinish(DestinationWorkspace &work) const; ///< Worker side: the whole finishing, recorded.
		static void ExactFlatPayloads(DestinationWorkspace &work); ///< Exact publication payloads; grown storage stays in the buffers.
		bool DominatedRevisit(uint32_t board, int64_t cost, int64_t next_cost) const;
		bool HasSameRunContinuationWitness(uint32_t board, uint32_t alight) const;
		int64_t SearchCost(const DestinationWorkspace &work, uint32_t state) const { return this->optimized ? work.search.cost[state] : work.scratch.labels[state].cost; }
		uint32_t SearchLegs(const DestinationWorkspace &work, uint32_t state) const { return this->optimized ? work.search.legs[state] : work.scratch.labels[state].legs; }
		uint32_t SearchBoard(const DestinationWorkspace &work, uint32_t state) const { return this->optimized ? work.search.metadata[state].board : work.scratch.labels[state].board; }
		uint32_t SearchAlight(const DestinationWorkspace &work, uint32_t state) const { return this->optimized ? work.search.metadata[state].alight : work.scratch.labels[state].alight; }
		int64_t SearchRiding(const DestinationWorkspace &work, uint32_t state) const { return this->optimized ? work.search.metadata[state].riding : work.scratch.labels[state].riding; }
	};
	using Revisit = GraphSnapshot::Revisit;
	using ColdEdge = GraphSnapshot::ColdEdge;
	std::shared_ptr<const GraphSnapshot> graph = std::make_shared<GraphSnapshot>();
	std::shared_ptr<GraphSnapshot> building_graph; ///< Exists only during cooperative preparation.
	std::map<OrderListID, Member> refreshed_members; ///< Live bindings never mutate the frozen graph.

	struct WorkerGeneration;
	struct WorkerMetrics;
	std::vector<std::shared_ptr<WorkerMetrics>> parallel_statistics;
	struct DestinationBuildResult {
		std::shared_ptr<WorkerGeneration> generation;
		DestinationWorkspace workspace;
		size_t destination = 0;
		StationID station = StationID::Invalid();
		uint64_t generation_id = 0, operations = 0, nanoseconds = 0;
		std::chrono::steady_clock::time_point wait_started{}; ///< Main-thread only, never serialized.
		std::atomic<bool> ready{false}, cancelled{false};
		bool error = false; ///< Read only after acquiring ready.
	};
	struct DestinationWorkerPool;
	struct ParallelState;
	static DestinationWorkerPool &WorkerPool();
	/** Main-thread ownership cancels on destruction. Workers own only the frozen
	 * graph, cancellation token and their result; never a table or this state. */
	std::shared_ptr<ParallelState> parallel;
	std::shared_ptr<DestinationBuildResult> parallel_result;
	uint64_t parallel_credit = 0;
	size_t serial_destination = SIZE_MAX;
	uint parallel_misses = 0; ///< Local CPU backoff only; never part of Work or saves.
	bool parallel_unavailable = false;
	bool preparation_step = false; ///< Local scheduling mode, scoped to StepPreparation.
	std::chrono::steady_clock::time_point preparation_deadline{};
	std::chrono::milliseconds preparation_wait_limit{2000}; ///< Failure bound; native tests may inject expiry.
	PassengerParallelBuildDiagnostic parallel_diagnostic;
	void (*parallel_quantum_hook)(size_t, uint64_t) = nullptr; ///< Native cancellation/failure scheduling tests.
	void (*parallel_search_hook)(size_t, bool) = nullptr; ///< Native scheduling/failure injection; unset in production.
	bool (*parallel_start_hook)(uint) = nullptr; ///< Native thread-start failure injection; unset in production.
	CargoType cargo = INVALID_CARGO;
	PassengerBuildTask graph_build; ///< Derived preparation only. No row can be published until this completes.
	uint64_t graph_work = 0; ///< Actual coroutine steps, used only for bounded join progress reports.
	PassengerBuildTask BuildGraph(std::vector<const PassengerPeriodicService *> services, CargoType cargo, int64_t transfer, uint64_t memory, bool sections, bool cooperative, std::shared_ptr<uint64_t> memory_budget);
	mutable PassengerPrecomputedDiagnostic diagnostic;
	uint32_t StateFor(uint32_t station, uint32_t pattern) const;
	const Member *FindMember(OrderListID service) const;
	const Section *FindSection(const PassengerJourneyExecution &ride) const;
	void StartDestination();
	void StartDestination(DestinationWorkspace &work, size_t destination) const;
	size_t SearchDestination(DestinationWorkspace &work, size_t allowance) const;
	size_t SearchHyperpath(DestinationWorkspace &work, size_t allowance) const;
	void CollectSearchCounters();
	bool PrepareParallelBatch(uint workers);
	bool ParallelCacheReady() const;
	bool ParallelIdle() const; ///< Native tests may poll outside simulation ticks.
	void CancelParallel();
	size_t AdvanceSearch(size_t allowance, uint workers);
	size_t StepImpl(size_t allowance, bool legacy_publication, const std::function<void(StationID)> &published, uint workers);
	bool UseCompressedRows() const;
	const Label &RowLabel(const Row &row, uint32_t origin, uint32_t state) const;
	const Label &AlternativeLabel(const Row &row, uint32_t origin) const;
	void PrepareCompressedRow(DestinationWorkspace &work) const;
	void CopyStationLabel(DestinationWorkspace &work, uint32_t origin) const;
	int64_t SearchCost(const DestinationWorkspace &work, uint32_t state) const { return this->graph->optimized ? work.search.cost[state] : work.scratch.labels[state].cost; }
	uint32_t SearchLegs(const DestinationWorkspace &work, uint32_t state) const { return this->graph->optimized ? work.search.legs[state] : work.scratch.labels[state].legs; }
	uint32_t SearchBoard(const DestinationWorkspace &work, uint32_t state) const { return this->graph->optimized ? work.search.metadata[state].board : work.scratch.labels[state].board; }
	uint32_t SearchAlight(const DestinationWorkspace &work, uint32_t state) const { return this->graph->optimized ? work.search.metadata[state].alight : work.scratch.labels[state].alight; }
	int64_t SearchRiding(const DestinationWorkspace &work, uint32_t state) const { return this->graph->optimized ? work.search.metadata[state].riding : work.scratch.labels[state].riding; }
	Label SearchLabel(const DestinationWorkspace &work, uint32_t state) const;
	void SetSearchLabel(DestinationWorkspace &work, uint32_t state, const Label &label) const;
	bool WaitingStationInvariant(const DestinationWorkspace &work, uint32_t station) const; ///< Defined only in the invariant-checking test TU.
	size_t RelaxIncoming(DestinationWorkspace &work, size_t allowance) const;
	size_t StepHyperpath(size_t allowance, const std::function<void(StationID)> &published, uint workers);
	void PushHyperpath(DestinationWorkspace &work, int64_t key, uint32_t kind, uint32_t id) const;
	uint32_t PolicyState(StationID origin, OrderListID previous) const;
	const Row *FindRow(StationID destination) const;
	Label EvaluateRide(const Row &row, uint32_t board, StationID destination) const;
	bool DominatedRevisit(uint32_t board, int64_t cost, int64_t next_cost) const;
	bool HasSameRunContinuationWitness(uint32_t board, uint32_t alight) const;
	void AddBoardEdges(uint32_t station, uint32_t to, uint32_t pattern, int64_t cost, EdgeKind kind, uint32_t occurrence, uint32_t distance = 0);
	void ContractGraph(size_t &nodes);
	void AddEdge(uint32_t from, uint32_t to, int64_t cost, EdgeKind kind, uint32_t occurrence = INVALID, uint32_t distance = 0, int64_t uncertainty = 0, int64_t riding = -1);
	PassengerBuildTask BuildSections(const std::vector<std::vector<const PassengerPeriodicService *>> &members, int64_t transfer,
			size_t &nodes, uint64_t &envelope, uint64_t memory, bool cooperative, bool &success);
	PassengerPrecomputedPlan Select(const Label &label, StateTicks ready, OrderListID excluded, uint32_t sample) const;
};

#endif
