# Schedule-aware passenger execution

`passenger_journey_schedule.cpp` searches the published `PassengerServiceDatabase`
station/OrderList incidence index. It does not build or forecast a ServiceGraph.
`CompilePolicy()` remains the Timed search. Active routing first requests a final
OD Timed route, then a duration-only route, then the existing live OD fallback.
Both searches fit in the existing admitted 65,536-work envelope. A usable Timed
winner takes priority even if alternatives could not be exhausted. An estimated
intent records `timed_deferred` when Timed admission was inconclusive.

The schedule search uses nonnegative integer tick costs: travel and intermediate
dwell, waiting, minimum interchange, transfer penalty, existing waiting/transfer
weights, and a capacity risk allowance. A single unknown-phase vehicle has an
expected residual wait of ceil(circuit/2). Multiple unresolved vehicle phases use
one whole circuit. All cargo-capable phases known use half the largest measured
phase gap. Certified dispatch slots use half the largest slot gap. Unresolved
dispatch uses the larger of the timetable and declared dispatch circuits, as an
uncertainty penalty; it does not assert assigned slots, regular departures, or
guaranteed connections. No period is divided by fleet size.

Capacity risk adds ceil(circuit/min(largest physical cargo capacity,64)) ticks
per boarding. This deterministic preference represents possible lost boarding
opportunity, not a seat forecast or a demand equilibrium. The largest individual
consist capacity is used; several vehicles never become one fictitious carrier.
Actual available seats, reservations, splits and gradual loading still belong
to the existing CargoList loading actions.

Publication rejects a physical vehicle when bounded inspection proves a declared
facility incompatible, and rejects a known out-of-range aircraft. Unknown current
order/phase alone does not reject it: static facility checks can run independently
of that current phase. Inspection budget exhaustion stays uncertain. Version-one
publications recapture uncertified vehicles under ordinary build admission;
certified fleet members and saved sweep progress remain available.

Known and unknown legs use these duration costs in a schedule policy. A timed
connection following uncertain arrival is never promised. Bounded labels retain
distinct transfer paths and station rounds. Transfer-station revisits and
immediate reboarding of the same service are excluded. Existing Work, label,
memory and maximum-transfer limits apply. Limits indicate an inconclusive
search, independently of whether a safe final-OD candidate was found.

The database owns a derived OD/evaluation/excluded-service policy cache. Every
publication or structural invalidation clears it. Hits replay cold logical work;
cache residency, wall-clock durations and diagnostic reset never affect routing
or admission. Timed and Schedule policy ceilings are 12 and 4 MiB, retaining the
existing combined 16 MiB budget. No nationwide search runs per CargoPacket.
Event caches share the first intent across packets with identical cargo, OD,
ready time and arriving-service exclusion.

`EstimatedServiceIntent` and `EstimatedBoundService` explicitly carry unknown
times. Their `departure` and `arrival` are always zero/unset, including tick
rebasing. They retain OrderList identity/incarnation/generation, boarding and
alighting order occurrences, real `not_before`/`planned_at`, and waiting durations.
Plan-time VehicleID remains invalid. An arriving shared physical vehicle can
bind only after checking the live occurrence, cargo capacity, facilities,
NoLoad/NoUnload, forced alighting, order prefix, current incarnation and published
generation. Onboard continuation validates the current physical order prefix;
timing-only generation publication does not force passengers off a safe carrier.
Observing an unknown phase coalesces into the normal timing epoch so that a
vehicle arrival cannot invalidate its own safe intent just before boarding.

At a planned or forced alighting, the packet keeps final OD and records the
actual arrival plus minimum interchange time and the arriving service incarnation. The next
route is evaluated before staging, and the same constraints survive deferred
planning and fallback. The arriving service cannot reload the packet. Intent
expiry after two conservative circuits permits re-evaluation; structural
invalidation also triggers ordinary rerouting. Selecting the same still-best
service after expiry is possible; congestion feedback/demand allocation is not
implemented.

Splits copy intent, constraints and carrier provenance. Unmoved station suffixes
restore both intent and previous carrier provenance. Merge equality includes
them. Returned reservations restore unbound intents, and remaining station
suffixes can use another shared carrier. The existing physical-vehicle reverse
index includes estimated bindings. Journey save feature 3 stores these fields;
periodic feature/wire version 2 adds the unresolved dispatch circuit. Old Journey
versions 0–2 and periodic wire version 1 remain readable.

Console diagnostics separately report Timed, Schedule and Untimed adoption and
actual transport. Last-carrier provenance survives next-intent staging, so
transfer/delivery counts identify the vehicle that performed the move. A mixed
route can consequently have different categories on successive segments.
Reservations and staging do not increment actual transport. Completed-interchange
counters additionally observe the next vehicle actually loading the people,
categorized by the previous carrier; alighting alone does not increment them. Waiting difference
uses passenger-weighted expected and actual durations; counters observe repeated
boarding, not unique people. Schedule inventory separates available plus reserved
waiting from Timed waiting. Dedicated reasons include missing service, incarnation,
generation, station, disabled mode, deleted vehicle, live prefix, forced unload
and wait expiry.

An additional persistent history bit is set at a real arrival when CargoList
stages physically stored schedule cargo (reserved cargo is excluded by the
existing Stage invariant). Final-OD delivery after any such segment is counted
as Schedule-assisted, including mixed paths whose last carrier was Timed. This
overlaps the disjoint final-carrier delivery counts and must not be added to their
sum. Planned intents and returned unboarded reservations do not set this bit.

Partial timetables and unsupported order controls continue to use Untimed
fallback. This model makes no absolute timing, full-network optimality, occupancy
forecast or equal-spacing guarantee.
