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

Intermediate real-map measurements and final B-mode results will be recorded
separately; passing construction tests alone does not establish live boarding,
save/load or multiplayer correctness.
