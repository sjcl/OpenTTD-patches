/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
#ifndef THREAD_CPU_TIMER_HPP
#define THREAD_CPU_TIMER_HPP

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/** Local diagnostic nanoseconds of CPU on the calling thread, excluding sleep.
 * Zero on platforms without a thread CPU clock; never used for simulation decisions. */
inline uint64_t GetCurrentThreadCPUTime()
{
#ifdef _WIN32
	FILETIME created{}, exited{}, kernel{}, user{};
	if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
	auto value = [](FILETIME time) { return (uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime; };
	return (value(kernel) + value(user)) * 100;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
	timespec time{};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) return 0;
	return uint64_t(time.tv_sec) * 1000000000 + time.tv_nsec;
#else
	return 0;
#endif
}
#endif
