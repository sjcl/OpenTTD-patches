/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */

/** @file station_cargo_gui.cpp Read-only passenger destination/source/service hierarchy regressions. */
#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../station_cargo_gui.h"
#include "../passenger_journey_active.h"
#include "../passenger_service_database.h"
#include "../core/backup_type.hpp"
#include "../settings_type.h"
#include "../cargopacket.h"
#include "../vehicle_base.h"
#include "../group.h"
#include "../core/random_func.hpp"
#include "passenger_test_stations.h"

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

static CargoDataEntry &ExpandPassenger(CargoDataEntry &expanded, StationID destination = B, StationID source = A)
{
	return expanded.InsertOrRetrieve(PAX).InsertOrRetrieve(destination).InsertOrRetrieve(source);
}

TEST_CASE("Station waiting passengers use final destination source and typed service for every cargo preference", "[passenger][station-gui]")
{
	GuiFixture f; GuiTrain rapid(VehicleID{100}), local(VehicleID{101});
	rapid.group_id = f.GroupFor(rapid); local.group_id = f.GroupFor(local);
	Station::CreateAtIndex(C, TileIndex{0}); Station::CreateAtIndex(D, TileIndex{0});
	CargoPacket *packet = CargoPacket::Create(10, 0, A, TileIndex{0}, Money{0});
	REQUIRE(packet->SetPassengerDestination(PAX, D));
	std::array<StationCargoGrouping, 4> order{GR_CARGO, GR_SOURCE, GR_NEXT, GR_DESTINATION};
	do {
		CargoDataEntry expanded, data; ExpandPassenger(expanded, D);
		StationPassengerServiceCache cache;
		auto show = [&](GuiTrain &train, uint count) {
			packet->SetPassengerJourney(train.Ride(), D);
			return ShowStationWaitingCargo(data, expanded, order, A, false, PAX, *packet, B, C, count, cache);
		};
		CargoDataEntry *rapid_row = show(rapid, 70), *local_row = show(local, 40);
		REQUIRE(rapid_row->IsService()); REQUIRE(local_row->IsService());
		CHECK(rapid_row != local_row); CHECK(show(rapid, 10) == rapid_row);
		CHECK(rapid_row->GetService().group == rapid.group_id); CHECK(rapid_row->GetCount() == 80);
		CHECK(local_row->GetCount() == 40); CHECK(data.GetCount() == 120);
		auto *destination = data.Retrieve(PAX)->Retrieve(D);
		REQUIRE(destination != nullptr); CHECK(destination->GetCount() == 120);
		auto *source = destination->Retrieve(A); REQUIRE(source != nullptr);
		CHECK(source->GetCount() == 120); CHECK(source->GetNumChildren() == 2);
		CHECK(data.GetNumChildren() == 5);
		source->Resort(CargoSortType::Count, SO_DESCENDING);
		CHECK((**source->Begin()).GetCount() == 80);
		CHECK(data.GetNumChildren() == 5); CHECK(data.GetCount() == 120);
		data.Clear(); CHECK(data.GetNumChildren() == 0); CHECK(data.GetCount() == 0);
	} while (std::next_permutation(order.begin() + 1, order.end()));
}

TEST_CASE("Station passenger tree expansion hides descendants and conserves totals", "[passenger][station-gui]")
{
	GuiFixture f; CargoPacket *packet = f.Packet();
	std::array<StationCargoGrouping, 4> order{GR_CARGO, GR_NEXT, GR_SOURCE, GR_DESTINATION};
	CargoDataEntry expanded, data; StationPassengerServiceCache cache;
	auto show = [&]() { return ShowStationWaitingCargo(data, expanded, order, A, true, PAX, *packet, C, D, 10, cache); };
	CHECK(show()->GetCargo() == PAX); CHECK(data.GetNumChildren() == 1); CHECK(data.GetCount() == 10);
	expanded.InsertOrRetrieve(PAX); data.Clear();
	CHECK(show()->GetStation() == B); CHECK(data.GetNumChildren() == 2); CHECK(data.GetCount() == 10);
	expanded.Retrieve(PAX)->InsertOrRetrieve(B); data.Clear();
	CHECK(show()->GetStation() == A); CHECK(data.GetNumChildren() == 3); CHECK(data.GetCount() == 10);
	ExpandPassenger(expanded); data.Clear();
	CHECK(show()->IsService()); CHECK(data.GetNumChildren() == 4); CHECK(data.GetCount() == 10);
	expanded.Retrieve(PAX)->Remove(B); data.Clear();
	CHECK(show()->GetStation() == B); CHECK(data.GetNumChildren() == 2); CHECK(data.GetCount() == 10);
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
	CargoDataEntry *row = show(3); REQUIRE(row->IsService()); CHECK(row->GetService().group == GroupID::Invalid());
	CHECK(show(7) == row); CHECK(show(0) == nullptr);
	CHECK(row->GetCount() == 10); CHECK(data.GetCount() == 10);
	CHECK(data.Retrieve(PAX)->Retrieve(StationID::Invalid())->GetCount() == 10);
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

TEST_CASE("Station passenger totals match across destinations origins and ambiguous service leaves", "[passenger][station-gui]")
{
	GuiFixture f; GuiTrain unique(VehicleID{100}), mixed(VehicleID{101}), other(VehicleID{102});
	unique.group_id = mixed.group_id = f.GroupFor(unique); other.group_id = f.GroupFor(other); other.Share(mixed);
	Station::CreateAtIndex(C, TileIndex{0}); Station::CreateAtIndex(D, TileIndex{0});
	CargoDataEntry expanded, data; ExpandPassenger(expanded, D, A); ExpandPassenger(expanded, D, C); ExpandPassenger(expanded, B, C);
	StationPassengerServiceCache cache;
	std::array<StationCargoGrouping, 4> order{GR_CARGO, GR_NEXT, GR_SOURCE, GR_DESTINATION};
	auto show = [&](StationID destination, StationID origin, GuiTrain *train, uint16_t count) {
		auto *packet = CargoPacket::Create(count, 0, origin, TileIndex{0}, Money{0});
		REQUIRE(packet->SetPassengerDestination(PAX, destination));
		if (train != nullptr) packet->SetPassengerJourney(train->Ride(), B);
		ShowStationWaitingCargo(data, expanded, order, A, true, PAX, *packet, B, destination, count, cache);
	};
	show(D, A, &unique, 70); show(D, A, &mixed, 40); show(D, A, nullptr, 10);
	show(D, C, &unique, 60); show(B, C, &mixed, 70);
	CHECK(data.GetCount() == 250); CHECK(data.Retrieve(PAX)->GetCount() == 250);
	auto *destination = data.Retrieve(PAX)->Retrieve(D); REQUIRE(destination != nullptr);
	CHECK(destination->GetCount() == 180); CHECK(destination->Retrieve(A)->GetCount() == 120);
	CHECK(destination->Retrieve(C)->GetCount() == 60);
	CHECK(destination->Retrieve(A)->GetNumChildren() == 3); // Named, representative + others, and unknown are separate typed leaves.
	CHECK(data.Retrieve(PAX)->Retrieve(B)->GetCount() == 70);
	CHECK(data.Retrieve(PAX)->Retrieve(B)->Retrieve(C)->GetCount() == 70);
	for (auto di = data.Retrieve(PAX)->Begin(); di != data.Retrieve(PAX)->End(); ++di) {
		uint origins = 0;
		for (auto si = (*di)->Begin(); si != (*di)->End(); ++si) {
			uint services = 0;
			for (auto gi = (*si)->Begin(); gi != (*si)->End(); ++gi) { CHECK((*gi)->IsService()); services += (*gi)->GetCount(); }
			CHECK(services == (*si)->GetCount()); origins += (*si)->GetCount();
		}
		CHECK(origins == (*di)->GetCount());
	}
}

TEST_CASE("Station undecided Journey transfer hints never describe a direct final destination as a transfer", "[passenger][station-gui]")
{
	GuiFixture f; GuiTrain train(VehicleID{100});
	Station::CreateAtIndex(D, TileIndex{0});
	auto *packet = CargoPacket::Create(10, 0, A, TileIndex{0}, Money{0});
	REQUIRE(packet->SetPassengerDestination(PAX, D));
	packet->SetPassengerJourney(train.Ride(), B);
	StationPassengerServiceCache cache;
	CHECK(cache.Resolve(PAX, *packet, A).group == GroupID::Invalid());
	CHECK(cache.Resolve(PAX, *packet, A).transfer == B);
	auto *direct = f.Packet(); direct->SetPassengerJourney(train.Ride(), B);
	CHECK(cache.Resolve(PAX, *direct, A) == StationPassengerService{});
	auto stale = train.Ride(); ++stale.signature; packet->SetPassengerJourney(stale, B);
	CHECK(cache.Resolve(PAX, *packet, A) == StationPassengerService{});
	train.group_id = f.GroupFor(train); cache.Clear(); packet->SetPassengerJourney(train.Ride(), B);
	CHECK(cache.Resolve(PAX, *packet, A) == StationPassengerService{train.group_id});
}
