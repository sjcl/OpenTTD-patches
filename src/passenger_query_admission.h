/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_query_admission.h Deterministic, scoped logical Work reservation for any periodic planner. */
#ifndef PASSENGER_QUERY_ADMISSION_H
#define PASSENGER_QUERY_ADMISSION_H

class PassengerQueryAdmission {
public:
	static constexpr size_t MAX_WORK = 65536;
	static constexpr size_t SCHEDULE_PROTECTION = 16384;
	static constexpr size_t QUERIES_PER_TICK = 16; ///< 24/32/64 raised real-map CPU disproportionately; retune with a faster planner.
	enum class Rejection : uint8_t { None, EventQueries, TickQueries, EventWork, GlobalWork, FairWork, FairQueries, End };
private:
	size_t &event_work, &global_work;
	size_t reserved = 0, used = 0;
	Rejection rejection = Rejection::None;
public:
	PassengerQueryAdmission(size_t &event_work, size_t &global_work, size_t &event_queries, size_t &global_queries,
			size_t protected_work = 0, size_t protected_queries = 0) : event_work(event_work), global_work(global_work)
	{
		if (event_queries == 0) this->rejection = Rejection::EventQueries;
		else if (global_queries == 0) this->rejection = Rejection::TickQueries;
		else if (event_work == 0) this->rejection = Rejection::EventWork;
		else if (global_work == 0) this->rejection = Rejection::GlobalWork;
		else if (global_work <= protected_work) this->rejection = Rejection::FairWork;
		else if (global_queries <= protected_queries) this->rejection = Rejection::FairQueries;
		else {
			this->reserved = std::min({MAX_WORK, event_work, global_work - protected_work});
			event_work -= this->reserved; global_work -= this->reserved;
			--event_queries; --global_queries;
		}
	}
	PassengerQueryAdmission(const PassengerQueryAdmission &) = delete;
	PassengerQueryAdmission &operator=(const PassengerQueryAdmission &) = delete;
	~PassengerQueryAdmission()
	{
		this->event_work += this->Returned(); this->global_work += this->Returned();
	}
	explicit operator bool() const { return this->reserved != 0; }
	Rejection GetRejection() const { return this->rejection; }
	size_t Reserved() const { return this->reserved; }
	size_t Used() const { return this->used; }
	size_t Returned() const { return this->reserved - this->used; }
	/** A smaller tail allowance keeps the same ratio. Fallback may borrow all unused Timed Work. */
	size_t TimedAllowance(size_t protection = SCHEDULE_PROTECTION) const
	{
		assert(protection <= MAX_WORK);
		return this->reserved - uint64_t(this->reserved) * protection / MAX_WORK;
	}
	size_t ScheduleAllowance() const { return this->Returned(); }
	void Charge(size_t work) { assert(work <= this->Returned()); this->used += work; }
};

#endif /* PASSENGER_QUERY_ADMISSION_H */
