"""Verify real-map joins and the uninstrumented product after timed comparisons."""
import argparse
import datetime
import importlib.util
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
SOURCE = HERE.parents[2]
spec = importlib.util.spec_from_file_location("runner", HERE.parent / "passenger-routing-profile-2026-10-13/Run-Profile.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("build", type=pathlib.Path)
p.add_argument("checkpoint", type=pathlib.Path)
p.add_argument("dependencies", type=pathlib.Path)
p.add_argument("--wait-seconds", type=int, default=7200)
p.add_argument("--after-sequence", type=pathlib.Path, help="Wait for this sequence.json to complete before acquiring the CPU lock")
p.add_argument("--multiplayer-input", type=pathlib.Path)
p.add_argument("--multiplayer-directory", default="mp-arena")
a = p.parse_args()
build, checkpoint = a.build.resolve(), a.checkpoint.resolve()
deadline = time.monotonic() + a.wait_seconds
def ready():
    if not checkpoint.is_file():
        return False
    if a.after_sequence is None:
        return True
    return a.after_sequence.is_file() and json.loads(a.after_sequence.read_text(encoding="utf-8"))["status"] == "complete"


while not ready():
    if time.monotonic() >= deadline:
        raise TimeoutError("Anytime checkpoint has not been produced")
    time.sleep(10)
lock_path = pathlib.Path(tempfile.gettempdir()) / "openttd-passenger-routing-measurement.lock"
lock = runner.acquire_measurement_lock(lock_path, a.wait_seconds)
try:
    os.write(lock, json.dumps({"pid": os.getpid(), "task": "Timed Anytime final joins and product verification", "output": str(build)}).encode())
    runner.wait_for_idle(a.wait_seconds, grace=20)
    started = datetime.datetime.now(datetime.timezone.utc).isoformat()

    def command(args, log):
        with (build / log).open("w", encoding="utf-8") as stream:
            subprocess.run(list(map(str, args)), cwd=SOURCE, stdout=stream, stderr=subprocess.STDOUT, check=True,
                           env={**os.environ, "MSBUILDDISABLENODEREUSE": "1"})

    multiplayer_save = a.multiplayer_input.resolve() if a.multiplayer_input else checkpoint
    multiplayer_root = build / a.multiplayer_directory
    command([sys.executable, HERE.parent / "passenger-timed-search-arena-2026-10-13/Prepare-Multiplayer.py", build,
             "--directory", a.multiplayer_directory], "prepare-anytime-multiplayer.log")
    command([sys.executable, build / "Exercise-Pruning-Multiplayer.py", build, multiplayer_root,
             "--binary", "openttd-network.exe", "--backend", "periodic", "--input-save", multiplayer_save, "--duration", "25"], "anytime-multiplayer.log")
    console = (multiplayer_root / "server/console.txt").read_text(encoding="utf-8", errors="replace")
    adoptions = re.findall(r"Timed plan adoption:.*?provisional_packets=(\d+)", console)
    if not adoptions or int(adoptions[-1]) == 0:
        raise RuntimeError("Joined peers did not exercise provisional packet adoption")
    command(["cmake", "-S", SOURCE, "-B", build, "-G", "Visual Studio 17 2022", "-A", "x64",
             "-DCMAKE_PREFIX_PATH=" + str(a.dependencies.resolve()), "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
             "-DOPTION_NO_LTO=ON", "-DOPTION_USE_ASSERTS=ON", "-DOPTION_PASSENGER_BENCHMARK=OFF"], "configure-product.log")
    for name in ("passenger_service_database.obj", "console_cmds.obj", "null_v.obj"):
        (build / "openttd_lib.dir/Release" / name).unlink(missing_ok=True)
    command(["cmake", "--build", build, "--config", "Release", "--target", "openttd", "openttd_test", "regression_files", "--parallel", "8"], "build-product.log")
    command(["ctest", "--test-dir", build, "-C", "Release", "--output-on-failure", "--parallel", "8"], "ctest-product.log")
    runner.dump(build / "final-verification.json", {
        "started_utc": started, "finished_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "source_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=SOURCE, text=True).strip(),
        "checkpoint_sha256": runner.sha256(checkpoint),
        "multiplayer_input_sha256": runner.sha256(multiplayer_save),
        "network_sha256": runner.sha256(build / "openttd-network.exe"),
        "product_sha256": runner.sha256(build / "Release/openttd.exe"),
        "product_instrumented": False, "product_ctest_passed": True,
        "multiplayer_provisional_packets": int(adoptions[-1]),
        "compiler": runner.compiler_info(build),
    })
finally:
    os.close(lock)
    lock_path.unlink()
