"""Test a real loopback join, then synchronized transport, with default timeouts.

Requires copied measurement TUs from passenger_precomputed_join_prepare.py.
The inspector and command mailbox are measurement-only; routing/networking and
timeouts are the product code. --stall suspends only this probe's own client
after its first progress packet and verifies the unchanged timeout still fires.
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import time
import ctypes

import passenger_precomputed_real_map as real


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "package", "input", "output", "lang"):
        parser.add_argument(name, type=pathlib.Path)
    parser.add_argument("--port", type=int, default=17529)
    parser.add_argument("--ticks", type=int, default=1024)
    parser.add_argument("--initial-build", action="store_true")
    parser.add_argument("--stall", action="store_true")
    parser.add_argument("--observe-timeout", type=int, default=180,
                        help="Harness observation seconds for loading/replay; does not change game/network timeouts")
    args = parser.parse_args()
    if args.observe_timeout <= 0:
        parser.error("--observe-timeout must be positive")
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    processes, streams, observed = [], [], {}
    result = dict(address=f"127.0.0.1:{args.port}", default_timeouts=True, paused_join=True,
                  input=str(args.input.resolve()), joined=False, packet_payment_equal=False,
                  binary_sha256=real.PROFILE.sha256(args.binary), input_sha256=real.PROFILE.sha256(args.input),
                  requested_ticks=args.ticks, initial_build=args.initial_build, stall_test=args.stall,
                  observation_timeout_seconds=args.observe_timeout)

    def read(directory, name):
        path = directory / name
        return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""

    def prepare(name):
        directory = root / name
        directory.mkdir()
        for asset in ("baseset", "newgrf", "content_download", "ai", "game"):
            source = args.package / asset
            if source.is_dir():
                def link_or_copy(source, destination):
                    try:
                        os.link(source, destination)
                    except OSError:
                        shutil.copy2(source, destination)
                shutil.copytree(source, directory / asset, copy_function=link_or_copy)
        shutil.copytree(args.lang, directory / "lang")
        (directory / "scripts").mkdir()
        (directory / "openttd.cfg").write_text(
            "[misc]\nlanguage = english.lng\n[gui]\nautosave = off\nautosave_on_exit = false\n"
            f"[network]\nclient_name = routing-{name}\nserver_game_type = local\nserver_port = {args.port}\n"
            "pause_on_join = true\nmin_active_clients = 0\n[graphicsset]\nname = zBase\n", encoding="utf-8")
        return directory

    def start(directory, arguments):
        out, err = (directory / "stdout.txt").open("w"), (directory / "stderr.txt").open("w")
        streams.extend((out, err))
        process = subprocess.Popen([str(args.binary.resolve()), "-x", "-X", "-c", "openttd.cfg", "-snull", "-mnull",
                                    "-d", "net=3,desync=1", *arguments], cwd=directory,
                                   stdin=subprocess.PIPE, stdout=out, stderr=err, text=True, encoding="utf-8",
                                   creationflags=subprocess.CREATE_NO_WINDOW,
                                   env=dict(os.environ, OPENTTD_PASSENGER_MEASURE="1"))
        processes.append(process)
        return process

    def wait(predicate, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if predicate():
                return True
            if any(process.poll() is not None for process in processes):
                return False
            for index, process in enumerate(processes):
                for key, value in real.CPU.observe(process).items():
                    name = f"{index}_{key}"
                    observed[name] = max(observed.get(name, 0), value)
            time.sleep(0.25)
        return False

    def command(process, text):
        process.stdin.write(text + "\n")
        process.stdin.flush()

    def json_file(directory, name):
        try:
            return json.loads(read(directory, name))
        except (ValueError, OSError):
            return None

    def client_commands(directory, commands):
        temporary = directory / "probe-request.tmp"
        temporary.write_text("\n".join(commands) + "\n", encoding="utf-8")
        temporary.replace(directory / "probe-request.txt")

    def snapshot_server(process, directory):
        path = directory / "save-integrity.json"
        previous = path.stat().st_mtime_ns if path.exists() else 0
        command(process, "passenger_journey_stats save_integrity")
        if not wait(lambda: path.exists() and path.stat().st_mtime_ns != previous and json_file(directory, path.name), 10):
            raise RuntimeError("Server snapshot did not complete")
        return json_file(directory, path.name)

    def snapshot_client(directory):
        path = directory / "save-integrity.json"
        previous = path.stat().st_mtime_ns if path.exists() else 0
        client_commands(directory, [f"script {(directory / 'transport-console.txt').as_posix()}",
                                    "passenger_journey_stats total", "passenger_journey_stats save_integrity", "script"])
        if not wait(lambda: path.exists() and path.stat().st_mtime_ns != previous and json_file(directory, path.name), 10):
            raise RuntimeError("Client snapshot did not complete")
        return json_file(directory, path.name)

    def compare(server, client):
        keys = ("state_ticks", "packets", "cargo_units", "known_od_units", "journey_packets",
                "packet_state_digest", "payment_digest", "payments_valid", "database_digest",
                "ownership_digest", "loading_digest", "completed_rows", "retained_rows",
                "table_operations", "published_revision", "company_deliveries", "journey_routing", "precomputed_routing")
        return [key for key in keys if server[key] != client[key]]

    def errors(server, client):
        return bool(re.search(r"disconnected|dropped|Sync error|desync detected|longer than",
                              read(server, "stderr.txt") + read(client, "stderr.txt"), re.I))

    started = time.monotonic()
    try:
        server = prepare("server")
        initial = ("setting linkgraph.passenger_journey_precomputed true\n"
                   "passenger_journey_stats partial_precomputed\n" if args.initial_build else "")
        (server / "scripts/game_start.scr").write_text(
            f"script {(server / 'console.txt').as_posix()}\npause\ngetfulldate\n"
            "setting max_join_time\nsetting max_lag_time\nsetting max_init_time\nsetting max_download_time\n"
            + initial + "passenger_journey_stats total\npassenger_journey_stats save_integrity\nscript\n", encoding="utf-8")
        host = start(server, ["-D", result["address"], "-g", str(args.input.resolve())])
        if not wait(lambda: (server / "save-integrity.json").exists(), args.observe_timeout):
            raise RuntimeError("Server did not finish loading")
        # Console settings/pause are synchronized commands; let them execute
        # before comparing the server with the subsequently downloaded map.
        wait(lambda: False, 1)
        result["server_snapshot"] = snapshot_server(host, server)
        client = prepare("client")
        client_commands(client, [f"script {(client / 'console.txt').as_posix()}", "getfulldate",
                                "passenger_journey_stats total", "passenger_journey_stats save_integrity", "script"])
        guest = start(client, ["-v", "null:until_exit", "-n", result["address"] + "#255"])
        join_started = time.monotonic()
        if args.stall:
            if not wait(lambda: "map replay progress" in read(server, "stderr.txt"), 90):
                raise RuntimeError("Client never reported replay progress")
            # Do not send fake heartbeats while work is stopped.
            ctypes.windll.kernel32.OpenProcess.restype = ctypes.c_void_p
            handle = ctypes.windll.kernel32.OpenProcess(0x0800, False, guest.pid)
            if not handle:
                raise ctypes.WinError()
            try:
                ctypes.windll.ntdll.NtSuspendProcess.argtypes = [ctypes.c_void_p]
                ctypes.windll.ntdll.NtResumeProcess.argtypes = [ctypes.c_void_p]
                if ctypes.windll.ntdll.NtSuspendProcess(handle) != 0:
                    raise RuntimeError("Could not suspend probe client")
                suspended = time.monotonic()
                try:
                    result["stall_dropped"] = wait(lambda: "longer than 500 ticks to join" in
                                                  (read(server, "stdout.txt") + read(server, "stderr.txt")), 30)
                    result["stall_seconds"] = time.monotonic() - suspended
                finally:
                    ctypes.windll.ntdll.NtResumeProcess(handle)
            finally:
                ctypes.windll.kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
                ctypes.windll.kernel32.CloseHandle(handle)
            result["pass"] = result["stall_dropped"]
            if not result["pass"]:
                raise RuntimeError("Stalled replay was not dropped at the default join deadline")
            return
        snapshot = wait(lambda: json_file(client, "save-integrity.json") or errors(server, client), args.observe_timeout)
        result["join_elapsed_seconds"] = time.monotonic() - join_started
        if (client / "save-integrity.json").exists():
            result["client_snapshot"] = json_file(client, "save-integrity.json")
            result["join_mismatches"] = compare(result["server_snapshot"], result["client_snapshot"])
            result["packet_payment_equal"] = not result["join_mismatches"]
        # Permit the MAP_OK exchange to reach the server after the local load.
        wait(lambda: "joined as" in read(server, "stderr.txt") or guest.poll() is not None, 10)
        result["joined"] = "joined as" in read(server, "stderr.txt")
        result["client_exit_code"] = guest.poll()
        result["network_error"] = errors(server, client)
        result["completed_observation"] = bool(snapshot)
        result["replay"] = json_file(client, "join-replay.json")
        result["progress_packets"] = len(re.findall("map replay progress", read(server, "stderr.txt")))
        if not (result["joined"] and result["packet_payment_equal"] and not result["network_error"]):
            raise RuntimeError("Join or canonical snapshot comparison failed")
        if args.ticks:
            initial_tick = result["server_snapshot"]["state_ticks"]
            command(host, "unpause")
            deadline = time.monotonic() + 900
            last_tick = initial_tick
            while last_tick - initial_tick < args.ticks:
                if time.monotonic() > deadline or errors(server, client):
                    raise RuntimeError("Transport did not reach its requested ticks")
                time.sleep(2)
                last_tick = snapshot_server(host, server)["state_ticks"]
            command(host, "pause")
            # A pause command is distributed for a future network frame. Sample
            # only after the client has executed it, rather than assuming a
            # fixed wall-clock delay is enough on a busy real map.
            settle_deadline = time.monotonic() + 30
            while True:
                server_end = snapshot_server(host, server)
                client_end = snapshot_client(client)
                if server_end["state_ticks"] == client_end["state_ticks"]:
                    confirmation = snapshot_server(host, server)
                    if confirmation["state_ticks"] == server_end["state_ticks"]:
                        break
                if time.monotonic() > settle_deadline or errors(server, client):
                    raise RuntimeError("Client did not reach the server's paused StateTicks")
                time.sleep(.5)
            result["transport_server"] = server_end
            result["transport_client"] = client_end
            result["transport_mismatches"] = compare(result["transport_server"], result["transport_client"])
            result["actual_ticks"] = result["transport_server"]["state_ticks"] - initial_tick
            result["transport_equal"] = not result["transport_mismatches"]
        else:
            result["transport_equal"] = True
        command(host, f"script {(server / 'end-console.txt').as_posix()}")
        command(host, "clients")
        command(host, "passenger_journey_stats total")
        command(host, "script")
        result["network_error"] = errors(server, client)
        result["pass"] = result["joined"] and result["packet_payment_equal"] and result["transport_equal"] and not result["network_error"]
    except Exception as error:
        result.update(error=str(error), **{"pass": False})
        result["network_error"] = errors(root / "server", root / "client")
    finally:
        for index, process in enumerate(processes):
            if process.poll() is None:
                try:
                    if index == 0:
                        command(process, "quit")
                        process.wait(timeout=5)
                    else:
                        process.terminate()
                except (OSError, subprocess.TimeoutExpired):
                    process.terminate()
            process.wait(timeout=15)
        for stream in streams:
            stream.close()
        result["elapsed_seconds"] = time.monotonic() - started
        result["process_observations"] = observed
        result["newgrf_warning_lines"] = {directory.name: len(re.findall(r"\[grf:0\]", read(directory, "stderr.txt")))
                                          for directory in (root / "server", root / "client")}
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(result, indent=2))
    if not result["pass"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
