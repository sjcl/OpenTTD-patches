/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
/** @file passenger_timed_rounds.cpp Anytime service scans, without a future-visit graph. */
#include "stdafx.h"
#include "passenger_timed_rounds.h"
#include "passenger_query_cpu.h"
#include "station_base.h"
#include "vehicle_base.h"
#include <chrono>
#include <deque>
#include "safeguards.h"

namespace {
	bool Add(int64_t &value, int64_t delta)
	{
		if (delta < 0 || value > INT64_MAX - delta) return false;
		value += delta; return true;
	}
	bool Difference(int64_t end, int64_t begin, int64_t &delta)
	{
		if (end < begin || uint64_t(end) - uint64_t(begin) > INT64_MAX) return false;
		delta = end - begin; return true;
	}
	struct RoundLabel {
		StationID station;
		StateTicks time;
		int64_t cost;
		size_t parent, rides;
		PassengerJourneyExecution leg, first;
		bool active = true;
	};
	struct Onboard {
		size_t parent, board, expires;
		StateTicks departure, not_before;
		int64_t time, cost; ///< Arrival at the current unrolled occurrence, including weighted boarding wait.
		PassengerJourneyExecution first;
	};
	struct RoundService {
		const PassengerPeriodicService *service;
		size_t start;
		int64_t bound;
		bool direct;
	};
	/** Admission follows requested capacity and fixed envelopes, independently
	 * of pointer width, standard-library reserve policy, or physical reuse. */
	template <typename T, size_t BYTES> struct RoundBuffer : std::vector<T> {
		static_assert(sizeof(T) <= BYTES);
		static constexpr size_t UNIT_BYTES = BYTES;
		size_t logical_capacity = 0;
	};
	auto FirstKey(const PassengerJourneyExecution &ride)
	{
		return std::tuple(ride.departure, ride.service, ride.boarding_order, ride.alighting_order);
	}
	size_t LogWork(size_t size)
	{
		return size == 0 ? 0 : size * std::bit_width(size);
	}
}

PassengerTimedRoundResult PassengerServiceDatabase::PlanRounds(CargoType cargo, StationID origin, StationID destination,
		StateTicks ready, PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, OrderListID excluded)
{
	return PassengerTimedRounds::Plan(*this, cargo, origin, destination, ready, limits, evaluation, excluded);
}

PassengerTimedRoundResult PassengerTimedRounds::Plan(PassengerServiceDatabase &db, CargoType cargo, StationID origin,
		StationID destination, StateTicks ready, PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, OrderListID excluded)
{
	using Reason = PassengerPeriodicPlan::Reason;
	using Outcome = PassengerTimedRoundOutcome;
	PassengerTimedRoundResult answer;
	auto &result = answer.plan;
	auto &stats = answer.diagnostic;
	PassengerQueryCpuTimer cpu;
	const auto started = std::chrono::steady_clock::now();
	std::optional<std::chrono::steady_clock::time_point> forward_started;
	bool truncated = false, stopped = false, uncertain = false;
	size_t round = 0, winner = SIZE_MAX;
	PassengerServiceDatabase::JourneyPolicy temporary;
	const PassengerServiceDatabase::JourneyPolicy *policy = nullptr;
	PassengerServiceDatabase::Dependencies dependencies;
	PassengerServiceDatabase::PolicyKey policy_key{2, cargo, origin, destination, OrderListID::Invalid(), limits, evaluation};
	bool store_policy = false;
	auto observe = [&](Reason reason) { result.observations[static_cast<size_t>(reason)] = true; };
	auto limit = [&](Reason reason) {
		observe(reason); truncated = true;
		if (!result.deferred || reason == Reason::Work || reason == Reason::Memory) result.reason = reason;
		result.deferred = true;
	};
	auto spend = [&](size_t count = 1) {
		if (stopped) return false;
		if (count > limits.work - result.work) { limit(Reason::Work); stopped = true; return false; }
		result.work += count;
		if (round != 0) stats.round_work[std::min<size_t>(round - 1, 16)] += count;
		return true;
	};
	auto memory = [&](size_t bytes) {
		if (bytes > limits.memory_bytes - result.memory) { limit(Reason::Memory); stopped = true; return false; }
		result.memory += bytes; return true;
	};
	auto finish = [&]() {
		const auto forward_us = forward_started ? std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - *forward_started).count() : 0;
		if (store_policy) {
			const size_t bytes = 256 + sizeof(std::pair<StationID, int64_t>) * temporary.lower_bounds.size();
			db.StorePolicy(policy_key, std::move(temporary), std::move(dependencies), bytes);
		}
		observe(result.reason);
		stats.queries = 1; stats.work = result.work; stats.labels = result.labels; stats.peak_memory = result.memory;
		stats.query_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
		if (auto elapsed = cpu.Elapsed()) { stats.query_cpu_nanoseconds = *elapsed; stats.query_cpu_samples = 1; }
		++stats.outcomes[static_cast<size_t>(answer.outcome)];
		auto &total = db.round_diagnostic;
		#define ACCUMULATE(field) total.field += stats.field
		ACCUMULATE(queries); ACCUMULATE(query_microseconds); ACCUMULATE(rounds); ACCUMULATE(stations);
		ACCUMULATE(query_cpu_nanoseconds); ACCUMULATE(query_cpu_samples);
		ACCUMULATE(service_candidates); ACCUMULATE(service_scans); ACCUMULATE(occurrences); ACCUMULATE(departures);
		ACCUMULATE(label_attempts); ACCUMULATE(label_comparisons); ACCUMULATE(onboard_comparisons);
		ACCUMULATE(labels); ACCUMULATE(work); ACCUMULATE(reconstruction);
		#undef ACCUMULATE
		total.peak_memory = std::max(total.peak_memory, stats.peak_memory);
		for (size_t i = 0; i < stats.outcomes.size(); ++i) total.outcomes[i] += stats.outcomes[i];
		for (size_t i = 0; i < stats.round_work.size(); ++i) total.round_work[i] += stats.round_work[i];
		++db.diagnostic.searches; db.diagnostic.search_work += result.work;
		db.diagnostic.query_cpu_nanoseconds += stats.query_cpu_nanoseconds; db.diagnostic.query_cpu_samples += stats.query_cpu_samples;
		db.diagnostic.query_microseconds += stats.query_microseconds;
		db.diagnostic.forward_microseconds += forward_us;
		db.diagnostic.timed_labels += result.labels;
		++db.diagnostic.outcomes[static_cast<size_t>(result.reason)];
		for (size_t i = 0; i < result.observations.size(); ++i) db.diagnostic.observations[i] += result.observations[i];
		return answer;
	};
	if (cargo >= NUM_CARGO || origin == destination || !Station::IsValidID(origin) || !Station::IsValidID(destination) ||
			evaluation.minimum_transfer < 0 || evaluation.transfer_penalty < 0) return finish();
	if (evaluation.crowding_penalty != 0 || evaluation.capacity_shortfall_penalty != 0) {
		result.reason = Reason::Evaluation; answer.outcome = Outcome::Indeterminate; return finish();
	}
	if (!spend() || !memory(4096)) { answer.outcome = Outcome::BudgetDeferred; return finish(); }
	const uint64_t mask = uint64_t{1} << cargo;
	/* Missing timed publications are missing evidence, including unsupported
	 * control flow discarded by the compiler. Do not prove physical absence. */
	if (db.StopsAt(origin).empty() || db.StopsAt(destination).empty()) uncertain = true;
	if (limits.work >= 4096 && limits.memory_bytes >= 32768) {
		if (auto *cached = db.FindPolicy(policy_key)) {
			policy = &std::get<PassengerServiceDatabase::JourneyPolicy>(cached->value); ++db.diagnostic.policy_hits;
		} else {
			auto compilation = limits; compilation.work /= 4; compilation.memory_bytes -= result.memory;
			const auto compile_started = std::chrono::steady_clock::now();
			temporary = db.CompilePolicy(cargo, origin, destination, compilation, evaluation, dependencies);
			db.diagnostic.policy_compile_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - compile_started).count();
			policy = &temporary; store_policy = true; ++db.diagnostic.policy_misses;
		}
		if (!spend(policy->work) || !memory(std::max(policy->bytes, policy->scratch_bytes))) { answer.outcome = Outcome::BudgetDeferred; return finish(); }
		/* A known-phase relaxed tree cannot certify absence or optimality across
		 * an unknown-phase gap, including a truncated phase-gap diagnostic. */
		uncertain |= policy->unknown_phase;
		if (!policy->limited && policy->reason != Reason::NoCandidate) {
			result.reason = policy->reason;
			answer.outcome = !uncertain && !db.IsBuilding() && (policy->reason == Reason::Unreachable || policy->reason == Reason::NoAlighting) ? Outcome::Unreachable : Outcome::Indeterminate;
			return finish();
		}
	}
	forward_started = std::chrono::steady_clock::now();
	auto bound = [&](StationID station, int64_t cost) {
		if (policy != nullptr && !Add(cost, policy->DistanceLowerBound(station))) return INT64_MAX;
		return cost;
	};
	/* The logical envelope covers deque blocks, continuation/round-count maps,
	 * both sparse station frontiers,
	 * geometric index vectors, parent paths and the returned route. It does not
	 * discount warm storage or optional departure-index residency. */
	constexpr size_t LABEL_BYTES = 1024, SERVICE_BYTES = 128;
	static_assert(sizeof(RoundLabel) < LABEL_BYTES / 2);
	std::deque<RoundLabel> labels;
	using Rows = std::map<StationID, std::vector<size_t>>;
	using FrontKey = std::tuple<StationID, OrderListID, VehicleOrderID>;
	std::map<FrontKey, std::vector<size_t>> fronts;
	std::map<std::pair<StationID, size_t>, size_t> round_counts;
	Rows previous, next;
	RoundBuffer<Onboard, 256> onboard;
	RoundBuffer<VehicleID, 4> loading;
	RoundBuffer<size_t, 8> ranked, dominated;
	RoundBuffer<OrderListID, 4> destination_services;
	RoundBuffer<RoundService, 64> service_order;
	std::map<OrderListID, RoundService> services;
	size_t service_capacity = 0;
	auto reserve = [&](auto &vector, size_t needed) {
		if (needed <= vector.logical_capacity) return true;
		size_t capacity = std::max(needed, vector.logical_capacity * 2);
		if (!memory((capacity - vector.logical_capacity) * std::decay_t<decltype(vector)>::UNIT_BYTES)) return false;
		vector.reserve(capacity); vector.logical_capacity = capacity; return true;
	};
	auto dominates = [&](const RoundLabel &a, const RoundLabel &b) {
		++stats.label_comparisons;
		/* A continuation can depend on its incoming service/physical occurrence.
		 * Weighted waiting does not permit merging merely earlier arrivals. The
		 * first-service rank is retained even when later costs could become equal. */
		return a.active && (evaluation.waiting_weight == 0 ? a.time <= b.time : a.time == b.time) &&
			a.cost <= b.cost && a.rides <= b.rides && a.leg.service == b.leg.service &&
			a.leg.alighting_order == b.leg.alighting_order && FirstKey(a.first) <= FirstKey(b.first);
	};
	for (const auto &ref : db.StopsAt(destination)) {
		if (!spend()) break;
		const auto *service = db.Get(ref.service);
		if (service == nullptr || !(service->timetable.stops[ref.stop].alight & mask)) continue;
		if (!destination_services.empty() && destination_services.back() == ref.service) continue;
		if (!reserve(destination_services, destination_services.size() + 1)) break;
		destination_services.push_back(ref.service);
	}
	auto insert = [&](RoundLabel candidate) {
		++stats.label_attempts;
		if (!spend()) return;
		if (winner != SIZE_MAX && bound(candidate.station, candidate.cost) > labels[winner].cost) return;
		/* These continuation identities cannot dominate each other. Index them
		 * separately, while preserving the shared Station x Round label ceiling. */
		FrontKey key{candidate.station, candidate.leg.service, candidate.leg.alighting_order};
		auto row = fronts.find(key);
		dominated.clear();
		auto count = round_counts.find({candidate.station, candidate.rides});
		size_t retained = count == round_counts.end() ? 0 : count->second;
		if (candidate.station != destination && row != fronts.end()) {
			for (size_t id : row->second) {
				if (!spend(2)) return;
				if (dominates(labels[id], candidate)) return;
				if (!labels[id].active) continue;
				if (dominates(candidate, labels[id])) {
					if (!reserve(dominated, dominated.size() + 1)) return;
					dominated.push_back(id);
					if (labels[id].rides == candidate.rides) --retained;
				}
			}
		}
		if (labels.size() >= limits.labels || (candidate.station != destination && retained >= limits.labels_per_station)) { limit(Reason::Labels); return; }
		/* One storage unit prepays each eventual parent reconstruction step, so
		 * an admitted destination is usable even when the next operation stops. */
		if (!spend(2) || !memory(LABEL_BYTES)) return;
		/* Comparison work above prepays these flag mutations. */
		for (size_t id : dominated) {
			labels[id].active = false;
			--round_counts.at({labels[id].station, labels[id].rides});
		}
		size_t id = labels.size(); labels.push_back(std::move(candidate));
		const auto &label = labels.back();
		if (label.station == destination) {
			++result.candidates;
			if (winner == SIZE_MAX || std::tuple(label.cost, label.time, label.rides, FirstKey(label.first)) <
					std::tuple(labels[winner].cost, labels[winner].time, labels[winner].rides, FirstKey(labels[winner].first))) winner = id;
		} else {
			fronts[key].push_back(id); next[label.station].push_back(id);
			++round_counts[{label.station, label.rides}];
		}
	};
	insert({origin, ready, 0, SIZE_MAX, 0, {}, {}});
	previous.swap(next);
	while (!previous.empty() && !stopped) {
		++round;
		if (!spend()) break;
		++stats.rounds;
		if (round - 1 > limits.max_transfers) { limit(Reason::Transfers); break; }
		/* A destination-guided beam keeps broad interchange rounds from spending
		 * all work before their useful services. This is an explicit omission,
		 * never a certificate: retain the incumbent and report Provisional.
		 * The safety transfer ceiling still permits arbitrarily deep sparse ODs. */
		static constexpr size_t ROUND_FRONTIER = 128;
		ranked.clear();
		for (const auto &[station, ids] : previous) for (size_t id : ids) {
			if (!spend()) break;
			if (!labels[id].active || (winner != SIZE_MAX && bound(station, labels[id].cost) > labels[winner].cost)) continue;
			if (!reserve(ranked, ranked.size() + 1)) break;
			ranked.push_back(id);
		}
		if (stopped) break;
		if (ranked.size() > ROUND_FRONTIER) {
			if (!spend(LogWork(ranked.size()))) break;
			std::sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b) {
				const auto &x = labels[a], &y = labels[b];
				return std::tuple(bound(x.station, x.cost), x.cost, x.time, FirstKey(x.first), x.station, a) <
					std::tuple(bound(y.station, y.cost), y.cost, y.time, FirstKey(y.first), y.station, b);
			});
			ranked.resize(ROUND_FRONTIER); limit(Reason::Labels);
		}
		if (!spend(LogWork(ranked.size()))) break;
		std::sort(ranked.begin(), ranked.end());
		for (auto &[station, ids] : previous) {
			/* Rank traversal prepays this linear filter; binary membership has the
			 * same fixed, <=8 comparison bound on every cache/peer. */
			if (!spend(8 * ids.size())) break;
			std::erase_if(ids, [&](size_t id) { return !std::binary_search(ranked.begin(), ranked.end(), id); });
		}
		if (stopped) break;
		services.clear();
		for (const auto &[station, ids] : previous) {
			if (!spend()) break;
			++stats.stations;
			bool active = false, same_incoming = true; int64_t best_bound = INT64_MAX;
			OrderListID incoming = OrderListID::Invalid();
			for (size_t id : ids) {
				if (!spend()) break;
				if (!labels[id].active || (winner != SIZE_MAX && labels[id].cost > labels[winner].cost)) continue;
				if (!active) incoming = labels[id].leg.service;
				else same_incoming &= incoming == labels[id].leg.service;
				active = true; best_bound = std::min(best_bound, bound(station, labels[id].cost));
			}
			if (!active || stopped) continue;
			for (const auto &ref : db.StopsAt(station)) {
				if (!spend()) break;
				++stats.service_candidates;
				if (round == 1 && ref.service == excluded) continue;
				if (same_incoming && incoming == ref.service) continue;
				const auto *service = db.Get(ref.service);
				if (service == nullptr || !(service->timetable.stops[ref.stop].board & mask)) continue;
				auto queued = services.find(ref.service);
				if (queued != services.end()) {
					queued->second.start = std::min(queued->second.start, ref.stop);
					queued->second.bound = std::min(queued->second.bound, best_bound); continue;
				}
				if (services.size() == service_capacity) { if (!memory(SERVICE_BYTES)) break; ++service_capacity; }
				/* Each query pins this published incarnation/generation; the DB cannot
				 * publish during a synchronous simulation-thread round. */
				if (!spend(std::bit_width(destination_services.size()))) break;
				services.emplace(ref.service, RoundService{service, ref.stop, best_bound,
					std::binary_search(destination_services.begin(), destination_services.end(), ref.service)});
			}
		}
		service_order.clear();
		if (!reserve(service_order, services.size()) || !spend(services.size() + LogWork(services.size()))) break;
		for (const auto &[id, entry] : services) service_order.push_back(entry);
		std::sort(service_order.begin(), service_order.end(), [](const auto &a, const auto &b) {
			return std::tuple(!a.direct, a.bound, a.service->id) < std::tuple(!b.direct, b.bound, b.service->id);
		});
		for (const auto &entry : service_order) {
			if (stopped || !spend()) break;
			if (winner != SIZE_MAX && entry.bound > labels[winner].cost) continue;
			const auto &service = *entry.service;
			const auto &pattern = service.timetable;
			const size_t count = pattern.stops.size(), start = entry.start;
			if (pattern.period <= 0 || count < 2) { uncertain = true; observe(Reason::UnknownPhase); continue; }
			++stats.service_scans;
			/* Charge cold fleet/index construction work even with a resident index.
			 * Physical acceleration must not alter admission on a joining client. */
			if (!spend(service.vehicles.size() + LogWork(service.vehicles.size()))) break;
			const auto *phases = db.DepartureIndex(service, cargo);
			onboard.clear();
			auto dispatch_departure = [&](int64_t offset, int64_t earliest) {
				std::optional<StateTicks> departure;
				int64_t floor = service.dispatch_floor.base();
				if (!Add(floor, offset)) { uncertain = true; observe(Reason::Evaluation); return departure; }
				for (uint32_t slot : service.dispatch_slots) {
					if (!spend()) break;
					++db.diagnostic.departure_calculations;
					auto time = PassengerPeriodicTimetable::NextDeparture(service.dispatch_base, int64_t(slot) + offset,
						service.dispatch_period, StateTicks{std::max(earliest, floor)});
					if (time && (!departure || *time < *departure)) departure = time;
				}
				return departure;
			};
			for (size_t occurrence = start; occurrence < start + 2 * count - 1 && !stopped; ++occurrence) {
				if (!spend()) break;
				++stats.occurrences;
				const size_t stop_index = occurrence % count;
				const auto &stop = pattern.stops[stop_index];
				if (occurrence != start) {
					const auto &prior = pattern.stops[(occurrence - 1) % count];
					int64_t elapsed = stop.arrival - prior.arrival;
					if (stop_index == 0) elapsed += pattern.period;
					size_t retained = 0;
					for (auto &ride : onboard) {
						if (!spend()) break;
						if (occurrence >= ride.expires) continue;
						if (!Add(ride.time, elapsed) || !Add(ride.cost, elapsed)) { uncertain = true; observe(Reason::Evaluation); continue; }
						/* Operating stops need not be ground-transfer vertices in the
						 * reverse policy. Ride cost alone bounds every later alighting. */
						if (winner != SIZE_MAX && ride.cost > labels[winner].cost) continue;
						onboard[retained++] = ride;
					}
					onboard.resize(retained);
				}
				if (stopped) break;
				if (stop.alight & mask) for (const auto &ride : onboard) {
					if (!spend()) break;
					const auto &parent = labels[ride.parent];
					const auto &board = pattern.stops[ride.board % count];
					PassengerJourneyExecution leg{VehicleID::Invalid(), service.id, board.station, stop.station,
						board.order, stop.order, ride.departure, StateTicks{ride.time}, service.incarnation};
					leg.kind = PassengerRideKind::ServiceIntent; leg.generation = service.generation; leg.not_before = ride.not_before;
					insert({stop.station, StateTicks{ride.time}, ride.cost, ride.parent, parent.rides + 1, leg, parent.rides == 0 ? leg : parent.first});
				}
				if (stop.forced & mask) onboard.clear();
				if (stopped) break;
				if (service.dispatch_period > 0 && stop_index == service.dispatch_stop) {
					/* Arrival can precede the next certified slot. Alighting above
					 * uses the actual arrival; through passengers retain the holding
					 * time as a virtual arrival before the next adjacent segment. */
					size_t retained = 0;
					for (auto &ride : onboard) {
						if (!spend()) break;
						if (occurrence + 1 >= ride.expires || occurrence + 1 >= start + 2 * count - 1) continue;
						++stats.departures;
						int64_t earliest = ride.time;
						if (!Add(earliest, stop.departure - stop.arrival)) { uncertain = true; observe(Reason::Evaluation); continue; }
						auto departure = dispatch_departure(0, earliest);
						if (stopped) break;
						int64_t extra;
						if (!departure || !Difference(departure->base(), earliest, extra) || !Add(ride.time, extra) || !Add(ride.cost, extra)) {
							uncertain = true; observe(Reason::Evaluation); continue;
						}
						onboard[retained++] = ride;
					}
					onboard.resize(retained);
				}
				if (stopped) break;
				/* Every physical boarding occurrence appears once; the second circuit
				 * only carries passengers through the wrap, at most N-1 segments. */
				if (occurrence >= start + count) { if (onboard.empty()) break; continue; }
				if (!(stop.board & mask)) continue;
				auto station = previous.find(stop.station);
				if (station == previous.end()) continue;
				for (size_t parent_id : station->second) {
					if (!spend()) break;
					const auto &parent = labels[parent_id];
					if (!parent.active || (winner != SIZE_MAX && bound(parent.station, parent.cost) > labels[winner].cost)) continue;
					/* PlanWaiting/CanBoard exclude the arriving incarnation on every
					 * real interchange, including other physical occurrences/fleet
					 * members. A through ride already stays on this service. */
					if (parent.rides != 0 && parent.leg.service == service.id) continue;
					int64_t earliest = parent.time.base();
					if (parent.rides != 0 && !Add(earliest, evaluation.minimum_transfer)) { uncertain = true; observe(Reason::Evaluation); continue; }
					const Station *live_station = Station::GetIfValid(stop.station);
					if (live_station == nullptr) { uncertain = true; continue; }
					/* Logical fleet and live-stop work is identical for index/scan paths.
					 * This also pays for bounded live lookups and exclusions. */
					if (!spend(service.vehicles.size() + live_station->loading_vehicles.size() + 1)) break;
					++stats.departures;
					std::optional<StateTicks> departure;
					auto consider = [&](std::optional<StateTicks> time) { if (time && (!departure || *time < *departure)) departure = time; };
					if (phases != nullptr) {
						++db.diagnostic.departure_index_hits;
						if (phases->Unknown()) { uncertain = true; observe(Reason::UnknownPhase); }
						loading.clear();
						for (const Vehicle *live : live_station->loading_vehicles) {
							if (Vehicle::GetIfValid(live->index) != live || !phases->Contains(live->index)) continue;
							auto base = db.LoadingPhase(live, service, stop);
							if (!base) continue;
							if (!reserve(loading, loading.size() + 1)) break;
							loading.push_back(live->index);
							++db.diagnostic.departure_calculations;
							consider(PassengerPeriodicTimetable::NextDeparture(*base, stop.departure, pattern.period, StateTicks{earliest}));
						}
						if (!spend(LogWork(loading.size()))) break;
						std::sort(loading.begin(), loading.end());
						consider(phases->Next(stop.departure, StateTicks{earliest}, loading, db.diagnostic.departure_calculations));
					} else {
						++db.diagnostic.departure_index_fallbacks;
						/* Same loading scratch/memory and sort charge on cache-disabled peers. */
						loading.clear();
						for (const auto &vehicle : service.vehicles) {
							if (!vehicle.capacity[cargo]) continue;
							if (!vehicle.phase_known) { uncertain = true; observe(Reason::UnknownPhase); continue; }
							if (service.dispatch_period > 0) {
								int64_t offset = stop.departure - pattern.stops[service.dispatch_stop].departure;
								if (offset < 0) offset += pattern.period;
								consider(dispatch_departure(offset, earliest));
								if (stopped) break;
								continue;
							}
							StateTicks base = vehicle.base;
							if (auto live = db.LoadingPhase(Vehicle::GetIfValid(vehicle.id), service, stop)) {
								base = *live;
								if (!reserve(loading, loading.size() + 1)) break;
								loading.push_back(vehicle.id);
							}
							++db.diagnostic.departure_calculations;
							consider(PassengerPeriodicTimetable::NextDeparture(base, stop.departure, pattern.period, StateTicks{earliest}));
						}
						if (!spend(LogWork(loading.size()))) break;
					}
					if (stopped) break;
					if (!departure) { uncertain = true; observe(Reason::UnknownPhase); continue; }
					int64_t wait = 0, cost = parent.cost;
					if (!Difference(departure->base(), parent.time.base(), wait) || !Add(cost, wait) ||
						(parent.rides && (!Add(cost, evaluation.transfer_penalty) || !Add(cost, int64_t(evaluation.minimum_transfer) * evaluation.transfer_time_weight / 1000))) ||
						(evaluation.waiting_weight && (wait > INT64_MAX / evaluation.waiting_weight || !Add(cost, wait * evaluation.waiting_weight / 1000)))) {
						uncertain = true; observe(Reason::Evaluation); continue;
					}
					if (winner != SIZE_MAX && cost > labels[winner].cost) continue;
					/* Board at departure: represent its arrival at this occurrence by
					 * subtracting the dwell, which is restored on the next segment. */
					int64_t dwell = stop.departure - stop.arrival;
					if (departure->base() < INT64_MIN + dwell) { uncertain = true; observe(Reason::Evaluation); continue; }
					Onboard candidate{parent_id, occurrence, occurrence + count, *departure, StateTicks{earliest}, departure->base() - dwell, cost - dwell, parent.first};
					if (parent.rides == 0) {
						candidate.first.service = service.id; candidate.first.departure = *departure;
						candidate.first.boarding_order = stop.order;
						/* Both profiles will alight at the same future occurrence. */
						candidate.first.alighting_order = INVALID_VEH_ORDER_ID;
					}
					auto onboard_dominates = [&](const Onboard &a, const Onboard &b) {
						++stats.onboard_comparisons;
						return (evaluation.waiting_weight == 0 ? a.time <= b.time : a.time == b.time) && a.cost <= b.cost &&
							labels[a.parent].rides <= labels[b.parent].rides && a.expires >= b.expires && FirstKey(a.first) <= FirstKey(b.first);
					};
					bool dominated = false;
					for (const auto &other : onboard) {
						if (!spend(2)) break;
						if (onboard_dominates(other, candidate)) { dominated = true; break; }
					}
					if (stopped) break;
					if (!dominated) {
						std::erase_if(onboard, [&](const Onboard &other) { return onboard_dominates(candidate, other); });
						if (!reserve(onboard, onboard.size() + 1)) break;
						onboard.push_back(candidate);
					}
				}
			}
		}
		previous.clear(); previous.swap(next);
	}
	result.labels = labels.size();
	if (winner != SIZE_MAX) {
		const auto &label = labels[winner];
		result.arrival = label.time; result.cost = label.cost; result.transfers = label.rides - 1;
		for (size_t id = winner; labels[id].parent != SIZE_MAX; id = labels[id].parent) { result.route.push_back(labels[id].leg); ++stats.reconstruction; }
		std::reverse(result.route.begin(), result.route.end()); result.ride = result.route.front();
		/* Unpublished/unknown services and resource omissions can improve the
		 * route. Completeness of this candidate is independent of that proof. */
		answer.outcome = truncated || uncertain || db.IsBuilding() ? Outcome::Provisional : Outcome::Found;
		result.deferred = truncated;
		if (!truncated) result.reason = Reason::Found;
	} else if (truncated) {
		answer.outcome = Outcome::BudgetDeferred;
	} else if (uncertain || db.IsBuilding()) {
		answer.outcome = Outcome::Indeterminate; result.reason = Reason::UnknownPhase;
	} else {
		answer.outcome = Outcome::Unreachable; result.reason = Reason::Unreachable;
	}
	return finish();
}
