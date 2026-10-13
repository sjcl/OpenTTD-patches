/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../passenger_service_database.h"
#include "../passenger_journey_active.h"
#include "../vehicle_base.h"
#include "../station_base.h"
#include "../settings_type.h"
#include "../date_func.h"
#include "../order_func.h"
#include "../order_cmd.h"
#include "../vehicle_gui.h"
#include "../landscape_cmd.h"
#include "../command_func.h"
#include "../company_func.h"
#include "../clear_map.h"
#include "../core/backup_type.hpp"
#include "../core/random_func.hpp"
#include "../sl/saveload.h"
#include "passenger_test_stations.h"

extern SaveLoadVersion _sl_version;
extern std::vector<uint8_t> SlTestSaveLoadTable(void *, void *, const NamedSaveLoadTable &);

namespace {
	constexpr CargoType PAX{0};
	constexpr StationID A{0}, B{1}, C{2}, D{3};
	constexpr StateTicks NOW{64000};
	Order Stop(StationID station, TimetableTicks travel = 100, TimetableTicks wait = 10)
	{
		Order order; order.MakeGoToStation(station);
		order.SetTravelTime(travel); order.SetTravelTimetabled(true);
		order.SetWaitTime(wait); order.SetWaitTimetabled(true);
		return order;
	}
	struct PeriodicTrain : Vehicle {
		PeriodicTrain(VehicleID id, std::vector<Order> orders) : Vehicle(id, VehicleType::Train)
		{
			this->orders = OrderList::Create(std::move(orders), this);
			EnsurePassengerTestStations(this->orders->GetOrderVector());
			this->cargo_type = PAX; this->cargo_cap = 100; this->direction = Direction::NE;
			this->LoadAt(0);
		}
		PeriodicTrain(VehicleID id, PeriodicTrain *other) : Vehicle(id, VehicleType::Train)
		{
			this->orders = other->orders; this->AddToShared(other);
			this->cargo_type = PAX; this->cargo_cap = 100; this->direction = Direction::NE; this->LoadAt(0);
		}
		bool IsPrimaryVehicle() const override { return true; }
		void LoadAt(VehicleOrderID index)
		{
			this->cur_real_order_index = this->cur_implicit_order_index = index;
			this->cur_timetable_order_index = INVALID_VEH_ORDER_ID;
			this->last_station_visited = this->GetOrder(index)->GetDestination().ToStationID();
			this->current_order.MakeLoading(false); this->current_order_time = 0;
		}
	};
	struct Fixture {
		AutoRestoreBackup<LinkGraphSettings> settings{_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{}};
		AutoRestoreBackup<StateTicks> ticks{_state_ticks, NOW};
		AutoRestoreBackup<CargoClasses> classes{CargoSpec::Get(PAX)->classes, CargoClasses{CargoClass::Passengers}};
		AutoRestoreBackup<uint8_t> bitnum{CargoSpec::Get(PAX)->bitnum, uint8_t{0}};
		Fixture()
		{
			REQUIRE(Vehicle::GetNumItems() == 0); REQUIRE(Station::GetNumItems() == 0);
			AllocateMap(128, 128);
			_settings_game.linkgraph.passenger_journey_routing = _settings_game.linkgraph.passenger_journey_periodic = true;
			_settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Asymmetric;
			_settings_game.linkgraph.passenger_transfer_penalty = 0;
			ResetPassengerServiceDatabase(); BeginPassengerJourneyTick();
		}
		~Fixture()
		{
			_vehicle_pool.CleanPool(); _orderlist_pool.CleanPool(); _order_pool.CleanPool();
			_station_pool.CleanPool(); _cargopacket_pool.CleanPool(); ClearCargoPacketDeferredPayments();
			ResetPassengerServiceDatabase();
		}
		PeriodicTrain *Train(std::vector<Order> orders) { return Vehicle::Create<PeriodicTrain>(std::move(orders)); }
		PassengerServiceDatabase &Build() { auto &db = GetPassengerServiceDatabase(); for (int i = 0; i < 128 && db.IsBuilding(); ++i) db.Tick(); REQUIRE_FALSE(db.IsBuilding()); return db; }
		void PublishUpdates(PassengerServiceDatabase &db) { _state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; for (int i = 0; i < 128 && db.IsBuilding(); ++i) db.Tick(); REQUIRE_FALSE(db.IsBuilding()); }
		CargoPacket *Packet(uint16_t count = 10)
		{
			auto *packet = CargoPacket::Create(count, 0, A, TileIndex{0}, Money{0}); REQUIRE(packet->SetPassengerDestination(PAX, D)); return packet;
		}
	};
}

TEST_CASE("Periodic round trips repeat without horizon expansion", "[passenger][periodic]")
{
	auto table = PassengerPeriodicTimetable::Compile(std::vector<Order>{Stop(A), Stop(B), Stop(C), Stop(B)});
	REQUIRE(table.period == 440); REQUIRE(table.stops.size() == 4);
	CHECK(table.stops[1].arrival == 110); CHECK(table.stops[3].order == 3);
	for (int64_t cycle : {int64_t{0}, int64_t{1}, int64_t{1000000}}) {
		CHECK(PassengerPeriodicTimetable::NextDeparture(NOW, 120, table.period, NOW + cycle * table.period) == NOW + cycle * table.period + 120);
	}
}

TEST_CASE("Periodic departure boundaries do not overflow state ticks", "[passenger][periodic][determinism]")
{
	using T = PassengerPeriodicTimetable;
	CHECK(T::NextDeparture(NOW, 10, 100, NOW + 10) == NOW + 10);
	CHECK(T::NextDeparture(NOW, 10, 100, NOW + 11) == NOW + 110);
	CHECK_FALSE(T::NextDeparture(NOW, 0, 0, NOW));
	CHECK_FALSE(T::NextDeparture(StateTicks{INT64_MAX}, 1, 10, NOW));
	CHECK_FALSE(T::NextDeparture(StateTicks{INT64_MAX - 5}, 0, 10, StateTicks{INT64_MAX}));
	CHECK(T::NextDeparture(StateTicks{INT64_MIN}, 0, 10, StateTicks{INT64_MAX - 20}).has_value());
}

TEST_CASE("Periodic compiler distinguishes explicit zero from missing zero", "[passenger][periodic]")
{
	auto orders = std::vector<Order>{Stop(A, 100, 0), Stop(D, 0, 0)};
	CHECK(PassengerPeriodicTimetable::Compile(orders).period == 100);
	orders[1].SetTravelTimetabled(false); CHECK(PassengerPeriodicTimetable::Compile(orders).period == 0);
	orders[1].SetTravelTimetabled(true); orders[0].SetWaitTimetabled(false); CHECK(PassengerPeriodicTimetable::Compile(orders).period == 0);
	orders[0].SetWaitTimetabled(true); orders[0].SetTravelTime(0); CHECK(PassengerPeriodicTimetable::Compile(orders).period == 0);
}

TEST_CASE("Periodic control restrictions remain untimed and full load uses declared wait", "[passenger][periodic]")
{
	auto orders = std::vector<Order>{Stop(A), Stop(D)};
	SECTION("Full load") { orders[0].SetLoadType(OrderLoadType::FullLoad); CHECK(PassengerPeriodicTimetable::Compile(orders).period == 220); }
	SECTION("Conditional") { orders[1].MakeConditional(0); CHECK(PassengerPeriodicTimetable::Compile(orders).period == 0); }
	SECTION("Depot") { orders[1].MakeGoToDepot(DepotID{0}, {}); CHECK(PassengerPeriodicTimetable::Compile(orders).period == 0); }
	SECTION("Refit") { orders[1].SetRefit(PAX); CHECK(PassengerPeriodicTimetable::Compile(orders).period == 0); }
}

TEST_CASE("Shared periodic vehicles retain individually observed phases", "[passenger][periodic]")
{
	Fixture f; auto *first = f.Train({Stop(A), Stop(D)});
	auto *second = Vehicle::Create<PeriodicTrain>(first); second->current_order_time = 40;
	auto &db = f.Build(); auto *service = db.Get(first->orders->index); REQUIRE(service != nullptr);
	REQUIRE(service->vehicles.size() == 2); CHECK(service->vehicles[0].base != service->vehicles[1].base);
	auto plan = db.Plan(PAX, A, D, NOW + 11); REQUIRE(plan.ride); CHECK(plan.ride->departure == NOW + 220);
	CHECK(plan.ride->vehicle == VehicleID::Invalid()); CHECK(plan.ride->kind == PassengerRideKind::ServiceIntent);
}

TEST_CASE("Periodic transfers evaluate different periods at the current ready time", "[passenger][periodic]")
{
	Fixture f; f.Train({Stop(A, 90, 0), Stop(B, 10, 0)}); auto *connection = f.Train({Stop(B, 80, 20), Stop(D, 20, 0)});
	auto &db = f.Build();
	auto early = db.Plan(PAX, A, D, NOW); REQUIRE(early.ride); CHECK(early.arrival == NOW + 40);
	auto late = db.Plan(PAX, A, D, NOW + 101); REQUIRE(late.ride); CHECK(late.arrival > early.arrival);
	CHECK(late.ride->departure >= NOW + 101);
	CHECK(db.GetDiagnostic().policy_hits >= 1);
	CHECK(connection->orders->GetNumOrders() == 2);
}

TEST_CASE("Periodic local express and direct services compare actual phases", "[passenger][periodic]")
{
	Fixture f; auto *local = f.Train({Stop(A, 80, 0), Stop(B, 20, 0), Stop(D, 50, 0)});
	auto *express = f.Train({Stop(A, 100, 20), Stop(D, 10, 0)});
	auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride);
	CHECK(plan.ride->service == express->orders->index); CHECK(plan.arrival == NOW + 30);
	CHECK(plan.ride->service != local->orders->index);
}

TEST_CASE("Periodic timed routes win while missing timetable services stay unknown", "[passenger][periodic]")
{
	Fixture f; auto *timed = f.Train({Stop(A, 200, 0), Stop(D, 100, 0)});
	Order unknown = Stop(A); unknown.SetWaitTimetabled(false); f.Train({unknown, Stop(D, 1)});
	auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK(plan.ride->service == timed->orders->index);
}

TEST_CASE("Periodic policies are inert across cold warm evicted and unused caches", "[passenger][periodic][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); GameRandomSeedChecker seeds;
	auto cold = db.Plan(PAX, A, D, NOW + 10000); auto warm = db.Plan(PAX, A, D, NOW + 10000);
	CHECK(cold.ride == warm.ride); CHECK(cold.work == warm.work);
	db.ClearPolicies(); auto rebuilt = db.Plan(PAX, A, D, NOW + 10000);
	CHECK(cold.ride == rebuilt.ride); CHECK(cold.work == rebuilt.work);
	for (int i = 0; i < 3000; ++i) { _state_ticks++; db.Tick(); }
	CHECK(db.GetDiagnostic().builds == 1); CHECK(db.GetDiagnostic().bytes < 4096); CHECK(seeds.Check());
}

TEST_CASE("Periodic local invalidation preserves unrelated published services", "[passenger][periodic][determinism]")
{
	Fixture f; auto *one = f.Train({Stop(A), Stop(B)}); auto *two = f.Train({Stop(C), Stop(D)});
	auto &db = f.Build(); auto generation = db.Get(two->orders->index)->generation;
	one->GetOrder(1)->SetTravelTime(50); db.Invalidate(one->orders->index);
	CHECK(db.GetPublication(one->orders->index) == PassengerServiceDatabase::Publication::Dirty); CHECK(db.Get(two->orders->index)->generation == generation);
	db.Tick(1); CHECK(db.GetPublication(one->orders->index) == PassengerServiceDatabase::Publication::Dirty);
	f.PublishUpdates(db); CHECK(db.Get(two->orders->index)->generation == generation); CHECK(db.Get(one->orders->index)->timetable.period == 170);
}

TEST_CASE("Periodic scheduler progress and captured phases survive save and join", "[passenger][periodic][save][determinism]")
{
	Fixture f; auto *one = f.Train({Stop(A), Stop(D)}); auto *two = f.Train({Stop(B), Stop(C)}); auto &db = f.Build();
	one->current_order_time = 5000; one->lateness_counter = 9999;
	db.Invalidate(two->orders->index); db.Tick(1);
	auto wire = db.Save(); PassengerServiceDatabase joined; REQUIRE(joined.Load(wire)); joined.FinishLoad(true);
	CHECK(joined.Save() == wire); CHECK(joined.Get(one->orders->index)->vehicles[0].base == NOW);
	db.Tick(); joined.Tick(); CHECK(db.Save() == joined.Save());
	CHECK(db.Plan(PAX, A, D, NOW).ride == joined.Plan(PAX, A, D, NOW).ride);
}

TEST_CASE("Periodic database rejects malformed and oversized saved state", "[passenger][periodic][save]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto wire = db.Save(); PassengerServiceDatabase other;
	wire.pop_back(); CHECK_FALSE(other.Load(wire)); wire = db.Save(); wire[0] = 9; CHECK_FALSE(other.Load(wire));
	CHECK(other.GetDiagnostic().services == 0);
}

TEST_CASE("Periodic rebuilding tolerates another edit and falls back before publication", "[passenger][periodic]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = GetPassengerServiceDatabase();
	CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride); db.Tick(1);
	train->GetOrder(1)->SetTravelTime(50); db.Invalidate(train->orders->index); db.Tick(1);
	train->GetOrder(1)->SetTravelTime(25); db.Invalidate(train->orders->index); f.Build();
	auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK(plan.arrival == NOW + 35);
}

TEST_CASE("Periodic shared dispatch competition never invents physical trips", "[passenger][periodic][dispatch]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); train->vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
	Vehicle::Create<PeriodicTrain>(train)->vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
	auto &db = f.Build(); CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride);
	CHECK(db.GetDiagnostic().vehicles == 2); CHECK(db.GetDiagnostic().timed_services == 0);
}

TEST_CASE("Periodic single vehicle dispatch uses real slots with turnaround checks", "[passenger][periodic][dispatch]")
{
	Fixture f; auto *train = f.Train({Stop(A, 20, 0), Stop(D, 20, 0)});
	train->vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
	train->orders->GetScheduledDispatchScheduleSet().emplace_back();
	auto &schedule = train->orders->GetDispatchScheduleByIndex(0);
	schedule.SetScheduledDispatchStartTick(NOW); schedule.SetScheduledDispatchDuration(200);
	schedule.GetScheduledDispatchMutable().push_back({10, 0}); schedule.GetScheduledDispatchMutable().push_back({100, 0});
	train->GetOrder(0)->SetDispatchScheduleIndex(0);
	SECTION("Multiple nonconflicting slots") { auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW + 11); REQUIRE(plan.ride); CHECK(plan.ride->departure == NOW + 100); }
	SECTION("Reuse disabled does not invent a competing vehicle") { schedule.SetScheduledDispatchReuseSlots(false); auto &db = f.Build(); CHECK(db.Plan(PAX, A, D, NOW + 11).ride.has_value()); }
	SECTION("Overlapping turnaround") { schedule.GetScheduledDispatchMutable()[1].offset = 30; auto &db = f.Build(); CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride); }
	SECTION("Previously allocated future non-reusable slot") { schedule.SetScheduledDispatchReuseSlots(false); schedule.SetScheduledDispatchLastDispatch(100); auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK(plan.ride->departure == NOW + 100); }
}

TEST_CASE("Periodic active journeys board a delayed actual vehicle with real empty seats", "[passenger][periodic][active]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto *packet = f.Packet();
	PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, D));
	CHECK(packet->GetPassengerJourney().kind == PassengerRideKind::ServiceIntent);
	Station::Get(A)->goods[PAX].CreateData().cargo.Append(packet, StationID::Invalid());
	_state_ticks += 5000; train->current_order_time = 5000; train->lateness_counter = 5000; BeginPassengerJourneyTick();
	PassengerJourneyLoadingContext loading; std::array next{D};
	CHECK(Station::Get(A)->goods[PAX].data->cargo.Reserve(10, &train->cargo, next, TileIndex{0}, train, &loading) == 10);
	CHECK(train->cargo.Packets()->front()->GetPassengerJourney().kind == PassengerRideKind::BoundService);
	CHECK(train->cargo.Packets()->front()->GetPassengerJourney().vehicle == train->index);
	CHECK(GetPassengerJourneyTickDiagnostic().builds == 0); CHECK(db.GetDiagnostic().builds == 1);
}

TEST_CASE("Periodic ride validation rejects no load no unload forced transfer and changed identities", "[passenger][periodic][active]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(B), Stop(D)}); auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride);
	CHECK(ValidatePassengerServiceRide(*plan.ride, train, PAX, true));
	SECTION("No load") { train->GetOrder(0)->SetLoadType(OrderLoadType::NoLoad); }
	SECTION("No unload") { train->GetOrder(2)->SetUnloadType(OrderUnloadType::NoUnload); }
	SECTION("Forced intermediate transfer") { train->GetOrder(1)->SetUnloadType(OrderUnloadType::Transfer); }
	SECTION("Wrong service incarnation") { plan.ride->signature++; }
	CHECK_FALSE(ValidatePassengerServiceRide(*plan.ride, train, PAX, true));
}

TEST_CASE("Periodic routing disabled leaves the scheduler and cargo state untouched", "[passenger][periodic][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = GetPassengerServiceDatabase(); auto wire = db.Save();
	_settings_game.linkgraph.passenger_journey_routing = false; db.Tick(); CHECK(db.Save() == wire);
}

TEST_CASE("Cached periodic route alternatives change the winner with departure phase", "[passenger][periodic]")
{
	Fixture f;
	auto *local = f.Train({Stop(A, 900, 0), Stop(D, 100, 0)});
	auto *express = f.Train({Stop(A, 990, 200), Stop(D, 10, 0)});
	auto &db = f.Build();
	auto early = db.Plan(PAX, A, D, NOW); REQUIRE(early.ride); CHECK(early.ride->service == local->orders->index);
	auto late = db.Plan(PAX, A, D, NOW + 190); REQUIRE(late.ride); CHECK(late.ride->service == express->orders->index);
	CHECK(db.GetDiagnostic().policy_hits == 1); CHECK(late.ride->departure == NOW + 200);
}

TEST_CASE("Periodic and snapshot planners agree on complete small timetable networks", "[passenger][periodic][comparison]")
{
	Fixture f; f.Train({Stop(A, 1000, 0), Stop(B, 20, 0)}); f.Train({Stop(B, 1000, 30), Stop(D, 10, 0)});
	auto &db = f.Build();
	PassengerServiceGraphLimits graph_limits; graph_limits.horizon = 3000;
	auto graph = PassengerServiceGraph::Build(PAX, NOW, graph_limits, {}, nullptr, PassengerTimingPolicy::AssumeTimetable);
	PassengerJourneyPlanner planner(graph);
	for (int64_t ready : {int64_t{0}, int64_t{100}, int64_t{1100}}) {
		auto old = planner.Plan(PAX, A, D, NOW + ready); auto periodic = db.Plan(PAX, A, D, NOW + ready);
		REQUIRE(old.HasJourney()); REQUIRE(periodic.ride);
		CHECK(periodic.arrival == old.journeys.front().arrival); CHECK(periodic.cost == old.journeys.front().generalized_time.base());
		REQUIRE(periodic.route.size() == old.journeys.front().legs.size());
		for (size_t i = 0; i < periodic.route.size(); ++i) {
			const auto &leg = old.journeys.front().legs[i];
			CHECK(periodic.route[i].service == graph.GetTrips()[leg.boarding.trip].service);
			CHECK(periodic.route[i].boarding == leg.boarding_station); CHECK(periodic.route[i].alighting == leg.alighting_station);
		}
	}
}

TEST_CASE("Periodic split passengers can board a second real shared vehicle immediately", "[passenger][periodic][active]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto *shared = Vehicle::Create<PeriodicTrain>(train); f.Build();
	auto *packet = f.Packet(200); PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, D));
	auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo; waiting.Append(packet, StationID::Invalid());
	std::array next{D}; PassengerJourneyLoadingContext loading;
	CHECK(waiting.Load(100, &train->cargo, next, TileIndex{0}, train, &loading) == 100);
	CHECK(packet->GetPassengerJourney().kind == PassengerRideKind::ServiceIntent);
	CHECK(waiting.Load(100, &shared->cargo, next, TileIndex{0}, shared, &loading) == 100);
	CHECK(waiting.TotalCount() == 0); CHECK(GetPassengerJourneyTickDiagnostic().boarded_passengers == 200);
}

TEST_CASE("Periodic many packets share one event policy and never build service graphs", "[passenger][periodic][active]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); f.Build(); PassengerJourneyActiveContext context(nullptr, A);
	for (int i = 0; i < 4096; ++i) { auto *packet = f.Packet(1); REQUIRE(context.PlanWaiting(*packet, PAX, D)); }
	CHECK(GetPassengerJourneyTickDiagnostic().queries == 1); CHECK(GetPassengerJourneyTickDiagnostic().query_cache_hits == 4095);
	CHECK(GetPassengerJourneyTickDiagnostic().builds == 0); CHECK(GetPassengerServiceDatabase().GetDiagnostic().searches == 1);
}

TEST_CASE("Periodic network scales to 8000 stations without expanded physical visits", "[passenger][periodic][scaling]")
{
	Fixture f;
	for (uint16_t i = 0; i < 4000; ++i) f.Train({Stop(StationID{static_cast<uint16_t>(2 * i)}), Stop(StationID{static_cast<uint16_t>(2 * i + 1)})});
	auto &db = GetPassengerServiceDatabase();
	for (int i = 0; i < 100; ++i) { _state_ticks++; db.Tick(); }
	auto diagnostic = db.GetDiagnostic(); CHECK(diagnostic.services == 4000); CHECK(diagnostic.vehicles == 4000);
	CHECK(Station::GetNumItems() == 8000); CHECK(diagnostic.bytes < PassengerServiceDatabase::MAX_BYTES);
	auto plan = db.Plan(PAX, A, B, _state_ticks); REQUIRE(plan.ride);
	CHECK(GetPassengerJourneyTickDiagnostic().builds == 0); CHECK(GetPassengerJourneyTickDiagnostic().prediction_executions == 0);
}

TEST_CASE("Periodic dirty queue rotation prevents repeated edits from starving other lists", "[passenger][periodic][determinism]")
{
	Fixture f; auto *one = f.Train({Stop(A), Stop(B)}); auto *two = f.Train({Stop(C), Stop(D)}); auto &db = f.Build();
	db.Invalidate(one->orders->index); db.Invalidate(two->orders->index);
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH;
	for (int i = 0; i < 10 && db.GetPublication(two->orders->index) != PassengerServiceDatabase::Publication::Published; ++i) { db.Invalidate(one->orders->index); _state_ticks++; db.Tick(600); }
	REQUIRE(db.GetPublication(two->orders->index) == PassengerServiceDatabase::Publication::Published);
}

TEST_CASE("Periodic unplanned implicit stops never become forecast boarding occurrences", "[passenger][periodic]")
{
	Fixture f; Order implicit; implicit.MakeImplicit(B); auto *train = f.Train({Stop(A), implicit, Stop(D)}); f.Build();
	auto *service = GetPassengerServiceDatabase().Get(train->orders->index); REQUIRE(service != nullptr);
	CHECK(service->timetable.stops.size() == 2); CHECK(service->timetable.stops[1].order == 2);
	train->cur_real_order_index = train->cur_implicit_order_index = 1; train->last_station_visited = B;
	auto ride = GetPassengerServiceDatabase().Plan(PAX, A, D, NOW).ride; REQUIRE(ride);
	CHECK_FALSE(ValidatePassengerServiceRide(*ride, train, PAX, true));
}

TEST_CASE("Periodic saved order list identities cannot bind to recycled pool IDs", "[passenger][periodic][save]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride);
	OrderListID id = train->orders->index; uint64_t incarnation = train->orders->GetCargoDistRouteGeneration();
	train->orders->FreeChain(); train->orders = OrderList::Create(std::vector<Order>{Stop(A), Stop(D)}, train);
	CHECK(train->orders->index == id); CHECK(train->orders->GetCargoDistRouteGeneration() != incarnation);
	CHECK_FALSE(ValidatePassengerServiceRide(*plan.ride, train, PAX, true)); CHECK_FALSE(db.IsCurrent(*plan.ride));
	f.PublishUpdates(db); CHECK(db.Get(id)->incarnation != incarnation);
}

TEST_CASE("Periodic state tick rebasing preserves departures and packet retry intervals", "[passenger][periodic][save]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride);
	auto *packet = f.Packet(); packet->SetPassengerJourney(*plan.ride, D);
	StateTicksDelta delta{-50000}; db.ShiftTicks(delta); packet->ShiftPassengerJourneyTicks(delta); _state_ticks += delta;
	auto shifted = db.Plan(PAX, A, D, _state_ticks); REQUIRE(shifted.ride);
	CHECK(shifted.ride->departure == packet->GetPassengerJourney().departure); CHECK(shifted.arrival == plan.arrival + delta);
}

TEST_CASE("Periodic long shared lines preserve their direct ride within a bounded policy search", "[passenger][periodic][scaling]")
{
	Fixture f; std::vector<Order> orders;
	for (uint16_t i = 0; i < 300; ++i) orders.push_back(Stop(StationID{i}));
	auto *train = f.Train(orders); Vehicle::Create<PeriodicTrain>(train); auto &db = f.Build();
	PassengerJourneyLimits limits; limits.work = 65536; limits.labels = 8192; limits.memory_bytes = 8 * 1024 * 1024;
	auto plan = db.Plan(PAX, A, StationID{299}, NOW, limits); REQUIRE(plan.ride); CHECK_FALSE(plan.deferred);
	CHECK(plan.ride->alighting_order == 299); CHECK(plan.transfers == 0); CHECK(plan.work <= limits.work);
}

TEST_CASE("Expired periodic intents choose an alternative while a delayed live ride remains usable", "[passenger][periodic][active]")
{
	Fixture f; auto *original = f.Train({Stop(A, 900, 0), Stop(D, 100, 0)}); auto *express = f.Train({Stop(A, 990, 200), Stop(D, 10, 0)}); f.Build();
	auto *packet = f.Packet(); PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, D));
	CHECK(packet->GetPassengerJourney().service == original->orders->index);
	Station::Get(A)->goods[PAX].CreateData().cargo.Append(packet, StationID::Invalid()); _state_ticks = NOW + 190; BeginPassengerJourneyTick();
	SECTION("Missed original vehicle") {
		/* It has actually left; a vehicle still in OT_LOADING at A is a
		 * catchable live departure, even if its published time has passed. */
		original->cur_real_order_index = original->cur_implicit_order_index = 1;
		original->current_order = *original->GetOrder(1);
		Station::Get(A)->loading_vehicles.push_back(express);
	}
	SECTION("Actual delayed original vehicle") { Station::Get(A)->loading_vehicles.push_back(original); }
	Station::Get(A)->goods[PAX].data->cargo.RoutePassengerJourneys(PAX, A);
	REQUIRE(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
	CHECK(packet->GetPassengerJourney().service == Station::Get(A)->loading_vehicles.front()->orders->index);
}

TEST_CASE("Periodic transfer intents enforce the actual minimum interchange time", "[passenger][periodic][active]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); auto *connection = f.Train({Stop(B, 100, 0), Stop(D, 10, 0)}); f.Build();
	_settings_game.linkgraph.passenger_transfer_penalty = 20; auto *packet = f.Packet();
	PassengerJourneyActiveContext arrived(connection, B); REQUIRE(arrived.PlanWaiting(*packet, PAX, D));
	CHECK(packet->GetPassengerJourney().not_before == NOW + 20);
	CHECK_FALSE(ValidatePassengerServiceRide(packet->GetPassengerJourney(), connection, PAX, true));
	_state_ticks += 20; CHECK(ValidatePassengerServiceRide(packet->GetPassengerJourney(), connection, PAX, true));
}

TEST_CASE("Dirty service intents retain their saved retry state until the next simulation event", "[passenger][periodic][save][determinism]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto *packet = f.Packet();
	PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, D));
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	auto *loaded = CargoPacket::Create(); REQUIRE_FALSE(SlTestSaveLoadTable(packet, loaded, GetCargoPacketDesc()).empty());
	auto saved_ride = loaded->GetPassengerJourney(); Station::Get(A)->goods[PAX].CreateData().cargo.Append(loaded, StationID::Invalid());
	db.Invalidate(train->orders->index); auto wire = db.Save(); REQUIRE(db.Load(wire)); AfterLoadPassengerJourneys();
	CHECK(loaded->GetPassengerRoutingMode() == PassengerRoutingMode::Journey); CHECK(loaded->GetPassengerJourney() == saved_ride);
	CHECK(db.Save() == wire);
}

TEST_CASE("Periodic capacity changes invalidate only the affected service while occupancy does not", "[passenger][periodic][active]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto generation = db.Get(train->orders->index)->generation;
	train->cargo.Append(f.Packet()); ObservePassengerPeriodicVehicle(train); CHECK(db.Get(train->orders->index)->generation == generation);
	train->cargo_cap = 0; ObservePassengerPeriodicVehicle(train); CHECK(db.GetPublication(train->orders->index) == PassengerServiceDatabase::Publication::Dirty); f.PublishUpdates(db);
	CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride);
}

TEST_CASE("Periodic round trips prefer the earlier equivalent boarding occurrence", "[passenger][periodic]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(B), Stop(D), Stop(B), Stop(A)}); train->LoadAt(4); auto &db = f.Build();
	auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK(plan.ride->boarding_order == 4);
	CHECK(ValidatePassengerServiceRide(*plan.ride, train, PAX, true));
}

TEST_CASE("Routine order window notifications preserve periodic generations and policies", "[passenger][periodic][active]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build();
	auto generation = db.Get(train->orders->index)->generation;
	REQUIRE(db.Plan(PAX, A, D, NOW).ride);
	for (uint i = 0; i < 3000; ++i) InvalidateVehicleOrder(train, VIWD_MODIFY_ORDERS);
	CHECK(db.Get(train->orders->index)->generation == generation);
	CHECK(db.Plan(PAX, A, D, NOW).ride.has_value());
	CHECK(db.GetDiagnostic().policy_hits == 1);
}

TEST_CASE("Unrelated landscape clearing preserves the published passenger network", "[passenger][periodic][active]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build();
	auto generation = db.Get(train->orders->index)->generation;
	AutoRestoreBackup company{_current_company, OWNER_DEITY};
	TileIndex tile = TileXY(40, 40); MakeClear(tile, ClearGround::Grass, 3);
	REQUIRE(Command<Commands::LandscapeClear>::Do({DoCommandFlag::Execute}, tile).Succeeded());
	CHECK(db.Get(train->orders->index)->generation == generation);
	CHECK(db.GetDiagnostic().global_invalidations == 0);
}

TEST_CASE("Automatic timetable edits coalesce while safe old generations and policies survive", "[passenger][periodic][save][determinism]")
{
	Fixture f; _state_ticks = StateTicks{65536}; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build();
	auto generation = db.Get(train->orders->index)->generation;
	REQUIRE(db.Plan(PAX, A, D, NOW).ride);
	train->GetOrder(1)->SetTravelTime(200);
	for (uint i = 0; i < PassengerServiceDatabase::TIMETABLE_EPOCH - 1; ++i) {
		db.TimetableChanged(train->orders->index); db.Tick();
		REQUIRE(db.Get(train->orders->index)->generation == generation); ++_state_ticks;
	}
	CHECK(db.Plan(PAX, A, D, _state_ticks).ride.has_value()); CHECK(db.GetDiagnostic().policy_hits == 1);
	auto wire = db.Save(); PassengerServiceDatabase joined; REQUIRE(joined.Load(wire)); joined.FinishLoad(true);
	++_state_ticks; joined.Tick(); db.Tick();
	CHECK(db.Get(train->orders->index)->generation > generation);
	CHECK(db.Save() == joined.Save());
	CHECK(db.Plan(PAX, A, D, _state_ticks).arrival == joined.Plan(PAX, A, D, _state_ticks).arrival);
}

TEST_CASE("An OD outside the timed station index does not expand the national timed network", "[passenger][periodic]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); EnsurePassengerTestStations(std::vector<Order>{Stop(D)}); auto &db = f.Build();
	for (uint i = 0; i < 1000; ++i) {
		auto plan = db.Plan(PAX, A, D, NOW + i);
		CHECK_FALSE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.work == 1);
	}
	CHECK(db.GetDiagnostic().policy_hits == 999);
}

TEST_CASE("An unknown destination phase stays untimed without expanding other timed services", "[passenger][periodic]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); auto *unknown = f.Train({Stop(B), Stop(D)});
	unknown->current_order.Free(); auto &db = f.Build();
	auto plan = db.Plan(PAX, A, D, NOW); CHECK_FALSE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.work < 10);
	unknown->LoadAt(1); ObservePassengerPeriodicVehicle(unknown);
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; db.Tick();
	CHECK(db.Plan(PAX, A, D, NOW).ride.has_value());
}

TEST_CASE("Separate timed components reject unreachable ODs with bounded incidence work", "[passenger][periodic][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); f.Train({Stop(C), Stop(D)}); auto &db = f.Build();
	auto cold = db.Plan(PAX, A, D, NOW); CHECK_FALSE(cold.ride); CHECK_FALSE(cold.deferred); CHECK(cold.work < 32);
	auto warm = db.Plan(PAX, A, D, NOW + 999); CHECK(warm.work == cold.work); CHECK_FALSE(warm.ride);
}

TEST_CASE("Timing publication epochs use floor division for negative state ticks", "[passenger][periodic][determinism]")
{
	Fixture f; _state_ticks = StateTicks{-1}; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build();
	auto generation = db.Get(train->orders->index)->generation;
	db.TimetableChanged(train->orders->index); db.Tick(); CHECK(db.Get(train->orders->index)->generation == generation);
	_state_ticks = StateTicks{0}; db.Tick(); CHECK(db.Get(train->orders->index)->generation > generation);
}

TEST_CASE("Operating stops retain physical occurrences, dwell, phases and saved publication", "[passenger][periodic][operational][save][determinism]")
{
	Fixture f; _state_ticks = StateTicks{65536};
	Order closed = Stop(B, 20, 30); closed.SetLoadType(OrderLoadType::NoLoad); closed.SetUnloadType(OrderUnloadType::NoUnload);
	auto *train = f.Train({Stop(A, 100, 10), closed, Stop(D, 40, 10), closed});
	auto &db = f.Build(); auto *service = db.Get(train->orders->index); REQUIRE(service);
	REQUIRE(service->timetable.stops.size() == 4); CHECK(service->timetable.period == 260);
	CHECK(service->timetable.stops[1].order == 1); CHECK(service->timetable.stops[3].order == 3);
	CHECK(service->timetable.stops[1].board == 0); CHECK(service->timetable.stops[1].alight == 0);
	GameRandomSeedChecker seed;
	for (int64_t cycle : {int64_t{0}, int64_t{1}, int64_t{1000000}}) {
		auto ready = _state_ticks + cycle * 260;
		auto cold = db.Plan(PAX, A, D, ready), warm = db.Plan(PAX, A, D, ready);
		REQUIRE(cold.ride); CHECK(cold.ride->departure == ready + 10); CHECK(cold.arrival == ready + 100);
		CHECK(cold.ride == warm.ride); CHECK(cold.work == warm.work);
		CHECK_FALSE(db.Plan(PAX, A, B, ready).ride); CHECK_FALSE(db.Plan(PAX, B, D, ready).ride);
	}
	PassengerServiceGraphLimits limits; limits.horizon = 1000;
	auto graph = PassengerServiceGraph::Build(PAX, _state_ticks, limits, {}, nullptr, PassengerTimingPolicy::AssumeTimetable);
	CHECK(graph.GetServicesAtStation(B).empty()); CHECK(graph.GetStationVisits(B).empty());
	REQUIRE(graph.GetService(train->orders->index)->stops.size() == 4);
	CHECK(graph.GetTrips()[0].prediction.visits[1].order == 1);
	CHECK(graph.GetVisitStop({0, 1}).station == B);
	PassengerJourneyPlanner planner(graph);
	for (int64_t ready : {int64_t{0}, int64_t{11}, int64_t{270}}) {
		auto old = planner.Plan(PAX, A, D, _state_ticks + ready); auto current = db.Plan(PAX, A, D, _state_ticks + ready);
		REQUIRE(old.HasJourney()); REQUIRE(current.ride); CHECK(current.arrival == old.journeys.front().arrival);
	}
	auto generation = service->generation;
	train->GetOrder(1)->SetWaitTime(50); db.TimetableChanged(train->orders->index);
	auto wire = db.Save(); PassengerServiceDatabase joined; REQUIRE(joined.Load(wire)); joined.FinishLoad(true);
	CHECK(joined.Save() == wire); CHECK(joined.Get(train->orders->index)->generation == generation);
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; db.Tick(); joined.Tick();
	CHECK(db.Save() == joined.Save()); CHECK(db.Get(train->orders->index)->timetable.period == 280);
	auto updated = db.Plan(PAX, A, D, _state_ticks); REQUIRE(updated.ride);
	CHECK(updated.arrival - updated.ride->departure == StateTicksDelta{110}); CHECK(seed.Check());
	/* Removing passenger index entries must retain the physical loading lookup
	 * used by the snapshot planner for passengers already onboard. */
	train->LoadAt(1);
	auto onboard_graph = PassengerServiceGraph::Build(PAX, _state_ticks, limits,
			PassengerServiceScope{B, D, train->orders->index}, nullptr, PassengerTimingPolicy::AssumeTimetable);
	CHECK(onboard_graph.GetStationVisits(B).empty());
	auto visit = onboard_graph.FindOnboardVisit(train->index, B, 1); REQUIRE(visit);
	CHECK(onboard_graph.GetVisitStop(*visit).order == 1);
	PassengerJourneyPlanner onboard_planner(onboard_graph); auto onward = onboard_planner.PlanOnboard(PAX, *visit, D);
	REQUIRE(onward.HasJourney()); CHECK(onward.journeys.front().arrival == _state_ticks + 90);
	CHECK(onward.journeys.front().transfers == 0);
}

TEST_CASE("Cargo-specific operating stops cannot join passenger service components", "[passenger][periodic][operational][index]")
{
	Fixture f; constexpr CargoType OTHER{1};
	AutoRestoreBackup classes{CargoSpec::Get(OTHER)->classes, CargoClasses{CargoClass::Passengers}};
	AutoRestoreBackup distribution{_settings_game.linkgraph.distribution_per_cargo[OTHER], DistributionType::Asymmetric};
	Order closed = Stop(B, 20, 30);
	closed.SetLoadType(OrderLoadType::CargoTypeLoad); closed.SetLoadType(OrderLoadType::NoLoad, PAX);
	closed.SetUnloadType(OrderUnloadType::CargoTypeUnload); closed.SetUnloadType(OrderUnloadType::NoUnload, PAX);
	auto *first = f.Train({Stop(A), closed}); auto *second = f.Train({Stop(C), closed, Stop(D)});
	for (auto *train : {first, second}) { auto *part = Vehicle::Create<PeriodicTrain>(train); part->cargo_type = OTHER; }
	auto &db = f.Build(); CHECK_FALSE(db.Plan(PAX, A, D, _state_ticks).ride); REQUIRE(db.Plan(OTHER, A, D, _state_ticks).ride);
	PassengerServiceGraphLimits limits; limits.horizon = 3000;
	for (CargoType cargo : {PAX, OTHER, PAX}) {
		auto graph = PassengerServiceGraph::Build(cargo, _state_ticks, limits, PassengerServiceScope{A, D}, nullptr, PassengerTimingPolicy::AssumeTimetable);
		if (cargo == PAX) { CHECK(graph.GetServices().empty()); CHECK(graph.GetServicesAtStation(B).empty()); }
		else { CHECK(graph.GetServices().size() == 2); CHECK(graph.GetServicesAtStation(B).size() == 2); }
	}
	/* Permission edits must invalidate a warm corridor even though neither the
	 * destination stations nor the eligible fleet IDs changed. */
	AutoRestoreBackup company{_current_company, OWNER_DEITY};
	AutoRestoreBackup first_owner{first->owner, OWNER_DEITY}, second_owner{second->owner, OWNER_DEITY};
	REQUIRE(Command<Commands::ModifyOrder>::Do({DoCommandFlag::Execute}, first->index, VehicleOrderID{1},
			MOF_CARGO_TYPE_UNLOAD, static_cast<uint16_t>(OrderUnloadType::UnloadIfPossible), PAX, {}).Succeeded());
	REQUIRE(Command<Commands::ModifyOrder>::Do({DoCommandFlag::Execute}, second->index, VehicleOrderID{1},
			MOF_CARGO_TYPE_LOAD, static_cast<uint16_t>(OrderLoadType::LoadIfPossible), PAX, {}).Succeeded());
	auto changed = PassengerServiceGraph::Build(PAX, _state_ticks, limits, PassengerServiceScope{A, D}, nullptr, PassengerTimingPolicy::AssumeTimetable);
	CHECK(changed.GetServices().size() == 2); CHECK_FALSE(changed.GetDiagnostic().static_cache_hit);
	PassengerJourneyPlanner planner(changed); REQUIRE(planner.Plan(PAX, A, D, _state_ticks).HasJourney());
}

TEST_CASE("Mixed passenger exchange stops keep directional permissions", "[passenger][periodic][operational]")
{
	Fixture f; Order board = Stop(A), alight = Stop(D), closed = Stop(C, 20, 30);
	SECTION("Different operating station") {}
	SECTION("Operating stop returns to the boarding StationID") { closed.SetDestination(A); }
	board.SetUnloadType(OrderUnloadType::NoUnload); alight.SetLoadType(OrderLoadType::NoLoad);
	closed.SetLoadType(OrderLoadType::NoLoad); closed.SetUnloadType(OrderUnloadType::NoUnload);
	auto *train = f.Train({board, Stop(B), closed, alight}); auto &db = f.Build();
	REQUIRE(db.Plan(PAX, A, D, NOW).ride); REQUIRE(db.Plan(PAX, B, D, NOW).ride);
	CHECK_FALSE(db.Plan(PAX, B, A, NOW).ride); CHECK_FALSE(db.Plan(PAX, D, B, NOW).ride);
	CHECK_FALSE(db.Plan(PAX, A, C, NOW).ride); CHECK_FALSE(db.Plan(PAX, C, D, NOW).ride);
	PassengerServiceGraphLimits limits; limits.horizon = 2000;
	auto graph = PassengerServiceGraph::Build(PAX, NOW, limits, {}, nullptr, PassengerTimingPolicy::AssumeTimetable);
	PassengerJourneyPlanner planner(graph);
	REQUIRE(planner.Plan(PAX, A, D, NOW).HasJourney()); CHECK_FALSE(planner.Plan(PAX, B, A, NOW).HasJourney());
	CHECK_FALSE(planner.Plan(PAX, D, B, NOW).HasJourney()); CHECK(graph.GetServicesAtStation(C).empty());
	PassengerRouteState state{train->orders->GetOrderVector(), {}, 0, 0, 0, true, false, 1};
	auto prediction = PredictPassengerJourney(state, 0, D, PAX, NOW, true, PassengerTimingPolicy::AssumeTimetable);
	CHECK(prediction.arrival == NOW + 270);
	auto *packet = f.Packet(); PassengerOdFallbackContext fallback; CHECK(fallback.CanBoard(*packet, train, PAX));
	auto ride = db.Plan(PAX, A, D, NOW).ride; REQUIRE(ride); CHECK(ValidatePassengerServiceRide(*ride, train, PAX, true));
	train->LoadAt(2); train->current_order.SetLoadType(OrderLoadType::NoLoad); train->current_order.SetUnloadType(OrderUnloadType::NoUnload);
	CHECK(ValidatePassengerServiceRide(*ride, train, PAX, false)); CHECK_FALSE(ValidatePassengerServiceRide(*ride, train, PAX, true));
}

TEST_CASE("Operating first stop remains a dispatch and phase anchor", "[passenger][periodic][operational][dispatch][save]")
{
	Fixture f; Order closed = Stop(B, 20, 30); closed.SetLoadType(OrderLoadType::NoLoad); closed.SetUnloadType(OrderUnloadType::NoUnload);
	auto *train = f.Train({closed, Stop(A, 40, 0), Stop(D, 50, 0)});
	train->vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
	train->orders->GetScheduledDispatchScheduleSet().emplace_back();
	auto &schedule = train->orders->GetDispatchScheduleByIndex(0);
	schedule.SetScheduledDispatchStartTick(NOW); schedule.SetScheduledDispatchDuration(300);
	schedule.GetScheduledDispatchMutable().push_back({30, 0}); train->GetOrder(0)->SetDispatchScheduleIndex(0);
	auto &db = f.Build(); auto *service = db.Get(train->orders->index); REQUIRE(service);
	CHECK(service->dispatch_stop == 0); CHECK(service->timetable.stops[0].station == B);
	for (int64_t cycle : {int64_t{0}, int64_t{1}, int64_t{1000}}) {
		auto plan = db.Plan(PAX, A, D, NOW + 300 * cycle); REQUIRE(plan.ride);
		CHECK(plan.ride->boarding_order == 1); CHECK(plan.ride->departure == NOW + 300 * cycle + 70); CHECK(plan.arrival == NOW + 300 * cycle + 120);
	}
	auto wire = db.Save(); PassengerServiceDatabase restored; REQUIRE(restored.Load(wire)); restored.FinishLoad(true);
	CHECK(restored.Save() == wire); CHECK(restored.Plan(PAX, A, D, NOW).ride == db.Plan(PAX, A, D, NOW).ride);
}

TEST_CASE("Waypoint waits and reversals are operating events in periodic and snapshot cycles", "[passenger][periodic][operational][comparison]")
{
	Fixture f; Order waypoint; waypoint.MakeGoToWaypoint(C);
	waypoint.SetWaypointFlags({OrderWaypointFlag::Reverse}); waypoint.SetTravelTime(20); waypoint.SetTravelTimetabled(true);
	waypoint.SetWaitTime(30); waypoint.SetWaitTimetabled(true);
	bool timed = true; int64_t dwell = 30;
	SECTION("Declared dwell") {}
	SECTION("Explicit zero dwell") { dwell = 0; waypoint.SetWaitTime(0); }
	SECTION("Passing waypoint has no dwell instruction") { dwell = 0; waypoint.SetWaitTime(0); waypoint.SetWaitTimetabled(false); }
	SECTION("Missing travel zero is unknown") { timed = false; waypoint.SetTravelTime(0); waypoint.SetTravelTimetabled(false); }
	SECTION("Explicit travel zero") { waypoint.SetTravelTime(0); }
	SECTION("Missing positive dwell is unknown") { timed = false; waypoint.SetWaitTimetabled(false); }
	f.Train({Stop(A, 100, 10), waypoint, Stop(D, 40, 0)}); auto &db = f.Build();
	auto plan = db.Plan(PAX, A, D, NOW); CHECK(plan.ride.has_value() == timed);
	CHECK_FALSE(db.Plan(PAX, A, C, NOW).ride); CHECK_FALSE(db.Plan(PAX, C, D, NOW).ride);
	PassengerServiceGraphLimits limits; limits.horizon = 2000;
	auto graph = PassengerServiceGraph::Build(PAX, NOW, limits, {}, nullptr, PassengerTimingPolicy::AssumeTimetable);
	CHECK(graph.GetServicesAtStation(C).empty()); CHECK(graph.GetServices()[0].reverse_waypoints.size() == 1);
	if (timed) {
		CHECK(plan.arrival == NOW + 10 + waypoint.GetTravelTime() + dwell + 40);
		PassengerJourneyPlanner planner(graph); auto old = planner.Plan(PAX, A, D, NOW); REQUIRE(old.HasJourney()); CHECK(old.journeys.front().arrival == plan.arrival);
	}
}

TEST_CASE("Depot traversal separates passenger continuity from timing certainty", "[passenger][periodic][operational][depot][active]")
{
	Fixture f; Order depot; depot.MakeGoToDepot(DepotID{0}, {OrderDepotTypeFlag::PartOfOrders});
	depot.SetTravelTime(20); depot.SetTravelTimetabled(true); depot.SetWaitTime(30); depot.SetWaitTimetabled(true);
	auto expected = PassengerDepotTraversal::Timed;
	bool timed = true;
	SECTION("Regular entry with declared dwell") {}
	SECTION("Explicit zero travel and dwell") { depot.SetTravelTime(0); depot.SetWaitTime(0); }
	SECTION("Missing travel") { timed = false; depot.SetTravelTime(0); depot.SetTravelTimetabled(false); }
	SECTION("Missing dwell") { timed = false; depot.SetWaitTime(0); depot.SetWaitTimetabled(false); }
	SECTION("Only when servicing is needed") { expected = PassengerDepotTraversal::Untimed; timed = false; depot.SetDepotOrderType({OrderDepotTypeFlag::PartOfOrders, OrderDepotTypeFlag::Service}); }
	SECTION("Nearest depot") { expected = PassengerDepotTraversal::Untimed; timed = false; depot.SetDepotActionType(ODATFB_NEAREST_DEPOT); }
	SECTION("Unbunching") { expected = PassengerDepotTraversal::Untimed; timed = false; depot.SetDepotActionType(ODATFB_UNBUNCH); }
	SECTION("Halt") { expected = PassengerDepotTraversal::Blocked; timed = false; depot.SetDepotActionType(ODATFB_HALT); }
	SECTION("Sell without halt bit") { expected = PassengerDepotTraversal::Blocked; timed = false; depot.SetDepotActionType(ODATFB_SELL); }
	SECTION("Refit even to same cargo") { expected = PassengerDepotTraversal::Blocked; timed = false; depot.SetRefit(PAX); }
	SECTION("Automatic depot instruction") { expected = PassengerDepotTraversal::Blocked; timed = false; depot.SetDepotOrderType({}); }
	CHECK(ClassifyPassengerDepot(depot) == expected);
	auto *train = f.Train({Stop(A, 100, 10), depot, Stop(B, 40, 5), Stop(D, 60, 0)}); auto &db = f.Build();
	auto plan = db.Plan(PAX, A, D, NOW); CHECK(plan.ride.has_value() == timed);
	auto *packet = f.Packet(); PassengerOdFallbackContext fallback;
	const bool continuing = expected != PassengerDepotTraversal::Blocked;
	CHECK(fallback.CanBoard(*packet, train, PAX) == continuing);
	PassengerJourneyExecution ride; ride.kind = PassengerRideKind::ServiceIntent;
	ride.service = train->orders->index; ride.signature = train->orders->GetCargoDistRouteGeneration();
	ride.generation = db.Get(train->orders->index)->generation;
	ride.boarding = A; ride.alighting = D; ride.boarding_order = 0; ride.alighting_order = 3; ride.not_before = NOW;
	CHECK(ValidatePassengerServiceRide(ride, train, PAX, true) == continuing);
	PassengerRouteState state{train->orders->GetOrderVector(), {}, 0, 0, 0, true, false, 1};
	auto predicted = PredictPassengerJourney(state, 0, D, PAX, NOW, true, PassengerTimingPolicy::AssumeTimetable);
	CHECK(predicted.arrival.has_value() == timed);
	if (timed) {
		CHECK(plan.arrival == NOW + 10 + depot.GetTravelTime() + depot.GetWaitTime() + 40 + 5 + 60);
		CHECK(predicted.arrival == plan.arrival);
		PassengerServiceGraphLimits limits; limits.horizon = 2000;
		auto graph = PassengerServiceGraph::Build(PAX, NOW, limits, {}, nullptr, PassengerTimingPolicy::AssumeTimetable);
		CHECK(graph.GetService(train->orders->index)->stops.size() == 3);
		PassengerJourneyPlanner planner(graph); auto old = planner.Plan(PAX, A, D, NOW); REQUIRE(old.HasJourney()); CHECK(old.journeys.front().arrival == plan.arrival);
	} else if (continuing) { CHECK(predicted.reachable); CHECK_FALSE(predicted.arrival); }
	if (continuing) {
		train->GetOrder(2)->SetUnloadType(OrderUnloadType::Transfer);
		auto *interchange = CargoPacket::Create(1, 0, A, TileIndex{0}, Money{0}); REQUIRE(interchange->SetPassengerDestination(PAX, B));
		PassengerOdFallbackContext forced; CHECK(forced.CanBoard(*interchange, train, PAX)); CHECK_FALSE(forced.CanBoard(*packet, train, PAX));
		CHECK_FALSE(ValidatePassengerServiceRide(ride, train, PAX, true));
	}
}

TEST_CASE("Schedule duration-only direct policies retain no invented absolute times", "[passenger][schedule]")
{
	Fixture f; auto *v = f.Train({Stop(A, 80, 10), Stop(B, 120, 20), Stop(D, 80, 0)}); v->current_order.Free(); auto &db = f.Build();
	CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride);
	auto plan = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(plan.ride);
	CHECK(plan.riding == 220); CHECK(plan.waiting == 155); CHECK(plan.transfers == 0);
	CHECK(plan.ride->kind == PassengerRideKind::EstimatedServiceIntent); CHECK(plan.ride->vehicle == VehicleID::Invalid());
	CHECK(plan.ride->departure == StateTicks{}); CHECK(plan.ride->arrival == StateTicks{}); CHECK(plan.ride->not_before == NOW);
	v->LoadAt(0); PassengerJourneyLoadingContext loading; auto *packet = f.Packet(); packet->SetPassengerJourney(*plan.ride, D);
	REQUIRE(loading.CanBoard(*packet, v)); loading.PrepareBoard(*packet, v);
	CHECK(packet->GetPassengerJourney().kind == PassengerRideKind::EstimatedBoundService);
	v->LoadAt(1); PassengerJourneyActiveContext arrived(v, B); CHECK(arrived.Decide(*packet, PAX, false, OrderUnloadType::UnloadIfPossible) == VehicleCargoList::MoveToAction::Keep);
}

TEST_CASE("Schedule routes compare frequency capacity direct and transfer costs", "[passenger][schedule]")
{
	Fixture f; auto *local = f.Train({Stop(A, 100, 0), Stop(B, 50, 20), Stop(D, 50, 0)});
	auto *express = f.Train({Stop(A, 1000, 0), Stop(D, 10, 0)});
	local->current_order.Free(); express->current_order.Free();
	SECTION("Infrequent express loses despite short riding time") {}
	SECTION("Frequent express wins") { express->GetOrder(0)->SetTravelTime(50); }
	SECTION("Low capacity express loses") { express->GetOrder(0)->SetTravelTime(200); express->cargo_cap = 1; }
	auto &db = f.Build(); auto plan = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(plan.ride);
	CHECK(plan.ride->service == (express->GetOrder(0)->GetTravelTime() == 50 ? express : local)->orders->index);
}

TEST_CASE("Schedule compares multiple interchange corridors including known phases", "[passenger][schedule]")
{
	Fixture f;
	auto *ab = f.Train({Stop(A, 50, 0), Stop(B, 50, 0)}); auto *bd = f.Train({Stop(B, 50, 0), Stop(D, 50, 0)});
	auto *ac = f.Train({Stop(A, 900, 0), Stop(C, 25, 0)}); auto *cd = f.Train({Stop(C, 900, 0), Stop(D, 25, 0)});
	auto *direct = f.Train({Stop(A, 250, 0), Stop(D, 250, 0)});
	for (auto *v : {ab, bd, ac, cd, direct}) v->current_order.Free();
	SECTION("Both interchange legs unknown") {}
	SECTION("Downstream timing known does not promise a connection") { bd->LoadAt(0); }
	SECTION("Upstream timing known still produces a duration policy") { ab->LoadAt(0); }
	auto &db = f.Build(); PassengerJourneyEvaluation evaluation; evaluation.minimum_transfer = 10; evaluation.transfer_penalty = 10;
	auto plan = db.PlanEstimated(PAX, A, D, NOW, {}, evaluation); REQUIRE(plan.ride);
	CHECK(plan.ride->service == ab->orders->index); CHECK(plan.ride->alighting == B); CHECK(plan.transfers == 1);
	PassengerJourneyLimits limits; limits.max_transfers = 0;
	auto no_transfer = db.PlanEstimated(PAX, A, D, NOW, limits, evaluation); REQUIRE(no_transfer.ride); CHECK(no_transfer.ride->service == direct->orders->index);
}

TEST_CASE("Schedule fleet frequency never fabricates evenly spaced unknown phases", "[passenger][schedule]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); auto *shared = Vehicle::Create<PeriodicTrain>(v);
	v->current_order.Free(); shared->current_order.Free(); auto &db = f.Build();
	auto unknown = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(unknown.ride); CHECK(unknown.ride->estimated_wait == 220);
	v->LoadAt(0); shared->LoadAt(1);
	db.Invalidate(v->orders->index); f.PublishUpdates(db);
	auto known = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(known.ride); CHECK(known.ride->estimated_wait == 55);
}

TEST_CASE("Schedule active selection gives timed OD routes priority", "[passenger][schedule][active]")
{
	Fixture f; auto *timed = f.Train({Stop(A, 500, 0), Stop(D, 500, 0)});
	auto *unknown = f.Train({Stop(A, 10, 0), Stop(D, 10, 0)}); unknown->current_order.Free(); auto &db = f.Build();
	auto *packet = f.Packet(); PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, D));
	CHECK(packet->GetPassengerJourney().service == timed->orders->index); CHECK(packet->GetPassengerJourney().kind == PassengerRideKind::ServiceIntent);
	CHECK(db.GetScheduleDiagnostic().misses == 0);
}

TEST_CASE("Schedule work admission is identical across hits eviction saved DB and peers", "[passenger][schedule][determinism][save]")
{
	Fixture f; auto *ab = f.Train({Stop(A), Stop(B)}); auto *bd = f.Train({Stop(B), Stop(D)});
	ab->current_order.Free(); bd->current_order.Free(); auto &db = f.Build(); GameRandomSeedChecker seeds;
	auto cold = db.PlanEstimated(PAX, A, D, NOW); auto hot = db.PlanEstimated(PAX, A, D, NOW);
	REQUIRE(cold.ride); CHECK(cold.ride == hot.ride); CHECK(cold.work == hot.work);
	db.ClearPolicies(); CHECK(db.PlanEstimated(PAX, A, D, NOW).ride == cold.ride);
	auto wire = db.Save(); PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
	CHECK(peer.PlanEstimated(PAX, A, D, NOW).ride == cold.ride); CHECK(peer.PlanEstimated(PAX, A, D, NOW).work == cold.work); CHECK(db.Save() == wire);
	PassengerJourneyLimits limits; limits.work = cold.work - 1;
	auto limited = db.PlanEstimated(PAX, A, D, NOW, limits); CHECK_FALSE(limited.ride); CHECK(limited.deferred); CHECK(limited.work <= limits.work);
	CHECK(db.PlanEstimated(PAX, A, D, NOW, limits).work == limited.work); CHECK(seeds.Check());
	limits = {}; limits.memory_bytes = 0; CHECK(db.PlanEstimated(PAX, A, D, NOW, limits).deferred);
	limits = {}; limits.labels = 0; CHECK(db.PlanEstimated(PAX, A, D, NOW, limits).deferred);
}

TEST_CASE("Schedule actual shared carriers use reservations splits and saved independent intents", "[passenger][schedule][active][save]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); auto *shared = Vehicle::Create<PeriodicTrain>(v);
	v->current_order.Free(); shared->current_order.Free(); f.Build(); auto *packet = f.Packet(200);
	PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, D));
	auto intent = packet->GetPassengerJourney();
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION}; AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	auto *loaded = CargoPacket::Create(); REQUIRE_FALSE(SlTestSaveLoadTable(packet, loaded, GetCargoPacketDesc()).empty()); CHECK(loaded->GetPassengerJourney() == intent);
	v->LoadAt(0); shared->LoadAt(0); auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo; waiting.Append(packet, StationID::Invalid());
	PassengerJourneyLoadingContext loading; std::array next{D};
	CHECK(waiting.Reserve(70, &shared->cargo, next, TileIndex{0}, shared, &loading) == 70);
	CHECK(packet->GetPassengerJourney() == intent); CHECK(shared->cargo.Packets()->front()->GetPassengerJourney().vehicle == shared->index);
	CHECK(waiting.Load(35, &shared->cargo, next, TileIndex{0}, shared, &loading) == 35);
	CHECK(waiting.Load(35, &shared->cargo, next, TileIndex{0}, shared, &loading) == 35);
	CHECK(GetPassengerJourneyTickDiagnostic().schedule_boarded_passengers == 70); CHECK(GetPassengerJourneyTickDiagnostic().boarded_passengers == 0);
	CHECK(waiting.Load(100, &v->cargo, next, TileIndex{0}, v, &loading) == 100);
	CHECK(packet->Count() == 30); CHECK(packet->GetPassengerJourney() == intent);
	CHECK(packet->GetPassengerTransport() == PassengerTransportKind::None);
	REQUIRE(packet->Merge(loaded)); CHECK(packet->Count() == 230);
	auto *different = packet->Split(10); auto changed = different->GetPassengerJourney(); ++changed.generation;
	different->SetPassengerJourney(changed, D); CHECK_FALSE(packet->Merge(different));
}

TEST_CASE("Schedule boarding enforces live restrictions incarnation and generation", "[passenger][schedule][active]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(B), Stop(D)}); v->current_order.Free(); auto &db = f.Build();
	auto plan = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(plan.ride); v->LoadAt(0); CHECK(ValidatePassengerServiceRide(*plan.ride, v, PAX, true));
	SECTION("NoLoad") { v->GetOrder(0)->SetLoadType(OrderLoadType::NoLoad); }
	SECTION("NoUnload") { v->GetOrder(2)->SetUnloadType(OrderUnloadType::NoUnload); }
	SECTION("Forced unload") { v->GetOrder(1)->SetUnloadType(OrderUnloadType::Unload); }
	SECTION("Forced transfer") { v->GetOrder(1)->SetUnloadType(OrderUnloadType::Transfer); }
	SECTION("Capacity removed") { v->cargo_cap = 0; }
	SECTION("Facilities removed") { Station::Get(D)->facilities = {}; }
	SECTION("Vehicle stopped") { v->vehstatus.Set(VehState::Stopped); }
	SECTION("Generation changed") { db.Invalidate(v->orders->index); db.Tick(); }
	SECTION("Incarnation changed") { plan.ride->signature++; }
	SECTION("Wrong occurrence") { plan.ride->boarding_order = 1; }
	CHECK_FALSE(ValidatePassengerServiceRide(*plan.ride, v, PAX, true));
}

TEST_CASE("Schedule transfer constraints survive replanning fallback and packet save", "[passenger][schedule][active][save]")
{
	Fixture f; auto *ab = f.Train({Stop(A), Stop(B)}); auto *bd = f.Train({Stop(B), Stop(D)});
	ab->current_order.Free(); bd->current_order.Free(); auto &db = f.Build(); auto *packet = f.Packet();
	REQUIRE(PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*packet, PAX, D));
	ab->LoadAt(0); PassengerJourneyLoadingContext loading; REQUIRE(loading.CanBoard(*packet, ab)); loading.PrepareBoard(*packet, ab);
	_state_ticks += 100; ab->LoadAt(1); _settings_game.linkgraph.passenger_transfer_penalty = 20;
	PassengerJourneyActiveContext arrived(ab, B); CHECK(arrived.Decide(*packet, PAX, false, OrderUnloadType::UnloadIfPossible) == VehicleCargoList::MoveToAction::Transfer);
	REQUIRE(packet->GetPassengerJourney().kind == PassengerRideKind::EstimatedServiceIntent); CHECK(packet->GetPassengerJourney().service == bd->orders->index);
	bd->LoadAt(0); CHECK_FALSE(loading.CanBoard(*packet, bd)); CHECK_FALSE(loading.CanBoard(*packet, ab));
	_state_ticks += 20; CHECK(loading.CanBoard(*packet, bd));
	packet->FallbackPassengerJourney(B, _state_ticks); CHECK_FALSE(loading.CanBoard(*packet, ab));
	CHECK(packet->GetPassengerInterchangeReady() == NOW + 120); CHECK(packet->GetPassengerArrivingService() == ab->orders->index);
	CHECK(db.GetScheduleDiagnostic().misses >= 2);
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION}; AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	auto *loaded = CargoPacket::Create(); REQUIRE_FALSE(SlTestSaveLoadTable(packet, loaded, GetCargoPacketDesc()).empty());
	CHECK(loaded->HasSamePassengerRouting(*packet));
}

TEST_CASE("Schedule expiry and timetable deletion safely trigger another OD policy", "[passenger][schedule][active]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); v->current_order.Free(); auto &db = f.Build(); auto *packet = f.Packet();
	REQUIRE(PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*packet, PAX, D));
	Station::Get(A)->goods[PAX].CreateData().cargo.Append(packet, StationID::Invalid());
	SECTION("Long wait") { _state_ticks += 441; }
	SECTION("Generation") { db.Invalidate(v->orders->index); db.Tick(); _state_ticks++; }
	BeginPassengerJourneyTick(); Station::Get(A)->goods[PAX].data->cargo.RoutePassengerJourneys(PAX, A);
	CHECK(packet->GetPassengerDestination() == D); CHECK(packet->GetPassengerJourney().departure == StateTicks{});
	CHECK(GetPassengerJourneyTickDiagnostic().schedule_expired + GetPassengerJourneyTickDiagnostic().schedule_stale == 1);
}

TEST_CASE("Schedule 8000 station network caches OD policies without physical visit expansion", "[passenger][schedule][scaling]")
{
	Fixture f;
	for (uint16_t i = 0; i < 4000; ++i) f.Train({Stop(StationID{static_cast<uint16_t>(2 * i)}), Stop(StationID{static_cast<uint16_t>(2 * i + 1)})})->current_order.Free();
	auto &db = GetPassengerServiceDatabase(); for (int i = 0; i < 100; ++i) { _state_ticks++; db.Tick(); }
	REQUIRE(db.GetDiagnostic().vehicles == 4000); auto cold = db.PlanEstimated(PAX, A, B, _state_ticks); REQUIRE(cold.ride);
	for (int i = 0; i < 4096; ++i) CHECK(db.PlanEstimated(PAX, A, B, _state_ticks).work == cold.work);
	CHECK(db.GetScheduleDiagnostic().hits == 4096); CHECK(cold.work < 64);
	CHECK(GetPassengerJourneyTickDiagnostic().builds == 0); CHECK(GetPassengerJourneyTickDiagnostic().prediction_executions == 0);
	auto d = db.GetScheduleDiagnostic();
	WARN("Schedule 8000-station benchmark: queries=" << d.hits + d.misses << "; work_per_query=" << cold.work << "; total_query_us=" << d.query_microseconds << "; policy_bytes=" << d.bytes);
}

TEST_CASE("Schedule reads old periodic wire state and unresolved dispatch circuits conservatively", "[passenger][schedule][save][dispatch]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); v->vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
	v->orders->GetScheduledDispatchScheduleSet().emplace_back(); auto &schedule = v->orders->GetDispatchScheduleByIndex(0);
	schedule.SetScheduledDispatchStartTick(NOW); schedule.SetScheduledDispatchDuration(2000);
	schedule.GetScheduledDispatchMutable().push_back({10, 0}); v->GetOrder(0)->SetDispatchScheduleIndex(0);
	Vehicle::Create<PeriodicTrain>(v)->vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
	auto &db = f.Build(); auto plan = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK(plan.ride->estimated_wait == 2000);
	auto wire = db.Save(); PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
	CHECK(peer.PlanEstimated(PAX, A, D, NOW).ride == plan.ride);
	/* Version one had the same record prefix without the final dispatch estimate. */
	wire.resize(wire.size() - 7 * 8); wire[0] = 1; wire.resize(wire.size() - 8); REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
	CHECK(peer.Get(v->orders->index) == nullptr); CHECK(peer.GetDiagnostic().pending != 0);
	peer.Tick(); REQUIRE(peer.Get(v->orders->index) != nullptr);
	CHECK(peer.Get(v->orders->index)->uncertain_dispatch_period == 2000); CHECK(peer.PlanEstimated(PAX, A, D, NOW).ride.has_value());
}

TEST_CASE("Schedule forced alighting replans without boarding the arriving service", "[passenger][schedule][active]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(B), Stop(D)}); auto *next = f.Train({Stop(B), Stop(D)});
	v->current_order.Free(); next->current_order.Free(); auto &db = f.Build();
	auto *packet = f.Packet(); auto plan = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(plan.ride);
	packet->SetPassengerJourney(*plan.ride, D); v->LoadAt(0); PassengerJourneyLoadingContext loading; loading.PrepareBoard(*packet, v);
	v->LoadAt(1); _state_ticks += 100; _settings_game.linkgraph.passenger_transfer_penalty = 20;
	OrderUnloadType unload = GENERATE(OrderUnloadType::Unload, OrderUnloadType::Transfer);
	PassengerJourneyActiveContext arrived(v, B); CHECK(arrived.Decide(*packet, PAX, true, unload) == VehicleCargoList::MoveToAction::Transfer);
	CHECK(packet->GetPassengerDestination() == D); CHECK(packet->GetPassengerJourney().service == next->orders->index);
	CHECK(packet->GetPassengerJourney().not_before == _state_ticks + 20);
}

TEST_CASE("Schedule interchange exclusion distinguishes recycled order list incarnations", "[passenger][schedule][active]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto *packet = f.Packet();
	OrderListID id = v->orders->index; uint64_t incarnation = v->orders->GetCargoDistRouteGeneration();
	packet->SetPassengerInterchange(NOW + 20, id, incarnation);
	v->orders->FreeChain(); v->orders = OrderList::Create(std::vector<Order>{Stop(A), Stop(D)}, v); v->LoadAt(0); f.PublishUpdates(db);
	REQUIRE(v->orders->index == id); REQUIRE(v->orders->GetCargoDistRouteGeneration() != incarnation);
	_state_ticks += 20; PassengerJourneyActiveContext waiting(nullptr, A); REQUIRE(waiting.PlanWaiting(*packet, PAX, D));
	CHECK(PassengerJourneyLoadingContext().CanBoard(*packet, v));
}

TEST_CASE("Timed interchange exclusion remains correct across shared policy cache hits", "[passenger][periodic][schedule][determinism]")
{
	Fixture f; auto *arriving = f.Train({Stop(A, 100, 0), Stop(D, 100, 0)});
	auto *next = f.Train({Stop(A, 100, 0), Stop(B, 50, 0), Stop(D, 150, 0)}); auto &db = f.Build();
	auto unrestricted = db.Plan(PAX, A, D, NOW); REQUIRE(unrestricted.ride); CHECK(unrestricted.ride->service == arriving->orders->index);
	auto excluded = db.Plan(PAX, A, D, NOW, {}, {}, arriving->orders->index);
	REQUIRE(excluded.ride); CHECK_FALSE(excluded.deferred); CHECK(excluded.ride->service == next->orders->index); CHECK(excluded.arrival == NOW + 200);
	db.ClearPolicies();
	auto cold = db.Plan(PAX, A, D, NOW, {}, {}, arriving->orders->index);
	CHECK(cold.ride == excluded.ride); CHECK(cold.arrival == excluded.arrival); CHECK(cold.work == excluded.work);
}

TEST_CASE("Observing an unknown phase preserves estimated boarding until timing publication", "[passenger][periodic][schedule][active]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); v->current_order.Free(); auto &db = f.Build();
	auto plan = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(plan.ride);
	v->LoadAt(0); ObservePassengerPeriodicVehicle(v); db.Tick();
	CHECK(db.IsCurrent(*plan.ride)); CHECK(ValidatePassengerServiceRide(*plan.ride, v, PAX, true));
	CHECK(db.GetPublication(v->orders->index) == PassengerServiceDatabase::Publication::TimingDirty);
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; db.Tick();
	CHECK(db.Get(v->orders->index)->vehicles[0].phase_known);
}

TEST_CASE("Former schedule journeys refresh constraints on a later fallback transfer", "[passenger][schedule][active][fallback]")
{
	Fixture f; auto *old = f.Train({Stop(A), Stop(B)}); auto *v = f.Train({Stop(B), Stop(D)}); f.Build(); auto *packet = f.Packet();
	packet->SetPassengerInterchange(NOW, old->orders->index, old->orders->GetCargoDistRouteGeneration());
	packet->FallbackPassengerJourney(B, NOW); _settings_game.linkgraph.passenger_transfer_penalty = 20;
	PassengerJourneyActiveContext arrived(v, B); REQUIRE(arrived.Decide(*packet, PAX, false, OrderUnloadType::Transfer) == VehicleCargoList::MoveToAction::Transfer);
	CHECK(packet->GetPassengerArrivingService() == v->orders->index); CHECK(packet->GetPassengerInterchangeReady() == NOW + 20);
}

TEST_CASE("Schedule bindings continue from a real implicit stop without forecasting it", "[passenger][schedule][active]")
{
	Fixture f; Order implicit; implicit.MakeImplicit(B); auto *v = f.Train({Stop(A), implicit, Stop(D)});
	EnsurePassengerTestStations(std::vector<Order>{Stop(B)}); v->current_order.Free(); auto &db = f.Build();
	auto plan = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK(db.Get(v->orders->index)->timetable.stops.size() == 2);
	auto *packet = f.Packet(); packet->SetPassengerJourney(*plan.ride, D); v->LoadAt(0);
	PassengerJourneyLoadingContext loading; REQUIRE(loading.CanBoard(*packet, v)); loading.PrepareBoard(*packet, v);
	v->LoadAt(1); v->cur_real_order_index = 2;
	CHECK_FALSE(GetPassengerRouteState(v)); // Forecasters still reject this undeclared current occurrence.
	PassengerJourneyActiveContext arrived(v, B); CHECK(arrived.Decide(*packet, PAX, false, OrderUnloadType::UnloadIfPossible) == VehicleCargoList::MoveToAction::Keep);
	CHECK(packet->GetPassengerJourney().kind == PassengerRideKind::EstimatedBoundService);
}

TEST_CASE("Eight slow direct lines cannot consume the timed transfer candidate budget", "[passenger][periodic][comparison]")
{
	Fixture f;
	std::vector<PeriodicTrain *> direct;
	for (int i = 0; i < 10; ++i) direct.push_back(f.Train({Stop(A, 100, 0), Stop(D, 1000, 0)}));
	auto *feeder = f.Train({Stop(A, 1900, 0), Stop(B, 100, 0)});
	f.Train({Stop(B, 1900, 100), Stop(D, 100, 0)});
	auto &db = f.Build();
	for (size_t count : {size_t{1}, size_t{8}, size_t{16}}) {
		PassengerJourneyLimits limits; limits.candidates = count;
		auto early = db.Plan(PAX, A, D, NOW, limits);
		REQUIRE(early.ride); CHECK_FALSE(early.deferred);
		CHECK(early.ride->service == feeder->orders->index); CHECK(early.arrival == NOW + 200);
		REQUIRE(early.route.size() == 2); CHECK(early.route.front().alighting == B);
		auto late = db.Plan(PAX, A, D, NOW + 1, limits);
		REQUIRE(late.ride); CHECK_FALSE(late.deferred);
		CHECK(late.ride->service == direct.front()->orders->index); CHECK(late.arrival == NOW + 2100);
	}
	auto paired = ComparePassengerPlanners(db, PAX, A, D, NOW); REQUIRE(paired.periodic.ride); REQUIRE_FALSE(paired.legacy_route.empty()); REQUIRE(paired.legacy_found); CHECK(paired.periodic.arrival == paired.legacy_arrival);
}

TEST_CASE("Periodic timed search retains a nominally slower express with an earlier phase", "[passenger][periodic]")
{
	Fixture f;
	for (int i = 0; i < 10; ++i) f.Train({Stop(A, 1000, 500), Stop(D, 1, 0)});
	auto *express = f.Train({Stop(A, 1000, 0), Stop(D, 100, 0)});
	auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW);
	REQUIRE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.ride->service == express->orders->index);
	CHECK(plan.arrival == NOW + 100);
}

TEST_CASE("Periodic local express and multiple interchange paths compete by actual arrival", "[passenger][periodic]")
{
	Fixture f;
	auto *local = f.Train({Stop(A, 900, 0), Stop(B, 100, 0), Stop(D, 900, 0)});
	f.Train({Stop(B, 900, 100), Stop(C, 50, 0)});
	f.Train({Stop(C, 900, 160), Stop(D, 40, 0)});
	auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW);
	REQUIRE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.arrival == NOW + 200);
	CHECK(plan.transfers == 2); REQUIRE(plan.route.size() == 3);
	CHECK(plan.route[0].service == local->orders->index); CHECK(plan.route[0].alighting == B); CHECK(plan.route[1].alighting == C);
}

TEST_CASE("Periodic exhausted search and resource truncation have distinct diagnoses", "[passenger][periodic][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); f.Train({Stop(B), Stop(D)}); auto &db = f.Build();
	PassengerJourneyLimits limits;
	SECTION("Work") { limits.work = 1; }
	SECTION("Zero work") { limits.work = 0; }
	SECTION("Labels") { limits.labels = 1; }
	SECTION("Memory") { limits.memory_bytes = 256; }
	SECTION("Zero memory") { limits.memory_bytes = 0; }
	SECTION("Transfer safety ceiling") { limits.max_transfers = 0; }
	auto cold = db.Plan(PAX, A, D, NOW, limits), warm = db.Plan(PAX, A, D, NOW, limits);
	CHECK_FALSE(cold.ride); CHECK(cold.deferred); CHECK(cold.reason != PassengerPeriodicPlan::Reason::NoCandidate);
	CHECK(cold.reason == warm.reason); CHECK(cold.work == warm.work); CHECK(cold.work <= limits.work);
	CHECK(cold.memory <= limits.memory_bytes); CHECK(cold.labels <= limits.labels);
}

TEST_CASE("Periodic candidate capacity is observed without certifying a false absence", "[passenger][periodic]")
{
	Fixture f; for (int i = 0; i < 12; ++i) f.Train({Stop(A), Stop(D)}); auto &db = f.Build();
	PassengerJourneyLimits limits; limits.candidates = 8;
	auto plan = db.Plan(PAX, A, D, NOW, limits); REQUIRE(plan.ride); CHECK_FALSE(plan.deferred);
	CHECK(plan.candidates == 8); CHECK(plan.observations[static_cast<size_t>(PassengerPeriodicPlan::Reason::Candidates)]);
}

TEST_CASE("Periodic overdue loading aligns with snapshot remaining dwell rather than another circuit", "[passenger][periodic][comparison]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); train->current_order_time = 40; train->lateness_counter = 500;
	auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW);
	REQUIRE(plan.ride); CHECK(plan.ride->departure == NOW); CHECK(plan.arrival == NOW + 100);

}

TEST_CASE("Periodic capture supports an actual waypoint phase without inventing overdue travel", "[passenger][periodic]")
{
	Fixture f; Order waypoint; waypoint.MakeGoToWaypoint(StationID{5}); waypoint.SetTravelTime(40); waypoint.SetTravelTimetabled(true);
	auto *train = f.Train({Stop(A, 100, 0), waypoint, Stop(D, 60, 0)});
	train->cur_real_order_index = train->cur_implicit_order_index = 1; train->current_order = waypoint;
	train->current_order_time = 10;
	SECTION("Known current travel") {
		auto &db = f.Build(); auto *service = db.Get(train->orders->index); REQUIRE(service); CHECK(service->vehicles[0].phase_known);
		CHECK(service->vehicles[0].base == NOW - 10);
		auto plan = db.Plan(PAX, D, A, NOW); REQUIRE(plan.ride); CHECK(plan.ride->departure == NOW + 90);
	}
	SECTION("Elapsed travel exceeds declared arrival") {
		train->current_order_time = 41; auto &db = f.Build(); REQUIRE(db.Get(train->orders->index));
		CHECK_FALSE(db.Get(train->orders->index)->vehicles[0].phase_known);
		train->LoadAt(2); ObservePassengerPeriodicVehicle(train);
		_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; db.Tick(); CHECK(db.Get(train->orders->index)->vehicles[0].phase_known);
	}
}

TEST_CASE("Actual explicit arrival recaptures a drifted periodic phase", "[passenger][periodic][determinism]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build();
	auto generation = db.Get(train->orders->index)->generation;
	_state_ticks += 500; train->LoadAt(1); ObservePassengerPeriodicVehicle(train);
	CHECK(db.Get(train->orders->index)->generation == generation); f.PublishUpdates(db);
	CHECK(db.Get(train->orders->index)->generation > generation);
	auto plan = db.Plan(PAX, D, A, _state_ticks); REQUIRE(plan.ride); CHECK(plan.ride->departure == _state_ticks + 10);
}

TEST_CASE("Paired periodic snapshot diagnostic cannot change publication packets or RNG", "[passenger][periodic][comparison][save][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); f.Train({Stop(B), Stop(D)}); auto &db = f.Build();

	auto saved = db.Save(); auto before = db.GetDiagnostic(); auto *packet = f.Packet(); auto mode = packet->GetPassengerRoutingMode();
	GameRandomSeedChecker seeds;
	auto paired = ComparePassengerPlanners(db, PAX, A, D, NOW);
	REQUIRE(paired.periodic.ride); REQUIRE_FALSE(paired.legacy_route.empty()); REQUIRE(paired.legacy_found);
	CHECK(paired.periodic.arrival == paired.legacy_arrival); CHECK(paired.periodic.cost == paired.legacy_cost);
	CHECK(db.Save() == saved); CHECK(db.GetDiagnostic().searches == before.searches);
	CHECK(packet->GetPassengerRoutingMode() == mode); CHECK(_state_ticks == NOW); CHECK(seeds.Check());
}

TEST_CASE("Periodic passenger denominator excludes cargo-only order lists", "[passenger][periodic]")
{
	Fixture f; f.Train({Stop(A), Stop(D)});
	auto *freight = f.Train({Stop(A), Stop(D)}); freight->cargo_type = CargoType{1};
	AutoRestoreBackup<CargoClasses> freight_class{CargoSpec::Get(CargoType{1})->classes, CargoClasses{CargoClass::Bulk}};
	auto *unknown = f.Train({Stop(A), Stop(D)}); unknown->GetOrder(1)->SetTravelTimetabled(false);
	auto &db = f.Build(); auto census = db.GetDiagnostic();
	CHECK(census.orderlists == 3); CHECK(census.passenger_orderlists == 2); CHECK(census.complete_timetables == 1);
	CHECK(census.declared_complete_timetables == 1); CHECK(census.published_passenger == 2); CHECK(census.compile_success == 1); CHECK(census.compile_failure == 1);
	CHECK(census.compile_failures[static_cast<size_t>(PassengerPeriodicTimetable::Failure::Travel)] == 1);
}
TEST_CASE("A tick ending during facility capture retries before publishing Unknown", "[passenger][periodic][determinism]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = GetPassengerServiceDatabase();
	const size_t task_work = 64 + 2 * (NUM_CARGO + 32) + 3 * NUM_CARGO + 32 + 4;
	db.Tick(task_work); CHECK(db.Get(train->orders->index) == nullptr);
	CHECK(db.GetPublication(train->orders->index) == PassengerServiceDatabase::Publication::Dirty);
	auto wire = db.Save(); PassengerServiceDatabase joined; REQUIRE(joined.Load(wire)); joined.FinishLoad(true);
	f.Build(); for (int i = 0; i < 128 && joined.IsBuilding(); ++i) joined.Tick(); CHECK(db.Save() == joined.Save());
	auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK_FALSE(plan.deferred);
	CHECK(db.GetDiagnostic().rejected_work == 1);
}

TEST_CASE("Periodic search obeys load unload and mandatory interchange permissions", "[passenger][periodic]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(B), Stop(D)});
	SECTION("No load") {
		train->GetOrder(0)->SetLoadType(OrderLoadType::NoLoad); auto &db = f.Build();
		auto plan = db.Plan(PAX, A, D, NOW); CHECK_FALSE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.reason == PassengerPeriodicPlan::Reason::NoCandidate);
	}
	SECTION("No unloading destination") {
		train->GetOrder(2)->SetUnloadType(OrderUnloadType::NoUnload); auto &db = f.Build();
		auto plan = db.Plan(PAX, A, D, NOW); CHECK_FALSE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.reason == PassengerPeriodicPlan::Reason::NoAlighting);
	}
	SECTION("Forced transfer to an express") {
		train->GetOrder(1)->SetUnloadType(OrderUnloadType::Transfer); f.Train({Stop(B, 500, 120), Stop(D, 10, 0)});
		auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK_FALSE(plan.deferred);
		CHECK(plan.transfers == 1); CHECK(plan.ride->alighting == B);
	}
}
TEST_CASE("A phase-only interchange gap is distinct from disconnected published components", "[passenger][periodic]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); auto *unknown = f.Train({Stop(B), Stop(C)}); unknown->current_order.Free();
	f.Train({Stop(C), Stop(D)}); auto &db = f.Build();
	auto plan = db.Plan(PAX, A, D, NOW); CHECK_FALSE(plan.ride); CHECK_FALSE(plan.deferred);
	CHECK(plan.reason == PassengerPeriodicPlan::Reason::UnknownPhase);
	unknown->LoadAt(0); ObservePassengerPeriodicVehicle(unknown);
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; db.Tick();
	plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK_FALSE(plan.deferred);
}
TEST_CASE("Periodic journeys can use more than the initial four transfer rounds", "[passenger][periodic]")
{
	Fixture f;
	for (uint16_t i = 0; i < 12; ++i) f.Train({Stop(StationID{i}, 1000, i * 10), Stop(StationID{static_cast<uint16_t>(i + 1)}, 10, 0)});
	auto &db = f.Build(); auto plan = db.Plan(PAX, A, StationID{12}, NOW);
	REQUIRE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.transfers == 11); CHECK(plan.arrival == NOW + 120);
}

TEST_CASE("Arrival phase comparison normalizes cycles across negative state ticks", "[passenger][periodic][save][determinism]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto generation = db.Get(train->orders->index)->generation;
	StateTicksDelta delta{-64100}; db.ShiftTicks(delta); _state_ticks += delta;
	_state_ticks += 220; train->LoadAt(0); ObservePassengerPeriodicVehicle(train);
	REQUIRE(db.Get(train->orders->index)); CHECK(db.Get(train->orders->index)->generation == generation);
	auto plan = db.Plan(PAX, A, D, _state_ticks); REQUIRE(plan.ride); CHECK(plan.ride->departure == _state_ticks + 10);
}
TEST_CASE("A new periodic query can board a live vehicle overdue since phase publication", "[passenger][periodic][comparison][determinism]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto saved = db.Save();
	_state_ticks += 40; train->current_order_time = 40; train->lateness_counter = 400;
	auto current = db.Plan(PAX, A, D, _state_ticks); REQUIRE(current.ride); CHECK_FALSE(current.deferred);
	CHECK(current.ride->departure == _state_ticks); CHECK(current.arrival == _state_ticks + 100);
	CHECK(db.Save() == saved);
	auto future = db.Plan(PAX, A, D, _state_ticks + 1); REQUIRE(future.ride); CHECK(future.ride->departure == _state_ticks + 220);
	auto paired = ComparePassengerPlanners(db, PAX, A, D, _state_ticks);
	REQUIRE(paired.legacy_found); CHECK(paired.periodic.arrival == paired.legacy_arrival);
}

TEST_CASE("A slower prefix survives when a common connection restores the canonical tie", "[passenger][periodic]")
{
	Fixture f;
	f.Train({Stop(A, 900, 10), Stop(B, 40, 0)});
	auto *local = f.Train({Stop(A, 900, 0), Stop(B, 60, 0)});
	f.Train({Stop(B, 900, 70), Stop(D, 30, 0)});
	auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW);
	REQUIRE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.arrival == NOW + 100);
	CHECK(plan.ride->service == local->orders->index); CHECK(plan.ride->departure == NOW);
}

TEST_CASE("A fleet larger than a full phase capture tick cannot stall publication", "[passenger][periodic][scaling][determinism]")
{
	Fixture f; std::vector<Order> orders;
	for (uint16_t i = 0; i < 100; ++i) orders.push_back(Stop(StationID{i}));
	auto *first = f.Train(orders);
	for (int i = 1; i < 150; ++i) Vehicle::Create<PeriodicTrain>(first);
	auto &db = f.Build(); const auto *service = db.Get(first->orders->index); REQUIRE(service);
	CHECK(service->vehicles.size() == 150);
	size_t known = std::count_if(service->vehicles.begin(), service->vehicles.end(), [](const auto &v) { return v.phase_known; });
	CHECK(known > 0); CHECK(known < 150); CHECK(db.GetDiagnostic().phase_capture_work > 0);
	CHECK(db.GetPublication(first->orders->index) == PassengerServiceDatabase::Publication::Published);
	db.Tick(); CHECK_FALSE(db.IsBuilding());
	auto plan = db.Plan(PAX, A, StationID{99}, NOW); REQUIRE(plan.ride); CHECK_FALSE(plan.deferred);
}


TEST_CASE("Shared policies remain resident through thousands of unchanged ticks", "[passenger][periodic][schedule][residency][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); GameRandomSeedChecker checker;
	auto timed = db.Plan(PAX, A, D, NOW), warm = db.Plan(PAX, A, D, NOW);
	auto estimated = db.PlanEstimated(PAX, A, D, NOW);
	auto saved = db.Save(); auto builds = db.GetDiagnostic().builds;
	for (int i = 0; i < 12000; ++i) { ++_state_ticks; db.Tick(); }
	CHECK(db.GetDiagnostic().builds == builds); CHECK(db.Save() == saved);
	CHECK(db.Plan(PAX, A, D, NOW).work == timed.work);
	CHECK(db.PlanEstimated(PAX, A, D, NOW).work == estimated.work);
	CHECK(db.GetDiagnostic().policy_hits == 2); CHECK(db.GetScheduleDiagnostic().hits == 1);
	CHECK(db.GetDiagnostic().policy_clears == 0); CHECK(timed.ride == warm.ride); CHECK(checker.Check());
}

TEST_CASE("An unrelated publication preserves both policy kinds and their logical work", "[passenger][periodic][schedule][residency][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); auto *unrelated = f.Train({Stop(C), Stop(D)}); auto &db = f.Build();
	auto timed = db.Plan(PAX, A, B, NOW); auto estimated = db.PlanEstimated(PAX, A, B, NOW); auto revision = db.GetDiagnostic().published_revision;
	unrelated->GetOrder(1)->SetTravelTime(500); db.TimetableChanged(unrelated->orders->index); f.PublishUpdates(db);
	CHECK(db.GetDiagnostic().published_revision > revision);
	auto hit = db.Plan(PAX, A, B, NOW); CHECK(hit.work == timed.work); REQUIRE(hit.ride); CHECK(hit.ride->service == timed.ride->service); CHECK(hit.ride->generation == timed.ride->generation);
	auto scheduled_hit = db.PlanEstimated(PAX, A, B, NOW); CHECK(scheduled_hit.work == estimated.work); CHECK(scheduled_hit.ride == estimated.ride);
	CHECK(db.GetDiagnostic().policy_hits == 1); CHECK(db.GetScheduleDiagnostic().hits == 1); CHECK(db.GetDiagnostic().policy_stale == 0);
}

TEST_CASE("A new shortcut invalidates policies which never used its service", "[passenger][periodic][schedule][residency][determinism]")
{
	Fixture f; f.Train({Stop(A, 400, 0), Stop(B, 100, 0)}); f.Train({Stop(B, 400, 100), Stop(D, 100, 0)}); auto &db = f.Build();
	StateTicks ready = NOW + 2 * PassengerServiceDatabase::TIMETABLE_EPOCH;
	auto before = db.Plan(PAX, A, D, ready); auto scheduled_before = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(before.ride); REQUIRE(scheduled_before.ride);
	auto *shortcut = f.Train({Stop(A, 100, 0), Stop(D, 1, 0)}); db.Tick();
	CHECK(db.Plan(PAX, A, D, ready).ride == before.ride); f.PublishUpdates(db);
	auto after = db.Plan(PAX, A, D, ready); auto scheduled_after = db.PlanEstimated(PAX, A, D, NOW);
	REQUIRE(after.ride); REQUIRE(scheduled_after.ride); CHECK(after.ride->service == shortcut->orders->index);
	CHECK(after.arrival < before.arrival); CHECK(scheduled_after.cost < scheduled_before.cost); CHECK(db.GetDiagnostic().policy_stale == 2);
	db.ClearPolicies(); auto cold = db.Plan(PAX, A, D, ready); auto schedule_cold = db.PlanEstimated(PAX, A, D, NOW);
	CHECK(after.ride == cold.ride); CHECK(after.work == cold.work); CHECK(schedule_cold.ride == scheduled_after.ride); CHECK(schedule_cold.work == scheduled_after.work);
}

TEST_CASE("A frozen publication batch survives additional edits and a cold joining peer", "[passenger][periodic][schedule][residency][save][determinism]")
{
	Fixture f; auto *one = f.Train({Stop(A), Stop(B)}); auto *two = f.Train({Stop(C), Stop(D)}); auto &db = f.Build();
	auto generation = db.Get(one->orders->index)->generation;
	one->GetOrder(1)->SetTravelTime(200); two->GetOrder(1)->SetTravelTime(300);
	db.TimetableChanged(one->orders->index); db.TimetableChanged(two->orders->index);
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; db.Tick(500);
	CHECK(db.Get(one->orders->index)->generation == generation); REQUIRE(db.GetDiagnostic().staged == 1);
	one->GetOrder(1)->SetTravelTime(400); db.TimetableChanged(one->orders->index);
	auto wire = db.Save(); PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true); CHECK(peer.Save() == wire);
	for (int i = 0; i < 8; ++i) { ++_state_ticks; db.Tick(500); peer.Tick(500); CHECK(db.Save() == peer.Save()); }
	CHECK(db.Get(one->orders->index)->timetable.period == 320); CHECK(db.GetPublication(one->orders->index) == PassengerServiceDatabase::Publication::TimingDirty);
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH;
	for (int i = 0; i < 8; ++i) { db.Tick(500); peer.Tick(500); CHECK(db.Save() == peer.Save()); }
	CHECK(db.Get(one->orders->index)->timetable.period == 520);
	auto hot = db.Plan(PAX, A, B, NOW); auto cold = peer.Plan(PAX, A, B, NOW); CHECK(hot.ride == cold.ride); CHECK(hot.work == cold.work);
}

TEST_CASE("Shared LRU evicts across Timed and Schedule-aware without changing routes", "[passenger][periodic][schedule][residency][scaling][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto timed = db.Plan(PAX, A, D, NOW); auto schedule = db.PlanEstimated(PAX, A, D, NOW);
	std::vector<Order> stations; for (uint16_t i = 4; i < 5004; ++i) stations.push_back(Stop(StationID{i})); EnsurePassengerTestStations(stations);
	for (uint16_t i = 4; i < 5004; ++i) {
		if (i % 2) db.Plan(PAX, A, StationID{i}, NOW); else db.PlanEstimated(PAX, A, StationID{i}, NOW);
	}
	auto d = db.GetDiagnostic(); CHECK(d.policy_entries <= PassengerServiceDatabase::POLICY_ENTRIES); CHECK(d.policy_bytes <= PassengerServiceDatabase::POLICY_BYTES); CHECK(d.policy_evictions > 0);
	CHECK(db.Plan(PAX, A, D, NOW).ride == timed.ride); CHECK(db.Plan(PAX, A, D, NOW).work == timed.work);
	CHECK(db.PlanEstimated(PAX, A, D, NOW).ride == schedule.ride); CHECK(db.PlanEstimated(PAX, A, D, NOW).work == schedule.work);
	WARN("Shared LRU: evictions=" << d.policy_evictions << "; eviction_us=" << d.policy_eviction_microseconds << "; validation_us=" << d.policy_validation_microseconds << "; entries=" << d.policy_entries << "; bytes=" << d.policy_bytes);
}

TEST_CASE("Structural edits block real boarding while the old publication remains readable", "[passenger][periodic][schedule][residency][active]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride);
	auto generation = db.Get(v->orders->index)->generation;
	v->GetOrder(0)->SetLoadType(OrderLoadType::NoLoad); db.Invalidate(v->orders->index);
	CHECK(db.Get(v->orders->index)->generation == generation); CHECK_FALSE(db.IsCurrent(*plan.ride)); CHECK_FALSE(ValidatePassengerServiceRide(*plan.ride, v, PAX, true));
	db.Tick(); CHECK(db.Get(v->orders->index)->generation == generation); f.PublishUpdates(db);
	CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride); CHECK_FALSE(db.PlanEstimated(PAX, A, D, NOW).ride);
}


TEST_CASE("Disabling shared residency preserves work outcomes and saved publication", "[passenger][periodic][schedule][residency][save][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(B), Stop(D)}); f.Train({Stop(B, 80, 0), Stop(D, 10, 0)}); auto &db = f.Build();
	auto wire = db.Save(); PassengerServiceDatabase cold; REQUIRE(cold.Load(wire)); cold.FinishLoad(true); cold.SetPolicyCacheEnabled(false);
	for (size_t work : {size_t{1}, size_t{10}, size_t{100}, size_t{65536}}) for (int i = 0; i < 64; ++i) {
		PassengerJourneyLimits limits; limits.work = work; StateTicks ready = NOW + i * 17;
		auto timed = db.Plan(PAX, A, D, ready, limits); auto uncached = cold.Plan(PAX, A, D, ready, limits);
		CHECK(timed.ride == uncached.ride); CHECK(timed.route == uncached.route); CHECK(timed.work == uncached.work); CHECK(timed.reason == uncached.reason); CHECK(timed.deferred == uncached.deferred); CHECK(timed.memory == uncached.memory);
		auto schedule = db.PlanEstimated(PAX, A, D, ready, limits); auto fresh = cold.PlanEstimated(PAX, A, D, ready, limits);
		CHECK(schedule.ride == fresh.ride); CHECK(schedule.work == fresh.work); CHECK(schedule.cost == fresh.cost); CHECK(schedule.deferred == fresh.deferred);
	}
	CHECK(db.Save() == wire); CHECK(cold.Save() == wire); CHECK(cold.GetDiagnostic().policy_entries == 0);
}


TEST_CASE("Deleted station and vehicle IDs cannot board through a resident old publication", "[passenger][periodic][schedule][residency][active][save]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto timed = db.Plan(PAX, A, D, NOW); auto schedule = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(timed.ride); REQUIRE(schedule.ride);
	SECTION("Station pool removal") {
		db.Invalidate(); _station_pool.CleanPool();
		CHECK_FALSE(ValidatePassengerServiceRide(*timed.ride, v, PAX, true)); CHECK_FALSE(ValidatePassengerServiceRide(*schedule.ride, v, PAX, true));
		CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride); CHECK_FALSE(db.PlanEstimated(PAX, A, D, NOW).ride);
	}
	SECTION("Vehicle and order list deletion") {
		VehicleID id = v->index; OrderListID service = v->orders->index;
		db.Invalidate(service); v->orders->FreeChain(); v->orders = nullptr; delete v;
		CHECK(Vehicle::GetIfValid(id) == nullptr); CHECK_FALSE(db.IsCurrent(*timed.ride)); CHECK_FALSE(db.IsCurrent(*schedule.ride));
		CHECK_FALSE(ValidatePassengerServiceRide(*timed.ride, Vehicle::GetIfValid(id), PAX, true));
		/* Planning and serialization use copied IDs and offsets, never freed pointers. */
		db.Plan(PAX, A, D, NOW); db.PlanEstimated(PAX, A, D, NOW); auto wire = db.Save(); PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
		f.PublishUpdates(db); _state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; for (int i = 0; i < 128 && peer.IsBuilding(); ++i) peer.Tick();
		CHECK(db.Get(service) == nullptr); CHECK(peer.Get(service) == nullptr); CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride); CHECK_FALSE(db.PlanEstimated(PAX, A, D, NOW).ride);
	}
}


TEST_CASE("A local disabled policy cache stays disabled across loading and joining", "[passenger][periodic][schedule][residency][save][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto wire = db.Save();
	db.SetPolicyCacheEnabled(false); REQUIRE(db.Load(wire)); db.FinishLoad(true);
	for (int i = 0; i < 3; ++i) { REQUIRE(db.Plan(PAX, A, D, NOW).ride); REQUIRE(db.PlanEstimated(PAX, A, D, NOW).ride); }
	CHECK(db.GetDiagnostic().policy_entries == 0); CHECK(db.GetDiagnostic().policy_hits == 0); CHECK(db.GetScheduleDiagnostic().hits == 0); CHECK(db.Save() == wire);
}


TEST_CASE("A staging memory boundary remains bounded and loadable during deletions", "[passenger][periodic][residency][save][scaling][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto wire = db.Save();
	wire.resize(wire.size() - 7 * 8); // Replace the empty version-three staging suffix.
	auto put = [&](uint64_t value) { for (uint i = 0; i < 8; ++i) { wire.push_back(value & 255); value >>= 8; } };
	put(1); put(NOW.base()); put(0); put(0); // Published revision, batch tick, publishing, bootstrap.
	put(3); for (uint16_t id : {60000, 60001, 60002}) { put(id); put(NOW.base()); }
	put(0); put(32); // Unsafe set and staged records.
	for (uint16_t index = 0; index < 32; ++index) {
		put(1000 + index); put(1); put(1); put(1);
		size_t stops = index == 31 ? 4062 : 4096; put(stops);
		for (size_t i = 0; i < stops; ++i) { put(A.base()); put(0); put(0); put(0); put(1); put(1); put(0); }
		put(0); put(0); put(0); put(0); put(0); put(0); put(0); put(NOW.base()); // Fleet, dispatch fields, estimate and staged age.
	}
	REQUIRE(db.Load(wire)); db.FinishLoad(true);
	REQUIRE(db.GetDiagnostic().staged_bytes == PassengerServiceDatabase::MAX_BYTES - 1024);
	db.Tick(); CHECK(db.GetDiagnostic().staged_bytes <= PassengerServiceDatabase::MAX_BYTES);
	auto partial = db.Save(); PassengerServiceDatabase peer; REQUIRE(peer.Load(partial)); peer.FinishLoad(true); CHECK(peer.Save() == partial);
	for (int i = 0; i < 16 && db.IsBuilding(); ++i) { db.Tick(); peer.Tick(); CHECK(db.Save() == peer.Save()); CHECK(db.GetDiagnostic().staged_bytes <= PassengerServiceDatabase::MAX_BYTES); }
	CHECK_FALSE(db.IsBuilding()); CHECK_FALSE(peer.IsBuilding());
}
