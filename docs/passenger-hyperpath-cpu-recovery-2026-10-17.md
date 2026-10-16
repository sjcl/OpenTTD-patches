# Hyperpath boarding CPU recovery

Work in progress on `feat/passenger-hyperpath-routing`, starting at
`21b7f06ad9cbe7e214ac3762d9e73f56e6f3fc46`. No routing optimization or capacity
policy change has been applied yet. Task 2 is gated on measured Task 1 results.

## Measurement instrumentation

`passenger_hyperpath_prepare.py --boarding-profile` adds hooks only to copied
measurement translation units. The product, saved fields, routing Work and
event schedule are untouched. The ordinary physical transport trace remains
disabled for CPU attribution.

`boarding-profile.csv` contains cumulative counters after every actual
simulation tick:

- `VisitBoardingCandidates` invocations and enumerated Hyperpath cohorts;
- Hyperpath enumeration-block calls and elapsed nanoseconds;
- `MayUseHyperpathPolicy` calls and elapsed nanoseconds;
- `EvaluateRide` calls, elapsed nanoseconds and visited stops, both globally
  and while inside the enumeration block.

Enumeration time includes its nested predicate and ride time. These values
must not be added. Timers themselves add observation overhead. A separate
uninstrumented comparison is necessary for the final CPU acceptance result.
The existing reset command clears counters after paused initial preparation.
Snapshot collection and CSV output run outside measured simulation calls.

After normal CMake generation:

```text
python -B utils/passenger_hyperpath_prepare.py SOURCE BUILD REVISION --boarding-profile
cmake --build BUILD --config Release --target openttd -j 6
```

Use the existing exact-StateTicks runner with tracing disabled, a completed
initial table, 2,048 ticks and replacement at tick 1,024. After both runs:

```text
python -B utils/passenger_hyperpath_boarding_report.py attribution.json LEGACY HYPERPATH
```

The report requires identical input and executable hashes, complete per-tick
snapshots, successful exit, no competing measurement and no physical trace.
It reports the enumeration increment divided by the simulation increment for
the complete interval, ordinary ticks, ordinary tail and replacement ticks.

## Input availability and baseline validation

The earlier construction fixture, SHA256
`306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`,
and its frozen JPSD/station input and real-map assets have been recovered from
another existing measurement checkout and copied into this task's ignored
build directory.

The former transport-comparison input, SHA256
`6783a500f47435a342a36e9a6544e33bb38f041abe91e9e2ff2c654edd16c978`,
is currently unavailable at the recorded paths. Several former measurement
checkouts, including `33d4` and `c430`, disappeared during setup. Their ignored
binaries, saves and measurement directories are unavailable there. A comparison
using the earlier fixture must be identified as a different starting state;
it cannot reproduce the previously reported transport interval.

MSVC x64 Release, assertions enabled, LTO disabled, static runtime. Baseline
validation passes all 721 cases: 714 unit cases and four script regressions
on the first run, then the three production-launch cases after placing the
language/basic graphics assets beside the Windows executable. The first
launch attempts timed out in the incomplete asset layout; no product edit
was needed. The instrumented executable also passes a 64-tick exact-driver
smoke with replacement at tick 32. The first real-map attribution pair has
completed as described below. No
CPU-recovery result is claimed yet.

## First attribution result and gate

Both cases use the recovered `306a8d…` input and the same measurement executable,
SHA256 `74de3c15aeac9d50d31066c5d8fcc90703198624e8959017ea62bf208ba5da6a`.
Each advances exactly 2,048 ticks from 3,181,119,581, with tracing disabled,
successful initial warm-up, cost replacement at elapsed tick 1,024, exit zero
and no detected competing game/build. These are timer-instrumented attribution
runs, not the final uninstrumented CPU acceptance comparison.

Simulation milliseconds per tick (mean / p95 / p99):

| Window | Previous | Hyperpath |
| --- | ---: | ---: |
| Ordinary 1–1,024 | 25.780 / 30.407 / 35.708 | 33.968 / 48.726 / 64.745 |
| Ordinary 513–1,024 | 26.115 / 31.034 / 35.506 | 34.594 / 46.393 / 62.246 |
| Replacement 1,025–2,048 | 32.068 / 44.557 / 56.020 | 38.183 / 50.888 / 61.384 |

In the 512-tick ordinary tail, the simulation increment is 4.341156 seconds
(8.478820 ms/tick). The Hyperpath enumeration block takes 2.085229 seconds;
subtracting the previous-mode empty-block overhead gives 2.082594 seconds,
**47.97% of the observed increment**. It enumerates 6,944,941 cohorts in
37,781 visits. Predicate time is 1.099845 seconds. Its nested `EvaluateRide`
work is only 53,759 calls, 839,796 visited stops and 0.044421 seconds, about
1.02% of the simulation increment. Journey mean changes from 1.565916 to
1.603215 ms/tick.

Across all 2,048 ticks, there are 22,842,650 enumerated cohorts and 208,740
enumeration ride evaluations (3,506,823 visited stops). Enumeration takes
6.949202 seconds, including 3.744191 predicate seconds and 0.172503 ride
seconds. Nested timings must not be added. The full-interval enumeration
increment explains 47.37% of the simulation increment.

This confirms a material enumeration cost but **does not establish the
proposed downstream label scan as the main cause**. Roughly half the ordinary
tail increment is outside this block, and observer overhead is not zero.
At that gate Task 2 remained unapplied pending broader `VisitBoardingCandidates`, waiting
index/update and concrete-resolution measurements. The additional counters
also distinguish NoLoad predicates from actual `ResolveHyperpath` calls.

Evidence: [attribution](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task1-attribution.json),
[CPU/diagnostic summary](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task1-summary.json)
and the per-tick [previous](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task1-legacy-boarding.csv)
/ [Hyperpath](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task1-hyperpath-boarding.csv) counters.

## Preliminary service 528 readback

The read-only `passenger_hyperpath_pattern_readback.py` checks and decodes an
extracted JPSD v9-v14 blob, including all snapshot records and the v14 trailer.
It does not load a game or advance Work. The recovered v9 fixture contains
864 services and 17,445 stops in each of its published/current/retained
snapshots, matching the original fixture census.

In its published service 528 there is one vehicle and 111 stops, with an
estimated circuit of 82,362 ticks (`period=0`). Order 33 is station 3393,
arrival/departure 26,196/26,418; order 34 is station 3394,
arrival/departure 26,788/27,010. Both permit boarding and alighting for
passengers. Their incoming travel quality is 1: a positive recorded travel
duration without timetable certification. These offsets correspond to
370-tick travel legs and 222-tick dwell.

`CompileEstimated` skips implicit/label orders, retains the real order index,
uses a positive recorded travel duration when available and adds the recorded
dwell. Thus this frozen pattern's order 33 is an explicit station occurrence,
not a synthetic extra frequency. This only establishes how the recovered
snapshot represents that order. Actual order executions, departure-time
passenger states and the cause of the OD regression still require a live
targeted diagnostic; none is claimed from this readback.

## Expanded attribution and permission to proceed

The expanded observer adds the whole visitor, waiting-index maintenance,
merge/update, concrete-resolution and NoLoad counters. It uses the same input,
2,048-tick interval, midpoint replacement and executable for both modes
(`6a298c24f89e8cbaf4a00c819c7722b8495a6a2200af970cd0c4a6a657c991ae`).
Both modes end in exactly the same canonical state as their first observer
runs. The comparison includes every canonical JSON field, not just unit counts.

For ordinary ticks 513–1,024, simulation increases by 3.680064 seconds.
Enumeration's net increment is 2.203634 seconds, **59.88%** of this increase.
The whole visitor increases by 2.173403 seconds, so enumeration accounts for
essentially all of the visitor regression. The inspected index/update, merge,
schedule and cache-maintenance scopes show no comparable positive increment;
the largest positive one is 17.354 ms for boarding-entry removal. Journey's
tail difference is -95.876 ms. These overlapping scopes cannot be added, and
the remaining simulation increment has not been fully attributed.

Hyperpath's tail enumerates 6,944,941 cohorts. Predicate time is 1.275600 s,
including 700.462 ms of concrete-resolution time. Over the full interval,
23,374,464 ResolveHyperpath calls take 2.557902 s, whereas all 215,829
EvaluateRide calls take 174.049 ms. The NoLoad early-exit counter is zero.
Repeated resolution/cache lookup is material; a full stop scan on every cohort
is not supported by the data. Calls alone do not establish a cache hit ratio.

The full-interval enumeration increment explains 36.75% of simulation growth;
the ordinary first-half share is 43.65%. The ordinary-tail evidence identifies
enumeration as the largest observed regression and clears Task 2's diagnostic
gate. The product optimization is then checked separately with uninstrumented
CPU runs and exact state comparisons.

Evidence: [expanded attribution](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task1b-attribution.json),
[CPU summary](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task1b-summary.json),
[previous state control](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task1b-legacy-state-control.json)
and [Hyperpath state control](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task1b-hyperpath-state-control.json).

## Task 2: separated membership and selected-cohort execution

The loading context now resolves the current/retained boarding occurrences
once per vehicle/cargo/order/station context. Cohorts check sorted candidates
without EvaluateRide. A cached failed-service local policy supplies membership;
a cold local cache is an unknown positive superset and is resolved only when
its cohort reaches the FIFO front. The original complete group predicate then
runs before any visitor can queue an event or mutate cargo. Base/adjusted union
and the existing FIFO/round-robin merge are retained. RecordCapacityMiss
explicitly requests the original full predicate during enumeration.

All **723 tests pass**. New regressions cover an attractive but unsafe live
binding and a cold failed-policy superset that is rejected without changing
intent/failure/event/reservation/FIFO/database state. A separate ordinary
4,096-tick before/after run matches **all 24 canonical fields**, including the
packet, payment, ownership, loading and DB digests, deliveries and table Work.
The 2,048-tick midpoint replacement also matches at tick 1,024 and at the end.
These comparisons use the recovered `306a8d…` fixture from StateTicks
3,181,119,581, not the missing original transport input.

The first uninstrumented CPU triplet, milliseconds/tick (mean / p95 / p99):

| Window | Previous | Before Hyperpath | Separated Hyperpath |
| --- | ---: | ---: | ---: |
| Ordinary 1–1,024 | 25.480 / 29.304 / 32.727 | 32.952 / 46.257 / 62.449 | 30.164 / 45.437 / 70.093 |
| Ordinary 513–1,024 | 24.295 / 27.357 / 29.019 | 32.579 / 42.072 / 53.606 | 31.944 / 45.437 / 82.329 |
| Replacement 1,025–2,048 | 27.650 / 41.000 / 46.100 | 39.059 / 49.733 / 57.739 | 33.109 / 42.384 / 48.295 |

Ordinary mean drops 8.46% and replacement mean 15.23%. The ordinary excess
over previous mode shrinks by about 37%, but the tail mean improves only 1.95%
and its p95/p99 regress. Tail median changes from 31.396 to 29.548 ms. Seven
after ticks exceed 80 ms, clustered around elapsed ticks 623–635; the matching
before case has one such tick. No competing game/build/test was detected by
the host census. This first triplet does not establish stable tail recovery.
An additional reversed execution order is pending, with source preparation
kept outside its measured games. The expanded after observer also checks that
any remaining enumeration ride evaluations belong exclusively to the
intentionally unchanged capacity visitor.

Evidence: [CPU summary](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-cpu-summary.json),
[4,096-tick comparison](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-canonical-4096.json),
[replacement endpoint](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-cpu-end-state.json)
and [native tests](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-native-ctest.txt).
