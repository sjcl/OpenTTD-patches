/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
/** @file passenger_service_graph_index.cpp Paired legacy/contiguous index regression and benchmark. */
#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../passenger_service_graph.h"
#include "../passenger_journey_planner.h"
#include "../passenger_journey_active.h"
#include "../vehicle_base.h"
#include "../sl/saveload.h"
#include "../sl/extended_ver_sl.h"
#include "passenger_test_stations.h"
#include "passenger_index_allocations.h"
#include <chrono>
#include <deque>
#include <iostream>

using Graph = PassengerServiceGraph;
using Status = PassengerPredictionStatus;
using VisitID = Graph::VisitID;
static constexpr StateTicks INDEX_NOW{10000};
extern SaveLoadVersion _sl_version;
extern NamedSaveLoadTable GetOrderDescription();
extern std::vector<uint8_t> SlTestSaveLoadTable(void *, void *, const NamedSaveLoadTable &);

static Status MergeIndexStatus(Status a, Status b) { return static_cast<Status>(std::max(to_underlying(a), to_underlying(b))); }

struct IndexStageSample { uint64_t ns = 0; size_t calls = 0, bytes = 0; };
static std::array<IndexStageSample, 6> _index_samples;
static std::chrono::steady_clock::time_point _index_stage_start;
static PassengerIndexAllocations _index_stage_allocations;

static void ObserveIndexStage(size_t stage, bool begin)
{
	if (begin) {
		_index_stage_start = std::chrono::steady_clock::now();
		_index_stage_allocations = _passenger_index_allocations;
	} else {
		_index_samples[stage].ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _index_stage_start).count();
		_index_samples[stage].calls += _passenger_index_allocations.calls - _index_stage_allocations.calls;
		_index_samples[stage].bytes += _passenger_index_allocations.bytes - _index_stage_allocations.bytes;
	}
}

/** The map/vector algorithm at eab176fa0a, split only at independent measurement boundaries. */
struct LegacyPassengerIndex {
	struct Departures { std::vector<VisitID> timed, unknown; Status status = Status::Complete; };
	struct Component { std::vector<StationID> stations; Status status = Status::Complete; };
	std::map<StationID, std::vector<VisitID>> visits;
	std::map<StationID, Departures> departures;
	std::map<VehicleID, std::optional<VisitID>> loading;
	std::map<StationID, StationID> roots;
	std::map<StationID, Component> components;
	std::vector<StationID> uncertain;
	Status uncertain_status = Status::Complete;

	void Build(const Graph &graph, bool enabled, bool measure = false)
	{
		auto observe = [&](size_t stage, bool begin) { if (measure) ObserveIndexStage(stage, begin); };
		observe(0, true);
		for (size_t t = 0; t < graph.GetTrips().size(); ++t) {
			for (size_t v = 0; v < graph.GetTrips()[t].prediction.visits.size(); ++v) visits[graph.GetVisitStop({t, v}).station].push_back({t, v});
		}
		observe(0, false);
		if (!enabled) return;
		observe(4, true);
		std::map<StationID, size_t> ranks;
		auto root = [&](StationID station) {
			StationID id = station;
			while (roots.at(id) != id) id = roots.at(id);
			while (station != id) { StationID parent = roots.at(station); roots.at(station) = id; station = parent; }
			return id;
		};
		for (const auto &service : graph.GetServices()) {
			for (const auto &stop : service.stops) { roots.try_emplace(stop.station, stop.station); ranks.try_emplace(stop.station, 0); }
			if (service.stops.empty()) continue;
			StationID anchor = service.stops.front().station;
			roots.try_emplace(anchor, anchor); ranks.try_emplace(anchor, 0);
			for (const auto &stop : service.stops) {
				StationID a = root(anchor), b = root(stop.station);
				if (a == b) continue;
				if (ranks[a] < ranks[b]) std::swap(a, b);
				roots[b] = a;
				if (ranks[a] == ranks[b]) ++ranks[a];
			}
		}
		for (auto &[station, parent] : roots) parent = root(station);
		observe(4, false);
		observe(1, true);
		for (const auto &[station, parent] : roots) {
			auto &index = departures[station];
			for (OrderListID service : graph.GetServicesAtStation(station)) index.status = MergeIndexStatus(index.status, graph.GetService(service)->coverage_status);
		}
		auto sort = [&](Departures &index) {
			std::sort(index.timed.begin(), index.timed.end(), [&](VisitID a, VisitID b) {
				const auto &ta = graph.GetTrips()[a.trip], &tb = graph.GetTrips()[b.trip];
				return std::tuple(*ta.prediction.visits[a.visit].departure, ta.vehicle, a.trip, a.visit) < std::tuple(*tb.prediction.visits[b.visit].departure, tb.vehicle, b.trip, b.visit);
			});
		};
		for (const auto &[station, ids] : visits) {
			auto &index = departures[station];
			for (VisitID id : ids) {
				const auto &trip = graph.GetTrips()[id.trip];
				if (!measure && trip.prediction.visits[id.visit].loading_now) {
					auto [found, inserted] = loading.try_emplace(trip.vehicle, id);
					if (!inserted) found->second.reset();
				}
				if (trip.operation != Graph::Operation::Running || !graph.GetVisitStop(id).can_board || trip.capacity == 0) continue;
				(trip.prediction.visits[id.visit].departure ? index.timed : index.unknown).push_back(id);
			}
			if (!measure) sort(index); // Exact original interleaving for uninstrumented totals.
		}
		observe(1, false);
		observe(2, true);
		if (measure) for (auto &[station, index] : departures) sort(index);
		observe(2, false);
		observe(3, true);
		if (measure) for (const auto &[station, ids] : visits) {
			for (VisitID id : ids) {
				const auto &trip = graph.GetTrips()[id.trip];
				if (!trip.prediction.visits[id.visit].loading_now) continue;
				auto [found, inserted] = loading.try_emplace(trip.vehicle, id);
				if (!inserted) found->second.reset();
			}
		}
		observe(3, false);
		observe(5, true);
		for (const auto &[station, index] : departures) {
			auto status = index.unknown.empty() ? index.status : MergeIndexStatus(index.status, Status::Unknown);
			if (status == Status::Complete) continue;
			uncertain.push_back(station);
			uncertain_status = MergeIndexStatus(uncertain_status, status);
			auto &component = components[roots.at(station)];
			component.stations.push_back(station);
			component.status = MergeIndexStatus(component.status, status);
		}
		observe(5, false);
	}

	Graph::Departures GetDepartures(const Graph &graph, StationID station, StateTicks from, StateTicks to, bool scoped = false) const;
};

/** Access is confined to this test translation unit; no alternate production query path. */
struct PassengerServiceGraphTestAccess {
	static bool Enabled(const Graph &graph) { return graph.status < Status::LimitReached; }
	static Status CaptureStatus(const Graph &graph) { return graph.status; }
	static Graph Capture(const Graph &source)
	{
		Graph graph;
		graph.cargo = source.cargo; graph.start = source.start; graph.end = source.end;
		graph.status = source.status; graph.work = source.work; graph.memory_bound = source.memory_bound;
		graph.scope = source.scope; graph.scoped_omissions = source.scoped_omissions;
		graph.diagnostic = source.diagnostic;
		graph.services = source.services; graph.trips = source.trips;
		graph.service_index = source.service_index; graph.station_services = source.station_services;
		return graph;
	}
	static void Build(Graph &graph, bool measure = false)
	{
		graph.BuildDepartureIndex(Enabled(graph), measure ? [](Graph::IndexStage stage, bool begin) { ObserveIndexStage(static_cast<size_t>(stage), begin); } : nullptr);
	}
	static Graph Reference(const Graph &source, const LegacyPassengerIndex &legacy)
	{
		Graph graph = Capture(source);
		for (const auto &[station, services] : graph.station_services) {
			Graph::StationIndex index{station};
			index.visits.begin = graph.station_visits.size();
			auto visits = legacy.visits.find(station);
			if (visits != legacy.visits.end()) { index.visits.count = visits->second.size(); graph.station_visits.insert(graph.station_visits.end(), visits->second.begin(), visits->second.end()); }
			index.timed.begin = graph.timed_departures.size(); index.unknown.begin = graph.unknown_departures.size();
			auto departures = legacy.departures.find(station);
			if (departures != legacy.departures.end()) {
				const auto &old = departures->second;
				index.status = old.status; index.timed.count = old.timed.size(); index.unknown.count = old.unknown.size();
				graph.timed_departures.insert(graph.timed_departures.end(), old.timed.begin(), old.timed.end());
				graph.unknown_departures.insert(graph.unknown_departures.end(), old.unknown.begin(), old.unknown.end());
			}
			graph.station_index.push_back(index);
		}
		graph.loading_visits.assign(legacy.loading.begin(), legacy.loading.end());
		graph.uncertain_stations = legacy.uncertain; graph.uncertain_status = legacy.uncertain_status;
		if (!legacy.roots.empty()) {
			graph.uncertain_components.resize(graph.station_index.size());
			for (auto &index : graph.station_index) {
				index.component = graph.FindStationIndex(legacy.roots.at(index.station)) - graph.station_index.data();
			}
			for (const auto &[station, old] : legacy.components) {
				auto &component = graph.uncertain_components[graph.FindStationIndex(station) - graph.station_index.data()];
				component.status = old.status; component.stations = {graph.component_stations.size(), old.stations.size()};
				graph.component_stations.insert(graph.component_stations.end(), old.stations.begin(), old.stations.end());
			}
		}
		return graph;
	}
	static Graph Synthetic(size_t station_count, size_t trip_count, size_t visits_per_trip, bool hub = false, bool sparse = false)
	{
		Graph graph;
		graph.cargo = CargoType{0}; graph.start = INDEX_NOW; graph.end = INDEX_NOW + 1000000;
		graph.services.reserve(trip_count); graph.trips.reserve(trip_count);
		for (size_t t = 0; t < trip_count; ++t) {
			Graph::Service service{}; service.id = OrderListID{static_cast<uint16_t>(t)};
			auto pattern = std::make_shared<Graph::ServicePattern>();
			pattern->id = service.id;
			service.coverage_status = t % 11 == 0 ? Status::Unknown : Status::Complete;
			service.trips.push_back(t);
			for (size_t v = 0; v < 8; ++v) {
				const size_t raw = hub ? (v % 2 == 0 ? 0 : 1 + t % (station_count - 1)) : (t * 8 + v) % station_count;
				const StationID station{static_cast<uint16_t>(sparse ? 63000 + raw : raw)};
				pattern->stops.push_back({station, static_cast<VehicleOrderID>(v), v % 5 != 3, v % 5 != 2, v == 6, {}, -1});
				auto &ids = graph.station_services[station];
				if (ids.empty() || ids.back() != service.id) ids.push_back(service.id);
			}
			service.pattern = pattern;
			service.stops = pattern->stops;
			Graph::Trip trip{}; trip.service = service.id; trip.vehicle = VehicleID{static_cast<uint32_t>(trip_count - t)};
			if (t != 0 && t % 41 == 0) trip.vehicle = graph.trips.back().vehicle; // Also exercise vehicle ties resolved by Trip/Visit.
			trip.operation = t % 29 == 0 ? Graph::Operation::Stopped : Graph::Operation::Running;
			trip.capacity = t % 31 == 0 ? 0 : 80; trip.loading = true; trip.current_order = 0;
			trip.prediction.start = INDEX_NOW; trip.prediction.complete_until = service.coverage_status == Status::Complete ? graph.end : INDEX_NOW + 500;
			trip.prediction.status = service.coverage_status;
			trip.prediction.visits.reserve(visits_per_trip);
			for (size_t v = 0; v < visits_per_trip; ++v) {
				const auto tick = INDEX_NOW + (hub ? 100 : static_cast<int64_t>((t * 17 + v * 19) % 1000));
				trip.prediction.visits.push_back({static_cast<VehicleOrderID>(v % 8), tick - 1, t % 11 == 0 && v > 4 ? std::optional<StateTicks>{} : tick, v == 0, {}});
			}
			graph.service_index.emplace(service.id, t); graph.services.push_back(std::move(service)); graph.trips.push_back(std::move(trip));
		}
		return graph;
	}
	static void Ambiguous(Graph &graph) { graph.trips[1].prediction.visits[1].loading_now = true; graph.trips[1].prediction.visits[2].loading_now = true; }
	static size_t StationStorage(const Graph &graph) { return graph.station_index.capacity(); }
};

Graph::Departures LegacyPassengerIndex::GetDepartures(const Graph &graph, StationID station, StateTicks from, StateTicks to, bool scoped) const
{
	Graph::Departures result{{}, {}, scoped ? PassengerServiceGraphTestAccess::CaptureStatus(graph) : graph.GetStatus()};
	if (from < graph.GetStart() || to > graph.GetEnd() || from >= to) { result.status = MergeIndexStatus(result.status, Status::Unknown); return result; }
	auto it = departures.find(station);
	if (it == departures.end()) return result;
	const auto &index = it->second;
	result.status = MergeIndexStatus(result.status, index.status); result.unknown = index.unknown;
	if (!index.unknown.empty()) result.status = MergeIndexStatus(result.status, Status::Unknown);
	auto lower = [&](StateTicks time) { return std::lower_bound(index.timed.begin(), index.timed.end(), time, [&](VisitID id, StateTicks tick) { return *graph.GetTrips()[id.trip].prediction.visits[id.visit].departure < tick; }); };
	result.timed = std::span<const VisitID>(lower(from), lower(to));
	return result;
}

template <typename T> static bool SameIndexSpan(std::span<const T> a, std::span<const T> b) { return std::equal(a.begin(), a.end(), b.begin(), b.end()); }

static void CheckIndexPair(const Graph &graph)
{
	LegacyPassengerIndex legacy; legacy.Build(graph, PassengerServiceGraphTestAccess::Enabled(graph));
	const Graph reference = PassengerServiceGraphTestAccess::Reference(graph, legacy);
	CHECK(SameIndexSpan(graph.GetUncertainStations(), reference.GetUncertainStations()));
	CHECK(graph.GetUncertainStatus() == reference.GetUncertainStatus());
	CHECK(graph.GetWork() == reference.GetWork());
	CHECK(graph.GetMemoryBound() == reference.GetMemoryBound());
	std::set<StationID> checked;
	for (const auto &service : graph.GetServices()) {
		for (const auto &stop : service.stops) {
			const StationID station = stop.station;
			if (!checked.insert(station).second) continue;
			CHECK(SameIndexSpan(graph.GetStationVisits(station), reference.GetStationVisits(station)));
			CHECK(SameIndexSpan(graph.GetUncertainStations(station), reference.GetUncertainStations(station)));
			CHECK(graph.GetUncertainStatus(station) == reference.GetUncertainStatus(station));
			CHECK(graph.GetCoverage(station, station) == reference.GetCoverage(station, station));
			for (size_t budget : {size_t{0}, size_t{1}, size_t{7}, size_t{128}}) {
				const auto a = graph.GetVisits(station, graph.GetStart(), graph.GetEnd(), budget), b = reference.GetVisits(station, graph.GetStart(), graph.GetEnd(), budget);
				CHECK(a.items == b.items); CHECK(a.status == b.status);
				const auto x = graph.GetBoardableTrips(station, graph.GetStart(), graph.GetEnd(), budget), y = reference.GetBoardableTrips(station, graph.GetStart(), graph.GetEnd(), budget);
				REQUIRE(x.items.size() == y.items.size()); CHECK(x.status == y.status);
				for (size_t i = 0; i < x.items.size(); ++i) {
					CHECK(x.items[i].boarding == y.items[i].boarding); CHECK(x.items[i].reachable == y.items[i].reachable); CHECK(x.items[i].status == y.items[i].status);
				}
			}
			for (auto [from, to] : {std::pair(graph.GetStart(), graph.GetEnd()), std::pair(INDEX_NOW + 100, INDEX_NOW + 101), std::pair(INDEX_NOW, INDEX_NOW + 100), std::pair(INDEX_NOW - 1, INDEX_NOW + 1), std::pair(INDEX_NOW, INDEX_NOW)}) {
				for (bool scoped : {false, true}) {
					const auto a = graph.GetDepartures(station, from, to, scoped), b = legacy.GetDepartures(graph, station, from, to, scoped);
					CHECK(SameIndexSpan(a.timed, b.timed)); CHECK(SameIndexSpan(a.unknown, b.unknown)); CHECK(a.status == b.status);
				}
			}
		}
	}
	for (const auto &trip : graph.GetTrips()) {
		for (const auto &visit : trip.prediction.visits) {
			if (!visit.loading_now) continue;
			const StationID station = graph.GetVisitStop({static_cast<size_t>(&trip - graph.GetTrips().data()), static_cast<size_t>(&visit - trip.prediction.visits.data())}).station;
			CHECK(graph.FindOnboardVisit(trip.vehicle, station, visit.order) == reference.FindOnboardVisit(trip.vehicle, station, visit.order));
		}
	}
	CHECK(graph.GetStationVisits(StationID::Invalid()).empty());
}

TEST_CASE("Contiguous service indices match all legacy station and departure sequences", "[passenger][service_graph][index]")
{
	Graph capture;
	SECTION("Empty snapshots") { capture = PassengerServiceGraphTestAccess::Synthetic(0, 0, 0); }
	SECTION("Sparse IDs and ambiguous current visits") { capture = PassengerServiceGraphTestAccess::Synthetic(4, 30, 32, false, true); PassengerServiceGraphTestAccess::Ambiguous(capture); }
	SECTION("Thousands of tied hub departures and repeated circuits") { capture = PassengerServiceGraphTestAccess::Synthetic(100, 3000, 48, true); }
	SECTION("Eight thousand stations and 120000 visits in independent components") { capture = PassengerServiceGraphTestAccess::Synthetic(8000, 3000, 40); }
	PassengerServiceGraphTestAccess::Build(capture);
	CheckIndexPair(capture);
	if (!capture.GetServices().empty() && capture.GetServices()[0].stops[0].station.base() >= 63000) CHECK(PassengerServiceGraphTestAccess::StationStorage(capture) == 4);
}

static void CheckIndexJourneys(const Graph &graph, StationID origin, StationID destination)
{
	LegacyPassengerIndex legacy; legacy.Build(graph, PassengerServiceGraphTestAccess::Enabled(graph));
	const auto reference = PassengerServiceGraphTestAccess::Reference(graph, legacy);
	PassengerJourneyPlanner before(reference, nullptr, {0, 0}), after(graph, nullptr, {0, 0});
	for (size_t work : {size_t{0}, size_t{1}, size_t{10}, size_t{100}, size_t{65536}}) {
		PassengerJourneyLimits limits; limits.work = work;
		const auto a = before.Plan(CargoType{0}, origin, destination, INDEX_NOW, limits), b = after.Plan(CargoType{0}, origin, destination, INDEX_NOW, limits);
		CHECK(a.journeys == b.journeys); CHECK(a.status == b.status); CHECK(a.stop_reason == b.stop_reason);
		CHECK(a.diagnostic.work == b.diagnostic.work); CHECK(a.diagnostic.reverse_work == b.diagnostic.reverse_work);
		CHECK(a.diagnostic.extensions == b.diagnostic.extensions); CHECK(a.diagnostic.labels == b.diagnostic.labels); CHECK(a.diagnostic.rounds == b.diagnostic.rounds);
	}
}

struct IndexVehicle : Vehicle {
	IndexVehicle(StationID a, StationID b, VehicleID id) : Vehicle(id, VehicleType::Train)
	{
		std::vector<Order> orders;
		for (StationID station : {a, b, a}) {
			Order order; order.MakeGoToStation(station); order.SetTravelTime(100); order.SetTravelTimetabled(true); order.SetWaitTime(10); order.SetWaitTimetabled(true); orders.push_back(order);
		}
		this->orders = OrderList::Create(std::move(orders), this); EnsurePassengerTestStations(this->orders->GetOrderVector());
		this->cargo_type = CargoType{0}; this->cargo_cap = 80; this->direction = Direction::NE;
		this->current_order.MakeLoading(false); this->last_station_visited = a; this->cur_timetable_order_index = INVALID_VEH_ORDER_ID;
	}
	~IndexVehicle() { this->orders->FreeChain(); this->orders = nullptr; }
	bool IsPrimaryVehicle() const override { return true; }
};

TEST_CASE("Live index capture preserves cold warm scoped and truncated query work", "[passenger][service_graph][index][budget][cache]")
{
	PassengerTestStations stations;
	std::deque<IndexVehicle> fleet;
	for (uint32_t t = 0; t < 4000; ++t) fleet.emplace_back(StationID{0}, StationID{static_cast<uint16_t>(1 + t % 100)}, VehicleID{10000 + t});
	SECTION("Unknown wait and load unload transfer flags") {
		fleet[0].GetOrder(1)->SetWaitTimetabled(false);
		fleet[1].GetOrder(0)->SetLoadType(OrderLoadType::NoLoad);
		fleet[2].GetOrder(1)->SetUnloadType(OrderUnloadType::NoUnload);
		fleet[3].GetOrder(1)->SetUnloadType(OrderUnloadType::Transfer);
	}
	PassengerServiceGraphLimits limits; limits.work = 1 << 24; limits.horizon = 1200; limits.memory_bytes = 32 * 1024 * 1024;
	SECTION("Work cutoff") { limits.work = 35000; }
	SECTION("Visit cutoff") { limits.total_visits = 100; }
	SECTION("Trip cutoff") { limits.trips = 10; }
	for (bool scoped : {false, true}) {
		InvalidatePassengerGraphServiceIndex();
		const auto scope = scoped ? std::optional(PassengerServiceScope{StationID{0}, StationID{1}}) : std::nullopt;
		const auto cold = Graph::Build(CargoType{0}, INDEX_NOW, limits, scope), warm = Graph::Build(CargoType{0}, INDEX_NOW, limits, scope);
		CHECK(cold.GetMemoryBound() <= limits.memory_bytes);
		if (!scoped) {
			CHECK(cold.GetStatus() == (limits.work == 35000 ? Status::BudgetExhausted : Status::LimitReached));
			const auto reason = limits.work == 35000 ? PassengerGraphFailure::Work : limits.total_visits == 100 ? PassengerGraphFailure::TotalVisits :
					limits.trips == 10 ? PassengerGraphFailure::Trips : PassengerGraphFailure::Memory;
			CHECK(cold.GetDiagnostic().first_failure.reason == reason);
		}
		CHECK(cold.GetWork() == warm.GetWork()); CHECK(cold.GetMemoryBound() == warm.GetMemoryBound()); CHECK(cold.GetStatus() == warm.GetStatus());
		CheckIndexPair(cold); CheckIndexPair(warm);
		CheckIndexJourneys(cold, StationID{0}, StationID{1}); CheckIndexJourneys(warm, StationID{0}, StationID{1});
	}
}

TEST_CASE("Order save tables and afterload recreate deterministic service indices", "[passenger][service_graph][index][saveload]")
{
	PassengerTestStations stations;
	std::deque<IndexVehicle> fleet;
	for (uint32_t t = 0; t < 64; ++t) fleet.emplace_back(StationID{static_cast<uint16_t>(t % 8 * 3)}, StationID{static_cast<uint16_t>(t % 8 * 3 + 1)}, VehicleID{20000 + t});
	fleet[0].GetOrder(1)->SetWaitTimetabled(false);
	fleet[1].GetOrder(0)->SetLoadType(OrderLoadType::NoLoad);
	fleet[2].GetOrder(1)->SetUnloadType(OrderUnloadType::NoUnload);
	fleet[3].GetOrder(1)->SetUnloadType(OrderUnloadType::Transfer);
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	PassengerServiceGraphLimits limits; limits.horizon = 2000;
	for (bool scoped : {false, true}) {
		const auto scope = scoped ? std::optional(PassengerServiceScope{StationID{0}, StationID{1}}) : std::nullopt;
		InvalidatePassengerGraphServiceIndex();
		const auto before = Graph::Build(CargoType{0}, INDEX_NOW, limits, scope);
		for (auto &vehicle : fleet) {
			for (VehicleOrderID i = 0; i < vehicle.GetNumOrders(); ++i) {
				Order loaded;
				REQUIRE_FALSE(SlTestSaveLoadTable(vehicle.GetOrder(i), &loaded, GetOrderDescription()).empty());
				*vehicle.GetOrder(i) = std::move(loaded);
			}
			Order loaded;
			REQUIRE_FALSE(SlTestSaveLoadTable(&vehicle.current_order, &loaded, GetOrderDescription()).empty());
			vehicle.current_order = std::move(loaded);
		}
		AfterLoadPassengerJourneys();
		const auto after = Graph::Build(CargoType{0}, INDEX_NOW, limits, scope);
		CHECK(before.GetWork() == after.GetWork()); CHECK(before.GetMemoryBound() == after.GetMemoryBound());
		CHECK(before.GetStatus() == after.GetStatus()); CHECK(before.GetCoverage(StationID{0}, StationID{1}) == after.GetCoverage(StationID{0}, StationID{1}));
		for (const auto &service : before.GetServices()) {
			for (const auto &stop : service.stops) {
				const StationID station = stop.station;
				CHECK(SameIndexSpan(before.GetStationVisits(station), after.GetStationVisits(station)));
				const auto a = before.GetDepartures(station, INDEX_NOW, before.GetEnd()), b = after.GetDepartures(station, INDEX_NOW, after.GetEnd());
				CHECK(SameIndexSpan(a.timed, b.timed)); CHECK(SameIndexSpan(a.unknown, b.unknown)); CHECK(a.status == b.status);
				CHECK(SameIndexSpan(before.GetUncertainStations(station), after.GetUncertainStations(station)));
			}
		}
		PassengerJourneyPlanner old_planner(before, nullptr, {0, 0}), new_planner(after, nullptr, {0, 0});
		const auto a = old_planner.Plan(CargoType{0}, StationID{0}, StationID{1}, INDEX_NOW), b = new_planner.Plan(CargoType{0}, StationID{0}, StationID{1}, INDEX_NOW);
		CHECK(a.journeys == b.journeys); CHECK(a.status == b.status); CHECK(a.diagnostic.work == b.diagnostic.work); CHECK(a.diagnostic.reverse_work == b.diagnostic.reverse_work);
		CheckIndexPair(after);
	}
}

TEST_CASE("Service index stage allocation comparison report", "[passenger][service_graph][index][.benchmark]")
{
#ifndef WITH_SERVICE_INDEX_ALLOCATIONS
	WARN("Allocation counters are unavailable: configure MSVC with OPTION_SERVICE_INDEX_ALLOCATIONS=ON to measure them.");
#endif
	static constexpr const char *names[] = {"station_visits", "station_departures", "departure_sort", "loading_visits", "components", "uncertain_components"};
	std::vector<uint64_t> eviction(16 * 1024 * 1024); // 128 MiB, larger than typical last-level caches.
	std::cout << "INDEX_CSV,fixture,cache,algorithm,run,stage,ns,allocations,allocated_bytes,retained_bytes,peak_bytes\n";
	for (bool hub : {false, true}) {
		_passenger_index_allocations = {}; _passenger_index_tracking = true;
		const auto source = PassengerServiceGraphTestAccess::Synthetic(8000, 3000, 40, hub);
		_passenger_index_tracking = false;
		const auto capture = _passenger_index_allocations;
		std::cout << "INDEX_CAPTURE_CSV," << (hub ? "hub" : "components") << ',' << capture.calls << ',' << capture.bytes << ',' << capture.live << ',' << capture.peak << '\n';
		for (bool cold : {true, false}) {
			for (size_t run = 0; run < 7; ++run) {
				for (bool old : {run % 2 == 0, run % 2 != 0}) {
					auto graph = PassengerServiceGraphTestAccess::Capture(source);
					LegacyPassengerIndex legacy;
					if (cold) { volatile uint64_t *memory = eviction.data(); for (size_t i = 0; i < eviction.size(); i += 8) memory[i] = memory[i] + run + 1; }
					_index_samples = {}; _passenger_index_allocations = {}; _passenger_index_tracking = true;
					const auto started = std::chrono::steady_clock::now();
					if (old) legacy.Build(graph, true, true); else PassengerServiceGraphTestAccess::Build(graph, true);
					const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
					_passenger_index_tracking = false;
					const auto allocations = _passenger_index_allocations;
					auto emit = [&](const char *stage, uint64_t ns, size_t calls, size_t bytes, size_t live, size_t peak) {
						std::cout << "INDEX_CSV," << (hub ? "hub" : "components") << ',' << (cold ? "cold" : "warm") << ',' << (old ? "legacy" : "flat") << ',' << run << ',' << stage << ',' << ns << ',' << calls << ',' << bytes << ',' << live << ',' << peak << '\n';
					};
					for (size_t stage = 0; stage < _index_samples.size(); ++stage) emit(names[stage], _index_samples[stage].ns, _index_samples[stage].calls, _index_samples[stage].bytes, 0, 0);
					emit("total", elapsed, allocations.calls, allocations.bytes, allocations.live, allocations.peak);
					{
						auto stage_graph = PassengerServiceGraphTestAccess::Capture(source);
						LegacyPassengerIndex stage_legacy;
						if (cold) { volatile uint64_t *memory = eviction.data(); for (size_t i = 0; i < eviction.size(); i += 8) memory[i] = memory[i] + run + 1; }
						_index_samples = {};
						if (old) stage_legacy.Build(stage_graph, true, true); else PassengerServiceGraphTestAccess::Build(stage_graph, true);
						for (size_t stage = 0; stage < _index_samples.size(); ++stage) emit((std::string(names[stage]) + "_untracked").c_str(), _index_samples[stage].ns, 0, 0, 0, 0);
					}
					/* Check the total without stage observer overhead and with the original
					 * interleaving of loading, departure insertion and per-station sorting. */
					{
						auto exact_graph = PassengerServiceGraphTestAccess::Capture(source);
						LegacyPassengerIndex exact_legacy;
						if (cold) { volatile uint64_t *memory = eviction.data(); for (size_t i = 0; i < eviction.size(); i += 8) memory[i] = memory[i] + run + 1; }
						_passenger_index_allocations = {}; _passenger_index_tracking = true;
						const auto exact_start = std::chrono::steady_clock::now();
						if (old) exact_legacy.Build(exact_graph, true); else PassengerServiceGraphTestAccess::Build(exact_graph);
						const auto exact_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - exact_start).count();
						_passenger_index_tracking = false;
						const auto exact = _passenger_index_allocations;
						emit("total_exact", exact_ns, exact.calls, exact.bytes, exact.live, exact.peak);
					}
					{
						auto plain_graph = PassengerServiceGraphTestAccess::Capture(source);
						LegacyPassengerIndex plain_legacy;
						if (cold) { volatile uint64_t *memory = eviction.data(); for (size_t i = 0; i < eviction.size(); i += 8) memory[i] = memory[i] + run + 1; }
						const auto plain_start = std::chrono::steady_clock::now();
						if (old) plain_legacy.Build(plain_graph, true); else PassengerServiceGraphTestAccess::Build(plain_graph);
						emit("total_untracked", std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - plain_start).count(), 0, 0, 0, 0);
					}
					if (cold) { volatile uint64_t *memory = eviction.data(); for (size_t i = 0; i < eviction.size(); i += 8) memory[i] = memory[i] + run + 1; }
					const auto queries_started = std::chrono::steady_clock::now();
					size_t items = 0;
					_passenger_index_allocations = {}; _passenger_index_tracking = true;
					for (uint32_t q = 0; q < 100000; ++q) {
						const StationID station{static_cast<uint16_t>(q * 7919 % 8000)};
						auto result = old ? legacy.GetDepartures(graph, station, INDEX_NOW + 100, INDEX_NOW + 101) : graph.GetDepartures(station, INDEX_NOW + 100, INDEX_NOW + 101);
						items += result.timed.size() + result.unknown.size() + to_underlying(result.status);
					}
					_passenger_index_tracking = false;
					emit("get_departures_100000", std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - queries_started).count(), _passenger_index_allocations.calls, _passenger_index_allocations.bytes, 0, 0);
					REQUIRE(items != 0);
				}
			}
		}
	}
	CHECK(eviction[0] != 0);
}
