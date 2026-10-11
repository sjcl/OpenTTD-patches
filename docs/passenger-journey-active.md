# Active passenger journeys

`linkgraph.passenger_journey_routing` is an experimental, synchronized game
setting, disabled by default. Enable automatic passenger distribution first.
`linkgraph.passenger_od_assignment` is independent: routing only considers
packets that already have an explicit final destination. Only train journeys
are timed; unassigned passengers use CargoDist. Known-OD fallback uses the
untimed, destination-aware policy below.
The console-only shadow diagnostics remain independent and read-only.

## Execution and real capacity

Each packet records Legacy, Journey or Fallback. A Journey stores only its next
ride: physical primary VehicleID, OrderListID, boarding/alighting stations and
order indices, absolute departure/arrival ticks and a signature of the order
and dispatch pattern. Snapshot VisitIDs and complete future itineraries are
never saved. Splits copy execution, OD and cooldown; merges require equal
execution/mode/cooldown as well as the existing payment/source attributes.

`LoadUnloadStation` routes new OD packets before reservations/loading.
Journey and enabled known-OD fallback queues use the Invalid station bucket,
with the previous CargoDist key stored separately. `StationCargoList::Load`,
`Reserve` and `HasCargoFor` filter them against a live boarding context. Matching the next station
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
CargoDist destination/via matches cannot deliver an active Journey. With routing
enabled, Stage enforces the known final OD for Legacy, deferred and Fallback
packets as well, including later arrivals after execution was cleared. A
fallback ride stays aboard through intermediate CargoDist destinations when
the live order prefix still reaches its OD or chosen interchange. An accepting
final OD delivers even if CargoDist has
a stale onward flow; NoUnload still keeps the packet. An unaccepted final OD
keeps passengers rather than deleting them. Disabled/manual routing and unknown
ODs retain the original CargoDist delivery behavior.

An exhausted network-search/admission budget does not cancel a still-valid
onboard ride. A lazy, arrival-local continuation check reuses the boarding
validator's bounded order signature and visit prediction: the physical train,
service, loading order and saved absolute alighting occurrence must still match.
It keeps the original Journey/leg without asserting a new optimum. NoLoad does
not prevent existing passengers from staying; forced unload/transfer still
applies. A failed validation uses the OD-preserving fallback protected above.

## Expiry and fallback

Every loading pass validates waiting Journey packets. Order/timetable/dispatch
changes, refits, NoLoad, stopped/deleted trains, missed/changed occurrences,
removed stations and incomplete searches clear execution and
switch to the filtered OD queue while routing remains enabled. Disabling/manual
distribution restores the CargoDist queue key. Vehicle deletion invalidates
matching legs
before its pool ID can be reused. The final OD survives fallback; the existing
station-removal policy clears an OD when that station itself is removed.

Legacy, deferred and Fallback packets with a known OD prefer a verified direct
ride to their destination. Otherwise a bounded breadth-first search of the live
cargo LinkGraph selects the first hop of a shortest directed path to that OD,
using positive-capacity unrestricted links only. With unchanged links, completed
interchanges strictly reduce remaining hop count. Source-based CargoDist flows
do not select these hops. The current vehicle's executed order prefix must reach
the chosen hop before returning to its boarding station or encountering an
unresolved conditional, implicit stop, refit/depot detour or forced interchange.
Untimetabled deterministic prefixes remain usable, without asserting timing or
JourneyPlanner completeness. An admitted through target is kept until its
interchange and revalidated on every arrival.

When no progressing ride can be verified, onboard passengers transfer to the
filtered OD queue. Load, Reserve, HasCargoFor and gradual-loading reservation
validation apply the same test. An Invalid queue key grants no blanket boarding
permission. Missing/restricted links, unsupported prefixes and exhausted work
leave passengers waiting for a later pass or network repair, without clearing
OD or declaring permanent unreachability. Forced unload/transfer is respected;
fallback packets cannot board vehicles performing that forced interchange at
the current stop, preventing immediate unload/reload there. Stale historical
LinkGraph links can still delay a transfer; this policy does not guarantee that
a future connecting vehicle will operate or have seats. Direct rides and each
boarding are checked against live orders. Other vehicle types can use this
untimed fallback if their loading order prefix can be validated.

Fallback caches belong only to an arrival/loading pass, with at most 128 hop
queries and 128 vehicle/cargo prefixes. Refit invalidates the affected prefix.
They use an independent deterministic 65,536-work event allowance and 4 Mi-work
tick allowance, including prefix sorting and graph scratch initialization. A
prefix examines at most 4,096 orders; BFS scratch is at most two 65,535-element
NodeID vectors (under 256 KiB). No extra packet fields, saved itineraries, RNG,
cross-event route cache or worker-thread mutation are introduced. Diagnostics
report fallback work and path searches separately from timed journey work.

Fallback packets have a 256-state-tick cooldown at the same station. Retries
are staggered by station ID every 64 ticks (1024 ticks above 512 stations), or may
occur at a later station. Admission budget waits use Legacy mode and the existing
saved retry fields, use OD-aware fallback transport, and retry from the next tick;
they are counted separately from route failures and bypass Fallback cohorts.
New Legacy OD packets can be considered immediately before boarding. Packets
waiting beyond their chosen departure can fall back and board legacy services.
Disabling the setting cancels Journey execution and returns only Journey
reservations using the existing bookkeeping. Legacy reservations keep their
order, counts and payment shares. Invalid reserved Journey portions are selected
individually during loading/afterload; loading also returns OD fallback
reservations whose live prefix no longer reaches a progressing hop. Gradual
loading can leave a packet spanning stored and reserved cargo, so only its reserved portion is
split/returned. At the packet pool limit, an unsplittable boundary reservation
is retained and its mode can fall back rather than returning stored passengers.

Vehicle deletion uses a derived VehicleID-to-Journey-packet-ID reverse index.
Split, merge, mode/vehicle changes and packet destruction maintain it; pool
cleanup clears it and afterload reconstructs it before lifecycle validation.
Deleting an unreferenced wagon requires a map lookup, while invalidating a
selected train visits only its referring Journey packets in packet-ID order.
The index is not saved, owns no packet pointers and uses O(Journey packet count)
additional memory; Legacy/Fallback packets consume no entries.

## Save/load and determinism

The separate, non-ignorable `passenger_journey_routing` save extension is version
1. It saves mode, execution, previous CargoDist hop and retry station/time.
Old saves default to Legacy with active routing disabled. After the vehicle,
station and order conversions, invalid IDs/indices/stops and changed order
patterns become Fallback; invalid reservations are returned and queues rekeyed.
Loading does not perform journey searches or consume RNG.

All routing happens on the simulation thread, in existing station/consist/packet
order. There is no wall-time limit, random sampling, GUI-dependent state or
shared shadow quota. Dynamic snapshots and OD results belong to one simulation
event. Only a structural station/service index is shared between events; its
logical preprocessing allowance is identical when cold and warm. Global work
allowances reset at the simulation tick boundary, including after a join/load.

## Work and storage

One arrival context shares one scoped snapshot per requested cargo/destination
across coaches. A shared, untimed station/service index prunes excursions that
cannot improve an OD journey with unweighted waiting. Freight with no current
capacity or possible refit is excluded before detailed prediction. See
[service graph scaling](passenger-service-graph-scaling.md) for the completeness
proof, cache lifecycle, budgets and measured comparison.
One station/cargo pass shares OD results across packets, including failed
searches. A discovery build is lazy: validating an existing leg or cooling down
fallback packets does not rebuild the national graph. There is no per-packet
graph construction. Snapshots are discarded at the end of the event rather
than reusing predictions after fleet/order/timing state changes.

The station loading pass also owns one bounded boarding-context cache keyed by
primary VehicleID and actual CargoType. Waiting validation, HasCargoFor, Reserve
and Load share it across coaches. Context construction is cheap; signatures and
visit prediction are initialized only for a matching Journey packet. Legacy-only
loading and other trains' packets never trigger boarding prediction. Refit
invalidates the consist's entries; the cache is discarded after the pass, so
later ticks/stops cannot reuse old timing state. A two-coach reservation and ten
gradual-load calls exercise exactly one boarding prediction in the regression.

Waiting routing runs only when a station actually has a due loading vehicle.
It validates the available packets but keeps unchanged queue nodes, ring-buffer
storage and order. Changed buckets compact in place in linear time, then append
only packets whose queue keys changed; there is no whole-map reconstruction.
The 4,096 inspection allowance is charged only after mode, OD and cooldown
eligibility checks. Existing Journey packets cannot starve ready Fallback retries.

An event has 4,194,304 work units, at most 128 destination cache entries and
queries, and a 256 MiB snapshot envelope. Each graph is capped at
2,097,152 work units, a 128 MiB conservative allocation envelope, 262,144 visits
and 128 visits per train. Each distinct query has 65,536 work units, 8,192
labels, an 8 MiB scratch envelope and one selected candidate. Graph work is
charged deterministically, with identical static-index allowances when warm;
each distinct query reserves 65,536 units. Signature traversal is charged too.
A state tick permits 16 Mi work units for graphs, 1 Mi for queries/signatures,
and 16 snapshot constructions. Up to four rotating waiting origins have a
protected first snapshot/query share. Before station countdowns change, a bounded
census checks due loading passes and searchable OD packets with the same
retry/cohort predicate as the waiting router. It consumes at most 65,536 units
of the graph ledger. A skipped or completed waiting pass releases any unused
share in the same tick; an unfinished census reserves none. Identical OD packets
reuse the same result.
Waiting-pass searches admit at most 4,096 packets; excess known-OD packets use
the filtered fallback policy. Validation caches at most 128 train/cargo pairs and has a separate
bounded signature allowance. List rekeying/counting remains O(packet count).

No new packets are created by route selection/rekeying. Only the existing
partial-load/reserve actions split packets. The packet pool maximum is unchanged;
adding execution metadata increases each packet's storage, including Legacy
packets. On the Windows x64 Release build, `sizeof(CargoPacket)` is 104 bytes
and the executable next leg is 40 bytes. A 4,096-packet identical-OD test uses
one graph/search and about 65,931 work allowance units; routing creates no
additional packets. A partial 300-person load creates the existing 100/200
split and preserves total feeder share and OD.
An event's graph envelopes total at most 256 MiB, plus at most 4 MiB of planner
result caches and 8 MiB of current query scratch. Actual allocation depends on
the captured network. Large/truncated captures safely
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
CTest suite passed: **331/331**, including the existing game/AI/GS regressions
and shadow production test. The active production test loads an old save in a
fresh game process, toggles/saves/reloads the synchronized setting, verifies
the late afterload hook and compares byte-identical saves with the station
window absent, present and initialized in a different grouping/sort/Planned
mode. These runs use the null video driver; they do not exercise mouse clicks
or visually render the window. The build reports the existing Windows CRT
linker warning LNK4098.

The loading performance fixes add regressions for lazy/shared prediction,
refit invalidation, reverse-index split/merge/mode/deletion/pool-ID lifecycle,
unchanged queue storage, retry eligibility after 4,096 existing Journey packets,
mixed Legacy/Journey reservations at a gradual-loading boundary, and selective
afterload validation with unchanged RNG. Existing save extension version 1 and
the CargoPacket structure size are unchanged by these fixes.

OD fallback regressions cover repeated staging at accepting intermediate
stations without unloading/reloading, destination-aware interchanges around a
CargoDist dead end/cycle with real final delivery, loss and restoration of an
onward destination link, restricted/stale LinkGraph nodes, changed reservations,
untimed/conditional prefixes, forced-interchange reloading, and deterministic
tick-budget exhaustion/recovery without RNG use. On the final MSVC x64 Release
build, 352 default unit cases (111,868 assertions), 157 explicitly selected
passenger cases (120,836 assertions) and all 359 CTest entries passed.

The predictor deliberately falls back for unresolved conditional orders,
unknown full-load departure times, refits and competing future dispatch slots.
There is no guaranteed future-seat reservation or multimodal prediction.
Snapshots are shared within an event; a fresh arrival can still require a
national capture. Real-map throughput and a live multiplayer session need
separate validation. GUI count tests do not substitute for visual window QA.
