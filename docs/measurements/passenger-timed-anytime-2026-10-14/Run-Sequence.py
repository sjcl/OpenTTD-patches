"""Run matched control/Anytime/quality/continuation serially under one CPU lock."""
import argparse
import copy
import importlib.util
import json
import os
import pathlib
import shutil
import tempfile
from types import SimpleNamespace

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
spec = importlib.util.spec_from_file_location("runner", HERE.parent / "passenger-routing-profile-2026-10-13/Run-Profile.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("build", type=pathlib.Path)
p.add_argument("--output", default="sequence")
p.add_argument("--quality-ticks", type=int, default=2048)
a = p.parse_args()
build = a.build.resolve()
output = build / a.output
output.mkdir(exist_ok=False)
for mode in ("legacy", "anytime", "quality"):
    if not (build / f"openttd-{mode}.exe").is_file():
        raise ValueError("Missing variant: " + mode)
lock_path = pathlib.Path(tempfile.gettempdir()) / "openttd-passenger-routing-measurement.lock"
lock = runner.acquire_measurement_lock(lock_path, 3600)
try:
    os.write(lock, json.dumps({"pid": os.getpid(), "output": str(output), "task": "Timed Anytime matched comparison"}).encode())
    manifests = []

    def run(label, mode, modes, repetitions=1, ticks=8192, package=None, phase="initial_8192", resume=False):
        binary = build / f"openttd-{mode}.exe"
        metadata = json.loads(binary.with_suffix(".instrumentation.json").read_text(encoding="utf-8"))
        args = SimpleNamespace(package=package or build / "package", binary=binary, build=build,
            output=output / label, label=label, revision=metadata["source_revision"], kind="minimal",
            phase=phase, ticks=ticks, repetitions=repetitions, modes=modes, cache_off_scope="all",
            resume_settings=resume, wait_idle_seconds=3600, idle_grace_seconds=90)
        runner.run_locked(args)
        manifests.append(str(args.output / "manifest.json"))
        runner.dump(output / "sequence.json", {"manifests": manifests, "status": "running"})

    run("baseline", "legacy", ["on"])
    run("anytime", "anytime", ["on"], repetitions=2)
    run("anytime-cold", "anytime", ["cold"])
    run("quality", "quality", ["on"], ticks=a.quality_ticks, phase="quality_sample")
    checkpoint = output / "continuation-package"
    checkpoint.mkdir()
    for directory in runner.ASSET_DIRS:
        if (build / "package" / directory).exists():
            shutil.copytree(build / "package" / directory, checkpoint / directory, copy_function=runner.hardlink_or_copy)
    shutil.copy2(output / "anytime/on-1/on-1-result.sav", checkpoint / "input.sav")
    shutil.copy2(build / "package/openttd.cfg", checkpoint / "openttd.cfg")
    metadata = copy.deepcopy(json.loads((build / "package/package.json").read_text(encoding="utf-8")))
    metadata["input_sha256"] = runner.sha256(checkpoint / "input.sav")
    metadata["checkpoint_after_ticks"] = 8192
    runner.dump(checkpoint / "package.json", metadata)
    run("continuation", "anytime", ["on"], package=checkpoint, phase="continuation_8192", resume=True)
    runner.dump(output / "sequence.json", {"manifests": manifests, "status": "complete"})
finally:
    os.close(lock)
    lock_path.unlink()
