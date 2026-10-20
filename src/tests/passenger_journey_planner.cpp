/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file passenger_journey_planner.cpp Actual-order journey, capacity, cache and national-network regressions. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../passenger_journey_planner.h"
#include "../passenger_journey_active.h"
#include "../vehicle_base.h"
#include "passenger_test_stations.h"
#include "../core/random_func.hpp"
#include "../core/backup_type.hpp"
#include <chrono>
#include <deque>

static constexpr CargoType PAX{0};
static constexpr StationID A{0}, B{1}, C{2}, D{3};
static constexpr StateTicks NOW{10000};

static Order JourneyStop(StationID station, TimetableTicks travel = 10000, TimetableTicks wait = 10)
{
	Order order;
	order.MakeGoToStation(station);
	order.SetTravelTime(travel);
	order.SetTravelTimetabled(true);
	order.SetWaitTime(wait);
	order.SetWaitTimetabled(true);
	return order;
}

struct JourneyVehicle : Vehicle {
	explicit JourneyVehicle(std::vector<Order> orders, VehicleID id = VehicleID{100}) : Vehicle(id, VehicleType::Train)
	{
		this->orders = OrderList::Create(std::move(orders), this);
		EnsurePassengerTestStations(this->orders->GetOrderVector());
		this->cur_timetable_order_index = INVALID_VEH_ORDER_ID;
		this->current_order.MakeLoading(false);
		this->last_station_visited = this->GetOrder(0)->GetDestination().ToStationID();
		this->cargo_type = PAX;
		this->cargo_cap = 80;
		this->direction = Direction::NE;
	}
	~JourneyVehicle()
	{
		this->orders->FreeChain();
		this->orders = nullptr;
	}
	bool IsPrimaryVehicle() const override { return true; }
};

static PassengerServiceGraph JourneyGraph(Ticks horizon = 1000)
{
	PassengerServiceGraphLimits limits;
	limits.horizon = horizon;
	return PassengerServiceGraph::Build(PAX, NOW, limits);
}

/** One physical train per connection, with actual timetabled departures at NOW + 10 + 20*i.
 * The 10000-tick return leg keeps subsequent circuits outside these test horizons. */
struct JourneyChain {
	std::deque<JourneyVehicle> trains;
	explicit JourneyChain(uint32_t transfers)
	{
		for (uint32_t i = 0; i <= transfers; ++i) {
			this->trains.emplace_back(std::vector<Order>{JourneyStop(StationID{static_cast<uint16_t>(i)}, 10000, 10 + 20 * i),
					JourneyStop(StationID{static_cast<uint16_t>(i + 1)}, 10)}, VehicleID{1000 + i});
		}
	}
};

static void CheckSameJourneyResult(const PassengerJourneyResult &a, const PassengerJourneyResult &b)
{
	CHECK(a.journeys == b.journeys);
	CHECK(a.status == b.status);
	CHECK(a.stop_reason == b.stop_reason);
	CHECK(a.prediction_end == b.prediction_end);
	CHECK(a.IsUnreachable() == b.IsUnreachable());
	CHECK(a.IsInconclusive() == b.IsInconclusive());
	CHECK(a.diagnostic.work == b.diagnostic.work);
	CHECK(a.diagnostic.reverse_work == b.diagnostic.reverse_work);
	CHECK(a.diagnostic.rounds == b.diagnostic.rounds);
	CHECK(a.diagnostic.extensions == b.diagnostic.extensions);
	CHECK(a.diagnostic.labels == b.diagnostic.labels);
	CHECK(a.diagnostic.scratch_bytes == b.diagnostic.scratch_bytes);
}

TEST_CASE("Scoped alternatives preserve local express returns and cyclic detours", "[passenger][journey][scope]")
{
	PassengerTestStations stations;
	JourneyVehicle local({JourneyStop(A), JourneyStop(B, 100), JourneyStop(C, 1000), JourneyStop(D, 1000)});
	JourneyVehicle express({JourneyStop(B, 10000, 120), JourneyStop(C, 20)}, VehicleID{101});
	JourneyVehicle onward({JourneyStop(C, 10000, 160), JourneyStop(D, 20)}, VehicleID{102});
	JourneyVehicle branch({JourneyStop(A), JourneyStop(StationID{99}, 20)}, VehicleID{103});
	auto limits = ActivePassengerServiceGraphLimits(); limits.horizon = 3000;
	const auto full = PassengerServiceGraph::Build(PAX, NOW, limits);
	const auto scoped = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
	REQUIRE(scoped.GetTrips().size() == 3);
	PassengerJourneyPlanner reference(full), planner(scoped);
	auto expected = reference.Plan(PAX, A, D, NOW), actual = planner.Plan(PAX, A, D, NOW);
	REQUIRE(actual.HasJourney());
	CHECK(actual.status == PassengerPredictionStatus::Complete);
	CHECK(actual.journeys.front().arrival == expected.journeys.front().arrival);
	CHECK(actual.journeys.front().generalized_time == expected.journeys.front().generalized_time);
	CHECK(actual.journeys.front().legs.size() == 3);
	CHECK(planner.Plan(PAX, StationID{99}, D, NOW).status == PassengerPredictionStatus::Unknown);
}

TEST_CASE("Scoped uncertainty retains future refits and conditional alternatives", "[passenger][journey][scope]")
{
	PassengerTestStations stations;
	JourneyVehicle local({JourneyStop(A), JourneyStop(D, 1000)});
	JourneyVehicle alternative({JourneyStop(A), JourneyStop(D, 10)}, VehicleID{101});
	SECTION("A zero-capacity consist can become passenger capable") { alternative.cargo_type = CargoType{1}; alternative.GetOrder(1)->SetRefit(PAX); }
	SECTION("A current refit outside the declared pattern is unknown") { alternative.cargo_type = CargoType{1}; alternative.current_order.SetRefit(PAX); }
	SECTION("A conditional branch is not an exclusion certificate") {
		Order conditional; conditional.MakeConditional(1); conditional.SetConditionVariable(OrderConditionVariable::LoadPercentage);
		alternative.orders->InsertOrderAt(std::move(conditional), 1);
	}
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, ActivePassengerServiceGraphLimits(), PassengerServiceScope{A, D});
	PassengerJourneyPlanner planner(graph);
	auto result = planner.Plan(PAX, A, D, NOW);
	CHECK(result.HasJourney());
	CHECK(result.status != PassengerPredictionStatus::Complete);
	CHECK_FALSE(result.IsUnreachable());
	CHECK(graph.GetTrips().size() == 2);
}

TEST_CASE("Weighted waiting never certifies a scoped graph that omitted riding excursions", "[passenger][journey][scope]")
{
	PassengerTestStations stations;
	JourneyVehicle direct({JourneyStop(A, 10000, 100), JourneyStop(D, 10, 0)});
	JourneyVehicle excursion({JourneyStop(A, 10000, 0), JourneyStop(B, 10, 0), JourneyStop(A, 10, 0)}, VehicleID{101});
	auto limits = ActivePassengerServiceGraphLimits(); limits.horizon = 200;
	const auto full = PassengerServiceGraph::Build(PAX, NOW, limits);
	const auto scoped = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
	PassengerJourneyPlanner reference(full), planner(scoped);
	PassengerJourneyEvaluation evaluation; evaluation.waiting_weight = 1000;
	PassengerJourneyLimits query; query.candidates = 1;
	auto better = reference.Plan(PAX, A, D, NOW, query, evaluation), partial = planner.Plan(PAX, A, D, NOW, query, evaluation);
	REQUIRE(better.HasJourney()); REQUIRE(partial.HasJourney());
	CHECK(better.journeys.front().generalized_time < partial.journeys.front().generalized_time);
	CHECK(partial.status == PassengerPredictionStatus::Unknown);
	CHECK(planner.Plan(PAX, A, D, NOW, query).status == PassengerPredictionStatus::Complete);
}

struct NationalCoach : Vehicle {
	explicit NationalCoach(VehicleID id) : Vehicle(id, VehicleType::Train) { this->cargo_type = CargoType{1}; this->cargo_cap = 100; }
	bool IsPrimaryVehicle() const override { return false; }
};

struct NationalServiceFleet {
	std::deque<JourneyVehicle> passengers, freight;
	std::vector<NationalCoach *> coaches;
	NationalServiceFleet()
	{
		for (uint16_t route = 0; route < 80; ++route) {
			std::vector<Order> orders{JourneyStop(A, 100000, 1)};
			for (uint16_t station = 1; station <= 100; ++station) orders.push_back(JourneyStop(StationID{static_cast<uint16_t>(route * 100 + station)}, 1, 1));
			for (uint16_t train = 0; train < 10; ++train) this->passengers.emplace_back(orders, VehicleID{1000u + route * 10u + train});
		}
		for (uint32_t i = 0; i < 3840; ++i) {
			this->freight.emplace_back(std::vector<Order>{JourneyStop(A, 100000, 1), JourneyStop(StationID{static_cast<uint16_t>(9000 + i)}, 1, 1)}, VehicleID{3000 + i});
			this->freight.back().cargo_type = CargoType{1};
			Vehicle *tail = &this->freight.back();
			for (uint32_t part = 0; part < 4; ++part) {
				this->coaches.push_back(Vehicle::Create<NationalCoach>());
				tail->SetNext(this->coaches.back()); tail = this->coaches.back();
			}
		}
	}
	~NationalServiceFleet()
	{
		for (auto &train : this->freight) train.SetNext(nullptr);
		for (auto *coach : this->coaches) coach->SetNext(nullptr);
		for (auto *coach : this->coaches) delete coach;
	}
};

TEST_CASE("Standard scoped capture serves an 80800 visit twenty thousand object network", "[passenger][journey][scope][performance]")
{
	PassengerTestStations stations;
	NationalServiceFleet fleet;
	REQUIRE(fleet.passengers.size() + fleet.freight.size() + fleet.coaches.size() == 20000);
	auto limits = ActivePassengerServiceGraphLimits();
	InvalidatePassengerServiceIndex();
	GameRandomSeedChecker random;
	const auto cold = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, StationID{8000}});
	const auto warm = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, StationID{8000}});
	REQUIRE(cold.GetQueryStatus(A, StationID{8000}) == PassengerPredictionStatus::Complete);
	REQUIRE(cold.GetTrips().size() == 10);
	CHECK(cold.GetDiagnostic().visits == 1010);
	CHECK(cold.GetDiagnostic().trains == 4640);
	CHECK(cold.GetDiagnostic().excluded_cargo == 3840);
	CHECK(cold.GetDiagnostic().excluded_scope == 790);
	CHECK(cold.GetWork() == warm.GetWork());
	CHECK(cold.GetWork() < limits.work);
	CHECK(cold.GetMemoryBound() <= limits.memory_bytes);
	PassengerJourneyLimits query; query.candidates = 1; query.work = 65536; query.labels = 8192; query.memory_bytes = 8 * 1024 * 1024;
	PassengerJourneyPlanner planner(warm), uncached(warm, nullptr, {0, 0});
	auto journey = planner.Plan(PAX, A, StationID{8000}, NOW, query);
	REQUIRE(journey.HasJourney());
	CHECK(journey.status == PassengerPredictionStatus::Complete);
	CHECK(journey.journeys.front().arrival == NOW + 200);
	CheckSameJourneyResult(journey, uncached.Plan(PAX, A, StationID{8000}, NOW, query));
	for (uint i = 0; i < 128; ++i) CHECK(planner.Plan(PAX, A, StationID{8000}, NOW, query).diagnostic.cache_hit);
	CHECK(random.Check());
	WARN("Scoped national standard: cold=" << cold.GetDiagnostic().build_microseconds << " us; warm=" << warm.GetDiagnostic().build_microseconds << " us; visits=" << cold.GetDiagnostic().visits << "; logical work=" << cold.GetWork() << "; retained bytes=" << cold.GetDiagnostic().allocation_bytes << "; memory bound=" << cold.GetMemoryBound() << "; query work=" << journey.diagnostic.work);
}

TEST_CASE("Dense national cycles remain incomplete when query allowances cannot certify alternatives", "[passenger][journey][scope][performance]")
{
	PassengerTestStations stations;
	NationalServiceFleet fleet;
	std::deque<JourneyVehicle> connectors;
	for (uint16_t route = 0; route < 80; ++route) {
		StationID from{static_cast<uint16_t>(route * 100 + 100)}, to{static_cast<uint16_t>(((route + 1) % 80) * 100 + 1)};
		connectors.emplace_back(std::vector<Order>{JourneyStop(from, 100000, 1), JourneyStop(to, 1, 1)}, VehicleID{30000u + route});
	}
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, ActivePassengerServiceGraphLimits(), PassengerServiceScope{A, StationID{8000}});
	REQUIRE(graph.GetQueryStatus(A, StationID{8000}) == PassengerPredictionStatus::Complete);
	CHECK(graph.GetDiagnostic().visits >= 80800);
	CHECK(graph.GetMemoryBound() <= 128 * 1024 * 1024);
	PassengerJourneyLimits limits; limits.work = 65536; limits.candidates = 1; limits.labels = 8192; limits.memory_bytes = 8 * 1024 * 1024;
	PassengerJourneyPlanner planner(graph);
	auto result = planner.Plan(PAX, A, StationID{8000}, NOW, limits);
	CHECK(result.status != PassengerPredictionStatus::Complete);
	CHECK_FALSE(result.IsUnreachable());
	CHECK(result.diagnostic.work <= limits.work);
}

TEST_CASE("Scoped standard work permits three hundred actual transfers", "[passenger][journey][scope]")
{
	PassengerTestStations stations;
	JourneyChain chain(300);
	for (auto &train : chain.trains) train.GetOrder(0)->SetTravelTime(100000);
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, ActivePassengerServiceGraphLimits(), PassengerServiceScope{A, StationID{301}});
	PassengerJourneyLimits limits; limits.work = 65536; limits.candidates = 1; limits.labels = 8192; limits.memory_bytes = 8 * 1024 * 1024;
	PassengerJourneyPlanner planner(graph);
	auto result = planner.Plan(PAX, A, StationID{301}, NOW, limits);
	REQUIRE(result.HasJourney());
	CHECK(result.status == PassengerPredictionStatus::Complete);
	CHECK(result.journeys.front().transfers == 300);
}

TEST_CASE("Service graph national comparison report", "[.service_graph_benchmark]")
{
	PassengerTestStations stations;
	NationalServiceFleet fleet;
	for (bool roomy : {false, true}) {
		auto limits = ActivePassengerServiceGraphLimits();
		if (roomy) limits.memory_bytes = 1024 * 1024 * 1024;
		InvalidatePassengerServiceIndex();
		uint64_t total_us = 0, max_us = 0, first_us = 0;
		size_t work = 0, max_work = 0, visits = 0, predictions = 0, retained = 0, bound = 0, static_bytes = 0, index_hits = 0, hits = 0, applied = 0;
		const auto start = std::chrono::steady_clock::now();
		for (uint iteration = 0; iteration < 10; ++iteration) {
			const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, StationID{8000}});
			const auto &diagnostic = graph.GetDiagnostic();
			total_us += diagnostic.build_microseconds; max_us = std::max(max_us, diagnostic.build_microseconds);
			if (iteration == 0) first_us = diagnostic.build_microseconds;
			work += graph.GetWork(); max_work = std::max(max_work, graph.GetWork());
			for (const auto &trip : graph.GetTrips()) { visits += trip.prediction.visits.size(); ++predictions; }
			retained = std::max(retained, diagnostic.allocation_bytes); bound = std::max(bound, graph.GetMemoryBound()); static_bytes = std::max(static_bytes, diagnostic.static_allocation_bytes);
			index_hits += diagnostic.static_cache_hit;
			PassengerJourneyPlanner planner(graph);
			PassengerJourneyLimits query; query.work = 65536; query.candidates = 1; query.labels = 8192; query.memory_bytes = 8 * 1024 * 1024;
			for (uint repeat = 0; repeat < 16; ++repeat) {
				auto result = planner.Plan(PAX, A, StationID{8000}, NOW, query);
				hits += result.diagnostic.cache_hit;
				applied += result.HasJourney() && result.status == PassengerPredictionStatus::Complete;
			}
		}
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		WARN("National comparison: roomy=" << roomy << "; objects=20000; network passenger visits=80800; builds=10; applied=" << applied << "/160; predictions/build=" << predictions / 10 << "; visits/build=" << visits / 10 << "; mean_work=" << work / 10 << "; max_work=" << max_work << "; cold_us=" << first_us << "; mean_us=" << total_us / 10 << "; max_us=" << max_us << "; total_graph_search_us=" << elapsed << "; retained=" << retained << "; static=" << static_bytes << "; bound=" << bound << "; index_hits=" << index_hits << "; OD_hits=" << hits << "/160");
	}
}

struct JourneyCapacity : PassengerJourneyCapacityProvider {
	std::map<VehicleID, PassengerJourneyCapacity> estimates;
	uint64_t revision = 1;
	uint64_t Revision() const override { return this->revision; }
	PassengerJourneyCapacity Estimate(const PassengerServiceGraph &graph, PassengerServiceGraph::VisitID board,
			PassengerServiceGraph::VisitID) const override
	{
		auto found = this->estimates.find(graph.GetTrips()[board.trip].vehicle);
		return found == this->estimates.end() ? PassengerJourneyCapacity{} : found->second;
	}
};

TEST_CASE("Passenger journeys retain direct local and express interchange alternatives", "[passenger][journey]")
{
	PassengerTestStations stations;
	JourneyVehicle local({ JourneyStop(A), JourneyStop(B, 100), JourneyStop(D, 200) });
	JourneyVehicle express({ JourneyStop(B, 10000, 130), JourneyStop(D, 40) }, VehicleID{101});
	PassengerJourneyEvaluation evaluation;
	SECTION("Actual departure wait and distinct preference costs") {
		const auto graph = JourneyGraph();
		PassengerJourneyPlanner planner(graph);
		auto result = planner.Plan(PAX, A, D, NOW);
		REQUIRE(result.journeys.size() == 2);
		CHECK(result.status == PassengerPredictionStatus::Complete);
		CHECK(result.journeys[0].arrival == NOW + 170);
		CHECK(result.journeys[0].waiting == StateTicksDelta{30});
		CHECK(result.journeys[0].riding == StateTicksDelta{140});
		CHECK(result.journeys[0].transfers == 1);
		CHECK(result.journeys[1].arrival == NOW + 320);
		CHECK(result.journeys[1].transfers == 0);
		REQUIRE(result.journeys[1].legs.size() == 1);
		CHECK(result.journeys[1].legs[0].vehicle == local.index);
		evaluation.transfer_penalty = 200;
		auto preferred = planner.Plan(PAX, A, D, NOW, {}, evaluation);
		REQUIRE(preferred.journeys.size() == 2);
		CHECK(preferred.journeys[0].transfers == 0);
		CHECK(preferred.journeys[1].arrival == NOW + 170);
		CHECK(preferred.journeys[1].generalized_time == StateTicksDelta{370});
	}
	SECTION("Through local travel is faster than the express") {
		express.GetOrder(1)->SetTravelTime(400);
		const auto graph = JourneyGraph();
		PassengerJourneyPlanner planner(graph);
		auto result = planner.Plan(PAX, A, D, NOW);
		REQUIRE(result.journeys.size() == 2);
		CHECK(result.journeys.front().legs.size() == 1);
		CHECK(result.journeys.front().transfers == 0);
		CHECK(result.journeys.front().arrival == NOW + 320);
	}
	SECTION("Minimum interchange and maximum transfer count") {
		const auto graph = JourneyGraph();
		PassengerJourneyPlanner planner(graph);
		evaluation.minimum_transfer = 40;
		auto missed = planner.Plan(PAX, A, D, NOW, {}, evaluation);
		REQUIRE(missed.journeys.size() == 1);
		CHECK(missed.journeys[0].transfers == 0);
		evaluation.minimum_transfer = 20;
		evaluation.transfer_time_weight = 1000;
		auto caught = planner.Plan(PAX, A, D, NOW, {}, evaluation);
		REQUIRE(caught.journeys.size() == 2);
		CHECK(caught.journeys[0].transfer_time == StateTicksDelta{20});
		CHECK(caught.journeys[0].generalized_time == StateTicksDelta{190});
		PassengerJourneyLimits limits;
		limits.max_transfers = 0;
		auto direct = planner.Plan(PAX, A, D, NOW, limits);
		REQUIRE(direct.journeys.size() == 1);
		CHECK(direct.journeys[0].transfers == 0);
	}
}

TEST_CASE("Passenger journeys compare low frequency shortcut and high capacity trunk", "[passenger][journey][capacity]")
{
	PassengerTestStations stations;
	JourneyVehicle local({ JourneyStop(A, 10000, 5500), JourneyStop(D, 1500) });
	JourneyVehicle trunk1({ JourneyStop(A, 10000, 500), JourneyStop(B, 1200) }, VehicleID{101});
	JourneyVehicle trunk2({ JourneyStop(B, 10000, 2000), JourneyStop(D, 1300) }, VehicleID{102});
	SECTION("Long wait makes the longer trunk journey faster") {
		const auto graph = JourneyGraph(8000);
		PassengerJourneyPlanner planner(graph);
		auto result = planner.Plan(PAX, A, D, NOW);
		REQUIRE(result.journeys.size() == 2);
		CHECK(result.journeys[0].arrival == NOW + 3300);
		CHECK(result.journeys[0].riding == StateTicksDelta{2500});
		CHECK(result.journeys[0].waiting == StateTicksDelta{800});
		CHECK(result.journeys[1].arrival == NOW + 7000);
	}
	SECTION("An imminent local train wins on time but loses with demand and real service capacity") {
		local.GetOrder(0)->SetWaitTime(100);
		const auto graph = JourneyGraph(8000);
		PassengerJourneyPlanner ordinary(graph);
		auto fastest = ordinary.Plan(PAX, A, D, NOW);
		REQUIRE(fastest.journeys.size() == 2);
		CHECK(fastest.journeys[0].legs[0].vehicle == local.index);
		JourneyCapacity provider;
		for (VehicleID vehicle : { local.index, trunk1.index, trunk2.index }) {
			auto &estimate = provider.estimates[vehicle];
			estimate.future_total = vehicle == local.index ? 80 : 100;
			estimate.future_free = *estimate.future_total;
			estimate.certainty = PassengerCapacityCertainty::Estimated;
			estimate.departures_in_window = vehicle == local.index ? 1 : 20;
			estimate.seats_per_departure = *estimate.future_total;
			estimate.demand_in_window = 500;
			estimate.window_ticks = StateTicksDelta{6000};
		}
		PassengerJourneyEvaluation evaluation;
		evaluation.capacity_shortfall_penalty = 6000;
		PassengerJourneyPlanner planner(graph, &provider);
		auto result = planner.Plan(PAX, A, D, NOW, {}, evaluation);
		REQUIRE(result.journeys.size() == 2);
		CHECK(result.journeys[0].legs[0].vehicle == trunk1.index);
		CHECK(result.journeys[1].capacity_penalty == StateTicksDelta{5040});
		CHECK(result.journeys[1].arrival == NOW + 1600);
		CHECK(result.journeys[0].capacity_uncertain); // Estimate, not a seat reservation.
		provider.estimates[local.index].certainty = PassengerCapacityCertainty::Unknown;
		provider.estimates[local.index].future_free.reset();
		++provider.revision;
		auto revised = planner.Plan(PAX, A, D, NOW, {}, evaluation);
		CHECK_FALSE(revised.diagnostic.cache_hit);
	}
}

TEST_CASE("Unknown future seats remain unknown and full predictions require a later physical train", "[passenger][journey][capacity]")
{
	PassengerTestStations stations;
	JourneyVehicle first({ JourneyStop(A), JourneyStop(D, 200) });
	JourneyVehicle next({ JourneyStop(A, 10000, 100), JourneyStop(D, 200) }, VehicleID{101});
	const auto graph = JourneyGraph();
	PassengerJourneyPlanner unknown(graph);
	auto uncertain = unknown.Plan(PAX, A, D, NOW);
	REQUIRE(uncertain.journeys.size() == 2);
	for (const auto &journey : uncertain.journeys) {
		CHECK(journey.capacity_uncertain);
		CHECK(journey.legs[0].capacity.observed_total == 80);
		CHECK(journey.legs[0].capacity.observed_occupants == 0);
		CHECK(journey.legs[0].capacity.current_free == 80);
		CHECK_FALSE(journey.legs[0].capacity.future_free.has_value());
		CHECK(journey.legs[0].capacity.certainty == PassengerCapacityCertainty::Unknown);
	}
	JourneyCapacity provider;
	provider.estimates[first.index].future_total = 80;
	provider.estimates[first.index].future_free = 0;
	provider.estimates[first.index].certainty = PassengerCapacityCertainty::Confirmed;
	provider.estimates[next.index].future_total = 80;
	provider.estimates[next.index].future_free = 20;
	provider.estimates[next.index].certainty = PassengerCapacityCertainty::Estimated;
	PassengerJourneyPlanner planner(graph, &provider);
	auto result = planner.Plan(PAX, A, D, NOW);
	REQUIRE(result.journeys.size() == 1);
	CHECK(result.journeys[0].legs[0].vehicle == next.index);
	CHECK(result.journeys[0].arrival == NOW + 300);
	CHECK(result.diagnostic.capacity_rejections > 0);
}

TEST_CASE("Capacity preference ratios avoid intermediate overflow and unknown guarantees", "[passenger][journey][capacity]")
{
	PassengerTestStations stations;
	JourneyVehicle train({JourneyStop(A), JourneyStop(D, 100)});
	const auto graph = JourneyGraph();
	JourneyCapacity provider;
	auto &estimate = provider.estimates[train.index];
	estimate.future_total = UINT64_MAX;
	estimate.future_free = UINT64_MAX / 2;
	estimate.certainty = PassengerCapacityCertainty::Estimated;
	estimate.window_ticks = StateTicksDelta{1000};
	estimate.departures_in_window = 0;
	estimate.seats_per_departure = 80;
	estimate.demand_in_window = UINT64_MAX;
	PassengerJourneyEvaluation evaluation;
	evaluation.crowding_penalty = 1000;
	evaluation.capacity_shortfall_penalty = 1000;
	PassengerJourneyPlanner planner(graph, &provider);
	auto result = planner.Plan(PAX, A, D, NOW, {}, evaluation);
	REQUIRE(result.journeys.size() == 1);
	CHECK(result.journeys[0].capacity_penalty == StateTicksDelta{1500});
	CHECK(result.journeys[0].generalized_time == StateTicksDelta{1610});
	estimate.certainty = PassengerCapacityCertainty::Unknown;
	++provider.revision;
	auto unknown = planner.Plan(PAX, A, D, NOW, {}, evaluation);
	REQUIRE(unknown.journeys.size() == 1);
	CHECK_FALSE(unknown.journeys[0].legs[0].capacity.future_free.has_value());
	CHECK(unknown.journeys[0].capacity_uncertain);
	CHECK(unknown.journeys[0].capacity_penalty == StateTicksDelta{1000});
}

TEST_CASE("Capacity tie breaks prefer usable confirmations without ranking estimates as guarantees", "[passenger][journey][capacity]")
{
	PassengerTestStations stations;
	JourneyVehicle unknown({JourneyStop(A), JourneyStop(D, 100)}, VehicleID{100});
	JourneyVehicle estimated({JourneyStop(A), JourneyStop(D, 100)}, VehicleID{101});
	JourneyVehicle confirmed({JourneyStop(A), JourneyStop(D, 100)}, VehicleID{102});
	JourneyCapacity provider;
	provider.estimates[estimated.index].certainty = PassengerCapacityCertainty::Estimated;
	provider.estimates[estimated.index].future_free = 80;
	provider.estimates[confirmed.index].certainty = PassengerCapacityCertainty::Confirmed;
	provider.estimates[confirmed.index].future_free = 80;
	SECTION("A usable confirmation precedes tied unknown/estimated paths") {
		const auto graph = JourneyGraph();
		PassengerJourneyPlanner planner(graph, &provider);
		auto result = planner.Plan(PAX, A, D, NOW);
		REQUIRE(result.journeys.size() == 3);
		CHECK(result.journeys[0].legs[0].vehicle == confirmed.index);
		CHECK_FALSE(result.journeys[0].capacity_uncertain);
		CHECK(result.journeys[1].legs[0].vehicle == unknown.index);
		CHECK(result.journeys[2].legs[0].vehicle == estimated.index);
		PassengerJourneyLimits limits;
		limits.labels_per_station = 1;
		auto pruned = planner.Plan(PAX, A, D, NOW, limits);
		REQUIRE(pruned.journeys.size() == 1);
		CHECK(pruned.journeys[0].legs[0].vehicle == confirmed.index);
		CHECK(pruned.status == PassengerPredictionStatus::LimitReached);
	}
	SECTION("Confirmed without a seat count stays uncertain") {
		provider.estimates[unknown.index].certainty = PassengerCapacityCertainty::Confirmed;
		provider.estimates[confirmed.index].future_free.reset();
		const auto graph = JourneyGraph();
		PassengerJourneyPlanner planner(graph, &provider);
		auto result = planner.Plan(PAX, A, D, NOW);
		REQUIRE(result.journeys.size() == 3);
		CHECK(result.journeys[0].legs[0].vehicle == unknown.index);
		for (const auto &journey : result.journeys) CHECK(journey.capacity_uncertain);
	}
	SECTION("A faster estimate retains its time preference") {
		estimated.GetOrder(1)->SetTravelTime(90);
		const auto graph = JourneyGraph();
		PassengerJourneyPlanner planner(graph, &provider);
		auto result = planner.Plan(PAX, A, D, NOW);
		REQUIRE(result.journeys.size() == 3);
		CHECK(result.journeys[0].legs[0].vehicle == estimated.index);
		CHECK(result.journeys[1].legs[0].vehicle == confirmed.index);
	}
}

TEST_CASE("Journey labels allocate incrementally within their caps", "[passenger][journey]")
{
	PassengerTestStations stations;
	SECTION("A short query reserves a small label vector") {
		JourneyVehicle train({JourneyStop(A), JourneyStop(D, 100)});
		const auto graph = JourneyGraph();
		PassengerJourneyPlanner planner(graph, nullptr, {0, 0});
		auto result = planner.Plan(PAX, A, D, NOW);
		REQUIRE(result.journeys.size() == 1);
		CHECK(result.diagnostic.labels == 2);
		CHECK(result.diagnostic.label_capacity == 32);
		CHECK(result.diagnostic.scratch_bytes < 64 * 1024);
	}
	SECTION("Longer searches grow instead of reserving the full cap") {
		std::vector<Order> stops{JourneyStop(A)};
		for (uint16_t i = 1; i <= 100; ++i) stops.push_back(JourneyStop(StationID{i}, 1, 1));
		JourneyVehicle train(stops);
		const auto graph = JourneyGraph();
		PassengerJourneyPlanner planner(graph);
		auto result = planner.Plan(PAX, A, StationID{100}, NOW);
		REQUIRE(result.journeys.size() == 1);
		CHECK(result.diagnostic.labels == 101);
		CHECK(result.diagnostic.label_capacity == 128);
		PassengerJourneyLimits limits;
		limits.labels = 50;
		auto limited = planner.Plan(PAX, A, StationID{100}, NOW, limits);
		CHECK(limited.status == PassengerPredictionStatus::LimitReached);
		CHECK(limited.diagnostic.labels == 50);
		CHECK(limited.diagnostic.label_capacity <= limits.labels);
		CHECK_FALSE(limited.IsUnreachable());
		limits = {};
		limits.memory_bytes = 4096;
		auto memory_limited = planner.Plan(PAX, A, StationID{100}, NOW, limits);
		CHECK(memory_limited.status == PassengerPredictionStatus::LimitReached);
		CHECK(memory_limited.diagnostic.labels == 0); // Reverse reachability shares the same memory budget.
		CHECK(memory_limited.diagnostic.scratch_bytes <= limits.memory_bytes);
		CHECK_FALSE(memory_limited.IsUnreachable());
	}
}

TEST_CASE("Reverse journey pruning preserves unknown connections beyond captured visits", "[passenger][journey]")
{
	PassengerTestStations stations;
	JourneyVehicle feeder({JourneyStop(A), JourneyStop(B, 100)});
	Order unknown = JourneyStop(C, 100);
	unknown.SetWaitTimetabled(false);
	JourneyVehicle branch({JourneyStop(B, 10000, 120), unknown, JourneyStop(D, 100)}, VehicleID{101});
	const auto graph = JourneyGraph();
	PassengerJourneyPlanner planner(graph);
	auto result = planner.Plan(PAX, A, D, NOW);
	CHECK(result.status == PassengerPredictionStatus::Unknown);
	CHECK_FALSE(result.IsUnreachable());
	CHECK(result.journeys.empty());
	/* Direct-only pruning must not turn a possible interchange into certified absence. */
	PassengerJourneyLimits limits;
	limits.max_transfers = 0;
	auto direct = planner.Plan(PAX, A, D, NOW, limits);
	CHECK(direct.IsInconclusive());
	CHECK(direct.stop_reason == PassengerJourneyStopReason::PredictionIncomplete);
	CHECK(direct.GetOutcome() == PassengerJourneyOutcome::NoTimedRoute);
}

TEST_CASE("Journey destination lower bounds retain faster interchanges and honor preference costs", "[passenger][journey]")
{
	PassengerTestStations stations;
	std::deque<JourneyVehicle> direct;
	for (uint32_t i = 0; i < 8; ++i) direct.emplace_back(std::vector<Order>{JourneyStop(A), JourneyStop(D, 300)}, VehicleID{100 + i});
	JourneyVehicle feeder({JourneyStop(A), JourneyStop(B, 40)}, VehicleID{200});
	JourneyVehicle express({JourneyStop(B, 10000, 70), JourneyStop(D, 50)}, VehicleID{201});
	PassengerJourneyEvaluation evaluation;
	SECTION("A faster interchange displaces an already complete direct frontier") {}
	SECTION("Transfer costs make direct travel preferable") { evaluation.transfer_penalty = 300; }
	SECTION("Weighted waiting participates in the lower bound") { evaluation.waiting_weight = 10000; }
	SECTION("Minimum interchange prevents catching the express") { evaluation.minimum_transfer = 30; }
	const auto graph = JourneyGraph();
	PassengerJourneyPlanner planner(graph, nullptr, {0, 0});
	PassengerJourneyLimits all_limits;
	all_limits.candidates = 64; // This fixture has fewer candidates; the completed-frontier bound cannot prune.
	auto all = planner.Plan(PAX, A, D, NOW, all_limits, evaluation);
	PassengerJourneyLimits limits;
	limits.candidates = 1;
	auto best = planner.Plan(PAX, A, D, NOW, limits, evaluation);
	REQUIRE_FALSE(all.journeys.empty());
	REQUIRE(best.journeys.size() == 1);
	CHECK(best.status == PassengerPredictionStatus::Complete);
	CHECK(best.journeys[0] == all.journeys[0]);
	if (evaluation.minimum_transfer == 0 && evaluation.transfer_penalty == 0 && evaluation.waiting_weight == 0) {
		CHECK(best.journeys[0].transfers == 1);
		CHECK(best.journeys[0].arrival == NOW + 120);
	} else {
		CHECK(best.journeys[0].transfers == 0);
	}
}

TEST_CASE("Journey boarding and alighting restrictions and repeated station occurrences", "[passenger][journey]")
{
	PassengerTestStations stations;
	Order middle = JourneyStop(B, 100);
	SECTION("NoLoad at the origin") {
		Order origin = JourneyStop(A);
		origin.SetLoadType(OrderLoadType::NoLoad);
		JourneyVehicle train({ origin, middle, JourneyStop(D, 100) });
		const auto graph = JourneyGraph();
		PassengerJourneyPlanner planner(graph);
		CHECK(planner.Plan(PAX, A, D, NOW).IsUnreachable());
	}
	SECTION("NoUnload prevents a destination but permits through riding") {
		middle.SetUnloadType(OrderUnloadType::NoUnload);
		JourneyVehicle train({ JourneyStop(A), middle, JourneyStop(D, 100) });
		const auto graph = JourneyGraph();
		PassengerJourneyPlanner planner(graph);
		CHECK(planner.Plan(PAX, A, B, NOW).IsUnreachable());
		auto through = planner.Plan(PAX, A, D, NOW);
		REQUIRE(through.journeys.size() == 1);
		CHECK(through.journeys[0].legs.size() == 1);
		CHECK(through.journeys[0].transfers == 0);
	}
	SECTION("Forced transfer ends a leg and requires explicit catchable reboarding") {
		middle.SetUnloadType(OrderUnloadType::Transfer);
		JourneyVehicle train({ JourneyStop(A), middle, JourneyStop(D, 100) });
		const auto graph = JourneyGraph();
		PassengerJourneyPlanner planner(graph);
		auto result = planner.Plan(PAX, A, D, NOW);
		REQUIRE(result.journeys.size() == 1);
		CHECK(result.journeys[0].legs.size() == 2);
		CHECK(result.journeys[0].legs[0].alighting == result.journeys[0].legs[1].boarding);
		PassengerJourneyEvaluation evaluation;
		evaluation.minimum_transfer = 11;
		CHECK(planner.Plan(PAX, A, D, NOW, {}, evaluation).IsInconclusive());
	}
	SECTION("Reversal and revisit retain distinct snapshot VisitIDs") {
		Order reversal;
		reversal.MakeGoToWaypoint(StationID{20});
		reversal.SetWaypointFlags(OrderWaypointFlags{OrderWaypointFlag::Reverse});
		reversal.SetTravelTime(10);
		reversal.SetTravelTimetabled(true);
		JourneyVehicle train({ JourneyStop(A), middle, reversal, JourneyStop(A, 100), JourneyStop(D, 100) });
		const auto graph = JourneyGraph();
		PassengerJourneyPlanner planner(graph);
		auto result = planner.Plan(PAX, A, D, NOW);
		REQUIRE_FALSE(result.journeys.empty());
		CHECK(result.journeys[0].transfers == 0);
		CHECK(result.journeys[0].legs[0].alighting.visit == 3);
		CHECK(result.journeys[0].legs[0].boarding.visit < 3);
	}
}

TEST_CASE("Journey uncertainty and exhausted searches never certify unreachable", "[passenger][journey]")
{
	PassengerTestStations stations;
	JourneyVehicle train({ JourneyStop(A), JourneyStop(B, 100), JourneyStop(D, 100) });
	SECTION("Unknown origin departure") { train.GetOrder(0)->SetWaitTimetabled(false); }
	SECTION("Unknown intermediate dwell") { train.GetOrder(1)->SetWaitTimetabled(false); }
	SECTION("Dynamic condition") {
		train.GetOrder(1)->MakeConditional(2);
		train.GetOrder(1)->SetConditionVariable(OrderConditionVariable::LoadPercentage);
	}
	SECTION("Refit") { train.GetOrder(1)->SetRefit(CargoType{1}); }
	SECTION("Stopped trains do not generate synthetic services") {
		train.vehstatus.Set(VehState::Stopped);
		const auto graph = JourneyGraph();
		PassengerJourneyPlanner planner(graph);
		CHECK(planner.Plan(PAX, A, D, NOW).IsUnreachable());
		return;
	}
	SECTION("Work exhaustion") {
		const auto graph = JourneyGraph();
		PassengerJourneyPlanner planner(graph);
		PassengerJourneyLimits limits;
		limits.work = 1;
		auto result = planner.Plan(PAX, A, D, NOW, limits);
		CHECK(result.status == PassengerPredictionStatus::BudgetExhausted);
		CHECK_FALSE(result.IsUnreachable());
		CHECK(result.diagnostic.work <= limits.work);
		return;
	}
	SECTION("Label memory exhaustion") {
		const auto graph = JourneyGraph();
		PassengerJourneyPlanner planner(graph);
		PassengerJourneyLimits limits;
		limits.memory_bytes = 1;
		auto result = planner.Plan(PAX, A, D, NOW, limits);
		CHECK(result.status == PassengerPredictionStatus::LimitReached);
		CHECK_FALSE(result.IsUnreachable());
		return;
	}
	SECTION("Prediction visit limit") {
		PassengerServiceGraphLimits limits;
		limits.visits_per_trip = 1;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		PassengerJourneyPlanner planner(graph);
		auto result = planner.Plan(PAX, A, D, NOW);
		CHECK(result.status == PassengerPredictionStatus::LimitReached);
		CHECK_FALSE(result.IsUnreachable());
		return;
	}
	const auto graph = JourneyGraph();
	PassengerJourneyPlanner planner(graph);
	auto result = planner.Plan(PAX, A, D, NOW);
	CHECK_FALSE(result.IsUnreachable());
	CHECK(result.status == PassengerPredictionStatus::Unknown);
}

TEST_CASE("Journey event interval cache and departure index preserve deterministic routing", "[passenger][journey]")
{
	PassengerTestStations stations;
	JourneyVehicle later({ JourneyStop(A, 10000, 30), JourneyStop(D, 100) }, VehicleID{102});
	JourneyVehicle earlier({ JourneyStop(A, 10000, 10), JourneyStop(D, 200) }, VehicleID{101});
	JourneyVehicle tied({ JourneyStop(A, 10000, 10), JourneyStop(D, 200) }, VehicleID{100});
	const auto graph = JourneyGraph();
	auto departures = graph.GetDepartures(A, NOW, graph.GetEnd());
	REQUIRE(departures.timed.size() == 3);
	CHECK(graph.GetTrips()[departures.timed[0].trip].vehicle == tied.index);
	CHECK(graph.GetTrips()[departures.timed[1].trip].vehicle == earlier.index);
	CHECK(graph.GetTrips()[departures.timed[2].trip].vehicle == later.index);
	PassengerJourneyPlanner planner(graph), uncached(graph, nullptr, {0, 0});
	GameRandomSeedChecker seeds;
	auto first = planner.Plan(PAX, A, D, NOW);
	auto cached = planner.Plan(PAX, A, D, NOW);
	CHECK(cached.diagnostic.cache_hit);
	CHECK(first.journeys == cached.journeys);
	auto shifted = planner.Plan(PAX, A, D, NOW + 5);
	CHECK(shifted.diagnostic.cache_hit);
	CHECK(shifted.journeys == uncached.Plan(PAX, A, D, NOW + 5).journeys);
	CHECK(first.journeys[0].waiting - StateTicksDelta{5} == shifted.journeys[0].waiting);
	auto missed = planner.Plan(PAX, A, D, NOW + 11);
	CHECK_FALSE(missed.diagnostic.cache_hit);
	REQUIRE(missed.journeys.size() == 1);
	CHECK(missed.journeys[0].legs[0].vehicle == later.index);
	planner.ClearCache();
	CHECK(planner.Plan(PAX, A, D, NOW).journeys == first.journeys);
	CHECK(seeds.Check());
	CHECK(planner.Plan(PAX, A, A, NOW).journeys[0].arrival == NOW);
	CHECK(planner.Plan(PAX, A, D, NOW - 1).status == PassengerPredictionStatus::Unknown);
	CHECK(planner.Plan(CargoType{1}, A, D, NOW).status == PassengerPredictionStatus::Unknown);
	PassengerJourneyEvaluation fractional;
	fractional.waiting_weight = 333;
	CHECK(planner.Plan(PAX, A, D, NOW, {}, fractional).journeys == uncached.Plan(PAX, A, D, NOW, {}, fractional).journeys);
	CHECK(planner.Plan(PAX, A, D, NOW + 5, {}, fractional).journeys == uncached.Plan(PAX, A, D, NOW + 5, {}, fractional).journeys);
	PassengerJourneyPlanner tiny_cache(graph, nullptr, {1, 4096});
	CHECK(tiny_cache.Plan(PAX, A, D, NOW).journeys == first.journeys);
	tiny_cache.Plan(PAX, A, D, NOW + 11);
	CHECK(tiny_cache.GetCacheEntries() <= 1);
	CHECK(tiny_cache.GetCacheBytes() <= 4096);
	CHECK(tiny_cache.Plan(PAX, A, D, NOW).journeys == first.journeys);
}

TEST_CASE("Journey partial candidates and route diversity are retained under explicit limits", "[passenger][journey]")
{
	PassengerTestStations stations;
	JourneyVehicle first({JourneyStop(A), JourneyStop(D, 100)});
	JourneyVehicle second({JourneyStop(A, 10000, 30), JourneyStop(D, 200)}, VehicleID{101});
	const auto graph = JourneyGraph();
	PassengerJourneyPlanner planner(graph), uncached(graph, nullptr, {0, 0});
	PassengerJourneyLimits limits;
	limits.max_transfers = 0;
	SECTION("Insufficient work after finding a valid journey") {
		limits.work = uncached.Plan(PAX, A, D, NOW, limits).diagnostic.reverse_work + 6;
		auto result = planner.Plan(PAX, A, D, NOW, limits);
		CHECK(result.status == PassengerPredictionStatus::BudgetExhausted);
		REQUIRE(result.journeys.size() == 1);
		CHECK(result.journeys[0].arrival == NOW + 110);
		CHECK(planner.Plan(PAX, A, D, NOW, limits).journeys == result.journeys);
		CHECK(uncached.Plan(PAX, A, D, NOW, limits).journeys == result.journeys);
	}
	SECTION("A label cap explicitly reports pruned alternatives") {
		limits.labels_per_station = 1;
		auto result = planner.Plan(PAX, A, D, NOW, limits);
		CHECK(result.status == PassengerPredictionStatus::LimitReached);
		REQUIRE(result.journeys.size() == 1);
		CHECK(result.journeys[0].legs[0].vehicle == first.index);
	}
}

TEST_CASE("Journey circular trains and scheduled dispatch use real future occurrences", "[passenger][journey]")
{
	PassengerTestStations stations;
	SECTION("Forced unloading can wait for the same physical train on its next circuit") {
		Order transfer = JourneyStop(B, 100, 10);
		transfer.SetUnloadType(OrderUnloadType::Transfer);
		JourneyVehicle train({JourneyStop(A, 100, 10), transfer, JourneyStop(D, 100, 10)});
		const auto graph = JourneyGraph(1000);
		PassengerJourneyPlanner planner(graph);
		PassengerJourneyEvaluation evaluation;
		evaluation.minimum_transfer = 11;
		auto result = planner.Plan(PAX, A, D, NOW, {}, evaluation);
		REQUIRE_FALSE(result.journeys.empty());
		CHECK(result.journeys[0].arrival == NOW + 550);
		REQUIRE(result.journeys[0].legs.size() == 2);
		CHECK(result.journeys[0].legs[0].alighting.visit == 1);
		CHECK(result.journeys[0].legs[1].boarding.visit == 4);
		CHECK(result.journeys[0].transfers == 1);
	}
	SECTION("Next circular visit is distinct and staying aboard adds no transfer") {
		JourneyVehicle train({ JourneyStop(A, 100, 10), JourneyStop(B, 100, 10), JourneyStop(D, 100, 10) });
		const auto graph = JourneyGraph(1000);
		PassengerJourneyPlanner planner(graph);
		auto result = planner.Plan(PAX, A, D, NOW + 250);
		REQUIRE_FALSE(result.journeys.empty());
		CHECK(result.journeys[0].arrival == NOW + 550);
		CHECK(result.journeys[0].transfers == 0);
		CHECK(result.journeys[0].legs[0].boarding.visit == 3);
		CHECK(result.journeys[0].legs[0].alighting.visit == 5);
		CHECK(result.journeys[0].waiting == StateTicksDelta{90});
	}
	SECTION("Scheduled dispatch delays the actual departure and through ride") {
		JourneyVehicle train({ JourneyStop(A, 10000, 0), JourneyStop(B, 10, 0), JourneyStop(D, 10, 0) });
		train.GetOrder(1)->SetDispatchScheduleIndex(0);
		train.vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
		train.orders->GetScheduledDispatchScheduleSet().resize(1);
		auto &schedule = train.orders->GetDispatchScheduleByIndex(0);
		schedule.SetScheduledDispatchStartTick(NOW);
		schedule.SetScheduledDispatchDuration(1000);
		schedule.GetScheduledDispatchMutable() = { {100, 0, {}}, {350, 0, {}} };
		const auto graph = JourneyGraph(500);
		PassengerJourneyPlanner planner(graph);
		auto through = planner.Plan(PAX, A, D, NOW);
		REQUIRE(through.journeys.size() == 1);
		CHECK(through.journeys[0].arrival == NOW + 110);
		CHECK(through.journeys[0].riding == StateTicksDelta{110});
		CHECK(through.journeys[0].transfers == 0);
		auto boarding = planner.Plan(PAX, B, D, NOW);
		REQUIRE(boarding.journeys.size() == 1);
		CHECK(boarding.journeys[0].legs[0].departure == NOW + 100);
		CHECK(boarding.journeys[0].waiting == StateTicksDelta{100});
		CHECK(schedule.GetScheduledDispatchLastDispatch() == INVALID_SCHEDULED_DISPATCH_OFFSET);
	}
}

TEST_CASE("National journey queries reach eight thousand stations without whole network rescans", "[passenger][journey][performance][aircraft_cost]")
{
	PassengerTestStations stations;
	AutoRestoreBackup settings{_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{}};
	_settings_game.linkgraph.aircraft_link_scale = GENERATE(uint16_t{100}, uint16_t{50}, uint16_t{300});
	_settings_game.linkgraph.aircraft_link_add = _settings_game.linkgraph.aircraft_link_scale == 300 ? 3 : 0;
	/* 80 routes of 100 stations, each with 10 physical trains: 8000 stations and 80000 visits.
	 * The station-0 hub reaches every route in one boarding round. No synthetic headway expansion. */
	std::deque<JourneyVehicle> fleet;
	for (uint16_t route = 0; route < 80; ++route) {
		std::vector<Order> orders{JourneyStop(A, 100000, 1)};
		for (uint16_t station = 1; station <= 100; ++station) orders.push_back(JourneyStop(StationID{static_cast<uint16_t>(route * 100 + station)}, 1, 1));
		for (uint16_t train = 0; train < 10; ++train) fleet.emplace_back(orders, VehicleID{static_cast<uint32_t>(1000 + route * 10 + train)});
	}
	StationID origin = A;
	std::optional<JourneyVehicle> feeder;
	SECTION("Hub query") {}
	SECTION("A feeder interchange into the last hub route") {
		origin = StationID{8001};
		feeder.emplace(std::vector<Order>{JourneyStop(origin, 100000, 0), JourneyStop(A, 1, 1)}, VehicleID{2000});
	}
	PassengerServiceGraphLimits graph_limits;
	graph_limits.horizon = 1000;
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, graph_limits);
	CHECK_FALSE(graph.HasAircraftDiscount());
	REQUIRE(graph.GetTrips().size() == (feeder ? 801 : 800));
	size_t visits = 0;
	for (const auto &trip : graph.GetTrips()) visits += trip.prediction.visits.size();
	REQUIRE(visits >= 80000);
	PassengerJourneyLimits limits; // Initial four transfers and the original work/label caps.
	PassengerJourneyPlanner planner(graph);
	const auto start = std::chrono::steady_clock::now();
	auto result = planner.Plan(PAX, origin, StationID{8000}, NOW);
	const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	REQUIRE_FALSE(result.journeys.empty());
	CHECK(result.status == PassengerPredictionStatus::Complete);
	CHECK(result.journeys[0].arrival == NOW + 200);
	CHECK(result.journeys[0].transfers == (feeder ? 1 : 0));
	CHECK(result.diagnostic.labels <= (feeder ? 1002 : 1001));
	CHECK(result.diagnostic.label_capacity <= 1024);
	CHECK(result.diagnostic.boardings == (feeder ? 11 : 10));
	CHECK(result.diagnostic.pruned_boardings == (feeder ? 791 : 790));
	CHECK(result.diagnostic.pruned_labels > 0);
	CHECK(result.diagnostic.visits == (feeder ? 1001 : 1000));
	CHECK(result.diagnostic.reverse_work < 10000);
	CHECK(result.diagnostic.work < 65536);
	CHECK(result.diagnostic.work <= limits.work);
	CHECK(result.diagnostic.scratch_bytes <= limits.memory_bytes);
	CHECK(result.diagnostic.scratch_bytes < 2 * 1024 * 1024);
	CHECK(planner.Plan(PAX, origin, StationID{8000}, NOW).diagnostic.cache_hit);
	/* A leaf query examines one route, not 80000 unrelated visits. */
	auto leaf = planner.Plan(PAX, StationID{7901}, StationID{8000}, NOW);
	REQUIRE_FALSE(leaf.journeys.empty());
	CHECK(leaf.status == PassengerPredictionStatus::Complete);
	CHECK(leaf.diagnostic.visits < 2000);
	WARN("National rail journey: scale=" << _settings_game.linkgraph.aircraft_link_scale << "; add=" << _settings_game.linkgraph.aircraft_link_add << "; " << us << " us; trips=" << graph.GetTrips().size() << "; visits=" << visits << "; labels=" << result.diagnostic.labels << "; work=" << result.diagnostic.work << "; reverse work=" << result.diagnostic.reverse_work << "; scratch peak=" << result.diagnostic.scratch_bytes);
}

TEST_CASE("Journey stages continue actual train chains without replaying earlier rounds", "[passenger][journey][transfer-limits]")
{
	PassengerTestStations stations;
	const uint32_t transfers = GENERATE(0u, 4u, 5u, 9u, 15u, 33u, 300u);
	CAPTURE(transfers);
	JourneyChain chain(transfers);
	const auto graph = JourneyGraph(20 * (transfers + 2));
	const StationID destination{static_cast<uint16_t>(transfers + 1)};
	PassengerJourneyPlanner planner(graph), uncached(graph, nullptr, {0, 0});
	GameRandomSeedChecker seeds;
	auto result = planner.Plan(PAX, A, destination, NOW);
	REQUIRE(result.HasJourney());
	REQUIRE(result.journeys.size() == 1);
	CHECK(result.status == PassengerPredictionStatus::Complete);
	CHECK(result.stop_reason == PassengerJourneyStopReason::Exhausted);
	const auto &journey = result.journeys[0];
	CHECK(journey.transfers == transfers);
	CHECK(journey.arrival == NOW + 20 * (transfers + 1));
	REQUIRE(journey.legs.size() == transfers + 1);
	for (uint32_t i = 0; i <= transfers; ++i) {
		const auto &leg = journey.legs[i];
		CHECK(leg.vehicle == chain.trains[i].index);
		CHECK(leg.departure == NOW + 10 + 20 * i);
		CHECK(leg.arrival == NOW + 20 * (i + 1));
		CHECK(graph.GetTrips()[leg.boarding.trip].prediction.visits[leg.boarding.visit].departure == leg.departure);
		CHECK(graph.GetTrips()[leg.alighting.trip].prediction.visits[leg.alighting.visit].arrival == leg.arrival);
	}
	CHECK(result.diagnostic.rounds == transfers + 1);
	CHECK(result.diagnostic.labels == transfers + 2);
	CHECK(result.diagnostic.boardings == transfers + 1);
	uint32_t expected_extensions = 0, stage = 4;
	while (stage < transfers) { stage *= 2; ++expected_extensions; }
	CHECK(result.diagnostic.extensions == expected_extensions);
	PassengerJourneyLimits single_stage;
	single_stage.initial_max_transfers = stage;
	auto single = uncached.Plan(PAX, A, destination, NOW, single_stage);
	CHECK(single.journeys == result.journeys);
	CHECK(single.diagnostic.extensions == 0);
	/* Equal accounting proves that extending the frontier did not recalculate the prefix. */
	CHECK(single.diagnostic.work == result.diagnostic.work);
	CHECK(single.diagnostic.reverse_work == result.diagnostic.reverse_work);
	CHECK(single.diagnostic.labels == result.diagnostic.labels);
	CHECK(single.diagnostic.scratch_bytes == result.diagnostic.scratch_bytes);
	CHECK(result.diagnostic.work <= PassengerJourneyLimits{}.work);
	CHECK(result.diagnostic.scratch_bytes <= PassengerJourneyLimits{}.memory_bytes);
	auto cached = planner.Plan(PAX, A, destination, NOW);
	CHECK(cached.diagnostic.cache_hit);
	CheckSameJourneyResult(result, cached);
	CheckSameJourneyResult(result, uncached.Plan(PAX, A, destination, NOW));
	CHECK(seeds.Check());
	WARN("Transfer chain: transfers=" << transfers << "; rounds=" << result.diagnostic.rounds << "; extensions=" << result.diagnostic.extensions
			<< "; work=" << result.diagnostic.work << "; labels=" << result.diagnostic.labels << "; scratch=" << result.diagnostic.scratch_bytes);
}

TEST_CASE("Journey candidates stop extension at the current stage and disclose unsearched alternatives", "[passenger][journey][transfer-limits]")
{
	PassengerTestStations stations;
	JourneyChain chain(5);
	const StationID destination{6};
	JourneyVehicle direct({JourneyStop(A), JourneyStop(destination, 500)}, VehicleID{2000});
	const auto graph = JourneyGraph();
	PassengerJourneyPlanner planner(graph);
	auto result = planner.Plan(PAX, A, destination, NOW);
	REQUIRE(result.journeys.size() == 1);
	CHECK(result.journeys[0].transfers == 0);
	CHECK(result.diagnostic.rounds == 5);
	CHECK(result.diagnostic.extensions == 0);
	CHECK(result.status == PassengerPredictionStatus::LimitReached);
	CHECK(result.stop_reason == PassengerJourneyStopReason::TransferLimit);
	CHECK_FALSE(result.IsInconclusive()); // An end-to-end candidate remains usable, despite incomplete alternatives.
	PassengerJourneyLimits limits;
	limits.initial_max_transfers = 8;
	auto wider = planner.Plan(PAX, A, destination, NOW, limits);
	REQUIRE(wider.journeys.size() == 2);
	CHECK(wider.journeys[0].transfers == 5);
	CHECK_FALSE(wider.diagnostic.cache_hit);
	limits.max_transfers = 0;
	auto only_direct = planner.Plan(PAX, A, destination, NOW, limits);
	REQUIRE(only_direct.journeys.size() == 1);
	CHECK(only_direct.journeys[0].transfers == 0);
	CHECK(only_direct.diagnostic.rounds == 1);
}

TEST_CASE("Journey safety ceilings and resource limits remain inconclusive and cache consistently", "[passenger][journey][transfer-limits]")
{
	PassengerTestStations stations;
	JourneyChain chain(9);
	const auto graph = JourneyGraph();
	const StationID destination{10};
	PassengerJourneyPlanner planner(graph), uncached(graph, nullptr, {0, 0});
	PassengerJourneyLimits limits;
	PassengerJourneyStopReason reason;
	SECTION("Explicit direct-only request") { limits.max_transfers = 0; reason = PassengerJourneyStopReason::TransferLimit; }
	SECTION("Transfer safety ceiling") { limits.max_transfers = 8; reason = PassengerJourneyStopReason::TransferLimit; }
	SECTION("Work exhaustion") { limits.work = 100; reason = PassengerJourneyStopReason::WorkLimit; }
	SECTION("Label exhaustion") { limits.labels = 4; reason = PassengerJourneyStopReason::LabelLimit; }
	SECTION("Memory exhaustion") { limits.memory_bytes = 4096; reason = PassengerJourneyStopReason::MemoryLimit; }
	auto result = planner.Plan(PAX, A, destination, NOW, limits);
	CHECK(result.journeys.empty());
	CHECK_FALSE(result.IsUnreachable());
	CHECK(result.IsInconclusive());
	CHECK(result.stop_reason == reason);
	CHECK(result.diagnostic.work <= limits.work);
	CHECK(result.diagnostic.scratch_bytes <= limits.memory_bytes);
	CheckSameJourneyResult(result, planner.Plan(PAX, A, destination, NOW, limits));
	CheckSameJourneyResult(result, uncached.Plan(PAX, A, destination, NOW, limits));
	auto complete = planner.Plan(PAX, A, destination, NOW);
	CHECK_FALSE(complete.diagnostic.cache_hit);
	CHECK(complete.HasJourney());
	CHECK(complete.status == PassengerPredictionStatus::Complete);
}

TEST_CASE("Journey expanded stages preserve candidates when subsequent work or labels run out", "[passenger][journey][transfer-limits]")
{
	PassengerTestStations stations;
	JourneyChain chain(5);
	JourneyVehicle branch({JourneyStop(StationID{5}, 10000, 110), JourneyStop(StationID{20}, 10)}, VehicleID{2000});
	JourneyVehicle onward({JourneyStop(StationID{20}, 10000, 150), JourneyStop(StationID{6}, 10)}, VehicleID{2001});
	const auto graph = JourneyGraph();
	PassengerJourneyPlanner planner(graph), uncached(graph, nullptr, {0, 0});
	PassengerJourneyLimits limits;
	auto full = uncached.Plan(PAX, A, StationID{6}, NOW);
	REQUIRE(full.journeys.size() == 2);
	SECTION("Work exhaustion after the first expanded-stage candidate") { limits.work = full.diagnostic.work - 1; }
	SECTION("Label exhaustion after the first expanded-stage candidate") { limits.labels = 7; }
	auto result = planner.Plan(PAX, A, StationID{6}, NOW, limits);
	REQUIRE(result.journeys.size() == 1);
	CHECK(result.journeys[0] == full.journeys[0]);
	CHECK(result.journeys[0].transfers == 5);
	CHECK(result.journeys[0].legs.back().alighting_station == StationID{6});
	CHECK(result.diagnostic.extensions == 1);
	CHECK(result.status != PassengerPredictionStatus::Complete);
	CHECK_FALSE(result.IsUnreachable());
	CheckSameJourneyResult(result, planner.Plan(PAX, A, StationID{6}, NOW, limits));
	CheckSameJourneyResult(result, uncached.Plan(PAX, A, StationID{6}, NOW, limits));
}

TEST_CASE("Journey absence distinguishes disconnection, a short horizon and invalid OD", "[passenger][journey][transfer-limits]")
{
	PassengerTestStations stations;
	JourneyVehicle train({JourneyStop(A), JourneyStop(B, 10), JourneyStop(D, 200)});
	const auto short_graph = JourneyGraph(50);
	PassengerJourneyPlanner short_planner(short_graph), uncached(short_graph, nullptr, {0, 0});
	auto short_result = short_planner.Plan(PAX, A, D, NOW);
	CHECK(short_result.journeys.empty());
	CHECK(short_result.IsInconclusive());
	CHECK_FALSE(short_result.IsUnreachable());
	CHECK(short_result.stop_reason == PassengerJourneyStopReason::PredictionHorizon);
	CHECK(short_result.prediction_end == NOW + 50);
	CheckSameJourneyResult(short_result, short_planner.Plan(PAX, A, D, NOW));
	CheckSameJourneyResult(short_result, uncached.Plan(PAX, A, D, NOW));
	const auto long_graph = JourneyGraph(500);
	PassengerJourneyPlanner long_planner(long_graph);
	CHECK(long_planner.Plan(PAX, A, D, NOW).HasJourney());
	auto isolated = long_planner.Plan(PAX, A, StationID{999}, NOW);
	CHECK(isolated.IsUnreachable());
	CHECK(isolated.status == PassengerPredictionStatus::Complete);
	CHECK(isolated.stop_reason == PassengerJourneyStopReason::Exhausted);
	CHECK(isolated.prediction_end == NOW + 500);
	auto invalid = long_planner.Plan(PAX, A, StationID::Invalid(), NOW);
	CHECK(invalid.IsInconclusive());
	CHECK(invalid.stop_reason == PassengerJourneyStopReason::InvalidQuery);
	CHECK_FALSE(invalid.IsUnreachable());
}

TEST_CASE("Journey station cycles and zero-time reversals terminate without losing real onward trips", "[passenger][journey][transfer-limits]")
{
	PassengerTestStations stations;
	TimetableTicks duration = 1;
	SECTION("Timed circular service") {}
	SECTION("Zero-time circular service") { duration = 0; }
	Order a = JourneyStop(A, duration, duration), b = JourneyStop(B, duration, duration), c = JourneyStop(C, duration, duration);
	for (Order *stop : {&a, &b, &c}) stop->SetUnloadType(OrderUnloadType::Transfer);
	Order reversal;
	reversal.MakeGoToWaypoint(StationID{20});
	reversal.SetWaypointFlags(OrderWaypointFlags{OrderWaypointFlag::Reverse});
	reversal.SetTravelTime(duration);
	reversal.SetTravelTimetabled(true);
	JourneyVehicle circle({a, b, c, reversal});
	JourneyVehicle onward({JourneyStop(C, 10000, 20), JourneyStop(D, 1)}, VehicleID{101});
	PassengerServiceGraphLimits graph_limits;
	graph_limits.horizon = 40;
	graph_limits.visits_per_trip = 32;
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, graph_limits);
	PassengerJourneyPlanner planner(graph);
	PassengerJourneyLimits limits;
	limits.initial_max_transfers = UINT32_MAX;
	limits.max_transfers = UINT32_MAX;
	auto result = planner.Plan(PAX, A, D, NOW, limits);
	REQUIRE(result.HasJourney());
	CHECK(result.journeys[0].legs.back().alighting_station == D);
	CHECK(result.diagnostic.rounds <= 4);
	CHECK(result.diagnostic.cycle_rejections > 0);
	CHECK(result.diagnostic.work <= limits.work);
	CHECK(result.diagnostic.scratch_bytes <= limits.memory_bytes);
}

TEST_CASE("Journey boarding and reverse counters exceed eight bits and clamp extreme caller limits safely", "[passenger][journey][transfer-limits]")
{
	PassengerTestStations stations;
	JourneyChain chain(300);
	const auto graph = JourneyGraph(7000);
	PassengerJourneyPlanner planner(graph);
	PassengerJourneyLimits limits;
	limits.max_transfers = UINT32_MAX;
	limits.initial_max_transfers = UINT32_MAX;
	auto result = planner.Plan(PAX, A, StationID{301}, NOW, limits);
	REQUIRE(result.journeys.size() == 1);
	CHECK(result.journeys[0].transfers == 300);
	CHECK(result.journeys[0].legs.size() == 301);
	CHECK(result.diagnostic.rounds == 301);
	CHECK(result.status == PassengerPredictionStatus::Complete);
	CHECK(result.diagnostic.extensions == 0);
}

TEST_CASE("Journey cycle suppression preserves a useful ride when waiting has a preference penalty", "[passenger][journey][transfer-limits]")
{
	PassengerTestStations stations;
	JourneyVehicle out({JourneyStop(A), JourneyStop(B, 10)});
	JourneyVehicle back({JourneyStop(B, 10000, 30), JourneyStop(A, 10)}, VehicleID{101});
	JourneyVehicle onward({JourneyStop(A, 10000, 50), JourneyStop(D, 10)}, VehicleID{102});
	const auto graph = JourneyGraph(100);
	PassengerJourneyPlanner planner(graph);
	PassengerJourneyEvaluation evaluation;
	SECTION("Waiting can replace the loop at equal elapsed cost") {
		auto result = planner.Plan(PAX, A, D, NOW);
		REQUIRE(result.journeys.size() == 1);
		CHECK(result.journeys[0].transfers == 0);
		CHECK(result.diagnostic.cycle_rejections == 1);
	}
	SECTION("Weighted waiting permits a positive-time circuit") {
		evaluation.waiting_weight = 1000;
		auto result = planner.Plan(PAX, A, D, NOW, {}, evaluation);
		REQUIRE(result.journeys.size() == 2);
		CHECK(result.journeys[0].transfers == 2);
		CHECK(result.journeys[0].arrival == NOW + 60);
		CHECK(result.journeys[0].waiting == StateTicksDelta{30});
		CHECK(result.journeys[0].generalized_time == StateTicksDelta{90});
		CHECK(result.journeys[1].generalized_time == StateTicksDelta{110});
	}
}

TEST_CASE("Journey resource exhaustion preserves the candidate before a better bucket replacement", "[passenger][journey][transfer-limits]")
{
	PassengerTestStations stations;
	JourneyVehicle slow({JourneyStop(A), JourneyStop(D, 500)});
	JourneyVehicle fast({JourneyStop(A, 10000, 30), JourneyStop(D, 50)}, VehicleID{101});
	const auto graph = JourneyGraph();
	PassengerJourneyPlanner planner(graph);
	PassengerJourneyLimits limits;
	limits.max_transfers = 0;
	limits.labels = 2;
	limits.labels_per_station = 1;
	auto result = planner.Plan(PAX, A, D, NOW, limits);
	REQUIRE(result.journeys.size() == 1);
	CHECK(result.journeys[0].legs[0].vehicle == slow.index);
	CHECK(result.stop_reason == PassengerJourneyStopReason::LabelLimit);
	CHECK(result.status == PassengerPredictionStatus::LimitReached);
	CHECK_FALSE(result.IsUnreachable());
}

TEST_CASE("Journey extension preserves capacity forecasts and an incomplete prediction suffix", "[passenger][journey][transfer-limits]")
{
	PassengerTestStations stations;
	JourneyChain chain(8);
	Order unknown = JourneyStop(StationID{11}, 10);
	unknown.SetWaitTimetabled(false);
	JourneyVehicle tail({JourneyStop(StationID{9}, 10000, 190), JourneyStop(StationID{10}, 10), unknown}, VehicleID{1009});
	JourneyCapacity capacity;
	for (const auto &train : chain.trains) {
		auto &estimate = capacity.estimates[train.index];
		estimate.future_total = 80;
		estimate.future_free = 80;
		estimate.certainty = PassengerCapacityCertainty::Confirmed;
	}
	capacity.estimates[tail.index].future_total = 80;
	capacity.estimates[tail.index].future_free = 40;
	capacity.estimates[tail.index].certainty = PassengerCapacityCertainty::Estimated;
	PassengerJourneyEvaluation evaluation;
	evaluation.crowding_penalty = 100;
	evaluation.transfer_penalty = 3;
	const auto graph = JourneyGraph();
	PassengerJourneyPlanner planner(graph, &capacity), uncached(graph, &capacity, {0, 0});
	auto result = planner.Plan(PAX, A, StationID{10}, NOW, {}, evaluation);
	REQUIRE(result.journeys.size() == 1);
	CHECK(result.diagnostic.extensions == 2);
	CHECK(result.status == PassengerPredictionStatus::Unknown);
	CHECK(result.stop_reason == PassengerJourneyStopReason::PredictionIncomplete);
	const auto &journey = result.journeys[0];
	CHECK(journey.transfers == 9);
	CHECK(journey.capacity_uncertain);
	CHECK(journey.capacity_penalty == StateTicksDelta{50});
	CHECK(journey.generalized_time == StateTicksDelta{277});
	CHECK(journey.prediction_status == PassengerPredictionStatus::Complete); // Its executed physical segments have validated times.
	CHECK(journey.legs.back().capacity.future_free == 40);
	CHECK(journey.legs.back().alighting_station == StationID{10});
	CheckSameJourneyResult(result, planner.Plan(PAX, A, StationID{10}, NOW, {}, evaluation));
	CheckSameJourneyResult(result, uncached.Plan(PAX, A, StationID{10}, NOW, {}, evaluation));
}
