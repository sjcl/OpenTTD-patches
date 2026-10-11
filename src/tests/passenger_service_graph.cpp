/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file passenger_service_graph.cpp Service snapshot and shared prediction regression tests. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../passenger_service_graph.h"
#include "../vehicle_base.h"
#include "../core/random_func.hpp"
#include <deque>

static constexpr StationID A{0}, B{1}, C{2}, D{3};
static constexpr CargoType PAX{0};
static constexpr StateTicks NOW{10000};

static Order GraphStop(StationID station, TimetableTicks travel = 100, TimetableTicks wait = 10)
{
	Order order;
	order.MakeGoToStation(station);
	order.SetTravelTime(travel);
	order.SetTravelTimetabled(true);
	order.SetWaitTime(wait);
	order.SetWaitTimetabled(true);
	return order;
}

/** Real order lists/consists, without engine/GUI setup. Dispose before the temporary Vehicle destructor. */
struct GraphVehicle : Vehicle {
	explicit GraphVehicle(std::vector<Order> orders, VehicleID id = VehicleID{100}) : Vehicle(id, VehicleType::Train)
	{
		this->orders = OrderList::Create(std::move(orders), this);
		this->LoadAt(0);
		this->cargo_type = PAX;
		this->cargo_cap = 80;
		this->direction = Direction::NE;
	}

	explicit GraphVehicle(GraphVehicle &other, VehicleID id) : Vehicle(id, VehicleType::Train)
	{
		this->orders = other.orders;
		this->AddToShared(&other);
		this->LoadAt(0);
		this->cargo_type = PAX;
		this->cargo_cap = 120;
	}

	~GraphVehicle()
	{
		if (this->orders == nullptr) return;
		if (this->orders->IsShared()) {
			OrderList *list = this->orders;
			list->AddVehicle(nullptr); // Suppress the single-vehicle GUI notification, as in passenger_routing tests.
			this->RemoveFromShared();
			list->RemoveVehicle(nullptr);
		} else {
			this->orders->FreeChain();
		}
		this->orders = nullptr;
	}

	bool IsPrimaryVehicle() const override { return true; }
	void LoadAt(VehicleOrderID order)
	{
		this->cur_real_order_index = this->cur_implicit_order_index = order;
		this->cur_timetable_order_index = INVALID_VEH_ORDER_ID;
		this->last_station_visited = this->GetOrder(order)->GetDestination().ToStationID();
		this->current_order.MakeLoading(false);
	}
};

static PassengerServiceGraphLimits GraphLimits(Ticks horizon = 1000)
{
	PassengerServiceGraphLimits limits;
	limits.horizon = horizon;
	return limits;
}

struct GraphCoach : Vehicle {
	explicit GraphCoach(VehicleID id) : Vehicle(id, VehicleType::Train) {}
	bool IsPrimaryVehicle() const override { return false; }
};

static const PassengerServiceGraph::Trip &GraphTrip(const PassengerServiceGraph &graph, VehicleID id)
{
	auto trips = graph.GetTrips();
	auto it = std::find_if(trips.begin(), trips.end(), [&](const auto &trip) { return trip.vehicle == id; });
	REQUIRE(it != trips.end());
	return *it;
}

TEST_CASE("Graph discovery excludes definite freight before timetable capture", "[passenger][service_graph][performance]")
{
	std::deque<GraphVehicle> freight;
	for (uint32_t i = 0; i < 5000; ++i) { freight.emplace_back(std::vector<Order>{GraphStop(A), GraphStop(B)}, VehicleID{1000 + i}); freight.back().cargo_type = CargoType{1}; }
	GraphVehicle passenger({GraphStop(A), GraphStop(D)}, VehicleID{9000});
	auto limits = GraphLimits(); limits.work = 32768; limits.memory_bytes = 1024 * 1024;
	GameRandomSeedChecker random;
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
	REQUIRE(graph.GetStatus() == PassengerPredictionStatus::Complete);
	CHECK(graph.GetTrips().size() == 1);
	CHECK(graph.GetDiagnostic().trains == 5001);
	CHECK(graph.GetDiagnostic().cargo_capable == 1);
	CHECK(graph.GetDiagnostic().excluded_cargo == 5000);
	CHECK(graph.GetDiagnostic().predicted == 1);
	CHECK(graph.GetWork() < limits.work);
	CHECK(graph.GetMemoryBound() <= limits.memory_bytes);
	CHECK(random.Check());
}

TEST_CASE("Graph allocation allowance is independent of traversal work", "[passenger][service_graph]")
{
	std::vector<Order> orders;
	for (uint16_t i = 0; i < 400; ++i) orders.push_back(GraphStop(StationID{i}, 1, 1));
	GraphVehicle train(std::move(orders));
	auto limits = GraphLimits(1000); limits.memory_bytes = 1024 * 1024;
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
	CHECK(graph.GetStatus() == PassengerPredictionStatus::Complete);
	CHECK(graph.GetWork() > limits.memory_bytes / 1024);
	CHECK(graph.GetMemoryBound() <= limits.memory_bytes);
	CHECK(graph.GetDiagnostic().allocation_bytes < graph.GetMemoryBound());
	limits.memory_bytes = 100;
	CHECK(PassengerServiceGraph::Build(PAX, NOW, limits).GetStatus() == PassengerPredictionStatus::LimitReached);
}

TEST_CASE("Freight topology cannot expand a passenger corridor and live refits rebuild its view", "[passenger][service_graph][cache]")
{
	GraphVehicle direct({GraphStop(A), GraphStop(D)});
	GraphVehicle branch({GraphStop(A), GraphStop(B)}, VehicleID{101});
	GraphVehicle bridge({GraphStop(B), GraphStop(D)}, VehicleID{102});
	bridge.cargo_type = CargoType{1};
	const PassengerServiceScope scope{A, D};
	const auto cold = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope);
	REQUIRE(cold.GetQueryStatus(A, D) == PassengerPredictionStatus::Complete);
	CHECK(cold.GetTrips().size() == 1);
	CHECK(cold.GetDiagnostic().excluded_cargo == 1);
	CHECK(cold.GetDiagnostic().excluded_scope == 1);
	const auto warm = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope);
	CHECK(warm.GetDiagnostic().static_cache_hit);
	CHECK(warm.GetWork() == cold.GetWork());
	/* No topology hook is called: the exact live eligibility set must invalidate reuse. */
	bridge.cargo_type = PAX;
	const auto refitted = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope);
	CHECK_FALSE(refitted.GetDiagnostic().static_cache_hit);
	CHECK(refitted.GetTrips().size() == 3); // The branch is now a real alternative via B.
	CHECK(refitted.GetQueryStatus(A, D) == PassengerPredictionStatus::Complete);
}

TEST_CASE("Scoped graph cache residency cannot change capture or logical allowances", "[passenger][service_graph][cache]")
{
	GraphVehicle direct({GraphStop(A), GraphStop(D)});
	GraphVehicle branch({GraphStop(A), GraphStop(B), GraphStop(C)}, VehicleID{101});
	InvalidatePassengerServiceIndex();
	const auto cold = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
	const auto warm = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
	REQUIRE(cold.GetTrips().size() == 1);
	CHECK(cold.GetCoverage(A, D) == PassengerServiceGraph::Coverage::ScopedComplete);
	CHECK(cold.GetStatus() == PassengerPredictionStatus::Unknown);
	CHECK(cold.GetQueryStatus(B, D) == PassengerPredictionStatus::Unknown);
	CHECK(cold.GetVisits(B, NOW, NOW + 1000).status == PassengerPredictionStatus::Unknown);
	CHECK(cold.GetDepartures(A, NOW, NOW + 1000).status == PassengerPredictionStatus::Unknown);
	CHECK_FALSE(cold.GetDiagnostic().static_cache_hit);
	CHECK(warm.GetDiagnostic().static_cache_hit);
	CHECK(cold.GetWork() == warm.GetWork());
	CHECK(cold.GetMemoryBound() == warm.GetMemoryBound());
	CHECK(cold.GetDiagnostic().visits == warm.GetDiagnostic().visits);
	CHECK(cold.GetTrips().front().vehicle == warm.GetTrips().front().vehicle);
	/* Destination edits use the same invalidation hook as insert/delete/clone/free/afterload. */
	branch.GetOrder(2)->SetDestination(D); InvalidatePassengerServiceIndex();
	const auto changed = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
	CHECK_FALSE(changed.GetDiagnostic().static_cache_hit);
	CHECK(changed.GetTrips().size() == 2);
}

TEST_CASE("Implicit freight and unavailable trains cannot poison a passenger scope", "[passenger][service_graph][scope]")
{
	GraphVehicle direct({GraphStop(A), GraphStop(D)});
	GraphVehicle irrelevant({GraphStop(A), GraphStop(B)}, VehicleID{101});
	irrelevant.GetOrder(1)->MakeImplicit(B); InvalidatePassengerServiceIndex();
	SECTION("Definite freight") { irrelevant.cargo_type = CargoType{1}; }
	SECTION("Stopped passenger train") { irrelevant.vehstatus.Set(VehState::Stopped); }
	SECTION("Crashed passenger train") { irrelevant.vehstatus.Set(VehState::Crashed); }
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
	CHECK(graph.GetQueryStatus(A, D) == PassengerPredictionStatus::Complete);
	CHECK(graph.GetTrips().size() == 1);
	CHECK(graph.GetDiagnostic().predicted == 1);
}

TEST_CASE("Passenger graph separates local and express services and shared trips", "[passenger][service_graph]")
{
	GraphVehicle local({ GraphStop(A), GraphStop(B, 200), GraphStop(C, 300), GraphStop(D, 400) });
	GraphVehicle express({ GraphStop(B, 100, 40), GraphStop(D, 100) }, VehicleID{101});
	GraphVehicle second(express, VehicleID{102});
	second.lateness_counter = -200;
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
	REQUIRE(graph.GetServices().size() == 2);
	REQUIRE(graph.GetTrips().size() == 3);
	REQUIRE(graph.GetServicesAtStation(B).size() == 2);
	REQUIRE(graph.GetServicesAtStation(C).size() == 1);
	const auto *slow = graph.GetService(local.orders->index);
	const auto *fast = graph.GetService(express.orders->index);
	REQUIRE(slow != nullptr);
	REQUIRE(fast != nullptr);
	CHECK(slow->stops.size() == 4);
	CHECK(fast->stops.size() == 2);
	CHECK(slow->stops[2].station == C);
	CHECK(slow->stops[2].order == 2);
	CHECK(slow->segments[0].travel == StateTicksDelta{200});
	CHECK(fast->segments[0].travel == StateTicksDelta{100});
	CHECK(fast->trips.size() == 2);
	CHECK(fast->cycle_known);
	const auto &first_trip = GraphTrip(graph, express.index);
	const auto &second_trip = GraphTrip(graph, second.index);
	CHECK(first_trip.service == second_trip.service);
	CHECK(first_trip.capacity == 80);
	CHECK(second_trip.capacity == 120);
	CHECK(first_trip.prediction.visits[0].departure == NOW + 40);
	CHECK(second_trip.prediction.visits[0].departure == NOW + 240);
	CHECK(first_trip.prediction.visits[1].arrival == NOW + 140);
	CHECK(second_trip.prediction.visits[1].arrival == NOW + 340);
	const auto boardable = graph.GetBoardableTrips(B, NOW, NOW + 1000);
	REQUIRE_FALSE(boardable.items.empty());
	CHECK(graph.GetService(OrderListID::Invalid()) == nullptr);
	CHECK(graph.GetServicesAtStation(StationID{999}).empty());
	CHECK(graph.GetVisits(StationID{999}, NOW, NOW + 1000).status == PassengerPredictionStatus::Complete);
}

TEST_CASE("Passenger graph stop permissions and forced unloading bound through rides", "[passenger][service_graph]")
{
	auto a = GraphStop(A), b = GraphStop(B), c = GraphStop(C), d = GraphStop(D);
	b.SetNonStopType(ONSF_NO_STOP_AT_DESTINATION_STATION);
	c.SetLoadType(OrderLoadType::NoLoad);
	c.SetUnloadType(OrderUnloadType::NoUnload);
	GraphVehicle train({ a, b, c, d });
	SECTION("Pass-through and closed stops") {
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(400));
		const auto *service = graph.GetService(train.orders->index);
		REQUIRE(service->stops.size() == 3);
		CHECK(graph.GetServicesAtStation(B).empty());
		CHECK_FALSE(service->stops[1].can_board);
		CHECK_FALSE(service->stops[1].can_alight);
		CHECK(graph.GetBoardableTrips(C, NOW, NOW + 400).items.empty());
		const auto options = graph.GetBoardableTrips(A, NOW, NOW + 400);
		REQUIRE(options.items.size() == 1);
		REQUIRE(options.items[0].reachable.size() == 1);
		const auto id = options.items[0].reachable[0];
		CHECK(graph.GetTrips()[id.trip].prediction.visits[id.visit].order == 3);
	}
	SECTION("Forced unload and transfer") {
		for (auto unload : { OrderUnloadType::Unload, OrderUnloadType::Transfer }) {
			train.orders->GetOrderAt(2)->SetUnloadType(unload);
			const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(400));
			CHECK(graph.GetService(train.orders->index)->stops[1].forced_transfer);
			const auto options = graph.GetBoardableTrips(A, NOW, NOW + 400);
			REQUIRE(options.items.size() == 1);
			REQUIRE(options.items[0].reachable.size() == 1);
			const auto id = options.items[0].reachable[0];
			CHECK(graph.GetTrips()[id.trip].prediction.visits[id.visit].order == 2);
		}
	}
}

TEST_CASE("Passenger graph capacity includes only the requested cargo across a consist", "[passenger][service_graph]")
{
	GraphVehicle train({ GraphStop(A), GraphStop(B) });
	/* SetNext updates tagged pool pointers, so consist parts must be real pool entries. */
	GraphCoach *passengers = Vehicle::Create<GraphCoach>();
	GraphCoach *other = Vehicle::Create<GraphCoach>();
	passengers->cargo_type = PAX;
	passengers->cargo_cap = 50;
	other->cargo_type = CargoType{1};
	other->cargo_cap = 90;
	train.SetNext(passengers);
	passengers->SetNext(other);
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
	train.SetNext(nullptr);
	passengers->SetNext(nullptr);
	delete passengers;
	delete other;
	CHECK(GraphTrip(graph, train.index).capacity == 130);
}

TEST_CASE("Passenger graph retains partial times and reuses journey timing", "[passenger][service_graph]")
{
	GraphVehicle train({ GraphStop(A, 100, 10), GraphStop(B, 600, 100), GraphStop(D, 600) });
	train.LoadAt(1);
	train.current_order = *train.GetOrder(1);
	train.current_order_time = 400;
	train.lateness_counter = 40;
	SECTION("Known progress, dwell and delay") {
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(1000));
		const auto &trip = GraphTrip(graph, train.index);
		REQUIRE(trip.prediction.visits.size() >= 2);
		CHECK(trip.prediction.visits[0].arrival == NOW + 200);
		CHECK(trip.prediction.visits[0].departure == NOW + 260);
		CHECK(trip.prediction.visits[1].arrival == NOW + 860);
		const auto state = GetPassengerRouteState(&train);
		REQUIRE(state);
		const auto journey = PredictPassengerJourney(*state, 1, D, PAX, NOW, true);
		CHECK(trip.prediction.visits[0].departure == journey.departure);
		CHECK(trip.prediction.visits[1].arrival == journey.arrival);
		CHECK(trip.prediction.status == PassengerPredictionStatus::Complete);
		CHECK(graph.GetVisits(B, NOW + 300, NOW + 1000).items.empty());
	}
	SECTION("Missing dwell retains arrival but not departure") {
		train.orders->GetOrderAt(1)->SetWaitTimetabled(false);
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		const auto &trip = GraphTrip(graph, train.index);
		CHECK(trip.prediction.visits[0].arrival == NOW + 200);
		CHECK_FALSE(trip.prediction.visits[0].departure);
		CHECK_FALSE(trip.prediction.visits[1].arrival);
		CHECK(trip.prediction.visits[1].travel_from_previous == StateTicksDelta{600});
		CHECK(graph.GetBoardableTrips(B, NOW, NOW + 1000).status == PassengerPredictionStatus::Unknown);
	}
	SECTION("Missing travel or overdue travel has no invented arrival") {
		train.orders->GetOrderAt(1)->SetTravelTime(0);
		train.orders->GetOrderAt(1)->SetTravelTimetabled(false);
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		CHECK_FALSE(GraphTrip(graph, train.index).prediction.visits[0].arrival);
	}
	SECTION("Full loading preserves arrival only") {
		train.orders->GetOrderAt(1)->SetLoadType(OrderLoadType::FullLoad);
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		CHECK(GraphTrip(graph, train.index).prediction.visits[0].arrival == NOW + 200);
		CHECK_FALSE(GraphTrip(graph, train.index).prediction.visits[0].departure);
	}
	SECTION("Another cargo's full load also makes departure unknown") {
		train.orders->GetOrderAt(1)->SetLoadType(OrderLoadType::CargoTypeLoad);
		train.orders->GetOrderAt(1)->SetLoadType(OrderLoadType::FullLoad, CargoType{1});
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		CHECK(GraphTrip(graph, train.index).prediction.visits[0].arrival == NOW + 200);
		CHECK_FALSE(GraphTrip(graph, train.index).prediction.visits[0].departure);
	}
}

TEST_CASE("Passenger graph waypoint segments, cargo flags and direction are copied", "[passenger][service_graph]")
{
	Order waypoint;
	waypoint.MakeGoToWaypoint(StationID{4});
	waypoint.SetTravelTime(40);
	waypoint.SetTravelTimetabled(true);
	waypoint.SetWaitTime(20);
	waypoint.SetWaitTimetabled(true);
	waypoint.SetWaypointFlags({ OrderWaypointFlag::Reverse });
	Order b = GraphStop(B, 100);
	b.SetLoadType(OrderLoadType::CargoTypeLoad);
	b.SetLoadType(OrderLoadType::NoLoad, PAX);
	b.SetUnloadType(OrderUnloadType::CargoTypeUnload);
	b.SetUnloadType(OrderUnloadType::NoUnload, PAX);
	GraphVehicle train({ GraphStop(A), waypoint, b });
	train.vehicle_flags.Set(VehicleFlag::DrivingBackwards);
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(300));
	const auto *service = graph.GetService(train.orders->index);
	REQUIRE(service->segments.size() >= 1);
	CHECK(service->segments[0].travel == StateTicksDelta{160});
	REQUIRE(service->reverse_waypoints.size() == 1);
	CHECK(service->reverse_waypoints[0] == 1);
	CHECK_FALSE(service->stops[1].can_board);
	CHECK_FALSE(service->stops[1].can_alight);
	CHECK(GraphTrip(graph, train.index).direction == ReverseDir(Direction::NE));
	CHECK(graph.GetServicesAtStation(StationID{4}).empty());
}

TEST_CASE("Passenger graph unknown branches, depots and refits never certify a path", "[passenger][service_graph]")
{
	Order barrier;
	barrier.MakeConditional(3);
	barrier.SetConditionVariable(OrderConditionVariable::LoadPercentage);
	GraphVehicle train({ GraphStop(A), barrier, GraphStop(B), GraphStop(D) });
	SECTION("Dynamic conditional") {}
	SECTION("Depot") { train.orders->GetOrderAt(1)->MakeGoToDepot(DestinationID{0}, OrderDepotTypeFlags{}); }
	SECTION("Refit") { *train.orders->GetOrderAt(1) = GraphStop(C); train.orders->GetOrderAt(1)->SetRefit(PAX); }
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
	const auto &trip = GraphTrip(graph, train.index);
	REQUIRE(trip.prediction.visits.size() == 1);
	CHECK(trip.prediction.status == PassengerPredictionStatus::Unknown);
	const auto options = graph.GetBoardableTrips(A, NOW, NOW + 1000);
	REQUIRE(options.items.size() == 1);
	CHECK(options.items[0].reachable.empty());
	CHECK(options.status == PassengerPredictionStatus::Unknown);
	CHECK(graph.GetVisits(D, NOW, NOW + 1000).status == PassengerPredictionStatus::Unknown);
}

TEST_CASE("Passenger graph handles repeated stops, unconditional jumps and bounded cycles", "[passenger][service_graph]")
{
	GraphVehicle train({ GraphStop(A), GraphStop(B), GraphStop(A), GraphStop(D) });
	SECTION("Repeated station orders and circuits") {
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		const auto *service = graph.GetService(train.orders->index);
		CHECK(service->stops[0].station == service->stops[2].station);
		CHECK(service->stops[0].order != service->stops[2].order);
		CHECK(graph.GetServicesAtStation(A).size() == 1);
		const auto visits = graph.GetVisits(A, NOW, NOW + 1000);
		REQUIRE(visits.items.size() >= 4);
		CHECK(visits.items[0] != visits.items[1]);
	}
	SECTION("Known unconditional jump uses branch time") {
		Order *branch = train.orders->GetOrderAt(1);
		branch->MakeConditional(3);
		branch->SetConditionVariable(OrderConditionVariable::Unconditionally);
		branch->SetWaitTime(50);
		branch->SetWaitTimetabled(true);
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(400));
		CHECK(GraphTrip(graph, train.index).prediction.visits[1].order == 3);
		CHECK(GraphTrip(graph, train.index).prediction.visits[1].arrival == NOW + 60);
	}
	SECTION("Conditional self loop") {
		Order *branch = train.orders->GetOrderAt(1);
		branch->MakeConditional(1);
		branch->SetConditionVariable(OrderConditionVariable::Unconditionally);
		branch->SetWaitTime(0);
		branch->SetWaitTimetabled(true);
		auto limits = GraphLimits();
		limits.steps_per_trip = 20;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		CHECK(GraphTrip(graph, train.index).prediction.status == PassengerPredictionStatus::LimitReached);
	}
	SECTION("Zero-time circuit") {
		for (Order *order : train.orders->Orders()) { order->SetWaitTime(0); order->SetTravelTime(0); }
		auto limits = GraphLimits();
		limits.visits_per_trip = 7;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		CHECK(GraphTrip(graph, train.index).prediction.visits.size() == 7);
		CHECK(GraphTrip(graph, train.index).prediction.status == PassengerPredictionStatus::LimitReached);
	}
}

TEST_CASE("Passenger graph dispatch is independent per vehicle and owns its snapshot", "[passenger][service_graph]")
{
	GraphVehicle train({ GraphStop(A, 10, 0), GraphStop(B, 10, 0), GraphStop(D, 10, 0) });
	train.orders->GetOrderAt(1)->SetDispatchScheduleIndex(0);
	train.vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
	train.orders->GetScheduledDispatchScheduleSet().resize(1);
	auto &schedule = train.orders->GetDispatchScheduleByIndex(0);
	schedule.SetScheduledDispatchStartTick(NOW);
	schedule.SetScheduledDispatchDuration(1000);
	schedule.GetScheduledDispatchMutable() = { { 100, 0, {} }, { 350, 0, {} } };
	GameRandomSeedChecker seeds;
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(500));
	const auto &trip = GraphTrip(graph, train.index);
	REQUIRE(trip.prediction.visits.size() >= 3);
	CHECK(trip.prediction.visits[1].arrival == NOW + 10);
	CHECK(trip.prediction.visits[1].departure == NOW + 100);
	CHECK(trip.prediction.visits[2].arrival == NOW + 110);
	CHECK(schedule.GetScheduledDispatchLastDispatch() == INVALID_SCHEDULED_DISPATCH_OFFSET);
	CHECK(train.current_order_time == 0);
	CHECK(train.lateness_counter == 0);
	CHECK(train.cur_real_order_index == 0);
	CHECK(train.current_order.IsAnyLoadingType());
	const auto connection = [&]() {
		size_t budget = 32768;
		return PredictPassengerConnection(*GetPassengerRouteState(&train), 1, D, PAX, NOW, NOW, NOW + 500, budget);
	}();
	CHECK(connection.departure == trip.prediction.visits[1].departure);
	CHECK(connection.arrival == trip.prediction.visits[2].arrival);
	SECTION("Source edits and destruction do not alter predictions or queries") {
		const OrderListID id = train.orders->index;
		schedule.SetScheduledDispatchLastDispatch(999);
		schedule.GetScheduledDispatchMutable().clear();
		train.orders->GetOrderAt(1)->SetTravelTime(999);
		train.lateness_counter = 999;
		train.orders->FreeChain();
		train.orders = nullptr;
		const auto visits = graph.GetVisits(B, NOW, NOW + 500);
		CHECK_FALSE(visits.items.empty());
		CHECK(graph.GetService(id)->schedules[0].GetScheduledDispatchLastDispatch() == INVALID_SCHEDULED_DISPATCH_OFFSET);
		CHECK(GraphTrip(graph, train.index).prediction.visits[1].departure == NOW + 100);
	}
	SECTION("Unassigned competing dispatch slots stay unknown") {
		GraphVehicle second(train, VehicleID{101});
		second.vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
		const auto competing = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(500));
		CHECK_FALSE(GraphTrip(competing, train.index).prediction.visits[1].departure);
		CHECK(competing.GetVisits(D, NOW, NOW + 500).status == PassengerPredictionStatus::Unknown);
		schedule.SetScheduledDispatchReuseSlots(true);
		const auto reusable = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(500));
		CHECK(GraphTrip(reusable, train.index).prediction.visits[1].departure == NOW + 100);
	}
	SECTION("Invalid dispatch retains its known arrival only") {
		schedule.SetScheduledDispatchDuration(0);
		const auto invalid = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(500));
		CHECK(GraphTrip(invalid, train.index).prediction.visits[1].arrival == NOW + 10);
		CHECK_FALSE(GraphTrip(invalid, train.index).prediction.visits[1].departure);
		CHECK(GraphTrip(invalid, train.index).prediction.status == PassengerPredictionStatus::Unknown);
	}
	SECTION("Copying dispatch slots is charged before allocation") {
		for (uint32_t i = 0; i < 1000; ++i) schedule.GetScheduledDispatchMutable().push_back({ i, 0, {} });
		auto limits = GraphLimits(500);
		limits.work = 500;
		const auto limited = PassengerServiceGraph::Build(PAX, NOW, limits);
		CHECK(limited.GetStatus() == PassengerPredictionStatus::BudgetExhausted);
		CHECK(limited.GetTrips().empty());
	}
	CHECK(seeds.Check());
}

TEST_CASE("Passenger graph distinguishes inactive trains and exhausted budgets from no service", "[passenger][service_graph]")
{
	GraphVehicle train({ GraphStop(A), GraphStop(B) });
	SECTION("Stopped or crashed trains have no usable departures") {
		for (auto flag : { VehState::Stopped, VehState::Crashed }) {
			train.vehstatus = { flag };
			const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
			CHECK(GraphTrip(graph, train.index).prediction.visits.empty());
			CHECK(graph.GetBoardableTrips(A, NOW, NOW + 1000).items.empty());
		}
	}
	SECTION("Mismatched current/implicit orders remain unknown") {
		train.cur_implicit_order_index = 1;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		CHECK(GraphTrip(graph, train.index).operation == PassengerServiceGraph::Operation::Unknown);
		CHECK(graph.GetBoardableTrips(A, NOW, NOW + 1000).status == PassengerPredictionStatus::Unknown);
	}
	SECTION("Cold discovery budget") {
		auto limits = GraphLimits();
		limits.work = 0;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		CHECK(graph.GetStatus() == PassengerPredictionStatus::BudgetExhausted);
		CHECK(graph.GetBoardableTrips(A, NOW, NOW + 1000).status == PassengerPredictionStatus::BudgetExhausted);
	}
	SECTION("Trip limit") {
		auto limits = GraphLimits();
		limits.trips = 0;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		CHECK(graph.GetStatus() == PassengerPredictionStatus::LimitReached);
	}
	SECTION("Prediction and query limits") {
		auto limits = GraphLimits();
		limits.visits_per_trip = 1;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		CHECK(GraphTrip(graph, train.index).prediction.status == PassengerPredictionStatus::LimitReached);
		CHECK(graph.GetVisits(A, NOW, NOW + 1000, 0).status == PassengerPredictionStatus::BudgetExhausted);
		CHECK(graph.GetVisits(A, NOW - 1, NOW + 1000).status == PassengerPredictionStatus::Unknown);
		CHECK(graph.GetVisits(A, NOW, NOW + 1001).status == PassengerPredictionStatus::Unknown);
	}
}

TEST_CASE("Passenger graph reports unfinished boarding candidates at the exact budget boundary", "[passenger][service_graph]")
{
	GraphVehicle first({ GraphStop(A) });
	GraphVehicle second(first, VehicleID{101});
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(50));
	REQUIRE(graph.GetTrips().size() == 2);
	for (const auto &trip : graph.GetTrips()) {
		REQUIRE(trip.prediction.status == PassengerPredictionStatus::Complete);
		REQUIRE(trip.prediction.visits.size() == 1); // No downstream visits consume extra work.
	}
	/* Selection costs one service, two trips and two visits. Each candidate costs one more. */
	SECTION("Only the first candidate fits") {
		const auto result = graph.GetBoardableTrips(A, NOW, NOW + 50, 6);
		REQUIRE(result.items.size() == 1);
		CHECK(result.items[0].reachable.empty());
		CHECK(result.items[0].status == PassengerPredictionStatus::Complete);
		CHECK(result.status == PassengerPredictionStatus::BudgetExhausted);
	}
	SECTION("The last candidate uses the final unit of work") {
		const auto result = graph.GetBoardableTrips(A, NOW, NOW + 50, 7);
		CHECK(result.items.size() == 2);
		CHECK(result.status == PassengerPredictionStatus::Complete);
	}
	SECTION("Selection uses the final unit before any candidate can be processed") {
		const auto result = graph.GetBoardableTrips(A, NOW, NOW + 50, 5);
		CHECK(result.items.empty());
		CHECK(result.status == PassengerPredictionStatus::BudgetExhausted);
	}
}

TEST_CASE("Passenger graph certifies only the known time prefix of a truncated prediction", "[passenger][service_graph]")
{
	GraphVehicle train({ GraphStop(A), GraphStop(B) });
	auto limits = GraphLimits();
	SECTION("Visit limit preserves complete earlier station windows") {
		limits.visits_per_trip = 3;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		const auto &prediction = GraphTrip(graph, train.index).prediction;
		CHECK(prediction.status == PassengerPredictionStatus::LimitReached);
		CHECK(prediction.start == NOW);
		CHECK(prediction.complete_until == NOW + 230);
		CHECK(prediction.Covers(NOW, NOW + 230));
		CHECK_FALSE(prediction.Covers(NOW + 230, NOW + 231));
		CHECK(graph.GetVisits(A, NOW, NOW + 230).status == PassengerPredictionStatus::Complete);
		CHECK(graph.GetVisits(A, NOW + 230, NOW + 231).status == PassengerPredictionStatus::LimitReached);
		const auto absent = graph.GetVisits(B, NOW, NOW + 100);
		CHECK(absent.items.empty());
		CHECK(absent.status == PassengerPredictionStatus::Complete);
		const auto boarding = graph.GetBoardableTrips(A, NOW, NOW + 200);
		REQUIRE(boarding.items.size() == 1);
		CHECK(boarding.status == PassengerPredictionStatus::Complete);
		CHECK(boarding.items[0].reachable.size() == 2);
		CHECK(boarding.items[0].status == PassengerPredictionStatus::LimitReached);
		CHECK(graph.GetBoardableTrips(A, NOW, NOW + 100, 6).status == PassengerPredictionStatus::BudgetExhausted);
		CHECK(graph.GetBoardableTrips(A, NOW, NOW + 100, 7).status == PassengerPredictionStatus::Complete);
		CHECK(graph.GetBoardableTrips(A, NOW, NOW + 1000).status == PassengerPredictionStatus::LimitReached);
	}
	SECTION("Step limit preserves the same prefix contract") {
		limits.steps_per_trip = 2;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		const auto &prediction = GraphTrip(graph, train.index).prediction;
		CHECK(prediction.status == PassengerPredictionStatus::LimitReached);
		CHECK(prediction.complete_until == NOW + 120);
		CHECK(graph.GetVisits(B, NOW, NOW + 120).status == PassengerPredictionStatus::Complete);
		CHECK(graph.GetVisits(B, NOW, NOW + 121).status == PassengerPredictionStatus::LimitReached);
	}
	SECTION("Zero-time occurrences at the cutoff remain uncertified") {
		for (Order *order : train.orders->Orders()) { order->SetWaitTime(0); order->SetTravelTime(0); }
		limits.visits_per_trip = 3;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		const auto &prediction = GraphTrip(graph, train.index).prediction;
		CHECK(prediction.complete_until == NOW);
		CHECK_FALSE(prediction.Covers(NOW, NOW + 1));
		CHECK(graph.GetVisits(A, NOW, NOW + 1).status == PassengerPredictionStatus::LimitReached);
	}
	SECTION("Prediction work exhaustion preserves only the already processed prefix") {
		size_t budget = 1;
		const auto prediction = PredictPassengerVisits(*GetPassengerRouteState(&train), PAX, NOW, NOW + 1000, 128, 4096, budget);
		CHECK(budget == 0);
		CHECK(prediction.status == PassengerPredictionStatus::BudgetExhausted);
		CHECK(prediction.complete_until == NOW + 10);
		CHECK(prediction.Covers(NOW, NOW + 10));
		CHECK_FALSE(prediction.Covers(NOW, NOW + 11));
	}
	SECTION("Full horizon coverage includes empty intervals between events") {
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		const auto &prediction = GraphTrip(graph, train.index).prediction;
		CHECK(prediction.status == PassengerPredictionStatus::Complete);
		CHECK(prediction.complete_until == graph.GetEnd());
		CHECK(prediction.Covers(graph.GetStart(), graph.GetEnd()));
	}
}

TEST_CASE("Passenger graph keeps uncertainty beyond the certified prefix out of early queries", "[passenger][service_graph]")
{
	GraphVehicle train({ GraphStop(A), GraphStop(B), GraphStop(D) });
	SECTION("Unknown dwell and later unknown visits do not fall inside earlier windows") {
		train.orders->GetOrderAt(1)->SetWaitTimetabled(false);
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		const auto &prediction = GraphTrip(graph, train.index).prediction;
		CHECK(prediction.status == PassengerPredictionStatus::Unknown);
		CHECK(prediction.complete_until == NOW + 110);
		for (StationID station : { B, D }) {
			const auto early = graph.GetVisits(station, NOW, NOW + 110);
			CHECK(early.items.empty());
			CHECK(early.status == PassengerPredictionStatus::Complete);
			CHECK(graph.GetVisits(station, NOW, NOW + 111).status == PassengerPredictionStatus::Unknown);
		}
		const auto boarding = graph.GetBoardableTrips(A, NOW, NOW + 110);
		REQUIRE(boarding.items.size() == 1);
		CHECK(boarding.status == PassengerPredictionStatus::Complete);
		CHECK(boarding.items[0].status == PassengerPredictionStatus::Unknown);
	}
	SECTION("Dynamic branches stop coverage at the last known departure") {
		Order *branch = train.orders->GetOrderAt(1);
		branch->MakeConditional(2);
		branch->SetConditionVariable(OrderConditionVariable::LoadPercentage);
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		CHECK(GraphTrip(graph, train.index).prediction.complete_until == NOW + 10);
		CHECK(graph.GetVisits(D, NOW, NOW + 10).status == PassengerPredictionStatus::Complete);
		CHECK(graph.GetVisits(D, NOW, NOW + 11).status == PassengerPredictionStatus::Unknown);
	}
	SECTION("An unresolved dispatch does not certify its nominal dwell") {
		Order *order = train.orders->GetOrderAt(1);
		order->SetWaitTime(50);
		order->SetDispatchScheduleIndex(0);
		train.vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
		train.orders->GetScheduledDispatchScheduleSet().resize(1);
		auto &schedule = train.orders->GetDispatchScheduleByIndex(0);
		schedule.SetScheduledDispatchStartTick(NOW);
		schedule.SetScheduledDispatchDuration(1000);
		schedule.GetScheduledDispatchMutable() = { { 200, 0, {} }, { 400, 0, {} } };
		size_t budget = 2; // Orders fit; the dispatch slot search does not.
		const auto prediction = PredictPassengerVisits(*GetPassengerRouteState(&train), PAX, NOW, NOW + 1000, 128, 4096, budget);
		CHECK(prediction.status == PassengerPredictionStatus::BudgetExhausted);
		CHECK(prediction.complete_until == NOW + 110);
		CHECK_FALSE(prediction.visits.back().departure);
	}
}

TEST_CASE("Passenger graph station queries retain occurrence order rather than departure order", "[passenger][service_graph]")
{
	GraphVehicle first({ GraphStop(A, 100, 20) });
	GraphVehicle second(first, VehicleID{101});
	first.lateness_counter = -100;
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(50));
	const auto visits = graph.GetVisits(A, NOW, NOW + 50);
	REQUIRE(visits.items.size() == 1); // The first trip's later departure lies outside this window.
	CHECK(visits.items[0].trip == 1);
	const auto extended = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(150));
	const auto all = extended.GetVisits(A, NOW, NOW + 150);
	REQUIRE(all.items.size() == 3);
	CHECK(all.items[0] == PassengerServiceGraph::VisitID{0, 0});
	CHECK(all.items[1] == PassengerServiceGraph::VisitID{1, 0});
	CHECK(all.items[2] == PassengerServiceGraph::VisitID{1, 1});
	CHECK(*extended.GetTrips()[0].prediction.visits[0].departure > *extended.GetTrips()[1].prediction.visits[0].departure);
	const auto boarding = extended.GetBoardableTrips(A, NOW, NOW + 150);
	REQUIRE(boarding.items.size() == all.items.size());
	for (size_t i = 0; i < all.items.size(); ++i) CHECK(boarding.items[i].boarding == all.items[i]);
}

TEST_CASE("Passenger graph construction and queries are deterministic and leave live state unchanged", "[passenger][service_graph]")
{
	GraphVehicle train({ GraphStop(A), GraphStop(B), GraphStop(D) });
	GraphVehicle second(train, VehicleID{101});
	second.current_order_time = 3;
	second.lateness_counter = -15;
	const auto before_order = train.current_order;
	const auto before_orders = train.orders->GetOrderVector();
	const auto before_vehicles = Vehicle::GetNumItems();
	const auto before_lists = OrderList::GetNumItems();
	GameRandomSeedChecker seeds;
	const auto first = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
	const auto other = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
	REQUIRE(first.GetTrips().size() == other.GetTrips().size());
	for (size_t i = 0; i < first.GetTrips().size(); ++i) {
		const auto &a = first.GetTrips()[i];
		const auto &b = other.GetTrips()[i];
		CHECK(a.vehicle == b.vehicle);
		CHECK(a.service == b.service);
		CHECK(a.prediction.status == b.prediction.status);
		CHECK(a.prediction.start == b.prediction.start);
		CHECK(a.prediction.complete_until == b.prediction.complete_until);
		REQUIRE(a.prediction.visits.size() == b.prediction.visits.size());
		for (size_t j = 0; j < a.prediction.visits.size(); ++j) {
			CHECK(a.prediction.visits[j].order == b.prediction.visits[j].order);
			CHECK(a.prediction.visits[j].arrival == b.prediction.visits[j].arrival);
			CHECK(a.prediction.visits[j].departure == b.prediction.visits[j].departure);
		}
	}
	CHECK(first.GetVisits(B, NOW, NOW + 1000).items == other.GetVisits(B, NOW, NOW + 1000).items);
	CHECK(first.GetBoardableTrips(A, NOW, NOW + 1000).items.size() == other.GetBoardableTrips(A, NOW, NOW + 1000).items.size());
	CHECK(train.current_order.Equals(before_order));
	CHECK(train.orders->GetNumVehicles() == 2);
	CHECK(train.NextShared() == &second);
	CHECK(second.orders == train.orders);
	CHECK(second.current_order_time == 3);
	CHECK(second.lateness_counter == -15);
	for (size_t i = 0; i < before_orders.size(); ++i) {
		CHECK(train.GetOrder(i)->Equals(before_orders[i]));
		CHECK(train.GetOrder(i)->GetWaitTime() == before_orders[i].GetWaitTime());
		CHECK(train.GetOrder(i)->GetTravelTime() == before_orders[i].GetTravelTime());
	}
	CHECK(Vehicle::GetNumItems() == before_vehicles);
	CHECK(OrderList::GetNumItems() == before_lists);
	CHECK(seeds.Check());
}
