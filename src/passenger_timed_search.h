/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
/** @file passenger_timed_search.h Simulation-thread scratch for the periodic forward search. */
#ifndef PASSENGER_TIMED_SEARCH_H
#define PASSENGER_TIMED_SEARCH_H

#include "passenger_journey_type.h"
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <queue>
#include <span>
#include <tuple>

struct PassengerTimedLabel {
	StationID station{};
	StateTicks time{};
	int64_t cost = 0;
	size_t parent = SIZE_MAX, rides = 0;
	PassengerJourneyExecution leg{}, first{};
	bool active = true;
	size_t next = SIZE_MAX; ///< Intrusive frontier link, in original insertion order.
	bool complete_valid = false; ///< Admitted destination with a current, connected chain to the origin.
};
static_assert(sizeof(PassengerTimedLabel) <= 512);

/* Keep every original key, including the unique insertion index. Inheriting only
 * exposes the standard queue's vector for reuse; push/pop/top are unchanged. */
using PassengerTimedQueueItem = std::tuple<int64_t, int64_t, StateTicks, size_t, StateTicks, OrderListID, VehicleOrderID, VehicleOrderID, size_t>;
struct PassengerTimedQueue : std::priority_queue<PassengerTimedQueueItem, std::vector<PassengerTimedQueueItem>, std::greater<PassengerTimedQueueItem>> {
	size_t CapacityBytes() const { return this->c.capacity() * sizeof(PassengerTimedQueueItem); }
	const auto &Items() const { return this->c; } ///< Read-only diagnostics; never pop or reorder the heap.
	void Clear() { this->c.clear(); }
	void Release() { std::vector<PassengerTimedQueueItem>{}.swap(this->c); }
};

/** Owned by one DB, never shared with diagnostic DBs or serialized. Only touched
 * rows are reset. Lazy pages also keep a sparse high station ID inexpensive. */
struct PassengerTimedSearchScratch {
	static constexpr size_t MAX_RETAINED_BYTES = 8 * 1024 * 1024;
	std::vector<PassengerTimedLabel> labels;
	PassengerTimedQueue queue;
	std::vector<VehicleID> loading;
	bool in_use = false;

	struct Frontier { size_t head = SIZE_MAX, tail = SIZE_MAX; };
	static constexpr size_t FRONTIER_PAGE_SIZE = 64, FRONTIER_PAGE_COUNT = (size_t{UINT16_MAX} + 1) / FRONTIER_PAGE_SIZE;
	using Page = std::array<Frontier, FRONTIER_PAGE_SIZE>;
	std::array<std::unique_ptr<Page>, FRONTIER_PAGE_COUNT> frontiers{};
	size_t pages = 0;
	std::vector<StationID> touched;
	std::vector<VehicleID> &Loading(std::vector<VehicleID> &) { return this->loading; }

	struct Iterator {
		const std::vector<PassengerTimedLabel> *labels;
		size_t index;
		size_t operator*() const { return this->index; }
		Iterator &operator++() { this->index = (*this->labels)[this->index].next; return *this; }
		bool operator!=(const Iterator &other) const { return this->index != other.index; }
	};
	struct Range {
		const std::vector<PassengerTimedLabel> *labels;
		size_t head;
		Iterator begin() const { return {this->labels, this->head}; }
		Iterator end() const { return {this->labels, SIZE_MAX}; }
	};
	Range At(StationID station) const
	{
		const auto &page = this->frontiers[station.base() / FRONTIER_PAGE_SIZE];
		return {&this->labels, page ? (*page)[station.base() % FRONTIER_PAGE_SIZE].head : SIZE_MAX};
	}
	void Append(StationID station, size_t index)
	{
		const size_t slot = station.base();
		auto &page = this->frontiers[slot / FRONTIER_PAGE_SIZE];
		if (!page) { page = std::make_unique<Page>(); ++this->pages; }
		auto &row = (*page)[slot % FRONTIER_PAGE_SIZE];
		if (row.head == SIZE_MAX) {
			this->touched.push_back(station);
			row.head = index;
		} else {
			this->labels[row.tail].next = index;
		}
		row.tail = index;
	}
	size_t CapacityBytes() const
	{
		return this->labels.capacity() * sizeof(PassengerTimedLabel) + this->queue.CapacityBytes() +
			this->loading.capacity() * sizeof(VehicleID) + this->pages * sizeof(Page) + this->touched.capacity() * sizeof(StationID);
	}
	void Release()
	{
		std::vector<PassengerTimedLabel>{}.swap(this->labels);
		this->queue.Release();
		std::vector<VehicleID>{}.swap(this->loading);
		for (auto &page : this->frontiers) page.reset();
		this->pages = 0;
		std::vector<StationID>{}.swap(this->touched);
	}
	void Clear()
	{
		for (StationID station : this->touched) (*this->frontiers[station.base() / FRONTIER_PAGE_SIZE])[station.base() % FRONTIER_PAGE_SIZE] = {};
		this->touched.clear();
		/* Erase path identities before retaining storage; no old labels remain
		 * live or readable by the next search, including on a budget exit. */
		std::fill(this->labels.begin(), this->labels.end(), PassengerTimedLabel{});
		this->labels.clear(); this->queue.Clear(); this->loading.clear();
		if (this->CapacityBytes() > MAX_RETAINED_BYTES) this->Release();
	}
	struct Scope {
		PassengerTimedSearchScratch &scratch;
		explicit Scope(PassengerTimedSearchScratch &scratch) : scratch(scratch) { this->scratch.in_use = true; }
		~Scope() { this->scratch.Clear(); this->scratch.in_use = false; }
	};
};

/** Explicit comparison only: the original tree, per-station vectors and fresh
 * standard priority queue. Both storage adapters run the same search body. */
struct PassengerTimedLegacySearch {
	std::vector<PassengerTimedLabel> labels;
	PassengerTimedQueue queue;
	std::map<StationID, std::vector<size_t>> frontiers;
	std::vector<VehicleID> &Loading(std::vector<VehicleID> &temporary) { return temporary; }
	std::span<const size_t> At(StationID station) const
	{
		auto found = this->frontiers.find(station);
		return found == this->frontiers.end() ? std::span<const size_t>{} : found->second;
	}
	void Append(StationID station, size_t index) { this->frontiers[station].push_back(index); }
};

#endif /* PASSENGER_TIMED_SEARCH_H */
