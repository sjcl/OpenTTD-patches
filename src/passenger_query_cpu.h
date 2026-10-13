/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
/** @file passenger_query_cpu.h Opt-in OS thread CPU observation; never a routing input. */
#ifndef PASSENGER_QUERY_CPU_H
#define PASSENGER_QUERY_CPU_H

#include <cstdint>
#include <optional>
#if defined(WITH_PASSENGER_BENCHMARK) && defined(_WIN32)
#include <windows.h>
#endif

class PassengerQueryCpuTimer {
	static std::optional<uint64_t> Read()
	{
#if defined(WITH_PASSENGER_BENCHMARK) && defined(_WIN32)
		FILETIME creation, exit, kernel, user;
		if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return {};
		uint64_t ticks = (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
			(uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
		return ticks * 100;
#else
		return {};
#endif
	}
	std::optional<uint64_t> start = Read();
public:
	std::optional<uint64_t> Elapsed() const
	{
		auto end = Read();
		if (!this->start || !end || *end < *this->start) return {};
		return *end - *this->start;
	}
};

#endif /* PASSENGER_QUERY_CPU_H */
