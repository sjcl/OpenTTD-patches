/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file passenger_od.cpp Passenger OD distribution and station cargo generation. */

#include "stdafx.h"
#include "passenger_od.h"
#include "station_base.h"
#include "linkgraph/linkgraphjob.h"
#include "sl/saveload.h"
#include <chrono>
#include <numeric>

#include "safeguards.h"

PassengerOdStats _passenger_od_stats;
uint64_t _passenger_od_revision = 0;

uint64_t NextPassengerOdRevision()
{
	/* Exhaustion cannot occur in a game; do not silently wrap even with a corrupt save. */
	if (_passenger_od_revision == UINT64_MAX) SlErrorCorrupt("Passenger OD revision exhausted");
	return ++_passenger_od_revision;
}

bool IsPassengerOdEnabled(CargoType cargo)
{
	return _settings_game.linkgraph.passenger_od_assignment && cargo < NUM_CARGO && CargoSpec::Get(cargo)->IsValid() &&
		IsCargoInClass(cargo, CargoClass::Passengers) &&
		_settings_game.linkgraph.GetDistributionType(cargo) != DistributionType::Manual;
}

bool PassengerOdDistribution::Validate() const
{
	const size_t buckets = this->entries.size() + (this->unassigned_weight != 0);
	if (buckets == 0 || this->entries.size() > PASSENGER_OD_NODE_LIMIT ||
			(this->entries.empty() ? this->cursor != 0 : this->cursor >= this->entries.size()) ||
			this->total_weight == 0 || this->total_weight > uint64_t{INVALID_NODE} * UINT32_MAX ||
			this->total_weight > uint64_t{INT64_MAX} / (2 * buckets) ||
			this->unassigned_weight > this->total_weight ||
			(this->unassigned_weight == 0 && this->unassigned_credit != 0)) return false;
	const int64_t bound = static_cast<int64_t>(this->total_weight * buckets);
	int64_t credit_sum = 0;
	auto credit_valid = [&](int64_t credit) {
		if (credit <= -static_cast<int64_t>(this->total_weight) || credit >= bound) return false;
		credit_sum += credit; // At most 2 * bound, checked above before adding.
		return credit_sum > -bound && credit_sum < bound;
	};
	uint64_t total = this->unassigned_weight;
	StationID previous = StationID::Invalid();
	for (const auto &entry : this->entries) {
		if (entry.destination == StationID::Invalid() || entry.node == INVALID_NODE || entry.weight == 0 ||
				(previous != StationID::Invalid() && entry.destination <= previous) || !credit_valid(entry.credit)) return false;
		total += entry.weight;
		previous = entry.destination;
	}
	return credit_valid(this->unassigned_credit) && total == this->total_weight && credit_sum == 0;
}

/**
 * Integer quota rounding, visiting only the retained sparse destinations. A partial
 * selection handles the deficit; excess quotas use a bounded histogram to remove
 * whole rounds followed by one selection. No per-passenger loop or game RNG.
 * O(N) scratch and linear quota/round correction (nth_element selection).
 */
std::vector<uint32_t> PassengerOdDistribution::Allocate(uint32_t amount)
{
	assert(this->Validate());
	const size_t n = this->entries.size();
	std::vector<uint32_t> result(n + 1, 0);
	if (amount == 0) return result;
	const size_t buckets = n + (this->unassigned_weight != 0);
	std::vector<uint64_t> counts(buckets, 0);
	std::vector<size_t> order(buckets);
	std::iota(order.begin(), order.end(), 0);
	const int64_t total = static_cast<int64_t>(this->total_weight);
	auto credit = [&](size_t i) -> int64_t & { return i == n ? this->unassigned_credit : this->entries[i].credit; };
	uint64_t quota_sum = 0, remainder_sum = 0, assigned = 0;
	auto quota = [&](size_t i, uint64_t count, uint64_t remainder) {
		int64_t &residual = credit(i);
		residual += static_cast<int64_t>(remainder);
		if (residual < 0 && count > 0) { --count; residual += total; }
		if (residual >= total) { count += residual / total; residual %= total; }
		counts[i] = count;
		assigned += count;
	};
	for (size_t i = 0; i < n; ++i) {
		uint64_t product = uint64_t{amount} * this->entries[i].weight;
		quota_sum += product / this->total_weight;
		remainder_sum += product % this->total_weight;
		quota(i, product / this->total_weight, product % this->total_weight);
	}
	if (this->unassigned_weight != 0) {
		/* Derive the aggregate bucket from the complement of retained products.
		 * This avoids amount * uint64 weight overflow, without floating point. */
		uint64_t remainder = remainder_sum % this->total_weight;
		quota(n, uint64_t{amount} - quota_sum - remainder_sum / this->total_weight - (remainder != 0),
			remainder == 0 ? 0 : this->total_weight - remainder);
	}
	if (assigned > amount) {
		size_t excess = static_cast<size_t>(assigned - amount);
		assert(excess < buckets);
		std::vector<size_t> histogram(excess + 1, 0);
		size_t active = 0;
		for (auto count : counts) {
			if (count != 0) { ++active; ++histogram[std::min<uint64_t>(count, excess)]; }
		}
		size_t rounds = 0, removed = 0;
		while (active != 0 && excess - removed >= active) {
			removed += active;
			active -= histogram[++rounds];
		}
		for (size_t i = 0; i < buckets; ++i) {
			uint64_t take = std::min<uint64_t>(counts[i], rounds);
			counts[i] -= take;
			credit(i) += static_cast<int64_t>(take * this->total_weight);
		}
		size_t extra = excess - removed;
		if (extra != 0) {
			auto less = [&](size_t a, size_t b) {
				if ((counts[a] == 0) != (counts[b] == 0)) return counts[a] != 0;
				return credit(a) != credit(b) ? credit(a) < credit(b) : a < b;
			};
			std::nth_element(order.begin(), order.begin() + extra, order.end(), less);
			for (size_t k = 0; k < extra; ++k) { --counts[order[k]]; credit(order[k]) += total; }
		}
	} else if (assigned < amount) {
		size_t deficit = static_cast<size_t>(amount - assigned);
		assert(deficit < buckets);
		auto less = [&](size_t a, size_t b) { return credit(a) != credit(b) ? credit(a) > credit(b) : a < b; };
		std::nth_element(order.begin(), order.begin() + deficit, order.end(), less);
		for (size_t k = 0; k < deficit; ++k) { ++counts[order[k]]; credit(order[k]) -= total; }
	}
	assert(this->Validate());
	for (size_t i = 0; i < buckets; ++i) result[i] = static_cast<uint32_t>(counts[i]);
	return result;
}
bool ValidatePassengerOd(const Station &source, CargoType cargo, const PassengerOdDistribution &od)
{
	if (!IsPassengerOdEnabled(cargo) || !od.Validate()) return false;
	const GoodsEntry &ge = source.goods[cargo];
	const LinkGraph *graph = LinkGraph::GetIfValid(od.graph);
	if (graph == nullptr || graph->Cargo() != cargo || graph->PassengerOdRevision() != od.revision ||
			ge.link_graph != od.graph || ge.node != od.source_node || ge.node >= graph->Size() ||
			(*graph)[ge.node].Station() != source.index) return false;
	for (const auto &entry : od.entries) {
		const Station *dest = Station::GetIfValid(entry.destination);
		if (dest == nullptr || dest == &source || entry.node >= graph->Size() ||
				(*graph)[entry.node].Station() != entry.destination || dest->goods[cargo].link_graph != od.graph ||
				dest->goods[cargo].node != entry.node) return false;
	}
	return true;
}

/** Called by FinaliseJob on the simulation thread, before it moves job flows. */
void ApplyPassengerOd(LinkGraphJob &job)
{
	ApplyPassengerOd(job, MAX_PASSENGER_OD_ENTRIES);
}

void ApplyPassengerOd(LinkGraphJob &job, size_t entry_budget)
{
	entry_budget = std::min<size_t>(entry_budget, MAX_PASSENGER_OD_ENTRIES);
	const CargoType cargo = job.Cargo();
	if (!IsPassengerOdEnabled(cargo)) return;
	const LinkGraph &snapshot = job.Graph();
	const LinkGraph *graph = LinkGraph::GetIfValid(snapshot.index);
	if (graph == nullptr || graph->Size() == 0 || graph->Cargo() != cargo || graph->Size() != snapshot.Size() ||
			graph->PassengerOdRevision() != snapshot.PassengerOdRevision()) return;
	/* Validate every node once, rather than once per demand. */
	for (NodeID node = 0; node < graph->Size(); ++node) {
		StationID id = snapshot[node].Station();
		const Station *st = Station::GetIfValid(id);
		if (st == nullptr || (*graph)[node].Station() != id || st->goods[cargo].link_graph != graph->index ||
				st->goods[cargo].node != node) return;
	}
	const auto start = std::chrono::steady_clock::now();
	/* Replacing this graph releases its existing records. Only genuinely dense
	 * jobs share the remaining payload budget; sparse hubs keep every candidate. */
	size_t occupied = 0;
	for (Station *st : Station::Iterate()) {
		for (CargoType old_cargo : EnumRange(NUM_CARGO)) {
			auto &goods = st->goods[old_cargo];
			if (goods.data != nullptr && goods.data->passenger_od != nullptr) {
				const auto &old = *goods.data->passenger_od;
				const LinkGraph *old_graph = LinkGraph::GetIfValid(old.graph);
				/* Free stale topology even if this origin has not generated cargo. */
				if (!IsPassengerOdEnabled(old_cargo) || old_graph == nullptr || old_graph->Cargo() != old_cargo ||
						old_graph->PassengerOdRevision() != old.revision || goods.link_graph != old.graph ||
						goods.node != old.source_node || goods.node >= old_graph->Size() || (*old_graph)[goods.node].Station() != st->index) {
					goods.data->passenger_od.reset();
					continue;
				}
				bool replacing = goods.data->passenger_od->graph == graph->index && &goods == &st->goods[cargo] &&
					goods.node < graph->Size() && (*graph)[goods.node].Station() == st->index;
				if (!replacing) occupied += goods.data->passenger_od->entries.size();
			}
		}
	}
	const size_t available = occupied >= entry_budget ? 0 : entry_budget - occupied;
	uint64_t candidates = 0;
	std::vector<size_t> eligible_counts(graph->Size(), 0);
	auto eligible = [&](const auto &demand, NodeID node) {
		return demand.dest < graph->Size() && demand.dest != node && demand.demand != 0 && demand.unsatisfied_demand < demand.demand;
	};
	for (NodeID node = 0; node < graph->Size(); ++node) {
		for (const auto &demand : job[node].GetDemandAnnotations()) {
			if (eligible(demand, node)) { ++candidates; ++eligible_counts[node]; }
		}
	}
	/* Sparse jobs that fit retain every candidate, even when one hub reaches
	 * thousands of stations. For oversized jobs, satisfy small origins first,
	 * then share all remaining records; no small origin strands its unused share. */
	std::vector<size_t> allowances = eligible_counts;
	if (candidates > available) {
		std::vector<NodeID> origins;
		for (NodeID node = 0; node < graph->Size(); ++node) {
			if (eligible_counts[node] != 0) origins.push_back(node);
		}
		std::sort(origins.begin(), origins.end(), [&](NodeID a, NodeID b) {
			return eligible_counts[a] != eligible_counts[b] ? eligible_counts[a] < eligible_counts[b] : a < b;
		});
		size_t remaining = available;
		for (size_t i = 0; i < origins.size(); ++i) {
			const size_t active = origins.size() - i;
			const size_t share = remaining / active;
			if (eligible_counts[origins[i]] <= share) {
				remaining -= eligible_counts[origins[i]];
				continue;
			}
			const size_t extra = remaining % active;
			for (size_t j = i; j < origins.size(); ++j) allowances[origins[j]] = share + (j - i < extra);
			break;
		}
	}
	auto better = [](const PassengerOdEntry &a, const PassengerOdEntry &b) {
		return a.weight != b.weight ? a.weight > b.weight : a.destination < b.destination;
	};
	for (NodeID node = 0; node < graph->Size(); ++node) {
		GoodsEntry &ge = Station::Get(snapshot[node].Station())->goods[cargo];
		auto od = std::make_unique<PassengerOdDistribution>();
		od->graph = graph->index;
		od->source_node = node;
		od->revision = graph->PassengerOdRevision();
		const size_t capacity = allowances[node];
		od->entries.reserve(capacity);
		for (const auto &demand : job[node].GetDemandAnnotations()) {
			if (!eligible(demand, node)) continue;
			od->total_weight += demand.demand;
			PassengerOdEntry entry{ snapshot[demand.dest].Station(), demand.dest, demand.demand, 0 };
			/* The heap keeps the largest original demands. Every eligible demand
			 * contributes to the denominator, including those in the unknown bucket. */
			if (od->entries.size() < capacity) {
				od->entries.push_back(entry);
				std::push_heap(od->entries.begin(), od->entries.end(), better);
			} else if (capacity != 0 && better(entry, od->entries.front())) {
				std::pop_heap(od->entries.begin(), od->entries.end(), better);
				od->entries.back() = entry;
				std::push_heap(od->entries.begin(), od->entries.end(), better);
			}
		}
		od->unassigned_weight = od->total_weight;
		for (const auto &entry : od->entries) od->unassigned_weight -= entry.weight;
		std::sort(od->entries.begin(), od->entries.end(), [](const auto &a, const auto &b) { return a.destination < b.destination; });
		if (od->Validate()) {
			/* Preserve rounding state only if every destination and weight is unchanged. */
			if (ge.data != nullptr && ge.data->passenger_od != nullptr) {
				const auto &old = *ge.data->passenger_od;
				if (old.graph == od->graph && old.source_node == node && old.revision == od->revision &&
						old.total_weight == od->total_weight && old.unassigned_weight == od->unassigned_weight &&
						old.entries.size() == od->entries.size() && old.Validate()) {
					bool same = true;
					for (size_t i = 0; i < old.entries.size(); ++i) {
						if (old.entries[i].destination != od->entries[i].destination || old.entries[i].node != od->entries[i].node ||
								old.entries[i].weight != od->entries[i].weight) same = false;
					}
					if (same) {
						for (size_t i = 0; i < old.entries.size(); ++i) od->entries[i].credit = old.entries[i].credit;
						od->cursor = old.cursor;
						od->unassigned_credit = old.unassigned_credit;
					}
				}
			}
			++_passenger_od_stats.distributions;
			_passenger_od_stats.entries += od->entries.size();
			ge.CreateData().passenger_od = std::move(od);
		} else if (ge.data != nullptr) {
			ge.data->passenger_od.reset();
		}
	}
	_passenger_od_stats.build_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	Debug(misc, 3, "Passenger OD: {} distributions, {} entries, {} us building", _passenger_od_stats.distributions, _passenger_od_stats.entries, _passenger_od_stats.build_microseconds);
}

void ClearPassengerOdDistributions()
{
	/* Settings changes also discard jobs calculated under the previous distribution mode. */
	for (LinkGraph *graph : LinkGraph::Iterate()) {
		if (IsCargoInClass(graph->Cargo(), CargoClass::Passengers)) graph->InvalidatePassengerOd();
	}
	for (Station *st : Station::Iterate()) {
		for (auto &ge : st->goods) if (ge.data != nullptr) ge.data->passenger_od.reset();
	}
}

void AfterLoadPassengerOd()
{
	if (SlXvIsFeatureMissing(XSLFI_PASSENGER_OD_ASSIGNMENT)) {
		_settings_game.linkgraph.passenger_od_assignment = false;
		_passenger_od_revision = 0;
	}
	for (const LinkGraph *graph : LinkGraph::Iterate()) {
		if (graph->PassengerOdRevision() > _passenger_od_revision) SlErrorCorrupt("Invalid passenger OD graph revision");
	}
	for (const LinkGraphJob *job : LinkGraphJob::Iterate()) {
		if (job->Graph().PassengerOdRevision() > _passenger_od_revision) SlErrorCorrupt("Invalid passenger OD job revision");
	}
	for (Station *st : Station::Iterate()) {
		for (CargoType cargo : EnumRange(NUM_CARGO)) {
			auto &ge = st->goods[cargo];
			if (ge.data != nullptr && ge.data->passenger_od != nullptr &&
					!ValidatePassengerOd(*st, cargo, *ge.data->passenger_od)) ge.data->passenger_od.reset();
		}
	}
	Debug(sl, 2, "Passenger OD afterload: feature {}, enabled {}, revision {}", _sl_xv_feature_versions[XSLFI_PASSENGER_OD_ASSIGNMENT],
		_settings_game.linkgraph.passenger_od_assignment, _passenger_od_revision);
}

/** The legacy next-hop draw remains exactly one per successful generation batch. */
void GenerateStationCargo(Station &st, CargoType cargo, uint amount, Source source)
{
	GoodsEntry &ge = st.goods[cargo];
	StationID next = ge.GetVia(st.index);
	if (!IsPassengerOdEnabled(cargo)) {
		ge.CreateData().cargo.Append(CargoPacket::Create(st.index, amount, source), next);
		return;
	}
	/* UpdateStationWaiting supplies one legacy-sized generation batch. Avoid narrowing Count on
	 * this path; increasing the destination packet limit does not increase Count's physical limit. */
	assert(amount != 0 && amount <= CargoPacket::MAX_COUNT);
	++_passenger_od_stats.batches;
	_passenger_od_stats.generated_passengers += amount;
	const auto start = std::chrono::steady_clock::now();
	auto diagnose = [&](size_t packets) {
		_passenger_od_stats.packets += packets;
		_passenger_od_stats.max_batch_packets = std::max<uint64_t>(_passenger_od_stats.max_batch_packets, packets);
		_passenger_od_stats.max_station_packets = std::max<uint64_t>(_passenger_od_stats.max_station_packets, ge.data->cargo.Packets()->size());
		_passenger_od_stats.peak_pool_packets = std::max<uint64_t>(_passenger_od_stats.peak_pool_packets, CargoPacket::GetNumItems());
		_passenger_od_stats.allocation_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	};
	auto &ged = ge.CreateData();
	auto &od = ged.passenger_od;
	if (od == nullptr || !ValidatePassengerOd(st, cargo, *od)) {
		od.reset();
		ged.cargo.Append(CargoPacket::Create(st.index, amount, source), next);
		diagnose(1);
		_passenger_od_stats.fallback_passengers += amount;
		return;
	}
	/* Quotas are tentative until packets have been emitted. Advance the rounding
	 * phase for emitted destinations and the natural weighted unknown quota. */
	std::vector<int64_t> previous_credits(od->entries.size());
	for (size_t i = 0; i < od->entries.size(); ++i) previous_credits[i] = od->entries[i].credit;
	const int64_t previous_unassigned_credit = od->unassigned_credit;
	auto counts = od->Allocate(amount);
	const size_t positive = std::count_if(counts.begin(), counts.end() - 1, [](uint32_t count) { return count != 0; });
	const size_t packet_limit = std::min<size_t>(MAX_PASSENGER_OD_PACKETS, CargoPacketPool::MAX_SIZE - CargoPacket::GetNumItems());
	if (positive < od->entries.size() && positive + (counts.back() != 0) > packet_limit) {
		/* A small quota can disappear between the tentative batch and the reduced
		 * rounding phase. In that case commit a plan that fits in its entirety:
		 * at most one nonzero destination per planned passenger, plus fallback.
		 * Every rounding award is then emitted, so tiny demands cannot lose turns. */
		for (size_t i = 0; i < od->entries.size(); ++i) od->entries[i].credit = previous_credits[i];
		od->unassigned_credit = previous_unassigned_credit;
		counts = od->Allocate(std::min<uint32_t>(amount, packet_limit > 0 ? static_cast<uint32_t>(packet_limit - 1) : 0));
	}
	/* Reserve an unassigned packet before any split so exhaustion never loses the remainder. */
	CargoPacket *remainder = CargoPacket::Create(st.index, amount, source);
	uint remaining = amount;
	size_t packets = 1;
	const size_t first = od->cursor;
	for (size_t offset = 0; offset < od->entries.size(); ++offset) {
		size_t i = (first + offset) % od->entries.size();
		if (counts[i] == 0) continue;
		/* Advance only after emitting a destination. Exhaustion retries the
		 * remaining destinations without consuming their turn. */
		if (counts[i] == remaining) {
			remainder->SetPassengerDestination(cargo, od->entries[i].destination);
			od->cursor = static_cast<uint16_t>((i + 1) % od->entries.size());
			break;
		}
		if (packets == MAX_PASSENGER_OD_PACKETS || !CargoPacket::CanAllocateItem()) break;
		CargoPacket *cp = remainder->Split(counts[i]);
		if (cp == nullptr) break;
		cp->SetPassengerDestination(cargo, od->entries[i].destination);
		ged.cargo.Append(cp, next);
		remaining -= counts[i];
		++packets;
		od->cursor = static_cast<uint16_t>((i + 1) % od->entries.size());
	}
	if (remainder->GetPassengerDestination() == StationID::Invalid()) {
		_passenger_od_stats.fallback_passengers += remaining;
		/* Weighted unknown demand was processed normally. Only packet-capacity
		 * fallback suppresses the rounding phase of otherwise assignable quotas. */
		const uint32_t suppressed = remaining - counts.back();
		if (suppressed != 0) {
			for (size_t i = 0; i < od->entries.size(); ++i) od->entries[i].credit = previous_credits[i];
			od->unassigned_credit = previous_unassigned_credit;
			/* Only the fractional rounding phase is retained. Unassigned whole quotas
			 * are not a backlog to repay by assigning extra passengers in a later batch.
			 * Packet-limited fairness is provided independently by the saved cursor. */
			od->Allocate(amount - suppressed);
		}
	}
	ged.cargo.Append(remainder, next);
	diagnose(packets);
	Debug(misc, 4, "Passenger OD: {} passengers / {} batches, {} packets, peak batch {} / station {} / pool {}, {} unassigned passengers, {} us allocating",
			_passenger_od_stats.generated_passengers, _passenger_od_stats.batches, _passenger_od_stats.packets,
			_passenger_od_stats.max_batch_packets, _passenger_od_stats.max_station_packets, _passenger_od_stats.peak_pool_packets,
			_passenger_od_stats.fallback_passengers, _passenger_od_stats.allocation_microseconds);
}
