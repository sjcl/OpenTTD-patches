# Event-local passenger service capture

Implemented on `perf/passenger-service-capture`, based on
`fix/passenger-journey-scoped-uncertainty` at
`eab176fa0a539bbe83dddd36648a3fe28a4e63b3`. This change does not include
`feat/passenger-journey-pragmatic-routing` or change service selection,
journey scoring, uncertainty certification, fallback, or save formats.

## Shared and live inputs

`PassengerJourneyActiveContext` owns one `PassengerServiceCapture`. A complete
discovery records the requested cargo, scoped/unscoped mode, eligible OrderList
IDs, their candidate vehicle IDs, and the original discovery work and memory
costs. Subsequent ODs can reuse that discovery without iterating the OrderList
pool or analysing every order's refit flag again.

Selected services share immutable, owned `ServicePattern` values: copied Orders,
trimmed DispatchSchedules, explicit stopping orders and cargo-specific boarding
and alighting flags, reversal waypoints, nominal segments, and cycle/pattern
status. The pattern key includes CargoType, OrderListID and the prediction step
limit. Dispatch slot arrays and Order extra data are copied once. Graph services
hold shared ownership and read-only spans, so destroying the event or changing
the game pools cannot invalidate an already constructed graph.

Capacity and occupancy are still read from the live consist on every graph
build. Position, direction, order progress, loading state, lateness, dispatch
enablement and individual visit timing are captured afresh. `GetPassengerRouteState`
still performs live facility checks, including road/airport/NewGRF conditions.
Future visit predictions are not cached. The existing `SelectPassengerServices`
runs for each OD with the same eligible IDs; unrelated services remain excluded.

## Lifetime and invalidation

The production owner is a stack-local ActiveContext in `BeginLoading` or
`StationCargoList::RoutePassengerJourneys`. Both are synchronous simulation-thread
passes. During arrival staging, packets can change routing metadata or unloading
categories, but vehicles do not move and Orders, consists, refits and dispatch
settings do not advance. Waiting-queue evaluation changes station packet buckets;
it does not run the vehicle simulation or refit a consist. Live occupancy is read
even though these passes preserve vehicle cargo totals.

The capture is destroyed when this pass returns. A later event creates a new
owner even if it has the same state tick. There is no reuse across events,
simulation ticks, save/load or network joins. Tick and existing structural-index
revision checks provide additional guards; that revision is **not** a general
proof of timetable, refit or dispatch freshness. Standalone callers of the optional
Build API must call `Clear()` before starting another event or mutating those
inputs. A changed candidate capacity, missing fleet or stopped scoped vehicle
also rejects discovery reuse. No game-object pointers are retained in the capture.

## Work and memory

Elapsed time and allocation observations are diagnostics only. A cache hit pays
the original discovery, order, schedule and nominal-prediction logical work.
The original candidate/fleet/order/schedule/visit memory admissions still run,
including the structural index's cold scratch peak. If an allowance cannot pay
a complete cached discovery or nominal pattern, the original traversal runs and
produces the same partial result, first failure, Work and memory bound.

Cache storage is optional. Its independent retention ceiling is 16 MiB; a zero
or smaller cache does not change admission or results. Candidate ID records fit
inside the existing discovery envelope. Shared patterns are funded by the same
per-service admissions that previously paid for their private copies. A separate
retention envelope accounts for owned pattern vectors and cache nodes, rather
than treating the larger graph envelope (which includes prediction scratch) as
retained storage. ActiveContext continues charging each graph's original logical
bound against its 256 MiB event allowance, including repeated charges for shared
data. Cache rejection never rejects a graph.

`allocation_bytes` and `capture_allocation_bytes` are sizeof/capacity/node
estimates, with an upper estimate for copied Order extra data. They are not RSS
measurements. Both include referenced shared patterns, so adding the two counters
would double-count those patterns. Their values do not enter simulation budgets.

## Diagnostics and reproduction

`passenger_journey_stats tick` and `total` now report seven construction phases:
Discovery, cargo/consist checks, OD selection and structural index, Orders and
Dispatch copies, nominal stopping pattern, physical trip prediction, and
DepartureIndex. Phase durations accumulate before rounding to microseconds.
The prediction phase also includes live route-state checks and capture-order
visit indexing. Allocation observations after indexing are included in total
build time but not in the seven phase counters.

On a loaded map, run:

```text
passenger_service_capture_benchmark 0 0 100
```

This reads CargoType 0, origin station 0, and the first 100 other stations in
pool order. It alternates the uncached/captured pair order, compares logical
Work, memory, capture status and every physical trip's predicted visits, and
reports phase times and cache hits. All pairs use the same tick and live game
state. It does not move cargo or spend ActiveContext's global tick credits.
Thus 100 independent graph admissions are measured; production event/tick
quotas are not raised to permit 100 successful ActiveContext searches.

The synthetic benchmark is also available:

```text
openttd_test.exe "Event capture benchmark*"
```

## Validation

All 411 CTest entries passed in 19.74 seconds, including production OD/Journey
save/load, diagnostic/GUI save purity, shadow save comparison and the four
AI/GameScript regressions. The explicit existing passenger selection passed
191 cases and 135,563 assertions. The four new capture cases passed 45,953
assertions, including hidden performance sections.

The new coverage includes 100 distinct OD plans with shared local, express,
trunk, shortcut and disconnected services; exact service pattern ownership
reuse; mixed CargoType consists; Scheduled Dispatch, refit and conditional
orders; every Work allowance from 0 through 399; memory allowances in 64-byte
increments through the successful bound; changed step/visit limits; incomplete
timetables; disabled/tiny cache storage; changed progress, occupancy and station
facilities; and new events with changed consists, Orders and Dispatch settings
in the same tick. It compares statuses, budgets, first failure details, stop
permissions, segments, physical trips, Visit times and journey results.
The 100 OD planning case also checks the game's random state.

The real-map command was bracketed by two production saves. Their complete
SHA-256 values matched, and the input save's hash was unchanged. This verifies
read-only benchmark execution on that loaded map; it is not a live multiplayer
join test or a full paired simulation run. No serialized state or CargoPacket,
payment, reservation, loading or unloading implementation was changed.

## Measurements

The paired baseline runs the existing preprocessing steps with `capture=nullptr`
in the same Release executable. It includes the same phase instrumentation and
uses the existing structural index. The captured side includes the first cold
event build. The comparison isolates event capture; it is not a rerun of the
original 472-second multi-tick measurement with a separately built base binary.

Three runs alternate pair order. The host uses MSVC 19.44, x64 Release, assertions
enabled, LTO disabled and the existing static dependency cache. Absolute elapsed
times vary with host load. See the [raw measurements](measurements/passenger-service-capture-2026-10-12/runs.csv)
and [phase measurements](measurements/passenger-service-capture-2026-10-12/phases.csv).

The synthetic fixtures contain 2,500 services and 105,000 Visit occurrences per
graph. The three-order fixture improved only 0.6–2.2% across paired runs. The
twenty-order fixture improved 11.7–12.2%. Both reused 247,500 of 250,000 requested
service patterns (99%), with identical Work and logical memory bounds.

| Synthetic fixture | Median uncached, 100 OD | Median captured, 100 OD | Logical graph peak | Graph estimate | Capture estimate |
|---|---:|---:|---:|---:|---:|
| 3 Orders/service | 2.132404 s | 2.085247 s | 47,236,864 B | 17,309,108 B | 1,925,160 B |
| 20 Orders/service | 3.097794 s | 2.719080 s | 90,756,864 B | 24,528,836 B | 9,265,160 B |

The real input is the existing `verified-result.sav` from the
[12 October real-map run](passenger-journey-real-map-2026-10-12.md), using its
115 local GRF/tar assets and normal NewGRF scanning. The benchmark uses standard
Active graph limits and a 4,096-tick horizon. Maximum Visit count is 178,068.
Final three-run measurements and save hashes are retained with the raw evidence.

| Real map, 100 OD | Uncached | Event capture |
|---|---:|---:|
| Median construction time | 3.488020 s | 3.293095 s |
| Logical Work, each run | 70,407,374 | 70,407,374 |
| Peak logical graph memory | 79,085,888 B | 79,085,888 B |

Paired improvement was 5.4–5.6%. Discovery hit 99 of 100 calls; service patterns
hit 56,584 of 57,227 requests (98.9%). Graph and capture retained estimates were
28,131,418 B and 2,703,532 B respectively, including shared patterns in both
counters. All 300 pairs reported zero mismatches. NewGRF scanning reported no
missing GRFs or removed invalid vehicles in this final run.

| Phase, real-map run 1 | Uncached | Event capture |
|---|---:|---:|
| OrderList/vehicle discovery | 18.925 ms | 0.386 ms |
| Cargo/consist checks | 212.886 ms | 136.340 ms |
| OD selection/static index | 19.123 ms | 16.230 ms |
| Orders/Dispatch capture | 52.480 ms | 17.548 ms |
| Nominal stopping pattern | 96.326 ms | 8.699 ms |
| Physical trip prediction | 1,245.214 ms | 1,274.132 ms |
| DepartureIndex | 1,809.578 ms | 1,800.179 ms |

The first five phases fell from 399.740 to 179.203 ms, a 55.2% reduction.
Prediction and DepartureIndex account for 87.6% of the uncached build time.
Live prediction did not improve and sometimes measured slower with capture;
only repeated discovery and pattern/copy processing was removed.

Both production saves surrounding the three runs have SHA-256
`53956a9d3c983561e268f8254a051d2ce19d968255f48b649b849e01386c89be`.
The input remained
`14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390`.
The [metadata](measurements/passenger-service-capture-2026-10-12/metadata.json)
records compiler, binary hash and limits.

Most remaining time is in live per-trip prediction and DepartureIndex. Their
algorithms and logical costs are unchanged. The smaller improvement for short
order lists is real: shared preprocessing is a small fraction of total work in
that fixture. No conclusion about whole-game throughput or RSS follows from
these graph-only diagnostics.
