/* Isolated AVX2 experiment. No global compiler architecture flags. */
#pragma once
#include <cstdint>
#include <cstdlib>
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#define PASSENGER_RANGE_X86 1
#endif

inline uint32_t PassengerScalarMask(int64_t cost, uint32_t legs, const int64_t *old_cost, const uint32_t *old_legs)
{
	uint32_t mask = 0;
	for (unsigned i = 0; i < 4; ++i) if (cost < old_cost[i] || (cost == old_cost[i] && legs < old_legs[i])) mask |= 1U << i;
	return mask;
}

inline bool PassengerHasAVX2()
{
#if defined(PASSENGER_RANGE_X86) && defined(_MSC_VER)
	int info[4]; __cpuid(info, 0); if (info[0] < 7) return false;
	__cpuidex(info, 1, 0);
	if ((info[2] & ((1 << 27) | (1 << 28))) != ((1 << 27) | (1 << 28)) || (_xgetbv(0) & 6) != 6) return false;
	__cpuidex(info, 7, 0); return (info[1] & (1 << 5)) != 0;
#elif defined(PASSENGER_RANGE_X86) && defined(__GNUC__)
	return __builtin_cpu_supports("avx2");
#else
	return false;
#endif
}

#if defined(PASSENGER_RANGE_X86)
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((target("avx2"), noinline))
#endif
inline uint32_t PassengerAVX2Mask(int64_t cost, uint32_t legs, const int64_t *old_cost, const uint32_t *old_legs)
{
	const auto a = _mm256_set1_epi64x(cost), b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(old_cost));
	const auto less = _mm256_cmpgt_epi64(b, a), equal = _mm256_cmpeq_epi64(a, b);
	if (_mm256_movemask_pd(_mm256_castsi256_pd(equal)) == 0) return static_cast<uint32_t>(_mm256_movemask_pd(_mm256_castsi256_pd(less)));
	/* Widen unsigned legs to 64 bits before the signed comparison. */
	const auto l = _mm256_set1_epi64x(legs), old_l = _mm256_cvtepu32_epi64(_mm_loadu_si128(reinterpret_cast<const __m128i *>(old_legs)));
	const auto improve = _mm256_or_si256(less, _mm256_and_si256(equal, _mm256_cmpgt_epi64(old_l, l)));
	return static_cast<uint32_t>(_mm256_movemask_pd(_mm256_castsi256_pd(improve)));
}
#endif

inline uint32_t PassengerRangeMask(int64_t cost, uint32_t legs, const int64_t *old_cost, const uint32_t *old_legs)
{
#if defined(PASSENGER_RANGE_X86)
	static const bool use_avx2 = PassengerHasAVX2() && getenv("OPENTTD_TABLE_SCALAR") == nullptr;
	if (use_avx2) return PassengerAVX2Mask(cost, legs, old_cost, old_legs);
#endif
	return PassengerScalarMask(cost, legs, old_cost, old_legs);
}
