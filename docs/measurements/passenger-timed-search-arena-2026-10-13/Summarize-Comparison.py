"""Require identical saved state, search admission and physical transport."""
import argparse
import importlib.util
import json
import pathlib
import shutil
import statistics

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("run", type=pathlib.Path)
p.add_argument("output", type=pathlib.Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = pathlib.Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("profile", root / "docs/measurements/passenger-routing-profile-2026-10-13/Summarize-Profile.py")
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)
manifest = json.loads((a.run / "manifest.json").read_text(encoding="utf-8"))
runs = {}
for item in manifest["runs"]:
    label = item["label"]
    directory = a.run / label
    parsed = profile.parse_console((directory / f"{label}-console.txt").read_text(encoding="utf-8-sig", errors="replace"))
    process = json.loads((directory / f"{label}-process.json").read_text(encoding="utf-8-sig"))
    r = parsed["records"]
    ticks = r["Simulation tick time"]["completed_ticks"]
    if ticks != item["ticks"] or len(parsed["state_ticks"]) != 2 or parsed["state_ticks"][1] - parsed["state_ticks"][0] != ticks or process["exit_code"] != 0 or item["returncode"] or item["contamination"]:
        raise ValueError(f"Invalid run: {label}")
    parsed.update(process=process, save_sha256=item["save_sha256"], derived=profile.derived(parsed), semantic_signature=profile.semantic_signature(parsed))
    parsed["derived"]["forward_ms_per_tick"] = r["Periodic departures"]["forward_us"] / ticks / 1000
    runs[label] = parsed
    for suffix in ("console.txt", "process.json"):
        shutil.copy2(directory / f"{label}-{suffix}", a.output / f"{label}-{suffix}")
    stderr = (directory / f"{label}-stderr.txt").read_text(encoding="utf-8-sig", errors="replace")
    (a.output / f"{label}-stderr-excerpt.txt").write_text("\n".join(dict.fromkeys(stderr.splitlines())) + "\n", encoding="utf-8")

def same(left, right):
    ra, rb = left["records"], right["records"]
    return dict(save=left["save_sha256"] == right["save_sha256"], semantics=left["semantic_signature"] == right["semantic_signature"],
                work=ra["Periodic departures"]["search_work"] == rb["Periodic departures"]["search_work"],
                pruning=ra["Periodic service pruning"] == rb["Periodic service pruning"],
                storage={k: ra["Timed search storage"][k] for k in ("labels", "frontier_lookups", "queue_pushes", "queue_pops")} ==
                        {k: rb["Timed search storage"][k] for k in ("labels", "frontier_lookups", "queue_pushes", "queue_pops")})

equivalence = {label: same(runs["legacy-1"], runs[label]) for label in ("arena-1", "arena-2", "legacy-2", "disabled")}
equivalence["detail"] = same(runs["legacy-detail"], runs["arena-detail"])
speed = {}
for field in ("forward_ms_per_tick", "journey_mean_ms", "simulation_mean_ms"):
    old = [runs[f"legacy-{i}"]["derived"][field] for i in (1, 2)]
    new = [runs[f"arena-{i}"]["derived"][field] for i in (1, 2)]
    speed[field] = dict(legacy=old, arena=new, legacy_mean=statistics.mean(old), arena_mean=statistics.mean(new),
                        improvement_percent=100 * (1 - statistics.mean(new) / statistics.mean(old)))
details = {mode: runs[mode + "-detail"]["records"]["Timed queue profile"] for mode in ("legacy", "arena")}
result = dict(manifest=manifest, runs=runs, speed=speed, equivalence=equivalence, queue_detail=details)
(a.output / "summary.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps(dict(speed=speed, equivalence=equivalence, queue_detail=details), indent=2))
if not all(all(check.values()) for check in equivalence.values()):
    raise SystemExit("Storage variants changed simulation behavior")
