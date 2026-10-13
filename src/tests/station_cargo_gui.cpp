/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */

/** @file station_cargo_gui.cpp Read-only passenger via/service and hierarchy regressions. */
#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../station_cargo_gui.h"
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
	CHECK(GetStationCargoVia(PAX, *packet, StationID::Invalid()) == B);
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
	CHECK(cache.Resolve(PAX, first.Ride()).group == GroupID::Invalid());
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

TEST_CASE("Station distinct shared services produce distinct named summaries at the same via station", "[passenger][station-gui]")
{
	GuiFixture f; GuiTrain rapid(VehicleID{100}), local(VehicleID{101});
	rapid.group_id = f.GroupFor(rapid); local.group_id = f.GroupFor(local);
	StationPassengerServiceCache cache;
	CargoDataEntry expanded, data;
	expanded.InsertOrRetrieve(PAX);
	std::array<StationCargoGrouping, 4> order{GR_CARGO, GR_NEXT, GR_SOURCE, GR_DESTINATION};
	CargoDataEntry *via = ShowStationCargo(data, expanded, order, A, true, PAX, A, B, B, 420);
	REQUIRE(via != nullptr); via->AddService(cache.Resolve(PAX, rapid.Ride()), 420);
	CHECK(ShowStationCargo(data, expanded, order, A, true, PAX, A, B, B, 310) == via);
	via->AddService(cache.Resolve(PAX, local.Ride(PassengerRideKind::EstimatedServiceIntent)), 310);
	CHECK(via->GetServices().size() == 2);
	CHECK(via->GetServices().at({rapid.group_id}) == 420);
	CHECK(via->GetServices().at({local.group_id}) == 310);
	CHECK(data.GetCount() == 730); CHECK(data.GetNumRows() == 4);
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

static void ExpandPath(CargoDataEntry &expanded, std::span<const StationCargoGrouping, 4> order, StationID source, StationID via, StationID destination)
{
	CargoDataEntry *row = &expanded.InsertOrRetrieve(PAX);
	for (size_t i = 1; i < order.size(); ++i) row = &row->InsertOrRetrieve(order[i] == GR_SOURCE ? source : order[i] == GR_NEXT ? via : destination);
}

TEST_CASE("Station service summaries conserve counts and stay in their via branch for all group orders", "[passenger][station-gui]")
{
	std::array<StationCargoGrouping, 4> order{GR_CARGO, GR_SOURCE, GR_NEXT, GR_DESTINATION};
	const StationPassengerService rapid{GroupID{10}}, local{GroupID{11}}, mixed{GroupID::Invalid(), true}, unknown{};
	do {
		CargoDataEntry expanded, data;
		ExpandPath(expanded, order, A, B, D);
		ExpandPath(expanded, order, C, B, A);
		auto add = [&](StationID source, StationID destination, StationPassengerService service, uint count) {
			CargoDataEntry *via = ShowStationCargo(data, expanded, order, A, true, PAX, source, B, destination, count);
			REQUIRE(via != nullptr); via->AddService(service, count); return via;
		};
		CargoDataEntry *via = add(A, D, rapid, 420);
		CHECK(add(A, D, local, 310) == via);
		CHECK(add(A, D, mixed, 30) == via);
		CHECK(add(A, D, unknown, 20) == via);
		CHECK(add(A, D, rapid, 10) == via);
		CHECK(via->GetServices().at(rapid) == 430);
		CHECK(via->GetServices().at(local) == 310);
		CHECK(via->GetServices().at(mixed) == 30);
		CHECK(via->GetServices().at(unknown) == 20);
		CHECK(data.GetCount() == 790);
		CHECK(data.GetNumRows() == data.GetNumChildren() + 4);
		CargoDataEntry *other = add(C, A, local, 100);
		if (order[1] == GR_NEXT) {
			CHECK(other == via); CHECK(via->GetServices().at(local) == 410);
		} else {
			CHECK(other != via); CHECK(other->GetServices().size() == 1);
			CHECK(via->GetServices().at(local) == 310);
		}
		CHECK(data.GetCount() == 890);
		CHECK(data.Retrieve(PAX)->GetCount() == 890);
		data.Retrieve(PAX)->Resort(CargoSortType::Count, SO_DESCENDING);
		CHECK(data.GetCount() == 890);
		CHECK(data.GetNumRows() == data.GetNumChildren() + (other == via ? 4 : 5));
		data.Clear(); CHECK(data.GetCount() == 0); CHECK(data.GetNumRows() == 0);
	} while (std::next_permutation(order.begin() + 1, order.end()));
}

TEST_CASE("Station annotations persist below a collapsed via and vanish when its ancestors collapse", "[passenger][station-gui]")
{
	std::array<StationCargoGrouping, 4> order{GR_CARGO, GR_NEXT, GR_SOURCE, GR_DESTINATION};
	CargoDataEntry expanded, data;
	expanded.InsertOrRetrieve(PAX);
	CargoDataEntry *via = ShowStationCargo(data, expanded, order, A, true, PAX, A, B, D, 100);
	REQUIRE(via != nullptr); via->AddService({GroupID{10}}, 100);
	CHECK(via->GetNumChildren() == 0); CHECK(via->GetNumRows() == 1);
	CHECK(data.GetNumRows() == 3); CHECK(data.GetCount() == 100);
	expanded.Remove(PAX); data.Clear();
	CHECK(ShowStationCargo(data, expanded, order, A, true, PAX, A, B, D, 100) == nullptr);
	CHECK(data.GetNumRows() == 1); CHECK(data.GetCount() == 100);
	expanded.InsertOrRetrieve(PAX); data.Clear();
	CHECK(ShowStationCargo(data, expanded, order, A, false, PAX, A, B, D, 100) == nullptr);
	CHECK(data.GetNumRows() == 1); CHECK(data.GetCount() == 100);
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
	CHECK(cache.Resolve(PAX, fleet.front()->Ride()).group == GroupID::Invalid());
	CHECK_FALSE(cache.Resolve(PAX, fleet.front()->Ride()).multiple);
	for (auto it = fleet.rbegin(); it != fleet.rend(); ++it) delete *it;
}
