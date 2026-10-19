#!/usr/bin/env python3
"""Instrument isolated VS translation units and replay fixed real-map ticks.

Product routing, capacity, Work limits and CargoPacket data are unchanged.
The opt-in driver counts StateTicks, records existing diagnostics, and resumes
NewGRF error pauses. Such warnings/resumptions are retained as limitations.
"""
import argparse
import ctypes
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
    pause = '''#ifdef WITH_PASSENGER_BENCHMARK
\t/* Opt-in measurement build only, used with isolated inputs. */
\tif (_pause_mode.Test(PauseMode::Error)) Command<Commands::Pause>::Post(PauseMode::Error, false);
#endif'''
    assert content.count(pause) == 1
    content = content.replace(pause, '''    /* Copied measurement TU: keep the product's error-pause policy intact. */
    if (getenv("OPENTTD_PASSENGER_MEASURE") != nullptr && _pause_mode.Test(PauseMode::Error)) {
        IConsolePrint(CC_WARNING, "Measurement resumed a NewGRF error pause.");
        Command<Commands::Pause>::Post(PauseMode::Error, false);
    }''')
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
    if (operation == "wait_saved_replay") {
        auto &db = GetPassengerServiceDatabase();
        const auto started = std::chrono::steady_clock::now();
        while (db.GetLoadState() == PassengerServiceDatabase::LoadState::Replaying) {
            db.StepLoad(16 * 1024 * 1024);
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(600)) break;
        }
        const bool ready = db.GetLoadState() == PassengerServiceDatabase::LoadState::Ready;
        IConsolePrint(CC_DEFAULT, "Saved passenger replay: ready={}; elapsed_us={}", ready,
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
        return ready;
    }
    if (operation == "build_precomputed") {
        auto &db = GetPassengerServiceDatabase();
        if (db.GetLoadState() != PassengerServiceDatabase::LoadState::Ready) {
            IConsolePrint(CC_ERROR, "Warm build probe requires completed passenger save replay.");
            return false;
        }
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
    headers = ''.join('#include "' + (source / "src" / name).as_posix() + '"\n' for name in ["openttd.h", "date_func.h", "passenger_journey_active.h", "console_func.h", "settings_type.h"])
    content = content.replace(stdafx, stdafx + headers + '#include <charconv>\n#include <chrono>\n#include <cstdio>\n#include <fstream>\n#include <vector>\n')
    anchor = "\t\tfor (int i = 0; i < this->ticks; i++) {\n\t\t\t::GameLoop();\n\t\t\t::InputLoop();\n\t\t\t::UpdateWindows();\n\t\t}"
    assert content.count(anchor) == 1
    driver = r'''
        struct Sample { int64_t tick; uint64_t simulation, journey; };
        std::vector<Sample> samples;
        samples.reserve(this->ticks);
        int64_t start = 0;
        bool started = false;
        uint64_t loops = 0, error_resumes = 0;
        int64_t checkpoint = 0;
        int64_t rebuild_tick = 0;
        if (const char *value = getenv("OPENTTD_PASSENGER_CHECKPOINT_TICK")) std::from_chars(value, value + strlen(value), checkpoint);
        if (const char *value = getenv("OPENTTD_PASSENGER_REBUILD_TICK")) std::from_chars(value, value + strlen(value), rebuild_tick);
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
                    const auto position = line.find(prefix); // Console logs may prefix a wall-clock timestamp.
                    if (position == std::string::npos) continue;
                    auto parsed = std::from_chars(line.data() + position + prefix.size(), line.data() + line.size(), start);
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
                if (checkpoint > 0 && _state_ticks.base() - start == checkpoint) {
                    IConsoleCmdExec("passenger_journey_stats save_integrity");
                    std::rename("save-integrity.json", "transport-checkpoint.json");
                }
                if (rebuild_tick > 0 && _state_ticks.base() - start == rebuild_tick) {
                    /* A declared measurement transition between ticks. Keep its
                     * diagnostic/command time outside both simulation samples. */
                    const auto diagnostic_started = std::chrono::steady_clock::now();
                    IConsoleCmdExec("script midpoint-console.txt");
                    IConsoleCmdExec("passenger_journey_stats total");
                    IConsoleCmdExec("passenger_journey_stats save_integrity");
                    IConsoleCmdExec("script");
                    std::rename("save-integrity.json", "midpoint-save-integrity.json");
                    const auto setting_started = std::chrono::steady_clock::now();
                    IConsoleCmdExec("setting linkgraph.passenger_transfer_penalty 17");
                    const auto finished = std::chrono::steady_clock::now();
                    std::ofstream event("rebuild-transition.json");
                    event << "{\"elapsed_tick\":" << rebuild_tick << ",\"state_tick\":" << _state_ticks.base()
                        << ",\"cost\":" << _settings_game.linkgraph.passenger_transfer_penalty
                        << ",\"diagnostic_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(setting_started - diagnostic_started).count()
                        << ",\"setting_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(finished - setting_started).count() << "}\n";
                }
            }
            if (loops == 1 || loops % 256 == 0) {
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
    finish = '''#ifdef WITH_PASSENGER_BENCHMARK
\tif (!this->until_exit) {
\t\tIConsoleCmdExec("exec scripts/passenger_benchmark_end.scr 0");
\t}
#endif'''
    assert content.count(finish) == 1
    content = content.replace(finish, '''    if (!this->until_exit && getenv("OPENTTD_PASSENGER_MEASURE") != nullptr) {
        IConsoleCmdExec("exec scripts/passenger_benchmark_end.scr 0");
    }''')
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


def run(args, measurement_lock_fd=None):
    wpr_profile = getattr(args, 'wpr_profile', False)
    if wpr_profile and (os.name != 'nt' or not args.ticks):
        raise ValueError('--wpr-profile requires Windows and a positive tick count')
    hyperpath_filter = getattr(args, 'hyperpath_filter', None)
    hyperpath_filter_alternate = getattr(args, 'hyperpath_filter_alternate', False)
    if sum(x for x in (hyperpath_filter_alternate, hyperpath_filter is not None, bool(getattr(args, 'hyperpath_filter_mode', None)))) > 1:
        raise ValueError('Use only one of --hyperpath-filter/--no-hyperpath-filter, --hyperpath-filter-alternate and --hyperpath-filter-mode')
    midpoint = getattr(args, 'mid_rebuild_tick', 0)
    finish_rebuild = getattr(args, 'finish_rebuild_precomputed', False)
    if midpoint and (not 0 < midpoint < args.ticks or args.precomputed is not True or not args.warm_precomputed or args.rebuild_precomputed or getattr(args, 'full_rebuild_precomputed', False)):
        raise ValueError('--mid-rebuild-tick requires a warm table, 0 < midpoint < ticks, and no other replacement option')
    if finish_rebuild and not midpoint:
        raise ValueError('--finish-rebuild-precomputed requires --mid-rebuild-tick')
    if getattr(args, 'full_rebuild_precomputed', False) and (args.precomputed is not True or not args.warm_precomputed or args.rebuild_precomputed):
        raise ValueError('--full-rebuild-precomputed requires --precomputed --warm-precomputed and excludes --rebuild-precomputed')
    affinity_mask = getattr(args, "affinity_mask", None)
    main_affinity = getattr(args, "main_thread_affinity_mask", None)
    observe_host = getattr(args, "observe_host", False)
    wait_background = getattr(args, "wait_background_ready", False)
    if wait_background and not (args.precomputed is True and (args.warm_precomputed or args.rebuild_precomputed)):
        raise ValueError("--wait-background-ready requires precomputed table warm-up")
    if observe_host and (os.name != "nt" or main_affinity is None):
        raise ValueError("--observe-host requires Windows and --main-thread-affinity-mask")
    for mask in (affinity_mask, main_affinity):
        if mask is not None and (os.name != "nt" or mask <= 0):
            raise ValueError("A positive Windows affinity mask is required")
    if args.cold_precomputed and (args.precomputed is not True or not args.warm_precomputed):
        raise ValueError("--cold-precomputed requires --precomputed --warm-precomputed")
    if args.fixture_estimated and (args.precomputed is not True or not args.warm_precomputed or args.fixture_timetable):
        raise ValueError("--fixture-estimated requires --precomputed --warm-precomputed and excludes --fixture-timetable")
    directory = args.output.resolve()
    if any(c.isspace() for c in str(directory)):
        raise ValueError("Console scripts require an output directory without whitespace")
    directory.mkdir(parents=True, exist_ok=False)
    lock_name = f"openttd-passenger-routing-validation-{os.getpid()}.lock" if args.validation_only else "openttd-passenger-routing-measurement.lock"
    lock_path = pathlib.Path(tempfile.gettempdir()) / lock_name
    borrowed_lock = measurement_lock_fd is not None
    if borrowed_lock:
        if args.validation_only or json.loads(lock_path.read_text(encoding="utf-8"))["pid"] != os.getpid():
            raise ValueError("Only the owning process can reuse a measurement reservation")
        lock = measurement_lock_fd
    else:
        lock = PROFILE.acquire_measurement_lock(lock_path, getattr(args, "measurement_wait", 60))
    observer = None
    try:
        if not borrowed_lock:
            os.write(lock, json.dumps(dict(pid=os.getpid(), output=str(directory))).encode())
        if not args.validation_only:
            PROFILE.wait_for_idle(getattr(args, "measurement_wait", 60), 5)
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
        if args.cold_precomputed:
            # Exercise the ordinary disable callback to discard saved rows;
            # report that frontier before enabling and warming the empty table.
            setup += "setting linkgraph.passenger_journey_precomputed false\n"
            if "OPENTTD_PASSENGER_TRACE" in os.environ:
                setup += "passenger_journey_stats trace_prepare\n"
            setup += "passenger_journey_stats total\n"
            if "OPENTTD_PASSENGER_TRACE" in os.environ:
                setup += "passenger_journey_stats trace_state\n"
        if args.precomputed is not None:
            setup += f"setting linkgraph.passenger_journey_precomputed {'true' if args.precomputed else 'false'}\n"
        if args.hyperpath is not None:
            setup += f"setting linkgraph.passenger_journey_hyperpath {'true' if args.hyperpath else 'false'}\n"
        if hyperpath_filter is not None:
            setup += f"passenger_journey_stats hyperpath_filter_{'on' if hyperpath_filter else 'off'}\n"
        if hyperpath_filter_alternate:
            setup += "passenger_journey_stats hyperpath_filter_alternate\n"
        if getattr(args, 'hyperpath_filter_mode', None):
            setup += f"passenger_journey_stats hyperpath_filter_{args.hyperpath_filter_mode}\n"
        if args.fixture_timetable:
            setup += "passenger_journey_stats fixture_timetable\n"
        if args.fixture_estimated:
            setup += "passenger_journey_stats fixture_estimated\n"
        if getattr(args, 'full_rebuild_precomputed', False) or midpoint:
            setup += "setting linkgraph.passenger_transfer_penalty 20\n"
        if args.warm_precomputed or args.rebuild_precomputed:
            setup += "passenger_journey_stats build_precomputed\n"
        if wait_background:
            setup += "passenger_journey_stats wait_background_ready\n"
        if args.rebuild_precomputed:
            # Trigger a deterministic cost generation after completing the old
            # rows. The ordinary product tick builder must keep those rows live.
            setup += "setting linkgraph.passenger_transfer_penalty 17\n"
        if getattr(args, 'full_rebuild_precomputed', False):
            # Separate paused all-row replacement timing from ordinary
            # tick-budgeted replacement. Both retain the prior live rows.
            setup += "setting linkgraph.passenger_transfer_penalty 17\npassenger_journey_stats build_precomputed\n"
        if args.quality_probe:
            setup += "passenger_journey_stats quality_probe\n"
        if args.fixture_estimated:
            setup += "passenger_journey_stats fixture_estimated_od\n"
        replay = "passenger_journey_stats wait_saved_replay\n" if getattr(args, "wait_saved_replay", False) else ""
        start = f"script {log}\ngetfulldate\n{replay}passenger_journey_stats inventory\n{setup}passenger_journey_stats reset\n"
        # The existing component profiler is printed after the timed ticks.
        finish = f"script {log}\ngetfulldate\npassenger_journey_stats inventory\npassenger_journey_stats total\nfps\npassenger_journey_stats save_integrity\nsave {result}\nscript\n"
        if finish_rebuild:
            finish = finish.replace('passenger_journey_stats inventory\n', 'passenger_journey_stats build_precomputed\npassenger_journey_stats inventory\n', 1)
        if args.quality_probe:
            finish = finish.replace("passenger_journey_stats total\n", "passenger_journey_stats total\npassenger_journey_stats quality_probe\n")
        if wpr_profile:
            start += "passenger_journey_stats wpr_begin\n"
        start += "unpause\nscript\n" if args.ticks else ("unpause\n" if args.unpause_before_save else "") + "script\n" + finish + "quit\n"
        (scripts / "game_start.scr").write_text(start, encoding="utf-8")
        (scripts / "passenger_benchmark_end.scr").write_text(finish, encoding="utf-8")
        command = [str(directory / "openttd.exe"), "-x", "-X", "-c", "openttd.cfg", "-g", "input.sav", "-v",
                   f"null:ticks={args.ticks}" if args.ticks else "null:until_exit", "-snull", "-mnull", "-d", "sl=2"]
        started, observed, contamination = time.perf_counter(), {}, []
        run_env = dict(os.environ, OPENTTD_PASSENGER_MEASURE='1')
        run_env.pop('OPENTTD_WPR_PROFILE', None)
        if wpr_profile:
            run_env['OPENTTD_WPR_PROFILE'] = '1'
        run_env.pop('OPENTTD_PASSENGER_REBUILD_TICK', None)
        if midpoint:
            run_env['OPENTTD_PASSENGER_REBUILD_TICK'] = str(midpoint)
        phase_contamination, simulation_contamination = [], []
        main_thread = None
        with (directory / "stdout.txt").open("wb") as out, (directory / "stderr.txt").open("wb") as err:
            process = subprocess.Popen(command, cwd=directory, stdout=out, stderr=err, creationflags=subprocess.CREATE_NO_WINDOW,
                                       env=run_env)
            if affinity_mask is not None:
                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                kernel.SetProcessAffinityMask.argtypes = (ctypes.c_void_p, ctypes.c_size_t)
                kernel.SetProcessAffinityMask.restype = ctypes.c_int
                if not kernel.SetProcessAffinityMask(int(process._handle), affinity_mask):
                    error = ctypes.WinError(ctypes.get_last_error())
                    process.kill()
                    process.wait()
                    raise error
            if main_affinity is not None:
                try:
                    # Null video executes MainLoop/GameLoop on the initial
                    # process thread; preserve the other workers' CPU access.
                    result = PROFILE.powershell(
                        "$ErrorActionPreference = 'Stop'; "
                        f"$benchProcess = Get-Process -Id {process.pid} -ErrorAction Stop; "
                        "$benchThread = $benchProcess.Threads | Sort-Object StartTime | Select-Object -First 1; "
                        "if ($null -eq $benchThread) { throw 'No benchmark thread' }; "
                        f"$benchThread.ProcessorAffinity = [IntPtr]{main_affinity}; "
                        "[pscustomobject]@{thread_id=$benchThread.Id; process_affinity=[int64]$benchProcess.ProcessorAffinity} | ConvertTo-Json -Compress")
                    main_thread = json.loads(result)
                    class GroupAffinity(ctypes.Structure):
                        _fields_ = [("mask", ctypes.c_size_t), ("group", ctypes.c_ushort),
                                    ("reserved", ctypes.c_ushort * 3)]

                    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                    kernel.OpenThread.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
                    kernel.OpenThread.restype = ctypes.c_void_p
                    kernel.GetThreadGroupAffinity.argtypes = (ctypes.c_void_p, ctypes.POINTER(GroupAffinity))
                    kernel.GetThreadGroupAffinity.restype = ctypes.c_int
                    kernel.CloseHandle.argtypes = (ctypes.c_void_p,)
                    thread = kernel.OpenThread(0x0040, False, main_thread["thread_id"])
                    if not thread:
                        raise ctypes.WinError(ctypes.get_last_error())
                    try:
                        actual = GroupAffinity()
                        if not kernel.GetThreadGroupAffinity(thread, ctypes.byref(actual)):
                            raise ctypes.WinError(ctypes.get_last_error())
                        if actual.mask != main_affinity:
                            raise RuntimeError("Main thread CPU affinity was not applied")
                        main_thread.update(verified_mask=actual.mask, group=actual.group)
                    finally:
                        kernel.CloseHandle(thread)
                except Exception:
                    process.kill()
                    process.wait()
                    raise
            if observe_host:
                try:
                    host = module(ROOT / "utils/passenger_host_observer.py", "host_observer")
                    observer = host.HostObserver(int(process._handle), process.pid, main_thread["thread_id"])
                except Exception:
                    process.kill()
                    process.wait()
                    raise
            last_census = time.monotonic()
            next_observation = last_census + 1
            while process.poll() is None:
                for key, value in CPU.observe(process).items():
                    observed[key] = max(observed.get(key, 0), value)
                if observer is not None and time.monotonic() >= next_observation:
                    stage = "preparation"
                    if (directory / "actual-ticks.json").exists() or (wpr_profile and (directory / "wpr-finished.txt").exists()):
                        stage = "after_simulation"
                    elif (directory / "on-1-console.txt").exists() and "Console log file closed." in (directory / "on-1-console.txt").read_text(encoding="utf-8", errors="replace"):
                        stage = "simulation"
                    elif (directory / "progress.json").exists():
                        try:
                            if json.loads((directory / "progress.json").read_text(encoding="utf-8"))["samples"]:
                                stage = "simulation"
                        except (OSError, ValueError, KeyError):
                            stage = "simulation"
                    try:
                        observer.sample(time.perf_counter() - started, stage)
                    except Exception:
                        process.kill()
                        process.wait()
                        raise
                    next_observation = time.monotonic() + 1
                if time.monotonic() - last_census > 10:
                    busy = [p for p in PROFILE.competing_processes() if p != f"openttd:{process.pid}"]
                    if busy:
                        contamination.append(busy)
                        # Only StateGameLoop samples are used for ordinary-tick
                        # timing. Keep preparation/load/save overlap visible;
                        # it invalidates whole-process timing, not a later clean
                        # simulation window. Existing sample files delimit it.
                        stage = "preparation"
                        # WPR waits for ETL serialization before writing the
                        # sample file. Its finished gate already delimits the
                        # completed simulation during that wait.
                        if (directory / "actual-ticks.json").exists() or (wpr_profile and (directory / "wpr-finished.txt").exists()):
                            stage = "after_simulation"
                        elif (directory / "on-1-console.txt").exists() and "Console log file closed." in (directory / "on-1-console.txt").read_text(encoding="utf-8", errors="replace"):
                            # game_start closes the log immediately after its
                            # unpause. Do not miss the first 255 ticks between
                            # the driver's periodic progress-file updates.
                            stage = "simulation"
                        elif (directory / "progress.json").exists():
                            try:
                                if json.loads((directory / "progress.json").read_text(encoding="utf-8"))["samples"]:
                                    stage = "simulation"
                            except (OSError, ValueError, KeyError):
                                stage = "simulation"  # A concurrent rewrite is conservative.
                        phase_contamination.append(dict(elapsed_seconds=time.perf_counter()-started, stage=stage, processes=busy))
                        if stage == "simulation":
                            simulation_contamination.append(busy)
                    last_census = time.monotonic()
                if time.perf_counter() - started > args.timeout:
                    process.kill()
                    process.wait()
                    raise TimeoutError(f"Own benchmark exceeded {args.timeout}s; output retained at {directory}")
                time.sleep(.1)
            for key, value in CPU.observe(process).items():
                observed[key] = max(observed.get(key, 0), value)
        metadata = dict(exit_code=process.returncode, requested_ticks=args.ticks, elapsed_seconds=time.perf_counter()-started,
            trace_enabled="OPENTTD_PASSENGER_TRACE" in os.environ, checkpoint_tick=os.environ.get("OPENTTD_PASSENGER_CHECKPOINT_TICK"),
            focused_od_trace="OPENTTD_PASSENGER_FOCUS_TRACE" in os.environ,
            departure_diagnostic="OPENTTD_PASSENGER_DEPARTURE_DIAGNOSTIC" in os.environ,
            contamination=contamination, input_sha256=PROFILE.sha256(directory / "input.sav"), binary_sha256=PROFILE.sha256(args.binary),
            language_sha256=PROFILE.sha256(directory / "lang/english.lng"),
            resume_settings=args.resume, fixture_timetable=args.fixture_timetable, fixture_estimated=args.fixture_estimated, cold_precomputed=args.cold_precomputed,
            precomputed=args.precomputed, hyperpath=args.hyperpath, hyperpath_filter=hyperpath_filter, hyperpath_filter_alternate=hyperpath_filter_alternate, hyperpath_filter_mode=getattr(args, 'hyperpath_filter_mode', None), warm_precomputed=args.warm_precomputed,
            rebuild_precomputed=args.rebuild_precomputed, full_rebuild_precomputed=getattr(args, 'full_rebuild_precomputed', False),
            mid_rebuild_tick=midpoint, finish_rebuild_precomputed=finish_rebuild, command=command, **observed)
        metadata["unpause_before_save"] = args.unpause_before_save
        metadata['wpr_profile'] = wpr_profile
        metadata["validation_only"] = args.validation_only
        # Copied retirement instrumentation is independent of saved algorithm flags.
        metadata["retirement_profile"] = "OPENTTD_RETIREMENT_PROFILE" in os.environ
        metadata["retirement_reference"] = "OPENTTD_RETIREMENT_REFERENCE" in os.environ
        metadata["affinity_mask"] = affinity_mask
        metadata["main_thread_affinity_mask"] = main_affinity
        metadata["main_thread"] = main_thread
        if observer is not None:
            dump(directory / "host-observation.json", observer.result())
            metadata["host_observation"] = dict(file="host-observation.json", samples=len(observer.samples),
                maximum_observation_cost_us=max((s["observation_cost_us"] for s in observer.samples), default=None),
                read_only=True)
        metadata["phase_contamination"] = phase_contamination
        metadata["whole_process_timing_valid"] = not args.validation_only and not contamination
        for name in ("actual-ticks.json", "save-integrity.json", "rebuild-transition.json", "midpoint-save-integrity.json"):
            if (directory / name).is_file():
                metadata[name] = json.loads((directory / name).read_text(encoding="utf-8"))
        clock = metadata.get("actual-ticks.json", {})
        complete_ticks = bool(args.ticks) and clock.get("end_tick", 0) - clock.get("start_tick", 0) == args.ticks and len(clock.get("samples", [])) == args.ticks
        metadata["simulation_timing_valid"] = complete_ticks and not args.validation_only and not simulation_contamination
        if (directory / "result.sav").is_file():
            metadata["result_sha256"] = PROFILE.sha256(directory / "result.sav")
        stderr = (directory / "stderr.txt").read_text(encoding="utf-8", errors="replace")
        metadata["load_failed"] = "Game load failed" in stderr
        metadata["newgrf_warning_lines"] = sum("[grf:0]" in line for line in stderr.splitlines())
        console = (directory / "on-1-console.txt").read_text(encoding="utf-8", errors="replace") if (directory / "on-1-console.txt").is_file() else ""
        if getattr(args, "wait_saved_replay", False):
            replay = re.search(r"Saved passenger replay: ready=(true|false); elapsed_us=(\d+)", console)
            metadata["saved_replay"] = dict(ready=replay[1] == "true", elapsed_us=int(replay[2])) if replay else None
            if replay is None or replay[1] != "true":
                metadata["simulation_timing_valid"] = False
                dump(directory / "process.json", metadata)
                raise RuntimeError(f"Saved passenger replay did not complete; see {directory}")
        warms = re.findall(r'Precomputed warm: ready=(true|false);', console)
        metadata['warm_observations'] = warms
        required_warms = 2 if getattr(args, 'full_rebuild_precomputed', False) or finish_rebuild else 1
        metadata["warm_complete"] = len(warms) >= required_warms and all(ready == 'true' for ready in warms) if args.warm_precomputed or args.rebuild_precomputed else None
        if wait_background:
            ready = re.search(r"Background jobs ready: completed=(true|false); initial=(\d+); total=(\d+); elapsed_us=(\d+); tick_before=(\d+); tick_after=(\d+)", console)
            metadata["background_ready"] = dict(completed=ready[1] == "true", initial=int(ready[2]), total=int(ready[3]),
                elapsed_us=int(ready[4]), tick_before=int(ready[5]), tick_after=int(ready[6])) if ready else None
            if ready is None or ready[1] != "true" or ready[5] != ready[6]:
                metadata["simulation_timing_valid"] = False
                dump(directory / "process.json", metadata)
                raise RuntimeError(f"Background preparation failed or advanced StateTicks; see {directory}")
        metadata["script_error_pause_resumes"] = console.count("Measurement resumed a NewGRF error pause.")
        if args.cold_precomputed and "OPENTTD_PASSENGER_TRACE" in os.environ:
            metadata["trace_preserved_during_initial_snapshot"] = "Trace recording: final=false" in console
            if not metadata["trace_preserved_during_initial_snapshot"]:
                dump(directory / "process.json", metadata)
                raise RuntimeError("The initial cold snapshot finalized transport observation")
        dump(directory / "process.json", metadata)
        print(json.dumps({k:v for k,v in metadata.items() if k != "actual-ticks.json"}, indent=2), flush=True)
        timing_overlap = simulation_contamination if args.ticks else contamination
        if process.returncode or (timing_overlap and not args.validation_only) or metadata["load_failed"] or metadata["warm_complete"] is False or "result_sha256" not in metadata or "save-integrity.json" not in metadata:
            raise RuntimeError(f"Invalid run; see {directory}")
        if args.ticks and (metadata["actual-ticks.json"]["end_tick"] - metadata["actual-ticks.json"]["start_tick"] != args.ticks or len(metadata["actual-ticks.json"]["samples"]) != args.ticks):
            raise RuntimeError("Actual simulation tick count differs from requested")
        if midpoint:
            event = metadata.get('rebuild-transition.json', {})
            if event.get('elapsed_tick') != midpoint or event.get('state_tick') != metadata['actual-ticks.json']['start_tick'] + midpoint or event.get('cost') != 17:
                raise RuntimeError('The declared cost-replacement transition was not executed at its requested tick')
    finally:
        if observer is not None:
            observer.close()
        if not borrowed_lock:
            os.close(lock)
            lock_path.unlink()


def summarize(args):
    rows = []
    for directory in args.runs:
        process = json.loads((directory / "process.json").read_text(encoding="utf-8"))
        if process.get("validation_only"):
            raise ValueError(f"Correctness-only run cannot be summarized as performance: {directory}")
        if not process.get("simulation_timing_valid", not process.get("contamination")):
            raise ValueError(f"Overlapping simulation workload cannot be summarized as performance: {directory}")
        samples = process.get("actual-ticks.json", {}).get("samples", [])
        clock = process.get("actual-ticks.json", {})
        if not process.get("requested_ticks") or len(samples) != process["requested_ticks"] or clock.get("end_tick", 0) - clock.get("start_tick", 0) != process["requested_ticks"]:
            raise ValueError(f"Incomplete tick window cannot be summarized as performance: {directory}")
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
        if midpoint := process.get('mid_rebuild_tick'):
            row['ordinary_simulation'] = distribution([s[1] for s in samples[:midpoint]])
            row['ordinary_journey'] = distribution([s[2] for s in samples[:midpoint]])
            row['ordinary_tail_simulation'] = distribution([s[1] for s in samples[midpoint//2:midpoint]])
            row['ordinary_tail_journey'] = distribution([s[2] for s in samples[midpoint//2:midpoint]])
            row['replacement_simulation'] = distribution([s[1] for s in samples[midpoint:]])
            row['replacement_journey'] = distribution([s[2] for s in samples[midpoint:]])
        lines = (directory / "on-1-console.txt").read_text(encoding="utf-8", errors="replace").splitlines()
        observations = {}
        for line in lines:
            line = re.sub(r"^\[[^\]]+\]\s*", "", line)
            if ": " not in line:
                continue
            name, rest = line.split(": ", 1)
            if name == 'Passenger scheduler cause':
                cause = re.search(r'\bcause=(\w+)', rest)
                if cause:
                    name += '/' + cause[1]
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
    p.add_argument("--measurement-wait", type=int, default=60, help="Wait for another measurement/host workload without stopping it")
    p.add_argument('--wpr-profile', action='store_true', help='Use the copied WPR gate hooks; requires passenger_wpr_prepare.py and the bounded elevated recorder')
    p.add_argument("--input", type=pathlib.Path)
    p.add_argument("--lang", type=pathlib.Path, help="Matching build language directory (defaults beside the binary)")
    p.add_argument("--resume", action="store_true")
    p.add_argument("--wait-saved-replay", action="store_true", help="Complete validated saved passenger replay before changing measurement settings; requires freshly prepared copied TUs")
    p.add_argument("--quality-probe", action="store_true", help="Read-only OD/service diagnostics in the copied estimated-pattern measurement TUs")
    p.add_argument("--fixture-timetable", action="store_true", help="Synthetic MP fixture only: assign fixed timetable times using the copied join console hook")
    p.add_argument("--fixture-estimated", action="store_true", help="Synthetic MP fixture only: clear timing certification and assign waiting passengers to reachable OD pairs")
    p.add_argument("--cold-precomputed", action="store_true", help="Discard saved rows through the normal disable callback before enabling and warming; requires --precomputed --warm-precomputed")
    p.add_argument("--warm-precomputed", action="store_true", help="Complete mandatory rows while paused before the first tick; initial build CPU remains in process totals")
    p.add_argument("--rebuild-precomputed", action="store_true", help="Start with complete rows then change the transfer penalty to 17; measure ordinary replacement ticks")
    p.add_argument("--full-rebuild-precomputed", action="store_true", help="Time a complete paused cost replacement after warming the initial table; requires --warm-precomputed")
    p.add_argument('--mid-rebuild-tick', type=int, default=0, help='Record ordinary ticks, then change transfer cost from 20 to 17 at this exact tick')
    p.add_argument('--finish-rebuild-precomputed', action='store_true', help='Complete the mid-run replacement while paused after all measured ticks')
    p.add_argument("--unpause-before-save", action="store_true", help="For a zero-tick MP fixture, use the ordinary unpause command before saving without advancing simulation")
    p.add_argument("--validation-only", action="store_true", help="Allow other processes for a correctness-only load; record them and prohibit performance summarization")
    p.add_argument("--affinity-mask", type=lambda value: int(value, 0), help="Optional Windows affinity for this benchmark child only; recorded in the output")
    p.add_argument("--main-thread-affinity-mask", type=lambda value: int(value, 0), help="Fix only the null driver's main simulation thread; preserve worker parallelism")
    p.add_argument("--observe-host", action="store_true", help="Read Windows CPU counters, main-thread CPU time and power-policy state; requires main-thread affinity")
    p.add_argument("--wait-background-ready", action="store_true", help="After warming rows, wait for loaded linkgraph workers without publishing results or advancing StateTicks")
    p.add_argument("--precomputed", action=argparse.BooleanOptionalAction, default=None)
    p.add_argument("--hyperpath", action=argparse.BooleanOptionalAction, default=None)
    p.add_argument("--hyperpath-filter", action=argparse.BooleanOptionalAction, default=None,
                   help="Set the local boarding destination filter before resetting diagnostics; routing settings are separate")
    p.add_argument("--hyperpath-filter-alternate", action="store_true",
                   help="Alternate the occurrence index and the destination mask per loading context in one process; compare indexed_* with filtered_*")
    p.add_argument("--hyperpath-filter-mode", choices=["off", "on", "index", "alternate", "validate"],
                   help="Set any boarding candidate mode; validate decides as off and counts index superset violations")
    p = sub.add_parser("summarize")
    p.add_argument("output", type=pathlib.Path)
    p.add_argument("runs", type=pathlib.Path, nargs='+')
    args = parser.parse_args()
    globals()[args.action](args)


if __name__ == "__main__":
    main()
