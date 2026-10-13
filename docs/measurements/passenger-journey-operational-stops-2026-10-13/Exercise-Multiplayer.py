"""Test release executable code on loopback; a console-header copy supports logging."""
import argparse
import ctypes
import datetime
import hashlib
import json
import pathlib
import re
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument("package", type=pathlib.Path)
p.add_argument("save", type=pathlib.Path)
p.add_argument("root", type=pathlib.Path)
p.add_argument("--duration", type=float, default=35)
p.add_argument("--transfer-timetables", choices=["true", "false", "preserve"], default="preserve")
p.add_argument("--binary", default="openttd.exe")
p.add_argument("--single-core-clients", action="store_true")
p.add_argument("--net-debug", type=int, default=7)
p.add_argument("--desync-debug", type=int, default=1)
p.add_argument("--load-debug", type=int, default=0)
p.add_argument("--late-delay", type=float, default=12)
p.add_argument("--backend", choices=["periodic","legacy","off","preserve"], default="periodic")
p.add_argument("--revision", default="fix/passenger-journey-operational-stops")
a = p.parse_args()
package, save, root = a.package.resolve(), a.save.resolve(), a.root.resolve()
binary = package / a.binary
processes, handles, affinities = [], [], {}
result = {"revision": a.revision, "backend": a.backend, "binary": str(binary),
          "started_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
          "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
          "public_binary_sha256": hashlib.sha256((package / 'openttd.exe').read_bytes()).hexdigest(),
          "input_save_sha256": hashlib.sha256(save.read_bytes()).hexdigest(),
          "transfer_timetables": a.transfer_timetables,
          "loopback_address": "127.0.0.1:17493", "sync_frequency": 1}
result["late_join_delay_seconds"] = a.late_delay
result["late_join_observation_seconds"] = a.duration

def read(path):
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""

def wait_for(predicate, label, seconds=180):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if predicate():
            return
        if any(proc.poll() is not None for proc in processes):
            raise RuntimeError(f"Child exited while waiting for {label}: {[proc.poll() for proc in processes]}")
        time.sleep(.25)
    raise TimeoutError(label)

def prepare(name):
    directory = root / name
    (directory / "scripts").mkdir(parents=True, exist_ok=True)
    (directory / "console.txt").write_text("", encoding="utf-8")
    (directory / "openttd.cfg").write_text(
        "[misc]\nlanguage = english.lng\nautosave = off\nthreaded_saves = false\n"
        f"[network]\nclient_name = release-{name}\nserver_game_type = local\n"
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
                             "-x", "-d", f"net={a.net_debug},desync={a.desync_debug},sl={a.load_debug}", *args], cwd=directory,
                            stdin=subprocess.PIPE, stdout=out, stderr=err,
                            startupinfo=startup, creationflags=subprocess.CREATE_NEW_CONSOLE,
                            text=True, encoding="utf-8")
    processes.append(proc)
    return proc

def command(proc, line):
    proc.stdin.write(line + "\n")
    proc.stdin.flush()

def snapshot(proc, directory):
    command(proc, f"script {(directory / 'console.txt').as_posix()}")
    command(proc, "getfulldate")
    command(proc, "clients")
    command(proc, "setting linkgraph.passenger_od_assignment")
    command(proc, "setting linkgraph.passenger_journey_routing")
    command(proc, "setting linkgraph.passenger_journey_periodic")
    command(proc, "passenger_journey_stats inventory")
    command(proc, "passenger_journey_stats total")
    command(proc, "script")

def client(name):
    directory = prepare(name)
    (directory / "scripts" / "game_start.scr").write_text(
        f"script {(directory / 'console.txt').as_posix()}\ngetfulldate\nscript\n", encoding="utf-8")
    proc = start(directory, ["-v", "null:until_exit", "-n", "127.0.0.1:17493#255"])
    if not a.single_core_clients:
        affinities[name] = {"restricted": False}
        return proc, directory
    kernel = ctypes.windll.kernel32
    kernel.OpenProcess.restype = ctypes.c_void_p
    handle = kernel.OpenProcess(0x200 | 0x400, False, proc.pid)
    process_mask, system_mask = ctypes.c_size_t(), ctypes.c_size_t()
    applied, mask = False, 0
    if handle and kernel.GetProcessAffinityMask(ctypes.c_void_p(handle), ctypes.byref(process_mask), ctypes.byref(system_mask)):
        mask = process_mask.value & -process_mask.value
        applied = bool(kernel.SetProcessAffinityMask(ctypes.c_void_p(handle), ctypes.c_size_t(mask)))
        kernel.CloseHandle(ctypes.c_void_p(handle))
    affinities[name] = {"mask": mask, "applied": applied}
    return proc, directory

try:
    server_dir = prepare("server")
    server_log = server_dir / "console.txt"
    (server_dir / "scripts" / "game_start.scr").write_text(
        f"script {server_log.as_posix()}\n"
        + (f"setting linkgraph.passenger_transfer_timetables {a.transfer_timetables}\n" if a.transfer_timetables != "preserve" else "")
        + ("setting linkgraph.passenger_od_assignment true\n" + f"setting linkgraph.passenger_journey_routing {'false' if a.backend == 'off' else 'true'}\n" + f"setting linkgraph.passenger_journey_periodic {'true' if a.backend == 'periodic' else 'false'}\n" if a.backend != "preserve" else "")
        + "setting network.sync_freq 1\nsetting network.max_lag_time 32000\n"
        "setting network.max_join_time 32000\nsetting network.max_download_time 32000\npause\ngetfulldate\n"
        "setting linkgraph.passenger_transfer_timetables\n"
        "setting linkgraph.distribution_pax\nsetting linkgraph.recalc_interval\n"
        "setting linkgraph.recalc_time\nsetting linkgraph.passenger_od_assignment\n"
        "setting linkgraph.passenger_journey_routing\nsetting linkgraph.passenger_journey_periodic\n"
        "passenger_journey_stats inventory\npassenger_journey_stats total\nscript\n", encoding="utf-8")
    server = start(server_dir, ["-D", "127.0.0.1:17493", "-g", str(save)])
    wait_for(lambda: "State ticks:" in read(server_log), "server loaded")
    print("Current server loaded; joining initial client", flush=True)
    first, first_dir = client("client-initial")
    wait_for(lambda: "Client #2" in read(server_dir / "stderr.txt") and "joined as" in read(server_dir / "stderr.txt"), "first join")
    print("Initial client joined; unpausing", flush=True)
    command(server, "unpause")
    time.sleep(a.late_delay)
    snapshot(server, server_dir)
    wait_for(lambda: len(re.findall(r"State ticks: (\d+)", read(server_log))) >= 2, "pre-join checkpoint")
    current_ticks = re.findall(r"State ticks: (\d+)", read(server_log))
    result["ticks_before_late_join"] = current_ticks
    if len(current_ticks) < 2 or current_ticks[-1] == current_ticks[0]:
        raise RuntimeError("Release server did not advance after unpause")
    print(f"Server advanced to {current_ticks[-1]}; joining late client", flush=True)
    second, second_dir = client("client-late")
    wait_for(lambda: "Client #3" in read(server_dir / "stderr.txt") and read(server_dir / "stderr.txt").count("joined as") >= 2, "late join")
    print("Late client joined; observing synchronization", flush=True)
    time.sleep(a.duration)
    command(server, "pause")
    time.sleep(2)
    snapshot(server, server_dir)
    wait_for(lambda: len(re.findall(r"State ticks: (\d+)", read(server_log))) >= 3, "final checkpoint")
    time.sleep(1)
    result["client_exit_codes_at_checkpoint"] = [first.poll(), second.poll()]
    result["phase_completed"] = True
except Exception as exc:
    result["error"] = str(exc)
    print(str(exc), flush=True)
finally:
    logs = [read(root / name / "stderr.txt") for name in ["server", "client-initial", "client-late"]]
    reports = []
    excerpts = []
    for name in ["server", "client-initial", "client-late"]:
        for report in sorted((root / name).glob("desync-*.log")):
            report_text = read(report)
            reports.append({"path": str(report), "bytes": report.stat().st_size,
                            "sha256": hashlib.sha256(report.read_bytes()).hexdigest(),
                            "flags": re.findall(r"^Flags: (.*)", report_text, re.M),
                            "subframe_mismatches": re.findall(r"^Desync subframe mismatch: (.*)", report_text, re.M),
                            "game_loaded_at": re.findall(r"^Game loaded at: (.*)", report_text, re.M)})
            excerpts.append(str(report) + "\n" + "\n".join(report_text.splitlines()[:24]))
    result["desync_reports"] = reports
    (root / "desync-excerpt.txt").write_text("\n\n".join(excerpts), encoding="utf-8")
    result["joined_clients"] = read(root / "server" / "stderr.txt").count("joined as")
    result["detected_sync_failure"] = bool(reports) or any(re.search(r"Sync error|sync_err:|Assertion failed|desync detected", log, re.I) for log in logs)
    result["server_state_ticks"] = re.findall(r"State ticks: (\d+)", read(root / "server" / "console.txt"))
    result["affinity"] = affinities
    result["single_core_clients"] = len(affinities) == 2 and all(v.get("applied", False) for v in affinities.values())
    result["client_ack_frames"] = {name: [int(v) for v in re.findall(r"Sent ACK at (\d+)", read(root / name / "stderr.txt"))]
                                   for name in ["client-initial", "client-late"]}
    result["connection_timeouts"] = re.findall(r"^.*(?:dropped because|general timeout).*$", read(root / "server" / "stdout.txt"), re.M)
    final_snapshot = read(root / "server" / "console.txt").rsplit("Console log output started", 1)[-1]
    result["connected_at_checkpoint"] = re.findall(r"Client #(\d+)\s+name: '([^']+)'", final_snapshot)
    result["finished_at_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    result["verdict"] = "sync failure observed" if result["detected_sync_failure"] else ("inconclusive: " + result["error"] if "error" in result else ("inconclusive: client timeout" if result["connection_timeouts"] else "no sync failure observed in test interval"))
    if a.binary == "openttd-console.exe":
        result["console_copy_change"] = "Only PE subsystem flag changed from GUI to console; executable code unchanged"
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2), flush=True)
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
