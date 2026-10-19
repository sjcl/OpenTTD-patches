/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file network_join_progress.h Server-owned join deadlines, independent of frame ACKs and client claims. */
#ifndef NETWORK_JOIN_PROGRESS_H
#define NETWORK_JOIN_PROGRESS_H

#include <chrono>
#include <optional>

class NetworkJoinProgress {
public:
	using Clock = std::chrono::steady_clock;
	enum class Result { Accepted, Invalid, Flood };
private:
	std::optional<Clock::time_point> started;
	Clock::time_point progress{}, refill{};
	uint64_t work = 0, limit = 0;
	static constexpr uint BURST = 16;
	uint tokens = BURST;
public:
	void Start(Clock::time_point now)
	{
		/* Map queue transitions must never restart the absolute deadline. */
		if (!this->started) this->started = now;
	}
	void BeginReplay(uint64_t limit, Clock::time_point now)
	{
		this->work = 0; this->limit = limit;
		this->progress = this->refill = now; this->tokens = BURST;
	}
	Clock::duration GetJoinTime(Clock::time_point now) const
	{ return this->started ? now - *this->started : Clock::duration::zero(); }
	Result Report(uint64_t value, Clock::time_point now)
	{
		if (value <= this->work || value > this->limit) return Result::Invalid;
		/* Allow honest TCP coalescing; bound sustained reports and reply traffic. */
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - this->refill).count() / 250;
		if (elapsed > 0) {
			this->tokens += static_cast<uint>(std::min<int64_t>(BURST - this->tokens, elapsed));
			this->refill = now;
		}
		if (this->tokens == 0) return Result::Flood;
		--this->tokens;
		this->work = value; this->progress = now;
		return Result::Accepted;
	}
	bool Expired(Clock::time_point now, std::chrono::seconds maximum) const
	{
		return this->started && this->GetJoinTime(now) >= maximum;
	}
	bool Idle(Clock::time_point now, std::chrono::milliseconds maximum) const
	{ return now - this->progress > maximum; }
};

#endif /* NETWORK_JOIN_PROGRESS_H */
