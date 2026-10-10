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
	CHECK(edge.Routes().size() == LinkGraph::MAX_EDGE_ROUTES);
}

/** Restore the temporary map even when an assertion aborts a test section. */
struct RoutePenaltyTestMap {
	RoutePenaltyTestMap() { AllocateMap(64, 64); }
	~RoutePenaltyTestMap() { DeallocateMap(); }
};

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
