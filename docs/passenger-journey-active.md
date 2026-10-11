# Active passenger journeys

`linkgraph.passenger_journey_routing` is an experimental, synchronized game
setting, disabled by default. Enable automatic passenger distribution first.
`linkgraph.passenger_od_assignment` is independent: routing only considers
packets that already have an explicit final destination. Only train journeys
are predicted; unassigned passengers and unsupported journeys use CargoDist.
The console-only shadow diagnostics remain independent and read-only.

## Execution and real capacity

Each packet records Legacy, Journey or Fallback. A Journey stores only its next
ride: physical primary VehicleID, OrderListID, boarding/alighting stations and
order indices, absolute departure/arrival ticks and a signature of the order
and dispatch pattern. Snapshot VisitIDs and complete future itineraries are
never saved. Splits copy execution, OD and cooldown; merges require equal
execution/mode/cooldown as well as the existing payment/source attributes.

`LoadUnloadStation` routes new OD packets before reservations/loading.
Journey queues use the Invalid station bucket, with the previous CargoDist key
stored separately. `StationCargoList::Load`, `Reserve` and `HasCargoFor` filter
Journey packets against a live boarding context. Matching the next station
alone cannot board a packet onto a different ordinary/express train.

The boarding context checks primary vehicle, shared orders, current station,
boarding order, order/dispatch signature, NoLoad, a currently loading occurrence
and the predicted absolute departure/alighting occurrence. Repeated visits to
the same order cannot reuse an old boarding. Prediction uncertainty never
creates permission to board. Invalid reserved suffixes are returned through
CargoReturn before loading is attempted again.

Forecast capacity is only the physical train's total-seat upper bound. Queries
use demand 1 so a 300-person packet is not rejected just because only 100 seats
are currently available. The existing per-coach free-capacity calculation,
CargoLoad/CargoReservation actions and CargoPacket::Split assign real seats.
Reserved cargo leaves the station's available list immediately, preventing two
coaches/groups from using the same passengers or seats. Existing gradual/full
load, refit, platform and exclusive-rights checks still apply.

`PrepareUnload` supplies one active context across the consist to
`VehicleCargoList::Stage`. Journey passengers use `PlanOnboard` instead of
CargoDist flow selection: a continuation keeps the passenger's existing seat;
a different first boarding transfers through the existing payment/unloading
actions; an accepted final station delivers through CargoPayment. Intermediate
CargoDist destination/via matches cannot deliver an active Journey. If the
final station does not accept the cargo, the packet falls back and is kept
rather than deleted.

## Expiry and fallback

Every loading pass validates waiting Journey packets. Order/timetable/dispatch
changes, refits, NoLoad, stopped/deleted trains, missed/changed occurrences,
removed stations and incomplete/budget-limited searches clear execution and
restore the CargoDist queue key. Vehicle deletion invalidates matching legs
before its pool ID can be reused. The final OD survives fallback; the existing
station-removal policy clears an OD when that station itself is removed.

Fallback packets have a 256-state-tick cooldown at the same station. Retries
are staggered by station ID every 64 ticks, or may occur at a later station.
New Legacy OD packets can be considered immediately before boarding. Packets
waiting beyond their chosen departure can fall back and board legacy services.
Disabling the setting cancels Journey execution and returns reserved cargo
using the existing bookkeeping.

## Save/load and determinism

The separate, non-ignorable `passenger_journey_routing` save extension is version
1. It saves mode, execution, previous CargoDist hop and retry station/time.
Old saves default to Legacy with active routing disabled. After the vehicle,
station and order conversions, invalid IDs/indices/stops and changed order
patterns become Fallback; invalid reservations are returned and queues rekeyed.
Loading does not perform journey searches or consume RNG.

All routing happens on the simulation thread, in existing station/consist/packet
order. There is no wall-time limit, random sampling, GUI-dependent state or
shared shadow quota. Scratch snapshots and caches belong to one simulation
event and are reconstructed on subsequent events; joining/loading therefore
does not introduce different persistent cache or quota state.

## Work and storage

One arrival context shares one snapshot per requested cargo across coaches.
One station/cargo pass shares OD results across packets, including failed
searches. A discovery build is lazy: validating an existing leg or cooling down
fallback packets does not rebuild the national graph. There is no per-packet
graph construction. Snapshots are discarded at the end of the event rather
than reusing predictions after fleet/order/timing state changes.

An event has 2,097,152 work units, at most 128 destination cache entries and
queries, and at most four graph capture allowances. Each graph is capped at
524,288 work units, a 128 MiB conservative allocation envelope, 65,536 visits
and 128 visits per train. Each distinct query has 65,536 work units, 8,192
labels, an 8 MiB scratch envelope and one selected candidate. Fixed capture and
query allowances are charged independent of cache timing; signature traversal
is bounded and charged too. Identical OD packets reuse the same result.
Waiting-pass searches admit at most 4,096 packets; excess packets remain on
CargoDist. Validation caches at most 128 loading trains and has a separate
bounded signature allowance. List rekeying/counting remains O(packet count).

No new packets are created by route selection/rekeying. Only the existing
partial-load/reserve actions split packets. The packet pool maximum is unchanged;
adding execution metadata increases each packet's storage, including Legacy
packets. On the Windows x64 Release build, `sizeof(CargoPacket)` is 104 bytes
and the executable next leg is 40 bytes. A 4,096-packet identical-OD test uses
one graph/search and 590,144 fixed work allowance units; routing creates no
additional packets. A partial 300-person load creates the existing 100/200
split and preserves total feeder share and OD.
Worst-case event snapshot envelopes total 512 MiB, though actual
allocation depends on the captured network. Large/truncated captures safely
fall back rather than declaring an OD permanently unreachable.

## Station GUI

The existing waiting/planned hierarchy remains intact. Explicit OD packets show
their actual final station in the waiting hierarchy. A separate panel shows,
per passenger cargo, JourneyPlanner/CargoDist/Fallback totals plus available,
reserved and combined waiting totals. Mode totals include the reserved suffixes
on station-loading vehicles exactly once; onboard stored passengers are excluded.
Planned rows still represent CargoDist flow estimates. GUI aggregation performs
no planner calls or cargo mutations. New English strings use the normal missing
translation fallback.

## Validation and limits

The active tests exercise physical train selection, ordinary/express sharing a
next stop, partial boarding, two-coach reservations/gradual loading, stale
reservations, keep/transfer/deliver staging, unsupported/unknown predictions,
cooldown, merge compatibility, independent save-table round trips, invalid
vehicle lifecycle validation, GUI count/RNG purity, setting disablement and
4,096-packet OD reuse/work limits. The transfer test loads the chosen express,
then performs real CargoDelivery at the final station and checks the receiving
company's delivered passenger count. A passenger coach behind an engine with a
different cargo is covered through live boarding and afterload validation.
Existing planner/service/shadow tests cover
national networks, dispatch, repeated occurrences and the integrated gradual
transfer-limit expansion.

MSVC x64 Release builds of `openttd` and `openttd_test` succeeded. The complete
CTest suite passed: **323/323**, including the existing game/AI/GS regressions
and shadow production test. The active production test loads an old save in a
fresh game process, toggles/saves/reloads the synchronized setting, verifies
the late afterload hook and compares byte-identical saves with the station
window absent, present and initialized in a different grouping/sort/Planned
mode. These runs use the null video driver; they do not exercise mouse clicks
or visually render the window. The build reports the existing Windows CRT
linker warning LNK4098.

The predictor deliberately falls back for unresolved conditional orders,
unknown full-load departure times, refits and competing future dispatch slots.
There is no guaranteed future-seat reservation or multimodal prediction.
Snapshots are shared within an event; a fresh arrival can still require a
national capture. Real-map throughput and a live multiplayer session need
separate validation. GUI count tests do not substitute for visual window QA.
