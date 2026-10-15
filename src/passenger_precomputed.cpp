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
			service.timetable.Circuit() <= 0 || p.estimated != (service.timetable.period == 0) || p.occurrences.size() != service.timetable.stops.size()) return false;
	uint64_t mask = uint64_t{1} << this->cargo;
	for (size_t i = 0; i < p.occurrences.size(); ++i) {
		const auto &old = this->occurrences[p.occurrences[i]];
		const auto &stop = service.timetable.stops[i];
		if (old.station != stop.station || old.order != stop.order || old.board != ((stop.board & mask) != 0) ||
				old.alight != ((stop.alight & mask) != 0) || old.forced != ((stop.forced & mask) != 0) || old.continues != stop.continues) return false;
	}
	member->generation = service.generation;
	return true;
}

void PassengerPrecomputedTable::AddEdge(uint32_t from, uint32_t to, int64_t cost, EdgeKind kind, uint32_t occurrence, uint32_t distance, int64_t uncertainty)
{
	assert(cost >= 0);
	this->edges.push_back({from, to, occurrence, cost, distance, kind, uncertainty});
}

void PassengerPrecomputedTable::Begin(std::span<const PassengerPeriodicService *const> services, CargoType cargo, int64_t transfer, uint64_t memory, bool sections, bool cooperative, std::shared_ptr<uint64_t> memory_budget, bool estimates)
{
	*this = {}; this->cargo = cargo; this->estimates = estimates;
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
	memory = std::min(memory, MAX_BYTES);
	/* Canonical byte charges are identical on 32/64-bit peers. Native sizeof
	 * and allocator residency are diagnostics only, never routing admission. */
	static_assert(sizeof(Label) <= 64 && sizeof(Edge) <= 64 && sizeof(QueueItem) <= 32 && sizeof(Row) <= 64);
	uint64_t envelope = 1024 * 1024;
	if (envelope > memory) { this->failed = true; co_return; }
	for (const auto *s : services) {
		if (cooperative) co_await std::suspend_always{};
		if (!Product(1, 256, envelope, memory) || !Product(s->timetable.stops.size(), 256, envelope, memory) ||
				!Product(s->vehicles.size(), 64, envelope, memory) || !Product(s->dispatch_slots.size(), 32, envelope, memory)) {
			this->failed = true; this->diagnostic.envelope = memory; co_return;
		}
	}
	try {
		this->station_index.assign(StationID::Invalid().base(), INVALID);
		this->service_pattern.assign(OrderListID::Invalid().base(), INVALID);
		std::map<std::vector<int64_t>, std::vector<const PassengerPeriodicService *>> groups;
		std::set<StationID> station_ids;
		for (const auto *s : services) {
			if (cooperative) co_await std::suspend_always{};
			bool capacity = std::any_of(s->vehicles.begin(), s->vehicles.end(), [cargo](const auto &v) { return v.capacity[cargo] != 0; });
			if (!capacity) { this->unsupported |= s->timetable.period == 0; continue; }
			if ((this->estimates ? s->timetable.Circuit() : s->timetable.period) <= 0 || s->timetable.stops.size() < 2) { this->unsupported = true; continue; }
			groups[PatternKey(*s)].push_back(s);
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
				p.members.push_back({s->id, s->incarnation, s->generation});
				this->service_pattern[s->id.base()] = static_cast<uint32_t>(this->patterns.size());
				for (const auto &v : s->vehicles) if (v.capacity[cargo]) {
					if (cooperative) co_await std::suspend_always{};
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
		for (auto &o : this->occurrences) { o.arrival = static_cast<uint32_t>(nodes++); o.departure = static_cast<uint32_t>(nodes++); }
		size_t edge_count = 0;
		for (size_t p = 0; p < this->patterns.size(); ++p) for (const auto &stop : representatives[p]->timetable.stops) {
			if (cooperative) co_await std::suspend_always{};
			size_t contribution = 2 + ((stop.alight & mask) != 0);
			if (stop.board & mask) contribution += 1 + this->waiting_states[this->station_index[stop.station.base()]].size();
			if (contribution > INVALID - edge_count) { this->failed = true; co_return; }
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
				Product(edge_count, 264, envelope, memory) && Product(nodes, 144, envelope, memory);
		this->diagnostic.envelope = envelope;
		if (!fits) { this->failed = true; co_return; }
		this->edges.reserve(edge_count);
		this->reachable_destinations.resize(this->stations.size());
		for (uint32_t p = 0; p < this->patterns.size(); ++p) {
			if (cooperative) co_await std::suspend_always{};
			const auto &pattern = this->patterns[p]; const auto &t = representatives[p]->timetable;
			for (size_t i = 0; i < t.stops.size(); ++i) {
				if (cooperative) co_await std::suspend_always{};
				const auto &stop = t.stops[i]; uint32_t id = pattern.occurrences[i]; const auto &o = this->occurrences[id];
				if (stop.board & mask) {
					uint32_t station = this->station_index[stop.station.base()];
					this->AddEdge(station, o.departure, pattern.wait + pattern.risk, EdgeKind::Board, id);
					for (const auto &[previous, state] : this->waiting_states[station]) {
						if (cooperative) co_await std::suspend_always{};
						if (previous != p || pattern.members.size() > 1) this->AddEdge(state, o.departure, pattern.wait + pattern.risk, EdgeKind::Board, id);
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
				this->AddEdge(o.departure, this->occurrences[pattern.occurrences[next]].arrival, travel + uncertainty, EdgeKind::Ride, INVALID, distance, uncertainty);
			}
		}
		if (sections) {
			bool success = false;
			auto task = this->BuildSections(pattern_members, transfer, nodes, envelope, memory, cooperative, success);
			while (!task.Resume(1)) co_await std::suspend_always{};
			if (!success) { this->failed = true; this->diagnostic.envelope = memory; co_return; }
		}
		this->diagnostic.envelope = envelope;
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
					!Product(1, 64, envelope, memory) || !Product(edges, 264, envelope, memory)) co_return;
			if (this->landings[path.alight] == INVALID) {
				if (nodes >= INVALID || !Product(1, 144 + 264, envelope, memory)) co_return;
				this->landings[path.alight] = static_cast<uint32_t>(nodes++);
				this->AddEdge(this->landings[path.alight], this->StateFor(this->station_index[alight.station.base()], alight.pattern),
						2 * transfer, EdgeKind::Alight, path.alight);
			}
			uint32_t id = static_cast<uint32_t>(this->section_rides.size());
			int64_t cost = path.riding + path.uncertainty + wait + risk;
			this->section_rides.push_back({path.board, path.alight, path.riding, wait, cost});
			this->AddEdge(station, this->landings[path.alight], cost, EdgeKind::Section, id, path.distance);
			for (const auto &[previous, state] : this->waiting_states[station]) {
				if (cooperative) co_await std::suspend_always{};
				if (previous != board.pattern || this->patterns[board.pattern].members.size() > 1) {
					this->AddEdge(state, this->landings[path.alight], cost, EdgeKind::Section, id, path.distance);
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
	this->scratch.labels.clear(); this->scratch.labels.reserve(this->incoming_offsets.size() - 1);
	this->scratch.alternative.assign(this->stations.size(), Label{});
	this->prepared.labels.clear(); this->prepared.labels.reserve(this->waiting_state_count);
	this->expanding = INVALID; this->finishing_row = false; this->finishing = 0;
	this->initializing_row = true; this->seeding = 0;
}

size_t PassengerPrecomputedTable::Step(size_t allowance, bool legacy_publication, const std::function<void(StationID)> &published)
{
	if (!this->PrepareGraph(std::min<size_t>(allowance, 1024))) return 0;
	const auto started = Clock::now(); size_t used = 0;
	auto index_origin = [&](size_t from) {
		if (from >= this->stations.size() || this->scratch.labels[from].cost == INT64_MAX) return;
		auto &destinations = this->reachable_destinations[from]; size_t capacity = destinations.capacity();
		destinations.push_back(this->stations[this->destination]);
		this->reachable_bytes += (destinations.capacity() - capacity) * sizeof(StationID);
	};
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
					const auto &best = this->scratch.labels[station];
					if (best.board == INVALID || this->occurrences[best.board].pattern == board.pattern) continue;
					const auto &suffix = this->scratch.labels[this->landings[ride.alight]];
					if (suffix.cost > INT64_MAX - ride.cost) continue;
					int64_t cost = suffix.cost + ride.cost;
					if (cost == INT64_MAX || cost - best.cost > best.cost / 4) continue;
					if (this->estimates && (suffix.riding + ride.riding > best.riding + std::max<int64_t>(32, best.riding / 4) ||
							suffix.legs + 1 > best.legs + 1)) continue;
					auto &alt = this->scratch.alternative[station];
					if (cost < alt.cost) {
						alt = suffix; alt.board = ride.board; alt.alight = ride.alight; ++alt.legs;
						alt.cost = cost; alt.waiting += ride.waiting; alt.riding += ride.riding; alt.first_wait = ride.waiting;
					}
				} else if (this->finishing < this->occurrences.size()) {
					uint32_t id = static_cast<uint32_t>(this->finishing++); const auto &o = this->occurrences[id];
					uint32_t station = this->station_index[o.station.base()]; if (station == INVALID) continue;
					const auto &best = this->scratch.labels[station], &suffix = this->scratch.labels[o.departure];
					const auto &p = this->patterns[o.pattern];
					if (best.board == INVALID || suffix.alight == INVALID || suffix.cost == INT64_MAX || suffix.cost >= best.cost ||
							this->occurrences[best.board].pattern == o.pattern || suffix.cost > INT64_MAX - p.wait - p.risk) continue;
					int64_t cost = suffix.cost + p.wait + p.risk;
					if (cost - best.cost > best.cost / 4) continue;
					/* Dispersion may pay a modest cost premium, but a waiting-time
					 * estimate alone must not randomly select a much longer ride or
					 * several extra transfers. Faster winning routes stay unrestricted. */
					if (this->estimates && (suffix.riding > best.riding + std::max<int64_t>(32, best.riding / 4) ||
							suffix.legs + 1 > best.legs + 1)) continue;
					/* Only real Board edges are alternatives; NoLoad stops never enter. */
					if (!o.board) continue;
					auto &alt = this->scratch.alternative[station];
					if (cost < alt.cost) {
						alt = suffix; alt.board = id; ++alt.legs;
						alt.cost = cost; alt.waiting += p.wait; alt.first_wait = p.wait;
					}
				} else {
					/* Arrival/departure labels are construction scratch. Queries use
					 * only initial/transfer waits and one complete dispersion label. */
					if (legacy_publication) {
						this->prepared.labels.assign(this->scratch.labels.begin(), this->scratch.labels.begin() + this->waiting_state_count);
						for (size_t from = 0; from < this->stations.size(); ++from) index_origin(from);
					} else if (this->prepared.labels.size() < this->waiting_state_count) {
						/* Charge inverse-index construction to the existing label-copy
						 * slice, rather than visiting all stations at publication. */
						index_origin(this->prepared.labels.size());
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
				if (edge.kind == EdgeKind::Board) {
					next.board = edge.occurrence; ++next.legs;
					next.first_wait = this->patterns[this->occurrences[edge.occurrence].pattern].wait; next.waiting += next.first_wait;
				}
				if (edge.kind == EdgeKind::Section) {
					const auto &ride = this->section_rides[edge.occurrence];
					next.board = ride.board; next.alight = ride.alight; ++next.legs;
					next.first_wait = ride.waiting; next.waiting += ride.waiting; next.riding += ride.riding;
				}
				if (edge.kind == EdgeKind::Ride) next.riding += edge.cost - edge.uncertainty;
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
			(this->scratch.labels.capacity() + this->scratch.alternative.capacity() + this->prepared.labels.capacity()) * sizeof(Label) +
			this->section_rides.capacity() * sizeof(SectionRide) + this->landings.capacity() * sizeof(uint32_t);
	this->diagnostic.bytes += this->binding_bytes + this->reachable_bytes;
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
	result.estimated_service = pattern.estimated;
	return result;
}

PassengerPrecomputedPlan PassengerPrecomputedTable::Plan(StationID origin, StationID destination, StateTicks ready, OrderListID excluded, uint32_t sample) const
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
