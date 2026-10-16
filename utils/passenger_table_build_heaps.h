/* Isolated heap candidates for the passenger build benchmark. */
#pragma once
#include <array>
#include <bit>
#include <queue>
#include <tuple>
#include <vector>
#ifndef PASSENGER_TABLE_COUNT
#define PASSENGER_TABLE_COUNT 1
#endif

template <typename Queue> size_t PassengerBenchmarkQueueBytes(const Queue &queue)
{
	if constexpr (requires { queue.bytes(); }) return queue.bytes();
	else if constexpr (requires { queue.AllocatedBytes(); }) return queue.AllocatedBytes();
	else {
		struct Access : Queue { static auto Storage() { return &Access::c; } };
		return (queue.*Access::Storage()).capacity() * sizeof(typename Queue::value_type);
	}
}

template <typename Item> struct PassengerQueueGreater {
	inline static uint64_t comparisons = 0;
	bool operator()(const Item &a, const Item &b) const { if constexpr (PASSENGER_TABLE_COUNT) ++comparisons; return a > b; }
};

/** Four children reduce dependent levels, at the price of more comparisons. */
template <typename Item> class PassengerFourQueue {
	std::vector<Item> heap;
	bool Before(const Item &a, const Item &b) { return PassengerQueueGreater<Item>{}(b, a); }
public:
	bool empty() const { return this->heap.empty(); }
	size_t size() const { return this->heap.size(); }
	const Item &top() const { return this->heap.front(); }
	void reset(size_t) { assert(this->empty()); }
	template <typename... Args> void emplace(Args... args)
	{
		Item item(args...); size_t at = this->heap.size(); this->heap.push_back(item);
		while (at != 0) {
			size_t parent = (at - 1) / 4;
			if (!this->Before(item, this->heap[parent])) break;
			this->heap[at] = this->heap[parent]; at = parent;
		}
		this->heap[at] = item;
	}
	void pop()
	{
		Item item = this->heap.back(); this->heap.pop_back();
		if (this->heap.empty()) return;
		size_t at = 0;
		while (at * 4 + 1 < this->heap.size()) {
			size_t child = at * 4 + 1;
			const size_t end = std::min(child + 4, this->heap.size());
			for (size_t i = child + 1; i < end; ++i) if (this->Before(this->heap[i], this->heap[child])) child = i;
			if (!this->Before(this->heap[child], item)) break;
			this->heap[at] = this->heap[child]; at = child;
		}
		this->heap[at] = item;
	}
	size_t bytes() const { return this->heap.capacity() * sizeof(Item); }
};

/** Actual frozen graph bounds select the packed representation. A finite
 * improving path cannot contain a cycle of nonnegative (cost, legs), so it
 * contains at most states-1 edges. Guard each insertion as well; even an
 * erroneous future bound causes a lossless migration to wide keys. */
template <typename Item> class PassengerPackedFourQueue {
	using Packed = std::tuple<uint64_t, uint32_t>;
	PassengerFourQueue<Packed> packed;
	PassengerFourQueue<Item> wide;
	unsigned leg_bits = 0;
	bool enabled = false;
	Item current;
public:
	void SetBounds(uint64_t states, uint64_t max_cost)
	{
		assert(this->empty());
		this->leg_bits = std::bit_width(states);
		this->enabled = this->leg_bits < 64 && (states == 0 || max_cost <= (UINT64_MAX >> this->leg_bits) / states);
	}
	bool PackedEnabled() const { return this->enabled; }
	unsigned LegBits() const { return this->leg_bits; }
	void reset(size_t) { assert(this->empty()); }
	bool empty() const { return this->packed.empty() && this->wide.empty(); }
	size_t size() const { return this->packed.size() + this->wide.size(); }
	template <typename... Args> void emplace(Args... args)
	{
		Item item(args...);
		uint64_t cost = std::get<0>(item), legs = std::get<1>(item);
		if (this->enabled && (cost > (UINT64_MAX >> this->leg_bits) || legs >= (uint64_t{1} << this->leg_bits))) {
			while (!this->packed.empty()) {
				auto [key, state] = this->packed.top(); this->packed.pop();
				this->wide.emplace(static_cast<int64_t>(key >> this->leg_bits), static_cast<uint32_t>(key & ((uint64_t{1} << this->leg_bits) - 1)), state);
			}
			this->enabled = false;
		}
		if (this->enabled) this->packed.emplace((cost << this->leg_bits) | legs, std::get<2>(item));
		else this->wide.emplace(item);
	}
	const Item &top()
	{
		if (!this->enabled) return this->wide.top();
		auto [key, state] = this->packed.top();
		this->current = Item(static_cast<int64_t>(key >> this->leg_bits), static_cast<uint32_t>(key & ((uint64_t{1} << this->leg_bits) - 1)), state);
		return this->current;
	}
	void pop() { if (this->enabled) this->packed.pop(); else this->wide.pop(); }
	size_t bytes() const { return this->wide.bytes() + this->packed.bytes(); }
};

template <typename Item> class PassengerIndexedQueue {
	std::vector<Item> heap;
	std::vector<uint32_t> position;
	bool Before(const Item &a, const Item &b) { if constexpr (PASSENGER_TABLE_COUNT) ++PassengerQueueGreater<Item>::comparisons; return a < b; }
	void Put(size_t at, const Item &item) { this->heap[at] = item; this->position[std::get<2>(item)] = static_cast<uint32_t>(at); }
public:
	uint64_t pushes = 0, decreases = 0;
	bool empty() const { return this->heap.empty(); }
	size_t size() const { return this->heap.size(); }
	const Item &top() const { return this->heap.front(); }
	void reset(size_t states) { this->heap.clear(); this->position.assign(states, UINT32_MAX); }
	template <typename... Args> void emplace(Args... args)
	{
		Item item(args...); uint32_t state = std::get<2>(item);
		size_t at = this->position[state];
		if (at == UINT32_MAX) { ++this->pushes; at = this->heap.size(); this->heap.push_back(item); }
		else { ++this->decreases; if (!this->Before(item, this->heap[at])) return; }
		while (at != 0) {
			size_t parent = (at - 1) / 2;
			if (!this->Before(item, this->heap[parent])) break;
			this->Put(at, this->heap[parent]); at = parent;
		}
		this->Put(at, item);
	}
	void pop()
	{
		this->position[std::get<2>(this->heap.front())] = UINT32_MAX;
		Item last = this->heap.back(); this->heap.pop_back();
		if (this->heap.empty()) return;
		size_t at = 0;
		while (2 * at + 1 < this->heap.size()) {
			size_t child = 2 * at + 1;
			if (child + 1 < this->heap.size() && this->Before(this->heap[child + 1], this->heap[child])) ++child;
			if (!this->Before(this->heap[child], last)) break;
			this->Put(at, this->heap[child]); at = child;
		}
		this->Put(at, last);
	}
	size_t bytes() const { return this->heap.capacity() * sizeof(Item) + this->position.capacity() * sizeof(uint32_t); }
};

/** Only cost is monotone. Bucket zero is a full (cost, legs, state) heap;
 * zero-cost relaxations can insert a smaller state after the preceding pop. */
template <typename Item> class PassengerRadixQueue {
	std::array<std::vector<Item>, 65> buckets;
	std::priority_queue<Item, std::vector<Item>, PassengerQueueGreater<Item>> zero;
	uint64_t last = 0;
	size_t count = 0;
	void Insert(const Item &item)
	{
		uint64_t cost = static_cast<uint64_t>(std::get<0>(item)); assert(cost >= this->last);
		if (cost == this->last) this->zero.push(item);
		else this->buckets[std::bit_width(cost ^ this->last)].push_back(item);
	}
public:
	uint64_t pushes = 0, decreases = 0;
	bool empty() const { return this->count == 0; }
	size_t size() const { return this->count; }
	void reset(size_t) { assert(this->empty()); this->last = 0; }
	template <typename... Args> void emplace(Args... args) { ++this->pushes; ++this->count; this->Insert(Item(args...)); }
	const Item &top()
	{
		if (this->zero.empty()) {
			size_t bucket = 1; while (this->buckets[bucket].empty()) ++bucket;
			this->last = UINT64_MAX;
			for (const auto &item : this->buckets[bucket]) this->last = std::min<uint64_t>(this->last, std::get<0>(item));
			/* Redistribution always targets a lower bucket. */
			for (const auto &item : this->buckets[bucket]) this->Insert(item);
			this->buckets[bucket].clear();
		}
		return this->zero.top();
	}
	void pop() { this->zero.pop(); --this->count; }
	size_t bytes() const
	{
		size_t bytes = PassengerBenchmarkQueueBytes(this->zero);
		for (const auto &bucket : this->buckets) bytes += bucket.capacity() * sizeof(Item);
		return bytes;
	}
};
