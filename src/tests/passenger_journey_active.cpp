/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_journey_active.cpp Production boarding, reservation, staging and persistence regressions. */
#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../passenger_journey_active.h"
#include "../vehicle_base.h"
#include "../station_base.h"
#include "../economy_base.h"
#include "../company_base.h"
#include "../town.h"
#include "../core/backup_type.hpp"
#include "../core/random_func.hpp"
#include "../date_func.h"
#include "../sl/saveload.h"

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
	CHECK(fixture.Waiting().Reserve(100, &train.cargo, next, TileIndex{0}, &train) == 100);
	CHECK(fixture.Waiting().Reserve(100, &coach.cargo, next, TileIndex{0}, &coach) == 100);
	CHECK(fixture.Waiting().AvailableCount() == 100);
	CHECK(fixture.Waiting().ReservedCount() == 200);
	for (uint i = 0; i < 5; ++i) {
		CHECK(fixture.Waiting().Load(20, &train.cargo, next, TileIndex{0}, &train) == 20);
		CHECK(fixture.Waiting().Load(20, &coach.cargo, next, TileIndex{0}, &coach) == 20);
	}
	CHECK(train.cargo.StoredCount() == 100); CHECK(coach.cargo.StoredCount() == 100);
	CHECK(fixture.Waiting().ReservedCount() == 0);
	CHECK(fixture.Waiting().AvailableCount() == 100);
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
		CHECK(packet->GetPassengerJourney().vehicle == express.index);
		REQUIRE(local.cargo.Unload(10, &fixture.Waiting(B), PAX, &payment, TileIndex{0}) == 10);
		CHECK(fixture.Waiting(B).Load(10, &local.cargo, next, TileIndex{0}, &local) == 0);
		std::array express_next{D};
		CHECK(fixture.Waiting(B).Load(10, &express.cargo, express_next, TileIndex{0}, &express) == 10);
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
		CHECK(express.cargo.TotalCount() == 0);
		CHECK(fixture.Waiting(B).TotalCount() == 0);
		CHECK(fixture.Waiting(D).TotalCount() == 0);
		CHECK(delivery.company->cur_economy.delivered_cargo[PAX] == 10);
	}
}

TEST_CASE("Journey failure and unsupported passengers use CargoDist", "[passenger][active]")
{
	ActiveFixture fixture;
	ActiveTrain train({ActiveStop(A), ActiveStop(D, 100)});
	bool fallback = true;
	std::optional<ActiveTrain> uncertain;
	auto *packet = fixture.Packet();
	SECTION("Missing destination route") { train.GetOrder(1)->MakeGoToStation(B); }
	SECTION("Incomplete timetable") { train.GetOrder(0)->SetWaitTimetabled(false); }
	SECTION("A known path cannot hide an incomplete alternative") {
		Order untimed = ActiveStop(A); untimed.SetWaitTimetabled(false);
		uncertain.emplace(std::vector<Order>{untimed, ActiveStop(D, 50)}, VehicleID{101});
	}
	SECTION("Conditional service") { train.GetOrder(1)->MakeConditional(0); }
	SECTION("Manual distribution") { _settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Manual; fallback = false; }
	SECTION("Unknown OD") { delete packet; packet = fixture.Packet(10, StationID::Invalid()); fallback = false; }
	SECTION("Disabled") { _settings_game.linkgraph.passenger_journey_routing = false; fallback = false; }
	SECTION("Other transport modes remain legacy") { train.type = VehicleType::Road; }
	fixture.Waiting().Append(packet, D);
	fixture.Waiting().RoutePassengerJourneys(PAX, A);
	CHECK(packet->GetPassengerRoutingMode() == (fallback ? PassengerRoutingMode::Fallback : PassengerRoutingMode::Legacy));
	std::array next{D};
	CHECK(fixture.Waiting().Load(10, &train.cargo, next, TileIndex{0}, &train) == 10);
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
	SECTION("Old save defaults to Legacy") { _sl_xv_feature_versions[XSLFI_PASSENGER_JOURNEY_ROUTING] = 0; expected = PassengerRoutingMode::Legacy; }
	auto loaded = std::unique_ptr<CargoPacket>(CargoPacket::Create());
	REQUIRE_FALSE(SlTestSaveLoadTable(packet.get(), loaded.get(), GetCargoPacketDesc()).empty());
	CHECK(loaded->GetPassengerRoutingMode() == expected); CHECK(loaded->GetPassengerDestination() == D);
	CHECK(loaded->Count() == 10); CHECK(loaded->GetFeederShare() == 100);
	if (expected == PassengerRoutingMode::Journey) CHECK(loaded->GetPassengerJourney() == packet->GetPassengerJourney());
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
	CHECK(context.GetWorkAllowanceUsed() <= 2097152);
	auto reachable = std::unique_ptr<CargoPacket>(fixture.Packet());
	CHECK_FALSE(context.PlanWaiting(*reachable, PAX, D)); // Budget exhaustion is inconclusive.
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
