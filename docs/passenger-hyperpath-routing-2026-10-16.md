# Hyperpath passenger routing

Work in progress on `feat/passenger-hyperpath-routing`, based on
`667e1548161d976bbc9f0ef87635572a3d8603de`.

The A-mode checkpoint is commit `76b4e51091`. Subsequent changes connect real
B-mode boarding, local capacity-failure replanning and saved algorithm identity.
The live MP join/toggle/replacement probe and matched untraced CPU measurements
are complete. Long-duration matched transport remains pending.

## Construction

The reverse graph keeps RangeEdge, hot/cold CSR, SoA labels, occurrence
contraction and a monotone radix queue. It processes separate min-node,
strategy-node and Board events. Board events are ordered by post-boarding cost,
including capacity risk, rather than by the departure label alone. Section
edges do not contribute additional frequencies in this mode.

Each OrderList has its own boarding opportunities and aggregates its actual
vehicles with the existing certified/unknown-phase wait estimator. Identical
stop patterns belonging to distinct OrderLists remain distinct, so excluding
the arriving service cannot remove another service's frequency. One departure
node chooses its best downstream alighting; several alighting choices never
create additional frequencies.

There is no K limit. Sparse candidate lists are shared across passengers and
waiting states reuse the base station policy when both membership and cost
coincide. A frozen-input worst-case memory envelope admits the full candidate
sets or returns Memory; allocator residency never changes route admission.

Hyperpath Work charges one unit for each state initialization, seed, heap pop,
incoming descriptor (including its Board-event generation), virtual range
target, copied label, copied candidate, state-set header and destination-row
publication. Heap pushes and acceptance arithmetic belong to the descriptor or
range-target operation that causes them. Phase transitions are charged too.
The active incoming/range offset and publication cursor allow a slice to stop
inside a Board range or candidate list. Loading replays the saved operation
total with the saved algorithm identity; wall-clock timing never publishes a
row or chooses a route. Equal keys use Board, min, strategy order followed by
stable edge/state rank.

## Integer model

Alpha is the single Q48 unit, 2^48. Frequencies are `ceil(alpha / wait)`.
Supported single-service waits are 1 through 2^24 ticks: this bound ensures
`ceil(alpha / frequency) == wait`, preserving the old integer single-service
expected cost exactly. Larger waits explicitly reject this construction mode.
Frequency rounding has relative error less than `wait / 2^48`.

Frozen graph admission proves sum(f) < 2^63 at each station. The 128-bit
numerator is therefore bounded by `2^48 + (2^63-1)*(INT64_MAX-1)`, below
2^127. Candidate acceptance compares the full 128-bit `x*F` with N. Heap keys
are ceil(N/F), and every insertion asserts that its key is at least the
current popped key. MSVC x64 uses `_umul128`/`_udiv128`; other targets use
native unsigned 128-bit arithmetic or the portable split-word implementation.
Published and downstream labels retain integer tick units, rounding expectations
upward; acceptance compares N/F exactly within the Q48 frequency model.

The independent Python oracle uses exact `Fraction` arithmetic and a separate
event algorithm. It reports each candidate's x, frequency and pre-acceptance u.
The C++ diagnostic also evaluates each published candidate using the frozen
forward Ride/Alight recurrence and asserts equality with the reverse departure
label plus boarding risk.

## Frequency-model limitations

For two regularly spaced services, each with headway h and independently
uniform phases, actual expected wait is h/3; this model estimates h/4.
Simultaneous departures can also make combined waiting optimistic because
different real services are treated as independent boarding opportunities.
Known departure phases affect the existing single-service wait estimator,
but the strategy does not use arrival-specific phases or precise interchange
connections. This work does not learn departure gaps or global crowding.

## Validation

The A-mode intermediate run used the existing frozen real-map fixture from
`passenger-precomputed-build-2026-10-16`: source save SHA-256
`306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`.
MSVC x64 Release, assertions enabled, LTO disabled. The measured executable's
SHA-256 was `ae8007137aeb8fd8f7f0e7d4c81b37360ed01a7fdf325d5b5dae452ec9e015e2`.

| Intermediate construction metric | Result |
|---|---:|
| New graph + all destination policies | 99.723 s |
| Logical Work | 1,878,463,918 |
| Heap pushes / pops | 390,155,495 / 390,155,495 |
| Board events / accepted state candidates | 83,672,294 / 137,543,387 |
| Range targets updated | 137,573,308 |
| Frozen memory envelope | 12,876,341,048 bytes |
| Stored base / difference policy payloads | 958,905,448 / 1,485,314,920 bytes |
| Candidate distribution over 70,470,444 waiting states | mean 1.9518, p95 5, max 26 |

The partial array diagnostic is 6,108,687,336 bytes; it excludes graph,
workspace and allocator overhead and must not be interpreted as process peak
memory. The existing 60.3 s baseline was measured in a previous run, so it
does not establish the exact performance regression of this executable.

| OD | Old minimum cost | Strategy expectation | Attractive service(Order), selected alighting station |
|---|---:|---:|---|
| 3222 → 400 | 6136 | 5843 | 543(27)→3221; 300(14)→3221 |
| 971 → 916 | 22862 | 18749 | 310(9)→395; 509(17)→395; 154(9)→395; 508(18)→444 |
| 3393 → 3394 | 5552 | 3832 | 525(13)→3394; 524(13)→3394; 528(33)→3394 |

The [raw intermediate log](measurements/passenger-hyperpath-2026-10-16/intermediate-real.log)
and [integer acceptance reconstruction](measurements/passenger-hyperpath-2026-10-16/intermediate-summary.json)
record candidate x, f, wait and u before acceptance. Every published candidate
passed the forward/reverse cost equality assertion. The independent rational
oracle passed six cases. The existing precomputed regression passed 89 cases
and 564,225 assertions. Additional synthetic tests cover risk-sensitive Board
event ordering, certified/estimated single-service equivalence and arbitrary
Work slicing.

These are construction measurements. Live B-mode boarding, save/load, MP and
long-duration transport outcomes remain separate completion requirements.

The subsequent matched frozen-map run uses one MSVC x64 Release executable
(assertions enabled, LTO disabled, normal client build, static runtime), SHA-256
`b95d42e04b6a36ccbd8dc5a438f070b6099438a6c80f43199261324cd7689638`.
Both algorithms consume the same frozen input in one sequential probe:

| Current frozen-map metric | Previous | Hyperpath |
|---|---:|---:|
| Graph + all rows | 82.073 s | 102.082 s (+24.38%) |
| Logical Work | 5,665,642,229 | 1,878,463,918 |
| Reported array capacities | 4,635,480,774 B | 7,817,735,166 B |
| Canonical admission envelope | 6,596,276,484 B | 16,275,076,536 B |

The new byte counter includes graph, workspace, row/vector capacities and
indices. These counters exclude allocator metadata and are not process peak
memory. Base/difference policy payloads are 1,152,320,560 / 1,788,057,000 B;
candidate distribution and the three focus ODs match the A checkpoint.
All published candidates again pass the forward/reverse x assertion.

Among 23,673,690 mutually reachable OD pairs (including same-station pairs),
23,281,933 improve, 355,550 are equal and 36,207 increase. No OD gains or loses
reachability. Summed expected cost falls from 1,051,915,529,021 to
956,753,854,181 ticks (9.0465%). The largest increase is 2789 ticks. These are
model expectations, not physical travel-time improvements. Raw evidence is in
[the full-OD log](measurements/passenger-hyperpath-2026-10-16/full-od.log) and
[its summary](measurements/passenger-hyperpath-2026-10-16/full-od-summary.json).

## B-mode execution and local replanning

With precomputed routing enabled, `linkgraph.passenger_journey_hyperpath`
defaults to true. Setting it false selects the previous precomputed algorithm
for controlled comparisons; the selected algorithm remains part of the saved
construction identity. Changing the setting replaces tables deterministically.

A waiting HyperpathIntent stores origin, final destination, cargo and a saved
policy generation, plus the existing readiness and expiry fields. It does not
commit to a service. At real loading, membership is checked for the actual
OrderList/occurrence, the shared frozen Ride/Alight evaluator chooses alighting,
and the native service validator checks live orders, facilities and generation.
The existing cargo reservation/load action then binds the actual vehicle.
Partial loads restore the shared intention and failure state to the remainder.
Reservations retain failure state until physical loading. A partially loaded
failed packet splits at the stored/reserved boundary, clearing only the boarded
people; ordinary empty-state packets retain allocation-free reassignment. If
the packet pool cannot supply this split, that partial load is postponed and
station/vehicle reservation counts still use the actual loaded quantity.
Returning a reservation restores a shared policy immediately when its row is
available, preserving failures and interchange readiness.

The station index uses option (b), present-destination/policy-state scans and
binary membership lookup. A shared FIFO policy queue excludes per-packet
planning timestamps from its key. Previous-service and failed-service identities
remain separate policy variants. Failure-expiry extrema select the union of
base/adjusted membership when necessary; each selected packet still receives
its native readiness/expiry/legality validation. An expired representative cannot
hide a valid passenger behind it. There is no candidate-set expansion/reindexing
on publication. Concrete ride evaluation is shared by a loading context.
Existing FIFO merging, round-robin buckets and Legacy/Fallback/section indices
remain. Lookup scales with present destinations and distinct policy states,
not unrelated packet timestamps. The expanded scaling benchmark assigns every
unrelated packet a different planning timestamp. With 128 / 1024 / 8192 / 32768
unrelated packets, warm lookup averaged 1522 / 1296 / 1328 / 1158 ns over 256
queries, inspecting one eligible packet, one group and one native validation.
Cold derived-index reconstruction remains proportional to resident packets:
0.103 / 0.992 / 14.755 / 74.122 ms. There are two destinations in this test;
it does not claim independence from destination or policy-state cardinality.

A full consist records one capacity opportunity when LoadingFinished transitions
from false to true. Read-only predicates and repeated gradual-loading ticks do
not increment failures. Readiness, membership and native legality must pass;
any remaining capacity prevents recording. After three failures of one service,
the local policy excludes it and re-evaluates **all** station boarding
occurrences, including previously rejected alternatives. No alternative restores
the failed service. Results are shared in a bounded derived policy cache; cache
residency never changes the computed result or search admission.

Failure state contains one service identity (OrderList and incarnation), count
and start tick. Its hold is the saved waiting limit clamped to 4096–4194304
ticks. Successful boarding or expiry clears it; another failed service cannot
replace the tracked identity before expiry, including below the threshold.
Splits copy it, and merges require exact equality.
Updating all affected eligible packets at a completed full loading is still
proportional to those affected packets; unrelated station packets are not scanned.

The waiting safety deadline uses `wait + 3 * (2 * wait)`, clamped to
256–4194304 ticks. Here wait is the combined SF residual and twice that value
is the effective headway. Capacity adaptation uses actual failed opportunities,
not repeated deadline expiry.

## Save and deterministic replay

JPSD v12 saves current/retained Hyperpath flags and policy generations.
passenger_periodic v14 identifies set intentions and explicitly serializes
failure identity/count/start tick. Previous ride-kind values are unchanged.
JPSD v1–v11 retain their original graph, queue, costs, Work and publication
sequence; a completed old table can coexist with a new table during replacement.
Old waiting intentions migrate only when a new set intention is available,
under the existing saved station/national generation-sweep allowances. A retained
legacy row does not reset their old expiry while its replacement is building.
The derived migration set depends on saved packet fields, not cache residency;
legacy mode retains its original unresolved-only generation sweep.
The 16 GiB canonical envelope is unchanged for old algorithms. New strategies
use a 32 GiB canonical admission envelope while either current or retained
generation is Hyperpath; this
is a worst-case bound, not a request to allocate that amount of physical memory.
The increased policy-header charge covers vector growth as well as complete
candidate sets. Physical allocation failure stops play rather than selecting a
different route on one peer.

The current MSVC Release run passes all 715 CTest cases, including the native
integration and regression fixtures. The timestamp-diverse lookup benchmark
passes 44192 assertions; the independent Fraction oracle passes seven cases.
Saved partial Hyperpath event
frontiers replay to identical wire state under different Work slices. Current
packet saves retain failure state; older packet versions default it to empty.
The live server/client probe below complements these replay tests.

## Live B-mode and multiplayer checkpoint

The matched transport source save is SHA-256
`6783a500f47435a342a36e9a6544e33bb38f041abe91e9e2ff2c654edd16c978`.
The 512-tick B-mode smoke advances exactly 512 StateTicks and records 512
simulation samples. There is no measurement contamination. It records 41,691
capacity-missed packet opportunities / 72,619 passenger opportunities, with no
local replans yet (the three-failure threshold has not been reached in this
short interval). The last 256 ticks average 59.640 ms simulation and 2.093 ms
Journey time, with tracing enabled. These are smoke results, not a transport
quality conclusion. The ordinary and target-OD observers remain independent.

The actual loopback server/client probe loads that smoke's save, prepares the
initial table while StateTicks stay fixed, joins, disables/re-enables precomputed
routing, then changes transfer cost and transports for 1029 ticks. This run
includes the readiness-index fix `40b434315a`, executable SHA-256
`eda700271cafaad0956aaa36a9fd78e74aed4c2066b83525b87357eb8de41b65`.
It passes:

- Initial join, connected-peer re-enable, and transport have zero mismatched
  packet/payment/database/ownership/loading/row/work/publication/delivery fields.
- Client replay performs 1,879,590,756 Work in 11,788 slices; the largest observed
  loop is 25.792 ms. Replay changes neither StateTicks nor the client frame.
- There are 183 actual progress notifications. Native map-ready elapsed time is
  191.118 s; the harness join observation is 194.036 s. The server's initial
  preparation wait is 322.696 s and releases at frame zero.
- Default network limits remain max_join_time=500, max_join_time_total=900 s,
  max_lag_time=500, max_init_time=100, max_download_time=1000. The harness's 600 s
  observation bound changes no native timeout. Networking source is unchanged.
- During ordinary replacement the peers agree on 174 current / 4743 retained
  destination rows and 67,371,008 Work, including packet failure state and saved
  waiting-event fields in their canonical digest.
- The MP interval exercises actual three-miss adaptation: the server computes
  3693 shared local policies in 19.7206 ms total (cache misses across generations),
  while recording 59,461 capacity-missed packet / 115,715 passenger opportunities.
  These counters reset with the process and exclude the preceding smoke run.

Evidence: [MP result](measurements/passenger-hyperpath-2026-10-16/mp-hyperpath-fixed.json),
[binary provenance](measurements/passenger-hyperpath-2026-10-16/mp-hyperpath-fixed-provenance.json),
[end counters](measurements/passenger-hyperpath-2026-10-16/mp-hyperpath-fixed-end.txt),
[default settings](measurements/passenger-hyperpath-2026-10-16/mp-default-timeouts.txt),
and [512-tick measurements](measurements/passenger-hyperpath-2026-10-16/b-smoke.json).
The input's JP+ Multiple Units NewGRF emits capacity-change warnings on both
peers (27,155 lines each); the probe reports no network error and its explicit
paused snapshots match. This verifies this fixture/run, not all NewGRF/network
combinations. Long-duration quality and packet-fragmentation census remain
pending.

## Matched CPU and memory

Both cases use source `f9407e6041` (product fix `40b434315a`), the same initial
save above, and immutable executable SHA-256
`4d24261d3b37ea9b0b85b18966da1ea19f0e73f13fc02dce3de55f25ff0184be`.
Each advances exactly 2048 ticks with 2048 samples, with tracing disabled and
no detected competing measurement. Initial tables are completed while paused.
At elapsed tick 1024, transfer cost changes from 20 to 17; the next 1024 ticks
use the normal replacement budget and retained table. The replacement is then
completed while paused. Both warm observations succeed in both cases.

All simulation figures below are milliseconds per tick (mean / p95 / p99).
Journey is the existing Journey timer, not all passenger-related game work.

| Window | Previous simulation | Hyperpath simulation | Previous / Hyperpath Journey mean |
|---|---:|---:|---:|
| First 1024 ordinary ticks, including initial intent migration | 28.243 / 34.166 / 41.071 | 33.344 / 54.400 / 76.868 | 1.528 / 2.770 |
| Last 512 of those ordinary ticks | 27.924 / 33.201 / 37.719 | 29.970 / 48.183 / 58.491 | 1.223 / 1.581 |
| 1024 replacement ticks | 31.292 / 47.741 / 55.270 | 37.980 / 60.355 / 74.710 | 3.900 / 9.203 |

The observed Hyperpath ordinary-tail mean is 7.3% higher and replacement mean
21.4% higher; upper-tail latency and initial migration remain costs. It retains
bounded shared lookup/local replanning, but does not match the old mode's CPU
or tail latency on this fixture. The quality run must be considered alongside
these costs. This is one sequential pair, not a statistical claim across maps.
Host frequency, temperature and scheduling were not pinned or logged in this
pair; the observed timing differences include run variance.

| Build / process metric | Previous | Hyperpath |
|---|---:|---:|
| Initial paused warm elapsed | 97.634 s | 127.153 s |
| Remaining replacement completion while paused after 1024 background ticks | 94.583 s | 128.165 s |
| Completed replacement cumulative builder time | 100.371 s | 133.079 s |
| Completed replacement Work | 5,667,026,635 | 1,879,772,949 |
| Peak resident / commit | 6.687 / 6.712 GiB | 9.559 / 10.375 GiB |

The paused replacement figure is its remaining completion time, not total
wall time from the setting change. Process peaks include load, initial build,
transport, retained-table replacement and save. They are distinct from stored
array bytes and the conservative canonical memory envelope. At the completed
cost-17 replacement, Hyperpath reports 7,820,281,490 array bytes, a
16,275,076,536-byte envelope, 1,153,706,120 base-policy bytes and 1,796,819,312
difference-policy bytes. There are 83,672,294 Board events, 137,930,134 accepted
state candidates, 137,961,676 range updates and 390,640,267 pushes/pops.
The frozen full-OD comparison above provides the complete candidate distribution;
this changed-cost replacement has maximum 22.

The Hyperpath interval records 129,226 actual capacity-missed packet /
243,908 passenger opportunities and computes 7044 shared local policies in
42.756 ms total across generations. These are opportunities, not distinct
people. Target validation probes are reported separately from actual capacity
misses. Indexed probes omit unqueried vehicles/packets and can repeat within a
loading; the report keeps raw and deduplicated probe counts.

The midpoint snapshot/command takes 217.940 ms / 72 microseconds in the old case
and 200.340 ms / 57 microseconds in Hyperpath. These sit outside tick samples.
The measured loops need no error-pause resumption; each startup script separately
resumes one pre-existing NewGRF error pause before sampling. Capacity-change
warnings remain (23,489 / 23,657 lines). No source changes or timeout changes
were made to suppress them.

Evidence: [CPU summary](measurements/passenger-hyperpath-2026-10-16/cpu-matched.json),
[old raw samples](measurements/passenger-hyperpath-2026-10-16/cpu-previous-raw.json),
[Hyperpath raw samples](measurements/passenger-hyperpath-2026-10-16/cpu-hyperpath-raw.json),
and [binary provenance](measurements/passenger-hyperpath-2026-10-16/matched-binary-provenance.json).

The measurements use a normal MSVC x64 Release build, static runtime
`MultiThreaded`, `OPTION_DEDICATED=OFF`, `OPTION_USE_ASSERTS=ON` and
`OPTION_NO_LTO=ON`. The measurement preparer copies translation units and
modifies only the build project. Snapshot the resulting executable before
restoring that project with CMake. Two pre-existing local source edits (an index
variable rename and explicit noncapturing-lambda conversion in a test) were
present in both cases, remain outside the feature commits, and have hashes in
the provenance. The independent compiler probe accepts the original lambda
expression too.

The corrected legacy control completes all 24576 ticks with 24576 samples,
successful initial warm and no detected measurement competition. Its complete
canonical end state matches the earlier pre-readiness-fix legacy control, and
the delivered, pending, lost and population CSVs are byte-identical. The
16384-tick canonical checkpoint matches too. This is a behavioural regression
check across the two executables, not an isolated CPU comparison: elapsed
process times are 1209.074 s and 1280.097 s. Evidence:
[canonical comparison](measurements/passenger-hyperpath-2026-10-16/legacy-control-equivalence.json)
and [trace hashes](measurements/passenger-hyperpath-2026-10-16/legacy-trace-equivalence.json).

For reproduction, `utils/passenger_hyperpath_prepare.py` prepares the quality
probe in an already configured MSVC build. `utils/passenger_hyperpath_quality.py`
accepts the immutable executable, asset package, initial save, empty output
directory and matching language directory, then runs the 64-tick smoke and both
24576-tick modes. `utils/passenger_hyperpath_report.py` summarizes its output.
For untraced CPU runs, unset both trace environment variables and use
`utils/passenger_precomputed_real_map.py run` with `--precomputed`,
`--cold-precomputed`, `--warm-precomputed`, `--ticks 2048`,
`--mid-rebuild-tick 1024`, `--finish-rebuild-precomputed` and respectively
`--no-hyperpath` / `--hyperpath`. Use `--timeout 5400` and a sufficient
`--measurement-wait` for this fixture. The normal-host shared measurement lock
serializes builds and games; frozen measurements, CPU and transport are separate
stages. The source save and NewGRF asset package are existing local fixtures and
are not embedded in the result JSON or repository.

## Correctness coverage

The following obligations are covered by the current tests. Long-duration
transport measurements remain pending.

| Requested cases | Evidence in the tests |
|---|---|
| 1, 8, 9 | Single-service certified/estimated equivalence, exact independent LCM rational accumulator (256 cases), nine-member unbounded set and Fraction event oracle |
| 2, 3, 4 | Both local/express arrival orders bind the arriving vehicle; express excluded for a local-only destination |
| 5, 6, 7 | Short-turn rejected as a through service; one alight per departure; no Section frequency in strategies |
| 10, 11 | Separate OrderLists with identical stops, previous-service exclusion, masks/forced unloading, repeated station occurrences and zero dwell |
| 12, 13, 14 | Three actual capacity opportunities, previously rejected alternative admitted, no-alternative failed-service restoration |
| 15, 16 | Every published candidate asserts forward/reverse x equality; every heap insertion asserts monotonicity |
| 17 | Failure state copied by splits, strict merge equality, moved suffix cleared on successful boarding |
| 18, 19 | Partial JPSD event frontiers replay to identical wire bytes; different Work slices preserve results and total Work |

Additional regression cases cover interleaved full services, expired policy-queue
heads, no extra Hyperpath validations for legacy full trains, and bounded legacy
intent migration with warm versus reconstructed derived indices.
Reservation tests distinguish reserve, partial physical load and returned suffix.

The capacity-readiness regression reproduces a derived-index dependency before
commit `40b434315a`: an index retained from before interchange readiness records
zero misses, while reconstruction at readiness records one. Capacity handling
now wakes due entries before its empty-policy fast exit. Both paths record the
same service, incarnation, count and start tick. Legacy full trains still avoid
additional routing/validation calls. Evidence: [counterexample](measurements/passenger-hyperpath-2026-10-16/capacity-readiness-before-fix.log)
and [715-test run](measurements/passenger-hyperpath-2026-10-16/ctest-readiness-fix.log).
