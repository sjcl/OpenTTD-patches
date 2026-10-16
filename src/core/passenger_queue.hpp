/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
#ifndef PASSENGER_QUEUE_HPP
#define PASSENGER_QUEUE_HPP

#include <array>
#include <bit>
#include <queue>
#include <memory>
#include <tuple>
#include <vector>

/** Monotone cost queue with exact (cost, legs, original state rank) ordering.
 * Only cost is monotone: zero-cost edges may introduce a smaller state or leg
 * count after a pop. Such entries go into a secondary binary heap immediately.
 * Legacy mode retains the original lazy binary heap and its work sequence. */
template <typename Item> class PassengerDestinationQueue {
	template <typename> friend struct PassengerDestinationQueueInspector;
	template <typename Compare> struct Heap : std::priority_queue<Item, std::vector<Item>, Compare> {
		template <typename> friend struct PassengerDestinationQueueInspector;
		size_t Bytes() const { return this->c.capacity() * sizeof(Item); }
	};
	struct Later {
		bool operator()(const Item &a, const Item &b) const { return std::tie(std::get<1>(a), std::get<2>(a)) > std::tie(std::get<1>(b), std::get<2>(b)); }
	};
	/** Allocate the bucket headers only for active optimized cargo tables.
	 * There are two tables per cargo in every database, including empty ones. */
	struct Radix {
		Heap<Later> zero;
		std::array<std::vector<Item>, 65> buckets;
		uint64_t last = 0, retained_limit = 0;
		size_t count = 0, bucket_bytes = 0;
		void Insert(const Item &item)
		{
			uint64_t cost = static_cast<uint64_t>(std::get<0>(item));
			assert(std::get<0>(item) >= 0 && cost >= this->last);
			if (cost == this->last) this->zero.push(item);
			else {
				auto &bucket = this->buckets[std::bit_width(cost ^ this->last)];
				size_t capacity = bucket.capacity(); bucket.push_back(item);
				this->bucket_bytes += (bucket.capacity() - capacity) * sizeof(Item);
			}
		}
		const Item &Top()
		{
			if (this->zero.empty()) {
				size_t bucket = 1;
				while (this->buckets[bucket].empty()) ++bucket;
				this->last = UINT64_MAX;
				for (const auto &item : this->buckets[bucket]) this->last = std::min<uint64_t>(this->last, std::get<0>(item));
				/* The highest differing bit disappears, so every item moves to a
				 * lower bucket and cannot invalidate this iteration. */
				for (const auto &item : this->buckets[bucket]) this->Insert(item);
				this->buckets[bucket].clear();
				/* Bound empty-bucket retention across destinations. The frozen
				 * edge/state bound, rather than allocator residency, sets this limit.
				 * Live entries and comparison/publication order never depend on it. */
				if (this->bucket_bytes > this->retained_limit) {
					this->bucket_bytes -= this->buckets[bucket].capacity() * sizeof(Item);
					std::vector<Item>().swap(this->buckets[bucket]);
				}
			}
			return this->zero.top();
		}
	};
	Heap<std::greater<Item>> legacy;
	std::unique_ptr<Radix> radix;
public:
	void SetOptimized(bool optimized) { if (optimized) this->radix = std::make_unique<Radix>(); else this->radix.reset(); }
	void SetCapacityLimit(uint64_t items) { if (this->radix) this->radix->retained_limit = items * 2 * sizeof(Item); }
	bool empty() const { return this->radix ? this->radix->count == 0 : this->legacy.empty(); }
	size_t size() const { return this->radix ? this->radix->count : this->legacy.size(); }
	void Reset() { assert(this->empty()); if (this->radix) this->radix->last = 0; }
	template <typename... Args> void emplace(Args... args)
	{
		if (!this->radix) { this->legacy.emplace(args...); return; }
		++this->radix->count; this->radix->Insert(Item(args...));
	}
	const Item &top() { return this->radix ? this->radix->Top() : this->legacy.top(); }
	void pop()
	{
		if (!this->radix) { this->legacy.pop(); return; }
		this->radix->zero.pop(); --this->radix->count;
	}
	size_t AllocatedBytes() const
	{
		return this->legacy.Bytes() + (this->radix ? sizeof(Radix) + this->radix->zero.Bytes() + this->radix->bucket_bytes : 0);
	}
};

#endif
