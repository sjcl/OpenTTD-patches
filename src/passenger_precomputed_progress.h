/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */

/** @file passenger_precomputed_progress.h Local, read-only initial construction progress. */
#ifndef PASSENGER_PRECOMPUTED_PROGRESS_H
#define PASSENGER_PRECOMPUTED_PROGRESS_H

#include <optional>

struct PassengerPrecomputedProgress {
	enum class Phase : uint8_t { Disabled, Services, Graph, Restoring, Tables, Complete, MemoryRejected, Invalid };
	Phase phase = Phase::Disabled;
	size_t destinations = 0, stations = 0;
	size_t restored_destinations = 0, retained_stations = 0;
	bool retained_total_known = false;
	bool preparing = false, total_known = false;

	/** Fraction of published destination tables, never elapsed time or replay Work. */
	std::optional<uint> Percent() const
	{
		if (!this->total_known || this->stations == 0 || this->phase == Phase::MemoryRejected || this->phase == Phase::Invalid) return {};
		return static_cast<uint>(uint64_t{this->destinations} * 100 / this->stations);
	}
};

/** GUI lifecycle, explicitly armed by game initialization, never by a DB rebuild. */
class PassengerInitialBuildProgress {
	bool pending = false, active = false, dismissed = false;
public:
	void Reset(bool arm) { this->pending = arm; this->active = this->dismissed = false; }
	void Observe(bool preparing)
	{
		if (!this->pending) return;
		this->active = preparing;
		if (!preparing) this->pending = false;
	}
	void Dismiss() { this->dismissed = true; }
	bool Active() const { return this->active; }
	bool Pending() const { return this->pending; }
	bool ShowWindow() const { return this->active && !this->dismissed; }
};

#endif /* PASSENGER_PRECOMPUTED_PROGRESS_H */
