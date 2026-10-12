/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file passenger_service_graph.cpp Service snapshot and shared prediction regression tests. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../passenger_service_graph.h"
#include "../vehicle_base.h"
#include "../passenger_journey_planner.h"
#include "../passenger_journey_active.h"
#include "passenger_test_stations.h"
#include "../core/random_func.hpp"
#include <deque>

static constexpr StationID A{0}, B{1}, C{2}, D{3};
static constexpr CargoType PAX{0};
static constexpr StateTicks NOW{10000};

static Order GraphStop(StationID station, TimetableTicks travel = 100, TimetableTicks wait = 10)
{
	Order order;
	order.MakeGoToStation(station);
	order.SetTravelTime(travel);
	order.SetTravelTimetabled(true);
	order.SetWaitTime(wait);
	order.SetWaitTimetabled(true);
	return order;
}

/** Real order lists/consists, without engine/GUI setup. Dispose before the temporary Vehicle destructor. */
struct GraphVehicle : Vehicle {
	explicit GraphVehicle(std::vector<Order> orders, VehicleID id = VehicleID{100}) : Vehicle(id, VehicleType::Train)
	{
		this->orders = OrderList::Create(std::move(orders), this);
		EnsurePassengerTestStations(this->orders->GetOrderVector());
		this->LoadAt(0);
		this->cargo_type = PAX;
		this->cargo_cap = 80;
		this->direction = Direction::NE;
	}

	explicit GraphVehicle(GraphVehicle &other, VehicleID id) : Vehicle(id, VehicleType::Train)
	{
		this->orders = other.orders;
		this->AddToShared(&other);
		this->LoadAt(0);
		this->cargo_type = PAX;
		this->cargo_cap = 120;
	}

	~GraphVehicle()
	{
		if (this->orders == nullptr) return;
		if (this->orders->IsShared()) {
			OrderList *list = this->orders;
			list->AddVehicle(nullptr); // Suppress the single-vehicle GUI notification, as in passenger_routing tests.
			this->RemoveFromShared();
			list->RemoveVehicle(nullptr);
		} else {
			this->orders->FreeChain();
		}
		this->orders = nullptr;
	}

	bool IsPrimaryVehicle() const override { return true; }
	void LoadAt(VehicleOrderID order)
	{
		this->cur_real_order_index = this->cur_implicit_order_index = order;
		this->cur_timetable_order_index = INVALID_VEH_ORDER_ID;
		this->last_station_visited = this->GetOrder(order)->GetDestination().ToStationID();
		this->current_order.MakeLoading(false);
	}
};

static PassengerServiceGraphLimits GraphLimits(Ticks horizon = 1000)
{
	PassengerServiceGraphLimits limits;
	limits.horizon = horizon;
	return limits;
}

struct GraphCoach : Vehicle {
	explicit GraphCoach(VehicleID id) : Vehicle(id, VehicleType::Train) {}
	bool IsPrimaryVehicle() const override { return false; }
};

static const PassengerServiceGraph::Trip &GraphTrip(const PassengerServiceGraph &graph, VehicleID id)
{
	auto trips = graph.GetTrips();
	auto it = std::find_if(trips.begin(), trips.end(), [&](const auto &trip) { return trip.vehicle == id; });
	REQUIRE(it != trips.end());
	return *it;
}

/** Compare semantic values and admission, excluding residency and elapsed observations. */
static void CheckCaptureEquivalent(const PassengerServiceGraph &a, const PassengerServiceGraph &b, StationID destination = D)
{
	CHECK(a.GetStatus() == b.GetStatus());
	CHECK(a.GetQueryStatus(A, destination) == b.GetQueryStatus(A, destination));
	CHECK(a.GetCoverage(A, destination) == b.GetCoverage(A, destination));
	CHECK(a.GetWork() == b.GetWork());
	CHECK(a.GetMemoryBound() == b.GetMemoryBound());
	const auto &x = a.GetDiagnostic(), &y = b.GetDiagnostic();
	CHECK(std::tuple(x.trains, x.cargo_capable, x.possible_refit, x.inactive, x.excluded_cargo, x.excluded_scope, x.visits, x.omitted_trains, x.truncated_predictions) ==
			std::tuple(y.trains, y.cargo_capable, y.possible_refit, y.inactive, y.excluded_cargo, y.excluded_scope, y.visits, y.omitted_trains, y.truncated_predictions));
	CHECK(std::tuple(x.first_failure.reason, x.first_failure.stage, x.first_failure.work, x.first_failure.memory, x.first_failure.trips, x.first_failure.visits) ==
			std::tuple(y.first_failure.reason, y.first_failure.stage, y.first_failure.work, y.first_failure.memory, y.first_failure.trips, y.first_failure.visits));
	const auto &xf = x.first_prediction_failure, &yf = y.first_prediction_failure;
	CHECK(std::tuple(xf.reason, xf.vehicle, xf.work, xf.memory, xf.trips, xf.visits, xf.stopped_at, xf.complete_until, xf.required_until, xf.stopped_order, xf.uncertain_order, xf.uncertain_reason) ==
			std::tuple(yf.reason, yf.vehicle, yf.work, yf.memory, yf.trips, yf.visits, yf.stopped_at, yf.complete_until, yf.required_until, yf.stopped_order, yf.uncertain_order, yf.uncertain_reason));
	CHECK(x.prediction_work == y.prediction_work);
	REQUIRE(a.GetServices().size() == b.GetServices().size());
	for (size_t i = 0; i < a.GetServices().size(); ++i) {
		const auto &p = a.GetServices()[i], &q = b.GetServices()[i];
		CHECK(std::tuple(p.id, p.pattern_status, p.cycle_known, p.coverage_status, p.uncertain_from, p.trips) ==
				std::tuple(q.id, q.pattern_status, q.cycle_known, q.coverage_status, q.uncertain_from, q.trips));
		CHECK(std::equal(p.stops.begin(), p.stops.end(), q.stops.begin(), q.stops.end(), [](const auto &s, const auto &t) {
			return std::tuple(s.station, s.order, s.can_board, s.can_alight, s.forced_transfer, s.dwell, s.dispatch_schedule) ==
					std::tuple(t.station, t.order, t.can_board, t.can_alight, t.forced_transfer, t.dwell, t.dispatch_schedule);
		}));
		CHECK(std::equal(p.segments.begin(), p.segments.end(), q.segments.begin(), q.segments.end(), [](const auto &s, const auto &t) {
			return std::tuple(s.from, s.to, s.travel) == std::tuple(t.from, t.to, t.travel);
		}));
	}
	REQUIRE(a.GetTrips().size() == b.GetTrips().size());
	for (size_t i = 0; i < a.GetTrips().size(); ++i) {
		const auto &p = a.GetTrips()[i], &q = b.GetTrips()[i];
		CHECK(std::tuple(p.vehicle, p.service, p.tile, p.direction, p.current_order, p.elapsed, p.lateness, p.capacity, p.occupants, p.current_free, p.operation, p.prediction_reason) ==
				std::tuple(q.vehicle, q.service, q.tile, q.direction, q.current_order, q.elapsed, q.lateness, q.capacity, q.occupants, q.current_free, q.operation, q.prediction_reason));
		CHECK(std::tuple(p.prediction.status, p.prediction.start, p.prediction.complete_until, p.prediction.stop, p.prediction.stopped_at, p.prediction.stopped_order, p.prediction.uncertain_order, p.prediction.uncertain_reason) ==
				std::tuple(q.prediction.status, q.prediction.start, q.prediction.complete_until, q.prediction.stop, q.prediction.stopped_at, q.prediction.stopped_order, q.prediction.uncertain_order, q.prediction.uncertain_reason));
		CHECK(std::equal(p.prediction.visits.begin(), p.prediction.visits.end(), q.prediction.visits.begin(), q.prediction.visits.end(), [](const auto &s, const auto &t) {
			return std::tuple(s.order, s.arrival, s.departure, s.loading_now, s.travel_from_previous) == std::tuple(t.order, t.arrival, t.departure, t.loading_now, t.travel_from_previous);
		}));
	}
	CHECK(a.GetVisits(A, a.GetStart(), a.GetEnd()).items == b.GetVisits(A, b.GetStart(), b.GetEnd()).items);
}

TEST_CASE("Event capture shares services across 100 distinct OD searches", "[passenger][service_graph][capture]")
{
	PassengerTestStations stations;
	std::vector<Order> stops{GraphStop(A, 100000, 0), GraphStop(B, 5, 0), GraphStop(D, 5, 0)};
	for (uint16_t i = 4; i < 104; ++i) stops.push_back(GraphStop(StationID{i}, 5, 0));
	GraphVehicle local(stops), shared(local, VehicleID{101});
	GraphVehicle express({GraphStop(A, 100000, 0), GraphStop(D, 2, 0), GraphStop(StationID{103}, 2, 0)}, VehicleID{102});
	GraphVehicle shortcut({GraphStop(B, 100000, 0), GraphStop(StationID{52}, 1, 0)}, VehicleID{103});
	GraphVehicle trunk({GraphStop(A, 100000, 0), GraphStop(StationID{40}, 50, 0), GraphStop(StationID{80}, 20, 0)}, VehicleID{104});
	GraphVehicle unrelated({GraphStop(StationID{200}), GraphStop(StationID{201})}, VehicleID{105});
	PassengerServiceCapture capture;
	std::shared_ptr<const PassengerServiceGraph::ServicePattern> first;
	GameRandomSeedChecker seeds;
	size_t local_executions = 0, local_hits = 0;
	for (uint16_t i = 4; i < 104; ++i) {
		const StationID destination{i};
		const PassengerServiceScope scope{A, destination};
		const auto baseline = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(1000), scope);
		const auto cached = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(1000), scope, &capture);
		CheckCaptureEquivalent(baseline, cached, destination);
		local_executions += GraphTrip(cached, local.index).prediction_executed;
		local_hits += GraphTrip(cached, local.index).prediction_cache_hit;
		CHECK(cached.GetService(unrelated.orders->index) == nullptr);
		const auto *service = cached.GetService(local.orders->index);
		REQUIRE(service != nullptr);
		if (!first) first = service->pattern;
		else { CHECK(first == service->pattern); CHECK(cached.GetDiagnostic().capture_discovery_hit); CHECK(cached.GetDiagnostic().capture_service_hits > 0); }
		PassengerJourneyPlanner p(baseline), q(cached);
		const auto expected = p.Plan(PAX, A, destination, NOW), actual = q.Plan(PAX, A, destination, NOW);
		CHECK(expected.status == actual.status);
		CHECK(expected.journeys == actual.journeys);
		CHECK(expected.stop_reason == actual.stop_reason);
		CHECK(std::tie(expected.diagnostic.work, expected.diagnostic.reverse_work, expected.diagnostic.rounds, expected.diagnostic.labels, expected.diagnostic.scratch_bytes) ==
				std::tie(actual.diagnostic.work, actual.diagnostic.reverse_work, actual.diagnostic.rounds, actual.diagnostic.labels, actual.diagnostic.scratch_bytes));
	}
	CHECK(local_executions == 1); CHECK(local_hits == 99);
	CHECK(capture.GetAllocationBytes() < PassengerServiceCapture::MAXIMUM_BYTES);
	CHECK(seeds.Check());
}

TEST_CASE("Trip cache rejects live input changes within the same tick", "[passenger][service_graph][capture][trip_cache]")
{
	PassengerTestStations stations;
	GraphVehicle train({GraphStop(A), GraphStop(B), GraphStop(D)});
	PassengerServiceCapture capture;
	const PassengerServiceScope scope{A, D};
	auto limits = GraphLimits();
	const auto original = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture);
	REQUIRE(original.GetDiagnostic().prediction_executions == 1);
	REQUIRE(PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture).GetDiagnostic().prediction_cache_hits == 1);
	SECTION("Tile") { train.tile = TileIndex{100}; }
	SECTION("Coordinates within tile") { ++train.x_pos; ++train.y_pos; ++train.z_pos; }
	SECTION("Direction") { train.direction = Direction::SE; }
	SECTION("Reversal") { train.vehicle_flags.Set(VehicleFlag::DrivingBackwards); }
	SECTION("Lateness") { train.lateness_counter = 5; }
	SECTION("Elapsed progress") { train.current_order_time = 7; }
	SECTION("Current instruction flags") { train.current_order.SetLoadType(OrderLoadType::NoLoad); }
	SECTION("Travel timetable") { train.GetOrder(1)->SetTravelTime(250); }
	SECTION("Wait timetable") { train.GetOrder(0)->SetWaitTime(70); }
	SECTION("Unknown timetable") { train.GetOrder(1)->SetTravelTimetabled(false); train.GetOrder(1)->SetTravelTime(0); }
	SECTION("Load another cargo") { train.GetOrder(1)->SetLoadType(OrderLoadType::CargoTypeLoad); train.GetOrder(1)->SetLoadType(OrderLoadType::FullLoad, CargoType{1}); }
	SECTION("Unload permission") { train.GetOrder(1)->SetUnloadType(OrderUnloadType::NoUnload); }
	SECTION("Refit instruction") { train.GetOrder(1)->SetRefit(CargoType{1}); }
	SECTION("Facility result") { Station::Get(B)->facilities.Reset(StationFacility::Train); }
	SECTION("Dispatch enablement") { train.vehicle_flags.Set(VehicleFlag::ScheduledDispatch); }
	SECTION("Dispatch schedule") {
		train.GetOrder(1)->SetDispatchScheduleIndex(0); train.vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
		train.orders->GetScheduledDispatchScheduleSet().resize(1);
		auto &s = train.orders->GetDispatchScheduleByIndex(0);
		s.SetScheduledDispatchDuration(300); s.SetScheduledDispatchStartTick(NOW); s.GetScheduledDispatchMutable() = {{40, 0, {}}};
	}
	SECTION("Same-tick boarding") { train.cargo.Append(CargoPacket::Create(12, 0, A, TileIndex{0}, Money{0})); }
	const auto changed = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture);
	CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, limits, scope), changed);
	CHECK(changed.GetDiagnostic().prediction_cache_hits == 0);
	CHECK(changed.GetDiagnostic().prediction_executions == 1);
	CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, limits, scope), PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture));
	train.cargo.Truncate();
}

TEST_CASE("Trip cache separates events periods limits and resource stops", "[passenger][service_graph][capture][trip_cache][budget]")
{
	PassengerTestStations stations;
	GraphVehicle train({GraphStop(A, 4096, 0), GraphStop(B, 4096, 0), GraphStop(D, 4096, 0)});
	PassengerServiceCapture capture;
	const PassengerServiceScope scope{A, D};
	auto limits = GraphLimits(4096); limits.visits_per_trip = 512;
	SECTION("Periods in both directions and half-open boundary") {
		for (Ticks horizon : {4096, 16384, 65536, 16384, 4096}) {
			limits.horizon = horizon;
			const auto fresh = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture);
			CHECK(fresh.GetDiagnostic().prediction_cache_hits == 0);
			CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, limits, scope), fresh);
			const auto hit = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture);
			CHECK(hit.GetDiagnostic().prediction_cache_hits == 1);
			CheckCaptureEquivalent(fresh, hit);
		}
	}
	SECTION("Timing policies never share physical predictions") {
		for (PassengerTimingPolicy timing : {PassengerTimingPolicy::Conservative, PassengerTimingPolicy::AssumeTimetable,
				PassengerTimingPolicy::Conservative, PassengerTimingPolicy::AssumeTimetable}) {
			const auto fresh = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture, timing);
			CHECK(fresh.GetDiagnostic().prediction_cache_hits == 0);
			CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, limits, scope, nullptr, timing), fresh);
			const auto hit = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture, timing);
			CHECK(hit.GetDiagnostic().prediction_cache_hits == 1);
			CheckCaptureEquivalent(fresh, hit);
		}
	}
	SECTION("New event at same tick and next tick") {
		PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture); capture.Clear();
		CHECK(capture.GetPredictionBytes() == 0);
		CHECK(PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture).GetDiagnostic().prediction_cache_hits == 0);
		const auto next = PassengerServiceGraph::Build(PAX, NOW + 1, limits, scope, &capture);
		CHECK(next.GetDiagnostic().prediction_cache_hits == 0);
		CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW + 1, limits, scope), next);
	}
	SECTION("Partial resource results are never resident") {
		limits.horizon = 65536;
		for (size_t steps : {size_t{0}, size_t{1}, size_t{2}, size_t{3}}) {
			limits.steps_per_trip = steps;
			for (int repeat = 0; repeat < 2; ++repeat) {
				const auto partial = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture);
				CHECK(partial.GetDiagnostic().prediction_cache_hits == 0);
				CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, limits, scope), partial);
			}
		}
		CHECK(capture.GetPredictionBytes() == 0);
	}
	SECTION("Scoped prefix and unscoped traversal") {
		PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture);
		const auto full = PassengerServiceGraph::Build(PAX, NOW, limits, {}, &capture);
		CHECK(full.GetDiagnostic().prediction_cache_hits == 0);
		CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, limits), full);
	}
}

TEST_CASE("Trip cache validates changed dispatch history and every timing input", "[passenger][service_graph][trip_cache]")
{
	PassengerTestStations stations;
	GraphVehicle train({GraphStop(A), GraphStop(B), GraphStop(D)});
	train.GetOrder(1)->SetDispatchScheduleIndex(0); train.vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
	train.orders->GetScheduledDispatchScheduleSet().resize(1);
	auto &schedule = train.orders->GetDispatchScheduleByIndex(0);
	schedule.SetScheduledDispatchDuration(300); schedule.SetScheduledDispatchStartTick(NOW); schedule.SetScheduledDispatchReuseSlots(true);
	schedule.GetScheduledDispatchMutable() = {{80, 0, {}}, {180, 0, {}}};
	PassengerServiceCapture capture;
	const PassengerServiceScope scope{A, D};
	PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &capture);
	REQUIRE(PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &capture).GetDiagnostic().prediction_cache_hits == 1);
	SECTION("History") { schedule.SetScheduledDispatchLastDispatch(500); }
	SECTION("Start") { schedule.SetScheduledDispatchStartTick(NOW - 30); }
	SECTION("Duration") { schedule.SetScheduledDispatchDuration(500); }
	SECTION("Delay") { schedule.SetScheduledDispatchDelay(20); }
	SECTION("Reuse") { schedule.SetScheduledDispatchReuseSlots(false); }
	SECTION("Slot time") { schedule.GetScheduledDispatchMutable()[0].offset = 50; }
	SECTION("Slot flags") { schedule.GetScheduledDispatchMutable()[0].flags = 1; }
	SECTION("Slot route") { schedule.GetScheduledDispatchMutable()[0].route_id = 1; }
	const auto changed = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &capture);
	CHECK(changed.GetDiagnostic().prediction_cache_hits == 0);
	CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope), changed);
}

TEST_CASE("Trip cache is bounded and never shares predictions between shared vehicles", "[passenger][service_graph][capture][trip_cache][determinism]")
{
	PassengerTestStations stations;
	GraphVehicle first({GraphStop(A), GraphStop(B), GraphStop(D)});
	std::deque<GraphVehicle> fleet;
	for (uint32_t i = 101; i < 229; ++i) fleet.emplace_back(first, VehicleID{i});
	PassengerServiceCapture warm, disabled(16 * 1024 * 1024, 0), tiny(16 * 1024 * 1024, 2000);
	const PassengerServiceScope scope{A, D};
	const auto base = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &warm);
	REQUIRE(base.GetDiagnostic().prediction_executions == 129);
	const auto hit = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &warm);
	CHECK(hit.GetDiagnostic().prediction_cache_hits == 129);
	GameRandomSeedChecker random;
	for (PassengerServiceCapture *peer : {&disabled, &tiny}) {
		CheckCaptureEquivalent(base, PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, peer));
		CheckCaptureEquivalent(hit, PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, peer));
	}
	CHECK(disabled.GetPredictionBytes() == 0); CHECK(tiny.GetPredictionBytes() <= 2000); CHECK(warm.GetPredictionBytes() <= PassengerServiceCapture::MAXIMUM_PREDICTION_BYTES);
	CHECK(random.Check());
}

TEST_CASE("Trip capture notices newly eligible refits and shared fleet changes", "[passenger][service_graph][capture][trip_cache]")
{
	PassengerTestStations stations;
	GraphVehicle passenger({GraphStop(A), GraphStop(D)}), freight({GraphStop(A), GraphStop(D)}, VehicleID{101});
	freight.cargo_type = CargoType{1};
	PassengerServiceCapture capture;
	const PassengerServiceScope scope{A, D};
	PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &capture);
	SECTION("Previously excluded freight can refit") { freight.GetOrder(1)->SetRefit(PAX); }
	SECTION("Previously excluded freight refitted") { freight.cargo_type = PAX; }
	SECTION("New vehicle on shared orders") {
		GraphVehicle added(passenger, VehicleID{102});
		CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope), PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &capture));
	}
	SECTION("Consist length changes without changing passenger capacity") {
		std::unique_ptr<GraphCoach> coach(Vehicle::Create<GraphCoach>());
		coach->cargo_type = CargoType{1}; coach->cargo_cap = 50; passenger.SetNext(coach.get());
		CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope), PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &capture));
		passenger.SetNext(nullptr);
	}
	CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope), PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &capture));
}

TEST_CASE("Expanded Trip cache retains visits beyond the former eight MiB ceiling", "[passenger][service_graph][trip_cache][budget]")
{
	PassengerTestStations stations;
	GraphVehicle first({GraphStop(A, 10, 0), GraphStop(B, 10, 0), GraphStop(D, 10, 0)}, VehicleID{1000});
	std::deque<GraphVehicle> fleet;
	for (uint32_t i = 1001; i < 1256; ++i) fleet.emplace_back(first, VehicleID{i});
	auto limits = GraphLimits(2560); limits.visits_per_trip = 512;
	const PassengerServiceScope scope{A, D};
	PassengerServiceCapture old_limit(16 * 1024 * 1024, 8 * 1024 * 1024), expanded;
	const auto prime = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &expanded);
	REQUIRE(prime.GetDiagnostic().prediction_executions == 256);
	REQUIRE(expanded.GetPredictionBytes() > 8 * 1024 * 1024);
	CHECK(expanded.GetPredictionBytes() <= PassengerServiceCapture::MAXIMUM_PREDICTION_BYTES);
	CheckCaptureEquivalent(prime, PassengerServiceGraph::Build(PAX, NOW, limits, scope, &old_limit));
	const auto old = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &old_limit);
	const auto hit = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &expanded);
	CHECK(old.GetDiagnostic().prediction_executions > 0);
	CHECK(old_limit.GetPredictionBytes() <= 8 * 1024 * 1024);
	CHECK(hit.GetDiagnostic().prediction_executions == 0);
	CHECK(hit.GetDiagnostic().prediction_cache_hits == 256);
	CheckCaptureEquivalent(old, hit);
}

TEST_CASE("Event capture preserves every small work boundary and memory admission", "[passenger][service_graph][capture][budget]")
{
	PassengerTestStations stations;
	GraphVehicle train({GraphStop(A), GraphStop(B), GraphStop(D)}), shared(train, VehicleID{101});
	SECTION("Incomplete timetable") { train.GetOrder(1)->SetTravelTimetabled(false); }
	SECTION("Refit") { train.GetOrder(1)->SetRefit(CargoType{1}); }
	SECTION("Conditional order") { train.GetOrder(1)->MakeConditional(2); train.GetOrder(1)->SetConditionVariable(OrderConditionVariable::LoadPercentage); }
	SECTION("Dispatch") {
		train.GetOrder(0)->SetDispatchScheduleIndex(0); train.vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
		train.orders->GetScheduledDispatchScheduleSet().resize(1);
		auto &s = train.orders->GetDispatchScheduleByIndex(0);
		s.SetScheduledDispatchStartTick(NOW); s.SetScheduledDispatchDuration(300); s.SetScheduledDispatchReuseSlots(true);
		s.GetScheduledDispatchMutable() = {{40, 0, {}}, {100, 0, {}}};
	}
	PassengerServiceCapture capture;
	const PassengerServiceScope scope{A, D};
	const auto prime = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &capture);
	for (size_t work = 0; work < 400; ++work) {
		auto limits = GraphLimits(); limits.work = work;
		INFO("work=" << work);
		CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, limits, scope), PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture));
	}
	for (size_t bytes = 0; bytes <= prime.GetMemoryBound() + 512; bytes += 64) {
		auto limits = GraphLimits(); limits.memory_bytes = bytes;
		INFO("memory=" << bytes);
		CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, limits, scope), PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture));
	}
	for (size_t steps : {size_t{0}, size_t{1}, size_t{2}, size_t{4}, size_t{4096}}) {
		auto limits = GraphLimits(); limits.steps_per_trip = steps; limits.total_visits = 3;
		CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, limits, scope), PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture));
	}
	PassengerServiceCapture evicted(0), tiny(1024);
	CheckCaptureEquivalent(prime, PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &evicted));
	CheckCaptureEquivalent(prime, PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &tiny));
}

TEST_CASE("Capture separates cargos and rereads progress and new events within one tick", "[passenger][service_graph][capture]")
{
	PassengerTestStations stations;
	AutoRestoreBackup<CargoClasses> other{CargoSpec::Get(CargoType{1})->classes, CargoClasses{CargoClass::Passengers}};
	GraphVehicle train({GraphStop(A), GraphStop(B), GraphStop(D)});
	std::unique_ptr<GraphCoach> coach(Vehicle::Create<GraphCoach>());
	coach->cargo_type = CargoType{1}; coach->cargo_cap = 50; train.SetNext(coach.get());
	PassengerServiceCapture capture;
	const PassengerServiceScope scope{A, D};
	const auto first = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &capture);
	for (CargoType cargo : {CargoType{1}, PAX, CargoType{1}}) {
		CheckCaptureEquivalent(PassengerServiceGraph::Build(cargo, NOW, GraphLimits(), scope), PassengerServiceGraph::Build(cargo, NOW, GraphLimits(), scope, &capture));
	}
	train.LoadAt(1); train.current_order_time = 7; train.lateness_counter = 15;
	CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope), PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &capture));
	train.cargo.Append(CargoPacket::Create(12, 0, A, TileIndex{0}, Money{0}));
	const auto occupied = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &capture);
	CHECK(GraphTrip(occupied, train.index).occupants == 12);
	CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope), occupied);
	train.cargo.Truncate();
	Station::Get(B)->facilities.Reset(StationFacility::Train);
	CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope), PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &capture));
	Station::Get(B)->facilities.Set(StationFacility::Train);
	/* A game-state mutation is a new event, even at the same state tick. */
	capture.Clear(); coach->cargo_type = PAX; train.GetOrder(1)->SetTravelTime(200);
	train.orders->GetScheduledDispatchScheduleSet().resize(1);
	CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope), PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &capture));
	CHECK(GraphTrip(first, train.index).capacity == 80);
	CHECK(GraphTrip(PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &capture), train.index).capacity == 130);
	train.GetOrder(1)->SetDestination(D); InvalidatePassengerServiceIndex();
	const auto changed = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope, &capture);
	CHECK_FALSE(changed.GetDiagnostic().capture_discovery_hit);
	CheckCaptureEquivalent(PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope), changed);
	const auto next_tick = PassengerServiceGraph::Build(PAX, NOW + 1, GraphLimits(), scope, &capture);
	CHECK_FALSE(next_tick.GetDiagnostic().capture_discovery_hit);
	train.SetNext(nullptr);
}

TEST_CASE("Event capture benchmark has thousands of services and one hundred thousand visits", "[passenger][service_graph][capture][.performance]")
{
	PassengerTestStations stations;
	std::deque<GraphVehicle> fleet;
	size_t orders = 3;
	SECTION("Three orders") {}
	SECTION("Twenty orders") { orders = 20; }
	for (uint32_t i = 0; i < 2500; ++i) {
		std::vector<Order> stops{GraphStop(A, 10, 0), GraphStop(D, 10, 0)};
		for (size_t j = 2; j < orders; ++j) stops.push_back(GraphStop(StationID{static_cast<uint16_t>(4 + (i + j) % 100)}, 10, 0));
		fleet.emplace_back(std::move(stops), VehicleID{1000 + i});
	}
	auto limits = ActivePassengerServiceGraphLimits(); limits.work = 16 * 1024 * 1024; limits.horizon = 420;
	PassengerServiceCapture capture;
	uint64_t uncached_us = 0, cached_us = 0;
	PassengerGraphPhaseTimes cold_phases{}, warm_phases{};
	size_t hits = 0, reused = 0, visits = 0, bound = 0, allocation = 0;
	for (uint16_t od = 4; od < 104; ++od) {
		const PassengerServiceScope scope{A, StationID{od}};
		PassengerServiceGraph a, b;
		/* Alternate pair order to reduce CPU-cache/thermal bias in elapsed diagnostics. */
		if (od % 2 == 0) {
			a = PassengerServiceGraph::Build(PAX, NOW, limits, scope);
			b = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture);
		} else {
			b = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &capture);
			a = PassengerServiceGraph::Build(PAX, NOW, limits, scope);
		}
		REQUIRE(a.GetQueryStatus(A, StationID{od}) == PassengerPredictionStatus::Complete);
		CHECK(a.GetWork() == b.GetWork()); CHECK(a.GetMemoryBound() == b.GetMemoryBound());
		CHECK(a.GetTrips().size() == b.GetTrips().size()); CHECK(a.GetDiagnostic().visits == b.GetDiagnostic().visits);
		REQUIRE(b.GetDiagnostic().visits >= 100000);
		uncached_us += a.GetDiagnostic().build_microseconds; cached_us += b.GetDiagnostic().build_microseconds;
		hits += b.GetDiagnostic().capture_service_hits; reused += b.GetDiagnostic().capture_reused_work;
		visits = b.GetDiagnostic().visits; bound = b.GetMemoryBound(); allocation = b.GetDiagnostic().allocation_bytes;
		for (size_t i = 0; i < cold_phases.size(); ++i) { cold_phases[i] += a.GetDiagnostic().phase_microseconds[i]; warm_phases[i] += b.GetDiagnostic().phase_microseconds[i]; }
	}
	WARN("Capture 100 OD: services=2500; orders=" << orders << "; visits=" << visits << "; uncached_us=" << uncached_us << "; cached_us=" << cached_us << "; service_hits=" << hits
			<< "; replayed_work=" << reused << "; graph_bound=" << bound << "; graph_estimate=" << allocation << "; capture_estimate=" << capture.GetAllocationBytes());
	for (size_t i = 0; i < cold_phases.size(); ++i) WARN("Phase " << i << ": uncached_us=" << cold_phases[i] << "; cached_us=" << warm_phases[i]);
}

TEST_CASE("Trip prediction benchmark over 4800 stations and 100 ODs", "[passenger][service_graph][trip_cache][.performance]")
{
	PassengerTestStations stations;
	std::deque<GraphVehicle> fleet;
	for (uint32_t i = 0; i < 2399; ++i) {
		fleet.emplace_back(std::vector<Order>{GraphStop(A, 100000, 0), GraphStop(B, 100, 0),
				GraphStop(StationID{static_cast<uint16_t>(2 + 2 * i)}, 100, 0), GraphStop(StationID{static_cast<uint16_t>(3 + 2 * i)}, 100, 0)}, VehicleID{1000 + i});
	}
	REQUIRE(Station::GetNumItems() == 4800);
	size_t ceiling = 8 * 1024 * 1024;
	SECTION("Resident") {}
	SECTION("Saturated") { ceiling = 128 * 1024; }
	for (size_t run = 0; run < 3; ++run) {
		PassengerServiceCapture disabled(16 * 1024 * 1024, 0), enabled(16 * 1024 * 1024, ceiling);
		uint64_t cold_us = 0, warm_us = 0, cold_prediction_ns = 0, warm_prediction_ns = 0, cold_query_us = 0, warm_query_us = 0;
		size_t cold_calls = 0, warm_calls = 0, hits = 0, work = 0, visits = 0, routes = 0;
		auto limits = ActivePassengerServiceGraphLimits(); limits.horizon = 4096;
		GameRandomSeedChecker random;
		for (uint16_t od = 2; od < 102; ++od) {
			const PassengerServiceScope scope{A, StationID{od}};
			PassengerServiceGraph a, b;
			if ((od + run) % 2 == 0) {
				a = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &disabled);
				b = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &enabled);
			} else {
				b = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &enabled);
				a = PassengerServiceGraph::Build(PAX, NOW, limits, scope, &disabled);
			}
			REQUIRE(a.GetTrips().size() == 2399); REQUIRE(a.GetStatus() == PassengerPredictionStatus::Complete);
			CHECK(a.GetWork() == b.GetWork()); CHECK(a.GetMemoryBound() == b.GetMemoryBound());
			CHECK(a.GetDiagnostic().visits == b.GetDiagnostic().visits);
			cold_us += a.GetDiagnostic().build_microseconds; warm_us += b.GetDiagnostic().build_microseconds;
			cold_prediction_ns += a.GetDiagnostic().prediction_nanoseconds; warm_prediction_ns += b.GetDiagnostic().prediction_nanoseconds;
			cold_calls += a.GetDiagnostic().prediction_executions; warm_calls += b.GetDiagnostic().prediction_executions; hits += b.GetDiagnostic().prediction_cache_hits;
			work += b.GetWork(); visits += b.GetDiagnostic().visits;
			const auto start = std::chrono::steady_clock::now();
			PassengerJourneyPlanner p(a); const auto expected = p.Plan(PAX, A, scope.destination, NOW);
			const auto middle = std::chrono::steady_clock::now();
			PassengerJourneyPlanner q(b); const auto actual = q.Plan(PAX, A, scope.destination, NOW);
			const auto end = std::chrono::steady_clock::now();
			cold_query_us += std::chrono::duration_cast<std::chrono::microseconds>(middle - start).count();
			warm_query_us += std::chrono::duration_cast<std::chrono::microseconds>(end - middle).count();
			CHECK(expected.journeys == actual.journeys); CHECK(expected.status == actual.status); CHECK(expected.diagnostic.work == actual.diagnostic.work);
			routes += actual.HasJourney();
		}
		CHECK(random.Check()); CHECK(enabled.GetPredictionBytes() <= ceiling); CHECK(cold_calls == 239900); CHECK(warm_calls < cold_calls);
		if (ceiling == 8 * 1024 * 1024) { CHECK(warm_calls == 2399); CHECK(hits == 237501); }
		WARN("Trip benchmark: run=" << run << "; stations=4800; vehicles=2399; builds=100; cache_ceiling=" << ceiling << "; disabled_calls=" << cold_calls << "; enabled_calls=" << warm_calls
				<< "; hits=" << hits << "; requests=" << cold_calls << "; disabled_build_us=" << cold_us << "; enabled_build_us=" << warm_us << "; disabled_predictor_ns=" << cold_prediction_ns
				<< "; enabled_predictor_ns=" << warm_prediction_ns << "; logical_work=" << work << "; logical_visits=" << visits << "; searches=100; timed_routes=" << routes
				<< "; disabled_query_us=" << cold_query_us << "; enabled_query_us=" << warm_query_us << "; prediction_bytes=" << enabled.GetPredictionBytes() << "; capture_bytes=" << enabled.GetAllocationBytes());
	}
}

TEST_CASE("Historic implicit orders do not hide the fastest explicit train or invent transfers", "[passenger][journey][uncertainty][implicit]")
{
	PassengerTestStations stations;
	GraphVehicle slow({GraphStop(A, 100000, 0), GraphStop(D, 200, 0)}, VehicleID{200});
	Order historic; historic.MakeImplicit(B);
	GraphVehicle fast({GraphStop(A, 100000, 0), historic, GraphStop(D, 50, 0)}, VehicleID{100});
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
	CHECK(graph.GetCoverage(A, D) == PassengerServiceGraph::Coverage::GlobalComplete);
	CHECK(graph.GetServicesAtStation(B).empty());
	CHECK(graph.GetService(fast.orders->index)->stops.size() == 2);
	CHECK(graph.GetService(fast.orders->index)->pattern_status == PassengerPredictionStatus::Complete);
	CHECK(graph.GetDiagnostic().prediction_stops[static_cast<size_t>(PassengerPredictionStop::Implicit)] == 0);
	PassengerJourneyPlanner planner(graph);
	PassengerJourneyLimits query; query.candidates = 1;
	const auto result = planner.Plan(PAX, A, D, NOW, query);
	REQUIRE(result.HasJourney());
	CHECK(result.status == PassengerPredictionStatus::Complete);
	CHECK(result.journeys.front().arrival == NOW + 50);
	const auto &leg = result.journeys.front().legs.front();
	CHECK(leg.vehicle == fast.index);
	CHECK(graph.GetTrips()[leg.alighting.trip].prediction.visits[leg.alighting.visit].order == 2);
}

TEST_CASE("Timed journeys ignore unknown alternatives and retain physical prefixes", "[passenger][journey][uncertainty]")
{
	PassengerTestStations stations;
	GraphVehicle direct({GraphStop(A, 100000, 0), GraphStop(D, 100, 0)}, VehicleID{200});
	Order unknown = GraphStop(B, 0, 0); unknown.SetWaitTimetabled(false);
	GraphVehicle alternative({GraphStop(A, 100000, 200), unknown, GraphStop(D, 0, 0)}, VehicleID{100});
	PassengerJourneyLimits query; query.candidates = 1;
	auto limits = ActivePassengerServiceGraphLimits(); limits.horizon = 1000;
	SECTION("An untimed corridor starting after the winning score is harmless") {
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
		CHECK(graph.GetCoverage(A, D) == PassengerServiceGraph::Coverage::Incomplete);
		CHECK(graph.GetQueryStatus(A, D) == PassengerPredictionStatus::Complete);
		CHECK(GraphTrip(graph, alternative.index).prediction.complete_until == NOW + 200);
		PassengerJourneyPlanner planner(graph);
		const auto actual = planner.Plan(PAX, A, D, NOW, query);
		REQUIRE(actual.HasJourney());
		CHECK(actual.status == PassengerPredictionStatus::Unknown);
		CHECK(actual.GetOutcome() == PassengerJourneyOutcome::TimedRouteFound);
		CHECK(actual.diagnostic.unknown_alternatives_ignored);
		CHECK_FALSE(actual.diagnostic.uncertainty_resolved);
		CHECK_FALSE(actual.diagnostic.unknown_arrival_bound);
		CHECK(actual.journeys.front().legs.front().vehicle == direct.index);
		*alternative.GetOrder(1) = GraphStop(B, 0, 0);
		EnsurePassengerTestStations(alternative.orders->GetOrderVector()); InvalidatePassengerServiceIndex();
		const auto full = PassengerServiceGraph::Build(PAX, NOW, limits);
		PassengerJourneyPlanner reference(full);
		CHECK(actual.journeys.front() == reference.Plan(PAX, A, D, NOW, query).journeys.front());
	}
	SECTION("An unknown train could arrive faster") {
		alternative.GetOrder(0)->SetWaitTime(50);
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
		PassengerJourneyPlanner planner(graph);
		const auto result = planner.Plan(PAX, A, D, NOW, query);
		REQUIRE(result.HasJourney());
		CHECK(result.status == PassengerPredictionStatus::Unknown);
		CHECK_FALSE(result.diagnostic.unknown_arrival_bound);
		CHECK(result.GetOutcome() == PassengerJourneyOutcome::TimedRouteFound);
		CHECK_FALSE(result.diagnostic.uncertainty_resolved);
	}
	SECTION("Equal scores cannot ignore vehicle and capacity tie breaks") {
		alternative.GetOrder(0)->SetWaitTime(100);
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
		PassengerJourneyPlanner planner(graph);
		const auto result = planner.Plan(PAX, A, D, NOW, query);
		REQUIRE(result.HasJourney());
		CHECK(result.status == PassengerPredictionStatus::Unknown);
		CHECK_FALSE(result.diagnostic.unknown_arrival_bound);
		CHECK(result.GetOutcome() == PassengerJourneyOutcome::TimedRouteFound);
	}
	SECTION("Certification of K alternatives requires every requested slot") {
		query.candidates = 2;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
		PassengerJourneyPlanner planner(graph);
		CHECK(planner.Plan(PAX, A, D, NOW, query).status == PassengerPredictionStatus::Unknown);
	}
	SECTION("Timing becomes unknown before the implicit order is reached") {
		alternative.GetOrder(0)->SetWaitTimetabled(false);
		Order implicit; implicit.MakeImplicit(C); alternative.orders->InsertOrderAt(std::move(implicit), 1);
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
		const auto &prediction = GraphTrip(graph, alternative.index).prediction;
		CHECK(prediction.stop == PassengerPredictionStop::Timing);
		CHECK(prediction.stopped_order == 0);
		CHECK(prediction.uncertain_order == 0);
		CHECK(prediction.uncertain_reason == PassengerPredictionStop::Timing);
		CHECK(prediction.complete_until == NOW);
		CHECK(prediction.visits.size() == 1); // No nominal untimed suffix was allocated.
		CHECK(graph.GetDiagnostic().timing_uncertainty == 1);
		PassengerJourneyPlanner planner(graph);
		CHECK(planner.Plan(PAX, A, D, NOW, query).status == PassengerPredictionStatus::Unknown);
	}
	SECTION("A certified physical ride survives an unknown later dwell") {
		alternative.GetOrder(0)->SetWaitTime(0);
		*alternative.GetOrder(1) = GraphStop(D, 100, 0);
		*alternative.GetOrder(2) = GraphStop(B, 200, 0);
		alternative.GetOrder(2)->SetWaitTimetabled(false);
		EnsurePassengerTestStations(alternative.orders->GetOrderVector()); InvalidatePassengerServiceIndex();
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
		PassengerJourneyPlanner planner(graph);
		const auto result = planner.Plan(PAX, A, D, NOW, query);
		REQUIRE(result.HasJourney());
		CHECK(GraphTrip(graph, alternative.index).prediction.status == PassengerPredictionStatus::Unknown);
		CHECK(result.status == PassengerPredictionStatus::Unknown);
		CHECK(result.GetOutcome() == PassengerJourneyOutcome::TimedRouteFound);
		CHECK(result.journeys.front().prediction_status == PassengerPredictionStatus::Complete);
		CHECK(result.journeys.front().legs.front().prediction_status == PassengerPredictionStatus::Complete);
		CHECK(result.journeys.front().legs.front().vehicle == alternative.index);
	}
}

TEST_CASE("Bounded route controls remain unknown until a fresh live state can certify them", "[passenger][journey][uncertainty][cache]")
{
	PassengerTestStations stations;
	GraphVehicle direct({GraphStop(A, 100000, 0), GraphStop(D, 100, 0)}, VehicleID{200});
	Order control;
	SECTION("Conditional") { control.MakeConditional(2); control.SetConditionVariable(OrderConditionVariable::LoadPercentage); }
	SECTION("Depot") { control.MakeGoToDepot(DestinationID{0}, {}); }
	SECTION("Slot") { control.MakeTryAcquireSlot(); }
	SECTION("Counter") { control.MakeChangeCounter(); }
	SECTION("Refit") { control = GraphStop(B, 0, 0); control.SetRefit(PAX); }
	GraphVehicle alternative({GraphStop(A, 100000, 50), control, GraphStop(D, 0, 0)}, VehicleID{100});
	auto limits = ActivePassengerServiceGraphLimits(); limits.horizon = 1000;
	PassengerJourneyLimits query; query.candidates = 1;
	GameRandomSeedChecker seeds;
	const auto cold = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
	const auto warm = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
	PassengerJourneyPlanner planner(cold), other(warm);
	const auto before = planner.Plan(PAX, A, D, NOW, query), repeated = other.Plan(PAX, A, D, NOW, query);
	CHECK(before.status == PassengerPredictionStatus::Unknown);
	CHECK(before.journeys == repeated.journeys);
	CHECK(before.diagnostic.work == repeated.diagnostic.work);
	CHECK(cold.GetWork() == warm.GetWork());
	CHECK(cold.GetMemoryBound() == warm.GetMemoryBound());
	*alternative.GetOrder(1) = GraphStop(B, 0, 0);
	EnsurePassengerTestStations(alternative.orders->GetOrderVector()); InvalidatePassengerServiceIndex();
	const auto changed = PassengerServiceGraph::Build(PAX, NOW + 1, limits, PassengerServiceScope{A, D});
	PassengerJourneyPlanner repaired(changed);
	const auto after = repaired.Plan(PAX, A, D, NOW + 1, query);
	REQUIRE(after.HasJourney());
	CHECK(after.status == PassengerPredictionStatus::Complete);
	CHECK(after.journeys.front().legs.front().vehicle == alternative.index);
	CHECK(seeds.Check());
}

TEST_CASE("Unknown alternatives leave timed access and generalised costs unchanged", "[passenger][journey][uncertainty]")
{
	PassengerTestStations stations;
	GraphVehicle direct({GraphStop(A, 100000, 0), GraphStop(D, 100, 0)}, VehicleID{200});
	GraphVehicle access({GraphStop(A, 100000, 0), GraphStop(B, 200, 0)}, VehicleID{201});
	Order missing = GraphStop(C, 0, 0); missing.SetWaitTimetabled(false);
	GraphVehicle unknown({GraphStop(B, 100000, 50), missing, GraphStop(D, 0, 0)}, VehicleID{100});
	auto limits = ActivePassengerServiceGraphLimits(); limits.horizon = 1000;
	PassengerJourneyLimits query; query.candidates = 1;
	SECTION("An early unknown train cannot be accessed before the direct train arrives") {
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
		PassengerJourneyPlanner planner(graph);
		const auto result = planner.Plan(PAX, A, D, NOW, query);
		REQUIRE(result.HasJourney());
		CHECK(result.status == PassengerPredictionStatus::Unknown);
		CHECK(result.GetOutcome() == PassengerJourneyOutcome::TimedRouteFound);
		CHECK_FALSE(result.diagnostic.unknown_arrival_bound);
		CHECK(result.GetOutcome() == PassengerJourneyOutcome::TimedRouteFound);
	}
	SECTION("A late unknown arrival may still beat a penalised known journey") {
		direct.GetOrder(0)->SetWaitTime(90); direct.GetOrder(1)->SetTravelTime(10);
		access.GetOrder(1)->SetTravelTime(150);
		PassengerJourneyEvaluation evaluation; evaluation.waiting_weight = 1000;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits); // No excluded weighted excursions.
		PassengerJourneyPlanner planner(graph);
		const auto result = planner.Plan(PAX, A, D, NOW, query, evaluation);
		REQUIRE(result.HasJourney());
		CHECK(result.journeys.front().arrival == NOW + 100);
		CHECK(result.journeys.front().generalized_time == StateTicksDelta{190});
		CHECK_FALSE(result.diagnostic.unknown_arrival_bound);
		CHECK(result.GetOutcome() == PassengerJourneyOutcome::TimedRouteFound);
		CHECK(result.status == PassengerPredictionStatus::Unknown);
	}
}

TEST_CASE("Partial fleet timing and timed query budgets have separate outcomes", "[passenger][journey][uncertainty][limits]")
{
	PassengerTestStations stations;
	GraphVehicle first({GraphStop(A, 100000, 200), GraphStop(D, 100, 0)});
	GraphVehicle second(first, VehicleID{101});
	first.GetOrder(1)->SetWaitTimetabled(false);
	second.current_order.MakeGoToStation(A); second.current_order_time = 0;
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(250), PassengerServiceScope{A, D});
	CHECK(GraphTrip(graph, first.index).prediction.status == PassengerPredictionStatus::Complete);
	CHECK(GraphTrip(graph, second.index).prediction.status == PassengerPredictionStatus::Complete);
	/* Move this physical sibling to a station with unknown dwell, without editing orders. */
	second.LoadAt(1);
	const auto moved = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(250), PassengerServiceScope{A, D});
	CHECK(GraphTrip(moved, first.index).prediction.status == PassengerPredictionStatus::Complete);
	CHECK(GraphTrip(moved, second.index).prediction.status == PassengerPredictionStatus::Unknown);
	PassengerJourneyLimits query; query.candidates = 1;
	PassengerJourneyPlanner planner(moved);
	CHECK(planner.Plan(PAX, A, D, NOW, query).status == PassengerPredictionStatus::Unknown);
	query.work = 1;
	CHECK(planner.Plan(PAX, A, D, NOW, query).status == PassengerPredictionStatus::BudgetExhausted);
	query.work = 1048576; query.memory_bytes = 1;
	CHECK(planner.Plan(PAX, A, D, NOW, query).status == PassengerPredictionStatus::LimitReached);
	first.GetOrder(1)->SetWaitTimetabled(true);
	const auto updated = PassengerServiceGraph::Build(PAX, NOW + 1, GraphLimits(500), PassengerServiceScope{A, D});
	CHECK(updated.GetCoverage(A, D) == PassengerServiceGraph::Coverage::GlobalComplete);
	query.memory_bytes = 128 * 1024 * 1024;
	PassengerJourneyPlanner recovered(updated);
	CHECK(recovered.Plan(PAX, A, D, NOW + 1, query).status == PassengerPredictionStatus::Complete);
}

TEST_CASE("Timed choices match the timed-only network and perform no unknown bound search", "[passenger][journey][uncertainty][reference]")
{
	PassengerTestStations stations;
	GraphVehicle direct({GraphStop(A, 100000, 0), GraphStop(D, 100, 0)}, VehicleID{200});
	GraphVehicle slower({GraphStop(A, 100000, 0), GraphStop(D, 150, 0)}, VehicleID{201});
	PassengerJourneyLimits query; query.candidates = 1;
	auto limits = ActivePassengerServiceGraphLimits(); limits.horizon = 1000;
	const auto timed = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
	PassengerJourneyPlanner reference(timed);
	const auto expected = reference.Plan(PAX, A, D, NOW, query);
	REQUIRE(expected.GetOutcome() == PassengerJourneyOutcome::TimedRouteFound);
	GraphVehicle alternative({GraphStop(A, 100000, 0), GraphStop(B, 0, 0), GraphStop(D, 0, 0)}, VehicleID{100});
	for (TimetableTicks delay : {0, 50, 100, 150, 200}) {
		CAPTURE(delay);
		alternative.GetOrder(0)->SetWaitTime(delay);
		alternative.GetOrder(1)->SetWaitTimetabled(false);
		const auto partial = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
		PassengerJourneyPlanner planner(partial);
		const auto actual = planner.Plan(PAX, A, D, NOW, query);
		REQUIRE(actual.GetOutcome() == PassengerJourneyOutcome::TimedRouteFound);
		CHECK(actual.status == PassengerPredictionStatus::Unknown);
		CHECK(actual.journeys.front() == expected.journeys.front());
		CHECK_FALSE(actual.diagnostic.unknown_arrival_bound);
		CHECK_FALSE(actual.diagnostic.uncertainty_resolved);
		CHECK(actual.diagnostic.unknown_alternatives_ignored);
	}
	query.work = 1;
	const auto limited = reference.Plan(PAX, A, D, NOW, query);
	CHECK(limited.GetOutcome() == PassengerJourneyOutcome::BudgetDeferred);
	CHECK_FALSE(limited.IsUnreachable());
	query.work = 1048576; query.labels = 0;
	CHECK(reference.Plan(PAX, A, D, NOW, query).GetOutcome() == PassengerJourneyOutcome::BudgetDeferred);
	query.labels = 65536; query.memory_bytes = 1;
	CHECK(reference.Plan(PAX, A, D, NOW, query).GetOutcome() == PassengerJourneyOutcome::BudgetDeferred);
}

TEST_CASE("Graph admission pays for generated visits rather than maximum trip reservations", "[passenger][service_graph][scaling]")
{
	PassengerTestStations stations;
	GraphVehicle first({GraphStop(A, 10, 0), GraphStop(D, 10, 0)});
	std::deque<GraphVehicle> fleet;
	for (uint32_t i = 1; i < 4000; ++i) fleet.emplace_back(first, VehicleID{1000 + i});
	auto limits = ActivePassengerServiceGraphLimits(); limits.horizon = 260;
	const auto cold = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
	const auto warm = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
	REQUIRE(cold.GetTrips().size() == 4000);
	CHECK(cold.GetCoverage(A, D) != PassengerServiceGraph::Coverage::Incomplete);
	CHECK(cold.GetDiagnostic().visits >= 100000);
	CHECK(cold.GetDiagnostic().admitted_visits == cold.GetDiagnostic().visits);
	CHECK(cold.GetMemoryBound() < 64 * 1024 * 1024);
	CHECK(cold.GetMemoryBound() == warm.GetMemoryBound());
	CHECK(cold.GetWork() == warm.GetWork());
	CHECK(cold.GetDiagnostic().allocation_bytes + cold.GetDiagnostic().static_allocation_bytes < cold.GetMemoryBound());
	/* The previous 128-visit reservation alone exhausted standard memory at this fleet size. */
	CHECK(4000 * (1024 + 320 * 128) > limits.memory_bytes);
	PassengerJourneyPlanner planner(cold);
	PassengerJourneyLimits query; query.work = 4 * 1024 * 1024; query.candidates = 1;
	auto result = planner.Plan(PAX, A, D, NOW, query);
	CHECK(result.status == PassengerPredictionStatus::Complete);
	CHECK(result.HasJourney());
	WARN("Actual-visit fleet: trips=" << cold.GetTrips().size() << "; visits=" << cold.GetDiagnostic().visits << "; work=" << cold.GetWork()
			<< "; bound=" << cold.GetMemoryBound() << "; retained=" << cold.GetDiagnostic().allocation_bytes << "; cold_us=" << cold.GetDiagnostic().build_microseconds
			<< "; warm_us=" << warm.GetDiagnostic().build_microseconds);
}

TEST_CASE("High frequency prediction extends past 128 occurrences without hiding limits", "[passenger][service_graph][scaling]")
{
	PassengerTestStations stations;
	GraphVehicle train({GraphStop(A, 10, 0), GraphStop(D, 10, 0)});
	auto limits = GraphLimits(4000);
	const auto limited = PassengerServiceGraph::Build(PAX, NOW, limits);
	CHECK(GraphTrip(limited, train.index).prediction.stop == PassengerPredictionStop::Visits);
	CHECK(limited.GetCoverage(A, D) == PassengerServiceGraph::Coverage::Incomplete);
	CHECK(limited.GetDiagnostic().prediction_stops[static_cast<size_t>(PassengerPredictionStop::Visits)] == 1);
	const auto &first_failure = limited.GetDiagnostic().first_prediction_failure;
	CHECK(first_failure.reason == PassengerPredictionStop::Visits);
	CHECK(first_failure.vehicle == train.index);
	CHECK(first_failure.work >= 128);
	CHECK(first_failure.visits == 128);
	CHECK(first_failure.complete_until == GraphTrip(limited, train.index).prediction.complete_until);
	CHECK(first_failure.required_until == NOW + 4000);
	limits.visits_per_trip = 4096;
	const auto complete = PassengerServiceGraph::Build(PAX, NOW, limits);
	CHECK(GraphTrip(complete, train.index).prediction.visits.size() > 128);
	CHECK(complete.GetCoverage(A, D) == PassengerServiceGraph::Coverage::GlobalComplete);
	CHECK(complete.GetMemoryBound() <= limits.memory_bytes);
}

TEST_CASE("Graph failure diagnostics distinguish independent resource and prediction ceilings", "[passenger][service_graph][scaling]")
{
	PassengerTestStations stations;
	GraphVehicle train({GraphStop(A, 10, 0), GraphStop(D, 10, 0)});
	GraphVehicle second({GraphStop(A, 10, 0), GraphStop(D, 10, 0)}, VehicleID{101});
	auto limits = GraphLimits(4000);
	SECTION("Memory") { limits.memory_bytes = 10000; }
	SECTION("Work") { limits.work = 1; }
	SECTION("Services") { limits.services = 1; }
	SECTION("Trips") { limits.trips = 1; }
	SECTION("Total visits") { limits.total_visits = 3; }
	SECTION("Prediction steps") { limits.steps_per_trip = 2; }
	SECTION("Timing") { train.GetOrder(1)->SetWaitTimetabled(false); }
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
	CHECK(graph.GetCoverage(A, D) == PassengerServiceGraph::Coverage::Incomplete);
	CHECK(graph.GetMemoryBound() <= limits.memory_bytes);
	CHECK(graph.GetWork() <= limits.work);
	const auto reason = graph.GetDiagnostic().first_failure.reason;
	if (limits.memory_bytes == 10000) CHECK(reason == PassengerGraphFailure::Memory);
	if (limits.work == 1) CHECK(reason == PassengerGraphFailure::Work);
	if (limits.services == 1) CHECK(reason == PassengerGraphFailure::Services);
	if (limits.trips == 1) CHECK(reason == PassengerGraphFailure::Trips);
	if (limits.total_visits == 3) CHECK(reason == PassengerGraphFailure::TotalVisits);
	if (limits.steps_per_trip == 2) CHECK(graph.GetDiagnostic().prediction_stops[static_cast<size_t>(PassengerPredictionStop::Steps)] == 2);
	if (!train.GetOrder(1)->IsWaitTimetabled()) CHECK(GraphTrip(graph, train.index).prediction.stop == PassengerPredictionStop::Timing);
}

TEST_CASE("Graph discovery excludes definite freight before timetable capture", "[passenger][service_graph][performance]")
{
	PassengerTestStations stations;
	std::deque<GraphVehicle> freight;
	for (uint32_t i = 0; i < 5000; ++i) { freight.emplace_back(std::vector<Order>{GraphStop(A), GraphStop(B)}, VehicleID{1000 + i}); freight.back().cargo_type = CargoType{1}; }
	GraphVehicle passenger({GraphStop(A), GraphStop(D)}, VehicleID{9000});
	auto limits = GraphLimits(); limits.work = 32768; limits.memory_bytes = 1024 * 1024;
	GameRandomSeedChecker random;
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
	REQUIRE(graph.GetStatus() == PassengerPredictionStatus::Complete);
	CHECK(graph.GetTrips().size() == 1);
	CHECK(graph.GetDiagnostic().trains == 5001);
	CHECK(graph.GetDiagnostic().cargo_capable == 1);
	CHECK(graph.GetDiagnostic().excluded_cargo == 5000);
	CHECK(graph.GetDiagnostic().predicted == 1);
	CHECK(graph.GetWork() < limits.work);
	CHECK(graph.GetMemoryBound() <= limits.memory_bytes);
	CHECK(random.Check());
}

TEST_CASE("Graph allocation allowance is independent of traversal work", "[passenger][service_graph]")
{
	PassengerTestStations stations;
	std::vector<Order> orders;
	for (uint16_t i = 0; i < 400; ++i) orders.push_back(GraphStop(StationID{i}, 1, 1));
	GraphVehicle train(std::move(orders));
	auto limits = GraphLimits(1000); limits.memory_bytes = 1024 * 1024;
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
	CHECK(graph.GetStatus() == PassengerPredictionStatus::Complete);
	CHECK(graph.GetWork() > limits.memory_bytes / 1024);
	CHECK(graph.GetMemoryBound() <= limits.memory_bytes);
	CHECK(graph.GetDiagnostic().allocation_bytes < graph.GetMemoryBound());
	limits.memory_bytes = 100;
	CHECK(PassengerServiceGraph::Build(PAX, NOW, limits).GetStatus() == PassengerPredictionStatus::LimitReached);
}

TEST_CASE("Freight topology cannot expand a passenger corridor and live refits rebuild its view", "[passenger][service_graph][cache]")
{
	PassengerTestStations stations;
	GraphVehicle direct({GraphStop(A), GraphStop(D)});
	GraphVehicle branch({GraphStop(A), GraphStop(B)}, VehicleID{101});
	GraphVehicle bridge({GraphStop(B), GraphStop(D)}, VehicleID{102});
	bridge.cargo_type = CargoType{1};
	const PassengerServiceScope scope{A, D};
	const auto cold = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope);
	REQUIRE(cold.GetQueryStatus(A, D) == PassengerPredictionStatus::Complete);
	CHECK(cold.GetTrips().size() == 1);
	CHECK(cold.GetDiagnostic().excluded_cargo == 1);
	CHECK(cold.GetDiagnostic().excluded_scope == 1);
	const auto warm = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope);
	CHECK(warm.GetDiagnostic().static_cache_hit);
	CHECK(warm.GetWork() == cold.GetWork());
	/* No topology hook is called: the exact live eligibility set must invalidate reuse. */
	bridge.cargo_type = PAX;
	const auto refitted = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), scope);
	CHECK_FALSE(refitted.GetDiagnostic().static_cache_hit);
	CHECK(refitted.GetTrips().size() == 3); // The branch is now a real alternative via B.
	CHECK(refitted.GetQueryStatus(A, D) == PassengerPredictionStatus::Complete);
}

TEST_CASE("Scoped graph cache residency cannot change capture or logical allowances", "[passenger][service_graph][cache]")
{
	PassengerTestStations stations;
	GraphVehicle direct({GraphStop(A), GraphStop(D)});
	GraphVehicle branch({GraphStop(A), GraphStop(B), GraphStop(C)}, VehicleID{101});
	InvalidatePassengerServiceIndex();
	const auto cold = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
	const auto warm = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
	REQUIRE(cold.GetTrips().size() == 1);
	CHECK(cold.GetCoverage(A, D) == PassengerServiceGraph::Coverage::ScopedComplete);
	CHECK(cold.GetStatus() == PassengerPredictionStatus::Unknown);
	CHECK(cold.GetQueryStatus(B, D) == PassengerPredictionStatus::Unknown);
	CHECK(cold.GetVisits(B, NOW, NOW + 1000).status == PassengerPredictionStatus::Unknown);
	CHECK(cold.GetDepartures(A, NOW, NOW + 1000).status == PassengerPredictionStatus::Unknown);
	CHECK_FALSE(cold.GetDiagnostic().static_cache_hit);
	CHECK(warm.GetDiagnostic().static_cache_hit);
	CHECK(cold.GetWork() == warm.GetWork());
	CHECK(cold.GetMemoryBound() == warm.GetMemoryBound());
	CHECK(cold.GetDiagnostic().visits == warm.GetDiagnostic().visits);
	CHECK(cold.GetTrips().front().vehicle == warm.GetTrips().front().vehicle);
	/* Destination edits use the same invalidation hook as insert/delete/clone/free/afterload. */
	branch.GetOrder(2)->SetDestination(D); InvalidatePassengerServiceIndex();
	const auto changed = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
	CHECK_FALSE(changed.GetDiagnostic().static_cache_hit);
	CHECK(changed.GetTrips().size() == 2);
}

TEST_CASE("Implicit freight and unavailable trains cannot poison a passenger scope", "[passenger][service_graph][scope]")
{
	PassengerTestStations stations;
	GraphVehicle direct({GraphStop(A), GraphStop(D)});
	GraphVehicle irrelevant({GraphStop(A), GraphStop(B)}, VehicleID{101});
	irrelevant.GetOrder(1)->MakeImplicit(B); InvalidatePassengerServiceIndex();
	SECTION("Definite freight") { irrelevant.cargo_type = CargoType{1}; }
	SECTION("Stopped passenger train") { irrelevant.vehstatus.Set(VehState::Stopped); }
	SECTION("Crashed passenger train") { irrelevant.vehstatus.Set(VehState::Crashed); }
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
	CHECK(graph.GetQueryStatus(A, D) == PassengerPredictionStatus::Complete);
	CHECK(graph.GetTrips().size() == 1);
	CHECK(graph.GetDiagnostic().predicted == 1);
}

TEST_CASE("Historic implicit stops are skipped consistently by scoped forecasts and topology", "[passenger][service_graph][scope][cache]")
{
	PassengerTestStations stations;
	GraphVehicle direct({GraphStop(A), GraphStop(D)});
	Order implicit; implicit.MakeImplicit(StationID{12});
	GraphVehicle remote({GraphStop(StationID{10}), implicit, GraphStop(StationID{11})}, VehicleID{101});
	SECTION("Disconnected running passengers do not poison the OD") {
		const auto cold = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
		const auto warm = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
		CHECK(cold.GetQueryStatus(A, D) == PassengerPredictionStatus::Complete);
		CHECK(cold.GetCoverage(A, D) == PassengerServiceGraph::Coverage::ScopedComplete);
		CHECK(cold.GetTrips().size() == 1);
		CHECK(cold.GetDiagnostic().excluded_scope == 1);
		CHECK(warm.GetQueryStatus(A, D) == cold.GetQueryStatus(A, D));
		CHECK(warm.GetWork() == cold.GetWork());
	}
	SECTION("Historic implicit stops do not create a possible transfer") {
		remote.GetOrder(1)->MakeImplicit(A);
		remote.GetOrder(2)->MakeGoToStation(D);
		InvalidatePassengerServiceIndex();
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
		CHECK(graph.GetTrips().size() == 1);
		CHECK(graph.GetQueryStatus(A, D) == PassengerPredictionStatus::Complete); // The unknown alternative is inventoried.
		PassengerJourneyPlanner planner(graph);
		CHECK(planner.Plan(PAX, A, D, NOW).status == PassengerPredictionStatus::Complete);
		CHECK(graph.GetServicesAtStation(A).size() == 1);
		CHECK(graph.GetCoverage(A, D) == PassengerServiceGraph::Coverage::ScopedComplete);
	}
	SECTION("An unknown implicit station does not invent an additional stop") {
		remote.GetOrder(1)->MakeImplicit(StationID::Invalid()); InvalidatePassengerServiceIndex();
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
		CHECK(graph.GetQueryStatus(A, D) == PassengerPredictionStatus::Complete);
		CHECK(graph.GetTrips().size() == 1);
	}
	SECTION("Implicit-only history does not create a forecast passenger service") {
		remote.GetOrder(0)->MakeImplicit(StationID{10}); remote.GetOrder(2)->MakeImplicit(StationID{11});
		remote.current_order.Free(); remote.cur_implicit_order_index = 1;
		InvalidatePassengerServiceIndex();
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
		CHECK(graph.GetQueryStatus(A, D) == PassengerPredictionStatus::Complete);
		CHECK(graph.GetTrips().size() == 1);
		PassengerJourneyPlanner planner(graph);
		CHECK(planner.Plan(PAX, A, D, NOW).status == PassengerPredictionStatus::Complete);
	}
	SECTION("An implicit-only history at this OD has no forecast boarding") {
		remote.GetOrder(0)->MakeImplicit(A); remote.GetOrder(2)->MakeImplicit(D);
		remote.current_order.Free(); remote.cur_implicit_order_index = 1;
		InvalidatePassengerServiceIndex();
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
		CHECK(graph.GetTrips().size() == 1);
		CHECK(graph.GetQueryStatus(A, D) == PassengerPredictionStatus::Complete);
		PassengerJourneyPlanner planner(graph);
		PassengerJourneyLimits query; query.candidates = 1;
		const auto result = planner.Plan(PAX, A, D, NOW, query);
		CHECK(result.HasJourney());
		CHECK(result.status == PassengerPredictionStatus::Complete);
		CHECK(graph.GetService(remote.orders->index) == nullptr);
	}
	SECTION("A known implicit order after the proven horizon is harmless") {
		Order later_implicit; later_implicit.MakeImplicit(StationID{22});
		GraphVehicle late({GraphStop(A), GraphStop(D), GraphStop(StationID{21}, 100000), later_implicit}, VehicleID{102});
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D});
		CHECK(graph.GetTrips().size() == 2);
		CHECK(graph.GetQueryStatus(A, D) == PassengerPredictionStatus::Complete);
		CHECK(graph.GetCoverage(A, D) == PassengerServiceGraph::Coverage::ScopedComplete);
	}
	SECTION("Only explicit instructions define the operational model") {
		remote.GetOrder(0)->MakeImplicit(StationID{10}); remote.GetOrder(2)->MakeImplicit(StationID{11});
		InvalidatePassengerServiceIndex();
		CHECK(PassengerServiceGraph::Build(PAX, NOW, GraphLimits(), PassengerServiceScope{A, D}).GetQueryStatus(A, D) == PassengerPredictionStatus::Complete);
	}
}

TEST_CASE("Scoped capture stops at the first unknown dwell and recovers after an order edit", "[passenger][service_graph][scope][scaling]")
{
	PassengerTestStations stations;
	Order implicit; implicit.MakeImplicit(B);
	GraphVehicle head({GraphStop(A), implicit, GraphStop(D)});
	head.GetOrder(0)->SetWaitTimetabled(false);
	std::deque<GraphVehicle> fleet;
	for (uint i = 0; i < 1000; ++i) fleet.emplace_back(head, VehicleID{1000 + i});
	auto limits = ActivePassengerServiceGraphLimits(); limits.horizon = 4096;
	GameRandomSeedChecker seeds;
	const auto cold = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
	const auto warm = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
	CHECK(cold.GetQueryStatus(A, D) == PassengerPredictionStatus::Complete);
	CHECK(cold.GetCoverage(A, D) == PassengerServiceGraph::Coverage::Incomplete);
	CHECK(cold.GetDiagnostic().selected_trips == 1001);
	CHECK(cold.GetTrips().size() == 1001);
	CHECK(cold.GetDiagnostic().predicted == 1001);
	CHECK(cold.GetDiagnostic().omitted_trains == 0);
	CHECK(cold.GetDiagnostic().first_failure.reason == PassengerGraphFailure::None);
	CHECK(cold.GetDiagnostic().timing_uncertainty == 1001);
	PassengerJourneyPlanner planner(cold);
	CHECK(planner.Plan(PAX, A, D, NOW).status == PassengerPredictionStatus::Unknown);
	CHECK(warm.GetWork() == cold.GetWork());
	CHECK(warm.GetMemoryBound() == cold.GetMemoryBound());
	*head.GetOrder(1) = GraphStop(B);
	head.GetOrder(0)->SetWaitTimetabled(true);
	EnsurePassengerTestStations(head.orders->GetOrderVector());
	InvalidatePassengerServiceIndex();
	const auto repaired = PassengerServiceGraph::Build(PAX, NOW, limits, PassengerServiceScope{A, D});
	CHECK(repaired.GetQueryStatus(A, D) == PassengerPredictionStatus::Complete);
	CHECK(repaired.GetCoverage(A, D) == PassengerServiceGraph::Coverage::GlobalComplete);
	CHECK(repaired.GetTrips().size() == 1001);
	CHECK(seeds.Check());
}

TEST_CASE("Passenger graph separates local and express services and shared trips", "[passenger][service_graph]")
{
	PassengerTestStations stations;
	GraphVehicle local({ GraphStop(A), GraphStop(B, 200), GraphStop(C, 300), GraphStop(D, 400) });
	GraphVehicle express({ GraphStop(B, 100, 40), GraphStop(D, 100) }, VehicleID{101});
	GraphVehicle second(express, VehicleID{102});
	second.lateness_counter = -200;
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
	REQUIRE(graph.GetServices().size() == 2);
	REQUIRE(graph.GetTrips().size() == 3);
	REQUIRE(graph.GetServicesAtStation(B).size() == 2);
	REQUIRE(graph.GetServicesAtStation(C).size() == 1);
	const auto *slow = graph.GetService(local.orders->index);
	const auto *fast = graph.GetService(express.orders->index);
	REQUIRE(slow != nullptr);
	REQUIRE(fast != nullptr);
	CHECK(slow->stops.size() == 4);
	CHECK(fast->stops.size() == 2);
	CHECK(slow->stops[2].station == C);
	CHECK(slow->stops[2].order == 2);
	CHECK(slow->segments[0].travel == StateTicksDelta{200});
	CHECK(fast->segments[0].travel == StateTicksDelta{100});
	CHECK(fast->trips.size() == 2);
	CHECK(fast->cycle_known);
	const auto &first_trip = GraphTrip(graph, express.index);
	const auto &second_trip = GraphTrip(graph, second.index);
	CHECK(first_trip.service == second_trip.service);
	CHECK(first_trip.capacity == 80);
	CHECK(second_trip.capacity == 120);
	CHECK(first_trip.prediction.visits[0].departure == NOW + 40);
	CHECK(second_trip.prediction.visits[0].departure == NOW + 240);
	CHECK(first_trip.prediction.visits[1].arrival == NOW + 140);
	CHECK(second_trip.prediction.visits[1].arrival == NOW + 340);
	const auto boardable = graph.GetBoardableTrips(B, NOW, NOW + 1000);
	REQUIRE_FALSE(boardable.items.empty());
	CHECK(graph.GetService(OrderListID::Invalid()) == nullptr);
	CHECK(graph.GetServicesAtStation(StationID{999}).empty());
	CHECK(graph.GetVisits(StationID{999}, NOW, NOW + 1000).status == PassengerPredictionStatus::Complete);
}

TEST_CASE("Passenger graph stop permissions and forced unloading bound through rides", "[passenger][service_graph]")
{
	PassengerTestStations stations;
	auto a = GraphStop(A), b = GraphStop(B), c = GraphStop(C), d = GraphStop(D);
	b.SetNonStopType(ONSF_NO_STOP_AT_DESTINATION_STATION);
	c.SetLoadType(OrderLoadType::NoLoad);
	c.SetUnloadType(OrderUnloadType::NoUnload);
	GraphVehicle train({ a, b, c, d });
	SECTION("Pass-through and closed stops") {
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(400));
		const auto *service = graph.GetService(train.orders->index);
		REQUIRE(service->stops.size() == 3);
		CHECK(graph.GetServicesAtStation(B).empty());
		CHECK_FALSE(service->stops[1].can_board);
		CHECK_FALSE(service->stops[1].can_alight);
		CHECK(graph.GetBoardableTrips(C, NOW, NOW + 400).items.empty());
		const auto options = graph.GetBoardableTrips(A, NOW, NOW + 400);
		REQUIRE(options.items.size() == 1);
		REQUIRE(options.items[0].reachable.size() == 1);
		const auto id = options.items[0].reachable[0];
		CHECK(graph.GetTrips()[id.trip].prediction.visits[id.visit].order == 3);
	}
	SECTION("Forced unload and transfer") {
		for (auto unload : { OrderUnloadType::Unload, OrderUnloadType::Transfer }) {
			train.orders->GetOrderAt(2)->SetUnloadType(unload);
			const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(400));
			CHECK(graph.GetService(train.orders->index)->stops[1].forced_transfer);
			const auto options = graph.GetBoardableTrips(A, NOW, NOW + 400);
			REQUIRE(options.items.size() == 1);
			REQUIRE(options.items[0].reachable.size() == 1);
			const auto id = options.items[0].reachable[0];
			CHECK(graph.GetTrips()[id.trip].prediction.visits[id.visit].order == 2);
		}
	}
}

TEST_CASE("Passenger graph capacity includes only the requested cargo across a consist", "[passenger][service_graph]")
{
	PassengerTestStations stations;
	GraphVehicle train({ GraphStop(A), GraphStop(B) });
	/* SetNext updates tagged pool pointers, so consist parts must be real pool entries. */
	GraphCoach *passengers = Vehicle::Create<GraphCoach>();
	GraphCoach *other = Vehicle::Create<GraphCoach>();
	passengers->cargo_type = PAX;
	passengers->cargo_cap = 50;
	other->cargo_type = CargoType{1};
	other->cargo_cap = 90;
	train.SetNext(passengers);
	passengers->SetNext(other);
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
	train.SetNext(nullptr);
	passengers->SetNext(nullptr);
	delete passengers;
	delete other;
	CHECK(GraphTrip(graph, train.index).capacity == 130);
}

TEST_CASE("Passenger graph retains partial times and reuses journey timing", "[passenger][service_graph]")
{
	PassengerTestStations stations;
	GraphVehicle train({ GraphStop(A, 100, 10), GraphStop(B, 600, 100), GraphStop(D, 600) });
	train.LoadAt(1);
	train.current_order = *train.GetOrder(1);
	train.current_order_time = 400;
	train.lateness_counter = 40;
	SECTION("Known progress, dwell and delay") {
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(1000));
		const auto &trip = GraphTrip(graph, train.index);
		REQUIRE(trip.prediction.visits.size() >= 2);
		CHECK(trip.prediction.visits[0].arrival == NOW + 200);
		CHECK(trip.prediction.visits[0].departure == NOW + 260);
		CHECK(trip.prediction.visits[1].arrival == NOW + 860);
		const auto state = GetPassengerRouteState(&train);
		REQUIRE(state);
		const auto journey = PredictPassengerJourney(*state, 1, D, PAX, NOW, true);
		CHECK(trip.prediction.visits[0].departure == journey.departure);
		CHECK(trip.prediction.visits[1].arrival == journey.arrival);
		CHECK(trip.prediction.status == PassengerPredictionStatus::Complete);
		CHECK(graph.GetVisits(B, NOW + 300, NOW + 1000).items.empty());
	}
	SECTION("Missing dwell retains arrival but not departure") {
		train.orders->GetOrderAt(1)->SetWaitTimetabled(false);
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		const auto &trip = GraphTrip(graph, train.index);
		CHECK(trip.prediction.visits[0].arrival == NOW + 200);
		CHECK_FALSE(trip.prediction.visits[0].departure);
		CHECK_FALSE(trip.prediction.visits[1].arrival);
		CHECK(trip.prediction.visits[1].travel_from_previous == StateTicksDelta{600});
		CHECK(graph.GetBoardableTrips(B, NOW, NOW + 1000).status == PassengerPredictionStatus::Unknown);
	}
	SECTION("Missing travel or overdue travel has no invented arrival") {
		train.orders->GetOrderAt(1)->SetTravelTime(0);
		train.orders->GetOrderAt(1)->SetTravelTimetabled(false);
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		CHECK_FALSE(GraphTrip(graph, train.index).prediction.visits[0].arrival);
	}
	SECTION("Full loading preserves arrival only") {
		train.orders->GetOrderAt(1)->SetLoadType(OrderLoadType::FullLoad);
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		CHECK(GraphTrip(graph, train.index).prediction.visits[0].arrival == NOW + 200);
		CHECK_FALSE(GraphTrip(graph, train.index).prediction.visits[0].departure);
	}
	SECTION("Another cargo's full load also makes departure unknown") {
		train.orders->GetOrderAt(1)->SetLoadType(OrderLoadType::CargoTypeLoad);
		train.orders->GetOrderAt(1)->SetLoadType(OrderLoadType::FullLoad, CargoType{1});
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		CHECK(GraphTrip(graph, train.index).prediction.visits[0].arrival == NOW + 200);
		CHECK_FALSE(GraphTrip(graph, train.index).prediction.visits[0].departure);
	}
}

TEST_CASE("Passenger graph waypoint segments, cargo flags and direction are copied", "[passenger][service_graph]")
{
	PassengerTestStations stations;
	Order waypoint;
	waypoint.MakeGoToWaypoint(StationID{4});
	waypoint.SetTravelTime(40);
	waypoint.SetTravelTimetabled(true);
	waypoint.SetWaitTime(20);
	waypoint.SetWaitTimetabled(true);
	waypoint.SetWaypointFlags({ OrderWaypointFlag::Reverse });
	Order b = GraphStop(B, 100);
	b.SetLoadType(OrderLoadType::CargoTypeLoad);
	b.SetLoadType(OrderLoadType::NoLoad, PAX);
	b.SetUnloadType(OrderUnloadType::CargoTypeUnload);
	b.SetUnloadType(OrderUnloadType::NoUnload, PAX);
	GraphVehicle train({ GraphStop(A), waypoint, b });
	train.vehicle_flags.Set(VehicleFlag::DrivingBackwards);
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(300));
	const auto *service = graph.GetService(train.orders->index);
	REQUIRE(service->segments.size() >= 1);
	CHECK(service->segments[0].travel == StateTicksDelta{160});
	REQUIRE(service->reverse_waypoints.size() == 1);
	CHECK(service->reverse_waypoints[0] == 1);
	CHECK_FALSE(service->stops[1].can_board);
	CHECK_FALSE(service->stops[1].can_alight);
	CHECK(GraphTrip(graph, train.index).direction == ReverseDir(Direction::NE));
	CHECK(graph.GetServicesAtStation(StationID{4}).empty());
}

TEST_CASE("Passenger graph unknown branches, depots and refits never certify a path", "[passenger][service_graph]")
{
	PassengerTestStations stations;
	Order barrier;
	barrier.MakeConditional(3);
	barrier.SetConditionVariable(OrderConditionVariable::LoadPercentage);
	GraphVehicle train({ GraphStop(A), barrier, GraphStop(B), GraphStop(D) });
	SECTION("Dynamic conditional") {}
	SECTION("Depot") { train.orders->GetOrderAt(1)->MakeGoToDepot(DestinationID{0}, OrderDepotTypeFlags{}); }
	SECTION("Refit") { *train.orders->GetOrderAt(1) = GraphStop(C); train.orders->GetOrderAt(1)->SetRefit(PAX); }
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
	const auto &trip = GraphTrip(graph, train.index);
	REQUIRE(trip.prediction.visits.size() == 1);
	CHECK(trip.prediction.status == PassengerPredictionStatus::Unknown);
	const auto options = graph.GetBoardableTrips(A, NOW, NOW + 1000);
	REQUIRE(options.items.size() == 1);
	CHECK(options.items[0].reachable.empty());
	CHECK(options.status == PassengerPredictionStatus::Unknown);
	CHECK(graph.GetVisits(D, NOW, NOW + 1000).status == PassengerPredictionStatus::Unknown);
}

TEST_CASE("Passenger graph handles repeated stops, unconditional jumps and bounded cycles", "[passenger][service_graph]")
{
	PassengerTestStations stations;
	GraphVehicle train({ GraphStop(A), GraphStop(B), GraphStop(A), GraphStop(D) });
	SECTION("Repeated station orders and circuits") {
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		const auto *service = graph.GetService(train.orders->index);
		CHECK(service->stops[0].station == service->stops[2].station);
		CHECK(service->stops[0].order != service->stops[2].order);
		CHECK(graph.GetServicesAtStation(A).size() == 1);
		const auto visits = graph.GetVisits(A, NOW, NOW + 1000);
		REQUIRE(visits.items.size() >= 4);
		CHECK(visits.items[0] != visits.items[1]);
	}
	SECTION("Known unconditional jump uses branch time") {
		Order *branch = train.orders->GetOrderAt(1);
		branch->MakeConditional(3);
		branch->SetConditionVariable(OrderConditionVariable::Unconditionally);
		branch->SetWaitTime(50);
		branch->SetWaitTimetabled(true);
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(400));
		CHECK(GraphTrip(graph, train.index).prediction.visits[1].order == 3);
		CHECK(GraphTrip(graph, train.index).prediction.visits[1].arrival == NOW + 60);
	}
	SECTION("Conditional self loop") {
		Order *branch = train.orders->GetOrderAt(1);
		branch->MakeConditional(1);
		branch->SetConditionVariable(OrderConditionVariable::Unconditionally);
		branch->SetWaitTime(0);
		branch->SetWaitTimetabled(true);
		auto limits = GraphLimits();
		limits.steps_per_trip = 20;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		CHECK(GraphTrip(graph, train.index).prediction.status == PassengerPredictionStatus::LimitReached);
	}
	SECTION("Zero-time circuit") {
		for (Order *order : train.orders->Orders()) { order->SetWaitTime(0); order->SetTravelTime(0); }
		auto limits = GraphLimits();
		limits.visits_per_trip = 7;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		CHECK(GraphTrip(graph, train.index).prediction.visits.size() == 7);
		CHECK(GraphTrip(graph, train.index).prediction.status == PassengerPredictionStatus::LimitReached);
	}
}

TEST_CASE("Passenger graph dispatch is independent per vehicle and owns its snapshot", "[passenger][service_graph]")
{
	PassengerTestStations stations;
	GraphVehicle train({ GraphStop(A, 10, 0), GraphStop(B, 10, 0), GraphStop(D, 10, 0) });
	train.orders->GetOrderAt(1)->SetDispatchScheduleIndex(0);
	train.vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
	train.orders->GetScheduledDispatchScheduleSet().resize(1);
	auto &schedule = train.orders->GetDispatchScheduleByIndex(0);
	schedule.SetScheduledDispatchStartTick(NOW);
	schedule.SetScheduledDispatchDuration(1000);
	schedule.GetScheduledDispatchMutable() = { { 100, 0, {} }, { 350, 0, {} } };
	GameRandomSeedChecker seeds;
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(500));
	const auto &trip = GraphTrip(graph, train.index);
	REQUIRE(trip.prediction.visits.size() >= 3);
	CHECK(trip.prediction.visits[1].arrival == NOW + 10);
	CHECK(trip.prediction.visits[1].departure == NOW + 100);
	CHECK(trip.prediction.visits[2].arrival == NOW + 110);
	CHECK(schedule.GetScheduledDispatchLastDispatch() == INVALID_SCHEDULED_DISPATCH_OFFSET);
	CHECK(train.current_order_time == 0);
	CHECK(train.lateness_counter == 0);
	CHECK(train.cur_real_order_index == 0);
	CHECK(train.current_order.IsAnyLoadingType());
	const auto connection = [&]() {
		size_t budget = 32768;
		return PredictPassengerConnection(*GetPassengerRouteState(&train), 1, D, PAX, NOW, NOW, NOW + 500, budget);
	}();
	CHECK(connection.departure == trip.prediction.visits[1].departure);
	CHECK(connection.arrival == trip.prediction.visits[2].arrival);
	SECTION("Source edits and destruction do not alter predictions or queries") {
		const OrderListID id = train.orders->index;
		schedule.SetScheduledDispatchLastDispatch(999);
		schedule.GetScheduledDispatchMutable().clear();
		train.orders->GetOrderAt(1)->SetTravelTime(999);
		train.lateness_counter = 999;
		train.orders->FreeChain();
		train.orders = nullptr;
		const auto visits = graph.GetVisits(B, NOW, NOW + 500);
		CHECK_FALSE(visits.items.empty());
		CHECK(graph.GetService(id)->schedules[0].GetScheduledDispatchLastDispatch() == INVALID_SCHEDULED_DISPATCH_OFFSET);
		CHECK(GraphTrip(graph, train.index).prediction.visits[1].departure == NOW + 100);
	}
	SECTION("Unassigned competing dispatch slots stay unknown") {
		GraphVehicle second(train, VehicleID{101});
		second.vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
		const auto competing = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(500));
		CHECK_FALSE(GraphTrip(competing, train.index).prediction.visits[1].departure);
		CHECK(competing.GetVisits(D, NOW, NOW + 500).status == PassengerPredictionStatus::Unknown);
		schedule.SetScheduledDispatchReuseSlots(true);
		const auto reusable = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(500));
		CHECK(GraphTrip(reusable, train.index).prediction.visits[1].departure == NOW + 100);
	}
	SECTION("Invalid dispatch retains its known arrival only") {
		schedule.SetScheduledDispatchDuration(0);
		const auto invalid = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(500));
		CHECK(GraphTrip(invalid, train.index).prediction.visits[1].arrival == NOW + 10);
		CHECK_FALSE(GraphTrip(invalid, train.index).prediction.visits[1].departure);
		CHECK(GraphTrip(invalid, train.index).prediction.status == PassengerPredictionStatus::Unknown);
	}
	SECTION("Copying dispatch slots is charged before allocation") {
		for (uint32_t i = 0; i < 1000; ++i) schedule.GetScheduledDispatchMutable().push_back({ i, 0, {} });
		auto limits = GraphLimits(500);
		limits.work = 500;
		const auto limited = PassengerServiceGraph::Build(PAX, NOW, limits);
		CHECK(limited.GetStatus() == PassengerPredictionStatus::BudgetExhausted);
		CHECK(limited.GetTrips().empty());
	}
	CHECK(seeds.Check());
}

TEST_CASE("Passenger graph distinguishes inactive trains and exhausted budgets from no service", "[passenger][service_graph]")
{
	PassengerTestStations stations;
	GraphVehicle train({ GraphStop(A), GraphStop(B) });
	SECTION("Stopped or crashed trains have no usable departures") {
		for (auto flag : { VehState::Stopped, VehState::Crashed }) {
			train.vehstatus = { flag };
			const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
			CHECK(GraphTrip(graph, train.index).prediction.visits.empty());
			CHECK(graph.GetBoardableTrips(A, NOW, NOW + 1000).items.empty());
		}
	}
	SECTION("Mismatched current/implicit orders remain unknown") {
		train.cur_implicit_order_index = 1;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		CHECK(GraphTrip(graph, train.index).operation == PassengerServiceGraph::Operation::Unknown);
		CHECK(graph.GetBoardableTrips(A, NOW, NOW + 1000).status == PassengerPredictionStatus::Unknown);
	}
	SECTION("Cold discovery budget") {
		auto limits = GraphLimits();
		limits.work = 0;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		CHECK(graph.GetStatus() == PassengerPredictionStatus::BudgetExhausted);
		CHECK(graph.GetBoardableTrips(A, NOW, NOW + 1000).status == PassengerPredictionStatus::BudgetExhausted);
	}
	SECTION("Trip limit") {
		auto limits = GraphLimits();
		limits.trips = 0;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		CHECK(graph.GetStatus() == PassengerPredictionStatus::LimitReached);
	}
	SECTION("Prediction and query limits") {
		auto limits = GraphLimits();
		limits.visits_per_trip = 1;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		CHECK(GraphTrip(graph, train.index).prediction.status == PassengerPredictionStatus::LimitReached);
		CHECK(graph.GetVisits(A, NOW, NOW + 1000, 0).status == PassengerPredictionStatus::BudgetExhausted);
		CHECK(graph.GetVisits(A, NOW - 1, NOW + 1000).status == PassengerPredictionStatus::Unknown);
		CHECK(graph.GetVisits(A, NOW, NOW + 1001).status == PassengerPredictionStatus::Unknown);
	}
}

TEST_CASE("Passenger graph reports unfinished boarding candidates at the exact budget boundary", "[passenger][service_graph]")
{
	PassengerTestStations stations;
	GraphVehicle first({ GraphStop(A) });
	GraphVehicle second(first, VehicleID{101});
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(50));
	REQUIRE(graph.GetTrips().size() == 2);
	for (const auto &trip : graph.GetTrips()) {
		REQUIRE(trip.prediction.status == PassengerPredictionStatus::Complete);
		REQUIRE(trip.prediction.visits.size() == 1); // No downstream visits consume extra work.
	}
	/* Selection costs one service, two trips and two visits. Each candidate costs one more. */
	SECTION("Only the first candidate fits") {
		const auto result = graph.GetBoardableTrips(A, NOW, NOW + 50, 6);
		REQUIRE(result.items.size() == 1);
		CHECK(result.items[0].reachable.empty());
		CHECK(result.items[0].status == PassengerPredictionStatus::Complete);
		CHECK(result.status == PassengerPredictionStatus::BudgetExhausted);
	}
	SECTION("The last candidate uses the final unit of work") {
		const auto result = graph.GetBoardableTrips(A, NOW, NOW + 50, 7);
		CHECK(result.items.size() == 2);
		CHECK(result.status == PassengerPredictionStatus::Complete);
	}
	SECTION("Selection uses the final unit before any candidate can be processed") {
		const auto result = graph.GetBoardableTrips(A, NOW, NOW + 50, 5);
		CHECK(result.items.empty());
		CHECK(result.status == PassengerPredictionStatus::BudgetExhausted);
	}
}

TEST_CASE("Passenger graph certifies only the known time prefix of a truncated prediction", "[passenger][service_graph]")
{
	PassengerTestStations stations;
	GraphVehicle train({ GraphStop(A), GraphStop(B) });
	auto limits = GraphLimits();
	SECTION("Visit limit preserves complete earlier station windows") {
		limits.visits_per_trip = 3;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		const auto &prediction = GraphTrip(graph, train.index).prediction;
		CHECK(prediction.status == PassengerPredictionStatus::LimitReached);
		CHECK(prediction.start == NOW);
		CHECK(prediction.complete_until == NOW + 230);
		CHECK(prediction.Covers(NOW, NOW + 230));
		CHECK_FALSE(prediction.Covers(NOW + 230, NOW + 231));
		CHECK(graph.GetVisits(A, NOW, NOW + 230).status == PassengerPredictionStatus::Complete);
		CHECK(graph.GetVisits(A, NOW + 230, NOW + 231).status == PassengerPredictionStatus::LimitReached);
		const auto absent = graph.GetVisits(B, NOW, NOW + 100);
		CHECK(absent.items.empty());
		CHECK(absent.status == PassengerPredictionStatus::Complete);
		const auto boarding = graph.GetBoardableTrips(A, NOW, NOW + 200);
		REQUIRE(boarding.items.size() == 1);
		CHECK(boarding.status == PassengerPredictionStatus::Complete);
		CHECK(boarding.items[0].reachable.size() == 2);
		CHECK(boarding.items[0].status == PassengerPredictionStatus::LimitReached);
		CHECK(graph.GetBoardableTrips(A, NOW, NOW + 100, 6).status == PassengerPredictionStatus::BudgetExhausted);
		CHECK(graph.GetBoardableTrips(A, NOW, NOW + 100, 7).status == PassengerPredictionStatus::Complete);
		CHECK(graph.GetBoardableTrips(A, NOW, NOW + 1000).status == PassengerPredictionStatus::LimitReached);
	}
	SECTION("Step limit preserves the same prefix contract") {
		limits.steps_per_trip = 2;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		const auto &prediction = GraphTrip(graph, train.index).prediction;
		CHECK(prediction.status == PassengerPredictionStatus::LimitReached);
		CHECK(prediction.complete_until == NOW + 120);
		CHECK(graph.GetVisits(B, NOW, NOW + 120).status == PassengerPredictionStatus::Complete);
		CHECK(graph.GetVisits(B, NOW, NOW + 121).status == PassengerPredictionStatus::LimitReached);
	}
	SECTION("Zero-time occurrences at the cutoff remain uncertified") {
		for (Order *order : train.orders->Orders()) { order->SetWaitTime(0); order->SetTravelTime(0); }
		limits.visits_per_trip = 3;
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		const auto &prediction = GraphTrip(graph, train.index).prediction;
		CHECK(prediction.complete_until == NOW);
		CHECK_FALSE(prediction.Covers(NOW, NOW + 1));
		CHECK(graph.GetVisits(A, NOW, NOW + 1).status == PassengerPredictionStatus::LimitReached);
	}
	SECTION("Prediction work exhaustion preserves only the already processed prefix") {
		size_t budget = 1;
		const auto prediction = PredictPassengerVisits(*GetPassengerRouteState(&train), PAX, NOW, NOW + 1000, 128, 4096, budget);
		CHECK(budget == 0);
		CHECK(prediction.status == PassengerPredictionStatus::BudgetExhausted);
		CHECK(prediction.complete_until == NOW + 10);
		CHECK(prediction.Covers(NOW, NOW + 10));
		CHECK_FALSE(prediction.Covers(NOW, NOW + 11));
	}
	SECTION("Full horizon coverage includes empty intervals between events") {
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, limits);
		const auto &prediction = GraphTrip(graph, train.index).prediction;
		CHECK(prediction.status == PassengerPredictionStatus::Complete);
		CHECK(prediction.complete_until == graph.GetEnd());
		CHECK(prediction.Covers(graph.GetStart(), graph.GetEnd()));
	}
}

TEST_CASE("Passenger graph keeps uncertainty beyond the certified prefix out of early queries", "[passenger][service_graph]")
{
	PassengerTestStations stations;
	GraphVehicle train({ GraphStop(A), GraphStop(B), GraphStop(D) });
	SECTION("Unknown dwell and later unknown visits do not fall inside earlier windows") {
		train.orders->GetOrderAt(1)->SetWaitTimetabled(false);
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		const auto &prediction = GraphTrip(graph, train.index).prediction;
		CHECK(prediction.status == PassengerPredictionStatus::Unknown);
		CHECK(prediction.complete_until == NOW + 110);
		for (StationID station : { B, D }) {
			const auto early = graph.GetVisits(station, NOW, NOW + 110);
			CHECK(early.items.empty());
			CHECK(early.status == PassengerPredictionStatus::Complete);
			CHECK(graph.GetVisits(station, NOW, NOW + 111).status == PassengerPredictionStatus::Unknown);
		}
		const auto boarding = graph.GetBoardableTrips(A, NOW, NOW + 110);
		REQUIRE(boarding.items.size() == 1);
		CHECK(boarding.status == PassengerPredictionStatus::Complete);
		CHECK(boarding.items[0].status == PassengerPredictionStatus::Unknown);
	}
	SECTION("Dynamic branches stop coverage at the last known departure") {
		Order *branch = train.orders->GetOrderAt(1);
		branch->MakeConditional(2);
		branch->SetConditionVariable(OrderConditionVariable::LoadPercentage);
		const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
		CHECK(GraphTrip(graph, train.index).prediction.complete_until == NOW + 10);
		CHECK(graph.GetVisits(D, NOW, NOW + 10).status == PassengerPredictionStatus::Complete);
		CHECK(graph.GetVisits(D, NOW, NOW + 11).status == PassengerPredictionStatus::Unknown);
	}
	SECTION("An unresolved dispatch does not certify its nominal dwell") {
		Order *order = train.orders->GetOrderAt(1);
		order->SetWaitTime(50);
		order->SetDispatchScheduleIndex(0);
		train.vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
		train.orders->GetScheduledDispatchScheduleSet().resize(1);
		auto &schedule = train.orders->GetDispatchScheduleByIndex(0);
		schedule.SetScheduledDispatchStartTick(NOW);
		schedule.SetScheduledDispatchDuration(1000);
		schedule.GetScheduledDispatchMutable() = { { 200, 0, {} }, { 400, 0, {} } };
		size_t budget = 2; // Orders fit; the dispatch slot search does not.
		const auto prediction = PredictPassengerVisits(*GetPassengerRouteState(&train), PAX, NOW, NOW + 1000, 128, 4096, budget);
		CHECK(prediction.status == PassengerPredictionStatus::BudgetExhausted);
		CHECK(prediction.complete_until == NOW + 110);
		CHECK_FALSE(prediction.visits.back().departure);
	}
}

TEST_CASE("Passenger graph station queries retain occurrence order rather than departure order", "[passenger][service_graph]")
{
	PassengerTestStations stations;
	GraphVehicle first({ GraphStop(A, 100, 20) });
	GraphVehicle second(first, VehicleID{101});
	first.lateness_counter = -100;
	const auto graph = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(50));
	const auto visits = graph.GetVisits(A, NOW, NOW + 50);
	REQUIRE(visits.items.size() == 1); // The first trip's later departure lies outside this window.
	CHECK(visits.items[0].trip == 1);
	const auto extended = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(150));
	const auto all = extended.GetVisits(A, NOW, NOW + 150);
	REQUIRE(all.items.size() == 3);
	CHECK(all.items[0] == PassengerServiceGraph::VisitID{0, 0});
	CHECK(all.items[1] == PassengerServiceGraph::VisitID{1, 0});
	CHECK(all.items[2] == PassengerServiceGraph::VisitID{1, 1});
	CHECK(*extended.GetTrips()[0].prediction.visits[0].departure > *extended.GetTrips()[1].prediction.visits[0].departure);
	const auto boarding = extended.GetBoardableTrips(A, NOW, NOW + 150);
	REQUIRE(boarding.items.size() == all.items.size());
	for (size_t i = 0; i < all.items.size(); ++i) CHECK(boarding.items[i].boarding == all.items[i]);
}

TEST_CASE("Passenger graph construction and queries are deterministic and leave live state unchanged", "[passenger][service_graph]")
{
	PassengerTestStations stations;
	GraphVehicle train({ GraphStop(A), GraphStop(B), GraphStop(D) });
	GraphVehicle second(train, VehicleID{101});
	second.current_order_time = 3;
	second.lateness_counter = -15;
	const auto before_order = train.current_order;
	const auto before_orders = train.orders->GetOrderVector();
	const auto before_vehicles = Vehicle::GetNumItems();
	const auto before_lists = OrderList::GetNumItems();
	GameRandomSeedChecker seeds;
	const auto first = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
	const auto other = PassengerServiceGraph::Build(PAX, NOW, GraphLimits());
	REQUIRE(first.GetTrips().size() == other.GetTrips().size());
	for (size_t i = 0; i < first.GetTrips().size(); ++i) {
		const auto &a = first.GetTrips()[i];
		const auto &b = other.GetTrips()[i];
		CHECK(a.vehicle == b.vehicle);
		CHECK(a.service == b.service);
		CHECK(a.prediction.status == b.prediction.status);
		CHECK(a.prediction.start == b.prediction.start);
		CHECK(a.prediction.complete_until == b.prediction.complete_until);
		REQUIRE(a.prediction.visits.size() == b.prediction.visits.size());
		for (size_t j = 0; j < a.prediction.visits.size(); ++j) {
			CHECK(a.prediction.visits[j].order == b.prediction.visits[j].order);
			CHECK(a.prediction.visits[j].arrival == b.prediction.visits[j].arrival);
			CHECK(a.prediction.visits[j].departure == b.prediction.visits[j].departure);
		}
	}
	CHECK(first.GetVisits(B, NOW, NOW + 1000).items == other.GetVisits(B, NOW, NOW + 1000).items);
	CHECK(first.GetBoardableTrips(A, NOW, NOW + 1000).items.size() == other.GetBoardableTrips(A, NOW, NOW + 1000).items.size());
	CHECK(train.current_order.Equals(before_order));
	CHECK(train.orders->GetNumVehicles() == 2);
	CHECK(train.NextShared() == &second);
	CHECK(second.orders == train.orders);
	CHECK(second.current_order_time == 3);
	CHECK(second.lateness_counter == -15);
	for (size_t i = 0; i < before_orders.size(); ++i) {
		CHECK(train.GetOrder(i)->Equals(before_orders[i]));
		CHECK(train.GetOrder(i)->GetWaitTime() == before_orders[i].GetWaitTime());
		CHECK(train.GetOrder(i)->GetTravelTime() == before_orders[i].GetTravelTime());
	}
	CHECK(Vehicle::GetNumItems() == before_vehicles);
	CHECK(OrderList::GetNumItems() == before_lists);
	CHECK(seeds.Check());
}

TEST_CASE("Service pattern capture isolates timing policies and agrees with physical trips", "[passenger][service_graph][timetable]")
{
	PassengerTestStations stations;
	GraphVehicle train({GraphStop(A, 100, 100), GraphStop(B, 200, 100), GraphStop(D, 300, 100)});
	train.GetOrder(0)->SetLeaveType(OLT_LEAVE_EARLY);
	train.GetOrder(1)->SetLoadType(OrderLoadType::FullLoadAny);
	PassengerServiceCapture capture;
	const auto conservative = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(1000), {}, &capture);
	CHECK_FALSE(GraphTrip(conservative, train.index).prediction.visits[0].departure);
	const auto assumed = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(1000), {}, &capture, PassengerTimingPolicy::AssumeTimetable);
	const auto &prediction = GraphTrip(assumed, train.index).prediction;
	REQUIRE(prediction.visits.size() >= 3);
	CHECK(prediction.visits[0].departure == NOW + 100);
	CHECK(prediction.visits[1].arrival == NOW + 300);
	CHECK(prediction.visits[1].departure == NOW + 400);
	const auto *pattern = assumed.GetService(train.orders->index);
	REQUIRE(pattern->segments.size() >= 2);
	CHECK(pattern->cycle_known);
	CHECK(pattern->segments[0].travel == StateTicksDelta{200});
	const auto scoped = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(1000), PassengerServiceScope{A, D}, &capture, PassengerTimingPolicy::AssumeTimetable);
	CHECK(GraphTrip(scoped, train.index).prediction.visits[1].departure == prediction.visits[1].departure);
	const auto conservative_again = PassengerServiceGraph::Build(PAX, NOW, GraphLimits(1000), {}, &capture);
	CHECK_FALSE(GraphTrip(conservative_again, train.index).prediction.visits[0].departure);
	CHECK(train.GetOrder(0)->GetLeaveType() == OLT_LEAVE_EARLY);
	CHECK(train.GetOrder(1)->GetLoadType() == OrderLoadType::FullLoadAny);
	CHECK(train.current_order_time == 0);
}
