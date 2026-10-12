# Passenger ServiceGraph scaling

Active journeys now capture an OD corridor instead of predicting every train.
Only the untimed structural index survives a simulation event. Stored leg,
reservation and packet/save layouts are unchanged. Fallback and deferred
packets use existing cargo loading/payment actions with destination-aware
untimed routing and the final-OD delivery guard described below.

## Corridor and completeness

The structural index is an undirected station/order-list incidence graph. It
includes every declared station of a live cargo-capable or possibly refittable
service, including pass-through stations, regardless of load flags, timetable
or dispatch state. Definite freight cannot join unrelated passenger corridors.
Conditional branches enlarge the candidate set rather than deleting paths.

An iterative low-link traversal creates a block forest. Blocks meeting at an
order-list vertex are contracted: using an express between two stops of the
same local pattern must remain possible. Only station articulations can separate
blocks. Every service in every block on the origin/destination forest path is
retained, including cyclic alternatives. The current onboard service is retained
as well, even when immediately transferring is preferable.

A discarded block can be entered and left only through the same articulation
station. With unweighted waiting, waiting there preserves every onward
departure and cannot cost more than the excursion. Services spanning either
side were contracted into the same block, so a through ride or a local/express
interchange cannot be accidentally discarded. This is stronger than connected
component filtering; all 80 routes of the national fixture are connected.

Weighted waiting can make riding a circuit preferable to waiting. Such scoped
queries return `Unknown` when services were omitted; callers need an unscoped
snapshot for a completeness certificate. Active execution uses zero waiting
weight. Known implicit StationIDs are included in both static topology and
captured service stops. A bounded implicit service that is disconnected or an
irrelevant articulation excursion can be excluded without poisoning the OD.
A retained implicit service makes the scope Unknown when its prediction cannot
cover the requested horizon. Known implicit orders beyond a proven horizon or
skipped by an unconditional branch do not invalidate complete timing coverage.
Invalid implicit StationIDs and implicit-only wandering have no bounded declared
route: they stay captured and make the scope Unknown even if their currently
known stops appear disconnected. Implicit freight with no capacity/refit
possibility, and unavailable stopped/crashed trains, do not poison a scope.

`GetStatus()` describes global capture. `GetQueryStatus(origin, destination)`
permits complete capture only for the requested OD. `GetCoverage()` additionally
checks timing uncertainty in the destination component and distinguishes
GlobalComplete, ScopedComplete and Incomplete. Legacy station-wide visit and
departure queries remain globally incomplete on pruned snapshots. The planner
uses scoped departure coverage after checking its OD/evaluation contract.
Trip timing uncertainty and query work/label/transfer limits still propagate to
the journey result. Finding a valid destination candidate does not certify
that the alternatives were exhausted. Active execution accepts only complete
results; the dense cyclic fixture deliberately falls back at its query limit.

## Live capture and cache lifetime

Before copying orders/schedules or predicting visits, each train's live consist
is inspected for the requested cargo. Zero capacity without an order/current
refit excludes detailed capture. Possible refits remain candidates, including
a current refit outside the declared pattern, whose future seats are Unknown.
Scoped stopped/crashed trains need no detailed capture. Invalid live progress
and unsupported timing remain Unknown.

The shared index contains IDs and topology only. Order initialization, copy,
insert/delete/destination registration/free, station-removal order edits, pool
reset and afterload invalidate it through the existing service-index hook.
Changing timetable/load/refit flags, dispatch slots, consist capacity, first
shared vehicle, occupancy or progress does not make its topology stale: these
values are read again during cargo/fleet discovery. Before reuse, the canonical
eligible order-list ID vector is compared exactly with the owned cache key; a
changed cargo, refit, stopped/crashed state or shared fleet can replace the view
even without a topology hook. Timing and occupancy are never cached. Creating
a new order list invalidates topology. Replacement releases the old view before
allocating construction scratch; the single-entry cache has bounded residency.

Dynamic snapshots are keyed by cargo/destination within one active context.
`PrepareUnload` owns that context across one consist's staging; waiting routing
owns another context for its station/cargo pass. Position, progress and declared
orders remain stable during each pass. Staging can change observed occupancy,
but active evaluation uses the total-seat upper bound; actual seats are assigned
by the existing loading/reservation code. No dynamic snapshot is shared with
another event, even in the same state tick. Delay, position, timetable/refit and
fleet edits are therefore captured at the next event. Boarding contexts retain
their existing refit invalidation and loading-pass lifetime.

Execution-result caches, including failed searches, are bounded and live only
in their event. The planner also has its ordinary immutable-snapshot OD cache.
Repeated packets use the same deterministically admitted OD result. Cache
eviction/residency cannot replenish the logical admission ledger. Joining or
loading rebuilds the structural index and starts the next simulation tick with
the same allowances as other peers; no new persistent save data was introduced.

## Complexity, deterministic work and memory

Let O be declared orders, V consist parts, E unique station/service incidence
edges, and P/D the selected prediction steps/dispatch-slot inspections. Cold
structural construction uses O(O + E) traversal/storage, with ordered-map and
sorting logarithmic factors. It uses explicit stacks, including on long chains.
A warm corridor query walks its block-forest path and retained block members.
Live classification still costs O(O + V); detailed prediction costs O(P + D)
only for retained potentially capable trains. Prediction and departure indices
retain their existing sorting costs. The RAPTOR query's independent budget and
reverse reachability remain unchanged.

Every scoped build charges the same conservative index allowance
`sum(8 + 16 * order_count)` over eligible services on cold and warm calls, plus
corridor traversal and
capture work. Warm calls avoid this preprocessing physically. Diagnostics show
the avoided allowance separately. No clock, allocator capacity, GUI state or
thread scheduling participates in admission, service selection or quota reset.
Shared fleets have a deterministic 64-tick rotation of their first captured
vehicle. Incomplete captures are rejected, so a budget-limited prefix cannot
silently choose a poorer train based on pool order.

| Allowance | Standard active value |
|---|---:|
| One graph Work | 2,097,152 |
| One graph allocation envelope | 128 MiB |
| Total / per-trip visits | 262,144 / 128 |
| Prediction horizon / per-trip steps | 65,536 ticks / 4,096 |
| Event Work / graph envelopes | 4,194,304 / 256 MiB |
| Event destination entries / searches | 128 / 128 |
| One query Work / labels / scratch | 65,536 / 8,192 / 8 MiB |
| Per-snapshot planner cache | 2 entries / 256 KiB |
| Tick graph Work / query-and-signature Work | 16 Mi / 1 Mi |
| Tick snapshot constructions | 16 |
| OD fallback event / tick Work (independent ledger) | 65,536 / 4 Mi |

Graph and query/signature counters are simulation state, reset at the tick
boundary in `StateGameLoop`, and consumed in simulation order. Afterload resets
derived state before the next tick. A depleted query allowance prevents an
unnecessary new graph. Fallback retries use 64-tick station cohorts on smaller
maps and 1024-tick cohorts above 512 stations, reducing simultaneous competition.
Admission exhaustion is now a deferred budget wait, separately counted from
an exhausted search, unknown prediction or unreachable OD. The packet retains
its OD and CargoDist queue key, uses Legacy mode plus the existing saved retry
station/deadline, and retries from the next tick without the Fallback cooldown
or station cohort delay. Split, merge and current saves preserve this state;
there is no new enum value, field, packet size or save feature. The filtered
OD queue allows destination-aware fallback transport while a timed query waits.

Before the station loading loop changes any countdowns or queues, a census
selects up to four searchable loading origins in a rotating pool-ID window.
An origin needs a running loading vehicle whose countdown is due this tick and
a waiting passenger with a valid OD, eligible mode and elapsed retry deadline.
The same retry/cohort predicate is used by the waiting router. Idle stations,
unknown ODs, existing Journey packets, cooldowns, stopped/crashed vehicles and
advance-loading orders cannot acquire a share merely by having waiting cargo.
Each selected origin gets a protected first waiting snapshot: 2 Mi graph Work,
128 Ki query/signature Work and one snapshot slot. Ordinary waiting searches
and onboard arrivals cannot spend those unused shares. A claim releases unused
headroom, and a completed or skipped LoadUnloadStation waiting pass releases
any unclaimed share immediately, before its onboard searches. No dynamic
snapshot is shared across events.

The census examines stations, loading vehicles, passenger cargo types and
waiting packets until it finds an eligible packet at each due origin. Its
combined traversal is bounded by 65,536 Work, charged to the same 16 Mi graph
ledger. If it cannot finish, it reserves no shares and leaves all remaining
credits for ordinary searches. The transient origin vector stores at most one
StationID per station; only four IDs and claim bits survive until the next tick.
Diagnostics reset cannot rerun the census, change admission or release shares.

For a stable set of N searchable loading origins, a completed census and four
shares, each origin is selected within ceil(N/4) ticks, independent of simulation
visitation order. Progress still requires a complete graph/query within its
ordinary per-search limits. Rapidly changing demand, census overload and multiple
ODs inside one origin have no strict latency bound.
The hard maximum remains 16 new snapshots/queries per tick, and signatures can
reduce that count. This change schedules overload fairly rather than claiming
that thousands of distinct ODs fit in a single tick.

When an onboard network search cannot run within its admission/search limits,
an arrival-local bounded signature and visit prediction validate the saved ride.
Matching train/service/orders and the absolute alighting occurrence preserves
the Journey leg and returns Keep; it does not certify a newly optimized route.
Forced unload/transfer and invalid rides cannot use this continuation. Stage
also protects the known final OD after a fallback or deferral and on later
arrivals: a verified direct ride or committed OD interchange returns Keep
through intermediate CargoDist destinations; only the accepting final OD can
finish delivery. NoUnload remains effective. Disabled/manual routing and unknown
ODs retain legacy behavior.

Untimed fallback prefers a validated direct vehicle prefix to the OD. Otherwise
it selects the first hop of a shortest directed, unrestricted positive-capacity
cargo LinkGraph path to the final destination. With stable topology, completed
interchanges reduce remaining hop count. A vehicle must reach that hop before
returning to its boarding station or encountering an unsupported/forced order.
Load, Reserve and HasCargoFor enforce the same condition, including returned
gradual-loading reservations. When no progressing ride can be verified,
passengers wait in the filtered OD bucket; an Invalid key cannot permit blind
reboarding. Route removal blocks cycling trains, and a restored path is checked
in the next loading pass. Stale historical links cannot guarantee future service
or seats, and unsupported prefixes/work exhaustion do not establish permanent
unreachability. This policy supplies no timed-completeness certificate.

Each arrival/loading pass caches at most 128 hop queries and 128 vehicle/cargo
prefixes, examines at most 4,096 orders per prefix, and shares a 65,536-unit
allowance across traversal/sorting/BFS. The independent tick ledger caps this
work at 4 Mi; it never replenishes or spends timed JourneyPlanner allowance.
BFS uses under 256 KiB of transient NodeID vectors, validates station/node
identity and retains no graph pointers across events. Refit invalidates live
prefixes. Existing saved passenger_via stores the validated through target,
leaving packet size/save schema unchanged. Diagnostics report work and searches.

Traversal no longer spends memory for every Work unit. Admission reserves,
before allocation, 256 bytes per discovered fleet and 128 bytes per temporary
candidate, 512 + 1024 bytes/order per
captured service, 512 bytes/schedule + 64 bytes/slot, and 1024 bytes/trip + 320
bytes/allowed visit. These envelopes cover order extra data, geometric vector
growth, nominal prediction, trip visits, loading/station/departure/component
indices, and scratch maps. Inactive legacy trips do not reserve visit storage.

The cold index scratch envelope is `4096 + sum(1024 + 512 * order_count)` over
eligible services.
Its resident envelope is calculated from deterministic vertex/edge counts,
with 128 bytes per map/vector node and 32 bytes per adjacency entry, including
a conservative selection-set allowance. Index scratch is destroyed before
dynamic capture. Live discovery buffers coexist with both phases, so the peak
is `discovery + max(cold_index_scratch, resident_index + dynamic_capture)`. The same peak is charged when warm. `vector::capacity()` contributes only
to retained allocation diagnostics, never to these limits. Retained estimates
include allocator/map-node estimates, not a measurement of process RSS or the
unobserved transient heap peak.

## Diagnostics

`passenger_journey_stats [total|tick|inventory|reset]` reports active decisions/passengers,
build/search/OD-cache counts, train/cargo/refit/inactive observations, predictions,
visits, omitted trains, truncated predictions, Work, construction/search times,
retained/static/cache/scratch estimates and conservative envelopes. It separates
GraphWork, GraphMemory, SnapshotLimit, QueryWork, QueryLimit, QueryMemory,
PredictionIncomplete, PredictionHorizon, NoJourney, InvalidLeg, StaleLeg,
Disabled and VehicleDeleted fallbacks, admission deferrals per reason, and
fair-share claims/releases/scan Work and validated retained onboard rides.
Observations are summed over builds, not unique fleet inventory. Exact omitted
visit counts cannot be known without doing the omitted prediction work; the
command explicitly reports that those suffix counts are unknown.

`inventory` takes a read-only census of current passenger holdings, reporting
people and packets in Legacy, Journey and Fallback modes and the known-OD
subset of each mode. Global holdings include station-available and vehicle-held
packets once; reserved cargo is already held in vehicle lists. Waiting people
are station-available plus station-reserved cargo. Deferred people are also
reported. The census stops after 16,777,216 station/vehicle/packet inspections
and labels incomplete output as truncated. Rates derived from complete output
are current inventory shares, not probabilities of a search succeeding.
`dump_linkgraph_jobs` includes graph/cargo identities, worker completion flags,
timing and current graph inventories to distinguish successive recalculations.

Times are diagnostic only. `reset` clears observations, never simulation quotas
or caches. Production save hashes are compared after reading/resetting these
diagnostics and after opening/changing station GUI views. Game tick time outside
the measured graph/search functions can be inspected with the existing FPS tools.

## Synthetic comparison

The comparison below is synthetic. A subsequent
[real-map measurement on 12 October 2026](passenger-journey-real-map-2026-10-12.md)
used a 4,726-node passenger LinkGraph and recorded no successful timed Journey
applications after all existing graphs had refreshed. The synthetic comparison
uses 80 hub routes of 100 stations, ten actual primary train instances per route
(80,800 passenger visits), 3,840 freight trains with four pooled coaches each, and 20,000
vehicle objects in total. A separate production-context test creates 8,001 real
Station pool entries and routes 64 destination packets at standard settings.

For the before measurement, `PassengerServiceGraph::Build` from base commit
`6305d4738a27469e19fef37bc32345de6a2cd2cd` was compiled against the same fixture
and planner, adapting only the API and diagnostic instrumentation. Its 128 MiB
envelope still admits only 131,072 Work, exactly the former active effective cap;
it truncates before the former 65,536-visit cap could matter. The supplemental
roomy run lifts memory to 1 GiB and visits to 262,144, permitting a full old
capture. Neither run uses an emulation of the old prediction algorithm.

Ten snapshots and sixteen identical OD queries per snapshot, MSVC x64 Release,
assertions enabled, one illustrative measured run:

| Metric | Before, standard memory | Before, enough capture budget | After, standard scoped |
|---|---:|---:|---:|
| Complete applicable OD results | 0 / 160 | 160 / 160 | 160 / 160 |
| Fallback share of these queries | 100% | 0% | 0% |
| Dynamic builds | 10 | 10 | 10 |
| Whole-network detailed builds attempted | 10 | 10 | 0 |
| Structural index builds / hits | none | none | 1 / 9 |
| Captured physical trips per build | 257 | 4,640 | 10 |
| Visits per build | 25,957 | 88,480 | 1,010 |
| Mean / maximum logical Work | 131,072 / 131,072 | 480,960 / 480,960 | 1,422,144 / 1,422,144 |
| Mean construction time | 13.493 ms | 53.621 ms | 2.151 ms |
| Maximum construction time | 16.332 ms | 56.235 ms | 11.903 ms |
| Cold construction / mean warm construction | 13.079 ms / 13.539 ms | 56.235 ms / 53.331 ms | 11.903 ms / 1.067 ms |
| Combined graph/query time, ten events | 145.746 ms | 602.611 ms | 24.665 ms |
| Retained graph allocation estimate | 6.101 MB | 22.928 MB | 0.238 MB |
| Shared retained index estimate | 0 | 0 | 1.397 MB |
| Conservative peak envelope | 128 MiB | 469.688 MiB | 40.531 MiB |
| Immutable-planner OD cache hits | 150 / 160 | 150 / 160 | 150 / 160 |

Logical Work includes new conservative preprocessing admission, so it is not a
CPU-comparison unit across algorithms. A warm build avoids 1,298,400 of those
charged units physically. The full old capture includes 7,680 useless freight
visits; the new capture reports 3,840 freight exclusions and 790 scope exclusions.
The first index construction remains a roughly 12 ms spike in this sample;
subsequent events reuse structure but reacquire all selected timing inputs.
Dynamic build count is not artificially reduced by unsafe same-tick reuse.
The production-context sample applies all 64 packets, builds/searches once and
hits its execution cache 63 times. The existing 4,096-packet reuse regression
still builds/searches once and consumes no RNG.

A separate admission fixture queues 2,000 different destinations at one origin,
with one direct train per OD. The first tick applies 15 journeys and defers the
remaining 1,985 packets. All 2,000 obtain a plan within 134 ticks, with no route
failure fallbacks and no tick quota exceeded. This fixture holds train positions
fixed and measures admission/retry behavior, not a full game with boarding or
moving trains. A 32-origin fixture puts a busy origin first on every tick and
checks that protected rotating shares still admit the other origins; complete
cold/warm replays through LoadUnloadStation produce identical decisions. Another
loading fixture uses 4,801 stations with waiting cargo but only one due,
searchable origin; its first waiting pass admits 14 queries and claims one share.
Idle stations, unknown ODs, delayed loading, inactive vehicles and cooldowns do
not withhold half the query allowance. Changed demand after the
census releases all four unused shares in the same tick, allowing 15 subsequent
ordinary queries. A 70,000-packet unknown-OD fixture exhausts the bounded census
and still permits 15 ordinary queries, with no reservations.

The explicit benchmark can be rerun with:

```powershell
build-service-graph/Release/openttd_test.exe 'Service graph national comparison report' --reporter compact
```

## Validation and remaining limits

New tests cover freight-first discovery, independent memory/Work, cold/warm
logical equivalence, destination-edit invalidation, wrong-OD/station-global
coverage, freight bridge/refit view invalidation, disconnected/relevant/invalid
implicit stops, complete coverage before a later implicit order, implicit-only
wandering and inactive exclusions,
local/express/local alternatives, future/current refits, conditionals, weighted-wait excursions, 20,000 objects,
80,800 visits, dense connected cycles, 300 transfers, same-tick dynamic changes,
onboard off-corridor transfers, global tick limits, 32 distinct queued ODs,
2,000 distinct destinations, next-tick deferrals and completed retries that
establish a route failure, rotating waiting shares under a hot origin, saved deferred
state and diagnostic-reset purity,
actual loading countdowns/unused-share release, bounded census exhaustion,
onboard budget exhaustion with valid/stale occurrences, forced transfers,
accepted intermediate CargoDist delivery and final-OD payment,
OD-aware fallback interchanges, no intermediate unload/reload, unreachable and
restored destination links, restricted/stale graph identities, stale fallback
reservations and bounded fallback work,
8,001 real stations and actual active execution. Existing suites additionally
exercise high-frequency detours, local-only access, unknown national networks,
shared orders, deletion, departure changes, loading/reservations, save/load,
CargoDist parity with active mode disabled, GUI counts and random-state purity.

Dense blocks may retain most of the network. The connected ring fixture captures
more than 80,800 visits within the 128 MiB graph allowance, but its 65,536 query
Work cannot certify the alternatives and it safely remains inconclusive.
Conditional/refit/untimetabled/contended-dispatch predictions remain conservative;
they are not made artificially complete. Static invalidation still has a cold
rebuild cost, and live fleet classification is not shared between events.
Validation on MSVC x64 Release (assertions enabled): all 352 default unit cases
with 111,868 assertions passed, and the explicit passenger selection passed 157
cases with 120,836 assertions, including hidden national performance fixtures.
The complete CTest run passed all 359 entries (18.62 seconds), including
production legacy/current saves, GUI/diagnostic save-hash purity, disabled-mode
parity and shadow execution. On this Visual Studio build,
`lang` and `baseset` were copied alongside the `Release` executable for the
production runners; their initial missing-data startup failures were resolved.

There was no live multiplayer/server soak test or full-game tick/RSS measurement;
determinism is checked through cold/warm replay, fixed quotas, seed preservation,
load tests and GUI/diagnostic save-state equality. The synthetic numbers do not
claim a national real-map performance result.
