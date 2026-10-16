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

namespace {
template <typename L> auto Values(const L &l)
{
	return std::tuple(l.cost, l.riding, l.waiting, l.first_wait, l.board, l.alight, l.legs, l.distance);
}
template <typename A, typename B> void Compare(const A &a, const B &b)
{
	REQUIRE(a.rows.size() == b.rows.size());
	for (size_t d = 0; d < a.rows.size(); ++d) {
		REQUIRE(a.rows[d].labels.size() == b.rows[d].labels.size());
		for (size_t s = 0; s < a.rows[d].labels.size(); ++s) {
			if (Values(a.rows[d].labels[s]) != Values(b.rows[d].labels[s])) {
				const auto &x = a.rows[d].labels[s]; const auto &y = b.rows[d].labels[s];
				std::cerr << "label_mismatch," << d << ',' << s << ',' << x.cost << ',' << y.cost << ',' << x.legs << ',' << y.legs << ',' << x.board << ',' << y.board << ',' << x.alight << ',' << y.alight << std::endl;
			}
			REQUIRE(Values(a.rows[d].labels[s]) == Values(b.rows[d].labels[s]));
		}
		for (size_t s = 0; s < a.rows[d].alternative.size(); ++s) REQUIRE(Values(a.rows[d].alternative[s]) == Values(b.rows[d].alternative[s]));
	}
	REQUIRE(a.patterns.size() == b.patterns.size());
	for (const auto &pattern : a.patterns) for (const auto &member : pattern.members) for (uint32_t id : pattern.occurrences) {
		auto x = a.SectionBoardingKeys(member.id, a.occurrences[id].order), y = b.SectionBoardingKeys(member.id, a.occurrences[id].order);
		REQUIRE(std::equal(x.begin(), x.end(), y.begin(), y.end()));
	}
	if (getenv("OPENTTD_TABLE_SKIP_PLAN") != nullptr) return;
	for (auto from : a.stations) for (auto to : a.stations) for (uint32_t sample : {0U, 1U, UINT32_MAX / 2, UINT32_MAX}) {
		for (OrderListID excluded : {OrderListID::Invalid(), OrderListID{0}, OrderListID{1}, OrderListID{7}}) {
			auto p = a.Plan(from, to, StateTicks{100}, excluded, sample), q = b.Plan(from, to, StateTicks{100}, excluded, sample);
			REQUIRE(p.state == q.state); REQUIRE(p.ride == q.ride);
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
	if constexpr (requires { table.queue.PackedEnabled(); }) std::cout << "packed_bounds," << name << ',' << table.queue.PackedEnabled() << ',' << table.queue.LegBits() << std::endl;
	size_t rows_bytes = 0;
	for (const auto &row : table.rows) rows_bytes += (row.labels.capacity() + row.alternative.capacity()) * sizeof(typename T::Label);
	auto row_times = m.destinations_ns; std::sort(row_times.begin(), row_times.end());
	std::cout << "memory," << name << ',' << rows_bytes << ',' << PassengerBenchmarkQueueBytes(table.queue) << ','
		<< (table.scratch.labels.capacity() + table.scratch.alternative.capacity() + table.prepared.labels.capacity()) * sizeof(typename T::Label);
	if constexpr (requires { table.search; }) std::cout << ',' << table.search.cost.capacity() * 8 + table.search.legs.capacity() * 4 + table.search.metadata.capacity() * sizeof(typename T::LabelMetadata);
	std::cout << std::endl;
	if constexpr (requires { d.range_edges; }) std::cout << "graph_layout," << name << ',' << d.edges - d.range_edges << ',' << d.range_edges << ',' << d.logical_edges << ',' << m.csr_ns << std::endl;
	std::cout << "phases," << name << ',' << m.sections_ns;
	if (!row_times.empty()) std::cout << ',' << row_times.front() << ',' << row_times[row_times.size() / 2] << ',' << row_times[row_times.size() * 95 / 100] << ',' << row_times.back();
	std::cout << std::endl;
	return table;
}

template <typename Before, typename After> void CompareRangeSlices()
{
	if constexpr (requires(Before t) { t.RelaxIncoming(1); }) {
		for (uint32_t excluded : {UINT32_MAX, 5U, 6U, 10U, 14U, 15U}) for (unsigned overflow = 0; overflow < 3; ++overflow) {
			for (size_t quantum : {1U, 2U, 3U, 4U, 5U, 7U, 64U}) {
				CAPTURE(excluded, overflow, quantum);
				Before before; After after;
				auto init = [&]<typename T>(T &t) {
					t.search.cost.resize(24); t.search.legs.resize(24); t.search.metadata.resize(24);
					for (uint32_t s = 0; s < 24; ++s) {
						typename T::Label label;
						label.cost = s % 4 == 0 ? 103 : s % 4 == 1 ? 104 : INT64_MAX;
						label.legs = s % 4 == 1 ? 3 : 0; t.SetSearchLabel(s, label);
					}
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
