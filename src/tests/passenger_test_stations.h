/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
#ifndef PASSENGER_TEST_STATIONS_H
#define PASSENGER_TEST_STATIONS_H

#include "../station_base.h"
#include "../core/backup_type.hpp"

/** Forecast fixtures use real station facilities, even when engines/windows are mocked. */
static void EnsurePassengerTestStations(std::span<const Order> orders)
{
	for (const Order &order : orders) {
		if (!order.IsType(OT_GOTO_STATION)) continue;
		StationID id = order.GetDestination().ToStationID();
		Station *station = Station::GetIfValid(id);
		if (station == nullptr) station = Station::CreateAtIndex(id, TileIndex{0});
		station->owner = OWNER_NONE;
		station->facilities.Set(StationFacility::Train);
	}
}

struct PassengerTestStations {
	AutoRestoreBackup<CargoClasses> classes{CargoSpec::Get(CargoType{0})->classes, CargoClasses{CargoClass::Passengers}};
	PassengerTestStations() { REQUIRE(Station::GetNumItems() == 0); AllocateMap(256, 256); }
	~PassengerTestStations() { _station_pool.CleanPool(); }
};

#endif
