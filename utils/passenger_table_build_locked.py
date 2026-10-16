#!/usr/bin/env python3
"""Reserve the shared host measurement lock for CMake builds and full tests."""
import argparse
import json
import os
import pathlib
import subprocess
import tempfile

from passenger_precomputed_real_map import PROFILE, ROOT


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("build", type=pathlib.Path)
    parser.add_argument("log", type=pathlib.Path)
    parser.add_argument("--include", help="Reconfigure CMAKE_PROJECT_INCLUDE; empty restores product TUs")
    parser.add_argument("--target", default="openttd_test")
    parser.add_argument("--all-tests", action="store_true")
    parser.add_argument("--runtime-reference", help="Prepare the copied runtime with a pre-A3 reference after configuration")
    parser.add_argument("--measurement-wait", type=int, default=60)
    args = parser.parse_args()
    build, log = args.build.resolve(), args.log.resolve()
    log.parent.mkdir(parents=True, exist_ok=True)
    path = pathlib.Path(tempfile.gettempdir()) / "openttd-passenger-routing-measurement.lock"
    lock = PROFILE.acquire_measurement_lock(path, args.measurement_wait)
    try:
        os.write(lock, json.dumps(dict(pid=os.getpid(), purpose="build/test", output=str(log))).encode())
        PROFILE.wait_for_idle(60, 5)
        with log.open("wb") as stream:
            def run(command):
                print(json.dumps(command), flush=True)
                subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True)
            if args.include is not None:
                run(["cmake", "-S", str(ROOT), "-B", str(build), "-DCMAKE_PROJECT_INCLUDE=" + args.include])
            if args.runtime_reference:
                import sys
                run([sys.executable, "-B", str(ROOT / "utils/passenger_estimated_prepare.py"), str(ROOT), str(build), args.runtime_reference, "--cpu"])
                run([sys.executable, "-B", str(ROOT / "utils/passenger_table_retirement_prepare.py"), str(ROOT), str(build), "--reference", args.runtime_reference])
            run(["cmake", "--build", str(build), "--config", "Release", "--target", args.target, "-j", "8"])
        if args.all_tests:
            with log.with_suffix(".tests.log").open("wb") as stream:
                subprocess.run([str(build / "Release/openttd_test.exe")], cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True)
    finally:
        os.close(lock)
        path.unlink()


if __name__ == "__main__":
    main()
