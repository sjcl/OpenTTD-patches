## JGR's Patchpack + Yahho's additions Changelog

* * *

### v0.73.3+3 (2026-10-18)
* Add optional explicit passenger destinations and deterministic sparse OD assignment
* Add periodic and schedule-aware passenger journeys, shared precomputed destination policies, and optional Hyperpath strategies with replanning after capacity misses
* Bound passenger calculation work, retain usable routes during replacement, and restore saved construction progress deterministically when loading or joining multiplayer games
* Add station passenger routing status and diagnostics, and aircraft link cost additions
* Consolidate unreleased passenger save formats into version 1 and use the current precomputed algorithms for saved construction progress; retain loading of v0.73.3+2 saves, without compatibility for intervening development builds
* Import automatic renewal of the fund new buildings town action from ns2091/OpenTTD-patches, with a per-company toggle in each town's local authority window
* Import optional markers above vehicles in the selected group, coloured by the immediate child group and including sub-groups, ungrouped vehicles, and all vehicles
* Keep diagonal rail tunnels and tunnel/bridge station platforms excluded

### v0.73.3+2 (2026-10-11)
* Add optional timetable-aware passenger train transfers, comparing arrival times, connection waits, and transfer penalties while retaining CargoDist flow choices
* Cache service lookups and arrival estimates per unloading operation, with bounded work and conservative fallback for uncertain orders and timings
* Import per-shared-order-list CargoDist cost scale/addition controls and same-mode/mode-change transfer penalties from ns2091/OpenTTD-patches
* Import near-end stopping for trains longer than their platforms and fix MSVC SSE4 blitter name lookup
* Add passenger routing and route penalty tests; the full unit suite and all four script regression tests pass
* Existing saves remain loadable; saves written by this build require support for the linkgraph_route_penalty save feature

### v0.73.3+1 (2026-10-11)
* Update base JGRPP to 270709121ef173f82f8345ac042081ae05327440 (0.73.3 branch)
* Match the upstream version while preserving the jgrpp-ya prefix, +1 suffix, and fork-specific gameplay changes
* Adapt the create_town console command and Japanese translations to upstream changes

### v0.64.1+1 (2025-02-08)
* Patch feeder payments system
* Update base jgrpp to 4312e8ee89a27dbb500145b8ae93798e60cde0c9
*
### v0.58.3+1 (2024-04-20)
* Update Japanese translation
* Update base jgrpp to 6056a002229e337a2d84918cc4445a524ade8595

### v0.58.1+1 (2023-09-26)
* Update Japanese translation
* Fix town generate command
* Update base jgrpp to 7796451b6c5ea1a507a3901e52d7001d53e80f15

### v0.55.3+0 (2023-09-26)
* Update Japanese translation
* Update base jgrpp to d4ed08849855b99b39fd95a83faad3c7cec4f2e0

### v0.54.5+1 (2023-08-06)
* Update Japanese translation
* Update base jgrpp to 5bcc71f2c2903facbacb7ead525f06f4081887f7

### v0.54.4+2 (2023-07-25)
* Add Zstd for Windows

### v0.54.4+1 (2023-07-25)
* Fix Actions

### v0.54.4+0 (2023-07-24)
* Update base jgrpp to 0.54.4
* Cherry-pick sjcl's chunnel restriction removal patch
* Update Japanese translation

### v0.47.3+0 (2022-06-09)
* Update base jgrpp to 0.47.3
* Update Japanese translation

### v0.45.1+0 (2022-01-10)
* Update base jgrpp to 0.45.1

### v0.45.0+0 (2022-01-07)
* Update base jgrpp to 0.45.0
* Update Japanese translation

### v0.44.2+0 (2021-12-10)
* Update base jgrpp to 0.44.2
* Update Japanese translation

### v0.44.1+0 (2021-11-29)
* Update base jgrpp to 0.44.1
* Update Japanese translation

### v0.44.0+1 (2021-11-16)
* Update base jgrpp to #25a46773
* Update Japanese translation

### v0.44.0+0 (2021-11-11)
* Update base jgrpp version to 0.44.0
* Update & fix Japanese translation
* Fix some string use

### v0.44-rc1+0 (2021-11-05)
* Update base jgrpp version to 0.44-rc1
* Update & fix Japanese translation

### v0.43.2+0 (2021-10-30)
* Update base jgrpp version to 0.43.2
* Cherry-pick sjcl's non-cheat industry remove patch
* Update Japanese translation

### v0.43.1+3 (2021-10-21)
* Add workaround online contents browser issue
* Fix some Japanese translation

### v0.43.1+2 (2021-10-20)
* Fix Release failure
* Add result message for command 'createtown'
* Include upstream(jgrpp) BugFix

### v0.43.1+1 (2021-10-18)
* Include upstream(jgrpp) BugFix
* Cherry-pick sjcl's 'createtown' command patch

### v0.43.1+0 (2021-10-17)
* Fix zstd for windows build
* Update Japanese Translation
* Marge sjcl's cargo dist patch
