"""Extract counters without inventing missing fields, and retain raw evidence."""
import argparse
import json
import pathlib
import re
import shutil
import hashlib

p = argparse.ArgumentParser()
p.add_argument("run", type=pathlib.Path)
p.add_argument("output", type=pathlib.Path)
p.add_argument("labels", nargs="+")
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
prefixes = {
    "Simulation tick time:": "simulation",
    "Tick histogram": "histogram",
    "Journey inclusive scopes:": "inclusive_scopes",
    "Periodic database:": "database",
    "Periodic passenger census": "passenger_census",
    "Periodic passenger eligibility:": "eligibility",
    "Periodic pending age:": "pending_age",
    "OD fallback cache:": "fallback_cache",
    "Trip prediction:": "prediction",
    "Graph work=": "graph",
    "Graph phase_us:": "graph_phases",
    "OD fallback:": "fallback",
    "Pragmatic routing:": "pragmatic",
}
results = {}
for label in a.labels:
    console = (a.run / f"{label}-console.txt").read_text(encoding="utf-8", errors="replace")
    data = {"process": json.loads((a.run / f"{label}-process.json").read_text(encoding="utf-8-sig"))}
    data["state_ticks"] = [int(x) for x in re.findall(r"^State ticks: (\d+)", console, re.M)]
    for line in console.splitlines():
        numbers = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
        for prefix, key in prefixes.items():
            if line.startswith(prefix):
                data[key] = numbers
        if line.startswith("Periodic OD calls:"):
            match = re.match(r"Periodic OD calls: (\w+) outcomes=(\d+); overlapping_observations=(\d+)", line)
            if match:
                data.setdefault("od_calls", {})[match[1]] = {"outcomes": int(match[2]), "overlapping_observations": int(match[3])}
        if line.startswith("Periodic live compilation:") or line.startswith("Periodic passenger vehicle phase census:"):
            match = re.search(r": (\w+)=(\d+)", line)
            if match:
                key = "live_compilation" if "live compilation" in line else "phase_census"
                data.setdefault(key, {})[match[1]] = int(match[2])
        if line.startswith("Periodic execution observations:"):
            match = re.search(r"expired=(\d+) packets/(\d+) passengers; failed_same_service_boarding_checks=(\d+)", line)
            if match:
                data["execution_observations"] = {"expired_packets": int(match[1]), "expired_passengers": int(match[2]), "boarding_checks": int(match[3])}
        if line.startswith("Waiting routing:"):
            data.setdefault("waiting", []).append(numbers)
        for prefix, key in [("Journey transport:", "timed_transport"), ("Untimed transport:", "untimed_transport")]:
            if line.startswith(prefix):
                data[key] = numbers
        if line.startswith("Active journey total:"):
            data["active"] = numbers
            applied = re.search(r"applied=(\d+) packets/(\d+) passengers", line)
            if applied:
                data["active"]["applied_packets"] = int(applied[1])
                data["active"]["applied_passengers"] = int(applied[2])
        if line.startswith("Fallback "):
            fallback = re.match(r"Fallback (\w+): (\d+) packets; deferred=(\d+) packets", line)
            if fallback:
                data.setdefault("fallback_reasons", {})[fallback[1]] = {"fallback": int(fallback[2]), "deferred": int(fallback[3])}
        for prefix, key in [("Periodic global invalidations:", "global_invalidations"),
                            ("Periodic known vehicle phases:", "known_vehicle_phases")]:
            if line.startswith(prefix):
                data[key] = int(line.split(":", 1)[1])
    results[label] = data
    for suffix in ["console.txt", "process.json", "stdout.txt"]:
        shutil.copyfile(a.run / f"{label}-{suffix}", a.output / f"{label}-{suffix}")
    raw_errors = (a.run / f"{label}-stderr.txt").read_bytes()
    lines = raw_errors.decode("utf-8", errors="replace").splitlines()
    data["stderr"] = {"sha256": hashlib.sha256(raw_errors).hexdigest(), "bytes": len(raw_errors), "lines": len(lines)}
    # Repeated pre-existing NewGRF warnings are large; preserve a bounded excerpt,
    # full-file hash and counts, while keeping the complete stream in the run dir.
    distinct = list(dict.fromkeys(line for line in lines if line))
    (a.output / f"{label}-stderr-excerpt.txt").write_text("\n".join(distinct[:80]) + "\n", encoding="utf-8")
(a.output / "summary.json").write_text(json.dumps(results, indent=2, ensure_ascii=False), encoding="utf-8")
print(json.dumps(results, indent=2, ensure_ascii=False))
