"""Serial real-map A/B: identical input/assets/ticks, synchronized method setting.

Build and test first. A process census, shared lock, and idle grace protect CPU
measurements. Quality comparison is a separate stopped-state console command.
No OS time or measurement preference changes the planner's work allowance.
"""
import argparse
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
PROFILES = ROOT / "docs/measurements/passenger-routing-profile-2026-10-13"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


profile = module("profile_runner", PROFILES / "Run-Profile.py")
summarize = module("profile_summary", PROFILES / "Summarize-Profile.py")


def run(args):
    args.output.mkdir(parents=True, exist_ok=False)
    package = json.loads((args.package / "package.json").read_text(encoding="utf-8"))
    if profile.sha256(args.package / "input.sav") != package["input_sha256"]:
        raise ValueError("Input save changed")
    if profile.inventory(args.package) != package["assets"]:
        raise ValueError("Frozen assets changed")
    manifest = {"input": package, "binary_sha256": profile.sha256(args.binary), "ticks": args.ticks,
                "compiler": profile.compiler_info(args.build), "runs": [], "idle_grace_seconds": 90,
                "runner_sha256": profile.sha256(__file__), "harness_sha256": profile.sha256(profile.HARNESS),
                "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "source_files": {p.name: profile.sha256(p) for p in (ROOT / "src").glob("passenger*.*")},
                "budgets": {"work": 65536, "labels": 8192, "memory": 8388608},
                "status": "running"}
    lock_path = pathlib.Path(tempfile.gettempdir()) / "openttd-passenger-routing-measurement.lock"
    while True:
        try:
            lock = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            break
        except FileExistsError:
            time.sleep(5)
    try:
        os.write(lock, json.dumps({"pid": os.getpid(), "output": str(args.output.resolve())}).encode())
        for repetition in range(args.repetitions):
            methods = ["legacy", "rounds"] if repetition % 2 == 0 else ["rounds", "legacy"]
            for method in methods:
                idle_since = None
                while idle_since is None or time.monotonic() - idle_since < 90:
                    busy = profile.competing_processes()
                    if busy:
                        idle_since = None
                    elif idle_since is None:
                        idle_since = time.monotonic()
                    time.sleep(5)
                label = f"{method}-{repetition + 1}"
                directory = args.output / label
                directory.mkdir()
                for name in profile.ASSET_DIRS:
                    if (args.package / name).exists():
                        shutil.copytree(args.package / name, directory / name, copy_function=profile.hardlink_or_copy)
                # Updated language IDs, identical in every A/B run. Never modify hardlinks.
                language_target = (directory / "lang").resolve()
                if not language_target.is_relative_to(args.output.resolve()):
                    raise ValueError("Language copy escaped the benchmark output")
                shutil.rmtree(language_target)
                shutil.copytree(args.build / "lang", directory / "lang")
                for name in ["input.sav", "openttd.cfg"]:
                    shutil.copy2(args.package / name, directory / name)
                shutil.copy2(args.binary, directory / "openttd.exe")
                print(f"Starting {label}: {args.ticks} ticks", flush=True)
                command = ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(profile.HARNESS),
                           "-RunDirectory", str(directory.resolve()), "-Binary", "openttd.exe", "-Label", label,
                           "-Backend", "periodic", "-Ticks", str(args.ticks), "-TimedMethod", method]
                with (directory / "harness.log").open("w", encoding="utf-8") as log:
                    process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                    try:
                        while process.poll() is None:
                            time.sleep(5)
                            pid_file = directory / f"{label}.pid"
                            owned = pid_file.read_text(encoding="utf-8-sig").strip() if pid_file.exists() else None
                            busy = [p for p in profile.competing_processes() if owned is None or not p.endswith(":" + owned)]
                            if busy:
                                profile.abort_own_game(pid_file, directory, process.pid)
                                raise RuntimeError(f"Contaminated run {label}: {busy}")
                    finally:
                        process.wait(timeout=60)
                    if process.returncode:
                        raise RuntimeError(f"{label} harness failed")
                console = (directory / f"{label}-console.txt").read_text(encoding="utf-8", errors="replace")
                parsed = summarize.parse_console(console)
                parsed["round_outcomes"] = {name: int(value) for name, value in re.findall(r"Timed round outcome (\w+): (\d+)", console)}
                parsed["round_work"] = {name: int(value) for name, value in re.findall(r"Timed round work (\d+): (\d+)", console)}
                row = {"label": label, "method": method, "parsed": parsed, "derived": summarize.derived(parsed),
                       "process": json.loads((directory / f"{label}-process.json").read_text(encoding="utf-8-sig")),
                       "final_save_sha256": profile.sha256(directory / f"{label}-result.sav"),
                       "console_sha256": profile.sha256(directory / f"{label}-console.txt"),
                       "stderr_sha256": profile.sha256(directory / f"{label}-stderr.txt")}
                stderr = (directory / f"{label}-stderr.txt").read_text(encoding="utf-8", errors="replace")
                row["stderr_unique_lines"] = sorted(set(stderr.splitlines()))
                if row["derived"]["completed_ticks"] != args.ticks:
                    raise RuntimeError("Incorrect simulation interval")
                manifest["runs"].append(row)
                profile.dump(args.output / "manifest.json", manifest)
                print(f"Completed {label}: {row['derived']['simulation_mean_ms']:.3f} ms/tick", flush=True)
        manifest["status"] = "complete"
    finally:
        profile.dump(args.output / "manifest.json", manifest)
        os.close(lock)
        if lock_path.exists() and json.loads(lock_path.read_text(encoding="utf-8"))["pid"] == os.getpid():
            lock_path.unlink()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["package", "binary", "build", "output"]:
        parser.add_argument("--" + name, type=pathlib.Path, required=True)
    parser.add_argument("--ticks", type=int, default=8192)
    parser.add_argument("--repetitions", type=int, default=2)
    run(parser.parse_args())
