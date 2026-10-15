/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../passenger_service_database.h"
#include "../passenger_journey_active.h"
#include "../vehicle_base.h"
#include "../station_base.h"
#include "../settings_type.h"
#include "../date_func.h"
#include "../order_func.h"
#include "../order_cmd.h"
#include "../vehicle_gui.h"
#include "../landscape_cmd.h"
#include "../command_func.h"
#include "../company_func.h"
#include "../clear_map.h"
#include "../core/backup_type.hpp"
#include "../core/random_func.hpp"
#include "../sl/saveload.h"
#include "passenger_test_stations.h"
#include "passenger_index_allocations.h"
#include <iostream>
#include <chrono>

extern SaveLoadVersion _sl_version;
extern NamedSaveLoadTable GetGoodsDesc();
extern NamedSaveLoadTable GetPassengerWaitingEventDesc();
extern std::vector<uint8_t> SlTestSaveLoadTable(void *, void *, const NamedSaveLoadTable &);
extern std::vector<uint8_t> SlTestSaveLoadTable(void *, void *, const NamedSaveLoadTable &, const NamedSaveLoadTable &);

namespace {
	constexpr CargoType PAX{0};
	constexpr StationID A{0}, B{1}, C{2}, D{3};
	constexpr StateTicks NOW{64000};
	Order Stop(StationID station, TimetableTicks travel = 100, TimetableTicks wait = 10)
	{
		Order order; order.MakeGoToStation(station);
		order.SetTravelTime(travel); order.SetTravelTimetabled(true);
		order.SetWaitTime(wait); order.SetWaitTimetabled(true);
		return order;
	}
	/** Produce the actual pre-estimate record layout for compatibility fixtures.
	 * Only certified services are used here; dropping estimates is never a
	 * production save operation. Counts are parsed instead of guessing offsets. */
	std::vector<uint8_t> LegacyDatabaseWire(const std::vector<uint8_t> &wire, uint8_t version = 8)
	{
		REQUIRE(wire[0] == 9); REQUIRE((version == 3 || version == 8));
		std::vector<uint8_t> old; size_t cursor = 0;
		auto value = [&]() { REQUIRE(cursor + 8 <= wire.size()); uint64_t v = 0; for (size_t i = 0; i < 8; ++i) v |= uint64_t{wire[cursor++]} << (8 * i); return v; };
		auto copy = [&]() { size_t start = cursor; auto v = value(); old.insert(old.end(), wire.begin() + start, wire.begin() + cursor); return v; };
		auto words = [&](size_t n) { while (n-- != 0) copy(); };
		auto service = [&]() {
			words(4); REQUIRE(value() == 0); // No estimated circuit in these fixtures.
			for (uint64_t n = copy(); n != 0; --n) { words(7); REQUIRE(value() == 1); REQUIRE(value() == 0); }
			for (uint64_t n = copy(); n != 0; --n) words(4 + NUM_CARGO);
			words(4); auto slots = copy(); words(static_cast<size_t>(slots)); copy();
		};
		copy(); old[0] = version; words(5);
		auto dirty = copy(); words(static_cast<size_t>(dirty) * 2);
		for (uint64_t n = copy(); n != 0; --n) service();
		words(4); auto pending = copy(); words(static_cast<size_t>(pending) * 2);
		auto unsafe = copy(); words(static_cast<size_t>(unsafe));
		for (uint64_t n = copy(); n != 0; --n) { service(); copy(); }
		if (version == 3) return old;
		words(5); for (uint64_t n = copy(); n != 0; --n) service();
		words(2); for (uint64_t n = copy(); n != 0; --n) service();
		words(2); value(); value(); REQUIRE(cursor == wire.size());
		return old;
	}
	struct PeriodicTrain : Vehicle {
		PeriodicTrain(VehicleID id, std::vector<Order> orders) : Vehicle(id, VehicleType::Train)
		{
			this->orders = OrderList::Create(std::move(orders), this);
			EnsurePassengerTestStations(this->orders->GetOrderVector());
			this->cargo_type = PAX; this->cargo_cap = 100; this->direction = Direction::NE;
			this->LoadAt(0);
		}
		PeriodicTrain(VehicleID id, PeriodicTrain *other) : Vehicle(id, VehicleType::Train)
		{
			this->orders = other->orders; this->AddToShared(other);
			this->cargo_type = PAX; this->cargo_cap = 100; this->direction = Direction::NE; this->LoadAt(0);
		}
		~PeriodicTrain() { for (Station *station : Station::Iterate()) std::erase(station->loading_vehicles, this); }
		bool IsPrimaryVehicle() const override { return true; }
		void LoadAt(VehicleOrderID index)
		{
			for (Station *station : Station::Iterate()) std::erase(station->loading_vehicles, this);
			this->cur_real_order_index = this->cur_implicit_order_index = index;
			this->cur_timetable_order_index = INVALID_VEH_ORDER_ID;
			this->last_station_visited = this->GetOrder(index)->GetDestination().ToStationID();
			this->current_order.MakeLoading(false); this->current_order_time = 0;
			Station::Get(this->last_station_visited)->loading_vehicles.push_back(this);
		}
	};
	struct Fixture {
		AutoRestoreBackup<LinkGraphSettings> settings{_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{}};
		AutoRestoreBackup<StateTicks> ticks{_state_ticks, NOW};
		AutoRestoreBackup<CargoClasses> classes{CargoSpec::Get(PAX)->classes, CargoClasses{CargoClass::Passengers}};
		AutoRestoreBackup<uint8_t> bitnum{CargoSpec::Get(PAX)->bitnum, uint8_t{0}};
		Fixture()
		{
			REQUIRE(Vehicle::GetNumItems() == 0); REQUIRE(Station::GetNumItems() == 0);
			AllocateMap(128, 128);
			_settings_game.linkgraph.passenger_journey_routing = _settings_game.linkgraph.passenger_journey_periodic = true;
			_settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Asymmetric;
			_settings_game.linkgraph.passenger_transfer_penalty = 0;
			ResetPassengerServiceDatabase(); BeginPassengerJourneyTick();
		}
		~Fixture()
		{
			_vehicle_pool.CleanPool(); _orderlist_pool.CleanPool(); _order_pool.CleanPool();
			_station_pool.CleanPool(); _cargopacket_pool.CleanPool(); ClearCargoPacketDeferredPayments();
			ResetPassengerServiceDatabase();
		}
		PeriodicTrain *Train(std::vector<Order> orders) { return Vehicle::Create<PeriodicTrain>(std::move(orders)); }
		PassengerServiceDatabase &Build() { auto &db = GetPassengerServiceDatabase(); for (int i = 0; i < 128 && db.IsBuilding(); ++i) db.Tick(); REQUIRE_FALSE(db.IsBuilding()); return db; }
		void PublishUpdates(PassengerServiceDatabase &db) { _state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; for (int i = 0; i < 128 && db.IsBuilding(); ++i) db.Tick(); REQUIRE_FALSE(db.IsBuilding()); }
		CargoPacket *Packet(uint16_t count = 10)
		{
			auto *packet = CargoPacket::Create(count, 0, A, TileIndex{0}, Money{0}); REQUIRE(packet->SetPassengerDestination(PAX, D)); return packet;
		}
	};
}

namespace {
	void SameDeparturePlan(const PassengerPeriodicPlan &a, const PassengerPeriodicPlan &b)
	{
		CHECK(a.ride == b.ride); CHECK(a.route == b.route); CHECK(a.arrival == b.arrival); CHECK(a.cost == b.cost);
		CHECK(a.transfers == b.transfers); CHECK(a.deferred == b.deferred); CHECK(a.reason == b.reason);
		CHECK(a.work == b.work); CHECK(a.memory == b.memory); CHECK(a.labels == b.labels); CHECK(a.candidates == b.candidates);
		CHECK(a.observations == b.observations);
		CHECK(a.state == b.state);
		CHECK(a.search.complete_labels == b.search.complete_labels); CHECK(a.search.queued_complete == b.search.queued_complete);
		CHECK(a.search.popped_complete == b.search.popped_complete); CHECK(a.search.best_complete_cost == b.search.best_complete_cost);
		CHECK(a.search.best_complete_arrival == b.search.best_complete_arrival); CHECK(a.search.work_stops == b.search.work_stops);
	}

	std::optional<StateTicks> ScannedDeparture(const PassengerPeriodicService &service, CargoType cargo, int64_t offset, StateTicks ready, std::span<const VehicleID> excluded)
	{
		std::optional<StateTicks> result;
		for (const auto &v : service.vehicles) {
			if (!v.phase_known || !v.capacity[cargo] || std::binary_search(excluded.begin(), excluded.end(), v.id)) continue;
			auto next = PassengerPeriodicTimetable::NextDeparture(v.base, offset, service.timetable.period, ready);
			if (next && (!result || *next < *result)) result = next;
		}
		return result;
	}
}

TEST_CASE("Incomplete timetables retain concrete estimated rides and indexed loading", "[passenger][precomputed][estimated][active][save]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	std::vector<Order> orders{Stop(A, 100, 10), Stop(B, 120, 20), Stop(D, 80, 10)};
	SECTION("Only certification flags are missing") { for (auto &order : orders) { order.SetTravelTimetabled(false); order.SetWaitTimetabled(false); } }
	SECTION("One section has no travel measurement") { orders[1].SetTravelTime(0); orders[1].SetTravelTimetabled(false); }
	SECTION("Only the return circuit is unknown") { orders[0].SetTravelTimetabled(false); }
	SECTION("Waypoint and regular depot retain continuity") {
		Order waypoint; waypoint.MakeGoToWaypoint(C); waypoint.SetTravelTime(30);
		Order depot; depot.MakeGoToDepot(DepotID{0}, {OrderDepotTypeFlag::PartOfOrders}); depot.SetTravelTime(20);
		orders.insert(orders.begin() + 1, waypoint); orders.insert(orders.begin() + 2, depot);
	}
	auto *train = f.Train(orders); auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	const auto *service = db.Get(train->orders->index); REQUIRE(service);
	CHECK(service->timetable.GetCertainty() == PassengerPeriodicTimetable::Certainty::Estimated);
	CHECK(service->timetable.period == 0); CHECK(service->timetable.estimated_period > 0);
	CHECK_FALSE(service->vehicles[0].phase_known); CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride);
	auto plan = db.PlanPrecomputed(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK(plan.estimated_service);
	CHECK(plan.ride->boarding_order == 0); CHECK(plan.ride->alighting_order == orders.size() - 1);
	CHECK(plan.ride->departure == StateTicks{}); CHECK(plan.ride->arrival == StateTicks{}); CHECK(plan.transfers == 0);
	const auto wire = db.Save(); PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true, true);
	while (peer.StepLoad(7) == PassengerServiceDatabase::LoadState::Replaying) {}
	REQUIRE(peer.GetLoadState() == PassengerServiceDatabase::LoadState::Ready);
	CHECK(peer.Save() == wire); CHECK(peer.PlanPrecomputed(PAX, A, D, NOW).ride == plan.ride);
	auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo; auto *packet = f.Packet(); waiting.Append(packet, D);
	waiting.RoutePassengerJourneys(PAX, A); REQUIRE(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
	const auto searches = db.GetDiagnostic().searches;
	PassengerJourneyLoadingContext loading; std::array<StationID, 1> next{D};
	CHECK(waiting.Load(10, &train->cargo, next, TileIndex{0}, train, &loading) == 10);
	CHECK(packet->GetPassengerJourney().kind == PassengerRideKind::EstimatedBoundService);
	CHECK(db.GetDiagnostic().searches == searches);
	train->LoadAt(static_cast<VehicleOrderID>(orders.size() - 1));
	PassengerJourneyActiveContext arrived(train, D);
	CHECK(arrived.Decide(*packet, PAX, true, OrderUnloadType::UnloadIfPossible) == VehicleCargoList::MoveToAction::Deliver);
}

TEST_CASE("Estimated patterns cut unsafe controls and preserve safe partial stretches", "[passenger][precomputed][estimated][orders]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	Order control; control.MakeConditional(0);
	SECTION("Conditional jump") {}
	SECTION("Refit depot") { control.MakeGoToDepot(DepotID{0}, {OrderDepotTypeFlag::PartOfOrders}); control.SetRefit(PAX); }
	SECTION("Halt depot") { control.MakeGoToDepot(DepotID{0}, {OrderDepotTypeFlag::PartOfOrders}); control.SetDepotActionType(ODATFB_HALT); }
	SECTION("Sell depot") { control.MakeGoToDepot(DepotID{0}, {OrderDepotTypeFlag::PartOfOrders}); control.SetDepotActionType(ODATFB_SELL); }
	auto *train = f.Train({Stop(A), Stop(B), control, Stop(C), Stop(D)});
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	const auto &t = db.Get(train->orders->index)->timetable;
	REQUIRE(t.stops.size() == 4); CHECK_FALSE(t.stops[1].continues);
	REQUIRE(db.PlanPrecomputed(PAX, A, B, NOW).ride); REQUIRE(db.PlanPrecomputed(PAX, C, D, NOW).ride);
	CHECK_FALSE(db.PlanPrecomputed(PAX, A, D, NOW).ride);
	CHECK_FALSE(db.PlanPrecomputed(PAX, B, C, NOW).ride);
	auto safe = db.PlanPrecomputed(PAX, C, D, NOW); REQUIRE(safe.ride); train->LoadAt(3);
	CHECK(ValidatePassengerServiceRide(*safe.ride, train, PAX, true));
	train->LoadAt(0); auto forged = *safe.ride; forged.boarding = A; forged.boarding_order = 0;
	CHECK_FALSE(ValidatePassengerServiceRide(forged, train, PAX, true));
}

TEST_CASE("Estimated durations preserve measured sections and use deterministic mode costs", "[passenger][precomputed][estimated][cost]")
{
	Fixture f;
	std::vector<Order> orders{Stop(A), Stop(B, 120), Stop(D, 80)};
	f.Train(orders);
	orders[0].SetTravelTimetabled(false);
	for (auto type : {VehicleType::Train, VehicleType::Road, VehicleType::Ship, VehicleType::Aircraft}) {
		auto table = PassengerPeriodicTimetable::CompileEstimated(orders, type);
		REQUIRE(table.stops.size() == 3); CHECK(table.period == 0); CHECK(table.estimated_period == 330);
		CHECK(table.stops[1].arrival - table.stops[0].departure == 120); CHECK(table.stops[1].travel_quality == 0);
		CHECK(table.stops[0].travel_quality == 1);
	}
	orders[1].SetTravelTime(0); orders[1].SetTravelTimetabled(false);
	auto train = PassengerPeriodicTimetable::CompileEstimated(orders, VehicleType::Train);
	auto bus = PassengerPeriodicTimetable::CompileEstimated(orders, VehicleType::Road);
	auto ship = PassengerPeriodicTimetable::CompileEstimated(orders, VehicleType::Ship);
	CHECK(train.stops[1].travel_quality == 2);
	CHECK(train.estimated_period < bus.estimated_period); CHECK(bus.estimated_period < ship.estimated_period);
	for (auto &order : orders) { order.SetTravelTime(0); order.SetWaitTime(0); order.SetTravelTimetabled(true); order.SetWaitTimetabled(true); }
	CHECK(PassengerPeriodicTimetable::Compile(orders).period == 0);
	CHECK(PassengerPeriodicTimetable::CompileEstimated(orders, VehicleType::Train).estimated_period > 0);
}

TEST_CASE("Estimated boarding rechecks changed orders cargo facilities and generations", "[passenger][precomputed][estimated][boarding]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto a = Stop(A); a.SetTravelTimetabled(false);
	auto *train = f.Train({a, Stop(B), Stop(D)}); auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	auto plan = db.PlanPrecomputed(PAX, A, D, NOW); REQUIRE(plan.ride);
	REQUIRE(ValidatePassengerServiceRide(*plan.ride, train, PAX, true));
	SECTION("NoLoad") { train->GetOrder(0)->SetLoadType(OrderLoadType::NoLoad); }
	SECTION("NoUnload") { train->GetOrder(2)->SetUnloadType(OrderUnloadType::NoUnload); }
	SECTION("Forced intermediate transfer") { train->GetOrder(1)->SetUnloadType(OrderUnloadType::Transfer); }
	SECTION("Refit") { train->GetOrder(1)->SetRefit(PAX); }
	SECTION("Facilities") { Station::Get(D)->facilities.Reset(StationFacility::Train); }
	SECTION("Capacity") { train->cargo_cap = 0; }
	SECTION("Generation") { db.Invalidate(train->orders->index); }
	SECTION("Transfer ready time") { plan.ride->not_before = NOW + 1; }
	SECTION("Wrong current occurrence") { train->LoadAt(1); }
	CHECK_FALSE(ValidatePassengerServiceRide(*plan.ride, train, PAX, true));
}

TEST_CASE("Timed completion diagnostics preserve the legacy Work boundaries", "[passenger][periodic][anytime][determinism]")
{
	Fixture f;
	f.Train({Stop(A, 1000, 0), Stop(D, 100, 0)});
	f.Train({Stop(A, 1000, 0), Stop(D, 50, 0)});
	f.Train({Stop(A, 1000, 0), Stop(D, 50, 0)});
	f.Train({Stop(A, 1000, 0), Stop(B, 20, 0)});
	f.Train({Stop(B, 1000, 30), Stop(D, 20, 0)});
	auto &db = f.Build(); auto wire = db.Save(); GameRandomSeedChecker seeds;
	PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
	peer.SetPolicyCacheEnabled(false); peer.SetDepartureIndexEnabled(false); peer.SetTimedSearchScratchEnabled(false);
	size_t queued_only = 0, missing = 0, popped = 0;
	for (size_t work = 0; work <= 160; ++work) {
		PassengerJourneyLimits limits; limits.work = work;
		auto plan = db.Plan(PAX, A, D, NOW, limits);
		auto cold = peer.Plan(PAX, A, D, NOW, limits); SameDeparturePlan(plan, cold);
		CHECK(plan.search.reverse_work + plan.search.forward_work == plan.work);
		CHECK(plan.search.complete_labels == plan.search.queued_complete + plan.search.popped_complete);
		CHECK(plan.search.best_complete_cost.has_value() == (plan.search.complete_labels != 0));
		CHECK(plan.search.work_stops == cold.search.work_stops);
		if (plan.reason != PassengerPeriodicPlan::Reason::Work) continue;
		queued_only += plan.search.queued_complete != 0 && plan.search.popped_complete == 0;
		missing += plan.search.complete_labels == 0;
		popped += plan.search.popped_complete != 0;
	}
	CHECK(queued_only > 0); CHECK(missing > 0); CHECK(popped > 0);
	CHECK(db.GetDiagnostic().work_with_complete + db.GetDiagnostic().work_without_complete == db.GetDiagnostic().outcomes[static_cast<size_t>(PassengerPeriodicPlan::Reason::Work)]);
	CHECK(db.Save() == wire); CHECK(peer.Save() == wire); CHECK(seeds.Check());
}

TEST_CASE("Anytime complete paths survive every Work boundary through 65536", "[passenger][periodic][anytime][determinism][save]")
{
	Fixture f; f.Train({Stop(A, 1000, 0), Stop(D, 100, 0), Stop(C, 100, 0)});
	f.Train({Stop(A, 1000, 0), Stop(B, 20, 0)}); f.Train({Stop(B, 1000, 30), Stop(D, 20, 0)});
	auto &db = f.Build(); auto wire = db.Save(); GameRandomSeedChecker seeds;
	PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
	peer.SetPolicyCacheEnabled(false); peer.SetDepartureIndexEnabled(false); peer.SetTimedSearchScratchEnabled(false);
	auto reference = db.Plan(PAX, A, D, NOW); REQUIRE(reference.state == PassengerPeriodicPlan::State::Certified);
	size_t rescued = 0, improved_later = 0, just_generated = 0, before_pop = 0, missing = 0;
	for (size_t work = 0; work <= 65536; ++work) {
		PassengerJourneyLimits limits; limits.work = work;
		auto plan = db.Plan(PAX, A, D, NOW, limits); auto old = db.PlanWithLegacyCompletion(PAX, A, D, NOW, limits);
		CHECK(plan.work == old.work); CHECK(plan.labels == old.labels); CHECK(plan.memory == old.memory);
		CHECK(plan.search.work_stops == old.search.work_stops); CHECK(plan.work <= work);
		if (work < 256 || work % 1024 == 0 || work == 65536) SameDeparturePlan(plan, peer.Plan(PAX, A, D, NOW, limits));
		if (old.state == PassengerPeriodicPlan::State::Certified) {
			CHECK(plan.state == PassengerPeriodicPlan::State::Certified); CHECK(plan.route == old.route); CHECK(plan.cost == old.cost);
		}
		if (plan.ride) {
			REQUIRE_FALSE(plan.route.empty()); CHECK(plan.route.front() == *plan.ride);
			CHECK(plan.route.front().boarding == A); CHECK(plan.route.back().alighting == D);
			CHECK(plan.cost >= reference.cost); CHECK(plan.arrival >= reference.arrival);
			for (size_t i = 1; i < plan.route.size(); ++i) {
				CHECK(plan.route[i - 1].alighting == plan.route[i].boarding);
				CHECK(plan.route[i - 1].arrival <= plan.route[i].departure);
			}
		}
		if (plan.state == PassengerPeriodicPlan::State::Provisional) {
			CHECK(plan.deferred); CHECK(plan.reason != PassengerPeriodicPlan::Reason::Found);
			CHECK(plan.cost == *plan.search.best_complete_cost);
			rescued += !old.ride; improved_later += plan.cost > reference.cost && plan.search.improvement_possible;
			before_pop += plan.search.first_work_stop == PassengerPeriodicPlan::WorkStop::Queue && !plan.search.popped_complete;
			just_generated += plan.search.first_work_stop == PassengerPeriodicPlan::WorkStop::Alighting && !plan.search.popped_complete;
		} else if (plan.state == PassengerPeriodicPlan::State::Deferred) { CHECK_FALSE(plan.ride); ++missing; }
	}
	CHECK(rescued > 0); CHECK(improved_later > 0); CHECK(before_pop > 0); CHECK(just_generated > 0); CHECK(missing > 0);
	WARN("Anytime Work boundaries 0..65536: rescued=" << rescued << "; inferior_to_reference=" << improved_later << "; after_generation=" << just_generated << "; before_pop=" << before_pop);
	CHECK(db.Save() == wire); CHECK(peer.Save() == wire); CHECK(seeds.Check());
}

TEST_CASE("Anytime admission limits never promote a rejected or partial label", "[passenger][periodic][anytime]")
{
	Fixture f; f.Train({Stop(A, 1000, 0), Stop(D, 100, 0)});
	f.Train({Stop(A, 1000, 0), Stop(B, 20, 0)}); f.Train({Stop(B, 1000, 30), Stop(D, 20, 0)});
	auto &db = f.Build(); auto reference = db.Plan(PAX, A, D, NOW);
	bool label_provisional = false, memory_provisional = false;
	for (size_t labels = 0; labels <= 12; ++labels) for (size_t memory : {size_t{0}, size_t{255}, size_t{256}, size_t{1792}, size_t{3328}, size_t{4096}, size_t{4864}, size_t{6400}, size_t{65536}}) {
		PassengerJourneyLimits limits; limits.labels = labels; limits.memory_bytes = memory;
		auto plan = db.Plan(PAX, A, D, NOW, limits);
		CHECK(plan.labels <= labels);
		if (!plan.search.complete_labels) CHECK_FALSE(plan.ride);
		if (plan.ride) { CHECK(plan.route.back().alighting == D); CHECK(plan.cost >= reference.cost); }
		if (plan.state == PassengerPeriodicPlan::State::Provisional) {
			CHECK(plan.reason != PassengerPeriodicPlan::Reason::Found);
			label_provisional |= plan.observations[static_cast<size_t>(PassengerPeriodicPlan::Reason::Labels)];
			memory_provisional |= plan.observations[static_cast<size_t>(PassengerPeriodicPlan::Reason::Memory)];
		}
	}
	CHECK(label_provisional); CHECK(memory_provisional);
	for (Vehicle *v : Vehicle::Iterate()) v->GetOrder(v->GetNumOrders() - 1)->SetUnloadType(OrderUnloadType::NoUnload);
	ResetPassengerServiceDatabase(); f.Build();
	for (size_t work : {0, 1, 16, 65536}) {
		PassengerJourneyLimits limits; limits.work = work; auto plan = db.Plan(PAX, A, D, NOW, limits);
		CHECK_FALSE(plan.ride); CHECK(plan.route.empty()); CHECK(plan.search.complete_labels == 0);
	}
}

TEST_CASE("Anytime ties keep the original complete key and earlier occurrence", "[passenger][periodic][anytime]")
{
	Fixture f; auto *first = f.Train({Stop(A, 1000, 0), Stop(D, 50, 0), Stop(A, 0, 0), Stop(D, 0, 0)});
	f.Train({Stop(A, 1000, 0), Stop(D, 50, 0)}); auto &db = f.Build();
	bool provisional = false;
	for (size_t work = 1; work < 160; ++work) {
		PassengerJourneyLimits limits; limits.work = work; auto plan = db.Plan(PAX, A, D, NOW, limits);
		if (!plan.ride) continue;
		CHECK(plan.ride->service == first->orders->index); CHECK(plan.ride->boarding_order == 0); CHECK(plan.ride->alighting_order == 1);
		provisional |= plan.state == PassengerPeriodicPlan::State::Provisional;
	}
	CHECK(provisional);
}

TEST_CASE("Stale cheap services cannot hide or prune a current complete journey", "[passenger][periodic][anytime][active]")
{
	Fixture f; auto *stale = f.Train({Stop(A, 1000, 0), Stop(D, 10, 0)});
	auto *current = f.Train({Stop(A, 1000, 0), Stop(D, 100, 0)});
	auto &db = f.Build(); db.Invalidate(stale->orders->index);
	auto wire = db.Save(); GameRandomSeedChecker seeds;
	auto reference = db.Plan(PAX, A, D, NOW);
	REQUIRE(reference.state == PassengerPeriodicPlan::State::Certified);
	REQUIRE(reference.ride); CHECK(reference.ride->service == current->orders->index);
	bool provisional = false, observed_stale_geometry = false;
	for (size_t work = 0; work < 160; ++work) {
		PassengerJourneyLimits limits; limits.work = work;
		auto plan = db.Plan(PAX, A, D, NOW, limits);
		auto original = db.PlanWithOriginalCompletion(PAX, A, D, NOW, limits);
		observed_stale_geometry |= original.search.destination_labels > original.search.complete_labels;
		if (plan.ride) { CHECK(plan.ride->service == current->orders->index); CHECK(db.IsCurrent(*plan.ride)); CHECK(plan.cost == reference.cost); }
		provisional |= plan.state == PassengerPeriodicPlan::State::Provisional;
	}
	CHECK(provisional); CHECK(observed_stale_geometry); CHECK(db.Save() == wire); CHECK(seeds.Check());
}

TEST_CASE("A stale transfer suffix cannot certify or suppress a valid direct alternative", "[passenger][periodic][anytime][active]")
{
	Fixture f; f.Train({Stop(A, 1000, 0), Stop(B, 10, 0)});
	auto *suffix = f.Train({Stop(B, 1000, 0), Stop(D, 10, 0)});
	auto *direct = f.Train({Stop(A, 1000, 0), Stop(D, 100, 0)});
	auto &db = f.Build(); db.Invalidate(suffix->orders->index);
	auto plan = db.Plan(PAX, A, D, NOW);
	REQUIRE(plan.state == PassengerPeriodicPlan::State::Certified); REQUIRE(plan.ride);
	CHECK(plan.route.size() == 1); CHECK(plan.ride->service == direct->orders->index);
	auto unpruned = db.PlanWithoutServicePruning(PAX, A, D, NOW);
	CHECK(plan.route == unpruned.route); CHECK(plan.cost == unpruned.cost);
}

TEST_CASE("Budget stops with only stale services remain Deferred without a usable complete path", "[passenger][periodic][anytime][active]")
{
	Fixture f; auto *first = f.Train({Stop(A, 1000, 0), Stop(D, 10, 0)});
	auto *second = f.Train({Stop(A, 1000, 0), Stop(D, 20, 0)});
	auto &db = f.Build(); db.Invalidate(first->orders->index); db.Invalidate(second->orders->index);
	bool stopped = false;
	for (size_t work = 0; work < 80; ++work) {
		PassengerJourneyLimits limits; limits.work = work; auto plan = db.Plan(PAX, A, D, NOW, limits);
		CHECK_FALSE(plan.ride); CHECK(plan.search.complete_labels == 0);
		if (plan.reason == PassengerPeriodicPlan::Reason::Work) {
			CHECK(plan.state == PassengerPeriodicPlan::State::Deferred); CHECK(plan.deferred); stopped = true;
		}
	}
	CHECK(stopped);
}

TEST_CASE("Anytime separates exhausted budgets unknown timing and proved Timed absence", "[passenger][periodic][anytime]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)});
	SECTION("Unknown phase") {
		train->current_order.Free(); auto &db = f.Build();
		auto plan = db.Plan(PAX, A, D, NOW); CHECK_FALSE(plan.ride); CHECK(plan.state == PassengerPeriodicPlan::State::Unavailable);
	}
	SECTION("No alighting") {
		train->GetOrder(1)->SetUnloadType(OrderUnloadType::NoUnload); auto &db = f.Build();
		auto plan = db.Plan(PAX, A, D, NOW); CHECK_FALSE(plan.ride); CHECK(plan.state == PassengerPeriodicPlan::State::NoRoute);
	}
	SECTION("Unsupported evaluation") {
		auto &db = f.Build(); PassengerJourneyEvaluation evaluation; evaluation.crowding_penalty = 1;
		auto plan = db.Plan(PAX, A, D, NOW, {}, evaluation); CHECK_FALSE(plan.ride); CHECK(plan.state == PassengerPeriodicPlan::State::Unavailable);
	}
	SECTION("Zero Work") {
		auto &db = f.Build(); PassengerJourneyLimits limits; limits.work = 0;
		auto plan = db.Plan(PAX, A, D, NOW, limits); CHECK_FALSE(plan.ride); CHECK(plan.state == PassengerPeriodicPlan::State::Deferred);
	}
}

TEST_CASE("Anytime provisional ServiceIntent passes the ordinary live safety checks", "[passenger][periodic][anytime][active]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(B), Stop(D)}); auto &db = f.Build();
	PassengerPeriodicPlan plan;
	for (size_t work = 1; work < 100; ++work) {
		PassengerJourneyLimits limits; limits.work = work; plan = db.Plan(PAX, A, D, NOW, limits);
		if (plan.state == PassengerPeriodicPlan::State::Provisional) break;
	}
	REQUIRE(plan.state == PassengerPeriodicPlan::State::Provisional); REQUIRE(plan.ride);
	CHECK(ValidatePassengerServiceRide(*plan.ride, train, PAX, true));
	SECTION("NoLoad") { train->GetOrder(0)->SetLoadType(OrderLoadType::NoLoad); }
	SECTION("NoUnload") { train->GetOrder(2)->SetUnloadType(OrderUnloadType::NoUnload); }
	SECTION("Forced transfer") { train->GetOrder(1)->SetUnloadType(OrderUnloadType::Transfer); }
	SECTION("Incarnation") { ++plan.ride->signature; }
	SECTION("Generation") { ++plan.ride->generation; }
	SECTION("Occurrence") { plan.ride->boarding_order = 1; }
	SECTION("Cargo capacity") { train->cargo_cap = 0; }
	SECTION("Minimum interchange") { plan.ride->not_before = NOW + 1; }
	CHECK_FALSE(ValidatePassengerServiceRide(*plan.ride, train, PAX, true));
}

TEST_CASE("Waiting packets actually adopt cache and serialize an unpopped provisional path", "[passenger][periodic][anytime][active][save]")
{
	Fixture f; auto *direct = f.Train({Stop(A, 1000, 0), Stop(D, 1000, 0)});
	std::vector<Order> stops; for (int i = 0; i < 64; ++i) { stops.push_back(Stop(A, 1, 0)); stops.push_back(Stop(B, 1, 0)); }
	/* Keep each publication task below BUILD_WORK; four admitted fleets then
	 * exhaust the query while expanding the origin, after the direct label. */
	for (int line = 0; line < 4; ++line) {
		auto *long_line = f.Train(stops);
		for (int i = 1; i < 80; ++i) Vehicle::Create<PeriodicTrain>(long_line);
	}
	f.Train({Stop(B, 1000, 0), Stop(D, 10, 0)});
	auto &db = f.Build(); REQUIRE(db.GetDiagnostic().timed_services == 6);
	PassengerJourneyLimits limits; limits.work = 65536; limits.labels = 8192; limits.memory_bytes = 8 * 1024 * 1024;
	auto old = db.PlanWithLegacyCompletion(PAX, A, D, NOW, limits);
	auto plan = db.Plan(PAX, A, D, NOW, limits);
	CAPTURE(old.work, plan.work, plan.labels, plan.search.complete_labels, plan.search.first_work_stop);
	CHECK_FALSE(old.ride); REQUIRE(plan.state == PassengerPeriodicPlan::State::Provisional); REQUIRE(plan.ride);
	CHECK(plan.search.popped_complete == 0); CHECK(plan.ride->service == direct->orders->index);
	PassengerJourneyActiveContext waiting(nullptr, A); auto *packet = f.Packet(); auto *cached = f.Packet();
	REQUIRE(waiting.PlanWaiting(*packet, PAX, D)); REQUIRE(waiting.PlanWaiting(*cached, PAX, D));
	CHECK(packet->GetPassengerJourney() == *plan.ride); CHECK_FALSE(packet->IsPassengerJourneyDeferred());
	CHECK(GetPassengerJourneyTickDiagnostic().timed_provisional_packets == 2); CHECK(GetPassengerJourneyTickDiagnostic().query_cache_hits == 1);
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	auto *loaded = CargoPacket::Create(); REQUIRE_FALSE(SlTestSaveLoadTable(packet, loaded, GetCargoPacketDesc()).empty());
	CHECK(loaded->GetPassengerJourney() == packet->GetPassengerJourney());
	PassengerServiceDatabase peer; auto wire = db.Save(); REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
	peer.SetPolicyCacheEnabled(false); peer.SetDepartureIndexEnabled(false); peer.SetTimedSearchScratchEnabled(false);
	SameDeparturePlan(plan, peer.Plan(PAX, A, D, NOW, limits)); CHECK(peer.Save() == wire);
	auto &cargo = Station::Get(A)->goods[PAX].CreateData().cargo; cargo.Append(loaded, StationID::Invalid());
	_state_ticks += 5000; direct->current_order_time = 5000; direct->lateness_counter = 5000; BeginPassengerJourneyTick();
	PassengerJourneyLoadingContext loading; std::array next{D};
	CHECK(cargo.Reserve(10, &direct->cargo, next, TileIndex{0}, direct, &loading) == 10);
	CHECK(direct->cargo.Packets()->front()->GetPassengerJourney().kind == PassengerRideKind::BoundService);
}

TEST_CASE("Departure index respects first starts duplicate phases cargo and signed tick boundaries", "[passenger][periodic][departure-index][determinism]")
{
	uint64_t seed = 1729;
	for (size_t fleet : {1, 2, 40, 150}) for (int64_t period : {int64_t{1}, int64_t{220}, int64_t{1} << 40}) {
		PassengerPeriodicService service; service.incarnation = 7; service.generation = 9; service.timetable.period = period;
		for (size_t i = 0; i < fleet; ++i) {
			seed = seed * 6364136223846793005ULL + 1;
			PassengerPeriodicVehicle v; v.id = VehicleID{static_cast<uint32_t>(i)};
			v.base = StateTicks{int64_t(seed % 200000) - 100000};
			if (i % 7 == 0) v.base = StateTicks{INT64_MIN};
			if (i % 11 == 0) v.base = StateTicks{INT64_MAX};
			if (i % 3 == 0 && i > 0) v.base = service.vehicles.front().base;
			v.phase_known = i % 9 != 8; v.capacity[0] = i % 5 ? 100 : 0; v.capacity[1] = i % 4 ? 0 : 50;
			service.vehicles.push_back(v);
		}
		for (CargoType cargo : {CargoType{0}, CargoType{1}}) {
			PassengerDepartureIndex index(service, cargo); CHECK(index.Matches(service)); CHECK(index.Bytes() <= PassengerDepartureIndex::Envelope(fleet));
			for (int64_t offset : {int64_t{0}, int64_t{1}, period, INT64_MAX}) {
				for (int64_t ready : {INT64_MIN, INT64_MIN + 1, int64_t{-100001}, int64_t{-1}, int64_t{0}, int64_t{100001}, INT64_MAX - period, INT64_MAX}) {
					for (int exclusions = 0; exclusions < 3; ++exclusions) {
						std::vector<VehicleID> excluded;
						for (size_t i = 0; i < fleet; ++i) if (exclusions == 2 || (exclusions == 1 && i % 2 == 0)) excluded.push_back(VehicleID{static_cast<uint32_t>(i)});
						uint64_t calculations = 0;
						CHECK(index.Next(offset, StateTicks{ready}, excluded, calculations) == ScannedDeparture(service, cargo, offset, StateTicks{ready}, excluded));
						CHECK(calculations <= 2);
					}
				}
			}
		}
	}
	/* This would depart at 110 with remainder-only indexing, but the only
	 * vehicle at that remainder first starts at 1000. The active other wins. */
	PassengerPeriodicService s; s.timetable.period = 100;
	PassengerPeriodicVehicle future, active; future.id = VehicleID{0}; active.id = VehicleID{1};
	future.base = StateTicks{1000}; active.base = StateTicks{20}; future.phase_known = active.phase_known = true;
	future.capacity[0] = active.capacity[0] = 100; s.vehicles = {future, active};
	PassengerDepartureIndex index(s, CargoType{0}); uint64_t calculations = 0;
	CHECK(index.Next(10, StateTicks{101}, {}, calculations) == StateTicks{130});
}

TEST_CASE("Indexed and scanned plans match every work boundary for physical fleets", "[passenger][periodic][departure-index][determinism][save]")
{
	Fixture f; size_t count = GENERATE(1, 2, 40, 150);
	auto *first = f.Train({Stop(A), Stop(B), Stop(A), Stop(D)});
	for (size_t i = 1; i < count; ++i) {
		auto *v = Vehicle::Create<PeriodicTrain>(first); v->LoadAt(static_cast<VehicleOrderID>(i % 4));
		v->current_order_time = static_cast<uint32_t>(i % 20);
		if (i % 7 == 0) v->current_order.Free();
		if (i % 11 == 0) v->cargo_cap = 0;
	}
	SECTION("Repeated occurrences") {}
	SECTION("NoLoad NoUnload and forced interchange") {
		first->GetOrder(1)->SetUnloadType(OrderUnloadType::Transfer);
		first->GetOrder(2)->SetLoadType(OrderLoadType::NoLoad);
		first->GetOrder(2)->SetUnloadType(OrderUnloadType::NoUnload);
		f.Train({Stop(B), Stop(D)});
	}
	SECTION("Depot without Wait and waypoint preserve the cycle") {
		Order depot; depot.MakeGoToDepot(DepotID{0}, {OrderDepotTypeFlag::PartOfOrders}); depot.SetTravelTime(20); depot.SetTravelTimetabled(true);
		first->GetOrder(1)->AssignOrder(depot);
		Order waypoint; waypoint.MakeGoToWaypoint(B); waypoint.SetTravelTime(20); waypoint.SetTravelTimetabled(true);
		first->GetOrder(2)->AssignOrder(waypoint);
		for (auto *v = first; v != nullptr; v = static_cast<PeriodicTrain *>(v->NextShared())) v->LoadAt(0);
	}
	auto &db = f.Build(); auto wire = db.Save();
	PassengerServiceDatabase scanned, joined;
	REQUIRE(scanned.Load(wire)); scanned.FinishLoad(true); scanned.SetDepartureIndexEnabled(false);
	REQUIRE(joined.Load(wire)); joined.FinishLoad(true);
	for (size_t work = 0; work < 700; ++work) {
		PassengerJourneyLimits limits; limits.work = work;
		StateTicks ready = NOW + int64_t(work % 13);
		auto old = scanned.Plan(PAX, A, D, ready, limits);
		SameDeparturePlan(old, db.Plan(PAX, A, D, ready, limits));
		SameDeparturePlan(old, db.Plan(PAX, A, D, ready, limits));
		joined.ClearDepartureIndices(); SameDeparturePlan(old, joined.Plan(PAX, A, D, ready, limits));
	}
	CHECK(db.GetDiagnostic().departure_index_hits > 0); CHECK(db.GetDiagnostic().departure_index_fallbacks > 0);
	CHECK(db.GetDiagnostic().vehicle_candidates == 2 * scanned.GetDiagnostic().vehicle_candidates);
	CHECK(db.GetDiagnostic().next_departures == 2 * scanned.GetDiagnostic().next_departures);
	CHECK(db.Save() == wire); CHECK(joined.Save() == wire); CHECK(scanned.Save() == wire);
}

TEST_CASE("Live dwells replace indexed phases even after scheduled departure and for future queries", "[passenger][periodic][departure-index][determinism]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)});
	for (size_t i = 1; i < 40; ++i) { auto *v = Vehicle::Create<PeriodicTrain>(train); v->LoadAt(1); }
	auto &db = f.Build(); auto wire = db.Save();
	PassengerServiceDatabase scanned; REQUIRE(scanned.Load(wire)); scanned.FinishLoad(true); scanned.SetDepartureIndexEnabled(false);
	_state_ticks += 40; train->current_order_time = 40;
	SECTION("Delayed loading") {}
	SECTION("Stopped") { train->vehstatus.Set(VehState::Stopped); }
	SECTION("Crashed") { train->vehstatus.Set(VehState::Crashed); }
	SECTION("Loading advance") { train->current_order.MakeLoadingAdvance(A); }
	SECTION("Dispatch flag") { train->vehicle_flags.Set(VehicleFlag::ScheduledDispatch); }
	SECTION("Implicit occurrence") { train->cur_implicit_order_index = 1; }
	SECTION("Timetable occurrence") { train->cur_timetable_order_index = 1; }
	SECTION("Already departed") { train->current_order.MakeLeaveStation(); Station::Get(A)->loading_vehicles.clear(); }
	for (int64_t delay : {0, 1, 9, 10, 11, 219, 220, 221, 10000}) SameDeparturePlan(scanned.Plan(PAX, A, D, _state_ticks + delay), db.Plan(PAX, A, D, _state_ticks + delay));
	CHECK(db.Save() == wire);
}

TEST_CASE("Departure indices follow publication reuse rebasing and cold peers", "[passenger][periodic][departure-index][determinism][save]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); auto &db = f.Build();
	db.Plan(PAX, A, D, NOW); auto builds = db.GetDiagnostic().departure_index_builds;
	auto old_generation = db.Get(v->orders->index)->generation;
	v->GetOrder(1)->SetTravelTime(200); db.TimetableChanged(v->orders->index);
	db.Plan(PAX, A, D, NOW); CHECK(db.GetDiagnostic().departure_index_builds == builds); CHECK(db.Get(v->orders->index)->generation == old_generation);
	auto queued = db.Save(); PassengerServiceDatabase peer; REQUIRE(peer.Load(queued)); peer.FinishLoad(true); peer.SetDepartureIndexEnabled(false);
	SameDeparturePlan(peer.Plan(PAX, A, D, NOW), db.Plan(PAX, A, D, NOW));
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH;
	for (size_t i = 0; i < 128 && (db.IsBuilding() || peer.IsBuilding()); ++i) { db.Tick(); peer.Tick(); CHECK(db.Save() == peer.Save()); }
	SameDeparturePlan(peer.Plan(PAX, A, D, _state_ticks), db.Plan(PAX, A, D, _state_ticks)); CHECK(db.GetDiagnostic().departure_index_builds > builds);
	StateTicksDelta delta{-100000}; db.ShiftTicks(delta); peer.ShiftTicks(delta); _state_ticks += delta;
	CHECK(db.GetDiagnostic().departure_index_bytes == 0);
	SameDeparturePlan(peer.Plan(PAX, A, D, _state_ticks), db.Plan(PAX, A, D, _state_ticks));
	OrderListID id = v->orders->index; VehicleID vehicle_id = v->index;
	db.Invalidate(id); v->orders->FreeChain(); v->orders = nullptr;
	for (Station *station : Station::Iterate()) std::erase(station->loading_vehicles, v); delete v;
	auto *replacement = f.Train({Stop(A, 100, 20), Stop(D, 100, 20)});
	CHECK(replacement->index == vehicle_id); CHECK(replacement->orders->index == id);
	f.PublishUpdates(db);
	peer.SetDepartureIndexEnabled(false); REQUIRE(peer.Load(db.Save())); peer.FinishLoad(true);
	SameDeparturePlan(peer.Plan(PAX, A, D, _state_ticks), db.Plan(PAX, A, D, _state_ticks));
	CHECK(peer.GetDiagnostic().departure_index_hits == 0); CHECK(db.GetDiagnostic().departure_index_hits > 0);
}

TEST_CASE("Dispatch slot scans preserve each partial work prefix", "[passenger][periodic][departure-index][dispatch][determinism]")
{
	Fixture f; auto *train = f.Train({Stop(A, 20, 0), Stop(D, 20, 0)});
	train->vehicle_flags.Set(VehicleFlag::ScheduledDispatch); train->GetOrder(0)->SetDispatchScheduleIndex(0);
	train->orders->GetScheduledDispatchScheduleSet().emplace_back(); auto &schedule = train->orders->GetDispatchScheduleByIndex(0);
	schedule.SetScheduledDispatchStartTick(NOW); schedule.SetScheduledDispatchDuration(200);
	for (uint32_t offset : {10, 60, 110, 160}) schedule.GetScheduledDispatchMutable().push_back({offset, 0});
	auto &db = f.Build(); REQUIRE(db.Get(train->orders->index)->dispatch_period > 0);
	PassengerServiceDatabase scanned; REQUIRE(scanned.Load(db.Save())); scanned.FinishLoad(true); scanned.SetDepartureIndexEnabled(false);
	for (size_t work = 0; work < 100; ++work) {
		PassengerJourneyLimits limits; limits.work = work;
		SameDeparturePlan(scanned.Plan(PAX, A, D, NOW + 11, limits), db.Plan(PAX, A, D, NOW + 11, limits));
	}
	CHECK(db.GetDiagnostic().departure_index_hits == 0); CHECK(db.GetDiagnostic().departure_index_fallbacks > 0);
}

TEST_CASE("Cargo-specific departure rows keep capacity and unknown observations separate", "[passenger][periodic][departure-index][determinism]")
{
	Fixture f; CargoType second{1};
	AutoRestoreBackup classes{CargoSpec::Get(second)->classes, CargoClasses{CargoClass::Passengers}};
	auto *first = f.Train({Stop(A), Stop(D)});
	for (size_t i = 1; i < 40; ++i) {
		auto *v = Vehicle::Create<PeriodicTrain>(first); v->LoadAt(1);
		v->cargo_type = i % 2 ? PAX : second; v->cargo_cap = i % 5 ? 10 : 0;
		if (i % 7 == 0) v->current_order.Free();
	}
	auto &db = f.Build(); PassengerServiceDatabase scanned; REQUIRE(scanned.Load(db.Save())); scanned.FinishLoad(true); scanned.SetDepartureIndexEnabled(false);
	for (CargoType cargo : {PAX, second}) for (int64_t offset = 0; offset < 500; ++offset) SameDeparturePlan(scanned.Plan(cargo, A, D, NOW + offset), db.Plan(cargo, A, D, NOW + offset));
	CHECK(db.GetDiagnostic().departure_index_hits > 0);
}

TEST_CASE("Departure successor trees agree with dense reference queries", "[passenger][periodic][departure-index][determinism]")
{
	PassengerPeriodicService service; service.timetable.period = 113;
	for (uint32_t i = 0; i < 150; ++i) {
		PassengerPeriodicVehicle v; v.id = VehicleID{i}; v.base = StateTicks{int64_t(i * 7919 % 3001) - 1500};
		v.phase_known = true; v.capacity[0] = 100; service.vehicles.push_back(v);
	}
	PassengerDepartureIndex index(service, CargoType{0});
	for (int64_t ready = -1700; ready < 1800; ++ready) {
		std::vector<VehicleID> excluded;
		for (uint32_t i = 0; i < 150; ++i) if ((i + uint64_t(ready)) % 17 == 0) excluded.push_back(VehicleID{i});
		uint64_t calculations = 0; int64_t offset = (ready + 1700) % 114;
		CHECK(index.Next(offset, StateTicks{ready}, excluded, calculations) == ScannedDeparture(service, CargoType{0}, offset, StateTicks{ready}, excluded));
	}
}

TEST_CASE("Periodic round trips repeat without horizon expansion", "[passenger][periodic]")
{
	auto table = PassengerPeriodicTimetable::Compile(std::vector<Order>{Stop(A), Stop(B), Stop(C), Stop(B)});
	REQUIRE(table.period == 440); REQUIRE(table.stops.size() == 4);
	CHECK(table.stops[1].arrival == 110); CHECK(table.stops[3].order == 3);
	for (int64_t cycle : {int64_t{0}, int64_t{1}, int64_t{1000000}}) {
		CHECK(PassengerPeriodicTimetable::NextDeparture(NOW, 120, table.period, NOW + cycle * table.period) == NOW + cycle * table.period + 120);
	}
}

TEST_CASE("Periodic departure boundaries do not overflow state ticks", "[passenger][periodic][determinism]")
{
	using T = PassengerPeriodicTimetable;
	CHECK(T::NextDeparture(NOW, 10, 100, NOW + 10) == NOW + 10);
	CHECK(T::NextDeparture(NOW, 10, 100, NOW + 11) == NOW + 110);
	CHECK_FALSE(T::NextDeparture(NOW, 0, 0, NOW));
	CHECK_FALSE(T::NextDeparture(StateTicks{INT64_MAX}, 1, 10, NOW));
	CHECK_FALSE(T::NextDeparture(StateTicks{INT64_MAX - 5}, 0, 10, StateTicks{INT64_MAX}));
	CHECK(T::NextDeparture(StateTicks{INT64_MIN}, 0, 10, StateTicks{INT64_MAX - 20}).has_value());
}

TEST_CASE("Periodic compiler distinguishes explicit zero from missing zero", "[passenger][periodic]")
{
	auto orders = std::vector<Order>{Stop(A, 100, 0), Stop(D, 0, 0)};
	CHECK(PassengerPeriodicTimetable::Compile(orders).period == 100);
	orders[1].SetTravelTimetabled(false); CHECK(PassengerPeriodicTimetable::Compile(orders).period == 0);
	orders[1].SetTravelTimetabled(true); orders[0].SetWaitTimetabled(false); CHECK(PassengerPeriodicTimetable::Compile(orders).period == 0);
	orders[0].SetWaitTimetabled(true); orders[0].SetTravelTime(0); CHECK(PassengerPeriodicTimetable::Compile(orders).period == 0);
}

TEST_CASE("Periodic control restrictions remain untimed and full load uses declared wait", "[passenger][periodic]")
{
	auto orders = std::vector<Order>{Stop(A), Stop(D)};
	SECTION("Full load") { orders[0].SetLoadType(OrderLoadType::FullLoad); CHECK(PassengerPeriodicTimetable::Compile(orders).period == 220); }
	SECTION("Conditional") { orders[1].MakeConditional(0); CHECK(PassengerPeriodicTimetable::Compile(orders).period == 0); }
	SECTION("Depot") { orders[1].MakeGoToDepot(DepotID{0}, {}); CHECK(PassengerPeriodicTimetable::Compile(orders).period == 0); }
	SECTION("Refit") { orders[1].SetRefit(PAX); CHECK(PassengerPeriodicTimetable::Compile(orders).period == 0); }
}

TEST_CASE("Shared periodic vehicles retain individually observed phases", "[passenger][periodic]")
{
	Fixture f; auto *first = f.Train({Stop(A), Stop(D)});
	auto *second = Vehicle::Create<PeriodicTrain>(first); second->current_order_time = 40;
	auto &db = f.Build(); auto *service = db.Get(first->orders->index); REQUIRE(service != nullptr);
	REQUIRE(service->vehicles.size() == 2); CHECK(service->vehicles[0].base != service->vehicles[1].base);
	auto plan = db.Plan(PAX, A, D, NOW + 11); REQUIRE(plan.ride); CHECK(plan.ride->departure == NOW + 220);
	CHECK(plan.ride->vehicle == VehicleID::Invalid()); CHECK(plan.ride->kind == PassengerRideKind::ServiceIntent);
}

TEST_CASE("Periodic transfers evaluate different periods at the current ready time", "[passenger][periodic]")
{
	Fixture f; f.Train({Stop(A, 90, 0), Stop(B, 10, 0)}); auto *connection = f.Train({Stop(B, 80, 20), Stop(D, 20, 0)});
	auto &db = f.Build();
	auto early = db.Plan(PAX, A, D, NOW); REQUIRE(early.ride); CHECK(early.arrival == NOW + 40);
	auto late = db.Plan(PAX, A, D, NOW + 101); REQUIRE(late.ride); CHECK(late.arrival > early.arrival);
	CHECK(late.ride->departure >= NOW + 101);
	CHECK(db.GetDiagnostic().policy_hits >= 1);
	CHECK(connection->orders->GetNumOrders() == 2);
}

TEST_CASE("Periodic local express and direct services compare actual phases", "[passenger][periodic]")
{
	Fixture f; auto *local = f.Train({Stop(A, 80, 0), Stop(B, 20, 0), Stop(D, 50, 0)});
	auto *express = f.Train({Stop(A, 100, 20), Stop(D, 10, 0)});
	auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride);
	CHECK(plan.ride->service == express->orders->index); CHECK(plan.arrival == NOW + 30);
	CHECK(plan.ride->service != local->orders->index);
}

TEST_CASE("Timed service bounds skip slow fleets before computing departures", "[passenger][periodic][timed-pruning]")
{
	Fixture f;
	auto *fast = f.Train({Stop(A, 1000, 0), Stop(D, 100, 0)});
	for (int i = 0; i < 10; ++i) {
		auto *slow = f.Train({Stop(A, 1000, 0), Stop(D, 1000, 0)});
		for (int n = 1; n < 40; ++n) { auto *v = Vehicle::Create<PeriodicTrain>(slow); v->LoadAt(1); }
	}
	auto &db = f.Build(); db.ResetDiagnostic();
	auto plan = db.Plan(PAX, A, D, NOW); auto pruned = db.GetDiagnostic();
	REQUIRE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.ride->service == fast->orders->index); CHECK(plan.arrival == NOW + 100);
	CHECK(pruned.pruned_services == 10); CHECK(pruned.pruned_vehicle_visits == 400);
	db.ResetDiagnostic(); auto original = db.PlanWithoutServicePruning(PAX, A, D, NOW); auto scanned = db.GetDiagnostic();
	CHECK(plan.route == original.route); CHECK(plan.cost == original.cost); CHECK(plan.arrival == original.arrival);
	CHECK(plan.work < original.work); CHECK(pruned.departure_calculations < scanned.departure_calculations);
}

TEST_CASE("Timed arena preserves every partial work prefix and canonical tie", "[passenger][periodic][timed-arena][determinism][save]")
{
	Fixture f;
	const bool weighted = GENERATE(false, true);
	CargoType second{1}; AutoRestoreBackup classes{CargoSpec::Get(second)->classes, CargoClasses{CargoClass::Passengers}};
	for (int i = 0; i < 12; ++i) {
		Order forced = Stop(B, 20, 10); forced.SetUnloadType(OrderUnloadType::Transfer);
		auto *v = f.Train({Stop(A, 1000, 0), forced, Stop(A, 20, 0), Stop(D, 200, 0)});
		for (int n = 1; n < 5; ++n) {
			auto *shared = Vehicle::Create<PeriodicTrain>(v); shared->LoadAt(1);
			if (n == 1) shared->cargo_type = second;
			if (n == 2) shared->current_order.Free();
		}
	}
	f.Train({Stop(B, 1000, 20), Stop(C, 10, 0), Stop(D, 10, 0)});
	auto &db = f.Build(); auto wire = db.Save();
	PassengerServiceDatabase peer, unused;
	REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
	REQUIRE(unused.Load(wire)); unused.FinishLoad(true);
	unused.SetPolicyCacheEnabled(false); unused.SetDepartureIndexEnabled(false); unused.SetTimedSearchScratchEnabled(false);
	PassengerJourneyEvaluation evaluation; evaluation.waiting_weight = weighted ? 500 : 0;
	evaluation.minimum_transfer = 3; evaluation.transfer_penalty = 4;
	for (size_t work = 0; work < 700; ++work) {
		PassengerJourneyLimits limits; limits.work = work; limits.memory_bytes = 8 * 1024 * 1024; limits.labels = 8192;
		CargoType cargo = work % 2 ? PAX : second;
		StateTicks ready = NOW + int64_t(work % 31);
		CAPTURE(work, cargo, weighted);
		auto old = db.PlanWithLegacySearchStructures(cargo, A, D, ready, limits, evaluation);
		SameDeparturePlan(old, db.Plan(cargo, A, D, ready, limits, evaluation));
		SameDeparturePlan(old, db.Plan(cargo, A, D, ready, limits, evaluation));
		SameDeparturePlan(old, peer.Plan(cargo, A, D, ready, limits, evaluation));
		SameDeparturePlan(old, unused.Plan(cargo, A, D, ready, limits, evaluation));
	}
	for (size_t labels : {0, 1, 2, 16, 128, 8192}) for (size_t memory : {0, 255, 256, 1791, 1792, 3328, 32768, 8 * 1024 * 1024}) {
		PassengerJourneyLimits limits; limits.labels = labels; limits.memory_bytes = memory;
		limits.labels_per_station = labels % 5;
		auto old = db.PlanWithLegacySearchStructures(PAX, A, D, NOW, limits, evaluation);
		SameDeparturePlan(old, db.Plan(PAX, A, D, NOW, limits, evaluation));
		SameDeparturePlan(old, unused.Plan(PAX, A, D, NOW, limits, evaluation));
	}
	CHECK(db.GetDiagnostic().search_scratch_bytes > 0);
	CHECK(db.GetDiagnostic().search_scratch_bytes <= PassengerTimedSearchScratch::MAX_RETAINED_BYTES);
	CHECK(unused.GetDiagnostic().search_scratch_bytes == 0);
	CHECK(db.Save() == wire); CHECK(peer.Save() == wire); CHECK(unused.Save() == wire);
	REQUIRE(db.Load(wire)); db.FinishLoad(true);
	CHECK(db.GetDiagnostic().search_scratch_bytes == 0);
	SameDeparturePlan(peer.PlanWithLegacySearchStructures(PAX, A, D, NOW), db.Plan(PAX, A, D, NOW));
	db.Reset(); CHECK(db.GetDiagnostic().search_scratch_bytes == 0);
}

TEST_CASE("Timed arena standard heap keeps all equal-key insertion indices in order", "[passenger][periodic][timed-arena][determinism]")
{
	using Item = PassengerTimedQueueItem;
	std::priority_queue<Item, std::vector<Item>, std::greater<Item>> old;
	PassengerTimedQueue reused;
	uint64_t seed = 1729;
	for (size_t round = 0; round < 8; ++round) {
		for (size_t i = 0; i < 2048; ++i) {
			seed = seed * 6364136223846793005ULL + 1;
			Item item{int64_t(seed % 3), int64_t(seed % 7), NOW + int64_t(seed % 5), seed % 3,
				NOW + int64_t(seed % 11), OrderListID{uint16_t(seed % 3)}, VehicleOrderID(seed % 3), VehicleOrderID(seed % 5), i};
			old.push(item); reused.push(item);
			if (i % 7 == 0) { REQUIRE(old.top() == reused.top()); old.pop(); reused.pop(); }
		}
		while (!old.empty()) { REQUIRE_FALSE(reused.empty()); REQUIRE(old.top() == reused.top()); old.pop(); reused.pop(); }
		CHECK(reused.empty()); reused.Clear(); CHECK(reused.CapacityBytes() > 0);
	}
}

TEST_CASE("Timed arena resets touched high IDs and drops oversized retained storage", "[passenger][periodic][timed-arena][memory]")
{
	PassengerTimedSearchScratch scratch;
	for (StationID station : {A, StationID{63999}, B, A}) {
		scratch.labels.push_back({station, NOW, 1, SIZE_MAX, 0, {}, {}});
		scratch.Append(station, scratch.labels.size() - 1);
	}
	std::vector<size_t> row;
	for (size_t index : scratch.At(A)) row.push_back(index);
	CHECK(row == std::vector<size_t>{0, 3});
	scratch.Clear(); CHECK(scratch.labels.empty()); CHECK(scratch.queue.empty()); CHECK(scratch.touched.empty());
	for (StationID station : {A, B, StationID{63999}}) for ([[maybe_unused]] size_t index : scratch.At(station)) FAIL("Old frontier survived clear");
	scratch.labels.push_back({A, NOW + 10, 5, SIZE_MAX, 0, {}, {}}); scratch.Append(A, 0);
	row.clear(); for (size_t index : scratch.At(A)) row.push_back(index); CHECK(row == std::vector<size_t>{0});
	scratch.labels.reserve(PassengerTimedSearchScratch::MAX_RETAINED_BYTES / sizeof(PassengerTimedLabel) + 1);
	CHECK(scratch.CapacityBytes() > PassengerTimedSearchScratch::MAX_RETAINED_BYTES);
	scratch.Clear(); CHECK(scratch.CapacityBytes() == 0);
}

TEST_CASE("Timed search arena allocation and latency report", "[passenger][periodic][timed-arena][.benchmark]")
{
#ifndef WITH_SERVICE_INDEX_ALLOCATIONS
	WARN("Configure OPTION_SERVICE_INDEX_ALLOCATIONS=ON on MSVC for allocation counts.");
#endif
	std::cout << "ARENA_CSV,fixture,cache,algorithm,run,ns,forward_us,labels,frontier_lookups,queue_pushes,queue_pops,allocations,allocated_bytes,peak_new_bytes,scratch_before,scratch_retained,work,memory\n";
	std::cout << "ARENA_LAYOUT,label_bytes=" << sizeof(PassengerTimedLabel) << "; item_bytes=" << sizeof(PassengerTimedQueueItem)
		<< "; fixed_scratch_bytes=" << sizeof(PassengerTimedSearchScratch) << "; page_bytes=" << sizeof(PassengerTimedSearchScratch::Page) << '\n';
	for (int fixture = 0; fixture < 3; ++fixture) {
		Fixture f;
		StationID destination;
		if (fixture == 0) {
			for (uint16_t i = 0; i < 4000; ++i) f.Train({Stop(StationID{uint16_t(2 * i)}), Stop(StationID{uint16_t(2 * i + 1)})});
			destination = B;
		} else if (fixture == 1) {
			for (uint16_t line = 0; line < 32; ++line) {
				std::vector<Order> orders;
				for (uint16_t i = 0; i < 250; ++i) orders.push_back(Stop(StationID{uint16_t(line * 250 + i)}, 10, 0));
				f.Train(std::move(orders));
			}
			destination = StationID{249};
		} else {
			for (uint16_t i = 1; i < 4000; ++i) f.Train({Stop(A, 1000, 0), Stop(StationID{i}, 10, 0), Stop(A, 10, 0), Stop(StationID{uint16_t(i + 4000)}, 1000, 0)});
			f.Train({Stop(StationID{4000}), Stop(A)});
			destination = StationID{7999};
		}
		auto &db = f.Build(); REQUIRE(Station::GetNumItems() == 8000);
		REQUIRE(db.GetDiagnostic().timed_services > 0); REQUIRE(db.GetDiagnostic().known_phase_vehicles > 0);
		struct RestorePreferences {
			PassengerServiceDatabase &db;
			~RestorePreferences() { this->db.SetPolicyCacheEnabled(true); this->db.SetDepartureIndexEnabled(true); this->db.SetTimedSearchScratchEnabled(true); }
		} restore{db};
		PassengerJourneyLimits limits; limits.work = 65536; limits.labels = 8192; limits.memory_bytes = 8 * 1024 * 1024;
		const char *name = fixture == 0 ? "pairs8000" : fixture == 1 ? "lines8000" : "hub8000";
		for (int cache = 0; cache < 3; ++cache) {
			db.SetPolicyCacheEnabled(cache != 2); db.SetDepartureIndexEnabled(cache != 2);
			db.SetTimedSearchScratchEnabled(cache != 2);
			for (size_t run = 0; run < 30; ++run) {
				StateTicks ready = NOW + int64_t(run % 11);
				for (bool legacy : {run % 2 == 0, run % 2 != 0}) {
					db.SetTimedSearchScratchEnabled(!legacy && cache != 2);
					if (cache == 0) {
						db.ClearPolicies(); db.ClearDepartureIndices();
						db.SetTimedSearchScratchEnabled(false); db.SetTimedSearchScratchEnabled(true);
					} else if (cache == 1) {
						if (legacy) db.PlanWithLegacySearchStructures(PAX, A, destination, ready, limits);
						else db.Plan(PAX, A, destination, ready, limits); // Prime the same capacities outside measurement.
					} else {
						db.Plan(PAX, A, destination, ready, limits); // Prime bounded diagnostic history; caches remain disabled.
					}
					db.ResetDiagnostic(); const size_t resident = db.GetDiagnostic().search_scratch_bytes;
					_passenger_index_allocations = {}; _passenger_index_tracking = true;
					const auto started = std::chrono::steady_clock::now();
					auto plan = legacy ? db.PlanWithLegacySearchStructures(PAX, A, destination, ready, limits) : db.Plan(PAX, A, destination, ready, limits);
					const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
					_passenger_index_tracking = false;
					REQUIRE(plan.labels > 0); // A rejected publication is not a forward-search benchmark.
					const auto allocations = _passenger_index_allocations;
					auto d = db.GetDiagnostic();
					std::cout << "ARENA_CSV," << name << ',' << (cache == 0 ? "cold" : cache == 1 ? "warm" : "disabled") << ',' << (legacy ? "legacy" : "arena") << ',' << run << ',' << ns << ',' << d.forward_microseconds << ',' << plan.labels << ',' << d.frontier_lookups << ',' << d.queue_pushes << ',' << d.queue_pops << ',' << allocations.calls << ',' << allocations.bytes << ',' << allocations.peak << ',' << resident << ',' << d.search_scratch_bytes << ',' << plan.work << ',' << plan.memory << '\n';
					SameDeparturePlan(plan, legacy ? db.Plan(PAX, A, destination, ready, limits) : db.PlanWithLegacySearchStructures(PAX, A, destination, ready, limits));
					/* Latency without the global allocator observer. Cold runs rebuild
					 * both derived caches and scratch again before the timed query. */
					if (cache == 0) {
						db.ClearPolicies(); db.ClearDepartureIndices();
						db.SetTimedSearchScratchEnabled(false); db.SetTimedSearchScratchEnabled(!legacy);
					}
					db.ResetDiagnostic();
					const size_t plain_resident = db.GetDiagnostic().search_scratch_bytes;
					const auto plain_start = std::chrono::steady_clock::now();
					auto plain = legacy ? db.PlanWithLegacySearchStructures(PAX, A, destination, ready, limits) : db.Plan(PAX, A, destination, ready, limits);
					const auto plain_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - plain_start).count();
					d = db.GetDiagnostic();
					std::cout << "ARENA_CSV," << name << ',' << (cache == 0 ? "cold" : cache == 1 ? "warm" : "disabled") << ',' << (legacy ? "legacy-untracked" : "arena-untracked") << ',' << run << ',' << plain_ns << ',' << d.forward_microseconds << ',' << plain.labels << ',' << d.frontier_lookups << ',' << d.queue_pushes << ',' << d.queue_pops << ",0,0,0," << plain_resident << ',' << d.search_scratch_bytes << ',' << plain.work << ',' << plain.memory << '\n';
					SameDeparturePlan(plan, plain);
				}
			}
		}
	}
}

TEST_CASE("Timed pruning preserves cost ties phases transfers and boarding restrictions", "[passenger][periodic][timed-pruning]")
{
	Fixture f;
	const int scenario = GENERATE(0, 1, 2, 3, 4, 5, 6);
	PassengerJourneyEvaluation evaluation;
	if (scenario == 0) { // Equal cost, arrival, transfers and departure: ServiceID decides.
		for (int i = 0; i < 12; ++i) f.Train({Stop(A, 1000, 0), Stop(D, 100, 0)});
	} else if (scenario == 1) { // A less frequent express must lose when its next phase is late.
		f.Train({Stop(A, 1900, 0), Stop(D, 100, 0)});
		auto *local = f.Train({Stop(A, 100, 0), Stop(B, 50, 0), Stop(D, 50, 0)});
		for (int i = 0; i < 5; ++i) { auto *v = Vehicle::Create<PeriodicTrain>(local); v->LoadAt(1); }
	} else if (scenario == 2) { // Cheap prefix plus express beats more than eight direct services.
		for (int i = 0; i < 10; ++i) f.Train({Stop(A, 100, 0), Stop(D, 1000, 0)});
		f.Train({Stop(A, 1900, 0), Stop(B, 100, 0)}); f.Train({Stop(B, 1900, 100), Stop(D, 100, 0)});
	} else if (scenario == 3) { // Repeated occurrences, NoLoad/NoUnload and forced unload.
		f.Train({Stop(A, 900, 0), Stop(D, 200, 0)});
		Order operating = Stop(B, 20, 80); operating.SetLoadType(OrderLoadType::NoLoad); operating.SetUnloadType(OrderUnloadType::NoUnload);
		Order forced = Stop(C, 30, 0); forced.SetUnloadType(OrderUnloadType::Transfer);
		f.Train({Stop(A, 1000, 0), operating, Stop(A, 20, 0), forced, Stop(D, 1, 0)});
		f.Train({Stop(C, 1000, 50), Stop(D, 40, 0)});
	} else if (scenario == 4) { // All minimum interchange costs are admissible, including weighted waiting.
		f.Train({Stop(A, 1000, 0), Stop(D, 400, 0)});
		f.Train({Stop(A, 1000, 0), Stop(B, 20, 0)}); f.Train({Stop(B, 1000, 40), Stop(D, 20, 0)});
		evaluation.minimum_transfer = 10; evaluation.transfer_penalty = 20; evaluation.waiting_weight = 500; evaluation.transfer_time_weight = 1500;
	} else if (scenario == 5) { // Same service reboarding on another physical vehicle remains possible.
		auto *line = f.Train({Stop(A, 1000, 0), Stop(B, 20, 100), Stop(D, 20, 0)});
		auto *other = Vehicle::Create<PeriodicTrain>(line); other->LoadAt(1); other->current_order_time = 70;
		f.Train({Stop(A, 1000, 0), Stop(D, 200, 0)});
	} else { // Scheduled dispatch and a mixture of known and unknown physical phases.
		f.Train({Stop(A, 1000, 0), Stop(D, 100, 0)});
		auto *dispatch = f.Train({Stop(A, 1000, 0), Stop(D, 200, 0)});
		dispatch->vehicle_flags.Set(VehicleFlag::ScheduledDispatch); dispatch->GetOrder(0)->SetDispatchScheduleIndex(0);
		dispatch->orders->GetScheduledDispatchScheduleSet().emplace_back(); auto &schedule = dispatch->orders->GetDispatchScheduleByIndex(0);
		schedule.SetScheduledDispatchStartTick(NOW); schedule.SetScheduledDispatchDuration(2000);
		for (uint32_t offset : {0, 500, 1000, 1500}) schedule.GetScheduledDispatchMutable().push_back({offset, 0});
		auto *line = f.Train({Stop(A, 1000, 0), Stop(D, 300, 0)});
		auto *unknown = Vehicle::Create<PeriodicTrain>(line); unknown->current_order.Free();
	}
	auto &db = f.Build();
	for (int64_t delay : {0, 1, 10, 101, 501, 2100}) {
		CAPTURE(scenario, delay);
		auto original = db.PlanWithoutServicePruning(PAX, A, D, NOW + delay, {}, evaluation);
		auto plan = db.Plan(PAX, A, D, NOW + delay, {}, evaluation);
		SameDeparturePlan(plan, db.PlanWithLegacySearchStructures(PAX, A, D, NOW + delay, {}, evaluation));
		REQUIRE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.ride == original.ride); CHECK(plan.route == original.route);
		CHECK(plan.arrival == original.arrival); CHECK(plan.cost == original.cost); CHECK(plan.transfers == original.transfers);
		CHECK(plan.work <= original.work);
		if (scenario == 0) CHECK(plan.work == original.work);
		if (scenario == 5 && delay == 0) {
			REQUIRE(plan.route.size() == 2); CHECK(plan.route[0].service == plan.route[1].service);
			CHECK(plan.arrival == NOW + 50); CHECK(plan.transfers == 1);
		}
	}
}

TEST_CASE("Timed pruning budgets are independent of caches save load and joining", "[passenger][periodic][timed-pruning][determinism][save]")
{
	Fixture f; f.Train({Stop(A, 1000, 0), Stop(D, 100, 0)});
	for (int i = 0; i < 10; ++i) {
		auto *slow = f.Train({Stop(A, 1000, 0), Stop(B, 100, 0), Stop(D, 900, 0)});
		for (int n = 1; n < 8; ++n) { auto *v = Vehicle::Create<PeriodicTrain>(slow); v->LoadAt(1); if (n % 3 == 0) v->current_order.Free(); }
	}
	auto &db = f.Build(); auto wire = db.Save(); PassengerServiceDatabase cold, peer;
	REQUIRE(cold.Load(wire)); cold.FinishLoad(true); cold.SetPolicyCacheEnabled(false); cold.SetDepartureIndexEnabled(false);
	REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
	size_t improved = 0, old_admitted = 0, new_admitted = 0;
	for (size_t work = 0; work < 500; ++work) {
		CAPTURE(work);
		PassengerJourneyLimits limits; limits.work = work;
		auto plan = db.Plan(PAX, A, D, NOW, limits);
		SameDeparturePlan(plan, db.Plan(PAX, A, D, NOW, limits));
		SameDeparturePlan(plan, cold.Plan(PAX, A, D, NOW, limits));
		peer.ClearPolicies(); peer.ClearDepartureIndices(); SameDeparturePlan(plan, peer.Plan(PAX, A, D, NOW, limits));
		auto original = db.PlanWithoutServicePruning(PAX, A, D, NOW, limits);
		old_admitted += original.ride && !original.deferred; new_admitted += plan.ride && !plan.deferred;
		if (plan.ride && !plan.deferred && (!original.ride || original.deferred)) ++improved;
		if (original.ride && !original.deferred) { REQUIRE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.cost <= original.cost); CHECK(plan.route == original.route); }
	}
	CHECK(improved > 0); CHECK(db.GetDiagnostic().pruned_services > 0);
	WARN("Timed Work boundaries: old_admitted=" << old_admitted << "; new_admitted=" << new_admitted << "; newly_admitted=" << improved << "; route_quality_changes=0");
	CHECK(db.Save() == wire); CHECK(cold.Save() == wire); CHECK(peer.Save() == wire);
	// The same admitted complete label must survive Label/Memory admission boundaries.
	for (size_t labels : {1, 2, 4, 8, 32}) for (size_t memory : {size_t{256}, size_t{1792}, size_t{4096}, size_t{65536}}) {
		PassengerJourneyLimits limits; limits.labels = labels; limits.memory_bytes = memory;
		SameDeparturePlan(db.Plan(PAX, A, D, NOW, limits), cold.Plan(PAX, A, D, NOW, limits));
	}
}

TEST_CASE("Timed partial reverse bounds retain an eleven transfer journey", "[passenger][periodic][timed-pruning][determinism]")
{
	Fixture f;
	for (uint16_t i = 0; i < 12; ++i) {
		StationID from{i}, to{uint16_t(i + 1)};
		f.Train({Stop(from, 1000, uint16_t(i * 20)), Stop(to, 10, 0)});
	}
	// A feasible direct incumbent must not make unsettled stations unreachable.
	f.Train({Stop(A, 1000, 0), Stop(StationID{12}, 2000, 0)});
	for (uint16_t i = 13; i < 43; ++i) f.Train({Stop(StationID{i}, 1000, 0), Stop(StationID{12}, 5000, 0)});
	auto &db = f.Build(); PassengerJourneyLimits limits;
	auto full = db.Plan(PAX, A, StationID{12}, NOW, limits);
	REQUIRE(full.ride); CHECK_FALSE(full.deferred); CHECK(full.transfers == 11); CHECK(full.route.size() == 12); CHECK(full.arrival == NOW + 230);
	bool partial_found = false;
	for (size_t work = 40; work < 700; ++work) {
		limits.work = work; auto plan = db.Plan(PAX, A, StationID{12}, NOW, limits);
		auto original = db.PlanWithoutServicePruning(PAX, A, StationID{12}, NOW, limits);
		if (plan.ride && !plan.deferred) {
			CHECK(plan.cost == full.cost); CHECK(plan.route == full.route);
			partial_found |= plan.observations[static_cast<size_t>(PassengerPeriodicPlan::Reason::Work)];
		}
		if (original.ride && !original.deferred) { REQUIRE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.cost <= original.cost); }
	}
	CHECK(partial_found);
	limits = {}; limits.max_transfers = 10;
	auto direct = db.Plan(PAX, A, StationID{12}, NOW, limits); REQUIRE(direct.ride); CHECK(direct.arrival == NOW + 2000);
	CHECK(direct.deferred); // A safety ceiling cannot certify the slower direct route as optimal.
}

TEST_CASE("Timed service pruning follows cargo-specific exchange masks and capacities", "[passenger][periodic][timed-pruning]")
{
	Fixture f; CargoType second{1}; AutoRestoreBackup classes{CargoSpec::Get(second)->classes, CargoClasses{CargoClass::Passengers}};
	auto *direct = f.Train({Stop(A, 1000, 0), Stop(D, 100, 0)});
	auto *second_direct = Vehicle::Create<PeriodicTrain>(direct); second_direct->cargo_type = second;
	Order restricted = Stop(B, 10, 0); restricted.SetLoadType(OrderLoadType::CargoTypeLoad); restricted.SetUnloadType(OrderUnloadType::CargoTypeUnload);
	restricted.SetLoadType(OrderLoadType::NoLoad, PAX); restricted.SetUnloadType(OrderUnloadType::NoUnload, PAX);
	auto *local = f.Train({Stop(A, 1000, 0), restricted, Stop(D, 1000, 0)});
	auto *second_local = Vehicle::Create<PeriodicTrain>(local); second_local->cargo_type = second;
	auto *express = f.Train({Stop(B, 1000, 10), Stop(D, 10, 0)}); express->cargo_type = second;
	auto &db = f.Build();
	for (CargoType cargo : {PAX, second}) {
		auto original = db.PlanWithoutServicePruning(cargo, A, D, NOW);
		auto plan = db.Plan(cargo, A, D, NOW); REQUIRE(plan.ride); CHECK_FALSE(plan.deferred);
		CHECK(plan.route == original.route); CHECK(plan.cost == original.cost);
		CHECK(plan.arrival == NOW + (cargo == PAX ? 100 : 20));
	}
}

TEST_CASE("Periodic timed routes win while missing timetable services stay unknown", "[passenger][periodic]")
{
	Fixture f; auto *timed = f.Train({Stop(A, 200, 0), Stop(D, 100, 0)});
	Order unknown = Stop(A); unknown.SetWaitTimetabled(false); f.Train({unknown, Stop(D, 1)});
	auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK(plan.ride->service == timed->orders->index);
}

TEST_CASE("Periodic policies are inert across cold warm evicted and unused caches", "[passenger][periodic][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); GameRandomSeedChecker seeds;
	auto cold = db.Plan(PAX, A, D, NOW + 10000); auto warm = db.Plan(PAX, A, D, NOW + 10000);
	CHECK(cold.ride == warm.ride); CHECK(cold.work == warm.work);
	db.ClearPolicies(); auto rebuilt = db.Plan(PAX, A, D, NOW + 10000);
	CHECK(cold.ride == rebuilt.ride); CHECK(cold.work == rebuilt.work);
	for (int i = 0; i < 3000; ++i) { _state_ticks++; db.Tick(); }
	CHECK(db.GetDiagnostic().builds == 1); CHECK(db.GetDiagnostic().bytes < 4096); CHECK(seeds.Check());
}

TEST_CASE("Periodic actual Work admission shares packets and caps cheap queries across contexts", "[passenger][periodic][admission][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = f.Build();
	const bool cached = GENERATE(false, true); db.SetPolicyCacheEnabled(cached); db.SetDepartureIndexEnabled(cached);
	GameRandomSeedChecker seed;
	PassengerJourneyActiveContext event(nullptr, A);
	REQUIRE(event.PlanWaiting(*f.Packet(), PAX, D));
	auto work = GetPassengerJourneyTickDiagnostic().admission.used; REQUIRE(work < 100);
	for (int i = 0; i < 100; ++i) REQUIRE(event.PlanWaiting(*f.Packet(), PAX, D));
	CHECK(GetPassengerJourneyTickDiagnostic().admission.accepted == 1); CHECK(GetPassengerJourneyTickDiagnostic().admission.used == work);
	for (size_t i = 1; i < PassengerQueryAdmission::QUERIES_PER_TICK; ++i) REQUIRE(PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*f.Packet(), PAX, D));
	CHECK(GetPassengerJourneyTickDiagnostic().admission.used == work * PassengerQueryAdmission::QUERIES_PER_TICK);
	CHECK(GetPassengerJourneyTickDiagnostic().admission.reserved == GetPassengerJourneyTickDiagnostic().admission.used + GetPassengerJourneyTickDiagnostic().admission.returned);
	CHECK(event.GetWorkAllowanceUsed() == work);
	ResetPassengerJourneyDiagnostic(); db.ClearPolicies();
	PassengerJourneyActiveContext exhausted(nullptr, A);
	auto *denied = f.Packet(); CHECK_FALSE(exhausted.PlanWaiting(*denied, PAX, D));
	CHECK_FALSE(exhausted.PlanWaiting(*f.Packet(), PAX, D));
	CHECK(GetPassengerJourneyTickDiagnostic().query_cache_hits == 1);
	CHECK(GetPassengerJourneyTickDiagnostic().deferred[static_cast<size_t>(PassengerJourneyFallbackReason::QueryLimit)] == 2);
	CHECK(GetPassengerJourneyTickDiagnostic().deferred[static_cast<size_t>(PassengerJourneyFallbackReason::QueryWork)] == 0);
	CHECK(denied->IsPassengerJourneyDeferred());
	CHECK(GetPassengerJourneyTickDiagnostic().admission.rejections[static_cast<size_t>(PassengerQueryAdmission::Rejection::TickQueries)] == 1);
	++_state_ticks; BeginPassengerJourneyTick(); REQUIRE(PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*denied, PAX, D));
	CHECK(seed.Check());
}

TEST_CASE("Periodic missing publications charge one Work and retry after publication", "[passenger][periodic][admission][save][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); Station::CreateAtIndex(D, TileIndex{0}); auto &db = f.Build();
	auto *missing = f.Packet(); PassengerJourneyActiveContext settled(nullptr, A);
	CHECK_FALSE(settled.PlanWaiting(*missing, PAX, D));
	CHECK(GetPassengerJourneyTickDiagnostic().admission.missing_published == 1);
	CHECK(GetPassengerJourneyTickDiagnostic().admission.used == 1); CHECK(db.GetScheduleDiagnostic().misses == 0);
	auto *new_line = f.Train({Stop(A), Stop(D)}); db.Invalidate(new_line->orders->index);
	auto *unpublished = f.Packet(); CHECK_FALSE(PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*unpublished, PAX, D));
	CHECK(unpublished->IsPassengerJourneyDeferred()); CHECK(GetPassengerJourneyTickDiagnostic().admission.missing_building == 1);
	auto wire = db.Save(); PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true); CHECK(peer.IsBuilding());
	f.PublishUpdates(db); BeginPassengerJourneyTick(); REQUIRE(PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*unpublished, PAX, D));
}

TEST_CASE("Schedule protection rescues an exhausted Timed query and survives packet save split and transport", "[passenger][periodic][admission][schedule][save][determinism]")
{
	Fixture f;
	const bool rescue = GENERATE(false, true);
	auto *estimated = f.Train({Stop(A, rescue ? 1 : 100, 0), Stop(D, rescue ? 1 : 100, 0)}); estimated->current_order.Free();
	std::vector<Order> stops; for (int i = 0; i < 30; ++i) { stops.push_back(Stop(A, 1, 0)); stops.push_back(Stop(B, 1, 0)); }
	for (int n = 0; n < 3; ++n) {
		auto *line = f.Train(stops); for (int i = 1; i < 170; ++i) {
			auto *vehicle = Vehicle::Create<PeriodicTrain>(line); vehicle->LoadAt(1); vehicle->current_order_time = i % 50;
		}
	}
	f.Train({Stop(B, 1000, 0), Stop(D, 10, 0)}); auto &db = f.Build();
	const bool cached = GENERATE(false, true); db.SetPolicyCacheEnabled(cached); db.SetDepartureIndexEnabled(cached);
	PassengerJourneyActiveContext context(nullptr, A); auto *packet = f.Packet(), *shared = f.Packet();
	auto found = context.PlanWaiting(*packet, PAX, D);
	CAPTURE(GetPassengerJourneyTickDiagnostic().admission.timed_used, GetPassengerJourneyTickDiagnostic().admission.schedule_used);
	CHECK(found == rescue); CHECK(context.PlanWaiting(*shared, PAX, D) == rescue);
	const auto &d = GetPassengerJourneyTickDiagnostic();
	CAPTURE(d.admission.timed_used, d.admission.schedule_used, packet->GetPassengerJourney().kind);
	CHECK(d.admission.timed_used == 49152); CHECK(d.admission.schedule_allocated == 16384);
	CHECK(d.admission.schedule_rescued == (rescue ? 1 : 0)); CHECK(d.admission.work_stopped == 1); CHECK(d.admission.work_no_route == (rescue ? 0 : 1));
	CHECK(IsEstimatedPassengerRide(packet->GetPassengerJourney().kind) == rescue);
	if (!rescue) { CHECK(d.admission.used == 65536); CHECK(packet->IsPassengerJourneyDeferred()); }
	CHECK(context.GetWorkAllowanceUsed() == d.admission.used); CHECK(d.admission.used == d.admission.timed_used + d.admission.schedule_used);
	CHECK(d.admission.used + d.admission.returned == 65536);
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	auto *loaded = CargoPacket::Create(); REQUIRE_FALSE(SlTestSaveLoadTable(packet, loaded, GetCargoPacketDesc()).empty());
	CHECK(loaded->GetPassengerRoutingMode() == packet->GetPassengerRoutingMode());
	CHECK(loaded->GetPassengerJourney() == packet->GetPassengerJourney());
	auto *split = loaded->Split(3); REQUIRE(split);
	CHECK(loaded->HasSamePassengerRouting(*split)); REQUIRE(loaded->Merge(split)); CHECK(loaded->Count() == 10);
	loaded->FallbackPassengerJourney(A, NOW);
	loaded->SetPassengerVia(D); // A real untimed boarding has a validated progressing hop.
	RecordPassengerJourneyMovement(PassengerJourneyMovement::Board, *loaded, 7);
	RecordPassengerJourneyMovement(PassengerJourneyMovement::Deliver, *loaded, 3);
	CHECK(d.untimed_boarded_passengers == 7); CHECK(d.untimed_delivered_passengers == 3);
}

TEST_CASE("Retired v4 Work history is skipped and cannot prevent Packet merging", "[passenger][periodic][admission][save]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); f.Build();
	auto *packet = f.Packet(); REQUIRE(PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*packet, PAX, D));
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	REQUIRE(_sl_xv_feature_versions[XSLFI_PASSENGER_JOURNEY_ROUTING] == 3);
	const auto current = GetCargoPacketDesc();
	std::vector<NamedSaveLoad> old(current.begin(), current.end());
	bool work_history = true;
	old.push_back(NSL("passenger_work_exhausted", SLEG_CONDVAR_X(work_history, SLE_BOOL, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_PASSENGER_JOURNEY_ROUTING, 4))));
	_sl_xv_feature_versions[XSLFI_PASSENGER_JOURNEY_ROUTING] = 4;
	auto *exhausted = CargoPacket::Create();
	const auto tagged_save = SlTestSaveLoadTable(packet, exhausted, old, current);
	work_history = false;
	auto *unexhausted = CargoPacket::Create();
	const auto untagged_save = SlTestSaveLoadTable(packet, unexhausted, old, current);
	CHECK(tagged_save != untagged_save);
	CHECK(exhausted->GetPassengerJourney() == packet->GetPassengerJourney());
	CHECK(exhausted->HasSamePassengerRouting(*unexhausted));
	CHECK(StationCargoList::AreMergable(exhausted, unexhausted)); CHECK(VehicleCargoList::AreMergable(exhausted, unexhausted));
	REQUIRE(exhausted->Merge(unexhausted)); CHECK(exhausted->Count() == 20);
	_sl_xv_feature_versions[XSLFI_PASSENGER_JOURNEY_ROUTING] = 3;
	auto *roundtrip = CargoPacket::Create();
	const auto saved = SlTestSaveLoadTable(exhausted, roundtrip, current);
	CHECK(roundtrip->Count() == 20); CHECK(roundtrip->GetPassengerJourney() == packet->GetPassengerJourney());
	const std::string field = "passenger_work_exhausted";
	CHECK(std::search(saved.begin(), saved.end(), field.begin(), field.end()) == saved.end());
}

TEST_CASE("Periodic local invalidation preserves unrelated published services", "[passenger][periodic][determinism]")
{
	Fixture f; auto *one = f.Train({Stop(A), Stop(B)}); auto *two = f.Train({Stop(C), Stop(D)});
	auto &db = f.Build(); auto generation = db.Get(two->orders->index)->generation;
	one->GetOrder(1)->SetTravelTime(50); db.Invalidate(one->orders->index);
	CHECK(db.GetPublication(one->orders->index) == PassengerServiceDatabase::Publication::Dirty); CHECK(db.Get(two->orders->index)->generation == generation);
	db.Tick(1); CHECK(db.GetPublication(one->orders->index) == PassengerServiceDatabase::Publication::Dirty);
	f.PublishUpdates(db); CHECK(db.Get(two->orders->index)->generation == generation); CHECK(db.Get(one->orders->index)->timetable.period == 170);
}

TEST_CASE("Periodic scheduler progress and captured phases survive save and join", "[passenger][periodic][save][determinism]")
{
	Fixture f; auto *one = f.Train({Stop(A), Stop(D)}); auto *two = f.Train({Stop(B), Stop(C)}); auto &db = f.Build();
	one->current_order_time = 5000; one->lateness_counter = 9999;
	db.Invalidate(two->orders->index); db.Tick(1);
	auto wire = db.Save(); PassengerServiceDatabase joined; REQUIRE(joined.Load(wire)); joined.FinishLoad(true);
	CHECK(joined.Save() == wire); CHECK(joined.Get(one->orders->index)->vehicles[0].base == NOW);
	db.Tick(); joined.Tick(); CHECK(db.Save() == joined.Save());
	CHECK(db.Plan(PAX, A, D, NOW).ride == joined.Plan(PAX, A, D, NOW).ride);
}

TEST_CASE("Periodic database rejects malformed and oversized saved state", "[passenger][periodic][save]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto wire = db.Save(); PassengerServiceDatabase other;
	wire.pop_back(); CHECK_FALSE(other.Load(wire)); wire = db.Save(); wire[0] = 10; CHECK_FALSE(other.Load(wire));
	CHECK(other.GetDiagnostic().services == 0);
}

TEST_CASE("Periodic rebuilding tolerates another edit and falls back before publication", "[passenger][periodic]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = GetPassengerServiceDatabase();
	CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride); db.Tick(1);
	train->GetOrder(1)->SetTravelTime(50); db.Invalidate(train->orders->index); db.Tick(1);
	train->GetOrder(1)->SetTravelTime(25); db.Invalidate(train->orders->index); f.Build();
	auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK(plan.arrival == NOW + 35);
}

TEST_CASE("Periodic shared dispatch competition never invents physical trips", "[passenger][periodic][dispatch]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); train->vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
	Vehicle::Create<PeriodicTrain>(train)->vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
	auto &db = f.Build(); CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride);
	CHECK(db.GetDiagnostic().vehicles == 2); CHECK(db.GetDiagnostic().timed_services == 0);
}

TEST_CASE("Periodic single vehicle dispatch uses real slots with turnaround checks", "[passenger][periodic][dispatch]")
{
	Fixture f; auto *train = f.Train({Stop(A, 20, 0), Stop(D, 20, 0)});
	train->vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
	train->orders->GetScheduledDispatchScheduleSet().emplace_back();
	auto &schedule = train->orders->GetDispatchScheduleByIndex(0);
	schedule.SetScheduledDispatchStartTick(NOW); schedule.SetScheduledDispatchDuration(200);
	schedule.GetScheduledDispatchMutable().push_back({10, 0}); schedule.GetScheduledDispatchMutable().push_back({100, 0});
	train->GetOrder(0)->SetDispatchScheduleIndex(0);
	SECTION("Multiple nonconflicting slots") { auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW + 11); REQUIRE(plan.ride); CHECK(plan.ride->departure == NOW + 100); }
	SECTION("Reuse disabled does not invent a competing vehicle") { schedule.SetScheduledDispatchReuseSlots(false); auto &db = f.Build(); CHECK(db.Plan(PAX, A, D, NOW + 11).ride.has_value()); }
	SECTION("Overlapping turnaround") { schedule.GetScheduledDispatchMutable()[1].offset = 30; auto &db = f.Build(); CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride); }
	SECTION("Previously allocated future non-reusable slot") { schedule.SetScheduledDispatchReuseSlots(false); schedule.SetScheduledDispatchLastDispatch(100); auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK(plan.ride->departure == NOW + 100); }
}

TEST_CASE("Periodic active journeys board a delayed actual vehicle with real empty seats", "[passenger][periodic][active]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto *packet = f.Packet();
	PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, D));
	CHECK(packet->GetPassengerJourney().kind == PassengerRideKind::ServiceIntent);
	Station::Get(A)->goods[PAX].CreateData().cargo.Append(packet, StationID::Invalid());
	_state_ticks += 5000; train->current_order_time = 5000; train->lateness_counter = 5000; BeginPassengerJourneyTick();
	PassengerJourneyLoadingContext loading; std::array next{D};
	CHECK(Station::Get(A)->goods[PAX].data->cargo.Reserve(10, &train->cargo, next, TileIndex{0}, train, &loading) == 10);
	CHECK(train->cargo.Packets()->front()->GetPassengerJourney().kind == PassengerRideKind::BoundService);
	CHECK(train->cargo.Packets()->front()->GetPassengerJourney().vehicle == train->index);
	CHECK(GetPassengerJourneyTickDiagnostic().builds == 0); CHECK(db.GetDiagnostic().builds == 1);
}

TEST_CASE("Periodic ride validation rejects no load no unload forced transfer and changed identities", "[passenger][periodic][active]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(B), Stop(D)}); auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride);
	CHECK(ValidatePassengerServiceRide(*plan.ride, train, PAX, true));
	SECTION("No load") { train->GetOrder(0)->SetLoadType(OrderLoadType::NoLoad); }
	SECTION("No unload") { train->GetOrder(2)->SetUnloadType(OrderUnloadType::NoUnload); }
	SECTION("Forced intermediate transfer") { train->GetOrder(1)->SetUnloadType(OrderUnloadType::Transfer); }
	SECTION("Wrong service incarnation") { plan.ride->signature++; }
	CHECK_FALSE(ValidatePassengerServiceRide(*plan.ride, train, PAX, true));
}

TEST_CASE("Periodic routing disabled leaves the scheduler and cargo state untouched", "[passenger][periodic][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = GetPassengerServiceDatabase(); auto wire = db.Save();
	_settings_game.linkgraph.passenger_journey_routing = false; db.Tick(); CHECK(db.Save() == wire);
}

TEST_CASE("Cached periodic route alternatives change the winner with departure phase", "[passenger][periodic]")
{
	Fixture f;
	auto *local = f.Train({Stop(A, 900, 0), Stop(D, 100, 0)});
	auto *express = f.Train({Stop(A, 990, 200), Stop(D, 10, 0)});
	auto &db = f.Build();
	auto early = db.Plan(PAX, A, D, NOW); REQUIRE(early.ride); CHECK(early.ride->service == local->orders->index);
	auto late = db.Plan(PAX, A, D, NOW + 190); REQUIRE(late.ride); CHECK(late.ride->service == express->orders->index);
	CHECK(db.GetDiagnostic().policy_hits == 1); CHECK(late.ride->departure == NOW + 200);
}

TEST_CASE("Periodic and snapshot planners agree on complete small timetable networks", "[passenger][periodic][comparison]")
{
	Fixture f; f.Train({Stop(A, 1000, 0), Stop(B, 20, 0)}); f.Train({Stop(B, 1000, 30), Stop(D, 10, 0)});
	auto &db = f.Build();
	PassengerServiceGraphLimits graph_limits; graph_limits.horizon = 3000;
	auto graph = PassengerServiceGraph::Build(PAX, NOW, graph_limits, {}, nullptr, PassengerTimingPolicy::AssumeTimetable);
	PassengerJourneyPlanner planner(graph);
	for (int64_t ready : {int64_t{0}, int64_t{100}, int64_t{1100}}) {
		auto old = planner.Plan(PAX, A, D, NOW + ready); auto periodic = db.Plan(PAX, A, D, NOW + ready);
		REQUIRE(old.HasJourney()); REQUIRE(periodic.ride);
		CHECK(periodic.arrival == old.journeys.front().arrival); CHECK(periodic.cost == old.journeys.front().generalized_time.base());
		REQUIRE(periodic.route.size() == old.journeys.front().legs.size());
		for (size_t i = 0; i < periodic.route.size(); ++i) {
			const auto &leg = old.journeys.front().legs[i];
			CHECK(periodic.route[i].service == graph.GetTrips()[leg.boarding.trip].service);
			CHECK(periodic.route[i].boarding == leg.boarding_station); CHECK(periodic.route[i].alighting == leg.alighting_station);
		}
	}
}

TEST_CASE("Periodic split passengers can board a second real shared vehicle immediately", "[passenger][periodic][active]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto *shared = Vehicle::Create<PeriodicTrain>(train); f.Build();
	auto *packet = f.Packet(200); PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, D));
	auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo; waiting.Append(packet, StationID::Invalid());
	std::array next{D}; PassengerJourneyLoadingContext loading;
	CHECK(waiting.Load(100, &train->cargo, next, TileIndex{0}, train, &loading) == 100);
	CHECK(packet->GetPassengerJourney().kind == PassengerRideKind::ServiceIntent);
	CHECK(waiting.Load(100, &shared->cargo, next, TileIndex{0}, shared, &loading) == 100);
	CHECK(waiting.TotalCount() == 0); CHECK(GetPassengerJourneyTickDiagnostic().boarded_passengers == 200);
}

TEST_CASE("Periodic many packets share one event policy and never build service graphs", "[passenger][periodic][active]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); f.Build(); PassengerJourneyActiveContext context(nullptr, A);
	for (int i = 0; i < 4096; ++i) { auto *packet = f.Packet(1); REQUIRE(context.PlanWaiting(*packet, PAX, D)); }
	CHECK(GetPassengerJourneyTickDiagnostic().queries == 1); CHECK(GetPassengerJourneyTickDiagnostic().query_cache_hits == 4095);
	CHECK(GetPassengerJourneyTickDiagnostic().builds == 0); CHECK(GetPassengerServiceDatabase().GetDiagnostic().searches == 1);
}

TEST_CASE("Periodic network scales to 8000 stations without expanded physical visits", "[passenger][periodic][scaling]")
{
	Fixture f;
	for (uint16_t i = 0; i < 4000; ++i) f.Train({Stop(StationID{static_cast<uint16_t>(2 * i)}), Stop(StationID{static_cast<uint16_t>(2 * i + 1)})});
	auto &db = GetPassengerServiceDatabase();
	for (int i = 0; i < 100; ++i) { _state_ticks++; db.Tick(); }
	auto diagnostic = db.GetDiagnostic(); CHECK(diagnostic.services == 4000); CHECK(diagnostic.vehicles == 4000);
	CHECK(Station::GetNumItems() == 8000); CHECK(diagnostic.bytes < PassengerServiceDatabase::MAX_BYTES);
	auto plan = db.Plan(PAX, A, B, _state_ticks); REQUIRE(plan.ride);
	SameDeparturePlan(plan, db.PlanWithLegacySearchStructures(PAX, A, B, _state_ticks));
	for (uint16_t origin : {uint16_t{0}, uint16_t{2000}, uint16_t{4000}, uint16_t{7998}}) {
		StationID from{origin}, to{uint16_t(origin + 1)};
		auto old = db.PlanWithLegacySearchStructures(PAX, from, to, _state_ticks);
		SameDeparturePlan(old, db.Plan(PAX, from, to, _state_ticks));
		SameDeparturePlan(old, db.Plan(PAX, from, to, _state_ticks));
	}
	CHECK(GetPassengerJourneyTickDiagnostic().builds == 0); CHECK(GetPassengerJourneyTickDiagnostic().prediction_executions == 0);
}

TEST_CASE("Periodic dirty queue rotation prevents repeated edits from starving other lists", "[passenger][periodic][determinism]")
{
	Fixture f; auto *one = f.Train({Stop(A), Stop(B)}); auto *two = f.Train({Stop(C), Stop(D)}); auto &db = f.Build();
	db.Invalidate(one->orders->index); db.Invalidate(two->orders->index);
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH;
	for (int i = 0; i < 10 && db.GetPublication(two->orders->index) != PassengerServiceDatabase::Publication::Published; ++i) { db.Invalidate(one->orders->index); _state_ticks++; db.Tick(600); }
	REQUIRE(db.GetPublication(two->orders->index) == PassengerServiceDatabase::Publication::Published);
}

TEST_CASE("Periodic unplanned implicit stops never become forecast boarding occurrences", "[passenger][periodic]")
{
	Fixture f; Order implicit; implicit.MakeImplicit(B); auto *train = f.Train({Stop(A), implicit, Stop(D)}); f.Build();
	auto *service = GetPassengerServiceDatabase().Get(train->orders->index); REQUIRE(service != nullptr);
	CHECK(service->timetable.stops.size() == 2); CHECK(service->timetable.stops[1].order == 2);
	train->cur_real_order_index = train->cur_implicit_order_index = 1; train->last_station_visited = B;
	auto ride = GetPassengerServiceDatabase().Plan(PAX, A, D, NOW).ride; REQUIRE(ride);
	CHECK_FALSE(ValidatePassengerServiceRide(*ride, train, PAX, true));
}

TEST_CASE("Periodic saved order list identities cannot bind to recycled pool IDs", "[passenger][periodic][save]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride);
	OrderListID id = train->orders->index; uint64_t incarnation = train->orders->GetCargoDistRouteGeneration();
	train->orders->FreeChain(); train->orders = OrderList::Create(std::vector<Order>{Stop(A), Stop(D)}, train);
	CHECK(train->orders->index == id); CHECK(train->orders->GetCargoDistRouteGeneration() != incarnation);
	CHECK_FALSE(ValidatePassengerServiceRide(*plan.ride, train, PAX, true)); CHECK_FALSE(db.IsCurrent(*plan.ride));
	f.PublishUpdates(db); CHECK(db.Get(id)->incarnation != incarnation);
}

TEST_CASE("Periodic state tick rebasing preserves departures and packet retry intervals", "[passenger][periodic][save]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride);
	auto *packet = f.Packet(); packet->SetPassengerJourney(*plan.ride, D);
	StateTicksDelta delta{-50000}; db.ShiftTicks(delta); packet->ShiftPassengerJourneyTicks(delta); _state_ticks += delta;
	auto shifted = db.Plan(PAX, A, D, _state_ticks); REQUIRE(shifted.ride);
	CHECK(shifted.ride->departure == packet->GetPassengerJourney().departure); CHECK(shifted.arrival == plan.arrival + delta);
}

TEST_CASE("Periodic long shared lines preserve their direct ride within a bounded policy search", "[passenger][periodic][scaling]")
{
	Fixture f; std::vector<Order> orders;
	for (uint16_t i = 0; i < 300; ++i) orders.push_back(Stop(StationID{i}));
	auto *train = f.Train(orders); Vehicle::Create<PeriodicTrain>(train); auto &db = f.Build();
	PassengerJourneyLimits limits; limits.work = 65536; limits.labels = 8192; limits.memory_bytes = 8 * 1024 * 1024;
	auto plan = db.Plan(PAX, A, StationID{299}, NOW, limits); REQUIRE(plan.ride); CHECK_FALSE(plan.deferred);
	CHECK(plan.ride->alighting_order == 299); CHECK(plan.transfers == 0); CHECK(plan.work <= limits.work);
}

TEST_CASE("Expired periodic intents choose an alternative while a delayed live ride remains usable", "[passenger][periodic][active]")
{
	Fixture f; auto *original = f.Train({Stop(A, 900, 0), Stop(D, 100, 0)}); f.Train({Stop(A, 990, 200), Stop(D, 10, 0)}); f.Build();
	auto *packet = f.Packet(); PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, D));
	CHECK(packet->GetPassengerJourney().service == original->orders->index);
	Station::Get(A)->goods[PAX].CreateData().cargo.Append(packet, StationID::Invalid()); _state_ticks = NOW + 190; BeginPassengerJourneyTick();
	SECTION("Missed original vehicle") {
		/* It has actually left; a vehicle still in OT_LOADING at A is a
		 * catchable live departure, even if its published time has passed. */
		original->cur_real_order_index = original->cur_implicit_order_index = 1;
		original->current_order = *original->GetOrder(1);
		std::erase(Station::Get(A)->loading_vehicles, original);
	}
	SECTION("Actual delayed original vehicle") {}
	Station::Get(A)->goods[PAX].data->cargo.RoutePassengerJourneys(PAX, A);
	REQUIRE(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
	CHECK(packet->GetPassengerJourney().service == Station::Get(A)->loading_vehicles.front()->orders->index);
}

TEST_CASE("Periodic transfer intents enforce the actual minimum interchange time", "[passenger][periodic][active]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); auto *connection = f.Train({Stop(B, 100, 0), Stop(D, 10, 0)}); f.Build();
	_settings_game.linkgraph.passenger_transfer_penalty = 20; auto *packet = f.Packet();
	PassengerJourneyActiveContext arrived(connection, B); REQUIRE(arrived.PlanWaiting(*packet, PAX, D));
	CHECK(packet->GetPassengerJourney().not_before == NOW + 20);
	CHECK_FALSE(ValidatePassengerServiceRide(packet->GetPassengerJourney(), connection, PAX, true));
	_state_ticks += 20; CHECK(ValidatePassengerServiceRide(packet->GetPassengerJourney(), connection, PAX, true));
}

TEST_CASE("Dirty service intents retain their saved retry state until the next simulation event", "[passenger][periodic][save][determinism]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto *packet = f.Packet();
	PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, D));
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	auto *loaded = CargoPacket::Create(); REQUIRE_FALSE(SlTestSaveLoadTable(packet, loaded, GetCargoPacketDesc()).empty());
	auto saved_ride = loaded->GetPassengerJourney(); Station::Get(A)->goods[PAX].CreateData().cargo.Append(loaded, StationID::Invalid());
	db.Invalidate(train->orders->index); auto wire = db.Save(); REQUIRE(db.Load(wire)); AfterLoadPassengerJourneys();
	CHECK(loaded->GetPassengerRoutingMode() == PassengerRoutingMode::Journey); CHECK(loaded->GetPassengerJourney() == saved_ride);
	CHECK(db.Save() == wire);
}

TEST_CASE("Periodic capacity changes invalidate only the affected service while occupancy does not", "[passenger][periodic][active]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto generation = db.Get(train->orders->index)->generation;
	train->cargo.Append(f.Packet()); ObservePassengerPeriodicVehicle(train); CHECK(db.Get(train->orders->index)->generation == generation);
	train->cargo_cap = 0; ObservePassengerPeriodicVehicle(train); CHECK(db.GetPublication(train->orders->index) == PassengerServiceDatabase::Publication::Dirty); f.PublishUpdates(db);
	CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride);
	train->cargo_cap = 100; ObservePassengerPeriodicVehicle(train); CHECK(db.GetPublication(train->orders->index) == PassengerServiceDatabase::Publication::Dirty); f.PublishUpdates(db);
	CHECK(db.Plan(PAX, A, D, NOW).ride.has_value());
}

TEST_CASE("Periodic round trips prefer the earlier equivalent boarding occurrence", "[passenger][periodic]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(B), Stop(D), Stop(B), Stop(A)}); train->LoadAt(4); auto &db = f.Build();
	auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK(plan.ride->boarding_order == 4);
	CHECK(ValidatePassengerServiceRide(*plan.ride, train, PAX, true));
}

TEST_CASE("Routine order window notifications preserve periodic generations and policies", "[passenger][periodic][active]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build();
	auto generation = db.Get(train->orders->index)->generation;
	REQUIRE(db.Plan(PAX, A, D, NOW).ride);
	for (uint i = 0; i < 3000; ++i) InvalidateVehicleOrder(train, VIWD_MODIFY_ORDERS);
	CHECK(db.Get(train->orders->index)->generation == generation);
	CHECK(db.Plan(PAX, A, D, NOW).ride.has_value());
	CHECK(db.GetDiagnostic().policy_hits == 1);
}

TEST_CASE("Unrelated landscape clearing preserves the published passenger network", "[passenger][periodic][active]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build();
	auto generation = db.Get(train->orders->index)->generation;
	AutoRestoreBackup company{_current_company, OWNER_DEITY};
	TileIndex tile = TileXY(40, 40); MakeClear(tile, ClearGround::Grass, 3);
	REQUIRE(Command<Commands::LandscapeClear>::Do({DoCommandFlag::Execute}, tile).Succeeded());
	CHECK(db.Get(train->orders->index)->generation == generation);
	CHECK(db.GetDiagnostic().global_invalidations == 0);
}

TEST_CASE("Automatic timetable edits coalesce while safe old generations and policies survive", "[passenger][periodic][save][determinism]")
{
	Fixture f; _state_ticks = StateTicks{65536}; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build();
	auto generation = db.Get(train->orders->index)->generation;
	REQUIRE(db.Plan(PAX, A, D, NOW).ride);
	train->GetOrder(1)->SetTravelTime(200);
	for (uint i = 0; i < PassengerServiceDatabase::TIMETABLE_EPOCH - 1; ++i) {
		db.TimetableChanged(train->orders->index); db.Tick();
		REQUIRE(db.Get(train->orders->index)->generation == generation); ++_state_ticks;
	}
	CHECK(db.Plan(PAX, A, D, _state_ticks).ride.has_value()); CHECK(db.GetDiagnostic().policy_hits == 1);
	auto wire = db.Save(); PassengerServiceDatabase joined; REQUIRE(joined.Load(wire)); joined.FinishLoad(true);
	++_state_ticks; joined.Tick(); db.Tick();
	CHECK(db.Get(train->orders->index)->generation > generation);
	CHECK(db.Save() == joined.Save());
	CHECK(db.Plan(PAX, A, D, _state_ticks).arrival == joined.Plan(PAX, A, D, _state_ticks).arrival);
}

TEST_CASE("An OD outside the timed station index does not expand the national timed network", "[passenger][periodic]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); EnsurePassengerTestStations(std::vector<Order>{Stop(D)}); auto &db = f.Build();
	for (uint i = 0; i < 1000; ++i) {
		auto plan = db.Plan(PAX, A, D, NOW + i);
		CHECK_FALSE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.work == 1);
	}
	CHECK(db.GetDiagnostic().policy_hits == 999);
}

TEST_CASE("An unknown destination phase stays untimed without expanding other timed services", "[passenger][periodic]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); auto *unknown = f.Train({Stop(B), Stop(D)});
	unknown->current_order.Free(); auto &db = f.Build();
	auto plan = db.Plan(PAX, A, D, NOW); CHECK_FALSE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.work < 10);
	unknown->LoadAt(1); ObservePassengerPeriodicVehicle(unknown);
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; db.Tick();
	CHECK(db.Plan(PAX, A, D, NOW).ride.has_value());
}

TEST_CASE("Separate timed components reject unreachable ODs with bounded incidence work", "[passenger][periodic][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); f.Train({Stop(C), Stop(D)}); auto &db = f.Build();
	auto cold = db.Plan(PAX, A, D, NOW); CHECK_FALSE(cold.ride); CHECK_FALSE(cold.deferred); CHECK(cold.work < 32);
	auto warm = db.Plan(PAX, A, D, NOW + 999); CHECK(warm.work == cold.work); CHECK_FALSE(warm.ride);
}

TEST_CASE("Timing publication epochs use floor division for negative state ticks", "[passenger][periodic][determinism]")
{
	Fixture f; _state_ticks = StateTicks{-1}; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build();
	auto generation = db.Get(train->orders->index)->generation;
	db.TimetableChanged(train->orders->index); db.Tick(); CHECK(db.Get(train->orders->index)->generation == generation);
	_state_ticks = StateTicks{0}; db.Tick(); CHECK(db.Get(train->orders->index)->generation > generation);
}

TEST_CASE("Operating stops retain physical occurrences, dwell, phases and saved publication", "[passenger][periodic][operational][save][determinism]")
{
	Fixture f; _state_ticks = StateTicks{65536};
	Order closed = Stop(B, 20, 30); closed.SetLoadType(OrderLoadType::NoLoad); closed.SetUnloadType(OrderUnloadType::NoUnload);
	auto *train = f.Train({Stop(A, 100, 10), closed, Stop(D, 40, 10), closed});
	auto &db = f.Build(); auto *service = db.Get(train->orders->index); REQUIRE(service);
	REQUIRE(service->timetable.stops.size() == 4); CHECK(service->timetable.period == 260);
	CHECK(service->timetable.stops[1].order == 1); CHECK(service->timetable.stops[3].order == 3);
	CHECK(service->timetable.stops[1].board == 0); CHECK(service->timetable.stops[1].alight == 0);
	GameRandomSeedChecker seed;
	for (int64_t cycle : {int64_t{0}, int64_t{1}, int64_t{1000000}}) {
		auto ready = _state_ticks + cycle * 260;
		auto cold = db.Plan(PAX, A, D, ready), warm = db.Plan(PAX, A, D, ready);
		REQUIRE(cold.ride); CHECK(cold.ride->departure == ready + 10); CHECK(cold.arrival == ready + 100);
		CHECK(cold.ride == warm.ride); CHECK(cold.work == warm.work);
		CHECK_FALSE(db.Plan(PAX, A, B, ready).ride); CHECK_FALSE(db.Plan(PAX, B, D, ready).ride);
	}
	PassengerServiceGraphLimits limits; limits.horizon = 1000;
	auto graph = PassengerServiceGraph::Build(PAX, _state_ticks, limits, {}, nullptr, PassengerTimingPolicy::AssumeTimetable);
	CHECK(graph.GetServicesAtStation(B).empty()); CHECK(graph.GetStationVisits(B).empty());
	REQUIRE(graph.GetService(train->orders->index)->stops.size() == 4);
	CHECK(graph.GetTrips()[0].prediction.visits[1].order == 1);
	CHECK(graph.GetVisitStop({0, 1}).station == B);
	PassengerJourneyPlanner planner(graph);
	for (int64_t ready : {int64_t{0}, int64_t{11}, int64_t{270}}) {
		auto old = planner.Plan(PAX, A, D, _state_ticks + ready); auto current = db.Plan(PAX, A, D, _state_ticks + ready);
		REQUIRE(old.HasJourney()); REQUIRE(current.ride); CHECK(current.arrival == old.journeys.front().arrival);
	}
	auto generation = service->generation;
	train->GetOrder(1)->SetWaitTime(50); db.TimetableChanged(train->orders->index);
	auto wire = db.Save(); PassengerServiceDatabase joined; REQUIRE(joined.Load(wire)); joined.FinishLoad(true);
	CHECK(joined.Save() == wire); CHECK(joined.Get(train->orders->index)->generation == generation);
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; db.Tick(); joined.Tick();
	CHECK(db.Save() == joined.Save()); CHECK(db.Get(train->orders->index)->timetable.period == 280);
	auto updated = db.Plan(PAX, A, D, _state_ticks); REQUIRE(updated.ride);
	CHECK(updated.arrival - updated.ride->departure == StateTicksDelta{110}); CHECK(seed.Check());
	/* Removing passenger index entries must retain the physical loading lookup
	 * used by the snapshot planner for passengers already onboard. */
	train->LoadAt(1);
	auto onboard_graph = PassengerServiceGraph::Build(PAX, _state_ticks, limits,
			PassengerServiceScope{B, D, train->orders->index}, nullptr, PassengerTimingPolicy::AssumeTimetable);
	CHECK(onboard_graph.GetStationVisits(B).empty());
	auto visit = onboard_graph.FindOnboardVisit(train->index, B, 1); REQUIRE(visit);
	CHECK(onboard_graph.GetVisitStop(*visit).order == 1);
	PassengerJourneyPlanner onboard_planner(onboard_graph); auto onward = onboard_planner.PlanOnboard(PAX, *visit, D);
	REQUIRE(onward.HasJourney()); CHECK(onward.journeys.front().arrival == _state_ticks + 90);
	CHECK(onward.journeys.front().transfers == 0);
}

TEST_CASE("Cargo-specific operating stops cannot join passenger service components", "[passenger][periodic][operational][index]")
{
	Fixture f; constexpr CargoType OTHER{1};
	AutoRestoreBackup classes{CargoSpec::Get(OTHER)->classes, CargoClasses{CargoClass::Passengers}};
	AutoRestoreBackup distribution{_settings_game.linkgraph.distribution_per_cargo[OTHER], DistributionType::Asymmetric};
	Order closed = Stop(B, 20, 30);
	closed.SetLoadType(OrderLoadType::CargoTypeLoad); closed.SetLoadType(OrderLoadType::NoLoad, PAX);
	closed.SetUnloadType(OrderUnloadType::CargoTypeUnload); closed.SetUnloadType(OrderUnloadType::NoUnload, PAX);
	auto *first = f.Train({Stop(A), closed}); auto *second = f.Train({Stop(C), closed, Stop(D)});
	for (auto *train : {first, second}) { auto *part = Vehicle::Create<PeriodicTrain>(train); part->cargo_type = OTHER; }
	auto &db = f.Build(); CHECK_FALSE(db.Plan(PAX, A, D, _state_ticks).ride); REQUIRE(db.Plan(OTHER, A, D, _state_ticks).ride);
	PassengerServiceGraphLimits limits; limits.horizon = 3000;
	for (CargoType cargo : {PAX, OTHER, PAX}) {
		auto graph = PassengerServiceGraph::Build(cargo, _state_ticks, limits, PassengerServiceScope{A, D}, nullptr, PassengerTimingPolicy::AssumeTimetable);
		if (cargo == PAX) { CHECK(graph.GetServices().empty()); CHECK(graph.GetServicesAtStation(B).empty()); }
		else { CHECK(graph.GetServices().size() == 2); CHECK(graph.GetServicesAtStation(B).size() == 2); }
	}
	/* Permission edits must invalidate a warm corridor even though neither the
	 * destination stations nor the eligible fleet IDs changed. */
	AutoRestoreBackup company{_current_company, OWNER_DEITY};
	AutoRestoreBackup first_owner{first->owner, OWNER_DEITY}, second_owner{second->owner, OWNER_DEITY};
	REQUIRE(Command<Commands::ModifyOrder>::Do({DoCommandFlag::Execute}, first->index, VehicleOrderID{1},
			MOF_CARGO_TYPE_UNLOAD, static_cast<uint16_t>(OrderUnloadType::UnloadIfPossible), PAX, {}).Succeeded());
	REQUIRE(Command<Commands::ModifyOrder>::Do({DoCommandFlag::Execute}, second->index, VehicleOrderID{1},
			MOF_CARGO_TYPE_LOAD, static_cast<uint16_t>(OrderLoadType::LoadIfPossible), PAX, {}).Succeeded());
	auto changed = PassengerServiceGraph::Build(PAX, _state_ticks, limits, PassengerServiceScope{A, D}, nullptr, PassengerTimingPolicy::AssumeTimetable);
	CHECK(changed.GetServices().size() == 2); CHECK_FALSE(changed.GetDiagnostic().static_cache_hit);
	PassengerJourneyPlanner planner(changed); REQUIRE(planner.Plan(PAX, A, D, _state_ticks).HasJourney());
}

TEST_CASE("Mixed passenger exchange stops keep directional permissions", "[passenger][periodic][operational]")
{
	Fixture f; Order board = Stop(A), alight = Stop(D), closed = Stop(C, 20, 30);
	SECTION("Different operating station") {}
	SECTION("Operating stop returns to the boarding StationID") { closed.SetDestination(A); }
	board.SetUnloadType(OrderUnloadType::NoUnload); alight.SetLoadType(OrderLoadType::NoLoad);
	closed.SetLoadType(OrderLoadType::NoLoad); closed.SetUnloadType(OrderUnloadType::NoUnload);
	auto *train = f.Train({board, Stop(B), closed, alight}); auto &db = f.Build();
	REQUIRE(db.Plan(PAX, A, D, NOW).ride); REQUIRE(db.Plan(PAX, B, D, NOW).ride);
	CHECK_FALSE(db.Plan(PAX, B, A, NOW).ride); CHECK_FALSE(db.Plan(PAX, D, B, NOW).ride);
	CHECK_FALSE(db.Plan(PAX, A, C, NOW).ride); CHECK_FALSE(db.Plan(PAX, C, D, NOW).ride);
	PassengerServiceGraphLimits limits; limits.horizon = 2000;
	auto graph = PassengerServiceGraph::Build(PAX, NOW, limits, {}, nullptr, PassengerTimingPolicy::AssumeTimetable);
	PassengerJourneyPlanner planner(graph);
	REQUIRE(planner.Plan(PAX, A, D, NOW).HasJourney()); CHECK_FALSE(planner.Plan(PAX, B, A, NOW).HasJourney());
	CHECK_FALSE(planner.Plan(PAX, D, B, NOW).HasJourney()); CHECK(graph.GetServicesAtStation(C).empty());
	PassengerRouteState state{train->orders->GetOrderVector(), {}, 0, 0, 0, true, false, 1};
	auto prediction = PredictPassengerJourney(state, 0, D, PAX, NOW, true, PassengerTimingPolicy::AssumeTimetable);
	CHECK(prediction.arrival == NOW + 270);
	auto *packet = f.Packet(); PassengerOdFallbackContext fallback; CHECK(fallback.CanBoard(*packet, train, PAX));
	auto ride = db.Plan(PAX, A, D, NOW).ride; REQUIRE(ride); CHECK(ValidatePassengerServiceRide(*ride, train, PAX, true));
	train->LoadAt(2); train->current_order.SetLoadType(OrderLoadType::NoLoad); train->current_order.SetUnloadType(OrderUnloadType::NoUnload);
	CHECK(ValidatePassengerServiceRide(*ride, train, PAX, false)); CHECK_FALSE(ValidatePassengerServiceRide(*ride, train, PAX, true));
}

TEST_CASE("Operating first stop remains a dispatch and phase anchor", "[passenger][periodic][operational][dispatch][save]")
{
	Fixture f; Order closed = Stop(B, 20, 30); closed.SetLoadType(OrderLoadType::NoLoad); closed.SetUnloadType(OrderUnloadType::NoUnload);
	auto *train = f.Train({closed, Stop(A, 40, 0), Stop(D, 50, 0)});
	train->vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
	train->orders->GetScheduledDispatchScheduleSet().emplace_back();
	auto &schedule = train->orders->GetDispatchScheduleByIndex(0);
	schedule.SetScheduledDispatchStartTick(NOW); schedule.SetScheduledDispatchDuration(300);
	schedule.GetScheduledDispatchMutable().push_back({30, 0}); train->GetOrder(0)->SetDispatchScheduleIndex(0);
	auto &db = f.Build(); auto *service = db.Get(train->orders->index); REQUIRE(service);
	CHECK(service->dispatch_stop == 0); CHECK(service->timetable.stops[0].station == B);
	for (int64_t cycle : {int64_t{0}, int64_t{1}, int64_t{1000}}) {
		auto plan = db.Plan(PAX, A, D, NOW + 300 * cycle); REQUIRE(plan.ride);
		CHECK(plan.ride->boarding_order == 1); CHECK(plan.ride->departure == NOW + 300 * cycle + 70); CHECK(plan.arrival == NOW + 300 * cycle + 120);
	}
	auto wire = db.Save(); PassengerServiceDatabase restored; REQUIRE(restored.Load(wire)); restored.FinishLoad(true);
	CHECK(restored.Save() == wire); CHECK(restored.Plan(PAX, A, D, NOW).ride == db.Plan(PAX, A, D, NOW).ride);
}

TEST_CASE("Waypoint waits and reversals are operating events in periodic and snapshot cycles", "[passenger][periodic][operational][comparison]")
{
	Fixture f; Order waypoint; waypoint.MakeGoToWaypoint(C);
	waypoint.SetWaypointFlags({OrderWaypointFlag::Reverse}); waypoint.SetTravelTime(20); waypoint.SetTravelTimetabled(true);
	waypoint.SetWaitTime(30); waypoint.SetWaitTimetabled(true);
	bool timed = true; int64_t dwell = 30;
	SECTION("Declared dwell") {}
	SECTION("Explicit zero dwell") { dwell = 0; waypoint.SetWaitTime(0); }
	SECTION("Passing waypoint has no dwell instruction") { dwell = 0; waypoint.SetWaitTime(0); waypoint.SetWaitTimetabled(false); }
	SECTION("Missing travel zero is unknown") { timed = false; waypoint.SetTravelTime(0); waypoint.SetTravelTimetabled(false); }
	SECTION("Explicit travel zero") { waypoint.SetTravelTime(0); }
	SECTION("Missing positive dwell is unknown") { timed = false; waypoint.SetWaitTimetabled(false); }
	f.Train({Stop(A, 100, 10), waypoint, Stop(D, 40, 0)}); auto &db = f.Build();
	auto plan = db.Plan(PAX, A, D, NOW); CHECK(plan.ride.has_value() == timed);
	CHECK_FALSE(db.Plan(PAX, A, C, NOW).ride); CHECK_FALSE(db.Plan(PAX, C, D, NOW).ride);
	PassengerServiceGraphLimits limits; limits.horizon = 2000;
	auto graph = PassengerServiceGraph::Build(PAX, NOW, limits, {}, nullptr, PassengerTimingPolicy::AssumeTimetable);
	CHECK(graph.GetServicesAtStation(C).empty()); CHECK(graph.GetServices()[0].reverse_waypoints.size() == 1);
	if (timed) {
		CHECK(plan.arrival == NOW + 10 + waypoint.GetTravelTime() + dwell + 40);
		PassengerJourneyPlanner planner(graph); auto old = planner.Plan(PAX, A, D, NOW); REQUIRE(old.HasJourney()); CHECK(old.journeys.front().arrival == plan.arrival);
	}
}

TEST_CASE("Depot traversal separates passenger continuity from timing certainty", "[passenger][periodic][operational][depot][active]")
{
	Fixture f; Order depot; depot.MakeGoToDepot(DepotID{0}, {OrderDepotTypeFlag::PartOfOrders});
	depot.SetTravelTime(20); depot.SetTravelTimetabled(true); depot.SetWaitTime(30); depot.SetWaitTimetabled(true);
	auto expected = PassengerDepotTraversal::Timed;
	bool timed = true;
	SECTION("Regular entry with declared dwell") {}
	SECTION("Explicit zero travel and dwell") { depot.SetTravelTime(0); depot.SetWaitTime(0); }
	SECTION("Missing travel") { timed = false; depot.SetTravelTime(0); depot.SetTravelTimetabled(false); }
	SECTION("No depot wait instruction") { depot.SetWaitTime(0); depot.SetWaitTimetabled(false); }
	SECTION("Unconfirmed depot wait is not executed") { depot.SetWaitTimetabled(false); }
	SECTION("Only when servicing is needed") { expected = PassengerDepotTraversal::Untimed; timed = false; depot.SetDepotOrderType({OrderDepotTypeFlag::PartOfOrders, OrderDepotTypeFlag::Service}); }
	SECTION("Nearest depot") { expected = PassengerDepotTraversal::Untimed; timed = false; depot.SetDepotActionType(ODATFB_NEAREST_DEPOT); }
	SECTION("Unbunching") { expected = PassengerDepotTraversal::Untimed; timed = false; depot.SetDepotActionType(ODATFB_UNBUNCH); }
	SECTION("Halt") { expected = PassengerDepotTraversal::Blocked; timed = false; depot.SetDepotActionType(ODATFB_HALT); }
	SECTION("Sell without halt bit") { expected = PassengerDepotTraversal::Blocked; timed = false; depot.SetDepotActionType(ODATFB_SELL); }
	SECTION("Refit even to same cargo") { expected = PassengerDepotTraversal::Blocked; timed = false; depot.SetRefit(PAX); }
	SECTION("Automatic depot instruction") { expected = PassengerDepotTraversal::Blocked; timed = false; depot.SetDepotOrderType({}); }
	CHECK(ClassifyPassengerDepot(depot) == expected);
	auto *train = f.Train({Stop(A, 100, 10), depot, Stop(B, 40, 5), Stop(D, 60, 0)}); auto &db = f.Build();
	auto plan = db.Plan(PAX, A, D, NOW); CHECK(plan.ride.has_value() == timed);
	auto *packet = f.Packet(); PassengerOdFallbackContext fallback;
	const bool continuing = expected != PassengerDepotTraversal::Blocked;
	CHECK(fallback.CanBoard(*packet, train, PAX) == continuing);
	PassengerJourneyExecution ride; ride.kind = PassengerRideKind::ServiceIntent;
	ride.service = train->orders->index; ride.signature = train->orders->GetCargoDistRouteGeneration();
	ride.generation = db.Get(train->orders->index)->generation;
	ride.boarding = A; ride.alighting = D; ride.boarding_order = 0; ride.alighting_order = 3; ride.not_before = NOW;
	CHECK(ValidatePassengerServiceRide(ride, train, PAX, true) == continuing);
	PassengerRouteState state{train->orders->GetOrderVector(), {}, 0, 0, 0, true, false, 1};
	auto predicted = PredictPassengerJourney(state, 0, D, PAX, NOW, true, PassengerTimingPolicy::AssumeTimetable);
	CHECK(predicted.arrival.has_value() == timed);
	if (timed) {
		CHECK(plan.arrival == NOW + 10 + depot.GetTravelTime() + depot.GetTimetabledWait() + 40 + 5 + 60);
		CHECK(predicted.arrival == plan.arrival);
		PassengerServiceGraphLimits limits; limits.horizon = 2000;
		auto graph = PassengerServiceGraph::Build(PAX, NOW, limits, {}, nullptr, PassengerTimingPolicy::AssumeTimetable);
		CHECK(graph.GetService(train->orders->index)->stops.size() == 3);
		PassengerJourneyPlanner planner(graph); auto old = planner.Plan(PAX, A, D, NOW); REQUIRE(old.HasJourney()); CHECK(old.journeys.front().arrival == plan.arrival);
	} else if (continuing) { CHECK(predicted.reachable); CHECK_FALSE(predicted.arrival); }
	if (continuing) {
		train->GetOrder(2)->SetUnloadType(OrderUnloadType::Transfer);
		auto *interchange = CargoPacket::Create(1, 0, A, TileIndex{0}, Money{0}); REQUIRE(interchange->SetPassengerDestination(PAX, B));
		PassengerOdFallbackContext forced; CHECK(forced.CanBoard(*interchange, train, PAX)); CHECK_FALSE(forced.CanBoard(*packet, train, PAX));
		CHECK_FALSE(ValidatePassengerServiceRide(ride, train, PAX, true));
	}
}

TEST_CASE("Schedule duration-only direct policies retain no invented absolute times", "[passenger][schedule]")
{
	Fixture f; auto *v = f.Train({Stop(A, 80, 10), Stop(B, 120, 20), Stop(D, 80, 0)}); v->current_order.Free(); auto &db = f.Build();
	CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride);
	auto plan = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(plan.ride);
	CHECK(plan.riding == 220); CHECK(plan.waiting == 155); CHECK(plan.transfers == 0);
	CHECK(plan.ride->kind == PassengerRideKind::EstimatedServiceIntent); CHECK(plan.ride->vehicle == VehicleID::Invalid());
	CHECK(plan.ride->departure == StateTicks{}); CHECK(plan.ride->arrival == StateTicks{}); CHECK(plan.ride->not_before == NOW);
	v->LoadAt(0); PassengerJourneyLoadingContext loading; auto *packet = f.Packet(); packet->SetPassengerJourney(*plan.ride, D);
	REQUIRE(loading.CanBoard(*packet, v)); loading.PrepareBoard(*packet, v);
	CHECK(packet->GetPassengerJourney().kind == PassengerRideKind::EstimatedBoundService);
	v->LoadAt(1); PassengerJourneyActiveContext arrived(v, B); CHECK(arrived.Decide(*packet, PAX, false, OrderUnloadType::UnloadIfPossible) == VehicleCargoList::MoveToAction::Keep);
}

TEST_CASE("Schedule routes compare frequency capacity direct and transfer costs", "[passenger][schedule]")
{
	Fixture f; auto *local = f.Train({Stop(A, 100, 0), Stop(B, 50, 20), Stop(D, 50, 0)});
	auto *express = f.Train({Stop(A, 1000, 0), Stop(D, 10, 0)});
	local->current_order.Free(); express->current_order.Free();
	SECTION("Infrequent express loses despite short riding time") {}
	SECTION("Frequent express wins") { express->GetOrder(0)->SetTravelTime(50); }
	SECTION("Low capacity express loses") { express->GetOrder(0)->SetTravelTime(200); express->cargo_cap = 1; }
	auto &db = f.Build(); auto plan = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(plan.ride);
	CHECK(plan.ride->service == (express->GetOrder(0)->GetTravelTime() == 50 ? express : local)->orders->index);
}

TEST_CASE("Schedule compares multiple interchange corridors including known phases", "[passenger][schedule]")
{
	Fixture f;
	auto *ab = f.Train({Stop(A, 50, 0), Stop(B, 50, 0)}); auto *bd = f.Train({Stop(B, 50, 0), Stop(D, 50, 0)});
	auto *ac = f.Train({Stop(A, 900, 0), Stop(C, 25, 0)}); auto *cd = f.Train({Stop(C, 900, 0), Stop(D, 25, 0)});
	auto *direct = f.Train({Stop(A, 250, 0), Stop(D, 250, 0)});
	for (auto *v : {ab, bd, ac, cd, direct}) v->current_order.Free();
	SECTION("Both interchange legs unknown") {}
	SECTION("Downstream timing known does not promise a connection") { bd->LoadAt(0); }
	SECTION("Upstream timing known still produces a duration policy") { ab->LoadAt(0); }
	auto &db = f.Build(); PassengerJourneyEvaluation evaluation; evaluation.minimum_transfer = 10; evaluation.transfer_penalty = 10;
	auto plan = db.PlanEstimated(PAX, A, D, NOW, {}, evaluation); REQUIRE(plan.ride);
	CHECK(plan.ride->service == ab->orders->index); CHECK(plan.ride->alighting == B); CHECK(plan.transfers == 1);
	PassengerJourneyLimits limits; limits.max_transfers = 0;
	auto no_transfer = db.PlanEstimated(PAX, A, D, NOW, limits, evaluation); REQUIRE(no_transfer.ride); CHECK(no_transfer.ride->service == direct->orders->index);
}

TEST_CASE("Schedule fleet frequency never fabricates evenly spaced unknown phases", "[passenger][schedule]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); auto *shared = Vehicle::Create<PeriodicTrain>(v);
	v->current_order.Free(); shared->current_order.Free(); auto &db = f.Build();
	auto unknown = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(unknown.ride); CHECK(unknown.ride->estimated_wait == 220);
	v->LoadAt(0); shared->LoadAt(1);
	db.Invalidate(v->orders->index); f.PublishUpdates(db);
	auto known = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(known.ride); CHECK(known.ride->estimated_wait == 55);
}

TEST_CASE("Schedule active selection gives timed OD routes priority", "[passenger][schedule][active]")
{
	Fixture f; auto *timed = f.Train({Stop(A, 500, 0), Stop(D, 500, 0)});
	auto *unknown = f.Train({Stop(A, 10, 0), Stop(D, 10, 0)}); unknown->current_order.Free(); auto &db = f.Build();
	auto *packet = f.Packet(); PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, D));
	CHECK(packet->GetPassengerJourney().service == timed->orders->index); CHECK(packet->GetPassengerJourney().kind == PassengerRideKind::ServiceIntent);
	CHECK(db.GetScheduleDiagnostic().misses == 0);
}

TEST_CASE("Schedule work admission is identical across hits eviction saved DB and peers", "[passenger][schedule][determinism][save]")
{
	Fixture f; auto *ab = f.Train({Stop(A), Stop(B)}); auto *bd = f.Train({Stop(B), Stop(D)});
	ab->current_order.Free(); bd->current_order.Free(); auto &db = f.Build(); GameRandomSeedChecker seeds;
	auto cold = db.PlanEstimated(PAX, A, D, NOW); auto hot = db.PlanEstimated(PAX, A, D, NOW);
	REQUIRE(cold.ride); CHECK(cold.ride == hot.ride); CHECK(cold.work == hot.work);
	db.ClearPolicies(); CHECK(db.PlanEstimated(PAX, A, D, NOW).ride == cold.ride);
	auto wire = db.Save(); PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
	CHECK(peer.PlanEstimated(PAX, A, D, NOW).ride == cold.ride); CHECK(peer.PlanEstimated(PAX, A, D, NOW).work == cold.work); CHECK(db.Save() == wire);
	PassengerJourneyLimits limits; limits.work = cold.work - 1;
	auto limited = db.PlanEstimated(PAX, A, D, NOW, limits); CHECK_FALSE(limited.ride); CHECK(limited.deferred); CHECK(limited.work <= limits.work);
	CHECK(db.PlanEstimated(PAX, A, D, NOW, limits).work == limited.work); CHECK(seeds.Check());
	limits = {}; limits.memory_bytes = 0; CHECK(db.PlanEstimated(PAX, A, D, NOW, limits).deferred);
	limits = {}; limits.labels = 0; CHECK(db.PlanEstimated(PAX, A, D, NOW, limits).deferred);
}

TEST_CASE("Schedule actual shared carriers use reservations splits and saved independent intents", "[passenger][schedule][active][save]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); auto *shared = Vehicle::Create<PeriodicTrain>(v);
	v->current_order.Free(); shared->current_order.Free(); f.Build(); auto *packet = f.Packet(200);
	PassengerJourneyActiveContext context(nullptr, A); REQUIRE(context.PlanWaiting(*packet, PAX, D));
	auto intent = packet->GetPassengerJourney();
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION}; AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	auto *loaded = CargoPacket::Create(); REQUIRE_FALSE(SlTestSaveLoadTable(packet, loaded, GetCargoPacketDesc()).empty()); CHECK(loaded->GetPassengerJourney() == intent);
	v->LoadAt(0); shared->LoadAt(0); auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo; waiting.Append(packet, StationID::Invalid());
	PassengerJourneyLoadingContext loading; std::array next{D};
	CHECK(waiting.Reserve(70, &shared->cargo, next, TileIndex{0}, shared, &loading) == 70);
	CHECK(packet->GetPassengerJourney() == intent); CHECK(shared->cargo.Packets()->front()->GetPassengerJourney().vehicle == shared->index);
	CHECK(waiting.Load(35, &shared->cargo, next, TileIndex{0}, shared, &loading) == 35);
	CHECK(waiting.Load(35, &shared->cargo, next, TileIndex{0}, shared, &loading) == 35);
	CHECK(GetPassengerJourneyTickDiagnostic().schedule_boarded_passengers == 70); CHECK(GetPassengerJourneyTickDiagnostic().boarded_passengers == 0);
	CHECK(waiting.Load(100, &v->cargo, next, TileIndex{0}, v, &loading) == 100);
	CHECK(packet->Count() == 30); CHECK(packet->GetPassengerJourney() == intent);
	CHECK(packet->GetPassengerTransport() == PassengerTransportKind::None);
	REQUIRE(packet->Merge(loaded)); CHECK(packet->Count() == 230);
	auto *different = packet->Split(10); auto changed = different->GetPassengerJourney(); ++changed.generation;
	different->SetPassengerJourney(changed, D); CHECK_FALSE(packet->Merge(different));
}

TEST_CASE("Schedule boarding enforces live restrictions incarnation and generation", "[passenger][schedule][active]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(B), Stop(D)}); v->current_order.Free(); auto &db = f.Build();
	auto plan = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(plan.ride); v->LoadAt(0); CHECK(ValidatePassengerServiceRide(*plan.ride, v, PAX, true));
	SECTION("NoLoad") { v->GetOrder(0)->SetLoadType(OrderLoadType::NoLoad); }
	SECTION("NoUnload") { v->GetOrder(2)->SetUnloadType(OrderUnloadType::NoUnload); }
	SECTION("Forced unload") { v->GetOrder(1)->SetUnloadType(OrderUnloadType::Unload); }
	SECTION("Forced transfer") { v->GetOrder(1)->SetUnloadType(OrderUnloadType::Transfer); }
	SECTION("Capacity removed") { v->cargo_cap = 0; }
	SECTION("Facilities removed") { Station::Get(D)->facilities = {}; }
	SECTION("Vehicle stopped") { v->vehstatus.Set(VehState::Stopped); }
	SECTION("Generation changed") { db.Invalidate(v->orders->index); db.Tick(); }
	SECTION("Incarnation changed") { plan.ride->signature++; }
	SECTION("Wrong occurrence") { plan.ride->boarding_order = 1; }
	CHECK_FALSE(ValidatePassengerServiceRide(*plan.ride, v, PAX, true));
}

TEST_CASE("Schedule transfer constraints survive replanning fallback and packet save", "[passenger][schedule][active][save]")
{
	Fixture f; auto *ab = f.Train({Stop(A), Stop(B)}); auto *bd = f.Train({Stop(B), Stop(D)});
	Station::CreateAtIndex(C, TileIndex{0});
	Station::Get(B)->goods[PAX].CreateData().flows.insert(FlowStat(A, C, 10));
	ab->current_order.Free(); bd->current_order.Free(); auto &db = f.Build(); auto *packet = f.Packet();
	REQUIRE(PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*packet, PAX, D));
	ab->LoadAt(0); PassengerJourneyLoadingContext loading; REQUIRE(loading.CanBoard(*packet, ab)); loading.PrepareBoard(*packet, ab);
	_state_ticks += 100; ab->LoadAt(1); _settings_game.linkgraph.passenger_transfer_penalty = 20;
	PassengerJourneyActiveContext arrived(ab, B); CHECK(arrived.Decide(*packet, PAX, false, OrderUnloadType::UnloadIfPossible) == VehicleCargoList::MoveToAction::Transfer);
	REQUIRE(packet->GetPassengerJourney().kind == PassengerRideKind::EstimatedServiceIntent); CHECK(packet->GetPassengerJourney().service == bd->orders->index);
	CHECK(packet->GetPassengerLegacyHop() == C);
	bd->LoadAt(0); CHECK_FALSE(loading.CanBoard(*packet, bd)); CHECK_FALSE(loading.CanBoard(*packet, ab));
	_state_ticks += 20; CHECK(loading.CanBoard(*packet, bd));
	packet->FallbackPassengerJourney(B, _state_ticks); CHECK_FALSE(loading.CanBoard(*packet, ab));
	CHECK(packet->GetPassengerInterchangeReady() == NOW + 120); CHECK(packet->GetPassengerArrivingService() == ab->orders->index);
	CHECK(db.GetScheduleDiagnostic().misses >= 2);
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION}; AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	auto *loaded = CargoPacket::Create(); REQUIRE_FALSE(SlTestSaveLoadTable(packet, loaded, GetCargoPacketDesc()).empty());
	CHECK(loaded->HasSamePassengerRouting(*packet));
}

TEST_CASE("Schedule expiry and timetable deletion safely trigger another OD policy", "[passenger][schedule][active]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); v->current_order.Free(); auto &db = f.Build(); auto *packet = f.Packet();
	REQUIRE(PassengerJourneyActiveContext(nullptr, A).PlanWaiting(*packet, PAX, D));
	Station::Get(A)->goods[PAX].CreateData().cargo.Append(packet, StationID::Invalid());
	SECTION("Long wait") { _state_ticks += 441; }
	SECTION("Generation") { db.Invalidate(v->orders->index); db.Tick(); _state_ticks++; }
	BeginPassengerJourneyTick(); Station::Get(A)->goods[PAX].data->cargo.RoutePassengerJourneys(PAX, A);
	CHECK(packet->GetPassengerDestination() == D); CHECK(packet->GetPassengerJourney().departure == StateTicks{});
	CHECK(GetPassengerJourneyTickDiagnostic().schedule_expired + GetPassengerJourneyTickDiagnostic().schedule_stale == 1);
}

TEST_CASE("Schedule 8000 station network caches OD policies without physical visit expansion", "[passenger][schedule][scaling]")
{
	Fixture f;
	for (uint16_t i = 0; i < 4000; ++i) f.Train({Stop(StationID{static_cast<uint16_t>(2 * i)}), Stop(StationID{static_cast<uint16_t>(2 * i + 1)})})->current_order.Free();
	auto &db = GetPassengerServiceDatabase(); for (int i = 0; i < 100; ++i) { _state_ticks++; db.Tick(); }
	REQUIRE(db.GetDiagnostic().vehicles == 4000); auto cold = db.PlanEstimated(PAX, A, B, _state_ticks); REQUIRE(cold.ride);
	for (int i = 0; i < 4096; ++i) CHECK(db.PlanEstimated(PAX, A, B, _state_ticks).work == cold.work);
	CHECK(db.GetScheduleDiagnostic().hits == 4096); CHECK(cold.work < 64);
	CHECK(GetPassengerJourneyTickDiagnostic().builds == 0); CHECK(GetPassengerJourneyTickDiagnostic().prediction_executions == 0);
	auto d = db.GetScheduleDiagnostic();
	WARN("Schedule 8000-station benchmark: queries=" << d.hits + d.misses << "; work_per_query=" << cold.work << "; total_query_us=" << d.query_microseconds << "; policy_bytes=" << d.bytes);
}

TEST_CASE("Schedule reads old periodic wire state and unresolved dispatch circuits conservatively", "[passenger][schedule][save][dispatch]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); v->vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
	v->orders->GetScheduledDispatchScheduleSet().emplace_back(); auto &schedule = v->orders->GetDispatchScheduleByIndex(0);
	schedule.SetScheduledDispatchStartTick(NOW); schedule.SetScheduledDispatchDuration(2000);
	schedule.GetScheduledDispatchMutable().push_back({10, 0}); v->GetOrder(0)->SetDispatchScheduleIndex(0);
	Vehicle::Create<PeriodicTrain>(v)->vehicle_flags.Set(VehicleFlag::ScheduledDispatch);
	auto &db = f.Build(); auto plan = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK(plan.ride->estimated_wait == 2000);
	auto wire = db.Save(); PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
	CHECK(peer.PlanEstimated(PAX, A, D, NOW).ride == plan.ride);
	/* Version one had the same record prefix without the final dispatch estimate. */
	wire = LegacyDatabaseWire(wire, 3); wire.resize(wire.size() - 7 * 8); wire[0] = 1; wire.resize(wire.size() - 8); REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
	CHECK(peer.Get(v->orders->index) == nullptr); CHECK(peer.GetDiagnostic().pending != 0);
	peer.Tick(); REQUIRE(peer.Get(v->orders->index) != nullptr);
	CHECK(peer.Get(v->orders->index)->uncertain_dispatch_period == 2000); CHECK(peer.PlanEstimated(PAX, A, D, NOW).ride.has_value());
}

TEST_CASE("Schedule forced alighting replans without boarding the arriving service", "[passenger][schedule][active]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(B), Stop(D)}); auto *next = f.Train({Stop(B), Stop(D)});
	v->current_order.Free(); next->current_order.Free(); auto &db = f.Build();
	auto *packet = f.Packet(); auto plan = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(plan.ride);
	packet->SetPassengerJourney(*plan.ride, D); v->LoadAt(0); PassengerJourneyLoadingContext loading; loading.PrepareBoard(*packet, v);
	v->LoadAt(1); _state_ticks += 100; _settings_game.linkgraph.passenger_transfer_penalty = 20;
	OrderUnloadType unload = GENERATE(OrderUnloadType::Unload, OrderUnloadType::Transfer);
	PassengerJourneyActiveContext arrived(v, B); CHECK(arrived.Decide(*packet, PAX, true, unload) == VehicleCargoList::MoveToAction::Transfer);
	CHECK(packet->GetPassengerDestination() == D); CHECK(packet->GetPassengerJourney().service == next->orders->index);
	CHECK(packet->GetPassengerJourney().not_before == _state_ticks + 20);
}

TEST_CASE("Schedule interchange exclusion distinguishes recycled order list incarnations", "[passenger][schedule][active]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto *packet = f.Packet();
	OrderListID id = v->orders->index; uint64_t incarnation = v->orders->GetCargoDistRouteGeneration();
	packet->SetPassengerInterchange(NOW + 20, id, incarnation);
	v->orders->FreeChain(); v->orders = OrderList::Create(std::vector<Order>{Stop(A), Stop(D)}, v); v->LoadAt(0); f.PublishUpdates(db);
	REQUIRE(v->orders->index == id); REQUIRE(v->orders->GetCargoDistRouteGeneration() != incarnation);
	_state_ticks += 20; PassengerJourneyActiveContext waiting(nullptr, A); REQUIRE(waiting.PlanWaiting(*packet, PAX, D));
	CHECK(PassengerJourneyLoadingContext().CanBoard(*packet, v));
}

TEST_CASE("Timed interchange exclusion remains correct across shared policy cache hits", "[passenger][periodic][schedule][determinism]")
{
	Fixture f; auto *arriving = f.Train({Stop(A, 100, 0), Stop(D, 100, 0)});
	auto *next = f.Train({Stop(A, 100, 0), Stop(B, 50, 0), Stop(D, 150, 0)}); auto &db = f.Build();
	auto unrestricted = db.Plan(PAX, A, D, NOW); REQUIRE(unrestricted.ride); CHECK(unrestricted.ride->service == arriving->orders->index);
	auto excluded = db.Plan(PAX, A, D, NOW, {}, {}, arriving->orders->index);
	REQUIRE(excluded.ride); CHECK_FALSE(excluded.deferred); CHECK(excluded.ride->service == next->orders->index); CHECK(excluded.arrival == NOW + 200);
	db.ClearPolicies();
	auto cold = db.Plan(PAX, A, D, NOW, {}, {}, arriving->orders->index);
	CHECK(cold.ride == excluded.ride); CHECK(cold.arrival == excluded.arrival); CHECK(cold.work == excluded.work);
}

TEST_CASE("Observing an unknown phase preserves estimated boarding until timing publication", "[passenger][periodic][schedule][active]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); v->current_order.Free(); auto &db = f.Build();
	auto plan = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(plan.ride);
	v->LoadAt(0); ObservePassengerPeriodicVehicle(v); db.Tick();
	CHECK(db.IsCurrent(*plan.ride)); CHECK(ValidatePassengerServiceRide(*plan.ride, v, PAX, true));
	CHECK(db.GetPublication(v->orders->index) == PassengerServiceDatabase::Publication::TimingDirty);
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; db.Tick();
	CHECK(db.Get(v->orders->index)->vehicles[0].phase_known);
}

TEST_CASE("Former schedule journeys refresh constraints on a later fallback transfer", "[passenger][schedule][active][fallback]")
{
	Fixture f; auto *old = f.Train({Stop(A), Stop(B)}); auto *v = f.Train({Stop(B), Stop(D)}); f.Build(); auto *packet = f.Packet();
	packet->SetPassengerInterchange(NOW, old->orders->index, old->orders->GetCargoDistRouteGeneration());
	packet->FallbackPassengerJourney(B, NOW); _settings_game.linkgraph.passenger_transfer_penalty = 20;
	PassengerJourneyActiveContext arrived(v, B); REQUIRE(arrived.Decide(*packet, PAX, false, OrderUnloadType::Transfer) == VehicleCargoList::MoveToAction::Transfer);
	CHECK(packet->GetPassengerArrivingService() == v->orders->index); CHECK(packet->GetPassengerInterchangeReady() == NOW + 20);
}

TEST_CASE("Schedule bindings continue from a real implicit stop without forecasting it", "[passenger][schedule][active]")
{
	Fixture f; Order implicit; implicit.MakeImplicit(B); auto *v = f.Train({Stop(A), implicit, Stop(D)});
	EnsurePassengerTestStations(std::vector<Order>{Stop(B)}); v->current_order.Free(); auto &db = f.Build();
	auto plan = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK(db.Get(v->orders->index)->timetable.stops.size() == 2);
	auto *packet = f.Packet(); packet->SetPassengerJourney(*plan.ride, D); v->LoadAt(0);
	PassengerJourneyLoadingContext loading; REQUIRE(loading.CanBoard(*packet, v)); loading.PrepareBoard(*packet, v);
	v->LoadAt(1); v->cur_real_order_index = 2;
	CHECK_FALSE(GetPassengerRouteState(v)); // Forecasters still reject this undeclared current occurrence.
	PassengerJourneyActiveContext arrived(v, B); CHECK(arrived.Decide(*packet, PAX, false, OrderUnloadType::UnloadIfPossible) == VehicleCargoList::MoveToAction::Keep);
	CHECK(packet->GetPassengerJourney().kind == PassengerRideKind::EstimatedBoundService);
}

TEST_CASE("Eight slow direct lines cannot consume the timed transfer candidate budget", "[passenger][periodic][comparison]")
{
	Fixture f;
	std::vector<PeriodicTrain *> direct;
	for (int i = 0; i < 10; ++i) direct.push_back(f.Train({Stop(A, 100, 0), Stop(D, 1000, 0)}));
	auto *feeder = f.Train({Stop(A, 1900, 0), Stop(B, 100, 0)});
	f.Train({Stop(B, 1900, 100), Stop(D, 100, 0)});
	auto &db = f.Build();
	for (size_t count : {size_t{1}, size_t{8}, size_t{16}}) {
		PassengerJourneyLimits limits; limits.candidates = count;
		auto early = db.Plan(PAX, A, D, NOW, limits);
		REQUIRE(early.ride); CHECK_FALSE(early.deferred);
		CHECK(early.ride->service == feeder->orders->index); CHECK(early.arrival == NOW + 200);
		REQUIRE(early.route.size() == 2); CHECK(early.route.front().alighting == B);
		auto late = db.Plan(PAX, A, D, NOW + 1, limits);
		REQUIRE(late.ride); CHECK_FALSE(late.deferred);
		CHECK(late.ride->service == direct.front()->orders->index); CHECK(late.arrival == NOW + 2100);
	}
	auto paired = ComparePassengerPlanners(db, PAX, A, D, NOW); REQUIRE(paired.periodic.ride); REQUIRE_FALSE(paired.legacy_route.empty()); REQUIRE(paired.legacy_found); CHECK(paired.periodic.arrival == paired.legacy_arrival);
}

TEST_CASE("Periodic timed search retains a nominally slower express with an earlier phase", "[passenger][periodic]")
{
	Fixture f;
	for (int i = 0; i < 10; ++i) f.Train({Stop(A, 1000, 500), Stop(D, 1, 0)});
	auto *express = f.Train({Stop(A, 1000, 0), Stop(D, 100, 0)});
	auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW);
	REQUIRE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.ride->service == express->orders->index);
	CHECK(plan.arrival == NOW + 100);
}

TEST_CASE("Periodic local express and multiple interchange paths compete by actual arrival", "[passenger][periodic]")
{
	Fixture f;
	auto *local = f.Train({Stop(A, 900, 0), Stop(B, 100, 0), Stop(D, 900, 0)});
	f.Train({Stop(B, 900, 100), Stop(C, 50, 0)});
	f.Train({Stop(C, 900, 160), Stop(D, 40, 0)});
	auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW);
	REQUIRE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.arrival == NOW + 200);
	CHECK(plan.transfers == 2); REQUIRE(plan.route.size() == 3);
	CHECK(plan.route[0].service == local->orders->index); CHECK(plan.route[0].alighting == B); CHECK(plan.route[1].alighting == C);
}

TEST_CASE("Periodic exhausted search and resource truncation have distinct diagnoses", "[passenger][periodic][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); f.Train({Stop(B), Stop(D)}); auto &db = f.Build();
	PassengerJourneyLimits limits;
	SECTION("Work") { limits.work = 1; }
	SECTION("Zero work") { limits.work = 0; }
	SECTION("Labels") { limits.labels = 1; }
	SECTION("Memory") { limits.memory_bytes = 256; }
	SECTION("Zero memory") { limits.memory_bytes = 0; }
	SECTION("Transfer safety ceiling") { limits.max_transfers = 0; }
	auto cold = db.Plan(PAX, A, D, NOW, limits), warm = db.Plan(PAX, A, D, NOW, limits);
	CHECK_FALSE(cold.ride); CHECK(cold.deferred); CHECK(cold.reason != PassengerPeriodicPlan::Reason::NoCandidate);
	CHECK(cold.reason == warm.reason); CHECK(cold.work == warm.work); CHECK(cold.work <= limits.work);
	CHECK(cold.memory <= limits.memory_bytes); CHECK(cold.labels <= limits.labels);
}

TEST_CASE("Periodic candidate capacity is observed without certifying a false absence", "[passenger][periodic]")
{
	Fixture f; for (int i = 0; i < 12; ++i) f.Train({Stop(A), Stop(D)}); auto &db = f.Build();
	PassengerJourneyLimits limits; limits.candidates = 8;
	auto plan = db.Plan(PAX, A, D, NOW, limits); REQUIRE(plan.ride); CHECK_FALSE(plan.deferred);
	CHECK(plan.candidates == 8); CHECK(plan.observations[static_cast<size_t>(PassengerPeriodicPlan::Reason::Candidates)]);
}

TEST_CASE("Periodic overdue loading aligns with snapshot remaining dwell rather than another circuit", "[passenger][periodic][comparison]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); train->current_order_time = 40; train->lateness_counter = 500;
	auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW);
	REQUIRE(plan.ride); CHECK(plan.ride->departure == NOW); CHECK(plan.arrival == NOW + 100);

}

TEST_CASE("Periodic capture supports an actual waypoint phase without inventing overdue travel", "[passenger][periodic]")
{
	Fixture f; Order waypoint; waypoint.MakeGoToWaypoint(StationID{5}); waypoint.SetTravelTime(40); waypoint.SetTravelTimetabled(true);
	auto *train = f.Train({Stop(A, 100, 0), waypoint, Stop(D, 60, 0)});
	train->cur_real_order_index = train->cur_implicit_order_index = 1; train->current_order = waypoint;
	train->current_order_time = 10;
	SECTION("Known current travel") {
		auto &db = f.Build(); auto *service = db.Get(train->orders->index); REQUIRE(service); CHECK(service->vehicles[0].phase_known);
		CHECK(service->vehicles[0].base == NOW - 10);
		auto plan = db.Plan(PAX, D, A, NOW); REQUIRE(plan.ride); CHECK(plan.ride->departure == NOW + 90);
	}
	SECTION("Elapsed travel exceeds declared arrival") {
		train->current_order_time = 41; auto &db = f.Build(); REQUIRE(db.Get(train->orders->index));
		CHECK_FALSE(db.Get(train->orders->index)->vehicles[0].phase_known);
		train->LoadAt(2); ObservePassengerPeriodicVehicle(train);
		_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; db.Tick(); CHECK(db.Get(train->orders->index)->vehicles[0].phase_known);
	}
}

TEST_CASE("Actual explicit arrival recaptures a drifted periodic phase", "[passenger][periodic][determinism]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build();
	auto generation = db.Get(train->orders->index)->generation;
	_state_ticks += 500; train->LoadAt(1); ObservePassengerPeriodicVehicle(train);
	CHECK(db.Get(train->orders->index)->generation == generation); f.PublishUpdates(db);
	CHECK(db.Get(train->orders->index)->generation > generation);
	auto plan = db.Plan(PAX, D, A, _state_ticks); REQUIRE(plan.ride); CHECK(plan.ride->departure == _state_ticks + 10);
}

TEST_CASE("Paired periodic snapshot diagnostic cannot change publication packets or RNG", "[passenger][periodic][comparison][save][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); f.Train({Stop(B), Stop(D)}); auto &db = f.Build();

	auto saved = db.Save(); auto before = db.GetDiagnostic(); auto *packet = f.Packet(); auto mode = packet->GetPassengerRoutingMode();
	GameRandomSeedChecker seeds;
	auto paired = ComparePassengerPlanners(db, PAX, A, D, NOW);
	REQUIRE(paired.periodic.ride); REQUIRE_FALSE(paired.legacy_route.empty()); REQUIRE(paired.legacy_found);
	CHECK(paired.periodic.arrival == paired.legacy_arrival); CHECK(paired.periodic.cost == paired.legacy_cost);
	CHECK(db.Save() == saved); CHECK(db.GetDiagnostic().searches == before.searches);
	CHECK(packet->GetPassengerRoutingMode() == mode); CHECK(_state_ticks == NOW); CHECK(seeds.Check());
}

TEST_CASE("Periodic passenger denominator excludes cargo-only order lists", "[passenger][periodic]")
{
	Fixture f; f.Train({Stop(A), Stop(D)});
	auto *freight = f.Train({Stop(A), Stop(D)}); freight->cargo_type = CargoType{1};
	AutoRestoreBackup<CargoClasses> freight_class{CargoSpec::Get(CargoType{1})->classes, CargoClasses{CargoClass::Bulk}};
	auto *unknown = f.Train({Stop(A), Stop(D)}); unknown->GetOrder(1)->SetTravelTimetabled(false);
	auto &db = f.Build(); auto census = db.GetDiagnostic();
	CHECK(census.orderlists == 3); CHECK(census.passenger_orderlists == 2); CHECK(census.complete_timetables == 1);
	CHECK(census.declared_complete_timetables == 1); CHECK(census.published_passenger == 2); CHECK(census.compile_success == 1); CHECK(census.compile_failure == 1);
	CHECK(census.compile_failures[static_cast<size_t>(PassengerPeriodicTimetable::Failure::Travel)] == 1);
}
TEST_CASE("A tick ending during facility capture retries before publishing Unknown", "[passenger][periodic][determinism]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = GetPassengerServiceDatabase();
	const size_t task_work = 64 + 2 * (NUM_CARGO + 32) + 3 * NUM_CARGO + 32 + 4;
	db.Tick(task_work); CHECK(db.Get(train->orders->index) == nullptr);
	CHECK(db.GetPublication(train->orders->index) == PassengerServiceDatabase::Publication::Dirty);
	auto wire = db.Save(); PassengerServiceDatabase joined; REQUIRE(joined.Load(wire)); joined.FinishLoad(true);
	f.Build(); for (int i = 0; i < 128 && joined.IsBuilding(); ++i) joined.Tick(); CHECK(db.Save() == joined.Save());
	auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK_FALSE(plan.deferred);
	CHECK(db.GetDiagnostic().rejected_work == 1);
}

TEST_CASE("Periodic search obeys load unload and mandatory interchange permissions", "[passenger][periodic]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(B), Stop(D)});
	SECTION("No load") {
		train->GetOrder(0)->SetLoadType(OrderLoadType::NoLoad); auto &db = f.Build();
		auto plan = db.Plan(PAX, A, D, NOW); CHECK_FALSE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.reason == PassengerPeriodicPlan::Reason::NoCandidate);
	}
	SECTION("No unloading destination") {
		train->GetOrder(2)->SetUnloadType(OrderUnloadType::NoUnload); auto &db = f.Build();
		auto plan = db.Plan(PAX, A, D, NOW); CHECK_FALSE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.reason == PassengerPeriodicPlan::Reason::NoAlighting);
	}
	SECTION("Forced transfer to an express") {
		train->GetOrder(1)->SetUnloadType(OrderUnloadType::Transfer); f.Train({Stop(B, 500, 120), Stop(D, 10, 0)});
		auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK_FALSE(plan.deferred);
		CHECK(plan.transfers == 1); CHECK(plan.ride->alighting == B);
	}
}
TEST_CASE("A phase-only interchange gap is distinct from disconnected published components", "[passenger][periodic]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); auto *unknown = f.Train({Stop(B), Stop(C)}); unknown->current_order.Free();
	f.Train({Stop(C), Stop(D)}); auto &db = f.Build();
	auto plan = db.Plan(PAX, A, D, NOW); CHECK_FALSE(plan.ride); CHECK_FALSE(plan.deferred);
	CHECK(plan.reason == PassengerPeriodicPlan::Reason::UnknownPhase);
	unknown->LoadAt(0); ObservePassengerPeriodicVehicle(unknown);
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; db.Tick();
	plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride); CHECK_FALSE(plan.deferred);
}
TEST_CASE("Periodic journeys can use more than the initial four transfer rounds", "[passenger][periodic]")
{
	Fixture f;
	for (uint16_t i = 0; i < 12; ++i) f.Train({Stop(StationID{i}, 1000, i * 10), Stop(StationID{static_cast<uint16_t>(i + 1)}, 10, 0)});
	auto &db = f.Build(); auto plan = db.Plan(PAX, A, StationID{12}, NOW);
	REQUIRE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.transfers == 11); CHECK(plan.arrival == NOW + 120);
}

TEST_CASE("Arrival phase comparison normalizes cycles across negative state ticks", "[passenger][periodic][save][determinism]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto generation = db.Get(train->orders->index)->generation;
	StateTicksDelta delta{-64100}; db.ShiftTicks(delta); _state_ticks += delta;
	_state_ticks += 220; train->LoadAt(0); ObservePassengerPeriodicVehicle(train);
	REQUIRE(db.Get(train->orders->index)); CHECK(db.Get(train->orders->index)->generation == generation);
	auto plan = db.Plan(PAX, A, D, _state_ticks); REQUIRE(plan.ride); CHECK(plan.ride->departure == _state_ticks + 10);
}
TEST_CASE("A new periodic query can board a live vehicle overdue since phase publication", "[passenger][periodic][comparison][determinism]")
{
	Fixture f; auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto saved = db.Save();
	_state_ticks += 40; train->current_order_time = 40; train->lateness_counter = 400;
	auto current = db.Plan(PAX, A, D, _state_ticks); REQUIRE(current.ride); CHECK_FALSE(current.deferred);
	CHECK(current.ride->departure == _state_ticks); CHECK(current.arrival == _state_ticks + 100);
	CHECK(db.Save() == saved);
	auto future = db.Plan(PAX, A, D, _state_ticks + 1); REQUIRE(future.ride); CHECK(future.ride->departure == _state_ticks + 220);
	auto paired = ComparePassengerPlanners(db, PAX, A, D, _state_ticks);
	REQUIRE(paired.legacy_found); CHECK(paired.periodic.arrival == paired.legacy_arrival);
}

TEST_CASE("A slower prefix survives when a common connection restores the canonical tie", "[passenger][periodic]")
{
	Fixture f;
	f.Train({Stop(A, 900, 10), Stop(B, 40, 0)});
	auto *local = f.Train({Stop(A, 900, 0), Stop(B, 60, 0)});
	f.Train({Stop(B, 900, 70), Stop(D, 30, 0)});
	auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW);
	REQUIRE(plan.ride); CHECK_FALSE(plan.deferred); CHECK(plan.arrival == NOW + 100);
	CHECK(plan.ride->service == local->orders->index); CHECK(plan.ride->departure == NOW);
}

TEST_CASE("A fleet larger than a full phase capture tick cannot stall publication", "[passenger][periodic][scaling][determinism]")
{
	Fixture f; std::vector<Order> orders;
	for (uint16_t i = 0; i < 100; ++i) orders.push_back(Stop(StationID{i}));
	auto *first = f.Train(orders);
	for (int i = 1; i < 150; ++i) Vehicle::Create<PeriodicTrain>(first);
	auto &db = f.Build(); const auto *service = db.Get(first->orders->index); REQUIRE(service);
	CHECK(service->vehicles.size() == 150);
	size_t known = std::count_if(service->vehicles.begin(), service->vehicles.end(), [](const auto &v) { return v.phase_known; });
	CHECK(known > 0); CHECK(known < 150); CHECK(db.GetDiagnostic().phase_capture_work > 0);
	CHECK(db.GetPublication(first->orders->index) == PassengerServiceDatabase::Publication::Published);
	db.Tick(); CHECK_FALSE(db.IsBuilding());
	auto plan = db.Plan(PAX, A, StationID{99}, NOW); REQUIRE(plan.ride); CHECK_FALSE(plan.deferred);
}


TEST_CASE("Shared policies remain resident through thousands of unchanged ticks", "[passenger][periodic][schedule][residency][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); GameRandomSeedChecker checker;
	auto timed = db.Plan(PAX, A, D, NOW), warm = db.Plan(PAX, A, D, NOW);
	auto estimated = db.PlanEstimated(PAX, A, D, NOW);
	auto saved = db.Save(); auto builds = db.GetDiagnostic().builds;
	for (int i = 0; i < 12000; ++i) { ++_state_ticks; db.Tick(); }
	CHECK(db.GetDiagnostic().builds == builds); CHECK(db.Save() == saved);
	CHECK(db.Plan(PAX, A, D, NOW).work == timed.work);
	CHECK(db.PlanEstimated(PAX, A, D, NOW).work == estimated.work);
	CHECK(db.GetDiagnostic().policy_hits == 2); CHECK(db.GetScheduleDiagnostic().hits == 1);
	CHECK(db.GetDiagnostic().policy_clears == 0); CHECK(timed.ride == warm.ride); CHECK(checker.Check());
}

TEST_CASE("An unrelated publication preserves both policy kinds and their logical work", "[passenger][periodic][schedule][residency][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(B)}); auto *unrelated = f.Train({Stop(C), Stop(D)}); auto &db = f.Build();
	auto timed = db.Plan(PAX, A, B, NOW); auto estimated = db.PlanEstimated(PAX, A, B, NOW); auto revision = db.GetDiagnostic().published_revision;
	unrelated->GetOrder(1)->SetTravelTime(500); db.TimetableChanged(unrelated->orders->index); f.PublishUpdates(db);
	CHECK(db.GetDiagnostic().published_revision > revision);
	auto hit = db.Plan(PAX, A, B, NOW); CHECK(hit.work == timed.work); REQUIRE(hit.ride); CHECK(hit.ride->service == timed.ride->service); CHECK(hit.ride->generation == timed.ride->generation);
	auto scheduled_hit = db.PlanEstimated(PAX, A, B, NOW); CHECK(scheduled_hit.work == estimated.work); CHECK(scheduled_hit.ride == estimated.ride);
	CHECK(db.GetDiagnostic().policy_hits == 1); CHECK(db.GetScheduleDiagnostic().hits == 1); CHECK(db.GetDiagnostic().policy_stale == 0);
}

TEST_CASE("A new shortcut invalidates policies which never used its service", "[passenger][periodic][schedule][residency][determinism]")
{
	Fixture f; f.Train({Stop(A, 400, 0), Stop(B, 100, 0)}); f.Train({Stop(B, 400, 100), Stop(D, 100, 0)}); auto &db = f.Build();
	StateTicks ready = NOW + 2 * PassengerServiceDatabase::TIMETABLE_EPOCH;
	auto before = db.Plan(PAX, A, D, ready); auto scheduled_before = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(before.ride); REQUIRE(scheduled_before.ride);
	auto *shortcut = f.Train({Stop(A, 100, 0), Stop(D, 1, 0)}); db.Tick();
	CHECK(db.Plan(PAX, A, D, ready).ride == before.ride); f.PublishUpdates(db);
	auto after = db.Plan(PAX, A, D, ready); auto scheduled_after = db.PlanEstimated(PAX, A, D, NOW);
	REQUIRE(after.ride); REQUIRE(scheduled_after.ride); CHECK(after.ride->service == shortcut->orders->index);
	CHECK(after.arrival < before.arrival); CHECK(scheduled_after.cost < scheduled_before.cost); CHECK(db.GetDiagnostic().policy_stale == 2);
	db.ClearPolicies(); auto cold = db.Plan(PAX, A, D, ready); auto schedule_cold = db.PlanEstimated(PAX, A, D, NOW);
	CHECK(after.ride == cold.ride); CHECK(after.work == cold.work); CHECK(schedule_cold.ride == scheduled_after.ride); CHECK(schedule_cold.work == scheduled_after.work);
}

TEST_CASE("A frozen publication batch survives additional edits and a cold joining peer", "[passenger][periodic][schedule][residency][save][determinism]")
{
	Fixture f; auto *one = f.Train({Stop(A), Stop(B)}); auto *two = f.Train({Stop(C), Stop(D)}); auto &db = f.Build();
	auto generation = db.Get(one->orders->index)->generation;
	one->GetOrder(1)->SetTravelTime(200); two->GetOrder(1)->SetTravelTime(300);
	db.TimetableChanged(one->orders->index); db.TimetableChanged(two->orders->index);
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; db.Tick(500);
	CHECK(db.Get(one->orders->index)->generation == generation); REQUIRE(db.GetDiagnostic().staged == 1);
	one->GetOrder(1)->SetTravelTime(400); db.TimetableChanged(one->orders->index);
	auto wire = db.Save(); PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true); CHECK(peer.Save() == wire);
	for (int i = 0; i < 8; ++i) { ++_state_ticks; db.Tick(500); peer.Tick(500); CHECK(db.Save() == peer.Save()); }
	CHECK(db.Get(one->orders->index)->timetable.period == 320); CHECK(db.GetPublication(one->orders->index) == PassengerServiceDatabase::Publication::TimingDirty);
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH;
	for (int i = 0; i < 8; ++i) { db.Tick(500); peer.Tick(500); CHECK(db.Save() == peer.Save()); }
	CHECK(db.Get(one->orders->index)->timetable.period == 520);
	auto hot = db.Plan(PAX, A, B, NOW); auto cold = peer.Plan(PAX, A, B, NOW); CHECK(hot.ride == cold.ride); CHECK(hot.work == cold.work);
}

TEST_CASE("Shared LRU evicts across Timed and Schedule-aware without changing routes", "[passenger][periodic][schedule][residency][scaling][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto timed = db.Plan(PAX, A, D, NOW); auto schedule = db.PlanEstimated(PAX, A, D, NOW);
	std::vector<Order> stations; for (uint16_t i = 4; i < 5004; ++i) stations.push_back(Stop(StationID{i})); EnsurePassengerTestStations(stations);
	for (uint16_t i = 4; i < 5004; ++i) {
		if (i % 2) db.Plan(PAX, A, StationID{i}, NOW); else db.PlanEstimated(PAX, A, StationID{i}, NOW);
	}
	auto d = db.GetDiagnostic(); CHECK(d.policy_entries <= PassengerServiceDatabase::POLICY_ENTRIES); CHECK(d.policy_bytes <= PassengerServiceDatabase::POLICY_BYTES); CHECK(d.policy_evictions > 0);
	CHECK(db.Plan(PAX, A, D, NOW).ride == timed.ride); CHECK(db.Plan(PAX, A, D, NOW).work == timed.work);
	CHECK(db.PlanEstimated(PAX, A, D, NOW).ride == schedule.ride); CHECK(db.PlanEstimated(PAX, A, D, NOW).work == schedule.work);
	WARN("Shared LRU: evictions=" << d.policy_evictions << "; eviction_us=" << d.policy_eviction_microseconds << "; validation_us=" << d.policy_validation_microseconds << "; entries=" << d.policy_entries << "; bytes=" << d.policy_bytes);
}

TEST_CASE("Structural edits block real boarding while the old publication remains readable", "[passenger][periodic][schedule][residency][active]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto plan = db.Plan(PAX, A, D, NOW); REQUIRE(plan.ride);
	auto generation = db.Get(v->orders->index)->generation;
	v->GetOrder(0)->SetLoadType(OrderLoadType::NoLoad); db.Invalidate(v->orders->index);
	CHECK(db.Get(v->orders->index)->generation == generation); CHECK_FALSE(db.IsCurrent(*plan.ride)); CHECK_FALSE(ValidatePassengerServiceRide(*plan.ride, v, PAX, true));
	db.Tick(); CHECK(db.Get(v->orders->index)->generation == generation); f.PublishUpdates(db);
	CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride); CHECK_FALSE(db.PlanEstimated(PAX, A, D, NOW).ride);
}


TEST_CASE("Disabling shared residency preserves work outcomes and saved publication", "[passenger][periodic][schedule][residency][save][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(B), Stop(D)}); f.Train({Stop(B, 80, 0), Stop(D, 10, 0)}); auto &db = f.Build();
	auto wire = db.Save(); PassengerServiceDatabase cold; REQUIRE(cold.Load(wire)); cold.FinishLoad(true); cold.SetPolicyCacheEnabled(false);
	for (size_t work : {size_t{1}, size_t{10}, size_t{100}, size_t{65536}}) for (int i = 0; i < 64; ++i) {
		PassengerJourneyLimits limits; limits.work = work; StateTicks ready = NOW + i * 17;
		auto timed = db.Plan(PAX, A, D, ready, limits); auto uncached = cold.Plan(PAX, A, D, ready, limits);
		CHECK(timed.ride == uncached.ride); CHECK(timed.route == uncached.route); CHECK(timed.work == uncached.work); CHECK(timed.reason == uncached.reason); CHECK(timed.deferred == uncached.deferred); CHECK(timed.memory == uncached.memory);
		auto schedule = db.PlanEstimated(PAX, A, D, ready, limits); auto fresh = cold.PlanEstimated(PAX, A, D, ready, limits);
		CHECK(schedule.ride == fresh.ride); CHECK(schedule.work == fresh.work); CHECK(schedule.cost == fresh.cost); CHECK(schedule.deferred == fresh.deferred);
	}
	CHECK(db.Save() == wire); CHECK(cold.Save() == wire); CHECK(cold.GetDiagnostic().policy_entries == 0);
}


TEST_CASE("Deleted station and vehicle IDs cannot board through a resident old publication", "[passenger][periodic][schedule][residency][active][save]")
{
	Fixture f; auto *v = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto timed = db.Plan(PAX, A, D, NOW); auto schedule = db.PlanEstimated(PAX, A, D, NOW); REQUIRE(timed.ride); REQUIRE(schedule.ride);
	SECTION("Station pool removal") {
		db.Invalidate(); _station_pool.CleanPool();
		CHECK_FALSE(ValidatePassengerServiceRide(*timed.ride, v, PAX, true)); CHECK_FALSE(ValidatePassengerServiceRide(*schedule.ride, v, PAX, true));
		CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride); CHECK_FALSE(db.PlanEstimated(PAX, A, D, NOW).ride);
	}
	SECTION("Vehicle and order list deletion") {
		VehicleID id = v->index; OrderListID service = v->orders->index;
		db.Invalidate(service); v->orders->FreeChain(); v->orders = nullptr; delete v;
		CHECK(Vehicle::GetIfValid(id) == nullptr); CHECK_FALSE(db.IsCurrent(*timed.ride)); CHECK_FALSE(db.IsCurrent(*schedule.ride));
		CHECK_FALSE(ValidatePassengerServiceRide(*timed.ride, Vehicle::GetIfValid(id), PAX, true));
		/* Planning and serialization use copied IDs and offsets, never freed pointers. */
		db.Plan(PAX, A, D, NOW); db.PlanEstimated(PAX, A, D, NOW); auto wire = db.Save(); PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
		f.PublishUpdates(db); _state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH; for (int i = 0; i < 128 && peer.IsBuilding(); ++i) peer.Tick();
		CHECK(db.Get(service) == nullptr); CHECK(peer.Get(service) == nullptr); CHECK_FALSE(db.Plan(PAX, A, D, NOW).ride); CHECK_FALSE(db.PlanEstimated(PAX, A, D, NOW).ride);
	}
}


TEST_CASE("A local disabled policy cache stays disabled across loading and joining", "[passenger][periodic][schedule][residency][save][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto wire = db.Save();
	db.SetPolicyCacheEnabled(false); REQUIRE(db.Load(wire)); db.FinishLoad(true);
	for (int i = 0; i < 3; ++i) { REQUIRE(db.Plan(PAX, A, D, NOW).ride); REQUIRE(db.PlanEstimated(PAX, A, D, NOW).ride); }
	CHECK(db.GetDiagnostic().policy_entries == 0); CHECK(db.GetDiagnostic().policy_hits == 0); CHECK(db.GetScheduleDiagnostic().hits == 0); CHECK(db.Save() == wire);
}


TEST_CASE("A staging memory boundary remains bounded and loadable during deletions", "[passenger][periodic][residency][save][scaling][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); auto wire = db.Save();
	wire.resize(wire.size() - 7 * 8); // Replace the empty version-three staging suffix.
	auto put = [&](uint64_t value) { for (uint i = 0; i < 8; ++i) { wire.push_back(value & 255); value >>= 8; } };
	put(1); put(NOW.base()); put(0); put(0); // Published revision, batch tick, publishing, bootstrap.
	put(3); for (uint16_t id : {60000, 60001, 60002}) { put(id); put(NOW.base()); }
	put(0); put(32); // Unsafe set and staged records.
	for (uint16_t index = 0; index < 32; ++index) {
		put(1000 + index); put(1); put(1); put(1);
		size_t stops = index == 31 ? 4062 : 4096; put(stops);
		for (size_t i = 0; i < stops; ++i) { put(A.base()); put(0); put(0); put(0); put(1); put(1); put(0); }
		put(0); put(0); put(0); put(0); put(0); put(0); put(0); put(NOW.base()); // Fleet, dispatch fields, estimate and staged age.
	}
	REQUIRE(db.Load(wire)); db.FinishLoad(true);
	REQUIRE(db.GetDiagnostic().staged_bytes == PassengerServiceDatabase::MAX_BYTES - 1024);
	db.Tick(); CHECK(db.GetDiagnostic().staged_bytes <= PassengerServiceDatabase::MAX_BYTES);
	auto partial = db.Save(); PassengerServiceDatabase peer; REQUIRE(peer.Load(partial)); peer.FinishLoad(true); CHECK(peer.Save() == partial);
	for (int i = 0; i < 16 && db.IsBuilding(); ++i) { db.Tick(); peer.Tick(); CHECK(db.Save() == peer.Save()); CHECK(db.GetDiagnostic().staged_bytes <= PassengerServiceDatabase::MAX_BYTES); }
	CHECK_FALSE(db.IsBuilding()); CHECK_FALSE(peer.IsBuilding());
}
TEST_CASE("Depot entry without a wait preserves full cycles and queued saved publication", "[passenger][periodic][operational][depot][save][determinism]")
{
	Fixture f;
	Order depot; depot.MakeGoToDepot(DepotID{0}, {OrderDepotTypeFlag::PartOfOrders});
	depot.SetTravelTime(20); depot.SetTravelTimetabled(true);
	depot.SetWaitTime(77); // An unconfirmed raw value is not a depot waiting instruction.
	CHECK_FALSE(depot.IsWaitTimetabled());
	auto *train = f.Train({depot, Stop(A, 120, 10), depot, Stop(D, 40, 5)});
	train->LoadAt(1);
	SECTION("Known cycles include both depot travels, with no invented waiting") {
		auto &db = f.Build(); auto *service = db.Get(train->orders->index); REQUIRE(service);
		REQUIRE(service->timetable.period == 215); REQUIRE(service->timetable.stops.size() == 2);
		CHECK(service->timetable.stops[0].order == 1); CHECK(service->timetable.stops[1].order == 3);
		GameRandomSeedChecker seed;
		for (int64_t cycle : {int64_t{0}, int64_t{1}, int64_t{1000000}}) {
			auto ready = NOW + cycle * 215;
			auto cold = db.Plan(PAX, A, D, ready), warm = db.Plan(PAX, A, D, ready);
			REQUIRE(cold.ride); CHECK(cold.ride->departure == ready + 10); CHECK(cold.arrival == ready + 70);
			CHECK(cold.ride == warm.ride); CHECK(cold.work == warm.work);
		}
		auto wire = db.Save(); PassengerServiceDatabase joined; REQUIRE(joined.Load(wire)); joined.FinishLoad(true);
		CHECK(joined.Save() == wire);
	}
	SECTION("An actual downstream arrival ignores the unconfirmed depot wait") {
		train->LoadAt(3); auto &db = f.Build();
		auto plan = db.Plan(PAX, D, A, NOW); REQUIRE(plan.ride);
		CHECK(plan.ride->departure == NOW + 5); CHECK(plan.arrival == NOW + 145);
	}
	SECTION("A formerly untimed saved depot record refreshes at a real loading event") {
		train->GetOrder(2)->SetTravelTimetabled(false);
		auto &db = f.Build(); REQUIRE(db.Get(train->orders->index)->timetable.period == 0);
		/* Reproduce the old published empty record with fully known live orders.
		 * A setter alone does not invoke any invalidation or timetable command. */
		train->GetOrder(2)->SetTravelTimetabled(true);
		auto wire = db.Save(); auto generation = db.Get(train->orders->index)->generation;
		REQUIRE(db.Load(wire)); db.FinishLoad(true); CHECK(db.Save() == wire);
		ObservePassengerPeriodicVehicle(train); db.Tick();
		CHECK(db.Get(train->orders->index)->generation == generation);
		CHECK(db.Get(train->orders->index)->timetable.period == 0);
		auto queued = db.Save(); REQUIRE(queued != wire);
		PassengerServiceDatabase joined; REQUIRE(joined.Load(queued)); joined.FinishLoad(true);
		CHECK(joined.Save() == queued);
		_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH;
		for (int i = 0; i < 128 && (db.IsBuilding() || joined.IsBuilding()); ++i) { db.Tick(); joined.Tick(); CHECK(db.Save() == joined.Save()); }
		REQUIRE_FALSE(db.IsBuilding()); REQUIRE_FALSE(joined.IsBuilding());
		REQUIRE(db.Get(train->orders->index)->timetable.period == 215);
		REQUIRE(db.Plan(PAX, A, D, _state_ticks).ride);
		auto published = db.Save(); ObservePassengerPeriodicVehicle(train); db.Tick(); CHECK(db.Save() == published);
	}
}

TEST_CASE("Precomputed active journeys bypass OD admission and survive partial-build saves", "[passenger][precomputed][active][determinism]")
{
	Fixture f;
	_settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	auto &db = f.Build();
	for (int i = 0; i < 1000; ++i) db.TickPrecomputed();
	const auto timed_searches = db.GetDiagnostic().searches;
	PassengerJourneyActiveContext context(nullptr, A);
	for (int i = 0; i < 100; ++i) {
		auto *packet = f.Packet();
		REQUIRE(context.PlanWaiting(*packet, PAX, B));
		CHECK(packet->GetPassengerJourney().alighting == D);
		CHECK(packet->GetPassengerJourney().boarding_order == 0);
		PassengerJourneyLoadingContext loading;
		CHECK(loading.CanBoard(*packet, train, PAX));
		delete packet;
	}
	CHECK(GetPassengerJourneyTickDiagnostic().admission.accepted == 0);
	CHECK(GetPassengerJourneyTickDiagnostic().admission.used == 0);
	CHECK(db.GetDiagnostic().searches == timed_searches);
	CHECK(db.GetPrecomputedDiagnostic(PAX).hits == 1);
	CHECK(GetPassengerJourneyTickDiagnostic().query_cache_hits == 99);
	auto wire = db.Save(); REQUIRE(wire[0] == 9);
	PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
	CHECK(peer.Save() == wire);
	CHECK(peer.PlanPrecomputed(PAX, A, D, NOW).ride == db.PlanPrecomputed(PAX, A, D, NOW).ride);
	/* A different transfer setting starts a new table, then stop inside a row. */
	_settings_game.linkgraph.passenger_transfer_penalty = 17;
	db.TickPrecomputed(1); wire = db.Save();
	REQUIRE(peer.Load(wire)); peer.FinishLoad(true); CHECK(peer.Save() == wire);
	for (size_t i = 0; i < 1000; ++i) {
		CHECK(peer.PlanPrecomputed(PAX, A, D, NOW).state == db.PlanPrecomputed(PAX, A, D, NOW).state);
		CHECK(peer.PlanPrecomputed(PAX, A, D, NOW).ride == db.PlanPrecomputed(PAX, A, D, NOW).ride);
		peer.TickPrecomputed(1); db.TickPrecomputed(1);
	}
	CHECK(peer.Save() == db.Save());
}

TEST_CASE("Precomputed frozen build progresses during a pending sweep and replays saved input", "[passenger][precomputed][generation][save]")
{
	Fixture f;
	f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	auto &db = f.Build();
	_settings_game.linkgraph.passenger_journey_precomputed = true;
	db.TickPrecomputed(1);
	db.Invalidate();
	REQUIRE(db.IsBuilding());
	for (int i = 0; i < 13; ++i) db.TickPrecomputed(1);
	REQUIRE(db.GetPrecomputedDiagnostic(PAX).operations == 14);
	auto wire = db.Save();
	PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
	REQUIRE(peer.Save() == wire);
	for (int i = 0; i < 1000; ++i) {
		db.TickPrecomputed(1); peer.TickPrecomputed(1);
		CHECK(peer.GetPrecomputedDiagnostic(PAX).destinations == db.GetPrecomputedDiagnostic(PAX).destinations);
	}
	CHECK(db.GetPrecomputedDiagnostic(PAX).destinations == 4);
	CHECK_FALSE(db.PlanPrecomputed(PAX, A, D, NOW).ride); // Real boarding remains unsafe until publication.
	CHECK(peer.Save() == db.Save());
	f.PublishUpdates(db);
	REQUIRE(db.PlanPrecomputed(PAX, A, D, _state_ticks).ride);
}

TEST_CASE("Precomputed long builds retain a complete epoch before cost refresh", "[passenger][precomputed][generation]")
{
	Fixture f;
	auto *train = f.Train({Stop(A), Stop(D)});
	auto &db = f.Build();
	_settings_game.linkgraph.passenger_journey_precomputed = true;
	db.TickPrecomputed(1);
	train->orders->GetOrderAt(1)->SetWaitTime(11);
	db.TimetableChanged(train->orders->index);
	f.PublishUpdates(db);
	const auto frozen = db.PlanPrecomputed(PAX, A, D, _state_ticks); REQUIRE(frozen.ride);
	db.TickPrecomputed();
	CHECK(db.PlanPrecomputed(PAX, A, D, _state_ticks).cost == frozen.cost);
	_state_ticks += PassengerServiceDatabase::TIMETABLE_EPOCH;
	db.TickPrecomputed();
	CHECK(db.PlanPrecomputed(PAX, A, D, _state_ticks).cost > frozen.cost);
}

TEST_CASE("Precomputed cost regrouping wakes queues without a live revision or row-count change", "[passenger][precomputed][events]")
{
	Fixture f;
	f.Train({Stop(A), Stop(D)});
	auto *second = f.Train({Stop(A), Stop(D)});
	auto &db = f.Build();
	second->orders->GetOrderAt(1)->SetWaitTime(11);
	db.TimetableChanged(second->orders->index);
	_state_ticks = NOW + 3000;
	_settings_game.linkgraph.passenger_journey_precomputed = true;
	db.TickPrecomputed(); REQUIRE(db.GetPrecomputedDiagnostic(PAX).patterns == 1);
	_state_ticks = NOW + PassengerServiceDatabase::TIMETABLE_EPOCH;
	for (int i = 0; i < 128 && db.IsBuilding(); ++i) db.Tick();
	REQUIRE_FALSE(db.IsBuilding()); REQUIRE(db.GetPrecomputedDiagnostic(PAX).patterns == 1);
	auto &list = Station::Get(A)->goods[PAX].CreateData().cargo;
	list.RoutePassengerJourneys(PAX, A);
	REQUIRE_FALSE(list.passenger_routing_dirty);
	const auto revision = db.GetPublishedRevision();
	const auto rows = db.GetPrecomputedDiagnostic(PAX).destinations;
	_state_ticks = NOW + 3000 + PassengerServiceDatabase::TIMETABLE_EPOCH;
	db.TickPrecomputed();
	CHECK(db.GetPrecomputedDiagnostic(PAX).patterns == 2);
	CHECK(db.GetPublishedRevision() == revision);
	CHECK(db.GetPrecomputedDiagnostic(PAX).destinations == rows);
	CHECK(list.passenger_routing_dirty);
}

TEST_CASE("Precomputed waiting index excludes unrelated packets and preserves partial reservations", "[passenger][precomputed][boarding][cargo]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(B), Stop(D)});
	auto *other = f.Train({Stop(A), Stop(C), Stop(D)});
	auto &db = f.Build(); for (int i = 0; i < 100; ++i) db.TickPrecomputed();
	StationCargoList waiting; PassengerJourneyLoadingContext loading;
	auto make = [&](const Vehicle *v, uint16_t count, uint16_t age) {
		auto *packet = CargoPacket::Create(count, age, A, TileIndex{0}, Money{17}); REQUIRE(packet->SetPassengerDestination(PAX, D));
		const auto *service = db.Get(v->orders->index); REQUIRE(service != nullptr);
		PassengerJourneyExecution ride; ride.service = service->id; ride.signature = service->incarnation; ride.generation = service->generation;
		ride.boarding = A; ride.alighting = D; ride.boarding_order = 0; ride.alighting_order = 2;
		ride.kind = PassengerRideKind::EstimatedServiceIntent; ride.not_before = ride.planned_at = NOW; ride.waiting_limit = 1000;
		packet->SetPassengerJourney(ride, B); waiting.Append(packet, B); return packet;
	};
	for (int i = 0; i < 5000; ++i) make(other, 1, i);
	auto *target = make(train, 20, 6000); CargoPacketID target_id = target->index;
	REQUIRE(waiting.HasCargoFor(std::array{B, D}, train, &loading, PAX));
	CHECK(waiting.BoardingCandidatesInspected() == 1);
	const uint before = waiting.TotalCount();
	CHECK(waiting.Reserve(7, &train->cargo, std::array{B, D}, TileIndex{0}, train, &loading) == 7);
	CHECK(waiting.TotalCount() == before); CHECK(waiting.ReservedCount() == 7);
	CHECK(CargoPacket::Get(target_id)->Count() == 13);
	CHECK(CargoPacket::Get(target_id)->GetPassengerJourney().kind == PassengerRideKind::EstimatedServiceIntent);
	CHECK(waiting.BoardingCandidatesInspected() == 2);
	CHECK(train->cargo.Return(7, &waiting, B, TileIndex{0}, PAX) == 7);
	CHECK(waiting.ReservedCount() == 0); CHECK(waiting.TotalCount() == before);
	CHECK(waiting.Load(20, &train->cargo, std::array{B, D}, TileIndex{0}, train, &loading) == 20);
	CHECK(waiting.AvailableCount() == 5000); CHECK(train->cargo.StoredCount() == 20);
	CHECK(waiting.BoardingCandidatesInspected() <= 4);
	CHECK_FALSE(waiting.HasCargoFor(std::array{B, D}, train, &loading, PAX));
}

TEST_CASE("Precomputed waiting wake queue and identical service binding are live validated", "[passenger][precomputed][boarding]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *first = f.Train({Stop(A), Stop(D)}), *second = f.Train({Stop(A), Stop(D)});
	auto &db = f.Build(); for (int i = 0; i < 100; ++i) db.TickPrecomputed();
	auto *packet = f.Packet(); auto plan = db.PlanPrecomputed(PAX, A, D, NOW + 10); REQUIRE(plan.ride);
	/* Force the intent to the other equivalent order list. */
	const auto *source = db.Get(first->orders->index); plan.ride->service = source->id; plan.ride->signature = source->incarnation; plan.ride->generation = source->generation;
	packet->SetPassengerJourney(*plan.ride, D);
	StationCargoList waiting; waiting.Append(packet, D); PassengerJourneyLoadingContext loading;
	CHECK_FALSE(waiting.HasCargoFor(std::array{D}, second, &loading, PAX)); CHECK(waiting.BoardingCandidatesInspected() == 0);
	_state_ticks += 10;
	REQUIRE(waiting.HasCargoFor(std::array{D}, second, &loading, PAX));
	CHECK(waiting.Load(10, &second->cargo, std::array{D}, TileIndex{0}, second, &loading) == 10);
	REQUIRE(second->cargo.StoredCount() == 10);
	CHECK(second->cargo.Packets()->front()->GetPassengerJourney().service == second->orders->index);
	CHECK(second->cargo.Packets()->front()->GetPassengerJourney().vehicle == second->index);
}

TEST_CASE("Precomputed fallback boarding selects OD cohorts without scanning unrelated legacy packets", "[passenger][precomputed][boarding][cargo][scaling]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(B)}); f.Train({Stop(B), Stop(D)}); f.Train({Stop(C), Stop(StationID{4})});
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	StationCargoList waiting;
	for (int i = 0; i < 8000; ++i) {
		auto *packet = CargoPacket::Create(1, i, A, TileIndex{0}, Money{17});
		if (i % 3 != 0) { REQUIRE(packet->SetPassengerDestination(PAX, C)); if (i % 3 == 1) packet->FallbackPassengerJourney(A, NOW, C); }
		waiting.Append(packet, C);
	}
	auto *target = f.Packet(20); target->FallbackPassengerJourney(A, NOW); waiting.Append(target, StationID::Invalid());
	PassengerJourneyLoadingContext loading;
	REQUIRE(waiting.HasCargoFor(std::array{B}, train, &loading, PAX));
	CHECK(waiting.BoardingCandidatesInspected() == 1); CHECK(waiting.boarding_groups_inspected == 1);
	CHECK(loading.GetValidationCount() == 1); CHECK(GetPassengerJourneyTickDiagnostic().od_fallback_searches == 0);
	const auto target_id = target->index;
	REQUIRE(waiting.Reserve(7, &train->cargo, std::array{B}, TileIndex{0}, train, &loading) == 7);
	CHECK(loading.GetValidationCount() == 1); CHECK(loading.GetBoardingCacheHits() >= 2);
	CHECK(CargoPacket::Get(target_id)->Count() == 13); CHECK(waiting.ReservedCount() == 7);
	CHECK(train->cargo.Packets()->front()->GetPassengerJourney().alighting == B);
	REQUIRE(train->cargo.Return(7, &waiting, StationID::Invalid(), TileIndex{0}, PAX) == 7);
	waiting.ClearWaitingIndex();
	REQUIRE(waiting.Load(20, &train->cargo, std::array{B}, TileIndex{0}, train, &loading) == 20);
	CHECK(waiting.AvailableCount() == 8000); CHECK(waiting.ReservedCount() == 0);
	const auto inspected = waiting.BoardingCandidatesInspected(), validations = loading.GetValidationCount();
	CHECK_FALSE(waiting.HasCargoFor(std::array{B}, train, &loading, PAX));
	CHECK(waiting.BoardingCandidatesInspected() == inspected); CHECK(loading.GetValidationCount() == validations);
}

TEST_CASE("Precomputed boarding preserves FIFO with recycled IDs and rotates primary buckets after save", "[passenger][precomputed][boarding][save][determinism]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(B), Stop(C), Stop(D)}); auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	StationCargoList waiting; PassengerJourneyLoadingContext loading;
	auto *recycled = CargoPacket::Create();
	auto *old = CargoPacket::Create(1, 1, A, TileIndex{0}, Money{7}); waiting.Append(old, B); auto old_id = old->index;
	delete recycled;
	auto *young = CargoPacket::Create(1, 2, A, TileIndex{0}, Money{7}); waiting.Append(young, B);
	REQUIRE(young->index < old_id);
	auto *other = CargoPacket::Create(1, 3, A, TileIndex{0}, Money{7}); waiting.Append(other, C); auto other_id = other->index;
	auto *journey = f.Packet(1); auto plan = db.PlanPrecomputed(PAX, A, D, NOW); REQUIRE(plan.ride); journey->SetPassengerJourney(*plan.ride, D); waiting.Append(journey, D); auto journey_id = journey->index;
	REQUIRE(waiting.Load(1, &train->cargo, std::array{B, C, D}, TileIndex{0}, train, &loading) == 1);
	CHECK(train->cargo.Packets()->back()->index == old_id); CHECK(waiting.passenger_boarding_last == B);
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	StationCargoList checkpoint; SlTestSaveLoadTable(&waiting, &checkpoint, GetPassengerWaitingEventDesc());
	waiting.ClearWaitingIndex(); SlTestSaveLoadTable(&checkpoint, &waiting, GetPassengerWaitingEventDesc());
	REQUIRE(waiting.Load(1, &train->cargo, std::array{B, C, D}, TileIndex{0}, train, &loading) == 1);
	CHECK(train->cargo.Packets()->back()->index == other_id);
	REQUIRE(waiting.Load(1, &train->cargo, std::array{B, C, D}, TileIndex{0}, train, &loading) == 1);
	CHECK(train->cargo.Packets()->back()->index == journey_id);
	REQUIRE(waiting.Load(1, &train->cargo, std::array{B, C, D}, TileIndex{0}, train, &loading) == 1);
	CHECK(train->cargo.Packets()->back()->index == young->index);
}

TEST_CASE("Precomputed fallback eligibility updates only the changed cohort and wakes at interchange time", "[passenger][precomputed][boarding][events]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	StationCargoList waiting; PassengerJourneyLoadingContext loading;
	CargoPacket *target = nullptr;
	for (int i = 0; i < 3000; ++i) {
		auto *packet = CargoPacket::Create(1, i, A, TileIndex{0}, Money{0}); REQUIRE(packet->SetPassengerDestination(PAX, D));
		packet->SetPassengerInterchange(NOW, train->orders->index, train->orders->GetCargoDistRouteGeneration()); waiting.Append(packet, D); target = packet;
	}
	CHECK_FALSE(waiting.HasCargoFor(std::array{D}, train, &loading, PAX)); CHECK(waiting.BoardingCandidatesInspected() == 0);
	target->SetPassengerInterchange(NOW + 10, OrderListID{100}, 1);
	CHECK_FALSE(waiting.HasCargoFor(std::array{D}, train, &loading, PAX)); CHECK(waiting.BoardingCandidatesInspected() == 0);
	_state_ticks += 10;
	REQUIRE(waiting.HasCargoFor(std::array{D}, train, &loading, PAX)); CHECK(waiting.BoardingCandidatesInspected() == 1);
	train->orders->GetOrderAt(1)->SetUnloadType(OrderUnloadType::NoUnload); loading.Invalidate(train);
	CHECK_FALSE(waiting.HasCargoFor(std::array{D}, train, &loading, PAX));
	train->orders->GetOrderAt(1)->SetUnloadType(OrderUnloadType::UnloadIfPossible); loading.Invalidate(train);
	REQUIRE(waiting.HasCargoFor(std::array{D}, train, &loading, PAX));
	REQUIRE(waiting.Load(1, &train->cargo, std::array{D}, TileIndex{0}, train, &loading) == 1);
	CHECK(waiting.AvailableCount() == 2999); CHECK_FALSE(waiting.HasCargoFor(std::array{D}, train, &loading, PAX));
}

TEST_CASE("Precomputed boarding memo rejects a newly unsafe service before another move", "[passenger][precomputed][boarding][generation]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	auto *packet = f.Packet(); auto plan = db.PlanPrecomputed(PAX, A, D, NOW); REQUIRE(plan.ride); packet->SetPassengerJourney(*plan.ride, D);
	PassengerJourneyLoadingContext loading; REQUIRE(loading.CanBoard(*packet, train, PAX)); REQUIRE(loading.CanBoard(*packet, train, PAX));
	CHECK(loading.GetValidationCount() == 1); CHECK(loading.GetBoardingCacheHits() == 1);
	db.Invalidate(train->orders->index); CHECK_FALSE(loading.CanBoard(*packet, train, PAX)); CHECK(loading.GetValidationCount() == 2);
	delete packet;
}

TEST_CASE("Precomputed fallback cohorts are not truncated by the old per-event OD cache ceiling", "[passenger][precomputed][boarding][scaling]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(B)}); train->cargo_cap = 200;
	for (uint16_t i = 10; i < 150; ++i) f.Train({Stop(B), Stop(StationID{i})});
	auto &db = f.Build(); db.DiscardPrecomputed(); while (!db.PreparePrecomputed()) {}
	StationCargoList waiting; PassengerJourneyLoadingContext loading;
	for (uint16_t i = 10; i < 150; ++i) {
		auto *packet = CargoPacket::Create(1, i, A, TileIndex{0}, Money{7}); REQUIRE(packet->SetPassengerDestination(PAX, StationID{i}));
		packet->FallbackPassengerJourney(A, NOW); waiting.Append(packet, StationID::Invalid());
	}
	CHECK(waiting.Load(140, &train->cargo, std::array{B}, TileIndex{0}, train, &loading) == 140);
	CHECK(waiting.AvailableCount() == 0); CHECK(train->cargo.StoredCount() == 140);
	CHECK(GetPassengerJourneyTickDiagnostic().od_fallback_searches == 0);
}

TEST_CASE("Precomputed circular cohort iterators survive removal of their split boundary", "[passenger][precomputed][boarding][cargo]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(B), Stop(C), Stop(D)}); auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	StationCargoList waiting; PassengerJourneyLoadingContext loading;
	std::array<CargoPacketID, 2> ids;
	for (int i = 0; i < 2; ++i) {
		auto *packet = CargoPacket::Create(1, i, A, TileIndex{0}, Money{7}); REQUIRE(packet->SetPassengerDestination(PAX, D));
		waiting.Append(packet, i == 0 ? B : C); packet->SetPassengerLegacyHop(B); ids[i] = packet->index;
	}
	waiting.passenger_boarding_last = B;
	REQUIRE(waiting.Load(2, &train->cargo, std::array{B, C, D}, TileIndex{0}, train, &loading) == 2);
	CHECK(train->cargo.Packets()->front()->index == ids[1]); CHECK(train->cargo.Packets()->back()->index == ids[0]);
	CHECK(waiting.AvailableCount() == 0); CHECK(waiting.passenger_boarding_last == B);
}

TEST_CASE("Precomputed count-only packet changes retain boarding cohort handles", "[passenger][precomputed][boarding][cargo]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	StationCargoList waiting; PassengerJourneyLoadingContext loading;
	auto *packet = f.Packet(20); packet->FallbackPassengerJourney(A, NOW, D); waiting.Append(packet, D);
	REQUIRE(waiting.HasCargoFor(std::array{D}, train, &loading, PAX)); auto updates = waiting.boarding_index_updates;
	CHECK(waiting.Truncate(3) == 3); CHECK(waiting.TotalCount() == 17); CHECK(waiting.boarding_index_updates == updates);
	CHECK(waiting.Load(17, &train->cargo, std::array{D}, TileIndex{0}, train, &loading) == 17);
}

TEST_CASE("Precomputed boarding directly excludes other physical vehicles and boarding occurrences", "[passenger][precomputed][boarding][scaling]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(B), Stop(D)}), *other = Vehicle::Create<PeriodicTrain>(train);
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	auto plan = db.PlanPrecomputed(PAX, A, D, NOW); REQUIRE(plan.ride);
	StationCargoList waiting; PassengerJourneyLoadingContext loading;
	for (int i = 0; i < 5000; ++i) {
		auto *packet = CargoPacket::Create(1, i, A, TileIndex{0}, Money{0}); REQUIRE(packet->SetPassengerDestination(PAX, D)); auto ride = *plan.ride;
		if (i % 2 == 0) { ride.kind = PassengerRideKind::EstimatedBoundService; ride.vehicle = other->index; }
		else ride.boarding_order = 1;
		packet->SetPassengerJourney(ride, B); waiting.Append(packet, B);
	}
	auto *target = f.Packet(1); target->SetPassengerJourney(*plan.ride, B); waiting.Append(target, B);
	REQUIRE(waiting.HasCargoFor(std::array{B, D}, train, &loading, PAX));
	CHECK(waiting.BoardingCandidatesInspected() == 1); CHECK(loading.GetValidationCount() == 1);
	REQUIRE(waiting.Load(1, &train->cargo, std::array{B, D}, TileIndex{0}, train, &loading) == 1);
	CHECK_FALSE(waiting.HasCargoFor(std::array{B, D}, train, &loading, PAX));
	CHECK(waiting.BoardingCandidatesInspected() == 2); CHECK(loading.GetValidationCount() == 1);
}

TEST_CASE("Precomputed journey cohorts validate lazily without a 128 ride rejection ceiling", "[passenger][precomputed][boarding][scaling]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	std::vector<Order> orders{Stop(A)}; std::vector<StationID> next;
	for (uint16_t i = 4; i < 144; ++i) { orders.push_back(Stop(StationID{i})); next.push_back(StationID{i}); }
	auto *train = f.Train(std::move(orders)); train->cargo_cap = 200;
	auto &db = f.Build(); db.DiscardPrecomputed(); while (!db.PreparePrecomputed()) {}
	StationCargoList waiting; PassengerJourneyLoadingContext loading;
	for (StationID destination : next) {
		auto *packet = CargoPacket::Create(1, destination.base(), A, TileIndex{0}, Money{7}); REQUIRE(packet->SetPassengerDestination(PAX, destination));
		auto plan = db.PlanPrecomputed(PAX, A, destination, NOW); REQUIRE(plan.ride);
		packet->SetPassengerJourney(*plan.ride, destination); waiting.Append(packet, destination);
	}
	REQUIRE(waiting.HasCargoFor(next, train, &loading, PAX)); CHECK(loading.GetValidationCount() == 1);
	REQUIRE(waiting.Load(140, &train->cargo, next, TileIndex{0}, train, &loading) == 140);
	CHECK(waiting.AvailableCount() == 0); CHECK(train->cargo.StoredCount() == 140); CHECK(loading.GetValidationCount() == 140);
}

TEST_CASE("Common section cohort aliases rebind another occurrence of the same service once", "[passenger][precomputed][sections][boarding][cargo]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(B), Stop(C), Stop(A), Stop(B), Stop(C), Stop(D)});
	f.Train({Stop(A), Stop(B), Stop(C)});
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	auto plan = db.PlanPrecomputed(PAX, A, C, NOW); REQUIRE(plan.ride);
	REQUIRE(db.RebindPrecomputed(*plan.ride, train->orders->index, PAX, 3));
	const auto intent = *plan.ride; REQUIRE(intent.boarding_order == 3); REQUIRE(intent.alighting_order == 5);
	auto *packet = CargoPacket::Create(10, 7, A, TileIndex{0}, Money{17}); REQUIRE(packet->SetPassengerDestination(PAX, C)); packet->SetPassengerJourney(intent, C);
	StationCargoList waiting; waiting.Append(packet, C); waiting.ClearWaitingIndex(); PassengerJourneyLoadingContext loading;
	REQUIRE(waiting.HasCargoFor(std::array{B, C, D}, train, &loading, PAX));
	REQUIRE(waiting.Reserve(4, &train->cargo, std::array{B, C, D}, TileIndex{0}, train, &loading) == 4);
	CHECK(packet->GetPassengerJourney() == intent);
	REQUIRE(train->cargo.Return(4, &waiting, C, TileIndex{0}, PAX) == 4);
	REQUIRE(waiting.Load(10, &train->cargo, std::array{B, C, D}, TileIndex{0}, train, &loading) == 10);
	CHECK(waiting.AvailableCount() == 0); CHECK(waiting.ReservedCount() == 0); CHECK(train->cargo.StoredCount() == 10);
	for (const auto *bound : *train->cargo.Packets()) {
		CHECK(bound->GetPassengerJourney().boarding_order == 0); CHECK(bound->GetPassengerJourney().alighting_order == 2);
	}
}

TEST_CASE("Precomputed fallback boarding CPU scaling", "[.benchmark][passenger][precomputed][boarding][fallback][cpu]")
{
	using Clock = std::chrono::steady_clock;
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(B)}); f.Train({Stop(B), Stop(D)}); f.Train({Stop(C), Stop(StationID{4})});
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	std::cout << "unrelated_packets,query_ns,packet_inspections,cohort_inspections,expensive_validations,validation_cache_hits\n";
	for (int size : {128, 1024, 8192, 32768}) {
		StationCargoList waiting; PassengerJourneyLoadingContext loading;
		for (int i = 0; i < size; ++i) {
			auto *packet = CargoPacket::Create(1, i, A, TileIndex{0}, Money{7});
			if (i % 3 != 0) { REQUIRE(packet->SetPassengerDestination(PAX, C)); if (i % 3 == 1) packet->FallbackPassengerJourney(A, NOW, C); }
			waiting.Append(packet, C);
		}
		auto *target = f.Packet(); target->FallbackPassengerJourney(A, NOW); waiting.Append(target, StationID::Invalid());
		REQUIRE(waiting.HasCargoFor(std::array{B}, train, &loading, PAX));
		auto packets = waiting.BoardingCandidatesInspected(), groups = waiting.boarding_groups_inspected;
		auto validations = loading.GetValidationCount(), hits = loading.GetBoardingCacheHits();
		auto started = Clock::now(); size_t successes = 0;
		for (int i = 0; i < 1024; ++i) successes += waiting.HasCargoFor(std::array{B}, train, &loading, PAX);
		auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count() / 1024;
		CHECK(successes == 1024); CHECK(waiting.BoardingCandidatesInspected() - packets == 1024);
		CHECK(waiting.boarding_groups_inspected - groups == 1024); CHECK(loading.GetValidationCount() == validations);
		std::cout << size << ',' << elapsed << ',' << waiting.BoardingCandidatesInspected() - packets << ',' << waiting.boarding_groups_inspected - groups << ','
				<< loading.GetValidationCount() - validations << ',' << loading.GetBoardingCacheHits() - hits << '\n';
	}
}

TEST_CASE("Precomputed pending cohorts board a simultaneous fleet despite an older backlog", "[.benchmark][passenger][precomputed][boarding][arrivals][cpu]")
{
	using Clock = std::chrono::steady_clock;
	std::cout << "simultaneous_trains,older_packets,loaded_people,arrival_us,candidate_packets,validated_groups,pending_after\n";
	for (int count : {1, 16, 64, 256}) {
		Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
		auto *first = f.Train({Stop(A), Stop(D)});
		std::vector<PeriodicTrain *> trains{first};
		for (int i = 1; i < count; ++i) trains.push_back(Vehicle::Create<PeriodicTrain>(first));
		f.Train({Stop(B), Stop(C)});
		auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
		auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo;
		constexpr int older = 32768, seats = 64;
		for (int i = 0; i < older; ++i) {
			auto *packet = CargoPacket::Create(1, i, A, TileIndex{0}, Money{7});
			REQUIRE(packet->SetPassengerDestination(PAX, C)); waiting.Append(packet, C);
		}
		for (int i = 0; i < count * seats; ++i) {
			auto *packet = CargoPacket::Create(1, older + i, A, TileIndex{0}, Money{11});
			REQUIRE(packet->SetPassengerDestination(PAX, D)); waiting.Append(packet, D);
		}
		RunPassengerWaitingEvents(); // The station quantum cannot reach the newer, boardable cohort.
		const auto start = Clock::now();
		size_t loaded = 0, validated = 0;
		for (auto *train : trains) {
			PassengerJourneyLoadingContext loading;
			loaded += waiting.Load(seats, &train->cargo, std::array{D}, TileIndex{0}, train, &loading);
			validated += loading.GetValidationCount();
			CHECK(train->cargo.StoredCount() == seats);
		}
		const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
		CHECK(loaded == count * seats); CHECK(waiting.AvailableCount() == older); CHECK(waiting.ReservedCount() == 0);
		CHECK(_state_ticks == NOW); CHECK(GetPassengerJourneyTickDiagnostic().od_fallback_searches == 0);
		std::cout << count << ',' << older << ',' << loaded << ',' << us << ',' << waiting.BoardingCandidatesInspected()
				<< ',' << validated << ',' << GetPassengerWaitingEventDiagnostic().pending << '\n';
	}
}

TEST_CASE("Precomputed boarding CPU scaling benchmark", "[.benchmark][passenger][precomputed][boarding][cpu]")
{
	using Clock = std::chrono::steady_clock;
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(B), Stop(D)}), *other = f.Train({Stop(A), Stop(C), Stop(D)});
	auto &db = f.Build(); for (int i = 0; i < 100; ++i) db.TickPrecomputed();
	std::cout << "waiting_packets,legacy_ns,indexed_ns,indexed_candidates\n";
	for (int packets : {128, 1024, 8192, 32768}) {
		StationCargoList waiting{};
		for (int i = 0; i <= packets; ++i) {
			const auto *s = db.Get((i == packets ? train : other)->orders->index);
			auto *packet = CargoPacket::Create(1, i, A, TileIndex{0}, Money{0}); REQUIRE(packet->SetPassengerDestination(PAX, D));
			PassengerJourneyExecution ride; ride.service = s->id; ride.signature = s->incarnation; ride.generation = s->generation;
			ride.boarding = A; ride.alighting = D; ride.boarding_order = 0; ride.alighting_order = 2;
			ride.kind = PassengerRideKind::EstimatedServiceIntent; ride.not_before = ride.planned_at = NOW; ride.waiting_limit = 1000;
			packet->SetPassengerJourney(ride, B); waiting.Append(packet, B);
		}
		std::array<int64_t, 2> times{};
		for (int indexed = 0; indexed < 2; ++indexed) {
			_settings_game.linkgraph.passenger_journey_precomputed = indexed != 0;
			PassengerJourneyLoadingContext loading; REQUIRE(waiting.HasCargoFor(std::array{B, D}, train, &loading, PAX));
			auto start = Clock::now(); size_t successes = 0;
			for (int i = 0; i < 512; ++i) successes += waiting.HasCargoFor(std::array{B, D}, train, &loading, PAX);
			times[indexed] = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count() / 512;
			CHECK(successes == 512);
		}
		std::cout << packets << ',' << times[0] << ',' << times[1] << ',' << waiting.BoardingCandidatesInspected() << '\n';
	}
}

TEST_CASE("Precomputed waiting events survive save tables without inventing a routing pass", "[passenger][precomputed][save][determinism]")
{
	Fixture f;
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	bool legacy = false;
	SECTION("Current packet-event save") { REQUIRE(_sl_xv_feature_versions[XSLFI_PASSENGER_PERIODIC] == 11); }
	SECTION("v9 packet events predate the boarding cursor") { _sl_xv_feature_versions[XSLFI_PASSENGER_PERIODIC] = 9; }
	SECTION("v8 common-section save has no packet-event fields") { legacy = true; _sl_xv_feature_versions[XSLFI_PASSENGER_PERIODIC] = 8; }
	GoodsEntry source{}, loaded{}; source.CreateData(); loaded.CreateData();
	auto &a = source.data->cargo;
	a.passenger_routing_dirty = false; a.passenger_routing_revision = 17; a.passenger_routing_rows = 23; a.passenger_routing_due = NOW + 100;
	a.passenger_routing_sweeping = true; a.passenger_routing_cursor = 7; a.passenger_routing_end = 103;
	a.passenger_routing_tick = NOW; a.passenger_routing_work = 31;
	a.passenger_boarding_last = B;
	CHECK_FALSE(source.data->MayBeRemoved());
	REQUIRE_FALSE(SlTestSaveLoadTable(&source, &loaded, GetGoodsDesc()).empty());
	auto &b = loaded.data->cargo; b.RebuildWaitingIndex();
	CHECK_FALSE(b.passenger_routing_dirty); CHECK(b.passenger_routing_revision == 17); CHECK(b.passenger_routing_rows == 23); CHECK(b.passenger_routing_due == NOW + 100);
	CHECK(b.passenger_routing_sweeping == !legacy); CHECK(b.passenger_routing_cursor == (legacy ? 0 : 7)); CHECK(b.passenger_routing_end == (legacy ? 0 : 103));
	CHECK(b.passenger_routing_tick == (legacy ? INVALID_STATE_TICKS : NOW)); CHECK(b.passenger_routing_work == (legacy ? 0 : 31));
	CHECK(b.passenger_boarding_last == (_sl_xv_feature_versions[XSLFI_PASSENGER_PERIODIC] >= 10 ? B : StationID::Invalid()));
	CHECK_FALSE(loaded.data->MayBeRemoved());
	auto *packet = CargoPacket::Create(10, 0, A, TileIndex{0}, Money{0});
	auto *copy = CargoPacket::Create(); packet->SchedulePassengerRouting(NOW + 150);
	REQUIRE_FALSE(SlTestSaveLoadTable(packet, copy, GetCargoPacketDesc()).empty());
	CHECK(copy->GetPassengerRoutingEvent() == (legacy ? StateTicks{} : NOW + 150));
	delete packet; delete copy;
}

TEST_CASE("Precomputed single packet events leave thousands of sleeping intents untouched", "[passenger][precomputed][events][scaling]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo;
	std::vector<CargoPacket *> packets;
	for (int i = 0; i < 5000; ++i) {
		auto *packet = CargoPacket::Create(2, i, A, TileIndex{0}, Money{14});
		REQUIRE(packet->SetPassengerDestination(PAX, D)); waiting.Append(packet, D); packets.push_back(packet);
	}
	for (int i = 0; i < 200; ++i) { waiting.RoutePassengerJourneys(PAX, A); ++_state_ticks; }
	REQUIRE_FALSE(waiting.passenger_routing_sweeping);
	for (auto *packet : packets) REQUIRE(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
	const auto inspected = waiting.passenger_routing_inspected;
	const auto generations = waiting.passenger_generation_inspected;
	const auto queries = db.GetPrecomputedDiagnostic(PAX).queries;
	/* Count/payment changes must not dirty a route. */
	packets.back()->Reduce(1); waiting.ClearWaitingIndex(); waiting.RebuildWaitingIndex();
	waiting.RoutePassengerJourneys(PAX, A);
	CHECK(waiting.passenger_routing_inspected == inspected);
	InvalidatePassengerWaitingRouting(); waiting.RoutePassengerJourneys(PAX, A);
	CHECK(waiting.passenger_routing_inspected == inspected);
	CHECK(waiting.passenger_generation_inspected == generations);
	auto *added = CargoPacket::Create(2, 6000, A, TileIndex{0}, Money{19});
	REQUIRE(added->SetPassengerDestination(PAX, D)); waiting.Append(added, D);
	waiting.RoutePassengerJourneys(PAX, A);
	CHECK(added->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
	CHECK(added->GetFeederShare() == Money{19}); CHECK(added->Count() == 2);
	CHECK(waiting.passenger_routing_inspected == inspected + 1);
	CHECK(waiting.passenger_generation_inspected == generations);
	CHECK(db.GetPrecomputedDiagnostic(PAX).queries == queries + 1);
	/* Interchange readiness and expiry each wake only the affected packet. */
	added->SetPassengerInterchange(_state_ticks + 10, OrderListID::Invalid());
	waiting.RoutePassengerJourneys(PAX, A);
	const auto ready_inspected = waiting.passenger_routing_inspected;
	_state_ticks += 9; waiting.RoutePassengerJourneys(PAX, A);
	CHECK(waiting.passenger_routing_inspected == ready_inspected);
	++_state_ticks; waiting.RoutePassengerJourneys(PAX, A);
	CHECK(waiting.passenger_routing_inspected == ready_inspected + 1);
	auto expired = added->GetPassengerJourney(); expired.planned_at = _state_ticks - expired.waiting_limit - 1;
	added->SetPassengerJourney(expired, D); ++_state_ticks;
	waiting.RoutePassengerJourneys(PAX, A);
	CHECK(waiting.passenger_routing_inspected == ready_inspected + 2);
	CHECK(added->GetPassengerJourney().planned_at == _state_ticks);
	CHECK(db.GetPrecomputedDiagnostic(PAX).queries < 205); // One lookup per cohort/slice, not per packet.
}

TEST_CASE("Precomputed reroutes keep packet event handles valid for split and whole moves", "[passenger][precomputed][events][cargo]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	GoodsEntry flows{}; flows.CreateData().flows.insert(FlowStat(A, C, 100));
	StationCargoList source, destination;
	auto *packet = CargoPacket::Create(10, 0, A, TileIndex{0}, Money{100});
	REQUIRE(packet->SetPassengerDestination(PAX, D)); source.Append(packet, B);
	REQUIRE(source.Reroute(4, &destination, B, A, &flows) == 4);
	REQUIRE(source.RerouteFromSource(3, &source, A, B, A, &flows) == 3);
	REQUIRE(source.Reroute(3, &source, B, A, &flows) == 3);
	CHECK(source.AvailableCount() == 6); CHECK(destination.AvailableCount() == 4);
	CHECK(source.AvailableViaCount(B) == 0); CHECK(source.AvailableViaCount(C) == 6);
	source.RoutePassengerJourneys(PAX, A); destination.RoutePassengerJourneys(PAX, A);
	Money feeder{};
	for (const auto *list : {&source, &destination}) for (StationCargoList::ConstIterator it(list->Packets()->begin()); it != list->Packets()->end(); ++it) {
		CHECK((*it)->GetPassengerRoutingMode() == PassengerRoutingMode::Journey); feeder += (*it)->GetFeederShare();
	}
	CHECK(feeder == Money{100});
	PassengerJourneyLoadingContext loading;
	CHECK(source.Reserve(4, &train->cargo, std::array{B, C, D}, TileIndex{0}, train, &loading) == 4);
	CHECK(source.TotalCount() == 6); CHECK(source.ReservedCount() == 4);
	CHECK(train->cargo.Return(4, &source, C, TileIndex{0}, PAX) == 4);
	CHECK(source.Truncate() == 6); CHECK(destination.Truncate() == 4);
	++_state_ticks; source.RoutePassengerJourneys(PAX, A); destination.RoutePassengerJourneys(PAX, A);
	CHECK(source.AvailableCount() == 0); CHECK(destination.AvailableCount() == 0);
}

TEST_CASE("Precomputed generation events do not depend on waiting index residency", "[passenger][precomputed][events][determinism]")
{
	Fixture f; f.Train({Stop(A), Stop(B), Stop(D)});
	auto &warm = Station::Get(A)->goods[PAX].CreateData().cargo;
	auto &cold = Station::Get(B)->goods[PAX].CreateData().cargo;
	warm.RebuildWaitingIndex(); cold.ClearWaitingIndex();
	warm.passenger_routing_dirty = cold.passenger_routing_dirty = false;
	InvalidatePassengerWaitingRouting();
	CHECK(warm.passenger_routing_dirty); CHECK(cold.passenger_routing_dirty);
}

TEST_CASE("Precomputed service changes wake only actually referenced stale intents", "[passenger][precomputed][events][generation]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo;
	std::vector<CargoPacket *> packets;
	for (int i = 0; i < 200; ++i) {
		auto *packet = CargoPacket::Create(1, i, A, TileIndex{0}, Money{7});
		REQUIRE(packet->SetPassengerDestination(PAX, D)); waiting.Append(packet, D); packets.push_back(packet);
	}
	for (int tick = 0; tick < 10; ++tick) { ++_state_ticks; waiting.RoutePassengerJourneys(PAX, A); }
	const auto inspected = waiting.passenger_routing_inspected;
	db.Invalidate(train->orders->index); waiting.RoutePassengerJourneys(PAX, A);
	CHECK(waiting.passenger_routing_inspected == inspected);
	const auto due = packets.back()->GetPassengerRoutingEvent();
	PassengerJourneyLoadingContext query;
	CHECK_FALSE(query.CanBoard(*packets.back(), train, PAX));
	CHECK(packets.back()->GetPassengerRoutingEvent() == due);
	PassengerJourneyLoadingContext actual; actual.EnableWaitingEvents();
	CHECK_FALSE(actual.CanBoard(*packets.back(), train, PAX));
	CHECK(packets.back()->GetPassengerRoutingEvent() == _state_ticks);
	waiting.RoutePassengerJourneys(PAX, A);
	CHECK(waiting.passenger_routing_inspected == inspected + 1);
	for (size_t i = 0; i + 1 < packets.size(); ++i) CHECK(packets[i]->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
}

TEST_CASE("National passenger events run at stations without loading vehicles", "[passenger][precomputed][events][scheduler]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	Station::Get(A)->loading_vehicles.clear();
	auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo;
	std::vector<CargoPacket *> packets;
	for (uint i = 0; i < 2000; ++i) {
		auto *packet = CargoPacket::Create(2, i, A, TileIndex{0}, Money{17});
		REQUIRE(packet->SetPassengerDestination(PAX, D)); waiting.Append(packet, D); packets.push_back(packet);
	}
	SECTION("Warm index") {}
	SECTION("Cold index with no subsequent arrivals") { waiting.ClearWaitingIndex(); }
	InvalidatePassengerWaitingRouting();
	ResetPassengerJourneyDiagnostic();
	for (int i = 0; i < 10; ++i) { ++_state_ticks; RunPassengerWaitingEvents(); }
	for (auto *packet : packets) {
		CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
		CHECK(packet->Count() == 2); CHECK(packet->GetFeederShare() == Money{17});
	}
	CHECK(waiting.passenger_routing_inspected == 2000);
	CHECK(GetPassengerWaitingEventDiagnostic().groups <= 10);
	const auto inspected = waiting.passenger_routing_inspected;
	RunPassengerWaitingEvents(); CHECK(waiting.passenger_routing_inspected == inspected);
}

TEST_CASE("Passenger event diagnostics exclude inactive cargo and disabled routing", "[passenger][precomputed][events][scheduler][diagnostic]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	constexpr CargoType INACTIVE{1};
	AutoRestoreBackup<CargoClasses> inactive_classes{CargoSpec::Get(INACTIVE)->classes, CargoClasses{}};
	f.Train({Stop(A), Stop(D)});
	auto &active = Station::Get(A)->goods[PAX].CreateData().cargo;
	auto &inactive = Station::Get(A)->goods[INACTIVE].CreateData().cargo;
	ResetPassengerWaitingEventDiagnostic();
	auto *passenger = f.Packet(1); active.Append(passenger, D); passenger->SchedulePassengerRouting(NOW + 10);
	active.RebuildWaitingIndex();
	/* Save/NewGRF cargo conversions can leave passenger metadata in an
	 * inactive goods list. These are not runnable passenger events. */
	for (int i = 0; i < 100; ++i) {
		auto *packet = CargoPacket::Create(1, i, A, TileIndex{0}, Money{7});
		REQUIRE(packet->SetPassengerDestination(PAX, D)); packet->SchedulePassengerRouting(StateTicks{0}); inactive.Append(packet, D);
	}
	inactive.RebuildWaitingIndex();
	const auto stats = GetPassengerWaitingEventDiagnostic();
	CHECK(stats.pending == 1); CHECK(stats.overdue == 0); CHECK(stats.oldest_ticks == 0);
	CHECK(stats.maximum_station_backlog == 1);
	_settings_game.linkgraph.passenger_journey_precomputed = false;
	CHECK(GetPassengerWaitingEventDiagnostic().pending == 0);
	_settings_game.linkgraph.passenger_journey_precomputed = true;
	_settings_game.linkgraph.passenger_journey_routing = false;
	CHECK(GetPassengerWaitingEventDiagnostic().pending == 0);
}

TEST_CASE("National passenger event fairness and debit survive index reconstruction", "[passenger][precomputed][events][scheduler][determinism]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	std::vector<Order> orders;
	constexpr size_t STATIONS = PASSENGER_ROUTING_TICK_WORK / StationCargoList::PASSENGER_ROUTING_SLICE + 3;
	for (size_t i = 0; i < STATIONS; ++i) orders.push_back(Stop(StationID{static_cast<uint16_t>(i)}));
	f.Train(std::move(orders));
	std::vector<StationCargoList *> lists;
	for (size_t i = 0; i < STATIONS; ++i) {
		auto &list = Station::Get(StationID{static_cast<uint16_t>(i)})->goods[PAX].CreateData().cargo;
		lists.push_back(&list);
		for (uint j = 0; j < StationCargoList::PASSENGER_ROUTING_SLICE + 2; ++j) {
			auto *packet = CargoPacket::Create(1, j, A, TileIndex{0}, Money{3});
			REQUIRE(packet->SetPassengerDestination(PAX, i == D.base() ? A : D)); list.Append(packet, D);
		}
	}
	RunPassengerWaitingEvents();
	size_t work = 0; for (auto *list : lists) work += list->passenger_routing_work;
	CHECK(work == PASSENGER_ROUTING_TICK_WORK);
	const auto last = lists.back()->passenger_routing_inspected;
	for (auto *list : lists) { list->ClearWaitingIndex(); list->RebuildWaitingIndex(); }
	RebuildPassengerWaitingScheduler(); RunPassengerWaitingEvents();
	size_t restored = 0; for (auto *list : lists) restored += list->passenger_routing_work;
	CHECK(restored == work); CHECK(lists.back()->passenger_routing_inspected == last);
	/* New work at low IDs cannot displace the stations that missed their turn. */
	++_state_ticks; RunPassengerWaitingEvents();
	CHECK(lists.back()->passenger_routing_inspected > last);
	for (auto *list : lists) CHECK(list->passenger_routing_inspected >= StationCargoList::PASSENGER_ROUTING_SLICE);
}

TEST_CASE("Indexed boarding resolves unprocessed cohorts beyond the station event quantum", "[passenger][precomputed][events][scheduler][boarding]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *long_train = f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	auto *short_train = f.Train({Stop(A), Stop(B), Stop(C)});
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo;
	for (uint i = 0; i < 1000; ++i) {
		auto *packet = CargoPacket::Create(1, i, A, TileIndex{0}, Money{7});
		REQUIRE(packet->SetPassengerDestination(PAX, C)); waiting.Append(packet, C);
	}
	auto *through = f.Packet(9); waiting.Append(through, D);
	/* Deliberately consume the scheduled slice before this large arrival. */
	RunPassengerWaitingEvents();
	const auto before = waiting.passenger_routing_inspected;
	PassengerJourneyLoadingContext loading;
	CHECK(waiting.Reserve(900, &short_train->cargo, std::array{B, C}, TileIndex{0}, short_train, &loading) == 900);
	CHECK(waiting.passenger_routing_inspected == before);
	CHECK(through->GetPassengerRoutingMode() != PassengerRoutingMode::Journey);
	CHECK(GetPassengerWaitingEventDiagnostic().arrival_candidates > 64);
	CHECK(loading.GetValidationCount() < 16);
	CHECK(waiting.Reserve(100, &short_train->cargo, std::array{B, C}, TileIndex{0}, short_train, &loading) == 100);
	/* A partial reservation leaves a reusable unbound intent in FIFO order. */
	auto *partial = CargoPacket::Create(20, 2000, A, TileIndex{0}, Money{31});
	REQUIRE(partial->SetPassengerDestination(PAX, C)); waiting.Append(partial, C);
	PassengerJourneyLoadingContext remaining;
	CHECK(waiting.Reserve(10, &short_train->cargo, std::array{B, C}, TileIndex{0}, short_train, &remaining) == 10);
	CHECK(partial->Count() == 10); CHECK(partial->GetPassengerJourney().kind == PassengerRideKind::EstimatedServiceIntent);
	CHECK(waiting.Reserve(19, &long_train->cargo, std::array{B, C, D}, TileIndex{0}, long_train, &remaining) == 19);
	CHECK(waiting.AvailableCount() == 0);
}

TEST_CASE("Indexed boarding renews stale intents after the national event allowance is spent", "[passenger][precomputed][events][scheduler][boarding][generation]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	auto plan = db.PlanPrecomputed(PAX, A, D, NOW); REQUIRE(plan.ride);
	auto *packet = f.Packet(10); packet->SetPassengerJourney(*plan.ride, D);
	auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo; waiting.Append(packet, D);
	const auto old_intent = packet->GetPassengerJourney();
	train->current_order_time = 1; db.Invalidate(train->orders->index); f.PublishUpdates(db);
	REQUIRE_FALSE(db.IsCurrent(old_intent)); REQUIRE(db.PlanPrecomputed(PAX, A, D, _state_ticks).ride);
	while (ConsumePassengerRoutingWork()) {}
	const auto deadline = packet->GetPassengerRoutingEvent();
	PassengerJourneyLoadingContext loading;
	CHECK(waiting.HasCargoFor(std::array{D}, train, &loading, PAX));
	CHECK(packet->GetPassengerJourney() == old_intent); CHECK(packet->GetPassengerRoutingEvent() == deadline);
	CHECK(waiting.Load(4, &train->cargo, std::array{D}, TileIndex{0}, train, &loading) == 4);
	CHECK(waiting.AvailableCount() == 6); CHECK(packet->Count() == 6);
	CHECK(packet->GetPassengerJourney().kind == PassengerRideKind::EstimatedServiceIntent);
	CHECK(db.IsCurrent(packet->GetPassengerJourney())); CHECK(packet->GetPassengerDestination() == D);
	for (const auto *loaded : *train->cargo.Packets()) {
		CHECK(db.IsCurrent(loaded->GetPassengerJourney())); CHECK(loaded->GetPassengerJourney().vehicle == train->index);
	}
}

TEST_CASE("Precomputed dormant no-route cohorts wake on publication without periodic polling", "[passenger][precomputed][events][generation]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	bool deferred = false, building = false;
	SECTION("New unresolved passengers") {}
	SECTION("Previously deferred passengers") { deferred = true; }
	SECTION("Previously deferred while destination rows build") { deferred = building = true; }
	f.Train({Stop(A), Stop(D)}); f.Train({Stop(B), Stop(C)});
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	if (building) { db.DiscardPrecomputed(); db.TickPrecomputed(1); REQUIRE(db.GetPrecomputedDiagnostic(PAX).destinations == 0); }
	auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo;
	std::vector<CargoPacket *> packets;
	for (int i = 0; i < 300; ++i) {
		auto *packet = CargoPacket::Create(1, i, A, TileIndex{0}, Money{5});
		REQUIRE(packet->SetPassengerDestination(PAX, C));
		if (deferred) packet->DeferPassengerJourney(A, NOW, D);
		waiting.Append(packet, D); packets.push_back(packet);
	}
	for (int tick = 0; tick < 20; ++tick) { ++_state_ticks; waiting.RoutePassengerJourneys(PAX, A); }
	for (auto *packet : packets) {
		CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Fallback);
		CHECK(packet->GetPassengerRoutingEvent() == INVALID_STATE_TICKS);
	}
	const auto inspected = waiting.passenger_routing_inspected;
	_state_ticks += 1024; waiting.RoutePassengerJourneys(PAX, A);
	CHECK(waiting.passenger_routing_inspected == inspected);
	f.Train({Stop(A), Stop(C)}); db.Invalidate(); f.PublishUpdates(db);
	for (int i = 0; i < 100; ++i) db.TickPrecomputed();
	for (int tick = 0; tick < 20; ++tick) { ++_state_ticks; waiting.RoutePassengerJourneys(PAX, A); }
	for (auto *packet : packets) {
		CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
		CHECK(packet->GetPassengerJourney().alighting == C);
	}
}

TEST_CASE("Precomputed boarding regrouping agrees before and after rebuilding pending indices", "[passenger][precomputed][events][boarding][determinism]")
{
	Fixture f;
	f.Train({Stop(A), Stop(D)}); auto *second = f.Train({Stop(A), Stop(D)});
	auto &db = f.Build();
	second->orders->GetOrderAt(1)->SetWaitTime(11); db.TimetableChanged(second->orders->index);
	_settings_game.linkgraph.passenger_journey_precomputed = true;
	_state_ticks = NOW + 3000; db.TickPrecomputed();
	REQUIRE(db.GetPrecomputedDiagnostic(PAX).patterns == 1);
	_state_ticks = NOW + PassengerServiceDatabase::TIMETABLE_EPOCH;
	for (int i = 0; i < 128 && db.IsBuilding(); ++i) db.Tick();
	auto *packet = f.Packet(); auto plan = db.PlanPrecomputed(PAX, A, D, _state_ticks); REQUIRE(plan.ride);
	const auto *service = db.Get(second->orders->index); REQUIRE(service != nullptr);
	plan.ride->service = service->id; plan.ride->signature = service->incarnation; plan.ride->generation = service->generation;
	plan.ride->waiting_limit = 20000;
	packet->SetPassengerJourney(*plan.ride, D);
	auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo; waiting.Append(packet, D);
	PassengerJourneyLoadingContext initial;
	REQUIRE(waiting.HasCargoFor(std::array{D}, second, &initial, PAX));
	_state_ticks = NOW + 3000 + PassengerServiceDatabase::TIMETABLE_EPOCH; db.TickPrecomputed();
	REQUIRE(db.GetPrecomputedDiagnostic(PAX).patterns == 2);
	REQUIRE(db.IsCurrent(packet->GetPassengerJourney()));
	/* No routing/refresh event between regrouping and this actual reference. */
	PassengerJourneyLoadingContext before;
	const bool live = waiting.HasCargoFor(std::array{D}, second, &before, PAX);
	waiting.ClearWaitingIndex(); waiting.RebuildWaitingIndex();
	PassengerJourneyLoadingContext after;
	CHECK(waiting.HasCargoFor(std::array{D}, second, &after, PAX) == live);
	CHECK(live);
}

TEST_CASE("Precomputed event slices preserve old deadlines and station shares under arrivals", "[passenger][precomputed][events][fairness]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	f.Train({Stop(A), Stop(B), Stop(D)}); auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	auto &a = Station::Get(A)->goods[PAX].CreateData().cargo;
	auto &b = Station::Get(B)->goods[PAX].CreateData().cargo;
	auto *spare = f.Packet(); const auto recycled = spare->index;
	std::vector<CargoPacket *> queued;
	for (uint i = 0; i < 3 * StationCargoList::PASSENGER_ROUTING_SLICE + 44; ++i) {
		auto *packet = CargoPacket::Create(1, i, A, TileIndex{0}, Money{7});
		REQUIRE(packet->SetPassengerDestination(PAX, D)); a.Append(packet, D); queued.push_back(packet);
	}
	a.RoutePassengerJourneys(PAX, A);
	CHECK(a.passenger_routing_work == StationCargoList::PASSENGER_ROUTING_SLICE);
	const auto inspected = a.passenger_routing_inspected;
	for (int i = 0; i < 10; ++i) a.RoutePassengerJourneys(PAX, A);
	CHECK(a.passenger_routing_inspected == inspected);
	b.Append(f.Packet(), D); b.RoutePassengerJourneys(PAX, B);
	CHECK(b.passenger_routing_inspected == 1);
	delete spare; ++_state_ticks;
	auto *newcomer = f.Packet(); REQUIRE(newcomer->index == recycled); a.Append(newcomer, D);
	a.RoutePassengerJourneys(PAX, A);
	CHECK(newcomer->GetPassengerRoutingMode() == PassengerRoutingMode::Legacy);
	/* Repeated invalidations do not reset an unfinished sweep to low pool IDs. */
	for (int tick = 0; tick < 20; ++tick) {
		++_state_ticks; InvalidatePassengerWaitingRouting();
		auto *packet = CargoPacket::Create(1, 1000 + tick, A, TileIndex{0}, Money{3});
		REQUIRE(packet->SetPassengerDestination(PAX, D)); a.Append(packet, D);
		a.RoutePassengerJourneys(PAX, A);
		CHECK(a.passenger_routing_work <= StationCargoList::PASSENGER_ROUTING_SLICE);
	}
	for (auto *packet : queued) CHECK(packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
	CHECK(newcomer->GetPassengerRoutingMode() == PassengerRoutingMode::Journey);
}

TEST_CASE("Precomputed pending slices replay packet deadlines and cursors through save tables", "[passenger][precomputed][events][save][determinism]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo;
	std::vector<CargoPacket *> packets, saved, completed;
	for (uint i = 0; i < 3 * StationCargoList::PASSENGER_ROUTING_SLICE + 8; ++i) {
		auto *packet = CargoPacket::Create(2, i, A, TileIndex{0}, Money{17});
		REQUIRE(packet->SetPassengerDestination(PAX, D)); waiting.Append(packet, D); packets.push_back(packet);
	}
	waiting.RoutePassengerJourneys(PAX, A);
	REQUIRE(waiting.passenger_routing_sweeping);
	StationCargoList saved_scheduler;
	SlTestSaveLoadTable(&waiting, &saved_scheduler, GetPassengerWaitingEventDesc());
	for (auto *packet : packets) {
		auto *copy = f.Packet(); SlTestSaveLoadTable(packet, copy, GetCargoPacketDesc()); saved.push_back(copy);
	}
	const StateTicks saved_tick = _state_ticks;
	using Checkpoint = std::tuple<uint32_t, bool, uint16_t>;
	std::vector<Checkpoint> checkpoints;
	for (int i = 0; i < 8; ++i) {
		++_state_ticks; waiting.RoutePassengerJourneys(PAX, A);
		checkpoints.emplace_back(waiting.passenger_routing_cursor, waiting.passenger_routing_sweeping, waiting.passenger_routing_work);
	}
	for (auto *packet : packets) {
		auto *copy = f.Packet(); SlTestSaveLoadTable(packet, copy, GetCargoPacketDesc()); completed.push_back(copy);
	}
	waiting.ClearWaitingIndex();
	for (size_t i = 0; i < packets.size(); ++i) SlTestSaveLoadTable(saved[i], packets[i], GetCargoPacketDesc());
	SlTestSaveLoadTable(&saved_scheduler, &waiting, GetPassengerWaitingEventDesc());
	_state_ticks = saved_tick; waiting.RebuildWaitingIndex();
	/* A join/rebuild in the middle of the same tick cannot buy another slice. */
	const auto inspected = waiting.passenger_routing_inspected;
	waiting.RoutePassengerJourneys(PAX, A); CHECK(waiting.passenger_routing_inspected == inspected);
	for (int i = 0; i < 8; ++i) {
		++_state_ticks; waiting.RoutePassengerJourneys(PAX, A);
		CHECK(Checkpoint(waiting.passenger_routing_cursor, waiting.passenger_routing_sweeping, waiting.passenger_routing_work) == checkpoints[i]);
	}
	for (size_t i = 0; i < packets.size(); ++i) {
		CHECK(packets[i]->HasSamePassengerRouting(*completed[i]));
		CHECK(packets[i]->GetPassengerRoutingEvent() == completed[i]->GetPassengerRoutingEvent());
		CHECK(packets[i]->Count() == 2); CHECK(packets[i]->GetFeederShare() == Money{17});
	}
}

TEST_CASE("Precomputed idle loading passes do not rescan or replan waiting passengers", "[passenger][precomputed][events]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); for (int i = 0; i < 100; ++i) db.TickPrecomputed();
	auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo; waiting.Append(f.Packet(), D);
	waiting.RoutePassengerJourneys(PAX, A);
	REQUIRE(db.GetPrecomputedDiagnostic(PAX).queries == 1);
	const auto operations = db.GetPrecomputedDiagnostic(PAX).operations;
	for (int i = 0; i < 100; ++i) {
		++_state_ticks; BeginPassengerJourneyTick(); db.Tick(); waiting.RoutePassengerJourneys(PAX, A);
		CHECK(db.GetPrecomputedDiagnostic(PAX).queries == 1); CHECK(db.GetPrecomputedDiagnostic(PAX).operations == operations);
		CHECK(GetPassengerJourneyTickDiagnostic().fair_share_work == 0);
	}
	db.Invalidate(); CHECK(waiting.passenger_routing_dirty);
}

TEST_CASE("Precomputed waiting event CPU scaling", "[.benchmark][passenger][precomputed][events][scaling][cpu]")
{
	using Clock = std::chrono::steady_clock;
	std::cout << "waiting_packets,idle_ns,single_event_ns,generation_ns,processed_events,generation_packets\n";
	for (int size : {128, 1024, 8192, 32768}) {
		Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
		f.Train({Stop(A), Stop(D)}); auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
		auto ride = db.PlanPrecomputed(PAX, A, D, _state_ticks).ride; REQUIRE(ride);
		ride->waiting_limit = 1000000;
		auto &waiting = Station::Get(A)->goods[PAX].CreateData().cargo;
		CargoPacket *target = nullptr;
		for (int i = 0; i < size; ++i) {
			auto *packet = CargoPacket::Create(2, i, A, TileIndex{0}, Money{7});
			REQUIRE(packet->SetPassengerDestination(PAX, D)); packet->SetPassengerJourney(*ride, D);
			waiting.Append(packet, D); target = packet;
		}
		for (int tick = 0; tick < size / StationCargoList::PASSENGER_ROUTING_SLICE + 2; ++tick) {
			++_state_ticks; waiting.RoutePassengerJourneys(PAX, A);
		}
		const auto inspected = waiting.passenger_routing_inspected;
		const auto generations = waiting.passenger_generation_inspected;
		std::array<int64_t, 3> elapsed{};
		for (int mode = 0; mode < 3; ++mode) {
			const auto start = Clock::now();
			for (int tick = 0; tick < 1024; ++tick) {
				++_state_ticks;
				if (mode == 1) target->SetPassengerInterchange(_state_ticks, OrderListID::Invalid());
				if (mode == 2) InvalidatePassengerWaitingRouting();
				waiting.RoutePassengerJourneys(PAX, A);
			}
			elapsed[mode] = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count() / 1024;
		}
		CHECK(waiting.passenger_routing_inspected - inspected == 1024);
		CHECK(waiting.passenger_generation_inspected == generations);
		std::cout << size << ',' << elapsed[0] << ',' << elapsed[1] << ',' << elapsed[2] << ','
				<< waiting.passenger_routing_inspected - inspected << ',' << waiting.passenger_generation_inspected - generations << '\n';
	}
}

TEST_CASE("Precomputed generation capture retains large shared physical fleets", "[passenger][precomputed][fleet][save]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *first = f.Train({Stop(A), Stop(D)});
	for (int i = 1; i < 600; ++i) Vehicle::Create<PeriodicTrain>(first);
	auto &db = f.Build(); for (int i = 0; i < 100; ++i) db.TickPrecomputed();
	REQUIRE(db.Get(first->orders->index) != nullptr); CHECK(db.Get(first->orders->index)->vehicles.size() == 600);
	CHECK(db.GetPrecomputedDiagnostic(PAX).patterns == 1); CHECK(db.GetPrecomputedDiagnostic(PAX).states == 8);
	REQUIRE(db.PlanPrecomputed(PAX, A, D, NOW).ride);
	PassengerServiceDatabase peer; auto wire = db.Save(); REQUIRE(peer.Load(wire)); peer.FinishLoad(true);
	CHECK(peer.Save() == wire); CHECK(peer.PlanPrecomputed(PAX, A, D, NOW).ride == db.PlanPrecomputed(PAX, A, D, NOW).ride);
}

TEST_CASE("Precomputed startup completes discovery and all rows without simulation", "[passenger][precomputed][startup]")
{
	Fixture f;
	f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	_settings_game.linkgraph.passenger_journey_precomputed = true;
	auto &db = GetPassengerServiceDatabase();
	for (size_t slices = 0; !db.PreparePrecomputed(); ++slices) REQUIRE(slices < 5000);
	CHECK(_state_ticks == NOW);
	CHECK(db.PrecomputedFinished()); CHECK_FALSE(db.IsPreparingPrecomputed());
	CHECK(db.GetPrecomputedDiagnostic(PAX).destinations == 4);
	REQUIRE(db.PlanPrecomputed(PAX, A, D, NOW).ride);
	auto wire = db.Save();
	CHECK(db.PreparePrecomputed()); CHECK(db.Save() == wire);
	PassengerServiceDatabase joined; REQUIRE(joined.Load(wire)); joined.FinishLoad(true);
	CHECK(joined.Save() == wire); CHECK(joined.PreparePrecomputed());
	CHECK(joined.PlanPrecomputed(PAX, A, D, NOW).ride == db.PlanPrecomputed(PAX, A, D, NOW).ride);
}

TEST_CASE("Saving suspended initial fleet capture resumes the same publication", "[passenger][precomputed][startup][save][determinism]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *first = f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	for (uint i = 0; i < 600; ++i) Vehicle::Create<PeriodicTrain>(first);
	auto &db = GetPassengerServiceDatabase();
	CHECK_FALSE(db.PreparePrecomputed());
	const auto saved = db.Save();
	PassengerServiceDatabase peer; REQUIRE(peer.Load(saved)); peer.FinishLoad(true, true);
	for (uint i = 0; db.IsPreparingPrecomputed() || peer.IsPreparingPrecomputed(); ++i) {
		REQUIRE(i < 10000);
		db.PreparePrecomputed(); peer.PreparePrecomputed();
		CHECK(_state_ticks == NOW);
	}
	CHECK(db.Save() == peer.Save());
	CHECK(db.PlanPrecomputed(PAX, A, D, NOW).ride == peer.PlanPrecomputed(PAX, A, D, NOW).ride);
}

TEST_CASE("Replacement preparation completes only the graph and retains the row frontier", "[passenger][precomputed][startup][generation][determinism]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *first = f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	for (uint i = 0; i < 2000; ++i) Vehicle::Create<PeriodicTrain>(first);
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	const auto old_rows = db.GetPrecomputedDiagnostic(PAX).destinations;
	_settings_game.linkgraph.passenger_transfer_penalty = 17;
	db.Tick(PassengerServiceDatabase::BUILD_WORK, true, true);
	REQUIRE(db.IsPreparingPrecomputed());
	const auto wire = db.Save();
	PassengerServiceDatabase loaded; REQUIRE(loaded.Load(wire)); loaded.FinishLoad(true, true);
	while (loaded.StepLoad() == PassengerServiceDatabase::LoadState::Replaying) {}
	while (!db.PreparePrecomputed()) {
		CHECK(_state_ticks == NOW);
		CHECK(db.Save() == wire);
	}
	CHECK(db.Save() == wire);
	CHECK(loaded.Save() == db.Save());
	CHECK_FALSE(db.PrecomputedFinished());
	CHECK(db.GetPrecomputedDiagnostic(PAX).destinations == 0);
	CHECK(db.GetPrecomputedDiagnostic(PAX).retained_destinations == old_rows);
	db.TickPrecomputed(1);
	loaded.TickPrecomputed(1);
	CHECK(loaded.Save() == db.Save());
	CHECK(db.GetPrecomputedDiagnostic(PAX).operations == 1);
}

TEST_CASE("Precomputed replacement retains pending destination rows across every saved frontier", "[passenger][precomputed][generation][save]")
{
	Fixture f;
	f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	f.Train({Stop(A), Stop(B), Stop(C)});
	auto &db = f.Build(); _settings_game.linkgraph.passenger_journey_precomputed = true;
	while (!db.PreparePrecomputed()) {}
	const auto old = db.PlanPrecomputed(PAX, A, D, NOW); REQUIRE(old.ride);
	_settings_game.linkgraph.passenger_transfer_penalty = 17;
	db.TickPrecomputed(1);
	REQUIRE_FALSE(db.PrecomputedFinished()); CHECK_FALSE(db.IsPreparingPrecomputed());
	CHECK(db.PlanPrecomputed(PAX, A, D, NOW).ride == old.ride);
	PassengerServiceDatabase joined;
	for (int i = 0; !db.PrecomputedFinished(); ++i) {
		REQUIRE(i < 10000);
		/* Loading at arbitrary row frontiers restores both inputs and retires
		 * exactly the rows already replaced, without an extra routing pass. */
		if (i % 19 == 0) {
			auto wire = db.Save(); REQUIRE(joined.Load(wire)); joined.FinishLoad(true); CHECK(joined.Save() == wire);
		}
		for (StationID from : {A, B, C, D}) for (StationID to : {A, B, C, D}) if (from != to) {
			auto plan = db.PlanPrecomputed(PAX, from, to, NOW); REQUIRE(plan.ride);
		}
		db.TickPrecomputed(1);
	}
	const auto completed = db.Save(); REQUIRE(joined.Load(completed)); joined.FinishLoad(true);
	CHECK(joined.Save() == completed);
	CHECK(db.PlanPrecomputed(PAX, A, D, NOW).cost == old.cost);
}

TEST_CASE("Precomputed pre-section cursors replay the old graph before upgrading", "[passenger][precomputed][sections][save][network]")
{
	Fixture f;
	f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	f.Train({Stop(A), Stop(B), Stop(C)});
	auto &db = f.Build(); _settings_game.linkgraph.passenger_journey_precomputed = true;
	db.TickPrecomputed(1);
	auto old = LegacyDatabaseWire(db.Save()); old.resize(old.size() - 16); old[0] = 7;
	SECTION("v6 without a retained generation") { old.resize(old.size() - 24); old[0] = 6; }
	SECTION("v7 with a retained-generation suffix") {}
	PassengerServiceDatabase migrated; REQUIRE(migrated.Load(old)); migrated.FinishLoad(true, true);
	while (migrated.StepLoad(1) == PassengerServiceDatabase::LoadState::Replaying) {}
	REQUIRE(migrated.GetLoadState() == PassengerServiceDatabase::LoadState::Ready);
	CHECK(migrated.GetPrecomputedDiagnostic(PAX).shared_sections == 0);
	while (!migrated.PrecomputedFinished()) migrated.TickPrecomputed();
	REQUIRE(migrated.PlanPrecomputed(PAX, A, D, NOW).ride);
	migrated.TickPrecomputed(1); // New graph, retaining a completed old graph.
	CHECK(migrated.GetPrecomputedDiagnostic(PAX).shared_sections > 0);
	CHECK(migrated.GetPrecomputedDiagnostic(PAX).retained_destinations > 0);
	auto mixed = migrated.Save(); PassengerServiceDatabase peer;
	REQUIRE(peer.Load(mixed)); peer.FinishLoad(true); CHECK(peer.Save() == mixed);
	for (size_t i = 0; !migrated.PrecomputedFinished(); ++i) {
		REQUIRE(i < 10000); migrated.TickPrecomputed(19); peer.TickPrecomputed(19);
		CHECK(migrated.Save() == peer.Save());
		CHECK(migrated.PlanPrecomputed(PAX, A, C, NOW).ride == peer.PlanPrecomputed(PAX, A, C, NOW).ride);
	}
}

TEST_CASE("Precomputed join replay yields at exact work boundaries for all publication states", "[passenger][precomputed][save][network]")
{
	Fixture f; f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	f.Train({Stop(A), Stop(B), Stop(C)});
	auto &db = f.Build(); _settings_game.linkgraph.passenger_journey_precomputed = true;
	db.TickPrecomputed(73);
	SECTION("Initial frontier") { REQUIRE_FALSE(db.PrecomputedFinished()); }
	SECTION("Completed generation") { while (!db.PrecomputedFinished()) db.TickPrecomputed(); }
	SECTION("Replacement frontier with retained rows") {
		while (!db.PrecomputedFinished()) db.TickPrecomputed();
		_settings_game.linkgraph.passenger_transfer_penalty = 17;
		db.TickPrecomputed(73);
		REQUIRE(db.GetPrecomputedDiagnostic(PAX).retained_destinations > 0);
	}
	const auto wire = db.Save(); const auto limit = db.GetLoadWorkLimit();
	for (size_t quantum : {size_t{1}, size_t{19}, PassengerServiceDatabase::BUILD_WORK}) {
		PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true, true);
		REQUIRE(peer.GetLoadState() == PassengerServiceDatabase::LoadState::Replaying);
		CHECK(peer.StepLoad(0) == PassengerServiceDatabase::LoadState::Replaying);
		uint64_t last = 0, last_progress = 0;
		for (size_t slices = 0; peer.GetLoadState() == PassengerServiceDatabase::LoadState::Replaying; ++slices) {
			REQUIRE(slices < 100000);
			peer.StepLoad(quantum);
			CHECK(peer.GetLoadWork() - last <= quantum); last = peer.GetLoadWork();
			CHECK(peer.GetLoadWork() <= limit);
			CHECK(peer.GetLoadProgress() >= last_progress);
			CHECK(peer.GetLoadProgress() <= limit);
			last_progress = peer.GetLoadProgress();
			CHECK(_state_ticks == NOW);
			CHECK(peer.Save() == wire); // Serialized progress is the saved frontier, never the local replay cursor.
		}
		REQUIRE(peer.GetLoadState() == PassengerServiceDatabase::LoadState::Ready);
		CHECK(peer.GetPrecomputedDiagnostic(PAX).destinations == db.GetPrecomputedDiagnostic(PAX).destinations);
		CHECK(peer.GetPrecomputedDiagnostic(PAX).retained_destinations == db.GetPrecomputedDiagnostic(PAX).retained_destinations);
		for (StationID from : {A, B, C, D}) for (StationID to : {A, B, C, D}) {
			const auto actual = peer.PlanPrecomputed(PAX, from, to, NOW), expected = db.PlanPrecomputed(PAX, from, to, NOW);
			CHECK(actual.state == expected.state); CHECK(actual.ride == expected.ride); CHECK(actual.cost == expected.cost);
		}
		for (int i = 0; i < 100; ++i) {
			/* Different replay slice sizes must resume the same deterministic builder. */
			peer.TickPrecomputed(11);
		}
		PassengerServiceDatabase control; REQUIRE(control.Load(wire)); control.FinishLoad(true);
		for (int i = 0; i < 100; ++i) control.TickPrecomputed(11);
		CHECK(peer.Save() == control.Save());
	}
}

TEST_CASE("Precomputed sliced replay rejects impossible cursors and migrates legacy frontiers", "[passenger][precomputed][save][network]")
{
	Fixture f; f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	auto &db = f.Build();
	const size_t cursor_offset = LegacyDatabaseWire(db.Save(), 3).size(); // v3 prefix precedes the precomputed extension.
	_settings_game.linkgraph.passenger_journey_precomputed = true; db.TickPrecomputed(1);
	auto wire = LegacyDatabaseWire(db.Save()); REQUIRE(wire.size() > cursor_offset + 8);
	SECTION("Invalid work never authorizes simulation") {
		for (size_t i = 0; i < 8; ++i) wire[cursor_offset + i] = 255;
		wire[cursor_offset] = 254; // UINT64_MAX is the separate 'not started' sentinel.
		PassengerServiceDatabase peer; REQUIRE(peer.Load(wire)); peer.FinishLoad(true, true);
		for (size_t i = 0; peer.GetLoadState() == PassengerServiceDatabase::LoadState::Replaying; ++i) {
			REQUIRE(i < 10000); peer.StepLoad(19);
		}
		CHECK(peer.GetLoadState() == PassengerServiceDatabase::LoadState::Invalid);
		CHECK(peer.StepLoad() == PassengerServiceDatabase::LoadState::Invalid);
		CHECK(_state_ticks == NOW);
	}
	SECTION("Legacy work format remains saveable until conversion completes") {
		auto legacy = wire; legacy[0] = 5; legacy.resize(legacy.size() - 40);
		PassengerServiceDatabase peer; REQUIRE(peer.Load(legacy)); peer.FinishLoad(true, true);
		CHECK(peer.Save() == legacy);
		while (peer.StepLoad(1) == PassengerServiceDatabase::LoadState::Replaying) CHECK(peer.Save() == legacy);
		CHECK(peer.GetLoadState() == PassengerServiceDatabase::LoadState::Ready);
		auto upgraded = wire; upgraded[upgraded.size() - 16] = 0;
		CHECK(LegacyDatabaseWire(peer.Save()) == upgraded);
	}
}

TEST_CASE("Precomputed replacement coalesces edits and rejects only unsafe live rides", "[passenger][precomputed][generation]")
{
	Fixture f;
	auto *first = f.Train({Stop(A), Stop(B)});
	f.Train({Stop(C), Stop(D)});
	auto &db = f.Build(); _settings_game.linkgraph.passenger_journey_precomputed = true;
	while (!db.PreparePrecomputed()) {}
	_settings_game.linkgraph.passenger_transfer_penalty = 17;
	db.TickPrecomputed(1);
	const auto operations = db.GetPrecomputedDiagnostic(PAX).operations;
	db.Invalidate(first->orders->index);
	CHECK_FALSE(db.PlanPrecomputed(PAX, A, B, NOW).ride);
	REQUIRE(db.PlanPrecomputed(PAX, C, D, NOW).ride);
	db.TickPrecomputed(1);
	CHECK(db.GetPrecomputedDiagnostic(PAX).operations == operations + 1);
	PassengerServiceDatabase joined; auto wire = db.Save(); REQUIRE(joined.Load(wire)); joined.FinishLoad(true);
	CHECK(joined.Save() == wire);
	CHECK_FALSE(joined.PlanPrecomputed(PAX, A, B, NOW).ride);
	CHECK(joined.PlanPrecomputed(PAX, C, D, NOW).ride == db.PlanPrecomputed(PAX, C, D, NOW).ride);
}

TEST_CASE("Precomputed boarding uses primary FIFO order through refresh wake and partial moves", "[passenger][precomputed][boarding][fairness]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(D)});
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	StationCargoList waiting;
	/* Deliberately enqueue the larger pool ID first. Pool allocation/reuse
	 * must not give newly arriving passengers priority over the saved queue. */
	auto *newer = CargoPacket::Create(10, 1, A, TileIndex{0}, Money{0});
	auto *older = CargoPacket::Create(10, 2, A, TileIndex{0}, Money{0});
	REQUIRE(older->index > newer->index);
	for (CargoPacket *packet : {older, newer}) {
		REQUIRE(packet->SetPassengerDestination(PAX, D));
		auto plan = db.PlanPrecomputed(PAX, A, D, NOW + 10); REQUIRE(plan.ride);
		packet->SetPassengerJourney(*plan.ride, D); waiting.Append(packet, D);
	}
	waiting.RefreshWaitingPacket(*older);
	_state_ticks += 10;
	PassengerJourneyLoadingContext loading;
	REQUIRE(waiting.HasCargoFor(std::array{D}, train, &loading, PAX));
	REQUIRE(waiting.Reserve(5, &train->cargo, std::array{D}, TileIndex{0}, train, &loading) == 5);
	CHECK(train->cargo.Packets()->front()->GetPeriodsInTransit() == 2);
	CHECK(older->Count() == 5); CHECK(newer->Count() == 10);
	/* A derived-index reconstruction, as on load, preserves the same order. */
	waiting.ClearWaitingIndex();
	REQUIRE(waiting.Load(5, &train->cargo, std::array{D}, TileIndex{0}, train, &loading) == 5); // Consume the reservation first.
	REQUIRE(waiting.Load(5, &train->cargo, std::array{D}, TileIndex{0}, train, &loading) == 5);
	CHECK(newer->Count() == 10);
	REQUIRE(waiting.Load(10, &train->cargo, std::array{D}, TileIndex{0}, train, &loading) == 10);
	CHECK(waiting.AvailableCount() == 0); // Erasing the last indexed bucket is safe.
}

TEST_CASE("Precomputed same service boarding prefix CPU benchmark", "[.benchmark][passenger][precomputed][boarding][cpu]")
{
	using Clock = std::chrono::steady_clock;
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *train = f.Train({Stop(A), Stop(D)});
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	std::cout << "cohort_packets,prefix_ns,prefix_candidates\n";
	for (int packets : {128, 1024, 8192, 32768}) {
		StationCargoList waiting;
		for (int i = 0; i < packets; ++i) {
			auto *packet = CargoPacket::Create(1, i, A, TileIndex{0}, Money{0}); REQUIRE(packet->SetPassengerDestination(PAX, D));
			auto plan = db.PlanPrecomputed(PAX, A, D, NOW); REQUIRE(plan.ride);
			packet->SetPassengerJourney(*plan.ride, D); waiting.Append(packet, D);
		}
		PassengerJourneyLoadingContext loading;
		REQUIRE(waiting.HasCargoFor(std::array{D}, train, &loading, PAX));
		const auto before = waiting.BoardingCandidatesInspected();
		auto started = Clock::now();
		size_t successes = 0;
		for (int i = 0; i < 512; ++i) successes += waiting.HasCargoFor(std::array{D}, train, &loading, PAX);
		const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count() / 512;
		CHECK(successes == 512);
		CHECK(waiting.BoardingCandidatesInspected() - before == 512);
		std::cout << packets << ',' << ns << ',' << (waiting.BoardingCandidatesInspected() - before) / 512 << '\n';
	}
}

TEST_CASE("Common section intents board the first eligible pattern and keep concrete orders", "[passenger][precomputed][sections][boarding][save]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *long_train = f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	auto *short_train = f.Train({Stop(B), Stop(A), Stop(B), Stop(C)}); short_train->LoadAt(1); short_train->current_order_time = 5;
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	PeriodicTrain *first = short_train, *second = long_train;
	SECTION("Short turn arrives first") {}
	SECTION("Long service arrives first") { std::swap(first, second); }
	auto plan = db.PlanPrecomputed(PAX, A, C, NOW + 10); REQUIRE(plan.ride);
	/* Anchor the saved intent to the train which will arrive second. The
	 * first train must board it without changing/replanning the chosen route. */
	REQUIRE(db.RebindPrecomputed(*plan.ride, second->orders->index, PAX, second->cur_real_order_index));
	const auto intent = *plan.ride;
	auto *packet = CargoPacket::Create(20, 7, A, TileIndex{0}, Money{17}); REQUIRE(packet->SetPassengerDestination(PAX, C));
	packet->SetPassengerJourney(intent, C);
	StationCargoList waiting; waiting.Append(packet, C); PassengerJourneyLoadingContext loading;
	CHECK_FALSE(waiting.HasCargoFor(std::array{B, C}, first, &loading, PAX)); CHECK(waiting.BoardingCandidatesInspected() == 0);
	/* Both the Packet fields and derived group membership survive a database
	 * and CargoPacket table round trip. No new persistent Packet fields. */
	AutoRestoreBackup version{_sl_version, SAVEGAME_VERSION};
	AutoRestoreBackup features{_sl_xv_feature_versions, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup static_features{_sl_xv_feature_static_versions, AutoRestoreBackupNoNewValueTag{}};
	SlXvSetStaticCurrentVersions(); _sl_xv_feature_versions = _sl_xv_feature_static_versions;
	auto *copy = CargoPacket::Create(); REQUIRE_FALSE(SlTestSaveLoadTable(packet, copy, GetCargoPacketDesc()).empty());
	CHECK(copy->GetPassengerJourney() == intent); delete copy;
	auto wire = db.Save(); REQUIRE(db.Load(wire)); db.FinishLoad(true); CHECK(db.Save() == wire);
	waiting.ClearWaitingIndex(); _state_ticks += 10;
	const auto queries = db.GetPrecomputedDiagnostic(PAX).queries;
	REQUIRE(waiting.HasCargoFor(std::array{B, C}, first, &loading, PAX));
	REQUIRE(waiting.Reserve(7, &first->cargo, std::array{B, C}, TileIndex{0}, first, &loading) == 7);
	CHECK(packet->Count() == 13); CHECK(packet->GetPassengerJourney() == intent);
	REQUIRE(waiting.Load(7, &first->cargo, std::array{B, C}, TileIndex{0}, first, &loading) == 7);
	REQUIRE(waiting.Load(13, &second->cargo, std::array{B, C}, TileIndex{0}, second, &loading) == 13);
	CHECK(waiting.AvailableCount() == 0); CHECK(waiting.ReservedCount() == 0);
	CHECK(first->cargo.StoredCount() == 7); CHECK(second->cargo.StoredCount() == 13);
	for (const auto *train : {long_train, short_train}) {
		const auto &bound = train->cargo.Packets()->front()->GetPassengerJourney();
		CHECK(bound.kind == PassengerRideKind::EstimatedBoundService); CHECK(bound.vehicle == train->index);
		CHECK(bound.service == train->orders->index); CHECK(bound.alighting == C);
		CHECK(bound.boarding_order == (train == long_train ? 0 : 1)); CHECK(bound.alighting_order == (train == long_train ? 2 : 3));
	}
	CHECK(db.GetPrecomputedDiagnostic(PAX).queries == queries);
}

TEST_CASE("Common section binding preserves live restrictions and through destinations", "[passenger][precomputed][sections][boarding]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *long_train = f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	auto *short_train = f.Train({Stop(A), Stop(B), Stop(C)});
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	auto common = db.PlanPrecomputed(PAX, A, C, NOW); REQUIRE(common.ride);
	REQUIRE(db.RebindPrecomputed(*common.ride, long_train->orders->index, PAX, 0));
	auto *packet = f.Packet(); packet->SetPassengerJourney(*common.ride, C);
	SECTION("NoLoad is checked on the actual arriving train") { short_train->current_order.SetLoadType(OrderLoadType::NoLoad); }
	SECTION("NoUnload is checked on the actual selected alighting order") { short_train->GetOrder(2)->SetUnloadType(OrderUnloadType::NoUnload); }
	SECTION("Forced unloading cannot be bypassed") { short_train->GetOrder(1)->SetUnloadType(OrderUnloadType::Transfer); }
	SECTION("New service generation is rejected") { db.Invalidate(short_train->orders->index); }
	PassengerJourneyLoadingContext loading;
	CHECK_FALSE(loading.CanBoard(*packet, short_train, PAX)); CHECK(loading.CanBoard(*packet, long_train, PAX));
	auto through = db.PlanPrecomputed(PAX, A, D, NOW, short_train->orders->index); REQUIRE(through.ride);
	CHECK(through.ride->alighting == D); CHECK_FALSE(db.RebindPrecomputed(*through.ride, short_train->orders->index, PAX, 0));
	packet->SetPassengerJourney(*through.ride, D);
	CHECK_FALSE(loading.CanBoard(*packet, short_train, PAX)); delete packet;
}

TEST_CASE("Common section boarding merges retained and replacement keys without duplicate inspections", "[passenger][precomputed][sections][boarding][save]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *long_train = f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	auto *short_train = f.Train({Stop(A), Stop(B), Stop(C)});
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	auto plan = db.PlanPrecomputed(PAX, A, C, NOW); REQUIRE(plan.ride);
	REQUIRE(db.RebindPrecomputed(*plan.ride, long_train->orders->index, PAX, 0));
	auto *packet = f.Packet(); packet->SetPassengerJourney(*plan.ride, C);
	StationCargoList waiting; waiting.Append(packet, C);
	_settings_game.linkgraph.passenger_transfer_penalty = 17; db.TickPrecomputed(1);
	REQUIRE(db.GetPrecomputedDiagnostic(PAX).retained_destinations > 0);
	auto keys = db.PrecomputedBoardingKeys(PAX, short_train->orders->index, 0);
	REQUIRE_FALSE(keys.sections[0].empty()); CHECK(std::equal(keys.sections[0].begin(), keys.sections[0].end(), keys.sections[1].begin(), keys.sections[1].end()));
	auto wire = db.Save(); REQUIRE(db.Load(wire)); db.FinishLoad(true); CHECK(db.Save() == wire);
	waiting.ClearWaitingIndex(); PassengerJourneyLoadingContext loading;
	REQUIRE(waiting.HasCargoFor(std::array{B, C}, short_train, &loading, PAX)); CHECK(waiting.BoardingCandidatesInspected() == 1);
	REQUIRE(waiting.Load(10, &short_train->cargo, std::array{B, C}, TileIndex{0}, short_train, &loading) == 10);
	CHECK(short_train->cargo.Packets()->front()->GetPassengerJourney().service == short_train->orders->index);
}

TEST_CASE("Common section boarding is independent of warm indices when a pool representative changes", "[passenger][precomputed][sections][boarding][determinism]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *earlier = f.Train({Stop(A), Stop(B), Stop(C)}); earlier->cargo_cap = 0;
	auto *long_train = f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	auto *short_train = f.Train({Stop(A), Stop(B), Stop(C)});
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	auto plan = db.PlanPrecomputed(PAX, A, C, NOW); REQUIRE(plan.ride);
	REQUIRE(db.RebindPrecomputed(*plan.ride, long_train->orders->index, PAX, 0));
	plan.ride->waiting_limit = 1000000; // Remain valid across the publication epoch under test.
	auto *packet = f.Packet(); packet->SetPassengerJourney(*plan.ride, C);
	StationCargoList waiting; waiting.Append(packet, C); PassengerJourneyLoadingContext before;
	REQUIRE(waiting.HasCargoFor(std::array{B, C}, short_train, &before, PAX));
	/* Introduce a lower-ID eligible member without changing the saved anchor.
	 * A canonical-table key in IndexPacket would now differ on a cold peer. */
	earlier->cargo_cap = 100; db.Invalidate(earlier->orders->index); f.PublishUpdates(db);
	db.TickPrecomputed(); while (!db.PrecomputedFinished()) db.TickPrecomputed();
	REQUIRE(db.IsCurrent(*plan.ride));
	const auto wire = db.Save(); const auto intent = packet->GetPassengerJourney();
	PassengerJourneyLoadingContext warm, cold;
	const bool a = waiting.HasCargoFor(std::array{B, C}, short_train, &warm, PAX);
	waiting.ClearWaitingIndex();
	const bool b = waiting.HasCargoFor(std::array{B, C}, short_train, &cold, PAX);
	CHECK(a); CHECK(a == b); CHECK(packet->GetPassengerJourney() == intent); CHECK(db.Save() == wire);
}

TEST_CASE("Common section boarding ignores unrelated destinations in the same anchor service", "[.benchmark][passenger][precomputed][sections][boarding][cpu]")
{
	Fixture f; _settings_game.linkgraph.passenger_journey_precomputed = true;
	auto *long_train = f.Train({Stop(A), Stop(B), Stop(C), Stop(D)});
	auto *short_train = f.Train({Stop(A), Stop(B), Stop(C)});
	auto &db = f.Build(); while (!db.PreparePrecomputed()) {}
	auto common = db.PlanPrecomputed(PAX, A, C, NOW); REQUIRE(common.ride);
	REQUIRE(db.RebindPrecomputed(*common.ride, long_train->orders->index, PAX, 0));
	auto through = db.PlanPrecomputed(PAX, A, D, NOW, short_train->orders->index); REQUIRE(through.ride);
	std::cout << "unrelated_packets,section_ns,section_candidates,section_groups,section_keys\n";
	for (int count : {128, 1024, 8192, 32768}) {
		StationCargoList waiting;
		for (int i = 0; i <= count; ++i) {
			auto *packet = CargoPacket::Create(1, i, A, TileIndex{0}, Money{0}); REQUIRE(packet->SetPassengerDestination(PAX, i == count ? C : D));
			packet->SetPassengerJourney(i == count ? *common.ride : *through.ride, C); waiting.Append(packet, C);
		}
		PassengerJourneyLoadingContext loading; REQUIRE(waiting.HasCargoFor(std::array{B, C}, short_train, &loading, PAX));
		const auto before = waiting.BoardingCandidatesInspected(); auto start = std::chrono::steady_clock::now();
		for (int i = 0; i < 512; ++i) REQUIRE(waiting.HasCargoFor(std::array{B, C}, short_train, &loading, PAX));
		const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count() / 512;
		CHECK(waiting.BoardingCandidatesInspected() - before == 512);
		auto keys = db.PrecomputedBoardingKeys(PAX, short_train->orders->index, 0);
		std::cout << count << ',' << elapsed << ',' << (waiting.BoardingCandidatesInspected() - before) / 512 << ','
				<< db.GetPrecomputedDiagnostic(PAX).shared_sections << ',' << keys.sections[0].size() << '\n';
	}
}
