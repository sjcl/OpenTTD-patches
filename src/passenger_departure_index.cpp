/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_departure_index.cpp Phase successor queries with first-circuit lower bounds. */
#include "stdafx.h"
#include "passenger_departure_index.h"
#include "passenger_service_database.h"
#include "safeguards.h"

PassengerDepartureIndex::PassengerDepartureIndex(const PassengerPeriodicService &service, CargoType cargo) :
		service(service.id), incarnation(service.incarnation), generation(service.generation), period(service.timetable.period)
{
	assert(this->period > 0 && service.dispatch_period == 0 && cargo < NUM_CARGO);
	for (const auto &v : service.vehicles) {
		if (v.capacity[cargo] == 0) continue;
		if (!v.phase_known) { this->unknown = true; continue; }
		int64_t remainder = v.base.base() % this->period;
		if (remainder < 0) remainder += this->period;
		this->phases.push_back({remainder, v.base.base(), v.id}); this->ids.push_back(v.id);
	}
	std::sort(this->phases.begin(), this->phases.end(), [](const auto &a, const auto &b) { return std::tie(a.remainder, a.base, a.id) < std::tie(b.remainder, b.base, b.id); });
	std::sort(this->ids.begin(), this->ids.end());
	for (size_t i = 0; i < this->phases.size(); ++i) this->starts.push_back(i);
	std::sort(this->starts.begin(), this->starts.end(), [&](size_t a, size_t b) { return std::tie(this->phases[a].base, this->phases[a].id) < std::tie(this->phases[b].base, this->phases[b].id); });
	while (this->leaves < this->phases.size()) this->leaves *= 2;
	this->minimum_base.assign(2 * this->leaves, INT64_MAX);
	for (size_t i = 0; i < this->phases.size(); ++i) this->minimum_base[this->leaves + i] = this->phases[i].base;
	for (size_t i = this->leaves; --i > 0;) this->minimum_base[i] = std::min(this->minimum_base[2 * i], this->minimum_base[2 * i + 1]);
}

bool PassengerDepartureIndex::Matches(const PassengerPeriodicService &service) const
{
	return this->service == service.id && this->incarnation == service.incarnation && this->generation == service.generation && this->period == service.timetable.period;
}

size_t PassengerDepartureIndex::Bytes() const
{
	return 256 + sizeof(*this) + this->phases.capacity() * sizeof(Phase) + this->starts.capacity() * sizeof(size_t) +
		this->ids.capacity() * sizeof(VehicleID) + this->minimum_base.capacity() * sizeof(int64_t);
}

size_t PassengerDepartureIndex::Find(size_t node, size_t begin, size_t end, size_t from, size_t until, int64_t floor, std::span<const VehicleID> excluded) const
{
	if (end <= from || begin >= until || begin >= this->phases.size() || this->minimum_base[node] > floor) return SIZE_MAX;
	if (end - begin == 1) return std::binary_search(excluded.begin(), excluded.end(), this->phases[begin].id) ? SIZE_MAX : begin;
	size_t middle = begin + (end - begin) / 2;
	size_t first = this->Find(2 * node, begin, middle, from, until, floor, excluded);
	return first != SIZE_MAX ? first : this->Find(2 * node + 1, middle, end, from, until, floor, excluded);
}

std::optional<StateTicks> PassengerDepartureIndex::Next(int64_t offset, StateTicks ready, std::span<const VehicleID> excluded, uint64_t &calculations) const
{
	assert(offset >= 0);
	std::optional<StateTicks> result;
	/* Active entries have base+offset <= ready. Their cyclic successor is
	 * found by remainder, with the tree enforcing the first-circuit bound.
	 * Future entries instead depart at their first base+offset. Both subsets
	 * must be considered: sorting remainders alone invents earlier circuits.
	 * Subtract only when representable; otherwise every entry is future. */
	bool active = ready.base() >= INT64_MIN + offset;
	int64_t floor = active ? ready.base() - offset : INT64_MIN;
	if (active) {
		int64_t remainder = ready.base() % this->period;
		if (remainder < 0) remainder += this->period;
		remainder -= offset < this->period ? offset : offset % this->period;
		if (remainder < 0) remainder += this->period;
		size_t split = std::lower_bound(this->phases.begin(), this->phases.end(), remainder, [](const auto &phase, int64_t value) { return phase.remainder < value; }) - this->phases.begin();
		size_t index;
		if (!this->starts.empty() && excluded.empty() && this->phases[this->starts.back()].base <= floor) {
			index = split < this->phases.size() ? split : 0; // Common case: every vehicle has started.
		} else {
			index = this->Find(1, 0, this->leaves, split, this->phases.size(), floor, excluded);
			if (index == SIZE_MAX) index = this->Find(1, 0, this->leaves, 0, split, floor, excluded);
		}
		if (index != SIZE_MAX) {
			++calculations;
			int64_t delta = this->phases[index].remainder - remainder;
			if (delta < 0) delta += this->period;
			if (ready.base() <= INT64_MAX - delta) result = StateTicks{ready.base() + delta};
		}
	}
	auto future = active ? std::upper_bound(this->starts.begin(), this->starts.end(), floor, [&](int64_t value, size_t index) { return value < this->phases[index].base; }) : this->starts.begin();
	while (future != this->starts.end() && std::binary_search(excluded.begin(), excluded.end(), this->phases[*future].id)) ++future;
	if (future != this->starts.end()) {
		++calculations;
		int64_t base = this->phases[*future].base;
		if (base <= INT64_MAX - offset) {
			StateTicks first{base + offset};
			if (!result || first < *result) result = first;
		}
	}
	return result;
}
