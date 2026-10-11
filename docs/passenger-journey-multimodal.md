# Multimodal passenger journeys

Implemented on `feat/passenger-journey-multimodal`, based on
`feat/passenger-journey-active` at `6305d4738a`.

All four primary company vehicle types now participate in one cargo-specific
`PassengerServiceGraph` and one `PassengerJourneyPlanner`. Eligibility uses the
actual passenger-class cargo compartments and their current capacity. Effects,
aircraft shadows/rotors, wagons and articulated parts are not separate trips;
their passenger compartments contribute to their primary vehicle's capacity.
Freight-only and zero-passenger-capacity vehicles have no passenger trips.

The synchronized Active setting remains disabled by default. Automatic passenger
distribution and an explicit `passenger_destination` are still required. The
independent timetable-transfer setting and read-only Shadow observer also accept
all four modes. Different StationIDs receive no walking connection.

## Phase 1: common prediction and physical-stop adapters

`GetPassengerRouteState` validates the live service, order progress and physical
loading state. A bounded, owned mask records which declared stopping orders have
usable facilities. `PredictPassengerVisits` retains its existing order/timetable
and dispatch traversal, including certified prefixes before an uncertain suffix.
An incompatible facility ends the certified prefix because the actual controller
may skip that order; it never supplies a guessed time for the skipped journey.

| Mode | Implemented checks and interpretation |
| --- | --- |
| Train | Primary consist, passenger compartments and train station facility. Existing platform, reversal, refit and loading rules in the train/economy path remain authoritative. |
| Road | Actual cargo class of the front determines bus/truck facilities, so a freight front with an articulated passenger compartment can use truck stops. Iterate all stops in the same StationID; check road types, infrastructure access and order direction. Articulated vehicles require drive-through facilities. Current loading requires a bay-stop state or an entered drive-through state at the actual compatible stop; driving through alone is not loading. |
| Ship | Dock facility and a physical docking tile belonging to the current StationID. Sailing remains the current travel order. Buoys/waypoints contribute timetable traversal, never passenger boarding/alighting. Repeated port orders retain distinct occurrences. |
| Aircraft | Airport exists, is open and supports the aircraft/helicopter type. Only terminal/helipad loading at the target airport counts as boarding. Landing, holding and taxi movement belong to travel until the controller begins loading; hangar/depot loading is rejected. Range errors are Unknown. Passenger and mail compartments are counted separately. |

Ownership uses the existing infrastructure-sharing rules. StationID integration
permits a transfer between facilities at the same station, but does not permit a
vehicle to use a facility belonging to another mode.

The former Train restrictions were classified as follows:

| Location | Replacement / retained vehicle-specific responsibility |
| --- | --- |
| ServiceGraph Build | Common primary-service eligibility, actual cargo capacity and live adapter; all trips use the existing VisitID/index contract. |
| Boarding Initialize / CanBoard | Common physical VehicleID and order-occurrence checks, with adapter validation of a real loading stop. |
| Active Decide | Common keep/transfer/deliver policy; the existing cargo actions perform movement and payment. |
| Station RoutePassengerJourneys | Common endpoint facilities, capacity, order and signature revalidation, using bounded event caches. |
| Shadow context | Observe all primary modes without changing cargo, RNG or synchronized state. |
| Vehicle destructor | Invalidate referring journeys for every company vehicle before ID reuse. No derived-type access is performed from the base destructor. |
| PassengerRoutingContext IsEnabled | Common timetable-transfer eligibility, with the same live adapters for alternatives. |
| Economy / vehicle controllers | Retain real loading, road-stop entry, docking, terminal, platform, gradual/full load and per-compartment reservation controls. No mode-specific JourneyPlanner is introduced. |

## Phase 2: one network and one search

Trip adds the vehicle type and a diagnostic prediction reason; these are snapshot
values and are not saved. Vehicle type is used by diagnostics and the connection
evaluation boundary, not by separate searches. The round algorithm, reverse
reachability, departure indexes, capacity evaluation, tie breaks and OD cache
remain shared. Capture follows order-list pool order, shared-vehicle chain order
and visit order, without separate per-mode registration passes.

`PassengerJourneyEvaluation::MinimumTransfer(from, to, station)` currently returns
the existing common minimum. It provides a place for future mode-pair evaluation;
the common field remains the lower bound used by departure indexing and pruning.
Selected labels accumulate connection time. Transfers count physical boardings,
with the existing same-vehicle continuation and forced-unloading rules retained.
The integrated transfer-limit expansion handles the tested six-interchange mixed
route. The high-frequency road/aircraft alternative can beat a late direct train.

## Phase 3: actual boarding, reservations and delivery

Every mode uses the existing station loading event, cargo reservation actions,
`VehicleCargoList::Stage`, transfer actions and final CargoPayment delivery. The
boarding context verifies the exact physical vehicle, OrderListID, signature,
station, boarding order, absolute departure and downstream alighting occurrence.
NoLoad/NoUnload, pass-through destinations and forced Unload/Transfer apply to all
modes. A second service sharing the next station cannot board the chosen packet.

Forecast capacity is an upper bound for the requested actual CargoType. It never
guarantees future empty seats. Existing real per-compartment free-space and
reservation bookkeeping assigns seats once. Partial/gradual loading uses the
existing packet splitting; stale reserved suffixes are returned without returning
the stored portion. The new tests check count, destination and total feeder share
through this boundary for each of the four types.

Waiting validation shares endpoint results, signatures and boarding contexts in
the event. The new endpoint caches have at most 4,096 records and charge work for
new records and facility traversal. The existing 128 vehicle/cargo boarding cache,
snapshot sharing, OD cache and per-event search budgets remain in place.

## Phase 4: lifecycle, save compatibility and diagnostics

Deleting any company vehicle visits only its indexed journey packets, in packet
ID order, before the pool can reuse the VehicleID. Invalidation preserves the
final destination, passenger count, feeder share and previous CargoDist hop. The
existing station cooldown uses the saved boarding station, preventing immediate
retry when a different vehicle receives the same pool ID.

The `passenger_journey_routing` save extension remains **version 1**; the
CargoPacket layout and saved fields are unchanged. No complete itinerary,
snapshot, new vehicle-type field or prediction reason is saved. Old saves without
the extension still load as Legacy with Active disabled. Current multimodal
execution, Fallback/cooldown and the previous CargoDist hop use the existing
descriptor. Afterload rebuilds the reverse index and validates all primary types,
actual owning cargo, capacity, facilities, order endpoints and signature; invalid
reservations return through the existing CargoReturn path. Loading performs no
journey searches or RNG draws.

The signature now includes a road order's explicit travel direction. Orders with
no direction constraint keep their previous hash, preserving existing train
execution signatures. Refit, timetable/order changes, removed stops and changed
airport/dock compatibility are rechecked before new boarding.

Shadow statistics include per-mode evaluated/journey/fallback packet counts,
mixed-mode connections in the selected itinerary and snapshot reasons for
prediction uncertainty. Sampling and fixed quotas remain deterministic; measured
wall-clock times are diagnostic output only. Shadow never applies its chosen
journey. The Station GUI uses the existing routing-mode aggregation and does not
classify a supported non-train Journey as Fallback.

## Remaining uncertainty and limits

The predictor deliberately uses Unknown, or an explicit limit/budget status, for:

- Inconsistent real/implicit/timetable order progress, unsupported implicit
  stops, or a current movement/loading order that cannot be aligned safely.
- Missing travel times, overdue travel/airport holding and other timing that the
  existing timetable cannot certify. No distance/speed estimates are introduced.
- Unresolved conditional branches, refits, depot detours and other unsupported
  order effects. Known unconditional branches retain the common traversal rules.
- Full-load/leave constraints whose departure cannot be established, and
  competing future dispatch slots that cannot be assigned safely.
- Removed/incompatible facilities, road types/directions, closed or unsupported
  airports, aircraft range errors, or a purported loading state outside the
  actual stop/terminal/dock. Stopped/crashed vehicles provide no running trip.
- Capture horizon, visit, label, memory, cache or work limits. A truncated graph
  is not a complete inventory and never certifies unreachable destinations.

CargoDist fallback clears execution, retains the final OD and uses the existing
cooldown. Structural reachability deliberately overapproximates declared stops,
including unresolved branches, so an uncertain skipped stop is not reported as a
proven disconnection. A full load can still proceed through the existing cargo
controller while Journey prediction remains Unknown.

Mode-neutral registration does not promise complete capture when the global
budget is exhausted. The mixed-fleet test checks all modes under the full capture
allowance, and a truncated capture explicitly reports its limit. There are no
per-mode priorities or hidden quotas.

## Validation

MSVC 19.44, Windows x64 Release, dedicated build with assertions enabled and LTO
disabled. Both `openttd` and `openttd_test` build successfully. The static dependency
configuration reports the existing CRT linker warning LNK4098.

- Explicit `[passenger]` selection: **143 test cases, 111,728 assertions passed**,
  including hidden passenger performance cases.
- Default unit selection: **340 cases, 105,349 assertions passed**. Its assertion
  total is lower because the default selection excludes hidden performance cases.
- Full CTest: **347/347 passed**, including all registered unit tests and the
  existing four AI/GS regressions (17.43 seconds on the final run).
- Production load/Active/Shadow tests pass, including old-save migration,
  save/reload of the synchronized setting, late afterload, GUI aggregation
  non-interference and byte-identical Shadow-on/off saves.

`src/tests/passenger_multimodal.cpp` adds 16 cases with generated vehicle types
and sections. Fixtures allocate real Train/RoadVehicle/Ship/Aircraft objects,
actual map stops/docks/airports and actual cargo queues/actions; tests do not
substitute a generic Vehicle object for a specialized vehicle layout. Earlier
pure prediction fixtures now create station facilities needed by live validation.

| Requested scenarios | Regression coverage |
| --- | --- |
| 1-3, 21-23, 26 | Each mode plans, rejects the other same-destination vehicle, reserves, gradually loads, stages final delivery and checks company delivered cargo. |
| 4-9, 12 | Both train/road directions, train/aircraft, train/ship, ship/train, road/aircraft/train, all four types and six interchanges; shared StationIDs. |
| 10 | Move the final connection to a different StationID; no walking journey is created. |
| 11 | Frequent road/aircraft trunk wins against an infrequent direct train. |
| 13-14 | Bay/drive-through state, direction, multiple stops, articulated bus/truck facilities and implicit-order fallback. |
| 15 | Buoy traversal contributes time without a boarding entry; repeated ports retain order occurrences. |
| 16 | Aircraft terminal, landing, hangar, holding, helicopter, heliport compatibility, airport closure, range error and separate mail capacity. |
| 17-20 | Common NoLoad/NoUnload/pass-through, full load, refit, stopped, conditional/implicit and changed timing/facility fallback for every mode; earlier common tests cover forced Transfer/Unload and dispatch. Conditional skipping stays inconclusive. |
| 24-25, 30 | Real A-B-C-D-E train/road/aircraft/ship Load/Stage/Unload chain, transfer queues emptied and accepted final company delivery. |
| 27 | Per-mode partial reservation/gradual loading, stale suffix return, seat/count/feeder conservation, plus existing multi-compartment and mixed Legacy/Journey boundary regressions. |
| 28 | Real derived/base vehicle destruction, reverse-index invalidation, same pool ID reuse and cooldown for every mode. |
| 29 | Actual cargo refit, order/timetable and road-direction signature edits invalidate existing execution. |
| 31-32 | Existing rail planner/Active/routing tests and disabled-setting/Legacy reservation regressions remain passing. |
| 33 | Every mode's execution/Fallback/old-save descriptor roundtrip, invalid facility afterload and reverse-index rebuild; fresh-process production save/load tests. |
| 34 | Cold/warm snapshot and OD-cache results, same-time ties, packet index invalidation order, unchanged RNG and production Shadow/GUI save equality. |
| 35 | The mixed national fixture below, existing larger rail fixtures and explicit capture/query/cache/memory-limit regressions. |

A live two-client multiplayer session and interactive GUI rendering were not run.
Determinism is validated through simulation-order, cache/tie/RNG tests and save
comparisons; this is not a substitute for a live networking soak test. Mixed-mode
execution is exercised through production cargo methods with constructed physical
fixtures; the process-level save regressions use the existing bundled save.

## Performance measurement

One Release run of the mixed national fixture, using `steady_clock` only for
reporting elapsed time:

| Metric | Result |
| --- | --- |
| Stations / physical trips | 2,402 / 480 (120 of each mode) |
| Stopping orders | 21 per physical trip |
| Capture horizon | 1,000 state ticks |
| Graph build | 6,734 microseconds |
| Active-style query | 89 microseconds |
| Graph work / query work | 75,480 / 2,262 |
| Query scratch envelope | 114,320 bytes |
| Graph conservative allocation envelope | 77,291,520 bytes (73.7 MiB) |
| Query cache | Identical OD query hits the shared cache |

The query selects one candidate with the Active 65,536-work and 8 MiB scratch
allowances. All 480 trips are captured and the result is Complete. The allocation
envelope is a conservative accounting bound, not measured resident memory.
Numbers describe a constructed network on this machine, not arbitrary real-map
throughput. Many tied alternatives can still reach the explicit label limit;
such results retain their incomplete status and Active safely falls back.

Active retains 2,097,152 work units per event, up to four 524,288-work graph
captures, 128 OD cache/query entries, 65,536 visits per graph, 128 visits per
vehicle and 8,192 query labels. A graph has a 128 MiB allocation envelope and
each query has an 8 MiB scratch envelope. Packets share these snapshots/results;
there is no per-packet national fleet scan. The earlier rail performance fixtures
and their bounded reverse/departure-index searches remain passing.

To reproduce the final checks after configuring the build:

```powershell
cmake --build build-multimodal --config Release --target openttd_test openttd regression_files --parallel 8
Copy-Item -LiteralPath build-multimodal/lang -Destination build-multimodal/Release/lang -Recurse -Force
Copy-Item -LiteralPath build-multimodal/baseset -Destination build-multimodal/Release/baseset -Recurse -Force
& ./build-multimodal/Release/openttd_test.exe '[passenger]'
& ./build-multimodal/Release/openttd_test.exe
ctest --test-dir build-multimodal -C Release --output-on-failure --parallel 4
```

`regression_files` supplies the AI/GS fixture files required by CTest. Multi-config
Windows executables also need the generated language/baseset data at runtime.
