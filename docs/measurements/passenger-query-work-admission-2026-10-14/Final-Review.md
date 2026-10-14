# Final verification reproduction

The final report is [../../passenger-query-work-admission-final-2026-10-14.md](../../passenger-query-work-admission-final-2026-10-14.md). The scripts use isolated copied translation units for observers. No probe is part of the product source or a routing input. Binary files, proprietary NewGRFs and the input save remain local; manifests record their hashes.

Use an MSVC x64 Release build with the frozen package from the original measurement, `/O2 /Ob2 /MT`, assertions enabled and LTO disabled. Set `OPTION_PASSENGER_BENCHMARK=ON` for observations. Reconfigure with CMake between different probe types to restore original project inputs. Do not run games, builds or other measurements concurrently. The performance and network wrappers acquire the existing shared host lock and wait for competing processes to finish.

## Corrected performance

Starting from the existing profile-prepared build and package:

```powershell
python docs/measurements/passenger-query-work-admission-2026-10-14/Build-Variants.py build-admission --help
python docs/measurements/passenger-query-work-admission-2026-10-14/Build-Variants.py build-admission --variants control q16-t49152 --final-metrics
python docs/measurements/passenger-query-work-admission-2026-10-14/Run-Final-Review.py build-admission PATH_TO_FROZEN_PACKAGE --output NEW_RUN_DIRECTORY
python docs/measurements/passenger-query-work-admission-2026-10-14/Summarize-Final-Review.py build-admission/NEW_RUN_DIRECTORY/sequence.json --archive PATH_TO_ARCHIVE
```

The wrapper rejects another input SHA or actual tick interval. It runs A/B/C/C/B/A, two repetitions each, and only retries independently detected host contention. The summarizer rehashes saves, checks logical accounting and caps, and compares ordered queries, results, admissions, rejection reasons, transport and complete save hashes. `final-review/` contains the completed six-run evidence. Complete stderr is retained as lossless gzip with uncompressed byte count and SHA-256 in the summary, to avoid committing hundreds of thousands of repeated NewGRF warning lines. Raw console and regression logs retain their original whitespace.

## Real v4 compatibility

After reconfiguring the benchmark build, use `Prepare-Save-Integrity.py build-admission --legacy-reader`, build `openttd`, and retain a copy of its executable. Repeat preparation without `--legacy-reader` and retain that executable. The reader observes the old byte in a temporary scalar; the native variant skips the unknown field exactly as the product does. Both write v3 and preserve normal Packet merging.

`Run-Save-Stage.py --help` describes the frozen assets, executable, input, output directory and tick arguments. Do not use `-Q`: normal NewGRF scanning is necessary for this real map. Run the reader and native variants at zero ticks, then reload the native v3 result at zero ticks. For actual continuation use the copied driver from `passenger-timed-anytime-2026-10-14/Prepare-Continuation.py` and run 256 ticks. `Read-Save-Feature.py` reads the actual compressed SLXI version independently. `Finalize-Save-Review.py` validates and archives the completed stages; `final-save-review/` contains official results and the excluded setup attempts.

## Loopback join

After a clean CMake reconfiguration of the benchmark build:

```powershell
python docs/measurements/passenger-query-work-admission-2026-10-14/Prepare-Save-Integrity.py build-admission
python docs/measurements/passenger-timed-pruning-2026-10-13/Prepare-Network-Probe.py . build-admission
python docs/measurements/passenger-query-work-admission-2026-10-14/Prepare-Final-Multiplayer.py build-admission PATH_TO_FROZEN_PACKAGE --directory NEW_NETWORK_DIRECTORY
python docs/measurements/passenger-query-work-admission-2026-10-14/Prepare-Network-Close-Probe.py build-admission
cmake --build build-admission --config Release --target openttd --parallel 8
python docs/measurements/passenger-query-work-admission-2026-10-14/Run-Final-Multiplayer.py build-admission PATH_TO_INPUT_SAVE --directory NEW_NETWORK_DIRECTORY --duration 120
```

Server and clients have unrestricted CPU affinity. Both clients disable policy, departure, fallback residency and scratch. Sync frequency is one. Joining alone is insufficient: the harness requires continued progress, two connected clients, valid payments, and identical full saved Packet fields at the same paused tick. Close probes log status and frame without changing any protocol decision.

## Product and boundary checks

Restore original project inputs with CMake and set `OPTION_PASSENGER_BENCHMARK=OFF`. Remove stale objects only for the explicitly replaced translation units before rebuilding the product, test executable and regression files. Run all CTest cases and the passenger suite; preserve logs and final build configuration. Compile `Age-Boundaries.cpp` against the actual production header with GCC `-std=c++20 -O1 -g -fsanitize=undefined -fsanitize-undefined-trap-on-error`. A normal invocation must pass 95 cases; the `calibrate-old-overflow` argument deliberately runs the retired unsafe subtraction and must trap.

`Check-Final-Artifacts.py` validates all JSON and decompresses every final performance/network stderr archive to verify its original byte count and SHA-256. It also requires every offline cache and real-save check to pass. It does not turn failed network trials into passing results.
