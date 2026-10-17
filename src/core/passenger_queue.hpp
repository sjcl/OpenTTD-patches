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
	 * There are two tables per cargo in every database, including empty ones.
	 * A ring of one-cost buckets covers (last, last + RING); later costs wait in an
	 * exact heap. Every item moves at most once more, into the zero heap of its cost. */
	struct Radix {
		static constexpr uint64_t RING = 1024;
		Heap<Later> zero;
		std::vector<std::vector<Item>> ring;
		std::array<uint64_t, RING / 64> occupied{};
		Heap<std::greater<Item>> far_heap;
		uint64_t last = 0, retained_limit = 0;
		size_t count = 0, bucket_bytes = 0;
		Radix() : ring(RING) {}
		void Insert(const Item &item)
		{
			uint64_t cost = static_cast<uint64_t>(std::get<0>(item));
			assert(std::get<0>(item) >= 0 && cost >= this->last);
			if (cost == this->last) this->zero.push(item);
			else if (cost - this->last < RING) {
				const size_t slot = cost & (RING - 1);
				auto &bucket = this->ring[slot];
				size_t capacity = bucket.capacity(); bucket.push_back(item);
				this->bucket_bytes += (bucket.capacity() - capacity) * sizeof(Item);
				this->occupied[slot / 64] |= uint64_t{1} << (slot % 64);
			} else this->far_heap.push(item);
		}
		/** Distance (1..RING-1) from last to the nearest occupied ring slot, or 0 if none.
		 * The slot of last itself is always empty: its cost is in the zero heap. */
		uint64_t NextOccupied() const
		{
			const size_t origin = this->last & (RING - 1);
			size_t slot = (origin + 1) & (RING - 1);
			for (size_t scanned = 0; scanned < RING;) {
				const uint64_t word = this->occupied[slot / 64] >> (slot % 64);
				if (word != 0) {
					const uint64_t distance = (slot + std::countr_zero(word) - origin) & (RING - 1);
					assert(distance != 0);
					return distance;
				}
				scanned += 64 - slot % 64;
				slot = (slot - slot % 64 + 64) & (RING - 1);
			}
			return 0;
		}
		const Item &Top()
		{
			if (this->zero.empty()) {
				const uint64_t distance = this->NextOccupied();
				uint64_t next = distance != 0 ? this->last + distance : UINT64_MAX;
				if (!this->far_heap.empty()) next = std::min<uint64_t>(next, static_cast<uint64_t>(std::get<0>(this->far_heap.top())));
				this->last = next;
				const size_t slot = next & (RING - 1);
				auto &bucket = this->ring[slot];
				if (!bucket.empty() && static_cast<uint64_t>(std::get<0>(bucket.front())) == next) {
					for (const auto &item : bucket) this->zero.push(item);
					bucket.clear();
					this->occupied[slot / 64] &= ~(uint64_t{1} << (slot % 64));
					/* Bound empty-bucket retention across destinations. The frozen
					 * edge/state bound, rather than allocator residency, sets this limit.
					 * Live entries and comparison/publication order never depend on it. */
					if (this->bucket_bytes > this->retained_limit) {
						this->bucket_bytes -= bucket.capacity() * sizeof(Item);
						std::vector<Item>().swap(bucket);
					}
				}
				while (!this->far_heap.empty() && static_cast<uint64_t>(std::get<0>(this->far_heap.top())) == next) { this->zero.push(this->far_heap.top()); this->far_heap.pop(); }
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
		return this->legacy.Bytes() + (this->radix ? sizeof(Radix) + Radix::RING * sizeof(std::vector<Item>) + this->radix->zero.Bytes() + this->radix->far_heap.Bytes() + this->radix->bucket_bytes : 0);
	}
};

#endif
