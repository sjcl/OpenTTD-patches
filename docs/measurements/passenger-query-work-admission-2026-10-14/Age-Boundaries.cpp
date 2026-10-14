/* Standalone sanitizer check of the actual production age helper. */
#include "../../../src/stdafx.h"
#include "../../../src/passenger_journey_type.h"
#include <array>

int main(int argc, char **)
{
	if (argc > 1) {
		/* Negative calibration: the sanitizer must trap this old subtraction. */
		volatile int64_t high = INT64_MAX, low = INT64_MIN + 1;
		volatile int64_t invalid_difference = high - low;
		(void)invalid_difference;
		return 99;
	}
	auto check = [](bool value) { if (!value) std::abort(); };
	struct Case { int64_t planned, now, minimum; bool valid, aged; uint64_t age; };
	const std::array cases{
		Case{0, 4095, 4096, true, false, 4095}, Case{0, 4096, 4096, true, true, 4096}, Case{0, 4097, 4096, true, true, 4097},
		Case{INT64_MIN + 1, INT64_MAX, 4096, true, true, UINT64_MAX - 1},
		Case{INT64_MIN + 1, INT64_MAX, INT64_MAX, true, true, UINT64_MAX - 1},
		Case{INT64_MAX - 4096, INT64_MAX, 4096, true, true, 4096}, Case{INT64_MAX - 4095, INT64_MAX, 4096, true, false, 4095},
		Case{10, 9, 0, false, false, 0}, Case{-8192, -4096, 4096, true, true, 4096},
		Case{-4096, 0, 4096, true, true, 4096}, Case{-4096, 1, 4096, true, true, 4097},
		Case{0, 0, 0, true, true, 0}, Case{-10, -10, 0, true, true, 0},
		Case{0, 4096, -1, true, false, 4096}, Case{0, 4096, INT64_MIN, true, false, 4096},
		Case{INT64_MIN, 0, 0, false, false, 0}, Case{0, INT64_MIN, 0, false, false, 0},
		Case{INT64_MIN, INT64_MIN, 0, false, false, 0}, Case{INT64_MAX, INT64_MAX, 0, true, true, 0},
	};
	unsigned checks = 0;
	for (const auto kind : {PassengerRideKind::VehicleSnapshot, PassengerRideKind::ServiceIntent, PassengerRideKind::BoundService,
			PassengerRideKind::EstimatedServiceIntent, PassengerRideKind::EstimatedBoundService}) {
		for (const auto &test : cases) {
			PassengerJourneyExecution ride; ride.kind = kind; ride.planned_at = StateTicks{test.planned};
			const auto original = ride;
			const auto age = GetPassengerScheduleIntentAge(ride, StateTicks{test.now});
			check(age.has_value() == (IsEstimatedPassengerRide(kind) && test.valid));
			if (age) check(*age == test.age);
			check(IsPassengerScheduleIntentAged(ride, StateTicks{test.now}, test.minimum) == (IsEstimatedPassengerRide(kind) && test.aged));
			check(ride == original); ++checks;
		}
	}
	std::printf("Production age helper: %u cases passed\n", checks);
}
