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
	constexpr StationID A{0}, D{3};
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
}

TEST_CASE("Precomputed graph aggregates equivalent physical services", "[passenger][precomputed][graph]")
{
    std::array records{Service(0, {A, D}), Service(1, {A, D})};
    std::array<const PassengerPeriodicService *, 2> pointers{&records[0], &records[1]};
    PassengerPrecomputedTable graph; graph.Begin(pointers, PAX, 20);
    CHECK(graph.GetDiagnostic().patterns == 1);
    CHECK(graph.GetDiagnostic().stations == 2);
    CHECK(graph.GetDiagnostic().states == 8);
    CHECK(graph.GetDiagnostic().edges == 10);
}
