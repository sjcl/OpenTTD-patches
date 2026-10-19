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
    parser.add_argument("--client-binary", type=pathlib.Path, help="Use a separately built client for cross-compiler synchronization checks")
    parser.add_argument("--port", type=int, default=17529)
    parser.add_argument("--frame-freq", type=int, default=0)
    parser.add_argument("--ticks", type=int, default=1024)
    parser.add_argument("--initial-build", action="store_true")
    parser.add_argument("--stall", action="store_true")
    parser.add_argument("--toggle-precomputed", action="store_true", help="Disable and re-enable routing after joining, then compare the peer preparation barrier")
    parser.add_argument("--wait-server-ready", action="store_true", help="Connect after server preparation instead of testing the server-wait phase")
    parser.add_argument("--wait-saved-replay", action="store_true", help="Complete validated saved passenger replay in the server setup script before connecting")
    parser.add_argument("--replace-precomputed", action="store_true", help="Change transfer cost after joining; compare ordinary replacement ticks with retained rows")
    parser.add_argument("--server-workers", type=int, choices=range(9), help="Local initial passenger destination workers on the server")
    parser.add_argument("--client-workers", type=int, choices=range(9), help="Local initial passenger destination workers on the client")
    parser.add_argument("--hyperpath", action=argparse.BooleanOptionalAction, default=None, help="Routing mode for the isolated loopback fixture")
    parser.add_argument("--fixture-timetable", action="store_true", help="Enable routing and fixed timetable times in an isolated vanilla regression save")
    parser.add_argument("--require-journey-movement", action="store_true", help="Require known-OD fixture packets and actual journey boarding/delivery")
    for peer in ("server", "client"):
        parser.add_argument(f"--{peer}-worker-delay-us", type=int, default=0)
        parser.add_argument(f"--{peer}-worker-fail-work", type=int, default=0)
        parser.add_argument(f"--{peer}-main-headstart-us", type=int, default=0,
                            help="Copied probe only: give Workers CPU before the first row decision")
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
                  requested_ticks=args.ticks, initial_build=args.initial_build, stall_test=args.stall, frame_freq=args.frame_freq,
                  toggle_precomputed=args.toggle_precomputed, replace_precomputed=args.replace_precomputed,
                  wait_server_ready=args.wait_server_ready,
                  worker_hooks={peer: {key: getattr(args, peer + "_" + key) for key in
                                       ("worker_delay_us", "worker_fail_work", "main_headstart_us")}
                                for peer in ("server", "client")},
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
        workers = args.server_workers if name == "server" else args.client_workers
        worker_setting = "" if workers is None else f"passenger_precomputed_workers = {workers}\n"
        (directory / "openttd.cfg").write_text(
            "[misc]\nlanguage = english.lng\n[gui]\nautosave = off\nautosave_on_exit = false\n"
            + worker_setting + f"[network]\nclient_name = routing-{name}\nserver_game_type = local\nserver_port = {args.port}\n"
            f"pause_on_join = true\nmin_active_clients = 0\nframe_freq = {args.frame_freq}\n[graphicsset]\nname = zBase\n", encoding="utf-8")
        return directory

    def start(directory, arguments):
        out, err = (directory / "stdout.txt").open("w"), (directory / "stderr.txt").open("w")
        streams.extend((out, err))
        binary = args.client_binary if directory.name == "client" and args.client_binary else args.binary
        result[directory.name + "_binary_sha256"] = real.PROFILE.sha256(binary)
        process = subprocess.Popen([str(binary.resolve()), "-x", "-X", "-c", "openttd.cfg", "-snull", "-mnull",
                                    "-d", f"net={4 if args.frame_freq else 3},desync=1", *arguments], cwd=directory,
                                   stdin=subprocess.PIPE, stdout=out, stderr=err, text=True, encoding="utf-8",
                                   creationflags=subprocess.CREATE_NO_WINDOW,
                                   env=dict(os.environ, OPENTTD_PASSENGER_MEASURE="1",
                                            OPENTTD_PASSENGER_WORKER_DELAY_US=str(getattr(args, directory.name + "_worker_delay_us")),
                                            OPENTTD_PASSENGER_WORKER_FAIL_WORK=str(getattr(args, directory.name + "_worker_fail_work")),
                                            OPENTTD_PASSENGER_MAIN_HEADSTART_US=str(getattr(args, directory.name + "_main_headstart_us"))))
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
        if "table_digest" in server:
            keys += ("table_digest", "retained_table_digest", "finance_digest")
        return [key for key in keys if server[key] != client[key]]

    def errors(server, client):
        return bool(re.search(r"disconnected|dropped|Sync error|desync detected|longer than",
                              read(server, "stderr.txt") + read(client, "stderr.txt"), re.I))

    started = time.monotonic()
    try:
        server = prepare("server")
        result.update(server_workers=args.server_workers, client_workers=args.client_workers, hyperpath=args.hyperpath, fixture_timetable=args.fixture_timetable)
        initial = ("setting linkgraph.passenger_journey_precomputed true\n"
                   "passenger_journey_stats partial_precomputed\n" if args.initial_build else "")
        (server / "scripts/game_start.scr").write_text(
            f"script {(server / 'console.txt').as_posix()}\npause\ngetfulldate\n"
            + ("passenger_journey_stats wait_saved_replay\n" if args.wait_saved_replay else "")
            + "setting max_join_time\nsetting max_join_time_total\nsetting max_lag_time\nsetting max_init_time\nsetting max_download_time\n"
            + ("" if args.hyperpath is None else f"setting linkgraph.passenger_journey_hyperpath {'true' if args.hyperpath else 'false'}\n")
            + ("setting linkgraph.passenger_journey_routing true\nsetting linkgraph.passenger_journey_periodic true\n"
               "setting linkgraph.passenger_od_assignment true\nsetting linkgraph.passenger_journey_precomputed true\nsetting linkgraph.distribution_pax 2\n"
               "passenger_journey_stats fixture_timetable\n" if args.fixture_timetable else "")
            + initial + "passenger_journey_stats total\npassenger_journey_stats save_integrity\nscript\n", encoding="utf-8")
        host = start(server, ["-D", result["address"], "-g", str(args.input.resolve())])
        if not wait(lambda: (server / "save-integrity.json").exists(), args.observe_timeout):
            raise RuntimeError("Server did not finish loading")
        if args.wait_saved_replay and "Saved passenger replay: ready=true;" not in read(server, "console.txt"):
            raise RuntimeError("Server saved passenger replay did not complete")
        if args.initial_build and "Partial build probe requires completed passenger save replay." in read(server, "console.txt"):
            raise RuntimeError("Partial build probe cannot mutate an unfinished save replay; use an input without an active saved table")
        # Console settings/pause are synchronized commands; let them execute
        # before comparing the server with the subsequently downloaded map.
        wait(lambda: False, 1)
        result["server_snapshot"] = snapshot_server(host, server)
        if args.require_journey_movement and result["server_snapshot"]["known_od_units"] == 0:
            raise RuntimeError("Fixture seeded no known-OD passenger packets")
        if args.wait_server_ready:
            preparation_started = time.monotonic()
            prepared_in_setup = (args.wait_saved_replay and not args.initial_build and not args.fixture_timetable and
                                 args.hyperpath is None and "Precomputed initial preparation: pending=false;" in read(server, "console.txt"))
            if not wait(lambda: "Initial passenger tables ready at frame" in read(server, "stderr.txt") or
                        prepared_in_setup, args.observe_timeout):
                raise RuntimeError("Server initial passenger preparation did not finish")
            result["server_preparation_wait_seconds"] = time.monotonic() - preparation_started
            prepared = snapshot_server(host, server)
            if prepared["state_ticks"] != result["server_snapshot"]["state_ticks"]:
                raise RuntimeError("Server preparation advanced paused simulation")
            result["server_loading_snapshot"] = result["server_snapshot"]
            result["server_snapshot"] = prepared
        client = prepare("client")
        client_commands(client, [f"script {(client / 'console.txt').as_posix()}", "getfulldate",
                                "passenger_journey_stats total", "passenger_journey_stats save_integrity", "script"])
        guest = start(client, ["-v", "null:until_exit", "-n", result["address"] + "#255"])
        join_started = time.monotonic()
        if args.stall:
            if not wait(lambda: "map replay progress" in read(server, "stderr.txt") or errors(server, client), args.observe_timeout) or "map replay progress" not in read(server, "stderr.txt"):
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
            result["server_loading_snapshot"] = result["server_snapshot"]
            result["server_snapshot"] = snapshot_server(host, server)
            result["client_snapshot"] = json_file(client, "save-integrity.json")
            result["join_mismatches"] = compare(result["server_snapshot"], result["client_snapshot"])
            result["packet_payment_equal"] = not result["join_mismatches"]
        # Permit the MAP_OK exchange to reach the server after the local load.
        wait(lambda: "joined as" in read(server, "stderr.txt") or guest.poll() is not None, 10)
        result["joined"] = "joined as" in read(server, "stderr.txt")
        timing = re.search(r"map ready: server_preparation_ms=(\d+); client_join_ms=(\d+)", read(server, "stderr.txt"))
        if timing:
            result["server_owned_preparation_seconds"] = int(timing[1]) / 1000
            result["client_join_budget_seconds"] = int(timing[2]) / 1000
        result["client_exit_code"] = guest.poll()
        result["network_error"] = errors(server, client)
        result["completed_observation"] = bool(snapshot)
        result["replay"] = json_file(client, "join-replay.json")
        result["progress_packets"] = len(re.findall("map replay progress", read(server, "stderr.txt")))
        if not (result["joined"] and result["packet_payment_equal"] and not result["network_error"]):
            raise RuntimeError("Join or canonical snapshot comparison failed")
        if args.toggle_precomputed:
            command(host, "setting linkgraph.passenger_journey_precomputed false")
            deadline = time.monotonic() + 30
            while True:
                off_server, off_client = snapshot_server(host, server), snapshot_client(client)
                if not off_server["precomputed_routing"] and not off_client["precomputed_routing"]:
                    break
                if time.monotonic() >= deadline or errors(server, client):
                    raise RuntimeError("Peers did not execute the disable command")
                time.sleep(.25)
            barriers = read(server, "stderr.txt").count("Initial passenger tables ready at frame")
            command(host, "setting linkgraph.passenger_journey_precomputed true")
            if not wait(lambda: read(server, "stderr.txt").count("Initial passenger tables ready at frame") > barriers or errors(server, client), args.observe_timeout):
                raise RuntimeError("Connected-peer preparation barrier did not release")
            result["toggle_server"], result["toggle_client"] = snapshot_server(host, server), snapshot_client(client)
            result["toggle_mismatches"] = compare(result["toggle_server"], result["toggle_client"])
            if result["toggle_mismatches"] or result["toggle_server"]["state_ticks"] != off_server["state_ticks"]:
                raise RuntimeError("Connected-peer preparation advanced or diverged simulation state")
        if args.ticks:
            if args.replace_precomputed:
                command(host, "setting linkgraph.passenger_transfer_penalty 17")
                result["replacement_requested"] = True
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
            if "journey_boarded" in server_end:
                movement_keys = ("journey_boarded", "journey_transferred", "journey_delivered")
                server_base = result.get("toggle_server", result["server_snapshot"])
                client_base = result.get("toggle_client", result["client_snapshot"])
                result["journey_movement_server"] = {key: server_end[key] - server_base[key] for key in movement_keys}
                result["journey_movement_client"] = {key: client_end[key] - client_base[key] for key in movement_keys}
                result["transport_mismatches"] += [key for key in movement_keys if result["journey_movement_server"][key] != result["journey_movement_client"][key]]
            result["actual_ticks"] = result["transport_server"]["state_ticks"] - initial_tick
            result["transport_equal"] = not result["transport_mismatches"]
        else:
            result["transport_equal"] = True
        if args.require_journey_movement:
            movement = result.get("journey_movement_server", {})
            if not movement.get("journey_boarded", 0) or not movement.get("journey_delivered", 0):
                raise RuntimeError("Fixture did not exercise real journey boarding and delivery")
        command(host, f"script {(server / 'end-console.txt').as_posix()}")
        command(host, "clients")
        command(host, "passenger_journey_stats total")
        command(host, "script")
        result["network_error"] = errors(server, client)
        result["predicted_preparation_waits"] = read(client, "stderr.txt").count("ready at predicted frame")
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
                except (OSError, RuntimeError, subprocess.TimeoutExpired):
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
