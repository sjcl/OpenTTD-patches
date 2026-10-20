/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */

/** @file station_passenger_route_gui.cpp Waiting passenger tooltip regression tests. */
#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../station_passenger_route_gui.h"
#include "../passenger_service_database.h"
#include "../passenger_journey_active.h"
#include "../cargopacket.h"
#include "../vehicle_base.h"
#include "../group.h"
#include "../date_func.h"
#include "../settings_type.h"
#include "../core/backup_type.hpp"
#include "../core/random_func.hpp"
#include "../sl/saveload.h"
#include "../openttd.h"
#include "passenger_test_stations.h"

extern std::vector<uint8_t> SlTestSaveLoadTable(void *, void *, const NamedSaveLoadTable &);

namespace {
constexpr CargoType PAX{0};
constexpr StationID A{0}, B{1}, C{2}, D{3};
constexpr StateTicks NOW{64000};

Order RouteStop(StationID station)
{
	Order order; order.MakeGoToStation(station);
	order.SetTravelTime(100); order.SetTravelTimetabled(true);
	order.SetWaitTime(10); order.SetWaitTimetabled(true);
	return order;
}

struct RouteTrain : Vehicle {
	RouteTrain(VehicleID id, std::vector<Order> stops) : Vehicle(id, VehicleType::Train)
	{
		this->cargo_type = PAX; this->cargo_cap = 100;
		this->orders = OrderList::Create(std::move(stops), this);
		EnsurePassengerTestStations(this->orders->GetOrderVector());
		this->direction = Direction::NE;
		this->cur_real_order_index = this->cur_implicit_order_index = 0;
		this->last_station_visited = this->GetOrder(0)->GetDestination().ToStationID();
		this->current_order.MakeLoading(false);
		Station::Get(this->last_station_visited)->loading_vehicles.push_back(this);
	}
	~RouteTrain() { for (Station *station : Station::Iterate()) std::erase(station->loading_vehicles, this); }
	bool IsPrimaryVehicle() const override { return true; }
};

struct RouteFixture : PassengerTestStations {
	AutoRestoreBackup<LinkGraphSettings> settings{_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<StateTicks> ticks{_state_ticks, NOW};
	AutoRestoreBackup<uint8_t> bitnum{CargoSpec::Get(PAX)->bitnum, uint8_t{0}};
	RouteFixture()
	{
		_settings_game.linkgraph.passenger_journey_routing = _settings_game.linkgraph.passenger_journey_periodic = true;
		_settings_game.linkgraph.passenger_journey_precomputed = true;
		_settings_game.linkgraph.passenger_journey_hyperpath = true;
		_settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Asymmetric;
		_settings_game.linkgraph.passenger_transfer_penalty = 0;
		ResetPassengerServiceDatabase(); BeginPassengerJourneyTick();
	}
	~RouteFixture()
	{
		_vehicle_pool.CleanPool(); _orderlist_pool.CleanPool(); _order_pool.CleanPool(); _group_pool.CleanPool();
		_station_pool.CleanPool(); _cargopacket_pool.CleanPool(); ClearCargoPacketDeferredPayments(); ResetPassengerServiceDatabase();
	}
	RouteTrain *Train(std::initializer_list<StationID> stops)
	{
		std::vector<Order> orders;
		for (auto station : stops) orders.push_back(RouteStop(station));
		return Vehicle::Create<RouteTrain>(std::move(orders));
	}
	PassengerServiceDatabase &Build(bool prepare = true)
	{
		auto &db = GetPassengerServiceDatabase();
		for (size_t i = 0; db.IsBuilding() && i < 128; ++i) db.Tick();
		REQUIRE_FALSE(db.IsBuilding());
		if (prepare) { size_t slices = 0; while (!db.PreparePrecomputed()) REQUIRE(++slices < 10000); }
		return db;
	}
	PassengerJourneyExecution Ride(const RouteTrain &train, StationID board, StationID alight)
	{
		const auto *service = GetPassengerServiceDatabase().Get(train.orders->index); REQUIRE(service != nullptr);
		PassengerJourneyExecution ride; ride.kind = PassengerRideKind::EstimatedServiceIntent;
		ride.service = train.orders->index; ride.signature = service->incarnation; ride.generation = service->generation;
		ride.boarding = board; ride.alighting = alight;
		for (VehicleOrderID i = 0; i < train.orders->GetNumOrders(); ++i) {
			if (train.GetOrder(i)->GetDestination() == board) ride.boarding_order = i;
			if (train.GetOrder(i)->GetDestination() == alight) ride.alighting_order = i;
		}
		return ride;
	}
	StationPassengerRouteSamples Sample(const PassengerJourneyExecution &ride, uint count = 10)
	{
		return {{{PassengerRoutingMode::Journey, ride, OrderListID::Invalid(), OrderListID::Invalid(), count}}};
	}
	StationPassengerRouteExamples Estimate(const StationPassengerRouteSamples &samples, StationID destination = D, size_t budget = 8192)
	{
		return EstimateStationPassengerRoutes(PAX, A, destination, samples, GetPassengerServiceDatabase(), budget);
	}
};
}

TEST_CASE("Station route tooltip selects only typed waiting destination rows at the scrolled offset", "[passenger][station-gui][route-tooltip]")
{
	RouteFixture f; f.Train({A, D});
	std::vector<StationCargoKey> path{PAX, StationPassengerService{}, D};
	REQUIRE(GetStationPassengerRouteTarget(true, path));
	CHECK_FALSE(GetStationPassengerRouteTarget(false, path));
	path.push_back(A); CHECK_FALSE(GetStationPassengerRouteTarget(true, path));
	path.pop_back(); path.pop_back(); CHECK_FALSE(GetStationPassengerRouteTarget(true, path));
	path = {PAX, A, D}; CHECK_FALSE(GetStationPassengerRouteTarget(true, path));
	path = {CargoType{1}, StationPassengerService{}, D}; CHECK_FALSE(GetStationPassengerRouteTarget(true, path));
	path = {PAX, StationPassengerService{}, StationID::Invalid()}; CHECK_FALSE(GetStationPassengerRouteTarget(true, path));
	CHECK(GetStationPassengerRouteRow(125, 123, 4) == 2);
	CHECK_FALSE(GetStationPassengerRouteRow(122, 123, 4));
	CHECK_FALSE(GetStationPassengerRouteRow(127, 123, 4));
	CHECK_FALSE(GetStationPassengerRouteRow(INT_MAX, 123, 4));
	CHECK_FALSE(GetStationPassengerRouteRow(0, 0, 0));
}

TEST_CASE("Station route tooltip respects concrete rides and shows only transfers", "[passenger][station-gui][route-tooltip]")
{
	RouteFixture f; auto *train = f.Train({A, B, D}); auto &db = f.Build();
	SECTION("Direct rides do not enumerate through stops") {
		for (auto kind : {PassengerRideKind::ServiceIntent, PassengerRideKind::EstimatedServiceIntent, PassengerRideKind::VehicleSnapshot}) {
			auto ride = f.Ride(*train, A, D); ride.kind = kind;
			if (kind == PassengerRideKind::VehicleSnapshot) { ride.vehicle = train->index; ride.signature = PassengerJourneyServiceSignature(*train->orders, PAX); }
			auto routes = f.Estimate(f.Sample(ride));
			REQUIRE(routes.routes.size() == 1); CHECK(routes.routes[0].complete);
			REQUIRE(routes.routes[0].legs.size() == 1); CHECK(routes.routes[0].legs[0].alighting == D);
		}
	}
	SECTION("The saved alighting precedes today's direct policy") {
		auto routes = f.Estimate(f.Sample(f.Ride(*train, A, B)));
		REQUIRE(routes.routes.size() == 1); CHECK(routes.routes[0].legs[0].alighting == B);
		CHECK_FALSE(routes.routes[0].complete); // Cannot reboard the incoming service at B.
	}
	SECTION("Stale service identity cannot invent a replacement first leg") {
		auto ride = f.Ride(*train, A, D); ++ride.signature;
		auto routes = f.Estimate(f.Sample(ride)); CHECK(routes.routes.empty()); CHECK(routes.unknown);
	}
	CHECK(db.IsCurrent(f.Ride(*train, A, D)));
}

TEST_CASE("Station route tooltip follows multiple Hyperpath transfers without treating intent destination as a ride", "[passenger][hyperpath][station-gui][route-tooltip]")
{
	RouteFixture f; f.Train({A, B}); f.Train({B, D}); f.Train({A, C}); f.Train({C, D});
	auto &db = f.Build(); auto intent = db.PlanPrecomputed(PAX, A, D, NOW); REQUIRE(intent.ride);
	REQUIRE(intent.ride->kind == PassengerRideKind::HyperpathIntent); CHECK(intent.ride->alighting == D);
	auto routes = f.Estimate(f.Sample(*intent.ride));
	REQUIRE(routes.routes.size() == 2);
	for (const auto &route : routes.routes) {
		CHECK(route.complete); REQUIRE(route.legs.size() == 2);
		CHECK((route.legs[0].alighting == B || route.legs[0].alighting == C));
		CHECK(route.legs[1].boarding == route.legs[0].alighting); CHECK(route.legs[1].alighting == D);
	}
}

TEST_CASE("Station route tooltip keeps concrete prefixes on unknown suffix and GUI Work exhaustion", "[passenger][station-gui][route-tooltip]")
{
	RouteFixture f; auto *train = f.Train({A, B}); f.Train({C, D}); f.Build(false);
	auto sample = f.Sample(f.Ride(*train, A, B));
	auto routes = f.Estimate(sample); REQUIRE(routes.routes.size() == 1);
	CHECK_FALSE(routes.routes[0].complete); CHECK(routes.routes[0].legs.back().alighting == B);
	routes = f.Estimate(sample, D, 0); CHECK(routes.routes.empty()); CHECK(routes.unknown);
	for (size_t allowance : {size_t{1}, size_t{4}, size_t{16}}) {
		size_t work = allowance; routes = EstimateStationPassengerRoutes(PAX, A, D, sample, GetPassengerServiceDatabase(), work);
		CHECK(work <= allowance);
		for (const auto &route : routes.routes) CHECK_FALSE(route.complete);
	}
}

TEST_CASE("Station route tooltip deduplicates mixed cohorts and caps route examples", "[passenger][station-gui][route-tooltip]")
{
	RouteFixture f; auto *first = f.Train({A, D}); auto *second = f.Train({A, B, D});
	auto *third = f.Train({A, C, D}); auto *fourth = f.Train({A, B, C, D}); f.Build();
	auto samples = f.Sample(f.Ride(*first, A, D), 100);
	for (auto *train : {first, second, third, fourth}) samples.packets.push_back(f.Sample(f.Ride(*train, A, D)).packets[0]);
	samples.multiple = true;
	auto routes = f.Estimate(samples); REQUIRE(routes.routes.size() == 3); CHECK(routes.more); CHECK(routes.multiple);
	CHECK(routes.routes.front().legs[0].service == first->orders->index);
	auto stale = f.Sample(f.Ride(*first, A, D)).packets[0]; ++stale.ride.signature; samples.packets.push_back(stale);
	CHECK(f.Estimate(samples).unknown);
}

TEST_CASE("Station route tooltip respects retained generations and refuses retired intents", "[passenger][hyperpath][station-gui][route-tooltip]")
{
	RouteFixture f; f.Train({A, B}); f.Train({B, D}); auto &db = f.Build();
	auto plan = db.PlanPrecomputed(PAX, A, D, NOW); REQUIRE(plan.ride); auto sample = f.Sample(*plan.ride);
	_settings_game.linkgraph.passenger_transfer_penalty = 17; db.TickPrecomputed(1);
	REQUIRE(db.IsCurrent(*plan.ride)); REQUIRE(db.GetPrecomputedDiagnostic(PAX).retained_destinations > 0);
	REQUIRE_FALSE(f.Estimate(sample).routes.empty());
	while (!db.PrecomputedFinished()) db.TickPrecomputed();
	CHECK_FALSE(db.IsCurrent(*plan.ride)); CHECK(f.Estimate(sample).routes.empty()); CHECK(f.Estimate(sample).unknown);
}

TEST_CASE("Station route tooltip stops after five legs and preserves the known prefix", "[passenger][station-gui][route-tooltip]")
{
	RouteFixture f;
	for (uint16_t i = 0; i < 7; ++i) f.Train({StationID{i}, StationID{uint16_t(i + 1)}});
	auto &db = f.Build(); const StationID destination{7};
	auto plan = db.PlanPrecomputed(PAX, A, destination, NOW); REQUIRE(plan.ride);
	auto routes = f.Estimate(f.Sample(*plan.ride), destination);
	REQUIRE_FALSE(routes.routes.empty());
	for (const auto &route : routes.routes) {
		REQUIRE(route.legs.size() == StationPassengerRouteExamples::MAX_LEGS);
		CHECK_FALSE(route.complete); CHECK(route.legs.back().alighting != destination);
	}
}

TEST_CASE("Station route tooltip collection bounds work and rechecks live row membership", "[passenger][station-gui][route-tooltip]")
{
	RouteFixture f; auto *train = f.Train({A, D}); auto *other = f.Train({A, B, D}); f.Build();
	train->group_id = Group::Create(train->owner, train->type)->index; other->group_id = Group::Create(other->owner, other->type)->index;
	StationPassengerServiceCache services;
	auto add = [&](uint count, const PassengerJourneyExecution &ride) {
		auto *packet = CargoPacket::Create(count, 0, A, TileIndex{0}, Money{0}); REQUIRE(packet->SetPassengerDestination(PAX, D));
		packet->SetPassengerJourney(ride, D); Station::Get(A)->goods[PAX].CreateData().cargo.Append(packet, D); return packet;
	};
	add(10, f.Ride(*other, A, D)); auto *packet = add(100, f.Ride(*train, A, D));
	StationPassengerRouteTarget target{PAX, StationPassengerService{train->group_id}, D};
	const auto &station = *Station::Get(A);
	auto samples = CollectStationPassengerRouteSamples(station, target, 100, services);
	REQUIRE(samples.packets.size() == 1); CHECK(samples.packets.front().count == 100); CHECK_FALSE(samples.incomplete);
	auto bounded = CollectStationPassengerRouteSamples(station, target, 100, services, 0);
	CHECK(bounded.packets.empty()); CHECK(bounded.incomplete);
	packet->SetPassengerJourney(f.Ride(*other, A, D), D);
	CHECK(CollectStationPassengerRouteSamples(station, target, 100, services).packets.empty());
	CHECK(CollectStationPassengerRouteSamples(station, target, 100, services).incomplete);
	packet->SetPassengerJourney(f.Ride(*train, A, D), D);
	StationPassengerRouteCache cache;
	REQUIRE_FALSE(cache.Get(station, target, 100, services).routes.empty());
	const auto *cached_routes = cache.Get(station, target, 100, services).routes.data();
	_state_ticks += 1;
	CHECK(cache.Get(station, target, 100, services).routes.data() == cached_routes);
	SECTION("Removed physical carrier invalidates a paused cached route") {
		delete train;
		CHECK(cache.Get(station, target, 100, services).routes.empty());
	}
	SECTION("Packet deletion and pool ID reuse cannot resurrect a route") {
		Station::Get(A)->goods[PAX].data->cargo.Truncate();
		auto *replacement = CargoPacket::Create(100, 0, A, TileIndex{0}, Money{0});
		REQUIRE(replacement->SetPassengerDestination(PAX, D));
		replacement->SetPassengerJourney(f.Ride(*train, A, D), D); // It is outside the waiting queue.
		CHECK(cache.Get(station, target, 100, services).routes.empty());
	}
	SECTION("Deleted groups do not survive a current row recheck") {
		delete Group::Get(train->group_id);
		CHECK(cache.Get(station, target, 100, services).routes.empty());
	}
}

TEST_CASE("Station route tooltip samples prioritize large cohorts within fixed scan and memory bounds", "[passenger][station-gui][route-tooltip]")
{
	RouteFixture f; auto *train = f.Train({A, D}); f.Build();
	train->group_id = Group::Create(train->owner, train->type)->index;
	for (int i = 0; i < 20; ++i) {
		auto ride = f.Ride(*train, A, D); ride.not_before = NOW + i;
		auto *packet = CargoPacket::Create(i == 19 ? 1000 : 1, 0, A, TileIndex{0}, Money{0});
		REQUIRE(packet->SetPassengerDestination(PAX, D)); packet->SetPassengerJourney(ride, D);
		Station::Get(A)->goods[PAX].CreateData().cargo.Append(packet, D);
	}
	StationPassengerServiceCache services;
	StationPassengerRouteTarget target{PAX, StationPassengerService{train->group_id}, D};
	auto samples = CollectStationPassengerRouteSamples(*Station::Get(A), target, 1019, services);
	REQUIRE(samples.packets.size() == StationPassengerRouteSamples::MAX_SAMPLES);
	CHECK(samples.packets.front().count == 1000); CHECK(samples.incomplete); CHECK(samples.multiple);
	auto prefix = CollectStationPassengerRouteSamples(*Station::Get(A), target, 1019, services, 4);
	CHECK(prefix.packets.size() == 4); CHECK(prefix.incomplete);
}

TEST_CASE("Station route tooltip is paused-client-local and leaves saves diagnostics and RNG unchanged", "[passenger][station-gui][route-tooltip][determinism]")
{
	RouteFixture f; auto *one = f.Train({A, D}); auto *two = f.Train({A, D}); auto &db = f.Build();
	AutoRestoreBackup paused{_pause_mode, PauseModes{PauseMode::Normal}};
	auto plan = db.PlanPrecomputed(PAX, A, D, NOW); REQUIRE(plan.ride);
	auto *packet = CargoPacket::Create(100, 0, A, TileIndex{0}, Money{0}); REQUIRE(packet->SetPassengerDestination(PAX, D));
	packet->SetPassengerJourney(*plan.ride, D);
	packet->SetPassengerInterchange(NOW, one->orders->index, one->orders->GetCargoDistRouteGeneration());
	for (int i = 0; i < 3; ++i) packet->RecordPassengerCapacityFailure(two->orders->index, NOW);
	Station::Get(A)->goods[PAX].CreateData().cargo.Append(packet, D);
	StationPassengerServiceCache services; StationPassengerRouteCache cache;
	services.Resolve(PAX, *packet, A); services.Advance(1000);
	StationPassengerRouteTarget target{PAX, PassengerServiceExpansionKey(services.Resolve(PAX, *packet, A)), D};
	const auto saved = db.Save(); const auto diagnostic = db.GetPrecomputedDiagnostic(PAX); const auto tick = _state_ticks;
	auto *copy = CargoPacket::Create();
	const auto packet_wire = SlTestSaveLoadTable(packet, copy, GetCargoPacketDesc());
	REQUIRE_FALSE(packet_wire.empty());
	const auto execution = packet->GetPassengerJourney(); const auto failure = packet->GetPassengerBoardingFailure();
	const auto simulation = GetPassengerJourneyTickDiagnostic();
	const auto interactive = _interactive_random;
	GameRandomSeedChecker seeds;
	for (int i = 0; i < 20; ++i) {
		const auto &routes = cache.Get(*Station::Get(A), target, 100, services);
		REQUIRE_FALSE(routes.routes.empty());
		// The arriving service is excluded; the sole remaining failed service may be retained by the policy.
		for (const auto &route : routes.routes) CHECK(route.legs[0].service != one->orders->index);
	}
	CHECK(db.Save() == saved); CHECK(db.GetPrecomputedDiagnostic(PAX).queries == diagnostic.queries);
	CHECK(SlTestSaveLoadTable(packet, copy, GetCargoPacketDesc()) == packet_wire);
	CHECK(db.GetPrecomputedDiagnostic(PAX).local_replans == diagnostic.local_replans);
	const auto after = GetPassengerJourneyTickDiagnostic();
	CHECK(after.query_work == simulation.query_work); CHECK(after.graph_work == simulation.graph_work);
	CHECK(after.admission.used == simulation.admission.used); CHECK(after.admission.reserved == simulation.admission.reserved);
	CHECK(_interactive_random.state[0] == interactive.state[0]); CHECK(_interactive_random.state[1] == interactive.state[1]);
	CHECK(packet->GetPassengerJourney() == execution); CHECK(packet->GetPassengerBoardingFailure() == failure);
	CHECK(packet->Count() == 100); CHECK(_state_ticks == tick); CHECK(seeds.Check());
}
