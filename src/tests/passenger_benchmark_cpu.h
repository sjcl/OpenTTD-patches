/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
#ifndef TESTS_PASSENGER_BENCHMARK_CPU_H
#define TESTS_PASSENGER_BENCHMARK_CPU_H
#ifdef _WIN32
#include <windows.h>
#elif defined(CLOCK_THREAD_CPUTIME_ID) || defined(__linux__)
#include <time.h>
#ifdef __linux__
#include <pthread.h>
#endif
#endif
#ifdef _WIN32
using PassengerBenchmarkThread = HANDLE;
inline PassengerBenchmarkThread PassengerBenchmarkCaptureThread() { return OpenThread(THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId()); }
inline void PassengerBenchmarkReleaseThread(PassengerBenchmarkThread thread) { CloseHandle(thread); }
inline uint64_t PassengerBenchmarkCPU(PassengerBenchmarkThread thread)
{
	FILETIME created{}, exited{}, kernel{}, user{};
	if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return 0;
	auto value = [](FILETIME time) { return (uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime; };
	return (value(kernel) + value(user)) * 100;
}
#elif defined(__linux__)
using PassengerBenchmarkThread = pthread_t;
inline PassengerBenchmarkThread PassengerBenchmarkCaptureThread() { return pthread_self(); }
inline void PassengerBenchmarkReleaseThread(PassengerBenchmarkThread) {}
inline uint64_t PassengerBenchmarkCPU(PassengerBenchmarkThread thread)
{
	clockid_t clock; timespec time{};
	if (pthread_getcpuclockid(thread, &clock) != 0 || clock_gettime(clock, &time) != 0) return 0;
	return uint64_t(time.tv_sec) * 1000000000 + time.tv_nsec;
}
#else
using PassengerBenchmarkThread = uint64_t;
inline PassengerBenchmarkThread PassengerBenchmarkCaptureThread() { return 0; }
inline void PassengerBenchmarkReleaseThread(PassengerBenchmarkThread) {}
inline uint64_t PassengerBenchmarkCPU(PassengerBenchmarkThread) { return 0; }
#endif
inline uint64_t PassengerBenchmarkThreadCPU()
{
#ifdef _WIN32
	return PassengerBenchmarkCPU(GetCurrentThread());
#elif defined(CLOCK_THREAD_CPUTIME_ID)
	timespec time{}; if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) return 0;
	return uint64_t(time.tv_sec) * 1000000000 + time.tv_nsec;
#else
	return 0;
#endif
}
#endif
