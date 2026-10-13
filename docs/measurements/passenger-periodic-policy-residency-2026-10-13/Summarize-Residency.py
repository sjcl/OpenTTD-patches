"""Retain raw evidence and extract counters without filling missing observations."""
import argparse
import collections
import hashlib
import json
import pathlib
import re
import shutil
p = argparse.ArgumentParser()
p.add_argument("build", type=pathlib.Path)
p.add_argument("output", type=pathlib.Path)
a = p.parse_args()
a.output.mkdir(exist_ok=True, parents=True)
prefixes = {
    "Simulation tick time:": "simulation", "Tick histogram": "histogram",
    "Journey inclusive scopes:": "inclusive_scopes", "Periodic database:": "database",
    "Schedule routing:": "schedule", "Periodic residency:": "residency",
    "Periodic cache cost:": "cache_cost", "Periodic publication cost:": "publication_cost",
    "Schedule policy compilation:": "schedule_compilation", "Periodic pending age:": "pending_age",
    "OD fallback cache:": "fallback_cache", "Journey transport:": "timed_transport",
    "Schedule transport:": "schedule_transport", "Active journey total:": "active",
    "Graph coverage:": "graph", "Periodic passenger eligibility:": "eligibility",
}
summary = {}
for label, folder in [("baseline", "run-baseline"), ("resident", "run-resident"), ("cold", "run-cold"), ("compact", "run-resident")]:
    run = a.build / folder
    console = (run / f"{label}-console.txt").read_text(encoding="utf-8", errors="replace")
    data = {"process": json.loads((run / f"{label}-process.json").read_text(encoding="utf-8-sig"))}
    data["state_ticks"] = [int(x) for x in re.findall(r"^State ticks: (\d+)", console, re.M)]
    for line in console.splitlines():
        numbers = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
        for prefix, key in prefixes.items():
            if line.startswith(prefix): data[key] = numbers
        if line.startswith("Active journey total:"):
            m = re.search(r"applied=(\d+) packets/(\d+) passengers", line)
            if m: data["active"].update(applied_packets=int(m[1]), applied_passengers=int(m[2]))
        if line.startswith("Periodic global invalidations:"):
            data["global_invalidations"] = int(line.split(":", 1)[1])
    data["final_save_sha256"] = hashlib.sha256((run / f"{label}-result.sav").read_bytes()).hexdigest()
    ticks = data["simulation"]["completed_ticks"]
    db, schedule = data["database"], data["schedule"]
    data["derived"] = {
        "timed_hit_percent": 100 * db["policy_hits"] / (db["policy_hits"] + db["policy_misses"]),
        "schedule_hit_percent": 100 * schedule["hits"] / (schedule["hits"] + schedule["misses"]),
        "timed_query_ms_per_tick": db["query_us"] / ticks / 1000,
        "schedule_query_ms_per_tick": schedule["query_us"] / ticks / 1000,
        "database_build_ms_per_tick": db["build_us"] / ticks / 1000,
    }
    if "cache_cost" in data:
        c = data["cache_cost"]
        data["derived"].update(
            timed_compile_ms_per_tick=c["compile_us"] / ticks / 1000,
            timed_forward_and_other_ms_per_tick=(db["query_us"] - c["compile_us"]) / ticks / 1000,
            shared_eviction_ms_per_tick=c["eviction_us"] / ticks / 1000,
            shared_validation_ms_per_tick=c["validation_us"] / ticks / 1000,
            remaining_journey_ms_per_tick=data["histogram"]["journey_mean_us"] / 1000 - (db["query_us"] + schedule["query_us"] + db["build_us"]) / ticks / 1000,
        )
    summary[label] = data
    for suffix in ["console.txt", "process.json", "stdout.txt"]:
        shutil.copyfile(run / f"{label}-{suffix}", a.output / f"{label}-{suffix}")
    stderr = (run / f"{label}-stderr.txt").read_bytes()
    data["stderr"] = {"sha256": hashlib.sha256(stderr).hexdigest(), "bytes": len(stderr)}
    lines = list(dict.fromkeys(stderr.decode("utf-8", errors="replace").splitlines()))
    (a.output / f"{label}-stderr-excerpt.txt").write_text("\n".join(lines[:60]) + "\n", encoding="utf-8")
trace = a.build / "run-baseline/baseline-trace-stderr.txt"
clears = collections.defaultdict(lambda: dict(calls=0, nonempty=0, lost_timed=0, lost_schedule=0, maximum_lost=0))
for m in re.finditer(r"OD baseline clear: reason=(\w+); timed=(\d+); schedule=(\d+)", trace.read_text(encoding="utf-8", errors="replace")):
    c = clears[m[1]]; timed, schedule = int(m[2]), int(m[3])
    c["calls"] += 1; c["nonempty"] += bool(timed + schedule); c["lost_timed"] += timed; c["lost_schedule"] += schedule
    c["maximum_lost"] = max(c["maximum_lost"], timed + schedule)
summary["baseline_clear_trace"] = {"driver_ticks": 1530, "includes_load": True, "excluded_from_speed_comparison": True, "reasons": dict(clears), "stderr_sha256": hashlib.sha256(trace.read_bytes()).hexdigest()}
for name in ["real", "controlled"]:
    run = a.build / f"mp-{name}"
    if not (run / "result.json").exists(): continue
    summary[f"mp_{name}"] = json.loads((run / "result.json").read_text())
    shutil.copyfile(run / "result.json", a.output / f"mp-{name}-result.json")
    shutil.copyfile(run / "server/console.txt", a.output / f"mp-{name}-server-console.txt")
    logs = {}
    for peer in ["server", "client-initial", "client-late"]:
        raw = (run / peer / "stderr.txt").read_bytes()
        logs[peer] = {"sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw), "missed_sync_frames": raw.count(b"Missed frame for sync")}
        text = raw.decode("utf-8", errors="replace")
        bad = [line for line in text.splitlines() if re.search(r"Sync error|sync_err:|desync detected|Assertion failed", line, re.I)]
        (a.output / f"mp-{name}-{peer}-sync-excerpt.txt").write_text("\n".join(bad) + "\n", encoding="utf-8")
    summary[f"mp_{name}"]["logs"] = logs
for source, dest in [("passenger-tests.log", "passenger-final.log"), ("ctest-product.log", "ctest-product.log")]:
    if (a.build / source).exists(): shutil.copyfile(a.build / source, a.output / dest)
(a.output / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
print(json.dumps({label: data.get("derived", {}) for label, data in summary.items()}, indent=2))
