#!/usr/bin/env python3
"""Instrument isolated VS translation units and replay fixed real-map ticks.

Product routing, capacity, Work limits and CargoPacket data are unchanged.
The opt-in driver counts StateTicks, records existing diagnostics, and resumes
NewGRF error pauses. Such warnings/resumptions are retained as limitations.
"""
import argparse
import importlib.util
import json
import math
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[1]


def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


PROFILE = module(ROOT / "docs/measurements/passenger-routing-profile-2026-10-13/Run-Profile.py", "profile")
CPU = module(ROOT / "utils/passenger_precomputed_benchmark.py", "cpu")


def dump(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def prepare(args):
    source, build = args.source.resolve(), args.build.resolve()
    integrity = module(ROOT / "docs/measurements/passenger-query-work-admission-2026-10-14/Prepare-Save-Integrity.py", "integrity")
    integrity.ROOT = source
    saved_argv = sys.argv
    try:
        sys.argv = [str(integrity.__file__), str(build)]
        integrity.main()
    finally:
        sys.argv = saved_argv
    census(args)
    driver(args)


def census(args):
    source, build = args.source.resolve(), args.build.resolve()
    target = build / "save-integrity/console_cmds.cpp"
    content = target.read_text(encoding="utf-8")
    opening = '\tif (operation == "inventory") {\n'
    anchor = opening + '\t\t/* Explicit read-only census.'
    assert content.count(anchor) == 1
    content = content.replace(anchor, opening + '''
        uint64_t company_deliveries = 0;
        for (const Company *company : Company::Iterate()) for (CargoType cargo : EnumRange(NUM_CARGO)) {
            if (IsCargoInClass(cargo, CargoClass::Passengers)) company_deliveries += company->cur_economy.delivered_cargo[cargo];
        }
        IConsolePrint(CC_DEFAULT, "All passenger company deliveries: current_economy_delivered={}", company_deliveries);
''' + '\t\t/* Explicit read-only census.')
    if (source / "src/passenger_precomputed.h").is_file():
        warm = '''
    if (operation == "build_precomputed") {
        auto &db = GetPassengerServiceDatabase();
        bool ready = false;
        const auto started = std::chrono::steady_clock::now();
        for (int i = 0; i < 4096; ++i) {
            db.TickPrecomputed(16 * 1024 * 1024);
            if (db.HasRejectedPrecomputedMemory()) break;
            ready = true;
            for (CargoType cargo : EnumRange(NUM_CARGO)) if (IsCargoInClass(cargo, CargoClass::Passengers)) {
                const auto p = db.GetPrecomputedDiagnostic(cargo);
                if (p.destinations != p.stations) ready = false;
            }
            if (ready) break;
        }
        IConsolePrint(CC_DEFAULT, "Precomputed warm: ready={}; elapsed_us={}", ready,
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
        return ready;
    }
'''
        census_opening = opening + '\n        uint64_t company_deliveries'
        assert content.count(census_opening) == 1
        content = content.replace(census_opening, warm + census_opening)
    stdafx = '#include "' + (source / "src/stdafx.h").as_posix() + '"\n'
    content = content.replace(stdafx, stdafx + '#include "' + (source / "src/company_base.h").as_posix() + '"\n')
    target.write_text(content, encoding="utf-8")


def driver(args):
    source, build = args.source.resolve(), args.build.resolve()
    original = source / "src/video/null_v.cpp"
    content = original.read_text(encoding="utf-8")
    content = re.sub(r'#include "([^"]+)"', lambda match: '#include "' + (original.parent / match[1]).resolve().as_posix() + '"', content)
    stdafx = '#include "' + (source / "src/stdafx.h").as_posix() + '"\n'
    assert content.count(stdafx) == 1
    headers = ''.join('#include "' + (source / "src" / name).as_posix() + '"\n' for name in ["openttd.h", "date_func.h", "passenger_journey_active.h"])
    content = content.replace(stdafx, stdafx + headers + '#include <charconv>\n#include <fstream>\n#include <vector>\n')
    anchor = "\t\tfor (int i = 0; i < this->ticks; i++) {\n\t\t\t::GameLoop();\n\t\t\t::InputLoop();\n\t\t\t::UpdateWindows();\n\t\t}"
    assert content.count(anchor) == 1
    driver = r'''
        struct Sample { int64_t tick; uint64_t simulation, journey; };
        std::vector<Sample> samples;
        samples.reserve(this->ticks);
        int64_t start = 0;
        bool started = false;
        uint64_t loops = 0, error_resumes = 0;
        while (!_exit_game && (!started || _state_ticks.base() - start < this->ticks)) {
            if (started && _pause_mode.Test(PauseMode::Error)) {
                ++error_resumes;
                IConsoleCmdExec("unpause");
            }
            int64_t before = _state_ticks.base();
            ::GameLoop();
            ::InputLoop();
            ::UpdateWindows();
            ++loops;
            if (!started && _game_mode == GameMode::Normal) {
                std::ifstream clock("on-1-console.txt");
                std::string line;
                while (std::getline(clock, line)) {
                    constexpr std::string_view prefix = "State ticks: ";
                    if (!line.starts_with(prefix)) continue;
                    auto parsed = std::from_chars(line.data() + prefix.size(), line.data() + line.size(), start);
                    if (parsed.ec != std::errc{}) continue;
                    started = true;
                    before = start;
                    break;
                }
                if (!started) { Debug(misc, 0, "Missing benchmark StateTicks"); _exit_game = true; }
            }
            if (started && _state_ticks.base() > before) {
                const auto &stats = GetPassengerJourneyTickDiagnostic();
                samples.push_back({_state_ticks.base(), stats.simulation_microseconds, stats.journey_microseconds});
            }
            if (loops == 1 || loops % 100000 == 0) {
                std::ofstream progress("progress.json");
                progress << "{\"start\":" << start << ",\"end\":" << _state_ticks.base()
                    << ",\"samples\":" << samples.size() << ",\"pause_bits\":" << unsigned(_pause_mode.base()) << "}\n";
            }
            if (!started && loops > 4096) {
                Debug(misc, 0, "Benchmark did not advance StateTicks"); _exit_game = true;
            }
        }
        std::ofstream out("actual-ticks.json");
        out << "{\"start_tick\":" << start << ",\"end_tick\":" << _state_ticks.base()
            << ",\"driver_loops\":" << loops << ",\"error_resumes\":" << error_resumes << ",\"samples\":[";
        bool first = true;
        for (const auto &sample : samples) {
            out << (first ? "" : ",") << '[' << sample.tick << ',' << sample.simulation << ',' << sample.journey << ']';
            first = false;
        }
        out << "]}\n";
'''
    content = content.replace(anchor, driver)
    target = build / "real-map-driver/null_v.cpp"
    target.parent.mkdir(exist_ok=True)
    target.write_text(content, encoding="utf-8")
    project = build / "openttd_lib.vcxproj"
    xml = project.read_text(encoding="utf-8-sig")
    xml = xml.replace(str(target), str(original))
    assert xml.count(str(original)) == 1
    project.write_text(xml.replace(str(original), str(target)), encoding="utf-8-sig")
    dump(build / "real-map-instrumentation.json", dict(source=str(source),
         source_revision=args.revision, original_driver_sha256=PROFILE.sha256(original),
         instrumented_driver_sha256=PROFILE.sha256(target), compiler=PROFILE.compiler_info(build),
         note="Copied TUs only; exact StateTicks; error resumptions retained; read-only packet/payment inspector."))


def freeze(args):
    package = args.package.resolve()
    package.mkdir(parents=True, exist_ok=False)
    shutil.copy2(args.save, package / "input.sav")
    # Only local rendering/autosave preferences; game settings come from the save
    # or the explicitly recorded console setup, never private user config files.
    (package / "openttd.cfg").write_text("[misc]\ngraphicsset = zBase\nlanguage = english.lng\n[gui]\nautosave = off\nautosave_on_exit = false\nautosave_interval = 0\n", encoding="utf-8")
    for name in PROFILE.ASSET_DIRS:
        if (args.assets / name).is_dir():
            shutil.copytree(args.assets / name, package / name)
    for name in ("baseset", "lang"):
        shutil.copytree(args.build / name, package / name, dirs_exist_ok=True)
    dump(package / "package.json", dict(input_sha256=PROFILE.sha256(package / "input.sav"),
        config_sha256=PROFILE.sha256(package / "openttd.cfg"), assets=PROFILE.inventory(package)))


def stdio(args):
    source, build = args.source.resolve(), args.build.resolve()
    original = source / "src/os/windows/win32.cpp"
    content = original.read_text(encoding="utf-8")
    content = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (original.parent / m[1]).resolve().as_posix() + '"'
                     if (original.parent / m[1]).is_file() else m[0], content)
    anchor = 'void CreateConsole()\n{'
    assert content.count(anchor) == 1
    content = content.replace(anchor, anchor + r'''
    if (getenv("OPENTTD_PASSENGER_MEASURE") != nullptr) {
        _has_console = true;
        freopen("stdout.txt", "a", stdout);
        freopen("stderr.txt", "a", stderr);
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        return;
    }
''')
    target = build / "real-map-driver/win32.cpp"
    target.write_text(content, encoding="utf-8")
    project = build / "openttd_lib.vcxproj"
    xml = project.read_text(encoding="utf-8-sig")
    xml = xml.replace(str(target), str(original))
    assert xml.count(str(original)) == 1
    project.write_text(xml.replace(str(original), str(target)), encoding="utf-8-sig")
    metadata_path = build / "real-map-instrumentation.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    metadata["translation_units"] = [{"path": p.relative_to(build).as_posix(), "sha256": PROFILE.sha256(p)}
        for folder in (build / "save-integrity", build / "real-map-driver") for p in sorted(folder.glob("*")) if p.is_file()]
    dump(metadata_path, metadata)


def packet_id_control(args):
    """Measurement-only control: restore the old boarding order, retaining direct iteration."""
    import difflib
    build = args.build.resolve()
    original = build / "save-integrity/cargopacket.cpp"
    before = original.read_text(encoding="utf-8")
    content = before
    changes = {
        "const uint64_t rank = sequence ? *sequence : this->waiting_next_sequence++;":
            "const uint64_t rank = packet->index.base(); // Measurement-only PacketID order control.",
        "board->second.erase({entry.key, entry.sequence, id})":
            "board->second.erase({StationID::Invalid(), entry.sequence, id})",
        "this->boarding_index[service].emplace(key, rank, packet->index)":
            "this->boarding_index[service].emplace(StationID::Invalid(), rank, packet->index)",
        "this->boarding_index[it->second.service].emplace(it->second.key, it->second.sequence, wake->second)":
            "this->boarding_index[it->second.service].emplace(StationID::Invalid(), it->second.sequence, wake->second)",
    }
    for old, new in changes.items():
        if content.count(old) != 1:
            raise ValueError(f"Boarding order control anchor changed: {old}")
        content = content.replace(old, new)
    folder = build / "packet-id-control"
    folder.mkdir(exist_ok=True)
    target = folder / "cargopacket.cpp"
    target.write_text(content, encoding="utf-8")
    (folder / "control.patch").write_text("".join(difflib.unified_diff(
        before.splitlines(keepends=True), content.splitlines(keepends=True),
        fromfile="FIFO/cargopacket.cpp", tofile="PacketID-control/cargopacket.cpp")), encoding="utf-8")
    project = build / "openttd_lib.vcxproj"
    xml = project.read_text(encoding="utf-8-sig").replace(str(target), str(original))
    if xml.count(str(original)) != 1:
        raise ValueError("Expected prepared save-integrity cargo TU")
    project.write_text(xml.replace(str(original), str(target)), encoding="utf-8-sig")
    dump(folder / "provenance.json", dict(scope="Copied TU only; restore pre-change PacketID priority with direct iteration",
        original_sha256=PROFILE.sha256(original), control_sha256=PROFILE.sha256(target)))


def probe(args):
    source, build = args.source.resolve(), args.build.resolve()
    original = source / "src/passenger_od.cpp"
    content = original.read_text(encoding="utf-8")
    content = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (original.parent / m[1]).resolve().as_posix() + '"'
                     if (original.parent / m[1]).is_file() else m[0], content)
    anchor = 'if (graph->PassengerOdRevision() > _passenger_od_revision) SlErrorCorrupt("Invalid passenger OD graph revision");'
    assert content.count(anchor) == 1
    content = content.replace(anchor, 'if (graph->PassengerOdRevision() > _passenger_od_revision) { Debug(sl, 0, "OD revision probe: graph={}, cargo={}, graph_revision={}, saved_counter={}", graph->index, graph->Cargo(), graph->PassengerOdRevision(), _passenger_od_revision); SlErrorCorrupt("Invalid passenger OD graph revision"); }')
    target = build / "real-map-driver/passenger_od.cpp"
    target.write_text(content, encoding="utf-8")
    project = build / "openttd_lib.vcxproj"
    xml = project.read_text(encoding="utf-8-sig")
    assert xml.count(str(original)) == 1
    project.write_text(xml.replace(str(original), str(target)), encoding="utf-8-sig")


def recover_counter(args):
    """One-off copied reader; only its re-saved output may be benchmarked."""
    source, build = args.source.resolve(), args.build.resolve()
    original = source / "src/passenger_od.cpp"
    content = original.read_text(encoding="utf-8")
    content = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (original.parent / m[1]).resolve().as_posix() + '"'
                     if (original.parent / m[1]).is_file() else m[0], content)
    anchor = 'void AfterLoadPassengerOd()\n{'
    assert content.count(anchor) == 1
    content = content.replace(anchor, anchor + r'''
    uint64_t original_counter = _passenger_od_revision;
    uint64_t maximum = original_counter;
    for (const LinkGraph *graph : LinkGraph::Iterate()) maximum = std::max(maximum, graph->PassengerOdRevision());
    for (const LinkGraphJob *job : LinkGraphJob::Iterate()) maximum = std::max(maximum, job->Graph().PassengerOdRevision());
    if (maximum == UINT64_MAX) SlErrorCorrupt("Unrecoverable passenger OD identity counter");
    _passenger_od_revision = maximum;
    Debug(sl, 0, "Measurement-only OD counter recovery: {} -> {}; no graph or Packet revisions modified", original_counter, maximum);
''')
    target = build / "real-map-driver/recovery/passenger_od.cpp"
    target.parent.mkdir(exist_ok=True)
    target.write_text(content, encoding="utf-8")
    project = build / "openttd_lib.vcxproj"
    xml = project.read_text(encoding="utf-8-sig")
    assert xml.count(str(original)) == 1
    project.write_text(xml.replace(str(original), str(target)), encoding="utf-8-sig")


def run(args):
    directory = args.output.resolve()
    if any(c.isspace() for c in str(directory)):
        raise ValueError("Console scripts require an output directory without whitespace")
    directory.mkdir(parents=True, exist_ok=False)
    lock_path = pathlib.Path(tempfile.gettempdir()) / "openttd-passenger-routing-measurement.lock"
    lock = PROFILE.acquire_measurement_lock(lock_path, 60)
    try:
        os.write(lock, json.dumps(dict(pid=os.getpid(), output=str(directory))).encode())
        PROFILE.wait_for_idle(60, 5)
        package = args.package.resolve()
        manifest = json.loads((package / "package.json").read_text(encoding="utf-8"))
        if PROFILE.sha256(args.input or package / "input.sav") != manifest["input_sha256"] and args.input is None:
            raise ValueError("Frozen input changed")
        for name in PROFILE.ASSET_DIRS:
            if name == "lang":
                continue
            if (package / name).is_dir():
                shutil.copytree(package / name, directory / name, copy_function=PROFILE.hardlink_or_copy)
        language = args.lang or args.binary.resolve().parent / "lang"
        shutil.copytree(language, directory / "lang", copy_function=PROFILE.hardlink_or_copy)
        shutil.copy2(package / "openttd.cfg", directory / "openttd.cfg")
        shutil.copy2(args.input or package / "input.sav", directory / "input.sav")
        shutil.copy2(args.binary, directory / "openttd.exe")
        scripts = directory / "scripts"
        scripts.mkdir(exist_ok=True)
        log, result = (directory / "on-1-console.txt").as_posix(), (directory / "result").as_posix()
        setup = "" if args.resume else ("setting linkgraph.distribution_pax 2\nsetting linkgraph.passenger_od_assignment true\n"
            "setting linkgraph.passenger_journey_routing true\nsetting linkgraph.passenger_journey_periodic true\n"
            "setting linkgraph.recalc_interval 8\nsetting linkgraph.recalc_time 32\n")
        if args.precomputed is not None:
            setup += f"setting linkgraph.passenger_journey_precomputed {'true' if args.precomputed else 'false'}\n"
        if args.warm_precomputed or args.rebuild_precomputed:
            setup += "passenger_journey_stats build_precomputed\n"
        if args.rebuild_precomputed:
            # Trigger a deterministic cost generation after completing the old
            # rows. The ordinary product tick builder must keep those rows live.
            setup += "setting linkgraph.passenger_transfer_penalty 17\n"
        start = f"script {log}\ngetfulldate\npassenger_journey_stats inventory\n{setup}passenger_journey_stats reset\n"
        finish = f"script {log}\ngetfulldate\npassenger_journey_stats inventory\npassenger_journey_stats total\npassenger_journey_stats save_integrity\nsave {result}\nscript\n"
        start += "unpause\nscript\n" if args.ticks else "script\n" + finish + "quit\n"
        (scripts / "game_start.scr").write_text(start, encoding="utf-8")
        (scripts / "passenger_benchmark_end.scr").write_text(finish, encoding="utf-8")
        command = [str(directory / "openttd.exe"), "-x", "-X", "-c", "openttd.cfg", "-g", "input.sav", "-v",
                   f"null:ticks={args.ticks}" if args.ticks else "null:until_exit", "-snull", "-mnull", "-d", "sl=2"]
        started, observed, contamination = time.perf_counter(), {}, []
        with (directory / "stdout.txt").open("wb") as out, (directory / "stderr.txt").open("wb") as err:
            process = subprocess.Popen(command, cwd=directory, stdout=out, stderr=err, creationflags=subprocess.CREATE_NO_WINDOW,
                                       env=dict(os.environ, OPENTTD_PASSENGER_MEASURE="1"))
            last_census = time.monotonic()
            while process.poll() is None:
                for key, value in CPU.observe(process).items():
                    observed[key] = max(observed.get(key, 0), value)
                if time.monotonic() - last_census > 10:
                    busy = [p for p in PROFILE.competing_processes() if p != f"openttd:{process.pid}"]
                    if busy:
                        contamination.append(busy)
                    last_census = time.monotonic()
                if time.perf_counter() - started > args.timeout:
                    process.kill()
                    process.wait()
                    raise TimeoutError(f"Own benchmark exceeded {args.timeout}s; output retained at {directory}")
                time.sleep(.1)
            for key, value in CPU.observe(process).items():
                observed[key] = max(observed.get(key, 0), value)
        metadata = dict(exit_code=process.returncode, requested_ticks=args.ticks, elapsed_seconds=time.perf_counter()-started,
            contamination=contamination, input_sha256=PROFILE.sha256(directory / "input.sav"), binary_sha256=PROFILE.sha256(args.binary),
            language_sha256=PROFILE.sha256(directory / "lang/english.lng"),
            resume_settings=args.resume, precomputed=args.precomputed, warm_precomputed=args.warm_precomputed,
            rebuild_precomputed=args.rebuild_precomputed, command=command, **observed)
        for name in ("actual-ticks.json", "save-integrity.json"):
            if (directory / name).is_file():
                metadata[name] = json.loads((directory / name).read_text(encoding="utf-8"))
        if (directory / "result.sav").is_file():
            metadata["result_sha256"] = PROFILE.sha256(directory / "result.sav")
        stderr = (directory / "stderr.txt").read_text(encoding="utf-8", errors="replace")
        metadata["load_failed"] = "Game load failed" in stderr
        metadata["newgrf_warning_lines"] = sum("[grf:0]" in line for line in stderr.splitlines())
        console = (directory / "on-1-console.txt").read_text(encoding="utf-8", errors="replace") if (directory / "on-1-console.txt").is_file() else ""
        metadata["warm_complete"] = "Precomputed warm: ready=true;" in console if args.warm_precomputed or args.rebuild_precomputed else None
        dump(directory / "process.json", metadata)
        print(json.dumps({k:v for k,v in metadata.items() if k != "actual-ticks.json"}, indent=2), flush=True)
        if process.returncode or contamination or metadata["load_failed"] or metadata["warm_complete"] is False or "result_sha256" not in metadata or "save-integrity.json" not in metadata:
            raise RuntimeError(f"Invalid run; see {directory}")
        if args.ticks and (metadata["actual-ticks.json"]["end_tick"] - metadata["actual-ticks.json"]["start_tick"] != args.ticks or len(metadata["actual-ticks.json"]["samples"]) != args.ticks):
            raise RuntimeError("Actual simulation tick count differs from requested")
    finally:
        os.close(lock)
        lock_path.unlink()


def summarize(args):
    rows = []
    for directory in args.runs:
        process = json.loads((directory / "process.json").read_text(encoding="utf-8"))
        samples = process.get("actual-ticks.json", {}).get("samples", [])
        def distribution(values):
            values = sorted(values)
            if not values:
                return None
            return dict(count=len(values), mean_us=sum(values)/len(values),
                p95_us=values[math.ceil(.95*len(values))-1], p99_us=values[math.ceil(.99*len(values))-1], max_us=values[-1])
        row = {k:v for k,v in process.items() if k not in ("actual-ticks.json", "command")}
        row["run"] = directory.name
        row["simulation"] = distribution([s[1] for s in samples])
        row["journey"] = distribution([s[2] for s in samples])
        row["second_half_simulation"] = distribution([s[1] for s in samples[len(samples)//2:]])
        row["second_half_journey"] = distribution([s[2] for s in samples[len(samples)//2:]])
        lines = (directory / "on-1-console.txt").read_text(encoding="utf-8", errors="replace").splitlines()
        observations = {}
        for line in lines:
            if ": " not in line:
                continue
            name, rest = line.split(": ", 1)
            values = {k:float(v) if '.' in v else int(v) for k,v in re.findall(r'(\w+)=(-?\d+(?:\.\d+)?)', rest)}
            if values:
                observations.setdefault(name, []).append(values)
        row["observations"] = observations
        rows.append(row)
    dump(args.output, dict(runs=rows, note="Raw tick samples are elapsed simulation-thread times; OS process CPU includes load/save. Transport counts reflect actual moves; reservations are excluded."))
    print(args.output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("source", type=pathlib.Path)
    p.add_argument("build", type=pathlib.Path)
    p.add_argument("revision")
    p = sub.add_parser("census")
    p.add_argument("source", type=pathlib.Path)
    p.add_argument("build", type=pathlib.Path)
    p = sub.add_parser("driver")
    p.add_argument("source", type=pathlib.Path)
    p.add_argument("build", type=pathlib.Path)
    p.add_argument("revision")
    p = sub.add_parser("stdio")
    p.add_argument("source", type=pathlib.Path)
    p.add_argument("build", type=pathlib.Path)
    p = sub.add_parser("packet_id_control")
    p.add_argument("source", type=pathlib.Path)
    p.add_argument("build", type=pathlib.Path)
    p = sub.add_parser("probe")
    p.add_argument("source", type=pathlib.Path)
    p.add_argument("build", type=pathlib.Path)
    p = sub.add_parser("recover_counter")
    p.add_argument("source", type=pathlib.Path)
    p.add_argument("build", type=pathlib.Path)
    p = sub.add_parser("freeze")
    for name in ("save", "assets", "build", "package"):
        p.add_argument(name, type=pathlib.Path)
    p = sub.add_parser("run")
    for name in ("binary", "package", "output"):
        p.add_argument(name, type=pathlib.Path)
    p.add_argument("--ticks", type=int, default=4096)
    p.add_argument("--timeout", type=int, default=900)
    p.add_argument("--input", type=pathlib.Path)
    p.add_argument("--lang", type=pathlib.Path, help="Matching build language directory (defaults beside the binary)")
    p.add_argument("--resume", action="store_true")
    p.add_argument("--warm-precomputed", action="store_true", help="Complete mandatory rows while paused before the first tick; initial build CPU remains in process totals")
    p.add_argument("--rebuild-precomputed", action="store_true", help="Start with complete rows then change the transfer penalty to 17; measure ordinary replacement ticks")
    p.add_argument("--precomputed", action=argparse.BooleanOptionalAction, default=None)
    p = sub.add_parser("summarize")
    p.add_argument("output", type=pathlib.Path)
    p.add_argument("runs", type=pathlib.Path, nargs='+')
    args = parser.parse_args()
    globals()[args.action](args)


if __name__ == "__main__":
    main()
