/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file passenger_od.cpp Final passenger destination lifecycle and save compatibility regressions. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../cargopacket.h"
#include "../station_base.h"
#include "../vehicle_base.h"
#include "../economy_base.h"
#include "../settings_type.h"
#include "../core/backup_type.hpp"
#include "../core/random_func.hpp"
#include "../sl/saveload.h"

extern SaveLoadVersion _sl_version;
extern btree::btree_map<uint64_t, Money> _cargo_packet_deferred_payments;
extern std::vector<uint8_t> SlTestSaveLoadTable(void *saved_object, void *loaded_object, const NamedSaveLoadTable &table);

static constexpr StationID A{0}, B{1}, C{2}, D{3};
static constexpr CargoType PAX{0};
using Action = VehicleCargoList::MoveToAction;

/** Real station/packet pools, with no engine or graphical setup. Lists must die before this fixture. */
struct PassengerOdFixture {
	AutoRestoreBackup<CargoClasses> classes{CargoSpec::Get(PAX)->classes, CargoClasses{CargoClass::Passengers}};
	AutoRestoreBackup<uint8_t> bitnum{CargoSpec::Get(PAX)->bitnum, uint8_t{0}};
	AutoRestoreBackup<Money> payment{CargoSpec::Get(PAX)->current_payment, Money{0}};
	AutoRestoreBackup<LinkGraphSettings> settings{_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<Randomizer> random{_random, AutoRestoreBackupNoNewValueTag{}};

	PassengerOdFixture()
	{
		REQUIRE(Station::GetNumItems() == 0);
		AllocateMap(64, 64);
		for (StationID id : {A, B, C, D}) Station::CreateAtIndex(id, TileIndex{0})->owner = OWNER_NONE;
		_settings_game.linkgraph.passenger_transfer_timetables = false;
		_settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Asymmetric;
	}

	~PassengerOdFixture() { _station_pool.CleanPool(); }

	CargoPacket *Packet(StationID destination = StationID::Invalid(), uint16_t count = 10)
	{
		auto cp = std::unique_ptr<CargoPacket>(CargoPacket::Create(count, 7, A, TileIndex{0}, Money{100}));
		if (destination != StationID::Invalid()) REQUIRE(cp->SetPassengerDestination(PAX, destination));
		return cp.release();
	}
};

static void CheckOd(const VehicleCargoList &list, StationID destination)
{
	for (const CargoPacket *cp : *list.Packets()) CHECK(cp->GetPassengerDestination() == destination);
}

static void CheckOd(const StationCargoList &list, StationID destination)
{
	for (StationCargoList::ConstIterator it = list.Packets()->begin(); it != list.Packets()->end(); ++it) {
		CHECK((*it)->GetPassengerDestination() == destination);
	}
}

static const SaveLoad &OdField()
{
	NamedSaveLoadTable fields = GetCargoPacketDesc();
	auto it = std::find_if(fields.begin(), fields.end(), [](const NamedSaveLoad &field) {
		return std::string_view(field.name) == "passenger_destination";
	});
	REQUIRE(it != fields.end());
	return it->save_load;
}

/** Temporary vehicle provides real cargo staging/payment without engine/GUI setup. */
struct OdVehicle : Vehicle {
	OdVehicle() : Vehicle(VehicleID::Invalid(), VehicleType::Train) { this->last_station_visited = B; }
	bool IsPrimaryVehicle() const override { return true; }
};

TEST_CASE("Passenger OD defaults to unassigned in every creation path", "[passenger][od]")
{
	PassengerOdFixture fixture;
	auto cp = std::unique_ptr<CargoPacket>(CargoPacket::Create(A, 10, Source{Source::Invalid, SourceType::Industry}));
	auto legacy = std::unique_ptr<CargoPacket>(fixture.Packet());
	auto loaded = std::unique_ptr<CargoPacket>(CargoPacket::Create());
	CHECK(cp->GetPassengerDestination() == StationID::Invalid());
	CHECK(legacy->GetPassengerDestination() == StationID::Invalid());
	CHECK(loaded->GetPassengerDestination() == StationID::Invalid());
	cp->SetNextHop(B);
	CHECK(cp->GetPassengerDestination() == StationID::Invalid());
	CHECK(cp->GetFirstStation() == A);
	CHECK(cp->SetPassengerDestination(PAX, StationID::Invalid()));
}

TEST_CASE("Passenger OD assignment validates cargo and stations and cannot replace an assigned OD", "[passenger][od]")
{
	PassengerOdFixture fixture;
	auto cp = std::unique_ptr<CargoPacket>(fixture.Packet());
	GameRandomSeedChecker seeds;
	CHECK_FALSE(cp->SetPassengerDestination(INVALID_CARGO, D));
	CHECK_FALSE(cp->SetPassengerDestination(PAX, StationID{100}));
	CHECK_FALSE(cp->SetPassengerDestination(PAX, NEW_STATION));
	CHECK_FALSE(cp->SetPassengerDestination(PAX, ADJACENT_STATION));
	CHECK(cp->GetPassengerDestination() == StationID::Invalid());
	SECTION("Freight cannot acquire passenger OD") {
		CargoSpec::Get(PAX)->classes = CargoClasses{CargoClass::Bulk};
		CHECK_FALSE(cp->SetPassengerDestination(PAX, D));
		CHECK(cp->GetPassengerDestination() == StationID::Invalid());
	}
	SECTION("Unused cargo specifications are rejected") {
		CargoSpec::Get(PAX)->bitnum = INVALID_CARGO_BITNUM;
		CHECK_FALSE(cp->SetPassengerDestination(PAX, D));
	}
	SECTION("Passenger assignment is explicit and idempotent") {
		REQUIRE(cp->SetPassengerDestination(PAX, D));
		CHECK(cp->SetPassengerDestination(PAX, D));
		CHECK_FALSE(cp->SetPassengerDestination(PAX, C));
		CHECK_FALSE(cp->SetPassengerDestination(PAX, StationID::Invalid()));
		cp->SetNextHop(B);
		CHECK(cp->GetPassengerDestination() == D);
		CHECK(cp->GetFirstStation() == A);
		CHECK(cp->GetNextHop() == B);
	}
	CHECK(seeds.Check());
}

TEST_CASE("Passenger OD survives splitting merging and partial removal with cargo accounting", "[passenger][od]")
{
	PassengerOdFixture fixture;
	REQUIRE(CargoPacket::GetNumItems() == 0);
	/* Other fixtures may clean pools independently of this global credit table. */
	AutoRestoreBackup credits{_cargo_packet_deferred_payments, btree::btree_map<uint64_t, Money>{}};
	auto cp = std::unique_ptr<CargoPacket>(fixture.Packet(D));
	cp->SetNextHop(B);
	cp->UpdateLoadingTile(TileIndex{0});
	cp->RegisterDeferredCargoPayment(CompanyID{0}, VehicleType::Train, Money{50});
	GameRandomSeedChecker seeds;
	auto split = std::unique_ptr<CargoPacket>(cp->Split(4));
	REQUIRE(split != nullptr);
	CHECK(split->GetPassengerDestination() == D);
	CHECK(cp->GetPassengerDestination() == D);
	CHECK(split->Count() == 4);
	CHECK(cp->Count() == 6);
	CHECK(split->GetFeederShare() == 40);
	CHECK(cp->GetFeederShare() == 60);
	CHECK(split->GetPeriodsInTransit() == 7);
	CHECK(split->GetFirstStation() == A);
	CHECK(split->GetNextHop() == B);
	CHECK(split->GetDistance(TileIndex{0}) == cp->GetDistance(TileIndex{0}));
	CHECK(CargoPacket::ValidateDeferredCargoPayments());
	REQUIRE(cp->Merge(split.get()));
	split.release();
	CHECK(cp->Count() == 10);
	CHECK(cp->GetFeederShare() == 100);
	CHECK(CargoPacket::ValidateDeferredCargoPayments());
	cp->Reduce(3);
	CHECK(cp->Count() == 7);
	CHECK(cp->GetFeederShare() == 70);
	CHECK(cp->GetPeriodsInTransit() == 7);
	CHECK(cp->GetPassengerDestination() == D);
	cp->UpdateUnloadingTile(TileIndex{1});
	CHECK(cp->GetPassengerDestination() == D);
	CHECK(seeds.Check());
}

TEST_CASE("Passenger OD is a merge invariant for both station and vehicle cargo lists", "[passenger][od]")
{
	PassengerOdFixture fixture;
	StationID left = GENERATE(StationID::Invalid(), C, D);
	StationID right = GENERATE(StationID::Invalid(), C, D);
	auto cp1 = std::unique_ptr<CargoPacket>(fixture.Packet(left));
	auto cp2 = std::unique_ptr<CargoPacket>(fixture.Packet(right));
	CHECK(StationCargoList::AreMergable(cp1.get(), cp2.get()) == (left == right));
	CHECK(VehicleCargoList::AreMergable(cp1.get(), cp2.get()) == (left == right));
	SECTION("Station append merges exactly matching destinations") {
		StationCargoList list{};
		list.Append(cp1.release(), B);
		list.Append(cp2.release(), B);
		CHECK(list.Packets()->size() == (left == right ? 1 : 2));
		CHECK(list.AvailableCount() == 20);
		CHECK(list.CargoPeriodsInTransit() == 140);
	}
	SECTION("Vehicle append merges exactly matching destinations") {
		VehicleCargoList list{};
		list.Append(cp1.release());
		list.Append(cp2.release());
		CHECK(list.Packets()->size() == (left == right ? 1 : 2));
		CHECK(list.TotalCount() == 20);
		CHECK(list.GetFeederShare() == 200);
		CHECK(list.CargoPeriodsInTransit() == 140);
	}
	SECTION("Direct merge also refuses different destinations without deleting either packet") {
		CHECK(cp1->Merge(cp2.get()) == (left == right));
		if (left == right) {
			cp2.release();
			CHECK(cp1->Count() == 20);
		} else {
			CHECK(cp1->Count() == 10);
			CHECK(cp2->Count() == 10);
			CHECK(cp1->GetFeederShare() == 100);
			CHECK(cp2->GetPassengerDestination() == right);
		}
	}
}

TEST_CASE("Passenger OD survives reservation return loading vehicle moves and station transfer", "[passenger][od]")
{
	PassengerOdFixture fixture;
	StationCargoList station{}, interchange{};
	VehicleCargoList vehicle{};
	OdVehicle other_vehicle;
	VehicleCargoList &other = other_vehicle.cargo;
	CargoPayment transfer_payment(CargoPaymentID::Invalid(), &other_vehicle);
	std::array next{B};
	station.Append(fixture.Packet(D), B);
	REQUIRE(station.Reserve(6, &vehicle, next, TileIndex{0}) == 6);
	CHECK(station.ReservedCount() == 6);
	CHECK(vehicle.ReservedCount() == 6);
	CheckOd(station, D);
	CheckOd(vehicle, D);
	REQUIRE(vehicle.Return(2, &station, B, TileIndex{0}) == 2);
	CHECK(station.ReservedCount() == 4);
	CheckOd(station, D);
	CheckOd(vehicle, D);
	REQUIRE(station.Load(4, &vehicle, next, TileIndex{0}) == 4);
	CHECK(station.ReservedCount() == 0);
	CHECK(vehicle.StoredCount() == 4);
	REQUIRE(station.Load(2, &vehicle, next, TileIndex{0}) == 2);
	CheckOd(station, D);
	CheckOd(vehicle, D);
	REQUIRE(vehicle.Shift(3, &other) == 3);
	CheckOd(vehicle, D);
	CheckOd(other, D);
	GoodsEntry ge;
	ge.CreateData().flows.insert(FlowStat(A, C, 100));
	std::array other_next{D};
	other.Stage(false, B, other_next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &transfer_payment, TileIndex{1});
	REQUIRE(other.ActionCount(Action::Transfer) == 3);
	REQUIRE(other.Unload(2, &interchange, PAX, nullptr, TileIndex{1}) == 2);
	CheckOd(other, D);
	CheckOd(interchange, D);
	std::array onward{C};
	REQUIRE(interchange.Load(1, &vehicle, onward, TileIndex{1}) == 1);
	CheckOd(interchange, D);
	CheckOd(vehicle, D);
	REQUIRE(station.Truncate(1) == 1);
	REQUIRE(vehicle.Truncate(1) == 1);
	CheckOd(station, D);
	CheckOd(vehicle, D);
	CHECK(station.TotalCount() + interchange.TotalCount() + vehicle.TotalCount() + other.TotalCount() == 8);
}

TEST_CASE("Passenger OD survives all station and vehicle reroute variants", "[passenger][od]")
{
	PassengerOdFixture fixture;
	GoodsEntry ge;
	ge.CreateData().flows.insert(FlowStat(A, C, 100));
	SECTION("Station reroute and reroute from source include partial movement") {
		StationCargoList list{}, dest{};
		list.Append(fixture.Packet(D), B);
		REQUIRE(list.Reroute(4, &dest, B, A, &ge) == 4);
		CHECK(dest.AvailableViaCount(C) == 4);
		REQUIRE(list.RerouteFromSource(3, &list, A, B, A, &ge) == 3);
		CHECK(list.AvailableViaCount(C) == 3);
		CheckOd(list, D);
		CheckOd(dest, D);
	}
	SECTION("Vehicle reroute and reroute from source include partial movement") {
		VehicleCargoList list{}, dest{};
		CargoPacket *cp = fixture.Packet(D);
		cp->UpdateLoadingTile(TileIndex{0});
		cp->SetNextHop(B);
		list.Append(cp, Action::Transfer);
		REQUIRE(list.Reroute(4, &dest, B, A, &ge) == 4);
		REQUIRE(list.RerouteFromSource(3, &list, A, B, A, &ge) == 3);
		CHECK(list.TotalCount() == 6);
		CHECK(dest.TotalCount() == 4);
		CheckOd(list, D);
		CheckOd(dest, D);
	}
}

TEST_CASE("Station deletion clears passenger destination without changing an unrelated OD", "[passenger][od]")
{
	PassengerOdFixture fixture;
	StationCargoList station{};
	VehicleCargoList vehicle{};
	station.Append(fixture.Packet(D), B);
	vehicle.Append(fixture.Packet(D));
	auto unrelated = std::unique_ptr<CargoPacket>(fixture.Packet(C));
	SECTION("Deleting the origin preserves the final destination") {
		delete Station::Get(A);
		CheckOd(station, D);
		CheckOd(vehicle, D);
		CHECK(unrelated->GetFirstStation() == StationID::Invalid());
		CHECK(unrelated->GetPassengerDestination() == C);
	}
	SECTION("Deleting the final station clears both cargo lists before ID reuse") {
		delete Station::Get(D);
		CheckOd(station, StationID::Invalid());
		CheckOd(vehicle, StationID::Invalid());
		CHECK(unrelated->GetPassengerDestination() == C);
		Station::CreateAtIndex(D, TileIndex{0});
		CheckOd(station, StationID::Invalid());
		CheckOd(vehicle, StationID::Invalid());
		CHECK(vehicle.Packets()->front()->GetFirstStation() == A);
	}
}

TEST_CASE("Passenger OD table serialisation round trips and validates loaded station IDs", "[passenger][od][saveload]")
{
	PassengerOdFixture fixture;
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions();
	_sl_xv_feature_versions = _sl_xv_feature_static_versions;
	REQUIRE(_sl_xv_feature_versions[XSLFI_PASSENGER_DESTINATION] == 1);
	const SaveLoad &field = OdField();
	CHECK(field.conv.file == SLE_FILE_U16);
	CHECK(field.conv.mem == SLE_VAR_U16);
	CHECK(field.ext_feature_test.IsFeaturePresent(_sl_xv_feature_versions, SAVEGAME_VERSION, SL_MIN_VERSION, SL_MAX_VERSION));
	auto cp = std::unique_ptr<CargoPacket>(fixture.Packet(D));
	StationID expected = D;
	SECTION("Valid OD survives repeated save load and an absent passenger via feature") {
		_sl_xv_feature_versions[XSLFI_PASSENGER_VIA] = 0;
	}
	SECTION("Invalid OD round trips") {
		WriteValue(GetVariableAddress(cp.get(), field), field.conv.mem, StationID::Invalid().base());
		expected = StationID::Invalid();
	}
	SECTION("Nonexistent station is sanitised after load") {
		WriteValue(GetVariableAddress(cp.get(), field), field.conv.mem, 100);
		expected = StationID::Invalid();
	}
	SECTION("Out of range station is sanitised after load") {
		WriteValue(GetVariableAddress(cp.get(), field), field.conv.mem, NEW_STATION.base());
		expected = StationID::Invalid();
	}
	SECTION("A removed destination is sanitised after load") {
		/* Pool cleaning emulates a loaded station pool where the referenced station is missing. */
		_station_pool.CleanPool();
		Station::CreateAtIndex(A, TileIndex{0});
		expected = StationID::Invalid();
	}
	SECTION("Old tables omit OD even when passenger via is present") {
		_sl_xv_feature_versions[XSLFI_PASSENGER_DESTINATION] = 0;
		CHECK_FALSE(field.ext_feature_test.IsFeaturePresent(_sl_xv_feature_versions, SAVEGAME_VERSION, SL_MIN_VERSION, SL_MAX_VERSION));
		expected = StationID::Invalid();
	}
	std::vector<uint8_t> previous_bytes;
	for (uint iteration = 0; iteration < 3; ++iteration) {
		auto loaded = std::unique_ptr<CargoPacket>(CargoPacket::Create());
		auto bytes = SlTestSaveLoadTable(cp.get(), loaded.get(), GetCargoPacketDesc());
		REQUIRE_FALSE(bytes.empty());
		/* After sanitising the first load, the entire serialized packet must be stable. */
		if (iteration == 2) CHECK(bytes == previous_bytes);
		previous_bytes = std::move(bytes);
		if (SlXvIsFeatureMissing(XSLFI_PASSENGER_DESTINATION)) CHECK(loaded->GetPassengerDestination() == StationID::Invalid());
		CargoPacket::AfterLoad();
		CHECK(loaded->GetPassengerDestination() == expected);
		CHECK(loaded->Count() == 10);
		CHECK(loaded->GetFeederShare() == 100);
		CHECK(loaded->GetPeriodsInTransit() == 7);
		cp = std::move(loaded);
	}
}

TEST_CASE("CargoDist staging ignores OD for boarding transfer and delivery decisions", "[passenger][od]")
{
	PassengerOdFixture fixture;
	StationID destination = GENERATE(StationID::Invalid(), B, C, D);
	OdVehicle vehicle;
	CargoPayment payment(CargoPaymentID::Invalid(), &vehicle);
	GoodsEntry ge;
	ge.CreateData().flows.insert(FlowStat(A, C, 100));
	CargoPacket *cp = fixture.Packet(destination);
	cp->UpdateLoadingTile(TileIndex{0});
	vehicle.cargo.Append(cp);
	std::array next{C};
	SECTION("Matching CargoDist next hop keeps cargo on board") {
		vehicle.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0});
		CHECK(vehicle.cargo.ActionCount(Action::Keep) == 10);
	}
	SECTION("Different next hop transfers even if OD is the current station") {
		std::array other_next{D};
		vehicle.cargo.Stage(false, B, other_next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0});
		CHECK(vehicle.cargo.ActionCount(Action::Transfer) == 10);
		CHECK(vehicle.cargo.Packets()->front()->GetNextHop() == C);
	}
	SECTION("CargoDist acceptance delivers even if OD is another station") {
		vehicle.cargo.Stage(true, C, next, OrderUnloadType::UnloadIfPossible, &ge, PAX, &payment, TileIndex{0});
		CHECK(vehicle.cargo.ActionCount(Action::Deliver) == 10);
	}
	CheckOd(vehicle.cargo, destination);
}
