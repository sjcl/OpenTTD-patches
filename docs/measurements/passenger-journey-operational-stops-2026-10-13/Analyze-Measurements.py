"""Re-extract raw console counters and compare two sequential runs per binary."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument("run", type=pathlib.Path)
p.add_argument("output", type=pathlib.Path)
a = p.parse_args()
labels = ["base", "base2", "after", "after2"]
docs = pathlib.Path(__file__).resolve().parents[2]
subprocess.run([sys.executable, str(docs / "measurements/passenger-journey-periodic-2026-10-12/Summarize-Measurements.py"),
                str(a.run), str(a.output), *labels], check=True, stdout=subprocess.DEVNULL)
data = json.loads((a.output / "summary.json").read_text(encoding="utf-8"))
for label, record in data.items():
    console = (a.run / f"{label}-console.txt").read_text(encoding="utf-8")
    events = re.findall(r"^Periodic operating events: (.*)$", console, re.M)
    if events:
        record["operating_events"] = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", events[-1])}
(a.output / "summary.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

def logical(record):
    database = {k: v for k, v in record["database"].items() if k not in ["build_us", "query_us"]}
    return {"state_ticks": record["state_ticks"], "database": database,
            **{k: record[k] for k in ["global_invalidations", "known_vehicle_phases", "fallback", "pragmatic", "timed_transport", "untimed_transport"]}}

comparison = {
    "base_commit": "4bf419c4bf4dd4b0610b4ef403411c33013ace1f",
    "base_instrumentation": "OPTION_PASSENGER_BENCHMARK=ON; fixture-only ClientPoolID comparison corrected to client_id; PE subsystem changed to console",
    "comparison_run_order": ["base", "base2", "after", "after2"],
    "earlier_runs": "Two intermediate after runs preceded base2; final after runs include the order-edit corridor invalidation hook",
    "base_repeated_logical_equal": logical(data["base"]) == logical(data["base2"]),
    "after_repeated_logical_equal": logical(data["after"]) == logical(data["after2"]),
    "histogram_precision": "1 ms bucket upper bounds; two runs per binary, no confidence interval",
    "runtime_source_sha256": {},
}
for metric, section, field in [("simulation", "simulation", "mean_us"), ("journey", "histogram", "journey_mean_us")]:
    base = sum(data[k][section][field] for k in ["base", "base2"]) / 2
    after = sum(data[k][section][field] for k in ["after", "after2"]) / 2
    comparison[metric] = {"base_mean_us": base, "after_mean_us": after, "increase_percent": 100 * (after / base - 1)}
repo = docs.parent
for path in sorted((repo / "src").glob("passenger*")):
    if path.is_file():
        comparison["runtime_source_sha256"][path.relative_to(repo).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
comparison["runtime_source_sha256"]["src/order_cmd.cpp"] = hashlib.sha256((repo / "src/order_cmd.cpp").read_bytes()).hexdigest()
(a.output / "comparison.json").write_text(json.dumps(comparison, indent=2) + "\n", encoding="utf-8")
print(json.dumps({k: v for k, v in comparison.items() if k != "runtime_source_sha256"}, indent=2))
