"""Test one paused loopback join with default network timeouts.

Requires the copied-TU save_integrity/stdio measurement binary prepared by
passenger_precomputed_real_map.py. No product networking or timeout is changed.
This is a paused join/Packet-payment snapshot check, not the full transport suite.
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "package", "input", "output", "lang"):
        parser.add_argument(name, type=pathlib.Path)
    parser.add_argument("--port", type=int, default=17529)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    processes, streams = [], []
    result = dict(address=f"127.0.0.1:{args.port}", default_timeouts=True, paused_join=True,
                  input=str(args.input.resolve()), joined=False, packet_payment_equal=False)

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
            time.sleep(0.25)
        return False

    def command(process, text):
        process.stdin.write(text + "\n")
        process.stdin.flush()

    started = time.monotonic()
    try:
        server = prepare("server")
        (server / "scripts/game_start.scr").write_text(
            f"script {(server / 'console.txt').as_posix()}\npause\ngetfulldate\n"
            "passenger_journey_stats total\npassenger_journey_stats save_integrity\nscript\n", encoding="utf-8")
        host = start(server, ["-D", result["address"], "-g", str(args.input.resolve())])
        if not wait(lambda: (server / "save-integrity.json").exists(), 180):
            raise RuntimeError("Server did not finish loading")
        result["server_snapshot"] = json.loads(read(server, "save-integrity.json"))
        client = prepare("client")
        (client / "scripts/game_start.scr").write_text(
            f"script {(client / 'console.txt').as_posix()}\ngetfulldate\n"
            "passenger_journey_stats total\npassenger_journey_stats save_integrity\nscript\n", encoding="utf-8")
        guest = start(client, ["-v", "null:until_exit", "-n", result["address"] + "#255"])
        join_started = time.monotonic()
        snapshot = wait(lambda: (client / "save-integrity.json").exists() or
                        re.search(r"disconnected|dropped|Sync error|desync detected", read(client, "stderr.txt"), re.I), 120)
        result["join_elapsed_seconds"] = time.monotonic() - join_started
        if (client / "save-integrity.json").exists():
            result["client_snapshot"] = json.loads(read(client, "save-integrity.json"))
            keys = ("state_ticks", "packets", "cargo_units", "known_od_units", "journey_packets",
                    "packet_state_digest", "payment_digest", "payments_valid")
            result["packet_payment_equal"] = all(result["server_snapshot"][key] == result["client_snapshot"][key] for key in keys)
        # Permit the MAP_OK exchange to reach the server after the local load.
        wait(lambda: "joined as" in read(server, "stderr.txt") or guest.poll() is not None, 10)
        result["joined"] = "joined as" in read(server, "stderr.txt")
        result["client_exit_code"] = guest.poll()
        result["network_error"] = bool(re.search(r"disconnected|dropped|Sync error|desync detected|longer than",
                                                read(server, "stderr.txt") + read(client, "stderr.txt"), re.I))
        result["completed_observation"] = bool(snapshot)
        command(host, f"script {(server / 'end-console.txt').as_posix()}")
        command(host, "clients")
        command(host, "script")
        result["pass"] = result["joined"] and result["packet_payment_equal"] and not result["network_error"]
    except Exception as error:
        result.update(error=str(error), **{"pass": False})
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
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(result, indent=2))
    if not result["pass"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
