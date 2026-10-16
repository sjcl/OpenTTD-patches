# Hyperpath passenger routing

Work in progress on `feat/passenger-hyperpath-routing`, based on
`667e1548161d976bbc9f0ef87635572a3d8603de`.

The first checkpoint is an explicitly selected construction/diagnostic mode.
It does not change live passenger intentions or the saved algorithm identity.
Completion requires the B-mode boarding, local capacity-failure replanning,
savegame/MP compatibility and transport measurements described in the request.

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
