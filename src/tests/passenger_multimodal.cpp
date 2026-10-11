/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
/** @file passenger_multimodal.cpp Real vehicle adapters, mixed journeys and cargo execution regressions. */
#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../passenger_journey_active.h"
#include "../passenger_journey_shadow.h"
#include "../train.h"
#include "../roadveh.h"
#include "../ship.h"
#include "../aircraft.h"
#include "../airport.h"
#include "../roadstop_base.h"
#include "../station_base.h"
#include "../economy_base.h"
#include "../company_base.h"
#include "../town.h"
#include "../date_func.h"
#include "../core/backup_type.hpp"
#include "../core/random_func.hpp"
#include "../sl/saveload.h"
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
		AirportSpec::ResetAirports();
		_settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Asymmetric;
		_settings_game.linkgraph.passenger_journey_routing = true;
		_settings_game.linkgraph.passenger_transfer_timetables = false;
		_settings_game.linkgraph.passenger_transfer_penalty = 20;
		for (StationID id : {A, B, C, D, E}) this->StationAt(id);
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
	SECTION("Drive-through pass-through state is not loading") { road->state = RVSB_IN_DT_ROAD_STOP; CHECK_FALSE(GetPassengerRouteState(road)); return; }
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
	SECTION("Implicit order remains unknown") { v->GetOrder(1)->MakeImplicit(E); }
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
	CHECK(f.Waiting().AvailableViaCount(E) == 10);
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
	SECTION("Current execution") {}
	SECTION("Saved fallback and cooldown") { packet->FallbackPassengerJourney(A, NOW); expected = PassengerRoutingMode::Fallback; }
	SECTION("Old save without execution fields") { _sl_xv_feature_versions[XSLFI_PASSENGER_JOURNEY_ROUTING] = 0; expected = PassengerRoutingMode::Legacy; }
	SECTION("Invalid facilities afterload") { Station::Get(E)->facilities = {}; Station::Get(E)->bus_stops = nullptr; expected = PassengerRoutingMode::Fallback; }
	auto *loaded = CargoPacket::Create();
	CHECK_FALSE(SlTestSaveLoadTable(packet, loaded, GetCargoPacketDesc()).empty());
	f.Waiting().Append(loaded, StationID::Invalid());
	GameRandomSeedChecker random;
	AfterLoadPassengerJourneys();
	CHECK(loaded->GetPassengerRoutingMode() == expected); CHECK(loaded->GetPassengerDestination() == E);
	CHECK(loaded->Count() == 10); CHECK(loaded->GetFeederShare() == 100);
	if (expected == PassengerRoutingMode::Journey) {
		CHECK(loaded->GetPassengerJourney() == leg);
		CHECK(PassengerJourneyBoardingContext(v).CanBoard(*loaded));
		CHECK(InvalidatePassengerJourneyVehicle(v->index) >= 1); // Rebuilt reverse index contains restored packets.
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
