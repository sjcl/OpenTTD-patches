/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */

/** @file station_passenger_route_gui.cpp Read-only route examples; never admits simulation queries. */
#include "stdafx.h"
#include "station_passenger_route_gui.h"
#include "cargopacket.h"
#include "station_base.h"
#include "passenger_service_database.h"
#include "linkgraph/passenger_route_overlay.h"
#include "date_func.h"

std::optional<StationPassengerRouteTarget> GetStationPassengerRouteTarget(bool waiting, std::span<const StationCargoKey> path)
{
	if (!waiting || path.size() != 3) return {};
	const auto *cargo = std::get_if<CargoType>(&path[0]);
	const auto *service = std::get_if<StationPassengerService>(&path[1]);
	const auto *destination = std::get_if<StationID>(&path[2]);
	if (cargo == nullptr || *cargo >= NUM_CARGO || !IsCargoInClass(*cargo, CargoClass::Passengers) || service == nullptr ||
			service->label == StationPassengerServiceLabel::Reserved || destination == nullptr || !Station::IsValidID(*destination)) return {};
	return StationPassengerRouteTarget{*cargo, PassengerServiceExpansionKey(*service), *destination};
}

std::optional<size_t> GetStationPassengerRouteRow(int scrolled_row, int position, size_t displayed_count)
{
	if (scrolled_row < position || size_t(scrolled_row - position) >= displayed_count) return {};
	return size_t(scrolled_row - position);
}

StationPassengerRouteSamples CollectStationPassengerRouteSamples(const Station &station, const StationPassengerRouteTarget &target,
		uint row_count, StationPassengerServiceCache &services, size_t scan_limit)
{
	StationPassengerRouteSamples result;
	uint matched = 0, packets = 0;
	const auto *list = station.goods[target.cargo].ConstCargoList().Packets();
	StationCargoList::ConstIterator it = list->begin();
	for (; it != list->end() && scan_limit != 0; ++it, --scan_limit) {
		const CargoPacket &packet = **it;
		if (packet.GetPassengerDestination() != target.destination ||
				PassengerServiceExpansionKey(services.Resolve(target.cargo, packet, station.index)) != target.service) continue;
		++packets; matched += packet.Count();
		const auto *arriving = OrderList::GetIfValid(packet.GetPassengerArrivingService());
		OrderListID previous = arriving != nullptr && arriving->GetCargoDistRouteGeneration() == packet.GetPassengerArrivingIncarnation() ? arriving->index : OrderListID::Invalid();
		StationPassengerRouteSample sample{packet.GetPassengerRoutingMode(), packet.GetPassengerJourney(), previous,
				packet.GetPassengerFailedService(_state_ticks), packet.Count()};
		auto same = std::find_if(result.packets.begin(), result.packets.end(), [&](const auto &s) {
			return s.mode == sample.mode && s.ride == sample.ride && s.previous == sample.previous && s.failed == sample.failed;
		});
		if (same != result.packets.end()) {
			same->count += sample.count;
		} else if (result.packets.size() < StationPassengerRouteSamples::MAX_SAMPLES) {
			result.packets.push_back(sample);
		} else {
			result.incomplete = true;
			auto smallest = std::min_element(result.packets.begin(), result.packets.end(), [](const auto &a, const auto &b) { return a.count < b.count; });
			if (sample.count > smallest->count) *smallest = sample;
		}
	}
	result.incomplete |= it != list->end() || matched != row_count;
	result.multiple = packets > 1;
	std::stable_sort(result.packets.begin(), result.packets.end(), [](const auto &a, const auto &b) { return a.count > b.count; });
	return result;
}

namespace {
/** Compare the displayed transfer/service itinerary, ignoring cohort timing and binding details. */
bool SameRoute(const StationPassengerRouteExample &a, const StationPassengerRouteExample &b)
{
	if (a.complete != b.complete || a.legs.size() != b.legs.size()) return false;
	for (size_t i = 0; i < a.legs.size(); ++i) {
		if (std::tie(a.legs[i].service, a.legs[i].boarding, a.legs[i].alighting) !=
				std::tie(b.legs[i].service, b.legs[i].boarding, b.legs[i].alighting)) return false;
	}
	return true;
}

bool ValidRide(CargoType cargo, const PassengerJourneyExecution &ride, const PassengerServiceDatabase &db, size_t &work)
{
	if (ride.kind == PassengerRideKind::HyperpathIntent || ride.boarding == ride.alighting ||
			!Station::IsValidID(ride.boarding) || !Station::IsValidID(ride.alighting) ||
			(ride.kind != PassengerRideKind::VehicleSnapshot && !db.IsCurrent(ride))) return false;
	size_t allowance = std::min(work, StationPassengerRouteExamples::QUERY_WORK), left = allowance;
	const bool valid = GetPassengerRouteStops(ride, cargo, left).size() >= 2;
	work -= allowance - left;
	return valid;
}

struct RouteBuilder {
	CargoType cargo;
	StationID destination;
	const PassengerServiceDatabase &db;
	size_t &work;
	std::vector<StationPassengerRouteExample> routes;
	bool more = false, unknown = false;
	size_t branches = 0;

	void Save(StationPassengerRouteExample route)
	{
		if (route.legs.empty()) { this->unknown = true; return; }
		if (std::none_of(this->routes.begin(), this->routes.end(), [&](const auto &r) { return SameRoute(r, route); })) {
			if (this->routes.size() < StationPassengerRouteExamples::MAX_ROUTES) this->routes.push_back(std::move(route));
			else this->more = true;
		}
	}

	void Ride(StationPassengerRouteExample route, const PassengerJourneyExecution &ride)
	{
		if (this->work == 0 || !ValidRide(this->cargo, ride, this->db, this->work) ||
				std::any_of(route.legs.begin(), route.legs.end(), [&](const auto &leg) { return leg.boarding == ride.alighting; })) {
			this->Save(std::move(route)); return;
		}
		route.legs.push_back(ride);
		route.complete = ride.alighting == this->destination;
		if (route.complete || route.legs.size() >= StationPassengerRouteExamples::MAX_LEGS) this->Save(std::move(route));
		else this->Policy(std::move(route), ride.alighting, ride.service, OrderListID::Invalid());
	}

	void Policy(StationPassengerRouteExample route, StationID from, OrderListID previous, OrderListID failed,
			const PassengerJourneyExecution *intent = nullptr)
	{
		if (this->work == 0) { this->Save(std::move(route)); return; }
		/* InspectRoutes can otherwise fall back to today's table for a stale intent. Do not silently replace a cohort's policy. */
		if (intent != nullptr && (!this->db.IsCurrent(*intent) || intent->signature != uint64_t(this->cargo) + 1 ||
				intent->boarding != from || intent->alighting != this->destination)) { this->Save(std::move(route)); return; }
		size_t allowance = std::min(this->work, StationPassengerRouteExamples::QUERY_WORK), left = allowance;
		auto candidates = this->db.InspectRoutes(this->cargo, from, this->destination, previous, failed, left, intent);
		this->work -= allowance - left;
		if (candidates.empty()) { this->Save(std::move(route)); return; }
		double probability = 0;
		for (const auto &candidate : candidates) probability += candidate.probability;
		/* InspectRoutes retains whole-policy probabilities even when its display/Work limit omits branches. */
		this->more |= probability < 0.999999;
		for (const auto &candidate : candidates) {
			if (this->branches >= 16 || this->routes.size() >= StationPassengerRouteExamples::MAX_ROUTES) { this->more = true; break; }
			++this->branches;
			if (candidate.ride.boarding != from) continue;
			this->Ride(route, candidate.ride);
		}
	}
};
} // namespace

StationPassengerRouteExamples EstimateStationPassengerRoutes(CargoType cargo, StationID from, StationID destination,
		const StationPassengerRouteSamples &samples, const PassengerServiceDatabase &database, size_t &work)
{
	StationPassengerRouteExamples result;
	result.unknown = samples.incomplete || samples.packets.empty(); result.multiple = samples.multiple;
	if (cargo >= NUM_CARGO || !Station::IsValidID(from) || !Station::IsValidID(destination)) { result.unknown = true; return result; }
	std::vector<RouteBuilder> cohorts;
	cohorts.reserve(StationPassengerRouteSamples::MAX_SAMPLES);
	for (const auto &sample : samples.packets) {
		if (cohorts.size() >= StationPassengerRouteSamples::MAX_SAMPLES) { result.unknown = true; break; }
		cohorts.push_back({cargo, destination, database, work});
		auto &builder = cohorts.back();
		if (sample.mode == PassengerRoutingMode::Journey) {
			if (sample.ride.boarding != from) builder.unknown = true;
			else if (sample.ride.kind == PassengerRideKind::HyperpathIntent) builder.Policy({}, from, sample.previous, sample.failed, &sample.ride);
			else builder.Ride({}, sample.ride);
		} else {
			builder.Policy({}, from, sample.previous, sample.failed);
		}
		result.unknown |= builder.unknown;
		result.more |= builder.more;
	}
	/* First show one example per sampled cohort (largest first), then their alternatives.
	 * Counts only select representatives; they never become route probabilities. */
	for (size_t round = 0; round < StationPassengerRouteExamples::MAX_ROUTES; ++round) {
		for (const auto &cohort : cohorts) {
			if (round >= cohort.routes.size()) continue;
			const auto &route = cohort.routes[round];
			if (std::any_of(result.routes.begin(), result.routes.end(), [&](const auto &r) { return SameRoute(r, route); })) continue;
			if (result.routes.size() < StationPassengerRouteExamples::MAX_ROUTES) result.routes.push_back(route);
			else result.more = true;
		}
	}
	return result;
}

struct StationPassengerRouteCache::Impl {
	using Revision = decltype(GetPassengerServiceDatabase().BoardingRevision());
	std::optional<StationPassengerRouteTarget> target;
	StationID station = StationID::Invalid();
	StationPassengerRouteSamples samples;
	StationPassengerRouteExamples result;
	Revision revision{};
	uint64_t epoch = 0, changes = 0, source = 0, reset = 0;
};
StationPassengerRouteCache::StationPassengerRouteCache() : impl(std::make_unique<Impl>()) {}
StationPassengerRouteCache::~StationPassengerRouteCache() = default;
void StationPassengerRouteCache::Clear() { this->impl = std::make_unique<Impl>(); }

const StationPassengerRouteExamples &StationPassengerRouteCache::Get(const Station &station, const StationPassengerRouteTarget &target,
		uint row_count, StationPassengerServiceCache &services)
{
	auto &cache = *this->impl;
	const auto &db = GetPassengerServiceDatabase();
	auto samples = CollectStationPassengerRouteSamples(station, target, row_count, services);
	bool valid = cache.target == target && cache.station == station.index && cache.samples == samples &&
			cache.revision == db.BoardingRevision() && cache.epoch == db.HyperpathLookupEpoch() && cache.changes == db.HyperpathDestinationChanges() &&
			cache.source == db.HyperpathDisplaySourceRevision() && cache.reset == db.HyperpathDisplayResetEpoch();
	/* Even paused games may delete vehicles or change facilities/groups. Recheck live rides under a separate local bound. */
	size_t work = StationPassengerRouteExamples::TOTAL_WORK;
	if (valid) for (const auto &route : cache.result.routes) for (const auto &leg : route.legs) if (!ValidRide(target.cargo, leg, db, work)) valid = false;
	if (!valid) {
		work = StationPassengerRouteExamples::TOTAL_WORK;
		cache.result = EstimateStationPassengerRoutes(target.cargo, station.index, target.destination, samples, db, work);
		cache.target = target; cache.station = station.index; cache.samples = std::move(samples);
		cache.revision = db.BoardingRevision(); cache.epoch = db.HyperpathLookupEpoch(); cache.changes = db.HyperpathDestinationChanges();
		cache.source = db.HyperpathDisplaySourceRevision(); cache.reset = db.HyperpathDisplayResetEpoch();
	}
	return cache.result;
}
