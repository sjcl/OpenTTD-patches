/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_journey_active.cpp Deterministic journey execution, without forecast seat bookings. */
#include "stdafx.h"
#include "passenger_journey_active.h"
#include "passenger_service_database.h"
#include "vehicle_base.h"
#include "station_base.h"
#include "settings_type.h"
#include "date_func.h"
#include "window_func.h"
#include "sl/saveload.h"
#include "debug.h"
#include "linkgraph/linkgraph.h"
#include <set>
#include <chrono>

#include "safeguards.h"

namespace {
	static constexpr size_t FAIR_GRAPH_WORK = 2 * 1024 * 1024, FAIR_QUERY_WORK = 128 * 1024;
	struct JourneyFairShare {
		StationID station = StationID::Invalid();
		bool claimed = false;
	};
	struct JourneyTickBudget {
		StateTicks tick = INVALID_STATE_TICKS;
		size_t graph = 16 * 1024 * 1024;
		size_t query = 1024 * 1024;
		size_t snapshots = 16;
		size_t od_fallback = 4 * 1024 * 1024;
		PassengerJourneyTickDiagnostic diagnostic;
		std::array<JourneyFairShare, 4> fair_shares;
		bool fair_shares_initialized = false;
	};
	static JourneyTickBudget tick_budget;
	static PassengerJourneyTickDiagnostic total_diagnostic;
	static size_t journey_timer_depth = 0;
	struct CounterTimer {
		uint64_t &counter;
		std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
		~CounterTimer() { this->counter += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - this->start).count(); }
	};
	struct ResidentDistances {
		std::shared_ptr<const std::vector<uint32_t>> hops;
		size_t work, bytes;
		uint64_t used;
	};
	using BindingNodes = std::vector<std::pair<StationID, bool>>;
	struct ResidentBinding { std::shared_ptr<const BindingNodes> nodes; uint64_t token; size_t bytes; };
	static std::map<std::pair<LinkGraphID, uint64_t>, ResidentBinding> resident_bindings;
	static size_t resident_binding_bytes = 0;
	static uint64_t binding_token = 0, distance_use = 0;
	using ReverseKey = std::tuple<LinkGraphID, uint64_t, uint64_t, uint64_t>;
	struct ReverseIndex { std::vector<uint32_t> offsets; std::vector<NodeID> incoming; size_t bytes; };
	static std::map<ReverseKey, std::shared_ptr<const ReverseIndex>> reverse_indices;
	static size_t reverse_bytes = 0;
	using DistanceKey = std::tuple<CargoType, StationID, LinkGraphID, uint64_t, uint64_t, uint64_t, NodeID>;
	static std::map<DistanceKey, ResidentDistances> resident_distances;
	static size_t resident_distance_bytes = 0;
	static void AddDiagnostic(PassengerJourneyTickDiagnostic &a, const PassengerJourneyTickDiagnostic &b)
	{
		for (size_t i = 0; i < a.graph_failures.size(); ++i) a.graph_failures[i] += b.graph_failures[i];
		for (size_t i = 0; i < a.prediction_stops.size(); ++i) a.prediction_stops[i] += b.prediction_stops[i];
		if (a.first_graph_failure.reason == PassengerGraphFailure::None) a.first_graph_failure = b.first_graph_failure;
		if (a.first_prediction_failure.reason == PassengerPredictionStop::None) a.first_prediction_failure = b.first_prediction_failure;
		if (a.opaque_service == OrderListID::Invalid()) { a.opaque_service = b.opaque_service; a.opaque_vehicle = b.opaque_vehicle; }
		a.complete_graphs += b.complete_graphs; a.admitted_visits += b.admitted_visits;
		a.timing_uncertainty += b.timing_uncertainty; a.path_uncertainty += b.path_uncertainty; a.uncertainty_resolved += b.uncertainty_resolved;
		a.timed_routes_ignoring_unknown += b.timed_routes_ignoring_unknown; a.timed_budget_deferred += b.timed_budget_deferred;
		a.untimed_routes += b.untimed_routes; a.untimed_no_route += b.untimed_no_route; a.untimed_budget_deferred += b.untimed_budget_deferred;
		a.untimed_boarded_passengers += b.untimed_boarded_passengers;
		a.untimed_transferred_passengers += b.untimed_transferred_passengers;
		a.untimed_delivered_passengers += b.untimed_delivered_passengers;
		a.schedule_packets += b.schedule_packets; a.schedule_passengers += b.schedule_passengers; a.schedule_deferred_packets += b.schedule_deferred_packets;
		a.schedule_boarded_passengers += b.schedule_boarded_passengers; a.schedule_transferred_passengers += b.schedule_transferred_passengers; a.schedule_delivered_passengers += b.schedule_delivered_passengers;
		a.schedule_connected_passengers += b.schedule_connected_passengers; a.timed_connected_passengers += b.timed_connected_passengers;
		a.schedule_assisted_delivered_passengers += b.schedule_assisted_delivered_passengers;
		a.schedule_expired += b.schedule_expired; a.schedule_stale += b.schedule_stale;
		for (size_t i = 0; i < a.schedule_invalidations.size(); ++i) a.schedule_invalidations[i] += b.schedule_invalidations[i];
		a.schedule_expected_wait += b.schedule_expected_wait; a.schedule_actual_wait += b.schedule_actual_wait; a.schedule_wait_samples += b.schedule_wait_samples;
		for (size_t i = 0; i < b.query_sample_count && a.query_sample_count < a.query_samples.size(); ++i) a.query_samples[a.query_sample_count++] = b.query_samples[i];
		a.simulation_ticks += b.simulation_ticks; a.simulation_microseconds += b.simulation_microseconds;
		a.maximum_simulation_microseconds = std::max(a.maximum_simulation_microseconds, b.maximum_simulation_microseconds);
		a.journey_microseconds += b.journey_microseconds;
		a.waiting_microseconds += b.waiting_microseconds; a.fallback_distance_microseconds += b.fallback_distance_microseconds; a.boarding_microseconds += b.boarding_microseconds;
		a.maximum_journey_microseconds = std::max(a.maximum_journey_microseconds, b.journey_microseconds);
		for (size_t i = 0; i < a.simulation_histogram.size(); ++i) { a.simulation_histogram[i] += b.simulation_histogram[i]; a.journey_histogram[i] += b.journey_histogram[i]; }
		a.eligible_services += b.eligible_services; a.selected_services += b.selected_services; a.selected_trips += b.selected_trips;
		a.largest_block_services = std::max(a.largest_block_services, b.largest_block_services);
		a.maximum_unpredicted_ticks = std::max(a.maximum_unpredicted_ticks, b.maximum_unpredicted_ticks);
		a.builds += b.builds; a.static_cache_hits += b.static_cache_hits; a.queries += b.queries; a.query_cache_hits += b.query_cache_hits;
		a.trains += b.trains; a.cargo_capable += b.cargo_capable; a.possible_refit += b.possible_refit; a.predicted += b.predicted; a.visits += b.visits;
		a.excluded_cargo += b.excluded_cargo; a.excluded_scope += b.excluded_scope; a.inactive += b.inactive; a.omitted_trains += b.omitted_trains; a.truncated_predictions += b.truncated_predictions;
		a.graph_work += b.graph_work; a.query_work += b.query_work; a.reused_graph_work += b.reused_graph_work;
		a.maximum_graph_work = std::max(a.maximum_graph_work, b.maximum_graph_work);
		a.journeys += b.journeys; a.passengers += b.passengers;
		a.graph_memory_bound = std::max(a.graph_memory_bound, b.graph_memory_bound);
		a.graph_allocation_bytes = std::max(a.graph_allocation_bytes, b.graph_allocation_bytes);
		a.static_allocation_bytes = std::max(a.static_allocation_bytes, b.static_allocation_bytes);
		a.query_scratch_bytes = std::max(a.query_scratch_bytes, b.query_scratch_bytes);
		a.query_cache_bytes = std::max(a.query_cache_bytes, b.query_cache_bytes);
		a.build_microseconds += b.build_microseconds;
		for (size_t i = 0; i < a.graph_phase_microseconds.size(); ++i) a.graph_phase_microseconds[i] += b.graph_phase_microseconds[i];
		a.capture_discovery_hits += b.capture_discovery_hits; a.capture_service_hits += b.capture_service_hits; a.capture_reused_work += b.capture_reused_work;
		a.capture_allocation_bytes = std::max(a.capture_allocation_bytes, b.capture_allocation_bytes);
		a.prediction_executions += b.prediction_executions; a.prediction_cache_hits += b.prediction_cache_hits;
		a.prediction_work += b.prediction_work; a.prediction_executed_visits += b.prediction_executed_visits;
		a.prediction_nanoseconds += b.prediction_nanoseconds; a.prediction_cache_bytes = std::max(a.prediction_cache_bytes, b.prediction_cache_bytes);
		a.maximum_build_microseconds = std::max(a.maximum_build_microseconds, b.maximum_build_microseconds);
		a.query_microseconds += b.query_microseconds;
		a.fair_share_work += b.fair_share_work; a.fair_share_claims += b.fair_share_claims;
		a.fair_share_releases += b.fair_share_releases; a.retained_rides += b.retained_rides;
		a.od_fallback_work += b.od_fallback_work; a.od_fallback_searches += b.od_fallback_searches;
		a.od_fallback_cache_hits += b.od_fallback_cache_hits; a.od_fallback_cache_bytes = std::max(a.od_fallback_cache_bytes, b.od_fallback_cache_bytes);
		a.boarded_passengers += b.boarded_passengers; a.transferred_passengers += b.transferred_passengers; a.delivered_passengers += b.delivered_passengers;
		a.depot_service_boarded_passengers += b.depot_service_boarded_passengers; a.depot_service_delivered_passengers += b.depot_service_delivered_passengers;
		a.depot_crossing_boarded_passengers += b.depot_crossing_boarded_passengers; a.depot_crossing_delivered_passengers += b.depot_crossing_delivered_passengers;
		a.maximum_tick_microseconds = std::max({a.maximum_tick_microseconds, b.maximum_tick_microseconds, b.build_microseconds + b.query_microseconds});
		for (size_t i = 0; i < a.fallbacks.size(); ++i) { a.fallbacks[i] += b.fallbacks[i]; a.deferred[i] += b.deferred[i]; }
	}
	static size_t UnclaimedShares(const JourneyTickBudget &global)
	{
		return std::count_if(global.fair_shares.begin(), global.fair_shares.end(), [](const auto &share) { return share.station != StationID::Invalid() && !share.claimed; });
	}
	static size_t Unreserved(size_t remaining, size_t reserved) { return remaining > reserved ? remaining - reserved : 0; }
	static bool IsJourneyRetryTick(StationID station)
	{
		const bool turn = Station::GetNumItems() <= 512 || ((station.base() >> 6) & 15) == ((_state_ticks.base() >> 6) & 15);
		return turn && (_state_ticks.base() & 63) == (station.base() & 63);
	}
	static bool CanSearchWaitingJourney(const CargoPacket &packet, StationID station, bool retry_tick)
	{
		return packet.GetPassengerRoutingMode() != PassengerRoutingMode::Journey &&
				(retry_tick || packet.GetPassengerRoutingMode() == PassengerRoutingMode::Legacy) &&
				Station::IsValidID(packet.GetPassengerDestination()) && packet.GetPassengerDestination() != station &&
				packet.CanRetryPassengerJourney(station, _state_ticks);
	}
	/** Called before the station loading loop mutates countdowns or waiting queues.
	 * Reserve only for due loading passes containing an actually searchable OD.
	 * A bounded census avoids replacing a routing bottleneck with a packet scan. */
	static void InitializeFairShares(JourneyTickBudget &global)
	{
		if (global.fair_shares_initialized) return;
		global.fair_shares_initialized = true;
		if (!_settings_game.linkgraph.passenger_journey_routing) return;
		size_t budget = std::min<size_t>(65536, global.graph), allowance = budget;
		auto spend = [&]() { if (budget == 0) return false; --budget; return true; };
		std::array<CargoType, NUM_CARGO> cargos;
		size_t count = 0;
		for (CargoType cargo : EnumRange(NUM_CARGO)) {
			if (!spend()) break;
			if (IsPassengerJourneyRoutingEnabled(cargo)) cargos[count++] = cargo;
		}
		if (count == 0) {
			global.graph -= allowance - budget; global.diagnostic.fair_share_work += allowance - budget;
			return;
		}
		std::vector<StationID> origins;
		origins.reserve(Station::GetNumItems()); // At most one ID per station; transient, never saved.
		auto searchable = [&](const Station *station) {
			bool due = false;
			for (const Vehicle *vehicle : station->loading_vehicles) {
				if (!spend()) return false;
				if (vehicle->load_unload_ticks == 1 && !vehicle->vehstatus.Any({VehState::Stopped, VehState::Crashed}) &&
						!vehicle->current_order.IsType(OT_LOADING_ADVANCE)) { due = true; break; }
			}
			if (!due) return false;
			for (size_t i = 0; i < count; ++i) {
				if (!spend()) return false;
				const GoodsEntry &goods = station->goods[cargos[i]];
				if (goods.data == nullptr || goods.CargoAvailableCount() == 0) continue;
				for (StationCargoList::ConstIterator it(goods.data->cargo.Packets()->begin()); it != goods.data->cargo.Packets()->end(); ++it) {
					if (!spend()) return false;
					if (CanSearchWaitingJourney(**it, station->index, IsJourneyRetryTick(station->index))) return true;
				}
			}
			return false;
		};
		for (const Station *station : Station::Iterate()) {
			if (!spend()) break;
			if (searchable(station)) origins.push_back(station->index);
		}
		global.graph -= allowance - budget; global.diagnostic.fair_share_work += allowance - budget;
		/* If the census cannot finish, reserve nothing. Ordinary queries retain all
		 * remaining allowance rather than losing half to unverified stations. */
		if (budget == 0 || origins.empty()) return;
		const size_t shares = std::min({global.fair_shares.size(), origins.size(), global.graph / FAIR_GRAPH_WORK, global.query / FAIR_QUERY_WORK, global.snapshots});
		if (shares == 0) return;
		const size_t first = (static_cast<uint64_t>(_state_ticks.base()) * shares) % origins.size();
		for (size_t i = 0; i < shares; ++i) global.fair_shares[i].station = origins[(first + i) % origins.size()];
	}
	static JourneyTickBudget &TickBudget()
	{
		if (tick_budget.tick != _state_ticks) BeginPassengerJourneyTick();
		return tick_budget;
	}
	static void CountJourneyFallback(PassengerJourneyFallbackReason reason)
	{
		++TickBudget().diagnostic.fallbacks[static_cast<size_t>(reason)];
	}
	static void CountJourneyDeferral(PassengerJourneyFallbackReason reason)
	{
		++TickBudget().diagnostic.deferred[static_cast<size_t>(reason)];
	}
	static PassengerJourneyFallbackReason JourneySearchFailure(const PassengerJourneyResult &result)
	{
		switch (result.stop_reason) {
			case PassengerJourneyStopReason::WorkLimit: return PassengerJourneyFallbackReason::QueryWork;
			case PassengerJourneyStopReason::MemoryLimit: return PassengerJourneyFallbackReason::QueryMemory;
			case PassengerJourneyStopReason::LabelLimit:
			case PassengerJourneyStopReason::TransferLimit: return PassengerJourneyFallbackReason::QueryLimit;
			case PassengerJourneyStopReason::PredictionHorizon: return PassengerJourneyFallbackReason::PredictionHorizon;
			default: return result.status == PassengerPredictionStatus::Complete ? PassengerJourneyFallbackReason::NoJourney : PassengerJourneyFallbackReason::PredictionIncomplete;
		}
	}
	static void CountJourneySearch(const PassengerJourneyResult &result, const PassengerJourneyPlanner &planner, std::chrono::steady_clock::time_point start, StationID origin, StationID destination)
	{
		auto &diagnostic = TickBudget().diagnostic;
		diagnostic.query_scratch_bytes = std::max(diagnostic.query_scratch_bytes, result.diagnostic.scratch_bytes);
		diagnostic.query_cache_bytes = std::max(diagnostic.query_cache_bytes, planner.GetCacheBytes());
		diagnostic.uncertainty_resolved += result.diagnostic.uncertainty_resolved;
		if (diagnostic.query_sample_count < diagnostic.query_samples.size()) {
			diagnostic.query_samples[diagnostic.query_sample_count++] = {origin, destination, _state_ticks, result.status, result.stop_reason, result.journeys.size(),
					result.HasJourney() ? std::optional(result.journeys.front().generalized_time) : std::nullopt, result.diagnostic.unknown_arrival_bound, result.diagnostic.unknown_service};
		}
		diagnostic.query_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	}
}

void BeginPassengerJourneyTick()
{
	AddDiagnostic(total_diagnostic, tick_budget.diagnostic);
	tick_budget = {}; tick_budget.tick = _state_ticks;
}

void RecordPassengerJourneySimulationTick(uint64_t microseconds)
{
	auto &diagnostic = TickBudget().diagnostic;
	++diagnostic.simulation_ticks;
	diagnostic.simulation_microseconds += microseconds;
	diagnostic.maximum_simulation_microseconds = std::max(diagnostic.maximum_simulation_microseconds, microseconds);
	++diagnostic.simulation_histogram[std::min<size_t>(2047, microseconds / 1000)];
	++diagnostic.journey_histogram[std::min<size_t>(2047, diagnostic.journey_microseconds / 1000)];
}
PassengerJourneyWorkTimer::PassengerJourneyWorkTimer() : started(0)
{
	if (journey_timer_depth++ == 0) this->started = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
PassengerJourneyWorkTimer::~PassengerJourneyWorkTimer()
{
	assert(journey_timer_depth > 0);
	if (--journey_timer_depth == 0) TickBudget().diagnostic.journey_microseconds += (std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() - this->started) / 1000;
}
void PreparePassengerJourneyLoading() { PassengerJourneyWorkTimer timer; if (!_settings_game.linkgraph.passenger_journey_periodic) InitializeFairShares(TickBudget()); }
void ReleasePassengerJourneyWaitingShare(StationID station)
{
	if (!_settings_game.linkgraph.passenger_journey_routing) return;
	auto &global = TickBudget();
	for (auto &share : global.fair_shares) if (share.station == station && !share.claimed) {
		share.claimed = true;
		++global.diagnostic.fair_share_releases;
	}
}
const PassengerJourneyTickDiagnostic &GetPassengerJourneyTickDiagnostic() { return tick_budget.diagnostic; }
PassengerJourneyTickDiagnostic GetPassengerJourneyTotalDiagnostic()
{
	auto result = total_diagnostic;
	AddDiagnostic(result, tick_budget.diagnostic);
	return result;
}
void ResetPassengerJourneyDiagnostic() { total_diagnostic = {}; tick_budget.diagnostic = {}; GetPassengerServiceDatabase().ResetDiagnostic(); }
void ClearPassengerOdFallbackCache()
{
	resident_distances.clear(); resident_distance_bytes = 0; resident_bindings.clear(); resident_binding_bytes = 0;
	reverse_indices.clear(); reverse_bytes = 0; binding_token = distance_use = 0;
}
void RecordPassengerJourneyFallback(PassengerJourneyFallbackReason reason) { CountJourneyFallback(reason); }
void RecordPassengerScheduleInvalidation(PassengerScheduleInvalidation reason) { ++TickBudget().diagnostic.schedule_invalidations[static_cast<size_t>(reason)]; }
void RecordPassengerJourneyMovement(PassengerJourneyMovement movement, uint count, bool timed)
{
	auto &diagnostic = TickBudget().diagnostic;
	switch (movement) {
		case PassengerJourneyMovement::Board: (timed ? diagnostic.boarded_passengers : diagnostic.untimed_boarded_passengers) += count; break;
		case PassengerJourneyMovement::Transfer: (timed ? diagnostic.transferred_passengers : diagnostic.untimed_transferred_passengers) += count; break;
		case PassengerJourneyMovement::Deliver: (timed ? diagnostic.delivered_passengers : diagnostic.untimed_delivered_passengers) += count; break;
	}
}

/** Known-OD transport actually handled by the untimed policy, including budget
 * deferred Legacy packets. Neither unknown-OD CargoDist nor reservations count. */
void RecordPassengerJourneyMovement(PassengerJourneyMovement movement, const CargoPacket &packet, uint count)
{
	if (movement == PassengerJourneyMovement::Deliver && packet.HasUsedPassengerSchedule() && _settings_game.linkgraph.passenger_journey_routing && Station::IsValidID(packet.GetPassengerDestination())) TickBudget().diagnostic.schedule_assisted_delivered_passengers += count;
	if (movement == PassengerJourneyMovement::Board && packet.GetPassengerArrivingService() != OrderListID::Invalid()) {
		auto &d = TickBudget().diagnostic;
		if (packet.GetPassengerInterchangeTransport() == PassengerTransportKind::Schedule) d.schedule_connected_passengers += count;
		if (packet.GetPassengerInterchangeTransport() == PassengerTransportKind::Timed) d.timed_connected_passengers += count;
	}
	auto kind = packet.GetPassengerTransport();
	/* Last carrier is retained while the next leg is planned during staging.
	 * This observes the people actually moved, including partial unloading. */
	if (kind == PassengerTransportKind::Schedule || (kind == PassengerTransportKind::None && packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey && IsEstimatedPassengerRide(packet.GetPassengerJourney().kind))) {
		auto &d = TickBudget().diagnostic;
		switch (movement) {
			case PassengerJourneyMovement::Board:
				d.schedule_boarded_passengers += count;
				if (IsEstimatedPassengerRide(packet.GetPassengerJourney().kind)) {
					const auto &ride = packet.GetPassengerJourney();
					d.schedule_expected_wait += uint64_t(ride.estimated_wait) * count;
					d.schedule_actual_wait += uint64_t(std::max<int64_t>(0, _state_ticks.base() - ride.planned_at.base())) * count;
					d.schedule_wait_samples += count;
				}
				break;
			case PassengerJourneyMovement::Transfer: d.schedule_transferred_passengers += count; break;
			case PassengerJourneyMovement::Deliver: d.schedule_delivered_passengers += count; break;
		}
		return;
	}
	if (kind == PassengerTransportKind::Untimed) { RecordPassengerJourneyMovement(movement, count, false); return; }
	if (kind == PassengerTransportKind::Timed || packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey) {
		RecordPassengerJourneyMovement(movement, count, true);
		/* Observe actual loading/delivery, not reservations or plan adoption.
		 * This bounded read changes no execution, routing budget or saved state. */
		if (movement != PassengerJourneyMovement::Board && movement != PassengerJourneyMovement::Deliver) return;
		const auto &ride = packet.GetPassengerJourney();
		const OrderList *list = OrderList::GetIfValid(ride.service);
		if (list == nullptr || (ride.kind != PassengerRideKind::VehicleSnapshot && list->GetCargoDistRouteGeneration() != ride.signature) || list->GetNumOrders() > 4096 ||
				ride.boarding_order >= list->GetNumOrders() || ride.alighting_order >= list->GetNumOrders()) return;
		const size_t count_orders = list->GetNumOrders();
		const size_t end = (ride.alighting_order + count_orders - ride.boarding_order) % count_orders;
		bool depot_service = false, crossing = false;
		for (size_t i = 0; i < count_orders; ++i) if (list->GetOrderAt(i)->IsType(OT_GOTO_DEPOT)) {
			depot_service = true;
			const size_t offset = (i + count_orders - ride.boarding_order) % count_orders;
			crossing |= offset > 0 && offset <= end;
		}
		auto &diagnostic = TickBudget().diagnostic;
		if (depot_service) (movement == PassengerJourneyMovement::Board ? diagnostic.depot_service_boarded_passengers : diagnostic.depot_service_delivered_passengers) += count;
		if (crossing) (movement == PassengerJourneyMovement::Board ? diagnostic.depot_crossing_boarded_passengers : diagnostic.depot_crossing_delivered_passengers) += count;
	} else if (_settings_game.linkgraph.passenger_journey_routing && Station::IsValidID(packet.GetPassengerDestination()) &&
			(movement != PassengerJourneyMovement::Board || Station::IsValidID(packet.GetPassengerVia()))) {
		RecordPassengerJourneyMovement(movement, count, false);
	}
}

PassengerServiceGraphLimits ActivePassengerServiceGraphLimits()
{
	PassengerServiceGraphLimits limits;
	limits.work = 2 * 1024 * 1024;
	limits.memory_bytes = 128 * 1024 * 1024;
	limits.total_visits = 262144;
	limits.visits_per_trip = 4096;
	return limits;
}

bool IsPassengerJourneyRoutingEnabled(CargoType cargo)
{
	return _settings_game.linkgraph.passenger_journey_routing && cargo < NUM_CARGO && CargoSpec::Get(cargo)->IsValid() &&
			IsCargoInClass(cargo, CargoClass::Passengers) && _settings_game.linkgraph.GetDistributionType(cargo) != DistributionType::Manual;
}

/** Stable value hash, excluding occupancy and GUI names. All predictor inputs in orders/schedules are included.
 * Conditional orders remain unforecastable; their hash still invalidates an already selected leg. */
uint64_t PassengerJourneyServiceSignature(const OrderList &orders, CargoType cargo, size_t *budget)
{
	size_t local_budget = 65536;
	if (budget == nullptr) budget = &local_budget;
	auto spend = [&](size_t amount) { if (amount > *budget) return false; *budget -= amount; return true; };
	uint64_t hash = 14695981039346656037ULL;
	auto add = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
	add(orders.GetNumOrders());
	add(orders.GetNumVehicles());
	for (const Order &order : orders.GetOrderVector()) {
		if (!spend(32 + 2 * NUM_CARGO)) return 0;
		add(order.GetType()); add(order.GetDestination().base()); add(order.GetRawFlags());
		add(order.IsWaitFixed()); add(order.IsTravelFixed());
		add(order.GetXData()); add(order.GetXData2()); add(order.GetNonStopType());
		add(order.GetWaitTime()); add(order.GetTravelTime()); add(order.IsWaitTimetabled()); add(order.IsTravelTimetabled());
		add(order.GetRefitCargo()); add(order.GetDispatchScheduleIndex()); add(order.GetLeaveType());
		/* Preserve existing train hashes; only road direction constraints add a field. */
		if (order.GetRoadVehTravelDirection() != DiagDirection::Invalid) { add(0x524F4144); add(to_underlying(order.GetRoadVehTravelDirection())); }
		/* A full load of another cargo can delay this vehicle too. */
		for (CargoType cid : EnumRange(NUM_CARGO)) { add(to_underlying(order.GetCargoLoadType(cid))); add(to_underlying(order.GetCargoUnloadType(cid))); }
	}
	for (const DispatchSchedule &schedule : orders.GetScheduledDispatchScheduleSet()) {
		if (!spend(8 + schedule.GetScheduledDispatch().size())) return 0;
		add(schedule.GetScheduledDispatchStartTick().base()); add(schedule.GetScheduledDispatchDuration());
		add(schedule.GetScheduledDispatchLastDispatch()); add(schedule.GetScheduledDispatchDelay()); add(schedule.GetScheduledDispatchReuseSlots());
		for (const auto &slot : schedule.GetScheduledDispatch()) { add(slot.offset); add(slot.flags); }
	}
	add(cargo);
	return hash;
}

/** Future capacity is an upper bound. Real seats are assigned exclusively by Load/Reserve below.
 * Do not use captured occupancy to reject a group that can be split across coaches/gradual loading. */
struct JourneyCapacityUpperBound : PassengerJourneyCapacityProvider {
	uint64_t Revision() const override { return 0; }
	PassengerJourneyCapacity Estimate(const PassengerServiceGraph &graph, PassengerServiceGraph::VisitID boarding,
			PassengerServiceGraph::VisitID) const override
	{
		PassengerJourneyCapacity capacity;
		capacity.future_total = capacity.future_free = graph.GetTrips()[boarding.trip].capacity;
		capacity.certainty = PassengerCapacityCertainty::Estimated;
		return capacity;
	}
};

struct PassengerJourneyActiveContext::Snapshot {
	PassengerServiceGraph graph;
	JourneyCapacityUpperBound capacity;
	PassengerJourneyPlanner planner;
	std::map<OrderListID, uint64_t> signatures;
	std::map<StationID, std::optional<PassengerJourneyExecution>> waiting;
	std::map<StationID, std::optional<std::pair<PassengerJourneyExecution, bool>>> onboard;
	PassengerJourneyFallbackReason failure = PassengerJourneyFallbackReason::NoJourney;
	bool deferred = false, budget_limited = false;
	bool waiting_ignored_unknown = false, onboard_ignored_unknown = false;
	Snapshot(PassengerServiceGraph graph) : graph(std::move(graph)), planner(this->graph, &this->capacity, {2, 256 * 1024}) {}
};

PassengerJourneyActiveContext::PassengerJourneyActiveContext(const Vehicle *vehicle, StationID station, size_t prediction_cache_bytes) :
		capture(PassengerServiceCapture::MAXIMUM_BYTES, prediction_cache_bytes), vehicle(vehicle), station(station) { if (_settings_game.linkgraph.passenger_journey_periodic) ObservePassengerPeriodicVehicle(vehicle); }
PassengerJourneyActiveContext::~PassengerJourneyActiveContext() = default;

PassengerJourneyActiveContext::Snapshot *PassengerJourneyActiveContext::GetSnapshot(CargoType cargo, StationID destination, Ticks horizon)
{
	auto key = std::tuple(cargo, destination, horizon);
	auto found = this->snapshots.find(key);
	if (found != this->snapshots.end()) return found->second.get();
	auto &global = TickBudget();
	JourneyFairShare *ticket = nullptr;
	if (this->vehicle == nullptr) for (auto &share : global.fair_shares) if (share.station != StationID::Invalid() && share.station == this->station && !share.claimed) { ticket = &share; break; }
	const size_t reserved = UnclaimedShares(global) - (ticket != nullptr);
	const size_t graph_work = Unreserved(global.graph, reserved * FAIR_GRAPH_WORK), query_work = Unreserved(global.query, reserved * FAIR_QUERY_WORK);
	this->deferred = true;
	if (this->queries_left == 0) { this->failure = PassengerJourneyFallbackReason::QueryLimit; return nullptr; }
	if (this->work_left < 65536 || query_work < 65536) { this->failure = PassengerJourneyFallbackReason::QueryWork; return nullptr; }
	if (global.snapshots <= reserved || this->snapshots.size() >= 128) { this->failure = PassengerJourneyFallbackReason::SnapshotLimit; return nullptr; }
	if (this->work_left == 0 || graph_work == 0) { this->failure = PassengerJourneyFallbackReason::GraphWork; return nullptr; }
	if (this->memory_left == 0) { this->failure = PassengerJourneyFallbackReason::GraphMemory; return nullptr; }
	auto limits = ActivePassengerServiceGraphLimits();
	limits.horizon = horizon;
	limits.work = std::min({limits.work, this->work_left, graph_work});
	limits.memory_bytes = std::min(limits.memory_bytes, this->memory_left);
	const bool budget_limited = limits.work < ActivePassengerServiceGraphLimits().work || limits.memory_bytes < ActivePassengerServiceGraphLimits().memory_bytes;
	if (ticket != nullptr) { ticket->claimed = true; ++global.diagnostic.fair_share_claims; }
	this->deferred = false;
	--global.snapshots;
	PassengerServiceScope scope{this->station, destination};
	if (this->vehicle != nullptr && this->vehicle->orders != nullptr) scope.onboard_service = this->vehicle->orders->index;
	auto graph = PassengerServiceGraph::Build(cargo, _state_ticks, limits, scope, &this->capture, PassengerTimingPolicy::AssumeTimetable);
	this->work_left -= graph.GetWork(); global.graph -= graph.GetWork(); this->memory_left -= graph.GetMemoryBound();
	auto &diagnostic = global.diagnostic;
	++diagnostic.builds;
	diagnostic.static_cache_hits += graph.GetDiagnostic().static_cache_hit;
	diagnostic.graph_work += graph.GetWork();
	diagnostic.maximum_graph_work = std::max(diagnostic.maximum_graph_work, graph.GetWork());
	diagnostic.graph_memory_bound = std::max(diagnostic.graph_memory_bound, graph.GetMemoryBound());
	diagnostic.graph_allocation_bytes = std::max(diagnostic.graph_allocation_bytes, graph.GetDiagnostic().allocation_bytes);
	diagnostic.build_microseconds += graph.GetDiagnostic().build_microseconds;
	diagnostic.maximum_build_microseconds = std::max(diagnostic.maximum_build_microseconds, graph.GetDiagnostic().build_microseconds);
	const auto &capture = graph.GetDiagnostic();
	for (size_t i = 0; i < diagnostic.graph_phase_microseconds.size(); ++i) diagnostic.graph_phase_microseconds[i] += capture.phase_microseconds[i];
	diagnostic.capture_discovery_hits += capture.capture_discovery_hit; diagnostic.capture_service_hits += capture.capture_service_hits;
	diagnostic.capture_reused_work += capture.capture_reused_work;
	diagnostic.capture_allocation_bytes = std::max(diagnostic.capture_allocation_bytes, capture.capture_allocation_bytes);
	diagnostic.prediction_executions += capture.prediction_executions; diagnostic.prediction_cache_hits += capture.prediction_cache_hits;
	diagnostic.prediction_work += capture.prediction_work; diagnostic.prediction_executed_visits += capture.prediction_executed_visits;
	diagnostic.prediction_nanoseconds += capture.prediction_nanoseconds; diagnostic.prediction_cache_bytes = std::max(diagnostic.prediction_cache_bytes, capture.prediction_cache_bytes);
	diagnostic.timing_uncertainty += capture.timing_uncertainty;
	diagnostic.path_uncertainty += capture.path_uncertainty;
	++diagnostic.graph_failures[static_cast<size_t>(capture.first_failure.reason)];
	if (diagnostic.first_graph_failure.reason == PassengerGraphFailure::None) diagnostic.first_graph_failure = capture.first_failure;
	if (diagnostic.first_prediction_failure.reason == PassengerPredictionStop::None) diagnostic.first_prediction_failure = capture.first_prediction_failure;
	if (diagnostic.opaque_service == OrderListID::Invalid()) { diagnostic.opaque_service = capture.opaque_service; diagnostic.opaque_vehicle = capture.opaque_vehicle; }
	for (size_t i = 0; i < diagnostic.prediction_stops.size(); ++i) diagnostic.prediction_stops[i] += capture.prediction_stops[i];
	diagnostic.complete_graphs += graph.GetCoverage(this->station, destination) != PassengerServiceGraph::Coverage::Incomplete;
	diagnostic.admitted_visits += capture.admitted_visits;
	diagnostic.eligible_services += capture.eligible_services; diagnostic.selected_services += capture.selected_services; diagnostic.selected_trips += capture.selected_trips;
	diagnostic.largest_block_services = std::max(diagnostic.largest_block_services, capture.largest_block_services);
	diagnostic.maximum_unpredicted_ticks = std::max(diagnostic.maximum_unpredicted_ticks, capture.maximum_unpredicted_ticks);
	diagnostic.trains += capture.trains; diagnostic.cargo_capable += capture.cargo_capable; diagnostic.possible_refit += capture.possible_refit;
	diagnostic.predicted += capture.predicted; diagnostic.visits += capture.visits; diagnostic.excluded_cargo += capture.excluded_cargo;
	diagnostic.excluded_scope += capture.excluded_scope; diagnostic.inactive += capture.inactive; diagnostic.omitted_trains += capture.omitted_trains;
	diagnostic.truncated_predictions += capture.truncated_predictions; diagnostic.reused_graph_work += capture.reused_work;
	diagnostic.static_allocation_bytes = std::max(diagnostic.static_allocation_bytes, capture.static_allocation_bytes);
	auto snapshot = std::make_unique<Snapshot>(std::move(graph));
	snapshot->budget_limited = budget_limited;
	return this->snapshots.emplace(key, std::move(snapshot)).first->second.get();
}

bool PassengerJourneyActiveContext::AdmitQuery()
{
	constexpr size_t QUERY_WORK = 65536;
	auto &global = TickBudget();
	this->deferred = true;
	if (this->queries_left == 0) { this->failure = PassengerJourneyFallbackReason::QueryLimit; return false; }
	if (this->work_left < QUERY_WORK || Unreserved(global.query, UnclaimedShares(global) * FAIR_QUERY_WORK) < QUERY_WORK) { this->failure = PassengerJourneyFallbackReason::QueryWork; return false; }
	this->deferred = false;
	this->work_left -= QUERY_WORK; global.query -= QUERY_WORK; --this->queries_left;
	++global.diagnostic.queries; global.diagnostic.query_work += QUERY_WORK;
	return true;
}

static PassengerJourneyLimits ActiveJourneyLimits()
{
	PassengerJourneyLimits limits;
	limits.candidates = 1;
	limits.work = 65536;
	limits.labels = 8192;
	limits.memory_bytes = 8 * 1024 * 1024;
	return limits;
}

static PassengerJourneyEvaluation ActiveJourneyEvaluation()
{
	PassengerJourneyEvaluation evaluation;
	evaluation.minimum_transfer = evaluation.transfer_penalty = _settings_game.linkgraph.passenger_transfer_penalty;
	return evaluation;
}

std::optional<PassengerJourneyExecution> PassengerJourneyActiveContext::Execute(Snapshot &stored, CargoType cargo, const PassengerJourneyLeg &leg)
{
	auto *snapshot = &stored;
	if (!Station::IsValidID(leg.boarding_station) || !Station::IsValidID(leg.alighting_station)) return {};
	const auto &trip = snapshot->graph.GetTrips()[leg.boarding.trip];
	const OrderList *orders = OrderList::GetIfValid(trip.service);
	if (orders == nullptr) return {};
	auto [signature, inserted] = snapshot->signatures.try_emplace(trip.service, 0);
	if (inserted) {
		auto &global = TickBudget();
		const size_t allowance = std::min(this->work_left, Unreserved(global.query, UnclaimedShares(global) * FAIR_QUERY_WORK));
		size_t remaining = allowance;
		signature->second = PassengerJourneyServiceSignature(*orders, cargo, &remaining);
		this->work_left -= allowance - remaining; global.query -= allowance - remaining;
		global.diagnostic.query_work += allowance - remaining;
	}
	if (signature->second == 0) { this->failure = PassengerJourneyFallbackReason::QueryWork; this->deferred = true; return {}; }
	return PassengerJourneyExecution{leg.vehicle, trip.service, leg.boarding_station, leg.alighting_station,
			trip.prediction.visits[leg.boarding.visit].order, trip.prediction.visits[leg.alighting.visit].order,
			leg.departure, leg.arrival, signature->second};
}

bool PassengerJourneyActiveContext::PlanWaiting(CargoPacket &packet, CargoType cargo, StationID legacy_hop)
{
	PassengerJourneyWorkTimer timer;
	this->deferred = false;
	if (!IsPassengerJourneyRoutingEnabled(cargo) || !Station::IsValidID(packet.GetPassengerDestination()) ||
			packet.GetPassengerDestination() == this->station || !packet.CanRetryPassengerJourney(this->station, _state_ticks)) return false;
	if (_settings_game.linkgraph.passenger_journey_periodic) {
		StateTicks ready = packet.GetPassengerArrivingService() == OrderListID::Invalid() ? _state_ticks : std::max(_state_ticks, packet.GetPassengerInterchangeReady());
		if (this->vehicle != nullptr) ready = std::max(ready, _state_ticks + StateTicksDelta{ActiveJourneyEvaluation().minimum_transfer});
		const OrderList *arriving = OrderList::GetIfValid(packet.GetPassengerArrivingService());
		OrderListID excluded = arriving != nullptr && arriving->GetCargoDistRouteGeneration() == packet.GetPassengerArrivingIncarnation() ? arriving->index : OrderListID::Invalid();
		auto key = std::tuple(cargo, packet.GetPassengerDestination(), ready, excluded);
		auto apply = [&](const PassengerJourneyExecution &ride) {
			packet.SetPassengerJourney(ride, legacy_hop);
			if (IsEstimatedPassengerRide(ride.kind)) {
				++TickBudget().diagnostic.schedule_packets; TickBudget().diagnostic.schedule_passengers += packet.Count();
				TickBudget().diagnostic.schedule_deferred_packets += ride.timed_deferred;
			} else { ++TickBudget().diagnostic.journeys; TickBudget().diagnostic.passengers += packet.Count(); }
		};
		auto cached = this->periodic_waiting.find(key);
		if (cached != this->periodic_waiting.end()) {
			++TickBudget().diagnostic.query_cache_hits;
			if (!cached->second.ride || cached->second.deferred) {
				if (cached->second.deferred) { CountJourneyDeferral(PassengerJourneyFallbackReason::QueryWork); packet.DeferPassengerJourney(this->station, _state_ticks); }
				return false;
			}
			apply(*cached->second.ride);
			return true;
		}
		if (this->periodic_waiting.size() >= 128) return false;
		if (!this->AdmitQuery()) { CountJourneyDeferral(this->failure); packet.DeferPassengerJourney(this->station, _state_ticks); return false; }
		auto limits = ActiveJourneyLimits(); limits.candidates = 8;
		auto &db = GetPassengerServiceDatabase();
		auto plan = db.Plan(cargo, this->station, packet.GetPassengerDestination(), ready, limits, ActiveJourneyEvaluation(), excluded);
		if (plan.deferred) ++TickBudget().diagnostic.timed_budget_deferred;
		/* Timed winners always take priority. The two searches share the admitted
		 * work envelope; cache hits replay the same logical work as cold peers. */
		if (!plan.ride) {
			auto estimated_limits = limits; estimated_limits.work -= std::min(limits.work, plan.work);
			auto estimated = db.PlanEstimated(cargo, this->station, packet.GetPassengerDestination(), ready, estimated_limits, ActiveJourneyEvaluation(), excluded);
			if (estimated.ride) {
				estimated.ride->planned_at = _state_ticks; estimated.ride->timed_deferred = plan.deferred;
				plan.ride = estimated.ride; plan.deferred = false;
			} else plan.deferred |= estimated.deferred;
		} else plan.deferred = false;
		this->periodic_waiting.emplace(key, PeriodicWaiting{plan.ride, plan.deferred});
		if (!plan.ride || plan.deferred) {
			if (plan.deferred) { CountJourneyDeferral(PassengerJourneyFallbackReason::QueryWork); packet.DeferPassengerJourney(this->station, _state_ticks); }
			else CountJourneyFallback(GetPassengerServiceDatabase().IsBuilding() ? PassengerJourneyFallbackReason::PredictionIncomplete : PassengerJourneyFallbackReason::NoJourney);
			return false;
		}
		apply(*plan.ride);
		return true;
	}
	Snapshot *snapshot = nullptr;
	auto reject = [&](PassengerJourneyFallbackReason reason) {
		if (snapshot != nullptr) { snapshot->failure = reason; snapshot->deferred = this->deferred; }
		if (reason == PassengerJourneyFallbackReason::GraphWork || reason == PassengerJourneyFallbackReason::GraphMemory ||
				reason == PassengerJourneyFallbackReason::SnapshotLimit || reason == PassengerJourneyFallbackReason::QueryWork ||
				reason == PassengerJourneyFallbackReason::QueryLimit || reason == PassengerJourneyFallbackReason::QueryMemory) ++TickBudget().diagnostic.timed_budget_deferred;
		if (this->deferred) { CountJourneyDeferral(reason); packet.DeferPassengerJourney(this->station, _state_ticks); }
		else {
			CountJourneyFallback(reason);
			/* A completed retry may establish a real route failure. Do not let the
			 * previous budget-wait marker turn that into another immediate retry. */
			if (packet.IsPassengerJourneyDeferred()) packet.FallbackPassengerJourney(this->station, _state_ticks);
		}
		return false;
	};
	/* A suffix starting at the exclusive horizon cannot beat a certified journey
	 * whose nonnegative generalized cost is strictly below that horizon. Otherwise
	 * extend, paying ordinary event/tick allowances for every build and search. */
	for (Ticks horizon = 4096;; horizon = std::min<Ticks>(65536, horizon * 4)) {
		snapshot = this->GetSnapshot(cargo, packet.GetPassengerDestination(), horizon);
		if (snapshot == nullptr) return reject(this->failure);
		if (snapshot->graph.GetQueryStatus(this->station, packet.GetPassengerDestination()) >= PassengerPredictionStatus::LimitReached) {
			this->deferred = snapshot->budget_limited && snapshot->graph.GetQueryStatus(this->station, packet.GetPassengerDestination()) >= PassengerPredictionStatus::LimitReached;
			return reject(snapshot->graph.GetStatus() == PassengerPredictionStatus::BudgetExhausted ? PassengerJourneyFallbackReason::GraphWork :
					snapshot->graph.GetStatus() == PassengerPredictionStatus::LimitReached ? PassengerJourneyFallbackReason::GraphMemory : PassengerJourneyFallbackReason::PredictionIncomplete);
		}
		if (snapshot->waiting.size() >= 128 && !snapshot->waiting.contains(packet.GetPassengerDestination())) return false;
		auto [cached, inserted] = snapshot->waiting.try_emplace(packet.GetPassengerDestination());
		if (!inserted) {
			++TickBudget().diagnostic.query_cache_hits;
			if (!cached->second) {
				if (snapshot->failure == PassengerJourneyFallbackReason::PredictionHorizon && horizon < 65536) continue;
				this->deferred = snapshot->deferred; return reject(snapshot->failure);
			}
			packet.SetPassengerJourney(*cached->second, legacy_hop);
			TickBudget().diagnostic.timed_routes_ignoring_unknown += snapshot->waiting_ignored_unknown;
			++TickBudget().diagnostic.journeys; TickBudget().diagnostic.passengers += packet.Count();
			return true;
		}
		/* Reserve a fixed query allowance. Cache reuse is a performance optimization only. */
		auto limits = ActiveJourneyLimits();
		if (!this->AdmitQuery()) return reject(this->failure);
		const auto query_start = std::chrono::steady_clock::now();
		auto result = snapshot->planner.Plan(cargo, this->station, packet.GetPassengerDestination(), _state_ticks, limits, ActiveJourneyEvaluation(), 1);
		CountJourneySearch(result, snapshot->planner, query_start, this->station, packet.GetPassengerDestination());
		if (result.stop_reason == PassengerJourneyStopReason::PredictionHorizon ||
				(result.GetOutcome() == PassengerJourneyOutcome::TimedRouteFound && result.journeys.front().generalized_time.base() >= horizon)) {
			snapshot->failure = PassengerJourneyFallbackReason::PredictionHorizon;
			if (horizon < 65536) continue;
			return reject(PassengerJourneyFallbackReason::PredictionHorizon);
		}
		if (result.GetOutcome() != PassengerJourneyOutcome::TimedRouteFound) return reject(JourneySearchFailure(result));
		if (!result.HasJourney() || result.journeys.front().legs.empty()) return reject(PassengerJourneyFallbackReason::NoJourney);
		auto execution = this->Execute(*snapshot, cargo, result.journeys.front().legs.front());
		if (!execution) return reject(this->deferred ? this->failure : PassengerJourneyFallbackReason::InvalidLeg);
		cached->second = *execution;
		snapshot->waiting_ignored_unknown = result.diagnostic.unknown_alternatives_ignored;
		packet.SetPassengerJourney(*execution, legacy_hop);
		TickBudget().diagnostic.timed_routes_ignoring_unknown += snapshot->waiting_ignored_unknown;
		++TickBudget().diagnostic.journeys; TickBudget().diagnostic.passengers += packet.Count();
		return true;
	}
}

bool PassengerOdFallbackContext::Spend(size_t amount)
{
	auto &global = TickBudget();
	if (amount > this->work_left || amount > global.od_fallback) { this->resource_limited = true; return false; }
	this->work_left -= amount; global.od_fallback -= amount;
	global.diagnostic.od_fallback_work += amount;
	return true;
}

/** Certify only the executed order prefix, stopping before a return to this station,
 * an unresolved branch/refit/detour or a forced interchange. Timings may be absent. */
bool PassengerOdFallbackContext::CanReach(const Vehicle *vehicle, CargoType cargo, StationID target)
{
	if (vehicle == nullptr || !Station::IsValidID(target)) return false;
	vehicle = vehicle->First();
	const auto key = std::pair(vehicle->index, cargo);
	if (!this->stops.contains(key) && this->stops.size() >= 128) { this->resource_limited = true; return false; }
	auto [cached, inserted] = this->stops.try_emplace(key);
	if (inserted) {
		auto &global = TickBudget();
		const size_t allowance = std::min(this->work_left, global.od_fallback);
		size_t remaining = allowance;
		auto state = GetPassengerRouteState(vehicle, &remaining);
		this->Spend(allowance - remaining);
		if (!state && remaining == 0) this->resource_limited = true;
		if (!state || !state->loading) return false;
		if (state->orders.size() > 4096) { this->resource_limited = true; return false; }
		std::array<bool, 4096> seen{};
		VehicleOrderID index = (state->current_order + 1) % state->orders.size();
		seen[state->current_order] = true;
		while (!seen[index]) {
			/* Includes traversal, station validation and sorting the retained prefix. */
			if (!this->Spend(16)) break;
			seen[index] = true;
			const Order &order = state->orders[index];
			if (order.IsType(OT_CONDITIONAL)) {
				if (order.GetConditionVariable() != OrderConditionVariable::Unconditionally ||
						order.GetConditionSkipToOrder() >= state->orders.size()) break;
				index = order.GetConditionSkipToOrder();
				continue;
			}
			if (order.IsType(OT_IMPLICIT)) { index = (index + 1) % state->orders.size(); continue; }
			if (order.IsType(OT_GOTO_DEPOT)) {
				if (ClassifyPassengerDepot(order) == PassengerDepotTraversal::Blocked) break;
				index = (index + 1) % state->orders.size();
				continue;
			}
			if (order.IsRefit() || order.IsSlotCounterOrder() || order.IsType(OT_DUMMY)) break;
			if (order.IsType(OT_GOTO_STATION) && !(order.GetNonStopType() & ONSF_NO_STOP_AT_DESTINATION_STATION)) {
				StationID station = order.GetDestination().ToStationID();
				if (station == vehicle->last_station_visited && order.GetCargoUnloadType(cargo) != OrderUnloadType::NoUnload) break;
				if (!Station::IsValidID(station) || !state->usable_stops[index]) break;
				if (order.GetCargoUnloadType(cargo) != OrderUnloadType::NoUnload) cached->second.push_back(station);
				if (order.GetCargoUnloadType(cargo) == OrderUnloadType::Unload || order.GetCargoUnloadType(cargo) == OrderUnloadType::Transfer) break;
			} else if (!order.IsType(OT_GOTO_STATION) && !order.IsType(OT_GOTO_WAYPOINT) && !order.IsType(OT_LABEL)) break;
			index = (index + 1) % state->orders.size();
		}
		std::sort(cached->second.begin(), cached->second.end());
	}
	return std::binary_search(cached->second.begin(), cached->second.end(), target);
}

/** Reverse hop distances are hints for progress, never proof of a live service.
 * Share one bounded result per cargo/destination across all packets and vehicles
 * in this event. A completed interchange must strictly reduce this distance. */
uint32_t PassengerOdFallbackContext::Distance(CargoType cargo, StationID origin, StationID destination)
{
	CounterTimer timer{TickBudget().diagnostic.fallback_distance_microseconds};
	constexpr uint32_t UNREACHABLE = UINT32_MAX;
	const auto key = std::pair(cargo, destination);
	const Station *source = Station::GetIfValid(origin), *target = Station::GetIfValid(destination);
	if (source == nullptr || target == nullptr) return UNREACHABLE;
	const GoodsEntry &from = source->goods[cargo], &to = target->goods[cargo];
	const LinkGraph *graph = LinkGraph::GetIfValid(to.link_graph);
	if (graph == nullptr || graph->Cargo() != cargo || from.link_graph != graph->index) return UNREACHABLE;
	auto valid_node = [&](NodeID node) {
		if (node >= graph->Size()) return false;
		const Station *station = Station::GetIfValid((*graph)[node].Station());
		return station != nullptr && station->goods[cargo].link_graph == graph->index && station->goods[cargo].node == node;
	};
	if (!valid_node(from.node) || !valid_node(to.node) || (*graph)[from.node].Station() != origin ||
			(*graph)[to.node].Station() != destination) return UNREACHABLE;
	/* A stale interchange's node must not cache failure for other valid origins
	 * sharing this destination in the same event. */
	if (!this->distances.contains(key) && this->distances.size() >= 128) { this->resource_limited = true; return UNREACHABLE; }
	auto [cached, inserted] = this->distances.try_emplace(key);
	if (inserted) {
		const size_t bytes = graph->Size() * sizeof(uint32_t);
		/* Bound both retained destination arrays and transient reverse adjacency. */
		if (bytes > 1024 * 1024 - this->distance_bytes ||
				graph->Size() * (sizeof(std::vector<NodeID>) + sizeof(NodeID) + sizeof(uint32_t)) +
				2 * graph->GetEdges().size() * sizeof(NodeID) > 2 * 1024 * 1024 || !this->Spend(graph->Size())) {
			this->resource_limited = true;
			return UNREACHABLE;
		}
		auto spend_prefix = [&](size_t amount) {
			size_t available = std::min(this->work_left, TickBudget().od_fallback);
			if (amount > available) { this->Spend(available); this->Spend(); return false; }
			return this->Spend(amount);
		};
		const auto binding_key = std::pair(graph->index, graph->PassengerOdRevision());
		auto event_binding = this->bindings.find(binding_key);
		if (event_binding == this->bindings.end()) {
			auto resident_binding = resident_bindings.find(binding_key);
			bool matches = resident_binding != resident_bindings.end() && resident_binding->second.nodes->size() == graph->Size();
			for (NodeID node = 0; matches && node < graph->Size(); ++node) matches = (*resident_binding->second.nodes)[node] == std::pair((*graph)[node].Station(), valid_node(node));
			if (!matches) {
				if (resident_binding != resident_bindings.end()) { resident_binding_bytes -= resident_binding->second.bytes; resident_bindings.erase(resident_binding); }
				auto nodes = std::make_shared<BindingNodes>(); nodes->reserve(graph->Size());
				for (NodeID node = 0; node < graph->Size(); ++node) nodes->emplace_back((*graph)[node].Station(), valid_node(node));
				size_t binding_bytes = 128 + nodes->size() * sizeof(BindingNodes::value_type) * 2;
				while (!resident_bindings.empty() && (resident_bindings.size() >= 32 || resident_binding_bytes + binding_bytes > 8 * 1024 * 1024)) {
					resident_binding_bytes -= resident_bindings.begin()->second.bytes; resident_bindings.erase(resident_bindings.begin());
				}
				assert(binding_token != UINT64_MAX);
				resident_binding = resident_bindings.emplace(binding_key, ResidentBinding{nodes, ++binding_token, binding_bytes}).first; resident_binding_bytes += binding_bytes;
			}
			event_binding = this->bindings.emplace(binding_key, Binding{resident_binding->second.nodes, resident_binding->second.token}).first;
		}
		const auto &node_bindings = *event_binding->second.nodes;
		DistanceKey resident_key{cargo, destination, graph->index, graph->PassengerOdRevision(), graph->PassengerRoutingRevision(), event_binding->second.token, to.node};
		auto resident = resident_distances.find(resident_key);
		if (resident != resident_distances.end()) {
			/* A cache hit replays the complete cold debit, including a failed
			 * prefix when this event has less remaining work. */
			if (!spend_prefix(resident->second.work)) return UNREACHABLE;
			resident->second.used = ++distance_use;
			cached->second.graph = graph->index; cached->second.hops = resident->second.hops; this->distance_bytes += bytes;
			++TickBudget().diagnostic.od_fallback_cache_hits;
			TickBudget().diagnostic.od_fallback_cache_bytes = resident_distance_bytes + resident_binding_bytes + reverse_bytes;
			return (*cached->second.hops)[from.node];
		}
		const size_t original_work = this->work_left;
		++TickBudget().diagnostic.od_fallback_searches;
		if (!spend_prefix(graph->GetEdges().size())) return UNREACHABLE;
		ReverseKey reverse_key{graph->index, graph->PassengerOdRevision(), graph->PassengerRoutingRevision(), event_binding->second.token};
		auto reverse = reverse_indices.find(reverse_key);
		std::shared_ptr<const ReverseIndex> index;
		if (reverse != reverse_indices.end()) index = reverse->second;
		else {
			auto built = std::make_shared<ReverseIndex>(); built->offsets.resize(graph->Size() + 1);
			auto eligible = [&](const auto &edge, const auto &info) {
				return edge.first < node_bindings.size() && edge.second < node_bindings.size() && node_bindings[edge.first].second && node_bindings[edge.second].second &&
						info.capacity != 0 && info.last_unrestricted_update != EconTime::INVALID_DATE;
			};
			for (const auto &[edge, info] : graph->GetEdges()) if (eligible(edge, info)) ++built->offsets[edge.second + 1];
			for (size_t node = 1; node < built->offsets.size(); ++node) built->offsets[node] += built->offsets[node - 1];
			built->incoming.resize(built->offsets.back()); auto cursor = built->offsets;
			for (const auto &[edge, info] : graph->GetEdges()) if (eligible(edge, info)) built->incoming[cursor[edge.second]++] = edge.first;
			built->bytes = 256 + built->offsets.size() * sizeof(uint32_t) * 2 + built->incoming.size() * sizeof(NodeID) * 2;
			while (!reverse_indices.empty() && (reverse_indices.size() >= 32 || reverse_bytes + built->bytes > 8 * 1024 * 1024)) {
				reverse_bytes -= reverse_indices.begin()->second->bytes; reverse_indices.erase(reverse_indices.begin());
			}
			index = built; reverse_indices.emplace(reverse_key, built); reverse_bytes += built->bytes;
		}
		std::vector<uint32_t> hops(graph->Size(), UNREACHABLE);
		std::vector<NodeID> queue; queue.reserve(graph->Size());
		hops[to.node] = 0; queue.push_back(to.node);
		for (size_t head = 0; head < queue.size(); ++head) {
			if (!this->Spend()) return UNREACHABLE;
			for (uint32_t offset = index->offsets[queue[head]]; offset < index->offsets[queue[head] + 1]; ++offset) {
				if (!this->Spend()) return UNREACHABLE;
				NodeID previous = index->incoming[offset];
				if (hops[previous] != UNREACHABLE) continue;
				hops[previous] = hops[queue[head]] + 1; queue.push_back(previous);
			}
		}
		cached->second.graph = graph->index;
		cached->second.hops = std::make_shared<const std::vector<uint32_t>>(std::move(hops)); this->distance_bytes += bytes;
		const size_t resident_bytes = bytes * 2 + 256;
		constexpr size_t MAX_RESIDENT_BYTES = 16 * 1024 * 1024;
		if (resident_bytes <= MAX_RESIDENT_BYTES) {
			while (!resident_distances.empty() && (resident_distances.size() >= 512 || resident_distance_bytes + resident_bytes > MAX_RESIDENT_BYTES)) {
				auto oldest = std::min_element(resident_distances.begin(), resident_distances.end(), [](const auto &a, const auto &b) { return a.second.used < b.second.used; });
				resident_distance_bytes -= oldest->second.bytes; resident_distances.erase(oldest);
			}
			resident_distances.emplace(resident_key, ResidentDistances{cached->second.hops, original_work - this->work_left, resident_bytes, ++distance_use}); resident_distance_bytes += resident_bytes;
		}
	}
	return cached->second.graph == graph->index && cached->second.hops != nullptr && cached->second.hops->size() == graph->Size() ? (*cached->second.hops)[from.node] : UNREACHABLE;
}

StationID PassengerOdFallbackContext::ProgressingVia(const Vehicle *vehicle, CargoType cargo, StationID destination)
{
	if (vehicle == nullptr) return StationID::Invalid();
	if (this->CanReach(vehicle, cargo, destination)) return destination;
	uint32_t remaining = this->Distance(cargo, vehicle->last_station_visited, destination);
	if (remaining == UINT32_MAX) return StationID::Invalid();
	StationID best = StationID::Invalid();
	/* CanReach populated only the live, usable prefix. Equal-progress stops tie by
	 * StationID; a different first hop or an express skipping it can still qualify. */
	auto stops = this->stops.find(std::pair(vehicle->index, cargo));
	if (stops == this->stops.end()) return StationID::Invalid();
	for (StationID stop : stops->second) {
		if (!this->Spend()) break; // Any retained candidate still has a verified live prefix.
		uint32_t distance = this->Distance(cargo, stop, destination);
		if (distance < remaining) { remaining = distance; best = stop; }
	}
	return best;
}

StationID PassengerOdFallbackContext::BoardingVia(const Vehicle *vehicle, CargoType cargo, StationID destination)
{
	if (vehicle == nullptr) return StationID::Invalid();
	vehicle = vehicle->First();
	if (destination == vehicle->last_station_visited || !vehicle->current_order.IsAnyLoadingType() ||
			vehicle->vehstatus.Any({VehState::Stopped, VehState::Crashed}) ||
			vehicle->current_order.GetCargoLoadType(cargo) == OrderLoadType::NoLoad ||
			vehicle->current_order.GetCargoUnloadType(cargo) == OrderUnloadType::Unload ||
			vehicle->current_order.GetCargoUnloadType(cargo) == OrderUnloadType::Transfer) return StationID::Invalid();
	const auto key = std::tuple(vehicle->index, cargo, destination);
	if (!this->routes.contains(key) && this->routes.size() >= 128) {
		++TickBudget().diagnostic.untimed_budget_deferred;
		return StationID::Invalid();
	}
	auto [route, inserted] = this->routes.try_emplace(key, StationID::Invalid());
	if (inserted) {
		route->second = this->ProgressingVia(vehicle, cargo, destination);
		if (route->second != StationID::Invalid()) ++TickBudget().diagnostic.untimed_routes;
		else if (this->resource_limited) ++TickBudget().diagnostic.untimed_budget_deferred;
		else ++TickBudget().diagnostic.untimed_no_route;
	}
	return route->second;
}

bool PassengerOdFallbackContext::CanBoard(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo)
{
	return this->BoardingVia(vehicle, cargo, packet.GetPassengerDestination()) != StationID::Invalid();
}

void PassengerOdFallbackContext::PrepareBoard(CargoPacket &packet, const Vehicle *vehicle, CargoType cargo)
{
	packet.SetPassengerVia(this->BoardingVia(vehicle, cargo, packet.GetPassengerDestination()));
}

void PassengerOdFallbackContext::Invalidate(const Vehicle *vehicle)
{
	VehicleID id = vehicle->First()->index;
	for (auto it = this->stops.lower_bound({id, CargoType{0}}); it != this->stops.end() && it->first.first == id;) it = this->stops.erase(it);
	this->routes.clear();
	this->distances.clear(); this->distance_bytes = 0; this->bindings.clear();
}

std::optional<VehicleCargoList::MoveToAction> PassengerOdFallbackContext::Decide(CargoPacket &packet, const Vehicle *vehicle,
		CargoType cargo, StationID station, bool accepted, OrderUnloadType unload)
{
	using Action = VehicleCargoList::MoveToAction;
	if (packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey || !IsPassengerJourneyRoutingEnabled(cargo) ||
			!Station::IsValidID(packet.GetPassengerDestination())) return {};
	if (unload == OrderUnloadType::NoUnload) return Action::Keep;
	if (packet.GetPassengerDestination() == station) return accepted ? Action::Deliver : Action::Keep;
	if (unload != OrderUnloadType::Unload && unload != OrderUnloadType::Transfer) {
		StationID via = packet.GetPassengerVia();
		/* Keep the validated ride to its interchange; intermediate CargoDist flows
		 * cannot redraw it. Validate the live prefix and onward OD on every arrival. */
		if (via != station && this->CanReach(vehicle, cargo, via) &&
				(via == packet.GetPassengerDestination() || this->Distance(cargo, via, packet.GetPassengerDestination()) <
					this->Distance(cargo, station, packet.GetPassengerDestination()))) return Action::Keep;
		via = this->ProgressingVia(vehicle, cargo, packet.GetPassengerDestination());
		if (this->CanReach(vehicle, cargo, via)) { packet.SetPassengerVia(via); return Action::Keep; }
	}
	packet.SetPassengerVia(StationID::Invalid());
	packet.SetNextHop(StationID::Invalid()); // OD queue: boarding requires an independently verified progressing ride.
	return Action::Transfer;
}

PassengerJourneyBoardingContext::PassengerJourneyBoardingContext(const Vehicle *vehicle, CargoType cargo) : vehicle(vehicle),
		cargo(cargo != INVALID_CARGO ? cargo : vehicle == nullptr ? INVALID_CARGO : vehicle->cargo_type)
{
	if (vehicle != nullptr) this->vehicle = vehicle->First();
}

void PassengerJourneyBoardingContext::Initialize() const
{
	if (this->initialized) return;
	this->initialized = true;
	if (this->vehicle == nullptr) return;
	const auto limits = ActivePassengerServiceGraphLimits();
	/* Use planning's traversal and work ceilings, including live facility capture.
	 * A separately truncated boarding prediction must not reject a planned leg. */
	size_t budget = limits.work;
	if (!IsPassengerServiceVehicle(this->vehicle) || !IsPassengerJourneyRoutingEnabled(this->cargo) ||
			!HasPassengerVehicleCapacity(this->vehicle, this->cargo, budget)) return;
	auto state = GetPassengerRouteState(this->vehicle, &budget);
	if (!state || !state->loading) return;
	this->signature = PassengerJourneyServiceSignature(*this->vehicle->orders, this->cargo, &budget);
	++this->predictions;
	this->prediction = PredictPassengerVisits(*state, this->cargo, _state_ticks, _state_ticks + 65536,
			limits.visits_per_trip, limits.steps_per_trip, budget, {}, true, PassengerTimingPolicy::AssumeTimetable);
}

bool PassengerJourneyBoardingContext::ValidateServiceRide(const PassengerJourneyExecution &ride, bool boarding) const
{
	if (this->vehicle == nullptr || (IsBoundPassengerRide(ride.kind) && ride.vehicle != this->vehicle->index)) return false;
	if (boarding && _state_ticks < ride.not_before) return false;
	if (boarding && !GetPassengerServiceDatabase().IsCurrent(ride)) return false;
	auto key = std::tuple(ride.service, ride.signature, ride.boarding_order, ride.alighting_order, ride.boarding, ride.alighting, boarding);
	auto found = this->service_rides.find(key);
	if (found != this->service_rides.end()) return found->second;
	if (this->service_rides.size() >= 128) return false;
	bool valid = ValidatePassengerServiceRide(ride, this->vehicle, this->cargo, boarding);
	if (boarding && !valid && this->vehicle->orders != nullptr && this->vehicle->orders->index == ride.service) GetPassengerServiceDatabase().RecordBoardingFailure();
	return this->service_rides.emplace(key, valid).first->second;
}

bool PassengerJourneyBoardingContext::CanBoard(const CargoPacket &packet) const
{
	if (packet.GetPassengerRoutingMode() != PassengerRoutingMode::Journey) return true;
	const auto &leg = packet.GetPassengerJourney();
	if (leg.kind != PassengerRideKind::VehicleSnapshot) return this->ValidateServiceRide(leg, true);
	/* Reject other physical services and unsupported loading without capturing anything. */
	if (!IsPassengerServiceVehicle(this->vehicle) || this->vehicle->orders == nullptr ||
			!IsPassengerJourneyRoutingEnabled(this->cargo) || this->vehicle->vehstatus.Any({VehState::Stopped, VehState::Crashed}) ||
			this->vehicle->current_order.GetCargoLoadType(this->cargo) == OrderLoadType::NoLoad ||
			leg.vehicle != this->vehicle->index || leg.service != this->vehicle->orders->index ||
			leg.boarding != this->vehicle->last_station_visited || leg.boarding_order != this->vehicle->cur_real_order_index) return false;
	this->Initialize();
	if (this->signature == 0 || this->prediction.visits.empty()) return false;
	const auto &first = this->prediction.visits.front();
	if (leg.vehicle != this->vehicle->index || leg.service != this->vehicle->orders->index || leg.signature != this->signature ||
			leg.boarding != this->vehicle->last_station_visited || leg.boarding_order != this->vehicle->cur_real_order_index ||
			!first.loading_now || !first.departure || *first.departure != leg.departure) return false;
	for (const auto &visit : this->prediction.visits) {
		const Order *order = this->vehicle->GetOrder(visit.order);
		if (visit.order == leg.alighting_order && visit.arrival == leg.arrival && order->GetDestination() == leg.alighting &&
				order->GetCargoUnloadType(this->cargo) != OrderUnloadType::NoUnload) return true;
		if (!visit.loading_now && (order->GetCargoUnloadType(this->cargo) == OrderUnloadType::Unload ||
				order->GetCargoUnloadType(this->cargo) == OrderUnloadType::Transfer)) break;
	}
	return false;
}

/** Validate the previously admitted ride, without admitting another network search.
 * Matching an absolute arrival certifies the same occurrence at an intermediate stop. */
bool PassengerJourneyBoardingContext::CanContinue(const CargoPacket &packet) const
{
	if (packet.GetPassengerRoutingMode() != PassengerRoutingMode::Journey) return false;
	const auto &leg = packet.GetPassengerJourney();
	if (leg.kind != PassengerRideKind::VehicleSnapshot) return this->ValidateServiceRide(leg, false);
	if (!IsPassengerServiceVehicle(this->vehicle) || this->vehicle->orders == nullptr ||
			!IsPassengerJourneyRoutingEnabled(this->cargo) || this->vehicle->vehstatus.Any({VehState::Stopped, VehState::Crashed}) ||
			!this->vehicle->current_order.IsAnyLoadingType() || leg.vehicle != this->vehicle->index || leg.service != this->vehicle->orders->index ||
			!Station::IsValidID(leg.alighting) || leg.alighting == this->vehicle->last_station_visited ||
			leg.departure > _state_ticks || leg.arrival <= _state_ticks || leg.alighting_order >= this->vehicle->GetNumOrders()) return false;
	const Order *alight = this->vehicle->GetOrder(leg.alighting_order);
	if (!alight->IsType(OT_GOTO_STATION) || alight->GetDestination() != leg.alighting ||
			(alight->GetNonStopType() & ONSF_NO_STOP_AT_DESTINATION_STATION) || alight->GetCargoUnloadType(this->cargo) == OrderUnloadType::NoUnload) return false;
	this->Initialize();
	if (this->signature == 0 || this->signature != leg.signature || this->prediction.visits.empty() || !this->prediction.visits.front().loading_now) return false;
	for (const auto &visit : this->prediction.visits) {
		if (visit.order == leg.alighting_order && visit.arrival == leg.arrival) return true;
	}
	return false;
}

bool PassengerJourneyLoadingContext::CanBoard(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo)
{
	PassengerJourneyWorkTimer timer;
	CounterTimer category{TickBudget().diagnostic.boarding_microseconds};
	if (vehicle == nullptr) return packet.GetPassengerRoutingMode() != PassengerRoutingMode::Journey;
	if (cargo == INVALID_CARGO) cargo = vehicle->cargo_type;
	if (IsPassengerJourneyRoutingEnabled(cargo) && Station::IsValidID(packet.GetPassengerDestination()) &&
			packet.GetPassengerArrivingService() != OrderListID::Invalid() && (_state_ticks < packet.GetPassengerInterchangeReady() ||
			(vehicle->First()->orders != nullptr && vehicle->First()->orders->index == packet.GetPassengerArrivingService() && vehicle->First()->orders->GetCargoDistRouteGeneration() == packet.GetPassengerArrivingIncarnation()))) return false;
	if (packet.GetPassengerRoutingMode() != PassengerRoutingMode::Journey) {
		return !IsPassengerJourneyRoutingEnabled(cargo) || !Station::IsValidID(packet.GetPassengerDestination()) || this->fallback.CanBoard(packet, vehicle, cargo);
	}
	if (packet.GetPassengerJourney().kind != PassengerRideKind::ServiceIntent && packet.GetPassengerJourney().kind != PassengerRideKind::EstimatedServiceIntent && packet.GetPassengerJourney().vehicle != vehicle->First()->index) return false;
	auto key = std::pair(vehicle->First()->index, cargo);
	if (!this->boarding.contains(key) && this->boarding.size() >= 128) return false;
	return this->boarding.try_emplace(key, vehicle, cargo).first->second.CanBoard(packet);
}

void PassengerJourneyLoadingContext::PrepareBoard(CargoPacket &packet, const Vehicle *vehicle)
{
	if (vehicle != nullptr) {
		packet.SetPassengerTransport(packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey ?
				(IsEstimatedPassengerRide(packet.GetPassengerJourney().kind) ? PassengerTransportKind::Schedule : PassengerTransportKind::Timed) :
				!IsPassengerJourneyRoutingEnabled(vehicle->cargo_type) || !Station::IsValidID(packet.GetPassengerDestination()) ? PassengerTransportKind::None : PassengerTransportKind::Untimed);
	}
	if (vehicle != nullptr && packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey &&
			(packet.GetPassengerJourney().kind == PassengerRideKind::ServiceIntent || packet.GetPassengerJourney().kind == PassengerRideKind::EstimatedServiceIntent)) {
		auto ride = packet.GetPassengerJourney();
		ride.vehicle = vehicle->First()->index; ride.kind = IsEstimatedPassengerRide(ride.kind) ? PassengerRideKind::EstimatedBoundService : PassengerRideKind::BoundService;
		packet.SetPassengerJourney(ride, packet.GetPassengerLegacyHop());
	}
	if (vehicle != nullptr && packet.GetPassengerRoutingMode() != PassengerRoutingMode::Journey &&
			IsPassengerJourneyRoutingEnabled(vehicle->cargo_type) && Station::IsValidID(packet.GetPassengerDestination())) {
		this->fallback.PrepareBoard(packet, vehicle, vehicle->cargo_type);
	}
}

void PassengerJourneyLoadingContext::Invalidate(const Vehicle *vehicle)
{
	ObservePassengerPeriodicVehicle(vehicle->First());
	this->fallback.Invalidate(vehicle);
	VehicleID id = vehicle->First()->index;
	for (auto it = this->boarding.lower_bound({id, CargoType{0}}); it != this->boarding.end() && it->first.first == id;) it = this->boarding.erase(it);
}

size_t PassengerJourneyLoadingContext::GetPredictionCount() const
{
	size_t count = 0;
	for (const auto &[key, context] : this->boarding) count += context.GetPredictionCount();
	return count;
}

std::optional<VehicleCargoList::MoveToAction> PassengerJourneyActiveContext::Decide(CargoPacket &packet, CargoType cargo, bool accepted, OrderUnloadType unload)
{
	PassengerJourneyWorkTimer timer;
	using Action = VehicleCargoList::MoveToAction;
	if (packet.GetPassengerRoutingMode() != PassengerRoutingMode::Journey) {
		auto action = this->fallback.Decide(packet, this->vehicle, cargo, this->station, accepted, unload);
		/* A former journey can continue through fallback. Its next real transfer
		 * must replace the previous arriving identity and minimum-ready time too. */
		if (action == Action::Transfer && packet.GetPassengerArrivingService() != OrderListID::Invalid() && this->vehicle != nullptr && this->vehicle->orders != nullptr) {
			packet.SetPassengerInterchange(_state_ticks + StateTicksDelta{ActiveJourneyEvaluation().minimum_transfer}, this->vehicle->orders->index, this->vehicle->orders->GetCargoDistRouteGeneration());
		}
		return action;
	}
	if (packet.GetPassengerTransport() == PassengerTransportKind::None) packet.SetPassengerTransport(IsEstimatedPassengerRide(packet.GetPassengerJourney().kind) ? PassengerTransportKind::Schedule : PassengerTransportKind::Timed);
	Snapshot *snapshot = nullptr;
	this->failure = PassengerJourneyFallbackReason::InvalidLeg;
	this->deferred = false;
	auto fallback = [&]() -> std::optional<Action> {
		if (snapshot != nullptr) { snapshot->failure = this->failure; snapshot->deferred = this->deferred; }
		const bool budget_failure = this->deferred || this->failure == PassengerJourneyFallbackReason::GraphWork ||
				this->failure == PassengerJourneyFallbackReason::GraphMemory || this->failure == PassengerJourneyFallbackReason::QueryWork ||
				this->failure == PassengerJourneyFallbackReason::QueryMemory || this->failure == PassengerJourneyFallbackReason::QueryLimit;
		if (budget_failure) ++TickBudget().diagnostic.timed_budget_deferred;
		if ((budget_failure || this->failure == PassengerJourneyFallbackReason::PredictionIncomplete ||
				this->failure == PassengerJourneyFallbackReason::PredictionHorizon || this->failure == PassengerJourneyFallbackReason::NoJourney) && unload != OrderUnloadType::Unload && unload != OrderUnloadType::Transfer &&
				this->continuation.try_emplace(cargo, this->vehicle, cargo).first->second.CanContinue(packet)) {
			CountJourneyDeferral(this->failure);
			++TickBudget().diagnostic.retained_rides;
			return Action::Keep;
		}
		if (this->deferred) { CountJourneyDeferral(this->failure); packet.DeferPassengerJourney(this->station, _state_ticks); }
		else { CountJourneyFallback(this->failure); packet.FallbackPassengerJourney(this->station, _state_ticks); }
		return this->fallback.Decide(packet, this->vehicle, cargo, this->station, accepted, unload);
	};
	if (!IsPassengerJourneyRoutingEnabled(cargo) || !IsPassengerServiceVehicle(this->vehicle) ||
			!Station::IsValidID(packet.GetPassengerDestination())) return fallback();
	if (unload == OrderUnloadType::NoUnload) return Action::Keep;
	if (packet.GetPassengerDestination() == this->station) return accepted ? std::optional(Action::Deliver) : fallback();
	if (_settings_game.linkgraph.passenger_journey_periodic || packet.GetPassengerJourney().kind != PassengerRideKind::VehicleSnapshot) {
		const auto &ride = packet.GetPassengerJourney();
		const bool interchange = ride.alighting == this->station && ride.alighting_order == this->vehicle->cur_real_order_index;
		if (unload != OrderUnloadType::Unload && unload != OrderUnloadType::Transfer && !interchange &&
				this->continuation.try_emplace(cargo, this->vehicle, cargo).first->second.CanContinue(packet)) return Action::Keep;
		/* Arrival at the planned interchange is a new routing event, not a failed
		 * retry at this station. Plan the next intent before staging the transfer. */
		packet.SetPassengerInterchange(_state_ticks + StateTicksDelta{ActiveJourneyEvaluation().minimum_transfer}, this->vehicle->orders == nullptr ? OrderListID::Invalid() : this->vehicle->orders->index,
				this->vehicle->orders == nullptr ? 0 : this->vehicle->orders->GetCargoDistRouteGeneration());
		if (IsEstimatedPassengerRide(ride.kind) && !interchange) RecordPassengerScheduleInvalidation(unload == OrderUnloadType::Unload || unload == OrderUnloadType::Transfer ? PassengerScheduleInvalidation::ForcedUnload : PassengerScheduleInvalidation::LiveRide);
		packet.FallbackPassengerJourney(StationID::Invalid(), _state_ticks); packet.SetNextHop(StationID::Invalid());
		this->PlanWaiting(packet, cargo, StationID::Invalid());
		return Action::Transfer;
	}
	for (Ticks horizon = 4096;; horizon = std::min<Ticks>(65536, horizon * 4)) {
		snapshot = this->GetSnapshot(cargo, packet.GetPassengerDestination(), horizon);
		if (snapshot == nullptr) return fallback();
		if (snapshot->graph.GetQueryStatus(this->station, packet.GetPassengerDestination()) >= PassengerPredictionStatus::LimitReached) {
			this->deferred = snapshot->budget_limited && snapshot->graph.GetQueryStatus(this->station, packet.GetPassengerDestination()) >= PassengerPredictionStatus::LimitReached;
			this->failure = snapshot->graph.GetStatus() == PassengerPredictionStatus::BudgetExhausted ? PassengerJourneyFallbackReason::GraphWork :
					snapshot->graph.GetStatus() == PassengerPredictionStatus::LimitReached ? PassengerJourneyFallbackReason::GraphMemory : PassengerJourneyFallbackReason::PredictionIncomplete;
			return fallback();
		}
		if (snapshot->onboard.size() >= 128 && !snapshot->onboard.contains(packet.GetPassengerDestination())) return fallback();
		auto [cached, inserted] = snapshot->onboard.try_emplace(packet.GetPassengerDestination());
		if (!inserted) {
			++TickBudget().diagnostic.query_cache_hits;
			if (!cached->second) {
				if (snapshot->failure == PassengerJourneyFallbackReason::PredictionHorizon && horizon < 65536) continue;
				this->failure = snapshot->failure; this->deferred = snapshot->deferred; return fallback();
			}
			packet.SetPassengerJourney(cached->second->first, StationID::Invalid());
			packet.SetNextHop(StationID::Invalid());
			TickBudget().diagnostic.timed_routes_ignoring_unknown += snapshot->onboard_ignored_unknown;
			++TickBudget().diagnostic.journeys; TickBudget().diagnostic.passengers += packet.Count();
			return cached->second->second ? Action::Keep : Action::Transfer;
		}
		auto onboard = snapshot->graph.FindOnboardVisit(this->vehicle->index, this->station, this->vehicle->cur_real_order_index);
		auto limits = ActiveJourneyLimits();
		if (!onboard) return fallback();
		if (!this->AdmitQuery()) return fallback();
		const auto query_start = std::chrono::steady_clock::now();
		auto result = snapshot->planner.PlanOnboard(cargo, *onboard, packet.GetPassengerDestination(), limits, ActiveJourneyEvaluation(), 1);
		CountJourneySearch(result, snapshot->planner, query_start, this->station, packet.GetPassengerDestination());
		if (result.stop_reason == PassengerJourneyStopReason::PredictionHorizon ||
				(result.GetOutcome() == PassengerJourneyOutcome::TimedRouteFound && result.journeys.front().generalized_time.base() >= horizon)) {
			snapshot->failure = PassengerJourneyFallbackReason::PredictionHorizon;
			if (horizon < 65536) continue;
			this->failure = PassengerJourneyFallbackReason::PredictionHorizon; return fallback();
		}
		if (result.GetOutcome() != PassengerJourneyOutcome::TimedRouteFound || result.journeys.front().legs.empty()) {
			this->failure = JourneySearchFailure(result); return fallback();
		}
		const auto &leg = result.journeys.front().legs.front();
		auto execution = this->Execute(*snapshot, cargo, leg);
		if (!execution) return fallback();
		bool staying = leg.vehicle == this->vehicle->index && leg.boarding == *onboard;
		if (staying && (unload == OrderUnloadType::Unload || unload == OrderUnloadType::Transfer)) return fallback();
		cached->second = std::pair(*execution, staying);
		snapshot->onboard_ignored_unknown = result.diagnostic.unknown_alternatives_ignored;
		packet.SetPassengerJourney(*execution, StationID::Invalid());
		packet.SetNextHop(StationID::Invalid());
		TickBudget().diagnostic.timed_routes_ignoring_unknown += snapshot->onboard_ignored_unknown;
		++TickBudget().diagnostic.journeys; TickBudget().diagnostic.passengers += packet.Count();
		return staying ? Action::Keep : Action::Transfer;
	}
}

size_t StationCargoList::RoutePassengerJourneys(CargoType cargo, StationID station, bool allow_search, PassengerJourneyLoadingContext *loading)
{
	CounterTimer category{TickBudget().diagnostic.waiting_microseconds};
	PassengerJourneyActiveContext context(nullptr, station);
	PassengerJourneyLoadingContext local_loading;
	if (loading == nullptr) loading = &local_loading;
	/* New OD packets are considered before boarding. OD fallback/deferred queues
	 * share the filtered bucket, allowing a verified through ride past intermediate stops. */
	/* Spread national-map waiting cohorts across 1024 ticks instead of 64.
	 * Each station receives a search opportunity; an overloaded cohort still
	 * consumes the shared allowance in deterministic simulation order. */
	const bool search = IsJourneyRetryTick(station);
	std::vector<std::pair<StationID, CargoPacket *>> moved;
	std::map<OrderListID, uint64_t> signatures;
	std::map<std::tuple<VehicleID, VehicleOrderID, VehicleOrderID, StationID>, bool> usable;
	std::map<std::tuple<OrderListID, uint64_t, VehicleOrderID, VehicleOrderID, StationID>, bool> periodic_available;
	size_t validation_work = 524288;
	size_t inspected = 0;
	bool changed = false;
	StationCargoPacketMap::Map &queues = this->packets;
	for (auto bucket = queues.begin(); bucket != queues.end();) {
		auto &list = bucket->second;
		size_t kept = 0;
		for (CargoPacket *packet : list) {
			StationID key = bucket->first;
			PassengerRoutingMode previous_mode = packet->GetPassengerRoutingMode();
			if (packet->GetPassengerRoutingMode() != PassengerRoutingMode::Journey && key == StationID::Invalid()) key = packet->GetPassengerLegacyHop();
			if (packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey) {
				const auto &leg = packet->GetPassengerJourney();
				if (leg.kind != PassengerRideKind::VehicleSnapshot) {
					if (!IsPassengerJourneyRoutingEnabled(cargo) || !Station::IsValidID(packet->GetPassengerDestination()) || leg.boarding != station ||
							!Station::IsValidID(leg.alighting) || (allow_search && !GetPassengerServiceDatabase().IsCurrent(leg))) {
						CountJourneyFallback(PassengerJourneyFallbackReason::StaleLeg);
						if (IsEstimatedPassengerRide(leg.kind)) {
							++TickBudget().diagnostic.schedule_stale;
							const auto *s = GetPassengerServiceDatabase().Get(leg.service);
							RecordPassengerScheduleInvalidation(!IsPassengerJourneyRoutingEnabled(cargo) ? PassengerScheduleInvalidation::Disabled :
									!Station::IsValidID(packet->GetPassengerDestination()) || leg.boarding != station || !Station::IsValidID(leg.alighting) ? PassengerScheduleInvalidation::Station :
									s == nullptr ? PassengerScheduleInvalidation::ServiceMissing : s->incarnation != leg.signature ? PassengerScheduleInvalidation::Incarnation : PassengerScheduleInvalidation::Generation);
						}
						GetPassengerServiceDatabase().RecordExpiry(packet->Count());
						key = packet->GetPassengerLegacyHop(); packet->FallbackPassengerJourney(station, _state_ticks);
					} else {
						key = StationID::Invalid();
						if (IsBoundPassengerRide(leg.kind)) {
							auto intent = leg; intent.kind = IsEstimatedPassengerRide(leg.kind) ? PassengerRideKind::EstimatedServiceIntent : PassengerRideKind::ServiceIntent; intent.vehicle = VehicleID::Invalid();
							packet->SetPassengerJourney(intent, packet->GetPassengerLegacyHop());
						}
						bool estimated_expired = IsEstimatedPassengerRide(leg.kind) && _state_ticks > leg.planned_at &&
								uint64_t(_state_ticks.base()) - uint64_t(leg.planned_at.base()) > uint64_t(leg.waiting_limit);
						if (estimated_expired && allow_search) {
							++TickBudget().diagnostic.schedule_expired; key = packet->GetPassengerLegacyHop();
							RecordPassengerScheduleInvalidation(PassengerScheduleInvalidation::WaitingExpired);
							packet->DeferPassengerJourney(station, _state_ticks + StateTicksDelta{-1});
						} else if (allow_search && !IsEstimatedPassengerRide(leg.kind) && _state_ticks > leg.departure) {
							auto ride_key = std::tuple(leg.service, leg.signature, leg.boarding_order, leg.alighting_order, leg.alighting);
							auto available = periodic_available.find(ride_key);
							if (available == periodic_available.end() && periodic_available.size() < 4096) {
								bool present = false;
								for (const Vehicle *v : Station::Get(station)->loading_vehicles) {
									if (validation_work == 0) break;
									--validation_work;
									if (v->orders == nullptr || v->orders->index != leg.service || v->current_order.IsType(OT_LOADING_ADVANCE) || !loading->CanBoard(*packet, v, cargo)) continue;
									for (const Vehicle *part = v; part != nullptr && validation_work > 0; part = part->Next()) {
										--validation_work;
										if (part->cargo_type == cargo && part->cargo_cap > part->cargo.RemainingCount()) { present = true; break; }
									}
									if (present) break;
								}
								available = periodic_available.emplace(ride_key, present).first;
							}
							if (available == periodic_available.end() || !available->second) {
								key = packet->GetPassengerLegacyHop(); CountJourneyFallback(PassengerJourneyFallbackReason::StaleLeg);
								GetPassengerServiceDatabase().RecordExpiry(packet->Count());
								packet->DeferPassengerJourney(station, _state_ticks + StateTicksDelta{-1}); // Expiry is an immediate routing event.
							}
						}
					}
				} else {
				const Vehicle *vehicle = Vehicle::GetIfValid(leg.vehicle);
				bool valid = IsPassengerJourneyRoutingEnabled(cargo) && IsPassengerServiceVehicle(vehicle) &&
						!vehicle->vehstatus.Any({VehState::Stopped, VehState::Crashed}) && Station::IsValidID(packet->GetPassengerDestination()) &&
						vehicle->orders != nullptr && vehicle->orders->index == leg.service && leg.boarding == station &&
						Station::IsValidID(leg.alighting);
				if (valid) {
					auto key = std::tuple(leg.vehicle, leg.boarding_order, leg.alighting_order, leg.alighting);
					auto entry = usable.find(key);
					bool inserted = false;
					if (entry == usable.end() && usable.size() < 4096 && validation_work >= 16) {
						validation_work -= 16; // Bound unique validation records as well as facility traversal.
						std::tie(entry, inserted) = usable.try_emplace(key, false);
					}
					if (inserted && leg.boarding_order < vehicle->GetNumOrders() && leg.alighting_order < vehicle->GetNumOrders()) {
						const Order *board = vehicle->GetOrder(leg.boarding_order), *alight = vehicle->GetOrder(leg.alighting_order);
						entry->second = board->GetDestination() == station && alight->GetDestination() == leg.alighting &&
								board->GetCargoLoadType(cargo) != OrderLoadType::NoLoad && alight->GetCargoUnloadType(cargo) != OrderUnloadType::NoUnload &&
								HasPassengerVehicleCapacity(vehicle, cargo, validation_work) &&
								CanPassengerVehicleUseStation(vehicle, station, board, validation_work) &&
								CanPassengerVehicleUseStation(vehicle, leg.alighting, alight, validation_work);
					}
					valid = entry != usable.end() && entry->second;
				}
				if (valid && vehicle->current_order.IsAnyLoadingType() && vehicle->last_station_visited == station) {
					valid = loading->CanBoard(*packet, vehicle, cargo);
				} else if (valid) {
					auto [signature, inserted] = signatures.try_emplace(leg.service, 0);
					if (inserted) signature->second = PassengerJourneyServiceSignature(*vehicle->orders, cargo, &validation_work);
					valid = _state_ticks <= leg.departure && signature->second != 0 && leg.signature == signature->second;
				}
				if (!valid) {
					CountJourneyFallback(IsPassengerJourneyRoutingEnabled(cargo) ? PassengerJourneyFallbackReason::StaleLeg : PassengerJourneyFallbackReason::Disabled);
					key = packet->GetPassengerLegacyHop(); packet->FallbackPassengerJourney(station, _state_ticks);
				}
				else key = StationID::Invalid();
				}
			}
			if (IsPassengerJourneyRoutingEnabled(cargo) && allow_search && CanSearchWaitingJourney(*packet, station, search) && inspected < 4096) {
				++inspected;
				if (context.PlanWaiting(*packet, cargo, key)) key = StationID::Invalid();
				else if (!packet->IsPassengerJourneyDeferred()) packet->FallbackPassengerJourney(station, _state_ticks);
			}
			if (IsPassengerJourneyRoutingEnabled(cargo) && Station::IsValidID(packet->GetPassengerDestination()) &&
					packet->GetPassengerRoutingMode() != PassengerRoutingMode::Journey) {
				packet->SetPassengerLegacyHop(key);
				key = StationID::Invalid(); // CanBoard checks OD progression, including reservations and refits.
			}
			changed |= key != bucket->first || previous_mode != packet->GetPassengerRoutingMode();
			if (key != bucket->first) {
				moved.emplace_back(key, packet);
			} else {
				/* Compact once per bucket, avoiding quadratic ring-buffer erases. */
				if (list[kept] != packet) list[kept] = packet;
				++kept;
			}
		}
		while (list.size() > kept) list.pop_back();
		if (list.empty()) bucket = queues.erase(bucket);
		else ++bucket;
	}
	/* Delay insertion so relocated packets cannot be revisited in this pass. Unchanged
	 * buckets retain their storage and packet order; only changed queue keys allocate. */
	for (const auto &[key, packet] : moved) this->packets.Insert(key, packet);
	if (changed) SetWindowDirty(WindowClass::StationView, station);
	return moved.size();
}

void RouteWaitingPassengerJourneys(Station *station, PassengerJourneyLoadingContext *loading)
{
	PassengerJourneyWorkTimer timer;
	if (!_settings_game.linkgraph.passenger_journey_routing) return;
	for (CargoType cargo : EnumRange(NUM_CARGO)) {
		if (!IsCargoInClass(cargo, CargoClass::Passengers) || station->goods[cargo].data == nullptr) continue;
		station->goods[cargo].data->cargo.RoutePassengerJourneys(cargo, station->index, true, loading);
	}
}

std::array<uint, 3> CountPassengerRoutingModes(const StationCargoList &cargo)
{
	std::array<uint, 3> counts{};
	for (StationCargoList::ConstIterator it(cargo.Packets()->begin()); it != cargo.Packets()->end(); ++it) counts[to_underlying((*it)->GetPassengerRoutingMode())] += (*it)->Count();
	return counts;
}

std::array<uint, 3> CountReservedPassengerRoutingModes(const Station &station, CargoType cargo)
{
	std::array<uint, 3> counts{};
	for (const Vehicle *front : station.loading_vehicles) {
		for (const Vehicle *v = front; v != nullptr; v = v->Next()) {
			if (v->cargo_type != cargo) continue;
			uint remaining = v->cargo.ReservedCount();
			for (auto it = v->cargo.Packets()->rbegin(); remaining > 0 && it != v->cargo.Packets()->rend(); ++it) {
				uint count = std::min(remaining, static_cast<uint>((*it)->Count()));
				counts[to_underlying((*it)->GetPassengerRoutingMode())] += count;
				remaining -= count;
			}
		}
	}
	return counts;
}

void PassengerJourneySettingsChanged(int32_t)
{
	InvalidatePassengerServiceDatabase();
	/* Reserved cargo lives on vehicles. Return it before changing queue keys or mode. */
	for (Station *station : Station::Iterate()) {
		for (Vehicle *front : station->loading_vehicles) {
			for (Vehicle *v = front; v != nullptr; v = v->Next()) {
				if (!IsCargoInClass(v->cargo_type, CargoClass::Passengers) || IsPassengerJourneyRoutingEnabled(v->cargo_type)) continue;
				v->cargo.ReturnReserved(&station->goods[v->cargo_type].CreateData().cargo, StationID::Invalid(), v->GetCargoTile(),
						[](const CargoPacket &packet) { return packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey; });
				for (CargoPacket *packet : *v->cargo.Packets()) if (packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey) {
					CountJourneyFallback(PassengerJourneyFallbackReason::Disabled);
					packet->FallbackPassengerJourney(station->index, _state_ticks);
				}
			}
		}
		for (CargoType cargo : EnumRange(NUM_CARGO)) {
			if (IsCargoInClass(cargo, CargoClass::Passengers) && station->goods[cargo].data != nullptr) {
				station->goods[cargo].data->cargo.RoutePassengerJourneys(cargo, station->index, false);
			}
		}
	}
	for (CargoPacket *packet : CargoPacket::Iterate()) {
		if (!_settings_game.linkgraph.passenger_journey_routing && packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey) {
			CountJourneyFallback(PassengerJourneyFallbackReason::Disabled);
			packet->FallbackPassengerJourney(StationID::Invalid(), _state_ticks);
		}
	}
}

void AfterLoadPassengerJourneys()
{
	ClearPassengerOdFallbackCache();
	GetPassengerServiceDatabase().FinishLoad(SlXvIsFeaturePresent(XSLFI_PASSENGER_PERIODIC));
	if (SlXvIsFeatureMissing(XSLFI_PASSENGER_PERIODIC)) _settings_game.linkgraph.passenger_journey_periodic = true;
	InvalidatePassengerServiceIndex();
	BeginPassengerJourneyTick();
	/* Saveload writes packet fields directly; reconstruct the derived index before
	 * validation changes modes or vehicles can be deleted/reuse their pool IDs. */
	RebuildPassengerJourneyVehicleIndex();
	if (SlXvIsFeatureMissing(XSLFI_PASSENGER_JOURNEY_ROUTING)) _settings_game.linkgraph.passenger_journey_routing = false;
	std::set<CargoPacketID> invalidated;
	for (CargoPacket *packet : CargoPacket::Iterate()) {
		if (packet->GetPassengerRoutingMode() != PassengerRoutingMode::Journey) continue;
		const auto &leg = packet->GetPassengerJourney();
		if (leg.kind != PassengerRideKind::VehicleSnapshot) {
			/* A saved intent can refer to a dirty service awaiting publication.
			 * Preserve it until the ordinary simulation event, including retry time.
			 * Live boarding/continuation checks cannot use stale pool identities. */
			continue;
		}
		const Vehicle *vehicle = Vehicle::GetIfValid(leg.vehicle);
		if (!IsPassengerServiceVehicle(vehicle) || vehicle->orders == nullptr || vehicle->orders->index != leg.service ||
				!Station::IsValidID(leg.boarding) || !Station::IsValidID(leg.alighting) || !Station::IsValidID(packet->GetPassengerDestination()) ||
				leg.boarding_order >= vehicle->GetNumOrders() || leg.alighting_order >= vehicle->GetNumOrders() || !_settings_game.linkgraph.passenger_journey_routing) {
			CountJourneyFallback(PassengerJourneyFallbackReason::StaleLeg);
			packet->FallbackPassengerJourney(StationID::Invalid(), _state_ticks);
			invalidated.insert(packet->index);
		} else {
			const Order *board = vehicle->GetOrder(leg.boarding_order), *alight = vehicle->GetOrder(leg.alighting_order);
			if (!board->IsType(OT_GOTO_STATION) || board->GetDestination() != leg.boarding || !alight->IsType(OT_GOTO_STATION) ||
					alight->GetDestination() != leg.alighting || leg.arrival < leg.departure) {
				CountJourneyFallback(PassengerJourneyFallbackReason::StaleLeg);
				packet->FallbackPassengerJourney(StationID::Invalid(), _state_ticks);
				invalidated.insert(packet->index);
			}
		}
	}
	/* A primary engine may carry a different cargo. Validate the saved hash using the
	 * owning coach's actual cargo, and return invalid reserved suffixes through CargoReturn. */
	for (Vehicle *v : Vehicle::Iterate()) {
		const Vehicle *front = v->First();
		bool invalid = false;
		std::map<std::tuple<VehicleOrderID, VehicleOrderID, StationID, StationID>, bool> usable;
		size_t validation_work = 65536;
		for (CargoPacket *packet : *v->cargo.Packets()) {
			if (packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey && packet->GetPassengerJourney().kind != PassengerRideKind::VehicleSnapshot) continue;
			if (packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey && front->orders != nullptr) {
				const auto &leg = packet->GetPassengerJourney();
				auto key = std::tuple(leg.boarding_order, leg.alighting_order, leg.boarding, leg.alighting);
				auto entry = usable.find(key);
				bool inserted = false;
				if (entry == usable.end() && usable.size() < 4096 && validation_work >= 16) {
					validation_work -= 16;
					std::tie(entry, inserted) = usable.try_emplace(key, false);
				}
				if (inserted && leg.boarding_order < front->GetNumOrders() && leg.alighting_order < front->GetNumOrders()) {
					entry->second = HasPassengerVehicleCapacity(front, v->cargo_type, validation_work) &&
							CanPassengerVehicleUseStation(front, leg.boarding, front->GetOrder(leg.boarding_order), validation_work) &&
							CanPassengerVehicleUseStation(front, leg.alighting, front->GetOrder(leg.alighting_order), validation_work);
				}
				if (entry == usable.end() || !entry->second) { packet->FallbackPassengerJourney(front->last_station_visited, _state_ticks); invalidated.insert(packet->index); }
			}
			if (packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey && (!IsPassengerJourneyRoutingEnabled(v->cargo_type) ||
					front->orders == nullptr || packet->GetPassengerJourney().vehicle != front->index ||
					packet->GetPassengerJourney().signature != (packet->GetPassengerJourney().kind == PassengerRideKind::VehicleSnapshot ?
							PassengerJourneyServiceSignature(*front->orders, v->cargo_type) : front->orders->GetCargoDistRouteGeneration()))) {
				CountJourneyFallback(PassengerJourneyFallbackReason::StaleLeg);
				packet->FallbackPassengerJourney(front->last_station_visited, _state_ticks);
				invalid = true;
				invalidated.insert(packet->index);
			}
			if (invalidated.contains(packet->index)) invalid = true;
		}
		Station *station = Station::GetIfValid(front->last_station_visited);
		if (invalid && v->cargo.ReservedCount() > 0 && station != nullptr) v->cargo.ReturnReserved(
				&station->goods[v->cargo_type].CreateData().cargo, StationID::Invalid(), v->GetCargoTile(),
				[&](const CargoPacket &packet) { return invalidated.contains(packet.index); });
	}
	/* Preserve saved waiting queue keys and packet order. Reclassifying here
	 * changes station rating/truncation RNG on a joining client. Ordinary
	 * routing and boarding events validate and rekey packets on all peers. */
	Debug(sl, 2, "Passenger journey afterload: feature {}, enabled {}", _sl_xv_feature_versions[XSLFI_PASSENGER_JOURNEY_ROUTING],
			_settings_game.linkgraph.passenger_journey_routing);
}
