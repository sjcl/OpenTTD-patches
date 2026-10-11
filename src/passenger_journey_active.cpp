/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_journey_active.cpp Deterministic journey execution, without forecast seat bookings. */
#include "stdafx.h"
#include "passenger_journey_active.h"
#include "vehicle_base.h"
#include "station_base.h"
#include "settings_type.h"
#include "date_func.h"
#include "window_func.h"
#include "sl/saveload.h"
#include "debug.h"
#include <set>

#include "safeguards.h"

bool IsPassengerJourneyRoutingEnabled(CargoType cargo)
{
	return _settings_game.linkgraph.passenger_journey_routing && cargo < NUM_CARGO && CargoSpec::Get(cargo)->IsValid() &&
			IsCargoInClass(cargo, CargoClass::Passengers) && _settings_game.linkgraph.GetDistributionType(cargo) != DistributionType::Manual;
}

/** Stable value hash, excluding occupancy and GUI names. All predictor inputs in orders/schedules are included.
 * Conditional orders remain unforecastable; their hash still invalidates an already selected leg. */
uint64_t PassengerJourneyServiceSignature(const OrderList &orders, CargoType cargo, size_t *budget)
{
	size_t local_budget = 65536;
	if (budget == nullptr) budget = &local_budget;
	auto spend = [&](size_t amount) { if (amount > *budget) return false; *budget -= amount; return true; };
	uint64_t hash = 14695981039346656037ULL;
	auto add = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
	add(orders.GetNumOrders());
	add(orders.GetNumVehicles());
	for (const Order &order : orders.GetOrderVector()) {
		if (!spend(32 + 2 * NUM_CARGO)) return 0;
		add(order.GetType()); add(order.GetDestination().base()); add(order.GetRawFlags());
		add(order.IsWaitFixed()); add(order.IsTravelFixed());
		add(order.GetXData()); add(order.GetXData2()); add(order.GetNonStopType());
		add(order.GetWaitTime()); add(order.GetTravelTime()); add(order.IsWaitTimetabled()); add(order.IsTravelTimetabled());
		add(order.GetRefitCargo()); add(order.GetDispatchScheduleIndex()); add(order.GetLeaveType());
		/* A full load of another cargo can delay this train too. */
		for (CargoType cid : EnumRange(NUM_CARGO)) { add(to_underlying(order.GetCargoLoadType(cid))); add(to_underlying(order.GetCargoUnloadType(cid))); }
	}
	for (const DispatchSchedule &schedule : orders.GetScheduledDispatchScheduleSet()) {
		if (!spend(8 + schedule.GetScheduledDispatch().size())) return 0;
		add(schedule.GetScheduledDispatchStartTick().base()); add(schedule.GetScheduledDispatchDuration());
		add(schedule.GetScheduledDispatchLastDispatch()); add(schedule.GetScheduledDispatchDelay()); add(schedule.GetScheduledDispatchReuseSlots());
		for (const auto &slot : schedule.GetScheduledDispatch()) { add(slot.offset); add(slot.flags); }
	}
	add(cargo);
	return hash;
}

/** Future capacity is an upper bound. Real seats are assigned exclusively by Load/Reserve below.
 * Do not use captured occupancy to reject a group that can be split across coaches/gradual loading. */
struct JourneyCapacityUpperBound : PassengerJourneyCapacityProvider {
	uint64_t Revision() const override { return 0; }
	PassengerJourneyCapacity Estimate(const PassengerServiceGraph &graph, PassengerServiceGraph::VisitID boarding,
			PassengerServiceGraph::VisitID) const override
	{
		PassengerJourneyCapacity capacity;
		capacity.future_total = capacity.future_free = graph.GetTrips()[boarding.trip].capacity;
		capacity.certainty = PassengerCapacityCertainty::Estimated;
		return capacity;
	}
};

struct PassengerJourneyActiveContext::Snapshot {
	PassengerServiceGraph graph;
	JourneyCapacityUpperBound capacity;
	PassengerJourneyPlanner planner;
	std::map<OrderListID, uint64_t> signatures;
	std::map<StationID, std::optional<PassengerJourneyExecution>> waiting;
	std::map<StationID, std::optional<std::pair<PassengerJourneyExecution, bool>>> onboard;
	Snapshot(PassengerServiceGraph graph) : graph(std::move(graph)), planner(this->graph, &this->capacity, {128, 4 * 1024 * 1024}) {}
};

PassengerJourneyActiveContext::PassengerJourneyActiveContext(const Vehicle *vehicle, StationID station) : vehicle(vehicle), station(station) {}
PassengerJourneyActiveContext::~PassengerJourneyActiveContext() = default;

PassengerJourneyActiveContext::Snapshot *PassengerJourneyActiveContext::GetSnapshot(CargoType cargo)
{
	auto found = this->snapshots.find(cargo);
	if (found != this->snapshots.end()) return found->second.get();
	/* The budget belongs to this simulation event, never a local/global diagnostic cache.
	 * Joining/loading a save therefore cannot change which packets receive an allowance. */
	if (this->work_left < 524288) return nullptr;
	PassengerServiceGraphLimits limits;
	limits.work = 524288;
	limits.memory_bytes = 128 * 1024 * 1024;
	limits.total_visits = 65536;
	limits.visits_per_trip = 128;
	this->work_left -= limits.work; // Charge the same fixed allowance even for a small graph.
	auto snapshot = std::make_unique<Snapshot>(PassengerServiceGraph::Build(cargo, _state_ticks, limits));
	return this->snapshots.emplace(cargo, std::move(snapshot)).first->second.get();
}

static PassengerJourneyLimits ActiveJourneyLimits()
{
	PassengerJourneyLimits limits;
	limits.candidates = 1;
	limits.work = 65536;
	limits.labels = 8192;
	limits.memory_bytes = 8 * 1024 * 1024;
	return limits;
}

static PassengerJourneyEvaluation ActiveJourneyEvaluation()
{
	PassengerJourneyEvaluation evaluation;
	evaluation.minimum_transfer = evaluation.transfer_penalty = _settings_game.linkgraph.passenger_transfer_penalty;
	return evaluation;
}

std::optional<PassengerJourneyExecution> PassengerJourneyActiveContext::Execute(CargoType cargo, const PassengerJourneyLeg &leg)
{
	auto *snapshot = this->GetSnapshot(cargo);
	if (snapshot == nullptr || !Station::IsValidID(leg.boarding_station) || !Station::IsValidID(leg.alighting_station)) return {};
	const auto &trip = snapshot->graph.GetTrips()[leg.boarding.trip];
	const OrderList *orders = OrderList::GetIfValid(trip.service);
	if (orders == nullptr) return {};
	auto [signature, inserted] = snapshot->signatures.try_emplace(trip.service, 0);
	if (inserted) signature->second = PassengerJourneyServiceSignature(*orders, cargo, &this->work_left);
	if (signature->second == 0) return {};
	return PassengerJourneyExecution{leg.vehicle, trip.service, leg.boarding_station, leg.alighting_station,
			trip.prediction.visits[leg.boarding.visit].order, trip.prediction.visits[leg.alighting.visit].order,
			leg.departure, leg.arrival, signature->second};
}

bool PassengerJourneyActiveContext::PlanWaiting(CargoPacket &packet, CargoType cargo, StationID legacy_hop)
{
	if (!IsPassengerJourneyRoutingEnabled(cargo) || !Station::IsValidID(packet.GetPassengerDestination()) ||
			packet.GetPassengerDestination() == this->station || !packet.CanRetryPassengerJourney(this->station, _state_ticks)) return false;
	auto *snapshot = this->GetSnapshot(cargo);
	if (snapshot == nullptr || snapshot->graph.GetStatus() != PassengerPredictionStatus::Complete || this->queries_left == 0) return false;
	if (snapshot->waiting.size() >= 128 && !snapshot->waiting.contains(packet.GetPassengerDestination())) return false;
	auto [cached, inserted] = snapshot->waiting.try_emplace(packet.GetPassengerDestination());
	if (!inserted) {
		if (!cached->second) return false;
		packet.SetPassengerJourney(*cached->second, legacy_hop);
		return true;
	}
	/* Reserve a fixed query allowance. Cache reuse is a performance optimization only. */
	auto limits = ActiveJourneyLimits();
	if (this->work_left < limits.work) return false;
	this->work_left -= limits.work;
	--this->queries_left;
	auto result = snapshot->planner.Plan(cargo, this->station, packet.GetPassengerDestination(), _state_ticks, limits, ActiveJourneyEvaluation(), 1);
	if (!result.HasJourney() || result.journeys.front().legs.empty() || result.status != PassengerPredictionStatus::Complete) return false;
	auto execution = this->Execute(cargo, result.journeys.front().legs.front());
	if (!execution) return false;
	cached->second = *execution;
	packet.SetPassengerJourney(*execution, legacy_hop);
	return true;
}

PassengerJourneyBoardingContext::PassengerJourneyBoardingContext(const Vehicle *vehicle, CargoType cargo) : vehicle(vehicle),
		cargo(cargo != INVALID_CARGO ? cargo : vehicle == nullptr ? INVALID_CARGO : vehicle->cargo_type)
{
	if (vehicle == nullptr) return;
	this->vehicle = vehicle->First();
	if (this->vehicle->type != VehicleType::Train || !IsPassengerJourneyRoutingEnabled(this->cargo) ||
			this->vehicle->current_order.GetCargoLoadType(this->cargo) == OrderLoadType::NoLoad) return;
	auto state = GetPassengerRouteState(this->vehicle);
	if (!state || !state->loading) return;
	this->signature = PassengerJourneyServiceSignature(*this->vehicle->orders, this->cargo);
	size_t budget = 4096;
	this->prediction = PredictPassengerVisits(*state, this->cargo, _state_ticks, _state_ticks + 65536, 128, 4096, budget);
}

bool PassengerJourneyBoardingContext::CanBoard(const CargoPacket &packet) const
{
	if (packet.GetPassengerRoutingMode() != PassengerRoutingMode::Journey) return true;
	if (this->vehicle == nullptr || this->signature == 0 || this->prediction.visits.empty()) return false;
	const auto &leg = packet.GetPassengerJourney();
	const auto &first = this->prediction.visits.front();
	if (leg.vehicle != this->vehicle->index || leg.service != this->vehicle->orders->index || leg.signature != this->signature ||
			leg.boarding != this->vehicle->last_station_visited || leg.boarding_order != this->vehicle->cur_real_order_index ||
			!first.loading_now || !first.departure || *first.departure != leg.departure) return false;
	for (const auto &visit : this->prediction.visits) {
		if (visit.order == leg.alighting_order && visit.arrival == leg.arrival) return true;
	}
	return false;
}

std::optional<VehicleCargoList::MoveToAction> PassengerJourneyActiveContext::Decide(CargoPacket &packet, CargoType cargo, bool accepted, OrderUnloadType unload)
{
	using Action = VehicleCargoList::MoveToAction;
	if (packet.GetPassengerRoutingMode() != PassengerRoutingMode::Journey) return {};
	auto fallback = [&]() -> std::optional<Action> { packet.FallbackPassengerJourney(this->station, _state_ticks); return {}; };
	if (!IsPassengerJourneyRoutingEnabled(cargo) || this->vehicle == nullptr || this->vehicle->type != VehicleType::Train ||
			!Station::IsValidID(packet.GetPassengerDestination())) return fallback();
	if (unload == OrderUnloadType::NoUnload) return Action::Keep;
	if (packet.GetPassengerDestination() == this->station) return accepted ? std::optional(Action::Deliver) : fallback();
	auto *snapshot = this->GetSnapshot(cargo);
	if (snapshot == nullptr || snapshot->graph.GetStatus() != PassengerPredictionStatus::Complete || this->queries_left == 0) return fallback();
	if (snapshot->onboard.size() >= 128 && !snapshot->onboard.contains(packet.GetPassengerDestination())) return fallback();
	auto [cached, inserted] = snapshot->onboard.try_emplace(packet.GetPassengerDestination());
	if (!inserted) {
		if (!cached->second) return fallback();
		packet.SetPassengerJourney(cached->second->first, StationID::Invalid());
		packet.SetNextHop(StationID::Invalid());
		return cached->second->second ? Action::Keep : Action::Transfer;
	}
	auto onboard = snapshot->graph.FindOnboardVisit(this->vehicle->index, this->station, this->vehicle->cur_real_order_index);
	auto limits = ActiveJourneyLimits();
	if (!onboard || this->work_left < limits.work) return fallback();
	this->work_left -= limits.work;
	--this->queries_left;
	auto result = snapshot->planner.PlanOnboard(cargo, *onboard, packet.GetPassengerDestination(), limits, ActiveJourneyEvaluation(), 1);
	if (!result.HasJourney() || result.journeys.front().legs.empty() || result.status != PassengerPredictionStatus::Complete) return fallback();
	const auto &leg = result.journeys.front().legs.front();
	auto execution = this->Execute(cargo, leg);
	if (!execution) return fallback();
	bool staying = leg.vehicle == this->vehicle->index && leg.boarding == *onboard;
	if (staying && (unload == OrderUnloadType::Unload || unload == OrderUnloadType::Transfer)) return fallback();
	cached->second = std::pair(*execution, staying);
	packet.SetPassengerJourney(*execution, StationID::Invalid());
	packet.SetNextHop(StationID::Invalid());
	return staying ? Action::Keep : Action::Transfer;
}

void StationCargoList::RoutePassengerJourneys(CargoType cargo, StationID station, bool allow_search)
{
	PassengerJourneyActiveContext context(nullptr, station);
	/* New OD packets are considered before boarding. Fallback retries are staggered every
	 * 64 ticks after the cooldown; expired packets immediately regain their CargoDist key. */
	const bool search = allow_search && IsPassengerJourneyRoutingEnabled(cargo) && (_state_ticks.base() & 63) == (station.base() & 63);
	StationCargoPacketMap updated;
	std::map<VehicleID, std::unique_ptr<PassengerJourneyBoardingContext>> boarding;
	std::map<OrderListID, uint64_t> signatures;
	size_t validation_work = 524288;
	size_t inspected = 0;
	bool changed = false;
	for (Iterator it(this->packets.begin()); it != this->packets.end(); ++it) {
		CargoPacket *packet = *it;
		StationID key = it.GetKey();
		PassengerRoutingMode previous_mode = packet->GetPassengerRoutingMode();
		if (packet->GetPassengerRoutingMode() == PassengerRoutingMode::Fallback && key == StationID::Invalid()) key = packet->GetPassengerLegacyHop();
		if (packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey) {
			const auto &leg = packet->GetPassengerJourney();
			const Vehicle *vehicle = Vehicle::GetIfValid(leg.vehicle);
			bool valid = IsPassengerJourneyRoutingEnabled(cargo) && vehicle != nullptr && vehicle->type == VehicleType::Train &&
					!vehicle->vehstatus.Any({VehState::Stopped, VehState::Crashed}) && Station::IsValidID(packet->GetPassengerDestination()) &&
					vehicle->orders != nullptr && vehicle->orders->index == leg.service && leg.boarding == station &&
					Station::IsValidID(leg.alighting);
			if (valid && vehicle->current_order.IsAnyLoadingType() && vehicle->last_station_visited == station) {
				if (!boarding.contains(vehicle->index) && boarding.size() >= 128) valid = false;
				if (valid) {
					auto [found, inserted] = boarding.try_emplace(vehicle->index);
					if (inserted) found->second = std::make_unique<PassengerJourneyBoardingContext>(vehicle, cargo);
					valid = found->second->CanBoard(*packet);
				}
			} else if (valid) {
				auto [signature, inserted] = signatures.try_emplace(leg.service, 0);
				if (inserted) signature->second = PassengerJourneyServiceSignature(*vehicle->orders, cargo, &validation_work);
				valid = _state_ticks <= leg.departure && signature->second != 0 && leg.signature == signature->second;
			}
			if (!valid) { key = packet->GetPassengerLegacyHop(); packet->FallbackPassengerJourney(station, _state_ticks); }
			else key = StationID::Invalid();
		}
		if (IsPassengerJourneyRoutingEnabled(cargo) && allow_search && (search || packet->GetPassengerRoutingMode() == PassengerRoutingMode::Legacy) &&
				inspected++ < 4096 && packet->GetPassengerRoutingMode() != PassengerRoutingMode::Journey &&
				Station::IsValidID(packet->GetPassengerDestination()) && packet->GetPassengerDestination() != station &&
				packet->CanRetryPassengerJourney(station, _state_ticks)) {
			if (context.PlanWaiting(*packet, cargo, key)) key = StationID::Invalid();
			else packet->FallbackPassengerJourney(station, _state_ticks);
		}
		updated[key].push_back(packet);
		changed |= key != it.GetKey() || previous_mode != packet->GetPassengerRoutingMode();
	}
	this->packets = std::move(updated);
	if (changed) SetWindowDirty(WindowClass::StationView, station);
}

void RouteWaitingPassengerJourneys(Station *station)
{
	if (!_settings_game.linkgraph.passenger_journey_routing) return;
	for (CargoType cargo : EnumRange(NUM_CARGO)) {
		if (!IsCargoInClass(cargo, CargoClass::Passengers) || station->goods[cargo].data == nullptr) continue;
		station->goods[cargo].data->cargo.RoutePassengerJourneys(cargo, station->index);
	}
}

std::array<uint, 3> CountPassengerRoutingModes(const StationCargoList &cargo)
{
	std::array<uint, 3> counts{};
	for (StationCargoList::ConstIterator it(cargo.Packets()->begin()); it != cargo.Packets()->end(); ++it) counts[to_underlying((*it)->GetPassengerRoutingMode())] += (*it)->Count();
	return counts;
}

std::array<uint, 3> CountReservedPassengerRoutingModes(const Station &station, CargoType cargo)
{
	std::array<uint, 3> counts{};
	for (const Vehicle *front : station.loading_vehicles) {
		for (const Vehicle *v = front; v != nullptr; v = v->Next()) {
			if (v->cargo_type != cargo) continue;
			uint remaining = v->cargo.ReservedCount();
			for (auto it = v->cargo.Packets()->rbegin(); remaining > 0 && it != v->cargo.Packets()->rend(); ++it) {
				uint count = std::min(remaining, static_cast<uint>((*it)->Count()));
				counts[to_underlying((*it)->GetPassengerRoutingMode())] += count;
				remaining -= count;
			}
		}
	}
	return counts;
}

void PassengerJourneySettingsChanged(int32_t)
{
	/* Reserved cargo lives on vehicles. Return it before changing queue keys or mode. */
	for (Station *station : Station::Iterate()) {
		for (Vehicle *front : station->loading_vehicles) {
			for (Vehicle *v = front; v != nullptr; v = v->Next()) {
				if (!IsCargoInClass(v->cargo_type, CargoClass::Passengers) || IsPassengerJourneyRoutingEnabled(v->cargo_type)) continue;
				for (CargoPacket *packet : *v->cargo.Packets()) if (packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey) packet->FallbackPassengerJourney(station->index, _state_ticks);
				v->cargo.Return(UINT_MAX, &station->goods[v->cargo_type].CreateData().cargo, StationID::Invalid(), v->GetCargoTile());
			}
		}
		for (CargoType cargo : EnumRange(NUM_CARGO)) {
			if (IsCargoInClass(cargo, CargoClass::Passengers) && station->goods[cargo].data != nullptr) {
				station->goods[cargo].data->cargo.RoutePassengerJourneys(cargo, station->index, false);
			}
		}
	}
	for (CargoPacket *packet : CargoPacket::Iterate()) {
		if (!_settings_game.linkgraph.passenger_journey_routing && packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey) packet->FallbackPassengerJourney(StationID::Invalid(), _state_ticks);
	}
}

void AfterLoadPassengerJourneys()
{
	if (SlXvIsFeatureMissing(XSLFI_PASSENGER_JOURNEY_ROUTING)) _settings_game.linkgraph.passenger_journey_routing = false;
	std::set<CargoPacketID> invalidated;
	for (CargoPacket *packet : CargoPacket::Iterate()) {
		if (packet->GetPassengerRoutingMode() != PassengerRoutingMode::Journey) continue;
		const auto &leg = packet->GetPassengerJourney();
		const Vehicle *vehicle = Vehicle::GetIfValid(leg.vehicle);
		if (vehicle == nullptr || vehicle->type != VehicleType::Train || vehicle->orders == nullptr || vehicle->orders->index != leg.service ||
				!Station::IsValidID(leg.boarding) || !Station::IsValidID(leg.alighting) || !Station::IsValidID(packet->GetPassengerDestination()) ||
				leg.boarding_order >= vehicle->GetNumOrders() || leg.alighting_order >= vehicle->GetNumOrders() || !_settings_game.linkgraph.passenger_journey_routing) {
			packet->FallbackPassengerJourney(StationID::Invalid(), _state_ticks);
			invalidated.insert(packet->index);
		} else {
			const Order *board = vehicle->GetOrder(leg.boarding_order), *alight = vehicle->GetOrder(leg.alighting_order);
			if (!board->IsType(OT_GOTO_STATION) || board->GetDestination() != leg.boarding || !alight->IsType(OT_GOTO_STATION) ||
					alight->GetDestination() != leg.alighting || leg.arrival < leg.departure) {
				packet->FallbackPassengerJourney(StationID::Invalid(), _state_ticks);
				invalidated.insert(packet->index);
			}
		}
	}
	/* A primary engine may carry a different cargo. Validate the saved hash using the
	 * owning coach's actual cargo, and return invalid reserved suffixes through CargoReturn. */
	for (Vehicle *v : Vehicle::Iterate()) {
		const Vehicle *front = v->First();
		bool invalid = false;
		for (CargoPacket *packet : *v->cargo.Packets()) {
			if (packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey && (!IsPassengerJourneyRoutingEnabled(v->cargo_type) ||
					front->orders == nullptr || packet->GetPassengerJourney().vehicle != front->index ||
					packet->GetPassengerJourney().signature != PassengerJourneyServiceSignature(*front->orders, v->cargo_type))) {
				packet->FallbackPassengerJourney(front->last_station_visited, _state_ticks);
				invalid = true;
				invalidated.insert(packet->index);
			}
			if (invalidated.contains(packet->index)) invalid = true;
		}
		Station *station = Station::GetIfValid(front->last_station_visited);
		if (invalid && v->cargo.ReservedCount() > 0 && station != nullptr) v->cargo.Return(UINT_MAX,
				&station->goods[v->cargo_type].CreateData().cargo, StationID::Invalid(), v->GetCargoTile());
	}
	/* Rekey without searching: loading a game must never route or consume RNG. */
	for (Station *station : Station::Iterate()) {
		for (CargoType cargo : EnumRange(NUM_CARGO)) {
			if (station->goods[cargo].data != nullptr) station->goods[cargo].data->cargo.RoutePassengerJourneys(cargo, station->index, false);
		}
	}
	Debug(sl, 2, "Passenger journey afterload: feature {}, enabled {}", _sl_xv_feature_versions[XSLFI_PASSENGER_JOURNEY_ROUTING],
			_settings_game.linkgraph.passenger_journey_routing);
}

void InvalidatePassengerJourneyVehicle(VehicleID vehicle)
{
	/* Do this before the pool ID can be reused, even for an identical replacement service. */
	for (CargoPacket *packet : CargoPacket::Iterate()) {
		if (packet->GetPassengerRoutingMode() == PassengerRoutingMode::Journey && packet->GetPassengerJourney().vehicle == vehicle) {
			packet->FallbackPassengerJourney(StationID::Invalid(), _state_ticks);
		}
	}
}
