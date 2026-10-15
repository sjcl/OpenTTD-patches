# Integration evidence

Runtime base: `39a62348c9df0ce2636a45af4d6869067e621222`.
Runtime fixes: `9e3899d654`, `1dbc033a73`, `f46a92b8e7`, `fa91456e04`, `a2915e1844`.
See [the report](../../passenger-routing-integration-2026-10-15.md) for results and limitations.
Large saves, executables, fonts and NewGRFs are retained locally in the ignored
`build-integration` directory; only their identities and compact observations are committed.

## Reproduction

Use MSVC x64 Release with asserts enabled, LTO disabled and
`OPTION_PASSENGER_BENCHMARK=OFF`. Supply matching language files and an isolated asset
package accepted by `utils/passenger_precomputed_real_map.py run`.
The real-map input SHA-256 is
`6783a500f47435a342a36e9a6544e33bb38f041abe91e9e2ff2c654edd16c978`.
Keep the host free of other builds, tests and OpenTTD runs during performance measurements.

The following preparation changes generated VS project references to copied TUs;
it does not edit product source files. Pass absolute paths for `SOURCE` and `BUILD`.

```powershell
python -B utils/passenger_precomputed_join_prepare.py SOURCE BUILD a2915e1844
python -B utils/passenger_precomputed_real_map.py driver SOURCE BUILD a2915e1844
cmake --build BUILD --config Release --target openttd --parallel 8
```

Preserve that binary for CPU measurements. For the separate physical-trace binary,
run the following before rebuilding:

```powershell
python -B utils/passenger_transport_trace.py SOURCE BUILD
cmake --build BUILD --config Release --target openttd --parallel 8
```

Run `real_map.py run` (full filename above) with `--resume --ticks 4096`, once with
`--no-precomputed`, once with `--precomputed --warm-precomputed`, and once with
`--precomputed` using the saved replacement frontier. The last case starts with
18 current rows and 4,618 retained rows; it is not an initial empty-table case.
Use different output directories and run sequentially. Quality runs use the trace
binary, `OPENTTD_PASSENGER_TRACE=1`, `OPENTTD_PASSENGER_CHECKPOINT_TICK=4096`,
`--ticks 24576`, and the first two routing setups. Other game settings come from
the same save. The checkpoint permits canonical comparison with the untraced
4,096-tick result, independently for each routing mode.

For the separate empty-table construction observation, use the CPU binary with
`--resume --ticks 0 --precomputed --warm-precomputed --cold-precomputed`.
The normal disable callback discards saved rows, diagnostics record that frontier,
and the normal enable callback and copied warm command complete the table.
Confirm the two inventory observations have identical StateTicks and passenger
counts. Its process CPU/memory includes input load/replay, while the warm command
timer covers construction after discarding the saved frontier.
Reload that completed save separately with the CPU binary, `--resume --ticks 0
--precomputed --warm-precomputed`, omitting `--cold-precomputed`. Compare every
canonical field with the completed-build snapshot, including reconstructed row
and operation counters. Its elapsed/process CPU/peak memory describe completed
table load, and its warm timer only includes any remaining preparation.

`passenger_precomputed_real_map.py summarize` aggregates tick CPU observations;
`passenger_transport_summary.py` aggregates actual vehicle movement and passenger
boarding/transfer/delivery samples. Its two input directories must be ordered
legacy, then precomputed for the OD ratio direction.

For multiplayer use `passenger_precomputed_join.py` with an isolated binary made
by `join_prepare.py` **without** subsequently replacing its null client driver.
The large-map trial uses `--toggle-precomputed --ticks 1024` and the default game
timeouts. The granted-frame regression uses the small timed fixture,
`--frame-freq 20 --ticks 5000`, and the explicitly recorded copied graph quantum1
seam. That seam binary must never be used for CPU or real-map quality comparisons.

Re-run CMake configuration after instrumentation to restore original TU references,
then build the ordinary product. Check both the project and binary for measurement
markers before using or distributing that executable.

## Interpretation

Tick samples time the simulation thread; process CPU and peak memory also include
load, paused warm-up and save. Trace-enabled runs are not CPU benchmarks.
Sampled complete trips, loaded left-censored trips and pending right-censored trips
are separate. Group names provide a heuristic service class, not a formal timetable
classification. The trace hashes packet ID and birth tick, choosing approximately
one eligible packet in 64 and following its midpoint through splits and merges.
It enrolls initial known-OD people before changing routing settings, so those
individuals can be matched by saved packet ID and offset. This is not a
population-weighted travel-time estimate. The separate origin-departure measure
starts at an observed first boarding; loaded onboard travel remains left-censored.
Origin and transfer waits are separate, and pending/lost people are reported.
Initial sample fates must sum exactly; fresh and all sample starts must balance.

`transport-population.csv` counts **all passenger** boardings, transfer unloads
and deliveries using actual carrying vehicles; its `known_od_*` columns are the
known-OD subset. Those columns should agree with the routing movement counters
across Timed, Schedule and Untimed modes. Reservations are excluded.
The inventory balance records initial + generated - delivered - remaining;
other removals include ordinary station cargo loss and are not successful trips.
Known-OD balance can also include destination invalidation. Group classes use
the actual vehicle's group hierarchy captured at movement, with unclassified
names left visible. The company delivery census includes all passenger cargo
within its current economy period and can reset at an economy boundary.

`passenger_precomputed_diagnostic_readback.py` reopens end saves on a loopback
dedicated server whose startup script reads diagnostics and quits before replay
or simulation. It verifies all canonical fields against the end snapshot; only
the three derived ready-row/operation display counters are excluded until replay.
Its event gauges describe the loaded state, and its reset process-local counters
are not substitutes for the original run's cumulative work/CPU observations.

Incomplete harness trials and the pre-fix MP failure are retained separately and
excluded from successful comparisons. The final precomputed physical trace
completed every requested tick with an end save and valid payments, but the
shared harness returned failure after observing one `cl:64896` compiler process.
Its raw contamination record remains intact. Only deterministic transport
observations are used from that run; its timing is excluded. All three CPU-only
runs, the cold build and the completed-table reload have no recorded competitors.
Known NewGRF capacity warnings and measurement-only
error-pause resumptions are retained in run metadata and raw logs.

The evidence manifest records both original and compressed SHA-256 values.
`integration-validation.json` records checkpoint equality, sample accounting,
test counts, utility hashes, and the restored product binary/project marker checks.
The 16,384-tick legacy calibration also verifies that adding initial enrollment
and generation/loss bookkeeping did not change canonical game state.

The one-off read-only quality probe scripts under `checks` are retained as run.
To repeat them, copy those three `*quality_probe.py` files into the repository's
ignored `build-integration` directory first; their path resolution assumes that
location. Supply the same local asset package and completed-build save, or adjust
the package path in the runner. Finish the isolated profiles before building the
probe. The build helper restores the ordinary product after copying the diagnostic
binary. The probe records timetable flags/periods and verifies all canonical
fields after inspection; its timing is not a benchmark.
