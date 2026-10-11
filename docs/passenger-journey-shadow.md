# Passenger journey shadow diagnostics

Originally introduced on `feat/passenger-journey-shadow`, based on
`feat/passenger-journey-planner`. The current
[multimodal implementation](passenger-journey-multimodal.md) extends capture
and observation to all four primary passenger vehicle types.

The shadow observes the final CargoDist/timetable decision in `Stage`, before
payment or cargo movement. It does not apply the planner's result. The legacy
decision still evaluates its next via station; the planner evaluates the packet's
final `passenger_destination`. A disagreement is an observation, not proof of a
routing bug.

## Capture and onboard search

`PrepareUnload` creates `PassengerJourneyShadowContext` before inserting the
vehicle into the station's loading list, changing consist flags or creating a
`CargoPayment`. Passenger coaches share its value-owned graph and planner.
NoUnload bypasses Stage; its actual Keep action is observed at this same point.
Stage passes the already selected final Keep/Transfer/Deliver action to the
observer. The observer never repeats flow selection or writes to a packet.

Only passenger-class, known-OD, non-Manual primary-vehicle cargo can be evaluated. Fleet
capture is sampled at the first eligible arrival/cargo in each 64-state-tick
epoch. Discovery examines at most 256 coaches and 512 packets. Other arrivals
are counted as skipped. This sample is deterministic and consumes no RNG.
It can favor frequent early arrivals, so aggregate agreement is not an unbiased
estimate for all passengers on the map.

`PlanOnboard` finds a unique loading occurrence using vehicle ID, current order
and station in the captured graph. An ambiguous or missing event is Unknown.
It reuses the ordinary journey engine, with these root-state differences:

- Continuing the current vehicle needs no new seat, waiting or transfer charge.
  Remaining dwell is riding time. NoLoad does not reject passengers already aboard.
- Another vehicle requires permission to alight, its boarding rules, minimum
  connection time, capacity checks and one transfer. Forced unloading stops
  through riding; NoUnload prevents an initial interchange.
- The cache includes onboard trip/visit indices, OD, count and evaluation limits.
  All coaches at that arrival share it. Destruction discards every graph, cache
  and VisitID. Advancing state ticks invalidates observations in an old context.

The shadow has no future capacity provider. Transfer candidates with unknown
future free seats remain timetable comparisons, not promised seats. Continuing
the occupied vehicle does not require such a forecast.

## Console and statistics

`passenger_journey_shadow on` enables the local diagnostic; `off` disables it.
`stats` prints weighted passenger counts and packet counts; `reset` clears
statistics/details without resetting the current resource quota. `details` prints
at most 64 retained nonmatching, constrained or incomplete observations. The
default is off. Per-mode counts include evaluated/journey/fallback packets,
mixed-mode connections in the selected itinerary and snapshot prediction reasons
(vehicle state, order progress, facilities, order/timing and budget). These are
sampled diagnostics; the fallback counter does not mutate packet routing mode.
No synchronized setting, GUI setting, save field or multiplayer
command is added. A fresh process loading a save starts disabled with empty stats.
`inventory` performs an explicitly requested read-only count of current known-OD
passenger holdings by vehicle mode, capped at 1,048,576 vehicle/packet inspections.
It reports truncation and does not generate OD assignments or advance the game.

Categories include both Stay, both Transfer, legacy Transfer/planner Stay, legacy
Stay/planner Transfer, arrival mismatch, both Arrived, Unknown, Unreachable within
the search horizon, stop constraints and skipped. Forced unloading, NoUnload and
NoLoad observations remain in the independent constraints category.

Coverage is evaluated people / observed people with known OD. Complete-decision
coverage additionally excludes inconclusive results. Stay/Transfer agreement
uses only the four comparable Stay/Transfer categories. Arrivals, constraints,
unknowns and skips do not enter that denominator. All are arrival observations:
the same person may be counted again at a later station.
Eligible non-Manual train OD, excluded non-train/Manual OD and eligible observations
without a captured snapshot are also counted separately. A zero denominator
prints `n/a`, not a zero agreement/Unknown rate.

Unknown, BudgetExhausted and LimitReached are separate prediction statuses;
BudgetExhausted and LimitReached also count toward the broad Unknown decision.
PredictionUnknown includes any incomplete prediction, including horizon/visit
truncation, and is not exclusively a horizon counter. A Complete empty search
means Unreachable only within the configured horizon and transfer range.

Unknown-service reverse-search seeds are scoped to the destination's undirected
component of declared stops, including unresolved conditional branches. This
excludes physically disconnected untimed lines without inventing times or
certifying the absence of a possible connected unknown route.

## Resource ceilings

All allowances are deterministic work/memory counts; measured clocks never
select a route or decide whether to skip. Per 64 state ticks on this client:

| Resource | Ceiling |
|---|---:|
| Graph builds, across all trains/cargoes | 1 |
| Graph capture work | 1,048,576 |
| Graph conservative allocation envelope | 1 GiB |
| Uncached journey queries | 32 |
| Evaluated packet observations, including cache hits | 512 |
| Combined graph/query work | 2,097,152 |
| Individual query work / scratch | 65,536 / 8 MiB |
| Arrival cache | 32 entries / 1 MiB |
| Retained details | 64 |
| Default forecast horizon / transfers / candidates | 65,536 state ticks / 4 / 1 |

Graph allocation is admitted before capture using 1,024 bytes per work unit,
covering owned orders/schedules, transient vector growth and indices. The reported
graph memory bound is this conservative envelope, not process RSS or an actual
allocator measurement. Query scratch and cache have separate ceilings. Fleet
capture/index construction is bounded by admitted orders, vehicles and visits;
index sorting also has bounded logarithmic overhead. Work limits are not a
millisecond deadline and graph construction can still cause an observable pause.

The counters expose graph/search time, additional elapsed time in diagnostic
capture/observation/destruction, mean/max uncached query work, peak conservative
scratch/cache/graph memory and cache hit rate. Nanosecond accumulation also counts
short skipped observations. Timing numbers describe this machine/run only.

## Verification

MSVC 19.44 x64 Release: 290 unit cases and 64,039 assertions pass. Tests exercise
onboard full trains, NoLoad/NoUnload/forced unloading, missed connections, both
agreement cases, unnecessary express transfers, infrequent shortcuts, unknown
OD, incomplete predictions, graph/query memory and work exhaustion, invalid
snapshots, cache reuse and deterministic 4,096-packet overflow handling.

The actual Stage path is run with identical inputs and random seed with diagnostics
off/on. Checks compare full CargoPacket serialization, action counts, payments,
profit/transfer/revenue, simulation RNG, OD revision/allocation statistics and
deferred payments normalized by logical packet position. Cargo counts and the
legacy decision remain equal, including force-unload, force-transfer, NoUnload
and timetable-based legacy transfer sections.

All 296 CTest cases pass, including CargoDist, OD, graph/planner tests, four
AI/GameScript regressions, OD production load migration and the shadow full-save
comparison/reload test. The regression map has no evaluated OD packets; the
Stage test above and the national fixture exercise the actual observer.

The production CMake runner migrates an old save once, forks that identical
current-format save, advances both modes to the same calendar boundary, and
requires byte-identical full saves. Migrating once avoids the independently
generated save UID/generation ID of two legacy loads. It then reloads the shadow
save in a new process and verifies disabled/empty diagnostics. Missing NewGRFs
or removal of invalid vehicles abort the comparison. Actual multi-client network
sessions have not been exercised; the RNG/state comparison and complete-save
comparison verify the local-diagnostic determinism boundary.

For a real map, supply `LEGACY_SAVE`, `WORK_DIR`, `OPENTTD_EXECUTABLE` and, on
MSVC, `EDITBIN_EXECUTABLE` to `cmake -P
cmake/scripts/PassengerJourneyShadow.cmake`. `NEWGRF_DIR` optionally names a flat
directory of copied `.grf`/`.tar` assets. This enables the NewGRF checksum/TAR scan,
which `-Q` intentionally skips in the small regression. `MONTHS` defaults to 3
and can range from 1 to 12; `RUN_TIMEOUT` defaults to 90 seconds per run.
The test writes only beneath the selected work directory and executable folder.

An error-paused save must normally be rejected. With explicit permission to
continue despite its existing NewGRF errors, Windows can instead use
`VIDEO_DRIVER=win32`, `ALLOW_ERROR_PAUSE=ON` and a longer timeout. These options
open the normal game confirmation; they do not suppress or automatically accept
it. A human or UI automation must acknowledge the game's warning and resume
each run. Neither option changes the production diagnostic or its disabled
default. Such results must identify the pre-existing map/GRF errors.

The 8,001-station, 800-train fixture uses production defaults: about 45-68 ms for
capture plus query, 45-68 ms graph time, 0.4-0.7 ms search, 6,002 work, 119,666 bytes
scratch and a 417,792,000-byte graph envelope. The 4,001-train/8,004-declared-station
fixture includes 4,000 disconnected untimed lines; its complete trunk query needs
7 reverse work units. Connecting one uncertain line makes the result Unknown
again. The 4,096-packet fixture evaluates 512 packets, reuses 511 queries from
cache, skips 3,584, and retains only 64 details.

## User map comparison on 2026-10-11

Selected the newest save beneath Documents/OpenTTD/save, including autosave:
`C:\Users\bebe\OneDrive\ドキュメント\OpenTTD\save\autosave\netsave5.sav`
(37,379,151 bytes, modified 2026-10-11 19:29:52 +09:00). Its SHA256 was
`bcb4685db084921bf3eb82676f985a5b2ae186782b539ec0a316a1d2653c7a98` before and
after validation. Only copies under `build-shadow/real-map` were used/changed.
Local copies of 116 NewGRF files supplied the required checksums; runs with
missing GRFs or removed vehicles were discarded.

The save retained an error pause and its NewGRFs reported capacity changes
outside depots/refitting. The user explicitly authorized ignoring these errors.
Both runs acknowledged the normal unsafe-unpause dialog, using the same migrated
input, passenger CargoDist setting 2 and OD assignment enabled. The map has
2,283 primary trains and 20,830 total vehicles at the start. These pre-existing
GRF errors mean this is a local diagnostic comparison, not validation of those
GRFs or their multiplayer safety.

Both runs advanced from calendar 117486-12-19 to 117487-03-01, state ticks
3,175,431,315 to 3,175,436,570 (5,255 ticks). Full result saves matched byte for
byte, SHA256
`68f3f868c5d6d59d3324a9a2b14c126f05c2e4cd9153126dd0b3324ff276ca81`.
Reloading the on save started with disabled, empty diagnostics.

| On-run observation | Result |
|---|---:|
| Passenger arrival observations | 2,099,973 people / 195,357 packets |
| Observed known OD | 1,413 people / 347 packets |
| Evaluated / graph builds / searches | 0 / 0 / 0 |
| Skipped | 2,099,973 people / 195,357 packets |
| Measured extra diagnostic elapsed time | 37,814 microseconds total |
| Extra time per simulated state tick | about 7.2 microseconds |
| Unknown / agreement / cache-hit rates | not defined: no evaluations/queries |

An additional complete inventory of the resulting save found no known-OD
passenger packets on trains, ships or aircraft. Roads held 154 known-OD people in
27 packets, all non-Manual. Thus this map/run did not supply an evaluated train
OD sample; its zero counters must not be read as a zero Unknown rate or evidence
that the planner agrees with CargoDist. No real-map disagreement example was
observed. The controlled examples below validate both requested disagreement
directions; a train-OD-rich real map remains necessary to measure their prevalence
and prediction completeness. The measured 37.8 ms includes skipped observation
overhead and does not measure graph/query overhead; use the national fixture
measurements for that separate workload.

Logs and saves are in `build-shadow/real-map/final-comparison/{initial,off,on,reload}`;
the inventory log is `build-shadow/real-map/inventory/console.log`. Wall times
include waiting for warning confirmation and GUI/fast-forward differences, so
the off/on wall-time difference is not a CPU-overhead estimate.

## Representative controlled comparisons

From a loading local train at B, remaining dwell is 10 ticks, B-C is 100,
C dwell is 10 and C-D is 100. Staying reaches D in 220 ticks with no transfer.
An express that waits 500 ticks then takes 40 reaches D in 540 ticks, or 560
generalized ticks with the 20-tick transfer penalty. A legacy Transfer action is
therefore classified `legacy_transfer_planner_stay`. This directly detects the
requested unnecessary local-to-express interchange.

If the express leaves in 50 ticks and takes 40, it arrives in 90, with one
transfer and generalized time 110. Both Transfer can agree; if legacy Keep
chooses a local/shortcut waiting 500 ticks, the planner instead chooses the
faster trunk. These fixtures demonstrate the classifications; they are distinct
from observations on the user's map.

## Next stage: applying a journey

Actual boarding integration needs live permission/capacity validation after
choosing the timetable candidate, segment-specific reservations and a revisioned
capacity provider, missed-connection/full-train replanning and an explicit
contract between final OD and legacy via stations. Unknown OD/inconclusive
queries need a defined fallback. Authoritative decisions must use synchronized
budgets/state; the local sampling, counters and cache here must not govern cargo
movement. Save any future itinerary using stable identifiers, never snapshot
VisitIDs. Existing occupied-train continuation remains separate from new boarding.
