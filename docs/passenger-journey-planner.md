# Passenger journey planner

`PassengerJourneyPlanner` searches an immutable `PassengerServiceGraph` for
train journeys. This implementation does not change `VehicleCargoList::Stage`,
loading/unloading decisions, freight routing, or savegame structures. Unknown OD
and inconclusive journey searches continue to use the existing CargoDist path
when the planner is eventually connected to boarding.

## API and snapshot ownership

Build a graph on the simulation thread, while orders and vehicles are stable.
Construct a planner referring to that graph, optionally with a capacity provider.
Both objects must outlive the planner. Call `Plan(cargo, origin, destination,
ready, limits, evaluation, passengers)`; the cargo must match the snapshot.
Inputs and all costs use simulation state ticks, independent of GUI time units.

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
by that round become the next round's station frontier; defaults permit four
transfers, meaning five boarding rounds. The search ends at the destination.
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
never a claim that a train can actually be caught. A departure is skipped if its
suffix cannot reach the destination with the remaining boardings. Intermediate
labels are retained only at stations that can still complete the journey.

Incomplete predictions require a second conservative reverse search, seeded at
declared stations of those services even when their dynamic branch captured no
visit there. Such suffixes remain possible, and an origin connected to them
retains incomplete status. Prediction uncertainty cannot become certified
unreachability merely because destination pruning removed a captured prefix.

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

Distinct physical train paths can still exceed the explicit label allowance.
In that case deterministic cost ordering keeps the best labels and the result
reports `LimitReached`. This bounded search is not an unbounded enumeration of
every Pareto path. Defaults return up to eight final candidates, retain up to
16 labels per station/round, and allow 1,048,576 work units and 65,536 stored
labels, further constrained by a 128 MiB conservative scratch allowance. The
public limits are independently adjustable in tests. Defensive hard caps are
eight transfers, 64 final candidates, 256 labels per station, 2^20 stored labels
and 2^24 work units.

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

Reverse queries use the existing station occurrence index. They allocate one
byte per occurrence only for connected trips, plus bounded station/queue and
trip-map storage. Each of the two reverse searches costs
O((P_c + S_c) log(S_c + T_c)), including byte-array initialization, for P_c visits
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
count total/reverse work, expanded/pruned boarding candidates, pruned frontiers,
visited occurrences, stored labels, allocated label slots, capacity rejections
and a conservative peak allocated scratch estimate. Back pointers avoid copying
whole paths into every label; final reconstruction is bounded by candidates * R.
Labels start with at most 32 slots and grow by doubling, clipped to the remaining
memory/record cap. Short cache misses therefore do not reserve tens of thousands
of labels. Frontier/map bookkeeping and transient vector growth are covered by
`2 * sizeof(internal label) + 256` per allocated slot, plus reverse-search
storage. The diagnostic reports this estimate for actual allocated slots,
rather than reporting the entire configured maximum as a reservation.

The planner cache belongs to one graph instance, with defaults of 128 entries
and an 8 MiB retained-payload allowance. Keys include OD, cargo, ready departure
event, passenger count, all search/evaluation parameters and capacity-provider
revision. The provider must increment its revision when forecasts change.
Cached partial results use exactly the same allowances as misses.

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
`BudgetExhausted`, preserving graph and query uncertainty. Valid prefix journeys
can be returned with incomplete status. `IsUnreachable()` is true only for an
empty complete search, within the snapshot horizon and configured transfer range.
Prediction suffix status is also retained on individual legs/journeys; reaching
a destination before an uncertain suffix does not make the suffix known.

The next stage should construct/reuse a snapshot and planner for a simulation
epoch, group packets by OD and readiness event, and select a time-valid candidate
before separately verifying live loading rules and available capacity. It should
reserve seats by physical vehicle and segment, update the capacity-provider
revision, and replan after missed/full departures. OD-unknown packets and
inconclusive results should keep the CargoDist fallback. Never persist these
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

The 8,000-destination OD generation fixture produced 400 batches of 512 people
with zero unknown destinations. Total generation time was approximately
74-106 ms across these runs, or 0.19-0.27 ms per batch. Tests additionally cover
256 equal destinations receiving exactly two people each, the 4,096 packet
cap with fair overflow rotation, one remaining pool slot, retained merge
behavior, maximum Count, unchanged cargo totals/ratios and persisted cursor/
rounding state. The graph, routing and CargoDist regression suites remain green.
