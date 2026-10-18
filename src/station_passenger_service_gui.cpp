/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */

/** @file station_passenger_service_gui.cpp Persistent, incremental, display-only passenger service resolution. */
#include "stdafx.h"
#include "station_cargo_gui.h"
#include "passenger_service_database.h"
#include "passenger_journey_active.h"
#include "cargopacket.h"
#include "date_func.h"
#include "group.h"
#include "vehicle_base.h"
#include "station_base.h"
#include "roadveh.h"
#include "roadstop_base.h"
#include "infrastructure_func.h"
#include <deque>
#include <variant>

namespace {
using State = StationPassengerServiceState;
using DisplayLabel = StationPassengerServiceLabel;
constexpr size_t MAX_ENTRIES = 4096, MAX_WORK = 16384, SLICE = 64;

struct FleetKey {
	CargoType cargo;
	OrderListID service;
	uint64_t signature;
	StationID boarding, alighting;
	VehicleOrderID board, alight;
	VehicleID vehicle;
	PassengerRideKind kind;
	bool policy = false;
	auto operator<=>(const FleetKey &) const = default;
};
using PolicyKey = std::tuple<CargoType, uint64_t, StationID, StationID, OrderListID, OrderListID>;

FleetKey KeyFor(CargoType cargo, const PassengerJourneyExecution &ride, bool policy = false)
{
	auto kind = IsPassengerRideIntent(ride.kind) ? PassengerRideKind::ServiceIntent : ride.kind;
	return {cargo, ride.service, ride.signature, ride.boarding, ride.alighting, ride.boarding_order, ride.alighting_order,
			kind == PassengerRideKind::ServiceIntent ? VehicleID::Invalid() : ride.vehicle, kind, policy};
}

/** Evidence contains only validated candidates, with a live representative for cheap invalidation. */
struct Evidence {
	GroupID group = GroupID::Invalid();
	bool multiple = false, ungrouped = false, usable = false, pooled = false, strict_signature = true, check_database = false;
	VehicleID representative = VehicleID::Invalid(), compartment = VehicleID::Invalid();
	OrderListID service = OrderListID::Invalid();
	uint64_t signature = 0, generation = 0, route_generation = 0;
	CargoType cargo = INVALID_CARGO;
};

bool ValidRepresentative(const Evidence &e)
{
	if (e.group == GroupID::Invalid()) return true;
	const Group *group = Group::GetIfValid(e.group);
	if (group == nullptr) return false;
	if (!e.pooled) return true; // Non-pooled, immediate test fixtures never retain a game pointer.
	if (e.check_database) {
		PassengerJourneyExecution ride; ride.kind = PassengerRideKind::EstimatedServiceIntent;
		ride.service = e.service; ride.signature = e.signature; ride.generation = e.generation;
		if (!GetPassengerServiceDatabase().IsCurrent(ride)) return false;
	}
	const Vehicle *v = Vehicle::GetIfValid(e.representative), *part = Vehicle::GetIfValid(e.compartment);
	return v != nullptr && part != nullptr && part->First() == v && part->cargo_type == e.cargo && part->cargo_cap != 0 &&
			v->orders != nullptr && v->orders->index == e.service && v->orders->GetCargoDistRouteGeneration() == e.route_generation && v->group_id == e.group &&
			group->owner == v->owner && group->vehicle_type == v->type && !v->vehstatus.Test(VehState::Crashed) &&
			(!e.strict_signature || v->orders->GetCargoDistRouteGeneration() == e.signature);
}

void Merge(Evidence &a, const Evidence &b)
{
	bool different = a.group != GroupID::Invalid() && b.group != GroupID::Invalid() && a.group != b.group;
	bool multiple = a.multiple || b.multiple || different;
	bool ungrouped = a.ungrouped || b.ungrouped, usable = a.usable || b.usable;
	if (b.group != GroupID::Invalid() && (a.group == GroupID::Invalid() || b.group < a.group)) a = b;
	a.multiple = multiple; a.ungrouped = ungrouped; a.usable = usable;
}

struct Progress {
	State state = State::Pending;
	Evidence evidence, previous;
	uint64_t serial = 0, due = 0;
	bool queued = false;
};

enum class Phase : uint8_t { Vehicle, Capacity, Boarding, Alighting, Accept };
struct Fleet : Progress {
	PassengerJourneyExecution ride;
	VehicleID next = VehicleID::Invalid(), part = VehicleID::Invalid(), capacity_part = VehicleID::Invalid(), first = VehicleID::Invalid();
	RoadStopID stop = RoadStopID::Invalid();
	Phase phase = Phase::Vehicle;
	uint vehicle_count = 0;
	uint64_t route_generation = 0;
	size_t pins = 0;
	bool started = false, stop_started = false;
};
struct Policy : Progress {
	PassengerJourneyExecution intent, current;
	PassengerPrecomputedTable::DisplayCursor cursor;
	std::optional<FleetKey> waiting;
	uint64_t epoch = 0, destination_revision = 0;
	bool saw_candidate = false, uncertain = false, waiting_held = false;
};
static_assert((sizeof(Fleet) + sizeof(Policy) + 512) * MAX_ENTRIES < 8 * 1024 * 1024);
using JobKey = std::variant<FleetKey, PolicyKey>;
struct Job { JobKey key; uint64_t serial; };

Evidence VisibleEvidence(const Progress &p)
{
	Evidence result = p.evidence;
	if ((p.state == State::Pending || p.state == State::Partial) && ValidRepresentative(p.previous)) Merge(result, p.previous);
	return result;
}

StationPassengerService Summary(const Progress &p, const Evidence &e)
{
	StationPassengerService result;
	if (e.group != GroupID::Invalid()) {
		result.group = e.group;
		result.multiple = e.multiple || e.ungrouped || p.state == State::Pending || p.state == State::Partial;
	} else if (p.state == State::Pending || p.state == State::Partial) result.label = DisplayLabel::Checking;
	else if (p.state == State::Unavailable) result.label = DisplayLabel::Updating;
	else if (e.ungrouped) result.label = DisplayLabel::Ungrouped;
	return result;
}
} // namespace

struct StationPassengerServiceCache::Impl {
	std::map<FleetKey, Fleet> fleets;
	std::map<PolicyKey, Policy> policies;
	std::deque<FleetKey> fleet_eviction;
	std::deque<PolicyKey> policy_eviction;
	std::deque<Job> jobs;
	std::map<StationID, uint64_t> destination_revisions;
	StationPassengerServiceDiagnostics diagnostic;
	uint64_t now = 0, serial = 0, lookup_epoch = 0, changes = 0, policy_epoch = 1, version = 0;
	size_t credit = MAX_WORK;
	uint64_t fractional_credit = 0;

	template <typename Key> void Enqueue(const Key &key, Progress &p)
	{
		if (p.queued || (p.state != State::Pending && p.state != State::Partial)) return;
		p.queued = true; this->jobs.push_back({key, p.serial});
		// Removed entries leave at most one stale job each; compact before the queue can grow unbounded.
		if (this->jobs.size() > MAX_ENTRIES * 4) {
			std::erase_if(this->jobs, [&](const Job &job) { return !this->Find(job); });
		}
	}

	Progress *Find(const Job &job)
	{
		return std::visit([&](const auto &key) -> Progress * {
			using Key = std::decay_t<decltype(key)>;
			if constexpr (std::is_same_v<Key, FleetKey>) {
				auto it = this->fleets.find(key);
				return it != this->fleets.end() && it->second.serial == job.serial ? &it->second : nullptr;
			} else {
				auto it = this->policies.find(key);
				return it != this->policies.end() && it->second.serial == job.serial ? &it->second : nullptr;
			}
		}, job.key);
	}

	template <typename Map, typename Queue> bool MakeRoom(Map &map, Queue &queue)
	{
		if (map.size() < MAX_ENTRIES) return true;
		// Protect work in progress. Rotate a bounded clock, preferring settled entries for eviction.
		for (size_t i = 0; i < 64 && !queue.empty(); ++i) {
			auto key = queue.front(); queue.pop_front(); auto it = map.find(key);
			if (it == map.end()) continue;
			bool pinned = false;
			if constexpr (std::is_same_v<typename Map::mapped_type, Fleet>) pinned = it->second.pins != 0;
			if (!pinned && (it->second.state == State::Complete || it->second.state == State::Unavailable)) {
				if constexpr (std::is_same_v<typename Map::mapped_type, Policy>) this->ReleaseWaiting(it->second);
				map.erase(it); ++this->diagnostic.evictions; return true;
			}
			queue.push_back(key);
		}
		return false;
	}

	uint64_t Revision(StationID destination) const
	{
		auto it = this->destination_revisions.find(destination);
		return it == this->destination_revisions.end() ? 0 : it->second;
	}

	void Synchronize()
	{
		const auto &db = GetPassengerServiceDatabase();
		const uint64_t epoch = db.HyperpathLookupEpoch(), latest = db.HyperpathDestinationChanges();
		if (epoch == this->lookup_epoch && latest == this->changes) return;
		if (epoch != this->lookup_epoch || latest - this->changes > PassengerServiceDatabase::HYPERPATH_DESTINATION_LOG) {
			++this->policy_epoch; this->destination_revisions.clear();
		} else {
			for (uint64_t n = this->changes; n < latest; ++n) this->destination_revisions[db.HyperpathChangedDestination(n)] = n + 1;
			if (this->destination_revisions.size() > MAX_ENTRIES * 2) { ++this->policy_epoch; this->destination_revisions.clear(); }
		}
		this->lookup_epoch = epoch; this->changes = latest;
	}

	void Restart(Fleet &entry, bool retain)
	{
		Evidence previous = VisibleEvidence(entry);
		entry.evidence = {}; entry.previous = retain && ValidRepresentative(previous) ? previous : Evidence{};
		entry.state = State::Pending; entry.started = false; entry.phase = Phase::Vehicle;
		entry.next = entry.part = entry.capacity_part = VehicleID::Invalid(); entry.stop_started = false;
		++this->diagnostic.refreshes; ++this->version;
	}

	void ReleaseWaiting(Policy &entry)
	{
		if (entry.waiting_held && entry.waiting) {
			auto it = this->fleets.find(*entry.waiting);
			if (it != this->fleets.end() && it->second.pins != 0) --it->second.pins;
		}
		entry.waiting.reset(); entry.waiting_held = false;
	}

	void Restart(Policy &entry, bool retain)
	{
		Evidence previous = VisibleEvidence(entry);
		entry.evidence = {}; entry.previous = retain && ValidRepresentative(previous) ? previous : Evidence{};
		entry.state = State::Pending; entry.cursor = {}; this->ReleaseWaiting(entry); entry.saw_candidate = entry.uncertain = false;
		entry.epoch = this->policy_epoch; entry.destination_revision = this->Revision(entry.intent.alighting);
		++this->diagnostic.refreshes; ++this->version;
	}

	void Settle(Progress &entry, State state)
	{
		entry.state = state; entry.previous = {};
		if (state == State::Unavailable) entry.evidence = {};
		entry.due = this->now + (state == State::Unavailable ? 1000 : 5000) + (entry.serial * 2654435761ULL) % (state == State::Unavailable ? 1000 : 5000);
		++this->version;
	}

	/** Road stop chains, like consists, can span updates. The simulation helper remains unchanged. */
	std::optional<bool> StationAccess(Fleet &entry, const Vehicle *v, StationID station, const Order *order, size_t &work)
	{
		if (v->type != VehicleType::Road) {
			if (work == 0) return {};
			return CanPassengerVehicleUseStation(v, station, order, work);
		}
		const Station *st = Station::GetIfValid(station);
		if (!entry.stop_started) {
			if (work == 0) return {};
			--work;
			if (st == nullptr || !IsPassengerStationStop(*order) || !IsInfraUsageAllowed(v->type, v->owner, st->owner)) return false;
			const RoadStop *stop = st->GetPrimaryRoadStop(IsCargoInClass(v->cargo_type, CargoClass::Passengers) ? RoadStopType::Bus : RoadStopType::Truck);
			entry.stop = stop == nullptr ? RoadStopID::Invalid() : stop->index; entry.stop_started = true;
		}
		const RoadVehicle *rv = RoadVehicle::From(v);
		while (entry.stop != RoadStopID::Invalid()) {
			if (work == 0) return {};
			--work;
			const RoadStop *stop = RoadStop::GetIfValid(entry.stop);
			if (stop == nullptr) return false;
			entry.stop = stop->next == nullptr ? RoadStopID::Invalid() : stop->next->index;
			if (!IsValidTile(stop->xy) || !IsStationRoadStopTile(stop->xy) || GetStationIndex(stop->xy) != station ||
					!HasTileAnyRoadType(stop->xy, rv->compatible_roadtypes) || !IsInfraTileUsageAllowed(v->type, v->owner, stop->xy)) continue;
			if (IsBayRoadStopTile(stop->xy) && rv->HasArticulatedPart()) continue;
			if (order->GetRoadVehTravelDirection() != DiagDirection::Invalid &&
					((IsDriveThroughStopTile(stop->xy) && DiagDirToAxis(order->GetRoadVehTravelDirection()) != GetDriveThroughStopAxis(stop->xy)) ||
					(IsBayRoadStopTile(stop->xy) && order->GetRoadVehTravelDirection() != ReverseDiagDir(GetBayRoadStopDir(stop->xy))))) continue;
			return true;
		}
		return false;
	}

	void Process(const FleetKey &key, Fleet &entry, size_t &work)
	{
		if (work == 0) return;
		const OrderList *orders = OrderList::GetIfValid(key.service);
		if (orders == nullptr || (key.kind != PassengerRideKind::VehicleSnapshot && orders->GetCargoDistRouteGeneration() != key.signature)) {
			--work; ++this->diagnostic.stale_service; this->Settle(entry, State::Unavailable); return;
		}
		if (key.board >= orders->GetNumOrders() || (key.alight != INVALID_VEH_ORDER_ID && key.alight >= orders->GetNumOrders())) {
			--work; ++this->diagnostic.stale_service; this->Settle(entry, State::Unavailable); return;
		}
		const Order *board = orders->GetOrderAt(key.board), *alight = key.alight == INVALID_VEH_ORDER_ID ? nullptr : orders->GetOrderAt(key.alight);
		if (!IsPassengerStationStop(*board) || board->GetDestination() != key.boarding || board->GetCargoLoadType(key.cargo) == OrderLoadType::NoLoad ||
				(alight != nullptr && (!IsPassengerStationStop(*alight) || alight->GetDestination() != key.alighting || alight->GetCargoUnloadType(key.cargo) == OrderUnloadType::NoUnload))) {
			--work; ++this->diagnostic.unusable_vehicle; entry.evidence = {}; this->Settle(entry, State::Complete); return;
		}
		const Vehicle *first = orders->GetFirstSharedVehicle();
		if (entry.started && entry.route_generation != orders->GetCargoDistRouteGeneration()) this->Restart(entry, false);
		if (entry.started && (entry.first != (first == nullptr ? VehicleID::Invalid() : first->index) || entry.vehicle_count != orders->GetNumVehicles())) this->Restart(entry, true);
		const bool fixed = key.kind != PassengerRideKind::ServiceIntent;
		const Vehicle *v = !entry.started ? (fixed ? Vehicle::GetIfValid(key.vehicle) : first) : Vehicle::GetIfValid(entry.next);
		if (!entry.started) {
			entry.first = first == nullptr ? VehicleID::Invalid() : first->index;
			entry.vehicle_count = orders->GetNumVehicles(); entry.route_generation = orders->GetCargoDistRouteGeneration(); entry.started = true;
			if (v == nullptr) { --work; ++this->diagnostic.no_shared_vehicle; this->Settle(entry, State::Unavailable); return; }
		} else if (v == nullptr && entry.next != VehicleID::Invalid()) {
			--work; ++this->diagnostic.invalid_vehicle; this->Restart(entry, false); return;
		}
		entry.state = State::Partial;
		while (v != nullptr && work > 0) {
			entry.next = v->index;
			bool reject = false;
			if (entry.phase == Phase::Vehicle) {
				--work; ++this->diagnostic.vehicles;
				if (!IsPassengerServiceVehicle(v) || v->orders != orders || v->vehstatus.Test(VehState::Crashed)) reject = true;
				else { entry.phase = Phase::Capacity; entry.part = v->index; entry.capacity_part = VehicleID::Invalid(); }
			}
			if (!reject && entry.phase == Phase::Capacity) {
				// Keep transient pointers only inside this call, persisting a pool ID at each yield.
				const Vehicle *part = entry.part == v->index ? v : Vehicle::GetIfValid(entry.part);
				while (part != nullptr && work > 0) {
					--work;
					if (part->First() != v) { reject = true; break; }
					if (part->cargo_type == key.cargo && part->cargo_cap > 0) {
						entry.capacity_part = part->index; entry.phase = Phase::Boarding; entry.stop_started = false; break;
					}
					part = part->Next(); entry.part = part == nullptr ? VehicleID::Invalid() : part->index;
				}
				if (entry.phase == Phase::Capacity && part == nullptr) reject = true;
			}
			if (!reject && entry.phase == Phase::Boarding) {
				auto usable = this->StationAccess(entry, v, key.boarding, board, work);
				if (!usable) break;
				reject = !*usable; entry.phase = alight == nullptr ? Phase::Accept : Phase::Alighting; entry.stop_started = false;
			}
			if (!reject && entry.phase == Phase::Alighting) {
				auto usable = this->StationAccess(entry, v, key.alighting, alight, work);
				if (!usable) break;
				reject = !*usable; entry.phase = Phase::Accept; entry.stop_started = false;
			}
			if (!reject && entry.phase != Phase::Accept) break;
			if (reject) ++this->diagnostic.unusable_vehicle;
			else {
				Evidence found; found.usable = true;
				const Group *group = Group::GetIfValid(v->group_id);
				if (group != nullptr && group->owner == v->owner && group->vehicle_type == v->type) {
					found.group = group->index; found.representative = v->index; found.compartment = entry.capacity_part;
					found.pooled = Vehicle::GetIfValid(v->index) == v; found.service = key.service; found.signature = key.signature;
					found.strict_signature = key.kind != PassengerRideKind::VehicleSnapshot; found.cargo = key.cargo; found.generation = entry.ride.generation; found.check_database = key.policy; found.route_generation = orders->GetCargoDistRouteGeneration();
				} else { found.ungrouped = true; ++this->diagnostic.ungrouped; }
				Merge(entry.evidence, found);
			}
			v = fixed ? nullptr : v->NextShared();
			entry.next = v == nullptr ? VehicleID::Invalid() : v->index; entry.phase = Phase::Vehicle;
		}
		if (v == nullptr) this->Settle(entry, State::Complete);
	}

	Fleet *GetFleet(const FleetKey &key, const PassengerJourneyExecution &ride, bool immediate)
	{
		auto it = this->fleets.find(key);
		if (it == this->fleets.end()) {
			if (!this->MakeRoom(this->fleets, this->fleet_eviction)) return nullptr;
			it = this->fleets.try_emplace(key).first; it->second.serial = ++this->serial; it->second.ride = ride;
			this->fleet_eviction.push_back(key);
			if (immediate && this->credit != 0) {
				size_t allowance = std::min(this->credit, SLICE), left = allowance;
				this->Process(key, it->second, left); this->credit -= allowance - left; this->diagnostic.work += allowance - left;
			}
		} else {
			++this->diagnostic.cache_hits;
			auto &entry = it->second;
			entry.ride = ride;
			entry.evidence.generation = entry.previous.generation = ride.generation;
			Evidence evidence = VisibleEvidence(entry);
			const OrderList *list = OrderList::GetIfValid(key.service);
			bool stale = list == nullptr || (key.kind != PassengerRideKind::VehicleSnapshot && list->GetCargoDistRouteGeneration() != key.signature);
			bool changed = !stale && (entry.started && (entry.route_generation != list->GetCargoDistRouteGeneration() || list->GetNumVehicles() != entry.vehicle_count ||
					(list->GetFirstSharedVehicle() == nullptr ? VehicleID::Invalid() : list->GetFirstSharedVehicle()->index) != entry.first));
			if (stale) {
				if (entry.state != State::Unavailable) { ++this->diagnostic.stale_service; entry.evidence = entry.previous = {}; this->Settle(entry, State::Unavailable); }
			} else if ((changed || !ValidRepresentative(evidence)) && (entry.state != State::Unavailable || this->now >= entry.due)) this->Restart(entry, false);
			else if ((entry.state == State::Complete || entry.state == State::Unavailable) && this->now >= entry.due) this->Restart(entry, true);
		}
		this->Enqueue(key, it->second);
		return &it->second;
	}

	void Process(const PolicyKey &key, Policy &entry, size_t &work)
	{
		if (entry.epoch != this->policy_epoch || entry.destination_revision != this->Revision(entry.intent.alighting)) this->Restart(entry, false);
		auto &db = GetPassengerServiceDatabase();
		const auto [cargo, generation, station, destination, previous, failed] = key;
		while (work > 0) {
			if (!db.IsCurrent(entry.intent)) { --work; ++this->diagnostic.stale_policy; entry.evidence = {}; this->ReleaseWaiting(entry); this->Settle(entry, State::Unavailable); return; }
			if (entry.waiting) {
				--work;
				if (!db.IsCurrent(entry.current)) { ++this->diagnostic.stale_service; entry.uncertain = true; }
				else {
					Fleet *fleet = this->GetFleet(*entry.waiting, entry.current, false);
					if (fleet == nullptr) return;
					if (!entry.waiting_held) { ++fleet->pins; entry.waiting_held = true; }
					if (fleet->state == State::Pending || fleet->state == State::Partial) { entry.state = State::Partial; return; }
					Merge(entry.evidence, fleet->evidence);
					entry.uncertain |= fleet->state == State::Unavailable;
				}
				this->ReleaseWaiting(entry);
			}
			PassengerPrecomputedTable::DisplayCandidate candidate;
			auto read = db.NextHyperpathDisplayCandidate(cargo, entry.intent, previous, failed, entry.cursor, candidate, work);
			if (read == PassengerPrecomputedTable::DisplayRead::Unavailable) {
				++this->diagnostic.no_candidates; entry.evidence = {}; this->Settle(entry, State::Unavailable); return;
			}
			if (read == PassengerPrecomputedTable::DisplayRead::Partial) { entry.state = State::Partial; return; }
			if (read == PassengerPrecomputedTable::DisplayRead::Complete) {
				if (!entry.saw_candidate) ++this->diagnostic.no_candidates;
				entry.evidence.multiple |= entry.uncertain;
				this->Settle(entry, entry.uncertain && !entry.evidence.usable ? State::Unavailable : State::Complete); return;
			}
			++this->diagnostic.candidates; entry.saw_candidate = true; entry.state = State::Partial;
			entry.current = {};
			entry.current.kind = PassengerRideKind::EstimatedServiceIntent;
			entry.current.service = candidate.service; entry.current.signature = candidate.incarnation; entry.current.generation = candidate.generation;
			entry.current.boarding = station; entry.current.boarding_order = candidate.order;
			if (!db.IsCurrent(entry.current)) { ++this->diagnostic.stale_service; entry.uncertain = true; continue; }
			entry.waiting = KeyFor(cargo, entry.current, true);
		}
	}

	Evidence PolicyEvidence(const Policy &entry) const
	{
		Evidence evidence = VisibleEvidence(entry);
		if (entry.waiting) {
			auto fleet = this->fleets.find(*entry.waiting);
			if (fleet != this->fleets.end()) Merge(evidence, VisibleEvidence(fleet->second));
		}
		return evidence;
	}

	StationPassengerService ResolvePolicy(CargoType cargo, const CargoPacket &packet)
	{
		this->Synchronize();
		const auto &intent = packet.GetPassengerJourney();
		if (intent.signature != uint64_t(cargo) + 1) return {};
		const auto *arriving = OrderList::GetIfValid(packet.GetPassengerArrivingService());
		OrderListID previous = arriving != nullptr && arriving->GetCargoDistRouteGeneration() == packet.GetPassengerArrivingIncarnation() ? arriving->index : OrderListID::Invalid();
		PolicyKey key{cargo, intent.generation, intent.boarding, intent.alighting, previous, packet.GetPassengerFailedService(_state_ticks)};
		auto it = this->policies.find(key);
		if (it == this->policies.end()) {
			if (!this->MakeRoom(this->policies, this->policy_eviction)) return {GroupID::Invalid(), false, StationID::Invalid(), DisplayLabel::Checking};
			it = this->policies.try_emplace(key).first; auto &entry = it->second;
			entry.intent = intent; entry.serial = ++this->serial; entry.epoch = this->policy_epoch; entry.destination_revision = this->Revision(intent.alighting);
			this->policy_eviction.push_back(key);
		} else {
			++this->diagnostic.cache_hits; auto &entry = it->second;
			if (entry.epoch != this->policy_epoch || entry.destination_revision != this->Revision(intent.alighting) || !ValidRepresentative(this->PolicyEvidence(entry))) this->Restart(entry, false);
			else if ((entry.state == State::Complete || entry.state == State::Unavailable) && this->now >= entry.due) this->Restart(entry, true);
		}
		this->Enqueue(key, it->second);
		return Summary(it->second, this->PolicyEvidence(it->second));
	}
};

StationPassengerServiceCache::StationPassengerServiceCache() : impl(std::make_unique<Impl>()) {}
StationPassengerServiceCache::~StationPassengerServiceCache() = default;
void StationPassengerServiceCache::Clear() { this->impl = std::make_unique<Impl>(); }

StationPassengerService StationPassengerServiceCache::Resolve(CargoType cargo, const PassengerJourneyExecution &ride)
{
	if (cargo >= NUM_CARGO || !IsCargoInClass(cargo, CargoClass::Passengers) || ride.kind == PassengerRideKind::HyperpathIntent) return {};
	auto *entry = this->impl->GetFleet(KeyFor(cargo, ride), ride, true);
	if (entry == nullptr) return {GroupID::Invalid(), false, StationID::Invalid(), DisplayLabel::Checking};
	auto result = Summary(*entry, VisibleEvidence(*entry));
	if (result.label == DisplayLabel::Undecided && result.group == GroupID::Invalid() && entry->evidence.usable && Station::IsValidID(ride.alighting)) result.transfer = ride.alighting;
	return result;
}

StationPassengerService StationPassengerServiceCache::Resolve(CargoType cargo, const CargoPacket &packet, StationID station)
{
	if (packet.GetPassengerRoutingMode() != PassengerRoutingMode::Journey || packet.GetPassengerJourney().boarding != station) return {};
	if (packet.GetPassengerJourney().kind == PassengerRideKind::HyperpathIntent) return this->impl->ResolvePolicy(cargo, packet);
	auto result = this->Resolve(cargo, packet.GetPassengerJourney());
	if (!Station::IsValidID(packet.GetPassengerDestination()) || result.transfer == packet.GetPassengerDestination()) result.transfer = StationID::Invalid();
	return result;
}

bool StationPassengerServiceCache::Advance(uint delta_ms, size_t work_limit)
{
	auto &impl = *this->impl; const uint64_t before = impl.version;
	impl.now += delta_ms; impl.fractional_credit += uint64_t{delta_ms} * MAX_WORK;
	impl.credit = std::min(MAX_WORK, impl.credit + static_cast<size_t>(impl.fractional_credit / 1000)); impl.fractional_credit %= 1000;
	impl.Synchronize();
	size_t allowance = std::min(impl.credit, work_limit), left = allowance;
	while (left != 0 && !impl.jobs.empty()) {
		auto job = impl.jobs.front(); impl.jobs.pop_front();
		Progress *progress = impl.Find(job);
		if (progress == nullptr) continue;
		progress->queued = false;
		size_t slice = std::min(left, SLICE), remaining = slice;
		std::visit([&](const auto &key) {
			using Key = std::decay_t<decltype(key)>;
			if constexpr (std::is_same_v<Key, FleetKey>) impl.Process(key, impl.fleets.at(key), remaining);
			else impl.Process(key, impl.policies.at(key), remaining);
			impl.Enqueue(key, *progress);
		}, job.key);
		left -= slice - remaining;
		++impl.version; // Partial evidence can change a visible representative before completion.
	}
	impl.credit -= allowance - left; impl.diagnostic.work += allowance - left;
	if (left == 0 && !impl.jobs.empty()) ++impl.diagnostic.work_exhausted;
	return impl.version != before;
}

StationPassengerServiceDiagnostics StationPassengerServiceCache::GetDiagnostics() const
{
	auto result = this->impl->diagnostic;
	auto count = [&](const auto &entries) {
		result.entries += entries.size();
		for (const auto &[key, entry] : entries) switch (entry.state) {
			case State::Pending: ++result.pending; break;
			case State::Partial: ++result.partial; break;
			case State::Complete: ++result.complete; break;
			case State::Unavailable: ++result.unavailable; break;
		}
	};
	count(this->impl->fleets); count(this->impl->policies);
	return result;
}
