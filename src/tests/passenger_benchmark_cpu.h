/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
#ifndef TESTS_PASSENGER_BENCHMARK_CPU_H
#define TESTS_PASSENGER_BENCHMARK_CPU_H
#ifdef _WIN32
#include <windows.h>
#elif defined(CLOCK_THREAD_CPUTIME_ID) || defined(__linux__)
#include <time.h>
#endif
inline uint64_t PassengerBenchmarkThreadCPU()
{
#ifdef _WIN32
	FILETIME created{}, exited{}, kernel{}, user{};
	if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
	auto value = [](FILETIME time) { return (uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime; };
	return (value(kernel) + value(user)) * 100;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
	timespec time{}; if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) return 0;
	return uint64_t(time.tv_sec) * 1000000000 + time.tv_nsec;
#else
	return 0;
#endif
}
#endif
