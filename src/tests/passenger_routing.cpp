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
#include "passenger_test_stations.h"
#include "../station_base.h"
#include "../economy_base.h"
#include "../settings_type.h"
#include "../settings_internal.h"
#include "../date_func.h"
#include "../core/backup_type.hpp"
#include "../core/random_func.hpp"
#include "../sl/saveload.h"
#include "../timetable.h"
#include <chrono>
#include <deque>

static constexpr StationID A{0}, B{1}, C{2}, D{3};
static constexpr CargoType PAX{0};
static constexpr StateTicks NOW{10000};

extern NamedSaveLoadTable GetLinkGraphJobDesc();
extern SaveLoadVersion _sl_version;
extern std::vector<uint8_t> SlTestSaveLoadTable(void *, void *, const NamedSaveLoadTable &, const NamedSaveLoadTable &);

TEST_CASE("Passenger capacity rerouting is synchronized and persists with an enabled legacy default", "[passenger][settings][save]")
{
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	const auto *setting = GetSettingFromName("linkgraph.passenger_capacity_rerouting"); REQUIRE(setting != nullptr);
	REQUIRE(setting->AsIntSetting() != nullptr);
	CHECK(setting->AsIntSetting()->IsBoolSetting());
	CHECK_FALSE(setting->flags.Test(SettingFlag::NoNetworkSync));
	CHECK_FALSE(setting->flags.Test(SettingFlag::NotInSave));
	const std::array fields{NSL(setting->name, setting->save)};
	GameSettings source{}, loaded{};
	for (bool enabled : {false, true}) {
		source.linkgraph.passenger_capacity_rerouting = enabled;
		loaded.linkgraph.passenger_capacity_rerouting = !enabled;
		SlTestSaveLoadTable(&source, &loaded, fields, fields);
		CHECK(loaded.linkgraph.passenger_capacity_rerouting == enabled);
	}
	loaded.linkgraph.passenger_capacity_rerouting = false;
	setting->ResetToDefault(&loaded);
	SlTestSaveLoadTable(&source, &loaded, NamedSaveLoadTable{}, fields);
	CHECK(loaded.linkgraph.passenger_capacity_rerouting);
}

TEST_CASE("Passenger settings do not alter the legacy LinkGraphJob save schema", "[passenger]")
{
	for (const NamedSaveLoad &entry : GetLinkGraphJobDesc()) {
		CHECK(std::string_view(entry.name) != "linkgraph.passenger_transfer_timetables");
		CHECK(std::string_view(entry.name) != "linkgraph.passenger_transfer_penalty");
		CHECK(std::string_view(entry.name) != "linkgraph.passenger_capacity_rerouting");
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
	/* An excluded first departure alone does not prove the rest of the service is slower. */
	CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20, true) == PassengerTransferDecision::Stay);
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

TEST_CASE("Timetable assumption ignores early departure and full loading only in planner predictions", "[passenger][timetable]")
{
	std::vector<Order> orders{Stop(A, 100, 100), Stop(B, 200, 100), Stop(D, 300, 100)};
	const auto predict = [&](PassengerTimingPolicy timing) {
		size_t budget = 65536;
		return PredictPassengerVisits(State(orders), PAX, NOW, NOW + 1500, 32, 128, budget, {}, true, timing);
	};
	const auto normal = predict(PassengerTimingPolicy::AssumeTimetable);
	for (OrderLeaveType leave : {OLT_NORMAL, OLT_LEAVE_EARLY, OLT_LEAVE_EARLY_FULL_ANY, OLT_LEAVE_EARLY_FULL_ALL}) {
		for (OrderLoadType load : {OrderLoadType::LoadIfPossible, OrderLoadType::FullLoad, OrderLoadType::FullLoadAny, OrderLoadType::CargoTypeLoad}) {
			CAPTURE(leave, to_underlying(load));
			for (Order &order : orders) {
				order.SetLeaveType(leave);
				order.SetLoadType(load);
				if (load == OrderLoadType::CargoTypeLoad) order.SetLoadType(OrderLoadType::FullLoad, CargoType{1});
			}
			const auto assumed = predict(PassengerTimingPolicy::AssumeTimetable);
			REQUIRE(assumed.status == PassengerPredictionStatus::Complete);
			REQUIRE(assumed.visits.size() == normal.visits.size());
			for (size_t i = 0; i < normal.visits.size(); ++i) {
				CHECK(assumed.visits[i].arrival == normal.visits[i].arrival);
				CHECK(assumed.visits[i].departure == normal.visits[i].departure);
			}
			CHECK(assumed.visits[3].order == 0); // Next circuit is timed too.
			CHECK(assumed.visits[3].departure == NOW + 1000);
			const auto conservative = predict(PassengerTimingPolicy::Conservative);
			if (leave != OLT_NORMAL || load != OrderLoadType::LoadIfPossible) CHECK(conservative.status == PassengerPredictionStatus::Unknown);
		}
	}
}

TEST_CASE("Timetable assumption uses elapsed progress and explicit timing flags", "[passenger][timetable]")
{
	std::vector<Order> orders{Stop(A, 100, 100), Stop(B, 200, 100), Stop(D, 300, 100)};
	auto state = State(orders);
	const auto predict = [&](bool prefix = true) {
		size_t budget = 65536;
		return PredictPassengerVisits(state, PAX, NOW, NOW + 1500, 32, 128, budget, {}, prefix, PassengerTimingPolicy::AssumeTimetable);
	};
	SECTION("Early and late loading keep a full future dwell") {
		for (Ticks lateness : {-500, 0, 500}) {
			state.lateness = lateness; state.elapsed = 40;
			const auto prediction = predict();
			REQUIRE(prediction.visits.size() >= 3);
			CHECK(prediction.visits[0].departure == NOW + 60);
			CHECK(prediction.visits[1].arrival == NOW + 260);
			CHECK(prediction.visits[1].departure == NOW + 360);
		}
		state.elapsed = 500;
		CHECK(predict().visits[0].departure == NOW);
	}
	SECTION("Travelling subtracts only current elapsed time") {
		state.loading = false; state.elapsed = 40; state.lateness = 500;
		const auto prediction = predict();
		CHECK(prediction.visits[0].arrival == NOW + 60);
		CHECK(prediction.visits[0].departure == NOW + 160);
	}
	SECTION("Overdue travelling cannot invent an arrival but retains downstream durations") {
		state.loading = false; state.elapsed = 101;
		CHECK(predict().stop == PassengerPredictionStop::Timing);
		CHECK(predict().visits.empty());
		const auto prediction = predict(false);
		REQUIRE(prediction.visits.size() >= 2);
		CHECK_FALSE(prediction.visits[0].arrival);
		CHECK_FALSE(prediction.visits[1].arrival);
		CHECK(prediction.visits[1].travel_from_previous == StateTicksDelta{200});
	}
	SECTION("Unflagged nonzero travel remains unknown") {
		orders[1].SetTravelTimetabled(false);
		CHECK(predict().stop == PassengerPredictionStop::Timing);
		CHECK(predict().visits.size() == 1);
	}
	SECTION("Unset dwell is unknown even with full loading") {
		orders[0].SetWaitTimetabled(false); orders[0].SetLoadType(OrderLoadType::FullLoad);
		CHECK_FALSE(predict().visits[0].departure);
	}
	SECTION("Explicit zero is timed; unset zero is unknown") {
		orders[1].SetTravelTime(0); orders[1].SetWaitTime(0);
		CHECK(predict().status == PassengerPredictionStatus::Complete);
		orders[1].SetTravelTimetabled(false);
		CHECK(predict().status == PassengerPredictionStatus::Unknown);
	}
}

TEST_CASE("Passenger connections search actual repeated visits within the stay horizon", "[passenger]")
{
	std::vector<Order> express{ Stop(B, 400, 10), Stop(D, 100) };
	PassengerJourney stay{ true, NOW, NOW + 1000 };
	size_t budget = 32768;
	auto connection = [&]() {
		return PredictPassengerConnection(State(express), 0, D, PAX, NOW, NOW + 20, *stay.arrival - 20, budget);
	};
	SECTION("Missed first express, catchable second express arrives sooner") {
		std::array alternatives{ connection() };
		CHECK(alternatives[0].departure == NOW + 520);
		CHECK(alternatives[0].arrival == NOW + 620);
		CHECK_FALSE(alternatives[0].boarding_now);
		CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20, true) == PassengerTransferDecision::Transfer);
	}
	SECTION("Next circuit is too slow: complete exclusion means stay") {
		express[0].SetTravelTime(1200);
		std::array alternatives{ connection() };
		CHECK_FALSE(alternatives[0].reachable);
		CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20, true) == PassengerTransferDecision::Stay);
	}
	SECTION("Connection at exactly the minimum transfer time is catchable") {
		express[0].SetWaitTime(20);
		std::array alternatives{ connection() };
		CHECK(alternatives[0].boarding_now);
		CHECK(alternatives[0].departure == NOW + 20);
		CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20, true) == PassengerTransferDecision::Transfer);
	}
	SECTION("An arrival tied with staying after the transfer cost cannot justify getting off") {
		express[0].SetWaitTime(100);
		express[1].SetTravelTime(880);
		std::array alternatives{ connection() };
		CHECK_FALSE(alternatives[0].reachable);
		CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20, true) == PassengerTransferDecision::Stay);
	}
	SECTION("A later unknown travel leg is not a completed exclusion") {
		express[0].SetTravelTime(0);
		express[0].SetTravelTimetabled(false);
		std::array alternatives{ connection() };
		CHECK(alternatives[0].reachable);
		CHECK_FALSE(alternatives[0].arrival);
		CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20, true) == PassengerTransferDecision::Unknown);
	}
	SECTION("Exhausting the work allowance does not choose stay") {
		budget = 2;
		std::array alternatives{ connection() };
		CHECK(budget == 0);
		CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20, true) == PassengerTransferDecision::Unknown);
	}
	SECTION("Zero time circuits terminate conservatively") {
		express = { Stop(B, 0), Stop(D, 0) };
		std::array alternatives{ connection() };
		CHECK(budget > 32760);
		CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20, true) == PassengerTransferDecision::Unknown);
	}
	SECTION("Unknown full load after an excluded departure remains unknown") {
		express[1].SetLoadType(OrderLoadType::FullLoad);
		std::array alternatives{ connection() };
		CHECK(ComparePassengerJourneys(stay, alternatives, NOW, 20, true) == PassengerTransferDecision::Unknown);
	}
}

TEST_CASE("Passenger repeated dispatch predictions use private slot history", "[passenger]")
{
	std::vector<Order> express{ Stop(B, 10), Stop(D, 10) };
	express[0].SetDispatchScheduleIndex(0);
	std::vector<DispatchSchedule> schedules(1);
	auto &schedule = schedules[0];
	schedule.SetScheduledDispatchStartTick(NOW);
	schedule.SetScheduledDispatchDuration(1000);
	schedule.SetScheduledDispatchDelay(100); // Reusing the original history would select the already used zero slot.
	schedule.GetScheduledDispatchMutable() = { { 0, 0, {} }, { 100, 0, {} }, { 350, 0, {} } };
	auto state = State(express, 1, false);
	state.scheduled_dispatch = true;
	state.schedules = schedules;
	size_t budget = 32768;
	SECTION("Later dispatch slot is catchable, without changing the real schedule") {
		auto trip = PredictPassengerConnection(state, 0, D, PAX, NOW, NOW + 50, NOW + 500, budget);
		CHECK(trip.departure == NOW + 100);
		CHECK(trip.arrival == NOW + 110);
		CHECK(schedule.GetScheduledDispatchLastDispatch() == INVALID_SCHEDULED_DISPATCH_OFFSET);
	}
	SECTION("Unassigned shared slots remain uncertain") {
		state.shared_vehicles = 2;
		auto trip = PredictPassengerConnection(state, 0, D, PAX, NOW, NOW + 50, NOW + 500, budget);
		CHECK(trip.reachable);
		CHECK_FALSE(trip.arrival);
	}
	SECTION("Reusable shared slots can be predicted") {
		state.shared_vehicles = 2;
		schedule.SetScheduledDispatchReuseSlots(true);
		schedule.SetScheduledDispatchDelay(0);
		auto trip = PredictPassengerConnection(state, 0, D, PAX, NOW, NOW + 50, NOW + 500, budget);
		CHECK(trip.departure == NOW + 100);
	}
	SECTION("Slot traversal consumes the same bounded allowance as orders") {
		for (uint32_t i = 1; i < 40000; ++i) schedule.GetScheduledDispatchMutable().push_back({ i, 0, {} });
		auto trip = PredictPassengerConnection(state, 0, D, PAX, NOW, NOW + 50, NOW + 500, budget);
		CHECK(budget == 0);
		CHECK(trip.reachable);
		CHECK_FALSE(trip.arrival);
	}
}

TEST_CASE("Passenger predictions respect unloading, non-stop and route barriers", "[passenger]")
{
	const auto timing = GENERATE(PassengerTimingPolicy::Conservative, PassengerTimingPolicy::AssumeTimetable);
	std::vector<Order> orders{ Stop(B, 100), Stop(C, 200), Stop(D, 300) };
	SECTION("Forced transfer before the via") {
		orders[1].SetUnloadType(OrderUnloadType::Transfer);
		CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true, timing).reachable);
	}
	SECTION("Forced unloading before the via") {
		orders[1].SetUnloadType(OrderUnloadType::Unload);
		CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true, timing).reachable);
	}
	SECTION("Via cannot unload") {
		orders[2].SetUnloadType(OrderUnloadType::NoUnload);
		CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true, timing).reachable);
	}
	SECTION("Passes the via without stopping") {
		orders[2].SetNonStopType(ONSF_NO_STOP_AT_DESTINATION_STATION);
		CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true, timing).reachable);
	}
	SECTION("Depot detour") {
		orders[1].MakeGoToDepot(DestinationID{0}, OrderDepotTypeFlags{});
		CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true, timing).arrival.has_value());
	}
}

TEST_CASE("Passenger conditional prediction is bounded and conservative", "[passenger]")
{
	const auto timing = GENERATE(PassengerTimingPolicy::Conservative, PassengerTimingPolicy::AssumeTimetable);
	std::vector<Order> orders{ Stop(B, 100), Order{}, Stop(C, 200), Stop(D, 300) };
	orders[1].MakeConditional(3);
	orders[1].SetConditionVariable(OrderConditionVariable::Unconditionally);
	orders[1].SetWaitTime(500);
	orders[1].SetWaitTimetabled(true);
	CHECK(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true, timing).arrival == NOW + 500);
	orders[1].SetConditionVariable(OrderConditionVariable::LoadPercentage);
	CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true, timing).arrival.has_value());
	orders[1].SetConditionVariable(OrderConditionVariable::Unconditionally);
	orders[1].SetConditionSkipToOrder(1);
	CHECK_FALSE(PredictPassengerJourney(State(orders), 0, D, PAX, NOW, true, timing).reachable);
}

TEST_CASE("Passenger scheduled dispatch uses slots without modifying the schedule", "[passenger]")
{
	const auto timing = GENERATE(PassengerTimingPolicy::Conservative, PassengerTimingPolicy::AssumeTimetable);
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
	auto trip = PredictPassengerJourney(state, 1, D, PAX, NOW, false, timing);
	CHECK(trip.departure == NOW + 3600);
	CHECK(trip.arrival == NOW + 4200);
	CHECK(schedule.GetScheduledDispatchLastDispatch() == INVALID_SCHEDULED_DISPATCH_OFFSET);
	state.shared_vehicles = 2;
	CHECK_FALSE(PredictPassengerJourney(state, 1, D, PAX, NOW, false, timing).arrival.has_value());
	state.current_order = 1; // Already assigned a departure at the dispatch stop.
	state.lateness = -300;
	trip = PredictPassengerJourney(state, 1, D, PAX, NOW, true, timing);
	CHECK(trip.arrival == NOW + 900);
	schedule.SetScheduledDispatchDuration(0);
	CHECK_FALSE(PredictPassengerJourney(state, 1, D, PAX, NOW, true, timing).arrival.has_value());
	state.current_order = 0;
	CHECK_FALSE(PredictPassengerJourney(state, 1, D, PAX, NOW, false, timing).arrival.has_value());
}

/** Temporary vehicle avoids graphical/engine setup; all route and cargo processing uses real production code. */
struct PassengerTestVehicle : Vehicle {
	explicit PassengerTestVehicle(VehicleID id = VehicleID::Invalid()) : Vehicle(id, VehicleType::Train) {}
	bool IsPrimaryVehicle() const override { return true; }
};

/** Suppress the single-vehicle GUI notification: these temporary vehicles have no pool/window ID. */
static void DetachPassengerTestVehicle(PassengerTestVehicle &v)
{
	OrderList *list = v.orders;
	list->AddVehicle(nullptr);
	v.RemoveFromShared();
	list->RemoveVehicle(nullptr);
	v.orders = nullptr;
}

TEST_CASE("Passenger Stage retains a CargoDist via across intermediate stops", "[passenger]")
{
	PassengerTestStations stations;
	EnsurePassengerTestStations(std::array{Stop(A, 0), Stop(B, 0), Stop(C, 0), Stop(D, 0)});
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
	SECTION("A just-missed express and its slower next visit cannot make passengers get off") {
		express.orders->GetOrderAt(0)->SetWaitTime(10);
		PassengerRoutingContext routing(&local, B);
		CHECK(routing.Decide(PAX, D) == PassengerTransferDecision::Stay);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Keep) == 10);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Transfer) == 0);
	}
	SECTION("Future seating after a missed fast express is not a confirmed connection") {
		express.orders->GetOrderAt(0)->SetWaitTime(10);
		express.orders->GetOrderAt(1)->SetTravelTime(100);
		PassengerRoutingContext routing(&local, B);
		CHECK(routing.Decide(PAX, D) == PassengerTransferDecision::Unknown);
	}
	SECTION("A full express and its slower next visit are excluded") {
		express.orders->GetOrderAt(0)->SetWaitTime(100);
		express.cargo.Append(CargoPacket::Create(A, 100, Source{Source::Invalid, SourceType::Industry}));
		PassengerRoutingContext routing(&local, B);
		CHECK(routing.Decide(PAX, D) == PassengerTransferDecision::Stay);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Keep) == 10);
	}
	SECTION("Confirmed unloading at this station frees seats on an otherwise full express") {
		express.orders->GetOrderAt(0)->SetWaitTime(100);
		express.cargo.Append(CargoPacket::Create(A, 100, Source{Source::Invalid, SourceType::Industry}), VehicleCargoList::MoveToAction::Deliver);
		express.vehicle_flags.Set(VehicleFlag::CargoUnloading);
		PassengerRoutingContext routing(&local, B);
		CHECK(routing.Decide(PAX, D) == PassengerTransferDecision::Transfer);
	}
	SECTION("Reserved loading occupies seats even before passengers are on board") {
		express.orders->GetOrderAt(0)->SetWaitTime(100);
		express.cargo.Append(CargoPacket::Create(A, 100, Source{Source::Invalid, SourceType::Industry}), VehicleCargoList::MoveToAction::Load);
		CHECK(express.cargo.StoredCount() == 0);
		CHECK(express.cargo.ReservedCount() == 100);
		PassengerRoutingContext routing(&local, B);
		CHECK(routing.Decide(PAX, D) == PassengerTransferDecision::Stay);
	}
	SECTION("A few free seats are not promised to every packet on the incoming train") {
		express.orders->GetOrderAt(0)->SetWaitTime(100);
		express.cargo_cap = 5;
		PassengerRoutingContext routing(&local, B);
		CHECK(routing.Decide(PAX, D) == PassengerTransferDecision::Unknown);
	}
	SECTION("Unevenly spaced shared vehicles are compared using their own assigned departures") {
		express.orders->GetOrderAt(0)->SetWaitTime(10);
		PassengerTestVehicle second, third;
		for (Vehicle *v : { &second, &third }) {
			v->orders = express.orders;
			v->AddToShared(&express);
			v->current_order.MakeLoading(false);
			v->last_station_visited = B;
			v->cargo_type = PAX;
			v->cargo_cap = 100;
		}
		second.lateness_counter = -100;
		third.lateness_counter = -2000;
		PassengerRoutingContext routing(&local, B);
		CHECK(routing.Decide(PAX, D) == PassengerTransferDecision::Transfer);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Transfer) == 10);
		DetachPassengerTestVehicle(second);
		DetachPassengerTestVehicle(third);
	}
	SECTION("A later express with seats can beat the full earliest express") {
		express.orders->GetOrderAt(0)->SetWaitTime(100);
		express.cargo.Append(CargoPacket::Create(A, 100, Source{Source::Invalid, SourceType::Industry}));
		PassengerTestVehicle second;
		second.orders = express.orders;
		second.AddToShared(&express);
		second.current_order.MakeLoading(false);
		second.last_station_visited = B;
		second.cargo_type = PAX;
		second.cargo_cap = 100;
		second.lateness_counter = -100;
		PassengerRoutingContext routing(&local, B);
		CHECK(routing.Decide(PAX, D) == PassengerTransferDecision::Transfer);
		DetachPassengerTestVehicle(second);
	}
	SECTION("Assigned shared dispatch departures remain comparable") {
		express.orders->GetOrderAt(0)->SetWaitTime(100);
		express.orders->GetOrderAt(0)->SetDispatchScheduleIndex(0);
		express.orders->GetScheduledDispatchScheduleSet().resize(1);
		express.orders->GetDispatchScheduleByIndex(0).SetScheduledDispatchDuration(1000);
		express.orders->GetDispatchScheduleByIndex(0).SetScheduledDispatchStartTick(NOW);
		express.orders->GetDispatchScheduleByIndex(0).GetScheduledDispatchMutable().push_back({ 100, 0, {} });
		express.vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
		PassengerRoutingContext routing(&local, B);
		CHECK(routing.Decide(PAX, D) == PassengerTransferDecision::Transfer);
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
	SECTION("Mail keeps the original transfer behaviour") {
		CargoSpec::Get(PAX)->classes = CargoClasses{CargoClass::Mail};
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Transfer) == 10);
	}
	SECTION("Manual distribution keeps the original behaviour") {
		_settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Manual;
		PassengerRoutingContext routing(&local, B);
		CHECK(routing.Decide(PAX, D) == PassengerTransferDecision::Unknown);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Transfer) == 10);
	}
	SECTION("No-unload order keeps passengers aboard") {
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::NoUnload, &ge, PAX, &payment, TileIndex{0}, &routing);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Keep) == 10);
	}
	SECTION("Forced unload overrides the prediction") {
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::Unload, &ge, PAX, &payment, TileIndex{0}, &routing);
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
		express.orders->GetOrderAt(0)->SetWaitTime(100); // Even a known fast express must not conceal uncertainty.
		PassengerTestVehicle unknown;
		unknown.orders = OrderList::Create(std::vector<Order>{ Stop(B, 100), Stop(D, 100) }, &unknown);
		unknown.cargo_type = PAX;
		unknown.cargo_cap = 100;
		PassengerRoutingContext routing(&local, B);
		CHECK(routing.Decide(PAX, D) == PassengerTransferDecision::Unknown);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &routing);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Transfer) == 10);
		unknown.orders->FreeChain();
	}
	SECTION("Future refitting capacity cannot be excluded using present zero passenger capacity") {
		PassengerTestVehicle refitting;
		refitting.orders = OrderList::Create(std::vector<Order>{ Stop(B, 100), Stop(D, 100) }, &refitting);
		refitting.orders->GetOrderAt(0)->SetRefit(PAX);
		refitting.cargo_type = PAX;
		refitting.cargo_cap = 0;
		PassengerRoutingContext routing(&local, B);
		CHECK(routing.Decide(PAX, D) == PassengerTransferDecision::Unknown);
		refitting.orders->FreeChain();
	}
	SECTION("A faster connection at an intermediate stop can supersede through riding") {
		local.orders->FreeChain();
		constexpr StationID E{4};
		local.orders = OrderList::Create(std::vector<Order>{ Stop(B, 100), Stop(C, 600), Stop(E, 300), Stop(D, 300) }, &local);
		EnsurePassengerTestStations(local.orders->GetOrderVector());
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
		CHECK(at_c.Decide(PAX, D) == PassengerTransferDecision::Transfer);
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
	SECTION("Explicit OD remains independent of through riding commitments and arrival decisions") {
		AutoRestoreBackup bitnum_backup(CargoSpec::Get(PAX)->bitnum, uint8_t{0});
		REQUIRE(Station::IsValidID(C));
		local.cargo.Truncate();
		CargoPacket *cp = CargoPacket::Create(A, 10, Source{Source::Invalid, SourceType::Industry});
		REQUIRE(cp->SetPassengerDestination(PAX, C));
		cp->UpdateLoadingTile(TileIndex{0});
		local.cargo.Append(cp);
		PassengerRoutingContext at_b(&local, B);
		local.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &at_b);
		REQUIRE(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Keep) == 10);
		const SaveLoad *via_field = nullptr;
		for (const NamedSaveLoad &field : GetCargoPacketDesc()) {
			if (std::string_view(field.name) == "passenger_via") via_field = &field.save_load;
		}
		REQUIRE(via_field != nullptr);
		auto split = std::unique_ptr<CargoPacket>(cp->Split(4));
		REQUIRE(split != nullptr);
		CHECK(ReadValue(GetVariableAddress(split.get(), *via_field), via_field->conv.mem) == D.base());
		CHECK(split->GetPassengerDestination() == C);
		split->UpdateUnloadingTile(TileIndex{0});
		CHECK(ReadValue(GetVariableAddress(split.get(), *via_field), via_field->conv.mem) == StationID::Invalid().base());
		CHECK(split->GetPassengerDestination() == C);
		local.cargo.InvalidateCache();
		local.cargo.KeepAll();
		local.cur_real_order_index = local.cur_implicit_order_index = local.cur_timetable_order_index = 1;
		local.last_station_visited = C;
		PassengerRoutingContext at_c(&local, C);
		std::array next_d{D};
		local.cargo.Stage(true, C, next_d, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &at_c);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Keep) == 6);
		CHECK(cp->GetPassengerDestination() == C);
		local.cur_real_order_index = local.cur_implicit_order_index = local.cur_timetable_order_index = 2;
		local.last_station_visited = D;
		PassengerRoutingContext at_d(&local, D);
		local.cargo.Stage(true, D, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, &at_d);
		CHECK(local.cargo.ActionCount(VehicleCargoList::MoveToAction::Deliver) == 6);
		CHECK(cp->GetPassengerDestination() == C);
	}
	local.cargo.KeepAll();
	local.cargo.Truncate();
	express.cargo.KeepAll();
	express.cargo.Truncate();
	local.orders->FreeChain();
	express.orders->FreeChain();
	DeallocateMap();
}

TEST_CASE("Passenger seat snapshots account for eligible waiting queues and the entire consist", "[passenger]")
{
	AllocateMap(64, 64);
	AutoRestoreBackup settings_backup(_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{});
	AutoRestoreBackup classes_backup(CargoSpec::Get(PAX)->classes, CargoClasses{CargoClass::Passengers});
	AutoRestoreBackup ticks_backup(_state_ticks, NOW);
	_settings_game.linkgraph.passenger_transfer_timetables = true;
	_settings_game.linkgraph.passenger_transfer_penalty = 20;
	_settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Asymmetric;
	REQUIRE(Station::GetNumItems() == 0);
	Station *station = Station::CreateAtIndex(B, TileIndex{0});
	EnsurePassengerTestStations(std::array{Stop(A, 0), Stop(B, 0), Stop(C, 0), Stop(D, 0)});
	PassengerTestVehicle local, express;
	PassengerTestVehicle *coach = nullptr;
	local.orders = OrderList::Create(std::vector<Order>{ Stop(B, 100), Stop(C, 600), Stop(D, 600) }, &local);
	express.orders = OrderList::Create(std::vector<Order>{ Stop(B, 100, 100), Stop(D, 100) }, &express);
	for (Vehicle *v : { &local, &express }) {
		v->current_order.MakeLoading(false);
		v->last_station_visited = B;
		v->cargo_type = PAX;
		v->cargo_cap = 100;
	}
	local.cargo.Append(CargoPacket::Create(A, 10, Source{Source::Invalid, SourceType::Industry}));
	auto wait = [&](StationID via, uint16_t count) {
		station->goods[PAX].CreateData().cargo.Append(CargoPacket::Create(A, count, Source{Source::Invalid, SourceType::Industry}), via);
	};
	PassengerTransferDecision expected = PassengerTransferDecision::Transfer;
	SECTION("Waiting for an unrelated next station does not consume express seats") { wait(A, 1000); }
	SECTION("An eligible waiting queue makes seat availability uncertain") {
		wait(D, 100);
		expected = PassengerTransferDecision::Unknown;
	}
	SECTION("Unrouted waiting passengers are also eligible for loading") {
		wait(StationID::Invalid(), 100);
		expected = PassengerTransferDecision::Unknown;
	}
	SECTION("Enough seats remain after all currently eligible passengers") { wait(D, 80); }
	SECTION("Passenger demand covers all coaches, not just the packet being routed") {
		REQUIRE(Vehicle::GetNumItems() == 0);
		coach = Vehicle::Create<PassengerTestVehicle>(); // SetNext updates the pool's tagged pointer metadata.
		coach->cargo_type = PAX;
		coach->cargo_cap = 100;
		local.SetNext(coach);
		coach->cargo.Append(CargoPacket::Create(A, 10, Source{Source::Invalid, SourceType::Industry}));
		express.cargo_cap = 15;
		expected = PassengerTransferDecision::Unknown;
	}
	GameRandomSeedChecker seeds;
	PassengerRoutingContext routing(&local, B);
	CHECK(routing.Decide(PAX, D) == expected);
	CHECK(routing.Decide(PAX, D) == expected); // Same cache shared by coaches/packets.
	CHECK(seeds.Check());
	local.SetNext(nullptr);
	local.cargo.Truncate();
	if (coach != nullptr) {
		coach->cargo.Truncate();
		_vehicle_pool.CleanPool();
	}
	local.orders->FreeChain();
	express.orders->FreeChain();
	station->goods[PAX].CreateData().cargo.Truncate();
	_station_pool.CleanPool(); // No game world or station GUI exists in this unit-test fixture.
	DeallocateMap();
}

TEST_CASE("Passenger terminal comparisons are bounded and independent of index warmth", "[passenger]")
{
	PassengerTestStations stations;
	EnsurePassengerTestStations(std::array{Stop(A, 0), Stop(B, 0), Stop(C, 0), Stop(D, 0)});
	AutoRestoreBackup settings_backup(_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{});
	AutoRestoreBackup classes_backup(CargoSpec::Get(PAX)->classes, CargoClasses{CargoClass::Passengers});
	AutoRestoreBackup ticks_backup(_state_ticks, NOW);
	_settings_game.linkgraph.passenger_transfer_timetables = true;
	_settings_game.linkgraph.passenger_transfer_penalty = 20;
	_settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Asymmetric;
	PassengerTestVehicle local, express, fleet_head;
	local.orders = OrderList::Create(std::vector<Order>{ Stop(B, 100), Stop(C, 600), Stop(D, 600) }, &local);
	express.orders = OrderList::Create(std::vector<Order>{ Stop(B, 100, 3600), Stop(D, 100) }, &express);
	for (Vehicle *v : { &local, &express, &fleet_head }) {
		v->current_order.MakeLoading(false);
		v->last_station_visited = B;
		v->cargo_type = PAX;
		v->cargo_cap = 100;
	}
	std::deque<PassengerTestVehicle> fleet;
	PassengerTransferDecision expected = PassengerTransferDecision::Unknown;
	SECTION("Relevant fleet exceeds the allowance, even after finding fast connections") {
		fleet_head.orders = OrderList::Create(std::vector<Order>{ Stop(B, 100, 100), Stop(D, 100) }, &fleet_head);
	}
	SECTION("Unrelated large fleet is rejected before per-vehicle prediction") {
		fleet_head.orders = OrderList::Create(std::vector<Order>{ Stop(B, 100, 100), Stop(A, 100) }, &fleet_head);
		expected = PassengerTransferDecision::Stay;
	}
	SECTION("An oversized cold service index also falls back within the allowance") {
		fleet_head.orders = OrderList::Create(std::vector<Order>(32768, Stop(A, 100)), &fleet_head);
	}
	if (fleet_head.orders->GetNumOrders() == 2) {
		for (int i = 0; i < 9000; ++i) {
			auto &v = fleet.emplace_back();
			v.orders = fleet_head.orders;
			v.AddToShared(&fleet_head);
			v.current_order.MakeLoading(false);
			v.last_station_visited = B;
			v.cargo_type = PAX;
			v.cargo_cap = 100;
		}
	}
	InvalidatePassengerServiceIndex();
	GameRandomSeedChecker seeds;
	PassengerRoutingContext cold(&local, B);
	CHECK(cold.Decide(PAX, D) == expected);
	CHECK(cold.Decide(PAX, D) == expected);
	PassengerRoutingContext warm(&local, B);
	CHECK(warm.Decide(PAX, D) == expected);
	CHECK(seeds.Check());
	for (auto &v : fleet) DetachPassengerTestVehicle(v);
	local.orders->FreeChain();
	express.orders->FreeChain();
	fleet_head.orders->FreeChain();
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
