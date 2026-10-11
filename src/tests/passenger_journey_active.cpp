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
	PassengerJourneyLoadingContext loading;
	CHECK(fixture.Waiting().HasCargoFor(next, &train, &loading));
	CHECK(fixture.Waiting().Reserve(100, &train.cargo, next, TileIndex{0}, &train, &loading) == 100);
	CHECK(fixture.Waiting().Reserve(100, &coach.cargo, next, TileIndex{0}, &coach, &loading) == 100);
	CHECK(fixture.Waiting().AvailableCount() == 100);
	CHECK(fixture.Waiting().ReservedCount() == 200);
	for (uint i = 0; i < 5; ++i) {
		CHECK(fixture.Waiting().Load(20, &train.cargo, next, TileIndex{0}, &train, &loading) == 20);
		CHECK(fixture.Waiting().Load(20, &coach.cargo, next, TileIndex{0}, &coach, &loading) == 20);
	}
	CHECK(train.cargo.StoredCount() == 100); CHECK(coach.cargo.StoredCount() == 100);
	CHECK(fixture.Waiting().ReservedCount() == 0);
	CHECK(fixture.Waiting().AvailableCount() == 100);
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

TEST_CASE("Rotating waiting shares survive a hot origin and local resets", "[passenger][active][budget][determinism]")
{
	ActiveFixture fixture;
	std::vector<Order> orders{ActiveStop(A, 100000, 100)};
	for (uint16_t station = 1; station <= 32; ++station) {
		if (station >= 4) Station::CreateAtIndex(StationID{station}, TileIndex{0})->owner = OWNER_NONE;
		orders.push_back(ActiveStop(StationID{station}, 1, 1));
	}
	ActiveTrain train(std::move(orders));
	std::array<CargoPacket *, 32> packets;
	for (uint16_t station = 0; station < packets.size(); ++station) {
		packets[station] = fixture.Packet(1, StationID{32});
		fixture.Waiting(StationID{station}).Append(packets[station], D);
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
			/* A consumes every ordinary admission it can before other origins run. */
			for (uint repeat = 0; repeat < 40; ++repeat) {
				auto hot = std::unique_ptr<CargoPacket>(fixture.Packet(1, StationID{static_cast<uint16_t>(1 + repeat % 32)}));
				plan(*hot, A, D);
			}
			for (uint16_t station = 1; station < packets.size(); ++station) if (!served[station]) {
				served[station] = plan(*packets[station], StationID{station}, D);
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
