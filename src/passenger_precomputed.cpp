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
#include "thread.h"
#include <atomic>
#include <array>
#include <condition_variable>
#include <deque>
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
	return this->graph->StateFor(station, pattern);
}

OrderListID PassengerPrecomputedTable::BoardingKey(OrderListID service) const
{
	if (service.base() >= this->graph->service_pattern.size() || this->graph->service_pattern[service.base()] == INVALID) return service;
	return this->graph->patterns[this->graph->service_pattern[service.base()]].members.front().id;
}

std::span<const OrderListID> PassengerPrecomputedTable::BoardingServices(OrderListID service) const
{
	if (service.base() >= this->graph->service_pattern.size() || this->graph->service_pattern[service.base()] == INVALID) return {};
	return this->graph->patterns[this->graph->service_pattern[service.base()]].boarding_services;
}

uint64_t PassengerPrecomputedTable::RawSectionBoardingKey(const PassengerJourneyExecution &ride)
{
	return (uint64_t{1} << 63) | SectionPathKey(ride.service.base(), ride.boarding_order, ride.alighting_order);
}

const PassengerPrecomputedTable::Member *PassengerPrecomputedTable::FindMember(OrderListID service) const
{
	if (service.base() >= this->graph->service_pattern.size() || this->graph->service_pattern[service.base()] == INVALID) return nullptr;
	auto refreshed = this->refreshed_members.find(service);
	if (refreshed != this->refreshed_members.end()) return &refreshed->second;
	const auto &members = this->graph->patterns[this->graph->service_pattern[service.base()]].members;
	auto it = std::lower_bound(members.begin(), members.end(), service, [](const Member &member, OrderListID id) { return member.id < id; });
	return it != members.end() && it->id == service ? &*it : nullptr;
}

const PassengerPrecomputedTable::Section *PassengerPrecomputedTable::FindSection(const PassengerJourneyExecution &ride) const
{
	if (ride.kind != PassengerRideKind::EstimatedServiceIntent) return nullptr;
	const auto *member = this->FindMember(ride.service);
	if (member == nullptr || member->incarnation != ride.signature || member->generation != ride.generation) return nullptr;
	auto it = this->graph->section_paths.find(SectionPathKey(this->graph->service_pattern[ride.service.base()], ride.boarding_order, ride.alighting_order));
	if (it == this->graph->section_paths.end() || this->graph->occurrences[it->second.board].station != ride.boarding ||
			this->graph->occurrences[it->second.alight].station != ride.alighting) return nullptr;
	return &this->graph->sections[it->second.section];
}

std::optional<uint64_t> PassengerPrecomputedTable::SectionBoardingKey(const PassengerJourneyExecution &ride) const
{
	if (const auto *section = this->FindSection(ride)) return section->key;
	return std::nullopt;
}

std::span<const uint64_t> PassengerPrecomputedTable::SectionBoardingKeys(OrderListID service, VehicleOrderID boarding) const
{
	if (service.base() >= this->graph->service_pattern.size() || this->graph->service_pattern[service.base()] == INVALID || this->graph->boarding_sections.empty()) return {};
	const auto &stops = this->graph->patterns[this->graph->service_pattern[service.base()]].occurrences;
	auto it = std::lower_bound(stops.begin(), stops.end(), boarding, [&](uint32_t id, VehicleOrderID order) { return this->graph->occurrences[id].order < order; });
	return it != stops.end() && this->graph->occurrences[*it].order == boarding ? std::span<const uint64_t>(this->graph->boarding_sections[*it]) : std::span<const uint64_t>{};
}

bool PassengerPrecomputedTable::Rebind(PassengerJourneyExecution &ride, OrderListID service, VehicleOrderID boarding) const
{
	const auto *member = this->FindMember(service);
	const auto *original = this->FindMember(ride.service);
	if (member == nullptr || original == nullptr || original->incarnation != ride.signature || original->generation != ride.generation || IsBoundPassengerRide(ride.kind)) return false;
	uint32_t pattern = this->graph->service_pattern[service.base()];
	if (const auto *section = this->FindSection(ride)) {
		auto key = std::pair(pattern, boarding == INVALID_VEH_ORDER_ID ? VehicleOrderID{0} : boarding);
		auto it = std::lower_bound(section->bindings.begin(), section->bindings.end(), key, [&](const SectionBinding &binding, const auto &candidate) {
			const auto &stop = this->graph->occurrences[binding.board]; return std::pair(stop.pattern, stop.order) < candidate;
		});
		if (it == section->bindings.end() || this->graph->occurrences[it->board].pattern != pattern ||
				(boarding != INVALID_VEH_ORDER_ID && this->graph->occurrences[it->board].order != boarding)) return false;
		ride.boarding_order = this->graph->occurrences[it->board].order; ride.alighting_order = this->graph->occurrences[it->alight].order;
	} else if (pattern != this->graph->service_pattern[ride.service.base()] || (boarding != INVALID_VEH_ORDER_ID && ride.boarding_order != boarding)) {
		return false;
	}
	ride.service = service; ride.signature = member->incarnation; ride.generation = member->generation;
	return true;
}

bool PassengerPrecomputedTable::RefreshService(const PassengerPeriodicService &service)
{
	if (this->cargo >= NUM_CARGO || service.id.base() >= this->graph->service_pattern.size()) return false;
	uint32_t pattern = this->graph->service_pattern[service.id.base()];
	if (pattern == INVALID) return false;
	auto &p = this->graph->patterns[pattern];
	auto member = std::lower_bound(p.members.begin(), p.members.end(), service.id, [](const auto &m, OrderListID id) { return m.id < id; });
	if (member == p.members.end() || member->id != service.id || member->incarnation != service.incarnation ||
			service.timetable.Circuit() <= 0 || p.occurrences.size() != service.timetable.stops.size()) return false;
	if (!this->graph->estimates && p.estimated != (service.timetable.period == 0)) return false; // Reference binding without duration estimates.
	uint64_t mask = uint64_t{1} << this->cargo;
	for (size_t i = 0; i < p.occurrences.size(); ++i) {
		const auto &old = this->graph->occurrences[p.occurrences[i]];
		const auto &stop = service.timetable.stops[i];
		if (old.station != stop.station || old.order != stop.order || old.board != ((stop.board & mask) != 0) ||
				old.alight != ((stop.alight & mask) != 0) || old.forced != ((stop.forced & mask) != 0) || old.continues != stop.continues) return false;
	}
	auto binding = *member; binding.generation = service.generation; binding.estimated = service.timetable.period == 0;
	this->refreshed_members[service.id] = binding;
	return true;
}

void PassengerPrecomputedTable::AddEdge(uint32_t from, uint32_t to, int64_t cost, EdgeKind kind, uint32_t occurrence, uint32_t distance, int64_t uncertainty)
{
	assert(cost >= 0);
	this->building_graph->edges.push_back({from, to, occurrence, cost, distance, kind, uncertainty});
}

void PassengerPrecomputedTable::AddBoardEdges(uint32_t station, uint32_t to, uint32_t pattern, int64_t cost, EdgeKind kind, uint32_t occurrence, uint32_t distance)
{
	this->AddEdge(station, to, cost, kind, occurrence, distance);
	const auto &states = this->building_graph->waiting_states[station];
	if (states.empty()) return;
	uint32_t excluded = this->building_graph->patterns[pattern].members.size() == 1 ? this->StateFor(station, pattern) : INVALID;
	if (excluded == station) excluded = INVALID; // No waiting state exists for this previous pattern.
	this->building_graph->edges.back().range = static_cast<uint32_t>(this->building_graph->range_edges.size());
	this->building_graph->range_edges.push_back({states.front().second, states.back().second + 1, excluded});
}

void PassengerPrecomputedTable::Begin(std::span<const PassengerPeriodicService *const> services, CargoType cargo, int64_t transfer, uint64_t memory, bool sections, bool cooperative, std::shared_ptr<uint64_t> memory_budget, bool estimates, bool optimized, bool integrated, bool hyperpath, bool revisit_dominance, bool continuation_witness, bool flat_storage)
{
	*this = {}; this->building_graph = std::make_shared<GraphSnapshot>(); this->graph = this->building_graph; this->cargo = cargo; this->building_graph->estimates = estimates; this->building_graph->optimized = optimized; this->building_graph->integrated = optimized && integrated;
	this->building_graph->flat_storage = hyperpath && flat_storage;
	this->building_graph->hyperpath = hyperpath; this->building_graph->transfer_cost = transfer >= 0 && transfer <= INT64_MAX / 2 ? 2 * transfer : 0;
	this->building_graph->revisit_dominance = revisit_dominance;
	this->building_graph->continuation_witness = revisit_dominance && continuation_witness;
	if (hyperpath) { this->building_graph->optimized = this->building_graph->integrated = true; this->work.strategy.events.SetOptimized(true); }
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
	if (ready) { this->graph_build = {}; this->building_graph.reset(); }
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
	memory = std::min(memory, this->building_graph->hyperpath ? MAX_HYPERPATH_BYTES : MAX_BYTES);
	/* Canonical byte charges are identical on 32/64-bit peers. Native sizeof
	 * and allocator residency are diagnostics only, never routing admission. */
	static_assert(sizeof(Label) <= 64 && sizeof(Edge) <= 64 && sizeof(QueueItem) <= 32 && sizeof(Row) <= 256);
	/* The flat generation charges 512 bytes per row descriptor. Old replay
	 * retains its canonical charges despite the two additional empty vectors. */
	uint64_t envelope = 1024 * 1024;
	if (envelope > memory) { this->failed = true; co_return; }
	for (const auto *s : services) {
		if (cooperative) co_await std::suspend_always{};
		if (!Product(1, 256, envelope, memory) || !Product(s->timetable.stops.size(), 256, envelope, memory) ||
				!Product(s->vehicles.size(), this->building_graph->hyperpath ? 128 : 64, envelope, memory) || !Product(s->dispatch_slots.size(), 32, envelope, memory)) {
			this->failed = true; this->diagnostic.envelope = memory; co_return;
		}
	}
	try {
		this->work.queue.SetOptimized(this->building_graph->optimized);
		this->building_graph->station_index.assign(StationID::Invalid().base(), INVALID);
		this->building_graph->service_pattern.assign(OrderListID::Invalid().base(), INVALID);
		std::map<std::vector<int64_t>, std::vector<const PassengerPeriodicService *>> groups;
		std::set<StationID> station_ids;
		/* A vehicle moved between shared order lists can occur in two captures
		 * of a frozen publication. Count its newest captured ownership once;
		 * equal generations use the lower OrderList ID. Never consult live pools
		 * here, since replay must use the same immutable input on every peer. */
		std::map<VehicleID, std::pair<uint64_t, OrderListID>> owners;
		if (this->building_graph->hyperpath) for (const auto *s : services) for (const auto &v : s->vehicles) {
			if (cooperative) co_await std::suspend_always{};
			if (v.id == VehicleID::Invalid()) continue;
			auto [it, inserted] = owners.try_emplace(v.id, s->generation, s->id);
			if (!inserted && (s->generation > it->second.first || (s->generation == it->second.first && s->id < it->second.second))) it->second = {s->generation, s->id};
		}
		for (const auto *s : services) {
			if (cooperative) co_await std::suspend_always{};
			bool capacity = std::any_of(s->vehicles.begin(), s->vehicles.end(), [&](const auto &v) { return v.capacity[cargo] != 0 && (!this->building_graph->hyperpath || (owners.contains(v.id) && owners.at(v.id).second == s->id)); });
			if (!capacity) { this->unsupported |= s->timetable.period == 0; continue; }
			if ((this->building_graph->estimates ? s->timetable.Circuit() : s->timetable.period) <= 0 || s->timetable.stops.size() < 2) { this->unsupported = true; continue; }
			auto key = PatternKey(*s);
			/* A strategy distinguishes actual services, even when their stop
			 * patterns coincide. This makes exclusion remove exactly the arriving
			 * OrderList's frequency, while retaining its identical neighbours. */
			if (this->building_graph->hyperpath) key.push_back(s->id.base());
			groups[std::move(key)].push_back(s);
			for (const auto &stop : s->timetable.stops) if ((stop.board | stop.alight) & mask) station_ids.insert(stop.station);
		}
		this->building_graph->stations.assign(station_ids.begin(), station_ids.end());
		assert(std::is_sorted(this->building_graph->stations.begin(), this->building_graph->stations.end()));
		assert(std::adjacent_find(this->building_graph->stations.begin(), this->building_graph->stations.end()) == this->building_graph->stations.end());
		for (uint32_t i = 0; i < this->building_graph->stations.size(); ++i) this->building_graph->station_index[this->building_graph->stations[i].base()] = i;
		this->building_graph->waiting_states.resize(this->building_graph->stations.size());
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
				this->building_graph->service_pattern[s->id.base()] = static_cast<uint32_t>(this->building_graph->patterns.size());
				for (const auto &v : s->vehicles) if (v.capacity[cargo]) {
					if (cooperative) co_await std::suspend_always{};
					if (this->building_graph->hyperpath && (!owners.contains(v.id) || owners.at(v.id).second != s->id)) continue;
					if ((sections || this->building_graph->estimates) && !physical.insert(v.id).second) continue;
					seats = std::max(seats, v.capacity[cargo]); unknown |= !v.phase_known;
					unknown_fleet += !v.phase_known || p.estimated;
					if (this->building_graph->estimates && (!v.phase_known || p.estimated)) continue;
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
			if ((!unknown || this->building_graph->estimates) && !phases.empty()) {
				gap = 0;
				for (size_t i = 0; i < phases.size(); ++i) gap = std::max(gap, (i + 1 < phases.size() ? phases[i + 1] : p.circuit + phases.front()) - phases[i]);
			}
			/* Unknown phases retain a conservative circuit estimate. No departure
			 * timestamps or evenly spaced synthetic trains are generated. */
			p.wait = unknown ? p.circuit : (gap + 1) / 2;
			if (this->building_graph->estimates && (unknown || p.estimated)) {
				p.wait = UnknownWait(p.circuit, unknown_fleet, members.front()->uncertain_dispatch_period > 0);
				if (!phases.empty()) p.wait = std::min(p.wait, (gap + 1) / 2);
				gap = std::max<int64_t>(1, 2 * p.wait);
			}
			p.risk = (gap + std::min<uint64_t>(seats, 64) - 1) / std::min<uint64_t>(seats, 64);
			if (this->building_graph->estimates && (unknown || p.estimated)) p.risk += std::min<int64_t>(64, (p.wait + 7) / 8);
			p.weight = std::clamp<uint64_t>(std::min<uint64_t>(seats, 65535) * 65536 / std::max<int64_t>(1, gap), 1, UINT32_MAX / 2);
			uint32_t pattern = static_cast<uint32_t>(this->building_graph->patterns.size());
			for (const auto &stop : t.stops) {
				if (cooperative) co_await std::suspend_always{};
				p.occurrences.push_back(static_cast<uint32_t>(this->building_graph->occurrences.size()));
				this->building_graph->occurrences.push_back({stop.station, stop.order, pattern, 0, 0, (stop.board & mask) != 0, (stop.alight & mask) != 0, (stop.forced & mask) != 0, stop.continues});
				if (stop.alight & mask) this->building_graph->waiting_states[this->building_graph->station_index[stop.station.base()]].emplace_back(pattern, 0);
			}
			this->building_graph->patterns.push_back(std::move(p)); representatives.push_back(members.front());
			pattern_members.push_back(members);
		}
		size_t nodes = this->building_graph->stations.size();
		if (this->UseCompressedRows()) this->building_graph->exception_slots.assign(this->building_graph->stations.size(), INVALID);
		for (uint32_t station = 0; station < this->building_graph->waiting_states.size(); ++station) {
			auto &states = this->building_graph->waiting_states[station];
			if (cooperative) co_await std::suspend_always{};
			std::sort(states.begin(), states.end()); states.erase(std::unique(states.begin(), states.end()), states.end());
			for (auto &state : states) state.second = static_cast<uint32_t>(nodes++);
			if (this->UseCompressedRows() && !states.empty()) this->building_graph->exception_slots[station] = this->building_graph->exception_slot_count++;
		}
		this->building_graph->waiting_state_count = nodes;
		if (this->building_graph->hyperpath) {
			this->building_graph->waiting_station.resize(nodes);
			for (uint32_t s = 0; s < this->building_graph->stations.size(); ++s) {
				this->building_graph->waiting_station[s] = s;
				for (const auto &[p, state] : this->building_graph->waiting_states[s]) this->building_graph->waiting_station[state] = s;
			}
		}
		for (auto &o : this->building_graph->occurrences) { o.arrival = static_cast<uint32_t>(nodes++); o.departure = static_cast<uint32_t>(nodes++); }
		if (this->building_graph->hyperpath) {
			std::vector<uint64_t> frequency(this->building_graph->stations.size());
			this->building_graph->station_boarding.resize(this->building_graph->stations.size());
			uint64_t candidate_bound = 0;
			for (uint32_t id = 0; id < this->building_graph->occurrences.size(); ++id) if (this->building_graph->occurrences[id].board) {
				const auto &o = this->building_graph->occurrences[id];
				this->building_graph->station_boarding[this->building_graph->station_index[o.station.base()]].push_back(id);
				const auto wait = this->building_graph->patterns[o.pattern].wait;
				if (wait <= 0 || wait > PassengerStrategy::MAX_WAIT) { this->failed = true; co_return; }
				auto &sum = frequency[this->building_graph->station_index[o.station.base()]];
				uint64_t f = PassengerStrategy::Frequency(wait);
				if (f >= (uint64_t{1} << 63) - sum) { this->failed = true; co_return; }
				sum += f;
				candidate_bound += 1 + this->building_graph->waiting_states[this->building_graph->station_index[o.station.base()]].size();
			}
			size_t max_boarding = 0;
			for (const auto &boards : this->building_graph->station_boarding) max_boarding = std::max(max_boarding, boards.size());
			/* Full sparse sets, including worst-case differences; never truncate
			 * to K candidates. Admission is based solely on the frozen graph. */
			if ((this->building_graph->flat_storage && (candidate_bound >= INVALID || max_boarding >= FlatPolicy::FULL)) ||
					!(this->building_graph->flat_storage ? Product(4096, uint64_t{256} + uint64_t(max_boarding) * 16, envelope, memory) : Product(max_boarding, uint64_t{4096} * 64, envelope, memory)) ||
					!Product(candidate_bound, uint64_t(this->building_graph->stations.size()) * 8, envelope, memory) ||
					!Product(this->building_graph->waiting_state_count, uint64_t(this->building_graph->stations.size()) * (this->building_graph->flat_storage ? 64 : 96) + 128, envelope, memory)) {
				this->failed = true; this->diagnostic.envelope = memory; co_return;
			}
		}
		size_t edge_count = 0; uint64_t logical_edge_bound = 0;
		for (size_t p = 0; p < this->building_graph->patterns.size(); ++p) for (const auto &stop : representatives[p]->timetable.stops) {
			if (cooperative) co_await std::suspend_always{};
			size_t contribution = 2 + ((stop.alight & mask) != 0);
			if (stop.board & mask) contribution += 1 + this->building_graph->waiting_states[this->building_graph->station_index[stop.station.base()]].size();
			if (contribution > INVALID - edge_count) { this->failed = true; co_return; }
			logical_edge_bound += contribution;
			if (this->building_graph->integrated && (stop.board & mask)) contribution -= this->building_graph->waiting_states[this->building_graph->station_index[stop.station.base()]].size();
			edge_count += contribution;
		}
		/* Every product is checked before route/scratch allocation. Queue storage
		 * is bounded by successful edge relaxations, never by passengers or OD calls. */
		this->diagnostic.stations = this->building_graph->stations.size(); this->diagnostic.patterns = this->building_graph->patterns.size();
		this->diagnostic.states = nodes; this->diagnostic.edges = edge_count;
		this->diagnostic.stored_states = this->building_graph->waiting_state_count + this->building_graph->stations.size();
		bool fits = nodes < INVALID && edge_count < INVALID && nodes <= memory / 128 &&
				Product(this->building_graph->stations.size(), uint64_t(this->diagnostic.stored_states) * 64 + (this->building_graph->flat_storage ? 512 : 128), envelope, memory) &&
				Product(this->building_graph->stations.size(), uint64_t(this->building_graph->stations.size()) * 8 + 64, envelope, memory) &&
				Product(edge_count, this->building_graph->integrated ? 128 : 264, envelope, memory) &&
				(!this->building_graph->integrated || Product(logical_edge_bound, 128, envelope, memory)) && Product(nodes, this->building_graph->integrated ? 160 : 144, envelope, memory);
		this->diagnostic.envelope = envelope;
		if (!fits) { this->failed = true; co_return; }
		this->building_graph->edges.reserve(edge_count);
		this->reachable_destinations.resize(this->building_graph->stations.size());
		for (uint32_t p = 0; p < this->building_graph->patterns.size(); ++p) {
			if (cooperative) co_await std::suspend_always{};
			const auto &pattern = this->building_graph->patterns[p]; const auto &t = representatives[p]->timetable;
			for (size_t i = 0; i < t.stops.size(); ++i) {
				if (cooperative) co_await std::suspend_always{};
				const auto &stop = t.stops[i]; uint32_t id = pattern.occurrences[i]; auto &o = this->building_graph->occurrences[id];
				o.dwell = stop.departure - stop.arrival;
				if (stop.board & mask) {
					uint32_t station = this->building_graph->station_index[stop.station.base()];
					if (this->building_graph->integrated) this->AddBoardEdges(station, o.departure, p, pattern.wait + pattern.risk, EdgeKind::Board, id);
					else {
					this->AddEdge(station, o.departure, pattern.wait + pattern.risk, EdgeKind::Board, id);
					for (const auto &[previous, state] : this->building_graph->waiting_states[station]) {
						if (cooperative) co_await std::suspend_always{};
						if (previous != p || pattern.members.size() > 1) this->AddEdge(state, o.departure, pattern.wait + pattern.risk, EdgeKind::Board, id);
					}
					}
				}
				if (stop.alight & mask) this->AddEdge(o.arrival, this->StateFor(this->building_graph->station_index[stop.station.base()], p), 2 * transfer, EdgeKind::Alight, id);
				if (!(stop.forced & mask)) this->AddEdge(o.arrival, o.departure, stop.departure - stop.arrival, EdgeKind::Ride);
				if (!stop.continues) continue;
				size_t next = (i + 1) % t.stops.size();
				int64_t travel = std::max<int64_t>(1, t.stops[next].arrival - stop.departure + (next == 0 ? t.Circuit() : 0));
				uint32_t distance = 0;
				if (const auto *a = Station::GetIfValid(stop.station)) if (const auto *b = Station::GetIfValid(t.stops[next].station)) distance = DistanceManhattan(a->xy, b->xy);
				int64_t uncertainty = TravelUncertainty(travel, t.stops[next].travel_quality);
				o.travel = travel; o.uncertainty = uncertainty; o.distance = distance;
				this->AddEdge(o.departure, this->building_graph->occurrences[pattern.occurrences[next]].arrival, travel + uncertainty, EdgeKind::Ride, INVALID, distance, uncertainty);
			}
		}
		if (this->building_graph->hyperpath && this->building_graph->revisit_dominance) {
			if (!Product(this->building_graph->occurrences.size(), 16, envelope, memory)) { this->failed = true; this->diagnostic.envelope = memory; co_return; }
			this->building_graph->revisits.resize(this->building_graph->occurrences.size());
			/* Two backwards passes find the next visit across the circuit seam.
			 * A forced unload or broken continuation severs physical occupancy.
			 * Prefix costs include travel uncertainty and the arrival stop's dwell,
			 * exactly as the two Ride edges between departures do. */
			for (size_t p = 0; p < this->building_graph->patterns.size(); ++p) {
				const auto &pattern = this->building_graph->patterns[p];
				bool road = std::any_of(representatives[p]->vehicles.begin(), representatives[p]->vehicles.end(), [](const auto &v) { return v.type == VehicleType::Road; });
				std::map<StationID, std::pair<uint32_t, int64_t>> next;
				int64_t cost = 0;
				for (size_t k = 2 * pattern.occurrences.size(); k-- > 0;) {
					if (cooperative) co_await std::suspend_always{};
					uint32_t id = pattern.occurrences[k % pattern.occurrences.size()];
					const auto &o = this->building_graph->occurrences[id];
					const auto &to = this->building_graph->occurrences[pattern.occurrences[(k + 1) % pattern.occurrences.size()]];
					if (!o.continues || to.forced || cost > INT64_MAX - o.travel ||
							cost + o.travel > INT64_MAX - o.uncertainty || cost + o.travel + o.uncertainty > INT64_MAX - to.dwell) {
						next.clear(); cost = 0;
					} else cost += o.travel + o.uncertainty + to.dwell;
					auto found = next.find(o.station);
					if (k < pattern.occurrences.size() && o.board && found != next.end() && found->second.first != id && this->building_graph->occurrences[found->second.first].board) {
						/* Road orders can request different bay/drive-through directions at
						 * the same station. With no captured proof of equal facility access,
						 * retain the earlier opportunity. Other supported modes depend only
						 * on the same station/vehicle; refits and control flow are unsupported. */
						const auto &a = representatives[p]->timetable.stops[k];
						const auto &b = representatives[p]->timetable.stops[found->second.first - pattern.occurrences.front()];
						if (!this->building_graph->continuation_witness || !road || (a.boarding_facility != 0 && a.boarding_facility == b.boarding_facility))
							this->building_graph->revisits[id] = {found->second.first, cost - found->second.second};
					}
					next[o.station] = {id, cost};
				}
			}
		}
		if (sections && !this->building_graph->hyperpath) {
			bool success = false;
			auto task = this->BuildSections(pattern_members, transfer, nodes, envelope, memory, cooperative, success);
			while (!task.Resume(1)) co_await std::suspend_always{};
			if (!success) { this->failed = true; this->diagnostic.envelope = memory; co_return; }
		}
		this->diagnostic.envelope = envelope;
		if (this->building_graph->optimized) this->ContractGraph(nodes);
		this->building_graph->incoming_offsets.assign(nodes + 1, 0);
		for (const auto &edge : this->building_graph->edges) ++this->building_graph->incoming_offsets[edge.to + 1];
		for (size_t i = 1; i < this->building_graph->incoming_offsets.size(); ++i) this->building_graph->incoming_offsets[i] += this->building_graph->incoming_offsets[i - 1];
		auto cursor = this->building_graph->incoming_offsets;
		if (this->building_graph->integrated) {
			/* Write each stream directly into its stable reverse-CSR position.
			 * The builder AoS and destination ids are released before searching. */
			this->building_graph->hot_edges.from.resize(this->building_graph->edges.size());
			this->building_graph->hot_edges.cost.resize(this->building_graph->edges.size());
			this->building_graph->hot_edges.legs.resize(this->building_graph->edges.size());
			this->building_graph->hot_edges.range.resize(this->building_graph->edges.size());
			this->building_graph->cold_edges.resize(this->building_graph->edges.size());
			for (const auto &edge : this->building_graph->edges) {
				uint32_t at = cursor[edge.to]++;
				this->building_graph->hot_edges.range[at] = edge.range;
				this->building_graph->hot_edges.from[at] = edge.from; this->building_graph->hot_edges.cost[at] = edge.cost;
				this->building_graph->hot_edges.legs[at] = edge.kind == EdgeKind::Board || edge.kind == EdgeKind::Section;
				this->building_graph->cold_edges[at] = {edge.occurrence, edge.distance, edge.uncertainty, edge.kind};
			}
			this->diagnostic.logical_edges = this->building_graph->edges.size();
			for (const auto &range : this->building_graph->range_edges) this->diagnostic.logical_edges += range.end - range.begin - (range.excluded >= range.begin && range.excluded < range.end);
			this->diagnostic.range_edges = this->building_graph->range_edges.size();
			std::vector<Edge>().swap(this->building_graph->edges);
		} else if (this->building_graph->optimized) {
			/* Stable reverse CSR: the incoming order at each state is exactly
			 * the original insertion order, including equal-cost alternatives. */
			std::vector<Edge> reverse(this->building_graph->edges.size());
			for (const auto &edge : this->building_graph->edges) reverse[cursor[edge.to]++] = edge;
			this->building_graph->edges.swap(reverse);
		} else {
			this->building_graph->incoming.resize(this->building_graph->edges.size());
			for (uint32_t i = 0; i < this->building_graph->edges.size(); ++i) this->building_graph->incoming[cursor[this->building_graph->edges[i].to]++] = i;
		}
		if (this->building_graph->optimized) {
			this->building_graph->seed_offsets.assign(this->building_graph->stations.size() + 1, 0);
			for (const auto &o : this->building_graph->occurrences) {
				if (cooperative) co_await std::suspend_always{};
				if (o.alight) ++this->building_graph->seed_offsets[this->building_graph->station_index[o.station.base()] + 1];
			}
			for (size_t i = 1; i < this->building_graph->seed_offsets.size(); ++i) this->building_graph->seed_offsets[i] += this->building_graph->seed_offsets[i - 1];
			this->building_graph->seed_occurrences.resize(this->building_graph->seed_offsets.back()); auto cursor = this->building_graph->seed_offsets;
			for (uint32_t id = 0; id < this->building_graph->occurrences.size(); ++id) {
				if (cooperative) co_await std::suspend_always{};
				const auto &o = this->building_graph->occurrences[id];
				if (o.alight) this->building_graph->seed_occurrences[cursor[this->building_graph->station_index[o.station.base()]]++] = id;
			}
		}
		this->rows.reserve(this->building_graph->stations.size());
		this->diagnostic.stations = this->building_graph->stations.size(); this->diagnostic.patterns = this->building_graph->patterns.size();
		this->diagnostic.states = nodes; this->diagnostic.edges = this->building_graph->integrated ? this->building_graph->hot_edges.from.size() : this->building_graph->edges.size();
		this->work.queue.SetCapacityLimit(uint64_t(this->building_graph->integrated ? this->diagnostic.logical_edges : this->diagnostic.edges) + nodes);
		if (this->building_graph->hyperpath) this->work.strategy.events.SetCapacityLimit(uint64_t(this->diagnostic.logical_edges) + nodes);
		if (this->building_graph->hyperpath) {
			this->hyperpath_static_bytes = this->building_graph->revisits.capacity() * sizeof(Revisit) + (this->building_graph->station_index.capacity() + this->building_graph->service_pattern.capacity() + this->building_graph->waiting_station.capacity() +
					this->building_graph->incoming_offsets.capacity() + this->building_graph->seed_offsets.capacity() + this->building_graph->seed_occurrences.capacity()) * 4 + this->building_graph->stations.capacity() * sizeof(StationID) +
					this->building_graph->patterns.capacity() * sizeof(Pattern) + this->building_graph->occurrences.capacity() * sizeof(Occurrence) + this->building_graph->range_edges.capacity() * sizeof(RangeEdge) +
					this->building_graph->cold_edges.capacity() * sizeof(ColdEdge) + this->building_graph->hot_edges.from.capacity() * 4 + this->building_graph->hot_edges.cost.capacity() * 8 +
					this->building_graph->hot_edges.legs.capacity() + this->building_graph->hot_edges.range.capacity() * 4 + this->rows.capacity() * sizeof(Row) +
					this->building_graph->waiting_states.capacity() * sizeof(this->building_graph->waiting_states[0]) + this->building_graph->station_boarding.capacity() * sizeof(this->building_graph->station_boarding[0]);
			for (const auto &p : this->building_graph->patterns) this->hyperpath_static_bytes += p.members.capacity() * sizeof(Member) + p.boarding_services.capacity() * sizeof(OrderListID) + p.occurrences.capacity() * 4;
			for (const auto &waits : this->building_graph->waiting_states) this->hyperpath_static_bytes += waits.capacity() * sizeof(waits[0]);
			for (const auto &boards : this->building_graph->station_boarding) this->hyperpath_static_bytes += boards.capacity() * 4;
		}
		this->building_graph->states = this->diagnostic.states; this->building_graph->edge_count = this->diagnostic.edges; this->building_graph->logical_edges = this->diagnostic.logical_edges;
		if (!this->building_graph->stations.empty()) this->StartDestination();
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
	size_t candidates = std::count_if(this->building_graph->occurrences.begin(), this->building_graph->occurrences.end(), [](const auto &o) { return !o.board && !o.alight && !o.forced && o.continues; });
	if (uint64_t(candidates) * this->building_graph->stations.size() * 8 <= uint64_t(this->building_graph->edges.size()) * 2) return;
	if (this->building_graph->integrated) {
		uint64_t logical = this->building_graph->edges.size();
		for (const auto &range : this->building_graph->range_edges) logical += range.end - range.begin - (range.excluded >= range.begin && range.excluded < range.end);
		/* Range compression can make the physical graph tiny without reducing
		 * virtual search work. Require at least a one-percent upper bound on
		 * saved per-row work before paying for degree/rank construction. */
		if (uint64_t(candidates) * 8 * 100 <= nodes + logical) return;
	}
	/* Ranges refer only to the waiting-state prefix, whose ranks never move.
	 * Their missing per-wait degrees cannot affect pure occurrence nodes. */
	std::vector<uint32_t> in_degree(nodes), out_degree(nodes), successor(nodes, INVALID), rank(nodes, INVALID);
	for (uint32_t i = 0; i < this->building_graph->edges.size(); ++i) {
		const auto &edge = this->building_graph->edges[i];
		++in_degree[edge.to]; ++out_degree[edge.from]; successor[edge.from] = i;
	}
	std::vector<bool> remove(nodes);
	for (const auto &o : this->building_graph->occurrences) {
		if (o.board || o.alight || o.forced || !o.continues || in_degree[o.arrival] != 1 || out_degree[o.arrival] != 1 ||
				in_degree[o.departure] != 1 || out_degree[o.departure] != 1) continue;
		const auto &dwell = this->building_graph->edges[successor[o.arrival]], &travel = this->building_graph->edges[successor[o.departure]];
		if (dwell.kind != EdgeKind::Ride || dwell.to != o.departure || travel.kind != EdgeKind::Ride || travel.cost <= 0) continue;
		remove[o.arrival] = remove[o.departure] = true;
	}
	/* Validate all sums before mutating anything. Closed, unobservable cycles
	 * have no surviving predecessor and are simply unreachable from any seed. */
	for (const auto &edge : this->building_graph->edges) {
		if (remove[edge.from]) continue;
		int64_t cost = edge.cost, uncertainty = edge.uncertainty;
		uint32_t to = edge.to;
		while (remove[to]) {
			const auto &next = this->building_graph->edges[successor[to]];
			if (edge.kind != EdgeKind::Ride || cost > INT64_MAX - next.cost || uncertainty > INT64_MAX - next.uncertainty) return;
			cost += next.cost; uncertainty += next.uncertainty; to = next.to;
		}
	}
	uint32_t count = 0;
	for (uint32_t state = 0; state < nodes; ++state) if (!remove[state]) rank[state] = count++;
	std::vector<Edge> contracted; contracted.reserve(this->building_graph->edges.size());
	for (auto edge : this->building_graph->edges) {
		if (remove[edge.from]) continue;
		while (remove[edge.to]) {
			const auto &next = this->building_graph->edges[successor[edge.to]];
			edge.cost += next.cost; edge.uncertainty += next.uncertainty;
			edge.distance = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(edge.distance) + next.distance));
			edge.to = next.to;
		}
		edge.from = rank[edge.from]; edge.to = rank[edge.to];
		contracted.push_back(edge);
	}
	this->building_graph->edges.swap(contracted);
	for (auto &o : this->building_graph->occurrences) { o.arrival = rank[o.arrival]; o.departure = rank[o.departure]; }
	for (auto &state : this->building_graph->landings) if (state != INVALID) state = rank[state];
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
	for (uint32_t p = 0; p < this->building_graph->patterns.size(); ++p) for (uint32_t id : this->building_graph->patterns[p].occurrences) {
		if (cooperative) co_await std::suspend_always{};
		if (this->building_graph->occurrences[id].board) starters[this->building_graph->occurrences[id].station].insert(p);
	}
	for (uint32_t p = 0; p < this->building_graph->patterns.size(); ++p) {
		if (cooperative) co_await std::suspend_always{};
		const auto &pattern = this->building_graph->patterns[p]; const auto &t = members[p].front()->timetable;
		for (size_t start = 0; start < t.stops.size(); ++start) {
			if (cooperative) co_await std::suspend_always{};
			uint32_t board = pattern.occurrences[start]; const auto &b = this->building_graph->occurrences[board];
			if (!b.board || starters[b.station].size() < 2) continue;
			uint32_t prefix = INVALID, distance = 0;
			int64_t uncertainty = 0;
			for (size_t length = 0; length < t.stops.size(); ++length) {
				if (cooperative) co_await std::suspend_always{};
				size_t stop = (start + length) % t.stops.size();
				uint32_t alight = pattern.occurrences[stop]; const auto &a = this->building_graph->occurrences[alight];
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
		uint32_t first = this->building_graph->occurrences[paths.front().board].pattern;
		if (std::none_of(paths.begin(), paths.end(), [&](const Path &path) { return this->building_graph->occurrences[path.board].pattern != first; })) continue;
		uint64_t candidates = 0;
		for (const auto &path : paths) for (const auto *service : members[this->building_graph->occurrences[path.board].pattern]) {
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
		for (const auto &path : paths) for (const auto *s : members[this->building_graph->occurrences[path.board].pattern]) for (const auto &v : s->vehicles) {
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
			uint32_t p = this->building_graph->occurrences[path.board].pattern;
			size_t stop = path.board - this->building_graph->patterns[p].occurrences.front();
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
		if (this->building_graph->estimates && !unknown_vehicles.empty()) {
			/* Use the longest unknown circuit for fleet credit; heterogeneous
			 * long routes must not inherit a short shuttle's headway. */
			int64_t longest = 0;
			for (const auto &path : paths) for (const auto *s : members[this->building_graph->occurrences[path.board].pattern])
				longest = std::max(longest, std::max(s->timetable.Circuit(), s->uncertain_dispatch_period));
			int64_t estimated = std::min(UnknownWait(unknown_circuit, 1, uncertain_dispatch), UnknownWait(longest, unknown_vehicles.size(), uncertain_dispatch));
			wait = events.empty() ? estimated : std::min(wait, estimated);
			gap = std::max<int64_t>(1, 2 * wait);
		}
		int64_t risk = gap / seats + (gap % seats != 0);
		if (this->building_graph->estimates && !unknown_vehicles.empty()) risk += std::min<int64_t>(64, (wait + 7) / 8);
		++this->diagnostic.shared_sections;
		if (this->building_graph->landings.empty()) {
			if (!Product(this->building_graph->occurrences.size(), 36, envelope, memory)) co_return;
			this->building_graph->landings.assign(this->building_graph->occurrences.size(), INVALID);
			this->building_graph->boarding_sections.resize(this->building_graph->occurrences.size());
		}
		/* Persist the same complete paths used by the frequency estimate. Each
		 * pattern refers to its existing member vector, rather than copying this
		 * binding once per vehicle or shared order list. The saved concrete order
		 * pair identifies the section without adding anything to CargoPacket. */
		if (this->building_graph->sections.size() >= INVALID || !Product(1, 128, envelope, memory) || !Product(paths.size(), 128, envelope, memory)) co_return;
		Section section{UINT64_MAX, {}};
		section.bindings.reserve(paths.size());
		uint64_t key_count = 0;
		for (const auto &path : paths) key_count += this->building_graph->patterns[this->building_graph->occurrences[path.board].pattern].members.size();
		if (!Product(key_count, 16, envelope, memory) || !Product(key_count, uint64_t(paths.size()) * 16, envelope, memory)) co_return;
		std::vector<uint64_t> raw_keys;
		raw_keys.reserve(static_cast<size_t>(key_count));
		for (const auto &path : paths) {
			if (cooperative) co_await std::suspend_always{};
			const auto &board = this->building_graph->occurrences[path.board], &alight = this->building_graph->occurrences[path.alight];
			section.key = std::min(section.key, (uint64_t{1} << 63) |
					SectionPathKey(this->building_graph->patterns[board.pattern].members.front().id.base(), board.order, alight.order));
			for (const auto &member : this->building_graph->patterns[board.pattern].members) raw_keys.push_back((uint64_t{1} << 63) | SectionPathKey(member.id.base(), board.order, alight.order));
		}
		std::sort(raw_keys.begin(), raw_keys.end()); raw_keys.erase(std::unique(raw_keys.begin(), raw_keys.end()), raw_keys.end());
		for (const auto &path : paths) {
			if (cooperative) co_await std::suspend_always{};
			const auto &board = this->building_graph->occurrences[path.board], &alight = this->building_graph->occurrences[path.alight];
			section.bindings.push_back({path.board, path.alight});
			this->building_graph->section_paths.emplace(SectionPathKey(board.pattern, board.order, alight.order),
					SectionPath{static_cast<uint32_t>(this->building_graph->sections.size()), path.board, path.alight});
			this->building_graph->boarding_sections[path.board].insert(this->building_graph->boarding_sections[path.board].end(), raw_keys.begin(), raw_keys.end());
		}
		std::sort(section.bindings.begin(), section.bindings.end(), [&](const SectionBinding &a, const SectionBinding &b) {
			const auto &x = this->building_graph->occurrences[a.board], &y = this->building_graph->occurrences[b.board]; return std::tie(x.pattern, x.order) < std::tie(y.pattern, y.order);
		});
		this->building_graph->sections.push_back(std::move(section));
		for (const auto &path : paths) {
			if (cooperative) co_await std::suspend_always{};
			const auto &board = this->building_graph->occurrences[path.board], &alight = this->building_graph->occurrences[path.alight];
			uint32_t station = this->building_graph->station_index[board.station.base()];
			size_t edges = 1 + this->building_graph->waiting_states[station].size();
			if (this->building_graph->section_rides.size() >= INVALID || edges >= INVALID - this->building_graph->edges.size() ||
					!Product(1, 64, envelope, memory) || !Product(this->building_graph->integrated ? 1 : edges, this->building_graph->integrated ? 128 : 264, envelope, memory) ||
					(this->building_graph->integrated && !Product(edges, 128, envelope, memory))) co_return;
			if (this->building_graph->landings[path.alight] == INVALID) {
				if (nodes >= INVALID || !Product(1, 144 + 264, envelope, memory)) co_return;
				this->building_graph->landings[path.alight] = static_cast<uint32_t>(nodes++);
				this->AddEdge(this->building_graph->landings[path.alight], this->StateFor(this->building_graph->station_index[alight.station.base()], alight.pattern),
						2 * transfer, EdgeKind::Alight, path.alight);
			}
			uint32_t id = static_cast<uint32_t>(this->building_graph->section_rides.size());
			int64_t cost = path.riding + path.uncertainty + wait + risk;
			this->building_graph->section_rides.push_back({path.board, path.alight, path.riding, wait, cost});
			if (this->building_graph->integrated) this->AddBoardEdges(station, this->building_graph->landings[path.alight], board.pattern, cost, EdgeKind::Section, id, path.distance);
			else {
			this->AddEdge(station, this->building_graph->landings[path.alight], cost, EdgeKind::Section, id, path.distance);
			for (const auto &[previous, state] : this->building_graph->waiting_states[station]) {
				if (cooperative) co_await std::suspend_always{};
				if (previous != board.pattern || this->building_graph->patterns[board.pattern].members.size() > 1) {
					this->AddEdge(state, this->building_graph->landings[path.alight], cost, EdgeKind::Section, id, path.distance);
				}
			}
			}
		}
	}
	for (auto &keys : this->building_graph->boarding_sections) {
		if (cooperative) co_await std::suspend_always{};
		std::sort(keys.begin(), keys.end()); keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	}
	this->building_graph->binding_bytes = this->building_graph->sections.capacity() * sizeof(Section) + this->building_graph->boarding_sections.capacity() * sizeof(std::vector<uint64_t>) + this->building_graph->section_paths.size() * 64;
	for (const auto &section : this->building_graph->sections) this->building_graph->binding_bytes += section.bindings.capacity() * sizeof(SectionBinding);
	for (const auto &keys : this->building_graph->boarding_sections) this->building_graph->binding_bytes += keys.capacity() * sizeof(uint64_t);
	this->diagnostic.section_rides = this->building_graph->section_rides.size();
	success = true; co_return;
}

void PassengerPrecomputedTable::StartDestination()
{
	this->StartDestination(this->work, this->destination);
}

void PassengerPrecomputedTable::StartDestination(DestinationWorkspace &work, size_t destination) const
{
	this->graph->StartDestination(work, destination);
}

bool PassengerPrecomputedTable::UseCompressedRows() const
{
	return this->graph->UseCompressedRows();
}

const PassengerPrecomputedTable::Label &PassengerPrecomputedTable::RowLabel(const Row &row, uint32_t origin, uint32_t state) const
{
	if (!this->UseCompressedRows()) return row.labels[state];
	if (state != origin) {
		uint32_t slot = this->graph->exception_slots[origin];
		if (slot != INVALID && row.indices[this->graph->stations.size() + slot] == state) return row.exception_labels[slot];
	}
	return row.labels[origin];
}

const PassengerPrecomputedTable::Label &PassengerPrecomputedTable::AlternativeLabel(const Row &row, uint32_t origin) const
{
	if (!this->UseCompressedRows()) return row.alternative[origin];
	uint32_t index = row.indices[origin];
	static const Label empty;
	return index == INVALID ? empty : row.alternative[index];
}

void PassengerPrecomputedTable::PrepareCompressedRow(DestinationWorkspace &work) const
{
	/* Keep the original conservative admission charge: at most 100*n + 52*w
	 * payload bytes versus the reserved 128*n + 64*w per row. The shared Row also
	 * carries empty Hyperpath headers beyond the 128-byte row charge (at most 128
	 * more, including MSVC debug iterators); the 28*n payload slack covers them for
	 * n >= 5, and the 1 MiB base envelope for smaller tables. Never turn a saved
	 * v11 memory rejection into success. */
	static_assert(sizeof(Label) == 48 && sizeof(Row) <= 256);
	work.prepared.exception_labels.reserve(this->graph->exception_slot_count);
	work.prepared.indices.reserve(this->graph->stations.size() + this->graph->exception_slot_count);
	work.prepared.indices.assign(this->graph->stations.size() + this->graph->exception_slot_count, INVALID);
	size_t alternatives = std::count_if(work.scratch.alternative.begin(), work.scratch.alternative.end(), [](const Label &label) { return label.board != INVALID; });
	work.prepared.alternative.reserve(alternatives);
}

void PassengerPrecomputedTable::CopyStationLabel(DestinationWorkspace &work, uint32_t origin) const
{
	const Label base = this->SearchLabel(work, origin);
	work.prepared.labels.push_back(base);
	uint32_t slot = this->graph->exception_slots[origin];
	if (slot != INVALID) {
		assert(slot == work.prepared.exception_labels.size());
		uint32_t state = work.exception_state[origin];
		assert(state == INVALID || (state >= this->graph->waiting_states[origin].front().second && state <= this->graph->waiting_states[origin].back().second));
		Label exception = state == INVALID ? Label{} : this->SearchLabel(work, state);
		/* Priority ties may retain different metadata. Omit the cached ID only
		 * when every Label field equals the base, matching the scalar row form. */
		if (state != INVALID && exception == base) { state = INVALID; exception = {}; }
		work.prepared.indices[this->graph->stations.size() + slot] = state; work.prepared.exception_labels.push_back(exception);
	}
	const auto &alternative = work.scratch.alternative[origin];
	if (alternative.board == INVALID) {
		assert(alternative == Label{});
	} else {
		work.prepared.indices[origin] = static_cast<uint32_t>(work.prepared.alternative.size());
		work.prepared.alternative.push_back(alternative);
	}
}

PassengerPrecomputedTable::Label PassengerPrecomputedTable::SearchLabel(const DestinationWorkspace &work, uint32_t state) const
{
	return this->graph->SearchLabel(work, state);
}

void PassengerPrecomputedTable::SetSearchLabel(DestinationWorkspace &work, uint32_t state, const Label &label) const
{
	this->graph->SetSearchLabel(work, state, label);
}

#ifdef WITH_PASSENGER_TABLE_INVARIANT_CHECKS
bool PassengerPrecomputedTable::WaitingStationInvariant(const DestinationWorkspace &work, uint32_t station) const
{
	return this->graph->WaitingStationInvariant(work, station);
}

#endif

/** One descriptor is one original contiguous incoming-edge subsequence.
 * Each virtual relaxation costs exactly one Work unit. Excluded states were
 * never edges; skipping them is free. The base station always precedes waits.
 * Only the accepted states touch cold metadata, in original state order. */
size_t PassengerPrecomputedTable::RelaxIncoming(DestinationWorkspace &work, size_t allowance) const
{
	return this->graph->RelaxIncoming(work, allowance);
}

void PassengerPrecomputedTable::CollectSearchCounters()
{
	this->diagnostic.board_events += std::exchange(this->work.counters.board_events, 0);
	this->diagnostic.board_accepts += std::exchange(this->work.counters.board_accepts, 0);
	this->diagnostic.range_updates += std::exchange(this->work.counters.range_updates, 0);
	this->diagnostic.heap_pushes += std::exchange(this->work.counters.heap_pushes, 0);
	this->diagnostic.heap_pops += std::exchange(this->work.counters.heap_pops, 0);
}

/** Reservations cover 2*workers output slots, workers in-flight cancelled
 * jobs and workers reusable contexts, across ALL tables and retired generations.
 * This local cap changes cache availability alone, never canonical admission. */
struct PassengerPrecomputedTable::WorkerMetrics {
	std::atomic<uint64_t> searches{0}, operations{0}, nanoseconds{0}, cancelled_work{0}, minimum{UINT64_MAX}, maximum{0}, outstanding{0}, workspace_reuses{0};
	void Add(PassengerParallelBuildDiagnostic &result) const
	{
		result.workspace_reuses += this->workspace_reuses.load(); result.searches += this->searches.load(); result.search_work += this->operations.load();
		result.search_nanoseconds += this->nanoseconds.load(); result.cancelled_search_work += this->cancelled_work.load();
		result.minimum_work = std::min(result.minimum_work, this->minimum.load()); result.maximum_work = std::max(result.maximum_work, this->maximum.load());
	}
};

struct PassengerPrecomputedTable::WorkerGeneration {
	std::shared_ptr<const GraphSnapshot> graph;
	std::shared_ptr<std::atomic<uint64_t>> budget;
	uint64_t reserved = 0, id = 0;
	size_t workers = 0;
	void (*hook)(size_t, bool) = nullptr;
	void (*quantum_hook)(size_t, uint64_t) = nullptr;
	std::atomic<bool> cancelled{false}, failed{false};
	std::shared_ptr<WorkerMetrics> metrics = std::make_shared<WorkerMetrics>();
	~WorkerGeneration() { if (this->reserved != 0) this->budget->fetch_sub(this->reserved); }
};

struct PassengerPrecomputedTable::ParallelState {
	std::shared_ptr<WorkerGeneration> generation;
	std::deque<std::shared_ptr<DestinationBuildResult>> pending, recycled;
	size_t next = 0;
	~ParallelState() { this->generation->cancelled.store(true, std::memory_order_relaxed); }
};

/** Process-lifetime workers. Only process shutdown joins. Queue operations never
 * wait for a search. Cancelled generations keep their own graph alive until the
 * last running job/idle context releases it; the table can immediately disappear. */
struct PassengerPrecomputedTable::DestinationWorkerPool {
	static constexpr uint64_t SCRATCH_LIMIT = uint64_t{512} * 1024 * 1024;
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::shared_ptr<DestinationBuildResult>> jobs;
	std::vector<std::thread> threads;
	std::shared_ptr<std::atomic<uint64_t>> budget = std::make_shared<std::atomic<uint64_t>>(0);
	std::atomic<bool> stopping{false};
	uint64_t next_generation = 0; ///< Main-thread only; never saved or used by simulation.
	size_t Ensure(size_t count, uint64_t &starts)
	{
		std::lock_guard lock(this->mutex);
		this->threads.reserve(8);
		while (this->threads.size() < count) {
			std::thread thread; size_t index = this->threads.size();
			if (!StartNewThread(&thread, "pax-destination", [this, index]() { this->Run(index); })) break;
			this->threads.push_back(std::move(thread)); ++starts;
		}
		return this->threads.size();
	}
	std::shared_ptr<WorkerGeneration> Reserve(size_t workers, uint64_t bytes)
	{
		auto generation = std::make_shared<WorkerGeneration>();
		const uint64_t charge = workers * 4 * bytes;
		uint64_t used = this->budget->load();
		do { if (charge > SCRATCH_LIMIT - used) return {}; } while (!this->budget->compare_exchange_weak(used, used + charge));
		generation->budget = this->budget; generation->reserved = charge;
		generation->workers = workers; generation->id = ++this->next_generation; return generation;
	}
	using RetiredJobs = std::array<std::shared_ptr<DestinationBuildResult>, 16>;
	void Prune(RetiredJobs &retired)
	{
		size_t count = 0;
		std::erase_if(this->jobs, [&](auto &job) {
			if (!job->cancelled.load(std::memory_order_relaxed) && !job->generation->cancelled.load(std::memory_order_relaxed)) return false;
			job->cancelled.store(true); --job->generation->metrics->outstanding; job->ready.store(true, std::memory_order_release);
			retired[count++] = std::move(job); return true;
		});
	}
	bool Submit(const std::shared_ptr<DestinationBuildResult> &job)
	{
		RetiredJobs retired; // Destroy cancelled graph/context owners after unlocking.
		std::lock_guard lock(this->mutex); this->Prune(retired);
		if (this->jobs.size() >= 16) return false;
		this->jobs.push_back(job); ++job->generation->metrics->outstanding;
		/* With a full-size generation every sleeping thread is eligible. Avoid
		 * waking all eight for a single replenished slot. Smaller generations
		 * must also wake their eligible low-index workers in a larger pool. */
		if (job->generation->workers == this->threads.size()) this->wake.notify_one(); else this->wake.notify_all();
		return true;
	}
	void Run(size_t index)
	{
		DestinationWorkspace workspace;
		std::shared_ptr<WorkerGeneration> owner;
		std::unique_lock lock(this->mutex);
		for (;;) {
			/* A cancelled idle context must relinquish its reservation too. No
			 * destructor calls back into this pool (including at static teardown). */
			if (owner && owner->cancelled.load(std::memory_order_relaxed)) {
				lock.unlock(); workspace = {}; owner.reset(); lock.lock();
			}
			RetiredJobs retired; this->Prune(retired);
			if (retired.front()) { lock.unlock(); retired = {}; lock.lock(); }
			if (this->stopping.load()) return;
			auto selected = std::find_if(this->jobs.begin(), this->jobs.end(), [&](const auto &job) { return index < job->generation->workers; });
			if (selected == this->jobs.end()) { this->wake.wait_for(lock, std::chrono::milliseconds(20)); continue; }
			auto job = std::move(*selected); this->jobs.erase(selected);
			lock.unlock();
			if (owner != job->generation) { workspace = {}; owner = job->generation; }
			const auto started = Clock::now(); uint64_t operations = 0;
			auto cancelled = [&]() { return job->cancelled.load(std::memory_order_relaxed) || owner->cancelled.load(std::memory_order_relaxed) || this->stopping.load(std::memory_order_relaxed); };
			try {
				if (!cancelled()) {
					if (owner->hook) owner->hook(job->destination, false);
					if (!cancelled()) {
						if (workspace.queues_configured) ++owner->metrics->workspace_reuses;
						workspace.counters = {}; owner->graph->StartDestination(workspace, job->destination);
						while (!workspace.finishing_row && !cancelled()) {
							operations += owner->graph->hyperpath ? owner->graph->SearchHyperpath(workspace, 4096) : owner->graph->SearchDestination(workspace, 4096);
							if (owner->quantum_hook) owner->quantum_hook(job->destination, operations);
						}
					}
					if (owner->hook) owner->hook(job->destination, true);
				}
				if (!cancelled() && workspace.finishing_row) {
					job->operations = operations; std::swap(job->workspace, workspace);
					++owner->metrics->searches;
					uint64_t previous = owner->metrics->minimum.load(); while (operations < previous && !owner->metrics->minimum.compare_exchange_weak(previous, operations)) {}
					previous = owner->metrics->maximum.load(); while (operations > previous && !owner->metrics->maximum.compare_exchange_weak(previous, operations)) {}
				} else { job->cancelled.store(true); owner->metrics->cancelled_work += operations; workspace = {}; }
			} catch (...) { job->error = true; owner->failed.store(true); owner->cancelled.store(true); owner->metrics->cancelled_work += operations; workspace = {}; }
			job->nanoseconds = Elapsed(started); owner->metrics->nanoseconds += job->nanoseconds; owner->metrics->operations += operations;
			/* This is the final access to the job's non-atomic fields. A ready
			 * result may now be adopted/recycled by the simulation thread. */
			--owner->metrics->outstanding; job->ready.store(true, std::memory_order_release);
			lock.lock();
		}
	}
	~DestinationWorkerPool()
	{
		{ std::lock_guard lock(this->mutex); this->stopping.store(true); for (const auto &job : this->jobs) { job->generation->cancelled.store(true); --job->generation->metrics->outstanding; } this->jobs.clear(); this->wake.notify_all(); }
		for (auto &thread : this->threads) thread.join();
	}
};

PassengerPrecomputedTable::DestinationWorkerPool &PassengerPrecomputedTable::WorkerPool()
{
	static DestinationWorkerPool pool; return pool;
}

PassengerParallelBuildDiagnostic PassengerPrecomputedTable::GetParallelDiagnostic() const
{
	auto result = this->parallel_diagnostic;
	for (const auto &statistics : this->parallel_statistics) statistics->Add(result);
	return result;
}

bool PassengerPrecomputedTable::ParallelIdle() const
{
	return std::all_of(this->parallel_statistics.begin(), this->parallel_statistics.end(), [](const auto &metrics) { return metrics->outstanding.load() == 0; });
}

bool PassengerPrecomputedTable::ParallelCacheReady() const
{
	return this->parallel && (this->parallel->generation->failed.load() || std::all_of(this->parallel->pending.begin(), this->parallel->pending.end(), [](const auto &job) { return job->ready.load(std::memory_order_acquire); }));
}

/** Bounded, nonblocking lookahead. Startup, resource admission and failure affect
 * only local cache hits. All game Work, memory admission and publication remain
 * in StepImpl; no caller polls completion to choose a logical allowance. */
bool PassengerPrecomputedTable::PrepareParallelBatch(uint workers)
{
	if (workers == 0 || this->parallel_unavailable || this->graph_build || this->Finished()) return false;
	const auto started = Clock::now();
	try {
		if (!this->parallel) {
			const size_t cpus = std::max(1U, std::thread::hardware_concurrency());
			const uint64_t bytes = uint64_t(this->graph->states) * 256 + uint64_t(this->graph->integrated ? this->graph->logical_edges : this->graph->edge_count) * 32 + 65536;
			workers = static_cast<uint>(std::min<size_t>({workers, 8, cpus, DestinationWorkerPool::SCRATCH_LIMIT / bytes / 4, this->graph->stations.size() - this->destination}));
			size_t permitted = 0; while (permitted < workers && (!this->parallel_start_hook || this->parallel_start_hook(static_cast<uint>(permitted)))) ++permitted;
			if (permitted == 0) { if (workers != 0) { this->parallel_unavailable = true; ++this->parallel_diagnostic.fallbacks; } return false; }
			auto &pool = WorkerPool();
			permitted = std::min(permitted, pool.Ensure(permitted, this->parallel_diagnostic.thread_starts));
			if (permitted == 0) throw std::bad_alloc{};
			std::shared_ptr<WorkerGeneration> generation;
			while (permitted != 0 && !(generation = pool.Reserve(permitted, bytes))) --permitted;
			if (!generation) return false;
			generation->graph = this->graph; generation->hook = this->parallel_search_hook; generation->quantum_hook = this->parallel_quantum_hook;
			auto state = std::make_shared<ParallelState>(); state->generation = std::move(generation); state->next = this->destination;
			std::erase_if(this->parallel_statistics, [&](const auto &metrics) {
				if (metrics->outstanding.load() != 0) return false;
				metrics->Add(this->parallel_diagnostic); return true;
			});
			this->parallel_statistics.push_back(state->generation->metrics);
			this->parallel = std::move(state);
			this->parallel_diagnostic.workers = std::max(this->parallel_diagnostic.workers, permitted);
			this->parallel_diagnostic.estimated_workspace_bytes = std::max(this->parallel_diagnostic.estimated_workspace_bytes, static_cast<size_t>(this->parallel->generation->reserved));
		}
		auto &state = *this->parallel;
		if (state.generation->failed.load()) { this->parallel_unavailable = true; ++this->parallel_diagnostic.fallbacks; state.pending.clear(); return false; }
		if (state.generation->cancelled.load()) return false;
		while (!state.pending.empty() && state.pending.front()->destination < this->destination) { state.pending.front()->cancelled.store(true); state.pending.pop_front(); }
		state.next = std::max(state.next, this->destination + (this->serial_destination == this->destination));
		size_t submitted = 0;
		while (state.pending.size() + (this->parallel_result ? 1 : 0) < state.generation->workers * 2 && state.next < this->graph->stations.size()) {
			std::shared_ptr<DestinationBuildResult> job;
			if (!state.recycled.empty()) { job = std::move(state.recycled.front()); state.recycled.pop_front(); }
			else job = std::make_shared<DestinationBuildResult>();
			job->generation = state.generation; job->generation_id = state.generation->id;
			job->destination = state.next; job->station = this->graph->stations[state.next]; job->operations = 0; job->error = false;
			job->ready.store(false); job->cancelled.store(false);
			if (!WorkerPool().Submit(job)) break;
			state.pending.push_back(std::move(job)); ++state.next; ++submitted;
		}
		if (submitted != 0) ++this->parallel_diagnostic.batches;
		this->parallel_diagnostic.peak_pending = std::max(this->parallel_diagnostic.peak_pending, state.pending.size() + (this->parallel_result ? 1 : 0));
		this->parallel_diagnostic.batch_nanoseconds += Elapsed(started);
		return true;
	} catch (...) {
		if (this->parallel) { this->parallel->generation->cancelled.store(true); this->parallel->pending.clear(); }
		this->parallel_unavailable = true; ++this->parallel_diagnostic.fallbacks; return false;
	}
}

void PassengerPrecomputedTable::CancelParallel()
{
	if (this->parallel_credit != 0) {
		/* Reconstruct exactly the virtual prefix when switching to saved replay /
		 * scalar stepping. It is never charged again; no partial serial row can
		 * switch back to a late result. */
		while (this->parallel_credit != 0) {
			size_t quantum = static_cast<size_t>(std::min<uint64_t>(this->parallel_credit, 65536));
			size_t used = this->graph->hyperpath ? this->SearchHyperpath(this->work, quantum) : this->SearchDestination(this->work, quantum);
			assert(used == quantum); this->parallel_credit -= used;
		}
		this->CollectSearchCounters(); this->serial_destination = this->destination;
	}
	this->parallel_result.reset();
	if (this->parallel) { this->parallel->generation->cancelled.store(true); this->parallel->pending.clear(); this->parallel->recycled.clear(); this->parallel.reset(); }
}

size_t PassengerPrecomputedTable::AdvanceSearch(size_t allowance, uint workers)
{
	if (workers != 0 && !this->parallel_result && this->serial_destination != this->destination) {
		this->PrepareParallelBatch(workers);
		/* Make this choice exactly once, at the unsearched row boundary. A miss
		 * owns the serial cursor through completion, including across ticks. */
		const bool pristine = this->work.initializing_row && this->work.search.cost.empty() && this->work.scratch.labels.empty() && this->work.seeding == 0;
		if (this->parallel && !this->parallel->pending.empty() && this->parallel->pending.front()->destination == this->destination) {
			auto job = std::move(this->parallel->pending.front()); this->parallel->pending.pop_front();
			if (pristine && job->ready.load(std::memory_order_acquire) && !job->cancelled.load() && !job->error &&
					job->operations != 0 && job->workspace.finishing_row && job->generation_id == this->parallel->generation->id && job->station == this->graph->stations[this->destination]) {
				this->parallel_result = std::move(job); ++this->parallel_diagnostic.cache_hits;
			} else job->cancelled.store(true);
		}
		if (!this->parallel_result) { this->serial_destination = this->destination; ++this->parallel_diagnostic.serial_rows; }
		if (this->parallel_result) this->parallel_misses = 0;
		else if (this->parallel && !this->parallel_unavailable && ++this->parallel_misses >= 16) {
			/* A persistently late cache can otherwise burn a second CPU on rows
			 * the main thread already owns. Disable speculation locally for this
			 * generation; the same serial Work/publication path remains in use. */
			this->parallel_unavailable = true; ++this->parallel_diagnostic.throttles; this->CancelParallel();
		}
		this->PrepareParallelBatch(workers);
	}
	if (this->parallel_result) {
		auto &result = *this->parallel_result;
		assert(result.destination == this->destination && result.operations > this->parallel_credit);
		size_t used = static_cast<size_t>(std::min<uint64_t>(allowance, result.operations - this->parallel_credit));
		this->parallel_credit += used; this->parallel_diagnostic.cache_work += used;
		if (this->parallel_credit == result.operations) {
			this->CollectSearchCounters(); std::swap(this->work, result.workspace); this->CollectSearchCounters();
			if (this->parallel && !this->parallel->generation->cancelled.load()) {
				try { this->parallel->recycled.push_back(this->parallel_result); } catch (const std::bad_alloc &) { /* Reuse is optional. */ }
			}
			this->parallel_result.reset(); this->parallel_credit = 0;
		}
		return used;
	}
	const auto started = Clock::now();
	size_t used = this->graph->hyperpath ? this->SearchHyperpath(this->work, allowance) : this->SearchDestination(this->work, allowance);
	this->parallel_diagnostic.main_search_nanoseconds += Elapsed(started); this->parallel_diagnostic.serial_search_work += used;
	return used;
}

size_t PassengerPrecomputedTable::StepParallel(size_t allowance, uint workers, const std::function<void(StationID)> &published)
{
	if (workers == 0) return this->Step(allowance, false, published);
	const auto started = Clock::now();
	size_t used = this->StepImpl(allowance, false, published, workers);
	this->parallel_diagnostic.integration_nanoseconds += Elapsed(started);
	if (this->Finished()) this->CancelParallel();
	return used;
}

size_t PassengerPrecomputedTable::SearchDestination(DestinationWorkspace &work, size_t allowance) const
{
	return this->graph->SearchDestination(work, allowance);
}

size_t PassengerPrecomputedTable::Step(size_t allowance, bool legacy_publication, const std::function<void(StationID)> &published)
{
	try {
		this->CancelParallel();
	} catch (const std::bad_alloc &) {
		FatalError("Insufficient memory for passenger routing. Disable passenger_journey_precomputed or free memory before resuming.");
	}
	return this->StepImpl(allowance, legacy_publication, published, 0);
}

size_t PassengerPrecomputedTable::StepImpl(size_t allowance, bool legacy_publication, const std::function<void(StationID)> &published, uint workers)
{
	if (!this->PrepareGraph(std::min<size_t>(allowance, 1024))) return 0;
	if (workers != 0) this->PrepareParallelBatch(workers);
	if (this->graph->hyperpath) return this->StepHyperpath(allowance, published, workers);
	const auto started = Clock::now(); size_t used = 0;
	auto index_origin = [&](size_t from) {
		if (from >= this->graph->stations.size() || this->SearchCost(this->work, static_cast<uint32_t>(from)) == INT64_MAX) return;
		auto &destinations = this->reachable_destinations[from]; size_t capacity = destinations.capacity();
		destinations.push_back(this->graph->stations[this->destination]);
		this->reachable_bytes += (destinations.capacity() - capacity) * sizeof(StationID);
	};
	try {
		while (used < allowance && !this->Finished()) {
			if (!this->work.finishing_row) {
				used += this->AdvanceSearch(allowance - used, workers); continue;
			}
			++used;
			if (this->work.finishing_row) {
				if (this->work.finishing >= this->graph->occurrences.size() && this->work.finishing - this->graph->occurrences.size() < this->graph->section_rides.size()) {
					const auto &ride = this->graph->section_rides[this->work.finishing++ - this->graph->occurrences.size()];
					const auto &board = this->graph->occurrences[ride.board];
					uint32_t station = this->graph->station_index[board.station.base()];
					int64_t best_cost = this->SearchCost(this->work, station);
					if (best_cost == INT64_MAX) continue;
					uint32_t best_board = this->SearchBoard(this->work, station);
					if (best_board == INVALID || this->graph->occurrences[best_board].pattern == board.pattern) continue;
					uint32_t landing = this->graph->landings[ride.alight];
					int64_t suffix_cost = this->SearchCost(this->work, landing);
					if (suffix_cost > INT64_MAX - ride.cost) continue;
					int64_t cost = suffix_cost + ride.cost;
					if (cost == INT64_MAX || cost - best_cost > best_cost / 4) continue;
					int64_t best_riding = this->SearchRiding(this->work, station);
					if (this->graph->estimates && (this->SearchRiding(this->work, landing) + ride.riding > best_riding + std::max<int64_t>(32, best_riding / 4) ||
							this->SearchLegs(this->work, landing) + 1 > this->SearchLegs(this->work, station) + 1)) continue;
					auto &alt = this->work.scratch.alternative[station];
					if (cost < alt.cost) {
						alt = this->SearchLabel(this->work, landing); alt.board = ride.board; alt.alight = ride.alight; ++alt.legs;
						alt.cost = cost; alt.waiting += ride.waiting; alt.riding += ride.riding; alt.first_wait = ride.waiting;
					}
				} else if (this->work.finishing < this->graph->occurrences.size()) {
					uint32_t id = static_cast<uint32_t>(this->work.finishing++); const auto &o = this->graph->occurrences[id];
					if (this->graph->optimized && !o.board) continue;
					uint32_t station = this->graph->station_index[o.station.base()]; if (station == INVALID) continue;
					int64_t best_cost = this->SearchCost(this->work, station), suffix_cost = this->SearchCost(this->work, o.departure);
					if (best_cost == INT64_MAX || suffix_cost == INT64_MAX || suffix_cost >= best_cost) continue;
					uint32_t best_board = this->SearchBoard(this->work, station);
					const auto &p = this->graph->patterns[o.pattern];
					if (best_board == INVALID || this->SearchAlight(this->work, o.departure) == INVALID ||
							this->graph->occurrences[best_board].pattern == o.pattern || suffix_cost > INT64_MAX - p.wait - p.risk) continue;
					int64_t cost = suffix_cost + p.wait + p.risk;
					if (cost - best_cost > best_cost / 4) continue;
					/* Dispersion may pay a modest cost premium, but a waiting-time
					 * estimate alone must not randomly select a much longer ride or
					 * several extra transfers. Faster winning routes stay unrestricted. */
					int64_t best_riding = this->SearchRiding(this->work, station);
					if (this->graph->estimates && (this->SearchRiding(this->work, o.departure) > best_riding + std::max<int64_t>(32, best_riding / 4) ||
							this->SearchLegs(this->work, o.departure) + 1 > this->SearchLegs(this->work, station) + 1)) continue;
					/* Only real Board edges are alternatives; NoLoad stops never enter. */
					if (!o.board) continue;
					auto &alt = this->work.scratch.alternative[station];
					if (cost < alt.cost) {
						alt = this->SearchLabel(this->work, o.departure); alt.board = id; ++alt.legs;
						alt.cost = cost; alt.waiting += p.wait; alt.first_wait = p.wait;
					}
				} else {
					/* Arrival/departure labels are construction scratch. Queries use
					 * only initial/transfer waits and one complete dispersion label. */
					if (this->UseCompressedRows()) {
						if (this->work.publication_cursor == 0) this->PrepareCompressedRow(this->work);
						if (legacy_publication) {
							for (uint32_t origin = 0; origin < this->graph->stations.size(); ++origin) { index_origin(origin); this->CopyStationLabel(this->work, origin); }
						} else if (this->work.publication_cursor < this->graph->waiting_state_count) {
							/* Stage each station's base/exception together, but consume
							 * every original virtual copy before making the row public. */
							size_t count = std::min(this->graph->waiting_state_count - this->work.publication_cursor, allowance - used + 1);
							for (size_t origin = this->work.publication_cursor; origin < std::min(this->graph->stations.size(), this->work.publication_cursor + count); ++origin) {
								index_origin(origin); this->CopyStationLabel(this->work, static_cast<uint32_t>(origin));
							}
							this->work.publication_cursor += count; used += count - 1; continue;
						}
					} else {
						if (legacy_publication) {
							if (this->graph->optimized) {
								for (uint32_t state = 0; state < this->graph->waiting_state_count; ++state) this->work.prepared.labels.push_back(this->SearchLabel(this->work, state));
							} else this->work.prepared.labels.assign(this->work.scratch.labels.begin(), this->work.scratch.labels.begin() + this->graph->waiting_state_count);
							for (size_t from = 0; from < this->graph->stations.size(); ++from) index_origin(from);
						} else if (this->work.prepared.labels.size() < this->graph->waiting_state_count) {
							index_origin(this->work.prepared.labels.size());
							this->work.prepared.labels.push_back(this->SearchLabel(this->work, static_cast<uint32_t>(this->work.prepared.labels.size()))); continue;
						}
						this->work.prepared.alternative = std::move(this->work.scratch.alternative);
					}
					this->rows.push_back(std::move(this->work.prepared)); this->work.publication_cursor = 0;
					this->published_bytes += this->rows.back().AllocatedBytes();
					if (published) published(this->graph->stations[this->destination]);
					++this->destination;
					this->diagnostic.destinations = this->destination;
					if (!this->Finished()) this->StartDestination();
				}
				continue;
			}

		}
	} catch (const std::bad_alloc &) { FatalError("Insufficient memory for passenger routing. Disable passenger_journey_precomputed or free memory before resuming."); }
	this->CollectSearchCounters();
	this->diagnostic.operations += used; this->diagnostic.build_nanoseconds += Elapsed(started);
	this->diagnostic.bytes = this->graph->station_index.capacity() * 4 + this->graph->service_pattern.capacity() * 4 + this->graph->edges.capacity() * sizeof(Edge) +
			(this->graph->incoming.capacity() + this->graph->incoming_offsets.capacity()) * 4 + (this->UseCompressedRows() ? this->published_bytes : this->rows.size() * this->diagnostic.stored_states * sizeof(Label)) +
			(this->work.scratch.labels.capacity() + this->work.scratch.alternative.capacity() + this->work.prepared.labels.capacity()) * sizeof(Label) +
			this->graph->section_rides.capacity() * sizeof(SectionRide) + this->graph->landings.capacity() * sizeof(uint32_t);
	this->diagnostic.bytes += this->graph->hot_edges.from.capacity() * 4 + this->graph->hot_edges.cost.capacity() * 8 + this->graph->hot_edges.legs.capacity() + this->graph->hot_edges.range.capacity() * 4 + this->graph->range_edges.capacity() * sizeof(RangeEdge) + this->graph->cold_edges.capacity() * sizeof(ColdEdge);
	this->diagnostic.bytes += this->graph->binding_bytes + this->reachable_bytes;
	this->diagnostic.bytes += (this->graph->exception_slots.capacity() + this->work.exception_state.capacity()) * sizeof(uint32_t);
	this->diagnostic.bytes += (this->work.prepared.exception_labels.capacity() + this->work.prepared.alternative.capacity() + this->work.scratch.exception_labels.capacity()) * sizeof(Label);
	this->diagnostic.bytes += (this->work.prepared.indices.capacity() + this->work.scratch.indices.capacity()) * sizeof(uint32_t);
	this->diagnostic.bytes += (this->graph->seed_offsets.capacity() + this->graph->seed_occurrences.capacity()) * sizeof(uint32_t);
	this->diagnostic.bytes += this->work.queue.AllocatedBytes();
	this->diagnostic.bytes += this->work.search.cost.capacity() * sizeof(int64_t) + this->work.search.legs.capacity() * sizeof(uint32_t) + this->work.search.metadata.capacity() * sizeof(LabelMetadata);
	return used;
}

void PassengerPrecomputedTable::PushHyperpath(DestinationWorkspace &work, int64_t key, uint32_t kind, uint32_t id) const
{
	this->graph->PushHyperpath(work, key, kind, id);
}

/** Work units: one state initialization, seed, heap pop, incoming descriptor
 * (including generating its Board event), range target, copied label or copied
 * candidate. Row publication and each state-set header also cost one unit.
 * All cursors survive arbitrary slice boundaries; replay uses the saved total.
 * A v14 continuation-witness prefix stop also consumes one Work unit.
 * Event order is (ceil cost, Board/min/strategy, stable edge/state rank). */
size_t PassengerPrecomputedTable::SearchHyperpath(DestinationWorkspace &work, size_t allowance) const
{
	return this->graph->SearchHyperpath(work, allowance);
}

size_t PassengerPrecomputedTable::StepHyperpath(size_t allowance, const std::function<void(StationID)> &published, uint workers)
{
	const auto started = Clock::now(); size_t used = 0;
	try {
		while (used < allowance && !this->Finished()) {
			if (!this->work.finishing_row) {
				used += this->AdvanceSearch(allowance - used, workers); continue;
			}
			++used;
			if (this->work.finishing_row) {
				if (this->work.prepared.labels.size() < this->graph->waiting_state_count) {
					uint32_t state = static_cast<uint32_t>(this->work.prepared.labels.size());
					this->work.prepared.labels.push_back(this->SearchLabel(this->work, state));
					if (state < this->graph->stations.size() && this->SearchCost(this->work, state) != INT64_MAX) {
						auto &destinations = this->reachable_destinations[state]; size_t capacity = destinations.capacity();
						destinations.push_back(this->graph->stations[this->destination]); this->reachable_bytes += (destinations.capacity() - capacity) * sizeof(StationID);
					}
					continue;
				}
				if (this->work.finishing < this->graph->waiting_state_count) {
					uint32_t state = static_cast<uint32_t>(this->work.finishing);
					const auto &candidates = this->work.strategy.candidates[state];
					if (this->work.strategy.offset == 0) {
						++this->diagnostic.candidate_histogram[candidates.size()];
						uint32_t base = this->graph->waiting_station[state];
						if (base != state && candidates == this->work.strategy.candidates[base] && this->SearchCost(this->work, state) == this->SearchCost(this->work, base)) {
							this->work.prepared.indices.push_back(this->work.prepared.indices[base]); ++this->work.finishing; continue;
						}
						if (this->graph->flat_storage) {
							uint32_t previous = this->PreviousPattern(state);
							bool full = state == base;
							/* The ordinary difference is base minus the incoming service,
							 * plus every addition. Keep an exact full payload if a future
							 * recurrence violates that inclusion property. */
							if (!full) for (uint32_t board : this->work.strategy.candidates[base]) if (this->graph->occurrences[board].pattern != previous && !std::binary_search(candidates.begin(), candidates.end(), board)) { full = true; break; }
							if (!full) for (uint32_t board : candidates) if (this->graph->occurrences[board].pattern == previous) { full = true; break; }
							this->work.prepared.indices.push_back(static_cast<uint32_t>(this->work.prepared.flat_policies.size()));
							this->work.prepared.flat_policies.push_back({this->SearchCost(this->work, state), this->work.strategy.sums[state].Wait(), static_cast<uint32_t>(this->work.prepared.flat_candidates.size()), full ? FlatPolicy::FULL : 0});
						} else {
							this->work.prepared.indices.push_back(static_cast<uint32_t>(this->work.prepared.policies.size()));
							this->work.prepared.policies.push_back({this->SearchCost(this->work, state), this->work.strategy.sums[state].Wait(), {}});
							this->work.prepared.policies.back().candidates.reserve(candidates.size());
							this->work.prepared_candidate_bytes += this->work.prepared.policies.back().candidates.capacity() * 4;
						}
						++this->work.strategy.offset; continue;
					}
					if (this->work.strategy.offset <= candidates.size()) {
						uint32_t board = candidates[this->work.strategy.offset++ - 1];
						assert(this->EvaluateRide(this->work.prepared, board, this->graph->stations[this->destination]).cost ==
								this->SearchCost(this->work, this->graph->occurrences[board].departure) + this->graph->patterns[this->graph->occurrences[board].pattern].risk);
						this->MarkBoardingCandidate(board, state < this->graph->stations.size()); // A shared state equals its base, marked in both planes.
						if (this->graph->flat_storage) {
							auto &policy = this->work.prepared.flat_policies.back(); const auto &base = this->work.strategy.candidates[this->graph->waiting_station[state]];
							if ((policy.count & FlatPolicy::FULL) || !std::binary_search(base.begin(), base.end(), board)) {
								size_t capacity = this->work.prepared.flat_candidates.capacity(); this->work.prepared.flat_candidates.push_back(board); ++policy.count;
								this->work.prepared_candidate_bytes += (this->work.prepared.flat_candidates.capacity() - capacity) * 4;
							}
						} else this->work.prepared.policies.back().candidates.push_back(board);
						continue;
					}
					++this->diagnostic.policy_count; this->diagnostic.candidate_count += candidates.size();
					this->diagnostic.max_candidates = std::max(this->diagnostic.max_candidates, candidates.size());
					auto bytes = this->graph->flat_storage ? sizeof(FlatPolicy) + (this->work.prepared.flat_policies.back().count & FlatPolicy::MASK) * 4 : sizeof(Policy) + candidates.size() * 4;
					if (state < this->graph->stations.size()) this->diagnostic.base_policy_bytes += bytes;
					else this->diagnostic.difference_policy_bytes += bytes;
					++this->work.finishing; this->work.strategy.offset = 0; continue;
				}
				if (this->graph->flat_storage) {
					auto exact = [](auto &working, auto &buffer) { buffer.swap(working); working.assign(buffer.begin(), buffer.end()); buffer.clear(); };
					exact(this->work.prepared.flat_policies, this->work.flat_policy_buffer); exact(this->work.prepared.flat_candidates, this->work.flat_candidate_buffer);
					this->work.prepared_candidate_bytes = this->work.prepared.flat_candidates.capacity() * 4;
				}
				this->work.prepared.bytes = this->work.prepared.labels.capacity() * sizeof(Label) + this->work.prepared.indices.capacity() * 4 +
						this->work.prepared.policies.capacity() * sizeof(Policy) + this->work.prepared.flat_policies.capacity() * sizeof(FlatPolicy) + this->work.prepared_candidate_bytes;
				this->hyperpath_row_bytes += this->work.prepared.bytes;
				if (this->boarding_mask_words == 0) this->MarkBoardingCandidate(INVALID, false); // Allocate even if no row had a candidate yet.
				if (this->boarding_mask_words != 0) this->published_rows[this->destination / 64] |= uint64_t{1} << (this->destination % 64);
				this->rows.push_back(std::move(this->work.prepared));
				this->work.prepared_candidate_bytes = 0;
				if (published) published(this->graph->stations[this->destination]);
				++this->destination; this->diagnostic.destinations = this->destination;
				if (!this->Finished()) this->StartDestination();
				continue;
			}

		}
	} catch (const std::bad_alloc &) { FatalError("Insufficient memory for passenger strategies."); }
	this->CollectSearchCounters();
	this->diagnostic.operations += used; this->diagnostic.build_nanoseconds += Elapsed(started);
	this->diagnostic.bytes = this->hyperpath_static_bytes + this->hyperpath_row_bytes + this->work.hyperpath_candidate_bytes + this->local_policy_bytes + this->expanded_policy_bytes + this->boarding_mask.capacity() * sizeof(uint64_t) +
			this->reachable_bytes + this->reachable_destinations.capacity() * sizeof(this->reachable_destinations[0]) + this->work.queue.AllocatedBytes() +
			(this->work.scratch.labels.capacity() + this->work.scratch.alternative.capacity()) * sizeof(Label) +
			this->work.search.cost.capacity() * 8 + this->work.search.legs.capacity() * 4 + this->work.search.metadata.capacity() * sizeof(LabelMetadata) +
			this->work.strategy.sums.capacity() * sizeof(PassengerStrategy) + this->work.strategy.candidates.capacity() * sizeof(this->work.strategy.candidates[0]) +
			this->work.strategy.settled.capacity() / 8 + this->work.strategy.events.AllocatedBytes() + this->work.prepared_candidate_bytes +
			this->work.flat_policy_buffer.capacity() * sizeof(FlatPolicy) + this->work.flat_candidate_buffer.capacity() * 4 +
			this->work.prepared.labels.capacity() * sizeof(Label) + this->work.prepared.indices.capacity() * 4 + this->work.prepared.policies.capacity() * sizeof(Policy) + this->work.prepared.flat_policies.capacity() * sizeof(FlatPolicy);
	return used;
}

PassengerHyperpathMemoryDiagnostic PassengerPrecomputedTable::InspectMemory() const
{
	PassengerHyperpathMemoryDiagnostic result;
	if (!this->graph->hyperpath) return result;
	result.reported = this->diagnostic.bytes; result.graph = this->hyperpath_static_bytes; result.local = this->local_policy_bytes + this->expanded_policy_bytes;
	std::vector<uint32_t> expanded;
	auto inspect = [&](const Row &row, bool differences) {
		result.labels += row.labels.capacity() * sizeof(Label);
		result.indices += row.indices.capacity() * sizeof(uint32_t);
		result.policy_headers += row.policies.capacity() * sizeof(Policy) + row.flat_policies.capacity() * sizeof(FlatPolicy);
		result.candidate_arrays += row.flat_candidates.capacity() * sizeof(uint32_t);
		for (const auto &policy : row.policies) result.candidate_arrays += policy.candidates.capacity() * sizeof(uint32_t);
		if (!differences || row.indices.empty()) return;
		for (uint32_t station = 0; station < this->graph->waiting_states.size(); ++station) for (const auto &[previous, state] : this->graph->waiting_states[station]) {
			if (row.indices[state] == row.indices[station]) continue;
			++result.differences;
			const auto base = this->graph->flat_storage ? this->FlatPayload(row, row.indices[station]) : std::span<const uint32_t>(row.policies[row.indices[station]].candidates);
			std::span<const uint32_t> variant;
			if (this->graph->flat_storage) { this->ExpandPolicy(row, state, expanded); variant = expanded; }
			else variant = row.policies[row.indices[state]].candidates;
			uint64_t additions = 0;
			for (uint32_t board : variant) additions += !std::binary_search(base.begin(), base.end(), board);
			result.additions += additions; result.maximum_additions = std::max(result.maximum_additions, additions);
			result.multi_addition_differences += additions > 1;
			for (uint32_t board : base) if (this->graph->occurrences[board].pattern != previous && !std::binary_search(variant.begin(), variant.end(), board)) ++result.unexpected_removals;
		}
	};
	for (const auto &row : this->rows) inspect(row, true);
	inspect(this->work.prepared, false);
	/* Cost + wait + an offset/count pair for a flat addition array. Previous
	 * service comes from the waiting state. This is a representation estimate,
	 * excluding allocator overhead and any cache needed by span-based callers. */
	result.overlay_bytes = result.differences * 24 + result.additions * 4;
	return result;
}

void PassengerPrecomputedTable::PrepareRetirement(const PassengerPrecomputedTable &replacement)
{
	/* The replacement selects the publication engine. Its retained rows may
	 * originate in a v9/v10 save; their construction/replay remains untouched. */
	if (!replacement.graph->integrated || this->retirement_prepared || this->rows.empty()) return;
	assert(!replacement.IsPreparingGraph());
	std::vector<StationID> missing;
	std::set_difference(this->graph->stations.begin(), this->graph->stations.end(), replacement.graph->stations.begin(), replacement.graph->stations.end(), std::back_inserter(missing));
	if (!missing.empty()) {
		this->retirement_survivors.resize(this->reachable_destinations.size());
		for (size_t origin = 0; origin < this->reachable_destinations.size(); ++origin) {
			const auto &reachable = this->reachable_destinations[origin];
			auto &survivors = this->retirement_survivors[origin];
			for (StationID destination : missing) {
				if (std::binary_search(reachable.begin(), reachable.end(), destination)) survivors.push_back(destination);
			}
			this->reachable_bytes += survivors.capacity() * sizeof(StationID);
			this->diagnostic.bytes += survivors.capacity() * sizeof(StationID);
		}
		this->reachable_bytes += this->retirement_survivors.capacity() * sizeof(std::vector<StationID>);
		this->diagnostic.bytes += this->retirement_survivors.capacity() * sizeof(std::vector<StationID>);
	}
	this->retirement_prepared = true;
}

void PassengerPrecomputedTable::ReuseRetainedDestinations()
{
	if (!this->retirement_prepared) return;
	/* Another cargo may have rejected its replacement. All common rows of
	 * this cargo's completed replacement are retired; only its missing stations
	 * remain. Compact that small set before restarting the ascending watermark. */
	size_t freed = this->retirement_survivors.capacity() * sizeof(std::vector<StationID>);
	for (size_t origin = 0; origin < this->reachable_destinations.size(); ++origin) {
		auto &reachable = this->reachable_destinations[origin];
		if (this->retirement_survivors.empty()) {
			reachable.clear();
		} else {
			const auto &survivors = this->retirement_survivors[origin];
			assert(survivors.size() <= reachable.capacity());
			reachable.assign(survivors.begin(), survivors.end());
			freed += survivors.capacity() * sizeof(StationID);
		}
	}
	decltype(this->retirement_survivors){}.swap(this->retirement_survivors);
	this->reachable_bytes -= freed;
	this->diagnostic.bytes -= std::min(this->diagnostic.bytes, freed);
	this->retirement_watermark.reset(); this->retirement_prepared = false;
}

void PassengerPrecomputedTable::ReleaseDestination(StationID destination)
{
	if (this->retirement_prepared) {
		assert(!this->retirement_watermark || *this->retirement_watermark < destination);
		this->retirement_watermark = destination;
	}
	if (this->graph->hyperpath) {
		/* Both caches are keyed by destination first: erase that range only. */
		for (auto it = this->local_policies.lower_bound({destination, 0, OrderListID::Begin()}); it != this->local_policies.end() && std::get<0>(it->first) == destination;) {
			size_t bytes = 128 + it->second.candidates.capacity() * 4;
			this->local_policy_bytes -= bytes; this->diagnostic.bytes -= std::min(this->diagnostic.bytes, bytes);
			it = this->local_policies.erase(it);
		}
		for (auto it = this->station_choices.lower_bound({destination, 0}); it != this->station_choices.end() && it->first.first == destination;) {
			size_t bytes = 128 + it->second.capacity() * sizeof(it->second[0]);
			this->local_policy_bytes -= bytes; this->diagnostic.bytes -= std::min(this->diagnostic.bytes, bytes);
			it = this->station_choices.erase(it);
		}
	}
	for (auto it = this->expanded_policies.lower_bound({destination, 0}); it != this->expanded_policies.end() && it->first.first == destination;) {
		size_t bytes = 128 + it->second.capacity() * 4; this->expanded_policy_bytes -= bytes; this->diagnostic.bytes -= std::min(this->diagnostic.bytes, bytes);
		it = this->expanded_policies.erase(it);
	}
	if (destination.base() >= this->graph->station_index.size()) return;
	uint32_t row = this->graph->station_index[destination.base()];
	if (row < this->rows.size() && !this->rows[row].Empty()) {
		/* Hyperpath rows never enter published_bytes; they report their own charge. */
		const size_t bytes = this->graph->hyperpath ? this->rows[row].bytes : this->rows[row].AllocatedBytes();
		if (this->graph->hyperpath) this->hyperpath_row_bytes -= bytes; else this->published_bytes -= bytes;
		this->diagnostic.bytes -= std::min(this->diagnostic.bytes, bytes);
		this->rows[row] = {};
		if (this->boarding_mask_words != 0) this->published_rows[row / 64] &= ~(uint64_t{1} << (row % 64));
		if (!this->retirement_prepared) for (auto &destinations : this->reachable_destinations) {
			auto found = std::lower_bound(destinations.begin(), destinations.end(), destination);
			if (found != destinations.end() && *found == destination) destinations.erase(found);
		}
		--this->diagnostic.destinations;
	}
}

PassengerPrecomputedTable::DestinationSpans PassengerPrecomputedTable::ReachableDestinations(StationID origin) const
{
	if (origin.base() >= this->graph->station_index.size()) return {};
	uint32_t from = this->graph->station_index[origin.base()];
	if (from >= this->reachable_destinations.size()) return {};
	auto result = std::span<const StationID>(this->reachable_destinations[from]);
	/* A sliced copy can have staged one entry for the still-private row. */
	if (this->destination >= this->rows.size() && this->destination < this->graph->stations.size() && !result.empty() && result.back() == this->graph->stations[this->destination]) result = result.first(result.size() - 1);
	if (!this->retirement_watermark) return {std::span<const StationID>{}, result};
	const StationID watermark = *this->retirement_watermark;
	/* StationID successors are representable: a published ID is never Invalid. */
	auto suffix = result.subspan(std::lower_bound(result.begin(), result.end(), StationID{static_cast<uint16_t>(watermark.base() + 1)}) - result.begin());
	std::span<const StationID> survivors;
	if (!this->retirement_survivors.empty()) {
		survivors = this->retirement_survivors[from];
		survivors = survivors.first(std::upper_bound(survivors.begin(), survivors.end(), watermark) - survivors.begin());
	}
	return {survivors, suffix};
}

std::optional<int64_t> PassengerPrecomputedTable::RouteCost(StationID origin, StationID destination) const
{
	if (this->failed || origin.base() >= this->graph->station_index.size() || destination.base() >= this->graph->station_index.size()) return {};
	uint32_t from = this->graph->station_index[origin.base()], to = this->graph->station_index[destination.base()];
	if (from == INVALID || to >= this->rows.size() || this->rows[to].Empty()) return {};
	return this->RowLabel(this->rows[to], from, from).cost;
}

const PassengerPrecomputedTable::Row *PassengerPrecomputedTable::FindRow(StationID destination) const
{
	if (!this->graph->hyperpath || this->failed || destination.base() >= this->graph->station_index.size()) return nullptr;
	uint32_t row = this->graph->station_index[destination.base()];
	return row < this->rows.size() && !this->rows[row].indices.empty() ? &this->rows[row] : nullptr;
}

uint32_t PassengerPrecomputedTable::PolicyState(StationID origin, OrderListID previous) const
{
	if (origin.base() >= this->graph->station_index.size()) return INVALID;
	uint32_t station = this->graph->station_index[origin.base()];
	if (station == INVALID) return INVALID;
	uint32_t pattern = previous.base() < this->graph->service_pattern.size() ? this->graph->service_pattern[previous.base()] : INVALID;
	return pattern == INVALID ? station : this->StateFor(station, pattern);
}

std::optional<int64_t> PassengerPrecomputedTable::ServiceWait(OrderListID service) const
{
	if (!this->graph->hyperpath || service.base() >= this->graph->service_pattern.size()) return {};
	uint32_t pattern = this->graph->service_pattern[service.base()];
	return pattern == INVALID ? std::nullopt : std::optional{this->graph->patterns[pattern].wait};
}

uint32_t PassengerPrecomputedTable::BoardingOccurrence(StationID origin, OrderListID service, VehicleOrderID order) const
{
	if (!this->graph->hyperpath || service.base() >= this->graph->service_pattern.size()) return INVALID;
	uint32_t pattern = this->graph->service_pattern[service.base()];
	if (pattern == INVALID) return INVALID;
	const auto &stops = this->graph->patterns[pattern].occurrences;
	auto at = std::lower_bound(stops.begin(), stops.end(), order, [&](uint32_t id, VehicleOrderID value) { return this->graph->occurrences[id].order < value; });
	return at != stops.end() && this->graph->occurrences[*at].order == order && this->graph->occurrences[*at].station == origin && this->graph->occurrences[*at].board ? *at : INVALID;
}

void PassengerPrecomputedTable::MarkBoardingCandidate(uint32_t occurrence, bool base_state)
{
	if (this->boarding_mask_rejected) return;
	if (this->boarding_mask_words == 0) {
		const size_t words = (this->graph->stations.size() + 63) / 64;
		if (words == 0 || this->graph->occurrences.size() > MAX_BOARDING_MASK_BYTES / sizeof(uint64_t) / 2 / words) { this->boarding_mask_rejected = true; return; }
		this->boarding_mask.assign(this->graph->occurrences.size() * 2 * words, 0);
		this->published_rows.assign(words, 0);
		this->boarding_mask_words = words;
	}
	if (occurrence >= this->graph->occurrences.size()) return;
	const uint64_t bit = uint64_t{1} << (this->destination % 64);
	const uint32_t row = static_cast<uint32_t>(this->destination);
	if (base_state) this->boarding_mask[this->BoardingMaskWord(occurrence, false, row)] |= bit;
	this->boarding_mask[this->BoardingMaskWord(occurrence, true, row)] |= bit;
}

bool PassengerPrecomputedTable::MayBeStoredCandidate(uint32_t occurrence, StationID destination, bool any_state) const
{
	if (this->boarding_mask_words == 0 || occurrence >= this->graph->occurrences.size() || destination.base() >= this->graph->station_index.size()) return false;
	const uint32_t row = this->graph->station_index[destination.base()];
	if (row == INVALID) return false;
	return ((this->boarding_mask[this->BoardingMaskWord(occurrence, any_state, row)] >> (row % 64)) & 1) != 0;
}

bool PassengerPrecomputedTable::BoardingMaskCovers() const
{
	/* A rejected or unpublished table yields neither plans nor stored membership. */
	if (this->failed || this->rows.empty()) return true;
	return this->graph->hyperpath && this->boarding_mask_words != 0;
}

void PassengerPrecomputedTable::StoredCandidates(StationID origin, StationID destination, OrderListID previous, std::vector<uint32_t> &out) const
{
	const auto *row = this->FindRow(destination); const uint32_t state = this->PolicyState(origin, previous);
	if (row == nullptr || state == INVALID) return;
	/* The same exclusions as ContainsCached and ContainsStored, enumerated instead of queried. */
	auto emit = [&](uint32_t occurrence) {
		const auto &board = this->graph->occurrences[occurrence];
		if (board.station == origin && this->graph->patterns[board.pattern].members.front().id != previous) out.push_back(occurrence);
	};
	const uint32_t index = row->indices[state];
	if (!this->graph->flat_storage) {
		for (uint32_t occurrence : row->policies[index].candidates) emit(occurrence);
		return;
	}
	const auto payload = this->FlatPayload(*row, index);
	if ((row->flat_policies[index].count & FlatPolicy::FULL) == 0) {
		const uint32_t previous_pattern = this->PreviousPattern(state);
		for (uint32_t occurrence : this->FlatPayload(*row, row->indices[this->graph->waiting_station[state]])) {
			if (this->graph->occurrences[occurrence].pattern != previous_pattern) emit(occurrence);
		}
	}
	for (uint32_t occurrence : payload) emit(occurrence);
}

std::optional<bool> PassengerPrecomputedTable::ContainsCached(StationID origin, StationID destination, uint32_t occurrence, OrderListID previous, OrderListID failed) const
{
	if (failed == OrderListID::Invalid() && previous == OrderListID::Invalid() && this->boarding_mask_words != 0 && this->graph->hyperpath && !this->failed) {
		/* The station's base state: its base plane bit is exactly the stored base policy of a
		 * published row, read from two small arrays instead of the row's policy storage. */
		if (occurrence >= this->graph->occurrences.size() || this->graph->occurrences[occurrence].station != origin || destination.base() >= this->graph->station_index.size()) return false;
		const uint32_t row = this->graph->station_index[destination.base()];
		if (row == INVALID || ((this->published_rows[row / 64] >> (row % 64)) & 1) == 0) return false;
		return ((this->boarding_mask[this->BoardingMaskWord(occurrence, false, row)] >> (row % 64)) & 1) != 0;
	}
	const auto *row = this->FindRow(destination); uint32_t state = this->PolicyState(origin, previous);
	if (row == nullptr || state == INVALID || occurrence >= this->graph->occurrences.size()) return false;
	const auto &board = this->graph->occurrences[occurrence];
	if (board.station != origin || this->graph->patterns[board.pattern].members.front().id == previous) return false;
	std::span<const uint32_t> candidates;
	if (failed != OrderListID::Invalid()) {
		auto found = this->local_policies.find({destination, state, failed});
		/* A cold/evicted local policy is unknown, not a negative membership.
		 * Its deterministic construction belongs to the selected FIFO cohort.
		 * Enumerating an unrelated cohort must never run EvaluateRide. */
		if (found == this->local_policies.end()) return {};
		candidates = found->second.candidates;
	} else {
		return this->ContainsStored(*row, state, occurrence);
	}
	return std::binary_search(candidates.begin(), candidates.end(), occurrence);
}

std::span<const uint32_t> PassengerPrecomputedTable::FlatPayload(const Row &row, uint32_t index) const
{
	const auto &policy = row.flat_policies[index];
	return std::span<const uint32_t>(row.flat_candidates).subspan(policy.offset, policy.count & FlatPolicy::MASK);
}

uint32_t PassengerPrecomputedTable::PreviousPattern(uint32_t state) const
{
	uint32_t station = this->graph->waiting_station[state];
	if (state == station) return INVALID;
	const auto &waits = this->graph->waiting_states[station];
	assert(!waits.empty() && state >= waits.front().second);
	size_t at = state - waits.front().second;
	assert(at < waits.size() && waits[at].second == state);
	return waits[at].first;
}

bool PassengerPrecomputedTable::ContainsStored(const Row &row, uint32_t state, uint32_t occurrence) const
{
	if (state == INVALID) return false;
	uint32_t index = row.indices[state];
	if (!this->graph->flat_storage) {
		const auto &candidates = row.policies[index].candidates;
		return std::binary_search(candidates.begin(), candidates.end(), occurrence);
	}
	const auto payload = this->FlatPayload(row, index);
	if (row.flat_policies[index].count & FlatPolicy::FULL) return std::binary_search(payload.begin(), payload.end(), occurrence);
	if (this->graph->occurrences[occurrence].pattern == this->PreviousPattern(state)) return false;
	const auto base = this->FlatPayload(row, row.indices[this->graph->waiting_station[state]]);
	return std::binary_search(payload.begin(), payload.end(), occurrence) || std::binary_search(base.begin(), base.end(), occurrence);
}

std::pair<int64_t, int64_t> PassengerPrecomputedTable::PolicyValues(const Row &row, uint32_t state) const
{
	uint32_t index = row.indices[state];
	if (this->graph->flat_storage) return {row.flat_policies[index].cost, row.flat_policies[index].wait};
	return {row.policies[index].cost, row.policies[index].wait};
}

void PassengerPrecomputedTable::ExpandPolicy(const Row &row, uint32_t state, std::vector<uint32_t> &result) const
{
	const auto index = row.indices[state]; const auto payload = this->FlatPayload(row, index);
	result.clear();
	if (row.flat_policies[index].count & FlatPolicy::FULL) { result.assign(payload.begin(), payload.end()); return; }
	const auto base = this->FlatPayload(row, row.indices[this->graph->waiting_station[state]]);
	uint32_t previous = this->PreviousPattern(state);
	for (uint32_t board : base) if (this->graph->occurrences[board].pattern != previous) result.push_back(board);
	result.insert(result.end(), payload.begin(), payload.end());
	std::sort(result.begin(), result.end()); result.erase(std::unique(result.begin(), result.end()), result.end());
}

std::span<const uint32_t> PassengerPrecomputedTable::RowCandidates(const Row &row, StationID destination, uint32_t state) const
{
	uint32_t index = row.indices[state];
	if (!this->graph->flat_storage) return row.policies[index].candidates;
	if (row.flat_policies[index].count & FlatPolicy::FULL) return this->FlatPayload(row, index);
	auto key = std::pair(destination, state); auto found = this->expanded_policies.find(key);
	if (found != this->expanded_policies.end()) return found->second;
	std::vector<uint32_t> candidates; this->ExpandPolicy(row, state, candidates);
	if (this->expanded_policies.size() >= 4096) {
		size_t bytes = 128 + this->expanded_policies.begin()->second.capacity() * 4;
		this->expanded_policy_bytes -= bytes; this->diagnostic.bytes -= std::min(this->diagnostic.bytes, bytes);
		this->expanded_policies.erase(this->expanded_policies.begin());
	}
	size_t bytes = 128 + candidates.capacity() * 4;
	this->expanded_policy_bytes += bytes; this->diagnostic.bytes += bytes;
	return this->expanded_policies.emplace(key, std::move(candidates)).first->second;
}

std::vector<uint64_t> PassengerPrecomputedTable::InspectBuildingPolicyRow(StationID destination) const
{
	const auto *row = this->FindRow(destination);
	if (row == nullptr || this->destination >= this->graph->stations.size() || destination != this->graph->stations[this->destination]) return {};
	std::vector<uint64_t> result{destination.base(), this->graph->waiting_state_count}; std::vector<uint32_t> expanded;
	for (uint32_t state = 0; state < this->graph->waiting_state_count; ++state) {
		std::span<const uint32_t> candidates;
		if (this->graph->flat_storage) { this->ExpandPolicy(*row, state, expanded); candidates = expanded; }
		else candidates = row->policies[row->indices[state]].candidates;
		auto [cost, wait] = this->PolicyValues(*row, state); const auto &sum = this->work.strategy.sums[state];
		result.insert(result.end(), {state, uint64_t(cost), uint64_t(wait), sum.numerator.high, sum.numerator.low, sum.frequency, candidates.size()});
		for (uint32_t board : candidates) result.push_back(board);
		const auto &label = row->labels[state];
		result.insert(result.end(), {uint64_t(label.cost), uint64_t(label.riding), uint64_t(label.waiting), uint64_t(label.first_wait), label.board, label.alight, label.legs, label.distance});
	}
	return result;
}

std::span<const uint32_t> PassengerPrecomputedTable::Candidates(StationID origin, StationID destination, OrderListID previous, OrderListID failed) const
{
	const auto *row = this->FindRow(destination); uint32_t state = this->PolicyState(origin, previous);
	if (row == nullptr || state == INVALID) return {};
	if (failed != OrderListID::Invalid()) return this->LocalPolicy(*row, destination, state, failed).candidates;
	return this->RowCandidates(*row, destination, state);
}

PassengerPrecomputedTable::DisplayRead PassengerPrecomputedTable::NextDisplayCandidate(StationID origin, StationID destination,
		OrderListID previous, OrderListID failed, DisplayCursor &cursor, DisplayCandidate &out, size_t &work) const
{
	if (!cursor.bound || cursor.origin != origin || cursor.destination != destination || cursor.previous != previous || cursor.failed != failed) {
		cursor.phase = 0; cursor.occurrence = cursor.member = 0; cursor.inspected = false; cursor.bound = true;
		cursor.origin = origin; cursor.destination = destination; cursor.previous = previous; cursor.failed = failed;
	}
	const auto *row = this->FindRow(destination); const uint32_t state = this->PolicyState(origin, previous);
	if (!this->graph->hyperpath || row == nullptr || state == INVALID) return DisplayRead::Unavailable;
	std::array<std::span<const uint32_t>, 2> spans{};
	uint32_t excluded = INVALID;
	size_t phases = 1;
	if (failed != OrderListID::Invalid()) {
		auto found = this->local_policies.find({destination, state, failed});
		if (found == this->local_policies.end()) return DisplayRead::Unavailable;
		spans[0] = found->second.candidates;
	} else {
		const uint32_t index = row->indices[state];
		if (!this->graph->flat_storage) spans[0] = row->policies[index].candidates;
		else if (row->flat_policies[index].count & FlatPolicy::FULL) spans[0] = this->FlatPayload(*row, index);
		else {
			spans[0] = this->FlatPayload(*row, row->indices[this->graph->waiting_station[state]]);
			spans[1] = this->FlatPayload(*row, index); phases = 2; excluded = this->PreviousPattern(state);
		}
	}
	while (cursor.phase < phases) {
		const auto candidates = spans[cursor.phase];
		if (cursor.occurrence >= candidates.size()) { ++cursor.phase; cursor.occurrence = cursor.member = 0; cursor.inspected = false; continue; }
		const auto &o = this->graph->occurrences[candidates[cursor.occurrence]]; const auto &p = this->graph->patterns[o.pattern];
		if (!cursor.inspected) {
			if (work == 0) return DisplayRead::Partial;
			--work; cursor.inspected = true;
			if ((cursor.phase == 0 && o.pattern == excluded) || o.station != origin || p.members.front().id == previous) {
				++cursor.occurrence; cursor.inspected = false; continue;
			}
		}
		if (cursor.member >= p.members.size()) { ++cursor.occurrence; cursor.member = 0; cursor.inspected = false; continue; }
		if (work == 0) return DisplayRead::Partial;
		--work;
		const auto &m = *this->FindMember(p.members[cursor.member++].id);
		/* The cached policy already applies failure exclusion, including no-alternative retries. */
		if (m.id != previous) { out = {m.id, m.incarnation, m.generation, o.order}; return DisplayRead::Candidate; }
	}
	return DisplayRead::Complete;
}

bool PassengerPrecomputedTable::DisplayCandidates(StationID origin, StationID destination, OrderListID previous, OrderListID failed,
		std::vector<DisplayCandidate> &out, size_t &work) const
{
	out.clear(); DisplayCursor cursor; DisplayCandidate candidate;
	while (out.size() < 256) {
		auto result = this->NextDisplayCandidate(origin, destination, previous, failed, cursor, candidate, work);
		if (result == DisplayRead::Complete) return true;
		if (result != DisplayRead::Candidate) return false;
		out.push_back(candidate);
	}
	return false;
}

const std::vector<std::pair<int64_t, uint32_t>> &PassengerPrecomputedTable::StationChoices(const Row &row, StationID destination, uint32_t station) const
{
	auto key = std::pair(destination, station);
	auto found = this->station_choices.find(key);
	if (found != this->station_choices.end()) return found->second;
	std::vector<std::pair<int64_t, uint32_t>> choices;
	for (uint32_t board : this->graph->station_boarding[station]) {
		auto label = this->EvaluateRide(row, board, destination);
		if (!this->graph->revisits.empty() && this->graph->revisits[board].next != INVALID && this->DominatedRevisit(board,
				label.cost, this->EvaluateRide(row, this->graph->revisits[board].next, destination).cost) &&
				(!this->graph->continuation_witness || this->HasSameRunContinuationWitness(board, label.alight))) continue;
		if (label.cost != INT64_MAX) choices.emplace_back(label.cost, board);
	}
	std::sort(choices.begin(), choices.end());
	if (this->station_choices.size() >= 16384) {
		/* Evict the next key rather than always the lowest destinations. Residency is derived. */
		auto victim = this->station_choices.upper_bound(key);
		if (victim == this->station_choices.end()) victim = this->station_choices.begin();
		size_t bytes = 128 + victim->second.capacity() * sizeof(choices[0]);
		this->local_policy_bytes -= bytes; this->diagnostic.bytes -= std::min(this->diagnostic.bytes, bytes);
		this->station_choices.erase(victim);
	}
	size_t bytes = 128 + choices.capacity() * sizeof(choices[0]);
	this->local_policy_bytes += bytes; this->diagnostic.bytes += bytes;
	return this->station_choices.emplace(key, std::move(choices)).first->second;
}

const PassengerPrecomputedTable::Policy &PassengerPrecomputedTable::LocalPolicy(const Row &row, StationID destination, uint32_t state, OrderListID failed) const
{
	auto key = std::tuple(destination, state, failed);
	auto found = this->local_policies.find(key);
	if (found != this->local_policies.end()) return found->second;
	const auto started = Clock::now();
	/* Every state and failed carrier of a station shares its rides' costs: evaluate them once.
	 * Filtering a sorted list keeps the order of the former per-state sort exactly. */
	const uint32_t station = this->graph->waiting_station[state];
	const auto &choices = this->StationChoices(row, destination, station);
	Policy policy;
	auto compute = [&](bool exclude) {
		PassengerStrategy sum; policy.candidates.clear();
		for (auto [cost, board] : choices) {
			const auto &o = this->graph->occurrences[board];
			if (state != station && this->StateFor(station, o.pattern) == state) continue;
			const auto &p = this->graph->patterns[o.pattern];
			if (exclude && p.members.front().id == failed) continue;
			if (sum.Add(cost, PassengerStrategy::Frequency(p.wait))) policy.candidates.push_back(board);
		}
		policy.cost = sum.Cost(); policy.wait = sum.Wait();
	};
	compute(true);
	if (policy.candidates.empty()) compute(false); // Preserve reachability when there is no alternative.
	std::sort(policy.candidates.begin(), policy.candidates.end());
	if (this->local_policies.size() >= 4096) {
		/* Evict the next key rather than always the lowest destinations. An evicted policy is only
		 * "unknown" to ContainsCached, so residency never changes a decision. */
		auto victim = this->local_policies.upper_bound(key);
		if (victim == this->local_policies.end()) victim = this->local_policies.begin();
		size_t bytes = 128 + victim->second.candidates.capacity() * 4;
		this->local_policy_bytes -= bytes; this->diagnostic.bytes -= std::min(this->diagnostic.bytes, bytes);
		this->local_policies.erase(victim);
	}
	size_t bytes = 128 + policy.candidates.capacity() * 4;
	this->local_policy_bytes += bytes; this->diagnostic.bytes += bytes;
	++this->diagnostic.local_replans; this->diagnostic.local_replan_nanoseconds += Elapsed(started);
	return this->local_policies.emplace(key, std::move(policy)).first->second;
}

bool PassengerPrecomputedTable::DominatedRevisit(uint32_t board, int64_t cost, int64_t next_cost) const
{
	return this->graph->DominatedRevisit(board, cost, next_cost);
}

bool PassengerPrecomputedTable::HasSameRunContinuationWitness(uint32_t board, uint32_t alight) const
{
	return this->graph->HasSameRunContinuationWitness(board, alight);
}

/** The forward form of the same frozen Ride/Alight recurrence used by the
 * reverse search. Dwell belongs to continuing aboard, uncertainty to travel,
 * and risk to boarding. An actual destination seed has zero alighting cost.
 * Previous-service state, forced unloading, NoUnload and occurrence identity
 * are retained even if the search contracted unobservable intermediate nodes. */
PassengerPrecomputedTable::Label PassengerPrecomputedTable::EvaluateRide(const Row &row, uint32_t board, StationID destination) const
{
	const auto &first = this->graph->occurrences[board]; const auto &pattern = this->graph->patterns[first.pattern];
	const auto &stops = pattern.occurrences;
	auto start = std::lower_bound(stops.begin(), stops.end(), board);
	if (start == stops.end() || *start != board || !first.board) return {};
	size_t at = start - stops.begin(); int64_t cost = pattern.risk, riding = 0; uint32_t distance = 0;
	Label best;
	for (size_t step = 0; step < stops.size(); ++step) {
		const auto &from = this->graph->occurrences[stops[at]];
		if (!from.continues || (step != 0 && from.forced)) break;
		if (step != 0) {
			if (cost > INT64_MAX - from.dwell) break;
			cost += from.dwell; riding += from.dwell;
		}
		if (cost > INT64_MAX - from.travel || cost + from.travel > INT64_MAX - from.uncertainty) break;
		cost += from.travel + from.uncertainty; riding += from.travel;
		distance = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(distance) + from.distance));
		at = (at + 1) % stops.size(); const auto &to = this->graph->occurrences[stops[at]];
		if (!to.alight) continue;
		Label suffix = row.labels[this->StateFor(this->graph->station_index[to.station.base()], first.pattern)];
		int64_t alight = this->graph->transfer_cost;
		if (to.station == destination) { suffix = {}; suffix.cost = 0; alight = 0; }
		if (suffix.cost > INT64_MAX - alight || suffix.cost + alight > INT64_MAX - cost) continue;
		Label candidate = suffix; candidate.cost += alight + cost; ++candidate.legs;
		candidate.board = board; candidate.alight = stops[at]; candidate.riding += riding;
		candidate.distance = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(distance) + candidate.distance));
		candidate.waiting += alight / 2;
		bool earlier_tie = this->graph->continuation_witness && !this->graph->revisits.empty() && this->graph->revisits[board].next != INVALID;
		if (earlier_tie ? candidate.cost < best.cost : std::tie(candidate.cost, candidate.legs) < std::tie(best.cost, best.legs)) best = candidate;
	}
	return best;
}

PassengerPrecomputedPlan PassengerPrecomputedTable::Board(StationID origin, StationID destination, StateTicks ready,
		OrderListID service, VehicleOrderID order, OrderListID previous, OrderListID failed) const
{
	const auto *row = this->FindRow(destination);
	if (row == nullptr) return {};
	if (service == previous || service.base() >= this->graph->service_pattern.size()) return {PassengerPrecomputedPlan::State::NoRoute};
	uint32_t pattern = this->graph->service_pattern[service.base()];
	if (pattern == INVALID) return {PassengerPrecomputedPlan::State::NoRoute};
	const auto &stops = this->graph->patterns[pattern].occurrences;
	auto at = std::lower_bound(stops.begin(), stops.end(), order, [&](uint32_t id, VehicleOrderID value) { return this->graph->occurrences[id].order < value; });
	if (at == stops.end() || this->graph->occurrences[*at].order != order || this->graph->occurrences[*at].station != origin) return {PassengerPrecomputedPlan::State::NoRoute};
	bool member;
	if (failed == OrderListID::Invalid()) member = this->ContainsStored(*row, this->PolicyState(origin, previous), *at);
	else { const auto set = this->Candidates(origin, destination, previous, failed); member = std::binary_search(set.begin(), set.end(), *at); }
	if (!member) return {PassengerPrecomputedPlan::State::NoRoute};
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
		const auto &o = this->graph->occurrences[id]; const auto &p = this->graph->patterns[o.pattern]; auto label = this->EvaluateRide(*row, id, destination);
		result.push_back({id, p.members.front().id, o.order, label.alight == INVALID ? StationID::Invalid() : this->graph->occurrences[label.alight].station, label.cost, p.wait, PassengerStrategy::Frequency(p.wait)});
	}
	return result;
}

PassengerPrecomputedTable::RouteInspection PassengerPrecomputedTable::InspectRoutes(
		StationID origin, StationID destination, OrderListID previous, OrderListID failed, size_t &work) const
{
	using State = PassengerPrecomputedPlan::State;
	if (this->graph_build) return {};
	if (this->failed) return {State::Memory};
	if (origin.base() >= this->graph->station_index.size() || destination.base() >= this->graph->station_index.size()) return {State::Unsupported};
	const uint32_t from = this->graph->station_index[origin.base()], to = this->graph->station_index[destination.base()];
	if (from == INVALID || to == INVALID || from == to) return {State::Unsupported};
	if (to >= this->rows.size() || this->rows[to].Empty()) return {};
	if (work == 0) return {};
	--work;
	const auto &row = this->rows[to];
	const uint32_t state = this->PolicyState(origin, previous);
	auto evaluate = [&](uint32_t board) -> Label {
		/* EvaluateRide visits at most one circuit. Reserve that entire bound
		 * here so the simulation's recurrence and work accounting stay untouched. */
		const size_t count = this->graph->patterns[this->graph->occurrences[board].pattern].occurrences.size();
		if (count >= work) { work = 0; return {}; }
		work -= count;
		return this->EvaluateRide(row, board, destination);
	};
	RouteInspection result{State::Ready};
	if (!this->graph->hyperpath) {
		auto plan = this->Select(this->RowLabel(row, from, state), StateTicks{}, previous, 0);
		result.state = plan.state;
		if (plan.ride) result.candidates.push_back({*plan.ride, 1.0});
		return result;
	}

	std::vector<uint32_t> candidates;
	if (failed == OrderListID::Invalid()) {
		/* Expand into caller-owned storage, without RowCandidates' mutable cache.
		 * Bound both the base payload and the incoming-service additions before copying. */
		const uint32_t index = row.indices[state];
		size_t count = this->graph->flat_storage ? this->FlatPayload(row, index).size() : row.policies[index].candidates.size();
		if (this->graph->flat_storage && !(row.flat_policies[index].count & FlatPolicy::FULL)) count += this->FlatPayload(row, row.indices[from]).size();
		if (count >= work) { work = 0; return {}; }
		work -= count;
		if (this->graph->flat_storage) this->ExpandPolicy(row, state, candidates);
		else candidates = row.policies[index].candidates;
	} else {
		/* Same local preference as LocalPolicy, computed only in display-owned memory.
		 * A failed service can expose alternatives absent from the stored base policy. */
		std::vector<std::pair<int64_t, uint32_t>> choices;
		for (uint32_t board : this->graph->station_boarding[from]) {
			if (work == 0) return {};
			--work;
			const auto &o = this->graph->occurrences[board];
			if (state != from && this->StateFor(from, o.pattern) == state) continue;
			auto label = evaluate(board);
			if (!this->graph->revisits.empty() && this->graph->revisits[board].next != INVALID && this->DominatedRevisit(board,
					label.cost, evaluate(this->graph->revisits[board].next).cost) &&
					(!this->graph->continuation_witness || this->HasSameRunContinuationWitness(board, label.alight))) continue;
			if (label.cost != INT64_MAX) choices.emplace_back(label.cost, board);
		}
		if (work == 0) return {};
		std::sort(choices.begin(), choices.end());
		auto compute = [&](bool exclude) {
			PassengerStrategy sum; candidates.clear();
			for (auto [cost, board] : choices) {
				const auto &p = this->graph->patterns[this->graph->occurrences[board].pattern];
				if (exclude && p.members.front().id == failed) continue;
				if (sum.Add(cost, PassengerStrategy::Frequency(p.wait))) candidates.push_back(board);
			}
		};
		compute(true);
		if (candidates.empty()) compute(false);
	}

	double frequency = 0;
	for (uint32_t board : candidates) frequency += PassengerStrategy::Frequency(this->graph->patterns[this->graph->occurrences[board].pattern].wait);
	/* Retain the largest branches, without redistributing omitted probability. */
	std::sort(candidates.begin(), candidates.end(), [&](uint32_t a, uint32_t b) {
		const auto wa = this->graph->patterns[this->graph->occurrences[a].pattern].wait, wb = this->graph->patterns[this->graph->occurrences[b].pattern].wait;
		return wa != wb ? wa < wb : a < b;
	});
	if (candidates.size() > 64) candidates.resize(64);
	for (uint32_t board : candidates) {
		auto label = evaluate(board);
		if (work == 0) break;
		auto plan = this->Select(label, StateTicks{}, previous, 0);
		if (plan.ride) result.candidates.push_back({*plan.ride, PassengerStrategy::Frequency(this->graph->patterns[this->graph->occurrences[board].pattern].wait) / frequency});
	}
	return result;
}

PassengerPrecomputedPlan PassengerPrecomputedTable::Select(const Label &label, StateTicks ready, OrderListID excluded, uint32_t sample) const
{
	PassengerPrecomputedPlan result;
	if (label.board == INVALID || label.alight == INVALID || label.cost == INT64_MAX) { result.state = this->unsupported ? PassengerPrecomputedPlan::State::Unsupported : PassengerPrecomputedPlan::State::NoRoute; return result; }
	const auto &board = this->graph->occurrences[label.board], &alight = this->graph->occurrences[label.alight]; const auto &pattern = this->graph->patterns[board.pattern];
	size_t member = sample % pattern.members.size();
	if (pattern.members[member].id == excluded) member = (member + 1) % pattern.members.size();
	if (pattern.members[member].id == excluded) { result.state = PassengerPrecomputedPlan::State::NoRoute; return result; }
	const auto &m = *this->FindMember(pattern.members[member].id);
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
		if (origin.base() >= this->graph->station_index.size() || destination.base() >= this->graph->station_index.size()) return {PassengerPrecomputedPlan::State::Unsupported};
		uint32_t from = this->graph->station_index[origin.base()], to = this->graph->station_index[destination.base()];
		if (from == INVALID || to == INVALID || from == to) return {this->unsupported ? PassengerPrecomputedPlan::State::Unsupported : PassengerPrecomputedPlan::State::NoRoute};
		if (to >= this->rows.size() || this->rows[to].Empty()) return {PassengerPrecomputedPlan::State::Building};
		uint32_t previous = excluded.base() < this->graph->service_pattern.size() ? this->graph->service_pattern[excluded.base()] : INVALID;
		uint32_t state = previous == INVALID ? from : this->StateFor(from, previous);
		const auto &row = this->rows[to]; Label label = this->RowLabel(row, from, state);
		if (this->graph->hyperpath) {
			auto [policy_cost, policy_wait] = this->PolicyValues(row, state);
			if (failed != OrderListID::Invalid()) {
				const auto &policy = this->LocalPolicy(row, destination, state, failed); policy_cost = policy.cost; policy_wait = policy.wait;
				if (!policy.candidates.empty()) { label = this->EvaluateRide(row, policy.candidates.front(), destination); label.cost = policy_cost; label.first_wait = policy_wait; }
			}
			auto result = this->Select(label, ready, excluded, sample); result.hyperpath = true;
			if (result.ride) {
				PassengerJourneyExecution intent;
				intent.kind = PassengerRideKind::HyperpathIntent; intent.boarding = origin; intent.alighting = destination;
				intent.not_before = intent.planned_at = ready;
				/* Database replaces the standalone generation with the saved table
				 * generation. Signature encodes cargo+1, never a service incarnation. */
				intent.signature = uint64_t(this->cargo) + 1; intent.generation = 1;
				intent.estimated_wait = policy_wait;
				/* Effective headway is twice the frequency-model residual wait.
				 * k=3 gives wait+3*headway; this is an abnormal-service safety net. */
				intent.waiting_limit = std::clamp<int64_t>(policy_wait > (int64_t{1} << 22) / 7 ? int64_t{1} << 22 : 7 * policy_wait, 256, int64_t{1} << 22);
				result.ride = intent;
			}
			return result;
		}
		const auto &alternative = this->AlternativeLabel(row, from);
		if (state == from && (!this->graph->estimates || excluded == OrderListID::Invalid()) && label.board != INVALID && alternative.board != INVALID) {
			const auto &best = this->graph->patterns[this->graph->occurrences[label.board].pattern], &alt = this->graph->patterns[this->graph->occurrences[alternative.board].pattern];
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

/* Pure frozen-graph search engine shared by serial execution and workers. */

void PassengerPrecomputedTable::GraphSnapshot::StartDestination(DestinationWorkspace &work, size_t destination) const
{
	work.destination = destination;
	if (!work.queues_configured) {
		work.queue.SetOptimized(this->optimized);
		work.queue.SetCapacityLimit(uint64_t(this->integrated ? this->logical_edges : this->edge_count) + this->states);
		if (this->hyperpath) {
			work.strategy.events.SetOptimized(true);
			work.strategy.events.SetCapacityLimit(uint64_t(this->logical_edges) + this->states);
		}
		work.queues_configured = true;
	}
	if (this->hyperpath) {
		work.strategy.events.Reset(); work.strategy.last = 0; work.strategy.board_edge = INVALID;
		work.strategy.witness_edge = INVALID;
		work.strategy.sums.clear(); work.strategy.settled.clear();
		if (this->flat_storage) {
			/* After StartDestination the grown storage is in the row, not the buffer. A workspace
			 * that travels between the main thread and a worker restarts in that state; keep the
			 * larger allocation instead of growing the next row from empty on the main thread. */
			if (work.prepared.flat_policies.capacity() > work.flat_policy_buffer.capacity()) work.flat_policy_buffer.swap(work.prepared.flat_policies);
			if (work.prepared.flat_candidates.capacity() > work.flat_candidate_buffer.capacity()) work.flat_candidate_buffer.swap(work.prepared.flat_candidates);
			work.flat_policy_buffer.clear(); work.flat_candidate_buffer.clear();
		}
		work.prepared = {};
		work.prepared_candidate_bytes = 0;
		if (this->flat_storage) {
			work.prepared.indices.reserve(this->waiting_state_count);
			/* Grow in the working buffers only once per build, not once per row. */
			work.prepared.flat_policies.swap(work.flat_policy_buffer); work.prepared.flat_candidates.swap(work.flat_candidate_buffer);
			work.prepared_candidate_bytes = work.prepared.flat_candidates.capacity() * 4;
		}
	}
	work.queue.Reset();
	work.scratch.labels.clear();
	if (this->optimized) {
		work.search.cost.clear(); work.search.legs.clear();
		work.search.cost.reserve(this->states); work.search.legs.reserve(this->states);
		/* Infinite priorities make old payloads unobservable. Only successful
		 * relaxations/zero seeds write metadata; every row resets all hot keys. */
		work.search.metadata.resize(this->states);
	} else work.scratch.labels.reserve(this->incoming_offsets.size() - 1);
	if (this->UseCompressedRows()) work.exception_state.assign(this->stations.size(), INVALID);
	work.scratch.alternative.assign(this->stations.size(), Label{});
	work.publication_cursor = 0;
	work.prepared.labels.clear(); work.prepared.labels.reserve(this->UseCompressedRows() ? this->stations.size() : this->waiting_state_count);
	work.expanding = INVALID; work.finishing_row = false; work.finishing = 0;
	work.initializing_row = true; work.seeding = 0;
}

size_t PassengerPrecomputedTable::GraphSnapshot::SearchDestination(DestinationWorkspace &work, size_t allowance) const
{
	size_t used = 0;
	while (used < allowance && !work.finishing_row) {
		++used;
		if (work.initializing_row) {
			if (this->optimized) {
				if (work.search.cost.size() < this->states) {
					/* Resize only the budgeted prefix. Retained capacity is reused;
					 * no uncharged full-array reset occurs at a row boundary. */
					size_t count = this->integrated ? std::min(this->states - work.search.cost.size(), allowance - used + 1) : 1;
					work.search.cost.resize(work.search.cost.size() + count, INT64_MAX);
					work.search.legs.resize(work.search.legs.size() + count, 0);
					used += count - 1; continue;
				}
			} else if (work.scratch.labels.size() < this->states) { work.scratch.labels.emplace_back(); continue; }
			auto seed = [&](uint32_t state, uint32_t alight = INVALID) {
				auto label = this->SearchLabel(work, state); label.cost = 0; label.alight = alight; this->SetSearchLabel(work, state, label);
				work.queue.emplace(0, 0, state);
			};
			if (work.seeding == 0) {
				seed(static_cast<uint32_t>(work.destination));
				for (const auto &[pattern, state] : this->waiting_states[work.destination]) seed(state);
			}
			size_t seed_count = this->optimized ? this->seed_offsets[work.destination + 1] - this->seed_offsets[work.destination] : this->occurrences.size();
			if (work.seeding < seed_count) {
				uint32_t id = this->optimized ? this->seed_occurrences[this->seed_offsets[work.destination] + work.seeding++] : static_cast<uint32_t>(work.seeding++);
				const auto &o = this->occurrences[id];
				if (o.alight && o.station == this->stations[work.destination]) {
					seed(o.arrival, id);
					if (!this->landings.empty() && this->landings[id] != INVALID) seed(this->landings[id], id);
				}
			} else work.initializing_row = false;
			continue;
		}
		if (work.expanding == INVALID) {
			if (work.queue.empty()) { work.finishing_row = true; continue; }
			auto [cost, legs, state] = work.queue.top(); work.queue.pop();
			if (cost != this->SearchCost(work, state) || legs != this->SearchLegs(work, state)) continue;
			work.expanding = state; work.cursor = this->incoming_offsets[state]; work.range_offset = 0;
		} else if (work.cursor == this->incoming_offsets[work.expanding + 1]) {
			work.expanding = INVALID;
		} else if (this->integrated) {
			used += this->RelaxIncoming(work, allowance - used + 1) - 1;
		} else {
			uint32_t index = static_cast<uint32_t>(work.cursor++);
			const auto &edge = this->edges[this->optimized ? index : this->incoming[index]];
			int64_t cost = this->SearchCost(work, edge.to);
			if (cost > INT64_MAX - edge.cost) continue;
			cost += edge.cost;
			uint32_t legs = this->SearchLegs(work, edge.to) + (edge.kind == EdgeKind::Board || edge.kind == EdgeKind::Section);
			if (std::tuple(cost, legs) >= std::tuple(this->SearchCost(work, edge.from), this->SearchLegs(work, edge.from))) continue;
			Label next = this->SearchLabel(work, edge.to);
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
			this->SetSearchLabel(work, edge.from, next); work.queue.emplace(next.cost, next.legs, edge.from);
		}
	}
	return used;
}

size_t PassengerPrecomputedTable::GraphSnapshot::SearchHyperpath(DestinationWorkspace &work, size_t allowance) const
{
	size_t used = 0;
	while (used < allowance && !work.finishing_row) {
		++used;
		if (work.initializing_row) {
			if (work.search.cost.size() < this->states) {
				work.search.cost.push_back(INT64_MAX); work.search.legs.push_back(0);
				work.strategy.settled.push_back(false);
				if (work.strategy.sums.size() < this->waiting_state_count) {
					work.strategy.sums.emplace_back();
					if (work.strategy.candidates.size() < work.strategy.sums.size()) work.strategy.candidates.emplace_back();
					else work.strategy.candidates[work.strategy.sums.size() - 1].clear();
				}
				continue;
			}
			const auto &waits = this->waiting_states[work.destination];
			const size_t seeds = this->seed_offsets[work.destination + 1] - this->seed_offsets[work.destination];
			uint32_t state, alight = INVALID;
			if (work.seeding == 0) state = static_cast<uint32_t>(work.destination);
			else if (work.seeding <= waits.size()) state = waits[work.seeding - 1].second;
			else if (work.seeding <= waits.size() + seeds) {
				alight = this->seed_occurrences[this->seed_offsets[work.destination] + work.seeding - waits.size() - 1];
				state = this->occurrences[alight].arrival;
			} else { work.initializing_row = false; continue; }
			++work.seeding;
			Label label; label.cost = 0; label.alight = alight;
			this->SetSearchLabel(work, state, label); this->PushHyperpath(work, 0, 1, state);
			continue;
		}
		if (work.strategy.witness_edge != INVALID) {
			/* One prefix stop is one Work unit. Costs alone cannot discard a
			 * tied, earlier alighting. Certify that every optimal first alighting
			 * lies beyond b's departure, using the same frozen suffix labels as
			 * EvaluateRide. Unsettled suffixes cannot tie: travel is positive and
			 * their priorities are at least the already popped Board key. */
			uint32_t edge = work.strategy.witness_edge, board = this->cold_edges[edge].occurrence;
			const auto &first = this->occurrences[board]; const auto &stops = this->patterns[first.pattern].occurrences;
			const auto &from = this->occurrences[work.strategy.witness_at];
			int64_t step = from.travel + from.uncertainty;
			if (work.strategy.witness_at != board) step += from.dwell;
			work.strategy.witness_cost += step; // Bounded by the precomputed, checked departure-to-departure delta.
			work.strategy.witness_at = work.strategy.witness_at == stops.back() ? stops.front() : work.strategy.witness_at + 1;
			const auto &to = this->occurrences[work.strategy.witness_at];
			bool earlier = false;
			if (to.alight) {
				int64_t suffix = to.station == this->stations[work.destination] ? 0 : this->SearchCost(work, this->StateFor(this->station_index[to.station.base()], first.pattern));
				int64_t alight = to.station == this->stations[work.destination] ? 0 : this->transfer_cost;
				if (suffix <= INT64_MAX - alight && suffix + alight <= INT64_MAX - work.strategy.witness_cost)
					earlier = suffix + alight + work.strategy.witness_cost <= this->SearchCost(work, first.departure);
			}
			if (earlier || work.strategy.witness_at == this->revisits[board].next) {
				if (earlier || !this->HasSameRunContinuationWitness(board, this->SearchAlight(work, first.departure))) {
					work.strategy.board_edge = edge; work.strategy.offset = 0;
				}
				work.strategy.witness_edge = INVALID;
			}
			continue;
		}
		if (work.strategy.board_edge != INVALID) {
			uint32_t edge = work.strategy.board_edge, r = this->hot_edges.range[edge];
			auto range = r == INVALID ? RangeEdge{0, 0, INVALID} : this->range_edges[r];
			uint32_t from = work.strategy.offset == 0 ? this->hot_edges.from[edge] : range.begin + work.strategy.offset - 1;
			++work.strategy.offset;
			if (from != range.excluded && !work.strategy.settled[from]) {
				++work.counters.range_updates;
				uint32_t board = this->cold_edges[edge].occurrence;
				const auto &o = this->occurrences[board]; const auto &pattern = this->patterns[o.pattern];
				auto label = this->SearchLabel(work, o.departure);
				int64_t x = label.cost <= INT64_MAX - pattern.risk ? label.cost + pattern.risk : INT64_MAX;
				auto &sum = work.strategy.sums[from];
				if (sum.Add(x, PassengerStrategy::Frequency(pattern.wait))) {
					auto &set = work.strategy.candidates[from];
					size_t capacity = set.capacity();
					set.insert(std::lower_bound(set.begin(), set.end(), board), board);
					work.hyperpath_candidate_bytes += (set.capacity() - capacity) * 4;
					++work.counters.board_accepts;
					if (set.size() == 1) { label.board = board; ++label.legs; label.waiting += sum.Wait(); }
					else { label = this->SearchLabel(work, from); label.waiting += sum.Wait() - label.first_wait; }
					label.cost = sum.Cost(); label.first_wait = sum.Wait();
					this->SetSearchLabel(work, from, label); this->PushHyperpath(work, label.cost, 2, from);
				}
			}
			if (work.strategy.offset == 1 + range.end - range.begin) { work.strategy.board_edge = INVALID; work.strategy.offset = 0; }
			continue;
		}
		if (work.expanding != INVALID) {
			if (work.cursor == this->incoming_offsets[work.expanding + 1]) { work.expanding = INVALID; continue; }
			uint32_t edge = static_cast<uint32_t>(work.cursor++);
			const auto &cold = this->cold_edges[edge];
			if (cold.kind == EdgeKind::Board) {
				int64_t cost = this->SearchCost(work, work.expanding), risk = this->patterns[this->occurrences[cold.occurrence].pattern].risk;
				if (cost <= INT64_MAX - risk) { this->PushHyperpath(work, cost + risk, 0, edge); ++work.counters.board_events; }
				continue;
			}
			assert(cold.kind != EdgeKind::Section);
			uint32_t from = this->hot_edges.from[edge]; int64_t cost = this->hot_edges.cost[edge];
			if (work.strategy.settled[from] || this->SearchCost(work, work.expanding) > INT64_MAX - cost) continue;
			auto label = this->SearchLabel(work, work.expanding); label.cost += cost;
			if (label.cost >= this->SearchCost(work, from)) continue;
			label.distance = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(label.distance) + cold.distance));
			if (cold.kind == EdgeKind::Ride) label.riding += cost - cold.uncertainty;
			if (cold.kind == EdgeKind::Alight) { label.board = INVALID; label.alight = cold.occurrence; label.waiting += cost / 2; }
			this->SetSearchLabel(work, from, label); this->PushHyperpath(work, label.cost, 1, from); continue;
		}
		if (work.strategy.events.empty()) { work.finishing_row = true; work.strategy.offset = 0; continue; }
		auto [key, kind, id] = work.strategy.events.top(); work.strategy.events.pop();
		++work.counters.heap_pops; work.strategy.last = key;
		if (kind == 0) {
			uint32_t board = this->cold_edges[id].occurrence;
			if (!this->revisits.empty() && this->revisits[board].next != INVALID && this->DominatedRevisit(board,
					this->SearchCost(work, this->occurrences[board].departure), this->SearchCost(work, this->occurrences[this->revisits[board].next].departure))) {
				if (!this->continuation_witness) continue; // Reference event sequence without continuation witnesses.
				if (work.strategy.settled[this->occurrences[board].departure] && work.strategy.settled[this->occurrences[this->revisits[board].next].departure]) {
					work.strategy.witness_edge = id; work.strategy.witness_at = board; work.strategy.witness_cost = 0; continue;
				}
			}
			work.strategy.board_edge = id; work.strategy.offset = 0; continue;
		}
		if (work.strategy.settled[id] || key != this->SearchCost(work, id)) continue;
		work.strategy.settled[id] = true;
		work.expanding = id; work.cursor = this->incoming_offsets[id];
	}
	return used;
}

bool PassengerPrecomputedTable::GraphSnapshot::UseCompressedRows() const
{
	/* Hyperpath forces integrated, but its strategies differ per waiting state and
	 * EvaluateRide reads suffix labels by state: it keeps the per-state layout. */
	return this->integrated && !this->hyperpath;
}

PassengerPrecomputedTable::Label PassengerPrecomputedTable::GraphSnapshot::SearchLabel(const DestinationWorkspace &work, uint32_t state) const
{
	if (!this->optimized) return work.scratch.labels[state];
	if (work.search.cost[state] == INT64_MAX) return {};
	const auto &m = work.search.metadata[state];
	return {work.search.cost[state], m.riding, m.waiting, m.first_wait, m.board, m.alight, this->hyperpath ? m.legs : work.search.legs[state], m.distance};
}

void PassengerPrecomputedTable::GraphSnapshot::SetSearchLabel(DestinationWorkspace &work, uint32_t state, const Label &label) const
{
	if (!this->optimized) { work.scratch.labels[state] = label; return; }
	work.search.cost[state] = label.cost;
	if (this->hyperpath) { work.search.metadata[state] = {label.riding, label.waiting, label.first_wait, label.board, label.alight, label.distance, label.legs}; return; }
	work.search.legs[state] = label.legs;
	work.search.metadata[state] = {label.riding, label.waiting, label.first_wait, label.board, label.alight, label.distance};
}

#ifdef WITH_PASSENGER_TABLE_INVARIANT_CHECKS
bool PassengerPrecomputedTable::GraphSnapshot::WaitingStationInvariant(const DestinationWorkspace &work, uint32_t station) const
{
	const auto base = this->SearchLabel(work, station);
	for (const auto &[pattern, state] : this->waiting_states[station]) {
		if (state != work.exception_state[station] && this->SearchLabel(work, state) != base) return false;
	}
	return true;
}
#endif

size_t PassengerPrecomputedTable::GraphSnapshot::RelaxIncoming(DestinationWorkspace &work, size_t allowance) const
{
	size_t total = 0;
	/* Every edge is nonnegative in (cost, legs), so relaxing an incoming
	 * edge cannot improve the expanded state itself. Keep its priorities
	 * outside the descriptor loop, including at a saved slice boundary. */
	const int64_t source_cost = work.search.cost[work.expanding];
	const uint32_t source_legs = work.search.legs[work.expanding];
	do {
		const uint32_t index = static_cast<uint32_t>(work.cursor);
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
		auto accept = [&, index, cost, legs, edge_cost](uint32_t from) {
			if (!materialized) {
				const auto &edge = this->cold_edges[index];
				next = this->SearchLabel(work, work.expanding); next.cost = cost; next.legs = legs;
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
			this->SetSearchLabel(work, from, next); work.queue.emplace(cost, legs, from);
		};
		const uint32_t base = this->hot_edges.from[index];
		/* A resumed descriptor uses the original scalar scan. Its cursor owns
		 * the expansion until completion, so no other update can interleave. */
		bool compare_all = true;
		if (work.range_offset == 0) {
			const bool improves_base = finite && (cost < work.search.cost[base] || (cost == work.search.cost[base] && legs < work.search.legs[base]));
			if (improves_base) accept(base);
			if (range_id != INVALID) {
				compare_all = improves_base;
				if (improves_base) work.exception_state[base] = range.excluded >= range.begin && range.excluded < range.end ? range.excluded : INVALID;
			}
			work.range_offset = 1; used = 1;
		}
		const uint32_t first = range.begin + work.range_offset - 1;
		const bool excluded = range.excluded >= first && range.excluded < range.end;
		const size_t waiting = std::min<size_t>(range.end - first - excluded, allowance - used);
		/* Include the free hole even when the budget ends immediately before it. */
		const uint32_t last = first + static_cast<uint32_t>(waiting) + (excluded && range.excluded - first <= waiting);
		const uint32_t middle = excluded ? std::min(range.excluded, last) : last;
		/* Bounded waiting spans: only an improving state enters the cold path. */
		if (finite && compare_all) {
			for (uint32_t from = first; from < middle; ++from) {
				if (cost < work.search.cost[from] || (cost == work.search.cost[from] && legs < work.search.legs[from])) accept(from);
			}
			if (excluded && range.excluded < last) {
				for (uint32_t from = range.excluded + 1; from < last; ++from) {
					if (cost < work.search.cost[from] || (cost == work.search.cost[from] && legs < work.search.legs[from])) accept(from);
				}
			}
		}
		if (finite && !compare_all) {
			const uint32_t from = work.exception_state[base];
			if (from >= first && from < last && from != range.excluded) {
				if (cost < work.search.cost[from] || (cost == work.search.cost[from] && legs < work.search.legs[from])) accept(from);
			}
		}
		work.range_offset += last - first; used += waiting;
		if (work.range_offset == count) {
			if (range_id != INVALID) {
				const uint32_t exception = work.exception_state[base];
				assert(exception == INVALID || (exception >= range.begin && exception < range.end));
#ifdef WITH_PASSENGER_TABLE_INVARIANT_CHECKS
				assert(this->WaitingStationInvariant(work, base));
#endif
			}
			++work.cursor; work.range_offset = 0;
		}
		assert(used > 0);
		total += used; allowance -= used;
	} while (allowance != 0 && work.cursor < this->incoming_offsets[work.expanding + 1]);
	return total;
}

void PassengerPrecomputedTable::GraphSnapshot::PushHyperpath(DestinationWorkspace &work, int64_t key, uint32_t kind, uint32_t id) const
{
	assert(key >= work.strategy.last);
	if (key == INT64_MAX) return;
	work.strategy.events.emplace(key, kind, id); ++work.counters.heap_pushes;
}

uint32_t PassengerPrecomputedTable::GraphSnapshot::StateFor(uint32_t station, uint32_t pattern) const
{
	const auto &states = this->waiting_states[station];
	auto it = std::lower_bound(states.begin(), states.end(), pattern, [](const auto &a, uint32_t b) { return a.first < b; });
	return it != states.end() && it->first == pattern ? it->second : station;
}

bool PassengerPrecomputedTable::GraphSnapshot::DominatedRevisit(uint32_t board, int64_t cost, int64_t next_cost) const
{
	const auto &revisit = this->revisits[board];
	return cost != INT64_MAX && next_cost != INT64_MAX && next_cost <= INT64_MAX - revisit.cost &&
			(this->continuation_witness ? cost == next_cost + revisit.cost : cost >= next_cost + revisit.cost);
}

bool PassengerPrecomputedTable::GraphSnapshot::HasSameRunContinuationWitness(uint32_t board, uint32_t alight) const
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
