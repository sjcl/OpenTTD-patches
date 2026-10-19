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
		int64_t transfer_time = 0;
		int64_t penalty = 0;
		int64_t score = 0;
		int64_t aircraft_adjustment = 0;
		uint32_t boardings = 0;
		uint32_t transfers = 0;
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

	static bool Dominates(const JourneyLabel &a, const JourneyLabel &b, const PassengerJourneyEvaluation &evaluation)
	{
		/* At the same onward departure the elapsed-time part is identical. The
		 * earlier prefix must also have no greater aircraft adjustment. This keeps
		 * cheaper later discounted rides, while retaining the original dominance
		 * for rail paths and for earlier rides with equal or smaller surcharges.
		 * Weighted waiting must include the earlier label's extra wait until that
		 * departure; comparing waiting minus arrival avoids a potentially large sum. */
		return a.arrival <= b.arrival && a.aircraft_adjustment <= b.aircraft_adjustment &&
				(evaluation.waiting_weight == 0 || a.waiting - a.arrival.base() <= b.waiting - b.arrival.base()) &&
				a.waiting <= b.waiting && a.score <= b.score && a.penalty <= b.penalty && a.transfers <= b.transfers &&
				a.prediction_status <= b.prediction_status && a.capacity_uncertain <= b.capacity_uncertain &&
				(a.minimum_free.has_value() == b.minimum_free.has_value()) && (!a.minimum_free || *a.minimum_free >= *b.minimum_free);
	}

	static auto LabelOrder(const JourneyLabel &label)
	{
		/* After time/preferences tie, prefer a path with usable confirmed free seats on every leg.
		 * Estimated and unknown capacity stay tied: neither promises seats to this passenger group. */
		return std::tuple(label.score, label.arrival, label.transfers, label.boardings, label.waiting, label.capacity_uncertain, label.leg.vehicle,
				label.leg.boarding.trip, label.leg.boarding.visit, label.leg.alighting.visit, label.previous);
	}

	/** A relaxed reverse boarding graph: ignore readiness, interchange time and capacity forecasts.
	 * Station distances count necessary boardings; suffix distances count boardings after the current ride.
	 * FIFO station layers and memoized trip suffixes scan each occurrence at most once, even for long routes.
	 * Allocate a trip's distance array only when a connected alighting occurrence actually reaches it.
	 * Search all connected layers once: a stage ceiling must not erase frontiers needed by later stages. */
	struct JourneyReachability {
		static constexpr uint32_t UNREACHABLE = UINT32_MAX;
		std::map<StationID, uint32_t> stations;
		std::map<size_t, std::vector<uint32_t>> suffixes;
		std::optional<StateTicks> earliest_destination;

		uint32_t Boardings(StationID station) const
		{
			auto found = this->stations.find(station);
			return found == this->stations.end() ? UNREACHABLE : found->second;
		}

		uint32_t AfterBoarding(PassengerServiceGraph::VisitID id) const
		{
			auto found = this->suffixes.find(id.trip);
			return found == this->suffixes.end() ? UNREACHABLE : found->second[id.visit];
		}

		template <typename Spend, typename Admit>
		bool Build(const PassengerServiceGraph &graph, std::span<const StationID> seeds, uint32_t seed_distance,
				Spend &spend, Admit &admit)
		{
			std::vector<StationID> queue;
			auto add = [&](StationID station, uint32_t distance) {
				if (this->stations.contains(station)) return true;
				/* Map node, allocator overhead and up to two queue slots per station. */
				if (!spend() || !admit(128)) return false;
				this->stations.emplace(station, distance);
				queue.push_back(station);
				return true;
			};
			for (StationID station : seeds) if (!add(station, seed_distance)) return false;
			for (size_t q = 0; q < queue.size(); ++q) {
				uint32_t distance = this->Boardings(queue[q]);
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
						if (!spend(visits.size()) || !admit(128 + visits.size() * sizeof(uint32_t))) return false;
						found = this->suffixes.emplace(alighting.trip, std::vector<uint32_t>(visits.size(), UNREACHABLE)).first;
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

	/** Only empty, otherwise complete searches need this conservative, untimed corridor check.
	 * Declared stops cannot create a journey, but can show that a longer horizon may be needed.
	 * Each loop/entry pays the same query's work/memory budgets; even cyclic patterns terminate. */
	template <typename Spend, typename Admit>
	static std::optional<bool> HasJourneyCorridor(const PassengerServiceGraph &graph, StationID origin, StationID destination,
			Spend &spend, Admit &admit)
	{
		std::map<StationID, bool> seen;
		std::vector<StationID> queue;
		auto add = [&](StationID station) {
			if (seen.contains(station)) return true;
			if (!spend() || !admit(128)) return false;
			seen.emplace(station, true);
			queue.push_back(station);
			return true;
		};
		if (!add(destination)) return std::nullopt;
		for (size_t q = 0; q < queue.size(); ++q) {
			for (OrderListID id : graph.GetServicesAtStation(queue[q])) {
				if (!spend()) return std::nullopt;
				const auto &service = *graph.GetService(id);
				bool operational = false;
				for (size_t trip : service.trips) {
					if (!spend()) return std::nullopt;
					const auto &vehicle = graph.GetTrips()[trip];
					if (vehicle.operation == PassengerServiceGraph::Operation::Running && vehicle.capacity != 0) { operational = true; break; }
				}
				if (!operational) continue;
				for (size_t end = 0; end < service.stops.size(); ++end) {
					if (!spend()) return std::nullopt;
					if (service.stops[end].station != queue[q] || !service.stops[end].can_alight) continue;
					for (size_t step = 1; step < service.stops.size(); ++step) {
						if (!spend()) return std::nullopt;
						const auto &stop = service.stops[(end + service.stops.size() - step) % service.stops.size()];
						if (stop.can_board) {
							if (stop.station == origin) return true;
							if (!add(stop.station)) return std::nullopt;
						}
						if (stop.forced_transfer) break;
					}
				}
			}
		}
		return false;
	}
}

PassengerJourneyResult PassengerJourneyPlanner::Search(StationID origin, StationID destination, StateTicks ready, uint64_t passengers,
		PassengerJourneyLimits limits, const PassengerJourneyEvaluation &evaluation, std::optional<PassengerServiceGraph::VisitID> onboard) const
{
	PassengerJourneyResult result;
	result.prediction_end = this->graph.GetEnd();
	result.status = this->graph.GetQueryStatus(origin, destination, evaluation.waiting_weight != 0);
	auto limit = [&](PassengerJourneyStopReason reason) {
		result.status = MergeJourneyStatus(result.status, PassengerPredictionStatus::LimitReached);
		if (result.stop_reason == PassengerJourneyStopReason::Exhausted) result.stop_reason = reason;
	};
	if (limits.candidates > 64) limit(PassengerJourneyStopReason::LabelLimit);
	/* Each stored label pays for vector growth, two frontier index entries, and conservative map-node overhead.
	 * Hard caps protect callers as well as defaults; station IDs never set an arbitrary 256-station limit. */
	constexpr size_t LABEL_BYTES = 2 * sizeof(JourneyLabel) + 256;
	const size_t requested_labels = std::min(limits.labels, size_t{1} << 20);
	limits.labels = std::min({ limits.labels, limits.memory_bytes / LABEL_BYTES, size_t{1} << 20 });
	limits.labels_per_station = std::min<size_t>(limits.labels_per_station, 256);
	/* Wide counters and a bounded addition below protect UINT32_MAX caller inputs too. */
	limits.max_transfers = std::min<uint32_t>(limits.max_transfers, 65535);
	uint32_t stage = std::min(limits.initial_max_transfers, limits.max_transfers);
	limits.work = std::min<size_t>(limits.work, 1 << 24);
	limits.candidates = std::min<size_t>(limits.candidates, 64);
	if (limits.labels == 0 || limits.labels_per_station == 0 || limits.candidates == 0) {
		limit(limits.memory_bytes < LABEL_BYTES ? PassengerJourneyStopReason::MemoryLimit : PassengerJourneyStopReason::LabelLimit);
		return result;
	}
	size_t remaining_work = limits.work;
	auto spend = [&](size_t amount = 1) {
		if (amount > remaining_work) {
			result.status = PassengerPredictionStatus::BudgetExhausted;
			result.stop_reason = PassengerJourneyStopReason::WorkLimit;
			return false;
		}
		remaining_work -= amount;
		result.diagnostic.work += amount;
		return true;
	};
	/* Auxiliary storage includes reverse maps and reservations for returned journey paths. */
	size_t auxiliary_bytes = 0, label_bytes = 0;
	auto admit = [&](size_t bytes) {
		if (bytes > limits.memory_bytes - auxiliary_bytes - label_bytes) {
			limit(PassengerJourneyStopReason::MemoryLimit);
			return false;
		}
		auxiliary_bytes += bytes;
		result.diagnostic.scratch_bytes = auxiliary_bytes + label_bytes;
		return true;
	};
	auto reverse_spend = [&](size_t amount = 1) {
		if (!spend(amount)) return false;
		result.diagnostic.reverse_work += amount;
		return true;
	};
	JourneyReachability reachable;
	bool prediction_uncertain = this->graph.GetUncertainStatus(destination) != PassengerPredictionStatus::Complete;
	const std::array<StationID, 1> target{destination};
	if (!reachable.Build(this->graph, target, 0, reverse_spend, admit)) return result;
	/* Search only validated timed occurrences. Untimed routing is lazy and belongs
	 * to the live OD fallback, independently of this search and its costs. */
	auto boardings_left = [&](StationID station) { return reachable.Boardings(station); };
	auto can_finish_ride = [&](PassengerServiceGraph::VisitID id) {
		return reachable.AfterBoarding(id) != JourneyReachability::UNREACHABLE;
	};
	auto finish = [&]() {
		/* Prediction completeness describes the graph, not permission to execute a
		 * validated timed path. Unknown alternatives never need an optimality proof. */
		if (prediction_uncertain) result.status = MergeJourneyStatus(result.status, this->graph.GetUncertainStatus(destination));
		result.diagnostic.unknown_alternatives_ignored = prediction_uncertain && result.HasJourney();
		if (result.stop_reason == PassengerJourneyStopReason::Exhausted && result.status == PassengerPredictionStatus::Complete && result.journeys.empty()) {
			auto corridor = HasJourneyCorridor(this->graph, origin, destination, reverse_spend, admit);
			if (corridor && *corridor) {
				result.status = PassengerPredictionStatus::Unknown;
				result.stop_reason = PassengerJourneyStopReason::PredictionHorizon;
			}
		}
		if (result.stop_reason == PassengerJourneyStopReason::Exhausted && result.status != PassengerPredictionStatus::Complete) result.stop_reason = PassengerJourneyStopReason::PredictionIncomplete;
	};
	if (boardings_left(origin) == JourneyReachability::UNREACHABLE && (!onboard || !can_finish_ride(*onboard))) { finish(); return result; }
	if (boardings_left(origin) > limits.max_transfers + 1 && (!onboard || !can_finish_ride(*onboard))) { limit(PassengerJourneyStopReason::TransferLimit); return result; }
	limits.labels = std::min(limits.labels, (limits.memory_bytes - auxiliary_bytes) / LABEL_BYTES);
	if (limits.labels == 0) {
		limit(PassengerJourneyStopReason::MemoryLimit);
		return result;
	}
	auto label_limit_reason = [&]() {
		return (limits.memory_bytes - auxiliary_bytes) / LABEL_BYTES <= requested_labels ?
				PassengerJourneyStopReason::MemoryLimit : PassengerJourneyStopReason::LabelLimit;
	};
	std::vector<JourneyLabel> labels;
	labels.reserve(std::min<size_t>(limits.labels, 32));
	label_bytes = labels.capacity() * LABEL_BYTES;
	labels.push_back(JourneyLabel{});
	labels.back().arrival = ready;
	std::map<StationID, std::vector<size_t>> frontier{ { origin, { 0 } } };
	std::vector<size_t> destinations;
	bool stopped = false;
	for (uint32_t round = 0; round <= stage && !frontier.empty() && !stopped; ++round) {
		++result.diagnostic.rounds;
		std::map<StationID, std::vector<size_t>> next;
		/* Completed paths need only the requested K results. Keeping every physical
		 * fleet copy/circuit can hit a label limit before certifying an obvious direct
		 * optimum. Strict score bounds preserve capacity and occurrence tie-breaks. */
		auto worse_than_completed = [&](int64_t score) {
			size_t count = destinations.size();
			int64_t worst = 0;
			for (size_t id : destinations) worst = std::max(worst, labels[id].score);
			auto pending = next.find(destination);
			if (pending != next.end()) for (size_t id : pending->second) { ++count; worst = std::max(worst, labels[id].score); }
			return count >= limits.candidates && score > worst;
		};
		for (const auto &[station, active] : frontier) {
			for (size_t previous : active) {
				if (!spend()) { stopped = true; break; }
				const JourneyLabel parent = labels[previous];
				/* Every completion must pay the accumulated costs and at least the relaxed boarding
				 * distance. Once K candidates exist, a strictly worse (score, arrival, boardings)
				 * lower bound cannot alter their order, including the later capacity/ID tie-breaks. */
				if (!this->graph.HasAircraftDiscount() && parent.boardings != 0 && destinations.size() == limits.candidates && reachable.earliest_destination) {
					uint32_t boardings = parent.boardings + boardings_left(station);
					StateTicks arrival = std::max(parent.arrival, *reachable.earliest_destination);
					int64_t transfers = parent.transfers + boardings_left(station);
					int64_t score = JourneyAdd(arrival.base() - ready.base() + parent.aircraft_adjustment, parent.penalty);
					score = JourneyAdd(score, JourneyScale(transfers, evaluation.transfer_penalty, 1));
					score = JourneyAdd(score, JourneyScale(parent.waiting, evaluation.waiting_weight, 1000));
					score = JourneyAdd(score, JourneyScale(transfers * evaluation.minimum_transfer, evaluation.transfer_time_weight, 1000));
					const auto &worst = labels[destinations.back()];
					if (std::tuple(score, arrival, boardings) > std::tuple(worst.score, worst.arrival, worst.boardings)) {
						++result.diagnostic.pruned_labels;
						continue;
					}
				}
				const bool initial_onboard = onboard && parent.boardings == 0;
				const int64_t interchange = parent.boardings == 0 ? 0 : evaluation.minimum_transfer;
				if (parent.arrival.base() > this->graph.GetEnd().base() - interchange) continue;
				StateTicks earliest{ parent.arrival.base() + interchange };
				if (earliest >= this->graph.GetEnd()) continue;
				auto departures = this->graph.GetDepartures(station, earliest, this->graph.GetEnd(), true);
				if (departures.status >= PassengerPredictionStatus::LimitReached) result.status = MergeJourneyStatus(result.status, departures.status);
				prediction_uncertain |= departures.status == PassengerPredictionStatus::Unknown;
				/* The onboard occurrence can be NoLoad and is deliberately absent from the
				 * departure index. Scan it through the same engine as ordinary boardings. */
				for (size_t candidate = 0; candidate < departures.timed.size() + (initial_onboard ? 1 : 0); ++candidate) {
					auto boarding = initial_onboard && candidate == 0 ? *onboard : departures.timed[candidate - (initial_onboard ? 1 : 0)];
					if (!spend()) { stopped = true; break; }
					const bool continuing = initial_onboard && boarding == *onboard;
					if (initial_onboard && candidate != 0 && boarding == *onboard) continue;
					const auto &boarding_stop = this->graph.GetVisitStop(boarding);
					const auto &trip = this->graph.GetTrips()[boarding.trip];
					const auto &start = trip.prediction.visits[boarding.visit];
					const auto from_type = parent.boardings != 0 ? this->graph.GetTrips()[parent.leg.alighting.trip].type :
							onboard ? this->graph.GetTrips()[onboard->trip].type : VehicleType::Invalid;
					const int64_t connection = continuing || from_type == VehicleType::Invalid ? 0 : evaluation.MinimumTransfer(from_type, trip.type, station);
					if (start.departure && *start.departure < parent.arrival + connection) continue;
					if (!start.departure) { prediction_uncertain = true; continue; }
					if (continuing ? boarding_stop.forced_transfer : initial_onboard &&
							!this->graph.GetVisitStop(*onboard).can_alight) continue;
					const uint32_t transfers = parent.transfers + (continuing || (!onboard && parent.boardings == 0) ? 0 : 1);
					if (transfers > limits.max_transfers) continue;
					int64_t minimum_penalty = JourneyAdd(parent.penalty, JourneyScale(transfers, evaluation.transfer_penalty, 1));
					const int64_t waiting = parent.waiting + (continuing ? 0 : start.departure->base() - parent.arrival.base());
					minimum_penalty = JourneyAdd(minimum_penalty, JourneyScale(waiting, evaluation.waiting_weight, 1000));
					minimum_penalty = JourneyAdd(minimum_penalty, JourneyScale(JourneyAdd(parent.transfer_time, connection), evaluation.transfer_time_weight, 1000));
					if (worse_than_completed(JourneyAdd(start.departure->base() - ready.base() + parent.aircraft_adjustment, minimum_penalty))) { ++result.diagnostic.pruned_boardings; continue; }
					if (!can_finish_ride(boarding)) {
						++result.diagnostic.pruned_boardings;
						continue;
					}
					++result.diagnostic.boardings;
					/* Through travel is scanned in one round, including all intermediate dwell. At the same
					 * occurrence only forced unloading requires reboarding. A later circuit is a new,
					 * real boarding opportunity, including when the immediate departure was missed. */
					bool reboard = parent.boardings != 0 && parent.leg.vehicle == trip.vehicle;
					if (reboard && (boarding.visit < parent.leg.alighting.visit ||
							(boarding.visit == parent.leg.alighting.visit && !this->graph.GetVisitStop(boarding).forced_transfer))) continue;
					for (size_t i = boarding.visit + 1; i < trip.prediction.visits.size(); ++i) {
						if (!spend()) { stopped = true; break; }
						++result.diagnostic.visits;
						PassengerServiceGraph::VisitID alighting{ boarding.trip, i };
						const auto &stop = this->graph.GetVisitStop(alighting);
						const auto &visit = trip.prediction.visits[i];
						/* An unknown intermediate time ends the certified suffix: do not jump over refits,
						 * unresolved conditionals/dispatch/full loads, even if later nominal times exist. */
						if (!visit.arrival || *visit.arrival < *start.departure || *visit.arrival >= this->graph.GetEnd()) {
							prediction_uncertain = true;
							break;
						}
						const int64_t aircraft_adjustment = parent.aircraft_adjustment + this->graph.AircraftAdjustment(boarding, alighting);
						if (worse_than_completed(JourneyAdd(visit.arrival->base() - ready.base() + aircraft_adjustment, minimum_penalty))) break;
						if (stop.can_alight && boardings_left(stop.station) != JourneyReachability::UNREACHABLE) {
							/* Waiting at a previous interchange can replace a loop. With weighted waiting,
							 * riding a positive-time circuit can be preferable, so only suppress its zero-time
							 * cycles. Through rides and later physical occurrences remain valid in either case. */
							bool cycle = stop.station == origin && (evaluation.waiting_weight == 0 || *visit.arrival <= ready);
							for (size_t ancestor = previous; !cycle && labels[ancestor].boardings != 0; ancestor = labels[ancestor].previous) {
								if (!spend()) { stopped = true; break; }
								cycle = labels[ancestor].leg.alighting_station == stop.station &&
										(evaluation.waiting_weight == 0 || *visit.arrival <= labels[ancestor].arrival);
							}
							if (stopped) break;
							if (cycle) {
								++result.diagnostic.cycle_rejections;
								if (stop.forced_transfer || !visit.departure) break;
								continue;
							}
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
							if (!continuing && !unknown_capacity && *capacity.future_free < passengers) {
								++result.diagnostic.capacity_rejections;
								if (capacity.certainty != PassengerCapacityCertainty::Confirmed) result.status = MergeJourneyStatus(result.status, PassengerPredictionStatus::Unknown);
							} else {
								JourneyLabel label;
								label.previous = previous;
								label.leg = { trip.vehicle, station, stop.station, *start.departure, *visit.arrival, boarding, alighting,
											PassengerPredictionStatus::Complete, capacity };
								label.arrival = *visit.arrival;
								label.boardings = parent.boardings + 1;
								label.transfers = transfers;
								label.transfer_time = JourneyAdd(parent.transfer_time, connection);
								label.waiting = parent.waiting + (continuing ? 0 : start.departure->base() - parent.arrival.base());
								label.penalty = JourneyAdd(parent.penalty, continuing ? 0 : CapacityPenalty(capacity, evaluation));
								label.capacity_uncertain = parent.capacity_uncertain || (!continuing && (unknown_capacity || capacity.certainty != PassengerCapacityCertainty::Confirmed));
								if (continuing) label.minimum_free = UINT64_MAX; // Already seated; no new seats required.
								else if (!unknown_capacity && (parent.boardings == 0 || parent.minimum_free)) label.minimum_free =
										parent.boardings == 0 ? *capacity.future_free : std::min(*parent.minimum_free, *capacity.future_free);
								label.prediction_status = parent.prediction_status;
								label.aircraft_adjustment = aircraft_adjustment;
								label.score = JourneyAdd(label.arrival.base() - ready.base() + aircraft_adjustment, label.penalty);
								label.score = JourneyAdd(label.score, JourneyScale(transfers, evaluation.transfer_penalty, 1));
								label.score = JourneyAdd(label.score, JourneyScale(label.waiting, evaluation.waiting_weight, 1000));
								label.score = JourneyAdd(label.score, JourneyScale(label.transfer_time, evaluation.transfer_time_weight, 1000));
								if (!spend()) { stopped = true; break; }
								auto &bucket = next[stop.station];
								bool dominated = false;
								std::optional<size_t> completed_to_replace;
								for (size_t id : bucket) {
									if (!spend(1 + label.boardings)) { stopped = true; break; }
									if (SamePath(labels[id], label, labels) && Dominates(labels[id], label, evaluation)) { dominated = true; break; }
								}
								if (stopped) break;
								if (!dominated && stop.station == destination && bucket.size() >= limits.candidates) {
									auto worst = std::max_element(bucket.begin(), bucket.end(), [&](size_t a, size_t b) { return LabelOrder(labels[a]) < LabelOrder(labels[b]); });
									if (LabelOrder(label) >= LabelOrder(labels[*worst])) dominated = true;
									else completed_to_replace = *worst;
								}
								if (!dominated) {
									/* Do not erase an already found candidate before knowing that its replacement
									 * can be stored. Reconstruction storage/work is reserved on admission too. */
									if (labels.size() == limits.labels) { limit(label_limit_reason()); stopped = true; break; }
									if (!spend(bucket.size() * (2 + 2 * label.boardings))) { stopped = true; break; }
									if (stop.station == destination) {
										if (!spend(label.boardings) || !admit(2 * sizeof(PassengerPlannedJourney) + 2 * label.boardings * sizeof(PassengerJourneyLeg))) { stopped = true; break; }
										limits.labels = std::min(limits.labels, (limits.memory_bytes - auxiliary_bytes) / LABEL_BYTES);
										if (labels.size() == limits.labels) { limit(PassengerJourneyStopReason::MemoryLimit); stopped = true; break; }
									}
									std::erase_if(bucket, [&](size_t id) { return id == completed_to_replace || (SamePath(label, labels[id], labels) && Dominates(label, labels[id], evaluation)); });
									if (bucket.size() >= limits.labels_per_station) {
									limit(PassengerJourneyStopReason::LabelLimit);
										auto worst = std::max_element(bucket.begin(), bucket.end(), [&](size_t a, size_t b) { return LabelOrder(labels[a]) < LabelOrder(labels[b]); });
										if (LabelOrder(label) >= LabelOrder(labels[*worst])) dominated = true;
										else bucket.erase(worst);
									}
									if (!dominated) {
										bucket.push_back(labels.size());
										if (labels.size() == labels.capacity()) {
											labels.reserve(std::min(limits.labels, labels.capacity() * 2));
											label_bytes = labels.capacity() * LABEL_BYTES;
										}
										labels.push_back(std::move(label));
									}
								}
							}
						}
						if (stop.forced_transfer || !visit.departure || !can_finish_ride(alighting)) break;
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
		if (round == stage && !frontier.empty() && !stopped) {
			if (!destinations.empty() || stage == limits.max_transfers) {
				limit(PassengerJourneyStopReason::TransferLimit);
			} else {
				stage = std::min(limits.max_transfers, std::max(stage + 1, stage * 2));
				++result.diagnostic.extensions;
			}
		}
	}
	result.journeys.reserve(destinations.size());
	for (size_t id : destinations) {
		const JourneyLabel &label = labels[id];
		PassengerPlannedJourney journey;
		journey.arrival = label.arrival;
		journey.waiting = StateTicksDelta{label.waiting};
		journey.riding = StateTicksDelta{label.arrival.base() - ready.base() - label.waiting};
		journey.generalized_time = StateTicksDelta{label.score};
		journey.capacity_penalty = StateTicksDelta{label.penalty};
		journey.transfers = label.transfers;
		journey.transfer_time = StateTicksDelta{label.transfer_time};
		journey.capacity_uncertain = label.capacity_uncertain;
		journey.prediction_status = label.prediction_status;
		journey.legs.reserve(label.boardings);
		for (size_t current = id; labels[current].boardings != 0; current = labels[current].previous) journey.legs.push_back(labels[current].leg);
		std::reverse(journey.legs.begin(), journey.legs.end());
		result.journeys.push_back(std::move(journey));
	}
	result.diagnostic.labels = labels.size();
	result.diagnostic.label_capacity = labels.capacity();
	finish();
	result.diagnostic.scratch_bytes = auxiliary_bytes + labels.capacity() * LABEL_BYTES;
	return result;
}

PassengerJourneyResult PassengerJourneyPlanner::Plan(CargoType cargo, StationID origin, StationID destination, StateTicks ready,
		PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, uint64_t passengers)
{
	PassengerJourneyResult result;
	result.prediction_end = this->graph.GetEnd();
	if (cargo != this->graph.GetCargo() || origin == StationID::Invalid() || destination == StationID::Invalid() ||
			ready < this->graph.GetStart() || ready >= this->graph.GetEnd() || passengers == 0 ||
			evaluation.minimum_transfer < 0 || evaluation.transfer_penalty < 0 || evaluation.crowding_penalty < 0 || evaluation.capacity_shortfall_penalty < 0) {
		result.status = PassengerPredictionStatus::Unknown;
		result.stop_reason = PassengerJourneyStopReason::InvalidQuery;
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
	auto departures = this->graph.GetDepartures(origin, ready, this->graph.GetEnd(), true);
	/* Fractional waiting weights can change integer tie rounding across readiness ticks. Keep those
	 * queries exact instead of reusing an event interval whose preference ranking could differ. */
	StateTicks anchor = departures.timed.empty() || evaluation.waiting_weight % 1000 != 0 ||
			this->graph.GetCoverage(origin, destination) == PassengerServiceGraph::Coverage::Incomplete ? ready :
			*this->graph.GetTrips()[departures.timed.front().trip].prediction.visits[departures.timed.front().visit].departure;
	CacheKey key{ cargo, origin, destination, anchor, passengers, this->capacity_provider == nullptr ? 0 : this->capacity_provider->Revision(), limits, evaluation, SIZE_MAX, SIZE_MAX };
	auto cached = this->cache.find(key);
	if (cached != this->cache.end()) {
		result = cached->second;
		result.diagnostic.cache_hit = true;
	} else {
		result = this->Search(origin, destination, anchor, passengers, limits, evaluation);
		this->Retain(std::move(key), result);
	}
	if (departures.status >= PassengerPredictionStatus::LimitReached) result.status = MergeJourneyStatus(result.status, departures.status);
	if (result.stop_reason == PassengerJourneyStopReason::Exhausted && result.status != PassengerPredictionStatus::Complete) result.stop_reason = PassengerJourneyStopReason::PredictionIncomplete;
	int64_t initial_wait = anchor.base() - ready.base();
	for (auto &journey : result.journeys) {
		journey.waiting += StateTicksDelta{initial_wait};
		journey.generalized_time = StateTicksDelta{JourneyAdd(journey.generalized_time.base(), JourneyAdd(initial_wait, JourneyScale(initial_wait, evaluation.waiting_weight, 1000)))};
	}
	return result;
}

void PassengerJourneyPlanner::Retain(CacheKey key, const PassengerJourneyResult &result)
{
	size_t bytes = sizeof(CacheKey) + sizeof(PassengerJourneyResult) + 128 + result.journeys.capacity() * sizeof(PassengerPlannedJourney);
	for (const auto &journey : result.journeys) bytes += journey.legs.capacity() * sizeof(PassengerJourneyLeg);
	if (this->cache_limits.entries != 0 && bytes <= this->cache_limits.bytes) {
		if (this->cache.size() >= this->cache_limits.entries || bytes > this->cache_limits.bytes - this->cache_bytes) this->ClearCache();
		this->cache.emplace(std::move(key), result);
		this->cache_bytes += bytes;
	}
}

PassengerJourneyResult PassengerJourneyPlanner::PlanOnboard(CargoType cargo, PassengerServiceGraph::VisitID onboard, StationID destination,
		PassengerJourneyLimits limits, PassengerJourneyEvaluation evaluation, uint64_t passengers)
{
	PassengerJourneyResult result;
	result.status = PassengerPredictionStatus::Unknown;
	if (cargo != this->graph.GetCargo() || destination == StationID::Invalid() || passengers == 0 ||
			onboard.trip >= this->graph.GetTrips().size() || evaluation.minimum_transfer < 0 || evaluation.transfer_penalty < 0 ||
			evaluation.crowding_penalty < 0 || evaluation.capacity_shortfall_penalty < 0) return result;
	const auto &trip = this->graph.GetTrips()[onboard.trip];
	if (onboard.visit >= trip.prediction.visits.size()) return result;
	StationID origin = this->graph.GetVisitStop(onboard).station;
	if (this->graph.FindOnboardVisit(trip.vehicle, origin, trip.current_order) != onboard) return result;
	if (origin == destination) {
		result.status = PassengerPredictionStatus::Complete;
		PassengerPlannedJourney journey;
		journey.arrival = this->graph.GetStart();
		result.journeys.push_back(journey);
		return result;
	}
	CacheKey key{ cargo, origin, destination, this->graph.GetStart(), passengers,
			this->capacity_provider == nullptr ? 0 : this->capacity_provider->Revision(), limits, evaluation, onboard.trip, onboard.visit };
	auto found = this->cache.find(key);
	if (found != this->cache.end()) {
		result = found->second;
		result.diagnostic.cache_hit = true;
		return result;
	}
	result = this->Search(origin, destination, this->graph.GetStart(), passengers, limits, evaluation, onboard);
	this->Retain(std::move(key), result);
	return result;
}
