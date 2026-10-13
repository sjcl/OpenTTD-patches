"""Compare identical-input runs, retaining raw evidence and actual counters."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil

p = argparse.ArgumentParser()
p.add_argument("run", type=pathlib.Path)
p.add_argument("output", type=pathlib.Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
prefixes = {
    "Simulation tick time:": "simulation", "Tick histogram": "histogram",
    "Periodic database:": "database", "Periodic departures:": "departures",
    "Periodic cache cost:": "policies", "Periodic residency:": "publication",
    "Schedule routing:": "schedule", "Schedule transport:": "schedule_transport",
    "Journey transport:": "timed_transport", "Untimed transport:": "untimed_transport",
    "Pragmatic routing:": "routing", "Active journey total:": "active",
}
result = {}
for label in ["scanned", "indexed"]:
    console = (a.run / f"{label}-console.txt").read_text(encoding="utf-8", errors="replace")
    data = dict(process=json.loads((a.run / f"{label}-process.json").read_text(encoding="utf-8-sig")))
    data["state_ticks"] = [int(t) for t in re.findall(r"^State ticks: (-?\d+)", console, re.M)]
    for line in console.splitlines():
        for prefix, name in prefixes.items():
            if line.startswith(prefix):
                data[name] = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
                if name == "active":
                    m = re.search(r"applied=(\d+) packets/(\d+) passengers", line)
                    if m:
                        data[name].update(applied_packets=int(m[1]), applied_passengers=int(m[2]))
        if line.startswith("Periodic OD calls:") or line.startswith("Fallback "):
            data.setdefault("outcomes", []).append(line)
    data["final_save_sha256"] = hashlib.sha256((a.run / f"{label}-result.sav").read_bytes()).hexdigest()
    ticks = data["simulation"]["completed_ticks"]
    data["derived"] = {
        "forward_ms_per_tick": data["departures"]["forward_us"] / ticks / 1000,
        "timed_query_ms_per_tick": data["database"]["query_us"] / ticks / 1000,
        "journey_ms_per_tick": data["histogram"]["journey_mean_us"] / 1000,
        "simulation_ms_per_tick": data["simulation"]["total_us"] / ticks / 1000,
        "index_build_ms_total": data["departures"]["build_ns"] / 1e6,
    }
    result[label] = data
    for suffix in ["console.txt", "process.json"]:
        shutil.copyfile(a.run / f"{label}-{suffix}", a.output / f"{label}-{suffix}")
    stderr = (a.run / f"{label}-stderr.txt").read_bytes()
    data["stderr"] = dict(sha256=hashlib.sha256(stderr).hexdigest(), bytes=len(stderr))
    unique = list(dict.fromkeys(stderr.decode("utf-8", errors="replace").splitlines()))
    (a.output / f"{label}-stderr-excerpt.txt").write_text("\n".join(unique[:60]) + "\n", encoding="utf-8")
old, new = result["scanned"], result["indexed"]
result["equivalence"] = {
    "input": old["process"]["input_sha256"] == new["process"]["input_sha256"],
    "binary": old["process"]["binary_sha256"] == new["process"]["binary_sha256"],
    "ticks": old["state_ticks"] == new["state_ticks"],
    "final_save": old["final_save_sha256"] == new["final_save_sha256"],
    "work": old["departures"]["search_work"] == new["departures"]["search_work"],
    "logical_vehicles": old["departures"]["logical_vehicles"] == new["departures"]["logical_vehicles"],
    "logical_departures": old["departures"]["logical_departures"] == new["departures"]["logical_departures"],
    "outcomes_and_deferrals": old["outcomes"] == new["outcomes"],
    "timed_transport": old["timed_transport"] == new["timed_transport"],
    "untimed_transport": old["untimed_transport"] == new["untimed_transport"],
}
result["improvements_percent"] = {
    k: 100 * (1 - new["derived"][k] / old["derived"][k])
    for k in ["forward_ms_per_tick", "timed_query_ms_per_tick", "journey_ms_per_tick", "simulation_ms_per_tick"]
}
(a.output / "summary.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
print(json.dumps({k: v for k, v in result.items() if k in ["equivalence", "improvements_percent"]}, indent=2))
if not all(result["equivalence"].values()):
    raise SystemExit("Run equivalence failed; inspect raw evidence")
