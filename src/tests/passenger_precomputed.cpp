/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */
#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../passenger_precomputed.h"
#include "../passenger_service_database.h"
#include "passenger_test_stations.h"
#include <chrono>
#include <fstream>
#include <iostream>

namespace {
	constexpr CargoType PAX{0};
	constexpr StationID A{0}, B{1}, C{2}, D{3};
	PassengerPeriodicService Service(uint16_t id, std::initializer_list<StationID> stops, int64_t travel = 100)
	{
		PassengerPeriodicService s; s.id = OrderListID{id}; s.incarnation = id + 1; s.generation = id + 10;
		int64_t cursor = 0;
		for (StationID station : stops) {
			s.timetable.stops.push_back({station, static_cast<VehicleOrderID>(s.timetable.stops.size()), cursor, cursor + 10, 1, 1, 0});
			cursor += travel + 10;
		}
		s.timetable.period = cursor;
		PassengerPeriodicVehicle v; v.id = VehicleID{id}; v.type = VehicleType::Train; v.phase_known = true; v.capacity[PAX] = 100;
		s.vehicles.push_back(v); return s;
	}
	PassengerPrecomputedTable Build(std::span<const PassengerPeriodicService> services, size_t chunk = 65536, int64_t transfer = 20)
	{
		std::vector<const PassengerPeriodicService *> records;
		for (const auto &s : services) records.push_back(&s);
		PassengerPrecomputedTable table; table.Begin(records, PAX, transfer);
		for (size_t i = 0; !table.Finished() && i < 1000000; ++i) table.Step(chunk);
		REQUIRE(table.Finished()); return table;
	}
}

TEST_CASE("Hyperpath fixed point preserves single waits and exact acceptance", "[passenger][hyperpath]")
{
	for (int64_t wait : {1LL, 2LL, 3LL, 100LL, 65537LL, PassengerStrategy::MAX_WAIT}) {
		PassengerStrategy sum;
		REQUIRE(sum.Add(1234, PassengerStrategy::Frequency(wait)));
		CHECK(sum.Cost() == wait + 1234); CHECK(sum.Wait() == wait);
		CHECK_FALSE(sum.Attractive(sum.Cost()));
	}
	PassengerStrategy sum;
	REQUIRE(sum.Add(100, PassengerStrategy::Frequency(100)));
	CHECK(sum.Cost() == 200);
	REQUIRE(sum.Add(150, PassengerStrategy::Frequency(100)));
	CHECK(sum.Cost() == 175);
	CHECK_FALSE(sum.Add(175, PassengerStrategy::Frequency(1)));
	PassengerStrategy many;
	for (size_t i = 0; i < 9; ++i) REQUIRE(many.Add(100, PassengerStrategy::Frequency(100)));
	CHECK(many.Cost() == 112); // ceil(100 + 100/9), from the independent Fraction oracle.
	CHECK(PassengerWide::Multiply(UINT64_MAX, UINT64_MAX) == PassengerWide{UINT64_MAX - 1, 1});
}

TEST_CASE("Hyperpath frozen real map intermediate diagnostic", "[.hyperpath-real][passenger]")
{
	const char *path = getenv("OPENTTD_TABLE_DATABASE"); REQUIRE(path != nullptr);
	std::ifstream file(path, std::ios::binary); REQUIRE(file.good());
	std::vector<uint8_t> wire{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	PassengerServiceDatabase database; REQUIRE(database.Load(wire));
	PassengerTestStations fixture;
	if (const char *coordinates = getenv("OPENTTD_TABLE_STATIONS")) {
		std::ifstream positions(coordinates); uint32_t width, height, id, x, y; REQUIRE(bool(positions >> width >> height));
		AllocateMap(width, height);
		while (positions >> id >> x >> y) Station::CreateAtIndex(StationID{static_cast<uint16_t>(id)}, TileIndex{width * y + x});
	}
	std::vector<const PassengerPeriodicService *> records;
	for (uint32_t id = 0; id < OrderListID::Invalid().base(); ++id) if (auto *service = database.Get(OrderListID{static_cast<uint16_t>(id)})) records.push_back(service);
	std::array<std::pair<StationID, StationID>, 3> ods{{{StationID{3222}, StationID{400}}, {StationID{971}, StationID{916}}, {StationID{3393}, StationID{3394}}}};
	std::array<int64_t, 3> old_cost{};
	{
		PassengerPrecomputedTable old; old.Begin(records, PAX, 20);
		while (!old.Finished()) old.Step(16 * 1024 * 1024);
		for (size_t i = 0; i < ods.size(); ++i) old_cost[i] = old.RouteCost(ods[i].first, ods[i].second).value_or(INT64_MAX);
	}
	PassengerPrecomputedTable table;
	auto start = std::chrono::steady_clock::now();
	table.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, true);
	while (!table.Finished()) table.Step(16 * 1024 * 1024);
	const auto &d = table.GetDiagnostic();
	std::cout << "hyperpath_build," << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()
			<< ',' << d.operations << ',' << d.envelope << ',' << d.bytes << ',' << d.board_events << ',' << d.board_accepts << ',' << d.range_updates
			<< ',' << d.heap_pushes << ',' << d.heap_pops << ',' << d.base_policy_bytes << ',' << d.difference_policy_bytes << ',' << d.max_candidates << std::endl;
	REQUIRE_FALSE(table.MemoryRejected());
	for (const auto &[size, count] : d.candidate_histogram) std::cout << "hyperpath_histogram," << size << ',' << count << std::endl;
	for (size_t i = 0; i < ods.size(); ++i) {
		auto [from, to] = ods[i]; auto plan = table.Plan(from, to, StateTicks{});
		std::cout << "hyperpath_od," << from.base() << ',' << to.base() << ',' << old_cost[i] << ',' << plan.cost << std::endl;
		for (const auto &candidate : table.Inspect(from, to)) std::cout << "hyperpath_candidate," << from.base() << ',' << to.base() << ',' << candidate.occurrence << ','
				<< candidate.service.base() << ',' << candidate.order << ',' << candidate.alight.base() << ',' << candidate.cost << ',' << candidate.wait << ',' << candidate.frequency << std::endl;
	}
}

TEST_CASE("Hyperpath keeps all attractive real boarding occurrences", "[passenger][hyperpath]")
{
	std::vector<PassengerPeriodicService> services;
	for (uint16_t i = 0; i < 9; ++i) services.push_back(Service(i, {A, B, D}, 100 + i));
	services.push_back(Service(9, {A, C}, 100)); // A short turn cannot become a through ride to D.
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	uint64_t work = 0;
	for (size_t chunk : {size_t{1}, size_t{7}, size_t{65536}}) {
		PassengerPrecomputedTable table;
		table.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, true);
		while (!table.Finished()) table.Step(chunk);
		REQUIRE_FALSE(table.MemoryRejected());
		CAPTURE(chunk);
		if (work == 0) work = table.GetDiagnostic().operations;
		CHECK(work == table.GetDiagnostic().operations);
		auto set = table.Inspect(A, D);
		REQUIRE(set.size() == 9);
		CHECK(table.GetDiagnostic().section_rides == 0);
		CHECK(table.GetDiagnostic().max_candidates >= 9);
		auto plan = table.Plan(A, D, StateTicks{42}); REQUIRE(plan.ride); CHECK(plan.hyperpath);
		PassengerStrategy reference;
		std::sort(set.begin(), set.end(), [](const auto &a, const auto &b) { return std::tie(a.cost, a.occurrence) < std::tie(b.cost, b.occurrence); });
		for (const auto &candidate : set) {
			CAPTURE(candidate.service.base(), candidate.cost, candidate.frequency, reference.Cost());
			REQUIRE(reference.Add(candidate.cost, candidate.frequency));
			auto board = table.Board(A, D, StateTicks{42}, candidate.service, candidate.order);
			REQUIRE(board.ride); CHECK(board.ride->alighting == D); CHECK(board.cost == candidate.cost);
		}
		CHECK(plan.cost == reference.Cost());
		CHECK_FALSE(table.Board(A, D, StateTicks{}, OrderListID{9}, 0).ride);
		CHECK(table.Candidates(A, D, OrderListID{0}).size() == 8);
	}
}

TEST_CASE("Hyperpath excludes only the arriving OrderList and handles masks", "[passenger][hyperpath]")
{
	std::array services{Service(0, {A, B, D}), Service(1, {A, B, D}), Service(2, {A, C, D}, 100)};
	SECTION("Identical patterns remain distinct real opportunities") {}
	SECTION("NoLoad") { services[2].timetable.stops[0].board = 0; }
	SECTION("NoUnload") { services[2].timetable.stops[2].alight = 0; }
	SECTION("Forced unloading") { services[2].timetable.stops[1].forced = 1; }
	SECTION("Zero dwell") { for (auto &s : services) for (auto &stop : s.timetable.stops) stop.departure = stop.arrival; }
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	PassengerPrecomputedTable table; table.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, true);
	while (!table.Finished()) table.Step(3);
	REQUIRE_FALSE(table.MemoryRejected());
	auto set = table.Inspect(A, D, OrderListID{0});
	CHECK(std::none_of(set.begin(), set.end(), [](const auto &c) { return c.service == OrderListID{0}; }));
	CHECK(std::any_of(set.begin(), set.end(), [](const auto &c) { return c.service == OrderListID{1}; }));
}

TEST_CASE("Hyperpath orders Board events by cost including risk", "[passenger][hyperpath]")
{
	std::array services{Service(0, {A, D}, 100), Service(1, {A, D}, 150)};
	services[0].vehicles[0].capacity[PAX] = 1;
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	PassengerPrecomputedTable table; table.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, true);
	while (!table.Finished()) table.Step(1);
	auto set = table.Inspect(A, D);
	REQUIRE(set.size() == 1); CHECK(set[0].service == OrderListID{1});
	CHECK(set[0].cost == 155); CHECK(set[0].wait == 160);
	CHECK(table.Plan(A, D, StateTicks{}).cost == 315);
}

TEST_CASE("Hyperpath counts a frozen physical vehicle in only its newest captured service", "[passenger][hyperpath]")
{
	std::array services{Service(0, {A, D}, 100), Service(1, {A, D}, 100)};
	services[1].vehicles[0].id = services[0].vehicles[0].id;
	std::vector<const PassengerPeriodicService *> records{&services[0], &services[1]};
	PassengerPrecomputedTable table; table.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, true);
	while (!table.Finished()) table.Step(1);
	auto set = table.Inspect(A, D); REQUIRE(set.size() == 1);
	CHECK(set[0].service == services[1].id); // The newer frozen generation owns the single real opportunity.
	CHECK(table.Plan(A, D, StateTicks{}).cost == set[0].cost + set[0].wait);
}

TEST_CASE("Hyperpath matches single-service certified and estimated costs", "[passenger][hyperpath]")
{
	std::array services{Service(0, {A, B, D})};
	SECTION("Certified") {}
	SECTION("Unknown phase") { services[0].vehicles[0].phase_known = false; }
	SECTION("Estimated") { services[0].timetable.estimated_period = services[0].timetable.period; services[0].timetable.period = 0; }
	std::vector<const PassengerPeriodicService *> records{&services[0]};
	auto old = Build(services);
	PassengerPrecomputedTable table; table.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, true);
	while (!table.Finished()) table.Step(7);
	CHECK(table.Plan(A, D, StateTicks{}).cost == old.Plan(A, D, StateTicks{}).cost);
	auto set = table.Inspect(A, D); REQUIRE(set.size() == 1);
	CHECK(table.Plan(A, D, StateTicks{}).cost == set[0].cost + set[0].wait);
}

TEST_CASE("Hyperpath local replanning admits a previously rejected service and preserves reachability", "[passenger][hyperpath][replan]")
{
	std::vector<PassengerPeriodicService> services{Service(0, {A, D}, 100)};
	bool alternative = true;
	SECTION("Previously unattractive alternative") { services.push_back(Service(1, {A, D}, 220)); }
	SECTION("No alternative restores the failed service") { alternative = false; }
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	PassengerPrecomputedTable table; table.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, true);
	while (!table.Finished()) table.Step(3);
	REQUIRE(table.Candidates(A, D).size() == 1);
	CHECK_FALSE(table.Board(A, D, StateTicks{}, OrderListID{1}, 0).ride);
	OrderListID chosen{static_cast<uint16_t>(alternative ? 1 : 0)};
	for (size_t i = 0; i < 100; ++i) {
		auto board = table.Board(A, D, StateTicks{}, chosen, 0, OrderListID::Invalid(), OrderListID{0}); REQUIRE(board.ride);
		CHECK(board.ride->service == chosen); CHECK(board.ride->alighting == D);
	}
	CHECK(table.GetDiagnostic().local_replans == 1);
	if (alternative) CHECK_FALSE(table.Board(A, D, StateTicks{}, OrderListID{0}, 0, OrderListID::Invalid(), OrderListID{0}).ride);
}

TEST_CASE("Precomputed trees preserve through rides and physical occurrences", "[passenger][precomputed]")
{
	std::array services{Service(0, {A, B, C, D})};
	auto table = Build(services); auto plan = table.Plan(A, D, StateTicks{1000});
	REQUIRE(plan.ride); CHECK(plan.ride->boarding_order == 0); CHECK(plan.ride->alighting_order == 3);
	CHECK(plan.transfers == 0); CHECK(plan.riding == 320); CHECK(plan.waiting == 220);
	CHECK(plan.ride->not_before == StateTicks{1000}); CHECK(plan.ride->departure == StateTicks{}); CHECK(plan.ride->arrival == StateTicks{});
	services[0] = Service(0, {A, B, A, D}); table = Build(services);
	plan = table.Plan(A, D, StateTicks{1000}); REQUIRE(plan.ride); CHECK(plan.ride->boarding_order == 2); CHECK(plan.ride->alighting_order == 3);
}

TEST_CASE("Precomputed contraction preserves pass-through orders and alight-only dwell", "[passenger][precomputed][contraction]")
{
	std::array services{Service(0, {A, B, C, D}), Service(1, {A, B, C}, 90)};
	for (auto &s : services) { s.timetable.stops[1].board = s.timetable.stops[1].alight = 0; }
	SECTION("Pass-through chains") {}
	SECTION("Alight-only arrival retains its dwell boundary") { services[0].timetable.stops[2].board = 0; }
	SECTION("Forced stop cannot be contracted") { services[0].timetable.stops[1].forced = 1; }
	SECTION("Discontinued ride cannot be contracted") { services[0].timetable.stops[1].continues = false; }
	SECTION("Repeated station remains a distinct occurrence") { services[0].timetable.stops[2].station = A; }
	SECTION("Zero dwell and transfer") { for (auto &s : services) for (auto &stop : s.timetable.stops) stop.departure = stop.arrival; }
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	PassengerPrecomputedTable legacy, optimized;
	legacy.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, false);
	optimized.Begin(records, PAX, 0);
	while (!legacy.Finished()) legacy.Step(7);
	while (!optimized.Finished()) optimized.Step(3);
	CHECK(optimized.GetDiagnostic().states <= legacy.GetDiagnostic().states);
	CHECK(optimized.GetDiagnostic().stored_states == legacy.GetDiagnostic().stored_states);
	for (auto from : {A, B, C, D}) for (auto to : {A, B, C, D}) for (auto excluded : {OrderListID::Invalid(), OrderListID{0}, OrderListID{1}}) {
		for (uint32_t sample : {0U, UINT32_MAX / 2, UINT32_MAX}) {
			auto a = legacy.Plan(from, to, StateTicks{42}, excluded, sample), b = optimized.Plan(from, to, StateTicks{42}, excluded, sample);
			CHECK(a.ride == b.ride); CHECK(a.state == b.state);
			CHECK(std::tie(a.cost, a.riding, a.waiting, a.distance, a.transfers) == std::tie(b.cost, b.riding, b.waiting, b.distance, b.transfers));
		}
	}
}

TEST_CASE("Passenger radix queue preserves wide keys zero-cost ties and reinsertion", "[passenger][precomputed][heap]")
{
	using Item = std::tuple<int64_t, uint32_t, uint32_t>;
	for (bool optimized : {false, true}) {
		PassengerDestinationQueue<Item> queue; queue.SetOptimized(optimized); queue.SetCapacityLimit(1);
		queue.emplace(0, 2, 8); queue.emplace(0, 1, 9); queue.emplace(0, 1, 7);
		CHECK(queue.top() == Item(0, 1, 7)); queue.pop();
		queue.emplace(0, 1, 2); // A smaller state appears after a pop at the same cost.
		CHECK(queue.top() == Item(0, 1, 2)); queue.pop();
		queue.emplace(0, 0, 7); // A previously popped state may be reinserted.
		CHECK(queue.top() == Item(0, 0, 7)); queue.pop();
		CHECK(queue.top() == Item(0, 1, 9)); queue.pop();
		CHECK(queue.top() == Item(0, 2, 8)); queue.pop();
		for (int64_t cost : {int64_t{1} << 33, int64_t{1} << 48, INT64_MAX - 1}) queue.emplace(cost, UINT32_MAX, 1);
		for (int64_t cost : {int64_t{1} << 33, int64_t{1} << 48, INT64_MAX - 1}) { CHECK(queue.top() == Item(cost, UINT32_MAX, 1)); queue.pop(); }
		REQUIRE(queue.empty()); queue.Reset(); queue.emplace(0, 0, 0); CHECK(queue.top() == Item(0, 0, 0)); queue.pop();
	}
}

TEST_CASE("Alight-only zero dwell preserves the original equal-cost carrier choice", "[passenger][precomputed][contraction][determinism]")
{
	std::array services{Service(0, {A, B, C, StationID{4}}, 101), Service(1, {B, D}, 150), Service(2, {C, D}, 100)};
	for (size_t p = 0; p < services.size(); ++p) {
		int64_t travel = std::array<int64_t, 3>{101, 150, 100}[p];
		for (size_t i = 0; i < services[p].timetable.stops.size(); ++i) {
			auto &stop = services[p].timetable.stops[i]; stop.arrival = stop.departure = i * travel;
		}
		services[p].timetable.period = services[p].timetable.stops.size() * travel;
	}
	services[0].timetable.stops[1].board = 0;
	auto table = Build(services, 7, 0);
	auto plan = table.Plan(A, D, StateTicks{}); REQUIRE(plan.ride);
	/* Both continuations cost 615 with two legs. The original waiting state
	 * at B wins before B's departure. Moving the dwell onto a shortcut would
	 * discover the C continuation early and silently change the first alight. */
	CHECK(plan.cost == 615); CHECK(plan.riding == 251); CHECK(plan.transfers == 1);
	CHECK(plan.ride->alighting == B); CHECK(plan.ride->alighting_order == 1);
}

TEST_CASE("Cooperative graph preparation preserves exact trees and shared memory admission", "[passenger][precomputed][startup][network]")
{
	std::array services{Service(0, {A, B, C, D}), Service(1, {A, B, C}), Service(2, {B, C, D}, 80)};
	for (uint32_t i = 3; i < 600; ++i) { auto vehicle = services[0].vehicles.front(); vehicle.id = VehicleID{i}; services[0].vehicles.push_back(vehicle); }
	auto control = Build(services);
	std::vector<const PassengerPeriodicService *> records; for (const auto &service : services) records.push_back(&service);
	for (size_t quantum : {size_t{1}, size_t{7}, size_t{1024}}) {
		PassengerPrecomputedTable peer;
		peer.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_BYTES, true, true);
		size_t slices = 0;
		while (peer.IsPreparingGraph()) {
			CHECK_FALSE(peer.Finished()); CHECK(peer.GetDiagnostic().destinations == 0);
			CHECK(peer.Plan(A, D, StateTicks{}).state == PassengerPrecomputedPlan::State::Building);
			peer.PrepareGraph(quantum); REQUIRE(++slices < 100000);
		}
		REQUIRE(slices > 1);
		while (!peer.Finished()) peer.Step(19);
		CHECK(peer.GetDiagnostic().operations == control.GetDiagnostic().operations);
		CHECK(peer.GetDiagnostic().envelope == control.GetDiagnostic().envelope);
		for (auto from : {A, B, C, D}) for (auto to : {A, B, C, D}) {
			auto actual = peer.Plan(from, to, StateTicks{42}); auto expected = control.Plan(from, to, StateTicks{42});
			CHECK(actual.state == expected.state); CHECK(actual.ride == expected.ride); CHECK(actual.cost == expected.cost);
		}
	}
	auto remaining = std::make_shared<uint64_t>(control.GetDiagnostic().envelope);
	PassengerPrecomputedTable first, second;
	first.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_BYTES, true, true, remaining);
	second.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_BYTES, true, true, remaining);
	while (!first.PrepareGraph(7)) {}
	while (!second.PrepareGraph(3)) {}
	CHECK(*remaining == 0); CHECK_FALSE(first.MemoryRejected()); CHECK(second.MemoryRejected());
}

TEST_CASE("Precomputed masks and forced alighting cannot be bypassed", "[passenger][precomputed]")
{
	std::array services{Service(0, {A, B, D}), Service(1, {B, D}, 150)};
	services[0].timetable.stops[1].forced = 1; services[0].timetable.stops[1].board = 0;
	auto table = Build(services); auto plan = table.Plan(A, D, StateTicks{});
	REQUIRE(plan.ride); CHECK(plan.ride->alighting == B); CHECK(plan.transfers == 1);
	auto next = table.Plan(B, D, StateTicks{}, OrderListID{0}); REQUIRE(next.ride); CHECK(next.ride->service == OrderListID{1});
	services[0].timetable.stops[0].board = 0; table = Build(services);
	CHECK_FALSE(table.Plan(A, D, StateTicks{}).ride);
	services[0].timetable.stops[0].board = 1;
	services[0].timetable.stops[2].alight = services[1].timetable.stops[1].alight = 0;
	table = Build(services); CHECK_FALSE(table.Plan(A, D, StateTicks{}).ride);
}

TEST_CASE("Precomputed frozen costs refresh only compatible live service bindings", "[passenger][precomputed][generation]")
{
	std::array services{Service(0, {A, B, D})};
	auto table = Build(services);
	auto before = table.Plan(A, D, StateTicks{}); REQUIRE(before.ride);
	services[0].generation += 10;
	services[0].timetable.period += 100;
	services[0].timetable.stops.back().arrival += 100;
	services[0].timetable.stops.back().departure += 100;
	REQUIRE(table.RefreshService(services[0]));
	auto refreshed = table.Plan(A, D, StateTicks{}); REQUIRE(refreshed.ride);
	CHECK(refreshed.cost == before.cost);
	CHECK(refreshed.ride->generation == services[0].generation);
	SECTION("NoLoad") { services[0].timetable.stops.front().board = 0; }
	SECTION("NoUnload") { services[0].timetable.stops.back().alight = 0; }
	SECTION("Forced unload") { services[0].timetable.stops[1].forced = 1; }
	SECTION("Occurrence station") { services[0].timetable.stops[1].station = C; }
	SECTION("Recycled list") { ++services[0].incarnation; }
	++services[0].generation;
	CHECK_FALSE(table.RefreshService(services[0]));
	CHECK(table.Plan(A, D, StateTicks{}).ride->generation == refreshed.ride->generation);
}

TEST_CASE("Precomputed incoming service states rescue routes beyond fixed K", "[passenger][precomputed]")
{
	std::vector<PassengerPeriodicService> services;
	services.push_back(Service(0, {A, B, C, D}, 10));
	for (int i = 1; i <= 8; ++i) services.push_back(Service(i, {A, B, C, D}, 100 + i));
	auto table = Build(services);
	auto plan = table.Plan(A, D, StateTicks{}, OrderListID{0}); REQUIRE(plan.ride); CHECK(plan.ride->service != OrderListID{0});
	StationID station = A; OrderListID previous{0}; size_t legs = 0;
	while (station != D && legs < 8) {
		auto next = table.Plan(station, D, StateTicks{}, previous); REQUIRE(next.ride);
		CHECK(next.ride->service != previous); CHECK(next.ride->alighting != station);
		station = next.ride->alighting; previous = next.ride->service; ++legs;
	}
	CHECK(station == D); CHECK(legs <= 2);
}

TEST_CASE("Estimated connector replaces a large certified detour in mixed and untimed networks", "[passenger][precomputed][estimated][quality]")
{
	std::array services{Service(0, {A, B}, 40), Service(1, {B, D}, 40), Service(2, {A, C}, 1000), Service(3, {C, D}, 1000)};
	auto untimed = [](auto &s) { s.timetable.estimated_period = s.timetable.period; s.timetable.period = 0; for (auto &v : s.vehicles) v.phase_known = false; };
	SECTION("All certified") {}
	SECTION("Mixed connector has only unconfirmed measurements") { untimed(services[1]); services[1].timetable.stops[1].travel_quality = 1; }
	SECTION("Every timetable is incomplete") { for (auto &s : services) untimed(s); }
	SECTION("Cross mode transfer") { untimed(services[1]); services[1].vehicles[0].type = VehicleType::Road; }
	auto table = Build(services, 7); auto plan = table.Plan(A, D, StateTicks{}); REQUIRE(plan.ride);
	CHECK(plan.ride->service == OrderListID{0}); CHECK(plan.ride->alighting == B); CHECK(plan.transfers == 1); CHECK(plan.riding == 80);
	auto connector = table.Plan(B, D, StateTicks{}, OrderListID{0}); REQUIRE(connector.ride);
	CHECK(connector.ride->service == OrderListID{1}); CHECK(connector.ride->alighting_order == 1);
	CHECK(plan.cost < 500); CHECK(table.GetDiagnostic().estimated_patterns + table.GetDiagnostic().certified_patterns == table.GetDiagnostic().patterns);
}

TEST_CASE("Route dispersion cannot trade a small wait difference for a large riding detour", "[passenger][precomputed][estimated][quality]")
{
	std::array services{Service(0, {A, D}, 100), Service(1, {A, B}, 125), Service(2, {B, D}, 125)};
	services[0].timetable.period = 400;
	for (size_t service : {size_t{1}, size_t{2}}) {
		services[service].timetable.period = 300;
		for (uint32_t i = 1; i < 15; ++i) {
			auto v = services[service].vehicles.front(); v.id = VehicleID{static_cast<uint32_t>(service * 100 + i)};
			v.base = StateTicks{i * 20}; services[service].vehicles.push_back(v);
		}
	}
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	PassengerPrecomputedTable legacy; legacy.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, false);
	while (!legacy.Finished()) legacy.Step(10000);
	auto before = legacy.Plan(A, D, StateTicks{}, OrderListID::Invalid(), 0); REQUIRE(before.ride);
	CHECK(before.ride->service != OrderListID{0}); CHECK(before.riding == 250);
	auto table = Build(services);
	for (uint32_t sample : {0U, 123456789U, UINT32_MAX}) {
		auto plan = table.Plan(A, D, StateTicks{}, OrderListID::Invalid(), sample); REQUIRE(plan.ride);
		CHECK(plan.ride->service == OrderListID{0}); CHECK(plan.riding == 100); CHECK(plan.transfers == 0);
	}
}

TEST_CASE("Unknown phases receive bounded fleet credit without absolute departures", "[passenger][precomputed][estimated][frequency]")
{
	std::array services{Service(0, {A, D}, 100)};
	auto &s = services[0]; s.timetable.estimated_period = s.timetable.period; s.timetable.period = 0; s.vehicles[0].phase_known = false;
	auto baseline = Build(services).Plan(A, D, StateTicks{}); REQUIRE(baseline.ride); CHECK(baseline.ride->estimated_wait == 165);
	for (uint32_t i = 1; i < 16; ++i) { auto v = s.vehicles[0]; v.id = VehicleID{i}; s.vehicles.push_back(v); }
	auto fleet = Build(services); auto plan = fleet.Plan(A, D, StateTicks{999}); REQUIRE(plan.ride);
	CHECK(plan.ride->estimated_wait == 42); CHECK(plan.estimated_service);
	CHECK(plan.ride->departure == StateTicks{}); CHECK(plan.ride->arrival == StateTicks{});
	CHECK(plan.ride->waiting_limit == baseline.ride->waiting_limit); // Deadlines are not shortened to conceal a missed service.
	CHECK(fleet.GetDiagnostic().states == Build(services).GetDiagnostic().states);
}

TEST_CASE("Estimated masks and unsafe segment boundaries cannot invent a through ride", "[passenger][precomputed][estimated][orders]")
{
	std::array services{Service(0, {A, B, C, D}), Service(1, {B, C, D}, 150)};
	for (auto &s : services) { s.timetable.estimated_period = s.timetable.period; s.timetable.period = 0; s.vehicles[0].phase_known = false; }
	SECTION("NoLoad") { services[0].timetable.stops[0].board = 0; CHECK_FALSE(Build(services).Plan(A, D, StateTicks{}).ride); }
	SECTION("NoUnload") { services[0].timetable.stops[3].alight = 0; services[1].timetable.stops[2].alight = 0; CHECK_FALSE(Build(services).Plan(A, D, StateTicks{}).ride); }
	SECTION("Forced transfer") {
		services[0].timetable.stops[1].forced = 1; services[0].timetable.stops[1].board = 0;
		auto plan = Build(services).Plan(A, D, StateTicks{}); REQUIRE(plan.ride); CHECK(plan.ride->alighting == B); CHECK(plan.transfers == 1);
	}
	SECTION("Control boundary inside a shared prefix") {
		services[0].timetable.stops[1].continues = services[1].timetable.stops[0].continues = false;
		auto table = Build(services); CHECK_FALSE(table.Plan(A, D, StateTicks{}).ride); REQUIRE(table.Plan(C, D, StateTicks{}).ride);
	}
}

TEST_CASE("Estimated common sections retain real short turn express and occurrence contracts", "[passenger][precomputed][estimated][sections]")
{
	std::array services{Service(0, {A, B, C, D}), Service(1, {A, B, C}), Service(2, {A, C, D}, 60)};
	for (auto &s : services) { s.timetable.estimated_period = s.timetable.period; s.timetable.period = 0; s.vehicles[0].phase_known = false; }
	auto table = Build(services); REQUIRE(table.GetDiagnostic().shared_sections > 0);
	for (uint32_t sample : {0U, 123456789U, UINT32_MAX}) {
		auto through = table.Plan(A, D, StateTicks{}, OrderListID::Invalid(), sample); REQUIRE(through.ride);
		if (through.transfers == 0) CHECK(through.ride->service != OrderListID{1});
		auto local = table.Plan(A, B, StateTicks{}, OrderListID::Invalid(), sample); REQUIRE(local.ride);
		CHECK(local.ride->service != OrderListID{2});
		auto rebound = *local.ride; CHECK_FALSE(table.Rebind(rebound, OrderListID{2}, 0));
	}
	services[0] = Service(0, {A, B, A, D});
	services[0].timetable.estimated_period = services[0].timetable.period; services[0].timetable.period = 0; services[0].vehicles[0].phase_known = false;
	std::array repeated{services[0]}; auto repeat = Build(repeated).Plan(A, D, StateTicks{}); REQUIRE(repeat.ride);
	CHECK(repeat.ride->boarding_order == 2); CHECK(repeat.ride->alighting_order == 3);
}

TEST_CASE("Precomputed identical fleets aggregate without synthetic headways", "[passenger][precomputed]")
{
	std::array services{Service(0, {A, D}), Service(1, {A, D})};
	auto table = Build(services); CHECK(table.GetDiagnostic().patterns == 1);
	const auto baseline = table.Plan(A, D, StateTicks{});
	for (int i = 0; i < 10000; ++i) {
		auto vehicle = services[0].vehicles.front(); vehicle.id = VehicleID{static_cast<uint32_t>(i + 2)};
		services[0].vehicles.push_back(vehicle);
	}
	auto fleet = Build(services); CHECK(fleet.GetDiagnostic().states == table.GetDiagnostic().states);
	CHECK(fleet.GetDiagnostic().edges == table.GetDiagnostic().edges); CHECK(fleet.Plan(A, D, StateTicks{}).waiting == baseline.waiting);
	auto excluded = fleet.Plan(A, D, StateTicks{}, OrderListID{1}); REQUIRE(excluded.ride); CHECK(excluded.ride->service == OrderListID{0});
	services[0].vehicles[0].phase_known = false; fleet = Build(services);
	/* Thousands of coincident known phases do not earn unknown-fleet credit.
	 * The one remaining certified stream still supplies its half-circuit wait. */
	CHECK(fleet.Plan(A, D, StateTicks{}).waiting == services[0].timetable.period / 2);
}

TEST_CASE("Common sections aggregate short turns without inventing through trains", "[passenger][precomputed][sections]")
{
	constexpr StationID E{4};
	std::array services{Service(0, {A, B, C, D, E}), Service(1, {A, B, C}), Service(2, {A, B, C, D, E})};
	for (auto &s : services) s.timetable.period = 600;
	services[1].vehicles[0].base = StateTicks{400}; services[2].vehicles[0].base = StateTicks{200};
	auto table = Build(services);
	REQUIRE(table.GetDiagnostic().shared_sections > 0); CHECK(table.GetDiagnostic().patterns == 2);
	for (uint32_t sample : {0U, 1U, 1000000000U, 2000000001U, UINT32_MAX}) {
		auto common = table.Plan(A, C, StateTicks{}, OrderListID::Invalid(), sample);
		REQUIRE(common.ride); CHECK(common.ride->estimated_wait == 100); CHECK(common.waiting == 100);
		CHECK(common.riding == 210); CHECK(common.transfers == 0);
		auto through = table.Plan(A, E, StateTicks{}, OrderListID::Invalid(), sample);
		REQUIRE(through.ride);
		if (through.ride->service == OrderListID{1}) {
			CHECK(through.transfers > 0); CHECK(through.ride->alighting != E);
			CHECK(through.ride->alighting_order < 3); CHECK(through.ride->estimated_wait == 100);
		} else if (through.transfers == 0) {
			CHECK(through.ride->alighting == E); CHECK(through.ride->alighting_order == 4);
			CHECK(through.ride->estimated_wait == 200);
		}
		auto outer = table.Plan(C, E, StateTicks{}, OrderListID::Invalid(), sample);
		REQUIRE(outer.ride); CHECK(outer.ride->service != OrderListID{1}); CHECK(outer.ride->estimated_wait == 200);
	}
	/* Every representative is an anchor for the same whole-section intent. */
	std::set<OrderListID> selected;
	for (uint32_t sample = 0; sample < 32; ++sample) {
		auto plan = table.Plan(A, C, StateTicks{}, OrderListID::Invalid(), sample * 134217727U);
		REQUIRE(plan.ride); selected.insert(plan.ride->service);
		CHECK(table.Rebind(*plan.ride, plan.ride->service == OrderListID{1} ? OrderListID{0} : OrderListID{1}));
	}
	CHECK(selected.contains(OrderListID{1})); CHECK(selected.size() >= 2);
	auto through = table.Plan(A, E, StateTicks{}, OrderListID{1}); REQUIRE(through.ride);
	CHECK(through.transfers == 0); CHECK_FALSE(table.Rebind(*through.ride, OrderListID{1}));
}

TEST_CASE("Common section events respect periods physical identity and dispatch anchors", "[passenger][precomputed][sections]")
{
	std::array services{Service(0, {A, B, C, D}), Service(1, {A, B, C})};
	services[0].timetable.period = 600; services[1].timetable.period = 300;
	services[1].vehicles[0].base = StateTicks{150};
	SECTION("Different periods") {
		auto plan = Build(services).Plan(A, C, StateTicks{}); REQUIRE(plan.ride);
		CHECK(plan.ride->estimated_wait == 150); CHECK(plan.ride->estimated_wait < 300);
	}
	SECTION("One physical vehicle cannot belong to two frequency sources") {
		services[1].vehicles[0].id = services[0].vehicles[0].id;
		auto table = Build(services);
		/* The owner with the lowest service ID supplies this physical event.
		 * Exclude the short service to inspect the long service's estimate. */
		auto plan = table.Plan(A, C, StateTicks{}, OrderListID{1}); REQUIRE(plan.ride);
		CHECK(plan.ride->service == OrderListID{0}); CHECK(plan.ride->estimated_wait == 300);
	}
	SECTION("Coincident departures count once including across periods") {
		services[1].vehicles[0].base = StateTicks{};
		auto once = Build(services).Plan(A, C, StateTicks{}); REQUIRE(once.ride);
		for (int i = 2; i < 100; ++i) {
			auto v = services[1].vehicles[0]; v.id = VehicleID{static_cast<uint32_t>(i)}; services[1].vehicles.push_back(v);
		}
		auto many = Build(services).Plan(A, C, StateTicks{}); REQUIRE(many.ride);
		CHECK(many.waiting == once.waiting); CHECK(many.cost == once.cost);
	}
	SECTION("Certified slots use the boarding offset from the dispatch stop") {
		for (auto &s : services) { s.dispatch_period = 600; s.dispatch_stop = 1; s.dispatch_slots = {0}; }
		auto table = Build(services);
		auto plan = table.Plan(A, C, StateTicks{}); REQUIRE(plan.ride); CHECK(plan.ride->estimated_wait == 150);
		plan = table.Plan(B, C, StateTicks{}); REQUIRE(plan.ride); CHECK(plan.ride->estimated_wait == 300);
	}
	SECTION("Unknown phases do not create evenly spaced trains") {
		for (auto &s : services) s.vehicles[0].phase_known = false;
		auto plan = Build(services).Plan(A, C, StateTicks{}); REQUIRE(plan.ride); CHECK(plan.ride->estimated_wait == 225);
		CHECK(plan.ride->departure == StateTicks{}); CHECK(plan.ride->arrival == StateTicks{});
	}
}

TEST_CASE("Common sections preserve physical stop and exchange differences", "[passenger][precomputed][sections]")
{
	std::array services{Service(0, {A, B, C, D}), Service(1, {A, B, C})};
	for (auto &s : services) s.timetable.period = 600;
	services[1].vehicles[0].base = StateTicks{300};
	SECTION("NoLoad") { services[1].timetable.stops[0].board = 0; }
	SECTION("NoUnload") { services[1].timetable.stops[2].alight = 0; }
	SECTION("Forced unload") { services[1].timetable.stops[1].forced = 1; }
	SECTION("Operational pass through") { services[1].timetable.stops[1].board = services[1].timetable.stops[1].alight = 0; }
	SECTION("Skipped stop") { services[1] = Service(1, {A, C}); services[1].timetable.period = 600; services[1].vehicles[0].base = StateTicks{300}; }
	SECTION("Opposite direction") { services[1] = Service(1, {C, B, A}); services[1].timetable.period = 600; services[1].vehicles[0].base = StateTicks{300}; }
	auto table = Build(services);
	auto plan = table.Plan(A, C, StateTicks{}, OrderListID{1}); REQUIRE(plan.ride);
	CHECK(plan.ride->service == OrderListID{0}); CHECK(plan.ride->estimated_wait == 300);
}

TEST_CASE("Common sections match across order numbering and circuit boundaries", "[passenger][precomputed][sections][determinism]")
{
	std::array services{Service(0, {B, C, D, A}), Service(1, {A, B, C})};
	for (auto &s : services) s.timetable.period = 600;
	/* Long service boards A at 340, short service at 40: opposite phases. */
	services[1].vehicles[0].base = StateTicks{30};
	for (auto &stop : services[1].timetable.stops) stop.order += 20;
	auto fine = Build(services, 1), coarse = Build(services);
	CHECK(fine.GetDiagnostic().operations == coarse.GetDiagnostic().operations);
	for (uint32_t sample : {0U, 1000000000U, UINT32_MAX}) {
		auto a = fine.Plan(A, C, StateTicks{}, OrderListID::Invalid(), sample);
		auto b = coarse.Plan(A, C, StateTicks{}, OrderListID::Invalid(), sample);
		REQUIRE(a.ride); CHECK(a.ride == b.ride); CHECK(a.cost == b.cost); CHECK(a.ride->estimated_wait == 150);
		if (a.ride->service == OrderListID{0}) { CHECK(a.ride->boarding_order == 3); CHECK(a.ride->alighting_order == 1); }
		else { CHECK(a.ride->boarding_order == 20); CHECK(a.ride->alighting_order == 22); }
		auto long_ride = *a.ride, short_ride = *a.ride;
		REQUIRE(fine.Rebind(long_ride, OrderListID{0}, 3)); REQUIRE(fine.Rebind(short_ride, OrderListID{1}, 20));
		CHECK(long_ride.boarding_order == 3); CHECK(long_ride.alighting_order == 1);
		CHECK(short_ride.boarding_order == 20); CHECK(short_ride.alighting_order == 22);
		CHECK(fine.SectionBoardingKey(long_ride) == fine.SectionBoardingKey(short_ride));
	}
}

TEST_CASE("Common section bindings match the complete frequency pool", "[passenger][precomputed][sections][boarding]")
{
	std::array services{Service(0, {A, B, C, D}), Service(1, {A, B, C}), Service(2, {A, B, C, D})};
	services[0].timetable.period = services[2].timetable.period = 600; services[1].timetable.period = 300;
	services[1].vehicles[0].base = StateTicks{150}; services[2].vehicles[0].base = StateTicks{300};
	auto table = Build(services); auto plan = table.Plan(A, C, StateTicks{}); REQUIRE(plan.ride);
	const auto key = table.SectionBoardingKey(*plan.ride); REQUIRE(key);
	for (const auto &service : services) {
		auto ride = *plan.ride; REQUIRE(table.Rebind(ride, service.id, 0));
		CHECK(ride.service == service.id); CHECK(ride.signature == service.incarnation); CHECK(ride.generation == service.generation);
		CHECK(ride.boarding_order == 0); CHECK(ride.alighting_order == 2); CHECK(ride.alighting == C);
		CHECK(table.SectionBoardingKey(ride) == key);
		auto keys = table.SectionBoardingKeys(service.id, 0); CHECK(std::find(keys.begin(), keys.end(), *key) != keys.end());
		CHECK_FALSE(table.Rebind(ride, service.id, 1));
	}
	auto bound = *plan.ride; bound.kind = PassengerRideKind::EstimatedBoundService;
	CHECK_FALSE(table.SectionBoardingKey(bound)); CHECK_FALSE(table.Rebind(bound, OrderListID{1}, 0));
	auto stale = *plan.ride; ++stale.generation;
	CHECK_FALSE(table.SectionBoardingKey(stale)); CHECK_FALSE(table.Rebind(stale, OrderListID{1}, 0));
}

TEST_CASE("Common section bindings distinguish repeated boarding orders on the same service", "[passenger][precomputed][sections][boarding]")
{
	std::array services{Service(0, {A, B, C, A, B, C, D}), Service(1, {A, B, C})};
	auto table = Build(services); auto plan = table.Plan(A, C, StateTicks{}); REQUIRE(plan.ride);
	const auto key = table.SectionBoardingKey(*plan.ride); REQUIRE(key);
	for (VehicleOrderID order : {VehicleOrderID{0}, VehicleOrderID{3}}) {
		auto ride = *plan.ride; REQUIRE(table.Rebind(ride, OrderListID{0}, order));
		CHECK(ride.boarding_order == order); CHECK(ride.alighting_order == order + 2); CHECK(ride.alighting == C);
		CHECK(table.SectionBoardingKey(ride) == key);
		auto keys = table.SectionBoardingKeys(OrderListID{0}, order); CHECK(std::find(keys.begin(), keys.end(), *key) != keys.end());
	}
}

TEST_CASE("Common section admission and large period ratios stay deterministic", "[passenger][precomputed][sections][memory]")
{
	std::array services{Service(0, {A, B, C, D}), Service(1, {A, B, C})};
	services[0].timetable.period = int64_t{1} << 40; services[1].timetable.period = 300;
	services[1].vehicles[0].base = StateTicks{150};
	auto table = Build(services); auto plan = table.Plan(A, C, StateTicks{}, OrderListID{1});
	REQUIRE(plan.ride); CHECK(plan.ride->estimated_wait == 150);
	const auto work = table.GetDiagnostic().operations;
	for (int i = 0; i < 10000; ++i) REQUIRE(table.Plan(A, C, StateTicks{}, OrderListID::Invalid(), i * 654323U).ride);
	CHECK(table.GetDiagnostic().operations == work);
	std::array<const PassengerPeriodicService *, 2> records{&services[1], &services[0]};
	PassengerPrecomputedTable reversed; reversed.Begin(records, PAX, 20);
	while (!reversed.Finished()) reversed.Step(19);
	CHECK(reversed.GetDiagnostic().operations == work);
	CHECK(reversed.Plan(A, C, StateTicks{}).ride == table.Plan(A, C, StateTicks{}).ride);
	PassengerPrecomputedTable limited;
	limited.Begin(records, PAX, 20, table.GetDiagnostic().envelope - 1);
	CHECK(limited.MemoryRejected()); CHECK(limited.GetDiagnostic().destinations == 0);
}

TEST_CASE("Precomputed publication is independent of build chunk and query history", "[passenger][precomputed][determinism]")
{
	std::array services{Service(0, {A, B}), Service(1, {B, C}), Service(2, {C, D}), Service(3, {A, D}, 500)};
	auto fine = Build(services, 1), coarse = Build(services, 65536);
	CHECK(fine.GetDiagnostic().operations == coarse.GetDiagnostic().operations);
	for (StationID from : {A, B, C, D}) for (StationID to : {A, B, C, D}) for (uint32_t sample : {0U, 123456U, UINT32_MAX}) {
		auto a = fine.Plan(from, to, StateTicks{1000}, OrderListID::Invalid(), sample);
		auto b = coarse.Plan(from, to, StateTicks{1000}, OrderListID::Invalid(), sample);
		CHECK(a.state == b.state); CHECK(a.ride == b.ride); CHECK(a.cost == b.cost); CHECK(a.riding == b.riding); CHECK(a.waiting == b.waiting);
	}
	std::vector<const PassengerPeriodicService *> records{&services[0], &services[1], &services[2], &services[3]};
	PassengerPrecomputedTable partial; partial.Begin(records, PAX, 20); partial.Step(1);
	CHECK(partial.Plan(A, D, StateTicks{}).state == PassengerPrecomputedPlan::State::Building);
	PassengerPrecomputedTable limited; limited.Begin(records, PAX, 20, 1);
	CHECK(limited.Plan(A, D, StateTicks{}).state == PassengerPrecomputedPlan::State::Memory);
}

TEST_CASE("Precomputed published rows retain only query states", "[passenger][precomputed][memory]")
{
	std::array services{Service(0, {A, D})};
	auto &service = services.front(); service.timetable.stops.clear();
	for (uint16_t i = 0; i < 1000; ++i) {
		service.timetable.stops.push_back({i % 2 == 0 ? A : D, i, int64_t(i) * 110, int64_t(i) * 110 + 10, 1, 1, 0});
	}
	service.timetable.period = 110000;
	auto table = Build(services);
	CHECK(table.GetDiagnostic().stored_states == 6);
	CHECK(table.GetDiagnostic().states == 2004);
	REQUIRE(table.Plan(A, D, StateTicks{}).ride);
	CHECK(table.Plan(A, D, StateTicks{}).ride->alighting == D);
}

TEST_CASE("Precomputed replacement retires rows at publication without retaining two matrices", "[passenger][precomputed][memory]")
{
	std::array services{Service(0, {A, B, C, D})};
	auto old = Build(services);
	std::vector<const PassengerPeriodicService *> records{&services[0]};
	PassengerPrecomputedTable replacement; replacement.Begin(records, PAX, 30);
	size_t published = 0;
	while (!replacement.Finished()) replacement.Step(1, false, [&](StationID destination) {
		old.ReleaseDestination(destination); ++published;
		for (StationID origin : {A, B, C, D}) {
			auto destinations = old.ReachableDestinations(origin);
			CHECK_FALSE(std::binary_search(destinations.begin(), destinations.end(), destination));
			CHECK_FALSE(old.RouteCost(origin, destination));
		}
		CHECK(old.GetDiagnostic().destinations + published == 4);
		CHECK(old.Plan(destination == A ? B : A, destination, StateTicks{}).state == PassengerPrecomputedPlan::State::Building);
	});
	CHECK(published == 4); CHECK(old.GetDiagnostic().destinations == 0);
	CHECK(old.GetDiagnostic().bytes < replacement.GetDiagnostic().bytes);
}

TEST_CASE("Precomputed inverse destinations contain exactly the published finite rows", "[passenger][precomputed][boarding][save]")
{
	std::array services{Service(0, {A, B}), Service(1, {B, D}), Service(2, {C})};
	std::vector<const PassengerPeriodicService *> records; for (const auto &service : services) records.push_back(&service);
	PassengerPrecomputedTable table; table.Begin(records, PAX, 20);
	while (!table.Finished()) {
		table.Step(7);
		for (StationID origin : {A, B, C, D}) {
			auto destinations = table.ReachableDestinations(origin); REQUIRE(std::is_sorted(destinations.begin(), destinations.end()));
			for (StationID destination : {A, B, C, D}) {
				auto cost = table.RouteCost(origin, destination);
				CHECK(std::binary_search(destinations.begin(), destinations.end(), destination) == (cost && *cost != INT64_MAX));
			}
		}
	}
	CHECK(table.RouteCost(A, D).value() > table.RouteCost(B, D).value());
}

TEST_CASE("Precomputed legacy cursors map to the same compact publication frontier", "[passenger][precomputed][determinism][save]")
{
	std::array services{Service(0, {A, B}), Service(1, {B, C}), Service(2, {C, D}), Service(3, {A, D}, 500)};
	std::vector<const PassengerPeriodicService *> records;
	for (const auto &s : services) records.push_back(&s);
	PassengerPrecomputedTable legacy; legacy.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, false);
	for (int i = 0; i < 2000; ++i) {
		legacy.Step(1, true);
		if (i % 17 != 0 && !legacy.Finished()) continue;
		PassengerPrecomputedTable current; current.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, false);
		current.Step(legacy.GetDiagnostic().operations + legacy.LegacyPublicationWork());
		CHECK(current.GetDiagnostic().destinations == legacy.GetDiagnostic().destinations);
		for (StationID from : {A, B, C, D}) for (StationID to : {A, B, C, D}) {
			auto old = legacy.Plan(from, to, StateTicks{}, OrderListID{0}, UINT32_MAX);
			auto now = current.Plan(from, to, StateTicks{}, OrderListID{0}, UINT32_MAX);
			CHECK(now.state == old.state); CHECK(now.ride == old.ride); CHECK(now.cost == old.cost);
		}
		if (legacy.Finished()) break;
	}
	CHECK(legacy.Finished());
}

TEST_CASE("Precomputed CPU scaling benchmark", "[.benchmark][passenger][precomputed][cpu]")
{
	using Clock = std::chrono::steady_clock;
	std::cout << "axis,size,build_us,lookup_ns,p95_ns,p99_ns,bytes,envelope,states,edges,operations,hits\n";
	for (int axis = 0; axis < 5; ++axis) for (int scale : {16, 64, 256, 512}) {
		std::vector<PassengerPeriodicService> services{Service(0, {A, D})};
		if (axis == 4) {
			services = {Service(0, {A, B, C, D}), Service(1, {A, B, C})};
			services[0].timetable.period = 600; services[1].timetable.period = 300;
			services[1].vehicles[0].base = StateTicks{150};
			for (int i = 2; i < scale * 16; ++i) {
				auto &s = services[i % 2]; auto v = s.vehicles[0]; v.id = VehicleID{static_cast<uint32_t>(i)};
				s.vehicles.push_back(v);
			}
		}
		if (axis == 0 || axis == 3) for (int i = 4; i < scale; i += 2) services.push_back(Service(i, {StationID{static_cast<uint16_t>(i)}, StationID{static_cast<uint16_t>(i + 1)}}));
		if (axis == 1) for (int i = 1; i < scale * 16; ++i) {
			auto vehicle = services[0].vehicles.front(); vehicle.id = VehicleID{static_cast<uint32_t>(i)};
			services[0].vehicles.push_back(vehicle);
		}
		auto start = Clock::now(); auto table = Build(services);
		if (axis == 4) REQUIRE(table.GetDiagnostic().shared_sections > 0);
		uint64_t build_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
		std::vector<uint64_t> samples;
		const size_t batches = axis == 2 ? static_cast<size_t>(scale) * 4 : 1024;
		for (size_t i = 0; i < batches; ++i) {
			StationID origin = axis == 3 && i % services.size() ? services[i % services.size()].timetable.stops[0].station : A;
			StationID destination = axis == 3 && i % services.size() ? services[i % services.size()].timetable.stops[1].station : D;
			start = Clock::now();
			uint64_t checksum = 0;
			for (int j = 0; j < 256; ++j) {
				auto plan = table.Plan(origin, destination, StateTicks{}, OrderListID::Invalid(), static_cast<uint32_t>(i * 12347));
				if (plan.ride) checksum += uint64_t(plan.ride->service.base()) + 1;
			}
			samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count() / 256);
			REQUIRE(checksum >= 256);
		}
		uint64_t total = 0; for (uint64_t value : samples) total += value;
		std::sort(samples.begin(), samples.end()); const auto &d = table.GetDiagnostic();
		std::cout << axis << ',' << scale << ',' << build_us << ',' << total / samples.size() << ',' << samples[samples.size() * 95 / 100] << ',' << samples[samples.size() * 99 / 100]
				<< ',' << d.bytes << ',' << d.envelope << ',' << d.states << ',' << d.edges << ',' << d.operations << ',' << d.hits << '\n';
	}
}
