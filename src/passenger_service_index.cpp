/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_service_index.cpp Iterative block decomposition of declared station/service incidence. */
#include "stdafx.h"
#include "passenger_service_index.h"
#include "vehicle_base.h"

#include "safeguards.h"

namespace {
	struct ServiceIndex {
		CargoType cargo;
		std::vector<OrderListID> eligible;
		size_t vertices = 0;
		size_t allocation_bytes = 0;
		size_t resident_bound = 0;
		size_t largest_block_services = 0;
		std::map<StationID, size_t> stations;
		std::map<size_t, OrderListID> services;
		std::vector<std::vector<size_t>> tree;
		std::vector<size_t> parent, depth, root;
	};
	static std::optional<ServiceIndex> service_index;
	static uint64_t revision = 0;

	/** O(orders + services) vertices/edges, no recursion even for national transfer chains. */
	static ServiceIndex BuildIndex(CargoType cargo, std::span<const OrderListID> eligible)
	{
		ServiceIndex result;
		result.cargo = cargo;
		result.eligible.assign(eligible.begin(), eligible.end());
		std::vector<std::vector<size_t>> edges;
		for (OrderListID id : eligible) {
			const OrderList *list = OrderList::Get(id);
			size_t service = edges.size();
			edges.emplace_back();
			result.services.emplace(service, list->index);
			for (const Order &order : list->GetOrderVector()) {
				/* Match the predictor: implicit orders record past, incidental stops.
				 * They are skipped, and future incidental stops are not forecast. */
				if (!HasPassengerExchange(order, cargo)) continue;
				if (order.GetDestination().ToStationID() == StationID::Invalid()) continue;
				auto [station, inserted] = result.stations.try_emplace(order.GetDestination().ToStationID(), edges.size());
				if (inserted) edges.emplace_back();
				edges[service].push_back(station->second);
			}
			auto &stops = edges[service];
			std::sort(stops.begin(), stops.end());
			stops.erase(std::unique(stops.begin(), stops.end()), stops.end());
			for (size_t station : stops) edges[station].push_back(service);
		}
		const size_t vertices = edges.size();
		result.vertices = vertices;
		result.tree.resize(vertices);
		std::vector<size_t> discovery(vertices, SIZE_MAX), low(vertices), parent(vertices, SIZE_MAX), next(vertices);
		std::vector<size_t> stack;
		std::vector<std::pair<size_t, size_t>> pending;
		size_t clock = 0;
		for (size_t start = 0; start < vertices; ++start) {
			if (discovery[start] != SIZE_MAX) continue;
			discovery[start] = low[start] = clock++;
			stack.push_back(start);
			while (!stack.empty()) {
				size_t v = stack.back();
				if (next[v] < edges[v].size()) {
					size_t w = edges[v][next[v]++];
					if (w == parent[v]) continue;
					if (discovery[w] == SIZE_MAX) {
						parent[w] = v;
						pending.emplace_back(v, w);
						discovery[w] = low[w] = clock++;
						stack.push_back(w);
					} else if (discovery[w] < discovery[v]) {
						pending.emplace_back(v, w);
						low[v] = std::min(low[v], discovery[w]);
					}
					continue;
				}
				stack.pop_back();
				if (parent[v] == SIZE_MAX) continue;
				size_t p = parent[v];
				low[p] = std::min(low[p], low[v]);
				if (low[v] < discovery[p]) continue;
				std::vector<size_t> members;
				std::pair<size_t, size_t> edge;
				do {
					edge = pending.back(); pending.pop_back();
					members.push_back(edge.first); members.push_back(edge.second);
				} while (edge != std::pair(p, v));
				std::sort(members.begin(), members.end());
				members.erase(std::unique(members.begin(), members.end()), members.end());
				size_t block = result.tree.size();
				result.tree.push_back(std::move(members));
				for (size_t member : result.tree[block]) result.tree[member].push_back(block);
			}
		}
		/* A service vertex is not a valid separator: alight from a local at X,
		 * use an express to Y, then board the same local pattern again. Contract
		 * blocks across service articulations so ONLY station articulations can
		 * exclude excursions. This keeps those faster local/express alternatives. */
		std::vector<size_t> groups(result.tree.size());
		for (size_t i = 0; i < groups.size(); ++i) groups[i] = i;
		auto group = [&](size_t node) {
			size_t root = node;
			while (groups[root] != root) root = groups[root];
			while (node != root) { size_t parent = groups[node]; groups[node] = root; node = parent; }
			return root;
		};
		for (const auto &[service, id] : result.services) {
			if (result.tree[service].empty()) continue;
			for (size_t block : result.tree[service]) groups[group(block)] = group(result.tree[service].front());
		}
		std::vector<std::vector<size_t>> contracted(vertices);
		std::map<size_t, size_t> blocks;
		for (size_t block = vertices; block < result.tree.size(); ++block) {
			auto [target, inserted] = blocks.try_emplace(group(block), contracted.size());
			if (inserted) contracted.emplace_back();
			for (size_t member : result.tree[block]) contracted[target->second].push_back(member);
		}
		for (size_t block = vertices; block < contracted.size(); ++block) {
			auto &members = contracted[block];
			std::sort(members.begin(), members.end());
			members.erase(std::unique(members.begin(), members.end()), members.end());
			size_t service_count = 0;
			for (size_t member : members) service_count += result.services.contains(member);
			result.largest_block_services = std::max(result.largest_block_services, service_count);
			for (size_t member : members) contracted[member].push_back(block);
		}
		result.tree = std::move(contracted);
		result.parent.assign(result.tree.size(), SIZE_MAX);
		result.depth.resize(result.tree.size());
		result.root.resize(result.tree.size());
		for (size_t start = 0; start < result.tree.size(); ++start) {
			if (result.parent[start] != SIZE_MAX) continue;
			result.parent[start] = start; result.root[start] = start;
			stack.push_back(start);
			while (!stack.empty()) {
				size_t v = stack.back(); stack.pop_back();
				for (size_t w : result.tree[v]) {
					if (result.parent[w] != SIZE_MAX) continue;
					result.parent[w] = v; result.depth[w] = result.depth[v] + 1; result.root[w] = start;
					stack.push_back(w);
				}
			}
		}
		result.allocation_bytes = sizeof(ServiceIndex) + result.eligible.capacity() * sizeof(OrderListID) + (result.stations.size() + result.services.size()) * 80 +
				result.tree.capacity() * sizeof(std::vector<size_t>) + (result.parent.capacity() + result.depth.capacity() + result.root.capacity()) * sizeof(size_t);
		for (const auto &neighbours : result.tree) result.allocation_bytes += neighbours.capacity() * sizeof(size_t);
		/* Logical sizes, never capacities: owned maps, vector growth, allocator
		 * overhead and a worst-case selection set. Decomposition scratch is dead. */
		result.resident_bound = 4096 + 128 * (result.stations.size() + 2 * result.services.size() + result.tree.size());
		for (const auto &neighbours : result.tree) result.resident_bound += 32 * neighbours.size();
		return result;
	}
}

void InvalidatePassengerGraphServiceIndex() { service_index.reset(); ++revision; }
uint64_t PassengerGraphServiceIndexRevision() { return revision; }

PassengerServiceSelection SelectPassengerServices(CargoType cargo, PassengerServiceScope scope, std::span<const OrderListID> eligible, size_t &work, size_t memory_bytes)
{
	PassengerServiceSelection result;
	/* Charge the same conservative linear preprocessing allowance on cold and warm calls.
	 * A join/load, cache eviction or diagnostics must never change routing allowances.
	 * Map insertion and sorting add logarithmic factors, as in the snapshot indices. */
	size_t allowance = 0, lists = 0;
	result.memory_bound = 4096;
	if (memory_bytes < result.memory_bound) { result.status = PassengerPredictionStatus::LimitReached; return result; }
	for (OrderListID id : eligible) {
		const OrderList *list = OrderList::Get(id);
		++lists;
		const size_t orders = list->GetNumOrders();
		const size_t cost = 8 + 16 * orders;
		if (cost > work - allowance) { work = 0; result.status = PassengerPredictionStatus::BudgetExhausted; return result; }
		allowance += cost;
		const size_t bytes = 1024 + 512 * orders;
		if (result.memory_bound > memory_bytes || bytes > memory_bytes - result.memory_bound) {
			work -= allowance; result.status = PassengerPredictionStatus::LimitReached; return result;
		}
		result.memory_bound += bytes;
	}
	work -= allowance;
	if (scope.onboard_service != OrderListID::Invalid()) result.services.insert(scope.onboard_service);
	result.cache_hit = service_index && service_index->cargo == cargo && std::equal(eligible.begin(), eligible.end(), service_index->eligible.begin(), service_index->eligible.end());
	if (result.cache_hit) result.reused_work = allowance - lists;
	if (!result.cache_hit) {
		/* Release the previous cargo/fleet view before allocating replacement scratch. */
		service_index.reset();
		service_index = BuildIndex(cargo, eligible);
	}
	result.allocation_bytes = service_index->allocation_bytes;
	result.resident_bound = service_index->resident_bound;
	result.largest_block_services = service_index->largest_block_services;
	auto include_service = [&](OrderListID id) {
		result.services.insert(id);
		if (id == scope.onboard_service) result.onboard_on_corridor = true;
	};
	auto from = service_index->stations.find(scope.origin), to = service_index->stations.find(scope.destination);
	if (from == service_index->stations.end() || to == service_index->stations.end() || service_index->root[from->second] != service_index->root[to->second]) return result;
	auto include = [&](size_t node) {
		if (work == 0) { result.status = PassengerPredictionStatus::BudgetExhausted; return false; }
		--work;
		/* Tree block nodes contain all cyclic alternatives, not just a single DFS path. */
		if (node >= service_index->vertices) for (size_t member : service_index->tree[node]) {
			if (work == 0) { result.status = PassengerPredictionStatus::BudgetExhausted; return false; }
			--work;
			auto service = service_index->services.find(member);
			if (service != service_index->services.end()) include_service(service->second);
		}
		auto service = service_index->services.find(node);
		if (service != service_index->services.end()) include_service(service->second);
		return true;
	};
	size_t a = from->second, b = to->second;
	while (a != b) {
		if (service_index->depth[a] < service_index->depth[b]) std::swap(a, b);
		if (!include(a)) return result;
		a = service_index->parent[a];
	}
	include(a);
	return result;
}
