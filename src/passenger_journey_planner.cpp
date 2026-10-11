/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file passenger_journey_planner.cpp Deterministic multi-label boarding rounds over actual predicted visits. */

#include "stdafx.h"
#include "passenger_journey_planner.h"
#include "safeguards.h"

static PassengerPredictionStatus MergeJourneyStatus(PassengerPredictionStatus a, PassengerPredictionStatus b)
{
	return static_cast<PassengerPredictionStatus>(std::max(to_underlying(a), to_underlying(b)));
}

/** Saturation keeps preference costs separate from real, validated timestamps. */
static int64_t JourneyAdd(int64_t a, int64_t b)
{
	return a > INT64_MAX - b ? INT64_MAX : a + b;
}

static int64_t JourneyScale(uint64_t value, uint64_t factor, uint64_t divisor)
{
	if (divisor == 0) return 0;
	/* Floating point is deliberately excluded from synchronized evaluation. */
	if (factor == 0) return 0;
	if (value <= uint64_t{INT64_MAX} / factor) return static_cast<int64_t>(value * factor / divisor);
	/* Avoid saturating an intermediate product when the final ratio fits. Quotient/remainder
	 * long multiplication is portable to MSVC, where an unsigned 128-bit integer is unavailable. */
	uint64_t whole = value / divisor, residue = value % divisor;
	if (whole != 0 && factor > uint64_t{INT64_MAX} / whole) return INT64_MAX;
	uint64_t fraction = 0, remainder = 0;
	for (int bit = 63; bit >= 0; --bit) {
		fraction *= 2;
		if (remainder >= divisor - remainder) { remainder -= divisor - remainder; ++fraction; }
		else remainder += remainder;
		if ((factor >> bit) & 1) {
			if (remainder >= divisor - residue) { remainder -= divisor - residue; ++fraction; }
			else remainder += residue;
		}
	}
	return JourneyAdd(static_cast<int64_t>(whole * factor), static_cast<int64_t>(fraction));
}

static int64_t CapacityPenalty(const PassengerJourneyCapacity &capacity, const PassengerJourneyEvaluation &evaluation)
{
	int64_t penalty = 0;
	if (capacity.certainty != PassengerCapacityCertainty::Unknown && capacity.future_total && capacity.future_free && *capacity.future_total != 0) {
		penalty = JourneyScale(*capacity.future_total - std::min(*capacity.future_total, *capacity.future_free), evaluation.crowding_penalty, *capacity.future_total);
	}
	if (capacity.window_ticks.base() > 0 && capacity.departures_in_window && capacity.seats_per_departure && capacity.demand_in_window && *capacity.demand_in_window != 0) {
		uint64_t frequency = *capacity.departures_in_window, seats = *capacity.seats_per_departure;
		uint64_t supply = seats != 0 && frequency > UINT64_MAX / seats ? UINT64_MAX : frequency * seats;
		uint64_t demand = *capacity.demand_in_window;
		penalty = JourneyAdd(penalty, JourneyScale(demand - std::min(demand, supply), evaluation.capacity_shortfall_penalty, demand));
	}
	return penalty;
}

namespace {
	/** Back pointers bound path storage independently of the number of competing journeys. */
	struct JourneyLabel {
		size_t previous = SIZE_MAX;
		PassengerJourneyLeg leg{};
		StateTicks arrival{};
		int64_t waiting = 0;
		int64_t riding = 0;
		int64_t penalty = 0;
		int64_t score = 0;
		uint8_t boardings = 0;
		bool capacity_uncertain = false;
		std::optional<uint64_t> minimum_free;
		PassengerPredictionStatus prediction_status = PassengerPredictionStatus::Complete;
	};

	static bool SamePath(const JourneyLabel &a, const JourneyLabel &b, const std::vector<JourneyLabel> &labels)
	{
		const JourneyLabel *pa = &a, *pb = &b;
		while (pa->boardings != 0 && pb->boardings != 0) {
			if (pa->leg.vehicle != pb->leg.vehicle || pa->leg.boarding_station != pb->leg.boarding_station ||
					pa->leg.alighting_station != pb->leg.alighting_station) return false;
			pa = &labels[pa->previous]; pb = &labels[pb->previous];
		}
		return pa->boardings == pb->boardings;
	}

	static bool Dominates(const JourneyLabel &a, const JourneyLabel &b)
	{
		return a.arrival <= b.arrival && a.waiting <= b.waiting && a.score <= b.score && a.penalty <= b.penalty &&
				a.prediction_status <= b.prediction_status && a.capacity_uncertain <= b.capacity_uncertain &&
				(a.minimum_free.has_value() == b.minimum_free.has_value()) && (!a.minimum_free || *a.minimum_free >= *b.minimum_free);
	}

	static auto LabelOrder(const JourneyLabel &label)
	{
		/* After time/preferences tie, prefer a path with usable confirmed free seats on every leg.
		 * Estimated and unknown capacity stay tied: neither promises seats to this passenger group. */
		return std::tuple(label.score, label.arrival, label.boardings, label.waiting, label.capacity_uncertain, label.leg.vehicle,
				label.leg.boarding.trip, label.leg.boarding.visit, label.leg.alighting.visit, label.previous);
	}

	/** A relaxed reverse boarding graph: ignore readiness, interchange time and capacity forecasts.
	 * Station distances count necessary boardings; suffix distances count boardings after the current ride.
	 * FIFO station layers and memoized trip suffixes scan each occurrence at most once, even for long routes.
	 * Allocate a trip's byte array only when a connected alighting occurrence actually reaches it. */
	struct JourneyReachability {
		static constexpr uint8_t UNREACHABLE = UINT8_MAX;
		std::map<StationID, uint8_t> stations;
		std::map<size_t, std::vector<uint8_t>> suffixes;
		std::optional<StateTicks> earliest_destination;

		uint8_t Boardings(StationID station) const
		{
			auto found = this->stations.find(station);
			return found == this->stations.end() ? UNREACHABLE : found->second;
		}

		uint8_t AfterBoarding(PassengerServiceGraph::VisitID id) const
		{
			auto found = this->suffixes.find(id.trip);
			return found == this->suffixes.end() ? UNREACHABLE : found->second[id.visit];
		}

		template <typename Spend, typename Admit>
		bool Build(const PassengerServiceGraph &graph, std::span<const StationID> seeds, uint8_t seed_distance,
				uint8_t max_boardings, Spend &spend, Admit &admit)
		{
			std::vector<StationID> queue;
			auto add = [&](StationID station, uint8_t distance) {
				if (this->stations.contains(station)) return true;
				/* Map node, allocator overhead and up to two queue slots per station. */
				if (!spend() || !admit(128)) return false;
				this->stations.emplace(station, distance);
				queue.push_back(station);
				return true;
			};
			for (StationID station : seeds) if (!add(station, seed_distance)) return false;
			for (size_t q = 0; q < queue.size(); ++q) {
				uint8_t distance = this->Boardings(queue[q]);
				if (distance >= max_boardings) continue;
				for (auto alighting : graph.GetStationVisits(queue[q])) {
					if (!spend()) return false;
					const auto &trip = graph.GetTrips()[alighting.trip];
					const auto &visits = trip.prediction.visits;
					if (trip.operation != PassengerServiceGraph::Operation::Running || trip.capacity == 0 ||
							!graph.GetVisitStop(alighting).can_alight || !visits[alighting.visit].arrival) continue;
					if (distance == 0 && (!this->earliest_destination || *visits[alighting.visit].arrival < *this->earliest_destination)) {
						this->earliest_destination = visits[alighting.visit].arrival;
					}
					if (alighting.visit == 0) continue;
					auto found = this->suffixes.find(alighting.trip);
					if (found == this->suffixes.end()) {
						if (!spend(visits.size()) || !admit(128 + visits.size())) return false;
						found = this->suffixes.emplace(alighting.trip, std::vector<uint8_t>(visits.size(), UNREACHABLE)).first;
					}
					auto &suffix = found->second;
					for (size_t i = alighting.visit; i-- > 0;) {
						if (!spend()) return false;
						if (suffix[i] != UNREACHABLE) break; // An earlier BFS layer already owns this suffix.
						if (!visits[i].departure || !visits[i + 1].arrival) break;
						suffix[i] = distance;
						const auto &stop = graph.GetVisitStop({alighting.trip, i});
						if (stop.can_board && !add(stop.station, distance + 1)) return false;
						if (stop.forced_transfer) break; // Boarding here is allowed; through riding is not.
					}
				}
			}
			return true;
		}
	};
}

PassengerJourneyResult PassengerJourneyPlanner::Search(StationID origin, StationID destination, StateTicks ready, uint64_t passengers,
		PassengerJourneyLimits limits, const PassengerJourneyEvaluation &evaluation) const
{
	PassengerJourneyResult result;
	result.status = this->graph.GetStatus();
	if (limits.max_transfers > 8 || limits.candidates > 64) result.status = MergeJourneyStatus(result.status, PassengerPredictionStatus::LimitReached);
	/* Each stored label pays for vector growth, two frontier index entries, and conservative map-node overhead.
	 * Hard caps protect callers as well as defaults; station IDs never set an arbitrary 256-station limit. */
	constexpr size_t LABEL_BYTES = 2 * sizeof(JourneyLabel) + 256;
	limits.labels = std::min({ limits.labels, limits.memory_bytes / LABEL_BYTES, size_t{1} << 20 });
	limits.labels_per_station = std::min<size_t>(limits.labels_per_station, 256);
	limits.max_transfers = std::min<uint8_t>(limits.max_transfers, 8);
	limits.work = std::min<size_t>(limits.work, 1 << 24);
	limits.candidates = std::min<size_t>(limits.candidates, 64);
	if (limits.labels == 0 || limits.labels_per_station == 0 || limits.candidates == 0) {
		result.status = MergeJourneyStatus(result.status, PassengerPredictionStatus::LimitReached);
		return result;
	}
	size_t remaining_work = limits.work;
	auto spend = [&](size_t amount = 1) {
		if (amount > remaining_work) {
			result.status = PassengerPredictionStatus::BudgetExhausted;
			return false;
		}
		remaining_work -= amount;
		result.diagnostic.work += amount;
		return true;
	};
	size_t reachability_bytes = 0;
	auto admit = [&](size_t bytes) {
		if (bytes > limits.memory_bytes - reachability_bytes) {
			result.status = MergeJourneyStatus(result.status, PassengerPredictionStatus::LimitReached);
			return false;
		}
		reachability_bytes += bytes;
		result.diagnostic.scratch_bytes = reachability_bytes;
		return true;
	};
	auto reverse_spend = [&](size_t amount = 1) {
		if (!spend(amount)) return false;
		result.diagnostic.reverse_work += amount;
		return true;
	};
	JourneyReachability reachable, uncertain;
	const std::array<StationID, 1> target{destination};
	if (!reachable.Build(this->graph, target, 0, limits.max_transfers + 1, reverse_spend, admit) ||
			!uncertain.Build(this->graph, this->graph.GetUncertainStations(), 1, limits.max_transfers + 1, reverse_spend, admit)) return result;
	/* Incomplete services seed a second reverse search, even if their dynamic branch captured no
	 * destination visit. Pruning cannot turn a potentially reachable unknown suffix into absence. */
	if (uncertain.Boardings(origin) <= limits.max_transfers + 1) result.status = MergeJourneyStatus(result.status, this->graph.GetUncertainStatus());
	auto boardings_left = [&](StationID station) { return std::min(reachable.Boardings(station), uncertain.Boardings(station)); };
	auto can_finish_ride = [&](PassengerServiceGraph::VisitID id, uint8_t remaining) {
		return std::min(reachable.AfterBoarding(id), uncertain.AfterBoarding(id)) <= remaining ||
				!this->graph.GetTrips()[id.trip].prediction.Covers(this->graph.GetStart(), this->graph.GetEnd());
	};
	if (boardings_left(origin) > limits.max_transfers + 1) return result;
	limits.labels = std::min(limits.labels, (limits.memory_bytes - reachability_bytes) / LABEL_BYTES);
	if (limits.labels == 0) {
		result.status = MergeJourneyStatus(result.status, PassengerPredictionStatus::LimitReached);
		return result;
	}
	std::vector<JourneyLabel> labels;
	labels.reserve(std::min<size_t>(limits.labels, 32));
	labels.push_back(JourneyLabel{});
	labels.back().arrival = ready;
	std::map<StationID, std::vector<size_t>> frontier{ { origin, { 0 } } };
	std::vector<size_t> destinations;
	bool stopped = false;
	for (uint8_t round = 0; round <= limits.max_transfers && !frontier.empty() && !stopped; ++round) {
		std::map<StationID, std::vector<size_t>> next;
		for (const auto &[station, active] : frontier) {
			for (size_t previous : active) {
				if (!spend()) { stopped = true; break; }
				const JourneyLabel parent = labels[previous];
				/* Every completion must pay the accumulated costs and at least the relaxed boarding
				 * distance. Once K candidates exist, a strictly worse (score, arrival, boardings)
				 * lower bound cannot alter their order, including the later capacity/ID tie-breaks. */
				if (destinations.size() == limits.candidates && reachable.earliest_destination) {
					uint8_t boardings = parent.boardings + boardings_left(station);
					StateTicks arrival = std::max(parent.arrival, *reachable.earliest_destination);
					int64_t transfers = boardings - 1;
					int64_t score = JourneyAdd(arrival.base() - ready.base(), parent.penalty);
					score = JourneyAdd(score, JourneyScale(transfers, evaluation.transfer_penalty, 1));
					score = JourneyAdd(score, JourneyScale(parent.waiting, evaluation.waiting_weight, 1000));
					score = JourneyAdd(score, JourneyScale(transfers * evaluation.minimum_transfer, evaluation.transfer_time_weight, 1000));
					const auto &worst = labels[destinations.back()];
					if (std::tuple(score, arrival, boardings) > std::tuple(worst.score, worst.arrival, worst.boardings)) {
						++result.diagnostic.pruned_labels;
						continue;
					}
				}
				const int64_t interchange = parent.boardings == 0 ? 0 : evaluation.minimum_transfer;
				if (parent.arrival.base() > this->graph.GetEnd().base() - interchange) continue;
				StateTicks earliest{ parent.arrival.base() + interchange };
				if (earliest >= this->graph.GetEnd()) continue;
				auto departures = this->graph.GetDepartures(station, earliest, this->graph.GetEnd());
				result.status = MergeJourneyStatus(result.status, departures.status);
				for (auto boarding : departures.timed) {
					if (!spend()) { stopped = true; break; }
					if (!can_finish_ride(boarding, limits.max_transfers - round)) {
						++result.diagnostic.pruned_boardings;
						continue;
					}
					++result.diagnostic.boardings;
					const auto &trip = this->graph.GetTrips()[boarding.trip];
					/* Through travel is scanned in one round, including all intermediate dwell. At the same
					 * occurrence only forced unloading requires reboarding. A later circuit is a new,
					 * real boarding opportunity, including when the immediate departure was missed. */
					bool reboard = parent.boardings != 0 && parent.leg.vehicle == trip.vehicle;
					if (reboard && (boarding.visit < parent.leg.alighting.visit ||
							(boarding.visit == parent.leg.alighting.visit && !this->graph.GetVisitStop(boarding).forced_transfer))) continue;
					const auto &start = trip.prediction.visits[boarding.visit];
					for (size_t i = boarding.visit + 1; i < trip.prediction.visits.size(); ++i) {
						if (!spend()) { stopped = true; break; }
						++result.diagnostic.visits;
						PassengerServiceGraph::VisitID alighting{ boarding.trip, i };
						const auto &stop = this->graph.GetVisitStop(alighting);
						const auto &visit = trip.prediction.visits[i];
						/* An unknown intermediate time ends the certified suffix: do not jump over refits,
						 * unresolved conditionals/dispatch/full loads, even if later nominal times exist. */
						if (!visit.arrival || *visit.arrival < *start.departure || *visit.arrival >= this->graph.GetEnd()) {
							result.status = MergeJourneyStatus(result.status, PassengerPredictionStatus::Unknown);
							break;
						}
						if (stop.can_alight && boardings_left(stop.station) <= limits.max_transfers - round) {
							PassengerJourneyCapacity capacity;
							if (this->capacity_provider != nullptr) {
								if (!spend()) { stopped = true; break; }
								capacity = this->capacity_provider->Estimate(this->graph, boarding, alighting);
							}
							capacity.observed_total = trip.capacity;
							capacity.observed_occupants = trip.occupants;
							capacity.current_free = trip.current_free;
							if (capacity.certainty == PassengerCapacityCertainty::Unknown) capacity.future_free.reset();
							else if (capacity.future_free && capacity.future_total) capacity.future_free = std::min(*capacity.future_free, *capacity.future_total);
							/* Only the captured, currently loading occurrence can certify seats now. */
							if (capacity.certainty == PassengerCapacityCertainty::Unknown && start.loading_now &&
									*start.departure == this->graph.GetStart() && trip.current_free) {
								capacity.future_total = trip.capacity;
								capacity.future_free = trip.current_free;
								capacity.certainty = PassengerCapacityCertainty::Confirmed;
							}
							bool unknown_capacity = capacity.certainty == PassengerCapacityCertainty::Unknown || !capacity.future_free;
							if (!unknown_capacity && *capacity.future_free < passengers) {
								++result.diagnostic.capacity_rejections;
								if (capacity.certainty != PassengerCapacityCertainty::Confirmed) result.status = MergeJourneyStatus(result.status, PassengerPredictionStatus::Unknown);
							} else {
								JourneyLabel label;
								label.previous = previous;
								label.leg = { trip.vehicle, station, stop.station, *start.departure, *visit.arrival, boarding, alighting,
										trip.prediction.status, capacity };
								label.arrival = *visit.arrival;
								label.boardings = parent.boardings + 1;
								label.waiting = parent.waiting + start.departure->base() - parent.arrival.base();
								label.riding = parent.riding + visit.arrival->base() - start.departure->base();
								label.penalty = JourneyAdd(parent.penalty, CapacityPenalty(capacity, evaluation));
								label.capacity_uncertain = parent.capacity_uncertain || unknown_capacity || capacity.certainty != PassengerCapacityCertainty::Confirmed;
								if (!unknown_capacity && (parent.boardings == 0 || parent.minimum_free)) label.minimum_free =
										parent.boardings == 0 ? *capacity.future_free : std::min(*parent.minimum_free, *capacity.future_free);
								label.prediction_status = MergeJourneyStatus(parent.prediction_status, trip.prediction.status);
								int64_t transfers = label.boardings - 1;
								label.score = JourneyAdd(label.arrival.base() - ready.base(), label.penalty);
								label.score = JourneyAdd(label.score, JourneyScale(transfers, evaluation.transfer_penalty, 1));
								label.score = JourneyAdd(label.score, JourneyScale(label.waiting, evaluation.waiting_weight, 1000));
								label.score = JourneyAdd(label.score, JourneyScale(transfers * evaluation.minimum_transfer, evaluation.transfer_time_weight, 1000));
								if (!spend()) { stopped = true; break; }
								auto &bucket = next[stop.station];
								bool dominated = false;
								for (size_t id : bucket) {
									if (!spend(1 + label.boardings)) { stopped = true; break; }
									if (SamePath(labels[id], label, labels) && Dominates(labels[id], label)) { dominated = true; break; }
								}
								if (stopped) break;
								if (!dominated) {
									if (!spend(bucket.size() * (2 + 2 * label.boardings))) { stopped = true; break; }
									std::erase_if(bucket, [&](size_t id) { return SamePath(label, labels[id], labels) && Dominates(label, labels[id]); });
									if (bucket.size() >= limits.labels_per_station) {
										result.status = MergeJourneyStatus(result.status, PassengerPredictionStatus::LimitReached);
										auto worst = std::max_element(bucket.begin(), bucket.end(), [&](size_t a, size_t b) { return LabelOrder(labels[a]) < LabelOrder(labels[b]); });
										if (LabelOrder(label) >= LabelOrder(labels[*worst])) dominated = true;
										else bucket.erase(worst);
									}
									if (!dominated) {
										if (labels.size() == limits.labels) { result.status = MergeJourneyStatus(result.status, PassengerPredictionStatus::LimitReached); stopped = true; break; }
										bucket.push_back(labels.size());
										if (labels.size() == labels.capacity()) labels.reserve(std::min(limits.labels, labels.capacity() * 2));
										labels.push_back(std::move(label));
									}
								}
							}
						}
						if (stop.forced_transfer || !visit.departure || !can_finish_ride(alighting, limits.max_transfers - round)) break;
					}
					if (stopped) break;
				}
				if (stopped) break;
			}
			if (stopped) break;
		}
		auto reached = next.find(destination);
		if (reached != next.end()) {
			destinations.insert(destinations.end(), reached->second.begin(), reached->second.end());
			next.erase(reached); // A journey ends at its final destination.
		}
		std::sort(destinations.begin(), destinations.end(), [&](size_t a, size_t b) { return LabelOrder(labels[a]) < LabelOrder(labels[b]); });
		if (destinations.size() > limits.candidates) destinations.resize(limits.candidates);
		frontier = std::move(next);
	}
	for (size_t id : destinations) {
		const JourneyLabel &label = labels[id];
		PassengerPlannedJourney journey;
		journey.arrival = label.arrival;
		journey.waiting = StateTicksDelta{label.waiting};
		journey.riding = StateTicksDelta{label.riding};
		journey.generalized_time = StateTicksDelta{label.score};
		journey.capacity_penalty = StateTicksDelta{label.penalty};
		journey.transfers = label.boardings - 1;
		journey.transfer_time = StateTicksDelta{int64_t{journey.transfers} * evaluation.minimum_transfer};
		journey.capacity_uncertain = label.capacity_uncertain;
		journey.prediction_status = label.prediction_status;
		for (size_t current = id; labels[current].boardings != 0; current = labels[current].previous) journey.legs.push_back(labels[current].leg);
		std::reverse(journey.legs.begin(), journey.legs.end());
		result.journeys.push_back(std::move(journey));
	}
	result.diagnostic.labels = labels.size();
	result.diagnostic.label_capacity = labels.capacity();
	result.diagnostic.scratch_bytes = reachability_bytes + labels.capacity() * LABEL_BYTES;
	return result;
}

PassengerJourneyResult PassengerJourneyPlanner::Plan(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
		PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, uint64_t passengers)
{
	PassengerJourneyResult result;
	if (cargo != this->graph.GetCargo() || origin == StationID::Invalid() || destination == StationID::Invalid() ||
			ready < this->graph.GetStart() || ready >= this->graph.GetEnd() || passengers == 0 ||
			evaluation.minimum_transfer < 0 || evaluation.transfer_penalty < 0 || evaluation.crowding_penalty < 0 || evaluation.capacity_shortfall_penalty < 0) {
		result.status = PassengerPredictionStatus::Unknown;
		return result;
	}
	if (origin == destination) {
		PassengerPlannedJourney journey;
		journey.arrival = ready;
		result.journeys.push_back(journey);
		return result;
	}
	/* Between two origin departure events the catchable services, future legs and their order are
	 * identical. Search at the next event; add initial waiting/elapsed costs back for this caller.
	 * No rounding into a time bucket can invent a missed departure. Unknown coverage remains unknown. */
	auto departures = this->graph.GetDepartures(origin, ready, this->graph.GetEnd());
	/* Fractional waiting weights can change integer tie rounding across readiness ticks. Keep those
	 * queries exact instead of reusing an event interval whose preference ranking could differ. */
	StateTicks anchor = departures.timed.empty() || evaluation.waiting_weight % 1000 != 0 ? ready :
			*this->graph.GetTrips()[departures.timed.front().trip].prediction.visits[departures.timed.front().visit].departure;
	CacheKey key{ cargo, origin, destination, anchor, passengers, this->capacity_provider == nullptr ? 0 : this->capacity_provider->Revision(), limits, evaluation };
	auto cached = this->cache.find(key);
	if (cached != this->cache.end()) {
		result = cached->second;
		result.diagnostic.cache_hit = true;
	} else {
		result = this->Search(origin, destination, anchor, passengers, limits, evaluation);
		/* A bounded cache stores partial searches too: warm/cold query budgets produce the same journeys. */
		size_t bytes = sizeof(CacheKey) + sizeof(PassengerJourneyResult) + 128 + result.journeys.capacity() * sizeof(PassengerPlannedJourney);
		for (const auto &journey : result.journeys) bytes += journey.legs.capacity() * sizeof(PassengerJourneyLeg);
		if (this->cache_limits.entries != 0 && bytes <= this->cache_limits.bytes) {
			if (this->cache.size() >= this->cache_limits.entries || bytes > this->cache_limits.bytes - this->cache_bytes) this->ClearCache();
			this->cache.emplace(std::move(key), result);
			this->cache_bytes += bytes;
		}
	}
	result.status = MergeJourneyStatus(result.status, departures.status);
	int64_t initial_wait = anchor.base() - ready.base();
	for (auto &journey : result.journeys) {
		journey.waiting += StateTicksDelta{initial_wait};
		journey.generalized_time = StateTicksDelta{JourneyAdd(journey.generalized_time.base(), JourneyAdd(initial_wait, JourneyScale(initial_wait, evaluation.waiting_weight, 1000)))};
	}
	return result;
}
