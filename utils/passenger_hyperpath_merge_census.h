/* Measurement only: no product translation unit includes this header. */
#pragma once
#include <map>
#include <vector>
#include <fstream>

namespace SaveIntegrity {
inline std::vector<uint64_t> (*neutral_fields)(const CargoPacket &) = nullptr;

/** Minimum packet counts for the same endpoint people with splitting/repacking
 * allowed, retaining versus ignoring only their failure state. This read-only
 * counterfactual does not claim that live queues eagerly repack old cargo. */
inline void MergeCensus()
{
	uint64_t actual = 0, failed_packets = 0, failed_units = 0, strict = 0, neutral = 0;
	auto bins = [](uint64_t units) { return (units + CargoPacket::MAX_COUNT - 1) / CargoPacket::MAX_COUNT; };
	struct Bins { uint64_t neutral = 0; std::map<PassengerBoardingFailure, uint64_t> strict; };
	for (const Station *station : Station::Iterate()) for (CargoType cargo : EnumRange(NUM_CARGO)) {
		if (!IsCargoInClass(cargo, CargoClass::Passengers) || station->goods[cargo].data == nullptr) continue;
		for (const auto &[via, packets] : *station->goods[cargo].data->cargo.Packets()) {
			std::map<std::vector<uint64_t>, Bins> groups;
			for (const CargoPacket *packet : packets) {
				++actual;
				const auto &failure = packet->GetPassengerBoardingFailure();
				if (failure.Present()) { ++failed_packets; failed_units += packet->Count(); }
				auto &group = groups[neutral_fields(*packet)];
				group.strict[failure] += packet->Count(); group.neutral += packet->Count();
			}
			for (const auto &[key, group] : groups) {
				neutral += bins(group.neutral);
				for (const auto &[failure, units] : group.strict) strict += bins(units);
			}
		}
	}
	std::ofstream out("hyperpath-merge-census.json");
	out << "{\"station_packets\":" << actual << ",\"failure_packets\":" << failed_packets << ",\"failure_passengers\":" << failed_units
			<< ",\"strict_repacked\":" << strict << ",\"ignore_failure_repacked\":" << neutral << ",\"additional_failure_bins\":" << strict - neutral << ",\"repack_allows_splits\":true}\n";
}
}
