#!/usr/bin/env python3
"""Short deterministic loopback checks for joining a held preparation frontier.

prepare instruments copied TUs only, using the existing save inspector and null
driver mailbox. File gates control CPU scheduling, never simulation Work. run
uses a vanilla regression save, checks the real encrypted join/barrier and first
sync, then compares canonical state after 128 simulation ticks. No product hooks.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


def prepare(source, build):
    import passenger_precomputed_join_prepare as base
    previous = sys.argv
    try:
        sys.argv = [base.__file__, str(source), str(build), "join-preparation-probe", "--protocol-graph-quantum", "1"]
        base.main()
    finally:
        sys.argv = previous

    def copy(name, transform):
        original = source / "src" / name
        text = original.read_text(encoding="utf-8")
        text = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (original.parent / m[1]).resolve().as_posix() + '"'
                      if (original.parent / m[1]).is_file() else m[0], text)
        target = build / "join-probe" / original.name
        target.parent.mkdir(exist_ok=True)
        target.write_text(transform(text), encoding="utf-8")
        project = build / "openttd_lib.vcxproj"
        xml = project.read_text(encoding="utf-8-sig").replace(str(target), str(original))
        assert xml.count(str(original)) == 1, original
        project.write_text(xml.replace(str(original), str(target)), encoding="utf-8-sig")

    def database(text):
        anchor = "bool PassengerServiceDatabase::PreparePrecomputed()\n{"
        assert text.count(anchor) == 1
        return '#include <fstream>\n#include <filesystem>\n' + text.replace(anchor, anchor + r'''
    std::ifstream gate("hold-preparation");
    std::string phase; gate >> phase;
    if (!std::filesystem::exists("release-preparation") && this->IsPreparingPrecomputed() && (phase == "all" || phase == std::to_string(to_underlying(this->GetPrecomputedProgress().phase)))) {
        if (!std::filesystem::exists("preparation-held")) std::ofstream("preparation-held") << phase;
        return false;
    }
''')
    copy("passenger_service_database.cpp", database)

    def server(text):
        anchor = "\t\t/* Make a dump of the current game */"
        assert text.count(anchor) == 1
        return text.replace(anchor, '''        Debug(net, 0, "Join snapshot: frame={}; preparing={}; phase={}", _frame_counter,
            GetPassengerServiceDatabase().IsPreparingPrecomputed(), to_underlying(GetPassengerServiceDatabase().GetPrecomputedProgress().phase));
''' + anchor)
    copy("network/network_server.cpp", server)

    def client(text):
        anchor = "if (_network_join_status != NetworkJoinStatus::Registering) CloseWindowById(WindowClass::NetworkStatus, NetworkStatusWindowNumber::Join);"
        assert text.count(anchor) == 1
        text = '#include <fstream>\n' + text.replace(anchor, anchor + '''
                std::ofstream("join-synced") << (FindWindowById(WindowClass::NetworkStatus, NetworkStatusWindowNumber::Join) == nullptr);
''')
        anchor = "my_client->passenger_prepared = true;"
        assert text.count(anchor) == 1
        return text.replace(anchor, anchor + '\n        std::ofstream("preparation-ready") << "ready";')
    copy("network/network_client.cpp", client)

    console = build / "save-integrity/console_cmds.cpp"
    text = console.read_text(encoding="utf-8")
    anchor = '    if (operation == "fixture_timetable") {'
    assert text.count(anchor) == 1
    text = text.replace(anchor, r'''    if (operation == "join_fixture") {
        _settings_game.linkgraph.passenger_journey_routing = true;
        _settings_game.linkgraph.passenger_journey_periodic = true;
        _settings_game.linkgraph.passenger_journey_precomputed = true;
        for (OrderList *list : OrderList::Iterate()) {
            for (VehicleOrderID i = 0; i < list->GetNumOrders(); ++i) {
                Order *order = list->GetOrderAt(i);
                const Ticks old = order->GetTravelTime() + order->GetWaitTime();
                order->SetTravelTime(128); order->SetTravelTimetabled(true);
                order->SetWaitTime(32); order->SetWaitTimetabled(true);
                if (!order->IsType(OT_CONDITIONAL)) list->UpdateTotalDuration(160 - old);
            }
            list->RecalculateTimetableDuration();
        }
        auto &db = GetPassengerServiceDatabase(); db.Reset();
        std::ifstream gate("hold-preparation"); std::string phase; gate >> phase;
        if (phase == "2" || phase == "4") {
            while (db.IsBuilding()) db.Tick(PassengerServiceDatabase::BUILD_WORK, false);
            if (phase == "2") db.Tick(PassengerServiceDatabase::BUILD_WORK, true, true);
            else db.TickPrecomputed(1);
        }
        return true;
    }
''' + anchor)
    console.write_text(text, encoding="utf-8")


def run(binary, build, assets, output, case, port, phase):
    root = output / case
    root.mkdir(parents=True, exist_ok=False)
    processes, streams = [], []
    record = dict(case=case, phase=phase, port=port, passed=False)

    def read(directory, name):
        path = directory / name
        return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""

    def wait(predicate, seconds=60):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if predicate():
                return
            if processes and processes[0].poll() is not None:
                raise RuntimeError("Server exited")
            time.sleep(.05)
        raise RuntimeError("Timed out waiting for probe condition")

    def peer(name, workers, hold=None):
        directory = root / name
        directory.mkdir()
        (directory / "scripts").mkdir()
        (directory / "baseset").mkdir()
        shutil.copytree(build / "baseset", directory / "baseset", dirs_exist_ok=True)
        for path in (assets / "content_download/baseset").glob("*OpenGFX-*.tar"):
            shutil.copy2(path, directory / "baseset" / path.name)
        shutil.copytree(build / "lang", directory / "lang")
        (directory / "openttd.cfg").write_text(
            "[misc]\nlanguage = english.lng\n[gui]\nautosave = off\nautosave_on_exit = false\n"
            f"passenger_precomputed_workers = {workers}\n[network]\nclient_name = {name}\nserver_game_type = local\n"
            f"server_port = {port}\npause_on_join = true\nmin_active_clients = 0\nframe_freq = 1\n"
            + ("max_join_time_total = 60\n" if case == "stalled" and name == "server" else "") +
            "[graphicsset]\nname = OpenGFX\n", encoding="utf-8")
        if hold:
            (directory / "hold-preparation").write_text(hold)
        return directory

    def start(directory, args):
        out = (directory / "stdout.txt").open("w", encoding="utf-8")
        err = (directory / "stderr.txt").open("w", encoding="utf-8")
        streams.extend([out, err])
        process = subprocess.Popen([str(binary), "-x", "-X", "-c", "openttd.cfg", "-snull", "-mnull", "-dnet=4,desync=1", *args],
                                   cwd=directory, stdin=subprocess.PIPE, stdout=out, stderr=err, text=True, encoding="utf-8",
                                   creationflags=subprocess.CREATE_NO_WINDOW,
                                   env=dict(os.environ, OPENTTD_PASSENGER_MEASURE="1"))
        processes.append(process)
        return process

    def command(process, value):
        process.stdin.write(value + "\n"); process.stdin.flush()

    def snapshot(directory, process=None):
        path = directory / "save-integrity.json"
        old = path.stat().st_mtime_ns if path.exists() else 0
        if process:
            command(process, "passenger_journey_stats save_integrity")
        else:
            request = directory / "probe-request.tmp"
            request.write_text("passenger_journey_stats save_integrity\n", encoding="utf-8")
            request.replace(directory / "probe-request.txt")
        wait(lambda: path.exists() and path.stat().st_mtime_ns != old)
        return json.loads(path.read_text())

    def same(host, guest):
        keys = ("state_ticks", "database_digest", "packet_state_digest", "payment_digest", "ownership_digest", "loading_digest",
                "completed_rows", "retained_rows", "table_operations", "published_revision", "company_deliveries")
        return [key for key in keys if host[key] != guest[key]]

    try:
        held = case not in ("ordinary", "before-enable")
        server = peer("server", 8, str(phase) if held else None)
        (server / "scripts/game_start.scr").write_text(
            "pause\n" + ("passenger_journey_stats join_fixture\n" if case != "ordinary" else "") +
            "passenger_journey_stats save_integrity\n", encoding="utf-8")
        host = start(server, ["-D", f"127.0.0.1:{port}", "-g", str(Path(__file__).resolve().parents[1] / "regression/stationlist/test.sav")])
        wait(lambda: (server / ("preparation-held" if held else "save-integrity.json")).exists())
        clients = []
        join_started = time.monotonic()
        for index in range(2 if case in ("multiple", "disconnect") else 1):
            client = peer(f"client{index}", index, "all" if case in ("host-first", "multiple", "disconnect", "stalled") else None)
            guest = start(client, ["-v", "null:until_exit", "-n", f"127.0.0.1:{port}#255"])
            clients.append((client, guest))
            wait(lambda: read(server, "stderr.txt").count("map ready: client_join_ms=") >= index + 1)
        log = read(server, "stderr.txt")
        record["snapshots_during_preparation"] = log.count("preparing=true")
        if held:
            assert record["snapshots_during_preparation"] == len(clients)
            assert "Initial passenger tables ready at frame" not in log
            if case == "client-first":
                wait(lambda: (clients[0][0] / "preparation-ready").exists())
            (server / "release-preparation").write_text("release")
            if case in ("host-first", "multiple", "disconnect", "stalled"):
                for directory, _ in clients:
                    wait(lambda directory=directory: (directory / "preparation-held").exists())
                time.sleep(.25)
                assert "Initial passenger tables ready at frame" not in read(server, "stderr.txt")
                if case == "stalled":
                    wait(lambda: "has been disconnected" in read(server, "stderr.txt"), 75)
                    record["bounded_wait_seconds"] = time.monotonic() - join_started
                    assert 50 <= record["bounded_wait_seconds"] < 75
                    assert not (clients[0][0] / "join-synced").exists()
                    record["passed"] = True
                    return
                if case == "disconnect":
                    directory, process = clients.pop(); process.terminate(); process.wait(timeout=10)
                for directory, _ in clients:
                    (directory / "release-preparation").write_text("release")
        for directory, _ in clients:
            wait(lambda directory=directory: (directory / "join-synced").exists())
            assert (directory / "join-synced").read_text() == "1", "Join window still open after first sync"
        initial = snapshot(server, host)
        if case != "ordinary":
            assert initial["completed_rows"] > 0 and initial["table_operations"] > 0, "Fixture must have real passenger tables"
        record["join_mismatches"] = [same(initial, snapshot(directory)) for directory, _ in clients]
        assert not any(record["join_mismatches"]), record
        if case == "before-enable":
            command(host, "setting linkgraph.passenger_journey_precomputed false")
            wait(lambda: not snapshot(server, host)["precomputed_routing"] and all(not snapshot(directory)["precomputed_routing"] for directory, _ in clients))
            barriers = read(server, "stderr.txt").count("Initial passenger tables ready at frame")
            command(host, "setting linkgraph.passenger_journey_precomputed true")
            wait(lambda: read(server, "stderr.txt").count("Initial passenger tables ready at frame") > barriers)
            prepared = snapshot(server, host)
            assert prepared["state_ticks"] == initial["state_ticks"]
            record["preparation_mismatches"] = [same(prepared, snapshot(directory)) for directory, _ in clients]
            assert not any(record["preparation_mismatches"]), record
            initial = prepared
        command(host, "unpause")
        wait(lambda: snapshot(server, host)["state_ticks"] >= initial["state_ticks"] + 128)
        command(host, "pause")
        time.sleep(.5)
        final = snapshot(server, host)
        record["simulation_mismatches"] = [same(final, snapshot(directory)) for directory, _ in clients]
        assert not any(record["simulation_mismatches"]), record
        record["simulation_ticks"] = final["state_ticks"] - initial["state_ticks"]
        for directory, _ in clients:
            assert not re.search("Sync error|desync detected|disconnected|dropped", read(directory, "stderr.txt"), re.I)
        record["passed"] = True
    except Exception as error:
        record["error"] = str(error)
        raise
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate(); process.wait(timeout=10)
        for stream in streams:
            stream.close()
        (root / "result.json").write_text(json.dumps(record, indent=2) + "\n")
        print(json.dumps(record), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prep = commands.add_parser("prepare")
    prep.add_argument("source", type=Path); prep.add_argument("build", type=Path)
    test = commands.add_parser("run")
    for name in ("binary", "build", "assets", "output"):
        test.add_argument(name, type=Path)
    test.add_argument("--case", choices=("client-first", "host-first", "multiple", "disconnect", "ordinary", "before-enable", "stalled"), required=True)
    test.add_argument("--phase", type=int, choices=(1, 2, 4), default=1)
    test.add_argument("--port", type=int, default=17620)
    args = parser.parse_args()
    if args.command == "prepare":
        prepare(args.source.resolve(), args.build.resolve())
    else:
        run(*(getattr(args, name).resolve() for name in ("binary", "build", "assets", "output")), args.case, args.port, args.phase)


if __name__ == "__main__":
    main()
