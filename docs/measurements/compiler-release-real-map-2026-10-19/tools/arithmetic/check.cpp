#include "C:/Users/bebe/.codex/worktrees/6d2b/OpenTTD-patches/src/core/passenger_strategy.hpp"
#include <cstdio>
int main() {
    uint64_t digest = 0, cases = 0;
    for (uint64_t value : {uint64_t{0}, uint64_t{1}, uint64_t{3}, uint64_t{0xffffffff}, uint64_t{0x100000001}, uint64_t{INT64_MAX - 1}}) {
        for (uint64_t divisor : {uint64_t{1}, uint64_t{2}, uint64_t{7}, uint64_t{0xffffffff}, uint64_t{0x100000001}, uint64_t{UINT64_MAX}}) {
            const PassengerWide product = PassengerWide::Multiply(value, divisor);
            if (product.Ceil(divisor) != int64_t(value)) return 1;
            if (value && product.low != 0) {
                PassengerWide below = product; --below.low;
                if (below.Ceil(divisor) != int64_t(value - (divisor == 1))) return 2;
            }
            digest ^= product.high + product.low + product.Ceil(divisor); ++cases;
        }
    }
    for (uint64_t divisor : {uint64_t{1}, uint64_t{3}, uint64_t{UINT64_MAX}}) {
        if (PassengerWide::Multiply(INT64_MAX, divisor).Ceil(divisor) != INT64_MAX) return 3;
        if (PassengerWide{UINT64_MAX, UINT64_MAX}.Ceil(divisor) != INT64_MAX) return 4;
    }
    std::printf("cases=%llu digest=%llu\n", (unsigned long long)cases, (unsigned long long)digest);
}
