# Periodic passenger journey backend

`linkgraph.passenger_journey_periodic` defaults to true. It selects the persistent
backend when passenger journey routing and automatic passenger distribution are
enabled. Setting it to false retains the snapshot planner as a comparison path.
The standalone service graph, conservative routing and shadow tools remain available.

## Data and routing

`PassengerServiceDatabase` owns one record per pooled OrderListID. A record contains
the saved order-list incarnation, publication generation, explicit stop occurrences,
cargo-specific boarding/alighting/forced-unload masks, one cycle of timetable offsets,
and physical vehicle IDs with independent observed phases and capacity upper bounds.
Station-to-occurrence sets are retained with the database. It does not retain pool
pointers, copy order vectors per OD, expand a future visit horizon, or infer headways
from the number of shared vehicles.

Retention estimates reserve 512 bytes per service plus 512 per stop and twice the
fleet/dispatch element bytes, including distinct station tree nodes and sentinels.

Compilation requires explicit travel flags and station dwell flags. An explicitly
set zero is known; a missing flag remains unknown. Implicit stops and labels are
skipped. Conditional, refit and other unsupported control instructions leave
the service untimed. Full loading and leave-early controls use the declared dwell,
without changing vehicle control. Zero-length cycles are rejected. Next departures
use checked periodic arithmetic; no network-wide least common multiple is calculated.

Physical station occurrences are retained even for NoLoad + NoUnload orders. Their
travel/dwell contributes to the period, phase and dispatch anchor; their cargo masks
exclude them from passenger connectivity and route alternatives. Boarding-only and
alighting-only orders retain their respective direction. Snapshot Stop/Visit vectors
also retain physical occurrences, while station visit, departure and component indexes
contain only exchange occurrences for the queried cargo. Scoped service selection is
cargo-specific. Loading occurrences remain available for onboard continuation.
Revisiting the boarding StationID with NoUnload does not end the same physical ride.
Actual order edits invalidate the derived service indexes even when only cargo
permissions changed; timetable edits keep their existing coalesced publication.

Regular depot orders with PartOfOrders, a fixed destination, no refit/stop/sell action
and explicit travel flags contribute to timed cycles without adding a passenger
stop. A declared depot wait contributes its configured duration. An unset depot wait
means no waiting instruction: VehicleEnterDepot advances the order on entry, and
departure/acceleration is included in the next order's recorded travel. Unconfirmed
raw depot wait values are ignored, matching the controller. Depot dispatch anchors
remain untimed. Service-if-needed, nearest-depot and
unbunching instructions permit live onward reachability but leave the cycle untimed;
neither their possible detour nor release time is guessed. Halt, sell, refit and
automatic/breakdown depot instructions cannot certify through rides. Periodic,
snapshot and live fallback/execution checks share this classification. Missing travel
or station dwell flags still mean unknown time. A waypoint with no dwell instruction remains a passing
event; a declared waypoint wait and travel are included, and it never exchanges cargo.

Phases are captured from a verified explicit current order. Movement, cargo counts
and ordinary lateness do not rebuild the database. An initially unknown phase can
be established by a later real explicit loading event. Capacity/refit changes seen
at a real arrival invalidate the affected service; occupancy changes do not.
An old saved untimed record whose depot cycle is now fully supported is reconsidered
at a real loading event. A bounded flag/duration pass queues its normal coalesced
update; load/join does not rewrite its publication, phase, queue or station cargo.

Scheduled dispatch is timed only for a single physical shared vehicle, one dispatch
anchor, and slots with sufficient time to complete a cycle between departures.
Duration, start tick, existing non-reusable assignment and slot offsets are considered.
Multiple vehicles, ambiguous anchors, overlapping turnaround and unsupported slot
flags remain unknown. A dispatch slot never creates a synthetic vehicle.

OD policies contain bounded alternative **service occurrences**, not a previously
winning absolute departure. They are compiled on demand and reused across packets,
events and ticks. Each evaluation computes current next departures for all retained
alternatives and compares elapsed time, waiting, minimum transfer and transfer cost.
Ties prefer earlier arrival, fewer transfers and an earlier boarding, then stable IDs and occurrences.
Known timed candidates are usable despite unknown alternatives. No candidate is a
permanent disconnection certificate; unsuccessful searches use live OD-aware fallback.

## Execution and events

CargoPacket executions distinguish three kinds:

* `VehicleSnapshot`: the original vehicle-specific absolute-time contract.
* `ServiceIntent`: an unbound service incarnation, boarding/alighting occurrence,
  publication generation, evaluation times and earliest permissible real boarding.
* `BoundService`: an intent attached to the real vehicle when cargo is reserved/loaded.

Live boarding verifies the actual primary pooled vehicle, current station and order,
order-list incarnation, cargo compartments, loading state, facilities, load/unload
restrictions and the downstream alighting occurrence before any forced interchange.
It does not require an actual delayed departure to equal the timetable estimate.
Actual capacity, reservations, gradual loading and payments stay in the existing cargo
actions. Split passengers left at a station retain an unbound intent; returned
reservations become unbound again. Final delivery remains restricted to the final OD.

The actual arrival at an interchange creates the next intent. Its earliest boarding
includes the minimum interchange time. An expired waiting intent is evaluated at a
real station loading event: a safe delayed physical ride can still be used; otherwise
the current OD policy is evaluated again. Live checks are shared inside the loading
event. Valid waiting intents do not initiate another national search on every tick.

Untimed reverse hop distances are shared independently of the timetable database.
Cache keys include cargo, destination, graph topology and edge eligibility revisions.
Exact station/node binding validation prevents reuse after membership changes.
Live downstream order/facility checks remain mandatory. Both OD caches replay their
cold logical work on hits, including resource-stopped traversal prefixes, so optional
residency and eviction cannot change admission or routing decisions.

## Updates, bounds and persistence

The simulation thread spends at most 65,536 build work per normal tick. Dirty order
lists rotate in deterministic ID order; repeated edits cannot starve higher IDs.
Pool discovery advances by saved numeric cursor. Fleets are captured in VehicleID
order. A complete service task publishes atomically. A task exceeding the per-service
ceiling is represented as untimed, rather than blocking the queue indefinitely.

Explicit order mutations, implicit index changes, timetable/autotimetable edits,
dispatch commands, sharing changes, refits, deletion and pool cleanup have hooks.
Unchanged autotimetable values do not invalidate a service. Travel/wait edits coalesce
until the next deterministic 1,024-tick epoch, preserving the safe published timing and
policies in the meantime. Repeated measurements do not reset the first pending epoch.
Order, sharing and dispatch edits remove affected records immediately. Construction
and operating-state changes currently use conservative whole-database invalidation;
unrelated completed services survive local changes. Routine order window notifications,
unchanged auto-refits and clearing unrelated town/landscape tiles preserve the database.
Removed services are unavailable until publication. Whole-database disposal is immediate;
the build is divided across ticks, but disposal is not an incremental garbage collector.

Logical envelopes are 64 MiB for database records and station indexes, 16 MiB for OD
policies (at most 4,096 OD entries), 16 MiB for resident fallback distances, 8 MiB each
for shared station bindings and reverse CSR indexes, and 8 MiB per Active query. Atomic
builder scratch and dirty queues are separately bounded by pool IDs and the work
ceiling. Save/load can temporarily hold the old database, its bounded wire image and
the restored database simultaneously. This temporary overlap is additional to the
64 MiB resident envelope; measured process memory includes game assets and other systems.

The `JPSD` chunk saves compiled records, phases, generations, dirty queue/ages and
discovery progress. Rebuilding them from the current live timetable would lose the
saved publication state. Policies, fallback arrays, allocator capacities and wall-clock
diagnostics are derived and unsaved. Loading restores publication state before ticks
resume. Saved service intents, including those awaiting a dirty service, retain their
state until the normal simulation event; loading does not accelerate their retry.
Saved station cargo queue keys and packet order are also preserved. Reclassifying
queues on load changes the next-hop counts used by station rating and randomized
cargo truncation, and can desynchronize a joining client. Normal routing and
boarding events perform that reclassification on every peer.
Cargo execution extension version 2 carries the explicit kind, generation and boarding
deadline. Version 1 vehicle snapshots keep their original meaning. State tick rebasing
adjusts database phases, packet times and retry deadlines.

## Current limits and validation

Active retains up to eight route families, with fixed candidate, label, work and memory
ceilings. Retention uses nominal travel/transfer ranking and explicit direct alternatives.
It does not prove a global optimum over every possible route or fleet combination.
Partial timed sections of an unsupported cycle are not compiled. Occupancy/window
capacity-provider penalties remain available in the legacy planner; requests for those
unsupported periodic evaluations are explicitly deferred. Active uses capacity upper
bounds and assigns actual seats exclusively during loading.

`[periodic]` tests cover repeated circuits, zero/missing flags, arithmetic boundaries,
different periods, local/express/direct alternatives, shared phases, dispatch slots and
competition, live restrictions, expiry, delayed rides, transfer deadlines, split and
gradual reservations, final delivery in all four modes, local invalidation, rotating
updates, ID reuse, saved pending state, cold/warm work equality, tick rebasing, thousands
of unused ticks, 4,096 same-OD packets and 8,000 stations/4,000 vehicles. Small complete
networks are compared with the original snapshot planner. Production regressions cover
old/current saves, settings, GUI/diagnostic save purity and Journey-disabled behavior.

Performance and multiplayer evidence are recorded separately in
`passenger-journey-periodic-2026-10-12.md` and
`passenger-journey-operational-stops-2026-10-13.md`, with the depot no-wait follow-up in
`passenger-journey-depot-no-wait-2026-10-13.md`. Synthetic tests are not substituted for
real-map transport or live multiplayer measurements.
