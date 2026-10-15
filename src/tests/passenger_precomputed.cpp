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
	CHECK(fleet.Plan(A, D, StateTicks{}).waiting == services[0].timetable.period);
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
		auto plan = Build(services).Plan(A, C, StateTicks{}); REQUIRE(plan.ride); CHECK(plan.ride->estimated_wait == 300);
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
	PassengerPrecomputedTable legacy; legacy.Begin(records, PAX, 20);
	for (int i = 0; i < 2000; ++i) {
		legacy.Step(1, true);
		if (i % 17 != 0 && !legacy.Finished()) continue;
		PassengerPrecomputedTable current; current.Begin(records, PAX, 20);
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
