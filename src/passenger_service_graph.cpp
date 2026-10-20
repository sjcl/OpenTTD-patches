/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file passenger_service_graph.cpp Value-owned multimodal passenger services and predictions. */

#include "stdafx.h"
#include "passenger_service_graph.h"
#include "vehicle_base.h"
#include <chrono>
#include <unordered_map>

#include "safeguards.h"

static bool SpendGraphWork(size_t &budget, size_t work = 1)
{
	if (work > budget) { budget = 0; return false; }
	budget -= work;
	return true;
}

/** Limits dominate uncertainty, which dominates a complete result (including an empty one). */
static PassengerPredictionStatus MergeGraphStatus(PassengerPredictionStatus a, PassengerPredictionStatus b)
{
	return static_cast<PassengerPredictionStatus>(std::max(to_underlying(a), to_underlying(b)));
}

/** Compare values, never hashes. Names are absent from captured schedules and do
 * not enter GetScheduledDispatchTime; all slot/timing/history inputs do. */
static bool SamePredictionPattern(const PassengerServiceGraph::ServicePattern &pattern, const OrderList &list)
{
	if (!std::equal(pattern.orders.begin(), pattern.orders.end(), list.GetOrderVector().begin(), list.GetOrderVector().end(),
			[](const Order &a, const Order &b) { return a.EqualsForPassengerPrediction(b); })) return false;
	const auto &schedules = list.GetScheduledDispatchScheduleSet();
	return std::equal(pattern.schedules.begin(), pattern.schedules.end(), schedules.begin(), schedules.end(), [](const auto &a, const auto &b) {
		return std::tuple(a.GetScheduledDispatchStartTick(), a.GetScheduledDispatchDuration(), a.GetScheduledDispatchLastDispatch(),
				a.GetScheduledDispatchDelay(), a.GetScheduledDispatchReuseSlots()) ==
				std::tuple(b.GetScheduledDispatchStartTick(), b.GetScheduledDispatchDuration(), b.GetScheduledDispatchLastDispatch(),
				b.GetScheduledDispatchDelay(), b.GetScheduledDispatchReuseSlots()) &&
				std::equal(a.GetScheduledDispatch().begin(), a.GetScheduledDispatch().end(), b.GetScheduledDispatch().begin(), b.GetScheduledDispatch().end(), [](const auto &s, const auto &t) {
					return std::tie(s.offset, s.flags, s.route_id) == std::tie(t.offset, t.flags, t.route_id);
				});
	});
}

static auto PredictionTripInputs(const PassengerServiceGraph::Trip &trip)
{
	return std::tie(trip.vehicle, trip.service, trip.type, trip.tile, trip.direction, trip.current_order, trip.implicit_order,
			trip.timetable_order, trip.elapsed, trip.lateness, trip.loading, trip.scheduled_dispatch, trip.capacity, trip.occupants, trip.operation);
}

/** Discovery reuse must also notice newly eligible lists/vehicles (including
 * previously excluded freight). Exact eligibility values, not a tick/hash guard.
 * Validation is physical cache overhead; the original discovery debit is replayed. */
using PassengerDiscoveryInput = std::tuple<OrderListID, VehicleID, size_t, uint64_t, CargoType, bool, bool, bool, bool>;
template <typename F> static bool VisitPassengerDiscoveryInputs(CargoType cargo, F visit)
{
	for (const OrderList *list : OrderList::Iterate()) {
		const bool refit = std::any_of(list->GetOrderVector().begin(), list->GetOrderVector().end(), [](const Order &order) { return order.IsRefit(); });
		if (!visit(PassengerDiscoveryInput{list->index, VehicleID::Invalid(), list->GetNumOrders(), 0, INVALID_CARGO, IsPassengerServiceVehicle(list->GetFirstSharedVehicle()), refit, false, false})) return false;
		for (const Vehicle *v = list->GetFirstSharedVehicle(); v != nullptr; v = v->NextShared()) {
			uint64_t capacity = 0;
			if (IsPassengerServiceVehicle(v)) for (const Vehicle *u = v; u != nullptr; u = u->Next()) {
				/* Even a zero-passenger-capacity part changes discovery work. Cargo
				 * placement also changes where HasPassengerVehicleCapacity stops. */
				if (!visit(PassengerDiscoveryInput{list->index, u->index, 0, u->cargo_cap, u->cargo_type, false, false, false, false})) return false;
				if (u->cargo_type == cargo) capacity += u->cargo_cap;
			}
			if (!visit(PassengerDiscoveryInput{list->index, v->index, 0, capacity, v->cargo_type, IsPassengerServiceVehicle(v), false, v->current_order.IsRefit(),
					v->vehstatus.Any({VehState::Stopped, VehState::Crashed})})) return false;
		}
	}
	return true;
}

struct PassengerServiceCapture::Data {
	struct Candidate { VehicleID vehicle; uint64_t capacity; };
	struct Fleet { OrderListID list; std::vector<Candidate> candidates; };
	struct Discovery {
		CargoType cargo;
		bool scoped;
		size_t work, memory, storage;
		size_t trains, cargo_capable, possible_refit, inactive, excluded_cargo;
		std::vector<Fleet> fleets;
		std::vector<OrderListID> eligible;
		std::vector<PassengerDiscoveryInput> inputs;
	};
	struct Pattern { std::shared_ptr<const PassengerServiceGraph::ServicePattern> value; size_t work; };
	struct Prediction {
		std::shared_ptr<const PassengerServiceGraph::ServicePattern> pattern;
		PassengerServiceGraph::Trip trip; ///< Owned result and physical/progress identity; no game pointers.
		Order current_order;
		std::vector<uint8_t> usable_stops;
		StateTicks end;
		size_t max_visits, max_steps, bytes;
		uint shared_vehicles;
		int32_t x, y, z;
		Direction facing;
		CargoType vehicle_cargo;
		bool prefix_only;
	};
	StateTicks at;
	uint64_t revision;
	size_t storage_bound = 0;
	size_t prediction_bytes = 0;
	std::optional<Discovery> discovery;
	std::map<std::tuple<CargoType, OrderListID, size_t, PassengerTimingPolicy>, Pattern> patterns;
	struct PredictionLookupHash {
		size_t operator()(const std::pair<VehicleID, CargoType> &key) const { return std::hash<uint64_t>{}(static_cast<uint64_t>(key.first.base()) * NUM_CARGO + key.second); }
	};
	/** Lookup only: no hash or container iteration certifies reuse or enters
	 * simulation order. Full owned inputs are compared after finding the vehicle. */
	std::unordered_map<std::pair<VehicleID, CargoType>, Prediction, PredictionLookupHash> predictions;
	static_assert(sizeof(Prediction) + sizeof(OrderExtraInfo) + 4 * sizeof(void *) + sizeof(std::pair<VehicleID, CargoType>) <= 1024);
	Data(StateTicks at) : at(at), revision(PassengerGraphServiceIndexRevision()) {}
};

PassengerServiceCapture::PassengerServiceCapture(size_t maximum_bytes, size_t prediction_maximum_bytes) :
		maximum_bytes(std::min(maximum_bytes, MAXIMUM_BYTES)), prediction_maximum_bytes(std::min(prediction_maximum_bytes, MAXIMUM_PREDICTION_BYTES)) {}
PassengerServiceCapture::~PassengerServiceCapture() = default;
void PassengerServiceCapture::Clear() { this->data.reset(); }
size_t PassengerServiceCapture::GetPredictionBytes() const { return this->data ? this->data->prediction_bytes : 0; }
size_t PassengerServiceCapture::GetAllocationBytes() const
{
	if (!this->data) return 0;
	size_t bytes = sizeof(Data);
	if (this->data->discovery) {
		const auto &d = *this->data->discovery;
		bytes += d.fleets.capacity() * sizeof(Data::Fleet) + d.eligible.capacity() * sizeof(OrderListID) + d.inputs.capacity() * sizeof(PassengerDiscoveryInput);
		for (const auto &f : d.fleets) bytes += f.candidates.capacity() * sizeof(Data::Candidate);
	}
	for (const auto &[key, entry] : this->data->patterns) {
		const auto &p = *entry.value;
		bytes += 128 + sizeof(p) + p.orders.capacity() * sizeof(Order) + p.stops.capacity() * sizeof(PassengerServiceGraph::Stop) +
				p.segments.capacity() * sizeof(PassengerServiceGraph::Segment) + p.reverse_waypoints.capacity() * sizeof(VehicleOrderID) + p.schedules.capacity() * sizeof(DispatchSchedule);
		/* Every copied Order can own extra cargo flags. Count the upper bound. */
		bytes += p.orders.size() * sizeof(OrderExtraInfo);
		for (const auto &s : p.schedules) bytes += s.GetScheduledDispatch().capacity() * sizeof(DispatchSlot);
	}
	return bytes + this->GetPredictionBytes();
}

/** Aggregate sub-microsecond phases before rounding; no per-order clock calls. */
class PassengerGraphBuildTimer {
	using Clock = std::chrono::steady_clock;
	Clock::time_point last = Clock::now();
	PassengerGraphBuildPhase phase = PassengerGraphBuildPhase::Discovery;
	std::array<Clock::duration, static_cast<size_t>(PassengerGraphBuildPhase::End)> elapsed{};
public:
	void Set(PassengerGraphBuildPhase next)
	{
		if (next == this->phase) return;
		auto now = Clock::now();
		this->elapsed[static_cast<size_t>(this->phase)] += now - this->last;
		this->last = now; this->phase = next;
	}
	PassengerGraphPhaseTimes Finish()
	{
		this->elapsed[static_cast<size_t>(this->phase)] += Clock::now() - this->last;
		PassengerGraphPhaseTimes result{};
		for (size_t i = 0; i < result.size(); ++i) result[i] = std::chrono::duration_cast<std::chrono::microseconds>(this->elapsed[i]).count();
		return result;
	}
};

PassengerServiceGraph PassengerServiceGraph::Build(CargoType cargo, StateTicks now, PassengerServiceGraphLimits limits,
		std::optional<PassengerServiceScope> scope, PassengerServiceCapture *capture, PassengerTimingPolicy timing)
{
	const auto started = std::chrono::steady_clock::now();
	PassengerGraphBuildTimer timer;
	if (capture && std::min(capture->maximum_bytes, limits.memory_bytes) >= sizeof(PassengerServiceCapture::Data)) {
		if (capture->data && (capture->data->at != now || capture->data->revision != PassengerGraphServiceIndexRevision())) capture->Clear();
		/* Timetable/cargo flags and dispatch history can change without the topology
		 * revision changing. Reject both preprocessing and trip reuse in that case. */
		if (capture->data) for (const auto &[key, pattern] : capture->data->patterns) {
			const OrderList *list = OrderList::GetIfValid(pattern.value->id);
			if (list == nullptr || !SamePredictionPattern(*pattern.value, *list)) { capture->Clear(); break; }
		}
		if (!capture->data) capture->data = std::make_unique<PassengerServiceCapture::Data>(now);
	} else {
		capture = nullptr;
	}
	PassengerServiceGraph graph;
	graph.aircraft_cost = PassengerAircraftCost::Current();
	graph.cargo = cargo; graph.start = graph.end = now; graph.scope = scope;
	limits.work = std::min<size_t>(limits.work, 1 << 24);
	limits.services = std::min<size_t>(limits.services, 65536);
	limits.trips = std::min<size_t>(limits.trips, 65536);
	limits.total_visits = std::min<size_t>(limits.total_visits, 1 << 20);
	limits.visits_per_trip = std::min<size_t>(limits.visits_per_trip, 4096);
	limits.steps_per_trip = std::min<size_t>(limits.steps_per_trip, 65536);
	limits.horizon = std::clamp<Ticks>(limits.horizon, 0, 1 << 24);
	if (cargo >= NUM_CARGO || !IsCargoInClass(cargo, CargoClass::Passengers) || now.base() < 0 || now.base() > INT64_MAX - (int64_t{1} << 50)) {
		graph.status = PassengerPredictionStatus::Unknown;
		return graph;
	}
	graph.end += limits.horizon;
	size_t budget = limits.work, remaining_visits = limits.total_visits;
	size_t resident_bound = 0;
	static_assert(3 * sizeof(PassengerPredictedVisit) + 6 * sizeof(VisitID) + sizeof(int64_t) <= 320);
	static_assert(3 * sizeof(Trip) + 3 * sizeof(size_t) + 128 <= 1024);
	size_t predicting_visits = 0;
	PassengerGraphStage stage = PassengerGraphStage::Discovery;
	auto fail = [&](PassengerGraphFailure reason, PassengerPredictionStatus status) {
		graph.status = MergeGraphStatus(graph.status, status);
		if (graph.diagnostic.first_failure.reason == PassengerGraphFailure::None) {
			graph.diagnostic.first_failure = {reason, stage, limits.work - budget, resident_bound, graph.trips.size(), graph.diagnostic.visits + predicting_visits};
		}
	};
	/* Logical bytes reserve owned values, geometric vector growth, station/map indices,
	 * component/uncertainty scratch and predictor scratch BEFORE allocation. Traversal,
	 * including repeated circuits/dispatch searches, consumes work but not stored bytes.
	 * These constants are portable envelopes, never allocator capacity observations. */
	auto admit = [&](size_t bytes) {
		if (bytes > limits.memory_bytes - resident_bound) {
			fail(PassengerGraphFailure::Memory, PassengerPredictionStatus::LimitReached); return false;
		}
		resident_bound += bytes;
		graph.memory_bound = std::max(graph.memory_bound, resident_bound);
		return true;
	};
	struct Candidate { const Vehicle *vehicle; uint64_t capacity; uint64_t occupants; };
	struct FleetService { const OrderList *list; std::vector<Candidate> candidates; };
	std::vector<FleetService> fleets;
	std::vector<OrderListID> eligible;
	bool reused_discovery = false;
	if (capture && capture->data->discovery) {
		const auto &saved = *capture->data->discovery;
		/* A smaller allowance must execute the original traversal, including its
		 * partial diagnostics and exact first failure. Never replay a partial cache. */
		if (saved.cargo == cargo && saved.scoped == scope.has_value() && saved.work <= budget && saved.memory <= limits.memory_bytes) {
			size_t input = 0;
			reused_discovery = VisitPassengerDiscoveryInputs(cargo, [&](const auto &value) { return input < saved.inputs.size() && saved.inputs[input++] == value; }) && input == saved.inputs.size();
			if (reused_discovery) for (const auto &f : saved.fleets) {
				const OrderList *list = OrderList::GetIfValid(f.list);
				if (list == nullptr) { reused_discovery = false; break; }
				std::vector<Candidate> candidates;
				candidates.reserve(f.candidates.size());
				size_t c = 0;
				timer.Set(PassengerGraphBuildPhase::Capacity);
				for (const Vehicle *v = list->GetFirstSharedVehicle(); v != nullptr && c < f.candidates.size(); v = v->NextShared()) {
					if (v->index != f.candidates[c].vehicle) continue;
					/* Staging may alter packet metadata. Occupancy and compartment capacity
					 * are observed live, without charging the discovery work a second time. */
					uint64_t capacity = 0, occupants = 0;
					for (const Vehicle *u = v; u != nullptr; u = u->Next()) if (u->cargo_type == cargo) { capacity += u->cargo_cap; occupants += u->cargo.TotalCount(); }
					if (capacity != f.candidates[c].capacity || (scope && v->vehstatus.Any({VehState::Stopped, VehState::Crashed}))) { reused_discovery = false; break; }
					candidates.push_back({v, capacity, occupants}); ++c;
				}
				if (!reused_discovery || c != f.candidates.size()) { reused_discovery = false; break; }
				fleets.push_back({list, std::move(candidates)});
			}
			if (reused_discovery) {
				budget -= saved.work; admit(saved.memory);
				eligible = saved.eligible;
				graph.diagnostic.trains = saved.trains;
				graph.diagnostic.cargo_capable = saved.cargo_capable;
				graph.diagnostic.possible_refit = saved.possible_refit;
				graph.diagnostic.inactive = saved.inactive;
				graph.diagnostic.excluded_cargo = saved.excluded_cargo;
				graph.diagnostic.capture_discovery_hit = true;
				graph.diagnostic.capture_reused_work = saved.work;
			} else {
				fleets.clear(); capture->Clear(); capture->data = std::make_unique<PassengerServiceCapture::Data>(now);
			}
		}
	}
	/* Read cargo/refit eligibility before building topology: freight must not join
	 * otherwise unrelated passenger corridors. These pointers live only in Build. */
	if (!reused_discovery) for (const OrderList *list : OrderList::Iterate()) {
		timer.Set(PassengerGraphBuildPhase::Discovery);
		if (!SpendGraphWork(budget)) { fail(PassengerGraphFailure::Work, PassengerPredictionStatus::BudgetExhausted); break; }
		const Vehicle *first = list->GetFirstSharedVehicle();
		if (!IsPassengerServiceVehicle(first)) continue;
		timer.Set(PassengerGraphBuildPhase::Capacity);
		/* Unscoped snapshots expose actual capacity only. Exclude freight before
		 * reading orders or dispatch slots; scoped routing must also retain future
		 * refits as uncertain alternatives, so it checks the declared pattern below. */
		if (!scope) {
			bool passenger_service = false;
			size_t excluded = 0;
			for (const Vehicle *v = first; v != nullptr; v = v->NextShared()) {
				if (!SpendGraphWork(budget)) break;
				if (!IsPassengerServiceVehicle(v)) continue;
				if (HasPassengerVehicleCapacity(v, cargo, budget)) { passenger_service = true; break; }
				++excluded;
				if (budget == 0) break;
			}
			if (budget == 0) { fail(PassengerGraphFailure::Work, PassengerPredictionStatus::BudgetExhausted); break; }
			if (!passenger_service) {
				graph.diagnostic.trains += excluded;
				graph.diagnostic.excluded_cargo += excluded;
				continue;
			}
		}
		bool may_refit = false;
		if (!SpendGraphWork(budget, list->GetNumOrders())) { fail(PassengerGraphFailure::Work, PassengerPredictionStatus::BudgetExhausted); break; }
		for (const Order &order : list->GetOrderVector()) {
			may_refit |= order.IsRefit();
		}
		std::vector<Candidate> candidates;
		for (const Vehicle *v = first; v != nullptr; v = v->NextShared()) {
			if (!SpendGraphWork(budget)) { fail(PassengerGraphFailure::Work, PassengerPredictionStatus::BudgetExhausted); break; }
			if (!IsPassengerServiceVehicle(v)) continue;
			++graph.diagnostic.trains;
			uint64_t capacity = 0, occupants = 0;
			bool captured = true;
			for (const Vehicle *u = v; u != nullptr; u = u->Next()) {
				if (!SpendGraphWork(budget)) { captured = false; break; }
				if (u->cargo_type == cargo) { capacity += u->cargo_cap; occupants += u->cargo.TotalCount(); }
			}
			if (!captured) { fail(PassengerGraphFailure::Work, PassengerPredictionStatus::BudgetExhausted); ++graph.diagnostic.omitted_trains; break; }
			if (capacity != 0) ++graph.diagnostic.cargo_capable;
			else if (scope && (may_refit || v->current_order.IsRefit())) ++graph.diagnostic.possible_refit;
			else { ++graph.diagnostic.excluded_cargo; continue; }
			if (scope && v->vehstatus.Any({VehState::Stopped, VehState::Crashed})) { ++graph.diagnostic.inactive; continue; }
			if (!admit(128)) break; // Temporary candidate vector, including geometric growth.
			candidates.push_back({v, capacity, occupants});
		}
		if (graph.status != PassengerPredictionStatus::Complete) break;
		if (candidates.empty()) continue;
		if (!admit(256)) break; // Fleet discovery metadata, vector growth and eligible IDs.
		eligible.push_back(list->index);
		fleets.push_back({list, std::move(candidates)});
	}
	if (capture && !reused_discovery && graph.status == PassengerPredictionStatus::Complete) {
		/* The discovery envelope already funds both local pointer scratch and these
		 * much smaller owned ID records. No new logical bytes or work are admitted. */
		const size_t old = capture->data->discovery ? capture->data->discovery->storage : 0;
		const size_t available = capture->maximum_bytes - (capture->data->storage_bound - old);
		std::vector<PassengerDiscoveryInput> inputs;
		const bool guarded = resident_bound + sizeof(PassengerServiceCapture::Data) <= available && VisitPassengerDiscoveryInputs(cargo, [&](const auto &value) {
			if (2 * (inputs.size() + 1) * sizeof(PassengerDiscoveryInput) > available - resident_bound - sizeof(PassengerServiceCapture::Data)) return false;
			inputs.push_back(value); return true;
		});
		if (guarded) {
			const size_t storage = resident_bound + 2 * inputs.size() * sizeof(PassengerDiscoveryInput);
			PassengerServiceCapture::Data::Discovery saved{cargo, scope.has_value(), limits.work - budget, resident_bound, storage,
					graph.diagnostic.trains, graph.diagnostic.cargo_capable, graph.diagnostic.possible_refit, graph.diagnostic.inactive, graph.diagnostic.excluded_cargo, {}, eligible, std::move(inputs)};
			saved.fleets.reserve(fleets.size());
			for (const auto &f : fleets) {
				PassengerServiceCapture::Data::Fleet ids{f.list->index, {}};
				ids.candidates.reserve(f.candidates.size());
				for (const auto &c : f.candidates) ids.candidates.push_back({c.vehicle->index, c.capacity});
				saved.fleets.push_back(std::move(ids));
			}
			capture->data->discovery = std::move(saved);
			capture->data->storage_bound = capture->data->storage_bound - old + storage;
		}
	}
	timer.Set(PassengerGraphBuildPhase::Selection);
	PassengerServiceSelection selection;
	graph.diagnostic.eligible_services = eligible.size();
	if (scope && graph.status == PassengerPredictionStatus::Complete) {
		stage = PassengerGraphStage::Structure;
		selection = SelectPassengerServices(cargo, *scope, eligible, budget, limits.memory_bytes - resident_bound);
		graph.status = selection.status;
		if (selection.status != PassengerPredictionStatus::Complete) fail(selection.status == PassengerPredictionStatus::BudgetExhausted ?
				PassengerGraphFailure::Work : PassengerGraphFailure::Index, selection.status);
		graph.diagnostic.static_cache_hit = selection.cache_hit;
		graph.diagnostic.static_allocation_bytes = selection.allocation_bytes;
		graph.diagnostic.reused_work = selection.reused_work;
		graph.diagnostic.largest_block_services = selection.largest_block_services;
		if (selection.status == PassengerPredictionStatus::Complete) {
			/* Discovery coexists with index scratch; scratch is gone before dynamic
			 * capture. Charge the same peak warm, independently of cache residency. */
			graph.memory_bound = std::max(graph.memory_bound, resident_bound + selection.memory_bound);
			admit(selection.resident_bound);
		}
	}
	if (graph.status == PassengerPredictionStatus::Complete) for (const FleetService &fleet : fleets) {
		if (!scope || selection.services.contains(fleet.list->index)) {
			++graph.diagnostic.selected_services;
			graph.diagnostic.selected_trips += fleet.candidates.size();
		}
	}
	if (graph.status == PassengerPredictionStatus::Complete) for (FleetService &fleet : fleets) {
		timer.Set(PassengerGraphBuildPhase::Orders);
		stage = PassengerGraphStage::Orders;
		const OrderList *list = fleet.list;
		auto &candidates = fleet.candidates;
		if (scope && !selection.services.contains(list->index)) {
			graph.scoped_omissions = true;
			graph.diagnostic.excluded_scope += candidates.size();
			continue;
		}
		/* Implicit orders are historical records. Follow explicit instructions;
		 * real deviations and order-index changes are checked at execution. */
		if (graph.services.size() >= limits.services) { fail(PassengerGraphFailure::Services, PassengerPredictionStatus::LimitReached); graph.diagnostic.omitted_trains += candidates.size(); break; }
		if (!SpendGraphWork(budget, 3 * static_cast<size_t>(list->GetNumOrders()))) { fail(PassengerGraphFailure::Work, PassengerPredictionStatus::BudgetExhausted); graph.diagnostic.omitted_trains += candidates.size(); break; }
		if (!admit(512 + 1024 * static_cast<size_t>(list->GetNumOrders()))) break;
		const auto key = std::tuple(cargo, list->index, limits.steps_per_trip, timing);
		const PassengerServiceCapture::Data::Pattern *cached = nullptr;
		if (capture) {
			auto found = capture->data->patterns.find(key);
			if (found != capture->data->patterns.end()) cached = &found->second;
		}
		/* Cache retention has its own physical storage ceiling. Do not mistake the
		 * much larger per-graph logical envelope (including prediction scratch) for
		 * retained bytes, or large services would needlessly evict one another. */
		static_assert(sizeof(Order) + sizeof(OrderExtraInfo) + 2 * (sizeof(Stop) + sizeof(Segment) + sizeof(VehicleOrderID)) <= 256);
		size_t pattern_bytes = 512 + 256 * static_cast<size_t>(list->GetNumOrders());
		size_t copy_work = 3 * static_cast<size_t>(list->GetNumOrders());
		bool copied_schedules = true;
		stage = PassengerGraphStage::Schedules;
		for (const DispatchSchedule &source : list->GetScheduledDispatchScheduleSet()) {
			if (!SpendGraphWork(budget, 1 + source.GetScheduledDispatch().size())) { fail(PassengerGraphFailure::Work, PassengerPredictionStatus::BudgetExhausted); copied_schedules = false; break; }
			if (!admit(512 + 64 * source.GetScheduledDispatch().size())) { copied_schedules = false; break; }
			pattern_bytes += 512 + 64 * source.GetScheduledDispatch().size();
			copy_work += 1 + source.GetScheduledDispatch().size();
		}
		if (!copied_schedules) break;
		std::shared_ptr<const ServicePattern> structure;
		if (cached && cached->work <= budget) {
			timer.Set(PassengerGraphBuildPhase::Pattern);
			structure = cached->value; budget -= cached->work;
			++graph.diagnostic.capture_service_hits;
			graph.diagnostic.capture_reused_work += copy_work + cached->work;
		} else {
			auto built = std::make_shared<ServicePattern>();
			auto &service = *built;
			service.id = list->index;
			service.orders = list->GetOrderVector();
			const auto &orders = service.orders;
			for (const DispatchSchedule &source : list->GetScheduledDispatchScheduleSet()) {
				DispatchSchedule schedule;
				schedule.SetScheduledDispatchStartTick(source.GetScheduledDispatchStartTick());
				schedule.SetScheduledDispatchDuration(source.GetScheduledDispatchDuration());
				schedule.SetScheduledDispatchLastDispatch(source.GetScheduledDispatchLastDispatch());
				schedule.SetScheduledDispatchDelay(source.GetScheduledDispatchDelay());
				schedule.SetScheduledDispatchReuseSlots(source.GetScheduledDispatchReuseSlots());
				schedule.GetScheduledDispatchMutable() = source.GetScheduledDispatch();
				service.schedules.push_back(std::move(schedule));
			}
			timer.Set(PassengerGraphBuildPhase::Pattern);
			const size_t before_pattern = budget;
			for (VehicleOrderID i = 0; i < orders.size(); ++i) {
				const Order &order = orders[i];
				if (order.IsType(OT_GOTO_WAYPOINT) && order.GetWaypointFlags().Test(OrderWaypointFlag::Reverse)) service.reverse_waypoints.push_back(i);
				if (!order.IsType(OT_GOTO_STATION) || (order.GetNonStopType() & ONSF_NO_STOP_AT_DESTINATION_STATION)) continue;
				const OrderUnloadType unload = order.GetCargoUnloadType(cargo);
				service.stops.push_back({ order.GetDestination().ToStationID(), i, order.GetCargoLoadType(cargo) != OrderLoadType::NoLoad,
						unload != OrderUnloadType::NoUnload, unload == OrderUnloadType::Unload || unload == OrderUnloadType::Transfer,
						order.IsWaitTimetabled() ? std::optional(order.GetWaitTime()) : std::nullopt, order.GetDispatchScheduleIndex() });
			}
			if (!service.stops.empty()) {
				stage = PassengerGraphStage::Pattern;
				PassengerRouteState nominal{ orders, {}, service.stops.front().order, 0, 0, true, false, 1 };
				auto pattern = PredictPassengerVisits(nominal, cargo, StateTicks{0}, StateTicks{int64_t{1} << 50}, service.stops.size() + 1,
						limits.steps_per_trip, budget, {}, false, timing);
				service.pattern_status = pattern.status;
				for (size_t i = 1; i < pattern.visits.size(); ++i) {
					const auto &prev = pattern.visits[i - 1], &visit = pattern.visits[i];
					service.segments.push_back({ prev.order, visit.order, visit.travel_from_previous });
					if (visit.order == service.stops.front().order) { service.cycle_known = true; service.pattern_status = PassengerPredictionStatus::Complete; break; }
				}
				if (pattern.status == PassengerPredictionStatus::BudgetExhausted) { fail(PassengerGraphFailure::Work, pattern.status); break; }
			}
			structure = std::move(built);
			if (capture && cached == nullptr && pattern_bytes + sizeof(PassengerServiceCapture::Data) <= capture->maximum_bytes - capture->data->storage_bound) {
				capture->data->patterns.emplace(key, PassengerServiceCapture::Data::Pattern{structure, before_pattern - budget});
				capture->data->storage_bound += pattern_bytes;
			}
		}
		Service service{structure->id, structure, structure->stops, structure->segments, structure->pattern_status,
				structure->cycle_known, structure->reverse_waypoints, structure->schedules, {}};
		const auto &orders = structure->orders;
		timer.Set(PassengerGraphBuildPhase::Prediction);
		graph.service_index.emplace(service.id, graph.services.size());
		graph.services.push_back(std::move(service));
		Service &stored = graph.services.back();
		bool can_board = scope && scope->onboard_service == stored.id, can_alight = false;
		for (const Stop &stop : stored.stops) {
			can_board |= stop.can_board; can_alight |= stop.can_alight;
			if (!stop.can_board && !stop.can_alight) continue;
			auto &ids = graph.station_services[stop.station];
			if (ids.empty() || ids.back() != stored.id) ids.push_back(stored.id);
		}
		/* Legacy snapshots keep capture order. Scoped snapshots rotate each fleet's
		 * starting vehicle deterministically; a tight allowance cannot always favour its head. */
		if (scope && !candidates.empty()) {
			const size_t offset = (static_cast<uint64_t>(now.base() / 64) + scope->origin.base() + scope->destination.base()) % candidates.size();
			std::rotate(candidates.begin(), candidates.begin() + offset, candidates.end());
		}
		for (size_t c = 0; c < candidates.size(); ++c) {
			stage = PassengerGraphStage::Trips;
			const auto &[v, capacity, occupants] = candidates[c];
			if (graph.trips.size() >= limits.trips) { fail(PassengerGraphFailure::Trips, PassengerPredictionStatus::LimitReached); graph.diagnostic.omitted_trains += candidates.size() - c; break; }
			const size_t visit_limit = std::min(remaining_visits, limits.visits_per_trip);
			Operation operation = v->vehstatus.Test(VehState::Crashed) ? Operation::Crashed : v->vehstatus.Test(VehState::Stopped) ? Operation::Stopped : Operation::Running;
			if (!admit(1024)) { graph.diagnostic.omitted_trains += candidates.size() - c; break; }
			Trip trip{ v->index, stored.id, v->tile, v->GetMovingDirection(), v->cur_real_order_index, v->cur_implicit_order_index,
					v->cur_timetable_order_index, v->current_order_time, v->lateness_counter, v->current_order.IsAnyLoadingType(),
					v->vehicle_flags.Test(VehicleFlag::ScheduledDispatch), capacity, operation, {} };
			trip.occupants = occupants; trip.current_free = capacity - std::min(capacity, occupants);
			trip.type = v->type;
			/* Scope selection includes the onboard service even on a dead-end excursion.
			 * Only the actual OD corridor can justify discount-driven horizon extension.
			 * Do not require a predicted flight here: its first departure can be beyond end.
			 * An unscoped graph may also serve an onboard query at a NoLoad stop. */
			if (graph.aircraft_cost.scale < 100 && trip.type == VehicleType::Aircraft && operation == Operation::Running && capacity != 0 &&
					can_alight && (!scope || (can_board && (stored.id != scope->onboard_service || selection.onboard_on_corridor)))) graph.has_aircraft_discount = true;
			trip.prediction.start = trip.prediction.complete_until = now;
			trip.prediction.stopped_at = now;
			if (operation == Operation::Running && capacity == 0 && v->current_order.IsRefit()) {
				/* A current refit outside the declared pattern can create seats before
				 * departure. Capture uncertainty instead of certifying zero future capacity. */
				trip.prediction.stop = PassengerPredictionStop::Refit;
			} else if (operation == Operation::Running) {
				stage = PassengerGraphStage::Prediction;
				auto state = GetPassengerRouteState(v, &budget, &trip.prediction_reason);
				if (state) {
					state->orders = orders; state->schedules = stored.schedules;
					++graph.diagnostic.predicted;
					auto admit_prediction_visit = [&]() {
						if (!admit(320)) return false;
						++graph.diagnostic.admitted_visits;
						++predicting_visits;
						return true;
					};
					const auto prediction_key = std::pair(v->index, cargo);
					const PassengerServiceCapture::Data::Prediction *cached_prediction = nullptr;
					if (capture && capture->prediction_maximum_bytes != 0) {
						auto found = capture->data->predictions.find(prediction_key);
						if (found != capture->data->predictions.end()) {
							const auto &p = found->second;
							/* Pattern ownership is an identity only after its complete live value
							 * validation above. Route-state/facility checks still run and pay work. */
							if (p.pattern == structure && p.trip.prediction.start == now && p.end == graph.end && p.max_visits == visit_limit &&
									p.max_steps == limits.steps_per_trip && p.prefix_only == scope.has_value() && PredictionTripInputs(p.trip) == PredictionTripInputs(trip) &&
									p.current_order.EqualsForPassengerPrediction(v->current_order) && p.usable_stops == state->usable_stops && p.shared_vehicles == state->shared_vehicles &&
									std::tie(p.x, p.y, p.z, p.facing, p.vehicle_cargo) == std::tie(v->x_pos, v->y_pos, v->z_pos, v->direction, v->cargo_type) &&
									p.trip.prediction_work <= budget && p.trip.prediction.visits.size() <= (limits.memory_bytes - resident_bound) / 320) cached_prediction = &p;
						}
					}
					const size_t before_prediction = budget;
					if (cached_prediction) {
						/* Only non-resource-stopped results are resident. With sufficient work
						 * and bytes, every original admission succeeds and the debit is exact.
						 * Otherwise run the engine: admission/failure interleaving matters. */
						budget -= cached_prediction->trip.prediction_work;
						for (size_t i = 0; i < cached_prediction->trip.prediction.visits.size(); ++i) admit_prediction_visit();
						trip.prediction = cached_prediction->trip.prediction;
						trip.prediction_cache_hit = true;
						++graph.diagnostic.prediction_cache_hits;
					} else {
						const auto prediction_started = std::chrono::steady_clock::now();
						trip.prediction = PredictPassengerVisits(*state, cargo, now, graph.end, visit_limit, limits.steps_per_trip, budget, admit_prediction_visit, scope.has_value(), timing);
						trip.prediction_nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - prediction_started).count();
						++graph.diagnostic.prediction_executions;
						trip.prediction_executed = true;
						graph.diagnostic.prediction_executed_visits += trip.prediction.visits.size();
						graph.diagnostic.prediction_nanoseconds += trip.prediction_nanoseconds;
					}
					trip.prediction_work = before_prediction - budget;
					graph.diagnostic.prediction_work += trip.prediction_work;
					if (capture && !cached_prediction && trip.prediction.status < PassengerPredictionStatus::LimitReached) {
						/* The immutable pattern must already be funded by the capture; a trip
						 * cannot keep an unaccounted pattern alive. Full/tiny caches just miss. */
						const auto resident_pattern = capture->data->patterns.find(key);
						const size_t bytes = 1024 + 2 * state->usable_stops.size() + 2 * trip.prediction.visits.size() * sizeof(PassengerPredictedVisit);
						const auto previous = capture->data->predictions.find(prediction_key);
						const size_t old = previous == capture->data->predictions.end() ? 0 : previous->second.bytes;
						if (resident_pattern != capture->data->patterns.end() && resident_pattern->second.value == structure &&
								bytes <= capture->prediction_maximum_bytes - (capture->data->prediction_bytes - old) &&
								bytes + sizeof(PassengerServiceCapture::Data) <= capture->maximum_bytes - (capture->data->storage_bound - old)) {
							PassengerServiceCapture::Data::Prediction saved{structure, trip, v->current_order, state->usable_stops, graph.end,
									visit_limit, limits.steps_per_trip, bytes, state->shared_vehicles, v->x_pos, v->y_pos, v->z_pos, v->direction, v->cargo_type, scope.has_value()};
							capture->data->predictions.insert_or_assign(prediction_key, std::move(saved));
							capture->data->prediction_bytes = capture->data->prediction_bytes - old + bytes;
							capture->data->storage_bound = capture->data->storage_bound - old + bytes;
						}
					}
					if (trip.prediction.status == PassengerPredictionStatus::LimitReached && trip.prediction.visits.size() == visit_limit && visit_limit < limits.visits_per_trip) {
						fail(PassengerGraphFailure::TotalVisits, PassengerPredictionStatus::LimitReached);
					}
					if (trip.prediction.status != PassengerPredictionStatus::Complete && trip.prediction_reason == PassengerPredictionReason::None) {
						trip.prediction_reason = PassengerPredictionReason::OrderOrTiming;
					}
				} else {
					trip.operation = Operation::Unknown;
					trip.prediction.stop = trip.prediction_reason == PassengerPredictionReason::OrderProgress ? PassengerPredictionStop::OrderProgress :
							trip.prediction_reason == PassengerPredictionReason::StationFacilities ? PassengerPredictionStop::Facilities : PassengerPredictionStop::VehicleState;
					if (budget == 0) { trip.prediction.status = PassengerPredictionStatus::BudgetExhausted; trip.prediction.stop = PassengerPredictionStop::Work; }
				}
			} else {
				++graph.diagnostic.inactive;
				trip.prediction.status = PassengerPredictionStatus::Complete; trip.prediction.complete_until = graph.end;
			}
			remaining_visits -= trip.prediction.visits.size();
			graph.diagnostic.visits += trip.prediction.visits.size();
			predicting_visits = 0;
			if (trip.prediction.status >= PassengerPredictionStatus::LimitReached) ++graph.diagnostic.truncated_predictions;
			if (trip.prediction.status != PassengerPredictionStatus::Complete) {
				if (graph.diagnostic.first_prediction_failure.reason == PassengerPredictionStop::None) {
					graph.diagnostic.first_prediction_failure = {trip.prediction.stop, trip.vehicle, limits.work - budget, resident_bound,
							graph.trips.size() + 1, graph.diagnostic.visits, trip.prediction.stopped_at, trip.prediction.complete_until, graph.end,
							trip.prediction.stopped_order, trip.prediction.uncertain_order, trip.prediction.uncertain_reason, trip.tile, trip.current_order};
				}
				++graph.diagnostic.prediction_stops[static_cast<size_t>(trip.prediction.stop)];
				graph.diagnostic.maximum_unpredicted_ticks = std::max<Ticks>(graph.diagnostic.maximum_unpredicted_ticks, (graph.end - trip.prediction.complete_until).base());
			}
			if (trip.type == VehicleType::Aircraft && !graph.aircraft_cost.IsDefault()) {
				trip.aircraft_adjustments.resize(trip.prediction.visits.size());
				for (size_t i = 1; i < trip.prediction.visits.size(); ++i) {
					trip.aircraft_adjustments[i] = trip.aircraft_adjustments[i - 1] + graph.aircraft_cost.Adjustment(trip.prediction.visits[i].movement_from_previous.base());
				}
			}
			const size_t trip_index = graph.trips.size();
			stored.trips.push_back(trip_index); graph.trips.push_back(std::move(trip));
			const Trip &saved = graph.trips.back();
			/* Bounded missing timings/branches belong to this physical trip, not to
			 * every OD. Retain the certified prefix and inventory its missing suffix. */
			if (!saved.prediction.Covers(graph.start, graph.end)) {
				stored.coverage_status = MergeGraphStatus(stored.coverage_status, saved.prediction.status);
				if (!stored.uncertain_from || saved.prediction.complete_until < *stored.uncertain_from) stored.uncertain_from = saved.prediction.complete_until;
				if (saved.prediction.uncertain_reason == PassengerPredictionStop::Timing) ++graph.diagnostic.timing_uncertainty;
				else ++graph.diagnostic.path_uncertainty;
			}
			if (saved.prediction.status == PassengerPredictionStatus::BudgetExhausted) { fail(PassengerGraphFailure::Work, saved.prediction.status); graph.diagnostic.omitted_trains += candidates.size() - c - 1; break; }
		}
		if (graph.status != PassengerPredictionStatus::Complete) break;
	}
	stage = PassengerGraphStage::Indices;
	timer.Set(PassengerGraphBuildPhase::Departures);
	/* Failed capture cannot certify any query. Avoid sorting/indexing its prefix;
	 * declared stops and capture-order visits remain available for diagnostics. */
	graph.BuildDepartureIndex(graph.status < PassengerPredictionStatus::LimitReached);
	graph.diagnostic.phase_microseconds = timer.Finish();
	graph.work = limits.work - budget;
	/* Allocation observations and elapsed time are diagnostics, excluded from all decisions. */
	graph.diagnostic.allocation_bytes = sizeof(graph) + graph.services.capacity() * sizeof(Service) + graph.trips.capacity() * sizeof(Trip);
	for (const auto &service : graph.services) {
		const auto &p = *service.pattern;
		graph.diagnostic.allocation_bytes += sizeof(p) + p.orders.capacity() * sizeof(Order) + p.orders.size() * sizeof(OrderExtraInfo) +
				p.stops.capacity() * sizeof(Stop) + p.segments.capacity() * sizeof(Segment) + p.reverse_waypoints.capacity() * sizeof(VehicleOrderID) +
				p.schedules.capacity() * sizeof(DispatchSchedule) + service.trips.capacity() * sizeof(size_t);
		for (const auto &schedule : service.schedules) graph.diagnostic.allocation_bytes += schedule.GetScheduledDispatch().capacity() * sizeof(DispatchSlot);
	}
	for (const auto &trip : graph.trips) graph.diagnostic.allocation_bytes += trip.prediction.visits.capacity() * sizeof(PassengerPredictedVisit) +
			trip.aircraft_adjustments.capacity() * sizeof(int64_t);
	auto map_bytes = [](const auto &map) { return map.size() * (sizeof(typename std::decay_t<decltype(map)>::value_type) + 4 * sizeof(void *)); };
	graph.diagnostic.allocation_bytes += map_bytes(graph.service_index);
	for (const auto &[station, ids] : graph.station_services) graph.diagnostic.allocation_bytes += 96 + ids.capacity() * sizeof(OrderListID);
	graph.diagnostic.allocation_bytes += graph.station_index.capacity() * sizeof(StationIndex) +
			(graph.station_visits.capacity() + graph.timed_departures.capacity() + graph.unknown_departures.capacity()) * sizeof(VisitID) +
			graph.loading_visits.capacity() * sizeof(decltype(graph.loading_visits)::value_type) +
			graph.uncertain_components.capacity() * sizeof(UncertainComponent) +
			(graph.uncertain_stations.capacity() + graph.component_stations.capacity()) * sizeof(StationID);
	graph.diagnostic.capture_allocation_bytes = capture ? capture->GetAllocationBytes() : 0;
	graph.diagnostic.prediction_cache_bytes = capture ? capture->GetPredictionBytes() : 0;
	graph.diagnostic.build_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
	return graph;
}

PassengerPredictionStatus PassengerServiceGraph::GetStatus() const
{
	return this->scoped_omissions ? MergeGraphStatus(this->status, PassengerPredictionStatus::Unknown) : this->status;
}

PassengerPredictionStatus PassengerServiceGraph::GetQueryStatus(StationID origin, StationID destination, bool weighted_waiting) const
{
	if (origin == StationID::Invalid() || destination == StationID::Invalid()) return MergeGraphStatus(this->status, PassengerPredictionStatus::Unknown);
	if (this->scope && (this->scope->origin != origin || this->scope->destination != destination)) return MergeGraphStatus(this->status, PassengerPredictionStatus::Unknown);
	/* Weighted waiting can prefer riding an otherwise useless positive-time excursion.
	 * Active mode has zero waiting weight. Other evaluators need an unscoped graph to
	 * certify that these excluded excursions cannot improve their preference score. */
	if (weighted_waiting && this->scoped_omissions) return MergeGraphStatus(this->status, PassengerPredictionStatus::Unknown);
	return this->status;
}

PassengerServiceGraph::Coverage PassengerServiceGraph::GetCoverage(StationID origin, StationID destination) const
{
	if (this->GetQueryStatus(origin, destination) != PassengerPredictionStatus::Complete || this->GetUncertainStatus(destination) != PassengerPredictionStatus::Complete) return Coverage::Incomplete;
	return this->GetStatus() == PassengerPredictionStatus::Complete && this->uncertain_status == PassengerPredictionStatus::Complete ? Coverage::GlobalComplete : Coverage::ScopedComplete;
}

std::optional<PassengerServiceGraph::VisitID> PassengerServiceGraph::FindOnboardVisit(VehicleID vehicle, StationID station, VehicleOrderID order) const
{
	auto found = std::lower_bound(this->loading_visits.begin(), this->loading_visits.end(), vehicle,
			[](const auto &entry, VehicleID id) { return entry.first < id; });
	if (found == this->loading_visits.end() || found->first != vehicle || !found->second) return {};
	VisitID id = *found->second;
	const Trip &trip = this->trips[id.trip];
	const auto &visit = trip.prediction.visits[id.visit];
	if (trip.operation != Operation::Running || !trip.loading || trip.current_order != order ||
			visit.order != order || this->GetVisitStop(id).station != station) return {};
	return id;
}

/** Build once, without changing capture-order VisitIDs or legacy query ordering. */
void PassengerServiceGraph::BuildDepartureIndex(bool departures, IndexObserver observer)
{
	if (observer != nullptr) observer(IndexStage::Visits, true);
	/* Resolve declared stops once. Every station table is proportional to occupied
	 * stations, including declared stops with no predicted occurrences. */
	this->station_index.reserve(this->station_services.size());
	for (const auto &[station, services] : this->station_services) this->station_index.push_back({station});
	std::vector<size_t> stop_offsets(this->services.size() + 1);
	for (size_t s = 0; s < this->services.size(); ++s) stop_offsets[s + 1] = stop_offsets[s] + this->services[s].stops.size();
	std::vector<size_t> stop_stations(stop_offsets.back(), SIZE_MAX);
	for (size_t s = 0; s < this->services.size(); ++s) {
		for (size_t i = 0; i < this->services[s].stops.size(); ++i) {
			if (!this->services[s].stops[i].can_board && !this->services[s].stops[i].can_alight) continue;
			stop_stations[stop_offsets[s] + i] = this->FindStationIndex(this->services[s].stops[i].station) - this->station_index.data();
		}
	}
	enum class Kind : uint8_t { None, Timed, Unknown };
	struct Location { uint32_t station; Kind kind; };
	static_assert(3 * sizeof(PassengerPredictedVisit) + 6 * sizeof(VisitID) + sizeof(Location) <= 320);
	size_t visit_count = 0, loading_count = 0;
	for (const Trip &trip : this->trips) visit_count += trip.prediction.visits.size();
	std::vector<Location> locations;
	locations.reserve(visit_count);
	for (const Trip &trip : this->trips) {
		const size_t s = this->service_index.at(trip.service);
		const auto &stops = this->services[s].stops;
		for (const auto &visit : trip.prediction.visits) {
			auto stop = std::lower_bound(stops.begin(), stops.end(), visit.order, [](const Stop &stop, VehicleOrderID id) { return stop.order < id; });
			assert(stop != stops.end() && stop->order == visit.order);
			const size_t station = stop_stations[stop_offsets[s] + (stop - stops.begin())];
			/* Retain VisitIDs in each physical trip and the loading lookup, but
			 * omit cargo-closed occurrences from passenger station tables. */
			loading_count += visit.loading_now;
			if (station == SIZE_MAX) { locations.push_back({UINT32_MAX, Kind::None}); continue; }
			auto &index = this->station_index[station];
			++index.visits.count;
			Kind kind = Kind::None;
			if (departures && trip.operation == Operation::Running && stop->can_board && trip.capacity != 0) {
				kind = visit.departure ? Kind::Timed : Kind::Unknown;
				++(visit.departure ? index.timed : index.unknown).count;
			}
			locations.push_back({static_cast<uint32_t>(station), kind});
		}
	}
	size_t timed_count = 0, unknown_count = 0, offset = 0;
	for (auto &index : this->station_index) {
		index.visits.begin = offset; offset += index.visits.count;
		index.timed.begin = timed_count; timed_count += index.timed.count;
		index.unknown.begin = unknown_count; unknown_count += index.unknown.count;
	}
	this->station_visits.resize(offset);
	std::vector<size_t> cursors(this->station_index.size());
	size_t occurrence = 0;
	for (size_t t = 0; t < this->trips.size(); ++t) {
		for (size_t v = 0; v < this->trips[t].prediction.visits.size(); ++v) {
			const size_t station = locations[occurrence++].station;
			if (station == UINT32_MAX) continue;
			this->station_visits[this->station_index[station].visits.begin + cursors[station]++] = {t, v};
		}
	}
	if (observer != nullptr) observer(IndexStage::Visits, false);
	/* Failed captures historically retain station visits, but no departure,
	 * onboard or uncertainty index. Preserve that boundary and logical admission. */
	if (!departures) return;
	if (observer != nullptr) observer(IndexStage::Departures, true);
	for (size_t s = 0; s < this->services.size(); ++s) {
		for (size_t i = stop_offsets[s]; i < stop_offsets[s + 1]; ++i) {
			if (stop_stations[i] == SIZE_MAX) continue;
			auto &index = this->station_index[stop_stations[i]];
			index.status = MergeGraphStatus(index.status, this->services[s].coverage_status);
		}
	}
	this->timed_departures.resize(timed_count);
	this->unknown_departures.resize(unknown_count);
	std::fill(cursors.begin(), cursors.end(), 0);
	std::vector<size_t> unknown_cursors(cursors.size());
	occurrence = 0;
	for (size_t t = 0; t < this->trips.size(); ++t) {
		for (size_t v = 0; v < this->trips[t].prediction.visits.size(); ++v) {
			const auto &location = locations[occurrence++];
			if (location.station == UINT32_MAX) continue;
			const auto &index = this->station_index[location.station];
			if (location.kind == Kind::Timed) this->timed_departures[index.timed.begin + cursors[location.station]++] = {t, v};
			if (location.kind == Kind::Unknown) this->unknown_departures[index.unknown.begin + unknown_cursors[location.station]++] = {t, v};
		}
	}
	std::vector<Location>().swap(locations);
	std::vector<size_t>().swap(unknown_cursors);
	if (observer != nullptr) observer(IndexStage::Departures, false);
	if (observer != nullptr) observer(IndexStage::Sort, true);
	for (const auto &index : this->station_index) {
		auto timed = std::span(this->timed_departures).subspan(index.timed.begin, index.timed.count);
		std::sort(timed.begin(), timed.end(), [&](VisitID a, VisitID b) {
			const Trip &ta = this->trips[a.trip], &tb = this->trips[b.trip];
			return std::tuple(*ta.prediction.visits[a.visit].departure, ta.vehicle, a.trip, a.visit) <
					std::tuple(*tb.prediction.visits[b.visit].departure, tb.vehicle, b.trip, b.visit);
		});
	}
	if (observer != nullptr) observer(IndexStage::Sort, false);
	if (observer != nullptr) observer(IndexStage::Loading, true);
	this->loading_visits.reserve(loading_count);
	for (size_t t = 0; t < this->trips.size(); ++t) {
		for (size_t v = 0; v < this->trips[t].prediction.visits.size(); ++v) {
			if (this->trips[t].prediction.visits[v].loading_now) this->loading_visits.emplace_back(this->trips[t].vehicle, VisitID{t, v});
		}
	}
	std::sort(this->loading_visits.begin(), this->loading_visits.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	size_t unique = 0;
	for (size_t i = 0; i < this->loading_visits.size(); ++i) {
		if (unique != 0 && this->loading_visits[unique - 1].first == this->loading_visits[i].first) {
			this->loading_visits[unique - 1].second.reset(); // Every additional current event keeps it ambiguous.
		} else {
			this->loading_visits[unique++] = this->loading_visits[i];
		}
	}
	this->loading_visits.resize(unique);
	if (observer != nullptr) observer(IndexStage::Loading, false);
	if (observer != nullptr) observer(IndexStage::Components, true);
	/* Union exchange stops regardless of direction or timing.
	 * Array parents with path compression and rank retain the same overapproximation. */
	std::vector<uint8_t> ranks(this->station_index.size());
	for (size_t i = 0; i < this->station_index.size(); ++i) this->station_index[i].component = i;
	auto root = [&](size_t station) {
		size_t id = station;
		while (this->station_index[id].component != id) id = this->station_index[id].component;
		while (station != id) {
			size_t parent = this->station_index[station].component;
			this->station_index[station].component = id;
			station = parent;
		}
		return id;
	};
	for (size_t s = 0; s < this->services.size(); ++s) {
		size_t anchor = SIZE_MAX;
		for (size_t i = stop_offsets[s]; i < stop_offsets[s + 1]; ++i) {
			if (stop_stations[i] == SIZE_MAX) continue;
			if (anchor == SIZE_MAX) anchor = stop_stations[i];
			size_t a = root(anchor), b = root(stop_stations[i]);
			if (a == b) continue;
			if (ranks[a] < ranks[b]) std::swap(a, b);
			this->station_index[b].component = a;
			if (ranks[a] == ranks[b]) ++ranks[a];
		}
	}
	for (size_t i = 0; i < this->station_index.size(); ++i) this->station_index[i].component = root(i);
	std::vector<size_t>().swap(stop_offsets);
	std::vector<size_t>().swap(stop_stations);
	std::vector<uint8_t>().swap(ranks);
	if (observer != nullptr) observer(IndexStage::Components, false);
	if (observer != nullptr) observer(IndexStage::Uncertain, true);
	if (std::none_of(this->station_index.begin(), this->station_index.end(), [](const StationIndex &index) {
		return index.status != PassengerPredictionStatus::Complete || index.unknown.count != 0;
	})) {
		if (observer != nullptr) observer(IndexStage::Uncertain, false);
		return;
	}
	std::vector<UncertainComponent> components(this->station_index.size());
	size_t uncertain_count = 0;
	for (const auto &index : this->station_index) {
		auto status = index.unknown.count == 0 ? index.status : MergeGraphStatus(index.status, PassengerPredictionStatus::Unknown);
		if (status == PassengerPredictionStatus::Complete) continue;
		++uncertain_count;
		this->uncertain_status = MergeGraphStatus(this->uncertain_status, status);
		auto &component = components[index.component];
		++component.stations.count;
		component.status = MergeGraphStatus(component.status, status);
	}
	this->uncertain_stations.reserve(uncertain_count);
	this->component_stations.resize(uncertain_count);
	offset = 0;
	const size_t component_count = std::count_if(components.begin(), components.end(), [](const UncertainComponent &component) { return component.stations.count != 0; });
	this->uncertain_components.reserve(component_count);
	std::fill(cursors.begin(), cursors.end(), SIZE_MAX);
	for (size_t i = 0; i < components.size(); ++i) {
		auto &component = components[i];
		if (component.stations.count == 0) continue;
		component.stations.begin = offset; offset += component.stations.count;
		cursors[i] = this->uncertain_components.size();
		this->uncertain_components.push_back(component);
	}
	/* Retain only uncertain components. Components without uncertainty need no row. */
	for (auto &index : this->station_index) index.component = cursors[index.component];
	std::fill(cursors.begin(), cursors.end(), 0);
	for (const auto &index : this->station_index) {
		if (index.status == PassengerPredictionStatus::Complete && index.unknown.count == 0) continue;
		this->uncertain_stations.push_back(index.station);
		this->component_stations[this->uncertain_components[index.component].stations.begin + cursors[index.component]++] = index.station;
	}
	if (observer != nullptr) observer(IndexStage::Uncertain, false);
}

const PassengerServiceGraph::StationIndex *PassengerServiceGraph::FindStationIndex(StationID station) const
{
	auto it = std::lower_bound(this->station_index.begin(), this->station_index.end(), station,
			[](const StationIndex &index, StationID id) { return index.station < id; });
	return it == this->station_index.end() || it->station != station ? nullptr : &*it;
}

std::span<const StationID> PassengerServiceGraph::GetUncertainStations(StationID destination) const
{
	const auto *index = this->FindStationIndex(destination);
	if (index == nullptr || this->uncertain_components.empty() || index->component == SIZE_MAX) return {};
	const auto &range = this->uncertain_components[index->component].stations;
	return std::span(this->component_stations).subspan(range.begin, range.count);
}

PassengerPredictionStatus PassengerServiceGraph::GetUncertainStatus(StationID destination) const
{
	const auto *index = this->FindStationIndex(destination);
	return index == nullptr || this->uncertain_components.empty() || index->component == SIZE_MAX ? PassengerPredictionStatus::Complete : this->uncertain_components[index->component].status;
}

std::span<const PassengerServiceGraph::VisitID> PassengerServiceGraph::GetStationVisits(StationID station) const
{
	const auto *index = this->FindStationIndex(station);
	return index == nullptr ? std::span<const VisitID>{} : std::span(this->station_visits).subspan(index->visits.begin, index->visits.count);
}

PassengerServiceGraph::Departures PassengerServiceGraph::GetDepartures(StationID station, StateTicks from, StateTicks to, bool scoped_query) const
{
	Departures result{ {}, {}, scoped_query ? this->status : this->GetStatus() };
	if (from < this->start || to > this->end || from >= to) {
		result.status = MergeGraphStatus(result.status, PassengerPredictionStatus::Unknown);
		return result;
	}
	const auto *index = this->FindStationIndex(station);
	if (index == nullptr) return result;
	result.status = MergeGraphStatus(result.status, index->status);
	result.unknown = std::span(this->unknown_departures).subspan(index->unknown.begin, index->unknown.count);
	if (!result.unknown.empty()) result.status = MergeGraphStatus(result.status, PassengerPredictionStatus::Unknown);
	auto timed = std::span(this->timed_departures).subspan(index->timed.begin, index->timed.count);
	auto lower = [&](StateTicks time) {
		return std::lower_bound(timed.begin(), timed.end(), time, [&](VisitID id, StateTicks tick) {
			return *this->trips[id.trip].prediction.visits[id.visit].departure < tick;
		});
	};
	result.timed = std::span<const VisitID>(lower(from), lower(to));
	return result;
}

const PassengerServiceGraph::Service *PassengerServiceGraph::GetService(OrderListID id) const
{
	auto it = this->service_index.find(id);
	return it == this->service_index.end() ? nullptr : &this->services[it->second];
}

std::span<const OrderListID> PassengerServiceGraph::GetServicesAtStation(StationID station) const
{
	auto it = this->station_services.find(station);
	return it == this->station_services.end() ? std::span<const OrderListID>{} : it->second;
}

const PassengerServiceGraph::Stop &PassengerServiceGraph::GetStop(const Trip &trip, VehicleOrderID order) const
{
	const auto &stops = this->GetService(trip.service)->stops;
	auto it = std::lower_bound(stops.begin(), stops.end(), order, [](const Stop &stop, VehicleOrderID id) { return stop.order < id; });
	assert(it != stops.end() && it->order == order);
	return *it;
}

PassengerServiceGraph::QueryResult<PassengerServiceGraph::VisitID> PassengerServiceGraph::GetVisits(
		StationID station, StateTicks from, StateTicks to, size_t budget) const
{
	budget = std::min<size_t>(budget, 1 << 24);
	return this->SelectVisits(station, from, to, budget);
}

PassengerServiceGraph::QueryResult<PassengerServiceGraph::VisitID> PassengerServiceGraph::SelectVisits(
		StationID station, StateTicks from, StateTicks to, size_t &budget) const
{
	QueryResult<VisitID> result;
	result.status = this->GetStatus();
	if (from < this->start || to > this->end || from >= to) {
		result.status = MergeGraphStatus(result.status, PassengerPredictionStatus::Unknown);
		return result;
	}
	/* Even a trip whose dynamic branch prevented reaching this station makes absence uncertain. */
	for (OrderListID id : this->GetServicesAtStation(station)) {
		if (!SpendGraphWork(budget)) { result.status = PassengerPredictionStatus::BudgetExhausted; return result; }
		for (size_t index : this->GetService(id)->trips) {
			if (!SpendGraphWork(budget)) { result.status = PassengerPredictionStatus::BudgetExhausted; return result; }
			const auto &prediction = this->trips[index].prediction;
			if (!prediction.Covers(from, to)) result.status = MergeGraphStatus(result.status, prediction.status);
		}
	}
	for (VisitID id : this->GetStationVisits(station)) {
		if (!SpendGraphWork(budget)) { result.status = PassengerPredictionStatus::BudgetExhausted; return result; }
		const auto &prediction = this->trips[id.trip].prediction;
		const auto &visit = prediction.visits[id.visit];
		const bool covered = prediction.Covers(from, to);
		/* A currently loading arrival happened before the snapshot. Its absent historical
		 * timestamp does not create a possible future arrival in every query window. */
		const bool unknown_arrival = !covered && !visit.arrival && !visit.loading_now;
		const bool unknown_departure = !covered && !visit.departure;
		if (unknown_arrival || unknown_departure || (visit.arrival && *visit.arrival >= from && *visit.arrival < to) ||
				(visit.departure && *visit.departure >= from && *visit.departure < to)) {
			result.items.push_back(id);
			if (unknown_arrival || unknown_departure) result.status = MergeGraphStatus(result.status, PassengerPredictionStatus::Unknown);
		}
	}
	return result;
}

PassengerServiceGraph::QueryResult<PassengerServiceGraph::BoardableTrip> PassengerServiceGraph::GetBoardableTrips(
		StationID station, StateTicks from, StateTicks to, size_t budget) const
{
	budget = std::min<size_t>(budget, 1 << 24);
	QueryResult<BoardableTrip> result;
	auto visits = this->SelectVisits(station, from, to, budget);
	result.status = visits.status;
	for (VisitID id : visits.items) {
		if (!SpendGraphWork(budget)) { result.status = PassengerPredictionStatus::BudgetExhausted; break; }
		const Trip &trip = this->trips[id.trip];
		const auto &visit = trip.prediction.visits[id.visit];
		if (trip.capacity == 0 || !this->GetStop(trip, visit.order).can_board) continue;
		if (visit.departure && (*visit.departure < from || *visit.departure >= to)) continue;
		BoardableTrip boarding{ id, {}, trip.prediction.status };
		if (!visit.departure) boarding.status = MergeGraphStatus(boarding.status, PassengerPredictionStatus::Unknown);
		for (size_t i = id.visit + 1; i < trip.prediction.visits.size(); ++i) {
			if (!SpendGraphWork(budget)) { boarding.status = PassengerPredictionStatus::BudgetExhausted; break; }
			const auto &next = trip.prediction.visits[i];
			const Stop &stop = this->GetStop(trip, next.order);
			if (stop.can_alight) boarding.reachable.push_back({ id.trip, i });
			if (!next.arrival) boarding.status = MergeGraphStatus(boarding.status, PassengerPredictionStatus::Unknown);
			if (stop.forced_transfer) break;
		}
		/* A truncated future suffix does not invalidate complete boarding-window coverage.
		 * Its status stays on the candidate, but unfinished query work affects the whole result. */
		if (boarding.status == PassengerPredictionStatus::BudgetExhausted) result.status = boarding.status;
		result.items.push_back(std::move(boarding));
	}
	return result;
}
