"""Serial controlled real-map comparisons, with host process conflict checks."""
import argparse
import importlib.util
import json
import pathlib
import shutil
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("build", type=pathlib.Path)
p.add_argument("--ticks", type=int, default=2048)
p.add_argument("--detail-ticks", type=int, default=512)
p.add_argument("--output", default="comparison")
a = p.parse_args()
build = a.build.resolve()
source = pathlib.Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("runner", source / "docs/measurements/passenger-routing-profile-2026-10-13/Run-Profile.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
package = build / "package"
output = build / a.output
output.mkdir(parents=True, exist_ok=False)
manifest = {"package": json.loads((package / "package.json").read_text(encoding="utf-8")), "runs": [], "ticks": a.ticks}
sequence = [("baseline-1", "baseline", False, a.ticks), ("pruned-1", "pruned", False, a.ticks),
            ("pruned-2", "pruned", False, a.ticks), ("baseline-2", "baseline", False, a.ticks),
            ("cold", "pruned", True, a.ticks), ("baseline-detail", "baseline-detail", False, a.detail_ticks), ("detail", "detail", False, a.detail_ticks)]
try:
    for label, mode, cold, ticks in sequence:
        busy = runner.competing_processes()
        if busy:
            raise RuntimeError("Measurement blocked by other processes: " + ", ".join(busy))
        directory = output / label
        directory.mkdir()
        for name in runner.ASSET_DIRS:
            if (package / name).exists():
                shutil.copytree(package / name, directory / name, copy_function=runner.hardlink_or_copy)
        for name in ("input.sav", "openttd.cfg"):
            shutil.copy2(package / name, directory / name)
        binary = build / f"openttd-{mode}.exe"
        shutil.copy2(binary, directory / "openttd.exe")
        shell = shutil.which("pwsh.exe")
        if shell is None:
            raise RuntimeError("PowerShell 7 is required for the measurement harness")
        command = [shell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                   str(source / "docs/measurements/passenger-departure-index-2026-10-13/Measure-Departures.ps1"),
                   "-RunDirectory", str(directory), "-Binary", "openttd.exe", "-Label", label, "-Ticks", str(ticks)]
        if cold:
            command += ["-DisablePolicies", "-DisableDepartures"]
        print(f"{label}: {ticks} ticks", flush=True)
        with (directory / "harness.txt").open("w", encoding="utf-8") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            contamination = []
            while process.poll() is None:
                time.sleep(5)
                pid_file = directory / f"{label}.pid"
                expected = "openttd:" + pid_file.read_text(encoding="utf-8-sig").strip() if pid_file.exists() else None
                busy = [item for item in runner.competing_processes() if item.lower() != expected]
                if busy:
                    contamination.append(busy)
                    runner.abort_own_game(pid_file, directory, process.pid)
        record = dict(label=label, mode=mode, cold=cold, ticks=ticks, returncode=process.returncode,
                      contamination=contamination, binary_sha256=runner.sha256(binary),
                      save_sha256=runner.sha256(directory / f"{label}-result.sav") if (directory / f"{label}-result.sav").exists() else None)
        manifest["runs"].append(record)
        runner.dump(output / "manifest.json", manifest)
        if process.returncode or contamination:
            raise RuntimeError(f"Invalid run: {label}")
    manifest["status"] = "complete"
except Exception as error:
    manifest["status"] = "failed"
    manifest["error"] = str(error)
    raise
finally:
    runner.dump(output / "manifest.json", manifest)
