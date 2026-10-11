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

An identical recalculation retains its rounding residuals, weighted unknown state
and packet cursor. Changed weights or membership start with zero residuals and
cursor. Generation validates the distribution and its destinations again; invalid or absent distributions generate
only unassigned packets until a usable job completes.

## Deterministic allocation

Each destination retains a signed credit in units of total demand weight.
For each batch the allocator adds its integer quota and fractional remainder,
then corrects rounded totals using partial selection of destination indices.
Excess quotas remove whole rounds using a bounded histogram, followed by one
selection; deficits use one selection. Ties use StationID order. The correction
visits only the sparse entries, independent of the number of passengers. Credits sum to zero
and remain between `-W` and `N * W`, exclusively. Thus accumulated allocation
error is bounded when all proposed quotas can be emitted, and small destinations
are not permanently rounded away.

The allocator uses no game random calls and guarantees that all assigned counts
sum exactly to the input batch. Products are `uint32_t * uint32_t` widened to
`uint64_t`. An aggregate unknown bucket can exceed a 32-bit weight; its quota and
remainder are derived from the complement of retained products to avoid a wider
multiplication. Total weight and signed-credit bounds are validated before use;
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
most 4,096 newly allocated packets per batch. Counts beyond that budget remain
unassigned, with no reassignment to another destination. Merge rules already
require equal final destinations.

A saved, validated cursor rotates packet emission through StationID order. Each
successful destination advances it; exhaustion leaves the next turn pending.
Thus a 512-passenger batch with 256 equal demands emits 256 packets, with two
assigned passengers per destination and none unassigned. Packet-limited batches
visit other destinations on subsequent turns. Identical job recalculation
preserves the cursor.

Quota calculation is tentative. If packet capacity leaves passengers unassigned,
the allocator restores the previous credits and advances its fractional rounding
phase for only the number given explicit destinations plus the natural weighted
unknown quota. Whole quotas suppressed by packet capacity
are not counted as served demand or accumulated as debt. With no spare pool slot,
the explicit rounding state and cursor stay unchanged when no weighted unknown
bucket is present. Packet-limited allocation does not
claim the uncapped allocator's bound on error per destination; the cursor ensures
rotation while the generated quotas retain the original demand weights.

When tentative quotas contain zeroes and exceed available packet capacity, the
allocator instead proposes at most `available_packets - 1` passengers and leaves
the rest unassigned. That plan fits in its entirety: every nonzero rounding award
is emitted. This prevents small demands from losing awards between a tentative
large batch and its reduced rounding phase. It may assign fewer passengers in a
mixed-demand batch, while preserving the original weighted unknown proportion
within the committed plan. The test with 32 large demands and 224 tiny demands
confirms that every destination receives passengers across 10,000 batches.

There is no 256-destination policy limit. Every eligible demand is retained when
the sparse job fits the remaining global record budget. An 8,000-destination hub
with no other demand origins therefore keeps all 8,000 records. The only absolute
per-origin list limit is the physical NodeID range (65,534 destinations).

All graphs share a fixed logical budget of 134,217,728 destination records,
approximately 2 GiB of vector payload on x64. The synchronized limit does not
depend on `sizeof`, vector capacity, standard library or allocation history.
For example, 10,000 origins with 8,000 eligible
destinations each use approximately 1.28 GB of destination records and fit this
budget. Construction counts actual eligible records rather than
reserving an all-station matrix. If a job would exceed the remaining budget, it
fulfils small origins first and shares every remaining record among the other
active origins. Candidate-count ties use NodeID order, including when distributing
the remainder of an equal share. Thus origins with 1 and 200 candidates retain
1 and 99 records with a budget of 100. Each origin retains its largest
original demands, with StationID as a deterministic tie break. Omitted demand
is saved in a weighted unknown bucket. The denominator is still the original
total; retained destinations are never renormalised to absorb omitted demand.
Other graphs' logical vector sizes count against the budget. Capacity is only
a diagnostic of physical memory use. Distribution objects
and construction scratch are additional; replacing a graph can temporarily retain
both old and new vectors. The policy can yield different retention budgets
between graphs according to their existing allocations.

Generation and validation visit only retained destinations, with O(N) scratch;
quota scans and corrections are linear, using expected-linear `nth_element`
selection. Construction reads demand spans twice and uses bounded heaps and
sorting (O(D log N)) when choosing retained entries. No allocation occurs per
passenger and no persistent all-station matrix is introduced.

On this x64 build, a distribution with 256 entries occupies 4,168 bytes, including
its vector storage; each entry is 16 bytes. GoodsEntry itself does not grow.
GoodsEntryData gains one pointer (8 bytes on x64), and each LinkGraph gains an
8-byte topology identity. Freight never gets an OD vector.

`_passenger_od_stats` exposes cumulative accepted distribution/entry counts,
build time, allocation time, newly allocated packets and unassigned passenger
counts, batch/passenger totals and peak batch, retained-station and live-pool
packet counts. `misc=3` logs distribution statistics and `misc=4` also logs allocation
statistics. Diagnostic clocks/counters never control simulation decisions and
are not saved. The performance test reports bytes and time directly.

For 100 passengers and three destinations, the test measures three packets
instead of the legacy one. For 512 passengers and 256 equal destinations it
measures 256 packets: all 512 passengers have explicit destinations. When only
one pool slot remains, all 100 passengers fit in one
unassigned packet.

Splitting affects later probability granularity: existing cargo staging draws a
via per packet. A regression test stages one legacy packet and three OD packets
from the same random state and confirms different subsequent random states,
while both retain exactly 100 passengers. The generation-time draw count stays
unchanged. The disabled/manual/freight/fallback tests compare legacy counts,
packet counts and random states directly.

## Save compatibility

Independent feature `XSLFI_PASSENGER_OD_ASSIGNMENT`, version 3, saves the setting,
graph identities (including job snapshots), sparse station distributions,
original weights, every signed rounding credit, the packet cursor and the
weighted unknown bucket. Versions 1 and 2 remain readable; missing cursor or
unknown state starts at zero. Its SLXI extra data saves
the global identity counter so subsequent topology changes agree after a
multiplayer join. `XSLFI_PASSENGER_DESTINATION` is unchanged.

Station tables allow at most one distribution per goods entry. Physical node
limits and the same cumulative logical record budget are checked before resizing;
weights, ordering, duplicate destinations, total weight, credit ranges and credit
sums are checked before adopting loaded data. Invalid station/membership data
invalidates the entire distribution rather than reallocating its weight to
another destination. `AfterLoadGame()` calls `AfterLoadPassengerOd()` after
station/graph/cargo conversion, immediately before `AfterLoadLinkGraphs()` starts
loaded CargoDist workers. Previously the call was inside `AfterLoadLinkGraphs()`;
it was already reachable, and is now explicit at the game-load boundary.

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
  `src/sl/afterload.cpp`, `src/sl/extended_ver_sl.h`, `src/sl/extended_ver_sl.cpp`: independent extension
  and bounded serialization.
- `src/openttd.cpp`: reset identity and diagnostic state for a new game.
- `src/CMakeLists.txt`, `src/tests/CMakeLists.txt`,
  `src/tests/passenger_od_assignment.cpp`: build registration and regression tests.
- `docs/passenger-od-assignment.md`: implementation and verification report.
- `cmake/scripts/PassengerOdLoad.cmake`, `regression/CMakeLists.txt`: production
  load integration using the null video driver, a legacy save, and new enabled
  and disabled saves. No server or GUI is started.

## Verification

Windows x64, MSVC 19.44, Release, assertions enabled, dedicated-server build,
Ninja, existing vcpkg dependencies; both `openttd` and `openttd_test` built.
The configure used `/Od` for compiler-probe Debug flags to avoid a local MSVC
debug-database error; the actual targets used the Release optimization flags.

- OD assignment tests: 31 cases passed, including actual SLXI identity-counter
  callback round trips, unsigned-wrap rejection, packet rotation for 256 and
  1,000 destinations, mixed large/tiny demands, an 8,000-destination hub, earlier extension versions, and
  partial retention using a smaller budget to avoid gigabyte allocations in CI,
  unequal candidate counts, deterministic budget remainders and identical retained
  distributions/rounding state despite different `reserve()` histories in another
  graph.
- Passenger tests together (OD lifecycle, routing, service graph and assignment):
  68 cases, 47,849 assertions, passed in one process.
- CTest: 266 tests passed: 265 unit cases plus the production load integration.
  The integration loads a legacy save, then saves and loads current-format games
  with OD enabled and disabled, checking the validation hook and saved setting.
- Maximum-batch allocation benchmark: approximately 20 ms for 10,000 batches of
  256 destinations (about 2 microseconds per batch), with UINT32_MAX batch sizes.
- Full generation for a sparse 8,000-destination hub: approximately 91 ms for
  400 batches of 512 passengers (about 228 microseconds per batch), including
  destination validation, allocation and packet creation. Every destination is
  served, batch totals are conserved, and the 4,096-packet limit is maintained.
  Packet inspection and queue clearing are outside the timed interval. This
  synthetic measurement ran while the real-save benchmark was warming up; it
  has no timing threshold in CI and is not a whole-game speed measurement.
- `git diff --check`: passed.

During earlier validation of commit `6e9bd46fb8`, the four bundled AI/GS scenario
regressions were executed after building
`regression_files`. Their expected-output comparisons fail in this Windows
environment, including font-loading warnings. Each scenario's normalized output
was compared by SHA-256 with the existing `build-passenger-od-model/build-od`
executable: all four outputs are byte-identical between that base build and
this implementation with the option disabled. That existing executable's
revision label is `jgrpp-ya-0.73.3+2-3-gde9bfeac8-m`; it is a previously built
base-worktree binary, not a new clean build of immutable commit `a17ab057ad`.
These scenario tests are recorded as failing reference comparisons and were
excluded from the current 266-test run. They have not been rerun for this revision.

No live multiplayer server/client session was run.
Determinism was checked through integer allocation, random-state comparisons
and real serialization followed by the next generated passenger batch.

## Real-save performance measurement (2026-10-11)

The user-provided regional railway save contains 4,797 stations and a loaded
4,660-node LinkGraph job. Its original file was read only; its SHA-256 remained
`bdef0140b653f00853e4fbfcc1e4ea52727ba7863d2dbd75dc7f4c48110a4eda`.
Existing NewGRF files were used from a disposable workspace copy. Neither the
save nor the assets are included in the repository.

The measurement used the same Release build and original save for each setting,
with null video, sound and music drivers, configuration saving and autosaving
disabled. CargoDist settings remained those of the save (passenger distribution
asymmetric, recalculation interval 8 and recalculation time 32). After enabling
or disabling OD through the normal setting command, each run advanced 30,000
actual scaled simulation ticks. This lets loaded jobs finish and new jobs publish
OD state. The next 2,000 ticks were timed around GameLoop, InputLoop and
UpdateWindows, excluding save loading, warmup and the final diagnostic scan.
Both windows start at scaled tick 3,009,627,300 and end at 3,009,629,300.
Warmup preparation overlapped; the other benchmark process, including its worker
threads, was suspended before the disabled timing window and resumed afterwards.
The final tests also finished before the enabled timing window started.

| Measurement | OD disabled | OD enabled |
| --- | ---: | ---: |
| Time for 2,000 ticks | 36.243305 s | 36.414771 s |
| Time per tick | 18.121653 ms | 18.207386 ms |
| Stations at end | 4,797 | 4,797 |
| Vehicle pool objects at end | 20,871 | 20,843 |
| Retained distributions | 0 | 3,521 |
| Retained destination records | 0 | 112,380 |
| Destination-vector capacity bytes | 0 | 1,798,080 |
| Largest retained destination list | 0 | 712 |

The enabled window also recorded 10,881 microseconds building distributions,
360,455 microseconds allocating/generating cargo, 126,961 newly allocated packets
and one unassigned passenger. These counters cover only the timed window; the
retained-state scan describes its end. Allocation timing excludes the preceding
destination validation, whereas whole-loop timing includes it. Physical capacity
is diagnostic only and does not determine retention. The vehicle counts differ
because packet splitting changes later per-packet random draw granularity, as
documented above; the two simulations do not have identical future random states.

This single paired window was about 0.47% slower with OD enabled. It is an
exploratory result for this save, not a statistically established overhead or an
8,000-destination whole-network measurement. Generation still validates and scans
retained destinations per batch, and publication still scans station goods for
the logical budget. No validation cache or scratch-buffer reuse was introduced
based on this small observed difference; the separate synthetic generation test
covers the 8,000-destination path.

The supplied save emitted existing JP+ Multiple Units vehicle-capacity warnings
and entered error pause on load with the locally available NewGRFs. Both
disposable loaded states acknowledged that pause before warmup; subsequent
stderr warning output was sent to NUL in both runs. This is a condition of the
measurement and does not establish warning-free compatibility of the real save.
The temporary measurement harness was removed before the final normal build and
production save/load integration check. Raw measurement logs remain local in
`build-od/od-performance/off-final-1.log` and `on-final-1.log`.
