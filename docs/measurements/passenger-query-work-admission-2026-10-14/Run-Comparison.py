"""Serial matched real-map admission runs, preserving input, cache scope and CPU reservation."""
import argparse
import importlib.util
import json
import os
import pathlib
import re
import tempfile
from types import SimpleNamespace

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("runner", HERE.parent / "passenger-routing-profile-2026-10-13/Run-Profile.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("build", type=pathlib.Path)
    p.add_argument("package", type=pathlib.Path)
    p.add_argument("--output", default="comparison")
    p.add_argument("--ticks", type=int, default=8192)
    p.add_argument("--variants", nargs="+", default=["control", "q32-t49152", "q64-t49152", "q32-t32768"])
    p.add_argument("--repetitions", type=int, default=1)
    p.add_argument("--cold", action="store_true")
    p.add_argument("--resume", action="store_true")
    a = p.parse_args()
    build, package = a.build.resolve(), a.package.resolve()
    output = build / a.output
    output.mkdir(exist_ok=False)
    lock_path = pathlib.Path(tempfile.gettempdir()) / "openttd-passenger-routing-measurement.lock"
    lock = runner.acquire_measurement_lock(lock_path, 3600)
    try:
        os.write(lock, json.dumps({"pid": os.getpid(), "output": str(output), "task": "Query work admission"}).encode())
        manifests = []
        for variant in a.variants:
            binary = build / f"openttd-{variant}.exe"
            metadata = json.loads(binary.with_suffix(".instrumentation.json").read_text(encoding="utf-8"))
            args = SimpleNamespace(package=package, binary=binary, build=build, output=output / variant,
                label=variant, revision=metadata["source_revision"], kind="minimal", ticks=a.ticks,
                phase="continuation" if a.resume else f"initial_{a.ticks}", repetitions=a.repetitions,
                modes=["cold" if a.cold else "on"], cache_off_scope="all", resume_settings=a.resume,
                wait_idle_seconds=3600, idle_grace_seconds=90)
            runner.run_locked(args)
            manifest = json.loads((args.output / "manifest.json").read_text(encoding="utf-8"))
            for run in manifest["runs"]:
                console = (args.output / run["directory"] / (run["id"] + "-console.txt")).read_text(encoding="utf-8-sig")
                ticks = [int(x) for x in re.findall(r"^State ticks: (-?\d+)", console, re.M)]
                if len(ticks) < 2 or ticks[-1] - ticks[0] != a.ticks:
                    raise ValueError(f"Wrong simulation interval: {variant}: {ticks}")
            manifests.append(str(args.output / "manifest.json"))
            runner.dump(output / "sequence.json", {"manifests": manifests, "status": "running"})
        runner.dump(output / "sequence.json", {"manifests": manifests, "status": "complete"})
    finally:
        os.close(lock)
        lock_path.unlink()


if __name__ == "__main__":
    main()
