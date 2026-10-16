#!/usr/bin/env python3
"""Build and optionally test while respecting the shared measurement lease."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

import passenger_precomputed_real_map as real


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("log", type=Path)
    parser.add_argument("--target", nargs="+", default=["openttd_test", "openttd", "regression_files"])
    parser.add_argument("--ctest", action="store_true")
    parser.add_argument("--rerun-failed", action="store_true")
    parser.add_argument("--database", type=Path, help="Also run the frozen-map strategy comparison under the same lease")
    parser.add_argument("--stations", type=Path)
    parser.add_argument("--lookup-probe", action="store_true")
    parser.add_argument("--client", action="store_true", help="Configure a normal executable for the real null-video MP client too")
    args = parser.parse_args()
    lock_path = Path(tempfile.gettempdir()) / "openttd-passenger-routing-measurement.lock"
    lock = real.PROFILE.acquire_measurement_lock(lock_path, 1800)
    try:
        os.write(lock, json.dumps(dict(pid=os.getpid(), purpose="build/test", output=str(args.build.resolve()))).encode())
        real.PROFILE.wait_for_idle(1800, 5)
        if args.client:
            with args.log.with_suffix('.configure.log').open('wb') as stream:
                subprocess.run(['cmake', '-S', str(Path(__file__).resolve().parents[1]), '-B', str(args.build),
                                '-DOPTION_DEDICATED=OFF', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded'],
                               stdout=stream, stderr=subprocess.STDOUT, check=True)
        with args.log.open("wb") as stream:
            subprocess.run(["cmake", "--build", str(args.build), "--config", "Release", "--target", *args.target, "-j", "6"], stdout=stream, stderr=subprocess.STDOUT, check=True)
        if args.ctest:
            with args.log.with_suffix(".ctest.log").open("wb") as stream:
                subprocess.run(["ctest", "--test-dir", str(args.build), "-C", "Release", "--output-on-failure", "-j", "4",
                                *(['--rerun-failed'] if args.rerun_failed else [])], stdout=stream, stderr=subprocess.STDOUT, check=True)
        probes = []
        if args.lookup_probe:
            probes.append(('lookup', '[.hyperpath-cpu]'))
        if args.database:
            if not args.stations:
                parser.error('--database requires --stations')
            probes.append(('real', '[.hyperpath-real]'))
        env = dict(os.environ)
        if args.database:
            env.update(OPENTTD_TABLE_DATABASE=str(args.database.resolve()), OPENTTD_TABLE_STATIONS=str(args.stations.resolve()))
        for name, tag in probes:
            with args.log.with_suffix('.' + name + '.log').open('wb') as stream:
                subprocess.run([str((args.build / 'Release/openttd_test.exe').resolve()), tag], env=env,
                               stdout=stream, stderr=subprocess.STDOUT, check=True)
    finally:
        os.close(lock)
        lock_path.unlink()


if __name__ == "__main__":
    main()
