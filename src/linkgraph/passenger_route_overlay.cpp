/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */

/** @file passenger_route_overlay.cpp Bounded display-only propagation over saved passenger policies. */
#include "../stdafx.h"
#include "passenger_route_overlay.h"
#include "../station_base.h"
#include "../passenger_service_database.h"
#include "../passenger_journey_active.h"
#include "../date_func.h"
#include "../vehicle_base.h"

std::vector<StationID> GetPassengerRouteStops(const PassengerJourneyExecution &ride, CargoType cargo, size_t &work, size_t *usable_vehicles)
{
	if (usable_vehicles != nullptr) *usable_vehicles = 0;
	if (cargo >= NUM_CARGO || ride.kind == PassengerRideKind::HyperpathIntent) return {};
	const OrderList *list = OrderList::GetIfValid(ride.service);
	if (list == nullptr || ride.boarding_order >= list->GetNumOrders() || ride.alighting_order >= list->GetNumOrders()) return {};
	if (ride.kind == PassengerRideKind::VehicleSnapshot) {
		if (ride.signature == 0 || PassengerJourneyServiceSignature(*list, cargo, &work) != ride.signature) return {};
	} else if (list->GetCargoDistRouteGeneration() != ride.signature) return {};
	const auto &orders = list->GetOrderVector();
	const Order &boarding = orders[ride.boarding_order];
	if (!IsPassengerStationStop(boarding) || boarding.IsRefit() || boarding.GetDestination() != ride.boarding ||
			boarding.GetCargoLoadType(cargo) == OrderLoadType::NoLoad ||
			boarding.GetCargoUnloadType(cargo) == OrderUnloadType::Unload || boarding.GetCargoUnloadType(cargo) == OrderUnloadType::Transfer) return {};

	/* Keep occurrence order, including repeated stations and wraparound. Conditional
	 * orders and refits cannot certify a suffix. Forced unloading ends a through ride. */
	std::vector<std::pair<StationID, const Order *>> stops{{ride.boarding, &boarding}};
	bool complete = false;
	for (size_t step = 1; step < orders.size(); ++step) {
		if (work == 0) return {};
		--work;
		VehicleOrderID index = static_cast<VehicleOrderID>((ride.boarding_order + step) % orders.size());
		const Order &order = orders[index];
		if (order.IsType(OT_IMPLICIT) || order.IsType(OT_LABEL)) continue;
		if (order.IsType(OT_GOTO_DEPOT)) {
			if (ClassifyPassengerDepot(order) == PassengerDepotTraversal::Blocked) return {};
			continue;
		}
		if ((!order.IsType(OT_GOTO_STATION) && !order.IsType(OT_GOTO_WAYPOINT)) || order.IsRefit()) return {};
		if (!IsPassengerStationStop(order)) continue;
		StationID station = order.GetDestination().ToStationID();
		if (!Station::IsValidID(station)) return {};
		stops.emplace_back(station, &order);
		if (index == ride.alighting_order && station == ride.alighting && order.GetCargoUnloadType(cargo) != OrderUnloadType::NoUnload) { complete = true; break; }
		if (order.GetCargoUnloadType(cargo) == OrderUnloadType::Unload || order.GetCargoUnloadType(cargo) == OrderUnloadType::Transfer) return {};
	}
	if (!complete) return {};

	/* Fallback frequency counts every usable physical carrier, while fixed rides
	 * only need one. These helpers read pools and the caller's display budget. */
	size_t vehicles = 0;
	for (const Vehicle *vehicle = list->GetFirstSharedVehicle(); vehicle != nullptr; vehicle = vehicle->NextShared()) {
		if (work == 0) return {};
		--work;
		if (!IsPassengerServiceVehicle(vehicle) || vehicle->vehstatus.Any({VehState::Stopped, VehState::Crashed})) continue;
		if ((ride.kind == PassengerRideKind::VehicleSnapshot || IsBoundPassengerRide(ride.kind)) && vehicle->index != ride.vehicle) continue;
		if (!HasPassengerVehicleCapacity(vehicle, cargo, work)) continue;
		bool usable = true;
		for (const auto &[station, order] : stops) {
			if (!CanPassengerVehicleUseStation(vehicle, station, order, work)) { usable = false; break; }
		}
		if (usable && work != 0) {
			++vehicles;
			if (usable_vehicles == nullptr) break;
		}
	}
	if (vehicles == 0 || work == 0) return {};
	if (usable_vehicles != nullptr) *usable_vehicles = vehicles;
	std::vector<StationID> result;
	for (const auto &[station, order] : stops) result.push_back(station);
	return result;
}

namespace {
	constexpr double MIN_AMOUNT = 0.05;
	constexpr size_t MAX_DEPTH = 64, MAX_GROUPS = 8192;
	using PolicyKey = std::tuple<CargoType, StationID, StationID, OrderListID, OrderListID, uint64_t>;
	using RideKey = std::tuple<CargoType, OrderListID, uint64_t, VehicleID, PassengerRideKind, StationID, StationID, VehicleOrderID, VehicleOrderID>;

	/** Everything here is owned by one GUI rebuild; nothing survives in simulation caches. */
	struct RouteBuilder {
		const PassengerServiceDatabase &database;
		StationPassengerRoutes result;
		size_t work = 1 << 18;
		std::map<PolicyKey, std::vector<PassengerPrecomputedTable::RouteCandidate>> policies;
		struct RidePath { std::vector<StationID> stops; std::optional<size_t> vehicles; };
		std::map<RideKey, RidePath> rides;
		std::vector<StationID> transfers; ///< Branch-local transfer history; intermediate through stops may repeat.

		const std::vector<StationID> &Stops(CargoType cargo, const PassengerJourneyExecution &ride, size_t *usable_vehicles = nullptr)
		{
			RideKey key{cargo, ride.service, ride.signature, ride.vehicle, ride.kind, ride.boarding, ride.alighting, ride.boarding_order, ride.alighting_order};
			auto [it, inserted] = this->rides.try_emplace(key);
			if (inserted || (usable_vehicles != nullptr && !it->second.vehicles.has_value())) {
				size_t allowance = std::min(this->work, size_t{4096}), left = allowance;
				size_t vehicles = 0;
				it->second.stops = GetPassengerRouteStops(ride, cargo, left, usable_vehicles != nullptr ? &vehicles : nullptr);
				if (usable_vehicles != nullptr) it->second.vehicles = vehicles;
				this->work -= allowance - left;
			}
			if (usable_vehicles != nullptr) *usable_vehicles = it->second.vehicles.value_or(0);
			return it->second.stops;
		}

		void Ride(CargoType cargo, StationID destination, const PassengerJourneyExecution &ride, StationID first, double amount, size_t depth)
		{
			if (amount < MIN_AMOUNT || this->work == 0 || depth >= MAX_DEPTH) return;
			--this->work;
			if (ride.kind != PassengerRideKind::VehicleSnapshot && !this->database.IsCurrent(ride)) return;
			const auto &stops = this->Stops(cargo, ride);
			if (stops.size() < 2) return;
			if (stops.size() > this->work) return;
			this->work -= stops.size();
			if (std::find(this->transfers.begin(), this->transfers.end(), ride.alighting) != this->transfers.end()) return;
			if (first == StationID::Invalid()) {
				auto next = std::find_if(stops.begin(), stops.end(), [&](StationID station) { return station != stops.front(); });
				if (next == stops.end()) return;
				first = *next; this->result.first_hops[first] += amount;
			}
			for (size_t i = 1; i < stops.size(); ++i) {
				if (stops[i - 1] != stops[i]) this->result.links[{first, stops[i - 1], stops[i]}] += amount;
			}
			if (ride.alighting == destination) this->result.alighting[destination] += amount;
			else this->Policy(cargo, ride.alighting, destination, ride.service, OrderListID::Invalid(), first, amount, depth + 1);
		}

		void Policy(CargoType cargo, StationID from, StationID destination, OrderListID previous, OrderListID failed,
				StationID first, double amount, size_t depth, const PassengerJourneyExecution *intent = nullptr)
		{
			if (amount < MIN_AMOUNT || this->work == 0 || depth >= MAX_DEPTH || !Station::IsValidID(destination) ||
					std::find(this->transfers.begin(), this->transfers.end(), from) != this->transfers.end()) return;
			--this->work;
			PolicyKey key{cargo, from, destination, previous, failed, intent != nullptr ? intent->generation : 0};
			auto [it, inserted] = this->policies.try_emplace(key);
			if (inserted) {
				size_t allowance = std::min(this->work, size_t{4096}), left = allowance;
				it->second = this->database.InspectRoutes(cargo, from, destination, previous, failed, left, intent);
				this->work -= allowance - left;
			}
			this->transfers.push_back(from);
			for (const auto &candidate : it->second) this->Ride(cargo, destination, candidate.ride, first, amount * candidate.probability, depth);
			this->transfers.pop_back();
		}

		/** Fallback execution prefers a direct OD ride, otherwise its saved hop.
		 * Inspect published physical prefixes instead of assigning another OD's flows. */
		void Fallback(CargoType cargo, StationID from, StationID destination, StationID hop, OrderListID previous, double amount)
		{
			std::vector<std::pair<PassengerJourneyExecution, double>> candidates;
			double total = 0;
			size_t scan_left = 4096;
			const uint64_t mask = uint64_t{1} << cargo;
			for (const auto &ref : this->database.StopsAt(from)) {
				if (this->work == 0 || scan_left == 0 || candidates.size() >= 64) break;
				--this->work; --scan_left;
				const auto *service = this->database.Get(ref.service);
				if (service == nullptr || ref.service == previous || service->timetable.Circuit() <= 0) continue;
				const auto &stops = service->timetable.stops;
				if (ref.stop >= stops.size() || !(stops[ref.stop].board & mask) || (stops[ref.stop].forced & mask)) continue;
				PassengerJourneyExecution ride;
				ride.service = ref.service; ride.signature = service->incarnation; ride.generation = service->generation;
				ride.kind = PassengerRideKind::EstimatedServiceIntent;
				ride.boarding = from; ride.boarding_order = stops[ref.stop].order;
				if (!this->database.IsCurrent(ride)) continue;
				for (size_t step = 1; step < stops.size() && this->work != 0 && scan_left != 0; ++step) {
					--this->work; --scan_left;
					const auto &before = stops[(ref.stop + step - 1) % stops.size()], &stop = stops[(ref.stop + step) % stops.size()];
					if (!before.continues || (step > 1 && (before.forced & mask))) break;
					if ((stop.alight & mask) && (stop.station == destination || (ride.alighting == StationID::Invalid() && stop.station == hop))) {
						ride.alighting = stop.station; ride.alighting_order = stop.order;
						if (stop.station == destination) break;
					}
				}
				size_t vehicles = 0;
				if (ride.alighting == StationID::Invalid() || this->Stops(cargo, ride, &vehicles).empty()) continue;
				/* Display-only departures per tick. Circuit can exceed the Hyperpath
				 * fixed-point wait range; floating point preserves those long periods. */
				double frequency = double(vehicles) / double(service->timetable.Circuit());
				candidates.emplace_back(ride, frequency); total += frequency;
			}
			if (candidates.empty()) {
				this->Policy(cargo, from, destination, previous, OrderListID::Invalid(), StationID::Invalid(), amount, 0);
				return;
			}
			this->transfers.push_back(from);
			for (const auto &[ride, frequency] : candidates) this->Ride(cargo, destination, ride, StationID::Invalid(), amount * frequency / total, 0);
			this->transfers.pop_back();
		}

		void Legacy(CargoType cargo, StationID origin, StationID from, StationID next, StationID first, double amount, size_t depth)
		{
			if (amount < MIN_AMOUNT || this->work == 0 || depth >= MAX_DEPTH || !Station::IsValidID(next) || from == next ||
					std::find(this->transfers.begin(), this->transfers.end(), next) != this->transfers.end()) return;
			--this->work;
			this->result.links[{first, from, next}] += amount;
			if (depth == 0) this->result.first_hops[first] += amount;
			const auto &ge = Station::Get(next)->goods[cargo];
			if (ge.data == nullptr) return;
			auto flow = ge.data->flows.find(origin);
			if (flow == ge.data->flows.end() || flow->IsInvalid() || flow->begin() == flow->end()) return;
			uint32_t total = 0;
			for (auto share = flow->begin(); share != flow->end(); ++share) {
				if (this->work == 0) return;
				--this->work; total = share->first;
			}
			if (total == 0) return;
			this->transfers.push_back(next);
			uint32_t prev = 0;
			for (auto share = flow->begin(); share != flow->end(); ++share) {
				if (this->work == 0) break;
				--this->work;
				double part = amount * (share->first - prev) / total; prev = share->first;
				if (share->second == next) this->result.alighting[next] += part;
				else this->Legacy(cargo, origin, next, share->second, first, part, depth + 1);
			}
			this->transfers.pop_back();
		}
	};
}

StationPassengerRoutes BuildStationPassengerRoutes(const Station &station, const PassengerServiceDatabase &database)
{
	RouteBuilder builder{database};
	/* Timing fields do not affect an expected route. Group by actual routing
	 * identity, OD, incoming and failed service, rather than by packet/queue key. */
	using Group = std::tuple<PassengerRoutingMode, StationID, StationID, StationID, OrderListID, OrderListID,
			PassengerRideKind, OrderListID, VehicleID, StationID, StationID, VehicleOrderID, VehicleOrderID, uint64_t, uint64_t>;
	for (CargoType cargo : EnumRange(NUM_CARGO)) {
		if (!CargoSpec::Get(cargo)->IsValid() || !IsCargoInClass(cargo, CargoClass::Passengers)) continue;
		const auto &ge = station.goods[cargo];
		if (ge.data == nullptr) continue;
		std::map<Group, std::pair<const CargoPacket *, double>> groups;
		for (StationCargoList::ConstIterator it = ge.data->cargo.Packets()->begin(); it != ge.data->cargo.Packets()->end(); ++it) {
			const CargoPacket &packet = **it;
			const auto &ride = packet.GetPassengerJourney();
			const auto *arriving = OrderList::GetIfValid(packet.GetPassengerArrivingService());
			OrderListID previous = arriving != nullptr && arriving->GetCargoDistRouteGeneration() == packet.GetPassengerArrivingIncarnation() ? arriving->index : OrderListID::Invalid();
			const bool od = packet.GetPassengerDestination() != StationID::Invalid();
			StationID hop = it.GetKey() != StationID::Invalid() ? it.GetKey() : packet.GetPassengerLegacyHop();
			if (packet.GetPassengerRoutingMode() == PassengerRoutingMode::Journey) hop = StationID::Invalid();
			Group key{packet.GetPassengerRoutingMode(), packet.GetPassengerDestination(), od ? StationID::Invalid() : packet.GetFirstStation(), hop, previous,
					packet.GetPassengerFailedService(_state_ticks), ride.kind, ride.service, ride.vehicle, ride.boarding, ride.alighting,
					ride.boarding_order, ride.alighting_order, ride.signature, ride.generation};
			auto found = groups.find(key);
			if (found != groups.end()) found->second.second += packet.Count();
			else if (groups.size() < MAX_GROUPS) groups.emplace(key, std::pair(&packet, double(packet.Count())));
		}
		/* Give the largest cohorts the display allowance first. */
		std::vector<decltype(groups)::const_iterator> order;
		for (auto it = groups.begin(); it != groups.end(); ++it) order.push_back(it);
		std::stable_sort(order.begin(), order.end(), [](auto a, auto b) { return a->second.second > b->second.second; });
		for (auto it : order) {
			const auto &[key, group] = *it;
			const auto &[packet, amount] = group;
			const auto &ride = packet->GetPassengerJourney();
			const StationID destination = packet->GetPassengerDestination();
			builder.transfers.clear();
			if (packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey && ride.kind != PassengerRideKind::HyperpathIntent) {
				if (ride.boarding != station.index) continue;
				builder.transfers.push_back(station.index);
				builder.Ride(cargo, destination, ride, StationID::Invalid(), amount, 0);
			} else if (destination != StationID::Invalid()) {
				if (packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey) {
					builder.Policy(cargo, station.index, destination, std::get<4>(key), std::get<5>(key), StationID::Invalid(), amount, 0, &ride);
				} else {
					builder.Fallback(cargo, station.index, destination, std::get<3>(key), std::get<4>(key), amount);
				}
			} else {
				StationID next = std::get<3>(key);
				if (next == StationID::Invalid()) next = packet->GetPassengerLegacyHop();
				builder.transfers.push_back(station.index);
				builder.Legacy(cargo, packet->GetFirstStation(), station.index, next, next, amount, 0);
			}
		}
	}
	return std::move(builder.result);
}
