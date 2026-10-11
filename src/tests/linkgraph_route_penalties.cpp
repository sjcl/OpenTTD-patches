/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file linkgraph_route_penalties.cpp Regression tests for imported route costs and transfer penalties. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../linkgraph/linkgraphjob.h"
#include "../linkgraph/mcf.h"
#include "../order_base.h"
#include "../date_func.h"
#include "../core/backup_type.hpp"
#include "../linkgraph/flowmapper.h"
#include "../order_backup.h"
#include "../vehicle_base.h"
#include "../network/network_type.h"
#include "../train.h"
#include "../station_map.h"
#include "../rail_map.h"
#include "../company_func.h"
#include <chrono>

/** Restore the temporary map even when an assertion aborts a test section. */
struct RoutePenaltyTestMap {
	RoutePenaltyTestMap(uint size = 64) { AllocateMap(size, size); }
	~RoutePenaltyTestMap() { DeallocateMap(); }
};

TEST_CASE("CargoDist route costs survive copying shared orders", "[linkgraph][route_penalties]")
{
	OrderList original(OrderListID::Invalid()), copy(OrderListID::Invalid());
	CHECK(original.GetCargoDistCostScale() == 100);
	CHECK(original.GetCargoDistCostAdd() == 0);
	original.SetCargoDistCostScale(150);
	original.SetCargoDistCostAdd(3);
	copy.CopyOrderListContents(original);
	CHECK(copy.GetCargoDistCostScale() == 150);
	CHECK(copy.GetCargoDistCostAdd() == 3);
}

TEST_CASE("LinkGraph route records retain capacity and expire within their bound", "[linkgraph][route_penalties]")
{
	LinkGraph::BaseEdge storage;
	LinkGraph::Edge edge(storage);
	LinkGraphRouteInfo first{ 1, static_cast<uint8_t>(VehicleType::Train), 100, 0 };
	edge.UpdateRoute(first, 20, {EdgeUpdateMode::Refresh});
	edge.UpdateRoute(first, 10, {EdgeUpdateMode::Refresh});
	REQUIRE(edge.Routes().size() == 1);
	CHECK(edge.Routes().front().capacity == 20);
	first.cost_scale = 200;
	first.cost_add = 4;
	edge.UpdateRoute(first, 5, {EdgeUpdateMode::Increase});
	CHECK(edge.Routes().front().capacity == 25);
	CHECK(edge.Routes().front().cost_scale == 200);
	CHECK(edge.Routes().front().cost_add == 4);
	storage.routes.front().last_update -= EconTime::DateDelta{100};
	LinkGraphRouteInfo recent{ 2, static_cast<uint8_t>(VehicleType::Train), 100, 0 };
	edge.UpdateRoute(recent, 10, {EdgeUpdateMode::Refresh});
	edge.PruneRoutes(EconTime::DateDelta{10});
	REQUIRE(edge.Routes().size() == 1);
	CHECK(edge.Routes().front().route == 2);
	for (uint16_t id = 3; id < 100; ++id) {
		LinkGraphRouteInfo info{ id, static_cast<uint8_t>(VehicleType::Train), 100, 0 };
		edge.UpdateRoute(info, 10, {EdgeUpdateMode::Refresh});
	}
	CHECK(edge.Routes().size() == 98); // Every service survives, including the first of more than 16.
	CHECK(edge.Routes().front().route == 2);
}

/** Give synthetic nodes distinct station IDs for the flow-restricted second pass. */
struct RoutePenaltyGraph : LinkGraph {
	RoutePenaltyGraph(uint size) : LinkGraph(LinkGraphID::Invalid(), CargoType{0})
	{
		this->Init(size);
		for (NodeID node = 0; node < size; ++node) {
			this->nodes[node].station = StationID{node};
			this->nodes[node].xy = TileIndex{node};
		}
	}
};

struct RoutePenaltySettings {
	AutoRestoreBackup<LinkGraphSettings> settings{_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<CargoClasses> classes{CargoSpec::Get(CargoType{0})->classes, CargoClasses{CargoClass::Passengers}};
	AutoRestoreBackup<uint8_t> days{DateDetail::_effective_day_length, uint8_t{1}};
	RoutePenaltySettings()
	{
		_settings_game.linkgraph.aircraft_link_scale = 100;
		_settings_game.linkgraph.accuracy = 16;
		_settings_game.linkgraph.short_path_saturation = 100;
		_settings_game.linkgraph.transfer_penalty_same_mode = 100;
		_settings_game.linkgraph.transfer_penalty_mode_change = 100;
	}
};

TEST_CASE("MCF keeps a more expensive arrival on the continuing service in both passes", "[linkgraph][route_penalties]")
{
	RoutePenaltyTestMap map;
	RoutePenaltySettings settings;
	bool second = false, zero = false;
	SECTION("Distance pass") {}
	SECTION("Capacity pass") { second = true; }
	SECTION("Zero penalty distance pass") { zero = true; }
	SECTION("Zero penalty capacity pass") { zero = second = true; }
	SECTION("Timetable passenger transfers also enabled") { _settings_game.linkgraph.passenger_transfer_timetables = true; }
	if (zero) _settings_game.linkgraph.transfer_penalty_same_mode = _settings_game.linkgraph.transfer_penalty_mode_change = 0;
	RoutePenaltyGraph graph(4); // A=0, B=1, C=2, D=3
	graph[0].UpdateSupply(1);
	LinkGraphRouteInfo a{1, 0, 100, 0}, b{2, 0, 100, 0};
	graph.UpdateEdge(0, 1, 100, 0, 1, {EdgeUpdateMode::Refresh}, &a);
	graph.UpdateEdge(0, 2, 100, 0, 1, {EdgeUpdateMode::Refresh}, &b);
	graph.UpdateEdge(2, 1, 100, 0, 1, {EdgeUpdateMode::Refresh}, &b);
	graph.UpdateEdge(1, 3, 100, 0, 1, {EdgeUpdateMode::Refresh}, &b);
	LinkGraphJob job(LinkGraphJobID::Invalid(), graph, 1);
	job.Init();
	std::array<LinkGraphJob::DemandAnnotation, 2> demand{{{1, 1, 1}, {3, 1, 1}}};
	job[0].SetDemandAnnotations(demand);
	if (second) {
		for (auto &edge : job.GetEdges()) job[edge.From()].Flows().AddFlow(StationID{0}, StationID{edge.To()}, 1);
		MCF2ndPass solver(job);
	} else {
		MCF1stPass solver(job);
	}
	CHECK(demand[0].unsatisfied_demand == 0);
	CHECK(demand[1].unsatisfied_demand == 0);
	CHECK(job[0].GetEdgeTo(1).Flow() == (zero ? 2 : 1));
	CHECK(job[0].GetEdgeTo(2).Flow() == (zero ? 0 : 1));
	CHECK(job[2].GetEdgeTo(1).Flow() == (zero ? 0 : 1));
	CHECK(job[1].GetEdgeTo(3).Flow() == 1);
}

TEST_CASE("Shared links cannot hide a service change across three consecutive edges", "[linkgraph][route_penalties]")
{
	RoutePenaltyTestMap map;
	RoutePenaltySettings settings;
	RoutePenaltyGraph graph(4);
	graph[0].UpdateSupply(1);
	LinkGraphRouteInfo a{1, 0, 100, 0}, b{2, 0, 100, 0};
	graph.UpdateEdge(0, 1, 100, 0, 1, {EdgeUpdateMode::Refresh}, &a);
	graph.UpdateEdge(1, 2, 100, 0, 1, {EdgeUpdateMode::Refresh}, &a);
	graph.UpdateEdge(1, 2, 1, 0, 1, {EdgeUpdateMode::Refresh}, &b);
	graph.UpdateEdge(2, 3, 100, 0, 1, {EdgeUpdateMode::Refresh}, &b);
	graph.UpdateEdge(0, 3, 100, 0, 10 * DAY_TICKS, {EdgeUpdateMode::Refresh}, &a);
	LinkGraphJob job(LinkGraphJobID::Invalid(), graph, 1);
	job.Init();
	std::array<LinkGraphJob::DemandAnnotation, 1> demand{{{3, 1, 1}}};
	job[0].SetDemandAnnotations(demand);
	MCF1stPass solver(job);
	CHECK(job[0].GetEdgeTo(3).Flow() == 1);
	CHECK(job[0].GetEdgeTo(1).Flow() == 0);
}

TEST_CASE("Path costs exceed UINT_MAX without wrapping or becoming unreachable", "[linkgraph][route_penalties]")
{
	Path root(0, true), first(1), second(2), saturated(3), unreachable(4);
	first.Fork(&root, 100, 100, UINT_MAX);
	CHECK(first.GetDistance() == UINT_MAX);
	second.Fork(&first, 100, 100, UINT_MAX);
	CHECK(second.GetDistance() == 2ULL * UINT_MAX);
	saturated.Fork(&second, 100, 100, MAX_PATH_COST);
	CHECK(saturated.GetDistance() == MAX_PATH_COST);
	CHECK(unreachable.GetDistance() == INVALID_PATH_COST);
	CHECK(AddPathCost(MAX_PATH_COST, 100) == MAX_PATH_COST);
	CHECK(AddPathCost(INVALID_PATH_COST, 100) == INVALID_PATH_COST);
	CHECK(Path::GetCapacityRatio(-100, 100) == -16);
	LinkGraph::BaseEdge stored;
	stored.capacity = UINT_MAX - 1;
	stored.travel_time_sum = static_cast<uint64_t>(stored.capacity) * UINT_MAX;
	LinkGraph::Edge aggregate(stored);
	aggregate.Update(100, 100, UINT_MAX, {EdgeUpdateMode::Increase});
	CHECK(aggregate.Capacity() == UINT_MAX);
	CHECK(aggregate.TravelTime() == UINT_MAX);

	RoutePenaltyTestMap map;
	RoutePenaltySettings settings;
	DateDetail::_effective_day_length = 255;
	_settings_game.linkgraph.aircraft_link_scale = 65535;
	_settings_game.linkgraph.transfer_penalty_same_mode = 365;
	_settings_game.linkgraph.transfer_penalty_mode_change = 365;
	RoutePenaltyGraph graph(4);
	graph[0].UpdateSupply(1);
	LinkGraphRouteInfo a{1, 0, 1000, 365}, b{2, 1, 1000, 365}, c{3, 2, 1000, 365};
	graph.UpdateEdge(0, 1, UINT_MAX, 0, UINT_MAX, {EdgeUpdateMode::Refresh, EdgeUpdateMode::Aircraft}, &a);
	graph.UpdateEdge(1, 2, UINT_MAX, 0, UINT_MAX, {EdgeUpdateMode::Refresh, EdgeUpdateMode::Aircraft}, &b);
	graph.UpdateEdge(2, 3, UINT_MAX, 0, UINT_MAX, {EdgeUpdateMode::Refresh, EdgeUpdateMode::Aircraft}, &c);
	LinkGraphJob job(LinkGraphJobID::Invalid(), graph, 1);
	job.Init();
	const LinkGraphCost edge_cost = ((static_cast<LinkGraphCost>(UINT_MAX) + DAY_TICKS) * 65535 / 100) * 10 + 365ULL * DAY_TICKS * 255;
	CHECK(job[0].GetEdgeTo(1).DistanceAnno() == edge_cost);
	CHECK(edge_cost > UINT_MAX);
	std::array<LinkGraphJob::DemandAnnotation, 1> demand{{{3, 1, 1}}};
	job[0].SetDemandAnnotations(demand);
	SECTION("Distance cost arithmetic") { MCF1stPass solver(job); }
	SECTION("Capacity cost arithmetic") {
		for (auto &edge : job.GetEdges()) job[edge.From()].Flows().AddFlow(StationID{0}, StationID{edge.To()}, 1);
		MCF2ndPass solver(job);
	}
	CHECK(demand[0].unsatisfied_demand == 0);
	CHECK(job[2].GetEdgeTo(3).Flow() == 1);
}

TEST_CASE("An exhausted cheap prefix survives until a downstream exhausted edge", "[linkgraph][route_penalties]")
{
	RoutePenaltyTestMap map;
	RoutePenaltySettings settings;
	RoutePenaltyGraph graph(4);
	LinkGraphRouteInfo service{1, 0, 100, 0};
	graph.UpdateEdge(0, 1, 100, 0, 10 * DAY_TICKS, {EdgeUpdateMode::Refresh}, &service);
	graph.UpdateEdge(0, 2, 100, 0, 1, {EdgeUpdateMode::Refresh}, &service);
	graph.UpdateEdge(2, 1, 100, 0, 1, {EdgeUpdateMode::Refresh}, &service);
	graph.UpdateEdge(1, 3, 100, 0, 1, {EdgeUpdateMode::Refresh}, &service);
	LinkGraphJob job(LinkGraphJobID::Invalid(), graph, 1);
	job.Init();
	job[0].GetEdgeTo(2).AddFlow(100);
	job[2].GetEdgeTo(1).AddFlow(100);
	job[1].GetEdgeTo(3).AddFlow(100);
	std::array<LinkGraphJob::DemandAnnotation, 1> demand{{{3, 1, 1}}};
	job[0].SetDemandAnnotations(demand);
	MCF1stPass solver(job);
	CHECK(demand[0].unsatisfied_demand == 0);
	CHECK(job[0].GetEdgeTo(1).Flow() == 0);
	CHECK(job[0].GetEdgeTo(2).Flow() == 101);
}

TEST_CASE("Deleted and recycled order-list IDs cannot revive old link observations", "[linkgraph][route_penalties]")
{
	RoutePenaltyTestMap map;
	RoutePenaltySettings settings;
	OrderList *old = OrderList::Create();
	const OrderListID id = old->index;
	const uint64_t generation = old->GetCargoDistRouteGeneration();
	old->SetCargoDistCostScale(200);
	LinkGraphRouteInfo info = old->GetLinkGraphRouteInfo(VehicleType::Train);
	RoutePenaltyGraph graph(2);
	graph.UpdateEdge(0, 1, 100, 0, 100, {EdgeUpdateMode::Refresh}, &info);
	LinkGraphJob running(LinkGraphJobID::Invalid(), graph, 1);
	running.Init();
	delete old;
	LinkGraphJob deleted(LinkGraphJobID::Invalid(), graph, 1);
	deleted.Init();
	CHECK(deleted[0].GetEdgeTo(1).Routes().empty());
	OrderList *replacement = OrderList::CreateAtIndex(id);
	CHECK(replacement->GetCargoDistRouteGeneration() != generation);
	LinkGraphJob recycled(LinkGraphJobID::Invalid(), graph, 1);
	recycled.Init();
	CHECK(recycled[0].GetEdgeTo(1).Routes().empty());
	CHECK(running[0].GetEdgeTo(1).Routes().front().generation == generation);
	CHECK(running[0].GetEdgeTo(1).DistanceAnno() == (100 + DAY_TICKS) * 2);
	info = replacement->GetLinkGraphRouteInfo(VehicleType::Train);
	graph.UpdateEdge(0, 1, 7, 0, 100, {EdgeUpdateMode::Increase}, &info);
	REQUIRE(graph.GetConstEdge(0, 1).Routes().size() == 1);
	CHECK(graph.GetConstEdge(0, 1).Routes().front().capacity == 7);
	replacement->SetCargoDistCostScale(300); // No vehicle update is needed for the next job.
	LinkGraphJob edited(LinkGraphJobID::Invalid(), graph, 1);
	edited.Init();
	CHECK(edited[0].GetEdgeTo(1).DistanceAnno() == (100 + DAY_TICKS) * 3);
	graph.RefreshRouteMetadata(true, false); // New save/load preserves a matching generation.
	CHECK(graph.GetConstEdge(0, 1).Routes().size() == 1);
	delete replacement;
	graph.RefreshRouteMetadata(true, false);
	CHECK(graph.GetConstEdge(0, 1).Routes().empty());
}

TEST_CASE("Invalid and legacy service metadata are discarded before use", "[linkgraph][route_penalties]")
{
	LinkGraph::EdgeRoute route{1, 0, 100, 0, 10, EconTime::CurDate()};
	CHECK(route.IsValid());
	SECTION("Invalid vehicle type") { route.vehicle_type = 255; }
	SECTION("Scale below minimum") { route.cost_scale = 0; }
	SECTION("Scale above maximum") { route.cost_scale = 1001; }
	SECTION("Addition above maximum") { route.cost_add = 366; }
	SECTION("Invalid order-list ID") { route.route = 65535; }
	SECTION("Zero capacity") { route.capacity = 0; }
	SECTION("Invalid date") { route.last_update = EconTime::INVALID_DATE; }
	CHECK_FALSE(route.IsValid());
	LinkGraphJob::Edge edge;
	edge.InitEdge(0, 1, 10, 1, std::span(&route, 1));
	CHECK(edge.VehicleTypeMask() <= 8); // Invalid type never performs an unchecked shift.

	RoutePenaltyTestMap map;
	RoutePenaltyGraph graph(2);
	LinkGraphRouteInfo legacy{1, 0, 100, 0};
	graph.UpdateEdge(0, 1, 100, 0, 100, {EdgeUpdateMode::Refresh}, &legacy);
	graph.RefreshRouteMetadata(true, false);
	CHECK(graph.GetConstEdge(0, 1).Routes().empty());
}

TEST_CASE("Route identity allocator resumes the saved server counter after loading", "[linkgraph][route_penalties]")
{
	OrderList *deleted = OrderList::Create();
	const uint64_t checkpoint = deleted->GetCargoDistRouteGeneration();
	delete deleted;
	AutoRestoreBackup saved(_saved_cargodist_route_generation, checkpoint);
	{
		OrderList temporary(OrderListID::Invalid());
		CHECK(temporary.GetCargoDistRouteGeneration() == 0); // GUI saved-order lists never consume identities.
		CHECK(GetCargoDistRouteGenerationCounter() == checkpoint);
	}
	OrderList *loaded_temporary = OrderList::Create();
	CHECK(loaded_temporary->GetCargoDistRouteGeneration() > checkpoint);
	delete loaded_temporary;
	RestoreCargoDistRouteGenerationCounter();
	OrderList *next = OrderList::Create();
	CHECK(next->GetCargoDistRouteGeneration() == checkpoint + 1);
	delete next;
}

TEST_CASE("Graph compression and merging retain many services and 64-bit travel sums", "[linkgraph][route_penalties]")
{
	RoutePenaltyTestMap map;
	RoutePenaltySettings settings;
	REQUIRE(Station::GetNumItems() == 0);
	Station *first = Station::Create(TileXY(10, 10));
	Station *second = Station::Create(TileXY(11, 10));
	LinkGraph target(LinkGraphID::Invalid(), CargoType{0});
	LinkGraph *source = LinkGraph::Create(CargoType{0});
	source->AddNode(first);
	source->AddNode(second);
	for (uint16_t id = 0; id < 32; ++id) {
		LinkGraphRouteInfo info{id, 0, 100, 0};
		source->UpdateEdge(0, 1, 100000, 0, 100000, {EdgeUpdateMode::Refresh}, &info);
	}
	source->Compress();
	CHECK(source->GetConstEdge(0, 1).TravelTime() == 100000);
	CHECK(source->GetConstEdge(0, 1).Routes().size() == 32);
	target.Merge(source);
	CHECK(target.GetConstEdge(0, 1).TravelTime() == 100000);
	REQUIRE(target.GetConstEdge(0, 1).Routes().size() == 32);
	CHECK(target.GetConstEdge(0, 1).Routes().front().capacity > 0);
	_station_pool.CleanPool();
}

struct RouteBackupVehicle : Vehicle {
	RouteBackupVehicle(VehicleID id = VehicleID::Invalid()) : Vehicle(id, VehicleType::Train) {}
	bool IsPrimaryVehicle() const override { return true; }
};

TEST_CASE("OrderBackup restores private route costs after the source list is deleted", "[linkgraph][route_penalties]")
{
	RoutePenaltyTestMap map;
	RouteBackupVehicle source, restored;
	Order dummy;
	dummy.MakeDummy();
	source.tile = restored.tile = TileIndex{4};
	source.orders = OrderList::Create(std::move(dummy), &source);
	source.orders->SetCargoDistCostScale(175);
	source.orders->SetCargoDistCostAdd(23);
	OrderBackup::Backup(&source, ClientID::Server);
	source.orders->FreeChain();
	source.orders = nullptr;
	OrderBackup::Restore(&restored, ClientID::Server);
	REQUIRE(restored.orders != nullptr);
	CHECK(restored.orders->GetCargoDistCostScale() == 175);
	CHECK(restored.orders->GetCargoDistCostAdd() == 23);
	restored.orders->FreeChain();
	restored.orders = nullptr;
}

TEST_CASE("Shared OrderBackup restores the surviving live route and its current costs", "[linkgraph][route_penalties]")
{
	RoutePenaltyTestMap map;
	REQUIRE(Vehicle::GetNumItems() == 0);
	Train *source = Vehicle::Create<Train>(), *peer = Vehicle::Create<Train>(), *restored = Vehicle::Create<Train>();
	for (Train *v : {source, peer, restored}) {
		v->SetFrontEngine();
		v->owner = _current_company;
		v->tile = TileIndex{4};
	}
	Order dummy;
	dummy.MakeDummy();
	source->orders = OrderList::Create(std::move(dummy), source);
	source->orders->SetCargoDistCostScale(175);
	source->orders->SetCargoDistCostAdd(23);
	peer->orders = source->orders;
	peer->AddToShared(source);
	OrderBackup::Backup(source, ClientID::Server);
	source->RemoveFromShared();
	source->orders = nullptr;
	peer->orders->SetCargoDistCostScale(300);
	peer->orders->SetCargoDistCostAdd(25);
	OrderBackup::Restore(restored, ClientID::Server);
	CHECK(restored->orders == peer->orders);
	REQUIRE(restored->orders != nullptr);
	CHECK(restored->orders->GetCargoDistCostScale() == 300);
	CHECK(restored->orders->GetCargoDistCostAdd() == 25);
	restored->RemoveFromShared();
	restored->orders = nullptr;
	peer->orders->FreeChain();
	peer->orders = nullptr;
	_vehicle_pool.CleanPool();
}

TEST_CASE("Service labels share the capacity of their physical link", "[linkgraph][route_penalties]")
{
	RoutePenaltyTestMap map;
	RoutePenaltySettings settings;
	RoutePenaltyGraph graph(3);
	LinkGraphRouteInfo a{1, 0, 100, 0}, b{2, 0, 100, 0};
	for (NodeID from = 0; from < 2; ++from) {
		graph.UpdateEdge(from, from + 1, 100, 0, 1, {EdgeUpdateMode::Refresh}, &a);
		graph.UpdateEdge(from, from + 1, 100, 0, 1, {EdgeUpdateMode::Refresh}, &b);
	}
	LinkGraphJob job(LinkGraphJobID::Invalid(), graph, 1);
	job.Init();
	std::array<LinkGraphJob::DemandAnnotation, 1> demand{{{2, 150, 150}}};
	job[0].SetDemandAnnotations(demand);
	MCF1stPass solver(job);
	CHECK(demand[0].unsatisfied_demand == 50);
	CHECK(job[0].GetEdgeTo(1).Flow() == 100);
	CHECK(job[1].GetEdgeTo(2).Flow() == 100);
}

TEST_CASE("Service-state distance search agrees with exhaustive DAG journeys", "[linkgraph][route_penalties]")
{
	RoutePenaltyTestMap map;
	RoutePenaltySettings settings;
	uint32_t random = 739;
	auto next = [&]() { random = random * 1664525 + 1013904223; return random; };
	for (uint sample = 0; sample < 64; ++sample) {
		_settings_game.linkgraph.transfer_penalty_same_mode = sample % 5;
		_settings_game.linkgraph.transfer_penalty_mode_change = sample % 7;
		if (sample == 0) _settings_game.linkgraph.transfer_penalty_same_mode = 1;
		RoutePenaltyGraph graph(7);
		for (NodeID from = 0; from < 6; ++from) {
			for (NodeID to = from + 1; to < 7; ++to) {
				if (to != from + 1 && (next() % 3) == 0) continue;
				const uint time = 1 + next() % 1000;
				for (uint i = 0, count = 1 + next() % 3; i < count; ++i) {
					const uint16_t id = 1 + next() % 4;
					LinkGraphRouteInfo route{id, static_cast<uint8_t>(id % 2), static_cast<uint16_t>(100 + next() % 300), static_cast<uint16_t>(next() % 3)};
					graph.UpdateEdge(from, to, 100, 0, time, {EdgeUpdateMode::Refresh}, &route);
				}
			}
		}
		LinkGraphJob job(LinkGraphJobID::Invalid(), graph, 1);
		job.Init();
		auto exhaustive = [&](auto &&self, NodeID node, uint32_t previous, bool chosen_only) -> LinkGraphCost {
			if (node == 6) return 0;
			LinkGraphCost best = INVALID_PATH_COST;
			for (const auto &edge : job[node].GetEdges()) {
				if (chosen_only && edge.Flow() == 0) continue;
				for (const auto &route : edge.Routes()) {
					const uint32_t id = route.route | (static_cast<uint32_t>(route.vehicle_type) << 16);
					const LinkGraphCost cost = AddPathCost(AddPathCost(edge.DistanceAnno(), job.GetRouteTransferPenalty(previous, id)), self(self, edge.To(), id, chosen_only));
					best = std::min(best, cost);
				}
			}
			return best;
		};
		const LinkGraphCost expected = exhaustive(exhaustive, 0, UINT32_MAX, false);
		std::array<LinkGraphJob::DemandAnnotation, 1> demand{{{6, 1, 1}}};
		job[0].SetDemandAnnotations(demand);
		MCF1stPass solver(job);
		INFO("DAG sample " << sample);
		CHECK(demand[0].unsatisfied_demand == 0);
		CHECK(exhaustive(exhaustive, 0, UINT32_MAX, true) == expected);
	}
}

TEST_CASE("Capacity service search keeps its ancestry guard for cyclic flow graphs", "[linkgraph][route_penalties]")
{
	RoutePenaltyTestMap map;
	RoutePenaltySettings settings;
	RoutePenaltyGraph graph(4);
	LinkGraphRouteInfo service{1, 0, 100, 0};
	graph.UpdateEdge(0, 1, 100, 0, 1, {EdgeUpdateMode::Refresh}, &service);
	graph.UpdateEdge(1, 2, 100, 0, 1, {EdgeUpdateMode::Refresh}, &service);
	graph.UpdateEdge(2, 1, 10, 0, 1, {EdgeUpdateMode::Refresh}, &service);
	graph.UpdateEdge(2, 3, 100, 0, 1, {EdgeUpdateMode::Refresh}, &service);
	LinkGraphJob job(LinkGraphJobID::Invalid(), graph, 1);
	job.Init();
	job[0].GetEdgeTo(1).AddFlow(80);
	job[1].GetEdgeTo(2).AddFlow(80);
	for (const auto &edge : job.GetEdges()) job[edge.From()].Flows().AddFlow(StationID{0}, StationID{edge.To()}, 1);
	std::array<LinkGraphJob::DemandAnnotation, 1> demand{{{3, 1, 1}}};
	job[0].SetDemandAnnotations(demand);
	MCF2ndPass solver(job);
	CHECK(demand[0].unsatisfied_demand == 0);
	CHECK(job[2].GetEdgeTo(1).Flow() == 0);
	CHECK(job[2].GetEdgeTo(3).Flow() == 1);
}

extern int PredictStationStoppingLocation(const Train *, const Order *, int, DestinationID);

TEST_CASE("Long train near-end stop geometry agrees with the braking prediction", "[linkgraph][route_penalties][train]")
{
	RoutePenaltyTestMap map;
	REQUIRE(Vehicle::GetNumItems() == 0);
	REQUIRE(Station::GetNumItems() == 0);
	Station *station = Station::Create(TileXY(10, 10));
	for (uint x = 10; x < 12; ++x) MakeRailStation(TileXY(x, 10), Owner{0}, station->index, Axis::X, 0, RAILTYPE_RAIL);
	for (uint x = 12; x < 40; ++x) MakeRailNormal(TileXY(x, 10), Owner{0}, TRACK_BIT_X, RAILTYPE_RAIL);
	bool reverse = false, articulated = false, too_long_unit = false, obstructed = false, realistic = false, dual_headed = false, turnaround = false;
	uint count = 6;
	SECTION("Forward") {}
	SECTION("Reverse running") { reverse = true; }
	SECTION("Articulated moving units") { articulated = true; }
	SECTION("Much longer than the platform") { count = 32; }
	SECTION("Final moving unit cannot fit") { too_long_unit = true; }
	SECTION("Dead end beyond the platform") {
		MakeRailNormal(TileXY(12, 10), Owner{0}, TRACK_BIT_NONE, RAILTYPE_RAIL);
		obstructed = true;
	}
	SECTION("Incompatible track beyond the platform") {
		MakeRailNormal(TileXY(12, 10), Owner{0}, TRACK_BIT_X, RAILTYPE_MONO);
		obstructed = true;
	}
	SECTION("One-way signal blocks onward running") {
		TileIndex signal = TileXY(12, 10);
		SB(_m[signal].m5, 6, 2, to_underlying(RailTileType::Signals));
		SetSignalType(signal, TRACK_X, SignalType::PathOneWay);
		SetPresentSignals(signal, SignalAgainstTrackdir(TRACKDIR_X_SW));
		obstructed = true;
	}
	SECTION("Reserved onward track does not alter the station target") {
		SetTrackReservation(TileXY(13, 10), TRACK_BIT_X);
	}
	SECTION("Realistic braking uses the same geometric target") { realistic = true; }
	SECTION("Dual-headed engine parts") { dual_headed = true; }
	SECTION("Turnaround resets the platform flags") { turnaround = true; }
	std::vector<Train *> parts;
	for (uint i = 0; i < count; ++i) {
		Train *part = Vehicle::Create<Train>();
		if (!parts.empty()) parts.back()->SetNext(part);
		parts.push_back(part);
		part->gcache.cached_veh_length = 8;
		part->tcache.cached_tflags = (articulated && (i % 2) != 0) ? TrainCacheFlags{} : TCF_MOVING_UNIT_START;
		if (reverse) part->vehicle_flags.Set(VehicleFlag::DrivingBackwards);
		part->SetMovingDirection(Direction::SW);
		part->track = TRACK_BIT_X;
		part->tile = TileXY(12, 10);
		part->x_pos = 12 * TILE_SIZE + 8;
		part->y_pos = 10 * TILE_SIZE + 8;
	}
	Train *front = parts.front();
	if (dual_headed) {
		front->SetEngine();
		front->SetMultiheaded();
		parts.back()->SetMultiheaded();
		front->other_multiheaded_part = parts.back();
		parts.back()->other_multiheaded_part = front;
	}
	if (obstructed) {
		front->tile = TileXY(11, 10);
		front->x_pos = 11 * TILE_SIZE + 12;
	}
	front->gcache.cached_total_length = count * 8;
	if (realistic) front->tcache.cached_tflags |= TCF_RL_BRAKING;
	front->compatible_railtypes.Set(RAILTYPE_RAIL);
	front->current_order.MakeGoToStation(station->index);
	front->current_order.SetStopLocation(OrderStopLocation::NearEnd);
	if (too_long_unit) for (uint i = 2; i < count; ++i) parts[i]->tcache.cached_tflags = TrainCacheFlags{};
	int ahead, length;
	GetTrainStopLocation(station->index, TileXY(10, 10), front->GetMovingFront(), true, &ahead, &length);
	const Train *loading = front->GetStationLoadingVehicle();
	REQUIRE(loading != nullptr);
	int beyond = 0;
	for (const Train *part = front->GetMovingFront(); part != loading; part = part->GetMovingNext()) {
		CHECK(part->flags.Test(VehicleRailFlag::BeyondPlatformEnd));
		beyond += part->gcache.cached_veh_length;
	}
	CHECK(beyond == (too_long_unit ? 0 : front->gcache.cached_total_length - length));
	CHECK_FALSE(loading->flags.Test(VehicleRailFlag::BeyondPlatformEnd));
	const int actual = GetTrainStopLocation(station->index, TileXY(11, 10), const_cast<Train *>(loading), false, &ahead, &length);
	const int predicted = PredictStationStoppingLocation(front, &front->current_order, length, station->index);
	if (obstructed) {
		CHECK(predicted > actual + beyond + 4); // Movement limits shorten the geometric target.
	} else {
		CHECK(predicted == actual + beyond + 4);
	}
	if (turnaround) {
		for (Train *part : parts) {
			part->flags.Reset({VehicleRailFlag::BeyondPlatformEnd, VehicleRailFlag::NotYetInPlatform});
			part->vehicle_flags.Set(VehicleFlag::DrivingBackwards);
			part->SetMovingDirection(Direction::SW);
		}
		GetTrainStopLocation(station->index, TileXY(10, 10), front->GetMovingFront(), true, &ahead, &length);
		loading = front->GetStationLoadingVehicle();
		REQUIRE(loading != nullptr);
		CHECK_FALSE(loading->flags.Test(VehicleRailFlag::BeyondPlatformEnd));
		CHECK(PredictStationStoppingLocation(front, &front->current_order, length, station->index) ==
				GetTrainStopLocation(station->index, TileXY(11, 10), const_cast<Train *>(loading), false, &ahead, &length) + beyond + 4);
	}
	_vehicle_pool.CleanPool();
	_station_pool.CleanPool();
}

TEST_CASE("CargoDist sparse multi-service search performance", "[linkgraph][route_penalties][.performance]")
{
	RoutePenaltyTestMap map(128);
	RoutePenaltySettings settings;
	bool zero = false, capacity_pass = false;
	SECTION("Station-only baseline") { zero = true; }
	SECTION("Service states") {}
	SECTION("Station-only capacity baseline") { zero = capacity_pass = true; }
	SECTION("Service capacity states") { capacity_pass = true; }
	if (zero) _settings_game.linkgraph.transfer_penalty_same_mode = _settings_game.linkgraph.transfer_penalty_mode_change = 0;
	const NodeID size = std::getenv("OPENTTD_ROUTE_BENCH_LARGE") != nullptr ? 8192 : 2048;
	RoutePenaltyGraph graph(size);
	LinkGraphRouteInfo a{1, 0, 100, 0}, b{2, 0, 120, 1};
	for (NodeID from = 0; from + 1 < size; ++from) {
		graph.UpdateEdge(from, from + 1, 100, 0, 100, {EdgeUpdateMode::Refresh}, &a);
		graph.UpdateEdge(from, from + 1, 100, 0, 100, {EdgeUpdateMode::Refresh}, &b);
		if (from + 8 < size) graph.UpdateEdge(from, from + 8, 100, 0, 500, {EdgeUpdateMode::Refresh}, &b);
	}
	const uint source_count = std::getenv("OPENTTD_ROUTE_BENCH_ALL_SOURCES") != nullptr ? size - 1 : 32;
	std::vector<std::array<LinkGraphJob::DemandAnnotation, 1>> demands(source_count);
	LinkGraphJob job(LinkGraphJobID::Invalid(), graph, 1);
	job.Init();
	for (NodeID source = 0; source < demands.size(); ++source) {
		demands[source][0] = {static_cast<NodeID>(size - 1), 1, 1};
		job[source].SetDemandAnnotations(demands[source]);
		if (capacity_pass) for (const auto &edge : job.GetEdges()) job[edge.From()].Flows().AddFlow(StationID{source}, StationID{edge.To()}, 1);
	}
	const auto begin = std::chrono::steady_clock::now();
	if (capacity_pass) {
		MCF2ndPass solver(job);
	} else {
		MCF1stPass solver(job);
	}
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
	WARN(size << " nodes, " << job.GetEdges().size() << " edges, " << source_count << " active sources, " << (zero ? "zero" : "nonzero") << " penalties, " << (capacity_pass ? "capacity" : "distance") << " pass: " << elapsed << " ms; Path=" << sizeof(Path) << ", EdgeRoute=" << sizeof(LinkGraph::EdgeRoute));
	WARN("Search peaks: states=" << job.route_search_stats.states << ", labels=" << job.route_search_stats.labels << ", queue=" << job.route_search_stats.queue << ", scratch payload=" << job.route_search_stats.scratch_bytes << " bytes");
	for (const auto &demand : demands) CHECK(demand[0].unsatisfied_demand == 0);
}

TEST_CASE("LinkGraph jobs distinguish route transfers and snapshot weighted costs", "[linkgraph][route_penalties]")
{
	RoutePenaltyTestMap map;
	constexpr CargoType cargo{0};
	AutoRestoreBackup settings_backup(_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{});
	AutoRestoreBackup classes_backup(CargoSpec::Get(cargo)->classes, CargoClasses{CargoClass::Passengers});
	AutoRestoreBackup day_length_backup(DateDetail::_effective_day_length, uint8_t{1});
	_settings_game.linkgraph.aircraft_link_scale = 100;
	_settings_game.linkgraph.transfer_penalty_same_mode = 2;
	_settings_game.linkgraph.transfer_penalty_mode_change = 5;
	LinkGraph graph(LinkGraphID::Invalid(), cargo);
	graph.Init(4);
	for (NodeID node = 0; node < 4; ++node) graph[node].UpdateLocation(TileIndex{node});
	LinkGraphRouteInfo train_a{ 1, static_cast<uint8_t>(VehicleType::Train), 100, 0 };
	LinkGraphRouteInfo train_b{ 2, static_cast<uint8_t>(VehicleType::Train), 200, 4 };
	LinkGraphRouteInfo road{ 3, static_cast<uint8_t>(VehicleType::Road), 100, 0 };
	graph.UpdateEdge(0, 1, 100, 0, 600, {EdgeUpdateMode::Refresh}, &train_a);
	graph.UpdateEdge(0, 1, 300, 0, 600, {EdgeUpdateMode::Refresh}, &train_b);
	graph.UpdateEdge(1, 2, 100, 0, 600, {EdgeUpdateMode::Refresh}, &train_b);
	graph.UpdateEdge(2, 3, 100, 0, 600, {EdgeUpdateMode::Refresh}, &road);
	LinkGraphJob job(LinkGraphJobID::Invalid(), graph, 1);
	job.Init();
	const uint day_cost = DAY_TICKS * DayLengthFactor();
	REQUIRE(day_cost == DAY_TICKS);
	const auto &weighted = job[0].GetEdgeTo(1);
	const auto &train = job[1].GetEdgeTo(2);
	const auto &bus = job[2].GetEdgeTo(3);
	CHECK(weighted.DistanceAnno() == (600 + DAY_TICKS) * 175 / 100 + 3 * day_cost);
	CHECK(job.GetTransferPenalty(nullptr, train) == 0);
	CHECK(job.GetTransferPenalty(&weighted, train) == 0); // Route B continues across both edges.
	CHECK(job.GetTransferPenalty(&train, bus) == 5 * day_cost);

	SECTION("Separate train routes incur the same-mode penalty") {
		graph.UpdateEdge(3, 1, 100, 0, 600, {EdgeUpdateMode::Refresh}, &train_a);
		LinkGraphJob separate(LinkGraphJobID::Invalid(), graph, 1);
		separate.Init();
		CHECK(separate.GetTransferPenalty(&separate[3].GetEdgeTo(1), separate[1].GetEdgeTo(2)) == 2 * day_cost);
	}
	SECTION("Missing route metadata and zero penalties retain original costs") {
		graph.UpdateEdge(3, 0, 100, 0, 600, {EdgeUpdateMode::Refresh});
		_settings_game.linkgraph.transfer_penalty_same_mode = 0;
		_settings_game.linkgraph.transfer_penalty_mode_change = 0;
		LinkGraphJob disabled(LinkGraphJobID::Invalid(), graph, 1);
		disabled.Init();
		CHECK(disabled[3].GetEdgeTo(0).DistanceAnno() == 600 + DAY_TICKS);
		CHECK(disabled.GetTransferPenalty(&disabled[2].GetEdgeTo(3), disabled[3].GetEdgeTo(0)) == 0);
		CHECK(disabled.GetTransferPenalty(&disabled[1].GetEdgeTo(2), disabled[2].GetEdgeTo(3)) == 0);
		CHECK(job.GetTransferPenalty(&train, bus) == 5 * day_cost); // Existing job keeps its settings.
	}
	SECTION("Editing live route records cannot change a running job") {
		train_a.cost_scale = 300;
		graph.UpdateEdge(0, 1, 100, 0, 600, {EdgeUpdateMode::Refresh}, &train_a);
		CHECK(weighted.Routes().front().cost_scale == 100);
		CHECK(weighted.DistanceAnno() == (600 + DAY_TICKS) * 175 / 100 + 3 * day_cost);
		LinkGraphJob updated(LinkGraphJobID::Invalid(), graph, 1);
		updated.Init();
		CHECK(updated[0].GetEdgeTo(1).DistanceAnno() == (600 + DAY_TICKS) * 225 / 100 + 3 * day_cost);
	}
}

TEST_CASE("CargoDist transfer penalties change the chosen flow while zero retains the fast path", "[linkgraph][route_penalties]")
{
	RoutePenaltyTestMap map;
	constexpr CargoType cargo{0};
	AutoRestoreBackup settings_backup(_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{});
	AutoRestoreBackup classes_backup(CargoSpec::Get(cargo)->classes, CargoClasses{CargoClass::Passengers});
	AutoRestoreBackup day_length_backup(DateDetail::_effective_day_length, uint8_t{1});
	_settings_game.linkgraph.aircraft_link_scale = 100;
	_settings_game.linkgraph.accuracy = 16;
	_settings_game.linkgraph.short_path_saturation = 100;
	_settings_game.linkgraph.transfer_penalty_same_mode = 0;
	_settings_game.linkgraph.transfer_penalty_mode_change = 0;
	bool penalized = false;
	SECTION("Zero penalty selects the faster connection") {}
	SECTION("Positive penalty selects the through service") {
		_settings_game.linkgraph.transfer_penalty_same_mode = 2;
		penalized = true;
	}
	LinkGraph graph(LinkGraphID::Invalid(), cargo);
	graph.Init(3);
	for (NodeID node = 0; node < 3; ++node) graph[node].UpdateLocation(TileIndex{node});
	graph[0].UpdateSupply(10);
	LinkGraphRouteInfo a{ 1, static_cast<uint8_t>(VehicleType::Train), 100, 0 };
	LinkGraphRouteInfo b{ 2, static_cast<uint8_t>(VehicleType::Train), 100, 0 };
	graph.UpdateEdge(0, 1, 100, 0, 80, {EdgeUpdateMode::Refresh}, &a);
	graph.UpdateEdge(1, 2, 100, 0, 80, {EdgeUpdateMode::Refresh}, &b);
	graph.UpdateEdge(0, 2, 100, 0, 300, {EdgeUpdateMode::Refresh}, &a);
	LinkGraphJob job(LinkGraphJobID::Invalid(), graph, 1);
	job.Init();
	REQUIRE(job.GetTransferPenalty(&job[0].GetEdgeTo(1), job[1].GetEdgeTo(2)) == (penalized ? 2 * DAY_TICKS : 0));
	std::array<LinkGraphJob::DemandAnnotation, 1> demand{ { {2, 10, 10} } };
	job[0].SetDemandAnnotations(demand);
	MCF1stPass solver(job);
	CHECK(demand[0].unsatisfied_demand == 0);
	CHECK(job[0].GetEdgeTo(2).Flow() == (penalized ? 10 : 0));
	CHECK(job[0].GetEdgeTo(1).Flow() == (penalized ? 0 : 10));
	CHECK(job[1].GetEdgeTo(2).Flow() == (penalized ? 0 : 10));
}
