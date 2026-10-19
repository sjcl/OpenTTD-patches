/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_aircraft_cost.h Immutable aircraft preference costs, in state ticks. */
#ifndef PASSENGER_AIRCRAFT_COST_H
#define PASSENGER_AIRCRAFT_COST_H

#include "date_func.h"
#include "settings_type.h"

struct PassengerAircraftCost {
	uint16_t scale = 100;
	uint32_t addition = 0;
	/** Capture on the simulation thread. Workers use only the returned values. */
	static PassengerAircraftCost Current()
	{
		return {_settings_game.linkgraph.aircraft_link_scale,
				uint32_t{_settings_game.linkgraph.aircraft_link_add} * DAY_TICKS * DayLengthFactor()};
	}
	bool IsDefault() const { return this->scale == 100 && this->addition == 0; }
	int64_t Cost(int64_t movement) const
	{
		assert(movement >= 0);
		/* Divide before multiplying; the remainder product fits even at UINT16_MAX.
		 * Round each adjacent-stop segment once, including through rides. */
		int64_t fraction = movement % 100 * this->scale / 100 + this->addition;
		int64_t whole = movement / 100;
		return this->scale != 0 && whole > (INT64_MAX - fraction) / this->scale ? INT64_MAX : whole * this->scale + fraction;
	}
	int64_t Adjustment(int64_t movement) const { return this->Cost(movement) - movement; }
	auto operator<=>(const PassengerAircraftCost &) const = default;
};

#endif
