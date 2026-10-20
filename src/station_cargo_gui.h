/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */

/** @file station_cargo_gui.h Display-only waiting cargo hierarchy and passenger service summaries. */
#ifndef STATION_CARGO_GUI_H
#define STATION_CARGO_GUI_H

#include "cargo_type.h"
#include "station_type.h"
#include "group_type.h"
#include "sortlist_type.h"
#include "passenger_journey_type.h"
#include <map>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <variant>

struct CargoPacket;

enum class StationPassengerServiceState : uint8_t { Pending, Partial, Complete, Unavailable };
enum class StationPassengerServiceLabel : uint8_t { Undecided, Checking, Ungrouped, Updating, Reserved };

/** Per-window diagnostics only; neither saved nor used by simulation admission. */
struct StationPassengerServiceDiagnostics {
	uint64_t cache_hits = 0, work = 0, work_exhausted = 0, candidates = 0, vehicles = 0;
	uint64_t no_candidates = 0, stale_policy = 0, stale_service = 0, no_shared_vehicle = 0;
	uint64_t ungrouped = 0, invalid_vehicle = 0, unusable_vehicle = 0, refreshes = 0, evictions = 0;
	uint64_t search_releases = 0, capacity_regressions = 0;
	uint64_t result_evictions = 0, capacity_waits = 0, periodic_revalidations = 0, policy_revalidations = 0;
	uint64_t representative_lost = 0, display_regressions = 0, resolved = 0, validation_work = 0;
	uint64_t generation_reuses = 0;
	size_t results = 0, searches = 0, result_capacity = 0, estimated_bytes = 0, waiting_work = 0;
	size_t pending = 0, partial = 0, complete = 0, unavailable = 0, entries = 0;
};

/** One passenger belongs to exactly one named, ambiguous, or unknown service summary. */
struct StationPassengerService {
	GroupID group = GroupID::Invalid();
	bool multiple = false;
	StationID transfer = StationID::Invalid(); ///< Optional concrete Journey hint.
	StationPassengerServiceLabel label = StationPassengerServiceLabel::Undecided;
	auto operator<=>(const StationPassengerService &) const = default;
};

using StationCargoKey = std::variant<CargoType, StationID, StationPassengerService>;

/** Ambiguity may settle without changing the expansion identity of a named service. */
inline StationPassengerService PassengerServiceExpansionKey(StationPassengerService service)
{
	if (service.group != GroupID::Invalid()) { service.multiple = false; service.transfer = StationID::Invalid(); service.label = StationPassengerServiceLabel::Undecided; }
	return service;
}

enum class StationCargoGroupingMode : uint8_t { Conventional, Passengers, Mixed };
inline StationCargoGroupingMode GetStationCargoGroupingMode(bool planned, bool passengers, bool other)
{
	return planned || !passengers ? StationCargoGroupingMode::Conventional : other ? StationCargoGroupingMode::Mixed : StationCargoGroupingMode::Passengers;
}

/** Persistent, bounded, incremental GUI lookup. No live game pointers or names are retained. */
class StationPassengerServiceCache {
public:
	explicit StationPassengerServiceCache(size_t result_budget = 16 * 1024 * 1024);
	~StationPassengerServiceCache();
	StationPassengerService Resolve(CargoType cargo, const PassengerJourneyExecution &ride);
	StationPassengerService Resolve(CargoType cargo, const CargoPacket &packet, StationID station);
	/** Refill the independent GUI budget at 16,384 Work/second and round-robin pending tasks. */
	bool Advance(uint delta_ms, size_t work_limit = 16384);
	void Clear(); ///< Explicit reset, never called by the periodic GUI update.
	void BeginRefresh(); ///< Mark results referenced by the current window refresh.
	void Invalidate(OrderListID service = OrderListID::Invalid()); ///< GUI-only notification; never invalidates passenger routing.
	StationPassengerServiceDiagnostics GetDiagnostics() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

StationID GetStationCargoVia(CargoType cargo, const CargoPacket &packet, StationID queue_key, StationID station = StationID::Invalid());

/** Existing cargo/from/via/to columns, independently ordered by the station window. */
enum StationCargoGrouping : uint8_t { GR_SOURCE, GR_NEXT, GR_DESTINATION, GR_CARGO, GR_SERVICE };

class CargoDataEntry;

/** Order of sorting cargo entries in the station view. Values also belong to the saved GUI preference. */
enum SortOrder : uint8_t { SO_DESCENDING, SO_ASCENDING };

/** Ways of sorting cargo in the UI. */
enum class CargoSortType : uint8_t {
	AsGrouping,    ///< by the same principle the entries are being grouped
	Count,         ///< by amount of cargo
	StationString, ///< by station name
	StationID,     ///< by station id
	CargoType,     ///< by cargo type
};

class CargoSorter {
public:
	using is_transparent = void;
	CargoSorter(CargoSortType t = CargoSortType::StationID, SortOrder o = SO_ASCENDING) : type(t), order(o) {}
	CargoSortType GetSortType() {return this->type;}
	bool operator()(const CargoDataEntry &cd1, const CargoDataEntry &cd2) const;
	bool operator()(const CargoDataEntry &cd1, const std::unique_ptr<CargoDataEntry> &cd2) const { return this->operator()(cd1, *cd2); }
	bool operator()(const std::unique_ptr<CargoDataEntry> &cd1, const CargoDataEntry &cd2) const { return this->operator()(*cd1, cd2); }
	bool operator()(const std::unique_ptr<CargoDataEntry> &cd1, const std::unique_ptr<CargoDataEntry> &cd2) const { return this->operator()(*cd1, *cd2); }

private:
	CargoSortType type;
	SortOrder order;

	template <class Tid>
	bool SortId(Tid st1, Tid st2) const;
	bool SortCount(const CargoDataEntry &cd1, const CargoDataEntry &cd2) const;
	bool SortStation(StationID st1, StationID st2) const;
};

typedef std::set<std::unique_ptr<CargoDataEntry>, CargoSorter> CargoDataSet;

/**
 * A cargo data entry representing one possible row in the station view window's
 * top part. Cargo data entries form a tree where each entry can have several
 * children. Parents keep track of the sums of their children's cargo counts.
 */
class CargoDataEntry {
public:
	CargoDataEntry();
	~CargoDataEntry();

	/**
	 * Insert a new child or retrieve an existing child using a station ID as ID.
	 * @param station ID of the station for which an entry shall be created or retrieved
	 * @return a child entry associated with the given station.
	 */
	CargoDataEntry &InsertOrRetrieve(StationID station)
	{
		return this->InsertOrRetrieve<StationID>(station);
	}

	/**
	 * Insert a new child or retrieve an existing child using a cargo type as ID.
	 * @param cargo type of the cargo for which an entry shall be created or retrieved
	 * @return a child entry associated with the given cargo.
	 */
	CargoDataEntry &InsertOrRetrieve(CargoType cargo)
	{
		return this->InsertOrRetrieve<CargoType>(cargo);
	}

	void Update(uint count);

	/**
	 * Remove a child associated with the given station.
	 * @param station ID of the station for which the child should be removed.
	 */
	void Remove(StationID station)
	{
		CargoDataEntry t(station);
		this->Remove(t);
	}

	/**
	 * Remove a child associated with the given cargo.
	 * @param cargo type of the cargo for which the child should be removed.
	 */
	void Remove(CargoType cargo)
	{
		CargoDataEntry t(cargo);
		this->Remove(t);
	}

	/**
	 * Retrieve a child for the given station. Return nullptr if it doesn't exist.
	 * @param station ID of the station the child we're looking for is associated with.
	 * @return a child entry for the given station or nullptr.
	 */
	CargoDataEntry *Retrieve(StationID station) const
	{
		CargoDataEntry t(station);
		return this->Retrieve(this->children->find(t));
	}

	/**
	 * Retrieve a child for the given cargo. Return nullptr if it doesn't exist.
	 * @param cargo type of the cargo the child we're looking for is associated with.
	 * @return a child entry for the given cargo or nullptr.
	 */
	CargoDataEntry *Retrieve(CargoType cargo) const
	{
		CargoDataEntry t(cargo);
		return this->Retrieve(this->children->find(t));
	}

	CargoDataEntry *Retrieve(StationPassengerService service) const
	{
		CargoDataEntry key(service);
		return this->Retrieve(this->children->find(key));
	}
	void Remove(StationPassengerService service) { CargoDataEntry key(service); this->Remove(key); }

	CargoDataEntry &InsertOrRetrieve(const StationCargoKey &key) { return std::visit([&](auto value) -> CargoDataEntry & { return this->InsertOrRetrieve(value); }, key); }
	CargoDataEntry *Retrieve(const StationCargoKey &key) const { return std::visit([&](auto value) { return this->Retrieve(value); }, key); }
	void Remove(const StationCargoKey &key) { std::visit([&](auto value) { this->Remove(value); }, key); }
	StationCargoKey GetKey() const
	{
		if (this->service) return *this->service;
		return this->cargo != INVALID_CARGO ? StationCargoKey{this->cargo} : StationCargoKey{this->station};
	}

	void Resort(CargoSortType type, SortOrder order);

	/**
	 * Get the station ID for this entry.
	 * @return The station this cargo is at.
	 */
	StationID GetStation() const { return this->station; }

	/**
	 * Get the cargo type for this entry.
	 * @return The type of cargo.
	 */
	CargoType GetCargo() const { return this->cargo; }

	/**
	 * Get the cargo count for this entry.
	 * @return The amount of cargo.
	 */
	uint GetCount() const { return this->count; }

	/**
	 * Get the parent entry for this entry.
	 * @return The parent entry, can be \c nullptr.
	 */
	CargoDataEntry *GetParent() const { return this->parent; }

	/**
	 * Get the number of children for this entry.
	 * @return The number of children.
	 */
	uint GetNumChildren() const { return this->num_children; }

	/** Service summaries are ordinary hierarchy rows with their own typed key. */
	bool IsService() const { return this->service.has_value(); }
	StationPassengerService GetService() const { return *this->service; }
	CargoDataEntry &InsertOrRetrieve(StationPassengerService service) { return this->InsertOrRetrieve<StationPassengerService>(service); }

	/**
	 * Get an iterator pointing to the begin of the set of children.
	 * @return Iterator at the begin of our children.
	 */
	CargoDataSet::iterator Begin() const { return this->children->begin(); }

	/**
	 * Get an iterator pointing to the end of the set of children.
	 * @return Iterator at the end of our children.
	 */
	CargoDataSet::iterator End() const { return this->children->end(); }

	/**
	 * Has this entry transfers.
	 * @return \c true iff the transfers state has been set.
	 */
	bool HasTransfers() const { return this->transfers; }

	/**
	 * Set the transfers state.
	 * @param value The new transfers state.
	 */
	void SetTransfers(bool value) { this->transfers = value; }

	void Clear();

	CargoDataEntry(StationID station, uint count, CargoDataEntry *parent);
	CargoDataEntry(CargoType cargo, uint count, CargoDataEntry *parent);
	CargoDataEntry(StationID station);
	CargoDataEntry(CargoType cargo);
	CargoDataEntry(StationPassengerService service);
	CargoDataEntry(StationPassengerService service, uint count, CargoDataEntry *parent);

private:
	CargoDataEntry *Retrieve(CargoDataSet::iterator i) const;

	template <class Tid>
	CargoDataEntry &InsertOrRetrieve(Tid s);

	void Remove(CargoDataEntry &entry);
	void IncrementSize();

	CargoDataEntry *parent;   ///< the parent of this entry.
	StationID station = StationID::Invalid(); ///< Station key; never used for a service ID.
	CargoType cargo = INVALID_CARGO;
	bool transfers = false;
	std::optional<StationPassengerService> service;

	uint num_children;        ///< the number of subentries belonging to this entry.
	uint count;               ///< sum of counts of all children or amount of cargo for this entry.
	std::unique_ptr<CargoDataSet> children;   ///< the children of this entry.
};
/** Add conventional CargoDist cargo once, returning its visible via row if present. */
CargoDataEntry *ShowStationCargo(CargoDataEntry &root, const CargoDataEntry &expanded, std::span<const StationCargoGrouping, 4> groupings,
		StationID station, bool auto_distributed, CargoType cargo, StationID source, StationID next, StationID dest, uint count);

/** Waiting passengers always use service/destination/source, independently of cargo grouping preferences. */
CargoDataEntry *ShowStationWaitingCargo(CargoDataEntry &root, const CargoDataEntry &expanded, std::span<const StationCargoGrouping, 4> groupings,
		StationID station, bool auto_distributed, CargoType cargo, const CargoPacket &packet, StationID next, StationID dest, uint count,
		StationPassengerServiceCache &services);

#endif /* STATION_CARGO_GUI_H */
