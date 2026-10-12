/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
#ifndef PASSENGER_INDEX_ALLOCATIONS_H
#define PASSENGER_INDEX_ALLOCATIONS_H

/** Opt-in benchmark counters; usable allocator bytes include allocator rounding. */
struct PassengerIndexAllocations {
	size_t calls = 0, bytes = 0, live = 0, peak = 0;
};
#ifdef WITH_SERVICE_INDEX_ALLOCATIONS
extern thread_local bool _passenger_index_tracking;
extern thread_local PassengerIndexAllocations _passenger_index_allocations;
#else
static thread_local bool _passenger_index_tracking = false;
static thread_local PassengerIndexAllocations _passenger_index_allocations;
#endif

#endif
