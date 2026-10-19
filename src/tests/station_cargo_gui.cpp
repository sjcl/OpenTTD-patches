/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */

/** @file station_cargo_gui.cpp Read-only passenger service/destination/source hierarchy regressions. */
#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../station_cargo_gui.h"
#include "../passenger_journey_active.h"
#include "../passenger_service_database.h"
#include "../core/backup_type.hpp"
#include "../settings_type.h"
#include "../cargopacket.h"
#include "../date_func.h"
#include "../vehicle_base.h"
#include "../group.h"
#include "../core/random_func.hpp"
#include "passenger_test_stations.h"
#include <iostream>

static constexpr CargoType PAX{0};
static constexpr StationID A{0}, B{1}, C{2}, D{3};

static Order GuiStop(StationID station)
{
	Order order;
	order.MakeGoToStation(station);
	return order;
}

/** Minimal real shared-order chain with actual passenger capacity and station facilities. */
struct GuiTrain : Vehicle {
	mutable size_t primary_checks = 0;
	explicit GuiTrain(VehicleID id) : Vehicle(id, VehicleType::Train)
	{
		this->cargo_type = PAX; this->cargo_cap = 100;
		this->orders = OrderList::Create(std::vector<Order>{GuiStop(A), GuiStop(B)}, this);
		EnsurePassengerTestStations(this->orders->GetOrderVector());
	}
	~GuiTrain()
	{
		this->SetNext(nullptr);
		if (this->orders != nullptr) {
			if (this->IsOrderListShared()) {
				OrderList *list = this->orders;
				list->AddVehicle(nullptr); // Suppress the last shared vehicle's GUI notification in the fixture.
				this->RemoveFromShared();
				list->RemoveVehicle(nullptr);
			}
			else this->orders->FreeChain();
			this->orders = nullptr;
		}
	}
	bool IsPrimaryVehicle() const override { ++this->primary_checks; return true; }
	void Share(GuiTrain &other)
	{
		this->orders->FreeChain();
		this->orders = other.orders;
		this->AddToShared(&other);
	}
	PassengerJourneyExecution Ride(PassengerRideKind kind = PassengerRideKind::ServiceIntent) const
	{
		PassengerJourneyExecution ride;
		ride.service = this->orders->index;
		ride.signature = this->orders->GetCargoDistRouteGeneration();
		ride.boarding = A; ride.alighting = B;
		ride.boarding_order = 0; ride.alighting_order = 1;
		ride.kind = kind;
		if (kind == PassengerRideKind::VehicleSnapshot || IsBoundPassengerRide(kind)) ride.vehicle = this->index;
		return ride;
	}
};

struct GuiFixture : PassengerTestStations {
	AutoRestoreBackup<uint8_t> bitnum{CargoSpec::Get(PAX)->bitnum, uint8_t{0}};
	~GuiFixture() { _group_pool.CleanPool(); _cargopacket_pool.CleanPool(); ClearCargoPacketDeferredPayments(); }
	GroupID GroupFor(const Vehicle &vehicle)
	{
		return Group::Create(vehicle.owner, vehicle.type)->index;
	}
	CargoPacket *Packet()
	{
		EnsurePassengerTestStations(std::array{GuiStop(A), GuiStop(B)});
		CargoPacket *packet = CargoPacket::Create(10, 0, A, TileIndex{0}, Money{0});
		REQUIRE(packet->SetPassengerDestination(PAX, B));
		return packet;
	}
};

struct GuiCoach : Vehicle {
	explicit GuiCoach(VehicleID id) : Vehicle(id, VehicleType::Train) { this->cargo_type = PAX; this->cargo_cap = 50; }
	bool IsPrimaryVehicle() const override { return false; }
};

TEST_CASE("Station Journey via uses the alighting station with an invalid queue key", "[passenger][station-gui]")
{
	GuiFixture f; GuiTrain train(VehicleID{100});
	CargoPacket *packet = f.Packet();
	for (auto kind : {PassengerRideKind::VehicleSnapshot, PassengerRideKind::ServiceIntent, PassengerRideKind::EstimatedServiceIntent}) {
		packet->SetPassengerJourney(train.Ride(kind), D);
		CHECK(GetStationCargoVia(PAX, *packet, StationID::Invalid()) == B);
		CHECK(GetStationCargoVia(PAX, *packet, C) == B);
	}
	Station::CreateAtIndex(C, TileIndex{0});
	auto transfer = train.Ride(); transfer.alighting = C;
	packet->SetPassengerJourney(transfer, D);
	CHECK(GetStationCargoVia(PAX, *packet, StationID::Invalid()) == C);
	CHECK(packet->GetPassengerDestination() == B);
}

TEST_CASE("Station Legacy Fallback and non-passenger via preserve queue information", "[passenger][station-gui]")
{
	GuiFixture f; GuiTrain train(VehicleID{100});
	CargoPacket *packet = f.Packet();
	CHECK(GetStationCargoVia(PAX, *packet, B) == B);
	CHECK(GetStationCargoVia(PAX, *packet, StationID::Invalid()) == StationID::Invalid());
	packet->SetPassengerJourney(train.Ride(), B);
	CHECK(GetStationCargoVia(CargoType{1}, *packet, C) == C);
	packet->FallbackPassengerJourney(A, StateTicks{0});
	CHECK(GetStationCargoVia(PAX, *packet, C) == C);
	CHECK(GetStationCargoVia(PAX, *packet, StationID::Invalid()) == StationID::Invalid());
	packet->SetPassengerLegacyHop(B);
	CHECK(GetStationCargoVia(PAX, *packet, StationID::Invalid(), B) == StationID::Invalid());
	CHECK(GetStationCargoVia(PAX, *packet, B, B) == StationID::Invalid());
	packet->SetPassengerVia(B);
	packet->SetPassengerLegacyHop(A);
	GameRandomSeedChecker seeds;
	CHECK(GetStationCargoVia(PAX, *packet, StationID::Invalid(), B) == A);
	CHECK(packet->GetPassengerVia() == B);
	CHECK(packet->GetPassengerLegacyHop() == A);
	CHECK(seeds.Check());
}

TEST_CASE("Station fixed carriers show their own group in a mixed shared fleet", "[passenger][station-gui]")
{
	GuiFixture f;
	GuiTrain *first = Vehicle::Create<GuiTrain>(), *second = Vehicle::Create<GuiTrain>();
	first->group_id = f.GroupFor(*first); second->group_id = f.GroupFor(*second); second->Share(*first);
	StationPassengerServiceCache cache;
	for (auto kind : {PassengerRideKind::VehicleSnapshot, PassengerRideKind::BoundService, PassengerRideKind::EstimatedBoundService}) {
		CHECK(cache.Resolve(PAX, first->Ride(kind)).group == first->group_id);
		CHECK(cache.Resolve(PAX, second->Ride(kind)).group == second->group_id);
	}
	delete second; delete first;
}

TEST_CASE("Station service intentions distinguish unique and ambiguous shared groups", "[passenger][station-gui]")
{
	GuiFixture f; GuiTrain first(VehicleID{100}), second(VehicleID{101}); second.Share(first);
	first.group_id = second.group_id = f.GroupFor(first);
	StationPassengerServiceCache cache;
	for (auto kind : {PassengerRideKind::ServiceIntent, PassengerRideKind::EstimatedServiceIntent}) {
		CHECK(cache.Resolve(PAX, first.Ride(kind)).group == first.group_id);
		CHECK_FALSE(cache.Resolve(PAX, first.Ride(kind)).multiple);
	}
	second.group_id = f.GroupFor(second);
	cache.Clear();
	CHECK(cache.Resolve(PAX, first.Ride()).multiple);
	CHECK(cache.Resolve(PAX, first.Ride()).group == std::min(first.group_id, second.group_id));
	second.group_id = DEFAULT_GROUP; cache.Clear();
	CHECK(cache.Resolve(PAX, first.Ride()).multiple);
}

TEST_CASE("Station shared service candidates require usable passenger capacity and stops", "[passenger][station-gui]")
{
	GuiFixture f; GuiTrain first(VehicleID{100}), second(VehicleID{101}); second.Share(first);
	first.group_id = f.GroupFor(first); second.group_id = f.GroupFor(second);
	StationPassengerServiceCache cache;
	second.cargo_type = CargoType{1};
	CHECK(cache.Resolve(PAX, first.Ride()).group == first.group_id);
	second.cargo_type = PAX; second.cargo_cap = 0; cache.Clear();
	CHECK(cache.Resolve(PAX, first.Ride()).group == first.group_id);
	GuiCoach *coach = Vehicle::Create<GuiCoach>(); second.SetNext(coach); cache.Clear();
	CHECK(cache.Resolve(PAX, first.Ride()).multiple); // Passenger coaches also make a freight front a candidate.
	second.SetNext(nullptr); delete coach;
	second.cargo_cap = 100; second.vehstatus.Set(VehState::Crashed); cache.Clear();
	CHECK(cache.Resolve(PAX, first.Ride()).group == first.group_id);
	first.orders->GetOrderAt(0)->SetLoadType(OrderLoadType::NoLoad); cache.Clear();
	CHECK(cache.Resolve(PAX, first.Ride()).group == GroupID::Invalid());
	first.orders->GetOrderAt(0)->SetLoadType(OrderLoadType::LoadIfPossible);
	Station::Get(B)->facilities.Reset(StationFacility::Train); cache.Clear();
	CHECK(cache.Resolve(PAX, first.Ride()).group == GroupID::Invalid());
}

TEST_CASE("Station group and carrier removal and stale intentions remain safe", "[passenger][station-gui]")
{
	GuiFixture f; GuiTrain *train = Vehicle::Create<GuiTrain>(); train->group_id = f.GroupFor(*train);
	StationPassengerServiceCache cache;
	auto fixed = train->Ride(PassengerRideKind::VehicleSnapshot), intent = train->Ride();
	Group *group = Group::Get(train->group_id);
	group->name = "First name";
	CHECK(cache.Resolve(PAX, intent).group == group->index);
	group->name = "New name";
	CHECK(Group::Get(cache.Resolve(PAX, intent).group)->name == "New name");
	delete group;
	CHECK(cache.Resolve(PAX, fixed).group == GroupID::Invalid());
	cache.Clear(); CHECK(cache.Resolve(PAX, intent).group == GroupID::Invalid());
	intent.signature++; CHECK(cache.Resolve(PAX, intent).group == GroupID::Invalid());
	delete train;
	CHECK(cache.Resolve(PAX, fixed).group == GroupID::Invalid());
	CHECK(cache.Resolve(PAX, intent).group == GroupID::Invalid());
}

static CargoDataEntry &ExpandPassenger(CargoDataEntry &expanded, StationID destination = B, StationID source = A, StationPassengerService service = {})
{
	return expanded.InsertOrRetrieve(PAX).InsertOrRetrieve(PassengerServiceExpansionKey(service)).InsertOrRetrieve(destination).InsertOrRetrieve(source);
}

TEST_CASE("Station waiting passengers use service final destination and source for every cargo preference", "[passenger][station-gui]")
{
	GuiFixture f; GuiTrain rapid(VehicleID{100}), local(VehicleID{101});
	rapid.group_id = f.GroupFor(rapid); local.group_id = f.GroupFor(local);
	Station::CreateAtIndex(C, TileIndex{0}); Station::CreateAtIndex(D, TileIndex{0});
	CargoPacket *packet = CargoPacket::Create(10, 0, A, TileIndex{0}, Money{0}); REQUIRE(packet->SetPassengerDestination(PAX, D));
	std::array<StationCargoGrouping, 4> order{GR_CARGO, GR_SOURCE, GR_NEXT, GR_DESTINATION};
	do {
		CargoDataEntry expanded, data;
		ExpandPassenger(expanded, D, A, {rapid.group_id}); ExpandPassenger(expanded, D, A, {local.group_id});
		StationPassengerServiceCache cache;
		auto show = [&](GuiTrain &train, uint count) {
			packet->SetPassengerJourney(train.Ride(), D);
			return ShowStationWaitingCargo(data, expanded, order, A, false, PAX, *packet, B, C, count, cache);
		};
		CargoDataEntry *rapid_row = show(rapid, 70), *local_row = show(local, 40);
		CHECK(rapid_row->GetStation() == A); CHECK(local_row->GetStation() == A);
		CHECK(rapid_row != local_row); CHECK(show(rapid, 10) == rapid_row);
		CHECK(rapid_row->GetCount() == 80); CHECK(local_row->GetCount() == 40); CHECK(data.GetCount() == 120);
		auto *services = data.Retrieve(PAX);
		CHECK(services->Retrieve(StationPassengerService{rapid.group_id})->GetCount() == 80);
		CHECK(services->Retrieve(StationPassengerService{rapid.group_id})->Retrieve(D)->Retrieve(A) == rapid_row);
		CHECK(services->Retrieve(StationPassengerService{local.group_id})->Retrieve(D)->GetCount() == 40);
		CHECK(data.GetNumChildren() == 7);
		services->Resort(CargoSortType::Count, SO_DESCENDING); CHECK((**services->Begin()).GetCount() == 80);
		data.Clear(); CHECK(data.GetNumChildren() == 0); CHECK(data.GetCount() == 0);
	} while (std::next_permutation(order.begin() + 1, order.end()));
}

TEST_CASE("Station typed service and destination expansion hides descendants and conserves totals", "[passenger][station-gui]")
{
	GuiFixture f; CargoPacket *packet = f.Packet();
	std::array<StationCargoGrouping, 4> order{GR_CARGO, GR_NEXT, GR_SOURCE, GR_DESTINATION};
	CargoDataEntry expanded, data; StationPassengerServiceCache cache;
	auto show = [&]() { return ShowStationWaitingCargo(data, expanded, order, A, true, PAX, *packet, C, D, 10, cache); };
	CHECK(show()->GetCargo() == PAX); CHECK(data.GetNumChildren() == 1); CHECK(data.GetCount() == 10);
	expanded.InsertOrRetrieve(PAX); data.Clear();
	CHECK(show()->IsService()); CHECK(data.GetNumChildren() == 2); CHECK(data.GetCount() == 10);
	expanded.Retrieve(PAX)->InsertOrRetrieve(StationPassengerService{}); data.Clear();
	CHECK(show()->GetStation() == B); CHECK(data.GetNumChildren() == 3); CHECK(data.GetCount() == 10);
	ExpandPassenger(expanded); data.Clear();
	CHECK(show()->GetStation() == A); CHECK(data.GetNumChildren() == 4); CHECK(data.GetCount() == 10);
	expanded.Retrieve(PAX)->Remove(StationPassengerService{}); data.Clear();
	CHECK(show()->IsService()); CHECK(data.GetNumChildren() == 2); CHECK(data.GetCount() == 10);
}

TEST_CASE("Station GUI service reads do not mutate passenger execution or RNG", "[passenger][station-gui][determinism]")
{
	GuiFixture f; GuiTrain first(VehicleID{100}), second(VehicleID{101}); second.Share(first);
	first.group_id = second.group_id = f.GroupFor(first);
	auto *packet = f.Packet(); packet->SetPassengerJourney(first.Ride(), B);
	auto execution = packet->GetPassengerJourney();
	SavedRandomSeeds before{}, after{}; SaveRandomSeeds(&before);
	StationPassengerServiceCache cache;
	CHECK(cache.Resolve(PAX, execution).group == first.group_id);
	size_t checks = first.primary_checks + second.primary_checks;
	for (size_t i = 0; i < 10000; ++i) {
		CHECK(cache.Resolve(PAX, execution).group == first.group_id);
		CHECK(GetStationCargoVia(PAX, *packet, StationID::Invalid()) == B);
	}
	SaveRandomSeeds(&after);
	CHECK(packet->GetPassengerJourney() == execution); CHECK(packet->Count() == 10);
	CHECK(memcmp(&before, &after, sizeof(before)) == 0);
	CHECK(first.primary_checks + second.primary_checks == checks);
	cache.Clear(); CHECK(cache.Resolve(PAX, execution).group == first.group_id);
	CHECK(first.primary_checks + second.primary_checks > checks);
}

TEST_CASE("Station unknown and Fallback passengers have no inferred destination or service", "[passenger][station-gui]")
{
	GuiFixture f; GuiTrain train(VehicleID{100}); train.group_id = f.GroupFor(train);
	CargoPacket *packet = CargoPacket::Create(10, 0, A, TileIndex{0}, Money{0});
	CargoDataEntry expanded, data; ExpandPassenger(expanded, StationID::Invalid());
	std::array<StationCargoGrouping, 4> order{GR_CARGO, GR_NEXT, GR_DESTINATION, GR_SOURCE};
	StationPassengerServiceCache cache;
	auto show = [&](uint count) { return ShowStationWaitingCargo(data, expanded, order, A, true, PAX, *packet, B, B, count, cache); };
	CargoDataEntry *row = show(3); REQUIRE(row->GetStation() == A); CHECK(row->GetParent()->GetParent()->GetService().group == GroupID::Invalid());
	CHECK(show(7) == row); CHECK(show(0) == nullptr);
	CHECK(row->GetCount() == 10); CHECK(data.GetCount() == 10);
	CHECK(data.Retrieve(PAX)->Retrieve(StationPassengerService{})->Retrieve(StationID::Invalid())->GetCount() == 10);
	packet->SetPassengerJourney(train.Ride(), B); packet->FallbackPassengerJourney(A, StateTicks{});
	CHECK(cache.Resolve(PAX, *packet, A).group == GroupID::Invalid());
}

TEST_CASE("Station general cargo and planned passenger flows preserve all original groupings", "[passenger][station-gui]")
{
	GuiFixture f; CargoPacket *packet = f.Packet();
	std::array<StationCargoGrouping, 4> order{GR_CARGO, GR_SOURCE, GR_NEXT, GR_DESTINATION};
	do {
		CargoDataEntry expanded, cargo, planned;
		auto expand = [&](CargoType type) {
			CargoDataEntry *row = &expanded.InsertOrRetrieve(type);
			for (size_t i = 1; i < order.size(); ++i) row = &row->InsertOrRetrieve(order[i] == GR_SOURCE ? A : order[i] == GR_NEXT ? B : C);
		};
		expand(PAX); expand(CargoType{1}); StationPassengerServiceCache cache;
		ShowStationWaitingCargo(cargo, expanded, order, A, true, CargoType{1}, *packet, B, C, 20, cache);
		ShowStationCargo(planned, expanded, order, A, true, PAX, A, B, C, 30);
		CHECK(cargo.GetCount() == 20); CHECK(cargo.GetNumChildren() == 4);
		CHECK(planned.GetCount() == 30); CHECK(planned.GetNumChildren() == 4);
		auto check = [&](CargoDataEntry &root, CargoType type) {
			CargoDataEntry *row = root.Retrieve(type);
			for (size_t i = 1; i < order.size(); ++i) {
				row = row->Retrieve(order[i] == GR_SOURCE ? A : order[i] == GR_NEXT ? B : C);
				REQUIRE(row != nullptr); CHECK_FALSE(row->IsService());
			}
		};
		check(cargo, CargoType{1}); check(planned, PAX);
	} while (std::next_permutation(order.begin() + 1, order.end()));
}

TEST_CASE("Station service fleet inspection remains bounded and never guesses an uninspected group", "[passenger][station-gui]")
{
	GuiFixture f;
	std::vector<GuiTrain *> fleet;
	for (size_t i = 0; i < 5000; ++i) {
		GuiTrain *train = Vehicle::Create<GuiTrain>();
		if (!fleet.empty()) { train->Share(*fleet.front()); train->group_id = fleet.front()->group_id; }
		else train->group_id = f.GroupFor(*train);
		fleet.push_back(train);
	}
	StationPassengerServiceCache cache;
	CHECK(cache.Resolve(PAX, fleet.front()->Ride()).group == fleet.front()->group_id);
	CHECK(cache.Resolve(PAX, fleet.front()->Ride()).multiple);
	for (auto it = fleet.rbegin(); it != fleet.rend(); ++it) delete *it;
}

TEST_CASE("Station passenger totals match across services destinations and sources", "[passenger][station-gui]")
{
	GuiFixture f; GuiTrain unique(VehicleID{100}), mixed(VehicleID{101}), other(VehicleID{102});
	unique.group_id = mixed.group_id = f.GroupFor(unique); other.group_id = f.GroupFor(other); other.Share(mixed);
	Station::CreateAtIndex(C, TileIndex{0}); Station::CreateAtIndex(D, TileIndex{0});
	CargoDataEntry expanded, data;
	ExpandPassenger(expanded, D, A, {unique.group_id}); ExpandPassenger(expanded, D, C, {unique.group_id});
	ExpandPassenger(expanded, D, A, {unique.group_id, true}); ExpandPassenger(expanded, B, C, {unique.group_id, true});
	ExpandPassenger(expanded, D, A);
	StationPassengerServiceCache cache;
	std::array<StationCargoGrouping, 4> order{GR_CARGO, GR_NEXT, GR_SOURCE, GR_DESTINATION};
	auto show = [&](StationID destination, StationID origin, GuiTrain *train, uint16_t count) {
		auto *packet = CargoPacket::Create(count, 0, origin, TileIndex{0}, Money{0}); REQUIRE(packet->SetPassengerDestination(PAX, destination));
		if (train != nullptr) packet->SetPassengerJourney(train->Ride(), B);
		ShowStationWaitingCargo(data, expanded, order, A, true, PAX, *packet, B, destination, count, cache);
	};
	show(D, A, &unique, 70); show(D, A, &mixed, 40); show(D, A, nullptr, 10);
	show(D, C, &unique, 60); show(B, C, &mixed, 70);
	auto *cargo = data.Retrieve(PAX); CHECK(data.GetCount() == 250); CHECK(cargo->GetCount() == 250);
	CHECK(cargo->Retrieve(StationPassengerService{unique.group_id})->GetCount() == 130);
	CHECK(cargo->Retrieve(StationPassengerService{unique.group_id, true})->GetCount() == 110);
	CHECK(cargo->Retrieve(StationPassengerService{})->GetCount() == 10);
	uint total = 0;
	for (auto gi = cargo->Begin(); gi != cargo->End(); ++gi) {
		CHECK((*gi)->IsService()); uint destinations = 0;
		for (auto di = (*gi)->Begin(); di != (*gi)->End(); ++di) {
			uint sources = 0;
			for (auto si = (*di)->Begin(); si != (*di)->End(); ++si) sources += (*si)->GetCount();
			CHECK(sources == (*di)->GetCount()); destinations += (*di)->GetCount();
		}
		CHECK(destinations == (*gi)->GetCount()); total += (*gi)->GetCount();
	}
	CHECK(total == 250);
}

TEST_CASE("Station completed ungrouped Journey rides share one classification without a false transfer hint", "[passenger][station-gui]")
{
	GuiFixture f; GuiTrain train(VehicleID{100});
	Station::CreateAtIndex(D, TileIndex{0});
	auto *packet = CargoPacket::Create(10, 0, A, TileIndex{0}, Money{0});
	REQUIRE(packet->SetPassengerDestination(PAX, D));
	packet->SetPassengerJourney(train.Ride(), B);
	StationPassengerServiceCache cache;
	CHECK(cache.Resolve(PAX, *packet, A).group == GroupID::Invalid());
	CHECK(cache.Resolve(PAX, *packet, A).label == StationPassengerServiceLabel::Ungrouped);
	CHECK(cache.Resolve(PAX, *packet, A).transfer == StationID::Invalid());
	auto *direct = f.Packet(); direct->SetPassengerJourney(train.Ride(), B);
	CHECK(cache.Resolve(PAX, *direct, A).label == StationPassengerServiceLabel::Ungrouped);
	auto stale = train.Ride(); ++stale.signature; packet->SetPassengerJourney(stale, B);
	cache.Advance(0); CHECK(cache.Resolve(PAX, *packet, A).group == GroupID::Invalid());
	train.group_id = f.GroupFor(train); cache.Clear(); packet->SetPassengerJourney(train.Ride(), B);
	CHECK(cache.Resolve(PAX, *packet, A) == StationPassengerService{train.group_id});
}

TEST_CASE("Station exhausted display work does not permanently hide another named fleet", "[passenger][station-gui][service-progress]")
{
	GuiFixture f;
	std::vector<GuiTrain *> fleet;
	for (size_t i = 0; i < 5000; ++i) {
		auto *train = Vehicle::Create<GuiTrain>();
		if (fleet.empty()) train->group_id = f.GroupFor(*train);
		else { train->Share(*fleet.front()); train->group_id = fleet.front()->group_id; }
		fleet.push_back(train);
	}
	GuiTrain small(VehicleID{60000}); small.group_id = f.GroupFor(small);
	StationPassengerServiceCache cache;
	cache.Resolve(PAX, fleet.front()->Ride());
	for (size_t i = 0; i < 10; ++i) cache.Resolve(PAX, small.Ride());
	CHECK(cache.Resolve(PAX, small.Ride()).group == small.group_id);
	CHECK(cache.GetDiagnostics().partial == 1);
	for (size_t update = 0; update < 128 && cache.GetDiagnostics().partial != 0; ++update) cache.Advance(100, 512);
	CHECK(cache.GetDiagnostics().partial == 0); CHECK(cache.GetDiagnostics().complete == 2);
	CHECK(cache.GetDiagnostics().vehicles == 5001); // Each shared carrier is inspected once, not once per update.
	CHECK_FALSE(cache.Resolve(PAX, fleet.front()->Ride()).multiple);
	fleet.front()->orders->GetOrderAt(0)->SetLoadType(OrderLoadType::NoLoad);
	cache.Advance(11000); cache.Resolve(PAX, fleet.front()->Ride()); cache.Advance(0);
	CHECK(cache.Resolve(PAX, fleet.front()->Ride()).group == GroupID::Invalid());
	CHECK(cache.GetDiagnostics().unusable_vehicle > 0);
	for (auto it = fleet.rbegin(); it != fleet.rend(); ++it) delete *it;
}

TEST_CASE("Station complete services survive real-time seconds and revalidate live group changes", "[passenger][station-gui][service-progress]")
{
	GuiFixture f; GuiTrain *train = Vehicle::Create<GuiTrain>(); train->group_id = f.GroupFor(*train);
	StationPassengerServiceCache cache; auto ride = train->Ride();
	CHECK(cache.Resolve(PAX, ride).group == train->group_id);
	auto before = cache.GetDiagnostics(); size_t checks = train->primary_checks;
	cache.Advance(1000);
	CHECK(cache.Resolve(PAX, ride).group == train->group_id);
	CHECK(cache.GetDiagnostics().work == before.work); CHECK(train->primary_checks == checks);
	Group::Get(train->group_id)->name = "Renamed service";
	CHECK(Group::Get(cache.Resolve(PAX, ride).group)->name == "Renamed service");
	GroupID old = train->group_id; train->group_id = f.GroupFor(*train);
	cache.Resolve(PAX, ride); cache.Advance(0);
	CHECK(cache.Resolve(PAX, ride).group == train->group_id); CHECK(cache.Resolve(PAX, ride).group != old);
	delete train; cache.Resolve(PAX, ride); cache.Advance(0);
	CHECK(cache.Resolve(PAX, ride).group == GroupID::Invalid()); CHECK(cache.GetDiagnostics().stale_service > 0);
}

TEST_CASE("Station long consists resume with tiny slices and move counts out of checking exactly once", "[passenger][station-gui][service-progress][determinism]")
{
	GuiFixture f; auto *train = Vehicle::Create<GuiTrain>(); train->group_id = f.GroupFor(*train);
	train->cargo_type = CargoType{1}; train->cargo_cap = 0;
	std::vector<GuiCoach *> parts; Vehicle *tail = train;
	for (size_t i = 0; i < 150; ++i) {
		auto *part = Vehicle::Create<GuiCoach>(); part->cargo_type = CargoType{1}; tail->SetNext(part); tail = part; parts.push_back(part);
	}
	parts.back()->cargo_type = PAX;
	auto *packet = f.Packet(); packet->SetPassengerJourney(train->Ride(), B);
	const auto execution = packet->GetPassengerJourney(); GameRandomSeedChecker seeds;
	StationPassengerServiceCache cache;
	CargoDataEntry expanded, data; expanded.InsertOrRetrieve(PAX);
	std::array<StationCargoGrouping, 4> order{GR_CARGO, GR_NEXT, GR_SOURCE, GR_DESTINATION};
	auto show = [&]() {
		data.Clear(); ShowStationWaitingCargo(data, expanded, order, A, true, PAX, *packet, B, B, 100, cache);
		CHECK(data.GetCount() == 100); CHECK(data.Retrieve(PAX)->GetCount() == 100);
		CHECK(data.Retrieve(PAX)->GetNumChildren() == 1);
	};
	show(); CHECK((**data.Retrieve(PAX)->Begin()).GetService().label == StationPassengerServiceLabel::Checking);
	for (size_t i = 0; i < 300 && cache.GetDiagnostics().complete == 0; ++i) { cache.Advance(100, 1); show(); }
	REQUIRE(cache.GetDiagnostics().complete == 1);
	CHECK((**data.Retrieve(PAX)->Begin()).GetService().group == train->group_id);
	CHECK(cache.GetDiagnostics().vehicles == 1); CHECK(cache.GetDiagnostics().work < 180);
	CHECK(cache.GetDiagnostics().work_exhausted > 0);
	CHECK(packet->GetPassengerJourney() == execution); CHECK(packet->Count() == 10); CHECK(seeds.Check());
	train->SetNext(nullptr); for (auto *part : parts) { part->SetNext(nullptr); delete part; } delete train;
}

TEST_CASE("Station ungrouped fleets refresh without clearing unrelated completed entries", "[passenger][station-gui][service-progress]")
{
	GuiFixture f; GuiTrain first(VehicleID{100}), second(VehicleID{101});
	StationPassengerServiceCache cache;
	CHECK(cache.Resolve(PAX, first.Ride()).label == StationPassengerServiceLabel::Ungrouped);
	second.group_id = f.GroupFor(second); CHECK(cache.Resolve(PAX, second.Ride()).group == second.group_id);
	first.group_id = f.GroupFor(first); cache.Advance(11000);
	cache.Resolve(PAX, first.Ride()); cache.Advance(0);
	CHECK(cache.Resolve(PAX, first.Ride()).group == first.group_id);
	CHECK(cache.GetDiagnostics().ungrouped > 0); CHECK(cache.GetDiagnostics().refreshes > 0);
}

TEST_CASE("Station service caches have bounded records under many stale ride conditions", "[passenger][station-gui][service-progress]")
{
	GuiFixture f; GuiTrain train(VehicleID{100});
	StationPassengerServiceCache cache;
	for (size_t i = 0; i < 5000; ++i) { auto ride = train.Ride(); ride.signature += i + 1; cache.Resolve(PAX, ride); }
	CHECK(cache.GetDiagnostics().results <= cache.GetDiagnostics().result_capacity); CHECK(cache.GetDiagnostics().searches <= 4096);
	CHECK(cache.GetDiagnostics().stale_service > 0);
}

TEST_CASE("Station passenger grouping controls match cargo presence and planned mode", "[passenger][station-gui]")
{
	using Mode = StationCargoGroupingMode;
	CHECK(GetStationCargoGroupingMode(false, true, false) == Mode::Passengers);
	CHECK(GetStationCargoGroupingMode(false, true, true) == Mode::Mixed);
	CHECK(GetStationCargoGroupingMode(false, false, true) == Mode::Conventional);
	CHECK(GetStationCargoGroupingMode(false, false, false) == Mode::Conventional);
	CHECK(GetStationCargoGroupingMode(true, true, false) == Mode::Conventional);
	CHECK(GetStationCargoGroupingMode(true, true, true) == Mode::Conventional);
	StationPassengerService partial{GroupID{10}, true}, complete{GroupID{10}};
	CHECK(PassengerServiceExpansionKey(partial) == PassengerServiceExpansionKey(complete));
	CargoDataEntry expanded; ExpandPassenger(expanded, B, A, partial);
	REQUIRE(expanded.Retrieve(PAX)->Retrieve(PassengerServiceExpansionKey(complete)) != nullptr);
	expanded.Retrieve(PAX)->Remove(PassengerServiceExpansionKey(complete));
	CHECK(expanded.Retrieve(PAX)->Retrieve(PassengerServiceExpansionKey(partial)) == nullptr);
}

TEST_CASE("Station round robin lets a short pending consist finish while a large one is still checking", "[passenger][station-gui][service-progress]")
{
	GuiFixture f; auto *large = Vehicle::Create<GuiTrain>(), *small = Vehicle::Create<GuiTrain>();
	large->group_id = f.GroupFor(*large); small->group_id = f.GroupFor(*small);
	std::vector<GuiCoach *> large_parts, small_parts;
	auto attach = [&](GuiTrain *train, size_t count, std::vector<GuiCoach *> &parts) {
		train->cargo_type = CargoType{1}; train->cargo_cap = 0; Vehicle *tail = train;
		for (size_t i = 0; i < count; ++i) { auto *part = Vehicle::Create<GuiCoach>(); part->cargo_type = CargoType{1}; tail->SetNext(part); tail = part; parts.push_back(part); }
		parts.back()->cargo_type = PAX;
	};
	attach(large, 1000, large_parts); attach(small, 80, small_parts);
	StationPassengerServiceCache cache;
	CHECK(cache.Resolve(PAX, large->Ride()).label == StationPassengerServiceLabel::Checking);
	CHECK(cache.Resolve(PAX, small->Ride()).label == StationPassengerServiceLabel::Checking);
	for (size_t update = 0; update < 8; ++update) cache.Advance(100, 16);
	CHECK(cache.Resolve(PAX, small->Ride()).group == small->group_id);
	CHECK(cache.Resolve(PAX, large->Ride()).label == StationPassengerServiceLabel::Checking);
	CHECK(cache.GetDiagnostics().partial == 1); CHECK(cache.GetDiagnostics().complete == 1);
	CHECK(cache.GetDiagnostics().vehicles == 2);
	large->SetNext(nullptr); small->SetNext(nullptr);
	for (auto *part : large_parts) { part->SetNext(nullptr); delete part; }
	for (auto *part : small_parts) { part->SetNext(nullptr); delete part; }
	delete small; delete large;
}

TEST_CASE("Station concrete Journey cache does not require a Hyperpath database generation", "[passenger][station-gui][service-progress]")
{
	GuiFixture f; auto *train = Vehicle::Create<GuiTrain>(); train->group_id = f.GroupFor(*train);
	StationPassengerServiceCache cache;
	for (auto kind : {PassengerRideKind::VehicleSnapshot, PassengerRideKind::ServiceIntent, PassengerRideKind::BoundService}) {
		auto ride = train->Ride(kind); ride.generation = 123456;
		CHECK(cache.Resolve(PAX, ride).group == train->group_id);
		auto work = cache.GetDiagnostics().work;
		CHECK(cache.Resolve(PAX, ride).group == train->group_id); CHECK(cache.GetDiagnostics().work == work);
	}
	delete train;
}

TEST_CASE("Station paused completed results do not change classification on a refresh timer", "[passenger][station-gui][service-stability]")
{
	GuiFixture f; auto *train = Vehicle::Create<GuiTrain>(); train->group_id = f.GroupFor(*train);
	StationPassengerServiceCache cache; const auto ride = train->Ride();
	const auto expected = cache.Resolve(PAX, ride); REQUIRE(expected.group == train->group_id);
	const auto tick = _state_ticks; const auto work = cache.GetDiagnostics().work;
	for (size_t second = 0; second < 300; ++second) {
		cache.Advance(1000, 0); CHECK(cache.Resolve(PAX, ride) == expected);
		cache.Advance(0); CHECK(cache.Resolve(PAX, ride) == expected);
	}
	CHECK(cache.GetDiagnostics().work == work); CHECK(_state_ticks == tick);
	delete train;
}

TEST_CASE("Station ten thousand paused conditions accumulate stable completed results", "[passenger][station-gui][service-stability]")
{
	GuiFixture f; auto *train = Vehicle::Create<GuiTrain>(); train->group_id = f.GroupFor(*train);
	StationPassengerServiceCache cache; auto ride = train->Ride(PassengerRideKind::VehicleSnapshot);
	const auto tick = _state_ticks; const auto signature = ride.signature;
	size_t named = 0;
	for (size_t frame = 0; frame < 20; ++frame) {
		named = 0;
		for (size_t condition = 0; condition < 10000; ++condition) {
			ride.signature = signature + condition;
			if (cache.Resolve(PAX, ride) == StationPassengerService{train->group_id}) ++named;
		}
		cache.Advance(1000);
	}
	CHECK(named == 10000); CHECK(_state_ticks == tick);
	const auto work = cache.GetDiagnostics().work;
	for (size_t frame = 0; frame < 15; ++frame) {
		cache.Advance(1000);
		for (size_t condition = 0; condition < 10000; ++condition) {
			ride.signature = signature + condition; CHECK(cache.Resolve(PAX, ride) == StationPassengerService{train->group_id});
		}
	}
	CHECK(cache.GetDiagnostics().work == work);
	const auto d = cache.GetDiagnostics();
	CHECK(d.results == 10000); CHECK(d.searches == 0); CHECK(d.result_evictions == 0); CHECK(d.display_regressions == 0);
	std::cout << "Station 10000 paused conditions: results=" << d.results << ", capacity=" << d.result_capacity << ", search_workspaces=" << d.searches << ", estimated_bytes=" << d.estimated_bytes << ", work=" << d.work << ", result_evictions=" << d.result_evictions << std::endl;
	delete train;
}

TEST_CASE("Station capacity pressure preserves active results and declines excess work without churn", "[passenger][station-gui][service-stability]")
{
	GuiFixture f; auto *train = Vehicle::Create<GuiTrain>(); train->group_id = f.GroupFor(*train);
	StationPassengerServiceCache cache(64 * 1024); auto ride = train->Ride(PassengerRideKind::VehicleSnapshot);
	const auto signature = ride.signature, capacity = cache.GetDiagnostics().result_capacity;
	REQUIRE(capacity > 0); REQUIRE(capacity < 500);
	for (size_t frame = 0; frame < 20; ++frame) {
		cache.BeginRefresh();
		for (size_t query = 0; query < capacity + 50; ++query) {
			ride.signature = signature + query;
			auto result = cache.Resolve(PAX, ride);
			if (query < capacity) CHECK(result == StationPassengerService{train->group_id});
			else CHECK(result.label == StationPassengerServiceLabel::Updating);
		}
		CHECK_FALSE(cache.Advance(1000));
	}
	auto diagnostic = cache.GetDiagnostics();
	CHECK(diagnostic.results == capacity); CHECK(diagnostic.result_evictions == 0); CHECK(diagnostic.capacity_waits == 1000);
	CHECK(diagnostic.capacity_regressions == 0); CHECK(diagnostic.display_regressions == 0); CHECK(diagnostic.searches == 0);
	const auto work = diagnostic.work;
	// Once old conditions leave the window's working set their compact results can be replaced.
	for (size_t frame = 0; frame < 3; ++frame) { cache.BeginRefresh(); ride.signature = signature; cache.Resolve(PAX, ride); }
	ride.signature = signature + capacity + 100; CHECK(cache.Resolve(PAX, ride).group == train->group_id);
	CHECK(cache.GetDiagnostics().result_evictions > 0); CHECK(cache.GetDiagnostics().work > work);
	ride.signature = signature; CHECK(cache.Resolve(PAX, ride).group == train->group_id);
	delete train;
}

TEST_CASE("Station group notifications refresh other candidates without resetting unrelated results", "[passenger][station-gui][service-stability]")
{
	GuiFixture f; auto *first = Vehicle::Create<GuiTrain>(), *second = Vehicle::Create<GuiTrain>(), *unrelated = Vehicle::Create<GuiTrain>();
	first->group_id = second->group_id = f.GroupFor(*first); second->Share(*first); unrelated->group_id = f.GroupFor(*unrelated);
	StationPassengerServiceCache cache; const auto expected = StationPassengerService{first->group_id};
	REQUIRE(cache.Resolve(PAX, first->Ride()) == expected); REQUIRE(cache.Resolve(PAX, unrelated->Ride()).group == unrelated->group_id);
	const auto vehicles = cache.GetDiagnostics().vehicles;
	second->group_id = f.GroupFor(*second); cache.Invalidate(first->orders->index);
	CHECK(cache.Resolve(PAX, first->Ride()) == expected); // Hold the valid published display while work is pending.
	CHECK(cache.Resolve(PAX, unrelated->Ride()) == StationPassengerService{unrelated->group_id});
	cache.Advance(100);
	CHECK(cache.Resolve(PAX, first->Ride()) == StationPassengerService{first->group_id, true});
	CHECK(cache.GetDiagnostics().vehicles == vehicles + 2);
	CHECK(cache.GetDiagnostics().display_regressions == 0);
	second->group_id = first->group_id; cache.Invalidate(first->orders->index); cache.Resolve(PAX, first->Ride()); cache.Advance(100);
	CHECK(cache.Resolve(PAX, first->Ride()) == expected);
	delete unrelated; delete second; delete first;
}

TEST_CASE("Station background validation preserves a settled name and invisible work does not redraw", "[passenger][station-gui][service-stability]")
{
	GuiFixture f; auto *train = Vehicle::Create<GuiTrain>(); train->group_id = f.GroupFor(*train);
	std::vector<GuiCoach *> parts; Vehicle *tail = train; train->cargo_cap = 0;
	for (size_t i = 0; i < 250; ++i) { auto *part = Vehicle::Create<GuiCoach>(); part->cargo_cap = 0; tail->SetNext(part); tail = part; parts.push_back(part); }
	parts.back()->cargo_cap = 50;
	StationPassengerServiceCache cache; auto ride = train->Ride(); cache.Resolve(PAX, ride);
	CHECK_FALSE(cache.Advance(100, 1)); // Work advanced but the visible checking category did not change.
	for (size_t update = 0; cache.GetDiagnostics().complete == 0 && update < 1000; ++update) cache.Advance(100, 1);
	const auto expected = cache.Resolve(PAX, ride); REQUIRE(expected == StationPassengerService{train->group_id});
	AutoRestoreBackup tick{_state_ticks, _state_ticks + 1};
	cache.Advance(45000, 0); CHECK(cache.Resolve(PAX, ride) == expected);
	for (size_t update = 0; update < 300; ++update) { CHECK_FALSE(cache.Advance(100, 1)); CHECK(cache.Resolve(PAX, ride) == expected); }
	CHECK(cache.GetDiagnostics().periodic_revalidations > 0); CHECK(cache.GetDiagnostics().display_regressions == 0);
	train->SetNext(nullptr); for (auto *part : parts) { part->SetNext(nullptr); delete part; } delete train;
}

TEST_CASE("Station capacity admission resumes after the displayed working set changes", "[passenger][station-gui][service-stability]")
{
	GuiFixture f; auto *train = Vehicle::Create<GuiTrain>(); train->group_id = f.GroupFor(*train);
	StationPassengerServiceCache cache(4096); auto ride = train->Ride(PassengerRideKind::VehicleSnapshot);
	const auto signature = ride.signature, capacity = cache.GetDiagnostics().result_capacity;
	cache.BeginRefresh();
	for (size_t i = 0; i < capacity; ++i) { ride.signature = signature + i; REQUIRE(cache.Resolve(PAX, ride).group == train->group_id); }
	cache.BeginRefresh();
	for (size_t i = 0; i < capacity; ++i) { ride.signature = signature + capacity + i; CHECK(cache.Resolve(PAX, ride).label == StationPassengerServiceLabel::Updating); }
	CHECK(cache.Advance(1000)); // Free obsolete results and request one admission retry paint.
	cache.BeginRefresh();
	for (size_t i = 0; i < capacity; ++i) { ride.signature = signature + capacity + i; CHECK(cache.Resolve(PAX, ride).group == train->group_id); }
	CHECK(cache.GetDiagnostics().capacity_regressions == 0); CHECK(cache.GetDiagnostics().display_regressions == 0);
	delete train;
}
