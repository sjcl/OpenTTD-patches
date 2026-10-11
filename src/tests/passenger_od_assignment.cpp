/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file passenger_od_assignment.cpp Demand, generation and serialization regressions. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../passenger_od.h"
#include "../station_base.h"
#include "../vehicle_base.h"
#include "../economy_base.h"
#include "../linkgraph/linkgraphjob.h"
#include "../linkgraph/demands.h"
#include "../linkgraph/mcf.h"
#include "../linkgraph/flowmapper.h"
#include "../core/backup_type.hpp"
#include "../sl/saveload.h"
#include <chrono>
#include <numeric>

extern SaveLoadVersion _sl_version;
extern NamedSaveLoadTable GetGoodsDesc();
extern NamedSaveLoadTable GetLinkGraphDesc();
extern NamedSaveLoadTable GetLinkGraphJobDesc();
extern std::vector<uint8_t> SlTestSaveLoadTable(void *, void *, const NamedSaveLoadTable &);
extern uint32_t SavePassengerOdRevision(const SlxiSubChunkInfo &, bool);
extern void LoadPassengerOdRevision(const SlxiSubChunkInfo &, uint32_t);

static constexpr CargoType PAX{0};
static constexpr StationID A{0}, B{1}, C{2}, D{3};
static constexpr Source SOURCE{Source::Invalid, SourceType::Industry};

struct OdAssignmentFixture {
	AutoRestoreBackup<CargoClasses> classes{CargoSpec::Get(PAX)->classes, CargoClasses{CargoClass::Passengers}};
	AutoRestoreBackup<uint8_t> bitnum{CargoSpec::Get(PAX)->bitnum, uint8_t{0}};
	AutoRestoreBackup<Money> payment{CargoSpec::Get(PAX)->current_payment, Money{0}};
	AutoRestoreBackup<LinkGraphSettings> settings{_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<Randomizer> random{_random, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<uint64_t> revision{_passenger_od_revision, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<PassengerOdStats> stats{_passenger_od_stats, PassengerOdStats{}};
	LinkGraph *graph = nullptr;

	OdAssignmentFixture(size_t stations = 4)
	{
		REQUIRE(Station::GetNumItems() == 0);
		REQUIRE(LinkGraph::GetNumItems() == 0);
		REQUIRE(CargoPacket::GetNumItems() == 0);
		AllocateMap(64, 64);
		_settings_game.linkgraph.passenger_od_assignment = true;
		_settings_game.linkgraph.passenger_transfer_timetables = false;
		_settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Asymmetric;
		_settings_game.linkgraph.accuracy = 16;
		_settings_game.linkgraph.demand_distance = 0;
		_settings_game.linkgraph.short_path_saturation = 100;
		_settings_game.linkgraph.recalc_time = 8;
		graph = LinkGraph::Create(PAX);
		for (size_t i = 0; i < stations; ++i) {
			Station *st = Station::CreateAtIndex(StationID{static_cast<uint16_t>(i)}, TileIndex{static_cast<uint32_t>(i)});
			st->owner = OWNER_NONE;
			auto &ge = st->goods[PAX];
			ge.link_graph = graph->index;
			ge.node = graph->AddNode(st);
			(*graph)[ge.node].SetDemand(i == 0 ? 0 : 1);
		}
	}

	~OdAssignmentFixture()
	{
		_station_pool.CleanPool();
		_cargopacket_pool.CleanPool();
		_link_graph_pool.CleanPool();
	}

	GoodsEntry &Goods() { return Station::Get(A)->goods[PAX]; }
	PassengerOdDistribution &Od() { return *Goods().data->passenger_od; }

	void Install(std::span<LinkGraphJob::DemandAnnotation> demands)
	{
		LinkGraphJob job(LinkGraphJobID::Invalid(), *graph, 1);
		job.Init();
		job[0].SetDemandAnnotations(demands);
		job.FinaliseJob();
	}
};

static PassengerOdDistribution Distribution(std::initializer_list<uint32_t> weights)
{
	PassengerOdDistribution od;
	uint16_t index = 1;
	for (uint32_t weight : weights) {
		od.entries.push_back({ StationID{index}, index, weight, 0 });
		od.total_weight += weight;
		++index;
	}
	return od;
}

static std::array<uint32_t, 4> WaitingOd(const GoodsEntry &ge)
{
	std::array<uint32_t, 4> counts{};
	for (StationCargoList::ConstIterator it = ge.data->cargo.Packets()->begin(); it != ge.data->cargo.Packets()->end(); ++it) {
		StationID id = (*it)->GetPassengerDestination();
		counts[id == StationID::Invalid() ? 0 : id.base()] += (*it)->Count();
	}
	return counts;
}

TEST_CASE("Passenger OD uses original satisfied demand in stable station order", "[passenger][od_assignment]")
{
	OdAssignmentFixture f;
	std::array<LinkGraphJob::DemandAnnotation, 3> demands{{ {3, 100, 0}, {1, 600, 599}, {2, 300, 0} }};
	f.Install(demands);
	REQUIRE(f.Goods().data->passenger_od != nullptr);
	CHECK(f.Od().total_weight == 1000);
	CHECK(f.Od().entries == Distribution({600, 300, 100}).entries);
	GameRandomSeedChecker seeds;
	auto counts = f.Od().Allocate(100);
	CHECK(counts[0] == 60);
	CHECK(counts[1] == 30);
	CHECK(counts[2] == 10);
	CHECK(seeds.Check());
}

TEST_CASE("Real CargoDist demand and both MCF passes produce final passenger destinations", "[passenger][od_assignment][linkgraph]")
{
	OdAssignmentFixture f;
	(*f.graph)[0].UpdateSupply(1000);
	for (NodeID node = 1; node < 4; ++node) {
		f.graph->UpdateEdge(0, node, 10000, 0, 100, {EdgeUpdateMode::Refresh});
		f.graph->UpdateEdge(node, 0, 10000, 0, 100, {EdgeUpdateMode::Refresh});
	}
	LinkGraphJob job(LinkGraphJobID::Invalid(), *f.graph, 1);
	job.Init();
	DemandCalculator calculator(job);
	CHECK(job.demand_matrix == nullptr);
	REQUIRE(job[0].GetDemandAnnotations().size() == 3);
	MCF1stPass first(job);
	FlowMapper(false).Run(job);
	MCF2ndPass second(job);
	FlowMapper(true).Run(job);
	std::vector<uint32_t> original;
	for (const auto &demand : job[0].GetDemandAnnotations()) {
		REQUIRE(demand.unsatisfied_demand < demand.demand);
		original.push_back(demand.demand);
	}
	job.FinaliseJob();
	REQUIRE(f.Goods().data->passenger_od != nullptr);
	REQUIRE(f.Od().entries.size() == 3);
	for (size_t i = 0; i < 3; ++i) CHECK(f.Od().entries[i].weight == original[i]);
	GenerateStationCargo(*Station::Get(A), PAX, 100, SOURCE);
	CHECK(f.Goods().CargoAvailableCount() == 100);
	CHECK(WaitingOd(f.Goods())[0] == 0);
}

TEST_CASE("Passenger OD rounding carries small demand without changing batch totals", "[passenger][od_assignment]")
{
	auto od = Distribution({90, 10});
	std::array<uint64_t, 3> totals{};
	GameRandomSeedChecker seeds;
	for (uint i = 0; i < 100; ++i) {
		auto counts = od.Allocate(1);
		CHECK(std::accumulate(counts.begin(), counts.end(), uint64_t{0}) == 1);
		for (size_t j = 0; j < 2; ++j) totals[j] += counts[j];
	}
	CHECK(totals[0] == 90);
	CHECK(totals[1] == 10);
	od = Distribution({600, 300, 100});
	totals = {};
	for (uint i = 0; i < 100; ++i) {
		auto counts = od.Allocate(i % 2 + 1);
		for (size_t j = 0; j < 3; ++j) totals[j] += counts[j];
	}
	CHECK(totals == std::array<uint64_t, 3>{90, 45, 15});
	CHECK(seeds.Check());
}

TEST_CASE("Passenger OD integer bounds and irregular batches keep bounded residuals", "[passenger][od_assignment]")
{
	auto od = Distribution({UINT32_MAX, UINT32_MAX - 1, 1});
	uint32_t state = 17;
	for (uint i = 0; i < 1000; ++i) {
		state = state * 1664525U + 1013904223U;
		uint32_t amount = i % 3 == 0 ? UINT32_MAX : state;
		auto counts = od.Allocate(amount);
		CHECK(std::accumulate(counts.begin(), counts.end(), uint64_t{0}) == amount);
		CHECK(od.Validate());
	}
	CHECK(od.Allocate(0) == std::array<uint32_t, MAX_PASSENGER_OD_DESTINATIONS>{});
}

TEST_CASE("Passenger generation splits OD while preserving next hop source and random state", "[passenger][od_assignment]")
{
	OdAssignmentFixture f;
	std::array<LinkGraphJob::DemandAnnotation, 3> demands{{ {1, 600, 0}, {2, 300, 0}, {3, 100, 0} }};
	f.Install(demands);
	f.Goods().data->flows.insert(FlowStat(A, B, 75));
	f.Goods().data->flows.find(A)->AppendShare(C, 25, false);
	Randomizer before = _random;
	StationID expected_next = f.Goods().GetVia(A);
	Randomizer expected_random = _random;
	_random = before;
	GenerateStationCargo(*Station::Get(A), PAX, 100, SOURCE);
	CHECK(_random.state[0] == expected_random.state[0]);
	CHECK(_random.state[1] == expected_random.state[1]);
	CHECK(WaitingOd(f.Goods()) == std::array<uint32_t, 4>{0, 60, 30, 10});
	CHECK(f.Goods().data->cargo.Packets()->size() == 3);
	for (StationCargoList::ConstIterator it = f.Goods().data->cargo.Packets()->begin(); it != f.Goods().data->cargo.Packets()->end(); ++it) {
		CHECK((*it)->GetFirstStation() == A);
		CHECK((*it)->GetSource() == SOURCE);
		CHECK(it.GetKey() == expected_next);
	}
	GenerateStationCargo(*Station::Get(A), PAX, 100, SOURCE);
	CHECK(f.Goods().CargoAvailableCount() == 200);
	CHECK(WaitingOd(f.Goods()) == std::array<uint32_t, 4>{0, 120, 60, 20});
}

TEST_CASE("Passenger OD fallback preserves legacy generation when disabled manual freight or absent", "[passenger][od_assignment]")
{
	OdAssignmentFixture f;
	std::array<LinkGraphJob::DemandAnnotation, 1> demand{{ {1, 100, 0} }};
	f.Install(demand);
	SECTION("Disabled") { _settings_game.linkgraph.passenger_od_assignment = false; }
	SECTION("Manual") { _settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Manual; }
	SECTION("Freight") { CargoSpec::Get(PAX)->classes = CargoClasses{CargoClass::Bulk}; }
	SECTION("Absent distribution") { f.Goods().data->passenger_od.reset(); }
	SECTION("Empty distribution") { f.Od().entries.clear(); }
	SECTION("Removed destination") { delete Station::Get(B); }
	SECTION("Changed membership") { Station::Get(B)->goods[PAX].node = INVALID_NODE; }
	SECTION("Mismatched node to station mapping") { f.Od().entries[0].node = 2; }
	f.Goods().data->flows.insert(FlowStat(A, C, 1));
	Randomizer before = _random;
	StationCargoList legacy;
	legacy.Append(CargoPacket::Create(A, 100, SOURCE), f.Goods().GetVia(A));
	Randomizer expected = _random;
	_random = before;
	GenerateStationCargo(*Station::Get(A), PAX, 100, SOURCE);
	CHECK(_random.state[0] == expected.state[0]);
	CHECK(_random.state[1] == expected.state[1]);
	CHECK(f.Goods().CargoAvailableCount() == legacy.AvailableCount());
	CHECK(f.Goods().data->cargo.Packets()->size() == legacy.Packets()->size());
	CHECK(WaitingOd(f.Goods())[0] == 100);
}

struct OdAssignmentVehicle : Vehicle {
	OdAssignmentVehicle() : Vehicle(VehicleID::Invalid(), VehicleType::Train) { this->last_station_visited = B; }
	bool IsPrimaryVehicle() const override { return true; }
};

TEST_CASE("OD packet splitting changes later per-packet CargoDist draws without changing cargo total", "[passenger][od_assignment]")
{
	OdAssignmentFixture f;
	OdAssignmentVehicle legacy, split;
	CargoPayment legacy_payment(CargoPaymentID::Invalid(), &legacy), split_payment(CargoPaymentID::Invalid(), &split);
	GoodsEntry interchange;
	interchange.CreateData().flows.insert(FlowStat(A, C, 50));
	interchange.data->flows.find(A)->AppendShare(D, 50, false);
	std::array next{C};
	auto add = [&](VehicleCargoList &list, uint16_t amount, StationID destination) {
		CargoPacket *cp = CargoPacket::Create(A, amount, SOURCE);
		cp->UpdateLoadingTile(TileIndex{0});
		if (destination != StationID::Invalid()) REQUIRE(cp->SetPassengerDestination(PAX, destination));
		list.Append(cp);
	};
	add(legacy.cargo, 100, StationID::Invalid());
	add(split.cargo, 60, B);
	add(split.cargo, 30, C);
	add(split.cargo, 10, D);
	_random.SetSeed(17);
	Randomizer before = _random;
	legacy.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &interchange, PAX, &legacy_payment, TileIndex{1});
	Randomizer legacy_random = _random;
	_random = before;
	split.cargo.Stage(false, B, next, OrderUnloadType::UnloadIfPossible, &interchange, PAX, &split_payment, TileIndex{1});
	CHECK((_random.state[0] != legacy_random.state[0] || _random.state[1] != legacy_random.state[1]));
	CHECK(legacy.cargo.TotalCount() == 100);
	CHECK(split.cargo.TotalCount() == 100);
	CHECK(split.cargo.Packets()->size() == 3);
}

TEST_CASE("Passenger OD topology identity rejects changed jobs and stale distributions", "[passenger][od_assignment][linkgraph]")
{
	OdAssignmentFixture f;
	std::array<LinkGraphJob::DemandAnnotation, 1> demand{{ {1, 100, 0} }};
	f.graph->UpdateEdge(0, 1, 100, 0, 100, {EdgeUpdateMode::Refresh});
	f.Install(demand);
	LinkGraphJob pending(LinkGraphJobID::Invalid(), *f.graph, 1);
	pending.Init();
	pending[0].SetDemandAnnotations(demand);
	SECTION("Edge deletion models a component split") { f.graph->RemoveEdge(0, 1); }
	SECTION("Mutable edge expiration also invalidates") {
		f.graph->MutableIterateEdgesFromNode(0, [](auto) { return LinkGraph::EdgeIterationResult::EraseEdge; });
	}
	SECTION("Station deletion and ID reuse") {
		delete Station::Get(B);
		Station *st = Station::CreateAtIndex(B, TileIndex{1});
		st->goods[PAX].link_graph = f.graph->index;
		st->goods[PAX].node = f.graph->AddNode(st);
	}
	SECTION("Merge into a surviving graph") {
		delete Station::Get(D);
		Station *st = Station::CreateAtIndex(D, TileIndex{3});
		LinkGraph *other = LinkGraph::Create(PAX);
		st->goods[PAX].link_graph = other->index;
		st->goods[PAX].node = other->AddNode(st);
		f.graph->Merge(other);
	}
	SECTION("Settings changed to manual and back") { ClearPassengerOdDistributions(); }
	pending.FinaliseJob();
	if (f.Goods().data != nullptr && f.Goods().data->passenger_od != nullptr) CHECK_FALSE(ValidatePassengerOd(*Station::Get(A), PAX, f.Od()));
	GenerateStationCargo(*Station::Get(A), PAX, 10, SOURCE);
	CHECK(WaitingOd(f.Goods())[0] == 10);
	CHECK(f.Goods().data->passenger_od == nullptr);
	/* The next calculation may produce a valid distribution using the new topology. */
	f.Install(demand);
	CHECK(f.Goods().data->passenger_od != nullptr);
}

TEST_CASE("Passenger OD recalculation retains identical residuals and resets changed weights", "[passenger][od_assignment]")
{
	OdAssignmentFixture f;
	std::array<LinkGraphJob::DemandAnnotation, 2> demand{{ {1, 90, 0}, {2, 10, 0} }};
	f.Install(demand);
	f.Od().Allocate(1);
	auto previous = f.Od();
	f.Install(demand);
	CHECK(f.Od() == previous);
	demand[0].demand = 80;
	f.Install(demand);
	for (auto &entry : f.Od().entries) CHECK(entry.credit == 0);
}

TEST_CASE("Passenger OD never includes wholly unsatisfied or nonpositive demands", "[passenger][od_assignment]")
{
	OdAssignmentFixture f;
	std::array<LinkGraphJob::DemandAnnotation, 3> demand{{ {1, 90, 90}, {2, 0, 0}, {3, 10, 9} }};
	f.Install(demand);
	REQUIRE(f.Od().entries.size() == 1);
	CHECK(f.Od().entries[0].destination == D);
	CHECK(f.Od().total_weight == 10);
}

TEST_CASE("Passenger OD save table restores distribution residuals and the next generated passengers", "[passenger][od_assignment][saveload]")
{
	OdAssignmentFixture f;
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions();
	_sl_xv_feature_versions = _sl_xv_feature_static_versions;
	std::array<LinkGraphJob::DemandAnnotation, 3> demand{{ {1, 600, 0}, {2, 300, 0}, {3, 100, 0} }};
	f.Install(demand);
	f.Od().Allocate(7);
	GoodsEntry loaded;
	loaded.CreateData();
	auto bytes = SlTestSaveLoadTable(&f.Goods(), &loaded, GetGoodsDesc());
	REQUIRE_FALSE(bytes.empty());
	REQUIRE(loaded.data->passenger_od != nullptr);
	CHECK(*loaded.data->passenger_od == f.Od());
	CHECK(loaded.link_graph == f.Goods().link_graph);
	CHECK(loaded.node == f.Goods().node);
	GenerateStationCargo(*Station::Get(A), PAX, 13, SOURCE);
	auto expected = WaitingOd(f.Goods());
	auto expected_state = f.Od();
	f.Goods().data = std::move(loaded.data);
	AfterLoadPassengerOd();
	GenerateStationCargo(*Station::Get(A), PAX, 13, SOURCE);
	CHECK(WaitingOd(f.Goods()) == expected);
	CHECK(f.Od() == expected_state);
	LinkGraph graph_copy(LinkGraphID::Invalid());
	SlTestSaveLoadTable(f.graph, &graph_copy, GetLinkGraphDesc());
	CHECK(graph_copy.PassengerOdRevision() == f.graph->PassengerOdRevision());
	LinkGraphJob pending(LinkGraphJobID::Invalid(), *f.graph, 1), loaded_job(LinkGraphJobID::Invalid());
	SlTestSaveLoadTable(&pending, &loaded_job, GetLinkGraphJobDesc());
	CHECK(loaded_job.Graph().PassengerOdRevision() == f.graph->PassengerOdRevision());
}

TEST_CASE("Passenger OD legacy saves omit distribution and load safely", "[passenger][od_assignment][saveload]")
{
	OdAssignmentFixture f;
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions();
	_sl_xv_feature_versions = _sl_xv_feature_static_versions;
	_sl_xv_feature_versions[XSLFI_PASSENGER_OD_ASSIGNMENT] = 0;
	std::array<LinkGraphJob::DemandAnnotation, 1> demand{{ {1, 100, 0} }};
	f.Install(demand);
	GoodsEntry loaded;
	loaded.CreateData();
	SlTestSaveLoadTable(&f.Goods(), &loaded, GetGoodsDesc());
	CHECK(loaded.data->passenger_od == nullptr);
	LinkGraph old_graph(LinkGraphID::Invalid());
	SlTestSaveLoadTable(f.graph, &old_graph, GetLinkGraphDesc());
	CHECK(old_graph.PassengerOdRevision() == 0);
	LinkGraphID index = f.graph->index;
	delete f.graph;
	f.graph = LinkGraph::CreateAtIndex(index);
	SlTestSaveLoadTable(&old_graph, f.graph, GetLinkGraphDesc());
	f.Goods().data = std::move(loaded.data);
	AfterLoadPassengerOd();
	CHECK_FALSE(_settings_game.linkgraph.passenger_od_assignment);
	GenerateStationCargo(*Station::Get(A), PAX, 10, SOURCE);
	CHECK(WaitingOd(f.Goods())[0] == 10);
}

struct PassengerOdRevisionTestState { uint64_t revision = 0; };

/** Exercise the actual SLXI extra-data callbacks with the isolated serializer's buffer. */
struct PassengerOdRevisionTestHandler final : public TypedSaveLoadStructHandler<PassengerOdRevisionTestHandler, PassengerOdRevisionTestState> {
	NamedSaveLoadTable GetDescription() const override
	{
		static const NamedSaveLoad fields[]{ NSL("revision", SLE_VAR(PassengerOdRevisionTestState, revision, SLE_UINT64)) };
		return fields;
	}
	void Save(PassengerOdRevisionTestState *) const override
	{
		SlxiSubChunkInfo info{};
		CHECK(SavePassengerOdRevision(info, true) == 8);
		CHECK(SavePassengerOdRevision(info, false) == 8);
	}
	void Load(PassengerOdRevisionTestState *state) const override
	{
		SlxiSubChunkInfo info{};
		_passenger_od_revision = 0; // A newly joining client has no previous identity counter.
		LoadPassengerOdRevision(info, 8);
		state->revision = _passenger_od_revision;
	}
};

TEST_CASE("Passenger OD identity counter survives SLXI data and prevents unsigned wrap", "[passenger][od_assignment][saveload]")
{
	OdAssignmentFixture f;
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions();
	_sl_xv_feature_versions = _sl_xv_feature_static_versions;
	static const NamedSaveLoad fields[]{ NSLT_STRUCT<PassengerOdRevisionTestHandler>("identity") };
	PassengerOdRevisionTestState saved, loaded;
	SECTION("Restored counter produces the same next graph identity") {
		_passenger_od_revision = 12345;
		SlTestSaveLoadTable(&saved, &loaded, fields);
		CHECK(loaded.revision == 12345);
		CHECK(NextPassengerOdRevision() == 12346);
	}
	SECTION("An exhausted saved counter is rejected") {
		_passenger_od_revision = UINT64_MAX;
		CHECK_THROWS(SlTestSaveLoadTable(&saved, &loaded, fields));
		CHECK_THROWS(NextPassengerOdRevision());
	}
}

TEST_CASE("Passenger OD save validation rejects corrupt counts weights ordering and residuals", "[passenger][od_assignment][saveload]")
{
	OdAssignmentFixture f;
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions();
	_sl_xv_feature_versions = _sl_xv_feature_static_versions;
	std::array<LinkGraphJob::DemandAnnotation, 2> demand{{ {1, 90, 0}, {2, 10, 0} }};
	f.Install(demand);
	SECTION("Zero weight") { f.Od().entries[0].weight = 0; }
	SECTION("Total mismatch") { ++f.Od().total_weight; }
	SECTION("Residual overflow") { f.Od().entries[0].credit = INT64_MAX; }
	SECTION("Residual sum mismatch") { f.Od().entries[0].credit = 1; }
	SECTION("Duplicate destination") { f.Od().entries[1].destination = B; }
	SECTION("Oversized list") { f.Od().entries.resize(MAX_PASSENGER_OD_DESTINATIONS + 1); }
	GoodsEntry loaded;
	loaded.CreateData();
	CHECK_THROWS(SlTestSaveLoadTable(&f.Goods(), &loaded, GetGoodsDesc()));
}

TEST_CASE("Passenger OD load removes invalid destinations and ignores manual and freight data", "[passenger][od_assignment][saveload]")
{
	OdAssignmentFixture f;
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	_sl_xv_feature_versions[XSLFI_PASSENGER_OD_ASSIGNMENT] = 1;
	std::array<LinkGraphJob::DemandAnnotation, 1> demand{{ {1, 100, 0} }};
	f.Install(demand);
	SECTION("Missing station") { f.Od().entries[0].destination = StationID{100}; }
	SECTION("Deleted station") { delete Station::Get(B); }
	SECTION("Manual") { _settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Manual; }
	SECTION("Freight") { CargoSpec::Get(PAX)->classes = CargoClasses{CargoClass::Bulk}; }
	AfterLoadPassengerOd();
	CHECK(f.Goods().data->passenger_od == nullptr);
}

TEST_CASE("Passenger OD bounds destination memory packet growth and exhausted pool fallback", "[passenger][od_assignment][performance]")
{
	OdAssignmentFixture f(MAX_PASSENGER_OD_DESTINATIONS + 2);
	std::vector<LinkGraphJob::DemandAnnotation> demand;
	for (NodeID i = 1; i <= MAX_PASSENGER_OD_DESTINATIONS; ++i) demand.push_back({i, 1, 0});
	f.Install(demand);
	REQUIRE(f.Od().entries.size() == MAX_PASSENGER_OD_DESTINATIONS);
	SECTION("Packet bound") {
		GenerateStationCargo(*Station::Get(A), PAX, 512, SOURCE);
		CHECK(f.Goods().CargoAvailableCount() == 512);
		CHECK(f.Goods().data->cargo.Packets()->size() == MAX_PASSENGER_OD_PACKETS);
		CHECK(_passenger_od_stats.fallback_passengers == 512 - 2 * (MAX_PASSENGER_OD_PACKETS - 1));
	}
	SECTION("Only the legacy packet fits the pool") {
		const size_t original = _cargopacket_pool.items;
		const size_t fake = CargoPacketPool::MAX_SIZE - 1;
		_cargopacket_pool.items = fake;
		auto restore = scope_guard([&]() { _cargopacket_pool.items = original + (_cargopacket_pool.items - fake); });
		GenerateStationCargo(*Station::Get(A), PAX, 100, SOURCE);
		CHECK(f.Goods().CargoAvailableCount() == 100);
		CHECK(f.Goods().data->cargo.Packets()->size() == 1);
		CHECK(_passenger_od_stats.fallback_passengers == 100);
	}
	SECTION("Over the destination limit uses only unassigned cargo") {
		demand.push_back({MAX_PASSENGER_OD_DESTINATIONS + 1, 1, 0});
		f.Install(demand);
		CHECK((f.Goods().data == nullptr || f.Goods().data->passenger_od == nullptr));
		GenerateStationCargo(*Station::Get(A), PAX, 100, SOURCE);
		CHECK(f.Goods().data->cargo.Packets()->size() == 1);
		CHECK(_passenger_od_stats.fallback_passengers == 100);
	}
	SECTION("Batch time depends on destinations rather than passenger count") {
		auto od = f.Od();
		const auto start = std::chrono::steady_clock::now();
		for (uint i = 0; i < 10000; ++i) {
			auto counts = od.Allocate(UINT32_MAX);
			REQUIRE(std::accumulate(counts.begin(), counts.end(), uint64_t{0}) == UINT32_MAX);
		}
		const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		WARN("OD benchmark: " << us << " us / 10000 batches; entries=" << od.entries.size() << "; bytes=" << sizeof(od) + od.entries.capacity() * sizeof(PassengerOdEntry));
	}
}
