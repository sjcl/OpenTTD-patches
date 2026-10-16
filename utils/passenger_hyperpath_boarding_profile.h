/* Measurement-only hooks. Never include this header in a product translation unit. */
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <vector>

namespace PassengerBoardingProfile {
enum Stage : size_t { Enumeration, Predicate, Ride, EnumerationRide, Visit, Index, Forget, Refresh, Schedule, MergeHash, Merge, Shift, Ensure, AddEntry, RemoveEntry, Resolve, NoLoad, Count };
inline constexpr std::array<const char *, Count> names = {"enumeration", "predicate", "ride", "enumeration_ride", "visit", "index", "forget", "refresh", "schedule", "merge_hash", "merge", "shift", "ensure", "add_entry", "remove_entry", "resolve", "no_load"};
struct Counter { uint64_t calls = 0, nanoseconds = 0, stops = 0; };
inline std::array<Counter, Count> counters{};
inline uint64_t visits = 0, groups = 0;
inline size_t enumeration_depth = 0;
struct Sample { int64_t tick; uint64_t visits, groups; std::array<Counter, Count> counters; };
inline std::vector<Sample> samples;

struct Scope {
	Stage stage;
	bool enumerating;
	std::chrono::steady_clock::time_point started;
	explicit Scope(Stage stage) : stage(stage), enumerating(enumeration_depth != 0), started(std::chrono::steady_clock::now())
	{
		++counters[stage].calls;
		if (stage == Enumeration) ++enumeration_depth;
		if (stage == Ride && enumerating) ++counters[EnumerationRide].calls;
	}
	~Scope()
	{
		auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
		counters[stage].nanoseconds += elapsed;
		if (stage == Enumeration) --enumeration_depth;
		if (stage == Ride && enumerating) counters[EnumerationRide].nanoseconds += elapsed;
	}
};
inline void Stop()
{
	++counters[Ride].stops;
	if (enumeration_depth != 0) ++counters[EnumerationRide].stops;
}
inline void Reset() { counters = {}; visits = groups = 0; samples.clear(); }
inline void Snapshot(int64_t tick) { samples.push_back({tick, visits, groups, counters}); }
inline void Dump()
{
	std::ofstream out("boarding-profile.csv");
	out << "tick,visits,groups";
	for (const auto *name : names) out << ',' << name << "_calls," << name << "_ns," << name << "_stops";
	out << '\n';
	for (const auto &sample : samples) {
		out << sample.tick << ',' << sample.visits << ',' << sample.groups;
		for (const auto &counter : sample.counters) out << ',' << counter.calls << ',' << counter.nanoseconds << ',' << counter.stops;
		out << '\n';
	}
}
}
