/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cargopacket.h Base class for cargo packets. */

#ifndef CARGOPACKET_H
#define CARGOPACKET_H

#include "core/pool_type.hpp"
#include "core/geometry_type.hpp"
#include "money_type.h"
#include "station_type.h"
#include "order_type.h"
#include "passenger_journey_type.h"
#include "cargo_type.h"
#include "source_type.h"
#include "vehicle_type.h"
#include "company_type.h"
#include "map_func.h"
#include "core/multimap.hpp"
#include "sl/saveload_common_type.h"
#include "3rdparty/cpp-ring-buffer/ring_buffer.hpp"
#include "3rdparty/cpp-btree/btree_map.h"
#include <functional>
#include <list>
#include <set>
#include <unordered_map>
#include <tuple>

struct CargoPayment;
struct PassengerWaitingEventDiagnostic;

/** Unique identifier for a single cargo packet. */
struct CargoPacketIDTag : public PoolIDTraits<uint32_t, 0xFFF000, 0xFFFFFF> {};
using CargoPacketID = PoolID<CargoPacketIDTag>;
struct CargoPacket;
void PassengerWaitingPacketChanged(const CargoPacket &packet);
void PassengerWaitingRoutingChanged(CargoPacket &packet);
void InvalidatePassengerWaitingRouting();
void ShiftPassengerWaitingEventTicks(StateTicksDelta delta);
struct GoodsEntry;
void PassengerWaitingFlowsChanged(GoodsEntry &ge, CargoType cargo); ///< The station's CargoDist flows for this cargo changed.

/** Type of the pool for cargo packets for a little over 16 million packets. */
using CargoPacketPool = Pool<CargoPacket, CargoPacketID, 1024, PoolType::Normal, true>;
/** The actual pool with cargo packets. */
extern CargoPacketPool _cargopacket_pool;

struct GoodsEntry; // forward-declare for Stage() and RerouteStalePackets()
class PassengerJourneyShadowContext;
class PassengerRoutingContext;
class PassengerJourneyActiveContext;
class PassengerJourneyLoadingContext;

template <class Tinst, class Tcont> class CargoList;
class StationCargoList; // forward-declare, so we can use it in VehicleCargoList.
extern NamedSaveLoadTable GetCargoPacketDesc();

namespace upstream_sl {
	extern upstream_sl::SaveLoadTable GetCargoPacketDesc();
	class SlVehicleCommon;
	class SlStationGoods;
}

void ClearCargoPacketDeferredPayments();
void ChangeOwnershipOfCargoPacketDeferredPayments(Owner old_owner, Owner new_owner);

/**
 * Container for cargo from the same location and time.
 */
struct CargoPacket : CargoPacketPool::PoolItem<&_cargopacket_pool> {
private:
	uint16_t count = 0; ///< The amount of cargo in this packet.
	uint16_t periods_in_transit = 0; ///< Amount of cargo aging periods this packet has been in transit.

	Money feeder_share = 0; ///< Value of feeder pickup to be paid for on delivery of cargo.

	TileIndex source_xy = INVALID_TILE; ///< The origin of the cargo.
	Coord2D<int32_t> travelled{0, 0}; ///< If cargo is in station: the vector from the unload tile to the source tile. If in vehicle: an intermediate value.

	Source source{Source::Invalid, SourceType::Industry}; ///< Source of the cargo

	uint8_t flags = 0;                              ///< NOSAVE: temporary flags
	StationID first_station = StationID::Invalid(); ///< The station where the cargo came from first.
	StationID next_hop = StationID::Invalid();      ///< Station where the cargo wants to go next.
	StationID passenger_via = StationID::Invalid(); ///< Validated through-ride target (CargoDist or OD fallback).
	StationID passenger_destination = StationID::Invalid(); ///< Final passenger destination; independent of CargoDist hops and commitments.
	PassengerRoutingMode passenger_routing_mode = PassengerRoutingMode::Legacy;
	PassengerJourneyExecution passenger_journey{};
	PassengerBoardingFailure passenger_boarding_failure{};
	StationID passenger_legacy_hop = StationID::Invalid(); ///< Current CargoDist hop, revalidated when a waiting journey expires.
	StationID passenger_retry_station = StationID::Invalid();
	StateTicks passenger_retry_after{}; ///< Deterministic cooldown, shared by split packets.
	StateTicks passenger_interchange_ready{};
	OrderListID passenger_arriving_service = OrderListID::Invalid();
	uint64_t passenger_arriving_incarnation = 0;
	PassengerTransportKind passenger_transport = PassengerTransportKind::None; ///< Last carrier; survives next-intent staging.
	PassengerTransportKind passenger_interchange_transport = PassengerTransportKind::None; ///< Carrier before the next actual boarding.
	bool passenger_schedule_used = false; ///< A physically loaded schedule carrier was observed at an arrival.
	StateTicks passenger_routing_event{}; ///< Saved next waiting event; Invalid means dormant until a generation change.

	/** Cargo packet flag bits in CargoPacket::flags. */
	enum CargoPacketFlags {
		CPF_HAS_DEFERRED_PAYMENT = 0x01, ///< Cargo packet has 1 or more deferred payment(s)
		CPF_IN_VEHICLE           = 0x02, ///< Whether this cargo is in a vehicle or not. Only used when: defined(WITH_FULL_ASSERTS)
	};

	/** The CargoList caches, thus needs to know about it. */
	template <class Tinst, class Tcont> friend class CargoList;
	friend class VehicleCargoList;
	friend class StationCargoList;
	/** We want this to be saved, right? */
	friend NamedSaveLoadTable GetCargoPacketDesc();
	friend upstream_sl::SaveLoadTable upstream_sl::GetCargoPacketDesc();
	friend void Load_CPDP();
public:
	/** Maximum number of items in a single cargo packet. */
	static const uint16_t MAX_COUNT = UINT16_MAX;

	CargoPacket(CargoPacketID index);
	CargoPacket(CargoPacketID index, StationID first_station, uint16_t count, Source source);
	CargoPacket(CargoPacketID index, uint16_t count, uint16_t periods_in_transit, StationID first_station, TileIndex source_xy, Money feeder_share);
	CargoPacket(CargoPacketID index, uint16_t count, Money feeder_share, const CargoPacket &original);
	~CargoPacket();
	static void PreCleanPool();

	CargoPacket *Split(uint new_size);
	[[nodiscard]] bool Merge(CargoPacket *cp);
	void Reduce(uint count);

	/** Get the final passenger destination, or Invalid if no OD has been assigned. */
	StationID GetPassengerDestination() const { return this->passenger_destination; }
	PassengerRoutingMode GetPassengerRoutingMode() const { return this->passenger_routing_mode; }
	const PassengerJourneyExecution &GetPassengerJourney() const { return this->passenger_journey; }
	const PassengerBoardingFailure &GetPassengerBoardingFailure() const { return this->passenger_boarding_failure; }
	void SetPassengerBoardingFailure(PassengerBoardingFailure failure) { this->passenger_boarding_failure = failure; PassengerWaitingPacketChanged(*this); }
	void RecordPassengerCapacityFailure(OrderListID service, StateTicks now, int64_t expected_wait = 0);
	OrderListID GetPassengerFailedService(StateTicks now) const;
	StationID GetPassengerLegacyHop() const { return this->passenger_legacy_hop; }
	void SetPassengerLegacyHop(StationID next) { if (this->passenger_legacy_hop != next) { this->passenger_legacy_hop = next; PassengerWaitingRoutingChanged(*this); } }
	StationID GetPassengerVia() const { return this->passenger_via; }
	StateTicks GetPassengerInterchangeReady() const { return this->passenger_interchange_ready; }
	OrderListID GetPassengerArrivingService() const { return this->passenger_arriving_service; }
	uint64_t GetPassengerArrivingIncarnation() const { return this->passenger_arriving_incarnation; }
	PassengerTransportKind GetPassengerTransport() const { return this->passenger_transport; }
	PassengerTransportKind GetPassengerInterchangeTransport() const { return this->passenger_interchange_transport; }
	bool HasUsedPassengerSchedule() const { return this->passenger_schedule_used; }
	void SetPassengerTransport(PassengerTransportKind kind) { this->passenger_transport = kind; PassengerWaitingPacketChanged(*this); }
	void SetPassengerInterchange(StateTicks ready, OrderListID arriving, uint64_t incarnation = 0) { this->passenger_interchange_ready = ready; this->passenger_arriving_service = arriving; this->passenger_arriving_incarnation = incarnation; this->passenger_interchange_transport = this->passenger_transport; PassengerWaitingRoutingChanged(*this); }
	StateTicks GetPassengerRoutingEvent() const { return this->passenger_routing_event; }
	void SchedulePassengerRouting(StateTicks due) { this->passenger_routing_event = due; PassengerWaitingPacketChanged(*this); }
	void SetPassengerVia(StationID via) { this->passenger_via = via; }
	bool CanRetryPassengerJourney(StationID station, StateTicks now) const
	{
		return (this->passenger_routing_mode != PassengerRoutingMode::Fallback && !this->IsPassengerJourneyDeferred()) ||
				station != this->passenger_retry_station || now >= this->passenger_retry_after;
	}
	/** Budget wait uses existing saved Legacy/retry fields; CargoDist can still transport it. */
	bool IsPassengerJourneyDeferred() const { return this->passenger_routing_mode == PassengerRoutingMode::Legacy && this->passenger_retry_station != StationID::Invalid(); }
	void SetPassengerJourney(PassengerJourneyExecution journey, StationID legacy_hop);
	void FallbackPassengerJourney(StationID station, StateTicks now, StationID legacy_hop = StationID::Invalid());
	void DeferPassengerJourney(StationID station, StateTicks now, StationID legacy_hop = StationID::Invalid());
	void ShiftPassengerJourneyTicks(StateTicksDelta delta);
	bool HasSamePassengerRouting(const CargoPacket &other) const
	{
		return this->passenger_routing_mode == other.passenger_routing_mode && this->passenger_journey == other.passenger_journey && this->passenger_boarding_failure == other.passenger_boarding_failure &&
				this->passenger_legacy_hop == other.passenger_legacy_hop && this->passenger_retry_station == other.passenger_retry_station &&
				this->passenger_retry_after == other.passenger_retry_after && this->passenger_interchange_ready == other.passenger_interchange_ready &&
				this->passenger_arriving_service == other.passenger_arriving_service && this->passenger_arriving_incarnation == other.passenger_arriving_incarnation && this->passenger_transport == other.passenger_transport &&
				this->passenger_interchange_transport == other.passenger_interchange_transport && this->passenger_schedule_used == other.passenger_schedule_used;
	}
	uint64_t StationMergeHash() const;

	/**
	 * Assign an explicit final destination to an unassigned passenger packet.
	 * The caller must supply the actual cargo type of the owning list and assign
	 * OD before appending the packet: CargoPacket does not store its cargo type,
	 * and appending may merge and delete it. Never infer OD from GetVia(), next_hop
	 * or passenger_via. Repeating the same assignment is allowed; changing an
	 * assigned destination is rejected. Station removal clears it internally.
	 * @param cargo Actual cargo type, checked for the passenger class.
	 * @param destination Existing final station, or Invalid for unassigned OD.
	 * @return True if the assignment is valid; otherwise the packet is unchanged.
	 */
	[[nodiscard]] bool SetPassengerDestination(CargoType cargo, StationID destination);

	/**
	 * Sets the station where the packet is supposed to go next.
	 * @param next_hop Next station the packet should go to.
	 */
	void SetNextHop(StationID next_hop)
	{
		this->next_hop = next_hop;
	}

	/**
	 * Update for the cargo being loaded on this tile.
	 *
	 * When a CargoPacket is created, it is moved to a station. But at that
	 * moment in time it is not known yet at which tile the cargo will be
	 * picked up. As this tile is used for payment information, we delay
	 * setting the source_xy till first pickup, getting a better idea where
	 * a cargo started from.
	 *
	 * Further more, we keep track of the amount of tiles the cargo moved
	 * inside a vehicle. This is used in GetDistance() below.
	 *
	 * @param tile Tile the cargo is being picked up from.
	 */
	void UpdateLoadingTile(TileIndex tile)
	{
		if (this->source_xy == INVALID_TILE) {
			this->source_xy = tile;
			this->periods_in_transit = 0;
		}

#ifdef WITH_FULL_ASSERTS
		assert((this->flags & CPF_IN_VEHICLE) == 0);
		this->flags |= CPF_IN_VEHICLE;
#endif /* WITH_FULL_ASSERTS */

		/* We want to calculate the vector from tile-unload to tile-load. As
		 * we currently only know the latter, add it. When we know where we unload,
		 * we subtract is, giving us our vector (unload - load). */
		this->travelled.x += TileX(tile);
		this->travelled.y += TileY(tile);
	}

	/**
	 * Update for the cargo being unloaded on this tile.
	 *
	 * @param tile Tile the cargo is being dropped off at.
	 */
	void UpdateUnloadingTile(TileIndex tile)
	{
#ifdef WITH_FULL_ASSERTS
		assert((this->flags & CPF_IN_VEHICLE) != 0);
		this->flags &= ~CPF_IN_VEHICLE;
#endif /* WITH_FULL_ASSERTS */

		this->travelled.x -= TileX(tile);
		this->travelled.y -= TileY(tile);
		this->passenger_via = StationID::Invalid();
	}

	/**
	 * Adds some feeder share to the packet.
	 * @param new_share Feeder share to be added.
	 */
	void AddFeederShare(Money new_share)
	{
		this->feeder_share += new_share;
	}

	/**
	 * Gets the number of 'items' in this packet.
	 * @return Item count.
	 */
	inline uint16_t Count() const
	{
		return this->count;
	}

	/**
	 * Gets the amount of money already paid to earlier vehicles in
	 * the feeder chain.
	 * @return Feeder share.
	 */
	inline Money GetFeederShare() const
	{
		return this->feeder_share;
	}

	/**
	 * Gets part of the amount of money already paid to earlier vehicles in
	 * the feeder chain.
	 * @param part Amount of cargo to get the share for.
	 * @return Feeder share for the given amount of cargo.
	 */
	inline Money GetFeederShare(uint part) const
	{
		return this->feeder_share * part / static_cast<uint>(this->count);
	}

	void RegisterDeferredCargoPayment(CompanyID cid, VehicleType type, Money payment);
	void PayDeferredPayments();

	/**
	 * Gets the number of cargo aging periods this cargo has been in transit.
	 * By default a period is 2.5 days (CARGO_AGING_TICKS = 185 ticks), however
	 * vehicle NewGRFs can override the length of the cargo aging period. The
	 * value is capped at UINT16_MAX.
	 * @return Length this cargo has been in transit.
	 */
	inline uint16_t GetPeriodsInTransit() const
	{
		return this->periods_in_transit;
	}

	/**
	 * Gets the source of the packet for subsidy purposes.
	 * @return The source.
	 */
	inline Source GetSource() const
	{
		return this->source;
	}

	/**
	 * Gets the ID of the station where the cargo was loaded for the first time.
	 * @return StationID.
	 */
	inline StationID GetFirstStation() const
	{
		return this->first_station;
	}

	/**
	 * Get the current distance the cargo has traveled.
	 *
	 * @param current_tile Current tile of the cargo.
	 * @return uint The distance (in tiles) traveled.
	 */
	inline uint GetDistance(TileIndex current_tile) const
	{
		assert(this->source_xy != INVALID_TILE);
#ifdef WITH_FULL_ASSERTS
		assert((this->flags & CPF_IN_VEHICLE) != 0);
#endif /* WITH_FULL_ASSERTS */

		/* Distance is always requested when the cargo is still inside the
		 * vehicle. So first finish the calculation for travelled to
		 * become a vector. */
		auto local_travelled = travelled;
		local_travelled.x -= TileX(current_tile);
		local_travelled.y -= TileY(current_tile);

		/* Cargo-movement is a vector that indicates how much the cargo has
		 * actually traveled in a vehicle. This is the distance you get paid
		 * for. However, one could construct a route where this vector would
		 * be really long. To not overpay the player, cap out at the distance
		 * between source and destination.
		 *
		 * This way of calculating is to counter people moving cargo for free
		 * and instantly in stations, where you deliver it in one part of the
		 * station and pick it up in another. By using the actual distance
		 * traveled in a vehicle, using this trick doesn't give you more money.
		 *
		 * However, especially in large networks with large transfer station,
		 * etc, one could actually make the route a lot longer. In that case,
		 * use the actual distance between source and destination.
		 */

		uint distance_travelled = abs(local_travelled.x) + abs(local_travelled.y);
		uint distance_source_dest = DistanceManhattan(this->source_xy, current_tile);
		return std::min(distance_travelled, distance_source_dest);
	}

	/**
	 * Gets the ID of station the cargo wants to go next.
	 * @return Next station for this packets.
	 */
	inline StationID GetNextHop() const
	{
		return this->next_hop;
	}

	static void InvalidateAllFrom(Source src);
	static void InvalidateAllFrom(StationID sid);
	static void AfterLoad();
	static void PostVehiclesAfterLoad();
	static bool ValidateDeferredCargoPayments();
};

/**
 * Simple collection class for a list of cargo packets.
 * @tparam Tinst Actual instantiation of this cargo list.
 */
template <class Tinst, class Tcont>
class CargoList {
public:
	/** The iterator for our container. */
	typedef typename Tcont::iterator Iterator;
	/** The reverse iterator for our container. */
	typedef typename Tcont::reverse_iterator ReverseIterator;
	/** The const iterator for our container. */
	typedef typename Tcont::const_iterator ConstIterator;
	/** The const reverse iterator for our container. */
	typedef typename Tcont::const_reverse_iterator ConstReverseIterator;

	/** Kind of actions that could be done with packets on move. */
	enum class MoveToAction : uint8_t {
		Transfer, ///< Transfer the cargo to the station.
		Deliver, ///< Deliver the cargo to some town or industry.
		Keep, ///< Keep the cargo in the vehicle.
		Load, ///< Load the cargo from the station.
		End, ///< End marker.
	};

protected:
	uint64_t cargo_periods_in_transit = 0; ///< Cache for the sum of number of cargo aging periods in transit of each entity; comparable to man-hours.

	NO_UNIQUE_ADDRESS Tcont packets;       ///< The cargo packets in this list.
	uint count = 0;                        ///< Cache for the number of cargo entities.

	void AddToCache(const CargoPacket *cp);

	void RemoveFromCache(const CargoPacket *cp, uint count);

	static bool TryMerge(CargoPacket *cp, CargoPacket *icp);

public:
	/** Create the cargo list. */
	CargoList() {}

	~CargoList();

	void OnCleanPool();

	/**
	 * Returns a pointer to the cargo packet list (so you can iterate over it etc).
	 * @return Pointer to the packet list.
	 */
	inline const Tcont *Packets() const
	{
		return &this->packets;
	}

	/**
	 * Returns average number of cargo aging periods in transit for a cargo entity.
	 * @return The before mentioned number.
	 */
	inline uint PeriodsInTransit() const
	{
		return this->count == 0 ? 0 : this->cargo_periods_in_transit / this->count;
	}

	/**
	 * Returns sum of cargo, including reserved cargo.
	 * @return Sum of cargo.
	 */
	inline uint TotalCount() const
	{
		return this->count;
	}

	inline uint64_t CargoPeriodsInTransit() const
	{
		return this->cargo_periods_in_transit;
	}

	void InvalidateCache();
};

typedef jgr::ring_buffer<CargoPacket *> CargoPacketList;

/**
 * CargoList that is used for vehicles.
 */
class VehicleCargoList : public CargoList<VehicleCargoList, CargoPacketList> {
protected:
	/** The (direct) parent of this class. */
	typedef CargoList<VehicleCargoList, CargoPacketList> Parent;

	Money feeder_share; ///< Cache for the feeder share.
	EnumIndexArray<uint, MoveToAction, MoveToAction::End> action_counts{}; ///< Counts of cargo to be transferred, delivered, kept and loaded.

	template <class Taction>
	void ShiftCargo(Taction action);

	template <class Taction, class Tfilter>
	void ShiftCargoWithFrontInsert(Taction action, Tfilter filter);

	template <class Taction>
	void PopCargo(Taction action);

	inline uint RecalculateCargoTotal() const
	{
		uint total = 0;
		for (const auto &cp : this->packets) {
			total += cp->Count();
		}
		return total;
	}

	void AssertCountConsistencyError() const;
public:

	/**
	 * Assert that the designation counts add up.
	 */
	inline void AssertCountConsistency() const
	{
#ifdef WITH_ASSERT
		if (unlikely(this->action_counts[MoveToAction::Keep] +
				this->action_counts[MoveToAction::Deliver] +
				this->action_counts[MoveToAction::Transfer] +
				this->action_counts[MoveToAction::Load] != this->count)) {
			this->AssertCountConsistencyError();
		}
#endif
	}

protected:
	void AddToCache(const CargoPacket *cp);
	void RemoveFromCache(const CargoPacket *cp, uint count);

	void AddToMeta(const CargoPacket *cp, MoveToAction action);
	void RemoveFromMeta(const CargoPacket *cp, MoveToAction action, uint count);

	static MoveToAction ChooseAction(const CargoPacket *cp, StationID cargo_next,
			StationID current_station, bool accepted, std::span<const StationID> next_station);

public:
	/** The station cargo list needs to control the unloading. */
	friend class StationCargoList;
	friend upstream_sl::SlVehicleCommon;
	/** The super class ought to know what it's doing. */
	friend class CargoList<VehicleCargoList, CargoPacketList>;
	/** The vehicles have a cargo list (and we want that saved). */
	friend NamedSaveLoadTable GetVehicleDescription(VehicleType vt);

	friend class CargoShift;
	friend class CargoTransfer;
	friend class CargoDelivery;
	template <class Tsource>
	friend class CargoRemoval;
	friend class CargoReturn;
	friend class VehicleCargoReroute;

	/**
	 * Returns the first station of the first cargo packet in this list.
	 * @return The before mentioned station.
	 */
	inline StationID GetFirstStation() const
	{
		return this->count == 0 ? StationID::Invalid() : this->packets.front()->first_station;
	}

	/**
	 * Returns total sum of the feeder share for all packets.
	 * @return The before mentioned number.
	 */
	inline Money GetFeederShare() const
	{
		return this->feeder_share;
	}

	/**
	 * Returns the amount of cargo designated for a given purpose.
	 * @param action Action the cargo is designated for.
	 * @return Amount of cargo designated for the given action.
	 */
	inline uint ActionCount(MoveToAction action) const
	{
		return this->action_counts[action];
	}

	/**
	 * Returns sum of cargo on board the vehicle (ie not only
	 * reserved).
	 * @return Cargo on board the vehicle.
	 */
	inline uint StoredCount() const
	{
		return this->count - this->action_counts[MoveToAction::Load];
	}

	/**
	 * Returns sum of reserved cargo.
	 * @return Sum of reserved cargo.
	 */
	inline uint ReservedCount() const
	{
		return this->action_counts[MoveToAction::Load];
	}

	/**
	 * Returns sum of cargo to be moved out of the vehicle at the current station.
	 * @return Cargo to be moved.
	 */
	inline uint UnloadCount() const
	{
		return this->action_counts[MoveToAction::Transfer] + this->action_counts[MoveToAction::Deliver];
	}

	/**
	 * Returns the sum of cargo to be kept in the vehicle at the current station.
	 * @return Cargo to be kept or loaded.
	 */
	inline uint RemainingCount() const
	{
		return this->action_counts[MoveToAction::Keep] + this->action_counts[MoveToAction::Load];
	}

	void Append(CargoPacket *cp, MoveToAction action = MoveToAction::Keep);

	void AgeCargo();

	void InvalidateCache();

	bool Stage(bool accepted, StationID current_station, std::span<const StationID> next_station, OrderUnloadType unload_type, const GoodsEntry *ge, CargoType cargo, CargoPayment *payment, TileIndex current_tile, PassengerRoutingContext *routing = nullptr, PassengerJourneyShadowContext *shadow = nullptr, PassengerJourneyActiveContext *active = nullptr);

	/**
	 * Marks all cargo in the vehicle as to be kept. This is mostly useful for
	 * loading old savegames. When loading is aborted the reserved cargo has
	 * to be returned first.
	 */
	inline void KeepAll()
	{
		this->action_counts[MoveToAction::Deliver] = 0;
		this->action_counts[MoveToAction::Transfer] = 0;
		this->action_counts[MoveToAction::Load] = 0;
		this->action_counts[MoveToAction::Keep] = this->count;
	}

	/* Methods for moving cargo around. First parameter is always maximum
	 * amount of cargo to be moved. Second parameter is destination (if
	 * applicable), return value is amount of cargo actually moved. */

	template <MoveToAction Tfrom, MoveToAction Tto>
	uint Reassign(uint max_move);
	uint Return(uint max_move, StationCargoList *dest, StationID next_station, TileIndex current_tile, CargoType cargo = INVALID_CARGO);
	/** Return selected reserved portions only, preserving all stored cargo and other reservations. */
	uint ReturnReserved(StationCargoList *dest, StationID next_station, TileIndex current_tile, const std::function<bool(const CargoPacket &)> &select, CargoType cargo = INVALID_CARGO);
	uint Unload(uint max_move, StationCargoList *dest, CargoType cargo, CargoPayment *payment, TileIndex current_tile);
	uint Shift(uint max_move, VehicleCargoList *dest);
	uint Truncate(uint max_move = UINT_MAX);
	uint Reroute(uint max_move, VehicleCargoList *dest, StationID avoid, StationID avoid2, const GoodsEntry *ge);
	uint RerouteFromSource(uint max_move, VehicleCargoList *dest, StationID source, StationID avoid, StationID avoid2, const GoodsEntry *ge);

	/**
	 * Are the two CargoPackets mergeable in the context of
	 * a list of CargoPackets for a Vehicle?
	 * @param cp1 First CargoPacket.
	 * @param cp2 Second CargoPacket.
	 * @return True if they are mergeable.
	 */
	static bool AreMergable(const CargoPacket *cp1, const CargoPacket *cp2)
	{
		return cp1->source_xy           == cp2->source_xy &&
				cp1->periods_in_transit == cp2->periods_in_transit &&
				cp1->first_station      == cp2->first_station &&
				cp1->source             == cp2->source &&
				cp1->passenger_via      == cp2->passenger_via &&
				cp1->passenger_destination == cp2->passenger_destination && cp1->HasSamePassengerRouting(*cp2);
	}
};

using StationCargoPacketList = std::list<CargoPacket *>; ///< Stable handles for direct removal of indexed boarding candidates.
typedef MultiMap<StationID, CargoPacket *, StationCargoPacketList> StationCargoPacketMap;
typedef btree::btree_map<StationID, uint> StationCargoAmountMap;

/**
 * CargoList that is used for stations.
 */
class StationCargoList : public CargoList<StationCargoList, StationCargoPacketMap> {
	using FallbackKey = std::tuple<StationID, StationID, OrderListID, uint64_t>; ///< Destination, saved hop, arriving service/incarnation.
	using BoardingOrder = std::tuple<StationID, uint64_t, CargoPacketID>;
	using JourneyKey = std::tuple<OrderListID, VehicleID, StationID, VehicleOrderID>;
	using JourneyRideKey = std::tuple<PassengerJourneyExecution, OrderListID, uint64_t, PassengerBoardingFailure>;
	using JourneyRides = std::map<JourneyRideKey, std::set<BoardingOrder>>;
	using JourneyIndex = std::map<JourneyKey, JourneyRides>;
	static JourneyKey BoardingJourneyKey(const CargoPacket &packet);
	using HyperpathKey = std::tuple<StationID, uint64_t, uint64_t, OrderListID, uint64_t, OrderListID, uint64_t>;
	struct HyperpathWaiting {
		std::set<BoardingOrder> packets; std::multiset<StateTicks> expiries;
		/* Derived lookup state, never saved: HYPERPATH_UNINDEXED, HYPERPATH_PENDING, NONE, table slot 0/1 or ALWAYS. */
		int8_t lookup_slot = -2;
		std::vector<uint32_t> lookup_occurrences; ///< Entries in the slot table's occurrence numbering.
	};
	using HyperpathGroups = std::map<HyperpathKey, HyperpathWaiting>;
	struct HyperpathGroupRef { StationID destination; HyperpathGroups::iterator group; };
	/** Derived occurrence -> waiting policy group lookup, replacing the walk over every group.
	 * Rebuilt from the database epoch; kept current from its destination change log and the
	 * groups created since the last refresh. */
	struct HyperpathLookup {
		uint64_t epoch = UINT64_MAX, changes = UINT64_MAX;
		StationID station = StationID::Invalid();
		CargoType cargo = INVALID_CARGO;
		std::array<std::unordered_map<uint32_t, std::vector<HyperpathGroupRef>>, 2> occurrences;
		std::vector<HyperpathGroupRef> always; ///< Failed-carrier groups and legacy plans.
		std::vector<HyperpathGroupRef> pending; ///< New groups (HYPERPATH_PENDING) awaiting their first index.
		std::vector<StationID> changed; ///< Scratch for the change log.
	};
	static constexpr int8_t HYPERPATH_UNINDEXED = -2, HYPERPATH_PENDING = -3;
	static HyperpathKey BoardingHyperpathKey(const JourneyRideKey &ride);
	static StateTicks BoardingHyperpathExpiry(const JourneyRideKey &ride);
	struct WaitingEntry {
		StationID key;
		StationCargoPacketList::iterator position;
		uint64_t hash;
		OrderListID service;
		uint16_t space;
		bool pending;
		uint64_t sequence; ///< Primary queue order; preserved through refresh and reconstructed from saved lists.
		std::multimap<StateTicks, CargoPacketID>::iterator wake;
		StateTicks routing_due;
		bool journey;
		std::optional<FallbackKey> fallback;
		std::optional<std::pair<JourneyIndex::iterator, JourneyRides::iterator>> journey_group;
	};
	mutable bool waiting_index_valid = false;
	struct PacketHash { size_t operator()(CargoPacketID id) const { return id.base(); } };
	mutable std::unordered_map<CargoPacketID, WaitingEntry, PacketHash> waiting_entries;
	mutable std::unordered_map<uint64_t, std::set<std::pair<uint16_t, CargoPacketID>>> merge_index;
	mutable JourneyIndex boarding_index;
	mutable std::map<uint64_t, std::map<JourneyRideKey, const std::set<BoardingOrder> *>> section_boarding_index; ///< Raw saved order-pair keys to shared cohort queues.
	mutable std::map<StationID, HyperpathGroups> hyperpath_boarding_index; ///< Option (b): policy-state queues, independent of per-packet decision timestamps and table publication.
	mutable HyperpathLookup hyperpath_lookup;
	void IndexHyperpathGroup(StationID destination, HyperpathGroups::iterator group) const;
	void UnindexHyperpathGroup(HyperpathGroups::iterator group) const;
	void RefreshHyperpathLookup(StationID station) const;
	mutable std::map<OrderListID, size_t> boarding_services;
	mutable std::map<std::pair<bool, StationID>, std::set<BoardingOrder>> legacy_boarding_index;
	mutable std::map<FallbackKey, std::set<BoardingOrder>> fallback_boarding_index;
	mutable std::map<StationID, std::set<FallbackKey>> fallback_destinations, fallback_hops;
	mutable uint64_t waiting_next_sequence = 0;
	mutable std::multimap<StateTicks, CargoPacketID> waiting_wake;
	mutable std::set<std::pair<StateTicks, CargoPacketID>> routing_events; ///< Erasable time priority queue, without stale tombstones.
	mutable std::set<CargoPacketID> routing_packets; ///< Unresolved OD only; deterministic generation cursor, independent of primary queue order.
	mutable std::set<CargoPacketID> legacy_journey_packets; ///< Saved legacy intents eligible for budgeted migration when Hyperpath is enabled.
	CargoPacketID NextPassengerGenerationPacket(uint32_t cursor) const;
	StationID passenger_station = StationID::Invalid(); ///< Derived owner, assigned when the goods entry is created.
	mutable StateTicks passenger_scheduled_at = INVALID_STATE_TICKS; ///< Derived national queue handle.
	mutable bool passenger_unresolved_registered = false;
	void IndexPacket(CargoPacket *packet, StationID key, StationCargoPacketList::iterator position, std::optional<uint64_t> sequence = {}) const;
	void EnsureWaitingIndex() const;
	void AddBoardingEntry(CargoPacketID id, WaitingEntry &entry) const;
	void RemoveBoardingEntry(CargoPacketID id, const WaitingEntry &entry) const;
	template <typename Visitor> bool VisitBoardingCandidates(std::span<const StationID> next, const Vehicle *vehicle, PassengerJourneyLoadingContext &loading, Visitor &&visitor, bool defer_hyperpath = true) const;
	template <class Taction> uint ShiftIndexedJourneyCargo(Taction action, std::span<const StationID> next, const Vehicle *vehicle, PassengerJourneyLoadingContext &loading);
protected:
	/** The (direct) parent of this class. */
	typedef CargoList<StationCargoList, StationCargoPacketMap> Parent;

	uint reserved_count = 0; ///< Amount of cargo being reserved for loading.
	void RemoveFromCache(const CargoPacket *cp, uint count);

public:
	~StationCargoList();
	void BindPassengerStation(StationID station, CargoType cargo);
	void UpdatePassengerRoutingSchedule() const;
	friend void RunPassengerWaitingEvents();
	friend void RebuildPassengerWaitingScheduler();
	friend struct PassengerWaitingEventDiagnostic;
	friend PassengerWaitingEventDiagnostic GetPassengerWaitingEventDiagnostic();
	void ClearWaitingIndex();
	void ForgetWaitingPacket(CargoPacketID id) const;
	void RefreshWaitingPacket(const CargoPacket &packet) const;
	uint64_t BoardingCandidatesInspected() const { return this->boarding_candidates_inspected; }
	mutable uint64_t boarding_candidates_inspected = 0; ///< Diagnostic only; absent from saves.
	mutable uint64_t boarding_groups_inspected = 0; ///< Candidate cohorts, independent of unrelated packet population.
	mutable uint64_t boarding_index_updates = 0; ///< Diagnostic: count/payment-only refreshes do not touch boarding cohorts.
	StationID passenger_boarding_last = StationID::Invalid(); ///< Saved round-robin cursor across primary next-hop buckets; FIFO within each bucket.
	bool passenger_routing_dirty = true; ///< Saved event state; joining clients must not invent a waiting pass.
	uint64_t passenger_routing_revision = UINT64_MAX, passenger_routing_rows = UINT64_MAX;
	StateTicks passenger_routing_due = INVALID_STATE_TICKS;
	bool passenger_routing_sweeping = false;
	uint32_t passenger_routing_cursor = 0, passenger_routing_end = 0;
	StateTicks passenger_routing_tick = INVALID_STATE_TICKS;
	uint16_t passenger_routing_work = 0;
	StateTicks passenger_routing_swept = INVALID_STATE_TICKS; ///< Saved start of the latest generation sweep; bounds sweeps due only to publications.
	/** Empty cargo may still own saved scheduling state. In particular, dropping
	 * it on load must not reset a slice or pass its cursor to a reused goods entry. */
	bool HasPassengerRoutingState() const
	{
		return this->passenger_boarding_last != StationID::Invalid() || !this->passenger_routing_dirty || this->passenger_routing_revision != UINT64_MAX || this->passenger_routing_rows != UINT64_MAX ||
				this->passenger_routing_due != INVALID_STATE_TICKS || this->passenger_routing_sweeping || this->passenger_routing_cursor != 0 ||
				this->passenger_routing_end != 0 || this->passenger_routing_tick != INVALID_STATE_TICKS || this->passenger_routing_work != 0 ||
				this->passenger_routing_swept != INVALID_STATE_TICKS;
	}
	static constexpr uint16_t PASSENGER_ROUTING_SLICE = 256; ///< Cheap table events; national budget and station fairness also apply.
	/** A service publication alone starts a generation sweep at most this often. It cannot resolve
	 * a waiting packet through Hyperpath rows at all (a rare safety pass only); legacy rows keep a
	 * shorter bound. Table changes, flow changes and invalidations still sweep immediately. */
	static constexpr int64_t PASSENGER_REVISION_SWEEP_TICKS = 1024, PASSENGER_TABLE_REVISION_SWEEP_TICKS = 8192;
	uint64_t passenger_routing_inspected = 0, passenger_generation_inspected = 0; ///< Diagnostics only.
	void RebuildWaitingIndex() const { this->EnsureWaitingIndex(); }
	CargoType passenger_index_cargo = INVALID_CARGO; ///< Derived owning cargo, supplied by routing/boarding events.
	/** The super class ought to know what it's doing. */
	friend class CargoList<StationCargoList, StationCargoPacketMap>;
	/** The stations, via GoodsEntry, have a CargoList. */
	friend NamedSaveLoadTable GetGoodsDesc();
	friend upstream_sl::SlStationGoods;

	friend class CargoLoad;
	friend class CargoTransfer;
	template <class Tsource>
	friend class CargoRemoval;
	friend class CargoReservation;
	friend class CargoReturn;
	friend class StationCargoReroute;

	static void InvalidateAllFrom(Source src);

	template <class Taction>
	bool ShiftCargo(Taction &action, StationID next);

	template <class Taction>
	uint ShiftCargo(Taction action, std::span<const StationID> next, bool include_invalid);
	template <class Taction>
	uint ShiftJourneyCargo(Taction action, std::span<const StationID> next, const Vehicle *vehicle, PassengerJourneyLoadingContext &loading);

	template <class Taction>
	bool ShiftCargoFromSource(Taction &action, StationID source, StationID next);

	template <class Taction>
	uint ShiftCargoFromSource(Taction action, StationID source, std::span<const StationID> next, bool include_invalid);

	void Append(CargoPacket *cp, StationID next);

	/**
	 * Check for cargo headed for a specific station.
	 * @param next Station the cargo is headed for.
	 * @return If there is any cargo for that station.
	 */
	bool HasCargoFor(std::span<const StationID> next, const Vehicle *vehicle = nullptr, PassengerJourneyLoadingContext *loading = nullptr, CargoType cargo = INVALID_CARGO) const;
	void RecordCapacityMiss(std::span<const StationID> next, const Vehicle *vehicle, CargoType cargo, PassengerJourneyLoadingContext &loading);
	/** Re-key waiting packets without touching count/payment caches or splitting them. */
	size_t RoutePassengerJourneys(CargoType cargo, StationID station, bool allow_search = true, PassengerJourneyLoadingContext *loading = nullptr);

	/**
	 * Returns first station of the first cargo packet in this list.
	 * @return The before mentioned station.
	 */
	inline StationID GetFirstStation() const
	{
		return this->count == 0 ? StationID::Invalid() : this->packets.begin()->second.front()->first_station;
	}

	/**
	 * Returns sum of cargo still available for loading at the station.
	 * (i.e. not counting cargo which is already reserved for loading)
	 * @return Cargo on board the vehicle.
	 */
	inline uint AvailableCount() const
	{
		return this->count;
	}

	uint AvailableViaCount(StationID next) const;

	/**
	 * Returns sum of cargo reserved for loading onto vehicles.
	 * @return Cargo reserved for loading.
	 */
	inline uint ReservedCount() const
	{
		return this->reserved_count;
	}

	/**
	 * Returns total count of cargo at the station, including
	 * cargo which is already reserved for loading.
	 * @return Total cargo count.
	 */
	inline uint TotalCount() const
	{
		return this->count + this->reserved_count;
	}

	/* Methods for moving cargo around. First parameter is always maximum
	 * amount of cargo to be moved. Second parameter is destination (if
	 * applicable), return value is amount of cargo actually moved. */

	uint Reserve(uint max_move, VehicleCargoList *dest, std::span<const StationID> next, TileIndex current_tile, const Vehicle *vehicle = nullptr, PassengerJourneyLoadingContext *loading = nullptr);
	uint Load(uint max_move, VehicleCargoList *dest, std::span<const StationID> next, TileIndex current_tile, const Vehicle *vehicle = nullptr, PassengerJourneyLoadingContext *loading = nullptr);
	uint Truncate(uint max_move = UINT_MAX, StationCargoAmountMap *cargo_per_source = nullptr);
	uint Reroute(uint max_move, StationCargoList *dest, StationID avoid, StationID avoid2, const GoodsEntry *ge);
	uint RerouteFromSource(uint max_move, StationCargoList *dest, StationID source, StationID avoid, StationID avoid2, const GoodsEntry *ge);

	void AfterLoadIncreaseReservationCount(uint count)
	{
		this->reserved_count += count;
	}

	void LoadSetReservedCount(uint count)
	{
		this->reserved_count = count;
	}

	/**
	 * Are the two CargoPackets mergeable in the context of
	 * a list of CargoPackets for a Station?
	 * @param cp1 First CargoPacket.
	 * @param cp2 Second CargoPacket.
	 * @return True if they are mergeable.
	 */
	static bool AreMergable(const CargoPacket *cp1, const CargoPacket *cp2)
	{
		return cp1->source_xy           == cp2->source_xy &&
				cp1->periods_in_transit == cp2->periods_in_transit &&
				cp1->first_station      == cp2->first_station &&
				cp1->source             == cp2->source &&
				cp1->passenger_destination == cp2->passenger_destination && cp1->HasSamePassengerRouting(*cp2);
	}
};

#endif /* CARGOPACKET_H */
