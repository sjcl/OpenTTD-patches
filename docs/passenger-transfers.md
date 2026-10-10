# Timetable-aware passenger train transfers

Enable **Environment / Cargo distribution / Use timetables for passenger train
transfers**, or use the console:

```
set linkgraph.passenger_transfer_timetables true
```

The default is disabled, including when loading existing saves. Automatic
distribution must be enabled for the passenger cargo. Mail and freight retain
the original behaviour. The connection time/penalty setting is in simulation
ticks (`linkgraph.passenger_transfer_penalty`, default 20), independent of GUI
clock display units.

## Algorithm and integration

`PrepareUnload` creates a `PassengerRoutingContext` shared by all coaches.
`VehicleCargoList::Stage` still draws its via from the existing FlowStat. For
that via, the new predictor follows the current train's orders beyond its
immediate stopping station. Each alternative is identified by its order list,
boarding order position and individual vehicle state. Ordinary and express
running times are never combined into a station-pair average.

The predictor uses travel and dwell times, the current order index, time spent
on that order and timetable lateness. Elapsed loading time and early/late
timetable waits are accounted for. It obtains Scheduled Dispatch slots through
the simulation's read-only `GetScheduledDispatchTime` helper. The GUI departure
board is not recalculated, and no departure-board settings affect the result.

The absolute predicted arrival on the current train is compared with each
usable connection's predicted arrival plus the transfer penalty. An alternative
must leave at least the connection time after now. This is equivalent to
comparing remaining running/dwell time against waiting time plus alternative
running/dwell time plus penalty. Equal cost keeps the passenger onboard. A
faster connection preserves the transfer required by the selected via.
Unknown timings preserve the original action.

This feature suppresses existing transfers; it does not create a transfer when
the via is already the current train's next cargo stop. Doing so would let the
existing station loader immediately reload the passengers onto the same train.
Selecting a faster train in that situation requires service-aware station
boarding, beyond the existing station/via buckets and this implementation.

An alternative must stop and permit loading here, carry the same cargo, and
have the selected via as its next cargo stop. This last restriction is necessary
because the existing station loader only loads packets assigned to a vehicle's
next cargo stop. An alternative that merely calls at the via after other cargo
stops cannot currently pick up that bucket of waiting passengers.

After choosing to remain onboard, a packet retains that via in `passenger_via`.
At intermediate stations its arrival estimate is refreshed, and a faster,
permitted connection can still be selected. Otherwise it continues to the
original via, even if there are no predictable alternatives at this station.
Arrival at the via resumes normal FlowStat selection; the via is not treated
as the final destination. Unloading, deleted stations, disabled settings,
unpredictable onward journeys and forced unloading/transfer cancel the retained
via. Splitting copies it and vehicle merging distinguishes different retained
vias. Existing restricted-flow permissions are respected.

## Conservative prediction boundaries

Prediction stops at a repeated boarding station, forced unloading/transfer
before the via, or a via that cannot unload. Non-stop orders do not count as
station calls. Unconditional jumps with known branch time are followed, with
an order-count bound to prevent loops.

Nontrivial conditionals, depot detours (including service, halt and unbunching),
refits, implicit orders, slot/counter operations, unknown vehicle phases and
missing timetable data use the original algorithm. A train still travelling
beyond its recorded travel time has an unknown remaining delay. Full-load and leave-early
waits are unknown. Future Scheduled Dispatch bookings shared by multiple
vehicles are unknown unless the schedule permits reuse: the predictor does not
guess which competing vehicle will claim a slot. Already assigned departures
at the current loading stop can be predicted from their shifted timetables.
Empty/invalid schedules also fall back.

An active, potentially relevant alternative with unknown timings prevents a
cost-based override. Connections using other vehicle types similarly retain
the original decision. The cycle-time / fleet-size mean-headway approximation
is deliberately not used: it would hide irregular departures, branches and
dispatch-slot contention.

## Cache, determinism and compatibility

The station service index contains only order-list IDs. It is rebuilt lazily
after order destination registration/unregistration, order-list initialization
or copying, or destination-index clearing at game reset/load. It contains no
timetable, cargo-capacity or dispatch estimates. Vehicle membership, capacities,
load/unload flags, schedules and current phases are read fresh at each arrival.

Journey and comparison results are cached by cargo type and via for the lifetime
of one `PrepareUnload` call. Thus packets and coaches share calculations, and
all time estimates expire automatically at the next arrival. Index building
cost is proportional to the number of orders after invalidation; normal
lookups visit only services at this station. Alternative predictions are
bounded by a conservative budget of 32,768 order/consist visits per arrival;
exhausting it restores the original decision for that query. Each journey
traverses at most twice its order-list length, with at most one additional
linear scan to report an uncertain alternative.

All reads occur on the simulation thread during cargo staging. No LinkGraph
job reads live vehicles. Integer state ticks, deterministic pool/ordered-map
iteration and synchronized game settings are used; caches draw no random
numbers. Initial FlowStat choice remains probabilistic. Retained vias avoid
additional random route choices until the via is reached or invalidated.

Existing saves load with an invalid retained via. The new via is saved as a
named, versioned CargoPacket field (`passenger_via` feature); older JGR readers
can ignore this feature in table-format saves and resume their original
behaviour. No LinkGraph or FlowStat save schema is changed. The packet gains
one StationID field; differing retained vias may reduce packet merging.

## Verification

The `[passenger]` Catch2 tests exercise the real predictor and cargo staging:
slow and fast express connections, separate service running times, current
phase and delay, missed connections, unknown timings, force-unload/transfer,
non-stop/no-unload stops, bounded conditional loops, dispatch slots and
contention, disabled/freight behaviour, unrelated/relevant unknown services,
via retention through an intermediate
flow change, a faster connection at the intermediate stop, invalidation of an
unreachable retained via, avoiding a new unload/reload cycle on a direct next
stop, packet splitting/merging, staging 2,001 separate
packets and preservation of the legacy LinkGraphJob schema.

Run `openttd_test "[passenger]"`. The optional `[.performance]` test evaluates
10,000 journeys across 1,024 orders:

```
openttd_test "[.performance]"
```

The unit scenarios do not substitute for profiling a populated national-scale
map or a multiplayer replay. Physical future signal congestion, platform
queues, breakdowns and available seats are not forecast. Timetable estimates
can be inaccurate when these differ from recorded running times. Final-destination
optimization and changing the CargoDist allocation of demand among services
remain outside this implementation.

On the local Windows Release build, 10,000 predictions across 1,024 orders took
70–123 ms (73 ms on the final run), and staging 2,001 separate packets took
1–4 ms. These are synthetic
measurements, not national-map frame-time estimates. Both executable targets
build successfully. The full unit suite passes 3,801 assertions in 203 cases;
the explicit `[passenger]` run, including its hidden performance case, passes
10,225 assertions in 10 cases. The existing `stationlist` regression save loads, saves
with the feature enabled, and reloads with the setting preserved. Its regression
output passes when the generated test configuration uses installed Windows fonts
(the default bundled-font loading emits extra diagnostics in this environment).

## Changed files

- Prediction and simulation integration: `src/passenger_routing.h` (new),
  `src/passenger_routing.cpp` (new), `src/cargopacket.h`, `src/cargopacket.cpp`,
  `src/economy.cpp`.
- Service-index invalidation: `src/order_cmd.cpp`, `src/order_dest_func.h`.
- Synchronized settings and UI strings: `src/settings_type.h`,
  `src/table/settings/linkgraph_settings.ini`, `src/settingentry_gui.cpp`,
  `src/lang/english.txt`.
- Save compatibility: `src/sl/cargopacket_sl.cpp`, `src/sl/extended_ver_sl.h`,
  `src/sl/extended_ver_sl.cpp`, `src/sl/linkgraph_sl.cpp`.
- Build, tests and documentation: `src/CMakeLists.txt`, `src/tests/CMakeLists.txt`,
  `src/tests/passenger_routing.cpp` (new), `docs/passenger-transfers.md` (new).
