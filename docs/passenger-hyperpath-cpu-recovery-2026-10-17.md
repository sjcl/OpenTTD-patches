# Hyperpath boarding CPU recovery

Work on `feat/passenger-hyperpath-routing`, starting at
`21b7f06ad9cbe7e214ac3762d9e73f56e6f3fc46`. Membership/ride separation,
measurement-only target departures, time-based capacity failure and flat policy
storage are implemented. The selected T_FAIL is **4,096 ticks**.

Final untraced ordinary simulation improves **27.522 → 25.136 ms/tick** against
previous HP; an independent reversed sequence confirms **28.548 → 26.253**.
That is an observed **8–9% mean reduction**, with **24.3–27.5% still remaining above
legacy**. The enumeration observer improves about 26%, and normal boarding
enumeration performs zero EvaluateRide calls. CPU recovery is partial.

The identical frozen-map storage comparison reduces reported table bytes
**21.81%** and process peak commit **25.13%**, with full candidate/label/exact
N/F and saved-Work witnesses equal. The Task 2-only 4,096-tick comparison
matches all 24 canonical fields. All **731 native tests**, real MP join,
disable/enable, replacement and three old/new saved-Work readbacks pass.

The requested original input is missing. All new paired runs use the same
recovered save, exactly 24,576 ticks later. Fresh 971→916 and 3222→400 samples
are absent there. Transport gains, adverse metrics and that limitation are
reported with the [final CPU](#final-untraced-cpu-result),
[transport](#final-24576-tick-transport-result) and
[MP/readback](#final-native-saved-work-and-multiplayer-checks) evidence below.
Earlier sections retain the diagnostic sequence and intermediate results.

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
CPU-recovery result was claimed at that initial stage; final results appear below.

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
wait rises 1,745.198 → 1,828.153. Both travel 208 sixteenth-tile units,
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

## Capacity failure recording

At a fully eligible capacity-miss record, exclusion now uses the existing
three-count latch when either three opportunities fail, or elapsed time since
the first failure plus the failed carrier's frozen Pattern.wait reaches T_FAIL.
The expected wait comes from the intent's current/retained table generation;
a retired intent uses the already selected pending plan. An unavailable wait
retains the count-only rule. No expiry/holding rule, failure identity,
split/merge comparison, saved field or event is changed. Actual opportunity
counts remain separate in the capacity diagnostic; the saved saturated count
is the exclusion latch. No-alternative restoration remains intact.

The initial provisional native constant was 2,048 ticks. Both 2,048 and 4,096
were subsequently measured in copied binaries after flat-storage validation;
the final 4,096 choice and evidence are reported below. All 728 native CTest
cases passed at this stage, including
first-full-departure diversion on a long-headway carrier, no-alternative
restoration, short-headway three-count behavior, unknown-headway count-only
behavior, record-time-only latching and a packet Save/Load round trip.
RecordCapacityMiss still uses the full native eligibility visitor.

Evidence: [native test log](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task4-native-ctest.log)
and [native completion](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task4-native-completed.json).

## Flat policy storage

New Hyperpath rows use 24-byte cost/wait/offset/count headers and one candidate
slab per row. Incoming-carrier variants store base minus that carrier plus
every addition. An exact full-payload fallback retains correctness if a
future recurrence introduces an unexpected removal. No candidate is truncated.
Base/full spans refer directly to the slab; a bounded deterministic cache
reconstructs other spans. Normal membership and boarding query the stored
union directly, without materializing a variant span or computing a ride.
Local failed-carrier policy evaluation retains its existing algorithm.

The frozen admission formula now budgets compact headers, row descriptors,
the complete candidate bound and both bounded caches. It never reads OS free
memory or live passenger demand. Logical candidate/header/publication Work
remains charged at the previous boundaries. Labels retain their existing type.

JPSD v15 preserves the complete v14 prefix/trailer and appends separate current
and retained flat-storage flags; passenger_periodic is 17. Older generations
replay their original layout and admission formula, finish their old frontier,
then migrate to a new flat generation. The guard against starting a replacement
during load replay remains intact. New saves require a reader supporting the
new feature version; preserving a prefix does not make old binaries readers.

All 731 native CTest cases pass. Added checks compare every published state,
candidate, label and exact N/F across 16 synthetic networks and different Work
slices; retain eleven additions in one variant; replay v14 Work and mixed v15
generations; reject malformed flat flags; and compare joining peers. Multiple
large DB peers in the new replay test use heap storage to respect the native
test stack. The completed full real-map, separate-process witness and OS-memory
comparison is recorded in the following measurement section.

Evidence: [native tests](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task5-native-ctest.log)
and [native completion](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task5-native-completed.json).

## Complete frozen-map storage witness

The old-layout, flat-layout and persisted-Work replay processes each publish
all 4,917 destinations. Their normalized witnesses contain every waiting state,
all candidate IDs, cost/wait, complete labels and the exact numerator high/low
and frequency. Old versus flat and flat versus replay each compare
**9,511,174,912 bytes exactly**. All target public candidate lists match too.
All three consume **1,928,264,853 logical Work** units; replay uses the persisted
count with 104,729-unit slices rather than the original 16,777,216-unit slices.
The equality claim is a full byte comparison, not only matching hashes.

The measured table contains 37,887,369 non-shared incoming-carrier variants,
10,923,695 additions, 2,028,685 variants with multiple additions and a maximum
of **21** additions. Unexpected non-carrier removals remain zero. Labels and
their representation are unchanged.

Memory in decimal GB; each process owns the same frozen map/DB and one table:

| Component | Old layout | Flat layout | Reduction |
| --- | ---: | ---: | ---: |
| Policy headers | 3.555 | 2.133 | 40.00% |
| Candidate arrays/slabs | 0.439 | 0.236 | 46.18% |
| Policy indices | 0.358 | 0.282 | 21.28% |
| Labels | 3.383 | 3.383 | 0.00% |
| Reported table residency | 7.798 | 6.097 | 21.81% |
| Frozen admission envelope | 16.275 | 14.012 | 13.91% |
| Process peak working set | 7.730 | 6.171 | 20.16% |
| Process peak commit | 9.407 | 7.043 | 25.13% |

The replay process reproduces identical component counters; its peak working
set/commit are 6.161 / 7.043 GB. OS peaks include the fixture, allocator and
witness-export overhead; component counters describe completed table residency.
These are separate-process storage comparisons, not an end-game runtime
inventory or an untraced CPU benchmark.

The recovered construction fixture is a v9 DB/station snapshot, shared exactly
by all three storage runs. Its 3393→3394 base wait is 3,199 ticks, while the
loaded later-state departure diagnostic initially observes 4,110. They are
different frozen service metadata, and must not be silently treated as the
same simulation frontier. Fixture and binary hashes, exact source-file hashes,
committed source identity and compiler configuration are preserved below.
Native JPSD tests and the final real MP/readback separately validate wire and
mixed-generation replay.

Evidence: [complete comparison and counters](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task5-flat-comparison.json),
[persisted Work](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task5-flat-saved-work.json),
[source/binary identities](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task5-flat-source.json),
[compiler configuration](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task5-flat-CMakeCache.txt)
and [replay output](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/task5-flat-replay.log).
The three 9.51-GB witnesses remain in the ignored build directory.

## Failure threshold selection

**The selected native T_FAIL is 4,096 ticks.** Both traced 24,576-tick cases
complete exclusively from the same recovered later input, with identical
initial warming and independent ordinary/focused samples. The 4,096 native
constant and its record-time boundary test pass all 731 CTest cases again.

| Measure | T_FAIL 2,048 | T_FAIL 4,096 |
| --- | ---: | ---: |
| Actual passenger deliveries | 1,577,534 | 1,576,439 |
| End waiting people | 257,480 | 251,533 |
| End physically onboard people | 351,468 | 354,639 |
| HP capacity-missed packet opportunities | 675,595 | 663,975 |
| HP capacity-missed passenger opportunities | 1,358,516 | 1,309,587 |
| LocalPolicy cache constructions | 1,355,972 | 395,601 |
| Fresh complete ordinary sample trip mean / p95 / p99, ticks | 4,309.74 / 12,519 / 18,201 | 4,399.84 / 12,961 / 18,383 |
| Pending-new sample maximum-wait p95 / p99, ticks | 6,736 / 15,796 | 7,324 / 16,990 |
| Fresh samples with revisit / immediate return | 157 / 139 | 143 / 128 |
| Complete fresh sample mean transfers | 0.6077 | 0.6313 |
| 3393→3394 fresh complete mean / p95 / p99, ticks | 2,345.79 / 5,219 / 6,091 | 2,137.99 / 4,941 / 5,377 |
| Traced simulation mean / p95 / p99, ms | 40.368 / 51.604 / 76.056 | 39.585 / 49.146 / 59.580 |

4,096 reduces local-policy construction churn by 70.8%, end waiting by 2.31%
and observed capacity-missed passenger opportunities by 3.60%. Its delivery
count is only 1,095 people (0.069%) lower; generated population also differs
(1,730,237 / 1,724,959), so this is not a paired service-rate estimate. It
improves the observed 3393 target tail and sampled revisit/return counts.
Among the same 6,931 initially present ordinary representatives, 5,471 / 5,489
arrive, 1,145 / 1,127 remain pending and 315 / 315 are lost. For the 5,279
delivered in both cases, the mean elapsed delta is -23.99 ticks, median ratio
1.0 and mean ratio 1.000066: broadly similar, with individual regressions.

The tradeoff is explicit: 2,048 has better fresh-completed ordinary sample
means, a lower mean transfer count and better pending-wait p95/p99. 4,096
has more onboard people. The selection prioritizes substantially lower
recomputation cost and observed target/CPU tails over the very small delivery
difference; it is not a claim that every quality metric improves.

Before the time rule, observed HP records 658,911 packet / 1,279,457 passenger
capacity opportunities and 10,660 local-policy constructions. 4,096 still
raises local construction count 37.1-fold. These counts are cache misses,
including reconstruction after eviction, not distinct passengers replanned.
Capacity totals count eligible recorded opportunities, not unique people.
Legacy's zero HP-capacity counter means that this HP-only diagnostic does not
cover legacy capacity failures. No target samples establish a fresh 971→916
or 3222→400 comparison in this later interval.

All traced timings include transport observations and ordinary table refreshes;
the primary CPU conclusion uses the separate untraced ordinary/replacement
triplet and reversed ordinary control below. The requested original input remains
unavailable, so the threshold evidence applies to this recovered interval.

Evidence: [selection and tradeoffs](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/threshold-selected.json),
[complete review](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/threshold-review.json),
[ordinary comparison](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/threshold-2048-vs-4096-ordinary.json),
[focused comparison](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/threshold-2048-vs-4096-focused.json),
[capacity context](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/threshold-capacity-threshold-context.json)
and [selected-constant tests](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/selected-4096-native-ctest.log).

## Final untraced CPU result

All three 2,048-tick runs start from the identical recovered save, complete an
initial cold table, replace transfer cost 20 with 17 at tick 1,024, and finish
the replacement after the measured interval. Transport tracing and boarding
timers are disabled. The immutable legacy and pre-change HP controls reproduce
all 24 canonical end fields from their earlier controls exactly. The last
post-interval warm-up contributes to process peaks, but not per-tick times.

Values below are mean / p95 / p99 in milliseconds. Ordinary is ticks 1–1,024;
replacement is ticks 1,025–2,048. The tail is ordinary ticks 513–1,024.

| Interval and measure | Legacy | Previous HP | Final HP |
| --- | ---: | ---: | ---: |
| Ordinary simulation | 20.217 / 23.155 / 25.589 | 27.522 / 38.556 / 54.379 | 25.136 / 35.196 / 48.371 |
| Ordinary Journey | 1.271 / 1.862 / 2.229 | 2.109 / 3.212 / 26.189 | 2.094 / 3.925 / 23.118 |
| Ordinary tail simulation | 20.272 / 23.189 / 25.589 | 26.697 / 34.312 / 42.600 | 24.615 / 32.694 / 40.665 |
| Replacement simulation | 22.492 / 33.814 / 38.710 | 31.803 / 40.249 / 48.137 | 29.463 / 38.881 / 46.407 |
| Replacement Journey | 3.224 / 15.164 / 17.412 | 6.876 / 12.072 / 13.303 | 6.681 / 12.403 / 14.018 |

The primary sequence improves ordinary mean by 8.67% and replacement mean by
7.36% against previous HP. Ordinary remains 24.33% above legacy; the excess
shrinks by 32.66%. The tail mean improves 7.80% against previous HP, while
remaining 21.42% above legacy. This is partial CPU recovery. The Journey mean
barely changes in the ordinary interval, consistent with the removed work
being outside that timer.

These timings compare the combined final changes, including flat storage and
the new capacity rule. They are not an isolated causal estimate for the Task 2
memo. Its behavior-preserving canonical comparison is reported separately.

The independent reverse sequence runs final → previous HP → legacy. It accepts
only the first 1,024 ordinary ticks; its single replacement tick is excluded.
All 24 midpoint canonical fields agree with each corresponding primary run.

| Reversed ordinary measure | Legacy | Previous HP | Final HP |
| --- | ---: | ---: | ---: |
| Simulation mean / p95 / p99, ms | 20.598 / 23.457 / 26.192 | 28.548 / 39.066 / 52.884 | 26.253 / 38.653 / 52.575 |
| Journey mean / p95 / p99, ms | 1.294 / 1.900 / 2.288 | 2.008 / 3.312 / 24.096 | 2.225 / 4.530 / 23.656 |
| Tail simulation mean / p95 / p99, ms | 20.582 / 23.371 / 26.192 | 27.873 / 35.244 / 43.558 | 25.132 / 34.079 / 42.420 |

The reverse ordinary mean improves 8.04% against previous HP, and its tail
improves 9.84%. Final remains 27.45% above legacy. Both observed orders therefore
show an approximately 8–9% ordinary mean improvement, while latency tails vary
and the reverse Journey mean worsens. Two ordered runs are not a randomized
confidence interval or a claim of full CPU recovery.

Final CPU process peak working set / commit are 8.273 / 8.735 decimal GB,
against previous HP 10.232 / 11.103 and legacy 7.187 / 7.214. These include
initial/replacement construction and must be kept separate from the identical
single-table frozen-map storage comparison above.

The final copied-TU boarding observer also completes 2,048 ticks and agrees
with the uninstrumented final run in all 24 canonical end fields. Its counts
are shown alongside the original and the behavior-preserving Task 2 memo run.
Times are observer measurements, with nested scopes and timer overhead.

| Full interval counter | Before Task 2 | Task 2 memo only | Final combined HP |
| --- | ---: | ---: | ---: |
| Visitor calls | 154,940 | 154,940 | 154,046 |
| Enumerated cohorts | 22,842,650 | 22,842,650 | 22,832,661 |
| Enumeration seconds | 7.849864 | 5.800545 | 5.827620 |
| Light/full enumeration predicate calls | 22,842,650 | 22,767,827 | 22,755,865 |
| Enumeration predicate seconds | 4.583700 | 3.064429 | 3.069754 |
| ResolveHyperpath calls | 23,374,464 | 778,822 | 785,719 |
| ResolveHyperpath seconds | 2.557902 | 0.234133 | 0.237094 |
| All EvaluateRide calls / visited stops | 215,829 / 3,690,603 | 144,933 / 2,835,865 | 404,541 / 10,622,684 |
| All EvaluateRide seconds | 0.174049 | 0.129091 | 0.243642 |
| Enumeration EvaluateRide calls / stops | 208,740 / 3,506,823 | 17,570 / 220,438 | 22,013 / 339,879 |

All remaining enumeration ride evaluations in both after runs are inside
RecordCapacityMiss; ordinary boarding enumeration has **zero calls and stops**.
Final base/currentness memo hits are 20,265,093 of 22,755,865 light predicates
(89.05%). The Task 2 memo run preserves the same cohort population and reduces
enumeration time 26.11%; its ordinary tail falls 2.206051 → 1.659134 seconds.
The combined final run is 5.827620 seconds overall and 1.700346 in that tail.
Its new time rule changes passenger behavior, so these final counters are not
a behavior-identical optimization experiment.

The new failure rule raises total ride work above the original run, even
though the normal boarding pre-scan remains removed. The visitor still walks
22.8 million cohorts. Final enumeration contains 3.070 seconds in the measured
light-predicate scope and about 2.758 seconds outside it, including traversal
and the capacity-only full checks. These nested times must not be added to
the full visitor/Journey scopes. The results support residual cohort traversal
and membership cost, while not accounting for every simulation-time increment.
The original attribution remains 59.88% of the ordinary-tail simulation
increment in enumeration, with EvaluateRide itself only a small part of that
cost. Repeated ResolveHyperpath/predicate calls, rather than repeated complete
ride-cache misses, were the material pre-scan work.

Evidence: [primary CPU](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/final-cpu-summary.json),
[reverse CPU](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/final-reverse-summary.json),
[reverse state controls](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/final-reverse-controls.json),
[final observer](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/final-final-profile-report.json),
[observer state control](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/final-final-profile-state-control.json)
and [per-tick observer counters](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/final-boarding-profile.csv).

## Final 24,576-tick transport result

The legacy and selected 4,096 cases use the same recovered later input. Full
population transport counts and end-save inventory are distinct from the
unweighted one-in-64 packet representatives used for trip distributions.
The focused representatives cover all three requested ODs independently.

| Measure | Legacy | Final HP, T_FAIL 4,096 |
| --- | ---: | ---: |
| Initial passengers | 743,298 | 743,298 |
| Generated passengers | 1,726,505 | 1,724,959 |
| Delivered passengers | 1,546,349 | 1,576,439 |
| Remaining passengers | 637,250 | 606,172 |
| Waiting passengers | 296,457 | 251,533 |
| Physically onboard passengers | 340,793 | 354,639 |
| Other removals | 286,204 | 285,646 |
| Population transfer events | 1,522,326 | 1,766,167 |
| Fresh complete ordinary representatives | 13,820 | 14,222 |
| Fresh complete trip mean / p95 / p99, ticks | 4,412.93 / 12,807 / 18,619 | 4,399.84 / 12,961 / 18,383 |
| Mean transfers per complete fresh representative | 0.5664 | 0.6313 |
| Fresh representatives with revisit / immediate return | 64 / 58 | 143 / 128 |
| Right-censored fresh representatives | 6,833 | 6,697 |
| Pending maximum-wait mean / p95 / p99, ticks | 2,845.96 / 10,647 / 18,065 | 2,252.86 / 7,324 / 16,990 |
| Traced simulation mean / p95 / p99, ms | 34.151 / 45.934 / 52.711 | 39.585 / 49.146 / 59.580 |
| Traced Journey mean / p95 / p99, ms | 3.286 / 15.536 / 18.732 | 6.287 / 11.904 / 13.510 |
| Process peak working set / commit, GB | 7.305 / 7.338 | 8.356 / 8.842 |

Delivered population is 1.95% higher and waiting population is 15.15% lower,
with 4.06% more physically onboard. Transfers and sampled revisit/return counts
increase. Completed-trip mean improves only 0.30%, p95 worsens and p99 improves.
Completed and censored samples have different fates across runs; these are
descriptive distributions, not paired causal estimates of all travelers.
The longer traced run has ongoing table refreshes and observer overhead, and
does not replace the untraced CPU result.

The final HP records 663,975 eligible packet / 1,309,587 passenger capacity-miss
opportunities and 395,601 LocalPolicy cache constructions. These are repeated
opportunities/cache misses, not unique failed people or unique replans. Legacy
has no corresponding HP failure counter. The threshold comparison and its
37.1-fold construction increase against count-only HP remain material CPU
limitations, despite selecting the less costly 4,096 alternative.

For 3393→3394, fresh completed mean / p95 / p99 are 2,206.20 / 5,062 / 5,705
legacy versus 2,137.99 / 4,941 / 5,377 final HP. There are 182 / 194 completions
from 286 / 312 starts, 50 / 57 losses and 54 / 61 censored representatives.
All pending target representatives are waiting, with elapsed mean 2,799.83 /
3,076.70 and p95 6,346 / 6,448. Both completed populations have zero transfers,
revisits and immediate returns and distance 208 sixteenth-tile units. Mean
riding time is 461 / 453.96; corresponding origin wait is 1,745.20 / 1,684.03.
Among the same 36 initially present representatives, 29 / 25 complete and
7 / 11 are lost, with no pending representatives. Their remaining arrival
time is 5,423 / 5,405 ticks. The better completed fresh target distribution
does not erase the worse loss/censoring counts or establish the missing
original interval's cause.

Neither run generates a fresh 971→916 or 3222→400 representative. The single
initial 971→916 representative arrives after 4,275 / 4,369 ticks. 3222→400 has
no initial representative. Fresh comparisons for those ODs are unavailable.

## Final native, saved-Work and multiplayer checks

The selected native source passes all **731 CTest cases**. The real loopback
MP test also passes with no network/desync error: join, routing disable/enable
and replacement all match the 18 compared canonical/derived fields on both
peers. It requests 1,024 transport ticks and observes **1,071 actual ticks**
because the asynchronous network probe can overshoot; the final peer snapshots
have identical StateTicks and state. This is an MP synchronization check, not
an exact-duration CPU measurement.

The test uses frame_freq 20, paused join, the completed final 24,576-tick save,
and transfer-cost replacement 20 → 17. Server preparation takes 497.610 seconds
without advancing paused simulation. The client reports 197 replay progress
packets and replays 3,407,850,773 actual Work in 17,758 slices. Its start/end
StateTicks are both 3,181,144,157 and its replay frame remains 97; maximum
outer replay loop is 15.379 ms. Client join-budget time is 201.266 seconds.
The larger progress denominator is an upper estimate, not the actual replay
Work. Join, toggle and transport mismatch lists are all empty.

The observed network timeout settings remain their defaults: max_join_time
500, max_join_time_total 900, max_lag_time 500, max_init_time 100 and
max_download_time 1,000. The harness's 600-second observation bound is separate
from these product settings. It completes without raising that bound or
altering a product timeout. Configuration and observed-setting lines are
preserved with the result.

All three end saves load and replay their saved Work without advancing
StateTicks (3,181,144,157 in each case). Legacy and count-only HP upgrade JPSD
14 → 15 with neutral flat flags and byte-verified original prefixes. The final
HP save verifies its v15 bytes unchanged. Each then reports identical saved DB
bytes across forced replay. Every non-derived canonical field matches the
corresponding end snapshot; current/retained row counters and table operations
are reconstructed, and the known v14 → v15 database-digest change is covered
by the independent wire comparison.

| Replayed end save | Current rows | Retained rows | Current saved table Work | Waiting / physically onboard people |
| --- | ---: | ---: | ---: | ---: |
| Legacy, v14 → v15 | 1,272 | 3,645 | 1,477,967,872 | 296,457 / 340,793 |
| Count-only HP, v14 → v15 | 3,767 | 1,150 | 1,477,902,336 | 253,225 / 350,142 |
| Final flat HP, v15 | 3,767 | 1,150 | 1,478,033,408 | 251,533 / 354,639 |

The complete end-save census agrees with live transport metadata, including
virtual reservations being excluded from physically onboard people. All three
have zero reserved people at this endpoint. Pending event gauges after load
are 200,676 / 162,012 / 159,305 respectively; these are reconstructed live
gauges, not the cumulative scheduler-work counters reset on load. Their oldest
overdue ages are 1 / 0 / 0 ticks.

Actual replayed HP table components below use decimal GB and cold derived
caches. Reported table bytes and component counters do not include all process
allocations; the OS peaks are reported separately above.

| End-save table | Reported bytes, GB | Labels | Policy headers | Candidates | Indices |
| --- | ---: | ---: | ---: | ---: | ---: |
| Count-only HP current | 5.982 | 2.592 | 2.719 | 0.333 | 0.274 |
| Count-only HP retained | 1.876 | 0.791 | 0.835 | 0.103 | 0.084 |
| Final flat HP current | 4.684 | 2.592 | 1.632 | 0.181 | 0.216 |
| Final flat HP retained | 1.477 | 0.791 | 0.501 | 0.056 | 0.066 |

Their combined reported HP bytes are 7.859 / 6.161 GB. These are different
simulation outcomes/frozen generations; matching row counts do not make this
an isolated layout experiment. Use the identical frozen-map witness comparison
for the storage-only reduction. Legacy's HP-specific component API returns
zero because it uses another table mode; that is not zero legacy allocation.

Evidence: [MP result and snapshots](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/final-mp-result.json),
[observed default limits](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/final-mp-default-limits.json),
[readback/wire/inventory results](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/final-readback-result.json),
[old HP replay console](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/final-readback-hyperpath-console.txt),
[final flat replay console](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/final-readback-threshold-4096-console.txt)
and [combined final evidence](measurements/passenger-hyperpath-cpu-recovery-2026-10-17/final-summary.json).

## Remaining limits

The Task 2 behavior-preserving 4,096-tick canonical check, flat-map full witness
and saved-Work proofs establish the intended correctness properties. The
boarding pre-enumeration ride scan is removed. Ordinary CPU has residual
cohort-walk/membership work and other costs; it has not returned to legacy
speed. The final time rule also increases local-policy reconstruction churn.
No excluded reverse index, shared frozen ride-evaluation cache, narrowed label
type, learned effective frequency or new passenger event has been introduced.

The missing original input prevents the requested same-interval reproduction.
All newly paired evidence uses SHA256 306a8d…c629, exactly 24,576 StateTicks
later than the requested SHA256 6783a5…978. This report must not be read as
having reproduced the former 2,744 → 2,902 target-OD result.
