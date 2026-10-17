# Hyperpath boarding CPU recovery

Work on `feat/passenger-hyperpath-routing`, starting at
`21b7f06ad9cbe7e214ac3762d9e73f56e6f3fc46`. Task 1 was measured before the
Task 2 optimization. The measurements and subsequent changes are recorded below.

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
The reversed execution order is reported below, with source preparation
kept outside its measured games. The expanded after observer also checks that
any remaining enumeration ride evaluations belong exclusively to the
intentionally unchanged capacity visitor.

Evidence: [CPU summary](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-cpu-summary.json),
[4,096-tick comparison](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-canonical-4096.json),
[replacement endpoint](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-cpu-end-state.json)
and [native tests](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-native-ctest.txt).

The after observer ends in the same 24-field canonical state as the earlier
before observer. Across 2,048 ticks, cohort count remains 22,842,650 in 154,940
visitor calls. Enumeration time falls from 7.849864 to 6.157043 seconds
(-21.56%); its ordinary-tail time falls from 2.206051 to 1.812770 seconds
(-17.83%). ResolveHyperpath calls fall from 23,374,464 to 778,822 (-96.67%),
and their measured time from 2.557902 to 0.269540 seconds. The complete group
predicate now runs 247,008 times, including the unchanged capacity visitor.

Enumeration ride evaluations fall from 208,740 calls / 3,506,823 stops to
17,570 calls / 220,438 stops. **Every remaining enumeration evaluation belongs
to RecordCapacityMiss; ordinary boarding enumeration has zero calls/stops.**
All ride evaluations fall from 215,829 / 3,690,603 stops to 144,933 /
2,835,865 stops. Their measured time changes only from 174.049 to 143.438 ms,
again contradicting a dominant full-stop-scan explanation. The conservative
superset rejects only 443 FIFO range heads; current wrong-boarding-station
cohort rejections are zero in this interval.

The complete cohort walk remains. About 3.249 seconds of after enumeration
time is outside the light predicate, essentially unchanged from before.
Separation removes repeated concrete resolution but does not remove this
walk, so the observed enumeration cost is only partly recovered. No complete
explanation for the remaining whole-simulation regression is claimed here.
The old observer has 17 stages; after adds full-predicate, capacity-ride and
rejection counters while preserving the original stage definitions.

Evidence: [after attribution](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-after-profile.json),
[raw counters](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-after-profile-boarding.csv)
and [observer state control](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-profile-state.json).

## Reversed control and context membership reuse

The reversed 1,025-tick runs compare only the first 1,024 ordinary ticks;
the one replacement tick is excluded from CPU claims. All canonical fields
match their forward-run midpoints; before/after also match the reversed end.
Ordinary means are previous 22.086, before Hyperpath 26.619, separated v1
30.966 ms. Tail means are 22.130 / 26.398 / 30.552 ms. This reverses the first
triplet's apparent v1 speedup. **The separation alone did not establish a
stable CPU recovery.** The substantial run-order variation must not be hidden.

The follow-up retains an exact base-membership boolean in the existing loading
context and reads the representative intent by const reference. It caches
neither a ride, cost/label nor live legality. A full frozen boarding revision
(publication, invalidation and saved table Work) invalidates these booleans;
vehicle invalidation removes that consist's entries. Keys include carrier,
cargo/order, policy identity, previous carrier and both occurrence identities.
Failed-service membership still consults the table cache, so a cold unknown
can never become a stale negative. No global/shared route-evaluation cache,
reverse index, saved field or Work unit is introduced.

All **724 tests pass**, including a cached negative whose old policy is retired
and must replan under the new publication, distinct occurrence identities,
vehicle invalidation and missing orders. The 2,048-tick midpoint/replacement
end and independent 4,096-tick end again match all 24 canonical fields.
The final guarded observer also matches the pre-optimization observer exactly.

Memo-case uninstrumented milliseconds/tick (mean / p95 / p99):

| Window | Context membership memo |
| --- | ---: |
| Ordinary 1–1,024 | 22.096 / 32.560 / 45.739 |
| Ordinary 513–1,024 | 21.557 / 28.893 / 36.059 |
| Replacement 1,025–2,048 | 25.912 / 33.849 / 40.283 |

These ordinary means are near the reversed previous-mode control, but these
are separate runs, not simultaneous matched CPU samples. A fresh final
three-way comparison after the remaining features is still required.

The final observer records 20,271,043 base/currentness memo hits in 22,767,827
light predicates (89.03%). Full-interval enumeration is 5.800545 seconds
versus the original 7.849864 (-26.11%); the tail is 1.659134 versus 2.206051
seconds (-24.79%). Predicate time is 3.064429 seconds, slightly higher than
v1's 2.908455 despite the reuse, so that timer alone does not establish the
source of the larger uninstrumented gain. The cohort walk persists. Resolve
and ride counts are unchanged from separated v1: ordinary boarding enumeration
still has zero EvaluateRide calls/stops and capacity-only enumeration retains
17,570 calls / 220,438 stops. Measurements establish correctness and repeat
opportunities; they do not establish an exact causal percentage for the memo.

Evidence: [reverse control](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-reverse-summary.json),
[memo CPU](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-memo-cpu-summary.json),
[memo 4,096-tick state](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-memo-canonical-4096.json),
[guarded native tests](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-memo-final-ctest-native.txt)
and [memo attribution](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task2-memo-final-profile-report.json).

The recovered construction fixture is the former
`build-estimated/quality-through-final/result.sav`. Its StateTicks are exactly
24,576 later than the requested original input. It is a later simulation
state on the same topology, not a substitute that reproduces the prior interval.

## Target departure diagnosis

The copied-TU observer is restricted to station 3393, services 524/525/528
and destination 3394. It records available cargo and virtual reservations
separately, runs native eligibility without waking packets, and checks saved
DB bytes/Work, RNG and StateTicks at each departure. Queried packet intent,
failure and event state are also checked. The 4,096-tick checkpoint matches
all 24 canonical fields. All 14 departure guards and 300 packet queries pass.
The observed 24,576-tick run has no competing measurement. Its timings are
excluded from fair CPU comparisons because of the departure observer.

| Service / real order | Departures | Target people boarded | Target people left | Free seats at departure, min–max |
| --- | ---: | ---: | ---: | ---: |
| 524 / 13, forward | 4 | 74 | 0 | 894–941 |
| 524 / 79, reverse | 6 | 0 | 297 opportunities | 830–915 |
| 525 / 13, forward | 2 | 140 | 0 | 270–600 |
| 525 / 175, reverse | 2 | 0 | 34 opportunities | 547–585 |
| 528 | 0 | 0 | — | — |

All 331 remaining passenger opportunities are at reverse departures and
outside their candidate sets. There is no concrete ride to test there;
the -1 legality category must not be described as a native-prefix rejection.
They comprise 238 current Hyperpath-intent and 93 retired Hyperpath-intent
opportunities. **Native-eligible, capacity-available leftover is zero.**
All observed early, expired, unindexed, new-unprocessed and pending-event
counts are zero; no overdue event age is observed. Target reservations and
cancellation-created target opportunities are also zero. Counts repeat
people across departures; they are not distinct-person totals. Forward
departures clear all target waiting cargo, so free seats on reverse departures
do not establish a missed forward opportunity.

No expiry-to-replan pair or pending expiry is observed, so expiry latency
is **not estimable**, rather than measured to be zero. The observed base
intent waiting limits are 28,770 and 36,386 ticks, longer than this interval.
This later input cannot exclude expiry problems in the original interval,
whose reported waiting limit was different.

Published 528 has one vehicle and an estimated circuit of 82,510–83,694 ticks
across the captured publications. Its initial order 33 is a real boarding
stop at 3393 (arrival/departure 26,196/26,418), followed by 3394 at order 34;
the underlying travel estimate is 370 ticks with quality 1. The initially
observed real order is 93. Actual dispatches and transitions execute each
order 94–113 and 0–11 once; order 33 executes zero times. No PBS forecast
is counted as an execution. Thus this interval contains no actual 528
boarding at 3393. The pattern's 111 station stops are distinct from the
real order list's non-station orders.

524's eight frozen vehicles and 528's single vehicle have unknown phases.
The existing model uses its conservative UnknownWait rule, not a circuit
divided by an assumed evenly spaced fleet. This is observed model input,
not a new frequency-learning rule.

These observations narrow the candidates: missed eligible forward loading,
target reservation cancellation, observed event backlog, actual 528 boarding
and observed expiry are not the explanation in this interval. They do not
by themselves identify the original trip-mean regression. The matched
transport control and packet-representative sampling limits must also be
considered before attributing a change in mean travel time.

Evidence: [departure report](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task3-departure-report.json),
[raw waiting rows](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task3-target-waiting.csv),
[departure rows](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task3-target-departures.csv),
[528 executions](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task3-target-order-executions.csv),
[initial patterns](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task3-initial-patterns.json),
[final patterns](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task3-final-patterns.json),
[read-only guards](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task3-target-readonly-checks.csv)
and [state control](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task3-observer-state-control.json).

## Matched transport context for the diagnosis

The later fixed-input control also completes 24,576 ticks. For fresh completed
3393→3394 packet representatives, previous/Hyperpath mean is 2,206.198 /
2,266.631 ticks (+2.74%); median is 1,813 / 1,689, p95 5,062 / 5,522 and
p99 5,705 / 6,811. Mean riding falls 461.000 → 438.477 ticks, while origin
wait rises 1,745.198 → 1,828.153. Both travel the same 208 distance units,
with zero transfers, revisits and immediate returns in these fresh samples.
The observed degradation is in origin waiting, not a longer ridden route.

Actual target-boarding batches differ between modes. Vehicle 319 / service
524 / order 13 boards at elapsed tick 9,473 in previous mode and 8,559 in
Hyperpath. The following vehicle 1354 / 525 / 13 first boards at 14,939 /
15,097. That observed inter-batch gap grows 5,466 → 6,538 ticks. The longer
gap corresponds to the waiting tail, despite all observed Hyperpath forward
departures clearing the target queue. These are actual sampled boarding
batches, not asserted departure timestamps or a counterfactual proof of the
entire mean change. Changed vehicle timing/headway and sample composition
remain supported explanations; a candidate-filter loading failure does not.

There are 286 / 294 fresh target representatives, of which 182 / 176 arrive,
50 / 42 are lost and 54 / 76 remain waiting at the horizon. The completed
means therefore compare different selected cohorts. Do not treat them as a
population-weighted or paired causal estimate. Initially present target
people comprise the same 36 representatives: 29 / 25 arrive and 7 / 11 are
lost; none remain pending. Their arrival remaining-time is 5,423 / 5,315 ticks.

This interval has **no fresh 971→916 or 3222→400 samples**. 971→916 has one
initially present representative, arriving after 4,275 / 4,471 ticks.
3222→400 has no initial representative either. These are unavailable
comparisons, not zero travel times or evidence of improvement. The extended
report now separates fresh, initially present, completed, lost and censored
outcomes for all three targets and verifies their fate accounting.

The original input's reported 2,744 → 2,902 means are not reproduced by this
later save. The diagnosis narrows causes for the observed interval and must
not be promoted to a definitive explanation of that missing interval.

Evidence: [focused outcomes](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task3-transport-focused.json),
[ordinary transport](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task3-transport-ordinary.json),
[boarding batches](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task3-target-boarding-batches.json)
and [completed control](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task3-completed.json).
