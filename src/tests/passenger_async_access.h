/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
#ifndef TESTS_PASSENGER_ASYNC_ACCESS_H
#define TESTS_PASSENGER_ASYNC_ACCESS_H
#include "../passenger_precomputed.h"
#include "../passenger_service_database.h"
/** Compare every stored field and every shared publication side effect, including
 * partially copied inverse indices/masks. Search scratch can be virtual. */
struct PassengerPrecomputedTestAccess {
	template <typename Table, typename Sink> static void VisitPublished(const Table &table, Sink add)
	{
		auto array = [&](const auto &values) { add(values.size()); for (auto value : values) add(value); };
		auto labels = [&](const auto &values) {
			add(values.size());
			for (const auto &l : values) { add(l.cost); add(l.riding); add(l.waiting); add(l.first_wait); add(l.board); add(l.alight); add(l.legs); add(l.distance); }
		};
		add(table.destination); add(table.failed); add(table.diagnostic.operations); add(table.diagnostic.envelope);
		add(table.rows.size());
		for (const auto &row : table.rows) {
			add(row.bytes);
			labels(row.labels); labels(row.alternative); labels(row.exception_labels); array(row.indices);
			add(row.policies.size());
			for (const auto &p : row.policies) { add(p.cost); add(p.wait); array(p.candidates); }
			add(row.flat_policies.size());
			for (const auto &p : row.flat_policies) { add(p.cost); add(p.wait); add(p.offset); add(p.count); }
			array(row.flat_candidates);
		}
		array(table.boarding_mask); array(table.published_rows);
		add(table.reachable_destinations.size());
		for (const auto &destinations : table.reachable_destinations) { add(destinations.size()); for (auto d : destinations) add(d.base()); }
	}
	template <typename Table> static std::vector<uint64_t> Published(const Table &table)
	{
		std::vector<uint64_t> out; VisitPublished(table, [&](uint64_t value) { out.push_back(value); }); return out;
	}
	template <typename Table> static uint64_t Digest(const Table &table)
	{
		uint64_t digest = 0xcbf29ce484222325ULL;
		VisitPublished(table, [&](uint64_t value) { digest = (digest ^ value) * 0x100000001b3ULL; }); return digest;
	}
	template <typename Table> static void Hook(Table &table, void (*hook)(size_t, bool)) { table.parallel_search_hook = hook; }
	static void StartHook(PassengerPrecomputedTable &table, bool (*hook)(uint)) { table.parallel_start_hook = hook; }
	static void QuantumHook(PassengerPrecomputedTable &table, void (*hook)(size_t, uint64_t)) { table.parallel_quantum_hook = hook; }
	static bool Prime(PassengerPrecomputedTable &table, uint workers) { return table.PrepareParallelBatch(workers); }
	static bool Ready(const PassengerPrecomputedTable &table) { return table.ParallelCacheReady(); }
	template <typename Table> static bool Idle(const Table &table) { return table.ParallelIdle(); }
	static bool Serial(const PassengerPrecomputedTable &table) { return table.serial_destination == table.destination; }
	static bool Pending(const PassengerPrecomputedTable &table) { return table.parallel_credit != 0; }
	static void ExpirePreparationWait(PassengerPrecomputedTable &table) { table.preparation_wait_limit = std::chrono::milliseconds(0); }
};

struct PassengerServiceDatabaseTestAccess {
	static void DestinationChanged(PassengerServiceDatabase &db, StationID destination) { db.BumpHyperpathDestination(destination); }
	static void Seed(PassengerServiceDatabase &db, std::span<const PassengerPeriodicService> services)
	{
		for (const auto &service : services) {
			db.services.emplace(service.id, service);
			db.generation = std::max(db.generation, service.generation); // A saved service cannot be newer than its DB.
		}
		db.published_revision = 1; db.bootstrap = db.sweeping = false;
	}
	static void BeginReplacement(PassengerServiceDatabase &db, int64_t transfer) { db.BeginPrecomputed(transfer); }
	static PassengerPrecomputedTable &Table(PassengerServiceDatabase &db, CargoType cargo) { return db.precomputed[cargo]; }
	static const PassengerPrecomputedTable &Retained(const PassengerServiceDatabase &db, CargoType cargo) { return db.retained_precomputed[cargo]; }
	static PassengerPrecomputedTable &Retained(PassengerServiceDatabase &db, CargoType cargo) { return db.retained_precomputed[cargo]; }
};
#endif
