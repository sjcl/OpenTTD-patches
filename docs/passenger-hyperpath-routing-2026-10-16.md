# Hyperpath passenger routing

Work in progress on `feat/passenger-hyperpath-routing`, based on
`667e1548161d976bbc9f0ef87635572a3d8603de`.

The A-mode checkpoint is commit `76b4e51091`. Subsequent changes connect real
B-mode boarding, local capacity-failure replanning and saved algorithm identity.
The live MP join/toggle/replacement probe passes. Long-duration matched transport
and untraced simulation measurements are still pending.

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

The current MSVC Release run passes all 714 CTest cases, including the native
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
routing, then changes transfer cost and transports for 1071 ticks. It passes:

- Initial join, connected-peer re-enable, and transport have zero mismatched
  packet/payment/database/ownership/loading/row/work/publication/delivery fields.
- Client replay performs 1,879,590,756 Work in 15,085 slices; the largest observed
  loop is 28.911 ms. Replay changes neither StateTicks nor the client frame.
- There are 254 actual progress notifications. Native map-ready elapsed time is
  262.742 s; the harness join observation is 265.166 s. The server's initial
  preparation wait is 339.101 s and releases at frame zero.
- Default network limits remain max_join_time=500, max_join_time_total=900 s,
  max_lag_time=500, max_init_time=100, max_download_time=1000. The harness's 600 s
  observation bound changes no native timeout. Networking source is unchanged.
- During ordinary replacement the peers agree on 181 current / 4736 retained
  destination rows and 70,123,520 Work, including packet failure state and saved
  waiting-event fields in their canonical digest.

Evidence: [MP result](measurements/passenger-hyperpath-2026-10-16/mp-hyperpath.json),
[default settings](measurements/passenger-hyperpath-2026-10-16/mp-default-timeouts.txt),
and [512-tick measurements](measurements/passenger-hyperpath-2026-10-16/b-smoke.json).
The input's JP+ Multiple Units NewGRF emits capacity-change warnings on both
peers (27,337 lines each); the probe reports no network error and its explicit
paused snapshots match. This verifies this fixture/run, not all NewGRF/network
combinations. Long-duration quality, packet-fragmentation census and untraced
simulation comparison remain pending.

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
