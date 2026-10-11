/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_journey_shadow.cpp Bounded local diagnostics, excluded from synchronized and saved state. */
#include "stdafx.h"
#include "passenger_journey_shadow.h"
#include "vehicle_base.h"
#include "cargotype.h"
#include "settings_type.h"
#include "date_func.h"
#include <chrono>
#include "safeguards.h"

static bool _passenger_shadow_enabled = false;
static PassengerJourneyShadowStats _passenger_shadow_stats;
static std::vector<PassengerJourneyShadowDetail> _passenger_shadow_details;
static int64_t _passenger_shadow_epoch = -1;
static size_t _passenger_shadow_builds, _passenger_shadow_queries, _passenger_shadow_observations, _passenger_shadow_work;

using ShadowClock = std::chrono::steady_clock;
static uint64_t ShadowMicros(ShadowClock::time_point start)
{
	return std::chrono::duration_cast<std::chrono::microseconds>(ShadowClock::now() - start).count();
}

static uint64_t ShadowNanos(ShadowClock::time_point start)
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(ShadowClock::now() - start).count();
}

bool IsPassengerJourneyShadowEnabled() { return _passenger_shadow_enabled; }
void SetPassengerJourneyShadowEnabled(bool enabled) { _passenger_shadow_enabled = enabled; }
void ResetPassengerJourneyShadowStats()
{
	_passenger_shadow_stats = {};
	_passenger_shadow_details.clear();
	/* Quotas survive a stats reset: repeatedly resetting cannot evade the CPU cap. */
}
const PassengerJourneyShadowStats &GetPassengerJourneyShadowStats() { return _passenger_shadow_stats; }
std::span<const PassengerJourneyShadowDetail> GetPassengerJourneyShadowDetails() { return _passenger_shadow_details; }

const char *PassengerShadowCategoryName(PassengerShadowCategory category)
{
	static constexpr std::array names{ "both_stay", "both_transfer", "legacy_transfer_planner_stay", "legacy_stay_planner_transfer",
			"arrival_mismatch", "both_arrived", "unknown", "unreachable_in_horizon", "constraints", "skipped" };
	return names[to_underlying(category)];
}

PassengerShadowCategory ComparePassengerShadowDecision(VehicleCargoList::MoveToAction legacy, PassengerShadowDecision planner, bool constrained)
{
	if (constrained) return PassengerShadowCategory::Constraints;
	if (planner == PassengerShadowDecision::Unknown) return PassengerShadowCategory::Unknown;
	if (planner == PassengerShadowDecision::Unreachable) return PassengerShadowCategory::Unreachable;
	bool delivered = legacy == VehicleCargoList::MoveToAction::Deliver;
	if (delivered != (planner == PassengerShadowDecision::Arrived)) return PassengerShadowCategory::ArrivalMismatch;
	if (delivered) return PassengerShadowCategory::BothArrived;
	if (legacy == VehicleCargoList::MoveToAction::Keep) return planner == PassengerShadowDecision::Stay ?
			PassengerShadowCategory::BothStay : PassengerShadowCategory::LegacyStayPlannerTransfer;
	return planner == PassengerShadowDecision::Transfer ? PassengerShadowCategory::BothTransfer : PassengerShadowCategory::LegacyTransferPlannerStay;
}

PassengerJourneyShadowContext::Snapshot::Snapshot(PassengerServiceGraph graph, const Vehicle *vehicle, StationID station) :
		graph(std::move(graph)), planner(this->graph, nullptr, {32, 1024 * 1024}),
		onboard(this->graph.FindOnboardVisit(vehicle->index, station, vehicle->cur_real_order_index)) {}

PassengerJourneyShadowContext::PassengerJourneyShadowContext(const Vehicle *vehicle, StationID station, PassengerJourneyShadowLimits limits) :
		vehicle(vehicle), station(station), capture(_state_ticks), limits(limits)
{
	if (!_passenger_shadow_enabled) return;
	const auto start = ShadowClock::now();
	this->active = true;
	const int64_t epoch = this->capture.base() / 64;
	if (_passenger_shadow_epoch != epoch) {
		_passenger_shadow_epoch = epoch;
		_passenger_shadow_builds = _passenger_shadow_queries = _passenger_shadow_observations = _passenger_shadow_work = 0;
	}
	PassengerJourneyShadowLimits hard;
	this->limits.builds = std::min(limits.builds, hard.builds);
	this->limits.queries = std::min(limits.queries, hard.queries);
	this->limits.observations = std::min(limits.observations, hard.observations);
	this->limits.work = std::min(limits.work, hard.work);
	this->limits.journey.work = std::min(limits.journey.work, hard.journey.work);
	this->limits.journey.memory_bytes = std::min(limits.journey.memory_bytes, hard.journey.memory_bytes);
	this->limits.journey.candidates = 1;
	/* Fleet discovery is expensive. Select the first eligible cargo in consist order, once
	 * per epoch. This deterministic sampling consumes no game or interactive random state. */
	if (IsPassengerServiceVehicle(vehicle) && station != StationID::Invalid() && vehicle->current_order.IsAnyLoadingType()) {
		size_t coaches = 0;
		size_t inspected_packets = 0;
		for (const Vehicle *v = vehicle; v != nullptr && coaches++ < 256; v = v->Next()) {
			CargoType cargo = v->cargo_type;
			if (cargo >= NUM_CARGO || !IsCargoInClass(cargo, CargoClass::Passengers) || v->cargo.StoredCount() == 0 ||
					_settings_game.linkgraph.GetDistributionType(cargo) == DistributionType::Manual || this->snapshots.contains(cargo)) continue;
			if (_passenger_shadow_builds >= this->limits.builds || _passenger_shadow_work >= this->limits.work) break;
			bool known_od = false;
			for (const CargoPacket *packet : *v->cargo.Packets()) {
				if (inspected_packets++ >= this->limits.observations) break;
				if (packet->GetPassengerDestination() != StationID::Invalid()) { known_od = true; break; }
			}
			if (!known_od) continue;
			PassengerServiceGraphLimits graph_limits = limits.graph;
			graph_limits.work = std::min({graph_limits.work, hard.graph.work, this->limits.work - _passenger_shadow_work});
			graph_limits.memory_bytes = std::min(graph_limits.memory_bytes, hard.graph.memory_bytes);
			const auto graph_start = ShadowClock::now();
			auto graph = PassengerServiceGraph::Build(cargo, this->capture, graph_limits);
			for (const auto &trip : graph.GetTrips()) {
				if (trip.type < VehicleType::CompanyEnd) ++_passenger_shadow_stats.modes[to_underlying(trip.type)].prediction_reasons[to_underlying(trip.prediction_reason)];
			}
			_passenger_shadow_stats.graph_us += ShadowMicros(graph_start);
			_passenger_shadow_stats.graph_memory_bound = std::max(_passenger_shadow_stats.graph_memory_bound, graph.GetMemoryBound());
			_passenger_shadow_work += graph.GetWork();
			++_passenger_shadow_builds;
			++_passenger_shadow_stats.builds;
			this->snapshots.emplace(cargo, std::make_unique<Snapshot>(std::move(graph), vehicle, station));
		}
	}
	_passenger_shadow_stats.cpu_ns += ShadowNanos(start);
}

PassengerJourneyShadowContext::~PassengerJourneyShadowContext()
{
	if (!this->active) return;
	const auto start = ShadowClock::now();
	this->snapshots.clear();
	_passenger_shadow_stats.cpu_ns += ShadowNanos(start);
}

void PassengerJourneyShadowContext::Observe(const CargoPacket &packet, CargoType cargo, VehicleCargoList::MoveToAction legacy,
		StationID via, OrderUnloadType unload)
{
	if (!this->active || !_passenger_shadow_enabled || cargo >= NUM_CARGO || !IsCargoInClass(cargo, CargoClass::Passengers)) return;
	const auto start = ShadowClock::now();
	auto &stats = _passenger_shadow_stats;
	stats.observed.Add(packet.Count());
	const bool known_od = packet.GetPassengerDestination() != StationID::Invalid();
	if (known_od) stats.known_od.Add(packet.Count());
	const bool supported = IsPassengerServiceVehicle(this->vehicle);
	const bool manual = _settings_game.linkgraph.GetDistributionType(cargo) == DistributionType::Manual;
	const bool eligible = known_od && supported && !manual;
	if (known_od && !supported) stats.unsupported_od.Add(packet.Count());
	else if (known_od && manual) stats.manual_od.Add(packet.Count());
	if (eligible) stats.eligible_od.Add(packet.Count());
	PassengerShadowCategory category = PassengerShadowCategory::Skipped;
	PassengerShadowDecision decision = PassengerShadowDecision::Unknown;
	PassengerJourneyResult result;
	auto found = this->snapshots.find(cargo);
	if (eligible && found == this->snapshots.end()) stats.uncaptured_od.Add(packet.Count());
	if (eligible && found != this->snapshots.end() &&
			this->capture == _state_ticks && _passenger_shadow_observations < this->limits.observations &&
			_passenger_shadow_queries < this->limits.queries && _passenger_shadow_work < this->limits.work) {
		++_passenger_shadow_observations;
		auto &snapshot = *found->second;
		/* Discovery truncation and an ambiguous loading visit are observations of Unknown,
		 * never permission to use an old result or certify an unreachable destination. */
		result.status = snapshot.graph.GetStatus();
		if (result.status == PassengerPredictionStatus::Complete && snapshot.onboard) {
			PassengerJourneyEvaluation evaluation;
			evaluation.minimum_transfer = evaluation.transfer_penalty = _settings_game.linkgraph.passenger_transfer_penalty;
			PassengerJourneyLimits query_limits = this->limits.journey;
			/* Keep the same allowance/cache key until a full query no longer fits the epoch. */
			if (query_limits.work > this->limits.work - _passenger_shadow_work) {
				stats.categories[to_underlying(category)].Add(packet.Count());
				stats.cpu_ns += ShadowNanos(start);
				return;
			}
			const auto search_start = ShadowClock::now();
			result = snapshot.planner.PlanOnboard(cargo, *snapshot.onboard, packet.GetPassengerDestination(), query_limits, evaluation, packet.Count());
			stats.search_us += ShadowMicros(search_start);
			++stats.queries;
			if (result.diagnostic.cache_hit) ++stats.cache_hits;
			else {
				++_passenger_shadow_queries;
				_passenger_shadow_work += result.diagnostic.work;
				stats.work += result.diagnostic.work;
				stats.max_work = std::max<uint64_t>(stats.max_work, result.diagnostic.work);
			}
			stats.scratch_peak = std::max(stats.scratch_peak, result.diagnostic.scratch_bytes);
			stats.cache_peak = std::max(stats.cache_peak, snapshot.planner.GetCacheBytes());
		} else if (result.status == PassengerPredictionStatus::Complete) result.status = PassengerPredictionStatus::Unknown;
		stats.evaluated.Add(packet.Count());
		auto &mode = stats.modes[to_underlying(this->vehicle->type)];
		mode.evaluated.Add(packet.Count());
		if (result.HasJourney()) {
			mode.journeys.Add(packet.Count());
			VehicleType previous = this->vehicle->type;
			for (const auto &leg : result.journeys.front().legs) {
				VehicleType current = snapshot.graph.GetTrips()[leg.boarding.trip].type;
				if (current != previous) ++mode.mixed_transfers;
				previous = current;
			}
		} else mode.fallback.Add(packet.Count()); // Diagnostics only; no packet mutation.
		if (result.status == PassengerPredictionStatus::BudgetExhausted) stats.budget_exhausted.Add(packet.Count());
		if (result.status == PassengerPredictionStatus::LimitReached) stats.limit_reached.Add(packet.Count());
		if (result.status == PassengerPredictionStatus::Unknown) stats.horizon_incomplete.Add(packet.Count());
		if (result.status == PassengerPredictionStatus::Complete) {
			if (result.IsUnreachable()) decision = PassengerShadowDecision::Unreachable;
			else if (packet.GetPassengerDestination() == this->station) decision = PassengerShadowDecision::Arrived;
			else decision = result.journeys.front().legs.front().boarding == *snapshot.onboard ? PassengerShadowDecision::Stay : PassengerShadowDecision::Transfer;
		}
		if (decision == PassengerShadowDecision::Unknown) stats.unknown.Add(packet.Count());
		else stats.decidable.Add(packet.Count());
		const auto *order = this->vehicle->GetOrder(this->vehicle->cur_real_order_index);
		bool constrained = unload != OrderUnloadType::UnloadIfPossible ||
				(order != nullptr && order->GetCargoLoadType(cargo) == OrderLoadType::NoLoad);
		category = ComparePassengerShadowDecision(legacy, decision, constrained);
		/* A fixed sample of mismatches/constraints/incomplete queries, never per-packet output. */
		if (_passenger_shadow_details.size() < 64 && category != PassengerShadowCategory::BothStay &&
				category != PassengerShadowCategory::BothTransfer && category != PassengerShadowCategory::BothArrived) {
			PassengerJourneyShadowDetail detail{this->station, packet.GetFirstStation(), packet.GetPassengerDestination(), via,
					this->vehicle->index, legacy, decision, result.status, category, packet.Count()};
			if (!result.journeys.empty()) {
				const auto &journey = result.journeys.front();
				detail.arrival = journey.arrival;
				detail.transfers = journey.transfers;
				detail.generalized_time = journey.generalized_time;
			}
			_passenger_shadow_details.push_back(detail);
		}
	}
	stats.categories[to_underlying(category)].Add(packet.Count());
	stats.cpu_ns += ShadowNanos(start);
}
