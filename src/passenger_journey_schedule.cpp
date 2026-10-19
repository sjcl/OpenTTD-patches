/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

#include "stdafx.h"
#include "passenger_journey_schedule.h"
#include "passenger_service_database.h"
#include "station_base.h"
#include <chrono>
#include <queue>

#include "safeguards.h"

namespace {
	bool Add(int64_t &value, int64_t delta)
	{
		if (delta < 0 || value > INT64_MAX - delta) return false;
		value += delta; return true;
	}
	struct Frequency { int64_t wait = 0, capacity_penalty = 0, circuit = 0; };
	/** One unknown vehicle: ceil(period/2) expected residual wait. Multiple
	 * unresolved phases: one whole circuit, without assuming uniform spacing.
	 * All phases known: half the largest actual phase gap (conservative).
	 * Capacity risk: an additional circuit/maximum individual capacity, capped
	 * at 64 seats. Fleet sizes never manufacture departures or aggregate seats. */
	std::optional<Frequency> Estimate(const PassengerPeriodicService &service, CargoType cargo, size_t &work, size_t limit)
	{
		if (service.timetable.period <= 0) return {};
		uint64_t capacity = 0; bool unknown = false;
		std::vector<int64_t> phases;
		for (const auto &v : service.vehicles) {
			if (work >= limit) return {}; ++work;
			if (!v.capacity[cargo]) continue;
			capacity = std::max(capacity, v.capacity[cargo]); unknown |= !v.phase_known;
			int64_t phase = v.base.base() % service.timetable.period;
			if (phase < 0) phase += service.timetable.period;
			phases.push_back(phase);
		}
		if (!capacity) return {};
		int64_t circuit = std::max(service.timetable.period, service.uncertain_dispatch_period), gap = circuit;
		if (!unknown && service.dispatch_period > 0) {
			circuit = service.dispatch_period; gap = 0;
			for (size_t i = 0; i < service.dispatch_slots.size(); ++i) {
				if (work >= limit) return {}; ++work;
				int64_t next = i + 1 < service.dispatch_slots.size() ? service.dispatch_slots[i + 1] : circuit + service.dispatch_slots[0];
				gap = std::max(gap, next - service.dispatch_slots[i]);
			}
		} else if (!unknown && phases.size() > 1) {
			/* Charge sorting as well as scanning; floor(log2(n))+1 comparisons per element. */
			size_t charge = phases.size() * std::bit_width(phases.size());
			if (charge > limit - work) { work = limit; return {}; } work += charge;
			std::sort(phases.begin(), phases.end()); gap = 0;
			for (size_t i = 0; i < phases.size(); ++i) gap = std::max(gap, (i + 1 < phases.size() ? phases[i + 1] : circuit + phases[0]) - phases[i]);
		}
		int64_t wait = unknown && (phases.size() > 1 || service.uncertain_dispatch_period > 0) ? circuit : (gap + 1) / 2;
		int64_t seats = static_cast<int64_t>(std::min<uint64_t>(capacity, 64));
		return Frequency{wait, (circuit + seats - 1) / seats, circuit};
	}
}

PassengerSchedulePlan PassengerSchedulePlanner::Search(const PassengerServiceDatabase &db, CargoType cargo, StationID origin,
		StationID destination, OrderListID excluded, PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, std::map<StationID, uint64_t> &dependencies) const
{
	PassengerSchedulePlan result;
	auto stops_at = [&](StationID station) -> const auto & { dependencies.try_emplace(station, db.StationRevision(station)); return db.StopsAt(station); };
	auto spend = [&]() { if (result.work >= limits.work) { result.deferred = true; return false; } ++result.work; return true; };
	if (!spend() || stops_at(origin).empty() || stops_at(destination).empty()) return result;
	if (limits.memory_bytes < 1024 || limits.labels == 0) { result.deferred = true; return result; }
	struct Path {
		StationID station; int64_t cost = 0, riding = 0, waiting = 0;
		std::vector<StationID> visited;
		std::vector<OrderListID> services;
		PassengerJourneyExecution first;
	};
	std::vector<Path> paths{{origin}}; paths[0].visited.push_back(origin);
	using Item = std::tuple<int64_t, size_t, StationID, size_t>;
	std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
	queue.emplace(0, 0, origin, 0);
	std::map<std::pair<StationID, size_t>, size_t> labels;
	std::map<OrderListID, std::optional<Frequency>> frequency;
	size_t memory = 1024;
	while (!queue.empty()) {
		if (!spend()) break;
		auto [cost, rounds, station, index] = queue.top(); queue.pop();
		Path current = paths[index];
		if (station == destination) {
			result.ride = current.first; result.cost = cost; result.riding = current.riding; result.waiting = current.waiting;
			result.transfers = rounds - 1; break;
		}
		if (rounds > limits.max_transfers) continue;
		if (++labels[{station, rounds}] > limits.labels_per_station) { result.deferred = true; continue; }
		for (const auto &ref : stops_at(station)) {
			if (!spend()) break;
			if ((rounds == 0 && ref.service == excluded) || (!current.services.empty() && current.services.back() == ref.service)) continue;
			const auto &s = *db.Get(ref.service); const auto &t = s.timetable;
			const auto &board = t.stops[ref.stop]; uint64_t mask = uint64_t{1} << cargo;
			if (!(board.board & mask)) continue;
			auto f = frequency.find(s.id);
			if (f == frequency.end()) {
				if (memory + 256 + s.vehicles.size() * sizeof(int64_t) > limits.memory_bytes) { result.deferred = true; break; }
				memory += 256;
				f = frequency.emplace(s.id, Estimate(s, cargo, result.work, limits.work)).first;
				if (result.work >= limits.work) { result.deferred = true; break; }
			}
			if (!f->second) continue;
			const auto &freq = *f->second;
			for (size_t step = 1; step < t.stops.size(); ++step) {
				if (!spend()) break;
				size_t alighting = (ref.stop + step) % t.stops.size(); const auto &stop = t.stops[alighting];
				bool cycle = false;
				for (StationID previous : current.visited) { if (!spend()) break; cycle |= previous == stop.station; }
				if (result.work >= limits.work) break;
				if (!cycle && (stop.alight & mask)) {
					int64_t duration = stop.arrival - board.departure + (alighting <= ref.stop ? t.period : 0);
					int64_t wait = freq.wait;
					if (rounds && !Add(wait, evaluation.minimum_transfer)) continue;
					int64_t next_cost = cost;
					if (!Add(next_cost, duration + s.AircraftAdjustment(ref.stop, alighting)) || !Add(next_cost, wait) || !Add(next_cost, freq.capacity_penalty) ||
							(rounds && !Add(next_cost, evaluation.transfer_penalty)) ||
							(evaluation.waiting_weight && (wait > INT64_MAX / evaluation.waiting_weight || !Add(next_cost, wait * evaluation.waiting_weight / 1000))) ||
							(rounds && !Add(next_cost, int64_t(evaluation.minimum_transfer) * evaluation.transfer_time_weight / 1000))) continue;
					size_t allocation = 1024 + (rounds + 2) * 32;
					if (paths.size() >= limits.labels || allocation > limits.memory_bytes - memory) { result.deferred = true; break; }
					memory += allocation;
					Path next = current; next.station = stop.station; next.cost = next_cost;
					if (!Add(next.riding, duration) || !Add(next.waiting, wait)) continue;
					next.visited.push_back(stop.station); next.services.push_back(s.id);
					if (rounds == 0) {
						next.first.service = s.id; next.first.signature = s.incarnation; next.first.generation = s.generation;
						next.first.boarding = board.station; next.first.alighting = stop.station;
						next.first.boarding_order = board.order; next.first.alighting_order = stop.order;
						next.first.kind = PassengerRideKind::EstimatedServiceIntent;
						next.first.estimated_wait = freq.wait; next.first.waiting_limit = 2 * freq.circuit;
					}
					queue.emplace(next_cost, rounds + 1, stop.station, paths.size()); paths.push_back(std::move(next));
				}
				if (stop.forced & mask) break;
			}
		}
		if (result.work >= limits.work) break;
	}
	if (!result.ride && result.work >= limits.work) result.deferred = true;
	return result;
}

PassengerSchedulePlan PassengerSchedulePlanner::Plan(PassengerServiceDatabase &db, CargoType cargo, StationID origin,
		StationID destination, StateTicks ready, OrderListID excluded, PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation)
{
	PassengerSchedulePlan result;
	if (cargo >= NUM_CARGO || origin == destination || !Station::IsValidID(origin) || !Station::IsValidID(destination) ||
			evaluation.minimum_transfer < 0 || evaluation.transfer_penalty < 0 || evaluation.crowding_penalty || evaluation.capacity_shortfall_penalty) return result;
	const auto start = std::chrono::steady_clock::now();
	PassengerServiceDatabase::PolicyKey key{true, cargo, origin, destination, excluded, limits, evaluation};
	auto *found = db.FindPolicy(key);
	if (found != nullptr) { result = std::get<PassengerSchedulePlan>(found->value); ++this->diagnostic.hits; }
	else {
		PassengerServiceDatabase::Dependencies dependencies;
		const auto compile_start = std::chrono::steady_clock::now();
		result = this->Search(db, cargo, origin, destination, excluded, limits, evaluation, dependencies); ++this->diagnostic.misses;
		this->diagnostic.compile_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - compile_start).count();
		db.StorePolicy(key, result, std::move(dependencies), 512);
	}
	this->diagnostic.work += result.work;
	this->diagnostic.query_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	if (result.ride) { result.ride->not_before = ready; result.ride->planned_at = ready; }
	return result;
}
