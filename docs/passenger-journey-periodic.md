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
skipped. Conditional, depot, refit and other unsupported control instructions leave
the service untimed. Full loading and leave-early controls use the declared dwell,
without changing vehicle control. Zero-length cycles are rejected. Next departures
use checked periodic arithmetic; no network-wide least common multiple is calculated.

Phases are captured from a verified explicit current order. Movement, cargo counts
and ordinary lateness do not rebuild the database. An initially unknown phase can
be established by a later real explicit loading event. A verified arrival whose
phase differs from the published cycle reanchors that service. Waypoints and
non-stopping explicit orders have cycle offsets too. Overdue travel remains unknown;
overdue loading has zero remaining declared dwell, matching AssumeTimetable.
Capacity/refit changes seen
at a real arrival invalidate the affected service; occupancy changes do not.

Scheduled dispatch is timed only for a single physical shared vehicle, one dispatch
anchor, and slots with sufficient time to complete a cycle between departures.
Duration, start tick, existing non-reusable assignment and slot offsets are considered.
Multiple vehicles, ambiguous anchors, overlapping turnaround and unsupported slot
flags remain unknown. A dispatch slot never creates a synthetic vehicle.

OD policies contain admissible reverse time lower bounds over the existing periodic
stop incidence index. They retain neither routes nor previously winning departures.
Settling the origin ends reverse expansion; missing distances use the last settled
frontier bound and do not imply disconnection. Each query searches actual next
departures in a bounded priority queue, comparing elapsed time, waiting, minimum
transfer and transfer cost. Completion is evaluated when popped, never prefilled by
direct lines. The best complete path is retained; the capped candidate counter never
prevents a better or tied transfer prefix from being evaluated. Work, label and memory
truncation is distinguished from absence, with omitted lower bounds controlling
whether the selected result still needs deferral.
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
If preceding tasks leave insufficient facility work at the end of a tick, publication
waits for a fresh tick if its full capture envelope fits a tick. Larger fleets publish only individually verified phases, keeping the others unknown, so they cannot retry indefinitely and stall discovery.

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

`passenger_periodic_compare <cargo> <origin> <destination>` and
`passenger_periodic_compare sample <count>` explicitly compare the old Active
snapshot planner and the periodic planner on the same live state. They do not advance
simulation or alter packets, RNG or the live database. The periodic side uses an
independent saved-state copy, and expensive snapshots exist only inside this diagnostic.
The census distinguishes complete declared timetables from successfully compiled
cycles, and excludes cargo-only fleets from passenger eligibility. OD outcomes are
exclusive call counts; overlapping observations and execution packet/people counts
are labelled separately. See the [coverage report](passenger-periodic-coverage-2026-10-13.md).

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
`passenger-journey-periodic-2026-10-12.md`. Synthetic tests are not substituted for
real-map transport or live multiplayer measurements.
