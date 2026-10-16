/* Isolated opt-in measurement TU. Never linked into the game or ordinary tests. */
#include "../src/stdafx.h"
#include "../src/3rdparty/catch2/catch.hpp"
#include "../src/passenger_service_database.h"
#include "../src/tests/passenger_test_stations.h"
#include "PassengerTableReference.h"
#include "PassengerTableCandidate.h"
#include <fstream>
#include <iostream>
#include <random>

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
		for (size_t s = 0; s < a.rows[d].labels.size(); ++s) REQUIRE(Values(a.rows[d].labels[s]) == Values(b.rows[d].labels[s]));
		for (size_t s = 0; s < a.rows[d].alternative.size(); ++s) REQUIRE(Values(a.rows[d].alternative[s]) == Values(b.rows[d].alternative[s]));
	}
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
	auto started = std::chrono::steady_clock::now();
	table.Begin(records, CargoType{0}, transfer);
	auto graph = std::chrono::steady_clock::now();
	while (!table.Finished()) table.Step(16 * 1024 * 1024);
	auto finished = std::chrono::steady_clock::now();
	auto ns = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count(); };
	const auto &m = table.metrics; const auto &d = table.GetDiagnostic();
	std::cout << "table_build," << name << ',' << records.size() << ',' << table.occurrences.size() << ',' << d.states << ',' << d.edges << ','
		<< ns(started, graph) << ',' << ns(graph, finished) << ',' << m.init_ns << ',' << m.search_ns << ',' << m.finish_ns << ',' << m.publish_ns << ','
		<< m.relaxations << ',' << m.successes << ',' << m.pushes << ',' << m.pops << ',' << m.stale << ',' << m.peak_queue << ',' << m.copies << ',' << m.section_checks << ',' << d.bytes << ',' << d.operations << '\n';
	return table;
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
		std::ifstream positions(coordinates); uint32_t id, x, y;
		while (positions >> id >> x >> y) Station::CreateAtIndex(StationID{static_cast<uint16_t>(id)}, TileIndex{256 * y + x});
	} else std::cout << "limitation,real-map station coordinates absent; distances are zero\n";
	auto reference = Run<PassengerTableReference>(records, 20, "reference-real");
	auto candidate = Run<PassengerTableCandidate>(records, 20, "candidate-real");
	Compare(reference, candidate);
}
