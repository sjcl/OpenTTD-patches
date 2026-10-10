/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file passenger_routing.cpp Timetable transfer and cargo staging regression tests. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../passenger_routing.h"
#include "../vehicle_base.h"
#include "../station_base.h"
#include "../economy_base.h"
#include "../settings_type.h"
#include "../date_func.h"
#include "../core/backup_type.hpp"
#include "../sl/saveload.h"
#include <chrono>

static constexpr StationID A{0}, B{1}, C{2}, D{3};
static constexpr CargoType PAX{0};
static constexpr StateTicks NOW{10000};

extern NamedSaveLoadTable GetLinkGraphJobDesc();

TEST_CASE("Passenger settings do not alter the legacy LinkGraphJob save schema", "[passenger]")
{
	for (const NamedSaveLoad &entry : GetLinkGraphJobDesc()) {
		CHECK(std::string_view(entry.name) != "linkgraph.passenger_transfer_timetables");
		CHECK(std::string_view(entry.name) != "linkgraph.passenger_transfer_penalty");
	}
}

static Order Stop(StationID station, TimetableTicks travel, TimetableTicks wait = 0)
{
	Order order;
	order.MakeGoToStation(station);
	order.SetTravelTime(travel);
	order.SetTravelTimetabled(true);
	order.SetWaitTime(wait);
	order.SetWaitTimetabled(true);
	return order;
}

static PassengerRouteState State(std::span<const Order> orders, VehicleOrderID index = 0, bool loading = true)
{
	return { orders, {}, index, 0, 0, loading, false, 1 };
}

TEST_CASE("Passenger transfer compares local running time with express waiting time", "[passenger]")
{
	std::vector<Order> local{ Stop(B, 100), Stop(C, 600), Stop(D, 600) };
	auto stay = PredictPassengerJourney(State(local), 0, D, PAX, NOW, true);
	REQUIRE(stay.arrival == NOW + 1200);
	std::vector<Order> express{ Stop(B, 100, 3600), Stop(D, 600) };
	std::vector<PassengerJourney> alternatives{ PredictPassengerJourney(State(express), 0, D, PAX, NOW, false) };
	CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20) == PassengerTransferDecision::Stay);
	express[0].SetWaitTime(100);
	alternatives[0] = PredictPassengerJourney(State(express), 0, D, PAX, NOW, false);
	CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20) == PassengerTransferDecision::Transfer);

	/* Boarding through an intermediate cargo stop is not supported by the existing loading contract. */
	CHECK_FALSE(PredictPassengerJourney(State(local), 0, D, PAX, NOW, false).reachable);
}

TEST_CASE("Passenger services keep their own travel times and order positions", "[passenger]")
{
	std::vector<Order> slow{ Stop(B, 100, 100), Stop(D, 1800) };
	std::vector<Order> fast{ Stop(B, 100, 1000), Stop(D, 100) };
	std::vector<PassengerJourney> alternatives{
		PredictPassengerJourney(State(slow), 0, D, PAX, NOW, false),
		PredictPassengerJourney(State(fast), 0, D, PAX, NOW, false),
	};
	PassengerJourney stay{ true, NOW, NOW + 1200 };
	CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20) == PassengerTransferDecision::Transfer);
	fast[0].SetWaitTime(1300);
	alternatives[1] = PredictPassengerJourney(State(fast), 0, D, PAX, NOW, false);
	CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20) == PassengerTransferDecision::Stay);

	std::vector<Order> turns{ Stop(B, 100), Stop(A, 100), Stop(B, 100), Stop(D, 500) };
	CHECK_FALSE(PredictPassengerJourney(State(turns), 0, D, PAX, NOW, true).reachable);
	auto onward = PredictPassengerJourney(State(turns, 2), 2, D, PAX, NOW, true);
	CHECK(onward.arrival == NOW + 500);
}

TEST_CASE("Passenger predictions account for current progress, dwell and delay", "[passenger]")
{
	std::vector<Order> orders{ Stop(A, 100, 10), Stop(B, 600, 100), Stop(D, 600) };
	auto state = State(orders, 1, false);
	state.elapsed = 400;
	state.lateness = 40;
	auto trip = PredictPassengerJourney(state, 1, D, PAX, NOW, false);
	CHECK(trip.departure == NOW + 260);
	CHECK(trip.arrival == NOW + 860);
	state.loading = true;
	state.elapsed = 20;
	state.lateness = -50;
	trip = PredictPassengerJourney(state, 1, D, PAX, NOW, true);
	CHECK(trip.departure == NOW + 130);
	CHECK(trip.arrival == NOW + 730);
	state.loading = false;
	state.elapsed = 700; // Still travelling after its recorded travel time.
	CHECK_FALSE(PredictPassengerJourney(state, 1, D, PAX, NOW, false).arrival.has_value());
}

TEST_CASE("Passenger transfers require a usable connection and complete predictions", "[passenger]")
{
	PassengerJourney stay{ true, NOW, NOW + 1000 };
	std::vector<PassengerJourney> alternatives{ { true, NOW + 10, NOW + 100 } };
	CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20) == PassengerTransferDecision::Unknown);
	alternatives[0].departure = NOW + 100;
	CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20) == PassengerTransferDecision::Transfer);
	alternatives.push_back({ true, std::nullopt, std::nullopt });
	CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20) == PassengerTransferDecision::Unknown);
	alternatives[1].reachable = false;
	CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20) == PassengerTransferDecision::Transfer);

	std::vector<Order> orders{ Stop(B, 100), Stop(D, 600) };
	orders[1].SetTravelTime(0);
	orders[1].SetTravelTimetabled(false);
	CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true).arrival.has_value());
	orders[1] = Stop(D, 600);
	orders[0].SetLoadType(OrderLoadType::FullLoad);
	CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true).arrival.has_value());
}

TEST_CASE("Passenger predictions respect unloading, non-stop and route barriers", "[passenger]")
{
	std::vector<Order> orders{ Stop(B, 100), Stop(C, 200), Stop(D, 300) };
	SECTION("Forced transfer before the via") {
		orders[1].SetUnloadType(OrderUnloadType::Transfer);
		CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true).reachable);
	}
	SECTION("Forced unloading before the via") {
		orders[1].SetUnloadType(OrderUnloadType::Unload);
		CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true).reachable);
	}
	SECTION("Via cannot unload") {
		orders[2].SetUnloadType(OrderUnloadType::NoUnload);
		CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true).reachable);
	}
	SECTION("Passes the via without stopping") {
		orders[2].SetNonStopType(ONSF_NO_STOP_AT_DESTINATION_STATION);
		CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true).reachable);
	}
	SECTION("Depot detour") {
		orders[1].MakeGoToDepot(DestinationID{0}, OrderDepotTypeFlags{});
		CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true).arrival.has_value());
	}
}

TEST_CASE("Passenger conditional prediction is bounded and conservative", "[passenger]")
{
	std::vector<Order> orders{ Stop(B, 100), Order{}, Stop(C, 200), Stop(D, 300) };
	orders[1].MakeConditional(3);
	orders[1].SetConditionVariable(OrderConditionVariable::Unconditionally);
	orders[1].SetWaitTime(500);
	orders[1].SetWaitTimetabled(true);
	CHECK(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true).arrival == NOW + 500);
	orders[1].SetConditionVariable(OrderConditionVariable::LoadPercentage);
	CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true).arrival.has_value());
	orders[1].SetConditionVariable(OrderConditionVariable::Unconditionally);
	orders[1].SetConditionSkipToOrder(1);
	CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true).reachable);
}

TEST_CASE("Passenger scheduled dispatch uses slots without modifying the schedule", "[passenger]")
{
	std::vector<Order> orders{ Stop(A, 100), Stop(B, 100), Stop(D, 600) };
	orders[1].SetDispatchScheduleIndex(0);
	std::vector<DispatchSchedule> schedules(1);
	auto &schedule = schedules[0];
	schedule.SetScheduledDispatchStartTick(NOW);
	schedule.SetScheduledDispatchDuration(3600);
	schedule.GetScheduledDispatchMutable().push_back({ 0, 0, {} });
	auto state = State(orders);
	state.scheduled_dispatch = true;
	state.schedules = schedules;
	auto trip = PredictPassengerJourney(state, 1, D, PAX, NOW, false);
	CHECK(trip.departure == NOW + 3600);
	CHECK(trip.arrival == NOW + 4200);
	CHECK(schedule.GetScheduledDispatchLastDispatch() == INVALID_SCHEDULED_DISPATCH_OFFSET);
	state.shared_vehicles = 2;
	CHECK_FALSE(PredictPassengerJourney(state, 1, D, PAX, NOW, false).arrival.has_value());
	state.current_order = 1; // Already assigned a departure at the dispatch stop.
	state.lateness = -300;
	trip = PredictPassengerJourney(state, 1, D, PAX, NOW, true);
	CHECK(trip.arrival == NOW + 900);
	schedule.SetScheduledDispatchDuration(0);
	CHECK_FALSE(PredictPassengerJourney(state, 1, D, PAX, NOW, true).arrival.has_value());
	state.current_order = 0;
	CHECK_FALSE(PredictPassengerJourney(state, 1, D, PAX, NOW, false).arrival.has_value());
}

/** Temporary vehicle avoids graphical/engine setup; all route and cargo processing uses real production code. */
struct PassengerTestVehicle : Vehicle {
	PassengerTestVehicle() : Vehicle(VehicleID::Invalid(), VehicleType::Train) {}
	bool IsPrimaryVehicle() const override { return true; }
};

TEST_CASE("Passenger Stage retains a CargoDist via across intermediate stops", "[passenger]")
{
	AllocateMap(64, 64);
	AutoRestoreBackup linkgraph_backup(_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{});
	AutoRestoreBackup classes_backup(CargoSpec::Get(PAX)->classes, CargoClasses{CargoClass::Passengers});
	AutoRestoreBackup payment_backup(CargoSpec::Get(PAX)->current_payment, Money{0});
	AutoRestoreBackup ticks_backup(_state_ticks, NOW);
	_settings_game.linkgraph.passenger_transfer_timetables = true;
	_settings_game.linkgraph.passenger_transfer_penalty = 20;
	_settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Asymmetric;
	PassengerTestVehicle local, express;
	local.orders = OrderList::Create(std::vector<Order>{ Stop(B, 100), Stop(C, 600), Stop(D, 600) }, &local);
	express.orders = OrderList::Create(std::vector<Order>{ Stop(B, 100, 3600), Stop(D, 600) }, &express);
	for (Vehicle *v : { &local, &express }) {
		v->current_order.MakeLoading(false);
		v->last_station_visited = B;
		v->cargo_type = PAX;
		v->cargo_cap = 100;
	}
	CargoPayment payment(CargoPaymentID::Invalid(), &local);
	GoodsEntry ge;
	ge.CreateData().flows.insert(FlowStat(A, D, 100));
	auto add_packet = [&]() {
		CargoPacket *cp = CargoPacket::Create(A, 10, Source{Source::Invalid, SourceType::Industry});
		cp->UpdateLoadingTile(TileIndex{0});
		local.cargo.Append(cp, VehicleCargoList::MoveToAction::Keep);
	};
	add_packet();
	std::array next{ C };
	SECTION("Disabled feature retains the original transfer decision") {
		_settings_game.linkgraph.passenger_transfer_timetables = false;
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Transfer) == 10);
	}
	SECTION("Faster express still causes a transfer") {
		express.orders->GetOrderAt(0)->SetWaitTime(100);
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Transfer) == 10);
	}
	SECTION("Thousands of packets share one arrival-scoped comparison") {
		for (uint16_t i = 1; i <= 2000; ++i) {
			CargoPacket *cp = CargoPacket::Create(1, i, A, TileIndex{0}, Money{0});
			cp->UpdateLoadingTile(TileIndex{0});
			local.cargo.Append(cp, VehicleCargoList::MoveToAction::Keep);
		}
		REQUIRE(local.cargo.Packets()->size() == 2001);
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Keep) == 2010);
	}
	SECTION("Freight keeps the original transfer behaviour") {
		CargoSpec::Get(PAX)->classes = CargoClasses{CargoClass::Bulk};
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Transfer) == 10);
	}
	SECTION("Forced transfer overrides the prediction") {
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::Transfer, &ge, PAX, &payment, TileIndex{0}, &routing);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Transfer) == 10);
	}
	SECTION("Incomplete timetable uses the original transfer decision") {
		local.orders->GetOrderAt(2)->SetTravelTime(0);
		local.orders->GetOrderAt(2)->SetTravelTimetabled(false);
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Transfer) == 10);
	}
	SECTION("An unrelated service with unknown phase does not block a comparison") {
		PassengerTestVehicle unrelated;
		unrelated.orders = OrderList::Create(std::vector<Order>{ Stop(B, 100), Stop(A, 100) }, &unrelated);
		unrelated.cargo_type = PAX;
		unrelated.cargo_cap = 100;
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Keep) == 10);
		unrelated.orders->FreeChain();
	}
	SECTION("A relevant service with unknown phase retains the original decision") {
		PassengerTestVehicle unknown;
		unknown.orders = OrderList::Create(std::vector<Order>{ Stop(B, 100), Stop(D, 100) }, &unknown);
		unknown.cargo_type = PAX;
		unknown.cargo_cap = 100;
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Transfer) == 10);
		unknown.orders->FreeChain();
	}
	SECTION("A faster connection at an intermediate stop can supersede through riding") {
		local.orders->FreeChain();
		constexpr StationID E{4};
		local.orders = OrderList::Create(std::vector<Order>{ Stop(B, 100), Stop(C, 600), Stop(E, 300), Stop(D, 300) }, &local);
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		REQUIRE(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Keep) == 10);
		local.cur_real_order_index = local.cur_implicit_order_index = local.cur_timetable_order_index = 1;
		local.last_station_visited = C;
		express.orders->FreeChain();
		express.orders = OrderList::Create(std::vector<Order>{ Stop(C, 100, 100), Stop(D, 100) }, &express);
		express.last_station_visited = C;
		PassengerRoutingContext at_c(&local, C);
		std::array next_e{ E };
		local.cargo.Stage(false, C, next_e, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &at_c);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Transfer) == 10);
	}
	SECTION("A direct next stop does not create an unload and reload cycle") {
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		REQUIRE(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Keep) == 10);
		local.cur_real_order_index = local.cur_implicit_order_index = local.cur_timetable_order_index = 1;
		local.last_station_visited = C;
		express.orders->FreeChain();
		express.orders = OrderList::Create(std::vector<Order>{ Stop(C, 100, 100), Stop(D, 100) }, &express);
		express.last_station_visited = C;
		PassengerRoutingContext at_c(&local, C);
		std::array next_d{ D };
		CHECK_FALSE(local.cargo.Stage(false, C, next_d, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &at_c));
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Keep) == 10);
	}
	SECTION("Changing the route cancels an unreachable retained via") {
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		REQUIRE(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Keep) == 10);
		local.cur_real_order_index = local.cur_implicit_order_index = local.cur_timetable_order_index = 1;
		local.last_station_visited = C;
		local.orders->GetOrderAt(2)->SetUnloadType(OrderUnloadType::NoUnload);
		ge.CreateData().flows.erase(A);
		ge.CreateData().flows.insert(FlowStat(A, B, 100));
		PassengerRoutingContext at_c(&local, C);
		std::array next_d{ D };
		local.cargo.Stage(false, C, next_d, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &at_c);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Transfer) == 10);
	}
	SECTION("Through riding survives a changed flow at C and packet splitting") {
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		REQUIRE(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Keep) == 10);
		CargoPacket *split = local.cargo.Packets()->front()->Split(4);
		CHECK(VehicleCargoList::AreMergable(split, local.cargo.Packets()->front()));
		CargoPacket *uncommitted = CargoPacket::Create(A, 1, Source{Source::Invalid, SourceType::Industry});
		uncommitted->UpdateLoadingTile(TileIndex{0});
		CHECK_FALSE(VehicleCargoList::AreMergable(split, uncommitted));
		delete uncommitted;
		delete split;
		local.cargo.InvalidateCache();
		local.cargo.KeepAll();
		local.cur_real_order_index = local.cur_implicit_order_index = local.cur_timetable_order_index = 1;
		local.last_station_visited = C;
		ge.CreateData().flows.erase(A);
		ge.CreateData().flows.insert(FlowStat(A, B, 100)); // Original algorithm would now send the passenger backwards.
		PassengerRoutingContext at_c(&local, C);
		std::array next_d{ D };
		local.cargo.Stage(false, C, next_d, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &at_c);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Keep) == 6);
		local.cur_real_order_index = local.cur_implicit_order_index = local.cur_timetable_order_index = 2;
		local.last_station_visited = D;
		ge.CreateData().flows.erase(A);
		ge.CreateData().flows.insert(FlowStat(A, D, 100));
		PassengerRoutingContext at_d(&local, D);
		local.cargo.Stage(true, D, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &at_d);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Deliver) == 6);
	}
	local.cargo.KeepAll();
	local.cargo.Truncate();
	local.orders->FreeChain();
	express.orders->FreeChain();
	DeallocateMap();
}

TEST_CASE("Passenger predictor scales linearly with the order horizon", "[passenger][.performance]")
{
	std::vector<Order> orders;
	for (uint16_t i = 0; i < 1024; ++i) orders.push_back(Stop(StationID{i}, 100));
	const auto begin = std::chrono::steady_clock::now();
	for (int i = 0; i < 10000; ++i) {
		auto trip = PredictPassengerJourney(State(orders), 0, StationID{1023}, PAX, NOW, true);
		REQUIRE(trip.arrival == NOW + 102300);
	}
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
	WARN("10,000 predictions of 1,024 orders: " << ms << " ms");
}
