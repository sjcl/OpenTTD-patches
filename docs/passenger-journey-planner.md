# Passenger journey planner

`PassengerJourneyPlanner` searches an immutable `PassengerServiceGraph` for
train, road, ship and aircraft journeys in one network. Its read-only
[shadow integration](passenger-journey-shadow.md)
observes `VehicleCargoList::Stage` without changing loading/unloading decisions,
freight routing, or savegame structures. The [Active integration](passenger-journey-active.md)
controls real boarding and unloading. Unknown OD and inconclusive searches use
the existing CargoDist path. See [multimodal support](passenger-journey-multimodal.md)
for vehicle adapters and the latest validation results.

## API and snapshot ownership

Build a graph on the simulation thread, while orders and vehicles are stable.
Construct a planner referring to that graph, optionally with a capacity provider.
Both objects must outlive the planner. Call `Plan(cargo, origin, destination,
ready, limits, evaluation, passengers)`; the cargo must match the snapshot.
Inputs and all costs use simulation state ticks, independent of GUI time units.

`PlanOnboard(cargo, onboard_visit, destination, limits, evaluation, passengers)`
uses the same search with a unique currently loading occurrence as its root.
Continuing that ride requires no new seats or boarding/transfer cost, including
at a NoLoad stop. Its remaining dwell counts as riding. An initial interchange
requires permission to alight, minimum connection time and one transfer; forced
unloading prohibits uninterrupted riding. Find the root with
`FindOnboardVisit(vehicle, station, current_order)` on that same graph. Missing
or ambiguous events return Unknown. The onboard cache key includes both indices.

The result contains ordered `PassengerPlannedJourney` values. Each leg identifies
the physical vehicle, boarding/alighting stations, actual predicted departure
and arrival, and both snapshot `VisitID`s. Journey totals report arrival, waiting,
riding, minimum interchange time, transfers, capacity penalty and generalized
time separately. Waiting includes minimum interchange time; riding includes
intermediate station dwell while staying aboard. Neither preferences nor
capacity penalties change actual timestamps.

`VisitID` is a pair of indices into this particular snapshot, not a savegame ID.
Do not store it on CargoPackets or reuse it after rebuilding the graph. Current
vehicle/order pointers are never consulted during queries.

## Boarding rounds and alternative paths

The initial frontier is the origin at the ready time. A round boards one actual
predicted vehicle occurrence and scans its downstream visits. Labels produced
by that round become the next round's station frontier. The default
`initial_max_transfers = 4` first searches five boarding rounds. If no destination
candidate exists, the stage grows to 8, 16, 32, 64, and so on, bounded by
`max_transfers` (default and internal safety ceiling: 65,535), work, labels,
memory and the immutable prediction horizon. A stage resumes the existing
frontier: labels, back pointers, destination candidates, capacity evaluations,
uncertainty and spent work are never reset. Once a candidate exists, alternatives
are searched through the current stage, without another extension. An exhausted
frontier can finish earlier. Explicit `max_transfers = 0` searches direct rides
only; `initial_max_transfers = 0` with a larger safety ceiling may extend.
The search ends at the destination.
Through riding scans across intermediate stops in the same round and does not
add transfers. A forced unload ends the leg. Reboarding the same physical train
at that exact occurrence is an explicit interchange, subject to the same minimum
interchange time; it is not represented as uninterrupted riding.
If that departure is missed, a later predicted circuit of the same vehicle
remains an actual reboarding candidate.

Before boarding rounds, a reverse search starts from captured alighting visits
at the destination. Station layers count minimum necessary boardings; shared
backward trip scans propagate these distances up to a forced unload or unknown
time. Each trip occurrence is marked once, rather than scanning its entire
suffix separately for every possible alighting stop. Readiness, minimum
interchange and forecasts are relaxed, so these distances are lower bounds,
never a claim that a train can actually be caught. This reverse search computes
all connected layers once under the same budgets, independent of the initial
forward stage; a deep corridor must not be discarded before extension. A
departure is skipped if its suffix cannot reach the destination or an uncertain
service. Intermediate labels are retained at stations that can still complete
the journey, including beyond the current stage. Reverse work can therefore
increase for a destination connected to many distant interchange layers.

Incomplete predictions require a second conservative reverse search, seeded at
declared stations of those services even when their dynamic branch captured no
visit there. Such suffixes remain possible, and an origin connected to them
retains incomplete status. Prediction uncertainty cannot become certified
unreachability merely because destination pruning removed a captured prefix.
Uncertainty seeds are restricted to the destination's undirected component of
declared service stops. This deliberately includes unresolved conditional
branches and ignores timing/direction/restrictions. Disconnected untimed lines
cannot affect the query; connected uncertainty remains conservative.

Once the requested number of final candidates exists, a frontier is skipped
only when a lower bound on `(score, arrival, boardings)` is strictly worse than
the current last candidate. The bound includes accumulated waiting/capacity
costs, minimum remaining boardings and the earliest captured destination arrival.
Equal bounds continue searching, preserving capacity and deterministic ID
tie-breaks. This avoids needless transfers between synchronized trains after
enough better direct candidates have already arrived, while retaining express
interchanges that can improve the requested results.

There is no 256-station frontier limit. Per-station, per-round labels retain
different vehicle paths, including slower direct trains, faster express
interchanges and longer corridors. Pareto pruning applies within the same
sequence of vehicles and boarding/alighting stations, using arrival, waiting,
generalized cost, capacity penalty, prediction quality, capacity uncertainty
and minimum forecast free seats. It never lets the fastest arrival alone erase
all other corridors. Different transfer rounds also preserve fewer-transfer
alternatives. Same-path occurrences with incomparable capacity forecasts survive.
With unweighted waiting, returning to a previous interchange is excluded: waiting
there can catch the same actual onward departure with no worse preference cost.
With a positive waiting weight, a positive-time circuit can reduce the waiting
penalty and remains eligible; zero-time return cycles are still excluded. Each
ancestor comparison consumes work. Through rides across repeated stations and
forced unloading followed by a later real train occurrence remain supported.

Distinct physical train paths can still exceed the explicit label allowance.
In that case deterministic cost ordering keeps the best labels and the result
reports `LimitReached`. This bounded search is not an unbounded enumeration of
every Pareto path. Defaults return up to eight final candidates, retain up to
16 labels per station/round, and allow 1,048,576 work units and 65,536 stored
labels, further constrained by a 128 MiB conservative scratch allowance. The
public limits are independently adjustable in tests. Defensive hard caps are
65,535 transfers, 64 final candidates, 256 labels per station, 2^20 stored labels
and 2^24 work units.
Boarding rounds, reverse distances and public journey transfer counts use
`uint32_t`; the safety ceiling is applied before incrementing or doubling stages.
The initial stage was appended to the limits struct to preserve existing positional
limit arguments. There are no production consumers or persisted journey fields.

The predictor remains `PredictPassengerVisits`; it is not duplicated. NoLoad,
NoUnload and forced unloading come from the captured service stops. Repeated
station stops, circular trips and reversal orders retain occurrence IDs.
Unknown arrival/departure times stop certified travel; unresolved conditions,
refits, full loads and dispatch timing retain the predictor's uncertainty.
Stopped/crashed trains do not become services. Scheduled dispatch uses its
actual predicted departure slots. No headway produces fictional departures.

## Departure index, cost and cache

The snapshot now owns an additional station departure index. It contains known
boardable departures sorted by `(departure, VehicleID, trip, visit)` and a
separate untimed occurrence list. Existing capture-order queries and VisitIDs
retain their ordering and meaning. Station prediction coverage is summarized
once at construction, including services whose dynamic branches produced no
captured visit at that station. This is deliberately conservative over the full
snapshot horizon. `GetDepartures` returns spans without allocating or scanning
the fleet: O(log S + log P_s) lookup for S indexed stations and P_s station visits.

Index construction adds O(P log P) worst-case sorting and O(P + S) storage to
the bounded graph build, for P captured visits. Each VisitID is two size_t values;
on x64 the additional occurrence payload is about 16 bytes per captured
boardable visit, plus station containers and vector capacity. Sorting is snapshot
postprocessing, bounded by the graph's total visit limit; it does not consume a
journey query's allowance.

Reverse queries use the existing station occurrence index. They allocate four
bytes per occurrence only for connected trips, plus bounded station/queue and
trip-map storage. Each of the two reverse searches costs
O((P_c + S_c) log(S_c + T_c)), including distance-array initialization, for P_c visits
in connected trips, S_c reached stations and T_c reached trips. Their work and
memory count against the same query allowances as forward routing. Complete
snapshots require no uncertainty search and no scan of unrelated trip arrays.

Forward query work is proportional to the indexed boarding candidates and their
relevant downstream visits reached from retained labels, plus label comparisons.
If Q station labels inspect B boardings and expand V downstream visits, L is
the per-station label limit and R the maximum boarding count, search work is
O(Q (log S + log P_s) + B + V (log S + L R)), including frontier-map insertion
and label comparison. Bounded final candidate sorting/reconstruction add
O(K log K + C R), for K retained destination labels and C returned journeys.
The forward variant can scan a trip suffix from several boarding
occurrences, rather than guaranteeing one route scan per round. Explicit work
limits bound dense-network worst cases and report incomplete results. Diagnostics
count entered rounds, stage extensions, total/reverse work, expanded/pruned boarding candidates, pruned frontiers,
visited occurrences, stored labels, allocated label slots, capacity rejections
and cycle rejections, and a conservative peak storage allowance. Back pointers avoid copying
whole paths into every label; final reconstruction is bounded by candidates * R.
Final path reconstruction work and storage are reserved when a destination label
is admitted. This covers paths sharing a long prefix, whose returned leg arrays
can be much larger than the prefix's stored labels. A failed replacement leaves
the previously admitted candidate available. Storage reservations for discarded
destination alternatives remain charged conservatively for the query.
Labels start with at most 32 slots and grow by doubling, clipped to the remaining
memory/record cap. Short cache misses therefore do not reserve tens of thousands
of labels. Frontier/map bookkeeping and transient vector growth are covered by
`2 * sizeof(internal label) + 256` per allocated slot, plus reverse-search
storage and returned-path reservations. The diagnostic reports this estimate for actual allocated slots,
rather than reporting the entire configured maximum as a reservation.

The planner cache belongs to one graph instance, with defaults of 128 entries
and an 8 MiB retained-payload allowance. Keys include OD, cargo, ready departure
event, passenger count, all search/evaluation parameters and capacity-provider
revision. The provider must increment its revision when forecasts change.
Both initial and safety transfer ceilings are included in the limits key.
Cached partial results retain their stop reason and horizon and use exactly the
same allowances as misses.

Readiness ticks between consecutive origin departures have identical catchable
services. The search uses the next actual departure as its anchor and adds the
caller's initial waiting/elapsed costs back. Readiness after a departure uses
a different key. Fractional additional waiting weights use exact readiness keys
because integer cost rounding could change tie order between ticks. Cache hits,
misses, disabling the cache and bounded eviction do not change returned routes.

## Capacity and generalized time

Graph capture records total capacity, current cargo occupancy, and conservative
current free capacity. Staged unloading is still counted as occupied. These are
observations at capture, not promises of seats at a later visit. A zero-time,
currently loading departure may use the captured free seats as current confirmed
information. Every other visit needs an external forecast. With no provider,
future free seats remain absent and journey capacity is uncertain.

`PassengerJourneyCapacityProvider::Estimate` receives the graph and both visit
IDs, making segment-specific forecasts testable. It reports future total/free
capacity and `Unknown`, `Estimated` or `Confirmed` certainty separately from
the planner-filled current observations. It may also supply actual service
departures, seats per departure, demand and duration for the same observation or
forecast window. Window throughput is departures * seats per departure; total
train capacity alone is never interpreted as line throughput or future free seats.
The provider is pure, and expected to perform bounded O(1) work per estimate.

Insufficient forecast free seats exclude that boarding candidate for the supplied
passenger count; later real trains remain searchable. Estimated rejection also
makes the result uncertain, so it cannot certify infeasibility. No reservation
or assignment is made. Unknown free seats permit a time-valid candidate with
`capacity_uncertain`, not a confirmed available seat.

Candidate ordering first compares generalized time, arrival, boarding count and
waiting. When those tie, a journey with usable confirmed free seats on every
leg precedes an uncertain journey, before vehicle/occurrence ID tie-breaks.
`Confirmed` without a free-seat value stays uncertain. `Estimated` and `Unknown`
have equal priority at this tie-break: neither confirms seats for the passenger
group. Estimates still affect feasibility and configured crowding/throughput
costs. Confirmation never overrides a better time/preference score, and one
confirmed leg does not confirm the entire journey.

Generalized time is elapsed arrival minus ready time, plus transfer count times
the transfer penalty, additional weighted waiting, additional weighted minimum
interchange time, and accumulated capacity costs. Waiting and interchange
weights use integer thousandths. Crowding is occupancy / forecast capacity times
the crowding penalty. Window capacity shortfall is unmet demand / demand times
the shortfall penalty. Arithmetic saturates preference costs instead of
overflowing or changing real times. For a 500-person window, a shortcut serving
1 * 80 people incurs 84% of the shortfall penalty, whereas a trunk serving
20 * 100 people incurs none. The capacity test makes the slower trunk preferred.

## Completeness and future boarding integration

Result status distinguishes `Complete`, `Unknown`, `LimitReached` and
`BudgetExhausted`, preserving graph and query uncertainty. Every returned journey
reaches the requested final destination with validated leg times. `HasJourney()`
reports these usable candidates even if alternatives could not be fully searched.
`stop_reason` separately distinguishes exhausted frontiers, transfer ceilings,
work, label or memory exhaustion, incomplete predictions, insufficient horizon,
and invalid queries. Reaching the initial/current stage with remaining frontiers
reports `TransferLimit` even if a candidate was found: deeper alternatives may
improve it. Reaching a safety ceiling without a candidate is always inconclusive.

For an empty otherwise complete search, a budgeted reverse check of operational
declared stop patterns conservatively distinguishes disconnected OD from a
corridor that may require predictions beyond this snapshot. This check never
generates a timed journey or a synthetic departure. A possible corridor reports
`PredictionHorizon` / `Unknown`; a disconnected origin can report
`Exhausted` / `Complete`. `IsUnreachable()` requires an empty, complete, exhausted
result and applies only within the exclusive `prediction_end` and supplied
conditions. It never certifies permanent disconnection. `IsInconclusive()`
identifies empty uncertain/limited/invalid results for CargoDist fallback.
Prediction suffix status is also retained on individual legs/journeys; reaching
a destination before an uncertain suffix does not make the suffix known.

The next stage should construct/reuse a snapshot and planner for a simulation
epoch, group packets by OD and readiness event, and select a time-valid candidate
before separately verifying live loading rules and available capacity. It should
reserve seats by physical vehicle and segment, update the capacity-provider
revision, and replan after missed/full departures. OD-unknown packets and
inconclusive results should keep the CargoDist fallback. Never clear
`passenger_destination` after a limited search or replace it with an intermediate
station. This change does not connect the planner to loading or modify packets.
Never persist these
snapshot IDs or use current observations as future reservations.

## OD packet limit

Generation now permits 4,096 newly allocated packets per legacy-sized batch.
The existing residual allocator, weighted unknown bucket and saved cursor remain
unchanged. Pool slots are checked before splitting; a reserved remainder packet
preserves passengers if splitting stops. If no unknown bucket is needed and
exactly 4,096 destination packets suffice, the last destination reuses that
remainder. Overflow batches requiring unknown OD can emit 4,095 explicit
destination packets plus one remainder. CargoPacket Count remains uint16_t and
production generation batches retain their existing maximum of 65,535 people.
Freight generation and save format are unchanged.

256 equal destinations and 512 people produce 256 packets of two people, all
assigned. 8,000 equal destinations and 512 people produce 512 assigned packets
without a memory-budget unknown bucket. More than 4,096 simultaneous positive
quotas leave only the overflow in the remainder and rotate subsequent batches.
Pool-shortage tests continue to exercise the old 32-slot scenario explicitly,
rather than assuming the global limit is still 32.

OD allocation remains O(N) scratch/quota work over retained destinations, plus
up to min(positive quotas, available pool slots, 4096) packet allocations/appends.
Worst-case new packet work increases by 128 times over the old cap. In the
256-destination example retained packet count increases eightfold; in an
8,000-destination, 512-person batch the maximum relevant count is 512, not 4096.
Equal-OD packets can merge as before. More distinct packets increase station
storage, save size and later per-packet processing/random-draw granularity.
Diagnostics now include batch count, generated people, peak new packets per
batch, peak retained station packets, peak live pool count and elapsed allocation
time. None of these clocks/counters affect simulation decisions or save data.

## Validation on 2026-10-11

Windows x64 Release builds with assertions enabled passed with GCC 16.1 and
MSVC 19.44. GCC passed all 281 unit cases (59,564 assertions). The MSVC CTest
suite passed 286 cases, including all unit cases, the four AI/GameScript
regressions, and production legacy/current OD save/load migration. The
multi-configuration MSVC build needed language/baseset assets beside the
Release executable for the production runner; these are build resources, not
source changes. Neither the save schema nor existing boarding code changed.

The national fixture keeps the same 8,001 stations, 800 physical trains and
80,800 predicted visits. It now runs entirely with default limits: four transfers,
65,536 labels, 1,048,576 work units and a 128 MiB memory ceiling. The hub query
to station 8,000 returns `Complete`, expanding only the ten trains on its route
and 1,000 downstream visits; 790 unrelated departures are discarded. It consumes
35,692 work units, including 4,901 reverse work units, stores 1,001 labels in
1,024 slots, and reports 850,802 bytes of conservative peak scratch. The previous
unfiltered hub query needed 2,320,801 work units with zero transfers and raised
limits. Initial cold-query timings are about 0.5 ms on this machine.

A second national case adds an actual feeder to the hub (8,002 stations, 801
trains, 80,802 visits). With the same defaults it finds the required interchange
to the final route with `Complete`, consuming 49,203 work units and reporting
851,060 bytes of scratch. Leaf queries also complete under defaults and inspect
fewer than 2,000 downstream visits. Repeated hub/feeder queries hit the cache.
Tests additionally compare bounded best-candidate searches with full candidate
searches for faster expresses, transfer penalties, weighted waits and missed
interchanges. Timings are diagnostics, not test thresholds or simulation inputs.
Dense connected networks can still exhaust explicit work/label/memory limits;
these fixtures do not promise complete enumeration for arbitrary networks.

## Transfer-limit validation on 2026-10-11

The transfer-limit change builds with Windows x64 GCC 16.1, Release and assertions
enabled. `cmake --build build-transfer --target openttd_test -j 8` succeeded;
`build-transfer/openttd_test.exe` passed all 291 unit cases (62,091 assertions),
including the existing JourneyPlanner, PassengerServiceGraph, routing and OD
cases. No loading/staging or save-format source was changed. Ten new cases use
actual order timetables and test the requested depths, stage continuation,
direct-only limits, partial candidate retention, each resource stop reason,
short horizons, disconnected stations, invalid OD, zero-time/circular/reversing
services, useful weighted-wait circuits, wide counters, and capacity/prediction
state carried into later stages. Cache hits and disabled caches agree on routes,
status, stop reason, horizon and diagnostic counts; random seeds remain unchanged.

With one actual train per connection, the default search reports:

| Transfers | Boarding rounds | Stage extensions | Work | Stored labels | Conservative storage allowance (bytes) |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 1 | 0 | 12 | 2 | 27,064 |
| 4 | 5 | 0 | 70 | 6 | 29,720 |
| 5 | 6 | 1 | 87 | 7 | 30,384 |
| 9 | 10 | 2 | 165 | 11 | 33,040 |
| 15 | 16 | 2 | 312 | 17 | 37,024 |
| 33 | 34 | 4 | 969 | 35 | 75,088 |
| 300 | 301 | 7 | 48,762 | 302 | 617,944 |

For every depth, starting with the final wide stage consumes exactly the same
work, reverse work, labels and storage allowance as geometric extension. The
deep chain's quadratic ancestor checks remain inside the default work budget;
very deep/dense queries may still stop inconclusively when budgets run out.

The existing 8,000-station hub query still stores 1,001 labels in 1,024 slots and
expands 10 physical trains / 1,000 downstream visits. Compared with the base
branch's existing executable, work changes from 35,692 to 35,702 and the storage
allowance from 850,802 to 859,432 bytes (+1.0%). The feeder variant still stores
1,002 labels in 1,024 slots and expands 11 trains / 1,001 visits; work changes
from 49,203 to 50,223 (+2.1%) and storage from 851,060 to 863,696 bytes (+1.5%).
Both finish under unchanged default work/label/memory budgets. Added work pays
for cycle checks and final reconstruction; added storage accounts for four-byte
reverse distances and returned-path reservations. Cold fixture times remained
approximately 0.45-0.64 ms across these runs, without timing thresholds.

The 8,000-destination OD generation fixture produced 400 batches of 512 people
with zero unknown destinations. Total generation time was approximately
74-106 ms across these runs, or 0.19-0.27 ms per batch. Tests additionally cover
256 equal destinations receiving exactly two people each, the 4,096 packet
cap with fair overflow rotation, one remaining pool slot, retained merge
behavior, maximum Count, unchanged cargo totals/ratios and persisted cursor/
rounding state. The graph, routing and CargoDist regression suites remain green.
