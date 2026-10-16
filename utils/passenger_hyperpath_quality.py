#!/usr/bin/env python3
"""Run an exact-tick smoke test, then matched previous/Hyperpath transport.

The copied measurement executable observes ordinary and focused samples in
separate namespaces. Runs are sequential and use the shared measurement lock.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(__doc__)
    for name in ("binary", "package", "input", "output", "lang"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--ticks", type=int, default=24576)
    parser.add_argument("--timeout", type=int, default=3600)
    parser.add_argument("--measurement-wait", type=int, default=1800)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, OPENTTD_PASSENGER_TRACE="1", OPENTTD_PASSENGER_CHECKPOINT_TICK="4096")
    env.pop("OPENTTD_PASSENGER_FOCUS_TRACE", None)
    for name, save, ticks, hyperpath in (
        ("driver-smoke", root / "regression/stationlist/test.sav", 64, True),
        ("previous", args.input, args.ticks, False),
        ("hyperpath", args.input, args.ticks, True),
    ):
        command = [sys.executable, "-B", str(root / "utils/passenger_precomputed_real_map.py"), "run",
                   str(args.binary.resolve()), str(args.package.resolve()), str((args.output / name).resolve()),
                   "--input", str(save.resolve()), "--lang", str(args.lang.resolve()), "--ticks", str(ticks),
                   "--timeout", str(args.timeout), "--measurement-wait", str(args.measurement_wait),
                   "--precomputed", "--hyperpath" if hyperpath else "--no-hyperpath", "--warm-precomputed", "--cold-precomputed"]
        print(json.dumps(dict(run=name, command=command)), flush=True)
        subprocess.run(command, env=env, check=True)


if __name__ == "__main__":
    main()
