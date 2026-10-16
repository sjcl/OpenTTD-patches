# Hyperpath passenger routing

Work in progress on `feat/passenger-hyperpath-routing`, based on
`667e1548161d976bbc9f0ef87635572a3d8603de`.

The A-mode checkpoint is commit `76b4e51091`. Subsequent changes connect real
B-mode boarding, local capacity-failure replanning and saved algorithm identity.
Live multiplayer and long-duration transport measurements are still pending.

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

The station index uses option (b), present-destination/cohort scans and binary
membership lookup. It requires no candidate-set expansion/reindexing on policy
publication. Concrete ride evaluation is shared by a loading context. Existing
FIFO merging, round-robin buckets and Legacy/Fallback/section indices remain.
At two waiting destinations, warm lookup measured 1.4–1.7 microseconds with
128–32,768 unrelated packets and inspected one eligible packet/cohort per query.
Cold index construction was measured separately (0.073–24.563 milliseconds).
This establishes independence from packet population with fixed cohorts; it
does not claim constant cost as the number of distinct destinations grows.

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
replace an active exclusion. Splits copy it, and merges require exact equality.
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
The 16 GiB canonical envelope is unchanged for old algorithms. New strategies
use a 32 GiB canonical admission envelope including retained generations; this
is a worst-case bound, not a request to allocate that amount of physical memory.
The increased policy-header charge covers vector growth as well as complete
candidate sets. Physical allocation failure stops play rather than selecting a
different route on one peer.

Tests currently pass: 11 Hyperpath cases / 898 assertions, 89 previous
precomputed cases / 564255 assertions, and all 704 CTest cases after placing
the normal language and regression fixtures. Saved partial Hyperpath event
frontiers replay to identical wire state under different Work slices. Current
packet saves retain failure state; older packet versions default it to empty.
These tests do not substitute for the pending live server/client measurement.
