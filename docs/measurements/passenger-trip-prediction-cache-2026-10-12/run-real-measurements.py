import os
import pathlib
import subprocess
import time
import hashlib
import json
import re

root = pathlib.Path(__file__).resolve().parent
run = root / "real-run"
binary = root / "Release" / "trip_measurement.exe"
binary.write_bytes((root / "Release" / "openttd.exe").read_bytes())
editbin = pathlib.Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/editbin.exe")
subprocess.run([str(editbin), "/nologo", "/subsystem:console", str(binary)], check=True, capture_output=True)
results = []
for repeat in range(3):
    for enabled in ([False, True] if repeat % 2 == 0 else [True, False]):
        stage = f"run-{repeat}-{'enabled' if enabled else 'disabled'}"
        output = run / stage
        output.mkdir(exist_ok=True)
        commands = [f'script "{output.as_posix()}/console.log"', "getfulldate", "passenger_journey_stats inventory",
                    "setting linkgraph.passenger_journey_routing true", "setting linkgraph.passenger_od_assignment true"]
        if repeat == 0:
            commands += ["passenger_service_capture_benchmark 0 0 100"]
        commands += ["passenger_journey_stats reset", "unpause", "step 0"]
        (output / "console.log").write_text("", encoding="utf-8")
        (run / "scripts" / "game_start.scr").write_text("\n".join(commands) + "\n", encoding="utf-8")
        env = os.environ.copy()
        env["PASSENGER_TRIP_MEASURE_RESULT"] = (output / "result").as_posix()
        if not enabled:
            env["PASSENGER_TRIP_MEASURE_DISABLED"] = "1"
        else:
            env.pop("PASSENGER_TRIP_MEASURE_DISABLED", None)
        started = time.perf_counter()
        with (output / "stdout.log").open("wb") as stdout, (output / "stderr.log").open("wb") as stderr:
            completed = subprocess.run([str(binary), "-x", "-c", str(run / "openttd.cfg"), "-g", str(run / "input.sav"),
                                        "-vnull:ticks=33", "-snull", "-mnull", "-d", "sl=1"], cwd=run, env=env, stdout=stdout, stderr=stderr, timeout=120)
        result = dict(run=repeat, enabled=enabled, exit_code=completed.returncode, wall_seconds=time.perf_counter()-started)
        console = (output / "console.log").read_text(encoding="utf-8", errors="replace")
        samples = re.findall(r"Simulation tick time: completed_ticks=(\d+)", console)
        if not samples or int(samples[-1]) == 0:
            raise RuntimeError(f"{stage}: no completed simulation ticks")
        result["simulation_tick_samples"] = int(samples[-1])
        diagnostics = (output / "stderr.log").read_text(encoding="utf-8", errors="replace")
        if "not found; checksum" in diagnostics or "uncorrectably invalid" in diagnostics:
            raise RuntimeError(f"{stage}: missing assets/removed vehicles")
        save = output / "result.sav"
        if save.exists():
            result["save_sha256"] = hashlib.sha256(save.read_bytes()).hexdigest()
        results.append(result)
        print(json.dumps(result), flush=True)
        (run / "runs.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
        if completed.returncode:
            raise RuntimeError(f"{stage} failed")
