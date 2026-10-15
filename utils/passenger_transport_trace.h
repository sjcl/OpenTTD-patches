/* Measurement-only: included by copied translation units, never by the product. */
#pragma once
#include <fstream>
#include <map>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <array>
#include <tuple>

namespace PassengerTransportTrace {
struct Sample {
	uint offset;
	StationID origin, destination, location = StationID::Invalid();
	StateTicks born, waiting_since;
	bool complete_origin = true, onboard = false, position_known = false;
	VehicleID vehicle = VehicleID::Invalid();
	int x = 0, y = 0;
	uint64_t riding = 0, waiting = 0, distance = 0, boards = 0, revisits = 0, returns = 0, misses = 0, maximum_wait = 0;
	uint64_t origin_waiting = 0, transfer_waiting = 0, transfers = 0;
	std::map<VehicleID, StationID> opportunities;
	std::vector<uint16_t> stations, services, stops;
	uint64_t sample_id = 0;
	uint32_t initial_packet = 0, initial_offset = 0;
	bool initial = false, ever_onboard = false, delivered = false;
	StateTicks origin_departure = INVALID_STATE_TICKS;
};
inline std::map<CargoPacketID, std::vector<Sample>> samples;
inline std::map<CargoPacketID, StateTicks> births;
inline StateTicks births_tick = INVALID_STATE_TICKS;
inline std::map<const VehicleCargoList *, VehicleID> owners;
inline size_t fleet_size = SIZE_MAX;
inline uint64_t selected = 0, discarded = 0;
inline uint64_t complete_started = 0;
inline uint64_t initial_started = 0, assigned_packets = 0, assigned_units = 0, fresh_assigned_packets = 0, fresh_assigned_units = 0;
inline uint64_t generated_units = 0, initial_units = 0, initial_known_units = 0;
inline bool observation_started = false, recording_finished = false;
inline std::ofstream delivered, lost;
enum class Movement : uint8_t { Board, Transfer, Deliver };
using PopulationKey = std::tuple<uint32_t, uint32_t, uint32_t, uint64_t>;
struct Population {
	std::array<uint64_t, 3> all{}, known{};
	bool train = false;
	std::string group_path, vehicle_name, route;
};
inline std::map<PopulationKey, Population> population;
inline bool Enabled() { static const bool enabled = std::getenv("OPENTTD_PASSENGER_TRACE") != nullptr; return enabled; }
inline void Birth(const CargoPacket &packet)
{
	if (!Enabled() || selected >= 65536) return;
	/* OD allocation splits a freshly generated, unassigned remainder. The
	 * selected child can have a different pool-ID residue from its parent. */
	if (births_tick != _state_ticks) { births.clear(); births_tick = _state_ticks; }
	births[packet.index] = _state_ticks;
}
inline void Generation(CargoType cargo, uint count)
{
	if (Enabled() && observation_started && !recording_finished && cargo < NUM_CARGO && IsCargoInClass(cargo, CargoClass::Passengers)) generated_units += count;
}
inline void Header(std::ostream &out)
{
	out << "origin,destination,born,at,complete_origin,riding_ticks,waiting_ticks,origin_waiting_ticks,transfer_waiting_ticks,distance_x16,boardings,transfers,revisits,returns,observed_misses,maximum_wait,services,stop_counts,stations,sample_id,initial,initial_packet,initial_offset,origin_departure,onboard,location\n";
}
inline void Row(std::ostream &out, const Sample &s)
{
	out << s.origin.base() << ',' << s.destination.base() << ',' << s.born.base() << ',' << _state_ticks.base() << ',' << s.complete_origin
		<< ',' << s.riding << ',' << s.waiting << ',' << s.origin_waiting << ',' << s.transfer_waiting
		<< ',' << s.distance << ',' << s.boards << ',' << s.transfers
		<< ',' << s.revisits << ',' << s.returns << ',' << s.misses << ',' << s.maximum_wait;
	for (const auto *list : {&s.services, &s.stops, &s.stations}) {
		out << ','; bool first = true;
		for (auto value : *list) { if (!first) out << ';'; out << value; first = false; }
	}
	out << ',' << s.sample_id << ',' << s.initial << ',' << s.initial_packet << ',' << s.initial_offset
		<< ',' << s.origin_departure.base() << ',' << s.onboard << ',' << s.location.base() << '\n';
}
inline void RecordLoss(const Sample &s)
{
	if (!observation_started || recording_finished || s.delivered) return;
	if (!lost.is_open()) { lost.open("transport-lost.csv"); Header(lost); }
	Row(lost, s); lost.flush();
}
inline bool Selected(const CargoPacket &packet)
{
	uint64_t key = packet.index.base();
	if (auto birth = births.find(packet.index); birth != births.end() && birth->second == _state_ticks) key ^= uint64_t(birth->second.base()) * 0x9e3779b97f4a7c15ULL;
	key = (key ^ (key >> 30)) * 0xbf58476d1ce4e5b9ULL;
	key = (key ^ (key >> 27)) * 0x94d049bb133111ebULL;
	return ((key ^ (key >> 31)) & 63) == 0;
}
inline void Start(const CargoPacket &packet, bool complete = true)
{
	if (!Enabled() || recording_finished || !Station::IsValidID(packet.GetPassengerDestination())) return;
	if (complete) {
		++assigned_packets; assigned_units += packet.Count();
		if (births.contains(packet.index) && births.at(packet.index) == _state_ticks) { ++fresh_assigned_packets; fresh_assigned_units += packet.Count(); }
	}
	if (samples.contains(packet.index) || !Selected(packet)) return;
	if (selected >= 65536) { ++discarded; return; }
	++selected;
	Sample sample{static_cast<uint>(packet.Count() / 2), packet.GetFirstStation(), packet.GetPassengerDestination(), packet.GetFirstStation(), _state_ticks, _state_ticks};
	sample.complete_origin = complete && births.contains(packet.index) && births.at(packet.index) == _state_ticks;
	if (sample.complete_origin) ++complete_started;
	sample.sample_id = selected; sample.initial = !observation_started;
	if (sample.initial) ++initial_started;
	sample.initial_packet = packet.index.base(); sample.initial_offset = sample.offset;
	sample.stations.push_back(sample.origin.base()); samples[packet.index].push_back(std::move(sample));
}
inline void Wait(const CargoPacket &packet, StationID station, CargoType cargo)
{
	if (!Enabled() || recording_finished || cargo >= NUM_CARGO || !IsCargoInClass(cargo, CargoClass::Passengers) || !Station::IsValidID(station) || samples.contains(packet.index)) return;
	Start(packet, false);
	if (auto it = samples.find(packet.index); it != samples.end()) for (auto &s : it->second) {
		s.location = station; s.stations = {station.base()};
	}
}
inline void Erase(CargoPacketID id) { if (Enabled()) { if (auto it = samples.find(id); it != samples.end()) { for (const auto &s : it->second) RecordLoss(s); samples.erase(it); } births.erase(id); } }
inline void Split(const CargoPacket &source, const CargoPacket &part, uint count)
{
	if (!Enabled()) return;
	if (auto birth = births.find(source.index); birth != births.end() && birth->second == _state_ticks) births[part.index] = birth->second;
	auto it = samples.find(source.index); if (it == samples.end()) return;
	std::vector<Sample> remaining;
	for (auto &sample : it->second) {
		if (sample.offset < count) samples[part.index].push_back(std::move(sample));
		else { sample.offset -= count; remaining.push_back(std::move(sample)); }
	}
	it->second = std::move(remaining); if (it->second.empty()) samples.erase(it);
}
inline void Merge(const CargoPacket &target, const CargoPacket &source)
{
	if (!Enabled()) return;
	auto it = samples.find(source.index); if (it == samples.end()) return;
	for (auto &sample : it->second) { sample.offset += target.Count(); samples[target.index].push_back(std::move(sample)); }
	samples.erase(it);
}
inline void Reduce(const CargoPacket &packet, uint count)
{
	if (!Enabled()) return;
	auto it = samples.find(packet.index); if (it == samples.end()) return;
	std::erase_if(it->second, [count](Sample &s) { if (s.offset < count) { RecordLoss(s); return true; } s.offset -= count; return false; });
	if (it->second.empty()) samples.erase(it);
}
inline VehicleID Owner(const VehicleCargoList *cargo)
{
	auto it = owners.find(cargo);
	if (fleet_size != Vehicle::GetNumItems() || it == owners.end()) {
		owners.clear(); fleet_size = Vehicle::GetNumItems();
		for (const Vehicle *vehicle : Vehicle::Iterate()) owners.emplace(&vehicle->cargo, vehicle->index);
		it = owners.find(cargo);
	}
	return it == owners.end() ? VehicleID::Invalid() : it->second;
}
inline void CountPopulation(const CargoPacket &packet, uint count, const VehicleCargoList *cargo, Movement movement)
{
	if (cargo == nullptr) return;
	const auto *part = Vehicle::GetIfValid(Owner(cargo));
	if (part == nullptr || part->cargo_type >= NUM_CARGO || !IsCargoInClass(part->cargo_type, CargoClass::Passengers)) return;
	const auto *front = part->First();
	const auto service = front->orders != nullptr ? front->orders->index : OrderListID::Invalid();
	const uint64_t incarnation = front->orders != nullptr ? front->orders->GetCargoDistRouteGeneration() : 0;
	auto [it, inserted] = population.try_emplace(PopulationKey{front->index.base(), service.base(), front->group_id.base(), incarnation});
	auto &row = it->second;
	if (inserted) {
		row.train = front->type == VehicleType::Train; row.vehicle_name = front->name;
		for (const Group *group = Group::GetIfValid(front->group_id); group != nullptr; group = Group::GetIfValid(group->parent)) {
			if (!row.group_path.empty()) row.group_path += '/'; row.group_path += group->name;
		}
		for (VehicleOrderID i = 0; front->orders != nullptr && i < front->GetNumOrders(); ++i) {
			const Order *order = front->GetOrder(i);
			if (!order->IsType(OT_GOTO_STATION)) continue;
			if (!row.route.empty()) row.route += ';'; row.route += std::to_string(order->GetDestination().base());
		}
	}
	const auto index = static_cast<size_t>(movement);
	row.all[index] += count;
	if (Station::IsValidID(packet.GetPassengerDestination())) row.known[index] += count;
}
inline void Position(Sample &s)
{
	const auto *v = Vehicle::GetIfValid(s.vehicle); if (v == nullptr) return;
	if (s.position_known) s.distance += std::abs(v->x_pos - s.x) + std::abs(v->y_pos - s.y);
	s.x = v->x_pos; s.y = v->y_pos; s.position_known = true;
}
inline void BeginObservation()
{
	if (!Enabled() || observation_started || recording_finished) return;
	initial_units = initial_known_units = 0;
	for (const Station *station : Station::Iterate()) for (CargoType cargo : EnumRange(NUM_CARGO)) {
		if (!IsCargoInClass(cargo, CargoClass::Passengers) || station->goods[cargo].data == nullptr) continue;
		for (const auto &[via, packets] : *station->goods[cargo].data->cargo.Packets()) for (const auto *packet : packets) {
			initial_units += packet->Count(); if (Station::IsValidID(packet->GetPassengerDestination())) initial_known_units += packet->Count();
			Wait(*packet, station->index, cargo);
		}
	}
	for (const Vehicle *part : Vehicle::Iterate()) {
		if (part->cargo_type >= NUM_CARGO || !IsCargoInClass(part->cargo_type, CargoClass::Passengers)) continue;
		uint stored = part->cargo.StoredCount();
		for (const auto *packet : *part->cargo.Packets()) {
			initial_units += packet->Count(); if (Station::IsValidID(packet->GetPassengerDestination())) initial_known_units += packet->Count();
			const uint aboard = std::min<uint>(stored, packet->Count()); stored -= aboard;
			Start(*packet, false);
			if (auto it = samples.find(packet->index); it != samples.end()) for (auto &s : it->second) {
				s.vehicle = part->index; s.onboard = s.offset < aboard; s.ever_onboard = s.onboard;
				s.location = part->First()->last_station_visited; s.stations.clear();
				if ((!s.onboard || part->First()->current_order.IsAnyLoadingType()) && Station::IsValidID(s.location)) s.stations.push_back(s.location.base());
				if (s.onboard) Position(s);
			}
		}
	}
	selected = initial_started = complete_started = 0;
	assigned_packets = assigned_units = fresh_assigned_packets = fresh_assigned_units = 0;
	generated_units = 0;
	for (auto it = samples.begin(); it != samples.end();) {
		const auto *packet = CargoPacket::GetIfValid(it->first);
		if (packet == nullptr || !Station::IsValidID(packet->GetPassengerDestination())) { it = samples.erase(it); continue; }
		for (auto &s : it->second) {
			s.sample_id = ++selected; ++initial_started; s.initial = true; s.complete_origin = false;
			s.born = s.waiting_since = _state_ticks;
			s.riding = s.waiting = s.origin_waiting = s.transfer_waiting = s.distance = s.boards = s.transfers = s.revisits = s.returns = s.misses = s.maximum_wait = 0;
			s.services.clear(); s.stops.clear(); s.opportunities.clear(); s.delivered = false;
			s.origin_departure = INVALID_STATE_TICKS;
			s.ever_onboard = s.onboard || packet->GetPassengerArrivingService() != OrderListID::Invalid() || s.location != s.origin;
		}
		++it;
	}
	births.clear(); births_tick = _state_ticks;
	observation_started = true;
}
inline void Attach(const CargoPacket &packet, const VehicleCargoList *cargo)
{
	if (!Enabled()) return;
	const auto *part = Vehicle::GetIfValid(Owner(cargo));
	if (part == nullptr || part->cargo_type >= NUM_CARGO || !IsCargoInClass(part->cargo_type, CargoClass::Passengers)) return;
	Start(packet, false);
	auto it = samples.find(packet.index); if (it == samples.end()) return;
	const auto vehicle = Owner(cargo);
	for (auto &s : it->second) {
		if (s.vehicle != vehicle) { s.vehicle = vehicle; s.position_known = false; }
		if (s.onboard) {
			Position(s);
			const auto *v = Vehicle::GetIfValid(vehicle);
			if (v != nullptr && !s.services.empty() && s.services.back() == OrderListID::Invalid().base()) {
				s.services.back() = v->First()->orders ? v->First()->orders->index.base() : OrderListID::Invalid().base();
				s.stops.back() = static_cast<uint16_t>(v->First()->GetNumOrders());
			}
		}
	}
}
inline void Location(const CargoPacket &packet, TileIndex tile)
{
	if (!Enabled()) return;
	auto it = samples.find(packet.index); if (it == samples.end()) return;
	if (IsValidTile(tile) && IsTileType(tile, TileType::Station)) for (auto &s : it->second) s.location = GetStationIndex(tile);
}
inline void Board(const CargoPacket &packet, uint offset, uint count, const VehicleCargoList *cargo)
{
	if (!Enabled()) return;
	CountPopulation(packet, count, cargo, Movement::Board);
	const auto *carrier = Vehicle::GetIfValid(Owner(cargo));
	if (carrier == nullptr || carrier->cargo_type >= NUM_CARGO || !IsCargoInClass(carrier->cargo_type, CargoClass::Passengers)) return;
	Start(packet, false);
	auto it = samples.find(packet.index); if (it == samples.end()) return;
	for (auto &s : it->second) if (s.offset >= offset && s.offset - offset < count) {
		s.opportunities.clear();
		if (!s.ever_onboard && carrier != nullptr && carrier->First()->last_station_visited == s.origin) s.origin_departure = _state_ticks;
		s.ever_onboard = true;
		s.onboard = true; ++s.boards; s.position_known = false;
		s.maximum_wait = std::max<uint64_t>(s.maximum_wait, (_state_ticks - s.waiting_since).base());
		if (carrier != nullptr) s.vehicle = carrier->index;
		else if (packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey) s.vehicle = packet.GetPassengerJourney().vehicle;
		const auto *v = Vehicle::GetIfValid(s.vehicle);
		s.services.push_back(v != nullptr && v->First()->orders ? v->First()->orders->index.base() : OrderListID::Invalid().base());
		s.stops.push_back(v != nullptr ? static_cast<uint16_t>(v->First()->GetNumOrders()) : 0);
		Position(s);
	}
}
inline void Visit(Sample &s, uint16_t station)
{
	if (station != StationID::Invalid().base()) s.location = StationID{station};
	if (station == StationID::Invalid().base() || (!s.stations.empty() && s.stations.back() == station)) return;
	if (std::find(s.stations.begin(), s.stations.end(), station) != s.stations.end()) ++s.revisits;
	if (s.stations.size() >= 2 && s.stations[s.stations.size() - 2] == station) ++s.returns;
	s.stations.push_back(station);
}
inline void Arrive(const CargoPacket &packet, uint count, bool final, const VehicleCargoList *cargo)
{
	if (!Enabled()) return;
	CountPopulation(packet, count, cargo, final ? Movement::Deliver : Movement::Transfer);
	const auto *carrier = Vehicle::GetIfValid(Owner(cargo));
	auto it = samples.find(packet.index); if (it == samples.end()) return;
	for (auto &s : it->second) if (s.offset < count) {
		Position(s); s.onboard = false; s.waiting_since = _state_ticks;
		if (carrier != nullptr) s.location = carrier->First()->last_station_visited;
		if (!final) ++s.transfers;
		Visit(s, s.location.base());
		if (final) {
			s.delivered = true;
			if (!delivered.is_open()) { delivered.open("transport-delivered.csv"); Header(delivered); }
			Row(delivered, s); delivered.flush();
		}
	}
}
inline void Opportunity(const CargoPacket &packet, const Vehicle *vehicle, bool allowed)
{
	if (!Enabled() || !allowed || vehicle == nullptr) return;
	auto it = samples.find(packet.index); if (it == samples.end()) return;
	bool room = false;
	for (const auto *v = vehicle->First(); v != nullptr; v = v->Next()) if (v->cargo_type == vehicle->cargo_type && v->cargo_cap > v->cargo.RemainingCount()) { room = true; break; }
	if (room) for (auto &s : it->second) if (!s.onboard) s.opportunities[vehicle->First()->index] = vehicle->First()->last_station_visited;
}
inline void Tick()
{
	if (!Enabled()) return;
	for (auto &[id, group] : samples) for (auto &s : group) {
		if (s.onboard) {
			++s.riding; Position(s);
			if (const auto *v = Vehicle::GetIfValid(s.vehicle)) if (v->First()->current_order.IsAnyLoadingType()) Visit(s, v->First()->last_station_visited.base());
		} else {
			++s.waiting;
			if (!s.ever_onboard) ++s.origin_waiting; else ++s.transfer_waiting;
			s.maximum_wait = std::max<uint64_t>(s.maximum_wait, (_state_ticks - s.waiting_since).base());
			for (auto it = s.opportunities.begin(); it != s.opportunities.end();) {
				const auto *v = Vehicle::GetIfValid(it->first);
				if (v == nullptr || !v->current_order.IsAnyLoadingType() || v->last_station_visited != it->second) {
					s.misses += v != nullptr; it = s.opportunities.erase(it);
				} else ++it;
			}
		}
	}
}
inline void Dump()
{
	if (!Enabled()) return;
	std::ofstream out("transport-pending.csv"); Header(out);
	for (const auto &[id, group] : samples) for (const auto &s : group) Row(out, s);
	std::ofstream fleet("transport-services.csv");
	fleet << "service,vehicle,order_count,group_id,group_path,vehicle_name,route_stations\n";
	auto quoted = [](std::ostream &stream, std::string_view value) {
		stream << '"'; for (char c : value) { if (c == '"') stream << '"'; stream << c; } stream << '"';
	};
	for (const Vehicle *v : Vehicle::Iterate()) if (v == v->First() && v->orders != nullptr) {
		std::string path;
		for (const Group *group = Group::GetIfValid(v->group_id); group != nullptr; group = Group::GetIfValid(group->parent)) {
			if (!path.empty()) path += '/'; path += group->name;
		}
		fleet << v->orders->index.base() << ',' << v->index.base() << ',' << v->GetNumOrders() << ',' << v->group_id.base() << ',';
		quoted(fleet, path); fleet << ','; quoted(fleet, v->name); fleet << ',';
		bool first = true;
		for (VehicleOrderID i = 0; i < v->GetNumOrders(); ++i) {
			const Order *order = v->GetOrder(i);
			if (!order->IsType(OT_GOTO_STATION)) continue;
			if (!first) fleet << ';'; fleet << order->GetDestination().base(); first = false;
		}
		fleet << '\n';
	}
	std::ofstream counts("transport-population.csv");
	counts << "vehicle,service,group_id,incarnation,train,boarded,transferred,delivered,known_od_boarded,known_od_transferred,known_od_delivered,group_path,vehicle_name,route_stations\n";
	for (const auto &[key, row] : population) {
		counts << std::get<0>(key) << ',' << std::get<1>(key) << ',' << std::get<2>(key) << ',' << std::get<3>(key) << ',' << row.train;
		for (auto count : row.all) counts << ',' << count;
		for (auto count : row.known) counts << ',' << count;
		counts << ','; quoted(counts, row.group_path); counts << ','; quoted(counts, row.vehicle_name); counts << ','; quoted(counts, row.route); counts << '\n';
	}
	uint64_t remaining_units = 0, remaining_known_units = 0;
	auto add = [&](const CargoPacket &packet) { remaining_units += packet.Count(); if (Station::IsValidID(packet.GetPassengerDestination())) remaining_known_units += packet.Count(); };
	for (const Station *station : Station::Iterate()) for (CargoType cargo : EnumRange(NUM_CARGO)) {
		if (!IsCargoInClass(cargo, CargoClass::Passengers) || station->goods[cargo].data == nullptr) continue;
		for (const auto &[via, packets] : *station->goods[cargo].data->cargo.Packets()) for (const auto *packet : packets) add(*packet);
	}
	for (const Vehicle *part : Vehicle::Iterate()) if (part->cargo_type < NUM_CARGO && IsCargoInClass(part->cargo_type, CargoClass::Passengers)) for (const auto *packet : *part->cargo.Packets()) add(*packet);
	std::ofstream meta("transport-trace-meta.txt"); meta << "selected=" << selected << "\nfresh_selected=" << complete_started << "\ninitial_selected=" << initial_started
		<< "\nassigned_packets=" << assigned_packets << "\nassigned_units=" << assigned_units << "\nfresh_assigned_packets=" << fresh_assigned_packets << "\nfresh_assigned_units=" << fresh_assigned_units
		<< "\ninitial_passengers=" << initial_units << "\ninitial_known_passengers=" << initial_known_units << "\ngenerated_passengers=" << generated_units << "\nremaining_passengers=" << remaining_units << "\nremaining_known_passengers=" << remaining_known_units
		<< "\ndiscarded=" << discarded << "\nsampling=deterministic hash of packet ID and current birth tick; approximately 1/64; maximum 65536 sample starts\n";
	recording_finished = true;
}
}
