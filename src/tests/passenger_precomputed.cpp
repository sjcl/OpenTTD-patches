/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */
#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../passenger_precomputed.h"
#include "../passenger_service_database.h"
#include <chrono>
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

TEST_CASE("Precomputed identical fleets aggregate without synthetic headways", "[passenger][precomputed]")
{
	std::array services{Service(0, {A, D}), Service(1, {A, D})};
	auto table = Build(services); CHECK(table.GetDiagnostic().patterns == 1);
	const auto baseline = table.Plan(A, D, StateTicks{});
	for (int i = 0; i < 10000; ++i) services[0].vehicles.push_back(services[0].vehicles.front());
	auto fleet = Build(services); CHECK(fleet.GetDiagnostic().states == table.GetDiagnostic().states);
	CHECK(fleet.GetDiagnostic().edges == table.GetDiagnostic().edges); CHECK(fleet.Plan(A, D, StateTicks{}).waiting == baseline.waiting);
	auto excluded = fleet.Plan(A, D, StateTicks{}, OrderListID{1}); REQUIRE(excluded.ride); CHECK(excluded.ride->service == OrderListID{0});
	services[0].vehicles[0].phase_known = false; fleet = Build(services);
	CHECK(fleet.Plan(A, D, StateTicks{}).waiting == services[0].timetable.period);
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

TEST_CASE("Precomputed CPU scaling benchmark", "[.benchmark][passenger][precomputed][cpu]")
{
	using Clock = std::chrono::steady_clock;
	std::cout << "axis,size,build_us,lookup_ns,p95_ns,p99_ns,bytes,envelope,states,edges,operations,hits\n";
	for (int axis = 0; axis < 4; ++axis) for (int scale : {16, 64, 256, 512}) {
		std::vector<PassengerPeriodicService> services{Service(0, {A, D})};
		if (axis == 0 || axis == 3) for (int i = 4; i < scale; i += 2) services.push_back(Service(i, {StationID{static_cast<uint16_t>(i)}, StationID{static_cast<uint16_t>(i + 1)}}));
		if (axis == 1) for (int i = 1; i < scale * 16; ++i) services[0].vehicles.push_back(services[0].vehicles.front());
		auto start = Clock::now(); auto table = Build(services);
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
