"""Prepare the archived loopback test with frozen assets and all routing caches off on clients."""
import argparse
import importlib.util
import pathlib
import shutil
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("build", type=pathlib.Path)
p.add_argument("--directory", default="mp-pruning")
p.add_argument("--probe", action="store_true")
p.add_argument("--unrestricted-clients", action="store_true")
a = p.parse_args()
source = pathlib.Path(__file__).resolve().parents[3]
build = a.build.resolve()
spec = importlib.util.spec_from_file_location("runner", source / "docs/measurements/passenger-routing-profile-2026-10-13/Run-Profile.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
root = build / a.directory
for name in ("server", "client-initial", "client-late"):
    directory = root / name
    directory.mkdir(parents=True, exist_ok=False)
    for asset in runner.ASSET_DIRS:
        if (build / "package" / asset).exists():
            shutil.copytree(build / "package" / asset, directory / asset, copy_function=runner.hardlink_or_copy)
text = (source / "docs/measurements/passenger-departure-index-2026-10-13/Exercise-Multiplayer.py").read_text(encoding="utf-8")
old = 'passenger_journey_stats cache_off\\npassenger_journey_stats departures_off'
if text.count(old) != 2:
    raise ValueError("Client preference anchor changed")
text = text.replace(old, 'passenger_journey_stats cache_off\\npassenger_journey_stats fallback_cache_off\\npassenger_journey_stats departures_off')
text = text.replace('"server_port = 17523\\npause_on_join = true\\nmin_active_clients = 0\\n"',
                    '"server_port = 17523\\npause_on_join = true\\nmin_active_clients = 0\\nmax_lag_time = 32000\\nmax_join_time = 32000\\n"')
text = text.replace('f"script {log}\\npassenger_journey_stats cache_off\\n', 'f"script {log}\\ngetfulldate\\npassenger_journey_stats cache_off\\n')
text = text.replace('passenger_journey_stats departures_off\\nscript\\n", encoding="utf-8")',
                    'passenger_journey_stats departures_off\\nschedule on-next-calendar-month {finish}\\nscript\\n", encoding="utf-8")')
text = text.replace('"passenger_journey_stats total\\nscript\\n", encoding="utf-8")',
                    'f"passenger_journey_stats total\\nschedule on-next-calendar-month {finish}\\nscript\\n", encoding="utf-8")')
anchor = '    (root / "result.json").write_text'
if text.count(anchor) != 1:
    raise ValueError("Result anchor changed")
text = text.replace(anchor, '    result["client_fallback_cache_disabled"] = all("OD fallback residency: disabled" in text(directory / "console.txt") for directory in [first_dir, second_dir])\n' + anchor)
text = text.replace(anchor, '''    # Joins alone are insufficient: peers must still be connected and have
    # progressed under their disabled caches close to the paused server.
    server_tick = int(result["server_state_ticks"][-1])
    def client_ticks():
        return [[int(x) for x in re.findall(r"State ticks: (-?\\d+)", text(directory / "console.txt"))] for directory in [first_dir, second_dir]]
    # on_client runs before map load and its menu ticks are not game progress.
    baseline_tick = int(result["server_state_ticks"][0])
    wait_for(lambda: all(any(tick > baseline_tick and tick >= server_tick - 31 * 74 for tick in ticks) for ticks in client_ticks()), "client monthly game progress", seconds=120)
    result["client_state_ticks"] = client_ticks()
    result["connected_clients_at_end"] = len(re.findall(r"^Client #", text(server_log), re.M)) - 1
    result["max_lag_time"] = 32000
    result["max_join_time"] = 32000
    result["progress_observation_resolution_ticks"] = 31 * 74
    logs = [text(directory / "stderr.txt") for directory in [server_dir, first_dir, second_dir]]
    bad = any(re.search(r"Sync error|sync_err:|Assertion failed|desync detected|has been disconnected", log, re.I) for log in logs)
    result["detected_sync_failure"] = bad
    result["client_exit_codes"] = [first.poll(), second.poll()]
''' + anchor)
text = text.replace('if bad or result["joined_clients"]', 'if not result["client_fallback_cache_disabled"] or bad or result["joined_clients"]')
text = text.replace('if not result["client_fallback_cache_disabled"] or bad', 'if result["connected_clients_at_end"] != 2 or not result["client_fallback_cache_disabled"] or bad')
if a.probe:
    text = text.replace('"null:until_exit", "-n"', '"null:until_exit,pruning_probe", "-n"')
    text = text.replace('text(directory / "console.txt"))] for directory in [first_dir, second_dir]]', 'text(directory / "progress-console.txt"))] for directory in [first_dir, second_dir]]')
    text = text.replace('any(tick > baseline_tick and tick >= server_tick - 31 * 74 for tick in ticks)', 'len(ticks) >= 2 and ticks[-1] >= server_tick - 128 and ticks[-1] > baseline_tick')
    text = text.replace('result["progress_observation_resolution_ticks"] = 31 * 74', 'result["progress_observation_resolution_ticks"] = 128')
    # Assign different logical cores so two deliberately slow clients do not
    # compete for the same core while catching up to the paused server.
    text = text.replace('mask = process_mask.value & -process_mask.value', 'available = process_mask.value\n        if name == "client-late": available &= ~affinities.get("client-initial", {}).get("mask", 0)\n        mask = available & -available')
if a.unrestricted_clients:
    text = text.replace('if handle and kernel.GetProcessAffinityMask', 'if False and handle and kernel.GetProcessAffinityMask')
    text = text.replace('result["progress_observation_resolution_ticks"] = 128', 'result["progress_observation_resolution_ticks"] = 128\n    result["unrestricted_clients"] = True')
(build / "Exercise-Pruning-Multiplayer.py").write_text(text, encoding="utf-8")
print(root)
