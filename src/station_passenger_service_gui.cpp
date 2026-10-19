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
	Evidence evidence;
	uint64_t serial = 0, due = 0, source_revision = 0;
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

/** Compact published results are independent of evictable search workspaces. */
struct Result : Progress {
	StationPassengerService display{GroupID::Invalid(), false, StationID::Invalid(), DisplayLabel::Checking};
	bool settled = false, refreshing = false;
	uint64_t seen = 0, used = 0, epoch = 0, destination_revision = 0, route_generation = 0, service_generation = 0;
	StateTicks checked_tick{};
	size_t pins = 0;
	VehicleID first = VehicleID::Invalid();
	GroupID first_group = GroupID::Invalid();
	uint vehicle_count = 0;
};
constexpr size_t RESULT_BYTES = sizeof(Result) + sizeof(JobKey) + 128; // map node, allocator and eviction clock allowance
constexpr size_t RESULT_BUDGET = 16 * 1024 * 1024;

Evidence VisibleEvidence(const Progress &p) { return p.evidence; }

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
	std::map<JobKey, Result> results;
	std::deque<JobKey> result_clock;
	size_t result_capacity, policy_results = 0;
	bool capacity_pressure = false;
	uint64_t frame = 0, input_revision = 0, all_revision = 0;
	std::map<OrderListID, uint64_t> fleet_revisions;
	std::map<FleetKey, Fleet> fleets;
	std::map<PolicyKey, Policy> policies;
	std::deque<Job> jobs;
	std::map<StationID, uint64_t> destination_revisions;
	StationPassengerServiceDiagnostics diagnostic;
	std::array<size_t, 4> states{};
	uint64_t now = 0, serial = 0, lookup_epoch = 0, changes = 0, policy_epoch = 1, version = 0;
	size_t credit = MAX_WORK;
	uint64_t fractional_credit = 0;

	explicit Impl(size_t budget) : result_capacity(std::clamp(budget, RESULT_BYTES, RESULT_BUDGET) / RESULT_BYTES)
	{}

	uint64_t SourceRevision(const FleetKey &key) const
	{
		auto it = this->fleet_revisions.find(key.service);
		return it == this->fleet_revisions.end() ? this->all_revision : std::max(this->all_revision, it->second);
	}

	Result *UseResult(const JobKey &key, bool touch = true)
	{
		auto it = this->results.find(key);
		if (it == this->results.end()) {
			const bool policy = std::holds_alternative<PolicyKey>(key);
			// Leave bounded room for a policy's fleet dependencies, even before any work runs.
			const size_t policy_limit = this->result_capacity - std::min(MAX_ENTRIES, std::max(size_t{1}, this->result_capacity / 4));
			const bool policy_full = policy && this->policy_results >= policy_limit;
			if (policy_full || this->results.size() >= this->result_capacity) {
				// Never churn the currently referenced working set. Excess conditions get a stable
				// update-waiting label without spending work on results we cannot retain.
				bool room = false;
				for (size_t i = 0; i < 64 && !this->result_clock.empty(); ++i) {
					auto old = this->result_clock.front(); this->result_clock.pop_front();
					auto found = this->results.find(old);
					if (found == this->results.end()) continue;
					const auto *fleet = std::get_if<FleetKey>(&old);
					const bool dependency = fleet != nullptr && fleet->policy;
					if ((!policy_full || std::holds_alternative<PolicyKey>(old)) && found->second.pins == 0 &&
							(found->second.seen + 1 < this->frame || dependency) && !found->second.refreshing) {
						--this->states[static_cast<size_t>(found->second.state)];
						if (!dependency && found->second.settled && found->second.seen + 1 >= this->frame) ++this->diagnostic.capacity_regressions;
						if (std::holds_alternative<PolicyKey>(old)) --this->policy_results;
						this->results.erase(found); ++this->diagnostic.result_evictions; room = true; break;
					}
					this->result_clock.push_back(old);
				}
				if (!room) { ++this->diagnostic.capacity_waits; this->capacity_pressure = true; return nullptr; }
			}
			it = this->results.try_emplace(key).first; if (policy) ++this->policy_results; ++this->states[static_cast<size_t>(State::Pending)]; this->result_clock.push_back(key);
		} else ++this->diagnostic.cache_hits;
		if (touch) { it->second.seen = this->frame; it->second.used = this->now; }
		return &it->second;
	}

	void Display(Result &result, StationPassengerService display)
	{
		if (result.display == display) return;
		if (result.settled && display.label == DisplayLabel::Checking && display.group == GroupID::Invalid()) ++this->diagnostic.display_regressions;
		result.display = display;
		if (result.seen + 1 >= this->frame) ++this->version;
	}

	void LoseRepresentative(Result &result)
	{
		if (result.evidence.group != GroupID::Invalid()) ++this->diagnostic.representative_lost;
		result.evidence = {};
		this->Display(result, {GroupID::Invalid(), false, StationID::Invalid(), DisplayLabel::Updating});
	}

	template <typename Key> void Publish(const Key &key, const Progress &progress, const Evidence &evidence)
	{
		auto it = this->results.find(JobKey{key}); if (it == this->results.end()) return;
		auto &result = it->second;
		const bool settled = progress.state == State::Complete || progress.state == State::Unavailable;
		if (settled || !result.settled || (result.display.group == GroupID::Invalid() && evidence.group != GroupID::Invalid())) {
			auto display = Summary(progress, evidence);
			// A retry is internal work, not a new initial search. Keep settled failure labels.
			if (!settled && result.settled && display.group == GroupID::Invalid()) display = result.display;
			if (display.group != GroupID::Invalid() && result.display.group == GroupID::Invalid()) ++this->diagnostic.resolved;
			if (!settled && display.group != GroupID::Invalid()) result.evidence = evidence;
			this->Display(result, display);
		}
		if (!settled) {
			if (!result.settled && result.state != progress.state) { --this->states[static_cast<size_t>(result.state)]; result.state = progress.state; ++this->states[static_cast<size_t>(result.state)]; }
			return;
		}
		--this->states[static_cast<size_t>(result.state)]; ++this->states[static_cast<size_t>(progress.state)];
		result.evidence = evidence; result.state = progress.state; result.settled = true; result.refreshing = false;
		result.checked_tick = _state_ticks;
		result.due = this->now + (progress.state == State::Unavailable ? 2000 : 30000) + progress.serial % 10000;
		if constexpr (std::is_same_v<Key, FleetKey>) {
			const auto &fleet = this->fleets.at(key);
			result.first = fleet.first; result.vehicle_count = fleet.vehicle_count; result.route_generation = fleet.route_generation;
			const auto *list = OrderList::GetIfValid(key.service);
			const auto *first = list == nullptr ? nullptr : list->GetFirstSharedVehicle();
			result.first_group = first == nullptr ? GroupID::Invalid() : first->group_id;
			result.first = first == nullptr ? VehicleID::Invalid() : first->index;
			if (list != nullptr) { result.vehicle_count = list->GetNumVehicles(); result.route_generation = list->GetCargoDistRouteGeneration(); }
		}
	}

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
		if (latest - this->changes > PassengerServiceDatabase::HYPERPATH_DESTINATION_LOG) {
			++this->policy_epoch; this->destination_revisions.clear();
		} else {
			for (uint64_t n = this->changes; n < latest; ++n) this->destination_revisions[db.HyperpathChangedDestination(n)] = n + 1;
			if (this->destination_revisions.size() > MAX_ENTRIES * 2) { ++this->policy_epoch; this->destination_revisions.clear(); }
		}
		this->lookup_epoch = epoch; this->changes = latest;
	}

	void Restart(Fleet &entry)
	{
		entry.evidence = {}; entry.state = State::Pending; entry.started = false; entry.phase = Phase::Vehicle;
		entry.next = entry.part = entry.capacity_part = VehicleID::Invalid(); entry.stop_started = false;
		++this->diagnostic.refreshes;
	}

	void ReleaseWaiting(Policy &entry)
	{
		if (entry.waiting_held && entry.waiting) {
			auto it = this->results.find(JobKey{*entry.waiting});
			if (it != this->results.end() && it->second.pins != 0) --it->second.pins;
		}
		entry.waiting.reset(); entry.waiting_held = false;
	}

	void Restart(Policy &entry)
	{
		entry.evidence = {}; entry.state = State::Pending; entry.cursor = {}; this->ReleaseWaiting(entry);
		entry.saw_candidate = entry.uncertain = false;
		entry.epoch = this->policy_epoch; entry.destination_revision = this->Revision(entry.intent.alighting);
		++this->diagnostic.refreshes;
	}

	void Settle(Progress &entry, State state)
	{
		entry.state = state;
		if (state == State::Unavailable) entry.evidence = {};
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
		if (entry.started && entry.route_generation != orders->GetCargoDistRouteGeneration()) this->Restart(entry);
		if (entry.started && (entry.first != (first == nullptr ? VehicleID::Invalid() : first->index) || entry.vehicle_count != orders->GetNumVehicles())) this->Restart(entry);
		const bool fixed = key.kind != PassengerRideKind::ServiceIntent;
		const Vehicle *v = !entry.started ? (fixed ? Vehicle::GetIfValid(key.vehicle) : first) : Vehicle::GetIfValid(entry.next);
		if (!entry.started) {
			entry.first = first == nullptr ? VehicleID::Invalid() : first->index;
			entry.vehicle_count = orders->GetNumVehicles(); entry.route_generation = orders->GetCargoDistRouteGeneration(); entry.started = true;
			if (v == nullptr) { --work; ++this->diagnostic.no_shared_vehicle; this->Settle(entry, State::Unavailable); return; }
		} else if (v == nullptr && entry.next != VehicleID::Invalid()) {
			--work; ++this->diagnostic.invalid_vehicle; this->Restart(entry); return;
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

	Progress *GetFleet(const FleetKey &key, const PassengerJourneyExecution &ride, bool immediate, bool touch = true)
	{
		Result *result = this->UseResult(key, touch); if (result == nullptr) return nullptr;
		result->service_generation = ride.generation; result->evidence.generation = ride.generation;
		const OrderList *list = OrderList::GetIfValid(key.service);
		const Vehicle *first = list == nullptr ? nullptr : list->GetFirstSharedVehicle();
		bool stale = list == nullptr || (key.kind != PassengerRideKind::VehicleSnapshot && list->GetCargoDistRouteGeneration() != key.signature);
		bool changed = result->settled && !stale && (result->route_generation != list->GetCargoDistRouteGeneration() || result->vehicle_count != list->GetNumVehicles() ||
				result->first != (first == nullptr ? VehicleID::Invalid() : first->index) || result->first_group != (first == nullptr ? GroupID::Invalid() : first->group_id));
		bool invalid = !ValidRepresentative(result->evidence);
		bool source = result->source_revision != this->SourceRevision(key);
		if (!stale && result->evidence.usable) {
			const Order *board = key.board < list->GetNumOrders() ? list->GetOrderAt(key.board) : nullptr;
			const Order *alight = key.alight < list->GetNumOrders() ? list->GetOrderAt(key.alight) : nullptr;
			source |= board == nullptr || !IsPassengerStationStop(*board) || board->GetDestination() != key.boarding || board->GetCargoLoadType(key.cargo) == OrderLoadType::NoLoad ||
					(key.alight != INVALID_VEH_ORDER_ID && (alight == nullptr || !IsPassengerStationStop(*alight) || alight->GetDestination() != key.alighting || alight->GetCargoUnloadType(key.cargo) == OrderUnloadType::NoUnload));
		}
		bool periodic = result->settled && this->now >= result->due && _state_ticks != result->checked_tick;
		bool retry = result->settled && result->state == State::Unavailable && this->now >= result->due;
		if ((stale || invalid) && result->display.group != GroupID::Invalid()) this->LoseRepresentative(*result);
		if (!result->refreshing && result->settled && !changed && !invalid && !source && !periodic && !retry && !stale) return result;
		// A known stale source cannot become usable through the GUI timer alone.
		if (stale && result->settled && result->state == State::Unavailable && !source) return result;
		auto it = this->fleets.find(key);
		const bool new_search = it == this->fleets.end();
		if (new_search) {
			if (result->settled) ++this->diagnostic.refreshes;
			if (this->fleets.size() >= MAX_ENTRIES) return result;
			it = this->fleets.try_emplace(key).first; it->second.serial = ++this->serial;
		} else if (it->second.state == State::Complete || it->second.state == State::Unavailable || source || invalid) this->Restart(it->second);
		if (!result->refreshing && periodic) ++this->diagnostic.periodic_revalidations;
		result->source_revision = this->SourceRevision(key); result->refreshing = true;
		it->second.ride = ride;
		if (new_search && immediate && !result->settled && this->credit != 0) {
			size_t allowance = std::min(this->credit, SLICE), left = allowance;
			this->Process(key, it->second, left); this->credit -= allowance - left; this->diagnostic.work += allowance - left;
			this->Publish(key, it->second, it->second.evidence);
		}
		if (it->second.state == State::Complete || it->second.state == State::Unavailable) {
			this->fleets.erase(it); ++this->diagnostic.search_releases; return result;
		}
		this->Enqueue(key, it->second); return &it->second;
	}

	void Process(const PolicyKey &key, Policy &entry, size_t &work)
	{
		if (entry.epoch != this->policy_epoch || entry.destination_revision != this->Revision(entry.intent.alighting)) this->Restart(entry);
		auto &db = GetPassengerServiceDatabase();
		const auto [cargo, generation, station, destination, previous, failed] = key;
		while (work > 0) {
			if (!db.IsCurrent(entry.intent)) { --work; ++this->diagnostic.stale_policy; entry.evidence = {}; this->ReleaseWaiting(entry); this->Settle(entry, State::Unavailable); return; }
			if (entry.waiting) {
				--work;
				if (!db.IsCurrent(entry.current)) { ++this->diagnostic.stale_service; entry.uncertain = true; }
				else {
					Progress *fleet = this->GetFleet(*entry.waiting, entry.current, false);
					if (fleet == nullptr) return;
					if (!entry.waiting_held) { ++this->results.at(JobKey{*entry.waiting}).pins; entry.waiting_held = true; }
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

	Result *GetPolicy(const PolicyKey &key, const PassengerJourneyExecution &intent, bool touch = true)
	{
		Result *result = this->UseResult(key, touch); if (result == nullptr) return nullptr;
		const bool current = GetPassengerServiceDatabase().IsCurrent(intent);
		const bool policy = result->epoch != this->policy_epoch || result->destination_revision != this->Revision(intent.alighting);
		const bool source = result->source_revision != this->input_revision;
		const bool invalid = !ValidRepresentative(result->evidence);
		const bool periodic = result->settled && this->now >= result->due && _state_ticks != result->checked_tick;
		const bool retry = result->settled && result->state == State::Unavailable && this->now >= result->due;
		if (result->display.group != GroupID::Invalid()) {
			if (invalid) this->LoseRepresentative(*result);
			else if (!current || policy) {
				result->evidence = {};
				this->Display(*result, {GroupID::Invalid(), false, StationID::Invalid(), DisplayLabel::Updating});
			}
		}
		if (result->settled && !result->refreshing && !policy && !source && !invalid && !periodic && !retry && current) return result;
		if (!current && result->settled && result->state == State::Unavailable && !policy && !source) return result;
		auto it = this->policies.find(key);
		if (it == this->policies.end()) {
			if (this->policies.size() >= MAX_ENTRIES) return result;
			it = this->policies.try_emplace(key).first; it->second.serial = ++this->serial;
			it->second.intent = intent;
			this->Restart(it->second);
		} else if (it->second.state == State::Complete || it->second.state == State::Unavailable || policy || source || invalid) this->Restart(it->second);
		if (!result->refreshing) {
			if (policy && result->settled) ++this->diagnostic.policy_revalidations;
			else if (periodic) ++this->diagnostic.periodic_revalidations;
		}
		result->epoch = this->policy_epoch; result->destination_revision = this->Revision(intent.alighting);
		result->source_revision = this->input_revision; result->refreshing = true;
		this->Enqueue(key, it->second); return result;
	}

	StationPassengerService ResolvePolicy(CargoType cargo, const CargoPacket &packet)
	{
		this->Synchronize(); const auto &intent = packet.GetPassengerJourney();
		if (intent.signature != uint64_t(cargo) + 1) return {};
		const auto *arriving = OrderList::GetIfValid(packet.GetPassengerArrivingService());
		OrderListID previous = arriving != nullptr && arriving->GetCargoDistRouteGeneration() == packet.GetPassengerArrivingIncarnation() ? arriving->index : OrderListID::Invalid();
		PolicyKey key{cargo, intent.generation, intent.boarding, intent.alighting, previous, packet.GetPassengerFailedService(_state_ticks)};
		auto *result = this->GetPolicy(key, intent);
		return result == nullptr ? StationPassengerService{GroupID::Invalid(), false, StationID::Invalid(), DisplayLabel::Updating} : result->display;
	}

	PassengerJourneyExecution FleetRide(const FleetKey &key, const Result &result) const
	{
		PassengerJourneyExecution ride; ride.kind = key.kind; ride.service = key.service; ride.signature = key.signature;
		ride.boarding = key.boarding; ride.alighting = key.alighting; ride.boarding_order = key.board; ride.alighting_order = key.alight;
		ride.vehicle = key.vehicle; ride.generation = result.service_generation; return ride;
	}

	bool Probe(const JobKey &key)
	{
		auto it = this->results.find(key);
		if (it == this->results.end()) return true;
		// A paint has now visited all displayed conditions. Retire unused results under
		// pressure here, not halfway through building the next paint's reference set.
		if (this->capacity_pressure && it->second.seen < this->frame && !it->second.refreshing && it->second.pins == 0) {
			--this->states[static_cast<size_t>(it->second.state)];
			if (std::holds_alternative<PolicyKey>(key)) --this->policy_results;
			this->results.erase(it); ++this->diagnostic.result_evictions;
			++this->version; // Previously capacity-limited visible conditions can now be admitted.
			return true;
		}
		if (it->second.seen + 1 < this->frame) return false;
		std::visit([&](const auto &query) {
			using Key = std::decay_t<decltype(query)>;
			if constexpr (std::is_same_v<Key, FleetKey>) this->GetFleet(query, this->FleetRide(query, it->second), false, false);
			else {
				const auto [cargo, generation, station, destination, previous, failed] = query;
				PassengerJourneyExecution intent; intent.kind = PassengerRideKind::HyperpathIntent; intent.signature = uint64_t(cargo) + 1;
				intent.generation = generation; intent.boarding = station; intent.alighting = destination;
				this->GetPolicy(query, intent, false);
			}
		}, key);
		return false;
	}

};

StationPassengerServiceCache::StationPassengerServiceCache(size_t result_budget) : impl(std::make_unique<Impl>(result_budget)) {}
StationPassengerServiceCache::~StationPassengerServiceCache() = default;
void StationPassengerServiceCache::Clear() { this->impl = std::make_unique<Impl>(this->impl->result_capacity * RESULT_BYTES); }

StationPassengerService StationPassengerServiceCache::Resolve(CargoType cargo, const PassengerJourneyExecution &ride)
{
	if (cargo >= NUM_CARGO || !IsCargoInClass(cargo, CargoClass::Passengers) || ride.kind == PassengerRideKind::HyperpathIntent) return {};
	auto *entry = this->impl->GetFleet(KeyFor(cargo, ride), ride, true);
	if (entry == nullptr) return {GroupID::Invalid(), false, StationID::Invalid(), DisplayLabel::Updating};
	auto result = this->impl->results.at(JobKey{KeyFor(cargo, ride)}).display;
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
	const auto validation_before = impl.diagnostic.validation_work;
	// Cheap live-ID probes also detect edits while paused. They never enumerate fleets
	// solely because GUI time passed, and use at most a quarter of available GUI Work.
	for (size_t i = 0, probes = std::min({size_t{64}, left / 4, impl.results.size()}); i < probes && !impl.result_clock.empty(); ++i) {
		auto key = impl.result_clock.front(); impl.result_clock.pop_front();
		--left; ++impl.diagnostic.validation_work;
		if (!impl.Probe(key)) impl.result_clock.push_back(key);
	}
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
			if constexpr (std::is_same_v<Key, FleetKey>) impl.Publish(key, *progress, progress->evidence);
			else impl.Publish(key, *progress, impl.PolicyEvidence(impl.policies.at(key)));
			if (progress->state == State::Complete || progress->state == State::Unavailable) {
				if constexpr (std::is_same_v<Key, FleetKey>) impl.fleets.erase(key);
				else impl.policies.erase(key);
				++impl.diagnostic.search_releases;
			} else impl.Enqueue(key, *progress);
		}, job.key);
		left -= slice - remaining;
	}
	impl.credit -= allowance - left; impl.diagnostic.work += allowance - left - (impl.diagnostic.validation_work - validation_before);
	if (left == 0 && !impl.jobs.empty()) ++impl.diagnostic.work_exhausted;
	return impl.version != before;
}

StationPassengerServiceDiagnostics StationPassengerServiceCache::GetDiagnostics() const
{
	auto result = this->impl->diagnostic;
	result.pending = this->impl->states[static_cast<size_t>(State::Pending)];
	result.partial = this->impl->states[static_cast<size_t>(State::Partial)];
	result.complete = this->impl->states[static_cast<size_t>(State::Complete)];
	result.unavailable = this->impl->states[static_cast<size_t>(State::Unavailable)];
	result.entries = this->impl->results.size();
	result.results = this->impl->results.size(); result.result_capacity = this->impl->result_capacity;
	result.searches = this->impl->fleets.size() + this->impl->policies.size();
	result.waiting_work = this->impl->jobs.size();
	result.estimated_bytes = result.results * RESULT_BYTES + result.searches * (std::max(sizeof(Fleet), sizeof(Policy)) + sizeof(JobKey) + 128) + this->impl->jobs.size() * sizeof(Job);
	return result;
}

void StationPassengerServiceCache::BeginRefresh() { ++this->impl->frame; this->impl->capacity_pressure = false; }

void StationPassengerServiceCache::Invalidate(OrderListID service)
{
	auto &impl = *this->impl; ++impl.input_revision;
	if (service == OrderListID::Invalid() || impl.fleet_revisions.size() >= MAX_ENTRIES) {
		impl.all_revision = impl.input_revision; impl.fleet_revisions.clear();
	} else impl.fleet_revisions[service] = impl.input_revision;
}
