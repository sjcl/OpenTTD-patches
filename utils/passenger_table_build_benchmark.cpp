/* Isolated opt-in measurement TU. Never linked into the game or ordinary tests. */
#include "../src/stdafx.h"
#include "../src/3rdparty/catch2/catch.hpp"
#include "../src/passenger_service_database.h"
#include "../src/tests/passenger_test_stations.h"
#include "PassengerTableReference.h"
#include "PassengerTableCandidate.h"
#ifdef WITH_TABLE_BUILD_RANGE_SPLIT
#include "PassengerTableBeforeSIMD.h"
#include "PassengerTableSplitSIMD.h"
#endif
#ifdef WITH_TABLE_BUILD_HEAP_MATRIX
#include "PassengerTablePriority.h"
#include "PassengerTableFour.h"
#include "PassengerTablePacked.h"
#include "PassengerTableIndexed.h"
#include "PassengerTableRadix.h"
#include "PassengerTableGeneration.h"
#include "PassengerTablePlain.h"
#endif
#ifdef WITH_TABLE_BUILD_STAGES
#include "PassengerTableHotEdge.h"
#include "PassengerTableRanges.h"
#include "PassengerTableInit.h"
#include "PassengerTableBatch.h"
#include "PassengerTableContract.h"
#endif
#include <fstream>
#include <iostream>
#include <random>
#include "passenger_table_build_simd.h"

/** Inspect exact heap arrays without popping, rebuilding or normalizing them. */
template <typename Item> struct PassengerDestinationQueueInspector {
	static auto Snapshot(const PassengerDestinationQueue<Item> &queue)
	{
		using Buckets = std::array<std::vector<Item>, 65>;
		const auto *radix = queue.radix.get();
		std::array<size_t, 65> capacities{};
		if (radix) for (size_t i = 0; i < capacities.size(); ++i) capacities[i] = radix->buckets[i].capacity();
		return std::tuple(radix != nullptr, queue.legacy.c, radix ? radix->zero.c : std::vector<Item>{}, radix ? radix->buckets : Buckets{},
				radix ? radix->last : 0, radix ? radix->count : 0, radix ? radix->retained_limit : 0, radix ? radix->bucket_bytes : 0,
				queue.legacy.c.capacity(), radix ? radix->zero.c.capacity() : 0, capacities, queue.AllocatedBytes());
	}
};

namespace {
template <typename L> auto Values(const L &l)
{
	return std::tuple(l.cost, l.riding, l.waiting, l.first_wait, l.board, l.alight, l.legs, l.distance);
}
template <typename Row> bool StoredRowEmpty(const Row &row)
{
	if constexpr (requires { row.Empty(); }) return row.Empty();
	else return row.labels.empty();
}
template <typename T> size_t StoredRowBytes(const typename T::Row &row)
{
	if constexpr (requires { row.AllocatedBytes(); }) return row.AllocatedBytes();
	else return (row.labels.capacity() + row.alternative.capacity()) * sizeof(typename T::Label);
}
template <typename T> size_t PublicationPosition(const T &table)
{
	if constexpr (requires { table.publication_cursor; table.UseCompressedRows(); }) {
		if (table.UseCompressedRows()) return table.publication_cursor;
	}
	return table.prepared.labels.size();
}
template <typename T> const auto &StoredLabel(const T &table, const typename T::Row &row, uint32_t origin, uint32_t state)
{
	if constexpr (requires { table.RowLabel(row, origin, state); }) return table.RowLabel(row, origin, state);
	else return row.labels[state];
}
template <typename T> const auto &StoredAlternative(const T &table, const typename T::Row &row, uint32_t origin)
{
	if constexpr (requires { table.AlternativeLabel(row, origin); }) return table.AlternativeLabel(row, origin);
	else return row.alternative[origin];
}
template <typename T> void RowStatistics(const T &table, const char *name)
{
	uint64_t rows = 0, exceptions = 0, alternatives = 0, metadata_ties = 0, actual_payload = 0;
	uint64_t eligible = std::count_if(table.waiting_states.begin(), table.waiting_states.end(), [](const auto &states) { return !states.empty(); });
	for (const auto &row : table.rows) {
		if (StoredRowEmpty(row)) continue;
		++rows;
		if constexpr (requires { row.AllocatedBytes(); }) actual_payload += row.AllocatedBytes();
		else actual_payload += (row.labels.capacity() + row.alternative.capacity()) * sizeof(typename T::Label);
		for (uint32_t origin = 0; origin < table.stations.size(); ++origin) {
			const auto &base = StoredLabel(table, row, origin, origin);
			unsigned differences = 0;
			for (const auto &[pattern, state] : table.waiting_states[origin]) {
				const auto &label = StoredLabel(table, row, origin, state);
				if (Values(label) == Values(base)) continue;
				++differences;
				if (label.cost == base.cost && label.legs == base.legs) ++metadata_ties;
			}
			REQUIRE(differences <= 1); exceptions += differences;
			const auto &alt = StoredAlternative(table, row, origin);
			if (alt.board != T::INVALID) ++alternatives;
			else REQUIRE(Values(alt) == Values(typename T::Label{}));
		}
	}
	struct SparseException { uint32_t state; typename T::Label label; };
	const uint64_t cells = rows * table.stations.size(), labels = sizeof(typename T::Label), id = sizeof(uint32_t);
	const uint64_t dense = cells * (3 * labels + id);
	const uint64_t dense_eligible = cells * (2 * labels) + rows * eligible * (labels + id) + table.stations.size() * id;
	const uint64_t sparse_exception = cells * (2 * labels + id) + exceptions * sizeof(SparseException);
	const uint64_t sparse_both = cells * (labels + 2 * id) + exceptions * sizeof(SparseException) + alternatives * labels;
	const uint64_t dense_exception_sparse_alternative = cells * (labels + id) + rows * eligible * (labels + id) + alternatives * labels + table.stations.size() * id;
	std::cout << "row_distribution," << name << ',' << rows << ',' << table.stations.size() << ',' << cells << ',' << rows * eligible
		<< ',' << exceptions << ',' << alternatives << ',' << metadata_ties << ',' << actual_payload << ',' << dense << ',' << dense_eligible
		<< ',' << sparse_exception << ',' << sparse_both << ',' << sizeof(SparseException) << ',' << sizeof(std::vector<typename T::Label>) * 4 * table.rows.capacity()
		<< ',' << sizeof(std::vector<typename T::Label>) * 5 * table.rows.capacity() << ',' << dense_exception_sparse_alternative << std::endl;
}


template <typename A, typename B> void Compare(const A &a, const B &b)
{
	if constexpr (std::is_same_v<decltype(a.queue), decltype(b.queue)>) {
		if (a.integrated && b.integrated) {
		const auto &x = a.metrics; const auto &y = b.metrics;
		REQUIRE(std::tie(x.relaxations, x.successes, x.pushes, x.pops, x.stale, x.push_order, x.pop_order) ==
				std::tie(y.relaxations, y.successes, y.pushes, y.pops, y.stale, y.push_order, y.pop_order));
		REQUIRE(std::tie(x.copies, x.section_checks, x.peak_queue) == std::tie(y.copies, y.section_checks, y.peak_queue));
		REQUIRE(a.GetDiagnostic().operations == b.GetDiagnostic().operations);
		}
	}
	REQUIRE(a.rows.size() == b.rows.size());
	REQUIRE(a.stations == b.stations); REQUIRE(a.waiting_states == b.waiting_states);
	REQUIRE(a.waiting_state_count == b.waiting_state_count);
	for (size_t d = 0; d < a.rows.size(); ++d) {
		REQUIRE(StoredRowEmpty(a.rows[d]) == StoredRowEmpty(b.rows[d]));
		if (StoredRowEmpty(a.rows[d])) continue;
		for (uint32_t origin = 0; origin < a.stations.size(); ++origin) {
			REQUIRE(Values(StoredLabel(a, a.rows[d], origin, origin)) == Values(StoredLabel(b, b.rows[d], origin, origin)));
			for (const auto &[pattern, state] : a.waiting_states[origin]) {
				REQUIRE(Values(StoredLabel(a, a.rows[d], origin, state)) == Values(StoredLabel(b, b.rows[d], origin, state)));
			}
			REQUIRE(Values(StoredAlternative(a, a.rows[d], origin)) == Values(StoredAlternative(b, b.rows[d], origin)));
		}
	}
	REQUIRE(a.patterns.size() == b.patterns.size());
	for (const auto &pattern : a.patterns) for (const auto &member : pattern.members) for (uint32_t id : pattern.occurrences) {
		auto x = a.SectionBoardingKeys(member.id, a.occurrences[id].order), y = b.SectionBoardingKeys(member.id, a.occurrences[id].order);
		REQUIRE(std::equal(x.begin(), x.end(), y.begin(), y.end()));
	}
	/* Compare section IDs and every binding, not just the exported boarding key. */
	REQUIRE(a.sections.size() == b.sections.size());
	for (size_t i = 0; i < a.sections.size(); ++i) {
		const auto &x = a.sections[i]; const auto &y = b.sections[i];
		REQUIRE(x.key == y.key); REQUIRE(x.bindings.size() == y.bindings.size());
		for (size_t j = 0; j < x.bindings.size(); ++j) REQUIRE(std::tie(x.bindings[j].board, x.bindings[j].alight) == std::tie(y.bindings[j].board, y.bindings[j].alight));
	}
	REQUIRE(a.section_rides.size() == b.section_rides.size());
	for (size_t i = 0; i < a.section_rides.size(); ++i) {
		const auto &x = a.section_rides[i]; const auto &y = b.section_rides[i];
		REQUIRE(std::tie(x.board, x.alight, x.riding, x.waiting, x.cost) == std::tie(y.board, y.alight, y.riding, y.waiting, y.cost));
	}
	REQUIRE(a.landings == b.landings); REQUIRE(a.boarding_sections == b.boarding_sections);
	REQUIRE(a.section_paths.size() == b.section_paths.size());
	for (const auto &[key, x] : a.section_paths) {
		auto found = b.section_paths.find(key); REQUIRE(found != b.section_paths.end()); const auto &y = found->second;
		REQUIRE(std::tie(x.section, x.board, x.alight) == std::tie(y.section, y.board, y.alight));
	}
	if (getenv("OPENTTD_TABLE_SKIP_PLAN") != nullptr) return;
	for (size_t origin = 0; origin < a.stations.size(); ++origin) {
		/* Every excluded service that can change StateFor or Select is tested.
		 * All other valid IDs share the base state, disable dispersion in the
		 * same way, and cannot equal a selected boarding member. One represents
		 * that exact equivalence class, including IDs absent from the snapshot. */
		std::set<uint32_t> relevant;
		for (const auto &[pattern, state] : a.waiting_states[origin]) relevant.insert(pattern);
		for (const auto &occurrence : a.occurrences) if (occurrence.board && occurrence.station == a.stations[origin]) relevant.insert(occurrence.pattern);
		std::vector<OrderListID> exclusions{OrderListID::Invalid()};
		for (uint32_t pattern : relevant) for (const auto &member : a.patterns[pattern].members) exclusions.push_back(member.id);
		for (uint32_t id = 0; id < OrderListID::Invalid().base(); ++id) {
			if (id >= a.service_pattern.size() || !relevant.contains(a.service_pattern[id])) {
				exclusions.push_back(OrderListID{static_cast<uint16_t>(id)}); break;
			}
		}
		for (auto to : a.stations) for (uint32_t sample : {0U, 1U, UINT32_MAX / 2, UINT32_MAX}) for (OrderListID excluded : exclusions) {
			auto from = a.stations[origin];
			auto p = a.Plan(from, to, StateTicks{100}, excluded, sample), q = b.Plan(from, to, StateTicks{100}, excluded, sample);
			REQUIRE(p.state == q.state); REQUIRE(p.ride == q.ride);
			REQUIRE(p.estimated_service == q.estimated_service);
			REQUIRE(std::tie(p.cost, p.riding, p.waiting, p.distance, p.transfers) == std::tie(q.cost, q.riding, q.waiting, q.distance, q.transfers));
		}
	}
}
template <typename T> T Run(std::span<const PassengerPeriodicService *const> records, int64_t transfer, const char *name)
{
	T table;
	const auto comparisons = PassengerQueueGreater<typename T::QueueItem>::comparisons + PassengerQueueGreater<std::tuple<uint64_t, uint32_t>>::comparisons;
	auto started = std::chrono::steady_clock::now();
	table.Begin(records, CargoType{0}, transfer);
	auto graph = std::chrono::steady_clock::now();
	size_t quantum = 16 * 1024 * 1024;
	if (const char *value = getenv("OPENTTD_TABLE_QUANTUM")) quantum = std::max<size_t>(1, strtoull(value, nullptr, 10));
	size_t limit = 0;
	if (const char *value = getenv("OPENTTD_TABLE_MAX_DESTINATIONS")) limit = strtoull(value, nullptr, 10);
	while (!table.Finished()) {
		REQUIRE(table.Step(quantum) <= quantum);
		if (limit != 0 && table.GetDiagnostic().destinations >= limit) break;
	}
	auto finished = std::chrono::steady_clock::now();
	auto ns = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count(); };
	const auto &m = table.metrics; const auto &d = table.GetDiagnostic();
	std::cout << "table_window," << name << ',' << limit << ',' << d.destinations << std::endl;
	std::cout << "table_build," << name << ',' << records.size() << ',' << table.occurrences.size() << ',' << d.states << ',' << d.edges << ','
		<< ns(started, graph) << ',' << ns(graph, finished) << ',' << m.init_ns << ',' << m.search_ns << ',' << m.finish_ns << ',' << m.publish_ns << ','
		<< m.relaxations << ',' << m.successes << ',' << m.pushes << ',' << m.pops << ',' << m.stale << ',' << m.peak_queue << ',' << m.copies << ',' << m.section_checks << ',' << d.bytes << ',' << d.operations << std::endl;
	std::cout << "heap_stats," << name << ',' << PassengerQueueGreater<typename T::QueueItem>::comparisons + PassengerQueueGreater<std::tuple<uint64_t, uint32_t>>::comparisons - comparisons;
	if constexpr (requires { table.queue.decreases; }) std::cout << ',' << table.queue.pushes << ',' << table.queue.decreases;
	std::cout << std::endl;
	std::cout << "heap_sample," << name << ',' << m.heap_ns << ',' << m.heap_samples << std::endl;
	std::cout << "queue_order," << name << ',' << m.push_order << ',' << m.pop_order << std::endl;
	std::cout << "range_probes," << name << ',' << m.range_comparisons << ',' << m.invariant_comparisons << std::endl;
	std::cout << "alternative_probes," << name << ',' << m.alternative_base_reads << ',' << m.alternative_skipped << ',' << m.section_actual_reads << std::endl;
	if constexpr (requires { table.queue.PackedEnabled(); }) std::cout << "packed_bounds," << name << ',' << table.queue.PackedEnabled() << ',' << table.queue.LegBits() << std::endl;
	size_t rows_bytes = 0;
	for (const auto &row : table.rows) rows_bytes += StoredRowBytes<T>(row);
	auto row_times = m.destinations_ns; std::sort(row_times.begin(), row_times.end());
	std::cout << "memory," << name << ',' << rows_bytes << ',' << PassengerBenchmarkQueueBytes(table.queue) << ','
		<< (StoredRowBytes<T>(table.scratch) + StoredRowBytes<T>(table.prepared));
	if constexpr (requires { table.search; }) std::cout << ',' << table.search.cost.capacity() * 8 + table.search.legs.capacity() * 4 + table.search.metadata.capacity() * sizeof(typename T::LabelMetadata);
	std::cout << std::endl;
	if constexpr (requires { d.range_edges; }) std::cout << "graph_layout," << name << ',' << d.edges - d.range_edges << ',' << d.range_edges << ',' << d.logical_edges << ',' << m.csr_ns << std::endl;
	std::cout << "phases," << name << ',' << m.sections_ns;
	if (!row_times.empty()) std::cout << ',' << row_times.front() << ',' << row_times[row_times.size() / 2] << ',' << row_times[row_times.size() * 95 / 100] << ',' << row_times.back();
	std::cout << std::endl;
	if (getenv("OPENTTD_TABLE_ROW_STATS") != nullptr) RowStatistics(table, name);
	return table;
}

template <typename Before, typename After> void CompareRangeSlices()
{
	if constexpr (requires(Before t) { t.RelaxIncoming(1); }) {
		for (uint32_t excluded : {UINT32_MAX, 5U, 6U, 8U, 10U, 14U, 15U}) for (unsigned overflow = 0; overflow < 3; ++overflow) for (unsigned shape = 0; shape < 3; ++shape) {
			for (size_t quantum : {1U, 2U, 3U, 4U, 5U, 7U, 64U}) {
				CAPTURE(excluded, overflow, shape, quantum);
				Before before; After after;
				auto init = [&]<typename T>(T &t) {
					t.search.cost.resize(24); t.search.legs.resize(24); t.search.metadata.resize(24);
					for (uint32_t s = 0; s < 24; ++s) {
						typename T::Label label;
						label.cost = s % 4 == 0 ? 103 : s % 4 == 1 ? 104 : INT64_MAX;
						label.legs = s % 4 == 1 ? 3 : 0; t.SetSearchLabel(s, label);
					}
					/* Only one wait may differ from each base at descriptor entry. */
					typename T::Label base{shape == 0 ? 103 : shape == 1 ? 105 : 104, 13, 9, 2, 1, 2, shape == 2 ? 2U : 0U, 9};
					t.SetSearchLabel(2, base); t.stations.resize(6); t.waiting_states.resize(6);
					for (uint32_t state = 6; state < 15; ++state) { t.SetSearchLabel(state, base); t.waiting_states[2].emplace_back(0, state); }
					auto exception = base; exception.cost += shape == 2 ? 0 : 2; exception.legs = shape == 2 ? 3 : base.legs; ++exception.first_wait;
					t.SetSearchLabel(8, exception);
					base.cost = 104; t.SetSearchLabel(4, base);
					for (uint32_t state = 16; state < 20; ++state) { t.SetSearchLabel(state, base); t.waiting_states[4].emplace_back(0, state); }
					if constexpr (requires { t.exception_state; }) { t.exception_state.assign(6, UINT32_MAX); t.exception_state[2] = 8; }
					typename T::Label source{overflow == 1 ? INT64_MAX - 2 : 100, 50, 7, 3, 1, 2, overflow == 2 ? UINT32_MAX : 2, UINT32_MAX - 2};
					t.SetSearchLabel(23, source); t.expanding = 23;
					t.incoming_offsets.assign(25, 0); t.incoming_offsets.back() = 3;
					t.hot_edges.from = {2, 3, 4}; t.hot_edges.cost = {4, 1, 3};
					t.hot_edges.legs.assign(3, overflow == 2 ? 1 : 0);
					t.hot_edges.range = {0, UINT32_MAX, 1};
					t.range_edges = {{6, 15, excluded}, {16, 20, UINT32_MAX}};
					t.cold_edges.assign(3, typename T::ColdEdge{0, 8, 1, T::EdgeKind::Ride});
					t.queue.SetOptimized(true); t.queue.SetCapacityLimit(64);
				};
				init(before); init(after); size_t total = 0;
				while (before.cursor != 3) {
					size_t a = before.RelaxIncoming(quantum), b = after.RelaxIncoming(quantum);
					REQUIRE(a > 0); REQUIRE(a <= quantum); REQUIRE(a == b); total += a;
					REQUIRE(before.cursor == after.cursor); REQUIRE(before.range_offset == after.range_offset);
					REQUIRE(before.queue.size() == after.queue.size());
					REQUIRE(PassengerDestinationQueueInspector<typename Before::QueueItem>::Snapshot(before.queue) == PassengerDestinationQueueInspector<typename After::QueueItem>::Snapshot(after.queue));
					for (uint32_t s = 0; s < 24; ++s) REQUIRE(Values(before.SearchLabel(s)) == Values(after.SearchLabel(s)));
				}
				REQUIRE(total == 16 - (excluded >= 6 && excluded < 15));
				while (!before.queue.empty()) {
					REQUIRE(before.queue.top() == after.queue.top()); before.queue.pop(); after.queue.pop();
				}
				REQUIRE(after.queue.empty());
			}
		}
	}
}

TEST_CASE("Range slicing retains excluded-hole cursors and exact work at every boundary", "[.table-ranges]")
{
	CompareRangeSlices<PassengerTableReference, PassengerTableCandidate>();
#ifdef WITH_TABLE_BUILD_RANGE_SPLIT
	CompareRangeSlices<PassengerTableReference, PassengerTableBeforeSIMD>();
	CompareRangeSlices<PassengerTableReference, PassengerTableSplitSIMD>();
#endif
}

TEST_CASE("Experimental four-way and packed queues retain the complete ordering", "[.table-queues]")
{
	using Item = std::tuple<int64_t, uint32_t, uint32_t>;
	std::mt19937 rng(713);
	for (bool wide : {false, true}) {
		PassengerFourQueue<Item> four;
		PassengerPackedFourQueue<Item> packed; packed.SetBounds(1000, wide ? INT64_MAX : 1000000);
		std::priority_queue<Item, std::vector<Item>, std::greater<Item>> reference;
		for (unsigned i = 0; i < 20000; ++i) {
			Item item(wide ? int64_t(rng()) << 30 : rng() % 1000, rng() % 300, rng() % 1000);
			four.emplace(item); packed.emplace(item); reference.push(item);
			if (i % 3 == 0) {
				REQUIRE(four.top() == reference.top()); REQUIRE(packed.top() == reference.top());
				four.pop(); packed.pop(); reference.pop();
			}
		}
		/* Force the lossless runtime fallback after packed entries exist. */
		Item extreme(INT64_MAX - 1, UINT32_MAX, 0);
		four.emplace(extreme); packed.emplace(extreme); reference.push(extreme);
		while (!reference.empty()) {
			REQUIRE(four.top() == reference.top()); REQUIRE(packed.top() == reference.top());
			four.pop(); packed.pop(); reference.pop();
		}
	}
}

TEST_CASE("Experimental AVX2 masks equal scalar masks including wide legs", "[.table-queues]")
{
	std::cout << "range_simd,avx2_available," << PassengerHasAVX2() << std::endl;
	std::mt19937 rng(113);
	for (unsigned i = 0; i < 10000; ++i) {
		int64_t cost = i % 2 ? rng() % 16 : INT64_MAX - rng() % 16;
		uint32_t legs = rng(); int64_t old_cost[4]; uint32_t old_legs[4];
		for (unsigned j = 0; j < 4; ++j) { old_cost[j] = j % 2 ? cost : cost - 1; old_legs[j] = rng(); }
		REQUIRE(PassengerRangeMask(cost, legs, old_cost, old_legs) == PassengerScalarMask(cost, legs, old_cost, old_legs));
	}
}

std::vector<PassengerPeriodicService> Synthetic(uint32_t seed, size_t count, size_t stops, uint32_t stations)
{
	std::mt19937 rng(seed);
	std::vector<PassengerPeriodicService> result;
	for (size_t i = 0; i < count; ++i) {
		PassengerPeriodicService s; s.id = OrderListID{static_cast<uint16_t>(i)}; s.incarnation = i + 1; s.generation = i + 100;
		int64_t time = 0;
		for (size_t j = 0; j < stops; ++j) {
			uint64_t board = rng() % 4 != 0, alight = rng() % 4 != 0, forced = rng() % 20 == 0;
			int64_t dwell = rng() % 3 == 0 ? 0 : rng() % 15;
			s.timetable.stops.push_back({StationID{static_cast<uint16_t>(rng() % stations)}, static_cast<VehicleOrderID>(j), time, time + dwell, board, alight, forced, rng() % 20 != 0, static_cast<uint8_t>(rng() % 3)});
			time += dwell + rng() % 30 + 1;
		}
		if (seed % 3 == 0 || (seed % 3 == 2 && i % 2 == 0)) s.timetable.period = time;
		else s.timetable.estimated_period = time;
		PassengerPeriodicVehicle v; v.id = VehicleID{static_cast<uint32_t>(i)}; v.type = VehicleType::Train; v.capacity[CargoType{0}] = 100; v.phase_known = seed % 2 == 0;
		s.vehicles.push_back(v); result.push_back(std::move(s));
	}
	return result;
}
}

TEST_CASE("Precomputed build independent reference comparison", "[.table-build][passenger]")
{
	PassengerTestStations fixture;
	for (uint32_t id = 0; id < 64; ++id) Station::CreateAtIndex(StationID{static_cast<uint16_t>(id)}, TileIndex{256 * (id / 8 + 1) + id % 8 + 1});
	for (uint32_t seed = 0; seed < 80; ++seed) {
		CAPTURE(seed);
		auto services = Synthetic(seed, 12, 6, 12);
		std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
		auto reference = Run<PassengerTableReference>(records, seed % 3, "reference");
		auto candidate = Run<PassengerTableCandidate>(records, seed % 3, "candidate");
		Compare(reference, candidate);
#ifdef WITH_TABLE_BUILD_HEAP_MATRIX
		if (getenv("OPENTTD_TABLE_GENERATION")) {
			auto generation = Run<PassengerTableGeneration>(records, seed % 3, "generation"); Compare(reference, generation);
		}
#endif
	}
}

template <typename Before, typename After> void CompareEverySlice(std::span<const PassengerPeriodicService *const> records, int64_t transfer, size_t quantum)
{
	if constexpr (requires(Before table) { table.RelaxIncoming(1); }) {
		{
			Before before; After after;
			before.Begin(records, CargoType{0}, transfer, Before::MAX_BYTES, true, true);
			after.Begin(records, CargoType{0}, transfer, After::MAX_BYTES, true, true);
			while (before.IsPreparingGraph()) {
				REQUIRE(before.PrepareGraph(quantum) == after.PrepareGraph(quantum));
				REQUIRE(before.GetGraphWork() == after.GetGraphWork());
			}
			REQUIRE_FALSE(after.IsPreparingGraph()); Compare(before, after);
		}
		Before before; After after;
		before.Begin(records, CargoType{0}, transfer); after.Begin(records, CargoType{0}, transfer);
		std::vector<uint32_t> state_origins(before.waiting_state_count);
		for (uint32_t origin = 0; origin < before.stations.size(); ++origin) {
			state_origins[origin] = origin;
			for (const auto &[pattern, state] : before.waiting_states[origin]) state_origins[state] = origin;
		}
		while (!before.Finished()) {
			REQUIRE(before.Step(quantum) == after.Step(quantum));
			REQUIRE(before.Finished() == after.Finished());
			REQUIRE(std::tie(before.destination, before.cursor, before.range_offset, before.expanding, before.finishing,
					before.finishing_row, before.initializing_row, before.seeding) ==
					std::tie(after.destination, after.cursor, after.range_offset, after.expanding, after.finishing,
					after.finishing_row, after.initializing_row, after.seeding));
			REQUIRE(before.search.cost == after.search.cost); REQUIRE(before.search.legs == after.search.legs);
			bool metadata_equal = true;
			for (uint32_t state = 0; state < before.search.cost.size(); ++state) {
				if (Values(before.SearchLabel(state)) != Values(after.SearchLabel(state))) { metadata_equal = false; break; }
			}
			REQUIRE(metadata_equal);
			if constexpr (!requires { after.alternative_candidates; } && !requires { after.publication_cursor; }) {
				REQUIRE(std::equal(before.scratch.alternative.begin(), before.scratch.alternative.end(), after.scratch.alternative.begin(), after.scratch.alternative.end(),
						[](const auto &a, const auto &b) { return Values(a) == Values(b); }));
			}
			REQUIRE(PassengerDestinationQueueInspector<typename Before::QueueItem>::Snapshot(before.queue) == PassengerDestinationQueueInspector<typename After::QueueItem>::Snapshot(after.queue));
			const size_t copied = PublicationPosition(before);
			REQUIRE(copied == PublicationPosition(after));
			bool prepared_equal = true;
			for (uint32_t state = 0; state < copied; ++state) {
				uint32_t origin = state_origins[state];
				if (Values(StoredLabel(before, before.prepared, origin, state)) != Values(StoredLabel(after, after.prepared, origin, state))) { prepared_equal = false; break; }
			}
			REQUIRE(prepared_equal);
			const auto &a = before.metrics; const auto &b = after.metrics;
			REQUIRE(std::tie(a.relaxations, a.successes, a.pushes, a.pops, a.stale, a.copies, a.section_checks, a.push_order, a.pop_order) ==
					std::tie(b.relaxations, b.successes, b.pushes, b.pops, b.stale, b.copies, b.section_checks, b.push_order, b.pop_order));
			REQUIRE(before.GetDiagnostic().operations == after.GetDiagnostic().operations);
		}
		Compare(before, after);
	}
}

TEST_CASE("Integrated range cache preserves labels heap and work at every real slice", "[.table-slices][passenger]")
{
	PassengerTestStations fixture;
	for (uint32_t id = 0; id < 64; ++id) Station::CreateAtIndex(StationID{static_cast<uint16_t>(id)}, TileIndex{256 * (id / 8 + 1) + id % 8 + 1});
	for (uint32_t seed = 0; seed < 80; ++seed) {
		auto services = Synthetic(seed, 12, 6, 12);
		std::vector<const PassengerPeriodicService *> records; for (const auto &service : services) records.push_back(&service);
		for (size_t quantum : {1U, 2U, 3U, 4U, 5U, 7U, 64U}) {
			CAPTURE(seed, quantum);
			CompareEverySlice<PassengerTableReference, PassengerTableCandidate>(records, seed % 3, quantum);
		}
	}
}

TEST_CASE("Precomputed build fixed synthetic timing", "[.table-timing][passenger]")
{
	PassengerTestStations fixture;
	for (uint32_t id = 0; id < 256; ++id) Station::CreateAtIndex(StationID{static_cast<uint16_t>(id)}, TileIndex{256 * (id / 16 + 1) + id % 16 + 1});
	for (uint32_t seed : {0U, 1U, 2U}) {
		auto services = Synthetic(seed, 500, 12, 256);
		std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
		auto reference = Run<PassengerTableReference>(records, 20, "reference");
		auto candidate = Run<PassengerTableCandidate>(records, 20, "candidate");
		Compare(reference, candidate);
	}
}

TEST_CASE("Precomputed build fixed real map snapshot", "[.table-real][passenger]")
{
	const char *path = getenv("OPENTTD_TABLE_DATABASE"); REQUIRE(path != nullptr);
	std::ifstream file(path, std::ios::binary); REQUIRE(file.good());
	std::vector<uint8_t> wire{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	PassengerServiceDatabase database; REQUIRE(database.Load(wire));
	PassengerTestStations fixture;
	std::vector<const PassengerPeriodicService *> records;
	for (uint32_t id = 0; id < OrderListID::Invalid().base(); ++id) if (auto *service = database.Get(OrderListID{static_cast<uint16_t>(id)})) records.push_back(service);
	/* Coordinates are exported independently of the routing DB. Missing
	 * coordinates are reported explicitly; cost/timing still use real input. */
	if (const char *coordinates = getenv("OPENTTD_TABLE_STATIONS")) {
		std::ifstream positions(coordinates); uint32_t width, height, id, x, y; REQUIRE(bool(positions >> width >> height));
		AllocateMap(width, height);
		while (positions >> id >> x >> y) Station::CreateAtIndex(StationID{static_cast<uint16_t>(id)}, TileIndex{width * y + x});
	} else std::cout << "limitation,real-map station coordinates absent; distances are zero\n";
#ifdef WITH_TABLE_BUILD_RANGE_SPLIT
	if (getenv("OPENTTD_TABLE_RANGE_SPLIT") != nullptr) {
		/* Keep the same completed reference resident for every timed variant. */
		auto reference = Run<PassengerTableReference>(records, 20, "reference-warm-real");
		auto before = [&] { auto t = Run<PassengerTableReference>(records, 20, "before-scalar-real"); Compare(reference, t); };
		auto before_simd = [&] { auto t = Run<PassengerTableBeforeSIMD>(records, 20, "before-simd-real"); Compare(reference, t); };
		auto after = [&] { auto t = Run<PassengerTableCandidate>(records, 20, "split-scalar-real"); Compare(reference, t); };
		auto after_simd = [&] { auto t = Run<PassengerTableSplitSIMD>(records, 20, "split-simd-real"); Compare(reference, t); };
		if (getenv("OPENTTD_TABLE_REVERSE_ORDER") != nullptr) { after_simd(); after(); before_simd(); before(); }
		else { before(); before_simd(); after(); after_simd(); }
		return;
	}
#endif
	if (getenv("OPENTTD_TABLE_PAIRED_ORDER") != nullptr) {
		/* Both timed variants retain exactly the same completed warm table. */
		auto warm = Run<PassengerTableReference>(records, 20, "warm-real");
		auto before = [&] { auto t = Run<PassengerTableReference>(records, 20, "reference-real"); Compare(warm, t); };
		auto after = [&] { auto t = Run<PassengerTableCandidate>(records, 20, "candidate-real"); Compare(warm, t); };
		if (getenv("OPENTTD_TABLE_REVERSE_ORDER") != nullptr) { after(); before(); }
		else { before(); after(); }
		return;
	}
	if (getenv("OPENTTD_TABLE_REVERSE_ORDER") != nullptr) {
		auto candidate = Run<PassengerTableCandidate>(records, 20, "candidate-real");
		auto reference = Run<PassengerTableReference>(records, 20, "reference-real");
		Compare(reference, candidate); return;
	}
	auto reference = Run<PassengerTableReference>(records, 20, "reference-real");
#ifdef WITH_TABLE_BUILD_STAGES
	if (getenv("OPENTTD_TABLE_STAGES") != nullptr) {
		{ auto table = Run<PassengerTableHotEdge>(records, 20, "hot-edge-real"); Compare(reference, table); }
		{ auto table = Run<PassengerTableRanges>(records, 20, "range-real"); Compare(reference, table); }
		{ auto table = Run<PassengerTableInit>(records, 20, "init-real"); Compare(reference, table); }
		{ auto table = Run<PassengerTableBatch>(records, 20, "batch-real"); Compare(reference, table); }
		{ auto table = Run<PassengerTableCandidate>(records, 20, "simd-real"); Compare(reference, table); }
		{ auto table = Run<PassengerTableContract>(records, 20, "contract-real"); Compare(reference, table); }
		return;
	}
#endif
#ifdef WITH_TABLE_BUILD_HEAP_MATRIX
	if (getenv("OPENTTD_TABLE_EXPERIMENTS") != nullptr) {
		{ auto plain = Run<PassengerTablePlain>(records, 20, "plain-product-real"); Compare(reference, plain); }
		{ auto simd = Run<PassengerTableCandidate>(records, 20, "simd2-real"); Compare(reference, simd); }
		{ auto generation = Run<PassengerTableGeneration>(records, 20, "generation-real"); Compare(reference, generation); }
		return;
	}
	if (getenv("OPENTTD_TABLE_HEAP_MATRIX") != nullptr) {
		{ auto priority = Run<PassengerTablePriority>(records, 20, "priority-real"); Compare(reference, priority); }
		{ auto four = Run<PassengerTableFour>(records, 20, "four-real"); Compare(reference, four); }
		{ auto packed = Run<PassengerTablePacked>(records, 20, "packed-real"); Compare(reference, packed); }
		{ auto indexed = Run<PassengerTableIndexed>(records, 20, "indexed-real"); Compare(reference, indexed); }
		{ auto radix = Run<PassengerTableRadix>(records, 20, "radix-real"); Compare(reference, radix); }
		{ auto generation = Run<PassengerTableGeneration>(records, 20, "generation-real"); Compare(reference, generation); }
		return;
	}
#endif
	auto candidate = Run<PassengerTableCandidate>(records, 20, "candidate-real");
	Compare(reference, candidate);
}

TEST_CASE("Compressed rows retain canonical admission and legacy publication work", "[.table-rows][passenger]")
{
	PassengerTestStations fixture;
	for (uint32_t id = 0; id < 32; ++id) Station::CreateAtIndex(StationID{static_cast<uint16_t>(id)}, TileIndex{256 + id + 1});
	auto services = Synthetic(73, 8, 6, 12);
	std::vector<const PassengerPeriodicService *> records; for (const auto &service : services) records.push_back(&service);
	for (unsigned algorithm : {0U, 1U, 2U}) {
		PassengerTableReference complete;
		complete.Begin(records, CargoType{0}, 20, complete.MAX_BYTES, true, false, {}, true, algorithm != 0, algorithm == 2);
		const uint64_t envelope = complete.GetDiagnostic().envelope;
		for (uint64_t budget : {uint64_t{1}, uint64_t{1024 * 1024}, envelope - 1, envelope, complete.MAX_BYTES}) {
			PassengerTableReference before; PassengerTableCandidate after;
			before.Begin(records, CargoType{0}, 20, budget, true, false, {}, true, algorithm != 0, algorithm == 2);
			after.Begin(records, CargoType{0}, 20, budget, true, false, {}, true, algorithm != 0, algorithm == 2);
			REQUIRE(before.MemoryRejected() == after.MemoryRejected());
			REQUIRE(before.GetDiagnostic().envelope == after.GetDiagnostic().envelope);
			if (before.MemoryRejected()) continue;
			while (!before.Finished()) {
				REQUIRE(before.Step(5, true) == after.Step(5, true));
				REQUIRE(before.GetDiagnostic().operations == after.GetDiagnostic().operations);
				REQUIRE(before.GetDiagnostic().destinations == after.GetDiagnostic().destinations);
				REQUIRE(before.LegacyPublicationWork() == after.LegacyPublicationWork());
			}
			REQUIRE(after.Finished()); Compare(before, after);
		}
	}
}
