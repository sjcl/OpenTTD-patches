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
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <atomic>
#include <thread>
#include <mutex>
#ifdef WITH_PASSENGER_PARALLEL_REFERENCE
#include "PassengerSequentialReference.h"
#endif

#include "passenger_async_access.h"
#include "passenger_benchmark_cpu.h"
#include "../settings_type.h"
#include "../date_func.h"
#include "../core/backup_type.hpp"

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
	template <typename T> std::vector<StationID> DestinationList(const T &destinations)
	{
		if constexpr (std::is_same_v<typename T::value_type, StationID>) return {destinations.begin(), destinations.end()};
		else {
			std::vector<StationID> result;
			for (auto span : destinations) {
				REQUIRE(std::is_sorted(span.begin(), span.end()));
				result.insert(result.end(), span.begin(), span.end());
			}
			REQUIRE(std::is_sorted(result.begin(), result.end()));
			REQUIRE(std::adjacent_find(result.begin(), result.end()) == result.end());
			return result;
		}
	}
}

namespace {
	template <typename Predicate> void WaitForPassengerWorkers(Predicate ready)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!ready() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		REQUIRE(ready());
	}
	void PrimePassengerWorkers(PassengerPrecomputedTable &table, uint workers)
	{
		PassengerPrecomputedTestAccess::Prime(table, workers);
		WaitForPassengerWorkers([&]() { return PassengerPrecomputedTestAccess::Ready(table); });
	}
}

TEST_CASE("Hyperpath fixed point preserves single waits and exact acceptance", "[passenger][hyperpath]")
{
	for (int64_t wait : std::initializer_list<int64_t>{1, 2, 3, 100, 65537, PassengerStrategy::MAX_WAIT}) {
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

TEST_CASE("Aircraft table costs preserve physical metadata and separate identical train patterns", "[passenger][aircraft_cost][precomputed][hyperpath]")
{
	std::array services{Service(0, {A, D}), Service(1, {A, D})};
	services[0].vehicles.front().type = VehicleType::Aircraft;
	std::array records{&services[0], &services[1]};
	for (bool hyperpath : {false, true}) for (bool optimized : {false, true}) for (bool flat : {false, true}) {
		CAPTURE(hyperpath, optimized, flat);
		PassengerPrecomputedTable table;
		table.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, optimized, true, hyperpath, true, true, flat, {100, 1000});
		while (!table.Finished()) table.Step(7);
		CHECK(table.GetDiagnostic().patterns == 2);
		auto plan = table.Plan(A, D, StateTicks{}); REQUIRE(plan.ride);
		CHECK(plan.riding == 100); CHECK(plan.waiting == 110);
		if (hyperpath) {
			size_t work = 10000; auto routes = table.InspectRoutes(A, D, OrderListID::Invalid(), OrderListID::Invalid(), work);
			REQUIRE(routes.candidates.size() == 1); CHECK(routes.candidates.front().ride.service == services[1].id);
		} else CHECK(plan.ride->service == services[1].id);
	}
	std::array one{&services[0]};
	for (bool hyperpath : {false, true}) {
		int64_t base = 0;
		for (auto cost : {PassengerAircraftCost{}, PassengerAircraftCost{50, 74}, PassengerAircraftCost{300, 0}}) {
			PassengerPrecomputedTable table;
			table.Begin(one, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, hyperpath, true, true, true, cost);
			while (!table.Finished()) table.Step(65536);
			auto plan = table.Plan(A, D, StateTicks{}); REQUIRE(plan.ride);
			if (cost.IsDefault()) base = plan.cost;
			CHECK(plan.cost == base + cost.Adjustment(100));
			CHECK(plan.riding == 100); CHECK(plan.waiting == 110);
		}
	}
}

TEST_CASE("Aircraft preference arithmetic saturates without overflowing its ratio", "[passenger][aircraft_cost]")
{
	CHECK(PassengerAircraftCost{}.Cost(INT64_MAX) == INT64_MAX);
	CHECK(PassengerAircraftCost{50, 0}.Cost(INT64_MAX) == INT64_MAX / 2);
	CHECK(PassengerAircraftCost{65535, UINT32_MAX}.Cost(INT64_MAX) == INT64_MAX);
	CHECK(PassengerAircraftCost{10, 0}.Cost(1) == 0);
	CHECK(PassengerAircraftCost{50, 74}.Cost(101) == 124);
}

TEST_CASE("Aircraft settings are frozen across cooperative graphs and asymmetric worker timing", "[passenger][aircraft_cost][parallel][network]")
{
	AutoRestoreBackup settings{_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup days{DateDetail::_effective_day_length, uint8_t{1}};
	std::array services{Service(0, {A, B, A, D}), Service(1, {A, C, D}, 120), Service(2, {B, C}, 70)};
	services[0].vehicles.front().type = VehicleType::Aircraft;
	std::array records{&services[0], &services[1], &services[2]};
	for (bool hyperpath : {false, true}) for (auto cost : {PassengerAircraftCost{50, 0}, PassengerAircraftCost{300, 74}}) {
		PassengerPrecomputedTable serial, parallel;
		serial.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, true, {}, true, true, true, hyperpath, true, true, true, cost);
		_settings_game.linkgraph.aircraft_link_scale = cost.scale;
		_settings_game.linkgraph.aircraft_link_add = cost.addition == 74 ? 1 : 0;
		parallel.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, true, {}, true, true, true, hyperpath);
		// Both graph preparation and workers must ignore subsequent global edits.
		_settings_game.linkgraph.aircraft_link_scale = 700; _settings_game.linkgraph.aircraft_link_add = 365;
		for (auto *table : {&serial, &parallel}) while (!table->PrepareGraph(7)) {}
		PrimePassengerWorkers(parallel, 4);
		while (!serial.Finished()) {
			CHECK(serial.Step(7) == parallel.StepParallel(7, 4));
			REQUIRE(PassengerPrecomputedTestAccess::Published(serial) == PassengerPrecomputedTestAccess::Published(parallel));
		}
		CHECK(parallel.Finished());
	}
}

TEST_CASE("Parallel passenger searches preserve every publication boundary and stored field", "[passenger][precomputed][parallel][save][network]")
{
	std::array services{Service(0, {A, B, C}), Service(1, {B, D}), Service(2, {A, C, D}, 170), Service(3, {A, B, A, D}, 73)};
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	for (bool hyperpath : {false, true}) for (bool flat : {false, true}) for (uint workers : {0U, 1U, 2U, 4U, 8U}) {
		CAPTURE(hyperpath, flat, workers);
		PassengerPrecomputedTable serial, parallel;
		for (auto *table : {&serial, &parallel}) table->Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, hyperpath, true, true, flat);
		std::vector<StationID> expected, actual;
		size_t step = 0;
		while (!serial.Finished()) {
			const size_t chunk = std::array<size_t, 5>{1, 7, 31, 97, 13}[step++ % 5];
			REQUIRE(parallel.StepParallel(chunk, workers, [&](StationID d) { actual.push_back(d); }) == serial.Step(chunk, false, [&](StationID d) { expected.push_back(d); }));
			REQUIRE(actual == expected);
			REQUIRE(PassengerPrecomputedTestAccess::Published(parallel) == PassengerPrecomputedTestAccess::Published(serial));
			if (step % 17 == 0) {
				/* A loader reconstructs this same prefix with ordinary Step. */
				PassengerPrecomputedTable replay;
				replay.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, hyperpath, true, true, flat);
				replay.Step(static_cast<size_t>(parallel.GetDiagnostic().operations));
				REQUIRE(PassengerPrecomputedTestAccess::Published(replay) == PassengerPrecomputedTestAccess::Published(parallel));
			}
		}
		CHECK(parallel.Finished());
		for (auto from : {A, B, C, D}) for (auto to : {A, B, C, D}) for (const auto &s : services) {
			for (uint32_t sample : {0U, 17U, UINT32_MAX}) {
				auto a = serial.Plan(from, to, StateTicks{42}, s.id, sample), b = parallel.Plan(from, to, StateTicks{42}, s.id, sample);
				CHECK(a.state == b.state); CHECK(a.ride == b.ride); CHECK(a.cost == b.cost); CHECK(a.riding == b.riding); CHECK(a.waiting == b.waiting); CHECK(a.transfers == b.transfers); CHECK(a.distance == b.distance);
			}
		}
		CHECK(parallel.GetDiagnostic().board_events == serial.GetDiagnostic().board_events);
		CHECK(parallel.GetDiagnostic().heap_pops == serial.GetDiagnostic().heap_pops);
		/* A row finished by a worker merges its policy statistics at publication. */
		CHECK(parallel.GetDiagnostic().policy_count == serial.GetDiagnostic().policy_count);
		CHECK(parallel.GetDiagnostic().candidate_count == serial.GetDiagnostic().candidate_count);
		CHECK(parallel.GetDiagnostic().candidate_histogram == serial.GetDiagnostic().candidate_histogram);
		CHECK(parallel.GetDiagnostic().base_policy_bytes == serial.GetDiagnostic().base_policy_bytes);
		CHECK(parallel.GetParallelDiagnostic().peak_pending <= workers * 2);
	}
}

TEST_CASE("Worker-finished Hyperpath rows apply every table effect at the serial Work boundary", "[passenger][precomputed][parallel][hyperpath][network]")
{
	/* Every lookahead row is ready before each slice, so rows are finished by workers and
	 * replayed; small slices stop inside the label and policy phases of those rows. */
	std::array services{Service(0, {A, B, C}), Service(1, {B, D}), Service(2, {A, C, D}, 170), Service(3, {A, B, A, D}, 73)};
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	for (bool flat : {false, true}) {
		CAPTURE(flat);
		PassengerPrecomputedTable serial, parallel;
		for (auto *table : {&serial, &parallel}) table->Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, true, true, true, flat);
		std::vector<StationID> expected, actual;
		size_t step = 0;
		while (!serial.Finished()) {
			if (PassengerPrecomputedTestAccess::Prime(parallel, 4)) WaitForPassengerWorkers([&]() { return PassengerPrecomputedTestAccess::Ready(parallel); });
			const size_t chunk = std::array<size_t, 5>{1, 3, 7, 31, 97}[step++ % 5];
			REQUIRE(parallel.StepParallel(chunk, 4, [&](StationID d) { actual.push_back(d); }) == serial.Step(chunk, false, [&](StationID d) { expected.push_back(d); }));
			REQUIRE(actual == expected);
			REQUIRE(PassengerPrecomputedTestAccess::Published(parallel) == PassengerPrecomputedTestAccess::Published(serial));
		}
		CHECK(parallel.Finished());
		CHECK(parallel.GetParallelDiagnostic().finished_rows > 0);
		CHECK(parallel.GetParallelDiagnostic().finish_effects > 0);
		CHECK(parallel.GetDiagnostic().candidate_histogram == serial.GetDiagnostic().candidate_histogram);
		CHECK(parallel.GetDiagnostic().policy_count == serial.GetDiagnostic().policy_count);
	}
}

TEST_CASE("Parallel lookahead can be discarded midway without double charging search Work", "[passenger][precomputed][parallel][save]")
{
	std::array services{Service(0, {A, B, C}), Service(1, {B, D})};
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	for (bool hyperpath : {false, true}) for (size_t prefix : {size_t{1}, size_t{31}, size_t{97}}) {
		PassengerPrecomputedTable serial, parallel;
		for (auto *table : {&serial, &parallel}) table->Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, hyperpath);
		PrimePassengerWorkers(parallel, 4);
		serial.Step(prefix); parallel.StepParallel(prefix, 4);
		if (prefix == 1) REQUIRE(PassengerPrecomputedTestAccess::Pending(parallel));
		CHECK(PassengerPrecomputedTestAccess::Published(parallel) == PassengerPrecomputedTestAccess::Published(serial));
		while (!serial.Finished()) {
			CHECK(parallel.Step(7) == serial.Step(7));
			REQUIRE(PassengerPrecomputedTestAccess::Published(parallel) == PassengerPrecomputedTestAccess::Published(serial));
		}
		/* Reset may abandon jobs without retaining a pointer into the table. */
		parallel.Begin(records, PAX, 20); parallel.StepParallel(1); parallel.Begin(records, PAX, 20);
		while (!parallel.Finished()) parallel.StepParallel(65536);
		CHECK_FALSE(parallel.MemoryRejected());
	}
}

#ifdef WITH_PASSENGER_PARALLEL_REFERENCE
TEST_CASE("Passenger destination refactor matches the pinned original engine at every sliced frontier", "[passenger][precomputed][parallel][reference]")
{
	std::array services{Service(0, {A, B, C}), Service(1, {B, D}), Service(2, {A, C, D}, 170), Service(3, {A, B, A, D}, 73)};
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	for (bool hyperpath : {false, true}) for (bool flat : {false, true}) for (uint workers : {0U, 4U}) {
		PassengerSequentialReference reference; PassengerPrecomputedTable candidate;
		reference.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, hyperpath, true, true, flat);
		candidate.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, hyperpath, true, true, flat);
		if (workers != 0) PrimePassengerWorkers(candidate, workers);
		while (!reference.Finished()) {
			REQUIRE(candidate.StepParallel(17, workers) == reference.Step(17));
			REQUIRE(PassengerPrecomputedTestAccess::Published(reference) == PassengerPrecomputedTestAccess::Published(candidate));
		}
	}
}
#endif

TEST_CASE("Slow and failed passenger workers cannot change logical results", "[passenger][precomputed][parallel][network][memory]")
{
	std::array services{Service(0, {A, B, C}), Service(1, {B, D})};
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	static std::mutex mutex;
	static std::vector<size_t> completed;
	static std::atomic<size_t> active{0}, peak{0};
	for (bool hyperpath : {false, true}) for (bool fail : {false, true}) {
		completed.clear(); active = 0; peak = 0;
		PassengerPrecomputedTable serial, parallel;
		for (auto *table : {&serial, &parallel}) table->Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, hyperpath);
		PassengerPrecomputedTestAccess::Hook(parallel, fail ? +[](size_t d, bool done) { if (d == 0 && !done) throw std::bad_alloc{}; } : +[](size_t d, bool done) {
			if (!done) {
				size_t n = ++active, previous = peak.load(); while (previous < n && !peak.compare_exchange_weak(previous, n)) {}
				if (d == 0) std::this_thread::sleep_for(std::chrono::milliseconds(30));
			} else { --active; std::lock_guard<std::mutex> lock(mutex); completed.push_back(d); }
		});
		PrimePassengerWorkers(parallel, 4);
		while (!serial.Finished()) {
			CHECK(parallel.StepParallel(17, 4) == serial.Step(17));
			REQUIRE(PassengerPrecomputedTestAccess::Published(parallel) == PassengerPrecomputedTestAccess::Published(serial));
		}
		WaitForPassengerWorkers([&]() { return PassengerPrecomputedTestAccess::Idle(parallel); });
		if (fail) CHECK(parallel.GetParallelDiagnostic().fallbacks == 1);
		else if (std::thread::hardware_concurrency() > 1) { REQUIRE_FALSE(completed.empty()); CHECK(completed.front() != 0); CHECK(peak > 1); }
	}
	for (uint workers : {0U, 1U, 2U, 4U, 8U}) {
		PassengerPrecomputedTable rejected;
		rejected.Begin(records, PAX, 20, 1024);
		rejected.StepParallel(65536, workers);
		CHECK(rejected.MemoryRejected()); CHECK(rejected.GetParallelDiagnostic().searches == 0);
	}
}

TEST_CASE("Passenger worker startup failure and partial scalar searches fall back at the same Work", "[passenger][precomputed][parallel][save]")
{
	std::array services{Service(0, {A, B, C}), Service(1, {B, D})};
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	for (bool optimized : {false, true}) for (uint started : {0U, 1U, 4U}) {
		PassengerPrecomputedTable serial, parallel;
		for (auto *table : {&serial, &parallel}) table->Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, optimized);
		if (started == 0) PassengerPrecomputedTestAccess::StartHook(parallel, +[](uint) { return false; });
		if (started == 1) PassengerPrecomputedTestAccess::StartHook(parallel, +[](uint i) { return i == 0; });
		if (started == 4) { parallel.Step(7); serial.Step(7); } // Partially initialized, including scalar Label scratch.
		while (!serial.Finished()) {
			REQUIRE(parallel.StepParallel(13, 4) == serial.Step(13));
			REQUIRE(PassengerPrecomputedTestAccess::Published(parallel) == PassengerPrecomputedTestAccess::Published(serial));
		}
		if (started == 0) CHECK(parallel.GetParallelDiagnostic().fallbacks == 1);
		if (started == 1) CHECK(parallel.GetParallelDiagnostic().workers == 1);
	}
}


TEST_CASE("A late passenger row is discarded after the first serial Work unit", "[passenger][precomputed][parallel][network][save]")
{
	std::array services{Service(0, {A, B, C}), Service(1, {B, D})};
	std::vector<const PassengerPeriodicService *> records; for (const auto &service : services) records.push_back(&service);
	static std::atomic<bool> release{true};
	static std::atomic<uint> entered{0};
	struct Release { ~Release() { release.store(true); } } guard;
	for (bool hyperpath : {false, true}) {
		release.store(false); entered.store(0);
		PassengerPrecomputedTable serial, parallel;
		for (auto *table : {&serial, &parallel}) table->Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, hyperpath);
		PassengerPrecomputedTestAccess::Hook(parallel, +[](size_t d, bool done) {
			if (d != 0 || done) return;
			++entered; while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		});
		REQUIRE(PassengerPrecomputedTestAccess::Prime(parallel, 4));
		WaitForPassengerWorkers([&]() { return entered.load() != 0; });
		auto refreshed = services.front(); ++refreshed.generation;
		REQUIRE(parallel.RefreshService(refreshed)); REQUIRE(serial.RefreshService(refreshed));
		REQUIRE(parallel.StepParallel(7, 4) == serial.Step(7));
		CHECK_FALSE(release.load()); // This returned while the required Worker was blocked.
		REQUIRE(PassengerPrecomputedTestAccess::Serial(parallel));
		REQUIRE_FALSE(PassengerPrecomputedTestAccess::Pending(parallel));
		release.store(true);
		WaitForPassengerWorkers([&]() { return PassengerPrecomputedTestAccess::Idle(parallel); });
		while (!serial.Finished()) {
			REQUIRE(parallel.StepParallel(7, 4) == serial.Step(7));
			REQUIRE(PassengerPrecomputedTestAccess::Published(parallel) == PassengerPrecomputedTestAccess::Published(serial));
			if (parallel.GetDiagnostic().destinations == 0) CHECK(parallel.GetParallelDiagnostic().cache_hits == 0);
		}
		CHECK(parallel.GetParallelDiagnostic().cache_hits > 0); // Future rows still use completed lookahead.
		CHECK(parallel.Plan(A, D, StateTicks{}).ride == serial.Plan(A, D, StateTicks{}).ride);
	}
}

TEST_CASE("Persistently late passenger workers back off without delaying any logical row", "[passenger][precomputed][parallel][network]")
{
	auto service = Service(0, {}); int64_t cursor = 0;
	for (uint16_t station = 0; station < 24; ++station) {
		service.timetable.stops.push_back({StationID{station}, station, cursor, cursor + 10, 1, 1, 0}); cursor += 110;
	}
	service.timetable.period = cursor;
	std::vector<const PassengerPeriodicService *> records{&service};
	static std::atomic<bool> release{true}; static std::atomic<uint> entered{0};
	struct Release { ~Release() { release.store(true); } } guard;
	for (bool hyperpath : {false, true}) {
		release.store(false); entered.store(0);
		PassengerPrecomputedTable serial, parallel;
		for (auto *table : {&serial, &parallel}) table->Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, hyperpath);
		PassengerPrecomputedTestAccess::Hook(parallel, +[](size_t, bool done) {
			if (!done) { ++entered; while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
		});
		REQUIRE(PassengerPrecomputedTestAccess::Prime(parallel, 4));
		WaitForPassengerWorkers([&]() { return entered.load() != 0; });
		while (!serial.Finished()) {
			REQUIRE(parallel.StepParallel(97, 4) == serial.Step(97));
			REQUIRE(PassengerPrecomputedTestAccess::Published(parallel) == PassengerPrecomputedTestAccess::Published(serial));
		}
		CHECK_FALSE(release.load()); CHECK(parallel.GetParallelDiagnostic().throttles == 1); CHECK(parallel.GetParallelDiagnostic().cache_hits == 0);
		release.store(true); WaitForPassengerWorkers([&]() { return PassengerPrecomputedTestAccess::Idle(parallel); });
	}
}

TEST_CASE("Passenger search snapshots survive move reset and opposite-mode replacement", "[passenger][precomputed][parallel][memory]")
{
	std::array services{Service(0, {A, B, C}), Service(1, {B, D})};
	std::vector<const PassengerPeriodicService *> records; for (const auto &service : services) records.push_back(&service);
	static std::atomic<bool> release{true}; static std::atomic<uint> active{0};
	struct Release { ~Release() { release.store(true); } } guard;
	for (bool hyperpath : {false, true}) {
		release.store(false); active.store(0);
		PassengerPrecomputedTable original;
		original.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, hyperpath);
		PassengerPrecomputedTestAccess::Hook(original, +[](size_t d, bool done) {
			if (d != 0) return;
			if (!done) { ++active; while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
			else --active;
		});
		REQUIRE(PassengerPrecomputedTestAccess::Prime(original, 4));
		WaitForPassengerWorkers([&]() { return active.load() != 0; });
		auto moved = std::move(original); original = {};
		moved.Begin(records, PAX, 37, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, !hyperpath);
		PassengerPrecomputedTable serial; serial.Begin(records, PAX, 37, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, !hyperpath);
		while (!serial.Finished()) {
			REQUIRE(moved.StepParallel(31, 0) == serial.Step(31));
			REQUIRE(PassengerPrecomputedTestAccess::Published(moved) == PassengerPrecomputedTestAccess::Published(serial));
		}
		CHECK_FALSE(release.load()); // Reset and a complete build never waited for the old graph.
		release.store(true); WaitForPassengerWorkers([&]() { return active.load() == 0; });
	}
}

TEST_CASE("Passenger workers may finish in strictly reversed destination order", "[passenger][precomputed][parallel][network]")
{
	if (std::thread::hardware_concurrency() < 4) return;
	std::array services{Service(0, {A, B, C, D})};
	std::vector<const PassengerPeriodicService *> records{&services.front()};
	static std::atomic<int> next{3}; static std::atomic<bool> abort{false};
	static std::vector<size_t> completion; static std::mutex mutex;
	struct Release { ~Release() { abort.store(true); } } guard;
	for (bool hyperpath : {false, true}) {
		next.store(3); abort.store(false); completion.clear();
		PassengerPrecomputedTable serial, parallel;
		for (auto *table : {&serial, &parallel}) table->Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, hyperpath);
		PassengerPrecomputedTestAccess::Hook(parallel, +[](size_t d, bool done) {
			if (!done) return;
			while (next.load() != static_cast<int>(d) && !abort.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			{ std::lock_guard lock(mutex); completion.push_back(d); } --next;
		});
		PrimePassengerWorkers(parallel, 4);
		CHECK((completion == std::vector<size_t>{3, 2, 1, 0}));
		while (!serial.Finished()) {
			REQUIRE(parallel.StepParallel(1, 4) == serial.Step(1));
			REQUIRE(PassengerPrecomputedTestAccess::Published(parallel) == PassengerPrecomputedTestAccess::Published(serial));
		}
		CHECK(parallel.GetParallelDiagnostic().cache_hits == 4);
	}
}

TEST_CASE("A worker exception after a search quantum preserves every later Work boundary", "[passenger][precomputed][parallel][memory][network]")
{
	std::vector<PassengerPeriodicService> services;
	for (uint16_t line = 0; line < 24; ++line) {
		auto service = Service(line, {}); int64_t cursor = 0;
		for (uint16_t station = 0; station < 48; ++station) {
			service.timetable.stops.push_back({StationID{station}, station, cursor, cursor + 10, 1, 1, 0}); cursor += 110 + line;
		}
		service.timetable.period = cursor; services.push_back(std::move(service));
	}
	std::vector<const PassengerPeriodicService *> records; for (const auto &service : services) records.push_back(&service);
	for (bool hyperpath : {false, true}) {
		PassengerPrecomputedTable serial, parallel;
		for (auto *table : {&serial, &parallel}) table->Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, hyperpath);
		PassengerPrecomputedTestAccess::QuantumHook(parallel, +[](size_t d, uint64_t work) { if (d == 0 && work >= 4096) throw std::bad_alloc{}; });
		PrimePassengerWorkers(parallel, 4);
		while (!serial.Finished()) {
			REQUIRE(parallel.StepParallel(65536, 4) == serial.Step(65536));
			REQUIRE(PassengerPrecomputedTestAccess::Published(parallel) == PassengerPrecomputedTestAccess::Published(serial));
		}
		WaitForPassengerWorkers([&]() { return PassengerPrecomputedTestAccess::Idle(parallel); });
		CHECK(parallel.GetParallelDiagnostic().fallbacks == 1);
		CHECK(parallel.GetParallelDiagnostic().cancelled_search_work >= 4096);
	}
}

TEST_CASE("Hyperpath fixed point matches independent small exact rational strategies", "[passenger][hyperpath][reference]")
{
	/* LCM(1..16) is an exact rational unit for this independent oracle.
	 * Its numerator remains below 20*720720*1024+720720 < 2^34;
	 * this oracle does not use PassengerWide or PassengerStrategy arithmetic.
	 * Away from exact ties its smallest possible gap exceeds 1/(20*720720),
	 * greater than the Q48 rounding bound for these costs/waits. At a tie,
	 * rounding frequencies upward cannot turn a rejected x into an acceptance. */
	constexpr uint64_t UNIT = 720720;
	uint32_t seed = 0x18d2b4a7;
	for (size_t trial = 0; trial < 256; ++trial) {
		std::vector<std::tuple<int64_t, int64_t, size_t>> choices;
		for (size_t i = 0; i < 1 + trial % 20; ++i) {
			seed = seed * 1664525U + 1013904223U; int64_t wait = 1 + seed % 16;
			seed = seed * 1664525U + 1013904223U; int64_t cost = seed % 1024;
			choices.emplace_back(cost, wait, i);
		}
		std::sort(choices.begin(), choices.end());
		uint64_t numerator = UNIT, frequency = 0;
		PassengerStrategy production;
		for (auto [cost, wait, id] : choices) {
			bool accepted = frequency == 0 || uint64_t(cost) * frequency < numerator;
			CAPTURE(trial, id, cost, wait, numerator, frequency, production.Cost());
			CHECK(production.Add(cost, PassengerStrategy::Frequency(wait)) == accepted);
			if (accepted) { frequency += UNIT / wait; numerator += UNIT / wait * cost; }
			CHECK(production.Cost() == int64_t((numerator + frequency - 1) / frequency));
		}
	}
}

TEST_CASE("Hyperpath frozen graph matches an independent multi-stage rational event oracle", "[passenger][hyperpath][reference]")
{
	std::array services{Service(0, {A, B}, 100), Service(1, {B, D}, 100), Service(2, {B, D}, 100), Service(3, {A, D}, 220)};
	std::vector<const PassengerPeriodicService *> records; for (const auto &service : services) records.push_back(&service);
	PassengerPrecomputedTable table; table.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, true);
	while (!table.Finished()) table.Step(7);
	// passenger_hyperpath_reference.py: B=159, A=11087/34, attractive A={3,0}.
	CHECK(table.Plan(B, D, StateTicks{}).cost == 159);
	CHECK(table.Plan(A, D, StateTicks{}).cost == 327);
	auto set = table.Inspect(A, D); REQUIRE(set.size() == 2);
	for (const auto &candidate : set) {
		REQUIRE((candidate.service == OrderListID{0} || candidate.service == OrderListID{3}));
		CHECK(candidate.cost == (candidate.service == OrderListID{0} ? 263 : 228));
		CHECK(candidate.wait == (candidate.service == OrderListID{0} ? 110 : 230));
		auto ride = table.Board(A, D, StateTicks{}, candidate.service, candidate.order);
		REQUIRE(ride.ride); CHECK(ride.cost == candidate.cost);
		CHECK(ride.ride->alighting == (candidate.service == OrderListID{0} ? B : D));
	}
}

TEST_CASE("Route inspection shares Hyperpath frequencies without changing simulation caches", "[passenger][route-overlay]")
{
	std::array services{Service(0, {A, B}, 100), Service(1, {B, D}, 100), Service(2, {A, D}, 220)};
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	PassengerPrecomputedTable table;
	table.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, true);
	while (!table.Finished()) table.Step(7);
	for (auto previous : {OrderListID::Invalid(), OrderListID{0}}) {
		for (auto failed : {OrderListID::Invalid(), OrderListID{2}}) {
			const auto before = table.GetDiagnostic(); const auto memory = table.InspectMemory();
			size_t work = 4096;
			auto routes = table.InspectRoutes(A, D, previous, failed, work);
			REQUIRE(routes.state == PassengerPrecomputedPlan::State::Ready);
			CHECK(table.GetDiagnostic().queries == before.queries);
			CHECK(table.GetDiagnostic().bytes == before.bytes);
			CHECK(table.GetDiagnostic().local_replans == before.local_replans);
			CHECK(table.InspectMemory().local == memory.local);
			CHECK(table.InspectMemory().reported == memory.reported);
			auto candidates = table.Candidates(A, D, previous, failed);
			REQUIRE(routes.candidates.size() == candidates.size());
			double frequency = 0;
			for (const auto &route : routes.candidates) frequency += PassengerStrategy::Frequency(*table.ServiceWait(route.ride.service));
			double probability = 0;
			for (const auto &route : routes.candidates) {
				CHECK(route.probability == Approx(PassengerStrategy::Frequency(*table.ServiceWait(route.ride.service)) / frequency));
				CHECK(route.ride.service != previous);
				auto actual = table.Board(A, D, StateTicks{}, route.ride.service, route.ride.boarding_order, previous, failed);
				REQUIRE(actual.ride);
				CHECK(route.ride.alighting == actual.ride->alighting);
				probability += route.probability;
			}
			CHECK(probability == Approx(1.0));
		}
	}
	size_t work = 0;
	CHECK(table.InspectRoutes(A, D, OrderListID::Invalid(), OrderListID::Invalid(), work).candidates.empty());
}

TEST_CASE("Route inspection bounds branches without reallocating their probability or splicing short turns", "[passenger][route-overlay]")
{
	std::vector<PassengerPeriodicService> services;
	for (uint16_t i = 0; i < 80; ++i) services.push_back(Service(i, {A, B, D}));
	services.push_back(Service(80, {A, B}));
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	PassengerPrecomputedTable table;
	table.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, true);
	while (!table.Finished()) table.Step(65536);
	size_t work = 65536;
	auto routes = table.InspectRoutes(A, D, OrderListID::Invalid(), OrderListID::Invalid(), work);
	REQUIRE(routes.candidates.size() == 64);
	double probability = 0;
	for (const auto &candidate : routes.candidates) {
		if (candidate.ride.service == OrderListID{80}) CHECK(candidate.ride.alighting == B);
		probability += candidate.probability;
	}
	CHECK(probability > 0); CHECK(probability < 1);
	work = 1;
	CHECK(table.InspectRoutes(A, D, OrderListID::Invalid(), OrderListID::Invalid(), work).candidates.empty());
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
	std::set<StationID> station_set;
	for (const auto *service : records) for (const auto &stop : service->timetable.stops) if ((stop.board | stop.alight) & 1) station_set.insert(stop.station);
	std::vector<StationID> all_stations(station_set.begin(), station_set.end());
	std::vector<int64_t> old_costs; old_costs.reserve(all_stations.size() * all_stations.size());
	{
		auto started = std::chrono::steady_clock::now();
		PassengerPrecomputedTable old; old.Begin(records, PAX, 20);
		while (!old.Finished()) old.Step(16 * 1024 * 1024);
		const auto &d = old.GetDiagnostic();
		std::cout << "hyperpath_previous_build," << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() << ',' << d.operations << ',' << d.bytes << ',' << d.envelope << std::endl;
		for (size_t i = 0; i < ods.size(); ++i) old_cost[i] = old.RouteCost(ods[i].first, ods[i].second).value_or(INT64_MAX);
		for (auto from : all_stations) for (auto to : all_stations) old_costs.push_back(old.RouteCost(from, to).value_or(INT64_MAX));
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
	uint64_t improved = 0, equal = 0, worse = 0, newly_reachable = 0, newly_unreachable = 0; int64_t maximum_increase = 0;
	PassengerWide old_sum, new_sum;
	size_t position = 0;
	for (auto from : all_stations) for (auto to : all_stations) {
		int64_t before = old_costs[position++], after = table.RouteCost(from, to).value_or(INT64_MAX);
		if (before == INT64_MAX || after == INT64_MAX) {
			newly_reachable += before == INT64_MAX && after != INT64_MAX;
			newly_unreachable += before != INT64_MAX && after == INT64_MAX; continue;
		}
		old_sum.Add({0, uint64_t(before)}); new_sum.Add({0, uint64_t(after)});
		improved += after < before; equal += after == before; worse += after > before;
		if (after > before) maximum_increase = std::max(maximum_increase, after - before);
	}
	std::cout << "hyperpath_all_od," << improved << ',' << equal << ',' << worse << ',' << newly_reachable << ',' << newly_unreachable << ',' << maximum_increase << ','
			<< old_sum.high << ',' << old_sum.low << ',' << new_sum.high << ',' << new_sum.low << std::endl;
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
	for (const auto &candidate : set) {
		auto ride = table.Board(A, D, StateTicks{}, candidate.service, candidate.order, OrderListID{0});
		REQUIRE(ride.ride); CHECK(ride.cost == candidate.cost);
		if (candidate.service == OrderListID{2}) {
			CHECK((services[2].timetable.stops.front().board & 1) != 0);
			CHECK((services[2].timetable.stops[ride.ride->alighting_order].alight & 1) != 0);
			if (services[2].timetable.stops[1].forced) CHECK(ride.ride->alighting_order <= 1);
		}
	}
}

TEST_CASE("Hyperpath preserves repeated station occurrences and chooses one alight per departure", "[passenger][hyperpath][occurrence]")
{
	std::array services{Service(0, {A, B, A, D}, 100)};
	for (auto &stop : services[0].timetable.stops) stop.departure = stop.arrival; // Zero dwell at the repeated station too.
	std::vector<const PassengerPeriodicService *> records{&services[0]};
	PassengerPrecomputedTable table; table.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, true);
	while (!table.Finished()) table.Step(1);
	auto set = table.Inspect(A, D); REQUIRE_FALSE(set.empty());
	std::set<VehicleOrderID> boards;
	for (const auto &candidate : set) {
		CHECK(boards.insert(candidate.order).second);
		CHECK((candidate.order == 0 || candidate.order == 2));
		auto ride = table.Board(A, D, StateTicks{}, candidate.service, candidate.order);
		REQUIRE(ride.ride); CHECK(ride.ride->alighting_order == 3); CHECK(ride.cost == candidate.cost);
	}
	CHECK(boards.contains(2)); CHECK(table.GetDiagnostic().section_rides == 0);
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
	bool refitted = false;
	SECTION("Newer passenger ownership") {}
	SECTION("Newer ownership refitted away from passengers") { services[1].vehicles[0].capacity[PAX] = 0; refitted = true; }
	std::vector<const PassengerPeriodicService *> records{&services[0], &services[1]};
	PassengerPrecomputedTable table; table.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, true);
	while (!table.Finished()) table.Step(1);
	auto set = table.Inspect(A, D);
	if (refitted) { CHECK(set.empty()); CHECK_FALSE(table.Plan(A, D, StateTicks{}).ride); return; }
	REQUIRE(set.size() == 1);
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
	// Display the cached policy's chosen service, including its no-alternative retry.
	const auto before = table.GetDiagnostic();
	std::vector<PassengerPrecomputedTable::DisplayCandidate> displayed; size_t work = 4096;
	REQUIRE(table.DisplayCandidates(A, D, OrderListID::Invalid(), OrderListID{0}, displayed, work));
	REQUIRE(displayed.size() == 1);
	CHECK(displayed.front().service == chosen); CHECK(displayed.front().order == 0);
	CHECK(table.GetDiagnostic().local_replans == before.local_replans);
	CHECK(table.GetDiagnostic().bytes == before.bytes);
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
	old.PrepareRetirement(replacement);
	size_t published = 0;
	while (!replacement.Finished()) replacement.Step(1, false, [&](StationID destination) {
		old.ReleaseDestination(destination); ++published;
		for (StationID origin : {A, B, C, D}) {
			auto destinations = DestinationList(old.ReachableDestinations(origin));
			CHECK_FALSE(std::binary_search(destinations.begin(), destinations.end(), destination));
			CHECK_FALSE(old.RouteCost(origin, destination));
		}
		CHECK(old.GetDiagnostic().destinations + published == 4);
		CHECK(old.Plan(destination == A ? B : A, destination, StateTicks{}).state == PassengerPrecomputedPlan::State::Building);
	});
	CHECK(published == 4); CHECK(old.GetDiagnostic().destinations == 0);
	CHECK(old.GetDiagnostic().bytes < replacement.GetDiagnostic().bytes);
}

TEST_CASE("Retirement preserves every sliced reachable set across added and missing stations", "[passenger][precomputed][memory][save]")
{
	const std::array origins{A, B, C, D, StationID{4}, StationID{7}, StationID{8}, StationID{9}, StationID{10}};
	std::array previous{Service(0, {A, B, C, D, StationID{7}, StationID{9}}), Service(1, {A, StationID{10}})};
	std::array next{Service(0, {B, D, StationID{4}, StationID{8}}), Service(1, {StationID{4}, StationID{10}})};
	/* 3 is Hyperpath: it forces the integrated engine but keeps per-state rows,
	 * and must share the watermark retirement with compressed rows in either direction. */
	for (auto [algorithm, replacement_algorithm] : {std::pair{0U, 0U}, std::pair{1U, 1U}, std::pair{2U, 2U}, std::pair{0U, 2U}, std::pair{1U, 2U},
			std::pair{3U, 3U}, std::pair{2U, 3U}, std::pair{3U, 2U}, std::pair{0U, 3U}}) {
		uint64_t work = 0;
		for (size_t quantum : {1U, 2U, 3U, 4U, 5U, 7U, 64U}) {
			CAPTURE(algorithm, replacement_algorithm, quantum);
			PassengerPrecomputedTable retained, current;
			std::vector<const PassengerPeriodicService *> records;
			for (const auto &s : previous) records.push_back(&s);
			retained.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, algorithm != 0, algorithm >= 2, algorithm == 3);
			while (!retained.Finished()) retained.Step(65536);
			REQUIRE(retained.IsHyperpath() == (algorithm == 3));
			std::map<StationID, std::vector<StationID>> expected;
			std::map<std::pair<StationID, StationID>, std::optional<int64_t>> costs;
			for (StationID origin : origins) {
				expected[origin] = DestinationList(retained.ReachableDestinations(origin));
				for (StationID destination : origins) costs[{origin, destination}] = retained.RouteCost(origin, destination);
			}
			CHECK(std::any_of(expected.begin(), expected.end(), [](const auto &entry) { return !entry.second.empty(); }));
			records.clear(); for (const auto &s : next) records.push_back(&s);
			current.Begin(records, PAX, 30, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, replacement_algorithm != 0, replacement_algorithm >= 2, replacement_algorithm == 3);
			[&]<typename T>(T &old) { if constexpr (requires { old.PrepareRetirement(current); }) old.PrepareRetirement(current); }(retained);
			std::set<StationID> released;
			while (!current.Finished()) {
				CHECK(current.Step(quantum, false, [&](StationID destination) {
					CHECK((released.empty() || *released.rbegin() < destination));
					retained.ReleaseDestination(destination); released.insert(destination);
					for (auto &[origin, destinations] : expected) std::erase(destinations, destination);
				}) <= quantum);
				for (StationID origin : origins) {
					auto actual = DestinationList(retained.ReachableDestinations(origin));
					CHECK(actual == expected[origin]);
					/* Match both CargoPacket scan strategies, including calls for a
					 * reachable destination without a waiting fallback cohort. */
					for (size_t fallback_count : {size_t{0}, size_t{1}, origins.size()}) {
						std::vector<StationID> scalar_calls, span_calls;
						const auto &destinations = expected[origin];
						if (fallback_count < destinations.size()) {
							for (size_t i = 0; i < fallback_count; ++i) if (std::binary_search(destinations.begin(), destinations.end(), origins[i])) scalar_calls.push_back(origins[i]);
						} else scalar_calls = destinations;
						auto spans = retained.ReachableDestinations(origin);
						if (fallback_count < spans[0].size() + spans[1].size()) {
							for (size_t i = 0; i < fallback_count; ++i) if (std::binary_search(spans[0].begin(), spans[0].end(), origins[i]) || std::binary_search(spans[1].begin(), spans[1].end(), origins[i])) span_calls.push_back(origins[i]);
						} else for (auto span : spans) span_calls.insert(span_calls.end(), span.begin(), span.end());
						CHECK(span_calls == scalar_calls);
					}
					auto fresh = DestinationList(current.ReachableDestinations(origin));
					for (StationID destination : origins) {
						CHECK_FALSE((std::binary_search(actual.begin(), actual.end(), destination) && std::binary_search(fresh.begin(), fresh.end(), destination)));
						CHECK(retained.RouteCost(origin, destination) == (released.contains(destination) ? std::nullopt : costs[{origin, destination}]));
					}
				}
			}
			if (work == 0) work = current.GetDiagnostic().operations;
			CHECK(current.GetDiagnostic().operations == work);
		}
	}
}

TEST_CASE("Retained destinations survive repeated replacement epochs without resurrection", "[passenger][precomputed][memory][save]")
{
	const std::array origins{A, B, C, D, StationID{4}, StationID{7}, StationID{8}, StationID{9}, StationID{10}};
	std::array previous{Service(0, {A, B, C, D, StationID{7}, StationID{9}, StationID{10}})};
	const std::array generations{
		Service(0, {B, D, StationID{4}, StationID{8}, StationID{10}}),
		Service(0, {A, StationID{7}, StationID{10}}),
		Service(0, {C, StationID{9}}),
		Service(0, {A, B, C, D})};
	for (unsigned algorithm : {0U, 1U, 2U, 3U}) for (bool hyperpath_replacement : {false, true}) for (size_t quantum : {1U, 4U, 5U}) {
		CAPTURE(algorithm, hyperpath_replacement, quantum);
		PassengerPrecomputedTable retained;
		std::vector<const PassengerPeriodicService *> records{&previous[0]};
		retained.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, algorithm != 0, algorithm >= 2, algorithm == 3);
		while (!retained.Finished()) retained.Step(65536);
		std::map<StationID, std::vector<StationID>> expected;
		std::map<std::pair<StationID, StationID>, std::optional<int64_t>> costs;
		for (StationID origin : origins) {
			expected[origin] = DestinationList(retained.ReachableDestinations(origin));
			for (StationID destination : origins) costs[{origin, destination}] = retained.RouteCost(origin, destination);
		}
		std::set<StationID> retired;
		for (const auto &generation : generations) {
			retained.ReuseRetainedDestinations();
			for (StationID origin : origins) CHECK(DestinationList(retained.ReachableDestinations(origin)) == expected[origin]);
			records = {&generation};
			PassengerPrecomputedTable current; current.Begin(records, PAX, 30, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, hyperpath_replacement);
			retained.PrepareRetirement(current);
			while (!current.Finished()) {
				CHECK(current.Step(quantum, false, [&](StationID destination) {
					retained.ReleaseDestination(destination); retired.insert(destination);
					for (auto &[origin, destinations] : expected) std::erase(destinations, destination);
				}) <= quantum);
				for (StationID origin : origins) {
					auto actual = DestinationList(retained.ReachableDestinations(origin));
					CHECK(actual == expected[origin]); CHECK(std::is_sorted(actual.begin(), actual.end()));
					auto fresh = DestinationList(current.ReachableDestinations(origin));
					for (StationID destination : origins) {
						CHECK_FALSE((std::binary_search(actual.begin(), actual.end(), destination) && std::binary_search(fresh.begin(), fresh.end(), destination)));
						CHECK(retained.RouteCost(origin, destination) == (retired.contains(destination) ? std::nullopt : costs[{origin, destination}]));
					}
				}
			}
		}
		retained.ReuseRetainedDestinations();
		for (StationID origin : origins) CHECK(DestinationList(retained.ReachableDestinations(origin)).empty());
	}
}

TEST_CASE("Precomputed inverse destinations contain exactly the published finite rows", "[passenger][precomputed][boarding][save]")
{
	std::array services{Service(0, {A, B}), Service(1, {B, D}), Service(2, {C})};
	std::vector<const PassengerPeriodicService *> records; for (const auto &service : services) records.push_back(&service);
	PassengerPrecomputedTable table; table.Begin(records, PAX, 20);
	while (!table.Finished()) {
		table.Step(7);
		for (StationID origin : {A, B, C, D}) {
			auto destinations = DestinationList(table.ReachableDestinations(origin)); REQUIRE(std::is_sorted(destinations.begin(), destinations.end()));
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

TEST_CASE("Flat Hyperpath policies preserve every published candidate label and N/F across slices", "[passenger][hyperpath][flat][determinism]")
{
	uint32_t seed = 0x53d28b17;
	for (size_t trial = 0; trial < 16; ++trial) {
		CAPTURE(trial);
		std::vector<PassengerPeriodicService> services;
		for (uint16_t id = 0; id < 12; ++id) {
			seed = seed * 1664525U + 1013904223U;
			auto service = id % 3 == 0 ? Service(id, {A, B, A, D}, 20 + seed % 200) : id % 3 == 1 ? Service(id, {A, C, D}, 20 + seed % 200) : Service(id, {B, D, C, A}, 20 + seed % 200);
			if ((seed & 3) == 0) { service.timetable.estimated_period = service.timetable.period; service.timetable.period = 0; }
			if ((seed & 7) == 0) service.timetable.stops[1].board = 0;
			services.push_back(std::move(service));
		}
		std::vector<const PassengerPeriodicService *> records; for (const auto &service : services) records.push_back(&service);
		PassengerPrecomputedTable old, flat;
		old.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, true, true, true, false);
		flat.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, true, true, true, true);
		std::map<StationID, std::vector<uint64_t>> expected;
		while (!old.Finished()) old.Step(1 + trial, false, [&](StationID destination) { expected.emplace(destination, old.InspectBuildingPolicyRow(destination)); });
		while (!flat.Finished()) flat.Step(73 - trial, false, [&](StationID destination) {
			REQUIRE(expected.contains(destination));
			CHECK(flat.InspectBuildingPolicyRow(destination) == expected.at(destination));
		});
		REQUIRE_FALSE(old.MemoryRejected()); REQUIRE_FALSE(flat.MemoryRejected()); REQUIRE(expected.size() == 4);
		CHECK(flat.GetDiagnostic().operations == old.GetDiagnostic().operations);
		CHECK(flat.GetDiagnostic().candidate_histogram == old.GetDiagnostic().candidate_histogram);
		CHECK(flat.GetDiagnostic().candidate_count == old.GetDiagnostic().candidate_count);
		CHECK(flat.GetDiagnostic().envelope < old.GetDiagnostic().envelope);
		for (auto from : {A, B, C, D}) for (auto to : {A, B, C, D}) for (uint16_t id = 0; id < services.size(); ++id) {
			auto previous = OrderListID{id};
			for (auto failed : {OrderListID::Invalid(), previous}) {
				auto before = old.Candidates(from, to, previous, failed), after = flat.Candidates(from, to, previous, failed);
				CHECK(std::vector<uint32_t>(before.begin(), before.end()) == std::vector<uint32_t>(after.begin(), after.end()));
				CHECK(flat.Plan(from, to, StateTicks{42}, previous, 0, failed).ride == old.Plan(from, to, StateTicks{42}, previous, 0, failed).ride);
				for (const auto &stop : services[id].timetable.stops) if (stop.station == from) {
					auto occurrence = flat.BoardingOccurrence(from, previous, stop.order);
					CHECK(flat.ContainsCached(from, to, occurrence, OrderListID::Invalid(), failed) == old.ContainsCached(from, to, occurrence, OrderListID::Invalid(), failed));
					CHECK(flat.Board(from, to, StateTicks{42}, previous, stop.order, OrderListID::Invalid(), failed).ride == old.Board(from, to, StateTicks{42}, previous, stop.order, OrderListID::Invalid(), failed).ride);
				}
			}
		}
		auto before = old.InspectMemory(), after = flat.InspectMemory();
		CHECK(after.labels == before.labels); CHECK(after.policy_headers <= before.policy_headers);
		CHECK(after.unexpected_removals == before.unexpected_removals);
		CHECK(after.maximum_additions == before.maximum_additions);
		CHECK(after.labels + after.indices + after.policy_headers + after.candidate_arrays + after.graph + after.local <= after.reported);
	}
}

TEST_CASE("Flat Hyperpath differences retain every addition after excluding a fast carrier", "[passenger][hyperpath][flat]")
{
	std::vector<PassengerPeriodicService> services{Service(0, {A, D}, 1)};
	for (uint16_t id = 1; id <= 11; ++id) services.push_back(Service(id, {A, D}, 100));
	std::vector<const PassengerPeriodicService *> records; for (const auto &service : services) records.push_back(&service);
	PassengerPrecomputedTable old, flat;
	old.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, true, true, true, false);
	flat.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, true, true, true, true);
	while (!old.Finished()) old.Step(7); while (!flat.Finished()) flat.Step(3);
	REQUIRE(flat.Candidates(A, D).size() == 1);
	auto before = old.Candidates(A, D, OrderListID{0}), after = flat.Candidates(A, D, OrderListID{0});
	REQUIRE(before.size() == 11); CHECK(std::vector<uint32_t>(before.begin(), before.end()) == std::vector<uint32_t>(after.begin(), after.end()));
	CHECK(flat.InspectMemory().maximum_additions == 11); CHECK(flat.InspectMemory().multi_addition_differences > 0);
	for (uint16_t id = 1; id <= 11; ++id) {
		auto a = old.Board(A, D, StateTicks{}, OrderListID{id}, 0, OrderListID{0}), b = flat.Board(A, D, StateTicks{}, OrderListID{id}, 0, OrderListID{0});
		REQUIRE(a.ride); CHECK(a.ride == b.ride);
	}
}

TEST_CASE("Hyperpath flat real map exact publication witness", "[.hyperpath-flat-real][passenger]")
{
	const char *path = getenv("OPENTTD_TABLE_DATABASE"), *coordinates = getenv("OPENTTD_TABLE_STATIONS"), *output = getenv("OPENTTD_TABLE_WITNESS"), *storage = getenv("OPENTTD_TABLE_FLAT");
	REQUIRE(path != nullptr); REQUIRE(coordinates != nullptr); REQUIRE(output != nullptr); REQUIRE(storage != nullptr);
	std::ifstream file(path, std::ios::binary); REQUIRE(file.good());
	std::vector<uint8_t> wire{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	PassengerServiceDatabase database; REQUIRE(database.Load(wire)); PassengerTestStations fixture;
	std::ifstream positions(coordinates); uint32_t width, height, id, x, y; REQUIRE(bool(positions >> width >> height)); AllocateMap(width, height);
	while (positions >> id >> x >> y) Station::CreateAtIndex(StationID{static_cast<uint16_t>(id)}, TileIndex{width * y + x});
	std::vector<const PassengerPeriodicService *> records;
	for (uint32_t id = 0; id < OrderListID::Invalid().base(); ++id) if (auto *service = database.Get(OrderListID{static_cast<uint16_t>(id)})) records.push_back(service);
	bool flat = std::string_view(storage) == "1"; REQUIRE((flat || std::string_view(storage) == "0"));
	std::ofstream witness(output, std::ios::binary); REQUIRE(witness.good());
	PassengerPrecomputedTable table; auto started = std::chrono::steady_clock::now(); uint64_t rows = 0, words = 0;
	const char *replay = getenv("OPENTTD_TABLE_REPLAY_WORK"); uint64_t remaining = replay == nullptr ? 0 : std::strtoull(replay, nullptr, 10);
	table.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, true, true, true, flat);
	while (!table.Finished()) {
		REQUIRE((replay == nullptr || remaining != 0));
		size_t used = table.Step(replay == nullptr ? 16 * 1024 * 1024 : std::min<uint64_t>(remaining, 104729), false, [&](StationID destination) {
		auto row = table.InspectBuildingPolicyRow(destination); REQUIRE_FALSE(row.empty());
		uint64_t size = row.size(); witness.write(reinterpret_cast<const char *>(&size), sizeof(size));
		witness.write(reinterpret_cast<const char *>(row.data()), row.size() * sizeof(uint64_t)); REQUIRE(witness.good());
		++rows; words += size + 1;
		});
		if (replay != nullptr) { REQUIRE(used != 0); remaining -= used; }
	}
	REQUIRE_FALSE(table.MemoryRejected()); REQUIRE((replay == nullptr || remaining == 0)); witness.close();
	const auto &d = table.GetDiagnostic(); auto m = table.InspectMemory();
	std::cout << "flat_build," << flat << ',' << rows << ',' << words << ',' << d.operations << ',' << d.envelope << ',' << d.bytes << ',' << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() << std::endl;
	std::cout << "flat_memory," << m.reported << ',' << m.graph << ',' << m.labels << ',' << m.indices << ',' << m.policy_headers << ',' << m.candidate_arrays << ',' << m.local << ',' << m.differences << ',' << m.additions << ',' << m.maximum_additions << ',' << m.multi_addition_differences << ',' << m.unexpected_removals << std::endl;
	for (auto [origin, destination] : {std::pair{StationID{971}, StationID{916}}, std::pair{StationID{3222}, StationID{400}}, std::pair{StationID{3393}, StationID{3394}}}) {
		auto candidates = table.Candidates(origin, destination); auto plan = table.Plan(origin, destination, StateTicks{});
		std::cout << "flat_target," << origin.base() << ',' << destination.base() << ',' << table.RouteCost(origin, destination).value_or(INT64_MAX) << ',' << (plan.ride ? plan.ride->estimated_wait : -1) << ',' << candidates.size() << std::endl;
		for (const auto *service : records) for (const auto &stop : service->timetable.stops) if (stop.station == origin) {
			auto occurrence = table.BoardingOccurrence(origin, service->id, stop.order);
			if (std::binary_search(candidates.begin(), candidates.end(), occurrence)) std::cout << "flat_target_candidate," << origin.base() << ',' << destination.base() << ',' << service->id.base() << ',' << stop.order << ',' << occurrence << std::endl;
		}
	}
}

namespace {
	/** Random-walk lines over a square station grid; out-and-back or circular, 1-6 vehicles. */
	std::vector<PassengerPeriodicService> SyntheticNetwork(uint16_t side, uint16_t lines, uint32_t seed)
	{
		auto rnd = [&](uint32_t n) { seed = seed * 1664525U + 1013904223U; return (seed >> 8) % n; };
		std::vector<PassengerPeriodicService> services;
		for (uint16_t line = 0; line < lines; ++line) {
			PassengerPeriodicService s; s.id = OrderListID{line}; s.incarnation = line + 1; s.generation = line + 10;
			int x = static_cast<int>(rnd(side)), y = static_cast<int>(rnd(side));
			const uint32_t length = 6 + rnd(18);
			std::vector<StationID> stops;
			for (uint32_t i = 0; i < length; ++i) {
				const StationID station{static_cast<uint16_t>(y * side + x)};
				if (std::find(stops.begin(), stops.end(), station) == stops.end()) stops.push_back(station);
				switch (rnd(4)) {
					case 0: x = std::min<int>(side - 1, x + 1); break;
					case 1: x = std::max(0, x - 1); break;
					case 2: y = std::min<int>(side - 1, y + 1); break;
					default: y = std::max(0, y - 1); break;
				}
			}
			if (stops.size() < 2) continue;
			if (rnd(3) != 0) for (size_t i = stops.size() - 1; i-- > 1;) stops.push_back(stops[i]); // Out and back.
			int64_t cursor = 0;
			for (StationID station : stops) {
				const int64_t dwell = 5 + rnd(20);
				s.timetable.stops.push_back({station, static_cast<VehicleOrderID>(s.timetable.stops.size()), cursor, cursor + dwell, 1, 1, 0});
				cursor += dwell + 40 + rnd(200);
			}
			s.timetable.period = cursor;
			const uint32_t vehicles = 1 + rnd(6);
			for (uint32_t i = 0; i < vehicles; ++i) {
				PassengerPeriodicVehicle v; v.id = VehicleID{static_cast<uint32_t>(line * 8 + i)}; v.type = VehicleType::Train; v.phase_known = true;
				v.base = StateTicks{static_cast<int64_t>(cursor * i / vehicles)}; v.capacity[PAX] = 100;
				s.vehicles.push_back(v);
			}
			services.push_back(std::move(s));
		}
		return services;
	}
}

TEST_CASE("Hyperpath table build CPU on a national-scale synthetic network", "[.hyperpath-build-cpu][passenger]")
{
	/* Sized like the real map (about 800 patterns and 4,000-5,000 stations). Reports nanoseconds
	 * per Work unit over a fixed slice, and a digest of every row of a small complete table so a
	 * CPU-only change can show that it publishes exactly the same strategies. */
	if (const char *skip = getenv("OPENTTD_HYPERPATH_BUILD_DIGEST"); skip == nullptr || std::string_view{skip} != "0") for (bool hyperpath : {true, false}) {
		const auto services = SyntheticNetwork(24, 160, 0x0d16e57U);
		std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
		PassengerPrecomputedTable table;
		table.Begin(records, PAX, 20, hyperpath ? PassengerPrecomputedTable::MAX_HYPERPATH_BYTES : PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, hyperpath);
		const auto started = std::chrono::steady_clock::now();
		while (!table.Finished()) table.Step(65536);
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		REQUIRE_FALSE(table.MemoryRejected());
		uint64_t digest = 0xcbf29ce484222325ULL;
		auto mix = [&](uint64_t value) { digest = (digest ^ value) * 0x100000001b3ULL; };
		std::vector<uint32_t> stored;
		const auto queried = std::chrono::steady_clock::now();
		for (uint16_t origin = 0; origin < 24 * 24; ++origin) for (uint16_t destination = 0; destination < 24 * 24; ++destination) {
			const StationID from{origin}, to{destination};
			mix(static_cast<uint64_t>(table.RouteCost(from, to).value_or(-1)));
			const auto plan = table.Plan(from, to, StateTicks{});
			mix(static_cast<uint64_t>(plan.state)); mix(static_cast<uint64_t>(plan.cost)); mix(static_cast<uint64_t>(plan.riding)); mix(static_cast<uint64_t>(plan.waiting));
			mix(plan.distance); mix(plan.transfers); mix(plan.ride ? static_cast<uint64_t>(plan.ride->estimated_wait) : 0);
			OrderListID earlier = OrderListID::Invalid();
			for (const auto &service : services) {
				if (std::none_of(service.timetable.stops.begin(), service.timetable.stops.end(), [&](const auto &stop) { return stop.station == from; })) continue;
				stored.clear(); table.StoredCandidates(from, to, service.id, stored);
				for (uint32_t occurrence : stored) mix(occurrence);
				mix(UINT64_MAX);
				if (!hyperpath || (origin + destination) % 3 != 0) continue; // Local (failed-carrier) policies, sampled.
				for (uint32_t occurrence : table.Candidates(from, to, OrderListID::Invalid(), service.id)) mix(occurrence);
				if (earlier != OrderListID::Invalid()) for (uint32_t occurrence : table.Candidates(from, to, earlier, service.id)) mix(occurrence);
				const auto local = table.Plan(from, to, StateTicks{}, earlier, 0, service.id);
				mix(static_cast<uint64_t>(local.state)); mix(static_cast<uint64_t>(local.cost)); mix(local.ride ? static_cast<uint64_t>(local.ride->estimated_wait) : 0);
				earlier = service.id;
			}
			for (const auto &candidate : table.Inspect(from, to)) { mix(candidate.occurrence); mix(static_cast<uint64_t>(candidate.cost)); mix(static_cast<uint64_t>(candidate.wait)); }
		}
		std::cout << (hyperpath ? "hyperpath" : "legacy") << "_build_digest,destinations=" << table.GetDiagnostic().destinations << ",operations=" << table.GetDiagnostic().operations
				<< ",ms=" << ms << ",query_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - queried).count()
				<< ",digest=" << std::hex << digest << std::dec << std::endl;
	}
	const char *env_ops = getenv("OPENTTD_HYPERPATH_BUILD_OPS");
	const uint64_t ops = env_ops != nullptr ? std::strtoull(env_ops, nullptr, 10) : uint64_t{64} * 65536;
	const auto services = SyntheticNetwork(70, 800, 0x5eed1234U);
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	PassengerPrecomputedTable table;
	table.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, true);
	while (table.IsPreparingGraph() && !table.Finished()) table.Step(65536);
	REQUIRE_FALSE(table.MemoryRejected());
	const auto started = std::chrono::steady_clock::now();
	const uint64_t before = table.GetDiagnostic().operations;
	while (!table.Finished() && table.GetDiagnostic().operations - before < ops) table.Step(65536);
	const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
	const auto &d = table.GetDiagnostic();
	std::cout << "hyperpath_build_cpu,patterns=" << d.patterns << ",stations=" << d.stations << ",states=" << d.states << ",edges=" << d.logical_edges
			<< ",destinations=" << d.destinations << ",operations=" << d.operations - before << ",ns_per_op=" << double(ns) / double(d.operations - before)
			<< ",board_events=" << d.board_events << ",board_accepts=" << d.board_accepts << ",range_updates=" << d.range_updates
			<< ",heap_pushes=" << d.heap_pushes << ",heap_pops=" << d.heap_pops << ",bytes=" << d.bytes << std::endl;
}

namespace {
	std::vector<PassengerPeriodicService> PassengerBenchmarkNetwork(uint16_t side, uint16_t lines, bool uneven)
	{
		auto services = SyntheticNetwork(side, lines, 0x0d16e57U);
		if (uneven) for (uint16_t i = 0; i < 32; ++i) {
			const uint16_t station = static_cast<uint16_t>(side * side + i * 2);
			auto service = Service(static_cast<uint16_t>(lines + i), {StationID{station}, StationID{static_cast<uint16_t>(station + 1)}}, 37);
			service.vehicles.front().id = VehicleID{static_cast<uint32_t>((lines + i) * 8)};
			services.push_back(std::move(service));
		}
		return services;
	}
	struct PassengerBenchmarkWorkers {
		std::mutex mutex;
		std::vector<PassengerBenchmarkThread> threads;
		PassengerBenchmarkWorkers() { this->threads.reserve(8); }
		~PassengerBenchmarkWorkers() { for (auto thread : this->threads) PassengerBenchmarkReleaseThread(thread); }
	};
	PassengerBenchmarkWorkers &PassengerBenchmarkWorkerThreads() { static PassengerBenchmarkWorkers workers; return workers; }
	void PassengerBenchmarkWorkerCPU(size_t, bool done)
	{
		if (done) return;
		thread_local bool registered = false;
		if (registered) return;
		auto thread = PassengerBenchmarkCaptureThread();
		auto &workers = PassengerBenchmarkWorkerThreads(); std::lock_guard lock(workers.mutex);
		workers.threads.push_back(thread); registered = true;
	}
	uint64_t PassengerBenchmarkWorkersCPU()
	{
		auto &workers = PassengerBenchmarkWorkerThreads(); std::lock_guard lock(workers.mutex);
		uint64_t total = 0; for (auto thread : workers.threads) total += PassengerBenchmarkCPU(thread); return total;
	}
	unsigned PassengerBenchmarkNumber(const char *name, unsigned fallback)
	{
		const char *value = getenv(name); return value ? static_cast<unsigned>(std::strtoul(value, nullptr, 10)) : fallback;
	}
}

TEST_CASE("Passenger parallel complete construction benchmark", "[.passenger-parallel-benchmark][passenger]")
{
	auto number = [](const char *name, unsigned fallback) { const char *value = getenv(name); return value ? static_cast<unsigned>(std::strtoul(value, nullptr, 10)) : fallback; };
	const uint workers = number("OPENTTD_PARALLEL_WORKERS", 4);
	const bool hyperpath = number("OPENTTD_PARALLEL_HYPERPATH", 1) != 0;
	const auto services = PassengerBenchmarkNetwork(static_cast<uint16_t>(number("OPENTTD_PARALLEL_SIDE", 32)), static_cast<uint16_t>(number("OPENTTD_PARALLEL_LINES", 240)), number("OPENTTD_PARALLEL_UNEVEN", 0) != 0);
	std::vector<const PassengerPeriodicService *> records; for (const auto &s : services) records.push_back(&s);
	PassengerPrecomputedTable table;
	const uint64_t cpu_started = PassengerBenchmarkThreadCPU();
	const auto started = std::chrono::steady_clock::now();
	table.Begin(records, PAX, 20, PassengerPrecomputedTable::MAX_HYPERPATH_BYTES, true, false, {}, true, true, true, hyperpath);
	const auto graph_done = std::chrono::steady_clock::now(); const uint64_t cpu_graph = PassengerBenchmarkThreadCPU();
	const uint64_t worker_cpu_before = PassengerBenchmarkWorkersCPU(); PassengerPrecomputedTestAccess::Hook(table, PassengerBenchmarkWorkerCPU);
	uint64_t progress = 0xcbf29ce484222325ULL;
	while (!table.Finished()) {
		table.StepParallel(65536, workers);
		progress = (progress ^ table.GetDiagnostic().operations) * 0x100000001b3ULL;
		progress = (progress ^ table.GetDiagnostic().destinations) * 0x100000001b3ULL;
	}
	const auto finished = std::chrono::steady_clock::now();
	const uint64_t cpu_finished = PassengerBenchmarkThreadCPU();
	WaitForPassengerWorkers([&]() { return PassengerPrecomputedTestAccess::Idle(table); });
	/* Let the cancelled idle contexts park before reading their OS CPU totals.
	 * This settling interval is outside every reported build/tick wall window. */
	std::this_thread::sleep_for(std::chrono::milliseconds(25));
	const uint64_t worker_cpu_finished = PassengerBenchmarkWorkersCPU();
	REQUIRE_FALSE(table.MemoryRejected());
	const auto &d = table.GetDiagnostic(); const auto &p = table.GetParallelDiagnostic();
	std::cout << "passenger_parallel,mode=" << (hyperpath ? "hyperpath" : "legacy") << ",requested=" << workers << ",workers=" << p.workers
			<< ",progress_digest=" << progress << ",main_cpu_ns=" << cpu_finished - cpu_started << ",row_main_cpu_ns=" << cpu_finished - cpu_graph
			<< ",cache_hits=" << p.cache_hits << ",cache_work=" << p.cache_work << ",serial_rows=" << p.serial_rows << ",main_search_ns=" << p.main_search_nanoseconds
			<< ",serial_work=" << p.serial_search_work << ",cancelled_work=" << p.cancelled_search_work << ",thread_starts=" << p.thread_starts << ",workspace_bound=" << p.estimated_workspace_bytes << ",worker_wait_ns=0"
			<< ",wall_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count()
			<< ",graph_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(graph_done - started).count()
			<< ",row_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(finished - graph_done).count()
			<< ",batch_ns=" << p.batch_nanoseconds << ",search_ns=" << p.search_nanoseconds << ",integration_ns=" << p.integration_nanoseconds
			<< ",batches=" << p.batches << ",searches=" << p.searches << ",pending=" << p.peak_pending << ",work_min=" << p.minimum_work << ",work_max=" << p.maximum_work
			<< ",throttles=" << p.throttles << ",worker_cpu_ns=" << (worker_cpu_finished - worker_cpu_before) << ",workspace_reuses=" << p.workspace_reuses << ",duplicate_work=" << (p.search_work - p.cache_work)
			<< ",search_work=" << p.search_work << ",stations=" << d.stations << ",states=" << d.states << ",edges=" << d.logical_edges << ",operations=" << d.operations
			<< ",bytes=" << d.bytes << ",digest=" << PassengerPrecomputedTestAccess::Digest(table) << std::endl;
}

TEST_CASE("Passenger asynchronous replacement TickPrecomputed benchmark", "[.passenger-async-tick-benchmark][passenger]")
{
	PassengerTestStations fixture;
	const uint workers = PassengerBenchmarkNumber("OPENTTD_PARALLEL_WORKERS", 4), scenario = PassengerBenchmarkNumber("OPENTTD_ASYNC_SCENARIO", 0);
	const bool hyperpath = PassengerBenchmarkNumber("OPENTTD_PARALLEL_HYPERPATH", 1) != 0;
	const uint16_t side = static_cast<uint16_t>(PassengerBenchmarkNumber("OPENTTD_PARALLEL_SIDE", 32));
	const auto services = PassengerBenchmarkNetwork(side, static_cast<uint16_t>(PassengerBenchmarkNumber("OPENTTD_PARALLEL_LINES", 240)), PassengerBenchmarkNumber("OPENTTD_PARALLEL_UNEVEN", 0) != 0);
	for (const auto &service : services) for (const auto &stop : service.timetable.stops) if (!Station::IsValidID(stop.station)) {
		auto *station = Station::CreateAtIndex(stop.station, TileIndex{static_cast<uint32_t>((stop.station.base() / side) * 256 + stop.station.base() % side)});
		station->owner = OWNER_NONE; station->facilities.Set(StationFacility::Train);
	}
	AutoRestoreBackup settings{_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup worker_setting{_settings_client.gui.passenger_precomputed_workers, uint8_t{0}};
	_settings_game.linkgraph.passenger_journey_precomputed = true; _settings_game.linkgraph.passenger_journey_hyperpath = hyperpath;
	_settings_game.linkgraph.passenger_transfer_penalty = 20;
	auto storage = std::make_unique<PassengerServiceDatabase>(); auto &db = *storage;
	PassengerServiceDatabaseTestAccess::Seed(db, services);
	while (!db.PrecomputedFinished()) db.TickPrecomputed();
	/* Isolate the ordinary destination-row tick path from graph preparation;
	 * report the synchronous generation-start cost separately, including its CPU. */
	const auto graph_started = std::chrono::steady_clock::now(); const uint64_t graph_cpu = PassengerBenchmarkThreadCPU();
	PassengerServiceDatabaseTestAccess::BeginReplacement(db, 17); _settings_game.linkgraph.passenger_transfer_penalty = 17;
	const uint64_t graph_cpu_ns = PassengerBenchmarkThreadCPU() - graph_cpu;
	const auto graph_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - graph_started).count();
	auto &table = PassengerServiceDatabaseTestAccess::Table(db, PAX);
	const uint64_t worker_cpu_before = PassengerBenchmarkWorkersCPU(); PassengerPrecomputedTestAccess::Hook(table, PassengerBenchmarkWorkerCPU);
	static std::atomic<bool> release{true}; release.store(scenario != 2);
	struct Release { ~Release() { release.store(true); } } guard;
	if (scenario == 2) PassengerPrecomputedTestAccess::QuantumHook(table, +[](size_t, uint64_t) { while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
	_settings_client.gui.passenger_precomputed_workers = static_cast<uint8_t>(workers);
	uint64_t tick_ns = 0, progress = 0xcbf29ce484222325ULL; std::vector<uint64_t> samples;
	std::this_thread::sleep_for(std::chrono::milliseconds(25));
	const uint64_t main_cpu_started = PassengerBenchmarkThreadCPU();
	const auto started = std::chrono::steady_clock::now();
	while (!db.PrecomputedFinished()) {
		if (scenario == 1 && workers != 0 && PassengerPrecomputedTestAccess::Prime(table, workers)) WaitForPassengerWorkers([&]() { return PassengerPrecomputedTestAccess::Ready(table); });
		const auto tick_started = std::chrono::steady_clock::now();
		db.TickPrecomputed();
		const uint64_t duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tick_started).count();
		tick_ns += duration; samples.push_back(duration);
		progress = (progress ^ table.GetDiagnostic().operations) * 0x100000001b3ULL;
		progress = (progress ^ table.GetDiagnostic().destinations) * 0x100000001b3ULL;
		progress = (progress ^ PassengerServiceDatabaseTestAccess::Retained(db, PAX).GetDiagnostic().destinations) * 0x100000001b3ULL;
	}
	const auto finished = std::chrono::steady_clock::now();
	release.store(true); WaitForPassengerWorkers([&]() { return PassengerPrecomputedTestAccess::Idle(table); });
	/* Let the cancelled idle contexts park before reading their OS CPU totals.
	 * This settling interval is outside every reported build/tick wall window. */
	std::this_thread::sleep_for(std::chrono::milliseconds(25));
	const uint64_t worker_cpu_finished = PassengerBenchmarkWorkersCPU();
	const uint64_t main_cpu_finished = PassengerBenchmarkThreadCPU();
	REQUIRE_FALSE(table.MemoryRejected()); std::sort(samples.begin(), samples.end());
	const auto &d = table.GetDiagnostic(); const auto p = table.GetParallelDiagnostic();
	uint64_t database = 0xcbf29ce484222325ULL; for (uint8_t value : db.Save()) database = (database ^ value) * 0x100000001b3ULL;
	std::cout << "passenger_parallel,mode=" << (hyperpath ? "hyperpath" : "legacy") << ",requested=" << workers << ",workers=" << p.workers << ",scenario=" << scenario
			<< ",wall_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count() << ",graph_ns=" << graph_ns << ",graph_cpu_ns=" << graph_cpu_ns
			<< ",progress_digest=" << progress << ",tick_ns=" << tick_ns << ",main_cpu_ns=" << main_cpu_finished - main_cpu_started << ",ticks=" << samples.size() << ",tick_p50_ns=" << samples[samples.size() / 2]
			<< ",tick_p95_ns=" << samples[samples.size() * 95 / 100] << ",tick_max_ns=" << samples.back() << ",worker_wait_ns=0"
			<< ",cache_hits=" << p.cache_hits << ",cache_work=" << p.cache_work << ",serial_rows=" << p.serial_rows << ",main_search_ns=" << p.main_search_nanoseconds
			<< ",serial_work=" << p.serial_search_work << ",cancelled_work=" << p.cancelled_search_work << ",thread_starts=" << p.thread_starts << ",workspace_bound=" << p.estimated_workspace_bytes
			<< ",search_ns=" << p.search_nanoseconds << ",searches=" << p.searches << ",pending=" << p.peak_pending << ",work_min=" << p.minimum_work << ",work_max=" << p.maximum_work
			<< ",throttles=" << p.throttles << ",worker_cpu_ns=" << (worker_cpu_finished - worker_cpu_before) << ",workspace_reuses=" << p.workspace_reuses << ",duplicate_work=" << (p.search_work - p.cache_work)
			<< ",search_work=" << p.search_work << ",stations=" << d.stations << ",states=" << d.states << ",edges=" << d.logical_edges << ",operations=" << d.operations
			<< ",bytes=" << d.bytes << ",database_digest=" << database << ",digest=" << PassengerPrecomputedTestAccess::Digest(table) << std::endl;
}

TEST_CASE("Optimized destination queue pops exactly the binary heap order", "[passenger][precomputed][queue]")
{
	/* Monotone pushes with ties, same-cost pushes after pops, short and long jumps across
	 * radix buckets: the exact (cost, kind, id) order of a binary heap. */
	using Item = std::tuple<int64_t, uint32_t, uint32_t>;
	for (uint32_t seed : {1U, 0x9e3779b9U, 0x2545f491U, 0xdeadbeefU}) {
		auto rnd = [&](uint32_t n) { seed = seed * 1664525U + 1013904223U; return (seed >> 8) % n; };
		PassengerDestinationQueue<Item> queue; queue.SetOptimized(true); queue.SetCapacityLimit(64);
		std::priority_queue<Item, std::vector<Item>, std::greater<Item>> oracle;
		for (int destination = 0; destination < 6; ++destination) {
			int64_t last = 0;
			for (int step = 0; step < 6000; ++step) {
				if (oracle.empty() || rnd(5) < 3) {
					int64_t delta;
					switch (rnd(6)) {
						case 0: delta = 0; break;
						case 1: delta = rnd(4); break;
						case 2: delta = rnd(1100); break;
						case 3: delta = 1024; break;
						case 4: delta = 1023 + rnd(3); break;
						default: delta = rnd(50000); break;
					}
					Item item{last + delta, rnd(3), rnd(64)};
					queue.emplace(std::get<0>(item), std::get<1>(item), std::get<2>(item)); oracle.push(item);
				} else {
					REQUIRE(queue.size() == oracle.size());
					REQUIRE(queue.top() == oracle.top());
					last = std::get<0>(oracle.top());
					queue.pop(); oracle.pop();
				}
			}
			while (!oracle.empty()) { REQUIRE(queue.top() == oracle.top()); queue.pop(); oracle.pop(); }
			REQUIRE(queue.empty());
			queue.Reset();
		}
	}
}

TEST_CASE("Hyperpath display enumerates frozen policy storage with bounded read-only work", "[passenger][hyperpath][station-gui]")
{
	for (bool flat : {false, true}) {
		std::array services{Service(0, {A, B}), Service(1, {A, B}), Service(2, {A, C, B})};
		std::vector<const PassengerPeriodicService *> records; for (const auto &service : services) records.push_back(&service);
		PassengerPrecomputedTable table;
		table.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, true, true, true, flat);
		while (!table.Finished()) table.Step(64);
		std::vector<PassengerPrecomputedTable::DisplayCandidate> candidates;
		size_t work = 4096;
		REQUIRE(table.DisplayCandidates(A, B, OrderListID::Invalid(), OrderListID::Invalid(), candidates, work));
		REQUIRE(candidates.size() >= 2);
		for (const auto &candidate : candidates) {
			CHECK(candidate.order == 0); CHECK(candidate.incarnation == candidate.service.base() + 1);
			CHECK(table.Board(A, B, StateTicks{}, candidate.service, candidate.order).ride.has_value());
		}
		work = 4096;
		REQUIRE(table.DisplayCandidates(A, B, OrderListID{0}, OrderListID::Invalid(), candidates, work));
		CHECK_FALSE(candidates.empty());
		for (const auto &candidate : candidates) CHECK(candidate.service != OrderListID{0});
		const auto before = table.GetDiagnostic(); work = 4096;
		CHECK_FALSE(table.DisplayCandidates(A, B, OrderListID::Invalid(), OrderListID{0}, candidates, work));
		CHECK(candidates.empty()); CHECK(table.GetDiagnostic().local_replans == before.local_replans);
		CHECK(table.GetDiagnostic().bytes == before.bytes);
		work = 1;
		CHECK_FALSE(table.DisplayCandidates(A, B, OrderListID::Invalid(), OrderListID::Invalid(), candidates, work));
		CHECK(work == 0); CHECK(candidates.empty());
	}
}

TEST_CASE("Hyperpath display cursor advances beyond old candidate limits with single-work pages", "[passenger][hyperpath][station-gui][service-progress]")
{
	std::vector<PassengerPeriodicService> services;
	for (uint16_t id = 0; id < 270; ++id) services.push_back(Service(id, {A, B}));
	std::vector<const PassengerPeriodicService *> records; for (const auto &service : services) records.push_back(&service);
	PassengerPrecomputedTable table;
	table.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, true);
	while (!table.Finished()) table.Step(65536);
	const auto diagnostic = table.GetDiagnostic();
	PassengerPrecomputedTable::DisplayCursor cursor; PassengerPrecomputedTable::DisplayCandidate candidate;
	std::set<OrderListID> found; bool complete = false;
	for (size_t page = 0; page < 2000 && !complete; ++page) {
		size_t work = 1;
		auto result = table.NextDisplayCandidate(A, B, OrderListID::Invalid(), OrderListID::Invalid(), cursor, candidate, work);
		if (result == PassengerPrecomputedTable::DisplayRead::Candidate) CHECK(found.insert(candidate.service).second);
		else REQUIRE(result != PassengerPrecomputedTable::DisplayRead::Unavailable);
		complete = result == PassengerPrecomputedTable::DisplayRead::Complete;
	}
	CHECK(complete); CHECK(found.size() == 270);
	size_t work = 4;
	REQUIRE(table.NextDisplayCandidate(B, A, OrderListID::Invalid(), OrderListID::Invalid(), cursor, candidate, work) == PassengerPrecomputedTable::DisplayRead::Candidate);
	CHECK(candidate.service == OrderListID{0}); CHECK(candidate.order == 1); // Rebinding a cursor changes its query, not just its position.

	/* Live bindings stay outside the frozen worker graph. Both display paths
	 * must report the refreshed generation rather than the snapshot's member. */
	services[0].generation += 100;
	REQUIRE(table.RefreshService(services[0]));
	cursor = {}; work = 4;
	REQUIRE(table.NextDisplayCandidate(B, A, OrderListID::Invalid(), OrderListID::Invalid(), cursor, candidate, work) == PassengerPrecomputedTable::DisplayRead::Candidate);
	CHECK(candidate.service == services[0].id); CHECK(candidate.generation == services[0].generation);
	work = 4096;
	const auto routes = table.InspectRoutes(A, B, OrderListID::Invalid(), OrderListID::Invalid(), work);
	REQUIRE_FALSE(routes.candidates.empty());
	CHECK(routes.candidates.front().ride.service == services[0].id);
	CHECK(routes.candidates.front().ride.generation == services[0].generation);

	CHECK(table.GetDiagnostic().bytes == diagnostic.bytes); CHECK(table.GetDiagnostic().local_replans == diagnostic.local_replans);
}
