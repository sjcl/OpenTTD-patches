"""Run matched control/Anytime/quality/continuation serially under one CPU lock."""
import argparse
import copy
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
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
p.add_argument("--quality-label", default="quality")
p.add_argument("--rebuild-quality", action="store_true")
p.add_argument("--rebuild-continuation", action="store_true")
p.add_argument("--build-variants", action="store_true", help="Build and test all variants while holding the shared CPU reservation")
p.add_argument("--dependencies", type=pathlib.Path)
p.add_argument("--start-at", choices=("original", "baseline", "anytime", "cold", "quality", "continuation"), default="baseline")
p.add_argument("--groups", nargs="+", choices=("original", "baseline", "anytime", "cold", "quality", "continuation"),
               help="Run only these groups, in the standard order")
p.add_argument("--checkpoint", type=pathlib.Path, help="First Anytime result when resuming a later measurement group")
a = p.parse_args()
if (a.rebuild_quality or a.rebuild_continuation or a.build_variants) and a.dependencies is None:
    p.error("Variant rebuild requires --dependencies")
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
    for rebuild_mode in (["all"] if a.build_variants else []) + (["quality"] if a.rebuild_quality else []) + (["continuation"] if a.rebuild_continuation else []):
        runner.wait_for_idle(3600, grace=20)
        quote = lambda path: "'" + str(path).replace("'", "''") + "'"
        command = "& " + quote(HERE / "Build-Variants.ps1") + " -Source " + quote(ROOT) + " -Build " + quote(build) + " -Dependencies " + quote(a.dependencies.resolve())
        if rebuild_mode != "all":
            command += " -Modes " + rebuild_mode
        with (build / ("rebuild-" + rebuild_mode + ".log")).open("w", encoding="utf-8") as stream:
            subprocess.run([runner.POWERSHELL, "-NoProfile", "-NonInteractive", "-Command", command],
                           cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True)
    if a.build_variants:
        for script, log in ((HERE / "Test-Anytime.py", "python-anytime-final.log"),
                            (HERE.parent / "passenger-routing-profile-2026-10-13/Test-Profile.py", "python-profile-final.log")):
            with (build / log).open("w", encoding="utf-8") as stream:
                subprocess.run([__import__("sys").executable, "-B", str(script)], stdout=stream, stderr=subprocess.STDOUT, check=True)
    manifests = []

    def run(label, mode, modes, repetitions=1, ticks=8192, package=None, phase="initial_8192", resume=False):
        binary = build / f"openttd-{mode}.exe"
        metadata = json.loads(binary.with_suffix(".instrumentation.json").read_text(encoding="utf-8"))
        args = SimpleNamespace(package=package or build / "package", binary=binary, build=build,
            output=output / label, label=label, revision=metadata["source_revision"], kind="minimal",
            phase=phase, ticks=ticks, repetitions=repetitions, modes=modes, cache_off_scope="all",
            resume_settings=resume, wait_idle_seconds=3600, idle_grace_seconds=90)
        runner.run_locked(args)
        console = (args.output / (modes[0] + "-1") / (modes[0] + "-1-console.txt")).read_text(encoding="utf-8-sig")
        state_ticks = [int(x) for x in re.findall(r"^State ticks: (-?\d+)", console, re.M)]
        if len(state_ticks) < 2 or state_ticks[-1] - state_ticks[0] != ticks:
            raise ValueError("Simulation did not advance the requested StateTicks; exclude this run")
        if mode in ("legacy", "original"):
            console = (args.output / "on-1/on-1-console.txt").read_text(encoding="utf-8-sig")
            rescues = re.findall(r"Timed result states:.*?rescued_unpopped=(\d+)", console)
            if not rescues or int(rescues[-1]) != 0:
                raise ValueError("Control used Anytime completion; exclude this run")
        if mode == "quality" and not (args.output / "on-1/anytime-quality.jsonl").is_file():
            raise ValueError("Quality probe was not linked or did not write samples")
        manifests.append(str(args.output / "manifest.json"))
        runner.dump(output / "sequence.json", {"manifests": manifests, "status": "running"})

    order = ("original", "baseline", "anytime", "cold", "quality", "continuation")
    groups = a.groups or order[order.index(a.start_at):]
    if "original" in groups:
        run("original", "original", ["on"])
    if "baseline" in groups:
        run("baseline", "legacy", ["on"])
    if "anytime" in groups:
        run("anytime", "anytime", ["on"], repetitions=2)
    if "cold" in groups:
        run("anytime-cold", "anytime", ["cold"])
    if "quality" in groups:
        run(a.quality_label, "quality", ["on"], ticks=a.quality_ticks, phase="quality_sample")
    if "continuation" in groups:
        checkpoint = output / "continuation-package"
        checkpoint.mkdir()
        for directory in runner.ASSET_DIRS:
            if (build / "package" / directory).exists():
                shutil.copytree(build / "package" / directory, checkpoint / directory, copy_function=runner.hardlink_or_copy)
        source_checkpoint = a.checkpoint or output / "anytime/on-1/on-1-result.sav"
        shutil.copy2(source_checkpoint, checkpoint / "input.sav")
        shutil.copy2(build / "package/openttd.cfg", checkpoint / "openttd.cfg")
        metadata = copy.deepcopy(json.loads((build / "package/package.json").read_text(encoding="utf-8")))
        metadata["input_sha256"] = runner.sha256(checkpoint / "input.sav")
        metadata["checkpoint_after_ticks"] = 8192
        runner.dump(checkpoint / "package.json", metadata)
        continuation_mode = "continuation" if (build / "openttd-continuation.exe").is_file() else "anytime"
        run("continuation", continuation_mode, ["on"], package=checkpoint, phase="continuation_8192", resume=True)
    runner.dump(output / "sequence.json", {"manifests": manifests, "status": "complete"})
finally:
    os.close(lock)
    lock_path.unlink()
