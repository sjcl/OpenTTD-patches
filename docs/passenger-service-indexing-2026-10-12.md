# Passenger service index measurements, 12 October 2026

The contiguous station index reduces construction time and allocation churn while preserving the passenger graph query contract. The base is `fix/passenger-journey-scoped-uncertainty` at `eab176fa0a539bbe83dddd36648a3fe28a4e63b3`; GitHub and local branch tips matched at verification. The implementation is on `perf/passenger-service-indexing`.

## Implementation

- Resolve each declared stop to a dense ordinal in a sorted table of occupied station IDs. A station with declared stops and no predicted visits still has a row. Storage scales with occupied stations and admitted stops/visits, including sparse IDs.
- Count station visits and boardable Timed/Unknown occurrences, compute prefix offsets, and allocate each VisitID array once. Scatter in `(trip, visit)` capture order. VisitID remains an occurrence in the original prediction, independent of its index position.
- Resolve the service once per Trip during counting, and reuse the stop lookup for both station and departure classification. Aggregate each service coverage status over its declared stops; repeated stops merge idempotently.
- Sort each timed station slice by the existing `(departure, VehicleID, trip, visit)` comparator. A global sort adds station-key comparisons and would sort capture-order/Unknown rows unnecessarily, so station slices retain their separate ordering contracts.
- Collect current loading events in one reserved vector, sort by VehicleID, and collapse every duplicate vehicle to an ambiguous optional. Three or more events remain ambiguous.
- Use array parents and byte ranks with path compression for the same undirected declared-stop union. Count uncertainty by dense component, retain only nonempty uncertain-component rows, and scatter stations in ascending StationID order.
- Release occurrence/location and union scratch before later phases. The retained allocation diagnostic now reports contiguous vector capacities.

Capture admission constants, work debits, visit/trip limits, prediction logic, and query work debits are unchanged. Failed captures retain the same capture-order station visits and skip departure/onboard/uncertainty construction. The extra scratch is covered by the existing per-stop envelope and a strengthened per-visit size assertion. The 32 MiB test confirms a real Memory cutoff; the lower retained allocation estimate does not admit additional visits or change the cutoff.

JourneyPlanner implementation, boarding decisions, CargoDist, and save formats are unchanged. Public query signatures are unchanged. `GetDepartures` remains allocation-free, uses two lower bounds for `[from, to)`, and retains its Unknown candidates and status rules. No unordered iteration participates in query order.

## Method

MSVC 19.44.35227.0, Windows x64, Release, assertions enabled, LTO disabled. Both implementations receive identical owned Service/Trip/prediction input. The test retains the map/vector algorithm from the base and converts its output into a reference graph for paired query/Planner checks. Seven repetitions alternate which implementation runs first. Cold runs touch 128 MiB outside timing immediately before construction and before each 100,000-query batch; warm runs have no eviction. This is cache eviction rather than a hardware guarantee that every access misses. No CPU affinity was set.

The `components` fixture contains 8,000 occupied stations, 3,000 Trips, 120,000 Visits and 1,000 independent eight-station components. The `hub` fixture has 3,001 occupied stations, 3,000 Trips and 120,000 Visits; thousands of departures at its central station share one time. Both include repeated circuits, incomplete predictions, Unknown departures, stopped Trips, zero capacity, NoLoad, NoUnload, forced transfer and VehicleID ties. Separate sparse-ID and current-loading-ambiguity fixtures are regression tests.

Headline construction times use `total_untracked`: stage observers and allocation counters are disabled, and the legacy implementation keeps its original interleaving of loading insertion, departure insertion and station sorting. Stage attribution uses an independent pass with allocation counters disabled; the independent legacy loading/sort phases are separated for attribution. Allocation counts/bytes come from separate instrumented passes. Sums of stage medians need not equal the total median, because the passes, cache access order, and observations differ.

The opt-in MSVC test allocator counts C++ `new`/`new[]` calls and `_msize` payload, including vector alignment padding. It is absent from the game executable and disabled in normal test builds. Retained bytes are live payload after construction; peak includes construction scratch. Existing legacy container sentinels allocated before the index measurement are excluded. Heap bookkeeping, process RSS, game-wide peak memory, and production prediction/discovery scratch are outside these measurements.

## Construction and lookup

All times are medians in milliseconds. Query rows are the total for 100,000 mixed present/absent station lookups, not the cost of one query.

| Fixture | Cache | Build before | Build after | Speedup | GetDepartures before | GetDepartures after |
|---|---|---:|---:|---:|---:|---:|
| components | cold | 27.294 | 5.006 | 5.45x | 11.258 | 9.863 |
| components | warm | 27.371 | 3.750 | 7.30x | 11.312 | 9.710 |
| hub | cold | 15.278 | 5.067 | 3.02x | 3.937 | 3.179 |
| hub | warm | 15.004 | 4.512 | 3.33x | 3.921 | 3.103 |

Both lookup implementations allocate zero bytes in these batches.

## Stage attribution: 8,000 stations and 120,000 Visits

These are warm medians from passes without allocation tracking. Counts and cumulative bytes are measured in separate passes over the same fixture.

| Stage | Before ms | After ms | Allocations before | Allocations after | Allocated bytes before | Allocated bytes after |
|---|---:|---:|---:|---:|---:|---:|
| station_visits | 9.9227 | 2.4606 | 72,000 | 6 | 7,808,000 | 3,736,242 |
| station_departures | 10.0696 | 0.3773 | 68,437 | 3 | 6,536,144 | 1,633,797 |
| departure_sort | 2.1797 | 0.9002 | 0 | 0 | 0 | 0 |
| loading_visits | 1.3379 | 0.2422 | 2,927 | 1 | 187,328 | 96,039 |
| components | 5.9048 | 0.0379 | 16,001 | 1 | 640,048 | 8,039 |
| uncertain_components | 0.5092 | 0.0255 | 1,931 | 4 | 47,745 | 207,444 |

The uncertainty phase uses more cumulative scratch bytes than its map predecessor, because it counts all occupied stations in one array. Its scratch is released, and only uncertain components remain resident. This tradeoff is included in the smaller overall peak. Sorting keeps the same comparator; the changed memory layout and phase access order account for the measured timing difference.

## Allocation and memory

Exact payload bytes for the indices, including their construction scratch:

| Fixture | Allocations before / after | Cumulative bytes before / after | Retained bytes before / after | Peak bytes before / after |
|---|---:|---:|---:|---:|
| components | 161,296 / 15 | 15,219,265 / 5,681,561 | 6,143,293 / 4,177,280 | 6,527,353 / 5,370,039 |
| hub | 64,720 / 15 | 13,711,620 / 5,102,350 | 5,017,789 / 3,803,067 | 5,162,307 / 4,930,127 |

The 8,000-station retained index decreases from 5.859 MiB to 3.984 MiB (32.0%); its peak decreases from 6.225 MiB to 5.121 MiB (17.7%). The hub retained index decreases by 24.2%; its peak decreases by 4.5%.

The identical captured input also has owned allocation payload. Adding that shared payload to the retained index gives the following synthetic snapshot totals, excluding object headers and the measurement limitations above:

| Fixture | Common captured payload | Captured values + old indices | Captured values + new indices |
|---|---:|---:|---:|
| components | 9,908,190 | 16,051,483 | 14,085,470 |
| hub | 9,553,487 | 14,571,276 | 13,356,554 |

## Compatibility and regressions

- All occupied station VisitID columns and Timed/Unknown departure columns match the legacy output, including half-open boundary/empty/out-of-horizon queries.
- Capture order, departure ties by VehicleID/Trip/Visit, repeated station visits, NoLoad/NoUnload/forced transfer, current-loading ambiguity, and per-component uncertainty match.
- Paired GetVisits/GetBoardableTrips at budgets 0, 1, 7 and 128 match items, downstream order and status. Planner comparisons at Work 0, 1, 10, 100 and 65,536 match journeys, status, stop reason, work, reverse work, rounds, labels and extensions.
- Live 4,000-vehicle tests cover 32 MiB Memory admission, Work/Trip/Visit cutoffs, cold/warm structural caches, scoped capture and global/scoped coverage.
- Four occupied IDs near StationID 63,000 produce exactly four station rows. Empty snapshots are covered.
- Production Order save tables round-trip timetable and boarding/unloading flags and current orders. After the production afterload invalidation hook, all station visits, departures, uncertainty, coverage and Planner work match before/after in global and scoped captures. Full production save/load, diagnostic/GUI save-hash purity, shadow off/on identical saves, and AI/GameScript tests are also run.

The focused index tests passed **3 cases / 751,760 assertions**. The passenger selection excluding benchmark reports passed **191 cases / 874,734 assertions**, including the focused tests. The same 191 cases / 874,734 assertions passed in the standard test build with allocation instrumentation disabled. The standalone benchmark report passed **1 case / 57 assertions**. The final full CTest run passed **411/411 entries in 24.43 seconds**, including production save/load and AI/GameScript. Production shadow off/on saves had the same SHA-256, recorded in the metadata JSON.

## Reproduction and retained evidence

Configure the normal project with the locally available dependency prefix, then:

```powershell
cmake -S . -B build-service-indexing -G "Visual Studio 17 2022" -A x64 `
  -DOPTION_NO_LTO=ON -DOPTION_USE_ASSERTS=ON -DOPTION_SERVICE_INDEX_ALLOCATIONS=ON
cmake --build build-service-indexing --config Release --target openttd_test openttd regression_files --parallel 8
& ./build-service-indexing/Release/openttd_test.exe "[index]~[.benchmark]" --reporter compact
& ./build-service-indexing/Release/openttd_test.exe "Service index stage allocation comparison report" --reporter compact > benchmark.log
python utils/passenger_service_index_benchmark.py benchmark.log --csv benchmark.csv
ctest --test-dir build-service-indexing -C Release --output-on-failure -j 4
```

On this Visual Studio build, `lang` and `baseset` were copied into `build-service-indexing/Release` before production runners. Allocation measurement requires MSVC and the explicit option; other builds still support sequence/regression checks and timing but report unavailable allocation counters.

- [Raw per-run CSV](measurements/passenger-service-indexing-2026-10-12.csv)
- [Aggregate metrics, input allocation payload and metadata](measurements/passenger-service-indexing-2026-10-12.json)

These are synthetic index comparisons on one host. They do not measure a complete national game tick, real-map RSS, or a multiplayer soak test. The existing ordered Service/Station-service capture maps remain and can still contribute discovery/capture cost.
