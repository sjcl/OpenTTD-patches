/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_journey_shadow.cpp Onboard searches and side-effect-free production staging diagnostics. */
#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../passenger_journey_shadow.h"
#include "../passenger_od.h"
#include "../vehicle_base.h"
#include "../station_base.h"
#include "../economy_base.h"
#include "../core/backup_type.hpp"
#include "../core/random_func.hpp"
#include "../date_func.h"
#include "../sl/saveload.h"
#include <deque>
#include <chrono>

static constexpr CargoType PAX{0};
static constexpr StationID A{0}, B{1}, C{2}, D{3};
static constexpr StateTicks NOW{64000};
using Action = VehicleCargoList::MoveToAction;
extern std::vector<uint8_t> SlTestSaveLoadTable(void *, void *, const NamedSaveLoadTable &);
extern btree::btree_map<uint64_t, Money> _cargo_packet_deferred_payments;

static Order ShadowStop(StationID station, TimetableTicks travel = 10000, TimetableTicks wait = 10)
{
	Order order;
	order.MakeGoToStation(station);
	order.SetTravelTime(travel);
	order.SetTravelTimetabled(true);
	order.SetWaitTime(wait);
	order.SetWaitTimetabled(true);
	return order;
}

struct ShadowVehicle : Vehicle {
	explicit ShadowVehicle(std::vector<Order> orders, VehicleID id = VehicleID{100}) : Vehicle(id, VehicleType::Train)
	{
		this->orders = OrderList::Create(std::move(orders), this);
		this->cur_timetable_order_index = INVALID_VEH_ORDER_ID;
		this->current_order.MakeLoading(false);
		this->last_station_visited = this->GetOrder(0)->GetDestination().ToStationID();
		this->cargo_type = PAX;
		this->cargo_cap = 80;
		this->direction = Direction::NE;
	}
	~ShadowVehicle() { this->orders->FreeChain(); this->orders = nullptr; }
	bool IsPrimaryVehicle() const override { return true; }
};

struct ShadowFixture {
	AutoRestoreBackup<CargoClasses> classes{CargoSpec::Get(PAX)->classes, CargoClasses{CargoClass::Passengers}};
	AutoRestoreBackup<uint8_t> bitnum{CargoSpec::Get(PAX)->bitnum, uint8_t{0}};
	AutoRestoreBackup<LinkGraphSettings> settings{_settings_game.linkgraph, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<StateTicks> ticks{_state_ticks, NOW};
	AutoRestoreBackup<Money> payment{CargoSpec::Get(PAX)->current_payment, Money{0}};
	ShadowFixture()
	{
		REQUIRE(Station::GetNumItems() == 0);
		AllocateMap(64, 64);
		for (StationID id : {A, B, C, D}) Station::CreateAtIndex(id, TileIndex{0})->owner = OWNER_NONE;
		_settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Asymmetric;
		_settings_game.linkgraph.passenger_transfer_timetables = false;
		_settings_game.linkgraph.passenger_transfer_penalty = 20;
		/* Each test owns a distinct diagnostic epoch; resetting stats does not reset quota. */
		static int64_t epoch = 0;
		_state_ticks += ++epoch * 1024;
		SetPassengerJourneyShadowEnabled(true);
		ResetPassengerJourneyShadowStats();
	}
	~ShadowFixture() { SetPassengerJourneyShadowEnabled(false); _station_pool.CleanPool(); }
	CargoPacket *Packet(StationID destination = D, uint16_t count = 10)
	{
		auto packet = std::unique_ptr<CargoPacket>(CargoPacket::Create(count, 7, A, TileIndex{0}, Money{100}));
		if (destination != StationID::Invalid()) REQUIRE(packet->SetPassengerDestination(PAX, destination));
		return packet.release();
	}
	PassengerJourneyShadowLimits Limits() const
	{
		PassengerJourneyShadowLimits limits;
		limits.graph.horizon = 1000;
		return limits;
	}
};

static PassengerServiceGraph OnboardGraph(Ticks horizon = 1000)
{
	PassengerServiceGraphLimits limits;
	limits.horizon = horizon;
	return PassengerServiceGraph::Build(PAX, _state_ticks, limits);
}

struct FullTrainForecast : PassengerJourneyCapacityProvider {
	uint64_t Revision() const override { return 0; }
	PassengerJourneyCapacity Estimate(const PassengerServiceGraph &, PassengerServiceGraph::VisitID, PassengerServiceGraph::VisitID) const override
	{
		PassengerJourneyCapacity capacity;
		capacity.certainty = PassengerCapacityCertainty::Confirmed;
		capacity.future_total = 80;
		capacity.future_free = 0;
		return capacity;
	}
};

TEST_CASE("Onboard journeys reuse the search without new seats or a boarding cost", "[passenger][journey][shadow]")
{
	ShadowFixture fixture;
	ShadowVehicle local({ShadowStop(B), ShadowStop(D, 100)});
	local.GetOrder(0)->SetLoadType(OrderLoadType::NoLoad);
	const auto graph = OnboardGraph();
	auto onboard = graph.FindOnboardVisit(local.index, B, 0);
	REQUIRE(onboard);
	FullTrainForecast full;
	PassengerJourneyPlanner planner(graph, &full);
	PassengerJourneyEvaluation evaluation;
	evaluation.minimum_transfer = 20;
	evaluation.transfer_penalty = 500;
	evaluation.crowding_penalty = 1000;
	GameRandomSeedChecker random;
	auto result = planner.PlanOnboard(PAX, *onboard, D, {}, evaluation, 80);
	REQUIRE(result.status == PassengerPredictionStatus::Complete);
	REQUIRE(result.journeys.size() == 1);
	const auto &journey = result.journeys.front();
	CHECK(journey.arrival == _state_ticks + 110);
	CHECK(journey.generalized_time == StateTicksDelta{110});
	CHECK(journey.waiting == StateTicksDelta{0});
	CHECK(journey.riding == StateTicksDelta{110});
	CHECK(journey.transfers == 0);
	CHECK_FALSE(journey.capacity_uncertain);
	CHECK(result.diagnostic.capacity_rejections == 0);
	CHECK(planner.Plan(PAX, B, D, _state_ticks).IsUnreachable()); // A new passenger cannot board NoLoad.
	CHECK(planner.PlanOnboard(PAX, *onboard, D, {}, evaluation, 80).diagnostic.cache_hit);
	CHECK(random.Check());
	CHECK(planner.PlanOnboard(PAX, {SIZE_MAX, 0}, D).status == PassengerPredictionStatus::Unknown);
	CHECK_FALSE(graph.FindOnboardVisit(local.index, B, 1));
}

TEST_CASE("Onboard interchanges apply actual stop restrictions and transfer costs", "[passenger][journey][shadow]")
{
	ShadowFixture fixture;
	ShadowVehicle local({ShadowStop(B), ShadowStop(D, 300)});
	ShadowVehicle express({ShadowStop(B, 10000, 50), ShadowStop(D, 40)}, VehicleID{101});
	PassengerJourneyEvaluation evaluation;
	evaluation.minimum_transfer = evaluation.transfer_penalty = 20;
	bool expect_stay = false;
	SECTION("Faster express requires one transfer even as the first returned leg") {}
	SECTION("NoUnload prevents the initial interchange") { local.GetOrder(0)->SetUnloadType(OrderUnloadType::NoUnload); expect_stay = true; }
	SECTION("NoLoad on the express prevents boarding") { express.GetOrder(0)->SetLoadType(OrderLoadType::NoLoad); expect_stay = true; }
	SECTION("A missed connection cannot be caught") { express.GetOrder(0)->SetWaitTime(10); expect_stay = true; }
	SECTION("Forced transfer prohibits through riding") { local.GetOrder(0)->SetUnloadType(OrderUnloadType::Transfer); }
	SECTION("Forced unload prohibits through riding") { local.GetOrder(0)->SetUnloadType(OrderUnloadType::Unload); }
	const auto graph = OnboardGraph();
	PassengerJourneyPlanner planner(graph);
	auto onboard = graph.FindOnboardVisit(local.index, B, 0);
	REQUIRE(onboard);
	auto result = planner.PlanOnboard(PAX, *onboard, D, {}, evaluation);
	REQUIRE(result.status == PassengerPredictionStatus::Complete);
	REQUIRE_FALSE(result.journeys.empty());
	CHECK(result.journeys[0].legs[0].vehicle == (expect_stay ? local.index : express.index));
	CHECK(result.journeys[0].transfers == (expect_stay ? 0 : 1));
	if (!expect_stay) CHECK(result.journeys[0].generalized_time == StateTicksDelta{110});
}

TEST_CASE("Shadow compares the final legacy action with the complete destination journey", "[passenger][shadow]")
{
	ShadowFixture fixture;
	ShadowVehicle local({ShadowStop(B), ShadowStop(C, 100), ShadowStop(D, 100)});
	ShadowVehicle express({ShadowStop(B, 10000, 500), ShadowStop(D, 40)}, VehicleID{101});
	Action legacy = Action::Keep;
	OrderUnloadType unload = OrderUnloadType::UnloadIfPossible;
	StationID destination = D;
	PassengerShadowCategory expected = PassengerShadowCategory::BothStay;
	SECTION("Both stay") {}
	SECTION("Both transfer") {
		express.GetOrder(0)->SetWaitTime(50);
		legacy = Action::Transfer;
		expected = PassengerShadowCategory::BothTransfer;
	}
	SECTION("An unnecessary local to express interchange is detected") {
		legacy = Action::Transfer;
		expected = PassengerShadowCategory::LegacyTransferPlannerStay;
	}
	SECTION("A faster trunk beats a low frequency shortcut") {
		local.GetOrder(0)->SetWaitTime(500);
		express.GetOrder(0)->SetWaitTime(50);
		expected = PassengerShadowCategory::LegacyStayPlannerTransfer;
	}
	SECTION("Via station delivery differs from the final destination") {
		legacy = Action::Deliver;
		expected = PassengerShadowCategory::ArrivalMismatch;
	}
	SECTION("Arrived requires the final destination") {
		destination = B;
		legacy = Action::Deliver;
		expected = PassengerShadowCategory::BothArrived;
	}
	SECTION("Keeping passengers at their final destination is an arrival mismatch") {
		destination = B;
		expected = PassengerShadowCategory::ArrivalMismatch;
	}
	SECTION("A completely disconnected destination is unreachable within the horizon") {
		destination = StationID{4};
		Station::CreateAtIndex(destination, TileIndex{0})->owner = OWNER_NONE;
		expected = PassengerShadowCategory::Unreachable;
	}
	SECTION("NoUnload is classified independently of agreement") {
		unload = OrderUnloadType::NoUnload;
		local.GetOrder(0)->SetUnloadType(unload);
		expected = PassengerShadowCategory::Constraints;
	}
	SECTION("NoLoad still permits staying but is a constrained comparison") {
		local.GetOrder(0)->SetLoadType(OrderLoadType::NoLoad);
		expected = PassengerShadowCategory::Constraints;
	}
	SECTION("Forced transfer is a constrained comparison") {
		unload = OrderUnloadType::Transfer;
		legacy = Action::Transfer;
		local.GetOrder(0)->SetUnloadType(unload);
		expected = PassengerShadowCategory::Constraints;
	}
	SECTION("Unassigned OD is skipped") {
		destination = StationID::Invalid();
		expected = PassengerShadowCategory::Skipped;
	}
	SECTION("Manual CargoDist is skipped") {
		_settings_game.linkgraph.distribution_per_cargo[PAX] = DistributionType::Manual;
		expected = PassengerShadowCategory::Skipped;
	}
	SECTION("Known OD on another vehicle mode is skipped") {
		local.type = VehicleType::Road;
		expected = PassengerShadowCategory::Skipped;
	}
	local.cargo.Append(fixture.Packet(destination), Action::Keep);
	PassengerJourneyShadowContext context(&local, B, fixture.Limits());
	context.Observe(**local.cargo.Packets()->begin(), PAX, legacy, C, unload);
	const auto &stats = GetPassengerJourneyShadowStats();
	CHECK(stats.categories[to_underlying(expected)].passengers == 10);
	CHECK(stats.categories[to_underlying(expected)].packets == 1);
	if (destination == StationID::Invalid()) CHECK(stats.known_od.packets == 0);
	CHECK(stats.eligible_od.packets == (destination != StationID::Invalid() && local.type == VehicleType::Train &&
			_settings_game.linkgraph.GetDistributionType(PAX) != DistributionType::Manual ? 1 : 0));
}

TEST_CASE("Shadow distinguishes unknown prediction, search budget and memory limits", "[passenger][shadow]")
{
	ShadowFixture fixture;
	ShadowVehicle local({ShadowStop(B), ShadowStop(D, 100)});
	local.cargo.Append(fixture.Packet(), Action::Keep);
	auto limits = fixture.Limits();
	PassengerPredictionStatus expected = PassengerPredictionStatus::Unknown;
	SECTION("Incomplete timetable") { local.GetOrder(0)->SetWaitTimetabled(false); }
	SECTION("Query work limit") { limits.journey.work = 1; expected = PassengerPredictionStatus::BudgetExhausted; }
	SECTION("Query scratch limit") { limits.journey.memory_bytes = 1; expected = PassengerPredictionStatus::LimitReached; }
	SECTION("Graph memory limit") { limits.graph.memory_bytes = 1; expected = PassengerPredictionStatus::LimitReached; }
	SECTION("Missing loading event") { local.cur_timetable_order_index = 1; }
	PassengerJourneyShadowContext context(&local, B, limits);
	context.Observe(**local.cargo.Packets()->begin(), PAX, Action::Keep, D, OrderUnloadType::UnloadIfPossible);
	const auto &stats = GetPassengerJourneyShadowStats();
	REQUIRE(GetPassengerJourneyShadowDetails().size() == 1);
	CHECK(GetPassengerJourneyShadowDetails()[0].status == expected);
	CHECK(stats.categories[to_underlying(PassengerShadowCategory::Unknown)].packets == 1);
	CHECK(stats.budget_exhausted.packets == (expected == PassengerPredictionStatus::BudgetExhausted ? 1 : 0));
	CHECK(stats.limit_reached.packets == (expected == PassengerPredictionStatus::LimitReached ? 1 : 0));
}

TEST_CASE("Shadow arrival caches are shared across packets and never survive a new arrival", "[passenger][shadow]")
{
	ShadowFixture fixture;
	ShadowVehicle local({ShadowStop(B), ShadowStop(D, 100)});
	local.cargo.Append(fixture.Packet(), Action::Keep);
	const auto &packet = **local.cargo.Packets()->begin();
	{
		PassengerJourneyShadowContext context(&local, B, fixture.Limits());
		context.Observe(packet, PAX, Action::Keep, D, OrderUnloadType::UnloadIfPossible);
		context.Observe(packet, PAX, Action::Transfer, D, OrderUnloadType::UnloadIfPossible);
		CHECK(GetPassengerJourneyShadowStats().cache_hits == 1);
		CHECK(GetPassengerJourneyShadowStats().builds == 1);
		/* A capture cannot be reused after time advances, even inside a context. */
		_state_ticks += 64;
		context.Observe(packet, PAX, Action::Keep, D, OrderUnloadType::UnloadIfPossible);
		CHECK(GetPassengerJourneyShadowStats().categories[to_underlying(PassengerShadowCategory::Skipped)].packets == 1);
	}
	local.GetOrder(0)->SetWaitTimetabled(false);
	PassengerJourneyShadowContext fresh(&local, B, fixture.Limits());
	fresh.Observe(packet, PAX, Action::Keep, D, OrderUnloadType::UnloadIfPossible);
	CHECK(GetPassengerJourneyShadowStats().builds == 2);
	CHECK(GetPassengerJourneyShadowDetails().back().planner == PassengerShadowDecision::Unknown);
}

TEST_CASE("4096 distinct packet observations obey the shared shadow quota", "[passenger][shadow][performance]")
{
	ShadowFixture fixture;
	ShadowVehicle local({ShadowStop(B), ShadowStop(D, 100)});
	local.cargo.Append(fixture.Packet(), Action::Keep);
	std::vector<std::unique_ptr<CargoPacket>> packets;
	for (size_t i = 0; i < 4096; ++i) packets.emplace_back(fixture.Packet());
	PassengerJourneyShadowContext context(&local, B, fixture.Limits());
	GameRandomSeedChecker random;
	for (const auto &packet : packets) context.Observe(*packet, PAX, Action::Transfer, C, OrderUnloadType::UnloadIfPossible);
	const auto &stats = GetPassengerJourneyShadowStats();
	CHECK(stats.observed.packets == 4096);
	CHECK(stats.evaluated.packets == 512);
	CHECK(stats.categories[to_underlying(PassengerShadowCategory::Skipped)].packets == 3584);
	CHECK(stats.cache_hits == 511);
	CHECK(stats.builds == 1);
	CHECK(stats.max_work <= 65536);
	CHECK(stats.work <= 524288);
	CHECK(stats.scratch_peak <= 8 * 1024 * 1024);
	CHECK(GetPassengerJourneyShadowDetails().size() == 64);
	CHECK(random.Check());
	/* Another train arriving in this epoch cannot multiply the fleet-build allowance. */
	PassengerJourneyShadowContext next(&local, B, fixture.Limits());
	next.Observe(*packets.front(), PAX, Action::Keep, D, OrderUnloadType::UnloadIfPossible);
	CHECK(GetPassengerJourneyShadowStats().builds == 1);
}

TEST_CASE("Shadow enabled and disabled produce identical production staging and saved packets", "[passenger][shadow][determinism]")
{
	ShadowFixture fixture;
	ShadowVehicle local({ShadowStop(B), ShadowStop(C, 100), ShadowStop(D, 100)});
	ShadowVehicle express({ShadowStop(B, 10000, 50), ShadowStop(D, 40)}, VehicleID{101});
	GoodsEntry ge;
	ge.CreateData().flows.insert(FlowStat(A, D, 60));
	ge.CreateData().flows.find(A)->AppendShare(C, 40);
	std::array next{C};
	OrderUnloadType unload = OrderUnloadType::UnloadIfPossible;
	SECTION("Normal mixed random flow staging") {}
	SECTION("Force unload") { unload = OrderUnloadType::Unload; }
	SECTION("Force transfer") { unload = OrderUnloadType::Transfer; }
	SECTION("No unload") { unload = OrderUnloadType::NoUnload; }
	SECTION("Timetable legacy decisions") { _settings_game.linkgraph.passenger_transfer_timetables = true; }
	struct Outcome {
		std::vector<uint8_t> bytes;
		std::array<uint, 4> counts{};
		Money visual_profit, visual_transfer, route_profit, vehicle_profit;
		Randomizer random;
		uint64_t od_revision;
		PassengerOdStats od_stats;
		std::vector<std::tuple<size_t, uint32_t, Money>> deferred;
	};
	const Randomizer initial_random = _random;
	auto stage = [&](bool enabled) {
		SetPassengerJourneyShadowEnabled(enabled);
		_random = initial_random;
		local.cargo.Truncate(local.cargo.StoredCount());
		local.profit_this_year = 0;
		for (uint16_t i = 0; i < 12; ++i) {
			CargoPacket *packet = CargoPacket::Create(i + 1, i + 1, A, TileIndex{0}, Money{100});
			REQUIRE(packet->SetPassengerDestination(PAX, D));
			local.cargo.Append(packet, Action::Keep);
		}
		CargoPayment payment(CargoPaymentID::Invalid(), &local);
		PassengerJourneyShadowContext shadow(&local, B, fixture.Limits());
		PassengerRoutingContext routing(&local, B);
		local.cargo.Stage(false, B, next, unload, &ge, PAX, &payment, TileIndex{0}, &routing, &shadow);
		Outcome result;
		size_t position = 0;
		for (const CargoPacket *packet : *local.cargo.Packets()) {
			CargoPacket restored(CargoPacketID::Invalid());
			auto bytes = SlTestSaveLoadTable(const_cast<CargoPacket *>(packet), &restored, GetCargoPacketDesc());
			result.bytes.insert(result.bytes.end(), bytes.begin(), bytes.end());
			for (const auto &[key, value] : _cargo_packet_deferred_payments) {
				if (key >> 32 == packet->index) result.deferred.emplace_back(position, static_cast<uint32_t>(key), value);
			}
			++position;
		}
		for (size_t i = 0; i < 4; ++i) result.counts[i] = local.cargo.ActionCount(static_cast<Action>(i));
		result.visual_profit = payment.visual_profit;
		result.visual_transfer = payment.visual_transfer;
		result.route_profit = payment.route_profit;
		result.vehicle_profit = local.profit_this_year;
		result.random = _random;
		result.od_revision = _passenger_od_revision;
		result.od_stats = _passenger_od_stats;
		return result;
	};
	const auto disabled = stage(false), enabled = stage(true);
	CHECK(disabled.bytes == enabled.bytes);
	CHECK(disabled.counts == enabled.counts);
	CHECK(disabled.visual_profit == enabled.visual_profit);
	CHECK(disabled.visual_transfer == enabled.visual_transfer);
	CHECK(disabled.route_profit == enabled.route_profit);
	CHECK(disabled.vehicle_profit == enabled.vehicle_profit);
	CHECK(disabled.random.state[0] == enabled.random.state[0]);
	CHECK(disabled.random.state[1] == enabled.random.state[1]);
	CHECK(disabled.od_revision == enabled.od_revision);
	CHECK(disabled.od_stats.generated_passengers == enabled.od_stats.generated_passengers);
	CHECK(disabled.deferred == enabled.deferred);
}

TEST_CASE("Thousands of disconnected untimed lines do not poison a complete trunk journey", "[passenger][journey][shadow][performance]")
{
	ShadowFixture fixture;
	ShadowVehicle trunk({ShadowStop(B), ShadowStop(D, 100)});
	std::deque<ShadowVehicle> locals;
	for (uint16_t line = 0; line < 4000; ++line) {
		auto stop = ShadowStop(StationID{static_cast<uint16_t>(4 + line * 2)});
		stop.SetWaitTimetabled(false);
		locals.emplace_back(std::vector<Order>{stop, ShadowStop(StationID{static_cast<uint16_t>(5 + line * 2)})}, VehicleID{static_cast<uint32_t>(1000 + line)});
	}
	const auto graph = OnboardGraph();
	REQUIRE(graph.GetTrips().size() == 4001);
	CHECK(graph.GetUncertainStations().size() >= 8000);
	CHECK(graph.GetUncertainStations(D).empty());
	PassengerJourneyPlanner planner(graph);
	auto onboard = graph.FindOnboardVisit(trunk.index, B, 0);
	REQUIRE(onboard);
	auto result = planner.PlanOnboard(PAX, *onboard, D);
	CHECK(result.status == PassengerPredictionStatus::Complete);
	CHECK(result.diagnostic.reverse_work < 100);
	CHECK_FALSE(result.IsUnreachable());
	/* Connecting just one untimed declared branch keeps the unknown alternative possible. */
	locals.front().GetOrder(0)->SetDestination(B);
	locals.front().GetOrder(1)->SetDestination(D);
	const auto connected = OnboardGraph();
	PassengerJourneyPlanner uncertain(connected);
	auto connected_onboard = connected.FindOnboardVisit(trunk.index, B, 0);
	REQUIRE(connected_onboard);
	auto unknown = uncertain.PlanOnboard(PAX, *connected_onboard, D);
	CHECK(unknown.status == PassengerPredictionStatus::Unknown);
	CHECK_FALSE(unknown.IsUnreachable());
	WARN("Untimed national fixture: 8004 declared stations, 4001 trains; trunk reverse work=" << result.diagnostic.reverse_work << "; graph memory envelope=" << graph.GetMemoryBound());
}

TEST_CASE("Shadow measures a complete 8001 station fleet within its production quotas", "[passenger][shadow][performance]")
{
	ShadowFixture fixture;
	Station::CreateAtIndex(StationID{8000}, TileIndex{0})->owner = OWNER_NONE;
	std::deque<ShadowVehicle> fleet;
	for (uint16_t route = 0; route < 80; ++route) {
		std::vector<Order> orders{ShadowStop(A, 100000, 1)};
		for (uint16_t station = 1; station <= 100; ++station) orders.push_back(ShadowStop(StationID{static_cast<uint16_t>(route * 100 + station)}, 1, 1));
		for (uint16_t train = 0; train < 10; ++train) fleet.emplace_back(orders, VehicleID{static_cast<uint32_t>(1000 + route * 10 + train)});
	}
	auto &train = fleet.back();
	train.cargo.Append(fixture.Packet(StationID{8000}), Action::Keep);
	const auto start = std::chrono::steady_clock::now();
	PassengerJourneyShadowContext context(&train, A); // Production defaults, including the full horizon.
	context.Observe(**train.cargo.Packets()->begin(), PAX, Action::Keep, StationID{7901}, OrderUnloadType::UnloadIfPossible);
	const auto &stats = GetPassengerJourneyShadowStats();
	CHECK(stats.categories[to_underlying(PassengerShadowCategory::BothStay)].packets == 1);
	CHECK(stats.unknown.packets == 0);
	CHECK(stats.max_work <= 65536);
	CHECK(stats.graph_memory_bound <= 1024ULL * 1024 * 1024);
	CHECK(stats.scratch_peak <= 8 * 1024 * 1024);
	CHECK(stats.builds == 1);
	const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	WARN("National shadow: " << us << " us total; graph=" << stats.graph_us << " us; search=" << stats.search_us << " us; work=" << stats.max_work << "; scratch=" << stats.scratch_peak << "; graph envelope=" << stats.graph_memory_bound);
}
