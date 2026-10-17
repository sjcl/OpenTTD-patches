#!/usr/bin/env python3
"""Summarize retirement observations without silently admitting noisy runs."""
import argparse
import json
import math
import statistics


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("run")
    args = parser.parse_args()
    from pathlib import Path
    directory = Path(args.run)
    process = json.loads((directory / "process.json").read_text(encoding="utf-8"))
    clock = json.loads((directory / "actual-ticks.json").read_text(encoding="utf-8"))
    samples = clock["samples"]
    assert len(samples) == process["requested_ticks"] == clock["end_tick"] - clock["start_tick"]
    assert all(len(s) == 5 for s in samples)
    ordered = sorted(samples, key=lambda s: (s[1], s[0]))
    # The initial diagnostic predates the copied console reset hook. Preserve its
    # raw observations and report the warm-up contamination explicitly.
    setup = samples[0] if samples[0][4] > 1 else None
    released = [s for s in samples if s[4] and s is not setup]
    def distribution(values):
        values = sorted(values)
        return dict(mean=statistics.mean(values), p95=values[math.ceil(len(values) * .95) - 1],
                    p99=values[math.ceil(len(values) * .99) - 1], maximum=values[-1]) if values else None
    def point(q):
        tick, simulation, journey, release, calls = ordered[math.ceil(len(ordered) * q) - 1]
        return dict(tick=tick, simulation_us=simulation, journey_us=journey, retirement_us=release / 1000, calls=calls)
    result = dict(simulation_timing_valid=process["simulation_timing_valid"],
                  profile_enabled=process.get("retirement_profile", True), ticks=len(samples), setup_observation=setup, release_ticks=len(released), calls=sum(s[4] for s in released),
                  simulation_us=distribution([s[1] for s in samples]),
                  retirement_us=distribution([s[3] / 1000 for s in released]),
                  percentiles={"p95": point(.95), "p99": point(.99)},
                  tail_release_ticks={str(q): sum(s[4] != 0 and s is not setup for s in ordered[math.ceil(len(ordered)*q)-1:]) for q in (.95, .99)},
                  worst_retirements=[dict(tick=s[0], simulation_us=s[1], retirement_us=s[3]/1000, calls=s[4])
                                     for s in sorted(released, key=lambda s: s[3], reverse=True)[:10]])
    (directory / "retirement-summary.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
