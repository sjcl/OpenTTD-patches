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
#include <numeric>
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
		std::vector<int64_t> key{s.timetable.period, s.timetable.estimated_period, s.dispatch_period, s.uncertain_dispatch_period, static_cast<int64_t>(s.dispatch_stop)};
		for (const auto &stop : s.timetable.stops) {
			key.insert(key.end(), {stop.station.base(), stop.order, stop.arrival, stop.departure,
					static_cast<int64_t>(stop.board), static_cast<int64_t>(stop.alight), static_cast<int64_t>(stop.forced)});
			key.insert(key.end(), {stop.continues, stop.travel_quality});
		}
		key.push_back(-1);
		for (uint32_t slot : s.dispatch_slots) key.push_back(slot);
		return key;
	}

	int64_t Mod(int64_t value, int64_t period)
	{
		value %= period;
		return value < 0 ? value + period : value;
	}

	/** Unknown phases are duration estimates, not invented departures. Sublinear
	 * fleet credit allows bunching: only floor(sqrt(N)), capped at eight, counts.
	 * Three quarters of a circuit per effective vehicle is above the uniform
	 * residual (one half). Dispatch competition retains at least half a circuit. */
	int64_t UnknownWait(int64_t circuit, size_t fleet, bool dispatch = false)
	{
		size_t effective = 1;
		while (effective < 8 && (effective + 1) * (effective + 1) <= fleet) ++effective;
		int64_t divisor = 4 * effective;
		int64_t wait = (3 * circuit + divisor - 1) / divisor;
		return std::max<int64_t>(dispatch ? (circuit + 1) / 2 : 1, wait);
	}

	int64_t TravelUncertainty(int64_t travel, uint8_t quality)
	{
		return quality == 0 ? 0 : (travel + (quality == 1 ? 7 : 3)) / (quality == 1 ? 8 : 4);
	}

	uint64_t SectionPathKey(uint32_t pattern, VehicleOrderID boarding, VehicleOrderID alighting)
	{
		static_assert(sizeof(VehicleOrderID) <= 2);
		return (uint64_t{pattern} << 32) | (uint64_t{boarding} << 16) | alighting;
	}

	/** Union of real periodic departure streams, never an artificial fleet headway.
	 * Prefer a complete joint cycle; otherwise observe one longest circuit. If
	 * even that contains excessive events, retain the fastest certified stream's
	 * conservative gap. All arithmetic and admission are independent of the host. */
	int64_t SectionGap(const std::set<std::pair<int64_t, int64_t>> &events)
	{
		constexpr uint64_t MAX_EVENTS = 65536;
		const int64_t fastest = events.begin()->first;
		int64_t horizon = 1, longest = events.rbegin()->first;
		bool exact = true;
		for (auto [period, phase] : events) {
			int64_t factor = period / std::gcd(horizon, period);
			if (horizon > INT64_MAX / factor || horizon * factor / fastest > MAX_EVENTS) { exact = false; break; }
			horizon *= factor;
		}
		if (!exact) horizon = longest;
		uint64_t count = 0;
		for (auto [period, phase] : events) {
			uint64_t n = static_cast<uint64_t>(horizon / period) + 2;
			if (n > MAX_EVENTS - count) return fastest;
			count += n;
		}
		using Event = std::pair<int64_t, int64_t>; // next departure, period
		std::priority_queue<Event, std::vector<Event>, std::greater<Event>> queue;
		int64_t previous = -fastest, gap = 0;
		for (auto [period, phase] : events) {
			previous = std::max(previous, phase == 0 ? 0 : phase - period);
			queue.emplace(phase == 0 ? period : phase, period);
		}
		while (!queue.empty()) {
			auto [time, period] = queue.top(); queue.pop();
			gap = std::max(gap, time - previous);
			if (time >= horizon) break;
			previous = time; // Coincident events contribute a zero gap, once.
			if (time <= INT64_MAX - period) queue.emplace(time + period, period);
		}
		return std::min(gap, fastest);
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

std::span<const OrderListID> PassengerPrecomputedTable::BoardingServices(OrderListID service) const
{
	if (service.base() >= this->service_pattern.size() || this->service_pattern[service.base()] == INVALID) return {};
	return this->patterns[this->service_pattern[service.base()]].boarding_services;
}

uint64_t PassengerPrecomputedTable::RawSectionBoardingKey(const PassengerJourneyExecution &ride)
{
	return (uint64_t{1} << 63) | SectionPathKey(ride.service.base(), ride.boarding_order, ride.alighting_order);
}

const PassengerPrecomputedTable::Member *PassengerPrecomputedTable::FindMember(OrderListID service) const
{
	if (service.base() >= this->service_pattern.size() || this->service_pattern[service.base()] == INVALID) return nullptr;
	const auto &members = this->patterns[this->service_pattern[service.base()]].members;
	auto it = std::lower_bound(members.begin(), members.end(), service, [](const Member &member, OrderListID id) { return member.id < id; });
	return it != members.end() && it->id == service ? &*it : nullptr;
}

const PassengerPrecomputedTable::Section *PassengerPrecomputedTable::FindSection(const PassengerJourneyExecution &ride) const
{
	if (ride.kind != PassengerRideKind::EstimatedServiceIntent) return nullptr;
	const auto *member = this->FindMember(ride.service);
	if (member == nullptr || member->incarnation != ride.signature || member->generation != ride.generation) return nullptr;
	auto it = this->section_paths.find(SectionPathKey(this->service_pattern[ride.service.base()], ride.boarding_order, ride.alighting_order));
	if (it == this->section_paths.end() || this->occurrences[it->second.board].station != ride.boarding ||
			this->occurrences[it->second.alight].station != ride.alighting) return nullptr;
	return &this->sections[it->second.section];
}

std::optional<uint64_t> PassengerPrecomputedTable::SectionBoardingKey(const PassengerJourneyExecution &ride) const
{
	if (const auto *section = this->FindSection(ride)) return section->key;
	return std::nullopt;
}

std::span<const uint64_t> PassengerPrecomputedTable::SectionBoardingKeys(OrderListID service, VehicleOrderID boarding) const
{
	if (service.base() >= this->service_pattern.size() || this->service_pattern[service.base()] == INVALID || this->boarding_sections.empty()) return {};
	const auto &stops = this->patterns[this->service_pattern[service.base()]].occurrences;
	auto it = std::lower_bound(stops.begin(), stops.end(), boarding, [&](uint32_t id, VehicleOrderID order) { return this->occurrences[id].order < order; });
	return it != stops.end() && this->occurrences[*it].order == boarding ? std::span<const uint64_t>(this->boarding_sections[*it]) : std::span<const uint64_t>{};
}

bool PassengerPrecomputedTable::Rebind(PassengerJourneyExecution &ride, OrderListID service, VehicleOrderID boarding) const
{
	const auto *member = this->FindMember(service);
	const auto *original = this->FindMember(ride.service);
	if (member == nullptr || original == nullptr || original->incarnation != ride.signature || original->generation != ride.generation || IsBoundPassengerRide(ride.kind)) return false;
	uint32_t pattern = this->service_pattern[service.base()];
	if (const auto *section = this->FindSection(ride)) {
		auto key = std::pair(pattern, boarding == INVALID_VEH_ORDER_ID ? VehicleOrderID{0} : boarding);
		auto it = std::lower_bound(section->bindings.begin(), section->bindings.end(), key, [&](const SectionBinding &binding, const auto &candidate) {
			const auto &stop = this->occurrences[binding.board]; return std::pair(stop.pattern, stop.order) < candidate;
		});
		if (it == section->bindings.end() || this->occurrences[it->board].pattern != pattern ||
				(boarding != INVALID_VEH_ORDER_ID && this->occurrences[it->board].order != boarding)) return false;
		ride.boarding_order = this->occurrences[it->board].order; ride.alighting_order = this->occurrences[it->alight].order;
	} else if (pattern != this->service_pattern[ride.service.base()] || (boarding != INVALID_VEH_ORDER_ID && ride.boarding_order != boarding)) {
		return false;
	}
	ride.service = service; ride.signature = member->incarnation; ride.generation = member->generation;
	return true;
}

bool PassengerPrecomputedTable::RefreshService(const PassengerPeriodicService &service)
{
	if (this->cargo >= NUM_CARGO || service.id.base() >= this->service_pattern.size()) return false;
	uint32_t pattern = this->service_pattern[service.id.base()];
	if (pattern == INVALID) return false;
	auto &p = this->patterns[pattern];
	auto member = std::lower_bound(p.members.begin(), p.members.end(), service.id, [](const auto &m, OrderListID id) { return m.id < id; });
	if (member == p.members.end() || member->id != service.id || member->incarnation != service.incarnation ||
			service.timetable.Circuit() <= 0 || p.occurrences.size() != service.timetable.stops.size()) return false;
	if (!this->estimates && p.estimated != (service.timetable.period == 0)) return false; // Exact legacy binding replay.
	uint64_t mask = uint64_t{1} << this->cargo;
	for (size_t i = 0; i < p.occurrences.size(); ++i) {
		const auto &old = this->occurrences[p.occurrences[i]];
		const auto &stop = service.timetable.stops[i];
		if (old.station != stop.station || old.order != stop.order || old.board != ((stop.board & mask) != 0) ||
				old.alight != ((stop.alight & mask) != 0) || old.forced != ((stop.forced & mask) != 0) || old.continues != stop.continues) return false;
	}
	member->generation = service.generation;
	member->estimated = service.timetable.period == 0;
	return true;
}

void PassengerPrecomputedTable::AddEdge(uint32_t from, uint32_t to, int64_t cost, EdgeKind kind, uint32_t occurrence, uint32_t distance, int64_t uncertainty)
{
	assert(cost >= 0);
	this->edges.push_back({from, to, occurrence, cost, distance, kind, uncertainty});
}

void PassengerPrecomputedTable::AddBoardEdges(uint32_t station, uint32_t to, uint32_t pattern, int64_t cost, EdgeKind kind, uint32_t occurrence, uint32_t distance)
{
	this->AddEdge(station, to, cost, kind, occurrence, distance);
	const auto &states = this->waiting_states[station];
	if (states.empty()) return;
	uint32_t excluded = this->patterns[pattern].members.size() == 1 ? this->StateFor(station, pattern) : INVALID;
	if (excluded == station) excluded = INVALID; // No waiting state exists for this previous pattern.
	this->edges.back().range = static_cast<uint32_t>(this->range_edges.size());
	this->range_edges.push_back({states.front().second, states.back().second + 1, excluded});
}

void PassengerPrecomputedTable::Begin(std::span<const PassengerPeriodicService *const> services, CargoType cargo, int64_t transfer, uint64_t memory, bool sections, bool cooperative, std::shared_ptr<uint64_t> memory_budget, bool estimates, bool optimized, bool integrated, bool hyperpath, bool revisit_dominance, bool continuation_witness)
{
	*this = {}; this->cargo = cargo; this->estimates = estimates; this->optimized = optimized; this->integrated = optimized && integrated;
	this->hyperpath = hyperpath; this->transfer_cost = transfer >= 0 && transfer <= INT64_MAX / 2 ? 2 * transfer : 0;
	this->revisit_dominance = revisit_dominance;
	this->continuation_witness = revisit_dominance && continuation_witness;
	if (hyperpath) { this->optimized = this->integrated = true; this->strategy.events.SetOptimized(true); }
	this->graph_build = this->BuildGraph({services.begin(), services.end()}, cargo, transfer, memory, sections, cooperative, std::move(memory_budget));
	if (!cooperative) this->PrepareGraph(SIZE_MAX);
}

bool PassengerPrecomputedTable::PrepareGraph(size_t quantum)
{
	if (!this->graph_build) return true;
	const auto started = Clock::now();
	while (this->graph_build && quantum-- > 0) {
		this->graph_build.Resume(1);
		++this->graph_work;
	}
	const bool ready = !this->graph_build;
	this->diagnostic.build_nanoseconds += Elapsed(started);
	if (ready) this->graph_build = {};
	return ready;
}

PassengerBuildTask PassengerPrecomputedTable::BuildGraph(std::vector<const PassengerPeriodicService *> services, CargoType cargo, int64_t transfer, uint64_t memory, bool sections, bool cooperative, std::shared_ptr<uint64_t> memory_budget)
{
	if (memory_budget) memory = std::min(memory, *memory_budget);
	struct Charge {
		std::shared_ptr<uint64_t> remaining; const PassengerPrecomputedDiagnostic &diagnostic;
		~Charge() { if (remaining) *remaining -= std::min(*remaining, diagnostic.envelope); }
	} charge{std::move(memory_budget), this->diagnostic};
	this->diagnostic.builds = 1;
	if (cargo >= NUM_CARGO || transfer < 0 || transfer > INT64_MAX / 2) { this->failed = true; co_return; }
	const uint64_t mask = uint64_t{1} << cargo;
	memory = std::min(memory, this->hyperpath ? MAX_HYPERPATH_BYTES : MAX_BYTES);
	/* Canonical byte charges are identical on 32/64-bit peers. Native sizeof
	 * and allocator residency are diagnostics only, never routing admission. */
	static_assert(sizeof(Label) <= 64 && sizeof(Edge) <= 64 && sizeof(QueueItem) <= 32 && sizeof(Row) <= 128);
	uint64_t envelope = 1024 * 1024;
	if (envelope > memory) { this->failed = true; co_return; }
	for (const auto *s : services) {
		if (cooperative) co_await std::suspend_always{};
		if (!Product(1, 256, envelope, memory) || !Product(s->timetable.stops.size(), 256, envelope, memory) ||
				!Product(s->vehicles.size(), this->hyperpath ? 128 : 64, envelope, memory) || !Product(s->dispatch_slots.size(), 32, envelope, memory)) {
			this->failed = true; this->diagnostic.envelope = memory; co_return;
		}
	}
	try {
		this->queue.SetOptimized(this->optimized);
		this->station_index.assign(StationID::Invalid().base(), INVALID);
		this->service_pattern.assign(OrderListID::Invalid().base(), INVALID);
		std::map<std::vector<int64_t>, std::vector<const PassengerPeriodicService *>> groups;
		std::set<StationID> station_ids;
		/* A vehicle moved between shared order lists can occur in two captures
		 * of a frozen publication. Count its newest captured ownership once;
		 * equal generations use the lower OrderList ID. Never consult live pools
		 * here, since replay must use the same immutable input on every peer. */
		std::map<VehicleID, std::pair<uint64_t, OrderListID>> owners;
		if (this->hyperpath) for (const auto *s : services) for (const auto &v : s->vehicles) {
			if (cooperative) co_await std::suspend_always{};
			if (v.id == VehicleID::Invalid()) continue;
			auto [it, inserted] = owners.try_emplace(v.id, s->generation, s->id);
			if (!inserted && (s->generation > it->second.first || (s->generation == it->second.first && s->id < it->second.second))) it->second = {s->generation, s->id};
		}
		for (const auto *s : services) {
			if (cooperative) co_await std::suspend_always{};
			bool capacity = std::any_of(s->vehicles.begin(), s->vehicles.end(), [&](const auto &v) { return v.capacity[cargo] != 0 && (!this->hyperpath || (owners.contains(v.id) && owners.at(v.id).second == s->id)); });
			if (!capacity) { this->unsupported |= s->timetable.period == 0; continue; }
			if ((this->estimates ? s->timetable.Circuit() : s->timetable.period) <= 0 || s->timetable.stops.size() < 2) { this->unsupported = true; continue; }
			auto key = PatternKey(*s);
			/* A strategy distinguishes actual services, even when their stop
			 * patterns coincide. This makes exclusion remove exactly the arriving
			 * OrderList's frequency, while retaining its identical neighbours. */
			if (this->hyperpath) key.push_back(s->id.base());
			groups[std::move(key)].push_back(s);
			for (const auto &stop : s->timetable.stops) if ((stop.board | stop.alight) & mask) station_ids.insert(stop.station);
		}
		this->stations.assign(station_ids.begin(), station_ids.end());
		for (uint32_t i = 0; i < this->stations.size(); ++i) this->station_index[this->stations[i].base()] = i;
		this->waiting_states.resize(this->stations.size());
		std::vector<const PassengerPeriodicService *> representatives;
		std::vector<std::vector<const PassengerPeriodicService *>> pattern_members;
		for (auto &[key, members] : groups) {
			if (cooperative) co_await std::suspend_always{};
			if (sections) std::sort(members.begin(), members.end(), [](const auto *a, const auto *b) { return a->id < b->id; });
			Pattern p;
			const auto &t = members.front()->timetable;
			p.circuit = std::max(t.Circuit(), members.front()->uncertain_dispatch_period);
			p.estimated = t.period == 0;
			if (p.estimated) ++this->diagnostic.estimated_patterns; else ++this->diagnostic.certified_patterns;
			uint64_t seats = 0;
			size_t unknown_fleet = 0;
			bool unknown = false;
			std::vector<int64_t> phases;
			std::set<VehicleID> physical;
			for (const auto *s : members) {
				if (cooperative) co_await std::suspend_always{};
				p.members.push_back({s->id, s->incarnation, s->generation, s->timetable.period == 0});
				this->service_pattern[s->id.base()] = static_cast<uint32_t>(this->patterns.size());
				for (const auto &v : s->vehicles) if (v.capacity[cargo]) {
					if (cooperative) co_await std::suspend_always{};
					if (this->hyperpath && (!owners.contains(v.id) || owners.at(v.id).second != s->id)) continue;
					if ((sections || this->estimates) && !physical.insert(v.id).second) continue;
					seats = std::max(seats, v.capacity[cargo]); unknown |= !v.phase_known;
					unknown_fleet += !v.phase_known || p.estimated;
					if (this->estimates && (!v.phase_known || p.estimated)) continue;
					if (s->dispatch_period > 0) {
						p.circuit = s->dispatch_period;
						for (uint32_t slot : s->dispatch_slots) {
							if (cooperative) co_await std::suspend_always{};
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
			for (const auto &member : p.members) p.boarding_services.push_back(member.id);
			std::sort(phases.begin(), phases.end());
			phases.erase(std::unique(phases.begin(), phases.end()), phases.end());
			int64_t gap = p.circuit;
			if ((!unknown || this->estimates) && !phases.empty()) {
				gap = 0;
				for (size_t i = 0; i < phases.size(); ++i) gap = std::max(gap, (i + 1 < phases.size() ? phases[i + 1] : p.circuit + phases.front()) - phases[i]);
			}
			/* Unknown phases retain a conservative circuit estimate. No departure
			 * timestamps or evenly spaced synthetic trains are generated. */
			p.wait = unknown ? p.circuit : (gap + 1) / 2;
			if (this->estimates && (unknown || p.estimated)) {
				p.wait = UnknownWait(p.circuit, unknown_fleet, members.front()->uncertain_dispatch_period > 0);
				if (!phases.empty()) p.wait = std::min(p.wait, (gap + 1) / 2);
				gap = std::max<int64_t>(1, 2 * p.wait);
			}
			p.risk = (gap + std::min<uint64_t>(seats, 64) - 1) / std::min<uint64_t>(seats, 64);
			if (this->estimates && (unknown || p.estimated)) p.risk += std::min<int64_t>(64, (p.wait + 7) / 8);
			p.weight = std::clamp<uint64_t>(std::min<uint64_t>(seats, 65535) * 65536 / std::max<int64_t>(1, gap), 1, UINT32_MAX / 2);
			uint32_t pattern = static_cast<uint32_t>(this->patterns.size());
			for (const auto &stop : t.stops) {
				if (cooperative) co_await std::suspend_always{};
				p.occurrences.push_back(static_cast<uint32_t>(this->occurrences.size()));
				this->occurrences.push_back({stop.station, stop.order, pattern, 0, 0, (stop.board & mask) != 0, (stop.alight & mask) != 0, (stop.forced & mask) != 0, stop.continues});
				if (stop.alight & mask) this->waiting_states[this->station_index[stop.station.base()]].emplace_back(pattern, 0);
			}
			this->patterns.push_back(std::move(p)); representatives.push_back(members.front());
			pattern_members.push_back(members);
		}
		size_t nodes = this->stations.size();
		for (auto &states : this->waiting_states) {
			if (cooperative) co_await std::suspend_always{};
			std::sort(states.begin(), states.end()); states.erase(std::unique(states.begin(), states.end()), states.end());
			for (auto &state : states) state.second = static_cast<uint32_t>(nodes++);
		}
		this->waiting_state_count = nodes;
		if (this->hyperpath) {
			this->waiting_station.resize(nodes);
			for (uint32_t s = 0; s < this->stations.size(); ++s) {
				this->waiting_station[s] = s;
				for (const auto &[p, state] : this->waiting_states[s]) this->waiting_station[state] = s;
			}
		}
		for (auto &o : this->occurrences) { o.arrival = static_cast<uint32_t>(nodes++); o.departure = static_cast<uint32_t>(nodes++); }
		if (this->hyperpath) {
			std::vector<uint64_t> frequency(this->stations.size());
			this->station_boarding.resize(this->stations.size());
			uint64_t candidate_bound = 0;
			for (uint32_t id = 0; id < this->occurrences.size(); ++id) if (this->occurrences[id].board) {
				const auto &o = this->occurrences[id];
				this->station_boarding[this->station_index[o.station.base()]].push_back(id);
				const auto wait = this->patterns[o.pattern].wait;
				if (wait <= 0 || wait > PassengerStrategy::MAX_WAIT) { this->failed = true; co_return; }
				auto &sum = frequency[this->station_index[o.station.base()]];
				uint64_t f = PassengerStrategy::Frequency(wait);
				if (f >= (uint64_t{1} << 63) - sum) { this->failed = true; co_return; }
				sum += f;
				candidate_bound += 1 + this->waiting_states[this->station_index[o.station.base()]].size();
			}
			size_t max_boarding = 0;
			for (const auto &boards : this->station_boarding) max_boarding = std::max(max_boarding, boards.size());
			/* Full sparse sets, including worst-case differences; never truncate
			 * to K candidates. Admission is based solely on the frozen graph. */
			if (!Product(max_boarding, uint64_t{4096} * 64, envelope, memory) || !Product(candidate_bound, uint64_t(this->stations.size()) * 8, envelope, memory) ||
					!Product(this->waiting_state_count, uint64_t(this->stations.size()) * 96 + 128, envelope, memory)) {
				this->failed = true; this->diagnostic.envelope = memory; co_return;
			}
		}
		size_t edge_count = 0; uint64_t logical_edge_bound = 0;
		for (size_t p = 0; p < this->patterns.size(); ++p) for (const auto &stop : representatives[p]->timetable.stops) {
			if (cooperative) co_await std::suspend_always{};
			size_t contribution = 2 + ((stop.alight & mask) != 0);
			if (stop.board & mask) contribution += 1 + this->waiting_states[this->station_index[stop.station.base()]].size();
			if (contribution > INVALID - edge_count) { this->failed = true; co_return; }
			logical_edge_bound += contribution;
			if (this->integrated && (stop.board & mask)) contribution -= this->waiting_states[this->station_index[stop.station.base()]].size();
			edge_count += contribution;
		}
		/* Every product is checked before route/scratch allocation. Queue storage
		 * is bounded by successful edge relaxations, never by passengers or OD calls. */
		this->diagnostic.stations = this->stations.size(); this->diagnostic.patterns = this->patterns.size();
		this->diagnostic.states = nodes; this->diagnostic.edges = edge_count;
		this->diagnostic.stored_states = this->waiting_state_count + this->stations.size();
		bool fits = nodes < INVALID && edge_count < INVALID && nodes <= memory / 128 &&
				Product(this->stations.size(), uint64_t(this->diagnostic.stored_states) * 64 + 128, envelope, memory) &&
				Product(this->stations.size(), uint64_t(this->stations.size()) * 8 + 64, envelope, memory) &&
				Product(edge_count, this->integrated ? 128 : 264, envelope, memory) &&
				(!this->integrated || Product(logical_edge_bound, 128, envelope, memory)) && Product(nodes, this->integrated ? 160 : 144, envelope, memory);
		this->diagnostic.envelope = envelope;
		if (!fits) { this->failed = true; co_return; }
		this->edges.reserve(edge_count);
		this->reachable_destinations.resize(this->stations.size());
		for (uint32_t p = 0; p < this->patterns.size(); ++p) {
			if (cooperative) co_await std::suspend_always{};
			const auto &pattern = this->patterns[p]; const auto &t = representatives[p]->timetable;
			for (size_t i = 0; i < t.stops.size(); ++i) {
				if (cooperative) co_await std::suspend_always{};
				const auto &stop = t.stops[i]; uint32_t id = pattern.occurrences[i]; auto &o = this->occurrences[id];
				o.dwell = stop.departure - stop.arrival;
				if (stop.board & mask) {
					uint32_t station = this->station_index[stop.station.base()];
					if (this->integrated) this->AddBoardEdges(station, o.departure, p, pattern.wait + pattern.risk, EdgeKind::Board, id);
					else {
					this->AddEdge(station, o.departure, pattern.wait + pattern.risk, EdgeKind::Board, id);
					for (const auto &[previous, state] : this->waiting_states[station]) {
						if (cooperative) co_await std::suspend_always{};
						if (previous != p || pattern.members.size() > 1) this->AddEdge(state, o.departure, pattern.wait + pattern.risk, EdgeKind::Board, id);
					}
					}
				}
				if (stop.alight & mask) this->AddEdge(o.arrival, this->StateFor(this->station_index[stop.station.base()], p), 2 * transfer, EdgeKind::Alight, id);
				if (!(stop.forced & mask)) this->AddEdge(o.arrival, o.departure, stop.departure - stop.arrival, EdgeKind::Ride);
				if (!stop.continues) continue;
				size_t next = (i + 1) % t.stops.size();
				int64_t travel = std::max<int64_t>(1, t.stops[next].arrival - stop.departure + (next == 0 ? t.Circuit() : 0));
				uint32_t distance = 0;
				if (const auto *a = Station::GetIfValid(stop.station)) if (const auto *b = Station::GetIfValid(t.stops[next].station)) distance = DistanceManhattan(a->xy, b->xy);
				int64_t uncertainty = TravelUncertainty(travel, t.stops[next].travel_quality);
				o.travel = travel; o.uncertainty = uncertainty; o.distance = distance;
				this->AddEdge(o.departure, this->occurrences[pattern.occurrences[next]].arrival, travel + uncertainty, EdgeKind::Ride, INVALID, distance, uncertainty);
			}
		}
		if (this->hyperpath && this->revisit_dominance) {
			if (!Product(this->occurrences.size(), 16, envelope, memory)) { this->failed = true; this->diagnostic.envelope = memory; co_return; }
			this->revisits.resize(this->occurrences.size());
			/* Two backwards passes find the next visit across the circuit seam.
			 * A forced unload or broken continuation severs physical occupancy.
			 * Prefix costs include travel uncertainty and the arrival stop's dwell,
			 * exactly as the two Ride edges between departures do. */
			for (size_t p = 0; p < this->patterns.size(); ++p) {
				const auto &pattern = this->patterns[p];
				bool road = std::any_of(representatives[p]->vehicles.begin(), representatives[p]->vehicles.end(), [](const auto &v) { return v.type == VehicleType::Road; });
				std::map<StationID, std::pair<uint32_t, int64_t>> next;
				int64_t cost = 0;
				for (size_t k = 2 * pattern.occurrences.size(); k-- > 0;) {
					if (cooperative) co_await std::suspend_always{};
					uint32_t id = pattern.occurrences[k % pattern.occurrences.size()];
					const auto &o = this->occurrences[id];
					const auto &to = this->occurrences[pattern.occurrences[(k + 1) % pattern.occurrences.size()]];
					if (!o.continues || to.forced || cost > INT64_MAX - o.travel ||
							cost + o.travel > INT64_MAX - o.uncertainty || cost + o.travel + o.uncertainty > INT64_MAX - to.dwell) {
						next.clear(); cost = 0;
					} else cost += o.travel + o.uncertainty + to.dwell;
					auto found = next.find(o.station);
					if (k < pattern.occurrences.size() && o.board && found != next.end() && found->second.first != id && this->occurrences[found->second.first].board) {
						/* Road orders can request different bay/drive-through directions at
						 * the same station. With no captured proof of equal facility access,
						 * retain the earlier opportunity. Other supported modes depend only
						 * on the same station/vehicle; refits and control flow are unsupported. */
						const auto &a = representatives[p]->timetable.stops[k];
						const auto &b = representatives[p]->timetable.stops[found->second.first - pattern.occurrences.front()];
						if (!this->continuation_witness || !road || (a.boarding_facility != 0 && a.boarding_facility == b.boarding_facility))
							this->revisits[id] = {found->second.first, cost - found->second.second};
					}
					next[o.station] = {id, cost};
				}
			}
		}
		if (sections && !this->hyperpath) {
			bool success = false;
			auto task = this->BuildSections(pattern_members, transfer, nodes, envelope, memory, cooperative, success);
			while (!task.Resume(1)) co_await std::suspend_always{};
			if (!success) { this->failed = true; this->diagnostic.envelope = memory; co_return; }
		}
		this->diagnostic.envelope = envelope;
		if (this->optimized) this->ContractGraph(nodes);
		this->incoming_offsets.assign(nodes + 1, 0);
		for (const auto &edge : this->edges) ++this->incoming_offsets[edge.to + 1];
		for (size_t i = 1; i < this->incoming_offsets.size(); ++i) this->incoming_offsets[i] += this->incoming_offsets[i - 1];
		auto cursor = this->incoming_offsets;
		if (this->integrated) {
			/* Write each stream directly into its stable reverse-CSR position.
			 * The builder AoS and destination ids are released before searching. */
			this->hot_edges.from.resize(this->edges.size());
			this->hot_edges.cost.resize(this->edges.size());
			this->hot_edges.legs.resize(this->edges.size());
			this->hot_edges.range.resize(this->edges.size());
			this->cold_edges.resize(this->edges.size());
			for (const auto &edge : this->edges) {
				uint32_t at = cursor[edge.to]++;
				this->hot_edges.range[at] = edge.range;
				this->hot_edges.from[at] = edge.from; this->hot_edges.cost[at] = edge.cost;
				this->hot_edges.legs[at] = edge.kind == EdgeKind::Board || edge.kind == EdgeKind::Section;
				this->cold_edges[at] = {edge.occurrence, edge.distance, edge.uncertainty, edge.kind};
			}
			this->diagnostic.logical_edges = this->edges.size();
			for (const auto &range : this->range_edges) this->diagnostic.logical_edges += range.end - range.begin - (range.excluded >= range.begin && range.excluded < range.end);
			this->diagnostic.range_edges = this->range_edges.size();
			std::vector<Edge>().swap(this->edges);
		} else if (this->optimized) {
			/* Stable reverse CSR: the incoming order at each state is exactly
			 * the original insertion order, including equal-cost alternatives. */
			std::vector<Edge> reverse(this->edges.size());
			for (const auto &edge : this->edges) reverse[cursor[edge.to]++] = edge;
			this->edges.swap(reverse);
		} else {
			this->incoming.resize(this->edges.size());
			for (uint32_t i = 0; i < this->edges.size(); ++i) this->incoming[cursor[this->edges[i].to]++] = i;
		}
		if (this->optimized) {
			this->seed_offsets.assign(this->stations.size() + 1, 0);
			for (const auto &o : this->occurrences) {
				if (cooperative) co_await std::suspend_always{};
				if (o.alight) ++this->seed_offsets[this->station_index[o.station.base()] + 1];
			}
			for (size_t i = 1; i < this->seed_offsets.size(); ++i) this->seed_offsets[i] += this->seed_offsets[i - 1];
			this->seed_occurrences.resize(this->seed_offsets.back()); auto cursor = this->seed_offsets;
			for (uint32_t id = 0; id < this->occurrences.size(); ++id) {
				if (cooperative) co_await std::suspend_always{};
				const auto &o = this->occurrences[id];
				if (o.alight) this->seed_occurrences[cursor[this->station_index[o.station.base()]]++] = id;
			}
		}
		this->rows.reserve(this->stations.size());
		this->diagnostic.stations = this->stations.size(); this->diagnostic.patterns = this->patterns.size();
		this->diagnostic.states = nodes; this->diagnostic.edges = this->integrated ? this->hot_edges.from.size() : this->edges.size();
		this->queue.SetCapacityLimit(uint64_t(this->integrated ? this->diagnostic.logical_edges : this->diagnostic.edges) + nodes);
		if (this->hyperpath) {
			this->hyperpath_static_bytes = this->revisits.capacity() * sizeof(Revisit) + (this->station_index.capacity() + this->service_pattern.capacity() + this->waiting_station.capacity() +
					this->incoming_offsets.capacity() + this->seed_offsets.capacity() + this->seed_occurrences.capacity()) * 4 + this->stations.capacity() * sizeof(StationID) +
					this->patterns.capacity() * sizeof(Pattern) + this->occurrences.capacity() * sizeof(Occurrence) + this->range_edges.capacity() * sizeof(RangeEdge) +
					this->cold_edges.capacity() * sizeof(ColdEdge) + this->hot_edges.from.capacity() * 4 + this->hot_edges.cost.capacity() * 8 +
					this->hot_edges.legs.capacity() + this->hot_edges.range.capacity() * 4 + this->rows.capacity() * sizeof(Row) +
					this->waiting_states.capacity() * sizeof(this->waiting_states[0]) + this->station_boarding.capacity() * sizeof(this->station_boarding[0]);
			for (const auto &p : this->patterns) this->hyperpath_static_bytes += p.members.capacity() * sizeof(Member) + p.boarding_services.capacity() * sizeof(OrderListID) + p.occurrences.capacity() * 4;
			for (const auto &waits : this->waiting_states) this->hyperpath_static_bytes += waits.capacity() * sizeof(waits[0]);
			for (const auto &boards : this->station_boarding) this->hyperpath_static_bytes += boards.capacity() * 4;
		}
		if (!this->stations.empty()) this->StartDestination();
	} catch (const std::bad_alloc &) {
		/* Local OOM cannot select different routes on one network peer. The
		 * deterministic envelope uses legacy fallback; physical OOM stops play. */
		FatalError("Insufficient memory for passenger routing. Disable passenger_journey_precomputed or free memory before resuming.");
	}
}

/** Remove only unobservable, strictly advancing on-board chains. Keep the
 * occurrence metadata and the relative rank of every surviving state. A Board,
 * Alight, Section, landing or destination seed must never enter such a chain.
 * Alight-only arrivals have competing continuations: contracting their dwell
 * can change equal-label discovery order, so their departure is retained. */
void PassengerPrecomputedTable::ContractGraph(size_t &nodes)
{
	/* Even the upper bound on saved row work must amortize the two full
	 * edge passes below. This uses only frozen graph sizes, never elapsed time
	 * or available memory. Tiny pass-through populations keep the old graph. */
	size_t candidates = std::count_if(this->occurrences.begin(), this->occurrences.end(), [](const auto &o) { return !o.board && !o.alight && !o.forced && o.continues; });
	if (uint64_t(candidates) * this->stations.size() * 8 <= uint64_t(this->edges.size()) * 2) return;
	if (this->integrated) {
		uint64_t logical = this->edges.size();
		for (const auto &range : this->range_edges) logical += range.end - range.begin - (range.excluded >= range.begin && range.excluded < range.end);
		/* Range compression can make the physical graph tiny without reducing
		 * virtual search work. Require at least a one-percent upper bound on
		 * saved per-row work before paying for degree/rank construction. */
		if (uint64_t(candidates) * 8 * 100 <= nodes + logical) return;
	}
	/* Ranges refer only to the waiting-state prefix, whose ranks never move.
	 * Their missing per-wait degrees cannot affect pure occurrence nodes. */
	std::vector<uint32_t> in_degree(nodes), out_degree(nodes), successor(nodes, INVALID), rank(nodes, INVALID);
	for (uint32_t i = 0; i < this->edges.size(); ++i) {
		const auto &edge = this->edges[i];
		++in_degree[edge.to]; ++out_degree[edge.from]; successor[edge.from] = i;
	}
	std::vector<bool> remove(nodes);
	for (const auto &o : this->occurrences) {
		if (o.board || o.alight || o.forced || !o.continues || in_degree[o.arrival] != 1 || out_degree[o.arrival] != 1 ||
				in_degree[o.departure] != 1 || out_degree[o.departure] != 1) continue;
		const auto &dwell = this->edges[successor[o.arrival]], &travel = this->edges[successor[o.departure]];
		if (dwell.kind != EdgeKind::Ride || dwell.to != o.departure || travel.kind != EdgeKind::Ride || travel.cost <= 0) continue;
		remove[o.arrival] = remove[o.departure] = true;
	}
	/* Validate all sums before mutating anything. Closed, unobservable cycles
	 * have no surviving predecessor and are simply unreachable from any seed. */
	for (const auto &edge : this->edges) {
		if (remove[edge.from]) continue;
		int64_t cost = edge.cost, uncertainty = edge.uncertainty;
		uint32_t to = edge.to;
		while (remove[to]) {
			const auto &next = this->edges[successor[to]];
			if (edge.kind != EdgeKind::Ride || cost > INT64_MAX - next.cost || uncertainty > INT64_MAX - next.uncertainty) return;
			cost += next.cost; uncertainty += next.uncertainty; to = next.to;
		}
	}
	uint32_t count = 0;
	for (uint32_t state = 0; state < nodes; ++state) if (!remove[state]) rank[state] = count++;
	std::vector<Edge> contracted; contracted.reserve(this->edges.size());
	for (auto edge : this->edges) {
		if (remove[edge.from]) continue;
		while (remove[edge.to]) {
			const auto &next = this->edges[successor[edge.to]];
			edge.cost += next.cost; edge.uncertainty += next.uncertainty;
			edge.distance = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(edge.distance) + next.distance));
			edge.to = next.to;
		}
		edge.from = rank[edge.from]; edge.to = rank[edge.to];
		contracted.push_back(edge);
	}
	this->edges.swap(contracted);
	for (auto &o : this->occurrences) { o.arrival = rank[o.arrival]; o.departure = rank[o.departure]; }
	for (auto &state : this->landings) if (state != INVALID) state = rank[state];
	nodes = count;
}

PassengerBuildTask PassengerPrecomputedTable::BuildSections(const std::vector<std::vector<const PassengerPeriodicService *>> &members,
		int64_t transfer, size_t &nodes, uint64_t &envelope, uint64_t memory, bool cooperative, bool &success)
{
	struct Path { uint32_t board, alight; int64_t riding; uint32_t distance; int64_t uncertainty; };
	struct Prefix { std::vector<Path> paths; };
	/* Intern all directed stop prefixes. Timings, order numbers and the circuit
	 * outside the section are deliberately absent from compatibility; every
	 * physical stop and this cargo's exchange/forced flags remain in the key. */
	using Key = std::tuple<uint32_t, StationID, uint8_t>;
	std::map<Key, uint32_t> index;
	std::vector<Prefix> prefixes;
	std::map<StationID, std::set<uint32_t>> starters;
	for (uint32_t p = 0; p < this->patterns.size(); ++p) for (uint32_t id : this->patterns[p].occurrences) {
		if (cooperative) co_await std::suspend_always{};
		if (this->occurrences[id].board) starters[this->occurrences[id].station].insert(p);
	}
	for (uint32_t p = 0; p < this->patterns.size(); ++p) {
		if (cooperative) co_await std::suspend_always{};
		const auto &pattern = this->patterns[p]; const auto &t = members[p].front()->timetable;
		for (size_t start = 0; start < t.stops.size(); ++start) {
			if (cooperative) co_await std::suspend_always{};
			uint32_t board = pattern.occurrences[start]; const auto &b = this->occurrences[board];
			if (!b.board || starters[b.station].size() < 2) continue;
			uint32_t prefix = INVALID, distance = 0;
			int64_t uncertainty = 0;
			for (size_t length = 0; length < t.stops.size(); ++length) {
				if (cooperative) co_await std::suspend_always{};
				size_t stop = (start + length) % t.stops.size();
				uint32_t alight = pattern.occurrences[stop]; const auto &a = this->occurrences[alight];
				Key key{prefix, a.station, uint8_t((a.board ? 1 : 0) | (a.alight ? 2 : 0) | (a.forced ? 4 : 0))};
				auto found = index.find(key);
				if (found == index.end()) {
					if (prefixes.size() >= INVALID || !Product(1, 256, envelope, memory)) co_return;
					prefix = static_cast<uint32_t>(prefixes.size()); index.emplace(key, prefix); prefixes.emplace_back();
				} else prefix = found->second;
				if (length == 0) continue;
				const auto &previous = t.stops[(stop + t.stops.size() - 1) % t.stops.size()];
				if (!previous.continues) break;
				int64_t travel = std::max<int64_t>(1, t.stops[stop].arrival - previous.departure + (stop == 0 ? t.Circuit() : 0));
				uncertainty += TravelUncertainty(travel, t.stops[stop].travel_quality);
				if (const auto *x = Station::GetIfValid(previous.station)) if (const auto *y = Station::GetIfValid(a.station)) {
					distance = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(distance) + DistanceManhattan(x->xy, y->xy)));
				}
				if (a.alight && a.station != b.station) {
					if (!Product(1, 64, envelope, memory)) co_return;
					int64_t riding = t.stops[stop].arrival - t.stops[start].departure + (stop < start ? t.Circuit() : 0);
					prefixes[prefix].paths.push_back({board, alight, riding, distance, uncertainty});
				}
				if (a.forced) break;
			}
		}
	}
	uint64_t event_memory = 0;
	for (const auto &prefix : prefixes) {
		if (cooperative) co_await std::suspend_always{};
		const auto &paths = prefix.paths;
		if (paths.empty()) continue;
		uint32_t first = this->occurrences[paths.front().board].pattern;
		if (std::none_of(paths.begin(), paths.end(), [&](const Path &path) { return this->occurrences[path.board].pattern != first; })) continue;
		uint64_t candidates = 0;
		for (const auto &path : paths) for (const auto *service : members[this->occurrences[path.board].pattern]) {
			if (cooperative) co_await std::suspend_always{};
			candidates += service->vehicles.size() + service->dispatch_slots.size();
		}
		/* These maps/heap coexist only for one section. Charge their peak before
		 * allocating them, in addition to the prefix and final graph storage. */
		if (candidates > event_memory) {
			if (!Product(candidates - event_memory, 256, envelope, memory)) co_return;
			event_memory = candidates;
		}
		std::map<VehicleID, OrderListID> owners;
		for (const auto &path : paths) for (const auto *s : members[this->occurrences[path.board].pattern]) for (const auto &v : s->vehicles) {
			if (cooperative) co_await std::suspend_always{};
			if (!v.capacity[this->cargo]) continue;
			auto [it, inserted] = owners.emplace(v.id, s->id);
			if (!inserted) it->second = std::min(it->second, s->id);
		}
		std::set<std::pair<int64_t, int64_t>> events;
		int64_t unknown_gap = INT64_MAX;
		uint64_t seats = 1;
		std::set<VehicleID> unknown_vehicles;
		int64_t unknown_circuit = INT64_MAX;
		bool uncertain_dispatch = false;
		for (const auto &path : paths) {
			if (cooperative) co_await std::suspend_always{};
			uint32_t p = this->occurrences[path.board].pattern;
			size_t stop = path.board - this->patterns[p].occurrences.front();
			for (const auto *s : members[p]) for (const auto &v : s->vehicles) {
				if (cooperative) co_await std::suspend_always{};
				if (!v.capacity[this->cargo] || owners.at(v.id) != s->id) continue;
				seats = std::max(seats, std::min<uint64_t>(v.capacity[this->cargo], 64));
				int64_t circuit = std::max(s->timetable.Circuit(), s->uncertain_dispatch_period);
				if (s->dispatch_period > 0) {
					int64_t period = s->dispatch_period;
					int64_t offset = s->timetable.stops[stop].departure - s->timetable.stops[s->dispatch_stop].departure;
					/* A board before the dispatch anchor belongs to the next run. */
					if (stop < s->dispatch_stop) offset += s->timetable.period;
					for (uint32_t slot : s->dispatch_slots) events.emplace(period, Mod(Mod(s->dispatch_base.base(), period) + slot + offset, period));
				} else if (s->timetable.period > 0 && v.phase_known && s->uncertain_dispatch_period == 0) {
					events.emplace(circuit, Mod(Mod(v.base.base(), circuit) + s->timetable.stops[stop].departure, circuit));
				} else {
					unknown_gap = std::min(unknown_gap, circuit);
					unknown_vehicles.insert(v.id);
					unknown_circuit = std::min(unknown_circuit, circuit);
					uncertain_dispatch |= s->uncertain_dispatch_period > 0;
				}
			}
		}
		int64_t gap = events.empty() ? unknown_gap : SectionGap(events);
		if (gap == INT64_MAX) continue;
		int64_t wait = events.empty() ? gap : gap / 2 + gap % 2;
		if (this->estimates && !unknown_vehicles.empty()) {
			/* Use the longest unknown circuit for fleet credit; heterogeneous
			 * long routes must not inherit a short shuttle's headway. */
			int64_t longest = 0;
			for (const auto &path : paths) for (const auto *s : members[this->occurrences[path.board].pattern])
				longest = std::max(longest, std::max(s->timetable.Circuit(), s->uncertain_dispatch_period));
			int64_t estimated = std::min(UnknownWait(unknown_circuit, 1, uncertain_dispatch), UnknownWait(longest, unknown_vehicles.size(), uncertain_dispatch));
			wait = events.empty() ? estimated : std::min(wait, estimated);
			gap = std::max<int64_t>(1, 2 * wait);
		}
		int64_t risk = gap / seats + (gap % seats != 0);
		if (this->estimates && !unknown_vehicles.empty()) risk += std::min<int64_t>(64, (wait + 7) / 8);
		++this->diagnostic.shared_sections;
		if (this->landings.empty()) {
			if (!Product(this->occurrences.size(), 36, envelope, memory)) co_return;
			this->landings.assign(this->occurrences.size(), INVALID);
			this->boarding_sections.resize(this->occurrences.size());
		}
		/* Persist the same complete paths used by the frequency estimate. Each
		 * pattern refers to its existing member vector, rather than copying this
		 * binding once per vehicle or shared order list. The saved concrete order
		 * pair identifies the section without adding anything to CargoPacket. */
		if (this->sections.size() >= INVALID || !Product(1, 128, envelope, memory) || !Product(paths.size(), 128, envelope, memory)) co_return;
		Section section{UINT64_MAX, {}};
		section.bindings.reserve(paths.size());
		uint64_t key_count = 0;
		for (const auto &path : paths) key_count += this->patterns[this->occurrences[path.board].pattern].members.size();
		if (!Product(key_count, 16, envelope, memory) || !Product(key_count, uint64_t(paths.size()) * 16, envelope, memory)) co_return;
		std::vector<uint64_t> raw_keys;
		raw_keys.reserve(static_cast<size_t>(key_count));
		for (const auto &path : paths) {
			if (cooperative) co_await std::suspend_always{};
			const auto &board = this->occurrences[path.board], &alight = this->occurrences[path.alight];
			section.key = std::min(section.key, (uint64_t{1} << 63) |
					SectionPathKey(this->patterns[board.pattern].members.front().id.base(), board.order, alight.order));
			for (const auto &member : this->patterns[board.pattern].members) raw_keys.push_back((uint64_t{1} << 63) | SectionPathKey(member.id.base(), board.order, alight.order));
		}
		std::sort(raw_keys.begin(), raw_keys.end()); raw_keys.erase(std::unique(raw_keys.begin(), raw_keys.end()), raw_keys.end());
		for (const auto &path : paths) {
			if (cooperative) co_await std::suspend_always{};
			const auto &board = this->occurrences[path.board], &alight = this->occurrences[path.alight];
			section.bindings.push_back({path.board, path.alight});
			this->section_paths.emplace(SectionPathKey(board.pattern, board.order, alight.order),
					SectionPath{static_cast<uint32_t>(this->sections.size()), path.board, path.alight});
			this->boarding_sections[path.board].insert(this->boarding_sections[path.board].end(), raw_keys.begin(), raw_keys.end());
		}
		std::sort(section.bindings.begin(), section.bindings.end(), [&](const SectionBinding &a, const SectionBinding &b) {
			const auto &x = this->occurrences[a.board], &y = this->occurrences[b.board]; return std::tie(x.pattern, x.order) < std::tie(y.pattern, y.order);
		});
		this->sections.push_back(std::move(section));
		for (const auto &path : paths) {
			if (cooperative) co_await std::suspend_always{};
			const auto &board = this->occurrences[path.board], &alight = this->occurrences[path.alight];
			uint32_t station = this->station_index[board.station.base()];
			size_t edges = 1 + this->waiting_states[station].size();
			if (this->section_rides.size() >= INVALID || edges >= INVALID - this->edges.size() ||
					!Product(1, 64, envelope, memory) || !Product(this->integrated ? 1 : edges, this->integrated ? 128 : 264, envelope, memory) ||
					(this->integrated && !Product(edges, 128, envelope, memory))) co_return;
			if (this->landings[path.alight] == INVALID) {
				if (nodes >= INVALID || !Product(1, 144 + 264, envelope, memory)) co_return;
				this->landings[path.alight] = static_cast<uint32_t>(nodes++);
				this->AddEdge(this->landings[path.alight], this->StateFor(this->station_index[alight.station.base()], alight.pattern),
						2 * transfer, EdgeKind::Alight, path.alight);
			}
			uint32_t id = static_cast<uint32_t>(this->section_rides.size());
			int64_t cost = path.riding + path.uncertainty + wait + risk;
			this->section_rides.push_back({path.board, path.alight, path.riding, wait, cost});
			if (this->integrated) this->AddBoardEdges(station, this->landings[path.alight], board.pattern, cost, EdgeKind::Section, id, path.distance);
			else {
			this->AddEdge(station, this->landings[path.alight], cost, EdgeKind::Section, id, path.distance);
			for (const auto &[previous, state] : this->waiting_states[station]) {
				if (cooperative) co_await std::suspend_always{};
				if (previous != board.pattern || this->patterns[board.pattern].members.size() > 1) {
					this->AddEdge(state, this->landings[path.alight], cost, EdgeKind::Section, id, path.distance);
				}
			}
			}
		}
	}
	for (auto &keys : this->boarding_sections) {
		if (cooperative) co_await std::suspend_always{};
		std::sort(keys.begin(), keys.end()); keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	}
	this->binding_bytes = this->sections.capacity() * sizeof(Section) + this->boarding_sections.capacity() * sizeof(std::vector<uint64_t>) + this->section_paths.size() * 64;
	for (const auto &section : this->sections) this->binding_bytes += section.bindings.capacity() * sizeof(SectionBinding);
	for (const auto &keys : this->boarding_sections) this->binding_bytes += keys.capacity() * sizeof(uint64_t);
	this->diagnostic.section_rides = this->section_rides.size();
	success = true; co_return;
}

void PassengerPrecomputedTable::StartDestination()
{
	if (this->hyperpath) {
		this->strategy.events.Reset(); this->strategy.last = 0; this->strategy.board_edge = INVALID;
		this->strategy.witness_edge = INVALID;
		this->strategy.sums.clear(); this->strategy.settled.clear();
		this->prepared = {};
		this->prepared_candidate_bytes = 0;
	}
	this->queue.Reset();
	this->scratch.labels.clear();
	if (this->optimized) {
		this->search.cost.clear(); this->search.legs.clear();
		this->search.cost.reserve(this->diagnostic.states); this->search.legs.reserve(this->diagnostic.states);
		/* Infinite priorities make old payloads unobservable. Only successful
		 * relaxations/zero seeds write metadata; every row resets all hot keys. */
		this->search.metadata.resize(this->diagnostic.states);
	} else this->scratch.labels.reserve(this->incoming_offsets.size() - 1);
	this->scratch.alternative.assign(this->stations.size(), Label{});
	this->prepared.labels.clear(); this->prepared.labels.reserve(this->waiting_state_count);
	this->expanding = INVALID; this->finishing_row = false; this->finishing = 0;
	this->initializing_row = true; this->seeding = 0;
}

PassengerPrecomputedTable::Label PassengerPrecomputedTable::SearchLabel(uint32_t state) const
{
	if (!this->optimized) return this->scratch.labels[state];
	if (this->search.cost[state] == INT64_MAX) return {};
	const auto &m = this->search.metadata[state];
	return {this->search.cost[state], m.riding, m.waiting, m.first_wait, m.board, m.alight, this->search.legs[state], m.distance};
}

void PassengerPrecomputedTable::SetSearchLabel(uint32_t state, const Label &label)
{
	if (!this->optimized) { this->scratch.labels[state] = label; return; }
	this->search.cost[state] = label.cost; this->search.legs[state] = label.legs;
	this->search.metadata[state] = {label.riding, label.waiting, label.first_wait, label.board, label.alight, label.distance};
}

/** One descriptor is one original contiguous incoming-edge subsequence.
 * Each virtual relaxation costs exactly one Work unit. Excluded states were
 * never edges; skipping them is free. The base station always precedes waits.
 * Only the accepted states touch cold metadata, in original state order. */
size_t PassengerPrecomputedTable::RelaxIncoming(size_t allowance)
{
	size_t total = 0;
	/* Every edge is nonnegative in (cost, legs), so relaxing an incoming
	 * edge cannot improve the expanded state itself. Keep its priorities
	 * outside the descriptor loop, including at a saved slice boundary. */
	const int64_t source_cost = this->search.cost[this->expanding];
	const uint32_t source_legs = this->search.legs[this->expanding];
	do {
		const uint32_t index = static_cast<uint32_t>(this->cursor);
		const uint32_t range_id = this->hot_edges.range[index];
		const auto range = range_id == INVALID ? RangeEdge{0, 0, INVALID} : this->range_edges[range_id];
		const uint32_t count = 1 + range.end - range.begin;
		const int64_t edge_cost = this->hot_edges.cost[index];
		const uint32_t delta = this->hot_edges.legs[index];
		const bool finite = source_cost <= INT64_MAX - edge_cost && source_legs <= UINT32_MAX - delta;
		const int64_t cost = finite ? source_cost + edge_cost : INT64_MAX;
		const uint32_t legs = finite ? source_legs + delta : UINT32_MAX;
		size_t used = 0;
		Label next; bool materialized = false;
		while (this->range_offset < count && used < allowance) {
			uint32_t from = this->range_offset == 0 ? this->hot_edges.from[index] : range.begin + this->range_offset - 1;
			++this->range_offset;
			if (from == range.excluded) continue;
			++used;
			if (!finite || std::tuple(cost, legs) >= std::tuple(this->search.cost[from], this->search.legs[from])) continue;
			if (!materialized) {
				const auto &edge = this->cold_edges[index];
				next = this->SearchLabel(this->expanding); next.cost = cost; next.legs = legs;
				next.distance = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(next.distance) + edge.distance));
				if (edge.kind == EdgeKind::Board) {
					next.board = edge.occurrence;
					next.first_wait = this->patterns[this->occurrences[edge.occurrence].pattern].wait; next.waiting += next.first_wait;
				} else if (edge.kind == EdgeKind::Section) {
					const auto &ride = this->section_rides[edge.occurrence];
					next.board = ride.board; next.alight = ride.alight;
					next.first_wait = ride.waiting; next.waiting += ride.waiting; next.riding += ride.riding;
				} else if (edge.kind == EdgeKind::Ride) next.riding += edge_cost - edge.uncertainty;
				else if (edge.kind == EdgeKind::Alight) { next.board = INVALID; next.alight = edge.occurrence; next.waiting += edge_cost / 2; }
				materialized = true;
			}
			this->SetSearchLabel(from, next); this->queue.emplace(cost, legs, from);
		}
		/* An excluded final state consumes no work, including at a slice boundary. */
		if (this->range_offset < count && this->range_offset != 0 && range.begin + this->range_offset - 1 == range.excluded) ++this->range_offset;
		if (this->range_offset == count) { ++this->cursor; this->range_offset = 0; }
		assert(used > 0);
		total += used; allowance -= used;
	} while (allowance != 0 && this->cursor < this->incoming_offsets[this->expanding + 1]);
	return total;
}

size_t PassengerPrecomputedTable::Step(size_t allowance, bool legacy_publication, const std::function<void(StationID)> &published)
{
	if (!this->PrepareGraph(std::min<size_t>(allowance, 1024))) return 0;
	if (this->hyperpath) return this->StepHyperpath(allowance, published);
	const auto started = Clock::now(); size_t used = 0;
	auto index_origin = [&](size_t from) {
		if (from >= this->stations.size() || this->SearchCost(static_cast<uint32_t>(from)) == INT64_MAX) return;
		auto &destinations = this->reachable_destinations[from]; size_t capacity = destinations.capacity();
		destinations.push_back(this->stations[this->destination]);
		this->reachable_bytes += (destinations.capacity() - capacity) * sizeof(StationID);
	};
	try {
		while (used < allowance && !this->Finished()) {
			++used;
			if (this->initializing_row) {
				if (this->optimized) {
					if (this->search.cost.size() < this->diagnostic.states) {
						/* Resize only the budgeted prefix. Retained capacity is reused;
						 * no uncharged full-array reset occurs at a row boundary. */
						size_t count = this->integrated ? std::min(this->diagnostic.states - this->search.cost.size(), allowance - used + 1) : 1;
						this->search.cost.resize(this->search.cost.size() + count, INT64_MAX);
						this->search.legs.resize(this->search.legs.size() + count, 0);
						used += count - 1; continue;
					}
				} else if (this->scratch.labels.size() < this->diagnostic.states) { this->scratch.labels.emplace_back(); continue; }
				auto seed = [&](uint32_t state, uint32_t alight = INVALID) {
					auto label = this->SearchLabel(state); label.cost = 0; label.alight = alight; this->SetSearchLabel(state, label);
					this->queue.emplace(0, 0, state);
				};
				if (this->seeding == 0) {
					seed(static_cast<uint32_t>(this->destination));
					for (const auto &[pattern, state] : this->waiting_states[this->destination]) seed(state);
				}
				size_t seed_count = this->optimized ? this->seed_offsets[this->destination + 1] - this->seed_offsets[this->destination] : this->occurrences.size();
				if (this->seeding < seed_count) {
					uint32_t id = this->optimized ? this->seed_occurrences[this->seed_offsets[this->destination] + this->seeding++] : static_cast<uint32_t>(this->seeding++);
					const auto &o = this->occurrences[id];
					if (o.alight && o.station == this->stations[this->destination]) {
						seed(o.arrival, id);
						if (!this->landings.empty() && this->landings[id] != INVALID) seed(this->landings[id], id);
					}
				} else this->initializing_row = false;
				continue;
			}
			if (this->finishing_row) {
				if (this->finishing >= this->occurrences.size() && this->finishing - this->occurrences.size() < this->section_rides.size()) {
					const auto &ride = this->section_rides[this->finishing++ - this->occurrences.size()];
					const auto &board = this->occurrences[ride.board];
					uint32_t station = this->station_index[board.station.base()];
					int64_t best_cost = this->SearchCost(station);
					if (best_cost == INT64_MAX) continue;
					uint32_t best_board = this->SearchBoard(station);
					if (best_board == INVALID || this->occurrences[best_board].pattern == board.pattern) continue;
					uint32_t landing = this->landings[ride.alight];
					int64_t suffix_cost = this->SearchCost(landing);
					if (suffix_cost > INT64_MAX - ride.cost) continue;
					int64_t cost = suffix_cost + ride.cost;
					if (cost == INT64_MAX || cost - best_cost > best_cost / 4) continue;
					int64_t best_riding = this->SearchRiding(station);
					if (this->estimates && (this->SearchRiding(landing) + ride.riding > best_riding + std::max<int64_t>(32, best_riding / 4) ||
							this->SearchLegs(landing) + 1 > this->SearchLegs(station) + 1)) continue;
					auto &alt = this->scratch.alternative[station];
					if (cost < alt.cost) {
						alt = this->SearchLabel(landing); alt.board = ride.board; alt.alight = ride.alight; ++alt.legs;
						alt.cost = cost; alt.waiting += ride.waiting; alt.riding += ride.riding; alt.first_wait = ride.waiting;
					}
				} else if (this->finishing < this->occurrences.size()) {
					uint32_t id = static_cast<uint32_t>(this->finishing++); const auto &o = this->occurrences[id];
					if (this->optimized && !o.board) continue;
					uint32_t station = this->station_index[o.station.base()]; if (station == INVALID) continue;
					int64_t best_cost = this->SearchCost(station), suffix_cost = this->SearchCost(o.departure);
					if (best_cost == INT64_MAX || suffix_cost == INT64_MAX || suffix_cost >= best_cost) continue;
					uint32_t best_board = this->SearchBoard(station);
					const auto &p = this->patterns[o.pattern];
					if (best_board == INVALID || this->SearchAlight(o.departure) == INVALID ||
							this->occurrences[best_board].pattern == o.pattern || suffix_cost > INT64_MAX - p.wait - p.risk) continue;
					int64_t cost = suffix_cost + p.wait + p.risk;
					if (cost - best_cost > best_cost / 4) continue;
					/* Dispersion may pay a modest cost premium, but a waiting-time
					 * estimate alone must not randomly select a much longer ride or
					 * several extra transfers. Faster winning routes stay unrestricted. */
					int64_t best_riding = this->SearchRiding(station);
					if (this->estimates && (this->SearchRiding(o.departure) > best_riding + std::max<int64_t>(32, best_riding / 4) ||
							this->SearchLegs(o.departure) + 1 > this->SearchLegs(station) + 1)) continue;
					/* Only real Board edges are alternatives; NoLoad stops never enter. */
					if (!o.board) continue;
					auto &alt = this->scratch.alternative[station];
					if (cost < alt.cost) {
						alt = this->SearchLabel(o.departure); alt.board = id; ++alt.legs;
						alt.cost = cost; alt.waiting += p.wait; alt.first_wait = p.wait;
					}
				} else {
					/* Arrival/departure labels are construction scratch. Queries use
					 * only initial/transfer waits and one complete dispersion label. */
					if (legacy_publication) {
						if (this->optimized) {
							for (uint32_t state = 0; state < this->waiting_state_count; ++state) this->prepared.labels.push_back(this->SearchLabel(state));
						} else this->prepared.labels.assign(this->scratch.labels.begin(), this->scratch.labels.begin() + this->waiting_state_count);
						for (size_t from = 0; from < this->stations.size(); ++from) index_origin(from);
					} else if (this->prepared.labels.size() < this->waiting_state_count) {
						/* Charge inverse-index construction to the existing label-copy
						 * slice, rather than visiting all stations at publication. */
						index_origin(this->prepared.labels.size());
						this->prepared.labels.push_back(this->SearchLabel(static_cast<uint32_t>(this->prepared.labels.size()))); continue;
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
				if (cost != this->SearchCost(state) || legs != this->SearchLegs(state)) continue;
				this->expanding = state; this->cursor = this->incoming_offsets[state]; this->range_offset = 0;
			} else if (this->cursor == this->incoming_offsets[this->expanding + 1]) {
				this->expanding = INVALID;
			} else if (this->integrated) {
				used += this->RelaxIncoming(allowance - used + 1) - 1;
			} else {
				uint32_t index = static_cast<uint32_t>(this->cursor++);
				const auto &edge = this->edges[this->optimized ? index : this->incoming[index]];
				int64_t cost = this->SearchCost(edge.to);
				if (cost > INT64_MAX - edge.cost) continue;
				cost += edge.cost;
				uint32_t legs = this->SearchLegs(edge.to) + (edge.kind == EdgeKind::Board || edge.kind == EdgeKind::Section);
				if (std::tuple(cost, legs) >= std::tuple(this->SearchCost(edge.from), this->SearchLegs(edge.from))) continue;
				Label next = this->SearchLabel(edge.to);
				next.cost = cost; next.legs = legs;
				next.distance = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(next.distance) + edge.distance));
				if (edge.kind == EdgeKind::Board) {
					next.board = edge.occurrence;
					next.first_wait = this->patterns[this->occurrences[edge.occurrence].pattern].wait; next.waiting += next.first_wait;
				}
				if (edge.kind == EdgeKind::Section) {
					const auto &ride = this->section_rides[edge.occurrence];
					next.board = ride.board; next.alight = ride.alight;
					next.first_wait = ride.waiting; next.waiting += ride.waiting; next.riding += ride.riding;
				}
				if (edge.kind == EdgeKind::Ride) next.riding += edge.cost - edge.uncertainty;
				if (edge.kind == EdgeKind::Alight) { next.board = INVALID; next.alight = edge.occurrence; next.waiting += edge.cost / 2; }
				this->SetSearchLabel(edge.from, next); this->queue.emplace(next.cost, next.legs, edge.from);
			}
		}
	} catch (const std::bad_alloc &) { FatalError("Insufficient memory for passenger routing. Disable passenger_journey_precomputed or free memory before resuming."); }
	this->diagnostic.operations += used; this->diagnostic.build_nanoseconds += Elapsed(started);
	this->diagnostic.bytes = this->station_index.capacity() * 4 + this->service_pattern.capacity() * 4 + this->edges.capacity() * sizeof(Edge) +
			(this->incoming.capacity() + this->incoming_offsets.capacity()) * 4 + this->rows.size() * this->diagnostic.stored_states * sizeof(Label) +
			(this->scratch.labels.capacity() + this->scratch.alternative.capacity() + this->prepared.labels.capacity()) * sizeof(Label) +
			this->section_rides.capacity() * sizeof(SectionRide) + this->landings.capacity() * sizeof(uint32_t);
	this->diagnostic.bytes += this->hot_edges.from.capacity() * 4 + this->hot_edges.cost.capacity() * 8 + this->hot_edges.legs.capacity() + this->hot_edges.range.capacity() * 4 + this->range_edges.capacity() * sizeof(RangeEdge) + this->cold_edges.capacity() * sizeof(ColdEdge);
	this->diagnostic.bytes += this->binding_bytes + this->reachable_bytes;
	this->diagnostic.bytes += (this->seed_offsets.capacity() + this->seed_occurrences.capacity()) * sizeof(uint32_t);
	this->diagnostic.bytes += this->queue.AllocatedBytes();
	this->diagnostic.bytes += this->search.cost.capacity() * sizeof(int64_t) + this->search.legs.capacity() * sizeof(uint32_t) + this->search.metadata.capacity() * sizeof(LabelMetadata);
	return used;
}

void PassengerPrecomputedTable::PushHyperpath(int64_t key, uint32_t kind, uint32_t id)
{
	assert(key >= this->strategy.last);
	if (key == INT64_MAX) return;
	this->strategy.events.emplace(key, kind, id); ++this->diagnostic.heap_pushes;
}

/** Work units: one state initialization, seed, heap pop, incoming descriptor
 * (including generating its Board event), range target, copied label or copied
 * candidate. Row publication and each state-set header also cost one unit.
 * All cursors survive arbitrary slice boundaries; replay uses the saved total.
 * A v14 continuation-witness prefix stop also consumes one Work unit.
 * Event order is (ceil cost, Board/min/strategy, stable edge/state rank). */
size_t PassengerPrecomputedTable::StepHyperpath(size_t allowance, const std::function<void(StationID)> &published)
{
	const auto started = Clock::now(); size_t used = 0;
	try {
		while (used < allowance && !this->Finished()) {
			++used;
			if (this->initializing_row) {
				if (this->search.cost.size() < this->diagnostic.states) {
					this->search.cost.push_back(INT64_MAX); this->search.legs.push_back(0);
					this->strategy.settled.push_back(false);
					if (this->strategy.sums.size() < this->waiting_state_count) {
						this->strategy.sums.emplace_back();
						if (this->strategy.candidates.size() < this->strategy.sums.size()) this->strategy.candidates.emplace_back();
						else this->strategy.candidates[this->strategy.sums.size() - 1].clear();
					}
					continue;
				}
				const auto &waits = this->waiting_states[this->destination];
				const size_t seeds = this->seed_offsets[this->destination + 1] - this->seed_offsets[this->destination];
				uint32_t state, alight = INVALID;
				if (this->seeding == 0) state = static_cast<uint32_t>(this->destination);
				else if (this->seeding <= waits.size()) state = waits[this->seeding - 1].second;
				else if (this->seeding <= waits.size() + seeds) {
					alight = this->seed_occurrences[this->seed_offsets[this->destination] + this->seeding - waits.size() - 1];
					state = this->occurrences[alight].arrival;
				} else { this->initializing_row = false; continue; }
				++this->seeding;
				Label label; label.cost = 0; label.alight = alight;
				this->SetSearchLabel(state, label); this->PushHyperpath(0, 1, state);
				continue;
			}
			if (this->finishing_row) {
				if (this->prepared.labels.size() < this->waiting_state_count) {
					uint32_t state = static_cast<uint32_t>(this->prepared.labels.size());
					this->prepared.labels.push_back(this->SearchLabel(state));
					if (state < this->stations.size() && this->SearchCost(state) != INT64_MAX) {
						auto &destinations = this->reachable_destinations[state]; size_t capacity = destinations.capacity();
						destinations.push_back(this->stations[this->destination]); this->reachable_bytes += (destinations.capacity() - capacity) * sizeof(StationID);
					}
					continue;
				}
				if (this->finishing < this->waiting_state_count) {
					uint32_t state = static_cast<uint32_t>(this->finishing);
					const auto &candidates = this->strategy.candidates[state];
					if (this->strategy.offset == 0) {
						++this->diagnostic.candidate_histogram[candidates.size()];
						uint32_t base = this->waiting_station[state];
						if (base != state && candidates == this->strategy.candidates[base] && this->SearchCost(state) == this->SearchCost(base)) {
							this->prepared.policy_index.push_back(this->prepared.policy_index[base]); ++this->finishing; continue;
						}
						this->prepared.policy_index.push_back(static_cast<uint32_t>(this->prepared.policies.size()));
						this->prepared.policies.push_back({this->SearchCost(state), this->strategy.sums[state].Wait(), {}});
						this->prepared.policies.back().candidates.reserve(candidates.size());
						this->prepared_candidate_bytes += this->prepared.policies.back().candidates.capacity() * 4;
						++this->strategy.offset; continue;
					}
					if (this->strategy.offset <= candidates.size()) {
						uint32_t board = candidates[this->strategy.offset++ - 1];
						assert(this->EvaluateRide(this->prepared, board, this->stations[this->destination]).cost ==
								this->SearchCost(this->occurrences[board].departure) + this->patterns[this->occurrences[board].pattern].risk);
						this->prepared.policies.back().candidates.push_back(board); continue;
					}
					++this->diagnostic.policy_count; this->diagnostic.candidate_count += candidates.size();
					this->diagnostic.max_candidates = std::max(this->diagnostic.max_candidates, candidates.size());
					auto bytes = sizeof(Policy) + candidates.size() * 4;
					if (state < this->stations.size()) this->diagnostic.base_policy_bytes += bytes;
					else this->diagnostic.difference_policy_bytes += bytes;
					++this->finishing; this->strategy.offset = 0; continue;
				}
				this->prepared.bytes = this->prepared.labels.capacity() * sizeof(Label) + this->prepared.policy_index.capacity() * 4 +
						this->prepared.policies.capacity() * sizeof(Policy) + this->prepared_candidate_bytes;
				this->hyperpath_row_bytes += this->prepared.bytes;
				this->rows.push_back(std::move(this->prepared));
				this->prepared_candidate_bytes = 0;
				if (published) published(this->stations[this->destination]);
				++this->destination; this->diagnostic.destinations = this->destination;
				if (!this->Finished()) this->StartDestination();
				continue;
			}
			if (this->strategy.witness_edge != INVALID) {
				/* One prefix stop is one Work unit. Costs alone cannot discard a
				 * tied, earlier alighting. Certify that every optimal first alighting
				 * lies beyond b's departure, using the same frozen suffix labels as
				 * EvaluateRide. Unsettled suffixes cannot tie: travel is positive and
				 * their priorities are at least the already popped Board key. */
				uint32_t edge = this->strategy.witness_edge, board = this->cold_edges[edge].occurrence;
				const auto &first = this->occurrences[board]; const auto &stops = this->patterns[first.pattern].occurrences;
				const auto &from = this->occurrences[this->strategy.witness_at];
				int64_t step = from.travel + from.uncertainty;
				if (this->strategy.witness_at != board) step += from.dwell;
				this->strategy.witness_cost += step; // Bounded by the precomputed, checked departure-to-departure delta.
				this->strategy.witness_at = this->strategy.witness_at == stops.back() ? stops.front() : this->strategy.witness_at + 1;
				const auto &to = this->occurrences[this->strategy.witness_at];
				bool earlier = false;
				if (to.alight) {
					int64_t suffix = to.station == this->stations[this->destination] ? 0 : this->SearchCost(this->StateFor(this->station_index[to.station.base()], first.pattern));
					int64_t alight = to.station == this->stations[this->destination] ? 0 : this->transfer_cost;
					if (suffix <= INT64_MAX - alight && suffix + alight <= INT64_MAX - this->strategy.witness_cost)
						earlier = suffix + alight + this->strategy.witness_cost <= this->SearchCost(first.departure);
				}
				if (earlier || this->strategy.witness_at == this->revisits[board].next) {
					if (earlier || !this->HasSameRunContinuationWitness(board, this->SearchAlight(first.departure))) {
						this->strategy.board_edge = edge; this->strategy.offset = 0;
					}
					this->strategy.witness_edge = INVALID;
				}
				continue;
			}
			if (this->strategy.board_edge != INVALID) {
				uint32_t edge = this->strategy.board_edge, r = this->hot_edges.range[edge];
				auto range = r == INVALID ? RangeEdge{0, 0, INVALID} : this->range_edges[r];
				uint32_t from = this->strategy.offset == 0 ? this->hot_edges.from[edge] : range.begin + this->strategy.offset - 1;
				++this->strategy.offset;
				if (from != range.excluded && !this->strategy.settled[from]) {
					++this->diagnostic.range_updates;
					uint32_t board = this->cold_edges[edge].occurrence;
					const auto &o = this->occurrences[board]; const auto &pattern = this->patterns[o.pattern];
					auto label = this->SearchLabel(o.departure);
					int64_t x = label.cost <= INT64_MAX - pattern.risk ? label.cost + pattern.risk : INT64_MAX;
					auto &sum = this->strategy.sums[from];
					if (sum.Add(x, PassengerStrategy::Frequency(pattern.wait))) {
						auto &set = this->strategy.candidates[from];
						size_t capacity = set.capacity();
						set.insert(std::lower_bound(set.begin(), set.end(), board), board);
						this->hyperpath_candidate_bytes += (set.capacity() - capacity) * 4;
						++this->diagnostic.board_accepts;
						if (set.size() == 1) { label.board = board; ++label.legs; label.waiting += sum.Wait(); }
						else { label = this->SearchLabel(from); label.waiting += sum.Wait() - label.first_wait; }
						label.cost = sum.Cost(); label.first_wait = sum.Wait();
						this->SetSearchLabel(from, label); this->PushHyperpath(label.cost, 2, from);
					}
				}
				if (this->strategy.offset == 1 + range.end - range.begin) { this->strategy.board_edge = INVALID; this->strategy.offset = 0; }
				continue;
			}
			if (this->expanding != INVALID) {
				if (this->cursor == this->incoming_offsets[this->expanding + 1]) { this->expanding = INVALID; continue; }
				uint32_t edge = static_cast<uint32_t>(this->cursor++);
				const auto &cold = this->cold_edges[edge];
				if (cold.kind == EdgeKind::Board) {
					int64_t cost = this->SearchCost(this->expanding), risk = this->patterns[this->occurrences[cold.occurrence].pattern].risk;
					if (cost <= INT64_MAX - risk) { this->PushHyperpath(cost + risk, 0, edge); ++this->diagnostic.board_events; }
					continue;
				}
				assert(cold.kind != EdgeKind::Section);
				uint32_t from = this->hot_edges.from[edge]; int64_t cost = this->hot_edges.cost[edge];
				if (this->strategy.settled[from] || this->SearchCost(this->expanding) > INT64_MAX - cost) continue;
				auto label = this->SearchLabel(this->expanding); label.cost += cost;
				if (label.cost >= this->SearchCost(from)) continue;
				label.distance = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(label.distance) + cold.distance));
				if (cold.kind == EdgeKind::Ride) label.riding += cost - cold.uncertainty;
				if (cold.kind == EdgeKind::Alight) { label.board = INVALID; label.alight = cold.occurrence; label.waiting += cost / 2; }
				this->SetSearchLabel(from, label); this->PushHyperpath(label.cost, 1, from); continue;
			}
			if (this->strategy.events.empty()) { this->finishing_row = true; this->strategy.offset = 0; continue; }
			auto [key, kind, id] = this->strategy.events.top(); this->strategy.events.pop();
			++this->diagnostic.heap_pops; this->strategy.last = key;
			if (kind == 0) {
				uint32_t board = this->cold_edges[id].occurrence;
				if (!this->revisits.empty() && this->revisits[board].next != INVALID && this->DominatedRevisit(board,
						this->SearchCost(this->occurrences[board].departure), this->SearchCost(this->occurrences[this->revisits[board].next].departure))) {
					if (!this->continuation_witness) continue; // Exact v13 event replay.
					if (this->strategy.settled[this->occurrences[board].departure] && this->strategy.settled[this->occurrences[this->revisits[board].next].departure]) {
						this->strategy.witness_edge = id; this->strategy.witness_at = board; this->strategy.witness_cost = 0; continue;
					}
				}
				this->strategy.board_edge = id; this->strategy.offset = 0; continue;
			}
			if (this->strategy.settled[id] || key != this->SearchCost(id)) continue;
			this->strategy.settled[id] = true;
			this->expanding = id; this->cursor = this->incoming_offsets[id];
		}
	} catch (const std::bad_alloc &) { FatalError("Insufficient memory for passenger strategies."); }
	this->diagnostic.operations += used; this->diagnostic.build_nanoseconds += Elapsed(started);
	this->diagnostic.bytes = this->hyperpath_static_bytes + this->hyperpath_row_bytes + this->hyperpath_candidate_bytes + this->local_policy_bytes +
			this->reachable_bytes + this->reachable_destinations.capacity() * sizeof(this->reachable_destinations[0]) + this->queue.AllocatedBytes() +
			(this->scratch.labels.capacity() + this->scratch.alternative.capacity()) * sizeof(Label) +
			this->search.cost.capacity() * 8 + this->search.legs.capacity() * 4 + this->search.metadata.capacity() * sizeof(LabelMetadata) +
			this->strategy.sums.capacity() * sizeof(PassengerStrategy) + this->strategy.candidates.capacity() * sizeof(this->strategy.candidates[0]) +
			this->strategy.settled.capacity() / 8 + this->strategy.events.AllocatedBytes() + this->prepared_candidate_bytes +
			this->prepared.labels.capacity() * sizeof(Label) + this->prepared.policy_index.capacity() * 4 + this->prepared.policies.capacity() * sizeof(Policy);
	return used;
}

PassengerHyperpathMemoryDiagnostic PassengerPrecomputedTable::InspectMemory() const
{
	PassengerHyperpathMemoryDiagnostic result;
	if (!this->hyperpath) return result;
	result.reported = this->diagnostic.bytes; result.graph = this->hyperpath_static_bytes; result.local = this->local_policy_bytes;
	auto inspect = [&](const Row &row, bool differences) {
		result.labels += row.labels.capacity() * sizeof(Label);
		result.indices += row.policy_index.capacity() * sizeof(uint32_t);
		result.policy_headers += row.policies.capacity() * sizeof(Policy);
		for (const auto &policy : row.policies) result.candidate_arrays += policy.candidates.capacity() * sizeof(uint32_t);
		if (!differences || row.policy_index.empty()) return;
		for (uint32_t station = 0; station < this->waiting_states.size(); ++station) for (const auto &[previous, state] : this->waiting_states[station]) {
			if (row.policy_index[state] == row.policy_index[station]) continue;
			++result.differences;
			const auto &base = row.policies[row.policy_index[station]].candidates;
			const auto &variant = row.policies[row.policy_index[state]].candidates;
			uint64_t additions = 0;
			for (uint32_t board : variant) additions += !std::binary_search(base.begin(), base.end(), board);
			result.additions += additions; result.maximum_additions = std::max(result.maximum_additions, additions);
			result.multi_addition_differences += additions > 1;
			for (uint32_t board : base) if (this->occurrences[board].pattern != previous && !std::binary_search(variant.begin(), variant.end(), board)) ++result.unexpected_removals;
		}
	};
	for (const auto &row : this->rows) inspect(row, true);
	inspect(this->prepared, false);
	/* Cost + wait + an offset/count pair for a flat addition array. Previous
	 * service comes from the waiting state. This is a representation estimate,
	 * excluding allocator overhead and any cache needed by span-based callers. */
	result.overlay_bytes = result.differences * 24 + result.additions * 4;
	return result;
}

void PassengerPrecomputedTable::ReleaseDestination(StationID destination)
{
	if (this->hyperpath) {
		for (auto it = this->local_policies.begin(); it != this->local_policies.end();) {
			if (std::get<0>(it->first) == destination) { this->local_policy_bytes -= 128 + it->second.candidates.capacity() * 4; it = this->local_policies.erase(it); } else ++it;
		}
	}
	if (destination.base() >= this->station_index.size()) return;
	uint32_t row = this->station_index[destination.base()];
	if (row < this->rows.size() && !this->rows[row].labels.empty()) {
		this->hyperpath_row_bytes -= this->rows[row].bytes;
		this->diagnostic.bytes -= std::min(this->diagnostic.bytes,
				this->hyperpath ? this->rows[row].bytes : (this->rows[row].labels.capacity() + this->rows[row].alternative.capacity()) * sizeof(Label));
		this->rows[row] = {};
		for (auto &destinations : this->reachable_destinations) {
			auto found = std::lower_bound(destinations.begin(), destinations.end(), destination);
			if (found != destinations.end() && *found == destination) destinations.erase(found);
		}
		--this->diagnostic.destinations;
	}
}

std::span<const StationID> PassengerPrecomputedTable::ReachableDestinations(StationID origin) const
{
	if (origin.base() >= this->station_index.size()) return {};
	uint32_t from = this->station_index[origin.base()];
	if (from >= this->reachable_destinations.size()) return {};
	auto result = std::span<const StationID>(this->reachable_destinations[from]);
	/* A sliced copy can have staged one entry for the still-private row. */
	if (this->destination >= this->rows.size() && this->destination < this->stations.size() && !result.empty() && result.back() == this->stations[this->destination]) result = result.first(result.size() - 1);
	return result;
}

std::optional<int64_t> PassengerPrecomputedTable::RouteCost(StationID origin, StationID destination) const
{
	if (this->failed || origin.base() >= this->station_index.size() || destination.base() >= this->station_index.size()) return {};
	uint32_t from = this->station_index[origin.base()], to = this->station_index[destination.base()];
	if (from == INVALID || to >= this->rows.size() || this->rows[to].labels.empty()) return {};
	return this->rows[to].labels[from].cost;
}

const PassengerPrecomputedTable::Row *PassengerPrecomputedTable::FindRow(StationID destination) const
{
	if (!this->hyperpath || this->failed || destination.base() >= this->station_index.size()) return nullptr;
	uint32_t row = this->station_index[destination.base()];
	return row < this->rows.size() && !this->rows[row].policy_index.empty() ? &this->rows[row] : nullptr;
}

uint32_t PassengerPrecomputedTable::PolicyState(StationID origin, OrderListID previous) const
{
	if (origin.base() >= this->station_index.size()) return INVALID;
	uint32_t station = this->station_index[origin.base()];
	if (station == INVALID) return INVALID;
	uint32_t pattern = previous.base() < this->service_pattern.size() ? this->service_pattern[previous.base()] : INVALID;
	return pattern == INVALID ? station : this->StateFor(station, pattern);
}

uint32_t PassengerPrecomputedTable::BoardingOccurrence(StationID origin, OrderListID service, VehicleOrderID order) const
{
	if (!this->hyperpath || service.base() >= this->service_pattern.size()) return INVALID;
	uint32_t pattern = this->service_pattern[service.base()];
	if (pattern == INVALID) return INVALID;
	const auto &stops = this->patterns[pattern].occurrences;
	auto at = std::lower_bound(stops.begin(), stops.end(), order, [&](uint32_t id, VehicleOrderID value) { return this->occurrences[id].order < value; });
	return at != stops.end() && this->occurrences[*at].order == order && this->occurrences[*at].station == origin && this->occurrences[*at].board ? *at : INVALID;
}

std::optional<bool> PassengerPrecomputedTable::ContainsCached(StationID origin, StationID destination, uint32_t occurrence, OrderListID previous, OrderListID failed) const
{
	const auto *row = this->FindRow(destination); uint32_t state = this->PolicyState(origin, previous);
	if (row == nullptr || state == INVALID || occurrence >= this->occurrences.size()) return false;
	const auto &board = this->occurrences[occurrence];
	if (board.station != origin || this->patterns[board.pattern].members.front().id == previous) return false;
	std::span<const uint32_t> candidates;
	if (failed != OrderListID::Invalid()) {
		auto found = this->local_policies.find({destination, state, failed});
		/* A cold/evicted local policy is unknown, not a negative membership.
		 * Its deterministic construction belongs to the selected FIFO cohort.
		 * Enumerating an unrelated cohort must never run EvaluateRide. */
		if (found == this->local_policies.end()) return {};
		candidates = found->second.candidates;
	} else {
		candidates = row->policies[row->policy_index[state]].candidates;
	}
	return std::binary_search(candidates.begin(), candidates.end(), occurrence);
}

std::span<const uint32_t> PassengerPrecomputedTable::Candidates(StationID origin, StationID destination, OrderListID previous, OrderListID failed) const
{
	const auto *row = this->FindRow(destination); uint32_t state = this->PolicyState(origin, previous);
	if (row == nullptr || state == INVALID) return {};
	if (failed != OrderListID::Invalid()) return this->LocalPolicy(*row, destination, state, failed).candidates;
	return row->policies[row->policy_index[state]].candidates;
}

const PassengerPrecomputedTable::Policy &PassengerPrecomputedTable::LocalPolicy(const Row &row, StationID destination, uint32_t state, OrderListID failed) const
{
	auto key = std::tuple(destination, state, failed);
	auto found = this->local_policies.find(key);
	if (found != this->local_policies.end()) return found->second;
	const auto started = Clock::now();
	std::vector<std::pair<int64_t, uint32_t>> choices;
	for (uint32_t board : this->station_boarding[this->waiting_station[state]]) {
		const auto &o = this->occurrences[board];
		if (state != this->waiting_station[state] && this->StateFor(this->waiting_station[state], o.pattern) == state) continue;
		auto label = this->EvaluateRide(row, board, destination);
		if (!this->revisits.empty() && this->revisits[board].next != INVALID && this->DominatedRevisit(board,
				label.cost, this->EvaluateRide(row, this->revisits[board].next, destination).cost) &&
				(!this->continuation_witness || this->HasSameRunContinuationWitness(board, label.alight))) continue;
		if (label.cost != INT64_MAX) choices.emplace_back(label.cost, board);
	}
	std::sort(choices.begin(), choices.end());
	Policy policy;
	auto compute = [&](bool exclude) {
		PassengerStrategy sum; policy.candidates.clear();
		for (auto [cost, board] : choices) {
			const auto &p = this->patterns[this->occurrences[board].pattern];
			if (exclude && p.members.front().id == failed) continue;
			if (sum.Add(cost, PassengerStrategy::Frequency(p.wait))) policy.candidates.push_back(board);
		}
		policy.cost = sum.Cost(); policy.wait = sum.Wait();
	};
	compute(true);
	if (policy.candidates.empty()) compute(false); // Preserve reachability when there is no alternative.
	std::sort(policy.candidates.begin(), policy.candidates.end());
	if (this->local_policies.size() >= 4096) { this->local_policy_bytes -= 128 + this->local_policies.begin()->second.candidates.capacity() * 4; this->local_policies.erase(this->local_policies.begin()); }
	this->local_policy_bytes += 128 + policy.candidates.capacity() * 4;
	++this->diagnostic.local_replans; this->diagnostic.local_replan_nanoseconds += Elapsed(started);
	return this->local_policies.emplace(key, std::move(policy)).first->second;
}

bool PassengerPrecomputedTable::DominatedRevisit(uint32_t board, int64_t cost, int64_t next_cost) const
{
	const auto &revisit = this->revisits[board];
	return cost != INT64_MAX && next_cost != INT64_MAX && next_cost <= INT64_MAX - revisit.cost &&
			(this->continuation_witness ? cost == next_cost + revisit.cost : cost >= next_cost + revisit.cost);
}

bool PassengerPrecomputedTable::HasSameRunContinuationWitness(uint32_t board, uint32_t alight) const
{
	if (alight == INVALID || this->occurrences[alight].pattern != this->occurrences[board].pattern) return false;
	const auto &stops = this->patterns[this->occurrences[board].pattern].occurrences;
	uint32_t next = this->revisits[board].next;
	if (next == INVALID) return false;
	/* Pattern occurrence IDs are contiguous and ordered, including the circuit seam.
	 * Alighting at b's arrival does not witness continuation through its departure. */
	size_t to_alight = alight > board ? alight - board : stops.size() - (board - alight);
	size_t to_next = next > board ? next - board : stops.size() - (board - next);
	return to_alight > to_next;
}

/** The forward form of the same frozen Ride/Alight recurrence used by the
 * reverse search. Dwell belongs to continuing aboard, uncertainty to travel,
 * and risk to boarding. An actual destination seed has zero alighting cost.
 * Previous-service state, forced unloading, NoUnload and occurrence identity
 * are retained even if the search contracted unobservable intermediate nodes. */
PassengerPrecomputedTable::Label PassengerPrecomputedTable::EvaluateRide(const Row &row, uint32_t board, StationID destination) const
{
	const auto &first = this->occurrences[board]; const auto &pattern = this->patterns[first.pattern];
	const auto &stops = pattern.occurrences;
	auto start = std::lower_bound(stops.begin(), stops.end(), board);
	if (start == stops.end() || *start != board || !first.board) return {};
	size_t at = start - stops.begin(); int64_t cost = pattern.risk, riding = 0; uint32_t distance = 0;
	Label best;
	for (size_t step = 0; step < stops.size(); ++step) {
		const auto &from = this->occurrences[stops[at]];
		if (!from.continues || (step != 0 && from.forced)) break;
		if (step != 0) {
			if (cost > INT64_MAX - from.dwell) break;
			cost += from.dwell; riding += from.dwell;
		}
		if (cost > INT64_MAX - from.travel || cost + from.travel > INT64_MAX - from.uncertainty) break;
		cost += from.travel + from.uncertainty; riding += from.travel;
		distance = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(distance) + from.distance));
		at = (at + 1) % stops.size(); const auto &to = this->occurrences[stops[at]];
		if (!to.alight) continue;
		Label suffix = row.labels[this->StateFor(this->station_index[to.station.base()], first.pattern)];
		int64_t alight = this->transfer_cost;
		if (to.station == destination) { suffix = {}; suffix.cost = 0; alight = 0; }
		if (suffix.cost > INT64_MAX - alight || suffix.cost + alight > INT64_MAX - cost) continue;
		Label candidate = suffix; candidate.cost += alight + cost; ++candidate.legs;
		candidate.board = board; candidate.alight = stops[at]; candidate.riding += riding;
		candidate.distance = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(distance) + candidate.distance));
		candidate.waiting += alight / 2;
		bool earlier_tie = this->continuation_witness && !this->revisits.empty() && this->revisits[board].next != INVALID;
		if (earlier_tie ? candidate.cost < best.cost : std::tie(candidate.cost, candidate.legs) < std::tie(best.cost, best.legs)) best = candidate;
	}
	return best;
}

PassengerPrecomputedPlan PassengerPrecomputedTable::Board(StationID origin, StationID destination, StateTicks ready,
		OrderListID service, VehicleOrderID order, OrderListID previous, OrderListID failed) const
{
	const auto *row = this->FindRow(destination);
	if (row == nullptr) return {};
	if (service == previous || service.base() >= this->service_pattern.size()) return {PassengerPrecomputedPlan::State::NoRoute};
	uint32_t pattern = this->service_pattern[service.base()];
	if (pattern == INVALID) return {PassengerPrecomputedPlan::State::NoRoute};
	const auto &stops = this->patterns[pattern].occurrences;
	auto at = std::lower_bound(stops.begin(), stops.end(), order, [&](uint32_t id, VehicleOrderID value) { return this->occurrences[id].order < value; });
	const auto set = this->Candidates(origin, destination, previous, failed);
	if (at == stops.end() || this->occurrences[*at].order != order || this->occurrences[*at].station != origin || !std::binary_search(set.begin(), set.end(), *at)) return {PassengerPrecomputedPlan::State::NoRoute};
	auto label = this->EvaluateRide(*row, *at, destination);
	auto plan = this->Select(label, ready, previous, 0);
	plan.hyperpath = true;
	return plan;
}

std::vector<PassengerPrecomputedTable::CandidateDiagnostic> PassengerPrecomputedTable::Inspect(StationID origin, StationID destination, OrderListID previous) const
{
	std::vector<CandidateDiagnostic> result;
	const auto *row = this->FindRow(destination); if (row == nullptr) return result;
	for (uint32_t id : this->Candidates(origin, destination, previous)) {
		const auto &o = this->occurrences[id]; const auto &p = this->patterns[o.pattern]; auto label = this->EvaluateRide(*row, id, destination);
		result.push_back({id, p.members.front().id, o.order, label.alight == INVALID ? StationID::Invalid() : this->occurrences[label.alight].station, label.cost, p.wait, PassengerStrategy::Frequency(p.wait)});
	}
	return result;
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
	/* The saved service/order pair anchors a shared section; it does not
	 * restrict its carriers. SectionBoardingKey and Rebind recover the same
	 * complete path pool whose departures supplied the estimated wait. */
	ride.service = m.id; ride.signature = m.incarnation; ride.generation = m.generation;
	ride.boarding = board.station; ride.alighting = alight.station; ride.boarding_order = board.order; ride.alighting_order = alight.order;
	ride.kind = PassengerRideKind::EstimatedServiceIntent; ride.not_before = ready; ride.planned_at = ready;
	ride.estimated_wait = label.first_wait; ride.waiting_limit = std::min<int64_t>(INT64_MAX / 2, pattern.circuit) * 2;
	result.ride = ride; result.state = PassengerPrecomputedPlan::State::Ready;
	result.cost = label.cost; result.riding = label.riding; result.waiting = label.waiting; result.distance = label.distance; result.transfers = label.legs - 1;
	result.estimated_service = m.estimated;
	return result;
}

PassengerPrecomputedPlan PassengerPrecomputedTable::Plan(StationID origin, StationID destination, StateTicks ready, OrderListID excluded, uint32_t sample, OrderListID failed) const
{
	const auto started = Clock::now(); ++this->diagnostic.queries;
	auto lookup = [&]() -> PassengerPrecomputedPlan {
		if (this->graph_build) return {PassengerPrecomputedPlan::State::Building};
		if (this->failed) return {PassengerPrecomputedPlan::State::Memory};
		if (origin.base() >= this->station_index.size() || destination.base() >= this->station_index.size()) return {PassengerPrecomputedPlan::State::Unsupported};
		uint32_t from = this->station_index[origin.base()], to = this->station_index[destination.base()];
		if (from == INVALID || to == INVALID || from == to) return {this->unsupported ? PassengerPrecomputedPlan::State::Unsupported : PassengerPrecomputedPlan::State::NoRoute};
		if (to >= this->rows.size() || this->rows[to].labels.empty()) return {PassengerPrecomputedPlan::State::Building};
		uint32_t previous = excluded.base() < this->service_pattern.size() ? this->service_pattern[excluded.base()] : INVALID;
		uint32_t state = previous == INVALID ? from : this->StateFor(from, previous);
		const auto &row = this->rows[to]; Label label = row.labels[state];
		if (this->hyperpath) {
			const auto &policy = failed == OrderListID::Invalid() ? row.policies[row.policy_index[state]] : this->LocalPolicy(row, destination, state, failed);
			if (failed != OrderListID::Invalid() && !policy.candidates.empty()) {
				label = this->EvaluateRide(row, policy.candidates.front(), destination); label.cost = policy.cost; label.first_wait = policy.wait;
			}
			auto result = this->Select(label, ready, excluded, sample); result.hyperpath = true;
			if (result.ride) {
				PassengerJourneyExecution intent;
				intent.kind = PassengerRideKind::HyperpathIntent; intent.boarding = origin; intent.alighting = destination;
				intent.not_before = intent.planned_at = ready;
				/* Database replaces the standalone generation with the saved table
				 * generation. Signature encodes cargo+1, never a service incarnation. */
				intent.signature = uint64_t(this->cargo) + 1; intent.generation = 1;
				intent.estimated_wait = policy.wait;
				/* Effective headway is twice the frequency-model residual wait.
				 * k=3 gives wait+3*headway; this is an abnormal-service safety net. */
				intent.waiting_limit = std::clamp<int64_t>(policy.wait > (int64_t{1} << 22) / 7 ? int64_t{1} << 22 : 7 * policy.wait, 256, int64_t{1} << 22);
				result.ride = intent;
			}
			return result;
		}
		const auto &alternative = row.alternative[from];
		if (state == from && (!this->estimates || excluded == OrderListID::Invalid()) && label.board != INVALID && alternative.board != INVALID) {
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
