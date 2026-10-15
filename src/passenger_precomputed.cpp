/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

#include "stdafx.h"
#include "passenger_precomputed.h"
#include "passenger_service_database.h"
#include "station_base.h"
#include "map_func.h"
#include "error_func.h"
#include <chrono>
#include "safeguards.h"

namespace {
	using Clock = std::chrono::steady_clock;
	uint64_t Elapsed(Clock::time_point start) { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count(); }
	bool Product(uint64_t a, uint64_t b, uint64_t &total, uint64_t limit)
	{
		if (a != 0 && b > (limit - total) / a) return false;
		total += a * b; return true;
	}
	std::vector<int64_t> PatternKey(const PassengerPeriodicService &s)
	{
		std::vector<int64_t> key{s.timetable.period, s.dispatch_period, s.uncertain_dispatch_period, static_cast<int64_t>(s.dispatch_stop)};
		for (const auto &stop : s.timetable.stops) {
			key.insert(key.end(), {stop.station.base(), stop.order, stop.arrival, stop.departure,
					static_cast<int64_t>(stop.board), static_cast<int64_t>(stop.alight), static_cast<int64_t>(stop.forced)});
		}
		key.push_back(-1);
		for (uint32_t slot : s.dispatch_slots) key.push_back(slot);
		return key;
	}
}

uint32_t PassengerPrecomputedTable::StateFor(uint32_t station, uint32_t pattern) const
{
	const auto &states = this->waiting_states[station];
	auto it = std::lower_bound(states.begin(), states.end(), pattern, [](const auto &a, uint32_t b) { return a.first < b; });
	return it != states.end() && it->first == pattern ? it->second : station;
}

OrderListID PassengerPrecomputedTable::BoardingKey(OrderListID service) const
{
	if (service.base() >= this->service_pattern.size() || this->service_pattern[service.base()] == INVALID) return service;
	return this->patterns[this->service_pattern[service.base()]].members.front().id;
}

bool PassengerPrecomputedTable::Rebind(PassengerJourneyExecution &ride, OrderListID service) const
{
	if (ride.service.base() >= this->service_pattern.size() || service.base() >= this->service_pattern.size()) return false;
	uint32_t pattern = this->service_pattern[ride.service.base()];
	if (pattern == INVALID || pattern != this->service_pattern[service.base()]) return false;
	for (const auto &member : this->patterns[pattern].members) if (member.id == service) {
		ride.service = service; ride.signature = member.incarnation; ride.generation = member.generation; return true;
	}
	return false;
}

bool PassengerPrecomputedTable::RefreshService(const PassengerPeriodicService &service)
{
	if (this->cargo >= NUM_CARGO || service.id.base() >= this->service_pattern.size()) return false;
	uint32_t pattern = this->service_pattern[service.id.base()];
	if (pattern == INVALID) return false;
	auto &p = this->patterns[pattern];
	auto member = std::lower_bound(p.members.begin(), p.members.end(), service.id, [](const auto &m, OrderListID id) { return m.id < id; });
	if (member == p.members.end() || member->id != service.id || member->incarnation != service.incarnation ||
			service.timetable.period <= 0 || p.occurrences.size() != service.timetable.stops.size()) return false;
	uint64_t mask = uint64_t{1} << this->cargo;
	for (size_t i = 0; i < p.occurrences.size(); ++i) {
		const auto &old = this->occurrences[p.occurrences[i]];
		const auto &stop = service.timetable.stops[i];
		if (old.station != stop.station || old.order != stop.order || old.board != ((stop.board & mask) != 0) ||
				old.alight != ((stop.alight & mask) != 0) || old.forced != ((stop.forced & mask) != 0)) return false;
	}
	member->generation = service.generation;
	return true;
}

void PassengerPrecomputedTable::AddEdge(uint32_t from, uint32_t to, int64_t cost, EdgeKind kind, uint32_t occurrence, uint32_t distance)
{
	assert(cost >= 0);
	this->edges.push_back({from, to, occurrence, cost, distance, kind});
}

void PassengerPrecomputedTable::Begin(std::span<const PassengerPeriodicService *const> services, CargoType cargo, int64_t transfer, uint64_t memory)
{
	*this = {};
	this->cargo = cargo;
	const auto started = Clock::now();
	this->diagnostic.builds = 1;
	if (cargo >= NUM_CARGO || transfer < 0 || transfer > INT64_MAX / 2) { this->failed = true; return; }
	const uint64_t mask = uint64_t{1} << cargo;
	memory = std::min(memory, MAX_BYTES);
	/* Canonical byte charges are identical on 32/64-bit peers. Native sizeof
	 * and allocator residency are diagnostics only, never routing admission. */
	static_assert(sizeof(Label) <= 64 && sizeof(Edge) <= 64 && sizeof(QueueItem) <= 32 && sizeof(Row) <= 64);
	uint64_t envelope = 1024 * 1024;
	if (envelope > memory) { this->failed = true; return; }
	for (const auto *s : services) {
		if (!Product(1, 256, envelope, memory) || !Product(s->timetable.stops.size(), 256, envelope, memory) ||
				!Product(s->vehicles.size(), 64, envelope, memory) || !Product(s->dispatch_slots.size(), 32, envelope, memory)) {
			this->failed = true; this->diagnostic.envelope = memory; return;
		}
	}
	try {
		this->station_index.assign(StationID::Invalid().base(), INVALID);
		this->service_pattern.assign(OrderListID::Invalid().base(), INVALID);
		std::map<std::vector<int64_t>, std::vector<const PassengerPeriodicService *>> groups;
		std::set<StationID> station_ids;
		for (const auto *s : services) {
			bool capacity = std::any_of(s->vehicles.begin(), s->vehicles.end(), [cargo](const auto &v) { return v.capacity[cargo] != 0; });
			if (!capacity) { this->unsupported |= s->timetable.period == 0; continue; }
			if (s->timetable.period <= 0 || s->timetable.stops.size() < 2) { this->unsupported = true; continue; }
			groups[PatternKey(*s)].push_back(s);
			for (const auto &stop : s->timetable.stops) if ((stop.board | stop.alight) & mask) station_ids.insert(stop.station);
		}
		this->stations.assign(station_ids.begin(), station_ids.end());
		for (uint32_t i = 0; i < this->stations.size(); ++i) this->station_index[this->stations[i].base()] = i;
		this->waiting_states.resize(this->stations.size());
		std::vector<const PassengerPeriodicService *> representatives;
		for (const auto &[key, members] : groups) {
			Pattern p;
			const auto &t = members.front()->timetable;
			p.circuit = std::max(t.period, members.front()->uncertain_dispatch_period);
			uint64_t seats = 0;
			bool unknown = false;
			std::vector<int64_t> phases;
			for (const auto *s : members) {
				p.members.push_back({s->id, s->incarnation, s->generation});
				this->service_pattern[s->id.base()] = static_cast<uint32_t>(this->patterns.size());
				for (const auto &v : s->vehicles) if (v.capacity[cargo]) {
					seats = std::max(seats, v.capacity[cargo]); unknown |= !v.phase_known;
					if (s->dispatch_period > 0) {
						p.circuit = s->dispatch_period;
						for (uint32_t slot : s->dispatch_slots) {
							int64_t phase = (s->dispatch_base.base() % p.circuit + slot) % p.circuit;
							phases.push_back(phase < 0 ? phase + p.circuit : phase);
						}
					} else {
						int64_t phase = v.base.base() % p.circuit;
						phases.push_back(phase < 0 ? phase + p.circuit : phase);
					}
				}
			}
			std::sort(p.members.begin(), p.members.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
			std::sort(phases.begin(), phases.end());
			phases.erase(std::unique(phases.begin(), phases.end()), phases.end());
			int64_t gap = p.circuit;
			if (!unknown && !phases.empty()) {
				gap = 0;
				for (size_t i = 0; i < phases.size(); ++i) gap = std::max(gap, (i + 1 < phases.size() ? phases[i + 1] : p.circuit + phases.front()) - phases[i]);
			}
			/* Unknown phases retain a conservative circuit estimate. No departure
			 * timestamps or evenly spaced synthetic trains are generated. */
			p.wait = unknown ? p.circuit : (gap + 1) / 2;
			p.risk = (gap + std::min<uint64_t>(seats, 64) - 1) / std::min<uint64_t>(seats, 64);
			p.weight = std::clamp<uint64_t>(std::min<uint64_t>(seats, 65535) * 65536 / std::max<int64_t>(1, gap), 1, UINT32_MAX / 2);
			uint32_t pattern = static_cast<uint32_t>(this->patterns.size());
			for (const auto &stop : t.stops) {
				p.occurrences.push_back(static_cast<uint32_t>(this->occurrences.size()));
				this->occurrences.push_back({stop.station, stop.order, pattern, 0, 0, (stop.board & mask) != 0, (stop.alight & mask) != 0, (stop.forced & mask) != 0});
				if (stop.alight & mask) this->waiting_states[this->station_index[stop.station.base()]].emplace_back(pattern, 0);
			}
			this->patterns.push_back(std::move(p)); representatives.push_back(members.front());
		}
		size_t nodes = this->stations.size();
		for (auto &states : this->waiting_states) {
			std::sort(states.begin(), states.end()); states.erase(std::unique(states.begin(), states.end()), states.end());
			for (auto &state : states) state.second = static_cast<uint32_t>(nodes++);
		}
		this->waiting_state_count = nodes;
		for (auto &o : this->occurrences) { o.arrival = static_cast<uint32_t>(nodes++); o.departure = static_cast<uint32_t>(nodes++); }
		size_t edge_count = 0;
		for (size_t p = 0; p < this->patterns.size(); ++p) for (const auto &stop : representatives[p]->timetable.stops) {
			size_t contribution = 2 + ((stop.alight & mask) != 0);
			if (stop.board & mask) contribution += 1 + this->waiting_states[this->station_index[stop.station.base()]].size();
			if (contribution > INVALID - edge_count) { this->failed = true; return; }
			edge_count += contribution;
		}
		/* Every product is checked before route/scratch allocation. Queue storage
		 * is bounded by successful edge relaxations, never by passengers or OD calls. */
		this->diagnostic.stations = this->stations.size(); this->diagnostic.patterns = this->patterns.size();
		this->diagnostic.states = nodes; this->diagnostic.edges = edge_count;
		this->diagnostic.stored_states = this->waiting_state_count + this->stations.size();
		bool fits = nodes < INVALID && edge_count < INVALID && nodes <= memory / 128 &&
				Product(this->stations.size(), uint64_t(this->diagnostic.stored_states) * 64 + 128, envelope, memory) &&
				Product(edge_count, 264, envelope, memory) && Product(nodes, 144, envelope, memory);
		this->diagnostic.envelope = envelope;
		if (!fits) { this->failed = true; this->diagnostic.build_nanoseconds = Elapsed(started); return; }
		this->edges.reserve(edge_count);
		for (uint32_t p = 0; p < this->patterns.size(); ++p) {
			const auto &pattern = this->patterns[p]; const auto &t = representatives[p]->timetable;
			for (size_t i = 0; i < t.stops.size(); ++i) {
				const auto &stop = t.stops[i]; uint32_t id = pattern.occurrences[i]; const auto &o = this->occurrences[id];
				if (stop.board & mask) {
					uint32_t station = this->station_index[stop.station.base()];
					this->AddEdge(station, o.departure, pattern.wait + pattern.risk, EdgeKind::Board, id);
					for (const auto &[previous, state] : this->waiting_states[station]) {
						if (previous != p || pattern.members.size() > 1) this->AddEdge(state, o.departure, pattern.wait + pattern.risk, EdgeKind::Board, id);
					}
				}
				if (stop.alight & mask) this->AddEdge(o.arrival, this->StateFor(this->station_index[stop.station.base()], p), 2 * transfer, EdgeKind::Alight, id);
				if (!(stop.forced & mask)) this->AddEdge(o.arrival, o.departure, stop.departure - stop.arrival, EdgeKind::Ride);
				size_t next = (i + 1) % t.stops.size();
				int64_t travel = t.stops[next].arrival - stop.departure + (next == 0 ? t.period : 0);
				uint32_t distance = 0;
				if (const auto *a = Station::GetIfValid(stop.station)) if (const auto *b = Station::GetIfValid(t.stops[next].station)) distance = DistanceManhattan(a->xy, b->xy);
				this->AddEdge(o.departure, this->occurrences[pattern.occurrences[next]].arrival, std::max<int64_t>(1, travel), EdgeKind::Ride, INVALID, distance);
			}
		}
		this->incoming_offsets.assign(nodes + 1, 0);
		for (const auto &edge : this->edges) ++this->incoming_offsets[edge.to + 1];
		for (size_t i = 1; i < this->incoming_offsets.size(); ++i) this->incoming_offsets[i] += this->incoming_offsets[i - 1];
		this->incoming.resize(this->edges.size()); auto cursor = this->incoming_offsets;
		for (uint32_t i = 0; i < this->edges.size(); ++i) this->incoming[cursor[this->edges[i].to]++] = i;
		this->rows.reserve(this->stations.size());
		this->diagnostic.stations = this->stations.size(); this->diagnostic.patterns = this->patterns.size();
		this->diagnostic.states = nodes; this->diagnostic.edges = this->edges.size();
		if (!this->stations.empty()) this->StartDestination();
	} catch (const std::bad_alloc &) {
		/* Local OOM cannot select different routes on one network peer. The
		 * deterministic envelope uses legacy fallback; physical OOM stops play. */
		FatalError("Insufficient memory for passenger routing. Disable passenger_journey_precomputed or free memory before resuming.");
	}
	this->diagnostic.build_nanoseconds += Elapsed(started);
}

void PassengerPrecomputedTable::StartDestination()
{
	this->scratch.labels.clear(); this->scratch.labels.reserve(this->incoming_offsets.size() - 1);
	this->scratch.alternative.assign(this->stations.size(), Label{});
	this->prepared.labels.clear(); this->prepared.labels.reserve(this->waiting_state_count);
	this->expanding = INVALID; this->finishing_row = false; this->finishing = 0;
	this->initializing_row = true; this->seeding = 0;
}

size_t PassengerPrecomputedTable::Step(size_t allowance, bool legacy_publication, const std::function<void(StationID)> &published)
{
	const auto started = Clock::now(); size_t used = 0;
	try {
		while (used < allowance && !this->Finished()) {
			++used;
			if (this->initializing_row) {
				if (this->scratch.labels.size() < this->diagnostic.states) { this->scratch.labels.emplace_back(); continue; }
				auto seed = [&](uint32_t state, uint32_t alight = INVALID) {
					auto &label = this->scratch.labels[state]; label.cost = 0; label.alight = alight;
					this->queue.emplace(0, 0, state);
				};
				if (this->seeding == 0) {
					seed(static_cast<uint32_t>(this->destination));
					for (const auto &[pattern, state] : this->waiting_states[this->destination]) seed(state);
				}
				if (this->seeding < this->occurrences.size()) {
					uint32_t id = static_cast<uint32_t>(this->seeding++); const auto &o = this->occurrences[id];
					if (o.alight && o.station == this->stations[this->destination]) seed(o.arrival, id);
				} else this->initializing_row = false;
				continue;
			}
			if (this->finishing_row) {
				if (this->finishing < this->occurrences.size()) {
					uint32_t id = static_cast<uint32_t>(this->finishing++); const auto &o = this->occurrences[id];
					uint32_t station = this->station_index[o.station.base()]; if (station == INVALID) continue;
					const auto &best = this->scratch.labels[station], &suffix = this->scratch.labels[o.departure];
					const auto &p = this->patterns[o.pattern];
					if (best.board == INVALID || suffix.alight == INVALID || suffix.cost == INT64_MAX || suffix.cost >= best.cost ||
							this->occurrences[best.board].pattern == o.pattern || suffix.cost > INT64_MAX - p.wait - p.risk) continue;
					int64_t cost = suffix.cost + p.wait + p.risk;
					if (cost - best.cost > best.cost / 4) continue;
					/* Only real Board edges are alternatives; NoLoad stops never enter. */
					if (!o.board) continue;
					auto &alt = this->scratch.alternative[station];
					if (cost < alt.cost) {
						alt = suffix; alt.board = id; ++alt.legs;
						alt.cost = cost; alt.waiting += p.wait;
					}
				} else {
					/* Arrival/departure labels are construction scratch. Queries use
					 * only initial/transfer waits and one complete dispersion label. */
					if (legacy_publication) {
						this->prepared.labels.assign(this->scratch.labels.begin(), this->scratch.labels.begin() + this->waiting_state_count);
					} else if (this->prepared.labels.size() < this->waiting_state_count) {
						this->prepared.labels.push_back(this->scratch.labels[this->prepared.labels.size()]); continue;
					}
					this->prepared.alternative = std::move(this->scratch.alternative);
					this->rows.push_back(std::move(this->prepared));
					if (published) published(this->stations[this->destination]);
					++this->destination;
					this->diagnostic.destinations = this->destination;
					if (!this->Finished()) this->StartDestination();
				}
				continue;
			}
			if (this->expanding == INVALID) {
				if (this->queue.empty()) { this->finishing_row = true; continue; }
				auto [cost, legs, state] = this->queue.top(); this->queue.pop();
				if (cost != this->scratch.labels[state].cost || legs != this->scratch.labels[state].legs) continue;
				this->expanding = state; this->cursor = this->incoming_offsets[state];
			} else if (this->cursor == this->incoming_offsets[this->expanding + 1]) {
				this->expanding = INVALID;
			} else {
				const auto &edge = this->edges[this->incoming[this->cursor++]];
				Label next = this->scratch.labels[edge.to];
				if (next.cost > INT64_MAX - edge.cost) continue;
				next.cost += edge.cost;
				next.distance = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(next.distance) + edge.distance));
				if (edge.kind == EdgeKind::Board) { next.board = edge.occurrence; ++next.legs; next.waiting += this->patterns[this->occurrences[edge.occurrence].pattern].wait; }
				if (edge.kind == EdgeKind::Ride) next.riding += edge.cost;
				if (edge.kind == EdgeKind::Alight) { next.board = INVALID; next.alight = edge.occurrence; next.waiting += edge.cost / 2; }
				auto &old = this->scratch.labels[edge.from];
				if (std::tie(next.cost, next.legs) >= std::tie(old.cost, old.legs)) continue;
				old = next; this->queue.emplace(next.cost, next.legs, edge.from);
			}
		}
	} catch (const std::bad_alloc &) { FatalError("Insufficient memory for passenger routing. Disable passenger_journey_precomputed or free memory before resuming."); }
	this->diagnostic.operations += used; this->diagnostic.build_nanoseconds += Elapsed(started);
	this->diagnostic.bytes = this->station_index.capacity() * 4 + this->service_pattern.capacity() * 4 + this->edges.capacity() * sizeof(Edge) +
			(this->incoming.capacity() + this->incoming_offsets.capacity()) * 4 + this->rows.size() * this->diagnostic.stored_states * sizeof(Label) +
			(this->scratch.labels.capacity() + this->scratch.alternative.capacity() + this->prepared.labels.capacity()) * sizeof(Label);
	return used;
}

void PassengerPrecomputedTable::ReleaseDestination(StationID destination)
{
	if (destination.base() >= this->station_index.size()) return;
	uint32_t row = this->station_index[destination.base()];
	if (row < this->rows.size() && !this->rows[row].labels.empty()) {
		this->diagnostic.bytes -= std::min(this->diagnostic.bytes,
				(this->rows[row].labels.capacity() + this->rows[row].alternative.capacity()) * sizeof(Label));
		this->rows[row] = {};
		--this->diagnostic.destinations;
	}
}

PassengerPrecomputedPlan PassengerPrecomputedTable::Select(const Label &label, StateTicks ready, OrderListID excluded, uint32_t sample) const
{
	PassengerPrecomputedPlan result;
	if (label.board == INVALID || label.alight == INVALID || label.cost == INT64_MAX) { result.state = this->unsupported ? PassengerPrecomputedPlan::State::Unsupported : PassengerPrecomputedPlan::State::NoRoute; return result; }
	const auto &board = this->occurrences[label.board], &alight = this->occurrences[label.alight]; const auto &pattern = this->patterns[board.pattern];
	size_t member = sample % pattern.members.size();
	if (pattern.members[member].id == excluded) member = (member + 1) % pattern.members.size();
	if (pattern.members[member].id == excluded) { result.state = PassengerPrecomputedPlan::State::NoRoute; return result; }
	const auto &m = pattern.members[member];
	PassengerJourneyExecution ride;
	ride.service = m.id; ride.signature = m.incarnation; ride.generation = m.generation;
	ride.boarding = board.station; ride.alighting = alight.station; ride.boarding_order = board.order; ride.alighting_order = alight.order;
	ride.kind = PassengerRideKind::EstimatedServiceIntent; ride.not_before = ready; ride.planned_at = ready;
	ride.estimated_wait = pattern.wait; ride.waiting_limit = std::min<int64_t>(INT64_MAX / 2, pattern.circuit) * 2;
	result.ride = ride; result.state = PassengerPrecomputedPlan::State::Ready;
	result.cost = label.cost; result.riding = label.riding; result.waiting = label.waiting; result.distance = label.distance; result.transfers = label.legs - 1;
	return result;
}

PassengerPrecomputedPlan PassengerPrecomputedTable::Plan(StationID origin, StationID destination, StateTicks ready, OrderListID excluded, uint32_t sample) const
{
	const auto started = Clock::now(); ++this->diagnostic.queries;
	auto lookup = [&]() -> PassengerPrecomputedPlan {
		if (this->failed) return {PassengerPrecomputedPlan::State::Memory};
		if (origin.base() >= this->station_index.size() || destination.base() >= this->station_index.size()) return {PassengerPrecomputedPlan::State::Unsupported};
		uint32_t from = this->station_index[origin.base()], to = this->station_index[destination.base()];
		if (from == INVALID || to == INVALID || from == to) return {this->unsupported ? PassengerPrecomputedPlan::State::Unsupported : PassengerPrecomputedPlan::State::NoRoute};
		if (to >= this->rows.size() || this->rows[to].labels.empty()) return {PassengerPrecomputedPlan::State::Building};
		uint32_t previous = excluded.base() < this->service_pattern.size() ? this->service_pattern[excluded.base()] : INVALID;
		uint32_t state = previous == INVALID ? from : this->StateFor(from, previous);
		const auto &row = this->rows[to]; Label label = row.labels[state];
		const auto &alternative = row.alternative[from];
		if (state == from && label.board != INVALID && alternative.board != INVALID) {
			const auto &best = this->patterns[this->occurrences[label.board].pattern], &alt = this->patterns[this->occurrences[alternative.board].pattern];
			if (uint64_t(sample) * (best.weight + alt.weight) / (uint64_t{1} << 32) < alt.weight) {
				label = alternative;
			}
		}
		return this->Select(label, ready, excluded, sample);
	};
	auto result = lookup();
	this->diagnostic.hits += result.state == PassengerPrecomputedPlan::State::Ready;
	this->diagnostic.no_route += result.state == PassengerPrecomputedPlan::State::NoRoute;
	this->diagnostic.missing += !result.ride && result.state != PassengerPrecomputedPlan::State::NoRoute;
	this->diagnostic.query_nanoseconds += Elapsed(started);
	return result;
}
