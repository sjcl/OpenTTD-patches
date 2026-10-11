# Passenger OD assignment

Branch: `feat/passenger-od-assignment`, based on `feat/passenger-od-model`.

Enable `linkgraph.passenger_od_assignment` in the Cargo distribution settings.
This game setting defaults to false, is synchronized through the normal settings
commands, and is independent of `passenger_transfer_timetables`. Automatic
CargoDist distribution and a valid Passengers-class cargo specification are
required. Final destinations remain metadata: boarding, delivery, transfers,
MCF and FlowMapper continue to use their existing rules.

## Demand and lifetime

`LinkGraphJob::FinaliseJob()` applies sparse distributions on the simulation
thread after joining the worker, before moving its flow annotations. It consumes
the existing `DemandAnnotation` spans; it never rebuilds the discarded matrix or
recalculates OD demand. A candidate needs positive original `demand` and
`unsatisfied_demand < demand`. The weight is the original demand, including when
only part of it was assigned to a route. Unsatisfied demand never increases the
number of generated passengers.

Stations, cargo, graph identity, graph size and every NodeID/StationID mapping
are validated before publication. Destinations are stored in StationID order.
Topology identities change on node insertion/removal, edge insertion/removal
(including timeout removal through mutable iteration), merging, and distribution
setting changes. Identity checks reject obsolete jobs and distributions even if
pool IDs are reused or a topology is changed and later restored. Splitting a
component by deleting links also invalidates its distributions. Normal capacity
updates and compression do not invalidate them.

An identical recalculation retains its rounding residuals. Changed weights or
membership start with zero residuals. Generation validates the distribution and
its destinations again; invalid, absent or oversized distributions generate
only unassigned packets until a usable job completes.

## Deterministic allocation

Each destination retains a signed credit in units of total demand weight.
For each batch the allocator adds its integer quota and fractional remainder,
then corrects rounded totals using a heap of destination indices. Ties use
StationID order. The correction requires fewer than the number of destinations
in rounded units, independent of the number of passengers. Credits sum to zero
and remain between `-W` and `N * W`, exclusively. Thus accumulated allocation
error is bounded and small destinations are not permanently rounded away.

The allocator uses no game random calls and guarantees that all assigned counts
sum exactly to the input batch. Products are `uint32_t * uint32_t` widened to
`uint64_t`. Total weight and signed-credit bounds are validated before use;
temporary quotas are also 64-bit, including for `UINT32_MAX` input batches.

The tests verify 600:300:100 demand gives 60:30:10 for 100 passengers, and 90:10
demand gives 90:10 after 100 one-passenger batches. They also exercise irregular
batches, zero batches and maximum unsigned input values.

## Generation, bounds and diagnostics

`UpdateStationWaiting()` retains its fractional generation, station rating,
supply accounting and animation/randomisation calls. Its packet creation step
calls `GenerateStationCargo()`. This draws `ge.GetVia(origin)` exactly once per
batch, before either the legacy or OD creation path. All resulting packets share
that next hop and retain the original Source and first station. Final destinations
are set exclusively through `SetPassengerDestination()`.

A whole unassigned packet is allocated first. OD portions are split from it;
pool exhaustion leaves the remaining passengers in that packet. There are at
most 32 newly allocated packets per batch. Counts beyond that budget remain
unassigned, with no reassignment to another destination. Merge rules already
require equal final destinations.

There are at most 256 destinations per station/cargo distribution. A distribution
with more eligible destinations is discarded in full: a truncated distribution
would incorrectly renormalise its demand. Storage is sparse, with no permanent
all-station matrix. Allocation and validation use O(N) scratch, with O(N log N)
allocation time; distribution construction is O(V + D + sum(N log N)), with a
bounded destination vector per origin. No heap allocation occurs per passenger.

On this x64 build, a distribution with 256 entries occupies 4,144 bytes, including
its vector storage; each entry is 16 bytes. GoodsEntry itself does not grow.
GoodsEntryData gains one pointer (8 bytes on x64), and each LinkGraph gains an
8-byte topology identity. Freight never gets an OD vector.

`_passenger_od_stats` exposes cumulative accepted distribution/entry counts,
build time, allocation time, newly allocated packets and unassigned passenger
counts. `misc=3` logs distribution statistics and `misc=4` also logs allocation
statistics. Diagnostic clocks/counters never control simulation decisions and
are not saved. The performance test reports bytes and time directly.

For 100 passengers and three destinations, the test measures three packets
instead of the legacy one. For 512 passengers and 256 equal destinations it
measures 32 packets: 62 passengers have explicit destinations and 450 remain
unassigned. When only one pool slot remains, all 100 passengers fit in one
unassigned packet.

Splitting affects later probability granularity: existing cargo staging draws a
via per packet. A regression test stages one legacy packet and three OD packets
from the same random state and confirms different subsequent random states,
while both retain exactly 100 passengers. The generation-time draw count stays
unchanged. The disabled/manual/freight/fallback tests compare legacy counts,
packet counts and random states directly.

## Save compatibility

Independent feature `XSLFI_PASSENGER_OD_ASSIGNMENT`, version 1, saves the setting,
graph identities (including job snapshots), sparse station distributions,
original weights and every signed rounding credit. Its SLXI extra data saves
the global identity counter so subsequent topology changes agree after a
multiplayer join. `XSLFI_PASSENGER_DESTINATION` is unchanged.

Station tables use bounded lists: at most one distribution per goods entry and
256 destination records per distribution. Counts are checked before resizing;
weights, ordering, duplicate destinations, total weight, credit ranges and credit
sums are checked before adopting loaded data. Invalid station/membership data
invalidates the entire distribution rather than reallocating its weight to
another destination. Validation runs before loaded CargoDist workers start.

Old saves omit these fields and load with OD assignment disabled and no
distribution. New saves require a build that understands the new extension;
older executables cannot silently ignore state required for synchronization.
Tests use the real station, graph, job and packet serializers, including an old
table without the extension. They verify identical residuals and identical next
generated destinations after restoring station state.

## Changed files

- `src/passenger_od.h`, `src/passenger_od.cpp`: sparse model, allocation,
  validation, generation and diagnostics.
- `src/station_base.h`, `src/station_cmd.cpp`: optional goods data and generation.
- `src/linkgraph/linkgraph.h`, `src/linkgraph/linkgraph.cpp`,
  `src/linkgraph/linkgraphjob.cpp`: topology identity and simulation-thread apply.
- `src/settings_type.h`, `src/settings_table.cpp`, `src/settingentry_gui.cpp`,
  `src/table/settings/linkgraph_settings.ini`, `src/lang/english.txt`: synchronized
  opt-in setting, invalidation callback and GUI text.
- `src/sl/station_sl.cpp`, `src/sl/linkgraph_sl.cpp`,
  `src/sl/extended_ver_sl.h`, `src/sl/extended_ver_sl.cpp`: independent extension
  and bounded serialization.
- `src/openttd.cpp`: reset identity and diagnostic state for a new game.
- `src/CMakeLists.txt`, `src/tests/CMakeLists.txt`,
  `src/tests/passenger_od_assignment.cpp`: build registration and regression tests.
- `docs/passenger-od-assignment.md`: implementation and verification report.

## Verification

Windows x64, MSVC 19.44, Release, assertions enabled, dedicated-server build,
Ninja, existing vcpkg dependencies; both `openttd` and `openttd_test` built.
The configure used `/Od` for compiler-probe Debug flags to avoid a local MSVC
debug-database error; the actual targets used the Release optimization flags.

- New OD assignment tests: 16 cases, 12,366 assertions, passed, including actual
  SLXI identity-counter callback round trips and unsigned-wrap rejection.
- Passenger tests together (OD lifecycle, routing, service graph and assignment):
  53 cases, 23,611 assertions, passed.
- Full unit suite: 250 cases passed, both through CTest and in one process.
- Maximum-size allocation benchmark: approximately 50 ms for 10,000 batches of
  256 destinations (about 5 microseconds per batch), with UINT32_MAX batch sizes.
- `git diff --check`: passed.

The four bundled AI/GS scenario regressions were also executed after building
`regression_files`. Their expected-output comparisons fail in this Windows
environment, including font-loading warnings. Each scenario's normalized output
was compared by SHA-256 with the existing `build-passenger-od-model/build-od`
executable: all four outputs are byte-identical between that base build and
this implementation with the option disabled. That existing executable's
revision label is `jgrpp-ya-0.73.3+2-3-gde9bfeac8-m`; it is a previously built
base-worktree binary, not a new clean build of immutable commit `a17ab057ad`.
These scenario tests are therefore recorded as failing reference comparisons,
not as passing tests.

No live multiplayer server/client session or production-sized real map was run.
Determinism was checked through integer allocation, random-state comparisons
and real serialization followed by the next generated passenger batch.
