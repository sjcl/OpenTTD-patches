/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
/** @file passenger_index_allocations.cpp Optional MSVC benchmark allocator, never linked into the game. */
#include "../stdafx.h"
#include "passenger_index_allocations.h"
#include <malloc.h>
#include <new>

thread_local bool _passenger_index_tracking = false;
thread_local PassengerIndexAllocations _passenger_index_allocations;

void *operator new(size_t size)
{
	void *p = std::malloc(std::max<size_t>(size, 1));
	if (p == nullptr) throw std::bad_alloc();
	if (_passenger_index_tracking) {
		const size_t bytes = _msize(p);
		++_passenger_index_allocations.calls;
		_passenger_index_allocations.bytes += bytes;
		_passenger_index_allocations.live += bytes;
		_passenger_index_allocations.peak = std::max(_passenger_index_allocations.peak, _passenger_index_allocations.live);
	}
	return p;
}

void operator delete(void *p) noexcept
{
	if (p != nullptr && _passenger_index_tracking) _passenger_index_allocations.live -= _msize(p);
	std::free(p);
}
void *operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, size_t) noexcept { ::operator delete(p); }
