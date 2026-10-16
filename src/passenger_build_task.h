/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

/** @file passenger_build_task.h Cooperative preparation on the simulation thread, with no CPU-dependent publication. */
#ifndef PASSENGER_BUILD_TASK_H
#define PASSENGER_BUILD_TASK_H

#include <coroutine>
#include <exception>
#include <utility>

class PassengerBuildTask {
public:
	struct promise_type {
		std::exception_ptr error;
		PassengerBuildTask get_return_object() { return PassengerBuildTask{std::coroutine_handle<promise_type>::from_promise(*this)}; }
		std::suspend_always initial_suspend() noexcept { return {}; }
		std::suspend_always final_suspend() noexcept { return {}; }
		void return_void() {}
		void unhandled_exception() { this->error = std::current_exception(); }
	};
private:
	std::coroutine_handle<promise_type> task{};
	explicit PassengerBuildTask(std::coroutine_handle<promise_type> task) : task(task) {}
public:
	PassengerBuildTask() = default;
	PassengerBuildTask(const PassengerBuildTask &) = delete;
	PassengerBuildTask &operator=(const PassengerBuildTask &) = delete;
	PassengerBuildTask(PassengerBuildTask &&other) noexcept : task(std::exchange(other.task, {})) {}
	PassengerBuildTask &operator=(PassengerBuildTask &&other) noexcept
	{
		if (this->task) this->task.destroy();
		this->task = std::exchange(other.task, {}); return *this;
	}
	~PassengerBuildTask() { if (this->task) this->task.destroy(); }
	explicit operator bool() const { return this->task && !this->task.done(); }
	bool Resume(size_t quantum)
	{
		while (*this && quantum-- > 0) this->task.resume();
		if (this->task && this->task.promise().error) std::rethrow_exception(this->task.promise().error);
		return !static_cast<bool>(*this);
	}
};

#endif /* PASSENGER_BUILD_TASK_H */
