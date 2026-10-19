/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
/** @file passenger_multimodal.cpp Real vehicle adapters, mixed journeys and cargo execution regressions. */
#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../passenger_journey_active.h"
#include "../passenger_journey_shadow.h"
#include "../train.h"
#include "../roadveh.h"
#include "../road.h"
#include "../ship.h"
#include "../aircraft.h"
#include "../airport.h"
#include "../roadstop_base.h"
#include "../station_base.h"
#include "../economy_base.h"
#include "../company_base.h"
#include "../town.h"
#include "../date_func.h"
#include "../passenger_service_database.h"
#include "../core/backup_type.hpp"
#include "../core/random_func.hpp"
#include "../sl/saveload.h"
#include "../linkgraph/linkgraph.h"
#include "passenger_async_access.h"
#include <chrono>

static constexpr CargoType PAX{0}, MAIL{1};
static constexpr StationID A{0}, B{1}, C{2}, D{3}, E{4};
static constexpr StateTicks NOW{64000};
using Action = VehicleCargoList::MoveToAction;
extern SaveLoadVersion _sl_version;
extern std::vector<uint8_t> SlTestSaveLoadTable(void *, void *, const NamedSaveLoadTable &);

static Order MultiStop(StationID station, TimetableTicks travel = 10000, TimetableTicks wait = 100)
{
	Order order;
	order.MakeGoToStation(station);
	order.SetTravelTime(travel); order.SetTravelTimetabled(true);
	order.SetWaitTime(wait); order.SetWaitTimetabled(true);
	return order;
}

/** Real specialized vehicle layouts and map facilities; no pathfinder/engine callbacks are mocked by routing.
 * Pool cleanup suppresses unrelated engine/group/GUI destruction for these deliberately minimal vehicles. */
struct MultimodalFixture {
	AutoRestoreBackup<CargoClasses> classes{CargoSpec::Get(PAX)->classes, CargoClasses{CargoClass::Passengers}};
	AutoRestoreBackup<CargoClasses> mail_classes{CargoSpec::Get(MAIL)->classes, CargoClasses{CargoClass::Mail}};
	AutoRestoreBackup<uint8_t> bitnum{CargoSpec::Get(PAX)->bitnum, uint8_t{0}};
	AutoRestoreBackup<LinkGraphSettings> settings{_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<StateTicks> ticks{_state_ticks, NOW};
	AutoRestoreBackup<Money> payment{CargoSpec::Get(PAX)->current_payment, Money{0}};
	MultimodalFixture()
	{
		REQUIRE(Station::GetNumItems() == 0); REQUIRE(Vehicle::GetNumItems() == 0);
		AllocateMap(256, 256);
		ResetRoadTypes();
		AirportSpec::ResetAirports();
		_settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Asymmetric;
		_settings_game.linkgraph.passenger_journey_routing = true;
		_settings_game.linkgraph.passenger_journey_periodic = false; // Reference snapshot backend.
		_settings_game.linkgraph.passenger_transfer_timetables = false;
		_settings_game.linkgraph.passenger_transfer_penalty = 20;
		for (StationID id : {A, B, C, D, E}) this->StationAt(id);
		BeginPassengerJourneyTick();
	}
	~MultimodalFixture()
	{
		SetPassengerJourneyShadowEnabled(false);
		_vehicle_pool.CleanPool(); _orderlist_pool.CleanPool(); _order_pool.CleanPool();
		_station_pool.CleanPool(); _roadstop_pool.CleanPool(); _cargopacket_pool.CleanPool();
		_town_pool.CleanPool(); _company_pool.CleanPool(); ClearCargoPacketDeferredPayments();
		RebuildPassengerJourneyVehicleIndex();
	}
	Station *StationAt(StationID id)
	{
		if (Station::IsValidID(id)) return Station::Get(id);
		TileIndex road{uint32_t{256} + uint32_t{8} * id.base()};
		Station *st = Station::CreateAtIndex(id, road);
		st->owner = OWNER_NONE;
		st->facilities = {StationFacility::Train, StationFacility::BusStop, StationFacility::Dock, StationFacility::Airport};
		MakeDriveThroughRoadStop(road, OWNER_NONE, OWNER_NONE, OWNER_NONE, id, StationType::Bus, ROADTYPE_ROAD, INVALID_ROADTYPE, Axis::X);
		st->bus_stops = RoadStop::Create(road);
		/* A water-side dock and its adjacent docking tile, on the same StationID. */
		MakeDock(road + 2, OWNER_NONE, id, DiagDirection::SW, WaterClass::Sea);
		MakeSea(road + 4); SetDockingTile(road + 4, true);
		st->docking_station.Add(road + 4);
		st->airport.type = AT_SMALL;
		st->airport.tile = road + 5;
		MakeAirport(st->airport.tile, OWNER_NONE, id, 0, WaterClass::Invalid);
		return st;
	}
	void LoadAt(Vehicle *v, VehicleOrderID index)
	{
		v->cur_real_order_index = v->cur_implicit_order_index = index;
		v->cur_timetable_order_index = INVALID_VEH_ORDER_ID;
		v->last_station_visited = v->GetOrder(index)->GetDestination().ToStationID();
		v->current_order.MakeLoading(false); v->current_order_time = 0;
		Station *st = this->StationAt(v->last_station_visited);
		v->tile = st->xy; v->direction = Direction::NE;
		if (v->type == VehicleType::Road) RoadVehicle::From(v)->state = RVSB_IN_DT_ROAD_STOP | (1 << RVS_ENTERED_STOP);
		if (v->type == VehicleType::Ship) v->tile = st->xy + 4;
		if (v->type == VehicleType::Aircraft) { v->tile = st->airport.tile; Aircraft::From(v)->targetairport = st->index; Aircraft::From(v)->state = TERM1; }
	}
	Vehicle *Service(VehicleType type, std::vector<Order> orders)
	{
		Vehicle *v = nullptr;
		switch (type) {
			case VehicleType::Train: { auto *t = Vehicle::Create<Train>(); t->SetFrontEngine(); v = t; break; }
			case VehicleType::Road: { auto *r = Vehicle::Create<RoadVehicle>(); r->SetFrontEngine(); r->compatible_roadtypes.Set(ROADTYPE_ROAD); v = r; break; }
			case VehicleType::Ship: { auto *s = Vehicle::Create<Ship>(); s->state = TRACK_BIT_X; v = s; break; }
			case VehicleType::Aircraft: { auto *a = Vehicle::Create<Aircraft>(); a->subtype = AIR_AIRCRAFT; v = a; break; }
			default: FAIL("Unsupported test vehicle");
		}
		for (const Order &order : orders) if (order.IsType(OT_GOTO_STATION)) this->StationAt(order.GetDestination().ToStationID());
		v->orders = OrderList::Create(std::move(orders), v);
		v->cargo_type = PAX; v->cargo_cap = 100;
		this->LoadAt(v, 0);
		return v;
	}
	PassengerServiceGraph Graph(Ticks horizon = 1000)
	{
		PassengerServiceGraphLimits limits; limits.horizon = horizon;
		return PassengerServiceGraph::Build(PAX, _state_ticks, limits);
	}
	StationCargoList &Waiting(StationID station = A) { return Station::Get(station)->goods[PAX].CreateData().cargo; }
	CargoPacket *Packet(uint16_t count = 10, StationID destination = E)
	{
		auto *packet = CargoPacket::Create(count, 7, A, TileIndex{0}, Money{100});
		REQUIRE(packet->SetPassengerDestination(PAX, destination));
		return packet;
	}
};

struct AircraftCostFixture : MultimodalFixture {
	AutoRestoreBackup<uint8_t> days{DateDetail::_effective_day_length, uint8_t{1}};
	AircraftCostFixture()
	{
		_settings_game.linkgraph.aircraft_link_scale = 100;
		_settings_game.linkgraph.aircraft_link_add = 0;
		_settings_game.linkgraph.passenger_transfer_penalty = 0;
		_settings_game.linkgraph.passenger_journey_periodic = true;
		_settings_game.linkgraph.passenger_journey_precomputed = false;
		ResetPassengerServiceDatabase();
	}
	~AircraftCostFixture() { ResetPassengerServiceDatabase(); }
	PassengerServiceDatabase &Build()
	{
		auto &db = GetPassengerServiceDatabase();
		for (int i = 0; i < 128 && db.IsBuilding(); ++i) db.Tick();
		REQUIRE_FALSE(db.IsBuilding());
		return db;
	}
};

TEST_CASE("Aircraft preferences choose the same corridor in every passenger planner", "[passenger][aircraft_cost][multimodal]")
{
	AircraftCostFixture f;
	const uint16_t scale = GENERATE(50, 100, 300);
	_settings_game.linkgraph.aircraft_link_scale = scale;
	auto *air = f.Service(VehicleType::Aircraft, {MultiStop(A, 1000, 10), MultiStop(E, 100, 10)});
	auto *rail = f.Service(VehicleType::Train, {MultiStop(A, 1000, 10), MultiStop(E, 80, 10)});
	auto *expected = scale == 50 ? air : rail;
	auto graph = f.Graph(); PassengerJourneyPlanner timed(graph);
	PassengerJourneyLimits limits; limits.candidates = 1;
	auto route = timed.Plan(PAX, A, E, NOW, limits);
	REQUIRE(route.HasJourney()); CHECK(route.journeys.front().legs.front().vehicle == expected->index);
	auto &db = f.Build();
	auto periodic = db.Plan(PAX, A, E, NOW, limits);
	REQUIRE(periodic.ride); CHECK(periodic.ride->service == expected->orders->index);
	CHECK(periodic.cost == route.journeys.front().generalized_time.base());
	CHECK(periodic.arrival == route.journeys.front().arrival);
	CHECK(db.PlanWithoutServicePruning(PAX, A, E, NOW, limits).route == periodic.route);
	std::array records{db.Get(air->orders->index), db.Get(rail->orders->index)};
	for (bool hyperpath : {false, true}) {
		PassengerPrecomputedTable table;
		table.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, hyperpath);
		while (!table.Finished()) table.Step(65536);
		if (hyperpath) {
			auto candidates = table.InspectRoutes(A, E, OrderListID::Invalid(), OrderListID::Invalid(), limits.work);
			REQUIRE_FALSE(candidates.candidates.empty());
			CHECK(std::any_of(candidates.candidates.begin(), candidates.candidates.end(), [&](const auto &c) { return c.ride.service == expected->orders->index; }));
		} else {
			auto plan = table.Plan(A, E, NOW, OrderListID::Invalid(), UINT32_MAX); REQUIRE(plan.ride); CHECK(plan.ride->service == expected->orders->index);
		}
	}
}

TEST_CASE("Aircraft through segments change preference without changing clocks or dwell", "[passenger][aircraft_cost][journey][periodic]")
{
	AircraftCostFixture f; DateDetail::_effective_day_length = 3;
	_settings_game.linkgraph.aircraft_link_scale = 50; _settings_game.linkgraph.aircraft_link_add = 2;
	auto *air = f.Service(VehicleType::Aircraft, {MultiStop(A, 1000, 10), MultiStop(B, 100, 30), MultiStop(E, 200, 10)});
	auto graph = f.Graph(); PassengerJourneyPlanner timed(graph);
	auto route = timed.Plan(PAX, A, E, NOW);
	REQUIRE(route.HasJourney()); const auto &journey = route.journeys.front();
	CHECK(journey.arrival == NOW + 340); CHECK(journey.riding.base() == 330);
	CHECK(journey.generalized_time.base() == 340 - 150 + 2 * 2 * DAY_TICKS * 3);
	auto &db = f.Build(); auto periodic = db.Plan(PAX, A, E, NOW);
	REQUIRE(periodic.ride); CHECK(periodic.arrival == journey.arrival); CHECK(periodic.cost == journey.generalized_time.base());
	std::array records{db.Get(air->orders->index)};
	for (bool hyperpath : {false, true}) {
		PassengerPrecomputedTable table;
		table.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, hyperpath);
		while (!table.Finished()) table.Step(65536);
		auto plan = table.Plan(A, E, NOW); REQUIRE(plan.ride);
		CHECK(plan.riding == 330); // Includes the unscaled intermediate dwell.
		CHECK(plan.cost - plan.waiting >= 330 - 150 + 4 * DAY_TICKS * 3);
	}
	_state_ticks = NOW + 110; f.LoadAt(air, 1);
	auto onboard_graph = f.Graph(); PassengerJourneyPlanner onboard(onboard_graph);
	auto visit = onboard_graph.FindOnboardVisit(air->index, B, 1); REQUIRE(visit);
	auto remaining = onboard.PlanOnboard(PAX, *visit, E); REQUIRE(remaining.HasJourney());
	CHECK(remaining.journeys.front().arrival == NOW + 340);
	CHECK(remaining.journeys.front().generalized_time.base() == 30 + 100 + 2 * DAY_TICKS * 3);
}

TEST_CASE("Large aircraft preference costs remain physically reachable in active Timed routing", "[passenger][aircraft_cost][active]")
{
	AircraftCostFixture f;
	_settings_game.linkgraph.passenger_journey_periodic = false;
	_settings_game.linkgraph.aircraft_link_add = 365;
	DateDetail::_effective_day_length = 255;
	auto *air = f.Service(VehicleType::Aircraft, {MultiStop(A, 1000, 10), MultiStop(E, 100, 10)});
	auto *packet = f.Packet(); PassengerJourneyActiveContext context(nullptr, A);
	REQUIRE(context.PlanWaiting(*packet, PAX, E));
	CHECK(packet->GetPassengerJourney().vehicle == air->index);
	CHECK(packet->GetPassengerJourney().arrival == NOW + 110);
}

TEST_CASE("Discounted flight suffixes keep admissible bounds after a rail connection", "[passenger][aircraft_cost][pruning]")
{
	AircraftCostFixture f; _settings_game.linkgraph.aircraft_link_scale = 10;
	auto *rail = f.Service(VehicleType::Train, {MultiStop(A, 10000, 0), MultiStop(B, 100, 0)});
	f.Service(VehicleType::Aircraft, {MultiStop(B, 10000, 100), MultiStop(E, 500, 0)});
	f.Service(VehicleType::Train, {MultiStop(A, 10000, 0), MultiStop(E, 200, 0)});
	auto graph = f.Graph(1000); PassengerJourneyPlanner planner(graph);
	PassengerJourneyLimits limits; limits.candidates = 1;
	auto timed = planner.Plan(PAX, A, E, NOW, limits); REQUIRE(timed.HasJourney());
	CHECK(timed.journeys.front().legs.size() == 2); CHECK(timed.journeys.front().legs.front().vehicle == rail->index);
	CHECK(timed.journeys.front().arrival == NOW + 600); CHECK(timed.journeys.front().generalized_time.base() == 150);
	auto &db = f.Build(); auto periodic = db.Plan(PAX, A, E, NOW, limits); REQUIRE(periodic.ride);
	CHECK(periodic.route.size() == 2); CHECK(periodic.cost == 150); CHECK(periodic.arrival == NOW + 600);
	CHECK(db.PlanWithoutServicePruning(PAX, A, E, NOW, limits).route == periodic.route);
}

TEST_CASE("Aircraft changes discard old tables and preserve captured service generations", "[passenger][aircraft_cost][precomputed][save][network]")
{
	AircraftCostFixture f;
	auto *air = f.Service(VehicleType::Aircraft, {MultiStop(A, 1000, 10), MultiStop(E, 100, 10)});
	auto *rail = f.Service(VehicleType::Train, {MultiStop(A, 1000, 10), MultiStop(E, 150, 10)});
	_settings_game.linkgraph.passenger_journey_precomputed = true;
	for (bool hyperpath : {false, true}) {
		_settings_game.linkgraph.passenger_journey_hyperpath = hyperpath;
		ResetPassengerServiceDatabase(); auto &db = f.Build();
		for (int i = 0; !db.PrecomputedFinished() && i < 128; ++i) db.Tick();
		REQUIRE(db.PrecomputedFinished());
		const auto generation = db.Get(air->orders->index)->generation;
		REQUIRE(db.Plan(PAX, A, E, NOW).ride->service == air->orders->index);
		_settings_game.linkgraph.aircraft_link_add = 30;
		PassengerAircraftCostSettingsChanged(0);
		CHECK_FALSE(db.PrecomputedFinished());
		CHECK(db.Get(air->orders->index)->generation == generation);
		auto changed = db.Plan(PAX, A, E, NOW); REQUIRE(changed.ride); CHECK(changed.ride->service == rail->orders->index);
		// Save during graph preparation and replay with another local worker setting.
		db.Tick(1, true, true); const auto wire = db.Save();
		PassengerServiceDatabase loaded; REQUIRE(loaded.Load(wire)); loaded.FinishLoad(true);
		CHECK(loaded.Save() == wire);
		for (int i = 0; !db.PrecomputedFinished() && i < 128; ++i) db.Tick();
		REQUIRE(db.PrecomputedFinished()); auto plan = db.PlanPrecomputed(PAX, A, E, NOW, OrderListID::Invalid(), UINT32_MAX); REQUIRE(plan.ride);
		if (!hyperpath) CHECK(plan.ride->service == rail->orders->index);
		_settings_game.linkgraph.aircraft_link_add = 0; PassengerAircraftCostSettingsChanged(0);
	}
}

TEST_CASE("A discounted later arrival survives periodic dominance before a shared connection", "[passenger][aircraft_cost][pruning]")
{
	AircraftCostFixture f; _settings_game.linkgraph.aircraft_link_scale = 60;
	f.Service(VehicleType::Train, {MultiStop(A, 10000, 0), MultiStop(B, 100, 0)});
	auto *air = f.Service(VehicleType::Aircraft, {MultiStop(A, 10000, 0), MultiStop(B, 200, 0)});
	f.Service(VehicleType::Train, {MultiStop(B, 10000, 300), MultiStop(E, 100, 0)});
	f.Service(VehicleType::Train, {MultiStop(A, 10000, 0), MultiStop(E, 350, 0)});
	auto &db = f.Build(); auto plan = db.Plan(PAX, A, E, NOW);
	REQUIRE(plan.ride); CHECK(plan.ride->service == air->orders->index); CHECK(plan.cost == 320); CHECK(plan.arrival == NOW + 400);
}

TEST_CASE("Penalized untimed aircraft respect the existing CargoDist transfer hop", "[passenger][aircraft_cost][fallback]")
{
	AircraftCostFixture f; _settings_game.linkgraph.aircraft_link_add = 2;
	auto *air = f.Service(VehicleType::Aircraft, {MultiStop(A), MultiStop(E, 100)});
	auto *rail = f.Service(VehicleType::Train, {MultiStop(A), MultiStop(B, 100)});
	Station::Get(A)->goods[PAX].CreateData().flows.insert(FlowStat(A, B, 10));
	auto *packet = f.Packet(); packet->FallbackPassengerJourney(A, NOW);
	PassengerOdFallbackContext fallback;
	CHECK_FALSE(fallback.CanBoard(*packet, air, PAX)); CHECK(fallback.CanBoard(*packet, rail, PAX));
}

TEST_CASE("Aircraft edits cancel speculative old rows before publishing their replacements", "[passenger][aircraft_cost][parallel][network]")
{
	AircraftCostFixture f;
	auto *air = f.Service(VehicleType::Aircraft, {MultiStop(A, 1000, 10), MultiStop(B, 50, 10), MultiStop(E, 50, 10)});
	auto *rail = f.Service(VehicleType::Train, {MultiStop(A, 1000, 10), MultiStop(E, 200, 10)});
	auto &db = f.Build(); _settings_game.linkgraph.passenger_journey_precomputed = true;
	for (bool hyperpath : {false, true}) {
		_settings_game.linkgraph.passenger_journey_hyperpath = hyperpath;
		_settings_game.linkgraph.aircraft_link_add = 0; PassengerAircraftCostSettingsChanged(0);
		PassengerServiceDatabaseTestAccess::BeginReplacement(db, 0);
		PassengerPrecomputedTestAccess::Prime(PassengerServiceDatabaseTestAccess::Table(db, PAX), 4);
		_settings_game.linkgraph.aircraft_link_add = 30; PassengerAircraftCostSettingsChanged(0);
		CHECK_FALSE(db.PrecomputedFinished());
		for (int i = 0; !db.PrecomputedFinished() && i < 128; ++i) db.Tick();
		REQUIRE(db.PrecomputedFinished());
		std::array records{db.Get(air->orders->index), db.Get(rail->orders->index)};
		PassengerPrecomputedTable reference;
		reference.Begin(records, PAX, 0, PassengerPrecomputedTable::MAX_BYTES, true, false, {}, true, true, true, hyperpath);
		while (!reference.Finished()) reference.Step(65536);
		CHECK(db.PrecomputedRouteCost(PAX, A, E) == reference.RouteCost(A, E));
		CHECK(db.Plan(PAX, A, E, NOW).ride->service == rail->orders->index);
	}
}

TEST_CASE("Aircraft additions cannot overflow an extreme periodic departure wait", "[passenger][aircraft_cost][periodic]")
{
	AircraftCostFixture f; DateDetail::_effective_day_length = 255;
	_settings_game.linkgraph.aircraft_link_add = 365;
	auto *air = f.Service(VehicleType::Aircraft, {MultiStop(A, 1000, 10), MultiStop(E, 100, 10)});
	auto &db = f.Build();
	air->current_order.Free(); // Keep the captured phase; no current-loading override.
	auto *captured = const_cast<PassengerPeriodicService *>(db.Get(air->orders->index)); REQUIRE(captured);
	captured->vehicles.front().base = StateTicks{INT64_MAX - 1000};
	auto plan = db.Plan(PAX, A, E, NOW);
	CHECK_FALSE(plan.ride); CHECK(plan.observations[static_cast<size_t>(PassengerPeriodicPlan::Reason::Evaluation)]);
}

TEST_CASE("Every passenger transport uses the same planner and production boarding", "[passenger][multimodal][active]")
{
	MultimodalFixture f;
	VehicleType type = GENERATE(VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft);
	Vehicle *chosen = f.Service(type, {MultiStop(A), MultiStop(E, 100)});
	Vehicle *other = f.Service(type, {MultiStop(A, 10000, 200), MultiStop(E, 100)});
	const auto graph = f.Graph();
	REQUIRE(graph.GetTrips().size() == 2); CHECK(graph.GetTrips()[0].type == type);
	PassengerJourneyPlanner planner(graph);
	auto result = planner.Plan(PAX, A, E, NOW);
	REQUIRE(result.HasJourney()); CHECK(result.journeys.front().legs.front().vehicle == chosen->index);
	auto *packet = f.Packet(250);
	PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, E));
	f.Waiting().Append(packet, E);
	std::array next{E};
	CHECK(f.Waiting().Load(100, &other->cargo, next, other->tile, other) == 0);
	PassengerJourneyLoadingContext loading;
	REQUIRE(f.Waiting().Reserve(100, &chosen->cargo, next, chosen->tile, chosen, &loading) == 100);
	for (uint i = 0; i < 5; ++i) CHECK(f.Waiting().Load(20, &chosen->cargo, next, chosen->tile, chosen, &loading) == 20);
	CHECK(loading.GetPredictionCount() == 1);
	CHECK(f.Waiting().AvailableCount() == 150); CHECK(f.Waiting().ReservedCount() == 0);
	CHECK(chosen->cargo.StoredCount() == 100);
	CHECK(chosen->cargo.Packets()->front()->GetFeederShare() + packet->GetFeederShare() == 100);
	CHECK(packet->GetPassengerDestination() == E);
	Company *company = Company::Create(); Town *town = Town::Create(TileIndex{0});
	chosen->owner = company->index; Station::Get(E)->town = town; Station::Get(E)->always_accepted.Set(PAX);
	f.LoadAt(chosen, 1);
	CargoPayment payment(CargoPaymentID::Invalid(), chosen);
	PassengerJourneyActiveContext arrived(chosen, E);
	chosen->cargo.Stage(true, E, next, OrderUnloadType::UnloadIfPossible, &Station::Get(E)->goods[PAX], PAX, &payment,
			chosen->tile, nullptr, nullptr, &arrived);
	CHECK(chosen->cargo.ActionCount(Action::Deliver) == 100);
	CHECK(chosen->cargo.Unload(100, &f.Waiting(E), PAX, &payment, chosen->tile) == 100);
	CHECK(company->cur_economy.delivered_cargo[PAX] == 100);
	CHECK(chosen->cargo.TotalCount() == 0); CHECK(f.Waiting(E).TotalCount() == 0);
}

TEST_CASE("Periodic service intents use all real transport adapters and gradual reservations", "[passenger][periodic][multimodal][active]")
{
	MultimodalFixture f; _settings_game.linkgraph.passenger_journey_periodic = true;
	bool estimated = GENERATE(false, true);
	VehicleType type = GENERATE(VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft);
	Vehicle *vehicle = f.Service(type, {MultiStop(A), MultiStop(E, 100)});
	if (estimated) vehicle->current_order.Free();
	ResetPassengerServiceDatabase(); GetPassengerServiceDatabase().Tick();
	auto *packet = f.Packet(250); PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, E));
	if (estimated) f.LoadAt(vehicle, 0);
	f.Waiting().Append(packet, StationID::Invalid()); _state_ticks += 5000; vehicle->current_order_time += 5000;
	std::array next{E}; PassengerJourneyLoadingContext loading;
	REQUIRE(f.Waiting().Reserve(100, &vehicle->cargo, next, vehicle->tile, vehicle, &loading) == 100);
	for (int i = 0; i < 5; ++i) CHECK(f.Waiting().Load(20, &vehicle->cargo, next, vehicle->tile, vehicle, &loading) == 20);
	CHECK(loading.GetPredictionCount() == 0); CHECK(vehicle->cargo.StoredCount() == 100);
	CHECK(f.Waiting().AvailableCount() == 150); CHECK(packet->GetPassengerJourney().kind == (estimated ? PassengerRideKind::EstimatedServiceIntent : PassengerRideKind::ServiceIntent));
	Company *company = Company::Create(); Town *town = Town::Create(TileIndex{0});
	vehicle->owner = company->index; Station::Get(E)->town = town; Station::Get(E)->always_accepted.Set(PAX);
	f.LoadAt(vehicle, 1); CargoPayment payment(CargoPaymentID::Invalid(), vehicle);
	PassengerJourneyActiveContext arrived(vehicle, E);
	vehicle->cargo.Stage(true, E, next, OrderUnloadType::UnloadIfPossible, &Station::Get(E)->goods[PAX], PAX, &payment,
			vehicle->tile, nullptr, nullptr, &arrived);
	CHECK(vehicle->cargo.ActionCount(Action::Deliver) == 100);
	CHECK(vehicle->cargo.Unload(100, &f.Waiting(E), PAX, &payment, vehicle->tile) == 100);
	CHECK(company->cur_economy.delivered_cargo[PAX] == 100); CHECK(GetPassengerJourneyTickDiagnostic().builds == 0);
	CHECK(GetPassengerJourneyTickDiagnostic().schedule_boarded_passengers == (estimated ? 100 : 0));
	CHECK(GetPassengerJourneyTickDiagnostic().schedule_delivered_passengers == (estimated ? 100 : 0));
}

TEST_CASE("Periodic transfers through all four modes retain the final destination", "[passenger][periodic][multimodal][delivery]")
{
	MultimodalFixture f; _settings_game.linkgraph.passenger_journey_periodic = true; _settings_game.linkgraph.passenger_transfer_penalty = 0;
	ResetPassengerJourneyDiagnostic();
	bool estimated = GENERATE(false, true);
	std::array modes{VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft};
	std::array<Vehicle *, 4> services;
	for (size_t i = 0; i < services.size(); ++i) services[i] = f.Service(modes[i], {MultiStop(StationID{static_cast<uint16_t>(i)}, 10000, static_cast<TimetableTicks>(100 + 100 * i)), MultiStop(StationID{static_cast<uint16_t>(i + 1)}, 10)});
	if (estimated) for (auto *v : services) v->current_order.Free();
	ResetPassengerServiceDatabase(); GetPassengerServiceDatabase().Tick();
	auto *packet = f.Packet(); PassengerJourneyActiveContext planned(nullptr, A); REQUIRE(planned.PlanWaiting(*packet, PAX, B)); f.Waiting(A).Append(packet, StationID::Invalid());
	Company *company = Company::Create(); Town *town = Town::Create(TileIndex{0}); Station::Get(E)->town = town; Station::Get(E)->always_accepted.Set(PAX);
	for (size_t i = 0; i < services.size(); ++i) {
		Vehicle *v = services[i]; StationID to{static_cast<uint16_t>(i + 1)}; std::array next{to};
		REQUIRE(packet->GetPassengerJourney().kind == (estimated ? PassengerRideKind::EstimatedServiceIntent : PassengerRideKind::ServiceIntent)); REQUIRE(packet->GetPassengerJourney().service == v->orders->index);
		if (estimated) f.LoadAt(v, 0);
		CHECK(f.Waiting(StationID{static_cast<uint16_t>(i)}).Load(10, &v->cargo, next, v->tile, v) == 10);
		_state_ticks = NOW + 110 + 100 * i; f.LoadAt(v, 1); v->owner = company->index; CargoPayment payment(CargoPaymentID::Invalid(), v);
		PassengerJourneyActiveContext arrived(v, to);
		v->cargo.Stage(to == E, to, next, OrderUnloadType::UnloadIfPossible, &Station::Get(to)->goods[PAX], PAX, &payment, v->tile, nullptr, nullptr, &arrived);
		CHECK(v->cargo.ActionCount(to == E ? Action::Deliver : Action::Transfer) == 10);
		CHECK(v->cargo.Unload(10, &f.Waiting(to), PAX, &payment, v->tile) == 10);
		if (to != E) CHECK(packet->GetPassengerDestination() == E);
	}
	CHECK(company->cur_economy.delivered_cargo[PAX] == 10); CHECK(GetPassengerJourneyTickDiagnostic().builds == 0);
	CHECK(GetPassengerJourneyTotalDiagnostic().schedule_transferred_passengers == (estimated ? 30 : 0));
	CHECK(GetPassengerJourneyTotalDiagnostic().schedule_delivered_passengers == (estimated ? 10 : 0));
	CHECK(GetPassengerJourneyTotalDiagnostic().schedule_connected_passengers == (estimated ? 30 : 0));
	CHECK(GetPassengerJourneyTotalDiagnostic().timed_connected_passengers == (estimated ? 0 : 30));
	CHECK(GetPassengerJourneyTotalDiagnostic().schedule_assisted_delivered_passengers == (estimated ? 10 : 0));
}

TEST_CASE("A schedule leg connects to a timed carrier and reaches the final OD", "[passenger][schedule][multimodal][delivery]")
{
	MultimodalFixture f; _settings_game.linkgraph.passenger_journey_periodic = true; ResetPassengerJourneyDiagnostic();
	auto *first = f.Service(VehicleType::Train, {MultiStop(A), MultiStop(B, 100)});
	auto *next = f.Service(VehicleType::Road, {MultiStop(B), MultiStop(E, 100)});
	first->current_order.Free(); ResetPassengerServiceDatabase(); GetPassengerServiceDatabase().Tick();
	auto *packet = f.Packet(); REQUIRE(PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*packet, PAX, B));
	REQUIRE(packet->GetPassengerJourney().kind == PassengerRideKind::EstimatedServiceIntent);
	f.Waiting(A).Append(packet, StationID::Invalid()); f.LoadAt(first, 0); std::array hop{B};
	REQUIRE(f.Waiting(A).Load(10, &first->cargo, hop, first->tile, first) == 10);
	Company *company = Company::Create(); Town *town = Town::Create(TileIndex{0});
	first->owner = next->owner = company->index; Station::Get(E)->town = town; Station::Get(E)->always_accepted.Set(PAX);
	_state_ticks += 120; f.LoadAt(first, 1); CargoPayment transfer(CargoPaymentID::Invalid(), first);
	PassengerJourneyActiveContext interchange(first, B); std::array destination{E};
	first->cargo.Stage(false, B, destination, OrderUnloadType::UnloadIfPossible, &Station::Get(B)->goods[PAX], PAX, &transfer, first->tile, nullptr, nullptr, &interchange);
	REQUIRE(first->cargo.Unload(10, &f.Waiting(B), PAX, &transfer, first->tile) == 10);
	REQUIRE(packet->GetPassengerJourney().kind == PassengerRideKind::ServiceIntent); CHECK(packet->HasUsedPassengerSchedule());
	{
		AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION}; AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
		AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
		SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
		auto *loaded = CargoPacket::Create(); REQUIRE_FALSE(SlTestSaveLoadTable(packet, loaded, GetCargoPacketDesc()).empty()); CHECK(loaded->HasUsedPassengerSchedule()); CHECK(loaded->HasSamePassengerRouting(*packet));
	}
	CHECK(f.Waiting(B).Load(10, &next->cargo, destination, next->tile, next) == 0); _state_ticks += 20;
	REQUIRE(f.Waiting(B).Load(10, &next->cargo, destination, next->tile, next) == 10);
	f.LoadAt(next, 1); _state_ticks += 100; CargoPayment final_payment(CargoPaymentID::Invalid(), next);
	PassengerJourneyActiveContext arrived(next, E);
	next->cargo.Stage(true, E, destination, OrderUnloadType::UnloadIfPossible, &Station::Get(E)->goods[PAX], PAX, &final_payment, next->tile, nullptr, nullptr, &arrived);
	REQUIRE(next->cargo.Unload(10, &f.Waiting(E), PAX, &final_payment, next->tile) == 10);
	auto d = GetPassengerJourneyTotalDiagnostic(); CHECK(d.schedule_connected_passengers == 10);
	CHECK(d.schedule_delivered_passengers == 0); CHECK(d.delivered_passengers == 10); CHECK(d.schedule_assisted_delivered_passengers == 10);
	CHECK(company->cur_economy.delivered_cargo[PAX] == 10);
}

TEST_CASE("An unknown phase cannot advertise an incompatible physical facility", "[passenger][schedule][multimodal]")
{
	MultimodalFixture f; _settings_game.linkgraph.passenger_journey_periodic = true;
	auto *train = f.Service(VehicleType::Train, {MultiStop(A), MultiStop(E, 1)});
	auto *road = f.Service(VehicleType::Road, {MultiStop(A), MultiStop(E, 100)});
	train->current_order.Free(); road->current_order.Free(); Station::Get(E)->facilities.Reset(StationFacility::Train);
	ResetPassengerServiceDatabase(); auto &db = GetPassengerServiceDatabase(); db.Tick();
	CHECK(db.Get(train->orders->index)->vehicles.empty());
	auto plan = db.PlanEstimated(PAX, A, E, NOW); REQUIRE(plan.ride); CHECK(plan.ride->service == road->orders->index);
}

TEST_CASE("An unknown aircraft phase cannot conceal a known range restriction", "[passenger][schedule][multimodal]")
{
	MultimodalFixture f; _settings_game.linkgraph.passenger_journey_periodic = true;
	auto *air = f.Service(VehicleType::Aircraft, {MultiStop(A), MultiStop(E, 1)});
	auto *road = f.Service(VehicleType::Road, {MultiStop(A), MultiStop(E, 100)});
	Aircraft::From(air)->flags.Set(VehicleAirFlag::DestinationTooFar); road->current_order.Free();
	ResetPassengerServiceDatabase(); auto &db = GetPassengerServiceDatabase(); db.Tick();
	CHECK(db.Get(air->orders->index)->vehicles.empty());
	auto plan = db.PlanEstimated(PAX, A, E, NOW); REQUIRE(plan.ride); CHECK(plan.ride->service == road->orders->index);
}

TEST_CASE("Trip cache validates every real transport adapter", "[passenger][multimodal][trip_cache][determinism]")
{
	MultimodalFixture f;
	VehicleType type = GENERATE(VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft);
	Vehicle *v = f.Service(type, {MultiStop(A), MultiStop(E, 100)});
	PassengerServiceCapture capture;
	PassengerServiceGraphLimits limits; limits.horizon = 1000;
	const auto first = PassengerServiceGraph::Build(PAX, NOW, limits, {}, &capture);
	const auto hit = PassengerServiceGraph::Build(PAX, NOW, limits, {}, &capture);
	REQUIRE(hit.GetDiagnostic().prediction_cache_hits == 1);
	CHECK(first.GetTrips()[0].prediction == hit.GetTrips()[0].prediction); CHECK(first.GetWork() == hit.GetWork());
	SECTION("Direction changed in the same tick") { v->direction = Direction::SE; }
	SECTION("Facilities changed in the same tick") { Station::Get(E)->facilities = {}; Station::Get(E)->bus_stops = nullptr; }
	SECTION("Refit changed cargo in the same tick") { v->cargo_type = MAIL; }
	GameRandomSeedChecker random;
	const auto fresh = PassengerServiceGraph::Build(PAX, NOW, limits);
	const auto changed = PassengerServiceGraph::Build(PAX, NOW, limits, {}, &capture);
	CHECK(changed.GetDiagnostic().prediction_cache_hits == 0);
	CHECK(fresh.GetWork() == changed.GetWork()); CHECK(fresh.GetMemoryBound() == changed.GetMemoryBound());
	REQUIRE(fresh.GetTrips().size() == changed.GetTrips().size());
	if (!fresh.GetTrips().empty()) CHECK(fresh.GetTrips()[0].prediction == changed.GetTrips()[0].prediction);
	CHECK(random.Check());
}

TEST_CASE("Every mode returns only the stale reserved suffix after gradual loading", "[passenger][multimodal][active]")
{
	MultimodalFixture f;
	VehicleType type = GENERATE(VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft);
	Vehicle *v = f.Service(type, {MultiStop(A), MultiStop(E, 100)});
	auto *packet = f.Packet(250);
	PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, E));
	f.Waiting().Append(packet, E);
	std::array next{E};
	REQUIRE(f.Waiting().Reserve(100, &v->cargo, next, v->tile, v) == 100);
	REQUIRE(f.Waiting().Load(20, &v->cargo, next, v->tile, v) == 20);
	v->GetOrder(0)->SetWaitTime(200);
	CHECK(f.Waiting().Load(20, &v->cargo, next, v->tile, v) == 0);
	CHECK(v->cargo.StoredCount() == 20); CHECK(v->cargo.ReservedCount() == 0);
	CHECK(f.Waiting().AvailableCount() == 230); CHECK(f.Waiting().ReservedCount() == 0);
	f.Waiting().RoutePassengerJourneys(PAX, A, false);
	CHECK(CountPassengerRoutingModes(f.Waiting())[to_underlying(PassengerRoutingMode::Fallback)] == 230);
	Money feeder = 0;
	for (StationCargoList::ConstIterator it(f.Waiting().Packets()->begin()); it != f.Waiting().Packets()->end(); ++it) {
		const CargoPacket *waiting = *it;
		CHECK(waiting->GetPassengerDestination() == E); feeder += waiting->GetFeederShare();
	}
	for (const CargoPacket *stored : *v->cargo.Packets()) { CHECK(stored->GetPassengerDestination() == E); feeder += stored->GetFeederShare(); }
	CHECK(feeder == 100);
}

TEST_CASE("Multimodal OD fallback boards only progressing rides and respects forced transfers", "[passenger][multimodal][active][fallback]")
{
	MultimodalFixture f;
	VehicleType type = GENERATE(VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft);
	Vehicle *through = f.Service(type, {MultiStop(A), MultiStop(B, 100), MultiStop(E, 100)});
	Vehicle *wrong = f.Service(type, {MultiStop(A), MultiStop(C, 100)});
	through->GetOrder(0)->SetWaitTimetabled(false); // Untimed direct OD transport remains possible.
	auto *packet = f.Packet();
	packet->FallbackPassengerJourney(A, NOW);
	f.Waiting().Append(packet, StationID::Invalid());
	std::array next{B};
	PassengerJourneyLoadingContext loading;
	CHECK_FALSE(f.Waiting().HasCargoFor(next, wrong, &loading));
	CHECK(f.Waiting().Reserve(10, &wrong->cargo, next, wrong->tile, wrong, &loading) == 0);
	REQUIRE(f.Waiting().Reserve(10, &through->cargo, next, through->tile, through, &loading) == 10);
	REQUIRE(f.Waiting().Load(10, &through->cargo, next, through->tile, through, &loading) == 10);
	CHECK(packet->GetPassengerVia() == E);
	Company *company = Company::Create(); through->owner = company->index;
	f.LoadAt(through, 1);
	bool forced = GENERATE(false, true);
	OrderUnloadType unload = forced ? OrderUnloadType::Transfer : OrderUnloadType::UnloadIfPossible;
	through->current_order.SetUnloadType(unload);
	CargoPayment payment(CargoPaymentID::Invalid(), through);
	PassengerJourneyActiveContext context(through, B);
	through->cargo.Stage(true, B, next, unload, &Station::Get(B)->goods[PAX], PAX, &payment,
			through->tile, nullptr, nullptr, &context);
	CHECK(through->cargo.ActionCount(Action::Deliver) == 0);
	CHECK(through->cargo.ActionCount(forced ? Action::Transfer : Action::Keep) == 10);
	CHECK(through->cargo.Unload(10, &f.Waiting(B), PAX, &payment, through->tile) == (forced ? 10 : 0));
	if (forced) CHECK(f.Waiting(B).Load(10, &through->cargo, next, through->tile, through) == 0);
	CHECK(packet->GetPassengerDestination() == E);
	CHECK(packet->GetFeederShare() == 100);
}

TEST_CASE("Every mode uses resident OD bindings through reservation transfer and final delivery", "[passenger][multimodal][fallback][bindings][determinism]")
{
	MultimodalFixture f;
	VehicleType type = GENERATE(VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft);
	bool residency = GENERATE(true, false);
	struct Cleanup {
		~Cleanup() { SetPassengerOdFallbackCacheEnabled(true); SetPassengerOdFallbackBindingValidation(false); _link_graph_pool.CleanPool(); }
	} cleanup;
	SetPassengerOdFallbackCacheEnabled(residency); ClearPassengerOdFallbackCache();
	LinkGraph *graph = LinkGraph::Create(PAX);
	for (StationID station : {A, B, C, D, E}) {
		Station *st = Station::Get(station); st->goods[PAX].SetLinkGraph(graph->index, graph->AddNode(st));
	}
	graph->UpdateEdge(0, 1, 100, 0, 100, {EdgeUpdateMode::Refresh, EdgeUpdateMode::Unrestricted});
	graph->UpdateEdge(1, 4, 100, 0, 100, {EdgeUpdateMode::Refresh, EdgeUpdateMode::Unrestricted});
	Vehicle *feeder = f.Service(type, {MultiStop(A), MultiStop(B, 100)});
	Vehicle *connection = f.Service(type, {MultiStop(B), MultiStop(E, 100)});
	auto *packet = f.Packet(20); packet->FallbackPassengerJourney(A, NOW);
	auto observe = [&]() {
		const size_t before = GetPassengerJourneyTickDiagnostic().od_fallback_work;
		PassengerOdFallbackContext context; REQUIRE(context.CanBoard(*packet, feeder, PAX)); context.PrepareBoard(*packet, feeder, PAX);
		return std::pair(packet->GetPassengerVia(), GetPassengerJourneyTickDiagnostic().od_fallback_work - before);
	};
	const auto cold = observe(); REQUIRE(cold.first == B); CHECK(observe() == cold);
	SetPassengerOdFallbackBindingValidation(true); CHECK(observe() == cold); SetPassengerOdFallbackBindingValidation(false);
	f.Waiting().Append(packet, StationID::Invalid()); std::array next{B}; PassengerJourneyLoadingContext loading;
	REQUIRE(f.Waiting().Reserve(20, &feeder->cargo, next, feeder->tile, feeder, &loading) == 20);
	CHECK(f.Waiting().Load(5, &feeder->cargo, next, feeder->tile, feeder, &loading) == 5);
	CHECK(f.Waiting().Load(15, &feeder->cargo, next, feeder->tile, feeder, &loading) == 15);
	f.LoadAt(feeder, 1); CargoPayment payment(CargoPaymentID::Invalid(), feeder); PassengerJourneyActiveContext at_b(feeder, B);
	feeder->cargo.Stage(true, B, next, OrderUnloadType::Transfer, &Station::Get(B)->goods[PAX], PAX, &payment, feeder->tile, nullptr, nullptr, &at_b);
	REQUIRE(feeder->cargo.Unload(20, &f.Waiting(B), PAX, &payment, feeder->tile) == 20);
	std::array final{E}; REQUIRE(f.Waiting(B).Load(20, &connection->cargo, final, connection->tile, connection) == 20);
	Company *company = Company::Create(); connection->owner = company->index;
	Town *town = Town::Create(TileIndex{0}); Station::Get(E)->town = town; Station::Get(E)->always_accepted.Set(PAX);
	f.LoadAt(connection, 1); CargoPayment final_payment(CargoPaymentID::Invalid(), connection); PassengerJourneyActiveContext at_e(connection, E);
	connection->cargo.Stage(true, E, final, OrderUnloadType::UnloadIfPossible, &Station::Get(E)->goods[PAX], PAX, &final_payment, connection->tile, nullptr, nullptr, &at_e);
	CHECK(connection->cargo.Unload(20, &f.Waiting(E), PAX, &final_payment, connection->tile) == 20);
	CHECK(company->cur_economy.delivered_cargo[PAX] == 20);
	CHECK(GetPassengerJourneyTickDiagnostic().untimed_boarded_passengers == 40);
	CHECK(GetPassengerJourneyTickDiagnostic().untimed_transferred_passengers == 20);
	CHECK(GetPassengerJourneyTickDiagnostic().untimed_delivered_passengers == 20);
}

TEST_CASE("Freight-only and zero-capacity vehicles never become passenger trips", "[passenger][multimodal][prediction]")
{
	MultimodalFixture f;
	VehicleType type = GENERATE(VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft);
	Vehicle *v = f.Service(type, {MultiStop(A), MultiStop(E, 100)});
	SECTION("No passenger seats") { v->cargo_cap = 0; }
	SECTION("Actual cargo is mail") { v->cargo_type = MAIL; }
	size_t work = 4096;
	CHECK_FALSE(HasPassengerVehicleCapacity(v, PAX, work));
	const auto graph = f.Graph(); CHECK(graph.GetTrips().empty());
	CHECK(graph.GetServices().empty());
	CHECK(graph.GetServicesAtStation(A).empty());
	PassengerJourneyPlanner planner(graph); CHECK_FALSE(planner.Plan(PAX, A, E, NOW).HasJourney());
}

TEST_CASE("Freight-only fleets leave capture work and service slots for passenger services", "[passenger][multimodal][service_graph][freight]")
{
	MultimodalFixture f;
	constexpr size_t freight_count = 1024;
	std::array modes{VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft};
	std::vector<Order> freight_orders(64, MultiStop(B, 10, 10));
	freight_orders.front() = MultiStop(A, 10, 10);
	std::vector<Vehicle *> freight;
	for (size_t i = 0; i < freight_count; ++i) {
		Vehicle *v = f.Service(modes[i % modes.size()], freight_orders); v->cargo_type = MAIL;
		v->orders->GetScheduledDispatchScheduleSet().resize(1);
		auto &schedule = v->orders->GetDispatchScheduleByIndex(0);
		schedule.SetScheduledDispatchStartTick(NOW); schedule.SetScheduledDispatchDuration(1000);
		for (uint32_t slot = 0; slot < 256; ++slot) schedule.GetScheduledDispatchMutable().push_back({slot, 0, {}});
		freight.push_back(v);
	}
	/* Created last so every irrelevant OrderList is encountered before this service. */
	Vehicle *passenger = f.Service(VehicleType::Train, {MultiStop(A), MultiStop(E, 100)});
	PassengerServiceGraphLimits limits; limits.horizon = 1000; limits.work = 4096;
	SECTION("Work-limited capture") {}
	SECTION("Only one Service and Trip slot") { limits.services = limits.trips = 1; }
	SECTION("Preflight itself is bounded and never certifies an incomplete fleet") {
		limits.work = freight_count;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		CHECK(graph.GetStatus() == PassengerPredictionStatus::BudgetExhausted);
		CHECK(graph.GetWork() == limits.work);
		CHECK(graph.GetServices().empty()); CHECK(graph.GetTrips().empty());
		PassengerJourneyPlanner planner(graph); CHECK(planner.Plan(PAX, A, E, NOW).IsInconclusive());
		return;
	}
	GameRandomSeedChecker seeds;
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
	REQUIRE(graph.GetStatus() == PassengerPredictionStatus::Complete);
	REQUIRE(graph.GetServices().size() == 1); REQUIRE(graph.GetTrips().size() == 1);
	CHECK(graph.GetServices().front().id == passenger->orders->index);
	CHECK(graph.GetTrips().front().vehicle == passenger->index);
	CHECK(graph.GetServicesAtStation(B).empty());
	CHECK(graph.GetWork() < limits.work); CHECK(graph.GetMemoryBound() <= limits.memory_bytes);
	PassengerJourneyPlanner planner(graph); CHECK(planner.Plan(PAX, A, E, NOW).HasJourney());
	/* Dispatch length on excluded services has no effect on graph work or snapshots. */
	for (Vehicle *v : freight) v->orders->GetScheduledDispatchScheduleSet().clear();
	const auto without_slots = PassengerServiceGraph::Build(PAX, NOW, limits);
	CHECK(without_slots.GetStatus() == PassengerPredictionStatus::Complete);
	CHECK(without_slots.GetWork() == graph.GetWork());
	PassengerJourneyPlanner without_slots_planner(without_slots);
	CHECK(without_slots_planner.Plan(PAX, A, E, NOW).journeys == planner.Plan(PAX, A, E, NOW).journeys);
	CHECK(seeds.Check());
}

TEST_CASE("Shared orders retain passenger members among freight-only vehicles", "[passenger][multimodal][service_graph][freight]")
{
	MultimodalFixture f;
	VehicleType type = GENERATE(VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft);
	Vehicle *first = f.Service(type, {MultiStop(A), MultiStop(E, 100)}); first->cargo_type = MAIL;
	Vehicle *passenger = f.Service(type, {MultiStop(A), MultiStop(E, 100)});
	passenger->orders->FreeChain(); passenger->orders = first->orders; passenger->AddToShared(first);
	Vehicle *last = f.Service(type, {MultiStop(A), MultiStop(E, 100)}); last->cargo_type = MAIL;
	last->orders->FreeChain(); last->orders = first->orders; last->AddToShared(passenger);
	PassengerServiceGraphLimits limits; limits.horizon = 1000; limits.services = limits.trips = 1;
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
	REQUIRE(graph.GetStatus() == PassengerPredictionStatus::Complete);
	REQUIRE(graph.GetServices().size() == 1); REQUIRE(graph.GetTrips().size() == 1);
	CHECK(graph.GetServices().front().id == first->orders->index);
	CHECK(graph.GetTrips().front().vehicle == passenger->index);
	PassengerJourneyPlanner planner(graph); CHECK(planner.Plan(PAX, A, E, NOW).HasJourney());
	/* Rebuild from current capacity; no stale service-level eligibility cache. */
	passenger->cargo_cap = 0;
	limits.services = limits.trips = 0;
	const auto no_passengers = PassengerServiceGraph::Build(PAX, NOW, limits);
	CHECK(no_passengers.GetStatus() == PassengerPredictionStatus::Complete);
	CHECK(no_passengers.GetServices().empty()); CHECK(no_passengers.GetTrips().empty());
}

TEST_CASE("A road direction edit invalidates execution even when the stop is still usable", "[passenger][multimodal][active]")
{
	MultimodalFixture f;
	Vehicle *v = f.Service(VehicleType::Road, {MultiStop(A), MultiStop(E, 100)});
	auto *packet = f.Packet(); PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, E));
	f.Waiting().Append(packet, E);
	const uint64_t before = PassengerJourneyServiceSignature(*v->orders, PAX);
	v->GetOrder(0)->SetRoadVehTravelDirection(DiagDirection::NE);
	CHECK(GetPassengerRouteState(v));
	CHECK(PassengerJourneyServiceSignature(*v->orders, PAX) != before);
	CHECK_FALSE(PassengerJourneyBoardingContext(v).CanBoard(*packet));
	f.Waiting().RoutePassengerJourneys(PAX, A, false);
	CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Fallback);
}

TEST_CASE("Mixed transport chains transfer at shared StationIDs through the common rounds", "[passenger][multimodal][journey]")
{
	MultimodalFixture f;
	std::vector<VehicleType> modes;
	SECTION("Train to road") { modes = {VehicleType::Train, VehicleType::Road}; }
	SECTION("Road to train") { modes = {VehicleType::Road, VehicleType::Train}; }
	SECTION("Train to aircraft") { modes = {VehicleType::Train, VehicleType::Aircraft}; }
	SECTION("Train to ship") { modes = {VehicleType::Train, VehicleType::Ship}; }
	SECTION("Ship to train") { modes = {VehicleType::Ship, VehicleType::Train}; }
	SECTION("Road to aircraft to train") { modes = {VehicleType::Road, VehicleType::Aircraft, VehicleType::Train}; }
	SECTION("All four modes") { modes = {VehicleType::Train, VehicleType::Road, VehicleType::Aircraft, VehicleType::Ship}; }
	SECTION("Six interchanges expand the initial transfer limit") { modes = {VehicleType::Road, VehicleType::Train, VehicleType::Aircraft, VehicleType::Ship, VehicleType::Road, VehicleType::Ship, VehicleType::Train}; }
	std::vector<Vehicle *> vehicles;
	for (size_t i = 0; i < modes.size(); ++i) vehicles.push_back(f.Service(modes[i],
			{MultiStop(StationID{static_cast<uint16_t>(i)}, 10000, static_cast<TimetableTicks>(100 + 100 * i)), MultiStop(StationID{static_cast<uint16_t>(i + 1)}, 10)}));
	StationID destination{static_cast<uint16_t>(modes.size())};
	const auto graph = f.Graph(2000); PassengerJourneyPlanner planner(graph);
	PassengerJourneyEvaluation evaluation; evaluation.minimum_transfer = 20;
	auto result = planner.Plan(PAX, A, destination, NOW, {}, evaluation);
	REQUIRE(result.HasJourney()); CHECK(result.status == PassengerPredictionStatus::Complete);
	CHECK(result.journeys.front().transfers == modes.size() - 1);
	CHECK(result.journeys.front().legs.size() == modes.size());
	for (size_t i = 0; i < modes.size(); ++i) CHECK(result.journeys.front().legs[i].vehicle == vehicles[i]->index);
	if (modes.size() > 5) CHECK(result.diagnostic.extensions > 0);
	/* Neighbouring IDs have no walking edge. Same ID alone provides the interchange. */
	vehicles.back()->GetOrder(0)->SetDestination(StationID{50}); f.StationAt(StationID{50});
	f.LoadAt(vehicles.back(), 0);
	const auto disconnected = f.Graph(2000); PassengerJourneyPlanner separate(disconnected);
	CHECK_FALSE(separate.Plan(PAX, A, destination, NOW, {}, evaluation).HasJourney());
}

TEST_CASE("Active passengers execute train road aircraft ship transfers and settle the final delivery", "[passenger][multimodal][active]")
{
	MultimodalFixture f;
	std::array modes{VehicleType::Train, VehicleType::Road, VehicleType::Aircraft, VehicleType::Ship};
	std::array<Vehicle *, 4> services{};
	for (size_t i = 0; i < modes.size(); ++i) services[i] = f.Service(modes[i],
			{MultiStop(StationID{static_cast<uint16_t>(i)}, 10000, static_cast<TimetableTicks>(100 + 100 * i)), MultiStop(StationID{static_cast<uint16_t>(i + 1)}, 10)});
	auto *packet = f.Packet();
	PassengerJourneyActiveContext initial(nullptr, A); REQUIRE(initial.PlanWaiting(*packet, PAX, B));
	f.Waiting(A).Append(packet, B);
	Company *company = Company::Create(); Town *town = Town::Create(TileIndex{0});
	Station::Get(E)->town = town; Station::Get(E)->always_accepted.Set(PAX);
	for (size_t i = 0; i < services.size(); ++i) {
		Vehicle *v = services[i];
		StationID from{static_cast<uint16_t>(i)}, to{static_cast<uint16_t>(i + 1)};
		std::array next{to};
		REQUIRE(packet->GetPassengerJourney().vehicle == v->index);
		REQUIRE(f.Waiting(from).Load(10, &v->cargo, next, v->tile, v) == 10);
		/* Advance the same simulation timetable for the waiting connections. */
		_state_ticks = NOW + 110 + 100 * i;
		for (size_t j = i + 1; j < services.size(); ++j) services[j]->current_order_time = (_state_ticks - NOW).base();
		f.LoadAt(v, 1); v->owner = company->index;
		CargoPayment payment(CargoPaymentID::Invalid(), v);
		PassengerJourneyActiveContext context(v, to);
		v->cargo.Stage(to == E, to, next, OrderUnloadType::UnloadIfPossible, &Station::Get(to)->goods[PAX], PAX, &payment,
				v->tile, nullptr, nullptr, &context);
		CHECK(v->cargo.ActionCount(to == E ? Action::Deliver : Action::Transfer) == 10);
		CHECK(v->cargo.Unload(10, &f.Waiting(to), PAX, &payment, v->tile) == 10);
		CHECK(v->cargo.TotalCount() == 0);
		if (to != E) { CHECK(packet->GetPassengerDestination() == E); CHECK(packet->Count() == 10); }
	}
	CHECK(company->cur_economy.delivered_cargo[PAX] == 10);
	for (StationID station : {A, B, C, D, E}) CHECK(f.Waiting(station).TotalCount() == 0);
}

TEST_CASE("Road adapters honor bay drive-through cargo direction and articulated capacity", "[passenger][multimodal][prediction]")
{
	MultimodalFixture f;
	RoadVehicle *road = RoadVehicle::From(f.Service(VehicleType::Road, {MultiStop(A), MultiStop(E, 100)}));
	size_t budget = 4096;
	REQUIRE(GetPassengerRouteState(road, &budget));
	SECTION("Bay bus stop") { MakeRoadStop(road->tile, OWNER_NONE, A, RoadStopType::Bus, ROADTYPE_ROAD, INVALID_ROADTYPE, DiagDirection::NE); road->state = RVSB_IN_ROAD_STOP; }
	SECTION("Direction mismatch is not boarding") { road->GetOrder(0)->SetRoadVehTravelDirection(DiagDirection::SW); CHECK_FALSE(GetPassengerRouteState(road)); return; }
	SECTION("Loading behind another bus before the drive-through stop position") { road->state = RVSB_IN_DT_ROAD_STOP; }
	SECTION("A compatible stop elsewhere at the same StationID") {
		Station *st = Station::Get(E); MakeRoadStop(st->xy, OWNER_NONE, E, RoadStopType::Bus, ROADTYPE_ROAD, INVALID_ROADTYPE, DiagDirection::NE);
		RoadStop *extra = RoadStop::Create(st->xy + 5); st->bus_stops->next = extra;
		MakeDriveThroughRoadStop(extra->xy, OWNER_NONE, OWNER_NONE, OWNER_NONE, E, StationType::Bus, ROADTYPE_ROAD, INVALID_ROADTYPE, Axis::X);
		RoadVehicle *part = Vehicle::Create<RoadVehicle>(); part->SetArticulatedPart(); part->cargo_type = PAX; part->cargo_cap = 50; road->SetNext(part);
		CHECK(f.Graph().GetTrips().front().capacity == 150);
	}
	SECTION("Articulated vehicles reject bay-only destinations") {
		RoadVehicle *part = Vehicle::Create<RoadVehicle>(); part->SetArticulatedPart(); part->cargo_type = PAX; part->cargo_cap = 50; road->SetNext(part);
		Station *st = Station::Get(E); MakeRoadStop(st->xy, OWNER_NONE, E, RoadStopType::Bus, ROADTYPE_ROAD, INVALID_ROADTYPE, DiagDirection::NE);
		CHECK_FALSE(CanPassengerVehicleUseStation(road, E, road->GetOrder(1), budget)); return;
	}
	SECTION("Freight front with a passenger articulated compartment uses truck facilities") {
		RoadVehicle *part = Vehicle::Create<RoadVehicle>(); part->SetArticulatedPart(); part->cargo_type = PAX; part->cargo_cap = 50; road->SetNext(part); road->cargo_type = MAIL;
		for (StationID id : {A, E}) {
			Station *st = Station::Get(id); st->truck_stops = st->bus_stops; st->bus_stops = nullptr;
			MakeDriveThroughRoadStop(st->xy, OWNER_NONE, OWNER_NONE, OWNER_NONE, id, StationType::Truck, ROADTYPE_ROAD, INVALID_ROADTYPE, Axis::X);
		}
		CHECK(f.Graph().GetTrips().front().capacity == 50);
	}
	CHECK(GetPassengerRouteState(road));
	const auto graph = f.Graph(); PassengerJourneyPlanner planner(graph); CHECK(planner.Plan(PAX, A, E, NOW).HasJourney());
}

TEST_CASE("Drive-through buses with shared orders board simultaneously before the stop position", "[passenger][multimodal][boarding][roadstop]")
{
	MultimodalFixture f;
	const auto mode = GENERATE(std::string_view{"Hyperpath"}, "EstimatedServiceIntent", "ServiceIntent", "Timed", "Fallback", "Legacy");
	CAPTURE(mode);
	const bool periodic = mode == "Hyperpath" || mode == "EstimatedServiceIntent" || mode == "ServiceIntent";
	_settings_game.linkgraph.passenger_journey_periodic = periodic;
	_settings_game.linkgraph.passenger_journey_precomputed = mode == "Hyperpath";
	_settings_game.linkgraph.passenger_journey_hyperpath = mode == "Hyperpath";
	std::array<RoadVehicle *, 3> buses;
	for (auto &bus : buses) bus = RoadVehicle::From(f.Service(VehicleType::Road, {MultiStop(A), MultiStop(E, 100)}));
	for (size_t i = 1; i < buses.size(); ++i) {
		buses[i]->orders->FreeChain(); buses[i]->orders = buses[0]->orders; buses[i]->AddToShared(buses[i - 1]);
	}
	/* The controller can BeginLoading behind a stopped bus before setting RVS_ENTERED_STOP. */
	buses[0]->frame = RVC_DRIVE_THROUGH_STOP_FRAME;
	buses[1]->state = RVSB_IN_DT_ROAD_STOP; buses[1]->frame = 1;
	buses[2]->current_order = *buses[2]->GetOrder(0); // This bus arrives after the first two leave.
	if (mode == "EstimatedServiceIntent") for (auto *bus : buses) bus->current_order.Free();
	ResetPassengerServiceDatabase();
	auto &db = GetPassengerServiceDatabase();
	if (periodic) {
		for (int i = 0; i < 128 && db.IsBuilding(); ++i) db.Tick();
		REQUIRE_FALSE(db.IsBuilding());
		if (mode == "Hyperpath") while (!db.PreparePrecomputed()) {}
	}
	if (mode == "EstimatedServiceIntent") {
		f.LoadAt(buses[0], 0); f.LoadAt(buses[1], 0); buses[1]->state = RVSB_IN_DT_ROAD_STOP;
	}
	if (!periodic) {
		const auto graph = f.Graph();
		CHECK(std::any_of(graph.GetTrips().begin(), graph.GetTrips().end(), [&](const auto &trip) { return trip.vehicle == buses[1]->index; }));
	}
	auto *packet = f.Packet(350);
	if (mode == "Fallback") {
		buses[0]->GetOrder(0)->SetWaitTimetabled(false);
		packet->FallbackPassengerJourney(A, NOW, E);
	} else if (mode != "Legacy") {
		PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, E));
		const auto expected = mode == "Hyperpath" ? PassengerRideKind::HyperpathIntent : mode == "EstimatedServiceIntent" ?
				PassengerRideKind::EstimatedServiceIntent : mode == "ServiceIntent" ? PassengerRideKind::ServiceIntent : PassengerRideKind::VehicleSnapshot;
		REQUIRE(packet->GetPassengerJourney().kind == expected);
	}
	f.Waiting().Append(packet, E);
	Station::Get(A)->loading_vehicles = {buses[0], buses[1]};
	PassengerJourneyLoadingContext loading; std::array next{E};
	for (size_t i = 0; i < buses.size(); ++i) {
		auto *bus = buses[i]; CAPTURE(i);
		if (i == 2) {
			buses[0]->current_order.MakeLeaveStation(); buses[1]->current_order.MakeLeaveStation();
			f.LoadAt(bus, 0); bus->frame = RVC_DRIVE_THROUGH_STOP_FRAME;
			Station::Get(A)->loading_vehicles = {bus};
		}
		if (mode == "Timed") {
			/* Identical order progress has identical visit times; each snapshot binds its own bus. */
			auto ride = packet->GetPassengerJourney(); ride.vehicle = bus->index; packet->SetPassengerJourney(ride, E);
		}
		const auto state = GetPassengerRouteState(bus); CHECK(state); if (state) CHECK(state->loading);
		REQUIRE(loading.CanBoard(*packet, bus, PAX));
		REQUIRE(f.Waiting().HasCargoFor(next, bus, &loading, PAX));
		REQUIRE(f.Waiting().Reserve(100, &bus->cargo, next, bus->tile, bus, &loading) == 100);
		CHECK(f.Waiting().Load(40, &bus->cargo, next, bus->tile, bus, &loading) == 40);
		CHECK(f.Waiting().Load(60, &bus->cargo, next, bus->tile, bus, &loading) == 60);
		CHECK(bus->cargo.StoredCount() == 100);
		CHECK(HasBit(bus->state, RVS_ENTERED_STOP) == (i != 1));
	}
	CHECK(f.Waiting().AvailableCount() == 50); CHECK(f.Waiting().ReservedCount() == 0);
}

TEST_CASE("Drive-through loading retains physical stop and boarding restrictions", "[passenger][multimodal][boarding][roadstop]")
{
	MultimodalFixture f;
	AutoRestoreBackup<bool> sharing{_settings_game.economy.infrastructure_sharing[to_underlying(VehicleType::Road)], false};
	auto *bus = RoadVehicle::From(f.Service(VehicleType::Road, {MultiStop(A), MultiStop(E, 100)}));
	bus->state = RVSB_IN_DT_ROAD_STOP; bus->frame = 1;
	/* Plan while loading is valid, then change only the physical state or boarding permission. */
	auto *packet = f.Packet(); PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, E));
	SECTION("Passing through with a travel order") { bus->current_order = *bus->GetOrder(0); }
	SECTION("Waiting behind a bus before BeginLoading") { bus->current_order = *bus->GetOrder(0); bus->cur_speed = 0; }
	SECTION("Leaving the station") { bus->current_order.MakeLeaveStation(); }
	SECTION("Outside a road stop") { bus->state = 0; }
	SECTION("Not on a road stop tile") { bus->tile = Station::Get(A)->xy + 1; }
	SECTION("A different station tile") { bus->tile = Station::Get(E)->xy; }
	SECTION("Incompatible road type") { bus->compatible_roadtypes.Reset(); }
	SECTION("Infrastructure permission denied") {
		bus->owner = Company::Create()->index; SetTileOwner(bus->tile, Company::Create()->index);
	}
	SECTION("Wrong stop type") { MakeDriveThroughRoadStop(bus->tile, OWNER_NONE, OWNER_NONE, OWNER_NONE, A, StationType::Truck, ROADTYPE_ROAD, INVALID_ROADTYPE, Axis::X); }
	SECTION("Direction mismatch") { bus->GetOrder(0)->SetRoadVehTravelDirection(DiagDirection::SW); }
	SECTION("Stopped by the player") { bus->vehstatus.Set(VehState::Stopped); }
	SECTION("Crashed") { bus->vehstatus.Set(VehState::Crashed); }
	SECTION("No loading") { bus->current_order.SetLoadType(OrderLoadType::NoLoad); }
	PassengerJourneyLoadingContext loading;
	CHECK_FALSE(loading.CanBoard(*packet, bus, PAX));
	f.Waiting().Append(packet, E);
	CHECK_FALSE(f.Waiting().HasCargoFor(std::array{E}, bus, &loading, PAX));
	CHECK(f.Waiting().Load(10, &bus->cargo, std::array{E}, bus->tile, bus, &loading) == 0);
}

TEST_CASE("Ships traverse buoys without boarding and retain repeated port occurrences", "[passenger][multimodal][prediction]")
{
	MultimodalFixture f;
	Order buoy; buoy.MakeGoToWaypoint(StationID{50}); buoy.SetTravelTime(30); buoy.SetTravelTimetabled(true);
	Vehicle *ship = f.Service(VehicleType::Ship, {MultiStop(A), buoy, MultiStop(B, 40), MultiStop(A, 40), MultiStop(E, 40)});
	const auto graph = f.Graph();
	CHECK(graph.GetServicesAtStation(StationID{50}).empty()); CHECK(graph.GetStationVisits(StationID{50}).empty());
	REQUIRE(graph.GetTrips().front().prediction.visits.size() >= 4);
	CHECK(graph.GetTrips().front().prediction.visits[1].arrival == NOW + 170);
	CHECK(graph.GetTrips().front().prediction.visits[2].order == 3);
	PassengerJourneyPlanner planner(graph); CHECK(planner.Plan(PAX, A, E, NOW).HasJourney());
	ship->tile = Station::Get(B)->xy + 4; CHECK_FALSE(GetPassengerRouteState(ship));
}

TEST_CASE("Aircraft distinguish terminals landing holding hangars helicopters and mail", "[passenger][multimodal][prediction]")
{
	MultimodalFixture f;
	Aircraft *air = Aircraft::From(f.Service(VehicleType::Aircraft, {MultiStop(A), MultiStop(E, 100)}));
	Aircraft *mail = Vehicle::Create<Aircraft>(); mail->subtype = AIR_SHADOW; mail->cargo_type = MAIL; mail->cargo_cap = 500; air->SetNext(mail);
	SECTION("Mail compartment is not passenger capacity") { CHECK(f.Graph().GetTrips().front().capacity == 100); }
	SECTION("Hangar is not boarding") { air->state = HANGAR; CHECK_FALSE(GetPassengerRouteState(air)); return; }
	SECTION("A destination outside the aircraft range is unknown") { air->flags.Set(VehicleAirFlag::DestinationTooFar); CHECK_FALSE(GetPassengerRouteState(air)); return; }
	SECTION("Landing is travel until terminal BeginLoading") { air->state = LANDING; CHECK_FALSE(GetPassengerRouteState(air)); air->current_order = *air->GetOrder(0); air->current_order_time = 100; CHECK(GetPassengerRouteState(air)); return; }
	SECTION("Overdue airport holding remains Unknown") { air->state = FLYING; air->current_order = *air->GetOrder(0); air->current_order_time = 11000; CHECK_FALSE(f.Graph().GetTrips().front().prediction.visits.front().arrival); return; }
	SECTION("Helicopter uses a heliport") { air->subtype = AIR_HELICOPTER; air->state = HELIPAD1; for (StationID id : {A, E}) Station::Get(id)->airport.type = AT_HELIPORT; }
	SECTION("Airplane cannot use a heliport") { Station::Get(E)->airport.type = AT_HELIPORT; const auto graph = f.Graph(); PassengerJourneyPlanner planner(graph); CHECK(planner.Plan(PAX, A, E, NOW).IsInconclusive()); return; }
	SECTION("Closed destination airport") { Station::Get(E)->airport.blocks.Set(AirportBlock::AirportClosed); const auto graph = f.Graph(); PassengerJourneyPlanner planner(graph); CHECK(planner.Plan(PAX, A, E, NOW).IsInconclusive()); return; }
	CHECK(GetPassengerRouteState(air));
	const auto graph = f.Graph(); PassengerJourneyPlanner planner(graph); CHECK(planner.Plan(PAX, A, E, NOW).HasJourney());
}

TEST_CASE("All modes safely fallback on changed facilities orders capacity and unknown predictions", "[passenger][multimodal][active]")
{
	MultimodalFixture f;
	VehicleType type = GENERATE(VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft);
	Vehicle *v = f.Service(type, {MultiStop(A), MultiStop(B, 10), MultiStop(E, 100)});
	auto *packet = f.Packet(); PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, E));
	f.Waiting().Append(packet, E);
	SECTION("NoLoad") { v->GetOrder(0)->SetLoadType(OrderLoadType::NoLoad); }
	SECTION("NoUnload") { v->GetOrder(2)->SetUnloadType(OrderUnloadType::NoUnload); }
	SECTION("Nonstop destination") { v->GetOrder(2)->SetNonStopType(ONSF_NO_STOP_AT_DESTINATION_STATION); }
	SECTION("Refit order") { v->GetOrder(0)->SetRefit(MAIL); }
	SECTION("Actual refit changes capacity") { v->cargo_type = MAIL; }
	SECTION("Changed timetable") { v->GetOrder(1)->SetTravelTime(500); }
	SECTION("Changed loading occurrence") { v->current_order_time = 10; }
	SECTION("Full load leaves departure unknown") { v->GetOrder(0)->SetLoadType(OrderLoadType::FullLoad); }
	SECTION("Conditional skips stops and never certifies unreachable") { v->GetOrder(1)->MakeConditional(0); const auto graph = f.Graph(); PassengerJourneyPlanner planner(graph); CHECK(planner.Plan(PAX, A, E, NOW).IsInconclusive()); }
	SECTION("A changed implicit record invalidates the previous signature") { v->GetOrder(1)->MakeImplicit(E); }
	SECTION("Removed stopping facilities") { Station::Get(E)->facilities = {}; Station::Get(E)->bus_stops = nullptr; }
	SECTION("Stopped") { v->vehstatus.Set(VehState::Stopped); }
	CHECK_FALSE(PassengerJourneyBoardingContext(v).CanBoard(*packet));
	f.Waiting().RoutePassengerJourneys(PAX, A, false);
	CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Fallback);
	CHECK(packet->GetPassengerDestination() == E); CHECK(packet->Count() == 10); CHECK(packet->GetFeederShare() == 100);
}

TEST_CASE("Multimodal snapshots and OD caches preserve deterministic ties and bounded work", "[passenger][multimodal][determinism][performance]")
{
	MultimodalFixture f;
	for (VehicleType type : {VehicleType::Ship, VehicleType::Aircraft, VehicleType::Road, VehicleType::Train}) f.Service(type, {MultiStop(A), MultiStop(E, 100)});
	GameRandomSeedChecker random;
	const auto first = f.Graph(), second = f.Graph();
	PassengerJourneyPlanner cached(first), cold(second);
	auto result = cached.Plan(PAX, A, E, NOW);
	REQUIRE(result.HasJourney());
	CHECK(result.journeys == cached.Plan(PAX, A, E, NOW).journeys);
	CHECK(result.journeys == cold.Plan(PAX, A, E, NOW).journeys);
	CHECK(result.journeys.front().legs.front().vehicle == first.GetTrips().front().vehicle);
	CHECK(random.Check());
	PassengerServiceGraphLimits limits; limits.trips = 2;
	const auto truncated = PassengerServiceGraph::Build(PAX, NOW, limits);
	CHECK(truncated.GetStatus() == PassengerPredictionStatus::LimitReached);
	CHECK(truncated.GetWork() <= limits.work); CHECK(truncated.GetMemoryBound() <= limits.memory_bytes);
}

TEST_CASE("Multimodal onboard rides survive exhausted search budgets", "[passenger][multimodal][active][budget]")
{
	MultimodalFixture f;
	VehicleType type = GENERATE(VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft);
	Vehicle *vehicle = f.Service(type, {MultiStop(A), MultiStop(B, 100), MultiStop(E, 100)});
	OrderUnloadType unload = OrderUnloadType::UnloadIfPossible;
	SECTION("Normal continuation") {}
	SECTION("NoLoad permits existing passengers to stay") { vehicle->GetOrder(1)->SetLoadType(OrderLoadType::NoLoad); }
	SECTION("Forced transfer remains effective") { unload = OrderUnloadType::Transfer; }
	auto *packet = f.Packet();
	REQUIRE(PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*packet, PAX, E));
	const auto saved = packet->GetPassengerJourney();
	vehicle->cargo.Append(packet);
	f.LoadAt(vehicle, 1); _state_ticks = NOW + 200;
	BeginPassengerJourneyTick();
	for (uint i = 0; i < 40; ++i) {
		auto probe = std::unique_ptr<CargoPacket>(f.Packet(1));
		PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*probe, PAX, E);
	}
	CHECK(GetPassengerJourneyTickDiagnostic().query_work >= 15 * 65536);
	PassengerJourneyActiveContext context(vehicle, B);
	const auto action = context.Decide(*packet, PAX, true, unload);
	if (unload == OrderUnloadType::Transfer) {
		CHECK(action == Action::Transfer);
		CHECK(packet->GetPassengerRoutingMode() != PassengerRoutingMode::Journey);
	} else {
		CHECK(action == Action::Keep);
		CHECK(packet->GetPassengerJourney() == saved);
		CHECK(GetPassengerJourneyTickDiagnostic().retained_rides == 1);
	}
	CHECK(packet->GetPassengerDestination() == E);
	CHECK(packet->GetFeederShare() == 100);
}

TEST_CASE("A frequent mixed trunk beats the infrequent direct train", "[passenger][multimodal][journey]")
{
	MultimodalFixture f;
	f.Service(VehicleType::Train, {MultiStop(A, 10000, 800), MultiStop(E, 10)});
	Vehicle *road = f.Service(VehicleType::Road, {MultiStop(A), MultiStop(B, 10)});
	Vehicle *air = f.Service(VehicleType::Aircraft, {MultiStop(B, 10000, 200), MultiStop(E, 10)});
	const auto graph = f.Graph(); PassengerJourneyPlanner planner(graph);
	auto result = planner.Plan(PAX, A, E, NOW);
	REQUIRE(result.HasJourney()); REQUIRE(result.journeys.front().legs.size() == 2);
	CHECK(result.journeys.front().legs[0].vehicle == road->index); CHECK(result.journeys.front().legs[1].vehicle == air->index);
	CHECK(result.journeys.front().arrival == NOW + 210);
}

TEST_CASE("Multimodal vehicle destruction invalidates indexed journeys before pool ID reuse", "[passenger][multimodal][active]")
{
	MultimodalFixture f;
	VehicleType type = GENERATE(VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft);
	Vehicle *v = f.Service(type, {MultiStop(A), MultiStop(E, 100)});
	auto *packet = f.Packet(); PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, E));
	f.Waiting().Append(packet, E);
	VehicleID old = v->index;
	/* Supply only the unrelated destruction bookkeeping required by these minimal
	 * engines; the production derived/base destructors and reverse index run normally. */
	Company *company = Company::Create(); v->owner = company->index;
	v->unitnumber = company->freeunits[type].UseID(UnitID{1}); SetBit(v->subtype, GVSF_VIRTUAL);
	v->last_station_visited = StationID::Invalid();
	if (type == VehicleType::Road) RoadVehicle::From(v)->state = 0;
	if (type == VehicleType::Aircraft) Aircraft::From(v)->targetairport = StationID::Invalid();
	delete v;
	REQUIRE(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Fallback);
	CHECK(packet->GetPassengerJourney().vehicle == VehicleID::Invalid());
	CHECK(packet->GetPassengerDestination() == E); CHECK(packet->GetFeederShare() == 100); CHECK(packet->Count() == 10);
	Vehicle *replacement = f.Service(type, {MultiStop(A), MultiStop(E, 100)});
	CHECK(replacement->index == old);
	CHECK_FALSE(packet->CanRetryPassengerJourney(A, NOW));
	f.Waiting().RoutePassengerJourneys(PAX, A, false);
	CHECK(f.Waiting().AvailableViaCount(StationID::Invalid()) == 10);
	std::array next{E};
	CHECK(f.Waiting().Load(10, &replacement->cargo, next, replacement->tile, replacement) == 10);
}

TEST_CASE("Multimodal execution preserves the existing save extension and validates afterload", "[passenger][multimodal][saveload]")
{
	MultimodalFixture f;
	VehicleType type = GENERATE(VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft);
	Vehicle *v = f.Service(type, {MultiStop(A), MultiStop(E, 100)});
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	REQUIRE(_sl_xv_feature_versions[XSLFI_PASSENGER_JOURNEY_ROUTING] == 1);
	auto *packet = f.Packet(); PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, E));
	PassengerJourneyExecution leg = packet->GetPassengerJourney();
	PassengerRoutingMode expected = PassengerRoutingMode::Journey;
	bool changed_facilities = false;
	SECTION("Current execution") {}
	SECTION("Saved fallback and cooldown") { packet->FallbackPassengerJourney(A, NOW); expected = PassengerRoutingMode::Fallback; }
	SECTION("Old save without execution fields") { _sl_xv_feature_versions[XSLFI_PASSENGER_JOURNEY_ROUTING] = 0; expected = PassengerRoutingMode::Legacy; }
	SECTION("Saved facility changes are validated by the normal routing event") { Station::Get(E)->facilities = {}; Station::Get(E)->bus_stops = nullptr; changed_facilities = true; }
	auto *loaded = CargoPacket::Create();
	CHECK_FALSE(SlTestSaveLoadTable(packet, loaded, GetCargoPacketDesc()).empty());
	f.Waiting().Append(loaded, StationID::Invalid());
	GameRandomSeedChecker random;
	AfterLoadPassengerJourneys();
	CHECK(loaded->GetPassengerRoutingMode() == expected); CHECK(loaded->GetPassengerDestination() == E);
	CHECK(loaded->Count() == 10); CHECK(loaded->GetFeederShare() == 100);
	if (expected == PassengerRoutingMode::Journey) {
		CHECK(loaded->GetPassengerJourney() == leg);
		if (changed_facilities) {
			CHECK_FALSE(PassengerJourneyBoardingContext(v).CanBoard(*loaded));
			f.Waiting().RoutePassengerJourneys(PAX, A, false);
			CHECK(loaded->GetPassengerRoutingMode() == PassengerRoutingMode::Fallback);
			CHECK(loaded->GetPassengerDestination() == E);
			CHECK(loaded->Count() == 10);
		} else {
			CHECK(PassengerJourneyBoardingContext(v).CanBoard(*loaded));
			CHECK(InvalidatePassengerJourneyVehicle(v->index) >= 1); // Rebuilt reverse index contains restored packets.
		}
	}
	CHECK(random.Check());
}

TEST_CASE("Shadow diagnostics observe every transport without changing cargo or RNG", "[passenger][multimodal][shadow][determinism]")
{
	MultimodalFixture f;
	VehicleType type = GENERATE(VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft);
	Vehicle *v = f.Service(type, {MultiStop(A), MultiStop(E, 100)});
	auto *packet = f.Packet(); v->cargo.Append(packet);
	SetPassengerJourneyShadowEnabled(true); ResetPassengerJourneyShadowStats();
	static int64_t epoch = 1000; _state_ticks += ++epoch * 1024;
	GameRandomSeedChecker random;
	PassengerJourneyShadowContext shadow(v, A);
	shadow.Observe(*packet, PAX, Action::Keep, E, OrderUnloadType::UnloadIfPossible);
	const auto &stats = GetPassengerJourneyShadowStats();
	CHECK(stats.eligible_od.packets == 1); CHECK(stats.modes[to_underlying(type)].evaluated.packets == 1);
	CHECK(stats.modes[to_underlying(type)].journeys.packets == 1);
	CHECK(stats.unsupported_od.packets == 0);
	CHECK(packet->Count() == 10); CHECK(packet->GetFeederShare() == 100); CHECK(packet->GetPassengerDestination() == E);
	CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Legacy); CHECK(random.Check());
}

TEST_CASE("National mixed fleet shares snapshots and keeps graph query and cache bounds", "[passenger][multimodal][performance]")
{
	MultimodalFixture f;
	std::array modes{VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft};
	/* 2402 stations, 480 physical trips with 21 stopping orders each. Routes
	 * use consecutive modes across a shared hub without any mode-grouped registration. */
	for (uint16_t route = 0; route < 120; ++route) {
		std::vector<Order> orders{MultiStop(A, 60000, 100)};
		for (uint16_t i = 1; i <= 20; ++i) orders.push_back(MultiStop(StationID{static_cast<uint16_t>(1 + route * 20 + i)}, 1, 1));
		for (VehicleType type : modes) f.Service(type, orders);
	}
	auto start = std::chrono::steady_clock::now();
	const auto graph = f.Graph();
	auto build_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	REQUIRE(graph.GetStatus() == PassengerPredictionStatus::Complete); REQUIRE(graph.GetTrips().size() == 480);
	PassengerJourneyPlanner planner(graph); start = std::chrono::steady_clock::now();
	PassengerJourneyLimits limits; limits.candidates = 1; limits.work = 65536; limits.memory_bytes = 8 * 1024 * 1024;
	auto result = planner.Plan(PAX, A, StationID{2401}, NOW, limits);
	auto query_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	REQUIRE(result.HasJourney()); CHECK(result.status == PassengerPredictionStatus::Complete);
	CHECK(result.diagnostic.work <= PassengerJourneyLimits{}.work);
	CHECK(result.diagnostic.scratch_bytes <= PassengerJourneyLimits{}.memory_bytes);
	CHECK(graph.GetWork() <= PassengerServiceGraphLimits{}.work); CHECK(graph.GetMemoryBound() <= PassengerServiceGraphLimits{}.memory_bytes);
	CHECK(planner.Plan(PAX, A, StationID{2401}, NOW, limits).diagnostic.cache_hit);
	std::array<size_t, 4> counts{}; for (const auto &trip : graph.GetTrips()) ++counts[to_underlying(trip.type)];
	for (size_t count : counts) CHECK(count == 120);
	WARN("Mixed national network: stations=" << Station::GetNumItems() << "; trips=" << graph.GetTrips().size() <<
			"; graph_us=" << build_us << "; query_us=" << query_us << "; graph_work=" << graph.GetWork() <<
			"; query_work=" << result.diagnostic.work << "; scratch=" << result.diagnostic.scratch_bytes << "; envelope=" << graph.GetMemoryBound());
}

TEST_CASE("Every mode carries passengers through operating stops and continuing depots", "[passenger][operational][multimodal][active]")
{
	MultimodalFixture f;
	VehicleType type = GENERATE(VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft);
	bool periodic = GENERATE(false, true); bool optional_service = GENERATE(false, true);
	bool declared_depot_wait = GENERATE(false, true);
	_settings_game.linkgraph.passenger_journey_periodic = periodic;
	Order closed = MultiStop(B, 20, 30); closed.SetLoadType(OrderLoadType::NoLoad); closed.SetUnloadType(OrderUnloadType::NoUnload);
	Order depot; depot.MakeGoToDepot(DepotID{0}, {OrderDepotTypeFlag::PartOfOrders});
	if (optional_service) depot.SetDepotOrderType({OrderDepotTypeFlag::PartOfOrders, OrderDepotTypeFlag::Service});
	depot.SetTravelTime(20); depot.SetTravelTimetabled(true); depot.SetWaitTime(30); depot.SetWaitTimetabled(true);
	if (!declared_depot_wait) { depot.SetWaitTime(0); depot.SetWaitTimetabled(false); }
	Order final = MultiStop(E, 40, 0); final.SetUnloadType(OrderUnloadType::Transfer);
	auto *vehicle = f.Service(type, {MultiStop(A, 10000, 10), closed, depot, final});
	ResetPassengerJourneyDiagnostic();
	ResetPassengerServiceDatabase(); if (periodic) GetPassengerServiceDatabase().Tick();
	auto *packet = f.Packet(25); PassengerJourneyActiveContext context(nullptr, A);
	context.PlanWaiting(*packet, PAX, E);
	CHECK((packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey) == !optional_service);
	f.Waiting().Append(packet, E); std::array next{E}; PassengerJourneyLoadingContext loading;
	REQUIRE(f.Waiting().Reserve(20, &vehicle->cargo, next, vehicle->tile, vehicle, &loading) == 20);
	for (uint i = 0; i < 4; ++i) CHECK(f.Waiting().Load(5, &vehicle->cargo, next, vehicle->tile, vehicle, &loading) == 5);
	CHECK(vehicle->cargo.StoredCount() == 20); CHECK(f.Waiting().AvailableCount() == 5); CHECK(f.Waiting().ReservedCount() == 0);
	f.LoadAt(vehicle, 1); vehicle->current_order.SetLoadType(OrderLoadType::NoLoad); vehicle->current_order.SetUnloadType(OrderUnloadType::NoUnload);
	BeginPassengerJourneyTick(); PassengerJourneyActiveContext operating(vehicle, B);
	for (auto *onboard : *vehicle->cargo.Packets()) CHECK(operating.Decide(*onboard, PAX, true, OrderUnloadType::NoUnload) == Action::Keep);
	Company *company = Company::Create(); Town *town = Town::Create(TileIndex{0});
	vehicle->owner = company->index; Station::Get(E)->town = town; Station::Get(E)->always_accepted.Set(PAX);
	f.LoadAt(vehicle, 3); vehicle->current_order.SetUnloadType(OrderUnloadType::Transfer);
	CargoPayment payment(CargoPaymentID::Invalid(), vehicle); PassengerJourneyActiveContext arrived(vehicle, E);
	vehicle->cargo.Stage(true, E, next, OrderUnloadType::Transfer, &Station::Get(E)->goods[PAX], PAX, &payment,
			vehicle->tile, nullptr, nullptr, &arrived);
	CHECK(vehicle->cargo.ActionCount(Action::Deliver) == 20);
	CHECK(vehicle->cargo.Unload(20, &f.Waiting(E), PAX, &payment, vehicle->tile) == 20);
	CHECK(company->cur_economy.delivered_cargo[PAX] == 20); CHECK(vehicle->cargo.TotalCount() == 0);
	auto stats = GetPassengerJourneyTotalDiagnostic();
	CHECK(stats.depot_service_boarded_passengers == (optional_service ? 0 : 20));
	CHECK(stats.depot_crossing_boarded_passengers == (optional_service ? 0 : 20));
	CHECK(stats.depot_service_delivered_passengers == (optional_service ? 0 : 20));
	CHECK(stats.depot_crossing_delivered_passengers == (optional_service ? 0 : 20));
}
