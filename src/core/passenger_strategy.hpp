/* This file is part of OpenTTD, licensed under the GNU General Public License, version 2. */
#ifndef PASSENGER_STRATEGY_HPP
#define PASSENGER_STRATEGY_HPP

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

/** Unsigned double word. No compiler-specific integer type enters saved state.
 * Products use all 128 bits on every target, including 32-bit and MSVC builds. */
struct PassengerWide {
	uint64_t high = 0, low = 0;
	static PassengerWide Multiply(uint64_t a, uint64_t b)
	{
#if defined(_MSC_VER) && defined(_M_X64)
		PassengerWide r; r.low = _umul128(a, b, &r.high); return r;
#elif defined(__SIZEOF_INT128__)
		__uint128_t r = __uint128_t(a) * b;
		return {uint64_t(r >> 64), uint64_t(r)};
#else
		uint64_t a0 = uint32_t(a), a1 = a >> 32, b0 = uint32_t(b), b1 = b >> 32;
		uint64_t lo = a0 * b0, middle = a1 * b0 + (lo >> 32);
		uint64_t carry = middle >> 32;
		middle = uint32_t(middle) + a0 * b1;
		return {a1 * b1 + carry + (middle >> 32), (middle << 32) | uint32_t(lo)};
#endif
	}
	bool operator<(const PassengerWide &b) const { return this->high < b.high || (this->high == b.high && this->low < b.low); }
	bool operator==(const PassengerWide &) const = default;
	void Add(PassengerWide b)
	{
		uint64_t old = this->low; this->low += b.low;
		this->high += b.high + (this->low < old);
	}
	/** Saturating ceil division; the caller never schedules an infinite label. */
	int64_t Ceil(uint64_t divisor) const
	{
		assert(divisor != 0);
		if (!(*this < Multiply(INT64_MAX, divisor))) return INT64_MAX;
		uint64_t quotient, remainder;
#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
		quotient = _udiv128(this->high, this->low, divisor, &remainder);
#elif defined(__SIZEOF_INT128__)
		__uint128_t n = (__uint128_t(this->high) << 64) | this->low;
		quotient = uint64_t(n / divisor); remainder = uint64_t(n % divisor);
#else
		quotient = 0; remainder = this->high;
		for (int bit = 63; bit >= 0; --bit) {
			bool carry = (remainder >> 63) != 0;
			remainder = (remainder << 1) | ((this->low >> bit) & 1);
			if (carry || remainder >= divisor) { remainder -= divisor; quotient |= uint64_t{1} << bit; }
		}
#endif
		return int64_t(quotient + (remainder != 0));
	}
};

/** Spiess--Florian accumulator. Alpha is the single global Q48 unit.
 * f=ceil(alpha/w) is positive, with relative error < w/2^48. Supported waits
 * are <=2^24: ceil(alpha/f)==w exactly, preserving every single-line cost.
 * Graph admission proves sum(f)<2^63 at each station. Thus N is bounded by
 * alpha + (2^63-1)*(INT64_MAX-1) < 2^126+2^48; x*F fits in 126 bits.
 * No floating point or capacity weights participate in a routing decision. */
struct PassengerStrategy {
	static constexpr uint64_t ALPHA = uint64_t{1} << 48;
	static constexpr int64_t MAX_WAIT = int64_t{1} << 24;
	PassengerWide numerator{0, ALPHA};
	uint64_t frequency = 0;
	static uint64_t Frequency(int64_t wait)
	{
		assert(wait > 0 && wait <= MAX_WAIT);
		return ALPHA / wait + (ALPHA % wait != 0);
	}
	bool Attractive(int64_t cost) const
	{
		return cost >= 0 && cost < INT64_MAX && (this->frequency == 0 || PassengerWide::Multiply(uint64_t(cost), this->frequency) < this->numerator);
	}
	bool Add(int64_t cost, uint64_t frequency)
	{
		assert(frequency > 0 && frequency < (uint64_t{1} << 63) - this->frequency);
		if (!this->Attractive(cost)) return false;
		this->frequency += frequency;
		this->numerator.Add(PassengerWide::Multiply(uint64_t(cost), frequency));
		return true;
	}
	int64_t Cost() const { return this->frequency == 0 ? INT64_MAX : this->numerator.Ceil(this->frequency); }
	int64_t Wait() const { return this->frequency == 0 ? INT64_MAX : PassengerWide{0, ALPHA}.Ceil(this->frequency); }
};

#endif
