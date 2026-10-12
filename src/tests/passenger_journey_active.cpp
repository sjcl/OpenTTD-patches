/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_journey_active.cpp Production boarding, reservation, staging and persistence regressions. */
#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../passenger_journey_active.h"
#include "../vehicle_base.h"
#include "passenger_test_stations.h"
#include "../station_base.h"
#include "../economy_base.h"
#include "../economy_func.h"
#include "../roadveh.h"
#include "../company_base.h"
#include "../town.h"
#include "../core/backup_type.hpp"
#include "../core/random_func.hpp"
#include "../date_func.h"
#include "../sl/saveload.h"
#include "../linkgraph/linkgraph.h"
#include <deque>

extern std::vector<uint8_t> SlTestSaveLoadTable(void *, void *, const NamedSaveLoadTable &);
extern SaveLoadVersion _sl_version;
static constexpr CargoType PAX{0};
static constexpr StationID A{0}, B{1}, C{2}, D{3};
static constexpr StateTicks NOW{64000};
using Action = VehicleCargoList::MoveToAction;

static Order ActiveStop(StationID station, TimetableTicks travel = 10000, TimetableTicks wait = 100)
{
	Order order;
	order.MakeGoToStation(station);
	order.SetTravelTime(travel); order.SetTravelTimetabled(true);
	order.SetWaitTime(wait); order.SetWaitTimetabled(true);
	return order;
}

struct ActiveTrain : Vehicle {
	ActiveTrain(VehicleID id, std::vector<Order> orders) : ActiveTrain(std::move(orders), id) {}
	explicit ActiveTrain(std::vector<Order> orders, VehicleID id = VehicleID{100}) : Vehicle(id, VehicleType::Train)
	{
		this->orders = OrderList::Create(std::move(orders), this);
		EnsurePassengerTestStations(this->orders->GetOrderVector());
		this->cargo_type = PAX; this->cargo_cap = 100; this->direction = Direction::NE;
		this->LoadAt(0);
	}
	~ActiveTrain() { this->SetNext(nullptr); this->orders->FreeChain(); this->orders = nullptr; }
	bool IsPrimaryVehicle() const override { return true; }
	void LoadAt(VehicleOrderID index)
	{
		this->cur_real_order_index = this->cur_implicit_order_index = index;
		this->cur_timetable_order_index = INVALID_VEH_ORDER_ID;
		this->last_station_visited = this->GetOrder(index)->GetDestination().ToStationID();
		this->current_order.MakeLoading(false); this->current_order_time = 0;
	}
};

struct ActiveFixture {
	AutoRestoreBackup<CargoClasses> classes{CargoSpec::Get(PAX)->classes, CargoClasses{CargoClass::Passengers}};
	AutoRestoreBackup<uint8_t> bitnum{CargoSpec::Get(PAX)->bitnum, uint8_t{0}};
	AutoRestoreBackup<LinkGraphSettings> settings{_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<StateTicks> ticks{_state_ticks, NOW};
	AutoRestoreBackup<Money> rate{CargoSpec::Get(PAX)->current_payment, Money{0}};
	ActiveFixture()
	{
		REQUIRE(Station::GetNumItems() == 0);
		AllocateMap(64, 64);
		for (StationID station : {A, B, C, D}) Station::CreateAtIndex(station, TileIndex{0})->owner = OWNER_NONE;
		_settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Asymmetric;
		_settings_game.linkgraph.passenger_journey_routing = true;
		_settings_game.linkgraph.passenger_od_assignment = false; // Independent from the routing switch.
		_settings_game.linkgraph.passenger_transfer_timetables = false;
		_settings_game.linkgraph.passenger_transfer_penalty = 20;
		BeginPassengerJourneyTick();
	}
	~ActiveFixture() { _station_pool.CleanPool(); _cargopacket_pool.CleanPool(); ClearCargoPacketDeferredPayments(); }
	StationCargoList &Waiting(StationID station = A) { return Station::Get(station)->goods[PAX].CreateData().cargo; }
	CargoPacket *Packet(uint16_t count = 10, StationID destination = D, uint16_t age = 7)
	{
		CargoPacket *packet = CargoPacket::Create(count, age, A, TileIndex{0}, Money{100});
		if (destination != StationID::Invalid()) REQUIRE(packet->SetPassengerDestination(PAX, destination));
		return packet;
	}
	void Plan(CargoPacket &packet, StationID station = A, StationID legacy = B)
	{
		PassengerJourneyActiveContext context(nullptr, station);
		REQUIRE(context.PlanWaiting(packet, PAX, legacy));
	}
};

struct ActiveCoach : Vehicle {
	explicit ActiveCoach(VehicleID id) : Vehicle(id, VehicleType::Train) { this->cargo_type = PAX; this->cargo_cap = 100; }
	bool IsPrimaryVehicle() const override { return false; }
};

struct AttachedCoach {
	ActiveTrain &front;
	ActiveCoach *coach = Vehicle::Create<ActiveCoach>();
	explicit AttachedCoach(ActiveTrain &front) : front(front) { front.SetNext(this->coach); }
	~AttachedCoach() { this->front.SetNext(nullptr); delete this->coach; }
};

struct FinalDeliveryFixture {
	Company *company = Company::Create();
	Town *town = Town::Create(TileIndex{0});
	FinalDeliveryFixture() { Station::Get(D)->town = this->town; Station::Get(D)->always_accepted.Set(PAX); }
	~FinalDeliveryFixture() { Station::Get(D)->town = nullptr; _town_pool.CleanPool(); _company_pool.CleanPool(); }
};

struct FallbackLinks {
	LinkGraph *graph = LinkGraph::Create(PAX);
	FallbackLinks()
	{
		for (StationID id : {A, B, C, D}) {
			Station *station = Station::Get(id);
			station->goods[PAX].link_graph = this->graph->index;
			station->goods[PAX].node = this->graph->AddNode(station);
		}
	}
	~FallbackLinks()
	{
		for (StationID id : {A, B, C, D}) {
			Station::Get(id)->goods[PAX].link_graph = LinkGraphID::Invalid();
			Station::Get(id)->goods[PAX].node = INVALID_NODE;
		}
		_link_graph_pool.CleanPool();
	}
	void Edge(StationID from, StationID to, bool restricted = false)
	{
		graph->UpdateEdge(Station::Get(from)->goods[PAX].node, Station::Get(to)->goods[PAX].node,
				100, 0, 100, {EdgeUpdateMode::Refresh, restricted ? EdgeUpdateMode::Restricted : EdgeUpdateMode::Unrestricted});
	}
};

/** A zero-capacity real road vehicle exercises the complete station countdown
 * and waiting pass without railway geometry or engine/cargo loading setup. */
static RoadVehicle *DueLoadingVehicle(StationID station, uint ticks = 1)
{
	RoadVehicle *vehicle = Vehicle::Create<RoadVehicle>();
	vehicle->last_station_visited = station;
	vehicle->current_order.MakeLoading(false);
	vehicle->load_unload_ticks = ticks;
	vehicle->tile = TileIndex{0};
	Station::Get(station)->loading_vehicles.push_back(vehicle);
	return vehicle;
}

static void CleanLoadingVehicles()
{
	for (Station *station : Station::Iterate()) station->loading_vehicles.clear();
	_vehicle_pool.CleanPool();
}

static void ExhaustJourneyAdmission(ActiveFixture &fixture)
{
	for (uint i = 0; i < 40; ++i) {
		auto packet = std::unique_ptr<CargoPacket>(fixture.Packet(1));
		PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*packet, PAX, D);
	}
	CHECK(GetPassengerJourneyTickDiagnostic().query_work >= 15 * 65536);
}

TEST_CASE("Journey boarding selects a physical train even with the same next stop", "[passenger][active]")
{
	ActiveFixture fixture;
	ActiveTrain local({ActiveStop(A), ActiveStop(B, 200), ActiveStop(D, 200)});
	ActiveTrain express({ActiveStop(A, 10000, 120), ActiveStop(B, 20), ActiveStop(D, 20)}, VehicleID{101});
	auto *packet = fixture.Packet();
	fixture.Plan(*packet);
	REQUIRE(packet->GetPassengerJourney().vehicle == express.index);
	fixture.Waiting().Append(packet, B);
	std::array next{B};
	CHECK_FALSE(fixture.Waiting().HasCargoFor(next, &local));
	CHECK(fixture.Waiting().Load(100, &local.cargo, next, TileIndex{0}, &local) == 0);
	CHECK(fixture.Waiting().Reserve(100, &local.cargo, next, TileIndex{0}, &local) == 0);
	CHECK(fixture.Waiting().HasCargoFor(next, &express));
	CHECK(fixture.Waiting().Load(100, &express.cargo, next, TileIndex{0}, &express) == 10);
	CHECK(express.cargo.StoredCount() == 10);
}

TEST_CASE("Active horizons certify short frequent journeys and extend long ODs", "[passenger][active][scaling]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A, 10, 1), ActiveStop(D, 10, 1)});
	SECTION("High frequency loading and repeated packets") {
		ResetPassengerJourneyDiagnostic();
		PassengerJourneyActiveContext context(nullptr, A);
		for (uint i = 0; i < 4096; ++i) {
			auto *packet = fixture.Packet(1);
			REQUIRE(context.PlanWaiting(*packet, PAX, D));
			REQUIRE(packet->GetPassengerJourney().arrival == NOW + 11);
			fixture.Waiting().Append(packet, D);
		}
		const auto diagnostic = GetPassengerJourneyTickDiagnostic();
		CHECK(diagnostic.builds == 1); CHECK(diagnostic.queries == 1);
		CHECK(diagnostic.query_cache_hits == 4095);
		CHECK(diagnostic.visits > 128);
		std::array next{D};
		CHECK(fixture.Waiting().Load(100, &train.cargo, next, TileIndex{0}, &train) == 100);
		CHECK(GetPassengerJourneyTickDiagnostic().boarded_passengers == 100);
	}
	SECTION("Long destination extends only after a certified horizon miss") {
		train.GetOrder(1)->SetTravelTime(20000);
		ResetPassengerJourneyDiagnostic();
		auto *packet = fixture.Packet(); fixture.Plan(*packet);
		CHECK(packet->GetPassengerJourney().arrival == NOW + 20001);
		CHECK(GetPassengerJourneyTickDiagnostic().builds == 3);
		CHECK(GetPassengerJourneyTickDiagnostic().queries == 3);
	}
	SECTION("Unknown timetable cannot be certified by a partial path") {
		train.GetOrder(1)->SetWaitTimetabled(false);
		PassengerJourneyActiveContext context(nullptr, A);
		auto *packet = fixture.Packet();
		CHECK_FALSE(context.PlanWaiting(*packet, PAX, D));
		CHECK(packet->GetPassengerRoutingMode() != PassengerRoutingMode::Journey);
	}
	SECTION("A later unknown stop cannot affect the certified earlier optimum") {
		Order unknown = ActiveStop(B, 10000, 10); unknown.SetWaitTimetabled(false);
		train.orders->FreeChain();
		train.orders = OrderList::Create(std::vector<Order>{ActiveStop(A, 10000, 10), ActiveStop(D, 100, 10), unknown}, &train);
		EnsurePassengerTestStations(train.orders->GetOrderVector());
		auto *packet = fixture.Packet(); fixture.Plan(*packet);
		CHECK(packet->GetPassengerJourney().arrival == NOW + 110);
	}
}

TEST_CASE("A short horizon cannot hide a cheaper later direct service", "[passenger][active][scaling]")
{
	ActiveFixture fixture;
	_settings_game.linkgraph.passenger_transfer_penalty = 2000;
	ActiveTrain feeder({ActiveStop(A, 100000, 10), ActiveStop(B, 100, 10)});
	ActiveTrain connection({ActiveStop(B, 100000, 2200), ActiveStop(D, 100, 10)}, VehicleID{101});
	ActiveTrain direct({ActiveStop(A, 100000, 4100), ActiveStop(D, 100, 10)}, VehicleID{102});
	auto *packet = fixture.Packet(); fixture.Plan(*packet);
	CHECK(packet->GetPassengerJourney().vehicle == direct.index);
}

TEST_CASE("A chosen ordinary train boards and partial loading preserves OD and money", "[passenger][active]")
{
	ActiveFixture fixture;
	ActiveTrain local({ActiveStop(A), ActiveStop(D, 100)});
	auto *packet = fixture.Packet(300);
	fixture.Plan(*packet);
	fixture.Waiting().Append(packet, D);
	std::array next{D};
	REQUIRE(fixture.Waiting().Load(100, &local.cargo, next, TileIndex{0}, &local) == 100);
	CHECK(fixture.Waiting().AvailableCount() == 200);
	CHECK(local.cargo.StoredCount() == 100);
	const CargoPacket *loaded = local.cargo.Packets()->front();
	CHECK(loaded->GetPassengerDestination() == D);
	CHECK(loaded->GetPassengerJourney() == packet->GetPassengerJourney());
	CHECK(loaded->GetFeederShare() + packet->GetFeederShare() == 100);
	CHECK(loaded->Count() + packet->Count() == 300);
	CHECK(CountPassengerRoutingModes(fixture.Waiting())[1] == 200);
}

TEST_CASE("Reservations and gradual loading allocate seats only once", "[passenger][active]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	AttachedCoach attached(train);
	Vehicle &coach = *attached.coach;
	auto *packet = fixture.Packet(300); fixture.Plan(*packet); fixture.Waiting().Append(packet, D);
	std::array next{D};
	PassengerJourneyLoadingContext loading;
	ResetPassengerJourneyDiagnostic();
	CHECK(fixture.Waiting().HasCargoFor(next, &train, &loading));
	CHECK(fixture.Waiting().Reserve(100, &train.cargo, next, TileIndex{0}, &train, &loading) == 100);
	CHECK(fixture.Waiting().Reserve(100, &coach.cargo, next, TileIndex{0}, &coach, &loading) == 100);
	CHECK(fixture.Waiting().AvailableCount() == 100);
	CHECK(fixture.Waiting().ReservedCount() == 200);
	CHECK(GetPassengerJourneyTickDiagnostic().boarded_passengers == 0);
	for (uint i = 0; i < 5; ++i) {
		CHECK(fixture.Waiting().Load(20, &train.cargo, next, TileIndex{0}, &train, &loading) == 20);
		CHECK(fixture.Waiting().Load(20, &coach.cargo, next, TileIndex{0}, &coach, &loading) == 20);
	}
	CHECK(train.cargo.StoredCount() == 100); CHECK(coach.cargo.StoredCount() == 100);
	CHECK(fixture.Waiting().ReservedCount() == 0);
	CHECK(fixture.Waiting().AvailableCount() == 100);
	CHECK(GetPassengerJourneyTickDiagnostic().boarded_passengers == 200);
	CHECK(loading.GetPredictionCount() == 1);
	train.SetNext(nullptr);
}

TEST_CASE("A stale gradual-loading reservation is returned with consistent counts", "[passenger][active]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	auto *packet = fixture.Packet(); fixture.Plan(*packet); fixture.Waiting().Append(packet, D);
	std::array next{D};
	REQUIRE(fixture.Waiting().Reserve(100, &train.cargo, next, TileIndex{0}, &train) == 10);
	train.GetOrder(0)->SetWaitTime(200);
	CHECK(fixture.Waiting().Load(20, &train.cargo, next, TileIndex{0}, &train) == 0);
	CHECK(train.cargo.ReservedCount() == 0);
	CHECK(fixture.Waiting().ReservedCount() == 0);
	CHECK(fixture.Waiting().AvailableCount() == 10);
}

TEST_CASE("Onboard journey stages keep transfer and final delivery in production", "[passenger][active]")
{
	ActiveFixture fixture;
	ActiveTrain local({ActiveStop(B), ActiveStop(C, 100), ActiveStop(D, 100)});
	ActiveTrain express({ActiveStop(B, 10000, 500), ActiveStop(D, 40)}, VehicleID{101});
	auto *packet = fixture.Packet(); fixture.Plan(*packet, B);
	local.cargo.Append(packet);
	Action expected = Action::Keep;
	SECTION("Continue ordinary train when faster") {}
	SECTION("Transfer to a faster express") { express.GetOrder(0)->SetWaitTime(50); expected = Action::Transfer; }
	SECTION("High frequency trunk beats the low frequency shortcut") { local.GetOrder(0)->SetWaitTime(600); expected = Action::Transfer; }
	SECTION("Do not deliver at an intermediate CargoDist via") {}
	SECTION("Deliver at the accepted final destination") { local.LoadAt(2); expected = Action::Deliver; }
	SECTION("Unaccepted final destination falls back without deleting passengers") { local.LoadAt(2); expected = Action::Keep; }
	StationID station = local.last_station_visited;
	GoodsEntry &ge = Station::Get(station)->goods[PAX];
	ge.CreateData().flows.insert(FlowStat(A, station, 10)); // CargoDist would deliver here.
	CargoPayment payment(CargoPaymentID::Invalid(), &local);
	std::array next{C};
	PassengerJourneyActiveContext context(&local, station);
	bool accepted = true;
	/* The section changes acceptance only for the final no-acceptance case. */
	if (station == D && expected == Action::Keep) accepted = false;
	local.cargo.Stage(accepted, station, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, nullptr, nullptr, &context);
	CHECK(local.cargo.ActionCount(expected) == 10);
	CHECK(local.cargo.TotalCount() == 10);
	CHECK(packet->GetPassengerDestination() == D);
	if (expected == Action::Transfer) {
		ResetPassengerJourneyDiagnostic();
		CHECK(packet->GetPassengerJourney().vehicle == express.index);
		REQUIRE(local.cargo.Unload(10, &fixture.Waiting(B), PAX, &payment, TileIndex{0}) == 10);
		CHECK(GetPassengerJourneyTickDiagnostic().transferred_passengers == 10);
		CHECK(fixture.Waiting(B).Load(10, &local.cargo, next, TileIndex{0}, &local) == 0);
		std::array express_next{D};
		CHECK(fixture.Waiting(B).Load(10, &express.cargo, express_next, TileIndex{0}, &express) == 10);
		CHECK(GetPassengerJourneyTickDiagnostic().boarded_passengers == 10);
		/* Run the selected connection through the real final-delivery action too. */
		FinalDeliveryFixture delivery;
		express.owner = delivery.company->index;
		express.LoadAt(1);
		CargoPayment final_payment(CargoPaymentID::Invalid(), &express);
		PassengerJourneyActiveContext final_context(&express, D);
		express.cargo.Stage(true, D, next, OrderUnloadType::UnloadIfPossible, &Station::Get(D)->goods[PAX], PAX,
				&final_payment, TileIndex{0}, nullptr, nullptr, &final_context);
		CHECK(express.cargo.ActionCount(Action::Deliver) == 10);
		CHECK(express.cargo.Unload(10, &fixture.Waiting(D), PAX, &final_payment, TileIndex{0}) == 10);
		CHECK(GetPassengerJourneyTickDiagnostic().delivered_passengers == 10);
		CHECK(express.cargo.TotalCount() == 0);
		CHECK(fixture.Waiting(B).TotalCount() == 0);
		CHECK(fixture.Waiting(D).TotalCount() == 0);
		CHECK(delivery.company->cur_economy.delivered_cargo[PAX] == 10);
	}
}

TEST_CASE("Exhausted searches retain valid onboard rides and cannot deliver at intermediate stations", "[passenger][active][budget][delivery]")
{
	ActiveFixture fixture;
	FinalDeliveryFixture delivery;
	ActiveTrain train({ActiveStop(A), ActiveStop(B, 100), ActiveStop(C, 100), ActiveStop(D, 100)});
	train.owner = delivery.company->index;
	bool valid = true, changed_timetable = false, changed_occurrence = false, no_load = false;
	OrderUnloadType unload = OrderUnloadType::UnloadIfPossible;
	SECTION("Normal continuation") {}
	SECTION("NoLoad still permits existing passengers to stay") { train.GetOrder(1)->SetLoadType(OrderLoadType::NoLoad); no_load = true; }
	SECTION("Changed timetable invalidates the saved ride") { valid = false; changed_timetable = true; }
	SECTION("Changed occurrence invalidates the saved ride") { valid = false; changed_occurrence = true; }
	SECTION("Forced transfer is respected") { valid = false; unload = OrderUnloadType::Transfer; }
	auto *packet = fixture.Packet(); fixture.Plan(*packet); train.cargo.Append(packet);
	const auto saved = packet->GetPassengerJourney();
	train.LoadAt(1); _state_ticks = NOW + 200;
	if (no_load) train.current_order.SetLoadType(OrderLoadType::NoLoad);
	if (changed_timetable) train.GetOrder(3)->SetTravelTime(200);
	if (changed_occurrence) train.current_order_time = 10;
	BeginPassengerJourneyTick(); ExhaustJourneyAdmission(fixture);
	GoodsEntry &goods = Station::Get(B)->goods[PAX];
	goods.CreateData().flows.insert(FlowStat(A, B, 10));
	CargoPayment payment(CargoPaymentID::Invalid(), &train);
	std::array next{C};
	PassengerJourneyActiveContext context(&train, B);
	train.cargo.Stage(true, B, next, unload, &goods, PAX, &payment, TileIndex{0}, nullptr, nullptr, &context);
	CHECK(train.cargo.ActionCount(Action::Deliver) == 0);
	const bool transfer = unload == OrderUnloadType::Transfer;
	CHECK(train.cargo.ActionCount(transfer ? Action::Transfer : Action::Keep) == 10);
	CHECK(packet->GetPassengerDestination() == D);
	CHECK(packet->GetFeederShare() == 100);
	CHECK(delivery.company->cur_economy.delivered_cargo[PAX] == 0);
	if (valid) {
		CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
		CHECK(packet->GetPassengerJourney() == saved);
		CHECK(GetPassengerJourneyTickDiagnostic().retained_rides == 1);
		train.LoadAt(3); _state_ticks = saved.arrival;
		CargoPayment final_payment(CargoPaymentID::Invalid(), &train);
		PassengerJourneyActiveContext final_context(&train, D);
		train.cargo.Stage(true, D, next, OrderUnloadType::UnloadIfPossible, &Station::Get(D)->goods[PAX], PAX,
				&final_payment, TileIndex{0}, nullptr, nullptr, &final_context);
		CHECK(train.cargo.ActionCount(Action::Deliver) == 10);
		CHECK(train.cargo.Unload(10, &fixture.Waiting(D), PAX, &final_payment, TileIndex{0}) == 10);
		CHECK(delivery.company->cur_economy.delivered_cargo[PAX] == 10);
	} else {
		CHECK(packet->GetPassengerRoutingMode() != PassengerRoutingMode::Journey);
		CHECK(train.cargo.Unload(10, &fixture.Waiting(B), PAX, &payment, TileIndex{0}) == (transfer ? 10 : 0));
		CHECK(fixture.Waiting(B).AvailableCount() == (transfer ? 10 : 0));
		CHECK(packet->GetPassengerDestination() == D);
		CHECK(delivery.company->cur_economy.delivered_cargo[PAX] == 0);
	}
}

TEST_CASE("Fallback and deferred OD passengers stay on a direct ride through CargoDist destinations", "[passenger][active][delivery][fallback]")
{
	ActiveFixture fixture;
	FinalDeliveryFixture delivery;
	ActiveTrain train({ActiveStop(B), ActiveStop(C), ActiveStop(D)});
	train.owner = delivery.company->index;
	auto *packet = fixture.Packet();
	SECTION("Ordinary fallback") { packet->FallbackPassengerJourney(A, NOW); }
	SECTION("Deferred budget wait") { packet->DeferPassengerJourney(A, NOW); }
	SECTION("Legacy with a known OD") {}
	train.cargo.Append(packet);
	std::array next{D};
	for (VehicleOrderID index : {VehicleOrderID{0}, VehicleOrderID{1}}) {
		train.LoadAt(index);
		const StationID station = train.last_station_visited;
		GoodsEntry &goods = Station::Get(station)->goods[PAX];
		goods.CreateData().flows.insert(FlowStat(A, station, 10)); // CargoDist says Deliver even when accepted.
		CargoPayment payment(CargoPaymentID::Invalid(), &train);
		for (uint pass = 0; pass < 3; ++pass) {
			train.cargo.Stage(true, station, next, OrderUnloadType::UnloadIfPossible, &goods, PAX, &payment, TileIndex{0});
			CHECK(train.cargo.ActionCount(Action::Deliver) == 0);
			CHECK(train.cargo.ActionCount(Action::Transfer) == 0);
			CHECK(train.cargo.ActionCount(Action::Keep) == 10);
			CHECK(train.cargo.Unload(10, &fixture.Waiting(station), PAX, &payment, TileIndex{0}) == 0);
			CHECK(fixture.Waiting(station).AvailableCount() == 0);
			CHECK(fixture.Waiting(station).Load(10, &train.cargo, next, TileIndex{0}, &train) == 0);
		}
		CHECK(packet->GetPassengerDestination() == D);
		CHECK(packet->GetFeederShare() == 100);
		CHECK(delivery.company->cur_economy.delivered_cargo[PAX] == 0);
	}
	train.LoadAt(2);
	GoodsEntry &goods = Station::Get(D)->goods[PAX];
	goods.CreateData().flows.insert(FlowStat(A, B, 10)); // A stale CargoDist flow would transfer it away again.
	CargoPayment payment(CargoPaymentID::Invalid(), &train);
	train.cargo.Stage(true, D, next, OrderUnloadType::UnloadIfPossible, &goods, PAX, &payment, TileIndex{0});
	CHECK(train.cargo.ActionCount(Action::Deliver) == 10);
	CHECK(train.cargo.Unload(10, &fixture.Waiting(D), PAX, &payment, TileIndex{0}) == 10);
	CHECK(train.cargo.TotalCount() == 0);
	CHECK(delivery.company->cur_economy.delivered_cargo[PAX] == 10);
}

TEST_CASE("OD fallback chooses a progressing interchange instead of unrelated CargoDist flows", "[passenger][active][fallback]")
{
	ActiveFixture fixture;
	FinalDeliveryFixture delivery;
	FallbackLinks links;
	links.Edge(B, A); links.Edge(A, B); // A cycle and an accepting dead end must not attract the OD.
	links.Edge(B, C); links.Edge(C, D);
	ActiveTrain wrong({ActiveStop(B), ActiveStop(A)});
	ActiveTrain onward({ActiveStop(B), ActiveStop(C)}, VehicleID{101});
	ActiveTrain last({ActiveStop(C), ActiveStop(D)}, VehicleID{102});
	wrong.owner = onward.owner = last.owner = delivery.company->index;
	auto *packet = fixture.Packet();
	packet->FallbackPassengerJourney(B, NOW);
	wrong.cargo.Append(packet);
	GoodsEntry &goods = Station::Get(B)->goods[PAX]; goods.CreateData().flows.insert(FlowStat(A, B, 10));
	std::array wrong_next{A}, onward_next{C}, last_next{D};
	GameRandomSeedChecker seeds;
	CargoPayment payment(CargoPaymentID::Invalid(), &wrong);
	PassengerJourneyActiveContext context(&wrong, B);
	wrong.cargo.Stage(true, B, wrong_next, OrderUnloadType::UnloadIfPossible, &goods, PAX, &payment, TileIndex{0}, nullptr, nullptr, &context);
	REQUIRE(wrong.cargo.ActionCount(Action::Transfer) == 10);
	REQUIRE(wrong.cargo.Unload(10, &fixture.Waiting(B), PAX, &payment, TileIndex{0}) == 10);
	PassengerJourneyLoadingContext loading;
	CHECK_FALSE(fixture.Waiting(B).HasCargoFor(wrong_next, &wrong, &loading));
	CHECK(fixture.Waiting(B).Reserve(10, &wrong.cargo, wrong_next, TileIndex{0}, &wrong, &loading) == 0);
	CHECK(fixture.Waiting(B).Load(10, &wrong.cargo, wrong_next, TileIndex{0}, &wrong, &loading) == 0);
	REQUIRE(fixture.Waiting(B).Reserve(10, &onward.cargo, onward_next, TileIndex{0}, &onward, &loading) == 10);
	REQUIRE(fixture.Waiting(B).Load(10, &onward.cargo, onward_next, TileIndex{0}, &onward, &loading) == 10);
	CHECK(packet->GetPassengerVia() == C);
	onward.LoadAt(1);
	CargoPayment transfer(CargoPaymentID::Invalid(), &onward);
	onward.cargo.Stage(true, C, wrong_next, OrderUnloadType::UnloadIfPossible, &Station::Get(C)->goods[PAX], PAX, &transfer, TileIndex{0});
	REQUIRE(onward.cargo.ActionCount(Action::Transfer) == 10);
	REQUIRE(onward.cargo.Unload(10, &fixture.Waiting(C), PAX, &transfer, TileIndex{0}) == 10);
	CHECK(fixture.Waiting(C).Load(10, &onward.cargo, wrong_next, TileIndex{0}, &onward) == 0);
	REQUIRE(fixture.Waiting(C).Load(10, &last.cargo, last_next, TileIndex{0}, &last) == 10);
	last.LoadAt(1);
	CargoPayment final_payment(CargoPaymentID::Invalid(), &last);
	last.cargo.Stage(true, D, wrong_next, OrderUnloadType::UnloadIfPossible, &Station::Get(D)->goods[PAX], PAX, &final_payment, TileIndex{0});
	CHECK(last.cargo.Unload(10, &fixture.Waiting(D), PAX, &final_payment, TileIndex{0}) == 10);
	CHECK(delivery.company->cur_economy.delivered_cargo[PAX] == 10);
	CHECK(seeds.Check());
}

TEST_CASE("Unreachable OD fallback waits without reboarding a circulating train and can recover", "[passenger][active][fallback]")
{
	ActiveFixture fixture;
	FallbackLinks links; links.Edge(B, C); links.Edge(C, B); links.Edge(C, D);
	ActiveTrain train({ActiveStop(B), ActiveStop(C)});
	auto *packet = fixture.Packet(); packet->FallbackPassengerJourney(B, NOW); train.cargo.Append(packet);
	links.graph->RemoveEdge(Station::Get(C)->goods[PAX].node, Station::Get(D)->goods[PAX].node);
	std::array next{C};
	CargoPayment payment(CargoPaymentID::Invalid(), &train);
	train.cargo.Stage(true, B, next, OrderUnloadType::UnloadIfPossible, &Station::Get(B)->goods[PAX], PAX, &payment, TileIndex{0});
	REQUIRE(train.cargo.Unload(10, &fixture.Waiting(B), PAX, &payment, TileIndex{0}) == 10);
	for (uint pass = 0; pass < 5; ++pass) {
		_state_ticks += 1; BeginPassengerJourneyTick();
		PassengerJourneyLoadingContext loading;
		fixture.Waiting(B).RoutePassengerJourneys(PAX, B, true, &loading);
		CHECK_FALSE(fixture.Waiting(B).HasCargoFor(next, &train, &loading));
		CHECK(fixture.Waiting(B).Reserve(10, &train.cargo, next, TileIndex{0}, &train, &loading) == 0);
		CHECK(fixture.Waiting(B).Load(10, &train.cargo, next, TileIndex{0}, &train, &loading) == 0);
		CHECK(train.cargo.TotalCount() == 0);
		CHECK(fixture.Waiting(B).AvailableCount() == 10);
		CHECK(packet->GetPassengerDestination() == D);
		CHECK(packet->GetFeederShare() == 100);
	}
	links.Edge(C, D);
	PassengerJourneyLoadingContext restored;
	REQUIRE(fixture.Waiting(B).Load(10, &train.cargo, next, TileIndex{0}, &train, &restored) == 10);
	CHECK(packet->GetPassengerVia() == C);
}

TEST_CASE("OD fallback boarding validates live prefixes and restricted links", "[passenger][active][fallback]")
{
	ActiveFixture fixture;
	FallbackLinks links; links.Edge(B, C); links.Edge(C, D);
	ActiveTrain train({ActiveStop(B), ActiveStop(C)});
	auto *packet = fixture.Packet(300); packet->FallbackPassengerJourney(B, NOW);
	fixture.Waiting(B).Append(packet, StationID::Invalid());
	std::array next{C};
	SECTION("Restricted onward link cannot certify an interchange") {
		links.graph->RemoveEdge(2, 3); links.Edge(C, D, true);
		CHECK(fixture.Waiting(B).Load(10, &train.cargo, next, TileIndex{0}, &train) == 0);
	}
	SECTION("Stale station node identity cannot grant OD boarding") {
		links.Edge(A, C);
		Station::Get(B)->goods[PAX].node = Station::Get(A)->goods[PAX].node;
		CHECK(fixture.Waiting(B).Load(10, &train.cargo, next, TileIndex{0}, &train) == 0);
	}
	SECTION("A changed route returns reserved cargo and blocks repeated reloads") {
		PassengerJourneyLoadingContext loading;
		REQUIRE(fixture.Waiting(B).Reserve(100, &train.cargo, next, TileIndex{0}, &train, &loading) == 100);
		train.GetOrder(1)->MakeGoToStation(A); loading.Invalidate(&train);
		CHECK(fixture.Waiting(B).Load(100, &train.cargo, next, TileIndex{0}, &train, &loading) == 0);
		CHECK(fixture.Waiting(B).ReservedCount() == 0);
		CHECK(fixture.Waiting(B).AvailableCount() == 300);
		CHECK(train.cargo.TotalCount() == 0);
	}
	SECTION("Untimed but deterministic orders still transport OD fallback") {
		train.GetOrder(0)->SetWaitTimetabled(false);
		REQUIRE(fixture.Waiting(B).Load(100, &train.cargo, next, TileIndex{0}, &train) == 100);
		CHECK(train.cargo.StoredCount() == 100); CHECK(fixture.Waiting(B).AvailableCount() == 200);
		CHECK(train.cargo.Packets()->front()->GetPassengerVia() == C);
		CHECK(train.cargo.Packets()->front()->GetFeederShare() + packet->GetFeederShare() == 100);
	}
	SECTION("Conditional path cannot certify reaching the OD hop") {
		train.GetOrder(1)->MakeConditional(0);
		CHECK(fixture.Waiting(B).Load(10, &train.cargo, next, TileIndex{0}, &train) == 0);
	}
	SECTION("Forced interchange cannot immediately reload the same passengers") {
		train.current_order.SetUnloadType(OrderUnloadType::Transfer);
		CHECK(fixture.Waiting(B).Reserve(10, &train.cargo, next, TileIndex{0}, &train) == 0);
		CHECK(fixture.Waiting(B).Load(10, &train.cargo, next, TileIndex{0}, &train) == 0);
	}
}

TEST_CASE("OD fallback work is shared bounded deterministic and reusable until the next tick", "[passenger][active][fallback][budget]")
{
	ActiveFixture fixture;
	std::vector<Order> orders(4096);
	orders.front() = ActiveStop(B); orders.back() = ActiveStop(D);
	for (size_t i = 1; i + 1 < orders.size(); ++i) orders[i].MakeGoToWaypoint(C);
	ActiveTrain train(std::move(orders));
	auto *packet = fixture.Packet(); packet->FallbackPassengerJourney(B, NOW);
	fixture.Waiting(B).Append(packet, StationID::Invalid());
	GameRandomSeedChecker seeds;
	for (uint i = 0; i < 64; ++i) {
		PassengerJourneyLoadingContext loading;
		CHECK(loading.CanBoard(*packet, &train));
		size_t used = GetPassengerJourneyTickDiagnostic().od_fallback_work;
		CHECK(loading.CanBoard(*packet, &train)); // Same event reuses the exact result without charging again.
		CHECK(GetPassengerJourneyTickDiagnostic().od_fallback_work == used);
	}
	PassengerJourneyLoadingContext exhausted;
	std::array next{D};
	CHECK(fixture.Waiting(B).Reserve(10, &train.cargo, next, TileIndex{0}, &train, &exhausted) == 0);
	CHECK(fixture.Waiting(B).Load(10, &train.cargo, next, TileIndex{0}, &train, &exhausted) == 0);
	CHECK(GetPassengerJourneyTickDiagnostic().od_fallback_work == 4 * 1024 * 1024);
	CHECK(fixture.Waiting(B).AvailableCount() == 10);
	_state_ticks += 1; BeginPassengerJourneyTick();
	CHECK(fixture.Waiting(B).Load(10, &train.cargo, next, TileIndex{0}, &train) == 10);
	CHECK(packet->GetPassengerDestination() == D);
	CHECK(seeds.Check());
}

TEST_CASE("Destination enforcement preserves disabled and unknown OD CargoDist delivery", "[passenger][active][delivery]")
{
	ActiveFixture fixture;
	FinalDeliveryFixture delivery;
	Station::Get(B)->town = delivery.town; Station::Get(B)->always_accepted.Set(PAX);
	ActiveTrain train({ActiveStop(B), ActiveStop(D)}); train.owner = delivery.company->index;
	auto *packet = fixture.Packet();
	SECTION("Known OD with routing disabled") { _settings_game.linkgraph.passenger_journey_routing = false; }
	SECTION("Unknown OD with routing enabled") { delete packet; packet = fixture.Packet(10, StationID::Invalid()); }
	train.cargo.Append(packet);
	GoodsEntry &goods = Station::Get(B)->goods[PAX]; goods.CreateData().flows.insert(FlowStat(A, B, 10));
	CargoPayment payment(CargoPaymentID::Invalid(), &train);
	std::array next{D};
	train.cargo.Stage(true, B, next, OrderUnloadType::UnloadIfPossible, &goods, PAX, &payment, TileIndex{0});
	CHECK(train.cargo.ActionCount(Action::Deliver) == 10);
	CHECK(train.cargo.Unload(10, &fixture.Waiting(B), PAX, &payment, TileIndex{0}) == 10);
	CHECK(delivery.company->cur_economy.delivered_cargo[PAX] == 10);
	Station::Get(B)->town = nullptr;
}

TEST_CASE("Journey failure uses validated OD fallback while unsupported passengers retain CargoDist", "[passenger][active]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	bool fallback = true, can_board = true;
	std::optional<ActiveTrain> uncertain;
	auto *packet = fixture.Packet();
	SECTION("Missing destination route") { train.GetOrder(1)->MakeGoToStation(B); can_board = false; }
	SECTION("Incomplete timetable") { train.GetOrder(0)->SetWaitTimetabled(false); }
	SECTION("A known path cannot hide an incomplete alternative") {
		Order untimed = ActiveStop(A); untimed.SetWaitTimetabled(false);
		uncertain.emplace(std::vector<Order>{untimed, ActiveStop(D, 50)}, VehicleID{101});
	}
	SECTION("Conditional service") { train.GetOrder(1)->MakeConditional(0); can_board = false; }
	SECTION("Manual distribution") { _settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Manual; fallback = false; }
	SECTION("Unknown OD") { delete packet; packet = fixture.Packet(10, StationID::Invalid()); fallback = false; }
	SECTION("Disabled") { _settings_game.linkgraph.passenger_journey_routing = false; fallback = false; }
	SECTION("Unsupported vehicle types cannot certify OD boarding") { train.type = VehicleType::Effect; can_board = false; }
	fixture.Waiting().Append(packet, D);
	fixture.Waiting().RoutePassengerJourneys(PAX, A);
	CHECK(packet->GetPassengerRoutingMode() == (fallback ? PassengerRoutingMode::Fallback : PassengerRoutingMode::Legacy));
	std::array next{D};
	CHECK(fixture.Waiting().Load(10, &train.cargo, next, TileIndex{0}, &train) == (can_board ? 10 : 0));
}

TEST_CASE("Missed departure invalidates a leg and cooldown prevents routing oscillation", "[passenger][active]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	auto *packet = fixture.Packet(); fixture.Plan(*packet); fixture.Waiting().Append(packet, B);
	_state_ticks += 256;
	fixture.Waiting().RoutePassengerJourneys(PAX, A);
	CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Fallback);
	CHECK(packet->GetPassengerDestination() == D);
	CHECK_FALSE(packet->CanRetryPassengerJourney(A, _state_ticks));
	CHECK(packet->CanRetryPassengerJourney(B, _state_ticks));
	_state_ticks += 256;
	CHECK(packet->CanRetryPassengerJourney(A, _state_ticks));
	std::array next{B};
	CHECK(fixture.Waiting().Load(10, &train.cargo, next, TileIndex{0}, &train) == 10);
}

TEST_CASE("Journey merge checks include mode and the executable leg", "[passenger][active]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	auto first = std::unique_ptr<CargoPacket>(fixture.Packet()); fixture.Plan(*first);
	auto split = std::unique_ptr<CargoPacket>(first->Split(4));
	REQUIRE(split != nullptr);
	CHECK(StationCargoList::AreMergable(first.get(), split.get()));
	SECTION("Same execution can merge") { REQUIRE(first->Merge(split.release())); CHECK(first->Count() == 10); }
	SECTION("Fallback cannot merge into journey") { split->FallbackPassengerJourney(A, NOW); CHECK_FALSE(first->Merge(split.get())); }
	SECTION("Different physical occurrence cannot merge") {
		auto execution = split->GetPassengerJourney(); execution.departure += 1;
		split->SetPassengerJourney(execution, B);
		CHECK_FALSE(VehicleCargoList::AreMergable(first.get(), split.get())); CHECK_FALSE(first->Merge(split.get()));
	}
}

TEST_CASE("Routing state and leg have an independent versioned save round trip", "[passenger][active][saveload]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	REQUIRE(_sl_xv_feature_versions[XSLFI_PASSENGER_JOURNEY_ROUTING] == 1);
	auto packet = std::unique_ptr<CargoPacket>(fixture.Packet()); fixture.Plan(*packet);
	PassengerRoutingMode expected = PassengerRoutingMode::Journey;
	SECTION("Journey survives") {}
	SECTION("Fallback survives") { packet->FallbackPassengerJourney(A, NOW); expected = PassengerRoutingMode::Fallback; }
	SECTION("Budget wait survives without a new save feature") {
		packet->DeferPassengerJourney(A, NOW); expected = PassengerRoutingMode::Legacy;
	}
	SECTION("Old save defaults to Legacy") { _sl_xv_feature_versions[XSLFI_PASSENGER_JOURNEY_ROUTING] = 0; expected = PassengerRoutingMode::Legacy; }
	auto loaded = std::unique_ptr<CargoPacket>(CargoPacket::Create());
	REQUIRE_FALSE(SlTestSaveLoadTable(packet.get(), loaded.get(), GetCargoPacketDesc()).empty());
	CHECK(loaded->GetPassengerRoutingMode() == expected); CHECK(loaded->GetPassengerDestination() == D);
	CHECK(loaded->Count() == 10); CHECK(loaded->GetFeederShare() == 100);
	if (expected == PassengerRoutingMode::Journey) CHECK(loaded->GetPassengerJourney() == packet->GetPassengerJourney());
	if (packet->IsPassengerJourneyDeferred()) {
		CHECK(loaded->IsPassengerJourneyDeferred());
		CHECK_FALSE(loaded->CanRetryPassengerJourney(A, NOW));
		CHECK(loaded->CanRetryPassengerJourney(A, NOW + 1));
	}
	SECTION("Invalid vehicle becomes fallback during lifecycle validation") {
		AfterLoadPassengerJourneys(); CHECK(loaded->GetPassengerRoutingMode() == PassengerRoutingMode::Fallback);
	}
}

TEST_CASE("GUI counts available modes separately from reserved passengers without changing RNG", "[passenger][active][determinism]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	auto *journey = fixture.Packet(20); fixture.Plan(*journey); fixture.Waiting().Append(journey, D);
	auto *legacy = fixture.Packet(30, StationID::Invalid()); fixture.Waiting().Append(legacy, D);
	auto *fallback = fixture.Packet(40); fallback->FallbackPassengerJourney(A, NOW); fixture.Waiting().Append(fallback, D);
	std::array next{D};
	fixture.Waiting().Reserve(10, &train.cargo, next, TileIndex{0}, &train);
	Station::Get(A)->loading_vehicles.push_back(&train);
	GameRandomSeedChecker random;
	for (uint i = 0; i < 10; ++i) {
		auto counts = CountPassengerRoutingModes(fixture.Waiting());
		auto reserved = CountReservedPassengerRoutingModes(*Station::Get(A), PAX);
		CHECK(counts[0] + counts[1] + counts[2] == fixture.Waiting().AvailableCount());
		CHECK(reserved[0] + reserved[1] + reserved[2] == fixture.Waiting().ReservedCount());
		CHECK(counts[0] + counts[1] + counts[2] + fixture.Waiting().ReservedCount() == 90);
		CHECK(counts[2] == 40);
	}
	CHECK(random.Check());
	CHECK(fixture.Waiting().TotalCount() == 90);
}

TEST_CASE("4096 OD packets reuse one search and consume no random numbers", "[passenger][active][performance][determinism]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	PassengerJourneyActiveContext context(nullptr, A);
	GameRandomSeedChecker random;
	for (uint i = 0; i < 4096; ++i) {
		auto packet = std::unique_ptr<CargoPacket>(fixture.Packet(1));
		REQUIRE(context.PlanWaiting(*packet, PAX, D));
		CHECK(packet->GetPassengerJourney().vehicle == train.index);
	}
	CHECK(context.GetWorkAllowanceUsed() < 600000);
	WARN("Active routing: CargoPacket=" << sizeof(CargoPacket) << " bytes; leg=" << sizeof(PassengerJourneyExecution) <<
			" bytes; 4096 identical OD queries use=" << context.GetWorkAllowanceUsed() << " work allowance; pool=" << CargoPacket::GetNumItems());
	CHECK(random.Check());
}

TEST_CASE("Order dispatch refit and loading restrictions invalidate boarding", "[passenger][active]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	auto packet = std::unique_ptr<CargoPacket>(fixture.Packet()); fixture.Plan(*packet);
	REQUIRE(PassengerJourneyBoardingContext(&train).CanBoard(*packet));
	SECTION("NoLoad") { train.GetOrder(0)->SetLoadType(OrderLoadType::NoLoad); }
	SECTION("Changed timetable") { train.GetOrder(1)->SetTravelTime(200); }
	SECTION("Refit") { train.GetOrder(0)->SetRefit(CargoType{1}); }
	SECTION("Scheduled dispatch changed") {
		train.orders->GetScheduledDispatchScheduleSet().resize(1);
		auto &schedule = train.orders->GetDispatchScheduleByIndex(0);
		schedule.SetScheduledDispatchDuration(1000); schedule.GetScheduledDispatchMutable() = {{100, 0, {}}};
	}
	SECTION("Stopped train") { train.vehstatus.Set(VehState::Stopped); }
	SECTION("Different occurrence of the same order") { train.current_order_time = 10; }
	CHECK_FALSE(PassengerJourneyBoardingContext(&train).CanBoard(*packet));
}

TEST_CASE("Deleting the chosen train clears its leg before an ID can be reused", "[passenger][active]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	auto *packet = fixture.Packet(); fixture.Plan(*packet); fixture.Waiting().Append(packet, B);
	InvalidatePassengerJourneyVehicle(train.index);
	SECTION("Original CargoDist queue is still available") {}
	SECTION("Removed CargoDist queue cannot strand the fallback packet") { CargoPacket::InvalidateAllFrom(B); CHECK(packet->GetPassengerLegacyHop() == StationID::Invalid()); }
	CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Fallback);
	CHECK(packet->GetPassengerJourney().vehicle == VehicleID::Invalid());
	CHECK(packet->GetPassengerDestination() == D);
	fixture.Waiting().RoutePassengerJourneys(PAX, A);
	std::array next{B};
	CHECK(fixture.Waiting().Load(10, &train.cargo, next, TileIndex{0}, &train) == 10);
}

TEST_CASE("Disabling journeys rekeys passengers and preserves legacy random routing", "[passenger][active][determinism]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	auto *packet = fixture.Packet(); fixture.Plan(*packet); fixture.Waiting().Append(packet, B);
	_settings_game.linkgraph.passenger_journey_routing = false;
	PassengerJourneySettingsChanged(0);
	CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Fallback);
	CHECK(fixture.Waiting().AvailableViaCount(B) == 10);
	std::array next{B};
	CHECK(fixture.Waiting().Load(10, &train.cargo, next, TileIndex{0}, &train) == 10);
	train.cargo.Truncate();
	GoodsEntry ge;
	ge.CreateData().flows.insert(FlowStat(A, B, 50)); ge.CreateData().flows.find(A)->AppendShare(D, 50);
	const Randomizer initial = _random;
	std::array<uint, 3> previous{};
	Randomizer after{};
	for (bool context_enabled : {false, true}) {
		_random = initial;
		for (uint16_t age = 1; age < 20; ++age) train.cargo.Append(fixture.Packet(age, D, age));
		CargoPayment payment(CargoPaymentID::Invalid(), &train);
		PassengerJourneyActiveContext context(&train, A);
		train.cargo.Stage(true, A, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0}, nullptr, nullptr, context_enabled ? &context : nullptr);
		std::array counts{train.cargo.ActionCount(Action::Keep), train.cargo.ActionCount(Action::Transfer), train.cargo.ActionCount(Action::Deliver)};
		if (context_enabled) { CHECK(counts == previous); CHECK(_random.state[0] == after.state[0]); CHECK(_random.state[1] == after.state[1]); }
		else { previous = counts; after = _random; }
		/* Truncate removes Keep only; restore all staged cargo before cleaning the fixture. */
		train.cargo.KeepAll(); train.cargo.Truncate();
	}
	_random = initial;
}

TEST_CASE("Distinct OD searches stop at the deterministic batch work allowance", "[passenger][active][performance]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	PassengerJourneyActiveContext context(nullptr, A);
	uint unsuccessful = 0;
	for (uint16_t i = 4; i < 100; ++i) {
		StationID destination{i}; Station::CreateAtIndex(destination, TileIndex{0})->owner = OWNER_NONE;
		auto packet = std::unique_ptr<CargoPacket>(fixture.Packet(1, destination));
		if (!context.PlanWaiting(*packet, PAX, D)) ++unsuccessful;
	}
	CHECK(unsuccessful == 96);
	CHECK(context.GetWorkAllowanceUsed() <= 4194304);
	auto reachable = std::unique_ptr<CargoPacket>(fixture.Packet());
	CHECK_FALSE(context.PlanWaiting(*reachable, PAX, D)); // Budget exhaustion is inconclusive.
}

TEST_CASE("Event snapshots refresh delays timetables capacity and position within the same tick", "[passenger][active][cache]")
{
	ActiveFixture fixture;
	ActiveTrain local({ActiveStop(A), ActiveStop(D, 500)});
	ActiveTrain express({ActiveStop(A, 10000, 120), ActiveStop(D, 10)}, VehicleID{101});
	auto before = std::unique_ptr<CargoPacket>(fixture.Packet()); fixture.Plan(*before);
	REQUIRE(before->GetPassengerJourney().vehicle == express.index);
	SECTION("Delay") { express.lateness_counter = -1000; }
	SECTION("Timetable edit") { express.GetOrder(1)->SetTravelTime(2000); }
	bool changed_eligibility = false;
	SECTION("Refit consist") { express.cargo_type = CargoType{1}; changed_eligibility = true; }
	SECTION("Position and current order") { express.LoadAt(1); }
	auto after = std::unique_ptr<CargoPacket>(fixture.Packet()); fixture.Plan(*after);
	CHECK(after->GetPassengerJourney().vehicle == local.index);
	CHECK(after->GetPassengerJourney().arrival == NOW + 600);
	CHECK(_state_ticks == NOW);
	CHECK(GetPassengerJourneyTickDiagnostic().builds == 2);
	CHECK(GetPassengerJourneyTickDiagnostic().static_cache_hits == (changed_eligibility ? 0 : 1));
}

TEST_CASE("A current off corridor trip is captured for an immediate transfer", "[passenger][active][scope]")
{
	ActiveFixture fixture;
	ActiveTrain branch({ActiveStop(A), ActiveStop(B, 10000)});
	ActiveTrain direct({ActiveStop(A, 10000, 120), ActiveStop(D, 10)}, VehicleID{101});
	auto packet = std::unique_ptr<CargoPacket>(fixture.Packet()); fixture.Plan(*packet);
	auto leg = packet->GetPassengerJourney(); leg.vehicle = branch.index; leg.service = branch.orders->index;
	packet->SetPassengerJourney(leg, B);
	PassengerJourneyActiveContext context(&branch, A);
	CHECK(context.Decide(*packet, PAX, false, OrderUnloadType::UnloadIfPossible) == Action::Transfer);
	CHECK(packet->GetPassengerJourney().vehicle == direct.index);
}

TEST_CASE("Tick allowances survive local cache and diagnostic resets deterministically", "[passenger][active][determinism][budget]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	std::vector<bool> previous;
	GameRandomSeedChecker random;
	for (bool cold : {true, false}) {
		BeginPassengerJourneyTick(); ResetPassengerJourneyDiagnostic();
		std::vector<bool> decisions;
		for (uint i = 0; i < 40; ++i) {
			if (cold) InvalidatePassengerServiceIndex();
			auto packet = std::unique_ptr<CargoPacket>(fixture.Packet());
			PassengerJourneyActiveContext context(nullptr, A);
			decisions.push_back(context.PlanWaiting(*packet, PAX, D));
		}
		if (cold) previous = decisions; else CHECK(decisions == previous);
		CHECK(std::count(decisions.begin(), decisions.end(), true) > 0);
		CHECK(std::count(decisions.begin(), decisions.end(), false) > 0);
		const auto diagnostic = GetPassengerJourneyTickDiagnostic();
		CHECK(diagnostic.builds <= 16);
		CHECK(diagnostic.graph_work + diagnostic.fair_share_work <= 16 * 1024 * 1024);
		CHECK(diagnostic.query_work <= 1024 * 1024);
		CHECK(diagnostic.deferred[static_cast<size_t>(PassengerJourneyFallbackReason::QueryWork)] > 0);
		ResetPassengerJourneyDiagnostic(); // This is local UI state, not a new allowance.
		auto packet = std::unique_ptr<CargoPacket>(fixture.Packet());
		CHECK_FALSE(PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*packet, PAX, D));
	}
	++_state_ticks; BeginPassengerJourneyTick();
	auto packet = std::unique_ptr<CargoPacket>(fixture.Packet());
	CHECK(PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*packet, PAX, D));
	CHECK(random.Check());
}

TEST_CASE("An unrelated running implicit service leaves active journey routing complete", "[passenger][active][scope]")
{
	ActiveFixture fixture;
	for (uint16_t station = 4; station <= 6; ++station) Station::CreateAtIndex(StationID{station}, TileIndex{0})->owner = OWNER_NONE;
	ActiveTrain direct({ActiveStop(A), ActiveStop(D, 100)});
	Order implicit; implicit.MakeImplicit(StationID{6});
	ActiveTrain remote({ActiveStop(StationID{4}), implicit, ActiveStop(StationID{5})}, VehicleID{101});
	auto packet = std::unique_ptr<CargoPacket>(fixture.Packet());
	fixture.Plan(*packet);
	CHECK(packet->GetPassengerJourney().vehicle == direct.index);
	CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
}

TEST_CASE("Distinct OD budget waits retry next tick and remain normal failures when unreachable", "[passenger][active][budget]")
{
	ActiveFixture fixture;
	std::vector<Order> orders{ActiveStop(A, 100000, 100)};
	for (uint16_t station = 1; station <= 32; ++station) {
		if (station >= 4) Station::CreateAtIndex(StationID{station}, TileIndex{0})->owner = OWNER_NONE;
		orders.push_back(ActiveStop(StationID{station}, 1, 1));
	}
	auto train = std::unique_ptr<ActiveTrain>(Vehicle::Create<ActiveTrain>(std::move(orders)));
	std::vector<CargoPacket *> packets;
	for (uint16_t destination = 1; destination <= 32; ++destination) {
		packets.push_back(fixture.Packet(1, StationID{destination}));
		fixture.Waiting().Append(packets.back(), D);
	}
	ResetPassengerJourneyDiagnostic();
	fixture.Waiting().RoutePassengerJourneys(PAX, A);
	REQUIRE(std::count_if(packets.begin(), packets.end(), [](const auto *p) { return p->IsPassengerJourneyDeferred(); }) > 0);
	for (auto *packet : packets) if (packet->IsPassengerJourneyDeferred()) {
		CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Legacy);
		CHECK_FALSE(packet->CanRetryPassengerJourney(A, _state_ticks));
		CHECK(packet->CanRetryPassengerJourney(A, _state_ticks + 1));
	}
	CHECK(std::accumulate(GetPassengerJourneyTickDiagnostic().fallbacks.begin(), GetPassengerJourneyTickDiagnostic().fallbacks.end(), size_t{0}) == 0);
	for (uint tick = 0; tick < 3; ++tick) {
		++_state_ticks; ++train->current_order_time; BeginPassengerJourneyTick();
		fixture.Waiting().RoutePassengerJourneys(PAX, A);
	}
	for (auto *packet : packets) CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
	Station::CreateAtIndex(StationID{33}, TileIndex{0})->owner = OWNER_NONE;
	auto *unreachable = fixture.Packet(1, StationID{33}); fixture.Waiting().Append(unreachable, D);
	unreachable->DeferPassengerJourney(A, _state_ticks);
	++_state_ticks; ++train->current_order_time; BeginPassengerJourneyTick();
	fixture.Waiting().RoutePassengerJourneys(PAX, A);
	CHECK(unreachable->GetPassengerRoutingMode() == PassengerRoutingMode::Fallback);
	CHECK_FALSE(unreachable->IsPassengerJourneyDeferred());
	CHECK_FALSE(unreachable->CanRetryPassengerJourney(A, _state_ticks + 1));
}

TEST_CASE("Rotating loading shares survive a hot origin through LoadUnloadStation", "[passenger][active][budget][determinism]")
{
	ActiveFixture fixture;
	std::vector<Order> orders{ActiveStop(A, 100000, 100)};
	for (uint16_t station = 1; station <= 32; ++station) {
		if (station >= 4) Station::CreateAtIndex(StationID{station}, TileIndex{0})->owner = OWNER_NONE;
		orders.push_back(ActiveStop(StationID{station}, 1, 1));
	}
	Vehicle::Create<ActiveTrain>(std::move(orders));
	std::array<CargoPacket *, 32> packets;
	std::array<RoadVehicle *, 32> loaders;
	for (uint16_t station = 0; station < packets.size(); ++station) {
		packets[station] = fixture.Packet(1, StationID{32});
		fixture.Waiting(StationID{station}).Append(packets[station], D);
		loaders[station] = DueLoadingVehicle(StationID{station});
	}
	GameRandomSeedChecker random;
	std::vector<bool> previous;
	for (bool cold : {true, false}) {
		_state_ticks = NOW;
		std::array<bool, 32> served{};
		std::vector<bool> decisions;
		for (uint16_t station = 0; station < packets.size(); ++station) {
			packets[station]->DeferPassengerJourney(StationID{station}, NOW - 1);
			packets[station]->SetPassengerLegacyHop(D);
		}
		auto plan = [&](CargoPacket &packet, StationID origin, StationID legacy) {
			if (cold) InvalidatePassengerServiceIndex();
			const bool applied = PassengerJourneyActiveContext(nullptr, origin).PlanWaiting(packet, PAX, legacy);
			decisions.push_back(applied); return applied;
		};
		for (uint tick = 0; tick < 8; ++tick) {
			BeginPassengerJourneyTick();
			for (RoadVehicle *loader : loaders) loader->load_unload_ticks = 1;
			PreparePassengerJourneyLoading();
			/* A consumes every ordinary admission it can before other origins run. */
			for (uint repeat = 0; repeat < 40; ++repeat) {
				auto hot = std::unique_ptr<CargoPacket>(fixture.Packet(1, StationID{static_cast<uint16_t>(1 + repeat % 32)}));
				plan(*hot, A, D);
			}
			for (uint16_t station = 0; station < packets.size(); ++station) {
				if (cold) InvalidatePassengerServiceIndex();
				LoadUnloadStation(Station::Get(StationID{station}));
				served[station] = packets[station]->GetPassengerRoutingMode() == PassengerRoutingMode::Journey;
				decisions.push_back(served[station]);
			}
			const auto &diagnostic = GetPassengerJourneyTickDiagnostic();
			CHECK(diagnostic.graph_work + diagnostic.fair_share_work <= 16 * 1024 * 1024);
			CHECK(diagnostic.query_work <= 1024 * 1024);
			CHECK(diagnostic.builds <= 16);
			/* Claimed shares release their unused headroom to ordinary searches. */
			for (uint repeat = 0; repeat < 40; ++repeat) {
				auto hot = std::unique_ptr<CargoPacket>(fixture.Packet());
				plan(*hot, A, D);
			}
			ResetPassengerJourneyDiagnostic(); // Does not release reserved or consumed admissions.
			auto hot = std::unique_ptr<CargoPacket>(fixture.Packet());
			CHECK_FALSE(plan(*hot, A, D));
			++_state_ticks;
		}
		for (size_t station = 1; station < packets.size(); ++station) CHECK(served[station]);
		if (cold) previous = decisions; else CHECK(decisions == previous);
	}
	CHECK(random.Check());
	CleanLoadingVehicles();
}

TEST_CASE("Fair shares exclude idle stations unknown ODs and sleeping loading passes", "[passenger][active][budget][loading]")
{
	ActiveFixture fixture;
	for (uint16_t station = 4; station <= 4800; ++station) Station::CreateAtIndex(StationID{station}, TileIndex{0})->owner = OWNER_NONE;
	std::vector<Order> orders{ActiveStop(A, 100000, 100)};
	std::vector<CargoPacket *> packets;
	for (uint16_t destination = 10; destination < 42; ++destination) {
		orders.push_back(ActiveStop(StationID{destination}, 1, 1));
		packets.push_back(fixture.Packet(1, StationID{destination})); fixture.Waiting().Append(packets.back(), D);
	}
	Vehicle::Create<ActiveTrain>(std::move(orders));
	DueLoadingVehicle(A);
	/* The former available-count census would reserve shares for these idle origins. */
	for (uint16_t station = 1; station <= 4800; ++station) {
		fixture.Waiting(StationID{station}).Append(fixture.Packet(1, station == B.base() ? StationID::Invalid() : D), D);
	}
	DueLoadingVehicle(B); // Due, but no assigned OD.
	DueLoadingVehicle(C, 2); // Has an OD, but the loading countdown is not due.
	DueLoadingVehicle(D); // Its only packet has already reached its OD.
	DueLoadingVehicle(StationID{4})->vehstatus.Set(VehState::Stopped);
	DueLoadingVehicle(StationID{5})->vehstatus.Set(VehState::Crashed);
	DueLoadingVehicle(StationID{6})->current_order.MakeLoadingAdvance(StationID{6});
	DueLoadingVehicle(StationID{7});
	StationCargoList::ConstIterator cooled(fixture.Waiting(StationID{7}).Packets()->begin());
	(*cooled)->FallbackPassengerJourney(StationID{7}, _state_ticks); // Cooldown.
	BeginPassengerJourneyTick(); ResetPassengerJourneyDiagnostic();
	PreparePassengerJourneyLoading();
	LoadUnloadStation(Station::Get(A));
	const size_t applied = std::count_if(packets.begin(), packets.end(), [](const CargoPacket *p) { return p->GetPassengerRoutingMode() == PassengerRoutingMode::Journey; });
	CHECK(applied > 8); // Ordinary work was not halved by unsearchable stations.
	WARN("Due loading admission: stations=4801; applied=" << applied << "; waiting-share claims=" << GetPassengerJourneyTickDiagnostic().fair_share_claims);
	CHECK(GetPassengerJourneyTickDiagnostic().fair_share_claims == 1);
	CHECK(GetPassengerJourneyTickDiagnostic().fair_share_releases == 0);
	const size_t builds = GetPassengerJourneyTickDiagnostic().builds;
	for (Station *station : Station::Iterate()) if (station->index != A) LoadUnloadStation(station);
	CHECK(GetPassengerJourneyTickDiagnostic().builds == builds);
	CHECK(GetPassengerJourneyTickDiagnostic().query_work <= 1024 * 1024);
	CHECK(GetPassengerJourneyTickDiagnostic().graph_work + GetPassengerJourneyTickDiagnostic().fair_share_work <= 16 * 1024 * 1024);
	CleanLoadingVehicles();
}

TEST_CASE("Unused loading shares are released before later searches in the same tick", "[passenger][active][budget][loading]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	std::array<CargoPacket *, 4> waiting;
	for (uint16_t i = 0; i < waiting.size(); ++i) {
		StationID station{static_cast<uint16_t>(4 + i)};
		Station::CreateAtIndex(station, TileIndex{0})->owner = OWNER_NONE;
		waiting[i] = fixture.Packet(1); fixture.Waiting(station).Append(waiting[i], D);
		DueLoadingVehicle(station);
	}
	BeginPassengerJourneyTick(); ResetPassengerJourneyDiagnostic();
	PreparePassengerJourneyLoading();
	/* Demand becomes ineligible after the census. The real waiting passes still
	 * run, release their shares, and don't admit any searches. */
	for (uint16_t i = 0; i < waiting.size(); ++i) {
		StationID station{static_cast<uint16_t>(4 + i)};
		waiting[i]->FallbackPassengerJourney(station, _state_ticks);
		LoadUnloadStation(Station::Get(station));
	}
	CHECK(GetPassengerJourneyTickDiagnostic().builds == 0);
	CHECK(GetPassengerJourneyTickDiagnostic().fair_share_claims == 0);
	CHECK(GetPassengerJourneyTickDiagnostic().fair_share_releases == 4);
	size_t applied = 0;
	for (uint i = 0; i < 40; ++i) {
		auto packet = std::unique_ptr<CargoPacket>(fixture.Packet(1));
		applied += PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*packet, PAX, D);
	}
	CHECK(applied == 15); // All query Work is available again, in this same tick.
	CleanLoadingVehicles();
}

TEST_CASE("An exhausted fair-share census leaves ordinary search credits available", "[passenger][active][budget][loading][.performance]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	DueLoadingVehicle(A); fixture.Waiting().Append(fixture.Packet(1), D);
	DueLoadingVehicle(B);
	for (uint i = 0; i < 70000; ++i) fixture.Waiting(B).Append(fixture.Packet(1, StationID::Invalid(), static_cast<uint16_t>(i)), D);
	BeginPassengerJourneyTick(); ResetPassengerJourneyDiagnostic();
	PreparePassengerJourneyLoading();
	CHECK(GetPassengerJourneyTickDiagnostic().fair_share_work == 65536);
	CHECK(GetPassengerJourneyTickDiagnostic().fair_share_claims == 0);
	size_t applied = 0;
	for (uint i = 0; i < 40; ++i) {
		auto packet = std::unique_ptr<CargoPacket>(fixture.Packet(1));
		applied += PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*packet, PAX, D);
	}
	CHECK(applied == 15);
	CHECK(GetPassengerJourneyTickDiagnostic().fair_share_claims == 0);
	CHECK(GetPassengerJourneyTickDiagnostic().fair_share_releases == 0);
	CHECK(GetPassengerJourneyTickDiagnostic().graph_work + GetPassengerJourneyTickDiagnostic().fair_share_work <= 16 * 1024 * 1024);
	CleanLoadingVehicles();
}

TEST_CASE("Two thousand distinct destinations drain a deferred budget queue", "[passenger][active][budget][.performance]")
{
	ActiveFixture fixture;
	std::deque<ActiveTrain> fleet;
	std::vector<CargoPacket *> packets;
	for (uint16_t destination = 1; destination <= 2000; ++destination) {
		if (destination >= 4) Station::CreateAtIndex(StationID{destination}, TileIndex{0})->owner = OWNER_NONE;
		fleet.emplace_back(std::vector<Order>{ActiveStop(A, 100000, 1), ActiveStop(StationID{destination}, 1, 1)}, VehicleID{1000u + destination});
		packets.push_back(fixture.Packet(1, StationID{destination}));
		fixture.Waiting().Append(packets.back(), D);
	}
	size_t served = 0, first_applied = 0, first_deferred = 0, ticks = 0;
	while (served != packets.size() && ticks < 160) {
		BeginPassengerJourneyTick();
		PassengerJourneyActiveContext context(nullptr, A);
		for (CargoPacket *packet : packets) if (packet->GetPassengerRoutingMode() != PassengerRoutingMode::Journey) served += context.PlanWaiting(*packet, PAX, D);
		if (ticks == 0) {
			first_applied = served;
			first_deferred = std::count_if(packets.begin(), packets.end(), [](const auto *p) { return p->IsPassengerJourneyDeferred(); });
			CHECK(first_applied > 0); CHECK(first_applied <= 16);
			CHECK(first_applied + first_deferred == packets.size());
		}
		const auto &stats = GetPassengerJourneyTickDiagnostic();
		CHECK(stats.builds <= 16);
		CHECK(stats.query_work <= 1024 * 1024);
		CHECK(stats.graph_work + stats.fair_share_work <= 16 * 1024 * 1024);
		CHECK(std::accumulate(stats.fallbacks.begin(), stats.fallbacks.end(), size_t{0}) == 0);
		++ticks; ++_state_ticks;
	}
	CHECK(served == packets.size());
	WARN("Distinct OD admission: first applied=" << first_applied << "; deferred=" << first_deferred << "; total=" << served << "; ticks=" << ticks);
}

TEST_CASE("Production journey execution covers eight thousand stations at standard budgets", "[passenger][active][performance][scope]")
{
	ActiveFixture fixture;
	for (uint16_t station = 4; station <= 8000; ++station) Station::CreateAtIndex(StationID{station}, TileIndex{0})->owner = OWNER_NONE;
	std::deque<ActiveTrain> fleet;
	for (uint16_t route = 0; route < 80; ++route) {
		std::vector<Order> orders{ActiveStop(A, 100000, 1)};
		for (uint16_t station = 1; station <= 100; ++station) orders.push_back(ActiveStop(StationID{static_cast<uint16_t>(route * 100 + station)}, 1, 1));
		for (uint16_t train = 0; train < 10; ++train) fleet.emplace_back(orders, VehicleID{1000u + route * 10u + train});
	}
	InvalidatePassengerServiceIndex(); ResetPassengerJourneyDiagnostic();
	PassengerJourneyActiveContext context(nullptr, A);
	for (uint i = 0; i < 64; ++i) {
		auto packet = std::unique_ptr<CargoPacket>(fixture.Packet(10, StationID{8000}));
		REQUIRE(context.PlanWaiting(*packet, PAX, D));
		CHECK(packet->GetPassengerJourney().arrival == NOW + 200);
	}
	const auto diagnostic = GetPassengerJourneyTickDiagnostic();
	CHECK(diagnostic.builds == 1);
	CHECK(diagnostic.queries == 1);
	CHECK(diagnostic.query_cache_hits == 63);
	CHECK(diagnostic.journeys == 64);
	CHECK(diagnostic.visits == 1010);
	CHECK(std::accumulate(diagnostic.fallbacks.begin(), diagnostic.fallbacks.end(), size_t{0}) == 0);
	WARN("Production national: stations=" << Station::GetNumItems() << "; journey packets=" << diagnostic.journeys << "; builds=" << diagnostic.builds << "; work=" << diagnostic.graph_work << "; build us=" << diagnostic.build_microseconds << "; retained=" << diagnostic.graph_allocation_bytes << "; static=" << diagnostic.static_allocation_bytes << "; cache hits=" << diagnostic.query_cache_hits);
}

TEST_CASE("A primary engine with different cargo preserves the passenger coach journey after load", "[passenger][active][saveload]")
{
	ActiveFixture fixture;
	ActiveTrain *train = Vehicle::Create<ActiveTrain>(std::vector<Order>{ActiveStop(A), ActiveStop(D, 100)});
	ActiveCoach *coach = Vehicle::Create<ActiveCoach>();
	train->cargo_type = CargoType{1}; train->SetNext(coach);
	auto *packet = fixture.Packet();
	fixture.Waiting().Append(packet, D);
	fixture.Waiting().RoutePassengerJourneys(PAX, A);
	REQUIRE(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
	fixture.Waiting().RoutePassengerJourneys(PAX, A, false);
	CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
	const auto execution = packet->GetPassengerJourney();
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	AfterLoadPassengerJourneys();
	CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
	CHECK(packet->GetPassengerJourney() == execution);
	std::array next{D};
	CHECK(fixture.Waiting().Load(10, &coach->cargo, next, TileIndex{0}, coach) == 10);
	train->SetNext(nullptr);
	_vehicle_pool.CleanPool(); // Pool cleanup avoids unrelated engine/GUI destruction for these mock trains.
}

TEST_CASE("Boarding prediction is lazy for Legacy and other trains", "[passenger][active][performance]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	PassengerJourneyLoadingContext loading;
	PassengerJourneyBoardingContext boarding(&train);
	auto legacy = std::unique_ptr<CargoPacket>(fixture.Packet(30, StationID::Invalid()));
	CHECK(boarding.CanBoard(*legacy));
	CHECK(boarding.GetPredictionCount() == 0);
	fixture.Waiting().Append(legacy.release(), D);
	std::array next{D};
	CHECK(fixture.Waiting().HasCargoFor(next, &train, &loading));
	CHECK(fixture.Waiting().Reserve(20, &train.cargo, next, TileIndex{0}, &train, &loading) == 20);
	CHECK(fixture.Waiting().Load(30, &train.cargo, next, TileIndex{0}, &train, &loading) == 20);
	CHECK(fixture.Waiting().Load(10, &train.cargo, next, TileIndex{0}, &train, &loading) == 10);
	CHECK(loading.GetPredictionCount() == 0);
	auto journey = std::unique_ptr<CargoPacket>(fixture.Packet()); fixture.Plan(*journey);
	ActiveTrain other({ActiveStop(A), ActiveStop(D, 100)}, VehicleID{101});
	CHECK_FALSE(loading.CanBoard(*journey, &other));
	CHECK(loading.GetPredictionCount() == 0);
	CHECK(loading.CanBoard(*journey, &train));
	CHECK(loading.GetPredictionCount() == 1);
	train.GetOrder(1)->SetTravelTime(200);
	loading.Invalidate(&train);
	CHECK_FALSE(loading.CanBoard(*journey, &train));
	CHECK_FALSE(PassengerJourneyLoadingContext().CanBoard(*journey, &train));
}

TEST_CASE("Vehicle invalidation indexes only referring journeys through packet lifecycle", "[passenger][active][performance]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	auto first = std::unique_ptr<CargoPacket>(fixture.Packet()); fixture.Plan(*first);
	auto split = std::unique_ptr<CargoPacket>(first->Split(4));
	REQUIRE(split != nullptr);
	ActiveTrain other({ActiveStop(A), ActiveStop(D, 100)}, VehicleID{101});
	auto other_packet = std::unique_ptr<CargoPacket>(fixture.Packet());
	auto other_leg = first->GetPassengerJourney(); other_leg.vehicle = other.index; other_leg.service = other.orders->index;
	other_packet->SetPassengerJourney(other_leg, D);
	std::vector<std::unique_ptr<CargoPacket>> legacy;
	for (uint i = 0; i < 4096; ++i) legacy.emplace_back(fixture.Packet(1, StationID::Invalid()));
	size_t expected = 2, other_expected = 1;
	SECTION("Split entries are both indexed") {}
	SECTION("Merge removes the consumed entry") { REQUIRE(first->Merge(split.release())); expected = 1; }
	SECTION("Fallback removes its entry") { split->FallbackPassengerJourney(A, NOW); expected = 1; }
	SECTION("Packet deletion removes its entry") { split.reset(); expected = 1; }
	SECTION("Selecting another physical train moves its entry") { split->SetPassengerJourney(other_leg, D); expected = 1; other_expected = 2; }
	CHECK(InvalidatePassengerJourneyVehicle(VehicleID{102}) == 0);
	CHECK(InvalidatePassengerJourneyVehicle(train.index) == expected);
	CHECK(first->GetPassengerRoutingMode() == PassengerRoutingMode::Fallback);
	CHECK(other_packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
	CHECK(InvalidatePassengerJourneyVehicle(train.index) == 0);
	CHECK(InvalidatePassengerJourneyVehicle(other.index) == other_expected);
	for (const auto &packet : legacy) CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Legacy);
}

TEST_CASE("Unchanged waiting queues retain their buckets and packet storage", "[passenger][active][performance]")
{
	ActiveFixture fixture;
	ActiveTrain *train = Vehicle::Create<ActiveTrain>(std::vector<Order>{ActiveStop(A), ActiveStop(D, 100)});
	fixture.Waiting().Append(fixture.Packet(30, StationID::Invalid()), B);
	fixture.Waiting().Append(fixture.Packet(), D);
	const auto *bucket = &*fixture.Waiting().Packets()->find(B);
	const auto *storage = &bucket->second.front();
	PassengerJourneyLoadingContext loading;
	CHECK(fixture.Waiting().RoutePassengerJourneys(PAX, A, true, &loading) == 1);
	CHECK(&*fixture.Waiting().Packets()->find(B) == bucket);
	CHECK(&fixture.Waiting().Packets()->find(B)->second.front() == storage);
	CHECK(fixture.Waiting().RoutePassengerJourneys(PAX, A, true, &loading) == 0);
	CHECK(&*fixture.Waiting().Packets()->find(B) == bucket);
	CHECK(&fixture.Waiting().Packets()->find(B)->second.front() == storage);
	std::array next{D};
	CHECK(fixture.Waiting().Load(10, &train->cargo, next, TileIndex{0}, train, &loading) == 10);
	CHECK(loading.GetPredictionCount() == 1); // Waiting validation and loading share the capture.
	_vehicle_pool.CleanPool();
}

TEST_CASE("Existing Journey packets do not consume the Fallback retry inspection allowance", "[passenger][active][performance]")
{
	ActiveFixture fixture;
	ActiveTrain *train = Vehicle::Create<ActiveTrain>(std::vector<Order>{ActiveStop(A), ActiveStop(D, 100)});
	auto example = std::unique_ptr<CargoPacket>(fixture.Packet()); fixture.Plan(*example, A, StationID::Invalid());
	for (uint16_t i = 1; i <= 4096; ++i) {
		auto *packet = fixture.Packet(1, D, i);
		packet->SetPassengerJourney(example->GetPassengerJourney(), StationID::Invalid());
		fixture.Waiting().Append(packet, StationID::Invalid());
	}
	auto *retry = fixture.Packet(1, D, 5000);
	retry->FallbackPassengerJourney(A, NOW - 256);
	fixture.Waiting().Append(retry, StationID::Invalid());
	PassengerJourneyLoadingContext loading;
	CHECK(fixture.Waiting().RoutePassengerJourneys(PAX, A, true, &loading) == 0);
	CHECK(retry->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
	CHECK(loading.GetPredictionCount() == 1);
	CHECK(fixture.Waiting().AvailableCount() == 4097);
	_vehicle_pool.CleanPool();
}

TEST_CASE("Disabling Journey returns only its reservations including a gradual-loading boundary", "[passenger][active]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	Station::Get(A)->loading_vehicles.push_back(&train);
	std::array next{D};
	auto *first = fixture.Packet(10); fixture.Plan(*first); fixture.Waiting().Append(first, D);
	REQUIRE(fixture.Waiting().Reserve(10, &train.cargo, next, TileIndex{0}, &train) == 10);
	auto *legacy = fixture.Packet(30, StationID::Invalid()); fixture.Waiting().Append(legacy, D);
	REQUIRE(fixture.Waiting().Reserve(30, &train.cargo, next, TileIndex{0}, &train) == 30);
	auto *last = fixture.Packet(20, D, 8); fixture.Plan(*last); fixture.Waiting().Append(last, D);
	REQUIRE(fixture.Waiting().Reserve(20, &train.cargo, next, TileIndex{0}, &train) == 20);
	REQUIRE(fixture.Waiting().Load(5, &train.cargo, next, TileIndex{0}, &train) == 5);
	SECTION("Routing disabled") { _settings_game.linkgraph.passenger_journey_routing = false; }
	SECTION("CargoDist changed to Manual") { _settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Manual; }
	PassengerJourneySettingsChanged(0);
	CHECK(train.cargo.StoredCount() == 5);
	CHECK(train.cargo.ReservedCount() == 30);
	CHECK(fixture.Waiting().ReservedCount() == 30);
	CHECK(fixture.Waiting().AvailableCount() == 25);
	CHECK(train.cargo.Packets()->back() == legacy);
	CHECK(legacy->Count() == 30);
	CHECK(legacy->GetPassengerRoutingMode() == PassengerRoutingMode::Legacy);
	CHECK(legacy->GetFeederShare() == 100);
	Money share = train.cargo.GetFeederShare();
	for (StationCargoList::ConstIterator it(fixture.Waiting().Packets()->begin()); it != fixture.Waiting().Packets()->end(); ++it) share += (*it)->GetFeederShare();
	CHECK(share == 300);
	CHECK(CountPassengerRoutingModes(fixture.Waiting())[2] == 25);
	CHECK(fixture.Waiting().Load(30, &train.cargo, next, TileIndex{0}, &train) == 30);
	CHECK(train.cargo.StoredCount() == 35);
}

TEST_CASE("A stale Journey reservation preserves adjacent Legacy reservations", "[passenger][active]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	std::array next{D};
	auto *legacy = fixture.Packet(30, StationID::Invalid()); fixture.Waiting().Append(legacy, D);
	REQUIRE(fixture.Waiting().Reserve(30, &train.cargo, next, TileIndex{0}, &train) == 30);
	auto *journey = fixture.Packet(20); fixture.Plan(*journey); fixture.Waiting().Append(journey, D);
	REQUIRE(fixture.Waiting().Reserve(20, &train.cargo, next, TileIndex{0}, &train) == 20);
	train.GetOrder(0)->SetWaitTime(200);
	CHECK(fixture.Waiting().Load(10, &train.cargo, next, TileIndex{0}, &train) == 10);
	CHECK(train.cargo.StoredCount() == 10);
	CHECK(train.cargo.ReservedCount() == 20);
	CHECK(fixture.Waiting().ReservedCount() == 20);
	CHECK(fixture.Waiting().AvailableCount() == 20);
	CHECK(train.cargo.Packets()->front() == legacy);
	CHECK(legacy->Count() == 30);
}

TEST_CASE("Afterload returns invalid Journey reservations without disturbing Legacy", "[passenger][active][saveload]")
{
	ActiveFixture fixture;
	ActiveTrain *train = Vehicle::Create<ActiveTrain>(std::vector<Order>{ActiveStop(A), ActiveStop(D, 100)});
	Station::Get(A)->loading_vehicles.push_back(train);
	std::array next{D};
	auto *legacy = fixture.Packet(30, StationID::Invalid()); fixture.Waiting().Append(legacy, D);
	auto *journey = fixture.Packet(20); fixture.Plan(*journey); fixture.Waiting().Append(journey, D);
	REQUIRE(fixture.Waiting().Reserve(50, &train->cargo, next, TileIndex{0}, train) == 50);
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	bool invalid = true;
	SECTION("Changed orders") { train->GetOrder(0)->SetWaitTime(200); }
	SECTION("Invalid saved vehicle") {
		auto leg = journey->GetPassengerJourney(); leg.vehicle = VehicleID{999}; journey->SetPassengerJourney(leg, D);
	}
	SECTION("Valid Journey and Legacy reservations survive") { invalid = false; }
	GameRandomSeedChecker random;
	AfterLoadPassengerJourneys();
	CHECK(random.Check());
	CHECK(train->cargo.ReservedCount() == (invalid ? 30 : 50));
	CHECK(fixture.Waiting().ReservedCount() == (invalid ? 30 : 50));
	CHECK(fixture.Waiting().AvailableCount() == (invalid ? 20 : 0));
	CHECK(train->cargo.Packets()->front() == legacy);
	CHECK(legacy->Count() == 30);
	CHECK(legacy->GetPassengerRoutingMode() == PassengerRoutingMode::Legacy);
	CHECK(InvalidatePassengerJourneyVehicle(train->index) == (invalid ? 0 : 1));
	_vehicle_pool.CleanPool();
}

TEST_CASE("Cleaning the packet pool discards the reverse index before packet ID reuse", "[passenger][active]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	auto *packet = fixture.Packet(); fixture.Plan(*packet);
	CargoPacketID original = packet->index;
	_cargopacket_pool.CleanPool();
	auto replacement = std::unique_ptr<CargoPacket>(fixture.Packet(10, StationID::Invalid()));
	CHECK(replacement->index == original);
	CHECK(InvalidatePassengerJourneyVehicle(train.index) == 0);
	CHECK(replacement->GetPassengerRoutingMode() == PassengerRoutingMode::Legacy);
}
