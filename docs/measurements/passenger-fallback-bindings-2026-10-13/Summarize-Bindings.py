"""Extract raw paired-run evidence; timing scopes are inclusive and are not summed."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil

p = argparse.ArgumentParser()
p.add_argument("run", type=pathlib.Path)
p.add_argument("output", type=pathlib.Path)
p.add_argument("labels", nargs="+")
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
prefixes = {
    "Simulation tick time:": "simulation", "Tick histogram": "histogram",
    "Journey inclusive scopes:": "inclusive_scopes", "OD fallback cache:": "fallback_cache",
    "OD fallback bindings:": "bindings", "Untimed transport:": "untimed",
    "Journey transport:": "timed_transport", "Periodic database:": "database",
    "Schedule routing:": "schedule", "Active journey total:": "active",
}
summary = {}
for label in a.labels:
    console = (a.run / f"{label}-console.txt").read_text(encoding="utf-8", errors="replace")
    result = {"process": json.loads((a.run / f"{label}-process.json").read_text(encoding="utf-8-sig"))}
    result["state_ticks"] = [int(x) for x in re.findall(r"^State ticks: (\d+)", console, re.M)]
    for line in console.splitlines():
        for prefix, field in prefixes.items():
            if line.startswith(prefix):
                result[field] = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)}
    save = a.run / f"{label}-result.sav"
    result["save_sha256"] = hashlib.sha256(save.read_bytes()).hexdigest()
    ticks = result["simulation"]["completed_ticks"]
    scopes, binding, cache = result["inclusive_scopes"], result["bindings"], result["fallback_cache"]
    result["derived"] = {
        "distance_ms_per_tick": scopes["fallback_distance_us"] / ticks / 1000,
        "binding_validation_ms_per_tick": binding["validation_ns"] / ticks / 1e6,
        "reverse_build_ms_per_tick": binding["reverse_build_ns"] / ticks / 1e6,
        "distance_hit_percent": 100 * cache["hits"] / max(1, cache["hits"] + cache["searches"]),
        "binding_hit_percent": 100 * binding["hits"] / max(1, binding["hits"] + binding["builds"]),
    }
    summary[label] = result
    for suffix in ("console.txt", "process.json", "stdout.txt"):
        shutil.copyfile(a.run / f"{label}-{suffix}", a.output / f"{label}-{suffix}")
    raw = (a.run / f"{label}-stderr.txt").read_bytes()
    result["stderr"] = {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
    unique = list(dict.fromkeys(raw.decode("utf-8", errors="replace").splitlines()))
    (a.output / f"{label}-stderr-excerpt.txt").write_text("\n".join(unique[:60]) + "\n", encoding="utf-8")
(a.output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps({label: result["derived"] for label, result in summary.items()}, indent=2))
