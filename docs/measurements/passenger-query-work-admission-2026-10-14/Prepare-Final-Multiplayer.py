"""Prepare unrestricted cold clients and compare full Cargo state at the paused tick."""
import argparse
import importlib.util
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=pathlib.Path)
p.add_argument('package', type=pathlib.Path)
p.add_argument('--directory', default='mp-final-review')
a = p.parse_args()
build = a.build.resolve()
spec = importlib.util.spec_from_file_location('runner', HERE.parent / 'passenger-routing-profile-2026-10-13/Run-Profile.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
package = build / 'package'
package.mkdir(exist_ok=True)
for name in runner.ASSET_DIRS:
    if not (package / name).exists() and (a.package / name).exists():
        shutil.copytree(a.package / name, package / name, copy_function=runner.hardlink_or_copy)
subprocess.run([sys.executable, HERE.parent / 'passenger-timed-search-arena-2026-10-13/Prepare-Multiplayer.py', build,
    '--directory', a.directory], check=True)
target = build / 'Exercise-Pruning-Multiplayer.py'
text = target.read_text(encoding='utf-8')
text = text.replace('    time.sleep(a.duration)', '''    observation_end = time.monotonic() + a.duration
    wait_for(lambda: time.monotonic() >= observation_end, "post-join observation", seconds=a.duration + 60)''')
text = text.replace('        if any(proc.poll() is not None for proc in processes):', '''        if any("Final close probe:" in text(d / "stderr.txt") or "Closed client connection" in text(d / "stderr.txt")
               for d in [root / "client-initial", root / "client-late"]):
            raise RuntimeError(f"A client disconnected while waiting for {label}")
        if any(proc.poll() is not None for proc in processes):''')
text = text.replace('"client monthly game progress", seconds=120)', '"client monthly game progress", seconds=900)')
text = text.replace('    command(server, "passenger_journey_stats total")',
                    '    command(server, "passenger_journey_stats total")\n    command(server, "passenger_journey_stats save_integrity")')
anchor = '    (root / "result.json").write_text'
assert text.count(anchor) == 1
text = text.replace(anchor, '''    server_integrity = json.loads((server_dir / "save-integrity.json").read_text(encoding="utf-8"))
    def synchronized_cargo():
        try:
            peers = [json.loads((d / "save-integrity.json").read_text(encoding="utf-8")) for d in [first_dir, second_dir]]
            return all(p["state_ticks"] == server_integrity["state_ticks"] for p in peers)
        except (OSError, ValueError, KeyError):
            return False
    wait_for(synchronized_cargo, "paused tick Cargo snapshots", seconds=900)
    peer_integrity = [json.loads((d / "save-integrity.json").read_text(encoding="utf-8")) for d in [first_dir, second_dir]]
    cargo_keys = ["state_ticks", "packets", "cargo_units", "known_od_units", "journey_packets", "packet_state_digest", "payment_digest", "payments_valid"]
    result["cargo_state_equal"] = all(all(p[k] == server_integrity[k] for k in cargo_keys) for p in peer_integrity)
    result["cargo_snapshots"] = [server_integrity, *peer_integrity]
    if not result["cargo_state_equal"]:
        raise RuntimeError("Cargo state differs at the same paused tick")
''' + anchor)
target.write_text(text, encoding='utf-8')
probe = build / 'network-probe/null_v.cpp'
code = probe.read_text(encoding='utf-8')
marker = 'static int64_t next_pruning_probe = 0;'
assert code.count(marker) == 1
if 'static int64_t final_pause_snapshot' not in code:
    code = code.replace(marker, marker + '\nstatic int64_t final_pause_snapshot = INT64_MIN;')
anchor = '\t\t\t::UpdateWindows();'
assert code.count(anchor) == 2
if 'final_pause_snapshot != _state_ticks.base()' not in code:
    code = code.replace(anchor, anchor + '''
            if (pruning_probe && _networking && !_network_server && _game_mode == GameMode::Normal &&
                _pause_mode.Any() && final_pause_snapshot != _state_ticks.base()) {
                final_pause_snapshot = _state_ticks.base();
                IConsoleCmdExec("passenger_journey_stats save_integrity");
            }''', 1)
probe.write_text(code, encoding='utf-8')
