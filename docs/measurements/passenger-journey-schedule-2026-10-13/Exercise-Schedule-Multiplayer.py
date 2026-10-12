"""Isolated loopback test; requires prepared asset junctions in server/client dirs.

Run after all performance measurements. It uses the same binary and real map,
joins once before database discovery and again during published operation, and
keeps the server private on 127.0.0.1. No chat messages or external services.
"""
import argparse
import ctypes
import json
import pathlib
import re
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument("run", type=pathlib.Path)
p.add_argument("root", type=pathlib.Path)
p.add_argument("--binary", default="openttd-periodic.exe")
p.add_argument("--backend", choices=["periodic", "legacy", "off"], default="periodic")
p.add_argument("--duration", type=float, default=35)
p.add_argument("--input-save", default="input.sav")
p.add_argument("--fixture", action="store_true")
a = p.parse_args()
run, root = a.run.resolve(), a.root.resolve()
root.mkdir(parents=True, exist_ok=True)
binary = run / a.binary
processes = []
handles = []
affinities = {}

def text(path):
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""

def wait_for(predicate, label, seconds=180):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        if any(proc.poll() is not None for proc in processes):
            raise RuntimeError(f"A child exited while waiting for {label}: {[proc.poll() for proc in processes]}")
        time.sleep(.25)
    raise TimeoutError(label)

def prepare(name):
    directory = root / name
    (directory / "scripts").mkdir(parents=True, exist_ok=True)
    (directory / "console.txt").write_text("", encoding="utf-8")
    (directory / "openttd.cfg").write_text(
        f"[misc]\nlanguage = english.lng\nautosave = off\nthreaded_saves = false\n"
        f"[network]\nclient_name = periodic-{name}\nserver_game_type = local\n"
        "server_port = 17493\npause_on_join = true\nmin_active_clients = 0\n"
        "[graphicsset]\nname = OpenGFX\n", encoding="utf-8")
    return directory

def start(directory, args):
    out = (directory / "stdout.txt").open("w", encoding="utf-8")
    err = (directory / "stderr.txt").open("w", encoding="utf-8")
    handles.extend([out, err])
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    proc = subprocess.Popen([str(binary), "-c", "openttd.cfg", "-s", "null", "-m", "null",
                             "-x", "-d", "net=3,desync=1", *args], cwd=directory,
                            stdin=subprocess.PIPE, stdout=out, stderr=err,
                            startupinfo=startup, creationflags=subprocess.CREATE_NEW_CONSOLE,
                            text=True, encoding="utf-8")
    processes.append(proc)
    return proc

def command(proc, line):
    proc.stdin.write(line + "\n")
    proc.stdin.flush()

def client(name):
    directory = prepare(name)
    log = (directory / "console.txt").as_posix()
    finish = (directory / "scripts" / "finish.scr").as_posix()
    (directory / "scripts" / "game_start.scr").write_text(
        f"script {log}\ngetfulldate\npassenger_journey_stats inventory\n"
        f"passenger_journey_stats total\nsave {directory.as_posix()}/joined\n"
        f"schedule on-next-calendar-month {finish}\nscript\n", encoding="utf-8")
    (directory / "scripts" / "finish.scr").write_text(
        f"script {log}\ngetfulldate\npassenger_journey_stats inventory\n"
        "passenger_journey_stats total\nscript\n", encoding="utf-8")
    proc = start(directory, ["-v", "null:until_exit", "-n", "127.0.0.1:17493#255"])
    # A single core deliberately differs from the unrestricted host's speed.
    kernel = ctypes.windll.kernel32
    kernel.OpenProcess.restype = ctypes.c_void_p
    handle = kernel.OpenProcess(0x200 | 0x400, False, proc.pid)
    process_mask, system_mask = ctypes.c_size_t(), ctypes.c_size_t()
    applied = False
    mask = 0
    if handle and kernel.GetProcessAffinityMask(ctypes.c_void_p(handle), ctypes.byref(process_mask), ctypes.byref(system_mask)):
        mask = process_mask.value & -process_mask.value
        applied = bool(kernel.SetProcessAffinityMask(ctypes.c_void_p(handle), ctypes.c_size_t(mask)))
        kernel.CloseHandle(ctypes.c_void_p(handle))
    affinities[name] = {"mask": mask, "applied": applied}
    return proc, directory

try:
    if (root / "result.json").exists():
        (root / "result.json").unlink()
    server_dir = prepare("server")
    server_log = server_dir / "console.txt"
    (server_dir / "scripts" / "game_start.scr").write_text(
        f"script {server_log.as_posix()}\nsetting linkgraph.distribution_pax 2\n"
        "setting linkgraph.passenger_od_assignment true\n"
        f"setting linkgraph.passenger_journey_routing {'false' if a.backend == 'off' else 'true'}\n"
        f"setting linkgraph.passenger_journey_periodic {'true' if a.backend == 'periodic' else 'false'}\nsetting network.sync_freq 1\n"
        "setting linkgraph.recalc_interval 8\nsetting linkgraph.recalc_time 32\npause\n"
        + ("passenger_periodic_fixture estimated\n" if a.fixture else "") +
        "getfulldate\npassenger_journey_stats total\nscript\n", encoding="utf-8")
    server = start(server_dir, ["-D", "127.0.0.1:17493", "-g", str(run / a.input_save)])
    wait_for(lambda: "Simulation tick time:" in text(server_log), "server loaded")
    if a.fixture and not re.search(r"Periodic test fixture: services=[1-9]", text(server_log)):
        raise RuntimeError("No active passenger fixture was prepared")
    first, first_dir = client("client-initial")
    wait_for(lambda: "Client #2" in text(server_dir / "stderr.txt") and "joined as" in text(server_dir / "stderr.txt"), "first join")
    command(server, "unpause")
    # Join the second client after initial discovery has had time to publish.
    time.sleep(12)
    second, second_dir = client("client-late")
    wait_for(lambda: "Client #3" in text(server_dir / "stderr.txt") and text(server_dir / "stderr.txt").count("joined as") >= 2, "late join")
    # Leave both clients active for at least ten sync intervals.
    time.sleep(a.duration)
    command(server, "pause")
    time.sleep(2)
    command(server, f"script {server_log.as_posix()}")
    command(server, "getfulldate")
    command(server, "clients")
    command(server, "passenger_journey_stats inventory")
    command(server, "passenger_journey_stats total")
    command(server, f"save {server_dir.as_posix()}/final")
    command(server, "script")
    time.sleep(4)
    logs = [text(directory / "stderr.txt") for directory in [server_dir, first_dir, second_dir]]
    bad = any(re.search(r"Sync error|sync_err:|Assertion failed|desync detected", log, re.I) for log in logs)
    result = {"joined_clients": text(server_dir / "stderr.txt").count("joined as"),
              "backend": a.backend, "binary": a.binary,
              "input_save": a.input_save, "fixture": a.fixture,
              "detected_sync_failure": bad, "client_exit_codes": [first.poll(), second.poll()],
              "server_state_ticks": re.findall(r"State ticks: (\d+)", text(server_log)),
              "single_core_clients": len(affinities) == 2 and all(value["applied"] for value in affinities.values()),
              "affinity": affinities, "sync_frequency": 1,
              "loopback_address": "127.0.0.1:17493"}
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2), flush=True)
    if bad or result["joined_clients"] < 2:
        raise RuntimeError("multiplayer verification failed")
finally:
    for proc in processes:
        if proc.poll() is None:
            if proc is processes[0]:
                try:
                    command(proc, "quit")
                    proc.wait(timeout=15)
                except (OSError, subprocess.TimeoutExpired):
                    proc.terminate()
            else:
                proc.terminate()
        proc.wait(timeout=30)
    for handle in handles:
        handle.close()
