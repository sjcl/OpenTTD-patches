/* Opt-in copied-TU observer. No writes to packets, orders, Work or scheduler. */
#pragma once
#include "../src/passenger_journey_active.h"
#include "../src/passenger_service_database.h"
#include "../src/station_base.h"
#include "../src/vehicle_base.h"
#include "../src/cargotype.h"
#include "../src/date_func.h"
#include "../src/core/random_func.hpp"
#include <fstream>
#include <cstdlib>

namespace PassengerDepartureDiagnostic {
inline bool active = false;
inline StateTicks started{};
inline bool Enabled() { static const bool enabled = std::getenv("OPENTTD_PASSENGER_DEPARTURE_DIAGNOSTIC") != nullptr; return enabled && active; }
inline bool Target(const Vehicle &v) { return Enabled() && v.First() == &v && v.orders != nullptr && v.last_station_visited == StationID{3393} && (v.orders->index == OrderListID{524} || v.orders->index == OrderListID{525} || v.orders->index == OrderListID{528}); }
inline void Open(std::ofstream &out, const char *path, const char *header) { if (!out.is_open()) { out.open(path); out << header << '\n'; } }
struct Range { uint32_t offset, count; int64_t tick; };
using Ranges = std::map<CargoPacketID, std::vector<Range>>;
inline Ranges fresh, expired;
inline std::ofstream departures, waiting, orders, executions, patterns, replans, readonly_checks, reservations;
inline std::map<VehicleID, VehicleOrderID> last_order;
inline std::set<std::tuple<OrderListID, uint64_t, uint64_t>> published;

inline void Birth(const CargoPacket &packet) { if (Enabled() && packet.GetFirstStation() == StationID{3393}) fresh[packet.index] = {{0, packet.Count(), _state_ticks.base()}}; }
inline void Erase(CargoPacketID id) { if (Enabled()) { fresh.erase(id); expired.erase(id); } }
inline void SplitRanges(Ranges &all, const CargoPacket &source, const CargoPacket &part, uint count)
{
	all.erase(part.index); auto found = all.find(source.index); if (found == all.end()) return;
	std::vector<Range> remaining, moved;
	for (const auto &r : found->second) {
		uint32_t end = r.offset + r.count;
		if (r.offset < count) moved.push_back({r.offset, std::min<uint32_t>(end, count) - r.offset, r.tick});
		if (end > count) remaining.push_back({std::max<uint32_t>(r.offset, count) - count, end - std::max<uint32_t>(r.offset, count), r.tick});
	}
	if (remaining.empty()) all.erase(found); else found->second = std::move(remaining);
	if (!moved.empty()) all[part.index] = std::move(moved);
}
inline void Split(const CargoPacket &source, const CargoPacket &part, uint count) { if (Enabled()) { SplitRanges(fresh, source, part, count); SplitRanges(expired, source, part, count); } }
inline void Merge(const CargoPacket &target, const CargoPacket &source)
{
	if (!Enabled()) return;
	for (auto *all : {&fresh, &expired}) if (auto found = all->find(source.index); found != all->end()) {
		for (auto r : found->second) { r.offset += target.Count(); (*all)[target.index].push_back(r); }
		all->erase(found);
	}
}
inline void Reduce(const CargoPacket &packet, uint count)
{
	if (!Enabled()) return;
	for (auto *all : {&fresh, &expired}) if (auto found = all->find(packet.index); found != all->end()) {
		std::vector<Range> remaining;
		for (const auto &r : found->second) if (r.offset + r.count > count) {
			uint32_t begin = std::max<uint32_t>(r.offset, count);
			remaining.push_back({begin - count, r.offset + r.count - begin, r.tick});
		}
		if (remaining.empty()) all->erase(found); else found->second = std::move(remaining);
	}
}
inline uint64_t FreshUnits(const CargoPacket &packet, uint32_t offset = 0, uint32_t units = UINT32_MAX)
{
	uint64_t count = 0; uint32_t end = offset + std::min<uint32_t>(units, packet.Count() - offset);
	if (auto found = fresh.find(packet.index); found != fresh.end()) for (const auto &r : found->second) {
		uint32_t begin = std::max(offset, r.offset), last = std::min(end, r.offset + r.count);
		if (last > begin) count += last - begin;
	}
	return count;
}
inline void Decision(const CargoPacket &packet, const PassengerJourneyExecution *next)
{
	if (!Enabled()) return;
	const auto &old = packet.GetPassengerJourney();
	if (IsEstimatedPassengerRide(old.kind) && old.boarding == StationID{3393} && packet.GetPassengerDestination() == StationID{3394} &&
			IsPassengerScheduleIntentAged(old, _state_ticks, old.waiting_limit + 1) && !expired.contains(packet.index)) {
		expired[packet.index] = {{0, packet.Count(), old.planned_at.base() + old.waiting_limit + 1}};
	}
	fresh.erase(packet.index);
	if (next == nullptr || next->kind != PassengerRideKind::HyperpathIntent) return;
	if (auto found = expired.find(packet.index); found != expired.end()) {
		Open(replans, "target-expiry-replans.csv", "completed_tick,packet,units,expired_tick,latency_ticks");
		for (const auto &r : found->second) replans << _state_ticks.base() << ',' << packet.index.base() << ',' << r.count << ',' << r.tick << ',' << _state_ticks.base() - r.tick << '\n';
		expired.erase(found);
	}
}

struct Seats {
	uint64_t capacity = 0, occupied = 0, reserved = 0, stored = 0;
	uint64_t Free() const { return capacity > occupied ? capacity - occupied : 0; }
	uint64_t PhysicalFree() const { return capacity > stored ? capacity - stored : 0; }
};
inline std::array<Seats, NUM_CARGO> CountSeats(const Vehicle &front)
{
	std::array<Seats, NUM_CARGO> result{};
	for (const Vehicle *part = &front; part != nullptr; part = part->Next()) if (part->cargo_type < NUM_CARGO && IsCargoInClass(part->cargo_type, CargoClass::Passengers)) {
		auto &s = result[part->cargo_type]; s.capacity += part->cargo_cap; s.occupied += part->cargo.RemainingCount(); s.reserved += part->cargo.ReservedCount(); s.stored += part->cargo.StoredCount();
	}
	return result;
}
struct Stop {
	std::array<Seats, NUM_CARGO> before{};
	std::array<uint64_t, NUM_CARGO> boarded{};
	StateTicks first{}; bool partial = false;
	std::array<uint64_t, NUM_CARGO> eligible_available{}, target_reserved{}, eligible_reserved{};
};
inline std::map<VehicleID, Stop> stops;
inline std::map<const VehicleCargoList *, std::pair<VehicleID, CargoType>> owners;
inline void PublishedPatterns()
{
	if (!Enabled()) return;
	Open(patterns, "target-patterns.csv", "tick,service,incarnation,generation,period,estimated_period,fleet,stop_count,station,order,arrival,departure,board,alight,continues,travel_quality");
	for (OrderListID id : {OrderListID{524}, OrderListID{525}, OrderListID{528}}) {
		const auto *service = GetPassengerServiceDatabase().Get(id); if (service == nullptr || !published.emplace(id, service->incarnation, service->generation).second) continue;
		for (const auto &s : service->timetable.stops) patterns << _state_ticks.base() << ',' << id.base() << ',' << service->incarnation << ',' << service->generation << ',' << service->timetable.period << ',' << service->timetable.estimated_period << ',' << service->vehicles.size() << ',' << service->timetable.stops.size() << ',' << s.station.base() << ',' << s.order << ',' << s.arrival << ',' << s.departure << ',' << s.board << ',' << s.alight << ',' << s.continues << ',' << unsigned(s.travel_quality) << '\n';
	}
}
inline void BeforeLoad(const Vehicle &front)
{
	if (!Target(front)) return;
	PublishedPatterns();
	if (!stops.contains(front.index)) stops.emplace(front.index, Stop{CountSeats(front), {}, _state_ticks, _state_ticks <= started + 1});
	for (const Vehicle *part = &front; part != nullptr; part = part->Next()) owners[&part->cargo] = {front.index, part->cargo_type};
}
inline void Board(const CargoPacket &packet, uint, uint count, const VehicleCargoList *cargo)
{
	if (!Enabled() || packet.GetPassengerDestination() != StationID{3394}) return;
	if (auto owner = owners.find(cargo); owner != owners.end() && owner->second.second < NUM_CARGO) if (auto stop = stops.find(owner->second.first); stop != stops.end()) stop->second.boarded[owner->second.second] += count;
}
inline void ObserveOrder(const Vehicle &vehicle)
{
	if (!Enabled() || vehicle.First() != &vehicle || vehicle.orders == nullptr || vehicle.orders->index != OrderListID{528}) return;
	PublishedPatterns();
	auto [found, initial] = last_order.try_emplace(vehicle.index, vehicle.cur_real_order_index);
	if (!initial && found->second == vehicle.cur_real_order_index) return;
	Open(orders, "target-order-transitions.csv", "tick,vehicle,service,order,order_type,initial_observation");
	orders << _state_ticks.base() << ',' << vehicle.index.base() << ',' << vehicle.orders->index.base() << ',' << vehicle.cur_real_order_index << ',' << unsigned(vehicle.current_order.GetType()) << ',' << initial << '\n';
	found->second = vehicle.cur_real_order_index;
}
inline void ExecuteOrder(const Vehicle *v, const Order *order, int depth, bool look_ahead)
{
	if (!Enabled() || look_ahead || order == nullptr || v->First() != v || v->orders == nullptr || v->orders->index != OrderListID{528}) return;
	Open(executions, "target-order-executions.csv", "tick,vehicle,service,order,order_type,conditional_depth");
	executions << _state_ticks.base() << ',' << v->index.base() << ',' << v->orders->index.base() << ',' << v->cur_real_order_index << ',' << unsigned(order->GetType()) << ',' << depth << '\n';
}
struct OrderScope {
	VehicleID id = VehicleID::Invalid();
	explicit OrderScope(const Vehicle *v) { if (Enabled() && v->orders != nullptr && v->orders->index == OrderListID{528} && v->First() == v) { id = v->index; ObserveOrder(*v); } }
	~OrderScope() { if (id != VehicleID::Invalid()) if (const auto *v = Vehicle::GetIfValid(id)) ObserveOrder(*v); }
};

/* The copied friend diagnostic answers one exact index lookup before its
 * normal all-station census. It never calls EnsureWaitingIndex or an event. */
inline const StationCargoList *index_query = nullptr;
inline CargoPacketID index_packet = CargoPacketID::Invalid();
inline bool indexed = false, pending = false, generation_pending = false;
inline StateTicks indexed_due = INVALID_STATE_TICKS;
inline StationID indexed_key = StationID::Invalid();
inline uint64_t indexed_sequence = 0;
inline void ReadIndex(const StationCargoList &list, CargoPacketID packet)
{
	index_query = &list; index_packet = packet;
	GetPassengerWaitingEventDiagnostic();
	index_query = nullptr;
}
inline void ObserveReservations(const Vehicle &front, const Station &station, CargoType cargo, Stop &stop, PassengerJourneyLoadingContext &loading)
{
	Open(reservations, "target-reservations.csv", "tick,vehicle,service,order,cargo,reserved_for_vehicle,reserved_for_service,packet,units,for_departing_vehicle,can_board_departing_vehicle,mode,kind,current_intent,early,expired,boarding_station,planned_at,not_before,waiting_limit,policy_generation,failed_service,new_unprocessed_units");
	stop.target_reserved[cargo] = stop.eligible_reserved[cargo] = 0;
	for (const Vehicle *owner : station.loading_vehicles) for (const Vehicle *part = owner; part != nullptr; part = part->Next()) {
		if (part->cargo_type != cargo || part->cargo.ReservedCount() == 0) continue;
		uint32_t skip = part->cargo.StoredCount();
		for (const CargoPacket *packet : *part->cargo.Packets()) {
			if (skip >= packet->Count()) { skip -= packet->Count(); continue; }
			uint32_t offset = skip, count = packet->Count() - skip; skip = 0;
			if (packet->GetPassengerDestination() != StationID{3394}) continue;
			const auto intent = packet->GetPassengerJourney(); const auto failure = packet->GetPassengerBoardingFailure();
			const auto event = packet->GetPassengerRoutingEvent();
			bool journey = packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey;
			bool current = journey && intent.kind == PassengerRideKind::HyperpathIntent && GetPassengerServiceDatabase().IsCurrent(intent);
			bool aged = journey && IsPassengerScheduleIntentAged(intent, _state_ticks, intent.waiting_limit + 1);
			int mode = current ? 0 : journey ? 1 : packet->IsPassengerJourneyDeferred() ? 2 : packet->GetPassengerRoutingMode() == PassengerRoutingMode::Fallback ? 3 : 4;
			if (aged && intent.boarding == StationID{3393} && !expired.contains(packet->index)) expired[packet->index] = {{offset, count, intent.planned_at.base() + intent.waiting_limit + 1}};
			bool own = owner == &front, allowed = loading.CanBoard(*packet, &front, cargo);
			if (packet->GetPassengerJourney() != intent || packet->GetPassengerBoardingFailure() != failure || packet->GetPassengerRoutingEvent() != event) std::abort();
			if (own) { stop.target_reserved[cargo] += count; stop.eligible_reserved[cargo] += allowed ? count : 0; }
			reservations << _state_ticks.base() << ',' << front.index.base() << ',' << front.orders->index.base() << ',' << front.cur_real_order_index << ',' << unsigned(cargo) << ',' << owner->index.base() << ',' << (owner->orders == nullptr ? OrderListID::Invalid() : owner->orders->index).base() << ',' << packet->index.base() << ',' << count << ',' << own << ',' << allowed << ',' << mode << ',' << int(intent.kind) << ',' << GetPassengerServiceDatabase().IsCurrent(intent) << ',' << (_state_ticks < intent.not_before || _state_ticks < packet->GetPassengerInterchangeReady()) << ',' << aged << ',' << intent.boarding.base() << ',' << intent.planned_at.base() << ',' << intent.not_before.base() << ',' << intent.waiting_limit << ',' << intent.generation << ',' << packet->GetPassengerFailedService(_state_ticks).base() << ',' << FreshUnits(*packet, offset, count) << '\n';
		}
	}
}
struct Row { CargoPacketID packet; StationID key; uint64_t sequence, units, fresh, oldest; int64_t due; int mode, kind; bool current, early, expired, indexed, pending, generation_pending; int member, legal; bool allowed; };
struct ReadOnlyScope {
	const Vehicle &front;
	StateTicks tick = _state_ticks;
	GameRandomSeedChecker seeds;
	std::vector<uint8_t> saved = GetPassengerServiceDatabase().Save();
	explicit ReadOnlyScope(const Vehicle &front) : front(front) {}
	~ReadOnlyScope()
	{
		bool same_db = saved == GetPassengerServiceDatabase().Save(), same_rng = seeds.Check(), same_tick = tick == _state_ticks;
		Open(readonly_checks, "target-readonly-checks.csv", "tick,vehicle,service,database_save_unchanged,rng_unchanged,tick_unchanged");
		readonly_checks << tick.base() << ',' << front.index.base() << ',' << front.orders->index.base() << ',' << same_db << ',' << same_rng << ',' << same_tick << '\n';
		if (!same_db || !same_rng || !same_tick) std::abort();
	}
};
inline void BeforeDeparture(const Vehicle &front)
{
	if (!Target(front)) return;
	ReadOnlyScope unchanged(front);
	if (!stops.contains(front.index)) BeforeLoad(front);
	PublishedPatterns();
	Open(waiting, "target-waiting.csv", "tick,vehicle,service,order,cargo,packet,units,mode,kind,current_hyperpath,early,expired,indexed,pending,generation_pending,new_unprocessed_units,member,live_prefix_legal,can_board,oldest_event_ticks,loadable_left,boarding_station,planned_at,not_before,waiting_limit,policy_generation,arriving_service,interchange_ready,failed_service,primary_key,sequence,intent_service,intent_boarding_order,event_due_tick");
	Open(departures, "target-departures.csv", "tick,vehicle,service,order,cargo,phase,first_observed_tick,partial,capacity_before,free_before,reserved_before,capacity,free,reserved,boarded_target,waiting_packets,waiting_units,eligible_remaining,loadable_left,oldest_event_ticks,physical_free_before,physical_free,stored_before,stored,target_reserved,eligible_reserved,potential_loadable_after_cancel");
	const auto seats = CountSeats(front); auto &stop = stops.at(front.index);
	const Station *station = Station::Get(front.last_station_visited); auto &db = GetPassengerServiceDatabase();
	for (CargoType cargo : EnumRange(NUM_CARGO)) {
		if (!IsCargoInClass(cargo, CargoClass::Passengers) || (station->goods[cargo].data == nullptr && seats[cargo].capacity == 0 && seats[cargo].reserved == 0 && stop.before[cargo].capacity == 0)) continue;
		const StationCargoList *list = station->goods[cargo].data == nullptr ? nullptr : &station->goods[cargo].data->cargo; std::vector<Row> rows;
		PassengerJourneyLoadingContext loading; PassengerJourneyBoardingContext boarding(&front, cargo);
		ObserveReservations(front, *station, cargo, stop, loading);
		const auto occurrences = db.HyperpathBoardingOccurrences(cargo, station->index, front.orders->index, front.cur_real_order_index);
		if (list != nullptr) for (const auto &[key, packets] : *list->Packets()) for (const auto *packet : packets) {
			if (packet->GetPassengerDestination() != StationID{3394}) continue;
			ReadIndex(*list, packet->index); const auto original = packet->GetPassengerJourney();
			const auto failure_before = packet->GetPassengerBoardingFailure();
			const auto state_before = std::tuple(packet->GetPassengerRoutingMode(), packet->GetPassengerDestination(), packet->GetPassengerLegacyHop(), packet->GetPassengerVia(), packet->GetPassengerRoutingEvent(), packet->GetPassengerArrivingService(), packet->GetPassengerArrivingIncarnation(), packet->GetPassengerInterchangeReady(), packet->Count());
			bool journey = packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey;
			bool current = journey && original.kind == PassengerRideKind::HyperpathIntent && db.IsCurrent(original);
			bool early = (packet->GetPassengerArrivingService() != OrderListID::Invalid() && _state_ticks < packet->GetPassengerInterchangeReady()) || (journey && _state_ticks < original.not_before);
			bool aged = journey && IsPassengerScheduleIntentAged(original, _state_ticks, original.waiting_limit + 1);
			if (aged && IsEstimatedPassengerRide(original.kind) && original.boarding == StationID{3393} && !expired.contains(packet->index)) expired[packet->index] = {{0, packet->Count(), original.planned_at.base() + original.waiting_limit + 1}};
			int mode = current ? 0 : journey ? 1 : packet->IsPassengerJourneyDeferred() ? 2 : packet->GetPassengerRoutingMode() == PassengerRoutingMode::Fallback ? 3 : 4;
			StateTicks due = indexed ? indexed_due : packet->GetPassengerRoutingEvent();
			uint64_t age = due != INVALID_STATE_TICKS && due <= _state_ticks ? uint64_t(_state_ticks.base()) - uint64_t(due.base()) : 0;
			const auto *arriving = OrderList::GetIfValid(packet->GetPassengerArrivingService());
			OrderListID previous = arriving != nullptr && arriving->GetCargoDistRouteGeneration() == packet->GetPassengerArrivingIncarnation() ? arriving->index : OrderListID::Invalid();
			OrderListID failed = packet->GetPassengerFailedService(_state_ticks); auto intent = original;
			if (!current) { auto plan = db.PlanPrecomputed(cargo, station->index, StationID{3394}, std::max(_state_ticks, packet->GetPassengerInterchangeReady()), previous, 0, failed); intent = plan.ride.value_or(PassengerJourneyExecution{}); }
			int member = -1, legal = -1;
			if (intent.kind == PassengerRideKind::HyperpathIntent) {
				member = db.MayBoardHyperpath(intent, cargo, occurrences, previous, failed, false);
				auto ride = db.BoardHyperpath(intent, cargo, front.orders->index, front.cur_real_order_index, previous, failed).ride;
				legal = member ? -2 : -1; // No concrete ride is not a native-prefix failure.
				if (ride) { ride->not_before = ride->planned_at = _state_ticks; legal = boarding.CanBoardIntent(*ride); }
			}
			rows.push_back({packet->index, indexed ? indexed_key : key, indexed ? indexed_sequence : packet->index.base(), packet->Count(), FreshUnits(*packet), age, due.base(),
					mode, int(original.kind), current, early, aged, indexed, pending, generation_pending, member, legal, loading.CanBoard(*packet, &front, cargo)});
			if (packet->GetPassengerJourney() != original || packet->GetPassengerBoardingFailure() != failure_before || state_before != std::tuple(packet->GetPassengerRoutingMode(), packet->GetPassengerDestination(), packet->GetPassengerLegacyHop(), packet->GetPassengerVia(), packet->GetPassengerRoutingEvent(), packet->GetPassengerArrivingService(), packet->GetPassengerArrivingIncarnation(), packet->GetPassengerInterchangeReady(), packet->Count())) std::abort();
		}
		const auto last = list == nullptr ? StationID::Invalid() : list->passenger_boarding_last;
		std::sort(rows.begin(), rows.end(), [last](const Row &a, const Row &b) { return std::tuple(last != StationID::Invalid() && a.key <= last, a.key, a.sequence, a.packet) < std::tuple(last != StationID::Invalid() && b.key <= last, b.key, b.sequence, b.packet); });
		uint64_t units = 0, eligible = 0, oldest = 0, remaining = seats[cargo].Free();
		for (const auto &r : rows) {
			const auto *packet = CargoPacket::Get(r.packet); const auto &intent = packet->GetPassengerJourney();
			units += r.units; eligible += r.allowed ? r.units : 0; oldest = std::max(oldest, r.oldest);
			uint64_t missed = r.allowed ? std::min(remaining, r.units) : 0; remaining -= missed;
			waiting << _state_ticks.base() << ',' << front.index.base() << ',' << front.orders->index.base() << ',' << front.cur_real_order_index << ',' << unsigned(cargo) << ',' << r.packet.base() << ',' << r.units << ',' << r.mode << ',' << r.kind << ',' << r.current << ',' << r.early << ',' << r.expired << ',' << r.indexed << ',' << r.pending << ',' << r.generation_pending << ',' << r.fresh << ',' << r.member << ',' << r.legal << ',' << r.allowed << ',' << (r.due == INVALID_STATE_TICKS.base() ? std::string("-1") : std::to_string(r.oldest)) << ',' << missed
					<< ',' << intent.boarding.base() << ',' << intent.planned_at.base() << ',' << intent.not_before.base() << ',' << intent.waiting_limit << ',' << intent.generation << ',' << packet->GetPassengerArrivingService().base() << ',' << packet->GetPassengerInterchangeReady().base() << ',' << packet->GetPassengerFailedService(_state_ticks).base() << ',' << r.key.base() << ',' << r.sequence << ',' << intent.service.base() << ',' << intent.boarding_order << ',' << r.due << '\n';
		}
		stop.eligible_available[cargo] = eligible;
		departures << _state_ticks.base() << ',' << front.index.base() << ',' << front.orders->index.base() << ',' << front.cur_real_order_index << ',' << unsigned(cargo) << ",before_cancel," << stop.first.base() << ',' << stop.partial << ',' << stop.before[cargo].capacity << ',' << stop.before[cargo].Free() << ',' << stop.before[cargo].reserved << ',' << seats[cargo].capacity << ',' << seats[cargo].Free() << ',' << seats[cargo].reserved << ',' << stop.boarded[cargo] << ',' << rows.size() << ',' << units << ',' << eligible << ',' << seats[cargo].Free() - remaining << ',' << oldest << ',' << stop.before[cargo].PhysicalFree() << ',' << seats[cargo].PhysicalFree() << ',' << stop.before[cargo].stored << ',' << seats[cargo].stored << ',' << stop.target_reserved[cargo] << ',' << stop.eligible_reserved[cargo] << ",-1\n";
	}
}
inline void AfterCancel(const Vehicle &front)
{
	if (!Target(front)) return;
	const auto seats = CountSeats(front); const auto *station = Station::Get(front.last_station_visited);
	for (CargoType cargo : EnumRange(NUM_CARGO)) if (IsCargoInClass(cargo, CargoClass::Passengers) && stops.contains(front.index) &&
			(station->goods[cargo].data != nullptr || seats[cargo].capacity != 0 || stops.at(front.index).before[cargo].capacity != 0)) {
		uint64_t units = 0, packets = 0; const auto &stop = stops.at(front.index);
		if (station->goods[cargo].data != nullptr) for (const auto &[key, group] : *station->goods[cargo].data->cargo.Packets()) for (const auto *packet : group) if (packet->GetPassengerDestination() == StationID{3394}) { units += packet->Count(); ++packets; }
		departures << _state_ticks.base() << ',' << front.index.base() << ',' << front.orders->index.base() << ',' << front.cur_real_order_index << ',' << unsigned(cargo) << ",after_cancel," << stop.first.base() << ',' << stop.partial << ',' << stop.before[cargo].capacity << ',' << stop.before[cargo].Free() << ',' << stop.before[cargo].reserved << ',' << seats[cargo].capacity << ',' << seats[cargo].Free() << ',' << seats[cargo].reserved << ',' << stop.boarded[cargo] << ',' << packets << ',' << units << ",-1,-1,-1," << stop.before[cargo].PhysicalFree() << ',' << seats[cargo].PhysicalFree() << ',' << stop.before[cargo].stored << ',' << seats[cargo].stored << ',' << stop.target_reserved[cargo] << ',' << stop.eligible_reserved[cargo] << ',' << std::min(seats[cargo].Free(), stop.eligible_available[cargo] + stop.eligible_reserved[cargo]) << '\n';
	}
	for (const Vehicle *part = &front; part != nullptr; part = part->Next()) owners.erase(&part->cargo);
	stops.erase(front.index);
}
inline void Snapshot(const char *name)
{
	if (!Enabled()) return;
	const auto wire = GetPassengerServiceDatabase().Save(); std::ofstream out(name, std::ios::binary);
	out.write(reinterpret_cast<const char *>(wire.data()), wire.size());
}
inline void Reset() { active = std::getenv("OPENTTD_PASSENGER_DEPARTURE_DIAGNOSTIC") != nullptr; started = _state_ticks; fresh.clear(); expired.clear(); stops.clear(); owners.clear(); last_order.clear(); published.clear(); PublishedPatterns(); Snapshot("target-initial-database.bin"); }
inline void Dump()
{
	if (!Enabled()) return;
	std::ofstream censored("target-expiry-pending.csv"); censored << "tick,packet,units,expired_tick,age_ticks\n";
	for (const auto &[id, ranges] : expired) for (const auto &r : ranges) censored << _state_ticks.base() << ',' << id.base() << ',' << r.count << ',' << r.tick << ',' << _state_ticks.base() - r.tick << '\n';
	for (auto *out : {&departures, &waiting, &orders, &executions, &patterns, &replans, &readonly_checks, &reservations}) out->flush();
	Snapshot("target-final-database.bin");
	active = false;
}
}
