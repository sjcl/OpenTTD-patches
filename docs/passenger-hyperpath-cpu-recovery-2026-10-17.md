# Hyperpath boarding CPU recovery

Work in progress on `feat/passenger-hyperpath-routing`, starting at
`21b7f06ad9cbe7e214ac3762d9e73f56e6f3fc46`. No routing optimization or capacity
policy change has been applied yet. Task 2 is gated on measured Task 1 results.

## Measurement instrumentation

`passenger_hyperpath_prepare.py --boarding-profile` adds hooks only to copied
measurement translation units. The product, saved fields, routing Work and
event schedule are untouched. The ordinary physical transport trace remains
disabled for CPU attribution.

`boarding-profile.csv` contains cumulative counters after every actual
simulation tick:

- `VisitBoardingCandidates` invocations and enumerated Hyperpath cohorts;
- Hyperpath enumeration-block calls and elapsed nanoseconds;
- `MayUseHyperpathPolicy` calls and elapsed nanoseconds;
- `EvaluateRide` calls, elapsed nanoseconds and visited stops, both globally
  and while inside the enumeration block.

Enumeration time includes its nested predicate and ride time. These values
must not be added. Timers themselves add observation overhead. A separate
uninstrumented comparison is necessary for the final CPU acceptance result.
The existing reset command clears counters after paused initial preparation.
Snapshot collection and CSV output run outside measured simulation calls.

After normal CMake generation:

```text
python -B utils/passenger_hyperpath_prepare.py SOURCE BUILD REVISION --boarding-profile
cmake --build BUILD --config Release --target openttd -j 6
```

Use the existing exact-StateTicks runner with tracing disabled, a completed
initial table, 2,048 ticks and replacement at tick 1,024. After both runs:

```text
python -B utils/passenger_hyperpath_boarding_report.py attribution.json LEGACY HYPERPATH
```

The report requires identical input and executable hashes, complete per-tick
snapshots, successful exit, no competing measurement and no physical trace.
It reports the enumeration increment divided by the simulation increment for
the complete interval, ordinary ticks, ordinary tail and replacement ticks.

## Input availability and baseline validation

The earlier construction fixture, SHA256
`306a8d3094c406286ff1dd3249f1726bf9d10e230ead593eacdd91efc43ac629`,
and its frozen JPSD/station input and real-map assets have been recovered from
another existing measurement checkout and copied into this task's ignored
build directory.

The former transport-comparison input, SHA256
`6783a500f47435a342a36e9a6544e33bb38f041abe91e9e2ff2c654edd16c978`,
is currently unavailable at the recorded paths. Several former measurement
checkouts, including `33d4` and `c430`, disappeared during setup. Their ignored
binaries, saves and measurement directories are unavailable there. A comparison
using the earlier fixture must be identified as a different starting state;
it cannot reproduce the previously reported transport interval.

MSVC x64 Release, assertions enabled, LTO disabled, static runtime. Baseline
validation passes all 721 cases: 714 unit cases and four script regressions
on the first run, then the three production-launch cases after placing the
language/basic graphics assets beside the Windows executable. The first
launch attempts timed out in the incomplete asset layout; no product edit
was needed. The instrumented executable also passes a 64-tick exact-driver
smoke with replacement at tick 32. The real-map attribution pair is running;
no CPU-recovery result is claimed yet.

## Preliminary service 528 readback

The read-only `passenger_hyperpath_pattern_readback.py` checks and decodes an
extracted JPSD v9-v14 blob, including all snapshot records and the v14 trailer.
It does not load a game or advance Work. The recovered v9 fixture contains
864 services and 17,445 stops in each of its published/current/retained
snapshots, matching the original fixture census.

In its published service 528 there is one vehicle and 111 stops, with an
estimated circuit of 82,362 ticks (`period=0`). Order 33 is station 3393,
arrival/departure 26,196/26,418; order 34 is station 3394,
arrival/departure 26,788/27,010. Both permit boarding and alighting for
passengers. Their incoming travel quality is 1: a positive recorded travel
duration without timetable certification. These offsets correspond to
370-tick travel legs and 222-tick dwell.

`CompileEstimated` skips implicit/label orders, retains the real order index,
uses a positive recorded travel duration when available and adds the recorded
dwell. Thus this frozen pattern's order 33 is an explicit station occurrence,
not a synthetic extra frequency. This only establishes how the recovered
snapshot represents that order. Actual order executions, departure-time
passenger states and the cause of the OD regression still require a live
targeted diagnostic; none is claimed from this readback.
