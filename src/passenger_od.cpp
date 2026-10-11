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
	if (this->entries.empty() || this->entries.size() > MAX_PASSENGER_OD_DESTINATIONS ||
			this->total_weight == 0 || this->total_weight > MAX_PASSENGER_OD_DESTINATIONS * uint64_t{UINT32_MAX}) return false;
	uint64_t total = 0;
	int64_t credit_sum = 0;
	StationID previous = StationID::Invalid();
	for (const auto &entry : this->entries) {
		if (entry.destination == StationID::Invalid() || entry.node == INVALID_NODE || entry.weight == 0 ||
				(previous != StationID::Invalid() && entry.destination <= previous)) return false;
		/* Each negative remainder is greater than -W; with sum zero, positive remainders
		 * are less than (N-1)W. These bounds also make every signed addition safe. */
		if (entry.credit <= -static_cast<int64_t>(this->total_weight) ||
				entry.credit >= static_cast<int64_t>(this->total_weight * this->entries.size())) return false;
		total += entry.weight;
		credit_sum += entry.credit;
		previous = entry.destination;
	}
	return total == this->total_weight && credit_sum == 0;
}

/**
 * Carry rounding errors between batches. Quotients and remainders are computed
 * separately so uint32 amount * uint32 weight fits uint64, even for UINT32_MAX.
 * Correct at most N rounded units with a heap of destination indices (not passengers).
 * Tie breaking is StationID order. Sum of credits stays zero, and individual credit
 * stays in (-W, (N-1)W), hence long-term allocation error is bounded by N passengers.
 * Complexity O(N log N), O(N) stack scratch, no additional game random calls.
 */
std::array<uint32_t, MAX_PASSENGER_OD_DESTINATIONS> PassengerOdDistribution::Allocate(uint32_t amount)
{
	assert(this->Validate());
	if (amount == 0) return {};
	std::array<uint64_t, MAX_PASSENGER_OD_DESTINATIONS> counts{};
	std::array<size_t, MAX_PASSENGER_OD_DESTINATIONS> heap{};
	uint64_t assigned = 0;
	const int64_t total = static_cast<int64_t>(this->total_weight);
	for (size_t i = 0; i < this->entries.size(); ++i) {
		auto &entry = this->entries[i];
		uint64_t product = uint64_t{amount} * entry.weight;
		uint64_t count = product / this->total_weight;
		entry.credit += static_cast<int64_t>(product % this->total_weight);
		if (entry.credit < 0 && count > 0) {
			--count;
			entry.credit += total;
		}
		if (entry.credit >= total) {
			count += entry.credit / total;
			entry.credit %= total;
		}
		/* A quota can exceed amount by up to N before correction. Keep scratch wide. */
		assigned += count;
		counts[i] = count;
		heap[i] = i;
	}
	const bool excess = assigned > amount;
	auto less = [&](size_t a, size_t b) {
		if (excess && (counts[a] == 0) != (counts[b] == 0)) return counts[a] == 0;
		if (this->entries[a].credit == this->entries[b].credit) return a > b;
		return excess ? this->entries[a].credit > this->entries[b].credit : this->entries[a].credit < this->entries[b].credit;
	};
	auto end = heap.begin() + this->entries.size();
	std::make_heap(heap.begin(), end, less);
	while (assigned != amount) {
		std::pop_heap(heap.begin(), end, less);
		size_t i = *(end - 1);
		if (excess) {
			assert(counts[i] > 0);
			--counts[i];
			this->entries[i].credit += total;
			--assigned;
		} else {
			++counts[i];
			this->entries[i].credit -= total;
			++assigned;
		}
		std::push_heap(heap.begin(), end, less);
	}
	assert(this->Validate());
	std::array<uint32_t, MAX_PASSENGER_OD_DESTINATIONS> result{};
	for (size_t i = 0; i < this->entries.size(); ++i) result[i] = static_cast<uint32_t>(counts[i]);
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
	const CargoType cargo = job.Cargo();
	if (!IsPassengerOdEnabled(cargo)) return;
	const LinkGraph &snapshot = job.Graph();
	const LinkGraph *graph = LinkGraph::GetIfValid(snapshot.index);
	if (graph == nullptr || graph->Cargo() != cargo || graph->Size() != snapshot.Size() ||
			graph->PassengerOdRevision() != snapshot.PassengerOdRevision()) return;
	/* Validate every node once, rather than once per demand. */
	for (NodeID node = 0; node < graph->Size(); ++node) {
		StationID id = snapshot[node].Station();
		const Station *st = Station::GetIfValid(id);
		if (st == nullptr || (*graph)[node].Station() != id || st->goods[cargo].link_graph != graph->index ||
				st->goods[cargo].node != node) return;
	}
	const auto start = std::chrono::steady_clock::now();
	for (NodeID node = 0; node < graph->Size(); ++node) {
		GoodsEntry &ge = Station::Get(snapshot[node].Station())->goods[cargo];
		auto od = std::make_unique<PassengerOdDistribution>();
		od->graph = graph->index;
		od->source_node = node;
		od->revision = graph->PassengerOdRevision();
		for (const auto &demand : job[node].GetDemandAnnotations()) {
			if (demand.dest >= graph->Size() || demand.dest == node || demand.demand == 0 ||
					demand.unsatisfied_demand >= demand.demand) continue;
			if (od->entries.size() == MAX_PASSENGER_OD_DESTINATIONS) {
				/* Do not renormalise a truncated distribution onto the retained destinations. */
				od->entries.clear();
				break;
			}
			od->entries.push_back({ snapshot[demand.dest].Station(), demand.dest, demand.demand, 0 });
			od->total_weight += demand.demand;
		}
		std::sort(od->entries.begin(), od->entries.end(), [](const auto &a, const auto &b) { return a.destination < b.destination; });
		if (od->Validate()) {
			/* Preserve rounding state only if every destination and weight is unchanged. */
			if (ge.data != nullptr && ge.data->passenger_od != nullptr) {
				const auto &old = *ge.data->passenger_od;
				if (old.graph == od->graph && old.source_node == node && old.revision == od->revision &&
						old.total_weight == od->total_weight && old.entries.size() == od->entries.size() && old.Validate()) {
					bool same = true;
					for (size_t i = 0; i < old.entries.size(); ++i) {
						if (old.entries[i].destination != od->entries[i].destination || old.entries[i].node != od->entries[i].node ||
								old.entries[i].weight != od->entries[i].weight) same = false;
					}
					if (same) for (size_t i = 0; i < old.entries.size(); ++i) od->entries[i].credit = old.entries[i].credit;
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
	auto &ged = ge.CreateData();
	auto &od = ged.passenger_od;
	if (od == nullptr || !ValidatePassengerOd(st, cargo, *od)) {
		od.reset();
		ged.cargo.Append(CargoPacket::Create(st.index, amount, source), next);
		++_passenger_od_stats.packets;
		_passenger_od_stats.fallback_passengers += amount;
		return;
	}
	const auto start = std::chrono::steady_clock::now();
	auto counts = od->Allocate(amount);
	/* Reserve an unassigned packet before any split so exhaustion never loses the remainder. */
	CargoPacket *remainder = CargoPacket::Create(st.index, amount, source);
	uint remaining = amount;
	size_t packets = 1;
	for (size_t i = 0; i < od->entries.size(); ++i) {
		if (counts[i] == 0) continue;
		if (counts[i] == remaining) {
			remainder->SetPassengerDestination(cargo, od->entries[i].destination);
			break;
		}
		if (packets == MAX_PASSENGER_OD_PACKETS || !CargoPacket::CanAllocateItem()) break;
		CargoPacket *cp = remainder->Split(counts[i]);
		if (cp == nullptr) break;
		cp->SetPassengerDestination(cargo, od->entries[i].destination);
		ged.cargo.Append(cp, next);
		remaining -= counts[i];
		++packets;
	}
	if (remainder->GetPassengerDestination() == StationID::Invalid()) _passenger_od_stats.fallback_passengers += remaining;
	ged.cargo.Append(remainder, next);
	_passenger_od_stats.packets += packets;
	_passenger_od_stats.allocation_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	Debug(misc, 4, "Passenger OD: {} packets, {} unassigned passengers, {} us allocating", _passenger_od_stats.packets, _passenger_od_stats.fallback_passengers, _passenger_od_stats.allocation_microseconds);
}
