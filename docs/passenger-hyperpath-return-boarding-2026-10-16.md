# Hyperpath return boarding and waiting scheduler follow-up

This follow-up applies to `feat/passenger-hyperpath-routing`, after the
[initial transport comparison](passenger-hyperpath-routing-2026-10-16.md).

The final witness rule removes most sampled return rides. The waiting scheduler
also becomes cheaper than the unfixed Hyperpath implementation. **The overall
performance and transport-quality goal remains unmet:** simulation is still
slower than the old mode, 971→916 still waits longer, and onboard occupancy and
some route tails remain worse. The measurements below distinguish these limits
from the implemented corrections.

## Correlated departures and the operational rule

Two visits to a station are two real departures. However, when one vehicle
travels A → B → A → D, boarding at the first A does not advance its arrival at
D. Treating both opportunities as independent in the frequency model can
therefore underestimate waiting cost and encourage unnecessary occupancy.
Earlier boarding can still secure capacity. The new rule deliberately avoids
the out-and-back boarding; it is an operational preference for correlated
opportunities, rather than a claim of universal Spiess–Florian dominance.

Graph preparation records the next visit to the same station and the exact
departure-to-departure Ride cost, including uncertainty and intermediate dwell.
The Board event suppresses the earlier candidate only when both departure
labels are settled and finite, checked addition gives exact equality, and a
same-run continuation witnesses a first alighting strictly beyond the later
departure. A budgeted prefix scan also retains the earlier candidate when any
optimal alighting at or before that return ties the continuing path. LocalPolicy
uses the same equality and continuation rule; its forward evaluation prefers
the earlier alighting on such ties.

NoLoad, forced unloading, broken continuity, unavailable return boarding and
overflow prevent suppression. Road orders additionally require matching known
captured direction restrictions. Ride edges remain available to people already
aboard from an earlier station. Forward/reverse cost assertions remain enabled.

JPSD v14 preserves the complete v13 prefix and appends checked witness flags and
packed facility restrictions. The `passenger_periodic` feature is 16. Old v12
and v13 Work frontiers replay with their original algorithm identities; a
completed old generation is replaced by the new rule. New saves require a
reader supporting `passenger_periodic` feature 16; preserving the prefix does
not make them readable by older binaries. Tests cover partial
replay, malformed trailers, the circuit seam, earlier tied alighting, return
destinations, NoLoad, direction restrictions, uncertainty and through riders.
The final native suite passes all 721 cases, including the unchanged-fallback
and old-save publication regressions added afterward. Evidence:
[native test log](measurements/passenger-hyperpath-return-boarding-2026-10-16/ctest-final-721.log).

An older save's unknown access metadata is enriched without changing an otherwise
identical service's publication generation. The immutable table snapshot and its
saved Work remain intact. Actual order edits still change the incarnation. The
legacy 1,024-tick midpoint and 2,048-tick endpoint match the earlier control in
every canonical field except the versioned database digest. Evidence:
[midpoint](measurements/passenger-hyperpath-return-boarding-2026-10-16/cpu-old-midpoint-control.json)
and [endpoint](measurements/passenger-hyperpath-return-boarding-2026-10-16/cpu-old-endpoint-control.json).

## First diagnostic run

The numerical-only v13 prototype completed 24,576 ticks, but another worktree's
builds and measurements overlapped it. Its CPU timings are excluded from
controlled performance comparisons. The raw competition records are retained;
the explicit `--transport-only` report omits the timing comparison.

Its deterministic scheduler counts identified generation sweeps as the largest
source of additional work: 17,883,020 packet visits versus 6,242,978 in the old
mode. Failure, migration and expiry classifications were respectively 844,724,
177,036 and 1,921,866 visits. A separate capacity-record timer covers the
completed-loading visitor outside the national scheduler. Expiry classification
also includes timed Journey checks and is not identical to the expired-policy
counter.

The prototype reduced fresh sampled paths containing a revisit from 393 in the
unfixed Hyperpath run to 29, and immediate returns from 391 to 25. Sample
40772:2 boarded service 300 at order 1 and avoided 385 → 387 → 385. Its arrival
was 6,861 ticks earlier than the unfixed Hyperpath result, although it remained
later than the old mode. These are reference outcomes for the numerical rule;
the final witness-rule outcomes are reported separately below.

The numerical prototype is retained as
[reference evidence](measurements/passenger-hyperpath-return-boarding-2026-10-16/reference-numerical.json),
with [raw artifact hashes](measurements/passenger-hyperpath-return-boarding-2026-10-16/reference-raw-sha256.json).

At station 3393, actual departures in that prototype were 9 for service 524,
5 for 525 and 1 for 528. The sole 528 departure was at order 79; candidate order
33 departed zero times. Native capacity-miss hooks recorded zero packet or
passenger misses for 524 and 525 during the interval. This supports investigating
the modeled supply at occurrence 528(33), without inferring its long-term
frequency from one observation window.

## Waiting work and memory inspection

Scheduler diagnostics separate new, expiry, failure, migration and generation
work. A sleeping fallback can remain unresolved even when the base row has a
route: the incoming-service exclusion or live validation may prevent its use.
The scheduler now shares the exact cached query with PlanWaiting. When that
query still offers no ride, it renews the original retry timestamp and refreshes
the merge index without rebuilding boarding indices. The saved sweep cursor,
Work debit, FIFO position and final native packet fields match the full pass.
Packet deadlines, changed CargoDist hops, failure state, a usable ride and
deterministic memory fallback retain ordinary processing. An additional counter
reports these unchanged routing decisions. Warm/cold tests compare native retry
state with the full pass and verify that subsequent strict merges still work.

The short comparison uses one frozen executable, 2,048 untraced ticks and a
declared transfer-cost change after tick 1,024. The ordinary tail is ticks
513–1,024; replacement samples are ticks 1,025–2,048. The reverse-order check
uses the same ordinary trajectory and rejects a differing canonical midpoint.
These are elapsed simulation-thread measurements; operating-system process CPU
also includes startup, warming and saving. The 24,576-tick trace uses separate
ordinary and focused samples for transport outcomes and inclusive scheduler
attribution.

The explicit `hyperpath_memory` inspection separates graph/workspace, labels,
policy indices, policy headers, candidate arrays and local cache allocations.
It also checks whether each different incoming-service policy can be represented
as exclusion of that service plus an addition list, counting unexpected removals
and multiple additions. The estimate includes cost, wait and flat-list offsets;
allocator overhead and materialized-span caches are excluded. Saved-Work replay
checks database bytes and StateTicks before using this inspection. Compression
is not introduced by this diagnostic.

## Final matched 24,576-tick comparison

Both modes use source `4f2835adb5d0319be7a0d044204d7bd3b7e092c8` and the same
frozen measurement executable, SHA256
`d088e44d7e9bf920e72b97cbf70dbcceb536a0fbfc6f3b051e2b1d3b748fa63a`.
The input SHA256 is
`6783a500f47435a342a36e9a6544e33bb38f041abe91e9e2ff2c654edd16c978`.
Both tables finish warming before the measured interval, from StateTicks
3,181,095,005 through 3,181,119,581. Each case records exactly 24,576 simulation
samples, exits successfully, has no error-pause resumes and records no competing
build/game process. A shared exclusive lock serializes CPU runs and builds.
Assets are frozen inside this worktree.

The measurement executable includes diagnostic driver/trace code and the two
pre-existing working-tree changes identified by hashes in
[binary provenance](measurements/passenger-hyperpath-return-boarding-2026-10-16/measurement-binary-provenance.json).
Those changes are left untouched and are not part of these commits. The
[long manifest](measurements/passenger-hyperpath-return-boarding-2026-10-16/long-manifest.json)
and mode-specific process records preserve input/result hashes and raw sample
hashes. Canonical end-save readback has zero mismatches in both modes.

The old-mode control matches the earlier clean old run at ticks 4,096, 16,384
and 24,576 in every canonical field except the versioned database digest. All
six ordinary transport CSVs are byte-identical. See
[control equality](measurements/passenger-hyperpath-return-boarding-2026-10-16/long-old-control-equality.json)
and [readback](measurements/passenger-hyperpath-return-boarding-2026-10-16/long-readback.json).

| Whole population, people | Old | Final Hyperpath |
| --- | ---: | ---: |
| Initial passengers | 954,776 | 954,776 |
| Generated during interval | 1,730,357 | 1,732,059 |
| Delivered | 1,600,764 | 1,630,202 |
| Known-OD delivered | 934,857 | 963,449 |
| Remaining | 744,046 | 721,272 |
| Waiting at stations | 351,526 | 308,323 |
| Aboard | 392,520 | 412,949 |
| Other removals | 340,323 | 335,361 |

Remaining passengers include passengers aboard and waiting. Generated counts
need not match because the routing-dependent simulation trajectories diverge.
Final Hyperpath has 8,197 fewer people aboard than unfixed Hyperpath (421,146),
but still 20,429 more than the old mode. Delivery increases by 1.84% and station
waiting falls by 12.29% relative to the old mode.

| Ordinary sample, different fresh cohorts | Old | Unfixed Hyperpath reference | Final Hyperpath |
| --- | ---: | ---: | ---: |
| Observed fresh samples, all outcomes | 13,510 | 13,420 | 13,321 |
| Completed trips | 6,074 | 6,174 | 6,048 |
| Any observed fresh path containing a station revisit | 24 | 393 | 16 |
| Any observed fresh path containing an immediate return | 23 | 391 | 16 |
| Mean trip time, ticks | 3,177.55 | 3,164.70 | 3,081.63 |
| Mean riding time, ticks | 1,794.51 | 1,863.09 | 1,776.04 |
| Mean waiting time, ticks | 1,383.04 | 1,301.60 | 1,305.59 |
| Trip p95 / p99, ticks | 8,131 / 10,601 | 7,998 / 10,500 | 7,992 / 10,813 |
| Mean transfers | 0.3767 | 0.4065 | 0.3925 |
| Mean distance, x16 tile units | 932.38 | 982.58 | 972.11 |
| Distance p99 / maximum | 5,264 / 25,556 | 5,368 / 12,831 | 5,631 / 85,490 |

These are hash-sampled packet representatives of different newly generated
cohorts, not paired identical passengers. The two path-count rows include
delivered, pending and lost fresh samples; time, distance and transfer
distributions include only completed trips. The large return reduction is
relative to the clean unfixed run, not the numerical prototype. Revisit counts
describe paths containing a revisit, rather than total repeated-station occurrences.
Some poor paths remain: final maximum revisit count is 9 (old 2), even though
maximum immediate-return count is 2 in both modes. Examples and complete
aggregates are in
[outliers](measurements/passenger-hyperpath-return-boarding-2026-10-16/long-outlier-details.json)
and [ordinary transport](measurements/passenger-hyperpath-return-boarding-2026-10-16/long-transport-ordinary.json).

### Focused destinations

For initial passengers delivered in both modes, the paired comparison follows
the same initial packet and offset. Times measure remaining time from the
interval start, not a fresh complete journey.

| Paired initial OD | People | Old mean, ticks | Final mean, ticks | Old p95 | Final p95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 971→916 | 46 | 6,049.65 | 8,373.85 | 16,294 | 22,575 |
| 3222→400 | 7 | 3,992.14 | 1,687.00 | 5,673 | 1,687 |

971→916 improves from the unfixed Hyperpath mean of 9,100.37 ticks, but is still
38.42% slower than the old mode. The original problematic person illustrates
the remaining capacity problem:

| Initial person 40772:2, OD 971→916 | Old | Unfixed Hyperpath | Final Hyperpath |
| --- | ---: | ---: | ---: |
| Remaining time to delivery, ticks | 2,333 | 14,998 | 14,503 |
| Riding, ticks | 1,749 | 3,063 | 2,030 |
| Waiting, ticks | 583 | 11,934 | 12,472 |
| Service 300 boarding order | 1 | 65 | 1 |
| Path after transfer at 385 | 385;913;916 | 385;387;385;913;916 | 385;913;916 |

In the final run this passenger cannot board order 1 at ticks 3,181,095,997 and
3,181,101,787 because capacity/reservation leaves zero free seats. It boards
order 1 at tick 3,181,107,926, with 865 free seats, and arrives at tick
3,181,109,508. The unwanted return is removed, but waiting gets worse than the
unfixed result. This is consistent with the limitation that avoiding early
boarding can forgo reserving a seat. Evidence:
[old events](measurements/passenger-hyperpath-return-boarding-2026-10-16/long-previous-40772-events.csv),
[final events](measurements/passenger-hyperpath-return-boarding-2026-10-16/long-hyperpath-40772-events.csv).

For 3393→3394, fresh focused cohorts contain 110 old and 124 final trips. Mean
time is 2,744.17 versus 2,902.08 ticks; p95 is 6,636 versus 7,465 and p99 is
7,370 versus 8,145. Final is better than the unfixed mean of 3,137.81 ticks,
but remains worse than old. These fresh cohorts are not identical people.
See [focused transport](measurements/passenger-hyperpath-return-boarding-2026-10-16/long-transport-focused.json).

The final interval observes actual departures at 3393 of 9 for service 524,
5 for 525 and 1 for 528 (old: 10, 5 and 1). The only final 528 departure is
**order 79**, at tick 3,181,112,325; **order 33 departs zero times**. Maximum
observed successive gaps are 4,836 ticks for 524 and 6,734 for 525. All observed
departures have spare capacity, and actual completed-loading miss hooks record
zero packet/passenger misses for 524 and 525 at this station. This interval does
not support capacity failure as their explanation; the occurrence-specific
supply model needs further investigation. A single departure cannot establish
528's long-run frequency. Evidence:
[departures](measurements/passenger-hyperpath-return-boarding-2026-10-16/long-hyperpath-focused-transport-departures.csv),
[miss summary](measurements/passenger-hyperpath-return-boarding-2026-10-16/long-hyperpath-focused-transport-departure-summary.csv).

## Scheduler attribution and remaining CPU cost

All numbers in this table are cumulative for the traced 24,576-tick interval.
Per-cause timers cover routing/index operations after packet selection. Their
sum does not cover all scheduler work. Capacity-record visitor time is measured
outside the scheduler and must not be added to a cause already inside it.

| Scheduler category | Old packet visits | Final packet visits | Old seconds | Final seconds |
| --- | ---: | ---: | ---: | ---: |
| New | 1,221,932 | 1,126,143 | 9.192 | 8.068 |
| Expiry/check | 1,760,371 | 1,917,685 | 8.003 | 8.449 |
| Failure | 0 | 842,150 | 0 | 5.136 |
| Legacy migration | 0 | 177,036 | 0 | 0.859 |
| Generation sweep | 6,242,978 | 17,066,990 | 29.119 | 29.777 |
| Unattributed scheduler overhead | — | — | 5.734 | 13.772 |
| **Entire scheduler** | **9,882,183** | **21,216,953** | **52.048** | **66.062** |

The unchanged-generation fast path handles 16,458,955 of 17,066,990 generation
visits (96.44%). Sweep visits remain counted and budgeted, but these visits
avoid rebuilding the boarding index. Final generation time is near old despite
2.73 times as many visits. Entire scheduler time falls from approximately
103.1 seconds in unfixed Hyperpath to 66.1 seconds, but remains 26.93% above
old. Historical-versus-final timings are separate complete runs and do not
isolate this optimization from the witness correction.

Final capacity misses are 746,746 packet opportunities / 1,522,939 passenger
opportunities, versus unfixed 804,313 / 1,643,287. Repeated failures can count the
same person multiple times. The separate capacity visitor takes 6.269 seconds.
The final global local-policy counter is 394,342 replans / 3.320 seconds.
Schedule-policy expirations are 42,324, distinct from the 1,917,685 expiry/check
visits in the table. Full evidence:
[simulation aggregates](measurements/passenger-hyperpath-return-boarding-2026-10-16/long-simulation.json)
and [final counters](measurements/passenger-hyperpath-return-boarding-2026-10-16/long-hyperpath-console.txt).

Failure and migration still add work; scheduler selection, group iteration and
context cleanup remain in the unattributed portion. The attribution therefore
does not prove one complete explanation for the residual slowdown. Grouped
lazy capacity records, expiry renewal/jitter and publication-triggered legacy
wakeups are not implemented in this follow-up. The measured dominant generation
index work was addressed first.

| Elapsed simulation per tick, ms | Old mean | Final mean | Old p95 / p99 | Final p95 / p99 |
| --- | ---: | ---: | ---: | ---: |
| Long traced interval | 39.689 | 54.761 | 55.251 / 67.730 | 73.870 / 89.811 |
| Untraced ordinary 1–1,024 | 26.580 | 35.134 | 31.405 / 39.046 | 55.348 / 73.828 |
| Untraced ordinary 513–1,024 | 25.791 | 34.320 | 29.522 / 32.105 | 52.786 / 71.952 |
| Untraced replacement 1,025–2,048 | 30.187 | 36.228 | 47.816 / 55.106 | 49.729 / 62.213 |
| Reverse-order ordinary 1–1,024 | 27.538 | 42.521 | 36.121 / 45.438 | 65.191 / 83.074 |

Trace overhead and elapsed-time variance preclude pooling these rows. Ordinary
and replacement Journey means in the untraced comparison are respectively
1.497 / 2.718 ms and 3.949 / 7.563 ms (old / final). The reverse-order run executes
Hyperpath first and old second; both canonical midpoints match the normal-order
cases in every field, including database digest. It runs one subsequent
transition tick solely to obtain the midpoint; that tick is excluded from the
1,024 ordinary samples. Both execution orders still show a regression. The
long interval has a 38% overall elapsed simulation regression relative to old;
the scheduler improvement does not establish low overall CPU cost.
See [untraced samples](measurements/passenger-hyperpath-return-boarding-2026-10-16/cpu-sample-brief.json),
[reverse-order results](measurements/passenger-hyperpath-return-boarding-2026-10-16/cpu-reverse-summary.json)
and [canonical equality](measurements/passenger-hyperpath-return-boarding-2026-10-16/cpu-reverse-manifest.json).

Long-run process peak working sets are 6.814 GiB old and 9.690 GiB final
Hyperpath; peak commit sizes are 6.849 and 10.563 GiB. These are process-wide
peaks, including construction, and cannot be attributed directly to end-save
policy allocations. Memory compression remains an open task.

## Saved-Work memory audit

The inspector replays only the saved logical Work at the end of the unfixed
v12 run and final v14 run. Both reach Ready, preserve StateTicks 3,181,119,581
and reproduce their database bytes exactly. For v12, the loader's v14 wire
upgrade is independently checked against the original save: the old prefix is
unchanged apart from its version marker and the documented added zero fields;
the new witness/facility trailer is zero. Canonical packet, payment, ownership
and loading fields have zero mismatches. This is a read-only audit, not a CPU
benchmark. Evidence:
[memory/readback result](measurements/passenger-hyperpath-return-boarding-2026-10-16/memory-readback.json).

The following allocations sum the current and retained tables for cargo 0.
The unfixed endpoint has 3,873 current / 1,044 retained rows; final has
3,771 / 1,146. Both cover 4,917 rows, but the same Work budget has advanced the
new witness algorithm to a different frontier. Values are decimal GB and use
vector capacities; allocator metadata is excluded.

| End-save allocation component | Unfixed Hyperpath | Final Hyperpath |
| --- | ---: | ---: |
| Reported total, including other workspace | 7.882 | 7.861 |
| Labels | 3.383 | 3.383 |
| Policy indices | 0.358 | 0.358 |
| Policy headers | 3.555 | 3.555 |
| Candidate arrays | 0.460 | 0.439 |
| Graph/static workspace estimate | 0.012 | 0.013 |
| Local cache after load | 0 | 0 |

The approximately 21.5 MB total reduction is small. Policy header capacity and
labels dominate these allocations; candidate arrays alone do not explain the
peak. The live local cache is derived and cleared by loading. End-save totals
cannot establish the component split at the process-wide peak.

| Incoming-service variants, current + retained | Unfixed | Final |
| --- | ---: | ---: |
| Variants different from base | 37,945,702 | 37,975,608 |
| Added candidate IDs | 11,785,369 | 10,513,973 |
| Variants with more than one addition | 2,157,485 | 1,940,040 |
| Maximum additions per variant | 24 | 21 |
| Unexpected nonexcluded removals | 0 | 0 |
| Exclude-service + flat additions estimate, bytes | 957,838,324 | 953,470,484 |

Thus the observed variants can be represented by excluding the incoming
service and storing an addition list, but not by assuming at most one addition.
Excluding a service may remove multiple occurrences of that service. The
estimate is `24 × differing variants + 4 × additions`, including cost, wait,
offset and count. It is only the variant representation: base policies, labels,
indices and any materialized-span cache still need storage. It must not be
subtracted from all policy headers and arrays as if base policies disappeared.
This measurement supports a separate compression change; no such change is
claimed here.

The failure-state merge counterfactual repacks 178,339 final station packets
into 169,462 bins under strict equality and 169,315 when ignoring failure state,
a difference of 147 bins. The unfixed difference is 190. Repacking permits
splits; this is a counterfactual bin count, not an observed merge saving or a
measure of failure-record CPU cost.

## Deferred-load update guard

The first MP attempt joined within the native deadline and matched database
bytes, passengers, payments, ownership and loading. It nevertheless failed:
the server had zero retained rows and the client reconstructed 124. The
initial-build diagnostic hook had called `TickPrecomputed` while the server's
deferred v12 load was still replaying. The new algorithm identity could then
start a replacement from an empty local table, although its saved old snapshot
would be reconstructed in full on the client. Matching saved bytes alone did
not prove equal usable rows. The failed probe is retained as
[failure evidence](measurements/passenger-hyperpath-return-boarding-2026-10-16/mp-first-replay-update-failure.json).

Commit `7dfcfb53c3` makes `TickPrecomputed` defer ordinary updates while load replay is active.
Only `StepLoad` advances the original frontier; algorithm migration starts after
Ready. The v12/v13 sliced-replay regression attempts an ordinary update with
budget 67,108,864 before every replay slice and checks that neither saved bytes nor replay
Work changes. Existing plan, row-count and mixed-generation replay comparisons
still apply. The full 721-test native suite passes with this guard.

This later guard is outside the measured 24,576-tick and CPU intervals, which
run with load state Ready. The saved-Work memory audit calls `StepLoad` directly,
so its reconstruction path also remains unchanged. The performance binary and
source revision above remain the actual measured artifacts; they are not
relabeled as the later guarded build.

## Final MP and native verification

The repeated probe uses guarded source
`7dfcfb53c399d073778e393ff8bb2f1550c34f6b`, inspector SHA256
`41cd7c3272c9ec6569a057ca286a4d46fa27f7cd4592b590f470d493b9ad74ba`,
and the same v12 smoke save, SHA256
`28aa9050c40218160f090469587b1c3af95c750eb5cfaea1184cfcf1b2731525`.
It passes join, connected-peer disable/re-enable and cost-replacement transport
with **zero canonical mismatches and no network error**.

| Paused join / replacement observation | Both peers |
| --- | ---: |
| Join StateTicks | 3,181,095,517 |
| Join current / retained rows | 4,917 / 0 |
| Join saved Work | 1,879,590,756 |
| Re-enabled current / retained rows | 4,917 / 0 |
| Re-enabled Work, new witness rule | 1,931,148,360 |
| End StateTicks | 3,181,096,567 |
| End current / retained rows | 173 / 4,744 |
| End replacement Work | 68,747,264 |
| End publication revision | 2,798 |
| End company deliveries | 102,119 |

The requested minimum is 1,024 transport ticks; distributed pause and polling
produce **1,050 actual ticks**, with peers paused at exactly the same end tick.
The client replays 1,879,590,756 Work in 14,178 slices. Its maximum observed loop
is 27.823 ms; replay changes neither StateTicks nor frame (92). Native map-ready
join time is 239.403 seconds, and the harness observes 241.638 seconds. The
server's preceding preparation takes 355.030 seconds before the client joins.
The native limits stay at max_join_time=500, max_join_time_total=900 seconds,
max_lag_time=500, max_init_time=100 and max_download_time=1000. The harness's
600-second observation bound changes no native timeout.

This interval records 58,899 capacity-missed packet opportunities / 111,447
passenger opportunities and 3,655 shared local-policy computations (27.699 ms).
It exercises failure state as well as the new algorithm's retained replacement.
The input's NewGRF capacity warnings occur on both peers (27,305 lines each).
The result establishes equality for this fixture and interval, not every
NewGRF/network combination.

Evidence: [MP result](measurements/passenger-hyperpath-return-boarding-2026-10-16/mp-final-result.json),
[provenance](measurements/passenger-hyperpath-return-boarding-2026-10-16/mp-guarded-provenance.json),
[replay](measurements/passenger-hyperpath-return-boarding-2026-10-16/mp-final-join-replay.json),
[end counters](measurements/passenger-hyperpath-return-boarding-2026-10-16/mp-final-end-console.txt)
and [native limits](measurements/passenger-hyperpath-return-boarding-2026-10-16/mp-default-limits.txt).

The native CMake project and normal product executable are restored after the
probe. The restored executable SHA256 is
`d8829f22b4fc895931dff24b0754ac47f6710831c19ab84b1cd4c043c39e823b`.
The post-restoration Hyperpath filter passes 27 cases / 10,115 assertions;
the complete guarded suite passed all 721 cases before this probe. See
[final native manifest](measurements/passenger-hyperpath-return-boarding-2026-10-16/final-native-validation.json)
and [post-restoration test log](measurements/passenger-hyperpath-return-boarding-2026-10-16/native-hyperpath-tests.log).

## Reproduction settings

The committed drivers are
[prepare](../utils/passenger_hyperpath_prepare.py),
[run](../utils/passenger_precomputed_real_map.py),
[transport summary](../utils/passenger_hyperpath_report.py),
[saved-Work inspection](../utils/passenger_precomputed_diagnostic_readback.py)
and [MP probe](../utils/passenger_precomputed_join.py). Use the source revision,
working-tree hashes, frozen assets and input hash recorded above. Build the
measurement copy with `passenger_hyperpath_prepare.py`; use `--mp` for the
inspector/MP copy. The generated sources are measurement instrumentation, not
changes to product sources.

For each long `run`, use `--ticks 24576 --timeout 5400 --measurement-wait 14400
--cold-precomputed --precomputed --warm-precomputed`, the same `--input` and
`--lang`, and separate new output directories. Select `--no-hyperpath` for old
and `--hyperpath` for final. Set `OPENTTD_PASSENGER_TRACE=1` and
`OPENTTD_PASSENGER_CHECKPOINT_TICK=4096` for these cases; clear trace/checkpoint
variables for CPU and validation runs. The short CPU cases use 2,048 ticks,
`--mid-rebuild-tick 1024 --finish-rebuild-precomputed`, without tracing. The
reverse-order cases use 1,025 ticks and `--mid-rebuild-tick 1024`, exclude the last
tick, and require matching midpoint snapshots.

The memory audit uses `--hyperpath-census --replay-memory`. The MP probe uses
the saved Hyperpath smoke fixture, port 17582, `--frame-freq 20 --ticks 1024
--observe-timeout 600 --initial-build --wait-server-ready --toggle-precomputed
--replace-precomputed`. Hold the same exclusive host lease during validation;
the individual measurement runner also acquires it before each CPU case.

The [evidence hash manifest](measurements/passenger-hyperpath-return-boarding-2026-10-16/evidence-sha256.json)
covers the compact committed artifacts. Executables, saves and full observer
CSVs stay in the ignored build directories; their hashes are preserved in the
process records. The pre-existing source changes remain uncommitted and
unchanged at their recorded hashes.
