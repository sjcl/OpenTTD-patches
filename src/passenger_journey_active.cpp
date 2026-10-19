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
#include "core/math_func.hpp"
#include "core/hash_func.hpp"
#include "network/network.h"
#include <set>
#include <chrono>

#include "safeguards.h"

namespace {
	static constexpr size_t PERIODIC_QUERY_LIMIT = PassengerQueryAdmission::QUERIES_PER_TICK;
	static constexpr size_t PERIODIC_TIMED_WORK = 49152;
	static constexpr size_t PERIODIC_SCHEDULE_PROTECTION = PassengerQueryAdmission::SCHEDULE_PROTECTION;
	static constexpr size_t FAIR_GRAPH_WORK = 2 * 1024 * 1024, FAIR_QUERY_WORK = 128 * 1024;
	struct JourneyFairShare {
		StationID station = StationID::Invalid();
		bool claimed = false;
	};
	struct JourneyTickBudget {
		StateTicks tick = INVALID_STATE_TICKS;
		size_t graph = 16 * 1024 * 1024;
		size_t query = 1024 * 1024;
		size_t periodic_queries = PERIODIC_QUERY_LIMIT;
		size_t snapshots = 16;
		size_t od_fallback = 4 * 1024 * 1024;
		PassengerJourneyTickDiagnostic diagnostic;
		std::array<JourneyFairShare, 4> fair_shares;
		bool fair_shares_initialized = false;
	};
	static JourneyTickBudget tick_budget;
	static PassengerJourneyTickDiagnostic total_diagnostic;
	static size_t journey_timer_depth = 0;
	/* Bounded local observations only; losing an entry never changes retry or admission. */
	static std::map<std::tuple<CargoType, StationID, StationID>, uint64_t> periodic_attempts;
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
	struct NanosecondTimer {
		uint64_t &counter;
		std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
		~NanosecondTimer() { this->counter += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - this->start).count(); }
	};
	using BindingNodes = std::vector<std::pair<StationID, bool>>;
	struct ResidentBinding { std::shared_ptr<const BindingNodes> nodes; uint64_t token; size_t bytes; };
	using BindingKey = std::tuple<LinkGraphID, uint64_t, uint64_t, NodeID, CargoType>;
	static std::map<BindingKey, ResidentBinding> resident_bindings;
	static bool resident_fallback_enabled = true, validate_resident_bindings = false;
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
		a.admission.accepted += b.admission.accepted; a.admission.rejected += b.admission.rejected;
		a.admission.reserved += b.admission.reserved; a.admission.used += b.admission.used; a.admission.returned += b.admission.returned;
		a.admission.timed_allocated += b.admission.timed_allocated; a.admission.schedule_allocated += b.admission.schedule_allocated;
		a.admission.timed_used += b.admission.timed_used; a.admission.schedule_used += b.admission.schedule_used;
		a.admission.work_stopped += b.admission.work_stopped; a.admission.work_no_route += b.admission.work_no_route;
		a.admission.schedule_rescued += b.admission.schedule_rescued;
		a.admission.complete_unproved += b.admission.complete_unproved; a.admission.incomplete_work += b.admission.incomplete_work;
		a.admission.missing_published += b.admission.missing_published; a.admission.missing_building += b.admission.missing_building;
		a.admission.retries += b.admission.retries; a.admission.retry_tracking_evictions += b.admission.retry_tracking_evictions;
		for (size_t i = 0; i < b.admission.retry_sample_count && a.admission.retry_sample_count < a.admission.retry_samples.size(); ++i) a.admission.retry_samples[a.admission.retry_sample_count++] = b.admission.retry_samples[i];
		a.admission.maximum_od_attempts = std::max(a.admission.maximum_od_attempts, b.admission.maximum_od_attempts);
		a.admission.maximum_tick_queries = std::max(a.admission.maximum_tick_queries, b.admission.maximum_tick_queries);
		a.admission.maximum_tick_work = std::max(a.admission.maximum_tick_work, b.admission.maximum_tick_work);
		for (size_t i = 0; i < a.admission.rejections.size(); ++i) a.admission.rejections[i] += b.admission.rejections[i];
		for (size_t i = 0; i < a.admission.tick_queries.size(); ++i) a.admission.tick_queries[i] += b.admission.tick_queries[i];
		for (size_t i = 0; i < a.admission.tick_work.size(); ++i) a.admission.tick_work[i] += b.admission.tick_work[i];
		for (size_t i = 0; i < a.graph_failures.size(); ++i) a.graph_failures[i] += b.graph_failures[i];
		for (size_t i = 0; i < a.prediction_stops.size(); ++i) a.prediction_stops[i] += b.prediction_stops[i];
		if (a.first_graph_failure.reason == PassengerGraphFailure::None) a.first_graph_failure = b.first_graph_failure;
		if (a.first_prediction_failure.reason == PassengerPredictionStop::None) a.first_prediction_failure = b.first_prediction_failure;
		if (a.opaque_service == OrderListID::Invalid()) { a.opaque_service = b.opaque_service; a.opaque_vehicle = b.opaque_vehicle; }
		a.complete_graphs += b.complete_graphs; a.admitted_visits += b.admitted_visits;
		a.timing_uncertainty += b.timing_uncertainty; a.path_uncertainty += b.path_uncertainty; a.uncertainty_resolved += b.uncertainty_resolved;
		a.timed_routes_ignoring_unknown += b.timed_routes_ignoring_unknown; a.timed_budget_deferred += b.timed_budget_deferred;
		a.timed_certified_packets += b.timed_certified_packets; a.timed_provisional_packets += b.timed_provisional_packets;
		a.timed_provisional_passengers += b.timed_provisional_passengers;
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
		a.schedule_expected_wait = SaturatingAdd(a.schedule_expected_wait, b.schedule_expected_wait);
		a.schedule_actual_wait = SaturatingAdd(a.schedule_actual_wait, b.schedule_actual_wait);
		a.schedule_wait_samples = SaturatingAdd(a.schedule_wait_samples, b.schedule_wait_samples);
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
		a.od_fallback_binding_hits += b.od_fallback_binding_hits; a.od_fallback_binding_builds += b.od_fallback_binding_builds;
		a.od_fallback_binding_nodes += b.od_fallback_binding_nodes;
		a.od_fallback_reverse_hits += b.od_fallback_reverse_hits; a.od_fallback_reverse_builds += b.od_fallback_reverse_builds;
		a.od_fallback_binding_validation_nanoseconds += b.od_fallback_binding_validation_nanoseconds;
		a.od_fallback_binding_build_nanoseconds += b.od_fallback_binding_build_nanoseconds;
		a.od_fallback_reverse_build_nanoseconds += b.od_fallback_reverse_build_nanoseconds;
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
	++diagnostic.admission.tick_queries[std::min<uint64_t>(64, diagnostic.admission.accepted)];
	++diagnostic.admission.tick_work[std::min<uint64_t>(64, (diagnostic.admission.used + 16383) / 16384)];
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
void PreparePassengerJourneyLoading()
{
	PassengerJourneyWorkTimer timer;
	if (!_settings_game.linkgraph.passenger_journey_precomputed || GetPassengerServiceDatabase().HasRejectedPrecomputedMemory()) InitializeFairShares(TickBudget());
}
void ReleasePassengerJourneyWaitingShare(StationID station)
{
	if (!_settings_game.linkgraph.passenger_journey_routing) return;
	if (_settings_game.linkgraph.passenger_journey_precomputed && !tick_budget.fair_shares_initialized) return;
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
void ResetPassengerJourneyDiagnostic() { total_diagnostic = {}; tick_budget.diagnostic = {}; periodic_attempts.clear(); GetPassengerServiceDatabase().ResetDiagnostic(); ResetPassengerWaitingEventDiagnostic(); }
void ClearPassengerOdFallbackCache()
{
	resident_distances.clear(); resident_distance_bytes = 0; resident_bindings.clear(); resident_binding_bytes = 0;
	reverse_indices.clear(); reverse_bytes = 0; binding_token = distance_use = 0;
}
void SetPassengerOdFallbackCacheEnabled(bool enabled)
{
	resident_fallback_enabled = enabled;
	ClearPassengerOdFallbackCache();
}
void SetPassengerOdFallbackBindingValidation(bool enabled) { validate_resident_bindings = enabled; }
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
					if (const auto age = GetPassengerScheduleIntentAge(ride, _state_ticks)) {
						/* Saturate extreme diagnostic observations rather than wrap their
						 * weighted sum. No timestamp or routing state is changed. */
						auto accumulate = [count](uint64_t &total, uint64_t value) {
							const uint64_t maximum = UINT64_MAX;
							total = count != 0 && value > (maximum - total) / count ? maximum : total + value * count;
						};
						accumulate(d.schedule_expected_wait, uint64_t(std::max<int64_t>(0, ride.estimated_wait)));
						accumulate(d.schedule_actual_wait, *age);
						accumulate(d.schedule_wait_samples, 1);
					}
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

/** Retain an unrestricted hop from this station. New hops follow CargoDist flow
 * weights using a deterministic packet/station/tick sample, without consuming RNG.
 * A later interchange gets a new sample rather than always choosing the lowest ID. */
static StationID CurrentPassengerLegacyHop(const CargoPacket &packet, CargoType cargo, StationID station, StationID preferred)
{
	const Station *current = Station::GetIfValid(station);
	if (current == nullptr || current->goods[cargo].data == nullptr) return StationID::Invalid();
	const auto &flows = current->goods[cargo].ConstFlows();
	auto flow = flows.find(packet.GetFirstStation());
	if (flow == flows.end() || flow->IsInvalid()) return StationID::Invalid();
	uint32_t eligible = 0, previous = 0;
	for (const auto &share : *flow) {
		if (share.first > flow->GetUnrestricted()) break;
		const uint32_t weight = share.first - previous;
		previous = share.first;
		if (share.second == station || !Station::IsValidID(share.second)) continue;
		if (share.second == preferred) return preferred;
		eligible += weight;
	}
	if (eligible == 0) return StationID::Invalid();
	const uint64_t seed = (uint64_t{packet.index.base()} << 32) ^ (uint64_t{station.base()} << 16) ^
			packet.GetPassengerDestination().base() ^ static_cast<uint64_t>(_state_ticks.base());
	uint32_t sample = static_cast<uint32_t>((uint64_t{static_cast<uint32_t>(SimpleHash64(seed))} * eligible) >> 32);
	previous = 0;
	for (const auto &share : *flow) {
		if (share.first > flow->GetUnrestricted()) break;
		const uint32_t weight = share.first - previous;
		previous = share.first;
		if (share.second == station || !Station::IsValidID(share.second)) continue;
		if (sample < weight) return share.second;
		sample -= weight;
	}
	return StationID::Invalid();
}

const PassengerPrecomputedPlan &PassengerJourneyActiveContext::QueryWaiting(const CargoPacket &packet, CargoType cargo)
{
	StateTicks ready = std::max(_state_ticks, packet.GetPassengerInterchangeReady());
	if (this->vehicle != nullptr) ready = std::max(ready, _state_ticks + StateTicksDelta{ActiveJourneyEvaluation().minimum_transfer});
	const auto *arriving = OrderList::GetIfValid(packet.GetPassengerArrivingService());
	OrderListID excluded = arriving != nullptr && arriving->GetCargoDistRouteGeneration() == packet.GetPassengerArrivingIncarnation() ? arriving->index : OrderListID::Invalid();
	/* One decision per OD/ready/incoming-service cohort in this slice. A
	 * packet's pool allocation history is not part of its routing condition. */
	const auto failed = packet.GetPassengerFailedService(_state_ticks);
	const auto key = std::tuple(cargo, packet.GetPassengerDestination(), ready, excluded, failed);
	auto cached = this->precomputed_waiting.find(key);
	if (cached == this->precomputed_waiting.end()) {
		RecordPassengerWaitingGroup();
		uint32_t sample = static_cast<uint32_t>(SimpleHash64((uint64_t{excluded.base()} << 32) ^
				(uint64_t{this->station.base()} << 16) ^ packet.GetPassengerDestination().base() ^ uint64_t(ready.base())));
		cached = this->precomputed_waiting.emplace(key,
				GetPassengerServiceDatabase().PlanPrecomputed(cargo, this->station, packet.GetPassengerDestination(), ready, excluded, sample, failed)).first;
	} else ++TickBudget().diagnostic.query_cache_hits;
	return cached->second;
}

bool PassengerJourneyActiveContext::PlanWaiting(CargoPacket &packet, CargoType cargo, StationID legacy_hop, bool require_hyperpath)
{
	PassengerJourneyWorkTimer timer;
	this->deferred = false;
	if (!IsPassengerJourneyRoutingEnabled(cargo) || !Station::IsValidID(packet.GetPassengerDestination()) ||
			packet.GetPassengerDestination() == this->station ||
			(!_settings_game.linkgraph.passenger_journey_precomputed && !packet.CanRetryPassengerJourney(this->station, _state_ticks))) return false;
	if (_settings_game.linkgraph.passenger_journey_precomputed) {
		const auto &plan = this->QueryWaiting(packet, cargo);
		/* Migration must not refresh a valid old intent's expiry from a retained
		 * legacy row while its replacement destination is still building. */
		if (require_hyperpath && (!plan.ride || plan.ride->kind != PassengerRideKind::HyperpathIntent)) return false;
		if (plan.ride) {
			packet.SetPassengerJourney(*plan.ride, legacy_hop);
			++TickBudget().diagnostic.schedule_packets; TickBudget().diagnostic.schedule_passengers += packet.Count();
			return true;
		}
		if (plan.state != PassengerPrecomputedPlan::State::Memory) {
			/* A previous admitted-search wait is no longer a next-tick retry.
			 * Missing/building rows wake this unresolved cohort on publication;
			 * the indexed loading path can still bind a real arriving train. */
			if (packet.IsPassengerJourneyDeferred()) packet.FallbackPassengerJourney(this->station, _state_ticks, legacy_hop);
			return false;
		}
		// A deterministic table-size rejection uses the original admitted planners.
	}
	if (require_hyperpath) return false;
	const StationID fallback_hop = CurrentPassengerLegacyHop(packet, cargo, this->station, legacy_hop);
	packet.SetPassengerLegacyHop(fallback_hop);
	if (_settings_game.linkgraph.passenger_journey_periodic) {
		StateTicks ready = packet.GetPassengerArrivingService() == OrderListID::Invalid() ? _state_ticks : std::max(_state_ticks, packet.GetPassengerInterchangeReady());
		if (this->vehicle != nullptr) ready = std::max(ready, _state_ticks + StateTicksDelta{ActiveJourneyEvaluation().minimum_transfer});
		const OrderList *arriving = OrderList::GetIfValid(packet.GetPassengerArrivingService());
		OrderListID excluded = arriving != nullptr && arriving->GetCargoDistRouteGeneration() == packet.GetPassengerArrivingIncarnation() ? arriving->index : OrderListID::Invalid();
		auto key = std::tuple(cargo, packet.GetPassengerDestination(), ready, excluded);
		auto apply = [&](const PassengerJourneyExecution &ride, bool provisional) {
			packet.SetPassengerJourney(ride, fallback_hop);
			if (IsEstimatedPassengerRide(ride.kind)) {
				++TickBudget().diagnostic.schedule_packets; TickBudget().diagnostic.schedule_passengers += packet.Count();
				TickBudget().diagnostic.schedule_deferred_packets += ride.timed_deferred;
			} else {
				++TickBudget().diagnostic.journeys; TickBudget().diagnostic.passengers += packet.Count();
				if (provisional) { ++TickBudget().diagnostic.timed_provisional_packets; TickBudget().diagnostic.timed_provisional_passengers += packet.Count(); }
				else ++TickBudget().diagnostic.timed_certified_packets;
			}
		};
		auto cached = this->periodic_waiting.find(key);
		if (cached != this->periodic_waiting.end()) {
			++TickBudget().diagnostic.query_cache_hits;
			if (!cached->second.ride) {
				if (cached->second.deferred) { CountJourneyDeferral(cached->second.failure); packet.DeferPassengerJourney(this->station, _state_ticks, fallback_hop); }
				return false;
			}
			apply(*cached->second.ride, cached->second.provisional);
			return true;
		}
		if (this->periodic_waiting.size() >= 128) return false;
		auto &global = TickBudget();
		JourneyFairShare *ticket = nullptr;
		if (this->vehicle == nullptr) for (auto &share : global.fair_shares) {
			if (share.station == this->station && !share.claimed) { ticket = &share; break; }
		}
		const size_t protected_shares = UnclaimedShares(global) - (ticket != nullptr);
		PassengerQueryAdmission admission(this->work_left, global.query, this->queries_left, global.periodic_queries,
				protected_shares * FAIR_QUERY_WORK, protected_shares * 2);
		auto &diagnostic = global.diagnostic.admission;
		if (!admission) {
			++diagnostic.rejected; ++diagnostic.rejections[static_cast<size_t>(admission.GetRejection())];
			this->failure = admission.GetRejection() == PassengerQueryAdmission::Rejection::EventQueries || admission.GetRejection() == PassengerQueryAdmission::Rejection::TickQueries || admission.GetRejection() == PassengerQueryAdmission::Rejection::FairQueries ?
					PassengerJourneyFallbackReason::QueryLimit : PassengerJourneyFallbackReason::QueryWork;
			this->deferred = true;
			this->periodic_waiting.emplace(key, PeriodicWaiting{{}, true, false, this->failure});
			CountJourneyDeferral(this->failure); packet.DeferPassengerJourney(this->station, _state_ticks, fallback_hop); return false;
		}
		if (ticket != nullptr) { ticket->claimed = true; ++global.diagnostic.fair_share_claims; }
		++global.diagnostic.queries; ++diagnostic.accepted;
		diagnostic.maximum_tick_queries = std::max<uint64_t>(diagnostic.maximum_tick_queries, PERIODIC_QUERY_LIMIT - global.periodic_queries);
		diagnostic.reserved += admission.Reserved();
		/* Destroyed before the lease returns its unused Work. Every exit, including
		 * exceptions and missing publications, records exactly one settlement. */
		struct Settlement {
			PassengerQueryAdmission &admission;
			PassengerJourneyTickDiagnostic &diagnostic;
			~Settlement()
			{
				auto &a = this->diagnostic.admission;
				a.used += this->admission.Used(); a.returned += this->admission.Returned();
				a.maximum_tick_work = std::max(a.maximum_tick_work, a.used);
				this->diagnostic.query_work += this->admission.Used();
			}
		} settlement{admission, global.diagnostic};
		auto retry_key = std::tuple(cargo, this->station, packet.GetPassengerDestination());
		if (!periodic_attempts.contains(retry_key) && periodic_attempts.size() >= 4096) { periodic_attempts.erase(periodic_attempts.begin()); ++diagnostic.retry_tracking_evictions; }
		auto attempts = ++periodic_attempts[retry_key]; diagnostic.retries += attempts > 1;
		if (attempts > 1 && diagnostic.retry_sample_count < diagnostic.retry_samples.size()) diagnostic.retry_samples[diagnostic.retry_sample_count++] = {cargo, this->station, packet.GetPassengerDestination(), attempts};
		diagnostic.maximum_od_attempts = std::max(diagnostic.maximum_od_attempts, attempts);
		auto limits = ActiveJourneyLimits(); limits.candidates = 8;
		limits.work = std::min(PERIODIC_TIMED_WORK, admission.TimedAllowance(PERIODIC_SCHEDULE_PROTECTION)); diagnostic.timed_allocated += limits.work;
		auto &db = GetPassengerServiceDatabase();
		auto plan = db.Plan(cargo, this->station, packet.GetPassengerDestination(), ready, limits, ActiveJourneyEvaluation(), excluded);
		admission.Charge(plan.work); diagnostic.timed_used += plan.work;
		const bool work_stopped = plan.reason == PassengerPeriodicPlan::Reason::Work;
		diagnostic.work_stopped += work_stopped;
		diagnostic.complete_unproved += plan.ride.has_value() && plan.deferred;
		diagnostic.incomplete_work += work_stopped && !plan.ride;
		const bool missing = plan.reason == PassengerPeriodicPlan::Reason::OriginMissing || plan.reason == PassengerPeriodicPlan::Reason::DestinationMissing;
		if (missing) {
			if (db.IsBuilding()) { ++diagnostic.missing_building; plan.deferred = true; }
			else ++diagnostic.missing_published;
		}
		if (plan.deferred) ++TickBudget().diagnostic.timed_budget_deferred;
		/* Timed winners always take priority. The two searches share the admitted
		 * work envelope; cache hits replay the same logical work as cold peers. */
		if (!plan.ride && !missing) {
			auto estimated_limits = limits; estimated_limits.work = admission.ScheduleAllowance();
			diagnostic.schedule_allocated += estimated_limits.work;
			auto estimated = db.PlanEstimated(cargo, this->station, packet.GetPassengerDestination(), ready, estimated_limits, ActiveJourneyEvaluation(), excluded);
			admission.Charge(estimated.work); diagnostic.schedule_used += estimated.work;
			if (estimated.ride) {
				estimated.ride->planned_at = _state_ticks; estimated.ride->timed_deferred = plan.deferred;
				plan.ride = estimated.ride;
				diagnostic.schedule_rescued += work_stopped;
			} else plan.deferred |= estimated.deferred;
		}
		diagnostic.work_no_route += work_stopped && !plan.ride;
		/* A full Timed path may be selected while search optimality is unproved.
		 * Preserve the query status in this event cache; only absence of a usable
		 * ride defers the packet. Persistent execution still stores one ordinary
		 * ServiceIntent, with the same live boarding checks and transfer replan. */
		const bool provisional = plan.state == PassengerPeriodicPlan::State::Provisional;
		this->periodic_waiting.emplace(key, PeriodicWaiting{plan.ride, plan.deferred, provisional});
		if (!plan.ride) {
			if (plan.deferred) { CountJourneyDeferral(PassengerJourneyFallbackReason::QueryWork); packet.DeferPassengerJourney(this->station, _state_ticks, fallback_hop); }
			else CountJourneyFallback(GetPassengerServiceDatabase().IsBuilding() ? PassengerJourneyFallbackReason::PredictionIncomplete : PassengerJourneyFallbackReason::NoJourney);
			return false;
		}
		apply(*plan.ride, provisional);
		return true;
	}
	Snapshot *snapshot = nullptr;
	auto reject = [&](PassengerJourneyFallbackReason reason) {
		if (snapshot != nullptr) { snapshot->failure = reason; snapshot->deferred = this->deferred; }
		if (reason == PassengerJourneyFallbackReason::GraphWork || reason == PassengerJourneyFallbackReason::GraphMemory ||
				reason == PassengerJourneyFallbackReason::SnapshotLimit || reason == PassengerJourneyFallbackReason::QueryWork ||
				reason == PassengerJourneyFallbackReason::QueryLimit || reason == PassengerJourneyFallbackReason::QueryMemory) ++TickBudget().diagnostic.timed_budget_deferred;
		if (this->deferred) { CountJourneyDeferral(reason); packet.DeferPassengerJourney(this->station, _state_ticks, fallback_hop); }
		else {
			CountJourneyFallback(reason);
			/* A completed retry may establish a real route failure. Do not let the
			 * previous budget-wait marker turn that into another immediate retry. */
			if (packet.IsPassengerJourneyDeferred()) packet.FallbackPassengerJourney(this->station, _state_ticks, fallback_hop);
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
			packet.SetPassengerJourney(*cached->second, fallback_hop);
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
		packet.SetPassengerJourney(*execution, fallback_hop);
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
	if (!_settings_game.linkgraph.passenger_journey_precomputed && !this->stops.contains(key) && this->stops.size() >= 128) { this->resource_limited = true; return false; }
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

std::span<const StationID> PassengerOdFallbackContext::BoardingStops(const Vehicle *vehicle, CargoType cargo)
{
	if (vehicle == nullptr || cargo >= NUM_CARGO) return {};
	vehicle = vehicle->First();
	if (!vehicle->current_order.IsAnyLoadingType() || vehicle->vehstatus.Any({VehState::Stopped, VehState::Crashed}) ||
			vehicle->current_order.GetCargoLoadType(cargo) == OrderLoadType::NoLoad ||
			vehicle->current_order.GetCargoUnloadType(cargo) == OrderUnloadType::Unload ||
			vehicle->current_order.GetCargoUnloadType(cargo) == OrderUnloadType::Transfer) return {};
	this->CanReach(vehicle, cargo, vehicle->last_station_visited);
	auto found = this->stops.find({vehicle->index, cargo});
	return found == this->stops.end() ? std::span<const StationID>{} : std::span<const StationID>(found->second);
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
		/* All runtime goods assignments invalidate the affected graph's derived
		 * binding revision. Station lifetime changes have a separate generation.
		 * Both counters are NOSAVE and never reset; loaded/reused graph IDs cannot
		 * alias a prior binding. Edge edits need no station rescan. Endpoint checks
		 * above remain live, and the original size debit is paid on every path. */
		const BindingKey binding_key{graph->index, graph->PassengerBindingRevision(), PassengerStationBindingGeneration(), graph->Size(), cargo};
		auto event_binding = this->bindings.find(binding_key);
		if (event_binding == this->bindings.end()) {
			auto resident_binding = resident_fallback_enabled ? resident_bindings.find(binding_key) : resident_bindings.end();
			bool matches = resident_binding != resident_bindings.end() && resident_binding->second.nodes->size() == graph->Size();
			if (matches && validate_resident_bindings) {
				NanosecondTimer validation_timer{TickBudget().diagnostic.od_fallback_binding_validation_nanoseconds};
				size_t checked = 0;
				for (NodeID node = 0; matches && node < graph->Size(); ++node) {
					++checked;
					matches = (*resident_binding->second.nodes)[node] == std::pair((*graph)[node].Station(), valid_node(node));
				}
				TickBudget().diagnostic.od_fallback_binding_nodes += checked;
			}
			if (!matches) {
				NanosecondTimer build_timer{TickBudget().diagnostic.od_fallback_binding_build_nanoseconds};
				++TickBudget().diagnostic.od_fallback_binding_builds;
				if (resident_binding != resident_bindings.end()) { resident_binding_bytes -= resident_binding->second.bytes; resident_bindings.erase(resident_binding); }
				auto nodes = std::make_shared<BindingNodes>(); nodes->reserve(graph->Size());
				for (NodeID node = 0; node < graph->Size(); ++node) nodes->emplace_back((*graph)[node].Station(), valid_node(node));
				size_t binding_bytes = 128 + nodes->size() * sizeof(BindingNodes::value_type) * 2;
				while (!resident_bindings.empty() && (resident_bindings.size() >= 32 || resident_binding_bytes + binding_bytes > 8 * 1024 * 1024)) {
					resident_binding_bytes -= resident_bindings.begin()->second.bytes; resident_bindings.erase(resident_bindings.begin());
				}
				assert(binding_token != UINT64_MAX);
				const uint64_t token = ++binding_token;
				event_binding = this->bindings.emplace(binding_key, Binding{nodes, token}).first;
				if (resident_fallback_enabled) {
					resident_bindings.emplace(binding_key, ResidentBinding{nodes, token, binding_bytes}); resident_binding_bytes += binding_bytes;
				}
			} else {
				++TickBudget().diagnostic.od_fallback_binding_hits;
				event_binding = this->bindings.emplace(binding_key, Binding{resident_binding->second.nodes, resident_binding->second.token}).first;
			}
		}
		const auto &node_bindings = *event_binding->second.nodes;
		DistanceKey resident_key{cargo, destination, graph->index, graph->PassengerOdRevision(), graph->PassengerRoutingRevision(), event_binding->second.token, to.node};
		auto resident = resident_fallback_enabled ? resident_distances.find(resident_key) : resident_distances.end();
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
		auto reverse = resident_fallback_enabled ? reverse_indices.find(reverse_key) : reverse_indices.end();
		std::shared_ptr<const ReverseIndex> index;
		if (reverse != reverse_indices.end()) { index = reverse->second; ++TickBudget().diagnostic.od_fallback_reverse_hits; }
		else {
			NanosecondTimer reverse_timer{TickBudget().diagnostic.od_fallback_reverse_build_nanoseconds};
			++TickBudget().diagnostic.od_fallback_reverse_builds;
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
			index = built;
			if (resident_fallback_enabled) { reverse_indices.emplace(reverse_key, built); reverse_bytes += built->bytes; }
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
		if (resident_fallback_enabled && resident_bytes <= MAX_RESIDENT_BYTES) {
			while (!resident_distances.empty() && (resident_distances.size() >= 512 || resident_distance_bytes + resident_bytes > MAX_RESIDENT_BYTES)) {
				auto oldest = std::min_element(resident_distances.begin(), resident_distances.end(), [](const auto &a, const auto &b) { return a.second.used < b.second.used; });
				resident_distance_bytes -= oldest->second.bytes; resident_distances.erase(oldest);
			}
			resident_distances.emplace(resident_key, ResidentDistances{cached->second.hops, original_work - this->work_left, resident_bytes, ++distance_use}); resident_distance_bytes += resident_bytes;
		}
		TickBudget().diagnostic.od_fallback_cache_bytes = resident_distance_bytes + resident_binding_bytes + reverse_bytes;
	}
	return cached->second.graph == graph->index && cached->second.hops != nullptr && cached->second.hops->size() == graph->Size() ? (*cached->second.hops)[from.node] : UNREACHABLE;
}

StationID PassengerOdFallbackContext::ProgressingVia(const Vehicle *vehicle, CargoType cargo, StationID destination)
{
	if (vehicle == nullptr) return StationID::Invalid();
	if (this->CanReach(vehicle, cargo, destination)) return destination;
	if (_settings_game.linkgraph.passenger_journey_precomputed) {
		const auto &db = GetPassengerServiceDatabase();
		int64_t remaining = db.PrecomputedRouteCost(cargo, vehicle->First()->last_station_visited, destination).value_or(INT64_MAX);
		StationID best = StationID::Invalid();
		for (StationID stop : this->BoardingStops(vehicle, cargo)) {
			auto cost = db.PrecomputedRouteCost(cargo, stop, destination);
			if (cost && *cost < remaining) { remaining = *cost; best = stop; }
		}
		return best;
	}
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

StationID PassengerOdFallbackContext::BoardingVia(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo)
{
	if (vehicle == nullptr) return StationID::Invalid();
	vehicle = vehicle->First();
	const StationID destination = packet.GetPassengerDestination();
	if (destination == vehicle->last_station_visited || !vehicle->current_order.IsAnyLoadingType() ||
			vehicle->vehstatus.Any({VehState::Stopped, VehState::Crashed}) ||
			vehicle->current_order.GetCargoLoadType(cargo) == OrderLoadType::NoLoad ||
			vehicle->current_order.GetCargoUnloadType(cargo) == OrderUnloadType::Unload ||
			vehicle->current_order.GetCargoUnloadType(cargo) == OrderUnloadType::Transfer) return StationID::Invalid();
	/* A live direct ride takes priority over an intermediate CargoDist hop. Both
	 * checks reuse one order prefix and require no LinkGraph reverse exploration. */
	StationID via = destination;
	if (!this->CanReach(vehicle, cargo, destination)) {
		/* Precomputed waiting cohorts are indexed by their saved hop. Flow
		 * changes are routing events, never a hidden per-packet boarding search. */
		via = _settings_game.linkgraph.passenger_journey_precomputed ? packet.GetPassengerLegacyHop() :
				CurrentPassengerLegacyHop(packet, cargo, vehicle->last_station_visited, packet.GetPassengerLegacyHop());
	}
	/* The order prefix is shared across ODs. A full OD result cache cannot veto
	 * this local ride either; retaining a diagnostic entry is optional. */
	if (this->CanReach(vehicle, cargo, via)) {
		const auto key = std::tuple(vehicle->index, cargo, StationID::Invalid(), via);
		if (this->routes.size() < 128 && this->routes.try_emplace(key, via).second) ++TickBudget().diagnostic.untimed_routes;
		return via;
	}
	const auto key = std::tuple(vehicle->index, cargo, destination, StationID::Invalid());
	if (!_settings_game.linkgraph.passenger_journey_precomputed && !this->routes.contains(key) && this->routes.size() >= 128) {
		++TickBudget().diagnostic.untimed_budget_deferred;
		return StationID::Invalid();
	}
	auto [route, inserted] = this->routes.try_emplace(key, StationID::Invalid());
	if (inserted) {
		/* Only a missing or unreachable CargoDist hop needs the OD reverse search. */
		route->second = this->ProgressingVia(vehicle, cargo, destination);
		if (route->second != StationID::Invalid()) ++TickBudget().diagnostic.untimed_routes;
		else if (this->resource_limited) ++TickBudget().diagnostic.untimed_budget_deferred;
		else ++TickBudget().diagnostic.untimed_no_route;
	}
	return route->second;
}

bool PassengerOdFallbackContext::CanBoard(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo)
{
	return this->BoardingVia(packet, vehicle, cargo) != StationID::Invalid();
}

void PassengerOdFallbackContext::PrepareBoard(CargoPacket &packet, const Vehicle *vehicle, CargoType cargo)
{
	packet.SetPassengerVia(this->BoardingVia(packet, vehicle, cargo));
}

void PassengerOdFallbackContext::Invalidate(const Vehicle *vehicle)
{
	VehicleID id = vehicle->First()->index;
	for (auto it = this->stops.lower_bound({id, CargoType{0}}); it != this->stops.end() && it->first.first == id;) it = this->stops.erase(it);
	if (_settings_game.linkgraph.passenger_journey_precomputed) {
		for (auto it = this->routes.lower_bound({id, CargoType{0}, StationID{0}, StationID{0}}); it != this->routes.end() && std::get<0>(it->first) == id;) it = this->routes.erase(it);
	} else {
		this->routes.clear(); this->distances.clear(); this->distance_bytes = 0; this->bindings.clear();
	}
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
		/* Also repair an older CargoDist commitment or a changed live order suffix
		 * when this train can now deliver directly. Forced transfers still win. */
		if (this->CanReach(vehicle, cargo, packet.GetPassengerDestination())) {
			packet.SetPassengerVia(packet.GetPassengerDestination());
			return Action::Keep;
		}
		StationID via = packet.GetPassengerVia();
		/* Keep the validated ride to its interchange; intermediate CargoDist flows
		 * cannot redraw it. Arrival at the via starts a new transfer/routing event. */
		if (via != station) {
			if (this->CanReach(vehicle, cargo, via)) return Action::Keep;
			via = CurrentPassengerLegacyHop(packet, cargo, station, packet.GetPassengerLegacyHop());
			if (!this->CanReach(vehicle, cargo, via)) via = this->ProgressingVia(vehicle, cargo, packet.GetPassengerDestination());
			if (this->CanReach(vehicle, cargo, via)) { packet.SetPassengerVia(via); return Action::Keep; }
		}
	}
	packet.SetPassengerVia(StationID::Invalid());
	packet.SetPassengerLegacyHop(CurrentPassengerLegacyHop(packet, cargo, station, StationID::Invalid()));
	packet.SetNextHop(packet.GetPassengerLegacyHop());
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

bool PassengerJourneyBoardingContext::ValidateServiceRide(const PassengerJourneyExecution &original, bool boarding) const
{
	auto ride = original;
	/* An overdue event may be waiting behind this station's slice limit. It
	 * must not make an expired intent physically boardable in the meantime. */
	if (boarding && _settings_game.linkgraph.passenger_journey_precomputed && IsEstimatedPassengerRide(ride.kind) &&
			_state_ticks > ride.planned_at && uint64_t(_state_ticks.base()) - uint64_t(ride.planned_at.base()) > uint64_t(ride.waiting_limit)) return false;
	if (boarding && _settings_game.linkgraph.passenger_journey_precomputed && !IsBoundPassengerRide(ride.kind) && this->vehicle != nullptr &&
			this->vehicle->orders != nullptr && (ride.service != this->vehicle->orders->index || ride.boarding_order != this->vehicle->cur_real_order_index)) {
		if (!GetPassengerServiceDatabase().RebindPrecomputed(ride, this->vehicle->orders->index, this->cargo, this->vehicle->cur_real_order_index)) return false;
	}
	if (this->vehicle == nullptr || (IsBoundPassengerRide(ride.kind) && ride.vehicle != this->vehicle->index)) return false;
	if (boarding && _state_ticks < ride.not_before) return false;
	if (boarding && !GetPassengerServiceDatabase().IsCurrent(ride)) return false;
	auto key = std::tuple(ride.service, ride.signature, ride.boarding_order, ride.alighting_order, ride.boarding, ride.alighting, boarding);
	auto found = this->service_rides.find(key);
	if (found != this->service_rides.end()) return found->second;
	if (!_settings_game.linkgraph.passenger_journey_precomputed && this->service_rides.size() >= 128) return false;
	bool valid = ValidatePassengerServiceRide(ride, this->vehicle, this->cargo, boarding);
	if (boarding && !valid && this->vehicle->orders != nullptr && this->vehicle->orders->index == ride.service) GetPassengerServiceDatabase().RecordBoardingFailure();
	return this->service_rides.emplace(key, valid).first->second;
}

void PassengerJourneyBoardingContext::ExtendRide(PassengerJourneyExecution &ride, StationID destination) const
{
	if (ride.alighting == destination) return;
	auto &db = GetPassengerServiceDatabase();
	if (this->extension_revision != db.BoardingRevision()) { this->extensions.clear(); this->extension_revision = db.BoardingRevision(); }
	auto [it, inserted] = this->extensions.try_emplace(std::pair{ride, destination});
	if (inserted) {
		auto extended = db.ExtendPrecomputedRide(ride, this->cargo, destination);
		if (extended && ValidatePassengerServiceRide(*extended, this->vehicle, this->cargo, true)) it->second = std::move(extended);
	}
	if (it->second) ride = *it->second;
}

bool PassengerJourneyBoardingContext::CanBoard(const CargoPacket &packet) const
{
	if (_settings_game.linkgraph.passenger_journey_precomputed && this->vehicle != nullptr && this->vehicle->orders != nullptr &&
			packet.GetPassengerArrivingService() == this->vehicle->orders->index && packet.GetPassengerArrivingIncarnation() == this->vehicle->orders->GetCargoDistRouteGeneration()) return false;
	if (packet.GetPassengerRoutingMode() != PassengerRoutingMode::Journey) return true;
	const auto &leg = packet.GetPassengerJourney();
	if (leg.kind == PassengerRideKind::HyperpathIntent) {
		if (!IsPassengerJourneyRoutingEnabled(this->cargo) || !Station::IsValidID(packet.GetPassengerDestination()) || leg.alighting != packet.GetPassengerDestination() ||
				this->vehicle == nullptr || this->vehicle->orders == nullptr || _state_ticks < leg.not_before ||
				IsPassengerScheduleIntentAged(leg, _state_ticks, leg.waiting_limit + 1)) return false;
		auto plan = GetPassengerServiceDatabase().BoardHyperpath(leg, this->cargo, this->vehicle->orders->index,
				this->vehicle->cur_real_order_index, packet.GetPassengerArrivingService(), packet.GetPassengerFailedService(_state_ticks));
		return plan.ride && this->ValidateServiceRide(*plan.ride, true);
	}
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
	if (vehicle == nullptr || !_settings_game.linkgraph.passenger_journey_precomputed) return this->CanBoardUncached(packet, vehicle, cargo);
	if (cargo == INVALID_CARGO) cargo = vehicle->cargo_type;
	if (this->waiting_events && packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey && !GetPassengerServiceDatabase().IsCurrent(packet.GetPassengerJourney()))
		PassengerWaitingRoutingChanged(const_cast<CargoPacket &>(packet));
	BoardingState state{packet.GetPassengerRoutingMode(), packet.GetPassengerDestination(), packet.GetPassengerLegacyHop(),
			packet.GetPassengerInterchangeReady(), packet.GetPassengerArrivingService(), packet.GetPassengerArrivingIncarnation(), IsPassengerJourneyRoutingEnabled(cargo), packet.GetPassengerBoardingFailure()};
	auto key = std::pair(state, packet.GetPassengerJourney());
	auto &decisions = this->decisions[{vehicle->First()->index, cargo}];
	auto revision = GetPassengerServiceDatabase().BoardingRevision();
	auto found = decisions.find(key);
	if (found != decisions.end() && found->second.tick == _state_ticks && found->second.revision == revision) {
		++this->cache_hits; return found->second.allowed;
	}
	++this->validations;
	bool allowed = this->CanBoardUncached(packet, vehicle, cargo);
	decisions.insert_or_assign(key, Decision{_state_ticks, revision, allowed});
	return allowed;
}

bool PassengerJourneyLoadingContext::CanBoardUncached(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo)
{
	if (vehicle == nullptr) return packet.GetPassengerRoutingMode() != PassengerRoutingMode::Journey;
	if (cargo == INVALID_CARGO) cargo = vehicle->cargo_type;
	if (this->waiting_events && _settings_game.linkgraph.passenger_journey_precomputed &&
			packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey && !GetPassengerServiceDatabase().IsCurrent(packet.GetPassengerJourney())) {
		PassengerWaitingRoutingChanged(const_cast<CargoPacket &>(packet));
	}
	if (IsPassengerJourneyRoutingEnabled(cargo) && Station::IsValidID(packet.GetPassengerDestination()) &&
			packet.GetPassengerArrivingService() != OrderListID::Invalid() && (_state_ticks < packet.GetPassengerInterchangeReady() ||
			(vehicle->First()->orders != nullptr && vehicle->First()->orders->index == packet.GetPassengerArrivingService() && vehicle->First()->orders->GetCargoDistRouteGeneration() == packet.GetPassengerArrivingIncarnation()))) return false;
	const bool stale = _settings_game.linkgraph.passenger_journey_precomputed && packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey &&
			!GetPassengerServiceDatabase().IsCurrent(packet.GetPassengerJourney());
	if (packet.GetPassengerRoutingMode() != PassengerRoutingMode::Journey || stale) {
		if (_settings_game.linkgraph.passenger_journey_precomputed && IsPassengerJourneyRoutingEnabled(cargo) && Station::IsValidID(packet.GetPassengerDestination())) {
			const auto &plan = this->PendingPlan(packet, vehicle, cargo);
			if (plan.ride) {
				auto ride = plan.ride;
				if (ride->kind == PassengerRideKind::HyperpathIntent) ride = this->ResolveHyperpath(*ride, packet, vehicle, cargo);
				return ride && this->boarding.try_emplace({vehicle->First()->index, cargo}, vehicle, cargo).first->second.CanBoardIntent(*ride);
			}
		}
		if (stale) return false;
		return !IsPassengerJourneyRoutingEnabled(cargo) || !Station::IsValidID(packet.GetPassengerDestination()) || this->fallback.CanBoard(packet, vehicle, cargo);
	}
	if (packet.GetPassengerJourney().kind == PassengerRideKind::HyperpathIntent) {
		const auto &intent = packet.GetPassengerJourney();
		if (!IsPassengerJourneyRoutingEnabled(cargo) || !Station::IsValidID(packet.GetPassengerDestination()) || intent.alighting != packet.GetPassengerDestination()) return false;
		if (_state_ticks < intent.not_before || IsPassengerScheduleIntentAged(intent, _state_ticks, intent.waiting_limit + 1)) return false;
		auto ride = this->ResolveHyperpath(intent, packet, vehicle, cargo);
		return ride && this->boarding.try_emplace({vehicle->First()->index, cargo}, vehicle, cargo).first->second.CanBoardIntent(*ride);
	}
	if (!IsPassengerRideIntent(packet.GetPassengerJourney().kind) && packet.GetPassengerJourney().vehicle != vehicle->First()->index) return false;
	auto key = std::pair(vehicle->First()->index, cargo);
	if (!_settings_game.linkgraph.passenger_journey_precomputed && !this->boarding.contains(key) && this->boarding.size() >= 128) return false;
	return this->boarding.try_emplace(key, vehicle, cargo).first->second.CanBoard(packet);
}

std::optional<PassengerJourneyExecution> PassengerJourneyLoadingContext::ResolveHyperpath(const PassengerJourneyExecution &intent,
		const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo, bool override_failure, OrderListID failed)
{
	const auto *front = vehicle == nullptr ? nullptr : vehicle->First();
	if (front == nullptr || front->orders == nullptr || intent.boarding != front->last_station_visited) return {};
	auto &db = GetPassengerServiceDatabase();
	if (this->pending_revision != db.BoardingRevision()) {
		this->pending_plans.clear(); this->hyperpath_rides.clear(); this->hyperpath_boardings.clear(); this->hyperpath_memberships.clear(); this->pending_revision = db.BoardingRevision();
	}
	const auto *arriving = OrderList::GetIfValid(packet.GetPassengerArrivingService());
	OrderListID previous = arriving != nullptr && arriving->GetCargoDistRouteGeneration() == packet.GetPassengerArrivingIncarnation() ? arriving->index : OrderListID::Invalid();
	if (!override_failure) failed = packet.GetPassengerFailedService(_state_ticks);
	auto key = HyperpathKey{front->index, cargo, front->cur_real_order_index, intent.generation, intent.boarding, intent.alighting, previous, failed};
	auto [found, inserted] = this->hyperpath_rides.try_emplace(key);
	if (inserted) found->second = db.BoardHyperpath(intent, cargo, front->orders->index, front->cur_real_order_index, previous, failed);
	auto ride = found->second.ride;
	if (ride) { ride->not_before = intent.not_before; ride->planned_at = intent.planned_at; ride->estimated_wait = intent.estimated_wait; ride->waiting_limit = intent.waiting_limit; }
	return ride;
}

const std::array<uint32_t, 2> &PassengerJourneyLoadingContext::HyperpathBoardingOccurrences(const Vehicle *vehicle, CargoType cargo)
{
	auto &db = GetPassengerServiceDatabase();
	if (this->pending_revision != db.BoardingRevision()) {
		this->pending_plans.clear(); this->hyperpath_rides.clear(); this->hyperpath_boardings.clear(); this->hyperpath_memberships.clear(); this->pending_revision = db.BoardingRevision();
	}
	const auto *front = vehicle->First();
	auto key = HyperpathBoardingKey{front->index, cargo, front->orders->index, front->cur_real_order_index, front->last_station_visited};
	auto [found, inserted] = this->hyperpath_boardings.try_emplace(key);
	if (inserted) found->second = db.HyperpathBoardingOccurrences(cargo, front->last_station_visited, front->orders->index, front->cur_real_order_index);
	return found->second;
}

bool PassengerJourneyLoadingContext::MayUseHyperpathPolicy(const CargoPacket &representative, const Vehicle *vehicle, CargoType cargo,
		const std::array<uint32_t, 2> &occurrences, bool base, bool adjusted)
{
	if (vehicle == nullptr || vehicle->First()->orders == nullptr || !IsPassengerJourneyRoutingEnabled(cargo) ||
			vehicle->First()->current_order.GetCargoLoadType(cargo) == OrderLoadType::NoLoad) return false;
	const auto &intent = representative.GetPassengerJourney();
	if (intent.kind != PassengerRideKind::HyperpathIntent) return false;
	const auto &failure = representative.GetPassengerBoardingFailure();
	const auto *failed_list = OrderList::GetIfValid(failure.service);
	if (failed_list == nullptr || failed_list->GetCargoDistRouteGeneration() != failure.incarnation) { base = true; adjusted = false; }
	auto &db = GetPassengerServiceDatabase(); auto current_occurrences = occurrences;
	if (this->pending_revision != db.BoardingRevision()) this->HyperpathBoardingOccurrences(vehicle, cargo);
	const auto *arriving = OrderList::GetIfValid(representative.GetPassengerArrivingService());
	auto previous = arriving != nullptr && arriving->GetCargoDistRouteGeneration() == representative.GetPassengerArrivingIncarnation() ? arriving->index : OrderListID::Invalid();
	auto key = [&](const PassengerJourneyExecution &policy) {
		return HyperpathMembershipKey{HyperpathKey{vehicle->First()->index, cargo, vehicle->First()->cur_real_order_index,
				policy.generation, policy.boarding, policy.alighting, previous, OrderListID::Invalid()}, vehicle->First()->orders->index, policy.signature, current_occurrences};
	};
	/* Without an arriving carrier the table answers base membership exactly from two small
	 * bit arrays; a memo lookup with this key would cost more than the answer. */
	const bool direct = previous == OrderListID::Invalid();
	auto cached = direct ? this->hyperpath_memberships.end() : this->hyperpath_memberships.find(key(intent));
	const bool current = cached != this->hyperpath_memberships.end() || db.IsCurrent(intent);
	if (current && intent.boarding != vehicle->First()->last_station_visited) return false;
	auto inspect = [&](const PassengerJourneyExecution &policy, auto entry) {
		if (policy.boarding != vehicle->First()->last_station_visited) return false;
		if (direct) {
			return (base && db.MayBoardHyperpath(policy, cargo, current_occurrences, previous, OrderListID::Invalid())) ||
					(adjusted && db.MayBoardHyperpath(policy, cargo, current_occurrences, previous, failure.service));
		}
		if (entry == this->hyperpath_memberships.end()) {
			auto [found, inserted] = this->hyperpath_memberships.try_emplace(key(policy)); entry = found;
			if (inserted) entry->second = db.MayBoardHyperpath(policy, cargo, current_occurrences, previous, OrderListID::Invalid());
		}
		/* Base membership is exact and immutable for this frozen revision. Keep
		 * failed policies on the table's cache so a cold unknown is never latched.
		 * Membership entries contain neither a concrete ride nor live legality. */
		return (base && entry->second) || (adjusted && db.MayBoardHyperpath(policy, cargo, current_occurrences, previous, failure.service));
	};
	if (current) return inspect(intent, cached);
	auto plan = db.PlanPrecomputed(cargo, intent.boarding, intent.alighting, _state_ticks, previous);
	if (!plan.ride) return false;
	if (plan.ride->kind != PassengerRideKind::HyperpathIntent) return true;
	return inspect(*plan.ride, this->hyperpath_memberships.end());
}

int64_t PassengerJourneyLoadingContext::ExpectedHyperpathWait(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo)
{
	if (vehicle == nullptr || vehicle->First()->orders == nullptr) return 0;
	auto &db = GetPassengerServiceDatabase(); auto intent = packet.GetPassengerJourney();
	if (!db.IsCurrent(intent)) {
		const auto &plan = this->PendingPlan(packet, vehicle, cargo);
		if (!plan.ride) return 0;
		intent = *plan.ride;
	}
	return db.HyperpathServiceWait(intent, cargo, vehicle->First()->orders->index).value_or(0);
}

bool PassengerJourneyLoadingContext::CanUseHyperpathPolicy(const CargoPacket &representative, const Vehicle *vehicle, CargoType cargo, bool base, bool adjusted)
{
	if (vehicle == nullptr || !IsPassengerJourneyRoutingEnabled(cargo) ||
			vehicle->First()->current_order.GetCargoLoadType(cargo) == OrderLoadType::NoLoad) return false;
	auto intent = representative.GetPassengerJourney();
	if (intent.kind != PassengerRideKind::HyperpathIntent) return false;
	const auto &failure = representative.GetPassengerBoardingFailure();
	const auto *failed_list = OrderList::GetIfValid(failure.service);
	if (failed_list == nullptr || failed_list->GetCargoDistRouteGeneration() != failure.incarnation) { base = true; adjusted = false; }
	auto &db = GetPassengerServiceDatabase();
	if (!db.IsCurrent(intent)) {
		const auto *arriving = OrderList::GetIfValid(representative.GetPassengerArrivingService());
		auto previous = arriving != nullptr && arriving->GetCargoDistRouteGeneration() == representative.GetPassengerArrivingIncarnation() ? arriving->index : OrderListID::Invalid();
		auto plan = db.PlanPrecomputed(cargo, intent.boarding, intent.alighting, _state_ticks, previous);
		if (!plan.ride) return false;
		intent = *plan.ride;
		if (intent.kind != PassengerRideKind::HyperpathIntent) return true; // A retained legacy row is validated per packet.
	}
	/* This is a policy predicate, deliberately independent of the first
	 * packet's timestamps. Readiness is indexed separately; FIFO loading checks
	 * each selected packet's expiry. An expired representative must not hide a
	 * still-valid passenger in this shared queue. Expiry extrema choose the
	 * union of base/adjusted policies only when both actually occur here. */
	if (base && this->ResolveHyperpath(intent, representative, vehicle, cargo, true).has_value()) return true;
	return adjusted && this->ResolveHyperpath(intent, representative, vehicle, cargo, true, failure.service).has_value();
}

const PassengerPrecomputedPlan &PassengerJourneyLoadingContext::PendingPlan(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo)
{
	auto &db = GetPassengerServiceDatabase();
	const auto revision = db.BoardingRevision();
	if (this->pending_revision != revision) { this->pending_plans.clear(); this->hyperpath_rides.clear(); this->hyperpath_boardings.clear(); this->hyperpath_memberships.clear(); this->pending_revision = revision; }
	const auto *arriving = OrderList::GetIfValid(packet.GetPassengerArrivingService());
	const auto excluded = arriving != nullptr && arriving->GetCargoDistRouteGeneration() == packet.GetPassengerArrivingIncarnation() ? arriving->index : OrderListID::Invalid();
	const auto station = vehicle->First()->last_station_visited;
	const auto ready = std::max(_state_ticks, packet.GetPassengerInterchangeReady());
	auto failed = packet.GetPassengerFailedService(_state_ticks);
	auto [it, inserted] = this->pending_plans.try_emplace(PendingKey{cargo, station, packet.GetPassengerDestination(), ready, excluded, failed});
	if (inserted) {
		RecordPassengerWaitingGroup();
		uint32_t sample = static_cast<uint32_t>(SimpleHash64((uint64_t{excluded.base()} << 32) ^
				(uint64_t{station.base()} << 16) ^ packet.GetPassengerDestination().base() ^ uint64_t(ready.base())));
		it->second = db.PlanPrecomputed(cargo, station, packet.GetPassengerDestination(), ready, excluded, sample, failed);
	}
	return it->second;
}

void PassengerJourneyLoadingContext::ResolveWaiting(CargoPacket &packet, const Vehicle *vehicle)
{
	if (vehicle == nullptr || !_settings_game.linkgraph.passenger_journey_precomputed ||
			(packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey && GetPassengerServiceDatabase().IsCurrent(packet.GetPassengerJourney())) ||
			!IsPassengerJourneyRoutingEnabled(vehicle->cargo_type) || !Station::IsValidID(packet.GetPassengerDestination())) return;
	const auto &plan = this->PendingPlan(packet, vehicle, vehicle->cargo_type);
	if (plan.ride) {
		RecordPassengerWaitingEvent(packet.GetPassengerRoutingEvent());
		packet.SetPassengerJourney(*plan.ride, packet.GetPassengerLegacyHop());
	}
}

void PassengerJourneyLoadingContext::ObserveWaitingGroup(const CargoPacket &packet, const Vehicle *vehicle, CargoType cargo, size_t packets)
{
	if (!this->waiting_events || vehicle == nullptr ||
			(packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey && GetPassengerServiceDatabase().IsCurrent(packet.GetPassengerJourney())) ||
			!IsPassengerJourneyRoutingEnabled(cargo) || !Station::IsValidID(packet.GetPassengerDestination())) return;
	const auto &plan = this->PendingPlan(packet, vehicle, cargo);
	/* This is an upper estimate for rejected, unresolved cohorts, not proof of
	 * a lost seat. Capacity and restrictions are still checked by real loading. */
	bool missed = plan.state == PassengerPrecomputedPlan::State::Building && !this->CanBoard(packet, vehicle, cargo);
	RecordPassengerWaitingArrival(packets, uint64_t{packet.Count()} * packets, missed);
}

void PassengerJourneyLoadingContext::PrepareBoard(CargoPacket &packet, const Vehicle *vehicle)
{
	if (vehicle != nullptr) {
		packet.SetPassengerTransport(packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey ?
				(IsEstimatedPassengerRide(packet.GetPassengerJourney().kind) ? PassengerTransportKind::Schedule : PassengerTransportKind::Timed) :
				!IsPassengerJourneyRoutingEnabled(vehicle->cargo_type) || !Station::IsValidID(packet.GetPassengerDestination()) ? PassengerTransportKind::None : PassengerTransportKind::Untimed);
	}
	if (vehicle != nullptr && packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey &&
			IsPassengerRideIntent(packet.GetPassengerJourney().kind)) {
		auto ride = packet.GetPassengerJourney();
		bool hyperpath = ride.kind == PassengerRideKind::HyperpathIntent;
		if (hyperpath) {
			auto resolved = this->ResolveHyperpath(ride, packet, vehicle, vehicle->cargo_type);
			assert(resolved.has_value()); if (!resolved) return;
			ride = *resolved;
		}
		if (_settings_game.linkgraph.passenger_journey_precomputed && vehicle->First()->orders != nullptr &&
				(ride.service != vehicle->First()->orders->index || ride.boarding_order != vehicle->First()->cur_real_order_index))
			GetPassengerServiceDatabase().RebindPrecomputed(ride, vehicle->First()->orders->index, vehicle->cargo_type, vehicle->First()->cur_real_order_index);
		if (_settings_game.linkgraph.passenger_journey_precomputed && !hyperpath)
			this->boarding.try_emplace({vehicle->First()->index, vehicle->cargo_type}, vehicle, vehicle->cargo_type).first->second.ExtendRide(ride, packet.GetPassengerDestination());
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
	for (auto it = this->decisions.lower_bound({id, CargoType{0}}); it != this->decisions.end() && it->first.first == id;) it = this->decisions.erase(it);
	std::erase_if(this->hyperpath_boardings, [id](const auto &item) { return std::get<0>(item.first) == id; });
	std::erase_if(this->hyperpath_memberships, [id](const auto &item) { return std::get<0>(std::get<0>(item.first)) == id; });
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
		if (!this->PlanWaiting(packet, cargo, StationID::Invalid())) packet.SetNextHop(packet.GetPassengerLegacyHop());
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
	if (this->passenger_index_cargo != cargo) { this->ClearWaitingIndex(); this->passenger_index_cargo = cargo; }
	const bool precomputed = _settings_game.linkgraph.passenger_journey_precomputed;
	const auto revision = GetPassengerServiceDatabase().GetPublishedRevision();
	const auto rows = GetPassengerServiceDatabase().PrecomputedDestinations(cargo); // Per station call: never copy the diagnostic histogram.
	const bool events = precomputed && allow_search;
	if (events) {
		this->EnsureWaitingIndex();
		if (this->passenger_routing_tick != _state_ticks) {
			this->passenger_routing_tick = _state_ticks;
			this->passenger_routing_work = 0;
		}
		/* Do not restart an in-progress sweep on another publication. Finish its
		 * saved cursor first, then coalesce newer generations into the next one.
		 * A publication alone is bounded per list: a busy map publishes almost every tick. */
		const int64_t revision_interval = GetPassengerServiceDatabase().RevisionAffectsWaitingPlans() ? PASSENGER_REVISION_SWEEP_TICKS : PASSENGER_TABLE_REVISION_SWEEP_TICKS;
		const bool revision_due = this->passenger_routing_revision != revision && (this->passenger_routing_swept == INVALID_STATE_TICKS ||
				this->passenger_routing_swept > _state_ticks || _state_ticks.base() - this->passenger_routing_swept.base() >= revision_interval);
		if (!this->passenger_routing_sweeping && (this->passenger_routing_dirty || revision_due || this->passenger_routing_rows != rows)) {
			this->passenger_routing_swept = _state_ticks;
			this->passenger_routing_sweeping = true;
			this->passenger_routing_cursor = 0;
			this->passenger_routing_end = this->routing_packets.empty() ? 0 : this->routing_packets.rbegin()->base();
			if (_settings_game.linkgraph.passenger_journey_hyperpath && !this->legacy_journey_packets.empty()) this->passenger_routing_end = std::max(this->passenger_routing_end, this->legacy_journey_packets.rbegin()->base());
			this->passenger_routing_dirty = false;
			this->passenger_routing_revision = revision;
			this->passenger_routing_rows = rows;
		}
		if (this->passenger_routing_work >= PASSENGER_ROUTING_SLICE ||
				(!this->passenger_routing_sweeping && (this->routing_events.empty() || this->routing_events.begin()->first > _state_ticks))) {
			this->UpdatePassengerRoutingSchedule();
			return 0;
		}
	} else {
		this->ClearWaitingIndex();
	}
	CounterTimer category{TickBudget().diagnostic.waiting_microseconds};
	PassengerJourneyActiveContext context(nullptr, station);
	PassengerJourneyLoadingContext local_loading;
	if (loading == nullptr) loading = &local_loading;
	const bool search = IsJourneyRetryTick(station);
	std::map<OrderListID, uint64_t> signatures;
	std::map<std::tuple<VehicleID, VehicleOrderID, VehicleOrderID, StationID>, bool> usable;
	std::map<std::tuple<OrderListID, uint64_t, VehicleOrderID, VehicleOrderID, StationID>, bool> periodic_available;
	size_t validation_work = 524288;
	size_t inspected = 0;
	bool changed = false;
	auto route = [&](CargoPacket *packet, StationID original_key) {
		if (allow_search && precomputed && _settings_game.linkgraph.passenger_journey_hyperpath && packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey && packet->GetPassengerJourney().kind != PassengerRideKind::HyperpathIntent) {
			context.PlanWaiting(*packet, cargo, packet->GetPassengerLegacyHop(), true);
		}
		if (packet->GetPassengerBoardingFailure().Present() && packet->GetPassengerBoardingFailure().Expired(_state_ticks, packet->GetPassengerJourney().waiting_limit)) packet->SetPassengerBoardingFailure({});
		if (allow_search && packet->GetPassengerJourney().kind == PassengerRideKind::HyperpathIntent && packet->GetPassengerFailedService(_state_ticks) != OrderListID::Invalid()) {
			context.PlanWaiting(*packet, cargo, packet->GetPassengerLegacyHop());
		}
		StationID key = original_key;
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
		if (IsPassengerJourneyRoutingEnabled(cargo) && allow_search &&
				(precomputed ? packet->GetPassengerRoutingMode() != PassengerRoutingMode::Journey : CanSearchWaitingJourney(*packet, station, search)) &&
				(precomputed || inspected < 4096)) {
			++inspected;
			if (context.PlanWaiting(*packet, cargo, key)) key = StationID::Invalid();
			else if (!packet->IsPassengerJourneyDeferred()) packet->FallbackPassengerJourney(station, _state_ticks);
		}
		if (IsPassengerJourneyRoutingEnabled(cargo) && Station::IsValidID(packet->GetPassengerDestination()) &&
				packet->GetPassengerRoutingMode() != PassengerRoutingMode::Journey) {
			const StationID hop = CurrentPassengerLegacyHop(*packet, cargo, station, key);
			changed |= hop != packet->GetPassengerLegacyHop();
			packet->SetPassengerLegacyHop(hop);
			key = StationID::Invalid(); // CanBoard validates the through ride, including reservations and refits.
		}
		changed |= original_key != key || previous_mode != packet->GetPassengerRoutingMode();
		return key;
	};
	/* Deadlines are simulation state. Rebuilding the derived index must never
	 * create new work, discard pending work or reset the per-tick allowance. */
	auto schedule = [&](CargoPacket &packet) {
		StateTicks due = INVALID_STATE_TICKS;
		if (packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey) {
			const auto &ride = packet.GetPassengerJourney();
			const int64_t base = IsEstimatedPassengerRide(ride.kind) ? ride.planned_at.base() : ride.departure.base();
			const int64_t wait = IsEstimatedPassengerRide(ride.kind) ? ride.waiting_limit : 0;
			if (wait >= 0 && base < INT64_MAX - wait) due = std::max(_state_ticks + 1, StateTicks{base + wait + 1});
		} else if (packet.IsPassengerJourneyDeferred()) {
			due = _state_ticks + 1;
		}
		if (packet.GetPassengerInterchangeReady() > _state_ticks) due = due == INVALID_STATE_TICKS ? packet.GetPassengerInterchangeReady() : std::min(due, packet.GetPassengerInterchangeReady());
		const auto &failure = packet.GetPassengerBoardingFailure();
		if (failure.Present() && failure.since.base() <= INT64_MAX - failure.Hold(packet.GetPassengerJourney().waiting_limit)) {
			StateTicks expiry{failure.since.base() + failure.Hold(packet.GetPassengerJourney().waiting_limit)};
			due = due == INVALID_STATE_TICKS ? expiry : std::min(due, expiry);
		}
		packet.SchedulePassengerRouting(due);
	};
	size_t moved = 0;
	StationCargoPacketMap::Map &queues = this->packets;
	if (events) {
		/* Alternate generations and timed/new work. Each station has its own
		 * allowance; busy low-ID stations cannot consume another station's slice.
		 * Due time precedes pool ID, so a stream of new packets cannot starve old
		 * events. Both cursors survive save/load and repeated calls this tick. */
		while (this->passenger_routing_work < PASSENGER_ROUTING_SLICE) {
			const bool due = !this->routing_events.empty() && this->routing_events.begin()->first <= _state_ticks;
			bool sweep = this->passenger_routing_sweeping && (!due || (this->passenger_routing_work % 2 == 0));
			CargoPacketID id;
			if (sweep) {
				id = this->NextPassengerGenerationPacket(this->passenger_routing_cursor);
				if (id == CargoPacketID::Invalid() || id.base() > this->passenger_routing_end) { this->passenger_routing_sweeping = false; continue; }
			} else if (due) {
				id = this->routing_events.begin()->second;
			} else break;
			if (!ConsumePassengerRoutingWork()) break;
			if (sweep) {
				this->passenger_routing_cursor = id.base() + 1;
				++this->passenger_generation_inspected;
			}
			++this->passenger_routing_work;
			CargoPacket *packet = CargoPacket::Get(id);
			const auto cause = packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey && packet->GetPassengerJourney().kind != PassengerRideKind::HyperpathIntent && _settings_game.linkgraph.passenger_journey_hyperpath ? PassengerWaitingCause::Migration :
					packet->GetPassengerBoardingFailure().Present() ? PassengerWaitingCause::Failure : sweep ? PassengerWaitingCause::Generation :
					packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey ? PassengerWaitingCause::Expiry : PassengerWaitingCause::New;
			const auto cause_started = std::chrono::steady_clock::now();
			RecordPassengerWaitingEvent(sweep ? this->passenger_routing_due : packet->GetPassengerRoutingEvent());
			/* Existing Hyperpath intents use lazy validation and saved deadlines.
			 * Legacy intents join the budgeted generation sweep only in B mode. */
			++this->passenger_routing_inspected;
			const auto entry = this->waiting_entries.at(id);
			/* A frozen route can exist while its live service is unavailable. Use
			 * the same cached validation as PlanWaiting, rather than testing only
			 * reachability. An unchanged fallback needs its original retry/merge
			 * metadata update, but no boarding-index rebuild. Preserve the saved
			 * cursor/debit and every native field the full route/schedule pass
			 * would produce. Own events, changed hops and memory fallback still
			 * take that ordinary path. */
			StateTicks fallback_due = packet->GetPassengerInterchangeReady() > _state_ticks ? packet->GetPassengerInterchangeReady() : INVALID_STATE_TICKS;
			if (_settings_game.linkgraph.passenger_journey_hyperpath && sweep && IsPassengerJourneyRoutingEnabled(cargo) &&
					packet->GetPassengerRoutingMode() == PassengerRoutingMode::Fallback && entry.key == StationID::Invalid() &&
					!packet->GetPassengerBoardingFailure().Present() &&
					packet->GetPassengerRoutingEvent() == fallback_due &&
					CurrentPassengerLegacyHop(*packet, cargo, station, packet->GetPassengerLegacyHop()) == packet->GetPassengerLegacyHop()) {
				const auto &plan = context.QueryWaiting(*packet, cargo);
				if (!plan.ride && plan.state != PassengerPrecomputedPlan::State::Memory) {
					packet->passenger_journey = {}; packet->passenger_via = StationID::Invalid();
					packet->passenger_retry_station = station; packet->passenger_retry_after = _state_ticks + 256;
					this->RefreshWaitingPacket(*packet);
					RecordPassengerWaitingCause(cause, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - cause_started).count(), true);
					continue;
				}
			}
			/* Index once after routing. The entry still describes the old state until then, so a
			 * moved packet is forgotten from exactly the indices it was in. */
			SetPassengerWaitingRefreshDeferred(id);
			const StationID key = route(packet, entry.key);
			schedule(*packet);
			SetPassengerWaitingRefreshDeferred(CargoPacketID::Invalid());
			if (key == entry.key) {
				this->RefreshWaitingPacket(*packet);
			} else {
				this->ForgetWaitingPacket(id);
				auto bucket = queues.find(entry.key);
				auto &list = queues[key];
				list.splice(list.end(), bucket->second, entry.position);
				if (bucket->second.empty()) queues.erase(bucket);
				this->IndexPacket(packet, key, entry.position);
				++moved;
			}
			RecordPassengerWaitingCause(cause, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - cause_started).count());
		}
	} else {
		std::vector<std::pair<StationID, CargoPacket *>> relocated;
		for (auto bucket = queues.begin(); bucket != queues.end();) {
			auto &list = bucket->second;
			for (auto it = list.begin(); it != list.end();) {
				auto position = it++;
				CargoPacket *packet = *position;
				const StationID key = route(packet, bucket->first);
				schedule(*packet);
				if (key != bucket->first) { relocated.emplace_back(key, packet); list.erase(position); }
			}
			if (list.empty()) bucket = queues.erase(bucket); else ++bucket;
		}
		for (const auto &[key, packet] : relocated) this->packets.Insert(key, packet);
		moved = relocated.size();
		this->passenger_routing_dirty = false;
		this->passenger_routing_revision = revision; this->passenger_routing_rows = rows;
		if (precomputed) this->RebuildWaitingIndex();
	}
	this->UpdatePassengerRoutingSchedule();
	if (changed) SetWindowDirty(WindowClass::StationView, station);
	return moved;
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
	if (!_settings_game.linkgraph.passenger_journey_precomputed) GetPassengerServiceDatabase().DiscardPrecomputed();
	InvalidatePassengerServiceDatabase();
	InvalidatePassengerWaitingRouting(); // A mode change re-routes every waiting list at once.
	/* Reserved cargo lives on vehicles. Return it before changing queue keys or mode. */
	for (Station *station : Station::Iterate()) {
		for (Vehicle *front : station->loading_vehicles) {
			for (Vehicle *v = front; v != nullptr; v = v->Next()) {
				if (!IsCargoInClass(v->cargo_type, CargoClass::Passengers) || IsPassengerJourneyRoutingEnabled(v->cargo_type)) continue;
				v->cargo.ReturnReserved(&station->goods[v->cargo_type].CreateData().cargo, StationID::Invalid(), v->GetCargoTile(),
						[](const CargoPacket &packet) { return packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey; }, v->cargo_type);
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
	/* Use the existing cooperative replay/barrier in singleplayer too, so
	 * initial restoration can display progress and yield to video/input. */
	GetPassengerServiceDatabase().FinishLoad(SlXvIsFeaturePresent(XSLFI_PASSENGER_PERIODIC), _networking ||
			(_settings_game.linkgraph.passenger_journey_routing && _settings_game.linkgraph.passenger_journey_precomputed));
	if (SlXvIsFeatureMissing(XSLFI_PASSENGER_PERIODIC)) _settings_game.linkgraph.passenger_journey_precomputed = false;
	if (SlXvIsFeatureMissing(XSLFI_PASSENGER_PERIODIC)) _settings_game.linkgraph.passenger_journey_periodic = true;
	InvalidatePassengerServiceIndex();
	BeginPassengerJourneyTick();
	/* Saveload writes packet fields directly; reconstruct the derived index before
	 * validation changes modes or vehicles can be deleted/reuse their pool IDs. */
	RebuildPassengerJourneyVehicleIndex();
	for (Station *station : Station::Iterate()) for (CargoType cargo : EnumRange(NUM_CARGO)) {
		if (station->goods[cargo].data != nullptr) {
			/* Upstream legacy loaders can move a spare GoodsEntryData directly. */
			station->goods[cargo].data->cargo.BindPassengerStation(station->index, cargo);
			if (_settings_game.linkgraph.passenger_journey_precomputed) station->goods[cargo].data->cargo.RebuildWaitingIndex();
		}
	}
	RebuildPassengerWaitingScheduler();
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
				[&](const CargoPacket &packet) { return invalidated.contains(packet.index); }, v->cargo_type);
	}
	/* Preserve saved waiting queue keys and packet order. Reclassifying here
	 * changes station rating/truncation RNG on a joining client. Ordinary
	 * routing and boarding events validate and rekey packets on all peers. */
	Debug(sl, 2, "Passenger journey afterload: feature {}, enabled {}", _sl_xv_feature_versions[XSLFI_PASSENGER_JOURNEY_ROUTING],
			_settings_game.linkgraph.passenger_journey_routing);
}
