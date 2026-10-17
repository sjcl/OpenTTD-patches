# Station passenger route estimates

The station Routes menu shows waiting passengers or hides the overlay. Its former
monthly planned mode is removed; the station's ordinary CargoDist planned amounts
and the general LinkGraph overlay are unchanged.

`linkgraph/passenger_route_overlay.cpp` groups waiting packets by OD and routing
identity. Journey packets use their saved ride, independently of the queue key.
The service incarnation, generation, order occurrences, live carrier capacity and
station access must still be valid. The displayed path follows physical stopping
orders, including intermediate stops, repeated stations and the cycle seam.
NoLoad, NoUnload, non-stop orders, forced unloading, refits and conditional orders
are checked before the whole ride is accepted. VehicleSnapshot rides use the
existing bounded service signature check.

Hyperpath intents use `GetPassengerDestination()` as their OD destination.
`PassengerPrecomputedTable::InspectRoutes()` reads published policy storage into
GUI-owned memory. `PassengerServiceDatabase::InspectRoutes()` selects exactly one
current or retained table and validates the resulting service bindings. Incoming
service exclusions and active capacity-failure exclusions are included. A cold
failed-service policy is evaluated with the existing frozen ride recurrence and
strategy arithmetic, locally, without populating simulation caches.

For each attractive boarding occurrence, the expected share is

```
packet_count * PassengerStrategy::Frequency(pattern.wait) / sum(candidate_frequencies)
```

This uses the routing model's fixed-point inverse-wait frequencies. Actual
OrderLists remain distinct opportunities in Hyperpath; grouped legacy pattern
members supply one whole ride. No prefix is joined to another service's through
continuation. A short turn ends at its actual alighting occurrence. Onward
policies are expanded again, with the incoming service excluded. Hyperpath's ride
recurrence chooses whether to stay aboard; legacy shared sections also reuse the
existing through-ride extension check.

OD fallback packets prefer published, live-valid direct rides to their destination
or rides to their saved CargoDist hop, followed by destination policies where
available. These fallback first rides use inverse-circuit weights. Without such a
prefix, a destination policy can supply an estimate. Packets without an OD retain
the origin-specific CargoDist flow estimate. An unresolved suffix is never counted
as an arrival; only a real OD arrival or an explicit legacy self-flow adds an
alighting marker.

The overlay retains its existing colours, link drawing, tooltips and markers.
Amounts accumulate as doubles by first movement and directed segment, then round
once per displayed aggregate. Policies and validated ride paths are cached only
within a rebuild; rendered routes refresh at most once per second. Limits are
8,192 groups per cargo, 262,144 display work units per rebuild, 4,096 per policy or
ride validation, 64 candidates per policy, 64 rides per path, and a minimum branch
amount of 0.05 passengers. Transfer history rejects cycles while allowing real
through rides to revisit stations. Omitted Hyperpath branches keep their original
probability mass; surviving branches are not inflated to compensate.

No GUI lookup calls simulation Plan/Candidates, changes a packet, admits work,
updates simulation diagnostics, or changes saved data. Routing and boarding code
paths are unchanged. Missing policies, invalid services, unsupported orders and
display limits leave a partial estimate, without inventing a link to the OD.

Regression coverage is in `tests/passenger_precomputed.cpp` and
`tests/passenger_service_database.cpp`, tagged `[route-overlay]`. It includes
invalid queue keys, frequency shares, transfer branching, retained generations,
incoming/failed services, stop occurrences, fallback/legacy handling, display
limits, unchanged database serialization, diagnostic counters and admission work.

Validation: MSVC x64 Release builds of `openttd` and `openttd_test`, with assertions
enabled and LTO disabled, succeeded. The combined filter
`[route-overlay],[hyperpath]~[scaling],[precomputed]~[scaling],[station-gui]` passed
146 test cases / 7,682,215 assertions after integration with `jgrpp-ya` at
`093218f882`. `git diff --check` passed. No long real-map run or large benchmark
was performed.
