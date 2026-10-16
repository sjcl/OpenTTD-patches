#!/usr/bin/env python3
"""Same-image A3 runtime comparisons using the existing locked real-map runner."""
import argparse
import hashlib
import json
import os
import pathlib
import time

import passenger_precomputed_real_map as real


def main():
    parser = argparse.ArgumentParser(__doc__)
    for name in ("binary", "package", "input", "lang", "output"):
        parser.add_argument(name, type=pathlib.Path)
    parser.add_argument("--profile", action="store_true", help="Counter-enabled correctness pair; excluded from counter-free performance")
    parser.add_argument("--resume", action="store_true", help="Reuse successful fixed-image trials after another task acquired the shared lock")
    parser.add_argument("--observe-host", action="store_true", help="Record existing host CPU, main-thread and power observations")
    parser.add_argument("--measurement-wait", type=int, default=60)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    modes = [("normal-old", True), ("normal-new", False)]
    if not args.profile:
        modes += [("reverse-new", False), ("reverse-old", True)]
    original = {key: os.environ.get(key) for key in ("OPENTTD_RETIREMENT_PROFILE", "OPENTTD_RETIREMENT_REFERENCE")}
    canonical, results = None, []
    digest = hashlib.sha256(args.binary.read_bytes()).hexdigest()
    input_digest = hashlib.sha256(args.input.read_bytes()).hexdigest()
    try:
        for name, reference in modes:
            for key, enabled in (("OPENTTD_RETIREMENT_PROFILE", args.profile), ("OPENTTD_RETIREMENT_REFERENCE", reference)):
                if enabled:
                    os.environ[key] = "1"
                else:
                    os.environ.pop(key, None)
            output = args.output / name
            options = argparse.Namespace(binary=args.binary, package=args.package, input=args.input, lang=args.lang, output=output,
                ticks=2048, timeout=1800, resume=True, quality_probe=False, fixture_timetable=False, fixture_estimated=False,
                cold_precomputed=False, warm_precomputed=False, rebuild_precomputed=True, precomputed=True,
                unpause_before_save=False, validation_only=False, main_thread_affinity_mask=1, wait_background_ready=True, observe_host=args.observe_host,
                measurement_wait=args.measurement_wait)
            record = output / "process.json"
            reused = args.resume and record.is_file()
            if reused:
                saved = json.loads(record.read_text(encoding="utf-8"))
                assert saved["exit_code"] == 0 and saved["simulation_timing_valid"], (name, "Preserve unsuccessful attempts; use a new output root")
            else:
                if args.resume and output.exists():
                    # Lock acquisition can leave just this empty directory.
                    assert output.resolve().parent == args.output.resolve() and not any(output.iterdir())
                    output.rmdir()
                real.run(options)
            process = json.loads((output / "process.json").read_text(encoding="utf-8"))
            assert process["binary_sha256"] == digest
            assert process["input_sha256"] == input_digest
            assert process["retirement_profile"] == args.profile and process["retirement_reference"] == reference
            assert bool(process.get("host_observation")) == args.observe_host
            state = json.loads((output / "save-integrity.json").read_text(encoding="utf-8"))
            if canonical is None:
                canonical = state
            assert state == canonical, (name, {key: [canonical.get(key), state.get(key)] for key in canonical.keys() | state.keys() if canonical.get(key) != state.get(key)})
            results.append(dict(name=name, path=str(output.resolve()), canonical_equal=True, simulation_timing_valid=process["simulation_timing_valid"]))
            (args.output / "canonical-comparison.json").write_text(json.dumps(dict(binary_sha256=digest, profile=args.profile, runs=results, canonical=canonical), indent=2) + "\n", encoding="utf-8")
            if not reused:
                # The real-map runner has released the shared lock. Give a
                # waiting task an acquisition opportunity before the next case.
                time.sleep(10)
        if not args.profile:
            real.summarize(argparse.Namespace(output=args.output / "timing-summary.json", runs=[args.output / name for name, _ in modes]))
    finally:
        for key, value in original.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


if __name__ == "__main__":
    main()
